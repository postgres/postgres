/*
 *	common.h
 *		Common support routines for bin/scripts/
 *
 *	Copyright (c) 2003-2008, PostgreSQL Global Development Group
 *
 *	$PostgreSQL: pgsql/src/bin/scripts/common.h,v 1.19 2008/01/01 19:45:56 momjian Exp $
 */
#ifndef COMMON_H
#define COMMON_H

#include "libpq-fe.h"
#include "getopt_long.h"
#include "pqexpbuffer.h"

#ifndef HAVE_INT_OPTRESET
extern int	optreset;
#endif

typedef void (*help_handler) (const char *progname);

extern const char *get_user_name(const char *progname);

extern void handle_help_version_opts(int argc, char *argv[],
						 const char *fixed_progname,
						 help_handler hlp);

extern PGconn *connectDatabase(const char *dbname, const char *pghost,
				const char *pgport, const char *pguser,
				bool require_password, const char *progname);

extern PGresult *executeQuery(PGconn *conn, const char *query,
			 const char *progname, bool echo);

extern void executeCommand(PGconn *conn, const char *query,
			   const char *progname, bool echo);

extern bool executeMaintenanceCommand(PGconn *conn, const char *query,
						  bool echo);

extern bool yesno_prompt(const char *question);

extern void setup_cancel_handler(void);

#endif   /* COMMON_H */
