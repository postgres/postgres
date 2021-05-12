/*-------------------------------------------------------------------------
 * tablesync.c
 *	  PostgreSQL logical replication
 *
 * Copyright (c) 2012-2020, PostgreSQL Global Development Group
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
 *	  the main apply worker.
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
 *	  The stream position synchronization works in multiple steps.
 *	   - Sync finishes copy and sets worker state as SYNCWAIT and waits for
 *		 state to change in a loop.
 *	   - Apply periodically checks tables that are synchronizing for SYNCWAIT.
 *		 When the desired state appears, it will set the worker state to
 *		 CATCHUP and starts loop-waiting until either the table state is set
 *		 to SYNCDONE or the sync worker exits.
 *	   - After the sync worker has seen the state change to CATCHUP, it will
 *		 read the stream and apply changes (acting like an apply worker) until
 *		 it catches up to the specified stream position.  Then it sets the
 *		 state to SYNCDONE.  There might be zero changes applied between
 *		 CATCHUP and SYNCDONE, because the sync worker might be ahead of the
 *		 apply worker.
 *	   - Once the state was set to SYNCDONE, the apply will continue tracking
 *		 the table until it reaches the SYNCDONE stream position, at which
 *		 point it sets state to READY and stops tracking.  Again, there might
 *		 be zero changes in between.
 *
 *	  So the state progression is always: INIT -> DATASYNC -> SYNCWAIT -> CATCHUP ->
 *	  SYNCDONE -> READY.
 *
 *	  The catalog pg_subscription_rel is used to keep information about
 *	  subscribed tables and their state.  Some transient state during data
 *	  synchronization is kept in shared memory.  The states SYNCWAIT and
 *	  CATCHUP only appear in memory.
 *
 *	  Example flows look like this:
 *	   - Apply is in front:
 *		  sync:8
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
 *	   - Sync in front:
 *		  sync:10
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
#include "catalog/pg_subscription_rel.h"
#include "catalog/pg_type.h"
#include "commands/copy.h"
#include "miscadmin.h"
#include "parser/parse_relation.h"
#include "pgstat.h"
#include "replication/logicallauncher.h"
#include "replication/logicalrelation.h"
#include "replication/walreceiver.h"
#include "replication/worker_internal.h"
#include "storage/ipc.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

static bool table_states_valid = false;

StringInfo	copybuf = NULL;

/*
 * Exit routine for synchronization worker.
 */
static void
pg_attribute_noreturn()
finish_sync_worker(void)
{
	/*
	 * Commit any outstanding transaction. This is the usual case, unless
	 * there was nothing to do for the table.
	 */
	if (IsTransactionState())
	{
		CommitTransactionCommand();
		pgstat_report_stat(false);
	}

	/* And flush all writes. */
	XLogFlush(GetXLogWriteRecPtr());

	StartTransactionCommand();
	ereport(LOG,
			(errmsg("logical replication table synchronization worker for subscription \"%s\", table \"%s\" has finished",
					MySubscription->name,
					get_rel_name(MyLogicalRepWorker->relid))));
	CommitTransactionCommand();

	/* Find the main apply worker and signal it. */
	logicalrep_worker_wakeup(MyLogicalRepWorker->subid, InvalidOid);

	/* Stop gracefully */
	proc_exit(0);
}

/*
 * Wait until the relation synchronization state is set in the catalog to the
 * expected one.
 *
 * Used when transitioning from CATCHUP state to SYNCDONE.
 *
 * Returns false if the synchronization worker has disappeared or the table state
 * has been reset.
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

		/* XXX use cache invalidation here to improve performance? */
		PushActiveSnapshot(GetLatestSnapshot());
		state = GetSubscriptionRelState(MyLogicalRepWorker->subid,
										relid, &statelsn, true);
		PopActiveSnapshot();

		if (state == SUBREL_STATE_UNKNOWN)
			return false;

		if (state == expected_state)
			return true;

		/* Check if the sync worker is still running and bail if not. */
		LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

		/* Check if the opposite worker is still running and bail if not. */
		worker = logicalrep_worker_find(MyLogicalRepWorker->subid,
										am_tablesync_worker() ? InvalidOid : relid,
										false);
		LWLockRelease(LogicalRepWorkerLock);
		if (!worker)
			return false;

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
	table_states_valid = false;
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
	Assert(IsTransactionState());

	SpinLockAcquire(&MyLogicalRepWorker->relmutex);

	if (MyLogicalRepWorker->relstate == SUBREL_STATE_CATCHUP &&
		current_lsn >= MyLogicalRepWorker->relstate_lsn)
	{
		TimeLineID	tli;

		MyLogicalRepWorker->relstate = SUBREL_STATE_SYNCDONE;
		MyLogicalRepWorker->relstate_lsn = current_lsn;

		SpinLockRelease(&MyLogicalRepWorker->relmutex);

		UpdateSubscriptionRelState(MyLogicalRepWorker->subid,
								   MyLogicalRepWorker->relid,
								   MyLogicalRepWorker->relstate,
								   MyLogicalRepWorker->relstate_lsn);

		walrcv_endstreaming(LogRepWorkerWalRcvConn, &tli);
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
	static List *table_states = NIL;
	static HTAB *last_start_times = NULL;
	ListCell   *lc;
	bool		started_tx = false;

	Assert(!IsTransactionState());

	/* We need up-to-date sync state info for subscription tables here. */
	if (!table_states_valid)
	{
		MemoryContext oldctx;
		List	   *rstates;
		ListCell   *lc;
		SubscriptionRelState *rstate;

		/* Clean the old list. */
		list_free_deep(table_states);
		table_states = NIL;

		StartTransactionCommand();
		started_tx = true;

		/* Fetch all non-ready tables. */
		rstates = GetSubscriptionNotReadyRelations(MySubscription->oid);

		/* Allocate the tracking info in a permanent memory context. */
		oldctx = MemoryContextSwitchTo(CacheMemoryContext);
		foreach(lc, rstates)
		{
			rstate = palloc(sizeof(SubscriptionRelState));
			memcpy(rstate, lfirst(lc), sizeof(SubscriptionRelState));
			table_states = lappend(table_states, rstate);
		}
		MemoryContextSwitchTo(oldctx);

		table_states_valid = true;
	}

	/*
	 * Prepare a hash table for tracking last start times of workers, to avoid
	 * immediate restarts.  We don't need it if there are no tables that need
	 * syncing.
	 */
	if (table_states && !last_start_times)
	{
		HASHCTL		ctl;

		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(struct tablesync_start_time_mapping);
		last_start_times = hash_create("Logical replication table sync worker start times",
									   256, &ctl, HASH_ELEM | HASH_BLOBS);
	}

	/*
	 * Clean up the hash table when we're done with all tables (just to
	 * release the bit of memory).
	 */
	else if (!table_states && last_start_times)
	{
		hash_destroy(last_start_times);
		last_start_times = NULL;
	}

	/*
	 * Process all tables that are being synchronized.
	 */
	foreach(lc, table_states)
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
				rstate->state = SUBREL_STATE_READY;
				rstate->lsn = current_lsn;
				if (!started_tx)
				{
					StartTransactionCommand();
					started_tx = true;
				}

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

					/*
					 * Enter busy loop and wait for synchronization worker to
					 * reach expected state (or die trying).
					 */
					if (!started_tx)
					{
						StartTransactionCommand();
						started_tx = true;
					}

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
						logicalrep_worker_launch(MyLogicalRepWorker->dbid,
												 MySubscription->oid,
												 MySubscription->name,
												 MyLogicalRepWorker->userid,
												 rstate->relid);
						hentry->last_start_time = now;
					}
				}
			}
		}
	}

	if (started_tx)
	{
		CommitTransactionCommand();
		pgstat_report_stat(false);
	}
}

/*
 * Process possible state change(s) of tables that are being synchronized.
 */
void
process_syncing_tables(XLogRecPtr current_lsn)
{
	if (am_tablesync_worker())
		process_syncing_tables_for_sync(current_lsn);
	else
		process_syncing_tables_for_apply(current_lsn);
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
				outbuf = (void *) ((char *) outbuf + avail);
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
 */
static void
fetch_remote_table_info(char *nspname, char *relname,
						LogicalRepRelation *lrel)
{
	WalRcvExecResult *res;
	StringInfoData cmd;
	TupleTableSlot *slot;
	Oid			tableRow[] = {OIDOID, CHAROID, CHAROID};
	Oid			attrRow[] = {TEXTOID, OIDOID, INT4OID, BOOLOID};
	bool		isnull;
	int			natt;

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
				(errmsg("could not fetch table info for table \"%s.%s\" from publisher: %s",
						nspname, relname, res->err)));

	slot = MakeSingleTupleTableSlot(res->tupledesc, &TTSOpsMinimalTuple);
	if (!tuplestore_gettupleslot(res->tuplestore, true, false, slot))
		ereport(ERROR,
				(errmsg("table \"%s.%s\" not found on publisher",
						nspname, relname)));

	lrel->remoteid = DatumGetObjectId(slot_getattr(slot, 1, &isnull));
	Assert(!isnull);
	lrel->replident = DatumGetChar(slot_getattr(slot, 2, &isnull));
	Assert(!isnull);
	lrel->relkind = DatumGetChar(slot_getattr(slot, 3, &isnull));
	Assert(!isnull);

	ExecDropSingleTupleTableSlot(slot);
	walrcv_clear_result(res);

	/* Now fetch columns. */
	resetStringInfo(&cmd);
	appendStringInfo(&cmd,
					 "SELECT a.attname,"
					 "       a.atttypid,"
					 "       a.atttypmod,"
					 "       a.attnum = ANY(i.indkey)"
					 "  FROM pg_catalog.pg_attribute a"
					 "  LEFT JOIN pg_catalog.pg_index i"
					 "       ON (i.indexrelid = pg_get_replica_identity_index(%u))"
					 " WHERE a.attnum > 0::pg_catalog.int2"
					 "   AND NOT a.attisdropped %s"
					 "   AND a.attrelid = %u"
					 " ORDER BY a.attnum",
					 lrel->remoteid,
					 (walrcv_server_version(LogRepWorkerWalRcvConn) >= 120000 ?
					  "AND a.attgenerated = ''" : ""),
					 lrel->remoteid);
	res = walrcv_exec(LogRepWorkerWalRcvConn, cmd.data,
					  lengthof(attrRow), attrRow);

	if (res->status != WALRCV_OK_TUPLES)
		ereport(ERROR,
				(errmsg("could not fetch table info for table \"%s.%s\": %s",
						nspname, relname, res->err)));

	/* We don't know the number of rows coming, so allocate enough space. */
	lrel->attnames = palloc0(MaxTupleAttributeNumber * sizeof(char *));
	lrel->atttyps = palloc0(MaxTupleAttributeNumber * sizeof(Oid));
	lrel->attkeys = NULL;

	natt = 0;
	slot = MakeSingleTupleTableSlot(res->tupledesc, &TTSOpsMinimalTuple);
	while (tuplestore_gettupleslot(res->tuplestore, true, false, slot))
	{
		lrel->attnames[natt] =
			TextDatumGetCString(slot_getattr(slot, 1, &isnull));
		Assert(!isnull);
		lrel->atttyps[natt] = DatumGetObjectId(slot_getattr(slot, 2, &isnull));
		Assert(!isnull);
		if (DatumGetBool(slot_getattr(slot, 4, &isnull)))
			lrel->attkeys = bms_add_member(lrel->attkeys, natt);

		/* Should never happen. */
		if (++natt >= MaxTupleAttributeNumber)
			elog(ERROR, "too many columns in remote table \"%s.%s\"",
				 nspname, relname);

		ExecClearTuple(slot);
	}
	ExecDropSingleTupleTableSlot(slot);

	lrel->natts = natt;

	walrcv_clear_result(res);
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
	WalRcvExecResult *res;
	StringInfoData cmd;
	CopyState	cstate;
	List	   *attnamelist;
	ParseState *pstate;

	/* Get the publisher relation info. */
	fetch_remote_table_info(get_namespace_name(RelationGetNamespace(rel)),
							RelationGetRelationName(rel), &lrel);

	/* Put the relation into relmap. */
	logicalrep_relmap_update(&lrel);

	/* Map the publisher relation to local one. */
	relmapentry = logicalrep_rel_open(lrel.remoteid, NoLock);
	Assert(rel == relmapentry->localrel);

	/* Start copy on the publisher. */
	initStringInfo(&cmd);
	if (lrel.relkind == RELKIND_RELATION)
		appendStringInfo(&cmd, "COPY %s TO STDOUT",
						 quote_qualified_identifier(lrel.nspname, lrel.relname));
	else
	{
		/*
		 * For non-tables, we need to do COPY (SELECT ...), but we can't just
		 * do SELECT * because we need to not copy generated columns.
		 */
		appendStringInfo(&cmd, "COPY (SELECT ");
		for (int i = 0; i < lrel.natts; i++)
		{
			appendStringInfoString(&cmd, quote_identifier(lrel.attnames[i]));
			if (i < lrel.natts - 1)
				appendStringInfoString(&cmd, ", ");
		}
		appendStringInfo(&cmd, " FROM %s) TO STDOUT",
						 quote_qualified_identifier(lrel.nspname, lrel.relname));
	}
	res = walrcv_exec(LogRepWorkerWalRcvConn, cmd.data, 0, NULL);
	pfree(cmd.data);
	if (res->status != WALRCV_OK_COPY_OUT)
		ereport(ERROR,
				(errmsg("could not start initial contents copy for table \"%s.%s\": %s",
						lrel.nspname, lrel.relname, res->err)));
	walrcv_clear_result(res);

	copybuf = makeStringInfo();

	pstate = make_parsestate(NULL);
	(void) addRangeTableEntryForRelation(pstate, rel, AccessShareLock,
										 NULL, false, false);

	attnamelist = make_copy_attnamelist(relmapentry);
	cstate = BeginCopyFrom(pstate, rel, NULL, false, copy_read_data, attnamelist, NIL);

	/* Do the copy */
	(void) CopyFrom(cstate);

	logicalrep_rel_close(relmapentry, NoLock);
}

/*
 * Start syncing the table in the sync worker.
 *
 * The returned slot name is palloc'ed in current memory context.
 */
char *
LogicalRepSyncTableStart(XLogRecPtr *origin_startpos)
{
	char	   *slotname;
	char	   *err;
	char		relstate;
	XLogRecPtr	relstate_lsn;

	/* Check the state of the table synchronization. */
	StartTransactionCommand();
	relstate = GetSubscriptionRelState(MyLogicalRepWorker->subid,
									   MyLogicalRepWorker->relid,
									   &relstate_lsn, true);
	CommitTransactionCommand();

	SpinLockAcquire(&MyLogicalRepWorker->relmutex);
	MyLogicalRepWorker->relstate = relstate;
	MyLogicalRepWorker->relstate_lsn = relstate_lsn;
	SpinLockRelease(&MyLogicalRepWorker->relmutex);

	/*
	 * To build a slot name for the sync work, we are limited to NAMEDATALEN -
	 * 1 characters.  We cut the original slot name to NAMEDATALEN - 28 chars
	 * and append _%u_sync_%u (1 + 10 + 6 + 10 + '\0').  (It's actually the
	 * NAMEDATALEN on the remote that matters, but this scheme will also work
	 * reasonably if that is different.)
	 */
	StaticAssertStmt(NAMEDATALEN >= 32, "NAMEDATALEN too small");	/* for sanity */
	slotname = psprintf("%.*s_%u_sync_%u",
						NAMEDATALEN - 28,
						MySubscription->slotname,
						MySubscription->oid,
						MyLogicalRepWorker->relid);

	/*
	 * Here we use the slot name instead of the subscription name as the
	 * application_name, so that it is different from the main apply worker,
	 * so that synchronous replication can distinguish them.
	 */
	LogRepWorkerWalRcvConn = walrcv_connect(MySubscription->conninfo, true,
											slotname, &err);
	if (LogRepWorkerWalRcvConn == NULL)
		ereport(ERROR,
				(errmsg("could not connect to the publisher: %s", err)));

	switch (MyLogicalRepWorker->relstate)
	{
		case SUBREL_STATE_INIT:
		case SUBREL_STATE_DATASYNC:
			{
				Relation	rel;
				WalRcvExecResult *res;

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
				pgstat_report_stat(false);

				/*
				 * We want to do the table data sync in a single transaction.
				 */
				StartTransactionCommand();

				/*
				 * Use a standard write lock here. It might be better to
				 * disallow access to the table while it's being synchronized.
				 * But we don't want to block the main apply process from
				 * working and it has to open the relation in RowExclusiveLock
				 * when remapping remote relation id to local one.
				 */
				rel = table_open(MyLogicalRepWorker->relid, RowExclusiveLock);

				/*
				 * Create a temporary slot for the sync process. We do this
				 * inside the transaction so that we can use the snapshot made
				 * by the slot to get existing data.
				 */
				res = walrcv_exec(LogRepWorkerWalRcvConn,
								  "BEGIN READ ONLY ISOLATION LEVEL "
								  "REPEATABLE READ", 0, NULL);
				if (res->status != WALRCV_OK_COMMAND)
					ereport(ERROR,
							(errmsg("table copy could not start transaction on publisher"),
							 errdetail("The error was: %s", res->err)));
				walrcv_clear_result(res);

				/*
				 * Create new temporary logical decoding slot.
				 *
				 * We'll use slot for data copy so make sure the snapshot is
				 * used for the transaction; that way the COPY will get data
				 * that is consistent with the lsn used by the slot to start
				 * decoding.
				 */
				walrcv_create_slot(LogRepWorkerWalRcvConn, slotname, true,
								   CRS_USE_SNAPSHOT, origin_startpos);

				PushActiveSnapshot(GetTransactionSnapshot());
				copy_table(rel);
				PopActiveSnapshot();

				res = walrcv_exec(LogRepWorkerWalRcvConn, "COMMIT", 0, NULL);
				if (res->status != WALRCV_OK_COMMAND)
					ereport(ERROR,
							(errmsg("table copy could not finish transaction on publisher"),
							 errdetail("The error was: %s", res->err)));
				walrcv_clear_result(res);

				table_close(rel, NoLock);

				/* Make the copy visible. */
				CommandCounterIncrement();

				/*
				 * We are done with the initial data synchronization, update
				 * the state.
				 */
				SpinLockAcquire(&MyLogicalRepWorker->relmutex);
				MyLogicalRepWorker->relstate = SUBREL_STATE_SYNCWAIT;
				MyLogicalRepWorker->relstate_lsn = *origin_startpos;
				SpinLockRelease(&MyLogicalRepWorker->relmutex);

				/* Wait for main apply worker to tell us to catchup. */
				wait_for_worker_state_change(SUBREL_STATE_CATCHUP);

				/*----------
				 * There are now two possible states here:
				 * a) Sync is behind the apply.  If that's the case we need to
				 *	  catch up with it by consuming the logical replication
				 *	  stream up to the relstate_lsn.  For that, we exit this
				 *	  function and continue in ApplyWorkerMain().
				 * b) Sync is caught up with the apply.  So it can just set
				 *	  the state to SYNCDONE and finish.
				 *----------
				 */
				if (*origin_startpos >= MyLogicalRepWorker->relstate_lsn)
				{
					/*
					 * Update the new state in catalog.  No need to bother
					 * with the shmem state as we are exiting for good.
					 */
					UpdateSubscriptionRelState(MyLogicalRepWorker->subid,
											   MyLogicalRepWorker->relid,
											   SUBREL_STATE_SYNCDONE,
											   *origin_startpos);
					finish_sync_worker();
				}
				break;
			}
		case SUBREL_STATE_SYNCDONE:
		case SUBREL_STATE_READY:
		case SUBREL_STATE_UNKNOWN:

			/*
			 * Nothing to do here but finish.  (UNKNOWN means the relation was
			 * removed from pg_subscription_rel before the sync worker could
			 * start.)
			 */
			finish_sync_worker();
			break;
		default:
			elog(ERROR, "unknown relation state \"%c\"",
				 MyLogicalRepWorker->relstate);
	}

	return slotname;
}
