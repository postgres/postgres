/*
 * $Header: /cvsroot/pgsql/contrib/pgbench/pgbench.c,v 1.4 2000/05/29 05:44:29 tgl Exp $
 *
 * pgbench: a simple TPC-B like benchmark program for PostgreSQL
 * written by Tatsuo Ishii
 *
 * Copyright (c) 2000  Tatsuo Ishii
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

#include "config.h"

#include <stdio.h>
#include "postgres.h"
#include "libpq-fe.h"

#include <errno.h>

#ifdef WIN32
#include "win32.h"
#else
#include <sys/time.h>
#if HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

/* for getrlimit */
#include <sys/resource.h>

#endif	 /* WIN32 */

/********************************************************************
 * some configurable parameters */

#define MAXCLIENTS 1024			/* max number of clients allowed */

int			nclients = 1;		/* default number of simulated clients */
int			nxacts = 10;		/* default number of transactions per
								 * clients */

/*
 * scaling factor. for example, tps = 10 will make 1000000 tuples of
 * accounts table.
 */
int			tps = 1;

/*
 * end of configurable parameters
 *********************************************************************/

#define nbranches	1
#define ntellers	10
#define naccounts	100000

int			remains;			/* number of remained clients */

typedef struct
{
	PGconn	   *con;			/* connection handle to DB */
	int			state;			/* state No. */
	int			cnt;			/* xacts count */
	int			ecnt;			/* error count */
	int			listen;			/* none 0 indicates that an async query
								 * has been sent */
	int			aid;			/* account id for this transaction */
	int			bid;			/* branch id for this transaction */
	int			tid;			/* teller id for this transaction */
	int			delta;
	int			abalance;
}			CState;

static void
usage()
{
	fprintf(stderr, "usage: pgbench [-h hostname][-p port][-c nclients][-t ntransactions][-s scaling_factor][-n][-v][-S][-d][dbname]\n");
	fprintf(stderr, "(initialize mode): pgbench -i [-h hostname][-p port][-s scaling_factor][-d][dbname]\n");
}

/* random number generator */
static int
getrand(int min, int max)
{
	return (min + (int) (max * 1.0 * rand() / (RAND_MAX + 1.0)));
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
	return (0);
}

/* process a transaction */
static void
doOne(CState * state, int n, int debug)
{
	char		sql[256];
	PGresult   *res;
	CState	   *st = &state[n];

	if (st->listen)
	{							/* are we receiver? */
		if (debug)
			fprintf(stderr, "client %d receiving\n", n);
		while (PQisBusy(st->con) == TRUE)
		{
			if (!PQconsumeInput(st->con))
			{					/* there's something wrong */
				fprintf(stderr, "Client %d aborted in state %d. Probably the backend died while processing.\n", n, st->state);
				remains--;		/* I've aborted */
				PQfinish(st->con);
				st->con = NULL;
				return;
			}
		}

		switch (st->state)
		{
			case 0:				/* response to "begin" */
				res = PQgetResult(st->con);
				if (check(state, res, n, PGRES_COMMAND_OK))
					return;
				PQclear(res);
				discard_response(st);
				break;
			case 1:				/* response to "update accounts..." */
				res = PQgetResult(st->con);
				if (check(state, res, n, PGRES_COMMAND_OK))
					return;
				PQclear(res);
				discard_response(st);
				break;
			case 2:				/* response to "select abalance ..." */
				res = PQgetResult(st->con);
				if (check(state, res, n, PGRES_TUPLES_OK))
					return;
				PQclear(res);
				discard_response(st);
				break;
			case 3:				/* response to "update tellers ..." */
				res = PQgetResult(st->con);
				if (check(state, res, n, PGRES_COMMAND_OK))
					return;
				PQclear(res);
				discard_response(st);
				break;
			case 4:				/* response to "update branches ..." */
				res = PQgetResult(st->con);
				if (check(state, res, n, PGRES_COMMAND_OK))
					return;
				PQclear(res);
				discard_response(st);
				break;
			case 5:				/* response to "insert into history ..." */
				res = PQgetResult(st->con);
				if (check(state, res, n, PGRES_COMMAND_OK))
					return;
				PQclear(res);
				discard_response(st);
				break;
			case 6:				/* response to "end" */
				res = PQgetResult(st->con);
				if (check(state, res, n, PGRES_COMMAND_OK))
					return;
				PQclear(res);
				discard_response(st);

				if (++st->cnt >= nxacts)
				{
					remains--;	/* I've done */
					PQfinish(st->con);
					st->con = NULL;
					return;
				}
				break;
		}

		/* increment state counter */
		st->state++;
		if (st->state > 6)
			st->state = 0;
	}

	switch (st->state)
	{
		case 0:			/* about to start */
			strcpy(sql, "begin");
			st->aid = getrand(1, naccounts * tps);
			st->bid = getrand(1, nbranches * tps);
			st->tid = getrand(1, ntellers * tps);
			st->delta = getrand(1, 1000);
			break;
		case 1:
			sprintf(sql, "update accounts set abalance = abalance + %d where aid = %d\n", st->delta, st->aid);
			break;
		case 2:
			sprintf(sql, "select abalance from accounts where aid = %d", st->aid);
			break;
		case 3:
			sprintf(sql, "update tellers set tbalance = tbalance + %d where tid = %d\n",
					st->delta, st->tid);
			break;
		case 4:
			sprintf(sql, "update branches set bbalance = bbalance + %d where bid = %d", st->delta, st->bid);
			break;
		case 5:
			sprintf(sql, "insert into history(tid,bid,aid,delta,time) values(%d,%d,%d,%d,'now')",
					st->tid, st->bid, st->aid, st->delta);
			break;
		case 6:
			strcpy(sql, "end");
			break;
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
		st->listen++;			/* flags that should be listned */
	}
}

/* process a select only transaction */
static void
doSelectOnly(CState * state, int n, int debug)
{
	char		sql[256];
	PGresult   *res;
	CState	   *st = &state[n];

	if (st->listen)
	{							/* are we receiver? */
		if (debug)
			fprintf(stderr, "client %d receiving\n", n);
		while (PQisBusy(st->con) == TRUE)
		{
			if (!PQconsumeInput(st->con))
			{					/* there's something wrong */
				fprintf(stderr, "Client %d aborted in state %d. Probably the backend died while processing.\n", n, st->state);
				remains--;		/* I've aborted */
				PQfinish(st->con);
				st->con = NULL;
				return;
			}
		}

		switch (st->state)
		{
			case 0:				/* response to "select abalance ..." */
				res = PQgetResult(st->con);
				if (check(state, res, n, PGRES_TUPLES_OK))
					return;
				PQclear(res);
				discard_response(st);

				if (++st->cnt >= nxacts)
				{
					remains--;	/* I've done */
					PQfinish(st->con);
					st->con = NULL;
					return;
				}
				break;
		}

		/* increment state counter */
		st->state++;
		if (st->state > 0)
			st->state = 0;
	}

	switch (st->state)
	{
		case 0:
			st->aid = getrand(1, naccounts * tps);
			sprintf(sql, "select abalance from accounts where aid = %d", st->aid);
			break;
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
		st->listen++;			/* flags that should be listned */
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
init(char *pghost, char *pgport, char *dbName)
{
	PGconn	   *con;
	PGresult   *res;
	static char *DDLs[] = {
		"drop table branches",
		"create table branches(bid int, primary key(bid),bbalance int,filler char(88))",
		"drop table tellers",
		"create table tellers(tid int, primary key(tid),bid int,tbalance int,filler char(84))",
		"drop table accounts",
		"create table accounts(aid int,primary key(aid),bid int,abalance int,filler char(84))",
		"drop table history",
	"create table history(tid int,bid int,aid int,delta int,time timestamp,filler char(22))"};
	char		sql[256];

	int			i;

	con = PQsetdb(pghost, pgport, NULL, NULL, dbName);
	if (PQstatus(con) == CONNECTION_BAD)
	{
		fprintf(stderr, "Connection to database '%s' on %s failed.\n", dbName, pghost);
		fprintf(stderr, "%s", PQerrorMessage(con));
		exit(1);
	}

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

	for (i = 0; i < nbranches * tps; i++)
	{
		sprintf(sql, "insert into branches(bid,bbalance) values(%d,0)", i + 1);
		res = PQexec(con, sql);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			fprintf(stderr, "%s", PQerrorMessage(con));
			exit(1);
		}
		PQclear(res);
	}

	for (i = 0; i < ntellers * tps; i++)
	{
		sprintf(sql, "insert into tellers(tid,bid,tbalance) values (%d,%d,0)"
				,i + 1, i / ntellers + 1);
		res = PQexec(con, sql);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			fprintf(stderr, "%s", PQerrorMessage(con));
			exit(1);
		}
		PQclear(res);
	}

	res = PQexec(con, "copy accounts from stdin");
	if (PQresultStatus(res) != PGRES_COPY_IN)
	{
		fprintf(stderr, "%s", PQerrorMessage(con));
		exit(1);
	}
	PQclear(res);

	fprintf(stderr, "creating tables...\n");
	for (i = 0; i < naccounts * tps; i++)
	{
		int			j = i + 1;

		sprintf(sql, "%d\t%d\t%d\t\n", i + 1, (i + 1) / naccounts, 0);
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

	res = PQexec(con, "end");
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "%s", PQerrorMessage(con));
		exit(1);
	}

	/* vacuum */
	fprintf(stderr, "vacuum...");
	res = PQexec(con, "vacuum analyze");
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "%s", PQerrorMessage(con));
		exit(1);
	}
	fprintf(stderr, "done.\n");

	PQfinish(con);
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

	for (i = 0; i < nclients; i++)
		normal_xacts += state[i].cnt;

	t1 = (tv3->tv_sec - tv1->tv_sec) * 1000000.0 + (tv3->tv_usec - tv1->tv_usec);
	t1 = normal_xacts * 1000000.0 / t1;

	t2 = (tv3->tv_sec - tv2->tv_sec) * 1000000.0 + (tv3->tv_usec - tv2->tv_usec);
	t2 = normal_xacts * 1000000.0 / t2;

	printf("transaction type: %s\n", ttype == 0 ? "TPC-B (sort of)" : "SELECT only");
	printf("scaling factor: %d\n", tps);
	printf("number of clients: %d\n", nclients);
	printf("number of transactions per client: %d\n", nxacts);
	printf("number of transactions actually processed: %d/%d\n", normal_xacts, nxacts * nclients);
	printf("tps = %f(including connections establishing)\n", t1);
	printf("tps = %f(excluding connections establishing)\n", t2);
}

int
main(int argc, char **argv)
{
	extern char *optarg;
	extern int	optind,
				opterr,
				optopt;
	int			c;
	char	   *pghost = "";
	char	   *pgport = "";
	char	   *dbName;
	int			is_init_mode = 0;		/* initialize mode? */
	int			is_no_vacuum = 0;		/* no vacuum at all before
										 * testing? */
	int			is_full_vacuum = 0;		/* do full vacuum before testing? */
	int			debug = 0;		/* debug flag */
	int			ttype = 0;		/* transaction type. 0: TPC-B, 1: SELECT
								 * only */

	static CState state[MAXCLIENTS];	/* clients status */

	struct timeval tv1;			/* start up time */
	struct timeval tv2;			/* after establishing all connections to
								 * the backend */
	struct timeval tv3;			/* end time */

	int			i;

	fd_set		input_mask;
	int			nsocks;			/* return from select(2) */
	int			maxsock;		/* max socket number to be waited */

#ifndef __CYGWIN32__
	struct rlimit rlim;

#endif

	PGconn	   *con;
	PGresult   *res;

	while ((c = getopt(argc, argv, "ih:nvp:dc:t:s:S")) != EOF)
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
			case 'c':
				nclients = atoi(optarg);
				if (nclients <= 0 || nclients > MAXCLIENTS)
				{
					fprintf(stderr, "wrong number of clients: %d\n", nclients);
					exit(1);
				}
#ifndef __CYGWIN32__
#ifdef RLIMIT_NOFILE			/* most platform uses RLIMIT_NOFILE */
				if (getrlimit(RLIMIT_NOFILE, &rlim) == -1)
				{
#else							/* but BSD doesn't ... */
				if (getrlimit(RLIMIT_OFILE, &rlim) == -1)
				{
#endif	 /* HAVE_RLIMIT_NOFILE */
					fprintf(stderr, "getrlimit failed. reason: %s\n", strerror(errno));
					exit(1);
				}
				if (rlim.rlim_cur <= (nclients + 2))
				{
					fprintf(stderr, "You need at least %d open files resource but you are only allowed to use %ld.\n", nclients + 2, (long) rlim.rlim_cur);
					fprintf(stderr, "Use limit/ulimt to increase the limit before using pgbench.\n");
					exit(1);
				}
#endif	 /* #ifndef __CYGWIN32__ */
				break;
			case 's':
				tps = atoi(optarg);
				if (tps <= 0)
				{
					fprintf(stderr, "wrong scaling factor: %d\n", tps);
					exit(1);
				}
				break;
			case 't':
				nxacts = atoi(optarg);
				if (nxacts <= 0)
				{
					fprintf(stderr, "wrong number of transactions: %d\n", nxacts);
					exit(1);
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
		dbName = getenv("USER");
		if (dbName == NULL)
			dbName = "";
	}

	if (is_init_mode)
	{
		init(pghost, pgport, dbName);
		exit(0);
	}

	remains = nclients;

	if (debug)
	{
		printf("pghost: %s pgport: %s nclients: %d nxacts: %d dbName: %s\n",
			   pghost, pgport, nclients, nxacts, dbName);
	}

	/* opening connection... */
	con = PQsetdb(pghost, pgport, NULL, NULL, dbName);
	if (PQstatus(con) == CONNECTION_BAD)
	{
		fprintf(stderr, "Connection to database '%s' failed.\n", dbName);
		fprintf(stderr, "%s", PQerrorMessage(con));
		exit(1);
	}

	/*
	 * get the scaling factor that should be same as count(*) from
	 * branches...
	 */
	res = PQexec(con, "select count(*) from branches");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, "%s", PQerrorMessage(con));
		exit(1);
	}
	tps = atoi(PQgetvalue(res, 0, 0));
	if (tps < 0)
	{
		fprintf(stderr, "count(*) from branches invalid (%d)\n", tps);
		exit(1);
	}
	PQclear(res);

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
	gettimeofday(&tv1, 0);
	srand((uint) tv1.tv_usec);

	/* get start up time */
	gettimeofday(&tv1, 0);

	/* make connections to the database */
	for (i = 0; i < nclients; i++)
	{
		state[i].con = PQsetdb(pghost, pgport, NULL, NULL, dbName);
		if (PQstatus(state[i].con) == CONNECTION_BAD)
		{
			fprintf(stderr, "Connection to database '%s' failed.\n", dbName);
			fprintf(stderr, "%s", PQerrorMessage(state[i].con));
			exit(1);
		}
	}

	/* time after connections set up */
	gettimeofday(&tv2, 0);

	/* send start up quries in async manner */
	for (i = 0; i < nclients; i++)
	{
		if (ttype == 0)
			doOne(state, i, debug);
		else if (ttype == 1)
			doSelectOnly(state, i, debug);
	}

	for (;;)
	{
		if (remains <= 0)
		{						/* all done ? */
			disconnect_all(state);
			/* get end time */
			gettimeofday(&tv3, 0);
			printResults(ttype, state, &tv1, &tv2, &tv3);
			exit(0);
		}

		FD_ZERO(&input_mask);

		maxsock = 0;
		for (i = 0; i < nclients; i++)
		{
			if (state[i].con)
			{
				int			sock = PQsocket(state[i].con);

				if (sock < 0)
				{
					fprintf(stderr, "Client %d: PQsock failed\n", i);
					disconnect_all(state);
					exit(1);
				}
				FD_SET(sock, &input_mask);
				if (maxsock < sock)
					maxsock = sock;
			}
		}

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
		{						/* timeout */
			fprintf(stderr, "select timeout\n");
			for (i = 0; i < nclients; i++)
			{
				fprintf(stderr, "client %d:state %d cnt %d ecnt %d listen %d\n",
						i, state[i].state, state[i].cnt, state[i].ecnt, state[i].listen);
			}
			exit(0);
		}

		/* ok, backend returns reply */
		for (i = 0; i < nclients; i++)
		{
			if (state[i].con && FD_ISSET(PQsocket(state[i].con), &input_mask))
			{
				if (ttype == 0)
					doOne(state, i, debug);
				else if (ttype == 1)
					doSelectOnly(state, i, debug);
			}
		}
	}
}
