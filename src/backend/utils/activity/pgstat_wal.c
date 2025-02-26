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
 * Copyright (c) 2001-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_wal.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/instrument.h"
#include "utils/pgstat_internal.h"


/*
 * WAL usage counters saved from pgWalUsage at the previous call to
 * pgstat_report_wal(). This is used to calculate how much WAL usage
 * happens between pgstat_report_wal() calls, by subtracting
 * the previous counters from the current ones.
 */
static WalUsage prevWalUsage;


/*
 * Calculate how much WAL usage counters have increased and update
 * shared WAL and IO statistics.
 *
 * Must be called by processes that generate WAL, that do not call
 * pgstat_report_stat(), like walwriter.
 *
 * "force" set to true ensures that the statistics are flushed; note that
 * this needs to acquire the pgstat shmem LWLock, waiting on it.  When
 * set to false, the statistics may not be flushed if the lock could not
 * be acquired.
 */
void
pgstat_report_wal(bool force)
{
	bool		nowait;

	/* like in pgstat.c, don't wait for lock acquisition when !force */
	nowait = !force;

	/* flush wal stats */
	(void) pgstat_wal_flush_cb(nowait);

	/* flush IO stats */
	pgstat_flush_io(nowait);
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
pgstat_wal_flush_cb(bool nowait)
{
	PgStatShared_Wal *stats_shmem = &pgStatLocal.shmem->wal;
	WalUsage	wal_usage_diff = {0};

	Assert(IsUnderPostmaster || !IsPostmasterEnvironment);
	Assert(pgStatLocal.shmem != NULL &&
		   !pgStatLocal.shmem->is_shutdown);

	/*
	 * This function can be called even if nothing at all has happened. Avoid
	 * taking lock for nothing in that case.
	 */
	if (!pgstat_wal_have_pending_cb())
		return false;

	/*
	 * We don't update the WAL usage portion of the local WalStats elsewhere.
	 * Calculate how much WAL usage counters were increased by subtracting the
	 * previous counters from the current ones.
	 */
	WalUsageAccumDiff(&wal_usage_diff, &pgWalUsage, &prevWalUsage);

	if (!nowait)
		LWLockAcquire(&stats_shmem->lock, LW_EXCLUSIVE);
	else if (!LWLockConditionalAcquire(&stats_shmem->lock, LW_EXCLUSIVE))
		return true;

#define WALSTAT_ACC(fld, var_to_add) \
	(stats_shmem->stats.wal_counters.fld += var_to_add.fld)
	WALSTAT_ACC(wal_records, wal_usage_diff);
	WALSTAT_ACC(wal_fpi, wal_usage_diff);
	WALSTAT_ACC(wal_bytes, wal_usage_diff);
	WALSTAT_ACC(wal_buffers_full, wal_usage_diff);
#undef WALSTAT_ACC

	LWLockRelease(&stats_shmem->lock);

	/*
	 * Save the current counters for the subsequent calculation of WAL usage.
	 */
	prevWalUsage = pgWalUsage;

	return false;
}

void
pgstat_wal_init_backend_cb(void)
{
	/*
	 * Initialize prevWalUsage with pgWalUsage so that pgstat_wal_flush_cb()
	 * can calculate how much pgWalUsage counters are increased by subtracting
	 * prevWalUsage from pgWalUsage.
	 */
	prevWalUsage = pgWalUsage;
}

/*
 * To determine whether WAL usage happened.
 */
bool
pgstat_wal_have_pending_cb(void)
{
	return pgWalUsage.wal_records != prevWalUsage.wal_records;
}

void
pgstat_wal_init_shmem_cb(void *stats)
{
	PgStatShared_Wal *stats_shmem = (PgStatShared_Wal *) stats;

	LWLockInitialize(&stats_shmem->lock, LWTRANCHE_PGSTATS_DATA);
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
