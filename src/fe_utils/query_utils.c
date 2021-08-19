/*-------------------------------------------------------------------------
 *
 * Facilities for frontend code to query a databases.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/fe_utils/query_utils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "common/logging.h"
#include "fe_utils/cancel.h"
#include "fe_utils/query_utils.h"

/*
 * Run a query, return the results, exit program on failure.
 */
PGresult *
executeQuery(PGconn *conn, const char *query, bool echo)
{

	if (echo)
		printf("%s\n", query);

	PGresult   *res = PQexec(conn, query);

	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("query failed: %s", PQerrorMessage(conn));
		pg_log_info("query was: %s", query);
		PQfinish(conn);
		exit(1);
	}

	return res;
}


/*
 * As above for a SQL command (which returns nothing).
 */
void
executeCommand(PGconn *conn, const char *query, bool echo)
{

	if (echo)
		printf("%s\n", query);

	PGresult   *res = PQexec(conn, query);

	if (!res ||
		PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		pg_log_error("query failed: %s", PQerrorMessage(conn));
		pg_log_info("query was: %s", query);
		PQfinish(conn);
		exit(1);
	}

	PQclear(res);
}


/*
 * As above for a SQL maintenance command (returns command success).
 * Command is executed with a cancel handler set, so Ctrl-C can
 * interrupt it.
 */
bool
executeMaintenanceCommand(PGconn *conn, const char *query, bool echo)
{

	if (echo)
		printf("%s\n", query);

	SetCancelConn(conn);
	PGresult   *res = PQexec(conn, query);

	ResetCancelConn();

	bool		r = (res && PQresultStatus(res) == PGRES_COMMAND_OK);

	if (res)
		PQclear(res);

	return r;
}
