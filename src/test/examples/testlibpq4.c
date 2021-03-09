/*
 * src/test/examples/testlibpq4.c
 *
 *
 * testlibpq4.c
 *		this test program shows to use LIBPQ to make multiple backend
 * connections
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include "libpq-fe.h"

static void
exit_nicely(PGconn *conn1, PGconn *conn2)
{
	if (conn1)
		PQfinish(conn1);
	if (conn2)
		PQfinish(conn2);
	exit(1);
}

static void
check_prepare_conn(PGconn *conn, const char *dbName)
{
	PGresult   *res;

	/* check to see that the backend connection was successfully made */
	if (PQstatus(conn) != CONNECTION_OK)
	{
		fprintf(stderr, "%s", PQerrorMessage(conn));
		exit(1);
	}

	/* Set always-secure search path, so malicious users can't take control. */
	res = PQexec(conn,
				 "SELECT pg_catalog.set_config('search_path', '', false)");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, "SET failed: %s", PQerrorMessage(conn));
		PQclear(res);
		exit(1);
	}
	PQclear(res);
}

int
main(int argc, char **argv)
{
	char	   *pghost,
			   *pgport,
			   *pgoptions;
	char	   *dbName1,
			   *dbName2;
	char	   *tblName;
	int			nFields;
	int			i,
				j;

	PGconn	   *conn1,
			   *conn2;

	/*
	 * PGresult   *res1, *res2;
	 */
	PGresult   *res1;

	if (argc != 4)
	{
		fprintf(stderr, "usage: %s tableName dbName1 dbName2\n", argv[0]);
		fprintf(stderr, "      compares two tables in two databases\n");
		exit(1);
	}
	tblName = argv[1];
	dbName1 = argv[2];
	dbName2 = argv[3];


	/*
	 * begin, by setting the parameters for a backend connection if the
	 * parameters are null, then the system will try to use reasonable
	 * defaults by looking up environment variables or, failing that, using
	 * hardwired constants
	 */
	pghost = NULL;				/* host name of the backend */
	pgport = NULL;				/* port of the backend */
	pgoptions = NULL;			/* special options to start up the backend
								 * server */

	/* make a connection to the database */
	conn1 = PQsetdb(pghost, pgport, pgoptions, NULL, dbName1);
	check_prepare_conn(conn1, dbName1);

	conn2 = PQsetdb(pghost, pgport, pgoptions, NULL, dbName2);
	check_prepare_conn(conn2, dbName2);

	/* start a transaction block */
	res1 = PQexec(conn1, "BEGIN");
	if (PQresultStatus(res1) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "BEGIN command failed\n");
		PQclear(res1);
		exit_nicely(conn1, conn2);
	}

	/*
	 * make sure to PQclear() a PGresult whenever it is no longer needed to
	 * avoid memory leaks
	 */
	PQclear(res1);

	/*
	 * fetch instances from the pg_database, the system catalog of databases
	 */
	res1 = PQexec(conn1, "DECLARE myportal CURSOR FOR select * from pg_database");
	if (PQresultStatus(res1) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "DECLARE CURSOR command failed\n");
		PQclear(res1);
		exit_nicely(conn1, conn2);
	}
	PQclear(res1);

	res1 = PQexec(conn1, "FETCH ALL in myportal");
	if (PQresultStatus(res1) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, "FETCH ALL command didn't return tuples properly\n");
		PQclear(res1);
		exit_nicely(conn1, conn2);
	}

	/* first, print out the attribute names */
	nFields = PQnfields(res1);
	for (i = 0; i < nFields; i++)
		printf("%-15s", PQfname(res1, i));
	printf("\n\n");

	/* next, print out the instances */
	for (i = 0; i < PQntuples(res1); i++)
	{
		for (j = 0; j < nFields; j++)
			printf("%-15s", PQgetvalue(res1, i, j));
		printf("\n");
	}

	PQclear(res1);

	/* close the portal */
	res1 = PQexec(conn1, "CLOSE myportal");
	PQclear(res1);

	/* end the transaction */
	res1 = PQexec(conn1, "END");
	PQclear(res1);

	/* close the connections to the database and cleanup */
	PQfinish(conn1);
	PQfinish(conn2);

/*	 fclose(debug); */
	return 0;
}
