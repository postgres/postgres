/*-------------------------------------------------------------------------
 *
 * hba.h
 *	  Interface to hba.c
 *
 *
 * $PostgreSQL: pgsql/src/include/libpq/hba.h,v 1.35 2004/02/02 16:58:30 neilc Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef HBA_H
#define HBA_H

#ifndef WIN32
#include <netinet/in.h>
#endif

#include "nodes/pg_list.h"

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

extern List **get_user_line(const char *user);
extern void load_hba(void);
extern void load_ident(void);
extern void load_user(void);
extern void load_group(void);
extern int	hba_getauthmethod(hbaPort *port);
extern int	authident(hbaPort *port);

#endif
