/*
 * pgbench.c
 *
 * A simple benchmark program for PostgreSQL
 * Originally written by Tatsuo Ishii and enhanced by many contributors.
 *
 * $PostgreSQL: pgsql/contrib/pgbench/pgbench.c,v 1.87.2.1 2009/07/30 09:28:05 mha Exp $
 * Copyright (c) 2000-2009, PostgreSQL Global Development Group
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
#define FD_SETSIZE 1024		/* set before winsock2.h is included */
#endif   /* ! WIN32 */

#include "postgres_fe.h"

#include "libpq-fe.h"
#include "pqsignal.h"

#include <ctype.h>

#ifdef WIN32
#include <win32.h>
#else
#include <signal.h>
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

int			nclients = 1;		/* default number of simulated clients */
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

#define nbranches	1
#define ntellers	10
#define naccounts	100000

FILE	   *LOGFILE = NULL;

bool		use_log;			/* log transaction latencies to a file */

int			remains;			/* number of remaining clients */

int			is_connect;			/* establish connection  for each transaction */

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
	struct timeval until;		/* napping until */
	Variable   *variables;		/* array of variable definitions */
	int			nvariables;
	struct timeval txn_begin;	/* used for measuring latencies */
	int			use_file;		/* index in sql_files for this client */
	bool		prepared[MAX_FILES];
} CState;

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
	int			type;			/* command type (SQL_COMMAND or META_COMMAND) */
	int			argc;			/* number of commands */
	char	   *argv[MAX_ARGS]; /* command list */
} Command;

Command   **sql_files[MAX_FILES];		/* SQL script files */
int			num_files;			/* number of script files */

/* default scenario */
static char *tpc_b = {
	"\\set nbranches :scale\n"
	"\\set ntellers 10 * :scale\n"
	"\\set naccounts 100000 * :scale\n"
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
	"\\set nbranches :scale\n"
	"\\set ntellers 10 * :scale\n"
	"\\set naccounts 100000 * :scale\n"
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
	"\\set naccounts 100000 * :scale\n"
	"\\setrandom aid 1 :naccounts\n"
	"SELECT abalance FROM pgbench_accounts WHERE aid = :aid;\n"
};

/* Connection overhead time */
static struct timeval conn_total_time = {0, 0};

/* Function prototypes */
static void setalarm(int seconds);


/* Calculate total time */
static void
addTime(struct timeval * t1, struct timeval * t2, struct timeval * result)
{
	int			sec = t1->tv_sec + t2->tv_sec;
	int			usec = t1->tv_usec + t2->tv_usec;

	if (usec >= 1000000)
	{
		usec -= 1000000;
		sec++;
	}
	result->tv_sec = sec;
	result->tv_usec = usec;
}

/* Calculate time difference */
static void
diffTime(struct timeval * t1, struct timeval * t2, struct timeval * result)
{
	int			sec = t1->tv_sec - t2->tv_sec;
	int			usec = t1->tv_usec - t2->tv_usec;

	if (usec < 0)
	{
		usec += 1000000;
		sec--;
	}
	result->tv_sec = sec;
	result->tv_usec = usec;
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
		   "  -l           write transaction times to log file\n"
		   "  -M {simple|extended|prepared}\n"
		   "               protocol for submitting queries to server (default: simple)\n"
		   "  -n           do not run VACUUM before tests\n"
		   "  -N           do not update tables \"pgbench_tellers\" and \"pgbench_branches\"\n"
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

/* check to see if the SQL result was good */
static int
check(CState *state, PGresult *res, int n)
{
	CState	   *st = &state[n];

	switch (PQresultStatus(res))
	{
		case PGRES_COMMAND_OK:
		case PGRES_TUPLES_OK:
			/* OK */
			break;
		default:
			fprintf(stderr, "Client %d aborted in state %d: %s",
					n, st->state, PQerrorMessage(st->con));
			remains--;			/* I've aborted */
			PQfinish(st->con);
			st->con = NULL;
			return (-1);
	}
	return (0);					/* OK */
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

static int
putVariable(CState *st, char *name, char *value)
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

		if (st->variables)
			newvars = (Variable *) realloc(st->variables,
									(st->nvariables + 1) * sizeof(Variable));
		else
			newvars = (Variable *) malloc(sizeof(Variable));

		if (newvars == NULL)
			return false;

		st->variables = newvars;

		var = &newvars[st->nvariables];

		var->name = NULL;
		var->value = NULL;

		if ((var->name = strdup(name)) == NULL
			|| (var->value = strdup(value)) == NULL)
		{
			free(var->name);
			free(var->value);
			return false;
		}

		st->nvariables++;

		qsort((void *) st->variables, st->nvariables, sizeof(Variable),
			  compareVariables);
	}
	else
	{
		char	   *val;

		if ((val = strdup(value)) == NULL)
			return false;

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

	name = malloc(i);
	if (name == NULL)
		return NULL;
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
		char	   *tmp;
		size_t		offset = param - *sql;

		tmp = realloc(*sql, strlen(*sql) - len + valueln + 1);
		if (tmp == NULL)
		{
			free(*sql);
			return NULL;
		}
		*sql = tmp;
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

		if ((p = replaceVariable(&sql, p, eaten, val)) == NULL)
			return NULL;
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

#define MAX_PREPARE_NAME		32
static void
preparedStatementName(char *buffer, int file, int state)
{
	sprintf(buffer, "P%d_%d", file, state);
}

static void
doCustom(CState *state, int n, int debug)
{
	PGresult   *res;
	CState	   *st = &state[n];
	Command   **commands;

top:
	commands = sql_files[st->use_file];

	if (st->sleeping)
	{							/* are we sleeping? */
		int			usec;
		struct timeval now;

		gettimeofday(&now, NULL);
		usec = (st->until.tv_sec - now.tv_sec) * 1000000 +
			st->until.tv_usec - now.tv_usec;
		if (usec <= 0)
			st->sleeping = 0;	/* Done sleeping, go ahead with next command */
		else
			return;				/* Still sleeping, nothing to do here */
	}

	if (st->listen)
	{							/* are we receiver? */
		if (commands[st->state]->type == SQL_COMMAND)
		{
			if (debug)
				fprintf(stderr, "client %d receiving\n", n);
			if (!PQconsumeInput(st->con))
			{					/* there's something wrong */
				fprintf(stderr, "Client %d aborted in state %d. Probably the backend died while processing.\n", n, st->state);
				remains--;		/* I've aborted */
				PQfinish(st->con);
				st->con = NULL;
				return;
			}
			if (PQisBusy(st->con))
				return;			/* don't have the whole result yet */
		}

		/*
		 * transaction finished: record the time it took in the log
		 */
		if (use_log && commands[st->state + 1] == NULL)
		{
			double		diff;
			struct timeval now;

			gettimeofday(&now, NULL);
			diff = (int) (now.tv_sec - st->txn_begin.tv_sec) * 1000000.0 +
				(int) (now.tv_usec - st->txn_begin.tv_usec);

			fprintf(LOGFILE, "%d %d %.0f %d %ld %ld\n",
					st->id, st->cnt, diff, st->use_file,
					(long) now.tv_sec, (long) now.tv_usec);
		}

		if (commands[st->state]->type == SQL_COMMAND)
		{
			res = PQgetResult(st->con);
			if (check(state, res, n))
			{
				PQclear(res);
				return;
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
			{
				remains--;		/* I've done */
				if (st->con != NULL)
				{
					PQfinish(st->con);
					st->con = NULL;
				}
				return;
			}
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
		struct timeval t1,
					t2,
					t3;

		gettimeofday(&t1, NULL);
		if ((st->con = doConnect()) == NULL)
		{
			fprintf(stderr, "Client %d aborted in establishing connection.\n",
					n);
			remains--;			/* I've aborted */
			PQfinish(st->con);
			st->con = NULL;
			return;
		}
		gettimeofday(&t2, NULL);
		diffTime(&t2, &t1, &t3);
		addTime(&conn_total_time, &t3, &conn_total_time);
	}

	if (use_log && st->state == 0)
		gettimeofday(&(st->txn_begin), NULL);

	if (commands[st->state]->type == SQL_COMMAND)
	{
		const Command *command = commands[st->state];
		int			r;

		if (querymode == QUERY_SIMPLE)
		{
			char	   *sql;

			if ((sql = strdup(command->argv[0])) == NULL
				|| (sql = assignVariables(st, sql)) == NULL)
			{
				fprintf(stderr, "out of memory\n");
				st->ecnt++;
				return;
			}

			if (debug)
				fprintf(stderr, "client %d sending %s\n", n, sql);
			r = PQsendQuery(st->con, sql);
			free(sql);
		}
		else if (querymode == QUERY_EXTENDED)
		{
			const char *sql = command->argv[0];
			const char *params[MAX_ARGS];

			getQueryParams(st, command, params);

			if (debug)
				fprintf(stderr, "client %d sending %s\n", n, sql);
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
				fprintf(stderr, "client %d sending %s\n", n, name);
			r = PQsendQueryPrepared(st->con, name, command->argc - 1,
									params, NULL, NULL, 0);
		}
		else	/* unknown sql mode */
			r = 0;

		if (r == 0)
		{
			if (debug)
				fprintf(stderr, "client %d cannot send %s\n", n, command->argv[0]);
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
			fprintf(stderr, "client %d executing \\%s", n, argv[0]);
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
					return;
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
					return;
				}
				max = atoi(var);
			}
			else
				max = atoi(argv[3]);

			if (max < min || max > MAX_RANDOM_VALUE)
			{
				fprintf(stderr, "%s: invalid maximum number %d\n", argv[0], max);
				st->ecnt++;
				return;
			}

#ifdef DEBUG
			printf("min: %d max: %d random: %d\n", min, max, getrand(min, max));
#endif
			snprintf(res, sizeof(res), "%d", getrand(min, max));

			if (putVariable(st, argv[1], res) == false)
			{
				fprintf(stderr, "%s: out of memory\n", argv[0]);
				st->ecnt++;
				return;
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
					return;
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
						return;
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
						return;
					}
					snprintf(res, sizeof(res), "%d", ope1 / ope2);
				}
				else
				{
					fprintf(stderr, "%s: unsupported operator %s\n", argv[0], argv[3]);
					st->ecnt++;
					return;
				}
			}

			if (putVariable(st, argv[1], res) == false)
			{
				fprintf(stderr, "%s: out of memory\n", argv[0]);
				st->ecnt++;
				return;
			}

			st->listen = 1;
		}
		else if (pg_strcasecmp(argv[0], "sleep") == 0)
		{
			char	   *var;
			int			usec;
			struct timeval now;

			if (*argv[1] == ':')
			{
				if ((var = getVariable(st, argv[1] + 1)) == NULL)
				{
					fprintf(stderr, "%s: undefined variable %s\n", argv[0], argv[1]);
					st->ecnt++;
					return;
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

			gettimeofday(&now, NULL);
			st->until.tv_sec = now.tv_sec + (now.tv_usec + usec) / 1000000;
			st->until.tv_usec = (now.tv_usec + usec) % 1000000;
			st->sleeping = 1;

			st->listen = 1;
		}

		goto top;
	}
}

/* discard connections */
static void
disconnect_all(CState *state)
{
	int			i;

	for (i = 0; i < nclients; i++)
	{
		if (state[i].con)
			PQfinish(state[i].con);
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
		snprintf(sql, 256, "insert into pgbench_tellers(tid,bid,tbalance) values (%d,%d,0)"
				 ,i + 1, i / ntellers + 1);
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

	sql = strdup(raw_sql);
	if (sql == NULL)
		return false;
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
			return false;
		}

		sprintf(var, "$%d", cmd->argc);
		if ((p = replaceVariable(&sql, p, eaten, var)) == NULL)
			return false;

		cmd->argv[cmd->argc] = name;
		cmd->argc++;
	}

	cmd->argv[0] = sql;
	return true;
}

static Command *
process_commands(char *buf)
{
	const char	delim[] = " \f\n\r\t\v";

	Command    *my_commands;
	int			j;
	char	   *p,
			   *tok;

	if ((p = strchr(buf, '\n')) != NULL)
		*p = '\0';

	p = buf;
	while (isspace((unsigned char) *p))
		p++;

	if (*p == '\0' || strncmp(p, "--", 2) == 0)
	{
		return NULL;
	}

	my_commands = (Command *) malloc(sizeof(Command));
	if (my_commands == NULL)
	{
		return NULL;
	}

	my_commands->argc = 0;

	if (*p == '\\')
	{
		my_commands->type = META_COMMAND;

		j = 0;
		tok = strtok(++p, delim);

		while (tok != NULL)
		{
			if ((my_commands->argv[j] = strdup(tok)) == NULL)
				return NULL;

			my_commands->argc++;

			j++;
			tok = strtok(NULL, delim);
		}

		if (pg_strcasecmp(my_commands->argv[0], "setrandom") == 0)
		{
			if (my_commands->argc < 4)
			{
				fprintf(stderr, "%s: missing argument\n", my_commands->argv[0]);
				return NULL;
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
				return NULL;
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
				return NULL;
			}

			if (my_commands->argc >= 3)
			{
				if (pg_strcasecmp(my_commands->argv[2], "us") != 0 &&
					pg_strcasecmp(my_commands->argv[2], "ms") != 0 &&
					pg_strcasecmp(my_commands->argv[2], "s"))
				{
					fprintf(stderr, "%s: unknown time unit '%s' - must be us, ms or s\n",
							my_commands->argv[0], my_commands->argv[2]);
					return NULL;
				}
			}

			for (j = 3; j < my_commands->argc; j++)
				fprintf(stderr, "%s: extra argument \"%s\" ignored\n",
						my_commands->argv[0], my_commands->argv[j]);
		}
		else
		{
			fprintf(stderr, "Invalid command %s\n", my_commands->argv[0]);
			return NULL;
		}
	}
	else
	{
		my_commands->type = SQL_COMMAND;

		switch (querymode)
		{
			case QUERY_SIMPLE:
				if ((my_commands->argv[0] = strdup(p)) == NULL)
					return NULL;
				my_commands->argc++;
				break;
			case QUERY_EXTENDED:
			case QUERY_PREPARED:
				if (!parseQuery(my_commands, p))
					return NULL;
				break;
			default:
				return NULL;
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
	my_commands = (Command **) malloc(sizeof(Command *) * alloc_num);
	if (my_commands == NULL)
		return false;

	if (strcmp(filename, "-") == 0)
		fd = stdin;
	else if ((fd = fopen(filename, "r")) == NULL)
	{
		fprintf(stderr, "%s: %s\n", filename, strerror(errno));
		return false;
	}

	lineno = 0;

	while (fgets(buf, sizeof(buf), fd) != NULL)
	{
		Command    *commands;
		int			i;

		i = 0;
		while (isspace((unsigned char) buf[i]))
			i++;

		if (buf[i] != '\0' && strncmp(&buf[i], "--", 2) != 0)
		{
			commands = process_commands(&buf[i]);
			if (commands == NULL)
			{
				fclose(fd);
				return false;
			}
		}
		else
			continue;

		my_commands[lineno] = commands;
		lineno++;

		if (lineno >= alloc_num)
		{
			alloc_num += COMMANDS_ALLOC_NUM;
			my_commands = realloc(my_commands, sizeof(Command *) * alloc_num);
			if (my_commands == NULL)
			{
				fclose(fd);
				return false;
			}
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

	if (*tb == '\0')
		return NULL;

	alloc_num = COMMANDS_ALLOC_NUM;
	my_commands = (Command **) malloc(sizeof(Command *) * alloc_num);
	if (my_commands == NULL)
		return NULL;

	lineno = 0;

	for (;;)
	{
		char	   *p;
		Command    *commands;

		p = buf;
		while (*tb && *tb != '\n')
			*p++ = *tb++;

		if (*tb == '\0')
			break;

		if (*tb == '\n')
			tb++;

		*p = '\0';

		commands = process_commands(buf);
		if (commands == NULL)
		{
			return NULL;
		}

		my_commands[lineno] = commands;
		lineno++;

		if (lineno >= alloc_num)
		{
			alloc_num += COMMANDS_ALLOC_NUM;
			my_commands = realloc(my_commands, sizeof(Command *) * alloc_num);
			if (my_commands == NULL)
			{
				return NULL;
			}
		}
	}

	my_commands[lineno] = NULL;

	return my_commands;
}

/* print out results */
static void
printResults(
			 int ttype, CState *state,
			 struct timeval * start_time, struct timeval * end_time)
{
	double		t1,
				t2;
	int			i;
	int			normal_xacts = 0;
	char	   *s;

	for (i = 0; i < nclients; i++)
		normal_xacts += state[i].cnt;

	t1 = (end_time->tv_sec - start_time->tv_sec) * 1000000.0 + (end_time->tv_usec - start_time->tv_usec);
	t1 = normal_xacts * 1000000.0 / t1;

	t2 = (end_time->tv_sec - start_time->tv_sec - conn_total_time.tv_sec) * 1000000.0 +
		(end_time->tv_usec - start_time->tv_usec - conn_total_time.tv_usec);
	t2 = normal_xacts * 1000000.0 / t2;

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
	printf("tps = %f (including connections establishing)\n", t1);
	printf("tps = %f (excluding connections establishing)\n", t2);
}


int
main(int argc, char **argv)
{
	int			c;
	int			is_init_mode = 0;		/* initialize mode? */
	int			is_no_vacuum = 0;		/* no vacuum at all before testing? */
	int			do_vacuum_accounts = 0; /* do vacuum accounts before testing? */
	int			debug = 0;		/* debug flag */
	int			ttype = 0;		/* transaction type. 0: TPC-B, 1: SELECT only,
								 * 2: skip update of branches and tellers */
	char	   *filename = NULL;
	bool		scale_given = false;

	CState	   *state;			/* status of clients */

	struct timeval start_time;	/* start up time */
	struct timeval end_time;	/* end time */

	int			i;

	fd_set		input_mask;
	int			nsocks;			/* return from select(2) */
	int			maxsock;		/* max socket number to be waited */
	struct timeval now;
	struct timeval timeout;
	int			min_usec;

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

	state = (CState *) malloc(sizeof(CState));
	if (state == NULL)
	{
		fprintf(stderr, "Couldn't allocate memory for state\n");
		exit(1);
	}

	memset(state, 0, sizeof(*state));

	while ((c = getopt(argc, argv, "ih:nvp:dSNc:Cs:t:T:U:lf:D:F:M:")) != -1)
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
			case 'C':
				is_connect = 1;
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
					if (putVariable(&state[0], optarg, p) == false)
					{
						fprintf(stderr, "Couldn't allocate memory for variable\n");
						exit(1);
					}
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
					fprintf(stderr, "query mode (-M) should be specifiled before transaction scripts (-f)\n");
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

	remains = nclients;

	if (nclients > 1)
	{
		state = (CState *) realloc(state, sizeof(CState) * nclients);
		if (state == NULL)
		{
			fprintf(stderr, "Couldn't allocate memory for state\n");
			exit(1);
		}

		memset(state + 1, 0, sizeof(*state) * (nclients - 1));

		/* copy any -D switch values to all clients */
		for (i = 1; i < nclients; i++)
		{
			int			j;

			for (j = 0; j < state[0].nvariables; j++)
			{
				if (putVariable(&state[i], state[0].variables[j].name, state[0].variables[j].value) == false)
				{
					fprintf(stderr, "Couldn't allocate memory for variable\n");
					exit(1);
				}
			}
		}
	}

	if (use_log)
	{
		char		logpath[64];

		snprintf(logpath, 64, "pgbench_log.%d", (int) getpid());
		LOGFILE = fopen(logpath, "w");

		if (LOGFILE == NULL)
		{
			fprintf(stderr, "Couldn't open logfile \"%s\": %s", logpath, strerror(errno));
			exit(1);
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
			if (putVariable(&state[i], "scale", val) == false)
			{
				fprintf(stderr, "Couldn't allocate memory for variable\n");
				exit(1);
			}
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
	gettimeofday(&start_time, NULL);
	srandom((unsigned int) start_time.tv_usec);

	/* get start up time */
	gettimeofday(&start_time, NULL);

	/* set alarm if duration is specified. */
	if (duration > 0)
		setalarm(duration);

	if (is_connect == 0)
	{
		struct timeval t,
					now;

		/* make connections to the database */
		for (i = 0; i < nclients; i++)
		{
			state[i].id = i;
			if ((state[i].con = doConnect()) == NULL)
				exit(1);
		}
		/* time after connections set up */
		gettimeofday(&now, NULL);
		diffTime(&now, &start_time, &t);
		addTime(&conn_total_time, &t, &conn_total_time);
	}

	/* process bultin SQL scripts */
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

	/* send start up queries in async manner */
	for (i = 0; i < nclients; i++)
	{
		Command   **commands = sql_files[state[i].use_file];
		int			prev_ecnt = state[i].ecnt;

		state[i].use_file = getrand(0, num_files - 1);
		doCustom(state, i, debug);

		if (state[i].ecnt > prev_ecnt && commands[state[i].state]->type == META_COMMAND)
		{
			fprintf(stderr, "Client %d aborted in state %d. Execution meta-command failed.\n", i, state[i].state);
			remains--;			/* I've aborted */
			PQfinish(state[i].con);
			state[i].con = NULL;
		}
	}

	for (;;)
	{
		if (remains <= 0)
		{						/* all done ? */
			disconnect_all(state);
			/* get end time */
			gettimeofday(&end_time, NULL);
			printResults(ttype, state, &start_time, &end_time);
			if (LOGFILE)
				fclose(LOGFILE);
			exit(0);
		}

		FD_ZERO(&input_mask);

		maxsock = -1;
		min_usec = -1;
		for (i = 0; i < nclients; i++)
		{
			Command   **commands = sql_files[state[i].use_file];

			if (state[i].sleeping)
			{
				int			this_usec;
				int			sock = PQsocket(state[i].con);

				if (min_usec < 0)
				{
					gettimeofday(&now, NULL);
					min_usec = 0;
				}

				this_usec = (state[i].until.tv_sec - now.tv_sec) * 1000000 +
					state[i].until.tv_usec - now.tv_usec;

				if (this_usec > 0 && (min_usec == 0 || this_usec < min_usec))
					min_usec = this_usec;

				FD_SET		(sock, &input_mask);

				if (maxsock < sock)
					maxsock = sock;
			}
			else if (state[i].con && commands[state[i].state]->type != META_COMMAND)
			{
				int			sock = PQsocket(state[i].con);

				if (sock < 0)
				{
					disconnect_all(state);
					exit(1);
				}
				FD_SET		(sock, &input_mask);

				if (maxsock < sock)
					maxsock = sock;
			}
		}

		if (maxsock != -1)
		{
			if (min_usec >= 0)
			{
				timeout.tv_sec = min_usec / 1000000;
				timeout.tv_usec = min_usec % 1000000;

				nsocks = select(maxsock + 1, &input_mask, (fd_set *) NULL,
								(fd_set *) NULL, &timeout);
			}
			else
				nsocks = select(maxsock + 1, &input_mask, (fd_set *) NULL,
								(fd_set *) NULL, (struct timeval *) NULL);
			if (nsocks < 0)
			{
				if (errno == EINTR)
					continue;
				/* must be something wrong */
				disconnect_all(state);
				fprintf(stderr, "select failed: %s\n", strerror(errno));
				exit(1);
			}
#ifdef NOT_USED
			else if (nsocks == 0)
			{					/* timeout */
				fprintf(stderr, "select timeout\n");
				for (i = 0; i < nclients; i++)
				{
					fprintf(stderr, "client %d:state %d cnt %d ecnt %d listen %d\n",
							i, state[i].state, state[i].cnt, state[i].ecnt, state[i].listen);
				}
				exit(0);
			}
#endif
		}

		/* ok, backend returns reply */
		for (i = 0; i < nclients; i++)
		{
			Command   **commands = sql_files[state[i].use_file];
			int			prev_ecnt = state[i].ecnt;

			if (state[i].con && (FD_ISSET(PQsocket(state[i].con), &input_mask)
						  || commands[state[i].state]->type == META_COMMAND))
			{
				doCustom(state, i, debug);
			}

			if (state[i].ecnt > prev_ecnt && commands[state[i].state]->type == META_COMMAND)
			{
				fprintf(stderr, "Client %d aborted in state %d. Execution of meta-command failed.\n", i, state[i].state);
				remains--;		/* I've aborted */
				PQfinish(state[i].con);
				state[i].con = NULL;
			}
		}
	}
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

#endif   /* WIN32 */
