/*-------------------------------------------------------------------------
 *
 * hba.h
 *	  Interface to hba.c
 *
 *
 * $PostgreSQL: pgsql/src/include/libpq/hba.h,v 1.62 2010/04/19 19:02:18 sriggs Exp $
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
	uaImplicitReject,
	uaKrb5,
	uaTrust,
	uaIdent,
	uaPassword,
	uaMD5,
	uaGSS,
	uaSSPI,
	uaPAM,
	uaLDAP,
	uaCert,
	uaRADIUS
} UserAuth;

typedef enum IPCompareMethod
{
	ipCmpMask,
	ipCmpSameHost,
	ipCmpSameNet
} IPCompareMethod;

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
	IPCompareMethod ip_cmp_method;
	UserAuth	auth_method;

	char	   *usermap;
	char	   *pamservice;
	bool		ldaptls;
	char	   *ldapserver;
	int			ldapport;
	char	   *ldapbinddn;
	char	   *ldapbindpasswd;
	char	   *ldapsearchattribute;
	char	   *ldapbasedn;
	char	   *ldapprefix;
	char	   *ldapsuffix;
	bool		clientcert;
	char	   *krb_server_hostname;
	char	   *krb_realm;
	bool		include_realm;
	char	   *radiusserver;
	char	   *radiussecret;
	char	   *radiusidentifier;
	int			radiusport;
} HbaLine;

/* kluge to avoid including libpq/libpq-be.h here */
typedef struct Port hbaPort;

extern bool load_hba(void);
extern void load_ident(void);
extern int	hba_getauthmethod(hbaPort *port);
extern int check_usermap(const char *usermap_name,
			  const char *pg_role, const char *auth_user,
			  bool case_sensitive);
extern bool pg_isblank(const char c);

#endif   /* HBA_H */
