/*-------------------------------------------------------------------------
 *
 * hba.h
 *	  Interface to hba.c
 *
 *
 * $Id: hba.h,v 1.30 2001/10/28 06:26:07 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef HBA_H
#define HBA_H

#ifndef WIN32
#include <netinet/in.h>
#endif

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
	uaCrypt,
	uaMD5
#ifdef USE_PAM
	,uaPAM
#endif	 /* USE_PAM */
} UserAuth;

typedef struct Port hbaPort;

extern int	hba_getauthmethod(hbaPort *port);
extern int	authident(hbaPort *port);
extern void load_hba_and_ident(void);

#endif
