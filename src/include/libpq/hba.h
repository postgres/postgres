/*-------------------------------------------------------------------------
 *
 * hba.h
 *	  Interface to hba.c
 *
 *
 * $Id: hba.h,v 1.18 2000/06/15 03:32:42 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef HBA_H
#define HBA_H

#include <netinet/in.h>

#define CONF_FILE "pg_hba.conf"
 /* Name of the config file  */

#define USERMAP_FILE "pg_ident.conf"
 /* Name of the usermap file */

#define OLD_CONF_FILE "pg_hba"
 /* Name of the config file in prior releases of Postgres. */

#define IDENT_PORT 113
 /* Standard TCP port number for Ident service.  Assigned by IANA */

#define MAX_AUTH_ARG	80		/* Max size of an authentication arg */

typedef enum UserAuth
{
	uaReject,
	uaKrb4,
	uaKrb5,
	uaTrust,
	uaIdent,
	uaPassword,
	uaCrypt
} UserAuth;

typedef struct Port hbaPort;

int	hba_getauthmethod(hbaPort *port);
int authident(struct sockaddr_in * raddr, struct sockaddr_in * laddr,
		  const char *postgres_username, const char *auth_arg);

#endif
