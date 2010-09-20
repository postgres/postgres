/*
 * src/test/examples/testlibpq3.c
 *
 *
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
 * tuple 0: got
 *	i = (4 bytes) 2
 *	t = (8 bytes) 'ho there'
 *	b = (5 bytes) \004\003\002\001\000
 */

#ifdef WIN32
#include <windows.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
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

/*
 * This function prints a query result that is a binary-format fetch from
 * a table defined as in the comment above.  We split it out because the
 * main() function uses it twice.
 */
static void
show_binary_results(PGresult *res)
{
	int			i,
				j;
	int			i_fnum,
				t_fnum,
				b_fnum;

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
		 * The binary representation of INT4 is in network byte order, which
		 * we'd better coerce to the local byte order.
		 */
		ival = ntohl(*((uint32_t *) iptr));

		/*
		 * The binary representation of TEXT is, well, text, and since libpq
		 * was nice enough to append a zero byte to it, it'll work just fine
		 * as a C string.
		 *
		 * The binary representation of BYTEA is a bunch of bytes, which could
		 * include embedded nulls so we have to pay attention to field length.
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
}

int
main(int argc, char **argv)
{
	const char *conninfo;
	PGconn	   *conn;
	PGresult   *res;
	const char *paramValues[1];
	int			paramLengths[1];
	int			paramFormats[1];
	uint32_t	binaryIntVal;

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
		fprintf(stderr, "Connection to database failed: %s",
				PQerrorMessage(conn));
		exit_nicely(conn);
	}

	/*
	 * The point of this program is to illustrate use of PQexecParams() with
	 * out-of-line parameters, as well as binary transmission of data.
	 *
	 * This first example transmits the parameters as text, but receives the
	 * results in binary format.  By using out-of-line parameters we can avoid
	 * a lot of tedious mucking about with quoting and escaping, even though
	 * the data is text.  Notice how we don't have to do anything special with
	 * the quote mark in the parameter value.
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

	show_binary_results(res);

	PQclear(res);

	/*
	 * In this second example we transmit an integer parameter in binary form,
	 * and again retrieve the results in binary form.
	 *
	 * Although we tell PQexecParams we are letting the backend deduce
	 * parameter type, we really force the decision by casting the parameter
	 * symbol in the query text.  This is a good safety measure when sending
	 * binary parameters.
	 */

	/* Convert integer value "2" to network byte order */
	binaryIntVal = htonl((uint32_t) 2);

	/* Set up parameter arrays for PQexecParams */
	paramValues[0] = (char *) &binaryIntVal;
	paramLengths[0] = sizeof(binaryIntVal);
	paramFormats[0] = 1;		/* binary */

	res = PQexecParams(conn,
					   "SELECT * FROM test1 WHERE i = $1::int4",
					   1,		/* one param */
					   NULL,	/* let the backend deduce param type */
					   paramValues,
					   paramLengths,
					   paramFormats,
					   1);		/* ask for binary results */

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, "SELECT failed: %s", PQerrorMessage(conn));
		PQclear(res);
		exit_nicely(conn);
	}

	show_binary_results(res);

	PQclear(res);

	/* close the connection to the database and cleanup */
	PQfinish(conn);

	return 0;
}
