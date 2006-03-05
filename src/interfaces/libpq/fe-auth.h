/*-------------------------------------------------------------------------
 *
 * fe-auth.h
 *
 *	  Definitions for network authentication routines
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/interfaces/libpq/fe-auth.h,v 1.25 2006/03/05 15:59:08 momjian Exp $
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
