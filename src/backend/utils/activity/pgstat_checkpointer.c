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


/*
 * Checkpointer global statistics counters.  Stored directly in a stats
 * message structure so they can be sent without needing to copy things
 * around.  We assume this init to zeroes.
 */
PgStat_MsgCheckpointer PendingCheckpointerStats;


/* ----------
 * pgstat_send_checkpointer() -
 *
 *		Send checkpointer statistics to the collector
 * ----------
 */
void
pgstat_send_checkpointer(void)
{
	/* We assume this initializes to zeroes */
	static const PgStat_MsgCheckpointer all_zeroes;

	/*
	 * This function can be called even if nothing at all has happened. In
	 * this case, avoid sending a completely empty message to the stats
	 * collector.
	 */
	if (memcmp(&PendingCheckpointerStats, &all_zeroes, sizeof(PgStat_MsgCheckpointer)) == 0)
		return;

	/*
	 * Prepare and send the message
	 */
	pgstat_setheader(&PendingCheckpointerStats.m_hdr, PGSTAT_MTYPE_CHECKPOINTER);
	pgstat_send(&PendingCheckpointerStats, sizeof(PendingCheckpointerStats));

	/*
	 * Clear out the statistics buffer, so it can be re-used.
	 */
	MemSet(&PendingCheckpointerStats, 0, sizeof(PendingCheckpointerStats));
}
