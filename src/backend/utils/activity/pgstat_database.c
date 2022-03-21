/* -------------------------------------------------------------------------
 *
 * pgstat_database.c
 *	  Implementation of database statistics.
 *
 * This file contains the implementation of database statistics. It is kept
 * separate from pgstat.c to enforce the line between the statistics access /
 * storage implementation and the details about individual types of
 * statistics.
 *
 * Copyright (c) 2001-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_database.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/pgstat_internal.h"
#include "utils/timestamp.h"


static bool pgstat_should_report_connstat(void);


int			pgStatXactCommit = 0;
int			pgStatXactRollback = 0;
PgStat_Counter pgStatBlockReadTime = 0;
PgStat_Counter pgStatBlockWriteTime = 0;
PgStat_Counter pgStatActiveTime = 0;
PgStat_Counter pgStatTransactionIdleTime = 0;
SessionEndType pgStatSessionEndCause = DISCONNECT_NORMAL;


static PgStat_Counter pgLastSessionReportTime = 0;


/* ----------
 * pgstat_drop_database() -
 *
 *	Tell the collector that we just dropped a database.
 *	(If the message gets lost, we will still clean the dead DB eventually
 *	via future invocations of pgstat_vacuum_stat().)
 * ----------
 */
void
pgstat_drop_database(Oid databaseid)
{
	PgStat_MsgDropdb msg;

	if (pgStatSock == PGINVALID_SOCKET)
		return;

	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_DROPDB);
	msg.m_databaseid = databaseid;
	pgstat_send(&msg, sizeof(msg));
}

/* --------
 * pgstat_report_recovery_conflict() -
 *
 *	Tell the collector about a Hot Standby recovery conflict.
 * --------
 */
void
pgstat_report_recovery_conflict(int reason)
{
	PgStat_MsgRecoveryConflict msg;

	if (pgStatSock == PGINVALID_SOCKET || !pgstat_track_counts)
		return;

	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_RECOVERYCONFLICT);
	msg.m_databaseid = MyDatabaseId;
	msg.m_reason = reason;
	pgstat_send(&msg, sizeof(msg));
}

/* --------
 * pgstat_report_deadlock() -
 *
 *	Tell the collector about a deadlock detected.
 * --------
 */
void
pgstat_report_deadlock(void)
{
	PgStat_MsgDeadlock msg;

	if (pgStatSock == PGINVALID_SOCKET || !pgstat_track_counts)
		return;

	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_DEADLOCK);
	msg.m_databaseid = MyDatabaseId;
	pgstat_send(&msg, sizeof(msg));
}

/* --------
 * pgstat_report_checksum_failures_in_db() -
 *
 *	Tell the collector about one or more checksum failures.
 * --------
 */
void
pgstat_report_checksum_failures_in_db(Oid dboid, int failurecount)
{
	PgStat_MsgChecksumFailure msg;

	if (pgStatSock == PGINVALID_SOCKET || !pgstat_track_counts)
		return;

	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_CHECKSUMFAILURE);
	msg.m_databaseid = dboid;
	msg.m_failurecount = failurecount;
	msg.m_failure_time = GetCurrentTimestamp();

	pgstat_send(&msg, sizeof(msg));
}

/* --------
 * pgstat_report_checksum_failure() -
 *
 *	Tell the collector about a checksum failure.
 * --------
 */
void
pgstat_report_checksum_failure(void)
{
	pgstat_report_checksum_failures_in_db(MyDatabaseId, 1);
}

/* --------
 * pgstat_report_tempfile() -
 *
 *	Tell the collector about a temporary file.
 * --------
 */
void
pgstat_report_tempfile(size_t filesize)
{
	PgStat_MsgTempFile msg;

	if (pgStatSock == PGINVALID_SOCKET || !pgstat_track_counts)
		return;

	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_TEMPFILE);
	msg.m_databaseid = MyDatabaseId;
	msg.m_filesize = filesize;
	pgstat_send(&msg, sizeof(msg));
}

/* --------
 * pgstat_report_connect() -
 *
 *	Tell the collector about a new connection.
 * --------
 */
void
pgstat_report_connect(Oid dboid)
{
	PgStat_MsgConnect msg;

	if (!pgstat_should_report_connstat())
		return;

	pgLastSessionReportTime = MyStartTimestamp;

	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_CONNECT);
	msg.m_databaseid = MyDatabaseId;
	pgstat_send(&msg, sizeof(PgStat_MsgConnect));
}

/* --------
 * pgstat_report_disconnect() -
 *
 *	Tell the collector about a disconnect.
 * --------
 */
void
pgstat_report_disconnect(Oid dboid)
{
	PgStat_MsgDisconnect msg;

	if (!pgstat_should_report_connstat())
		return;

	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_DISCONNECT);
	msg.m_databaseid = MyDatabaseId;
	msg.m_cause = pgStatSessionEndCause;
	pgstat_send(&msg, sizeof(PgStat_MsgDisconnect));
}

void
AtEOXact_PgStat_Database(bool isCommit, bool parallel)
{
	/* Don't count parallel worker transaction stats */
	if (!parallel)
	{
		/*
		 * Count transaction commit or abort.  (We use counters, not just
		 * bools, in case the reporting message isn't sent right away.)
		 */
		if (isCommit)
			pgStatXactCommit++;
		else
			pgStatXactRollback++;
	}
}

/*
 * Subroutine for pgstat_send_tabstat: Handle xact commit/rollback and I/O
 * timings.
 */
void
pgstat_update_dbstats(PgStat_MsgTabstat *tsmsg, TimestampTz now)
{
	if (OidIsValid(tsmsg->m_databaseid))
	{
		tsmsg->m_xact_commit = pgStatXactCommit;
		tsmsg->m_xact_rollback = pgStatXactRollback;
		tsmsg->m_block_read_time = pgStatBlockReadTime;
		tsmsg->m_block_write_time = pgStatBlockWriteTime;

		if (pgstat_should_report_connstat())
		{
			long		secs;
			int			usecs;

			/*
			 * pgLastSessionReportTime is initialized to MyStartTimestamp by
			 * pgstat_report_connect().
			 */
			TimestampDifference(pgLastSessionReportTime, now, &secs, &usecs);
			pgLastSessionReportTime = now;
			tsmsg->m_session_time = (PgStat_Counter) secs * 1000000 + usecs;
			tsmsg->m_active_time = pgStatActiveTime;
			tsmsg->m_idle_in_xact_time = pgStatTransactionIdleTime;
		}
		else
		{
			tsmsg->m_session_time = 0;
			tsmsg->m_active_time = 0;
			tsmsg->m_idle_in_xact_time = 0;
		}
		pgStatXactCommit = 0;
		pgStatXactRollback = 0;
		pgStatBlockReadTime = 0;
		pgStatBlockWriteTime = 0;
		pgStatActiveTime = 0;
		pgStatTransactionIdleTime = 0;
	}
	else
	{
		tsmsg->m_xact_commit = 0;
		tsmsg->m_xact_rollback = 0;
		tsmsg->m_block_read_time = 0;
		tsmsg->m_block_write_time = 0;
		tsmsg->m_session_time = 0;
		tsmsg->m_active_time = 0;
		tsmsg->m_idle_in_xact_time = 0;
	}
}

/* --------
 * pgstat_should_report_connstats() -
 *
 *	We report session statistics only for normal backend processes.  Parallel
 *	workers run in parallel, so they don't contribute to session times, even
 *	though they use CPU time. Walsender processes could be considered here,
 *	but they have different session characteristics from normal backends (for
 *	example, they are always "active"), so they would skew session statistics.
 * ----------
 */
static bool
pgstat_should_report_connstat(void)
{
	return MyBackendType == B_BACKEND;
}
