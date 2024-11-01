/* -------------------------------------------------------------------------
 *
 * pgstat_bgwriter.c
 *	  Implementation of bgwriter statistics.
 *
 * This file contains the implementation of bgwriter statistics. It is kept
 * separate from pgstat.c to enforce the line between the statistics access /
 * storage implementation and the details about individual types of
 * statistics.
 *
 * Copyright (c) 2001-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_bgwriter.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/memutils.h"
#include "utils/pgstat_internal.h"


PgStat_BgWriterStats PendingBgWriterStats = {0};


/*
 * Report bgwriter and IO statistics
 */
void
pgstat_report_bgwriter(void)
{
	PgStatShared_BgWriter *stats_shmem = &pgStatLocal.shmem->bgwriter;

	Assert(!pgStatLocal.shmem->is_shutdown);
	pgstat_assert_is_up();

	/*
	 * This function can be called even if nothing at all has happened. In
	 * this case, avoid unnecessarily modifying the stats entry.
	 */
	if (pg_memory_is_all_zeros(&PendingBgWriterStats,
							   sizeof(struct PgStat_BgWriterStats)))
		return;

	pgstat_begin_changecount_write(&stats_shmem->changecount);

#define BGWRITER_ACC(fld) stats_shmem->stats.fld += PendingBgWriterStats.fld
	BGWRITER_ACC(buf_written_clean);
	BGWRITER_ACC(maxwritten_clean);
	BGWRITER_ACC(buf_alloc);
#undef BGWRITER_ACC

	pgstat_end_changecount_write(&stats_shmem->changecount);

	/*
	 * Clear out the statistics buffer, so it can be re-used.
	 */
	MemSet(&PendingBgWriterStats, 0, sizeof(PendingBgWriterStats));

	/*
	 * Report IO statistics
	 */
	pgstat_flush_io(false);
}

/*
 * Support function for the SQL-callable pgstat* functions. Returns
 * a pointer to the bgwriter statistics struct.
 */
PgStat_BgWriterStats *
pgstat_fetch_stat_bgwriter(void)
{
	pgstat_snapshot_fixed(PGSTAT_KIND_BGWRITER);

	return &pgStatLocal.snapshot.bgwriter;
}

void
pgstat_bgwriter_init_shmem_cb(void *stats)
{
	PgStatShared_BgWriter *stats_shmem = (PgStatShared_BgWriter *) stats;

	LWLockInitialize(&stats_shmem->lock, LWTRANCHE_PGSTATS_DATA);
}

void
pgstat_bgwriter_reset_all_cb(TimestampTz ts)
{
	PgStatShared_BgWriter *stats_shmem = &pgStatLocal.shmem->bgwriter;

	/* see explanation above PgStatShared_BgWriter for the reset protocol */
	LWLockAcquire(&stats_shmem->lock, LW_EXCLUSIVE);
	pgstat_copy_changecounted_stats(&stats_shmem->reset_offset,
									&stats_shmem->stats,
									sizeof(stats_shmem->stats),
									&stats_shmem->changecount);
	stats_shmem->stats.stat_reset_timestamp = ts;
	LWLockRelease(&stats_shmem->lock);
}

void
pgstat_bgwriter_snapshot_cb(void)
{
	PgStatShared_BgWriter *stats_shmem = &pgStatLocal.shmem->bgwriter;
	PgStat_BgWriterStats *reset_offset = &stats_shmem->reset_offset;
	PgStat_BgWriterStats reset;

	pgstat_copy_changecounted_stats(&pgStatLocal.snapshot.bgwriter,
									&stats_shmem->stats,
									sizeof(stats_shmem->stats),
									&stats_shmem->changecount);

	LWLockAcquire(&stats_shmem->lock, LW_SHARED);
	memcpy(&reset, reset_offset, sizeof(stats_shmem->stats));
	LWLockRelease(&stats_shmem->lock);

	/* compensate by reset offsets */
#define BGWRITER_COMP(fld) pgStatLocal.snapshot.bgwriter.fld -= reset.fld;
	BGWRITER_COMP(buf_written_clean);
	BGWRITER_COMP(maxwritten_clean);
	BGWRITER_COMP(buf_alloc);
#undef BGWRITER_COMP
}
