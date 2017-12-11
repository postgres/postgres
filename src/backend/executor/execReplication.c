/*-------------------------------------------------------------------------
 *
 * execReplication.c
 *	  miscellaneous executor routines for logical replication
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/execReplication.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/relscan.h"
#include "access/transam.h"
#include "access/xact.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/typcache.h"
#include "utils/tqual.h"


/*
 * Setup a ScanKey for a search in the relation 'rel' for a tuple 'key' that
 * is setup to match 'rel' (*NOT* idxrel!).
 *
 * Returns whether any column contains NULLs.
 *
 * This is not generic routine, it expects the idxrel to be replication
 * identity of a rel and meet all limitations associated with that.
 */
static bool
build_replindex_scan_key(ScanKey skey, Relation rel, Relation idxrel,
						 TupleTableSlot *searchslot)
{
	int			attoff;
	bool		isnull;
	Datum		indclassDatum;
	oidvector  *opclass;
	int2vector *indkey = &idxrel->rd_index->indkey;
	bool		hasnulls = false;

	Assert(RelationGetReplicaIndex(rel) == RelationGetRelid(idxrel));

	indclassDatum = SysCacheGetAttr(INDEXRELID, idxrel->rd_indextuple,
									Anum_pg_index_indclass, &isnull);
	Assert(!isnull);
	opclass = (oidvector *) DatumGetPointer(indclassDatum);

	/* Build scankey for every attribute in the index. */
	for (attoff = 0; attoff < RelationGetNumberOfAttributes(idxrel); attoff++)
	{
		Oid			operator;
		Oid			opfamily;
		RegProcedure regop;
		int			pkattno = attoff + 1;
		int			mainattno = indkey->values[attoff];
		Oid			optype = get_opclass_input_type(opclass->values[attoff]);

		/*
		 * Load the operator info.  We need this to get the equality operator
		 * function for the scan key.
		 */
		opfamily = get_opclass_family(opclass->values[attoff]);

		operator = get_opfamily_member(opfamily, optype,
									   optype,
									   BTEqualStrategyNumber);
		if (!OidIsValid(operator))
			elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
				 BTEqualStrategyNumber, optype, optype, opfamily);

		regop = get_opcode(operator);

		/* Initialize the scankey. */
		ScanKeyInit(&skey[attoff],
					pkattno,
					BTEqualStrategyNumber,
					regop,
					searchslot->tts_values[mainattno - 1]);

		/* Check for null value. */
		if (searchslot->tts_isnull[mainattno - 1])
		{
			hasnulls = true;
			skey[attoff].sk_flags |= SK_ISNULL;
		}
	}

	return hasnulls;
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
	HeapTuple	scantuple;
	ScanKeyData skey[INDEX_MAX_KEYS];
	IndexScanDesc scan;
	SnapshotData snap;
	TransactionId xwait;
	Relation	idxrel;
	bool		found;

	/* Open the index. */
	idxrel = index_open(idxoid, RowExclusiveLock);

	/* Start an index scan. */
	InitDirtySnapshot(snap);
	scan = index_beginscan(rel, idxrel, &snap,
						   RelationGetNumberOfAttributes(idxrel),
						   0);

	/* Build scan key. */
	build_replindex_scan_key(skey, rel, idxrel, searchslot);

retry:
	found = false;

	index_rescan(scan, skey, RelationGetNumberOfAttributes(idxrel), NULL, 0);

	/* Try to find the tuple */
	if ((scantuple = index_getnext(scan, ForwardScanDirection)) != NULL)
	{
		found = true;
		ExecStoreTuple(scantuple, outslot, InvalidBuffer, false);
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
	}

	/* Found tuple, try to lock it in the lockmode. */
	if (found)
	{
		Buffer		buf;
		HeapUpdateFailureData hufd;
		HTSU_Result res;
		HeapTupleData locktup;

		ItemPointerCopy(&outslot->tts_tuple->t_self, &locktup.t_self);

		PushActiveSnapshot(GetLatestSnapshot());

		res = heap_lock_tuple(rel, &locktup, GetCurrentCommandId(false),
							  lockmode,
							  LockWaitBlock,
							  false /* don't follow updates */ ,
							  &buf, &hufd);
		/* the tuple slot already has the buffer pinned */
		ReleaseBuffer(buf);

		PopActiveSnapshot();

		switch (res)
		{
			case HeapTupleMayBeUpdated:
				break;
			case HeapTupleUpdated:
				/* XXX: Improve handling here */
				ereport(LOG,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("concurrent update, retrying")));
				goto retry;
			case HeapTupleInvisible:
				elog(ERROR, "attempted to lock invisible tuple");
			default:
				elog(ERROR, "unexpected heap_lock_tuple status: %u", res);
				break;
		}
	}

	index_endscan(scan);

	/* Don't release lock until commit. */
	index_close(idxrel, NoLock);

	return found;
}

/*
 * Compare the tuple and slot and check if they have equal values.
 *
 * We use binary datum comparison which might return false negatives but
 * that's the best we can do here as there may be multiple notions of
 * equality for the data types and table columns don't specify which one
 * to use.
 */
static bool
tuple_equals_slot(TupleDesc desc, HeapTuple tup, TupleTableSlot *slot)
{
	Datum		values[MaxTupleAttributeNumber];
	bool		isnull[MaxTupleAttributeNumber];
	int			attrnum;

	heap_deform_tuple(tup, desc, values, isnull);

	/* Check equality of the attributes. */
	for (attrnum = 0; attrnum < desc->natts; attrnum++)
	{
		Form_pg_attribute att;
		TypeCacheEntry *typentry;

		/*
		 * If one value is NULL and other is not, then they are certainly not
		 * equal
		 */
		if (isnull[attrnum] != slot->tts_isnull[attrnum])
			return false;

		/*
		 * If both are NULL, they can be considered equal.
		 */
		if (isnull[attrnum])
			continue;

		att = desc->attrs[attrnum];

		typentry = lookup_type_cache(att->atttypid, TYPECACHE_EQ_OPR_FINFO);
		if (!OidIsValid(typentry->eq_opr_finfo.fn_oid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("could not identify an equality operator for type %s",
							format_type_be(att->atttypid))));

		if (!DatumGetBool(FunctionCall2(&typentry->eq_opr_finfo,
										values[attrnum],
										slot->tts_values[attrnum])))
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
	HeapTuple	scantuple;
	HeapScanDesc scan;
	SnapshotData snap;
	TransactionId xwait;
	bool		found;
	TupleDesc	desc = RelationGetDescr(rel);

	Assert(equalTupleDescs(desc, outslot->tts_tupleDescriptor));

	/* Start a heap scan. */
	InitDirtySnapshot(snap);
	scan = heap_beginscan(rel, &snap, 0, NULL);

retry:
	found = false;

	heap_rescan(scan, NULL);

	/* Try to find the tuple */
	while ((scantuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		if (!tuple_equals_slot(desc, scantuple, searchslot))
			continue;

		found = true;
		ExecStoreTuple(scantuple, outslot, InvalidBuffer, false);
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
	}

	/* Found tuple, try to lock it in the lockmode. */
	if (found)
	{
		Buffer		buf;
		HeapUpdateFailureData hufd;
		HTSU_Result res;
		HeapTupleData locktup;

		ItemPointerCopy(&outslot->tts_tuple->t_self, &locktup.t_self);

		PushActiveSnapshot(GetLatestSnapshot());

		res = heap_lock_tuple(rel, &locktup, GetCurrentCommandId(false),
							  lockmode,
							  LockWaitBlock,
							  false /* don't follow updates */ ,
							  &buf, &hufd);
		/* the tuple slot already has the buffer pinned */
		ReleaseBuffer(buf);

		PopActiveSnapshot();

		switch (res)
		{
			case HeapTupleMayBeUpdated:
				break;
			case HeapTupleUpdated:
				/* XXX: Improve handling here */
				ereport(LOG,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("concurrent update, retrying")));
				goto retry;
			case HeapTupleInvisible:
				elog(ERROR, "attempted to lock invisible tuple");
			default:
				elog(ERROR, "unexpected heap_lock_tuple status: %u", res);
				break;
		}
	}

	heap_endscan(scan);

	return found;
}

/*
 * Insert tuple represented in the slot to the relation, update the indexes,
 * and execute any constraints and per-row triggers.
 *
 * Caller is responsible for opening the indexes.
 */
void
ExecSimpleRelationInsert(EState *estate, TupleTableSlot *slot)
{
	bool		skip_tuple = false;
	HeapTuple	tuple;
	ResultRelInfo *resultRelInfo = estate->es_result_relation_info;
	Relation	rel = resultRelInfo->ri_RelationDesc;

	/* For now we support only tables. */
	Assert(rel->rd_rel->relkind == RELKIND_RELATION);

	CheckCmdReplicaIdentity(rel, CMD_INSERT);

	/* BEFORE ROW INSERT Triggers */
	if (resultRelInfo->ri_TrigDesc &&
		resultRelInfo->ri_TrigDesc->trig_insert_before_row)
	{
		slot = ExecBRInsertTriggers(estate, resultRelInfo, slot);

		if (slot == NULL)		/* "do nothing" */
			skip_tuple = true;
	}

	if (!skip_tuple)
	{
		List	   *recheckIndexes = NIL;

		/* Check the constraints of the tuple */
		if (rel->rd_att->constr)
			ExecConstraints(resultRelInfo, slot, estate);

		/* Store the slot into tuple that we can inspect. */
		tuple = ExecMaterializeSlot(slot);

		/* OK, store the tuple and create index entries for it */
		simple_heap_insert(rel, tuple);

		if (resultRelInfo->ri_NumIndices > 0)
			recheckIndexes = ExecInsertIndexTuples(slot, &(tuple->t_self),
												   estate, false, NULL,
												   NIL);

		/* AFTER ROW INSERT Triggers */
		ExecARInsertTriggers(estate, resultRelInfo, tuple,
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
ExecSimpleRelationUpdate(EState *estate, EPQState *epqstate,
						 TupleTableSlot *searchslot, TupleTableSlot *slot)
{
	bool		skip_tuple = false;
	HeapTuple	tuple;
	ResultRelInfo *resultRelInfo = estate->es_result_relation_info;
	Relation	rel = resultRelInfo->ri_RelationDesc;

	/* For now we support only tables. */
	Assert(rel->rd_rel->relkind == RELKIND_RELATION);

	CheckCmdReplicaIdentity(rel, CMD_UPDATE);

	/* BEFORE ROW UPDATE Triggers */
	if (resultRelInfo->ri_TrigDesc &&
		resultRelInfo->ri_TrigDesc->trig_update_before_row)
	{
		slot = ExecBRUpdateTriggers(estate, epqstate, resultRelInfo,
									&searchslot->tts_tuple->t_self,
									NULL, slot);

		if (slot == NULL)		/* "do nothing" */
			skip_tuple = true;
	}

	if (!skip_tuple)
	{
		List	   *recheckIndexes = NIL;

		/* Check the constraints of the tuple */
		if (rel->rd_att->constr)
			ExecConstraints(resultRelInfo, slot, estate);

		/* Store the slot into tuple that we can write. */
		tuple = ExecMaterializeSlot(slot);

		/* OK, update the tuple and index entries for it */
		simple_heap_update(rel, &searchslot->tts_tuple->t_self,
						   slot->tts_tuple);

		if (resultRelInfo->ri_NumIndices > 0 &&
			!HeapTupleIsHeapOnly(slot->tts_tuple))
			recheckIndexes = ExecInsertIndexTuples(slot, &(tuple->t_self),
												   estate, false, NULL,
												   NIL);

		/* AFTER ROW UPDATE Triggers */
		ExecARUpdateTriggers(estate, resultRelInfo,
							 &searchslot->tts_tuple->t_self,
							 NULL, tuple, recheckIndexes, NULL);

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
ExecSimpleRelationDelete(EState *estate, EPQState *epqstate,
						 TupleTableSlot *searchslot)
{
	bool		skip_tuple = false;
	ResultRelInfo *resultRelInfo = estate->es_result_relation_info;
	Relation	rel = resultRelInfo->ri_RelationDesc;

	/* For now we support only tables. */
	Assert(rel->rd_rel->relkind == RELKIND_RELATION);

	CheckCmdReplicaIdentity(rel, CMD_DELETE);

	/* BEFORE ROW DELETE Triggers */
	if (resultRelInfo->ri_TrigDesc &&
		resultRelInfo->ri_TrigDesc->trig_delete_before_row)
	{
		skip_tuple = !ExecBRDeleteTriggers(estate, epqstate, resultRelInfo,
										   &searchslot->tts_tuple->t_self,
										   NULL);
	}

	if (!skip_tuple)
	{
		List	   *recheckIndexes = NIL;

		/* OK, delete the tuple */
		simple_heap_delete(rel, &searchslot->tts_tuple->t_self);

		/* AFTER ROW DELETE Triggers */
		ExecARDeleteTriggers(estate, resultRelInfo,
							 &searchslot->tts_tuple->t_self, NULL, NULL);

		list_free(recheckIndexes);
	}
}

/*
 * Check if command can be executed with current replica identity.
 */
void
CheckCmdReplicaIdentity(Relation rel, CmdType cmd)
{
	PublicationActions *pubactions;

	/* We only need to do checks for UPDATE and DELETE. */
	if (cmd != CMD_UPDATE && cmd != CMD_DELETE)
		return;

	/* If relation has replica identity we are always good. */
	if (rel->rd_rel->relreplident == REPLICA_IDENTITY_FULL ||
		OidIsValid(RelationGetReplicaIndex(rel)))
		return;

	/*
	 * This is either UPDATE OR DELETE and there is no replica identity.
	 *
	 * Check if the table publishes UPDATES or DELETES.
	 */
	pubactions = GetRelationPublicationActions(rel);
	if (cmd == CMD_UPDATE && pubactions->pubupdate)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot update table \"%s\" because it does not have a replica identity and publishes updates",
						RelationGetRelationName(rel)),
				 errhint("To enable updating the table, set REPLICA IDENTITY using ALTER TABLE.")));
	else if (cmd == CMD_DELETE && pubactions->pubdelete)
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
	/*
	 * We currently only support writing to regular tables.
	 */
	if (relkind != RELKIND_RELATION)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("logical replication target relation \"%s.%s\" is not a table",
						nspname, relname)));
}
