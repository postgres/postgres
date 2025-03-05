/*-------------------------------------------------------------------------
 *
 * parallel.c
 *	  Infrastructure for launching parallel workers
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/transam/parallel.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/brin.h"
#include "access/gin.h"
#include "access/nbtree.h"
#include "access/parallel.h"
#include "access/session.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_enum.h"
#include "catalog/storage.h"
#include "commands/async.h"
#include "commands/vacuum.h"
#include "executor/execParallel.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "libpq/pqmq.h"
#include "miscadmin.h"
#include "optimizer/optimizer.h"
#include "pgstat.h"
#include "storage/ipc.h"
#include "storage/predicate.h"
#include "storage/spin.h"
#include "tcop/tcopprot.h"
#include "utils/combocid.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/relmapper.h"
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
 * Magic numbers for per-context parallel state sharing.  Higher-level code
 * should use smaller values, leaving these very large ones for use by this
 * module.
 */
#define PARALLEL_KEY_FIXED					UINT64CONST(0xFFFFFFFFFFFF0001)
#define PARALLEL_KEY_ERROR_QUEUE			UINT64CONST(0xFFFFFFFFFFFF0002)
#define PARALLEL_KEY_LIBRARY				UINT64CONST(0xFFFFFFFFFFFF0003)
#define PARALLEL_KEY_GUC					UINT64CONST(0xFFFFFFFFFFFF0004)
#define PARALLEL_KEY_COMBO_CID				UINT64CONST(0xFFFFFFFFFFFF0005)
#define PARALLEL_KEY_TRANSACTION_SNAPSHOT	UINT64CONST(0xFFFFFFFFFFFF0006)
#define PARALLEL_KEY_ACTIVE_SNAPSHOT		UINT64CONST(0xFFFFFFFFFFFF0007)
#define PARALLEL_KEY_TRANSACTION_STATE		UINT64CONST(0xFFFFFFFFFFFF0008)
#define PARALLEL_KEY_ENTRYPOINT				UINT64CONST(0xFFFFFFFFFFFF0009)
#define PARALLEL_KEY_SESSION_DSM			UINT64CONST(0xFFFFFFFFFFFF000A)
#define PARALLEL_KEY_PENDING_SYNCS			UINT64CONST(0xFFFFFFFFFFFF000B)
#define PARALLEL_KEY_REINDEX_STATE			UINT64CONST(0xFFFFFFFFFFFF000C)
#define PARALLEL_KEY_RELMAPPER_STATE		UINT64CONST(0xFFFFFFFFFFFF000D)
#define PARALLEL_KEY_UNCOMMITTEDENUMS		UINT64CONST(0xFFFFFFFFFFFF000E)
#define PARALLEL_KEY_CLIENTCONNINFO			UINT64CONST(0xFFFFFFFFFFFF000F)

/* Fixed-size parallel state. */
typedef struct FixedParallelState
{
	/* Fixed-size state that workers must restore. */
	Oid			database_id;
	Oid			authenticated_user_id;
	Oid			session_user_id;
	Oid			outer_user_id;
	Oid			current_user_id;
	Oid			temp_namespace_id;
	Oid			temp_toast_namespace_id;
	int			sec_context;
	bool		session_user_is_superuser;
	bool		role_is_superuser;
	PGPROC	   *parallel_leader_pgproc;
	pid_t		parallel_leader_pid;
	ProcNumber	parallel_leader_proc_number;
	TimestampTz xact_ts;
	TimestampTz stmt_ts;
	SerializableXactHandle serializable_xact_handle;

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
volatile sig_atomic_t ParallelMessagePending = false;

/* Are we initializing a parallel worker? */
bool		InitializingParallelWorker = false;

/* Pointer to our fixed parallel state. */
static FixedParallelState *MyFixedParallelState;

/* List of active parallel contexts. */
static dlist_head pcxt_list = DLIST_STATIC_INIT(pcxt_list);

/* Backend-local copy of data from FixedParallelState. */
static pid_t ParallelLeaderPid;

/*
 * List of internal parallel worker entry points.  We need this for
 * reasons explained in LookupParallelWorkerFunction(), below.
 */
static const struct
{
	const char *fn_name;
	parallel_worker_main_type fn_addr;
}			InternalParallelWorkers[] =

{
	{
		"ParallelQueryMain", ParallelQueryMain
	},
	{
		"_bt_parallel_build_main", _bt_parallel_build_main
	},
	{
		"_brin_parallel_build_main", _brin_parallel_build_main
	},
	{
		"_gin_parallel_build_main", _gin_parallel_build_main
	},
	{
		"parallel_vacuum_main", parallel_vacuum_main
	}
};

/* Private functions. */
static void ProcessParallelMessage(ParallelContext *pcxt, int i, StringInfo msg);
static void WaitForParallelWorkersToExit(ParallelContext *pcxt);
static parallel_worker_main_type LookupParallelWorkerFunction(const char *libraryname, const char *funcname);
static void ParallelWorkerShutdown(int code, Datum arg);


/*
 * Establish a new parallel context.  This should be done after entering
 * parallel mode, and (unless there is an error) the context should be
 * destroyed before exiting the current subtransaction.
 */
ParallelContext *
CreateParallelContext(const char *library_name, const char *function_name,
					  int nworkers)
{
	MemoryContext oldcontext;
	ParallelContext *pcxt;

	/* It is unsafe to create a parallel context if not in parallel mode. */
	Assert(IsInParallelMode());

	/* Number of workers should be non-negative. */
	Assert(nworkers >= 0);

	/* We might be running in a short-lived memory context. */
	oldcontext = MemoryContextSwitchTo(TopTransactionContext);

	/* Initialize a new ParallelContext. */
	pcxt = palloc0(sizeof(ParallelContext));
	pcxt->subid = GetCurrentSubTransactionId();
	pcxt->nworkers = nworkers;
	pcxt->nworkers_to_launch = nworkers;
	pcxt->library_name = pstrdup(library_name);
	pcxt->function_name = pstrdup(function_name);
	pcxt->error_context_stack = error_context_stack;
	shm_toc_initialize_estimator(&pcxt->estimator);
	dlist_push_head(&pcxt_list, &pcxt->node);

	/* Restore previous memory context. */
	MemoryContextSwitchTo(oldcontext);

	return pcxt;
}

/*
 * Establish the dynamic shared memory segment for a parallel context and
 * copy state and other bookkeeping information that will be needed by
 * parallel workers into it.
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
	Size		pendingsyncslen = 0;
	Size		reindexlen = 0;
	Size		relmapperlen = 0;
	Size		uncommittedenumslen = 0;
	Size		clientconninfolen = 0;
	Size		segsize = 0;
	int			i;
	FixedParallelState *fps;
	dsm_handle	session_dsm_handle = DSM_HANDLE_INVALID;
	Snapshot	transaction_snapshot = GetTransactionSnapshot();
	Snapshot	active_snapshot = GetActiveSnapshot();

	/* We might be running in a very short-lived memory context. */
	oldcontext = MemoryContextSwitchTo(TopTransactionContext);

	/* Allow space to store the fixed-size parallel state. */
	shm_toc_estimate_chunk(&pcxt->estimator, sizeof(FixedParallelState));
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/*
	 * If we manage to reach here while non-interruptible, it's unsafe to
	 * launch any workers: we would fail to process interrupts sent by them.
	 * We can deal with that edge case by pretending no workers were
	 * requested.
	 */
	if (!INTERRUPTS_CAN_BE_PROCESSED())
		pcxt->nworkers = 0;

	/*
	 * Normally, the user will have requested at least one worker process, but
	 * if by chance they have not, we can skip a bunch of things here.
	 */
	if (pcxt->nworkers > 0)
	{
		/* Get (or create) the per-session DSM segment's handle. */
		session_dsm_handle = GetSessionDsmHandle();

		/*
		 * If we weren't able to create a per-session DSM segment, then we can
		 * continue but we can't safely launch any workers because their
		 * record typmods would be incompatible so they couldn't exchange
		 * tuples.
		 */
		if (session_dsm_handle == DSM_HANDLE_INVALID)
			pcxt->nworkers = 0;
	}

	if (pcxt->nworkers > 0)
	{
		/* Estimate space for various kinds of state sharing. */
		library_len = EstimateLibraryStateSpace();
		shm_toc_estimate_chunk(&pcxt->estimator, library_len);
		guc_len = EstimateGUCStateSpace();
		shm_toc_estimate_chunk(&pcxt->estimator, guc_len);
		combocidlen = EstimateComboCIDStateSpace();
		shm_toc_estimate_chunk(&pcxt->estimator, combocidlen);
		if (IsolationUsesXactSnapshot())
		{
			tsnaplen = EstimateSnapshotSpace(transaction_snapshot);
			shm_toc_estimate_chunk(&pcxt->estimator, tsnaplen);
		}
		asnaplen = EstimateSnapshotSpace(active_snapshot);
		shm_toc_estimate_chunk(&pcxt->estimator, asnaplen);
		tstatelen = EstimateTransactionStateSpace();
		shm_toc_estimate_chunk(&pcxt->estimator, tstatelen);
		shm_toc_estimate_chunk(&pcxt->estimator, sizeof(dsm_handle));
		pendingsyncslen = EstimatePendingSyncsSpace();
		shm_toc_estimate_chunk(&pcxt->estimator, pendingsyncslen);
		reindexlen = EstimateReindexStateSpace();
		shm_toc_estimate_chunk(&pcxt->estimator, reindexlen);
		relmapperlen = EstimateRelationMapSpace();
		shm_toc_estimate_chunk(&pcxt->estimator, relmapperlen);
		uncommittedenumslen = EstimateUncommittedEnumsSpace();
		shm_toc_estimate_chunk(&pcxt->estimator, uncommittedenumslen);
		clientconninfolen = EstimateClientConnectionInfoSpace();
		shm_toc_estimate_chunk(&pcxt->estimator, clientconninfolen);
		/* If you add more chunks here, you probably need to add keys. */
		shm_toc_estimate_keys(&pcxt->estimator, 12);

		/* Estimate space need for error queues. */
		StaticAssertStmt(BUFFERALIGN(PARALLEL_ERROR_QUEUE_SIZE) ==
						 PARALLEL_ERROR_QUEUE_SIZE,
						 "parallel error queue size not buffer-aligned");
		shm_toc_estimate_chunk(&pcxt->estimator,
							   mul_size(PARALLEL_ERROR_QUEUE_SIZE,
										pcxt->nworkers));
		shm_toc_estimate_keys(&pcxt->estimator, 1);

		/* Estimate how much we'll need for the entrypoint info. */
		shm_toc_estimate_chunk(&pcxt->estimator, strlen(pcxt->library_name) +
							   strlen(pcxt->function_name) + 2);
		shm_toc_estimate_keys(&pcxt->estimator, 1);
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
	if (pcxt->nworkers > 0)
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
	fps->session_user_id = GetSessionUserId();
	fps->outer_user_id = GetCurrentRoleId();
	GetUserIdAndSecContext(&fps->current_user_id, &fps->sec_context);
	fps->session_user_is_superuser = GetSessionUserIsSuperuser();
	fps->role_is_superuser = current_role_is_superuser;
	GetTempNamespaceState(&fps->temp_namespace_id,
						  &fps->temp_toast_namespace_id);
	fps->parallel_leader_pgproc = MyProc;
	fps->parallel_leader_pid = MyProcPid;
	fps->parallel_leader_proc_number = MyProcNumber;
	fps->xact_ts = GetCurrentTransactionStartTimestamp();
	fps->stmt_ts = GetCurrentStatementStartTimestamp();
	fps->serializable_xact_handle = ShareSerializableXact();
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
		char	   *pendingsyncsspace;
		char	   *reindexspace;
		char	   *relmapperspace;
		char	   *error_queue_space;
		char	   *session_dsm_handle_space;
		char	   *entrypointstate;
		char	   *uncommittedenumsspace;
		char	   *clientconninfospace;
		Size		lnamelen;

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

		/*
		 * Serialize the transaction snapshot if the transaction isolation
		 * level uses a transaction snapshot.
		 */
		if (IsolationUsesXactSnapshot())
		{
			tsnapspace = shm_toc_allocate(pcxt->toc, tsnaplen);
			SerializeSnapshot(transaction_snapshot, tsnapspace);
			shm_toc_insert(pcxt->toc, PARALLEL_KEY_TRANSACTION_SNAPSHOT,
						   tsnapspace);
		}

		/* Serialize the active snapshot. */
		asnapspace = shm_toc_allocate(pcxt->toc, asnaplen);
		SerializeSnapshot(active_snapshot, asnapspace);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_ACTIVE_SNAPSHOT, asnapspace);

		/* Provide the handle for per-session segment. */
		session_dsm_handle_space = shm_toc_allocate(pcxt->toc,
													sizeof(dsm_handle));
		*(dsm_handle *) session_dsm_handle_space = session_dsm_handle;
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_SESSION_DSM,
					   session_dsm_handle_space);

		/* Serialize transaction state. */
		tstatespace = shm_toc_allocate(pcxt->toc, tstatelen);
		SerializeTransactionState(tstatelen, tstatespace);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_TRANSACTION_STATE, tstatespace);

		/* Serialize pending syncs. */
		pendingsyncsspace = shm_toc_allocate(pcxt->toc, pendingsyncslen);
		SerializePendingSyncs(pendingsyncslen, pendingsyncsspace);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_PENDING_SYNCS,
					   pendingsyncsspace);

		/* Serialize reindex state. */
		reindexspace = shm_toc_allocate(pcxt->toc, reindexlen);
		SerializeReindexState(reindexlen, reindexspace);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_REINDEX_STATE, reindexspace);

		/* Serialize relmapper state. */
		relmapperspace = shm_toc_allocate(pcxt->toc, relmapperlen);
		SerializeRelationMap(relmapperlen, relmapperspace);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_RELMAPPER_STATE,
					   relmapperspace);

		/* Serialize uncommitted enum state. */
		uncommittedenumsspace = shm_toc_allocate(pcxt->toc,
												 uncommittedenumslen);
		SerializeUncommittedEnums(uncommittedenumsspace, uncommittedenumslen);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_UNCOMMITTEDENUMS,
					   uncommittedenumsspace);

		/* Serialize our ClientConnectionInfo. */
		clientconninfospace = shm_toc_allocate(pcxt->toc, clientconninfolen);
		SerializeClientConnectionInfo(clientconninfolen, clientconninfospace);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_CLIENTCONNINFO,
					   clientconninfospace);

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
							 mul_size(PARALLEL_ERROR_QUEUE_SIZE,
									  pcxt->nworkers));
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

		/*
		 * Serialize entrypoint information.  It's unsafe to pass function
		 * pointers across processes, as the function pointer may be different
		 * in each process in EXEC_BACKEND builds, so we always pass library
		 * and function name.  (We use library name "postgres" for functions
		 * in the core backend.)
		 */
		lnamelen = strlen(pcxt->library_name);
		entrypointstate = shm_toc_allocate(pcxt->toc, lnamelen +
										   strlen(pcxt->function_name) + 2);
		strcpy(entrypointstate, pcxt->library_name);
		strcpy(entrypointstate + lnamelen + 1, pcxt->function_name);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_ENTRYPOINT, entrypointstate);
	}

	/* Update nworkers_to_launch, in case we changed nworkers above. */
	pcxt->nworkers_to_launch = pcxt->nworkers;

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

	/* Wait for any old workers to exit. */
	if (pcxt->nworkers_launched > 0)
	{
		WaitForParallelWorkersToFinish(pcxt);
		WaitForParallelWorkersToExit(pcxt);
		pcxt->nworkers_launched = 0;
		if (pcxt->known_attached_workers)
		{
			pfree(pcxt->known_attached_workers);
			pcxt->known_attached_workers = NULL;
			pcxt->nknown_attached_workers = 0;
		}
	}

	/* Reset a few bits of fixed parallel state to a clean state. */
	fps = shm_toc_lookup(pcxt->toc, PARALLEL_KEY_FIXED, false);
	fps->last_xlog_end = 0;

	/* Recreate error queues (if they exist). */
	if (pcxt->nworkers > 0)
	{
		char	   *error_queue_space;
		int			i;

		error_queue_space =
			shm_toc_lookup(pcxt->toc, PARALLEL_KEY_ERROR_QUEUE, false);
		for (i = 0; i < pcxt->nworkers; ++i)
		{
			char	   *start;
			shm_mq	   *mq;

			start = error_queue_space + i * PARALLEL_ERROR_QUEUE_SIZE;
			mq = shm_mq_create(start, PARALLEL_ERROR_QUEUE_SIZE);
			shm_mq_set_receiver(mq, MyProc);
			pcxt->worker[i].error_mqh = shm_mq_attach(mq, pcxt->seg, NULL);
		}
	}
}

/*
 * Reinitialize parallel workers for a parallel context such that we could
 * launch a different number of workers.  This is required for cases where
 * we need to reuse the same DSM segment, but the number of workers can
 * vary from run-to-run.
 */
void
ReinitializeParallelWorkers(ParallelContext *pcxt, int nworkers_to_launch)
{
	/*
	 * The number of workers that need to be launched must be less than the
	 * number of workers with which the parallel context is initialized.  But
	 * the caller might not know that InitializeParallelDSM reduced nworkers,
	 * so just silently trim the request.
	 */
	pcxt->nworkers_to_launch = Min(pcxt->nworkers, nworkers_to_launch);
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
	if (pcxt->nworkers == 0 || pcxt->nworkers_to_launch == 0)
		return;

	/* We need to be a lock group leader. */
	BecomeLockGroupLeader();

	/* If we do have workers, we'd better have a DSM segment. */
	Assert(pcxt->seg != NULL);

	/* We might be running in a short-lived memory context. */
	oldcontext = MemoryContextSwitchTo(TopTransactionContext);

	/* Configure a worker. */
	memset(&worker, 0, sizeof(worker));
	snprintf(worker.bgw_name, BGW_MAXLEN, "parallel worker for PID %d",
			 MyProcPid);
	snprintf(worker.bgw_type, BGW_MAXLEN, "parallel worker");
	worker.bgw_flags =
		BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION
		| BGWORKER_CLASS_PARALLEL;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	sprintf(worker.bgw_library_name, "postgres");
	sprintf(worker.bgw_function_name, "ParallelWorkerMain");
	worker.bgw_main_arg = UInt32GetDatum(dsm_segment_handle(pcxt->seg));
	worker.bgw_notify_pid = MyProcPid;

	/*
	 * Start workers.
	 *
	 * The caller must be able to tolerate ending up with fewer workers than
	 * expected, so there is no need to throw an error here if registration
	 * fails.  It wouldn't help much anyway, because registering the worker in
	 * no way guarantees that it will start up and initialize successfully.
	 */
	for (i = 0; i < pcxt->nworkers_to_launch; ++i)
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
			shm_mq_detach(pcxt->worker[i].error_mqh);
			pcxt->worker[i].error_mqh = NULL;
		}
	}

	/*
	 * Now that nworkers_launched has taken its final value, we can initialize
	 * known_attached_workers.
	 */
	if (pcxt->nworkers_launched > 0)
	{
		pcxt->known_attached_workers =
			palloc0(sizeof(bool) * pcxt->nworkers_launched);
		pcxt->nknown_attached_workers = 0;
	}

	/* Restore previous memory context. */
	MemoryContextSwitchTo(oldcontext);
}

/*
 * Wait for all workers to attach to their error queues, and throw an error if
 * any worker fails to do this.
 *
 * Callers can assume that if this function returns successfully, then the
 * number of workers given by pcxt->nworkers_launched have initialized and
 * attached to their error queues.  Whether or not these workers are guaranteed
 * to still be running depends on what code the caller asked them to run;
 * this function does not guarantee that they have not exited.  However, it
 * does guarantee that any workers which exited must have done so cleanly and
 * after successfully performing the work with which they were tasked.
 *
 * If this function is not called, then some of the workers that were launched
 * may not have been started due to a fork() failure, or may have exited during
 * early startup prior to attaching to the error queue, so nworkers_launched
 * cannot be viewed as completely reliable.  It will never be less than the
 * number of workers which actually started, but it might be more.  Any workers
 * that failed to start will still be discovered by
 * WaitForParallelWorkersToFinish and an error will be thrown at that time,
 * provided that function is eventually reached.
 *
 * In general, the leader process should do as much work as possible before
 * calling this function.  fork() failures and other early-startup failures
 * are very uncommon, and having the leader sit idle when it could be doing
 * useful work is undesirable.  However, if the leader needs to wait for
 * all of its workers or for a specific worker, it may want to call this
 * function before doing so.  If not, it must make some other provision for
 * the failure-to-start case, lest it wait forever.  On the other hand, a
 * leader which never waits for a worker that might not be started yet, or
 * at least never does so prior to WaitForParallelWorkersToFinish(), need not
 * call this function at all.
 */
void
WaitForParallelWorkersToAttach(ParallelContext *pcxt)
{
	int			i;

	/* Skip this if we have no launched workers. */
	if (pcxt->nworkers_launched == 0)
		return;

	for (;;)
	{
		/*
		 * This will process any parallel messages that are pending and it may
		 * also throw an error propagated from a worker.
		 */
		CHECK_FOR_INTERRUPTS();

		for (i = 0; i < pcxt->nworkers_launched; ++i)
		{
			BgwHandleStatus status;
			shm_mq	   *mq;
			int			rc;
			pid_t		pid;

			if (pcxt->known_attached_workers[i])
				continue;

			/*
			 * If error_mqh is NULL, then the worker has already exited
			 * cleanly.
			 */
			if (pcxt->worker[i].error_mqh == NULL)
			{
				pcxt->known_attached_workers[i] = true;
				++pcxt->nknown_attached_workers;
				continue;
			}

			status = GetBackgroundWorkerPid(pcxt->worker[i].bgwhandle, &pid);
			if (status == BGWH_STARTED)
			{
				/* Has the worker attached to the error queue? */
				mq = shm_mq_get_queue(pcxt->worker[i].error_mqh);
				if (shm_mq_get_sender(mq) != NULL)
				{
					/* Yes, so it is known to be attached. */
					pcxt->known_attached_workers[i] = true;
					++pcxt->nknown_attached_workers;
				}
			}
			else if (status == BGWH_STOPPED)
			{
				/*
				 * If the worker stopped without attaching to the error queue,
				 * throw an error.
				 */
				mq = shm_mq_get_queue(pcxt->worker[i].error_mqh);
				if (shm_mq_get_sender(mq) == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							 errmsg("parallel worker failed to initialize"),
							 errhint("More details may be available in the server log.")));

				pcxt->known_attached_workers[i] = true;
				++pcxt->nknown_attached_workers;
			}
			else
			{
				/*
				 * Worker not yet started, so we must wait.  The postmaster
				 * will notify us if the worker's state changes.  Our latch
				 * might also get set for some other reason, but if so we'll
				 * just end up waiting for the same worker again.
				 */
				rc = WaitLatch(MyLatch,
							   WL_LATCH_SET | WL_EXIT_ON_PM_DEATH,
							   -1, WAIT_EVENT_BGWORKER_STARTUP);

				if (rc & WL_LATCH_SET)
					ResetLatch(MyLatch);
			}
		}

		/* If all workers are known to have started, we're done. */
		if (pcxt->nknown_attached_workers >= pcxt->nworkers_launched)
		{
			Assert(pcxt->nknown_attached_workers == pcxt->nworkers_launched);
			break;
		}
	}
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
		int			nfinished = 0;
		int			i;

		/*
		 * This will process any parallel messages that are pending, which may
		 * change the outcome of the loop that follows.  It may also throw an
		 * error propagated from a worker.
		 */
		CHECK_FOR_INTERRUPTS();

		for (i = 0; i < pcxt->nworkers_launched; ++i)
		{
			/*
			 * If error_mqh is NULL, then the worker has already exited
			 * cleanly.  If we have received a message through error_mqh from
			 * the worker, we know it started up cleanly, and therefore we're
			 * certain to be notified when it exits.
			 */
			if (pcxt->worker[i].error_mqh == NULL)
				++nfinished;
			else if (pcxt->known_attached_workers[i])
			{
				anyone_alive = true;
				break;
			}
		}

		if (!anyone_alive)
		{
			/* If all workers are known to have finished, we're done. */
			if (nfinished >= pcxt->nworkers_launched)
			{
				Assert(nfinished == pcxt->nworkers_launched);
				break;
			}

			/*
			 * We didn't detect any living workers, but not all workers are
			 * known to have exited cleanly.  Either not all workers have
			 * launched yet, or maybe some of them failed to start or
			 * terminated abnormally.
			 */
			for (i = 0; i < pcxt->nworkers_launched; ++i)
			{
				pid_t		pid;
				shm_mq	   *mq;

				/*
				 * If the worker is BGWH_NOT_YET_STARTED or BGWH_STARTED, we
				 * should just keep waiting.  If it is BGWH_STOPPED, then
				 * further investigation is needed.
				 */
				if (pcxt->worker[i].error_mqh == NULL ||
					pcxt->worker[i].bgwhandle == NULL ||
					GetBackgroundWorkerPid(pcxt->worker[i].bgwhandle,
										   &pid) != BGWH_STOPPED)
					continue;

				/*
				 * Check whether the worker ended up stopped without ever
				 * attaching to the error queue.  If so, the postmaster was
				 * unable to fork the worker or it exited without initializing
				 * properly.  We must throw an error, since the caller may
				 * have been expecting the worker to do some work before
				 * exiting.
				 */
				mq = shm_mq_get_queue(pcxt->worker[i].error_mqh);
				if (shm_mq_get_sender(mq) == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							 errmsg("parallel worker failed to initialize"),
							 errhint("More details may be available in the server log.")));

				/*
				 * The worker is stopped, but is attached to the error queue.
				 * Unless there's a bug somewhere, this will only happen when
				 * the worker writes messages and terminates after the
				 * CHECK_FOR_INTERRUPTS() near the top of this function and
				 * before the call to GetBackgroundWorkerPid().  In that case,
				 * or latch should have been set as well and the right things
				 * will happen on the next pass through the loop.
				 */
			}
		}

		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, -1,
						 WAIT_EVENT_PARALLEL_FINISH);
		ResetLatch(MyLatch);
	}

	if (pcxt->toc != NULL)
	{
		FixedParallelState *fps;

		fps = shm_toc_lookup(pcxt->toc, PARALLEL_KEY_FIXED, false);
		if (fps->last_xlog_end > XactLastRecEnd)
			XactLastRecEnd = fps->last_xlog_end;
	}
}

/*
 * Wait for all workers to exit.
 *
 * This function ensures that workers have been completely shutdown.  The
 * difference between WaitForParallelWorkersToFinish and this function is
 * that the former just ensures that last message sent by a worker backend is
 * received by the leader backend whereas this ensures the complete shutdown.
 */
static void
WaitForParallelWorkersToExit(ParallelContext *pcxt)
{
	int			i;

	/* Wait until the workers actually die. */
	for (i = 0; i < pcxt->nworkers_launched; ++i)
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
		for (i = 0; i < pcxt->nworkers_launched; ++i)
		{
			if (pcxt->worker[i].error_mqh != NULL)
			{
				TerminateBackgroundWorker(pcxt->worker[i].bgwhandle);

				shm_mq_detach(pcxt->worker[i].error_mqh);
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
	 * We can't finish transaction commit or abort until all of the workers
	 * have exited.  This means, in particular, that we can't respond to
	 * interrupts at this stage.
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
	pfree(pcxt->library_name);
	pfree(pcxt->function_name);
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
 *
 * Note: this is called within a signal handler!  All we can do is set
 * a flag that will cause the next CHECK_FOR_INTERRUPTS() to invoke
 * ProcessParallelMessages().
 */
void
HandleParallelMessageInterrupt(void)
{
	InterruptPending = true;
	ParallelMessagePending = true;
	SetLatch(MyLatch);
}

/*
 * Process any queued protocol messages received from parallel workers.
 */
void
ProcessParallelMessages(void)
{
	dlist_iter	iter;
	MemoryContext oldcontext;

	static MemoryContext hpm_context = NULL;

	/*
	 * This is invoked from ProcessInterrupts(), and since some of the
	 * functions it calls contain CHECK_FOR_INTERRUPTS(), there is a potential
	 * for recursive calls if more signals are received while this runs.  It's
	 * unclear that recursive entry would be safe, and it doesn't seem useful
	 * even if it is safe, so let's block interrupts until done.
	 */
	HOLD_INTERRUPTS();

	/*
	 * Moreover, CurrentMemoryContext might be pointing almost anywhere.  We
	 * don't want to risk leaking data into long-lived contexts, so let's do
	 * our work here in a private context that we can reset on each use.
	 */
	if (hpm_context == NULL)	/* first time through? */
		hpm_context = AllocSetContextCreate(TopMemoryContext,
											"ProcessParallelMessages",
											ALLOCSET_DEFAULT_SIZES);
	else
		MemoryContextReset(hpm_context);

	oldcontext = MemoryContextSwitchTo(hpm_context);

	/* OK to process messages.  Reset the flag saying there are more to do. */
	ParallelMessagePending = false;

	dlist_foreach(iter, &pcxt_list)
	{
		ParallelContext *pcxt;
		int			i;

		pcxt = dlist_container(ParallelContext, node, iter.cur);
		if (pcxt->worker == NULL)
			continue;

		for (i = 0; i < pcxt->nworkers_launched; ++i)
		{
			/*
			 * Read as many messages as we can from each worker, but stop when
			 * either (1) the worker's error queue goes away, which can happen
			 * if we receive a Terminate message from the worker; or (2) no
			 * more messages can be read from the worker without blocking.
			 */
			while (pcxt->worker[i].error_mqh != NULL)
			{
				shm_mq_result res;
				Size		nbytes;
				void	   *data;

				res = shm_mq_receive(pcxt->worker[i].error_mqh, &nbytes,
									 &data, true);
				if (res == SHM_MQ_WOULD_BLOCK)
					break;
				else if (res == SHM_MQ_SUCCESS)
				{
					StringInfoData msg;

					initStringInfo(&msg);
					appendBinaryStringInfo(&msg, data, nbytes);
					ProcessParallelMessage(pcxt, i, &msg);
					pfree(msg.data);
				}
				else
					ereport(ERROR,
							(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							 errmsg("lost connection to parallel worker")));
			}
		}
	}

	MemoryContextSwitchTo(oldcontext);

	/* Might as well clear the context on our way out */
	MemoryContextReset(hpm_context);

	RESUME_INTERRUPTS();
}

/*
 * Process a single protocol message received from a single parallel worker.
 */
static void
ProcessParallelMessage(ParallelContext *pcxt, int i, StringInfo msg)
{
	char		msgtype;

	if (pcxt->known_attached_workers != NULL &&
		!pcxt->known_attached_workers[i])
	{
		pcxt->known_attached_workers[i] = true;
		pcxt->nknown_attached_workers++;
	}

	msgtype = pq_getmsgbyte(msg);

	switch (msgtype)
	{
		case PqMsg_ErrorResponse:
		case PqMsg_NoticeResponse:
			{
				ErrorData	edata;
				ErrorContextCallback *save_error_context_stack;

				/* Parse ErrorResponse or NoticeResponse. */
				pq_parse_errornotice(msg, &edata);

				/* Death of a worker isn't enough justification for suicide. */
				edata.elevel = Min(edata.elevel, ERROR);

				/*
				 * If desired, add a context line to show that this is a
				 * message propagated from a parallel worker.  Otherwise, it
				 * can sometimes be confusing to understand what actually
				 * happened.  (We don't do this in DEBUG_PARALLEL_REGRESS mode
				 * because it causes test-result instability depending on
				 * whether a parallel worker is actually used or not.)
				 */
				if (debug_parallel_query != DEBUG_PARALLEL_REGRESS)
				{
					if (edata.context)
						edata.context = psprintf("%s\n%s", edata.context,
												 _("parallel worker"));
					else
						edata.context = pstrdup(_("parallel worker"));
				}

				/*
				 * Context beyond that should use the error context callbacks
				 * that were in effect when the ParallelContext was created,
				 * not the current ones.
				 */
				save_error_context_stack = error_context_stack;
				error_context_stack = pcxt->error_context_stack;

				/* Rethrow error or print notice. */
				ThrowErrorData(&edata);

				/* Not an error, so restore previous context stack. */
				error_context_stack = save_error_context_stack;

				break;
			}

		case PqMsg_NotificationResponse:
			{
				/* Propagate NotifyResponse. */
				int32		pid;
				const char *channel;
				const char *payload;

				pid = pq_getmsgint(msg, 4);
				channel = pq_getmsgrawstring(msg);
				payload = pq_getmsgrawstring(msg);
				pq_endmessage(msg);

				NotifyMyFrontEnd(channel, payload, pid);

				break;
			}

		case PqMsg_Progress:
			{
				/*
				 * Only incremental progress reporting is currently supported.
				 * However, it's possible to add more fields to the message to
				 * allow for handling of other backend progress APIs.
				 */
				int			index = pq_getmsgint(msg, 4);
				int64		incr = pq_getmsgint64(msg);

				pq_getmsgend(msg);

				pgstat_progress_incr_param(index, incr);

				break;
			}

		case PqMsg_Terminate:
			{
				shm_mq_detach(pcxt->worker[i].error_mqh);
				pcxt->worker[i].error_mqh = NULL;
				break;
			}

		default:
			{
				elog(ERROR, "unrecognized message type received from parallel worker: %c (message length %d bytes)",
					 msgtype, msg->len);
			}
	}
}

/*
 * End-of-subtransaction cleanup for parallel contexts.
 *
 * Here we remove only parallel contexts initiated within the current
 * subtransaction.
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
 *
 * We nuke all remaining parallel contexts.
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
void
ParallelWorkerMain(Datum main_arg)
{
	dsm_segment *seg;
	shm_toc    *toc;
	FixedParallelState *fps;
	char	   *error_queue_space;
	shm_mq	   *mq;
	shm_mq_handle *mqh;
	char	   *libraryspace;
	char	   *entrypointstate;
	char	   *library_name;
	char	   *function_name;
	parallel_worker_main_type entrypt;
	char	   *gucspace;
	char	   *combocidspace;
	char	   *tsnapspace;
	char	   *asnapspace;
	char	   *tstatespace;
	char	   *pendingsyncsspace;
	char	   *reindexspace;
	char	   *relmapperspace;
	char	   *uncommittedenumsspace;
	char	   *clientconninfospace;
	char	   *session_dsm_handle_space;
	Snapshot	tsnapshot;
	Snapshot	asnapshot;

	/* Set flag to indicate that we're initializing a parallel worker. */
	InitializingParallelWorker = true;

	/* Establish signal handlers. */
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	/* Determine and set our parallel worker number. */
	Assert(ParallelWorkerNumber == -1);
	memcpy(&ParallelWorkerNumber, MyBgworkerEntry->bgw_extra, sizeof(int));

	/* Set up a memory context to work in, just for cleanliness. */
	CurrentMemoryContext = AllocSetContextCreate(TopMemoryContext,
												 "Parallel worker",
												 ALLOCSET_DEFAULT_SIZES);

	/*
	 * Attach to the dynamic shared memory segment for the parallel query, and
	 * find its table of contents.
	 *
	 * Note: at this point, we have not created any ResourceOwner in this
	 * process.  This will result in our DSM mapping surviving until process
	 * exit, which is fine.  If there were a ResourceOwner, it would acquire
	 * ownership of the mapping, but we have no need for that.
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
	fps = shm_toc_lookup(toc, PARALLEL_KEY_FIXED, false);
	MyFixedParallelState = fps;

	/* Arrange to signal the leader if we exit. */
	ParallelLeaderPid = fps->parallel_leader_pid;
	ParallelLeaderProcNumber = fps->parallel_leader_proc_number;
	before_shmem_exit(ParallelWorkerShutdown, PointerGetDatum(seg));

	/*
	 * Now we can find and attach to the error queue provided for us.  That's
	 * good, because until we do that, any errors that happen here will not be
	 * reported back to the process that requested that this worker be
	 * launched.
	 */
	error_queue_space = shm_toc_lookup(toc, PARALLEL_KEY_ERROR_QUEUE, false);
	mq = (shm_mq *) (error_queue_space +
					 ParallelWorkerNumber * PARALLEL_ERROR_QUEUE_SIZE);
	shm_mq_set_sender(mq, MyProc);
	mqh = shm_mq_attach(mq, seg, NULL);
	pq_redirect_to_shm_mq(seg, mqh);
	pq_set_parallel_leader(fps->parallel_leader_pid,
						   fps->parallel_leader_proc_number);

	/*
	 * Hooray! Primary initialization is complete.  Now, we need to set up our
	 * backend-local state to match the original backend.
	 */

	/*
	 * Join locking group.  We must do this before anything that could try to
	 * acquire a heavyweight lock, because any heavyweight locks acquired to
	 * this point could block either directly against the parallel group
	 * leader or against some process which in turn waits for a lock that
	 * conflicts with the parallel group leader, causing an undetected
	 * deadlock.  (If we can't join the lock group, the leader has gone away,
	 * so just exit quietly.)
	 */
	if (!BecomeLockGroupMember(fps->parallel_leader_pgproc,
							   fps->parallel_leader_pid))
		return;

	/*
	 * Restore transaction and statement start-time timestamps.  This must
	 * happen before anything that would start a transaction, else asserts in
	 * xact.c will fire.
	 */
	SetParallelStartTimestamps(fps->xact_ts, fps->stmt_ts);

	/*
	 * Identify the entry point to be called.  In theory this could result in
	 * loading an additional library, though most likely the entry point is in
	 * the core backend or in a library we just loaded.
	 */
	entrypointstate = shm_toc_lookup(toc, PARALLEL_KEY_ENTRYPOINT, false);
	library_name = entrypointstate;
	function_name = entrypointstate + strlen(library_name) + 1;

	entrypt = LookupParallelWorkerFunction(library_name, function_name);

	/*
	 * Restore current session authorization and role id.  No verification
	 * happens here, we just blindly adopt the leader's state.  Note that this
	 * has to happen before InitPostgres, since InitializeSessionUserId will
	 * not set these variables.
	 */
	SetAuthenticatedUserId(fps->authenticated_user_id);
	SetSessionAuthorization(fps->session_user_id,
							fps->session_user_is_superuser);
	SetCurrentRoleId(fps->outer_user_id, fps->role_is_superuser);

	/*
	 * Restore database connection.  We skip connection authorization checks,
	 * reasoning that (a) the leader checked these things when it started, and
	 * (b) we do not want parallel mode to cause these failures, because that
	 * would make use of parallel query plans not transparent to applications.
	 */
	BackgroundWorkerInitializeConnectionByOid(fps->database_id,
											  fps->authenticated_user_id,
											  BGWORKER_BYPASS_ALLOWCONN |
											  BGWORKER_BYPASS_ROLELOGINCHECK);

	/*
	 * Set the client encoding to the database encoding, since that is what
	 * the leader will expect.  (We're cheating a bit by not calling
	 * PrepareClientEncoding first.  It's okay because this call will always
	 * result in installing a no-op conversion.  No error should be possible,
	 * but check anyway.)
	 */
	if (SetClientEncoding(GetDatabaseEncoding()) < 0)
		elog(ERROR, "SetClientEncoding(%d) failed", GetDatabaseEncoding());

	/*
	 * Load libraries that were loaded by original backend.  We want to do
	 * this before restoring GUCs, because the libraries might define custom
	 * variables.
	 */
	libraryspace = shm_toc_lookup(toc, PARALLEL_KEY_LIBRARY, false);
	StartTransactionCommand();
	RestoreLibraryState(libraryspace);
	CommitTransactionCommand();

	/* Crank up a transaction state appropriate to a parallel worker. */
	tstatespace = shm_toc_lookup(toc, PARALLEL_KEY_TRANSACTION_STATE, false);
	StartParallelWorkerTransaction(tstatespace);

	/*
	 * Restore state that affects catalog access.  Ideally we'd do this even
	 * before calling InitPostgres, but that has order-of-initialization
	 * problems, and also the relmapper would get confused during the
	 * CommitTransactionCommand call above.
	 */
	pendingsyncsspace = shm_toc_lookup(toc, PARALLEL_KEY_PENDING_SYNCS,
									   false);
	RestorePendingSyncs(pendingsyncsspace);
	relmapperspace = shm_toc_lookup(toc, PARALLEL_KEY_RELMAPPER_STATE, false);
	RestoreRelationMap(relmapperspace);
	reindexspace = shm_toc_lookup(toc, PARALLEL_KEY_REINDEX_STATE, false);
	RestoreReindexState(reindexspace);
	combocidspace = shm_toc_lookup(toc, PARALLEL_KEY_COMBO_CID, false);
	RestoreComboCIDState(combocidspace);

	/* Attach to the per-session DSM segment and contained objects. */
	session_dsm_handle_space =
		shm_toc_lookup(toc, PARALLEL_KEY_SESSION_DSM, false);
	AttachSession(*(dsm_handle *) session_dsm_handle_space);

	/*
	 * If the transaction isolation level is REPEATABLE READ or SERIALIZABLE,
	 * the leader has serialized the transaction snapshot and we must restore
	 * it. At lower isolation levels, there is no transaction-lifetime
	 * snapshot, but we need TransactionXmin to get set to a value which is
	 * less than or equal to the xmin of every snapshot that will be used by
	 * this worker. The easiest way to accomplish that is to install the
	 * active snapshot as the transaction snapshot. Code running in this
	 * parallel worker might take new snapshots via GetTransactionSnapshot()
	 * or GetLatestSnapshot(), but it shouldn't have any way of acquiring a
	 * snapshot older than the active snapshot.
	 */
	asnapspace = shm_toc_lookup(toc, PARALLEL_KEY_ACTIVE_SNAPSHOT, false);
	tsnapspace = shm_toc_lookup(toc, PARALLEL_KEY_TRANSACTION_SNAPSHOT, true);
	asnapshot = RestoreSnapshot(asnapspace);
	tsnapshot = tsnapspace ? RestoreSnapshot(tsnapspace) : asnapshot;
	RestoreTransactionSnapshot(tsnapshot,
							   fps->parallel_leader_pgproc);
	PushActiveSnapshot(asnapshot);

	/*
	 * We've changed which tuples we can see, and must therefore invalidate
	 * system caches.
	 */
	InvalidateSystemCaches();

	/*
	 * Restore GUC values from launching backend.  We can't do this earlier,
	 * because GUC check hooks that do catalog lookups need to see the same
	 * database state as the leader.  Also, the check hooks for
	 * session_authorization and role assume we already set the correct role
	 * OIDs.
	 */
	gucspace = shm_toc_lookup(toc, PARALLEL_KEY_GUC, false);
	RestoreGUCState(gucspace);

	/*
	 * Restore current user ID and security context.  No verification happens
	 * here, we just blindly adopt the leader's state.  We can't do this till
	 * after restoring GUCs, else we'll get complaints about restoring
	 * session_authorization and role.  (In effect, we're assuming that all
	 * the restored values are okay to set, even if we are now inside a
	 * restricted context.)
	 */
	SetUserIdAndSecContext(fps->current_user_id, fps->sec_context);

	/* Restore temp-namespace state to ensure search path matches leader's. */
	SetTempNamespaceState(fps->temp_namespace_id,
						  fps->temp_toast_namespace_id);

	/* Restore uncommitted enums. */
	uncommittedenumsspace = shm_toc_lookup(toc, PARALLEL_KEY_UNCOMMITTEDENUMS,
										   false);
	RestoreUncommittedEnums(uncommittedenumsspace);

	/* Restore the ClientConnectionInfo. */
	clientconninfospace = shm_toc_lookup(toc, PARALLEL_KEY_CLIENTCONNINFO,
										 false);
	RestoreClientConnectionInfo(clientconninfospace);

	/*
	 * Initialize SystemUser now that MyClientConnectionInfo is restored. Also
	 * ensure that auth_method is actually valid, aka authn_id is not NULL.
	 */
	if (MyClientConnectionInfo.authn_id)
		InitializeSystemUser(MyClientConnectionInfo.authn_id,
							 hba_authname(MyClientConnectionInfo.auth_method));

	/* Attach to the leader's serializable transaction, if SERIALIZABLE. */
	AttachSerializableXact(fps->serializable_xact_handle);

	/*
	 * We've initialized all of our state now; nothing should change
	 * hereafter.
	 */
	InitializingParallelWorker = false;
	EnterParallelMode();

	/*
	 * Time to do the real work: invoke the caller-supplied code.
	 */
	entrypt(seg, toc);

	/* Must exit parallel mode to pop active snapshot. */
	ExitParallelMode();

	/* Must pop active snapshot so snapmgr.c doesn't complain. */
	PopActiveSnapshot();

	/* Shut down the parallel-worker transaction. */
	EndParallelWorkerTransaction();

	/* Detach from the per-session DSM segment. */
	DetachSession();

	/* Report success. */
	pq_putmessage(PqMsg_Terminate, NULL, 0);
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

/*
 * Make sure the leader tries to read from our error queue one more time.
 * This guards against the case where we exit uncleanly without sending an
 * ErrorResponse to the leader, for example because some code calls proc_exit
 * directly.
 *
 * Also explicitly detach from dsm segment so that subsystems using
 * on_dsm_detach() have a chance to send stats before the stats subsystem is
 * shut down as part of a before_shmem_exit() hook.
 *
 * One might think this could instead be solved by carefully ordering the
 * attaching to dsm segments, so that the pgstats segments get detached from
 * later than the parallel query one. That turns out to not work because the
 * stats hash might need to grow which can cause new segments to be allocated,
 * which then will be detached from earlier.
 */
static void
ParallelWorkerShutdown(int code, Datum arg)
{
	SendProcSignal(ParallelLeaderPid,
				   PROCSIG_PARALLEL_MESSAGE,
				   ParallelLeaderProcNumber);

	dsm_detach((dsm_segment *) DatumGetPointer(arg));
}

/*
 * Look up (and possibly load) a parallel worker entry point function.
 *
 * For functions contained in the core code, we use library name "postgres"
 * and consult the InternalParallelWorkers array.  External functions are
 * looked up, and loaded if necessary, using load_external_function().
 *
 * The point of this is to pass function names as strings across process
 * boundaries.  We can't pass actual function addresses because of the
 * possibility that the function has been loaded at a different address
 * in a different process.  This is obviously a hazard for functions in
 * loadable libraries, but it can happen even for functions in the core code
 * on platforms using EXEC_BACKEND (e.g., Windows).
 *
 * At some point it might be worthwhile to get rid of InternalParallelWorkers[]
 * in favor of applying load_external_function() for core functions too;
 * but that raises portability issues that are not worth addressing now.
 */
static parallel_worker_main_type
LookupParallelWorkerFunction(const char *libraryname, const char *funcname)
{
	/*
	 * If the function is to be loaded from postgres itself, search the
	 * InternalParallelWorkers array.
	 */
	if (strcmp(libraryname, "postgres") == 0)
	{
		int			i;

		for (i = 0; i < lengthof(InternalParallelWorkers); i++)
		{
			if (strcmp(InternalParallelWorkers[i].fn_name, funcname) == 0)
				return InternalParallelWorkers[i].fn_addr;
		}

		/* We can only reach this by programming error. */
		elog(ERROR, "internal function \"%s\" not found", funcname);
	}

	/* Otherwise load from external library. */
	return (parallel_worker_main_type)
		load_external_function(libraryname, funcname, true, NULL);
}
