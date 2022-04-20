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
 * Copyright (c) 2001-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_checkpointer.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/pgstat_internal.h"


PgStat_CheckpointerStats PendingCheckpointerStats = {0};


/*
 * Report checkpointer statistics
 */
void
pgstat_report_checkpointer(void)
{
	/* We assume this initializes to zeroes */
	static const PgStat_CheckpointerStats all_zeroes;
	PgStatShared_Checkpointer *stats_shmem = &pgStatLocal.shmem->checkpointer;

	Assert(!pgStatLocal.shmem->is_shutdown);
	pgstat_assert_is_up();

	/*
	 * This function can be called even if nothing at all has happened. In
	 * this case, avoid unnecessarily modifying the stats entry.
	 */
	if (memcmp(&PendingCheckpointerStats, &all_zeroes,
			   sizeof(all_zeroes)) == 0)
		return;

	pgstat_begin_changecount_write(&stats_shmem->changecount);

#define CHECKPOINTER_ACC(fld) stats_shmem->stats.fld += PendingCheckpointerStats.fld
	CHECKPOINTER_ACC(timed_checkpoints);
	CHECKPOINTER_ACC(requested_checkpoints);
	CHECKPOINTER_ACC(checkpoint_write_time);
	CHECKPOINTER_ACC(checkpoint_sync_time);
	CHECKPOINTER_ACC(buf_written_checkpoints);
	CHECKPOINTER_ACC(buf_written_backend);
	CHECKPOINTER_ACC(buf_fsync_backend);
#undef CHECKPOINTER_ACC

	pgstat_end_changecount_write(&stats_shmem->changecount);

	/*
	 * Clear out the statistics buffer, so it can be re-used.
	 */
	MemSet(&PendingCheckpointerStats, 0, sizeof(PendingCheckpointerStats));
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
pgstat_checkpointer_reset_all_cb(TimestampTz ts)
{
	PgStatShared_Checkpointer *stats_shmem = &pgStatLocal.shmem->checkpointer;

	/* see explanation above PgStatShared_Checkpointer for the reset protocol */
	LWLockAcquire(&stats_shmem->lock, LW_EXCLUSIVE);
	pgstat_copy_changecounted_stats(&stats_shmem->reset_offset,
									&stats_shmem->stats,
									sizeof(stats_shmem->stats),
									&stats_shmem->changecount);
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
	CHECKPOINTER_COMP(timed_checkpoints);
	CHECKPOINTER_COMP(requested_checkpoints);
	CHECKPOINTER_COMP(checkpoint_write_time);
	CHECKPOINTER_COMP(checkpoint_sync_time);
	CHECKPOINTER_COMP(buf_written_checkpoints);
	CHECKPOINTER_COMP(buf_written_backend);
	CHECKPOINTER_COMP(buf_fsync_backend);
#undef CHECKPOINTER_COMP
}
