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


/*
 * WAL global statistics counters.  Stored directly in a stats message
 * structure so they can be sent without needing to copy things around.  We
 * assume these init to zeroes.
 */
PgStat_MsgWal WalStats;


/*
 * WAL usage counters saved from pgWALUsage at the previous call to
 * pgstat_send_wal(). This is used to calculate how much WAL usage
 * happens between pgstat_send_wal() calls, by subtracting
 * the previous counters from the current ones.
 */
static WalUsage prevWalUsage;


/* ----------
 * pgstat_send_wal() -
 *
 *	Send WAL statistics to the collector.
 *
 * If 'force' is not set, WAL stats message is only sent if enough time has
 * passed since last one was sent to reach PGSTAT_STAT_INTERVAL.
 * ----------
 */
void
pgstat_send_wal(bool force)
{
	static TimestampTz sendTime = 0;

	/*
	 * This function can be called even if nothing at all has happened. In
	 * this case, avoid sending a completely empty message to the stats
	 * collector.
	 *
	 * Check wal_records counter to determine whether any WAL activity has
	 * happened since last time. Note that other WalUsage counters don't need
	 * to be checked because they are incremented always together with
	 * wal_records counter.
	 *
	 * m_wal_buffers_full also doesn't need to be checked because it's
	 * incremented only when at least one WAL record is generated (i.e.,
	 * wal_records counter is incremented). But for safely, we assert that
	 * m_wal_buffers_full is always zero when no WAL record is generated
	 *
	 * This function can be called by a process like walwriter that normally
	 * generates no WAL records. To determine whether any WAL activity has
	 * happened at that process since the last time, the numbers of WAL writes
	 * and syncs are also checked.
	 */
	if (pgWalUsage.wal_records == prevWalUsage.wal_records &&
		WalStats.m_wal_write == 0 && WalStats.m_wal_sync == 0)
	{
		Assert(WalStats.m_wal_buffers_full == 0);
		return;
	}

	if (!force)
	{
		TimestampTz now = GetCurrentTimestamp();

		/*
		 * Don't send a message unless it's been at least PGSTAT_STAT_INTERVAL
		 * msec since we last sent one to avoid overloading the stats
		 * collector.
		 */
		if (!TimestampDifferenceExceeds(sendTime, now, PGSTAT_STAT_INTERVAL))
			return;
		sendTime = now;
	}

	/*
	 * Set the counters related to generated WAL data if the counters were
	 * updated.
	 */
	if (pgWalUsage.wal_records != prevWalUsage.wal_records)
	{
		WalUsage	walusage;

		/*
		 * Calculate how much WAL usage counters were increased by subtracting
		 * the previous counters from the current ones. Fill the results in
		 * WAL stats message.
		 */
		MemSet(&walusage, 0, sizeof(WalUsage));
		WalUsageAccumDiff(&walusage, &pgWalUsage, &prevWalUsage);

		WalStats.m_wal_records = walusage.wal_records;
		WalStats.m_wal_fpi = walusage.wal_fpi;
		WalStats.m_wal_bytes = walusage.wal_bytes;

		/*
		 * Save the current counters for the subsequent calculation of WAL
		 * usage.
		 */
		prevWalUsage = pgWalUsage;
	}

	/*
	 * Prepare and send the message
	 */
	pgstat_setheader(&WalStats.m_hdr, PGSTAT_MTYPE_WAL);
	pgstat_send(&WalStats, sizeof(WalStats));

	/*
	 * Clear out the statistics buffer, so it can be re-used.
	 */
	MemSet(&WalStats, 0, sizeof(WalStats));
}

void
pgstat_wal_initialize(void)
{
	/*
	 * Initialize prevWalUsage with pgWalUsage so that pgstat_send_wal() can
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
pgstat_wal_pending(void)
{
	return pgWalUsage.wal_records != prevWalUsage.wal_records ||
		WalStats.m_wal_write != 0 ||
		WalStats.m_wal_sync != 0;
}
