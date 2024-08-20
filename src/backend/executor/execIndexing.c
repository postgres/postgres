/*-------------------------------------------------------------------------
 *
 * execIndexing.c
 *	  routines for inserting index tuples and enforcing unique and
 *	  exclusion constraints.
 *
 * ExecInsertIndexTuples() is the main entry point.  It's called after
 * inserting a tuple to the heap, and it inserts corresponding index tuples
 * into all indexes.  At the same time, it enforces any unique and
 * exclusion constraints:
 *
 * Unique Indexes
 * --------------
 *
 * Enforcing a unique constraint is straightforward.  When the index AM
 * inserts the tuple to the index, it also checks that there are no
 * conflicting tuples in the index already.  It does so atomically, so that
 * even if two backends try to insert the same key concurrently, only one
 * of them will succeed.  All the logic to ensure atomicity, and to wait
 * for in-progress transactions to finish, is handled by the index AM.
 *
 * If a unique constraint is deferred, we request the index AM to not
 * throw an error if a conflict is found.  Instead, we make note that there
 * was a conflict and return the list of indexes with conflicts to the
 * caller.  The caller must re-check them later, by calling index_insert()
 * with the UNIQUE_CHECK_EXISTING option.
 *
 * Exclusion Constraints
 * ---------------------
 *
 * Exclusion constraints are different from unique indexes in that when the
 * tuple is inserted to the index, the index AM does not check for
 * duplicate keys at the same time.  After the insertion, we perform a
 * separate scan on the index to check for conflicting tuples, and if one
 * is found, we throw an error and the transaction is aborted.  If the
 * conflicting tuple's inserter or deleter is in-progress, we wait for it
 * to finish first.
 *
 * There is a chance of deadlock, if two backends insert a tuple at the
 * same time, and then perform the scan to check for conflicts.  They will
 * find each other's tuple, and both try to wait for each other.  The
 * deadlock detector will detect that, and abort one of the transactions.
 * That's fairly harmless, as one of them was bound to abort with a
 * "duplicate key error" anyway, although you get a different error
 * message.
 *
 * If an exclusion constraint is deferred, we still perform the conflict
 * checking scan immediately after inserting the index tuple.  But instead
 * of throwing an error if a conflict is found, we return that information
 * to the caller.  The caller must re-check them later by calling
 * check_exclusion_constraint().
 *
 * Speculative insertion
 * ---------------------
 *
 * Speculative insertion is a two-phase mechanism used to implement
 * INSERT ... ON CONFLICT DO UPDATE/NOTHING.  The tuple is first inserted
 * to the heap and update the indexes as usual, but if a constraint is
 * violated, we can still back out the insertion without aborting the whole
 * transaction.  In an INSERT ... ON CONFLICT statement, if a conflict is
 * detected, the inserted tuple is backed out and the ON CONFLICT action is
 * executed instead.
 *
 * Insertion to a unique index works as usual: the index AM checks for
 * duplicate keys atomically with the insertion.  But instead of throwing
 * an error on a conflict, the speculatively inserted heap tuple is backed
 * out.
 *
 * Exclusion constraints are slightly more complicated.  As mentioned
 * earlier, there is a risk of deadlock when two backends insert the same
 * key concurrently.  That was not a problem for regular insertions, when
 * one of the transactions has to be aborted anyway, but with a speculative
 * insertion we cannot let a deadlock happen, because we only want to back
 * out the speculatively inserted tuple on conflict, not abort the whole
 * transaction.
 *
 * When a backend detects that the speculative insertion conflicts with
 * another in-progress tuple, it has two options:
 *
 * 1. back out the speculatively inserted tuple, then wait for the other
 *	  transaction, and retry. Or,
 * 2. wait for the other transaction, with the speculatively inserted tuple
 *	  still in place.
 *
 * If two backends insert at the same time, and both try to wait for each
 * other, they will deadlock.  So option 2 is not acceptable.  Option 1
 * avoids the deadlock, but it is prone to a livelock instead.  Both
 * transactions will wake up immediately as the other transaction backs
 * out.  Then they both retry, and conflict with each other again, lather,
 * rinse, repeat.
 *
 * To avoid the livelock, one of the backends must back out first, and then
 * wait, while the other one waits without backing out.  It doesn't matter
 * which one backs out, so we employ an arbitrary rule that the transaction
 * with the higher XID backs out.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/execIndexing.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "executor/executor.h"
#include "nodes/nodeFuncs.h"
#include "storage/lmgr.h"
#include "utils/snapmgr.h"

/* waitMode argument to check_exclusion_or_unique_constraint() */
typedef enum
{
	CEOUC_WAIT,
	CEOUC_NOWAIT,
	CEOUC_LIVELOCK_PREVENTING_WAIT,
} CEOUC_WAIT_MODE;

static bool check_exclusion_or_unique_constraint(Relation heap, Relation index,
												 IndexInfo *indexInfo,
												 ItemPointer tupleid,
												 const Datum *values, const bool *isnull,
												 EState *estate, bool newIndex,
												 CEOUC_WAIT_MODE waitMode,
												 bool violationOK,
												 ItemPointer conflictTid);

static bool index_recheck_constraint(Relation index, const Oid *constr_procs,
									 const Datum *existing_values, const bool *existing_isnull,
									 const Datum *new_values);
static bool index_unchanged_by_update(ResultRelInfo *resultRelInfo,
									  EState *estate, IndexInfo *indexInfo,
									  Relation indexRelation);
static bool index_expression_changed_walker(Node *node,
											Bitmapset *allUpdatedCols);

/* ----------------------------------------------------------------
 *		ExecOpenIndices
 *
 *		Find the indices associated with a result relation, open them,
 *		and save information about them in the result ResultRelInfo.
 *
 *		At entry, caller has already opened and locked
 *		resultRelInfo->ri_RelationDesc.
 * ----------------------------------------------------------------
 */
void
ExecOpenIndices(ResultRelInfo *resultRelInfo, bool speculative)
{
	Relation	resultRelation = resultRelInfo->ri_RelationDesc;
	List	   *indexoidlist;
	ListCell   *l;
	int			len,
				i;
	RelationPtr relationDescs;
	IndexInfo **indexInfoArray;

	resultRelInfo->ri_NumIndices = 0;

	/* fast path if no indexes */
	if (!RelationGetForm(resultRelation)->relhasindex)
		return;

	/*
	 * Get cached list of index OIDs
	 */
	indexoidlist = RelationGetIndexList(resultRelation);
	len = list_length(indexoidlist);
	if (len == 0)
		return;

	/*
	 * allocate space for result arrays
	 */
	relationDescs = (RelationPtr) palloc(len * sizeof(Relation));
	indexInfoArray = (IndexInfo **) palloc(len * sizeof(IndexInfo *));

	resultRelInfo->ri_NumIndices = len;
	resultRelInfo->ri_IndexRelationDescs = relationDescs;
	resultRelInfo->ri_IndexRelationInfo = indexInfoArray;

	/*
	 * For each index, open the index relation and save pg_index info. We
	 * acquire RowExclusiveLock, signifying we will update the index.
	 *
	 * Note: we do this even if the index is not indisready; it's not worth
	 * the trouble to optimize for the case where it isn't.
	 */
	i = 0;
	foreach(l, indexoidlist)
	{
		Oid			indexOid = lfirst_oid(l);
		Relation	indexDesc;
		IndexInfo  *ii;

		indexDesc = index_open(indexOid, RowExclusiveLock);

		/* extract index key information from the index's pg_index info */
		ii = BuildIndexInfo(indexDesc);

		/*
		 * If the indexes are to be used for speculative insertion or conflict
		 * detection in logical replication, add extra information required by
		 * unique index entries.
		 */
		if (speculative && ii->ii_Unique)
			BuildSpeculativeIndexInfo(indexDesc, ii);

		relationDescs[i] = indexDesc;
		indexInfoArray[i] = ii;
		i++;
	}

	list_free(indexoidlist);
}

/* ----------------------------------------------------------------
 *		ExecCloseIndices
 *
 *		Close the index relations stored in resultRelInfo
 * ----------------------------------------------------------------
 */
void
ExecCloseIndices(ResultRelInfo *resultRelInfo)
{
	int			i;
	int			numIndices;
	RelationPtr indexDescs;
	IndexInfo **indexInfos;

	numIndices = resultRelInfo->ri_NumIndices;
	indexDescs = resultRelInfo->ri_IndexRelationDescs;
	indexInfos = resultRelInfo->ri_IndexRelationInfo;

	for (i = 0; i < numIndices; i++)
	{
		if (indexDescs[i] == NULL)
			continue;			/* shouldn't happen? */

		/* Give the index a chance to do some post-insert cleanup */
		index_insert_cleanup(indexDescs[i], indexInfos[i]);

		/* Drop lock acquired by ExecOpenIndices */
		index_close(indexDescs[i], RowExclusiveLock);
	}

	/*
	 * XXX should free indexInfo array here too?  Currently we assume that
	 * such stuff will be cleaned up automatically in FreeExecutorState.
	 */
}

/* ----------------------------------------------------------------
 *		ExecInsertIndexTuples
 *
 *		This routine takes care of inserting index tuples
 *		into all the relations indexing the result relation
 *		when a heap tuple is inserted into the result relation.
 *
 *		When 'update' is true and 'onlySummarizing' is false,
 *		executor is performing an UPDATE that could not use an
 *		optimization like heapam's HOT (in more general terms a
 *		call to table_tuple_update() took place and set
 *		'update_indexes' to TUUI_All).  Receiving this hint makes
 *		us consider if we should pass down the 'indexUnchanged'
 *		hint in turn.  That's something that we figure out for
 *		each index_insert() call iff 'update' is true.
 *		(When 'update' is false we already know not to pass the
 *		hint to any index.)
 *
 *		If onlySummarizing is set, an equivalent optimization to
 *		HOT has been applied and any updated columns are indexed
 *		only by summarizing indexes (or in more general terms a
 *		call to table_tuple_update() took place and set
 *		'update_indexes' to TUUI_Summarizing). We can (and must)
 *		therefore only update the indexes that have
 *		'amsummarizing' = true.
 *
 *		Unique and exclusion constraints are enforced at the same
 *		time.  This returns a list of index OIDs for any unique or
 *		exclusion constraints that are deferred and that had
 *		potential (unconfirmed) conflicts.  (if noDupErr == true,
 *		the same is done for non-deferred constraints, but report
 *		if conflict was speculative or deferred conflict to caller)
 *
 *		If 'arbiterIndexes' is nonempty, noDupErr applies only to
 *		those indexes.  NIL means noDupErr applies to all indexes.
 * ----------------------------------------------------------------
 */
List *
ExecInsertIndexTuples(ResultRelInfo *resultRelInfo,
					  TupleTableSlot *slot,
					  EState *estate,
					  bool update,
					  bool noDupErr,
					  bool *specConflict,
					  List *arbiterIndexes,
					  bool onlySummarizing)
{
	ItemPointer tupleid = &slot->tts_tid;
	List	   *result = NIL;
	int			i;
	int			numIndices;
	RelationPtr relationDescs;
	Relation	heapRelation;
	IndexInfo **indexInfoArray;
	ExprContext *econtext;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];

	Assert(ItemPointerIsValid(tupleid));

	/*
	 * Get information from the result relation info structure.
	 */
	numIndices = resultRelInfo->ri_NumIndices;
	relationDescs = resultRelInfo->ri_IndexRelationDescs;
	indexInfoArray = resultRelInfo->ri_IndexRelationInfo;
	heapRelation = resultRelInfo->ri_RelationDesc;

	/* Sanity check: slot must belong to the same rel as the resultRelInfo. */
	Assert(slot->tts_tableOid == RelationGetRelid(heapRelation));

	/*
	 * We will use the EState's per-tuple context for evaluating predicates
	 * and index expressions (creating it if it's not already there).
	 */
	econtext = GetPerTupleExprContext(estate);

	/* Arrange for econtext's scan tuple to be the tuple under test */
	econtext->ecxt_scantuple = slot;

	/*
	 * for each index, form and insert the index tuple
	 */
	for (i = 0; i < numIndices; i++)
	{
		Relation	indexRelation = relationDescs[i];
		IndexInfo  *indexInfo;
		bool		applyNoDupErr;
		IndexUniqueCheck checkUnique;
		bool		indexUnchanged;
		bool		satisfiesConstraint;

		if (indexRelation == NULL)
			continue;

		indexInfo = indexInfoArray[i];

		/* If the index is marked as read-only, ignore it */
		if (!indexInfo->ii_ReadyForInserts)
			continue;

		/*
		 * Skip processing of non-summarizing indexes if we only update
		 * summarizing indexes
		 */
		if (onlySummarizing && !indexInfo->ii_Summarizing)
			continue;

		/* Check for partial index */
		if (indexInfo->ii_Predicate != NIL)
		{
			ExprState  *predicate;

			/*
			 * If predicate state not set up yet, create it (in the estate's
			 * per-query context)
			 */
			predicate = indexInfo->ii_PredicateState;
			if (predicate == NULL)
			{
				predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);
				indexInfo->ii_PredicateState = predicate;
			}

			/* Skip this index-update if the predicate isn't satisfied */
			if (!ExecQual(predicate, econtext))
				continue;
		}

		/*
		 * FormIndexDatum fills in its values and isnull parameters with the
		 * appropriate values for the column(s) of the index.
		 */
		FormIndexDatum(indexInfo,
					   slot,
					   estate,
					   values,
					   isnull);

		/* Check whether to apply noDupErr to this index */
		applyNoDupErr = noDupErr &&
			(arbiterIndexes == NIL ||
			 list_member_oid(arbiterIndexes,
							 indexRelation->rd_index->indexrelid));

		/*
		 * The index AM does the actual insertion, plus uniqueness checking.
		 *
		 * For an immediate-mode unique index, we just tell the index AM to
		 * throw error if not unique.
		 *
		 * For a deferrable unique index, we tell the index AM to just detect
		 * possible non-uniqueness, and we add the index OID to the result
		 * list if further checking is needed.
		 *
		 * For a speculative insertion (used by INSERT ... ON CONFLICT), do
		 * the same as for a deferrable unique index.
		 */
		if (!indexRelation->rd_index->indisunique)
			checkUnique = UNIQUE_CHECK_NO;
		else if (applyNoDupErr)
			checkUnique = UNIQUE_CHECK_PARTIAL;
		else if (indexRelation->rd_index->indimmediate)
			checkUnique = UNIQUE_CHECK_YES;
		else
			checkUnique = UNIQUE_CHECK_PARTIAL;

		/*
		 * There's definitely going to be an index_insert() call for this
		 * index.  If we're being called as part of an UPDATE statement,
		 * consider if the 'indexUnchanged' = true hint should be passed.
		 */
		indexUnchanged = update && index_unchanged_by_update(resultRelInfo,
															 estate,
															 indexInfo,
															 indexRelation);

		satisfiesConstraint =
			index_insert(indexRelation, /* index relation */
						 values,	/* array of index Datums */
						 isnull,	/* null flags */
						 tupleid,	/* tid of heap tuple */
						 heapRelation,	/* heap relation */
						 checkUnique,	/* type of uniqueness check to do */
						 indexUnchanged,	/* UPDATE without logical change? */
						 indexInfo);	/* index AM may need this */

		/*
		 * If the index has an associated exclusion constraint, check that.
		 * This is simpler than the process for uniqueness checks since we
		 * always insert first and then check.  If the constraint is deferred,
		 * we check now anyway, but don't throw error on violation or wait for
		 * a conclusive outcome from a concurrent insertion; instead we'll
		 * queue a recheck event.  Similarly, noDupErr callers (speculative
		 * inserters) will recheck later, and wait for a conclusive outcome
		 * then.
		 *
		 * An index for an exclusion constraint can't also be UNIQUE (not an
		 * essential property, we just don't allow it in the grammar), so no
		 * need to preserve the prior state of satisfiesConstraint.
		 */
		if (indexInfo->ii_ExclusionOps != NULL)
		{
			bool		violationOK;
			CEOUC_WAIT_MODE waitMode;

			if (applyNoDupErr)
			{
				violationOK = true;
				waitMode = CEOUC_LIVELOCK_PREVENTING_WAIT;
			}
			else if (!indexRelation->rd_index->indimmediate)
			{
				violationOK = true;
				waitMode = CEOUC_NOWAIT;
			}
			else
			{
				violationOK = false;
				waitMode = CEOUC_WAIT;
			}

			satisfiesConstraint =
				check_exclusion_or_unique_constraint(heapRelation,
													 indexRelation, indexInfo,
													 tupleid, values, isnull,
													 estate, false,
													 waitMode, violationOK, NULL);
		}

		if ((checkUnique == UNIQUE_CHECK_PARTIAL ||
			 indexInfo->ii_ExclusionOps != NULL) &&
			!satisfiesConstraint)
		{
			/*
			 * The tuple potentially violates the uniqueness or exclusion
			 * constraint, so make a note of the index so that we can re-check
			 * it later.  Speculative inserters are told if there was a
			 * speculative conflict, since that always requires a restart.
			 */
			result = lappend_oid(result, RelationGetRelid(indexRelation));
			if (indexRelation->rd_index->indimmediate && specConflict)
				*specConflict = true;
		}
	}

	return result;
}

/* ----------------------------------------------------------------
 *		ExecCheckIndexConstraints
 *
 *		This routine checks if a tuple violates any unique or
 *		exclusion constraints.  Returns true if there is no conflict.
 *		Otherwise returns false, and the TID of the conflicting
 *		tuple is returned in *conflictTid.
 *
 *		If 'arbiterIndexes' is given, only those indexes are checked.
 *		NIL means all indexes.
 *
 *		Note that this doesn't lock the values in any way, so it's
 *		possible that a conflicting tuple is inserted immediately
 *		after this returns.  This can be used for either a pre-check
 *		before insertion or a re-check after finding a conflict.
 *
 *		'tupleid' should be the TID of the tuple that has been recently
 *		inserted (or can be invalid if we haven't inserted a new tuple yet).
 *		This tuple will be excluded from conflict checking.
 * ----------------------------------------------------------------
 */
bool
ExecCheckIndexConstraints(ResultRelInfo *resultRelInfo, TupleTableSlot *slot,
						  EState *estate, ItemPointer conflictTid,
						  ItemPointer tupleid, List *arbiterIndexes)
{
	int			i;
	int			numIndices;
	RelationPtr relationDescs;
	Relation	heapRelation;
	IndexInfo **indexInfoArray;
	ExprContext *econtext;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	ItemPointerData invalidItemPtr;
	bool		checkedIndex = false;

	ItemPointerSetInvalid(conflictTid);
	ItemPointerSetInvalid(&invalidItemPtr);

	/*
	 * Get information from the result relation info structure.
	 */
	numIndices = resultRelInfo->ri_NumIndices;
	relationDescs = resultRelInfo->ri_IndexRelationDescs;
	indexInfoArray = resultRelInfo->ri_IndexRelationInfo;
	heapRelation = resultRelInfo->ri_RelationDesc;

	/*
	 * We will use the EState's per-tuple context for evaluating predicates
	 * and index expressions (creating it if it's not already there).
	 */
	econtext = GetPerTupleExprContext(estate);

	/* Arrange for econtext's scan tuple to be the tuple under test */
	econtext->ecxt_scantuple = slot;

	/*
	 * For each index, form index tuple and check if it satisfies the
	 * constraint.
	 */
	for (i = 0; i < numIndices; i++)
	{
		Relation	indexRelation = relationDescs[i];
		IndexInfo  *indexInfo;
		bool		satisfiesConstraint;

		if (indexRelation == NULL)
			continue;

		indexInfo = indexInfoArray[i];

		if (!indexInfo->ii_Unique && !indexInfo->ii_ExclusionOps)
			continue;

		/* If the index is marked as read-only, ignore it */
		if (!indexInfo->ii_ReadyForInserts)
			continue;

		/* When specific arbiter indexes requested, only examine them */
		if (arbiterIndexes != NIL &&
			!list_member_oid(arbiterIndexes,
							 indexRelation->rd_index->indexrelid))
			continue;

		if (!indexRelation->rd_index->indimmediate)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("ON CONFLICT does not support deferrable unique constraints/exclusion constraints as arbiters"),
					 errtableconstraint(heapRelation,
										RelationGetRelationName(indexRelation))));

		checkedIndex = true;

		/* Check for partial index */
		if (indexInfo->ii_Predicate != NIL)
		{
			ExprState  *predicate;

			/*
			 * If predicate state not set up yet, create it (in the estate's
			 * per-query context)
			 */
			predicate = indexInfo->ii_PredicateState;
			if (predicate == NULL)
			{
				predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);
				indexInfo->ii_PredicateState = predicate;
			}

			/* Skip this index-update if the predicate isn't satisfied */
			if (!ExecQual(predicate, econtext))
				continue;
		}

		/*
		 * FormIndexDatum fills in its values and isnull parameters with the
		 * appropriate values for the column(s) of the index.
		 */
		FormIndexDatum(indexInfo,
					   slot,
					   estate,
					   values,
					   isnull);

		satisfiesConstraint =
			check_exclusion_or_unique_constraint(heapRelation, indexRelation,
												 indexInfo, tupleid,
												 values, isnull, estate, false,
												 CEOUC_WAIT, true,
												 conflictTid);
		if (!satisfiesConstraint)
			return false;
	}

	if (arbiterIndexes != NIL && !checkedIndex)
		elog(ERROR, "unexpected failure to find arbiter index");

	return true;
}

/*
 * Check for violation of an exclusion or unique constraint
 *
 * heap: the table containing the new tuple
 * index: the index supporting the constraint
 * indexInfo: info about the index, including the exclusion properties
 * tupleid: heap TID of the new tuple we have just inserted (invalid if we
 *		haven't inserted a new tuple yet)
 * values, isnull: the *index* column values computed for the new tuple
 * estate: an EState we can do evaluation in
 * newIndex: if true, we are trying to build a new index (this affects
 *		only the wording of error messages)
 * waitMode: whether to wait for concurrent inserters/deleters
 * violationOK: if true, don't throw error for violation
 * conflictTid: if not-NULL, the TID of the conflicting tuple is returned here
 *
 * Returns true if OK, false if actual or potential violation
 *
 * 'waitMode' determines what happens if a conflict is detected with a tuple
 * that was inserted or deleted by a transaction that's still running.
 * CEOUC_WAIT means that we wait for the transaction to commit, before
 * throwing an error or returning.  CEOUC_NOWAIT means that we report the
 * violation immediately; so the violation is only potential, and the caller
 * must recheck sometime later.  This behavior is convenient for deferred
 * exclusion checks; we need not bother queuing a deferred event if there is
 * definitely no conflict at insertion time.
 *
 * CEOUC_LIVELOCK_PREVENTING_WAIT is like CEOUC_NOWAIT, but we will sometimes
 * wait anyway, to prevent livelocking if two transactions try inserting at
 * the same time.  This is used with speculative insertions, for INSERT ON
 * CONFLICT statements. (See notes in file header)
 *
 * If violationOK is true, we just report the potential or actual violation to
 * the caller by returning 'false'.  Otherwise we throw a descriptive error
 * message here.  When violationOK is false, a false result is impossible.
 *
 * Note: The indexam is normally responsible for checking unique constraints,
 * so this normally only needs to be used for exclusion constraints.  But this
 * function is also called when doing a "pre-check" for conflicts on a unique
 * constraint, when doing speculative insertion.  Caller may use the returned
 * conflict TID to take further steps.
 */
static bool
check_exclusion_or_unique_constraint(Relation heap, Relation index,
									 IndexInfo *indexInfo,
									 ItemPointer tupleid,
									 const Datum *values, const bool *isnull,
									 EState *estate, bool newIndex,
									 CEOUC_WAIT_MODE waitMode,
									 bool violationOK,
									 ItemPointer conflictTid)
{
	Oid		   *constr_procs;
	uint16	   *constr_strats;
	Oid		   *index_collations = index->rd_indcollation;
	int			indnkeyatts = IndexRelationGetNumberOfKeyAttributes(index);
	IndexScanDesc index_scan;
	ScanKeyData scankeys[INDEX_MAX_KEYS];
	SnapshotData DirtySnapshot;
	int			i;
	bool		conflict;
	bool		found_self;
	ExprContext *econtext;
	TupleTableSlot *existing_slot;
	TupleTableSlot *save_scantuple;

	if (indexInfo->ii_ExclusionOps)
	{
		constr_procs = indexInfo->ii_ExclusionProcs;
		constr_strats = indexInfo->ii_ExclusionStrats;
	}
	else
	{
		constr_procs = indexInfo->ii_UniqueProcs;
		constr_strats = indexInfo->ii_UniqueStrats;
	}

	/*
	 * If any of the input values are NULL, and the index uses the default
	 * nulls-are-distinct mode, the constraint check is assumed to pass (i.e.,
	 * we assume the operators are strict).  Otherwise, we interpret the
	 * constraint as specifying IS NULL for each column whose input value is
	 * NULL.
	 */
	if (!indexInfo->ii_NullsNotDistinct)
	{
		for (i = 0; i < indnkeyatts; i++)
		{
			if (isnull[i])
				return true;
		}
	}

	/*
	 * Search the tuples that are in the index for any violations, including
	 * tuples that aren't visible yet.
	 */
	InitDirtySnapshot(DirtySnapshot);

	for (i = 0; i < indnkeyatts; i++)
	{
		ScanKeyEntryInitialize(&scankeys[i],
							   isnull[i] ? SK_ISNULL | SK_SEARCHNULL : 0,
							   i + 1,
							   constr_strats[i],
							   InvalidOid,
							   index_collations[i],
							   constr_procs[i],
							   values[i]);
	}

	/*
	 * Need a TupleTableSlot to put existing tuples in.
	 *
	 * To use FormIndexDatum, we have to make the econtext's scantuple point
	 * to this slot.  Be sure to save and restore caller's value for
	 * scantuple.
	 */
	existing_slot = table_slot_create(heap, NULL);

	econtext = GetPerTupleExprContext(estate);
	save_scantuple = econtext->ecxt_scantuple;
	econtext->ecxt_scantuple = existing_slot;

	/*
	 * May have to restart scan from this point if a potential conflict is
	 * found.
	 */
retry:
	conflict = false;
	found_self = false;
	index_scan = index_beginscan(heap, index, &DirtySnapshot, indnkeyatts, 0);
	index_rescan(index_scan, scankeys, indnkeyatts, NULL, 0);

	while (index_getnext_slot(index_scan, ForwardScanDirection, existing_slot))
	{
		TransactionId xwait;
		XLTW_Oper	reason_wait;
		Datum		existing_values[INDEX_MAX_KEYS];
		bool		existing_isnull[INDEX_MAX_KEYS];
		char	   *error_new;
		char	   *error_existing;

		/*
		 * Ignore the entry for the tuple we're trying to check.
		 */
		if (ItemPointerIsValid(tupleid) &&
			ItemPointerEquals(tupleid, &existing_slot->tts_tid))
		{
			if (found_self)		/* should not happen */
				elog(ERROR, "found self tuple multiple times in index \"%s\"",
					 RelationGetRelationName(index));
			found_self = true;
			continue;
		}

		/*
		 * Extract the index column values and isnull flags from the existing
		 * tuple.
		 */
		FormIndexDatum(indexInfo, existing_slot, estate,
					   existing_values, existing_isnull);

		/* If lossy indexscan, must recheck the condition */
		if (index_scan->xs_recheck)
		{
			if (!index_recheck_constraint(index,
										  constr_procs,
										  existing_values,
										  existing_isnull,
										  values))
				continue;		/* tuple doesn't actually match, so no
								 * conflict */
		}

		/*
		 * At this point we have either a conflict or a potential conflict.
		 *
		 * If an in-progress transaction is affecting the visibility of this
		 * tuple, we need to wait for it to complete and then recheck (unless
		 * the caller requested not to).  For simplicity we do rechecking by
		 * just restarting the whole scan --- this case probably doesn't
		 * happen often enough to be worth trying harder, and anyway we don't
		 * want to hold any index internal locks while waiting.
		 */
		xwait = TransactionIdIsValid(DirtySnapshot.xmin) ?
			DirtySnapshot.xmin : DirtySnapshot.xmax;

		if (TransactionIdIsValid(xwait) &&
			(waitMode == CEOUC_WAIT ||
			 (waitMode == CEOUC_LIVELOCK_PREVENTING_WAIT &&
			  DirtySnapshot.speculativeToken &&
			  TransactionIdPrecedes(GetCurrentTransactionId(), xwait))))
		{
			reason_wait = indexInfo->ii_ExclusionOps ?
				XLTW_RecheckExclusionConstr : XLTW_InsertIndex;
			index_endscan(index_scan);
			if (DirtySnapshot.speculativeToken)
				SpeculativeInsertionWait(DirtySnapshot.xmin,
										 DirtySnapshot.speculativeToken);
			else
				XactLockTableWait(xwait, heap,
								  &existing_slot->tts_tid, reason_wait);
			goto retry;
		}

		/*
		 * We have a definite conflict (or a potential one, but the caller
		 * didn't want to wait).  Return it to caller, or report it.
		 */
		if (violationOK)
		{
			conflict = true;
			if (conflictTid)
				*conflictTid = existing_slot->tts_tid;
			break;
		}

		error_new = BuildIndexValueDescription(index, values, isnull);
		error_existing = BuildIndexValueDescription(index, existing_values,
													existing_isnull);
		if (newIndex)
			ereport(ERROR,
					(errcode(ERRCODE_EXCLUSION_VIOLATION),
					 errmsg("could not create exclusion constraint \"%s\"",
							RelationGetRelationName(index)),
					 error_new && error_existing ?
					 errdetail("Key %s conflicts with key %s.",
							   error_new, error_existing) :
					 errdetail("Key conflicts exist."),
					 errtableconstraint(heap,
										RelationGetRelationName(index))));
		else
			ereport(ERROR,
					(errcode(ERRCODE_EXCLUSION_VIOLATION),
					 errmsg("conflicting key value violates exclusion constraint \"%s\"",
							RelationGetRelationName(index)),
					 error_new && error_existing ?
					 errdetail("Key %s conflicts with existing key %s.",
							   error_new, error_existing) :
					 errdetail("Key conflicts with existing key."),
					 errtableconstraint(heap,
										RelationGetRelationName(index))));
	}

	index_endscan(index_scan);

	/*
	 * Ordinarily, at this point the search should have found the originally
	 * inserted tuple (if any), unless we exited the loop early because of
	 * conflict.  However, it is possible to define exclusion constraints for
	 * which that wouldn't be true --- for instance, if the operator is <>. So
	 * we no longer complain if found_self is still false.
	 */

	econtext->ecxt_scantuple = save_scantuple;

	ExecDropSingleTupleTableSlot(existing_slot);

	return !conflict;
}

/*
 * Check for violation of an exclusion constraint
 *
 * This is a dumbed down version of check_exclusion_or_unique_constraint
 * for external callers. They don't need all the special modes.
 */
void
check_exclusion_constraint(Relation heap, Relation index,
						   IndexInfo *indexInfo,
						   ItemPointer tupleid,
						   const Datum *values, const bool *isnull,
						   EState *estate, bool newIndex)
{
	(void) check_exclusion_or_unique_constraint(heap, index, indexInfo, tupleid,
												values, isnull,
												estate, newIndex,
												CEOUC_WAIT, false, NULL);
}

/*
 * Check existing tuple's index values to see if it really matches the
 * exclusion condition against the new_values.  Returns true if conflict.
 */
static bool
index_recheck_constraint(Relation index, const Oid *constr_procs,
						 const Datum *existing_values, const bool *existing_isnull,
						 const Datum *new_values)
{
	int			indnkeyatts = IndexRelationGetNumberOfKeyAttributes(index);
	int			i;

	for (i = 0; i < indnkeyatts; i++)
	{
		/* Assume the exclusion operators are strict */
		if (existing_isnull[i])
			return false;

		if (!DatumGetBool(OidFunctionCall2Coll(constr_procs[i],
											   index->rd_indcollation[i],
											   existing_values[i],
											   new_values[i])))
			return false;
	}

	return true;
}

/*
 * Check if ExecInsertIndexTuples() should pass indexUnchanged hint.
 *
 * When the executor performs an UPDATE that requires a new round of index
 * tuples, determine if we should pass 'indexUnchanged' = true hint for one
 * single index.
 */
static bool
index_unchanged_by_update(ResultRelInfo *resultRelInfo, EState *estate,
						  IndexInfo *indexInfo, Relation indexRelation)
{
	Bitmapset  *updatedCols;
	Bitmapset  *extraUpdatedCols;
	Bitmapset  *allUpdatedCols;
	bool		hasexpression = false;
	List	   *idxExprs;

	/*
	 * Check cache first
	 */
	if (indexInfo->ii_CheckedUnchanged)
		return indexInfo->ii_IndexUnchanged;
	indexInfo->ii_CheckedUnchanged = true;

	/*
	 * Check for indexed attribute overlap with updated columns.
	 *
	 * Only do this for key columns.  A change to a non-key column within an
	 * INCLUDE index should not be counted here.  Non-key column values are
	 * opaque payload state to the index AM, a little like an extra table TID.
	 *
	 * Note that row-level BEFORE triggers won't affect our behavior, since
	 * they don't affect the updatedCols bitmaps generally.  It doesn't seem
	 * worth the trouble of checking which attributes were changed directly.
	 */
	updatedCols = ExecGetUpdatedCols(resultRelInfo, estate);
	extraUpdatedCols = ExecGetExtraUpdatedCols(resultRelInfo, estate);
	for (int attr = 0; attr < indexInfo->ii_NumIndexKeyAttrs; attr++)
	{
		int			keycol = indexInfo->ii_IndexAttrNumbers[attr];

		if (keycol <= 0)
		{
			/*
			 * Skip expressions for now, but remember to deal with them later
			 * on
			 */
			hasexpression = true;
			continue;
		}

		if (bms_is_member(keycol - FirstLowInvalidHeapAttributeNumber,
						  updatedCols) ||
			bms_is_member(keycol - FirstLowInvalidHeapAttributeNumber,
						  extraUpdatedCols))
		{
			/* Changed key column -- don't hint for this index */
			indexInfo->ii_IndexUnchanged = false;
			return false;
		}
	}

	/*
	 * When we get this far and index has no expressions, return true so that
	 * index_insert() call will go on to pass 'indexUnchanged' = true hint.
	 *
	 * The _absence_ of an indexed key attribute that overlaps with updated
	 * attributes (in addition to the total absence of indexed expressions)
	 * shows that the index as a whole is logically unchanged by UPDATE.
	 */
	if (!hasexpression)
	{
		indexInfo->ii_IndexUnchanged = true;
		return true;
	}

	/*
	 * Need to pass only one bms to expression_tree_walker helper function.
	 * Avoid allocating memory in common case where there are no extra cols.
	 */
	if (!extraUpdatedCols)
		allUpdatedCols = updatedCols;
	else
		allUpdatedCols = bms_union(updatedCols, extraUpdatedCols);

	/*
	 * We have to work slightly harder in the event of indexed expressions,
	 * but the principle is the same as before: try to find columns (Vars,
	 * actually) that overlap with known-updated columns.
	 *
	 * If we find any matching Vars, don't pass hint for index.  Otherwise
	 * pass hint.
	 */
	idxExprs = RelationGetIndexExpressions(indexRelation);
	hasexpression = index_expression_changed_walker((Node *) idxExprs,
													allUpdatedCols);
	list_free(idxExprs);
	if (extraUpdatedCols)
		bms_free(allUpdatedCols);

	if (hasexpression)
	{
		indexInfo->ii_IndexUnchanged = false;
		return false;
	}

	/*
	 * Deliberately don't consider index predicates.  We should even give the
	 * hint when result rel's "updated tuple" has no corresponding index
	 * tuple, which is possible with a partial index (provided the usual
	 * conditions are met).
	 */
	indexInfo->ii_IndexUnchanged = true;
	return true;
}

/*
 * Indexed expression helper for index_unchanged_by_update().
 *
 * Returns true when Var that appears within allUpdatedCols located.
 */
static bool
index_expression_changed_walker(Node *node, Bitmapset *allUpdatedCols)
{
	if (node == NULL)
		return false;

	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (bms_is_member(var->varattno - FirstLowInvalidHeapAttributeNumber,
						  allUpdatedCols))
		{
			/* Var was updated -- indicates that we should not hint */
			return true;
		}

		/* Still haven't found a reason to not pass the hint */
		return false;
	}

	return expression_tree_walker(node, index_expression_changed_walker,
								  (void *) allUpdatedCols);
}
