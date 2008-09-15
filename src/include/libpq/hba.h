/*-------------------------------------------------------------------------
 *
 * hba.h
 *	  Interface to hba.c
 *
 *
 * $PostgreSQL: pgsql/src/include/libpq/hba.h,v 1.49 2008/09/15 12:32:57 mha Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef HBA_H
#define HBA_H

#include "nodes/pg_list.h"
#include "libpq/pqcomm.h"


typedef enum UserAuth
{
	uaReject,
	uaKrb5,
	uaTrust,
	uaIdent,
	uaPassword,
	uaCrypt,
	uaMD5,
	uaGSS,
	uaSSPI
#ifdef USE_PAM
	,uaPAM
#endif   /* USE_PAM */
#ifdef USE_LDAP
	,uaLDAP
#endif
} UserAuth;

typedef enum ConnType
{
	ctLocal,
	ctHost,
	ctHostSSL,
	ctHostNoSSL
} ConnType;

typedef struct 
{
	int			linenumber;
	ConnType	conntype;
	char	   *database;
	char	   *role;
	struct sockaddr_storage addr;
	struct sockaddr_storage mask;
	UserAuth	auth_method;
	char	   *usermap;
	char	   *auth_arg;
} HbaLine;

typedef struct Port hbaPort;

extern List **get_role_line(const char *role);
extern bool load_hba(void);
extern void load_ident(void);
extern void load_role(void);
extern int	hba_getauthmethod(hbaPort *port);
extern bool read_pg_database_line(FILE *fp, char *dbname, Oid *dboid,
					  Oid *dbtablespace, TransactionId *dbfrozenxid);
extern bool check_ident_usermap(const char *usermap_name,
					  const char *pg_role, const char *ident_user);
extern bool pg_isblank(const char c);

#endif   /* HBA_H */
