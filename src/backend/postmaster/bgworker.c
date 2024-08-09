/*--------------------------------------------------------------------
 * bgworker.c
 *		POSTGRES pluggable background workers implementation
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/bgworker.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/parallel.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/atomics.h"
#include "postmaster/bgworker_internals.h"
#include "postmaster/postmaster.h"
#include "replication/logicallauncher.h"
#include "replication/logicalworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/shmem.h"
#include "tcop/tcopprot.h"
#include "utils/ascii.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/timeout.h"

/*
 * The postmaster's list of registered background workers, in private memory.
 */
dlist_head	BackgroundWorkerList = DLIST_STATIC_INIT(BackgroundWorkerList);

/*
 * BackgroundWorkerSlots exist in shared memory and can be accessed (via
 * the BackgroundWorkerArray) by both the postmaster and by regular backends.
 * However, the postmaster cannot take locks, even spinlocks, because this
 * might allow it to crash or become wedged if shared memory gets corrupted.
 * Such an outcome is intolerable.  Therefore, we need a lockless protocol
 * for coordinating access to this data.
 *
 * The 'in_use' flag is used to hand off responsibility for the slot between
 * the postmaster and the rest of the system.  When 'in_use' is false,
 * the postmaster will ignore the slot entirely, except for the 'in_use' flag
 * itself, which it may read.  In this state, regular backends may modify the
 * slot.  Once a backend sets 'in_use' to true, the slot becomes the
 * responsibility of the postmaster.  Regular backends may no longer modify it,
 * but the postmaster may examine it.  Thus, a backend initializing a slot
 * must fully initialize the slot - and insert a write memory barrier - before
 * marking it as in use.
 *
 * As an exception, however, even when the slot is in use, regular backends
 * may set the 'terminate' flag for a slot, telling the postmaster not
 * to restart it.  Once the background worker is no longer running, the slot
 * will be released for reuse.
 *
 * In addition to coordinating with the postmaster, backends modifying this
 * data structure must coordinate with each other.  Since they can take locks,
 * this is straightforward: any backend wishing to manipulate a slot must
 * take BackgroundWorkerLock in exclusive mode.  Backends wishing to read
 * data that might get concurrently modified by other backends should take
 * this lock in shared mode.  No matter what, backends reading this data
 * structure must be able to tolerate concurrent modifications by the
 * postmaster.
 */
typedef struct BackgroundWorkerSlot
{
	bool		in_use;
	bool		terminate;
	pid_t		pid;			/* InvalidPid = not started yet; 0 = dead */
	uint64		generation;		/* incremented when slot is recycled */
	BackgroundWorker worker;
} BackgroundWorkerSlot;

/*
 * In order to limit the total number of parallel workers (according to
 * max_parallel_workers GUC), we maintain the number of active parallel
 * workers.  Since the postmaster cannot take locks, two variables are used for
 * this purpose: the number of registered parallel workers (modified by the
 * backends, protected by BackgroundWorkerLock) and the number of terminated
 * parallel workers (modified only by the postmaster, lockless).  The active
 * number of parallel workers is the number of registered workers minus the
 * terminated ones.  These counters can of course overflow, but it's not
 * important here since the subtraction will still give the right number.
 */
typedef struct BackgroundWorkerArray
{
	int			total_slots;
	uint32		parallel_register_count;
	uint32		parallel_terminate_count;
	BackgroundWorkerSlot slot[FLEXIBLE_ARRAY_MEMBER];
} BackgroundWorkerArray;

struct BackgroundWorkerHandle
{
	int			slot;
	uint64		generation;
};

static BackgroundWorkerArray *BackgroundWorkerData;

/*
 * List of internal background worker entry points.  We need this for
 * reasons explained in LookupBackgroundWorkerFunction(), below.
 */
static const struct
{
	const char *fn_name;
	bgworker_main_type fn_addr;
}			InternalBGWorkers[] =

{
	{
		"ParallelWorkerMain", ParallelWorkerMain
	},
	{
		"ApplyLauncherMain", ApplyLauncherMain
	},
	{
		"ApplyWorkerMain", ApplyWorkerMain
	},
	{
		"ParallelApplyWorkerMain", ParallelApplyWorkerMain
	},
	{
		"TablesyncWorkerMain", TablesyncWorkerMain
	}
};

/* Private functions. */
static bgworker_main_type LookupBackgroundWorkerFunction(const char *libraryname, const char *funcname);


/*
 * Calculate shared memory needed.
 */
Size
BackgroundWorkerShmemSize(void)
{
	Size		size;

	/* Array of workers is variably sized. */
	size = offsetof(BackgroundWorkerArray, slot);
	size = add_size(size, mul_size(max_worker_processes,
								   sizeof(BackgroundWorkerSlot)));

	return size;
}

/*
 * Initialize shared memory.
 */
void
BackgroundWorkerShmemInit(void)
{
	bool		found;

	BackgroundWorkerData = ShmemInitStruct("Background Worker Data",
										   BackgroundWorkerShmemSize(),
										   &found);
	if (!IsUnderPostmaster)
	{
		dlist_iter	iter;
		int			slotno = 0;

		BackgroundWorkerData->total_slots = max_worker_processes;
		BackgroundWorkerData->parallel_register_count = 0;
		BackgroundWorkerData->parallel_terminate_count = 0;

		/*
		 * Copy contents of worker list into shared memory.  Record the shared
		 * memory slot assigned to each worker.  This ensures a 1-to-1
		 * correspondence between the postmaster's private list and the array
		 * in shared memory.
		 */
		dlist_foreach(iter, &BackgroundWorkerList)
		{
			BackgroundWorkerSlot *slot = &BackgroundWorkerData->slot[slotno];
			RegisteredBgWorker *rw;

			rw = dlist_container(RegisteredBgWorker, rw_lnode, iter.cur);
			Assert(slotno < max_worker_processes);
			slot->in_use = true;
			slot->terminate = false;
			slot->pid = InvalidPid;
			slot->generation = 0;
			rw->rw_shmem_slot = slotno;
			rw->rw_worker.bgw_notify_pid = 0;	/* might be reinit after crash */
			memcpy(&slot->worker, &rw->rw_worker, sizeof(BackgroundWorker));
			++slotno;
		}

		/*
		 * Mark any remaining slots as not in use.
		 */
		while (slotno < max_worker_processes)
		{
			BackgroundWorkerSlot *slot = &BackgroundWorkerData->slot[slotno];

			slot->in_use = false;
			++slotno;
		}
	}
	else
		Assert(found);
}

/*
 * Search the postmaster's backend-private list of RegisteredBgWorker objects
 * for the one that maps to the given slot number.
 */
static RegisteredBgWorker *
FindRegisteredWorkerBySlotNumber(int slotno)
{
	dlist_iter	iter;

	dlist_foreach(iter, &BackgroundWorkerList)
	{
		RegisteredBgWorker *rw;

		rw = dlist_container(RegisteredBgWorker, rw_lnode, iter.cur);
		if (rw->rw_shmem_slot == slotno)
			return rw;
	}

	return NULL;
}

/*
 * Notice changes to shared memory made by other backends.
 * Accept new worker requests only if allow_new_workers is true.
 *
 * This code runs in the postmaster, so we must be very careful not to assume
 * that shared memory contents are sane.  Otherwise, a rogue backend could
 * take out the postmaster.
 */
void
BackgroundWorkerStateChange(bool allow_new_workers)
{
	int			slotno;

	/*
	 * The total number of slots stored in shared memory should match our
	 * notion of max_worker_processes.  If it does not, something is very
	 * wrong.  Further down, we always refer to this value as
	 * max_worker_processes, in case shared memory gets corrupted while we're
	 * looping.
	 */
	if (max_worker_processes != BackgroundWorkerData->total_slots)
	{
		ereport(LOG,
				(errmsg("inconsistent background worker state (max_worker_processes=%d, total_slots=%d)",
						max_worker_processes,
						BackgroundWorkerData->total_slots)));
		return;
	}

	/*
	 * Iterate through slots, looking for newly-registered workers or workers
	 * who must die.
	 */
	for (slotno = 0; slotno < max_worker_processes; ++slotno)
	{
		BackgroundWorkerSlot *slot = &BackgroundWorkerData->slot[slotno];
		RegisteredBgWorker *rw;

		if (!slot->in_use)
			continue;

		/*
		 * Make sure we don't see the in_use flag before the updated slot
		 * contents.
		 */
		pg_read_barrier();

		/* See whether we already know about this worker. */
		rw = FindRegisteredWorkerBySlotNumber(slotno);
		if (rw != NULL)
		{
			/*
			 * In general, the worker data can't change after it's initially
			 * registered.  However, someone can set the terminate flag.
			 */
			if (slot->terminate && !rw->rw_terminate)
			{
				rw->rw_terminate = true;
				if (rw->rw_pid != 0)
					kill(rw->rw_pid, SIGTERM);
				else
				{
					/* Report never-started, now-terminated worker as dead. */
					ReportBackgroundWorkerPID(rw);
				}
			}
			continue;
		}

		/*
		 * If we aren't allowing new workers, then immediately mark it for
		 * termination; the next stanza will take care of cleaning it up.
		 * Doing this ensures that any process waiting for the worker will get
		 * awoken, even though the worker will never be allowed to run.
		 */
		if (!allow_new_workers)
			slot->terminate = true;

		/*
		 * If the worker is marked for termination, we don't need to add it to
		 * the registered workers list; we can just free the slot. However, if
		 * bgw_notify_pid is set, the process that registered the worker may
		 * need to know that we've processed the terminate request, so be sure
		 * to signal it.
		 */
		if (slot->terminate)
		{
			int			notify_pid;

			/*
			 * We need a memory barrier here to make sure that the load of
			 * bgw_notify_pid and the update of parallel_terminate_count
			 * complete before the store to in_use.
			 */
			notify_pid = slot->worker.bgw_notify_pid;
			if ((slot->worker.bgw_flags & BGWORKER_CLASS_PARALLEL) != 0)
				BackgroundWorkerData->parallel_terminate_count++;
			slot->pid = 0;

			pg_memory_barrier();
			slot->in_use = false;

			if (notify_pid != 0)
				kill(notify_pid, SIGUSR1);

			continue;
		}

		/*
		 * Copy the registration data into the registered workers list.
		 */
		rw = MemoryContextAllocExtended(PostmasterContext,
										sizeof(RegisteredBgWorker),
										MCXT_ALLOC_NO_OOM | MCXT_ALLOC_ZERO);
		if (rw == NULL)
		{
			ereport(LOG,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
			return;
		}

		/*
		 * Copy strings in a paranoid way.  If shared memory is corrupted, the
		 * source data might not even be NUL-terminated.
		 */
		ascii_safe_strlcpy(rw->rw_worker.bgw_name,
						   slot->worker.bgw_name, BGW_MAXLEN);
		ascii_safe_strlcpy(rw->rw_worker.bgw_type,
						   slot->worker.bgw_type, BGW_MAXLEN);
		ascii_safe_strlcpy(rw->rw_worker.bgw_library_name,
						   slot->worker.bgw_library_name, MAXPGPATH);
		ascii_safe_strlcpy(rw->rw_worker.bgw_function_name,
						   slot->worker.bgw_function_name, BGW_MAXLEN);

		/*
		 * Copy various fixed-size fields.
		 *
		 * flags, start_time, and restart_time are examined by the postmaster,
		 * but nothing too bad will happen if they are corrupted.  The
		 * remaining fields will only be examined by the child process.  It
		 * might crash, but we won't.
		 */
		rw->rw_worker.bgw_flags = slot->worker.bgw_flags;
		rw->rw_worker.bgw_start_time = slot->worker.bgw_start_time;
		rw->rw_worker.bgw_restart_time = slot->worker.bgw_restart_time;
		rw->rw_worker.bgw_main_arg = slot->worker.bgw_main_arg;
		memcpy(rw->rw_worker.bgw_extra, slot->worker.bgw_extra, BGW_EXTRALEN);

		/*
		 * Copy the PID to be notified about state changes, but only if the
		 * postmaster knows about a backend with that PID.  It isn't an error
		 * if the postmaster doesn't know about the PID, because the backend
		 * that requested the worker could have died (or been killed) just
		 * after doing so.  Nonetheless, at least until we get some experience
		 * with how this plays out in the wild, log a message at a relative
		 * high debug level.
		 */
		rw->rw_worker.bgw_notify_pid = slot->worker.bgw_notify_pid;
		if (!PostmasterMarkPIDForWorkerNotify(rw->rw_worker.bgw_notify_pid))
		{
			elog(DEBUG1, "worker notification PID %d is not valid",
				 (int) rw->rw_worker.bgw_notify_pid);
			rw->rw_worker.bgw_notify_pid = 0;
		}

		/* Initialize postmaster bookkeeping. */
		rw->rw_pid = 0;
		rw->rw_crashed_at = 0;
		rw->rw_shmem_slot = slotno;
		rw->rw_terminate = false;

		/* Log it! */
		ereport(DEBUG1,
				(errmsg_internal("registering background worker \"%s\"",
								 rw->rw_worker.bgw_name)));

		dlist_push_head(&BackgroundWorkerList, &rw->rw_lnode);
	}
}

/*
 * Forget about a background worker that's no longer needed.
 *
 * NOTE: The entry is unlinked from BackgroundWorkerList.  If the caller is
 * iterating through it, better use a mutable iterator!
 *
 * Caller is responsible for notifying bgw_notify_pid, if appropriate.
 *
 * This function must be invoked only in the postmaster.
 */
void
ForgetBackgroundWorker(RegisteredBgWorker *rw)
{
	BackgroundWorkerSlot *slot;

	Assert(rw->rw_shmem_slot < max_worker_processes);
	slot = &BackgroundWorkerData->slot[rw->rw_shmem_slot];
	Assert(slot->in_use);

	/*
	 * We need a memory barrier here to make sure that the update of
	 * parallel_terminate_count completes before the store to in_use.
	 */
	if ((rw->rw_worker.bgw_flags & BGWORKER_CLASS_PARALLEL) != 0)
		BackgroundWorkerData->parallel_terminate_count++;

	pg_memory_barrier();
	slot->in_use = false;

	ereport(DEBUG1,
			(errmsg_internal("unregistering background worker \"%s\"",
							 rw->rw_worker.bgw_name)));

	dlist_delete(&rw->rw_lnode);
	pfree(rw);
}

/*
 * Report the PID of a newly-launched background worker in shared memory.
 *
 * This function should only be called from the postmaster.
 */
void
ReportBackgroundWorkerPID(RegisteredBgWorker *rw)
{
	BackgroundWorkerSlot *slot;

	Assert(rw->rw_shmem_slot < max_worker_processes);
	slot = &BackgroundWorkerData->slot[rw->rw_shmem_slot];
	slot->pid = rw->rw_pid;

	if (rw->rw_worker.bgw_notify_pid != 0)
		kill(rw->rw_worker.bgw_notify_pid, SIGUSR1);
}

/*
 * Report that the PID of a background worker is now zero because a
 * previously-running background worker has exited.
 *
 * NOTE: The entry may be unlinked from BackgroundWorkerList.  If the caller
 * is iterating through it, better use a mutable iterator!
 *
 * This function should only be called from the postmaster.
 */
void
ReportBackgroundWorkerExit(RegisteredBgWorker *rw)
{
	BackgroundWorkerSlot *slot;
	int			notify_pid;

	Assert(rw->rw_shmem_slot < max_worker_processes);
	slot = &BackgroundWorkerData->slot[rw->rw_shmem_slot];
	slot->pid = rw->rw_pid;
	notify_pid = rw->rw_worker.bgw_notify_pid;

	/*
	 * If this worker is slated for deregistration, do that before notifying
	 * the process which started it.  Otherwise, if that process tries to
	 * reuse the slot immediately, it might not be available yet.  In theory
	 * that could happen anyway if the process checks slot->pid at just the
	 * wrong moment, but this makes the window narrower.
	 */
	if (rw->rw_terminate ||
		rw->rw_worker.bgw_restart_time == BGW_NEVER_RESTART)
		ForgetBackgroundWorker(rw);

	if (notify_pid != 0)
		kill(notify_pid, SIGUSR1);
}

/*
 * Cancel SIGUSR1 notifications for a PID belonging to an exiting backend.
 *
 * This function should only be called from the postmaster.
 */
void
BackgroundWorkerStopNotifications(pid_t pid)
{
	dlist_iter	iter;

	dlist_foreach(iter, &BackgroundWorkerList)
	{
		RegisteredBgWorker *rw;

		rw = dlist_container(RegisteredBgWorker, rw_lnode, iter.cur);
		if (rw->rw_worker.bgw_notify_pid == pid)
			rw->rw_worker.bgw_notify_pid = 0;
	}
}

/*
 * Cancel any not-yet-started worker requests that have waiting processes.
 *
 * This is called during a normal ("smart" or "fast") database shutdown.
 * After this point, no new background workers will be started, so anything
 * that might be waiting for them needs to be kicked off its wait.  We do
 * that by canceling the bgworker registration entirely, which is perhaps
 * overkill, but since we're shutting down it does not matter whether the
 * registration record sticks around.
 *
 * This function should only be called from the postmaster.
 */
void
ForgetUnstartedBackgroundWorkers(void)
{
	dlist_mutable_iter iter;

	dlist_foreach_modify(iter, &BackgroundWorkerList)
	{
		RegisteredBgWorker *rw;
		BackgroundWorkerSlot *slot;

		rw = dlist_container(RegisteredBgWorker, rw_lnode, iter.cur);
		Assert(rw->rw_shmem_slot < max_worker_processes);
		slot = &BackgroundWorkerData->slot[rw->rw_shmem_slot];

		/* If it's not yet started, and there's someone waiting ... */
		if (slot->pid == InvalidPid &&
			rw->rw_worker.bgw_notify_pid != 0)
		{
			/* ... then zap it, and notify the waiter */
			int			notify_pid = rw->rw_worker.bgw_notify_pid;

			ForgetBackgroundWorker(rw);
			if (notify_pid != 0)
				kill(notify_pid, SIGUSR1);
		}
	}
}

/*
 * Reset background worker crash state.
 *
 * We assume that, after a crash-and-restart cycle, background workers without
 * the never-restart flag should be restarted immediately, instead of waiting
 * for bgw_restart_time to elapse.  On the other hand, workers with that flag
 * should be forgotten immediately, since we won't ever restart them.
 *
 * This function should only be called from the postmaster.
 */
void
ResetBackgroundWorkerCrashTimes(void)
{
	dlist_mutable_iter iter;

	dlist_foreach_modify(iter, &BackgroundWorkerList)
	{
		RegisteredBgWorker *rw;

		rw = dlist_container(RegisteredBgWorker, rw_lnode, iter.cur);

		if (rw->rw_worker.bgw_restart_time == BGW_NEVER_RESTART)
		{
			/*
			 * Workers marked BGW_NEVER_RESTART shouldn't get relaunched after
			 * the crash, so forget about them.  (If we wait until after the
			 * crash to forget about them, and they are parallel workers,
			 * parallel_terminate_count will get incremented after we've
			 * already zeroed parallel_register_count, which would be bad.)
			 */
			ForgetBackgroundWorker(rw);
		}
		else
		{
			/*
			 * The accounting which we do via parallel_register_count and
			 * parallel_terminate_count would get messed up if a worker marked
			 * parallel could survive a crash and restart cycle. All such
			 * workers should be marked BGW_NEVER_RESTART, and thus control
			 * should never reach this branch.
			 */
			Assert((rw->rw_worker.bgw_flags & BGWORKER_CLASS_PARALLEL) == 0);

			/*
			 * Allow this worker to be restarted immediately after we finish
			 * resetting.
			 */
			rw->rw_crashed_at = 0;

			/*
			 * If there was anyone waiting for it, they're history.
			 */
			rw->rw_worker.bgw_notify_pid = 0;
		}
	}
}

/*
 * Complain about the BackgroundWorker definition using error level elevel.
 * Return true if it looks ok, false if not (unless elevel >= ERROR, in
 * which case we won't return at all in the not-OK case).
 */
static bool
SanityCheckBackgroundWorker(BackgroundWorker *worker, int elevel)
{
	/* sanity check for flags */

	/*
	 * We used to support workers not connected to shared memory, but don't
	 * anymore. Thus this is a required flag now. We're not removing the flag
	 * for compatibility reasons and because the flag still provides some
	 * signal when reading code.
	 */
	if (!(worker->bgw_flags & BGWORKER_SHMEM_ACCESS))
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("background worker \"%s\": background workers without shared memory access are not supported",
						worker->bgw_name)));
		return false;
	}

	if (worker->bgw_flags & BGWORKER_BACKEND_DATABASE_CONNECTION)
	{
		if (worker->bgw_start_time == BgWorkerStart_PostmasterStart)
		{
			ereport(elevel,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("background worker \"%s\": cannot request database access if starting at postmaster start",
							worker->bgw_name)));
			return false;
		}

		/* XXX other checks? */
	}

	if ((worker->bgw_restart_time < 0 &&
		 worker->bgw_restart_time != BGW_NEVER_RESTART) ||
		(worker->bgw_restart_time > USECS_PER_DAY / 1000))
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("background worker \"%s\": invalid restart interval",
						worker->bgw_name)));
		return false;
	}

	/*
	 * Parallel workers may not be configured for restart, because the
	 * parallel_register_count/parallel_terminate_count accounting can't
	 * handle parallel workers lasting through a crash-and-restart cycle.
	 */
	if (worker->bgw_restart_time != BGW_NEVER_RESTART &&
		(worker->bgw_flags & BGWORKER_CLASS_PARALLEL) != 0)
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("background worker \"%s\": parallel workers may not be configured for restart",
						worker->bgw_name)));
		return false;
	}

	/*
	 * If bgw_type is not filled in, use bgw_name.
	 */
	if (strcmp(worker->bgw_type, "") == 0)
		strcpy(worker->bgw_type, worker->bgw_name);

	return true;
}

/*
 * Standard SIGTERM handler for background workers
 */
static void
bgworker_die(SIGNAL_ARGS)
{
	sigprocmask(SIG_SETMASK, &BlockSig, NULL);

	ereport(FATAL,
			(errcode(ERRCODE_ADMIN_SHUTDOWN),
			 errmsg("terminating background worker \"%s\" due to administrator command",
					MyBgworkerEntry->bgw_type)));
}

/*
 * Main entry point for background worker processes.
 */
void
BackgroundWorkerMain(char *startup_data, size_t startup_data_len)
{
	sigjmp_buf	local_sigjmp_buf;
	BackgroundWorker *worker;
	bgworker_main_type entrypt;

	if (startup_data == NULL)
		elog(FATAL, "unable to find bgworker entry");
	Assert(startup_data_len == sizeof(BackgroundWorker));
	worker = MemoryContextAlloc(TopMemoryContext, sizeof(BackgroundWorker));
	memcpy(worker, startup_data, sizeof(BackgroundWorker));

	/*
	 * Now that we're done reading the startup data, release postmaster's
	 * working memory context.
	 */
	if (PostmasterContext)
	{
		MemoryContextDelete(PostmasterContext);
		PostmasterContext = NULL;
	}

	MyBgworkerEntry = worker;
	MyBackendType = B_BG_WORKER;
	init_ps_display(worker->bgw_name);

	Assert(GetProcessingMode() == InitProcessing);

	/* Apply PostAuthDelay */
	if (PostAuthDelay > 0)
		pg_usleep(PostAuthDelay * 1000000L);

	/*
	 * Set up signal handlers.
	 */
	if (worker->bgw_flags & BGWORKER_BACKEND_DATABASE_CONNECTION)
	{
		/*
		 * SIGINT is used to signal canceling the current action
		 */
		pqsignal(SIGINT, StatementCancelHandler);
		pqsignal(SIGUSR1, procsignal_sigusr1_handler);
		pqsignal(SIGFPE, FloatExceptionHandler);

		/* XXX Any other handlers needed here? */
	}
	else
	{
		pqsignal(SIGINT, SIG_IGN);
		pqsignal(SIGUSR1, SIG_IGN);
		pqsignal(SIGFPE, SIG_IGN);
	}
	pqsignal(SIGTERM, bgworker_die);
	/* SIGQUIT handler was already set up by InitPostmasterChild */
	pqsignal(SIGHUP, SIG_IGN);

	InitializeTimeouts();		/* establishes SIGALRM handler */

	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR2, SIG_IGN);
	pqsignal(SIGCHLD, SIG_DFL);

	/*
	 * If an exception is encountered, processing resumes here.
	 *
	 * We just need to clean up, report the error, and go away.
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* Since not using PG_TRY, must reset error stack by hand */
		error_context_stack = NULL;

		/* Prevent interrupts while cleaning up */
		HOLD_INTERRUPTS();

		/*
		 * sigsetjmp will have blocked all signals, but we may need to accept
		 * signals while communicating with our parallel leader.  Once we've
		 * done HOLD_INTERRUPTS() it should be safe to unblock signals.
		 */
		BackgroundWorkerUnblockSignals();

		/* Report the error to the parallel leader and the server log */
		EmitErrorReport();

		/*
		 * Do we need more cleanup here?  For shmem-connected bgworkers, we
		 * will call InitProcess below, which will install ProcKill as exit
		 * callback.  That will take care of releasing locks, etc.
		 */

		/* and go away */
		proc_exit(1);
	}

	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	/*
	 * Create a per-backend PGPROC struct in shared memory.  We must do this
	 * before we can use LWLocks or access any shared memory.
	 */
	InitProcess();

	/*
	 * Early initialization.
	 */
	BaseInit();

	/*
	 * Look up the entry point function, loading its library if necessary.
	 */
	entrypt = LookupBackgroundWorkerFunction(worker->bgw_library_name,
											 worker->bgw_function_name);

	/*
	 * Note that in normal processes, we would call InitPostgres here.  For a
	 * worker, however, we don't know what database to connect to, yet; so we
	 * need to wait until the user code does it via
	 * BackgroundWorkerInitializeConnection().
	 */

	/*
	 * Now invoke the user-defined worker code
	 */
	entrypt(worker->bgw_main_arg);

	/* ... and if it returns, we're done */
	proc_exit(0);
}

/*
 * Connect background worker to a database.
 */
void
BackgroundWorkerInitializeConnection(const char *dbname, const char *username, uint32 flags)
{
	BackgroundWorker *worker = MyBgworkerEntry;
	bits32		init_flags = 0; /* never honor session_preload_libraries */

	/* ignore datallowconn? */
	if (flags & BGWORKER_BYPASS_ALLOWCONN)
		init_flags |= INIT_PG_OVERRIDE_ALLOW_CONNS;
	/* ignore rolcanlogin? */
	if (flags & BGWORKER_BYPASS_ROLELOGINCHECK)
		init_flags |= INIT_PG_OVERRIDE_ROLE_LOGIN;

	/* XXX is this the right errcode? */
	if (!(worker->bgw_flags & BGWORKER_BACKEND_DATABASE_CONNECTION))
		ereport(FATAL,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("database connection requirement not indicated during registration")));

	InitPostgres(dbname, InvalidOid,	/* database to connect to */
				 username, InvalidOid,	/* role to connect as */
				 init_flags,
				 NULL);			/* no out_dbname */

	/* it had better not gotten out of "init" mode yet */
	if (!IsInitProcessingMode())
		ereport(ERROR,
				(errmsg("invalid processing mode in background worker")));
	SetProcessingMode(NormalProcessing);
}

/*
 * Connect background worker to a database using OIDs.
 */
void
BackgroundWorkerInitializeConnectionByOid(Oid dboid, Oid useroid, uint32 flags)
{
	BackgroundWorker *worker = MyBgworkerEntry;
	bits32		init_flags = 0; /* never honor session_preload_libraries */

	/* ignore datallowconn? */
	if (flags & BGWORKER_BYPASS_ALLOWCONN)
		init_flags |= INIT_PG_OVERRIDE_ALLOW_CONNS;
	/* ignore rolcanlogin? */
	if (flags & BGWORKER_BYPASS_ROLELOGINCHECK)
		init_flags |= INIT_PG_OVERRIDE_ROLE_LOGIN;

	/* XXX is this the right errcode? */
	if (!(worker->bgw_flags & BGWORKER_BACKEND_DATABASE_CONNECTION))
		ereport(FATAL,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("database connection requirement not indicated during registration")));

	InitPostgres(NULL, dboid,	/* database to connect to */
				 NULL, useroid, /* role to connect as */
				 init_flags,
				 NULL);			/* no out_dbname */

	/* it had better not gotten out of "init" mode yet */
	if (!IsInitProcessingMode())
		ereport(ERROR,
				(errmsg("invalid processing mode in background worker")));
	SetProcessingMode(NormalProcessing);
}

/*
 * Block/unblock signals in a background worker
 */
void
BackgroundWorkerBlockSignals(void)
{
	sigprocmask(SIG_SETMASK, &BlockSig, NULL);
}

void
BackgroundWorkerUnblockSignals(void)
{
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);
}

/*
 * Register a new static background worker.
 *
 * This can only be called directly from postmaster or in the _PG_init
 * function of a module library that's loaded by shared_preload_libraries;
 * otherwise it will have no effect.
 */
void
RegisterBackgroundWorker(BackgroundWorker *worker)
{
	RegisteredBgWorker *rw;
	static int	numworkers = 0;

	/*
	 * Static background workers can only be registered in the postmaster
	 * process.
	 */
	if (IsUnderPostmaster || !IsPostmasterEnvironment)
	{
		/*
		 * In EXEC_BACKEND or single-user mode, we process
		 * shared_preload_libraries in backend processes too.  We cannot
		 * register static background workers at that stage, but many
		 * libraries' _PG_init() functions don't distinguish whether they're
		 * being loaded in the postmaster or in a backend, they just check
		 * process_shared_preload_libraries_in_progress.  It's a bit sloppy,
		 * but for historical reasons we tolerate it.  In EXEC_BACKEND mode,
		 * the background workers should already have been registered when the
		 * library was loaded in postmaster.
		 */
		if (process_shared_preload_libraries_in_progress)
			return;
		ereport(LOG,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("background worker \"%s\": must be registered in \"shared_preload_libraries\"",
						worker->bgw_name)));
		return;
	}

	/*
	 * Cannot register static background workers after calling
	 * BackgroundWorkerShmemInit().
	 */
	if (BackgroundWorkerData != NULL)
		elog(ERROR, "cannot register background worker \"%s\" after shmem init",
			 worker->bgw_name);

	ereport(DEBUG1,
			(errmsg_internal("registering background worker \"%s\"", worker->bgw_name)));

	if (!SanityCheckBackgroundWorker(worker, LOG))
		return;

	if (worker->bgw_notify_pid != 0)
	{
		ereport(LOG,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("background worker \"%s\": only dynamic background workers can request notification",
						worker->bgw_name)));
		return;
	}

	/*
	 * Enforce maximum number of workers.  Note this is overly restrictive: we
	 * could allow more non-shmem-connected workers, because these don't count
	 * towards the MAX_BACKENDS limit elsewhere.  For now, it doesn't seem
	 * important to relax this restriction.
	 */
	if (++numworkers > max_worker_processes)
	{
		ereport(LOG,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("too many background workers"),
				 errdetail_plural("Up to %d background worker can be registered with the current settings.",
								  "Up to %d background workers can be registered with the current settings.",
								  max_worker_processes,
								  max_worker_processes),
				 errhint("Consider increasing the configuration parameter \"%s\".", "max_worker_processes")));
		return;
	}

	/*
	 * Copy the registration data into the registered workers list.
	 */
	rw = MemoryContextAllocExtended(PostmasterContext,
									sizeof(RegisteredBgWorker),
									MCXT_ALLOC_NO_OOM);
	if (rw == NULL)
	{
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
		return;
	}

	rw->rw_worker = *worker;
	rw->rw_pid = 0;
	rw->rw_crashed_at = 0;
	rw->rw_terminate = false;

	dlist_push_head(&BackgroundWorkerList, &rw->rw_lnode);
}

/*
 * Register a new background worker from a regular backend.
 *
 * Returns true on success and false on failure.  Failure typically indicates
 * that no background worker slots are currently available.
 *
 * If handle != NULL, we'll set *handle to a pointer that can subsequently
 * be used as an argument to GetBackgroundWorkerPid().  The caller can
 * free this pointer using pfree(), if desired.
 */
bool
RegisterDynamicBackgroundWorker(BackgroundWorker *worker,
								BackgroundWorkerHandle **handle)
{
	int			slotno;
	bool		success = false;
	bool		parallel;
	uint64		generation = 0;

	/*
	 * We can't register dynamic background workers from the postmaster. If
	 * this is a standalone backend, we're the only process and can't start
	 * any more.  In a multi-process environment, it might be theoretically
	 * possible, but we don't currently support it due to locking
	 * considerations; see comments on the BackgroundWorkerSlot data
	 * structure.
	 */
	if (!IsUnderPostmaster)
		return false;

	if (!SanityCheckBackgroundWorker(worker, ERROR))
		return false;

	parallel = (worker->bgw_flags & BGWORKER_CLASS_PARALLEL) != 0;

	LWLockAcquire(BackgroundWorkerLock, LW_EXCLUSIVE);

	/*
	 * If this is a parallel worker, check whether there are already too many
	 * parallel workers; if so, don't register another one.  Our view of
	 * parallel_terminate_count may be slightly stale, but that doesn't really
	 * matter: we would have gotten the same result if we'd arrived here
	 * slightly earlier anyway.  There's no help for it, either, since the
	 * postmaster must not take locks; a memory barrier wouldn't guarantee
	 * anything useful.
	 */
	if (parallel && (BackgroundWorkerData->parallel_register_count -
					 BackgroundWorkerData->parallel_terminate_count) >=
		max_parallel_workers)
	{
		Assert(BackgroundWorkerData->parallel_register_count -
			   BackgroundWorkerData->parallel_terminate_count <=
			   MAX_PARALLEL_WORKER_LIMIT);
		LWLockRelease(BackgroundWorkerLock);
		return false;
	}

	/*
	 * Look for an unused slot.  If we find one, grab it.
	 */
	for (slotno = 0; slotno < BackgroundWorkerData->total_slots; ++slotno)
	{
		BackgroundWorkerSlot *slot = &BackgroundWorkerData->slot[slotno];

		if (!slot->in_use)
		{
			memcpy(&slot->worker, worker, sizeof(BackgroundWorker));
			slot->pid = InvalidPid; /* indicates not started yet */
			slot->generation++;
			slot->terminate = false;
			generation = slot->generation;
			if (parallel)
				BackgroundWorkerData->parallel_register_count++;

			/*
			 * Make sure postmaster doesn't see the slot as in use before it
			 * sees the new contents.
			 */
			pg_write_barrier();

			slot->in_use = true;
			success = true;
			break;
		}
	}

	LWLockRelease(BackgroundWorkerLock);

	/* If we found a slot, tell the postmaster to notice the change. */
	if (success)
		SendPostmasterSignal(PMSIGNAL_BACKGROUND_WORKER_CHANGE);

	/*
	 * If we found a slot and the user has provided a handle, initialize it.
	 */
	if (success && handle)
	{
		*handle = palloc(sizeof(BackgroundWorkerHandle));
		(*handle)->slot = slotno;
		(*handle)->generation = generation;
	}

	return success;
}

/*
 * Get the PID of a dynamically-registered background worker.
 *
 * If the worker is determined to be running, the return value will be
 * BGWH_STARTED and *pidp will get the PID of the worker process.  If the
 * postmaster has not yet attempted to start the worker, the return value will
 * be BGWH_NOT_YET_STARTED.  Otherwise, the return value is BGWH_STOPPED.
 *
 * BGWH_STOPPED can indicate either that the worker is temporarily stopped
 * (because it is configured for automatic restart and exited non-zero),
 * or that the worker is permanently stopped (because it exited with exit
 * code 0, or was not configured for automatic restart), or even that the
 * worker was unregistered without ever starting (either because startup
 * failed and the worker is not configured for automatic restart, or because
 * TerminateBackgroundWorker was used before the worker was successfully
 * started).
 */
BgwHandleStatus
GetBackgroundWorkerPid(BackgroundWorkerHandle *handle, pid_t *pidp)
{
	BackgroundWorkerSlot *slot;
	pid_t		pid;

	Assert(handle->slot < max_worker_processes);
	slot = &BackgroundWorkerData->slot[handle->slot];

	/*
	 * We could probably arrange to synchronize access to data using memory
	 * barriers only, but for now, let's just keep it simple and grab the
	 * lock.  It seems unlikely that there will be enough traffic here to
	 * result in meaningful contention.
	 */
	LWLockAcquire(BackgroundWorkerLock, LW_SHARED);

	/*
	 * The generation number can't be concurrently changed while we hold the
	 * lock.  The pid, which is updated by the postmaster, can change at any
	 * time, but we assume such changes are atomic.  So the value we read
	 * won't be garbage, but it might be out of date by the time the caller
	 * examines it (but that's unavoidable anyway).
	 *
	 * The in_use flag could be in the process of changing from true to false,
	 * but if it is already false then it can't change further.
	 */
	if (handle->generation != slot->generation || !slot->in_use)
		pid = 0;
	else
		pid = slot->pid;

	/* All done. */
	LWLockRelease(BackgroundWorkerLock);

	if (pid == 0)
		return BGWH_STOPPED;
	else if (pid == InvalidPid)
		return BGWH_NOT_YET_STARTED;
	*pidp = pid;
	return BGWH_STARTED;
}

/*
 * Wait for a background worker to start up.
 *
 * This is like GetBackgroundWorkerPid(), except that if the worker has not
 * yet started, we wait for it to do so; thus, BGWH_NOT_YET_STARTED is never
 * returned.  However, if the postmaster has died, we give up and return
 * BGWH_POSTMASTER_DIED, since it that case we know that startup will not
 * take place.
 *
 * The caller *must* have set our PID as the worker's bgw_notify_pid,
 * else we will not be awoken promptly when the worker's state changes.
 */
BgwHandleStatus
WaitForBackgroundWorkerStartup(BackgroundWorkerHandle *handle, pid_t *pidp)
{
	BgwHandleStatus status;
	int			rc;

	for (;;)
	{
		pid_t		pid;

		CHECK_FOR_INTERRUPTS();

		status = GetBackgroundWorkerPid(handle, &pid);
		if (status == BGWH_STARTED)
			*pidp = pid;
		if (status != BGWH_NOT_YET_STARTED)
			break;

		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_POSTMASTER_DEATH, 0,
					   WAIT_EVENT_BGWORKER_STARTUP);

		if (rc & WL_POSTMASTER_DEATH)
		{
			status = BGWH_POSTMASTER_DIED;
			break;
		}

		ResetLatch(MyLatch);
	}

	return status;
}

/*
 * Wait for a background worker to stop.
 *
 * If the worker hasn't yet started, or is running, we wait for it to stop
 * and then return BGWH_STOPPED.  However, if the postmaster has died, we give
 * up and return BGWH_POSTMASTER_DIED, because it's the postmaster that
 * notifies us when a worker's state changes.
 *
 * The caller *must* have set our PID as the worker's bgw_notify_pid,
 * else we will not be awoken promptly when the worker's state changes.
 */
BgwHandleStatus
WaitForBackgroundWorkerShutdown(BackgroundWorkerHandle *handle)
{
	BgwHandleStatus status;
	int			rc;

	for (;;)
	{
		pid_t		pid;

		CHECK_FOR_INTERRUPTS();

		status = GetBackgroundWorkerPid(handle, &pid);
		if (status == BGWH_STOPPED)
			break;

		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_POSTMASTER_DEATH, 0,
					   WAIT_EVENT_BGWORKER_SHUTDOWN);

		if (rc & WL_POSTMASTER_DEATH)
		{
			status = BGWH_POSTMASTER_DIED;
			break;
		}

		ResetLatch(MyLatch);
	}

	return status;
}

/*
 * Instruct the postmaster to terminate a background worker.
 *
 * Note that it's safe to do this without regard to whether the worker is
 * still running, or even if the worker may already have exited and been
 * unregistered.
 */
void
TerminateBackgroundWorker(BackgroundWorkerHandle *handle)
{
	BackgroundWorkerSlot *slot;
	bool		signal_postmaster = false;

	Assert(handle->slot < max_worker_processes);
	slot = &BackgroundWorkerData->slot[handle->slot];

	/* Set terminate flag in shared memory, unless slot has been reused. */
	LWLockAcquire(BackgroundWorkerLock, LW_EXCLUSIVE);
	if (handle->generation == slot->generation)
	{
		slot->terminate = true;
		signal_postmaster = true;
	}
	LWLockRelease(BackgroundWorkerLock);

	/* Make sure the postmaster notices the change to shared memory. */
	if (signal_postmaster)
		SendPostmasterSignal(PMSIGNAL_BACKGROUND_WORKER_CHANGE);
}

/*
 * Look up (and possibly load) a bgworker entry point function.
 *
 * For functions contained in the core code, we use library name "postgres"
 * and consult the InternalBGWorkers array.  External functions are
 * looked up, and loaded if necessary, using load_external_function().
 *
 * The point of this is to pass function names as strings across process
 * boundaries.  We can't pass actual function addresses because of the
 * possibility that the function has been loaded at a different address
 * in a different process.  This is obviously a hazard for functions in
 * loadable libraries, but it can happen even for functions in the core code
 * on platforms using EXEC_BACKEND (e.g., Windows).
 *
 * At some point it might be worthwhile to get rid of InternalBGWorkers[]
 * in favor of applying load_external_function() for core functions too;
 * but that raises portability issues that are not worth addressing now.
 */
static bgworker_main_type
LookupBackgroundWorkerFunction(const char *libraryname, const char *funcname)
{
	/*
	 * If the function is to be loaded from postgres itself, search the
	 * InternalBGWorkers array.
	 */
	if (strcmp(libraryname, "postgres") == 0)
	{
		int			i;

		for (i = 0; i < lengthof(InternalBGWorkers); i++)
		{
			if (strcmp(InternalBGWorkers[i].fn_name, funcname) == 0)
				return InternalBGWorkers[i].fn_addr;
		}

		/* We can only reach this by programming error. */
		elog(ERROR, "internal function \"%s\" not found", funcname);
	}

	/* Otherwise load from external library. */
	return (bgworker_main_type)
		load_external_function(libraryname, funcname, true, NULL);
}

/*
 * Given a PID, get the bgw_type of the background worker.  Returns NULL if
 * not a valid background worker.
 *
 * The return value is in static memory belonging to this function, so it has
 * to be used before calling this function again.  This is so that the caller
 * doesn't have to worry about the background worker locking protocol.
 */
const char *
GetBackgroundWorkerTypeByPid(pid_t pid)
{
	int			slotno;
	bool		found = false;
	static char result[BGW_MAXLEN];

	LWLockAcquire(BackgroundWorkerLock, LW_SHARED);

	for (slotno = 0; slotno < BackgroundWorkerData->total_slots; slotno++)
	{
		BackgroundWorkerSlot *slot = &BackgroundWorkerData->slot[slotno];

		if (slot->pid > 0 && slot->pid == pid)
		{
			strcpy(result, slot->worker.bgw_type);
			found = true;
			break;
		}
	}

	LWLockRelease(BackgroundWorkerLock);

	if (!found)
		return NULL;

	return result;
}
