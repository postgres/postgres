/*-------------------------------------------------------------------------
 *
 * Facilities for frontend code to query a databases.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/fe_utils/query_utils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef QUERY_UTILS_H
#define QUERY_UTILS_H

#include "postgres_fe.h"

#include "libpq-fe.h"

extern PGresult *executeQuery(PGconn *conn, const char *query, bool echo);

extern void executeCommand(PGconn *conn, const char *query, bool echo);

extern bool executeMaintenanceCommand(PGconn *conn, const char *query,
									  bool echo);

#endif							/* QUERY_UTILS_H */
