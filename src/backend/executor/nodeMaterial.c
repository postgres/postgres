/*-------------------------------------------------------------------------
 *
 * nodeMaterial.c
 *	  Routines to handle materialization nodes.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeMaterial.c,v 1.32 2000/07/12 02:37:03 tgl Exp $
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
#include "utils/tuplestore.h"

/* ----------------------------------------------------------------
 *		ExecMaterial
 *
 *		The first time this is called, ExecMaterial retrieves tuples
 *		from this node's outer subplan and inserts them into a tuplestore
 *		(a temporary tuple storage structure).  The first tuple is then
 *		returned.  Successive calls to ExecMaterial return successive
 *		tuples from the tuplestore.
 *
 *		Initial State:
 *
 *		matstate->tuplestorestate is initially NULL, indicating we
 *		haven't yet collected the results of the subplan.
 *
 * ----------------------------------------------------------------
 */
TupleTableSlot *				/* result tuple from subplan */
ExecMaterial(Material *node)
{
	EState	   *estate;
	MaterialState *matstate;
	ScanDirection dir;
	Tuplestorestate *tuplestorestate;
	HeapTuple	heapTuple;
	TupleTableSlot *slot;
	bool		should_free;

	/* ----------------
	 *	get state info from node
	 * ----------------
	 */
	matstate = node->matstate;
	estate = node->plan.state;
	dir = estate->es_direction;
	tuplestorestate = (Tuplestorestate *) matstate->tuplestorestate;

	/* ----------------
	 *	If first time through, read all tuples from outer plan and
	 *	pass them to tuplestore.c.
	 *	Subsequent calls just fetch tuples from tuplestore.
	 * ----------------
	 */

	if (tuplestorestate == NULL)
	{
		Plan	   *outerNode;

		/* ----------------
		 *	Want to scan subplan in the forward direction while creating
		 *	the stored data.  (Does setting my direction actually affect
		 *	the subplan?  I bet this is useless code...)
		 * ----------------
		 */
		estate->es_direction = ForwardScanDirection;

		/* ----------------
		 *	 Initialize tuplestore module.
		 * ----------------
		 */
		tuplestorestate = tuplestore_begin_heap(true, /* randomAccess */
												SortMem);

		matstate->tuplestorestate = (void *) tuplestorestate;

		/* ----------------
		 *	 Scan the subplan and feed all the tuples to tuplestore.
		 * ----------------
		 */
		outerNode = outerPlan((Plan *) node);

		for (;;)
		{
			slot = ExecProcNode(outerNode, (Plan *) node);

			if (TupIsNull(slot))
				break;

			tuplestore_puttuple(tuplestorestate, (void *) slot->val);
			ExecClearTuple(slot);
		}

		/* ----------------
		 *	 Complete the store.
		 * ----------------
		 */
		tuplestore_donestoring(tuplestorestate);

		/* ----------------
		 *	 restore to user specified direction
		 * ----------------
		 */
		estate->es_direction = dir;
	}

	/* ----------------
	 *	Get the first or next tuple from tuplestore.
	 *	Returns NULL if no more tuples.
	 * ----------------
	 */
	slot = (TupleTableSlot *) matstate->csstate.cstate.cs_ResultTupleSlot;
	heapTuple = tuplestore_getheaptuple(tuplestorestate,
										ScanDirectionIsForward(dir),
										&should_free);

	return ExecStoreTuple(heapTuple, slot, InvalidBuffer, should_free);
}

/* ----------------------------------------------------------------
 *		ExecInitMaterial
 * ----------------------------------------------------------------
 */
bool							/* initialization status */
ExecInitMaterial(Material *node, EState *estate, Plan *parent)
{
	MaterialState *matstate;
	Plan	   *outerPlan;

	/* ----------------
	 *	assign the node's execution state
	 * ----------------
	 */
	node->plan.state = estate;

	/* ----------------
	 * create state structure
	 * ----------------
	 */
	matstate = makeNode(MaterialState);
	matstate->tuplestorestate = NULL;
	node->matstate = matstate;

	/* ----------------
	 *	Miscellaneous initialization
	 *
	 *	Materialization nodes don't need ExprContexts because
	 *	they never call ExecQual or ExecProject.
	 * ----------------
	 */

#define MATERIAL_NSLOTS 1
	/* ----------------
	 * tuple table initialization
	 *
	 *	material nodes only return tuples from their materialized
	 *	relation.
	 * ----------------
	 */
	ExecInitResultTupleSlot(estate, &matstate->csstate.cstate);
	ExecInitScanTupleSlot(estate, &matstate->csstate);

	/* ----------------
	 * initializes child nodes
	 * ----------------
	 */
	outerPlan = outerPlan((Plan *) node);
	ExecInitNode(outerPlan, estate, (Plan *) node);

	/* ----------------
	 *	initialize tuple type.	no need to initialize projection
	 *	info because this node doesn't do projections.
	 * ----------------
	 */
	ExecAssignResultTypeFromOuterPlan((Plan *) node, &matstate->csstate.cstate);
	ExecAssignScanTypeFromOuterPlan((Plan *) node, &matstate->csstate);
	matstate->csstate.cstate.cs_ProjInfo = NULL;

	return TRUE;
}

int
ExecCountSlotsMaterial(Material *node)
{
	return ExecCountSlotsNode(outerPlan((Plan *) node)) +
	ExecCountSlotsNode(innerPlan((Plan *) node)) +
	MATERIAL_NSLOTS;
}

/* ----------------------------------------------------------------
 *		ExecEndMaterial
 * ----------------------------------------------------------------
 */
void
ExecEndMaterial(Material *node)
{
	MaterialState *matstate;
	Plan	   *outerPlan;

	/* ----------------
	 *	get info from the material state
	 * ----------------
	 */
	matstate = node->matstate;

	/* ----------------
	 *	shut down the subplan
	 * ----------------
	 */
	outerPlan = outerPlan((Plan *) node);
	ExecEndNode(outerPlan, (Plan *) node);

	/* ----------------
	 *	clean out the tuple table
	 * ----------------
	 */
	ExecClearTuple(matstate->csstate.css_ScanTupleSlot);

	/* ----------------
	 *	Release tuplestore resources
	 * ----------------
	 */
	if (matstate->tuplestorestate != NULL)
		tuplestore_end((Tuplestorestate *) matstate->tuplestorestate);
	matstate->tuplestorestate = NULL;
}

/* ----------------------------------------------------------------
 *		ExecMaterialMarkPos
 *
 *		Calls tuplestore to save the current position in the stored file.
 * ----------------------------------------------------------------
 */
void
ExecMaterialMarkPos(Material *node)
{
	MaterialState  *matstate = node->matstate;

	/* ----------------
	 *	if we haven't materialized yet, just return.
	 * ----------------
	 */
	if (!matstate->tuplestorestate)
		return;

	tuplestore_markpos((Tuplestorestate *) matstate->tuplestorestate);
}

/* ----------------------------------------------------------------
 *		ExecMaterialRestrPos
 *
 *		Calls tuplestore to restore the last saved file position.
 * ----------------------------------------------------------------
 */
void
ExecMaterialRestrPos(Material *node)
{
	MaterialState  *matstate = node->matstate;

	/* ----------------
	 *	if we haven't materialized yet, just return.
	 * ----------------
	 */
	if (!matstate->tuplestorestate)
		return;

	/* ----------------
	 *	restore the scan to the previously marked position
	 * ----------------
	 */
	tuplestore_restorepos((Tuplestorestate *) matstate->tuplestorestate);
}

/* ----------------------------------------------------------------
 *		ExecMaterialReScan
 *
 *		Rescans the materialized relation.
 * ----------------------------------------------------------------
 */
void
ExecMaterialReScan(Material *node, ExprContext *exprCtxt, Plan *parent)
{
	MaterialState *matstate = node->matstate;

	/*
	 * If we haven't materialized yet, just return. If outerplan' chgParam is
	 * not NULL then it will be re-scanned by ExecProcNode, else - no
	 * reason to re-scan it at all.
	 */
	if (!matstate->tuplestorestate)
		return;

	ExecClearTuple(matstate->csstate.cstate.cs_ResultTupleSlot);

	/*
	 * If subnode is to be rescanned then we forget previous stored results;
	 * we have to re-read the subplan and re-store.
	 *
	 * Otherwise we can just rewind and rescan the stored output.
	 */
	if (((Plan *) node)->lefttree->chgParam != NULL)
	{
		tuplestore_end((Tuplestorestate *) matstate->tuplestorestate);
		matstate->tuplestorestate = NULL;
	}
	else
		tuplestore_rescan((Tuplestorestate *) matstate->tuplestorestate);
}
