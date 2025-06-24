/*-------------------------------------------------------------------------
 * tablesync.c
 *	  PostgreSQL logical replication: initial table data synchronization
 *
 * Copyright (c) 2012-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/tablesync.c
 *
 * NOTES
 *	  This file contains code for initial table data synchronization for
 *	  logical replication.
 *
 *	  The initial data synchronization is done separately for each table,
 *	  in a separate apply worker that only fetches the initial snapshot data
 *	  from the publisher and then synchronizes the position in the stream with
 *	  the leader apply worker.
 *
 *	  There are several reasons for doing the synchronization this way:
 *	   - It allows us to parallelize the initial data synchronization
 *		 which lowers the time needed for it to happen.
 *	   - The initial synchronization does not have to hold the xid and LSN
 *		 for the time it takes to copy data of all tables, causing less
 *		 bloat and lower disk consumption compared to doing the
 *		 synchronization in a single process for the whole database.
 *	   - It allows us to synchronize any tables added after the initial
 *		 synchronization has finished.
 *
 *	  The stream position synchronization works in multiple steps:
 *	   - Apply worker requests a tablesync worker to start, setting the new
 *		 table state to INIT.
 *	   - Tablesync worker starts; changes table state from INIT to DATASYNC while
 *		 copying.
 *	   - Tablesync worker does initial table copy; there is a FINISHEDCOPY (sync
 *		 worker specific) state to indicate when the copy phase has completed, so
 *		 if the worker crashes with this (non-memory) state then the copy will not
 *		 be re-attempted.
 *	   - Tablesync worker then sets table state to SYNCWAIT; waits for state change.
 *	   - Apply worker periodically checks for tables in SYNCWAIT state.  When
 *		 any appear, it sets the table state to CATCHUP and starts loop-waiting
 *		 until either the table state is set to SYNCDONE or the sync worker
 *		 exits.
 *	   - After the sync worker has seen the state change to CATCHUP, it will
 *		 read the stream and apply changes (acting like an apply worker) until
 *		 it catches up to the specified stream position.  Then it sets the
 *		 state to SYNCDONE.  There might be zero changes applied between
 *		 CATCHUP and SYNCDONE, because the sync worker might be ahead of the
 *		 apply worker.
 *	   - Once the state is set to SYNCDONE, the apply will continue tracking
 *		 the table until it reaches the SYNCDONE stream position, at which
 *		 point it sets state to READY and stops tracking.  Again, there might
 *		 be zero changes in between.
 *
 *	  So the state progression is always: INIT -> DATASYNC -> FINISHEDCOPY
 *	  -> SYNCWAIT -> CATCHUP -> SYNCDONE -> READY.
 *
 *	  The catalog pg_subscription_rel is used to keep information about
 *	  subscribed tables and their state.  The catalog holds all states
 *	  except SYNCWAIT and CATCHUP which are only in shared memory.
 *
 *	  Example flows look like this:
 *	   - Apply is in front:
 *		  sync:8
 *			-> set in catalog FINISHEDCOPY
 *			-> set in memory SYNCWAIT
 *		  apply:10
 *			-> set in memory CATCHUP
 *			-> enter wait-loop
 *		  sync:10
 *			-> set in catalog SYNCDONE
 *			-> exit
 *		  apply:10
 *			-> exit wait-loop
 *			-> continue rep
 *		  apply:11
 *			-> set in catalog READY
 *
 *	   - Sync is in front:
 *		  sync:10
 *			-> set in catalog FINISHEDCOPY
 *			-> set in memory SYNCWAIT
 *		  apply:8
 *			-> set in memory CATCHUP
 *			-> continue per-table filtering
 *		  sync:10
 *			-> set in catalog SYNCDONE
 *			-> exit
 *		  apply:10
 *			-> set in catalog READY
 *			-> stop per-table filtering
 *			-> continue rep
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/table.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/pg_subscription_rel.h"
#include "catalog/pg_type.h"
#include "commands/copy.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_relation.h"
#include "pgstat.h"
#include "replication/logicallauncher.h"
#include "replication/logicalrelation.h"
#include "replication/logicalworker.h"
#include "replication/origin.h"
#include "replication/slot.h"
#include "replication/walreceiver.h"
#include "replication/worker_internal.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/usercontext.h"

typedef enum
{
	SYNC_TABLE_STATE_NEEDS_REBUILD,
	SYNC_TABLE_STATE_REBUILD_STARTED,
	SYNC_TABLE_STATE_VALID,
} SyncingTablesState;

static SyncingTablesState table_states_validity = SYNC_TABLE_STATE_NEEDS_REBUILD;
static List *table_states_not_ready = NIL;
static bool FetchTableStates(bool *started_tx);

static StringInfo copybuf = NULL;

/*
 * Exit routine for synchronization worker.
 */
pg_noreturn static void
finish_sync_worker(void)
{
	/*
	 * Commit any outstanding transaction. This is the usual case, unless
	 * there was nothing to do for the table.
	 */
	if (IsTransactionState())
	{
		CommitTransactionCommand();
		pgstat_report_stat(true);
	}

	/* And flush all writes. */
	XLogFlush(GetXLogWriteRecPtr());

	StartTransactionCommand();
	ereport(LOG,
			(errmsg("logical replication table synchronization worker for subscription \"%s\", table \"%s\" has finished",
					MySubscription->name,
					get_rel_name(MyLogicalRepWorker->relid))));
	CommitTransactionCommand();

	/* Find the leader apply worker and signal it. */
	logicalrep_worker_wakeup(MyLogicalRepWorker->subid, InvalidOid);

	/* Stop gracefully */
	proc_exit(0);
}

/*
 * Wait until the relation sync state is set in the catalog to the expected
 * one; return true when it happens.
 *
 * Returns false if the table sync worker or the table itself have
 * disappeared, or the table state has been reset.
 *
 * Currently, this is used in the apply worker when transitioning from
 * CATCHUP state to SYNCDONE.
 */
static bool
wait_for_relation_state_change(Oid relid, char expected_state)
{
	char		state;

	for (;;)
	{
		LogicalRepWorker *worker;
		XLogRecPtr	statelsn;

		CHECK_FOR_INTERRUPTS();

		InvalidateCatalogSnapshot();
		state = GetSubscriptionRelState(MyLogicalRepWorker->subid,
										relid, &statelsn);

		if (state == SUBREL_STATE_UNKNOWN)
			break;

		if (state == expected_state)
			return true;

		/* Check if the sync worker is still running and bail if not. */
		LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);
		worker = logicalrep_worker_find(MyLogicalRepWorker->subid, relid,
										false);
		LWLockRelease(LogicalRepWorkerLock);
		if (!worker)
			break;

		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 1000L, WAIT_EVENT_LOGICAL_SYNC_STATE_CHANGE);

		ResetLatch(MyLatch);
	}

	return false;
}

/*
 * Wait until the apply worker changes the state of our synchronization
 * worker to the expected one.
 *
 * Used when transitioning from SYNCWAIT state to CATCHUP.
 *
 * Returns false if the apply worker has disappeared.
 */
static bool
wait_for_worker_state_change(char expected_state)
{
	int			rc;

	for (;;)
	{
		LogicalRepWorker *worker;

		CHECK_FOR_INTERRUPTS();

		/*
		 * Done if already in correct state.  (We assume this fetch is atomic
		 * enough to not give a misleading answer if we do it with no lock.)
		 */
		if (MyLogicalRepWorker->relstate == expected_state)
			return true;

		/*
		 * Bail out if the apply worker has died, else signal it we're
		 * waiting.
		 */
		LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);
		worker = logicalrep_worker_find(MyLogicalRepWorker->subid,
										InvalidOid, false);
		if (worker && worker->proc)
			logicalrep_worker_wakeup_ptr(worker);
		LWLockRelease(LogicalRepWorkerLock);
		if (!worker)
			break;

		/*
		 * Wait.  We expect to get a latch signal back from the apply worker,
		 * but use a timeout in case it dies without sending one.
		 */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   1000L, WAIT_EVENT_LOGICAL_SYNC_STATE_CHANGE);

		if (rc & WL_LATCH_SET)
			ResetLatch(MyLatch);
	}

	return false;
}

/*
 * Callback from syscache invalidation.
 */
void
invalidate_syncing_table_states(Datum arg, int cacheid, uint32 hashvalue)
{
	table_states_validity = SYNC_TABLE_STATE_NEEDS_REBUILD;
}

/*
 * Handle table synchronization cooperation from the synchronization
 * worker.
 *
 * If the sync worker is in CATCHUP state and reached (or passed) the
 * predetermined synchronization point in the WAL stream, mark the table as
 * SYNCDONE and finish.
 */
static void
process_syncing_tables_for_sync(XLogRecPtr current_lsn)
{
	SpinLockAcquire(&MyLogicalRepWorker->relmutex);

	if (MyLogicalRepWorker->relstate == SUBREL_STATE_CATCHUP &&
		current_lsn >= MyLogicalRepWorker->relstate_lsn)
	{
		TimeLineID	tli;
		char		syncslotname[NAMEDATALEN] = {0};
		char		originname[NAMEDATALEN] = {0};

		MyLogicalRepWorker->relstate = SUBREL_STATE_SYNCDONE;
		MyLogicalRepWorker->relstate_lsn = current_lsn;

		SpinLockRelease(&MyLogicalRepWorker->relmutex);

		/*
		 * UpdateSubscriptionRelState must be called within a transaction.
		 */
		if (!IsTransactionState())
			StartTransactionCommand();

		UpdateSubscriptionRelState(MyLogicalRepWorker->subid,
								   MyLogicalRepWorker->relid,
								   MyLogicalRepWorker->relstate,
								   MyLogicalRepWorker->relstate_lsn);

		/*
		 * End streaming so that LogRepWorkerWalRcvConn can be used to drop
		 * the slot.
		 */
		walrcv_endstreaming(LogRepWorkerWalRcvConn, &tli);

		/*
		 * Cleanup the tablesync slot.
		 *
		 * This has to be done after updating the state because otherwise if
		 * there is an error while doing the database operations we won't be
		 * able to rollback dropped slot.
		 */
		ReplicationSlotNameForTablesync(MyLogicalRepWorker->subid,
										MyLogicalRepWorker->relid,
										syncslotname,
										sizeof(syncslotname));

		/*
		 * It is important to give an error if we are unable to drop the slot,
		 * otherwise, it won't be dropped till the corresponding subscription
		 * is dropped. So passing missing_ok = false.
		 */
		ReplicationSlotDropAtPubNode(LogRepWorkerWalRcvConn, syncslotname, false);

		CommitTransactionCommand();
		pgstat_report_stat(false);

		/*
		 * Start a new transaction to clean up the tablesync origin tracking.
		 * This transaction will be ended within the finish_sync_worker().
		 * Now, even, if we fail to remove this here, the apply worker will
		 * ensure to clean it up afterward.
		 *
		 * We need to do this after the table state is set to SYNCDONE.
		 * Otherwise, if an error occurs while performing the database
		 * operation, the worker will be restarted and the in-memory state of
		 * replication progress (remote_lsn) won't be rolled-back which would
		 * have been cleared before restart. So, the restarted worker will use
		 * invalid replication progress state resulting in replay of
		 * transactions that have already been applied.
		 */
		StartTransactionCommand();

		ReplicationOriginNameForLogicalRep(MyLogicalRepWorker->subid,
										   MyLogicalRepWorker->relid,
										   originname,
										   sizeof(originname));

		/*
		 * Resetting the origin session removes the ownership of the slot.
		 * This is needed to allow the origin to be dropped.
		 */
		replorigin_session_reset();
		replorigin_session_origin = InvalidRepOriginId;
		replorigin_session_origin_lsn = InvalidXLogRecPtr;
		replorigin_session_origin_timestamp = 0;

		/*
		 * Drop the tablesync's origin tracking if exists.
		 *
		 * There is a chance that the user is concurrently performing refresh
		 * for the subscription where we remove the table state and its origin
		 * or the apply worker would have removed this origin. So passing
		 * missing_ok = true.
		 */
		replorigin_drop_by_name(originname, true, false);

		finish_sync_worker();
	}
	else
		SpinLockRelease(&MyLogicalRepWorker->relmutex);
}

/*
 * Handle table synchronization cooperation from the apply worker.
 *
 * Walk over all subscription tables that are individually tracked by the
 * apply process (currently, all that have state other than
 * SUBREL_STATE_READY) and manage synchronization for them.
 *
 * If there are tables that need synchronizing and are not being synchronized
 * yet, start sync workers for them (if there are free slots for sync
 * workers).  To prevent starting the sync worker for the same relation at a
 * high frequency after a failure, we store its last start time with each sync
 * state info.  We start the sync worker for the same relation after waiting
 * at least wal_retrieve_retry_interval.
 *
 * For tables that are being synchronized already, check if sync workers
 * either need action from the apply worker or have finished.  This is the
 * SYNCWAIT to CATCHUP transition.
 *
 * If the synchronization position is reached (SYNCDONE), then the table can
 * be marked as READY and is no longer tracked.
 */
static void
process_syncing_tables_for_apply(XLogRecPtr current_lsn)
{
	struct tablesync_start_time_mapping
	{
		Oid			relid;
		TimestampTz last_start_time;
	};
	static HTAB *last_start_times = NULL;
	ListCell   *lc;
	bool		started_tx = false;
	bool		should_exit = false;

	Assert(!IsTransactionState());

	/* We need up-to-date sync state info for subscription tables here. */
	FetchTableStates(&started_tx);

	/*
	 * Prepare a hash table for tracking last start times of workers, to avoid
	 * immediate restarts.  We don't need it if there are no tables that need
	 * syncing.
	 */
	if (table_states_not_ready != NIL && !last_start_times)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(struct tablesync_start_time_mapping);
		last_start_times = hash_create("Logical replication table sync worker start times",
									   256, &ctl, HASH_ELEM | HASH_BLOBS);
	}

	/*
	 * Clean up the hash table when we're done with all tables (just to
	 * release the bit of memory).
	 */
	else if (table_states_not_ready == NIL && last_start_times)
	{
		hash_destroy(last_start_times);
		last_start_times = NULL;
	}

	/*
	 * Process all tables that are being synchronized.
	 */
	foreach(lc, table_states_not_ready)
	{
		SubscriptionRelState *rstate = (SubscriptionRelState *) lfirst(lc);

		if (rstate->state == SUBREL_STATE_SYNCDONE)
		{
			/*
			 * Apply has caught up to the position where the table sync has
			 * finished.  Mark the table as ready so that the apply will just
			 * continue to replicate it normally.
			 */
			if (current_lsn >= rstate->lsn)
			{
				char		originname[NAMEDATALEN];

				rstate->state = SUBREL_STATE_READY;
				rstate->lsn = current_lsn;
				if (!started_tx)
				{
					StartTransactionCommand();
					started_tx = true;
				}

				/*
				 * Remove the tablesync origin tracking if exists.
				 *
				 * There is a chance that the user is concurrently performing
				 * refresh for the subscription where we remove the table
				 * state and its origin or the tablesync worker would have
				 * already removed this origin. We can't rely on tablesync
				 * worker to remove the origin tracking as if there is any
				 * error while dropping we won't restart it to drop the
				 * origin. So passing missing_ok = true.
				 */
				ReplicationOriginNameForLogicalRep(MyLogicalRepWorker->subid,
												   rstate->relid,
												   originname,
												   sizeof(originname));
				replorigin_drop_by_name(originname, true, false);

				/*
				 * Update the state to READY only after the origin cleanup.
				 */
				UpdateSubscriptionRelState(MyLogicalRepWorker->subid,
										   rstate->relid, rstate->state,
										   rstate->lsn);
			}
		}
		else
		{
			LogicalRepWorker *syncworker;

			/*
			 * Look for a sync worker for this relation.
			 */
			LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

			syncworker = logicalrep_worker_find(MyLogicalRepWorker->subid,
												rstate->relid, false);

			if (syncworker)
			{
				/* Found one, update our copy of its state */
				SpinLockAcquire(&syncworker->relmutex);
				rstate->state = syncworker->relstate;
				rstate->lsn = syncworker->relstate_lsn;
				if (rstate->state == SUBREL_STATE_SYNCWAIT)
				{
					/*
					 * Sync worker is waiting for apply.  Tell sync worker it
					 * can catchup now.
					 */
					syncworker->relstate = SUBREL_STATE_CATCHUP;
					syncworker->relstate_lsn =
						Max(syncworker->relstate_lsn, current_lsn);
				}
				SpinLockRelease(&syncworker->relmutex);

				/* If we told worker to catch up, wait for it. */
				if (rstate->state == SUBREL_STATE_SYNCWAIT)
				{
					/* Signal the sync worker, as it may be waiting for us. */
					if (syncworker->proc)
						logicalrep_worker_wakeup_ptr(syncworker);

					/* Now safe to release the LWLock */
					LWLockRelease(LogicalRepWorkerLock);

					if (started_tx)
					{
						/*
						 * We must commit the existing transaction to release
						 * the existing locks before entering a busy loop.
						 * This is required to avoid any undetected deadlocks
						 * due to any existing lock as deadlock detector won't
						 * be able to detect the waits on the latch.
						 */
						CommitTransactionCommand();
						pgstat_report_stat(false);
					}

					/*
					 * Enter busy loop and wait for synchronization worker to
					 * reach expected state (or die trying).
					 */
					StartTransactionCommand();
					started_tx = true;

					wait_for_relation_state_change(rstate->relid,
												   SUBREL_STATE_SYNCDONE);
				}
				else
					LWLockRelease(LogicalRepWorkerLock);
			}
			else
			{
				/*
				 * If there is no sync worker for this table yet, count
				 * running sync workers for this subscription, while we have
				 * the lock.
				 */
				int			nsyncworkers =
					logicalrep_sync_worker_count(MyLogicalRepWorker->subid);

				/* Now safe to release the LWLock */
				LWLockRelease(LogicalRepWorkerLock);

				/*
				 * If there are free sync worker slot(s), start a new sync
				 * worker for the table.
				 */
				if (nsyncworkers < max_sync_workers_per_subscription)
				{
					TimestampTz now = GetCurrentTimestamp();
					struct tablesync_start_time_mapping *hentry;
					bool		found;

					hentry = hash_search(last_start_times, &rstate->relid,
										 HASH_ENTER, &found);

					if (!found ||
						TimestampDifferenceExceeds(hentry->last_start_time, now,
												   wal_retrieve_retry_interval))
					{
						/*
						 * Set the last_start_time even if we fail to start
						 * the worker, so that we won't retry until
						 * wal_retrieve_retry_interval has elapsed.
						 */
						hentry->last_start_time = now;
						(void) logicalrep_worker_launch(WORKERTYPE_TABLESYNC,
														MyLogicalRepWorker->dbid,
														MySubscription->oid,
														MySubscription->name,
														MyLogicalRepWorker->userid,
														rstate->relid,
														DSM_HANDLE_INVALID);
					}
				}
			}
		}
	}

	if (started_tx)
	{
		/*
		 * Even when the two_phase mode is requested by the user, it remains
		 * as 'pending' until all tablesyncs have reached READY state.
		 *
		 * When this happens, we restart the apply worker and (if the
		 * conditions are still ok) then the two_phase tri-state will become
		 * 'enabled' at that time.
		 *
		 * Note: If the subscription has no tables then leave the state as
		 * PENDING, which allows ALTER SUBSCRIPTION ... REFRESH PUBLICATION to
		 * work.
		 */
		if (MySubscription->twophasestate == LOGICALREP_TWOPHASE_STATE_PENDING)
		{
			CommandCounterIncrement();	/* make updates visible */
			if (AllTablesyncsReady())
			{
				ereport(LOG,
						(errmsg("logical replication apply worker for subscription \"%s\" will restart so that two_phase can be enabled",
								MySubscription->name)));
				should_exit = true;
			}
		}

		CommitTransactionCommand();
		pgstat_report_stat(true);
	}

	if (should_exit)
	{
		/*
		 * Reset the last-start time for this worker so that the launcher will
		 * restart it without waiting for wal_retrieve_retry_interval.
		 */
		ApplyLauncherForgetWorkerStartTime(MySubscription->oid);

		proc_exit(0);
	}
}

/*
 * Process possible state change(s) of tables that are being synchronized.
 */
void
process_syncing_tables(XLogRecPtr current_lsn)
{
	switch (MyLogicalRepWorker->type)
	{
		case WORKERTYPE_PARALLEL_APPLY:

			/*
			 * Skip for parallel apply workers because they only operate on
			 * tables that are in a READY state. See pa_can_start() and
			 * should_apply_changes_for_rel().
			 */
			break;

		case WORKERTYPE_TABLESYNC:
			process_syncing_tables_for_sync(current_lsn);
			break;

		case WORKERTYPE_APPLY:
			process_syncing_tables_for_apply(current_lsn);
			break;

		case WORKERTYPE_UNKNOWN:
			/* Should never happen. */
			elog(ERROR, "Unknown worker type");
	}
}

/*
 * Create list of columns for COPY based on logical relation mapping.
 */
static List *
make_copy_attnamelist(LogicalRepRelMapEntry *rel)
{
	List	   *attnamelist = NIL;
	int			i;

	for (i = 0; i < rel->remoterel.natts; i++)
	{
		attnamelist = lappend(attnamelist,
							  makeString(rel->remoterel.attnames[i]));
	}


	return attnamelist;
}

/*
 * Data source callback for the COPY FROM, which reads from the remote
 * connection and passes the data back to our local COPY.
 */
static int
copy_read_data(void *outbuf, int minread, int maxread)
{
	int			bytesread = 0;
	int			avail;

	/* If there are some leftover data from previous read, use it. */
	avail = copybuf->len - copybuf->cursor;
	if (avail)
	{
		if (avail > maxread)
			avail = maxread;
		memcpy(outbuf, &copybuf->data[copybuf->cursor], avail);
		copybuf->cursor += avail;
		maxread -= avail;
		bytesread += avail;
	}

	while (maxread > 0 && bytesread < minread)
	{
		pgsocket	fd = PGINVALID_SOCKET;
		int			len;
		char	   *buf = NULL;

		for (;;)
		{
			/* Try read the data. */
			len = walrcv_receive(LogRepWorkerWalRcvConn, &buf, &fd);

			CHECK_FOR_INTERRUPTS();

			if (len == 0)
				break;
			else if (len < 0)
				return bytesread;
			else
			{
				/* Process the data */
				copybuf->data = buf;
				copybuf->len = len;
				copybuf->cursor = 0;

				avail = copybuf->len - copybuf->cursor;
				if (avail > maxread)
					avail = maxread;
				memcpy(outbuf, &copybuf->data[copybuf->cursor], avail);
				outbuf = (char *) outbuf + avail;
				copybuf->cursor += avail;
				maxread -= avail;
				bytesread += avail;
			}

			if (maxread <= 0 || bytesread >= minread)
				return bytesread;
		}

		/*
		 * Wait for more data or latch.
		 */
		(void) WaitLatchOrSocket(MyLatch,
								 WL_SOCKET_READABLE | WL_LATCH_SET |
								 WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
								 fd, 1000L, WAIT_EVENT_LOGICAL_SYNC_DATA);

		ResetLatch(MyLatch);
	}

	return bytesread;
}


/*
 * Get information about remote relation in similar fashion the RELATION
 * message provides during replication.
 *
 * This function also returns (a) the relation qualifications to be used in
 * the COPY command, and (b) whether the remote relation has published any
 * generated column.
 */
static void
fetch_remote_table_info(char *nspname, char *relname, LogicalRepRelation *lrel,
						List **qual, bool *gencol_published)
{
	WalRcvExecResult *res;
	StringInfoData cmd;
	TupleTableSlot *slot;
	Oid			tableRow[] = {OIDOID, CHAROID, CHAROID};
	Oid			attrRow[] = {INT2OID, TEXTOID, OIDOID, BOOLOID, BOOLOID};
	Oid			qualRow[] = {TEXTOID};
	bool		isnull;
	int			natt;
	StringInfo	pub_names = NULL;
	Bitmapset  *included_cols = NULL;
	int			server_version = walrcv_server_version(LogRepWorkerWalRcvConn);

	lrel->nspname = nspname;
	lrel->relname = relname;

	/* First fetch Oid and replica identity. */
	initStringInfo(&cmd);
	appendStringInfo(&cmd, "SELECT c.oid, c.relreplident, c.relkind"
					 "  FROM pg_catalog.pg_class c"
					 "  INNER JOIN pg_catalog.pg_namespace n"
					 "        ON (c.relnamespace = n.oid)"
					 " WHERE n.nspname = %s"
					 "   AND c.relname = %s",
					 quote_literal_cstr(nspname),
					 quote_literal_cstr(relname));
	res = walrcv_exec(LogRepWorkerWalRcvConn, cmd.data,
					  lengthof(tableRow), tableRow);

	if (res->status != WALRCV_OK_TUPLES)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not fetch table info for table \"%s.%s\" from publisher: %s",
						nspname, relname, res->err)));

	slot = MakeSingleTupleTableSlot(res->tupledesc, &TTSOpsMinimalTuple);
	if (!tuplestore_gettupleslot(res->tuplestore, true, false, slot))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("table \"%s.%s\" not found on publisher",
						nspname, relname)));

	lrel->remoteid = DatumGetObjectId(slot_getattr(slot, 1, &isnull));
	Assert(!isnull);
	lrel->replident = DatumGetChar(slot_getattr(slot, 2, &isnull));
	Assert(!isnull);
	lrel->relkind = DatumGetChar(slot_getattr(slot, 3, &isnull));
	Assert(!isnull);

	ExecDropSingleTupleTableSlot(slot);
	walrcv_clear_result(res);


	/*
	 * Get column lists for each relation.
	 *
	 * We need to do this before fetching info about column names and types,
	 * so that we can skip columns that should not be replicated.
	 */
	if (server_version >= 150000)
	{
		WalRcvExecResult *pubres;
		TupleTableSlot *tslot;
		Oid			attrsRow[] = {INT2VECTOROID};

		/* Build the pub_names comma-separated string. */
		pub_names = makeStringInfo();
		GetPublicationsStr(MySubscription->publications, pub_names, true);

		/*
		 * Fetch info about column lists for the relation (from all the
		 * publications).
		 */
		resetStringInfo(&cmd);
		appendStringInfo(&cmd,
						 "SELECT DISTINCT"
						 "  (CASE WHEN (array_length(gpt.attrs, 1) = c.relnatts)"
						 "   THEN NULL ELSE gpt.attrs END)"
						 "  FROM pg_publication p,"
						 "  LATERAL pg_get_publication_tables(p.pubname) gpt,"
						 "  pg_class c"
						 " WHERE gpt.relid = %u AND c.oid = gpt.relid"
						 "   AND p.pubname IN ( %s )",
						 lrel->remoteid,
						 pub_names->data);

		pubres = walrcv_exec(LogRepWorkerWalRcvConn, cmd.data,
							 lengthof(attrsRow), attrsRow);

		if (pubres->status != WALRCV_OK_TUPLES)
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("could not fetch column list info for table \"%s.%s\" from publisher: %s",
							nspname, relname, pubres->err)));

		/*
		 * We don't support the case where the column list is different for
		 * the same table when combining publications. See comments atop
		 * fetch_table_list. So there should be only one row returned.
		 * Although we already checked this when creating the subscription, we
		 * still need to check here in case the column list was changed after
		 * creating the subscription and before the sync worker is started.
		 */
		if (tuplestore_tuple_count(pubres->tuplestore) > 1)
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot use different column lists for table \"%s.%s\" in different publications",
						   nspname, relname));

		/*
		 * Get the column list and build a single bitmap with the attnums.
		 *
		 * If we find a NULL value, it means all the columns should be
		 * replicated.
		 */
		tslot = MakeSingleTupleTableSlot(pubres->tupledesc, &TTSOpsMinimalTuple);
		if (tuplestore_gettupleslot(pubres->tuplestore, true, false, tslot))
		{
			Datum		cfval = slot_getattr(tslot, 1, &isnull);

			if (!isnull)
			{
				ArrayType  *arr;
				int			nelems;
				int16	   *elems;

				arr = DatumGetArrayTypeP(cfval);
				nelems = ARR_DIMS(arr)[0];
				elems = (int16 *) ARR_DATA_PTR(arr);

				for (natt = 0; natt < nelems; natt++)
					included_cols = bms_add_member(included_cols, elems[natt]);
			}

			ExecClearTuple(tslot);
		}
		ExecDropSingleTupleTableSlot(tslot);

		walrcv_clear_result(pubres);
	}

	/*
	 * Now fetch column names and types.
	 */
	resetStringInfo(&cmd);
	appendStringInfoString(&cmd,
						   "SELECT a.attnum,"
						   "       a.attname,"
						   "       a.atttypid,"
						   "       a.attnum = ANY(i.indkey)");

	/* Generated columns can be replicated since version 18. */
	if (server_version >= 180000)
		appendStringInfoString(&cmd, ", a.attgenerated != ''");

	appendStringInfo(&cmd,
					 "  FROM pg_catalog.pg_attribute a"
					 "  LEFT JOIN pg_catalog.pg_index i"
					 "       ON (i.indexrelid = pg_get_replica_identity_index(%u))"
					 " WHERE a.attnum > 0::pg_catalog.int2"
					 "   AND NOT a.attisdropped %s"
					 "   AND a.attrelid = %u"
					 " ORDER BY a.attnum",
					 lrel->remoteid,
					 (server_version >= 120000 && server_version < 180000 ?
					  "AND a.attgenerated = ''" : ""),
					 lrel->remoteid);
	res = walrcv_exec(LogRepWorkerWalRcvConn, cmd.data,
					  server_version >= 180000 ? lengthof(attrRow) : lengthof(attrRow) - 1, attrRow);

	if (res->status != WALRCV_OK_TUPLES)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not fetch table info for table \"%s.%s\" from publisher: %s",
						nspname, relname, res->err)));

	/* We don't know the number of rows coming, so allocate enough space. */
	lrel->attnames = palloc0(MaxTupleAttributeNumber * sizeof(char *));
	lrel->atttyps = palloc0(MaxTupleAttributeNumber * sizeof(Oid));
	lrel->attkeys = NULL;

	/*
	 * Store the columns as a list of names.  Ignore those that are not
	 * present in the column list, if there is one.
	 */
	natt = 0;
	slot = MakeSingleTupleTableSlot(res->tupledesc, &TTSOpsMinimalTuple);
	while (tuplestore_gettupleslot(res->tuplestore, true, false, slot))
	{
		char	   *rel_colname;
		AttrNumber	attnum;

		attnum = DatumGetInt16(slot_getattr(slot, 1, &isnull));
		Assert(!isnull);

		/* If the column is not in the column list, skip it. */
		if (included_cols != NULL && !bms_is_member(attnum, included_cols))
		{
			ExecClearTuple(slot);
			continue;
		}

		rel_colname = TextDatumGetCString(slot_getattr(slot, 2, &isnull));
		Assert(!isnull);

		lrel->attnames[natt] = rel_colname;
		lrel->atttyps[natt] = DatumGetObjectId(slot_getattr(slot, 3, &isnull));
		Assert(!isnull);

		if (DatumGetBool(slot_getattr(slot, 4, &isnull)))
			lrel->attkeys = bms_add_member(lrel->attkeys, natt);

		/* Remember if the remote table has published any generated column. */
		if (server_version >= 180000 && !(*gencol_published))
		{
			*gencol_published = DatumGetBool(slot_getattr(slot, 5, &isnull));
			Assert(!isnull);
		}

		/* Should never happen. */
		if (++natt >= MaxTupleAttributeNumber)
			elog(ERROR, "too many columns in remote table \"%s.%s\"",
				 nspname, relname);

		ExecClearTuple(slot);
	}
	ExecDropSingleTupleTableSlot(slot);

	lrel->natts = natt;

	walrcv_clear_result(res);

	/*
	 * Get relation's row filter expressions. DISTINCT avoids the same
	 * expression of a table in multiple publications from being included
	 * multiple times in the final expression.
	 *
	 * We need to copy the row even if it matches just one of the
	 * publications, so we later combine all the quals with OR.
	 *
	 * For initial synchronization, row filtering can be ignored in following
	 * cases:
	 *
	 * 1) one of the subscribed publications for the table hasn't specified
	 * any row filter
	 *
	 * 2) one of the subscribed publications has puballtables set to true
	 *
	 * 3) one of the subscribed publications is declared as TABLES IN SCHEMA
	 * that includes this relation
	 */
	if (server_version >= 150000)
	{
		/* Reuse the already-built pub_names. */
		Assert(pub_names != NULL);

		/* Check for row filters. */
		resetStringInfo(&cmd);
		appendStringInfo(&cmd,
						 "SELECT DISTINCT pg_get_expr(gpt.qual, gpt.relid)"
						 "  FROM pg_publication p,"
						 "  LATERAL pg_get_publication_tables(p.pubname) gpt"
						 " WHERE gpt.relid = %u"
						 "   AND p.pubname IN ( %s )",
						 lrel->remoteid,
						 pub_names->data);

		res = walrcv_exec(LogRepWorkerWalRcvConn, cmd.data, 1, qualRow);

		if (res->status != WALRCV_OK_TUPLES)
			ereport(ERROR,
					(errmsg("could not fetch table WHERE clause info for table \"%s.%s\" from publisher: %s",
							nspname, relname, res->err)));

		/*
		 * Multiple row filter expressions for the same table will be combined
		 * by COPY using OR. If any of the filter expressions for this table
		 * are null, it means the whole table will be copied. In this case it
		 * is not necessary to construct a unified row filter expression at
		 * all.
		 */
		slot = MakeSingleTupleTableSlot(res->tupledesc, &TTSOpsMinimalTuple);
		while (tuplestore_gettupleslot(res->tuplestore, true, false, slot))
		{
			Datum		rf = slot_getattr(slot, 1, &isnull);

			if (!isnull)
				*qual = lappend(*qual, makeString(TextDatumGetCString(rf)));
			else
			{
				/* Ignore filters and cleanup as necessary. */
				if (*qual)
				{
					list_free_deep(*qual);
					*qual = NIL;
				}
				break;
			}

			ExecClearTuple(slot);
		}
		ExecDropSingleTupleTableSlot(slot);

		walrcv_clear_result(res);
		destroyStringInfo(pub_names);
	}

	pfree(cmd.data);
}

/*
 * Copy existing data of a table from publisher.
 *
 * Caller is responsible for locking the local relation.
 */
static void
copy_table(Relation rel)
{
	LogicalRepRelMapEntry *relmapentry;
	LogicalRepRelation lrel;
	List	   *qual = NIL;
	WalRcvExecResult *res;
	StringInfoData cmd;
	CopyFromState cstate;
	List	   *attnamelist;
	ParseState *pstate;
	List	   *options = NIL;
	bool		gencol_published = false;

	/* Get the publisher relation info. */
	fetch_remote_table_info(get_namespace_name(RelationGetNamespace(rel)),
							RelationGetRelationName(rel), &lrel, &qual,
							&gencol_published);

	/* Put the relation into relmap. */
	logicalrep_relmap_update(&lrel);

	/* Map the publisher relation to local one. */
	relmapentry = logicalrep_rel_open(lrel.remoteid, NoLock);
	Assert(rel == relmapentry->localrel);

	/* Start copy on the publisher. */
	initStringInfo(&cmd);

	/* Regular table with no row filter or generated columns */
	if (lrel.relkind == RELKIND_RELATION && qual == NIL && !gencol_published)
	{
		appendStringInfo(&cmd, "COPY %s",
						 quote_qualified_identifier(lrel.nspname, lrel.relname));

		/* If the table has columns, then specify the columns */
		if (lrel.natts)
		{
			appendStringInfoString(&cmd, " (");

			/*
			 * XXX Do we need to list the columns in all cases? Maybe we're
			 * replicating all columns?
			 */
			for (int i = 0; i < lrel.natts; i++)
			{
				if (i > 0)
					appendStringInfoString(&cmd, ", ");

				appendStringInfoString(&cmd, quote_identifier(lrel.attnames[i]));
			}

			appendStringInfoChar(&cmd, ')');
		}

		appendStringInfoString(&cmd, " TO STDOUT");
	}
	else
	{
		/*
		 * For non-tables and tables with row filters, we need to do COPY
		 * (SELECT ...), but we can't just do SELECT * because we may need to
		 * copy only subset of columns including generated columns. For tables
		 * with any row filters, build a SELECT query with OR'ed row filters
		 * for COPY.
		 *
		 * We also need to use this same COPY (SELECT ...) syntax when
		 * generated columns are published, because copy of generated columns
		 * is not supported by the normal COPY.
		 */
		appendStringInfoString(&cmd, "COPY (SELECT ");
		for (int i = 0; i < lrel.natts; i++)
		{
			appendStringInfoString(&cmd, quote_identifier(lrel.attnames[i]));
			if (i < lrel.natts - 1)
				appendStringInfoString(&cmd, ", ");
		}

		appendStringInfoString(&cmd, " FROM ");

		/*
		 * For regular tables, make sure we don't copy data from a child that
		 * inherits the named table as those will be copied separately.
		 */
		if (lrel.relkind == RELKIND_RELATION)
			appendStringInfoString(&cmd, "ONLY ");

		appendStringInfoString(&cmd, quote_qualified_identifier(lrel.nspname, lrel.relname));
		/* list of OR'ed filters */
		if (qual != NIL)
		{
			ListCell   *lc;
			char	   *q = strVal(linitial(qual));

			appendStringInfo(&cmd, " WHERE %s", q);
			for_each_from(lc, qual, 1)
			{
				q = strVal(lfirst(lc));
				appendStringInfo(&cmd, " OR %s", q);
			}
			list_free_deep(qual);
		}

		appendStringInfoString(&cmd, ") TO STDOUT");
	}

	/*
	 * Prior to v16, initial table synchronization will use text format even
	 * if the binary option is enabled for a subscription.
	 */
	if (walrcv_server_version(LogRepWorkerWalRcvConn) >= 160000 &&
		MySubscription->binary)
	{
		appendStringInfoString(&cmd, " WITH (FORMAT binary)");
		options = list_make1(makeDefElem("format",
										 (Node *) makeString("binary"), -1));
	}

	res = walrcv_exec(LogRepWorkerWalRcvConn, cmd.data, 0, NULL);
	pfree(cmd.data);
	if (res->status != WALRCV_OK_COPY_OUT)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not start initial contents copy for table \"%s.%s\": %s",
						lrel.nspname, lrel.relname, res->err)));
	walrcv_clear_result(res);

	copybuf = makeStringInfo();

	pstate = make_parsestate(NULL);
	(void) addRangeTableEntryForRelation(pstate, rel, AccessShareLock,
										 NULL, false, false);

	attnamelist = make_copy_attnamelist(relmapentry);
	cstate = BeginCopyFrom(pstate, rel, NULL, NULL, false, copy_read_data, attnamelist, options);

	/* Do the copy */
	(void) CopyFrom(cstate);

	logicalrep_rel_close(relmapentry, NoLock);
}

/*
 * Determine the tablesync slot name.
 *
 * The name must not exceed NAMEDATALEN - 1 because of remote node constraints
 * on slot name length. We append system_identifier to avoid slot_name
 * collision with subscriptions in other clusters. With the current scheme
 * pg_%u_sync_%u_UINT64_FORMAT (3 + 10 + 6 + 10 + 20 + '\0'), the maximum
 * length of slot_name will be 50.
 *
 * The returned slot name is stored in the supplied buffer (syncslotname) with
 * the given size.
 *
 * Note: We don't use the subscription slot name as part of tablesync slot name
 * because we are responsible for cleaning up these slots and it could become
 * impossible to recalculate what name to cleanup if the subscription slot name
 * had changed.
 */
void
ReplicationSlotNameForTablesync(Oid suboid, Oid relid,
								char *syncslotname, Size szslot)
{
	snprintf(syncslotname, szslot, "pg_%u_sync_%u_" UINT64_FORMAT, suboid,
			 relid, GetSystemIdentifier());
}

/*
 * Start syncing the table in the sync worker.
 *
 * If nothing needs to be done to sync the table, we exit the worker without
 * any further action.
 *
 * The returned slot name is palloc'ed in current memory context.
 */
static char *
LogicalRepSyncTableStart(XLogRecPtr *origin_startpos)
{
	char	   *slotname;
	char	   *err;
	char		relstate;
	XLogRecPtr	relstate_lsn;
	Relation	rel;
	AclResult	aclresult;
	WalRcvExecResult *res;
	char		originname[NAMEDATALEN];
	RepOriginId originid;
	UserContext ucxt;
	bool		must_use_password;
	bool		run_as_owner;

	/* Check the state of the table synchronization. */
	StartTransactionCommand();
	relstate = GetSubscriptionRelState(MyLogicalRepWorker->subid,
									   MyLogicalRepWorker->relid,
									   &relstate_lsn);
	CommitTransactionCommand();

	/* Is the use of a password mandatory? */
	must_use_password = MySubscription->passwordrequired &&
		!MySubscription->ownersuperuser;

	SpinLockAcquire(&MyLogicalRepWorker->relmutex);
	MyLogicalRepWorker->relstate = relstate;
	MyLogicalRepWorker->relstate_lsn = relstate_lsn;
	SpinLockRelease(&MyLogicalRepWorker->relmutex);

	/*
	 * If synchronization is already done or no longer necessary, exit now
	 * that we've updated shared memory state.
	 */
	switch (relstate)
	{
		case SUBREL_STATE_SYNCDONE:
		case SUBREL_STATE_READY:
		case SUBREL_STATE_UNKNOWN:
			finish_sync_worker();	/* doesn't return */
	}

	/* Calculate the name of the tablesync slot. */
	slotname = (char *) palloc(NAMEDATALEN);
	ReplicationSlotNameForTablesync(MySubscription->oid,
									MyLogicalRepWorker->relid,
									slotname,
									NAMEDATALEN);

	/*
	 * Here we use the slot name instead of the subscription name as the
	 * application_name, so that it is different from the leader apply worker,
	 * so that synchronous replication can distinguish them.
	 */
	LogRepWorkerWalRcvConn =
		walrcv_connect(MySubscription->conninfo, true, true,
					   must_use_password,
					   slotname, &err);
	if (LogRepWorkerWalRcvConn == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("table synchronization worker for subscription \"%s\" could not connect to the publisher: %s",
						MySubscription->name, err)));

	Assert(MyLogicalRepWorker->relstate == SUBREL_STATE_INIT ||
		   MyLogicalRepWorker->relstate == SUBREL_STATE_DATASYNC ||
		   MyLogicalRepWorker->relstate == SUBREL_STATE_FINISHEDCOPY);

	/* Assign the origin tracking record name. */
	ReplicationOriginNameForLogicalRep(MySubscription->oid,
									   MyLogicalRepWorker->relid,
									   originname,
									   sizeof(originname));

	if (MyLogicalRepWorker->relstate == SUBREL_STATE_DATASYNC)
	{
		/*
		 * We have previously errored out before finishing the copy so the
		 * replication slot might exist. We want to remove the slot if it
		 * already exists and proceed.
		 *
		 * XXX We could also instead try to drop the slot, last time we failed
		 * but for that, we might need to clean up the copy state as it might
		 * be in the middle of fetching the rows. Also, if there is a network
		 * breakdown then it wouldn't have succeeded so trying it next time
		 * seems like a better bet.
		 */
		ReplicationSlotDropAtPubNode(LogRepWorkerWalRcvConn, slotname, true);
	}
	else if (MyLogicalRepWorker->relstate == SUBREL_STATE_FINISHEDCOPY)
	{
		/*
		 * The COPY phase was previously done, but tablesync then crashed
		 * before it was able to finish normally.
		 */
		StartTransactionCommand();

		/*
		 * The origin tracking name must already exist. It was created first
		 * time this tablesync was launched.
		 */
		originid = replorigin_by_name(originname, false);
		replorigin_session_setup(originid, 0);
		replorigin_session_origin = originid;
		*origin_startpos = replorigin_session_get_progress(false);

		CommitTransactionCommand();

		goto copy_table_done;
	}

	SpinLockAcquire(&MyLogicalRepWorker->relmutex);
	MyLogicalRepWorker->relstate = SUBREL_STATE_DATASYNC;
	MyLogicalRepWorker->relstate_lsn = InvalidXLogRecPtr;
	SpinLockRelease(&MyLogicalRepWorker->relmutex);

	/* Update the state and make it visible to others. */
	StartTransactionCommand();
	UpdateSubscriptionRelState(MyLogicalRepWorker->subid,
							   MyLogicalRepWorker->relid,
							   MyLogicalRepWorker->relstate,
							   MyLogicalRepWorker->relstate_lsn);
	CommitTransactionCommand();
	pgstat_report_stat(true);

	StartTransactionCommand();

	/*
	 * Use a standard write lock here. It might be better to disallow access
	 * to the table while it's being synchronized. But we don't want to block
	 * the main apply process from working and it has to open the relation in
	 * RowExclusiveLock when remapping remote relation id to local one.
	 */
	rel = table_open(MyLogicalRepWorker->relid, RowExclusiveLock);

	/*
	 * Start a transaction in the remote node in REPEATABLE READ mode.  This
	 * ensures that both the replication slot we create (see below) and the
	 * COPY are consistent with each other.
	 */
	res = walrcv_exec(LogRepWorkerWalRcvConn,
					  "BEGIN READ ONLY ISOLATION LEVEL REPEATABLE READ",
					  0, NULL);
	if (res->status != WALRCV_OK_COMMAND)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("table copy could not start transaction on publisher: %s",
						res->err)));
	walrcv_clear_result(res);

	/*
	 * Create a new permanent logical decoding slot. This slot will be used
	 * for the catchup phase after COPY is done, so tell it to use the
	 * snapshot to make the final data consistent.
	 */
	walrcv_create_slot(LogRepWorkerWalRcvConn,
					   slotname, false /* permanent */ , false /* two_phase */ ,
					   MySubscription->failover,
					   CRS_USE_SNAPSHOT, origin_startpos);

	/*
	 * Setup replication origin tracking. The purpose of doing this before the
	 * copy is to avoid doing the copy again due to any error in setting up
	 * origin tracking.
	 */
	originid = replorigin_by_name(originname, true);
	if (!OidIsValid(originid))
	{
		/*
		 * Origin tracking does not exist, so create it now.
		 *
		 * Then advance to the LSN got from walrcv_create_slot. This is WAL
		 * logged for the purpose of recovery. Locks are to prevent the
		 * replication origin from vanishing while advancing.
		 */
		originid = replorigin_create(originname);

		LockRelationOid(ReplicationOriginRelationId, RowExclusiveLock);
		replorigin_advance(originid, *origin_startpos, InvalidXLogRecPtr,
						   true /* go backward */ , true /* WAL log */ );
		UnlockRelationOid(ReplicationOriginRelationId, RowExclusiveLock);

		replorigin_session_setup(originid, 0);
		replorigin_session_origin = originid;
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("replication origin \"%s\" already exists",
						originname)));
	}

	/*
	 * Make sure that the copy command runs as the table owner, unless the
	 * user has opted out of that behaviour.
	 */
	run_as_owner = MySubscription->runasowner;
	if (!run_as_owner)
		SwitchToUntrustedUser(rel->rd_rel->relowner, &ucxt);

	/*
	 * Check that our table sync worker has permission to insert into the
	 * target table.
	 */
	aclresult = pg_class_aclcheck(RelationGetRelid(rel), GetUserId(),
								  ACL_INSERT);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult,
					   get_relkind_objtype(rel->rd_rel->relkind),
					   RelationGetRelationName(rel));

	/*
	 * COPY FROM does not honor RLS policies.  That is not a problem for
	 * subscriptions owned by roles with BYPASSRLS privilege (or superuser,
	 * who has it implicitly), but other roles should not be able to
	 * circumvent RLS.  Disallow logical replication into RLS enabled
	 * relations for such roles.
	 */
	if (check_enable_rls(RelationGetRelid(rel), InvalidOid, false) == RLS_ENABLED)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("user \"%s\" cannot replicate into relation with row-level security enabled: \"%s\"",
						GetUserNameFromId(GetUserId(), true),
						RelationGetRelationName(rel))));

	/* Now do the initial data copy */
	PushActiveSnapshot(GetTransactionSnapshot());
	copy_table(rel);
	PopActiveSnapshot();

	res = walrcv_exec(LogRepWorkerWalRcvConn, "COMMIT", 0, NULL);
	if (res->status != WALRCV_OK_COMMAND)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("table copy could not finish transaction on publisher: %s",
						res->err)));
	walrcv_clear_result(res);

	if (!run_as_owner)
		RestoreUserContext(&ucxt);

	table_close(rel, NoLock);

	/* Make the copy visible. */
	CommandCounterIncrement();

	/*
	 * Update the persisted state to indicate the COPY phase is done; make it
	 * visible to others.
	 */
	UpdateSubscriptionRelState(MyLogicalRepWorker->subid,
							   MyLogicalRepWorker->relid,
							   SUBREL_STATE_FINISHEDCOPY,
							   MyLogicalRepWorker->relstate_lsn);

	CommitTransactionCommand();

copy_table_done:

	elog(DEBUG1,
		 "LogicalRepSyncTableStart: '%s' origin_startpos lsn %X/%X",
		 originname, LSN_FORMAT_ARGS(*origin_startpos));

	/*
	 * We are done with the initial data synchronization, update the state.
	 */
	SpinLockAcquire(&MyLogicalRepWorker->relmutex);
	MyLogicalRepWorker->relstate = SUBREL_STATE_SYNCWAIT;
	MyLogicalRepWorker->relstate_lsn = *origin_startpos;
	SpinLockRelease(&MyLogicalRepWorker->relmutex);

	/*
	 * Finally, wait until the leader apply worker tells us to catch up and
	 * then return to let LogicalRepApplyLoop do it.
	 */
	wait_for_worker_state_change(SUBREL_STATE_CATCHUP);
	return slotname;
}

/*
 * Common code to fetch the up-to-date sync state info into the static lists.
 *
 * Returns true if subscription has 1 or more tables, else false.
 *
 * Note: If this function started the transaction (indicated by the parameter)
 * then it is the caller's responsibility to commit it.
 */
static bool
FetchTableStates(bool *started_tx)
{
	static bool has_subrels = false;

	*started_tx = false;

	if (table_states_validity != SYNC_TABLE_STATE_VALID)
	{
		MemoryContext oldctx;
		List	   *rstates;
		ListCell   *lc;
		SubscriptionRelState *rstate;

		table_states_validity = SYNC_TABLE_STATE_REBUILD_STARTED;

		/* Clean the old lists. */
		list_free_deep(table_states_not_ready);
		table_states_not_ready = NIL;

		if (!IsTransactionState())
		{
			StartTransactionCommand();
			*started_tx = true;
		}

		/* Fetch all non-ready tables. */
		rstates = GetSubscriptionRelations(MySubscription->oid, true);

		/* Allocate the tracking info in a permanent memory context. */
		oldctx = MemoryContextSwitchTo(CacheMemoryContext);
		foreach(lc, rstates)
		{
			rstate = palloc(sizeof(SubscriptionRelState));
			memcpy(rstate, lfirst(lc), sizeof(SubscriptionRelState));
			table_states_not_ready = lappend(table_states_not_ready, rstate);
		}
		MemoryContextSwitchTo(oldctx);

		/*
		 * Does the subscription have tables?
		 *
		 * If there were not-READY relations found then we know it does. But
		 * if table_states_not_ready was empty we still need to check again to
		 * see if there are 0 tables.
		 */
		has_subrels = (table_states_not_ready != NIL) ||
			HasSubscriptionRelations(MySubscription->oid);

		/*
		 * If the subscription relation cache has been invalidated since we
		 * entered this routine, we still use and return the relations we just
		 * finished constructing, to avoid infinite loops, but we leave the
		 * table states marked as stale so that we'll rebuild it again on next
		 * access. Otherwise, we mark the table states as valid.
		 */
		if (table_states_validity == SYNC_TABLE_STATE_REBUILD_STARTED)
			table_states_validity = SYNC_TABLE_STATE_VALID;
	}

	return has_subrels;
}

/*
 * Execute the initial sync with error handling. Disable the subscription,
 * if it's required.
 *
 * Allocate the slot name in long-lived context on return. Note that we don't
 * handle FATAL errors which are probably because of system resource error and
 * are not repeatable.
 */
static void
start_table_sync(XLogRecPtr *origin_startpos, char **slotname)
{
	char	   *sync_slotname = NULL;

	Assert(am_tablesync_worker());

	PG_TRY();
	{
		/* Call initial sync. */
		sync_slotname = LogicalRepSyncTableStart(origin_startpos);
	}
	PG_CATCH();
	{
		if (MySubscription->disableonerr)
			DisableSubscriptionAndExit();
		else
		{
			/*
			 * Report the worker failed during table synchronization. Abort
			 * the current transaction so that the stats message is sent in an
			 * idle state.
			 */
			AbortOutOfAnyTransaction();
			pgstat_report_subscription_error(MySubscription->oid, false);

			PG_RE_THROW();
		}
	}
	PG_END_TRY();

	/* allocate slot name in long-lived context */
	*slotname = MemoryContextStrdup(ApplyContext, sync_slotname);
	pfree(sync_slotname);
}

/*
 * Runs the tablesync worker.
 *
 * It starts syncing tables. After a successful sync, sets streaming options
 * and starts streaming to catchup with apply worker.
 */
static void
run_tablesync_worker()
{
	char		originname[NAMEDATALEN];
	XLogRecPtr	origin_startpos = InvalidXLogRecPtr;
	char	   *slotname = NULL;
	WalRcvStreamOptions options;

	start_table_sync(&origin_startpos, &slotname);

	ReplicationOriginNameForLogicalRep(MySubscription->oid,
									   MyLogicalRepWorker->relid,
									   originname,
									   sizeof(originname));

	set_apply_error_context_origin(originname);

	set_stream_options(&options, slotname, &origin_startpos);

	walrcv_startstreaming(LogRepWorkerWalRcvConn, &options);

	/* Apply the changes till we catchup with the apply worker. */
	start_apply(origin_startpos);
}

/* Logical Replication Tablesync worker entry point */
void
TablesyncWorkerMain(Datum main_arg)
{
	int			worker_slot = DatumGetInt32(main_arg);

	SetupApplyOrSyncWorker(worker_slot);

	run_tablesync_worker();

	finish_sync_worker();
}

/*
 * If the subscription has no tables then return false.
 *
 * Otherwise, are all tablesyncs READY?
 *
 * Note: This function is not suitable to be called from outside of apply or
 * tablesync workers because MySubscription needs to be already initialized.
 */
bool
AllTablesyncsReady(void)
{
	bool		started_tx = false;
	bool		has_subrels = false;

	/* We need up-to-date sync state info for subscription tables here. */
	has_subrels = FetchTableStates(&started_tx);

	if (started_tx)
	{
		CommitTransactionCommand();
		pgstat_report_stat(true);
	}

	/*
	 * Return false when there are no tables in subscription or not all tables
	 * are in ready state; true otherwise.
	 */
	return has_subrels && (table_states_not_ready == NIL);
}

/*
 * Update the two_phase state of the specified subscription in pg_subscription.
 */
void
UpdateTwoPhaseState(Oid suboid, char new_state)
{
	Relation	rel;
	HeapTuple	tup;
	bool		nulls[Natts_pg_subscription];
	bool		replaces[Natts_pg_subscription];
	Datum		values[Natts_pg_subscription];

	Assert(new_state == LOGICALREP_TWOPHASE_STATE_DISABLED ||
		   new_state == LOGICALREP_TWOPHASE_STATE_PENDING ||
		   new_state == LOGICALREP_TWOPHASE_STATE_ENABLED);

	rel = table_open(SubscriptionRelationId, RowExclusiveLock);
	tup = SearchSysCacheCopy1(SUBSCRIPTIONOID, ObjectIdGetDatum(suboid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR,
			 "cache lookup failed for subscription oid %u",
			 suboid);

	/* Form a new tuple. */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));

	/* And update/set two_phase state */
	values[Anum_pg_subscription_subtwophasestate - 1] = CharGetDatum(new_state);
	replaces[Anum_pg_subscription_subtwophasestate - 1] = true;

	tup = heap_modify_tuple(tup, RelationGetDescr(rel),
							values, nulls, replaces);
	CatalogTupleUpdate(rel, &tup->t_self, tup);

	heap_freetuple(tup);
	table_close(rel, RowExclusiveLock);
}
