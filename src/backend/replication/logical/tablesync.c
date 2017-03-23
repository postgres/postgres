/*-------------------------------------------------------------------------
 * tablesync.c
 *	  PostgreSQL logical replication
 *
 * Copyright (c) 2012-2016, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/tablesync.c
 *
 * NOTES
 *	  This file contains code for initial table data synchronization for
 *	  logical replication.
 *
 *	  The initial data synchronization is done separately for each table,
 *	  in separate apply worker that only fetches the initial snapshot data
 *	  from the publisher and then synchronizes the position in stream with
 *	  the main apply worker.
 *
 *	  The are several reasons for doing the synchronization this way:
 *	   - It allows us to parallelize the initial data synchronization
 *		 which lowers the time needed for it to happen.
 *	   - The initial synchronization does not have to hold the xid and LSN
 *		 for the time it takes to copy data of all tables, causing less
 *		 bloat and lower disk consumption compared to doing the
 *		 synchronization in single process for whole database.
 *	   - It allows us to synchronize the tables added after the initial
 *		 synchronization has finished.
 *
 *	  The stream position synchronization works in multiple steps.
 *	   - Sync finishes copy and sets table state as SYNCWAIT and waits
 *		 for state to change in a loop.
 *	   - Apply periodically checks tables that are synchronizing for SYNCWAIT.
 *		 When the desired state appears it will compare its position in the
 *		 stream with the SYNCWAIT position and based on that changes the
 *		 state to based on following rules:
 *		  - if the apply is in front of the sync in the wal stream the new
 *			state is set to CATCHUP and apply loops until the sync process
 *			catches up to the same LSN as apply
 *		  - if the sync is in front of the apply in the wal stream the new
 *			state is set to SYNCDONE
 *		  - if both apply and sync are at the same position in the wal stream
 *			the state of the table is set to READY
 *	   - If the state was set to CATCHUP sync will read the stream and
 *		 apply changes until it catches up to the specified stream
 *		 position and then sets state to READY and signals apply that it
 *		 can stop waiting and exits, if the state was set to something
 *		 else than CATCHUP the sync process will simply end.
 *	   - If the state was set to SYNCDONE by apply, the apply will
 *		 continue tracking the table until it reaches the SYNCDONE stream
 *		 position at which point it sets state to READY and stops tracking.
 *
 *	  The catalog pg_subscription_rel is used to keep information about
 *	  subscribed tables and their state and some transient state during
 *	  data synchronization is kept in shared memory.
 *
 *	  Example flows look like this:
 *	   - Apply is in front:
 *		  sync:8
 *			-> set SYNCWAIT
 *		  apply:10
 *			-> set CATCHUP
 *			-> enter wait-loop
 *		  sync:10
 *			-> set READY
 *			-> exit
 *		  apply:10
 *			-> exit wait-loop
 *			-> continue rep
 *	   - Sync in front:
 *		  sync:10
 *			-> set SYNCWAIT
 *		  apply:8
 *			-> set SYNCDONE
 *			-> continue per-table filtering
 *		  sync:10
 *			-> exit
 *		  apply:10
 *			-> set READY
 *			-> stop per-table filtering
 *			-> continue rep
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "pgstat.h"

#include "access/xact.h"

#include "catalog/pg_subscription_rel.h"
#include "catalog/pg_type.h"

#include "commands/copy.h"

#include "replication/logicallauncher.h"
#include "replication/logicalrelation.h"
#include "replication/walreceiver.h"
#include "replication/worker_internal.h"

#include "storage/ipc.h"

#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

static bool table_states_valid = false;

StringInfo	copybuf = NULL;

/*
 * Exit routine for synchronization worker.
 */
static void pg_attribute_noreturn()
finish_sync_worker(void)
{
	/* Commit any outstanding transaction. */
	if (IsTransactionState())
		CommitTransactionCommand();

	/* And flush all writes. */
	XLogFlush(GetXLogWriteRecPtr());

	/* Find the main apply worker and signal it. */
	logicalrep_worker_wakeup(MyLogicalRepWorker->subid, InvalidOid);

	ereport(LOG,
			(errmsg("logical replication synchronization worker finished processing")));

	/* Stop gracefully */
	walrcv_disconnect(wrconn);
	proc_exit(0);
}

/*
 * Wait until the table synchronization change.
 *
 * Returns false if the relation subscription state disappeared.
 */
static bool
wait_for_sync_status_change(Oid relid, char origstate)
{
	int		rc;
	char	state = origstate;

	while (!got_SIGTERM)
	{
		LogicalRepWorker   *worker;

		LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);
		worker = logicalrep_worker_find(MyLogicalRepWorker->subid,
										relid, false);
		if (!worker)
		{
			LWLockRelease(LogicalRepWorkerLock);
			return false;
		}
		state = worker->relstate;
		LWLockRelease(LogicalRepWorkerLock);

		if (state == SUBREL_STATE_UNKNOWN)
			return false;

		if (state != origstate)
			return true;

		rc = WaitLatch(&MyProc->procLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   10000L, WAIT_EVENT_LOGICAL_SYNC_STATE_CHANGE);

		/* emergency bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);

		ResetLatch(&MyProc->procLatch);
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
 * If the sync worker is in catch up mode and reached the predetermined
 * synchronization point in the WAL stream, mark the table as READY and
 * finish.  If it caught up too far, set to SYNCDONE and finish.  Things will
 * then proceed in the "sync in front" scenario.
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

		MyLogicalRepWorker->relstate =
			(current_lsn == MyLogicalRepWorker->relstate_lsn)
			? SUBREL_STATE_READY
			: SUBREL_STATE_SYNCDONE;
		MyLogicalRepWorker->relstate_lsn = current_lsn;

		SpinLockRelease(&MyLogicalRepWorker->relmutex);

		SetSubscriptionRelState(MyLogicalRepWorker->subid,
								MyLogicalRepWorker->relid,
								MyLogicalRepWorker->relstate,
								MyLogicalRepWorker->relstate_lsn);

		walrcv_endstreaming(wrconn, &tli);
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
 * workers).
 *
 * For tables that are being synchronized already, check if sync workers
 * either need action from the apply worker or have finished.
 *
 * The usual scenario is that the apply got ahead of the sync while the sync
 * ran, and then the action needed by apply is to mark a table for CATCHUP and
 * wait for the catchup to happen.  In the less common case that sync worker
 * got in front of the apply worker, the table is marked as SYNCDONE but not
 * ready yet, as it needs to be tracked until apply reaches the same position
 * to which it was synced.
 *
 * If the synchronization position is reached, then the table can be marked as
 * READY and is no longer tracked.
 */
static void
process_syncing_tables_for_apply(XLogRecPtr current_lsn)
{
	static List *table_states = NIL;
	ListCell   *lc;

	Assert(!IsTransactionState());

	/* We need up to date sync state info for subscription tables here. */
	if (!table_states_valid)
	{
		MemoryContext	oldctx;
		List		   *rstates;
		ListCell	   *lc;
		SubscriptionRelState *rstate;

		/* Clean the old list. */
		list_free_deep(table_states);
		table_states = NIL;

		StartTransactionCommand();

		/* Fetch all non-ready tables. */
		rstates	= GetSubscriptionNotReadyRelations(MySubscription->oid);

		/* Allocate the tracking info in a permanent memory context. */
		oldctx = MemoryContextSwitchTo(CacheMemoryContext);
		foreach(lc, rstates)
		{
			rstate = palloc(sizeof(SubscriptionRelState));
			memcpy(rstate, lfirst(lc), sizeof(SubscriptionRelState));
			table_states = lappend(table_states, rstate);
		}
		MemoryContextSwitchTo(oldctx);

		CommitTransactionCommand();

		table_states_valid = true;
	}

	/* Process all tables that are being synchronized. */
	foreach(lc, table_states)
	{
		SubscriptionRelState *rstate = (SubscriptionRelState *)lfirst(lc);

		if (rstate->state == SUBREL_STATE_SYNCDONE)
		{
			/*
			 * Apply has caught up to the position where the table sync
			 * has finished.  Time to mark the table as ready so that
			 * apply will just continue to replicate it normally.
			 */
			if (current_lsn >= rstate->lsn)
			{
				rstate->state = SUBREL_STATE_READY;
				rstate->lsn = current_lsn;
				StartTransactionCommand();
				SetSubscriptionRelState(MyLogicalRepWorker->subid,
										rstate->relid, rstate->state,
										rstate->lsn);
				CommitTransactionCommand();
			}
		}
		else
		{
			LogicalRepWorker   *syncworker;
			int					nsyncworkers = 0;

			LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);
			syncworker = logicalrep_worker_find(MyLogicalRepWorker->subid,
												rstate->relid, false);
			if (syncworker)
			{
				SpinLockAcquire(&syncworker->relmutex);
				rstate->state = syncworker->relstate;
				rstate->lsn = syncworker->relstate_lsn;
				SpinLockRelease(&syncworker->relmutex);
			}
			else
				/*
				 * If no sync worker for this table yet, could running sync
				 * workers for this subscription, while we have the lock, for
				 * later.
				 */
				nsyncworkers = logicalrep_sync_worker_count(MyLogicalRepWorker->subid);
			LWLockRelease(LogicalRepWorkerLock);

			/*
			 * There is a worker synchronizing the relation and waiting for
			 * apply to do something.
			 */
			if (syncworker && rstate->state == SUBREL_STATE_SYNCWAIT)
			{
				/*
				 * There are three possible synchronization situations here.
				 *
				 * a) Apply is in front of the table sync: We tell the table
				 *    sync to CATCHUP.
				 *
				 * b) Apply is behind the table sync: We tell the table sync
				 *    to mark the table as SYNCDONE and finish.

				 * c) Apply and table sync are at the same position: We tell
				 *    table sync to mark the table as READY and finish.
				 *
				 * In any case we'll need to wait for table sync to change
				 * the state in catalog and only then continue ourselves.
				 */
				if (current_lsn > rstate->lsn)
				{
					rstate->state = SUBREL_STATE_CATCHUP;
					rstate->lsn = current_lsn;
				}
				else if (current_lsn == rstate->lsn)
				{
					rstate->state = SUBREL_STATE_READY;
					rstate->lsn = current_lsn;
				}
				else
					rstate->state = SUBREL_STATE_SYNCDONE;

				SpinLockAcquire(&syncworker->relmutex);
				syncworker->relstate = rstate->state;
				syncworker->relstate_lsn = rstate->lsn;
				SpinLockRelease(&syncworker->relmutex);

				/* Signal the sync worker, as it may be waiting for us. */
				logicalrep_worker_wakeup_ptr(syncworker);

				/*
				 * Enter busy loop and wait for synchronization status
				 * change.
				 */
				wait_for_sync_status_change(rstate->relid, rstate->state);
			}

			/*
			 * If there is no sync worker registered for the table and
			 * there is some free sync worker slot, start new sync worker
			 * for the table.
			 */
			else if (!syncworker && nsyncworkers < max_sync_workers_per_subscription)
			{
				logicalrep_worker_launch(MyLogicalRepWorker->dbid,
										 MySubscription->oid,
										 MySubscription->name,
										 MyLogicalRepWorker->userid,
										 rstate->relid);
			}
		}
	}
}

/*
 * Process state possible change(s) of tables that are being synchronized.
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
	TupleDesc	desc = RelationGetDescr(rel->localrel);
	int			i;

	for (i = 0; i < desc->natts; i++)
	{
		int		remoteattnum = rel->attrmap[i];

		/* Skip dropped attributes. */
		if (desc->attrs[i]->attisdropped)
			continue;

		/* Skip attributes that are missing on remote side. */
		if (remoteattnum < 0)
			continue;

		attnamelist = lappend(attnamelist,
							makeString(rel->remoterel.attnames[remoteattnum]));
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
	int		bytesread = 0;
	int		avail;

	/* If there are some leftover data from previous read, use them. */
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

	while (!got_SIGTERM && maxread > 0 && bytesread < minread)
	{
		pgsocket	fd = PGINVALID_SOCKET;
		int			rc;
		int			len;
		char	   *buf = NULL;

		for (;;)
		{
			/* Try read the data. */
			len = walrcv_receive(wrconn, &buf, &fd);

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
		rc = WaitLatchOrSocket(&MyProc->procLatch,
							   WL_SOCKET_READABLE | WL_LATCH_SET |
							   WL_TIMEOUT | WL_POSTMASTER_DEATH,
							   fd, 1000L, WAIT_EVENT_LOGICAL_SYNC_DATA);

		/* Emergency bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);

		ResetLatch(&MyProc->procLatch);
	}

	/* Check for exit condition. */
	if (got_SIGTERM)
		proc_exit(0);

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
	WalRcvExecResult   *res;
	StringInfoData		cmd;
	TupleTableSlot	   *slot;
	Oid					tableRow[2] = {OIDOID, CHAROID};
	Oid					attrRow[4] = {TEXTOID, OIDOID, INT4OID, BOOLOID};
	bool				isnull;
	int					natt;

	lrel->nspname = nspname;
	lrel->relname = relname;

	/* First fetch Oid and replica identity. */
	initStringInfo(&cmd);
	appendStringInfo(&cmd, "SELECT c.oid, c.relreplident"
						   "  FROM pg_catalog.pg_class c,"
						   "       pg_catalog.pg_namespace n"
						   " WHERE n.nspname = %s"
						   "   AND c.relname = %s"
						   "   AND c.relkind = 'r'",
						   quote_literal_cstr(nspname),
						   quote_literal_cstr(relname));
	res = walrcv_exec(wrconn, cmd.data, 2, tableRow);

	if (res->status != WALRCV_OK_TUPLES)
		ereport(ERROR,
				(errmsg("could not fetch table info for table \"%s.%s\" from publisher: %s",
						nspname, relname, res->err)));

	slot = MakeSingleTupleTableSlot(res->tupledesc);
	if (!tuplestore_gettupleslot(res->tuplestore, true, false, slot))
		ereport(ERROR,
				(errmsg("table \"%s.%s\" not found on publisher",
						nspname, relname)));

	lrel->remoteid = DatumGetObjectId(slot_getattr(slot, 1, &isnull));
	Assert(!isnull);
	lrel->replident = DatumGetChar(slot_getattr(slot, 2, &isnull));
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
					 "   AND NOT a.attisdropped"
					 "   AND a.attrelid = %u"
					 " ORDER BY a.attnum",
					 lrel->remoteid, lrel->remoteid);
	res = walrcv_exec(wrconn, cmd.data, 4, attrRow);

	if (res->status != WALRCV_OK_TUPLES)
		ereport(ERROR,
				(errmsg("could not fetch table info for table \"%s.%s\": %s",
						nspname, relname, res->err)));

	/* We don't know number of rows coming, so allocate enough space. */
	lrel->attnames = palloc0(MaxTupleAttributeNumber * sizeof(char *));
	lrel->atttyps = palloc0(MaxTupleAttributeNumber * sizeof(Oid));
	lrel->attkeys = NULL;

	natt = 0;
	slot = MakeSingleTupleTableSlot(res->tupledesc);
	while (tuplestore_gettupleslot(res->tuplestore, true, false, slot))
	{
		lrel->attnames[natt] =
			pstrdup(TextDatumGetCString(slot_getattr(slot, 1, &isnull)));
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
	LogicalRepRelation	lrel;
	WalRcvExecResult   *res;
	StringInfoData		cmd;
	CopyState	cstate;
	List	   *attnamelist;

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
	appendStringInfo(&cmd, "COPY %s TO STDOUT",
					 quote_qualified_identifier(lrel.nspname, lrel.relname));
	res = walrcv_exec(wrconn, cmd.data, 0, NULL);
	pfree(cmd.data);
	if (res->status != WALRCV_OK_COPY_OUT)
		ereport(ERROR,
				(errmsg("could not start initial contents copy for table \"%s.%s\": %s",
						lrel.nspname, lrel.relname, res->err)));
	walrcv_clear_result(res);

	copybuf = makeStringInfo();

	/* Create CopyState for ingestion of the data from publisher. */
	attnamelist = make_copy_attnamelist(relmapentry);
	cstate = BeginCopyFrom(NULL, rel, NULL, false, copy_read_data, attnamelist, NIL);

	/* Do the copy */
	(void) CopyFrom(cstate);

	logicalrep_rel_close(relmapentry, NoLock);
}

/*
 * Start syncing the table in the sync worker.
 *
 * The returned slot name is palloced in current memory context.
 */
char *
LogicalRepSyncTableStart(XLogRecPtr *origin_startpos)
{
	char		   *slotname;
	char		   *err;

	/* Check the state of the table synchronization. */
	StartTransactionCommand();
	SpinLockAcquire(&MyLogicalRepWorker->relmutex);
	MyLogicalRepWorker->relstate =
		GetSubscriptionRelState(MyLogicalRepWorker->subid,
								MyLogicalRepWorker->relid,
								&MyLogicalRepWorker->relstate_lsn,
								false);
	SpinLockRelease(&MyLogicalRepWorker->relmutex);
	CommitTransactionCommand();

	/*
	 * To build a slot name for the sync work, we are limited to NAMEDATALEN -
	 * 1 characters.  We cut the original slot name to NAMEDATALEN - 28 chars
	 * and append _%u_sync_%u (1 + 10 + 6 + 10 + '\0').  (It's actually the
	 * NAMEDATALEN on the remote that matters, but this scheme will also work
	 * reasonably if that is different.)
	 */
	StaticAssertStmt(NAMEDATALEN >= 32, "NAMEDATALEN too small"); /* for sanity */
	slotname = psprintf("%.*s_%u_sync_%u",
						NAMEDATALEN - 28,
						MySubscription->slotname,
						MySubscription->oid,
						MyLogicalRepWorker->relid);

	wrconn = walrcv_connect(MySubscription->conninfo, true, slotname, &err);
	if (wrconn == NULL)
		ereport(ERROR,
				(errmsg("could not connect to the publisher: %s", err)));

	switch (MyLogicalRepWorker->relstate)
	{
		case SUBREL_STATE_INIT:
		case SUBREL_STATE_DATASYNC:
			{
				Relation	rel;
				WalRcvExecResult   *res;

				SpinLockAcquire(&MyLogicalRepWorker->relmutex);
				MyLogicalRepWorker->relstate = SUBREL_STATE_DATASYNC;
				MyLogicalRepWorker->relstate_lsn = InvalidXLogRecPtr;
				SpinLockRelease(&MyLogicalRepWorker->relmutex);

				/* Update the state and make it visible to others. */
				StartTransactionCommand();
				SetSubscriptionRelState(MyLogicalRepWorker->subid,
										MyLogicalRepWorker->relid,
										MyLogicalRepWorker->relstate,
										MyLogicalRepWorker->relstate_lsn);
				CommitTransactionCommand();

				/*
				 * We want to do the table data sync in single
				 * transaction.
				 */
				StartTransactionCommand();

				/*
				 * Use standard write lock here. It might be better to
				 * disallow access to table while it's being synchronized.
				 * But we don't want to block the main apply process from
				 * working and it has to open relation in RowExclusiveLock
				 * when remapping remote relation id to local one.
				 */
				rel = heap_open(MyLogicalRepWorker->relid, RowExclusiveLock);

				/*
				 * Create temporary slot for the sync process.
				 * We do this inside transaction so that we can use the
				 * snapshot made by the slot to get existing data.
				 */
				res = walrcv_exec(wrconn,
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
				 * We'll use slot for data copy so make sure the snapshot
				 * is used for the transaction, that way the COPY will get
				 * data that is consistent with the lsn used by the slot
				 * to start decoding.
				 */
				walrcv_create_slot(wrconn, slotname, true,
								   CRS_USE_SNAPSHOT, origin_startpos);

				copy_table(rel);

				res = walrcv_exec(wrconn, "COMMIT", 0, NULL);
				if (res->status != WALRCV_OK_COMMAND)
					ereport(ERROR,
							(errmsg("table copy could not finish transaction on publisher"),
							 errdetail("The error was: %s", res->err)));
				walrcv_clear_result(res);

				heap_close(rel, NoLock);

				/* Make the copy visible. */
				CommandCounterIncrement();

				/*
				 * We are done with the initial data synchronization,
				 * update the state.
				 */
				SpinLockAcquire(&MyLogicalRepWorker->relmutex);
				MyLogicalRepWorker->relstate = SUBREL_STATE_SYNCWAIT;
				MyLogicalRepWorker->relstate_lsn = *origin_startpos;
				SpinLockRelease(&MyLogicalRepWorker->relmutex);

				/*
				 * Wait for main apply worker to either tell us to
				 * catchup or that we are done.
				 */
				wait_for_sync_status_change(MyLogicalRepWorker->relid,
											MyLogicalRepWorker->relstate);
				if (MyLogicalRepWorker->relstate != SUBREL_STATE_CATCHUP)
				{
					/* Update the new state. */
					SetSubscriptionRelState(MyLogicalRepWorker->subid,
											MyLogicalRepWorker->relid,
											MyLogicalRepWorker->relstate,
											MyLogicalRepWorker->relstate_lsn);
					finish_sync_worker();
				}
				break;
			}
		case SUBREL_STATE_SYNCDONE:
		case SUBREL_STATE_READY:
			/* Nothing to do here but finish. */
			finish_sync_worker();
			break;
		default:
			elog(ERROR, "unknown relation state \"%c\"",
				 MyLogicalRepWorker->relstate);
	}

	return slotname;
}
