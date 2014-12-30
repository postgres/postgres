/*
 * pgbench.c
 *
 * A simple benchmark program for PostgreSQL
 * Originally written by Tatsuo Ishii and enhanced by many contributors.
 *
 * contrib/pgbench/pgbench.c
 * Copyright (c) 2000-2011, PostgreSQL Global Development Group
 * ALL RIGHTS RESERVED;
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose, without fee, and without a written agreement
 * is hereby granted, provided that the above copyright notice and this
 * paragraph and the following two paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL THE AUTHOR OR DISTRIBUTORS BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 * LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 * DOCUMENTATION, EVEN IF THE AUTHOR OR DISTRIBUTORS HAVE BEEN ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * THE AUTHOR AND DISTRIBUTORS SPECIFICALLY DISCLAIMS ANY WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE AUTHOR AND DISTRIBUTORS HAS NO OBLIGATIONS TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 */

#ifdef WIN32
#define FD_SETSIZE 1024			/* set before winsock2.h is included */
#endif   /* ! WIN32 */

#include "postgres_fe.h"

#include "libpq-fe.h"
#include "libpq/pqsignal.h"
#include "portability/instr_time.h"

#include <ctype.h>

#ifndef WIN32
#include <sys/time.h>
#include <unistd.h>
#endif   /* ! WIN32 */

#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>		/* for getrlimit */
#endif

#ifndef INT64_MAX
#define INT64_MAX	INT64CONST(0x7FFFFFFFFFFFFFFF)
#endif

/*
 * Multi-platform pthread implementations
 */

#ifdef WIN32
/* Use native win32 threads on Windows */
typedef struct win32_pthread *pthread_t;
typedef int pthread_attr_t;

static int	pthread_create(pthread_t *thread, pthread_attr_t * attr, void *(*start_routine) (void *), void *arg);
static int	pthread_join(pthread_t th, void **thread_return);
#elif defined(ENABLE_THREAD_SAFETY)
/* Use platform-dependent pthread capability */
#include <pthread.h>
#else
/* Use emulation with fork. Rename pthread identifiers to avoid conflicts */

#include <sys/wait.h>

#define pthread_t				pg_pthread_t
#define pthread_attr_t			pg_pthread_attr_t
#define pthread_create			pg_pthread_create
#define pthread_join			pg_pthread_join

typedef struct fork_pthread *pthread_t;
typedef int pthread_attr_t;

static int	pthread_create(pthread_t *thread, pthread_attr_t * attr, void *(*start_routine) (void *), void *arg);
static int	pthread_join(pthread_t th, void **thread_return);
#endif

extern char *optarg;
extern int	optind;


/********************************************************************
 * some configurable parameters */

/* max number of clients allowed */
#ifdef FD_SETSIZE
#define MAXCLIENTS	(FD_SETSIZE - 10)
#else
#define MAXCLIENTS	1024
#endif

#define DEFAULT_NXACTS	10		/* default nxacts */

int			nxacts = 0;			/* number of transactions per client */
int			duration = 0;		/* duration in seconds */

/*
 * scaling factor. for example, scale = 10 will make 1000000 tuples in
 * pgbench_accounts table.
 */
int			scale = 1;

/*
 * fillfactor. for example, fillfactor = 90 will use only 90 percent
 * space during inserts and leave 10 percent free.
 */
int			fillfactor = 100;

/*
 * end of configurable parameters
 *********************************************************************/

#define nbranches	1			/* Makes little sense to change this.  Change
								 * -s instead */
#define ntellers	10
#define naccounts	100000

bool		use_log;			/* log transaction latencies to a file */
bool		is_connect;			/* establish connection for each transaction */
bool		is_latencies;		/* report per-command latencies */
int			main_pid;			/* main process id used in log filename */

char	   *pghost = "";
char	   *pgport = "";
char	   *pgoptions = NULL;
char	   *pgtty = NULL;
char	   *login = NULL;
char	   *dbName;

volatile bool timer_exceeded = false;	/* flag from signal handler */

/* variable definitions */
typedef struct
{
	char	   *name;			/* variable name */
	char	   *value;			/* its value */
} Variable;

#define MAX_FILES		128		/* max number of SQL script files allowed */
#define SHELL_COMMAND_SIZE	256 /* maximum size allowed for shell command */

/*
 * structures used in custom query mode
 */

typedef struct
{
	PGconn	   *con;			/* connection handle to DB */
	int			id;				/* client No. */
	int			state;			/* state No. */
	int			cnt;			/* xacts count */
	int			ecnt;			/* error count */
	int			listen;			/* 0 indicates that an async query has been
								 * sent */
	int			sleeping;		/* 1 indicates that the client is napping */
	int64		until;			/* napping until (usec) */
	Variable   *variables;		/* array of variable definitions */
	int			nvariables;
	instr_time	txn_begin;		/* used for measuring transaction latencies */
	instr_time	stmt_begin;		/* used for measuring statement latencies */
	int			use_file;		/* index in sql_files for this client */
	bool		prepared[MAX_FILES];
} CState;

/*
 * Thread state and result
 */
typedef struct
{
	int			tid;			/* thread id */
	pthread_t	thread;			/* thread handle */
	CState	   *state;			/* array of CState */
	int			nstate;			/* length of state[] */
	instr_time	start_time;		/* thread start time */
	instr_time *exec_elapsed;	/* time spent executing cmds (per Command) */
	int		   *exec_count;		/* number of cmd executions (per Command) */
} TState;

#define INVALID_THREAD		((pthread_t) 0)

typedef struct
{
	instr_time	conn_time;
	int			xacts;
} TResult;

/*
 * queries read from files
 */
#define SQL_COMMAND		1
#define META_COMMAND	2
#define MAX_ARGS		10

typedef enum QueryMode
{
	QUERY_SIMPLE,				/* simple query */
	QUERY_EXTENDED,				/* extended query */
	QUERY_PREPARED,				/* extended query with prepared statements */
	NUM_QUERYMODE
} QueryMode;

static QueryMode querymode = QUERY_SIMPLE;
static const char *QUERYMODE[] = {"simple", "extended", "prepared"};

typedef struct
{
	char	   *line;			/* full text of command line */
	int			command_num;	/* unique index of this Command struct */
	int			type;			/* command type (SQL_COMMAND or META_COMMAND) */
	int			argc;			/* number of command words */
	char	   *argv[MAX_ARGS]; /* command word list */
} Command;

static Command **sql_files[MAX_FILES];	/* SQL script files */
static int	num_files;			/* number of script files */
static int	num_commands = 0;	/* total number of Command structs */
static int	debug = 0;			/* debug flag */

/* default scenario */
static char *tpc_b = {
	"\\set nbranches " CppAsString2(nbranches) " * :scale\n"
	"\\set ntellers " CppAsString2(ntellers) " * :scale\n"
	"\\set naccounts " CppAsString2(naccounts) " * :scale\n"
	"\\setrandom aid 1 :naccounts\n"
	"\\setrandom bid 1 :nbranches\n"
	"\\setrandom tid 1 :ntellers\n"
	"\\setrandom delta -5000 5000\n"
	"BEGIN;\n"
	"UPDATE pgbench_accounts SET abalance = abalance + :delta WHERE aid = :aid;\n"
	"SELECT abalance FROM pgbench_accounts WHERE aid = :aid;\n"
	"UPDATE pgbench_tellers SET tbalance = tbalance + :delta WHERE tid = :tid;\n"
	"UPDATE pgbench_branches SET bbalance = bbalance + :delta WHERE bid = :bid;\n"
	"INSERT INTO pgbench_history (tid, bid, aid, delta, mtime) VALUES (:tid, :bid, :aid, :delta, CURRENT_TIMESTAMP);\n"
	"END;\n"
};

/* -N case */
static char *simple_update = {
	"\\set nbranches " CppAsString2(nbranches) " * :scale\n"
	"\\set ntellers " CppAsString2(ntellers) " * :scale\n"
	"\\set naccounts " CppAsString2(naccounts) " * :scale\n"
	"\\setrandom aid 1 :naccounts\n"
	"\\setrandom bid 1 :nbranches\n"
	"\\setrandom tid 1 :ntellers\n"
	"\\setrandom delta -5000 5000\n"
	"BEGIN;\n"
	"UPDATE pgbench_accounts SET abalance = abalance + :delta WHERE aid = :aid;\n"
	"SELECT abalance FROM pgbench_accounts WHERE aid = :aid;\n"
	"INSERT INTO pgbench_history (tid, bid, aid, delta, mtime) VALUES (:tid, :bid, :aid, :delta, CURRENT_TIMESTAMP);\n"
	"END;\n"
};

/* -S case */
static char *select_only = {
	"\\set naccounts " CppAsString2(naccounts) " * :scale\n"
	"\\setrandom aid 1 :naccounts\n"
	"SELECT abalance FROM pgbench_accounts WHERE aid = :aid;\n"
};

/* Function prototypes */
static void setalarm(int seconds);
static void *threadRun(void *arg);


/*
 * routines to check mem allocations and fail noisily.
 */
static void *
xmalloc(size_t size)
{
	void	   *result;

	result = malloc(size);
	if (!result)
	{
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	return result;
}

static void *
xrealloc(void *ptr, size_t size)
{
	void	   *result;

	result = realloc(ptr, size);
	if (!result)
	{
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	return result;
}

static char *
xstrdup(const char *s)
{
	char	   *result;

	result = strdup(s);
	if (!result)
	{
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	return result;
}


static void
usage(const char *progname)
{
	printf("%s is a benchmarking tool for PostgreSQL.\n\n"
		   "Usage:\n"
		   "  %s [OPTIONS]... [DBNAME]\n"
		   "\nInitialization options:\n"
		   "  -i           invokes initialization mode\n"
		   "  -F NUM       fill factor\n"
		   "  -s NUM       scaling factor\n"
		   "\nBenchmarking options:\n"
		"  -c NUM       number of concurrent database clients (default: 1)\n"
		   "  -C           establish new connection for each transaction\n"
		   "  -D VARNAME=VALUE\n"
		   "               define variable for use by custom script\n"
		   "  -f FILENAME  read transaction script from FILENAME\n"
		   "  -j NUM       number of threads (default: 1)\n"
		   "  -l           write transaction times to log file\n"
		   "  -M {simple|extended|prepared}\n"
		   "               protocol for submitting queries to server (default: simple)\n"
		   "  -n           do not run VACUUM before tests\n"
		   "  -N           do not update tables \"pgbench_tellers\" and \"pgbench_branches\"\n"
		   "  -r           report average latency per command\n"
		   "  -s NUM       report this scale factor in output\n"
		   "  -S           perform SELECT-only transactions\n"
	 "  -t NUM       number of transactions each client runs (default: 10)\n"
		   "  -T NUM       duration of benchmark test in seconds\n"
		   "  -v           vacuum all four standard tables before tests\n"
		   "\nCommon options:\n"
		   "  -d           print debugging output\n"
		   "  -h HOSTNAME  database server host or socket directory\n"
		   "  -p PORT      database server port number\n"
		   "  -U USERNAME  connect as specified database user\n"
		   "  --help       show this help, then exit\n"
		   "  --version    output version information, then exit\n"
		   "\n"
		   "Report bugs to <pgsql-bugs@postgresql.org>.\n",
		   progname, progname);
}

/* random number generator: uniform distribution from min to max inclusive */
static int
getrand(int min, int max)
{
	/*
	 * Odd coding is so that min and max have approximately the same chance of
	 * being selected as do numbers between them.
	 */
	return min + (int) (((max - min + 1) * (double) random()) / (MAX_RANDOM_VALUE + 1.0));
}

/* call PQexec() and exit() on failure */
static void
executeStatement(PGconn *con, const char *sql)
{
	PGresult   *res;

	res = PQexec(con, sql);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "%s", PQerrorMessage(con));
		exit(1);
	}
	PQclear(res);
}

/* set up a connection to the backend */
static PGconn *
doConnect(void)
{
	PGconn	   *conn;
	static char *password = NULL;
	bool		new_pass;

	/*
	 * Start the connection.  Loop until we have a password if requested by
	 * backend.
	 */
	do
	{
		new_pass = false;

		conn = PQsetdbLogin(pghost, pgport, pgoptions, pgtty, dbName,
							login, password);
		if (!conn)
		{
			fprintf(stderr, "Connection to database \"%s\" failed\n",
					dbName);
			return NULL;
		}

		if (PQstatus(conn) == CONNECTION_BAD &&
			PQconnectionNeedsPassword(conn) &&
			password == NULL)
		{
			PQfinish(conn);
			password = simple_prompt("Password: ", 100, false);
			new_pass = true;
		}
	} while (new_pass);

	/* check to see that the backend connection was successfully made */
	if (PQstatus(conn) == CONNECTION_BAD)
	{
		fprintf(stderr, "Connection to database \"%s\" failed:\n%s",
				dbName, PQerrorMessage(conn));
		PQfinish(conn);
		return NULL;
	}

	return conn;
}

/* throw away response from backend */
static void
discard_response(CState *state)
{
	PGresult   *res;

	do
	{
		res = PQgetResult(state->con);
		if (res)
			PQclear(res);
	} while (res);
}

static int
compareVariables(const void *v1, const void *v2)
{
	return strcmp(((const Variable *) v1)->name,
				  ((const Variable *) v2)->name);
}

static char *
getVariable(CState *st, char *name)
{
	Variable	key,
			   *var;

	/* On some versions of Solaris, bsearch of zero items dumps core */
	if (st->nvariables <= 0)
		return NULL;

	key.name = name;
	var = (Variable *) bsearch((void *) &key,
							   (void *) st->variables,
							   st->nvariables,
							   sizeof(Variable),
							   compareVariables);
	if (var != NULL)
		return var->value;
	else
		return NULL;
}

/* check whether the name consists of alphabets, numerals and underscores. */
static bool
isLegalVariableName(const char *name)
{
	int			i;

	for (i = 0; name[i] != '\0'; i++)
	{
		if (!isalnum((unsigned char) name[i]) && name[i] != '_')
			return false;
	}

	return true;
}

static int
putVariable(CState *st, const char *context, char *name, char *value)
{
	Variable	key,
			   *var;

	key.name = name;
	/* On some versions of Solaris, bsearch of zero items dumps core */
	if (st->nvariables > 0)
		var = (Variable *) bsearch((void *) &key,
								   (void *) st->variables,
								   st->nvariables,
								   sizeof(Variable),
								   compareVariables);
	else
		var = NULL;

	if (var == NULL)
	{
		Variable   *newvars;

		/*
		 * Check for the name only when declaring a new variable to avoid
		 * overhead.
		 */
		if (!isLegalVariableName(name))
		{
			fprintf(stderr, "%s: invalid variable name '%s'\n", context, name);
			return false;
		}

		if (st->variables)
			newvars = (Variable *) xrealloc(st->variables,
									(st->nvariables + 1) * sizeof(Variable));
		else
			newvars = (Variable *) xmalloc(sizeof(Variable));

		st->variables = newvars;

		var = &newvars[st->nvariables];

		var->name = xstrdup(name);
		var->value = xstrdup(value);

		st->nvariables++;

		qsort((void *) st->variables, st->nvariables, sizeof(Variable),
			  compareVariables);
	}
	else
	{
		char	   *val;

		/* dup then free, in case value is pointing at this variable */
		val = xstrdup(value);

		free(var->value);
		var->value = val;
	}

	return true;
}

static char *
parseVariable(const char *sql, int *eaten)
{
	int			i = 0;
	char	   *name;

	do
	{
		i++;
	} while (isalnum((unsigned char) sql[i]) || sql[i] == '_');
	if (i == 1)
		return NULL;

	name = xmalloc(i);
	memcpy(name, &sql[1], i - 1);
	name[i - 1] = '\0';

	*eaten = i;
	return name;
}

static char *
replaceVariable(char **sql, char *param, int len, char *value)
{
	int			valueln = strlen(value);

	if (valueln > len)
	{
		size_t		offset = param - *sql;

		*sql = xrealloc(*sql, strlen(*sql) - len + valueln + 1);
		param = *sql + offset;
	}

	if (valueln != len)
		memmove(param + valueln, param + len, strlen(param + len) + 1);
	strncpy(param, value, valueln);

	return param + valueln;
}

static char *
assignVariables(CState *st, char *sql)
{
	char	   *p,
			   *name,
			   *val;

	p = sql;
	while ((p = strchr(p, ':')) != NULL)
	{
		int			eaten;

		name = parseVariable(p, &eaten);
		if (name == NULL)
		{
			while (*p == ':')
			{
				p++;
			}
			continue;
		}

		val = getVariable(st, name);
		free(name);
		if (val == NULL)
		{
			p++;
			continue;
		}

		p = replaceVariable(&sql, p, eaten, val);
	}

	return sql;
}

static void
getQueryParams(CState *st, const Command *command, const char **params)
{
	int			i;

	for (i = 0; i < command->argc - 1; i++)
		params[i] = getVariable(st, command->argv[i + 1]);
}

/*
 * Run a shell command. The result is assigned to the variable if not NULL.
 * Return true if succeeded, or false on error.
 */
static bool
runShellCommand(CState *st, char *variable, char **argv, int argc)
{
	char		command[SHELL_COMMAND_SIZE];
	int			i,
				len = 0;
	FILE	   *fp;
	char		res[64];
	char	   *endptr;
	int			retval;

	/*----------
	 * Join arguments with whitespace separators. Arguments starting with
	 * exactly one colon are treated as variables:
	 *	name - append a string "name"
	 *	:var - append a variable named 'var'
	 *	::name - append a string ":name"
	 *----------
	 */
	for (i = 0; i < argc; i++)
	{
		char	   *arg;
		int			arglen;

		if (argv[i][0] != ':')
		{
			arg = argv[i];		/* a string literal */
		}
		else if (argv[i][1] == ':')
		{
			arg = argv[i] + 1;	/* a string literal starting with colons */
		}
		else if ((arg = getVariable(st, argv[i] + 1)) == NULL)
		{
			fprintf(stderr, "%s: undefined variable %s\n", argv[0], argv[i]);
			return false;
		}

		arglen = strlen(arg);
		if (len + arglen + (i > 0 ? 1 : 0) >= SHELL_COMMAND_SIZE - 1)
		{
			fprintf(stderr, "%s: too long shell command\n", argv[0]);
			return false;
		}

		if (i > 0)
			command[len++] = ' ';
		memcpy(command + len, arg, arglen);
		len += arglen;
	}

	command[len] = '\0';

	/* Fast path for non-assignment case */
	if (variable == NULL)
	{
		if (system(command))
		{
			if (!timer_exceeded)
				fprintf(stderr, "%s: cannot launch shell command\n", argv[0]);
			return false;
		}
		return true;
	}

	/* Execute the command with pipe and read the standard output. */
	if ((fp = popen(command, "r")) == NULL)
	{
		fprintf(stderr, "%s: cannot launch shell command\n", argv[0]);
		return false;
	}
	if (fgets(res, sizeof(res), fp) == NULL)
	{
		if (!timer_exceeded)
			fprintf(stderr, "%s: cannot read the result\n", argv[0]);
		(void) pclose(fp);
		return false;
	}
	if (pclose(fp) < 0)
	{
		fprintf(stderr, "%s: cannot close shell command\n", argv[0]);
		return false;
	}

	/* Check whether the result is an integer and assign it to the variable */
	retval = (int) strtol(res, &endptr, 10);
	while (*endptr != '\0' && isspace((unsigned char) *endptr))
		endptr++;
	if (*res == '\0' || *endptr != '\0')
	{
		fprintf(stderr, "%s: must return an integer ('%s' returned)\n", argv[0], res);
		return false;
	}
	snprintf(res, sizeof(res), "%d", retval);
	if (!putVariable(st, "setshell", variable, res))
		return false;

#ifdef DEBUG
	printf("shell parameter name: %s, value: %s\n", argv[1], res);
#endif
	return true;
}

#define MAX_PREPARE_NAME		32
static void
preparedStatementName(char *buffer, int file, int state)
{
	sprintf(buffer, "P%d_%d", file, state);
}

static bool
clientDone(CState *st, bool ok)
{
	(void) ok;					/* unused */

	if (st->con != NULL)
	{
		PQfinish(st->con);
		st->con = NULL;
	}
	return false;				/* always false */
}

/* return false iff client should be disconnected */
static bool
doCustom(TState *thread, CState *st, instr_time *conn_time, FILE *logfile)
{
	PGresult   *res;
	Command   **commands;

top:
	commands = sql_files[st->use_file];

	if (st->sleeping)
	{							/* are we sleeping? */
		instr_time	now;

		INSTR_TIME_SET_CURRENT(now);
		if (st->until <= INSTR_TIME_GET_MICROSEC(now))
			st->sleeping = 0;	/* Done sleeping, go ahead with next command */
		else
			return true;		/* Still sleeping, nothing to do here */
	}

	if (st->listen)
	{							/* are we receiver? */
		if (commands[st->state]->type == SQL_COMMAND)
		{
			if (debug)
				fprintf(stderr, "client %d receiving\n", st->id);
			if (!PQconsumeInput(st->con))
			{					/* there's something wrong */
				fprintf(stderr, "Client %d aborted in state %d. Probably the backend died while processing.\n", st->id, st->state);
				return clientDone(st, false);
			}
			if (PQisBusy(st->con))
				return true;	/* don't have the whole result yet */
		}

		/*
		 * command finished: accumulate per-command execution times in
		 * thread-local data structure, if per-command latencies are requested
		 */
		if (is_latencies)
		{
			instr_time	now;
			int			cnum = commands[st->state]->command_num;

			INSTR_TIME_SET_CURRENT(now);
			INSTR_TIME_ACCUM_DIFF(thread->exec_elapsed[cnum],
								  now, st->stmt_begin);
			thread->exec_count[cnum]++;
		}

		/*
		 * if transaction finished, record the time it took in the log
		 */
		if (logfile && commands[st->state + 1] == NULL)
		{
			instr_time	now;
			instr_time	diff;
			double		usec;

			INSTR_TIME_SET_CURRENT(now);
			diff = now;
			INSTR_TIME_SUBTRACT(diff, st->txn_begin);
			usec = (double) INSTR_TIME_GET_MICROSEC(diff);

#ifndef WIN32
			/* This is more than we really ought to know about instr_time */
			fprintf(logfile, "%d %d %.0f %d %ld %ld\n",
					st->id, st->cnt, usec, st->use_file,
					(long) now.tv_sec, (long) now.tv_usec);
#else
			/* On Windows, instr_time doesn't provide a timestamp anyway */
			fprintf(logfile, "%d %d %.0f %d 0 0\n",
					st->id, st->cnt, usec, st->use_file);
#endif
		}

		if (commands[st->state]->type == SQL_COMMAND)
		{
			/*
			 * Read and discard the query result; note this is not included in
			 * the statement latency numbers.
			 */
			res = PQgetResult(st->con);
			switch (PQresultStatus(res))
			{
				case PGRES_COMMAND_OK:
				case PGRES_TUPLES_OK:
					break;		/* OK */
				default:
					fprintf(stderr, "Client %d aborted in state %d: %s",
							st->id, st->state, PQerrorMessage(st->con));
					PQclear(res);
					return clientDone(st, false);
			}
			PQclear(res);
			discard_response(st);
		}

		if (commands[st->state + 1] == NULL)
		{
			if (is_connect)
			{
				PQfinish(st->con);
				st->con = NULL;
			}

			++st->cnt;
			if ((st->cnt >= nxacts && duration <= 0) || timer_exceeded)
				return clientDone(st, true);	/* exit success */
		}

		/* increment state counter */
		st->state++;
		if (commands[st->state] == NULL)
		{
			st->state = 0;
			st->use_file = getrand(0, num_files - 1);
			commands = sql_files[st->use_file];
		}
	}

	if (st->con == NULL)
	{
		instr_time	start,
					end;

		INSTR_TIME_SET_CURRENT(start);
		if ((st->con = doConnect()) == NULL)
		{
			fprintf(stderr, "Client %d aborted in establishing connection.\n", st->id);
			return clientDone(st, false);
		}
		INSTR_TIME_SET_CURRENT(end);
		INSTR_TIME_ACCUM_DIFF(*conn_time, end, start);
	}

	/* Record transaction start time if logging is enabled */
	if (logfile && st->state == 0)
		INSTR_TIME_SET_CURRENT(st->txn_begin);

	/* Record statement start time if per-command latencies are requested */
	if (is_latencies)
		INSTR_TIME_SET_CURRENT(st->stmt_begin);

	if (commands[st->state]->type == SQL_COMMAND)
	{
		const Command *command = commands[st->state];
		int			r;

		if (querymode == QUERY_SIMPLE)
		{
			char	   *sql;

			sql = xstrdup(command->argv[0]);
			sql = assignVariables(st, sql);

			if (debug)
				fprintf(stderr, "client %d sending %s\n", st->id, sql);
			r = PQsendQuery(st->con, sql);
			free(sql);
		}
		else if (querymode == QUERY_EXTENDED)
		{
			const char *sql = command->argv[0];
			const char *params[MAX_ARGS];

			getQueryParams(st, command, params);

			if (debug)
				fprintf(stderr, "client %d sending %s\n", st->id, sql);
			r = PQsendQueryParams(st->con, sql, command->argc - 1,
								  NULL, params, NULL, NULL, 0);
		}
		else if (querymode == QUERY_PREPARED)
		{
			char		name[MAX_PREPARE_NAME];
			const char *params[MAX_ARGS];

			if (!st->prepared[st->use_file])
			{
				int			j;

				for (j = 0; commands[j] != NULL; j++)
				{
					PGresult   *res;
					char		name[MAX_PREPARE_NAME];

					if (commands[j]->type != SQL_COMMAND)
						continue;
					preparedStatementName(name, st->use_file, j);
					res = PQprepare(st->con, name,
						  commands[j]->argv[0], commands[j]->argc - 1, NULL);
					if (PQresultStatus(res) != PGRES_COMMAND_OK)
						fprintf(stderr, "%s", PQerrorMessage(st->con));
					PQclear(res);
				}
				st->prepared[st->use_file] = true;
			}

			getQueryParams(st, command, params);
			preparedStatementName(name, st->use_file, st->state);

			if (debug)
				fprintf(stderr, "client %d sending %s\n", st->id, name);
			r = PQsendQueryPrepared(st->con, name, command->argc - 1,
									params, NULL, NULL, 0);
		}
		else	/* unknown sql mode */
			r = 0;

		if (r == 0)
		{
			if (debug)
				fprintf(stderr, "client %d cannot send %s\n", st->id, command->argv[0]);
			st->ecnt++;
		}
		else
			st->listen = 1;		/* flags that should be listened */
	}
	else if (commands[st->state]->type == META_COMMAND)
	{
		int			argc = commands[st->state]->argc,
					i;
		char	  **argv = commands[st->state]->argv;

		if (debug)
		{
			fprintf(stderr, "client %d executing \\%s", st->id, argv[0]);
			for (i = 1; i < argc; i++)
				fprintf(stderr, " %s", argv[i]);
			fprintf(stderr, "\n");
		}

		if (pg_strcasecmp(argv[0], "setrandom") == 0)
		{
			char	   *var;
			int			min,
						max;
			char		res[64];

			if (*argv[2] == ':')
			{
				if ((var = getVariable(st, argv[2] + 1)) == NULL)
				{
					fprintf(stderr, "%s: undefined variable %s\n", argv[0], argv[2]);
					st->ecnt++;
					return true;
				}
				min = atoi(var);
			}
			else
				min = atoi(argv[2]);

#ifdef NOT_USED
			if (min < 0)
			{
				fprintf(stderr, "%s: invalid minimum number %d\n", argv[0], min);
				st->ecnt++;
				return;
			}
#endif

			if (*argv[3] == ':')
			{
				if ((var = getVariable(st, argv[3] + 1)) == NULL)
				{
					fprintf(stderr, "%s: undefined variable %s\n", argv[0], argv[3]);
					st->ecnt++;
					return true;
				}
				max = atoi(var);
			}
			else
				max = atoi(argv[3]);

			if (max < min || max > MAX_RANDOM_VALUE)
			{
				fprintf(stderr, "%s: invalid maximum number %d\n", argv[0], max);
				st->ecnt++;
				return true;
			}

#ifdef DEBUG
			printf("min: %d max: %d random: %d\n", min, max, getrand(min, max));
#endif
			snprintf(res, sizeof(res), "%d", getrand(min, max));

			if (!putVariable(st, argv[0], argv[1], res))
			{
				st->ecnt++;
				return true;
			}

			st->listen = 1;
		}
		else if (pg_strcasecmp(argv[0], "set") == 0)
		{
			char	   *var;
			int			ope1,
						ope2;
			char		res[64];

			if (*argv[2] == ':')
			{
				if ((var = getVariable(st, argv[2] + 1)) == NULL)
				{
					fprintf(stderr, "%s: undefined variable %s\n", argv[0], argv[2]);
					st->ecnt++;
					return true;
				}
				ope1 = atoi(var);
			}
			else
				ope1 = atoi(argv[2]);

			if (argc < 5)
				snprintf(res, sizeof(res), "%d", ope1);
			else
			{
				if (*argv[4] == ':')
				{
					if ((var = getVariable(st, argv[4] + 1)) == NULL)
					{
						fprintf(stderr, "%s: undefined variable %s\n", argv[0], argv[4]);
						st->ecnt++;
						return true;
					}
					ope2 = atoi(var);
				}
				else
					ope2 = atoi(argv[4]);

				if (strcmp(argv[3], "+") == 0)
					snprintf(res, sizeof(res), "%d", ope1 + ope2);
				else if (strcmp(argv[3], "-") == 0)
					snprintf(res, sizeof(res), "%d", ope1 - ope2);
				else if (strcmp(argv[3], "*") == 0)
					snprintf(res, sizeof(res), "%d", ope1 * ope2);
				else if (strcmp(argv[3], "/") == 0)
				{
					if (ope2 == 0)
					{
						fprintf(stderr, "%s: division by zero\n", argv[0]);
						st->ecnt++;
						return true;
					}
					snprintf(res, sizeof(res), "%d", ope1 / ope2);
				}
				else
				{
					fprintf(stderr, "%s: unsupported operator %s\n", argv[0], argv[3]);
					st->ecnt++;
					return true;
				}
			}

			if (!putVariable(st, argv[0], argv[1], res))
			{
				st->ecnt++;
				return true;
			}

			st->listen = 1;
		}
		else if (pg_strcasecmp(argv[0], "sleep") == 0)
		{
			char	   *var;
			int			usec;
			instr_time	now;

			if (*argv[1] == ':')
			{
				if ((var = getVariable(st, argv[1] + 1)) == NULL)
				{
					fprintf(stderr, "%s: undefined variable %s\n", argv[0], argv[1]);
					st->ecnt++;
					return true;
				}
				usec = atoi(var);
			}
			else
				usec = atoi(argv[1]);

			if (argc > 2)
			{
				if (pg_strcasecmp(argv[2], "ms") == 0)
					usec *= 1000;
				else if (pg_strcasecmp(argv[2], "s") == 0)
					usec *= 1000000;
			}
			else
				usec *= 1000000;

			INSTR_TIME_SET_CURRENT(now);
			st->until = INSTR_TIME_GET_MICROSEC(now) + usec;
			st->sleeping = 1;

			st->listen = 1;
		}
		else if (pg_strcasecmp(argv[0], "setshell") == 0)
		{
			bool		ret = runShellCommand(st, argv[1], argv + 2, argc - 2);

			if (timer_exceeded) /* timeout */
				return clientDone(st, true);
			else if (!ret)		/* on error */
			{
				st->ecnt++;
				return true;
			}
			else	/* succeeded */
				st->listen = 1;
		}
		else if (pg_strcasecmp(argv[0], "shell") == 0)
		{
			bool		ret = runShellCommand(st, NULL, argv + 1, argc - 1);

			if (timer_exceeded) /* timeout */
				return clientDone(st, true);
			else if (!ret)		/* on error */
			{
				st->ecnt++;
				return true;
			}
			else	/* succeeded */
				st->listen = 1;
		}
		goto top;
	}

	return true;
}

/* discard connections */
static void
disconnect_all(CState *state, int length)
{
	int			i;

	for (i = 0; i < length; i++)
	{
		if (state[i].con)
		{
			PQfinish(state[i].con);
			state[i].con = NULL;
		}
	}
}

/* create tables and setup data */
static void
init(void)
{
	/*
	 * Note: TPC-B requires at least 100 bytes per row, and the "filler"
	 * fields in these table declarations were intended to comply with that.
	 * But because they default to NULLs, they don't actually take any space.
	 * We could fix that by giving them non-null default values. However, that
	 * would completely break comparability of pgbench results with prior
	 * versions.  Since pgbench has never pretended to be fully TPC-B
	 * compliant anyway, we stick with the historical behavior.
	 */
	static char *DDLs[] = {
		"drop table if exists pgbench_branches",
		"create table pgbench_branches(bid int not null,bbalance int,filler char(88)) with (fillfactor=%d)",
		"drop table if exists pgbench_tellers",
		"create table pgbench_tellers(tid int not null,bid int,tbalance int,filler char(84)) with (fillfactor=%d)",
		"drop table if exists pgbench_accounts",
		"create table pgbench_accounts(aid int not null,bid int,abalance int,filler char(84)) with (fillfactor=%d)",
		"drop table if exists pgbench_history",
		"create table pgbench_history(tid int,bid int,aid int,delta int,mtime timestamp,filler char(22))"
	};
	static char *DDLAFTERs[] = {
		"alter table pgbench_branches add primary key (bid)",
		"alter table pgbench_tellers add primary key (tid)",
		"alter table pgbench_accounts add primary key (aid)"
	};

	PGconn	   *con;
	PGresult   *res;
	char		sql[256];
	int			i;

	if ((con = doConnect()) == NULL)
		exit(1);

	for (i = 0; i < lengthof(DDLs); i++)
	{
		/*
		 * set fillfactor for branches, tellers and accounts tables
		 */
		if ((strstr(DDLs[i], "create table pgbench_branches") == DDLs[i]) ||
			(strstr(DDLs[i], "create table pgbench_tellers") == DDLs[i]) ||
			(strstr(DDLs[i], "create table pgbench_accounts") == DDLs[i]))
		{
			char		ddl_stmt[128];

			snprintf(ddl_stmt, 128, DDLs[i], fillfactor);
			executeStatement(con, ddl_stmt);
			continue;
		}
		else
			executeStatement(con, DDLs[i]);
	}

	executeStatement(con, "begin");

	for (i = 0; i < nbranches * scale; i++)
	{
		snprintf(sql, 256, "insert into pgbench_branches(bid,bbalance) values(%d,0)", i + 1);
		executeStatement(con, sql);
	}

	for (i = 0; i < ntellers * scale; i++)
	{
		snprintf(sql, 256, "insert into pgbench_tellers(tid,bid,tbalance) values (%d,%d,0)",
				 i + 1, i / ntellers + 1);
		executeStatement(con, sql);
	}

	executeStatement(con, "commit");

	/*
	 * fill the pgbench_accounts table with some data
	 */
	fprintf(stderr, "creating tables...\n");

	executeStatement(con, "begin");
	executeStatement(con, "truncate pgbench_accounts");

	res = PQexec(con, "copy pgbench_accounts from stdin");
	if (PQresultStatus(res) != PGRES_COPY_IN)
	{
		fprintf(stderr, "%s", PQerrorMessage(con));
		exit(1);
	}
	PQclear(res);

	for (i = 0; i < naccounts * scale; i++)
	{
		int			j = i + 1;

		snprintf(sql, 256, "%d\t%d\t%d\t\n", j, i / naccounts + 1, 0);
		if (PQputline(con, sql))
		{
			fprintf(stderr, "PQputline failed\n");
			exit(1);
		}

		if (j % 10000 == 0)
			fprintf(stderr, "%d tuples done.\n", j);
	}
	if (PQputline(con, "\\.\n"))
	{
		fprintf(stderr, "very last PQputline failed\n");
		exit(1);
	}
	if (PQendcopy(con))
	{
		fprintf(stderr, "PQendcopy failed\n");
		exit(1);
	}
	executeStatement(con, "commit");

	/*
	 * create indexes
	 */
	fprintf(stderr, "set primary key...\n");
	for (i = 0; i < lengthof(DDLAFTERs); i++)
		executeStatement(con, DDLAFTERs[i]);

	/* vacuum */
	fprintf(stderr, "vacuum...");
	executeStatement(con, "vacuum analyze pgbench_branches");
	executeStatement(con, "vacuum analyze pgbench_tellers");
	executeStatement(con, "vacuum analyze pgbench_accounts");
	executeStatement(con, "vacuum analyze pgbench_history");

	fprintf(stderr, "done.\n");
	PQfinish(con);
}

/*
 * Parse the raw sql and replace :param to $n.
 */
static bool
parseQuery(Command *cmd, const char *raw_sql)
{
	char	   *sql,
			   *p;

	sql = xstrdup(raw_sql);
	cmd->argc = 1;

	p = sql;
	while ((p = strchr(p, ':')) != NULL)
	{
		char		var[12];
		char	   *name;
		int			eaten;

		name = parseVariable(p, &eaten);
		if (name == NULL)
		{
			while (*p == ':')
			{
				p++;
			}
			continue;
		}

		if (cmd->argc >= MAX_ARGS)
		{
			fprintf(stderr, "statement has too many arguments (maximum is %d): %s\n", MAX_ARGS - 1, raw_sql);
			free(name);
			return false;
		}

		sprintf(var, "$%d", cmd->argc);
		p = replaceVariable(&sql, p, eaten, var);

		cmd->argv[cmd->argc] = name;
		cmd->argc++;
	}

	cmd->argv[0] = sql;
	return true;
}

/* Parse a command; return a Command struct, or NULL if it's a comment */
static Command *
process_commands(char *buf)
{
	const char	delim[] = " \f\n\r\t\v";

	Command    *my_commands;
	int			j;
	char	   *p,
			   *tok;

	/* Make the string buf end at the next newline */
	if ((p = strchr(buf, '\n')) != NULL)
		*p = '\0';

	/* Skip leading whitespace */
	p = buf;
	while (isspace((unsigned char) *p))
		p++;

	/* If the line is empty or actually a comment, we're done */
	if (*p == '\0' || strncmp(p, "--", 2) == 0)
		return NULL;

	/* Allocate and initialize Command structure */
	my_commands = (Command *) xmalloc(sizeof(Command));
	my_commands->line = xstrdup(buf);
	my_commands->command_num = num_commands++;
	my_commands->type = 0;		/* until set */
	my_commands->argc = 0;

	if (*p == '\\')
	{
		my_commands->type = META_COMMAND;

		j = 0;
		tok = strtok(++p, delim);

		while (tok != NULL)
		{
			my_commands->argv[j++] = xstrdup(tok);
			my_commands->argc++;
			tok = strtok(NULL, delim);
		}

		if (pg_strcasecmp(my_commands->argv[0], "setrandom") == 0)
		{
			if (my_commands->argc < 4)
			{
				fprintf(stderr, "%s: missing argument\n", my_commands->argv[0]);
				exit(1);
			}

			for (j = 4; j < my_commands->argc; j++)
				fprintf(stderr, "%s: extra argument \"%s\" ignored\n",
						my_commands->argv[0], my_commands->argv[j]);
		}
		else if (pg_strcasecmp(my_commands->argv[0], "set") == 0)
		{
			if (my_commands->argc < 3)
			{
				fprintf(stderr, "%s: missing argument\n", my_commands->argv[0]);
				exit(1);
			}

			for (j = my_commands->argc < 5 ? 3 : 5; j < my_commands->argc; j++)
				fprintf(stderr, "%s: extra argument \"%s\" ignored\n",
						my_commands->argv[0], my_commands->argv[j]);
		}
		else if (pg_strcasecmp(my_commands->argv[0], "sleep") == 0)
		{
			if (my_commands->argc < 2)
			{
				fprintf(stderr, "%s: missing argument\n", my_commands->argv[0]);
				exit(1);
			}

			/*
			 * Split argument into number and unit to allow "sleep 1ms" etc.
			 * We don't have to terminate the number argument with null
			 * because it will be parsed with atoi, which ignores trailing
			 * non-digit characters.
			 */
			if (my_commands->argv[1][0] != ':')
			{
				char	   *c = my_commands->argv[1];

				while (isdigit((unsigned char) *c))
					c++;
				if (*c)
				{
					my_commands->argv[2] = c;
					if (my_commands->argc < 3)
						my_commands->argc = 3;
				}
			}

			if (my_commands->argc >= 3)
			{
				if (pg_strcasecmp(my_commands->argv[2], "us") != 0 &&
					pg_strcasecmp(my_commands->argv[2], "ms") != 0 &&
					pg_strcasecmp(my_commands->argv[2], "s"))
				{
					fprintf(stderr, "%s: unknown time unit '%s' - must be us, ms or s\n",
							my_commands->argv[0], my_commands->argv[2]);
					exit(1);
				}
			}

			for (j = 3; j < my_commands->argc; j++)
				fprintf(stderr, "%s: extra argument \"%s\" ignored\n",
						my_commands->argv[0], my_commands->argv[j]);
		}
		else if (pg_strcasecmp(my_commands->argv[0], "setshell") == 0)
		{
			if (my_commands->argc < 3)
			{
				fprintf(stderr, "%s: missing argument\n", my_commands->argv[0]);
				exit(1);
			}
		}
		else if (pg_strcasecmp(my_commands->argv[0], "shell") == 0)
		{
			if (my_commands->argc < 1)
			{
				fprintf(stderr, "%s: missing command\n", my_commands->argv[0]);
				exit(1);
			}
		}
		else
		{
			fprintf(stderr, "Invalid command %s\n", my_commands->argv[0]);
			exit(1);
		}
	}
	else
	{
		my_commands->type = SQL_COMMAND;

		switch (querymode)
		{
			case QUERY_SIMPLE:
				my_commands->argv[0] = xstrdup(p);
				my_commands->argc++;
				break;
			case QUERY_EXTENDED:
			case QUERY_PREPARED:
				if (!parseQuery(my_commands, p))
					exit(1);
				break;
			default:
				exit(1);
		}
	}

	return my_commands;
}

static int
process_file(char *filename)
{
#define COMMANDS_ALLOC_NUM 128

	Command   **my_commands;
	FILE	   *fd;
	int			lineno;
	char		buf[BUFSIZ];
	int			alloc_num;

	if (num_files >= MAX_FILES)
	{
		fprintf(stderr, "Up to only %d SQL files are allowed\n", MAX_FILES);
		exit(1);
	}

	alloc_num = COMMANDS_ALLOC_NUM;
	my_commands = (Command **) xmalloc(sizeof(Command *) * alloc_num);

	if (strcmp(filename, "-") == 0)
		fd = stdin;
	else if ((fd = fopen(filename, "r")) == NULL)
	{
		free(my_commands);
		fprintf(stderr, "%s: %s\n", filename, strerror(errno));
		return false;
	}

	lineno = 0;

	while (fgets(buf, sizeof(buf), fd) != NULL)
	{
		Command    *command;

		command = process_commands(buf);
		if (command == NULL)
			continue;

		my_commands[lineno] = command;
		lineno++;

		if (lineno >= alloc_num)
		{
			alloc_num += COMMANDS_ALLOC_NUM;
			my_commands = xrealloc(my_commands, sizeof(Command *) * alloc_num);
		}
	}
	fclose(fd);

	my_commands[lineno] = NULL;

	sql_files[num_files++] = my_commands;

	return true;
}

static Command **
process_builtin(char *tb)
{
#define COMMANDS_ALLOC_NUM 128

	Command   **my_commands;
	int			lineno;
	char		buf[BUFSIZ];
	int			alloc_num;

	alloc_num = COMMANDS_ALLOC_NUM;
	my_commands = (Command **) xmalloc(sizeof(Command *) * alloc_num);

	lineno = 0;

	for (;;)
	{
		char	   *p;
		Command    *command;

		p = buf;
		while (*tb && *tb != '\n')
			*p++ = *tb++;

		if (*tb == '\0')
			break;

		if (*tb == '\n')
			tb++;

		*p = '\0';

		command = process_commands(buf);
		if (command == NULL)
			continue;

		my_commands[lineno] = command;
		lineno++;

		if (lineno >= alloc_num)
		{
			alloc_num += COMMANDS_ALLOC_NUM;
			my_commands = xrealloc(my_commands, sizeof(Command *) * alloc_num);
		}
	}

	my_commands[lineno] = NULL;

	return my_commands;
}

/* print out results */
static void
printResults(int ttype, int normal_xacts, int nclients,
			 TState *threads, int nthreads,
			 instr_time total_time, instr_time conn_total_time)
{
	double		time_include,
				tps_include,
				tps_exclude;
	char	   *s;

	time_include = INSTR_TIME_GET_DOUBLE(total_time);
	tps_include = normal_xacts / time_include;
	tps_exclude = normal_xacts / (time_include -
						(INSTR_TIME_GET_DOUBLE(conn_total_time) / nthreads));

	if (ttype == 0)
		s = "TPC-B (sort of)";
	else if (ttype == 2)
		s = "Update only pgbench_accounts";
	else if (ttype == 1)
		s = "SELECT only";
	else
		s = "Custom query";

	printf("transaction type: %s\n", s);
	printf("scaling factor: %d\n", scale);
	printf("query mode: %s\n", QUERYMODE[querymode]);
	printf("number of clients: %d\n", nclients);
	printf("number of threads: %d\n", nthreads);
	if (duration <= 0)
	{
		printf("number of transactions per client: %d\n", nxacts);
		printf("number of transactions actually processed: %d/%d\n",
			   normal_xacts, nxacts * nclients);
	}
	else
	{
		printf("duration: %d s\n", duration);
		printf("number of transactions actually processed: %d\n",
			   normal_xacts);
	}
	printf("tps = %f (including connections establishing)\n", tps_include);
	printf("tps = %f (excluding connections establishing)\n", tps_exclude);

	/* Report per-command latencies */
	if (is_latencies)
	{
		int			i;

		for (i = 0; i < num_files; i++)
		{
			Command   **commands;

			if (num_files > 1)
				printf("statement latencies in milliseconds, file %d:\n", i + 1);
			else
				printf("statement latencies in milliseconds:\n");

			for (commands = sql_files[i]; *commands != NULL; commands++)
			{
				Command    *command = *commands;
				int			cnum = command->command_num;
				double		total_time;
				instr_time	total_exec_elapsed;
				int			total_exec_count;
				int			t;

				/* Accumulate per-thread data for command */
				INSTR_TIME_SET_ZERO(total_exec_elapsed);
				total_exec_count = 0;
				for (t = 0; t < nthreads; t++)
				{
					TState	   *thread = &threads[t];

					INSTR_TIME_ADD(total_exec_elapsed,
								   thread->exec_elapsed[cnum]);
					total_exec_count += thread->exec_count[cnum];
				}

				if (total_exec_count > 0)
					total_time = INSTR_TIME_GET_MILLISEC(total_exec_elapsed) / (double) total_exec_count;
				else
					total_time = 0.0;

				printf("\t%f\t%s\n", total_time, command->line);
			}
		}
	}
}


int
main(int argc, char **argv)
{
	int			c;
	int			nclients = 1;	/* default number of simulated clients */
	int			nthreads = 1;	/* default number of threads */
	int			is_init_mode = 0;		/* initialize mode? */
	int			is_no_vacuum = 0;		/* no vacuum at all before testing? */
	int			do_vacuum_accounts = 0; /* do vacuum accounts before testing? */
	int			ttype = 0;		/* transaction type. 0: TPC-B, 1: SELECT only,
								 * 2: skip update of branches and tellers */
	char	   *filename = NULL;
	bool		scale_given = false;

	CState	   *state;			/* status of clients */
	TState	   *threads;		/* array of thread */

	instr_time	start_time;		/* start up time */
	instr_time	total_time;
	instr_time	conn_total_time;
	int			total_xacts;

	int			i;

#ifdef HAVE_GETRLIMIT
	struct rlimit rlim;
#endif

	PGconn	   *con;
	PGresult   *res;
	char	   *env;

	char		val[64];

	const char *progname;

	progname = get_progname(argv[0]);

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage(progname);
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pgbench (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

#ifdef WIN32
	/* stderr is buffered on Win32. */
	setvbuf(stderr, NULL, _IONBF, 0);
#endif

	if ((env = getenv("PGHOST")) != NULL && *env != '\0')
		pghost = env;
	if ((env = getenv("PGPORT")) != NULL && *env != '\0')
		pgport = env;
	else if ((env = getenv("PGUSER")) != NULL && *env != '\0')
		login = env;

	state = (CState *) xmalloc(sizeof(CState));
	memset(state, 0, sizeof(CState));

	while ((c = getopt(argc, argv, "ih:nvp:dSNc:j:Crs:t:T:U:lf:D:F:M:")) != -1)
	{
		switch (c)
		{
			case 'i':
				is_init_mode++;
				break;
			case 'h':
				pghost = optarg;
				break;
			case 'n':
				is_no_vacuum++;
				break;
			case 'v':
				do_vacuum_accounts++;
				break;
			case 'p':
				pgport = optarg;
				break;
			case 'd':
				debug++;
				break;
			case 'S':
				ttype = 1;
				break;
			case 'N':
				ttype = 2;
				break;
			case 'c':
				nclients = atoi(optarg);
				if (nclients <= 0 || nclients > MAXCLIENTS)
				{
					fprintf(stderr, "invalid number of clients: %d\n", nclients);
					exit(1);
				}
#ifdef HAVE_GETRLIMIT
#ifdef RLIMIT_NOFILE			/* most platforms use RLIMIT_NOFILE */
				if (getrlimit(RLIMIT_NOFILE, &rlim) == -1)
#else							/* but BSD doesn't ... */
				if (getrlimit(RLIMIT_OFILE, &rlim) == -1)
#endif   /* RLIMIT_NOFILE */
				{
					fprintf(stderr, "getrlimit failed: %s\n", strerror(errno));
					exit(1);
				}
				if (rlim.rlim_cur <= (nclients + 2))
				{
					fprintf(stderr, "You need at least %d open files but you are only allowed to use %ld.\n", nclients + 2, (long) rlim.rlim_cur);
					fprintf(stderr, "Use limit/ulimit to increase the limit before using pgbench.\n");
					exit(1);
				}
#endif   /* HAVE_GETRLIMIT */
				break;
			case 'j':			/* jobs */
				nthreads = atoi(optarg);
				if (nthreads <= 0)
				{
					fprintf(stderr, "invalid number of threads: %d\n", nthreads);
					exit(1);
				}
				break;
			case 'C':
				is_connect = true;
				break;
			case 'r':
				is_latencies = true;
				break;
			case 's':
				scale_given = true;
				scale = atoi(optarg);
				if (scale <= 0)
				{
					fprintf(stderr, "invalid scaling factor: %d\n", scale);
					exit(1);
				}
				break;
			case 't':
				if (duration > 0)
				{
					fprintf(stderr, "specify either a number of transactions (-t) or a duration (-T), not both.\n");
					exit(1);
				}
				nxacts = atoi(optarg);
				if (nxacts <= 0)
				{
					fprintf(stderr, "invalid number of transactions: %d\n", nxacts);
					exit(1);
				}
				break;
			case 'T':
				if (nxacts > 0)
				{
					fprintf(stderr, "specify either a number of transactions (-t) or a duration (-T), not both.\n");
					exit(1);
				}
				duration = atoi(optarg);
				if (duration <= 0)
				{
					fprintf(stderr, "invalid duration: %d\n", duration);
					exit(1);
				}
				break;
			case 'U':
				login = optarg;
				break;
			case 'l':
				use_log = true;
				break;
			case 'f':
				ttype = 3;
				filename = optarg;
				if (process_file(filename) == false || *sql_files[num_files - 1] == NULL)
					exit(1);
				break;
			case 'D':
				{
					char	   *p;

					if ((p = strchr(optarg, '=')) == NULL || p == optarg || *(p + 1) == '\0')
					{
						fprintf(stderr, "invalid variable definition: %s\n", optarg);
						exit(1);
					}

					*p++ = '\0';
					if (!putVariable(&state[0], "option", optarg, p))
						exit(1);
				}
				break;
			case 'F':
				fillfactor = atoi(optarg);
				if ((fillfactor < 10) || (fillfactor > 100))
				{
					fprintf(stderr, "invalid fillfactor: %d\n", fillfactor);
					exit(1);
				}
				break;
			case 'M':
				if (num_files > 0)
				{
					fprintf(stderr, "query mode (-M) should be specified before transaction scripts (-f)\n");
					exit(1);
				}
				for (querymode = 0; querymode < NUM_QUERYMODE; querymode++)
					if (strcmp(optarg, QUERYMODE[querymode]) == 0)
						break;
				if (querymode >= NUM_QUERYMODE)
				{
					fprintf(stderr, "invalid query mode (-M): %s\n", optarg);
					exit(1);
				}
				break;
			default:
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
				exit(1);
				break;
		}
	}

	if (argc > optind)
		dbName = argv[optind];
	else
	{
		if ((env = getenv("PGDATABASE")) != NULL && *env != '\0')
			dbName = env;
		else if (login != NULL && *login != '\0')
			dbName = login;
		else
			dbName = "";
	}

	if (is_init_mode)
	{
		init();
		exit(0);
	}

	/* Use DEFAULT_NXACTS if neither nxacts nor duration is specified. */
	if (nxacts <= 0 && duration <= 0)
		nxacts = DEFAULT_NXACTS;

	if (nclients % nthreads != 0)
	{
		fprintf(stderr, "number of clients (%d) must be a multiple of number of threads (%d)\n", nclients, nthreads);
		exit(1);
	}

	/*
	 * is_latencies only works with multiple threads in thread-based
	 * implementations, not fork-based ones, because it supposes that the
	 * parent can see changes made to the per-thread execution stats by child
	 * threads.  It seems useful enough to accept despite this limitation, but
	 * perhaps we should FIXME someday (by passing the stats data back up
	 * through the parent-to-child pipes).
	 */
#ifndef ENABLE_THREAD_SAFETY
	if (is_latencies && nthreads > 1)
	{
		fprintf(stderr, "-r does not work with -j larger than 1 on this platform.\n");
		exit(1);
	}
#endif

	/*
	 * save main process id in the global variable because process id will be
	 * changed after fork.
	 */
	main_pid = (int) getpid();

	if (nclients > 1)
	{
		state = (CState *) xrealloc(state, sizeof(CState) * nclients);
		memset(state + 1, 0, sizeof(CState) * (nclients - 1));

		/* copy any -D switch values to all clients */
		for (i = 1; i < nclients; i++)
		{
			int			j;

			state[i].id = i;
			for (j = 0; j < state[0].nvariables; j++)
			{
				if (!putVariable(&state[i], "startup", state[0].variables[j].name, state[0].variables[j].value))
					exit(1);
			}
		}
	}

	if (debug)
	{
		if (duration <= 0)
			printf("pghost: %s pgport: %s nclients: %d nxacts: %d dbName: %s\n",
				   pghost, pgport, nclients, nxacts, dbName);
		else
			printf("pghost: %s pgport: %s nclients: %d duration: %d dbName: %s\n",
				   pghost, pgport, nclients, duration, dbName);
	}

	/* opening connection... */
	con = doConnect();
	if (con == NULL)
		exit(1);

	if (PQstatus(con) == CONNECTION_BAD)
	{
		fprintf(stderr, "Connection to database '%s' failed.\n", dbName);
		fprintf(stderr, "%s", PQerrorMessage(con));
		exit(1);
	}

	if (ttype != 3)
	{
		/*
		 * get the scaling factor that should be same as count(*) from
		 * pgbench_branches if this is not a custom query
		 */
		res = PQexec(con, "select count(*) from pgbench_branches");
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			fprintf(stderr, "%s", PQerrorMessage(con));
			exit(1);
		}
		scale = atoi(PQgetvalue(res, 0, 0));
		if (scale < 0)
		{
			fprintf(stderr, "count(*) from pgbench_branches invalid (%d)\n", scale);
			exit(1);
		}
		PQclear(res);

		/* warn if we override user-given -s switch */
		if (scale_given)
			fprintf(stderr,
			"Scale option ignored, using pgbench_branches table count = %d\n",
					scale);
	}

	/*
	 * :scale variables normally get -s or database scale, but don't override
	 * an explicit -D switch
	 */
	if (getVariable(&state[0], "scale") == NULL)
	{
		snprintf(val, sizeof(val), "%d", scale);
		for (i = 0; i < nclients; i++)
		{
			if (!putVariable(&state[i], "startup", "scale", val))
				exit(1);
		}
	}

	if (!is_no_vacuum)
	{
		fprintf(stderr, "starting vacuum...");
		executeStatement(con, "vacuum pgbench_branches");
		executeStatement(con, "vacuum pgbench_tellers");
		executeStatement(con, "truncate pgbench_history");
		fprintf(stderr, "end.\n");

		if (do_vacuum_accounts)
		{
			fprintf(stderr, "starting vacuum pgbench_accounts...");
			executeStatement(con, "vacuum analyze pgbench_accounts");
			fprintf(stderr, "end.\n");
		}
	}
	PQfinish(con);

	/* set random seed */
	INSTR_TIME_SET_CURRENT(start_time);
	srandom((unsigned int) INSTR_TIME_GET_MICROSEC(start_time));

	/* process builtin SQL scripts */
	switch (ttype)
	{
		case 0:
			sql_files[0] = process_builtin(tpc_b);
			num_files = 1;
			break;

		case 1:
			sql_files[0] = process_builtin(select_only);
			num_files = 1;
			break;

		case 2:
			sql_files[0] = process_builtin(simple_update);
			num_files = 1;
			break;

		default:
			break;
	}

	/* set up thread data structures */
	threads = (TState *) xmalloc(sizeof(TState) * nthreads);
	for (i = 0; i < nthreads; i++)
	{
		TState	   *thread = &threads[i];

		thread->tid = i;
		thread->state = &state[nclients / nthreads * i];
		thread->nstate = nclients / nthreads;

		if (is_latencies)
		{
			/* Reserve memory for the thread to store per-command latencies */
			int			t;

			thread->exec_elapsed = (instr_time *)
				xmalloc(sizeof(instr_time) * num_commands);
			thread->exec_count = (int *)
				xmalloc(sizeof(int) * num_commands);

			for (t = 0; t < num_commands; t++)
			{
				INSTR_TIME_SET_ZERO(thread->exec_elapsed[t]);
				thread->exec_count[t] = 0;
			}
		}
		else
		{
			thread->exec_elapsed = NULL;
			thread->exec_count = NULL;
		}
	}

	/* get start up time */
	INSTR_TIME_SET_CURRENT(start_time);

	/* set alarm if duration is specified. */
	if (duration > 0)
		setalarm(duration);

	/* start threads */
	for (i = 0; i < nthreads; i++)
	{
		TState	   *thread = &threads[i];

		INSTR_TIME_SET_CURRENT(thread->start_time);

		/* the first thread (i = 0) is executed by main thread */
		if (i > 0)
		{
			int			err = pthread_create(&thread->thread, NULL, threadRun, thread);

			if (err != 0 || thread->thread == INVALID_THREAD)
			{
				fprintf(stderr, "cannot create thread: %s\n", strerror(err));
				exit(1);
			}
		}
		else
		{
			thread->thread = INVALID_THREAD;
		}
	}

	/* wait for threads and accumulate results */
	total_xacts = 0;
	INSTR_TIME_SET_ZERO(conn_total_time);
	for (i = 0; i < nthreads; i++)
	{
		void	   *ret = NULL;

		if (threads[i].thread == INVALID_THREAD)
			ret = threadRun(&threads[i]);
		else
			pthread_join(threads[i].thread, &ret);

		if (ret != NULL)
		{
			TResult    *r = (TResult *) ret;

			total_xacts += r->xacts;
			INSTR_TIME_ADD(conn_total_time, r->conn_time);
			free(ret);
		}
	}
	disconnect_all(state, nclients);

	/* get end time */
	INSTR_TIME_SET_CURRENT(total_time);
	INSTR_TIME_SUBTRACT(total_time, start_time);
	printResults(ttype, total_xacts, nclients, threads, nthreads,
				 total_time, conn_total_time);

	return 0;
}

static void *
threadRun(void *arg)
{
	TState	   *thread = (TState *) arg;
	CState	   *state = thread->state;
	TResult    *result;
	FILE	   *logfile = NULL; /* per-thread log file */
	instr_time	start,
				end;
	int			nstate = thread->nstate;
	int			remains = nstate;		/* number of remaining clients */
	int			i;

	result = xmalloc(sizeof(TResult));
	INSTR_TIME_SET_ZERO(result->conn_time);

	/* open log file if requested */
	if (use_log)
	{
		char		logpath[64];

		if (thread->tid == 0)
			snprintf(logpath, sizeof(logpath), "pgbench_log.%d", main_pid);
		else
			snprintf(logpath, sizeof(logpath), "pgbench_log.%d.%d", main_pid, thread->tid);
		logfile = fopen(logpath, "w");

		if (logfile == NULL)
		{
			fprintf(stderr, "Couldn't open logfile \"%s\": %s", logpath, strerror(errno));
			goto done;
		}
	}

	if (!is_connect)
	{
		/* make connections to the database */
		for (i = 0; i < nstate; i++)
		{
			if ((state[i].con = doConnect()) == NULL)
				goto done;
		}
	}

	/* time after thread and connections set up */
	INSTR_TIME_SET_CURRENT(result->conn_time);
	INSTR_TIME_SUBTRACT(result->conn_time, thread->start_time);

	/* send start up queries in async manner */
	for (i = 0; i < nstate; i++)
	{
		CState	   *st = &state[i];
		Command   **commands = sql_files[st->use_file];
		int			prev_ecnt = st->ecnt;

		st->use_file = getrand(0, num_files - 1);
		if (!doCustom(thread, st, &result->conn_time, logfile))
			remains--;			/* I've aborted */

		if (st->ecnt > prev_ecnt && commands[st->state]->type == META_COMMAND)
		{
			fprintf(stderr, "Client %d aborted in state %d. Execution meta-command failed.\n", i, st->state);
			remains--;			/* I've aborted */
			PQfinish(st->con);
			st->con = NULL;
		}
	}

	while (remains > 0)
	{
		fd_set		input_mask;
		int			maxsock;	/* max socket number to be waited */
		int64		now_usec = 0;
		int64		min_usec;

		FD_ZERO(&input_mask);

		maxsock = -1;
		min_usec = INT64_MAX;
		for (i = 0; i < nstate; i++)
		{
			CState	   *st = &state[i];
			Command   **commands = sql_files[st->use_file];
			int			sock;

			if (st->sleeping)
			{
				int			this_usec;

				if (min_usec == INT64_MAX)
				{
					instr_time	now;

					INSTR_TIME_SET_CURRENT(now);
					now_usec = INSTR_TIME_GET_MICROSEC(now);
				}

				this_usec = st->until - now_usec;
				if (min_usec > this_usec)
					min_usec = this_usec;
			}
			else if (st->con == NULL)
			{
				continue;
			}
			else if (commands[st->state]->type == META_COMMAND)
			{
				min_usec = 0;	/* the connection is ready to run */
				break;
			}

			sock = PQsocket(st->con);
			if (sock < 0)
			{
				fprintf(stderr, "bad socket: %s\n", strerror(errno));
				goto done;
			}

			FD_SET(sock, &input_mask);

			if (maxsock < sock)
				maxsock = sock;
		}

		if (min_usec > 0 && maxsock != -1)
		{
			int			nsocks; /* return from select(2) */

			if (min_usec != INT64_MAX)
			{
				struct timeval timeout;

				timeout.tv_sec = min_usec / 1000000;
				timeout.tv_usec = min_usec % 1000000;
				nsocks = select(maxsock + 1, &input_mask, NULL, NULL, &timeout);
			}
			else
				nsocks = select(maxsock + 1, &input_mask, NULL, NULL, NULL);
			if (nsocks < 0)
			{
				if (errno == EINTR)
					continue;
				/* must be something wrong */
				fprintf(stderr, "select failed: %s\n", strerror(errno));
				goto done;
			}
		}

		/* ok, backend returns reply */
		for (i = 0; i < nstate; i++)
		{
			CState	   *st = &state[i];
			Command   **commands = sql_files[st->use_file];
			int			prev_ecnt = st->ecnt;

			if (st->con && (FD_ISSET(PQsocket(st->con), &input_mask)
							|| commands[st->state]->type == META_COMMAND))
			{
				if (!doCustom(thread, st, &result->conn_time, logfile))
					remains--;	/* I've aborted */
			}

			if (st->ecnt > prev_ecnt && commands[st->state]->type == META_COMMAND)
			{
				fprintf(stderr, "Client %d aborted in state %d. Execution of meta-command failed.\n", i, st->state);
				remains--;		/* I've aborted */
				PQfinish(st->con);
				st->con = NULL;
			}
		}
	}

done:
	INSTR_TIME_SET_CURRENT(start);
	disconnect_all(state, nstate);
	result->xacts = 0;
	for (i = 0; i < nstate; i++)
		result->xacts += state[i].cnt;
	INSTR_TIME_SET_CURRENT(end);
	INSTR_TIME_ACCUM_DIFF(result->conn_time, end, start);
	if (logfile)
		fclose(logfile);
	return result;
}


/*
 * Support for duration option: set timer_exceeded after so many seconds.
 */

#ifndef WIN32

static void
handle_sig_alarm(SIGNAL_ARGS)
{
	timer_exceeded = true;
}

static void
setalarm(int seconds)
{
	pqsignal(SIGALRM, handle_sig_alarm);
	alarm(seconds);
}

#ifndef ENABLE_THREAD_SAFETY

/*
 * implements pthread using fork.
 */

typedef struct fork_pthread
{
	pid_t		pid;
	int			pipes[2];
}	fork_pthread;

static int
pthread_create(pthread_t *thread,
			   pthread_attr_t * attr,
			   void *(*start_routine) (void *),
			   void *arg)
{
	fork_pthread *th;
	void	   *ret;
	instr_time	start_time;

	th = (fork_pthread *) xmalloc(sizeof(fork_pthread));
	if (pipe(th->pipes) < 0)
	{
		free(th);
		return errno;
	}

	th->pid = fork();
	if (th->pid == -1)			/* error */
	{
		free(th);
		return errno;
	}
	if (th->pid != 0)			/* in parent process */
	{
		close(th->pipes[1]);
		*thread = th;
		return 0;
	}

	/* in child process */
	close(th->pipes[0]);

	/* set alarm again because the child does not inherit timers */
	if (duration > 0)
		setalarm(duration);

	/*
	 * Set a different random seed in each child process.  Otherwise they all
	 * inherit the parent's state and generate the same "random" sequence. (In
	 * the threaded case, the different threads will obtain subsets of the
	 * output of a single random() sequence, which should be okay for our
	 * purposes.)
	 */
	INSTR_TIME_SET_CURRENT(start_time);
	srandom(((unsigned int) INSTR_TIME_GET_MICROSEC(start_time)) +
			((unsigned int) getpid()));

	ret = start_routine(arg);
	write(th->pipes[1], ret, sizeof(TResult));
	close(th->pipes[1]);
	free(th);
	exit(0);
}

static int
pthread_join(pthread_t th, void **thread_return)
{
	int			status;

	while (waitpid(th->pid, &status, 0) != th->pid)
	{
		if (errno != EINTR)
			return errno;
	}

	if (thread_return != NULL)
	{
		/* assume result is TResult */
		*thread_return = xmalloc(sizeof(TResult));
		if (read(th->pipes[0], *thread_return, sizeof(TResult)) != sizeof(TResult))
		{
			free(*thread_return);
			*thread_return = NULL;
		}
	}
	close(th->pipes[0]);

	free(th);
	return 0;
}
#endif
#else							/* WIN32 */

static VOID CALLBACK
win32_timer_callback(PVOID lpParameter, BOOLEAN TimerOrWaitFired)
{
	timer_exceeded = true;
}

static void
setalarm(int seconds)
{
	HANDLE		queue;
	HANDLE		timer;

	/* This function will be called at most once, so we can cheat a bit. */
	queue = CreateTimerQueue();
	if (seconds > ((DWORD) -1) / 1000 ||
		!CreateTimerQueueTimer(&timer, queue,
							   win32_timer_callback, NULL, seconds * 1000, 0,
							   WT_EXECUTEINTIMERTHREAD | WT_EXECUTEONLYONCE))
	{
		fprintf(stderr, "Failed to set timer\n");
		exit(1);
	}
}

/* partial pthread implementation for Windows */

typedef struct win32_pthread
{
	HANDLE		handle;
	void	   *(*routine) (void *);
	void	   *arg;
	void	   *result;
} win32_pthread;

static unsigned __stdcall
win32_pthread_run(void *arg)
{
	win32_pthread *th = (win32_pthread *) arg;

	th->result = th->routine(th->arg);

	return 0;
}

static int
pthread_create(pthread_t *thread,
			   pthread_attr_t * attr,
			   void *(*start_routine) (void *),
			   void *arg)
{
	int			save_errno;
	win32_pthread *th;

	th = (win32_pthread *) xmalloc(sizeof(win32_pthread));
	th->routine = start_routine;
	th->arg = arg;
	th->result = NULL;

	th->handle = (HANDLE) _beginthreadex(NULL, 0, win32_pthread_run, th, 0, NULL);
	if (th->handle == NULL)
	{
		save_errno = errno;
		free(th);
		return save_errno;
	}

	*thread = th;
	return 0;
}

static int
pthread_join(pthread_t th, void **thread_return)
{
	if (th == NULL || th->handle == NULL)
		return errno = EINVAL;

	if (WaitForSingleObject(th->handle, INFINITE) != WAIT_OBJECT_0)
	{
		_dosmaperr(GetLastError());
		return errno;
	}

	if (thread_return)
		*thread_return = th->result;

	CloseHandle(th->handle);
	free(th);
	return 0;
}

#endif   /* WIN32 */
