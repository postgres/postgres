/*-------------------------------------------------------------------------
 *
 * fe-auth.h
 *    
 *    Definitions for network authentication routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: fe-auth.h,v 1.2 1996/08/06 16:16:44 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FE_AUTH_H
#define	FE_AUTH_H

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

extern int fe_sendauth(MsgType msgtype, Port *port, const char *hostname, const char* PQerromsg);
extern void fe_setauthsvc(const char *name, char* PQerrormsg);

#define	PG_KRB4_VERSION	"PGVER4.1"	/* at most KRB_SENDAUTH_VLEN chars */
#define	PG_KRB5_VERSION	"PGVER5.1"

#endif /* FE_AUTH_H */

