/*-------------------------------------------------------------------------
 *
 * nodeSetOp.c
 *	  Routines to handle INTERSECT and EXCEPT selection
 *
 * The input of a SetOp node consists of tuples from two relations,
 * which have been combined into one dataset, with a junk attribute added
 * that shows which relation each tuple came from.  In SETOP_SORTED mode,
 * the input has furthermore been sorted according to all the grouping
 * columns (ie, all the non-junk attributes).  The SetOp node scans each
 * group of identical tuples to determine how many came from each input
 * relation.  Then it is a simple matter to emit the output demanded by the
 * SQL spec for INTERSECT, INTERSECT ALL, EXCEPT, or EXCEPT ALL.
 *
 * In SETOP_HASHED mode, the input is delivered in no particular order,
 * except that we know all the tuples from one input relation will come before
 * all the tuples of the other.  The planner guarantees that the first input
 * relation is the left-hand one for EXCEPT, and tries to make the smaller
 * input relation come first for INTERSECT.  We build a hash table in memory
 * with one entry for each group of identical tuples, and count the number of
 * tuples in the group from each relation.  After seeing all the input, we
 * scan the hashtable and generate the correct output using those counts.
 * We can avoid making hashtable entries for any tuples appearing only in the
 * second input relation, since they cannot result in any output.
 *
 * This node type is not used for UNION or UNION ALL, since those can be
 * implemented more cheaply (there's no need for the junk attribute to
 * identify the source relation).
 *
 * Note that SetOp does no qual checking nor projection.  The delivered
 * output tuples are just copies of the first-to-arrive tuple in each
 * input group.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeSetOp.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "executor/executor.h"
#include "executor/nodeSetOp.h"
#include "miscadmin.h"
#include "utils/memutils.h"


/*
 * SetOpStatePerGroupData - per-group working state
 *
 * These values are working state that is initialized at the start of
 * an input tuple group and updated for each input tuple.
 *
 * In SETOP_SORTED mode, we need only one of these structs, and it's kept in
 * the plan state node.  In SETOP_HASHED mode, the hash table contains one
 * of these for each tuple group.
 */
typedef struct SetOpStatePerGroupData
{
	long		numLeft;		/* number of left-input dups in group */
	long		numRight;		/* number of right-input dups in group */
}			SetOpStatePerGroupData;


static TupleTableSlot *setop_retrieve_direct(SetOpState *setopstate);
static void setop_fill_hash_table(SetOpState *setopstate);
static TupleTableSlot *setop_retrieve_hash_table(SetOpState *setopstate);


/*
 * Initialize state for a new group of input values.
 */
static inline void
initialize_counts(SetOpStatePerGroup pergroup)
{
	pergroup->numLeft = pergroup->numRight = 0;
}

/*
 * Advance the appropriate counter for one input tuple.
 */
static inline void
advance_counts(SetOpStatePerGroup pergroup, int flag)
{
	if (flag)
		pergroup->numRight++;
	else
		pergroup->numLeft++;
}

/*
 * Fetch the "flag" column from an input tuple.
 * This is an integer column with value 0 for left side, 1 for right side.
 */
static int
fetch_tuple_flag(SetOpState *setopstate, TupleTableSlot *inputslot)
{
	SetOp	   *node = (SetOp *) setopstate->ps.plan;
	int			flag;
	bool		isNull;

	flag = DatumGetInt32(slot_getattr(inputslot,
									  node->flagColIdx,
									  &isNull));
	Assert(!isNull);
	Assert(flag == 0 || flag == 1);
	return flag;
}

/*
 * Initialize the hash table to empty.
 */
static void
build_hash_table(SetOpState *setopstate)
{
	SetOp	   *node = (SetOp *) setopstate->ps.plan;
	ExprContext *econtext = setopstate->ps.ps_ExprContext;
	TupleDesc	desc = ExecGetResultType(outerPlanState(setopstate));

	Assert(node->strategy == SETOP_HASHED);
	Assert(node->numGroups > 0);

	setopstate->hashtable = BuildTupleHashTableExt(&setopstate->ps,
												   desc,
												   node->numCols,
												   node->dupColIdx,
												   setopstate->eqfuncoids,
												   setopstate->hashfunctions,
												   node->dupCollations,
												   node->numGroups,
												   0,
												   setopstate->ps.state->es_query_cxt,
												   setopstate->tableContext,
												   econtext->ecxt_per_tuple_memory,
												   false);
}

/*
 * We've completed processing a tuple group.  Decide how many copies (if any)
 * of its representative row to emit, and store the count into numOutput.
 * This logic is straight from the SQL92 specification.
 */
static void
set_output_count(SetOpState *setopstate, SetOpStatePerGroup pergroup)
{
	SetOp	   *plannode = (SetOp *) setopstate->ps.plan;

	switch (plannode->cmd)
	{
		case SETOPCMD_INTERSECT:
			if (pergroup->numLeft > 0 && pergroup->numRight > 0)
				setopstate->numOutput = 1;
			else
				setopstate->numOutput = 0;
			break;
		case SETOPCMD_INTERSECT_ALL:
			setopstate->numOutput =
				(pergroup->numLeft < pergroup->numRight) ?
				pergroup->numLeft : pergroup->numRight;
			break;
		case SETOPCMD_EXCEPT:
			if (pergroup->numLeft > 0 && pergroup->numRight == 0)
				setopstate->numOutput = 1;
			else
				setopstate->numOutput = 0;
			break;
		case SETOPCMD_EXCEPT_ALL:
			setopstate->numOutput =
				(pergroup->numLeft < pergroup->numRight) ?
				0 : (pergroup->numLeft - pergroup->numRight);
			break;
		default:
			elog(ERROR, "unrecognized set op: %d", (int) plannode->cmd);
			break;
	}
}


/* ----------------------------------------------------------------
 *		ExecSetOp
 * ----------------------------------------------------------------
 */
static TupleTableSlot *			/* return: a tuple or NULL */
ExecSetOp(PlanState *pstate)
{
	SetOpState *node = castNode(SetOpState, pstate);
	SetOp	   *plannode = (SetOp *) node->ps.plan;
	TupleTableSlot *resultTupleSlot = node->ps.ps_ResultTupleSlot;

	CHECK_FOR_INTERRUPTS();

	/*
	 * If the previously-returned tuple needs to be returned more than once,
	 * keep returning it.
	 */
	if (node->numOutput > 0)
	{
		node->numOutput--;
		return resultTupleSlot;
	}

	/* Otherwise, we're done if we are out of groups */
	if (node->setop_done)
		return NULL;

	/* Fetch the next tuple group according to the correct strategy */
	if (plannode->strategy == SETOP_HASHED)
	{
		if (!node->table_filled)
			setop_fill_hash_table(node);
		return setop_retrieve_hash_table(node);
	}
	else
		return setop_retrieve_direct(node);
}

/*
 * ExecSetOp for non-hashed case
 */
static TupleTableSlot *
setop_retrieve_direct(SetOpState *setopstate)
{
	PlanState  *outerPlan;
	SetOpStatePerGroup pergroup;
	TupleTableSlot *outerslot;
	TupleTableSlot *resultTupleSlot;
	ExprContext *econtext = setopstate->ps.ps_ExprContext;

	/*
	 * get state info from node
	 */
	outerPlan = outerPlanState(setopstate);
	pergroup = (SetOpStatePerGroup) setopstate->pergroup;
	resultTupleSlot = setopstate->ps.ps_ResultTupleSlot;

	/*
	 * We loop retrieving groups until we find one we should return
	 */
	while (!setopstate->setop_done)
	{
		/*
		 * If we don't already have the first tuple of the new group, fetch it
		 * from the outer plan.
		 */
		if (setopstate->grp_firstTuple == NULL)
		{
			outerslot = ExecProcNode(outerPlan);
			if (!TupIsNull(outerslot))
			{
				/* Make a copy of the first input tuple */
				setopstate->grp_firstTuple = ExecCopySlotHeapTuple(outerslot);
			}
			else
			{
				/* outer plan produced no tuples at all */
				setopstate->setop_done = true;
				return NULL;
			}
		}

		/*
		 * Store the copied first input tuple in the tuple table slot reserved
		 * for it.  The tuple will be deleted when it is cleared from the
		 * slot.
		 */
		ExecStoreHeapTuple(setopstate->grp_firstTuple,
						   resultTupleSlot,
						   true);
		setopstate->grp_firstTuple = NULL;	/* don't keep two pointers */

		/* Initialize working state for a new input tuple group */
		initialize_counts(pergroup);

		/* Count the first input tuple */
		advance_counts(pergroup,
					   fetch_tuple_flag(setopstate, resultTupleSlot));

		/*
		 * Scan the outer plan until we exhaust it or cross a group boundary.
		 */
		for (;;)
		{
			outerslot = ExecProcNode(outerPlan);
			if (TupIsNull(outerslot))
			{
				/* no more outer-plan tuples available */
				setopstate->setop_done = true;
				break;
			}

			/*
			 * Check whether we've crossed a group boundary.
			 */
			econtext->ecxt_outertuple = resultTupleSlot;
			econtext->ecxt_innertuple = outerslot;

			if (!ExecQualAndReset(setopstate->eqfunction, econtext))
			{
				/*
				 * Save the first input tuple of the next group.
				 */
				setopstate->grp_firstTuple = ExecCopySlotHeapTuple(outerslot);
				break;
			}

			/* Still in same group, so count this tuple */
			advance_counts(pergroup,
						   fetch_tuple_flag(setopstate, outerslot));
		}

		/*
		 * Done scanning input tuple group.  See if we should emit any copies
		 * of result tuple, and if so return the first copy.
		 */
		set_output_count(setopstate, pergroup);

		if (setopstate->numOutput > 0)
		{
			setopstate->numOutput--;
			return resultTupleSlot;
		}
	}

	/* No more groups */
	ExecClearTuple(resultTupleSlot);
	return NULL;
}

/*
 * ExecSetOp for hashed case: phase 1, read input and build hash table
 */
static void
setop_fill_hash_table(SetOpState *setopstate)
{
	SetOp	   *node = (SetOp *) setopstate->ps.plan;
	PlanState  *outerPlan;
	int			firstFlag;
	bool		in_first_rel PG_USED_FOR_ASSERTS_ONLY;
	ExprContext *econtext = setopstate->ps.ps_ExprContext;

	/*
	 * get state info from node
	 */
	outerPlan = outerPlanState(setopstate);
	firstFlag = node->firstFlag;
	/* verify planner didn't mess up */
	Assert(firstFlag == 0 ||
		   (firstFlag == 1 &&
			(node->cmd == SETOPCMD_INTERSECT ||
			 node->cmd == SETOPCMD_INTERSECT_ALL)));

	/*
	 * Process each outer-plan tuple, and then fetch the next one, until we
	 * exhaust the outer plan.
	 */
	in_first_rel = true;
	for (;;)
	{
		TupleTableSlot *outerslot;
		int			flag;
		TupleHashEntryData *entry;
		bool		isnew;

		outerslot = ExecProcNode(outerPlan);
		if (TupIsNull(outerslot))
			break;

		/* Identify whether it's left or right input */
		flag = fetch_tuple_flag(setopstate, outerslot);

		if (flag == firstFlag)
		{
			/* (still) in first input relation */
			Assert(in_first_rel);

			/* Find or build hashtable entry for this tuple's group */
			entry = LookupTupleHashEntry(setopstate->hashtable, outerslot,
										 &isnew, NULL);

			/* If new tuple group, initialize counts */
			if (isnew)
			{
				entry->additional = (SetOpStatePerGroup)
					MemoryContextAlloc(setopstate->hashtable->tablecxt,
									   sizeof(SetOpStatePerGroupData));
				initialize_counts((SetOpStatePerGroup) entry->additional);
			}

			/* Advance the counts */
			advance_counts((SetOpStatePerGroup) entry->additional, flag);
		}
		else
		{
			/* reached second relation */
			in_first_rel = false;

			/* For tuples not seen previously, do not make hashtable entry */
			entry = LookupTupleHashEntry(setopstate->hashtable, outerslot,
										 NULL, NULL);

			/* Advance the counts if entry is already present */
			if (entry)
				advance_counts((SetOpStatePerGroup) entry->additional, flag);
		}

		/* Must reset expression context after each hashtable lookup */
		ResetExprContext(econtext);
	}

	setopstate->table_filled = true;
	/* Initialize to walk the hash table */
	ResetTupleHashIterator(setopstate->hashtable, &setopstate->hashiter);
}

/*
 * ExecSetOp for hashed case: phase 2, retrieving groups from hash table
 */
static TupleTableSlot *
setop_retrieve_hash_table(SetOpState *setopstate)
{
	TupleHashEntryData *entry;
	TupleTableSlot *resultTupleSlot;

	/*
	 * get state info from node
	 */
	resultTupleSlot = setopstate->ps.ps_ResultTupleSlot;

	/*
	 * We loop retrieving groups until we find one we should return
	 */
	while (!setopstate->setop_done)
	{
		CHECK_FOR_INTERRUPTS();

		/*
		 * Find the next entry in the hash table
		 */
		entry = ScanTupleHashTable(setopstate->hashtable, &setopstate->hashiter);
		if (entry == NULL)
		{
			/* No more entries in hashtable, so done */
			setopstate->setop_done = true;
			return NULL;
		}

		/*
		 * See if we should emit any copies of this tuple, and if so return
		 * the first copy.
		 */
		set_output_count(setopstate, (SetOpStatePerGroup) entry->additional);

		if (setopstate->numOutput > 0)
		{
			setopstate->numOutput--;
			return ExecStoreMinimalTuple(entry->firstTuple,
										 resultTupleSlot,
										 false);
		}
	}

	/* No more groups */
	ExecClearTuple(resultTupleSlot);
	return NULL;
}

/* ----------------------------------------------------------------
 *		ExecInitSetOp
 *
 *		This initializes the setop node state structures and
 *		the node's subplan.
 * ----------------------------------------------------------------
 */
SetOpState *
ExecInitSetOp(SetOp *node, EState *estate, int eflags)
{
	SetOpState *setopstate;
	TupleDesc	outerDesc;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * create state structure
	 */
	setopstate = makeNode(SetOpState);
	setopstate->ps.plan = (Plan *) node;
	setopstate->ps.state = estate;
	setopstate->ps.ExecProcNode = ExecSetOp;

	setopstate->eqfuncoids = NULL;
	setopstate->hashfunctions = NULL;
	setopstate->setop_done = false;
	setopstate->numOutput = 0;
	setopstate->pergroup = NULL;
	setopstate->grp_firstTuple = NULL;
	setopstate->hashtable = NULL;
	setopstate->tableContext = NULL;

	/*
	 * create expression context
	 */
	ExecAssignExprContext(estate, &setopstate->ps);

	/*
	 * If hashing, we also need a longer-lived context to store the hash
	 * table.  The table can't just be kept in the per-query context because
	 * we want to be able to throw it away in ExecReScanSetOp.
	 */
	if (node->strategy == SETOP_HASHED)
		setopstate->tableContext =
			AllocSetContextCreate(CurrentMemoryContext,
								  "SetOp hash table",
								  ALLOCSET_DEFAULT_SIZES);

	/*
	 * initialize child nodes
	 *
	 * If we are hashing then the child plan does not need to handle REWIND
	 * efficiently; see ExecReScanSetOp.
	 */
	if (node->strategy == SETOP_HASHED)
		eflags &= ~EXEC_FLAG_REWIND;
	outerPlanState(setopstate) = ExecInitNode(outerPlan(node), estate, eflags);
	outerDesc = ExecGetResultType(outerPlanState(setopstate));

	/*
	 * Initialize result slot and type. Setop nodes do no projections, so
	 * initialize projection info for this node appropriately.
	 */
	ExecInitResultTupleSlotTL(&setopstate->ps,
							  node->strategy == SETOP_HASHED ?
							  &TTSOpsMinimalTuple : &TTSOpsHeapTuple);
	setopstate->ps.ps_ProjInfo = NULL;

	/*
	 * Precompute fmgr lookup data for inner loop. We need both equality and
	 * hashing functions to do it by hashing, but only equality if not
	 * hashing.
	 */
	if (node->strategy == SETOP_HASHED)
		execTuplesHashPrepare(node->numCols,
							  node->dupOperators,
							  &setopstate->eqfuncoids,
							  &setopstate->hashfunctions);
	else
		setopstate->eqfunction =
			execTuplesMatchPrepare(outerDesc,
								   node->numCols,
								   node->dupColIdx,
								   node->dupOperators,
								   node->dupCollations,
								   &setopstate->ps);

	if (node->strategy == SETOP_HASHED)
	{
		build_hash_table(setopstate);
		setopstate->table_filled = false;
	}
	else
	{
		setopstate->pergroup =
			(SetOpStatePerGroup) palloc0(sizeof(SetOpStatePerGroupData));
	}

	return setopstate;
}

/* ----------------------------------------------------------------
 *		ExecEndSetOp
 *
 *		This shuts down the subplan and frees resources allocated
 *		to this node.
 * ----------------------------------------------------------------
 */
void
ExecEndSetOp(SetOpState *node)
{
	/* free subsidiary stuff including hashtable */
	if (node->tableContext)
		MemoryContextDelete(node->tableContext);

	ExecEndNode(outerPlanState(node));
}


void
ExecReScanSetOp(SetOpState *node)
{
	PlanState  *outerPlan = outerPlanState(node);

	ExecClearTuple(node->ps.ps_ResultTupleSlot);
	node->setop_done = false;
	node->numOutput = 0;

	if (((SetOp *) node->ps.plan)->strategy == SETOP_HASHED)
	{
		/*
		 * In the hashed case, if we haven't yet built the hash table then we
		 * can just return; nothing done yet, so nothing to undo. If subnode's
		 * chgParam is not NULL then it will be re-scanned by ExecProcNode,
		 * else no reason to re-scan it at all.
		 */
		if (!node->table_filled)
			return;

		/*
		 * If we do have the hash table and the subplan does not have any
		 * parameter changes, then we can just rescan the existing hash table;
		 * no need to build it again.
		 */
		if (outerPlan->chgParam == NULL)
		{
			ResetTupleHashIterator(node->hashtable, &node->hashiter);
			return;
		}
	}

	/* Release first tuple of group, if we have made a copy */
	if (node->grp_firstTuple != NULL)
	{
		heap_freetuple(node->grp_firstTuple);
		node->grp_firstTuple = NULL;
	}

	/* Release any hashtable storage */
	if (node->tableContext)
		MemoryContextReset(node->tableContext);

	/* And rebuild empty hashtable if needed */
	if (((SetOp *) node->ps.plan)->strategy == SETOP_HASHED)
	{
		ResetTupleHashTable(node->hashtable);
		node->table_filled = false;
	}

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);
}
