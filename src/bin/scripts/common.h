/*
 *	common.h
 *		Common support routines for bin/scripts/
 *
 *	Copyright (c) 2003-2015, PostgreSQL Global Development Group
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

typedef void (*help_handler) (const char *progname);

extern void handle_help_version_opts(int argc, char *argv[],
						 const char *fixed_progname,
						 help_handler hlp);

extern PGconn *connectDatabase(const char *dbname, const char *pghost,
				const char *pgport, const char *pguser,
				enum trivalue prompt_password, const char *progname,
				bool fail_ok, bool allow_password_reuse);

extern PGconn *connectMaintenanceDatabase(const char *maintenance_db,
				  const char *pghost, const char *pgport, const char *pguser,
						enum trivalue prompt_password, const char *progname);

extern PGresult *executeQuery(PGconn *conn, const char *query,
			 const char *progname, bool echo);

extern void executeCommand(PGconn *conn, const char *query,
			   const char *progname, bool echo);

extern bool executeMaintenanceCommand(PGconn *conn, const char *query,
						  bool echo);

extern bool yesno_prompt(const char *question);

extern void setup_cancel_handler(void);

extern void SetCancelConn(PGconn *conn);
extern void ResetCancelConn(void);


#endif   /* COMMON_H */
