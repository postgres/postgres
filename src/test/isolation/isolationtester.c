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
 * connections represent spec-defined sessions.
 */
typedef struct IsoConnInfo
{
	/* The libpq connection object for this connection. */
	PGconn	   *conn;
	/* The backend PID, in numeric and string formats. */
	int			backend_pid;
	const char *backend_pid_str;
	/* Name of the associated session. */
	const char *sessionname;
	/* Active step on this connection, or NULL if idle. */
	PermutationStep *active_step;
	/* Number of NOTICE messages received from connection. */
	int			total_notices;
} IsoConnInfo;

static IsoConnInfo *conns = NULL;
static int	nconns = 0;

/* Flag indicating some new NOTICE has arrived */
static bool any_new_notice = false;

/* Maximum time to wait before giving up on a step (in usec) */
static int64 max_step_wait = 300 * USECS_PER_SEC;


static void check_testspec(TestSpec *testspec);
static void run_testspec(TestSpec *testspec);
static void run_all_permutations(TestSpec *testspec);
static void run_all_permutations_recurse(TestSpec *testspec, int *piles,
										 int nsteps, PermutationStep **steps);
static void run_named_permutations(TestSpec *testspec);
static void run_permutation(TestSpec *testspec, int nsteps,
							PermutationStep **steps);

/* Flag bits for try_complete_step(s) */
#define STEP_NONBLOCK	0x1		/* return as soon as cmd waits for a lock */
#define STEP_RETRY		0x2		/* this is a retry of a previously-waiting cmd */

static int	try_complete_steps(TestSpec *testspec, PermutationStep **waiting,
							   int nwaiting, int flags);
static bool try_complete_step(TestSpec *testspec, PermutationStep *pstep,
							  int flags);

static int	step_qsort_cmp(const void *a, const void *b);
static int	step_bsearch_cmp(const void *a, const void *b);

static bool step_has_blocker(PermutationStep *pstep);
static void printResultSet(PGresult *res);
static void isotesterNoticeProcessor(void *arg, const char *message);
static void blackholeNoticeProcessor(void *arg, const char *message);

static void
disconnect_atexit(void)
{
	int			i;

	for (i = 0; i < nconns; i++)
		if (conns[i].conn)
			PQfinish(conns[i].conn);
}

int
main(int argc, char **argv)
{
	const char *conninfo;
	const char *env_wait;
	TestSpec   *testspec;
	PGresult   *res;
	PQExpBufferData wait_query;
	int			opt;
	int			i;

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

	/* Perform post-parse checking, and fill in linking fields */
	check_testspec(testspec);

	printf("Parsed test spec with %d sessions\n", testspec->nsessions);

	/*
	 * Establish connections to the database, one for each session and an
	 * extra for lock wait detection and global work.
	 */
	nconns = 1 + testspec->nsessions;
	conns = (IsoConnInfo *) pg_malloc0(nconns * sizeof(IsoConnInfo));
	atexit(disconnect_atexit);

	for (i = 0; i < nconns; i++)
	{
		const char *sessionname;

		if (i == 0)
			sessionname = "control connection";
		else
			sessionname = testspec->sessions[i - 1]->name;

		conns[i].sessionname = sessionname;

		conns[i].conn = PQconnectdb(conninfo);
		if (PQstatus(conns[i].conn) != CONNECTION_OK)
		{
			fprintf(stderr, "Connection %d failed: %s",
					i, PQerrorMessage(conns[i].conn));
			exit(1);
		}

		/*
		 * Set up notice processors for the user-defined connections, so that
		 * messages can get printed prefixed with the session names.  The
		 * control connection gets a "blackhole" processor instead (hides all
		 * messages).
		 */
		if (i != 0)
			PQsetNoticeProcessor(conns[i].conn,
								 isotesterNoticeProcessor,
								 (void *) &conns[i]);
		else
			PQsetNoticeProcessor(conns[i].conn,
								 blackholeNoticeProcessor,
								 NULL);

		/*
		 * Similarly, append the session name to application_name to make it
		 * easier to map spec file sessions to log output and
		 * pg_stat_activity. The reason to append instead of just setting the
		 * name is that we don't know the name of the test currently running.
		 */
		res = PQexecParams(conns[i].conn,
						   "SELECT set_config('application_name',\n"
						   "  current_setting('application_name') || '/' || $1,\n"
						   "  false)",
						   1, NULL,
						   &sessionname,
						   NULL, NULL, 0);
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			fprintf(stderr, "setting of application name failed: %s",
					PQerrorMessage(conns[i].conn));
			exit(1);
		}

		/* Save each connection's backend PID for subsequent use. */
		conns[i].backend_pid = PQbackendPID(conns[i].conn);
		conns[i].backend_pid_str = psprintf("%d", conns[i].backend_pid);
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
	appendPQExpBufferStr(&wait_query, conns[1].backend_pid_str);
	for (i = 2; i < nconns; i++)
		appendPQExpBuffer(&wait_query, ",%s", conns[i].backend_pid_str);
	appendPQExpBufferStr(&wait_query, "}')");

	res = PQprepare(conns[0].conn, PREP_WAITING, wait_query.data, 0, NULL);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "prepare of lock wait query failed: %s",
				PQerrorMessage(conns[0].conn));
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

/*
 * Validity-check the test spec and fill in cross-links between nodes.
 */
static void
check_testspec(TestSpec *testspec)
{
	int			nallsteps;
	Step	  **allsteps;
	int			i,
				j,
				k;

	/* Create a sorted lookup table of all steps. */
	nallsteps = 0;
	for (i = 0; i < testspec->nsessions; i++)
		nallsteps += testspec->sessions[i]->nsteps;

	allsteps = pg_malloc(nallsteps * sizeof(Step *));

	k = 0;
	for (i = 0; i < testspec->nsessions; i++)
	{
		for (j = 0; j < testspec->sessions[i]->nsteps; j++)
			allsteps[k++] = testspec->sessions[i]->steps[j];
	}

	qsort(allsteps, nallsteps, sizeof(Step *), step_qsort_cmp);

	/* Verify that all step names are unique. */
	for (i = 1; i < nallsteps; i++)
	{
		if (strcmp(allsteps[i - 1]->name,
				   allsteps[i]->name) == 0)
		{
			fprintf(stderr, "duplicate step name: %s\n",
					allsteps[i]->name);
			exit(1);
		}
	}

	/* Set the session index fields in steps. */
	for (i = 0; i < testspec->nsessions; i++)
	{
		Session    *session = testspec->sessions[i];

		for (j = 0; j < session->nsteps; j++)
			session->steps[j]->session = i;
	}

	/*
	 * If we have manually-specified permutations, link PermutationSteps to
	 * Steps, and fill in blocker links.
	 */
	for (i = 0; i < testspec->npermutations; i++)
	{
		Permutation *p = testspec->permutations[i];

		for (j = 0; j < p->nsteps; j++)
		{
			PermutationStep *pstep = p->steps[j];
			Step	  **this = (Step **) bsearch(pstep->name,
												 allsteps,
												 nallsteps,
												 sizeof(Step *),
												 step_bsearch_cmp);

			if (this == NULL)
			{
				fprintf(stderr, "undefined step \"%s\" specified in permutation\n",
						pstep->name);
				exit(1);
			}
			pstep->step = *this;

			/* Mark the step used, for check below */
			pstep->step->used = true;
		}

		/*
		 * Identify any blocker steps.  We search only the current
		 * permutation, since steps not used there couldn't be concurrent.
		 * Note that it's OK to reference later permutation steps, so this
		 * can't be combined with the previous loop.
		 */
		for (j = 0; j < p->nsteps; j++)
		{
			PermutationStep *pstep = p->steps[j];

			for (k = 0; k < pstep->nblockers; k++)
			{
				PermutationStepBlocker *blocker = pstep->blockers[k];
				int			n;

				if (blocker->blocktype == PSB_ONCE)
					continue;	/* nothing to link to */

				blocker->step = NULL;
				for (n = 0; n < p->nsteps; n++)
				{
					PermutationStep *otherp = p->steps[n];

					if (strcmp(otherp->name, blocker->stepname) == 0)
					{
						blocker->step = otherp->step;
						break;
					}
				}
				if (blocker->step == NULL)
				{
					fprintf(stderr, "undefined blocking step \"%s\" referenced in permutation step \"%s\"\n",
							blocker->stepname, pstep->name);
					exit(1);
				}
				/* can't block on completion of step of own session */
				if (blocker->step->session == pstep->step->session)
				{
					fprintf(stderr, "permutation step \"%s\" cannot block on its own session\n",
							pstep->name);
					exit(1);
				}
			}
		}
	}

	/*
	 * If we have manually-specified permutations, verify that all steps have
	 * been used, warning about anything defined but not used.  We can skip
	 * this when using automatically-generated permutations.
	 */
	if (testspec->permutations)
	{
		for (i = 0; i < nallsteps; i++)
		{
			if (!allsteps[i]->used)
				fprintf(stderr, "unused step name: %s\n", allsteps[i]->name);
		}
	}

	free(allsteps);
}

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
	PermutationStep *steps;
	PermutationStep **stepptrs;
	int		   *piles;

	/* Count the total number of steps in all sessions */
	nsteps = 0;
	for (i = 0; i < testspec->nsessions; i++)
		nsteps += testspec->sessions[i]->nsteps;

	/* Create PermutationStep workspace array */
	steps = (PermutationStep *) pg_malloc0(sizeof(PermutationStep) * nsteps);
	stepptrs = (PermutationStep **) pg_malloc(sizeof(PermutationStep *) * nsteps);
	for (i = 0; i < nsteps; i++)
		stepptrs[i] = steps + i;

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

	run_all_permutations_recurse(testspec, piles, 0, stepptrs);

	free(steps);
	free(stepptrs);
	free(piles);
}

static void
run_all_permutations_recurse(TestSpec *testspec, int *piles,
							 int nsteps, PermutationStep **steps)
{
	int			i;
	bool		found = false;

	for (i = 0; i < testspec->nsessions; i++)
	{
		/* If there's any more steps in this pile, pick it and recurse */
		if (piles[i] < testspec->sessions[i]->nsteps)
		{
			Step	   *newstep = testspec->sessions[i]->steps[piles[i]];

			/*
			 * These automatically-generated PermutationSteps never have
			 * blocker conditions.  So we need only fill these fields, relying
			 * on run_all_permutations() to have zeroed the rest:
			 */
			steps[nsteps]->name = newstep->name;
			steps[nsteps]->step = newstep;

			piles[i]++;

			run_all_permutations_recurse(testspec, piles, nsteps + 1, steps);

			piles[i]--;

			found = true;
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
	int			i;

	for (i = 0; i < testspec->npermutations; i++)
	{
		Permutation *p = testspec->permutations[i];

		run_permutation(testspec, p->nsteps, p->steps);
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
run_permutation(TestSpec *testspec, int nsteps, PermutationStep **steps)
{
	PGresult   *res;
	int			i;
	int			nwaiting = 0;
	PermutationStep **waiting;

	waiting = pg_malloc(sizeof(PermutationStep *) * testspec->nsessions);

	printf("\nstarting permutation:");
	for (i = 0; i < nsteps; i++)
		printf(" %s", steps[i]->name);
	printf("\n");

	/* Perform setup */
	for (i = 0; i < testspec->nsetupsqls; i++)
	{
		res = PQexec(conns[0].conn, testspec->setupsqls[i]);
		if (PQresultStatus(res) == PGRES_TUPLES_OK)
		{
			printResultSet(res);
		}
		else if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			fprintf(stderr, "setup failed: %s", PQerrorMessage(conns[0].conn));
			exit(1);
		}
		PQclear(res);
	}

	/* Perform per-session setup */
	for (i = 0; i < testspec->nsessions; i++)
	{
		if (testspec->sessions[i]->setupsql)
		{
			res = PQexec(conns[i + 1].conn, testspec->sessions[i]->setupsql);
			if (PQresultStatus(res) == PGRES_TUPLES_OK)
			{
				printResultSet(res);
			}
			else if (PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				fprintf(stderr, "setup of session %s failed: %s",
						conns[i + 1].sessionname,
						PQerrorMessage(conns[i + 1].conn));
				exit(1);
			}
			PQclear(res);
		}
	}

	/* Perform steps */
	for (i = 0; i < nsteps; i++)
	{
		PermutationStep *pstep = steps[i];
		Step	   *step = pstep->step;
		IsoConnInfo *iconn = &conns[1 + step->session];
		PGconn	   *conn = iconn->conn;
		bool		mustwait;
		int			j;

		/*
		 * Check whether the session that needs to perform the next step is
		 * still blocked on an earlier step.  If so, wait for it to finish.
		 */
		if (iconn->active_step != NULL)
		{
			struct timeval start_time;

			gettimeofday(&start_time, NULL);

			while (iconn->active_step != NULL)
			{
				PermutationStep *oldstep = iconn->active_step;

				/*
				 * Wait for oldstep.  But even though we don't use
				 * STEP_NONBLOCK, it might not complete because of blocker
				 * conditions.
				 */
				if (!try_complete_step(testspec, oldstep, STEP_RETRY))
				{
					/* Done, so remove oldstep from the waiting[] array. */
					int			w;

					for (w = 0; w < nwaiting; w++)
					{
						if (oldstep == waiting[w])
							break;
					}
					if (w >= nwaiting)
						abort();	/* can't happen */
					if (w + 1 < nwaiting)
						memmove(&waiting[w], &waiting[w + 1],
								(nwaiting - (w + 1)) * sizeof(PermutationStep *));
					nwaiting--;
				}

				/*
				 * Check for other steps that have finished.  We should do
				 * this if oldstep completed, as it might have unblocked
				 * something.  On the other hand, if oldstep hasn't completed,
				 * we must poll all the active steps in hopes of unblocking
				 * oldstep.  So either way, poll them.
				 */
				nwaiting = try_complete_steps(testspec, waiting, nwaiting,
											  STEP_NONBLOCK | STEP_RETRY);

				/*
				 * If the target session is still busy, apply a timeout to
				 * keep from hanging indefinitely, which could happen with
				 * incorrect blocker annotations.  Use the same 2 *
				 * max_step_wait limit as try_complete_step does for deciding
				 * to die.  (We don't bother with trying to cancel anything,
				 * since it's unclear what to cancel in this case.)
				 */
				if (iconn->active_step != NULL)
				{
					struct timeval current_time;
					int64		td;

					gettimeofday(&current_time, NULL);
					td = (int64) current_time.tv_sec - (int64) start_time.tv_sec;
					td *= USECS_PER_SEC;
					td += (int64) current_time.tv_usec - (int64) start_time.tv_usec;
					if (td > 2 * max_step_wait)
					{
						fprintf(stderr, "step %s timed out after %d seconds\n",
								iconn->active_step->name,
								(int) (td / USECS_PER_SEC));
						fprintf(stderr, "active steps are:");
						for (j = 1; j < nconns; j++)
						{
							IsoConnInfo *oconn = &conns[j];

							if (oconn->active_step != NULL)
								fprintf(stderr, " %s",
										oconn->active_step->name);
						}
						fprintf(stderr, "\n");
						exit(1);
					}
				}
			}
		}

		/* Send the query for this step. */
		if (!PQsendQuery(conn, step->sql))
		{
			fprintf(stdout, "failed to send query for step %s: %s\n",
					step->name, PQerrorMessage(conn));
			exit(1);
		}

		/* Remember we launched a step. */
		iconn->active_step = pstep;

		/* Remember target number of NOTICEs for any blocker conditions. */
		for (j = 0; j < pstep->nblockers; j++)
		{
			PermutationStepBlocker *blocker = pstep->blockers[j];

			if (blocker->blocktype == PSB_NUM_NOTICES)
				blocker->target_notices = blocker->num_notices +
					conns[blocker->step->session + 1].total_notices;
		}

		/* Try to complete this step without blocking.  */
		mustwait = try_complete_step(testspec, pstep, STEP_NONBLOCK);

		/* Check for completion of any steps that were previously waiting. */
		nwaiting = try_complete_steps(testspec, waiting, nwaiting,
									  STEP_NONBLOCK | STEP_RETRY);

		/* If this step is waiting, add it to the array of waiters. */
		if (mustwait)
			waiting[nwaiting++] = pstep;
	}

	/* Wait for any remaining queries. */
	nwaiting = try_complete_steps(testspec, waiting, nwaiting, STEP_RETRY);
	if (nwaiting != 0)
	{
		fprintf(stderr, "failed to complete permutation due to mutually-blocking steps\n");
		exit(1);
	}

	/* Perform per-session teardown */
	for (i = 0; i < testspec->nsessions; i++)
	{
		if (testspec->sessions[i]->teardownsql)
		{
			res = PQexec(conns[i + 1].conn, testspec->sessions[i]->teardownsql);
			if (PQresultStatus(res) == PGRES_TUPLES_OK)
			{
				printResultSet(res);
			}
			else if (PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				fprintf(stderr, "teardown of session %s failed: %s",
						conns[i + 1].sessionname,
						PQerrorMessage(conns[i + 1].conn));
				/* don't exit on teardown failure */
			}
			PQclear(res);
		}
	}

	/* Perform teardown */
	if (testspec->teardownsql)
	{
		res = PQexec(conns[0].conn, testspec->teardownsql);
		if (PQresultStatus(res) == PGRES_TUPLES_OK)
		{
			printResultSet(res);
		}
		else if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			fprintf(stderr, "teardown failed: %s",
					PQerrorMessage(conns[0].conn));
			/* don't exit on teardown failure */
		}
		PQclear(res);
	}

	free(waiting);
}

/*
 * Check for completion of any waiting step(s).
 * Remove completed ones from the waiting[] array,
 * and return the new value of nwaiting.
 * See try_complete_step for the meaning of the flags.
 */
static int
try_complete_steps(TestSpec *testspec, PermutationStep **waiting,
				   int nwaiting, int flags)
{
	int			old_nwaiting;
	bool		have_blocker;

	do
	{
		int			w = 0;

		/* Reset latch; we only care about notices received within loop. */
		any_new_notice = false;

		/* Likewise, these variables reset for each retry. */
		old_nwaiting = nwaiting;
		have_blocker = false;

		/* Scan the array, try to complete steps. */
		while (w < nwaiting)
		{
			if (try_complete_step(testspec, waiting[w], flags))
			{
				/* Still blocked, leave it alone. */
				if (waiting[w]->nblockers > 0)
					have_blocker = true;
				w++;
			}
			else
			{
				/* Done, remove it from array. */
				if (w + 1 < nwaiting)
					memmove(&waiting[w], &waiting[w + 1],
							(nwaiting - (w + 1)) * sizeof(PermutationStep *));
				nwaiting--;
			}
		}

		/*
		 * If any of the still-waiting steps have blocker conditions attached,
		 * it's possible that one of the steps we examined afterwards has
		 * released them (either by completing, or by sending a NOTICE).  If
		 * any step completions or NOTICEs happened, repeat the loop until
		 * none occurs.  Without this provision, completion timing could vary
		 * depending on the order in which the steps appear in the array.
		 */
	} while (have_blocker && (nwaiting < old_nwaiting || any_new_notice));
	return nwaiting;
}

/*
 * Our caller already sent the query associated with this step.  Wait for it
 * to either complete, or hit a blocking condition.
 *
 * When calling this function on behalf of a given step for a second or later
 * time, pass the STEP_RETRY flag.  Do not pass it on the first call.
 *
 * Returns true if the step was *not* completed, false if it was completed.
 * Reasons for non-completion are (a) the STEP_NONBLOCK flag was specified
 * and the query is waiting to acquire a lock, or (b) the step has an
 * unsatisfied blocker condition.  When STEP_NONBLOCK is given, we assume
 * that any lock wait will persist until we have executed additional steps.
 */
static bool
try_complete_step(TestSpec *testspec, PermutationStep *pstep, int flags)
{
	Step	   *step = pstep->step;
	IsoConnInfo *iconn = &conns[1 + step->session];
	PGconn	   *conn = iconn->conn;
	fd_set		read_set;
	struct timeval start_time;
	struct timeval timeout;
	int			sock = PQsocket(conn);
	int			ret;
	PGresult   *res;
	PGnotify   *notify;
	bool		canceled = false;

	/*
	 * If the step is annotated with (*), then on the first call, force it to
	 * wait.  This is useful for ensuring consistent output when the step
	 * might or might not complete so fast that we don't observe it waiting.
	 */
	if (!(flags & STEP_RETRY))
	{
		int			i;

		for (i = 0; i < pstep->nblockers; i++)
		{
			PermutationStepBlocker *blocker = pstep->blockers[i];

			if (blocker->blocktype == PSB_ONCE)
			{
				printf("step %s: %s <waiting ...>\n",
					   step->name, step->sql);
				return true;
			}
		}
	}

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

				res = PQexecPrepared(conns[0].conn, PREP_WAITING, 1,
									 &conns[step->session + 1].backend_pid_str,
									 NULL, NULL, 0);
				if (PQresultStatus(res) != PGRES_TUPLES_OK ||
					PQntuples(res) != 1)
				{
					fprintf(stderr, "lock wait query failed: %s",
							PQerrorMessage(conns[0].conn));
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

	/*
	 * The step is done, but we won't report it as complete so long as there
	 * are blockers.
	 */
	if (step_has_blocker(pstep))
	{
		if (!(flags & STEP_RETRY))
			printf("step %s: %s <waiting ...>\n",
				   step->name, step->sql);
		return true;
	}

	/* Otherwise, go ahead and complete it. */
	if (flags & STEP_RETRY)
		printf("step %s: <... completed>\n", step->name);
	else
		printf("step %s: %s\n", step->name, step->sql);

	while ((res = PQgetResult(conn)))
	{
		switch (PQresultStatus(res))
		{
			case PGRES_COMMAND_OK:
			case PGRES_EMPTY_QUERY:
				break;
			case PGRES_TUPLES_OK:
				printResultSet(res);
				break;
			case PGRES_FATAL_ERROR:

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
						printf("%s:  %s\n", sev, msg);
					else
						printf("%s\n", PQresultErrorMessage(res));
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
		int			i;

		for (i = 0; i < testspec->nsessions; i++)
		{
			if (notify->be_pid == conns[i + 1].backend_pid)
			{
				sendername = conns[i + 1].sessionname;
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

	/* Connection is now idle. */
	iconn->active_step = NULL;

	return false;
}

/* Detect whether a step has any unsatisfied blocker conditions */
static bool
step_has_blocker(PermutationStep *pstep)
{
	int			i;

	for (i = 0; i < pstep->nblockers; i++)
	{
		PermutationStepBlocker *blocker = pstep->blockers[i];
		IsoConnInfo *iconn;

		switch (blocker->blocktype)
		{
			case PSB_ONCE:
				/* Ignore; try_complete_step handles this specially */
				break;
			case PSB_OTHER_STEP:
				/* Block if referenced step is active */
				iconn = &conns[1 + blocker->step->session];
				if (iconn->active_step &&
					iconn->active_step->step == blocker->step)
					return true;
				break;
			case PSB_NUM_NOTICES:
				/* Block if not enough notices received yet */
				iconn = &conns[1 + blocker->step->session];
				if (iconn->total_notices < blocker->target_notices)
					return true;
				break;
		}
	}
	return false;
}

static void
printResultSet(PGresult *res)
{
	PQprintOpt	popt;

	memset(&popt, 0, sizeof(popt));
	popt.header = true;
	popt.align = true;
	popt.fieldSep = "|";
	PQprint(stdout, res, &popt);
}

/* notice processor for regular user sessions */
static void
isotesterNoticeProcessor(void *arg, const char *message)
{
	IsoConnInfo *myconn = (IsoConnInfo *) arg;

	/* Prefix the backend's message with the session name. */
	printf("%s: %s", myconn->sessionname, message);
	/* Record notices, since we may need this to decide to unblock a step. */
	myconn->total_notices++;
	any_new_notice = true;
}

/* notice processor, hides the message */
static void
blackholeNoticeProcessor(void *arg, const char *message)
{
	/* do nothing */
}
