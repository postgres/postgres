/*--------------------------------------------------------------------------
 *
 * test_indexscan.c
 *		Test helpers for low-level index scan behavior.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/index/test_indexscan.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/genam.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "catalog/pg_class.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/itemptr.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplestore.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(index_scan_tids);
Datum
index_scan_tids(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	char	   *snaptype = text_to_cstring(PG_GETARG_TEXT_PP(1));
	bool		forwardscan = PG_GETARG_BOOL(2);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Oid			heapoid;
	Relation	heaprel;
	Relation	indexrel;
	ScanDirection dir = forwardscan ? ForwardScanDirection : BackwardScanDirection;
	SnapshotData snapdata;
	Snapshot	snapshot;
	IndexScanDesc scan;
	TupleTableSlot *slot;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use index scan test functions")));

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);

	/*
	 * Lock the heap before the index to avoid deadlock.  IndexGetRelation()
	 * runs without a lock, so if the OID isn't an index it returns
	 * InvalidOid; defer the complaint to index_open() below, which gives a
	 * better message.
	 */
	heapoid = IndexGetRelation(indexoid, true);
	if (OidIsValid(heapoid))
		heaprel = table_open(heapoid, AccessShareLock);
	else
		heaprel = NULL;

	indexrel = index_open(indexoid, AccessShareLock);

	/*
	 * Since the IndexGetRelation() call above ran without a lock, recheck now
	 * that both relations are locked: a concurrent drop and recreate could
	 * have left us with the wrong heap.
	 */
	if (heaprel == NULL || heapoid != IndexGetRelation(indexoid, false))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("could not open parent table of index \"%s\"",
						RelationGetRelationName(indexrel))));

	if (indexrel->rd_rel->relkind != RELKIND_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a regular index",
						RelationGetRelationName(indexrel))));

	if (indexrel->rd_indam->amgettuple == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("index \"%s\" does not support plain index scans",
						RelationGetRelationName(indexrel))));

	if (strcmp(snaptype, "mvcc") == 0)
		snapshot = GetActiveSnapshot();
	else if (strcmp(snaptype, "any") == 0)
		snapshot = SnapshotAny;
	else if (strcmp(snaptype, "self") == 0)
		snapshot = SnapshotSelf;
	else if (strcmp(snaptype, "dirty") == 0)
	{
		InitDirtySnapshot(snapdata);
		snapshot = &snapdata;
	}
	else if (strcmp(snaptype, "nonvacuumable") == 0)
	{
		InitNonVacuumableSnapshot(snapdata, GlobalVisTestFor(heaprel));
		snapshot = &snapdata;
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid snapshot type \"%s\"", snaptype)));

	slot = table_slot_create(heaprel, NULL);

	scan = index_beginscan(heaprel, indexrel, false, snapshot, NULL,
						   0, 0, SO_NONE);
	index_rescan(scan, NULL, 0, NULL, 0);

	while (table_index_getnext_slot(scan, dir, slot))
	{
		ItemPointerData tid = slot->tts_tid;
		Datum		values[1];
		bool		nulls[1];

		if (scan->xs_recheck)
			elog(ERROR, "unexpected recheck request");

		values[0] = ItemPointerGetDatum(&tid);
		nulls[0] = false;
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
	}

	index_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	index_close(indexrel, AccessShareLock);
	table_close(heaprel, AccessShareLock);

	return (Datum) 0;
}
