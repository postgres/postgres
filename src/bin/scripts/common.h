/*
 *	common.h
 *		Common support routines for bin/scripts/
 *
 *	Copyright (c) 2003-2018, PostgreSQL Global Development Group
 *
 *	src/bin/scripts/common.h
 */
#ifndef COMMON_H
#define COMMON_H

#include "common/username.h"
#include "libpq-fe.h"
#include "getopt_long.h"		/* pgrminclude ignore */
#include "pqexpbuffer.h"		/* pgrminclude ignore */

enum trivalue
{
	TRI_DEFAULT,
	TRI_NO,
	TRI_YES
};

extern bool CancelRequested;

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

extern PGresult *executeQuery(PGconn *conn, const char *query,
			 const char *progname, bool echo);

extern void executeCommand(PGconn *conn, const char *query,
			   const char *progname, bool echo);

extern bool executeMaintenanceCommand(PGconn *conn, const char *query,
						  bool echo);

extern void appendQualifiedRelation(PQExpBuffer buf, const char *name,
						PGconn *conn, const char *progname, bool echo);

extern bool yesno_prompt(const char *question);

extern void setup_cancel_handler(void);

extern void SetCancelConn(PGconn *conn);
extern void ResetCancelConn(void);


#endif							/* COMMON_H */
