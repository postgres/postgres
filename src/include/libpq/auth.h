/*-------------------------------------------------------------------------
 *
 * auth.h
 *	  Definitions for network authentication routines
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/libpq/auth.h,v 1.31 2005/10/17 16:24:20 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef AUTH_H
#define AUTH_H

#include "libpq/libpq-be.h"

extern char *pg_krb_server_keyfile;
extern char *pg_krb_srvnam;
extern bool pg_krb_caseins_users;
extern char *pg_krb_server_hostname;

extern void ClientAuthentication(Port *port);

#endif   /* AUTH_H */
