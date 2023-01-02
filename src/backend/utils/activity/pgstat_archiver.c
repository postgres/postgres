/* -------------------------------------------------------------------------
 *
 * pgstat_archiver.c
 *	  Implementation of archiver statistics.
 *
 * This file contains the implementation of archiver statistics. It is kept
 * separate from pgstat.c to enforce the line between the statistics access /
 * storage implementation and the details about individual types of
 * statistics.
 *
 * Copyright (c) 2001-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_archiver.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/pgstat_internal.h"
#include "utils/timestamp.h"


/*
 * Report archiver statistics
 */
void
pgstat_report_archiver(const char *xlog, bool failed)
{
	PgStatShared_Archiver *stats_shmem = &pgStatLocal.shmem->archiver;
	TimestampTz now = GetCurrentTimestamp();

	pgstat_begin_changecount_write(&stats_shmem->changecount);

	if (failed)
	{
		++stats_shmem->stats.failed_count;
		memcpy(&stats_shmem->stats.last_failed_wal, xlog,
			   sizeof(stats_shmem->stats.last_failed_wal));
		stats_shmem->stats.last_failed_timestamp = now;
	}
	else
	{
		++stats_shmem->stats.archived_count;
		memcpy(&stats_shmem->stats.last_archived_wal, xlog,
			   sizeof(stats_shmem->stats.last_archived_wal));
		stats_shmem->stats.last_archived_timestamp = now;
	}

	pgstat_end_changecount_write(&stats_shmem->changecount);
}

/*
 * Support function for the SQL-callable pgstat* functions. Returns
 * a pointer to the archiver statistics struct.
 */
PgStat_ArchiverStats *
pgstat_fetch_stat_archiver(void)
{
	pgstat_snapshot_fixed(PGSTAT_KIND_ARCHIVER);

	return &pgStatLocal.snapshot.archiver;
}

void
pgstat_archiver_reset_all_cb(TimestampTz ts)
{
	PgStatShared_Archiver *stats_shmem = &pgStatLocal.shmem->archiver;

	/* see explanation above PgStatShared_Archiver for the reset protocol */
	LWLockAcquire(&stats_shmem->lock, LW_EXCLUSIVE);
	pgstat_copy_changecounted_stats(&stats_shmem->reset_offset,
									&stats_shmem->stats,
									sizeof(stats_shmem->stats),
									&stats_shmem->changecount);
	stats_shmem->stats.stat_reset_timestamp = ts;
	LWLockRelease(&stats_shmem->lock);
}

void
pgstat_archiver_snapshot_cb(void)
{
	PgStatShared_Archiver *stats_shmem = &pgStatLocal.shmem->archiver;
	PgStat_ArchiverStats *stat_snap = &pgStatLocal.snapshot.archiver;
	PgStat_ArchiverStats *reset_offset = &stats_shmem->reset_offset;
	PgStat_ArchiverStats reset;

	pgstat_copy_changecounted_stats(stat_snap,
									&stats_shmem->stats,
									sizeof(stats_shmem->stats),
									&stats_shmem->changecount);

	LWLockAcquire(&stats_shmem->lock, LW_SHARED);
	memcpy(&reset, reset_offset, sizeof(stats_shmem->stats));
	LWLockRelease(&stats_shmem->lock);

	/* compensate by reset offsets */
	if (stat_snap->archived_count == reset.archived_count)
	{
		stat_snap->last_archived_wal[0] = 0;
		stat_snap->last_archived_timestamp = 0;
	}
	stat_snap->archived_count -= reset.archived_count;

	if (stat_snap->failed_count == reset.failed_count)
	{
		stat_snap->last_failed_wal[0] = 0;
		stat_snap->last_failed_timestamp = 0;
	}
	stat_snap->failed_count -= reset.failed_count;
}
