/*-------------------------------------------------------------------------
 *
 * connectdb.h
 *      Common header file for connection to the database.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    src/bin/pg_dump/connectdb.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CONNECTDB_H
#define CONNECTDB_H

#include "pg_backup.h"
#include "pg_backup_utils.h"

extern PGconn *ConnectDatabase(const char *dbname, const char *connection_string, const char *pghost,
							   const char *pgport, const char *pguser,
							   trivalue prompt_password, bool fail_on_error,
							   const char *progname, const char **connstr, int *server_version,
							   char *password, char *override_dbname);
extern PGresult *executeQuery(PGconn *conn, const char *query);
#endif							/* CONNECTDB_H */
