/*
 * src/test/isolation/isolationtester.c
 *
 * isolationtester.c
 *		Runs an isolation test specified by a spec file.
 */

#include "postgres_fe.h"

#include <sys/time.h>
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#include "datatype/timestamp.h"
#include "isolationtester.h"
#include "libpq-fe.h"
#include "pg_getopt.h"
#include "pqexpbuffer.h"

#define PREP_WAITING "isolationtester_waiting"

/*
 * conns[0] is the global setup, teardown, and watchdog connection.  Additional
 * connections represent spec-defined sessions.  We also track the backend
 * PID, in numeric and string formats, for each connection.
 */
static PGconn **conns = NULL;
static int *backend_pids = NULL;
static const char **backend_pid_strs = NULL;
static int	nconns = 0;

/* Maximum time to wait before giving up on a step (in usec) */
static int64 max_step_wait = 300 * USECS_PER_SEC;


static void run_testspec(TestSpec *testspec);
static void run_all_permutations(TestSpec *testspec);
static void run_all_permutations_recurse(TestSpec *testspec, int nsteps,
										 Step **steps);
static void run_named_permutations(TestSpec *testspec);
static void run_permutation(TestSpec *testspec, int nsteps, Step **steps);

#define STEP_NONBLOCK	0x1		/* return 0 as soon as cmd waits for a lock */
#define STEP_RETRY		0x2		/* this is a retry of a previously-waiting cmd */
static bool try_complete_step(TestSpec *testspec, Step *step, int flags);

static int	step_qsort_cmp(const void *a, const void *b);
static int	step_bsearch_cmp(const void *a, const void *b);

static void printResultSet(PGresult *res);
static void isotesterNoticeProcessor(void *arg, const char *message);
static void blackholeNoticeProcessor(void *arg, const char *message);

static void
disconnect_atexit(void)
{
	int			i;

	for (i = 0; i < nconns; i++)
		if (conns[i])
			PQfinish(conns[i]);
}

int
main(int argc, char **argv)
{
	const char *conninfo;
	const char *env_wait;
	TestSpec   *testspec;
	int			i,
				j;
	int			n;
	PGresult   *res;
	PQExpBufferData wait_query;
	int			opt;
	int			nallsteps;
	Step	  **allsteps;

	while ((opt = getopt(argc, argv, "V")) != -1)
	{
		switch (opt)
		{
			case 'V':
				puts("isolationtester (PostgreSQL) " PG_VERSION);
				exit(0);
			default:
				fprintf(stderr, "Usage: isolationtester [CONNINFO]\n");
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

	/*
	 * If PGISOLATIONTIMEOUT is set in the environment, adopt its value (given
	 * in seconds) as the max time to wait for any one step to complete.
	 */
	env_wait = getenv("PGISOLATIONTIMEOUT");
	if (env_wait != NULL)
		max_step_wait = ((int64) atoi(env_wait)) * USECS_PER_SEC;

	/* Read the test spec from stdin */
	spec_yyparse();
	testspec = &parseresult;

	/* Create a lookup table of all steps. */
	nallsteps = 0;
	for (i = 0; i < testspec->nsessions; i++)
		nallsteps += testspec->sessions[i]->nsteps;

	allsteps = pg_malloc(nallsteps * sizeof(Step *));

	n = 0;
	for (i = 0; i < testspec->nsessions; i++)
	{
		for (j = 0; j < testspec->sessions[i]->nsteps; j++)
			allsteps[n++] = testspec->sessions[i]->steps[j];
	}

	qsort(allsteps, nallsteps, sizeof(Step *), &step_qsort_cmp);
	testspec->nallsteps = nallsteps;
	testspec->allsteps = allsteps;

	/* Verify that all step names are unique */
	for (i = 1; i < testspec->nallsteps; i++)
	{
		if (strcmp(testspec->allsteps[i - 1]->name,
				   testspec->allsteps[i]->name) == 0)
		{
			fprintf(stderr, "duplicate step name: %s\n",
					testspec->allsteps[i]->name);
			exit(1);
		}
	}

	printf("Parsed test spec with %d sessions\n", testspec->nsessions);

	/*
	 * Establish connections to the database, one for each session and an
	 * extra for lock wait detection and global work.
	 */
	nconns = 1 + testspec->nsessions;
	conns = (PGconn **) pg_malloc0(nconns * sizeof(PGconn *));
	backend_pids = pg_malloc0(nconns * sizeof(*backend_pids));
	backend_pid_strs = pg_malloc0(nconns * sizeof(*backend_pid_strs));
	atexit(disconnect_atexit);

	for (i = 0; i < nconns; i++)
	{
		conns[i] = PQconnectdb(conninfo);
		if (PQstatus(conns[i]) != CONNECTION_OK)
		{
			fprintf(stderr, "Connection %d to database failed: %s",
					i, PQerrorMessage(conns[i]));
			exit(1);
		}

		/*
		 * Set up notice processors for the user-defined connections, so that
		 * messages can get printed prefixed with the session names.  The
		 * control connection gets a "blackhole" processor instead (hides all
		 * messages).
		 */
		if (i != 0)
			PQsetNoticeProcessor(conns[i],
								 isotesterNoticeProcessor,
								 (void *) (testspec->sessions[i - 1]->name));
		else
			PQsetNoticeProcessor(conns[i],
								 blackholeNoticeProcessor,
								 NULL);

		/* Save each connection's backend PID for subsequent use. */
		backend_pids[i] = PQbackendPID(conns[i]);
		backend_pid_strs[i] = psprintf("%d", backend_pids[i]);
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
						 "SELECT pg_catalog.pg_isolation_test_session_is_blocked($1, '{");
	/* The spec syntax requires at least one session; assume that here. */
	appendPQExpBufferStr(&wait_query, backend_pid_strs[1]);
	for (i = 2; i < nconns; i++)
		appendPQExpBuffer(&wait_query, ",%s", backend_pid_strs[i]);
	appendPQExpBufferStr(&wait_query, "}')");

	res = PQprepare(conns[0], PREP_WAITING, wait_query.data, 0, NULL);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "prepare of lock wait query failed: %s",
				PQerrorMessage(conns[0]));
		exit(1);
	}
	PQclear(res);
	termPQExpBuffer(&wait_query);

	/*
	 * Run the permutations specified in the spec, or all if none were
	 * explicitly specified.
	 */
	run_testspec(testspec);

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
	int			i;

	if (testspec->permutations)
		run_named_permutations(testspec);
	else
		run_all_permutations(testspec);

	/*
	 * Verify that all steps have been used, complaining about anything
	 * defined but not used.
	 */
	for (i = 0; i < testspec->nallsteps; i++)
	{
		if (!testspec->allsteps[i]->used)
			fprintf(stderr, "unused step name: %s\n",
					testspec->allsteps[i]->name);
	}
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

	steps = pg_malloc(sizeof(Step *) * nsteps);

	/*
	 * To generate the permutations, we conceptually put the steps of each
	 * session on a pile. To generate a permutation, we pick steps from the
	 * piles until all piles are empty. By picking steps from piles in
	 * different order, we get different permutations.
	 *
	 * A pile is actually just an integer which tells how many steps we've
	 * already picked from this pile.
	 */
	piles = pg_malloc(sizeof(int) * testspec->nsessions);
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

	for (i = 0; i < testspec->npermutations; i++)
	{
		Permutation *p = testspec->permutations[i];
		Step	  **steps;

		steps = pg_malloc(p->nsteps * sizeof(Step *));

		/* Find all the named steps using the lookup table */
		for (j = 0; j < p->nsteps; j++)
		{
			Step	  **this = (Step **) bsearch(p->stepnames[j],
												 testspec->allsteps,
												 testspec->nallsteps,
												 sizeof(Step *),
												 &step_bsearch_cmp);

			if (this == NULL)
			{
				fprintf(stderr, "undefined step \"%s\" specified in permutation\n",
						p->stepnames[j]);
				exit(1);
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
 * As above, but reports messages possibly emitted by multiple steps.  This is
 * useful when we have a blocked command awakened by another one; we want to
 * report all messages identically, for the case where we don't care which
 * one fails due to a timeout such as deadlock timeout.
 */
static void
report_multiple_error_messages(Step *step, int nextra, Step **extrastep)
{
	PQExpBufferData buffer;
	int			n;

	if (nextra == 0)
	{
		report_error_message(step);
		return;
	}

	initPQExpBuffer(&buffer);
	appendPQExpBufferStr(&buffer, step->name);

	for (n = 0; n < nextra; ++n)
		appendPQExpBuffer(&buffer, " %s", extrastep[n]->name);

	if (step->errormsg)
	{
		fprintf(stdout, "error in steps %s: %s\n", buffer.data,
				step->errormsg);
		free(step->errormsg);
		step->errormsg = NULL;
	}

	for (n = 0; n < nextra; ++n)
	{
		if (extrastep[n]->errormsg == NULL)
			continue;
		fprintf(stdout, "error in steps %s: %s\n",
				buffer.data, extrastep[n]->errormsg);
		free(extrastep[n]->errormsg);
		extrastep[n]->errormsg = NULL;
	}

	termPQExpBuffer(&buffer);
}

/*
 * Run one permutation
 */
static void
run_permutation(TestSpec *testspec, int nsteps, Step **steps)
{
	PGresult   *res;
	int			i;
	int			w;
	int			nwaiting = 0;
	int			nerrorstep = 0;
	Step	  **waiting;
	Step	  **errorstep;

	waiting = pg_malloc(sizeof(Step *) * testspec->nsessions);
	errorstep = pg_malloc(sizeof(Step *) * testspec->nsessions);

	printf("\nstarting permutation:");
	for (i = 0; i < nsteps; i++)
	{
		/* Track the permutation as in-use */
		steps[i]->used = true;
		printf(" %s", steps[i]->name);
	}
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
			exit(1);
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
				exit(1);
			}
			PQclear(res);
		}
	}

	/* Perform steps */
	for (i = 0; i < nsteps; i++)
	{
		Step	   *step = steps[i];
		PGconn	   *conn = conns[1 + step->session];
		Step	   *oldstep = NULL;
		bool		mustwait;

		/*
		 * Check whether the session that needs to perform the next step is
		 * still blocked on an earlier step.  If so, wait for it to finish.
		 *
		 * (In older versions of this tool, we allowed precisely one session
		 * to be waiting at a time.  If we reached a step that required that
		 * session to execute the next command, we would declare the whole
		 * permutation invalid, cancel everything, and move on to the next
		 * one.  Unfortunately, that made it impossible to test the deadlock
		 * detector using this framework, unless the number of processes
		 * involved in the deadlock was precisely two.  We now assume that if
		 * we reach a step that is still blocked, we need to wait for it to
		 * unblock itself.)
		 */
		for (w = 0; w < nwaiting; ++w)
		{
			if (step->session == waiting[w]->session)
			{
				oldstep = waiting[w];

				/* Wait for previous step on this connection. */
				try_complete_step(testspec, oldstep, STEP_RETRY);

				/* Remove that step from the waiting[] array. */
				if (w + 1 < nwaiting)
					memmove(&waiting[w], &waiting[w + 1],
							(nwaiting - (w + 1)) * sizeof(Step *));
				nwaiting--;

				break;
			}
		}
		if (oldstep != NULL)
		{
			/*
			 * Check for completion of any steps that were previously waiting.
			 * Remove any that have completed from waiting[], and include them
			 * in the list for report_multiple_error_messages().
			 */
			w = 0;
			nerrorstep = 0;
			while (w < nwaiting)
			{
				if (try_complete_step(testspec, waiting[w],
									  STEP_NONBLOCK | STEP_RETRY))
				{
					/* Still blocked on a lock, leave it alone. */
					w++;
				}
				else
				{
					/* This one finished, too! */
					errorstep[nerrorstep++] = waiting[w];
					if (w + 1 < nwaiting)
						memmove(&waiting[w], &waiting[w + 1],
								(nwaiting - (w + 1)) * sizeof(Step *));
					nwaiting--;
				}
			}

			/* Report all errors together. */
			report_multiple_error_messages(oldstep, nerrorstep, errorstep);
		}

		/* Send the query for this step. */
		if (!PQsendQuery(conn, step->sql))
		{
			fprintf(stdout, "failed to send query for step %s: %s\n",
					step->name, PQerrorMessage(conn));
			exit(1);
		}

		/* Try to complete this step without blocking.  */
		mustwait = try_complete_step(testspec, step, STEP_NONBLOCK);

		/* Check for completion of any steps that were previously waiting. */
		w = 0;
		nerrorstep = 0;
		while (w < nwaiting)
		{
			if (try_complete_step(testspec, waiting[w],
								  STEP_NONBLOCK | STEP_RETRY))
				w++;
			else
			{
				errorstep[nerrorstep++] = waiting[w];
				if (w + 1 < nwaiting)
					memmove(&waiting[w], &waiting[w + 1],
							(nwaiting - (w + 1)) * sizeof(Step *));
				nwaiting--;
			}
		}

		/* Report any error from this step, and any steps that it unblocked. */
		report_multiple_error_messages(step, nerrorstep, errorstep);

		/* If this step is waiting, add it to the array of waiters. */
		if (mustwait)
			waiting[nwaiting++] = step;
	}

	/* Wait for any remaining queries. */
	for (w = 0; w < nwaiting; ++w)
	{
		try_complete_step(testspec, waiting[w], STEP_RETRY);
		report_error_message(waiting[w]);
	}

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

	free(waiting);
	free(errorstep);
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
 * If the query returns an error, the message is saved in step->errormsg.
 * Caller should call report_error_message shortly after this, to have it
 * printed and cleared.
 *
 * If the STEP_NONBLOCK flag was specified and the query is waiting to acquire
 * a lock, returns true.  Otherwise, returns false.
 */
static bool
try_complete_step(TestSpec *testspec, Step *step, int flags)
{
	PGconn	   *conn = conns[1 + step->session];
	fd_set		read_set;
	struct timeval start_time;
	struct timeval timeout;
	int			sock = PQsocket(conn);
	int			ret;
	PGresult   *res;
	PGnotify   *notify;
	bool		canceled = false;

	if (sock < 0)
	{
		fprintf(stderr, "invalid socket: %s", PQerrorMessage(conn));
		exit(1);
	}

	gettimeofday(&start_time, NULL);
	FD_ZERO(&read_set);

	while (PQisBusy(conn))
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
			exit(1);
		}
		else if (ret == 0)		/* select() timeout: check for lock wait */
		{
			struct timeval current_time;
			int64		td;

			/* If it's OK for the step to block, check whether it has. */
			if (flags & STEP_NONBLOCK)
			{
				bool		waiting;

				res = PQexecPrepared(conns[0], PREP_WAITING, 1,
									 &backend_pid_strs[step->session + 1],
									 NULL, NULL, 0);
				if (PQresultStatus(res) != PGRES_TUPLES_OK ||
					PQntuples(res) != 1)
				{
					fprintf(stderr, "lock wait query failed: %s",
							PQerrorMessage(conns[0]));
					exit(1);
				}
				waiting = ((PQgetvalue(res, 0, 0))[0] == 't');
				PQclear(res);

				if (waiting)	/* waiting to acquire a lock */
				{
					/*
					 * Since it takes time to perform the lock-check query,
					 * some data --- notably, NOTICE messages --- might have
					 * arrived since we looked.  We must call PQconsumeInput
					 * and then PQisBusy to collect and process any such
					 * messages.  In the (unlikely) case that PQisBusy then
					 * returns false, we might as well go examine the
					 * available result.
					 */
					if (!PQconsumeInput(conn))
					{
						fprintf(stderr, "PQconsumeInput failed: %s\n",
								PQerrorMessage(conn));
						exit(1);
					}
					if (!PQisBusy(conn))
						break;

					/*
					 * conn is still busy, so conclude that the step really is
					 * waiting.
					 */
					if (!(flags & STEP_RETRY))
						printf("step %s: %s <waiting ...>\n",
							   step->name, step->sql);
					return true;
				}
				/* else, not waiting */
			}

			/* Figure out how long we've been waiting for this step. */
			gettimeofday(&current_time, NULL);
			td = (int64) current_time.tv_sec - (int64) start_time.tv_sec;
			td *= USECS_PER_SEC;
			td += (int64) current_time.tv_usec - (int64) start_time.tv_usec;

			/*
			 * After max_step_wait microseconds, try to cancel the query.
			 *
			 * If the user tries to test an invalid permutation, we don't want
			 * to hang forever, especially when this is running in the
			 * buildfarm.  This will presumably lead to this permutation
			 * failing, but remaining permutations and tests should still be
			 * OK.
			 */
			if (td > max_step_wait && !canceled)
			{
				PGcancel   *cancel = PQgetCancel(conn);

				if (cancel != NULL)
				{
					char		buf[256];

					if (PQcancel(cancel, buf, sizeof(buf)))
					{
						/*
						 * print to stdout not stderr, as this should appear
						 * in the test case's results
						 */
						printf("isolationtester: canceling step %s after %d seconds\n",
							   step->name, (int) (td / USECS_PER_SEC));
						canceled = true;
					}
					else
						fprintf(stderr, "PQcancel failed: %s\n", buf);
					PQfreeCancel(cancel);
				}
			}

			/*
			 * After twice max_step_wait, just give up and die.
			 *
			 * Since cleanup steps won't be run in this case, this may cause
			 * later tests to fail.  That stinks, but it's better than waiting
			 * forever for the server to respond to the cancel.
			 */
			if (td > 2 * max_step_wait)
			{
				fprintf(stderr, "step %s timed out after %d seconds\n",
						step->name, (int) (td / USECS_PER_SEC));
				exit(1);
			}
		}
		else if (!PQconsumeInput(conn)) /* select(): data available */
		{
			fprintf(stderr, "PQconsumeInput failed: %s\n",
					PQerrorMessage(conn));
			exit(1);
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

	/* Report any available NOTIFY messages, too */
	PQconsumeInput(conn);
	while ((notify = PQnotifies(conn)) != NULL)
	{
		/* Try to identify which session it came from */
		const char *sendername = NULL;
		char		pidstring[32];

		for (int i = 0; i < testspec->nsessions; i++)
		{
			if (notify->be_pid == backend_pids[i + 1])
			{
				sendername = testspec->sessions[i]->name;
				break;
			}
		}
		if (sendername == NULL)
		{
			/* Doesn't seem to be any test session, so show the hard way */
			snprintf(pidstring, sizeof(pidstring), "PID %d", notify->be_pid);
			sendername = pidstring;
		}
		printf("%s: NOTIFY \"%s\" with payload \"%s\" from %s\n",
			   testspec->sessions[step->session]->name,
			   notify->relname, notify->extra, sendername);
		PQfreemem(notify);
		PQconsumeInput(conn);
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

/* notice processor, prefixes each message with the session name */
static void
isotesterNoticeProcessor(void *arg, const char *message)
{
	printf("%s: %s", (char *) arg, message);
}

/* notice processor, hides the message */
static void
blackholeNoticeProcessor(void *arg, const char *message)
{
	/* do nothing */
}
