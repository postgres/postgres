/*
 * testlibpq3.c
 *		Test out-of-line parameters and binary I/O.
 *
 * Before running this, populate a database with the following commands
 * (provided in src/test/examples/testlibpq3.sql):
 *
 * CREATE TABLE test1 (i int4, t text, b bytea);
 *
 * INSERT INTO test1 values (1, 'joe''s place', '\\000\\001\\002\\003\\004');
 * INSERT INTO test1 values (2, 'ho there', '\\004\\003\\002\\001\\000');
 *
 * The expected output is:
 *
 * tuple 0: got
 *	i = (4 bytes) 1
 *	t = (11 bytes) 'joe's place'
 *	b = (5 bytes) \000\001\002\003\004
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "libpq-fe.h"

/* for ntohl/htonl */
#include <netinet/in.h>
#include <arpa/inet.h>


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
	const char *paramValues[1];
	int			i,
				j;
	int			i_fnum,
				t_fnum,
				b_fnum;

	/*
	 * If the user supplies a parameter on the command line, use it as the
	 * conninfo string; otherwise default to setting dbname=template1 and
	 * using environment variables or defaults for all other connection
	 * parameters.
	 */
	if (argc > 1)
		conninfo = argv[1];
	else
		conninfo = "dbname = template1";

	/* Make a connection to the database */
	conn = PQconnectdb(conninfo);

	/* Check to see that the backend connection was successfully made */
	if (PQstatus(conn) != CONNECTION_OK)
	{
		fprintf(stderr, "Connection to database '%s' failed.\n", PQdb(conn));
		fprintf(stderr, "%s", PQerrorMessage(conn));
		exit_nicely(conn);
	}

	/*
	 * The point of this program is to illustrate use of PQexecParams()
	 * with out-of-line parameters, as well as binary transmission of
	 * results.  By using out-of-line parameters we can avoid a lot of
	 * tedious mucking about with quoting and escaping.  Notice how we
	 * don't have to do anything special with the quote mark in the
	 * parameter value.
	 */

	/* Here is our out-of-line parameter value */
	paramValues[0] = "joe's place";

	res = PQexecParams(conn,
					   "SELECT * FROM test1 WHERE t = $1",
					   1,		/* one param */
					   NULL,	/* let the backend deduce param type */
					   paramValues,
					   NULL,	/* don't need param lengths since text */
					   NULL,	/* default to all text params */
					   1);		/* ask for binary results */

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, "SELECT failed: %s", PQerrorMessage(conn));
		PQclear(res);
		exit_nicely(conn);
	}

	/* Use PQfnumber to avoid assumptions about field order in result */
	i_fnum = PQfnumber(res, "i");
	t_fnum = PQfnumber(res, "t");
	b_fnum = PQfnumber(res, "b");

	for (i = 0; i < PQntuples(res); i++)
	{
		char	   *iptr;
		char	   *tptr;
		char	   *bptr;
		int			blen;
		int			ival;

		/* Get the field values (we ignore possibility they are null!) */
		iptr = PQgetvalue(res, i, i_fnum);
		tptr = PQgetvalue(res, i, t_fnum);
		bptr = PQgetvalue(res, i, b_fnum);

		/*
		 * The binary representation of INT4 is in network byte order,
		 * which we'd better coerce to the local byte order.
		 */
		ival = ntohl(*((uint32_t *) iptr));

		/*
		 * The binary representation of TEXT is, well, text, and since
		 * libpq was nice enough to append a zero byte to it, it'll work
		 * just fine as a C string.
		 *
		 * The binary representation of BYTEA is a bunch of bytes, which
		 * could include embedded nulls so we have to pay attention to
		 * field length.
		 */
		blen = PQgetlength(res, i, b_fnum);

		printf("tuple %d: got\n", i);
		printf(" i = (%d bytes) %d\n",
			   PQgetlength(res, i, i_fnum), ival);
		printf(" t = (%d bytes) '%s'\n",
			   PQgetlength(res, i, t_fnum), tptr);
		printf(" b = (%d bytes) ", blen);
		for (j = 0; j < blen; j++)
			printf("\\%03o", bptr[j]);
		printf("\n\n");
	}

	PQclear(res);

	/* close the connection to the database and cleanup */
	PQfinish(conn);

	return 0;
}
