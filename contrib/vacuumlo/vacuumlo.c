/*-------------------------------------------------------------------------
 *
 * vacuumlo.c
 *	  This removes orphaned large objects from a database.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/contrib/vacuumlo/vacuumlo.c,v 1.8 2001/01/24 19:42:45 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "libpq-fe.h"
#include "libpq/libpq-fs.h"

#define atooid(x)  ((Oid) strtoul((x), NULL, 10))

#define BUFSIZE			1024

int			vacuumlo(char *, int);


/*
 * This vacuums LOs of one database. It returns 0 on success, -1 on failure.
 */
int
vacuumlo(char *database, int verbose)
{
	PGconn	   *conn;
	PGresult   *res,
			   *res2;
	char		buf[BUFSIZE];
	int			matched;
	int			deleted;
	int			i;

	conn = PQsetdb(NULL, NULL, NULL, NULL, database);

	/* check to see that the backend connection was successfully made */
	if (PQstatus(conn) == CONNECTION_BAD)
	{
		fprintf(stderr, "Connection to database '%s' failed:\n", database);
		fprintf(stderr, "%s", PQerrorMessage(conn));
		PQfinish(conn);
		return -1;
	}

	if (verbose)
		fprintf(stdout, "Connected to %s\n", database);

	/*
	 * First we create and populate the LO temp table
	 */
	buf[0] = '\0';
	strcat(buf, "SELECT DISTINCT loid AS lo ");
	strcat(buf, "INTO TEMP TABLE vacuum_l ");
	strcat(buf, "FROM pg_largeobject ");
	res = PQexec(conn, buf);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "Failed to create temp table:\n");
		fprintf(stderr, "%s", PQerrorMessage(conn));
		PQclear(res);
		PQfinish(conn);
		return -1;
	}
	PQclear(res);
	/*
	 * Vacuum the temp table so that planner will generate decent plans
	 * for the DELETEs below.
	 */
	buf[0] = '\0';
	strcat(buf, "VACUUM ANALYZE vacuum_l ");
	res = PQexec(conn, buf);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "Failed to vacuum temp table:\n");
		fprintf(stderr, "%s", PQerrorMessage(conn));
		PQclear(res);
		PQfinish(conn);
		return -1;
	}
	PQclear(res);

	/*
	 * Now find any candidate tables who have columns of type oid.
	 *
	 * NOTE: the temp table formed above is ignored, because its real
	 * table name will be pg_something.  Also, pg_largeobject will be
	 * ignored.  If either of these were scanned, obviously we'd end up
	 * with nothing to delete...
	 *
	 * NOTE: the system oid column is ignored, as it has attnum < 1.
	 * This shouldn't matter for correctness, but it saves time.
	 */
	buf[0] = '\0';
	strcat(buf, "SELECT c.relname, a.attname ");
	strcat(buf, "FROM pg_class c, pg_attribute a, pg_type t ");
	strcat(buf, "WHERE a.attnum > 0 ");
	strcat(buf, "      AND a.attrelid = c.oid ");
	strcat(buf, "      AND a.atttypid = t.oid ");
	strcat(buf, "      AND t.typname = 'oid' ");
	strcat(buf, "      AND c.relkind = 'r'");
	strcat(buf, "      AND c.relname NOT LIKE 'pg_%'");
	res = PQexec(conn, buf);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, "Failed to find OID columns:\n");
		fprintf(stderr, "%s", PQerrorMessage(conn));
		PQclear(res);
		PQfinish(conn);
		return -1;
	}

	for (i = 0; i < PQntuples(res); i++)
	{
		char	   *table,
				   *field;

		table = PQgetvalue(res, i, 0);
		field = PQgetvalue(res, i, 1);

		if (verbose)
			fprintf(stdout, "Checking %s in %s\n", field, table);

		/*
		 * We use a DELETE with implicit join for efficiency.  This
		 * is a Postgres-ism and not portable to other DBMSs, but
		 * then this whole program is a Postgres-ism.
		 */
		sprintf(buf, "DELETE FROM vacuum_l WHERE lo = \"%s\".\"%s\" ",
				table, field);
		res2 = PQexec(conn, buf);
		if (PQresultStatus(res2) != PGRES_COMMAND_OK)
		{
			fprintf(stderr, "Failed to check %s in table %s:\n",
					field, table);
			fprintf(stderr, "%s", PQerrorMessage(conn));
			PQclear(res2);
			PQclear(res);
			PQfinish(conn);
			return -1;
		}
		PQclear(res2);
	}
	PQclear(res);

	/*
	 * Run the actual deletes in a single transaction.  Note that this
	 * would be a bad idea in pre-7.1 Postgres releases (since rolling
	 * back a table delete used to cause problems), but it should
	 * be safe now.
	 */
	res = PQexec(conn, "begin");
	PQclear(res);

	/*
	 * Finally, those entries remaining in vacuum_l are orphans.
	 */
	buf[0] = '\0';
	strcat(buf, "SELECT lo ");
	strcat(buf, "FROM vacuum_l");
	res = PQexec(conn, buf);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, "Failed to read temp table:\n");
		fprintf(stderr, "%s", PQerrorMessage(conn));
		PQclear(res);
		PQfinish(conn);
		return -1;
	}

	matched = PQntuples(res);
	deleted = 0;
	for (i = 0; i < matched; i++)
	{
		Oid			lo = atooid(PQgetvalue(res, i, 0));

		if (verbose)
		{
			fprintf(stdout, "\rRemoving lo %6u   ", lo);
			fflush(stdout);
		}

		if (lo_unlink(conn, lo) < 0)
		{
			fprintf(stderr, "\nFailed to remove lo %u: ", lo);
			fprintf(stderr, "%s", PQerrorMessage(conn));
		}
		else
			deleted++;
	}
	PQclear(res);

	/*
	 * That's all folks!
	 */
	res = PQexec(conn, "end");
	PQclear(res);

	PQfinish(conn);

	if (verbose)
		fprintf(stdout, "\rRemoved %d large objects from %s.\n",
				deleted, database);

	return 0;
}

int
main(int argc, char **argv)
{
	int			verbose = 0;
	int			arg;
	int			rc = 0;

	if (argc < 2)
	{
		fprintf(stderr, "Usage: %s [-v] database_name [db2 ... dbn]\n",
				argv[0]);
		exit(1);
	}

	for (arg = 1; arg < argc; arg++)
	{
		if (strcmp("-v", argv[arg]) == 0)
			verbose = !verbose;
		else
			rc += (vacuumlo(argv[arg], verbose) != 0);
	}

	return rc;
}
