/*-------------------------------------------------------------------------
 * launcher.c
 *	   PostgreSQL logical replication worker launcher process
 *
 * Copyright (c) 2016-2020, PostgreSQL Global Development Group
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

#include "access/heapam.h"
#include "access/htup.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_subscription_rel.h"
#include "funcapi.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/fork_process.h"
#include "postmaster/interrupt.h"
#include "postmaster/postmaster.h"
#include "replication/logicallauncher.h"
#include "replication/logicalworker.h"
#include "replication/slot.h"
#include "replication/walreceiver.h"
#include "replication/worker_internal.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
#include "tcop/tcopprot.h"
#include "utils/memutils.h"
#include "utils/pg_lsn.h"
#include "utils/ps_status.h"
#include "utils/snapmgr.h"
#include "utils/timeout.h"

/* max sleep time between cycles (3min) */
#define DEFAULT_NAPTIME_PER_CYCLE 180000L

int			max_logical_replication_workers = 4;
int			max_sync_workers_per_subscription = 2;

LogicalRepWorker *MyLogicalRepWorker = NULL;

typedef struct LogicalRepCtxStruct
{
	/* Supervisor process. */
	pid_t		launcher_pid;

	/* Background workers. */
	LogicalRepWorker workers[FLEXIBLE_ARRAY_MEMBER];
} LogicalRepCtxStruct;

LogicalRepCtxStruct *LogicalRepCtx;

typedef struct LogicalRepWorkerId
{
	Oid			subid;
	Oid			relid;
} LogicalRepWorkerId;

typedef struct StopWorkersData
{
	int			nestDepth;		/* Sub-transaction nest level */
	List	   *workers;		/* List of LogicalRepWorkerId */
	struct StopWorkersData *parent; /* This need not be an immediate
									 * subtransaction parent */
} StopWorkersData;

/*
 * Stack of StopWorkersData elements. Each stack element contains the workers
 * to be stopped for that subtransaction.
 */
static StopWorkersData *on_commit_stop_workers = NULL;

static void ApplyLauncherWakeup(void);
static void logicalrep_launcher_onexit(int code, Datum arg);
static void logicalrep_worker_onexit(int code, Datum arg);
static void logicalrep_worker_detach(void);
static void logicalrep_worker_cleanup(LogicalRepWorker *worker);

static bool on_commit_launcher_wakeup = false;

Datum		pg_stat_get_subscription(PG_FUNCTION_ARGS);


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
	TableScanDesc scan;
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

	rel = table_open(SubscriptionRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);

	while (HeapTupleIsValid(tup = heap_getnext(scan, ForwardScanDirection)))
	{
		Form_pg_subscription subform = (Form_pg_subscription) GETSTRUCT(tup);
		Subscription *sub;
		MemoryContext oldcxt;

		/*
		 * Allocate our results in the caller's context, not the
		 * transaction's. We do this inside the loop, and restore the original
		 * context at the end, so that leaky things like heap_getnext() are
		 * not called in a potentially long-lived context.
		 */
		oldcxt = MemoryContextSwitchTo(resultcxt);

		sub = (Subscription *) palloc0(sizeof(Subscription));
		sub->oid = subform->oid;
		sub->dbid = subform->subdbid;
		sub->owner = subform->subowner;
		sub->enabled = subform->subenabled;
		sub->name = pstrdup(NameStr(subform->subname));
		/* We don't fill fields we are not interested in. */

		res = lappend(res, sub);
		MemoryContextSwitchTo(oldcxt);
	}

	table_endscan(scan);
	table_close(rel, AccessShareLock);

	CommitTransactionCommand();

	return res;
}

/*
 * Wait for a background worker to start up and attach to the shmem context.
 *
 * This is only needed for cleaning up the shared memory in case the worker
 * fails to attach.
 */
static void
WaitForReplicationWorkerAttach(LogicalRepWorker *worker,
							   uint16 generation,
							   BackgroundWorkerHandle *handle)
{
	BgwHandleStatus status;
	int			rc;

	for (;;)
	{
		pid_t		pid;

		CHECK_FOR_INTERRUPTS();

		LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

		/* Worker either died or has started; no need to do anything. */
		if (!worker->in_use || worker->proc)
		{
			LWLockRelease(LogicalRepWorkerLock);
			return;
		}

		LWLockRelease(LogicalRepWorkerLock);

		/* Check if worker has died before attaching, and clean up after it. */
		status = GetBackgroundWorkerPid(handle, &pid);

		if (status == BGWH_STOPPED)
		{
			LWLockAcquire(LogicalRepWorkerLock, LW_EXCLUSIVE);
			/* Ensure that this was indeed the worker we waited for. */
			if (generation == worker->generation)
				logicalrep_worker_cleanup(worker);
			LWLockRelease(LogicalRepWorkerLock);
			return;
		}

		/*
		 * We need timeout because we generally don't get notified via latch
		 * about the worker attach.  But we don't expect to have to wait long.
		 */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   10L, WAIT_EVENT_BGWORKER_STARTUP);

		if (rc & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}
	}
}

/*
 * Walks the workers array and searches for one that matches given
 * subscription id and relid.
 */
LogicalRepWorker *
logicalrep_worker_find(Oid subid, Oid relid, bool only_running)
{
	int			i;
	LogicalRepWorker *res = NULL;

	Assert(LWLockHeldByMe(LogicalRepWorkerLock));

	/* Search for attached worker for a given subscription id. */
	for (i = 0; i < max_logical_replication_workers; i++)
	{
		LogicalRepWorker *w = &LogicalRepCtx->workers[i];

		if (w->in_use && w->subid == subid && w->relid == relid &&
			(!only_running || w->proc))
		{
			res = w;
			break;
		}
	}

	return res;
}

/*
 * Similar to logicalrep_worker_find(), but returns list of all workers for
 * the subscription, instead just one.
 */
List *
logicalrep_workers_find(Oid subid, bool only_running)
{
	int			i;
	List	   *res = NIL;

	Assert(LWLockHeldByMe(LogicalRepWorkerLock));

	/* Search for attached worker for a given subscription id. */
	for (i = 0; i < max_logical_replication_workers; i++)
	{
		LogicalRepWorker *w = &LogicalRepCtx->workers[i];

		if (w->in_use && w->subid == subid && (!only_running || w->proc))
			res = lappend(res, w);
	}

	return res;
}

/*
 * Start new apply background worker, if possible.
 */
void
logicalrep_worker_launch(Oid dbid, Oid subid, const char *subname, Oid userid,
						 Oid relid)
{
	BackgroundWorker bgw;
	BackgroundWorkerHandle *bgw_handle;
	uint16		generation;
	int			i;
	int			slot = 0;
	LogicalRepWorker *worker = NULL;
	int			nsyncworkers;
	TimestampTz now;

	ereport(DEBUG1,
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

retry:
	/* Find unused worker slot. */
	for (i = 0; i < max_logical_replication_workers; i++)
	{
		LogicalRepWorker *w = &LogicalRepCtx->workers[i];

		if (!w->in_use)
		{
			worker = w;
			slot = i;
			break;
		}
	}

	nsyncworkers = logicalrep_sync_worker_count(subid);

	now = GetCurrentTimestamp();

	/*
	 * If we didn't find a free slot, try to do garbage collection.  The
	 * reason we do this is because if some worker failed to start up and its
	 * parent has crashed while waiting, the in_use state was never cleared.
	 */
	if (worker == NULL || nsyncworkers >= max_sync_workers_per_subscription)
	{
		bool		did_cleanup = false;

		for (i = 0; i < max_logical_replication_workers; i++)
		{
			LogicalRepWorker *w = &LogicalRepCtx->workers[i];

			/*
			 * If the worker was marked in use but didn't manage to attach in
			 * time, clean it up.
			 */
			if (w->in_use && !w->proc &&
				TimestampDifferenceExceeds(w->launch_time, now,
										   wal_receiver_timeout))
			{
				elog(WARNING,
					 "logical replication worker for subscription %u took too long to start; canceled",
					 w->subid);

				logicalrep_worker_cleanup(w);
				did_cleanup = true;
			}
		}

		if (did_cleanup)
			goto retry;
	}

	/*
	 * We don't allow to invoke more sync workers once we have reached the sync
	 * worker limit per subscription. So, just return silently as we might get
	 * here because of an otherwise harmless race condition.
	 */
	if (OidIsValid(relid) && nsyncworkers >= max_sync_workers_per_subscription)
	{
		LWLockRelease(LogicalRepWorkerLock);
		return;
	}

	/*
	 * However if there are no more free worker slots, inform user about it
	 * before exiting.
	 */
	if (worker == NULL)
	{
		LWLockRelease(LogicalRepWorkerLock);
		ereport(WARNING,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("out of logical replication worker slots"),
				 errhint("You might need to increase max_logical_replication_workers.")));
		return;
	}

	/* Prepare the worker slot. */
	worker->launch_time = now;
	worker->in_use = true;
	worker->generation++;
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

	/* Before releasing lock, remember generation for future identification. */
	generation = worker->generation;

	LWLockRelease(LogicalRepWorkerLock);

	/* Register the new dynamic worker. */
	memset(&bgw, 0, sizeof(bgw));
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	snprintf(bgw.bgw_library_name, BGW_MAXLEN, "postgres");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "ApplyWorkerMain");
	if (OidIsValid(relid))
		snprintf(bgw.bgw_name, BGW_MAXLEN,
				 "logical replication worker for subscription %u sync %u", subid, relid);
	else
		snprintf(bgw.bgw_name, BGW_MAXLEN,
				 "logical replication worker for subscription %u", subid);
	snprintf(bgw.bgw_type, BGW_MAXLEN, "logical replication worker");

	bgw.bgw_restart_time = BGW_NEVER_RESTART;
	bgw.bgw_notify_pid = MyProcPid;
	bgw.bgw_main_arg = Int32GetDatum(slot);

	if (!RegisterDynamicBackgroundWorker(&bgw, &bgw_handle))
	{
		/* Failed to start worker, so clean up the worker slot. */
		LWLockAcquire(LogicalRepWorkerLock, LW_EXCLUSIVE);
		Assert(generation == worker->generation);
		logicalrep_worker_cleanup(worker);
		LWLockRelease(LogicalRepWorkerLock);

		ereport(WARNING,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("out of background worker slots"),
				 errhint("You might need to increase max_worker_processes.")));
		return;
	}

	/* Now wait until it attaches. */
	WaitForReplicationWorkerAttach(worker, generation, bgw_handle);
}

/*
 * Stop the logical replication worker for subid/relid, if any, and wait until
 * it detaches from the slot.
 */
void
logicalrep_worker_stop(Oid subid, Oid relid)
{
	LogicalRepWorker *worker;
	uint16		generation;

	LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

	worker = logicalrep_worker_find(subid, relid, false);

	/* No worker, nothing to do. */
	if (!worker)
	{
		LWLockRelease(LogicalRepWorkerLock);
		return;
	}

	/*
	 * Remember which generation was our worker so we can check if what we see
	 * is still the same one.
	 */
	generation = worker->generation;

	/*
	 * If we found a worker but it does not have proc set then it is still
	 * starting up; wait for it to finish starting and then kill it.
	 */
	while (worker->in_use && !worker->proc)
	{
		int			rc;

		LWLockRelease(LogicalRepWorkerLock);

		/* Wait a bit --- we don't expect to have to wait long. */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   10L, WAIT_EVENT_BGWORKER_STARTUP);

		if (rc & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}

		/* Recheck worker status. */
		LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

		/*
		 * Check whether the worker slot is no longer used, which would mean
		 * that the worker has exited, or whether the worker generation is
		 * different, meaning that a different worker has taken the slot.
		 */
		if (!worker->in_use || worker->generation != generation)
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

	/* ... and wait for it to die. */
	for (;;)
	{
		int			rc;

		/* is it gone? */
		if (!worker->proc || worker->generation != generation)
			break;

		LWLockRelease(LogicalRepWorkerLock);

		/* Wait a bit --- we don't expect to have to wait long. */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   10L, WAIT_EVENT_BGWORKER_SHUTDOWN);

		if (rc & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}

		LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);
	}

	LWLockRelease(LogicalRepWorkerLock);
}

/*
 * Request worker for specified sub/rel to be stopped on commit.
 */
void
logicalrep_worker_stop_at_commit(Oid subid, Oid relid)
{
	int			nestDepth = GetCurrentTransactionNestLevel();
	LogicalRepWorkerId *wid;
	MemoryContext oldctx;

	/* Make sure we store the info in context that survives until commit. */
	oldctx = MemoryContextSwitchTo(TopTransactionContext);

	/* Check that previous transactions were properly cleaned up. */
	Assert(on_commit_stop_workers == NULL ||
		   nestDepth >= on_commit_stop_workers->nestDepth);

	/*
	 * Push a new stack element if we don't already have one for the current
	 * nestDepth.
	 */
	if (on_commit_stop_workers == NULL ||
		nestDepth > on_commit_stop_workers->nestDepth)
	{
		StopWorkersData *newdata = palloc(sizeof(StopWorkersData));

		newdata->nestDepth = nestDepth;
		newdata->workers = NIL;
		newdata->parent = on_commit_stop_workers;
		on_commit_stop_workers = newdata;
	}

	/*
	 * Finally add a new worker into the worker list of the current
	 * subtransaction.
	 */
	wid = palloc(sizeof(LogicalRepWorkerId));
	wid->subid = subid;
	wid->relid = relid;
	on_commit_stop_workers->workers =
		lappend(on_commit_stop_workers->workers, wid);

	MemoryContextSwitchTo(oldctx);
}

/*
 * Wake up (using latch) any logical replication worker for specified sub/rel.
 */
void
logicalrep_worker_wakeup(Oid subid, Oid relid)
{
	LogicalRepWorker *worker;

	LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

	worker = logicalrep_worker_find(subid, relid, true);

	if (worker)
		logicalrep_worker_wakeup_ptr(worker);

	LWLockRelease(LogicalRepWorkerLock);
}

/*
 * Wake up (using latch) the specified logical replication worker.
 *
 * Caller must hold lock, else worker->proc could change under us.
 */
void
logicalrep_worker_wakeup_ptr(LogicalRepWorker *worker)
{
	Assert(LWLockHeldByMe(LogicalRepWorkerLock));

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

	if (!MyLogicalRepWorker->in_use)
	{
		LWLockRelease(LogicalRepWorkerLock);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical replication worker slot %d is empty, cannot attach",
						slot)));
	}

	if (MyLogicalRepWorker->proc)
	{
		LWLockRelease(LogicalRepWorkerLock);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical replication worker slot %d is already used by "
						"another worker, cannot attach", slot)));
	}

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

	logicalrep_worker_cleanup(MyLogicalRepWorker);

	LWLockRelease(LogicalRepWorkerLock);
}

/*
 * Clean up worker info.
 */
static void
logicalrep_worker_cleanup(LogicalRepWorker *worker)
{
	Assert(LWLockHeldByMeInMode(LogicalRepWorkerLock, LW_EXCLUSIVE));

	worker->in_use = false;
	worker->proc = NULL;
	worker->dbid = InvalidOid;
	worker->userid = InvalidOid;
	worker->subid = InvalidOid;
	worker->relid = InvalidOid;
}

/*
 * Cleanup function for logical replication launcher.
 *
 * Called on logical replication launcher exit.
 */
static void
logicalrep_launcher_onexit(int code, Datum arg)
{
	LogicalRepCtx->launcher_pid = 0;
}

/*
 * Cleanup function.
 *
 * Called on logical replication worker exit.
 */
static void
logicalrep_worker_onexit(int code, Datum arg)
{
	/* Disconnect gracefully from the remote side. */
	if (LogRepWorkerWalRcvConn)
		walrcv_disconnect(LogRepWorkerWalRcvConn);

	logicalrep_worker_detach();

	ApplyLauncherWakeup();
}

/*
 * Count the number of registered (not necessarily running) sync workers
 * for a subscription.
 */
int
logicalrep_sync_worker_count(Oid subid)
{
	int			i;
	int			res = 0;

	Assert(LWLockHeldByMe(LogicalRepWorkerLock));

	/* Search for attached worker for a given subscription id. */
	for (i = 0; i < max_logical_replication_workers; i++)
	{
		LogicalRepWorker *w = &LogicalRepCtx->workers[i];

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

/*
 * ApplyLauncherRegister
 *		Register a background worker running the logical replication launcher.
 */
void
ApplyLauncherRegister(void)
{
	BackgroundWorker bgw;

	if (max_logical_replication_workers == 0)
		return;

	memset(&bgw, 0, sizeof(bgw));
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	snprintf(bgw.bgw_library_name, BGW_MAXLEN, "postgres");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "ApplyLauncherMain");
	snprintf(bgw.bgw_name, BGW_MAXLEN,
			 "logical replication launcher");
	snprintf(bgw.bgw_type, BGW_MAXLEN,
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
		int			slot;

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
 * Check whether current transaction has manipulated logical replication
 * workers.
 */
bool
XactManipulatesLogicalReplicationWorkers(void)
{
	return (on_commit_stop_workers != NULL);
}

/*
 * Wakeup the launcher on commit if requested.
 */
void
AtEOXact_ApplyLauncher(bool isCommit)
{

	Assert(on_commit_stop_workers == NULL ||
		   (on_commit_stop_workers->nestDepth == 1 &&
			on_commit_stop_workers->parent == NULL));

	if (isCommit)
	{
		ListCell   *lc;

		if (on_commit_stop_workers != NULL)
		{
			List	   *workers = on_commit_stop_workers->workers;

			foreach(lc, workers)
			{
				LogicalRepWorkerId *wid = lfirst(lc);

				logicalrep_worker_stop(wid->subid, wid->relid);
			}
		}

		if (on_commit_launcher_wakeup)
			ApplyLauncherWakeup();
	}

	/*
	 * No need to pfree on_commit_stop_workers.  It was allocated in
	 * transaction memory context, which is going to be cleaned soon.
	 */
	on_commit_stop_workers = NULL;
	on_commit_launcher_wakeup = false;
}

/*
 * On commit, merge the current on_commit_stop_workers list into the
 * immediate parent, if present.
 * On rollback, discard the current on_commit_stop_workers list.
 * Pop out the stack.
 */
void
AtEOSubXact_ApplyLauncher(bool isCommit, int nestDepth)
{
	StopWorkersData *parent;

	/* Exit immediately if there's no work to do at this level. */
	if (on_commit_stop_workers == NULL ||
		on_commit_stop_workers->nestDepth < nestDepth)
		return;

	Assert(on_commit_stop_workers->nestDepth == nestDepth);

	parent = on_commit_stop_workers->parent;

	if (isCommit)
	{
		/*
		 * If the upper stack element is not an immediate parent
		 * subtransaction, just decrement the notional nesting depth without
		 * doing any real work.  Else, we need to merge the current workers
		 * list into the parent.
		 */
		if (!parent || parent->nestDepth < nestDepth - 1)
		{
			on_commit_stop_workers->nestDepth--;
			return;
		}

		parent->workers =
			list_concat(parent->workers, on_commit_stop_workers->workers);
	}
	else
	{
		/*
		 * Abandon everything that was done at this nesting level.  Explicitly
		 * free memory to avoid a transaction-lifespan leak.
		 */
		list_free_deep(on_commit_stop_workers->workers);
	}

	/*
	 * We have taken care of the current subtransaction workers list for both
	 * abort or commit. So we are ready to pop the stack.
	 */
	pfree(on_commit_stop_workers);
	on_commit_stop_workers = parent;
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

static void
ApplyLauncherWakeup(void)
{
	if (LogicalRepCtx->launcher_pid != 0)
		kill(LogicalRepCtx->launcher_pid, SIGUSR1);
}

/*
 * Main loop for the apply launcher process.
 */
void
ApplyLauncherMain(Datum main_arg)
{
	TimestampTz last_start_time = 0;

	ereport(DEBUG1,
			(errmsg("logical replication launcher started")));

	before_shmem_exit(logicalrep_launcher_onexit, (Datum) 0);

	Assert(LogicalRepCtx->launcher_pid == 0);
	LogicalRepCtx->launcher_pid = MyProcPid;

	/* Establish signal handlers. */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	/*
	 * Establish connection to nailed catalogs (we only ever access
	 * pg_subscription).
	 */
	BackgroundWorkerInitializeConnection(NULL, NULL, 0);

	/* Enter main loop */
	for (;;)
	{
		int			rc;
		List	   *sublist;
		ListCell   *lc;
		MemoryContext subctx;
		MemoryContext oldctx;
		TimestampTz now;
		long		wait_time = DEFAULT_NAPTIME_PER_CYCLE;

		CHECK_FOR_INTERRUPTS();

		now = GetCurrentTimestamp();

		/* Limit the start retry to once a wal_retrieve_retry_interval */
		if (TimestampDifferenceExceeds(last_start_time, now,
									   wal_retrieve_retry_interval))
		{
			/* Use temporary context for the database list and worker info. */
			subctx = AllocSetContextCreate(TopMemoryContext,
										   "Logical Replication Launcher sublist",
										   ALLOCSET_DEFAULT_SIZES);
			oldctx = MemoryContextSwitchTo(subctx);

			/* search for subscriptions to start or stop. */
			sublist = get_subscription_list();

			/* Start the missing workers for enabled subscriptions. */
			foreach(lc, sublist)
			{
				Subscription *sub = (Subscription *) lfirst(lc);
				LogicalRepWorker *w;

				if (!sub->enabled)
					continue;

				LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);
				w = logicalrep_worker_find(sub->oid, InvalidOid, false);
				LWLockRelease(LogicalRepWorkerLock);

				if (w == NULL)
				{
					last_start_time = now;
					wait_time = wal_retrieve_retry_interval;

					logicalrep_worker_launch(sub->dbid, sub->oid, sub->name,
											 sub->owner, InvalidOid);
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
			 * wal_retrieve_retry_interval since last worker was started, this
			 * usually means crash of the worker, so we should retry in
			 * wal_retrieve_retry_interval again.
			 */
			wait_time = wal_retrieve_retry_interval;
		}

		/* Wait for more work. */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   wait_time,
					   WAIT_EVENT_LOGICAL_LAUNCHER_MAIN);

		if (rc & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}

		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}
	}

	/* Not reachable */
}

/*
 * Is current process the logical replication launcher?
 */
bool
IsLogicalLauncher(void)
{
	return LogicalRepCtx->launcher_pid == MyProcPid;
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
				 errmsg("materialize mode required, but it is not allowed in this context")));

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

	for (i = 0; i < max_logical_replication_workers; i++)
	{
		/* for each row */
		Datum		values[PG_STAT_GET_SUBSCRIPTION_COLS];
		bool		nulls[PG_STAT_GET_SUBSCRIPTION_COLS];
		int			worker_pid;
		LogicalRepWorker worker;

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

		/*
		 * If only a single subscription was requested, and we found it,
		 * break.
		 */
		if (OidIsValid(subid))
			break;
	}

	LWLockRelease(LogicalRepWorkerLock);

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}
