/*-------------------------------------------------------------------------
 *
 * nodeMaterial.c
 *	  Routines to handle materialization nodes.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeMaterial.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecMaterial			- materialize the result of a subplan
 *		ExecInitMaterial		- initialize node and subnodes
 *		ExecEndMaterial			- shutdown node and subnodes
 *
 */
#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeMaterial.h"
#include "miscadmin.h"

/* ----------------------------------------------------------------
 *		ExecMaterial
 *
 *		As long as we are at the end of the data collected in the tuplestore,
 *		we collect one new row from the subplan on each call, and stash it
 *		aside in the tuplestore before returning it.  The tuplestore is
 *		only read if we are asked to scan backwards, rescan, or mark/restore.
 *
 * ----------------------------------------------------------------
 */
static TupleTableSlot *			/* result tuple from subplan */
ExecMaterial(PlanState *pstate)
{
	MaterialState *node = castNode(MaterialState, pstate);
	EState	   *estate;
	ScanDirection dir;
	bool		forward;
	Tuplestorestate *tuplestorestate;
	bool		eof_tuplestore;
	TupleTableSlot *slot;

	CHECK_FOR_INTERRUPTS();

	/*
	 * get state info from node
	 */
	estate = node->ss.ps.state;
	dir = estate->es_direction;
	forward = ScanDirectionIsForward(dir);
	tuplestorestate = node->tuplestorestate;

	/*
	 * If first time through, and we need a tuplestore, initialize it.
	 */
	if (tuplestorestate == NULL && node->eflags != 0)
	{
		tuplestorestate = tuplestore_begin_heap(true, false, work_mem);
		tuplestore_set_eflags(tuplestorestate, node->eflags);
		if (node->eflags & EXEC_FLAG_MARK)
		{
			/*
			 * Allocate a second read pointer to serve as the mark. We know it
			 * must have index 1, so needn't store that.
			 */
			int			ptrno PG_USED_FOR_ASSERTS_ONLY;

			ptrno = tuplestore_alloc_read_pointer(tuplestorestate,
												  node->eflags);
			Assert(ptrno == 1);
		}
		node->tuplestorestate = tuplestorestate;
	}

	/*
	 * If we are not at the end of the tuplestore, or are going backwards, try
	 * to fetch a tuple from tuplestore.
	 */
	eof_tuplestore = (tuplestorestate == NULL) ||
		tuplestore_ateof(tuplestorestate);

	if (!forward && eof_tuplestore)
	{
		if (!node->eof_underlying)
		{
			/*
			 * When reversing direction at tuplestore EOF, the first
			 * gettupleslot call will fetch the last-added tuple; but we want
			 * to return the one before that, if possible. So do an extra
			 * fetch.
			 */
			if (!tuplestore_advance(tuplestorestate, forward))
				return NULL;	/* the tuplestore must be empty */
		}
		eof_tuplestore = false;
	}

	/*
	 * If we can fetch another tuple from the tuplestore, return it.
	 */
	slot = node->ss.ps.ps_ResultTupleSlot;
	if (!eof_tuplestore)
	{
		if (tuplestore_gettupleslot(tuplestorestate, forward, false, slot))
			return slot;
		if (forward)
			eof_tuplestore = true;
	}

	/*
	 * If necessary, try to fetch another row from the subplan.
	 *
	 * Note: the eof_underlying state variable exists to short-circuit further
	 * subplan calls.  It's not optional, unfortunately, because some plan
	 * node types are not robust about being called again when they've already
	 * returned NULL.
	 */
	if (eof_tuplestore && !node->eof_underlying)
	{
		PlanState  *outerNode;
		TupleTableSlot *outerslot;

		/*
		 * We can only get here with forward==true, so no need to worry about
		 * which direction the subplan will go.
		 */
		outerNode = outerPlanState(node);
		outerslot = ExecProcNode(outerNode);
		if (TupIsNull(outerslot))
		{
			node->eof_underlying = true;
			return NULL;
		}

		/*
		 * Append a copy of the returned tuple to tuplestore.  NOTE: because
		 * the tuplestore is certainly in EOF state, its read position will
		 * move forward over the added tuple.  This is what we want.
		 */
		if (tuplestorestate)
			tuplestore_puttupleslot(tuplestorestate, outerslot);

		ExecCopySlot(slot, outerslot);
		return slot;
	}

	/*
	 * Nothing left ...
	 */
	return ExecClearTuple(slot);
}

/* ----------------------------------------------------------------
 *		ExecInitMaterial
 * ----------------------------------------------------------------
 */
MaterialState *
ExecInitMaterial(Material *node, EState *estate, int eflags)
{
	MaterialState *matstate;
	Plan	   *outerPlan;

	/*
	 * create state structure
	 */
	matstate = makeNode(MaterialState);
	matstate->ss.ps.plan = (Plan *) node;
	matstate->ss.ps.state = estate;
	matstate->ss.ps.ExecProcNode = ExecMaterial;

	/*
	 * We must have a tuplestore buffering the subplan output to do backward
	 * scan or mark/restore.  We also prefer to materialize the subplan output
	 * if we might be called on to rewind and replay it many times. However,
	 * if none of these cases apply, we can skip storing the data.
	 */
	matstate->eflags = (eflags & (EXEC_FLAG_REWIND |
								  EXEC_FLAG_BACKWARD |
								  EXEC_FLAG_MARK));

	/*
	 * Tuplestore's interpretation of the flag bits is subtly different from
	 * the general executor meaning: it doesn't think BACKWARD necessarily
	 * means "backwards all the way to start".  If told to support BACKWARD we
	 * must include REWIND in the tuplestore eflags, else tuplestore_trim
	 * might throw away too much.
	 */
	if (eflags & EXEC_FLAG_BACKWARD)
		matstate->eflags |= EXEC_FLAG_REWIND;

	matstate->eof_underlying = false;
	matstate->tuplestorestate = NULL;

	/*
	 * Miscellaneous initialization
	 *
	 * Materialization nodes don't need ExprContexts because they never call
	 * ExecQual or ExecProject.
	 */

	/*
	 * initialize child nodes
	 *
	 * We shield the child node from the need to support REWIND, BACKWARD, or
	 * MARK/RESTORE.
	 */
	eflags &= ~(EXEC_FLAG_REWIND | EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK);

	outerPlan = outerPlan(node);
	outerPlanState(matstate) = ExecInitNode(outerPlan, estate, eflags);

	/*
	 * Initialize result type and slot. No need to initialize projection info
	 * because this node doesn't do projections.
	 *
	 * material nodes only return tuples from their materialized relation.
	 */
	ExecInitResultTupleSlotTL(&matstate->ss.ps, &TTSOpsMinimalTuple);
	matstate->ss.ps.ps_ProjInfo = NULL;

	/*
	 * initialize tuple type.
	 */
	ExecCreateScanSlotFromOuterPlan(estate, &matstate->ss, &TTSOpsMinimalTuple);

	return matstate;
}

/* ----------------------------------------------------------------
 *		ExecEndMaterial
 * ----------------------------------------------------------------
 */
void
ExecEndMaterial(MaterialState *node)
{
	/*
	 * Release tuplestore resources
	 */
	if (node->tuplestorestate != NULL)
		tuplestore_end(node->tuplestorestate);
	node->tuplestorestate = NULL;

	/*
	 * shut down the subplan
	 */
	ExecEndNode(outerPlanState(node));
}

/* ----------------------------------------------------------------
 *		ExecMaterialMarkPos
 *
 *		Calls tuplestore to save the current position in the stored file.
 * ----------------------------------------------------------------
 */
void
ExecMaterialMarkPos(MaterialState *node)
{
	Assert(node->eflags & EXEC_FLAG_MARK);

	/*
	 * if we haven't materialized yet, just return.
	 */
	if (!node->tuplestorestate)
		return;

	/*
	 * copy the active read pointer to the mark.
	 */
	tuplestore_copy_read_pointer(node->tuplestorestate, 0, 1);

	/*
	 * since we may have advanced the mark, try to truncate the tuplestore.
	 */
	tuplestore_trim(node->tuplestorestate);
}

/* ----------------------------------------------------------------
 *		ExecMaterialRestrPos
 *
 *		Calls tuplestore to restore the last saved file position.
 * ----------------------------------------------------------------
 */
void
ExecMaterialRestrPos(MaterialState *node)
{
	Assert(node->eflags & EXEC_FLAG_MARK);

	/*
	 * if we haven't materialized yet, just return.
	 */
	if (!node->tuplestorestate)
		return;

	/*
	 * copy the mark to the active read pointer.
	 */
	tuplestore_copy_read_pointer(node->tuplestorestate, 1, 0);
}

/* ----------------------------------------------------------------
 *		ExecReScanMaterial
 *
 *		Rescans the materialized relation.
 * ----------------------------------------------------------------
 */
void
ExecReScanMaterial(MaterialState *node)
{
	PlanState  *outerPlan = outerPlanState(node);

	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);

	if (node->eflags != 0)
	{
		/*
		 * If we haven't materialized yet, just return. If outerplan's
		 * chgParam is not NULL then it will be re-scanned by ExecProcNode,
		 * else no reason to re-scan it at all.
		 */
		if (!node->tuplestorestate)
			return;

		/*
		 * If subnode is to be rescanned then we forget previous stored
		 * results; we have to re-read the subplan and re-store.  Also, if we
		 * told tuplestore it needn't support rescan, we lose and must
		 * re-read.  (This last should not happen in common cases; else our
		 * caller lied by not passing EXEC_FLAG_REWIND to us.)
		 *
		 * Otherwise we can just rewind and rescan the stored output. The
		 * state of the subnode does not change.
		 */
		if (outerPlan->chgParam != NULL ||
			(node->eflags & EXEC_FLAG_REWIND) == 0)
		{
			tuplestore_end(node->tuplestorestate);
			node->tuplestorestate = NULL;
			if (outerPlan->chgParam == NULL)
				ExecReScan(outerPlan);
			node->eof_underlying = false;
		}
		else
			tuplestore_rescan(node->tuplestorestate);
	}
	else
	{
		/* In this case we are just passing on the subquery's output */

		/*
		 * if chgParam of subnode is not null then plan will be re-scanned by
		 * first ExecProcNode.
		 */
		if (outerPlan->chgParam == NULL)
			ExecReScan(outerPlan);
		node->eof_underlying = false;
	}
}
