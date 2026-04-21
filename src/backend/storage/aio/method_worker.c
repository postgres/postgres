/*-------------------------------------------------------------------------
 *
 * method_worker.c
 *    AIO - perform AIO using worker processes
 *
 * IO workers consume IOs from a shared memory submission queue, run
 * traditional synchronous system calls, and perform the shared completion
 * handling immediately.  Client code submits most requests by pushing IOs
 * into the submission queue, and waits (if necessary) using condition
 * variables.  Some IOs cannot be performed in another process due to lack of
 * infrastructure for reopening the file, and must processed synchronously by
 * the client code when submitted.
 *
 * The pool of workers tries to stabilize at a size that can handle recently
 * seen variation in demand, within the configured limits.
 *
 * This method of AIO is available in all builds on all operating systems, and
 * is the default.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/aio/method_worker.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "postmaster/auxprocess.h"
#include "postmaster/interrupt.h"
#include "storage/aio.h"
#include "storage/aio_internal.h"
#include "storage/aio_subsys.h"
#include "storage/io_worker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "tcop/tcopprot.h"
#include "utils/injection_point.h"
#include "utils/memdebug.h"
#include "utils/ps_status.h"
#include "utils/wait_event.h"

/*
 * Saturation for counters used to estimate wakeup:IO ratio.
 *
 * We maintain hist_wakeups for wakeups received and hist_ios for IOs
 * processed by each worker.  When either counter reaches this saturation
 * value, we divide both by two.  The result is an exponentially decaying
 * ratio of wakeups to IOs, with a very short memory.
 *
 * If a worker is itself experiencing useless wakeups, it assumes that
 * higher-numbered workers would experience even more, so it should end the
 * chain.
 */
#define PGAIO_WORKER_WAKEUP_RATIO_SATURATE 4

/* Debugging support: show current IO and wakeups:ios statistics in ps. */
/* #define PGAIO_WORKER_SHOW_PS_INFO */

typedef struct PgAioWorkerSubmissionQueue
{
	uint32		size;
	uint32		head;
	uint32		tail;
	int			sqes[FLEXIBLE_ARRAY_MEMBER];
} PgAioWorkerSubmissionQueue;

typedef struct PgAioWorkerSlot
{
	ProcNumber	proc_number;
} PgAioWorkerSlot;

/*
 * Sets of worker IDs are held in a simple bitmap, accessed through functions
 * that provide a more readable abstraction.  If we wanted to support more
 * workers than that, the contention on the single queue would surely get too
 * high, so we might want to consider multiple pools instead of widening this.
 */
typedef uint64 PgAioWorkerSet;

#define PGAIO_WORKERSET_BITS (sizeof(PgAioWorkerSet) * CHAR_BIT)

static_assert(PGAIO_WORKERSET_BITS >= MAX_IO_WORKERS, "too small");

typedef struct PgAioWorkerControl
{
	/* Seen by postmaster */
	bool		grow;
	bool		grow_signal_sent;

	/* Protected by AioWorkerSubmissionQueueLock. */
	PgAioWorkerSet idle_workerset;

	/* Protected by AioWorkerControlLock. */
	PgAioWorkerSet workerset;
	int			nworkers;

	/* Protected by AioWorkerControlLock. */
	PgAioWorkerSlot workers[FLEXIBLE_ARRAY_MEMBER];
} PgAioWorkerControl;


static void pgaio_worker_shmem_request(void *arg);
static void pgaio_worker_shmem_init(void *arg);

static bool pgaio_worker_needs_synchronous_execution(PgAioHandle *ioh);
static int	pgaio_worker_submit(uint16 num_staged_ios, PgAioHandle **staged_ios);


const IoMethodOps pgaio_worker_ops = {
	.shmem_callbacks.request_fn = pgaio_worker_shmem_request,
	.shmem_callbacks.init_fn = pgaio_worker_shmem_init,

	.needs_synchronous_execution = pgaio_worker_needs_synchronous_execution,
	.submit = pgaio_worker_submit,
};


/* GUCs */
int			io_min_workers = 2;
int			io_max_workers = 8;
int			io_worker_idle_timeout = 60000;
int			io_worker_launch_interval = 100;


static int	io_worker_queue_size = 64;
static int	MyIoWorkerId = -1;
static PgAioWorkerSubmissionQueue *io_worker_submission_queue;
static PgAioWorkerControl *io_worker_control;


static void
pgaio_workerset_initialize(PgAioWorkerSet *set)
{
	*set = 0;
}

static bool
pgaio_workerset_is_empty(PgAioWorkerSet *set)
{
	return *set == 0;
}

static PgAioWorkerSet
pgaio_workerset_singleton(int worker)
{
	Assert(worker >= 0 && worker < MAX_IO_WORKERS);
	return UINT64_C(1) << worker;
}

static void
pgaio_workerset_all(PgAioWorkerSet *set)
{
	*set = UINT64_MAX >> (PGAIO_WORKERSET_BITS - MAX_IO_WORKERS);
}

static void
pgaio_workerset_subtract(PgAioWorkerSet *set1, const PgAioWorkerSet *set2)
{
	*set1 &= ~*set2;
}

static void
pgaio_workerset_insert(PgAioWorkerSet *set, int worker)
{
	Assert(worker >= 0 && worker < MAX_IO_WORKERS);
	*set |= pgaio_workerset_singleton(worker);
}

static void
pgaio_workerset_remove(PgAioWorkerSet *set, int worker)
{
	Assert(worker >= 0 && worker < MAX_IO_WORKERS);
	*set &= ~pgaio_workerset_singleton(worker);
}

static void
pgaio_workerset_remove_lte(PgAioWorkerSet *set, int worker)
{
	Assert(worker >= 0 && worker < MAX_IO_WORKERS);
	*set &= (~(PgAioWorkerSet) 0) << (worker + 1);
}

static int
pgaio_workerset_get_highest(PgAioWorkerSet *set)
{
	Assert(!pgaio_workerset_is_empty(set));
	return pg_leftmost_one_pos64(*set);
}

static int
pgaio_workerset_get_lowest(PgAioWorkerSet *set)
{
	Assert(!pgaio_workerset_is_empty(set));
	return pg_rightmost_one_pos64(*set);
}

static int
pgaio_workerset_pop_lowest(PgAioWorkerSet *set)
{
	int			worker = pgaio_workerset_get_lowest(set);

	pgaio_workerset_remove(set, worker);
	return worker;
}

#ifdef USE_ASSERT_CHECKING
static bool
pgaio_workerset_contains(PgAioWorkerSet *set, int worker)
{
	Assert(worker >= 0 && worker < MAX_IO_WORKERS);
	return (*set & pgaio_workerset_singleton(worker)) != 0;
}

static int
pgaio_workerset_count(PgAioWorkerSet *set)
{
	return pg_popcount64(*set);
}
#endif

static void
pgaio_worker_shmem_request(void *arg)
{
	size_t		size;
	int			queue_size;

	/* Round size up to next power of two so we can make a mask. */
	queue_size = pg_nextpower2_32(io_worker_queue_size);

	size = offsetof(PgAioWorkerSubmissionQueue, sqes) + sizeof(int) * queue_size;
	ShmemRequestStruct(.name = "AioWorkerSubmissionQueue",
					   .size = size,
					   .ptr = (void **) &io_worker_submission_queue,
		);

	size = offsetof(PgAioWorkerControl, workers) + sizeof(PgAioWorkerSlot) * MAX_IO_WORKERS;
	ShmemRequestStruct(.name = "AioWorkerControl",
					   .size = size,
					   .ptr = (void **) &io_worker_control,
		);
}

static void
pgaio_worker_shmem_init(void *arg)
{
	int			queue_size;

	/* Round size up like in pgaio_worker_shmem_request() */
	queue_size = pg_nextpower2_32(io_worker_queue_size);

	io_worker_submission_queue->size = queue_size;
	io_worker_submission_queue->head = 0;
	io_worker_submission_queue->tail = 0;
	io_worker_control->grow = false;
	pgaio_workerset_initialize(&io_worker_control->workerset);
	pgaio_workerset_initialize(&io_worker_control->idle_workerset);

	for (int i = 0; i < MAX_IO_WORKERS; ++i)
		io_worker_control->workers[i].proc_number = INVALID_PROC_NUMBER;
}

/*
 * Tell postmaster that we think a new worker is needed.
 */
static void
pgaio_worker_request_grow(void)
{
	/*
	 * Suppress useless signaling if we already know that we're at the
	 * maximum.  This uses an unlocked read of nworkers, but that's OK for
	 * this heuristic purpose.
	 */
	if (io_worker_control->nworkers >= io_max_workers)
		return;

	/* Already requested? */
	if (io_worker_control->grow)
		return;

	io_worker_control->grow = true;
	pg_memory_barrier();

	/*
	 * If the postmaster has already been signaled, don't do it again until
	 * the postmaster clears this flag.  There is no point in repeated signals
	 * if grow is being set and cleared repeatedly while the postmaster is
	 * waiting for io_worker_launch_interval, which it applies even to
	 * canceled requests.
	 */
	if (io_worker_control->grow_signal_sent)
		return;

	io_worker_control->grow_signal_sent = true;
	pg_memory_barrier();
	SendPostmasterSignal(PMSIGNAL_IO_WORKER_GROW);
}

/*
 * Cancel any request for a new worker, after observing an empty queue.
 */
static void
pgaio_worker_cancel_grow(void)
{
	if (!io_worker_control->grow)
		return;

	io_worker_control->grow = false;
	pg_memory_barrier();
}

/*
 * Called by the postmaster to check if a new worker has been requested (but
 * possibly canceled since).
 */
bool
pgaio_worker_pm_test_grow_signal_sent(void)
{
	pg_memory_barrier();
	return io_worker_control && io_worker_control->grow_signal_sent;
}

/*
 * Called by the postmaster to check if a new worker has been requested and
 * not canceled since.
 */
bool
pgaio_worker_pm_test_grow(void)
{
	pg_memory_barrier();
	return io_worker_control && io_worker_control->grow;
}

/*
 * Called by the postmaster to clear the request for a new worker.
 */
void
pgaio_worker_pm_clear_grow_signal_sent(void)
{
	if (!io_worker_control)
		return;

	io_worker_control->grow = false;
	io_worker_control->grow_signal_sent = false;
	pg_memory_barrier();
}

static int
pgaio_worker_choose_idle(int only_workers_above)
{
	PgAioWorkerSet workerset;
	int			worker;

	Assert(LWLockHeldByMeInMode(AioWorkerSubmissionQueueLock, LW_EXCLUSIVE));

	workerset = io_worker_control->idle_workerset;
	if (only_workers_above >= 0)
		pgaio_workerset_remove_lte(&workerset, only_workers_above);
	if (pgaio_workerset_is_empty(&workerset))
		return -1;

	/* Find the lowest numbered idle worker and mark it not idle. */
	worker = pgaio_workerset_get_lowest(&workerset);
	pgaio_workerset_remove(&io_worker_control->idle_workerset, worker);

	return worker;
}

/*
 * Try to wake a worker by setting its latch, to tell it there are IOs to
 * process in the submission queue.
 */
static void
pgaio_worker_wake(int worker)
{
	ProcNumber	proc_number;

	/*
	 * If the selected worker is concurrently exiting, then pgaio_worker_die()
	 * had not yet removed it as of when we saw it in idle_workerset.  That's
	 * OK, because it will wake all remaining workers to close wakeup-vs-exit
	 * races: *someone* will see the queued IO.  If there are no workers
	 * running, the postmaster will start a new one.
	 */
	proc_number = io_worker_control->workers[worker].proc_number;
	if (proc_number != INVALID_PROC_NUMBER)
		SetLatch(&GetPGProcByNumber(proc_number)->procLatch);
}

/*
 * Try to wake a set of workers.  Used on pool change, to close races
 * described in the callers.
 */
static void
pgaio_workerset_wake(PgAioWorkerSet workerset)
{
	while (!pgaio_workerset_is_empty(&workerset))
		pgaio_worker_wake(pgaio_workerset_pop_lowest(&workerset));
}

static bool
pgaio_worker_submission_queue_insert(PgAioHandle *ioh)
{
	PgAioWorkerSubmissionQueue *queue;
	uint32		new_head;

	Assert(LWLockHeldByMeInMode(AioWorkerSubmissionQueueLock, LW_EXCLUSIVE));

	queue = io_worker_submission_queue;
	new_head = (queue->head + 1) & (queue->size - 1);
	if (new_head == queue->tail)
	{
		pgaio_debug(DEBUG3, "io queue is full, at %u elements",
					io_worker_submission_queue->size);
		return false;			/* full */
	}

	queue->sqes[queue->head] = pgaio_io_get_id(ioh);
	queue->head = new_head;

	return true;
}

static int
pgaio_worker_submission_queue_consume(void)
{
	PgAioWorkerSubmissionQueue *queue;
	int			result;

	Assert(LWLockHeldByMeInMode(AioWorkerSubmissionQueueLock, LW_EXCLUSIVE));

	queue = io_worker_submission_queue;
	if (queue->tail == queue->head)
		return -1;				/* empty */

	result = queue->sqes[queue->tail];
	queue->tail = (queue->tail + 1) & (queue->size - 1);

	return result;
}

static uint32
pgaio_worker_submission_queue_depth(void)
{
	uint32		head;
	uint32		tail;

	Assert(LWLockHeldByMeInMode(AioWorkerSubmissionQueueLock, LW_EXCLUSIVE));

	head = io_worker_submission_queue->head;
	tail = io_worker_submission_queue->tail;

	if (tail > head)
		head += io_worker_submission_queue->size;

	Assert(head >= tail);

	return head - tail;
}

static bool
pgaio_worker_needs_synchronous_execution(PgAioHandle *ioh)
{
	return
		!IsUnderPostmaster
		|| ioh->flags & PGAIO_HF_REFERENCES_LOCAL
		|| !pgaio_io_can_reopen(ioh);
}

static int
pgaio_worker_submit(uint16 num_staged_ios, PgAioHandle **staged_ios)
{
	PgAioHandle **synchronous_ios = NULL;
	int			nsync = 0;
	int			worker = -1;

	Assert(num_staged_ios <= PGAIO_SUBMIT_BATCH_SIZE);

	for (int i = 0; i < num_staged_ios; i++)
		pgaio_io_prepare_submit(staged_ios[i]);

	if (LWLockConditionalAcquire(AioWorkerSubmissionQueueLock, LW_EXCLUSIVE))
	{
		for (int i = 0; i < num_staged_ios; ++i)
		{
			Assert(!pgaio_worker_needs_synchronous_execution(staged_ios[i]));
			if (!pgaio_worker_submission_queue_insert(staged_ios[i]))
			{
				/*
				 * Do the rest synchronously. If the queue is full, give up
				 * and do the rest synchronously. We're holding an exclusive
				 * lock on the queue so nothing can consume entries.
				 */
				synchronous_ios = &staged_ios[i];
				nsync = (num_staged_ios - i);

				break;
			}
		}
		/* Choose one worker to wake for this batch. */
		worker = pgaio_worker_choose_idle(-1);
		LWLockRelease(AioWorkerSubmissionQueueLock);

		/* Wake up chosen worker.  It will wake peers if necessary. */
		if (worker != -1)
			pgaio_worker_wake(worker);
	}
	else
	{
		/* do everything synchronously, no wakeup needed */
		synchronous_ios = staged_ios;
		nsync = num_staged_ios;
	}

	/* Run whatever is left synchronously. */
	if (nsync > 0)
	{
		for (int i = 0; i < nsync; ++i)
		{
			pgaio_io_perform_synchronously(synchronous_ios[i]);
		}
	}

	return num_staged_ios;
}

/*
 * on_shmem_exit() callback that releases the worker's slot in
 * io_worker_control.
 */
static void
pgaio_worker_die(int code, Datum arg)
{
	PgAioWorkerSet notify_set;

	LWLockAcquire(AioWorkerSubmissionQueueLock, LW_EXCLUSIVE);
	pgaio_workerset_remove(&io_worker_control->idle_workerset, MyIoWorkerId);
	LWLockRelease(AioWorkerSubmissionQueueLock);

	LWLockAcquire(AioWorkerControlLock, LW_EXCLUSIVE);
	Assert(io_worker_control->workers[MyIoWorkerId].proc_number == MyProcNumber);
	io_worker_control->workers[MyIoWorkerId].proc_number = INVALID_PROC_NUMBER;
	Assert(pgaio_workerset_contains(&io_worker_control->workerset, MyIoWorkerId));
	pgaio_workerset_remove(&io_worker_control->workerset, MyIoWorkerId);
	notify_set = io_worker_control->workerset;
	Assert(io_worker_control->nworkers > 0);
	io_worker_control->nworkers--;
	Assert(pgaio_workerset_count(&io_worker_control->workerset) ==
		   io_worker_control->nworkers);
	LWLockRelease(AioWorkerControlLock);

	/*
	 * Notify other workers on pool change.  This allows the new highest
	 * worker to know that it is now the one that can time out, and closes a
	 * wakeup-loss race described in pgaio_worker_wake().
	 */
	pgaio_workerset_wake(notify_set);
}

/*
 * Register the worker in shared memory, assign MyIoWorkerId and register a
 * shutdown callback to release registration.
 */
static void
pgaio_worker_register(void)
{
	PgAioWorkerSet free_workerset;
	PgAioWorkerSet old_workerset;

	MyIoWorkerId = -1;

	LWLockAcquire(AioWorkerControlLock, LW_EXCLUSIVE);
	/* Find lowest unused worker ID. */
	pgaio_workerset_all(&free_workerset);
	pgaio_workerset_subtract(&free_workerset, &io_worker_control->workerset);
	if (!pgaio_workerset_is_empty(&free_workerset))
		MyIoWorkerId = pgaio_workerset_get_lowest(&free_workerset);
	if (MyIoWorkerId == -1)
		elog(ERROR, "couldn't find a free worker ID");

	Assert(io_worker_control->workers[MyIoWorkerId].proc_number ==
		   INVALID_PROC_NUMBER);
	io_worker_control->workers[MyIoWorkerId].proc_number = MyProcNumber;

	old_workerset = io_worker_control->workerset;
	Assert(!pgaio_workerset_contains(&old_workerset, MyIoWorkerId));
	pgaio_workerset_insert(&io_worker_control->workerset, MyIoWorkerId);
	io_worker_control->nworkers++;
	Assert(io_worker_control->nworkers <= MAX_IO_WORKERS);
	Assert(pgaio_workerset_count(&io_worker_control->workerset) ==
		   io_worker_control->nworkers);
	LWLockRelease(AioWorkerControlLock);

	/*
	 * Notify other workers on pool change.  If we were the highest worker,
	 * this allows the new highest worker to know that it can time out.
	 */
	pgaio_workerset_wake(old_workerset);

	on_shmem_exit(pgaio_worker_die, 0);
}

static void
pgaio_worker_error_callback(void *arg)
{
	ProcNumber	owner;
	PGPROC	   *owner_proc;
	int32		owner_pid;
	PgAioHandle *ioh = arg;

	if (!ioh)
		return;

	Assert(ioh->owner_procno != MyProcNumber);
	Assert(MyBackendType == B_IO_WORKER);

	owner = ioh->owner_procno;
	owner_proc = GetPGProcByNumber(owner);
	owner_pid = owner_proc->pid;

	errcontext("I/O worker executing I/O on behalf of process %d", owner_pid);
}

/*
 * Check if this backend is allowed to time out, and thus should use a
 * non-infinite sleep time.  Only the highest-numbered worker is allowed to
 * time out, and only if the pool is above io_min_workers.  Serializing
 * timeouts keeps IDs in a range 0..N without gaps, and avoids undershooting
 * io_min_workers.
 *
 * The result is only instantaneously true and may be temporarily inconsistent
 * in different workers around transitions, but all workers are woken up on
 * pool size or GUC changes making the result eventually consistent.
 */
static bool
pgaio_worker_can_timeout(void)
{
	PgAioWorkerSet workerset;

	if (MyIoWorkerId < io_min_workers)
		return false;

	/* Serialize against pool size changes. */
	LWLockAcquire(AioWorkerControlLock, LW_SHARED);
	workerset = io_worker_control->workerset;
	LWLockRelease(AioWorkerControlLock);

	if (MyIoWorkerId != pgaio_workerset_get_highest(&workerset))
		return false;

	return true;
}

void
IoWorkerMain(const void *startup_data, size_t startup_data_len)
{
	sigjmp_buf	local_sigjmp_buf;
	TimestampTz idle_timeout_abs = 0;
	int			timeout_guc_used = 0;
	PgAioHandle *volatile error_ioh = NULL;
	ErrorContextCallback errcallback = {0};
	volatile int error_errno = 0;
	char		cmd[128];
	int			hist_ios = 0;
	int			hist_wakeups = 0;

	AuxiliaryProcessMainCommon();

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGINT, die);		/* to allow manually triggering worker restart */

	/*
	 * Ignore SIGTERM, will get explicit shutdown via SIGUSR2 later in the
	 * shutdown sequence, similar to checkpointer.
	 */
	pqsignal(SIGTERM, PG_SIG_IGN);
	/* SIGQUIT handler was already set up by InitPostmasterChild */
	pqsignal(SIGALRM, PG_SIG_IGN);
	pqsignal(SIGPIPE, PG_SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, SignalHandlerForShutdownRequest);

	/* also registers a shutdown callback to unregister */
	pgaio_worker_register();

	sprintf(cmd, "%d", MyIoWorkerId);
	set_ps_display(cmd);

	errcallback.callback = pgaio_worker_error_callback;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* see PostgresMain() */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		error_context_stack = NULL;
		HOLD_INTERRUPTS();

		EmitErrorReport();

		/*
		 * In the - very unlikely - case that the IO failed in a way that
		 * raises an error we need to mark the IO as failed.
		 *
		 * Need to do just enough error recovery so that we can mark the IO as
		 * failed and then exit (postmaster will start a new worker).
		 */
		LWLockReleaseAll();

		if (error_ioh != NULL)
		{
			/* should never fail without setting error_errno */
			Assert(error_errno != 0);

			errno = error_errno;

			START_CRIT_SECTION();
			pgaio_io_process_completion(error_ioh, -error_errno);
			END_CRIT_SECTION();
		}

		proc_exit(1);
	}

	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	while (!ShutdownRequestPending)
	{
		uint32		io_index;
		int			worker = -1;
		int			queue_depth = 0;
		bool		maybe_grow = false;

		/*
		 * Try to get a job to do.
		 *
		 * The lwlock acquisition also provides the necessary memory barrier
		 * to ensure that we don't see an outdated data in the handle.
		 */
		LWLockAcquire(AioWorkerSubmissionQueueLock, LW_EXCLUSIVE);
		if ((io_index = pgaio_worker_submission_queue_consume()) == -1)
		{
			/* Nothing to do.  Mark self idle. */
			pgaio_workerset_insert(&io_worker_control->idle_workerset,
								   MyIoWorkerId);
		}
		else
		{
			/* Got one.  Clear idle flag. */
			pgaio_workerset_remove(&io_worker_control->idle_workerset,
								   MyIoWorkerId);

			/*
			 * See if we should wake up a higher numbered peer.  Only do that
			 * if this worker is not receiving spurious wakeups itself.  The
			 * intention is create a frontier beyond which idle workers stay
			 * asleep.
			 *
			 * This heuristic tries to discover the useful wakeup propagation
			 * chain length when IOs are very fast and workers wake up to find
			 * that all IOs have already been taken.
			 *
			 * If we chose not to wake a worker when we ideally should have,
			 * then the ratio will soon change to correct that.
			 */
			if (hist_wakeups <= hist_ios)
			{
				queue_depth = pgaio_worker_submission_queue_depth();
				if (queue_depth > 0)
				{
					/* Choose a worker higher than me to wake. */
					worker = pgaio_worker_choose_idle(MyIoWorkerId);
					if (worker == -1)
						maybe_grow = true;
				}
			}
		}
		LWLockRelease(AioWorkerSubmissionQueueLock);

		/* Propagate wakeups. */
		if (worker != -1)
		{
			pgaio_worker_wake(worker);
		}
		else if (maybe_grow)
		{
			/*
			 * We know there was at least one more item in the queue, and we
			 * failed to find a higher-numbered idle worker to wake.  Now we
			 * decide if we should try to start one more worker.
			 *
			 * We do this with a simple heuristic: is the queue depth greater
			 * than the current number of workers?
			 *
			 * Consider the following situations:
			 *
			 * 1. The queue depth is constantly increasing, because IOs are
			 * arriving faster than they can possibly be serviced.  It doesn't
			 * matter much which threshold we choose, as we will surely hit
			 * it.  Crossing the current worker count is a useful signal
			 * because it's clearly too deep to avoid queuing latency already,
			 * but still leaves a small window of opportunity to improve the
			 * situation before the queue overflows.
			 *
			 * 2. The worker pool is keeping up, no latency is being
			 * introduced and an extra worker would be a waste of resources.
			 * Queue depth distributions tend to be heavily skewed, with long
			 * tails of low probability spikes (due to submission clustering,
			 * scheduling, jitter, stalls, noisy neighbors, etc).  We want a
			 * number that is very unlikely to be triggered by an outlier, and
			 * we bet that an exponential or similar distribution whose
			 * outliers never reach this threshold must be almost entirely
			 * concentrated at the low end.  If we do see a spike as big as
			 * the worker count, we take it as a signal that the distribution
			 * is surely too wide.
			 *
			 * On its own, this is an extremely crude signal.  When combined
			 * with the wakeup propagation test that precedes it (but on its
			 * own tends to overshoot) and io_worker_launch_interval, the
			 * result is that we gradually test each pool size until we find
			 * one that doesn't trigger further expansion, and then hold it
			 * for at least io_worker_idle_timeout.
			 *
			 * XXX Perhaps ideas from queueing theory or control theory could
			 * do a better job of this.
			 */

			/* Read nworkers without lock for this heuristic purpose. */
			if (queue_depth > io_worker_control->nworkers)
				pgaio_worker_request_grow();
		}

		if (io_index != -1)
		{
			PgAioHandle *ioh = NULL;

			/* Cancel timeout and update wakeup:work ratio. */
			idle_timeout_abs = 0;
			if (++hist_ios == PGAIO_WORKER_WAKEUP_RATIO_SATURATE)
			{
				hist_wakeups /= 2;
				hist_ios /= 2;
			}

			ioh = &pgaio_ctl->io_handles[io_index];
			error_ioh = ioh;
			errcallback.arg = ioh;

			pgaio_debug_io(DEBUG4, ioh,
						   "worker %d processing IO",
						   MyIoWorkerId);

			/*
			 * Prevent interrupts between pgaio_io_reopen() and
			 * pgaio_io_perform_synchronously() that otherwise could lead to
			 * the FD getting closed in that window.
			 */
			HOLD_INTERRUPTS();

			/*
			 * It's very unlikely, but possible, that reopen fails. E.g. due
			 * to memory allocations failing or file permissions changing or
			 * such.  In that case we need to fail the IO.
			 *
			 * There's not really a good errno we can report here.
			 */
			error_errno = ENOENT;
			pgaio_io_reopen(ioh);

			/*
			 * To be able to exercise the reopen-fails path, allow injection
			 * points to trigger a failure at this point.
			 */
			INJECTION_POINT("aio-worker-after-reopen", ioh);

			error_errno = 0;
			error_ioh = NULL;

			/*
			 * As part of IO completion the buffer will be marked as NOACCESS,
			 * until the buffer is pinned again - which never happens in io
			 * workers. Therefore the next time there is IO for the same
			 * buffer, the memory will be considered inaccessible. To avoid
			 * that, explicitly allow access to the memory before reading data
			 * into it.
			 */
#ifdef USE_VALGRIND
			{
				struct iovec *iov;
				uint16		iov_length = pgaio_io_get_iovec_length(ioh, &iov);

				for (int i = 0; i < iov_length; i++)
					VALGRIND_MAKE_MEM_UNDEFINED(iov[i].iov_base, iov[i].iov_len);
			}
#endif

#ifdef PGAIO_WORKER_SHOW_PS_INFO
			{
				char	   *description = pgaio_io_get_target_description(ioh);

				sprintf(cmd, "%d: [%s] %s",
						MyIoWorkerId,
						pgaio_io_get_op_name(ioh),
						description);
				pfree(description);
				set_ps_display(cmd);
			}
#endif

			/*
			 * We don't expect this to ever fail with ERROR or FATAL, no need
			 * to keep error_ioh set to the IO.
			 * pgaio_io_perform_synchronously() contains a critical section to
			 * ensure we don't accidentally fail.
			 */
			pgaio_io_perform_synchronously(ioh);

			RESUME_INTERRUPTS();
			errcallback.arg = NULL;
		}
		else
		{
			int			timeout_ms;

			/* Cancel new worker request if pending. */
			pgaio_worker_cancel_grow();

			/* Compute the remaining allowed idle time. */
			if (io_worker_idle_timeout == -1)
			{
				/* Never time out. */
				timeout_ms = -1;
			}
			else
			{
				TimestampTz now = GetCurrentTimestamp();

				/* If the GUC changes, reset timer. */
				if (idle_timeout_abs != 0 &&
					io_worker_idle_timeout != timeout_guc_used)
					idle_timeout_abs = 0;

				/* Only the highest-numbered worker can time out. */
				if (pgaio_worker_can_timeout())
				{
					if (idle_timeout_abs == 0)
					{
						/*
						 * I have just been promoted to the timeout worker, or
						 * the GUC changed.  Compute new absolute time from
						 * now.
						 */
						idle_timeout_abs =
							TimestampTzPlusMilliseconds(now,
														io_worker_idle_timeout);
						timeout_guc_used = io_worker_idle_timeout;
					}
					timeout_ms =
						TimestampDifferenceMilliseconds(now, idle_timeout_abs);
				}
				else
				{
					/* No timeout for me. */
					idle_timeout_abs = 0;
					timeout_ms = -1;
				}
			}

#ifdef PGAIO_WORKER_SHOW_PS_INFO
			sprintf(cmd, "%d: idle, wakeups:ios = %d:%d",
					MyIoWorkerId, hist_wakeups, hist_ios);
			set_ps_display(cmd);
#endif

			if (WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH | WL_TIMEOUT,
						  timeout_ms,
						  WAIT_EVENT_IO_WORKER_MAIN) == WL_TIMEOUT)
			{
				/* WL_TIMEOUT */
				if (pgaio_worker_can_timeout())
					if (GetCurrentTimestamp() >= idle_timeout_abs)
						break;
			}
			else
			{
				/* WL_LATCH_SET */
				if (++hist_wakeups == PGAIO_WORKER_WAKEUP_RATIO_SATURATE)
				{
					hist_wakeups /= 2;
					hist_ios /= 2;
				}
			}
			ResetLatch(MyLatch);
		}

		CHECK_FOR_INTERRUPTS();

		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);

			/* If io_max_workers has been decreased, exit highest first. */
			if (MyIoWorkerId >= io_max_workers)
				break;
		}
	}

	error_context_stack = errcallback.previous;
	proc_exit(0);
}

bool
pgaio_workers_enabled(void)
{
	return io_method == IOMETHOD_WORKER;
}
