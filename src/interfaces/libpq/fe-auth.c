/*-------------------------------------------------------------------------
 *
 * fe-auth.c
 *	   The front-end (client) authorization routines
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * NOTE: the error message strings returned by this module must not
 * exceed INITIAL_EXPBUFFER_SIZE (currently 256 bytes).
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/interfaces/libpq/fe-auth.c,v 1.121.2.2 2007/07/23 18:13:09 mha Exp $
 *
 *-------------------------------------------------------------------------
 */

/*
 * INTERFACE ROUTINES
 *	   frontend (client) routines:
 *		pg_fe_sendauth			send authentication information
 *		pg_fe_getauthname		get user's name according to the client side
 *								of the authentication system
 */

#include "postgres_fe.h"

#ifdef WIN32
#include "win32.h"
#else
#include <unistd.h>
#include <fcntl.h>
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
#include "fe-auth.h"
#include "libpq/md5.h"


#ifdef KRB5
/*
 * MIT Kerberos authentication system - protocol version 5
 */

#include <krb5.h>
/* Some old versions of Kerberos do not include <com_err.h> in <krb5.h> */
#if !defined(__COM_ERR_H) && !defined(__COM_ERR_H__)
#include <com_err.h>
#endif

/*
 * Heimdal doesn't have a free function for unparsed names. Just pass it to
 * standard free() which should work in these cases.
 */
#ifndef HAVE_KRB5_FREE_UNPARSED_NAME
static void
krb5_free_unparsed_name(krb5_context context, char *val)
{
	free(val);
}
#endif

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
 *
 * For WIN32, convert username to lowercase because the Win32 kerberos library
 * generates tickets with the username as the user entered it instead of as
 * it is entered in the directory.
 */
static char *
pg_an_to_ln(char *aname)
{
	char	   *p;

	if ((p = strchr(aname, '/')) || (p = strchr(aname, '@')))
		*p = '\0';
#ifdef WIN32
	for (p = aname; *p; p++)
		*p = pg_tolower((unsigned char) *p);
#endif

	return aname;
}


/*
 * Various krb5 state which is not connection specific, and a flag to
 * indicate whether we have initialised it yet.
 */
/*
static int	pg_krb5_initialised;
static krb5_context pg_krb5_context;
static krb5_ccache pg_krb5_ccache;
static krb5_principal pg_krb5_client;
static char *pg_krb5_name;
*/

struct krb5_info
{
	int			pg_krb5_initialised;
	krb5_context pg_krb5_context;
	krb5_ccache pg_krb5_ccache;
	krb5_principal pg_krb5_client;
	char	   *pg_krb5_name;
};


static int
pg_krb5_init(char *PQerrormsg, struct krb5_info * info)
{
	krb5_error_code retval;

	if (info->pg_krb5_initialised)
		return STATUS_OK;

	retval = krb5_init_context(&(info->pg_krb5_context));
	if (retval)
	{
		snprintf(PQerrormsg, INITIAL_EXPBUFFER_SIZE,
				 "pg_krb5_init: krb5_init_context: %s\n",
				 error_message(retval));
		return STATUS_ERROR;
	}

	retval = krb5_cc_default(info->pg_krb5_context, &(info->pg_krb5_ccache));
	if (retval)
	{
		snprintf(PQerrormsg, INITIAL_EXPBUFFER_SIZE,
				 "pg_krb5_init: krb5_cc_default: %s\n",
				 error_message(retval));
		krb5_free_context(info->pg_krb5_context);
		return STATUS_ERROR;
	}

	retval = krb5_cc_get_principal(info->pg_krb5_context, info->pg_krb5_ccache,
								   &(info->pg_krb5_client));
	if (retval)
	{
		snprintf(PQerrormsg, INITIAL_EXPBUFFER_SIZE,
				 "pg_krb5_init: krb5_cc_get_principal: %s\n",
				 error_message(retval));
		krb5_cc_close(info->pg_krb5_context, info->pg_krb5_ccache);
		krb5_free_context(info->pg_krb5_context);
		return STATUS_ERROR;
	}

	retval = krb5_unparse_name(info->pg_krb5_context, info->pg_krb5_client, &(info->pg_krb5_name));
	if (retval)
	{
		snprintf(PQerrormsg, INITIAL_EXPBUFFER_SIZE,
				 "pg_krb5_init: krb5_unparse_name: %s\n",
				 error_message(retval));
		krb5_free_principal(info->pg_krb5_context, info->pg_krb5_client);
		krb5_cc_close(info->pg_krb5_context, info->pg_krb5_ccache);
		krb5_free_context(info->pg_krb5_context);
		return STATUS_ERROR;
	}

	info->pg_krb5_name = pg_an_to_ln(info->pg_krb5_name);

	info->pg_krb5_initialised = 1;
	return STATUS_OK;
}

static void
pg_krb5_destroy(struct krb5_info * info)
{
	krb5_free_principal(info->pg_krb5_context, info->pg_krb5_client);
	krb5_cc_close(info->pg_krb5_context, info->pg_krb5_ccache);
	krb5_free_unparsed_name(info->pg_krb5_context, info->pg_krb5_name);
	krb5_free_context(info->pg_krb5_context);
}



/*
 * pg_krb5_authname -- returns a copy of whatever name the user
 *					   has authenticated to the system, or NULL
 */
static char *
pg_krb5_authname(char *PQerrormsg)
{
	char	   *tmp_name;
	struct krb5_info info;

	info.pg_krb5_initialised = 0;

	if (pg_krb5_init(PQerrormsg, &info) != STATUS_OK)
		return NULL;
	tmp_name = strdup(info.pg_krb5_name);
	pg_krb5_destroy(&info);

	return tmp_name;
}


/*
 * pg_krb5_sendauth -- client routine to send authentication information to
 *					   the server
 */
static int
pg_krb5_sendauth(char *PQerrormsg, int sock, const char *hostname, const char *servicename)
{
	krb5_error_code retval;
	int			ret;
	krb5_principal server;
	krb5_auth_context auth_context = NULL;
	krb5_error *err_ret = NULL;
	struct krb5_info info;

	info.pg_krb5_initialised = 0;

	if (!hostname)
	{
		snprintf(PQerrormsg, INITIAL_EXPBUFFER_SIZE,
				 "pg_krb5_sendauth: hostname must be specified for Kerberos authentication\n");
		return STATUS_ERROR;
	}

	ret = pg_krb5_init(PQerrormsg, &info);
	if (ret != STATUS_OK)
		return ret;

	retval = krb5_sname_to_principal(info.pg_krb5_context, hostname, servicename,
									 KRB5_NT_SRV_HST, &server);
	if (retval)
	{
		snprintf(PQerrormsg, INITIAL_EXPBUFFER_SIZE,
				 "pg_krb5_sendauth: krb5_sname_to_principal: %s\n",
				 error_message(retval));
		pg_krb5_destroy(&info);
		return STATUS_ERROR;
	}

	/*
	 * libpq uses a non-blocking socket. But kerberos needs a blocking socket,
	 * and we have to block somehow to do mutual authentication anyway. So we
	 * temporarily make it blocking.
	 */
	if (!pg_set_block(sock))
	{
		char		sebuf[256];

		snprintf(PQerrormsg, INITIAL_EXPBUFFER_SIZE,
				 libpq_gettext("could not set socket to blocking mode: %s\n"), pqStrerror(errno, sebuf, sizeof(sebuf)));
		krb5_free_principal(info.pg_krb5_context, server);
		pg_krb5_destroy(&info);
		return STATUS_ERROR;
	}

	retval = krb5_sendauth(info.pg_krb5_context, &auth_context,
						   (krb5_pointer) & sock, (char *) servicename,
						   info.pg_krb5_client, server,
						   AP_OPTS_MUTUAL_REQUIRED,
						   NULL, 0,		/* no creds, use ccache instead */
						   info.pg_krb5_ccache, &err_ret, NULL, NULL);
	if (retval)
	{
		if (retval == KRB5_SENDAUTH_REJECTED && err_ret)
		{
#if defined(HAVE_KRB5_ERROR_TEXT_DATA)
			snprintf(PQerrormsg, INITIAL_EXPBUFFER_SIZE,
				  libpq_gettext("Kerberos 5 authentication rejected: %*s\n"),
					 (int) err_ret->text.length, err_ret->text.data);
#elif defined(HAVE_KRB5_ERROR_E_DATA)
			snprintf(PQerrormsg, INITIAL_EXPBUFFER_SIZE,
				  libpq_gettext("Kerberos 5 authentication rejected: %*s\n"),
					 (int) err_ret->e_data->length,
					 (const char *) err_ret->e_data->data);
#else
#error "bogus configuration"
#endif
		}
		else
		{
			snprintf(PQerrormsg, INITIAL_EXPBUFFER_SIZE,
					 "krb5_sendauth: %s\n", error_message(retval));
		}

		if (err_ret)
			krb5_free_error(info.pg_krb5_context, err_ret);

		ret = STATUS_ERROR;
	}

	krb5_free_principal(info.pg_krb5_context, server);

	if (!pg_set_noblock(sock))
	{
		char		sebuf[256];

		snprintf(PQerrormsg, INITIAL_EXPBUFFER_SIZE,
		libpq_gettext("could not restore non-blocking mode on socket: %s\n"),
				 pqStrerror(errno, sebuf, sizeof(sebuf)));
		ret = STATUS_ERROR;
	}
	pg_krb5_destroy(&info);

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
	 * The backend doesn't care what we send here, but it wants exactly one
	 * character to force recvmsg() to block and wait for us.
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

		snprintf(PQerrormsg, INITIAL_EXPBUFFER_SIZE,
				 "pg_local_sendauth: sendmsg: %s\n",
				 pqStrerror(errno, sebuf, sizeof(sebuf)));
		return STATUS_ERROR;
	}
	return STATUS_OK;
#else
	snprintf(PQerrormsg, INITIAL_EXPBUFFER_SIZE,
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

				/* Allocate enough space for two MD5 hashes */
				crypt_pwd = malloc(2 * (MD5_PASSWD_LEN + 1));
				if (!crypt_pwd)
				{
					fprintf(stderr, libpq_gettext("out of memory\n"));
					return STATUS_ERROR;
				}

				crypt_pwd2 = crypt_pwd + MD5_PASSWD_LEN + 1;
				if (!pg_md5_encrypt(password, conn->pguser,
									strlen(conn->pguser), crypt_pwd2))
				{
					free(crypt_pwd);
					return STATUS_ERROR;
				}
				if (!pg_md5_encrypt(crypt_pwd2 + strlen("md5"), conn->md5Salt,
									sizeof(conn->md5Salt), crypt_pwd))
				{
					free(crypt_pwd);
					return STATUS_ERROR;
				}
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
 * pg_fe_sendauth
 *		client demux routine for outgoing authentication information
 */
int
pg_fe_sendauth(AuthRequest areq, PGconn *conn, const char *hostname,
			   const char *password, char *PQerrormsg)
{
#ifndef KRB5
	(void) hostname;			/* not used */
#endif

	switch (areq)
	{
		case AUTH_REQ_OK:
			break;

		case AUTH_REQ_KRB4:
			snprintf(PQerrormsg, INITIAL_EXPBUFFER_SIZE,
				 libpq_gettext("Kerberos 4 authentication not supported\n"));
			return STATUS_ERROR;

		case AUTH_REQ_KRB5:
#ifdef KRB5
			pglock_thread();
			if (pg_krb5_sendauth(PQerrormsg, conn->sock,
								 hostname, conn->krbsrvname) != STATUS_OK)
			{
				/* PQerrormsg already filled in */
				pgunlock_thread();
				return STATUS_ERROR;
			}
			pgunlock_thread();
			break;
#else
			snprintf(PQerrormsg, INITIAL_EXPBUFFER_SIZE,
				 libpq_gettext("Kerberos 5 authentication not supported\n"));
			return STATUS_ERROR;
#endif

		case AUTH_REQ_MD5:
		case AUTH_REQ_CRYPT:
		case AUTH_REQ_PASSWORD:
			if (password == NULL || *password == '\0')
			{
				(void) snprintf(PQerrormsg, INITIAL_EXPBUFFER_SIZE,
								PQnoPasswordSupplied);
				return STATUS_ERROR;
			}
			if (pg_password_sendauth(conn, password, areq) != STATUS_OK)
			{
				(void) snprintf(PQerrormsg, INITIAL_EXPBUFFER_SIZE,
					 "fe_sendauth: error sending password authentication\n");
				return STATUS_ERROR;
			}
			break;

		case AUTH_REQ_SCM_CREDS:
			if (pg_local_sendauth(PQerrormsg, conn) != STATUS_OK)
				return STATUS_ERROR;
			break;

		default:
			snprintf(PQerrormsg, INITIAL_EXPBUFFER_SIZE,
			libpq_gettext("authentication method %u not supported\n"), areq);
			return STATUS_ERROR;
	}

	return STATUS_OK;
}


/*
 * pg_fe_getauthname -- returns a pointer to dynamic space containing whatever
 *					 name the user has authenticated to the system
 *
 * if there is an error, return NULL with an error message in PQerrormsg
 */
char *
pg_fe_getauthname(char *PQerrormsg)
{
#ifdef KRB5
	char	   *krb5_name = NULL;
#endif
	const char *name = NULL;
	char	   *authn;

#ifdef WIN32
	char		username[128];
	DWORD		namesize = sizeof(username) - 1;
#else
	char		pwdbuf[BUFSIZ];
	struct passwd pwdstr;
	struct passwd *pw = NULL;
#endif

	/*
	 * pglock_thread() really only needs to be called around
	 * pg_krb5_authname(), but some users are using configure
	 * --enable-thread-safety-force, so we might as well do the locking within
	 * our library to protect pqGetpwuid(). In fact, application developers
	 * can use getpwuid() in their application if they use the locking call we
	 * provide, or install their own locking function using
	 * PQregisterThreadLock().
	 */
	pglock_thread();

#ifdef KRB5

	/*
	 * pg_krb5_authname gives us a strdup'd value that we need to free later,
	 * however, we don't want to free 'name' directly in case it's *not* a
	 * Kerberos login and we fall through to name = pw->pw_name;
	 */
	krb5_name = pg_krb5_authname(PQerrormsg);
	name = krb5_name;
#endif

	if (!name)
	{
#ifdef WIN32
		if (GetUserName(username, &namesize))
			name = username;
#else
		if (pqGetpwuid(geteuid(), &pwdstr, pwdbuf, sizeof(pwdbuf), &pw) == 0)
			name = pw->pw_name;
#endif
	}

	authn = name ? strdup(name) : NULL;

#ifdef KRB5
	/* Free the strdup'd string from pg_krb5_authname, if we got one */
	if (krb5_name)
		free(krb5_name);
#endif

	pgunlock_thread();

	return authn;
}


/*
 * PQencryptPassword -- exported routine to encrypt a password
 *
 * This is intended to be used by client applications that wish to send
 * commands like ALTER USER joe PASSWORD 'pwd'.  The password need not
 * be sent in cleartext if it is encrypted on the client side.	This is
 * good because it ensures the cleartext password won't end up in logs,
 * pg_stat displays, etc.  We export the function so that clients won't
 * be dependent on low-level details like whether the enceyption is MD5
 * or something else.
 *
 * Arguments are the cleartext password, and the SQL name of the user it
 * is for.
 *
 * Return value is a malloc'd string, or NULL if out-of-memory.  The client
 * may assume the string doesn't contain any special characters that would
 * require escaping.
 */
char *
PQencryptPassword(const char *passwd, const char *user)
{
	char	   *crypt_pwd;

	crypt_pwd = malloc(MD5_PASSWD_LEN + 1);
	if (!crypt_pwd)
		return NULL;

	if (!pg_md5_encrypt(passwd, user, strlen(user), crypt_pwd))
	{
		free(crypt_pwd);
		return NULL;
	}

	return crypt_pwd;
}
