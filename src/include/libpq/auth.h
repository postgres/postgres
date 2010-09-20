/*-------------------------------------------------------------------------
 *
 * auth.h
 *	  Definitions for network authentication routines
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/auth.h
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
extern char *pg_krb_realm;

extern void ClientAuthentication(Port *port);

#endif   /* AUTH_H */
