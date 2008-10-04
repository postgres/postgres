/*-------------------------------------------------------------------------
 *
 * nodeRecursiveunion.c
 *	  routines to handle RecursiveUnion nodes.
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeRecursiveunion.c,v 1.1 2008/10/04 21:56:53 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "executor/execdebug.h"
#include "executor/nodeRecursiveunion.h"
#include "miscadmin.h"


/* ----------------------------------------------------------------
 *		ExecRecursiveUnion(node)
 *
 *		Scans the recursive query sequentially and returns the next
 *      qualifying tuple.
 *
 * 1. evaluate non recursive term and assign the result to RT
 *
 * 2. execute recursive terms
 *
 * 2.1 WT := RT
 * 2.2 while WT is not empty repeat 2.3 to 2.6. if WT is empty returns RT
 * 2.3 replace the name of recursive term with WT
 * 2.4 evaluate the recursive term and store into WT
 * 2.5 append WT to RT
 * 2.6 go back to 2.2
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecRecursiveUnion(RecursiveUnionState *node)
{
	PlanState  *outerPlan = outerPlanState(node);
	PlanState  *innerPlan = innerPlanState(node);
	RecursiveUnion *plan = (RecursiveUnion *) node->ps.plan;
	TupleTableSlot *slot;

	/* 1. Evaluate non-recursive term */
	if (!node->recursing)
	{
		slot = ExecProcNode(outerPlan);
		if (!TupIsNull(slot))
		{
			tuplestore_puttupleslot(node->working_table, slot);
			return slot;
		}
		node->recursing = true;
	}

retry:
	/* 2. Execute recursive term */
	slot = ExecProcNode(innerPlan);
	if (TupIsNull(slot))
	{
		if (node->intermediate_empty)
			return NULL;

		/* done with old working table ... */
		tuplestore_end(node->working_table);

		/* intermediate table becomes working table */
		node->working_table = node->intermediate_table;

		/* create new empty intermediate table */
		node->intermediate_table = tuplestore_begin_heap(false, false, work_mem);
		node->intermediate_empty = true;

		/* and reset the inner plan */
		innerPlan->chgParam = bms_add_member(innerPlan->chgParam,
											 plan->wtParam);
		goto retry;
	}
 	else
 	{
 		node->intermediate_empty = false;
 		tuplestore_puttupleslot(node->intermediate_table, slot);
 	}

	return slot;
}

/* ----------------------------------------------------------------
 *		ExecInitRecursiveUnionScan
 * ----------------------------------------------------------------
 */
RecursiveUnionState *
ExecInitRecursiveUnion(RecursiveUnion *node, EState *estate, int eflags)
{
	RecursiveUnionState *rustate;
	ParamExecData *prmdata;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * create state structure
	 */
	rustate = makeNode(RecursiveUnionState);
	rustate->ps.plan = (Plan *) node;
	rustate->ps.state = estate;

	/* initialize processing state */
	rustate->recursing = false;
	rustate->intermediate_empty = true;
	rustate->working_table = tuplestore_begin_heap(false, false, work_mem);
	rustate->intermediate_table = tuplestore_begin_heap(false, false, work_mem);

	/*
	 * Make the state structure available to descendant WorkTableScan nodes
	 * via the Param slot reserved for it.
	 */
	prmdata = &(estate->es_param_exec_vals[node->wtParam]);
	Assert(prmdata->execPlan == NULL);
	prmdata->value = PointerGetDatum(rustate);
	prmdata->isnull = false;

	/*
	 * Miscellaneous initialization
	 *
	 * RecursiveUnion plans don't have expression contexts because they never
	 * call ExecQual or ExecProject.
	 */
	Assert(node->plan.qual == NIL);

#define RECURSIVEUNION_NSLOTS 1

	/*
	 * RecursiveUnion nodes still have Result slots, which hold pointers to
	 * tuples, so we have to initialize them.
	 */
	ExecInitResultTupleSlot(estate, &rustate->ps);

	/*
	 * Initialize result tuple type and projection info.  (Note: we have
	 * to set up the result type before initializing child nodes, because
	 * nodeWorktablescan.c expects it to be valid.)
	 */
	ExecAssignResultTypeFromTL(&rustate->ps);
	rustate->ps.ps_ProjInfo = NULL;

	/*
	 * initialize child nodes
	 */
	outerPlanState(rustate) = ExecInitNode(outerPlan(node), estate, eflags);
	innerPlanState(rustate) = ExecInitNode(innerPlan(node), estate, eflags);

	return rustate;
}

int
ExecCountSlotsRecursiveUnion(RecursiveUnion *node)
{
	return ExecCountSlotsNode(outerPlan(node)) +
		ExecCountSlotsNode(innerPlan(node)) +
		RECURSIVEUNION_NSLOTS;
}

/* ----------------------------------------------------------------
 *		ExecEndRecursiveUnionScan
 *
 *		frees any storage allocated through C routines.
 * ----------------------------------------------------------------
 */
void
ExecEndRecursiveUnion(RecursiveUnionState *node)
{
	/* Release tuplestores */
	tuplestore_end(node->working_table);
	tuplestore_end(node->intermediate_table);

	/*
	 * clean out the upper tuple table
	 */
	ExecClearTuple(node->ps.ps_ResultTupleSlot);

	/*
	 * close down subplans
	 */
	ExecEndNode(outerPlanState(node));
	ExecEndNode(innerPlanState(node));
}

/* ----------------------------------------------------------------
 *		ExecRecursiveUnionReScan
 *
 *		Rescans the relation.
 * ----------------------------------------------------------------
 */
void
ExecRecursiveUnionReScan(RecursiveUnionState *node, ExprContext *exprCtxt)
{
	PlanState  *outerPlan = outerPlanState(node);
	PlanState  *innerPlan = innerPlanState(node);
	RecursiveUnion *plan = (RecursiveUnion *) node->ps.plan;

	/*
	 * Set recursive term's chgParam to tell it that we'll modify the
	 * working table and therefore it has to rescan.
	 */
	innerPlan->chgParam = bms_add_member(innerPlan->chgParam, plan->wtParam);

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.  Because of above, we only have to do this to
	 * the non-recursive term.
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan, exprCtxt);

	/* reset processing state */
	node->recursing = false;
	node->intermediate_empty = true;
	tuplestore_clear(node->working_table);
	tuplestore_clear(node->intermediate_table);
}
