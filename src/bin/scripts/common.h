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

/* Parameters needed by connectDatabase/connectMaintenanceDatabase */
typedef struct _connParams
{
	/* These fields record the actual command line parameters */
	const char *dbname;			/* this may be a connstring! */
	const char *pghost;
	const char *pgport;
	const char *pguser;
	enum trivalue prompt_password;
	/* If not NULL, this overrides the dbname obtained from command line */
	/* (but *only* the DB name, not anything else in the connstring) */
	const char *override_dbname;
} ConnParams;

typedef void (*help_handler) (const char *progname);

extern void handle_help_version_opts(int argc, char *argv[],
									 const char *fixed_progname,
									 help_handler hlp);

extern PGconn *connectDatabase(const ConnParams *cparams,
							   const char *progname,
							   bool echo, bool fail_ok,
							   bool allow_password_reuse);

extern PGconn *connectMaintenanceDatabase(ConnParams *cparams,
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
