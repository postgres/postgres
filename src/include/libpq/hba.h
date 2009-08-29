/*-------------------------------------------------------------------------
 *
 * hba.h
 *	  Interface to hba.c
 *
 *
 * $PostgreSQL: pgsql/src/include/libpq/hba.h,v 1.57 2009/08/29 19:26:51 tgl Exp $
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
	uaMD5,
	uaGSS,
	uaSSPI,
	uaPAM,
	uaLDAP,
	uaCert
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
	char	   *pamservice;
	bool		ldaptls;
	char	   *ldapserver;
	int			ldapport;
	char	   *ldapprefix;
	char	   *ldapsuffix;
	bool		clientcert;
	char	   *krb_server_hostname;
	char	   *krb_realm;
	bool		include_realm;
} HbaLine;

/* kluge to avoid including libpq/libpq-be.h here */
typedef struct Port hbaPort;

extern bool load_hba(void);
extern void load_ident(void);
extern int	hba_getauthmethod(hbaPort *port);
extern bool read_pg_database_line(FILE *fp, char *dbname, Oid *dboid,
					  Oid *dbtablespace, TransactionId *dbfrozenxid);
extern int check_usermap(const char *usermap_name,
			  const char *pg_role, const char *auth_user,
			  bool case_sensitive);
extern bool pg_isblank(const char c);

#endif   /* HBA_H */
