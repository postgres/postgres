/*-------------------------------------------------------------------------
 *
 * auth.c
 *	  Routines to handle network authentication
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/libpq/auth.c,v 1.112.2.1 2003/12/20 18:25:02 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/param.h>
#include <sys/socket.h>
#if defined(HAVE_STRUCT_CMSGCRED) || defined(HAVE_STRUCT_FCRED) || defined(HAVE_STRUCT_SOCKCRED)
#include <sys/uio.h>
#include <sys/ucred.h>
#include <errno.h>
#endif
#include <netinet/in.h>
#include <arpa/inet.h>

#include "libpq/auth.h"
#include "libpq/crypt.h"
#include "libpq/hba.h"
#include "libpq/libpq.h"
#include "libpq/pqcomm.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "storage/ipc.h"


static void sendAuthRequest(Port *port, AuthRequest areq);
static void auth_failed(Port *port, int status);
static char *recv_password_packet(Port *port);
static int	recv_and_check_password_packet(Port *port);

char	   *pg_krb_server_keyfile;

#ifdef USE_PAM
#ifdef HAVE_PAM_PAM_APPL_H
#include <pam/pam_appl.h>
#endif
#ifdef HAVE_SECURITY_PAM_APPL_H
#include <security/pam_appl.h>
#endif

#define PGSQL_PAM_SERVICE "postgresql"	/* Service name passed to PAM */

static int	CheckPAMAuth(Port *port, char *user, char *password);
static int pam_passwd_conv_proc(int num_msg, const struct pam_message ** msg,
					 struct pam_response ** resp, void *appdata_ptr);

static struct pam_conv pam_passw_conv = {
	&pam_passwd_conv_proc,
	NULL
};

static char *pam_passwd = NULL; /* Workaround for Solaris 2.6 brokenness */
static Port *pam_port_cludge;	/* Workaround for passing "Port *port"
								 * into pam_passwd_conv_proc */
#endif   /* USE_PAM */

#ifdef KRB4
/*----------------------------------------------------------------
 * MIT Kerberos authentication system - protocol version 4
 *----------------------------------------------------------------
 */

#include "krb.h"

/*
 * pg_krb4_recvauth -- server routine to receive authentication information
 *					   from the client
 *
 * Nothing unusual here, except that we compare the username obtained from
 * the client's setup packet to the authenticated name.  (We have to retain
 * the name in the setup packet since we have to retain the ability to handle
 * unauthenticated connections.)
 */
static int
pg_krb4_recvauth(Port *port)
{
	long		krbopts = 0;	/* one-way authentication */
	KTEXT_ST	clttkt;
	char		instance[INST_SZ + 1],
				version[KRB_SENDAUTH_VLEN + 1];
	AUTH_DAT	auth_data;
	Key_schedule key_sched;
	int			status;

	strcpy(instance, "*");		/* don't care, but arg gets expanded
								 * anyway */
	status = krb_recvauth(krbopts,
						  port->sock,
						  &clttkt,
						  PG_KRB_SRVNAM,
						  instance,
						  &port->raddr.in,
						  &port->laddr.in,
						  &auth_data,
						  pg_krb_server_keyfile,
						  key_sched,
						  version);
	if (status != KSUCCESS)
	{
		ereport(LOG,
				(errmsg("Kerberos error: %s", krb_err_txt[status])));
		return STATUS_ERROR;
	}
	if (strncmp(version, PG_KRB4_VERSION, KRB_SENDAUTH_VLEN) != 0)
	{
		ereport(LOG,
				(errmsg("unexpected Kerberos protocol version received from client (received \"%s\", expected \"%s\")",
						version, PG_KRB4_VERSION)));
		return STATUS_ERROR;
	}
	if (strncmp(port->user_name, auth_data.pname, SM_DATABASE_USER) != 0)
	{
		ereport(LOG,
				(errmsg("unexpected Kerberos user name received from client (received \"%s\", expected \"%s\")",
						port->user_name, auth_data.pname)));
		return STATUS_ERROR;
	}
	return STATUS_OK;
}

#else

static int
pg_krb4_recvauth(Port *port)
{
	ereport(LOG,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("Kerberos 4 not implemented on this server")));
	return STATUS_ERROR;
}
#endif   /* KRB4 */


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
static int	pg_krb5_initialised;
static krb5_context pg_krb5_context;
static krb5_keytab pg_krb5_keytab;
static krb5_principal pg_krb5_server;


static int
pg_krb5_init(void)
{
	krb5_error_code retval;

	if (pg_krb5_initialised)
		return STATUS_OK;

	retval = krb5_init_context(&pg_krb5_context);
	if (retval)
	{
		ereport(LOG,
				(errmsg("Kerberos initialization returned error %d",
						retval)));
		com_err("postgres", retval, "while initializing krb5");
		return STATUS_ERROR;
	}

	retval = krb5_kt_resolve(pg_krb5_context, pg_krb_server_keyfile, &pg_krb5_keytab);
	if (retval)
	{
		ereport(LOG,
				(errmsg("Kerberos keytab resolving returned error %d",
						retval)));
		com_err("postgres", retval, "while resolving keytab file \"%s\"",
				pg_krb_server_keyfile);
		krb5_free_context(pg_krb5_context);
		return STATUS_ERROR;
	}

	retval = krb5_sname_to_principal(pg_krb5_context, NULL, PG_KRB_SRVNAM,
									 KRB5_NT_SRV_HST, &pg_krb5_server);
	if (retval)
	{
		ereport(LOG,
		 (errmsg("Kerberos sname_to_principal(\"%s\") returned error %d",
				 PG_KRB_SRVNAM, retval)));
		com_err("postgres", retval,
				"while getting server principal for service \"%s\"",
				PG_KRB_SRVNAM);
		krb5_kt_close(pg_krb5_context, pg_krb5_keytab);
		krb5_free_context(pg_krb5_context);
		return STATUS_ERROR;
	}

	pg_krb5_initialised = 1;
	return STATUS_OK;
}


/*
 * pg_krb5_recvauth -- server routine to receive authentication information
 *					   from the client
 *
 * We still need to compare the username obtained from the client's setup
 * packet to the authenticated name, as described in pg_krb4_recvauth.	This
 * is a bit more problematic in v5, as described above in pg_an_to_ln.
 *
 * We have our own keytab file because postgres is unlikely to run as root,
 * and so cannot read the default keytab.
 */
static int
pg_krb5_recvauth(Port *port)
{
	krb5_error_code retval;
	int			ret;
	krb5_auth_context auth_context = NULL;
	krb5_ticket *ticket;
	char	   *kusername;

	ret = pg_krb5_init();
	if (ret != STATUS_OK)
		return ret;

	retval = krb5_recvauth(pg_krb5_context, &auth_context,
						   (krb5_pointer) & port->sock, PG_KRB_SRVNAM,
						   pg_krb5_server, 0, pg_krb5_keytab, &ticket);
	if (retval)
	{
		ereport(LOG,
				(errmsg("Kerberos recvauth returned error %d",
						retval)));
		com_err("postgres", retval, "from krb5_recvauth");
		return STATUS_ERROR;
	}

	/*
	 * The "client" structure comes out of the ticket and is therefore
	 * authenticated.  Use it to check the username obtained from the
	 * postmaster startup packet.
	 *
	 * I have no idea why this is considered necessary.
	 */
#if defined(HAVE_KRB5_TICKET_ENC_PART2)
	retval = krb5_unparse_name(pg_krb5_context,
							   ticket->enc_part2->client, &kusername);
#elif defined(HAVE_KRB5_TICKET_CLIENT)
	retval = krb5_unparse_name(pg_krb5_context,
							   ticket->client, &kusername);
#else
#error "bogus configuration"
#endif
	if (retval)
	{
		ereport(LOG,
				(errmsg("Kerberos unparse_name returned error %d",
						retval)));
		com_err("postgres", retval, "while unparsing client name");
		krb5_free_ticket(pg_krb5_context, ticket);
		krb5_auth_con_free(pg_krb5_context, auth_context);
		return STATUS_ERROR;
	}

	kusername = pg_an_to_ln(kusername);
	if (strncmp(port->user_name, kusername, SM_DATABASE_USER))
	{
		ereport(LOG,
				(errmsg("unexpected Kerberos user name received from client (received \"%s\", expected \"%s\")",
						port->user_name, kusername)));
		ret = STATUS_ERROR;
	}
	else
		ret = STATUS_OK;

	krb5_free_ticket(pg_krb5_context, ticket);
	krb5_auth_con_free(pg_krb5_context, auth_context);
	free(kusername);

	return ret;
}

#else

static int
pg_krb5_recvauth(Port *port)
{
	ereport(LOG,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("Kerberos 5 not implemented on this server")));
	return STATUS_ERROR;
}
#endif   /* KRB5 */


/*
 * Tell the user the authentication failed, but not (much about) why.
 *
 * There is a tradeoff here between security concerns and making life
 * unnecessarily difficult for legitimate users.  We would not, for example,
 * want to report the password we were expecting to receive...
 * But it seems useful to report the username and authorization method
 * in use, and these are items that must be presumed known to an attacker
 * anyway.
 * Note that many sorts of failure report additional information in the
 * postmaster log, which we hope is only readable by good guys.
 */
static void
auth_failed(Port *port, int status)
{
	const char *authmethod = "Unknown auth method:";

	/*
	 * If we failed due to EOF from client, just quit; there's no point in
	 * trying to send a message to the client, and not much point in
	 * logging the failure in the postmaster log.  (Logging the failure
	 * might be desirable, were it not for the fact that libpq closes the
	 * connection unceremoniously if challenged for a password when it
	 * hasn't got one to send.  We'll get a useless log entry for every
	 * psql connection under password auth, even if it's perfectly
	 * successful, if we log STATUS_EOF events.)
	 */
	if (status == STATUS_EOF)
		proc_exit(0);

	switch (port->auth_method)
	{
		case uaReject:
			authmethod = "Rejected host:";
			break;
		case uaKrb4:
			authmethod = "Kerberos4";
			break;
		case uaKrb5:
			authmethod = "Kerberos5";
			break;
		case uaTrust:
			authmethod = "Trusted";
			break;
		case uaIdent:
			authmethod = "IDENT";
			break;
		case uaMD5:
		case uaCrypt:
		case uaPassword:
			authmethod = "Password";
			break;
#ifdef USE_PAM
		case uaPAM:
			authmethod = "PAM";
			break;
#endif   /* USE_PAM */
	}

	ereport(FATAL,
			(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
			 errmsg("%s authentication failed for user \"%s\"",
					authmethod, port->user_name)));
	/* doesn't return */
}


/*
 * Client authentication starts here.  If there is an error, this
 * function does not return and the backend process is terminated.
 */
void
ClientAuthentication(Port *port)
{
	int			status = STATUS_ERROR;

	/*
	 * Get the authentication method to use for this frontend/database
	 * combination.  Note: a failure return indicates a problem with the
	 * hba config file, not with the request.  hba.c should have dropped
	 * an error message into the postmaster logfile if it failed.
	 */
	if (hba_getauthmethod(port) != STATUS_OK)
		ereport(FATAL,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("missing or erroneous pg_hba.conf file"),
				 errhint("See server log for details.")));

	switch (port->auth_method)
	{
		case uaReject:

			/*
			 * This could have come from an explicit "reject" entry in
			 * pg_hba.conf, but more likely it means there was no matching
			 * entry.  Take pity on the poor user and issue a helpful
			 * error message.  NOTE: this is not a security breach,
			 * because all the info reported here is known at the frontend
			 * and must be assumed known to bad guys. We're merely helping
			 * out the less clueful good guys.
			 */
			{
				char		hostinfo[NI_MAXHOST];

				getnameinfo_all(&port->raddr.addr, port->raddr.salen,
								hostinfo, sizeof(hostinfo),
								NULL, 0,
								NI_NUMERICHOST);

#ifdef USE_SSL
				ereport(FATAL,
				   (errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
					errmsg("no pg_hba.conf entry for host \"%s\", user \"%s\", database \"%s\", %s",
						   hostinfo, port->user_name, port->database_name,
				   port->ssl ? gettext("SSL on") : gettext("SSL off"))));
#else
				ereport(FATAL,
				   (errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
					errmsg("no pg_hba.conf entry for host \"%s\", user \"%s\", database \"%s\"",
					   hostinfo, port->user_name, port->database_name)));
#endif
				break;
			}

		case uaKrb4:
			/* Kerberos 4 only seems to work with AF_INET. */
			if (port->raddr.addr.ss_family != AF_INET
				|| port->laddr.addr.ss_family != AF_INET)
				ereport(FATAL,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				   errmsg("Kerberos 4 only supports IPv4 connections")));
			sendAuthRequest(port, AUTH_REQ_KRB4);
			status = pg_krb4_recvauth(port);
			break;

		case uaKrb5:
			sendAuthRequest(port, AUTH_REQ_KRB5);
			status = pg_krb5_recvauth(port);
			break;

		case uaIdent:
			/*
			 * If we are doing ident on unix-domain sockets, use SCM_CREDS
			 * only if it is defined and SO_PEERCRED isn't.
			 */
#if !defined(HAVE_GETPEEREID) && !defined(SO_PEERCRED) && \
	(defined(HAVE_STRUCT_CMSGCRED) || defined(HAVE_STRUCT_FCRED) || \
	 (defined(HAVE_STRUCT_SOCKCRED) && defined(LOCAL_CREDS)))
			if (port->raddr.addr.ss_family == AF_UNIX)
			{
#if defined(HAVE_STRUCT_FCRED) || defined(HAVE_STRUCT_SOCKCRED)
				/*
				 * Receive credentials on next message receipt, BSD/OS,
				 * NetBSD. We need to set this before the client sends the
				 * next packet.
				 */
				int			on = 1;

				if (setsockopt(port->sock, 0, LOCAL_CREDS, &on, sizeof(on)) < 0)
					ereport(FATAL,
							(errcode_for_socket_access(),
					 errmsg("could not enable credential reception: %m")));
#endif

				sendAuthRequest(port, AUTH_REQ_SCM_CREDS);
			}
#endif
			status = authident(port);
			break;

		case uaMD5:
			sendAuthRequest(port, AUTH_REQ_MD5);
			status = recv_and_check_password_packet(port);
			break;

		case uaCrypt:
			sendAuthRequest(port, AUTH_REQ_CRYPT);
			status = recv_and_check_password_packet(port);
			break;

		case uaPassword:
			sendAuthRequest(port, AUTH_REQ_PASSWORD);
			status = recv_and_check_password_packet(port);
			break;

#ifdef USE_PAM
		case uaPAM:
			pam_port_cludge = port;
			status = CheckPAMAuth(port, port->user_name, "");
			break;
#endif   /* USE_PAM */

		case uaTrust:
			status = STATUS_OK;
			break;
	}

	if (status == STATUS_OK)
		sendAuthRequest(port, AUTH_REQ_OK);
	else
		auth_failed(port, status);
}


/*
 * Send an authentication request packet to the frontend.
 */
static void
sendAuthRequest(Port *port, AuthRequest areq)
{
	StringInfoData buf;

	pq_beginmessage(&buf, 'R');
	pq_sendint(&buf, (int32) areq, sizeof(int32));

	/* Add the salt for encrypted passwords. */
	if (areq == AUTH_REQ_MD5)
		pq_sendbytes(&buf, port->md5Salt, 4);
	else if (areq == AUTH_REQ_CRYPT)
		pq_sendbytes(&buf, port->cryptSalt, 2);

	pq_endmessage(&buf);

	/*
	 * Flush message so client will see it, except for AUTH_REQ_OK, which
	 * need not be sent until we are ready for queries.
	 */
	if (areq != AUTH_REQ_OK)
		pq_flush();
}


#ifdef USE_PAM

/*
 * PAM conversation function
 */

static int
pam_passwd_conv_proc(int num_msg, const struct pam_message ** msg,
					 struct pam_response ** resp, void *appdata_ptr)
{
	if (num_msg != 1 || msg[0]->msg_style != PAM_PROMPT_ECHO_OFF)
	{
		switch (msg[0]->msg_style)
		{
			case PAM_ERROR_MSG:
				ereport(LOG,
						(errmsg("error from underlying PAM layer: %s",
								msg[0]->msg)));
				return PAM_CONV_ERR;
			default:
				ereport(LOG,
						(errmsg("unsupported PAM conversation %d/%s",
								msg[0]->msg_style, msg[0]->msg)));
				return PAM_CONV_ERR;
		}
	}

	if (!appdata_ptr)
	{
		/*
		 * Workaround for Solaris 2.6 where the PAM library is broken and
		 * does not pass appdata_ptr to the conversation routine
		 */
		appdata_ptr = pam_passwd;
	}

	/*
	 * Password wasn't passed to PAM the first time around - let's go ask
	 * the client to send a password, which we then stuff into PAM.
	 */
	if (strlen(appdata_ptr) == 0)
	{
		char	   *passwd;

		sendAuthRequest(pam_port_cludge, AUTH_REQ_PASSWORD);
		passwd = recv_password_packet(pam_port_cludge);

		if (passwd == NULL)
			return PAM_CONV_ERR;	/* client didn't want to send password */

		if (strlen(passwd) == 0)
		{
			ereport(LOG,
					(errmsg("empty password returned by client")));
			return PAM_CONV_ERR;
		}
		appdata_ptr = passwd;
	}

	/*
	 * Explicitly not using palloc here - PAM will free this memory in
	 * pam_end()
	 */
	*resp = calloc(num_msg, sizeof(struct pam_response));
	if (!*resp)
	{
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
		return PAM_CONV_ERR;
	}

	(*resp)[0].resp = strdup((char *) appdata_ptr);
	(*resp)[0].resp_retcode = 0;

	return ((*resp)[0].resp ? PAM_SUCCESS : PAM_CONV_ERR);
}


/*
 * Check authentication against PAM.
 */
static int
CheckPAMAuth(Port *port, char *user, char *password)
{
	int			retval;
	pam_handle_t *pamh = NULL;

	/*
	 * Apparently, Solaris 2.6 is broken, and needs ugly static variable
	 * workaround
	 */
	pam_passwd = password;

	/*
	 * Set the application data portion of the conversation struct This is
	 * later used inside the PAM conversation to pass the password to the
	 * authentication module.
	 */
	pam_passw_conv.appdata_ptr = (char *) password;		/* from password above,
														 * not allocated */

	/* Optionally, one can set the service name in pg_hba.conf */
	if (port->auth_arg && port->auth_arg[0] != '\0')
		retval = pam_start(port->auth_arg, "pgsql@",
						   &pam_passw_conv, &pamh);
	else
		retval = pam_start(PGSQL_PAM_SERVICE, "pgsql@",
						   &pam_passw_conv, &pamh);

	if (retval != PAM_SUCCESS)
	{
		ereport(LOG,
				(errmsg("could not create PAM authenticator: %s",
						pam_strerror(pamh, retval))));
		pam_passwd = NULL;		/* Unset pam_passwd */
		return STATUS_ERROR;
	}

	retval = pam_set_item(pamh, PAM_USER, user);

	if (retval != PAM_SUCCESS)
	{
		ereport(LOG,
				(errmsg("pam_set_item(PAM_USER) failed: %s",
						pam_strerror(pamh, retval))));
		pam_passwd = NULL;		/* Unset pam_passwd */
		return STATUS_ERROR;
	}

	retval = pam_set_item(pamh, PAM_CONV, &pam_passw_conv);

	if (retval != PAM_SUCCESS)
	{
		ereport(LOG,
				(errmsg("pam_set_item(PAM_CONV) failed: %s",
						pam_strerror(pamh, retval))));
		pam_passwd = NULL;		/* Unset pam_passwd */
		return STATUS_ERROR;
	}

	retval = pam_authenticate(pamh, 0);

	if (retval != PAM_SUCCESS)
	{
		ereport(LOG,
				(errmsg("pam_authenticate failed: %s",
						pam_strerror(pamh, retval))));
		pam_passwd = NULL;		/* Unset pam_passwd */
		return STATUS_ERROR;
	}

	retval = pam_acct_mgmt(pamh, 0);

	if (retval != PAM_SUCCESS)
	{
		ereport(LOG,
				(errmsg("pam_acct_mgmt failed: %s",
						pam_strerror(pamh, retval))));
		pam_passwd = NULL;		/* Unset pam_passwd */
		return STATUS_ERROR;
	}

	retval = pam_end(pamh, retval);

	if (retval != PAM_SUCCESS)
	{
		ereport(LOG,
				(errmsg("could not release PAM authenticator: %s",
						pam_strerror(pamh, retval))));
	}

	pam_passwd = NULL;			/* Unset pam_passwd */

	return (retval == PAM_SUCCESS ? STATUS_OK : STATUS_ERROR);
}
#endif   /* USE_PAM */


/*
 * Collect password response packet from frontend.
 *
 * Returns NULL if couldn't get password, else palloc'd string.
 */
static char *
recv_password_packet(Port *port)
{
	StringInfoData buf;

	if (PG_PROTOCOL_MAJOR(port->proto) >= 3)
	{
		/* Expect 'p' message type */
		int			mtype;

		mtype = pq_getbyte();
		if (mtype != 'p')
		{
			/*
			 * If the client just disconnects without offering a password,
			 * don't make a log entry.  This is legal per protocol spec
			 * and in fact commonly done by psql, so complaining just
			 * clutters the log.
			 */
			if (mtype != EOF)
				ereport(COMMERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
					errmsg("expected password response, got message type %d",
						   mtype)));
			return NULL;		/* EOF or bad message type */
		}
	}
	else
	{
		/* For pre-3.0 clients, avoid log entry if they just disconnect */
		if (pq_peekbyte() == EOF)
			return NULL;		/* EOF */
	}

	initStringInfo(&buf);
	if (pq_getmessage(&buf, 1000))		/* receive password */
	{
		/* EOF - pq_getmessage already logged a suitable message */
		pfree(buf.data);
		return NULL;
	}

	/*
	 * Apply sanity check: password packet length should agree with length
	 * of contained string.  Note it is safe to use strlen here because
	 * StringInfo is guaranteed to have an appended '\0'.
	 */
	if (strlen(buf.data) + 1 != buf.len)
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid password packet size")));

	/* Do not echo password to logs, for security. */
	ereport(DEBUG5,
			(errmsg("received password packet")));

	/*
	 * Return the received string.	Note we do not attempt to do any
	 * character-set conversion on it; since we don't yet know the
	 * client's encoding, there wouldn't be much point.
	 */
	return buf.data;
}


/*
 * Called when we have sent an authorization request for a password.
 * Get the response and check it.
 */
static int
recv_and_check_password_packet(Port *port)
{
	char	   *passwd;
	int			result;

	passwd = recv_password_packet(port);

	if (passwd == NULL)
		return STATUS_EOF;		/* client wouldn't send password */

	result = md5_crypt_verify(port, port->user_name, passwd);

	pfree(passwd);

	return result;
}
