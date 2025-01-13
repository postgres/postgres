/*-------------------------------------------------------------------------
 *
 * nodeSetOp.c
 *	  Routines to handle INTERSECT and EXCEPT selection
 *
 * The input of a SetOp node consists of two relations (outer and inner)
 * with identical column sets.  In EXCEPT queries the outer relation is
 * always the left side, while in INTERSECT cases the planner tries to
 * make the outer relation be the smaller of the two inputs.
 *
 * In SETOP_SORTED mode, each input has been sorted according to all the
 * grouping columns.  The SetOp node essentially performs a merge join on
 * the grouping columns, except that it is only interested in counting how
 * many tuples from each input match.  Then it is a simple matter to emit
 * the output demanded by the SQL spec for INTERSECT, INTERSECT ALL, EXCEPT,
 * or EXCEPT ALL.
 *
 * In SETOP_HASHED mode, the inputs are delivered in no particular order.
 * We read the outer relation and build a hash table in memory with one entry
 * for each group of identical tuples, counting the number of tuples in the
 * group.  Then we read the inner relation and count the number of tuples
 * matching each outer group.  (We can disregard any tuples appearing only
 * in the inner relation, since they cannot result in any output.)  After
 * seeing all the input, we scan the hashtable and generate the correct
 * output using those counts.
 *
 * This node type is not used for UNION or UNION ALL, since those can be
 * implemented more cheaply (there's no need to count the number of
 * matching tuples).
 *
 * Note that SetOp does no qual checking nor projection.  The delivered
 * output tuples are just copies of the first-to-arrive tuple in each
 * input group.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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
 * In SETOP_SORTED mode, we need only one of these structs, and it's just a
 * local in setop_retrieve_sorted.  In SETOP_HASHED mode, the hash table
 * contains one of these for each tuple group.
 */
typedef struct SetOpStatePerGroupData
{
	int64		numLeft;		/* number of left-input dups in group */
	int64		numRight;		/* number of right-input dups in group */
} SetOpStatePerGroupData;

typedef SetOpStatePerGroupData *SetOpStatePerGroup;


static TupleTableSlot *setop_retrieve_sorted(SetOpState *setopstate);
static void setop_load_group(SetOpStatePerInput *input, PlanState *inputPlan,
							 SetOpState *setopstate);
static int	setop_compare_slots(TupleTableSlot *s1, TupleTableSlot *s2,
								SetOpState *setopstate);
static void setop_fill_hash_table(SetOpState *setopstate);
static TupleTableSlot *setop_retrieve_hash_table(SetOpState *setopstate);


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

	/*
	 * If both child plans deliver the same fixed tuple slot type, we can tell
	 * BuildTupleHashTable to expect that slot type as input.  Otherwise,
	 * we'll pass NULL denoting that any slot type is possible.
	 */
	setopstate->hashtable = BuildTupleHashTable(&setopstate->ps,
												desc,
												ExecGetCommonChildSlotOps(&setopstate->ps),
												node->numCols,
												node->cmpColIdx,
												setopstate->eqfuncoids,
												setopstate->hashfunctions,
												node->cmpCollations,
												node->numGroups,
												sizeof(SetOpStatePerGroupData),
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
		return setop_retrieve_sorted(node);
}

/*
 * ExecSetOp for non-hashed case
 */
static TupleTableSlot *
setop_retrieve_sorted(SetOpState *setopstate)
{
	PlanState  *outerPlan;
	PlanState  *innerPlan;
	TupleTableSlot *resultTupleSlot;

	/*
	 * get state info from node
	 */
	outerPlan = outerPlanState(setopstate);
	innerPlan = innerPlanState(setopstate);
	resultTupleSlot = setopstate->ps.ps_ResultTupleSlot;

	/*
	 * If first time through, establish the invariant that setop_load_group
	 * expects: each side's nextTupleSlot is the next output from the child
	 * plan, or empty if there is no more output from it.
	 */
	if (setopstate->need_init)
	{
		setopstate->need_init = false;

		setopstate->leftInput.nextTupleSlot = ExecProcNode(outerPlan);

		/*
		 * If the outer relation is empty, then we will emit nothing, and we
		 * don't need to read the inner relation at all.
		 */
		if (TupIsNull(setopstate->leftInput.nextTupleSlot))
		{
			setopstate->setop_done = true;
			return NULL;
		}

		setopstate->rightInput.nextTupleSlot = ExecProcNode(innerPlan);

		/* Set flags that we've not completed either side's group */
		setopstate->leftInput.needGroup = true;
		setopstate->rightInput.needGroup = true;
	}

	/*
	 * We loop retrieving groups until we find one we should return
	 */
	while (!setopstate->setop_done)
	{
		int			cmpresult;
		SetOpStatePerGroupData pergroup;

		/*
		 * Fetch the rest of the current outer group, if we didn't already.
		 */
		if (setopstate->leftInput.needGroup)
			setop_load_group(&setopstate->leftInput, outerPlan, setopstate);

		/*
		 * If no more outer groups, we're done, and don't need to look at any
		 * more of the inner relation.
		 */
		if (setopstate->leftInput.numTuples == 0)
		{
			setopstate->setop_done = true;
			break;
		}

		/*
		 * Fetch the rest of the current inner group, if we didn't already.
		 */
		if (setopstate->rightInput.needGroup)
			setop_load_group(&setopstate->rightInput, innerPlan, setopstate);

		/*
		 * Determine whether we have matching groups on both sides (this is
		 * basically like the core logic of a merge join).
		 */
		if (setopstate->rightInput.numTuples == 0)
			cmpresult = -1;		/* as though left input is lesser */
		else
			cmpresult = setop_compare_slots(setopstate->leftInput.firstTupleSlot,
											setopstate->rightInput.firstTupleSlot,
											setopstate);

		if (cmpresult < 0)
		{
			/* Left group is first, and has no right matches */
			pergroup.numLeft = setopstate->leftInput.numTuples;
			pergroup.numRight = 0;
			/* We'll need another left group next time */
			setopstate->leftInput.needGroup = true;
		}
		else if (cmpresult == 0)
		{
			/* We have matching groups */
			pergroup.numLeft = setopstate->leftInput.numTuples;
			pergroup.numRight = setopstate->rightInput.numTuples;
			/* We'll need to read from both sides next time */
			setopstate->leftInput.needGroup = true;
			setopstate->rightInput.needGroup = true;
		}
		else
		{
			/* Right group has no left matches, so we can ignore it */
			setopstate->rightInput.needGroup = true;
			continue;
		}

		/*
		 * Done scanning these input tuple groups.  See if we should emit any
		 * copies of result tuple, and if so return the first copy.  (Note
		 * that the result tuple is the same as the left input's firstTuple
		 * slot.)
		 */
		set_output_count(setopstate, &pergroup);

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
 * Load next group of tuples from one child plan or the other.
 *
 * On entry, we've already read the first tuple of the next group
 * (if there is one) into input->nextTupleSlot.  This invariant
 * is maintained on exit.
 */
static void
setop_load_group(SetOpStatePerInput *input, PlanState *inputPlan,
				 SetOpState *setopstate)
{
	input->needGroup = false;

	/* If we've exhausted this child plan, report an empty group */
	if (TupIsNull(input->nextTupleSlot))
	{
		ExecClearTuple(input->firstTupleSlot);
		input->numTuples = 0;
		return;
	}

	/* Make a local copy of the first tuple for comparisons */
	ExecStoreMinimalTuple(ExecCopySlotMinimalTuple(input->nextTupleSlot),
						  input->firstTupleSlot,
						  true);
	/* and count it */
	input->numTuples = 1;

	/* Scan till we find the end-of-group */
	for (;;)
	{
		int			cmpresult;

		/* Get next input tuple, if there is one */
		input->nextTupleSlot = ExecProcNode(inputPlan);
		if (TupIsNull(input->nextTupleSlot))
			break;

		/* There is; does it belong to same group as firstTuple? */
		cmpresult = setop_compare_slots(input->firstTupleSlot,
										input->nextTupleSlot,
										setopstate);
		Assert(cmpresult <= 0); /* else input is mis-sorted */
		if (cmpresult != 0)
			break;

		/* Still in same group, so count this tuple */
		input->numTuples++;
	}
}

/*
 * Compare the tuples in the two given slots.
 */
static int
setop_compare_slots(TupleTableSlot *s1, TupleTableSlot *s2,
					SetOpState *setopstate)
{
	/* We'll often need to fetch all the columns, so just do it */
	slot_getallattrs(s1);
	slot_getallattrs(s2);
	for (int nkey = 0; nkey < setopstate->numCols; nkey++)
	{
		SortSupport sortKey = setopstate->sortKeys + nkey;
		AttrNumber	attno = sortKey->ssup_attno;
		Datum		datum1 = s1->tts_values[attno - 1],
					datum2 = s2->tts_values[attno - 1];
		bool		isNull1 = s1->tts_isnull[attno - 1],
					isNull2 = s2->tts_isnull[attno - 1];
		int			compare;

		compare = ApplySortComparator(datum1, isNull1,
									  datum2, isNull2,
									  sortKey);
		if (compare != 0)
			return compare;
	}
	return 0;
}

/*
 * ExecSetOp for hashed case: phase 1, read inputs and build hash table
 */
static void
setop_fill_hash_table(SetOpState *setopstate)
{
	PlanState  *outerPlan;
	PlanState  *innerPlan;
	ExprContext *econtext = setopstate->ps.ps_ExprContext;
	bool		have_tuples = false;

	/*
	 * get state info from node
	 */
	outerPlan = outerPlanState(setopstate);
	innerPlan = innerPlanState(setopstate);

	/*
	 * Process each outer-plan tuple, and then fetch the next one, until we
	 * exhaust the outer plan.
	 */
	for (;;)
	{
		TupleTableSlot *outerslot;
		TupleHashEntryData *entry;
		bool		isnew;

		outerslot = ExecProcNode(outerPlan);
		if (TupIsNull(outerslot))
			break;
		have_tuples = true;

		/* Find or build hashtable entry for this tuple's group */
		entry = LookupTupleHashEntry(setopstate->hashtable,
									 outerslot,
									 &isnew, NULL);

		/* If new tuple group, initialize counts to zero */
		if (isnew)
		{
			entry->additional = (SetOpStatePerGroup)
				MemoryContextAllocZero(setopstate->hashtable->tablecxt,
									   sizeof(SetOpStatePerGroupData));
		}

		/* Advance the counts */
		((SetOpStatePerGroup) entry->additional)->numLeft++;

		/* Must reset expression context after each hashtable lookup */
		ResetExprContext(econtext);
	}

	/*
	 * If the outer relation is empty, then we will emit nothing, and we don't
	 * need to read the inner relation at all.
	 */
	if (have_tuples)
	{
		/*
		 * Process each inner-plan tuple, and then fetch the next one, until
		 * we exhaust the inner plan.
		 */
		for (;;)
		{
			TupleTableSlot *innerslot;
			TupleHashEntryData *entry;

			innerslot = ExecProcNode(innerPlan);
			if (TupIsNull(innerslot))
				break;

			/* For tuples not seen previously, do not make hashtable entry */
			entry = LookupTupleHashEntry(setopstate->hashtable,
										 innerslot,
										 NULL, NULL);

			/* Advance the counts if entry is already present */
			if (entry)
				((SetOpStatePerGroup) entry->additional)->numRight++;

			/* Must reset expression context after each hashtable lookup */
			ResetExprContext(econtext);
		}
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

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * create state structure
	 */
	setopstate = makeNode(SetOpState);
	setopstate->ps.plan = (Plan *) node;
	setopstate->ps.state = estate;
	setopstate->ps.ExecProcNode = ExecSetOp;

	setopstate->setop_done = false;
	setopstate->numOutput = 0;
	setopstate->numCols = node->numCols;
	setopstate->need_init = true;

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
	 * If we are hashing then the child plans do not need to handle REWIND
	 * efficiently; see ExecReScanSetOp.
	 */
	if (node->strategy == SETOP_HASHED)
		eflags &= ~EXEC_FLAG_REWIND;
	outerPlanState(setopstate) = ExecInitNode(outerPlan(node), estate, eflags);
	innerPlanState(setopstate) = ExecInitNode(innerPlan(node), estate, eflags);

	/*
	 * Initialize locally-allocated slots.  In hashed mode, we just need a
	 * result slot.  In sorted mode, we need one first-tuple-of-group slot for
	 * each input; we use the result slot for the left input's slot and create
	 * another for the right input.  (Note: the nextTupleSlot slots are not
	 * ours, but just point to the last slot returned by the input plan node.)
	 */
	ExecInitResultTupleSlotTL(&setopstate->ps, &TTSOpsMinimalTuple);
	if (node->strategy != SETOP_HASHED)
	{
		setopstate->leftInput.firstTupleSlot =
			setopstate->ps.ps_ResultTupleSlot;
		setopstate->rightInput.firstTupleSlot =
			ExecInitExtraTupleSlot(estate,
								   setopstate->ps.ps_ResultTupleDesc,
								   &TTSOpsMinimalTuple);
	}

	/* Setop nodes do no projections. */
	setopstate->ps.ps_ProjInfo = NULL;

	/*
	 * Precompute fmgr lookup data for inner loop.  We need equality and
	 * hashing functions to do it by hashing, while for sorting we need
	 * SortSupport data.
	 */
	if (node->strategy == SETOP_HASHED)
		execTuplesHashPrepare(node->numCols,
							  node->cmpOperators,
							  &setopstate->eqfuncoids,
							  &setopstate->hashfunctions);
	else
	{
		int			nkeys = node->numCols;

		setopstate->sortKeys = (SortSupport)
			palloc0(nkeys * sizeof(SortSupportData));
		for (int i = 0; i < nkeys; i++)
		{
			SortSupport sortKey = setopstate->sortKeys + i;

			sortKey->ssup_cxt = CurrentMemoryContext;
			sortKey->ssup_collation = node->cmpCollations[i];
			sortKey->ssup_nulls_first = node->cmpNullsFirst[i];
			sortKey->ssup_attno = node->cmpColIdx[i];
			/* abbreviated key conversion is not useful here */
			sortKey->abbreviate = false;

			PrepareSortSupportFromOrderingOp(node->cmpOperators[i], sortKey);
		}
	}

	/* Create a hash table if needed */
	if (node->strategy == SETOP_HASHED)
	{
		build_hash_table(setopstate);
		setopstate->table_filled = false;
	}

	return setopstate;
}

/* ----------------------------------------------------------------
 *		ExecEndSetOp
 *
 *		This shuts down the subplans and frees resources allocated
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
	ExecEndNode(innerPlanState(node));
}


void
ExecReScanSetOp(SetOpState *node)
{
	PlanState  *outerPlan = outerPlanState(node);
	PlanState  *innerPlan = innerPlanState(node);

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
		 * If we do have the hash table and the subplans do not have any
		 * parameter changes, then we can just rescan the existing hash table;
		 * no need to build it again.
		 */
		if (outerPlan->chgParam == NULL && innerPlan->chgParam == NULL)
		{
			ResetTupleHashIterator(node->hashtable, &node->hashiter);
			return;
		}

		/* Release any hashtable storage */
		if (node->tableContext)
			MemoryContextReset(node->tableContext);

		/* And rebuild an empty hashtable */
		ResetTupleHashTable(node->hashtable);
		node->table_filled = false;
	}
	else
	{
		/* Need to re-read first input from each side */
		node->need_init = true;
	}

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);
	if (innerPlan->chgParam == NULL)
		ExecReScan(innerPlan);
}
