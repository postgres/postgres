/*--------------------------------------------------------------------------
 *
 * test_custom_fixed_stats.c
 *		Test module for fixed-sized custom pgstats
 *
 * Copyright (c) 2025-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_custom_stats/test_custom_fixed_stats.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "funcapi.h"
#include "pgstat.h"
#include "utils/builtins.h"
#include "utils/pgstat_internal.h"

PG_MODULE_MAGIC_EXT(
					.name = "test_custom_fixed_stats",
					.version = PG_VERSION
);

/* Fixed-amount custom statistics entry */
typedef struct PgStat_StatCustomFixedEntry
{
	PgStat_Counter numcalls;	/* # of times update function called */
	TimestampTz stat_reset_timestamp;
} PgStat_StatCustomFixedEntry;

typedef struct PgStatShared_CustomFixedEntry
{
	LWLock		lock;			/* protects counters */
	uint32		changecount;	/* for atomic reads */
	PgStat_StatCustomFixedEntry stats;	/* current counters */
	PgStat_StatCustomFixedEntry reset_offset;	/* reset baseline */
} PgStatShared_CustomFixedEntry;

/* Callbacks for fixed-amount statistics */
static void test_custom_stats_fixed_init_shmem_cb(void *stats);
static void test_custom_stats_fixed_reset_all_cb(TimestampTz ts);
static void test_custom_stats_fixed_snapshot_cb(void);

static const PgStat_KindInfo custom_stats = {
	.name = "test_custom_fixed_stats",
	.fixed_amount = true,		/* exactly one entry */
	.write_to_file = true,		/* persist to stats file */

	.shared_size = sizeof(PgStat_StatCustomFixedEntry),
	.shared_data_off = offsetof(PgStatShared_CustomFixedEntry, stats),
	.shared_data_len = sizeof(((PgStatShared_CustomFixedEntry *) 0)->stats),

	.init_shmem_cb = test_custom_stats_fixed_init_shmem_cb,
	.reset_all_cb = test_custom_stats_fixed_reset_all_cb,
	.snapshot_cb = test_custom_stats_fixed_snapshot_cb,
};

/*
 * Kind ID for test_custom_fixed_stats.
 */
#define PGSTAT_KIND_TEST_CUSTOM_FIXED_STATS 26

/*--------------------------------------------------------------------------
 * Module initialization
 *--------------------------------------------------------------------------
 */

void
_PG_init(void)
{
	/* Must be loaded via shared_preload_libraries */
	if (!process_shared_preload_libraries_in_progress)
		return;

	/* Register custom statistics kind */
	pgstat_register_kind(PGSTAT_KIND_TEST_CUSTOM_FIXED_STATS, &custom_stats);
}

/*
 * test_custom_stats_fixed_init_shmem_cb
 *		Initialize shared memory structure
 */
static void
test_custom_stats_fixed_init_shmem_cb(void *stats)
{
	PgStatShared_CustomFixedEntry *stats_shmem =
		(PgStatShared_CustomFixedEntry *) stats;

	LWLockInitialize(&stats_shmem->lock, LWTRANCHE_PGSTATS_DATA);
}

/*
 * test_custom_stats_fixed_reset_all_cb
 *		Reset the fixed-sized stats
 */
static void
test_custom_stats_fixed_reset_all_cb(TimestampTz ts)
{
	PgStatShared_CustomFixedEntry *stats_shmem =
		pgstat_get_custom_shmem_data(PGSTAT_KIND_TEST_CUSTOM_FIXED_STATS);

	/* see explanation above PgStatShared_Archiver for the reset protocol */
	LWLockAcquire(&stats_shmem->lock, LW_EXCLUSIVE);
	pgstat_copy_changecounted_stats(&stats_shmem->reset_offset,
									&stats_shmem->stats,
									sizeof(stats_shmem->stats),
									&stats_shmem->changecount);
	stats_shmem->stats.stat_reset_timestamp = ts;
	LWLockRelease(&stats_shmem->lock);
}

/*
 * test_custom_stats_fixed_snapshot_cb
 *		Copy current stats to snapshot area
 */
static void
test_custom_stats_fixed_snapshot_cb(void)
{
	PgStatShared_CustomFixedEntry *stats_shmem =
		pgstat_get_custom_shmem_data(PGSTAT_KIND_TEST_CUSTOM_FIXED_STATS);
	PgStat_StatCustomFixedEntry *stat_snap =
		pgstat_get_custom_snapshot_data(PGSTAT_KIND_TEST_CUSTOM_FIXED_STATS);
	PgStat_StatCustomFixedEntry *reset_offset = &stats_shmem->reset_offset;
	PgStat_StatCustomFixedEntry reset;

	pgstat_copy_changecounted_stats(stat_snap,
									&stats_shmem->stats,
									sizeof(stats_shmem->stats),
									&stats_shmem->changecount);

	LWLockAcquire(&stats_shmem->lock, LW_SHARED);
	memcpy(&reset, reset_offset, sizeof(stats_shmem->stats));
	LWLockRelease(&stats_shmem->lock);

	/* Apply reset offsets */
#define FIXED_COMP(fld) stat_snap->fld -= reset.fld;
	FIXED_COMP(numcalls);
#undef FIXED_COMP
}

/*--------------------------------------------------------------------------
 * SQL-callable functions
 *--------------------------------------------------------------------------
 */

/*
 * test_custom_stats_fixed_update
 *		Increment call counter
 */
PG_FUNCTION_INFO_V1(test_custom_stats_fixed_update);
Datum
test_custom_stats_fixed_update(PG_FUNCTION_ARGS)
{
	PgStatShared_CustomFixedEntry *stats_shmem;

	stats_shmem = pgstat_get_custom_shmem_data(PGSTAT_KIND_TEST_CUSTOM_FIXED_STATS);

	LWLockAcquire(&stats_shmem->lock, LW_EXCLUSIVE);

	pgstat_begin_changecount_write(&stats_shmem->changecount);
	stats_shmem->stats.numcalls++;
	pgstat_end_changecount_write(&stats_shmem->changecount);

	LWLockRelease(&stats_shmem->lock);

	PG_RETURN_VOID();
}

/*
 * test_custom_stats_fixed_reset
 *		Reset statistics by calling pgstat system
 */
PG_FUNCTION_INFO_V1(test_custom_stats_fixed_reset);
Datum
test_custom_stats_fixed_reset(PG_FUNCTION_ARGS)
{
	pgstat_reset_of_kind(PGSTAT_KIND_TEST_CUSTOM_FIXED_STATS);

	PG_RETURN_VOID();
}

/*
 * test_custom_stats_fixed_report
 *		Return current counter values
 */
PG_FUNCTION_INFO_V1(test_custom_stats_fixed_report);
Datum
test_custom_stats_fixed_report(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[2] = {0};
	bool		nulls[2] = {false};
	PgStat_StatCustomFixedEntry *stats;

	/* Take snapshot (applies reset offsets) */
	pgstat_snapshot_fixed(PGSTAT_KIND_TEST_CUSTOM_FIXED_STATS);
	stats = pgstat_get_custom_snapshot_data(PGSTAT_KIND_TEST_CUSTOM_FIXED_STATS);

	/* Build return tuple */
	tupdesc = CreateTemplateTupleDesc(2);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "numcalls",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "stats_reset",
					   TIMESTAMPTZOID, -1, 0);
	BlessTupleDesc(tupdesc);

	values[0] = Int64GetDatum(stats->numcalls);

	/* Handle uninitialized timestamp (no reset yet) */
	if (stats->stat_reset_timestamp == 0)
	{
		nulls[1] = true;
	}
	else
	{
		values[1] = TimestampTzGetDatum(stats->stat_reset_timestamp);
	}

	/* Return as tuple */
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}
