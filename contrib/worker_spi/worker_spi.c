/* -------------------------------------------------------------------------
 *
 * worker_spi.c
 *		Sample background worker code that demonstrates usage of a database
 *		connection.
 *
 * This code connects to a database, create a schema and table, and summarizes
 * the numbers contained therein.  To see it working, insert an initial value
 * with "total" type and some initial value; then insert some other rows with
 * "delta" type.  Delta rows will be deleted by this worker and their values
 * aggregated into the total.
 *
 * Copyright (C) 2012, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/worker_spi/worker_spi.c
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
#include "utils/builtins.h"
#include "utils/snapmgr.h"

PG_MODULE_MAGIC;

void	_PG_init(void);

static bool	got_sigterm = false;


typedef struct worktable
{
	const char	   *schema;
	const char	   *name;
} worktable;

static void
worker_spi_sigterm(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_sigterm = true;
	if (MyProc)
		SetLatch(&MyProc->procLatch);

	errno = save_errno;
}

static void
worker_spi_sighup(SIGNAL_ARGS)
{
	elog(LOG, "got sighup!");
	if (MyProc)
		SetLatch(&MyProc->procLatch);
}

static void
initialize_worker_spi(worktable *table)
{
	int		ret;
	int		ntup;
	bool	isnull;
	StringInfoData	buf;

	StartTransactionCommand();
	SPI_connect();
	PushActiveSnapshot(GetTransactionSnapshot());

	initStringInfo(&buf);
	appendStringInfo(&buf, "select count(*) from pg_namespace where nspname = '%s'",
					 table->schema);

	ret = SPI_execute(buf.data, true, 0);
	if (ret != SPI_OK_SELECT)
		elog(FATAL, "SPI_execute failed: error code %d", ret);

	if (SPI_processed != 1)
		elog(FATAL, "not a singleton result");

	ntup = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0],
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

		ret = SPI_execute(buf.data, false, 0);

		if (ret != SPI_OK_UTILITY)
			elog(FATAL, "failed to create my schema");
	}

	SPI_finish();
	PopActiveSnapshot();
	CommitTransactionCommand();
}

static void
worker_spi_main(void *main_arg)
{
	worktable	   *table = (worktable *) main_arg;
	StringInfoData	buf;

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	/* Connect to our database */
	BackgroundWorkerInitializeConnection("postgres", NULL);

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

	while (!got_sigterm)
	{
		int		ret;
		int		rc;

		/*
		 * Background workers mustn't call usleep() or any direct equivalent:
		 * instead, they may wait on their process latch, which sleeps as
		 * necessary, but is awakened if postmaster dies.  That way the
		 * background process goes away immediately in an emergency.
		 */
		rc = WaitLatch(&MyProc->procLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   1000L);
		ResetLatch(&MyProc->procLatch);

		/* emergency bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);

		StartTransactionCommand();
		SPI_connect();
		PushActiveSnapshot(GetTransactionSnapshot());

		ret = SPI_execute(buf.data, false, 0);

		if (ret != SPI_OK_UPDATE_RETURNING)
			elog(FATAL, "cannot select from table %s.%s: error code %d",
				 table->schema, table->name, ret);

		if (SPI_processed > 0)
		{
			bool	isnull;
			int32	val;

			val = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0],
											   SPI_tuptable->tupdesc,
											   1, &isnull));
			if (!isnull)
				elog(LOG, "%s: count in %s.%s is now %d",
					 MyBgworkerEntry->bgw_name,
					 table->schema, table->name, val);
		}

		SPI_finish();
		PopActiveSnapshot();
		CommitTransactionCommand();
	}

	proc_exit(0);
}

/*
 * Entrypoint of this module.
 *
 * We register two worker processes here, to demonstrate how that can be done.
 */
void
_PG_init(void)
{
	BackgroundWorker	worker;
	worktable		   *table;

	/* register the worker processes.  These values are common for both */
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_main = worker_spi_main;
	worker.bgw_sighup = worker_spi_sighup;
	worker.bgw_sigterm = worker_spi_sigterm;

	/*
	 * These values are used for the first worker.
	 *
	 * Note these are palloc'd.  The reason this works after starting a new
	 * worker process is that if we only fork, they point to valid allocated
	 * memory in the child process; and if we fork and then exec, the exec'd
	 * process will run this code again, and so the memory is also valid there.
	 */
	table = palloc(sizeof(worktable));
	table->schema = pstrdup("schema1");
	table->name = pstrdup("counted");

	worker.bgw_name = "SPI worker 1";
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	worker.bgw_main_arg = (void *) table;
	RegisterBackgroundWorker(&worker);

	/* Values for the second worker */
	table = palloc(sizeof(worktable));
	table->schema = pstrdup("our schema2");
	table->name = pstrdup("counted rows");

	worker.bgw_name = "SPI worker 2";
	worker.bgw_restart_time = 2;
	worker.bgw_main_arg = (void *) table;
	RegisterBackgroundWorker(&worker);
}
