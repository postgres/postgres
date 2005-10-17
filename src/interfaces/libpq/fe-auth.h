/*-------------------------------------------------------------------------
 *
 * fe-auth.h
 *
 *	  Definitions for network authentication routines
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/interfaces/libpq/fe-auth.h,v 1.23 2005/10/17 16:24:20 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FE_AUTH_H
#define FE_AUTH_H

#include "libpq-fe.h"
#include "libpq-int.h"


extern int pg_fe_sendauth(AuthRequest areq, PGconn *conn, const char *hostname,
			const char *password, char *PQerrormsg);
extern char *pg_fe_getauthname(char *PQerrormsg);

#endif   /* FE_AUTH_H */
