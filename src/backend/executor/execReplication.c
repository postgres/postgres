/*-------------------------------------------------------------------------
 *
 * execReplication.c
 *	  miscellaneous executor routines for logical replication
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/execReplication.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/gist.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/pg_am_d.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "executor/nodeModifyTable.h"
#include "replication/conflict.h"
#include "replication/logicalrelation.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


static bool tuples_equal(TupleTableSlot *slot1, TupleTableSlot *slot2,
						 TypeCacheEntry **eq);

/*
 * Setup a ScanKey for a search in the relation 'rel' for a tuple 'key' that
 * is setup to match 'rel' (*NOT* idxrel!).
 *
 * Returns how many columns to use for the index scan.
 *
 * This is not generic routine, idxrel must be PK, RI, or an index that can be
 * used for REPLICA IDENTITY FULL table. See FindUsableIndexForReplicaIdentityFull()
 * for details.
 *
 * By definition, replication identity of a rel meets all limitations associated
 * with that. Note that any other index could also meet these limitations.
 */
static int
build_replindex_scan_key(ScanKey skey, Relation rel, Relation idxrel,
						 TupleTableSlot *searchslot)
{
	int			index_attoff;
	int			skey_attoff = 0;
	Datum		indclassDatum;
	oidvector  *opclass;
	int2vector *indkey = &idxrel->rd_index->indkey;

	indclassDatum = SysCacheGetAttrNotNull(INDEXRELID, idxrel->rd_indextuple,
										   Anum_pg_index_indclass);
	opclass = (oidvector *) DatumGetPointer(indclassDatum);

	/* Build scankey for every non-expression attribute in the index. */
	for (index_attoff = 0; index_attoff < IndexRelationGetNumberOfKeyAttributes(idxrel);
		 index_attoff++)
	{
		Oid			operator;
		Oid			optype;
		Oid			opfamily;
		RegProcedure regop;
		int			table_attno = indkey->values[index_attoff];
		StrategyNumber eq_strategy;

		if (!AttributeNumberIsValid(table_attno))
		{
			/*
			 * XXX: Currently, we don't support expressions in the scan key,
			 * see code below.
			 */
			continue;
		}

		/*
		 * Load the operator info.  We need this to get the equality operator
		 * function for the scan key.
		 */
		optype = get_opclass_input_type(opclass->values[index_attoff]);
		opfamily = get_opclass_family(opclass->values[index_attoff]);
		eq_strategy = IndexAmTranslateCompareType(COMPARE_EQ, idxrel->rd_rel->relam, opfamily, false);
		operator = get_opfamily_member(opfamily, optype,
									   optype,
									   eq_strategy);

		if (!OidIsValid(operator))
			elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
				 eq_strategy, optype, optype, opfamily);

		regop = get_opcode(operator);

		/* Initialize the scankey. */
		ScanKeyInit(&skey[skey_attoff],
					index_attoff + 1,
					eq_strategy,
					regop,
					searchslot->tts_values[table_attno - 1]);

		skey[skey_attoff].sk_collation = idxrel->rd_indcollation[index_attoff];

		/* Check for null value. */
		if (searchslot->tts_isnull[table_attno - 1])
			skey[skey_attoff].sk_flags |= (SK_ISNULL | SK_SEARCHNULL);

		skey_attoff++;
	}

	/* There must always be at least one attribute for the index scan. */
	Assert(skey_attoff > 0);

	return skey_attoff;
}


/*
 * Helper function to check if it is necessary to re-fetch and lock the tuple
 * due to concurrent modifications. This function should be called after
 * invoking table_tuple_lock.
 */
static bool
should_refetch_tuple(TM_Result res, TM_FailureData *tmfd)
{
	bool		refetch = false;

	switch (res)
	{
		case TM_Ok:
			break;
		case TM_Updated:
			/* XXX: Improve handling here */
			if (ItemPointerIndicatesMovedPartitions(&tmfd->ctid))
				ereport(LOG,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("tuple to be locked was already moved to another partition due to concurrent update, retrying")));
			else
				ereport(LOG,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("concurrent update, retrying")));
			refetch = true;
			break;
		case TM_Deleted:
			/* XXX: Improve handling here */
			ereport(LOG,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("concurrent delete, retrying")));
			refetch = true;
			break;
		case TM_Invisible:
			elog(ERROR, "attempted to lock invisible tuple");
			break;
		default:
			elog(ERROR, "unexpected table_tuple_lock status: %u", res);
			break;
	}

	return refetch;
}

/*
 * Search the relation 'rel' for tuple using the index.
 *
 * If a matching tuple is found, lock it with lockmode, fill the slot with its
 * contents, and return true.  Return false otherwise.
 */
bool
RelationFindReplTupleByIndex(Relation rel, Oid idxoid,
							 LockTupleMode lockmode,
							 TupleTableSlot *searchslot,
							 TupleTableSlot *outslot)
{
	ScanKeyData skey[INDEX_MAX_KEYS];
	int			skey_attoff;
	IndexScanDesc scan;
	SnapshotData snap;
	TransactionId xwait;
	Relation	idxrel;
	bool		found;
	TypeCacheEntry **eq = NULL;
	bool		isIdxSafeToSkipDuplicates;

	/* Open the index. */
	idxrel = index_open(idxoid, RowExclusiveLock);

	isIdxSafeToSkipDuplicates = (GetRelationIdentityOrPK(rel) == idxoid);

	InitDirtySnapshot(snap);

	/* Build scan key. */
	skey_attoff = build_replindex_scan_key(skey, rel, idxrel, searchslot);

	/* Start an index scan. */
	scan = index_beginscan(rel, idxrel, &snap, NULL, skey_attoff, 0);

retry:
	found = false;

	index_rescan(scan, skey, skey_attoff, NULL, 0);

	/* Try to find the tuple */
	while (index_getnext_slot(scan, ForwardScanDirection, outslot))
	{
		/*
		 * Avoid expensive equality check if the index is primary key or
		 * replica identity index.
		 */
		if (!isIdxSafeToSkipDuplicates)
		{
			if (eq == NULL)
				eq = palloc0(sizeof(*eq) * outslot->tts_tupleDescriptor->natts);

			if (!tuples_equal(outslot, searchslot, eq))
				continue;
		}

		ExecMaterializeSlot(outslot);

		xwait = TransactionIdIsValid(snap.xmin) ?
			snap.xmin : snap.xmax;

		/*
		 * If the tuple is locked, wait for locking transaction to finish and
		 * retry.
		 */
		if (TransactionIdIsValid(xwait))
		{
			XactLockTableWait(xwait, NULL, NULL, XLTW_None);
			goto retry;
		}

		/* Found our tuple and it's not locked */
		found = true;
		break;
	}

	/* Found tuple, try to lock it in the lockmode. */
	if (found)
	{
		TM_FailureData tmfd;
		TM_Result	res;

		PushActiveSnapshot(GetLatestSnapshot());

		res = table_tuple_lock(rel, &(outslot->tts_tid), GetActiveSnapshot(),
							   outslot,
							   GetCurrentCommandId(false),
							   lockmode,
							   LockWaitBlock,
							   0 /* don't follow updates */ ,
							   &tmfd);

		PopActiveSnapshot();

		if (should_refetch_tuple(res, &tmfd))
			goto retry;
	}

	index_endscan(scan);

	/* Don't release lock until commit. */
	index_close(idxrel, NoLock);

	return found;
}

/*
 * Compare the tuples in the slots by checking if they have equal values.
 */
static bool
tuples_equal(TupleTableSlot *slot1, TupleTableSlot *slot2,
			 TypeCacheEntry **eq)
{
	int			attrnum;

	Assert(slot1->tts_tupleDescriptor->natts ==
		   slot2->tts_tupleDescriptor->natts);

	slot_getallattrs(slot1);
	slot_getallattrs(slot2);

	/* Check equality of the attributes. */
	for (attrnum = 0; attrnum < slot1->tts_tupleDescriptor->natts; attrnum++)
	{
		Form_pg_attribute att;
		TypeCacheEntry *typentry;

		att = TupleDescAttr(slot1->tts_tupleDescriptor, attrnum);

		/*
		 * Ignore dropped and generated columns as the publisher doesn't send
		 * those
		 */
		if (att->attisdropped || att->attgenerated)
			continue;

		/*
		 * If one value is NULL and other is not, then they are certainly not
		 * equal
		 */
		if (slot1->tts_isnull[attrnum] != slot2->tts_isnull[attrnum])
			return false;

		/*
		 * If both are NULL, they can be considered equal.
		 */
		if (slot1->tts_isnull[attrnum] || slot2->tts_isnull[attrnum])
			continue;

		typentry = eq[attrnum];
		if (typentry == NULL)
		{
			typentry = lookup_type_cache(att->atttypid,
										 TYPECACHE_EQ_OPR_FINFO);
			if (!OidIsValid(typentry->eq_opr_finfo.fn_oid))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("could not identify an equality operator for type %s",
								format_type_be(att->atttypid))));
			eq[attrnum] = typentry;
		}

		if (!DatumGetBool(FunctionCall2Coll(&typentry->eq_opr_finfo,
											att->attcollation,
											slot1->tts_values[attrnum],
											slot2->tts_values[attrnum])))
			return false;
	}

	return true;
}

/*
 * Search the relation 'rel' for tuple using the sequential scan.
 *
 * If a matching tuple is found, lock it with lockmode, fill the slot with its
 * contents, and return true.  Return false otherwise.
 *
 * Note that this stops on the first matching tuple.
 *
 * This can obviously be quite slow on tables that have more than few rows.
 */
bool
RelationFindReplTupleSeq(Relation rel, LockTupleMode lockmode,
						 TupleTableSlot *searchslot, TupleTableSlot *outslot)
{
	TupleTableSlot *scanslot;
	TableScanDesc scan;
	SnapshotData snap;
	TypeCacheEntry **eq;
	TransactionId xwait;
	bool		found;
	TupleDesc	desc PG_USED_FOR_ASSERTS_ONLY = RelationGetDescr(rel);

	Assert(equalTupleDescs(desc, outslot->tts_tupleDescriptor));

	eq = palloc0(sizeof(*eq) * outslot->tts_tupleDescriptor->natts);

	/* Start a heap scan. */
	InitDirtySnapshot(snap);
	scan = table_beginscan(rel, &snap, 0, NULL);
	scanslot = table_slot_create(rel, NULL);

retry:
	found = false;

	table_rescan(scan, NULL);

	/* Try to find the tuple */
	while (table_scan_getnextslot(scan, ForwardScanDirection, scanslot))
	{
		if (!tuples_equal(scanslot, searchslot, eq))
			continue;

		found = true;
		ExecCopySlot(outslot, scanslot);

		xwait = TransactionIdIsValid(snap.xmin) ?
			snap.xmin : snap.xmax;

		/*
		 * If the tuple is locked, wait for locking transaction to finish and
		 * retry.
		 */
		if (TransactionIdIsValid(xwait))
		{
			XactLockTableWait(xwait, NULL, NULL, XLTW_None);
			goto retry;
		}

		/* Found our tuple and it's not locked */
		break;
	}

	/* Found tuple, try to lock it in the lockmode. */
	if (found)
	{
		TM_FailureData tmfd;
		TM_Result	res;

		PushActiveSnapshot(GetLatestSnapshot());

		res = table_tuple_lock(rel, &(outslot->tts_tid), GetActiveSnapshot(),
							   outslot,
							   GetCurrentCommandId(false),
							   lockmode,
							   LockWaitBlock,
							   0 /* don't follow updates */ ,
							   &tmfd);

		PopActiveSnapshot();

		if (should_refetch_tuple(res, &tmfd))
			goto retry;
	}

	table_endscan(scan);
	ExecDropSingleTupleTableSlot(scanslot);

	return found;
}

/*
 * Build additional index information necessary for conflict detection.
 */
static void
BuildConflictIndexInfo(ResultRelInfo *resultRelInfo, Oid conflictindex)
{
	for (int i = 0; i < resultRelInfo->ri_NumIndices; i++)
	{
		Relation	indexRelation = resultRelInfo->ri_IndexRelationDescs[i];
		IndexInfo  *indexRelationInfo = resultRelInfo->ri_IndexRelationInfo[i];

		if (conflictindex != RelationGetRelid(indexRelation))
			continue;

		/*
		 * This Assert will fail if BuildSpeculativeIndexInfo() is called
		 * twice for the given index.
		 */
		Assert(indexRelationInfo->ii_UniqueOps == NULL);

		BuildSpeculativeIndexInfo(indexRelation, indexRelationInfo);
	}
}

/*
 * Find the tuple that violates the passed unique index (conflictindex).
 *
 * If the conflicting tuple is found return true, otherwise false.
 *
 * We lock the tuple to avoid getting it deleted before the caller can fetch
 * the required information. Note that if the tuple is deleted before a lock
 * is acquired, we will retry to find the conflicting tuple again.
 */
static bool
FindConflictTuple(ResultRelInfo *resultRelInfo, EState *estate,
				  Oid conflictindex, TupleTableSlot *slot,
				  TupleTableSlot **conflictslot)
{
	Relation	rel = resultRelInfo->ri_RelationDesc;
	ItemPointerData conflictTid;
	TM_FailureData tmfd;
	TM_Result	res;

	*conflictslot = NULL;

	/*
	 * Build additional information required to check constraints violations.
	 * See check_exclusion_or_unique_constraint().
	 */
	BuildConflictIndexInfo(resultRelInfo, conflictindex);

retry:
	if (ExecCheckIndexConstraints(resultRelInfo, slot, estate,
								  &conflictTid, &slot->tts_tid,
								  list_make1_oid(conflictindex)))
	{
		if (*conflictslot)
			ExecDropSingleTupleTableSlot(*conflictslot);

		*conflictslot = NULL;
		return false;
	}

	*conflictslot = table_slot_create(rel, NULL);

	PushActiveSnapshot(GetLatestSnapshot());

	res = table_tuple_lock(rel, &conflictTid, GetActiveSnapshot(),
						   *conflictslot,
						   GetCurrentCommandId(false),
						   LockTupleShare,
						   LockWaitBlock,
						   0 /* don't follow updates */ ,
						   &tmfd);

	PopActiveSnapshot();

	if (should_refetch_tuple(res, &tmfd))
		goto retry;

	return true;
}

/*
 * Check all the unique indexes in 'recheckIndexes' for conflict with the
 * tuple in 'remoteslot' and report if found.
 */
static void
CheckAndReportConflict(ResultRelInfo *resultRelInfo, EState *estate,
					   ConflictType type, List *recheckIndexes,
					   TupleTableSlot *searchslot, TupleTableSlot *remoteslot)
{
	List	   *conflicttuples = NIL;
	TupleTableSlot *conflictslot;

	/* Check all the unique indexes for conflicts */
	foreach_oid(uniqueidx, resultRelInfo->ri_onConflictArbiterIndexes)
	{
		if (list_member_oid(recheckIndexes, uniqueidx) &&
			FindConflictTuple(resultRelInfo, estate, uniqueidx, remoteslot,
							  &conflictslot))
		{
			ConflictTupleInfo *conflicttuple = palloc0_object(ConflictTupleInfo);

			conflicttuple->slot = conflictslot;
			conflicttuple->indexoid = uniqueidx;

			GetTupleTransactionInfo(conflictslot, &conflicttuple->xmin,
									&conflicttuple->origin, &conflicttuple->ts);

			conflicttuples = lappend(conflicttuples, conflicttuple);
		}
	}

	/* Report the conflict, if found */
	if (conflicttuples)
		ReportApplyConflict(estate, resultRelInfo, ERROR,
							list_length(conflicttuples) > 1 ? CT_MULTIPLE_UNIQUE_CONFLICTS : type,
							searchslot, remoteslot, conflicttuples);
}

/*
 * Insert tuple represented in the slot to the relation, update the indexes,
 * and execute any constraints and per-row triggers.
 *
 * Caller is responsible for opening the indexes.
 */
void
ExecSimpleRelationInsert(ResultRelInfo *resultRelInfo,
						 EState *estate, TupleTableSlot *slot)
{
	bool		skip_tuple = false;
	Relation	rel = resultRelInfo->ri_RelationDesc;

	/* For now we support only tables. */
	Assert(rel->rd_rel->relkind == RELKIND_RELATION);

	CheckCmdReplicaIdentity(rel, CMD_INSERT);

	/* BEFORE ROW INSERT Triggers */
	if (resultRelInfo->ri_TrigDesc &&
		resultRelInfo->ri_TrigDesc->trig_insert_before_row)
	{
		if (!ExecBRInsertTriggers(estate, resultRelInfo, slot))
			skip_tuple = true;	/* "do nothing" */
	}

	if (!skip_tuple)
	{
		List	   *recheckIndexes = NIL;
		List	   *conflictindexes;
		bool		conflict = false;

		/* Compute stored generated columns */
		if (rel->rd_att->constr &&
			rel->rd_att->constr->has_generated_stored)
			ExecComputeStoredGenerated(resultRelInfo, estate, slot,
									   CMD_INSERT);

		/* Check the constraints of the tuple */
		if (rel->rd_att->constr)
			ExecConstraints(resultRelInfo, slot, estate);
		if (rel->rd_rel->relispartition)
			ExecPartitionCheck(resultRelInfo, slot, estate, true);

		/* OK, store the tuple and create index entries for it */
		simple_table_tuple_insert(resultRelInfo->ri_RelationDesc, slot);

		conflictindexes = resultRelInfo->ri_onConflictArbiterIndexes;

		if (resultRelInfo->ri_NumIndices > 0)
			recheckIndexes = ExecInsertIndexTuples(resultRelInfo,
												   slot, estate, false,
												   conflictindexes ? true : false,
												   &conflict,
												   conflictindexes, false);

		/*
		 * Checks the conflict indexes to fetch the conflicting local tuple
		 * and reports the conflict. We perform this check here, instead of
		 * performing an additional index scan before the actual insertion and
		 * reporting the conflict if any conflicting tuples are found. This is
		 * to avoid the overhead of executing the extra scan for each INSERT
		 * operation, even when no conflict arises, which could introduce
		 * significant overhead to replication, particularly in cases where
		 * conflicts are rare.
		 *
		 * XXX OTOH, this could lead to clean-up effort for dead tuples added
		 * in heap and index in case of conflicts. But as conflicts shouldn't
		 * be a frequent thing so we preferred to save the performance
		 * overhead of extra scan before each insertion.
		 */
		if (conflict)
			CheckAndReportConflict(resultRelInfo, estate, CT_INSERT_EXISTS,
								   recheckIndexes, NULL, slot);

		/* AFTER ROW INSERT Triggers */
		ExecARInsertTriggers(estate, resultRelInfo, slot,
							 recheckIndexes, NULL);

		/*
		 * XXX we should in theory pass a TransitionCaptureState object to the
		 * above to capture transition tuples, but after statement triggers
		 * don't actually get fired by replication yet anyway
		 */

		list_free(recheckIndexes);
	}
}

/*
 * Find the searchslot tuple and update it with data in the slot,
 * update the indexes, and execute any constraints and per-row triggers.
 *
 * Caller is responsible for opening the indexes.
 */
void
ExecSimpleRelationUpdate(ResultRelInfo *resultRelInfo,
						 EState *estate, EPQState *epqstate,
						 TupleTableSlot *searchslot, TupleTableSlot *slot)
{
	bool		skip_tuple = false;
	Relation	rel = resultRelInfo->ri_RelationDesc;
	ItemPointer tid = &(searchslot->tts_tid);

	/*
	 * We support only non-system tables, with
	 * check_publication_add_relation() accountable.
	 */
	Assert(rel->rd_rel->relkind == RELKIND_RELATION);
	Assert(!IsCatalogRelation(rel));

	CheckCmdReplicaIdentity(rel, CMD_UPDATE);

	/* BEFORE ROW UPDATE Triggers */
	if (resultRelInfo->ri_TrigDesc &&
		resultRelInfo->ri_TrigDesc->trig_update_before_row)
	{
		if (!ExecBRUpdateTriggers(estate, epqstate, resultRelInfo,
								  tid, NULL, slot, NULL, NULL, false))
			skip_tuple = true;	/* "do nothing" */
	}

	if (!skip_tuple)
	{
		List	   *recheckIndexes = NIL;
		TU_UpdateIndexes update_indexes;
		List	   *conflictindexes;
		bool		conflict = false;

		/* Compute stored generated columns */
		if (rel->rd_att->constr &&
			rel->rd_att->constr->has_generated_stored)
			ExecComputeStoredGenerated(resultRelInfo, estate, slot,
									   CMD_UPDATE);

		/* Check the constraints of the tuple */
		if (rel->rd_att->constr)
			ExecConstraints(resultRelInfo, slot, estate);
		if (rel->rd_rel->relispartition)
			ExecPartitionCheck(resultRelInfo, slot, estate, true);

		simple_table_tuple_update(rel, tid, slot, estate->es_snapshot,
								  &update_indexes);

		conflictindexes = resultRelInfo->ri_onConflictArbiterIndexes;

		if (resultRelInfo->ri_NumIndices > 0 && (update_indexes != TU_None))
			recheckIndexes = ExecInsertIndexTuples(resultRelInfo,
												   slot, estate, true,
												   conflictindexes ? true : false,
												   &conflict, conflictindexes,
												   (update_indexes == TU_Summarizing));

		/*
		 * Refer to the comments above the call to CheckAndReportConflict() in
		 * ExecSimpleRelationInsert to understand why this check is done at
		 * this point.
		 */
		if (conflict)
			CheckAndReportConflict(resultRelInfo, estate, CT_UPDATE_EXISTS,
								   recheckIndexes, searchslot, slot);

		/* AFTER ROW UPDATE Triggers */
		ExecARUpdateTriggers(estate, resultRelInfo,
							 NULL, NULL,
							 tid, NULL, slot,
							 recheckIndexes, NULL, false);

		list_free(recheckIndexes);
	}
}

/*
 * Find the searchslot tuple and delete it, and execute any constraints
 * and per-row triggers.
 *
 * Caller is responsible for opening the indexes.
 */
void
ExecSimpleRelationDelete(ResultRelInfo *resultRelInfo,
						 EState *estate, EPQState *epqstate,
						 TupleTableSlot *searchslot)
{
	bool		skip_tuple = false;
	Relation	rel = resultRelInfo->ri_RelationDesc;
	ItemPointer tid = &searchslot->tts_tid;

	CheckCmdReplicaIdentity(rel, CMD_DELETE);

	/* BEFORE ROW DELETE Triggers */
	if (resultRelInfo->ri_TrigDesc &&
		resultRelInfo->ri_TrigDesc->trig_delete_before_row)
	{
		skip_tuple = !ExecBRDeleteTriggers(estate, epqstate, resultRelInfo,
										   tid, NULL, NULL, NULL, NULL, false);
	}

	if (!skip_tuple)
	{
		/* OK, delete the tuple */
		simple_table_tuple_delete(rel, tid, estate->es_snapshot);

		/* AFTER ROW DELETE Triggers */
		ExecARDeleteTriggers(estate, resultRelInfo,
							 tid, NULL, NULL, false);
	}
}

/*
 * Check if command can be executed with current replica identity.
 */
void
CheckCmdReplicaIdentity(Relation rel, CmdType cmd)
{
	PublicationDesc pubdesc;

	/*
	 * Skip checking the replica identity for partitioned tables, because the
	 * operations are actually performed on the leaf partitions.
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		return;

	/* We only need to do checks for UPDATE and DELETE. */
	if (cmd != CMD_UPDATE && cmd != CMD_DELETE)
		return;

	/*
	 * It is only safe to execute UPDATE/DELETE if the relation does not
	 * publish UPDATEs or DELETEs, or all the following conditions are
	 * satisfied:
	 *
	 * 1. All columns, referenced in the row filters from publications which
	 * the relation is in, are valid - i.e. when all referenced columns are
	 * part of REPLICA IDENTITY.
	 *
	 * 2. All columns, referenced in the column lists are valid - i.e. when
	 * all columns referenced in the REPLICA IDENTITY are covered by the
	 * column list.
	 *
	 * 3. All generated columns in REPLICA IDENTITY of the relation, are valid
	 * - i.e. when all these generated columns are published.
	 *
	 * XXX We could optimize it by first checking whether any of the
	 * publications have a row filter or column list for this relation, or if
	 * the relation contains a generated column. If none of these exist and
	 * the relation has replica identity then we can avoid building the
	 * descriptor but as this happens only one time it doesn't seem worth the
	 * additional complexity.
	 */
	RelationBuildPublicationDesc(rel, &pubdesc);
	if (cmd == CMD_UPDATE && !pubdesc.rf_valid_for_update)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("cannot update table \"%s\"",
						RelationGetRelationName(rel)),
				 errdetail("Column used in the publication WHERE expression is not part of the replica identity.")));
	else if (cmd == CMD_UPDATE && !pubdesc.cols_valid_for_update)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("cannot update table \"%s\"",
						RelationGetRelationName(rel)),
				 errdetail("Column list used by the publication does not cover the replica identity.")));
	else if (cmd == CMD_UPDATE && !pubdesc.gencols_valid_for_update)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("cannot update table \"%s\"",
						RelationGetRelationName(rel)),
				 errdetail("Replica identity must not contain unpublished generated columns.")));
	else if (cmd == CMD_DELETE && !pubdesc.rf_valid_for_delete)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("cannot delete from table \"%s\"",
						RelationGetRelationName(rel)),
				 errdetail("Column used in the publication WHERE expression is not part of the replica identity.")));
	else if (cmd == CMD_DELETE && !pubdesc.cols_valid_for_delete)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("cannot delete from table \"%s\"",
						RelationGetRelationName(rel)),
				 errdetail("Column list used by the publication does not cover the replica identity.")));
	else if (cmd == CMD_DELETE && !pubdesc.gencols_valid_for_delete)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("cannot delete from table \"%s\"",
						RelationGetRelationName(rel)),
				 errdetail("Replica identity must not contain unpublished generated columns.")));

	/* If relation has replica identity we are always good. */
	if (OidIsValid(RelationGetReplicaIndex(rel)))
		return;

	/* REPLICA IDENTITY FULL is also good for UPDATE/DELETE. */
	if (rel->rd_rel->relreplident == REPLICA_IDENTITY_FULL)
		return;

	/*
	 * This is UPDATE/DELETE and there is no replica identity.
	 *
	 * Check if the table publishes UPDATES or DELETES.
	 */
	if (cmd == CMD_UPDATE && pubdesc.pubactions.pubupdate)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot update table \"%s\" because it does not have a replica identity and publishes updates",
						RelationGetRelationName(rel)),
				 errhint("To enable updating the table, set REPLICA IDENTITY using ALTER TABLE.")));
	else if (cmd == CMD_DELETE && pubdesc.pubactions.pubdelete)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot delete from table \"%s\" because it does not have a replica identity and publishes deletes",
						RelationGetRelationName(rel)),
				 errhint("To enable deleting from the table, set REPLICA IDENTITY using ALTER TABLE.")));
}


/*
 * Check if we support writing into specific relkind.
 *
 * The nspname and relname are only needed for error reporting.
 */
void
CheckSubscriptionRelkind(char relkind, const char *nspname,
						 const char *relname)
{
	if (relkind != RELKIND_RELATION && relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot use relation \"%s.%s\" as logical replication target",
						nspname, relname),
				 errdetail_relkind_not_supported(relkind)));
}
