/*--------------------------------------------------------------------------
 *
 * injection_stats_fixed.c
 *		Code for fixed-numbered statistics of injection points.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/test/modules/injection_points/injection_stats_fixed.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"

#include "common/hashfn.h"
#include "funcapi.h"
#include "injection_stats.h"
#include "pgstat.h"
#include "utils/builtins.h"
#include "utils/pgstat_internal.h"

/* Structures for statistics of injection points, fixed-size */
typedef struct PgStat_StatInjFixedEntry
{
	PgStat_Counter numattach;	/* number of points attached */
	PgStat_Counter numdetach;	/* number of points detached */
	PgStat_Counter numrun;		/* number of points run */
	PgStat_Counter numcached;	/* number of points cached */
	PgStat_Counter numloaded;	/* number of points loaded */
	TimestampTz stat_reset_timestamp;
} PgStat_StatInjFixedEntry;

typedef struct PgStatShared_InjectionPointFixed
{
	LWLock		lock;			/* protects all the counters */
	uint32		changecount;
	PgStat_StatInjFixedEntry stats;
	PgStat_StatInjFixedEntry reset_offset;
} PgStatShared_InjectionPointFixed;

/* Callbacks for fixed-numbered stats */
static void injection_stats_fixed_init_shmem_cb(void *stats);
static void injection_stats_fixed_reset_all_cb(TimestampTz ts);
static void injection_stats_fixed_snapshot_cb(void);

static const PgStat_KindInfo injection_stats_fixed = {
	.name = "injection_points_fixed",
	.fixed_amount = true,
	.write_to_file = true,

	.shared_size = sizeof(PgStat_StatInjFixedEntry),
	.shared_data_off = offsetof(PgStatShared_InjectionPointFixed, stats),
	.shared_data_len = sizeof(((PgStatShared_InjectionPointFixed *) 0)->stats),

	.init_shmem_cb = injection_stats_fixed_init_shmem_cb,
	.reset_all_cb = injection_stats_fixed_reset_all_cb,
	.snapshot_cb = injection_stats_fixed_snapshot_cb,
};

/*
 * Kind ID reserved for statistics of injection points.
 */
#define PGSTAT_KIND_INJECTION_FIXED	130

/* Track if fixed-numbered stats are loaded */
static bool inj_fixed_loaded = false;

static void
injection_stats_fixed_init_shmem_cb(void *stats)
{
	PgStatShared_InjectionPointFixed *stats_shmem =
		(PgStatShared_InjectionPointFixed *) stats;

	LWLockInitialize(&stats_shmem->lock, LWTRANCHE_PGSTATS_DATA);
}

static void
injection_stats_fixed_reset_all_cb(TimestampTz ts)
{
	PgStatShared_InjectionPointFixed *stats_shmem =
		pgstat_get_custom_shmem_data(PGSTAT_KIND_INJECTION_FIXED);

	LWLockAcquire(&stats_shmem->lock, LW_EXCLUSIVE);
	pgstat_copy_changecounted_stats(&stats_shmem->reset_offset,
									&stats_shmem->stats,
									sizeof(stats_shmem->stats),
									&stats_shmem->changecount);
	stats_shmem->stats.stat_reset_timestamp = ts;
	LWLockRelease(&stats_shmem->lock);
}

static void
injection_stats_fixed_snapshot_cb(void)
{
	PgStatShared_InjectionPointFixed *stats_shmem =
		pgstat_get_custom_shmem_data(PGSTAT_KIND_INJECTION_FIXED);
	PgStat_StatInjFixedEntry *stat_snap =
		pgstat_get_custom_snapshot_data(PGSTAT_KIND_INJECTION_FIXED);
	PgStat_StatInjFixedEntry *reset_offset = &stats_shmem->reset_offset;
	PgStat_StatInjFixedEntry reset;

	pgstat_copy_changecounted_stats(stat_snap,
									&stats_shmem->stats,
									sizeof(stats_shmem->stats),
									&stats_shmem->changecount);

	LWLockAcquire(&stats_shmem->lock, LW_SHARED);
	memcpy(&reset, reset_offset, sizeof(stats_shmem->stats));
	LWLockRelease(&stats_shmem->lock);

	/* compensate by reset offsets */
#define FIXED_COMP(fld) stat_snap->fld -= reset.fld;
	FIXED_COMP(numattach);
	FIXED_COMP(numdetach);
	FIXED_COMP(numrun);
	FIXED_COMP(numcached);
	FIXED_COMP(numloaded);
#undef FIXED_COMP
}

/*
 * Workhorse to do the registration work, called in _PG_init().
 */
void
pgstat_register_inj_fixed(void)
{
	pgstat_register_kind(PGSTAT_KIND_INJECTION_FIXED, &injection_stats_fixed);

	/* mark stats as loaded */
	inj_fixed_loaded = true;
}

/*
 * Report fixed number of statistics for an injection point.
 */
void
pgstat_report_inj_fixed(uint32 numattach,
						uint32 numdetach,
						uint32 numrun,
						uint32 numcached,
						uint32 numloaded)
{
	PgStatShared_InjectionPointFixed *stats_shmem;

	/* leave if disabled */
	if (!inj_fixed_loaded || !inj_stats_enabled)
		return;

	stats_shmem = pgstat_get_custom_shmem_data(PGSTAT_KIND_INJECTION_FIXED);

	pgstat_begin_changecount_write(&stats_shmem->changecount);
	stats_shmem->stats.numattach += numattach;
	stats_shmem->stats.numdetach += numdetach;
	stats_shmem->stats.numrun += numrun;
	stats_shmem->stats.numcached += numcached;
	stats_shmem->stats.numloaded += numloaded;
	pgstat_end_changecount_write(&stats_shmem->changecount);
}

/*
 * SQL function returning fixed-numbered statistics for injection points.
 */
PG_FUNCTION_INFO_V1(injection_points_stats_fixed);
Datum
injection_points_stats_fixed(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[5] = {0};
	bool		nulls[5] = {0};
	PgStat_StatInjFixedEntry *stats;

	if (!inj_fixed_loaded || !inj_stats_enabled)
		PG_RETURN_NULL();

	pgstat_snapshot_fixed(PGSTAT_KIND_INJECTION_FIXED);
	stats = pgstat_get_custom_snapshot_data(PGSTAT_KIND_INJECTION_FIXED);

	/* Initialise attributes information in the tuple descriptor */
	tupdesc = CreateTemplateTupleDesc(5);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "numattach",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "numdetach",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "numrun",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "numcached",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 5, "numloaded",
					   INT8OID, -1, 0);
	BlessTupleDesc(tupdesc);

	values[0] = Int64GetDatum(stats->numattach);
	values[1] = Int64GetDatum(stats->numdetach);
	values[2] = Int64GetDatum(stats->numrun);
	values[3] = Int64GetDatum(stats->numcached);
	values[4] = Int64GetDatum(stats->numloaded);
	nulls[0] = false;
	nulls[1] = false;
	nulls[2] = false;
	nulls[3] = false;
	nulls[4] = false;

	/* Returns the record as Datum */
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}
