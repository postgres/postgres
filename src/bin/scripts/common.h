/*
 *	common.h
 *		Common support routines for bin/scripts/
 *
 *	Copyright (c) 2003-2020, PostgreSQL Global Development Group
 *
 *	src/bin/scripts/common.h
 */
#ifndef COMMON_H
#define COMMON_H

#include "common/username.h"
#include "getopt_long.h"		/* pgrminclude ignore */
#include "libpq-fe.h"
#include "pqexpbuffer.h"		/* pgrminclude ignore */

enum trivalue
{
	TRI_DEFAULT,
	TRI_NO,
	TRI_YES
};

typedef void (*help_handler) (const char *progname);

extern void handle_help_version_opts(int argc, char *argv[],
									 const char *fixed_progname,
									 help_handler hlp);

extern PGconn *connectDatabase(const char *dbname, const char *pghost,
							   const char *pgport, const char *pguser,
							   enum trivalue prompt_password, const char *progname,
							   bool echo, bool fail_ok, bool allow_password_reuse);

extern PGconn *connectMaintenanceDatabase(const char *maintenance_db,
										  const char *pghost, const char *pgport,
										  const char *pguser, enum trivalue prompt_password,
										  const char *progname, bool echo);

extern void disconnectDatabase(PGconn *conn);

extern PGresult *executeQuery(PGconn *conn, const char *query, bool echo);

extern void executeCommand(PGconn *conn, const char *query, bool echo);

extern bool executeMaintenanceCommand(PGconn *conn, const char *query,
									  bool echo);

extern bool consumeQueryResult(PGconn *conn);

extern bool processQueryResult(PGconn *conn, PGresult *result);

extern void splitTableColumnsSpec(const char *spec, int encoding,
								  char **table, const char **columns);

extern void appendQualifiedRelation(PQExpBuffer buf, const char *name,
									PGconn *conn, bool echo);

extern bool yesno_prompt(const char *question);

#endif							/* COMMON_H */
