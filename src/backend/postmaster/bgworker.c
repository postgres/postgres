/*--------------------------------------------------------------------
 * bgworker.c
 *		POSTGRES pluggable background workers implementation
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/bgworker.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>

#include "miscadmin.h"
#include "libpq/pqsignal.h"
#include "postmaster/bgworker_internals.h"
#include "postmaster/postmaster.h"
#include "storage/barrier.h"
#include "storage/dsm.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/pg_shmem.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/shmem.h"
#include "tcop/tcopprot.h"
#include "utils/ascii.h"
#include "utils/ps_status.h"
#include "utils/timeout.h"

/*
 * The postmaster's list of registered background workers, in private memory.
 */
slist_head	BackgroundWorkerList = SLIST_STATIC_INIT(BackgroundWorkerList);

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

typedef struct BackgroundWorkerArray
{
	int			total_slots;
	BackgroundWorkerSlot slot[FLEXIBLE_ARRAY_MEMBER];
} BackgroundWorkerArray;

struct BackgroundWorkerHandle
{
	int			slot;
	uint64		generation;
};

static BackgroundWorkerArray *BackgroundWorkerData;

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
		slist_iter	siter;
		int			slotno = 0;

		BackgroundWorkerData->total_slots = max_worker_processes;

		/*
		 * Copy contents of worker list into shared memory.  Record the shared
		 * memory slot assigned to each worker.  This ensures a 1-to-1
		 * correspondence between the postmaster's private list and the array
		 * in shared memory.
		 */
		slist_foreach(siter, &BackgroundWorkerList)
		{
			BackgroundWorkerSlot *slot = &BackgroundWorkerData->slot[slotno];
			RegisteredBgWorker *rw;

			rw = slist_container(RegisteredBgWorker, rw_lnode, siter.cur);
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
	slist_iter	siter;

	slist_foreach(siter, &BackgroundWorkerList)
	{
		RegisteredBgWorker *rw;

		rw = slist_container(RegisteredBgWorker, rw_lnode, siter.cur);
		if (rw->rw_shmem_slot == slotno)
			return rw;
	}

	return NULL;
}

/*
 * Notice changes to shared memory made by other backends.  This code
 * runs in the postmaster, so we must be very careful not to assume that
 * shared memory contents are sane.  Otherwise, a rogue backend could take
 * out the postmaster.
 */
void
BackgroundWorkerStateChange(void)
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
		elog(LOG,
			 "inconsistent background worker state (max_worker_processes=%d, total_slots=%d",
			 max_worker_processes,
			 BackgroundWorkerData->total_slots);
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
			 * bgw_notify_pid completes before the store to in_use.
			 */
			notify_pid = slot->worker.bgw_notify_pid;
			pg_memory_barrier();
			slot->pid = 0;
			slot->in_use = false;
			if (notify_pid != 0)
				kill(notify_pid, SIGUSR1);

			continue;
		}

		/*
		 * Copy the registration data into the registered workers list.
		 */
		rw = malloc(sizeof(RegisteredBgWorker));
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
		ascii_safe_strlcpy(rw->rw_worker.bgw_library_name,
						   slot->worker.bgw_library_name, BGW_MAXLEN);
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
		rw->rw_worker.bgw_main = slot->worker.bgw_main;
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
			elog(DEBUG1, "worker notification PID %lu is not valid",
				 (long) rw->rw_worker.bgw_notify_pid);
			rw->rw_worker.bgw_notify_pid = 0;
		}

		/* Initialize postmaster bookkeeping. */
		rw->rw_backend = NULL;
		rw->rw_pid = 0;
		rw->rw_child_slot = 0;
		rw->rw_crashed_at = 0;
		rw->rw_shmem_slot = slotno;
		rw->rw_terminate = false;

		/* Log it! */
		ereport(DEBUG1,
				(errmsg("registering background worker \"%s\"",
						rw->rw_worker.bgw_name)));

		slist_push_head(&BackgroundWorkerList, &rw->rw_lnode);
	}
}

/*
 * Forget about a background worker that's no longer needed.
 *
 * The worker must be identified by passing an slist_mutable_iter that
 * points to it.  This convention allows deletion of workers during
 * searches of the worker list, and saves having to search the list again.
 *
 * This function must be invoked only in the postmaster.
 */
void
ForgetBackgroundWorker(slist_mutable_iter *cur)
{
	RegisteredBgWorker *rw;
	BackgroundWorkerSlot *slot;

	rw = slist_container(RegisteredBgWorker, rw_lnode, cur->cur);

	Assert(rw->rw_shmem_slot < max_worker_processes);
	slot = &BackgroundWorkerData->slot[rw->rw_shmem_slot];
	slot->in_use = false;

	ereport(DEBUG1,
			(errmsg("unregistering background worker \"%s\"",
					rw->rw_worker.bgw_name)));

	slist_delete_current(cur);
	free(rw);
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
 * Cancel SIGUSR1 notifications for a PID belonging to an exiting backend.
 *
 * This function should only be called from the postmaster.
 */
void
BackgroundWorkerStopNotifications(pid_t pid)
{
	slist_iter	siter;

	slist_foreach(siter, &BackgroundWorkerList)
	{
		RegisteredBgWorker *rw;

		rw = slist_container(RegisteredBgWorker, rw_lnode, siter.cur);
		if (rw->rw_worker.bgw_notify_pid == pid)
			rw->rw_worker.bgw_notify_pid = 0;
	}
}

/*
 * Reset background worker crash state.
 *
 * We assume that, after a crash-and-restart cycle, background workers without
 * the never-restart flag should be restarted immediately, instead of waiting
 * for bgw_restart_time to elapse.
 */
void
ResetBackgroundWorkerCrashTimes(void)
{
	slist_mutable_iter iter;

	slist_foreach_modify(iter, &BackgroundWorkerList)
	{
		RegisteredBgWorker *rw;

		rw = slist_container(RegisteredBgWorker, rw_lnode, iter.cur);

		/*
		 * For workers that should not be restarted, we don't want to lose the
		 * information that they have crashed; otherwise, they would be
		 * restarted, which is wrong.
		 */
		if (rw->rw_worker.bgw_restart_time != BGW_NEVER_RESTART)
			rw->rw_crashed_at = 0;
	}
}

#ifdef EXEC_BACKEND
/*
 * In EXEC_BACKEND mode, workers use this to retrieve their details from
 * shared memory.
 */
BackgroundWorker *
BackgroundWorkerEntry(int slotno)
{
	static BackgroundWorker myEntry;
	BackgroundWorkerSlot *slot;

	Assert(slotno < BackgroundWorkerData->total_slots);
	slot = &BackgroundWorkerData->slot[slotno];
	Assert(slot->in_use);

	/* must copy this in case we don't intend to retain shmem access */
	memcpy(&myEntry, &slot->worker, sizeof myEntry);
	return &myEntry;
}
#endif

/*
 * Complain about the BackgroundWorker definition using error level elevel.
 * Return true if it looks ok, false if not (unless elevel >= ERROR, in
 * which case we won't return at all in the not-OK case).
 */
static bool
SanityCheckBackgroundWorker(BackgroundWorker *worker, int elevel)
{
	/* sanity check for flags */
	if (worker->bgw_flags & BGWORKER_BACKEND_DATABASE_CONNECTION)
	{
		if (!(worker->bgw_flags & BGWORKER_SHMEM_ACCESS))
		{
			ereport(elevel,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("background worker \"%s\": must attach to shared memory in order to request a database connection",
							worker->bgw_name)));
			return false;
		}

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

	return true;
}

static void
bgworker_quickdie(SIGNAL_ARGS)
{
	sigaddset(&BlockSig, SIGQUIT);		/* prevent nested calls */
	PG_SETMASK(&BlockSig);

	/*
	 * We DO NOT want to run proc_exit() callbacks -- we're here because
	 * shared memory may be corrupted, so we don't want to try to clean up our
	 * transaction.  Just nail the windows shut and get out of town.  Now that
	 * there's an atexit callback to prevent third-party code from breaking
	 * things by calling exit() directly, we have to reset the callbacks
	 * explicitly to make this work as intended.
	 */
	on_exit_reset();

	/*
	 * Note we do exit(2) not exit(0).  This is to force the postmaster into a
	 * system reset cycle if some idiot DBA sends a manual SIGQUIT to a random
	 * backend.  This is necessary precisely because we don't clean up our
	 * shared memory state.  (The "dead man switch" mechanism in pmsignal.c
	 * should ensure the postmaster sees this as a crash, too, but no harm in
	 * being doubly sure.)
	 */
	exit(2);
}

/*
 * Standard SIGTERM handler for background workers
 */
static void
bgworker_die(SIGNAL_ARGS)
{
	PG_SETMASK(&BlockSig);

	ereport(FATAL,
			(errcode(ERRCODE_ADMIN_SHUTDOWN),
			 errmsg("terminating background worker \"%s\" due to administrator command",
					MyBgworkerEntry->bgw_name)));
}

/*
 * Standard SIGUSR1 handler for unconnected workers
 *
 * Here, we want to make sure an unconnected worker will at least heed
 * latch activity.
 */
static void
bgworker_sigusr1_handler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	latch_sigusr1_handler();

	errno = save_errno;
}

/*
 * Start a new background worker
 *
 * This is the main entry point for background worker, to be called from
 * postmaster.
 */
void
StartBackgroundWorker(void)
{
	sigjmp_buf	local_sigjmp_buf;
	char		buf[MAXPGPATH];
	BackgroundWorker *worker = MyBgworkerEntry;
	bgworker_main_type entrypt;

	if (worker == NULL)
		elog(FATAL, "unable to find bgworker entry");

	IsBackgroundWorker = true;

	/* Identify myself via ps */
	snprintf(buf, MAXPGPATH, "bgworker: %s", worker->bgw_name);
	init_ps_display(buf, "", "", "");

	/*
	 * If we're not supposed to have shared memory access, then detach from
	 * shared memory.  If we didn't request shared memory access, the
	 * postmaster won't force a cluster-wide restart if we exit unexpectedly,
	 * so we'd better make sure that we don't mess anything up that would
	 * require that sort of cleanup.
	 */
	if ((worker->bgw_flags & BGWORKER_SHMEM_ACCESS) == 0)
	{
		on_exit_reset();
		dsm_detach_all();
		PGSharedMemoryDetach();
	}

	SetProcessingMode(InitProcessing);

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
		pqsignal(SIGUSR1, bgworker_sigusr1_handler);
		pqsignal(SIGFPE, SIG_IGN);
	}
	pqsignal(SIGTERM, bgworker_die);
	pqsignal(SIGHUP, SIG_IGN);

	pqsignal(SIGQUIT, bgworker_quickdie);
	InitializeTimeouts();		/* establishes SIGALRM handler */

	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR2, SIG_IGN);
	pqsignal(SIGCHLD, SIG_DFL);

	/*
	 * If an exception is encountered, processing resumes here.
	 *
	 * See notes in postgres.c about the design of this coding.
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* Since not using PG_TRY, must reset error stack by hand */
		error_context_stack = NULL;

		/* Prevent interrupts while cleaning up */
		HOLD_INTERRUPTS();

		/* Report the error to the server log */
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
	 * If the background worker request shared memory access, set that up now;
	 * else, detach all shared memory segments.
	 */
	if (worker->bgw_flags & BGWORKER_SHMEM_ACCESS)
	{
		/*
		 * Early initialization.  Some of this could be useful even for
		 * background workers that aren't using shared memory, but they can
		 * call the individual startup routines for those subsystems if
		 * needed.
		 */
		BaseInit();

		/*
		 * Create a per-backend PGPROC struct in shared memory, except in the
		 * EXEC_BACKEND case where this was done in SubPostmasterMain. We must
		 * do this before we can use LWLocks (and in the EXEC_BACKEND case we
		 * already had to do some stuff with LWLocks).
		 */
#ifndef EXEC_BACKEND
		InitProcess();
#endif
	}

	/*
	 * If bgw_main is set, we use that value as the initial entrypoint.
	 * However, if the library containing the entrypoint wasn't loaded at
	 * postmaster startup time, passing it as a direct function pointer is not
	 * possible.  To work around that, we allow callers for whom a function
	 * pointer is not available to pass a library name (which will be loaded,
	 * if necessary) and a function name (which will be looked up in the named
	 * library).
	 */
	if (worker->bgw_main != NULL)
		entrypt = worker->bgw_main;
	else
		entrypt = (bgworker_main_type)
			load_external_function(worker->bgw_library_name,
								   worker->bgw_function_name,
								   true, NULL);

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
 * Register a new background worker while processing shared_preload_libraries.
 *
 * This can only be called in the _PG_init function of a module library
 * that's loaded by shared_preload_libraries; otherwise it has no effect.
 */
void
RegisterBackgroundWorker(BackgroundWorker *worker)
{
	RegisteredBgWorker *rw;
	static int	numworkers = 0;

	if (!IsUnderPostmaster)
		ereport(DEBUG1,
		 (errmsg("registering background worker \"%s\"", worker->bgw_name)));

	if (!process_shared_preload_libraries_in_progress)
	{
		if (!IsUnderPostmaster)
			ereport(LOG,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("background worker \"%s\": must be registered in shared_preload_libraries",
							worker->bgw_name)));
		return;
	}

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
				 errhint("Consider increasing the configuration parameter \"max_worker_processes\".")));
		return;
	}

	/*
	 * Copy the registration data into the registered workers list.
	 */
	rw = malloc(sizeof(RegisteredBgWorker));
	if (rw == NULL)
	{
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
		return;
	}

	rw->rw_worker = *worker;
	rw->rw_backend = NULL;
	rw->rw_pid = 0;
	rw->rw_child_slot = 0;
	rw->rw_crashed_at = 0;
	rw->rw_terminate = false;

	slist_push_head(&BackgroundWorkerList, &rw->rw_lnode);
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

	LWLockAcquire(BackgroundWorkerLock, LW_EXCLUSIVE);

	/*
	 * Look for an unused slot.  If we find one, grab it.
	 */
	for (slotno = 0; slotno < BackgroundWorkerData->total_slots; ++slotno)
	{
		BackgroundWorkerSlot *slot = &BackgroundWorkerData->slot[slotno];

		if (!slot->in_use)
		{
			memcpy(&slot->worker, worker, sizeof(BackgroundWorker));
			slot->pid = InvalidPid;		/* indicates not started yet */
			slot->generation++;
			slot->terminate = false;
			generation = slot->generation;

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
 * BGWH_STARTED and *pidp will get the PID of the worker process.
 * Otherwise, the return value will be BGWH_NOT_YET_STARTED if the worker
 * hasn't been started yet, and BGWH_STOPPED if the worker was previously
 * running but is no longer.
 *
 * In the latter case, the worker may be stopped temporarily (if it is
 * configured for automatic restart and exited non-zero) or gone for
 * good (if it exited with code 0 or if it is configured not to restart).
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
	 */
	if (handle->generation != slot->generation)
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
					   WL_LATCH_SET | WL_POSTMASTER_DEATH, 0);

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
			return status;

		rc = WaitLatch(&MyProc->procLatch,
					   WL_LATCH_SET | WL_POSTMASTER_DEATH, 0);

		if (rc & WL_POSTMASTER_DEATH)
			return BGWH_POSTMASTER_DIED;

		ResetLatch(&MyProc->procLatch);
	}

	return status;
}

/*
 * Instruct the postmaster to terminate a background worker.
 *
 * Note that it's safe to do this without regard to whether the worker is
 * still running, or even if the worker may already have existed and been
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
