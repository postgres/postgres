/*-------------------------------------------------------------------------
 *
 * fe-auth.c
 *	   The front-end (client) authorization routines
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * NOTE: the error message strings returned by this module must not
 * exceed INITIAL_EXPBUFFER_SIZE (currently 256 bytes).
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/libpq/fe-auth.c,v 1.84.2.3 2003/12/20 18:46:02 tgl Exp $
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

#include "postgres_fe.h"

#ifdef WIN32
#include "win32.h"
#else
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/param.h>			/* for MAXHOSTNAMELEN on most */
#include <sys/socket.h>
#if defined(HAVE_STRUCT_CMSGCRED) || defined(HAVE_STRUCT_FCRED) || defined(HAVE_STRUCT_SOCKCRED)
#include <sys/uio.h>
#include <sys/ucred.h>
#endif
#ifndef  MAXHOSTNAMELEN
#include <netdb.h>				/* for MAXHOSTNAMELEN on some */
#endif
#include <pwd.h>
#endif

#ifdef HAVE_CRYPT_H
#include <crypt.h>
#endif

#include "libpq-fe.h"
#include "libpq-int.h"
#include "fe-auth.h"
#include "libpq/crypt.h"


/*
 * common definitions for generic fe/be routines
 */

#define STARTUP_MSG		7		/* Initialise a connection */
#define STARTUP_KRB4_MSG	10	/* krb4 session follows */
#define STARTUP_KRB5_MSG	11	/* krb5 session follows */
#define STARTUP_PASSWORD_MSG	14		/* Password follows */

struct authsvc
{
	const char *name;			/* service nickname (for command line) */
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
#endif   /* KRB4 */
#ifdef KRB5
	{"krb5", STARTUP_KRB5_MSG, 1},
	{"kerberos", STARTUP_KRB5_MSG, 1},
#endif   /* KRB5 */
	{UNAUTHNAME, STARTUP_MSG,
#if defined(KRB4) || defined(KRB5)
		0
#else							/* !(KRB4 || KRB5) */
		1
#endif   /* !(KRB4 || KRB5) */
	},
	{"password", STARTUP_PASSWORD_MSG, 0}
};

static const int n_authsvcs = sizeof(authsvcs) / sizeof(struct authsvc);

#ifdef KRB4
/*
 * MIT Kerberos authentication system - protocol version 4
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
	static int	init_done = 0;

	if (init_done)
		return;
	init_done = 1;

	/*
	 * If the user set PGREALM, then we use a ticket file with a special
	 * name: <usual-ticket-file-name>@<PGREALM-value>
	 */
	if ((realm = getenv("PGREALM")))
	{
		char		tktbuf[MAXPGPATH];

		(void) snprintf(tktbuf, sizeof(tktbuf), "%s@%s", tkt_string(), realm);
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
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
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
pg_krb4_sendauth(char *PQerrormsg, int sock,
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
						  NULL,
						  laddr,
						  raddr,
						  PG_KRB4_VERSION);
	if (status != KSUCCESS)
	{
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				 libpq_gettext("Kerberos 4 error: %s\n"),
				 krb_err_txt[status]);
		return STATUS_ERROR;
	}
	return STATUS_OK;
}
#endif   /* KRB4 */

#ifdef KRB5
/*
 * MIT Kerberos authentication system - protocol version 5
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
 * Various krb5 state which is not connection specific, and a flag to
 * indicate whether we have initialised it yet.
 */
static int	pg_krb5_initialised;
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
	if (retval)
	{
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				 "pg_krb5_init: krb5_init_context: %s\n",
				 error_message(retval));
		return STATUS_ERROR;
	}

	retval = krb5_cc_default(pg_krb5_context, &pg_krb5_ccache);
	if (retval)
	{
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				 "pg_krb5_init: krb5_cc_default: %s\n",
				 error_message(retval));
		krb5_free_context(pg_krb5_context);
		return STATUS_ERROR;
	}

	retval = krb5_cc_get_principal(pg_krb5_context, pg_krb5_ccache,
								   &pg_krb5_client);
	if (retval)
	{
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				 "pg_krb5_init: krb5_cc_get_principal: %s\n",
				 error_message(retval));
		krb5_cc_close(pg_krb5_context, pg_krb5_ccache);
		krb5_free_context(pg_krb5_context);
		return STATUS_ERROR;
	}

	retval = krb5_unparse_name(pg_krb5_context, pg_krb5_client, &pg_krb5_name);
	if (retval)
	{
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				 "pg_krb5_init: krb5_unparse_name: %s\n",
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
pg_krb5_sendauth(char *PQerrormsg, int sock, const char *hostname)
{
	krb5_error_code retval;
	int			ret;
	krb5_principal server;
	krb5_auth_context auth_context = NULL;
	krb5_error *err_ret = NULL;
	int			flags;

	ret = pg_krb5_init(PQerrormsg);
	if (ret != STATUS_OK)
		return ret;

	retval = krb5_sname_to_principal(pg_krb5_context, hostname, PG_KRB_SRVNAM,
									 KRB5_NT_SRV_HST, &server);
	if (retval)
	{
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				 "pg_krb5_sendauth: krb5_sname_to_principal: %s\n",
				 error_message(retval));
		return STATUS_ERROR;
	}

	/*
	 * libpq uses a non-blocking socket. But kerberos needs a blocking
	 * socket, and we have to block somehow to do mutual authentication
	 * anyway. So we temporarily make it blocking.
	 */
	flags = fcntl(sock, F_GETFL);
	if (flags < 0 || fcntl(sock, F_SETFL, (long) (flags & ~O_NONBLOCK)))
	{
		char		sebuf[256];

		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				 libpq_gettext("could not set socket to blocking mode: %s\n"), pqStrerror(errno, sebuf, sizeof(sebuf)));
		krb5_free_principal(pg_krb5_context, server);
		return STATUS_ERROR;
	}

	retval = krb5_sendauth(pg_krb5_context, &auth_context,
						   (krb5_pointer) & sock, PG_KRB_SRVNAM,
						   pg_krb5_client, server,
						   AP_OPTS_MUTUAL_REQUIRED,
						   NULL, 0,		/* no creds, use ccache instead */
						   pg_krb5_ccache, &err_ret, NULL, NULL);
	if (retval)
	{
		if (retval == KRB5_SENDAUTH_REJECTED && err_ret)
		{
#if defined(HAVE_KRB5_ERROR_TEXT_DATA)
			snprintf(PQerrormsg, PQERRORMSG_LENGTH,
			  libpq_gettext("Kerberos 5 authentication rejected: %*s\n"),
					 (int) err_ret->text.length, err_ret->text.data);
#elif defined(HAVE_KRB5_ERROR_E_DATA)
			snprintf(PQerrormsg, PQERRORMSG_LENGTH,
			  libpq_gettext("Kerberos 5 authentication rejected: %*s\n"),
					 (int) err_ret->e_data->length,
					 (const char *) err_ret->e_data->data);
#else
#error "bogus configuration"
#endif
		}
		else
		{
			snprintf(PQerrormsg, PQERRORMSG_LENGTH,
					 "krb5_sendauth: %s\n", error_message(retval));
		}

		if (err_ret)
			krb5_free_error(pg_krb5_context, err_ret);

		ret = STATUS_ERROR;
	}

	krb5_free_principal(pg_krb5_context, server);

	if (fcntl(sock, F_SETFL, (long) flags))
	{
		char		sebuf[256];

		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				 libpq_gettext("could not restore non-blocking mode on socket: %s\n"),
				 pqStrerror(errno, sebuf, sizeof(sebuf)));
		ret = STATUS_ERROR;
	}

	return ret;
}
#endif   /* KRB5 */

/*
 * Respond to AUTH_REQ_SCM_CREDS challenge.
 *
 * Note: current backends will not use this challenge if HAVE_GETPEEREID
 * or SO_PEERCRED is defined, but pre-7.4 backends might, so compile the
 * code anyway.
 */
static int
pg_local_sendauth(char *PQerrormsg, PGconn *conn)
{
#if defined(HAVE_STRUCT_CMSGCRED) || defined(HAVE_STRUCT_FCRED) || \
	(defined(HAVE_STRUCT_SOCKCRED) && defined(LOCAL_CREDS))
	char		buf;
	struct iovec iov;
	struct msghdr msg;

#ifdef HAVE_STRUCT_CMSGCRED
	/* Prevent padding */
	char		cmsgmem[sizeof(struct cmsghdr) + sizeof(struct cmsgcred)];

	/* Point to start of first structure */
	struct cmsghdr *cmsg = (struct cmsghdr *) cmsgmem;
#endif

	/*
	 * The backend doesn't care what we send here, but it wants exactly
	 * one character to force recvmsg() to block and wait for us.
	 */
	buf = '\0';
	iov.iov_base = &buf;
	iov.iov_len = 1;

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

#ifdef HAVE_STRUCT_CMSGCRED
	/* Create control header, FreeBSD */
	msg.msg_control = cmsg;
	msg.msg_controllen = sizeof(cmsgmem);
	memset(cmsg, 0, sizeof(cmsgmem));
	cmsg->cmsg_len = sizeof(cmsgmem);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_CREDS;
#endif

	if (sendmsg(conn->sock, &msg, 0) == -1)
	{
		char		sebuf[256];

		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				 "pg_local_sendauth: sendmsg: %s\n",
				 pqStrerror(errno, sebuf, sizeof(sebuf)));
		return STATUS_ERROR;
	}
	return STATUS_OK;
#else
	snprintf(PQerrormsg, PQERRORMSG_LENGTH,
		libpq_gettext("SCM_CRED authentication method not supported\n"));
	return STATUS_ERROR;
#endif
}

static int
pg_password_sendauth(PGconn *conn, const char *password, AuthRequest areq)
{
	int			ret;
	char	   *crypt_pwd;

	/* Encrypt the password if needed. */

	switch (areq)
	{
		case AUTH_REQ_MD5:
			{
				char	   *crypt_pwd2;

				if (!(crypt_pwd = malloc(MD5_PASSWD_LEN + 1)) ||
					!(crypt_pwd2 = malloc(MD5_PASSWD_LEN + 1)))
				{
					perror("malloc");
					return STATUS_ERROR;
				}
				if (!EncryptMD5(password, conn->pguser,
								strlen(conn->pguser), crypt_pwd2))
				{
					free(crypt_pwd);
					free(crypt_pwd2);
					return STATUS_ERROR;
				}
				if (!EncryptMD5(crypt_pwd2 + strlen("md5"), conn->md5Salt,
								sizeof(conn->md5Salt), crypt_pwd))
				{
					free(crypt_pwd);
					free(crypt_pwd2);
					return STATUS_ERROR;
				}
				free(crypt_pwd2);
				break;
			}
		case AUTH_REQ_CRYPT:
			{
				char		salt[3];

				StrNCpy(salt, conn->cryptSalt, 3);
				crypt_pwd = crypt(password, salt);
				break;
			}
		case AUTH_REQ_PASSWORD:
			/* discard const so we can assign it */
			crypt_pwd = (char *) password;
			break;
		default:
			return STATUS_ERROR;
	}
	/* Packet has a message type as of protocol 3.0 */
	if (PG_PROTOCOL_MAJOR(conn->pversion) >= 3)
		ret = pqPacketSend(conn, 'p', crypt_pwd, strlen(crypt_pwd) + 1);
	else
		ret = pqPacketSend(conn, 0, crypt_pwd, strlen(crypt_pwd) + 1);
	if (areq == AUTH_REQ_MD5)
		free(crypt_pwd);
	return ret;
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
			if (pg_krb4_sendauth(PQerrormsg, conn->sock,
							   (struct sockaddr_in *) & conn->laddr.addr,
							   (struct sockaddr_in *) & conn->raddr.addr,
								 hostname) != STATUS_OK)
			{
				snprintf(PQerrormsg, PQERRORMSG_LENGTH,
					libpq_gettext("Kerberos 4 authentication failed\n"));
				return STATUS_ERROR;
			}
			break;
#else
			snprintf(PQerrormsg, PQERRORMSG_LENGTH,
			 libpq_gettext("Kerberos 4 authentication not supported\n"));
			return STATUS_ERROR;
#endif

		case AUTH_REQ_KRB5:
#ifdef KRB5
			if (pg_krb5_sendauth(PQerrormsg, conn->sock,
								 hostname) != STATUS_OK)
			{
				snprintf(PQerrormsg, PQERRORMSG_LENGTH,
					libpq_gettext("Kerberos 5 authentication failed\n"));
				return STATUS_ERROR;
			}
			break;
#else
			snprintf(PQerrormsg, PQERRORMSG_LENGTH,
			 libpq_gettext("Kerberos 5 authentication not supported\n"));
			return STATUS_ERROR;
#endif

		case AUTH_REQ_MD5:
		case AUTH_REQ_CRYPT:
		case AUTH_REQ_PASSWORD:
			if (password == NULL || *password == '\0')
			{
				(void) snprintf(PQerrormsg, PQERRORMSG_LENGTH,
								"fe_sendauth: no password supplied\n");
				return STATUS_ERROR;
			}
			if (pg_password_sendauth(conn, password, areq) != STATUS_OK)
			{
				(void) snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				 "fe_sendauth: error sending password authentication\n");
				return STATUS_ERROR;
			}
			break;

		case AUTH_REQ_SCM_CREDS:
			if (pg_local_sendauth(PQerrormsg, conn) != STATUS_OK)
				return STATUS_ERROR;
			break;

		default:
			snprintf(PQerrormsg, PQERRORMSG_LENGTH,
					 libpq_gettext("authentication method %u not supported\n"), areq);
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
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				 libpq_gettext("invalid authentication service name \"%s\", ignored\n"),
				 name);
	}
	return;
}

MsgType
fe_getauthsvc(char *PQerrormsg)
{
	if (pg_authsvc < 0 || pg_authsvc >= n_authsvcs)
	{
		fe_setauthsvc(DEFAULT_CLIENT_AUTHSVC, PQerrormsg);
		if (pg_authsvc < 0 || pg_authsvc >= n_authsvcs)
		{
			/* Can only get here if DEFAULT_CLIENT_AUTHSVC is misdefined */
			return 0;
		}
	}
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
	const char *name = (char *) NULL;
	char	   *authn = (char *) NULL;
	MsgType		authsvc;

	authsvc = fe_getauthsvc(PQerrormsg);

	/* this just guards against broken DEFAULT_CLIENT_AUTHSVC, see above */
	if (authsvc == 0)
		return NULL;			/* leave original error message in place */

#ifdef KRB4
	if (authsvc == STARTUP_KRB4_MSG)
		name = pg_krb4_authname(PQerrormsg);
#endif
#ifdef KRB5
	if (authsvc == STARTUP_KRB5_MSG)
		name = pg_krb5_authname(PQerrormsg);
#endif

	if (authsvc == STARTUP_MSG
		|| (authsvc == STARTUP_KRB4_MSG && !name)
		|| (authsvc == STARTUP_KRB5_MSG && !name))
	{
#ifdef WIN32
		char		username[128];
		DWORD		namesize = sizeof(username) - 1;

		if (GetUserName(username, &namesize))
			name = username;
#else
		char		pwdbuf[BUFSIZ];
		struct passwd pwdstr;
		struct passwd *pw = NULL;

		if (pqGetpwuid(geteuid(), &pwdstr,
					   pwdbuf, sizeof(pwdbuf), &pw) == 0)
			name = pw->pw_name;
#endif
	}

	if (authsvc != STARTUP_MSG && authsvc != STARTUP_KRB4_MSG && authsvc != STARTUP_KRB5_MSG)
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				 libpq_gettext("fe_getauthname: invalid authentication system: %d\n"),
				 authsvc);

	if (name && (authn = (char *) malloc(strlen(name) + 1)))
		strcpy(authn, name);
	return authn;
}
