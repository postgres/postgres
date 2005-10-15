/*-------------------------------------------------------------------------
 *
 * fe-auth.h
 *
 *	  Definitions for network authentication routines
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/interfaces/libpq/fe-auth.h,v 1.22 2005/10/15 02:49:48 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FE_AUTH_H
#define FE_AUTH_H

#include "libpq-fe.h"
#include "libpq-int.h"


/*----------------------------------------------------------------
 * Common routines and definitions
 *----------------------------------------------------------------
 */

/* what we call "no authentication system" */
#define UNAUTHNAME				"unauth"

/* what a frontend uses by default */
#ifndef KRB5
#define DEFAULT_CLIENT_AUTHSVC	UNAUTHNAME
#else
#define DEFAULT_CLIENT_AUTHSVC	"kerberos"
#endif   /* KRB5 */

extern int fe_sendauth(AuthRequest areq, PGconn *conn, const char *hostname,
			const char *password, char *PQerrormsg);
extern MsgType fe_getauthsvc(char *PQerrormsg);
extern void fe_setauthsvc(const char *name, char *PQerrormsg);
extern char *fe_getauthname(char *PQerrormsg);

#define PG_KRB5_VERSION "PGVER5.1"		/* at most KRB_SENDAUTH_VLEN chars */

#endif   /* FE_AUTH_H */
