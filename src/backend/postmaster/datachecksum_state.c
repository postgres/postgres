/*-------------------------------------------------------------------------
 *
 * datachecksum_state.c
 *	  Background worker for enabling or disabling data checksums online as
 *	  well as functionality for manipulating data checksum state
 *
 * When enabling data checksums on a cluster at initdb time or when shut down
 * with pg_checksums, no extra process is required as each page is checksummed,
 * and verified, when accessed.  When enabling checksums on an already running
 * cluster, this worker will ensure that all pages are checksummed before
 * verification of the checksums is turned on. In the case of disabling
 * checksums, the state transition is performed only in the control file, no
 * changes are performed on the data pages.
 *
 * Checksums can be either enabled or disabled cluster-wide, with on/off being
 * the end state for data_checksums.
 *
 * 1. Enabling checksums
 * ---------------------
 * When enabling checksums in an online cluster, data_checksums will be set to
 * "inprogress-on" which signals that write operations MUST compute and write
 * the checksum on the data page, but during reading the checksum SHALL NOT be
 * verified. This ensures that all objects created during when checksums are
 * being enabled will have checksums set, but reads won't fail due to missing or
 * invalid checksums. Invalid checksums can be present in case the cluster had
 * checksums enabled, then disabled them and updated the page while they were
 * disabled.
 *
 * The DataChecksumsWorker will compile a list of all databases at the start,
 * any databases created concurrently will see the in-progress state and will
 * be checksummed automatically.  All databases from the original list MUST BE
 * successfully processed in order for data checksums to be enabled, the only
 * exception are databases which are dropped before having been processed.
 *
 * For each database, all relations which have storage are read and every data
 * page is marked dirty to force a write with the checksum. This will generate
 * a lot of WAL as the entire database is read and written.
 *
 * If the processing is interrupted by a cluster crash or restart, it needs to
 * be restarted from the beginning again as state isn't persisted.
 *
 * 2. Disabling checksums
 * ----------------------
 * When disabling checksums, data_checksums will be set to "inprogress-off"
 * which signals that checksums are written but no longer need to be verified.
 * This ensures that backends which have not yet transitioned to the
 * "inprogress-off" state will still see valid checksums on pages.
 *
 * 3. Synchronization and Correctness
 * ----------------------------------
 * The processes involved in enabling or disabling data checksums in an
 * online cluster must be properly synchronized with the normal backends
 * serving concurrent queries to ensure correctness. Correctness is defined
 * as the following:
 *
 *    - Backends SHALL NOT violate the data_checksums state they have agreed to
 *      by acknowledging the procsignalbarrier:  This means that all backends
 *      MUST calculate and write data checksums during all states except off;
 *      MUST validate checksums only in the 'on' state.
 *    - Data checksums SHALL NOT be considered enabled cluster-wide until all
 *      currently connected backends have state "on": This means that all
 *      backends must wait on the procsignalbarrier to be acknowledged by all
 *      before proceeding to validate data checksums.
 *
 * There are two steps of synchronization required for changing data_checksums
 * in an online cluster: (i) changing state in the active backends ("on",
 * "off", "inprogress-on" and "inprogress-off"), and (ii) ensuring no
 * incompatible objects and processes are left in a database when workers end.
 * The former deals with cluster-wide agreement on data checksum state and the
 * latter with ensuring that any concurrent activity cannot break the data
 * checksum contract during processing.
 *
 * Synchronizing the state change is done with procsignal barriers. Before
 * updating the data_checksums state in the control file, all other backends must absorb the
 * barrier.  Barrier absorption will happen during interrupt processing, which
 * means that connected backends will change state at different times.  If
 * waiting for a barrier is done during startup, for example during replay, it
 * is important to realize that any locks held by the startup process might
 * cause deadlocks if backends end up waiting for those locks while startup
 * is waiting for a procsignalbarrier.
 *
 * 3.1 When Enabling Data Checksums
 * --------------------------------
 * A process which fails to observe data checksums being enabled can induce two
 * types of errors: failing to write the checksum when modifying the page and
 * failing to validate the data checksum on the page when reading it.
 *
 * When processing starts all backends belong to one of the below sets, with
 * one of Bd and Bi being empty:
 *
 * Bg: Backend updating the global state and emitting the procsignalbarrier
 * Bd: Backends in "off" state
 * Bi: Backends in "inprogress-on" state
 *
 * If processing is started in an online cluster then all backends are in Bd.
 * If processing was halted by the cluster shutting down (due to a crash or
 * intentional restart), the controlfile state "inprogress-on" will be observed
 * on system startup and all backends will be placed in Bd. The controlfile
 * state will also be set to "off".
 *
 * Backends transition Bd -> Bi via a procsignalbarrier which is emitted by the
 * DataChecksumsWorkerLauncherMain.  When all backends have acknowledged the
 * barrier then Bd will be empty and the next phase can begin: calculating and
 * writing data checksums with DataChecksumsWorkers.  When the
 * DataChecksumsWorker processes have finished writing checksums on all pages,
 * data checksums are enabled cluster-wide via another procsignalbarrier.
 * There are four sets of backends where Bd shall be an empty set:
 *
 * Bg: Backend updating the global state and emitting the procsignalbarrier
 * Bd: Backends in "off" state
 * Be: Backends in "on" state
 * Bi: Backends in "inprogress-on" state
 *
 * Backends in Bi and Be will write checksums when modifying a page, but only
 * backends in Be will verify the checksum during reading. The Bg backend is
 * blocked waiting for all backends in Bi to process interrupts and move to
 * Be. Any backend starting while Bg is waiting on the procsignalbarrier will
 * observe the global state being "on" and will thus automatically belong to
 * Be.  Checksums are enabled cluster-wide when Bi is an empty set. Bi and Be
 * are compatible sets while still operating based on their local state as
 * both write data checksums.
 *
 * 3.2 When Disabling Data Checksums
 * ---------------------------------
 * A process which fails to observe that data checksums have been disabled
 * can induce two types of errors: writing the checksum when modifying the
 * page and validating a data checksum which is no longer correct due to
 * modifications to the page. The former is not an error per se as data
 * integrity is maintained, but it is wasteful.  The latter will cause errors
 * in user operations.  Assuming the following sets of backends:
 *
 * Bg: Backend updating the global state and emitting the procsignalbarrier
 * Bd: Backends in "off" state
 * Be: Backends in "on" state
 * Bo: Backends in "inprogress-off" state
 * Bi: Backends in "inprogress-on" state
 *
 * Backends transition from the Be state to Bd like so: Be -> Bo -> Bd.  From
 * all other states, the transition can be straight to Bd.
 *
 * The goal is to transition all backends to Bd making the others empty sets.
 * Backends in Bo write data checksums, but don't validate them, such that
 * backends still in Be can continue to validate pages until the barrier has
 * been absorbed such that they are in Bo. Once all backends are in Bo, the
 * barrier to transition to "off" can be raised and all backends can safely
 * stop writing data checksums as no backend is enforcing data checksum
 * validation any longer.
 *
 * 4. Future opportunities for optimizations
 * -----------------------------------------
 * Below are some potential optimizations and improvements which were brought
 * up during reviews of this feature, but which weren't implemented in the
 * initial version. These are ideas listed without any validation on their
 * feasibility or potential payoff. More discussion on (most of) these can be
 * found on the -hackers threads linked to in the commit message of this
 * feature.
 *
 *   * Launching datachecksumsworker for resuming operation from the startup
 *     process: Currently users have to restart processing manually after a
 *     restart since dynamic background worker cannot be started from the
 *     postmaster. Changing the startup process could make restarting the
 *     processing automatic on cluster restart.
 *   * Avoid dirtying the page when checksums already match: Iff the checksum
 *     on the page happens to already match we still dirty the page. It should
 *     be enough to only do the log_newpage_buffer() call in that case.
 *   * Teach pg_checksums to avoid checksummed pages when pg_checksums is used
 *     to enable checksums on a cluster which is in inprogress-on state and
 *     may have checksummed pages (make pg_checksums be able to resume an
 *     online operation). This should only be attempted for wal_level minimal.
 *   * Restartability (not necessarily with page granularity).
 *   * Avoid processing databases which were created during inprogress-on.
 *     Right now all databases are processed regardless to be safe.
 *   * Teach CREATE DATABASE to calculate checksums for databases created
 *     during inprogress-on with a template database which has yet to be
 *     processed.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/datachecksum_state.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/indexing.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "common/relpath.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/bgwriter.h"
#include "postmaster/datachecksum_state.h"
#include "storage/bufmgr.h"
#include "storage/checksum.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/lwlock.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "storage/subsystems.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/injection_point.h"
#include "utils/lsyscache.h"
#include "utils/ps_status.h"
#include "utils/syscache.h"
#include "utils/wait_event.h"

/*
 * Configuration of conditions which must match when absorbing a procsignal
 * barrier during data checksum enable/disable operations.  A single function
 * is used for absorbing all barriers, and the current and target states must
 * be defined as a from/to tuple in the checksum_barriers struct.
 */
typedef struct ChecksumBarrierCondition
{
	/* Current state of data checksums */
	int			from;
	/* Target state for data checksums */
	int			to;
} ChecksumBarrierCondition;

static const ChecksumBarrierCondition checksum_barriers[9] =
{
	/*
	 * Disabling checksums: If checksums are currently enabled, disabling must
	 * go through the 'inprogress-off' state.
	 */
	{PG_DATA_CHECKSUM_VERSION, PG_DATA_CHECKSUM_INPROGRESS_OFF},
	{PG_DATA_CHECKSUM_INPROGRESS_OFF, PG_DATA_CHECKSUM_OFF},

	/*
	 * If checksums are in the process of being enabled, but are not yet being
	 * verified, we can abort by going back to 'off' state.
	 */
	{PG_DATA_CHECKSUM_INPROGRESS_ON, PG_DATA_CHECKSUM_OFF},

	/*
	 * Enabling checksums must normally go through the 'inprogress-on' state.
	 */
	{PG_DATA_CHECKSUM_OFF, PG_DATA_CHECKSUM_INPROGRESS_ON},
	{PG_DATA_CHECKSUM_INPROGRESS_ON, PG_DATA_CHECKSUM_VERSION},

	/*
	 * If checksums are being disabled but all backends are still computing
	 * checksums, we can go straight back to 'on'
	 */
	{PG_DATA_CHECKSUM_INPROGRESS_OFF, PG_DATA_CHECKSUM_VERSION},

	/*
	 * If checksums are being enabled when launcher_exit is executed, state is
	 * set to off since we cannot reach on at that point.
	 */
	{PG_DATA_CHECKSUM_INPROGRESS_ON, PG_DATA_CHECKSUM_INPROGRESS_OFF},

	/*
	 * Transitions that can happen when a new request is made while another is
	 * currently being processed.
	 */
	{PG_DATA_CHECKSUM_INPROGRESS_OFF, PG_DATA_CHECKSUM_INPROGRESS_ON},
	{PG_DATA_CHECKSUM_OFF, PG_DATA_CHECKSUM_INPROGRESS_OFF},
};

/*
 * Signaling between backends calling pg_enable/disable_data_checksums, the
 * checksums launcher process, and the checksums worker process.
 *
 * This struct is protected by DataChecksumsWorkerLock
 */
typedef struct DataChecksumsStateStruct
{
	/*
	 * These are set by pg_{enable|disable}_data_checksums, to tell the
	 * launcher what the target state is.
	 */
	DataChecksumsWorkerOperation launch_operation;
	int			launch_cost_delay;
	int			launch_cost_limit;

	/*
	 * Is a launcher process currently running?  This is set by the main
	 * launcher process, after it has read the above launch_* parameters.
	 */
	bool		launcher_running;

	/*
	 * Is a worker process currently running?  This is set by the worker
	 * launcher when it starts waiting for a worker process to finish.
	 */
	int			worker_pid;

	/*
	 * These fields indicate the target state that the launcher is currently
	 * working towards. They can be different from the corresponding launch_*
	 * fields, if a new pg_enable/disable_data_checksums() call was made while
	 * the launcher/worker was already running.
	 *
	 * The below members are set when the launcher starts, and are only
	 * accessed read-only by the single worker. Thus, we can access these
	 * without a lock. If multiple workers, or dynamic cost parameters, are
	 * supported at some point then this would need to be revisited.
	 */
	DataChecksumsWorkerOperation operation;
	int			cost_delay;
	int			cost_limit;

	/*
	 * Signaling between the launcher and the worker process.
	 *
	 * As there is only a single worker, and the launcher won't read these
	 * until the worker exits, they can be accessed without the need for a
	 * lock. If multiple workers are supported then this will have to be
	 * revisited.
	 */

	/* result, set by worker before exiting */
	DataChecksumsWorkerResult success;

	/*
	 * Tells the worker process whether it should also process the shared
	 * catalogs
	 */
	bool		process_shared_catalogs;
} DataChecksumsStateStruct;

/* Shared memory segment for datachecksumsworker */
static DataChecksumsStateStruct *DataChecksumState;

typedef struct DataChecksumsWorkerDatabase
{
	Oid			dboid;
	char	   *dbname;
} DataChecksumsWorkerDatabase;

/* Flag set by the interrupt handler */
static volatile sig_atomic_t abort_requested = false;

/*
 * Have we set the DataChecksumsStateStruct->launcher_running flag?
 * If we have, we need to clear it before exiting!
 */
static volatile sig_atomic_t launcher_running = false;

/* Are we enabling data checksums, or disabling them? */
static DataChecksumsWorkerOperation operation;

/* Prototypes */
static void DataChecksumsShmemRequest(void *arg);
static bool DatabaseExists(Oid dboid);
static List *BuildDatabaseList(void);
static List *BuildRelationList(bool temp_relations, bool include_shared);
static void FreeDatabaseList(List *dblist);
static DataChecksumsWorkerResult ProcessDatabase(DataChecksumsWorkerDatabase *db);
static bool ProcessAllDatabases(void);
static bool ProcessSingleRelationFork(Relation reln, ForkNumber forkNum, BufferAccessStrategy strategy);
static void launcher_cancel_handler(SIGNAL_ARGS);
static void WaitForAllTransactionsToFinish(void);

const ShmemCallbacks DataChecksumsShmemCallbacks = {
	.request_fn = DataChecksumsShmemRequest,
};

#define CHECK_FOR_ABORT_REQUEST() \
	do {															\
		LWLockAcquire(DataChecksumsWorkerLock, LW_SHARED);			\
		if (DataChecksumState->launch_operation != operation)		\
			abort_requested = true;									\
		LWLockRelease(DataChecksumsWorkerLock);						\
	} while (0)


/*****************************************************************************
 * Functionality for manipulating the data checksum state in the cluster
 */

void
EmitAndWaitDataChecksumsBarrier(uint32 state)
{
	uint64		barrier;

	switch (state)
	{
		case PG_DATA_CHECKSUM_INPROGRESS_ON:
			barrier = EmitProcSignalBarrier(PROCSIGNAL_BARRIER_CHECKSUM_INPROGRESS_ON);
			WaitForProcSignalBarrier(barrier);
			break;

		case PG_DATA_CHECKSUM_INPROGRESS_OFF:
			barrier = EmitProcSignalBarrier(PROCSIGNAL_BARRIER_CHECKSUM_INPROGRESS_OFF);
			WaitForProcSignalBarrier(barrier);
			break;

		case PG_DATA_CHECKSUM_VERSION:
			barrier = EmitProcSignalBarrier(PROCSIGNAL_BARRIER_CHECKSUM_ON);
			WaitForProcSignalBarrier(barrier);
			break;

		case PG_DATA_CHECKSUM_OFF:
			barrier = EmitProcSignalBarrier(PROCSIGNAL_BARRIER_CHECKSUM_OFF);
			WaitForProcSignalBarrier(barrier);
			break;

		default:
			Assert(false);
	}
}

/*
 * AbsorbDataChecksumsBarrier
 *		Generic function for absorbing data checksum state changes
 *
 * All procsignalbarriers regarding data checksum state changes are absorbed
 * with this function.  The set of conditions required for the state change to
 * be accepted are listed in the checksum_barriers struct, target_state is
 * used to look up the relevant entry.
 */
bool
AbsorbDataChecksumsBarrier(ProcSignalBarrierType barrier)
{
	uint32		target_state;
	int			current = data_checksums;
	bool		found = false;

	/*
	 * Translate the barrier condition to the target state, doing it here
	 * instead of in the procsignal code saves the latter from knowing about
	 * checksum states.
	 */
	switch (barrier)
	{
		case PROCSIGNAL_BARRIER_CHECKSUM_INPROGRESS_ON:
			target_state = PG_DATA_CHECKSUM_INPROGRESS_ON;
			break;
		case PROCSIGNAL_BARRIER_CHECKSUM_ON:
			target_state = PG_DATA_CHECKSUM_VERSION;
			break;
		case PROCSIGNAL_BARRIER_CHECKSUM_INPROGRESS_OFF:
			target_state = PG_DATA_CHECKSUM_INPROGRESS_OFF;
			break;
		case PROCSIGNAL_BARRIER_CHECKSUM_OFF:
			target_state = PG_DATA_CHECKSUM_OFF;
			break;
		default:
			elog(ERROR, "incorrect barrier \"%i\" received", barrier);
	}

	/*
	 * If the target state matches the current state then the barrier has been
	 * repeated.
	 */
	if (current == target_state)
		return true;

	/*
	 * If the cluster is in recovery we skip the validation of current state
	 * since the replay is trusted.
	 */
	if (RecoveryInProgress())
	{
		SetLocalDataChecksumState(target_state);
		return true;
	}

	/*
	 * Find the barrier condition definition for the target state. Not finding
	 * a condition would be a grave programmer error as the states are a
	 * discrete set.
	 */
	for (int i = 0; i < lengthof(checksum_barriers) && !found; i++)
	{
		if (checksum_barriers[i].from == current && checksum_barriers[i].to == target_state)
			found = true;
	}

	/*
	 * If the relevant state criteria aren't satisfied, throw an error which
	 * will be caught by the procsignal machinery for a later retry.
	 */
	if (!found)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("incorrect data checksum state %i for target state %i",
					   current, target_state));

	SetLocalDataChecksumState(target_state);
	return true;
}


/*
 * Disables data checksums for the cluster, if applicable. Starts a background
 * worker which turns off the data checksums.
 */
Datum
disable_data_checksums(PG_FUNCTION_ARGS)
{
	PreventCommandDuringRecovery("pg_disable_data_checksums()");

	if (!superuser())
		ereport(ERROR,
				errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				errmsg("must be superuser to change data checksum state"));

	StartDataChecksumsWorkerLauncher(DISABLE_DATACHECKSUMS, 0, 0);
	PG_RETURN_VOID();
}

/*
 * Enables data checksums for the cluster, if applicable.  Supports vacuum-
 * like cost based throttling to limit system load. Starts a background worker
 * which updates data checksums on existing data.
 */
Datum
enable_data_checksums(PG_FUNCTION_ARGS)
{
	int			cost_delay = PG_GETARG_INT32(0);
	int			cost_limit = PG_GETARG_INT32(1);

	PreventCommandDuringRecovery("pg_enable_data_checksums()");

	if (!superuser())
		ereport(ERROR,
				errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				errmsg("must be superuser to change data checksum state"));

	if (cost_delay < 0)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("cost delay cannot be a negative value"));

	if (cost_limit <= 0)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("cost limit must be greater than zero"));

	StartDataChecksumsWorkerLauncher(ENABLE_DATACHECKSUMS, cost_delay, cost_limit);

	PG_RETURN_VOID();
}


/*****************************************************************************
 * Functionality for running the datachecksumsworker and associated launcher
 */

/*
 * StartDataChecksumsWorkerLauncher
 *		Main entry point for datachecksumsworker launcher process
 *
 * The main entrypoint for starting data checksums processing for enabling as
 * well as disabling.
 */
void
StartDataChecksumsWorkerLauncher(DataChecksumsWorkerOperation op,
								 int cost_delay,
								 int cost_limit)
{
	BackgroundWorker bgw;
	BackgroundWorkerHandle *bgw_handle;
	bool		running;

#ifdef USE_ASSERT_CHECKING
	/* The cost delay settings have no effect when disabling */
	if (op == DISABLE_DATACHECKSUMS)
		Assert(cost_delay == 0 && cost_limit == 0);
#endif

	INJECTION_POINT("datachecksumsworker-startup-delay", NULL);

	/* Store the desired state in shared memory */
	LWLockAcquire(DataChecksumsWorkerLock, LW_EXCLUSIVE);

	DataChecksumState->launch_operation = op;
	DataChecksumState->launch_cost_delay = cost_delay;
	DataChecksumState->launch_cost_limit = cost_limit;

	/* Is the launcher already running? If so, what is it doing? */
	running = DataChecksumState->launcher_running;

	LWLockRelease(DataChecksumsWorkerLock);

	/*
	 * Launch a new launcher process, if it's not running already.
	 *
	 * If the launcher is currently busy enabling the checksums, and we want
	 * them disabled (or vice versa), the launcher will notice that at latest
	 * when it's about to exit, and will loop back process the new request. So
	 * if the launcher is already running, we don't need to do anything more
	 * here to abort it.
	 *
	 * If you call pg_enable/disable_data_checksums() twice in a row, before
	 * the launcher has had a chance to start up, we still end up launching it
	 * twice.  That's OK, the second invocation will see that a launcher is
	 * already running and exit quickly.
	 */
	if (!running)
	{
		if ((op == ENABLE_DATACHECKSUMS && DataChecksumsOn()) ||
			(op == DISABLE_DATACHECKSUMS && DataChecksumsOff()))
		{
			ereport(LOG,
					errmsg("data checksums already in desired state, exiting"));
			return;
		}

		/*
		 * Prepare the BackgroundWorker and launch it.
		 */
		memset(&bgw, 0, sizeof(bgw));
		bgw.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
		bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
		snprintf(bgw.bgw_library_name, BGW_MAXLEN, "postgres");
		snprintf(bgw.bgw_function_name, BGW_MAXLEN, "DataChecksumsWorkerLauncherMain");
		snprintf(bgw.bgw_name, BGW_MAXLEN, "datachecksum launcher");
		snprintf(bgw.bgw_type, BGW_MAXLEN, "datachecksum launcher");
		bgw.bgw_restart_time = BGW_NEVER_RESTART;
		bgw.bgw_notify_pid = MyProcPid;
		bgw.bgw_main_arg = (Datum) 0;

		if (!RegisterDynamicBackgroundWorker(&bgw, &bgw_handle))
			ereport(ERROR,
					errcode(ERRCODE_INSUFFICIENT_RESOURCES),
					errmsg("failed to start background worker to process data checksums"));
	}
	else
	{
		ereport(LOG,
				errmsg("data checksum processing already running"));
	}
}

/*
 * ProcessSingleRelationFork
 *		Enable data checksums in a single relation/fork.
 *
 * Returns true if successful, and false if *aborted*. On error, an actual
 * error is raised in the lower levels.
 */
static bool
ProcessSingleRelationFork(Relation reln, ForkNumber forkNum, BufferAccessStrategy strategy)
{
	BlockNumber numblocks = RelationGetNumberOfBlocksInFork(reln, forkNum);
	char		activity[NAMEDATALEN * 2 + 128];
	char	   *relns;

	relns = get_namespace_name(RelationGetNamespace(reln));

	/* Report the current relation to pg_stat_activity */
	snprintf(activity, sizeof(activity) - 1, "processing: %s.%s (%s, %u blocks)",
			 (relns ? relns : ""), RelationGetRelationName(reln), forkNames[forkNum], numblocks);
	pgstat_report_activity(STATE_RUNNING, activity);
	pgstat_progress_update_param(PROGRESS_DATACHECKSUMS_BLOCKS_TOTAL, numblocks);
	if (relns)
		pfree(relns);

	/*
	 * We are looping over the blocks which existed at the time of process
	 * start, which is safe since new blocks are created with checksums set
	 * already due to the state being "inprogress-on".
	 */
	for (BlockNumber blknum = 0; blknum < numblocks; blknum++)
	{
		Buffer		buf = ReadBufferExtended(reln, forkNum, blknum, RBM_NORMAL, strategy);

		/* Need to get an exclusive lock to mark the buffer as dirty */
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

		/*
		 * Mark the buffer as dirty and force a full page write.  We have to
		 * re-write the page to WAL even if the checksum hasn't changed,
		 * because if there is a replica it might have a slightly different
		 * version of the page with an invalid checksum, caused by unlogged
		 * changes (e.g. hint bits) on the primary happening while checksums
		 * were off. This can happen if there was a valid checksum on the page
		 * at one point in the past, so only when checksums are first on, then
		 * off, and then turned on again.  TODO: investigate if this could be
		 * avoided if the checksum is calculated to be correct and wal_level
		 * is set to "minimal",
		 */
		START_CRIT_SECTION();
		MarkBufferDirty(buf);
		log_newpage_buffer(buf, false);
		END_CRIT_SECTION();

		UnlockReleaseBuffer(buf);

		/*
		 * This is the only place where we check if we are asked to abort, the
		 * abortion will bubble up from here.
		 */
		Assert(operation == ENABLE_DATACHECKSUMS);
		LWLockAcquire(DataChecksumsWorkerLock, LW_SHARED);
		if (DataChecksumState->launch_operation == DISABLE_DATACHECKSUMS)
			abort_requested = true;
		LWLockRelease(DataChecksumsWorkerLock);

		if (abort_requested)
			return false;

		/* update the block counter */
		pgstat_progress_update_param(PROGRESS_DATACHECKSUMS_BLOCKS_DONE,
									 (blknum + 1));

		/*
		 * Processing is re-using the vacuum cost delay for process
		 * throttling, hence why we call vacuum APIs here.
		 */
		vacuum_delay_point(false);
	}

	return true;
}

/*
 * ProcessSingleRelationByOid
 *		Process a single relation based on oid.
 *
 * Returns true if successful, and false if *aborted*. On error, an actual
 * error is raised in the lower levels.
 */
static bool
ProcessSingleRelationByOid(Oid relationId, BufferAccessStrategy strategy)
{
	Relation	rel;
	bool		aborted = false;

	StartTransactionCommand();

	rel = try_relation_open(relationId, AccessShareLock);
	if (rel == NULL)
	{
		/*
		 * Relation no longer exists. We don't consider this an error since
		 * there are no pages in it that need data checksums, and thus return
		 * true. The worker operates off a list of relations generated at the
		 * start of processing, so relations being dropped in the meantime is
		 * to be expected.
		 */
		CommitTransactionCommand();
		pgstat_report_activity(STATE_IDLE, NULL);
		return true;
	}
	RelationGetSmgr(rel);

	for (ForkNumber fnum = 0; fnum <= MAX_FORKNUM; fnum++)
	{
		if (smgrexists(rel->rd_smgr, fnum))
		{
			if (!ProcessSingleRelationFork(rel, fnum, strategy))
			{
				aborted = true;
				break;
			}
		}
	}
	relation_close(rel, AccessShareLock);

	CommitTransactionCommand();
	pgstat_report_activity(STATE_IDLE, NULL);

	return !aborted;
}

/*
 * ProcessDatabase
 *		Enable data checksums in a single database.
 *
 * We do this by launching a dynamic background worker into this database, and
 * waiting for it to finish.  We have to do this in a separate worker, since
 * each process can only be connected to one database during its lifetime.
 */
static DataChecksumsWorkerResult
ProcessDatabase(DataChecksumsWorkerDatabase *db)
{
	BackgroundWorker bgw;
	BackgroundWorkerHandle *bgw_handle;
	BgwHandleStatus status;
	pid_t		pid;
	char		activity[NAMEDATALEN + 64];

	LWLockAcquire(DataChecksumsWorkerLock, LW_EXCLUSIVE);
	DataChecksumState->success = DATACHECKSUMSWORKER_FAILED;
	LWLockRelease(DataChecksumsWorkerLock);

	memset(&bgw, 0, sizeof(bgw));
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	snprintf(bgw.bgw_library_name, BGW_MAXLEN, "postgres");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "%s", "DataChecksumsWorkerMain");
	snprintf(bgw.bgw_name, BGW_MAXLEN, "datachecksum worker");
	snprintf(bgw.bgw_type, BGW_MAXLEN, "datachecksum worker");
	bgw.bgw_restart_time = BGW_NEVER_RESTART;
	bgw.bgw_notify_pid = MyProcPid;
	bgw.bgw_main_arg = ObjectIdGetDatum(db->dboid);

	/*
	 * If there are no worker slots available, there is little we can do.  If
	 * we retry in a bit it's still unlikely that the user has managed to
	 * reconfigure in the meantime and we'd be run through retries fast.
	 */
	if (!RegisterDynamicBackgroundWorker(&bgw, &bgw_handle))
	{
		ereport(WARNING,
				errmsg("could not start background worker for enabling data checksums in database \"%s\"",
					   db->dbname),
				errhint("The \"%s\" setting might be too low.", "max_worker_processes"));
		return DATACHECKSUMSWORKER_FAILED;
	}

	status = WaitForBackgroundWorkerStartup(bgw_handle, &pid);
	if (status == BGWH_STOPPED)
	{
		/*
		 * If the worker managed to start, and stop, before we got to waiting
		 * for it we can see a STOPPED status here without it being a failure.
		 */
		LWLockAcquire(DataChecksumsWorkerLock, LW_SHARED);
		if (DataChecksumState->success == DATACHECKSUMSWORKER_SUCCESSFUL)
		{
			LWLockRelease(DataChecksumsWorkerLock);
			pgstat_report_activity(STATE_IDLE, NULL);
			LWLockAcquire(DataChecksumsWorkerLock, LW_EXCLUSIVE);
			DataChecksumState->worker_pid = InvalidPid;
			LWLockRelease(DataChecksumsWorkerLock);
			return DataChecksumState->success;
		}
		LWLockRelease(DataChecksumsWorkerLock);

		ereport(WARNING,
				errmsg("could not start background worker for enabling data checksums in database \"%s\"",
					   db->dbname),
				errhint("More details on the error might be found in the server log."));

		/*
		 * Heuristic to see if the database was dropped, and if it was we can
		 * treat it as not an error, else treat as fatal and error out.
		 */
		if (DatabaseExists(db->dboid))
			return DATACHECKSUMSWORKER_FAILED;
		else
			return DATACHECKSUMSWORKER_DROPDB;
	}

	/*
	 * If the postmaster crashed we cannot end up with a processed database so
	 * we have no alternative other than exiting. When enabling checksums we
	 * won't at this time have changed the data checksums state in pg_control
	 * to enabled so when the cluster comes back up processing will have to be
	 * restarted.
	 */
	if (status == BGWH_POSTMASTER_DIED)
		ereport(FATAL,
				errcode(ERRCODE_ADMIN_SHUTDOWN),
				errmsg("cannot enable data checksums without the postmaster process"),
				errhint("Restart the database and restart data checksum processing by calling pg_enable_data_checksums()."));

	Assert(status == BGWH_STARTED);
	ereport(LOG,
			errmsg("initiating data checksum processing in database \"%s\"",
				   db->dbname));

	/* Save the pid of the worker so we can signal it later */
	LWLockAcquire(DataChecksumsWorkerLock, LW_EXCLUSIVE);
	DataChecksumState->worker_pid = pid;
	LWLockRelease(DataChecksumsWorkerLock);

	snprintf(activity, sizeof(activity) - 1,
			 "Waiting for worker in database %s (pid %ld)", db->dbname, (long) pid);
	pgstat_report_activity(STATE_RUNNING, activity);

	status = WaitForBackgroundWorkerShutdown(bgw_handle);
	if (status == BGWH_POSTMASTER_DIED)
		ereport(FATAL,
				errcode(ERRCODE_ADMIN_SHUTDOWN),
				errmsg("postmaster exited during data checksum processing in \"%s\"",
					   db->dbname),
				errhint("Restart the database and restart data checksum processing by calling pg_enable_data_checksums()."));

	LWLockAcquire(DataChecksumsWorkerLock, LW_SHARED);
	if (DataChecksumState->success == DATACHECKSUMSWORKER_ABORTED)
		ereport(LOG,
				errmsg("data checksums processing was aborted in database \"%s\"",
					   db->dbname));
	LWLockRelease(DataChecksumsWorkerLock);

	pgstat_report_activity(STATE_IDLE, NULL);
	LWLockAcquire(DataChecksumsWorkerLock, LW_EXCLUSIVE);
	DataChecksumState->worker_pid = InvalidPid;
	LWLockRelease(DataChecksumsWorkerLock);

	return DataChecksumState->success;
}

/*
 * launcher_exit
 *
 * Internal routine for cleaning up state when a launcher process which has
 * performed checksum operations exits. A launcher process which is exiting due
 * to a duplicate started launcher does not need to perform any cleanup and
 * this function should not be called. Otherwise, we need to clean up the abort
 * flag to ensure that processing started again if it was previously aborted
 * (note: started again, *not* restarted from where it left off).
 */
static void
launcher_exit(int code, Datum arg)
{
	abort_requested = false;

	if (launcher_running)
	{
		LWLockAcquire(DataChecksumsWorkerLock, LW_EXCLUSIVE);
		if (DataChecksumState->worker_pid != InvalidPid)
		{
			ereport(LOG,
					errmsg("data checksums launcher exiting while worker is still running, signalling worker"));
			kill(DataChecksumState->worker_pid, SIGTERM);
		}
		LWLockRelease(DataChecksumsWorkerLock);
	}

	/*
	 * If the launcher is exiting before data checksums are enabled then set
	 * the state to off since processing cannot be resumed.
	 */
	if (DataChecksumsInProgressOn())
		SetDataChecksumsOff();

	LWLockAcquire(DataChecksumsWorkerLock, LW_EXCLUSIVE);
	launcher_running = false;
	DataChecksumState->launcher_running = false;
	LWLockRelease(DataChecksumsWorkerLock);
}

/*
 * launcher_cancel_handler
 *
 * Internal routine for reacting to SIGINT and flagging the worker to abort.
 * The worker won't be interrupted immediately but will check for abort flag
 * between each block in a relation.
 */
static void
launcher_cancel_handler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	abort_requested = true;

	/*
	 * There is no sleeping in the main loop, the flag will be checked
	 * periodically in ProcessSingleRelationFork. The worker does however
	 * sleep when waiting for concurrent transactions to end so we still need
	 * to set the latch.
	 */
	SetLatch(MyLatch);

	errno = save_errno;
}

/*
 * WaitForAllTransactionsToFinish
 *		Blocks awaiting all current transactions to finish
 *
 * Returns when all transactions which are active at the call of the function
 * have ended, or if the postmaster dies while waiting. If the postmaster dies
 * the abort flag will be set to indicate that the caller of this shouldn't
 * proceed.
 *
 * NB: this will return early, if aborted by SIGINT or if the target state
 * is changed while we're running.
 */
static void
WaitForAllTransactionsToFinish(void)
{
	TransactionId waitforxid;

	LWLockAcquire(XidGenLock, LW_SHARED);
	waitforxid = XidFromFullTransactionId(TransamVariables->nextXid);
	LWLockRelease(XidGenLock);

	while (TransactionIdPrecedes(GetOldestActiveTransactionId(false, true), waitforxid))
	{
		char		activity[64];
		int			rc;

		/* Oldest running xid is older than us, so wait */
		snprintf(activity,
				 sizeof(activity),
				 "Waiting for current transactions to finish (waiting for %u)",
				 waitforxid);
		pgstat_report_activity(STATE_RUNNING, activity);

		/* Retry every 3 seconds */
		ResetLatch(MyLatch);
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   3000,
					   WAIT_EVENT_CHECKSUM_ENABLE_STARTCONDITION);

		/*
		 * If the postmaster died we won't be able to enable checksums
		 * cluster-wide so abort and hope to continue when restarted.
		 */
		if (rc & WL_POSTMASTER_DEATH)
			ereport(FATAL,
					errcode(ERRCODE_ADMIN_SHUTDOWN),
					errmsg("postmaster exited during data checksums processing"),
					errhint("Data checksums processing must be restarted manually after cluster restart."));

		CHECK_FOR_INTERRUPTS();
		CHECK_FOR_ABORT_REQUEST();

		if (abort_requested)
			break;
	}

	pgstat_report_activity(STATE_IDLE, NULL);
	return;
}

/*
 * DataChecksumsWorkerLauncherMain
 *
 * Main function for launching dynamic background workers for processing data
 * checksums in databases. This function has the bgworker management, with
 * ProcessAllDatabases being responsible for looping over the databases and
 * initiating processing.
 */
void
DataChecksumsWorkerLauncherMain(Datum arg)
{

	ereport(DEBUG1,
			errmsg("background worker \"datachecksums launcher\" started"));

	pqsignal(SIGTERM, die);
	pqsignal(SIGINT, launcher_cancel_handler);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, PG_SIG_IGN);

	BackgroundWorkerUnblockSignals();

	MyBackendType = B_DATACHECKSUMSWORKER_LAUNCHER;
	init_ps_display(NULL);

	INJECTION_POINT("datachecksumsworker-launcher-delay", NULL);

	LWLockAcquire(DataChecksumsWorkerLock, LW_EXCLUSIVE);

	if (DataChecksumState->launcher_running)
	{
		ereport(LOG,
				errmsg("background worker \"datachecksums launcher\" already running, exiting"));
		/* Launcher was already running, let it finish */
		LWLockRelease(DataChecksumsWorkerLock);
		return;
	}

	on_shmem_exit(launcher_exit, 0);
	launcher_running = true;

	/* Initialize a connection to shared catalogs only */
	BackgroundWorkerInitializeConnectionByOid(InvalidOid, InvalidOid, 0);

	operation = DataChecksumState->launch_operation;
	DataChecksumState->launcher_running = true;
	DataChecksumState->operation = operation;
	DataChecksumState->cost_delay = DataChecksumState->launch_cost_delay;
	DataChecksumState->cost_limit = DataChecksumState->launch_cost_limit;
	LWLockRelease(DataChecksumsWorkerLock);

	/*
	 * The target state can change while we are busy enabling/disabling
	 * checksums, if the user calls pg_disable/enable_data_checksums() before
	 * we are finished with the previous request. In that case, we will loop
	 * back here, to process the new request.
	 */
again:

	pgstat_progress_start_command(PROGRESS_COMMAND_DATACHECKSUMS,
								  InvalidOid);

	if (operation == ENABLE_DATACHECKSUMS)
	{
		/*
		 * If we are asked to enable checksums in a cluster which already has
		 * checksums enabled, exit immediately as there is nothing more to do.
		 */
		if (DataChecksumsNeedVerify())
			goto done;

		ereport(LOG,
				errmsg("enabling data checksums requested, starting data checksum calculation"));

		/*
		 * Set the state to inprogress-on and wait on the procsignal barrier.
		 */
		pgstat_progress_update_param(PROGRESS_DATACHECKSUMS_PHASE,
									 PROGRESS_DATACHECKSUMS_PHASE_ENABLING);
		SetDataChecksumsOnInProgress();

		/*
		 * All backends are now in inprogress-on state and are writing data
		 * checksums.  Start processing all data at rest.
		 */
		if (!ProcessAllDatabases())
		{
			/*
			 * If the target state changed during processing then it's not a
			 * failure, so restart processing instead.
			 */
			LWLockAcquire(DataChecksumsWorkerLock, LW_EXCLUSIVE);
			if (DataChecksumState->launch_operation != operation)
			{
				LWLockRelease(DataChecksumsWorkerLock);
				goto done;
			}
			LWLockRelease(DataChecksumsWorkerLock);
			ereport(ERROR,
					errcode(ERRCODE_INSUFFICIENT_RESOURCES),
					errmsg("unable to enable data checksums in cluster"));
		}

		/*
		 * Data checksums have been set on all pages, set the state to on in
		 * order to instruct backends to validate checksums on reading.
		 */
		SetDataChecksumsOn();

		ereport(LOG,
				errmsg("data checksums are now enabled"));
	}
	else if (operation == DISABLE_DATACHECKSUMS)
	{
		ereport(LOG,
				errmsg("disabling data checksums requested"));

		pgstat_progress_update_param(PROGRESS_DATACHECKSUMS_PHASE,
									 PROGRESS_DATACHECKSUMS_PHASE_DISABLING);
		SetDataChecksumsOff();
		ereport(LOG,
				errmsg("data checksums are now disabled"));
	}
	else
		Assert(false);

done:

	/*
	 * This state will only be displayed for a fleeting moment, but for the
	 * sake of correctness it is still added before ending the command.
	 */
	pgstat_progress_update_param(PROGRESS_DATACHECKSUMS_PHASE,
								 PROGRESS_DATACHECKSUMS_PHASE_DONE);

	/*
	 * All done. But before we exit, check if the target state was changed
	 * while we were running. In that case we will have to start all over
	 * again.
	 */
	LWLockAcquire(DataChecksumsWorkerLock, LW_EXCLUSIVE);
	if (DataChecksumState->launch_operation != operation)
	{
		DataChecksumState->operation = DataChecksumState->launch_operation;
		operation = DataChecksumState->launch_operation;
		DataChecksumState->cost_delay = DataChecksumState->launch_cost_delay;
		DataChecksumState->cost_limit = DataChecksumState->launch_cost_limit;
		LWLockRelease(DataChecksumsWorkerLock);
		goto again;
	}

	/* Shut down progress reporting as we are done */
	pgstat_progress_end_command();

	launcher_running = false;
	DataChecksumState->launcher_running = false;
	LWLockRelease(DataChecksumsWorkerLock);
}

/*
 * ProcessAllDatabases
 *		Compute the list of all databases and process checksums in each
 *
 * This will generate a list of databases to process for enabling checksums.
 * If a database encounters a failure then processing will end immediately and
 * return an error.
 */
static bool
ProcessAllDatabases(void)
{
	List	   *DatabaseList;
	int			cumulative_total = 0;

	/* Set up so first run processes shared catalogs, not once in every db */
	LWLockAcquire(DataChecksumsWorkerLock, LW_EXCLUSIVE);
	DataChecksumState->process_shared_catalogs = true;
	LWLockRelease(DataChecksumsWorkerLock);

	/* Get a list of all databases to process */
	WaitForAllTransactionsToFinish();
	DatabaseList = BuildDatabaseList();

	/*
	 * Update progress reporting with the total number of databases we need to
	 * process.  This number should not be changed during processing, the
	 * columns for processed databases is instead increased such that it can
	 * be compared against the total.
	 */
	{
		const int	index[] = {
			PROGRESS_DATACHECKSUMS_DBS_TOTAL,
			PROGRESS_DATACHECKSUMS_DBS_DONE,
			PROGRESS_DATACHECKSUMS_RELS_TOTAL,
			PROGRESS_DATACHECKSUMS_RELS_DONE,
			PROGRESS_DATACHECKSUMS_BLOCKS_TOTAL,
			PROGRESS_DATACHECKSUMS_BLOCKS_DONE,
		};

		int64		vals[6];

		vals[0] = list_length(DatabaseList);
		vals[1] = 0;
		/* translated to NULL */
		vals[2] = -1;
		vals[3] = -1;
		vals[4] = -1;
		vals[5] = -1;

		pgstat_progress_update_multi_param(6, index, vals);
	}

	foreach_ptr(DataChecksumsWorkerDatabase, db, DatabaseList)
	{
		DataChecksumsWorkerResult result;

		result = ProcessDatabase(db);

#ifdef USE_INJECTION_POINTS
		/* Allow a test process to alter the result of the operation */
		if (IS_INJECTION_POINT_ATTACHED("datachecksumsworker-fail-db-result"))
		{
			result = DATACHECKSUMSWORKER_FAILED;
			INJECTION_POINT_CACHED("datachecksumsworker-fail-db-result",
								   db->dbname);
		}
#endif

		pgstat_progress_update_param(PROGRESS_DATACHECKSUMS_DBS_DONE,
									 ++cumulative_total);

		if (result == DATACHECKSUMSWORKER_FAILED)
		{
			/*
			 * Disable checksums on cluster, because we failed one of the
			 * databases and this is an all or nothing process.
			 */
			SetDataChecksumsOff();
			ereport(ERROR,
					errcode(ERRCODE_INSUFFICIENT_RESOURCES),
					errmsg("data checksums failed to get enabled in all databases, aborting"),
					errhint("The server log might have more information on the cause of the error."));
		}
		else if (result == DATACHECKSUMSWORKER_ABORTED || abort_requested)
		{
			/* Abort flag set, so exit the whole process */
			return false;
		}

		/*
		 * When one database has completed, it will have done shared catalogs
		 * so we don't have to process them again.
		 */
		LWLockAcquire(DataChecksumsWorkerLock, LW_EXCLUSIVE);
		DataChecksumState->process_shared_catalogs = false;
		LWLockRelease(DataChecksumsWorkerLock);
	}

	FreeDatabaseList(DatabaseList);

	pgstat_progress_update_param(PROGRESS_DATACHECKSUMS_PHASE,
								 PROGRESS_DATACHECKSUMS_PHASE_WAITING_BARRIER);
	return true;
}

/*
 * DataChecksumsShmemRequest
 *		Request datachecksumsworker-related shared memory
 */
static void
DataChecksumsShmemRequest(void *arg)
{
	ShmemRequestStruct(.name = "DataChecksumsWorker Data",
					   .size = sizeof(DataChecksumsStateStruct),
					   .ptr = (void **) &DataChecksumState,
		);
}

/*
 * DatabaseExists
 *
 * Scans the system catalog to check if a database with the given Oid exists
 * and returns true if it is found and valid, else false. Note, we cannot use
 * database_is_invalid_oid here as it will ERROR out, and we want to gracefully
 * handle errors.
 */
static bool
DatabaseExists(Oid dboid)
{
	Relation	rel;
	ScanKeyData skey;
	SysScanDesc scan;
	bool		found;
	HeapTuple	tuple;
	Form_pg_database pg_database_tuple;

	StartTransactionCommand();

	rel = table_open(DatabaseRelationId, AccessShareLock);
	ScanKeyInit(&skey,
				Anum_pg_database_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(dboid));
	scan = systable_beginscan(rel, DatabaseOidIndexId, true, SnapshotSelf,
							  1, &skey);
	tuple = systable_getnext(scan);
	found = HeapTupleIsValid(tuple);

	/* If the Oid exists, ensure that it's not partially dropped */
	if (found)
	{
		pg_database_tuple = (Form_pg_database) GETSTRUCT(tuple);
		if (database_is_invalid_form(pg_database_tuple))
			found = false;
	}

	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	CommitTransactionCommand();

	return found;
}

/*
 * BuildDatabaseList
 *		Compile a list of all currently available databases in the cluster
 *
 * This creates the list of databases for the datachecksumsworker workers to
 * add checksums to. If the caller wants to ensure that no concurrently
 * running CREATE DATABASE calls exist, this needs to be preceded by a call
 * to WaitForAllTransactionsToFinish().
 */
static List *
BuildDatabaseList(void)
{
	List	   *DatabaseList = NIL;
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	MemoryContext ctx = CurrentMemoryContext;
	MemoryContext oldctx;

	StartTransactionCommand();

	rel = table_open(DatabaseRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);

	while (HeapTupleIsValid(tup = heap_getnext(scan, ForwardScanDirection)))
	{
		Form_pg_database pgdb = (Form_pg_database) GETSTRUCT(tup);
		DataChecksumsWorkerDatabase *db;

		oldctx = MemoryContextSwitchTo(ctx);

		db = (DataChecksumsWorkerDatabase *) palloc0(sizeof(DataChecksumsWorkerDatabase));

		db->dboid = pgdb->oid;
		db->dbname = pstrdup(NameStr(pgdb->datname));

		DatabaseList = lappend(DatabaseList, db);

		MemoryContextSwitchTo(oldctx);
	}

	table_endscan(scan);
	table_close(rel, AccessShareLock);

	CommitTransactionCommand();

	return DatabaseList;
}

static void
FreeDatabaseList(List *dblist)
{
	if (!dblist)
		return;

	foreach_ptr(DataChecksumsWorkerDatabase, db, dblist)
	{
		if (db->dbname != NULL)
			pfree(db->dbname);
	}

	list_free_deep(dblist);
}

/*
 * BuildRelationList
 *		Compile a list of relations in the database
 *
 * Returns a list of OIDs for the request relation types. If temp_relations
 * is True then only temporary relations are returned. If temp_relations is
 * False then non-temporary relations which have data checksums are returned.
 * If include_shared is True then shared relations are included as well in a
 * non-temporary list. include_shared has no relevance when building a list of
 * temporary relations.
 */
static List *
BuildRelationList(bool temp_relations, bool include_shared)
{
	List	   *RelationList = NIL;
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	MemoryContext ctx = CurrentMemoryContext;
	MemoryContext oldctx;

	StartTransactionCommand();

	rel = table_open(RelationRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);

	while (HeapTupleIsValid(tup = heap_getnext(scan, ForwardScanDirection)))
	{
		Form_pg_class pgc = (Form_pg_class) GETSTRUCT(tup);

		/* Only include temporary relations when explicitly asked to */
		if (pgc->relpersistence == RELPERSISTENCE_TEMP)
		{
			if (!temp_relations)
				continue;
		}
		else
		{
			/*
			 * If we are only interested in temp relations then continue
			 * immediately as the current relation isn't a temp relation.
			 */
			if (temp_relations)
				continue;

			if (!RELKIND_HAS_STORAGE(pgc->relkind))
				continue;

			if (pgc->relisshared && !include_shared)
				continue;
		}

		oldctx = MemoryContextSwitchTo(ctx);
		RelationList = lappend_oid(RelationList, pgc->oid);
		MemoryContextSwitchTo(oldctx);
	}

	table_endscan(scan);
	table_close(rel, AccessShareLock);

	CommitTransactionCommand();

	return RelationList;
}

/*
 * DataChecksumsWorkerMain
 *
 * Main function for enabling checksums in a single database. This is the
 * function set as the bgw_function_name in the dynamic background worker
 * process initiated for each database by the worker launcher. After enabling
 * data checksums in each applicable relation in the database, it will wait for
 * all temporary relations that were present when the function started to
 * disappear before returning. This is required since we cannot rewrite
 * existing temporary relations with data checksums.
 */
void
DataChecksumsWorkerMain(Datum arg)
{
	Oid			dboid = DatumGetObjectId(arg);
	List	   *RelationList = NIL;
	List	   *InitialTempTableList = NIL;
	BufferAccessStrategy strategy;
	bool		aborted = false;
	int64		rels_done;
#ifdef USE_INJECTION_POINTS
	bool		retried = false;
#endif

	operation = ENABLE_DATACHECKSUMS;

	pqsignal(SIGTERM, die);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);

	BackgroundWorkerUnblockSignals();

	MyBackendType = B_DATACHECKSUMSWORKER_WORKER;
	init_ps_display(NULL);

	BackgroundWorkerInitializeConnectionByOid(dboid, InvalidOid,
											  BGWORKER_BYPASS_ALLOWCONN);

	/* worker will have a separate entry in pg_stat_progress_data_checksums */
	pgstat_progress_start_command(PROGRESS_COMMAND_DATACHECKSUMS,
								  InvalidOid);

	/*
	 * Get a list of all temp tables present as we start in this database. We
	 * need to wait until they are all gone until we are done, since we cannot
	 * access these relations and modify them.
	 */
	InitialTempTableList = BuildRelationList(true, false);

	/*
	 * Enable vacuum cost delay, if any.  While this process isn't doing any
	 * vacuuming, we are re-using the infrastructure that vacuum cost delay
	 * provides rather than inventing something bespoke. This is an internal
	 * implementation detail and care should be taken to avoid it bleeding
	 * through to the user to avoid confusion.
	 */
	VacuumCostDelay = DataChecksumState->cost_delay;
	VacuumCostLimit = DataChecksumState->cost_limit;
	VacuumCostActive = (VacuumCostDelay > 0);
	VacuumCostBalance = 0;
	VacuumCostPageHit = 0;
	VacuumCostPageMiss = 0;
	VacuumCostPageDirty = 0;

	/*
	 * Create and set the vacuum strategy as our buffer strategy.
	 */
	strategy = GetAccessStrategy(BAS_VACUUM);

	RelationList = BuildRelationList(false,
									 DataChecksumState->process_shared_catalogs);

	/* Update the total number of relations to be processed in this DB. */
	{
		const int	index[] = {
			PROGRESS_DATACHECKSUMS_RELS_TOTAL,
			PROGRESS_DATACHECKSUMS_RELS_DONE
		};

		int64		vals[2];

		vals[0] = list_length(RelationList);
		vals[1] = 0;

		pgstat_progress_update_multi_param(2, index, vals);
	}

	/* Process the relations */
	rels_done = 0;
	foreach_oid(reloid, RelationList)
	{
		bool		costs_updated = false;

		if (!ProcessSingleRelationByOid(reloid, strategy))
		{
			aborted = true;
			break;
		}

		pgstat_progress_update_param(PROGRESS_DATACHECKSUMS_RELS_DONE,
									 ++rels_done);
		CHECK_FOR_INTERRUPTS();
		CHECK_FOR_ABORT_REQUEST();

		if (abort_requested)
			break;

		/*
		 * Check if the cost settings changed during runtime and if so, update
		 * to reflect the new values and signal that the access strategy needs
		 * to be refreshed.
		 */
		LWLockAcquire(DataChecksumsWorkerLock, LW_EXCLUSIVE);
		if ((DataChecksumState->launch_cost_delay != DataChecksumState->cost_delay)
			|| (DataChecksumState->launch_cost_limit != DataChecksumState->cost_limit))
		{
			costs_updated = true;
			VacuumCostDelay = DataChecksumState->launch_cost_delay;
			VacuumCostLimit = DataChecksumState->launch_cost_limit;
			VacuumCostActive = (VacuumCostDelay > 0);

			DataChecksumState->cost_delay = DataChecksumState->launch_cost_delay;
			DataChecksumState->cost_limit = DataChecksumState->launch_cost_limit;
		}
		else
			costs_updated = false;
		LWLockRelease(DataChecksumsWorkerLock);

		if (costs_updated)
		{
			FreeAccessStrategy(strategy);
			strategy = GetAccessStrategy(BAS_VACUUM);
		}
	}

	list_free(RelationList);
	FreeAccessStrategy(strategy);

	if (aborted || abort_requested)
	{
		LWLockAcquire(DataChecksumsWorkerLock, LW_EXCLUSIVE);
		DataChecksumState->success = DATACHECKSUMSWORKER_ABORTED;
		LWLockRelease(DataChecksumsWorkerLock);
		ereport(DEBUG1,
				errmsg("data checksum processing aborted in database OID %u",
					   dboid));
		return;
	}

	/* The worker is about to wait for temporary tables to go away. */
	pgstat_progress_update_param(PROGRESS_DATACHECKSUMS_PHASE,
								 PROGRESS_DATACHECKSUMS_PHASE_WAITING_TEMPREL);

	/*
	 * Wait for all temp tables that existed when we started to go away. This
	 * is necessary since we cannot "reach" them to enable checksums. Any temp
	 * tables created after we started will already have checksums in them
	 * (due to the "inprogress-on" state), so no need to wait for those.
	 */
	for (;;)
	{
		List	   *CurrentTempTables;
		int			numleft;
		char		activity[64];

		CurrentTempTables = BuildRelationList(true, false);
		numleft = 0;
		foreach_oid(tmptbloid, InitialTempTableList)
		{
			if (list_member_oid(CurrentTempTables, tmptbloid))
				numleft++;
		}
		list_free(CurrentTempTables);

#ifdef USE_INJECTION_POINTS
		if (IS_INJECTION_POINT_ATTACHED("datachecksumsworker-fake-temptable-wait"))
		{
			/* Make sure to just cause one retry */
			if (!retried && numleft == 0)
			{
				numleft = 1;
				retried = true;

				INJECTION_POINT_CACHED("datachecksumsworker-fake-temptable-wait", NULL);
			}
		}
#endif

		if (numleft == 0)
			break;

		/*
		 * At least one temp table is left to wait for, indicate in pgstat
		 * activity and progress reporting.
		 */
		snprintf(activity,
				 sizeof(activity),
				 "Waiting for %d temp tables to be removed", numleft);
		pgstat_report_activity(STATE_RUNNING, activity);

		/* Retry every 3 seconds */
		ResetLatch(MyLatch);
		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 3000,
						 WAIT_EVENT_CHECKSUM_ENABLE_TEMPTABLE_WAIT);

		CHECK_FOR_INTERRUPTS();
		CHECK_FOR_ABORT_REQUEST();

		if (aborted || abort_requested)
		{
			LWLockAcquire(DataChecksumsWorkerLock, LW_EXCLUSIVE);
			DataChecksumState->success = DATACHECKSUMSWORKER_ABORTED;
			LWLockRelease(DataChecksumsWorkerLock);
			ereport(LOG,
					errmsg("data checksum processing aborted in database OID %u",
						   dboid));
			return;
		}
	}

	list_free(InitialTempTableList);

	/* worker done */
	pgstat_progress_end_command();

	LWLockAcquire(DataChecksumsWorkerLock, LW_EXCLUSIVE);
	DataChecksumState->success = DATACHECKSUMSWORKER_SUCCESSFUL;
	LWLockRelease(DataChecksumsWorkerLock);
}
