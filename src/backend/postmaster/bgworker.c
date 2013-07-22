/*--------------------------------------------------------------------
 * bgworker.c
 *		POSTGRES pluggable background workers implementation
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/bgworker.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "postmaster/bgworker_internals.h"
#include "storage/barrier.h"
#include "storage/lwlock.h"
#include "storage/pmsignal.h"
#include "storage/shmem.h"
#include "utils/ascii.h"

/*
 * The postmaster's list of registered background workers, in private memory.
 */
slist_head BackgroundWorkerList = SLIST_STATIC_INIT(BackgroundWorkerList);

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
	bool	in_use;
	BackgroundWorker worker;
} BackgroundWorkerSlot;

typedef struct BackgroundWorkerArray
{
	int		total_slots;
	BackgroundWorkerSlot slot[FLEXIBLE_ARRAY_MEMBER];
} BackgroundWorkerArray;

BackgroundWorkerArray *BackgroundWorkerData;

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
		 * Copy contents of worker list into shared memory.  Record the
		 * shared memory slot assigned to each worker.  This ensures
		 * a 1-to-1 correspondence betwen the postmaster's private list and
		 * the array in shared memory.
		 */
		slist_foreach(siter, &BackgroundWorkerList)
		{
			BackgroundWorkerSlot *slot = &BackgroundWorkerData->slot[slotno];
			RegisteredBgWorker *rw;

			rw = slist_container(RegisteredBgWorker, rw_lnode, siter.cur);
			Assert(slotno < max_worker_processes);
			slot->in_use = true;
			rw->rw_shmem_slot = slotno;
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

static RegisteredBgWorker *
FindRegisteredWorkerBySlotNumber(int slotno)
{
	slist_iter	siter;

	/*
	 * Copy contents of worker list into shared memory.  Record the
	 * shared memory slot assigned to each worker.  This ensures
	 * a 1-to-1 correspondence betwen the postmaster's private list and
	 * the array in shared memory.
	 */
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
 * Notice changes to shared_memory made by other backends.  This code
 * runs in the postmaster, so we must be very careful not to assume that
 * shared memory contents are sane.  Otherwise, a rogue backend could take
 * out the postmaster.
 */
void
BackgroundWorkerStateChange(void)
{
	int		slotno;

	/*
	 * The total number of slots stored in shared memory should match our
	 * notion of max_worker_processes.  If it does not, something is very
	 * wrong.  Further down, we always refer to this value as
	 * max_worker_processes, in case shared memory gets corrupted while
	 * we're looping.
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
	 * Iterate through slots, looking for newly-registered workers or
	 * workers who must die.
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

		/*
		 * See whether we already know about this worker.  If not, we need
		 * to update our backend-private BackgroundWorkerList to match shared
		 * memory.
		 */
		rw = FindRegisteredWorkerBySlotNumber(slotno);
		if (rw != NULL)
			continue;

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
		 * Copy strings in a paranoid way.  If shared memory is corrupted,
		 * the source data might not even be NUL-terminated.
		 */
		ascii_safe_strlcpy(rw->rw_worker.bgw_name,
						   slot->worker.bgw_name, BGW_MAXLEN);
		ascii_safe_strlcpy(rw->rw_worker.bgw_library_name,
						   slot->worker.bgw_library_name, BGW_MAXLEN);
		ascii_safe_strlcpy(rw->rw_worker.bgw_function_name,
						   slot->worker.bgw_function_name, BGW_MAXLEN);

		/*
		 * Copy remaining fields.
		 *
		 * flags, start_time, and restart_time are examined by the
		 * postmaster, but nothing too bad will happen if they are
		 * corrupted.  The remaining fields will only be examined by the
		 * child process.  It might crash, but we won't.
		 */
		rw->rw_worker.bgw_flags = slot->worker.bgw_flags;
		rw->rw_worker.bgw_start_time = slot->worker.bgw_start_time;
		rw->rw_worker.bgw_restart_time = slot->worker.bgw_restart_time;
		rw->rw_worker.bgw_main = slot->worker.bgw_main;
		rw->rw_worker.bgw_main_arg = slot->worker.bgw_main_arg;

		/* Initialize postmaster bookkeeping. */
		rw->rw_backend = NULL;
		rw->rw_pid = 0;
		rw->rw_child_slot = 0;
		rw->rw_crashed_at = 0;
		rw->rw_shmem_slot = slotno;

		/* Log it! */
		ereport(LOG,
				(errmsg("registering background worker: %s",
					rw->rw_worker.bgw_name)));

		slist_push_head(&BackgroundWorkerList, &rw->rw_lnode);
	}
}

/*
 * Forget about a background worker that's no longer needed.
 *
 * At present, this only happens when a background worker marked
 * BGW_NEVER_RESTART exits.  This function should only be invoked in
 * the postmaster.
 */
void
ForgetBackgroundWorker(RegisteredBgWorker *rw)
{
	BackgroundWorkerSlot *slot;

	Assert(rw->rw_shmem_slot < max_worker_processes);
	slot = &BackgroundWorkerData->slot[rw->rw_shmem_slot];
	slot->in_use = false;

	ereport(LOG,
			(errmsg("unregistering background worker: %s",
				rw->rw_worker.bgw_name)));

	slist_delete(&BackgroundWorkerList, &rw->rw_lnode);
	free(rw);
}

#ifdef EXEC_BACKEND
/*
 * In EXEC_BACKEND mode, workers use this to retrieve their details from
 * shared memory.
 */
BackgroundWorker *
BackgroundWorkerEntry(int slotno)
{
	BackgroundWorkerSlot *slot;

	Assert(slotno < BackgroundWorkerData->total_slots);
	slot = &BackgroundWorkerData->slot[slotno];
	Assert(slot->in_use);
	return &slot->worker;		/* can't become free while we're still here */
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
		ereport(LOG,
			(errmsg("registering background worker: %s", worker->bgw_name)));

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

	slist_push_head(&BackgroundWorkerList, &rw->rw_lnode);
}

/*
 * Register a new background worker from a regular backend.
 *
 * Returns true on success and false on failure.  Failure typically indicates
 * that no background worker slots are currently available.
 */
bool
RegisterDynamicBackgroundWorker(BackgroundWorker *worker)
{
	int		slotno;
	bool	success = false;

	/*
	 * We can't register dynamic background workers from the postmaster.
	 * If this is a standalone backend, we're the only process and can't
	 * start any more.  In a multi-process environement, it might be
	 * theoretically possible, but we don't currently support it due to
	 * locking considerations; see comments on the BackgroundWorkerSlot
	 * data structure.
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

			/*
			 * Make sure postmaster doesn't see the slot as in use before
			 * it sees the new contents.
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

	return success;
}
