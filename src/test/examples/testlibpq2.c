/*
 * testlibpq2.c
 *		Test of the asynchronous notification interface
 *
   populate a database with the following:

CREATE TABLE TBL1 (i int4);

CREATE TABLE TBL2 (i int4);

CREATE RULE r1 AS ON INSERT TO TBL1 DO [INSERT INTO TBL2 values (new.i); NOTIFY TBL2];

 * Then start up this program
 * After the program has begun, do

INSERT INTO TBL1 values (10);

 *
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include "libpq-fe.h"

static void
exit_nicely(PGconn *conn)
{
	PQfinish(conn);
	exit(1);
}

int
main()
{
	char	   *pghost,
			   *pgport,
			   *pgoptions,
			   *pgtty;
	char	   *dbName;

	/*
	 * int			nFields; int		  i, j;
	 */

	PGconn	   *conn;
	PGresult   *res;
	PGnotify   *notify;

	/*
	 * begin, by setting the parameters for a backend connection if the
	 * parameters are null, then the system will try to use reasonable
	 * defaults by looking up environment variables or, failing that,
	 * using hardwired constants
	 */
	pghost = NULL;				/* host name of the backend server */
	pgport = NULL;				/* port of the backend server */
	pgoptions = NULL;			/* special options to start up the backend
								 * server */
	pgtty = NULL;				/* debugging tty for the backend server */
	dbName = getenv("USER");	/* change this to the name of your test
								 * database */

	/* make a connection to the database */
	conn = PQsetdb(pghost, pgport, pgoptions, pgtty, dbName);

	/* check to see that the backend connection was successfully made */
	if (PQstatus(conn) == CONNECTION_BAD)
	{
		fprintf(stderr, "Connection to database '%s' failed.\n", dbName);
		fprintf(stderr, "%s", PQerrorMessage(conn));
		exit_nicely(conn);
	}

	res = PQexec(conn, "LISTEN TBL2");
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "LISTEN command failed\n");
		PQclear(res);
		exit_nicely(conn);
	}

	/*
	 * should PQclear PGresult whenever it is no longer needed to avoid
	 * memory leaks
	 */
	PQclear(res);

	while (1)
	{
		/* async notification only come back as a result of a query */
		/* we can send empty queries */
		res = PQexec(conn, " ");
/*		printf("res->status = %s\n", PQresStatus(PQresultStatus(res))); */
		/* check for asynchronous returns */
		notify = PQnotifies(conn);
		if (notify)
		{
			fprintf(stderr,
				 "ASYNC NOTIFY of '%s' from backend pid '%d' received\n",
					notify->relname, notify->be_pid);
			free(notify);
			break;
		}
		PQclear(res);
	}

	/* close the connection to the database and cleanup */
	PQfinish(conn);
	return 0;					/* Though PQfinish(conn1) has called
								 * exit(1) */
}
