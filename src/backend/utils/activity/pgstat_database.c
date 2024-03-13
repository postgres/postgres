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
 * Copyright (c) 2001-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_database.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "storage/procsignal.h"
#include "utils/pgstat_internal.h"
#include "utils/timestamp.h"


static bool pgstat_should_report_connstat(void);


PgStat_Counter pgStatBlockReadTime = 0;
PgStat_Counter pgStatBlockWriteTime = 0;
PgStat_Counter pgStatActiveTime = 0;
PgStat_Counter pgStatTransactionIdleTime = 0;
SessionEndType pgStatSessionEndCause = DISCONNECT_NORMAL;


static int	pgStatXactCommit = 0;
static int	pgStatXactRollback = 0;
static PgStat_Counter pgLastSessionReportTime = 0;


/*
 * Remove entry for the database being dropped.
 */
void
pgstat_drop_database(Oid databaseid)
{
	pgstat_drop_transactional(PGSTAT_KIND_DATABASE, databaseid, InvalidOid);
}

/*
 * Called from autovacuum.c to report startup of an autovacuum process.
 * We are called before InitPostgres is done, so can't rely on MyDatabaseId;
 * the db OID must be passed in, instead.
 */
void
pgstat_report_autovac(Oid dboid)
{
	PgStat_EntryRef *entry_ref;
	PgStatShared_Database *dbentry;

	/* can't get here in single user mode */
	Assert(IsUnderPostmaster);

	/*
	 * End-of-vacuum is reported instantly. Report the start the same way for
	 * consistency. Vacuum doesn't run frequently and is a long-lasting
	 * operation so it doesn't matter if we get blocked here a little.
	 */
	entry_ref = pgstat_get_entry_ref_locked(PGSTAT_KIND_DATABASE,
											dboid, InvalidOid, false);

	dbentry = (PgStatShared_Database *) entry_ref->shared_stats;
	dbentry->stats.last_autovac_time = GetCurrentTimestamp();

	pgstat_unlock_entry(entry_ref);
}

/*
 * Report a Hot Standby recovery conflict.
 */
void
pgstat_report_recovery_conflict(int reason)
{
	PgStat_StatDBEntry *dbentry;

	Assert(IsUnderPostmaster);
	if (!pgstat_track_counts)
		return;

	dbentry = pgstat_prep_database_pending(MyDatabaseId);

	switch (reason)
	{
		case PROCSIG_RECOVERY_CONFLICT_DATABASE:

			/*
			 * Since we drop the information about the database as soon as it
			 * replicates, there is no point in counting these conflicts.
			 */
			break;
		case PROCSIG_RECOVERY_CONFLICT_TABLESPACE:
			dbentry->conflict_tablespace++;
			break;
		case PROCSIG_RECOVERY_CONFLICT_LOCK:
			dbentry->conflict_lock++;
			break;
		case PROCSIG_RECOVERY_CONFLICT_SNAPSHOT:
			dbentry->conflict_snapshot++;
			break;
		case PROCSIG_RECOVERY_CONFLICT_BUFFERPIN:
			dbentry->conflict_bufferpin++;
			break;
		case PROCSIG_RECOVERY_CONFLICT_LOGICALSLOT:
			dbentry->conflict_logicalslot++;
			break;
		case PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK:
			dbentry->conflict_startup_deadlock++;
			break;
	}
}

/*
 * Report a detected deadlock.
 */
void
pgstat_report_deadlock(void)
{
	PgStat_StatDBEntry *dbent;

	if (!pgstat_track_counts)
		return;

	dbent = pgstat_prep_database_pending(MyDatabaseId);
	dbent->deadlocks++;
}

/*
 * Report one or more checksum failures.
 */
void
pgstat_report_checksum_failures_in_db(Oid dboid, int failurecount)
{
	PgStat_EntryRef *entry_ref;
	PgStatShared_Database *sharedent;

	if (!pgstat_track_counts)
		return;

	/*
	 * Update the shared stats directly - checksum failures should never be
	 * common enough for that to be a problem.
	 */
	entry_ref =
		pgstat_get_entry_ref_locked(PGSTAT_KIND_DATABASE, dboid, InvalidOid, false);

	sharedent = (PgStatShared_Database *) entry_ref->shared_stats;
	sharedent->stats.checksum_failures += failurecount;
	sharedent->stats.last_checksum_failure = GetCurrentTimestamp();

	pgstat_unlock_entry(entry_ref);
}

/*
 * Report one checksum failure in the current database.
 */
void
pgstat_report_checksum_failure(void)
{
	pgstat_report_checksum_failures_in_db(MyDatabaseId, 1);
}

/*
 * Report creation of temporary file.
 */
void
pgstat_report_tempfile(size_t filesize)
{
	PgStat_StatDBEntry *dbent;

	if (!pgstat_track_counts)
		return;

	dbent = pgstat_prep_database_pending(MyDatabaseId);
	dbent->temp_bytes += filesize;
	dbent->temp_files++;
}

/*
 * Notify stats system of a new connection.
 */
void
pgstat_report_connect(Oid dboid)
{
	PgStat_StatDBEntry *dbentry;

	if (!pgstat_should_report_connstat())
		return;

	pgLastSessionReportTime = MyStartTimestamp;

	dbentry = pgstat_prep_database_pending(MyDatabaseId);
	dbentry->sessions++;
}

/*
 * Notify the stats system of a disconnect.
 */
void
pgstat_report_disconnect(Oid dboid)
{
	PgStat_StatDBEntry *dbentry;

	if (!pgstat_should_report_connstat())
		return;

	dbentry = pgstat_prep_database_pending(MyDatabaseId);

	switch (pgStatSessionEndCause)
	{
		case DISCONNECT_NOT_YET:
		case DISCONNECT_NORMAL:
			/* we don't collect these */
			break;
		case DISCONNECT_CLIENT_EOF:
			dbentry->sessions_abandoned++;
			break;
		case DISCONNECT_FATAL:
			dbentry->sessions_fatal++;
			break;
		case DISCONNECT_KILLED:
			dbentry->sessions_killed++;
			break;
	}
}

/*
 * Support function for the SQL-callable pgstat* functions. Returns
 * the collected statistics for one database or NULL. NULL doesn't mean
 * that the database doesn't exist, just that there are no statistics, so the
 * caller is better off to report ZERO instead.
 */
PgStat_StatDBEntry *
pgstat_fetch_stat_dbentry(Oid dboid)
{
	return (PgStat_StatDBEntry *)
		pgstat_fetch_entry(PGSTAT_KIND_DATABASE, dboid, InvalidOid);
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
 * Subroutine for pgstat_report_stat(): Handle xact commit/rollback and I/O
 * timings.
 */
void
pgstat_update_dbstats(TimestampTz ts)
{
	PgStat_StatDBEntry *dbentry;

	/*
	 * If not connected to a database yet, don't attribute time to "shared
	 * state" (InvalidOid is used to track stats for shared relations, etc.).
	 */
	if (!OidIsValid(MyDatabaseId))
		return;

	dbentry = pgstat_prep_database_pending(MyDatabaseId);

	/*
	 * Accumulate xact commit/rollback and I/O timings to stats entry of the
	 * current database.
	 */
	dbentry->xact_commit += pgStatXactCommit;
	dbentry->xact_rollback += pgStatXactRollback;
	dbentry->blk_read_time += pgStatBlockReadTime;
	dbentry->blk_write_time += pgStatBlockWriteTime;

	if (pgstat_should_report_connstat())
	{
		long		secs;
		int			usecs;

		/*
		 * pgLastSessionReportTime is initialized to MyStartTimestamp by
		 * pgstat_report_connect().
		 */
		TimestampDifference(pgLastSessionReportTime, ts, &secs, &usecs);
		pgLastSessionReportTime = ts;
		dbentry->session_time += (PgStat_Counter) secs * 1000000 + usecs;
		dbentry->active_time += pgStatActiveTime;
		dbentry->idle_in_transaction_time += pgStatTransactionIdleTime;
	}

	pgStatXactCommit = 0;
	pgStatXactRollback = 0;
	pgStatBlockReadTime = 0;
	pgStatBlockWriteTime = 0;
	pgStatActiveTime = 0;
	pgStatTransactionIdleTime = 0;
}

/*
 * We report session statistics only for normal backend processes.  Parallel
 * workers run in parallel, so they don't contribute to session times, even
 * though they use CPU time. Walsender processes could be considered here,
 * but they have different session characteristics from normal backends (for
 * example, they are always "active"), so they would skew session statistics.
 */
static bool
pgstat_should_report_connstat(void)
{
	return MyBackendType == B_BACKEND;
}

/*
 * Find or create a local PgStat_StatDBEntry entry for dboid.
 */
PgStat_StatDBEntry *
pgstat_prep_database_pending(Oid dboid)
{
	PgStat_EntryRef *entry_ref;

	/*
	 * This should not report stats on database objects before having
	 * connected to a database.
	 */
	Assert(!OidIsValid(dboid) || OidIsValid(MyDatabaseId));

	entry_ref = pgstat_prep_pending_entry(PGSTAT_KIND_DATABASE, dboid, InvalidOid,
										  NULL);

	return entry_ref->pending;
}

/*
 * Reset the database's reset timestamp, without resetting the contents of the
 * database stats.
 */
void
pgstat_reset_database_timestamp(Oid dboid, TimestampTz ts)
{
	PgStat_EntryRef *dbref;
	PgStatShared_Database *dbentry;

	dbref = pgstat_get_entry_ref_locked(PGSTAT_KIND_DATABASE, MyDatabaseId, InvalidOid,
										false);

	dbentry = (PgStatShared_Database *) dbref->shared_stats;
	dbentry->stats.stat_reset_timestamp = ts;

	pgstat_unlock_entry(dbref);
}

/*
 * Flush out pending stats for the entry
 *
 * If nowait is true, this function returns false if lock could not
 * immediately acquired, otherwise true is returned.
 */
bool
pgstat_database_flush_cb(PgStat_EntryRef *entry_ref, bool nowait)
{
	PgStatShared_Database *sharedent;
	PgStat_StatDBEntry *pendingent;

	pendingent = (PgStat_StatDBEntry *) entry_ref->pending;
	sharedent = (PgStatShared_Database *) entry_ref->shared_stats;

	if (!pgstat_lock_entry(entry_ref, nowait))
		return false;

#define PGSTAT_ACCUM_DBCOUNT(item)		\
	(sharedent)->stats.item += (pendingent)->item

	PGSTAT_ACCUM_DBCOUNT(xact_commit);
	PGSTAT_ACCUM_DBCOUNT(xact_rollback);
	PGSTAT_ACCUM_DBCOUNT(blocks_fetched);
	PGSTAT_ACCUM_DBCOUNT(blocks_hit);

	PGSTAT_ACCUM_DBCOUNT(tuples_returned);
	PGSTAT_ACCUM_DBCOUNT(tuples_fetched);
	PGSTAT_ACCUM_DBCOUNT(tuples_inserted);
	PGSTAT_ACCUM_DBCOUNT(tuples_updated);
	PGSTAT_ACCUM_DBCOUNT(tuples_deleted);

	/* last_autovac_time is reported immediately */
	Assert(pendingent->last_autovac_time == 0);

	PGSTAT_ACCUM_DBCOUNT(conflict_tablespace);
	PGSTAT_ACCUM_DBCOUNT(conflict_lock);
	PGSTAT_ACCUM_DBCOUNT(conflict_snapshot);
	PGSTAT_ACCUM_DBCOUNT(conflict_logicalslot);
	PGSTAT_ACCUM_DBCOUNT(conflict_bufferpin);
	PGSTAT_ACCUM_DBCOUNT(conflict_startup_deadlock);

	PGSTAT_ACCUM_DBCOUNT(temp_bytes);
	PGSTAT_ACCUM_DBCOUNT(temp_files);
	PGSTAT_ACCUM_DBCOUNT(deadlocks);

	/* checksum failures are reported immediately */
	Assert(pendingent->checksum_failures == 0);
	Assert(pendingent->last_checksum_failure == 0);

	PGSTAT_ACCUM_DBCOUNT(blk_read_time);
	PGSTAT_ACCUM_DBCOUNT(blk_write_time);

	PGSTAT_ACCUM_DBCOUNT(sessions);
	PGSTAT_ACCUM_DBCOUNT(session_time);
	PGSTAT_ACCUM_DBCOUNT(active_time);
	PGSTAT_ACCUM_DBCOUNT(idle_in_transaction_time);
	PGSTAT_ACCUM_DBCOUNT(sessions_abandoned);
	PGSTAT_ACCUM_DBCOUNT(sessions_fatal);
	PGSTAT_ACCUM_DBCOUNT(sessions_killed);
#undef PGSTAT_ACCUM_DBCOUNT

	pgstat_unlock_entry(entry_ref);

	memset(pendingent, 0, sizeof(*pendingent));

	return true;
}

void
pgstat_database_reset_timestamp_cb(PgStatShared_Common *header, TimestampTz ts)
{
	((PgStatShared_Database *) header)->stats.stat_reset_timestamp = ts;
}
