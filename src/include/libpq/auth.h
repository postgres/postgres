/*-------------------------------------------------------------------------
 *
 * auth.h--
 *    Definitions for network authentication routines
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: auth.h,v 1.4 1997/08/19 21:38:34 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef AUTH_H
#define	AUTH_H

#include <libpq/pqcomm.h>

/*----------------------------------------------------------------
 * Common routines and definitions
 *----------------------------------------------------------------
 */

/* what we call "no authentication system" */
#define	UNAUTHNAME		"unauth"

/* what a frontend uses by default */
#if !defined(KRB4) && !defined(KRB5)
#define	DEFAULT_CLIENT_AUTHSVC	UNAUTHNAME
#else /* KRB4 || KRB5 */
#define	DEFAULT_CLIENT_AUTHSVC	"kerberos"
#endif /* KRB4 || KRB5 */

extern int fe_sendauth(MsgType msgtype, Port *port, char *hostname);
extern void fe_setauthsvc(char *name);
extern MsgType fe_getauthsvc();
extern char *fe_getauthname(void);
extern int be_recvauth(MsgType msgtype, Port *port, char *username, StartupInfo* sp);
extern void be_setauthsvc(char *name);

/* the value that matches any dbName value when doing
   host based authentication*/
#define ALL_DBNAME      "*"

#define	PG_KRB4_VERSION	"PGVER4.1"	/* at most KRB_SENDAUTH_VLEN chars */
#define	PG_KRB5_VERSION	"PGVER5.1"

#endif /* AUTH_H */
