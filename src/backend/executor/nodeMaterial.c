/*-------------------------------------------------------------------------
 *
 * nodeMaterial.c
 *	  Routines to handle materialization nodes.
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeMaterial.c,v 1.40 2002/12/15 16:17:46 tgl Exp $
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
 *		(a temporary tuple storage structure).	The first tuple is then
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
ExecMaterial(MaterialState *node)
{
	EState	   *estate;
	ScanDirection dir;
	Tuplestorestate *tuplestorestate;
	HeapTuple	heapTuple;
	TupleTableSlot *slot;
	bool		should_free;

	/*
	 * get state info from node
	 */
	estate = node->ss.ps.state;
	dir = estate->es_direction;
	tuplestorestate = (Tuplestorestate *) node->tuplestorestate;

	/*
	 * If first time through, read all tuples from outer plan and pass
	 * them to tuplestore.c. Subsequent calls just fetch tuples from
	 * tuplestore.
	 */

	if (tuplestorestate == NULL)
	{
		PlanState  *outerNode;

		/*
		 * Want to scan subplan in the forward direction while creating
		 * the stored data.  (Does setting my direction actually affect
		 * the subplan?  I bet this is useless code...)
		 */
		estate->es_direction = ForwardScanDirection;

		/*
		 * Initialize tuplestore module.
		 */
		tuplestorestate = tuplestore_begin_heap(true,	/* randomAccess */
												SortMem);

		node->tuplestorestate = (void *) tuplestorestate;

		/*
		 * Scan the subplan and feed all the tuples to tuplestore.
		 */
		outerNode = outerPlanState(node);

		for (;;)
		{
			slot = ExecProcNode(outerNode);

			if (TupIsNull(slot))
				break;

			tuplestore_puttuple(tuplestorestate, (void *) slot->val);
			ExecClearTuple(slot);
		}

		/*
		 * Complete the store.
		 */
		tuplestore_donestoring(tuplestorestate);

		/*
		 * restore to user specified direction
		 */
		estate->es_direction = dir;
	}

	/*
	 * Get the first or next tuple from tuplestore. Returns NULL if no
	 * more tuples.
	 */
	slot = (TupleTableSlot *) node->ss.ps.ps_ResultTupleSlot;
	heapTuple = tuplestore_getheaptuple(tuplestorestate,
										ScanDirectionIsForward(dir),
										&should_free);

	return ExecStoreTuple(heapTuple, slot, InvalidBuffer, should_free);
}

/* ----------------------------------------------------------------
 *		ExecInitMaterial
 * ----------------------------------------------------------------
 */
MaterialState *
ExecInitMaterial(Material *node, EState *estate)
{
	MaterialState *matstate;
	Plan	   *outerPlan;

	/*
	 * create state structure
	 */
	matstate = makeNode(MaterialState);
	matstate->ss.ps.plan = (Plan *) node;
	matstate->ss.ps.state = estate;

	matstate->tuplestorestate = NULL;

	/*
	 * Miscellaneous initialization
	 *
	 * Materialization nodes don't need ExprContexts because they never call
	 * ExecQual or ExecProject.
	 */

#define MATERIAL_NSLOTS 2

	/*
	 * tuple table initialization
	 *
	 * material nodes only return tuples from their materialized relation.
	 */
	ExecInitResultTupleSlot(estate, &matstate->ss.ps);
	ExecInitScanTupleSlot(estate, &matstate->ss);

	/*
	 * initializes child nodes
	 */
	outerPlan = outerPlan(node);
	outerPlanState(matstate) = ExecInitNode(outerPlan, estate);

	/*
	 * initialize tuple type.  no need to initialize projection info
	 * because this node doesn't do projections.
	 */
	ExecAssignResultTypeFromOuterPlan(&matstate->ss.ps);
	ExecAssignScanTypeFromOuterPlan(&matstate->ss);
	matstate->ss.ps.ps_ProjInfo = NULL;

	return matstate;
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
ExecEndMaterial(MaterialState *node)
{
	/*
	 * clean out the tuple table
	 */
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/*
	 * Release tuplestore resources
	 */
	if (node->tuplestorestate != NULL)
		tuplestore_end((Tuplestorestate *) node->tuplestorestate);
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
	/*
	 * if we haven't materialized yet, just return.
	 */
	if (!node->tuplestorestate)
		return;

	tuplestore_markpos((Tuplestorestate *) node->tuplestorestate);
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
	/*
	 * if we haven't materialized yet, just return.
	 */
	if (!node->tuplestorestate)
		return;

	/*
	 * restore the scan to the previously marked position
	 */
	tuplestore_restorepos((Tuplestorestate *) node->tuplestorestate);
}

/* ----------------------------------------------------------------
 *		ExecMaterialReScan
 *
 *		Rescans the materialized relation.
 * ----------------------------------------------------------------
 */
void
ExecMaterialReScan(MaterialState *node, ExprContext *exprCtxt)
{
	/*
	 * If we haven't materialized yet, just return. If outerplan' chgParam
	 * is not NULL then it will be re-scanned by ExecProcNode, else - no
	 * reason to re-scan it at all.
	 */
	if (!node->tuplestorestate)
		return;

	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);

	/*
	 * If subnode is to be rescanned then we forget previous stored
	 * results; we have to re-read the subplan and re-store.
	 *
	 * Otherwise we can just rewind and rescan the stored output.
	 */
	if (((PlanState *) node)->lefttree->chgParam != NULL)
	{
		tuplestore_end((Tuplestorestate *) node->tuplestorestate);
		node->tuplestorestate = NULL;
	}
	else
		tuplestore_rescan((Tuplestorestate *) node->tuplestorestate);
}
