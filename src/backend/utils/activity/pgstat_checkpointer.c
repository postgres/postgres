/* -------------------------------------------------------------------------
 *
 * pgstat_checkpointer.c
 *	  Implementation of checkpoint statistics.
 *
 * This file contains the implementation of checkpoint statistics. It is kept
 * separate from pgstat.c to enforce the line between the statistics access /
 * storage implementation and the details about individual types of
 * statistics.
 *
 * Copyright (c) 2001-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_checkpointer.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/memutils.h"
#include "utils/pgstat_internal.h"


PgStat_CheckpointerStats PendingCheckpointerStats = {0};


/*
 * Report checkpointer and IO statistics
 */
void
pgstat_report_checkpointer(void)
{
	PgStatShared_Checkpointer *stats_shmem = &pgStatLocal.shmem->checkpointer;

	Assert(!pgStatLocal.shmem->is_shutdown);
	pgstat_assert_is_up();

	/*
	 * This function can be called even if nothing at all has happened. In
	 * this case, avoid unnecessarily modifying the stats entry.
	 */
	if (pg_memory_is_all_zeros(&PendingCheckpointerStats,
							   sizeof(struct PgStat_CheckpointerStats)))
		return;

	pgstat_begin_changecount_write(&stats_shmem->changecount);

#define CHECKPOINTER_ACC(fld) stats_shmem->stats.fld += PendingCheckpointerStats.fld
	CHECKPOINTER_ACC(num_timed);
	CHECKPOINTER_ACC(num_requested);
	CHECKPOINTER_ACC(num_performed);
	CHECKPOINTER_ACC(restartpoints_timed);
	CHECKPOINTER_ACC(restartpoints_requested);
	CHECKPOINTER_ACC(restartpoints_performed);
	CHECKPOINTER_ACC(write_time);
	CHECKPOINTER_ACC(sync_time);
	CHECKPOINTER_ACC(buffers_written);
	CHECKPOINTER_ACC(slru_written);
#undef CHECKPOINTER_ACC

	pgstat_end_changecount_write(&stats_shmem->changecount);

	/*
	 * Clear out the statistics buffer, so it can be re-used.
	 */
	MemSet(&PendingCheckpointerStats, 0, sizeof(PendingCheckpointerStats));

	/*
	 * Report IO statistics
	 */
	pgstat_flush_io(false);
}

/*
 * pgstat_fetch_stat_checkpointer() -
 *
 * Support function for the SQL-callable pgstat* functions. Returns
 * a pointer to the checkpointer statistics struct.
 */
PgStat_CheckpointerStats *
pgstat_fetch_stat_checkpointer(void)
{
	pgstat_snapshot_fixed(PGSTAT_KIND_CHECKPOINTER);

	return &pgStatLocal.snapshot.checkpointer;
}

void
pgstat_checkpointer_init_shmem_cb(void *stats)
{
	PgStatShared_Checkpointer *stats_shmem = (PgStatShared_Checkpointer *) stats;

	LWLockInitialize(&stats_shmem->lock, LWTRANCHE_PGSTATS_DATA);
}

void
pgstat_checkpointer_reset_all_cb(TimestampTz ts)
{
	PgStatShared_Checkpointer *stats_shmem = &pgStatLocal.shmem->checkpointer;

	/* see explanation above PgStatShared_Checkpointer for the reset protocol */
	LWLockAcquire(&stats_shmem->lock, LW_EXCLUSIVE);
	pgstat_copy_changecounted_stats(&stats_shmem->reset_offset,
									&stats_shmem->stats,
									sizeof(stats_shmem->stats),
									&stats_shmem->changecount);
	stats_shmem->stats.stat_reset_timestamp = ts;
	LWLockRelease(&stats_shmem->lock);
}

void
pgstat_checkpointer_snapshot_cb(void)
{
	PgStatShared_Checkpointer *stats_shmem = &pgStatLocal.shmem->checkpointer;
	PgStat_CheckpointerStats *reset_offset = &stats_shmem->reset_offset;
	PgStat_CheckpointerStats reset;

	pgstat_copy_changecounted_stats(&pgStatLocal.snapshot.checkpointer,
									&stats_shmem->stats,
									sizeof(stats_shmem->stats),
									&stats_shmem->changecount);

	LWLockAcquire(&stats_shmem->lock, LW_SHARED);
	memcpy(&reset, reset_offset, sizeof(stats_shmem->stats));
	LWLockRelease(&stats_shmem->lock);

	/* compensate by reset offsets */
#define CHECKPOINTER_COMP(fld) pgStatLocal.snapshot.checkpointer.fld -= reset.fld;
	CHECKPOINTER_COMP(num_timed);
	CHECKPOINTER_COMP(num_requested);
	CHECKPOINTER_COMP(num_performed);
	CHECKPOINTER_COMP(restartpoints_timed);
	CHECKPOINTER_COMP(restartpoints_requested);
	CHECKPOINTER_COMP(restartpoints_performed);
	CHECKPOINTER_COMP(write_time);
	CHECKPOINTER_COMP(sync_time);
	CHECKPOINTER_COMP(buffers_written);
	CHECKPOINTER_COMP(slru_written);
#undef CHECKPOINTER_COMP
}
