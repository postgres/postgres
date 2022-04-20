/*-------------------------------------------------------------------------
 *
 * Facilities for frontend code to connect to and disconnect from databases.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/fe_utils/connect_utils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CONNECT_UTILS_H
#define CONNECT_UTILS_H

#include "libpq-fe.h"

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

extern PGconn *connectDatabase(const ConnParams *cparams,
							   const char *progname,
							   bool echo, bool fail_ok,
							   bool allow_password_reuse);

extern PGconn *connectMaintenanceDatabase(ConnParams *cparams,
										  const char *progname, bool echo);

extern void disconnectDatabase(PGconn *conn);

#endif							/* CONNECT_UTILS_H */
