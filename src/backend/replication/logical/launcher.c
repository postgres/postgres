/*-------------------------------------------------------------------------
 * launcher.c
 *	   PostgreSQL logical replication worker launcher process
 *
 * Copyright (c) 2016-2017, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/launcher.c
 *
 * NOTES
 *	  This module contains the logical replication worker launcher which
 *	  uses the background worker infrastructure to start the logical
 *	  replication workers for every enabled subscription.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"

#include "access/heapam.h"
#include "access/htup.h"
#include "access/htup_details.h"
#include "access/xact.h"

#include "catalog/pg_subscription.h"
#include "catalog/pg_subscription_rel.h"

#include "libpq/pqsignal.h"

#include "postmaster/bgworker.h"
#include "postmaster/fork_process.h"
#include "postmaster/postmaster.h"

#include "replication/logicallauncher.h"
#include "replication/logicalworker.h"
#include "replication/slot.h"
#include "replication/worker_internal.h"

#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"

#include "tcop/tcopprot.h"

#include "utils/memutils.h"
#include "utils/pg_lsn.h"
#include "utils/ps_status.h"
#include "utils/timeout.h"
#include "utils/snapmgr.h"

/* max sleep time between cycles (3min) */
#define DEFAULT_NAPTIME_PER_CYCLE 180000L

int	max_logical_replication_workers = 4;
int max_sync_workers_per_subscription = 2;

LogicalRepWorker *MyLogicalRepWorker = NULL;

typedef struct LogicalRepCtxStruct
{
	/* Supervisor process. */
	pid_t		launcher_pid;

	/* Background workers. */
	LogicalRepWorker	workers[FLEXIBLE_ARRAY_MEMBER];
} LogicalRepCtxStruct;

LogicalRepCtxStruct *LogicalRepCtx;

static void logicalrep_worker_onexit(int code, Datum arg);
static void logicalrep_worker_detach(void);

bool		got_SIGTERM = false;
static bool	on_commit_launcher_wakeup = false;

Datum pg_stat_get_subscription(PG_FUNCTION_ARGS);


/*
 * Load the list of subscriptions.
 *
 * Only the fields interesting for worker start/stop functions are filled for
 * each subscription.
 */
static List *
get_subscription_list(void)
{
	List	   *res = NIL;
	Relation	rel;
	HeapScanDesc scan;
	HeapTuple	tup;
	MemoryContext resultcxt;

	/* This is the context that we will allocate our output data in */
	resultcxt = CurrentMemoryContext;

	/*
	 * Start a transaction so we can access pg_database, and get a snapshot.
	 * We don't have a use for the snapshot itself, but we're interested in
	 * the secondary effect that it sets RecentGlobalXmin.  (This is critical
	 * for anything that reads heap pages, because HOT may decide to prune
	 * them even if the process doesn't attempt to modify any tuples.)
	 */
	StartTransactionCommand();
	(void) GetTransactionSnapshot();

	rel = heap_open(SubscriptionRelationId, AccessShareLock);
	scan = heap_beginscan_catalog(rel, 0, NULL);

	while (HeapTupleIsValid(tup = heap_getnext(scan, ForwardScanDirection)))
	{
		Form_pg_subscription subform = (Form_pg_subscription) GETSTRUCT(tup);
		Subscription   *sub;
		MemoryContext	oldcxt;

		/*
		 * Allocate our results in the caller's context, not the
		 * transaction's. We do this inside the loop, and restore the original
		 * context at the end, so that leaky things like heap_getnext() are
		 * not called in a potentially long-lived context.
		 */
		oldcxt = MemoryContextSwitchTo(resultcxt);

		sub = (Subscription *) palloc(sizeof(Subscription));
		sub->oid = HeapTupleGetOid(tup);
		sub->dbid = subform->subdbid;
		sub->owner = subform->subowner;
		sub->enabled = subform->subenabled;
		sub->name = pstrdup(NameStr(subform->subname));

		/* We don't fill fields we are not interested in. */
		sub->conninfo = NULL;
		sub->slotname = NULL;
		sub->publications = NIL;

		res = lappend(res, sub);
		MemoryContextSwitchTo(oldcxt);
	}

	heap_endscan(scan);
	heap_close(rel, AccessShareLock);

	CommitTransactionCommand();

	return res;
}

/*
 * Wait for a background worker to start up and attach to the shmem context.
 *
 * This is like WaitForBackgroundWorkerStartup(), except that we wait for
 * attaching, not just start and we also just exit if postmaster died.
 */
static bool
WaitForReplicationWorkerAttach(LogicalRepWorker *worker,
							   BackgroundWorkerHandle *handle)
{
	BgwHandleStatus status;
	int			rc;

	for (;;)
	{
		pid_t		pid;

		CHECK_FOR_INTERRUPTS();

		status = GetBackgroundWorkerPid(handle, &pid);

		/*
		 * Worker started and attached to our shmem. This check is safe
		 * because only launcher ever starts the workers, so nobody can steal
		 * the worker slot.
		 */
		if (status == BGWH_STARTED && worker->proc)
			return true;
		/* Worker didn't start or died before attaching to our shmem. */
		if (status == BGWH_STOPPED)
			return false;

		/*
		 * We need timeout because we generally don't get notified via latch
		 * about the worker attach.
		 */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   1000L, WAIT_EVENT_BGWORKER_STARTUP);

		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);

		ResetLatch(MyLatch);
	}

	return false;
}

/*
 * Walks the workers array and searches for one that matches given
 * subscription id and relid.
 */
LogicalRepWorker *
logicalrep_worker_find(Oid subid, Oid relid, bool only_running)
{
	int	i;
	LogicalRepWorker   *res = NULL;

	Assert(LWLockHeldByMe(LogicalRepWorkerLock));

	/* Search for attached worker for a given subscription id. */
	for (i = 0; i < max_logical_replication_workers; i++)
	{
		LogicalRepWorker   *w = &LogicalRepCtx->workers[i];
		if (w->subid == subid && w->relid == relid &&
			(!only_running || (w->proc && IsBackendPid(w->proc->pid))))
		{
			res = w;
			break;
		}
	}

	return res;
}

/*
 * Start new apply background worker.
 */
void
logicalrep_worker_launch(Oid dbid, Oid subid, const char *subname, Oid userid,
						 Oid relid)
{
	BackgroundWorker	bgw;
	BackgroundWorkerHandle *bgw_handle;
	int					slot;
	LogicalRepWorker   *worker = NULL;

	ereport(LOG,
			(errmsg("starting logical replication worker for subscription \"%s\"",
					subname)));

	/* Report this after the initial starting message for consistency. */
	if (max_replication_slots == 0)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("cannot start logical replication workers when max_replication_slots = 0")));

	/*
	 * We need to do the modification of the shared memory under lock so that
	 * we have consistent view.
	 */
	LWLockAcquire(LogicalRepWorkerLock, LW_EXCLUSIVE);

	/* Find unused worker slot. */
	for (slot = 0; slot < max_logical_replication_workers; slot++)
	{
		if (!LogicalRepCtx->workers[slot].proc)
		{
			worker = &LogicalRepCtx->workers[slot];
			break;
		}
	}

	/* Bail if not found */
	if (worker == NULL)
	{
		LWLockRelease(LogicalRepWorkerLock);
		ereport(WARNING,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("out of logical replication workers slots"),
				 errhint("You might need to increase max_logical_replication_workers.")));
		return;
	}

	/* Prepare the worker info. */
	worker->proc = NULL;
	worker->dbid = dbid;
	worker->userid = userid;
	worker->subid = subid;
	worker->relid = relid;
	worker->relstate = SUBREL_STATE_UNKNOWN;
	worker->relstate_lsn = InvalidXLogRecPtr;
	worker->last_lsn = InvalidXLogRecPtr;
	TIMESTAMP_NOBEGIN(worker->last_send_time);
	TIMESTAMP_NOBEGIN(worker->last_recv_time);
	worker->reply_lsn = InvalidXLogRecPtr;
	TIMESTAMP_NOBEGIN(worker->reply_time);

	LWLockRelease(LogicalRepWorkerLock);

	/* Register the new dynamic worker. */
	bgw.bgw_flags =	BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	bgw.bgw_main = ApplyWorkerMain;
	if (OidIsValid(relid))
		snprintf(bgw.bgw_name, BGW_MAXLEN,
				 "logical replication worker for subscription %u sync %u", subid, relid);
	else
		snprintf(bgw.bgw_name, BGW_MAXLEN,
				 "logical replication worker for subscription %u", subid);

	bgw.bgw_restart_time = BGW_NEVER_RESTART;
	bgw.bgw_notify_pid = MyProcPid;
	bgw.bgw_main_arg = slot;

	if (!RegisterDynamicBackgroundWorker(&bgw, &bgw_handle))
	{
		ereport(WARNING,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("out of background workers slots"),
				 errhint("You might need to increase max_worker_processes.")));
		return;
	}

	/* Now wait until it attaches. */
	WaitForReplicationWorkerAttach(worker, bgw_handle);
}

/*
 * Stop the logical replication worker and wait until it detaches from the
 * slot.
 */
void
logicalrep_worker_stop(Oid subid, Oid relid)
{
	LogicalRepWorker *worker;

	LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

	worker = logicalrep_worker_find(subid, relid, false);

	/* No worker, nothing to do. */
	if (!worker)
	{
		LWLockRelease(LogicalRepWorkerLock);
		return;
	}

	/*
	 * If we found worker but it does not have proc set it is starting up,
	 * wait for it to finish and then kill it.
	 */
	while (worker && !worker->proc)
	{
		int	rc;

		LWLockRelease(LogicalRepWorkerLock);

		CHECK_FOR_INTERRUPTS();

		/* Wait for signal. */
		rc = WaitLatch(&MyProc->procLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   1000L, WAIT_EVENT_BGWORKER_STARTUP);

		/* emergency bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);

		ResetLatch(&MyProc->procLatch);

		/* Check worker status. */
		LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

		/*
		 * Worker is no longer associated with subscription.  It must have
		 * exited, nothing more for us to do.
		 */
		if (worker->subid == InvalidOid)
		{
			LWLockRelease(LogicalRepWorkerLock);
			return;
		}

		/* Worker has assigned proc, so it has started. */
		if (worker->proc)
			break;
	}

	/* Now terminate the worker ... */
	kill(worker->proc->pid, SIGTERM);
	LWLockRelease(LogicalRepWorkerLock);

	/* ... and wait for it to die. */
	for (;;)
	{
		int	rc;

		LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);
		if (!worker->proc)
		{
			LWLockRelease(LogicalRepWorkerLock);
			break;
		}
		LWLockRelease(LogicalRepWorkerLock);

		CHECK_FOR_INTERRUPTS();

		/* Wait for more work. */
		rc = WaitLatch(&MyProc->procLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   1000L, WAIT_EVENT_BGWORKER_SHUTDOWN);

		/* emergency bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);

		ResetLatch(&MyProc->procLatch);
	}
}

/*
 * Wake up (using latch) the logical replication worker.
 */
void
logicalrep_worker_wakeup(Oid subid, Oid relid)
{
	LogicalRepWorker   *worker;

	LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);
	worker = logicalrep_worker_find(subid, relid, true);
	LWLockRelease(LogicalRepWorkerLock);

	if (worker)
		logicalrep_worker_wakeup_ptr(worker);
}

/*
 * Wake up (using latch) the logical replication worker.
 */
void
logicalrep_worker_wakeup_ptr(LogicalRepWorker *worker)
{
	SetLatch(&worker->proc->procLatch);
}

/*
 * Attach to a slot.
 */
void
logicalrep_worker_attach(int slot)
{
	/* Block concurrent access. */
	LWLockAcquire(LogicalRepWorkerLock, LW_EXCLUSIVE);

	Assert(slot >= 0 && slot < max_logical_replication_workers);
	MyLogicalRepWorker = &LogicalRepCtx->workers[slot];

	if (MyLogicalRepWorker->proc)
		ereport(ERROR,
			   (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("logical replication worker slot %d already used by "
					   "another worker", slot)));

	MyLogicalRepWorker->proc = MyProc;
	before_shmem_exit(logicalrep_worker_onexit, (Datum) 0);

	LWLockRelease(LogicalRepWorkerLock);
}

/*
 * Detach the worker (cleans up the worker info).
 */
static void
logicalrep_worker_detach(void)
{
	/* Block concurrent access. */
	LWLockAcquire(LogicalRepWorkerLock, LW_EXCLUSIVE);

	MyLogicalRepWorker->dbid = InvalidOid;
	MyLogicalRepWorker->userid = InvalidOid;
	MyLogicalRepWorker->subid = InvalidOid;
	MyLogicalRepWorker->proc = NULL;

	LWLockRelease(LogicalRepWorkerLock);
}

/*
 * Cleanup function.
 *
 * Called on logical replication worker exit.
 */
static void
logicalrep_worker_onexit(int code, Datum arg)
{
	logicalrep_worker_detach();
}

/* SIGTERM: set flag to exit at next convenient time */
void
logicalrep_worker_sigterm(SIGNAL_ARGS)
{
	got_SIGTERM = true;

	/* Waken anything waiting on the process latch */
	SetLatch(MyLatch);
}

/*
 * Count the number of registered (not necessarily running) sync workers
 * for a subscription.
 */
int
logicalrep_sync_worker_count(Oid subid)
{
	int	i;
	int	res = 0;

	Assert(LWLockHeldByMe(LogicalRepWorkerLock));

	/* Search for attached worker for a given subscription id. */
	for (i = 0; i < max_logical_replication_workers; i++)
	{
		LogicalRepWorker   *w = &LogicalRepCtx->workers[i];
		if (w->subid == subid && OidIsValid(w->relid))
			res++;
	}

	return res;
}

/*
 * ApplyLauncherShmemSize
 *		Compute space needed for replication launcher shared memory
 */
Size
ApplyLauncherShmemSize(void)
{
	Size		size;

	/*
	 * Need the fixed struct and the array of LogicalRepWorker.
	 */
	size = sizeof(LogicalRepCtxStruct);
	size = MAXALIGN(size);
	size = add_size(size, mul_size(max_logical_replication_workers,
								   sizeof(LogicalRepWorker)));
	return size;
}

void
ApplyLauncherRegister(void)
{
	BackgroundWorker bgw;

	if (max_logical_replication_workers == 0)
		return;

	bgw.bgw_flags =	BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	bgw.bgw_main = ApplyLauncherMain;
	snprintf(bgw.bgw_name, BGW_MAXLEN,
			 "logical replication launcher");
	bgw.bgw_restart_time = 5;
	bgw.bgw_notify_pid = 0;
	bgw.bgw_main_arg = (Datum) 0;

	RegisterBackgroundWorker(&bgw);
}

/*
 * ApplyLauncherShmemInit
 *		Allocate and initialize replication launcher shared memory
 */
void
ApplyLauncherShmemInit(void)
{
	bool		found;

	LogicalRepCtx = (LogicalRepCtxStruct *)
		ShmemInitStruct("Logical Replication Launcher Data",
						ApplyLauncherShmemSize(),
						&found);

	if (!found)
	{
		int slot;

		memset(LogicalRepCtx, 0, ApplyLauncherShmemSize());

		/* Initialize memory and spin locks for each worker slot. */
		for (slot = 0; slot < max_logical_replication_workers; slot++)
		{
			LogicalRepWorker *worker = &LogicalRepCtx->workers[slot];

			memset(worker, 0, sizeof(LogicalRepWorker));
			SpinLockInit(&worker->relmutex);
		}
	}
}

/*
 * Wakeup the launcher on commit if requested.
 */
void
AtCommit_ApplyLauncher(void)
{
	if (on_commit_launcher_wakeup)
		ApplyLauncherWakeup();
}

/*
 * Request wakeup of the launcher on commit of the transaction.
 *
 * This is used to send launcher signal to stop sleeping and process the
 * subscriptions when current transaction commits. Should be used when new
 * tuple was added to the pg_subscription catalog.
*/
void
ApplyLauncherWakeupAtCommit(void)
{
	if (!on_commit_launcher_wakeup)
		on_commit_launcher_wakeup = true;
}

void
ApplyLauncherWakeup(void)
{
	if (IsBackendPid(LogicalRepCtx->launcher_pid))
		kill(LogicalRepCtx->launcher_pid, SIGUSR1);
}

/*
 * Main loop for the apply launcher process.
 */
void
ApplyLauncherMain(Datum main_arg)
{
	ereport(DEBUG1,
			(errmsg("logical replication launcher started")));

	/* Establish signal handlers. */
	pqsignal(SIGTERM, logicalrep_worker_sigterm);
	BackgroundWorkerUnblockSignals();

	/* Make it easy to identify our processes. */
	SetConfigOption("application_name", MyBgworkerEntry->bgw_name,
					PGC_USERSET, PGC_S_SESSION);

	LogicalRepCtx->launcher_pid = MyProcPid;

	/*
	 * Establish connection to nailed catalogs (we only ever access
	 * pg_subscription).
	 */
	BackgroundWorkerInitializeConnection(NULL, NULL);

	/* Enter main loop */
	while (!got_SIGTERM)
	{
		int			rc;
		List	   *sublist;
		ListCell   *lc;
		MemoryContext	subctx;
		MemoryContext	oldctx;
		TimestampTz		now;
		TimestampTz		last_start_time = 0;
		long			wait_time = DEFAULT_NAPTIME_PER_CYCLE;

		now = GetCurrentTimestamp();

		/* Limit the start retry to once a wal_retrieve_retry_interval */
		if (TimestampDifferenceExceeds(last_start_time, now,
									   wal_retrieve_retry_interval))
		{
			/* Use temporary context for the database list and worker info. */
			subctx = AllocSetContextCreate(TopMemoryContext,
										   "Logical Replication Launcher sublist",
										   ALLOCSET_DEFAULT_MINSIZE,
										   ALLOCSET_DEFAULT_INITSIZE,
										   ALLOCSET_DEFAULT_MAXSIZE);
			oldctx = MemoryContextSwitchTo(subctx);

			/* search for subscriptions to start or stop. */
			sublist = get_subscription_list();

			/* Start the missing workers for enabled subscriptions. */
			foreach(lc, sublist)
			{
				Subscription	   *sub = (Subscription *) lfirst(lc);
				LogicalRepWorker   *w;

				LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);
				w = logicalrep_worker_find(sub->oid, InvalidOid, false);
				LWLockRelease(LogicalRepWorkerLock);

				if (sub->enabled && w == NULL)
				{
					logicalrep_worker_launch(sub->dbid, sub->oid, sub->name,
											 sub->owner, InvalidOid);
					last_start_time = now;
					wait_time = wal_retrieve_retry_interval;
					/* Limit to one worker per mainloop cycle. */
					break;
				}
			}

			/* Switch back to original memory context. */
			MemoryContextSwitchTo(oldctx);
			/* Clean the temporary memory. */
			MemoryContextDelete(subctx);
		}
		else
		{
			/*
			 * The wait in previous cycle was interrupted in less than
			 * wal_retrieve_retry_interval since last worker was started,
			 * this usually means crash of the worker, so we should retry
			 * in wal_retrieve_retry_interval again.
			 */
			wait_time = wal_retrieve_retry_interval;
		}

		/* Wait for more work. */
		rc = WaitLatch(&MyProc->procLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   wait_time,
					   WAIT_EVENT_LOGICAL_LAUNCHER_MAIN);

		/* emergency bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);

		ResetLatch(&MyProc->procLatch);
	}

	LogicalRepCtx->launcher_pid = 0;

	/* ... and if it returns, we're done */
	ereport(DEBUG1,
			(errmsg("logical replication launcher shutting down")));

	proc_exit(0);
}

/*
 * Returns state of the subscriptions.
 */
Datum
pg_stat_get_subscription(PG_FUNCTION_ARGS)
{
#define PG_STAT_GET_SUBSCRIPTION_COLS	8
	Oid			subid = PG_ARGISNULL(0) ? InvalidOid : PG_GETARG_OID(0);
	int			i;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	/* Make sure we get consistent view of the workers. */
	LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

	for (i = 0; i <= max_logical_replication_workers; i++)
	{
		/* for each row */
		Datum		values[PG_STAT_GET_SUBSCRIPTION_COLS];
		bool		nulls[PG_STAT_GET_SUBSCRIPTION_COLS];
		int			worker_pid;
		LogicalRepWorker	worker;

		memcpy(&worker, &LogicalRepCtx->workers[i],
			   sizeof(LogicalRepWorker));
		if (!worker.proc || !IsBackendPid(worker.proc->pid))
			continue;

		if (OidIsValid(subid) && worker.subid != subid)
			continue;

		worker_pid = worker.proc->pid;

		MemSet(values, 0, sizeof(values));
		MemSet(nulls, 0, sizeof(nulls));

		values[0] = ObjectIdGetDatum(worker.subid);
		if (OidIsValid(worker.relid))
			values[1] = ObjectIdGetDatum(worker.relid);
		else
			nulls[1] = true;
		values[2] = Int32GetDatum(worker_pid);
		if (XLogRecPtrIsInvalid(worker.last_lsn))
			nulls[3] = true;
		else
			values[3] = LSNGetDatum(worker.last_lsn);
		if (worker.last_send_time == 0)
			nulls[4] = true;
		else
			values[4] = TimestampTzGetDatum(worker.last_send_time);
		if (worker.last_recv_time == 0)
			nulls[5] = true;
		else
			values[5] = TimestampTzGetDatum(worker.last_recv_time);
		if (XLogRecPtrIsInvalid(worker.reply_lsn))
			nulls[6] = true;
		else
			values[6] = LSNGetDatum(worker.reply_lsn);
		if (worker.reply_time == 0)
			nulls[7] = true;
		else
			values[7] = TimestampTzGetDatum(worker.reply_time);

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);

		/* If only a single subscription was requested, and we found it, break. */
		if (OidIsValid(subid))
			break;
	}

	LWLockRelease(LogicalRepWorkerLock);

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}
