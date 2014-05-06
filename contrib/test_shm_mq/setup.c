/*--------------------------------------------------------------------------
 *
 * setup.c
 *		Code to set up a dynamic shared memory segments and a specified
 *		number of background workers for shared memory message queue
 *		testing.
 *
 * Copyright (C) 2013, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/test_shm_mq/setup.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/procsignal.h"
#include "storage/shm_toc.h"
#include "utils/memutils.h"

#include "test_shm_mq.h"

typedef struct
{
	int			nworkers;
	BackgroundWorkerHandle *handle[FLEXIBLE_ARRAY_MEMBER];
} worker_state;

static void setup_dynamic_shared_memory(int64 queue_size, int nworkers,
							dsm_segment **segp,
							test_shm_mq_header **hdrp,
							shm_mq **outp, shm_mq **inp);
static worker_state *setup_background_workers(int nworkers,
						 dsm_segment *seg);
static void cleanup_background_workers(dsm_segment *seg, Datum arg);
static void wait_for_workers_to_become_ready(worker_state *wstate,
								 volatile test_shm_mq_header *hdr);
static bool check_worker_status(worker_state *wstate);

/*
 * Set up a dynamic shared memory segment and zero or more background workers
 * for a test run.
 */
void
test_shm_mq_setup(int64 queue_size, int32 nworkers, dsm_segment **segp,
				  shm_mq_handle **output, shm_mq_handle **input)
{
	dsm_segment *seg;
	test_shm_mq_header *hdr;
	shm_mq	   *outq = NULL;	/* placate compiler */
	shm_mq	   *inq = NULL;		/* placate compiler */
	worker_state *wstate;

	/* Set up a dynamic shared memory segment. */
	setup_dynamic_shared_memory(queue_size, nworkers, &seg, &hdr, &outq, &inq);
	*segp = seg;

	/* Register background workers. */
	wstate = setup_background_workers(nworkers, seg);

	/* Attach the queues. */
	*output = shm_mq_attach(outq, seg, wstate->handle[0]);
	*input = shm_mq_attach(inq, seg, wstate->handle[nworkers - 1]);

	/* Wait for workers to become ready. */
	wait_for_workers_to_become_ready(wstate, hdr);

	/*
	 * Once we reach this point, all workers are ready.  We no longer need to
	 * kill them if we die; they'll die on their own as the message queues
	 * shut down.
	 */
	cancel_on_dsm_detach(seg, cleanup_background_workers,
						 PointerGetDatum(wstate));
	pfree(wstate);
}

/*
 * Set up a dynamic shared memory segment.
 *
 * We set up a small control region that contains only a test_shm_mq_header,
 * plus one region per message queue.  There are as many message queues as
 * the number of workers, plus one.
 */
static void
setup_dynamic_shared_memory(int64 queue_size, int nworkers,
							dsm_segment **segp, test_shm_mq_header **hdrp,
							shm_mq **outp, shm_mq **inp)
{
	shm_toc_estimator e;
	int			i;
	Size		segsize;
	dsm_segment *seg;
	shm_toc    *toc;
	test_shm_mq_header *hdr;

	/* Ensure a valid queue size. */
	if (queue_size < 0 || ((uint64) queue_size) < shm_mq_minimum_size)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("queue size must be at least %zu bytes",
						shm_mq_minimum_size)));
	if (queue_size != ((Size) queue_size))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("queue size overflows size_t")));

	/*
	 * Estimate how much shared memory we need.
	 *
	 * Because the TOC machinery may choose to insert padding of oddly-sized
	 * requests, we must estimate each chunk separately.
	 *
	 * We need one key to register the location of the header, and we need
	 * nworkers + 1 keys to track the locations of the message queues.
	 */
	shm_toc_initialize_estimator(&e);
	shm_toc_estimate_chunk(&e, sizeof(test_shm_mq_header));
	for (i = 0; i <= nworkers; ++i)
		shm_toc_estimate_chunk(&e, (Size) queue_size);
	shm_toc_estimate_keys(&e, 2 + nworkers);
	segsize = shm_toc_estimate(&e);

	/* Create the shared memory segment and establish a table of contents. */
	seg = dsm_create(shm_toc_estimate(&e));
	toc = shm_toc_create(PG_TEST_SHM_MQ_MAGIC, dsm_segment_address(seg),
						 segsize);

	/* Set up the header region. */
	hdr = shm_toc_allocate(toc, sizeof(test_shm_mq_header));
	SpinLockInit(&hdr->mutex);
	hdr->workers_total = nworkers;
	hdr->workers_attached = 0;
	hdr->workers_ready = 0;
	shm_toc_insert(toc, 0, hdr);

	/* Set up one message queue per worker, plus one. */
	for (i = 0; i <= nworkers; ++i)
	{
		shm_mq	   *mq;

		mq = shm_mq_create(shm_toc_allocate(toc, (Size) queue_size),
						   (Size) queue_size);
		shm_toc_insert(toc, i + 1, mq);

		if (i == 0)
		{
			/* We send messages to the first queue. */
			shm_mq_set_sender(mq, MyProc);
			*outp = mq;
		}
		if (i == nworkers)
		{
			/* We receive messages from the last queue. */
			shm_mq_set_receiver(mq, MyProc);
			*inp = mq;
		}
	}

	/* Return results to caller. */
	*segp = seg;
	*hdrp = hdr;
}

/*
 * Register background workers.
 */
static worker_state *
setup_background_workers(int nworkers, dsm_segment *seg)
{
	MemoryContext oldcontext;
	BackgroundWorker worker;
	worker_state *wstate;
	int			i;

	/*
	 * We need the worker_state object and the background worker handles to
	 * which it points to be allocated in CurTransactionContext rather than
	 * ExprContext; otherwise, they'll be destroyed before the on_dsm_detach
	 * hooks run.
	 */
	oldcontext = MemoryContextSwitchTo(CurTransactionContext);

	/* Create worker state object. */
	wstate = MemoryContextAlloc(TopTransactionContext,
								offsetof(worker_state, handle) +
								sizeof(BackgroundWorkerHandle *) * nworkers);
	wstate->nworkers = 0;

	/*
	 * Arrange to kill all the workers if we abort before all workers are
	 * finished hooking themselves up to the dynamic shared memory segment.
	 *
	 * If we die after all the workers have finished hooking themselves up to
	 * the dynamic shared memory segment, we'll mark the two queues to which
	 * we're directly connected as detached, and the worker(s) connected to
	 * those queues will exit, marking any other queues to which they are
	 * connected as detached.  This will cause any as-yet-unaware workers
	 * connected to those queues to exit in their turn, and so on, until
	 * everybody exits.
	 *
	 * But suppose the workers which are supposed to connect to the queues to
	 * which we're directly attached exit due to some error before they
	 * actually attach the queues.  The remaining workers will have no way of
	 * knowing this.  From their perspective, they're still waiting for those
	 * workers to start, when in fact they've already died.
	 */
	on_dsm_detach(seg, cleanup_background_workers,
				  PointerGetDatum(wstate));

	/* Configure a worker. */
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	worker.bgw_main = NULL;		/* new worker might not have library loaded */
	sprintf(worker.bgw_library_name, "test_shm_mq");
	sprintf(worker.bgw_function_name, "test_shm_mq_main");
	snprintf(worker.bgw_name, BGW_MAXLEN, "test_shm_mq");
	worker.bgw_main_arg = UInt32GetDatum(dsm_segment_handle(seg));
	/* set bgw_notify_pid, so we can detect if the worker stops */
	worker.bgw_notify_pid = MyProcPid;

	/* Register the workers. */
	for (i = 0; i < nworkers; ++i)
	{
		if (!RegisterDynamicBackgroundWorker(&worker, &wstate->handle[i]))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
					 errmsg("could not register background process"),
				 errhint("You may need to increase max_worker_processes.")));
		++wstate->nworkers;
	}

	/* All done. */
	MemoryContextSwitchTo(oldcontext);
	return wstate;
}

static void
cleanup_background_workers(dsm_segment *seg, Datum arg)
{
	worker_state *wstate = (worker_state *) DatumGetPointer(arg);

	while (wstate->nworkers > 0)
	{
		--wstate->nworkers;
		TerminateBackgroundWorker(wstate->handle[wstate->nworkers]);
	}
}

static void
wait_for_workers_to_become_ready(worker_state *wstate,
								 volatile test_shm_mq_header *hdr)
{
	bool		save_set_latch_on_sigusr1;
	bool		result = false;

	save_set_latch_on_sigusr1 = set_latch_on_sigusr1;
	set_latch_on_sigusr1 = true;

	PG_TRY();
	{
		for (;;)
		{
			int			workers_ready;

			/* If all the workers are ready, we have succeeded. */
			SpinLockAcquire(&hdr->mutex);
			workers_ready = hdr->workers_ready;
			SpinLockRelease(&hdr->mutex);
			if (workers_ready >= wstate->nworkers)
			{
				result = true;
				break;
			}

			/* If any workers (or the postmaster) have died, we have failed. */
			if (!check_worker_status(wstate))
			{
				result = false;
				break;
			}

			/* Wait to be signalled. */
			WaitLatch(&MyProc->procLatch, WL_LATCH_SET, 0);

			/* An interrupt may have occurred while we were waiting. */
			CHECK_FOR_INTERRUPTS();

			/* Reset the latch so we don't spin. */
			ResetLatch(&MyProc->procLatch);
		}
	}
	PG_CATCH();
	{
		set_latch_on_sigusr1 = save_set_latch_on_sigusr1;
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (!result)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("one or more background workers failed to start")));
}

static bool
check_worker_status(worker_state *wstate)
{
	int			n;

	/* If any workers (or the postmaster) have died, we have failed. */
	for (n = 0; n < wstate->nworkers; ++n)
	{
		BgwHandleStatus status;
		pid_t		pid;

		status = GetBackgroundWorkerPid(wstate->handle[n], &pid);
		if (status == BGWH_STOPPED || status == BGWH_POSTMASTER_DIED)
			return false;
	}

	/* Otherwise, things still look OK. */
	return true;
}
