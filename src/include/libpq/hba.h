/*-------------------------------------------------------------------------
 *
 * hba.h
 *	  Interface to hba.c
 *
 *
 * src/include/libpq/hba.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HBA_H
#define HBA_H

#include "libpq/pqcomm.h"	/* pgrminclude ignore */	/* needed for NetBSD */
#include "nodes/pg_list.h"
#include "regex/regex.h"


/*
 * The following enum represents the authentication methods that
 * are supported by PostgreSQL.
 *
 * Note: keep this in sync with the UserAuthName array in hba.c.
 */
typedef enum UserAuth
{
	uaReject,
	uaImplicitReject,			/* Not a user-visible option */
	uaTrust,
	uaIdent,
	uaPassword,
	uaMD5,
	uaSCRAM,
	uaGSS,
	uaSSPI,
	uaPAM,
	uaBSD,
	uaLDAP,
	uaCert,
	uaRADIUS,
	uaPeer
#define USER_AUTH_LAST uaPeer	/* Must be last value of this enum */
} UserAuth;

typedef enum IPCompareMethod
{
	ipCmpMask,
	ipCmpSameHost,
	ipCmpSameNet,
	ipCmpAll
} IPCompareMethod;

typedef enum ConnType
{
	ctLocal,
	ctHost,
	ctHostSSL,
	ctHostNoSSL,
	ctHostGSS,
	ctHostNoGSS,
} ConnType;

typedef enum ClientCertMode
{
	clientCertOff,
	clientCertCA,
	clientCertFull
} ClientCertMode;

typedef struct HbaLine
{
	int			linenumber;
	char	   *rawline;
	ConnType	conntype;
	List	   *databases;
	List	   *roles;
	struct sockaddr_storage addr;
	struct sockaddr_storage mask;
	IPCompareMethod ip_cmp_method;
	char	   *hostname;
	UserAuth	auth_method;

	char	   *usermap;
	char	   *pamservice;
	bool		pam_use_hostname;
	bool		ldaptls;
	char	   *ldapscheme;
	char	   *ldapserver;
	int			ldapport;
	char	   *ldapbinddn;
	char	   *ldapbindpasswd;
	char	   *ldapsearchattribute;
	char	   *ldapsearchfilter;
	char	   *ldapbasedn;
	int			ldapscope;
	char	   *ldapprefix;
	char	   *ldapsuffix;
	ClientCertMode clientcert;
	char	   *krb_realm;
	bool		include_realm;
	bool		compat_realm;
	bool		upn_username;
	List	   *radiusservers;
	char	   *radiusservers_s;
	List	   *radiussecrets;
	char	   *radiussecrets_s;
	List	   *radiusidentifiers;
	char	   *radiusidentifiers_s;
	List	   *radiusports;
	char	   *radiusports_s;
} HbaLine;

typedef struct IdentLine
{
	int			linenumber;

	char	   *usermap;
	char	   *ident_user;
	char	   *pg_role;
	regex_t		re;
} IdentLine;

/* kluge to avoid including libpq/libpq-be.h here */
typedef struct Port hbaPort;

extern bool load_hba(void);
extern bool load_ident(void);
extern void hba_getauthmethod(hbaPort *port);
extern int	check_usermap(const char *usermap_name,
						  const char *pg_role, const char *auth_user,
						  bool case_sensitive);
extern bool pg_isblank(const char c);

#endif							/* HBA_H */
