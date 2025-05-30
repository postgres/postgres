/*-------------------------------------------------------------------------
 *
 * autovacuum.c
 *
 * PostgreSQL Integrated Autovacuum Daemon
 *
 * The autovacuum system is structured in two different kinds of processes: the
 * autovacuum launcher and the autovacuum worker.  The launcher is an
 * always-running process, started by the postmaster when the autovacuum GUC
 * parameter is set.  The launcher schedules autovacuum workers to be started
 * when appropriate.  The workers are the processes which execute the actual
 * vacuuming; they connect to a database as determined in the launcher, and
 * once connected they examine the catalogs to select the tables to vacuum.
 *
 * The autovacuum launcher cannot start the worker processes by itself,
 * because doing so would cause robustness issues (namely, failure to shut
 * them down on exceptional conditions, and also, since the launcher is
 * connected to shared memory and is thus subject to corruption there, it is
 * not as robust as the postmaster).  So it leaves that task to the postmaster.
 *
 * There is an autovacuum shared memory area, where the launcher stores
 * information about the database it wants vacuumed.  When it wants a new
 * worker to start, it sets a flag in shared memory and sends a signal to the
 * postmaster.  Then postmaster knows nothing more than it must start a worker;
 * so it forks a new child, which turns into a worker.  This new process
 * connects to shared memory, and there it can inspect the information that the
 * launcher has set up.
 *
 * If the fork() call fails in the postmaster, it sets a flag in the shared
 * memory area, and sends a signal to the launcher.  The launcher, upon
 * noticing the flag, can try starting the worker again by resending the
 * signal.  Note that the failure can only be transient (fork failure due to
 * high load, memory pressure, too many processes, etc); more permanent
 * problems, like failure to connect to a database, are detected later in the
 * worker and dealt with just by having the worker exit normally.  The launcher
 * will launch a new worker again later, per schedule.
 *
 * When the worker is done vacuuming it sends SIGUSR2 to the launcher.  The
 * launcher then wakes up and is able to launch another worker, if the schedule
 * is so tight that a new worker is needed immediately.  At this time the
 * launcher can also balance the settings for the various remaining workers'
 * cost-based vacuum delay feature.
 *
 * Note that there can be more than one worker in a database concurrently.
 * They will store the table they are currently vacuuming in shared memory, so
 * that other workers avoid being blocked waiting for the vacuum lock for that
 * table.  They will also fetch the last time the table was vacuumed from
 * pgstats just before vacuuming each table, to avoid vacuuming a table that
 * was just finished being vacuumed by another worker and thus is no longer
 * noted in shared memory.  However, there is a small window (due to not yet
 * holding the relation lock) during which a worker may choose a table that was
 * already vacuumed; this is a bug in the current design.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/autovacuum.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <sys/time.h>
#include <unistd.h>

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/reloptions.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
#include "commands/dbcommands.h"
#include "commands/vacuum.h"
#include "common/int.h"
#include "lib/ilist.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "postmaster/interrupt.h"
#include "postmaster/postmaster.h"
#include "storage/aio_subsys.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/smgr.h"
#include "tcop/tcopprot.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/guc_hooks.h"
#include "utils/injection_point.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"


/*
 * GUC parameters
 */
bool		autovacuum_start_daemon = false;
int			autovacuum_worker_slots;
int			autovacuum_max_workers;
int			autovacuum_work_mem = -1;
int			autovacuum_naptime;
int			autovacuum_vac_thresh;
int			autovacuum_vac_max_thresh;
double		autovacuum_vac_scale;
int			autovacuum_vac_ins_thresh;
double		autovacuum_vac_ins_scale;
int			autovacuum_anl_thresh;
double		autovacuum_anl_scale;
int			autovacuum_freeze_max_age;
int			autovacuum_multixact_freeze_max_age;

double		autovacuum_vac_cost_delay;
int			autovacuum_vac_cost_limit;

int			Log_autovacuum_min_duration = 600000;

/* the minimum allowed time between two awakenings of the launcher */
#define MIN_AUTOVAC_SLEEPTIME 100.0 /* milliseconds */
#define MAX_AUTOVAC_SLEEPTIME 300	/* seconds */

/*
 * Variables to save the cost-related storage parameters for the current
 * relation being vacuumed by this autovacuum worker. Using these, we can
 * ensure we don't overwrite the values of vacuum_cost_delay and
 * vacuum_cost_limit after reloading the configuration file. They are
 * initialized to "invalid" values to indicate that no cost-related storage
 * parameters were specified and will be set in do_autovacuum() after checking
 * the storage parameters in table_recheck_autovac().
 */
static double av_storage_param_cost_delay = -1;
static int	av_storage_param_cost_limit = -1;

/* Flags set by signal handlers */
static volatile sig_atomic_t got_SIGUSR2 = false;

/* Comparison points for determining whether freeze_max_age is exceeded */
static TransactionId recentXid;
static MultiXactId recentMulti;

/* Default freeze ages to use for autovacuum (varies by database) */
static int	default_freeze_min_age;
static int	default_freeze_table_age;
static int	default_multixact_freeze_min_age;
static int	default_multixact_freeze_table_age;

/* Memory context for long-lived data */
static MemoryContext AutovacMemCxt;

/* struct to keep track of databases in launcher */
typedef struct avl_dbase
{
	Oid			adl_datid;		/* hash key -- must be first */
	TimestampTz adl_next_worker;
	int			adl_score;
	dlist_node	adl_node;
} avl_dbase;

/* struct to keep track of databases in worker */
typedef struct avw_dbase
{
	Oid			adw_datid;
	char	   *adw_name;
	TransactionId adw_frozenxid;
	MultiXactId adw_minmulti;
	PgStat_StatDBEntry *adw_entry;
} avw_dbase;

/* struct to keep track of tables to vacuum and/or analyze, in 1st pass */
typedef struct av_relation
{
	Oid			ar_toastrelid;	/* hash key - must be first */
	Oid			ar_relid;
	bool		ar_hasrelopts;
	AutoVacOpts ar_reloptions;	/* copy of AutoVacOpts from the main table's
								 * reloptions, or NULL if none */
} av_relation;

/* struct to keep track of tables to vacuum and/or analyze, after rechecking */
typedef struct autovac_table
{
	Oid			at_relid;
	VacuumParams at_params;
	double		at_storage_param_vac_cost_delay;
	int			at_storage_param_vac_cost_limit;
	bool		at_dobalance;
	bool		at_sharedrel;
	char	   *at_relname;
	char	   *at_nspname;
	char	   *at_datname;
} autovac_table;

/*-------------
 * This struct holds information about a single worker's whereabouts.  We keep
 * an array of these in shared memory, sized according to
 * autovacuum_worker_slots.
 *
 * wi_links		entry into free list or running list
 * wi_dboid		OID of the database this worker is supposed to work on
 * wi_tableoid	OID of the table currently being vacuumed, if any
 * wi_sharedrel flag indicating whether table is marked relisshared
 * wi_proc		pointer to PGPROC of the running worker, NULL if not started
 * wi_launchtime Time at which this worker was launched
 * wi_dobalance Whether this worker should be included in balance calculations
 *
 * All fields are protected by AutovacuumLock, except for wi_tableoid and
 * wi_sharedrel which are protected by AutovacuumScheduleLock (note these
 * two fields are read-only for everyone except that worker itself).
 *-------------
 */
typedef struct WorkerInfoData
{
	dlist_node	wi_links;
	Oid			wi_dboid;
	Oid			wi_tableoid;
	PGPROC	   *wi_proc;
	TimestampTz wi_launchtime;
	pg_atomic_flag wi_dobalance;
	bool		wi_sharedrel;
} WorkerInfoData;

typedef struct WorkerInfoData *WorkerInfo;

/*
 * Possible signals received by the launcher from remote processes.  These are
 * stored atomically in shared memory so that other processes can set them
 * without locking.
 */
typedef enum
{
	AutoVacForkFailed,			/* failed trying to start a worker */
	AutoVacRebalance,			/* rebalance the cost limits */
}			AutoVacuumSignal;

#define AutoVacNumSignals (AutoVacRebalance + 1)

/*
 * Autovacuum workitem array, stored in AutoVacuumShmem->av_workItems.  This
 * list is mostly protected by AutovacuumLock, except that if an item is
 * marked 'active' other processes must not modify the work-identifying
 * members.
 */
typedef struct AutoVacuumWorkItem
{
	AutoVacuumWorkItemType avw_type;
	bool		avw_used;		/* below data is valid */
	bool		avw_active;		/* being processed */
	Oid			avw_database;
	Oid			avw_relation;
	BlockNumber avw_blockNumber;
} AutoVacuumWorkItem;

#define NUM_WORKITEMS	256

/*-------------
 * The main autovacuum shmem struct.  On shared memory we store this main
 * struct and the array of WorkerInfo structs.  This struct keeps:
 *
 * av_signal		set by other processes to indicate various conditions
 * av_launcherpid	the PID of the autovacuum launcher
 * av_freeWorkers	the WorkerInfo freelist
 * av_runningWorkers the WorkerInfo non-free queue
 * av_startingWorker pointer to WorkerInfo currently being started (cleared by
 *					the worker itself as soon as it's up and running)
 * av_workItems		work item array
 * av_nworkersForBalance the number of autovacuum workers to use when
 * 					calculating the per worker cost limit
 *
 * This struct is protected by AutovacuumLock, except for av_signal and parts
 * of the worker list (see above).
 *-------------
 */
typedef struct
{
	sig_atomic_t av_signal[AutoVacNumSignals];
	pid_t		av_launcherpid;
	dclist_head av_freeWorkers;
	dlist_head	av_runningWorkers;
	WorkerInfo	av_startingWorker;
	AutoVacuumWorkItem av_workItems[NUM_WORKITEMS];
	pg_atomic_uint32 av_nworkersForBalance;
} AutoVacuumShmemStruct;

static AutoVacuumShmemStruct *AutoVacuumShmem;

/*
 * the database list (of avl_dbase elements) in the launcher, and the context
 * that contains it
 */
static dlist_head DatabaseList = DLIST_STATIC_INIT(DatabaseList);
static MemoryContext DatabaseListCxt = NULL;

/* Pointer to my own WorkerInfo, valid on each worker */
static WorkerInfo MyWorkerInfo = NULL;

/* PID of launcher, valid only in worker while shutting down */
int			AutovacuumLauncherPid = 0;

static Oid	do_start_worker(void);
static void ProcessAutoVacLauncherInterrupts(void);
pg_noreturn static void AutoVacLauncherShutdown(void);
static void launcher_determine_sleep(bool canlaunch, bool recursing,
									 struct timeval *nap);
static void launch_worker(TimestampTz now);
static List *get_database_list(void);
static void rebuild_database_list(Oid newdb);
static int	db_comparator(const void *a, const void *b);
static void autovac_recalculate_workers_for_balance(void);

static void do_autovacuum(void);
static void FreeWorkerInfo(int code, Datum arg);

static autovac_table *table_recheck_autovac(Oid relid, HTAB *table_toast_map,
											TupleDesc pg_class_desc,
											int effective_multixact_freeze_max_age);
static void recheck_relation_needs_vacanalyze(Oid relid, AutoVacOpts *avopts,
											  Form_pg_class classForm,
											  int effective_multixact_freeze_max_age,
											  bool *dovacuum, bool *doanalyze, bool *wraparound);
static void relation_needs_vacanalyze(Oid relid, AutoVacOpts *relopts,
									  Form_pg_class classForm,
									  PgStat_StatTabEntry *tabentry,
									  int effective_multixact_freeze_max_age,
									  bool *dovacuum, bool *doanalyze, bool *wraparound);

static void autovacuum_do_vac_analyze(autovac_table *tab,
									  BufferAccessStrategy bstrategy);
static AutoVacOpts *extract_autovac_opts(HeapTuple tup,
										 TupleDesc pg_class_desc);
static void perform_work_item(AutoVacuumWorkItem *workitem);
static void autovac_report_activity(autovac_table *tab);
static void autovac_report_workitem(AutoVacuumWorkItem *workitem,
									const char *nspname, const char *relname);
static void avl_sigusr2_handler(SIGNAL_ARGS);
static bool av_worker_available(void);
static void check_av_worker_gucs(void);



/********************************************************************
 *					  AUTOVACUUM LAUNCHER CODE
 ********************************************************************/

/*
 * Main entry point for the autovacuum launcher process.
 */
void
AutoVacLauncherMain(const void *startup_data, size_t startup_data_len)
{
	sigjmp_buf	local_sigjmp_buf;

	Assert(startup_data_len == 0);

	/* Release postmaster's working memory context */
	if (PostmasterContext)
	{
		MemoryContextDelete(PostmasterContext);
		PostmasterContext = NULL;
	}

	MyBackendType = B_AUTOVAC_LAUNCHER;
	init_ps_display(NULL);

	ereport(DEBUG1,
			(errmsg_internal("autovacuum launcher started")));

	if (PostAuthDelay)
		pg_usleep(PostAuthDelay * 1000000L);

	Assert(GetProcessingMode() == InitProcessing);

	/*
	 * Set up signal handlers.  We operate on databases much like a regular
	 * backend, so we use the same signal handling.  See equivalent code in
	 * tcop/postgres.c.
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGINT, StatementCancelHandler);
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
	/* SIGQUIT handler was already set up by InitPostmasterChild */

	InitializeTimeouts();		/* establishes SIGALRM handler */

	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, avl_sigusr2_handler);
	pqsignal(SIGFPE, FloatExceptionHandler);
	pqsignal(SIGCHLD, SIG_DFL);

	/*
	 * Create a per-backend PGPROC struct in shared memory.  We must do this
	 * before we can use LWLocks or access any shared memory.
	 */
	InitProcess();

	/* Early initialization */
	BaseInit();

	InitPostgres(NULL, InvalidOid, NULL, InvalidOid, 0, NULL);

	SetProcessingMode(NormalProcessing);

	/*
	 * Create a memory context that we will do all our work in.  We do this so
	 * that we can reset the context during error recovery and thereby avoid
	 * possible memory leaks.
	 */
	AutovacMemCxt = AllocSetContextCreate(TopMemoryContext,
										  "Autovacuum Launcher",
										  ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(AutovacMemCxt);

	/*
	 * If an exception is encountered, processing resumes here.
	 *
	 * This code is a stripped down version of PostgresMain error recovery.
	 *
	 * Note that we use sigsetjmp(..., 1), so that the prevailing signal mask
	 * (to wit, BlockSig) will be restored when longjmp'ing to here.  Thus,
	 * signals other than SIGQUIT will be blocked until we complete error
	 * recovery.  It might seem that this policy makes the HOLD_INTERRUPTS()
	 * call redundant, but it is not since InterruptPending might be set
	 * already.
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* since not using PG_TRY, must reset error stack by hand */
		error_context_stack = NULL;

		/* Prevents interrupts while cleaning up */
		HOLD_INTERRUPTS();

		/* Forget any pending QueryCancel or timeout request */
		disable_all_timeouts(false);
		QueryCancelPending = false; /* second to avoid race condition */

		/* Report the error to the server log */
		EmitErrorReport();

		/* Abort the current transaction in order to recover */
		AbortCurrentTransaction();

		/*
		 * Release any other resources, for the case where we were not in a
		 * transaction.
		 */
		LWLockReleaseAll();
		pgstat_report_wait_end();
		pgaio_error_cleanup();
		UnlockBuffers();
		/* this is probably dead code, but let's be safe: */
		if (AuxProcessResourceOwner)
			ReleaseAuxProcessResources(false);
		AtEOXact_Buffers(false);
		AtEOXact_SMgr();
		AtEOXact_Files(false);
		AtEOXact_HashTables(false);

		/*
		 * Now return to normal top-level context and clear ErrorContext for
		 * next time.
		 */
		MemoryContextSwitchTo(AutovacMemCxt);
		FlushErrorState();

		/* Flush any leaked data in the top-level context */
		MemoryContextReset(AutovacMemCxt);

		/* don't leave dangling pointers to freed memory */
		DatabaseListCxt = NULL;
		dlist_init(&DatabaseList);

		/* Now we can allow interrupts again */
		RESUME_INTERRUPTS();

		/* if in shutdown mode, no need for anything further; just go away */
		if (ShutdownRequestPending)
			AutoVacLauncherShutdown();

		/*
		 * Sleep at least 1 second after any error.  We don't want to be
		 * filling the error logs as fast as we can.
		 */
		pg_usleep(1000000L);
	}

	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	/* must unblock signals before calling rebuild_database_list */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/*
	 * Set always-secure search path.  Launcher doesn't connect to a database,
	 * so this has no effect.
	 */
	SetConfigOption("search_path", "", PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * Force zero_damaged_pages OFF in the autovac process, even if it is set
	 * in postgresql.conf.  We don't really want such a dangerous option being
	 * applied non-interactively.
	 */
	SetConfigOption("zero_damaged_pages", "false", PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * Force settable timeouts off to avoid letting these settings prevent
	 * regular maintenance from being executed.
	 */
	SetConfigOption("statement_timeout", "0", PGC_SUSET, PGC_S_OVERRIDE);
	SetConfigOption("transaction_timeout", "0", PGC_SUSET, PGC_S_OVERRIDE);
	SetConfigOption("lock_timeout", "0", PGC_SUSET, PGC_S_OVERRIDE);
	SetConfigOption("idle_in_transaction_session_timeout", "0",
					PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * Force default_transaction_isolation to READ COMMITTED.  We don't want
	 * to pay the overhead of serializable mode, nor add any risk of causing
	 * deadlocks or delaying other transactions.
	 */
	SetConfigOption("default_transaction_isolation", "read committed",
					PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * Even when system is configured to use a different fetch consistency,
	 * for autovac we always want fresh stats.
	 */
	SetConfigOption("stats_fetch_consistency", "none", PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * In emergency mode, just start a worker (unless shutdown was requested)
	 * and go away.
	 */
	if (!AutoVacuumingActive())
	{
		if (!ShutdownRequestPending)
			do_start_worker();
		proc_exit(0);			/* done */
	}

	AutoVacuumShmem->av_launcherpid = MyProcPid;

	/*
	 * Create the initial database list.  The invariant we want this list to
	 * keep is that it's ordered by decreasing next_time.  As soon as an entry
	 * is updated to a higher time, it will be moved to the front (which is
	 * correct because the only operation is to add autovacuum_naptime to the
	 * entry, and time always increases).
	 */
	rebuild_database_list(InvalidOid);

	/* loop until shutdown request */
	while (!ShutdownRequestPending)
	{
		struct timeval nap;
		TimestampTz current_time = 0;
		bool		can_launch;

		/*
		 * This loop is a bit different from the normal use of WaitLatch,
		 * because we'd like to sleep before the first launch of a child
		 * process.  So it's WaitLatch, then ResetLatch, then check for
		 * wakening conditions.
		 */

		launcher_determine_sleep(av_worker_available(), false, &nap);

		/*
		 * Wait until naptime expires or we get some type of signal (all the
		 * signal handlers will wake us by calling SetLatch).
		 */
		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 (nap.tv_sec * 1000L) + (nap.tv_usec / 1000L),
						 WAIT_EVENT_AUTOVACUUM_MAIN);

		ResetLatch(MyLatch);

		ProcessAutoVacLauncherInterrupts();

		/*
		 * a worker finished, or postmaster signaled failure to start a worker
		 */
		if (got_SIGUSR2)
		{
			got_SIGUSR2 = false;

			/* rebalance cost limits, if needed */
			if (AutoVacuumShmem->av_signal[AutoVacRebalance])
			{
				LWLockAcquire(AutovacuumLock, LW_EXCLUSIVE);
				AutoVacuumShmem->av_signal[AutoVacRebalance] = false;
				autovac_recalculate_workers_for_balance();
				LWLockRelease(AutovacuumLock);
			}

			if (AutoVacuumShmem->av_signal[AutoVacForkFailed])
			{
				/*
				 * If the postmaster failed to start a new worker, we sleep
				 * for a little while and resend the signal.  The new worker's
				 * state is still in memory, so this is sufficient.  After
				 * that, we restart the main loop.
				 *
				 * XXX should we put a limit to the number of times we retry?
				 * I don't think it makes much sense, because a future start
				 * of a worker will continue to fail in the same way.
				 */
				AutoVacuumShmem->av_signal[AutoVacForkFailed] = false;
				pg_usleep(1000000L);	/* 1s */
				SendPostmasterSignal(PMSIGNAL_START_AUTOVAC_WORKER);
				continue;
			}
		}

		/*
		 * There are some conditions that we need to check before trying to
		 * start a worker.  First, we need to make sure that there is a worker
		 * slot available.  Second, we need to make sure that no other worker
		 * failed while starting up.
		 */

		current_time = GetCurrentTimestamp();
		LWLockAcquire(AutovacuumLock, LW_SHARED);

		can_launch = av_worker_available();

		if (AutoVacuumShmem->av_startingWorker != NULL)
		{
			int			waittime;
			WorkerInfo	worker = AutoVacuumShmem->av_startingWorker;

			/*
			 * We can't launch another worker when another one is still
			 * starting up (or failed while doing so), so just sleep for a bit
			 * more; that worker will wake us up again as soon as it's ready.
			 * We will only wait autovacuum_naptime seconds (up to a maximum
			 * of 60 seconds) for this to happen however.  Note that failure
			 * to connect to a particular database is not a problem here,
			 * because the worker removes itself from the startingWorker
			 * pointer before trying to connect.  Problems detected by the
			 * postmaster (like fork() failure) are also reported and handled
			 * differently.  The only problems that may cause this code to
			 * fire are errors in the earlier sections of AutoVacWorkerMain,
			 * before the worker removes the WorkerInfo from the
			 * startingWorker pointer.
			 */
			waittime = Min(autovacuum_naptime, 60) * 1000;
			if (TimestampDifferenceExceeds(worker->wi_launchtime, current_time,
										   waittime))
			{
				LWLockRelease(AutovacuumLock);
				LWLockAcquire(AutovacuumLock, LW_EXCLUSIVE);

				/*
				 * No other process can put a worker in starting mode, so if
				 * startingWorker is still INVALID after exchanging our lock,
				 * we assume it's the same one we saw above (so we don't
				 * recheck the launch time).
				 */
				if (AutoVacuumShmem->av_startingWorker != NULL)
				{
					worker = AutoVacuumShmem->av_startingWorker;
					worker->wi_dboid = InvalidOid;
					worker->wi_tableoid = InvalidOid;
					worker->wi_sharedrel = false;
					worker->wi_proc = NULL;
					worker->wi_launchtime = 0;
					dclist_push_head(&AutoVacuumShmem->av_freeWorkers,
									 &worker->wi_links);
					AutoVacuumShmem->av_startingWorker = NULL;
					ereport(WARNING,
							errmsg("autovacuum worker took too long to start; canceled"));
				}
			}
			else
				can_launch = false;
		}
		LWLockRelease(AutovacuumLock);	/* either shared or exclusive */

		/* if we can't do anything, just go back to sleep */
		if (!can_launch)
			continue;

		/* We're OK to start a new worker */

		if (dlist_is_empty(&DatabaseList))
		{
			/*
			 * Special case when the list is empty: start a worker right away.
			 * This covers the initial case, when no database is in pgstats
			 * (thus the list is empty).  Note that the constraints in
			 * launcher_determine_sleep keep us from starting workers too
			 * quickly (at most once every autovacuum_naptime when the list is
			 * empty).
			 */
			launch_worker(current_time);
		}
		else
		{
			/*
			 * because rebuild_database_list constructs a list with most
			 * distant adl_next_worker first, we obtain our database from the
			 * tail of the list.
			 */
			avl_dbase  *avdb;

			avdb = dlist_tail_element(avl_dbase, adl_node, &DatabaseList);

			/*
			 * launch a worker if next_worker is right now or it is in the
			 * past
			 */
			if (TimestampDifferenceExceeds(avdb->adl_next_worker,
										   current_time, 0))
				launch_worker(current_time);
		}
	}

	AutoVacLauncherShutdown();
}

/*
 * Process any new interrupts.
 */
static void
ProcessAutoVacLauncherInterrupts(void)
{
	/* the normal shutdown case */
	if (ShutdownRequestPending)
		AutoVacLauncherShutdown();

	if (ConfigReloadPending)
	{
		int			autovacuum_max_workers_prev = autovacuum_max_workers;

		ConfigReloadPending = false;
		ProcessConfigFile(PGC_SIGHUP);

		/* shutdown requested in config file? */
		if (!AutoVacuumingActive())
			AutoVacLauncherShutdown();

		/*
		 * If autovacuum_max_workers changed, emit a WARNING if
		 * autovacuum_worker_slots < autovacuum_max_workers.  If it didn't
		 * change, skip this to avoid too many repeated log messages.
		 */
		if (autovacuum_max_workers_prev != autovacuum_max_workers)
			check_av_worker_gucs();

		/* rebuild the list in case the naptime changed */
		rebuild_database_list(InvalidOid);
	}

	/* Process barrier events */
	if (ProcSignalBarrierPending)
		ProcessProcSignalBarrier();

	/* Perform logging of memory contexts of this process */
	if (LogMemoryContextPending)
		ProcessLogMemoryContextInterrupt();

	/* Process sinval catchup interrupts that happened while sleeping */
	ProcessCatchupInterrupt();
}

/*
 * Perform a normal exit from the autovac launcher.
 */
static void
AutoVacLauncherShutdown(void)
{
	ereport(DEBUG1,
			(errmsg_internal("autovacuum launcher shutting down")));
	AutoVacuumShmem->av_launcherpid = 0;

	proc_exit(0);				/* done */
}

/*
 * Determine the time to sleep, based on the database list.
 *
 * The "canlaunch" parameter indicates whether we can start a worker right now,
 * for example due to the workers being all busy.  If this is false, we will
 * cause a long sleep, which will be interrupted when a worker exits.
 */
static void
launcher_determine_sleep(bool canlaunch, bool recursing, struct timeval *nap)
{
	/*
	 * We sleep until the next scheduled vacuum.  We trust that when the
	 * database list was built, care was taken so that no entries have times
	 * in the past; if the first entry has too close a next_worker value, or a
	 * time in the past, we will sleep a small nominal time.
	 */
	if (!canlaunch)
	{
		nap->tv_sec = autovacuum_naptime;
		nap->tv_usec = 0;
	}
	else if (!dlist_is_empty(&DatabaseList))
	{
		TimestampTz current_time = GetCurrentTimestamp();
		TimestampTz next_wakeup;
		avl_dbase  *avdb;
		long		secs;
		int			usecs;

		avdb = dlist_tail_element(avl_dbase, adl_node, &DatabaseList);

		next_wakeup = avdb->adl_next_worker;
		TimestampDifference(current_time, next_wakeup, &secs, &usecs);

		nap->tv_sec = secs;
		nap->tv_usec = usecs;
	}
	else
	{
		/* list is empty, sleep for whole autovacuum_naptime seconds  */
		nap->tv_sec = autovacuum_naptime;
		nap->tv_usec = 0;
	}

	/*
	 * If the result is exactly zero, it means a database had an entry with
	 * time in the past.  Rebuild the list so that the databases are evenly
	 * distributed again, and recalculate the time to sleep.  This can happen
	 * if there are more tables needing vacuum than workers, and they all take
	 * longer to vacuum than autovacuum_naptime.
	 *
	 * We only recurse once.  rebuild_database_list should always return times
	 * in the future, but it seems best not to trust too much on that.
	 */
	if (nap->tv_sec == 0 && nap->tv_usec == 0 && !recursing)
	{
		rebuild_database_list(InvalidOid);
		launcher_determine_sleep(canlaunch, true, nap);
		return;
	}

	/* The smallest time we'll allow the launcher to sleep. */
	if (nap->tv_sec <= 0 && nap->tv_usec <= MIN_AUTOVAC_SLEEPTIME * 1000)
	{
		nap->tv_sec = 0;
		nap->tv_usec = MIN_AUTOVAC_SLEEPTIME * 1000;
	}

	/*
	 * If the sleep time is too large, clamp it to an arbitrary maximum (plus
	 * any fractional seconds, for simplicity).  This avoids an essentially
	 * infinite sleep in strange cases like the system clock going backwards a
	 * few years.
	 */
	if (nap->tv_sec > MAX_AUTOVAC_SLEEPTIME)
		nap->tv_sec = MAX_AUTOVAC_SLEEPTIME;
}

/*
 * Build an updated DatabaseList.  It must only contain databases that appear
 * in pgstats, and must be sorted by next_worker from highest to lowest,
 * distributed regularly across the next autovacuum_naptime interval.
 *
 * Receives the Oid of the database that made this list be generated (we call
 * this the "new" database, because when the database was already present on
 * the list, we expect that this function is not called at all).  The
 * preexisting list, if any, will be used to preserve the order of the
 * databases in the autovacuum_naptime period.  The new database is put at the
 * end of the interval.  The actual values are not saved, which should not be
 * much of a problem.
 */
static void
rebuild_database_list(Oid newdb)
{
	List	   *dblist;
	ListCell   *cell;
	MemoryContext newcxt;
	MemoryContext oldcxt;
	MemoryContext tmpcxt;
	HASHCTL		hctl;
	int			score;
	int			nelems;
	HTAB	   *dbhash;
	dlist_iter	iter;

	newcxt = AllocSetContextCreate(AutovacMemCxt,
								   "Autovacuum database list",
								   ALLOCSET_DEFAULT_SIZES);
	tmpcxt = AllocSetContextCreate(newcxt,
								   "Autovacuum database list (tmp)",
								   ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(tmpcxt);

	/*
	 * Implementing this is not as simple as it sounds, because we need to put
	 * the new database at the end of the list; next the databases that were
	 * already on the list, and finally (at the tail of the list) all the
	 * other databases that are not on the existing list.
	 *
	 * To do this, we build an empty hash table of scored databases.  We will
	 * start with the lowest score (zero) for the new database, then
	 * increasing scores for the databases in the existing list, in order, and
	 * lastly increasing scores for all databases gotten via
	 * get_database_list() that are not already on the hash.
	 *
	 * Then we will put all the hash elements into an array, sort the array by
	 * score, and finally put the array elements into the new doubly linked
	 * list.
	 */
	hctl.keysize = sizeof(Oid);
	hctl.entrysize = sizeof(avl_dbase);
	hctl.hcxt = tmpcxt;
	dbhash = hash_create("autovacuum db hash", 20, &hctl,	/* magic number here
															 * FIXME */
						 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/* start by inserting the new database */
	score = 0;
	if (OidIsValid(newdb))
	{
		avl_dbase  *db;
		PgStat_StatDBEntry *entry;

		/* only consider this database if it has a pgstat entry */
		entry = pgstat_fetch_stat_dbentry(newdb);
		if (entry != NULL)
		{
			/* we assume it isn't found because the hash was just created */
			db = hash_search(dbhash, &newdb, HASH_ENTER, NULL);

			/* hash_search already filled in the key */
			db->adl_score = score++;
			/* next_worker is filled in later */
		}
	}

	/* Now insert the databases from the existing list */
	dlist_foreach(iter, &DatabaseList)
	{
		avl_dbase  *avdb = dlist_container(avl_dbase, adl_node, iter.cur);
		avl_dbase  *db;
		bool		found;
		PgStat_StatDBEntry *entry;

		/*
		 * skip databases with no stat entries -- in particular, this gets rid
		 * of dropped databases
		 */
		entry = pgstat_fetch_stat_dbentry(avdb->adl_datid);
		if (entry == NULL)
			continue;

		db = hash_search(dbhash, &(avdb->adl_datid), HASH_ENTER, &found);

		if (!found)
		{
			/* hash_search already filled in the key */
			db->adl_score = score++;
			/* next_worker is filled in later */
		}
	}

	/* finally, insert all qualifying databases not previously inserted */
	dblist = get_database_list();
	foreach(cell, dblist)
	{
		avw_dbase  *avdb = lfirst(cell);
		avl_dbase  *db;
		bool		found;
		PgStat_StatDBEntry *entry;

		/* only consider databases with a pgstat entry */
		entry = pgstat_fetch_stat_dbentry(avdb->adw_datid);
		if (entry == NULL)
			continue;

		db = hash_search(dbhash, &(avdb->adw_datid), HASH_ENTER, &found);
		/* only update the score if the database was not already on the hash */
		if (!found)
		{
			/* hash_search already filled in the key */
			db->adl_score = score++;
			/* next_worker is filled in later */
		}
	}
	nelems = score;

	/* from here on, the allocated memory belongs to the new list */
	MemoryContextSwitchTo(newcxt);
	dlist_init(&DatabaseList);

	if (nelems > 0)
	{
		TimestampTz current_time;
		int			millis_increment;
		avl_dbase  *dbary;
		avl_dbase  *db;
		HASH_SEQ_STATUS seq;
		int			i;

		/* put all the hash elements into an array */
		dbary = palloc(nelems * sizeof(avl_dbase));

		i = 0;
		hash_seq_init(&seq, dbhash);
		while ((db = hash_seq_search(&seq)) != NULL)
			memcpy(&(dbary[i++]), db, sizeof(avl_dbase));

		/* sort the array */
		qsort(dbary, nelems, sizeof(avl_dbase), db_comparator);

		/*
		 * Determine the time interval between databases in the schedule. If
		 * we see that the configured naptime would take us to sleep times
		 * lower than our min sleep time (which launcher_determine_sleep is
		 * coded not to allow), silently use a larger naptime (but don't touch
		 * the GUC variable).
		 */
		millis_increment = 1000.0 * autovacuum_naptime / nelems;
		if (millis_increment <= MIN_AUTOVAC_SLEEPTIME)
			millis_increment = MIN_AUTOVAC_SLEEPTIME * 1.1;

		current_time = GetCurrentTimestamp();

		/*
		 * move the elements from the array into the dlist, setting the
		 * next_worker while walking the array
		 */
		for (i = 0; i < nelems; i++)
		{
			db = &(dbary[i]);

			current_time = TimestampTzPlusMilliseconds(current_time,
													   millis_increment);
			db->adl_next_worker = current_time;

			/* later elements should go closer to the head of the list */
			dlist_push_head(&DatabaseList, &db->adl_node);
		}
	}

	/* all done, clean up memory */
	if (DatabaseListCxt != NULL)
		MemoryContextDelete(DatabaseListCxt);
	MemoryContextDelete(tmpcxt);
	DatabaseListCxt = newcxt;
	MemoryContextSwitchTo(oldcxt);
}

/* qsort comparator for avl_dbase, using adl_score */
static int
db_comparator(const void *a, const void *b)
{
	return pg_cmp_s32(((const avl_dbase *) a)->adl_score,
					  ((const avl_dbase *) b)->adl_score);
}

/*
 * do_start_worker
 *
 * Bare-bones procedure for starting an autovacuum worker from the launcher.
 * It determines what database to work on, sets up shared memory stuff and
 * signals postmaster to start the worker.  It fails gracefully if invoked when
 * autovacuum_workers are already active.
 *
 * Return value is the OID of the database that the worker is going to process,
 * or InvalidOid if no worker was actually started.
 */
static Oid
do_start_worker(void)
{
	List	   *dblist;
	ListCell   *cell;
	TransactionId xidForceLimit;
	MultiXactId multiForceLimit;
	bool		for_xid_wrap;
	bool		for_multi_wrap;
	avw_dbase  *avdb;
	TimestampTz current_time;
	bool		skipit = false;
	Oid			retval = InvalidOid;
	MemoryContext tmpcxt,
				oldcxt;

	/* return quickly when there are no free workers */
	LWLockAcquire(AutovacuumLock, LW_SHARED);
	if (!av_worker_available())
	{
		LWLockRelease(AutovacuumLock);
		return InvalidOid;
	}
	LWLockRelease(AutovacuumLock);

	/*
	 * Create and switch to a temporary context to avoid leaking the memory
	 * allocated for the database list.
	 */
	tmpcxt = AllocSetContextCreate(CurrentMemoryContext,
								   "Autovacuum start worker (tmp)",
								   ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(tmpcxt);

	/* Get a list of databases */
	dblist = get_database_list();

	/*
	 * Determine the oldest datfrozenxid/relfrozenxid that we will allow to
	 * pass without forcing a vacuum.  (This limit can be tightened for
	 * particular tables, but not loosened.)
	 */
	recentXid = ReadNextTransactionId();
	xidForceLimit = recentXid - autovacuum_freeze_max_age;
	/* ensure it's a "normal" XID, else TransactionIdPrecedes misbehaves */
	/* this can cause the limit to go backwards by 3, but that's OK */
	if (xidForceLimit < FirstNormalTransactionId)
		xidForceLimit -= FirstNormalTransactionId;

	/* Also determine the oldest datminmxid we will consider. */
	recentMulti = ReadNextMultiXactId();
	multiForceLimit = recentMulti - MultiXactMemberFreezeThreshold();
	if (multiForceLimit < FirstMultiXactId)
		multiForceLimit -= FirstMultiXactId;

	/*
	 * Choose a database to connect to.  We pick the database that was least
	 * recently auto-vacuumed, or one that needs vacuuming to prevent Xid
	 * wraparound-related data loss.  If any db at risk of Xid wraparound is
	 * found, we pick the one with oldest datfrozenxid, independently of
	 * autovacuum times; similarly we pick the one with the oldest datminmxid
	 * if any is in MultiXactId wraparound.  Note that those in Xid wraparound
	 * danger are given more priority than those in multi wraparound danger.
	 *
	 * Note that a database with no stats entry is not considered, except for
	 * Xid wraparound purposes.  The theory is that if no one has ever
	 * connected to it since the stats were last initialized, it doesn't need
	 * vacuuming.
	 *
	 * XXX This could be improved if we had more info about whether it needs
	 * vacuuming before connecting to it.  Perhaps look through the pgstats
	 * data for the database's tables?  One idea is to keep track of the
	 * number of new and dead tuples per database in pgstats.  However it
	 * isn't clear how to construct a metric that measures that and not cause
	 * starvation for less busy databases.
	 */
	avdb = NULL;
	for_xid_wrap = false;
	for_multi_wrap = false;
	current_time = GetCurrentTimestamp();
	foreach(cell, dblist)
	{
		avw_dbase  *tmp = lfirst(cell);
		dlist_iter	iter;

		/* Check to see if this one is at risk of wraparound */
		if (TransactionIdPrecedes(tmp->adw_frozenxid, xidForceLimit))
		{
			if (avdb == NULL ||
				TransactionIdPrecedes(tmp->adw_frozenxid,
									  avdb->adw_frozenxid))
				avdb = tmp;
			for_xid_wrap = true;
			continue;
		}
		else if (for_xid_wrap)
			continue;			/* ignore not-at-risk DBs */
		else if (MultiXactIdPrecedes(tmp->adw_minmulti, multiForceLimit))
		{
			if (avdb == NULL ||
				MultiXactIdPrecedes(tmp->adw_minmulti, avdb->adw_minmulti))
				avdb = tmp;
			for_multi_wrap = true;
			continue;
		}
		else if (for_multi_wrap)
			continue;			/* ignore not-at-risk DBs */

		/* Find pgstat entry if any */
		tmp->adw_entry = pgstat_fetch_stat_dbentry(tmp->adw_datid);

		/*
		 * Skip a database with no pgstat entry; it means it hasn't seen any
		 * activity.
		 */
		if (!tmp->adw_entry)
			continue;

		/*
		 * Also, skip a database that appears on the database list as having
		 * been processed recently (less than autovacuum_naptime seconds ago).
		 * We do this so that we don't select a database which we just
		 * selected, but that pgstat hasn't gotten around to updating the last
		 * autovacuum time yet.
		 */
		skipit = false;

		dlist_reverse_foreach(iter, &DatabaseList)
		{
			avl_dbase  *dbp = dlist_container(avl_dbase, adl_node, iter.cur);

			if (dbp->adl_datid == tmp->adw_datid)
			{
				/*
				 * Skip this database if its next_worker value falls between
				 * the current time and the current time plus naptime.
				 */
				if (!TimestampDifferenceExceeds(dbp->adl_next_worker,
												current_time, 0) &&
					!TimestampDifferenceExceeds(current_time,
												dbp->adl_next_worker,
												autovacuum_naptime * 1000))
					skipit = true;

				break;
			}
		}
		if (skipit)
			continue;

		/*
		 * Remember the db with oldest autovac time.  (If we are here, both
		 * tmp->entry and db->entry must be non-null.)
		 */
		if (avdb == NULL ||
			tmp->adw_entry->last_autovac_time < avdb->adw_entry->last_autovac_time)
			avdb = tmp;
	}

	/* Found a database -- process it */
	if (avdb != NULL)
	{
		WorkerInfo	worker;
		dlist_node *wptr;

		LWLockAcquire(AutovacuumLock, LW_EXCLUSIVE);

		/*
		 * Get a worker entry from the freelist.  We checked above, so there
		 * really should be a free slot.
		 */
		wptr = dclist_pop_head_node(&AutoVacuumShmem->av_freeWorkers);

		worker = dlist_container(WorkerInfoData, wi_links, wptr);
		worker->wi_dboid = avdb->adw_datid;
		worker->wi_proc = NULL;
		worker->wi_launchtime = GetCurrentTimestamp();

		AutoVacuumShmem->av_startingWorker = worker;

		LWLockRelease(AutovacuumLock);

		SendPostmasterSignal(PMSIGNAL_START_AUTOVAC_WORKER);

		retval = avdb->adw_datid;
	}
	else if (skipit)
	{
		/*
		 * If we skipped all databases on the list, rebuild it, because it
		 * probably contains a dropped database.
		 */
		rebuild_database_list(InvalidOid);
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(tmpcxt);

	return retval;
}

/*
 * launch_worker
 *
 * Wrapper for starting a worker from the launcher.  Besides actually starting
 * it, update the database list to reflect the next time that another one will
 * need to be started on the selected database.  The actual database choice is
 * left to do_start_worker.
 *
 * This routine is also expected to insert an entry into the database list if
 * the selected database was previously absent from the list.
 */
static void
launch_worker(TimestampTz now)
{
	Oid			dbid;
	dlist_iter	iter;

	dbid = do_start_worker();
	if (OidIsValid(dbid))
	{
		bool		found = false;

		/*
		 * Walk the database list and update the corresponding entry.  If the
		 * database is not on the list, we'll recreate the list.
		 */
		dlist_foreach(iter, &DatabaseList)
		{
			avl_dbase  *avdb = dlist_container(avl_dbase, adl_node, iter.cur);

			if (avdb->adl_datid == dbid)
			{
				found = true;

				/*
				 * add autovacuum_naptime seconds to the current time, and use
				 * that as the new "next_worker" field for this database.
				 */
				avdb->adl_next_worker =
					TimestampTzPlusMilliseconds(now, autovacuum_naptime * 1000);

				dlist_move_head(&DatabaseList, iter.cur);
				break;
			}
		}

		/*
		 * If the database was not present in the database list, we rebuild
		 * the list.  It's possible that the database does not get into the
		 * list anyway, for example if it's a database that doesn't have a
		 * pgstat entry, but this is not a problem because we don't want to
		 * schedule workers regularly into those in any case.
		 */
		if (!found)
			rebuild_database_list(dbid);
	}
}

/*
 * Called from postmaster to signal a failure to fork a process to become
 * worker.  The postmaster should kill(SIGUSR2) the launcher shortly
 * after calling this function.
 */
void
AutoVacWorkerFailed(void)
{
	AutoVacuumShmem->av_signal[AutoVacForkFailed] = true;
}

/* SIGUSR2: a worker is up and running, or just finished, or failed to fork */
static void
avl_sigusr2_handler(SIGNAL_ARGS)
{
	got_SIGUSR2 = true;
	SetLatch(MyLatch);
}


/********************************************************************
 *					  AUTOVACUUM WORKER CODE
 ********************************************************************/

/*
 * Main entry point for autovacuum worker processes.
 */
void
AutoVacWorkerMain(const void *startup_data, size_t startup_data_len)
{
	sigjmp_buf	local_sigjmp_buf;
	Oid			dbid;

	Assert(startup_data_len == 0);

	/* Release postmaster's working memory context */
	if (PostmasterContext)
	{
		MemoryContextDelete(PostmasterContext);
		PostmasterContext = NULL;
	}

	MyBackendType = B_AUTOVAC_WORKER;
	init_ps_display(NULL);

	Assert(GetProcessingMode() == InitProcessing);

	/*
	 * Set up signal handlers.  We operate on databases much like a regular
	 * backend, so we use the same signal handling.  See equivalent code in
	 * tcop/postgres.c.
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);

	/*
	 * SIGINT is used to signal canceling the current table's vacuum; SIGTERM
	 * means abort and exit cleanly, and SIGQUIT means abandon ship.
	 */
	pqsignal(SIGINT, StatementCancelHandler);
	pqsignal(SIGTERM, die);
	/* SIGQUIT handler was already set up by InitPostmasterChild */

	InitializeTimeouts();		/* establishes SIGALRM handler */

	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, SIG_IGN);
	pqsignal(SIGFPE, FloatExceptionHandler);
	pqsignal(SIGCHLD, SIG_DFL);

	/*
	 * Create a per-backend PGPROC struct in shared memory.  We must do this
	 * before we can use LWLocks or access any shared memory.
	 */
	InitProcess();

	/* Early initialization */
	BaseInit();

	/*
	 * If an exception is encountered, processing resumes here.
	 *
	 * Unlike most auxiliary processes, we don't attempt to continue
	 * processing after an error; we just clean up and exit.  The autovac
	 * launcher is responsible for spawning another worker later.
	 *
	 * Note that we use sigsetjmp(..., 1), so that the prevailing signal mask
	 * (to wit, BlockSig) will be restored when longjmp'ing to here.  Thus,
	 * signals other than SIGQUIT will be blocked until we exit.  It might
	 * seem that this policy makes the HOLD_INTERRUPTS() call redundant, but
	 * it is not since InterruptPending might be set already.
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* since not using PG_TRY, must reset error stack by hand */
		error_context_stack = NULL;

		/* Prevents interrupts while cleaning up */
		HOLD_INTERRUPTS();

		/* Report the error to the server log */
		EmitErrorReport();

		/*
		 * We can now go away.  Note that because we called InitProcess, a
		 * callback was registered to do ProcKill, which will clean up
		 * necessary state.
		 */
		proc_exit(0);
	}

	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/*
	 * Set always-secure search path, so malicious users can't redirect user
	 * code (e.g. pg_index.indexprs).  (That code runs in a
	 * SECURITY_RESTRICTED_OPERATION sandbox, so malicious users could not
	 * take control of the entire autovacuum worker in any case.)
	 */
	SetConfigOption("search_path", "", PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * Force zero_damaged_pages OFF in the autovac process, even if it is set
	 * in postgresql.conf.  We don't really want such a dangerous option being
	 * applied non-interactively.
	 */
	SetConfigOption("zero_damaged_pages", "false", PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * Force settable timeouts off to avoid letting these settings prevent
	 * regular maintenance from being executed.
	 */
	SetConfigOption("statement_timeout", "0", PGC_SUSET, PGC_S_OVERRIDE);
	SetConfigOption("transaction_timeout", "0", PGC_SUSET, PGC_S_OVERRIDE);
	SetConfigOption("lock_timeout", "0", PGC_SUSET, PGC_S_OVERRIDE);
	SetConfigOption("idle_in_transaction_session_timeout", "0",
					PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * Force default_transaction_isolation to READ COMMITTED.  We don't want
	 * to pay the overhead of serializable mode, nor add any risk of causing
	 * deadlocks or delaying other transactions.
	 */
	SetConfigOption("default_transaction_isolation", "read committed",
					PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * Force synchronous replication off to allow regular maintenance even if
	 * we are waiting for standbys to connect. This is important to ensure we
	 * aren't blocked from performing anti-wraparound tasks.
	 */
	if (synchronous_commit > SYNCHRONOUS_COMMIT_LOCAL_FLUSH)
		SetConfigOption("synchronous_commit", "local",
						PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * Even when system is configured to use a different fetch consistency,
	 * for autovac we always want fresh stats.
	 */
	SetConfigOption("stats_fetch_consistency", "none", PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * Get the info about the database we're going to work on.
	 */
	LWLockAcquire(AutovacuumLock, LW_EXCLUSIVE);

	/*
	 * beware of startingWorker being INVALID; this should normally not
	 * happen, but if a worker fails after forking and before this, the
	 * launcher might have decided to remove it from the queue and start
	 * again.
	 */
	if (AutoVacuumShmem->av_startingWorker != NULL)
	{
		MyWorkerInfo = AutoVacuumShmem->av_startingWorker;
		dbid = MyWorkerInfo->wi_dboid;
		MyWorkerInfo->wi_proc = MyProc;

		/* insert into the running list */
		dlist_push_head(&AutoVacuumShmem->av_runningWorkers,
						&MyWorkerInfo->wi_links);

		/*
		 * remove from the "starting" pointer, so that the launcher can start
		 * a new worker if required
		 */
		AutoVacuumShmem->av_startingWorker = NULL;
		LWLockRelease(AutovacuumLock);

		on_shmem_exit(FreeWorkerInfo, 0);

		/* wake up the launcher */
		if (AutoVacuumShmem->av_launcherpid != 0)
			kill(AutoVacuumShmem->av_launcherpid, SIGUSR2);
	}
	else
	{
		/* no worker entry for me, go away */
		elog(WARNING, "autovacuum worker started without a worker entry");
		dbid = InvalidOid;
		LWLockRelease(AutovacuumLock);
	}

	if (OidIsValid(dbid))
	{
		char		dbname[NAMEDATALEN];

		/*
		 * Report autovac startup to the cumulative stats system.  We
		 * deliberately do this before InitPostgres, so that the
		 * last_autovac_time will get updated even if the connection attempt
		 * fails.  This is to prevent autovac from getting "stuck" repeatedly
		 * selecting an unopenable database, rather than making any progress
		 * on stuff it can connect to.
		 */
		pgstat_report_autovac(dbid);

		/*
		 * Connect to the selected database, specifying no particular user,
		 * and ignoring datallowconn.  Collect the database's name for
		 * display.
		 *
		 * Note: if we have selected a just-deleted database (due to using
		 * stale stats info), we'll fail and exit here.
		 */
		InitPostgres(NULL, dbid, NULL, InvalidOid,
					 INIT_PG_OVERRIDE_ALLOW_CONNS,
					 dbname);
		SetProcessingMode(NormalProcessing);
		set_ps_display(dbname);
		ereport(DEBUG1,
				(errmsg_internal("autovacuum: processing database \"%s\"", dbname)));

		if (PostAuthDelay)
			pg_usleep(PostAuthDelay * 1000000L);

		/* And do an appropriate amount of work */
		recentXid = ReadNextTransactionId();
		recentMulti = ReadNextMultiXactId();
		do_autovacuum();
	}

	/*
	 * The launcher will be notified of my death in ProcKill, *if* we managed
	 * to get a worker slot at all
	 */

	/* All done, go away */
	proc_exit(0);
}

/*
 * Return a WorkerInfo to the free list
 */
static void
FreeWorkerInfo(int code, Datum arg)
{
	if (MyWorkerInfo != NULL)
	{
		LWLockAcquire(AutovacuumLock, LW_EXCLUSIVE);

		/*
		 * Wake the launcher up so that he can launch a new worker immediately
		 * if required.  We only save the launcher's PID in local memory here;
		 * the actual signal will be sent when the PGPROC is recycled.  Note
		 * that we always do this, so that the launcher can rebalance the cost
		 * limit setting of the remaining workers.
		 *
		 * We somewhat ignore the risk that the launcher changes its PID
		 * between us reading it and the actual kill; we expect ProcKill to be
		 * called shortly after us, and we assume that PIDs are not reused too
		 * quickly after a process exits.
		 */
		AutovacuumLauncherPid = AutoVacuumShmem->av_launcherpid;

		dlist_delete(&MyWorkerInfo->wi_links);
		MyWorkerInfo->wi_dboid = InvalidOid;
		MyWorkerInfo->wi_tableoid = InvalidOid;
		MyWorkerInfo->wi_sharedrel = false;
		MyWorkerInfo->wi_proc = NULL;
		MyWorkerInfo->wi_launchtime = 0;
		pg_atomic_clear_flag(&MyWorkerInfo->wi_dobalance);
		dclist_push_head(&AutoVacuumShmem->av_freeWorkers,
						 &MyWorkerInfo->wi_links);
		/* not mine anymore */
		MyWorkerInfo = NULL;

		/*
		 * now that we're inactive, cause a rebalancing of the surviving
		 * workers
		 */
		AutoVacuumShmem->av_signal[AutoVacRebalance] = true;
		LWLockRelease(AutovacuumLock);
	}
}

/*
 * Update vacuum cost-based delay-related parameters for autovacuum workers and
 * backends executing VACUUM or ANALYZE using the value of relevant GUCs and
 * global state. This must be called during setup for vacuum and after every
 * config reload to ensure up-to-date values.
 */
void
VacuumUpdateCosts(void)
{
	if (MyWorkerInfo)
	{
		if (av_storage_param_cost_delay >= 0)
			vacuum_cost_delay = av_storage_param_cost_delay;
		else if (autovacuum_vac_cost_delay >= 0)
			vacuum_cost_delay = autovacuum_vac_cost_delay;
		else
			/* fall back to VacuumCostDelay */
			vacuum_cost_delay = VacuumCostDelay;

		AutoVacuumUpdateCostLimit();
	}
	else
	{
		/* Must be explicit VACUUM or ANALYZE */
		vacuum_cost_delay = VacuumCostDelay;
		vacuum_cost_limit = VacuumCostLimit;
	}

	/*
	 * If configuration changes are allowed to impact VacuumCostActive, make
	 * sure it is updated.
	 */
	if (VacuumFailsafeActive)
		Assert(!VacuumCostActive);
	else if (vacuum_cost_delay > 0)
		VacuumCostActive = true;
	else
	{
		VacuumCostActive = false;
		VacuumCostBalance = 0;
	}

	/*
	 * Since the cost logging requires a lock, avoid rendering the log message
	 * in case we are using a message level where the log wouldn't be emitted.
	 */
	if (MyWorkerInfo && message_level_is_interesting(DEBUG2))
	{
		Oid			dboid,
					tableoid;

		Assert(!LWLockHeldByMe(AutovacuumLock));

		LWLockAcquire(AutovacuumLock, LW_SHARED);
		dboid = MyWorkerInfo->wi_dboid;
		tableoid = MyWorkerInfo->wi_tableoid;
		LWLockRelease(AutovacuumLock);

		elog(DEBUG2,
			 "Autovacuum VacuumUpdateCosts(db=%u, rel=%u, dobalance=%s, cost_limit=%d, cost_delay=%g active=%s failsafe=%s)",
			 dboid, tableoid, pg_atomic_unlocked_test_flag(&MyWorkerInfo->wi_dobalance) ? "no" : "yes",
			 vacuum_cost_limit, vacuum_cost_delay,
			 vacuum_cost_delay > 0 ? "yes" : "no",
			 VacuumFailsafeActive ? "yes" : "no");
	}
}

/*
 * Update vacuum_cost_limit with the correct value for an autovacuum worker,
 * given the value of other relevant cost limit parameters and the number of
 * workers across which the limit must be balanced. Autovacuum workers must
 * call this regularly in case av_nworkersForBalance has been updated by
 * another worker or by the autovacuum launcher. They must also call it after a
 * config reload.
 */
void
AutoVacuumUpdateCostLimit(void)
{
	if (!MyWorkerInfo)
		return;

	/*
	 * note: in cost_limit, zero also means use value from elsewhere, because
	 * zero is not a valid value.
	 */

	if (av_storage_param_cost_limit > 0)
		vacuum_cost_limit = av_storage_param_cost_limit;
	else
	{
		int			nworkers_for_balance;

		if (autovacuum_vac_cost_limit > 0)
			vacuum_cost_limit = autovacuum_vac_cost_limit;
		else
			vacuum_cost_limit = VacuumCostLimit;

		/* Only balance limit if no cost-related storage parameters specified */
		if (pg_atomic_unlocked_test_flag(&MyWorkerInfo->wi_dobalance))
			return;

		Assert(vacuum_cost_limit > 0);

		nworkers_for_balance = pg_atomic_read_u32(&AutoVacuumShmem->av_nworkersForBalance);

		/* There is at least 1 autovac worker (this worker) */
		if (nworkers_for_balance <= 0)
			elog(ERROR, "nworkers_for_balance must be > 0");

		vacuum_cost_limit = Max(vacuum_cost_limit / nworkers_for_balance, 1);
	}
}

/*
 * autovac_recalculate_workers_for_balance
 *		Recalculate the number of workers to consider, given cost-related
 *		storage parameters and the current number of active workers.
 *
 * Caller must hold the AutovacuumLock in at least shared mode to access
 * worker->wi_proc.
 */
static void
autovac_recalculate_workers_for_balance(void)
{
	dlist_iter	iter;
	int			orig_nworkers_for_balance;
	int			nworkers_for_balance = 0;

	Assert(LWLockHeldByMe(AutovacuumLock));

	orig_nworkers_for_balance =
		pg_atomic_read_u32(&AutoVacuumShmem->av_nworkersForBalance);

	dlist_foreach(iter, &AutoVacuumShmem->av_runningWorkers)
	{
		WorkerInfo	worker = dlist_container(WorkerInfoData, wi_links, iter.cur);

		if (worker->wi_proc == NULL ||
			pg_atomic_unlocked_test_flag(&worker->wi_dobalance))
			continue;

		nworkers_for_balance++;
	}

	if (nworkers_for_balance != orig_nworkers_for_balance)
		pg_atomic_write_u32(&AutoVacuumShmem->av_nworkersForBalance,
							nworkers_for_balance);
}

/*
 * get_database_list
 *		Return a list of all databases found in pg_database.
 *
 * The list and associated data is allocated in the caller's memory context,
 * which is in charge of ensuring that it's properly cleaned up afterwards.
 *
 * Note: this is the only function in which the autovacuum launcher uses a
 * transaction.  Although we aren't attached to any particular database and
 * therefore can't access most catalogs, we do have enough infrastructure
 * to do a seqscan on pg_database.
 */
static List *
get_database_list(void)
{
	List	   *dblist = NIL;
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	MemoryContext resultcxt;

	/* This is the context that we will allocate our output data in */
	resultcxt = CurrentMemoryContext;

	/*
	 * Start a transaction so we can access pg_database.
	 */
	StartTransactionCommand();

	rel = table_open(DatabaseRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);

	while (HeapTupleIsValid(tup = heap_getnext(scan, ForwardScanDirection)))
	{
		Form_pg_database pgdatabase = (Form_pg_database) GETSTRUCT(tup);
		avw_dbase  *avdb;
		MemoryContext oldcxt;

		/*
		 * If database has partially been dropped, we can't, nor need to,
		 * vacuum it.
		 */
		if (database_is_invalid_form(pgdatabase))
		{
			elog(DEBUG2,
				 "autovacuum: skipping invalid database \"%s\"",
				 NameStr(pgdatabase->datname));
			continue;
		}

		/*
		 * Allocate our results in the caller's context, not the
		 * transaction's. We do this inside the loop, and restore the original
		 * context at the end, so that leaky things like heap_getnext() are
		 * not called in a potentially long-lived context.
		 */
		oldcxt = MemoryContextSwitchTo(resultcxt);

		avdb = (avw_dbase *) palloc(sizeof(avw_dbase));

		avdb->adw_datid = pgdatabase->oid;
		avdb->adw_name = pstrdup(NameStr(pgdatabase->datname));
		avdb->adw_frozenxid = pgdatabase->datfrozenxid;
		avdb->adw_minmulti = pgdatabase->datminmxid;
		/* this gets set later: */
		avdb->adw_entry = NULL;

		dblist = lappend(dblist, avdb);
		MemoryContextSwitchTo(oldcxt);
	}

	table_endscan(scan);
	table_close(rel, AccessShareLock);

	CommitTransactionCommand();

	/* Be sure to restore caller's memory context */
	MemoryContextSwitchTo(resultcxt);

	return dblist;
}

/*
 * Process a database table-by-table
 *
 * Note that CHECK_FOR_INTERRUPTS is supposed to be used in certain spots in
 * order not to ignore shutdown commands for too long.
 */
static void
do_autovacuum(void)
{
	Relation	classRel;
	HeapTuple	tuple;
	TableScanDesc relScan;
	Form_pg_database dbForm;
	List	   *table_oids = NIL;
	List	   *orphan_oids = NIL;
	HASHCTL		ctl;
	HTAB	   *table_toast_map;
	ListCell   *volatile cell;
	BufferAccessStrategy bstrategy;
	ScanKeyData key;
	TupleDesc	pg_class_desc;
	int			effective_multixact_freeze_max_age;
	bool		did_vacuum = false;
	bool		found_concurrent_worker = false;
	int			i;

	/*
	 * StartTransactionCommand and CommitTransactionCommand will automatically
	 * switch to other contexts.  We need this one to keep the list of
	 * relations to vacuum/analyze across transactions.
	 */
	AutovacMemCxt = AllocSetContextCreate(TopMemoryContext,
										  "Autovacuum worker",
										  ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(AutovacMemCxt);

	/* Start a transaction so our commands have one to play into. */
	StartTransactionCommand();

	/*
	 * This injection point is put in a transaction block to work with a wait
	 * that uses a condition variable.
	 */
	INJECTION_POINT("autovacuum-worker-start", NULL);

	/*
	 * Compute the multixact age for which freezing is urgent.  This is
	 * normally autovacuum_multixact_freeze_max_age, but may be less if we are
	 * short of multixact member space.
	 */
	effective_multixact_freeze_max_age = MultiXactMemberFreezeThreshold();

	/*
	 * Find the pg_database entry and select the default freeze ages. We use
	 * zero in template and nonconnectable databases, else the system-wide
	 * default.
	 */
	tuple = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for database %u", MyDatabaseId);
	dbForm = (Form_pg_database) GETSTRUCT(tuple);

	if (dbForm->datistemplate || !dbForm->datallowconn)
	{
		default_freeze_min_age = 0;
		default_freeze_table_age = 0;
		default_multixact_freeze_min_age = 0;
		default_multixact_freeze_table_age = 0;
	}
	else
	{
		default_freeze_min_age = vacuum_freeze_min_age;
		default_freeze_table_age = vacuum_freeze_table_age;
		default_multixact_freeze_min_age = vacuum_multixact_freeze_min_age;
		default_multixact_freeze_table_age = vacuum_multixact_freeze_table_age;
	}

	ReleaseSysCache(tuple);

	/* StartTransactionCommand changed elsewhere */
	MemoryContextSwitchTo(AutovacMemCxt);

	classRel = table_open(RelationRelationId, AccessShareLock);

	/* create a copy so we can use it after closing pg_class */
	pg_class_desc = CreateTupleDescCopy(RelationGetDescr(classRel));

	/* create hash table for toast <-> main relid mapping */
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(av_relation);

	table_toast_map = hash_create("TOAST to main relid map",
								  100,
								  &ctl,
								  HASH_ELEM | HASH_BLOBS);

	/*
	 * Scan pg_class to determine which tables to vacuum.
	 *
	 * We do this in two passes: on the first one we collect the list of plain
	 * relations and materialized views, and on the second one we collect
	 * TOAST tables. The reason for doing the second pass is that during it we
	 * want to use the main relation's pg_class.reloptions entry if the TOAST
	 * table does not have any, and we cannot obtain it unless we know
	 * beforehand what's the main table OID.
	 *
	 * We need to check TOAST tables separately because in cases with short,
	 * wide tables there might be proportionally much more activity in the
	 * TOAST table than in its parent.
	 */
	relScan = table_beginscan_catalog(classRel, 0, NULL);

	/*
	 * On the first pass, we collect main tables to vacuum, and also the main
	 * table relid to TOAST relid mapping.
	 */
	while ((tuple = heap_getnext(relScan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class classForm = (Form_pg_class) GETSTRUCT(tuple);
		PgStat_StatTabEntry *tabentry;
		AutoVacOpts *relopts;
		Oid			relid;
		bool		dovacuum;
		bool		doanalyze;
		bool		wraparound;

		if (classForm->relkind != RELKIND_RELATION &&
			classForm->relkind != RELKIND_MATVIEW)
			continue;

		relid = classForm->oid;

		/*
		 * Check if it is a temp table (presumably, of some other backend's).
		 * We cannot safely process other backends' temp tables.
		 */
		if (classForm->relpersistence == RELPERSISTENCE_TEMP)
		{
			/*
			 * We just ignore it if the owning backend is still active and
			 * using the temporary schema.  Also, for safety, ignore it if the
			 * namespace doesn't exist or isn't a temp namespace after all.
			 */
			if (checkTempNamespaceStatus(classForm->relnamespace) == TEMP_NAMESPACE_IDLE)
			{
				/*
				 * The table seems to be orphaned -- although it might be that
				 * the owning backend has already deleted it and exited; our
				 * pg_class scan snapshot is not necessarily up-to-date
				 * anymore, so we could be looking at a committed-dead entry.
				 * Remember it so we can try to delete it later.
				 */
				orphan_oids = lappend_oid(orphan_oids, relid);
			}
			continue;
		}

		/* Fetch reloptions and the pgstat entry for this table */
		relopts = extract_autovac_opts(tuple, pg_class_desc);
		tabentry = pgstat_fetch_stat_tabentry_ext(classForm->relisshared,
												  relid);

		/* Check if it needs vacuum or analyze */
		relation_needs_vacanalyze(relid, relopts, classForm, tabentry,
								  effective_multixact_freeze_max_age,
								  &dovacuum, &doanalyze, &wraparound);

		/* Relations that need work are added to table_oids */
		if (dovacuum || doanalyze)
			table_oids = lappend_oid(table_oids, relid);

		/*
		 * Remember TOAST associations for the second pass.  Note: we must do
		 * this whether or not the table is going to be vacuumed, because we
		 * don't automatically vacuum toast tables along the parent table.
		 */
		if (OidIsValid(classForm->reltoastrelid))
		{
			av_relation *hentry;
			bool		found;

			hentry = hash_search(table_toast_map,
								 &classForm->reltoastrelid,
								 HASH_ENTER, &found);

			if (!found)
			{
				/* hash_search already filled in the key */
				hentry->ar_relid = relid;
				hentry->ar_hasrelopts = false;
				if (relopts != NULL)
				{
					hentry->ar_hasrelopts = true;
					memcpy(&hentry->ar_reloptions, relopts,
						   sizeof(AutoVacOpts));
				}
			}
		}

		/* Release stuff to avoid per-relation leakage */
		if (relopts)
			pfree(relopts);
		if (tabentry)
			pfree(tabentry);
	}

	table_endscan(relScan);

	/* second pass: check TOAST tables */
	ScanKeyInit(&key,
				Anum_pg_class_relkind,
				BTEqualStrategyNumber, F_CHAREQ,
				CharGetDatum(RELKIND_TOASTVALUE));

	relScan = table_beginscan_catalog(classRel, 1, &key);
	while ((tuple = heap_getnext(relScan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class classForm = (Form_pg_class) GETSTRUCT(tuple);
		PgStat_StatTabEntry *tabentry;
		Oid			relid;
		AutoVacOpts *relopts;
		bool		free_relopts = false;
		bool		dovacuum;
		bool		doanalyze;
		bool		wraparound;

		/*
		 * We cannot safely process other backends' temp tables, so skip 'em.
		 */
		if (classForm->relpersistence == RELPERSISTENCE_TEMP)
			continue;

		relid = classForm->oid;

		/*
		 * fetch reloptions -- if this toast table does not have them, try the
		 * main rel
		 */
		relopts = extract_autovac_opts(tuple, pg_class_desc);
		if (relopts)
			free_relopts = true;
		else
		{
			av_relation *hentry;
			bool		found;

			hentry = hash_search(table_toast_map, &relid, HASH_FIND, &found);
			if (found && hentry->ar_hasrelopts)
				relopts = &hentry->ar_reloptions;
		}

		/* Fetch the pgstat entry for this table */
		tabentry = pgstat_fetch_stat_tabentry_ext(classForm->relisshared,
												  relid);

		relation_needs_vacanalyze(relid, relopts, classForm, tabentry,
								  effective_multixact_freeze_max_age,
								  &dovacuum, &doanalyze, &wraparound);

		/* ignore analyze for toast tables */
		if (dovacuum)
			table_oids = lappend_oid(table_oids, relid);

		/* Release stuff to avoid leakage */
		if (free_relopts)
			pfree(relopts);
		if (tabentry)
			pfree(tabentry);
	}

	table_endscan(relScan);
	table_close(classRel, AccessShareLock);

	/*
	 * Recheck orphan temporary tables, and if they still seem orphaned, drop
	 * them.  We'll eat a transaction per dropped table, which might seem
	 * excessive, but we should only need to do anything as a result of a
	 * previous backend crash, so this should not happen often enough to
	 * justify "optimizing".  Using separate transactions ensures that we
	 * don't bloat the lock table if there are many temp tables to be dropped,
	 * and it ensures that we don't lose work if a deletion attempt fails.
	 */
	foreach(cell, orphan_oids)
	{
		Oid			relid = lfirst_oid(cell);
		Form_pg_class classForm;
		ObjectAddress object;

		/*
		 * Check for user-requested abort.
		 */
		CHECK_FOR_INTERRUPTS();

		/*
		 * Try to lock the table.  If we can't get the lock immediately,
		 * somebody else is using (or dropping) the table, so it's not our
		 * concern anymore.  Having the lock prevents race conditions below.
		 */
		if (!ConditionalLockRelationOid(relid, AccessExclusiveLock))
			continue;

		/*
		 * Re-fetch the pg_class tuple and re-check whether it still seems to
		 * be an orphaned temp table.  If it's not there or no longer the same
		 * relation, ignore it.
		 */
		tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));
		if (!HeapTupleIsValid(tuple))
		{
			/* be sure to drop useless lock so we don't bloat lock table */
			UnlockRelationOid(relid, AccessExclusiveLock);
			continue;
		}
		classForm = (Form_pg_class) GETSTRUCT(tuple);

		/*
		 * Make all the same tests made in the loop above.  In event of OID
		 * counter wraparound, the pg_class entry we have now might be
		 * completely unrelated to the one we saw before.
		 */
		if (!((classForm->relkind == RELKIND_RELATION ||
			   classForm->relkind == RELKIND_MATVIEW) &&
			  classForm->relpersistence == RELPERSISTENCE_TEMP))
		{
			UnlockRelationOid(relid, AccessExclusiveLock);
			continue;
		}

		if (checkTempNamespaceStatus(classForm->relnamespace) != TEMP_NAMESPACE_IDLE)
		{
			UnlockRelationOid(relid, AccessExclusiveLock);
			continue;
		}

		/*
		 * Try to lock the temp namespace, too.  Even though we have lock on
		 * the table itself, there's a risk of deadlock against an incoming
		 * backend trying to clean out the temp namespace, in case this table
		 * has dependencies (such as sequences) that the backend's
		 * performDeletion call might visit in a different order.  If we can
		 * get AccessShareLock on the namespace, that's sufficient to ensure
		 * we're not running concurrently with RemoveTempRelations.  If we
		 * can't, back off and let RemoveTempRelations do its thing.
		 */
		if (!ConditionalLockDatabaseObject(NamespaceRelationId,
										   classForm->relnamespace, 0,
										   AccessShareLock))
		{
			UnlockRelationOid(relid, AccessExclusiveLock);
			continue;
		}

		/* OK, let's delete it */
		ereport(LOG,
				(errmsg("autovacuum: dropping orphan temp table \"%s.%s.%s\"",
						get_database_name(MyDatabaseId),
						get_namespace_name(classForm->relnamespace),
						NameStr(classForm->relname))));

		/*
		 * Deletion might involve TOAST table access, so ensure we have a
		 * valid snapshot.
		 */
		PushActiveSnapshot(GetTransactionSnapshot());

		object.classId = RelationRelationId;
		object.objectId = relid;
		object.objectSubId = 0;
		performDeletion(&object, DROP_CASCADE,
						PERFORM_DELETION_INTERNAL |
						PERFORM_DELETION_QUIETLY |
						PERFORM_DELETION_SKIP_EXTENSIONS);

		/*
		 * To commit the deletion, end current transaction and start a new
		 * one.  Note this also releases the locks we took.
		 */
		PopActiveSnapshot();
		CommitTransactionCommand();
		StartTransactionCommand();

		/* StartTransactionCommand changed current memory context */
		MemoryContextSwitchTo(AutovacMemCxt);
	}

	/*
	 * Optionally, create a buffer access strategy object for VACUUM to use.
	 * We use the same BufferAccessStrategy object for all tables VACUUMed by
	 * this worker to prevent autovacuum from blowing out shared buffers.
	 *
	 * VacuumBufferUsageLimit being set to 0 results in
	 * GetAccessStrategyWithSize returning NULL, effectively meaning we can
	 * use up to all of shared buffers.
	 *
	 * If we later enter failsafe mode on any of the tables being vacuumed, we
	 * will cease use of the BufferAccessStrategy only for that table.
	 *
	 * XXX should we consider adding code to adjust the size of this if
	 * VacuumBufferUsageLimit changes?
	 */
	bstrategy = GetAccessStrategyWithSize(BAS_VACUUM, VacuumBufferUsageLimit);

	/*
	 * create a memory context to act as fake PortalContext, so that the
	 * contexts created in the vacuum code are cleaned up for each table.
	 */
	PortalContext = AllocSetContextCreate(AutovacMemCxt,
										  "Autovacuum Portal",
										  ALLOCSET_DEFAULT_SIZES);

	/*
	 * Perform operations on collected tables.
	 */
	foreach(cell, table_oids)
	{
		Oid			relid = lfirst_oid(cell);
		HeapTuple	classTup;
		autovac_table *tab;
		bool		isshared;
		bool		skipit;
		dlist_iter	iter;

		CHECK_FOR_INTERRUPTS();

		/*
		 * Check for config changes before processing each collected table.
		 */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);

			/*
			 * You might be tempted to bail out if we see autovacuum is now
			 * disabled.  Must resist that temptation -- this might be a
			 * for-wraparound emergency worker, in which case that would be
			 * entirely inappropriate.
			 */
		}

		/*
		 * Find out whether the table is shared or not.  (It's slightly
		 * annoying to fetch the syscache entry just for this, but in typical
		 * cases it adds little cost because table_recheck_autovac would
		 * refetch the entry anyway.  We could buy that back by copying the
		 * tuple here and passing it to table_recheck_autovac, but that
		 * increases the odds of that function working with stale data.)
		 */
		classTup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
		if (!HeapTupleIsValid(classTup))
			continue;			/* somebody deleted the rel, forget it */
		isshared = ((Form_pg_class) GETSTRUCT(classTup))->relisshared;
		ReleaseSysCache(classTup);

		/*
		 * Hold schedule lock from here until we've claimed the table.  We
		 * also need the AutovacuumLock to walk the worker array, but that one
		 * can just be a shared lock.
		 */
		LWLockAcquire(AutovacuumScheduleLock, LW_EXCLUSIVE);
		LWLockAcquire(AutovacuumLock, LW_SHARED);

		/*
		 * Check whether the table is being vacuumed concurrently by another
		 * worker.
		 */
		skipit = false;
		dlist_foreach(iter, &AutoVacuumShmem->av_runningWorkers)
		{
			WorkerInfo	worker = dlist_container(WorkerInfoData, wi_links, iter.cur);

			/* ignore myself */
			if (worker == MyWorkerInfo)
				continue;

			/* ignore workers in other databases (unless table is shared) */
			if (!worker->wi_sharedrel && worker->wi_dboid != MyDatabaseId)
				continue;

			if (worker->wi_tableoid == relid)
			{
				skipit = true;
				found_concurrent_worker = true;
				break;
			}
		}
		LWLockRelease(AutovacuumLock);
		if (skipit)
		{
			LWLockRelease(AutovacuumScheduleLock);
			continue;
		}

		/*
		 * Store the table's OID in shared memory before releasing the
		 * schedule lock, so that other workers don't try to vacuum it
		 * concurrently.  (We claim it here so as not to hold
		 * AutovacuumScheduleLock while rechecking the stats.)
		 */
		MyWorkerInfo->wi_tableoid = relid;
		MyWorkerInfo->wi_sharedrel = isshared;
		LWLockRelease(AutovacuumScheduleLock);

		/*
		 * Check whether pgstat data still says we need to vacuum this table.
		 * It could have changed if something else processed the table while
		 * we weren't looking. This doesn't entirely close the race condition,
		 * but it is very small.
		 */
		MemoryContextSwitchTo(AutovacMemCxt);
		tab = table_recheck_autovac(relid, table_toast_map, pg_class_desc,
									effective_multixact_freeze_max_age);
		if (tab == NULL)
		{
			/* someone else vacuumed the table, or it went away */
			LWLockAcquire(AutovacuumScheduleLock, LW_EXCLUSIVE);
			MyWorkerInfo->wi_tableoid = InvalidOid;
			MyWorkerInfo->wi_sharedrel = false;
			LWLockRelease(AutovacuumScheduleLock);
			continue;
		}

		/*
		 * Save the cost-related storage parameter values in global variables
		 * for reference when updating vacuum_cost_delay and vacuum_cost_limit
		 * during vacuuming this table.
		 */
		av_storage_param_cost_delay = tab->at_storage_param_vac_cost_delay;
		av_storage_param_cost_limit = tab->at_storage_param_vac_cost_limit;

		/*
		 * We only expect this worker to ever set the flag, so don't bother
		 * checking the return value. We shouldn't have to retry.
		 */
		if (tab->at_dobalance)
			pg_atomic_test_set_flag(&MyWorkerInfo->wi_dobalance);
		else
			pg_atomic_clear_flag(&MyWorkerInfo->wi_dobalance);

		LWLockAcquire(AutovacuumLock, LW_SHARED);
		autovac_recalculate_workers_for_balance();
		LWLockRelease(AutovacuumLock);

		/*
		 * We wait until this point to update cost delay and cost limit
		 * values, even though we reloaded the configuration file above, so
		 * that we can take into account the cost-related storage parameters.
		 */
		VacuumUpdateCosts();


		/* clean up memory before each iteration */
		MemoryContextReset(PortalContext);

		/*
		 * Save the relation name for a possible error message, to avoid a
		 * catalog lookup in case of an error.  If any of these return NULL,
		 * then the relation has been dropped since last we checked; skip it.
		 * Note: they must live in a long-lived memory context because we call
		 * vacuum and analyze in different transactions.
		 */

		tab->at_relname = get_rel_name(tab->at_relid);
		tab->at_nspname = get_namespace_name(get_rel_namespace(tab->at_relid));
		tab->at_datname = get_database_name(MyDatabaseId);
		if (!tab->at_relname || !tab->at_nspname || !tab->at_datname)
			goto deleted;

		/*
		 * We will abort vacuuming the current table if something errors out,
		 * and continue with the next one in schedule; in particular, this
		 * happens if we are interrupted with SIGINT.
		 */
		PG_TRY();
		{
			/* Use PortalContext for any per-table allocations */
			MemoryContextSwitchTo(PortalContext);

			/* have at it */
			autovacuum_do_vac_analyze(tab, bstrategy);

			/*
			 * Clear a possible query-cancel signal, to avoid a late reaction
			 * to an automatically-sent signal because of vacuuming the
			 * current table (we're done with it, so it would make no sense to
			 * cancel at this point.)
			 */
			QueryCancelPending = false;
		}
		PG_CATCH();
		{
			/*
			 * Abort the transaction, start a new one, and proceed with the
			 * next table in our list.
			 */
			HOLD_INTERRUPTS();
			if (tab->at_params.options & VACOPT_VACUUM)
				errcontext("automatic vacuum of table \"%s.%s.%s\"",
						   tab->at_datname, tab->at_nspname, tab->at_relname);
			else
				errcontext("automatic analyze of table \"%s.%s.%s\"",
						   tab->at_datname, tab->at_nspname, tab->at_relname);
			EmitErrorReport();

			/* this resets ProcGlobal->statusFlags[i] too */
			AbortOutOfAnyTransaction();
			FlushErrorState();
			MemoryContextReset(PortalContext);

			/* restart our transaction for the following operations */
			StartTransactionCommand();
			RESUME_INTERRUPTS();
		}
		PG_END_TRY();

		/* Make sure we're back in AutovacMemCxt */
		MemoryContextSwitchTo(AutovacMemCxt);

		did_vacuum = true;

		/* ProcGlobal->statusFlags[i] are reset at the next end of xact */

		/* be tidy */
deleted:
		if (tab->at_datname != NULL)
			pfree(tab->at_datname);
		if (tab->at_nspname != NULL)
			pfree(tab->at_nspname);
		if (tab->at_relname != NULL)
			pfree(tab->at_relname);
		pfree(tab);

		/*
		 * Remove my info from shared memory.  We set wi_dobalance on the
		 * assumption that we are more likely than not to vacuum a table with
		 * no cost-related storage parameters next, so we want to claim our
		 * share of I/O as soon as possible to avoid thrashing the global
		 * balance.
		 */
		LWLockAcquire(AutovacuumScheduleLock, LW_EXCLUSIVE);
		MyWorkerInfo->wi_tableoid = InvalidOid;
		MyWorkerInfo->wi_sharedrel = false;
		LWLockRelease(AutovacuumScheduleLock);
		pg_atomic_test_set_flag(&MyWorkerInfo->wi_dobalance);
	}

	list_free(table_oids);

	/*
	 * Perform additional work items, as requested by backends.
	 */
	LWLockAcquire(AutovacuumLock, LW_EXCLUSIVE);
	for (i = 0; i < NUM_WORKITEMS; i++)
	{
		AutoVacuumWorkItem *workitem = &AutoVacuumShmem->av_workItems[i];

		if (!workitem->avw_used)
			continue;
		if (workitem->avw_active)
			continue;
		if (workitem->avw_database != MyDatabaseId)
			continue;

		/* claim this one, and release lock while performing it */
		workitem->avw_active = true;
		LWLockRelease(AutovacuumLock);

		perform_work_item(workitem);

		/*
		 * Check for config changes before acquiring lock for further jobs.
		 */
		CHECK_FOR_INTERRUPTS();
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
			VacuumUpdateCosts();
		}

		LWLockAcquire(AutovacuumLock, LW_EXCLUSIVE);

		/* and mark it done */
		workitem->avw_active = false;
		workitem->avw_used = false;
	}
	LWLockRelease(AutovacuumLock);

	/*
	 * We leak table_toast_map here (among other things), but since we're
	 * going away soon, it's not a problem.
	 */

	/*
	 * Update pg_database.datfrozenxid, and truncate pg_xact if possible. We
	 * only need to do this once, not after each table.
	 *
	 * Even if we didn't vacuum anything, it may still be important to do
	 * this, because one indirect effect of vac_update_datfrozenxid() is to
	 * update TransamVariables->xidVacLimit.  That might need to be done even
	 * if we haven't vacuumed anything, because relations with older
	 * relfrozenxid values or other databases with older datfrozenxid values
	 * might have been dropped, allowing xidVacLimit to advance.
	 *
	 * However, it's also important not to do this blindly in all cases,
	 * because when autovacuum=off this will restart the autovacuum launcher.
	 * If we're not careful, an infinite loop can result, where workers find
	 * no work to do and restart the launcher, which starts another worker in
	 * the same database that finds no work to do.  To prevent that, we skip
	 * this if (1) we found no work to do and (2) we skipped at least one
	 * table due to concurrent autovacuum activity.  In that case, the other
	 * worker has already done it, or will do so when it finishes.
	 */
	if (did_vacuum || !found_concurrent_worker)
		vac_update_datfrozenxid();

	/* Finally close out the last transaction. */
	CommitTransactionCommand();
}

/*
 * Execute a previously registered work item.
 */
static void
perform_work_item(AutoVacuumWorkItem *workitem)
{
	char	   *cur_datname = NULL;
	char	   *cur_nspname = NULL;
	char	   *cur_relname = NULL;

	/*
	 * Note we do not store table info in MyWorkerInfo, since this is not
	 * vacuuming proper.
	 */

	/*
	 * Save the relation name for a possible error message, to avoid a catalog
	 * lookup in case of an error.  If any of these return NULL, then the
	 * relation has been dropped since last we checked; skip it.
	 */
	Assert(CurrentMemoryContext == AutovacMemCxt);

	cur_relname = get_rel_name(workitem->avw_relation);
	cur_nspname = get_namespace_name(get_rel_namespace(workitem->avw_relation));
	cur_datname = get_database_name(MyDatabaseId);
	if (!cur_relname || !cur_nspname || !cur_datname)
		goto deleted2;

	autovac_report_workitem(workitem, cur_nspname, cur_relname);

	/* clean up memory before each work item */
	MemoryContextReset(PortalContext);

	/*
	 * We will abort the current work item if something errors out, and
	 * continue with the next one; in particular, this happens if we are
	 * interrupted with SIGINT.  Note that this means that the work item list
	 * can be lossy.
	 */
	PG_TRY();
	{
		/* Use PortalContext for any per-work-item allocations */
		MemoryContextSwitchTo(PortalContext);

		/*
		 * Have at it.  Functions called here are responsible for any required
		 * user switch and sandbox.
		 */
		switch (workitem->avw_type)
		{
			case AVW_BRINSummarizeRange:
				DirectFunctionCall2(brin_summarize_range,
									ObjectIdGetDatum(workitem->avw_relation),
									Int64GetDatum((int64) workitem->avw_blockNumber));
				break;
			default:
				elog(WARNING, "unrecognized work item found: type %d",
					 workitem->avw_type);
				break;
		}

		/*
		 * Clear a possible query-cancel signal, to avoid a late reaction to
		 * an automatically-sent signal because of vacuuming the current table
		 * (we're done with it, so it would make no sense to cancel at this
		 * point.)
		 */
		QueryCancelPending = false;
	}
	PG_CATCH();
	{
		/*
		 * Abort the transaction, start a new one, and proceed with the next
		 * table in our list.
		 */
		HOLD_INTERRUPTS();
		errcontext("processing work entry for relation \"%s.%s.%s\"",
				   cur_datname, cur_nspname, cur_relname);
		EmitErrorReport();

		/* this resets ProcGlobal->statusFlags[i] too */
		AbortOutOfAnyTransaction();
		FlushErrorState();
		MemoryContextReset(PortalContext);

		/* restart our transaction for the following operations */
		StartTransactionCommand();
		RESUME_INTERRUPTS();
	}
	PG_END_TRY();

	/* Make sure we're back in AutovacMemCxt */
	MemoryContextSwitchTo(AutovacMemCxt);

	/* We intentionally do not set did_vacuum here */

	/* be tidy */
deleted2:
	if (cur_datname)
		pfree(cur_datname);
	if (cur_nspname)
		pfree(cur_nspname);
	if (cur_relname)
		pfree(cur_relname);
}

/*
 * extract_autovac_opts
 *
 * Given a relation's pg_class tuple, return a palloc'd copy of the
 * AutoVacOpts portion of reloptions, if set; otherwise, return NULL.
 *
 * Note: callers do not have a relation lock on the table at this point,
 * so the table could have been dropped, and its catalog rows gone, after
 * we acquired the pg_class row.  If pg_class had a TOAST table, this would
 * be a risk; fortunately, it doesn't.
 */
static AutoVacOpts *
extract_autovac_opts(HeapTuple tup, TupleDesc pg_class_desc)
{
	bytea	   *relopts;
	AutoVacOpts *av;

	Assert(((Form_pg_class) GETSTRUCT(tup))->relkind == RELKIND_RELATION ||
		   ((Form_pg_class) GETSTRUCT(tup))->relkind == RELKIND_MATVIEW ||
		   ((Form_pg_class) GETSTRUCT(tup))->relkind == RELKIND_TOASTVALUE);

	relopts = extractRelOptions(tup, pg_class_desc, NULL);
	if (relopts == NULL)
		return NULL;

	av = palloc(sizeof(AutoVacOpts));
	memcpy(av, &(((StdRdOptions *) relopts)->autovacuum), sizeof(AutoVacOpts));
	pfree(relopts);

	return av;
}


/*
 * table_recheck_autovac
 *
 * Recheck whether a table still needs vacuum or analyze.  Return value is a
 * valid autovac_table pointer if it does, NULL otherwise.
 *
 * Note that the returned autovac_table does not have the name fields set.
 */
static autovac_table *
table_recheck_autovac(Oid relid, HTAB *table_toast_map,
					  TupleDesc pg_class_desc,
					  int effective_multixact_freeze_max_age)
{
	Form_pg_class classForm;
	HeapTuple	classTup;
	bool		dovacuum;
	bool		doanalyze;
	autovac_table *tab = NULL;
	bool		wraparound;
	AutoVacOpts *avopts;
	bool		free_avopts = false;

	/* fetch the relation's relcache entry */
	classTup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(classTup))
		return NULL;
	classForm = (Form_pg_class) GETSTRUCT(classTup);

	/*
	 * Get the applicable reloptions.  If it is a TOAST table, try to get the
	 * main table reloptions if the toast table itself doesn't have.
	 */
	avopts = extract_autovac_opts(classTup, pg_class_desc);
	if (avopts)
		free_avopts = true;
	else if (classForm->relkind == RELKIND_TOASTVALUE &&
			 table_toast_map != NULL)
	{
		av_relation *hentry;
		bool		found;

		hentry = hash_search(table_toast_map, &relid, HASH_FIND, &found);
		if (found && hentry->ar_hasrelopts)
			avopts = &hentry->ar_reloptions;
	}

	recheck_relation_needs_vacanalyze(relid, avopts, classForm,
									  effective_multixact_freeze_max_age,
									  &dovacuum, &doanalyze, &wraparound);

	/* OK, it needs something done */
	if (doanalyze || dovacuum)
	{
		int			freeze_min_age;
		int			freeze_table_age;
		int			multixact_freeze_min_age;
		int			multixact_freeze_table_age;
		int			log_min_duration;

		/*
		 * Calculate the vacuum cost parameters and the freeze ages.  If there
		 * are options set in pg_class.reloptions, use them; in the case of a
		 * toast table, try the main table too.  Otherwise use the GUC
		 * defaults, autovacuum's own first and plain vacuum second.
		 */

		/* -1 in autovac setting means use log_autovacuum_min_duration */
		log_min_duration = (avopts && avopts->log_min_duration >= 0)
			? avopts->log_min_duration
			: Log_autovacuum_min_duration;

		/* these do not have autovacuum-specific settings */
		freeze_min_age = (avopts && avopts->freeze_min_age >= 0)
			? avopts->freeze_min_age
			: default_freeze_min_age;

		freeze_table_age = (avopts && avopts->freeze_table_age >= 0)
			? avopts->freeze_table_age
			: default_freeze_table_age;

		multixact_freeze_min_age = (avopts &&
									avopts->multixact_freeze_min_age >= 0)
			? avopts->multixact_freeze_min_age
			: default_multixact_freeze_min_age;

		multixact_freeze_table_age = (avopts &&
									  avopts->multixact_freeze_table_age >= 0)
			? avopts->multixact_freeze_table_age
			: default_multixact_freeze_table_age;

		tab = palloc(sizeof(autovac_table));
		tab->at_relid = relid;
		tab->at_sharedrel = classForm->relisshared;

		/*
		 * Select VACUUM options.  Note we don't say VACOPT_PROCESS_TOAST, so
		 * that vacuum() skips toast relations.  Also note we tell vacuum() to
		 * skip vac_update_datfrozenxid(); we'll do that separately.
		 */
		tab->at_params.options =
			(dovacuum ? (VACOPT_VACUUM |
						 VACOPT_PROCESS_MAIN |
						 VACOPT_SKIP_DATABASE_STATS) : 0) |
			(doanalyze ? VACOPT_ANALYZE : 0) |
			(!wraparound ? VACOPT_SKIP_LOCKED : 0);

		/*
		 * index_cleanup and truncate are unspecified at first in autovacuum.
		 * They will be filled in with usable values using their reloptions
		 * (or reloption defaults) later.
		 */
		tab->at_params.index_cleanup = VACOPTVALUE_UNSPECIFIED;
		tab->at_params.truncate = VACOPTVALUE_UNSPECIFIED;
		/* As of now, we don't support parallel vacuum for autovacuum */
		tab->at_params.nworkers = -1;
		tab->at_params.freeze_min_age = freeze_min_age;
		tab->at_params.freeze_table_age = freeze_table_age;
		tab->at_params.multixact_freeze_min_age = multixact_freeze_min_age;
		tab->at_params.multixact_freeze_table_age = multixact_freeze_table_age;
		tab->at_params.is_wraparound = wraparound;
		tab->at_params.log_min_duration = log_min_duration;
		tab->at_params.toast_parent = InvalidOid;

		/*
		 * Later, in vacuum_rel(), we check reloptions for any
		 * vacuum_max_eager_freeze_failure_rate override.
		 */
		tab->at_params.max_eager_freeze_failure_rate = vacuum_max_eager_freeze_failure_rate;
		tab->at_storage_param_vac_cost_limit = avopts ?
			avopts->vacuum_cost_limit : 0;
		tab->at_storage_param_vac_cost_delay = avopts ?
			avopts->vacuum_cost_delay : -1;
		tab->at_relname = NULL;
		tab->at_nspname = NULL;
		tab->at_datname = NULL;

		/*
		 * If any of the cost delay parameters has been set individually for
		 * this table, disable the balancing algorithm.
		 */
		tab->at_dobalance =
			!(avopts && (avopts->vacuum_cost_limit > 0 ||
						 avopts->vacuum_cost_delay >= 0));
	}

	if (free_avopts)
		pfree(avopts);
	heap_freetuple(classTup);
	return tab;
}

/*
 * recheck_relation_needs_vacanalyze
 *
 * Subroutine for table_recheck_autovac.
 *
 * Fetch the pgstat of a relation and recheck whether a relation
 * needs to be vacuumed or analyzed.
 */
static void
recheck_relation_needs_vacanalyze(Oid relid,
								  AutoVacOpts *avopts,
								  Form_pg_class classForm,
								  int effective_multixact_freeze_max_age,
								  bool *dovacuum,
								  bool *doanalyze,
								  bool *wraparound)
{
	PgStat_StatTabEntry *tabentry;

	/* fetch the pgstat table entry */
	tabentry = pgstat_fetch_stat_tabentry_ext(classForm->relisshared,
											  relid);

	relation_needs_vacanalyze(relid, avopts, classForm, tabentry,
							  effective_multixact_freeze_max_age,
							  dovacuum, doanalyze, wraparound);

	/* Release tabentry to avoid leakage */
	if (tabentry)
		pfree(tabentry);

	/* ignore ANALYZE for toast tables */
	if (classForm->relkind == RELKIND_TOASTVALUE)
		*doanalyze = false;
}

/*
 * relation_needs_vacanalyze
 *
 * Check whether a relation needs to be vacuumed or analyzed; return each into
 * "dovacuum" and "doanalyze", respectively.  Also return whether the vacuum is
 * being forced because of Xid or multixact wraparound.
 *
 * relopts is a pointer to the AutoVacOpts options (either for itself in the
 * case of a plain table, or for either itself or its parent table in the case
 * of a TOAST table), NULL if none; tabentry is the pgstats entry, which can be
 * NULL.
 *
 * A table needs to be vacuumed if the number of dead tuples exceeds a
 * threshold.  This threshold is calculated as
 *
 * threshold = vac_base_thresh + vac_scale_factor * reltuples
 * if (threshold > vac_max_thresh)
 *     threshold = vac_max_thresh;
 *
 * For analyze, the analysis done is that the number of tuples inserted,
 * deleted and updated since the last analyze exceeds a threshold calculated
 * in the same fashion as above.  Note that the cumulative stats system stores
 * the number of tuples (both live and dead) that there were as of the last
 * analyze.  This is asymmetric to the VACUUM case.
 *
 * We also force vacuum if the table's relfrozenxid is more than freeze_max_age
 * transactions back, and if its relminmxid is more than
 * multixact_freeze_max_age multixacts back.
 *
 * A table whose autovacuum_enabled option is false is
 * automatically skipped (unless we have to vacuum it due to freeze_max_age).
 * Thus autovacuum can be disabled for specific tables. Also, when the cumulative
 * stats system does not have data about a table, it will be skipped.
 *
 * A table whose vac_base_thresh value is < 0 takes the base value from the
 * autovacuum_vacuum_threshold GUC variable.  Similarly, a vac_scale_factor
 * value < 0 is substituted with the value of
 * autovacuum_vacuum_scale_factor GUC variable.  Ditto for analyze.
 */
static void
relation_needs_vacanalyze(Oid relid,
						  AutoVacOpts *relopts,
						  Form_pg_class classForm,
						  PgStat_StatTabEntry *tabentry,
						  int effective_multixact_freeze_max_age,
 /* output params below */
						  bool *dovacuum,
						  bool *doanalyze,
						  bool *wraparound)
{
	bool		force_vacuum;
	bool		av_enabled;

	/* constants from reloptions or GUC variables */
	int			vac_base_thresh,
				vac_max_thresh,
				vac_ins_base_thresh,
				anl_base_thresh;
	float4		vac_scale_factor,
				vac_ins_scale_factor,
				anl_scale_factor;

	/* thresholds calculated from above constants */
	float4		vacthresh,
				vacinsthresh,
				anlthresh;

	/* number of vacuum (resp. analyze) tuples at this time */
	float4		vactuples,
				instuples,
				anltuples;

	/* freeze parameters */
	int			freeze_max_age;
	int			multixact_freeze_max_age;
	TransactionId xidForceLimit;
	TransactionId relfrozenxid;
	MultiXactId multiForceLimit;

	Assert(classForm != NULL);
	Assert(OidIsValid(relid));

	/*
	 * Determine vacuum/analyze equation parameters.  We have two possible
	 * sources: the passed reloptions (which could be a main table or a toast
	 * table), or the autovacuum GUC variables.
	 */

	/* -1 in autovac setting means use plain vacuum_scale_factor */
	vac_scale_factor = (relopts && relopts->vacuum_scale_factor >= 0)
		? relopts->vacuum_scale_factor
		: autovacuum_vac_scale;

	vac_base_thresh = (relopts && relopts->vacuum_threshold >= 0)
		? relopts->vacuum_threshold
		: autovacuum_vac_thresh;

	/* -1 is used to disable max threshold */
	vac_max_thresh = (relopts && relopts->vacuum_max_threshold >= -1)
		? relopts->vacuum_max_threshold
		: autovacuum_vac_max_thresh;

	vac_ins_scale_factor = (relopts && relopts->vacuum_ins_scale_factor >= 0)
		? relopts->vacuum_ins_scale_factor
		: autovacuum_vac_ins_scale;

	/* -1 is used to disable insert vacuums */
	vac_ins_base_thresh = (relopts && relopts->vacuum_ins_threshold >= -1)
		? relopts->vacuum_ins_threshold
		: autovacuum_vac_ins_thresh;

	anl_scale_factor = (relopts && relopts->analyze_scale_factor >= 0)
		? relopts->analyze_scale_factor
		: autovacuum_anl_scale;

	anl_base_thresh = (relopts && relopts->analyze_threshold >= 0)
		? relopts->analyze_threshold
		: autovacuum_anl_thresh;

	freeze_max_age = (relopts && relopts->freeze_max_age >= 0)
		? Min(relopts->freeze_max_age, autovacuum_freeze_max_age)
		: autovacuum_freeze_max_age;

	multixact_freeze_max_age = (relopts && relopts->multixact_freeze_max_age >= 0)
		? Min(relopts->multixact_freeze_max_age, effective_multixact_freeze_max_age)
		: effective_multixact_freeze_max_age;

	av_enabled = (relopts ? relopts->enabled : true);

	/* Force vacuum if table is at risk of wraparound */
	xidForceLimit = recentXid - freeze_max_age;
	if (xidForceLimit < FirstNormalTransactionId)
		xidForceLimit -= FirstNormalTransactionId;
	relfrozenxid = classForm->relfrozenxid;
	force_vacuum = (TransactionIdIsNormal(relfrozenxid) &&
					TransactionIdPrecedes(relfrozenxid, xidForceLimit));
	if (!force_vacuum)
	{
		MultiXactId relminmxid = classForm->relminmxid;

		multiForceLimit = recentMulti - multixact_freeze_max_age;
		if (multiForceLimit < FirstMultiXactId)
			multiForceLimit -= FirstMultiXactId;
		force_vacuum = MultiXactIdIsValid(relminmxid) &&
			MultiXactIdPrecedes(relminmxid, multiForceLimit);
	}
	*wraparound = force_vacuum;

	/* User disabled it in pg_class.reloptions?  (But ignore if at risk) */
	if (!av_enabled && !force_vacuum)
	{
		*doanalyze = false;
		*dovacuum = false;
		return;
	}

	/*
	 * If we found stats for the table, and autovacuum is currently enabled,
	 * make a threshold-based decision whether to vacuum and/or analyze.  If
	 * autovacuum is currently disabled, we must be here for anti-wraparound
	 * vacuuming only, so don't vacuum (or analyze) anything that's not being
	 * forced.
	 */
	if (PointerIsValid(tabentry) && AutoVacuumingActive())
	{
		float4		pcnt_unfrozen = 1;
		float4		reltuples = classForm->reltuples;
		int32		relpages = classForm->relpages;
		int32		relallfrozen = classForm->relallfrozen;

		vactuples = tabentry->dead_tuples;
		instuples = tabentry->ins_since_vacuum;
		anltuples = tabentry->mod_since_analyze;

		/* If the table hasn't yet been vacuumed, take reltuples as zero */
		if (reltuples < 0)
			reltuples = 0;

		/*
		 * If we have data for relallfrozen, calculate the unfrozen percentage
		 * of the table to modify insert scale factor. This helps us decide
		 * whether or not to vacuum an insert-heavy table based on the number
		 * of inserts to the more "active" part of the table.
		 */
		if (relpages > 0 && relallfrozen > 0)
		{
			/*
			 * It could be the stats were updated manually and relallfrozen >
			 * relpages. Clamp relallfrozen to relpages to avoid nonsensical
			 * calculations.
			 */
			relallfrozen = Min(relallfrozen, relpages);
			pcnt_unfrozen = 1 - ((float4) relallfrozen / relpages);
		}

		vacthresh = (float4) vac_base_thresh + vac_scale_factor * reltuples;
		if (vac_max_thresh >= 0 && vacthresh > (float4) vac_max_thresh)
			vacthresh = (float4) vac_max_thresh;

		vacinsthresh = (float4) vac_ins_base_thresh +
			vac_ins_scale_factor * reltuples * pcnt_unfrozen;
		anlthresh = (float4) anl_base_thresh + anl_scale_factor * reltuples;

		/*
		 * Note that we don't need to take special consideration for stat
		 * reset, because if that happens, the last vacuum and analyze counts
		 * will be reset too.
		 */
		if (vac_ins_base_thresh >= 0)
			elog(DEBUG3, "%s: vac: %.0f (threshold %.0f), ins: %.0f (threshold %.0f), anl: %.0f (threshold %.0f)",
				 NameStr(classForm->relname),
				 vactuples, vacthresh, instuples, vacinsthresh, anltuples, anlthresh);
		else
			elog(DEBUG3, "%s: vac: %.0f (threshold %.0f), ins: (disabled), anl: %.0f (threshold %.0f)",
				 NameStr(classForm->relname),
				 vactuples, vacthresh, anltuples, anlthresh);

		/* Determine if this table needs vacuum or analyze. */
		*dovacuum = force_vacuum || (vactuples > vacthresh) ||
			(vac_ins_base_thresh >= 0 && instuples > vacinsthresh);
		*doanalyze = (anltuples > anlthresh);
	}
	else
	{
		/*
		 * Skip a table not found in stat hash, unless we have to force vacuum
		 * for anti-wrap purposes.  If it's not acted upon, there's no need to
		 * vacuum it.
		 */
		*dovacuum = force_vacuum;
		*doanalyze = false;
	}

	/* ANALYZE refuses to work with pg_statistic */
	if (relid == StatisticRelationId)
		*doanalyze = false;
}

/*
 * autovacuum_do_vac_analyze
 *		Vacuum and/or analyze the specified table
 *
 * We expect the caller to have switched into a memory context that won't
 * disappear at transaction commit.
 */
static void
autovacuum_do_vac_analyze(autovac_table *tab, BufferAccessStrategy bstrategy)
{
	RangeVar   *rangevar;
	VacuumRelation *rel;
	List	   *rel_list;
	MemoryContext vac_context;
	MemoryContext old_context;

	/* Let pgstat know what we're doing */
	autovac_report_activity(tab);

	/* Create a context that vacuum() can use as cross-transaction storage */
	vac_context = AllocSetContextCreate(CurrentMemoryContext,
										"Vacuum",
										ALLOCSET_DEFAULT_SIZES);

	/* Set up one VacuumRelation target, identified by OID, for vacuum() */
	old_context = MemoryContextSwitchTo(vac_context);
	rangevar = makeRangeVar(tab->at_nspname, tab->at_relname, -1);
	rel = makeVacuumRelation(rangevar, tab->at_relid, NIL);
	rel_list = list_make1(rel);
	MemoryContextSwitchTo(old_context);

	vacuum(rel_list, &tab->at_params, bstrategy, vac_context, true);

	MemoryContextDelete(vac_context);
}

/*
 * autovac_report_activity
 *		Report to pgstat what autovacuum is doing
 *
 * We send a SQL string corresponding to what the user would see if the
 * equivalent command was to be issued manually.
 *
 * Note we assume that we are going to report the next command as soon as we're
 * done with the current one, and exit right after the last one, so we don't
 * bother to report "<IDLE>" or some such.
 */
static void
autovac_report_activity(autovac_table *tab)
{
#define MAX_AUTOVAC_ACTIV_LEN (NAMEDATALEN * 2 + 56)
	char		activity[MAX_AUTOVAC_ACTIV_LEN];
	int			len;

	/* Report the command and possible options */
	if (tab->at_params.options & VACOPT_VACUUM)
		snprintf(activity, MAX_AUTOVAC_ACTIV_LEN,
				 "autovacuum: VACUUM%s",
				 tab->at_params.options & VACOPT_ANALYZE ? " ANALYZE" : "");
	else
		snprintf(activity, MAX_AUTOVAC_ACTIV_LEN,
				 "autovacuum: ANALYZE");

	/*
	 * Report the qualified name of the relation.
	 */
	len = strlen(activity);

	snprintf(activity + len, MAX_AUTOVAC_ACTIV_LEN - len,
			 " %s.%s%s", tab->at_nspname, tab->at_relname,
			 tab->at_params.is_wraparound ? " (to prevent wraparound)" : "");

	/* Set statement_timestamp() to current time for pg_stat_activity */
	SetCurrentStatementStartTimestamp();

	pgstat_report_activity(STATE_RUNNING, activity);
}

/*
 * autovac_report_workitem
 *		Report to pgstat that autovacuum is processing a work item
 */
static void
autovac_report_workitem(AutoVacuumWorkItem *workitem,
						const char *nspname, const char *relname)
{
	char		activity[MAX_AUTOVAC_ACTIV_LEN + 12 + 2];
	char		blk[12 + 2];
	int			len;

	switch (workitem->avw_type)
	{
		case AVW_BRINSummarizeRange:
			snprintf(activity, MAX_AUTOVAC_ACTIV_LEN,
					 "autovacuum: BRIN summarize");
			break;
	}

	/*
	 * Report the qualified name of the relation, and the block number if any
	 */
	len = strlen(activity);

	if (BlockNumberIsValid(workitem->avw_blockNumber))
		snprintf(blk, sizeof(blk), " %u", workitem->avw_blockNumber);
	else
		blk[0] = '\0';

	snprintf(activity + len, MAX_AUTOVAC_ACTIV_LEN - len,
			 " %s.%s%s", nspname, relname, blk);

	/* Set statement_timestamp() to current time for pg_stat_activity */
	SetCurrentStatementStartTimestamp();

	pgstat_report_activity(STATE_RUNNING, activity);
}

/*
 * AutoVacuumingActive
 *		Check GUC vars and report whether the autovacuum process should be
 *		running.
 */
bool
AutoVacuumingActive(void)
{
	if (!autovacuum_start_daemon || !pgstat_track_counts)
		return false;
	return true;
}

/*
 * Request one work item to the next autovacuum run processing our database.
 * Return false if the request can't be recorded.
 */
bool
AutoVacuumRequestWork(AutoVacuumWorkItemType type, Oid relationId,
					  BlockNumber blkno)
{
	int			i;
	bool		result = false;

	LWLockAcquire(AutovacuumLock, LW_EXCLUSIVE);

	/*
	 * Locate an unused work item and fill it with the given data.
	 */
	for (i = 0; i < NUM_WORKITEMS; i++)
	{
		AutoVacuumWorkItem *workitem = &AutoVacuumShmem->av_workItems[i];

		if (workitem->avw_used)
			continue;

		workitem->avw_used = true;
		workitem->avw_active = false;
		workitem->avw_type = type;
		workitem->avw_database = MyDatabaseId;
		workitem->avw_relation = relationId;
		workitem->avw_blockNumber = blkno;
		result = true;

		/* done */
		break;
	}

	LWLockRelease(AutovacuumLock);

	return result;
}

/*
 * autovac_init
 *		This is called at postmaster initialization.
 *
 * All we do here is annoy the user if he got it wrong.
 */
void
autovac_init(void)
{
	if (!autovacuum_start_daemon)
		return;
	else if (!pgstat_track_counts)
		ereport(WARNING,
				(errmsg("autovacuum not started because of misconfiguration"),
				 errhint("Enable the \"track_counts\" option.")));
	else
		check_av_worker_gucs();
}

/*
 * AutoVacuumShmemSize
 *		Compute space needed for autovacuum-related shared memory
 */
Size
AutoVacuumShmemSize(void)
{
	Size		size;

	/*
	 * Need the fixed struct and the array of WorkerInfoData.
	 */
	size = sizeof(AutoVacuumShmemStruct);
	size = MAXALIGN(size);
	size = add_size(size, mul_size(autovacuum_worker_slots,
								   sizeof(WorkerInfoData)));
	return size;
}

/*
 * AutoVacuumShmemInit
 *		Allocate and initialize autovacuum-related shared memory
 */
void
AutoVacuumShmemInit(void)
{
	bool		found;

	AutoVacuumShmem = (AutoVacuumShmemStruct *)
		ShmemInitStruct("AutoVacuum Data",
						AutoVacuumShmemSize(),
						&found);

	if (!IsUnderPostmaster)
	{
		WorkerInfo	worker;
		int			i;

		Assert(!found);

		AutoVacuumShmem->av_launcherpid = 0;
		dclist_init(&AutoVacuumShmem->av_freeWorkers);
		dlist_init(&AutoVacuumShmem->av_runningWorkers);
		AutoVacuumShmem->av_startingWorker = NULL;
		memset(AutoVacuumShmem->av_workItems, 0,
			   sizeof(AutoVacuumWorkItem) * NUM_WORKITEMS);

		worker = (WorkerInfo) ((char *) AutoVacuumShmem +
							   MAXALIGN(sizeof(AutoVacuumShmemStruct)));

		/* initialize the WorkerInfo free list */
		for (i = 0; i < autovacuum_worker_slots; i++)
		{
			dclist_push_head(&AutoVacuumShmem->av_freeWorkers,
							 &worker[i].wi_links);
			pg_atomic_init_flag(&worker[i].wi_dobalance);
		}

		pg_atomic_init_u32(&AutoVacuumShmem->av_nworkersForBalance, 0);

	}
	else
		Assert(found);
}

/*
 * GUC check_hook for autovacuum_work_mem
 */
bool
check_autovacuum_work_mem(int *newval, void **extra, GucSource source)
{
	/*
	 * -1 indicates fallback.
	 *
	 * If we haven't yet changed the boot_val default of -1, just let it be.
	 * Autovacuum will look to maintenance_work_mem instead.
	 */
	if (*newval == -1)
		return true;

	/*
	 * We clamp manually-set values to at least 64kB.  Since
	 * maintenance_work_mem is always set to at least this value, do the same
	 * here.
	 */
	if (*newval < 64)
		*newval = 64;

	return true;
}

/*
 * Returns whether there is a free autovacuum worker slot available.
 */
static bool
av_worker_available(void)
{
	int			free_slots;
	int			reserved_slots;

	free_slots = dclist_count(&AutoVacuumShmem->av_freeWorkers);

	reserved_slots = autovacuum_worker_slots - autovacuum_max_workers;
	reserved_slots = Max(0, reserved_slots);

	return free_slots > reserved_slots;
}

/*
 * Emits a WARNING if autovacuum_worker_slots < autovacuum_max_workers.
 */
static void
check_av_worker_gucs(void)
{
	if (autovacuum_worker_slots < autovacuum_max_workers)
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"autovacuum_max_workers\" (%d) should be less than or equal to \"autovacuum_worker_slots\" (%d)",
						autovacuum_max_workers, autovacuum_worker_slots),
				 errdetail("The server will only start up to \"autovacuum_worker_slots\" (%d) autovacuum workers at a given time.",
						   autovacuum_worker_slots)));
}
