/*-------------------------------------------------------------------------
 * launcher.c
 *	   PostgreSQL logical replication worker launcher process
 *
 * Copyright (c) 2016-2025, PostgreSQL Global Development Group
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
#include "lib/dshash.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "replication/logicallauncher.h"
#include "replication/origin.h"
#include "replication/walreceiver.h"
#include "replication/worker_internal.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/pg_lsn.h"
#include "utils/snapmgr.h"

/* max sleep time between cycles (3min) */
#define DEFAULT_NAPTIME_PER_CYCLE 180000L

/* GUC variables */
int			max_logical_replication_workers = 4;
int			max_sync_workers_per_subscription = 2;
int			max_parallel_apply_workers_per_subscription = 2;

LogicalRepWorker *MyLogicalRepWorker = NULL;

typedef struct LogicalRepCtxStruct
{
	/* Supervisor process. */
	pid_t		launcher_pid;

	/* Hash table holding last start times of subscriptions' apply workers. */
	dsa_handle	last_start_dsa;
	dshash_table_handle last_start_dsh;

	/* Background workers. */
	LogicalRepWorker workers[FLEXIBLE_ARRAY_MEMBER];
} LogicalRepCtxStruct;

static LogicalRepCtxStruct *LogicalRepCtx;

/* an entry in the last-start-times shared hash table */
typedef struct LauncherLastStartTimesEntry
{
	Oid			subid;			/* OID of logrep subscription (hash key) */
	TimestampTz last_start_time;	/* last time its apply worker was started */
} LauncherLastStartTimesEntry;

/* parameters for the last-start-times shared hash table */
static const dshash_parameters dsh_params = {
	sizeof(Oid),
	sizeof(LauncherLastStartTimesEntry),
	dshash_memcmp,
	dshash_memhash,
	dshash_memcpy,
	LWTRANCHE_LAUNCHER_HASH
};

static dsa_area *last_start_times_dsa = NULL;
static dshash_table *last_start_times = NULL;

static bool on_commit_launcher_wakeup = false;


static void ApplyLauncherWakeup(void);
static void logicalrep_launcher_onexit(int code, Datum arg);
static void logicalrep_worker_onexit(int code, Datum arg);
static void logicalrep_worker_detach(void);
static void logicalrep_worker_cleanup(LogicalRepWorker *worker);
static int	logicalrep_pa_worker_count(Oid subid);
static void logicalrep_launcher_attach_dshmem(void);
static void ApplyLauncherSetWorkerStartTime(Oid subid, TimestampTz start_time);
static TimestampTz ApplyLauncherGetWorkerStartTime(Oid subid);


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
	 * Start a transaction so we can access pg_subscription.
	 */
	StartTransactionCommand();

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
 *
 * Returns whether the attach was successful.
 */
static bool
WaitForReplicationWorkerAttach(LogicalRepWorker *worker,
							   uint16 generation,
							   BackgroundWorkerHandle *handle)
{
	bool		result = false;
	bool		dropped_latch = false;

	for (;;)
	{
		BgwHandleStatus status;
		pid_t		pid;
		int			rc;

		CHECK_FOR_INTERRUPTS();

		LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

		/* Worker either died or has started. Return false if died. */
		if (!worker->in_use || worker->proc)
		{
			result = worker->in_use;
			LWLockRelease(LogicalRepWorkerLock);
			break;
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
			break;				/* result is already false */
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
			dropped_latch = true;
		}
	}

	/*
	 * If we had to clear a latch event in order to wait, be sure to restore
	 * it before exiting.  Otherwise caller may miss events.
	 */
	if (dropped_latch)
		SetLatch(MyLatch);

	return result;
}

/*
 * Walks the workers array and searches for one that matches given
 * subscription id and relid.
 *
 * We are only interested in the leader apply worker or table sync worker.
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

		/* Skip parallel apply workers. */
		if (isParallelApplyWorker(w))
			continue;

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
 * Similar to logicalrep_worker_find(), but returns a list of all workers for
 * the subscription, instead of just one.
 */
List *
logicalrep_workers_find(Oid subid, bool only_running, bool acquire_lock)
{
	int			i;
	List	   *res = NIL;

	if (acquire_lock)
		LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

	Assert(LWLockHeldByMe(LogicalRepWorkerLock));

	/* Search for attached worker for a given subscription id. */
	for (i = 0; i < max_logical_replication_workers; i++)
	{
		LogicalRepWorker *w = &LogicalRepCtx->workers[i];

		if (w->in_use && w->subid == subid && (!only_running || w->proc))
			res = lappend(res, w);
	}

	if (acquire_lock)
		LWLockRelease(LogicalRepWorkerLock);

	return res;
}

/*
 * Start new logical replication background worker, if possible.
 *
 * Returns true on success, false on failure.
 */
bool
logicalrep_worker_launch(LogicalRepWorkerType wtype,
						 Oid dbid, Oid subid, const char *subname, Oid userid,
						 Oid relid, dsm_handle subworker_dsm)
{
	BackgroundWorker bgw;
	BackgroundWorkerHandle *bgw_handle;
	uint16		generation;
	int			i;
	int			slot = 0;
	LogicalRepWorker *worker = NULL;
	int			nsyncworkers;
	int			nparallelapplyworkers;
	TimestampTz now;
	bool		is_tablesync_worker = (wtype == WORKERTYPE_TABLESYNC);
	bool		is_parallel_apply_worker = (wtype == WORKERTYPE_PARALLEL_APPLY);

	/*----------
	 * Sanity checks:
	 * - must be valid worker type
	 * - tablesync workers are only ones to have relid
	 * - parallel apply worker is the only kind of subworker
	 */
	Assert(wtype != WORKERTYPE_UNKNOWN);
	Assert(is_tablesync_worker == OidIsValid(relid));
	Assert(is_parallel_apply_worker == (subworker_dsm != DSM_HANDLE_INVALID));

	ereport(DEBUG1,
			(errmsg_internal("starting logical replication worker for subscription \"%s\"",
							 subname)));

	/* Report this after the initial starting message for consistency. */
	if (max_active_replication_origins == 0)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("cannot start logical replication workers when \"max_active_replication_origins\" is 0")));

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
	 * We don't allow to invoke more sync workers once we have reached the
	 * sync worker limit per subscription. So, just return silently as we
	 * might get here because of an otherwise harmless race condition.
	 */
	if (is_tablesync_worker && nsyncworkers >= max_sync_workers_per_subscription)
	{
		LWLockRelease(LogicalRepWorkerLock);
		return false;
	}

	nparallelapplyworkers = logicalrep_pa_worker_count(subid);

	/*
	 * Return false if the number of parallel apply workers reached the limit
	 * per subscription.
	 */
	if (is_parallel_apply_worker &&
		nparallelapplyworkers >= max_parallel_apply_workers_per_subscription)
	{
		LWLockRelease(LogicalRepWorkerLock);
		return false;
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
				 errhint("You might need to increase \"%s\".", "max_logical_replication_workers")));
		return false;
	}

	/* Prepare the worker slot. */
	worker->type = wtype;
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
	worker->stream_fileset = NULL;
	worker->leader_pid = is_parallel_apply_worker ? MyProcPid : InvalidPid;
	worker->parallel_apply = is_parallel_apply_worker;
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
	snprintf(bgw.bgw_library_name, MAXPGPATH, "postgres");

	switch (worker->type)
	{
		case WORKERTYPE_APPLY:
			snprintf(bgw.bgw_function_name, BGW_MAXLEN, "ApplyWorkerMain");
			snprintf(bgw.bgw_name, BGW_MAXLEN,
					 "logical replication apply worker for subscription %u",
					 subid);
			snprintf(bgw.bgw_type, BGW_MAXLEN, "logical replication apply worker");
			break;

		case WORKERTYPE_PARALLEL_APPLY:
			snprintf(bgw.bgw_function_name, BGW_MAXLEN, "ParallelApplyWorkerMain");
			snprintf(bgw.bgw_name, BGW_MAXLEN,
					 "logical replication parallel apply worker for subscription %u",
					 subid);
			snprintf(bgw.bgw_type, BGW_MAXLEN, "logical replication parallel worker");

			memcpy(bgw.bgw_extra, &subworker_dsm, sizeof(dsm_handle));
			break;

		case WORKERTYPE_TABLESYNC:
			snprintf(bgw.bgw_function_name, BGW_MAXLEN, "TablesyncWorkerMain");
			snprintf(bgw.bgw_name, BGW_MAXLEN,
					 "logical replication tablesync worker for subscription %u sync %u",
					 subid,
					 relid);
			snprintf(bgw.bgw_type, BGW_MAXLEN, "logical replication tablesync worker");
			break;

		case WORKERTYPE_UNKNOWN:
			/* Should never happen. */
			elog(ERROR, "unknown worker type");
	}

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
				 errhint("You might need to increase \"%s\".", "max_worker_processes")));
		return false;
	}

	/* Now wait until it attaches. */
	return WaitForReplicationWorkerAttach(worker, generation, bgw_handle);
}

/*
 * Internal function to stop the worker and wait until it detaches from the
 * slot.
 */
static void
logicalrep_worker_stop_internal(LogicalRepWorker *worker, int signo)
{
	uint16		generation;

	Assert(LWLockHeldByMeInMode(LogicalRepWorkerLock, LW_SHARED));

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
			return;

		/* Worker has assigned proc, so it has started. */
		if (worker->proc)
			break;
	}

	/* Now terminate the worker ... */
	kill(worker->proc->pid, signo);

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
}

/*
 * Stop the logical replication worker for subid/relid, if any.
 */
void
logicalrep_worker_stop(Oid subid, Oid relid)
{
	LogicalRepWorker *worker;

	LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

	worker = logicalrep_worker_find(subid, relid, false);

	if (worker)
	{
		Assert(!isParallelApplyWorker(worker));
		logicalrep_worker_stop_internal(worker, SIGTERM);
	}

	LWLockRelease(LogicalRepWorkerLock);
}

/*
 * Stop the given logical replication parallel apply worker.
 *
 * Node that the function sends SIGINT instead of SIGTERM to the parallel apply
 * worker so that the worker exits cleanly.
 */
void
logicalrep_pa_worker_stop(ParallelApplyWorkerInfo *winfo)
{
	int			slot_no;
	uint16		generation;
	LogicalRepWorker *worker;

	SpinLockAcquire(&winfo->shared->mutex);
	generation = winfo->shared->logicalrep_worker_generation;
	slot_no = winfo->shared->logicalrep_worker_slot_no;
	SpinLockRelease(&winfo->shared->mutex);

	Assert(slot_no >= 0 && slot_no < max_logical_replication_workers);

	/*
	 * Detach from the error_mq_handle for the parallel apply worker before
	 * stopping it. This prevents the leader apply worker from trying to
	 * receive the message from the error queue that might already be detached
	 * by the parallel apply worker.
	 */
	if (winfo->error_mq_handle)
	{
		shm_mq_detach(winfo->error_mq_handle);
		winfo->error_mq_handle = NULL;
	}

	LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

	worker = &LogicalRepCtx->workers[slot_no];
	Assert(isParallelApplyWorker(worker));

	/*
	 * Only stop the worker if the generation matches and the worker is alive.
	 */
	if (worker->generation == generation && worker->proc)
		logicalrep_worker_stop_internal(worker, SIGINT);

	LWLockRelease(LogicalRepWorkerLock);
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
 * Stop the parallel apply workers if any, and detach the leader apply worker
 * (cleans up the worker info).
 */
static void
logicalrep_worker_detach(void)
{
	/* Stop the parallel apply workers. */
	if (am_leader_apply_worker())
	{
		List	   *workers;
		ListCell   *lc;

		/*
		 * Detach from the error_mq_handle for all parallel apply workers
		 * before terminating them. This prevents the leader apply worker from
		 * receiving the worker termination message and sending it to logs
		 * when the same is already done by the parallel worker.
		 */
		pa_detach_all_error_mq();

		LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

		workers = logicalrep_workers_find(MyLogicalRepWorker->subid, true, false);
		foreach(lc, workers)
		{
			LogicalRepWorker *w = (LogicalRepWorker *) lfirst(lc);

			if (isParallelApplyWorker(w))
				logicalrep_worker_stop_internal(w, SIGTERM);
		}

		LWLockRelease(LogicalRepWorkerLock);
	}

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

	worker->type = WORKERTYPE_UNKNOWN;
	worker->in_use = false;
	worker->proc = NULL;
	worker->dbid = InvalidOid;
	worker->userid = InvalidOid;
	worker->subid = InvalidOid;
	worker->relid = InvalidOid;
	worker->leader_pid = InvalidPid;
	worker->parallel_apply = false;
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

	/* Cleanup fileset used for streaming transactions. */
	if (MyLogicalRepWorker->stream_fileset != NULL)
		FileSetDeleteAll(MyLogicalRepWorker->stream_fileset);

	/*
	 * Session level locks may be acquired outside of a transaction in
	 * parallel apply mode and will not be released when the worker
	 * terminates, so manually release all locks before the worker exits.
	 *
	 * The locks will be acquired once the worker is initialized.
	 */
	if (!InitializingApplyWorker)
		LockReleaseAll(DEFAULT_LOCKMETHOD, true);

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

		if (isTablesyncWorker(w) && w->subid == subid)
			res++;
	}

	return res;
}

/*
 * Count the number of registered (but not necessarily running) parallel apply
 * workers for a subscription.
 */
static int
logicalrep_pa_worker_count(Oid subid)
{
	int			i;
	int			res = 0;

	Assert(LWLockHeldByMe(LogicalRepWorkerLock));

	/*
	 * Scan all attached parallel apply workers, only counting those which
	 * have the given subscription id.
	 */
	for (i = 0; i < max_logical_replication_workers; i++)
	{
		LogicalRepWorker *w = &LogicalRepCtx->workers[i];

		if (isParallelApplyWorker(w) && w->subid == subid)
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

	/*
	 * The logical replication launcher is disabled during binary upgrades, to
	 * prevent logical replication workers from running on the source cluster.
	 * That could cause replication origins to move forward after having been
	 * copied to the target cluster, potentially creating conflicts with the
	 * copied data files.
	 */
	if (max_logical_replication_workers == 0 || IsBinaryUpgrade)
		return;

	memset(&bgw, 0, sizeof(bgw));
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	snprintf(bgw.bgw_library_name, MAXPGPATH, "postgres");
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

		LogicalRepCtx->last_start_dsa = DSA_HANDLE_INVALID;
		LogicalRepCtx->last_start_dsh = DSHASH_HANDLE_INVALID;

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
 * Initialize or attach to the dynamic shared hash table that stores the
 * last-start times, if not already done.
 * This must be called before accessing the table.
 */
static void
logicalrep_launcher_attach_dshmem(void)
{
	MemoryContext oldcontext;

	/* Quick exit if we already did this. */
	if (LogicalRepCtx->last_start_dsh != DSHASH_HANDLE_INVALID &&
		last_start_times != NULL)
		return;

	/* Otherwise, use a lock to ensure only one process creates the table. */
	LWLockAcquire(LogicalRepWorkerLock, LW_EXCLUSIVE);

	/* Be sure any local memory allocated by DSA routines is persistent. */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	if (LogicalRepCtx->last_start_dsh == DSHASH_HANDLE_INVALID)
	{
		/* Initialize dynamic shared hash table for last-start times. */
		last_start_times_dsa = dsa_create(LWTRANCHE_LAUNCHER_DSA);
		dsa_pin(last_start_times_dsa);
		dsa_pin_mapping(last_start_times_dsa);
		last_start_times = dshash_create(last_start_times_dsa, &dsh_params, NULL);

		/* Store handles in shared memory for other backends to use. */
		LogicalRepCtx->last_start_dsa = dsa_get_handle(last_start_times_dsa);
		LogicalRepCtx->last_start_dsh = dshash_get_hash_table_handle(last_start_times);
	}
	else if (!last_start_times)
	{
		/* Attach to existing dynamic shared hash table. */
		last_start_times_dsa = dsa_attach(LogicalRepCtx->last_start_dsa);
		dsa_pin_mapping(last_start_times_dsa);
		last_start_times = dshash_attach(last_start_times_dsa, &dsh_params,
										 LogicalRepCtx->last_start_dsh, NULL);
	}

	MemoryContextSwitchTo(oldcontext);
	LWLockRelease(LogicalRepWorkerLock);
}

/*
 * Set the last-start time for the subscription.
 */
static void
ApplyLauncherSetWorkerStartTime(Oid subid, TimestampTz start_time)
{
	LauncherLastStartTimesEntry *entry;
	bool		found;

	logicalrep_launcher_attach_dshmem();

	entry = dshash_find_or_insert(last_start_times, &subid, &found);
	entry->last_start_time = start_time;
	dshash_release_lock(last_start_times, entry);
}

/*
 * Return the last-start time for the subscription, or 0 if there isn't one.
 */
static TimestampTz
ApplyLauncherGetWorkerStartTime(Oid subid)
{
	LauncherLastStartTimesEntry *entry;
	TimestampTz ret;

	logicalrep_launcher_attach_dshmem();

	entry = dshash_find(last_start_times, &subid, false);
	if (entry == NULL)
		return 0;

	ret = entry->last_start_time;
	dshash_release_lock(last_start_times, entry);

	return ret;
}

/*
 * Remove the last-start-time entry for the subscription, if one exists.
 *
 * This has two use-cases: to remove the entry related to a subscription
 * that's been deleted or disabled (just to avoid leaking shared memory),
 * and to allow immediate restart of an apply worker that has exited
 * due to subscription parameter changes.
 */
void
ApplyLauncherForgetWorkerStartTime(Oid subid)
{
	logicalrep_launcher_attach_dshmem();

	(void) dshash_delete_key(last_start_times, &subid);
}

/*
 * Wakeup the launcher on commit if requested.
 */
void
AtEOXact_ApplyLauncher(bool isCommit)
{
	if (isCommit)
	{
		if (on_commit_launcher_wakeup)
			ApplyLauncherWakeup();
	}

	on_commit_launcher_wakeup = false;
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
	ereport(DEBUG1,
			(errmsg_internal("logical replication launcher started")));

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
		long		wait_time = DEFAULT_NAPTIME_PER_CYCLE;

		CHECK_FOR_INTERRUPTS();

		/* Use temporary context to avoid leaking memory across cycles. */
		subctx = AllocSetContextCreate(TopMemoryContext,
									   "Logical Replication Launcher sublist",
									   ALLOCSET_DEFAULT_SIZES);
		oldctx = MemoryContextSwitchTo(subctx);

		/* Start any missing workers for enabled subscriptions. */
		sublist = get_subscription_list();
		foreach(lc, sublist)
		{
			Subscription *sub = (Subscription *) lfirst(lc);
			LogicalRepWorker *w;
			TimestampTz last_start;
			TimestampTz now;
			long		elapsed;

			if (!sub->enabled)
				continue;

			LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);
			w = logicalrep_worker_find(sub->oid, InvalidOid, false);
			LWLockRelease(LogicalRepWorkerLock);

			if (w != NULL)
				continue;		/* worker is running already */

			/*
			 * If the worker is eligible to start now, launch it.  Otherwise,
			 * adjust wait_time so that we'll wake up as soon as it can be
			 * started.
			 *
			 * Each subscription's apply worker can only be restarted once per
			 * wal_retrieve_retry_interval, so that errors do not cause us to
			 * repeatedly restart the worker as fast as possible.  In cases
			 * where a restart is expected (e.g., subscription parameter
			 * changes), another process should remove the last-start entry
			 * for the subscription so that the worker can be restarted
			 * without waiting for wal_retrieve_retry_interval to elapse.
			 */
			last_start = ApplyLauncherGetWorkerStartTime(sub->oid);
			now = GetCurrentTimestamp();
			if (last_start == 0 ||
				(elapsed = TimestampDifferenceMilliseconds(last_start, now)) >= wal_retrieve_retry_interval)
			{
				ApplyLauncherSetWorkerStartTime(sub->oid, now);
				if (!logicalrep_worker_launch(WORKERTYPE_APPLY,
											  sub->dbid, sub->oid, sub->name,
											  sub->owner, InvalidOid,
											  DSM_HANDLE_INVALID))
				{
					/*
					 * We get here either if we failed to launch a worker
					 * (perhaps for resource-exhaustion reasons) or if we
					 * launched one but it immediately quit.  Either way, it
					 * seems appropriate to try again after
					 * wal_retrieve_retry_interval.
					 */
					wait_time = Min(wait_time,
									wal_retrieve_retry_interval);
				}
			}
			else
			{
				wait_time = Min(wait_time,
								wal_retrieve_retry_interval - elapsed);
			}
		}

		/* Switch back to original memory context. */
		MemoryContextSwitchTo(oldctx);
		/* Clean the temporary memory. */
		MemoryContextDelete(subctx);

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
 * Return the pid of the leader apply worker if the given pid is the pid of a
 * parallel apply worker, otherwise, return InvalidPid.
 */
pid_t
GetLeaderApplyWorkerPid(pid_t pid)
{
	int			leader_pid = InvalidPid;
	int			i;

	LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

	for (i = 0; i < max_logical_replication_workers; i++)
	{
		LogicalRepWorker *w = &LogicalRepCtx->workers[i];

		if (isParallelApplyWorker(w) && w->proc && pid == w->proc->pid)
		{
			leader_pid = w->leader_pid;
			break;
		}
	}

	LWLockRelease(LogicalRepWorkerLock);

	return leader_pid;
}

/*
 * Returns state of the subscriptions.
 */
Datum
pg_stat_get_subscription(PG_FUNCTION_ARGS)
{
#define PG_STAT_GET_SUBSCRIPTION_COLS	10
	Oid			subid = PG_ARGISNULL(0) ? InvalidOid : PG_GETARG_OID(0);
	int			i;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);

	/* Make sure we get consistent view of the workers. */
	LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

	for (i = 0; i < max_logical_replication_workers; i++)
	{
		/* for each row */
		Datum		values[PG_STAT_GET_SUBSCRIPTION_COLS] = {0};
		bool		nulls[PG_STAT_GET_SUBSCRIPTION_COLS] = {0};
		int			worker_pid;
		LogicalRepWorker worker;

		memcpy(&worker, &LogicalRepCtx->workers[i],
			   sizeof(LogicalRepWorker));
		if (!worker.proc || !IsBackendPid(worker.proc->pid))
			continue;

		if (OidIsValid(subid) && worker.subid != subid)
			continue;

		worker_pid = worker.proc->pid;

		values[0] = ObjectIdGetDatum(worker.subid);
		if (isTablesyncWorker(&worker))
			values[1] = ObjectIdGetDatum(worker.relid);
		else
			nulls[1] = true;
		values[2] = Int32GetDatum(worker_pid);

		if (isParallelApplyWorker(&worker))
			values[3] = Int32GetDatum(worker.leader_pid);
		else
			nulls[3] = true;

		if (XLogRecPtrIsInvalid(worker.last_lsn))
			nulls[4] = true;
		else
			values[4] = LSNGetDatum(worker.last_lsn);
		if (worker.last_send_time == 0)
			nulls[5] = true;
		else
			values[5] = TimestampTzGetDatum(worker.last_send_time);
		if (worker.last_recv_time == 0)
			nulls[6] = true;
		else
			values[6] = TimestampTzGetDatum(worker.last_recv_time);
		if (XLogRecPtrIsInvalid(worker.reply_lsn))
			nulls[7] = true;
		else
			values[7] = LSNGetDatum(worker.reply_lsn);
		if (worker.reply_time == 0)
			nulls[8] = true;
		else
			values[8] = TimestampTzGetDatum(worker.reply_time);

		switch (worker.type)
		{
			case WORKERTYPE_APPLY:
				values[9] = CStringGetTextDatum("apply");
				break;
			case WORKERTYPE_PARALLEL_APPLY:
				values[9] = CStringGetTextDatum("parallel apply");
				break;
			case WORKERTYPE_TABLESYNC:
				values[9] = CStringGetTextDatum("table synchronization");
				break;
			case WORKERTYPE_UNKNOWN:
				/* Should never happen. */
				elog(ERROR, "unknown worker type");
		}

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);

		/*
		 * If only a single subscription was requested, and we found it,
		 * break.
		 */
		if (OidIsValid(subid))
			break;
	}

	LWLockRelease(LogicalRepWorkerLock);

	return (Datum) 0;
}
