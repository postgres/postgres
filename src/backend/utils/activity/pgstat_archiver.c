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
 * Copyright (c) 2001-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_archiver.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/pgstat_internal.h"
#include "utils/timestamp.h"


/* ----------
 * pgstat_send_archiver() -
 *
 *	Tell the collector about the WAL file that we successfully
 *	archived or failed to archive.
 * ----------
 */
void
pgstat_send_archiver(const char *xlog, bool failed)
{
	PgStat_MsgArchiver msg;

	/*
	 * Prepare and send the message
	 */
	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_ARCHIVER);
	msg.m_failed = failed;
	strlcpy(msg.m_xlog, xlog, sizeof(msg.m_xlog));
	msg.m_timestamp = GetCurrentTimestamp();
	pgstat_send(&msg, sizeof(msg));
}
