/*-------------------------------------------------------------------------
 *
 * auth.h
 *	  Definitions for network authentication routines
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: auth.h,v 1.14 2000/08/25 10:00:33 petere Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef AUTH_H
#define AUTH_H

#include "libpq/libpq-be.h"

/*----------------------------------------------------------------
 * Common routines and definitions
 *----------------------------------------------------------------
 */

void		be_recvauth(Port *port);

#define PG_KRB4_VERSION "PGVER4.1"		/* at most KRB_SENDAUTH_VLEN chars */
#define PG_KRB5_VERSION "PGVER5.1"

extern char * pg_krb_server_keyfile;

#endif	 /* AUTH_H */
