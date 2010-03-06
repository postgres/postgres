/*-------------------------------------------------------------------------
 *
 * hba.h
 *	  Interface to hba.c
 *
 *
 * $Id: hba.h,v 1.33.4.1 2010/03/06 00:46:27 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef HBA_H
#define HBA_H

#ifndef WIN32
#include <netinet/in.h>
#endif

#include "nodes/pg_list.h"

#define CONF_FILE "pg_hba.conf"
 /* Name of the config file  */

#define USERMAP_FILE "pg_ident.conf"
 /* Name of the usermap file */

#define IDENT_PORT 113
 /* Standard TCP port number for Ident service.  Assigned by IANA */

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
#endif   /* USE_PAM */
} UserAuth;

typedef struct Port hbaPort;

#define MAX_TOKEN	256

extern List **get_user_line(const char *user);
extern void load_hba(void);
extern void load_ident(void);
extern void load_user(void);
extern void load_group(void);
extern int	hba_getauthmethod(hbaPort *port);
extern int	authident(hbaPort *port);

#endif
