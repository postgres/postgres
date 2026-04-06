/* -------------------------------------------------------------------------
 *
 * pgstat_lock.c
 *	  Implementation of lock statistics.
 *
 * This file contains the implementation of lock statistics.  It is kept
 * separate from pgstat.c to enforce the line between the statistics
 * access / storage implementation and the details about individual types
 * of statistics.
 *
 * Copyright (c) 2021-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_lock.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/pgstat_internal.h"

static PgStat_PendingLock PendingLockStats;
static bool have_lockstats = false;

PgStat_Lock *
pgstat_fetch_stat_lock(void)
{
	pgstat_snapshot_fixed(PGSTAT_KIND_LOCK);

	return &pgStatLocal.snapshot.lock;
}

/*
 * Simpler wrapper of pgstat_lock_flush_cb()
 */
void
pgstat_lock_flush(bool nowait)
{
	(void) pgstat_lock_flush_cb(nowait);
}

/*
 * Flush out locally pending lock statistics
 *
 * If no stats have been recorded, this function returns false.
 *
 * If nowait is true, this function returns true if the lock could not be
 * acquired. Otherwise, return false.
 */
bool
pgstat_lock_flush_cb(bool nowait)
{
	LWLock	   *lckstat_lock;
	PgStatShared_Lock *shstats;

	if (!have_lockstats)
		return false;

	shstats = &pgStatLocal.shmem->lock;
	lckstat_lock = &shstats->lock;

	if (!nowait)
		LWLockAcquire(lckstat_lock, LW_EXCLUSIVE);
	else if (!LWLockConditionalAcquire(lckstat_lock, LW_EXCLUSIVE))
		return true;

	for (int i = 0; i <= LOCKTAG_LAST_TYPE; i++)
	{
#define LOCKSTAT_ACC(fld) \
	(shstats->stats.stats[i].fld += PendingLockStats.stats[i].fld)
		LOCKSTAT_ACC(waits);
		LOCKSTAT_ACC(wait_time);
		LOCKSTAT_ACC(fastpath_exceeded);
#undef LOCKSTAT_ACC
	}

	LWLockRelease(lckstat_lock);

	memset(&PendingLockStats, 0, sizeof(PendingLockStats));
	have_lockstats = false;

	return false;
}

void
pgstat_lock_init_shmem_cb(void *stats)
{
	PgStatShared_Lock *stat_shmem = (PgStatShared_Lock *) stats;

	LWLockInitialize(&stat_shmem->lock, LWTRANCHE_PGSTATS_DATA);
}

void
pgstat_lock_reset_all_cb(TimestampTz ts)
{
	LWLock	   *lckstat_lock = &pgStatLocal.shmem->lock.lock;

	LWLockAcquire(lckstat_lock, LW_EXCLUSIVE);

	pgStatLocal.shmem->lock.stats.stat_reset_timestamp = ts;

	memset(pgStatLocal.shmem->lock.stats.stats, 0,
		   sizeof(pgStatLocal.shmem->lock.stats.stats));

	LWLockRelease(lckstat_lock);
}

void
pgstat_lock_snapshot_cb(void)
{
	LWLock	   *lckstat_lock = &pgStatLocal.shmem->lock.lock;

	LWLockAcquire(lckstat_lock, LW_SHARED);

	pgStatLocal.snapshot.lock = pgStatLocal.shmem->lock.stats;

	LWLockRelease(lckstat_lock);
}

/*
 * Increment counter for lock not acquired with the fast-path, per lock
 * type, due to the fast-path slot limit reached.
 *
 * Note: This function should not be called in performance-sensitive paths,
 * like lock acquisitions.
 */
void
pgstat_count_lock_fastpath_exceeded(uint8 locktag_type)
{
	Assert(locktag_type <= LOCKTAG_LAST_TYPE);
	PendingLockStats.stats[locktag_type].fastpath_exceeded++;
	have_lockstats = true;
	pgstat_report_fixed = true;
}

/*
 * Increment the number of waits and wait time, per lock type.
 *
 * Note: This function should not be called in performance-sensitive paths,
 * like lock acquisitions.
 */
void
pgstat_count_lock_waits(uint8 locktag_type, long msecs)
{
	Assert(locktag_type <= LOCKTAG_LAST_TYPE);
	PendingLockStats.stats[locktag_type].waits++;
	PendingLockStats.stats[locktag_type].wait_time += (PgStat_Counter) msecs;
	have_lockstats = true;
	pgstat_report_fixed = true;
}
