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
 * Copyright (c) 2001-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_bgwriter.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/pgstat_internal.h"


/*
 * BgWriter global statistics counters.  Stored directly in a stats
 * message structure so they can be sent without needing to copy things
 * around.  We assume this init to zeroes.
 */
PgStat_MsgBgWriter PendingBgWriterStats;


/* ----------
 * pgstat_send_bgwriter() -
 *
 *		Send bgwriter statistics to the collector
 * ----------
 */
void
pgstat_send_bgwriter(void)
{
	/* We assume this initializes to zeroes */
	static const PgStat_MsgBgWriter all_zeroes;

	pgstat_assert_is_up();

	/*
	 * This function can be called even if nothing at all has happened. In
	 * this case, avoid sending a completely empty message to the stats
	 * collector.
	 */
	if (memcmp(&PendingBgWriterStats, &all_zeroes, sizeof(PgStat_MsgBgWriter)) == 0)
		return;

	/*
	 * Prepare and send the message
	 */
	pgstat_setheader(&PendingBgWriterStats.m_hdr, PGSTAT_MTYPE_BGWRITER);
	pgstat_send(&PendingBgWriterStats, sizeof(PendingBgWriterStats));

	/*
	 * Clear out the statistics buffer, so it can be re-used.
	 */
	MemSet(&PendingBgWriterStats, 0, sizeof(PendingBgWriterStats));
}
