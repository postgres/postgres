/* -------------------------------------------------------------------------
 *
 * pgstat_wal.c
 *	  Implementation of WAL statistics.
 *
 * This file contains the implementation of WAL statistics. It is kept
 * separate from pgstat.c to enforce the line between the statistics access /
 * storage implementation and the details about individual types of
 * statistics.
 *
 * Copyright (c) 2001-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_wal.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/pgstat_internal.h"
#include "executor/instrument.h"


PgStat_WalStats PendingWalStats = {0};

/*
 * WAL usage counters saved from pgWALUsage at the previous call to
 * pgstat_report_wal(). This is used to calculate how much WAL usage
 * happens between pgstat_report_wal() calls, by subtracting
 * the previous counters from the current ones.
 */
static WalUsage prevWalUsage;


/*
 * Calculate how much WAL usage counters have increased and update
 * shared statistics.
 *
 * Must be called by processes that generate WAL, that do not call
 * pgstat_report_stat(), like walwriter.
 */
void
pgstat_report_wal(bool force)
{
	pgstat_flush_wal(force);
}

/*
 * Support function for the SQL-callable pgstat* functions. Returns
 * a pointer to the WAL statistics struct.
 */
PgStat_WalStats *
pgstat_fetch_stat_wal(void)
{
	pgstat_snapshot_fixed(PGSTAT_KIND_WAL);

	return &pgStatLocal.snapshot.wal;
}

/*
 * Calculate how much WAL usage counters have increased by subtracting the
 * previous counters from the current ones.
 *
 * If nowait is true, this function returns true if the lock could not be
 * acquired. Otherwise return false.
 */
bool
pgstat_flush_wal(bool nowait)
{
	PgStatShared_Wal *stats_shmem = &pgStatLocal.shmem->wal;
	WalUsage	diff = {0};

	Assert(IsUnderPostmaster || !IsPostmasterEnvironment);
	Assert(pgStatLocal.shmem != NULL &&
		   !pgStatLocal.shmem->is_shutdown);

	/*
	 * This function can be called even if nothing at all has happened. Avoid
	 * taking lock for nothing in that case.
	 */
	if (!pgstat_have_pending_wal())
		return false;

	/*
	 * We don't update the WAL usage portion of the local WalStats elsewhere.
	 * Calculate how much WAL usage counters were increased by subtracting the
	 * previous counters from the current ones.
	 */
	WalUsageAccumDiff(&diff, &pgWalUsage, &prevWalUsage);
	PendingWalStats.wal_records = diff.wal_records;
	PendingWalStats.wal_fpi = diff.wal_fpi;
	PendingWalStats.wal_bytes = diff.wal_bytes;

	if (!nowait)
		LWLockAcquire(&stats_shmem->lock, LW_EXCLUSIVE);
	else if (!LWLockConditionalAcquire(&stats_shmem->lock, LW_EXCLUSIVE))
		return true;

#define WALSTAT_ACC(fld) stats_shmem->stats.fld += PendingWalStats.fld
	WALSTAT_ACC(wal_records);
	WALSTAT_ACC(wal_fpi);
	WALSTAT_ACC(wal_bytes);
	WALSTAT_ACC(wal_buffers_full);
	WALSTAT_ACC(wal_write);
	WALSTAT_ACC(wal_sync);
	WALSTAT_ACC(wal_write_time);
	WALSTAT_ACC(wal_sync_time);
#undef WALSTAT_ACC

	LWLockRelease(&stats_shmem->lock);

	/*
	 * Save the current counters for the subsequent calculation of WAL usage.
	 */
	prevWalUsage = pgWalUsage;

	/*
	 * Clear out the statistics buffer, so it can be re-used.
	 */
	MemSet(&PendingWalStats, 0, sizeof(PendingWalStats));

	return false;
}

void
pgstat_init_wal(void)
{
	/*
	 * Initialize prevWalUsage with pgWalUsage so that pgstat_flush_wal() can
	 * calculate how much pgWalUsage counters are increased by subtracting
	 * prevWalUsage from pgWalUsage.
	 */
	prevWalUsage = pgWalUsage;
}

/*
 * To determine whether any WAL activity has occurred since last time, not
 * only the number of generated WAL records but also the numbers of WAL
 * writes and syncs need to be checked. Because even transaction that
 * generates no WAL records can write or sync WAL data when flushing the
 * data pages.
 */
bool
pgstat_have_pending_wal(void)
{
	return pgWalUsage.wal_records != prevWalUsage.wal_records ||
		PendingWalStats.wal_write != 0 ||
		PendingWalStats.wal_sync != 0;
}

void
pgstat_wal_reset_all_cb(TimestampTz ts)
{
	PgStatShared_Wal *stats_shmem = &pgStatLocal.shmem->wal;

	LWLockAcquire(&stats_shmem->lock, LW_EXCLUSIVE);
	memset(&stats_shmem->stats, 0, sizeof(stats_shmem->stats));
	stats_shmem->stats.stat_reset_timestamp = ts;
	LWLockRelease(&stats_shmem->lock);
}

void
pgstat_wal_snapshot_cb(void)
{
	PgStatShared_Wal *stats_shmem = &pgStatLocal.shmem->wal;

	LWLockAcquire(&stats_shmem->lock, LW_SHARED);
	memcpy(&pgStatLocal.snapshot.wal, &stats_shmem->stats,
		   sizeof(pgStatLocal.snapshot.wal));
	LWLockRelease(&stats_shmem->lock);
}
