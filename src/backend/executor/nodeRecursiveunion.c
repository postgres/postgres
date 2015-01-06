/*-------------------------------------------------------------------------
 *
 * nodeRecursiveunion.c
 *	  routines to handle RecursiveUnion nodes.
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeRecursiveunion.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "executor/execdebug.h"
#include "executor/nodeRecursiveunion.h"
#include "miscadmin.h"
#include "utils/memutils.h"


/*
 * To implement UNION (without ALL), we need a hashtable that stores tuples
 * already seen.  The hash key is computed from the grouping columns.
 */
typedef struct RUHashEntryData *RUHashEntry;

typedef struct RUHashEntryData
{
	TupleHashEntryData shared;	/* common header for hash table entries */
}	RUHashEntryData;


/*
 * Initialize the hash table to empty.
 */
static void
build_hash_table(RecursiveUnionState *rustate)
{
	RecursiveUnion *node = (RecursiveUnion *) rustate->ps.plan;

	Assert(node->numCols > 0);
	Assert(node->numGroups > 0);

	rustate->hashtable = BuildTupleHashTable(node->numCols,
											 node->dupColIdx,
											 rustate->eqfunctions,
											 rustate->hashfunctions,
											 node->numGroups,
											 sizeof(RUHashEntryData),
											 rustate->tableContext,
											 rustate->tempContext);
}


/* ----------------------------------------------------------------
 *		ExecRecursiveUnion(node)
 *
 *		Scans the recursive query sequentially and returns the next
 *		qualifying tuple.
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
	bool		isnew;

	/* 1. Evaluate non-recursive term */
	if (!node->recursing)
	{
		for (;;)
		{
			slot = ExecProcNode(outerPlan);
			if (TupIsNull(slot))
				break;
			if (plan->numCols > 0)
			{
				/* Find or build hashtable entry for this tuple's group */
				LookupTupleHashEntry(node->hashtable, slot, &isnew);
				/* Must reset temp context after each hashtable lookup */
				MemoryContextReset(node->tempContext);
				/* Ignore tuple if already seen */
				if (!isnew)
					continue;
			}
			/* Each non-duplicate tuple goes to the working table ... */
			tuplestore_puttupleslot(node->working_table, slot);
			/* ... and to the caller */
			return slot;
		}
		node->recursing = true;
	}

	/* 2. Execute recursive term */
	for (;;)
	{
		slot = ExecProcNode(innerPlan);
		if (TupIsNull(slot))
		{
			/* Done if there's nothing in the intermediate table */
			if (node->intermediate_empty)
				break;

			/* done with old working table ... */
			tuplestore_end(node->working_table);

			/* intermediate table becomes working table */
			node->working_table = node->intermediate_table;

			/* create new empty intermediate table */
			node->intermediate_table = tuplestore_begin_heap(false, false,
															 work_mem);
			node->intermediate_empty = true;

			/* reset the recursive term */
			innerPlan->chgParam = bms_add_member(innerPlan->chgParam,
												 plan->wtParam);

			/* and continue fetching from recursive term */
			continue;
		}

		if (plan->numCols > 0)
		{
			/* Find or build hashtable entry for this tuple's group */
			LookupTupleHashEntry(node->hashtable, slot, &isnew);
			/* Must reset temp context after each hashtable lookup */
			MemoryContextReset(node->tempContext);
			/* Ignore tuple if already seen */
			if (!isnew)
				continue;
		}

		/* Else, tuple is good; stash it in intermediate table ... */
		node->intermediate_empty = false;
		tuplestore_puttupleslot(node->intermediate_table, slot);
		/* ... and return it */
		return slot;
	}

	return NULL;
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

	rustate->eqfunctions = NULL;
	rustate->hashfunctions = NULL;
	rustate->hashtable = NULL;
	rustate->tempContext = NULL;
	rustate->tableContext = NULL;

	/* initialize processing state */
	rustate->recursing = false;
	rustate->intermediate_empty = true;
	rustate->working_table = tuplestore_begin_heap(false, false, work_mem);
	rustate->intermediate_table = tuplestore_begin_heap(false, false, work_mem);

	/*
	 * If hashing, we need a per-tuple memory context for comparisons, and a
	 * longer-lived context to store the hash table.  The table can't just be
	 * kept in the per-query context because we want to be able to throw it
	 * away when rescanning.
	 */
	if (node->numCols > 0)
	{
		rustate->tempContext =
			AllocSetContextCreate(CurrentMemoryContext,
								  "RecursiveUnion",
								  ALLOCSET_DEFAULT_MINSIZE,
								  ALLOCSET_DEFAULT_INITSIZE,
								  ALLOCSET_DEFAULT_MAXSIZE);
		rustate->tableContext =
			AllocSetContextCreate(CurrentMemoryContext,
								  "RecursiveUnion hash table",
								  ALLOCSET_DEFAULT_MINSIZE,
								  ALLOCSET_DEFAULT_INITSIZE,
								  ALLOCSET_DEFAULT_MAXSIZE);
	}

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

	/*
	 * RecursiveUnion nodes still have Result slots, which hold pointers to
	 * tuples, so we have to initialize them.
	 */
	ExecInitResultTupleSlot(estate, &rustate->ps);

	/*
	 * Initialize result tuple type and projection info.  (Note: we have to
	 * set up the result type before initializing child nodes, because
	 * nodeWorktablescan.c expects it to be valid.)
	 */
	ExecAssignResultTypeFromTL(&rustate->ps);
	rustate->ps.ps_ProjInfo = NULL;

	/*
	 * initialize child nodes
	 */
	outerPlanState(rustate) = ExecInitNode(outerPlan(node), estate, eflags);
	innerPlanState(rustate) = ExecInitNode(innerPlan(node), estate, eflags);

	/*
	 * If hashing, precompute fmgr lookup data for inner loop, and create the
	 * hash table.
	 */
	if (node->numCols > 0)
	{
		execTuplesHashPrepare(node->numCols,
							  node->dupOperators,
							  &rustate->eqfunctions,
							  &rustate->hashfunctions);
		build_hash_table(rustate);
	}

	return rustate;
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

	/* free subsidiary stuff including hashtable */
	if (node->tempContext)
		MemoryContextDelete(node->tempContext);
	if (node->tableContext)
		MemoryContextDelete(node->tableContext);

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
 *		ExecReScanRecursiveUnion
 *
 *		Rescans the relation.
 * ----------------------------------------------------------------
 */
void
ExecReScanRecursiveUnion(RecursiveUnionState *node)
{
	PlanState  *outerPlan = outerPlanState(node);
	PlanState  *innerPlan = innerPlanState(node);
	RecursiveUnion *plan = (RecursiveUnion *) node->ps.plan;

	/*
	 * Set recursive term's chgParam to tell it that we'll modify the working
	 * table and therefore it has to rescan.
	 */
	innerPlan->chgParam = bms_add_member(innerPlan->chgParam, plan->wtParam);

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.  Because of above, we only have to do this to the
	 * non-recursive term.
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);

	/* Release any hashtable storage */
	if (node->tableContext)
		MemoryContextResetAndDeleteChildren(node->tableContext);

	/* And rebuild empty hashtable if needed */
	if (plan->numCols > 0)
		build_hash_table(node);

	/* reset processing state */
	node->recursing = false;
	node->intermediate_empty = true;
	tuplestore_clear(node->working_table);
	tuplestore_clear(node->intermediate_table);
}
