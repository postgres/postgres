/*
 * testlibpq3.c
 *		Test the C version of LIBPQ, the POSTGRES frontend library.
 *	 tests the binary cursor interface
 *
 *
 *
 populate a database by doing the following:

CREATE TABLE test1 (i int4, d float4, p polygon);

INSERT INTO test1 values (1, 3.567, '(3.0, 4.0, 1.0, 2.0)'::polygon);

INSERT INTO test1 values (2, 89.05, '(4.0, 3.0, 2.0, 1.0)'::polygon);

 the expected output is:

tuple 0: got
 i = (4 bytes) 1,
 d = (4 bytes) 3.567000,
 p = (4 bytes) 2 points			boundbox = (hi=3.000000/4.000000, lo = 1.000000,2.000000)
tuple 1: got
 i = (4 bytes) 2,
 d = (4 bytes) 89.050003,
 p = (4 bytes) 2 points			boundbox = (hi=4.000000/3.000000, lo = 2.000000,1.000000)

 *
 */
#include "postgres.h"			/* -> "c.h" -> int16, in access/attnum.h */
#include "libpq-fe.h"
#include "utils/geo_decls.h"	/* for the POLYGON type */

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
	int			i;
	int			i_fnum,
				d_fnum,
				p_fnum;

	PGconn	   *conn;
	PGresult   *res;

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

	/* start a transaction block */
	res = PQexec(conn, "BEGIN");
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "BEGIN command failed\n");
		PQclear(res);
		exit_nicely(conn);
	}

	/*
	 * should PQclear PGresult whenever it is no longer needed to avoid
	 * memory leaks
	 */
	PQclear(res);

	/*
	 * fetch instances from the pg_database, the system catalog of
	 * databases
	 */
	res = PQexec(conn, "DECLARE mycursor BINARY CURSOR FOR select * from test1");
	if (res == NULL ||
		PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "DECLARE CURSOR command failed\n");
		if (res)
			PQclear(res);
		exit_nicely(conn);
	}
	PQclear(res);

	res = PQexec(conn, "FETCH ALL in mycursor");
	if (res == NULL ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, "FETCH ALL command didn't return tuples properly\n");
		if (res)
			PQclear(res);
		exit_nicely(conn);
	}

	i_fnum = PQfnumber(res, "i");
	d_fnum = PQfnumber(res, "d");
	p_fnum = PQfnumber(res, "p");

	for (i = 0; i < 3; i++)
	{
		printf("type[%d] = %d, size[%d] = %d\n",
			   i, PQftype(res, i),
			   i, PQfsize(res, i));
	}
	for (i = 0; i < PQntuples(res); i++)
	{
		int		   *ival;
		float	   *dval;
		int			plen;
		POLYGON    *pval;

		/* we hard-wire this to the 3 fields we know about */
		ival = (int *) PQgetvalue(res, i, i_fnum);
		dval = (float *) PQgetvalue(res, i, d_fnum);
		plen = PQgetlength(res, i, p_fnum);

		/*
		 * plen doesn't include the length field so need to increment by
		 * VARHDSZ
		 */
		pval = (POLYGON *) malloc(plen + VARHDRSZ);
		pval->size = plen;
		memmove((char *) &pval->npts, PQgetvalue(res, i, p_fnum), plen);
		printf("tuple %d: got\n", i);
		printf(" i = (%d bytes) %d,\n",
			   PQgetlength(res, i, i_fnum), *ival);
		printf(" d = (%d bytes) %f,\n",
			   PQgetlength(res, i, d_fnum), *dval);
		printf(" p = (%d bytes) %d points \tboundbox = (hi=%f/%f, lo = %f,%f)\n",
			   PQgetlength(res, i, d_fnum),
			   pval->npts,
			   pval->boundbox.high.x,
			   pval->boundbox.high.y,
			   pval->boundbox.low.x,
			   pval->boundbox.low.y);
	}

	PQclear(res);

	/* close the portal */
	res = PQexec(conn, "CLOSE mycursor");
	PQclear(res);

	/* end the transaction */
	res = PQexec(conn, "END");
	PQclear(res);

	/* close the connection to the database and cleanup */
	PQfinish(conn);
	return 0;					/* Though PQfinish(conn1) has called
								 * exit(1) */
}
