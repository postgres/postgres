/* -------------------------------------------------------------------------
 *
 * worker_spi.c
 *		Sample background worker code that demonstrates various coding
 *		patterns: establishing a database connection; starting and committing
 *		transactions; using GUC variables, and heeding SIGHUP to reread
 *		the configuration file; reporting to pg_stat_activity; using the
 *		process latch to sleep and exit in case of postmaster death.
 *
 * This code connects to a database, creates a schema and table, and summarizes
 * the numbers contained therein.  To see it working, insert an initial value
 * with "total" type and some initial value; then insert some other rows with
 * "delta" type.  Delta rows will be deleted by this worker and their values
 * aggregated into the total.
 *
 * Copyright (c) 2013-2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/worker_spi/worker_spi.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

/* These are always necessary for a bgworker */
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"

/* these headers are used by this particular worker's code */
#include "access/xact.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "pgstat.h"
#include "utils/builtins.h"
#include "utils/snapmgr.h"
#include "tcop/utility.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(worker_spi_launch);

void		_PG_init(void);
void		worker_spi_main(Datum) pg_attribute_noreturn();

/* flags set by signal handlers */
static volatile sig_atomic_t got_sighup = false;
static volatile sig_atomic_t got_sigterm = false;

/* GUC variables */
static int	worker_spi_naptime = 10;
static int	worker_spi_total_workers = 2;
static char *worker_spi_database = NULL;


typedef struct worktable
{
	const char *schema;
	const char *name;
} worktable;

/*
 * Signal handler for SIGTERM
 *		Set a flag to let the main loop to terminate, and set our latch to wake
 *		it up.
 */
static void
worker_spi_sigterm(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_sigterm = true;
	SetLatch(MyLatch);

	errno = save_errno;
}

/*
 * Signal handler for SIGHUP
 *		Set a flag to tell the main loop to reread the config file, and set
 *		our latch to wake it up.
 */
static void
worker_spi_sighup(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_sighup = true;
	SetLatch(MyLatch);

	errno = save_errno;
}

/*
 * Initialize workspace for a worker process: create the schema if it doesn't
 * already exist.
 */
static void
initialize_worker_spi(worktable *table)
{
	int			ret;
	int			ntup;
	bool		isnull;
	StringInfoData buf;

	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	SPI_connect();
	PushActiveSnapshot(GetTransactionSnapshot());
	pgstat_report_activity(STATE_RUNNING, "initializing worker_spi schema");

	/* XXX could we use CREATE SCHEMA IF NOT EXISTS? */
	initStringInfo(&buf);
	appendStringInfo(&buf, "select count(*) from pg_namespace where nspname = '%s'",
					 table->schema);

	ret = SPI_execute(buf.data, true, 0);
	if (ret != SPI_OK_SELECT)
		elog(FATAL, "SPI_execute failed: error code %d", ret);

	if (SPI_processed != 1)
		elog(FATAL, "not a singleton result");

	ntup = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0],
									   SPI_tuptable->tupdesc,
									   1, &isnull));
	if (isnull)
		elog(FATAL, "null result");

	if (ntup == 0)
	{
		resetStringInfo(&buf);
		appendStringInfo(&buf,
						 "CREATE SCHEMA \"%s\" "
						 "CREATE TABLE \"%s\" ("
						 "		type text CHECK (type IN ('total', 'delta')), "
						 "		value	integer)"
						 "CREATE UNIQUE INDEX \"%s_unique_total\" ON \"%s\" (type) "
						 "WHERE type = 'total'",
						 table->schema, table->name, table->name, table->name);

		/* set statement start time */
		SetCurrentStatementStartTimestamp();

		ret = SPI_execute(buf.data, false, 0);

		if (ret != SPI_OK_UTILITY)
			elog(FATAL, "failed to create my schema");
	}

	SPI_finish();
	PopActiveSnapshot();
	CommitTransactionCommand();
	pgstat_report_activity(STATE_IDLE, NULL);
}

void
worker_spi_main(Datum main_arg)
{
	int			index = DatumGetInt32(main_arg);
	worktable  *table;
	StringInfoData buf;
	char		name[20];

	table = palloc(sizeof(worktable));
	sprintf(name, "schema%d", index);
	table->schema = pstrdup(name);
	table->name = pstrdup("counted");

	/* Establish signal handlers before unblocking signals. */
	pqsignal(SIGHUP, worker_spi_sighup);
	pqsignal(SIGTERM, worker_spi_sigterm);

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	/* Connect to our database */
	BackgroundWorkerInitializeConnection(worker_spi_database, NULL, 0);

	elog(LOG, "%s initialized with %s.%s",
		 MyBgworkerEntry->bgw_name, table->schema, table->name);
	initialize_worker_spi(table);

	/*
	 * Quote identifiers passed to us.  Note that this must be done after
	 * initialize_worker_spi, because that routine assumes the names are not
	 * quoted.
	 *
	 * Note some memory might be leaked here.
	 */
	table->schema = quote_identifier(table->schema);
	table->name = quote_identifier(table->name);

	initStringInfo(&buf);
	appendStringInfo(&buf,
					 "WITH deleted AS (DELETE "
					 "FROM %s.%s "
					 "WHERE type = 'delta' RETURNING value), "
					 "total AS (SELECT coalesce(sum(value), 0) as sum "
					 "FROM deleted) "
					 "UPDATE %s.%s "
					 "SET value = %s.value + total.sum "
					 "FROM total WHERE type = 'total' "
					 "RETURNING %s.value",
					 table->schema, table->name,
					 table->schema, table->name,
					 table->name,
					 table->name);

	/*
	 * Main loop: do this until the SIGTERM handler tells us to terminate
	 */
	while (!got_sigterm)
	{
		int			ret;

		/*
		 * Background workers mustn't call usleep() or any direct equivalent:
		 * instead, they may wait on their process latch, which sleeps as
		 * necessary, but is awakened if postmaster dies.  That way the
		 * background process goes away immediately in an emergency.
		 */
		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 worker_spi_naptime * 1000L,
						 PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);

		CHECK_FOR_INTERRUPTS();

		/*
		 * In case of a SIGHUP, just reload the configuration.
		 */
		if (got_sighup)
		{
			got_sighup = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/*
		 * Start a transaction on which we can run queries.  Note that each
		 * StartTransactionCommand() call should be preceded by a
		 * SetCurrentStatementStartTimestamp() call, which sets both the time
		 * for the statement we're about the run, and also the transaction
		 * start time.  Also, each other query sent to SPI should probably be
		 * preceded by SetCurrentStatementStartTimestamp(), so that statement
		 * start time is always up to date.
		 *
		 * The SPI_connect() call lets us run queries through the SPI manager,
		 * and the PushActiveSnapshot() call creates an "active" snapshot
		 * which is necessary for queries to have MVCC data to work on.
		 *
		 * The pgstat_report_activity() call makes our activity visible
		 * through the pgstat views.
		 */
		SetCurrentStatementStartTimestamp();
		StartTransactionCommand();
		SPI_connect();
		PushActiveSnapshot(GetTransactionSnapshot());
		pgstat_report_activity(STATE_RUNNING, buf.data);

		/* We can now execute queries via SPI */
		ret = SPI_execute(buf.data, false, 0);

		if (ret != SPI_OK_UPDATE_RETURNING)
			elog(FATAL, "cannot select from table %s.%s: error code %d",
				 table->schema, table->name, ret);

		if (SPI_processed > 0)
		{
			bool		isnull;
			int32		val;

			val = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0],
											  SPI_tuptable->tupdesc,
											  1, &isnull));
			if (!isnull)
				elog(LOG, "%s: count in %s.%s is now %d",
					 MyBgworkerEntry->bgw_name,
					 table->schema, table->name, val);
		}

		/*
		 * And finish our transaction.
		 */
		SPI_finish();
		PopActiveSnapshot();
		CommitTransactionCommand();
		pgstat_report_stat(false);
		pgstat_report_activity(STATE_IDLE, NULL);
	}

	proc_exit(1);
}

/*
 * Entrypoint of this module.
 *
 * We register more than one worker process here, to demonstrate how that can
 * be done.
 */
void
_PG_init(void)
{
	BackgroundWorker worker;
	unsigned int i;

	/* get the configuration */
	DefineCustomIntVariable("worker_spi.naptime",
							"Duration between each check (in seconds).",
							NULL,
							&worker_spi_naptime,
							10,
							1,
							INT_MAX,
							PGC_SIGHUP,
							0,
							NULL,
							NULL,
							NULL);

	if (!process_shared_preload_libraries_in_progress)
		return;

	DefineCustomIntVariable("worker_spi.total_workers",
							"Number of workers.",
							NULL,
							&worker_spi_total_workers,
							2,
							1,
							100,
							PGC_POSTMASTER,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomStringVariable("worker_spi.database",
							   "Database to connect to.",
							   NULL,
							   &worker_spi_database,
							   "postgres",
							   PGC_POSTMASTER,
							   0,
							   NULL, NULL, NULL);

	/* set up common data for all our workers */
	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	sprintf(worker.bgw_library_name, "worker_spi");
	sprintf(worker.bgw_function_name, "worker_spi_main");
	worker.bgw_notify_pid = 0;

	/*
	 * Now fill in worker-specific data, and do the actual registrations.
	 */
	for (i = 1; i <= worker_spi_total_workers; i++)
	{
		snprintf(worker.bgw_name, BGW_MAXLEN, "worker_spi worker %d", i);
		snprintf(worker.bgw_type, BGW_MAXLEN, "worker_spi");
		worker.bgw_main_arg = Int32GetDatum(i);

		RegisterBackgroundWorker(&worker);
	}
}

/*
 * Dynamically launch an SPI worker.
 */
Datum
worker_spi_launch(PG_FUNCTION_ARGS)
{
	int32		i = PG_GETARG_INT32(0);
	BackgroundWorker worker;
	BackgroundWorkerHandle *handle;
	BgwHandleStatus status;
	pid_t		pid;

	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	sprintf(worker.bgw_library_name, "worker_spi");
	sprintf(worker.bgw_function_name, "worker_spi_main");
	snprintf(worker.bgw_name, BGW_MAXLEN, "worker_spi worker %d", i);
	snprintf(worker.bgw_type, BGW_MAXLEN, "worker_spi");
	worker.bgw_main_arg = Int32GetDatum(i);
	/* set bgw_notify_pid so that we can use WaitForBackgroundWorkerStartup */
	worker.bgw_notify_pid = MyProcPid;

	if (!RegisterDynamicBackgroundWorker(&worker, &handle))
		PG_RETURN_NULL();

	status = WaitForBackgroundWorkerStartup(handle, &pid);

	if (status == BGWH_STOPPED)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("could not start background process"),
				 errhint("More details may be available in the server log.")));
	if (status == BGWH_POSTMASTER_DIED)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("cannot start background processes without postmaster"),
				 errhint("Kill all remaining database processes and restart the database.")));
	Assert(status == BGWH_STARTED);

	PG_RETURN_INT32(pid);
}
