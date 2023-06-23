/*-------------------------------------------------------------------------
 *
 * libpq_pipeline.c
 *		Verify libpq pipeline execution functionality
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *		src/test/modules/libpq_pipeline/libpq_pipeline.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <sys/time.h>
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#include "catalog/pg_type_d.h"
#include "common/fe_memutils.h"
#include "libpq-fe.h"
#include "pg_getopt.h"
#include "portability/instr_time.h"


static void exit_nicely(PGconn *conn);
static void pg_attribute_noreturn() pg_fatal_impl(int line, const char *fmt,...)
			pg_attribute_printf(2, 3);
static bool process_result(PGconn *conn, PGresult *res, int results,
						   int numsent);

const char *const progname = "libpq_pipeline";

/* Options and defaults */
char	   *tracefile = NULL;	/* path to PQtrace() file */


#ifdef DEBUG_OUTPUT
#define	pg_debug(...)  do { fprintf(stderr, __VA_ARGS__); } while (0)
#else
#define pg_debug(...)
#endif

static const char *const drop_table_sql =
"DROP TABLE IF EXISTS pq_pipeline_demo";
static const char *const create_table_sql =
"CREATE UNLOGGED TABLE pq_pipeline_demo(id serial primary key, itemno integer,"
"int8filler int8);";
static const char *const insert_sql =
"INSERT INTO pq_pipeline_demo(itemno) VALUES ($1)";
static const char *const insert_sql2 =
"INSERT INTO pq_pipeline_demo(itemno,int8filler) VALUES ($1, $2)";

/* max char length of an int32/64, plus sign and null terminator */
#define MAXINTLEN 12
#define MAXINT8LEN 20

static void
exit_nicely(PGconn *conn)
{
	PQfinish(conn);
	exit(1);
}

/*
 * Print an error to stderr and terminate the program.
 */
#define pg_fatal(...) pg_fatal_impl(__LINE__, __VA_ARGS__)
static void
pg_attribute_noreturn()
pg_fatal_impl(int line, const char *fmt,...)
{
	va_list		args;


	fflush(stdout);

	fprintf(stderr, "\n%s:%d: ", progname, line);
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	Assert(fmt[strlen(fmt) - 1] != '\n');
	fprintf(stderr, "\n");
	exit(1);
}

static void
test_disallowed_in_pipeline(PGconn *conn)
{
	PGresult   *res = NULL;

	fprintf(stderr, "test error cases... ");

	if (PQisnonblocking(conn))
		pg_fatal("Expected blocking connection mode");

	if (PQenterPipelineMode(conn) != 1)
		pg_fatal("Unable to enter pipeline mode");

	if (PQpipelineStatus(conn) == PQ_PIPELINE_OFF)
		pg_fatal("Pipeline mode not activated properly");

	/* PQexec should fail in pipeline mode */
	res = PQexec(conn, "SELECT 1");
	if (PQresultStatus(res) != PGRES_FATAL_ERROR)
		pg_fatal("PQexec should fail in pipeline mode but succeeded");
	if (strcmp(PQerrorMessage(conn),
			   "synchronous command execution functions are not allowed in pipeline mode\n") != 0)
		pg_fatal("did not get expected error message; got: \"%s\"",
				 PQerrorMessage(conn));

	/* PQsendQuery should fail in pipeline mode */
	if (PQsendQuery(conn, "SELECT 1") != 0)
		pg_fatal("PQsendQuery should fail in pipeline mode but succeeded");
	if (strcmp(PQerrorMessage(conn),
			   "PQsendQuery not allowed in pipeline mode\n") != 0)
		pg_fatal("did not get expected error message; got: \"%s\"",
				 PQerrorMessage(conn));

	/* Entering pipeline mode when already in pipeline mode is OK */
	if (PQenterPipelineMode(conn) != 1)
		pg_fatal("re-entering pipeline mode should be a no-op but failed");

	if (PQisBusy(conn) != 0)
		pg_fatal("PQisBusy should return 0 when idle in pipeline mode, returned 1");

	/* ok, back to normal command mode */
	if (PQexitPipelineMode(conn) != 1)
		pg_fatal("couldn't exit idle empty pipeline mode");

	if (PQpipelineStatus(conn) != PQ_PIPELINE_OFF)
		pg_fatal("Pipeline mode not terminated properly");

	/* exiting pipeline mode when not in pipeline mode should be a no-op */
	if (PQexitPipelineMode(conn) != 1)
		pg_fatal("pipeline mode exit when not in pipeline mode should succeed but failed");

	/* can now PQexec again */
	res = PQexec(conn, "SELECT 1");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pg_fatal("PQexec should succeed after exiting pipeline mode but failed with: %s",
				 PQerrorMessage(conn));

	fprintf(stderr, "ok\n");
}

static void
test_multi_pipelines(PGconn *conn)
{
	PGresult   *res = NULL;
	const char *dummy_params[1] = {"1"};
	Oid			dummy_param_oids[1] = {INT4OID};

	fprintf(stderr, "multi pipeline... ");

	/*
	 * Queue up a couple of small pipelines and process each without returning
	 * to command mode first.
	 */
	if (PQenterPipelineMode(conn) != 1)
		pg_fatal("failed to enter pipeline mode: %s", PQerrorMessage(conn));

	if (PQsendQueryParams(conn, "SELECT $1", 1, dummy_param_oids,
						  dummy_params, NULL, NULL, 0) != 1)
		pg_fatal("dispatching first SELECT failed: %s", PQerrorMessage(conn));

	if (PQpipelineSync(conn) != 1)
		pg_fatal("Pipeline sync failed: %s", PQerrorMessage(conn));

	if (PQsendQueryParams(conn, "SELECT $1", 1, dummy_param_oids,
						  dummy_params, NULL, NULL, 0) != 1)
		pg_fatal("dispatching second SELECT failed: %s", PQerrorMessage(conn));

	if (PQpipelineSync(conn) != 1)
		pg_fatal("pipeline sync failed: %s", PQerrorMessage(conn));

	/* OK, start processing the results */
	res = PQgetResult(conn);
	if (res == NULL)
		pg_fatal("PQgetResult returned null when there's a pipeline item: %s",
				 PQerrorMessage(conn));

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pg_fatal("Unexpected result code %s from first pipeline item",
				 PQresStatus(PQresultStatus(res)));
	PQclear(res);
	res = NULL;

	if (PQgetResult(conn) != NULL)
		pg_fatal("PQgetResult returned something extra after first result");

	if (PQexitPipelineMode(conn) != 0)
		pg_fatal("exiting pipeline mode after query but before sync succeeded incorrectly");

	res = PQgetResult(conn);
	if (res == NULL)
		pg_fatal("PQgetResult returned null when sync result expected: %s",
				 PQerrorMessage(conn));

	if (PQresultStatus(res) != PGRES_PIPELINE_SYNC)
		pg_fatal("Unexpected result code %s instead of sync result, error: %s",
				 PQresStatus(PQresultStatus(res)), PQerrorMessage(conn));
	PQclear(res);

	/* second pipeline */

	res = PQgetResult(conn);
	if (res == NULL)
		pg_fatal("PQgetResult returned null when there's a pipeline item: %s",
				 PQerrorMessage(conn));

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pg_fatal("Unexpected result code %s from second pipeline item",
				 PQresStatus(PQresultStatus(res)));

	res = PQgetResult(conn);
	if (res != NULL)
		pg_fatal("Expected null result, got %s",
				 PQresStatus(PQresultStatus(res)));

	res = PQgetResult(conn);
	if (res == NULL)
		pg_fatal("PQgetResult returned null when there's a pipeline item: %s",
				 PQerrorMessage(conn));

	if (PQresultStatus(res) != PGRES_PIPELINE_SYNC)
		pg_fatal("Unexpected result code %s from second pipeline sync",
				 PQresStatus(PQresultStatus(res)));

	/* We're still in pipeline mode ... */
	if (PQpipelineStatus(conn) == PQ_PIPELINE_OFF)
		pg_fatal("Fell out of pipeline mode somehow");

	/* until we end it, which we can safely do now */
	if (PQexitPipelineMode(conn) != 1)
		pg_fatal("attempt to exit pipeline mode failed when it should've succeeded: %s",
				 PQerrorMessage(conn));

	if (PQpipelineStatus(conn) != PQ_PIPELINE_OFF)
		pg_fatal("exiting pipeline mode didn't seem to work");

	fprintf(stderr, "ok\n");
}

/*
 * Test behavior when a pipeline dispatches a number of commands that are
 * not flushed by a sync point.
 */
static void
test_nosync(PGconn *conn)
{
	int			numqueries = 10;
	int			results = 0;
	int			sock = PQsocket(conn);

	fprintf(stderr, "nosync... ");

	if (sock < 0)
		pg_fatal("invalid socket");

	if (PQenterPipelineMode(conn) != 1)
		pg_fatal("could not enter pipeline mode");
	for (int i = 0; i < numqueries; i++)
	{
		fd_set		input_mask;
		struct timeval tv;

		if (PQsendQueryParams(conn, "SELECT repeat('xyzxz', 12)",
							  0, NULL, NULL, NULL, NULL, 0) != 1)
			pg_fatal("error sending select: %s", PQerrorMessage(conn));
		PQflush(conn);

		/*
		 * If the server has written anything to us, read (some of) it now.
		 */
		FD_ZERO(&input_mask);
		FD_SET(sock, &input_mask);
		tv.tv_sec = 0;
		tv.tv_usec = 0;
		if (select(sock + 1, &input_mask, NULL, NULL, &tv) < 0)
		{
			fprintf(stderr, "select() failed: %s\n", strerror(errno));
			exit_nicely(conn);
		}
		if (FD_ISSET(sock, &input_mask) && PQconsumeInput(conn) != 1)
			pg_fatal("failed to read from server: %s", PQerrorMessage(conn));
	}

	/* tell server to flush its output buffer */
	if (PQsendFlushRequest(conn) != 1)
		pg_fatal("failed to send flush request");
	PQflush(conn);

	/* Now read all results */
	for (;;)
	{
		PGresult   *res;

		res = PQgetResult(conn);

		/* NULL results are only expected after TUPLES_OK */
		if (res == NULL)
			pg_fatal("got unexpected NULL result after %d results", results);

		/* We expect exactly one TUPLES_OK result for each query we sent */
		if (PQresultStatus(res) == PGRES_TUPLES_OK)
		{
			PGresult   *res2;

			/* and one NULL result should follow each */
			res2 = PQgetResult(conn);
			if (res2 != NULL)
				pg_fatal("expected NULL, got %s",
						 PQresStatus(PQresultStatus(res2)));
			PQclear(res);
			results++;

			/* if we're done, we're done */
			if (results == numqueries)
				break;

			continue;
		}

		/* anything else is unexpected */
		pg_fatal("got unexpected %s\n", PQresStatus(PQresultStatus(res)));
	}

	fprintf(stderr, "ok\n");
}

/*
 * When an operation in a pipeline fails the rest of the pipeline is flushed. We
 * still have to get results for each pipeline item, but the item will just be
 * a PGRES_PIPELINE_ABORTED code.
 *
 * This intentionally doesn't use a transaction to wrap the pipeline. You should
 * usually use an xact, but in this case we want to observe the effects of each
 * statement.
 */
static void
test_pipeline_abort(PGconn *conn)
{
	PGresult   *res = NULL;
	const char *dummy_params[1] = {"1"};
	Oid			dummy_param_oids[1] = {INT4OID};
	int			i;
	int			gotrows;
	bool		goterror;

	fprintf(stderr, "aborted pipeline... ");

	res = PQexec(conn, drop_table_sql);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pg_fatal("dispatching DROP TABLE failed: %s", PQerrorMessage(conn));

	res = PQexec(conn, create_table_sql);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pg_fatal("dispatching CREATE TABLE failed: %s", PQerrorMessage(conn));

	/*
	 * Queue up a couple of small pipelines and process each without returning
	 * to command mode first. Make sure the second operation in the first
	 * pipeline ERRORs.
	 */
	if (PQenterPipelineMode(conn) != 1)
		pg_fatal("failed to enter pipeline mode: %s", PQerrorMessage(conn));

	dummy_params[0] = "1";
	if (PQsendQueryParams(conn, insert_sql, 1, dummy_param_oids,
						  dummy_params, NULL, NULL, 0) != 1)
		pg_fatal("dispatching first insert failed: %s", PQerrorMessage(conn));

	if (PQsendQueryParams(conn, "SELECT no_such_function($1)",
						  1, dummy_param_oids, dummy_params,
						  NULL, NULL, 0) != 1)
		pg_fatal("dispatching error select failed: %s", PQerrorMessage(conn));

	dummy_params[0] = "2";
	if (PQsendQueryParams(conn, insert_sql, 1, dummy_param_oids,
						  dummy_params, NULL, NULL, 0) != 1)
		pg_fatal("dispatching second insert failed: %s", PQerrorMessage(conn));

	if (PQpipelineSync(conn) != 1)
		pg_fatal("pipeline sync failed: %s", PQerrorMessage(conn));

	dummy_params[0] = "3";
	if (PQsendQueryParams(conn, insert_sql, 1, dummy_param_oids,
						  dummy_params, NULL, NULL, 0) != 1)
		pg_fatal("dispatching second-pipeline insert failed: %s",
				 PQerrorMessage(conn));

	if (PQpipelineSync(conn) != 1)
		pg_fatal("pipeline sync failed: %s", PQerrorMessage(conn));

	/*
	 * OK, start processing the pipeline results.
	 *
	 * We should get a command-ok for the first query, then a fatal error and
	 * a pipeline aborted message for the second insert, a pipeline-end, then
	 * a command-ok and a pipeline-ok for the second pipeline operation.
	 */
	res = PQgetResult(conn);
	if (res == NULL)
		pg_fatal("Unexpected NULL result: %s", PQerrorMessage(conn));
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pg_fatal("Unexpected result status %s: %s",
				 PQresStatus(PQresultStatus(res)),
				 PQresultErrorMessage(res));
	PQclear(res);

	/* NULL result to signal end-of-results for this command */
	if ((res = PQgetResult(conn)) != NULL)
		pg_fatal("Expected null result, got %s",
				 PQresStatus(PQresultStatus(res)));

	/* Second query caused error, so we expect an error next */
	res = PQgetResult(conn);
	if (res == NULL)
		pg_fatal("Unexpected NULL result: %s", PQerrorMessage(conn));
	if (PQresultStatus(res) != PGRES_FATAL_ERROR)
		pg_fatal("Unexpected result code -- expected PGRES_FATAL_ERROR, got %s",
				 PQresStatus(PQresultStatus(res)));
	PQclear(res);

	/* NULL result to signal end-of-results for this command */
	if ((res = PQgetResult(conn)) != NULL)
		pg_fatal("Expected null result, got %s",
				 PQresStatus(PQresultStatus(res)));

	/*
	 * pipeline should now be aborted.
	 *
	 * Note that we could still queue more queries at this point if we wanted;
	 * they'd get added to a new third pipeline since we've already sent a
	 * second. The aborted flag relates only to the pipeline being received.
	 */
	if (PQpipelineStatus(conn) != PQ_PIPELINE_ABORTED)
		pg_fatal("pipeline should be flagged as aborted but isn't");

	/* third query in pipeline, the second insert */
	res = PQgetResult(conn);
	if (res == NULL)
		pg_fatal("Unexpected NULL result: %s", PQerrorMessage(conn));
	if (PQresultStatus(res) != PGRES_PIPELINE_ABORTED)
		pg_fatal("Unexpected result code -- expected PGRES_PIPELINE_ABORTED, got %s",
				 PQresStatus(PQresultStatus(res)));
	PQclear(res);

	/* NULL result to signal end-of-results for this command */
	if ((res = PQgetResult(conn)) != NULL)
		pg_fatal("Expected null result, got %s", PQresStatus(PQresultStatus(res)));

	if (PQpipelineStatus(conn) != PQ_PIPELINE_ABORTED)
		pg_fatal("pipeline should be flagged as aborted but isn't");

	/* Ensure we're still in pipeline */
	if (PQpipelineStatus(conn) == PQ_PIPELINE_OFF)
		pg_fatal("Fell out of pipeline mode somehow");

	/*
	 * The end of a failed pipeline is a PGRES_PIPELINE_SYNC.
	 *
	 * (This is so clients know to start processing results normally again and
	 * can tell the difference between skipped commands and the sync.)
	 */
	res = PQgetResult(conn);
	if (res == NULL)
		pg_fatal("Unexpected NULL result: %s", PQerrorMessage(conn));
	if (PQresultStatus(res) != PGRES_PIPELINE_SYNC)
		pg_fatal("Unexpected result code from first pipeline sync\n"
				 "Expected PGRES_PIPELINE_SYNC, got %s",
				 PQresStatus(PQresultStatus(res)));
	PQclear(res);

	if (PQpipelineStatus(conn) == PQ_PIPELINE_ABORTED)
		pg_fatal("sync should've cleared the aborted flag but didn't");

	/* We're still in pipeline mode... */
	if (PQpipelineStatus(conn) == PQ_PIPELINE_OFF)
		pg_fatal("Fell out of pipeline mode somehow");

	/* the insert from the second pipeline */
	res = PQgetResult(conn);
	if (res == NULL)
		pg_fatal("Unexpected NULL result: %s", PQerrorMessage(conn));
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pg_fatal("Unexpected result code %s from first item in second pipeline",
				 PQresStatus(PQresultStatus(res)));
	PQclear(res);

	/* Read the NULL result at the end of the command */
	if ((res = PQgetResult(conn)) != NULL)
		pg_fatal("Expected null result, got %s", PQresStatus(PQresultStatus(res)));

	/* the second pipeline sync */
	if ((res = PQgetResult(conn)) == NULL)
		pg_fatal("Unexpected NULL result: %s", PQerrorMessage(conn));
	if (PQresultStatus(res) != PGRES_PIPELINE_SYNC)
		pg_fatal("Unexpected result code %s from second pipeline sync",
				 PQresStatus(PQresultStatus(res)));
	PQclear(res);

	if ((res = PQgetResult(conn)) != NULL)
		pg_fatal("Expected null result, got %s: %s",
				 PQresStatus(PQresultStatus(res)),
				 PQerrorMessage(conn));

	/* Try to send two queries in one command */
	if (PQsendQueryParams(conn, "SELECT 1; SELECT 2", 0, NULL, NULL, NULL, NULL, 0) != 1)
		pg_fatal("failed to send query: %s", PQerrorMessage(conn));
	if (PQpipelineSync(conn) != 1)
		pg_fatal("pipeline sync failed: %s", PQerrorMessage(conn));
	goterror = false;
	while ((res = PQgetResult(conn)) != NULL)
	{
		switch (PQresultStatus(res))
		{
			case PGRES_FATAL_ERROR:
				if (strcmp(PQresultErrorField(res, PG_DIAG_SQLSTATE), "42601") != 0)
					pg_fatal("expected error about multiple commands, got %s",
							 PQerrorMessage(conn));
				printf("got expected %s", PQerrorMessage(conn));
				goterror = true;
				break;
			default:
				pg_fatal("got unexpected status %s", PQresStatus(PQresultStatus(res)));
				break;
		}
	}
	if (!goterror)
		pg_fatal("did not get cannot-insert-multiple-commands error");
	res = PQgetResult(conn);
	if (res == NULL)
		pg_fatal("got NULL result");
	if (PQresultStatus(res) != PGRES_PIPELINE_SYNC)
		pg_fatal("Unexpected result code %s from pipeline sync",
				 PQresStatus(PQresultStatus(res)));
	fprintf(stderr, "ok\n");

	/* Test single-row mode with an error partways */
	if (PQsendQueryParams(conn, "SELECT 1.0/g FROM generate_series(3, -1, -1) g",
						  0, NULL, NULL, NULL, NULL, 0) != 1)
		pg_fatal("failed to send query: %s", PQerrorMessage(conn));
	if (PQpipelineSync(conn) != 1)
		pg_fatal("pipeline sync failed: %s", PQerrorMessage(conn));
	PQsetSingleRowMode(conn);
	goterror = false;
	gotrows = 0;
	while ((res = PQgetResult(conn)) != NULL)
	{
		switch (PQresultStatus(res))
		{
			case PGRES_SINGLE_TUPLE:
				printf("got row: %s\n", PQgetvalue(res, 0, 0));
				gotrows++;
				break;
			case PGRES_FATAL_ERROR:
				if (strcmp(PQresultErrorField(res, PG_DIAG_SQLSTATE), "22012") != 0)
					pg_fatal("expected division-by-zero, got: %s (%s)",
							 PQerrorMessage(conn),
							 PQresultErrorField(res, PG_DIAG_SQLSTATE));
				printf("got expected division-by-zero\n");
				goterror = true;
				break;
			default:
				pg_fatal("got unexpected result %s", PQresStatus(PQresultStatus(res)));
		}
		PQclear(res);
	}
	if (!goterror)
		pg_fatal("did not get division-by-zero error");
	if (gotrows != 3)
		pg_fatal("did not get three rows");
	/* the third pipeline sync */
	if ((res = PQgetResult(conn)) == NULL)
		pg_fatal("Unexpected NULL result: %s", PQerrorMessage(conn));
	if (PQresultStatus(res) != PGRES_PIPELINE_SYNC)
		pg_fatal("Unexpected result code %s from third pipeline sync",
				 PQresStatus(PQresultStatus(res)));
	PQclear(res);

	/* We're still in pipeline mode... */
	if (PQpipelineStatus(conn) == PQ_PIPELINE_OFF)
		pg_fatal("Fell out of pipeline mode somehow");

	/* until we end it, which we can safely do now */
	if (PQexitPipelineMode(conn) != 1)
		pg_fatal("attempt to exit pipeline mode failed when it should've succeeded: %s",
				 PQerrorMessage(conn));

	if (PQpipelineStatus(conn) != PQ_PIPELINE_OFF)
		pg_fatal("exiting pipeline mode didn't seem to work");

	/*-
	 * Since we fired the pipelines off without a surrounding xact, the results
	 * should be:
	 *
	 * - Implicit xact started by server around 1st pipeline
	 * - First insert applied
	 * - Second statement aborted xact
	 * - Third insert skipped
	 * - Sync rolled back first implicit xact
	 * - Implicit xact created by server around 2nd pipeline
	 * - insert applied from 2nd pipeline
	 * - Sync commits 2nd xact
	 *
	 * So we should only have the value 3 that we inserted.
	 */
	res = PQexec(conn, "SELECT itemno FROM pq_pipeline_demo");

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pg_fatal("Expected tuples, got %s: %s",
				 PQresStatus(PQresultStatus(res)), PQerrorMessage(conn));
	if (PQntuples(res) != 1)
		pg_fatal("expected 1 result, got %d", PQntuples(res));
	for (i = 0; i < PQntuples(res); i++)
	{
		const char *val = PQgetvalue(res, i, 0);

		if (strcmp(val, "3") != 0)
			pg_fatal("expected only insert with value 3, got %s", val);
	}

	PQclear(res);

	fprintf(stderr, "ok\n");
}

/* State machine enum for test_pipelined_insert */
enum PipelineInsertStep
{
	BI_BEGIN_TX,
	BI_DROP_TABLE,
	BI_CREATE_TABLE,
	BI_PREPARE,
	BI_INSERT_ROWS,
	BI_COMMIT_TX,
	BI_SYNC,
	BI_DONE
};

static void
test_pipelined_insert(PGconn *conn, int n_rows)
{
	Oid			insert_param_oids[2] = {INT4OID, INT8OID};
	const char *insert_params[2];
	char		insert_param_0[MAXINTLEN];
	char		insert_param_1[MAXINT8LEN];
	enum PipelineInsertStep send_step = BI_BEGIN_TX,
				recv_step = BI_BEGIN_TX;
	int			rows_to_send,
				rows_to_receive;

	insert_params[0] = insert_param_0;
	insert_params[1] = insert_param_1;

	rows_to_send = rows_to_receive = n_rows;

	/*
	 * Do a pipelined insert into a table created at the start of the pipeline
	 */
	if (PQenterPipelineMode(conn) != 1)
		pg_fatal("failed to enter pipeline mode: %s", PQerrorMessage(conn));

	while (send_step != BI_PREPARE)
	{
		const char *sql;

		switch (send_step)
		{
			case BI_BEGIN_TX:
				sql = "BEGIN TRANSACTION";
				send_step = BI_DROP_TABLE;
				break;

			case BI_DROP_TABLE:
				sql = drop_table_sql;
				send_step = BI_CREATE_TABLE;
				break;

			case BI_CREATE_TABLE:
				sql = create_table_sql;
				send_step = BI_PREPARE;
				break;

			default:
				pg_fatal("invalid state");
				sql = NULL;		/* keep compiler quiet */
		}

		pg_debug("sending: %s\n", sql);
		if (PQsendQueryParams(conn, sql,
							  0, NULL, NULL, NULL, NULL, 0) != 1)
			pg_fatal("dispatching %s failed: %s", sql, PQerrorMessage(conn));
	}

	Assert(send_step == BI_PREPARE);
	pg_debug("sending: %s\n", insert_sql2);
	if (PQsendPrepare(conn, "my_insert", insert_sql2, 2, insert_param_oids) != 1)
		pg_fatal("dispatching PREPARE failed: %s", PQerrorMessage(conn));
	send_step = BI_INSERT_ROWS;

	/*
	 * Now we start inserting. We'll be sending enough data that we could fill
	 * our output buffer, so to avoid deadlocking we need to enter nonblocking
	 * mode and consume input while we send more output. As results of each
	 * query are processed we should pop them to allow processing of the next
	 * query. There's no need to finish the pipeline before processing
	 * results.
	 */
	if (PQsetnonblocking(conn, 1) != 0)
		pg_fatal("failed to set nonblocking mode: %s", PQerrorMessage(conn));

	while (recv_step != BI_DONE)
	{
		int			sock;
		fd_set		input_mask;
		fd_set		output_mask;

		sock = PQsocket(conn);

		if (sock < 0)
			break;				/* shouldn't happen */

		FD_ZERO(&input_mask);
		FD_SET(sock, &input_mask);
		FD_ZERO(&output_mask);
		FD_SET(sock, &output_mask);

		if (select(sock + 1, &input_mask, &output_mask, NULL, NULL) < 0)
		{
			fprintf(stderr, "select() failed: %s\n", strerror(errno));
			exit_nicely(conn);
		}

		/*
		 * Process any results, so we keep the server's output buffer free
		 * flowing and it can continue to process input
		 */
		if (FD_ISSET(sock, &input_mask))
		{
			PQconsumeInput(conn);

			/* Read until we'd block if we tried to read */
			while (!PQisBusy(conn) && recv_step < BI_DONE)
			{
				PGresult   *res;
				const char *cmdtag = "";
				const char *description = "";
				int			status;

				/*
				 * Read next result.  If no more results from this query,
				 * advance to the next query
				 */
				res = PQgetResult(conn);
				if (res == NULL)
					continue;

				status = PGRES_COMMAND_OK;
				switch (recv_step)
				{
					case BI_BEGIN_TX:
						cmdtag = "BEGIN";
						recv_step++;
						break;
					case BI_DROP_TABLE:
						cmdtag = "DROP TABLE";
						recv_step++;
						break;
					case BI_CREATE_TABLE:
						cmdtag = "CREATE TABLE";
						recv_step++;
						break;
					case BI_PREPARE:
						cmdtag = "";
						description = "PREPARE";
						recv_step++;
						break;
					case BI_INSERT_ROWS:
						cmdtag = "INSERT";
						rows_to_receive--;
						if (rows_to_receive == 0)
							recv_step++;
						break;
					case BI_COMMIT_TX:
						cmdtag = "COMMIT";
						recv_step++;
						break;
					case BI_SYNC:
						cmdtag = "";
						description = "SYNC";
						status = PGRES_PIPELINE_SYNC;
						recv_step++;
						break;
					case BI_DONE:
						/* unreachable */
						pg_fatal("unreachable state");
				}

				if (PQresultStatus(res) != status)
					pg_fatal("%s reported status %s, expected %s\n"
							 "Error message: \"%s\"",
							 description, PQresStatus(PQresultStatus(res)),
							 PQresStatus(status), PQerrorMessage(conn));

				if (strncmp(PQcmdStatus(res), cmdtag, strlen(cmdtag)) != 0)
					pg_fatal("%s expected command tag '%s', got '%s'",
							 description, cmdtag, PQcmdStatus(res));

				pg_debug("Got %s OK\n", cmdtag[0] != '\0' ? cmdtag : description);

				PQclear(res);
			}
		}

		/* Write more rows and/or the end pipeline message, if needed */
		if (FD_ISSET(sock, &output_mask))
		{
			PQflush(conn);

			if (send_step == BI_INSERT_ROWS)
			{
				snprintf(insert_param_0, MAXINTLEN, "%d", rows_to_send);
				/* use up some buffer space with a wide value */
				snprintf(insert_param_1, MAXINT8LEN, "%lld", 1LL << 62);

				if (PQsendQueryPrepared(conn, "my_insert",
										2, insert_params, NULL, NULL, 0) == 1)
				{
					pg_debug("sent row %d\n", rows_to_send);

					rows_to_send--;
					if (rows_to_send == 0)
						send_step++;
				}
				else
				{
					/*
					 * in nonblocking mode, so it's OK for an insert to fail
					 * to send
					 */
					fprintf(stderr, "WARNING: failed to send insert #%d: %s\n",
							rows_to_send, PQerrorMessage(conn));
				}
			}
			else if (send_step == BI_COMMIT_TX)
			{
				if (PQsendQueryParams(conn, "COMMIT",
									  0, NULL, NULL, NULL, NULL, 0) == 1)
				{
					pg_debug("sent COMMIT\n");
					send_step++;
				}
				else
				{
					fprintf(stderr, "WARNING: failed to send commit: %s\n",
							PQerrorMessage(conn));
				}
			}
			else if (send_step == BI_SYNC)
			{
				if (PQpipelineSync(conn) == 1)
				{
					fprintf(stdout, "pipeline sync sent\n");
					send_step++;
				}
				else
				{
					fprintf(stderr, "WARNING: pipeline sync failed: %s\n",
							PQerrorMessage(conn));
				}
			}
		}
	}

	/* We've got the sync message and the pipeline should be done */
	if (PQexitPipelineMode(conn) != 1)
		pg_fatal("attempt to exit pipeline mode failed when it should've succeeded: %s",
				 PQerrorMessage(conn));

	if (PQsetnonblocking(conn, 0) != 0)
		pg_fatal("failed to clear nonblocking mode: %s", PQerrorMessage(conn));

	fprintf(stderr, "ok\n");
}

static void
test_prepared(PGconn *conn)
{
	PGresult   *res = NULL;
	Oid			param_oids[1] = {INT4OID};
	Oid			expected_oids[4];
	Oid			typ;

	fprintf(stderr, "prepared... ");

	if (PQenterPipelineMode(conn) != 1)
		pg_fatal("failed to enter pipeline mode: %s", PQerrorMessage(conn));
	if (PQsendPrepare(conn, "select_one", "SELECT $1, '42', $1::numeric, "
					  "interval '1 sec'",
					  1, param_oids) != 1)
		pg_fatal("preparing query failed: %s", PQerrorMessage(conn));
	expected_oids[0] = INT4OID;
	expected_oids[1] = TEXTOID;
	expected_oids[2] = NUMERICOID;
	expected_oids[3] = INTERVALOID;
	if (PQsendDescribePrepared(conn, "select_one") != 1)
		pg_fatal("failed to send describePrepared: %s", PQerrorMessage(conn));
	if (PQpipelineSync(conn) != 1)
		pg_fatal("pipeline sync failed: %s", PQerrorMessage(conn));

	res = PQgetResult(conn);
	if (res == NULL)
		pg_fatal("PQgetResult returned null");
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pg_fatal("expected COMMAND_OK, got %s", PQresStatus(PQresultStatus(res)));
	PQclear(res);
	res = PQgetResult(conn);
	if (res != NULL)
		pg_fatal("expected NULL result");

	res = PQgetResult(conn);
	if (res == NULL)
		pg_fatal("PQgetResult returned NULL");
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pg_fatal("expected COMMAND_OK, got %s", PQresStatus(PQresultStatus(res)));
	if (PQnfields(res) != lengthof(expected_oids))
		pg_fatal("expected %zd columns, got %d",
				 lengthof(expected_oids), PQnfields(res));
	for (int i = 0; i < PQnfields(res); i++)
	{
		typ = PQftype(res, i);
		if (typ != expected_oids[i])
			pg_fatal("field %d: expected type %u, got %u",
					 i, expected_oids[i], typ);
	}
	PQclear(res);
	res = PQgetResult(conn);
	if (res != NULL)
		pg_fatal("expected NULL result");

	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_PIPELINE_SYNC)
		pg_fatal("expected PGRES_PIPELINE_SYNC, got %s", PQresStatus(PQresultStatus(res)));

	if (PQexitPipelineMode(conn) != 1)
		pg_fatal("could not exit pipeline mode: %s", PQerrorMessage(conn));

	PQexec(conn, "BEGIN");
	PQexec(conn, "DECLARE cursor_one CURSOR FOR SELECT 1");
	PQenterPipelineMode(conn);
	if (PQsendDescribePortal(conn, "cursor_one") != 1)
		pg_fatal("PQsendDescribePortal failed: %s", PQerrorMessage(conn));
	if (PQpipelineSync(conn) != 1)
		pg_fatal("pipeline sync failed: %s", PQerrorMessage(conn));
	res = PQgetResult(conn);
	if (res == NULL)
		pg_fatal("PQgetResult returned null");
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pg_fatal("expected COMMAND_OK, got %s", PQresStatus(PQresultStatus(res)));

	typ = PQftype(res, 0);
	if (typ != INT4OID)
		pg_fatal("portal: expected type %u, got %u",
				 INT4OID, typ);
	PQclear(res);
	res = PQgetResult(conn);
	if (res != NULL)
		pg_fatal("expected NULL result");
	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_PIPELINE_SYNC)
		pg_fatal("expected PGRES_PIPELINE_SYNC, got %s", PQresStatus(PQresultStatus(res)));

	if (PQexitPipelineMode(conn) != 1)
		pg_fatal("could not exit pipeline mode: %s", PQerrorMessage(conn));

	fprintf(stderr, "ok\n");
}

/* Notice processor: print notices, and count how many we got */
static void
notice_processor(void *arg, const char *message)
{
	int	   *n_notices = (int *) arg;

	(*n_notices)++;
	fprintf(stderr, "NOTICE %d: %s", *n_notices, message);
}

/* Verify behavior in "idle" state */
static void
test_pipeline_idle(PGconn *conn)
{
	PGresult   *res;
	int			n_notices = 0;

	fprintf(stderr, "\npipeline idle...\n");

	PQsetNoticeProcessor(conn, notice_processor, &n_notices);

	/* Try to exit pipeline mode in pipeline-idle state */
	if (PQenterPipelineMode(conn) != 1)
		pg_fatal("failed to enter pipeline mode: %s", PQerrorMessage(conn));
	if (PQsendQueryParams(conn, "SELECT 1", 0, NULL, NULL, NULL, NULL, 0) != 1)
		pg_fatal("failed to send query: %s", PQerrorMessage(conn));
	PQsendFlushRequest(conn);
	res = PQgetResult(conn);
	if (res == NULL)
		pg_fatal("PQgetResult returned null when there's a pipeline item: %s",
				 PQerrorMessage(conn));
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pg_fatal("unexpected result code %s from first pipeline item",
				 PQresStatus(PQresultStatus(res)));
	PQclear(res);
	res = PQgetResult(conn);
	if (res != NULL)
		pg_fatal("did not receive terminating NULL");
	if (PQsendQueryParams(conn, "SELECT 2", 0, NULL, NULL, NULL, NULL, 0) != 1)
		pg_fatal("failed to send query: %s", PQerrorMessage(conn));
	if (PQexitPipelineMode(conn) == 1)
		pg_fatal("exiting pipeline succeeded when it shouldn't");
	if (strncmp(PQerrorMessage(conn), "cannot exit pipeline mode",
				strlen("cannot exit pipeline mode")) != 0)
		pg_fatal("did not get expected error; got: %s",
				 PQerrorMessage(conn));
	PQsendFlushRequest(conn);
	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pg_fatal("unexpected result code %s from second pipeline item",
				 PQresStatus(PQresultStatus(res)));
	PQclear(res);
	res = PQgetResult(conn);
	if (res != NULL)
		pg_fatal("did not receive terminating NULL");
	if (PQexitPipelineMode(conn) != 1)
		pg_fatal("exiting pipeline failed: %s", PQerrorMessage(conn));

	if (n_notices > 0)
		pg_fatal("got %d notice(s)", n_notices);
	fprintf(stderr, "ok - 1\n");

	/* Have a WARNING in the middle of a resultset */
	if (PQenterPipelineMode(conn) != 1)
		pg_fatal("entering pipeline mode failed: %s", PQerrorMessage(conn));
	if (PQsendQueryParams(conn, "SELECT pg_catalog.pg_advisory_unlock(1,1)", 0, NULL, NULL, NULL, NULL, 0) != 1)
		pg_fatal("failed to send query: %s", PQerrorMessage(conn));
	PQsendFlushRequest(conn);
	res = PQgetResult(conn);
	if (res == NULL)
		pg_fatal("unexpected NULL result received");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pg_fatal("unexpected result code %s", PQresStatus(PQresultStatus(res)));
	if (PQexitPipelineMode(conn) != 1)
		pg_fatal("failed to exit pipeline mode: %s", PQerrorMessage(conn));
	fprintf(stderr, "ok - 2\n");
}

static void
test_simple_pipeline(PGconn *conn)
{
	PGresult   *res = NULL;
	const char *dummy_params[1] = {"1"};
	Oid			dummy_param_oids[1] = {INT4OID};

	fprintf(stderr, "simple pipeline... ");

	/*
	 * Enter pipeline mode and dispatch a set of operations, which we'll then
	 * process the results of as they come in.
	 *
	 * For a simple case we should be able to do this without interim
	 * processing of results since our output buffer will give us enough slush
	 * to work with and we won't block on sending. So blocking mode is fine.
	 */
	if (PQisnonblocking(conn))
		pg_fatal("Expected blocking connection mode");

	if (PQenterPipelineMode(conn) != 1)
		pg_fatal("failed to enter pipeline mode: %s", PQerrorMessage(conn));

	if (PQsendQueryParams(conn, "SELECT $1",
						  1, dummy_param_oids, dummy_params,
						  NULL, NULL, 0) != 1)
		pg_fatal("dispatching SELECT failed: %s", PQerrorMessage(conn));

	if (PQexitPipelineMode(conn) != 0)
		pg_fatal("exiting pipeline mode with work in progress should fail, but succeeded");

	if (PQpipelineSync(conn) != 1)
		pg_fatal("pipeline sync failed: %s", PQerrorMessage(conn));

	res = PQgetResult(conn);
	if (res == NULL)
		pg_fatal("PQgetResult returned null when there's a pipeline item: %s",
				 PQerrorMessage(conn));

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pg_fatal("Unexpected result code %s from first pipeline item",
				 PQresStatus(PQresultStatus(res)));

	PQclear(res);
	res = NULL;

	if (PQgetResult(conn) != NULL)
		pg_fatal("PQgetResult returned something extra after first query result.");

	/*
	 * Even though we've processed the result there's still a sync to come and
	 * we can't exit pipeline mode yet
	 */
	if (PQexitPipelineMode(conn) != 0)
		pg_fatal("exiting pipeline mode after query but before sync succeeded incorrectly");

	res = PQgetResult(conn);
	if (res == NULL)
		pg_fatal("PQgetResult returned null when sync result PGRES_PIPELINE_SYNC expected: %s",
				 PQerrorMessage(conn));

	if (PQresultStatus(res) != PGRES_PIPELINE_SYNC)
		pg_fatal("Unexpected result code %s instead of PGRES_PIPELINE_SYNC, error: %s",
				 PQresStatus(PQresultStatus(res)), PQerrorMessage(conn));

	PQclear(res);
	res = NULL;

	if (PQgetResult(conn) != NULL)
		pg_fatal("PQgetResult returned something extra after pipeline end: %s",
				 PQresStatus(PQresultStatus(res)));

	/* We're still in pipeline mode... */
	if (PQpipelineStatus(conn) == PQ_PIPELINE_OFF)
		pg_fatal("Fell out of pipeline mode somehow");

	/* ... until we end it, which we can safely do now */
	if (PQexitPipelineMode(conn) != 1)
		pg_fatal("attempt to exit pipeline mode failed when it should've succeeded: %s",
				 PQerrorMessage(conn));

	if (PQpipelineStatus(conn) != PQ_PIPELINE_OFF)
		pg_fatal("Exiting pipeline mode didn't seem to work");

	fprintf(stderr, "ok\n");
}

static void
test_singlerowmode(PGconn *conn)
{
	PGresult   *res;
	int			i;
	bool		pipeline_ended = false;

	if (PQenterPipelineMode(conn) != 1)
		pg_fatal("failed to enter pipeline mode: %s",
				 PQerrorMessage(conn));

	/* One series of three commands, using single-row mode for the first two. */
	for (i = 0; i < 3; i++)
	{
		char	   *param[1];

		param[0] = psprintf("%d", 44 + i);

		if (PQsendQueryParams(conn,
							  "SELECT generate_series(42, $1)",
							  1,
							  NULL,
							  (const char **) param,
							  NULL,
							  NULL,
							  0) != 1)
			pg_fatal("failed to send query: %s",
					 PQerrorMessage(conn));
		pfree(param[0]);
	}
	if (PQpipelineSync(conn) != 1)
		pg_fatal("pipeline sync failed: %s", PQerrorMessage(conn));

	for (i = 0; !pipeline_ended; i++)
	{
		bool		first = true;
		bool		saw_ending_tuplesok;
		bool		isSingleTuple = false;

		/* Set single row mode for only first 2 SELECT queries */
		if (i < 2)
		{
			if (PQsetSingleRowMode(conn) != 1)
				pg_fatal("PQsetSingleRowMode() failed for i=%d", i);
		}

		/* Consume rows for this query */
		saw_ending_tuplesok = false;
		while ((res = PQgetResult(conn)) != NULL)
		{
			ExecStatusType est = PQresultStatus(res);

			if (est == PGRES_PIPELINE_SYNC)
			{
				fprintf(stderr, "end of pipeline reached\n");
				pipeline_ended = true;
				PQclear(res);
				if (i != 3)
					pg_fatal("Expected three results, got %d", i);
				break;
			}

			/* Expect SINGLE_TUPLE for queries 0 and 1, TUPLES_OK for 2 */
			if (first)
			{
				if (i <= 1 && est != PGRES_SINGLE_TUPLE)
					pg_fatal("Expected PGRES_SINGLE_TUPLE for query %d, got %s",
							 i, PQresStatus(est));
				if (i >= 2 && est != PGRES_TUPLES_OK)
					pg_fatal("Expected PGRES_TUPLES_OK for query %d, got %s",
							 i, PQresStatus(est));
				first = false;
			}

			fprintf(stderr, "Result status %s for query %d", PQresStatus(est), i);
			switch (est)
			{
				case PGRES_TUPLES_OK:
					fprintf(stderr, ", tuples: %d\n", PQntuples(res));
					saw_ending_tuplesok = true;
					if (isSingleTuple)
					{
						if (PQntuples(res) == 0)
							fprintf(stderr, "all tuples received in query %d\n", i);
						else
							pg_fatal("Expected to follow PGRES_SINGLE_TUPLE, but received PGRES_TUPLES_OK directly instead");
					}
					break;

				case PGRES_SINGLE_TUPLE:
					isSingleTuple = true;
					fprintf(stderr, ", %d tuple: %s\n", PQntuples(res), PQgetvalue(res, 0, 0));
					break;

				default:
					pg_fatal("unexpected");
			}
			PQclear(res);
		}
		if (!pipeline_ended && !saw_ending_tuplesok)
			pg_fatal("didn't get expected terminating TUPLES_OK");
	}

	/*
	 * Now issue one command, get its results in with single-row mode, then
	 * issue another command, and get its results in normal mode; make sure
	 * the single-row mode flag is reset as expected.
	 */
	if (PQsendQueryParams(conn, "SELECT generate_series(0, 0)",
						  0, NULL, NULL, NULL, NULL, 0) != 1)
		pg_fatal("failed to send query: %s",
				 PQerrorMessage(conn));
	if (PQsendFlushRequest(conn) != 1)
		pg_fatal("failed to send flush request");
	if (PQsetSingleRowMode(conn) != 1)
		pg_fatal("PQsetSingleRowMode() failed");
	res = PQgetResult(conn);
	if (res == NULL)
		pg_fatal("unexpected NULL");
	if (PQresultStatus(res) != PGRES_SINGLE_TUPLE)
		pg_fatal("Expected PGRES_SINGLE_TUPLE, got %s",
				 PQresStatus(PQresultStatus(res)));
	res = PQgetResult(conn);
	if (res == NULL)
		pg_fatal("unexpected NULL");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pg_fatal("Expected PGRES_TUPLES_OK, got %s",
				 PQresStatus(PQresultStatus(res)));
	if (PQgetResult(conn) != NULL)
		pg_fatal("expected NULL result");

	if (PQsendQueryParams(conn, "SELECT 1",
						  0, NULL, NULL, NULL, NULL, 0) != 1)
		pg_fatal("failed to send query: %s",
				 PQerrorMessage(conn));
	if (PQsendFlushRequest(conn) != 1)
		pg_fatal("failed to send flush request");
	res = PQgetResult(conn);
	if (res == NULL)
		pg_fatal("unexpected NULL");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pg_fatal("Expected PGRES_TUPLES_OK, got %s",
				 PQresStatus(PQresultStatus(res)));
	if (PQgetResult(conn) != NULL)
		pg_fatal("expected NULL result");

	if (PQexitPipelineMode(conn) != 1)
		pg_fatal("failed to end pipeline mode: %s", PQerrorMessage(conn));

	fprintf(stderr, "ok\n");
}

/*
 * Simple test to verify that a pipeline is discarded as a whole when there's
 * an error, ignoring transaction commands.
 */
static void
test_transaction(PGconn *conn)
{
	PGresult   *res;
	bool		expect_null;
	int			num_syncs = 0;

	res = PQexec(conn, "DROP TABLE IF EXISTS pq_pipeline_tst;"
				 "CREATE TABLE pq_pipeline_tst (id int)");
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pg_fatal("failed to create test table: %s",
				 PQerrorMessage(conn));
	PQclear(res);

	if (PQenterPipelineMode(conn) != 1)
		pg_fatal("failed to enter pipeline mode: %s",
				 PQerrorMessage(conn));
	if (PQsendPrepare(conn, "rollback", "ROLLBACK", 0, NULL) != 1)
		pg_fatal("could not send prepare on pipeline: %s",
				 PQerrorMessage(conn));

	if (PQsendQueryParams(conn,
						  "BEGIN",
						  0, NULL, NULL, NULL, NULL, 0) != 1)
		pg_fatal("failed to send query: %s",
				 PQerrorMessage(conn));
	if (PQsendQueryParams(conn,
						  "SELECT 0/0",
						  0, NULL, NULL, NULL, NULL, 0) != 1)
		pg_fatal("failed to send query: %s",
				 PQerrorMessage(conn));

	/*
	 * send a ROLLBACK using a prepared stmt. Doesn't work because we need to
	 * get out of the pipeline-aborted state first.
	 */
	if (PQsendQueryPrepared(conn, "rollback", 0, NULL, NULL, NULL, 1) != 1)
		pg_fatal("failed to execute prepared: %s",
				 PQerrorMessage(conn));

	/* This insert fails because we're in pipeline-aborted state */
	if (PQsendQueryParams(conn,
						  "INSERT INTO pq_pipeline_tst VALUES (1)",
						  0, NULL, NULL, NULL, NULL, 0) != 1)
		pg_fatal("failed to send query: %s",
				 PQerrorMessage(conn));
	if (PQpipelineSync(conn) != 1)
		pg_fatal("pipeline sync failed: %s", PQerrorMessage(conn));
	num_syncs++;

	/*
	 * This insert fails even though the pipeline got a SYNC, because we're in
	 * an aborted transaction
	 */
	if (PQsendQueryParams(conn,
						  "INSERT INTO pq_pipeline_tst VALUES (2)",
						  0, NULL, NULL, NULL, NULL, 0) != 1)
		pg_fatal("failed to send query: %s",
				 PQerrorMessage(conn));
	if (PQpipelineSync(conn) != 1)
		pg_fatal("pipeline sync failed: %s", PQerrorMessage(conn));
	num_syncs++;

	/*
	 * Send ROLLBACK using prepared stmt. This one works because we just did
	 * PQpipelineSync above.
	 */
	if (PQsendQueryPrepared(conn, "rollback", 0, NULL, NULL, NULL, 1) != 1)
		pg_fatal("failed to execute prepared: %s",
				 PQerrorMessage(conn));

	/*
	 * Now that we're out of a transaction and in pipeline-good mode, this
	 * insert works
	 */
	if (PQsendQueryParams(conn,
						  "INSERT INTO pq_pipeline_tst VALUES (3)",
						  0, NULL, NULL, NULL, NULL, 0) != 1)
		pg_fatal("failed to send query: %s",
				 PQerrorMessage(conn));
	/* Send two syncs now -- match up to SYNC messages below */
	if (PQpipelineSync(conn) != 1)
		pg_fatal("pipeline sync failed: %s", PQerrorMessage(conn));
	num_syncs++;
	if (PQpipelineSync(conn) != 1)
		pg_fatal("pipeline sync failed: %s", PQerrorMessage(conn));
	num_syncs++;

	expect_null = false;
	for (int i = 0;; i++)
	{
		ExecStatusType restype;

		res = PQgetResult(conn);
		if (res == NULL)
		{
			printf("%d: got NULL result\n", i);
			if (!expect_null)
				pg_fatal("did not expect NULL here");
			expect_null = false;
			continue;
		}
		restype = PQresultStatus(res);
		printf("%d: got status %s", i, PQresStatus(restype));
		if (expect_null)
			pg_fatal("expected NULL");
		if (restype == PGRES_FATAL_ERROR)
			printf("; error: %s", PQerrorMessage(conn));
		else if (restype == PGRES_PIPELINE_ABORTED)
		{
			printf(": command didn't run because pipeline aborted\n");
		}
		else
			printf("\n");
		PQclear(res);

		if (restype == PGRES_PIPELINE_SYNC)
			num_syncs--;
		else
			expect_null = true;
		if (num_syncs <= 0)
			break;
	}
	if (PQgetResult(conn) != NULL)
		pg_fatal("returned something extra after all the syncs: %s",
				 PQresStatus(PQresultStatus(res)));

	if (PQexitPipelineMode(conn) != 1)
		pg_fatal("failed to end pipeline mode: %s", PQerrorMessage(conn));

	/* We expect to find one tuple containing the value "3" */
	res = PQexec(conn, "SELECT * FROM pq_pipeline_tst");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pg_fatal("failed to obtain result: %s", PQerrorMessage(conn));
	if (PQntuples(res) != 1)
		pg_fatal("did not get 1 tuple");
	if (strcmp(PQgetvalue(res, 0, 0), "3") != 0)
		pg_fatal("did not get expected tuple");
	PQclear(res);

	fprintf(stderr, "ok\n");
}

/*
 * In this test mode we send a stream of queries, with one in the middle
 * causing an error.  Verify that we can still send some more after the
 * error and have libpq work properly.
 */
static void
test_uniqviol(PGconn *conn)
{
	int			sock = PQsocket(conn);
	PGresult   *res;
	Oid			paramTypes[2] = {INT8OID, INT8OID};
	const char *paramValues[2];
	char		paramValue0[MAXINT8LEN];
	char		paramValue1[MAXINT8LEN];
	int			ctr = 0;
	int			numsent = 0;
	int			results = 0;
	bool		read_done = false;
	bool		write_done = false;
	bool		error_sent = false;
	bool		got_error = false;
	int			switched = 0;
	int			socketful = 0;
	fd_set		in_fds;
	fd_set		out_fds;

	fprintf(stderr, "uniqviol ...");

	PQsetnonblocking(conn, 1);

	paramValues[0] = paramValue0;
	paramValues[1] = paramValue1;
	sprintf(paramValue1, "42");

	res = PQexec(conn, "drop table if exists ppln_uniqviol;"
				 "create table ppln_uniqviol(id bigint primary key, idata bigint)");
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pg_fatal("failed to create table: %s", PQerrorMessage(conn));

	res = PQexec(conn, "begin");
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pg_fatal("failed to begin transaction: %s", PQerrorMessage(conn));

	res = PQprepare(conn, "insertion",
					"insert into ppln_uniqviol values ($1, $2) returning id",
					2, paramTypes);
	if (res == NULL || PQresultStatus(res) != PGRES_COMMAND_OK)
		pg_fatal("failed to prepare query: %s", PQerrorMessage(conn));

	if (PQenterPipelineMode(conn) != 1)
		pg_fatal("failed to enter pipeline mode");

	while (!read_done)
	{
		/*
		 * Avoid deadlocks by reading everything the server has sent before
		 * sending anything.  (Special precaution is needed here to process
		 * PQisBusy before testing the socket for read-readiness, because the
		 * socket does not turn read-ready after "sending" queries in aborted
		 * pipeline mode.)
		 */
		while (PQisBusy(conn) == 0)
		{
			bool		new_error;

			if (results >= numsent)
			{
				if (write_done)
					read_done = true;
				break;
			}

			res = PQgetResult(conn);
			new_error = process_result(conn, res, results, numsent);
			if (new_error && got_error)
				pg_fatal("got two errors");
			got_error |= new_error;
			if (results++ >= numsent - 1)
			{
				if (write_done)
					read_done = true;
				break;
			}
		}

		if (read_done)
			break;

		FD_ZERO(&out_fds);
		FD_SET(sock, &out_fds);

		FD_ZERO(&in_fds);
		FD_SET(sock, &in_fds);

		if (select(sock + 1, &in_fds, write_done ? NULL : &out_fds, NULL, NULL) == -1)
		{
			if (errno == EINTR)
				continue;
			pg_fatal("select() failed: %m");
		}

		if (FD_ISSET(sock, &in_fds) && PQconsumeInput(conn) == 0)
			pg_fatal("PQconsumeInput failed: %s", PQerrorMessage(conn));

		/*
		 * If the socket is writable and we haven't finished sending queries,
		 * send some.
		 */
		if (!write_done && FD_ISSET(sock, &out_fds))
		{
			for (;;)
			{
				int			flush;

				/*
				 * provoke uniqueness violation exactly once after having
				 * switched to read mode.
				 */
				if (switched >= 1 && !error_sent && ctr % socketful >= socketful / 2)
				{
					sprintf(paramValue0, "%d", numsent / 2);
					fprintf(stderr, "E");
					error_sent = true;
				}
				else
				{
					fprintf(stderr, ".");
					sprintf(paramValue0, "%d", ctr++);
				}

				if (PQsendQueryPrepared(conn, "insertion", 2, paramValues, NULL, NULL, 0) != 1)
					pg_fatal("failed to execute prepared query: %s", PQerrorMessage(conn));
				numsent++;

				/* Are we done writing? */
				if (socketful != 0 && numsent % socketful == 42 && error_sent)
				{
					if (PQsendFlushRequest(conn) != 1)
						pg_fatal("failed to send flush request");
					write_done = true;
					fprintf(stderr, "\ndone writing\n");
					PQflush(conn);
					break;
				}

				/* is the outgoing socket full? */
				flush = PQflush(conn);
				if (flush == -1)
					pg_fatal("failed to flush: %s", PQerrorMessage(conn));
				if (flush == 1)
				{
					if (socketful == 0)
						socketful = numsent;
					fprintf(stderr, "\nswitch to reading\n");
					switched++;
					break;
				}
			}
		}
	}

	if (!got_error)
		pg_fatal("did not get expected error");

	fprintf(stderr, "ok\n");
}

/*
 * Subroutine for test_uniqviol; given a PGresult, print it out and consume
 * the expected NULL that should follow it.
 *
 * Returns true if we read a fatal error message, otherwise false.
 */
static bool
process_result(PGconn *conn, PGresult *res, int results, int numsent)
{
	PGresult   *res2;
	bool		got_error = false;

	if (res == NULL)
		pg_fatal("got unexpected NULL");

	switch (PQresultStatus(res))
	{
		case PGRES_FATAL_ERROR:
			got_error = true;
			fprintf(stderr, "result %d/%d (error): %s\n", results, numsent, PQerrorMessage(conn));
			PQclear(res);

			res2 = PQgetResult(conn);
			if (res2 != NULL)
				pg_fatal("expected NULL, got %s",
						 PQresStatus(PQresultStatus(res2)));
			break;

		case PGRES_TUPLES_OK:
			fprintf(stderr, "result %d/%d: %s\n", results, numsent, PQgetvalue(res, 0, 0));
			PQclear(res);

			res2 = PQgetResult(conn);
			if (res2 != NULL)
				pg_fatal("expected NULL, got %s",
						 PQresStatus(PQresultStatus(res2)));
			break;

		case PGRES_PIPELINE_ABORTED:
			fprintf(stderr, "result %d/%d: pipeline aborted\n", results, numsent);
			res2 = PQgetResult(conn);
			if (res2 != NULL)
				pg_fatal("expected NULL, got %s",
						 PQresStatus(PQresultStatus(res2)));
			break;

		default:
			pg_fatal("got unexpected %s", PQresStatus(PQresultStatus(res)));
	}

	return got_error;
}


static void
usage(const char *progname)
{
	fprintf(stderr, "%s tests libpq's pipeline mode.\n\n", progname);
	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "  %s [OPTION] tests\n", progname);
	fprintf(stderr, "  %s [OPTION] TESTNAME [CONNINFO]\n", progname);
	fprintf(stderr, "\nOptions:\n");
	fprintf(stderr, "  -t TRACEFILE       generate a libpq trace to TRACEFILE\n");
	fprintf(stderr, "  -r NUMROWS         use NUMROWS as the test size\n");
}

static void
print_test_list(void)
{
	printf("disallowed_in_pipeline\n");
	printf("multi_pipelines\n");
	printf("nosync\n");
	printf("pipeline_abort\n");
	printf("pipeline_idle\n");
	printf("pipelined_insert\n");
	printf("prepared\n");
	printf("simple_pipeline\n");
	printf("singlerow\n");
	printf("transaction\n");
	printf("uniqviol\n");
}

int
main(int argc, char **argv)
{
	const char *conninfo = "";
	PGconn	   *conn;
	FILE	   *trace;
	char	   *testname;
	int			numrows = 10000;
	PGresult   *res;
	int			c;

	while ((c = getopt(argc, argv, "t:r:")) != -1)
	{
		switch (c)
		{
			case 't':			/* trace file */
				tracefile = pg_strdup(optarg);
				break;
			case 'r':			/* numrows */
				errno = 0;
				numrows = strtol(optarg, NULL, 10);
				if (errno != 0 || numrows <= 0)
				{
					fprintf(stderr, "couldn't parse \"%s\" as a positive integer\n",
							optarg);
					exit(1);
				}
				break;
		}
	}

	if (optind < argc)
	{
		testname = pg_strdup(argv[optind]);
		optind++;
	}
	else
	{
		usage(argv[0]);
		exit(1);
	}

	if (strcmp(testname, "tests") == 0)
	{
		print_test_list();
		exit(0);
	}

	if (optind < argc)
	{
		conninfo = pg_strdup(argv[optind]);
		optind++;
	}

	/* Make a connection to the database */
	conn = PQconnectdb(conninfo);
	if (PQstatus(conn) != CONNECTION_OK)
	{
		fprintf(stderr, "Connection to database failed: %s\n",
				PQerrorMessage(conn));
		exit_nicely(conn);
	}

	res = PQexec(conn, "SET lc_messages TO \"C\"");
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pg_fatal("failed to set lc_messages: %s", PQerrorMessage(conn));
	res = PQexec(conn, "SET force_parallel_mode = off");
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pg_fatal("failed to set force_parallel_mode: %s", PQerrorMessage(conn));

	/* Set the trace file, if requested */
	if (tracefile != NULL)
	{
		if (strcmp(tracefile, "-") == 0)
			trace = stdout;
		else
			trace = fopen(tracefile, "w");
		if (trace == NULL)
			pg_fatal("could not open file \"%s\": %m", tracefile);

		/* Make it line-buffered */
		setvbuf(trace, NULL, PG_IOLBF, 0);

		PQtrace(conn, trace);
		PQsetTraceFlags(conn,
						PQTRACE_SUPPRESS_TIMESTAMPS | PQTRACE_REGRESS_MODE);
	}

	if (strcmp(testname, "disallowed_in_pipeline") == 0)
		test_disallowed_in_pipeline(conn);
	else if (strcmp(testname, "multi_pipelines") == 0)
		test_multi_pipelines(conn);
	else if (strcmp(testname, "nosync") == 0)
		test_nosync(conn);
	else if (strcmp(testname, "pipeline_abort") == 0)
		test_pipeline_abort(conn);
	else if (strcmp(testname, "pipeline_idle") == 0)
		test_pipeline_idle(conn);
	else if (strcmp(testname, "pipelined_insert") == 0)
		test_pipelined_insert(conn, numrows);
	else if (strcmp(testname, "prepared") == 0)
		test_prepared(conn);
	else if (strcmp(testname, "simple_pipeline") == 0)
		test_simple_pipeline(conn);
	else if (strcmp(testname, "singlerow") == 0)
		test_singlerowmode(conn);
	else if (strcmp(testname, "transaction") == 0)
		test_transaction(conn);
	else if (strcmp(testname, "uniqviol") == 0)
		test_uniqviol(conn);
	else
	{
		fprintf(stderr, "\"%s\" is not a recognized test name\n", testname);
		exit(1);
	}

	/* close the connection to the database and cleanup */
	PQfinish(conn);
	return 0;
}
