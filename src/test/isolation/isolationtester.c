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

#ifndef WIN32
#include <sys/time.h>
#include <unistd.h>
#endif   /* ! WIN32 */

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#include "libpq-fe.h"

#include "isolationtester.h"

#define PREP_WAITING "isolationtester_waiting"

/*
 * conns[0] is the global setup, teardown, and watchdog connection.  Additional
 * connections represent spec-defined sessions.
 */
static PGconn **conns = NULL;
static const char **backend_ids = NULL;
static int	nconns = 0;

static void run_all_permutations(TestSpec * testspec);
static void run_all_permutations_recurse(TestSpec * testspec, int nsteps,
							 Step ** steps);
static void run_named_permutations(TestSpec * testspec);
static void run_permutation(TestSpec * testspec, int nsteps, Step ** steps);

#define STEP_NONBLOCK	0x1 /* return 0 as soon as cmd waits for a lock */
#define STEP_RETRY		0x2 /* this is a retry of a previously-waiting cmd */
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

	/*
	 * If the user supplies a parameter on the command line, use it as the
	 * conninfo string; otherwise default to setting dbname=postgres and using
	 * environment variables or defaults for all other connection parameters.
	 */
	if (argc > 1)
		conninfo = argv[1];
	else
		conninfo = "dbname = postgres";

	/* Read the test spec from stdin */
	spec_yyparse();
	testspec = &parseresult;
	printf("Parsed test spec with %d sessions\n", testspec->nsessions);

	/*
	 * Establish connections to the database, one for each session and an extra
	 * for lock wait detection and global work.
	 */
	nconns = 1 + testspec->nsessions;
	conns = calloc(nconns, sizeof(PGconn *));
	backend_ids = calloc(nconns, sizeof(*backend_ids));
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

		/* Get the backend ID for lock wait checking. */
		res = PQexec(conns[i], "SELECT i FROM pg_stat_get_backend_idset() t(i) "
					 "WHERE pg_stat_get_backend_pid(i) = pg_backend_pid()");
		if (PQresultStatus(res) == PGRES_TUPLES_OK)
		{
			if (PQntuples(res) == 1 && PQnfields(res) == 1)
				backend_ids[i] = strdup(PQgetvalue(res, 0, 0));
			else
			{
				fprintf(stderr, "backend id query returned %d rows and %d columns, expected 1 row and 1 column",
						PQntuples(res), PQnfields(res));
				exit_nicely();
			}
		}
		else
		{
			fprintf(stderr, "backend id query failed: %s",
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

	res = PQprepare(conns[0], PREP_WAITING,
					"SELECT 1 WHERE pg_stat_get_backend_waiting($1)", 0, NULL);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "prepare of lock wait query failed: %s",
				PQerrorMessage(conns[0]));
		exit_nicely();
	}
	PQclear(res);

	/*
	 * Run the permutations specified in the spec, or all if none were
	 * explicitly specified.
	 */
	if (testspec->permutations)
		run_named_permutations(testspec);
	else
		run_all_permutations(testspec);

	/* Clean up and exit */
	for (i = 0; i < nconns; i++)
		PQfinish(conns[i]);
	return 0;
}

static int *piles;

/*
 * Run all permutations of the steps and sessions.
 */
static void
run_all_permutations(TestSpec * testspec)
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
	 * session on a pile. To generate a permuation, we pick steps from the
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
run_all_permutations_recurse(TestSpec * testspec, int nsteps, Step ** steps)
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
run_named_permutations(TestSpec * testspec)
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

		/* Find all the named steps from the lookup table */
		for (j = 0; j < p->nsteps; j++)
		{
			steps[j] = *((Step **) bsearch(p->stepnames[j], allsteps, nallsteps,
										 sizeof(Step *), &step_bsearch_cmp));
			if (steps[j] == NULL)
			{
				fprintf(stderr, "undefined step \"%s\" specified in permutation\n", p->stepnames[j]);
				exit_nicely();
			}
		}

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
 * Run one permutation
 */
static void
run_permutation(TestSpec * testspec, int nsteps, Step ** steps)
{
	PGresult   *res;
	int			i;
	Step	   *waiting = NULL;

	printf("\nstarting permutation:");
	for (i = 0; i < nsteps; i++)
		printf(" %s", steps[i]->name);
	printf("\n");

	/* Perform setup */
	if (testspec->setupsql)
	{
		res = PQexec(conns[0], testspec->setupsql);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
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
			if (PQresultStatus(res) != PGRES_COMMAND_OK)
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
		Step *step = steps[i];

		if (!PQsendQuery(conns[1 + step->session], step->sql))
		{
			fprintf(stdout, "failed to send query: %s\n",
					PQerrorMessage(conns[1 + step->session]));
			exit_nicely();
		}

		if (waiting != NULL)
		{
			/* Some other step is already waiting: just block. */
			try_complete_step(step, 0);

			/* See if this step unblocked the waiting step. */
			if (!try_complete_step(waiting, STEP_NONBLOCK | STEP_RETRY))
				waiting = NULL;
		}
		else if (try_complete_step(step, STEP_NONBLOCK))
			waiting = step;
	}

	/* Finish any waiting query. */
	if (waiting != NULL)
		try_complete_step(waiting, STEP_RETRY);

	/* Perform per-session teardown */
	for (i = 0; i < testspec->nsessions; i++)
	{
		if (testspec->sessions[i]->teardownsql)
		{
			res = PQexec(conns[i + 1], testspec->sessions[i]->teardownsql);
			if (PQresultStatus(res) != PGRES_COMMAND_OK)
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
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
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
 * have executed additional steps in the permutation.  This is not fully
 * robust -- a concurrent autovacuum could briefly take a lock with which we
 * conflict.  The risk may be low enough to discount.
 *
 * When calling this function on behalf of a given step for a second or later
 * time, pass the STEP_RETRY flag.  This only affects the messages printed.
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

	while (flags & STEP_NONBLOCK && PQisBusy(conn))
	{
		FD_SET(sock, &read_set);
		timeout.tv_sec = 0;
		timeout.tv_usec = 10000;	/* Check for lock waits every 10ms. */

		ret = select(sock + 1, &read_set, NULL, NULL, &timeout);
		if (ret < 0)	/* error in select() */
		{
			fprintf(stderr, "select failed: %s\n", strerror(errno));
			exit_nicely();
		}
		else if (ret == 0)	/* select() timeout: check for lock wait */
		{
			int			ntuples;

			res = PQexecPrepared(conns[0], PREP_WAITING, 1,
								 &backend_ids[step->session + 1],
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
			fprintf(stderr, "PQconsumeInput failed: %s", PQerrorMessage(conn));
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
				/* Detail may contain xid values, so just show primary. */
				printf("%s:  %s\n", PQresultErrorField(res, PG_DIAG_SEVERITY),
					   PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY));
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
