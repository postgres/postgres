/*-------------------------------------------------------------------------
 *
 * fe-auth.c
 *	   The front-end (client) authorization routines
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * NOTE: the error message strings returned by this module must not
 * exceed INITIAL_EXPBUFFER_SIZE (currently 256 bytes).
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/libpq/fe-auth.c,v 1.40 2000/05/27 03:39:33 momjian Exp $
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

#ifndef WIN32
#include "postgres.h"
#endif
#include "libpq-fe.h"
#include "libpq-int.h"
#include "fe-auth.h"

#ifdef WIN32
#include "win32.h"
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/param.h>			/* for MAXHOSTNAMELEN on most */
#ifndef  MAXHOSTNAMELEN
#include <netdb.h>				/* for MAXHOSTNAMELEN on some */
#endif
#include <pwd.h>
#endif

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
static const struct authsvc authsvcs[] = {
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

static const int n_authsvcs = sizeof(authsvcs) / sizeof(struct authsvc);

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
		char		tktbuf[MAXPGPATH];

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
	char		instance[INST_SZ + 1];
	char		realm[REALM_SZ + 1];
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

#include <krb5.h>
#include <com_err.h>

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
pg_an_to_ln(char *aname)
{
	char	   *p;

	if ((p = strchr(aname, '/')) || (p = strchr(aname, '@')))
		*p = '\0';
	return aname;
}


/*
 * Various krb5 state which is not connection specfic, and a flag to
 * indicate whether we have initialised it yet.
 */
static int pg_krb5_initialised;
static krb5_context pg_krb5_context;
static krb5_ccache pg_krb5_ccache;
static krb5_principal pg_krb5_client;
static char *pg_krb5_name;


static int
pg_krb5_init(char *PQerrormsg)
{
	krb5_error_code retval;

	if (pg_krb5_initialised)
		return STATUS_OK;

	retval = krb5_init_context(&pg_krb5_context);
	if (retval) {
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				 "pg_krb5_init: krb5_init_context: %s",
				 error_message(retval));
		return STATUS_ERROR;
	}

	retval = krb5_cc_default(pg_krb5_context, &pg_krb5_ccache);
	if (retval) {
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				 "pg_krb5_init: krb5_cc_default: %s",
				 error_message(retval));
		krb5_free_context(pg_krb5_context);
		return STATUS_ERROR;
    }

    retval = krb5_cc_get_principal(pg_krb5_context, pg_krb5_ccache, 
								   &pg_krb5_client);
	if (retval) {
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				 "pg_krb5_init: krb5_cc_get_principal: %s",
				 error_message(retval));
		krb5_cc_close(pg_krb5_context, pg_krb5_ccache);
		krb5_free_context(pg_krb5_context);
		return STATUS_ERROR;
    }

    retval = krb5_unparse_name(pg_krb5_context, pg_krb5_client, &pg_krb5_name);
	if (retval) {
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				 "pg_krb5_init: krb5_unparse_name: %s",
				 error_message(retval));
		krb5_free_principal(pg_krb5_context, pg_krb5_client);
		krb5_cc_close(pg_krb5_context, pg_krb5_ccache);
		krb5_free_context(pg_krb5_context);
		return STATUS_ERROR;
	}	

	pg_krb5_name = pg_an_to_ln(pg_krb5_name);

	pg_krb5_initialised = 1;
	return STATUS_OK;
}


/*
 * pg_krb5_authname -- returns a pointer to static space containing whatever
 *					   name the user has authenticated to the system
  */
static const char *
pg_krb5_authname(char *PQerrormsg)
{
	if (pg_krb5_init(PQerrormsg) != STATUS_OK)
		return NULL;

	return pg_krb5_name;
}


/*
 * pg_krb5_sendauth -- client routine to send authentication information to
 *					   the server
 */
static int
pg_krb5_sendauth(char *PQerrormsg, int sock,
				 struct sockaddr_in * laddr,
				 struct sockaddr_in * raddr,
				 const char *hostname)
{
	krb5_error_code retval;
	int ret;
	krb5_principal server;
	krb5_auth_context auth_context = NULL;
    krb5_error *err_ret = NULL;
	int flags;

	ret = pg_krb5_init(PQerrormsg);
	if (ret != STATUS_OK)
		return ret;

	retval = krb5_sname_to_principal(pg_krb5_context, hostname, PG_KRB_SRVNAM, 
									 KRB5_NT_SRV_HST, &server);
	if (retval) {
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				 "pg_krb5_sendauth: krb5_sname_to_principal: %s",
				 error_message(retval));
		return STATUS_ERROR;
	}

	/* 
	 * libpq uses a non-blocking socket. But kerberos needs a blocking
	 * socket, and we have to block somehow to do mutual authentication
	 * anyway. So we temporarily make it blocking.
	 */
	flags = fcntl(sock, F_GETFL);
	if (flags < 0 || fcntl(sock, F_SETFL, (long)(flags & ~O_NONBLOCK))) {
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				 "pg_krb5_sendauth: fcntl: %s", strerror(errno));
		krb5_free_principal(pg_krb5_context, server);
		return STATUS_ERROR;
	}

	retval = krb5_sendauth(pg_krb5_context, &auth_context,
						   (krb5_pointer) &sock, PG_KRB_SRVNAM,
						   pg_krb5_client, server,
						   AP_OPTS_MUTUAL_REQUIRED,
						   NULL, 0,		/* no creds, use ccache instead */
						   pg_krb5_ccache, &err_ret, NULL, NULL);
	if (retval) {
		if (retval == KRB5_SENDAUTH_REJECTED && err_ret) {
			snprintf(PQerrormsg, PQERRORMSG_LENGTH,
					 "pg_krb5_sendauth: authentication rejected: \"%*s\"",
					 err_ret->text.length, err_ret->text.data);
		}
		else {
			snprintf(PQerrormsg, PQERRORMSG_LENGTH,
					 "pg_krb5_sendauth: krb5_sendauth: %s",
					 error_message(retval));
		}
			
		if (err_ret)
			krb5_free_error(pg_krb5_context, err_ret);
		
		ret = STATUS_ERROR;
	}

	krb5_free_principal(pg_krb5_context, server);
	
	if (fcntl(sock, F_SETFL, (long)flags)) {
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				 "pg_krb5_sendauth: fcntl: %s", strerror(errno));
		ret = STATUS_ERROR;
	}

	return ret;
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
#if !defined(KRB4) && !defined(KRB5)
	(void) hostname;			/* not used */
#endif

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
 *
 * NB: This is not thread-safe if different threads try to select different
 * authentication services!  It's OK for fe_getauthsvc to select the default,
 * since that will be the same for all threads, but direct application use
 * of fe_setauthsvc is not thread-safe.  However, use of fe_setauthsvc is
 * deprecated anyway...
 */

static int	pg_authsvc = -1;

void
fe_setauthsvc(const char *name, char *PQerrormsg)
{
	int			i;

	for (i = 0; i < n_authsvcs; ++i)
		if (strcmp(name, authsvcs[i].name) == 0)
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
