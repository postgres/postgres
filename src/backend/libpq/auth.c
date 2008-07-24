/*-------------------------------------------------------------------------
 *
 * auth.c
 *	  Routines to handle network authentication
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/libpq/auth.c,v 1.146.2.1 2008/07/24 17:52:09 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/param.h>
#include <sys/socket.h>
#if defined(HAVE_STRUCT_CMSGCRED) || defined(HAVE_STRUCT_FCRED) || defined(HAVE_STRUCT_SOCKCRED)
#include <sys/uio.h>
#include <sys/ucred.h>
#endif
#include <netinet/in.h>
#include <arpa/inet.h>

#include "libpq/auth.h"
#include "libpq/crypt.h"
#include "libpq/ip.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "storage/ipc.h"


static void sendAuthRequest(Port *port, AuthRequest areq);
static void auth_failed(Port *port, int status);
static char *recv_password_packet(Port *port);
static int	recv_and_check_password_packet(Port *port);

char	   *pg_krb_server_keyfile;
char	   *pg_krb_srvnam;
bool		pg_krb_caseins_users;
char	   *pg_krb_server_hostname = NULL;

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
static Port *pam_port_cludge;	/* Workaround for passing "Port *port" into
								 * pam_passwd_conv_proc */
#endif   /* USE_PAM */

#ifdef USE_LDAP
#ifndef WIN32
/* We use a deprecated function to keep the codepath the same as win32. */
#define LDAP_DEPRECATED 1
#include <ldap.h>
#else
#include <winldap.h>

/* Correct header from the Platform SDK */
typedef
ULONG(*__ldap_start_tls_sA) (
							 IN PLDAP ExternalHandle,
							 OUT PULONG ServerReturnValue,
							 OUT LDAPMessage ** result,
							 IN PLDAPControlA * ServerControls,
							 IN PLDAPControlA * ClientControls
);
#endif

static int	CheckLDAPAuth(Port *port);
#endif


#ifdef KRB5
/*----------------------------------------------------------------
 * MIT Kerberos authentication system - protocol version 5
 *----------------------------------------------------------------
 */

#include <krb5.h>
/* Some old versions of Kerberos do not include <com_err.h> in <krb5.h> */
#if !defined(__COM_ERR_H) && !defined(__COM_ERR_H__)
#include <com_err.h>
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
	char	   *khostname;

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

	/*
	 * If no hostname was specified, pg_krb_server_hostname is already NULL.
	 * If it's set to blank, force it to NULL.
	 */
	khostname = pg_krb_server_hostname;
	if (khostname && khostname[0] == '\0')
		khostname = NULL;

	retval = krb5_sname_to_principal(pg_krb5_context,
									 khostname,
									 pg_krb_srvnam,
									 KRB5_NT_SRV_HST,
									 &pg_krb5_server);
	if (retval)
	{
		ereport(LOG,
				(errmsg("Kerberos sname_to_principal(\"%s\", \"%s\") returned error %d",
		 khostname ? khostname : "server hostname", pg_krb_srvnam, retval)));
		com_err("postgres", retval,
		"while getting server principal for server \"%s\" for service \"%s\"",
				khostname ? khostname : "server hostname", pg_krb_srvnam);
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
 * packet to the authenticated name.
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
						   (krb5_pointer) & port->sock, pg_krb_srvnam,
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
	if (pg_krb_caseins_users)
		ret = pg_strncasecmp(port->user_name, kusername, SM_DATABASE_USER);
	else
		ret = strncmp(port->user_name, kusername, SM_DATABASE_USER);
	if (ret)
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
	const char *errstr;

	/*
	 * If we failed due to EOF from client, just quit; there's no point in
	 * trying to send a message to the client, and not much point in logging
	 * the failure in the postmaster log.  (Logging the failure might be
	 * desirable, were it not for the fact that libpq closes the connection
	 * unceremoniously if challenged for a password when it hasn't got one to
	 * send.  We'll get a useless log entry for every psql connection under
	 * password auth, even if it's perfectly successful, if we log STATUS_EOF
	 * events.)
	 */
	if (status == STATUS_EOF)
		proc_exit(0);

	switch (port->auth_method)
	{
		case uaReject:
			errstr = gettext_noop("authentication failed for user \"%s\": host rejected");
			break;
		case uaKrb5:
			errstr = gettext_noop("Kerberos 5 authentication failed for user \"%s\"");
			break;
		case uaTrust:
			errstr = gettext_noop("\"trust\" authentication failed for user \"%s\"");
			break;
		case uaIdent:
			errstr = gettext_noop("Ident authentication failed for user \"%s\"");
			break;
		case uaMD5:
		case uaCrypt:
		case uaPassword:
			errstr = gettext_noop("password authentication failed for user \"%s\"");
			break;
#ifdef USE_PAM
		case uaPAM:
			errstr = gettext_noop("PAM authentication failed for user \"%s\"");
			break;
#endif   /* USE_PAM */
#ifdef USE_LDAP
		case uaLDAP:
			errstr = gettext_noop("LDAP authentication failed for user \"%s\"");
			break;
#endif   /* USE_LDAP */
		default:
			errstr = gettext_noop("authentication failed for user \"%s\": invalid authentication method");
			break;
	}

	ereport(FATAL,
			(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
			 errmsg(errstr, port->user_name)));
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
	 * combination.  Note: a failure return indicates a problem with the hba
	 * config file, not with the request.  hba.c should have dropped an error
	 * message into the postmaster logfile if it failed.
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
			 * entry.  Take pity on the poor user and issue a helpful error
			 * message.  NOTE: this is not a security breach, because all the
			 * info reported here is known at the frontend and must be assumed
			 * known to bad guys. We're merely helping out the less clueful
			 * good guys.
			 */
			{
				char		hostinfo[NI_MAXHOST];

				pg_getnameinfo_all(&port->raddr.addr, port->raddr.salen,
								   hostinfo, sizeof(hostinfo),
								   NULL, 0,
								   NI_NUMERICHOST);

#ifdef USE_SSL
				ereport(FATAL,
						(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
						 errmsg("no pg_hba.conf entry for host \"%s\", user \"%s\", database \"%s\", %s",
							  hostinfo, port->user_name, port->database_name,
								port->ssl ? _("SSL on") : _("SSL off"))));
#else
				ereport(FATAL,
						(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
						 errmsg("no pg_hba.conf entry for host \"%s\", user \"%s\", database \"%s\"",
						   hostinfo, port->user_name, port->database_name)));
#endif
				break;
			}

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

#ifdef USE_LDAP
		case uaLDAP:
			status = CheckLDAPAuth(port);
			break;
#endif

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
	 * Flush message so client will see it, except for AUTH_REQ_OK, which need
	 * not be sent until we are ready for queries.
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
		 * Workaround for Solaris 2.6 where the PAM library is broken and does
		 * not pass appdata_ptr to the conversation routine
		 */
		appdata_ptr = pam_passwd;
	}

	/*
	 * Password wasn't passed to PAM the first time around - let's go ask the
	 * client to send a password, which we then stuff into PAM.
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


#ifdef USE_LDAP

static int
CheckLDAPAuth(Port *port)
{
	char	   *passwd;
	char		server[128];
	char		basedn[128];
	char		prefix[128];
	char		suffix[128];
	LDAP	   *ldap;
	bool		ssl = false;
	int			r;
	int			ldapversion = LDAP_VERSION3;
	int			ldapport = LDAP_PORT;
	char		fulluser[NAMEDATALEN + 256 + 1];

	if (!port->auth_arg || port->auth_arg[0] == '\0')
	{
		ereport(LOG,
				(errmsg("LDAP configuration URL not specified")));
		return STATUS_ERROR;
	}

	/*
	 * Crack the LDAP url. We do a very trivial parse:
	 *
	 * ldap[s]://<server>[:<port>]/<basedn>[;prefix[;suffix]]
	 *
	 * This code originally used "%127s" for the suffix, but that doesn't
	 * work for embedded whitespace.  We know that tokens formed by
	 * hba.c won't include newlines, so we can use a "not newline" scanset
	 * instead.
	 */

	server[0] = '\0';
	basedn[0] = '\0';
	prefix[0] = '\0';
	suffix[0] = '\0';

	/* ldap, including port number */
	r = sscanf(port->auth_arg,
			   "ldap://%127[^:]:%i/%127[^;];%127[^;];%127[^\n]",
			   server, &ldapport, basedn, prefix, suffix);
	if (r < 3)
	{
		/* ldaps, including port number */
		r = sscanf(port->auth_arg,
				   "ldaps://%127[^:]:%i/%127[^;];%127[^;];%127[^\n]",
				   server, &ldapport, basedn, prefix, suffix);
		if (r >= 3)
			ssl = true;
	}
	if (r < 3)
	{
		/* ldap, no port number */
		r = sscanf(port->auth_arg,
				   "ldap://%127[^/]/%127[^;];%127[^;];%127[^\n]",
				   server, basedn, prefix, suffix);
	}
	if (r < 2)
	{
		/* ldaps, no port number */
		r = sscanf(port->auth_arg,
				   "ldaps://%127[^/]/%127[^;];%127[^;];%127[^\n]",
				   server, basedn, prefix, suffix);
		if (r >= 2)
			ssl = true;
	}
	if (r < 2)
	{
		ereport(LOG,
				(errmsg("invalid LDAP URL: \"%s\"",
						port->auth_arg)));
		return STATUS_ERROR;
	}

	sendAuthRequest(port, AUTH_REQ_PASSWORD);

	passwd = recv_password_packet(port);
	if (passwd == NULL)
		return STATUS_EOF;		/* client wouldn't send password */

	ldap = ldap_init(server, ldapport);
	if (!ldap)
	{
#ifndef WIN32
		ereport(LOG,
				(errmsg("could not initialize LDAP: error code %d",
						errno)));
#else
		ereport(LOG,
				(errmsg("could not initialize LDAP: error code %d",
						(int) LdapGetLastError())));
#endif
		return STATUS_ERROR;
	}

	if ((r = ldap_set_option(ldap, LDAP_OPT_PROTOCOL_VERSION, &ldapversion)) != LDAP_SUCCESS)
	{
		ldap_unbind(ldap);
		ereport(LOG,
				(errmsg("could not set LDAP protocol version: error code %d", r)));
		return STATUS_ERROR;
	}

	if (ssl)
	{
#ifndef WIN32
		if ((r = ldap_start_tls_s(ldap, NULL, NULL)) != LDAP_SUCCESS)
#else
		static __ldap_start_tls_sA _ldap_start_tls_sA = NULL;

		if (_ldap_start_tls_sA == NULL)
		{
			/*
			 * Need to load this function dynamically because it does not
			 * exist on Windows 2000, and causes a load error for the whole
			 * exe if referenced.
			 */
			HANDLE		ldaphandle;

			ldaphandle = LoadLibrary("WLDAP32.DLL");
			if (ldaphandle == NULL)
			{
				/*
				 * should never happen since we import other files from
				 * wldap32, but check anyway
				 */
				ldap_unbind(ldap);
				ereport(LOG,
						(errmsg("could not load wldap32.dll")));
				return STATUS_ERROR;
			}
			_ldap_start_tls_sA = (__ldap_start_tls_sA) GetProcAddress(ldaphandle, "ldap_start_tls_sA");
			if (_ldap_start_tls_sA == NULL)
			{
				ldap_unbind(ldap);
				ereport(LOG,
						(errmsg("could not load function _ldap_start_tls_sA in wldap32.dll"),
						 errdetail("LDAP over SSL is not supported on this platform.")));
				return STATUS_ERROR;
			}

			/*
			 * Leak LDAP handle on purpose, because we need the library to stay
			 * open. This is ok because it will only ever be leaked once per
			 * process and is automatically cleaned up on process exit.
			 */
		}
		if ((r = _ldap_start_tls_sA(ldap, NULL, NULL, NULL, NULL)) != LDAP_SUCCESS)
#endif
		{
			ldap_unbind(ldap);
			ereport(LOG,
					(errmsg("could not start LDAP TLS session: error code %d", r)));
			return STATUS_ERROR;
		}
	}

	snprintf(fulluser, sizeof(fulluser), "%s%s%s",
			 prefix, port->user_name, suffix);
	fulluser[sizeof(fulluser) - 1] = '\0';

	r = ldap_simple_bind_s(ldap, fulluser, passwd);
	ldap_unbind(ldap);

	if (r != LDAP_SUCCESS)
	{
		ereport(LOG,
				(errmsg("LDAP login failed for user \"%s\" on server \"%s\": error code %d",
						fulluser, server, r)));
		return STATUS_ERROR;
	}

	return STATUS_OK;
}
#endif   /* USE_LDAP */

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
			 * don't make a log entry.  This is legal per protocol spec and in
			 * fact commonly done by psql, so complaining just clutters the
			 * log.
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
	 * Apply sanity check: password packet length should agree with length of
	 * contained string.  Note it is safe to use strlen here because
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
	 * character-set conversion on it; since we don't yet know the client's
	 * encoding, there wouldn't be much point.
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
