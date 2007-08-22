/*
 * $PostgreSQL: pgsql/contrib/pgbench/pgbench.c,v 1.58.2.2 2007/08/22 23:03:33 tgl Exp $
 *
 * pgbench: a simple benchmark program for PostgreSQL
 * written by Tatsuo Ishii
 *
 * Copyright (c) 2000-2007	Tatsuo Ishii
 *
 * Permission to use, copy, modify, and distribute this software and
 * its documentation for any purpose and without fee is hereby
 * granted, provided that the above copyright notice appear in all
 * copies and that both that copyright notice and this permission
 * notice appear in supporting documentation, and that the name of the
 * author not be used in advertising or publicity pertaining to
 * distribution of the software without specific, written prior
 * permission. The author makes no representations about the
 * suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 */
#include "postgres_fe.h"

#include "libpq-fe.h"

#include <ctype.h>

#ifdef WIN32
#include "win32.h"
#else
#include <sys/time.h>
#include <unistd.h>

#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>		/* for getrlimit */
#endif
#endif   /* ! WIN32 */

extern char *optarg;
extern int	optind;

#ifdef WIN32
#undef select
#endif


/********************************************************************
 * some configurable parameters */

#define MAXCLIENTS 1024			/* max number of clients allowed */

int			nclients = 1;		/* default number of simulated clients */
int			nxacts = 10;		/* default number of transactions per clients */

/*
 * scaling factor. for example, scale = 10 will make 1000000 tuples of
 * accounts table.
 */
int			scale = 1;

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
char	   *pwd = NULL;
char	   *dbName;

/* variable definitions */
typedef struct
{
	char	   *name;			/* variable name */
	char	   *value;			/* its value */
}	Variable;

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
	Variable   *variables;		/* array of variable definitions */
	int			nvariables;
	struct timeval txn_begin;	/* used for measuring latencies */
	int			use_file;		/* index in sql_files for this client */
}	CState;

/*
 * queries read from files
 */
#define SQL_COMMAND		1
#define META_COMMAND	2
#define MAX_ARGS		10

typedef struct
{
	int			type;			/* command type (SQL_COMMAND or META_COMMAND) */
	int			argc;			/* number of commands */
	char	   *argv[MAX_ARGS]; /* command list */
}	Command;

#define MAX_FILES		128		/* max number of SQL script files allowed */

Command   **sql_files[MAX_FILES];		/* SQL script files */
int			num_files;			/* its number */

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
	"UPDATE accounts SET abalance = abalance + :delta WHERE aid = :aid;\n"
	"SELECT abalance FROM accounts WHERE aid = :aid;\n"
	"UPDATE tellers SET tbalance = tbalance + :delta WHERE tid = :tid;\n"
	"UPDATE branches SET bbalance = bbalance + :delta WHERE bid = :bid;\n"
	"INSERT INTO history (tid, bid, aid, delta, mtime) VALUES (:tid, :bid, :aid, :delta, CURRENT_TIMESTAMP);\n"
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
	"UPDATE accounts SET abalance = abalance + :delta WHERE aid = :aid;\n"
	"SELECT abalance FROM accounts WHERE aid = :aid;\n"
	"INSERT INTO history (tid, bid, aid, delta, mtime) VALUES (:tid, :bid, :aid, :delta, CURRENT_TIMESTAMP);\n"
	"END;\n"
};

/* -S case */
static char *select_only = {
	"\\set naccounts 100000 * :scale\n"
	"\\setrandom aid 1 :naccounts\n"
	"SELECT abalance FROM accounts WHERE aid = :aid;\n"
};

static void
usage(void)
{
	fprintf(stderr, "usage: pgbench [-h hostname][-p port][-c nclients][-t ntransactions][-s scaling_factor][-D varname=value][-n][-C][-v][-S][-N][-f filename][-l][-U login][-P password][-d][dbname]\n");
	fprintf(stderr, "(initialize mode): pgbench -i [-h hostname][-p port][-s scaling_factor][-U login][-P password][-d][dbname]\n");
}

/* random number generator */
static int
getrand(int min, int max)
{
	return min + (int) (((max - min) * (double) random()) / MAX_RANDOM_VALUE + 0.5);
}

/* set up a connection to the backend */
static PGconn *
doConnect(void)
{
	PGconn	   *con;
	PGresult   *res;

	con = PQsetdbLogin(pghost, pgport, pgoptions, pgtty, dbName,
					   login, pwd);
	if (con == NULL)
	{
		fprintf(stderr, "Connection to database '%s' failed.\n", dbName);
		fprintf(stderr, "Memory allocatin problem?\n");
		return (NULL);
	}

	if (PQstatus(con) == CONNECTION_BAD)
	{
		fprintf(stderr, "Connection to database '%s' failed.\n", dbName);

		if (PQerrorMessage(con))
			fprintf(stderr, "%s", PQerrorMessage(con));
		else
			fprintf(stderr, "No explanation from the backend\n");

		return (NULL);
	}

	res = PQexec(con, "SET search_path = public");
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "%s", PQerrorMessage(con));
		exit(1);
	}
	PQclear(res);

	return (con);
}

/* throw away response from backend */
static void
discard_response(CState * state)
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
check(CState * state, PGresult *res, int n, int good)
{
	CState	   *st = &state[n];

	if (res && PQresultStatus(res) != good)
	{
		fprintf(stderr, "Client %d aborted in state %d: %s", n, st->state, PQerrorMessage(st->con));
		remains--;				/* I've aborted */
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
getVariable(CState * st, char *name)
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
putVariable(CState * st, char *name, char *value)
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
assignVariables(CState * st, char *sql)
{
	int			i,
				j;
	char	   *p,
			   *name,
			   *val;
	void	   *tmp;

	i = 0;
	while ((p = strchr(&sql[i], ':')) != NULL)
	{
		i = j = p - sql;
		do
		{
			i++;
		} while (isalnum((unsigned char) sql[i]) || sql[i] == '_');
		if (i == j + 1)
			continue;

		name = malloc(i - j);
		if (name == NULL)
			return NULL;
		memcpy(name, &sql[j + 1], i - (j + 1));
		name[i - (j + 1)] = '\0';
		val = getVariable(st, name);
		free(name);
		if (val == NULL)
			continue;

		if (strlen(val) > i - j)
		{
			tmp = realloc(sql, strlen(sql) - (i - j) + strlen(val) + 1);
			if (tmp == NULL)
			{
				free(sql);
				return NULL;
			}
			sql = tmp;
		}

		if (strlen(val) != i - j)
			memmove(&sql[j + strlen(val)], &sql[i], strlen(&sql[i]) + 1);

		strncpy(&sql[j], val, strlen(val));

		if (strlen(val) < i - j)
		{
			tmp = realloc(sql, strlen(sql) + 1);
			if (tmp == NULL)
			{
				free(sql);
				return NULL;
			}
			sql = tmp;
		}

		i = j + strlen(val);
	}

	return sql;
}

static void
doCustom(CState * state, int n, int debug)
{
	PGresult   *res;
	CState	   *st = &state[n];
	Command   **commands;

top:
	commands = sql_files[st->use_file];

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

			fprintf(LOGFILE, "%d %d %.0f\n", st->id, st->cnt, diff);
		}

		if (commands[st->state]->type == SQL_COMMAND)
		{
			res = PQgetResult(st->con);
			if (pg_strncasecmp(commands[st->state]->argv[0], "select", 6) != 0)
			{
				if (check(state, res, n, PGRES_COMMAND_OK))
					return;
			}
			else
			{
				if (check(state, res, n, PGRES_TUPLES_OK))
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

			if (++st->cnt >= nxacts)
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
		if ((st->con = doConnect()) == NULL)
		{
			fprintf(stderr, "Client %d aborted in establishing connection.\n",
					n);
			remains--;			/* I've aborted */
			PQfinish(st->con);
			st->con = NULL;
			return;
		}
	}

	if (use_log && st->state == 0)
		gettimeofday(&(st->txn_begin), NULL);

	if (commands[st->state]->type == SQL_COMMAND)
	{
		char	   *sql;

		if ((sql = strdup(commands[st->state]->argv[0])) == NULL
			|| (sql = assignVariables(st, sql)) == NULL)
		{
			fprintf(stderr, "out of memory\n");
			st->ecnt++;
			return;
		}

		if (debug)
			fprintf(stderr, "client %d sending %s\n", n, sql);
		if (PQsendQuery(st->con, sql) == 0)
		{
			if (debug)
				fprintf(stderr, "PQsendQuery(%s)failed\n", sql);
			st->ecnt++;
		}
		else
		{
			st->listen = 1;		/* flags that should be listened */
		}
		free(sql);
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

		goto top;
	}
}

/* discard connections */
static void
disconnect_all(CState * state)
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
	PGconn	   *con;
	PGresult   *res;
	static char *DDLs[] = {
		"drop table branches",
		"create table branches(bid int not null,bbalance int,filler char(88))",
		"drop table tellers",
		"create table tellers(tid int not null,bid int,tbalance int,filler char(84))",
		"drop table accounts",
		"create table accounts(aid int not null,bid int,abalance int,filler char(84))",
		"drop table history",
	"create table history(tid int,bid int,aid int,delta int,mtime timestamp,filler char(22))"};
	static char *DDLAFTERs[] = {
		"alter table branches add primary key (bid)",
		"alter table tellers add primary key (tid)",
	"alter table accounts add primary key (aid)"};


	char		sql[256];

	int			i;

	if ((con = doConnect()) == NULL)
		exit(1);

	for (i = 0; i < (sizeof(DDLs) / sizeof(char *)); i++)
	{
		res = PQexec(con, DDLs[i]);
		if (strncmp(DDLs[i], "drop", 4) && PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			fprintf(stderr, "%s", PQerrorMessage(con));
			exit(1);
		}
		PQclear(res);
	}

	res = PQexec(con, "begin");
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "%s", PQerrorMessage(con));
		exit(1);
	}
	PQclear(res);

	for (i = 0; i < nbranches * scale; i++)
	{
		snprintf(sql, 256, "insert into branches(bid,bbalance) values(%d,0)", i + 1);
		res = PQexec(con, sql);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			fprintf(stderr, "%s", PQerrorMessage(con));
			exit(1);
		}
		PQclear(res);
	}

	for (i = 0; i < ntellers * scale; i++)
	{
		snprintf(sql, 256, "insert into tellers(tid,bid,tbalance) values (%d,%d,0)"
				 ,i + 1, i / ntellers + 1);
		res = PQexec(con, sql);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			fprintf(stderr, "%s", PQerrorMessage(con));
			exit(1);
		}
		PQclear(res);
	}

	res = PQexec(con, "end");
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "%s", PQerrorMessage(con));
		exit(1);
	}
	PQclear(res);

	/*
	 * occupy accounts table with some data
	 */
	fprintf(stderr, "creating tables...\n");
	for (i = 0; i < naccounts * scale; i++)
	{
		int			j = i + 1;

		if (j % 10000 == 1)
		{
			res = PQexec(con, "copy accounts from stdin");
			if (PQresultStatus(res) != PGRES_COPY_IN)
			{
				fprintf(stderr, "%s", PQerrorMessage(con));
				exit(1);
			}
			PQclear(res);
		}

		snprintf(sql, 256, "%d\t%d\t%d\t\n", j, i / naccounts + 1, 0);
		if (PQputline(con, sql))
		{
			fprintf(stderr, "PQputline failed\n");
			exit(1);
		}

		if (j % 10000 == 0)
		{
			/*
			 * every 10000 tuples, we commit the copy command. this should
			 * avoid generating too much WAL logs
			 */
			fprintf(stderr, "%d tuples done.\n", j);
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

#ifdef NOT_USED

			/*
			 * do a checkpoint to purge the old WAL logs
			 */
			res = PQexec(con, "checkpoint");
			if (PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				fprintf(stderr, "%s", PQerrorMessage(con));
				exit(1);
			}
			PQclear(res);
#endif   /* NOT_USED */
		}
	}
	fprintf(stderr, "set primary key...\n");
	for (i = 0; i < (sizeof(DDLAFTERs) / sizeof(char *)); i++)
	{
		res = PQexec(con, DDLAFTERs[i]);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			fprintf(stderr, "%s", PQerrorMessage(con));
			exit(1);
		}
		PQclear(res);
	}

	/* vacuum */
	fprintf(stderr, "vacuum...");
	res = PQexec(con, "vacuum analyze");
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "%s", PQerrorMessage(con));
		exit(1);
	}
	PQclear(res);
	fprintf(stderr, "done.\n");

	PQfinish(con);
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
		else
		{
			fprintf(stderr, "invalid command %s\n", my_commands->argv[0]);
			return NULL;
		}
	}
	else
	{
		my_commands->type = SQL_COMMAND;

		if ((my_commands->argv[0] = strdup(p)) == NULL)
			return NULL;

		my_commands->argc++;
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
			 int ttype, CState * state,
			 struct timeval * tv1, struct timeval * tv2,
			 struct timeval * tv3)
{
	double		t1,
				t2;
	int			i;
	int			normal_xacts = 0;
	char	   *s;

	for (i = 0; i < nclients; i++)
		normal_xacts += state[i].cnt;

	t1 = (tv3->tv_sec - tv1->tv_sec) * 1000000.0 + (tv3->tv_usec - tv1->tv_usec);
	t1 = normal_xacts * 1000000.0 / t1;

	t2 = (tv3->tv_sec - tv2->tv_sec) * 1000000.0 + (tv3->tv_usec - tv2->tv_usec);
	t2 = normal_xacts * 1000000.0 / t2;

	if (ttype == 0)
		s = "TPC-B (sort of)";
	else if (ttype == 2)
		s = "Update only accounts";
	else if (ttype == 1)
		s = "SELECT only";
	else
		s = "Custom query";

	printf("transaction type: %s\n", s);
	printf("scaling factor: %d\n", scale);
	printf("number of clients: %d\n", nclients);
	printf("number of transactions per client: %d\n", nxacts);
	printf("number of transactions actually processed: %d/%d\n", normal_xacts, nxacts * nclients);
	printf("tps = %f (including connections establishing)\n", t1);
	printf("tps = %f (excluding connections establishing)\n", t2);
}


int
main(int argc, char **argv)
{
	int			c;
	int			is_init_mode = 0;		/* initialize mode? */
	int			is_no_vacuum = 0;		/* no vacuum at all before testing? */
	int			is_full_vacuum = 0;		/* do full vacuum before testing? */
	int			debug = 0;		/* debug flag */
	int			ttype = 0;		/* transaction type. 0: TPC-B, 1: SELECT only,
								 * 2: skip update of branches and tellers */
	char	   *filename = NULL;

	CState	   *state;			/* status of clients */

	struct timeval tv1;			/* start up time */
	struct timeval tv2;			/* after establishing all connections to the
								 * backend */
	struct timeval tv3;			/* end time */

	int			i;

	fd_set		input_mask;
	int			nsocks;			/* return from select(2) */
	int			maxsock;		/* max socket number to be waited */

#ifdef HAVE_GETRLIMIT
	struct rlimit rlim;
#endif

	PGconn	   *con;
	PGresult   *res;
	char	   *env;

	char		val[64];

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

	while ((c = getopt(argc, argv, "ih:nvp:dc:t:s:U:P:CNSlf:D:")) != -1)
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
				is_full_vacuum++;
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
#endif /* HAVE_GETRLIMIT */
				break;
			case 'C':
				is_connect = 1;
				break;
			case 's':
				scale = atoi(optarg);
				if (scale <= 0)
				{
					fprintf(stderr, "invalid scaling factor: %d\n", scale);
					exit(1);
				}
				break;
			case 't':
				nxacts = atoi(optarg);
				if (nxacts <= 0)
				{
					fprintf(stderr, "invalid number of transactions: %d\n", nxacts);
					exit(1);
				}
				break;
			case 'U':
				login = optarg;
				break;
			case 'P':
				pwd = optarg;
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
			default:
				usage();
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

	remains = nclients;

	if (getVariable(&state[0], "scale") == NULL)
	{
		snprintf(val, sizeof(val), "%d", scale);
		if (putVariable(&state[0], "scale", val) == false)
		{
			fprintf(stderr, "Couldn't allocate memory for variable\n");
			exit(1);
		}
	}

	if (nclients > 1)
	{
		state = (CState *) realloc(state, sizeof(CState) * nclients);
		if (state == NULL)
		{
			fprintf(stderr, "Couldn't allocate memory for state\n");
			exit(1);
		}

		memset(state + 1, 0, sizeof(*state) * (nclients - 1));

		snprintf(val, sizeof(val), "%d", scale);

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

			if (putVariable(&state[i], "scale", val) == false)
			{
				fprintf(stderr, "Couldn't allocate memory for variable\n");
				exit(1);
			}
		}
	}

	if (use_log)
	{
		char		logpath[64];

		snprintf(logpath, 64, "pgbench_log.%d", getpid());
		LOGFILE = fopen(logpath, "w");

		if (LOGFILE == NULL)
		{
			fprintf(stderr, "Couldn't open logfile \"%s\": %s", logpath, strerror(errno));
			exit(1);
		}
	}

	if (debug)
	{
		printf("pghost: %s pgport: %s nclients: %d nxacts: %d dbName: %s\n",
			   pghost, pgport, nclients, nxacts, dbName);
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
		 * branches if this is not a custom query
		 */
		res = PQexec(con, "select count(*) from branches");
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			fprintf(stderr, "%s", PQerrorMessage(con));
			exit(1);
		}
		scale = atoi(PQgetvalue(res, 0, 0));
		if (scale < 0)
		{
			fprintf(stderr, "count(*) from branches invalid (%d)\n", scale);
			exit(1);
		}
		PQclear(res);

		snprintf(val, sizeof(val), "%d", scale);
		if (putVariable(&state[0], "scale", val) == false)
		{
			fprintf(stderr, "Couldn't allocate memory for variable\n");
			exit(1);
		}

		if (nclients > 1)
		{
			for (i = 1; i < nclients; i++)
			{
				if (putVariable(&state[i], "scale", val) == false)
				{
					fprintf(stderr, "Couldn't allocate memory for variable\n");
					exit(1);
				}
			}
		}
	}

	if (!is_no_vacuum)
	{
		fprintf(stderr, "starting vacuum...");
		res = PQexec(con, "vacuum branches");
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			fprintf(stderr, "%s", PQerrorMessage(con));
			exit(1);
		}
		PQclear(res);

		res = PQexec(con, "vacuum tellers");
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			fprintf(stderr, "%s", PQerrorMessage(con));
			exit(1);
		}
		PQclear(res);

		res = PQexec(con, "delete from history");
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			fprintf(stderr, "%s", PQerrorMessage(con));
			exit(1);
		}
		PQclear(res);
		res = PQexec(con, "vacuum history");
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			fprintf(stderr, "%s", PQerrorMessage(con));
			exit(1);
		}
		PQclear(res);

		fprintf(stderr, "end.\n");

		if (is_full_vacuum)
		{
			fprintf(stderr, "starting full vacuum...");
			res = PQexec(con, "vacuum analyze accounts");
			if (PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				fprintf(stderr, "%s", PQerrorMessage(con));
				exit(1);
			}
			PQclear(res);
			fprintf(stderr, "end.\n");
		}
	}
	PQfinish(con);

	/* set random seed */
	gettimeofday(&tv1, NULL);
	srandom((unsigned int) tv1.tv_usec);

	/* get start up time */
	gettimeofday(&tv1, NULL);

	if (is_connect == 0)
	{
		/* make connections to the database */
		for (i = 0; i < nclients; i++)
		{
			state[i].id = i;
			if ((state[i].con = doConnect()) == NULL)
				exit(1);
		}
	}

	/* time after connections set up */
	gettimeofday(&tv2, NULL);

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
			gettimeofday(&tv3, NULL);
			printResults(ttype, state, &tv1, &tv2, &tv3);
			if (LOGFILE)
				fclose(LOGFILE);
			exit(0);
		}

		FD_ZERO(&input_mask);

		maxsock = -1;
		for (i = 0; i < nclients; i++)
		{
			Command   **commands = sql_files[state[i].use_file];

			if (state[i].con && commands[state[i].state]->type != META_COMMAND)
			{
				int			sock = PQsocket(state[i].con);

				if (sock < 0)
				{
					disconnect_all(state);
					exit(1);
				}
				FD_SET(sock, &input_mask);
				if (maxsock < sock)
					maxsock = sock;
			}
		}

		if (maxsock != -1)
		{
			if ((nsocks = select(maxsock + 1, &input_mask, (fd_set *) NULL,
							  (fd_set *) NULL, (struct timeval *) NULL)) < 0)
			{
				if (errno == EINTR)
					continue;
				/* must be something wrong */
				disconnect_all(state);
				fprintf(stderr, "select failed: %s\n", strerror(errno));
				exit(1);
			}
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
				fprintf(stderr, "Client %d aborted in state %d. Execution meta-command failed.\n", i, state[i].state);
				remains--;		/* I've aborted */
				PQfinish(state[i].con);
				state[i].con = NULL;
			}
		}
	}
}
