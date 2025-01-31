/*-------------------------------------------------------------------------
 *
 * auth.h
 *	  Definitions for network authentication routines
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/auth.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AUTH_H
#define AUTH_H

#include "libpq/libpq-be.h"

/*
 * Maximum accepted size of GSS and SSPI authentication tokens.
 * We also use this as a limit on ordinary password packet lengths.
 *
 * Kerberos tickets are usually quite small, but the TGTs issued by Windows
 * domain controllers include an authorization field known as the Privilege
 * Attribute Certificate (PAC), which contains the user's Windows permissions
 * (group memberships etc.). The PAC is copied into all tickets obtained on
 * the basis of this TGT (even those issued by Unix realms which the Windows
 * realm trusts), and can be several kB in size. The maximum token size
 * accepted by Windows systems is determined by the MaxAuthToken Windows
 * registry setting. Microsoft recommends that it is not set higher than
 * 65535 bytes, so that seems like a reasonable limit for us as well.
 */
#define PG_MAX_AUTH_TOKEN_LENGTH	65535

extern PGDLLIMPORT char *pg_krb_server_keyfile;
extern PGDLLIMPORT bool pg_krb_caseins_users;
extern PGDLLIMPORT bool pg_gss_accept_delegation;

extern void ClientAuthentication(Port *port);
extern void sendAuthRequest(Port *port, AuthRequest areq, const char *extradata,
							int extralen);

/* Hook for plugins to get control in ClientAuthentication() */
typedef void (*ClientAuthentication_hook_type) (Port *, int);
extern PGDLLIMPORT ClientAuthentication_hook_type ClientAuthentication_hook;

/* hook type for password manglers */
typedef char *(*auth_password_hook_typ) (char *input);

/* Default LDAP password mutator hook, can be overridden by a shared library */
extern PGDLLIMPORT auth_password_hook_typ ldap_password_hook;

#endif							/* AUTH_H */
