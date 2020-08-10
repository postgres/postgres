/*
 * findoidjoins.c
 *
 * Copyright (c) 2002-2020, PostgreSQL Global Development Group
 *
 * src/tools/findoidjoins/findoidjoins.c
 */
#include "postgres_fe.h"

#include "access/transam.h"
#include "catalog/pg_class_d.h"

#include "common/connect.h"
#include "libpq-fe.h"
#include "pqexpbuffer.h"


int
main(int argc, char **argv)
{
	PGconn	   *conn;
	PQExpBufferData sql;
	PGresult   *res;
	PGresult   *pkrel_res;
	PGresult   *fkrel_res;
	char	   *fk_relname;
	char	   *fk_nspname;
	char	   *fk_attname;
	char	   *pk_relname;
	char	   *pk_nspname;
	int			fk,
				pk;				/* loop counters */

	if (argc != 2)
	{
		fprintf(stderr, "Usage:  %s database\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	initPQExpBuffer(&sql);

	appendPQExpBuffer(&sql, "dbname=%s", argv[1]);

	conn = PQconnectdb(sql.data);
	if (PQstatus(conn) == CONNECTION_BAD)
	{
		fprintf(stderr, "connection error:  %s\n", PQerrorMessage(conn));
		exit(EXIT_FAILURE);
	}

	res = PQexec(conn, ALWAYS_SECURE_SEARCH_PATH_SQL);
	if (!res || PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, "sql error:  %s\n", PQerrorMessage(conn));
		exit(EXIT_FAILURE);
	}
	PQclear(res);

	/* Get a list of system relations that have OIDs */

	printfPQExpBuffer(&sql,
					  "SELECT c.relname, (SELECT nspname FROM "
					  "pg_catalog.pg_namespace n WHERE n.oid = c.relnamespace) AS nspname "
					  "FROM pg_catalog.pg_class c "
					  "WHERE c.relkind = " CppAsString2(RELKIND_RELATION)
					  " AND c.oid < '%u'"
					  " AND EXISTS(SELECT * FROM pg_attribute a"
					  "            WHERE a.attrelid = c.oid AND a.attname = 'oid' "
					  "                  AND a.atttypid = 'oid'::regtype)"
					  "ORDER BY nspname, c.relname",
					  FirstNormalObjectId
		);

	res = PQexec(conn, sql.data);
	if (!res || PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, "sql error:  %s\n", PQerrorMessage(conn));
		exit(EXIT_FAILURE);
	}
	pkrel_res = res;

	/* Get a list of system columns of OID type (or any OID-alias type) */

	printfPQExpBuffer(&sql,
					  "SELECT c.relname, "
					  "(SELECT nspname FROM pg_catalog.pg_namespace n WHERE n.oid = c.relnamespace) AS nspname, "
					  "a.attname "
					  "FROM pg_catalog.pg_class c, pg_catalog.pg_attribute a "
					  "WHERE a.attnum > 0"
					  " AND a.attname != 'oid'"
					  " AND c.relkind = " CppAsString2(RELKIND_RELATION)
					  " AND c.oid < '%u'"
					  " AND a.attrelid = c.oid"
					  " AND a.atttypid IN ('pg_catalog.oid'::regtype, "
					  " 'pg_catalog.regclass'::regtype, "
					  " 'pg_catalog.regoper'::regtype, "
					  " 'pg_catalog.regoperator'::regtype, "
					  " 'pg_catalog.regproc'::regtype, "
					  " 'pg_catalog.regprocedure'::regtype, "
					  " 'pg_catalog.regtype'::regtype, "
					  " 'pg_catalog.regconfig'::regtype, "
					  " 'pg_catalog.regdictionary'::regtype) "
					  "ORDER BY nspname, c.relname, a.attnum",
					  FirstNormalObjectId
		);

	res = PQexec(conn, sql.data);
	if (!res || PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, "sql error:  %s\n", PQerrorMessage(conn));
		exit(EXIT_FAILURE);
	}
	fkrel_res = res;

	/*
	 * For each column and each relation-having-OIDs, look to see if the
	 * column contains any values matching entries in the relation.
	 */

	for (fk = 0; fk < PQntuples(fkrel_res); fk++)
	{
		fk_relname = PQgetvalue(fkrel_res, fk, 0);
		fk_nspname = PQgetvalue(fkrel_res, fk, 1);
		fk_attname = PQgetvalue(fkrel_res, fk, 2);

		for (pk = 0; pk < PQntuples(pkrel_res); pk++)
		{
			pk_relname = PQgetvalue(pkrel_res, pk, 0);
			pk_nspname = PQgetvalue(pkrel_res, pk, 1);

			printfPQExpBuffer(&sql,
							  "SELECT	1 "
							  "FROM \"%s\".\"%s\" t1, "
							  "\"%s\".\"%s\" t2 "
							  "WHERE t1.\"%s\"::pg_catalog.oid = t2.oid "
							  "LIMIT 1",
							  fk_nspname, fk_relname,
							  pk_nspname, pk_relname,
							  fk_attname);

			res = PQexec(conn, sql.data);
			if (!res || PQresultStatus(res) != PGRES_TUPLES_OK)
			{
				fprintf(stderr, "sql error:  %s\n", PQerrorMessage(conn));
				exit(EXIT_FAILURE);
			}

			if (PQntuples(res) != 0)
				printf("Join %s.%s.%s => %s.%s.oid\n",
					   fk_nspname, fk_relname, fk_attname,
					   pk_nspname, pk_relname);

			PQclear(res);
		}
	}

	PQclear(fkrel_res);

	/* Now, do the same for referencing columns that are arrays */

	/* Get a list of columns of OID-array type (or any OID-alias type) */

	printfPQExpBuffer(&sql, "%s",
					  "SELECT c.relname, "
					  "(SELECT nspname FROM pg_catalog.pg_namespace n WHERE n.oid = c.relnamespace) AS nspname, "
					  "a.attname "
					  "FROM pg_catalog.pg_class c, pg_catalog.pg_attribute a "
					  "WHERE a.attnum > 0"
					  " AND c.relkind = " CppAsString2(RELKIND_RELATION)
					  " AND a.attrelid = c.oid"
					  " AND a.atttypid IN ('pg_catalog.oid[]'::regtype, "
					  " 'pg_catalog.oidvector'::regtype, "
					  " 'pg_catalog.regclass[]'::regtype, "
					  " 'pg_catalog.regoper[]'::regtype, "
					  " 'pg_catalog.regoperator[]'::regtype, "
					  " 'pg_catalog.regproc[]'::regtype, "
					  " 'pg_catalog.regprocedure[]'::regtype, "
					  " 'pg_catalog.regtype[]'::regtype, "
					  " 'pg_catalog.regconfig[]'::regtype, "
					  " 'pg_catalog.regdictionary[]'::regtype) "
					  "ORDER BY nspname, c.relname, a.attnum"
		);

	res = PQexec(conn, sql.data);
	if (!res || PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, "sql error:  %s\n", PQerrorMessage(conn));
		exit(EXIT_FAILURE);
	}
	fkrel_res = res;

	/*
	 * For each column and each relation-having-OIDs, look to see if the
	 * column contains any values matching entries in the relation.
	 */

	for (fk = 0; fk < PQntuples(fkrel_res); fk++)
	{
		fk_relname = PQgetvalue(fkrel_res, fk, 0);
		fk_nspname = PQgetvalue(fkrel_res, fk, 1);
		fk_attname = PQgetvalue(fkrel_res, fk, 2);

		for (pk = 0; pk < PQntuples(pkrel_res); pk++)
		{
			pk_relname = PQgetvalue(pkrel_res, pk, 0);
			pk_nspname = PQgetvalue(pkrel_res, pk, 1);

			printfPQExpBuffer(&sql,
							  "SELECT	1 "
							  "FROM \"%s\".\"%s\" t1, "
							  "\"%s\".\"%s\" t2 "
							  "WHERE t2.oid = ANY(t1.\"%s\")"
							  "LIMIT 1",
							  fk_nspname, fk_relname,
							  pk_nspname, pk_relname,
							  fk_attname);

			res = PQexec(conn, sql.data);
			if (!res || PQresultStatus(res) != PGRES_TUPLES_OK)
			{
				fprintf(stderr, "sql error:  %s\n", PQerrorMessage(conn));
				exit(EXIT_FAILURE);
			}

			if (PQntuples(res) != 0)
				printf("Join %s.%s.%s []=> %s.%s.oid\n",
					   fk_nspname, fk_relname, fk_attname,
					   pk_nspname, pk_relname);

			PQclear(res);
		}
	}

	PQclear(fkrel_res);

	PQclear(pkrel_res);

	PQfinish(conn);

	termPQExpBuffer(&sql);

	exit(EXIT_SUCCESS);
}
