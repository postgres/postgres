/*
 * src/test/examples/testlibpq2.c
 *
 *
 * testlibpq2.c
 *		Test of the asynchronous notification interface
 *
 * Start this program, then from psql in another window do
 *	 NOTIFY TBL2;
 * Repeat four times to get this program to exit.
 *
 * Or, if you want to get fancy, try this:
 * populate a database with the following commands
 * (provided in src/test/examples/testlibpq2.sql):
 *
 *	 CREATE SCHEMA TESTLIBPQ2;
 *	 SET search_path = TESTLIBPQ2;
 *	 CREATE TABLE TBL1 (i int4);
 *	 CREATE TABLE TBL2 (i int4);
 *	 CREATE RULE r1 AS ON INSERT TO TBL1 DO
 *	   (INSERT INTO TBL2 VALUES (new.i); NOTIFY TBL2);
 *
 * Start this program, then from psql do this four times:
 *
 *	 INSERT INTO TESTLIBPQ2.TBL1 VALUES (10);
 */

#ifdef WIN32
#include <windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/types.h>
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#include "libpq-fe.h"

static void
exit_nicely(PGconn *conn)
{
	PQfinish(conn);
	exit(1);
}

int
main(int argc, char **argv)
{
	const char *conninfo;
	PGconn	   *conn;
	PGresult   *res;
	PGnotify   *notify;
	int			nnotifies;

	/*
	 * If the user supplies a parameter on the command line, use it as the
	 * conninfo string; otherwise default to setting dbname=postgres and using
	 * environment variables or defaults for all other connection parameters.
	 */
	if (argc > 1)
		conninfo = argv[1];
	else
		conninfo = "dbname = postgres";

	/* Make a connection to the database */
	conn = PQconnectdb(conninfo);

	/* Check to see that the backend connection was successfully made */
	if (PQstatus(conn) != CONNECTION_OK)
	{
		fprintf(stderr, "%s", PQerrorMessage(conn));
		exit_nicely(conn);
	}

	/* Set always-secure search path, so malicious users can't take control. */
	res = PQexec(conn,
				 "SELECT pg_catalog.set_config('search_path', '', false)");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, "SET failed: %s", PQerrorMessage(conn));
		PQclear(res);
		exit_nicely(conn);
	}

	/*
	 * Should PQclear PGresult whenever it is no longer needed to avoid memory
	 * leaks
	 */
	PQclear(res);

	/*
	 * Issue LISTEN command to enable notifications from the rule's NOTIFY.
	 */
	res = PQexec(conn, "LISTEN TBL2");
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "LISTEN command failed: %s", PQerrorMessage(conn));
		PQclear(res);
		exit_nicely(conn);
	}
	PQclear(res);

	/* Quit after four notifies are received. */
	nnotifies = 0;
	while (nnotifies < 4)
	{
		/*
		 * Sleep until something happens on the connection.  We use select(2)
		 * to wait for input, but you could also use poll() or similar
		 * facilities.
		 */
		int			sock;
		fd_set		input_mask;

		sock = PQsocket(conn);

		if (sock < 0)
			break;				/* shouldn't happen */

		FD_ZERO(&input_mask);
		FD_SET(sock, &input_mask);

		if (select(sock + 1, &input_mask, NULL, NULL, NULL) < 0)
		{
			fprintf(stderr, "select() failed: %s\n", strerror(errno));
			exit_nicely(conn);
		}

		/* Now check for input */
		PQconsumeInput(conn);
		while ((notify = PQnotifies(conn)) != NULL)
		{
			fprintf(stderr,
					"ASYNC NOTIFY of '%s' received from backend PID %d\n",
					notify->relname, notify->be_pid);
			PQfreemem(notify);
			nnotifies++;
			PQconsumeInput(conn);
		}
	}

	fprintf(stderr, "Done.\n");

	/* close the connection to the database and cleanup */
	PQfinish(conn);

	return 0;
}
