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
 * So that the submitter can make just one system call when submitting a batch
 * of IOs, wakeups "fan out"; each woken IO worker can wake two more. XXX This
 * could be improved by using futexes instead of latches to wake N waiters.
 *
 * This method of AIO is available in all builds on all operating systems, and
 * is the default.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/aio/method_worker.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

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
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/injection_point.h"
#include "utils/memdebug.h"
#include "utils/ps_status.h"
#include "utils/wait_event.h"


/* How many workers should each worker wake up if needed? */
#define IO_WORKER_WAKEUP_FANOUT 2


typedef struct PgAioWorkerSubmissionQueue
{
	uint32		size;
	uint32		mask;
	uint32		head;
	uint32		tail;
	uint32		sqes[FLEXIBLE_ARRAY_MEMBER];
} PgAioWorkerSubmissionQueue;

typedef struct PgAioWorkerSlot
{
	Latch	   *latch;
	bool		in_use;
} PgAioWorkerSlot;

typedef struct PgAioWorkerControl
{
	uint64		idle_worker_mask;
	PgAioWorkerSlot workers[FLEXIBLE_ARRAY_MEMBER];
} PgAioWorkerControl;


static size_t pgaio_worker_shmem_size(void);
static void pgaio_worker_shmem_init(bool first_time);

static bool pgaio_worker_needs_synchronous_execution(PgAioHandle *ioh);
static int	pgaio_worker_submit(uint16 num_staged_ios, PgAioHandle **staged_ios);


const IoMethodOps pgaio_worker_ops = {
	.shmem_size = pgaio_worker_shmem_size,
	.shmem_init = pgaio_worker_shmem_init,

	.needs_synchronous_execution = pgaio_worker_needs_synchronous_execution,
	.submit = pgaio_worker_submit,
};


/* GUCs */
int			io_workers = 3;


static int	io_worker_queue_size = 64;
static int	MyIoWorkerId;
static PgAioWorkerSubmissionQueue *io_worker_submission_queue;
static PgAioWorkerControl *io_worker_control;


static size_t
pgaio_worker_queue_shmem_size(int *queue_size)
{
	/* Round size up to next power of two so we can make a mask. */
	*queue_size = pg_nextpower2_32(io_worker_queue_size);

	return offsetof(PgAioWorkerSubmissionQueue, sqes) +
		sizeof(uint32) * *queue_size;
}

static size_t
pgaio_worker_control_shmem_size(void)
{
	return offsetof(PgAioWorkerControl, workers) +
		sizeof(PgAioWorkerSlot) * MAX_IO_WORKERS;
}

static size_t
pgaio_worker_shmem_size(void)
{
	size_t		sz;
	int			queue_size;

	sz = pgaio_worker_queue_shmem_size(&queue_size);
	sz = add_size(sz, pgaio_worker_control_shmem_size());

	return sz;
}

static void
pgaio_worker_shmem_init(bool first_time)
{
	bool		found;
	int			queue_size;

	io_worker_submission_queue =
		ShmemInitStruct("AioWorkerSubmissionQueue",
						pgaio_worker_queue_shmem_size(&queue_size),
						&found);
	if (!found)
	{
		io_worker_submission_queue->size = queue_size;
		io_worker_submission_queue->head = 0;
		io_worker_submission_queue->tail = 0;
	}

	io_worker_control =
		ShmemInitStruct("AioWorkerControl",
						pgaio_worker_control_shmem_size(),
						&found);
	if (!found)
	{
		io_worker_control->idle_worker_mask = 0;
		for (int i = 0; i < MAX_IO_WORKERS; ++i)
		{
			io_worker_control->workers[i].latch = NULL;
			io_worker_control->workers[i].in_use = false;
		}
	}
}

static int
pgaio_worker_choose_idle(void)
{
	int			worker;

	if (io_worker_control->idle_worker_mask == 0)
		return -1;

	/* Find the lowest bit position, and clear it. */
	worker = pg_rightmost_one_pos64(io_worker_control->idle_worker_mask);
	io_worker_control->idle_worker_mask &= ~(UINT64_C(1) << worker);
	Assert(io_worker_control->workers[worker].in_use);

	return worker;
}

static bool
pgaio_worker_submission_queue_insert(PgAioHandle *ioh)
{
	PgAioWorkerSubmissionQueue *queue;
	uint32		new_head;

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

static uint32
pgaio_worker_submission_queue_consume(void)
{
	PgAioWorkerSubmissionQueue *queue;
	uint32		result;

	queue = io_worker_submission_queue;
	if (queue->tail == queue->head)
		return UINT32_MAX;		/* empty */

	result = queue->sqes[queue->tail];
	queue->tail = (queue->tail + 1) & (queue->size - 1);

	return result;
}

static uint32
pgaio_worker_submission_queue_depth(void)
{
	uint32		head;
	uint32		tail;

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

static void
pgaio_worker_submit_internal(int num_staged_ios, PgAioHandle **staged_ios)
{
	PgAioHandle *synchronous_ios[PGAIO_SUBMIT_BATCH_SIZE];
	int			nsync = 0;
	Latch	   *wakeup = NULL;
	int			worker;

	Assert(num_staged_ios <= PGAIO_SUBMIT_BATCH_SIZE);

	LWLockAcquire(AioWorkerSubmissionQueueLock, LW_EXCLUSIVE);
	for (int i = 0; i < num_staged_ios; ++i)
	{
		Assert(!pgaio_worker_needs_synchronous_execution(staged_ios[i]));
		if (!pgaio_worker_submission_queue_insert(staged_ios[i]))
		{
			/*
			 * We'll do it synchronously, but only after we've sent as many as
			 * we can to workers, to maximize concurrency.
			 */
			synchronous_ios[nsync++] = staged_ios[i];
			continue;
		}

		if (wakeup == NULL)
		{
			/* Choose an idle worker to wake up if we haven't already. */
			worker = pgaio_worker_choose_idle();
			if (worker >= 0)
				wakeup = io_worker_control->workers[worker].latch;

			pgaio_debug_io(DEBUG4, staged_ios[i],
						   "choosing worker %d",
						   worker);
		}
	}
	LWLockRelease(AioWorkerSubmissionQueueLock);

	if (wakeup)
		SetLatch(wakeup);

	/* Run whatever is left synchronously. */
	if (nsync > 0)
	{
		for (int i = 0; i < nsync; ++i)
		{
			pgaio_io_perform_synchronously(synchronous_ios[i]);
		}
	}
}

static int
pgaio_worker_submit(uint16 num_staged_ios, PgAioHandle **staged_ios)
{
	for (int i = 0; i < num_staged_ios; i++)
	{
		PgAioHandle *ioh = staged_ios[i];

		pgaio_io_prepare_submit(ioh);
	}

	pgaio_worker_submit_internal(num_staged_ios, staged_ios);

	return num_staged_ios;
}

/*
 * on_shmem_exit() callback that releases the worker's slot in
 * io_worker_control.
 */
static void
pgaio_worker_die(int code, Datum arg)
{
	LWLockAcquire(AioWorkerSubmissionQueueLock, LW_EXCLUSIVE);
	Assert(io_worker_control->workers[MyIoWorkerId].in_use);
	Assert(io_worker_control->workers[MyIoWorkerId].latch == MyLatch);

	io_worker_control->idle_worker_mask &= ~(UINT64_C(1) << MyIoWorkerId);
	io_worker_control->workers[MyIoWorkerId].in_use = false;
	io_worker_control->workers[MyIoWorkerId].latch = NULL;
	LWLockRelease(AioWorkerSubmissionQueueLock);
}

/*
 * Register the worker in shared memory, assign MyIoWorkerId and register a
 * shutdown callback to release registration.
 */
static void
pgaio_worker_register(void)
{
	MyIoWorkerId = -1;

	/*
	 * XXX: This could do with more fine-grained locking. But it's also not
	 * very common for the number of workers to change at the moment...
	 */
	LWLockAcquire(AioWorkerSubmissionQueueLock, LW_EXCLUSIVE);

	for (int i = 0; i < MAX_IO_WORKERS; ++i)
	{
		if (!io_worker_control->workers[i].in_use)
		{
			Assert(io_worker_control->workers[i].latch == NULL);
			io_worker_control->workers[i].in_use = true;
			MyIoWorkerId = i;
			break;
		}
		else
			Assert(io_worker_control->workers[i].latch != NULL);
	}

	if (MyIoWorkerId == -1)
		elog(ERROR, "couldn't find a free worker slot");

	io_worker_control->idle_worker_mask |= (UINT64_C(1) << MyIoWorkerId);
	io_worker_control->workers[MyIoWorkerId].latch = MyLatch;
	LWLockRelease(AioWorkerSubmissionQueueLock);

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

void
IoWorkerMain(const void *startup_data, size_t startup_data_len)
{
	sigjmp_buf	local_sigjmp_buf;
	PgAioHandle *volatile error_ioh = NULL;
	ErrorContextCallback errcallback = {0};
	volatile int error_errno = 0;
	char		cmd[128];

	MyBackendType = B_IO_WORKER;
	AuxiliaryProcessMainCommon();

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGINT, die);		/* to allow manually triggering worker restart */

	/*
	 * Ignore SIGTERM, will get explicit shutdown via SIGUSR2 later in the
	 * shutdown sequence, similar to checkpointer.
	 */
	pqsignal(SIGTERM, SIG_IGN);
	/* SIGQUIT handler was already set up by InitPostmasterChild */
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
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
		Latch	   *latches[IO_WORKER_WAKEUP_FANOUT];
		int			nlatches = 0;
		int			nwakeups = 0;
		int			worker;

		/*
		 * Try to get a job to do.
		 *
		 * The lwlock acquisition also provides the necessary memory barrier
		 * to ensure that we don't see an outdated data in the handle.
		 */
		LWLockAcquire(AioWorkerSubmissionQueueLock, LW_EXCLUSIVE);
		if ((io_index = pgaio_worker_submission_queue_consume()) == UINT32_MAX)
		{
			/*
			 * Nothing to do.  Mark self idle.
			 *
			 * XXX: Invent some kind of back pressure to reduce useless
			 * wakeups?
			 */
			io_worker_control->idle_worker_mask |= (UINT64_C(1) << MyIoWorkerId);
		}
		else
		{
			/* Got one.  Clear idle flag. */
			io_worker_control->idle_worker_mask &= ~(UINT64_C(1) << MyIoWorkerId);

			/* See if we can wake up some peers. */
			nwakeups = Min(pgaio_worker_submission_queue_depth(),
						   IO_WORKER_WAKEUP_FANOUT);
			for (int i = 0; i < nwakeups; ++i)
			{
				if ((worker = pgaio_worker_choose_idle()) < 0)
					break;
				latches[nlatches++] = io_worker_control->workers[worker].latch;
			}
		}
		LWLockRelease(AioWorkerSubmissionQueueLock);

		for (int i = 0; i < nlatches; ++i)
			SetLatch(latches[i]);

		if (io_index != UINT32_MAX)
		{
			PgAioHandle *ioh = NULL;

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
			WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, -1,
					  WAIT_EVENT_IO_WORKER_MAIN);
			ResetLatch(MyLatch);
		}

		CHECK_FOR_INTERRUPTS();

		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
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
