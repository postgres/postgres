/*-------------------------------------------------------------------------
 *
 * time_mapping.c
 *	  time to XID mapping information
 *
 * Copyright (c) 2020-2021, PostgreSQL Global Development Group
 *
 *	  contrib/old_snapshot/time_mapping.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "funcapi.h"
#include "storage/lwlock.h"
#include "utils/old_snapshot.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

/*
 * Backend-private copy of the information from oldSnapshotControl which relates
 * to the time to XID mapping, plus an index so that we can iterate.
 *
 * Note that the length of the xid_by_minute array is given by
 * OLD_SNAPSHOT_TIME_MAP_ENTRIES (which is not a compile-time constant).
 */
typedef struct
{
	int				current_index;
	int				head_offset;
	TimestampTz		head_timestamp;
	int				count_used;
	TransactionId	xid_by_minute[FLEXIBLE_ARRAY_MEMBER];
} OldSnapshotTimeMapping;

#define NUM_TIME_MAPPING_COLUMNS 3

PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(pg_old_snapshot_time_mapping);

static OldSnapshotTimeMapping *GetOldSnapshotTimeMapping(void);
static TupleDesc MakeOldSnapshotTimeMappingTupleDesc(void);
static HeapTuple MakeOldSnapshotTimeMappingTuple(TupleDesc tupdesc,
												 OldSnapshotTimeMapping *mapping);

/*
 * SQL-callable set-returning function.
 */
Datum
pg_old_snapshot_time_mapping(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	OldSnapshotTimeMapping *mapping;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext	oldcontext;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		mapping = GetOldSnapshotTimeMapping();
		funcctx->user_fctx = mapping;
		funcctx->tuple_desc = MakeOldSnapshotTimeMappingTupleDesc();
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	mapping = (OldSnapshotTimeMapping *) funcctx->user_fctx;

	while (mapping->current_index < mapping->count_used)
	{
		HeapTuple	tuple;

		tuple = MakeOldSnapshotTimeMappingTuple(funcctx->tuple_desc, mapping);
		++mapping->current_index;
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}

	SRF_RETURN_DONE(funcctx);
}

/*
 * Get the old snapshot time mapping data from shared memory.
 */
static OldSnapshotTimeMapping *
GetOldSnapshotTimeMapping(void)
{
	OldSnapshotTimeMapping *mapping;

	mapping = palloc(offsetof(OldSnapshotTimeMapping, xid_by_minute)
					 + sizeof(TransactionId) * OLD_SNAPSHOT_TIME_MAP_ENTRIES);
	mapping->current_index = 0;

	LWLockAcquire(OldSnapshotTimeMapLock, LW_SHARED);
	mapping->head_offset = oldSnapshotControl->head_offset;
	mapping->head_timestamp = oldSnapshotControl->head_timestamp;
	mapping->count_used = oldSnapshotControl->count_used;
	for (int i = 0; i < OLD_SNAPSHOT_TIME_MAP_ENTRIES; ++i)
		mapping->xid_by_minute[i] = oldSnapshotControl->xid_by_minute[i];
	LWLockRelease(OldSnapshotTimeMapLock);

	return mapping;
}

/*
 * Build a tuple descriptor for the pg_old_snapshot_time_mapping() SRF.
 */
static TupleDesc
MakeOldSnapshotTimeMappingTupleDesc(void)
{
	TupleDesc	tupdesc;

	tupdesc = CreateTemplateTupleDesc(NUM_TIME_MAPPING_COLUMNS);

	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "array_offset",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "end_timestamp",
					   TIMESTAMPTZOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "newest_xmin",
					   XIDOID, -1, 0);

	return BlessTupleDesc(tupdesc);
}

/*
 * Convert one entry from the old snapshot time mapping to a HeapTuple.
 */
static HeapTuple
MakeOldSnapshotTimeMappingTuple(TupleDesc tupdesc, OldSnapshotTimeMapping *mapping)
{
	Datum	values[NUM_TIME_MAPPING_COLUMNS];
	bool	nulls[NUM_TIME_MAPPING_COLUMNS];
	int		array_position;
	TimestampTz	timestamp;

	/*
	 * Figure out the array position corresponding to the current index.
	 *
	 * Index 0 means the oldest entry in the mapping, which is stored at
	 * mapping->head_offset. Index 1 means the next-oldest entry, which is a the
	 * following index, and so on. We wrap around when we reach the end of the array.
	 */
	array_position = (mapping->head_offset + mapping->current_index)
		% OLD_SNAPSHOT_TIME_MAP_ENTRIES;

	/*
	 * No explicit timestamp is stored for any entry other than the oldest one,
	 * but each entry corresponds to 1-minute period, so we can just add.
	 */
	timestamp = TimestampTzPlusMilliseconds(mapping->head_timestamp,
											mapping->current_index * 60000);

	/* Initialize nulls and values arrays. */
	memset(nulls, 0, sizeof(nulls));
	values[0] = Int32GetDatum(array_position);
	values[1] = TimestampTzGetDatum(timestamp);
	values[2] = TransactionIdGetDatum(mapping->xid_by_minute[array_position]);

	return heap_form_tuple(tupdesc, values, nulls);
}
