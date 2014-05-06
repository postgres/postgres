/*
 * src/test/isolation/isolationtester.c
 *
 * isolationtester.c
 *		Runs an isolation test specified by a spec file.
 */

#include "postgres_fe.h"

#ifdef WIN32
#include <windows.h>
#endif
#include <sys/time.h>
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#include "libpq-fe.h"
#include "pqexpbuffer.h"
#include "pg_getopt.h"

#include "isolationtester.h"

#define PREP_WAITING "isolationtester_waiting"

/*
 * conns[0] is the global setup, teardown, and watchdog connection.  Additional
 * connections represent spec-defined sessions.
 */
static PGconn **conns = NULL;
static const char **backend_pids = NULL;
static int	nconns = 0;

/* In dry run only output permutations to be run by the tester. */
static int	dry_run = false;

static void run_testspec(TestSpec *testspec);
static void run_all_permutations(TestSpec *testspec);
static void run_all_permutations_recurse(TestSpec *testspec, int nsteps,
							 Step **steps);
static void run_named_permutations(TestSpec *testspec);
static void run_permutation(TestSpec *testspec, int nsteps, Step **steps);

#define STEP_NONBLOCK	0x1		/* return 0 as soon as cmd waits for a lock */
#define STEP_RETRY		0x2		/* this is a retry of a previously-waiting cmd */
static bool try_complete_step(Step *step, int flags);

static int	step_qsort_cmp(const void *a, const void *b);
static int	step_bsearch_cmp(const void *a, const void *b);

static void printResultSet(PGresult *res);

/* close all connections and exit */
static void
exit_nicely(void)
{
	int			i;

	for (i = 0; i < nconns; i++)
		PQfinish(conns[i]);
	exit(1);
}

int
main(int argc, char **argv)
{
	const char *conninfo;
	TestSpec   *testspec;
	int			i;
	PGresult   *res;
	PQExpBufferData wait_query;
	int			opt;

	while ((opt = getopt(argc, argv, "nV")) != -1)
	{
		switch (opt)
		{
			case 'n':
				dry_run = true;
				break;
			case 'V':
				puts("isolationtester (PostgreSQL) " PG_VERSION);
				exit(0);
			default:
				fprintf(stderr, "Usage: isolationtester [-n] [CONNINFO]\n");
				return EXIT_FAILURE;
		}
	}

	/*
	 * Make stdout unbuffered to match stderr; and ensure stderr is unbuffered
	 * too, which it should already be everywhere except sometimes in Windows.
	 */
	setbuf(stdout, NULL);
	setbuf(stderr, NULL);

	/*
	 * If the user supplies a non-option parameter on the command line, use it
	 * as the conninfo string; otherwise default to setting dbname=postgres
	 * and using environment variables or defaults for all other connection
	 * parameters.
	 */
	if (argc > optind)
		conninfo = argv[optind];
	else
		conninfo = "dbname = postgres";

	/* Read the test spec from stdin */
	spec_yyparse();
	testspec = &parseresult;

	/*
	 * In dry-run mode, just print the permutations that would be run, and
	 * exit.
	 */
	if (dry_run)
	{
		run_testspec(testspec);
		return 0;
	}

	printf("Parsed test spec with %d sessions\n", testspec->nsessions);

	/*
	 * Establish connections to the database, one for each session and an
	 * extra for lock wait detection and global work.
	 */
	nconns = 1 + testspec->nsessions;
	conns = calloc(nconns, sizeof(PGconn *));
	backend_pids = calloc(nconns, sizeof(*backend_pids));
	for (i = 0; i < nconns; i++)
	{
		conns[i] = PQconnectdb(conninfo);
		if (PQstatus(conns[i]) != CONNECTION_OK)
		{
			fprintf(stderr, "Connection %d to database failed: %s",
					i, PQerrorMessage(conns[i]));
			exit_nicely();
		}

		/*
		 * Suppress NOTIFY messages, which otherwise pop into results at odd
		 * places.
		 */
		res = PQexec(conns[i], "SET client_min_messages = warning;");
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			fprintf(stderr, "message level setup failed: %s", PQerrorMessage(conns[i]));
			exit_nicely();
		}
		PQclear(res);

		/* Get the backend pid for lock wait checking. */
		res = PQexec(conns[i], "SELECT pg_backend_pid()");
		if (PQresultStatus(res) == PGRES_TUPLES_OK)
		{
			if (PQntuples(res) == 1 && PQnfields(res) == 1)
				backend_pids[i] = strdup(PQgetvalue(res, 0, 0));
			else
			{
				fprintf(stderr, "backend pid query returned %d rows and %d columns, expected 1 row and 1 column",
						PQntuples(res), PQnfields(res));
				exit_nicely();
			}
		}
		else
		{
			fprintf(stderr, "backend pid query failed: %s",
					PQerrorMessage(conns[i]));
			exit_nicely();
		}
		PQclear(res);
	}

	/* Set the session index fields in steps. */
	for (i = 0; i < testspec->nsessions; i++)
	{
		Session    *session = testspec->sessions[i];
		int			stepindex;

		for (stepindex = 0; stepindex < session->nsteps; stepindex++)
			session->steps[stepindex]->session = i;
	}

	/*
	 * Build the query we'll use to detect lock contention among sessions in
	 * the test specification.  Most of the time, we could get away with
	 * simply checking whether a session is waiting for *any* lock: we don't
	 * exactly expect concurrent use of test tables.  However, autovacuum will
	 * occasionally take AccessExclusiveLock to truncate a table, and we must
	 * ignore that transient wait.
	 */
	initPQExpBuffer(&wait_query);
	appendPQExpBufferStr(&wait_query,
						 "SELECT 1 FROM pg_locks holder, pg_locks waiter "
						 "WHERE NOT waiter.granted AND waiter.pid = $1 "
						 "AND holder.granted "
						 "AND holder.pid <> $1 AND holder.pid IN (");
	/* The spec syntax requires at least one session; assume that here. */
	appendPQExpBufferStr(&wait_query, backend_pids[1]);
	for (i = 2; i < nconns; i++)
		appendPQExpBuffer(&wait_query, ", %s", backend_pids[i]);
	appendPQExpBufferStr(&wait_query,
						 ") "

						 "AND holder.mode = ANY (CASE waiter.mode "
						 "WHEN 'AccessShareLock' THEN ARRAY["
						 "'AccessExclusiveLock'] "
						 "WHEN 'RowShareLock' THEN ARRAY["
						 "'ExclusiveLock',"
						 "'AccessExclusiveLock'] "
						 "WHEN 'RowExclusiveLock' THEN ARRAY["
						 "'ShareLock',"
						 "'ShareRowExclusiveLock',"
						 "'ExclusiveLock',"
						 "'AccessExclusiveLock'] "
						 "WHEN 'ShareUpdateExclusiveLock' THEN ARRAY["
						 "'ShareUpdateExclusiveLock',"
						 "'ShareLock',"
						 "'ShareRowExclusiveLock',"
						 "'ExclusiveLock',"
						 "'AccessExclusiveLock'] "
						 "WHEN 'ShareLock' THEN ARRAY["
						 "'RowExclusiveLock',"
						 "'ShareUpdateExclusiveLock',"
						 "'ShareRowExclusiveLock',"
						 "'ExclusiveLock',"
						 "'AccessExclusiveLock'] "
						 "WHEN 'ShareRowExclusiveLock' THEN ARRAY["
						 "'RowExclusiveLock',"
						 "'ShareUpdateExclusiveLock',"
						 "'ShareLock',"
						 "'ShareRowExclusiveLock',"
						 "'ExclusiveLock',"
						 "'AccessExclusiveLock'] "
						 "WHEN 'ExclusiveLock' THEN ARRAY["
						 "'RowShareLock',"
						 "'RowExclusiveLock',"
						 "'ShareUpdateExclusiveLock',"
						 "'ShareLock',"
						 "'ShareRowExclusiveLock',"
						 "'ExclusiveLock',"
						 "'AccessExclusiveLock'] "
						 "WHEN 'AccessExclusiveLock' THEN ARRAY["
						 "'AccessShareLock',"
						 "'RowShareLock',"
						 "'RowExclusiveLock',"
						 "'ShareUpdateExclusiveLock',"
						 "'ShareLock',"
						 "'ShareRowExclusiveLock',"
						 "'ExclusiveLock',"
						 "'AccessExclusiveLock'] END) "

				  "AND holder.locktype IS NOT DISTINCT FROM waiter.locktype "
				  "AND holder.database IS NOT DISTINCT FROM waiter.database "
				  "AND holder.relation IS NOT DISTINCT FROM waiter.relation "
						 "AND holder.page IS NOT DISTINCT FROM waiter.page "
						 "AND holder.tuple IS NOT DISTINCT FROM waiter.tuple "
			  "AND holder.virtualxid IS NOT DISTINCT FROM waiter.virtualxid "
		"AND holder.transactionid IS NOT DISTINCT FROM waiter.transactionid "
					"AND holder.classid IS NOT DISTINCT FROM waiter.classid "
						 "AND holder.objid IS NOT DISTINCT FROM waiter.objid "
				"AND holder.objsubid IS NOT DISTINCT FROM waiter.objsubid ");

	res = PQprepare(conns[0], PREP_WAITING, wait_query.data, 0, NULL);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "prepare of lock wait query failed: %s",
				PQerrorMessage(conns[0]));
		exit_nicely();
	}
	PQclear(res);
	termPQExpBuffer(&wait_query);

	/*
	 * Run the permutations specified in the spec, or all if none were
	 * explicitly specified.
	 */
	run_testspec(testspec);

	/* Clean up and exit */
	for (i = 0; i < nconns; i++)
		PQfinish(conns[i]);
	return 0;
}

static int *piles;

/*
 * Run the permutations specified in the spec, or all if none were
 * explicitly specified.
 */
static void
run_testspec(TestSpec *testspec)
{
	if (testspec->permutations)
		run_named_permutations(testspec);
	else
		run_all_permutations(testspec);
}

/*
 * Run all permutations of the steps and sessions.
 */
static void
run_all_permutations(TestSpec *testspec)
{
	int			nsteps;
	int			i;
	Step	  **steps;

	/* Count the total number of steps in all sessions */
	nsteps = 0;
	for (i = 0; i < testspec->nsessions; i++)
		nsteps += testspec->sessions[i]->nsteps;

	steps = malloc(sizeof(Step *) * nsteps);

	/*
	 * To generate the permutations, we conceptually put the steps of each
	 * session on a pile. To generate a permutation, we pick steps from the
	 * piles until all piles are empty. By picking steps from piles in
	 * different order, we get different permutations.
	 *
	 * A pile is actually just an integer which tells how many steps we've
	 * already picked from this pile.
	 */
	piles = malloc(sizeof(int) * testspec->nsessions);
	for (i = 0; i < testspec->nsessions; i++)
		piles[i] = 0;

	run_all_permutations_recurse(testspec, 0, steps);
}

static void
run_all_permutations_recurse(TestSpec *testspec, int nsteps, Step **steps)
{
	int			i;
	int			found = 0;

	for (i = 0; i < testspec->nsessions; i++)
	{
		/* If there's any more steps in this pile, pick it and recurse */
		if (piles[i] < testspec->sessions[i]->nsteps)
		{
			steps[nsteps] = testspec->sessions[i]->steps[piles[i]];
			piles[i]++;

			run_all_permutations_recurse(testspec, nsteps + 1, steps);

			piles[i]--;

			found = 1;
		}
	}

	/* If all the piles were empty, this permutation is completed. Run it */
	if (!found)
		run_permutation(testspec, nsteps, steps);
}

/*
 * Run permutations given in the test spec
 */
static void
run_named_permutations(TestSpec *testspec)
{
	int			i,
				j;
	int			n;
	int			nallsteps;
	Step	  **allsteps;

	/* First create a lookup table of all steps */
	nallsteps = 0;
	for (i = 0; i < testspec->nsessions; i++)
		nallsteps += testspec->sessions[i]->nsteps;

	allsteps = malloc(nallsteps * sizeof(Step *));

	n = 0;
	for (i = 0; i < testspec->nsessions; i++)
	{
		for (j = 0; j < testspec->sessions[i]->nsteps; j++)
			allsteps[n++] = testspec->sessions[i]->steps[j];
	}

	qsort(allsteps, nallsteps, sizeof(Step *), &step_qsort_cmp);

	for (i = 0; i < testspec->npermutations; i++)
	{
		Permutation *p = testspec->permutations[i];
		Step	  **steps;

		steps = malloc(p->nsteps * sizeof(Step *));

		/* Find all the named steps using the lookup table */
		for (j = 0; j < p->nsteps; j++)
		{
			Step	  **this = (Step **) bsearch(p->stepnames[j], allsteps,
												 nallsteps, sizeof(Step *),
												 &step_bsearch_cmp);

			if (this == NULL)
			{
				fprintf(stderr, "undefined step \"%s\" specified in permutation\n",
						p->stepnames[j]);
				exit_nicely();
			}
			steps[j] = *this;
		}

		/* And run them */
		run_permutation(testspec, p->nsteps, steps);

		free(steps);
	}
}

static int
step_qsort_cmp(const void *a, const void *b)
{
	Step	   *stepa = *((Step **) a);
	Step	   *stepb = *((Step **) b);

	return strcmp(stepa->name, stepb->name);
}

static int
step_bsearch_cmp(const void *a, const void *b)
{
	char	   *stepname = (char *) a;
	Step	   *step = *((Step **) b);

	return strcmp(stepname, step->name);
}

/*
 * If a step caused an error to be reported, print it out and clear it.
 */
static void
report_error_message(Step *step)
{
	if (step->errormsg)
	{
		fprintf(stdout, "%s\n", step->errormsg);
		free(step->errormsg);
		step->errormsg = NULL;
	}
}

/*
 * As above, but reports messages possibly emitted by two steps.  This is
 * useful when we have a blocked command awakened by another one; we want to
 * report both messages identically, for the case where we don't care which
 * one fails due to a timeout such as deadlock timeout.
 */
static void
report_two_error_messages(Step *step1, Step *step2)
{
	char	   *prefix;

	prefix = psprintf("%s %s", step1->name, step2->name);

	if (step1->errormsg)
	{
		fprintf(stdout, "error in steps %s: %s\n", prefix,
				step1->errormsg);
		free(step1->errormsg);
		step1->errormsg = NULL;
	}
	if (step2->errormsg)
	{
		fprintf(stdout, "error in steps %s: %s\n", prefix,
				step2->errormsg);
		free(step2->errormsg);
		step2->errormsg = NULL;
	}

	free(prefix);
}

/*
 * Run one permutation
 */
static void
run_permutation(TestSpec *testspec, int nsteps, Step **steps)
{
	PGresult   *res;
	int			i;
	Step	   *waiting = NULL;

	/*
	 * In dry run mode, just display the permutation in the same format used
	 * by spec files, and return.
	 */
	if (dry_run)
	{
		printf("permutation");
		for (i = 0; i < nsteps; i++)
			printf(" \"%s\"", steps[i]->name);
		printf("\n");
		return;
	}

	printf("\nstarting permutation:");
	for (i = 0; i < nsteps; i++)
		printf(" %s", steps[i]->name);
	printf("\n");

	/* Perform setup */
	for (i = 0; i < testspec->nsetupsqls; i++)
	{
		res = PQexec(conns[0], testspec->setupsqls[i]);
		if (PQresultStatus(res) == PGRES_TUPLES_OK)
		{
			printResultSet(res);
		}
		else if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			fprintf(stderr, "setup failed: %s", PQerrorMessage(conns[0]));
			exit_nicely();
		}
		PQclear(res);
	}

	/* Perform per-session setup */
	for (i = 0; i < testspec->nsessions; i++)
	{
		if (testspec->sessions[i]->setupsql)
		{
			res = PQexec(conns[i + 1], testspec->sessions[i]->setupsql);
			if (PQresultStatus(res) == PGRES_TUPLES_OK)
			{
				printResultSet(res);
			}
			else if (PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				fprintf(stderr, "setup of session %s failed: %s",
						testspec->sessions[i]->name,
						PQerrorMessage(conns[i + 1]));
				exit_nicely();
			}
			PQclear(res);
		}
	}

	/* Perform steps */
	for (i = 0; i < nsteps; i++)
	{
		Step	   *step = steps[i];
		PGconn	   *conn = conns[1 + step->session];

		if (waiting != NULL && step->session == waiting->session)
		{
			PGcancel   *cancel;
			PGresult   *res;
			int			j;

			/*
			 * This permutation is invalid: it can never happen in real life.
			 *
			 * A session is blocked on an earlier step (waiting) and no
			 * further steps from this session can run until it is unblocked,
			 * but it can only be unblocked by running steps from other
			 * sessions.
			 */
			fprintf(stderr, "invalid permutation detected\n");

			/* Cancel the waiting statement from this session. */
			cancel = PQgetCancel(conn);
			if (cancel != NULL)
			{
				char		buf[256];

				if (!PQcancel(cancel, buf, sizeof(buf)))
					fprintf(stderr, "PQcancel failed: %s\n", buf);

				/* Be sure to consume the error message. */
				while ((res = PQgetResult(conn)) != NULL)
					PQclear(res);

				PQfreeCancel(cancel);
			}

			/*
			 * Now we really have to complete all the running transactions to
			 * make sure teardown doesn't block.
			 */
			for (j = 1; j < nconns; j++)
			{
				res = PQexec(conns[j], "ROLLBACK");
				if (res != NULL)
					PQclear(res);
			}

			goto teardown;
		}

		if (!PQsendQuery(conn, step->sql))
		{
			fprintf(stdout, "failed to send query for step %s: %s\n",
					step->name, PQerrorMessage(conns[1 + step->session]));
			exit_nicely();
		}

		if (waiting != NULL)
		{
			/* Some other step is already waiting: just block. */
			try_complete_step(step, 0);

			/*
			 * See if this step unblocked the waiting step; report both error
			 * messages together if so.
			 */
			if (!try_complete_step(waiting, STEP_NONBLOCK | STEP_RETRY))
			{
				report_two_error_messages(step, waiting);
				waiting = NULL;
			}
			else
				report_error_message(step);
		}
		else
		{
			if (try_complete_step(step, STEP_NONBLOCK))
				waiting = step;
			report_error_message(step);
		}
	}

	/* Finish any waiting query. */
	if (waiting != NULL)
	{
		try_complete_step(waiting, STEP_RETRY);
		report_error_message(waiting);
	}

teardown:
	/* Perform per-session teardown */
	for (i = 0; i < testspec->nsessions; i++)
	{
		if (testspec->sessions[i]->teardownsql)
		{
			res = PQexec(conns[i + 1], testspec->sessions[i]->teardownsql);
			if (PQresultStatus(res) == PGRES_TUPLES_OK)
			{
				printResultSet(res);
			}
			else if (PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				fprintf(stderr, "teardown of session %s failed: %s",
						testspec->sessions[i]->name,
						PQerrorMessage(conns[i + 1]));
				/* don't exit on teardown failure */
			}
			PQclear(res);
		}
	}

	/* Perform teardown */
	if (testspec->teardownsql)
	{
		res = PQexec(conns[0], testspec->teardownsql);
		if (PQresultStatus(res) == PGRES_TUPLES_OK)
		{
			printResultSet(res);
		}
		else if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			fprintf(stderr, "teardown failed: %s",
					PQerrorMessage(conns[0]));
			/* don't exit on teardown failure */
		}
		PQclear(res);
	}
}

/*
 * Our caller already sent the query associated with this step.  Wait for it
 * to either complete or (if given the STEP_NONBLOCK flag) to block while
 * waiting for a lock.  We assume that any lock wait will persist until we
 * have executed additional steps in the permutation.
 *
 * When calling this function on behalf of a given step for a second or later
 * time, pass the STEP_RETRY flag.  This only affects the messages printed.
 *
 * If the connection returns an error, the message is saved in step->errormsg.
 * Caller should call report_error_message shortly after this, to have it
 * printed and cleared.
 *
 * If the STEP_NONBLOCK flag was specified and the query is waiting to acquire
 * a lock, returns true.  Otherwise, returns false.
 */
static bool
try_complete_step(Step *step, int flags)
{
	PGconn	   *conn = conns[1 + step->session];
	fd_set		read_set;
	struct timeval timeout;
	int			sock = PQsocket(conn);
	int			ret;
	PGresult   *res;

	FD_ZERO(&read_set);

	while ((flags & STEP_NONBLOCK) && PQisBusy(conn))
	{
		FD_SET(sock, &read_set);
		timeout.tv_sec = 0;
		timeout.tv_usec = 10000;	/* Check for lock waits every 10ms. */

		ret = select(sock + 1, &read_set, NULL, NULL, &timeout);
		if (ret < 0)			/* error in select() */
		{
			if (errno == EINTR)
				continue;
			fprintf(stderr, "select failed: %s\n", strerror(errno));
			exit_nicely();
		}
		else if (ret == 0)		/* select() timeout: check for lock wait */
		{
			int			ntuples;

			res = PQexecPrepared(conns[0], PREP_WAITING, 1,
								 &backend_pids[step->session + 1],
								 NULL, NULL, 0);
			if (PQresultStatus(res) != PGRES_TUPLES_OK)
			{
				fprintf(stderr, "lock wait query failed: %s",
						PQerrorMessage(conn));
				exit_nicely();
			}
			ntuples = PQntuples(res);
			PQclear(res);

			if (ntuples >= 1)	/* waiting to acquire a lock */
			{
				if (!(flags & STEP_RETRY))
					printf("step %s: %s <waiting ...>\n",
						   step->name, step->sql);
				return true;
			}
			/* else, not waiting: give it more time */
		}
		else if (!PQconsumeInput(conn)) /* select(): data available */
		{
			fprintf(stderr, "PQconsumeInput failed: %s\n",
					PQerrorMessage(conn));
			exit_nicely();
		}
	}

	if (flags & STEP_RETRY)
		printf("step %s: <... completed>\n", step->name);
	else
		printf("step %s: %s\n", step->name, step->sql);

	while ((res = PQgetResult(conn)))
	{
		switch (PQresultStatus(res))
		{
			case PGRES_COMMAND_OK:
				break;
			case PGRES_TUPLES_OK:
				printResultSet(res);
				break;
			case PGRES_FATAL_ERROR:
				if (step->errormsg != NULL)
				{
					printf("WARNING: this step had a leftover error message\n");
					printf("%s\n", step->errormsg);
				}

				/*
				 * Detail may contain XID values, so we want to just show
				 * primary.  Beware however that libpq-generated error results
				 * may not contain subfields, only an old-style message.
				 */
				{
					const char *sev = PQresultErrorField(res,
														 PG_DIAG_SEVERITY);
					const char *msg = PQresultErrorField(res,
													PG_DIAG_MESSAGE_PRIMARY);

					if (sev && msg)
						step->errormsg = psprintf("%s:  %s", sev, msg);
					else
						step->errormsg = pg_strdup(PQresultErrorMessage(res));
				}
				break;
			default:
				printf("unexpected result status: %s\n",
					   PQresStatus(PQresultStatus(res)));
		}
		PQclear(res);
	}

	return false;
}

static void
printResultSet(PGresult *res)
{
	int			nFields;
	int			i,
				j;

	/* first, print out the attribute names */
	nFields = PQnfields(res);
	for (i = 0; i < nFields; i++)
		printf("%-15s", PQfname(res, i));
	printf("\n\n");

	/* next, print out the rows */
	for (i = 0; i < PQntuples(res); i++)
	{
		for (j = 0; j < nFields; j++)
			printf("%-15s", PQgetvalue(res, i, j));
		printf("\n");
	}
}
