/*
 * testlibpq4.c
 *		this test programs shows to use LIBPQ to make multiple backend
 * connections
 *
 *
 */
#include <stdio.h>
#include "libpq-fe.h"

void
exit_nicely(PGconn * conn1, PGconn * conn2)
{
	if (conn1)
		PQfinish(conn1);
	if (conn2)
		PQfinish(conn2);
	exit(1);
}

void
check_conn(PGconn * conn)
{
	/* check to see that the backend connection was successfully made */
	if (PQstatus(conn) == CONNECTION_BAD)
	{
		fprintf(stderr, "Connection to database '%s' failed.\n", dbName);
		fprintf(stderr, "%s", PQerrorMessage(conn));
		exit(1);
	}
}

main()
{
	char	   *pghost,
			   *pgport,
			   *pgoptions,
			   *pgtty;
	char	   *dbName1,
				dbName2;
	char	   *tblName;
	int			nFields;
	int			i,
				j;

	PGconn	   *conn1,
				conn2;
	PGresult   *res1,
				res2;

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
	 * defaults by looking up environment variables or, failing that,
	 * using hardwired constants
	 */
	pghost = NULL;				/* host name of the backend server */
	pgport = NULL;				/* port of the backend server */
	pgoptions = NULL;			/* special options to start up the backend
								 * server */
	pgtty = NULL;				/* debugging tty for the backend server */

	/* make a connection to the database */
	conn1 = PQsetdb(pghost, pgport, pgoptions, pgtty, dbName1);
	check_conn(conn1);

	conn2 = PQsetdb(pghost, pgport, pgoptions, pgtty, dbName2);
	check_conn(conn2);

	/* start a transaction block */
	res1 = PQexec(conn1, "BEGIN");
	if (PQresultStatus(res1) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "BEGIN command failed\n");
		PQclear(res1);
		exit_nicely(conn1, conn2);
	}

	/*
	 * should PQclear PGresult whenever it is no longer needed to avoid
	 * memory leaks
	 */
	PQclear(res1);

	/*
	 * fetch instances from the pg_database, the system catalog of
	 * databases
	 */
	res = PQexec(conn, "DECLARE myportal CURSOR FOR select * from pg_database");
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "DECLARE CURSOR command failed\n");
		PQclear(res);
		exit_nicely(conn);
	}
	PQclear(res);

	res = PQexec(conn, "FETCH ALL in myportal");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, "FETCH ALL command didn't return tuples properly\n");
		PQclear(res);
		exit_nicely(conn);
	}

	/* first, print out the attribute names */
	nFields = PQnfields(res);
	for (i = 0; i < nFields; i++)
	{
		printf("%-15s", PQfname(res, i));
	}
	printf("\n\n");

	/* next, print out the instances */
	for (i = 0; i < PQntuples(res); i++)
	{
		for (j = 0; j < nFields; j++)
		{
			printf("%-15s", PQgetvalue(res, i, j));
		}
		printf("\n");
	}

	PQclear(res);

	/* close the portal */
	res = PQexec(conn, "CLOSE myportal");
	PQclear(res);

	/* end the transaction */
	res = PQexec(conn, "END");
	PQclear(res);

	/* close the connection to the database and cleanup */
	PQfinish(conn);

/*	 fclose(debug); */
}
