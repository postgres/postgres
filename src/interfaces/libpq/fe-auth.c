/*-------------------------------------------------------------------------
 *
 * fe-auth.c--
 *	   The front-end (client) authorization routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/libpq/fe-auth.c,v 1.25 1999/01/22 13:28:50 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

/*
 * INTERFACE ROUTINES
 *	   frontend (client) routines:
 *		fe_sendauth				send authentication information
 *		fe_getauthname			get user's name according to the client side
 *								of the authentication system
 *		fe_setauthsvc			set frontend authentication service
 *		fe_getauthsvc			get current frontend authentication service
 *
 *
 *
 */

#include "libpq-fe.h"
#include "libpq-int.h"
#include "fe-auth.h"
#include "postgres.h"

#ifdef WIN32
#include "win32.h"
#else
#include <string.h>
#include <sys/param.h>			/* for MAXHOSTNAMELEN on most */
#ifndef  MAXHOSTNAMELEN
#include <netdb.h>				/* for MAXHOSTNAMELEN on some */
#endif
#if !defined(NO_UNISTD_H)
#include <unistd.h>
#endif
#include <pwd.h>
#endif	 /* WIN32 */

#ifdef HAVE_CRYPT_H
#include <crypt.h>
#endif


/*----------------------------------------------------------------
 * common definitions for generic fe/be routines
 *----------------------------------------------------------------
 */

struct authsvc
{
	char		name[NAMEDATALEN];		/* service nickname (for command
										 * line) */
	MsgType		msgtype;		/* startup packet header type */
	int			allowed;		/* initially allowed (before command line
								 * option parsing)? */
};

/*
 * Command-line parsing routines use this structure to map nicknames
 * onto service types (and the startup packets to use with them).
 *
 * Programs receiving an authentication request use this structure to
 * decide which authentication service types are currently permitted.
 * By default, all authentication systems compiled into the system are
 * allowed.  Unauthenticated connections are disallowed unless there
 * isn't any authentication system.
 */
static struct authsvc authsvcs[] = {
#ifdef KRB4
	{"krb4", STARTUP_KRB4_MSG, 1},
	{"kerberos", STARTUP_KRB4_MSG, 1},
#endif	 /* KRB4 */
#ifdef KRB5
	{"krb5", STARTUP_KRB5_MSG, 1},
	{"kerberos", STARTUP_KRB5_MSG, 1},
#endif	 /* KRB5 */
	{UNAUTHNAME, STARTUP_MSG,
#if defined(KRB4) || defined(KRB5)
		0
#else							/* !(KRB4 || KRB5) */
		1
#endif	 /* !(KRB4 || KRB5) */
	},
	{"password", STARTUP_PASSWORD_MSG, 0}
};

static int n_authsvcs = sizeof(authsvcs) / sizeof(struct authsvc);

#ifdef KRB4
/*----------------------------------------------------------------
 * MIT Kerberos authentication system - protocol version 4
 *----------------------------------------------------------------
 */

#include "krb.h"

/* for some reason, this is not defined in krb.h ... */
extern char *tkt_string(void);

/*
 * pg_krb4_init -- initialization performed before any Kerberos calls are made
 *
 * For v4, all we need to do is make sure the library routines get the right
 * ticket file if we want them to see a special one.  (They will open the file
 * themselves.)
 */
static void
pg_krb4_init()
{
	char	   *realm;
	static		init_done = 0;

	if (init_done)
		return;
	init_done = 1;

	/*
	 * If the user set PGREALM, then we use a ticket file with a special
	 * name: <usual-ticket-file-name>@<PGREALM-value>
	 */
	if (realm = getenv("PGREALM"))
	{
		char		tktbuf[MAXPATHLEN];

		(void) sprintf(tktbuf, "%s@%s", tkt_string(), realm);
		krb_set_tkt_string(tktbuf);
	}
}

/*
 * pg_krb4_authname -- returns a pointer to static space containing whatever
 *					   name the user has authenticated to the system
 *
 * We obtain this information by digging around in the ticket file.
 */
static char *
pg_krb4_authname(char *PQerrormsg)
{
	char		instance[INST_SZ];
	char		realm[REALM_SZ];
	int			status;
	static char name[SNAME_SZ + 1] = "";

	if (name[0])
		return name;

	pg_krb4_init();

	name[SNAME_SZ] = '\0';
	status = krb_get_tf_fullname(tkt_string(), name, instance, realm);
	if (status != KSUCCESS)
	{
		(void) sprintf(PQerrormsg,
					   "pg_krb4_authname: krb_get_tf_fullname: %s\n",
					   krb_err_txt[status]);
		return (char *) NULL;
	}
	return name;
}

/*
 * pg_krb4_sendauth -- client routine to send authentication information to
 *					   the server
 *
 * This routine does not do mutual authentication, nor does it return enough
 * information to do encrypted connections.  But then, if we want to do
 * encrypted connections, we'll have to redesign the whole RPC mechanism
 * anyway.
 *
 * If the user is too lazy to feed us a hostname, we try to come up with
 * something other than "localhost" since the hostname is used as an
 * instance and instance names in v4 databases are usually actual hostnames
 * (canonicalized to omit all domain suffixes).
 */
static int
pg_krb4_sendauth(const char *PQerrormsg, int sock,
				 struct sockaddr_in * laddr,
				 struct sockaddr_in * raddr,
				 const char *hostname)
{
	long		krbopts = 0;	/* one-way authentication */
	KTEXT_ST	clttkt;
	int			status;
	char		hostbuf[MAXHOSTNAMELEN];
	const char *realm = getenv("PGREALM");		/* NULL == current realm */

	if (!hostname || !(*hostname))
	{
		if (gethostname(hostbuf, MAXHOSTNAMELEN) < 0)
			strcpy(hostbuf, "localhost");
		hostname = hostbuf;
	}

	pg_krb4_init();

	status = krb_sendauth(krbopts,
						  sock,
						  &clttkt,
						  PG_KRB_SRVNAM,
						  hostname,
						  realm,
						  (u_long) 0,
						  (MSG_DAT *) NULL,
						  (CREDENTIALS *) NULL,
						  (Key_schedule *) NULL,
						  laddr,
						  raddr,
						  PG_KRB4_VERSION);
	if (status != KSUCCESS)
	{
		(void) sprintf(PQerrormsg,
					   "pg_krb4_sendauth: kerberos error: %s\n",
					   krb_err_txt[status]);
		return STATUS_ERROR;
	}
	return STATUS_OK;
}

#endif	 /* KRB4 */

#ifdef KRB5
/*----------------------------------------------------------------
 * MIT Kerberos authentication system - protocol version 5
 *----------------------------------------------------------------
 */

#include "krb5/krb5.h"

/*
 * pg_an_to_ln -- return the local name corresponding to an authentication
 *				  name
 *
 * XXX Assumes that the first aname component is the user name.  This is NOT
 *	   necessarily so, since an aname can actually be something out of your
 *	   worst X.400 nightmare, like
 *		  ORGANIZATION=U. C. Berkeley/NAME=Paul M. Aoki@CS.BERKELEY.EDU
 *	   Note that the MIT an_to_ln code does the same thing if you don't
 *	   provide an aname mapping database...it may be a better idea to use
 *	   krb5_an_to_ln, except that it punts if multiple components are found,
 *	   and we can't afford to punt.
 */
static char *
pg_an_to_ln(const char *aname)
{
	char	   *p;

	if ((p = strchr(aname, '/')) || (p = strchr(aname, '@')))
		*p = '\0';
	return aname;
}


/*
 * pg_krb5_init -- initialization performed before any Kerberos calls are made
 *
 * With v5, we can no longer set the ticket (credential cache) file name;
 * we now have to provide a file handle for the open (well, "resolved")
 * ticket file everywhere.
 *
 */
static int
			krb5_ccache
pg_krb5_init(void)
{
	krb5_error_code code;
	char	   *realm,
			   *defname;
	char		tktbuf[MAXPATHLEN];
	static krb5_ccache ccache = (krb5_ccache) NULL;

	if (ccache)
		return ccache;

	/*
	 * If the user set PGREALM, then we use a ticket file with a special
	 * name: <usual-ticket-file-name>@<PGREALM-value>
	 */
	if (!(defname = krb5_cc_default_name()))
	{
		(void) sprintf(PQerrormsg,
					   "pg_krb5_init: krb5_cc_default_name failed\n");
		return (krb5_ccache) NULL;
	}
	strcpy(tktbuf, defname);
	if (realm = getenv("PGREALM"))
	{
		strcat(tktbuf, "@");
		strcat(tktbuf, realm);
	}

	if (code = krb5_cc_resolve(tktbuf, &ccache))
	{
		(void) sprintf(PQerrormsg,
				  "pg_krb5_init: Kerberos error %d in krb5_cc_resolve\n",
					   code);
		com_err("pg_krb5_init", code, "in krb5_cc_resolve");
		return (krb5_ccache) NULL;
	}
	return ccache;
}

/*
 * pg_krb5_authname -- returns a pointer to static space containing whatever
 *					   name the user has authenticated to the system
 *
 * We obtain this information by digging around in the ticket file.
 */
static const char *
pg_krb5_authname(const char *PQerrormsg)
{
	krb5_ccache ccache;
	krb5_principal principal;
	krb5_error_code code;
	static char *authname = (char *) NULL;

	if (authname)
		return authname;

	ccache = pg_krb5_init();	/* don't free this */

	if (code = krb5_cc_get_principal(ccache, &principal))
	{
		(void) sprintf(PQerrormsg,
		"pg_krb5_authname: Kerberos error %d in krb5_cc_get_principal\n",
					   code);
		com_err("pg_krb5_authname", code, "in krb5_cc_get_principal");
		return (char *) NULL;
	}
	if (code = krb5_unparse_name(principal, &authname))
	{
		(void) sprintf(PQerrormsg,
			"pg_krb5_authname: Kerberos error %d in krb5_unparse_name\n",
					   code);
		com_err("pg_krb5_authname", code, "in krb5_unparse_name");
		krb5_free_principal(principal);
		return (char *) NULL;
	}
	krb5_free_principal(principal);
	return pg_an_to_ln(authname);
}

/*
 * pg_krb5_sendauth -- client routine to send authentication information to
 *					   the server
 *
 * This routine does not do mutual authentication, nor does it return enough
 * information to do encrypted connections.  But then, if we want to do
 * encrypted connections, we'll have to redesign the whole RPC mechanism
 * anyway.
 *
 * Server hostnames are canonicalized v4-style, i.e., all domain suffixes
 * are simply chopped off.	Hence, we are assuming that you've entered your
 * server instances as
 *		<value-of-PG_KRB_SRVNAM>/<canonicalized-hostname>
 * in the PGREALM (or local) database.	This is probably a bad assumption.
 */
static int
pg_krb5_sendauth(const char *PQerrormsg, int sock,
				 struct sockaddr_in * laddr,
				 struct sockaddr_in * raddr,
				 const char *hostname)
{
	char		servbuf[MAXHOSTNAMELEN + 1 +
									sizeof(PG_KRB_SRVNAM)];
	const char *hostp;
	const char *realm;
	krb5_error_code code;
	krb5_principal client,
				server;
	krb5_ccache ccache;
	krb5_error *error = (krb5_error *) NULL;

	ccache = pg_krb5_init();	/* don't free this */

	/*
	 * set up client -- this is easy, we can get it out of the ticket
	 * file.
	 */
	if (code = krb5_cc_get_principal(ccache, &client))
	{
		(void) sprintf(PQerrormsg,
		"pg_krb5_sendauth: Kerberos error %d in krb5_cc_get_principal\n",
					   code);
		com_err("pg_krb5_sendauth", code, "in krb5_cc_get_principal");
		return STATUS_ERROR;
	}

	/*
	 * set up server -- canonicalize as described above
	 */
	strcpy(servbuf, PG_KRB_SRVNAM);
	*(hostp = servbuf + (sizeof(PG_KRB_SRVNAM) - 1)) = '/';
	if (hostname || *hostname)
		strncpy(++hostp, hostname, MAXHOSTNAMELEN);
	else
	{
		if (gethostname(++hostp, MAXHOSTNAMELEN) < 0)
			strcpy(hostp, "localhost");
	}
	if (hostp = strchr(hostp, '.'))
		*hostp = '\0';
	if (realm = getenv("PGREALM"))
	{
		strcat(servbuf, "@");
		strcat(servbuf, realm);
	}
	if (code = krb5_parse_name(servbuf, &server))
	{
		(void) sprintf(PQerrormsg,
			  "pg_krb5_sendauth: Kerberos error %d in krb5_parse_name\n",
					   code);
		com_err("pg_krb5_sendauth", code, "in krb5_parse_name");
		krb5_free_principal(client);
		return STATUS_ERROR;
	}

	/*
	 * The only thing we want back from krb5_sendauth is an error status
	 * and any error messages.
	 */
	if (code = krb5_sendauth((krb5_pointer) & sock,
							 PG_KRB5_VERSION,
							 client,
							 server,
							 (krb5_flags) 0,
							 (krb5_checksum *) NULL,
							 (krb5_creds *) NULL,
							 ccache,
							 (krb5_int32 *) NULL,
							 (krb5_keyblock **) NULL,
							 &error,
							 (krb5_ap_rep_enc_part **) NULL))
	{
		if ((code == KRB5_SENDAUTH_REJECTED) && error)
		{
			(void) sprintf(PQerrormsg,
				  "pg_krb5_sendauth: authentication rejected: \"%*s\"\n",
						   error->text.length, error->text.data);
		}
		else
		{
			(void) sprintf(PQerrormsg,
				"pg_krb5_sendauth: Kerberos error %d in krb5_sendauth\n",
						   code);
			com_err("pg_krb5_sendauth", code, "in krb5_sendauth");
		}
	}
	krb5_free_principal(client);
	krb5_free_principal(server);
	return code ? STATUS_ERROR : STATUS_OK;
}

#endif	 /* KRB5 */

static int
pg_password_sendauth(PGconn *conn, const char *password, AuthRequest areq)
{
	/* Encrypt the password if needed. */

	if (areq == AUTH_REQ_CRYPT)
		password = crypt(password, conn->salt);

	return pqPacketSend(conn, password, strlen(password) + 1);
}

/*
 * fe_sendauth -- client demux routine for outgoing authentication information
 */
int
fe_sendauth(AuthRequest areq, PGconn *conn, const char *hostname,
			const char *password, char *PQerrormsg)
{
	switch (areq)
	{
			case AUTH_REQ_OK:
			break;

		case AUTH_REQ_KRB4:
#ifdef KRB4
			if (pg_krb4_sendauth(PQerrormsg, conn->sock, &conn->laddr.in,
								 &conn->raddr.in,
								 hostname) != STATUS_OK)
			{
				(void) sprintf(PQerrormsg,
							"fe_sendauth: krb4 authentication failed\n");
				return STATUS_ERROR;
			}
			break;
#else
			(void) sprintf(PQerrormsg,
					 "fe_sendauth: krb4 authentication not supported\n");
			return STATUS_ERROR;
#endif

		case AUTH_REQ_KRB5:
#ifdef KRB5
			if (pg_krb5_sendauth(PQerrormsg, conn->sock, &conn->laddr.in,
								 &conn->raddr.in,
								 hostname) != STATUS_OK)
			{
				(void) sprintf(PQerrormsg,
							"fe_sendauth: krb5 authentication failed\n");
				return STATUS_ERROR;
			}
			break;
#else
			(void) sprintf(PQerrormsg,
					 "fe_sendauth: krb5 authentication not supported\n");
			return STATUS_ERROR;
#endif

		case AUTH_REQ_PASSWORD:
		case AUTH_REQ_CRYPT:
			if (password == NULL || *password == '\0')
			{
				(void) sprintf(PQerrormsg,
							   "fe_sendauth: no password supplied\n");
				return STATUS_ERROR;
			}
			if (pg_password_sendauth(conn, password, areq) != STATUS_OK)
			{
				(void) sprintf(PQerrormsg,
				 "fe_sendauth: error sending password authentication\n");
				return STATUS_ERROR;
			}

			break;

		default:
			(void) sprintf(PQerrormsg,
			"fe_sendauth: authentication type %u not supported\n", areq);
			return STATUS_ERROR;
	}

	return STATUS_OK;
}

/*
 * fe_setauthsvc
 * fe_getauthsvc
 *
 * Set/return the authentication service currently selected for use by the
 * frontend. (You can only use one in the frontend, obviously.)
 */
static int pg_authsvc = -1;

void
fe_setauthsvc(const char *name, char *PQerrormsg)
{
	int			i;

	for (i = 0; i < n_authsvcs; ++i)
		if (!strcmp(name, authsvcs[i].name))
		{
			pg_authsvc = i;
			break;
		}
	if (i == n_authsvcs)
	{
		(void) sprintf(PQerrormsg,
					   "fe_setauthsvc: invalid name: %s, ignoring...\n",
					   name);
	}
	return;
}

MsgType
fe_getauthsvc(char *PQerrormsg)
{
	if (pg_authsvc < 0 || pg_authsvc >= n_authsvcs)
		fe_setauthsvc(DEFAULT_CLIENT_AUTHSVC, PQerrormsg);
	return authsvcs[pg_authsvc].msgtype;
}

/*
 * fe_getauthname -- returns a pointer to dynamic space containing whatever
 *					 name the user has authenticated to the system
 * if there is an error, return the error message in PQerrormsg
 */
char *
fe_getauthname(char *PQerrormsg)
{
	char	   *name = (char *) NULL;
	char	   *authn = (char *) NULL;
	MsgType		authsvc;

	authsvc = fe_getauthsvc(PQerrormsg);
	switch ((int) authsvc)
	{
#ifdef KRB4
		case STARTUP_KRB4_MSG:
			name = pg_krb4_authname(PQerrormsg);
			break;
#endif
#ifdef KRB5
		case STARTUP_KRB5_MSG:
			name = pg_krb5_authname(PQerrormsg);
			break;
#endif
		case STARTUP_MSG:
			{
#ifdef WIN32
				char		username[128];
				DWORD		namesize = sizeof(username) - 1;

				if (GetUserName(username, &namesize))
					name = username;
#else
				struct passwd *pw = getpwuid(geteuid());

				if (pw)
					name = pw->pw_name;
#endif
			}
			break;
		default:
			(void) sprintf(PQerrormsg,
				   "fe_getauthname: invalid authentication system: %d\n",
						   authsvc);
			break;
	}

	if (name && (authn = (char *) malloc(strlen(name) + 1)))
		strcpy(authn, name);
	return authn;
}
