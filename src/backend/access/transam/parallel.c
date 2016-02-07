/*-------------------------------------------------------------------------
 *
 * parallel.c
 *	  Infrastructure for launching parallel workers
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/transam/parallel.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xact.h"
#include "access/xlog.h"
#include "access/parallel.h"
#include "commands/async.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "libpq/pqmq.h"
#include "miscadmin.h"
#include "optimizer/planmain.h"
#include "storage/ipc.h"
#include "storage/sinval.h"
#include "storage/spin.h"
#include "tcop/tcopprot.h"
#include "utils/combocid.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"

/*
 * We don't want to waste a lot of memory on an error queue which, most of
 * the time, will process only a handful of small messages.  However, it is
 * desirable to make it large enough that a typical ErrorResponse can be sent
 * without blocking.  That way, a worker that errors out can write the whole
 * message into the queue and terminate without waiting for the user backend.
 */
#define PARALLEL_ERROR_QUEUE_SIZE			16384

/* Magic number for parallel context TOC. */
#define PARALLEL_MAGIC						0x50477c7c

/*
 * Magic numbers for parallel state sharing.  Higher-level code should use
 * smaller values, leaving these very large ones for use by this module.
 */
#define PARALLEL_KEY_FIXED					UINT64CONST(0xFFFFFFFFFFFF0001)
#define PARALLEL_KEY_ERROR_QUEUE			UINT64CONST(0xFFFFFFFFFFFF0002)
#define PARALLEL_KEY_LIBRARY				UINT64CONST(0xFFFFFFFFFFFF0003)
#define PARALLEL_KEY_GUC					UINT64CONST(0xFFFFFFFFFFFF0004)
#define PARALLEL_KEY_COMBO_CID				UINT64CONST(0xFFFFFFFFFFFF0005)
#define PARALLEL_KEY_TRANSACTION_SNAPSHOT	UINT64CONST(0xFFFFFFFFFFFF0006)
#define PARALLEL_KEY_ACTIVE_SNAPSHOT		UINT64CONST(0xFFFFFFFFFFFF0007)
#define PARALLEL_KEY_TRANSACTION_STATE		UINT64CONST(0xFFFFFFFFFFFF0008)
#define PARALLEL_KEY_EXTENSION_TRAMPOLINE	UINT64CONST(0xFFFFFFFFFFFF0009)

/* Fixed-size parallel state. */
typedef struct FixedParallelState
{
	/* Fixed-size state that workers must restore. */
	Oid			database_id;
	Oid			authenticated_user_id;
	Oid			current_user_id;
	int			sec_context;
	PGPROC	   *parallel_master_pgproc;
	pid_t		parallel_master_pid;
	BackendId	parallel_master_backend_id;

	/* Entrypoint for parallel workers. */
	parallel_worker_main_type entrypoint;

	/* Mutex protects remaining fields. */
	slock_t		mutex;

	/* Maximum XactLastRecEnd of any worker. */
	XLogRecPtr	last_xlog_end;
} FixedParallelState;

/*
 * Our parallel worker number.  We initialize this to -1, meaning that we are
 * not a parallel worker.  In parallel workers, it will be set to a value >= 0
 * and < the number of workers before any user code is invoked; each parallel
 * worker will get a different parallel worker number.
 */
int			ParallelWorkerNumber = -1;

/* Is there a parallel message pending which we need to receive? */
bool		ParallelMessagePending = false;

/* Are we initializing a parallel worker? */
bool		InitializingParallelWorker = false;

/* Pointer to our fixed parallel state. */
static FixedParallelState *MyFixedParallelState;

/* List of active parallel contexts. */
static dlist_head pcxt_list = DLIST_STATIC_INIT(pcxt_list);

/* Private functions. */
static void HandleParallelMessage(ParallelContext *, int, StringInfo msg);
static void ParallelErrorContext(void *arg);
static void ParallelExtensionTrampoline(dsm_segment *seg, shm_toc *toc);
static void ParallelWorkerMain(Datum main_arg);
static void WaitForParallelWorkersToExit(ParallelContext *pcxt);

/*
 * Establish a new parallel context.  This should be done after entering
 * parallel mode, and (unless there is an error) the context should be
 * destroyed before exiting the current subtransaction.
 */
ParallelContext *
CreateParallelContext(parallel_worker_main_type entrypoint, int nworkers)
{
	MemoryContext oldcontext;
	ParallelContext *pcxt;

	/* It is unsafe to create a parallel context if not in parallel mode. */
	Assert(IsInParallelMode());

	/* Number of workers should be non-negative. */
	Assert(nworkers >= 0);

	/*
	 * If dynamic shared memory is not available, we won't be able to use
	 * background workers.
	 */
	if (dynamic_shared_memory_type == DSM_IMPL_NONE)
		nworkers = 0;

	/*
	 * If we are running under serializable isolation, we can't use
	 * parallel workers, at least not until somebody enhances that mechanism
	 * to be parallel-aware.
	 */
	if (IsolationIsSerializable())
		nworkers = 0;

	/* We might be running in a short-lived memory context. */
	oldcontext = MemoryContextSwitchTo(TopTransactionContext);

	/* Initialize a new ParallelContext. */
	pcxt = palloc0(sizeof(ParallelContext));
	pcxt->subid = GetCurrentSubTransactionId();
	pcxt->nworkers = nworkers;
	pcxt->entrypoint = entrypoint;
	pcxt->error_context_stack = error_context_stack;
	shm_toc_initialize_estimator(&pcxt->estimator);
	dlist_push_head(&pcxt_list, &pcxt->node);

	/* Restore previous memory context. */
	MemoryContextSwitchTo(oldcontext);

	return pcxt;
}

/*
 * Establish a new parallel context that calls a function provided by an
 * extension.  This works around the fact that the library might get mapped
 * at a different address in each backend.
 */
ParallelContext *
CreateParallelContextForExternalFunction(char *library_name,
										 char *function_name,
										 int nworkers)
{
	MemoryContext oldcontext;
	ParallelContext *pcxt;

	/* We might be running in a very short-lived memory context. */
	oldcontext = MemoryContextSwitchTo(TopTransactionContext);

	/* Create the context. */
	pcxt = CreateParallelContext(ParallelExtensionTrampoline, nworkers);
	pcxt->library_name = pstrdup(library_name);
	pcxt->function_name = pstrdup(function_name);

	/* Restore previous memory context. */
	MemoryContextSwitchTo(oldcontext);

	return pcxt;
}

/*
 * Establish the dynamic shared memory segment for a parallel context and
 * copied state and other bookkeeping information that will need by parallel
 * workers into it.
 */
void
InitializeParallelDSM(ParallelContext *pcxt)
{
	MemoryContext oldcontext;
	Size		library_len = 0;
	Size		guc_len = 0;
	Size		combocidlen = 0;
	Size		tsnaplen = 0;
	Size		asnaplen = 0;
	Size		tstatelen = 0;
	Size		segsize = 0;
	int			i;
	FixedParallelState *fps;
	Snapshot	transaction_snapshot = GetTransactionSnapshot();
	Snapshot	active_snapshot = GetActiveSnapshot();

	/* We might be running in a very short-lived memory context. */
	oldcontext = MemoryContextSwitchTo(TopTransactionContext);

	/* Allow space to store the fixed-size parallel state. */
	shm_toc_estimate_chunk(&pcxt->estimator, sizeof(FixedParallelState));
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/*
	 * Normally, the user will have requested at least one worker process, but
	 * if by chance they have not, we can skip a bunch of things here.
	 */
	if (pcxt->nworkers > 0)
	{
		/* Estimate space for various kinds of state sharing. */
		library_len = EstimateLibraryStateSpace();
		shm_toc_estimate_chunk(&pcxt->estimator, library_len);
		guc_len = EstimateGUCStateSpace();
		shm_toc_estimate_chunk(&pcxt->estimator, guc_len);
		combocidlen = EstimateComboCIDStateSpace();
		shm_toc_estimate_chunk(&pcxt->estimator, combocidlen);
		tsnaplen = EstimateSnapshotSpace(transaction_snapshot);
		shm_toc_estimate_chunk(&pcxt->estimator, tsnaplen);
		asnaplen = EstimateSnapshotSpace(active_snapshot);
		shm_toc_estimate_chunk(&pcxt->estimator, asnaplen);
		tstatelen = EstimateTransactionStateSpace();
		shm_toc_estimate_chunk(&pcxt->estimator, tstatelen);
		/* If you add more chunks here, you probably need to add keys. */
		shm_toc_estimate_keys(&pcxt->estimator, 6);

		/* Estimate space need for error queues. */
		StaticAssertStmt(BUFFERALIGN(PARALLEL_ERROR_QUEUE_SIZE) ==
						 PARALLEL_ERROR_QUEUE_SIZE,
						 "parallel error queue size not buffer-aligned");
		shm_toc_estimate_chunk(&pcxt->estimator,
							   PARALLEL_ERROR_QUEUE_SIZE * pcxt->nworkers);
		shm_toc_estimate_keys(&pcxt->estimator, 1);

		/* Estimate how much we'll need for extension entrypoint info. */
		if (pcxt->library_name != NULL)
		{
			Assert(pcxt->entrypoint == ParallelExtensionTrampoline);
			Assert(pcxt->function_name != NULL);
			shm_toc_estimate_chunk(&pcxt->estimator, strlen(pcxt->library_name)
								   + strlen(pcxt->function_name) + 2);
			shm_toc_estimate_keys(&pcxt->estimator, 1);
		}
	}

	/*
	 * Create DSM and initialize with new table of contents.  But if the user
	 * didn't request any workers, then don't bother creating a dynamic shared
	 * memory segment; instead, just use backend-private memory.
	 *
	 * Also, if we can't create a dynamic shared memory segment because the
	 * maximum number of segments have already been created, then fall back to
	 * backend-private memory, and plan not to use any workers.  We hope this
	 * won't happen very often, but it's better to abandon the use of
	 * parallelism than to fail outright.
	 */
	segsize = shm_toc_estimate(&pcxt->estimator);
	if (pcxt->nworkers != 0)
		pcxt->seg = dsm_create(segsize, DSM_CREATE_NULL_IF_MAXSEGMENTS);
	if (pcxt->seg != NULL)
		pcxt->toc = shm_toc_create(PARALLEL_MAGIC,
								   dsm_segment_address(pcxt->seg),
								   segsize);
	else
	{
		pcxt->nworkers = 0;
		pcxt->private_memory = MemoryContextAlloc(TopMemoryContext, segsize);
		pcxt->toc = shm_toc_create(PARALLEL_MAGIC, pcxt->private_memory,
								   segsize);
	}

	/* Initialize fixed-size state in shared memory. */
	fps = (FixedParallelState *)
		shm_toc_allocate(pcxt->toc, sizeof(FixedParallelState));
	fps->database_id = MyDatabaseId;
	fps->authenticated_user_id = GetAuthenticatedUserId();
	GetUserIdAndSecContext(&fps->current_user_id, &fps->sec_context);
	fps->parallel_master_pgproc = MyProc;
	fps->parallel_master_pid = MyProcPid;
	fps->parallel_master_backend_id = MyBackendId;
	fps->entrypoint = pcxt->entrypoint;
	SpinLockInit(&fps->mutex);
	fps->last_xlog_end = 0;
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_FIXED, fps);

	/* We can skip the rest of this if we're not budgeting for any workers. */
	if (pcxt->nworkers > 0)
	{
		char	   *libraryspace;
		char	   *gucspace;
		char	   *combocidspace;
		char	   *tsnapspace;
		char	   *asnapspace;
		char	   *tstatespace;
		char	   *error_queue_space;

		/* Serialize shared libraries we have loaded. */
		libraryspace = shm_toc_allocate(pcxt->toc, library_len);
		SerializeLibraryState(library_len, libraryspace);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_LIBRARY, libraryspace);

		/* Serialize GUC settings. */
		gucspace = shm_toc_allocate(pcxt->toc, guc_len);
		SerializeGUCState(guc_len, gucspace);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_GUC, gucspace);

		/* Serialize combo CID state. */
		combocidspace = shm_toc_allocate(pcxt->toc, combocidlen);
		SerializeComboCIDState(combocidlen, combocidspace);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_COMBO_CID, combocidspace);

		/* Serialize transaction snapshot and active snapshot. */
		tsnapspace = shm_toc_allocate(pcxt->toc, tsnaplen);
		SerializeSnapshot(transaction_snapshot, tsnapspace);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_TRANSACTION_SNAPSHOT,
					   tsnapspace);
		asnapspace = shm_toc_allocate(pcxt->toc, asnaplen);
		SerializeSnapshot(active_snapshot, asnapspace);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_ACTIVE_SNAPSHOT, asnapspace);

		/* Serialize transaction state. */
		tstatespace = shm_toc_allocate(pcxt->toc, tstatelen);
		SerializeTransactionState(tstatelen, tstatespace);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_TRANSACTION_STATE, tstatespace);

		/* Allocate space for worker information. */
		pcxt->worker = palloc0(sizeof(ParallelWorkerInfo) * pcxt->nworkers);

		/*
		 * Establish error queues in dynamic shared memory.
		 *
		 * These queues should be used only for transmitting ErrorResponse,
		 * NoticeResponse, and NotifyResponse protocol messages.  Tuple data
		 * should be transmitted via separate (possibly larger?) queues.
		 */
		error_queue_space =
			shm_toc_allocate(pcxt->toc,
							 PARALLEL_ERROR_QUEUE_SIZE * pcxt->nworkers);
		for (i = 0; i < pcxt->nworkers; ++i)
		{
			char	   *start;
			shm_mq	   *mq;

			start = error_queue_space + i * PARALLEL_ERROR_QUEUE_SIZE;
			mq = shm_mq_create(start, PARALLEL_ERROR_QUEUE_SIZE);
			shm_mq_set_receiver(mq, MyProc);
			pcxt->worker[i].error_mqh = shm_mq_attach(mq, pcxt->seg, NULL);
		}
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_ERROR_QUEUE, error_queue_space);

		/* Serialize extension entrypoint information. */
		if (pcxt->library_name != NULL)
		{
			Size		lnamelen = strlen(pcxt->library_name);
			char	   *extensionstate;

			extensionstate = shm_toc_allocate(pcxt->toc, lnamelen
										  + strlen(pcxt->function_name) + 2);
			strcpy(extensionstate, pcxt->library_name);
			strcpy(extensionstate + lnamelen + 1, pcxt->function_name);
			shm_toc_insert(pcxt->toc, PARALLEL_KEY_EXTENSION_TRAMPOLINE,
						   extensionstate);
		}
	}

	/* Restore previous memory context. */
	MemoryContextSwitchTo(oldcontext);
}

/*
 * Reinitialize the dynamic shared memory segment for a parallel context such
 * that we could launch workers for it again.
 */
void
ReinitializeParallelDSM(ParallelContext *pcxt)
{
	FixedParallelState *fps;
	char	   *error_queue_space;
	int			i;

	if (pcxt->nworkers_launched == 0)
		return;

	WaitForParallelWorkersToFinish(pcxt);
	WaitForParallelWorkersToExit(pcxt);

	/* Reset a few bits of fixed parallel state to a clean state. */
	fps = shm_toc_lookup(pcxt->toc, PARALLEL_KEY_FIXED);
	fps->last_xlog_end = 0;

	/* Recreate error queues. */
	error_queue_space =
		shm_toc_lookup(pcxt->toc, PARALLEL_KEY_ERROR_QUEUE);
	for (i = 0; i < pcxt->nworkers; ++i)
	{
		char	   *start;
		shm_mq	   *mq;

		start = error_queue_space + i * PARALLEL_ERROR_QUEUE_SIZE;
		mq = shm_mq_create(start, PARALLEL_ERROR_QUEUE_SIZE);
		shm_mq_set_receiver(mq, MyProc);
		pcxt->worker[i].error_mqh = shm_mq_attach(mq, pcxt->seg, NULL);
	}

	/* Reset number of workers launched. */
	pcxt->nworkers_launched = 0;
}

/*
 * Launch parallel workers.
 */
void
LaunchParallelWorkers(ParallelContext *pcxt)
{
	MemoryContext oldcontext;
	BackgroundWorker worker;
	int			i;
	bool		any_registrations_failed = false;

	/* Skip this if we have no workers. */
	if (pcxt->nworkers == 0)
		return;

	/* We need to be a lock group leader. */
	BecomeLockGroupLeader();

	/* If we do have workers, we'd better have a DSM segment. */
	Assert(pcxt->seg != NULL);

	/* We might be running in a short-lived memory context. */
	oldcontext = MemoryContextSwitchTo(TopTransactionContext);

	/* Configure a worker. */
	snprintf(worker.bgw_name, BGW_MAXLEN, "parallel worker for PID %d",
			 MyProcPid);
	worker.bgw_flags =
		BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	worker.bgw_main = ParallelWorkerMain;
	worker.bgw_main_arg = UInt32GetDatum(dsm_segment_handle(pcxt->seg));
	worker.bgw_notify_pid = MyProcPid;
	memset(&worker.bgw_extra, 0, BGW_EXTRALEN);

	/*
	 * Start workers.
	 *
	 * The caller must be able to tolerate ending up with fewer workers than
	 * expected, so there is no need to throw an error here if registration
	 * fails.  It wouldn't help much anyway, because registering the worker in
	 * no way guarantees that it will start up and initialize successfully.
	 */
	for (i = 0; i < pcxt->nworkers; ++i)
	{
		memcpy(worker.bgw_extra, &i, sizeof(int));
		if (!any_registrations_failed &&
			RegisterDynamicBackgroundWorker(&worker,
											&pcxt->worker[i].bgwhandle))
		{
			shm_mq_set_handle(pcxt->worker[i].error_mqh,
							  pcxt->worker[i].bgwhandle);
			pcxt->nworkers_launched++;
		}
		else
		{
			/*
			 * If we weren't able to register the worker, then we've bumped up
			 * against the max_worker_processes limit, and future
			 * registrations will probably fail too, so arrange to skip them.
			 * But we still have to execute this code for the remaining slots
			 * to make sure that we forget about the error queues we budgeted
			 * for those workers.  Otherwise, we'll wait for them to start,
			 * but they never will.
			 */
			any_registrations_failed = true;
			pcxt->worker[i].bgwhandle = NULL;
			pcxt->worker[i].error_mqh = NULL;
		}
	}

	/* Restore previous memory context. */
	MemoryContextSwitchTo(oldcontext);
}

/*
 * Wait for all workers to finish computing.
 *
 * Even if the parallel operation seems to have completed successfully, it's
 * important to call this function afterwards.  We must not miss any errors
 * the workers may have thrown during the parallel operation, or any that they
 * may yet throw while shutting down.
 *
 * Also, we want to update our notion of XactLastRecEnd based on worker
 * feedback.
 */
void
WaitForParallelWorkersToFinish(ParallelContext *pcxt)
{
	for (;;)
	{
		bool		anyone_alive = false;
		int			i;

		/*
		 * This will process any parallel messages that are pending, which may
		 * change the outcome of the loop that follows.  It may also throw an
		 * error propagated from a worker.
		 */
		CHECK_FOR_INTERRUPTS();

		for (i = 0; i < pcxt->nworkers; ++i)
		{
			if (pcxt->worker[i].error_mqh != NULL)
			{
				anyone_alive = true;
				break;
			}
		}

		if (!anyone_alive)
			break;

		WaitLatch(&MyProc->procLatch, WL_LATCH_SET, -1);
		ResetLatch(&MyProc->procLatch);
	}

	if (pcxt->toc != NULL)
	{
		FixedParallelState *fps;

		fps = shm_toc_lookup(pcxt->toc, PARALLEL_KEY_FIXED);
		if (fps->last_xlog_end > XactLastRecEnd)
			XactLastRecEnd = fps->last_xlog_end;
	}
}

/*
 * Wait for all workers to exit.
 *
 * This function ensures that workers have been completely shutdown.  The
 * difference between WaitForParallelWorkersToFinish and this function is
 * that former just ensures that last message sent by worker backend is
 * received by master backend whereas this ensures the complete shutdown.
 */
static void
WaitForParallelWorkersToExit(ParallelContext *pcxt)
{
	int			i;

	/* Wait until the workers actually die. */
	for (i = 0; i < pcxt->nworkers; ++i)
	{
		BgwHandleStatus status;

		if (pcxt->worker == NULL || pcxt->worker[i].bgwhandle == NULL)
			continue;

		status = WaitForBackgroundWorkerShutdown(pcxt->worker[i].bgwhandle);

		/*
		 * If the postmaster kicked the bucket, we have no chance of cleaning
		 * up safely -- we won't be able to tell when our workers are actually
		 * dead.  This doesn't necessitate a PANIC since they will all abort
		 * eventually, but we can't safely continue this session.
		 */
		if (status == BGWH_POSTMASTER_DIED)
			ereport(FATAL,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
				 errmsg("postmaster exited during a parallel transaction")));

		/* Release memory. */
		pfree(pcxt->worker[i].bgwhandle);
		pcxt->worker[i].bgwhandle = NULL;
	}
}

/*
 * Destroy a parallel context.
 *
 * If expecting a clean exit, you should use WaitForParallelWorkersToFinish()
 * first, before calling this function.  When this function is invoked, any
 * remaining workers are forcibly killed; the dynamic shared memory segment
 * is unmapped; and we then wait (uninterruptibly) for the workers to exit.
 */
void
DestroyParallelContext(ParallelContext *pcxt)
{
	int			i;

	/*
	 * Be careful about order of operations here!  We remove the parallel
	 * context from the list before we do anything else; otherwise, if an
	 * error occurs during a subsequent step, we might try to nuke it again
	 * from AtEOXact_Parallel or AtEOSubXact_Parallel.
	 */
	dlist_delete(&pcxt->node);

	/* Kill each worker in turn, and forget their error queues. */
	if (pcxt->worker != NULL)
	{
		for (i = 0; i < pcxt->nworkers; ++i)
		{
			if (pcxt->worker[i].error_mqh != NULL)
			{
				TerminateBackgroundWorker(pcxt->worker[i].bgwhandle);

				pfree(pcxt->worker[i].error_mqh);
				pcxt->worker[i].error_mqh = NULL;
			}
		}
	}

	/*
	 * If we have allocated a shared memory segment, detach it.  This will
	 * implicitly detach the error queues, and any other shared memory queues,
	 * stored there.
	 */
	if (pcxt->seg != NULL)
	{
		dsm_detach(pcxt->seg);
		pcxt->seg = NULL;
	}

	/*
	 * If this parallel context is actually in backend-private memory rather
	 * than shared memory, free that memory instead.
	 */
	if (pcxt->private_memory != NULL)
	{
		pfree(pcxt->private_memory);
		pcxt->private_memory = NULL;
	}

	/*
	 * We can't finish transaction commit or abort until all of the
	 * workers have exited.  This means, in particular, that we can't respond
	 * to interrupts at this stage.
	 */
	HOLD_INTERRUPTS();
	WaitForParallelWorkersToExit(pcxt);
	RESUME_INTERRUPTS();

	/* Free the worker array itself. */
	if (pcxt->worker != NULL)
	{
		pfree(pcxt->worker);
		pcxt->worker = NULL;
	}

	/* Free memory. */
	pfree(pcxt);
}

/*
 * Are there any parallel contexts currently active?
 */
bool
ParallelContextActive(void)
{
	return !dlist_is_empty(&pcxt_list);
}

/*
 * Handle receipt of an interrupt indicating a parallel worker message.
 */
void
HandleParallelMessageInterrupt(void)
{
	int			save_errno = errno;

	InterruptPending = true;
	ParallelMessagePending = true;
	SetLatch(MyLatch);

	errno = save_errno;
}

/*
 * Handle any queued protocol messages received from parallel workers.
 */
void
HandleParallelMessages(void)
{
	dlist_iter	iter;

	ParallelMessagePending = false;

	dlist_foreach(iter, &pcxt_list)
	{
		ParallelContext *pcxt;
		int			i;
		Size		nbytes;
		void	   *data;

		pcxt = dlist_container(ParallelContext, node, iter.cur);
		if (pcxt->worker == NULL)
			continue;

		for (i = 0; i < pcxt->nworkers; ++i)
		{
			/*
			 * Read as many messages as we can from each worker, but stop when
			 * either (1) the error queue goes away, which can happen if we
			 * receive a Terminate message from the worker; or (2) no more
			 * messages can be read from the worker without blocking.
			 */
			while (pcxt->worker[i].error_mqh != NULL)
			{
				shm_mq_result res;

				res = shm_mq_receive(pcxt->worker[i].error_mqh, &nbytes,
									 &data, true);
				if (res == SHM_MQ_WOULD_BLOCK)
					break;
				else if (res == SHM_MQ_SUCCESS)
				{
					StringInfoData msg;

					initStringInfo(&msg);
					appendBinaryStringInfo(&msg, data, nbytes);
					HandleParallelMessage(pcxt, i, &msg);
					pfree(msg.data);
				}
				else
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),	/* XXX: wrong errcode? */
							 errmsg("lost connection to parallel worker")));

				/* This might make the error queue go away. */
				CHECK_FOR_INTERRUPTS();
			}
		}
	}
}

/*
 * Handle a single protocol message received from a single parallel worker.
 */
static void
HandleParallelMessage(ParallelContext *pcxt, int i, StringInfo msg)
{
	char		msgtype;

	msgtype = pq_getmsgbyte(msg);

	switch (msgtype)
	{
		case 'K':				/* BackendKeyData */
			{
				int32		pid = pq_getmsgint(msg, 4);

				(void) pq_getmsgint(msg, 4);	/* discard cancel key */
				(void) pq_getmsgend(msg);
				pcxt->worker[i].pid = pid;
				break;
			}

		case 'E':				/* ErrorResponse */
		case 'N':				/* NoticeResponse */
			{
				ErrorData	edata;
				ErrorContextCallback errctx;
				ErrorContextCallback *save_error_context_stack;

				/*
				 * Rethrow the error using the error context callbacks that
				 * were in effect when the context was created, not the
				 * current ones.
				 */
				save_error_context_stack = error_context_stack;
				errctx.callback = ParallelErrorContext;
				errctx.arg = &pcxt->worker[i].pid;
				errctx.previous = pcxt->error_context_stack;
				error_context_stack = &errctx;

				/* Parse ErrorResponse or NoticeResponse. */
				pq_parse_errornotice(msg, &edata);

				/* Death of a worker isn't enough justification for suicide. */
				edata.elevel = Min(edata.elevel, ERROR);

				/* Rethrow error or notice. */
				ThrowErrorData(&edata);

				/* Restore previous context. */
				error_context_stack = save_error_context_stack;

				break;
			}

		case 'A':				/* NotifyResponse */
			{
				/* Propagate NotifyResponse. */
				pq_putmessage(msg->data[0], &msg->data[1], msg->len - 1);
				break;
			}

		case 'X':				/* Terminate, indicating clean exit */
			{
				pfree(pcxt->worker[i].error_mqh);
				pcxt->worker[i].error_mqh = NULL;
				break;
			}

		default:
			{
				elog(ERROR, "unknown message type: %c (%d bytes)",
					 msgtype, msg->len);
			}
	}
}

/*
 * End-of-subtransaction cleanup for parallel contexts.
 *
 * Currently, it's forbidden to enter or leave a subtransaction while
 * parallel mode is in effect, so we could just blow away everything.  But
 * we may want to relax that restriction in the future, so this code
 * contemplates that there may be multiple subtransaction IDs in pcxt_list.
 */
void
AtEOSubXact_Parallel(bool isCommit, SubTransactionId mySubId)
{
	while (!dlist_is_empty(&pcxt_list))
	{
		ParallelContext *pcxt;

		pcxt = dlist_head_element(ParallelContext, node, &pcxt_list);
		if (pcxt->subid != mySubId)
			break;
		if (isCommit)
			elog(WARNING, "leaked parallel context");
		DestroyParallelContext(pcxt);
	}
}

/*
 * End-of-transaction cleanup for parallel contexts.
 */
void
AtEOXact_Parallel(bool isCommit)
{
	while (!dlist_is_empty(&pcxt_list))
	{
		ParallelContext *pcxt;

		pcxt = dlist_head_element(ParallelContext, node, &pcxt_list);
		if (isCommit)
			elog(WARNING, "leaked parallel context");
		DestroyParallelContext(pcxt);
	}
}

/*
 * Main entrypoint for parallel workers.
 */
static void
ParallelWorkerMain(Datum main_arg)
{
	dsm_segment *seg;
	shm_toc    *toc;
	FixedParallelState *fps;
	char	   *error_queue_space;
	shm_mq	   *mq;
	shm_mq_handle *mqh;
	char	   *libraryspace;
	char	   *gucspace;
	char	   *combocidspace;
	char	   *tsnapspace;
	char	   *asnapspace;
	char	   *tstatespace;
	StringInfoData msgbuf;

	/* Set flag to indicate that we're initializing a parallel worker. */
	InitializingParallelWorker = true;

	/* Establish signal handlers. */
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	/* Determine and set our parallel worker number. */
	Assert(ParallelWorkerNumber == -1);
	memcpy(&ParallelWorkerNumber, MyBgworkerEntry->bgw_extra, sizeof(int));

	/* Set up a memory context and resource owner. */
	Assert(CurrentResourceOwner == NULL);
	CurrentResourceOwner = ResourceOwnerCreate(NULL, "parallel toplevel");
	CurrentMemoryContext = AllocSetContextCreate(TopMemoryContext,
												 "parallel worker",
												 ALLOCSET_DEFAULT_MINSIZE,
												 ALLOCSET_DEFAULT_INITSIZE,
												 ALLOCSET_DEFAULT_MAXSIZE);

	/*
	 * Now that we have a resource owner, we can attach to the dynamic shared
	 * memory segment and read the table of contents.
	 */
	seg = dsm_attach(DatumGetUInt32(main_arg));
	if (seg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("could not map dynamic shared memory segment")));
	toc = shm_toc_attach(PARALLEL_MAGIC, dsm_segment_address(seg));
	if (toc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			   errmsg("invalid magic number in dynamic shared memory segment")));

	/* Look up fixed parallel state. */
	fps = shm_toc_lookup(toc, PARALLEL_KEY_FIXED);
	Assert(fps != NULL);
	MyFixedParallelState = fps;

	/*
	 * Now that we have a worker number, we can find and attach to the error
	 * queue provided for us.  That's good, because until we do that, any
	 * errors that happen here will not be reported back to the process that
	 * requested that this worker be launched.
	 */
	error_queue_space = shm_toc_lookup(toc, PARALLEL_KEY_ERROR_QUEUE);
	mq = (shm_mq *) (error_queue_space +
					 ParallelWorkerNumber * PARALLEL_ERROR_QUEUE_SIZE);
	shm_mq_set_sender(mq, MyProc);
	mqh = shm_mq_attach(mq, seg, NULL);
	pq_redirect_to_shm_mq(seg, mqh);
	pq_set_parallel_master(fps->parallel_master_pid,
						   fps->parallel_master_backend_id);

	/*
	 * Send a BackendKeyData message to the process that initiated parallelism
	 * so that it has access to our PID before it receives any other messages
	 * from us.  Our cancel key is sent, too, since that's the way the
	 * protocol message is defined, but it won't actually be used for anything
	 * in this case.
	 */
	pq_beginmessage(&msgbuf, 'K');
	pq_sendint(&msgbuf, (int32) MyProcPid, sizeof(int32));
	pq_sendint(&msgbuf, (int32) MyCancelKey, sizeof(int32));
	pq_endmessage(&msgbuf);

	/*
	 * Hooray! Primary initialization is complete.  Now, we need to set up our
	 * backend-local state to match the original backend.
	 */

	/*
	 * Join locking group.  We must do this before anything that could try
	 * to acquire a heavyweight lock, because any heavyweight locks acquired
	 * to this point could block either directly against the parallel group
	 * leader or against some process which in turn waits for a lock that
	 * conflicts with the parallel group leader, causing an undetected
	 * deadlock.  (If we can't join the lock group, the leader has gone away,
	 * so just exit quietly.)
	 */
	if (!BecomeLockGroupMember(fps->parallel_master_pgproc,
							   fps->parallel_master_pid))
		return;

	/*
	 * Load libraries that were loaded by original backend.  We want to do
	 * this before restoring GUCs, because the libraries might define custom
	 * variables.
	 */
	libraryspace = shm_toc_lookup(toc, PARALLEL_KEY_LIBRARY);
	Assert(libraryspace != NULL);
	RestoreLibraryState(libraryspace);

	/* Restore database connection. */
	BackgroundWorkerInitializeConnectionByOid(fps->database_id,
											  fps->authenticated_user_id);

	/* Restore GUC values from launching backend. */
	gucspace = shm_toc_lookup(toc, PARALLEL_KEY_GUC);
	Assert(gucspace != NULL);
	StartTransactionCommand();
	RestoreGUCState(gucspace);
	CommitTransactionCommand();

	/* Crank up a transaction state appropriate to a parallel worker. */
	tstatespace = shm_toc_lookup(toc, PARALLEL_KEY_TRANSACTION_STATE);
	StartParallelWorkerTransaction(tstatespace);

	/* Restore combo CID state. */
	combocidspace = shm_toc_lookup(toc, PARALLEL_KEY_COMBO_CID);
	Assert(combocidspace != NULL);
	RestoreComboCIDState(combocidspace);

	/* Restore transaction snapshot. */
	tsnapspace = shm_toc_lookup(toc, PARALLEL_KEY_TRANSACTION_SNAPSHOT);
	Assert(tsnapspace != NULL);
	RestoreTransactionSnapshot(RestoreSnapshot(tsnapspace),
							   fps->parallel_master_pgproc);

	/* Restore active snapshot. */
	asnapspace = shm_toc_lookup(toc, PARALLEL_KEY_ACTIVE_SNAPSHOT);
	Assert(asnapspace != NULL);
	PushActiveSnapshot(RestoreSnapshot(asnapspace));

	/*
	 * We've changed which tuples we can see, and must therefore invalidate
	 * system caches.
	 */
	InvalidateSystemCaches();

	/* Restore user ID and security context. */
	SetUserIdAndSecContext(fps->current_user_id, fps->sec_context);

	/*
	 * We've initialized all of our state now; nothing should change
	 * hereafter.
	 */
	InitializingParallelWorker = false;
	EnterParallelMode();

	/*
	 * Time to do the real work: invoke the caller-supplied code.
	 *
	 * If you get a crash at this line, see the comments for
	 * ParallelExtensionTrampoline.
	 */
	fps->entrypoint(seg, toc);

	/* Must exit parallel mode to pop active snapshot. */
	ExitParallelMode();

	/* Must pop active snapshot so resowner.c doesn't complain. */
	PopActiveSnapshot();

	/* Shut down the parallel-worker transaction. */
	EndParallelWorkerTransaction();

	/* Report success. */
	pq_putmessage('X', NULL, 0);
}

/*
 * It's unsafe for the entrypoint invoked by ParallelWorkerMain to be a
 * function living in a dynamically loaded module, because the module might
 * not be loaded in every process, or might be loaded but not at the same
 * address.  To work around that problem, CreateParallelContextForExtension()
 * arranges to call this function rather than calling the extension-provided
 * function directly; and this function then looks up the real entrypoint and
 * calls it.
 */
static void
ParallelExtensionTrampoline(dsm_segment *seg, shm_toc *toc)
{
	char	   *extensionstate;
	char	   *library_name;
	char	   *function_name;
	parallel_worker_main_type entrypt;

	extensionstate = shm_toc_lookup(toc, PARALLEL_KEY_EXTENSION_TRAMPOLINE);
	Assert(extensionstate != NULL);
	library_name = extensionstate;
	function_name = extensionstate + strlen(library_name) + 1;

	entrypt = (parallel_worker_main_type)
		load_external_function(library_name, function_name, true, NULL);
	entrypt(seg, toc);
}

/*
 * Give the user a hint that this is a message propagated from a parallel
 * worker.  Otherwise, it can sometimes be confusing to understand what
 * actually happened.
 */
static void
ParallelErrorContext(void *arg)
{
	if (force_parallel_mode != FORCE_PARALLEL_REGRESS)
		errcontext("parallel worker, PID %d", *(int32 *) arg);
}

/*
 * Update shared memory with the ending location of the last WAL record we
 * wrote, if it's greater than the value already stored there.
 */
void
ParallelWorkerReportLastRecEnd(XLogRecPtr last_xlog_end)
{
	FixedParallelState *fps = MyFixedParallelState;

	Assert(fps != NULL);
	SpinLockAcquire(&fps->mutex);
	if (fps->last_xlog_end < last_xlog_end)
		fps->last_xlog_end = last_xlog_end;
	SpinLockRelease(&fps->mutex);
}
