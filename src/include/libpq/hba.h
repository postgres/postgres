/*-------------------------------------------------------------------------
 *
 * hba.h
 *	  Interface to hba.c
 *
 *
 * $PostgreSQL: pgsql/src/include/libpq/hba.h,v 1.36 2005/02/26 18:43:34 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef HBA_H
#define HBA_H

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
extern bool	read_pg_database_line(FILE *fp, char *dbname,
								  Oid *dboid, Oid *dbtablespace);

#endif /* HBA_H */
