/*-------------------------------------------------------------------------
 *
 * auth.c
 *	  Routines to handle network authentication
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/libpq/auth.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/param.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "libpq/auth.h"
#include "libpq/crypt.h"
#include "libpq/ip.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "libpq/md5.h"
#include "miscadmin.h"
#include "replication/walsender.h"
#include "storage/ipc.h"


/*----------------------------------------------------------------
 * Global authentication functions
 *----------------------------------------------------------------
 */
static void sendAuthRequest(Port *port, AuthRequest areq);
static void auth_failed(Port *port, int status, char *logdetail);
static char *recv_password_packet(Port *port);
static int	recv_and_check_password_packet(Port *port, char **logdetail);


/*----------------------------------------------------------------
 * Ident authentication
 *----------------------------------------------------------------
 */
/* Max size of username ident server can return */
#define IDENT_USERNAME_MAX 512

/* Standard TCP port number for Ident service.  Assigned by IANA */
#define IDENT_PORT 113

static int	ident_inet(hbaPort *port);

#ifdef HAVE_UNIX_SOCKETS
static int	auth_peer(hbaPort *port);
#endif


/*----------------------------------------------------------------
 * PAM authentication
 *----------------------------------------------------------------
 */
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


/*----------------------------------------------------------------
 * LDAP authentication
 *----------------------------------------------------------------
 */
#ifdef USE_LDAP
#ifndef WIN32
/* We use a deprecated function to keep the codepath the same as win32. */
#define LDAP_DEPRECATED 1
#include <ldap.h>
#else
#include <winldap.h>

/* Correct header from the Platform SDK */
typedef
ULONG		(*__ldap_start_tls_sA) (
												IN PLDAP ExternalHandle,
												OUT PULONG ServerReturnValue,
												OUT LDAPMessage **result,
										   IN PLDAPControlA * ServerControls,
											IN PLDAPControlA * ClientControls
);
#endif

static int	CheckLDAPAuth(Port *port);
#endif   /* USE_LDAP */

/*----------------------------------------------------------------
 * Cert authentication
 *----------------------------------------------------------------
 */
#ifdef USE_SSL
static int	CheckCertAuth(Port *port);
#endif


/*----------------------------------------------------------------
 * Kerberos and GSSAPI GUCs
 *----------------------------------------------------------------
 */
char	   *pg_krb_server_keyfile;
bool		pg_krb_caseins_users;


/*----------------------------------------------------------------
 * GSSAPI Authentication
 *----------------------------------------------------------------
 */
#ifdef ENABLE_GSS
#if defined(HAVE_GSSAPI_H)
#include <gssapi.h>
#else
#include <gssapi/gssapi.h>
#endif

static int	pg_GSS_recvauth(Port *port);
#endif   /* ENABLE_GSS */


/*----------------------------------------------------------------
 * SSPI Authentication
 *----------------------------------------------------------------
 */
#ifdef ENABLE_SSPI
typedef SECURITY_STATUS
			(WINAPI * QUERY_SECURITY_CONTEXT_TOKEN_FN) (
													   PCtxtHandle, void **);
static int	pg_SSPI_recvauth(Port *port);
#endif

/*----------------------------------------------------------------
 * RADIUS Authentication
 *----------------------------------------------------------------
 */
#ifdef USE_SSL
#include <openssl/rand.h>
#endif
static int	CheckRADIUSAuth(Port *port);


/*
 * Maximum accepted size of GSS and SSPI authentication tokens.
 *
 * Kerberos tickets are usually quite small, but the TGTs issued by Windows
 * domain controllers include an authorization field known as the Privilege
 * Attribute Certificate (PAC), which contains the user's Windows permissions
 * (group memberships etc.). The PAC is copied into all tickets obtained on
 * the basis of this TGT (even those issued by Unix realms which the Windows
 * realm trusts), and can be several kB in size. The maximum token size
 * accepted by Windows systems is determined by the MaxAuthToken Windows
 * registry setting. Microsoft recommends that it is not set higher than
 * 65535 bytes, so that seems like a reasonable limit for us as well.
 */
#define PG_MAX_AUTH_TOKEN_LENGTH	65535


/*----------------------------------------------------------------
 * Global authentication functions
 *----------------------------------------------------------------
 */

/*
 * This hook allows plugins to get control following client authentication,
 * but before the user has been informed about the results.  It could be used
 * to record login events, insert a delay after failed authentication, etc.
 */
ClientAuthentication_hook_type ClientAuthentication_hook = NULL;

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
 * postmaster log, which we hope is only readable by good guys.  In
 * particular, if logdetail isn't NULL, we send that string to the log.
 */
static void
auth_failed(Port *port, int status, char *logdetail)
{
	const char *errstr;
	char	   *cdetail;
	int			errcode_return = ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION;

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

	switch (port->hba->auth_method)
	{
		case uaReject:
		case uaImplicitReject:
			errstr = gettext_noop("authentication failed for user \"%s\": host rejected");
			break;
		case uaTrust:
			errstr = gettext_noop("\"trust\" authentication failed for user \"%s\"");
			break;
		case uaIdent:
			errstr = gettext_noop("Ident authentication failed for user \"%s\"");
			break;
		case uaPeer:
			errstr = gettext_noop("Peer authentication failed for user \"%s\"");
			break;
		case uaPassword:
		case uaMD5:
			errstr = gettext_noop("password authentication failed for user \"%s\"");
			/* We use it to indicate if a .pgpass password failed. */
			errcode_return = ERRCODE_INVALID_PASSWORD;
			break;
		case uaGSS:
			errstr = gettext_noop("GSSAPI authentication failed for user \"%s\"");
			break;
		case uaSSPI:
			errstr = gettext_noop("SSPI authentication failed for user \"%s\"");
			break;
		case uaPAM:
			errstr = gettext_noop("PAM authentication failed for user \"%s\"");
			break;
		case uaLDAP:
			errstr = gettext_noop("LDAP authentication failed for user \"%s\"");
			break;
		case uaCert:
			errstr = gettext_noop("certificate authentication failed for user \"%s\"");
			break;
		case uaRADIUS:
			errstr = gettext_noop("RADIUS authentication failed for user \"%s\"");
			break;
		default:
			errstr = gettext_noop("authentication failed for user \"%s\": invalid authentication method");
			break;
	}

	cdetail = psprintf(_("Connection matched pg_hba.conf line %d: \"%s\""),
					   port->hba->linenumber, port->hba->rawline);
	if (logdetail)
		logdetail = psprintf("%s\n%s", logdetail, cdetail);
	else
		logdetail = cdetail;

	ereport(FATAL,
			(errcode(errcode_return),
			 errmsg(errstr, port->user_name),
			 logdetail ? errdetail_log("%s", logdetail) : 0));

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
	char	   *logdetail = NULL;

	/*
	 * Get the authentication method to use for this frontend/database
	 * combination.  Note: we do not parse the file at this point; this has
	 * already been done elsewhere.  hba.c dropped an error message into the
	 * server logfile if parsing the hba config file failed.
	 */
	hba_getauthmethod(port);

	/*
	 * Enable immediate response to SIGTERM/SIGINT/timeout interrupts. (We
	 * don't want this during hba_getauthmethod() because it might have to do
	 * database access, eg for role membership checks.)
	 */
	ImmediateInterruptOK = true;
	/* And don't forget to detect one that already arrived */
	CHECK_FOR_INTERRUPTS();

	/*
	 * This is the first point where we have access to the hba record for the
	 * current connection, so perform any verifications based on the hba
	 * options field that should be done *before* the authentication here.
	 */
	if (port->hba->clientcert)
	{
		/*
		 * When we parse pg_hba.conf, we have already made sure that we have
		 * been able to load a certificate store. Thus, if a certificate is
		 * present on the client, it has been verified against our root
		 * certificate store, and the connection would have been aborted
		 * already if it didn't verify ok.
		 */
#ifdef USE_SSL
		if (!port->peer)
		{
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
				  errmsg("connection requires a valid client certificate")));
		}
#else

		/*
		 * hba.c makes sure hba->clientcert can't be set unless OpenSSL is
		 * present.
		 */
		Assert(false);
#endif
	}

	/*
	 * Now proceed to do the actual authentication check
	 */
	switch (port->hba->auth_method)
	{
		case uaReject:

			/*
			 * An explicit "reject" entry in pg_hba.conf.  This report exposes
			 * the fact that there's an explicit reject entry, which is
			 * perhaps not so desirable from a security standpoint; but the
			 * message for an implicit reject could confuse the DBA a lot when
			 * the true situation is a match to an explicit reject.  And we
			 * don't want to change the message for an implicit reject.  As
			 * noted below, the additional information shown here doesn't
			 * expose anything not known to an attacker.
			 */
			{
				char		hostinfo[NI_MAXHOST];

				pg_getnameinfo_all(&port->raddr.addr, port->raddr.salen,
								   hostinfo, sizeof(hostinfo),
								   NULL, 0,
								   NI_NUMERICHOST);

				if (am_walsender)
				{
#ifdef USE_SSL
					ereport(FATAL,
					   (errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
						errmsg("pg_hba.conf rejects replication connection for host \"%s\", user \"%s\", %s",
							   hostinfo, port->user_name,
							   port->ssl ? _("SSL on") : _("SSL off"))));
#else
					ereport(FATAL,
					   (errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
						errmsg("pg_hba.conf rejects replication connection for host \"%s\", user \"%s\"",
							   hostinfo, port->user_name)));
#endif
				}
				else
				{
#ifdef USE_SSL
					ereport(FATAL,
					   (errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
						errmsg("pg_hba.conf rejects connection for host \"%s\", user \"%s\", database \"%s\", %s",
							   hostinfo, port->user_name,
							   port->database_name,
							   port->ssl ? _("SSL on") : _("SSL off"))));
#else
					ereport(FATAL,
					   (errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
						errmsg("pg_hba.conf rejects connection for host \"%s\", user \"%s\", database \"%s\"",
							   hostinfo, port->user_name,
							   port->database_name)));
#endif
				}
				break;
			}

		case uaImplicitReject:

			/*
			 * No matching entry, so tell the user we fell through.
			 *
			 * NOTE: the extra info reported here is not a security breach,
			 * because all that info is known at the frontend and must be
			 * assumed known to bad guys.  We're merely helping out the less
			 * clueful good guys.
			 */
			{
				char		hostinfo[NI_MAXHOST];

				pg_getnameinfo_all(&port->raddr.addr, port->raddr.salen,
								   hostinfo, sizeof(hostinfo),
								   NULL, 0,
								   NI_NUMERICHOST);

#define HOSTNAME_LOOKUP_DETAIL(port) \
				(port->remote_hostname ? \
				 (port->remote_hostname_resolv == +1 ? \
				  errdetail_log("Client IP address resolved to \"%s\", forward lookup matches.", \
								port->remote_hostname) : \
				  port->remote_hostname_resolv == 0 ? \
				  errdetail_log("Client IP address resolved to \"%s\", forward lookup not checked.", \
								port->remote_hostname) : \
				  port->remote_hostname_resolv == -1 ? \
				  errdetail_log("Client IP address resolved to \"%s\", forward lookup does not match.", \
								port->remote_hostname) : \
				  port->remote_hostname_resolv == -2 ? \
				  errdetail_log("Could not translate client host name \"%s\" to IP address: %s.", \
								port->remote_hostname, \
								gai_strerror(port->remote_hostname_errcode)) : \
				  0) \
				 : (port->remote_hostname_resolv == -2 ? \
					errdetail_log("Could not resolve client IP address to a host name: %s.", \
								  gai_strerror(port->remote_hostname_errcode)) : \
					0))

				if (am_walsender)
				{
#ifdef USE_SSL
					ereport(FATAL,
					   (errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
						errmsg("no pg_hba.conf entry for replication connection from host \"%s\", user \"%s\", %s",
							   hostinfo, port->user_name,
							   port->ssl ? _("SSL on") : _("SSL off")),
						HOSTNAME_LOOKUP_DETAIL(port)));
#else
					ereport(FATAL,
					   (errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
						errmsg("no pg_hba.conf entry for replication connection from host \"%s\", user \"%s\"",
							   hostinfo, port->user_name),
						HOSTNAME_LOOKUP_DETAIL(port)));
#endif
				}
				else
				{
#ifdef USE_SSL
					ereport(FATAL,
					   (errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
						errmsg("no pg_hba.conf entry for host \"%s\", user \"%s\", database \"%s\", %s",
							   hostinfo, port->user_name,
							   port->database_name,
							   port->ssl ? _("SSL on") : _("SSL off")),
						HOSTNAME_LOOKUP_DETAIL(port)));
#else
					ereport(FATAL,
					   (errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
						errmsg("no pg_hba.conf entry for host \"%s\", user \"%s\", database \"%s\"",
							   hostinfo, port->user_name,
							   port->database_name),
						HOSTNAME_LOOKUP_DETAIL(port)));
#endif
				}
				break;
			}

		case uaGSS:
#ifdef ENABLE_GSS
			sendAuthRequest(port, AUTH_REQ_GSS);
			status = pg_GSS_recvauth(port);
#else
			Assert(false);
#endif
			break;

		case uaSSPI:
#ifdef ENABLE_SSPI
			sendAuthRequest(port, AUTH_REQ_SSPI);
			status = pg_SSPI_recvauth(port);
#else
			Assert(false);
#endif
			break;

		case uaPeer:
#ifdef HAVE_UNIX_SOCKETS
			status = auth_peer(port);
#else
			Assert(false);
#endif
			break;

		case uaIdent:
			status = ident_inet(port);
			break;

		case uaMD5:
			if (Db_user_namespace)
				ereport(FATAL,
						(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
						 errmsg("MD5 authentication is not supported when \"db_user_namespace\" is enabled")));
			sendAuthRequest(port, AUTH_REQ_MD5);
			status = recv_and_check_password_packet(port, &logdetail);
			break;

		case uaPassword:
			sendAuthRequest(port, AUTH_REQ_PASSWORD);
			status = recv_and_check_password_packet(port, &logdetail);
			break;

		case uaPAM:
#ifdef USE_PAM
			status = CheckPAMAuth(port, port->user_name, "");
#else
			Assert(false);
#endif   /* USE_PAM */
			break;

		case uaLDAP:
#ifdef USE_LDAP
			status = CheckLDAPAuth(port);
#else
			Assert(false);
#endif
			break;

		case uaCert:
#ifdef USE_SSL
			status = CheckCertAuth(port);
#else
			Assert(false);
#endif
			break;
		case uaRADIUS:
			status = CheckRADIUSAuth(port);
			break;
		case uaTrust:
			status = STATUS_OK;
			break;
	}

	if (ClientAuthentication_hook)
		(*ClientAuthentication_hook) (port, status);

	if (status == STATUS_OK)
		sendAuthRequest(port, AUTH_REQ_OK);
	else
		auth_failed(port, status, logdetail);

	/* Done with authentication, so we should turn off immediate interrupts */
	ImmediateInterruptOK = false;
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

#if defined(ENABLE_GSS) || defined(ENABLE_SSPI)

	/*
	 * Add the authentication data for the next step of the GSSAPI or SSPI
	 * negotiation.
	 */
	else if (areq == AUTH_REQ_GSS_CONT)
	{
		if (port->gss->outbuf.length > 0)
		{
			elog(DEBUG4, "sending GSS token of length %u",
				 (unsigned int) port->gss->outbuf.length);

			pq_sendbytes(&buf, port->gss->outbuf.value, port->gss->outbuf.length);
		}
	}
#endif

	pq_endmessage(&buf);

	/*
	 * Flush message so client will see it, except for AUTH_REQ_OK, which need
	 * not be sent until we are ready for queries.
	 */
	if (areq != AUTH_REQ_OK)
		pq_flush();
}

/*
 * Collect password response packet from frontend.
 *
 * Returns NULL if couldn't get password, else palloc'd string.
 */
static char *
recv_password_packet(Port *port)
{
	StringInfoData buf;

	pq_startmsgread();
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
	 * Return the received string.  Note we do not attempt to do any
	 * character-set conversion on it; since we don't yet know the client's
	 * encoding, there wouldn't be much point.
	 */
	return buf.data;
}


/*----------------------------------------------------------------
 * MD5 authentication
 *----------------------------------------------------------------
 */

/*
 * Called when we have sent an authorization request for a password.
 * Get the response and check it.
 * On error, optionally store a detail string at *logdetail.
 */
static int
recv_and_check_password_packet(Port *port, char **logdetail)
{
	char	   *passwd;
	int			result;

	passwd = recv_password_packet(port);

	if (passwd == NULL)
		return STATUS_EOF;		/* client wouldn't send password */

	result = md5_crypt_verify(port, port->user_name, passwd, logdetail);

	pfree(passwd);

	return result;
}



/*----------------------------------------------------------------
 * GSSAPI authentication system
 *----------------------------------------------------------------
 */
#ifdef ENABLE_GSS

#if defined(WIN32) && !defined(WIN32_ONLY_COMPILER)
/*
 * MIT Kerberos GSSAPI DLL doesn't properly export the symbols for MingW
 * that contain the OIDs required. Redefine here, values copied
 * from src/athena/auth/krb5/src/lib/gssapi/generic/gssapi_generic.c
 */
static const gss_OID_desc GSS_C_NT_USER_NAME_desc =
{10, (void *) "\x2a\x86\x48\x86\xf7\x12\x01\x02\x01\x02"};
static GSS_DLLIMP gss_OID GSS_C_NT_USER_NAME = &GSS_C_NT_USER_NAME_desc;
#endif


static void
pg_GSS_error(int severity, char *errmsg, OM_uint32 maj_stat, OM_uint32 min_stat)
{
	gss_buffer_desc gmsg;
	OM_uint32	lmin_s,
				msg_ctx;
	char		msg_major[128],
				msg_minor[128];

	/* Fetch major status message */
	msg_ctx = 0;
	gss_display_status(&lmin_s, maj_stat, GSS_C_GSS_CODE,
					   GSS_C_NO_OID, &msg_ctx, &gmsg);
	strlcpy(msg_major, gmsg.value, sizeof(msg_major));
	gss_release_buffer(&lmin_s, &gmsg);

	if (msg_ctx)

		/*
		 * More than one message available. XXX: Should we loop and read all
		 * messages? (same below)
		 */
		ereport(WARNING,
				(errmsg_internal("incomplete GSS error report")));

	/* Fetch mechanism minor status message */
	msg_ctx = 0;
	gss_display_status(&lmin_s, min_stat, GSS_C_MECH_CODE,
					   GSS_C_NO_OID, &msg_ctx, &gmsg);
	strlcpy(msg_minor, gmsg.value, sizeof(msg_minor));
	gss_release_buffer(&lmin_s, &gmsg);

	if (msg_ctx)
		ereport(WARNING,
				(errmsg_internal("incomplete GSS minor error report")));

	/*
	 * errmsg_internal, since translation of the first part must be done
	 * before calling this function anyway.
	 */
	ereport(severity,
			(errmsg_internal("%s", errmsg),
			 errdetail_internal("%s: %s", msg_major, msg_minor)));
}

static int
pg_GSS_recvauth(Port *port)
{
	OM_uint32	maj_stat,
				min_stat,
				lmin_s,
				gflags;
	int			mtype;
	int			ret;
	StringInfoData buf;
	gss_buffer_desc gbuf;

	/*
	 * GSS auth is not supported for protocol versions before 3, because it
	 * relies on the overall message length word to determine the GSS payload
	 * size in AuthenticationGSSContinue and PasswordMessage messages. (This
	 * is, in fact, a design error in our GSS support, because protocol
	 * messages are supposed to be parsable without relying on the length
	 * word; but it's not worth changing it now.)
	 */
	if (PG_PROTOCOL_MAJOR(FrontendProtocol) < 3)
		ereport(FATAL,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("GSSAPI is not supported in protocol version 2")));

	if (pg_krb_server_keyfile && strlen(pg_krb_server_keyfile) > 0)
	{
		/*
		 * Set default Kerberos keytab file for the Krb5 mechanism.
		 *
		 * setenv("KRB5_KTNAME", pg_krb_server_keyfile, 0); except setenv()
		 * not always available.
		 */
		if (getenv("KRB5_KTNAME") == NULL)
		{
			size_t		kt_len = strlen(pg_krb_server_keyfile) + 14;
			char	   *kt_path = malloc(kt_len);

			if (!kt_path)
			{
				ereport(LOG,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory")));
				return STATUS_ERROR;
			}
			snprintf(kt_path, kt_len, "KRB5_KTNAME=%s", pg_krb_server_keyfile);
			putenv(kt_path);
		}
	}

	/*
	 * We accept any service principal that's present in our keytab. This
	 * increases interoperability between kerberos implementations that see
	 * for example case sensitivity differently, while not really opening up
	 * any vector of attack.
	 */
	port->gss->cred = GSS_C_NO_CREDENTIAL;

	/*
	 * Initialize sequence with an empty context
	 */
	port->gss->ctx = GSS_C_NO_CONTEXT;

	/*
	 * Loop through GSSAPI message exchange. This exchange can consist of
	 * multiple messags sent in both directions. First message is always from
	 * the client. All messages from client to server are password packets
	 * (type 'p').
	 */
	do
	{
		pq_startmsgread();
		mtype = pq_getbyte();
		if (mtype != 'p')
		{
			/* Only log error if client didn't disconnect. */
			if (mtype != EOF)
				ereport(COMMERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("expected GSS response, got message type %d",
								mtype)));
			return STATUS_ERROR;
		}

		/* Get the actual GSS token */
		initStringInfo(&buf);
		if (pq_getmessage(&buf, PG_MAX_AUTH_TOKEN_LENGTH))
		{
			/* EOF - pq_getmessage already logged error */
			pfree(buf.data);
			return STATUS_ERROR;
		}

		/* Map to GSSAPI style buffer */
		gbuf.length = buf.len;
		gbuf.value = buf.data;

		elog(DEBUG4, "Processing received GSS token of length %u",
			 (unsigned int) gbuf.length);

		maj_stat = gss_accept_sec_context(
										  &min_stat,
										  &port->gss->ctx,
										  port->gss->cred,
										  &gbuf,
										  GSS_C_NO_CHANNEL_BINDINGS,
										  &port->gss->name,
										  NULL,
										  &port->gss->outbuf,
										  &gflags,
										  NULL,
										  NULL);

		/* gbuf no longer used */
		pfree(buf.data);

		elog(DEBUG5, "gss_accept_sec_context major: %d, "
			 "minor: %d, outlen: %u, outflags: %x",
			 maj_stat, min_stat,
			 (unsigned int) port->gss->outbuf.length, gflags);

		if (port->gss->outbuf.length != 0)
		{
			/*
			 * Negotiation generated data to be sent to the client.
			 */
			elog(DEBUG4, "sending GSS response token of length %u",
				 (unsigned int) port->gss->outbuf.length);

			sendAuthRequest(port, AUTH_REQ_GSS_CONT);

			gss_release_buffer(&lmin_s, &port->gss->outbuf);
		}

		if (maj_stat != GSS_S_COMPLETE && maj_stat != GSS_S_CONTINUE_NEEDED)
		{
			gss_delete_sec_context(&lmin_s, &port->gss->ctx, GSS_C_NO_BUFFER);
			pg_GSS_error(ERROR,
					   gettext_noop("accepting GSS security context failed"),
						 maj_stat, min_stat);
		}

		if (maj_stat == GSS_S_CONTINUE_NEEDED)
			elog(DEBUG4, "GSS continue needed");

	} while (maj_stat == GSS_S_CONTINUE_NEEDED);

	if (port->gss->cred != GSS_C_NO_CREDENTIAL)
	{
		/*
		 * Release service principal credentials
		 */
		gss_release_cred(&min_stat, &port->gss->cred);
	}

	/*
	 * GSS_S_COMPLETE indicates that authentication is now complete.
	 *
	 * Get the name of the user that authenticated, and compare it to the pg
	 * username that was specified for the connection.
	 */
	maj_stat = gss_display_name(&min_stat, port->gss->name, &gbuf, NULL);
	if (maj_stat != GSS_S_COMPLETE)
		pg_GSS_error(ERROR,
					 gettext_noop("retrieving GSS user name failed"),
					 maj_stat, min_stat);

	/*
	 * Split the username at the realm separator
	 */
	if (strchr(gbuf.value, '@'))
	{
		char	   *cp = strchr(gbuf.value, '@');

		/*
		 * If we are not going to include the realm in the username that is
		 * passed to the ident map, destructively modify it here to remove the
		 * realm. Then advance past the separator to check the realm.
		 */
		if (!port->hba->include_realm)
			*cp = '\0';
		cp++;

		if (port->hba->krb_realm != NULL && strlen(port->hba->krb_realm))
		{
			/*
			 * Match the realm part of the name first
			 */
			if (pg_krb_caseins_users)
				ret = pg_strcasecmp(port->hba->krb_realm, cp);
			else
				ret = strcmp(port->hba->krb_realm, cp);

			if (ret)
			{
				/* GSS realm does not match */
				elog(DEBUG2,
				   "GSSAPI realm (%s) and configured realm (%s) don't match",
					 cp, port->hba->krb_realm);
				gss_release_buffer(&lmin_s, &gbuf);
				return STATUS_ERROR;
			}
		}
	}
	else if (port->hba->krb_realm && strlen(port->hba->krb_realm))
	{
		elog(DEBUG2,
			 "GSSAPI did not return realm but realm matching was requested");

		gss_release_buffer(&lmin_s, &gbuf);
		return STATUS_ERROR;
	}

	ret = check_usermap(port->hba->usermap, port->user_name, gbuf.value,
						pg_krb_caseins_users);

	gss_release_buffer(&lmin_s, &gbuf);

	return ret;
}
#endif   /* ENABLE_GSS */


/*----------------------------------------------------------------
 * SSPI authentication system
 *----------------------------------------------------------------
 */
#ifdef ENABLE_SSPI
static void
pg_SSPI_error(int severity, const char *errmsg, SECURITY_STATUS r)
{
	char		sysmsg[256];

	if (FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, NULL, r, 0,
					  sysmsg, sizeof(sysmsg), NULL) == 0)
		ereport(severity,
				(errmsg_internal("%s", errmsg),
				 errdetail_internal("SSPI error %x", (unsigned int) r)));
	else
		ereport(severity,
				(errmsg_internal("%s", errmsg),
				 errdetail_internal("%s (%x)", sysmsg, (unsigned int) r)));
}

static int
pg_SSPI_recvauth(Port *port)
{
	int			mtype;
	StringInfoData buf;
	SECURITY_STATUS r;
	CredHandle	sspicred;
	CtxtHandle *sspictx = NULL,
				newctx;
	TimeStamp	expiry;
	ULONG		contextattr;
	SecBufferDesc inbuf;
	SecBufferDesc outbuf;
	SecBuffer	OutBuffers[1];
	SecBuffer	InBuffers[1];
	HANDLE		token;
	TOKEN_USER *tokenuser;
	DWORD		retlen;
	char		accountname[MAXPGPATH];
	char		domainname[MAXPGPATH];
	DWORD		accountnamesize = sizeof(accountname);
	DWORD		domainnamesize = sizeof(domainname);
	SID_NAME_USE accountnameuse;
	HMODULE		secur32;
	QUERY_SECURITY_CONTEXT_TOKEN_FN _QuerySecurityContextToken;

	/*
	 * SSPI auth is not supported for protocol versions before 3, because it
	 * relies on the overall message length word to determine the SSPI payload
	 * size in AuthenticationGSSContinue and PasswordMessage messages. (This
	 * is, in fact, a design error in our SSPI support, because protocol
	 * messages are supposed to be parsable without relying on the length
	 * word; but it's not worth changing it now.)
	 */
	if (PG_PROTOCOL_MAJOR(FrontendProtocol) < 3)
		ereport(FATAL,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("SSPI is not supported in protocol version 2")));

	/*
	 * Acquire a handle to the server credentials.
	 */
	r = AcquireCredentialsHandle(NULL,
								 "negotiate",
								 SECPKG_CRED_INBOUND,
								 NULL,
								 NULL,
								 NULL,
								 NULL,
								 &sspicred,
								 &expiry);
	if (r != SEC_E_OK)
		pg_SSPI_error(ERROR, _("could not acquire SSPI credentials"), r);

	/*
	 * Loop through SSPI message exchange. This exchange can consist of
	 * multiple messags sent in both directions. First message is always from
	 * the client. All messages from client to server are password packets
	 * (type 'p').
	 */
	do
	{
		pq_startmsgread();
		mtype = pq_getbyte();
		if (mtype != 'p')
		{
			/* Only log error if client didn't disconnect. */
			if (mtype != EOF)
				ereport(COMMERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("expected SSPI response, got message type %d",
								mtype)));
			return STATUS_ERROR;
		}

		/* Get the actual SSPI token */
		initStringInfo(&buf);
		if (pq_getmessage(&buf, PG_MAX_AUTH_TOKEN_LENGTH))
		{
			/* EOF - pq_getmessage already logged error */
			pfree(buf.data);
			return STATUS_ERROR;
		}

		/* Map to SSPI style buffer */
		inbuf.ulVersion = SECBUFFER_VERSION;
		inbuf.cBuffers = 1;
		inbuf.pBuffers = InBuffers;
		InBuffers[0].pvBuffer = buf.data;
		InBuffers[0].cbBuffer = buf.len;
		InBuffers[0].BufferType = SECBUFFER_TOKEN;

		/* Prepare output buffer */
		OutBuffers[0].pvBuffer = NULL;
		OutBuffers[0].BufferType = SECBUFFER_TOKEN;
		OutBuffers[0].cbBuffer = 0;
		outbuf.cBuffers = 1;
		outbuf.pBuffers = OutBuffers;
		outbuf.ulVersion = SECBUFFER_VERSION;


		elog(DEBUG4, "Processing received SSPI token of length %u",
			 (unsigned int) buf.len);

		r = AcceptSecurityContext(&sspicred,
								  sspictx,
								  &inbuf,
								  ASC_REQ_ALLOCATE_MEMORY,
								  SECURITY_NETWORK_DREP,
								  &newctx,
								  &outbuf,
								  &contextattr,
								  NULL);

		/* input buffer no longer used */
		pfree(buf.data);

		if (outbuf.cBuffers > 0 && outbuf.pBuffers[0].cbBuffer > 0)
		{
			/*
			 * Negotiation generated data to be sent to the client.
			 */
			elog(DEBUG4, "sending SSPI response token of length %u",
				 (unsigned int) outbuf.pBuffers[0].cbBuffer);

			port->gss->outbuf.length = outbuf.pBuffers[0].cbBuffer;
			port->gss->outbuf.value = outbuf.pBuffers[0].pvBuffer;

			sendAuthRequest(port, AUTH_REQ_GSS_CONT);

			FreeContextBuffer(outbuf.pBuffers[0].pvBuffer);
		}

		if (r != SEC_E_OK && r != SEC_I_CONTINUE_NEEDED)
		{
			if (sspictx != NULL)
			{
				DeleteSecurityContext(sspictx);
				free(sspictx);
			}
			FreeCredentialsHandle(&sspicred);
			pg_SSPI_error(ERROR,
						  _("could not accept SSPI security context"), r);
		}

		/*
		 * Overwrite the current context with the one we just received. If
		 * sspictx is NULL it was the first loop and we need to allocate a
		 * buffer for it. On subsequent runs, we can just overwrite the buffer
		 * contents since the size does not change.
		 */
		if (sspictx == NULL)
		{
			sspictx = malloc(sizeof(CtxtHandle));
			if (sspictx == NULL)
				ereport(ERROR,
						(errmsg("out of memory")));
		}

		memcpy(sspictx, &newctx, sizeof(CtxtHandle));

		if (r == SEC_I_CONTINUE_NEEDED)
			elog(DEBUG4, "SSPI continue needed");

	} while (r == SEC_I_CONTINUE_NEEDED);


	/*
	 * Release service principal credentials
	 */
	FreeCredentialsHandle(&sspicred);


	/*
	 * SEC_E_OK indicates that authentication is now complete.
	 *
	 * Get the name of the user that authenticated, and compare it to the pg
	 * username that was specified for the connection.
	 *
	 * MingW is missing the export for QuerySecurityContextToken in the
	 * secur32 library, so we have to load it dynamically.
	 */

	secur32 = LoadLibrary("SECUR32.DLL");
	if (secur32 == NULL)
		ereport(ERROR,
				(errmsg_internal("could not load secur32.dll: error code %lu",
								 GetLastError())));

	_QuerySecurityContextToken = (QUERY_SECURITY_CONTEXT_TOKEN_FN)
		GetProcAddress(secur32, "QuerySecurityContextToken");
	if (_QuerySecurityContextToken == NULL)
	{
		FreeLibrary(secur32);
		ereport(ERROR,
				(errmsg_internal("could not locate QuerySecurityContextToken in secur32.dll: error code %lu",
								 GetLastError())));
	}

	r = (_QuerySecurityContextToken) (sspictx, &token);
	if (r != SEC_E_OK)
	{
		FreeLibrary(secur32);
		pg_SSPI_error(ERROR,
					  _("could not get token from SSPI security context"), r);
	}

	FreeLibrary(secur32);

	/*
	 * No longer need the security context, everything from here on uses the
	 * token instead.
	 */
	DeleteSecurityContext(sspictx);
	free(sspictx);

	if (!GetTokenInformation(token, TokenUser, NULL, 0, &retlen) && GetLastError() != 122)
		ereport(ERROR,
			(errmsg_internal("could not get token user size: error code %lu",
							 GetLastError())));

	tokenuser = malloc(retlen);
	if (tokenuser == NULL)
		ereport(ERROR,
				(errmsg("out of memory")));

	if (!GetTokenInformation(token, TokenUser, tokenuser, retlen, &retlen))
		ereport(ERROR,
				(errmsg_internal("could not get user token: error code %lu",
								 GetLastError())));

	if (!LookupAccountSid(NULL, tokenuser->User.Sid, accountname, &accountnamesize,
						  domainname, &domainnamesize, &accountnameuse))
		ereport(ERROR,
			(errmsg_internal("could not look up account SID: error code %lu",
							 GetLastError())));

	free(tokenuser);

	/*
	 * Compare realm/domain if requested. In SSPI, always compare case
	 * insensitive.
	 */
	if (port->hba->krb_realm && strlen(port->hba->krb_realm))
	{
		if (pg_strcasecmp(port->hba->krb_realm, domainname) != 0)
		{
			elog(DEBUG2,
				 "SSPI domain (%s) and configured domain (%s) don't match",
				 domainname, port->hba->krb_realm);

			return STATUS_ERROR;
		}
	}

	/*
	 * We have the username (without domain/realm) in accountname, compare to
	 * the supplied value. In SSPI, always compare case insensitive.
	 *
	 * If set to include realm, append it in <username>@<realm> format.
	 */
	if (port->hba->include_realm)
	{
		char	   *namebuf;
		int			retval;

		namebuf = psprintf("%s@%s", accountname, domainname);
		retval = check_usermap(port->hba->usermap, port->user_name, namebuf, true);
		pfree(namebuf);
		return retval;
	}
	else
		return check_usermap(port->hba->usermap, port->user_name, accountname, true);
}
#endif   /* ENABLE_SSPI */



/*----------------------------------------------------------------
 * Ident authentication system
 *----------------------------------------------------------------
 */

/*
 *	Parse the string "*ident_response" as a response from a query to an Ident
 *	server.  If it's a normal response indicating a user name, return true
 *	and store the user name at *ident_user. If it's anything else,
 *	return false.
 */
static bool
interpret_ident_response(const char *ident_response,
						 char *ident_user)
{
	const char *cursor = ident_response;		/* Cursor into *ident_response */

	/*
	 * Ident's response, in the telnet tradition, should end in crlf (\r\n).
	 */
	if (strlen(ident_response) < 2)
		return false;
	else if (ident_response[strlen(ident_response) - 2] != '\r')
		return false;
	else
	{
		while (*cursor != ':' && *cursor != '\r')
			cursor++;			/* skip port field */

		if (*cursor != ':')
			return false;
		else
		{
			/* We're positioned to colon before response type field */
			char		response_type[80];
			int			i;		/* Index into *response_type */

			cursor++;			/* Go over colon */
			while (pg_isblank(*cursor))
				cursor++;		/* skip blanks */
			i = 0;
			while (*cursor != ':' && *cursor != '\r' && !pg_isblank(*cursor) &&
				   i < (int) (sizeof(response_type) - 1))
				response_type[i++] = *cursor++;
			response_type[i] = '\0';
			while (pg_isblank(*cursor))
				cursor++;		/* skip blanks */
			if (strcmp(response_type, "USERID") != 0)
				return false;
			else
			{
				/*
				 * It's a USERID response.  Good.  "cursor" should be pointing
				 * to the colon that precedes the operating system type.
				 */
				if (*cursor != ':')
					return false;
				else
				{
					cursor++;	/* Go over colon */
					/* Skip over operating system field. */
					while (*cursor != ':' && *cursor != '\r')
						cursor++;
					if (*cursor != ':')
						return false;
					else
					{
						int			i;	/* Index into *ident_user */

						cursor++;		/* Go over colon */
						while (pg_isblank(*cursor))
							cursor++;	/* skip blanks */
						/* Rest of line is user name.  Copy it over. */
						i = 0;
						while (*cursor != '\r' && i < IDENT_USERNAME_MAX)
							ident_user[i++] = *cursor++;
						ident_user[i] = '\0';
						return true;
					}
				}
			}
		}
	}
}


/*
 *	Talk to the ident server on host "remote_ip_addr" and find out who
 *	owns the tcp connection from his port "remote_port" to port
 *	"local_port_addr" on host "local_ip_addr".  Return the user name the
 *	ident server gives as "*ident_user".
 *
 *	IP addresses and port numbers are in network byte order.
 *
 *	But iff we're unable to get the information from ident, return false.
 */
static int
ident_inet(hbaPort *port)
{
	const SockAddr remote_addr = port->raddr;
	const SockAddr local_addr = port->laddr;
	char		ident_user[IDENT_USERNAME_MAX + 1];
	pgsocket	sock_fd;		/* File descriptor for socket on which we talk
								 * to Ident */
	int			rc;				/* Return code from a locally called function */
	bool		ident_return;
	char		remote_addr_s[NI_MAXHOST];
	char		remote_port[NI_MAXSERV];
	char		local_addr_s[NI_MAXHOST];
	char		local_port[NI_MAXSERV];
	char		ident_port[NI_MAXSERV];
	char		ident_query[80];
	char		ident_response[80 + IDENT_USERNAME_MAX];
	struct addrinfo *ident_serv = NULL,
			   *la = NULL,
				hints;

	/*
	 * Might look a little weird to first convert it to text and then back to
	 * sockaddr, but it's protocol independent.
	 */
	pg_getnameinfo_all(&remote_addr.addr, remote_addr.salen,
					   remote_addr_s, sizeof(remote_addr_s),
					   remote_port, sizeof(remote_port),
					   NI_NUMERICHOST | NI_NUMERICSERV);
	pg_getnameinfo_all(&local_addr.addr, local_addr.salen,
					   local_addr_s, sizeof(local_addr_s),
					   local_port, sizeof(local_port),
					   NI_NUMERICHOST | NI_NUMERICSERV);

	snprintf(ident_port, sizeof(ident_port), "%d", IDENT_PORT);
	hints.ai_flags = AI_NUMERICHOST;
	hints.ai_family = remote_addr.addr.ss_family;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = 0;
	hints.ai_addrlen = 0;
	hints.ai_canonname = NULL;
	hints.ai_addr = NULL;
	hints.ai_next = NULL;
	rc = pg_getaddrinfo_all(remote_addr_s, ident_port, &hints, &ident_serv);
	if (rc || !ident_serv)
	{
		if (ident_serv)
			pg_freeaddrinfo_all(hints.ai_family, ident_serv);
		return STATUS_ERROR;	/* we don't expect this to happen */
	}

	hints.ai_flags = AI_NUMERICHOST;
	hints.ai_family = local_addr.addr.ss_family;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = 0;
	hints.ai_addrlen = 0;
	hints.ai_canonname = NULL;
	hints.ai_addr = NULL;
	hints.ai_next = NULL;
	rc = pg_getaddrinfo_all(local_addr_s, NULL, &hints, &la);
	if (rc || !la)
	{
		if (la)
			pg_freeaddrinfo_all(hints.ai_family, la);
		return STATUS_ERROR;	/* we don't expect this to happen */
	}

	sock_fd = socket(ident_serv->ai_family, ident_serv->ai_socktype,
					 ident_serv->ai_protocol);
	if (sock_fd == PGINVALID_SOCKET)
	{
		ereport(LOG,
				(errcode_for_socket_access(),
				 errmsg("could not create socket for Ident connection: %m")));
		ident_return = false;
		goto ident_inet_done;
	}

	/*
	 * Bind to the address which the client originally contacted, otherwise
	 * the ident server won't be able to match up the right connection. This
	 * is necessary if the PostgreSQL server is running on an IP alias.
	 */
	rc = bind(sock_fd, la->ai_addr, la->ai_addrlen);
	if (rc != 0)
	{
		ereport(LOG,
				(errcode_for_socket_access(),
				 errmsg("could not bind to local address \"%s\": %m",
						local_addr_s)));
		ident_return = false;
		goto ident_inet_done;
	}

	rc = connect(sock_fd, ident_serv->ai_addr,
				 ident_serv->ai_addrlen);
	if (rc != 0)
	{
		ereport(LOG,
				(errcode_for_socket_access(),
				 errmsg("could not connect to Ident server at address \"%s\", port %s: %m",
						remote_addr_s, ident_port)));
		ident_return = false;
		goto ident_inet_done;
	}

	/* The query we send to the Ident server */
	snprintf(ident_query, sizeof(ident_query), "%s,%s\r\n",
			 remote_port, local_port);

	/* loop in case send is interrupted */
	do
	{
		rc = send(sock_fd, ident_query, strlen(ident_query), 0);
	} while (rc < 0 && errno == EINTR);

	if (rc < 0)
	{
		ereport(LOG,
				(errcode_for_socket_access(),
				 errmsg("could not send query to Ident server at address \"%s\", port %s: %m",
						remote_addr_s, ident_port)));
		ident_return = false;
		goto ident_inet_done;
	}

	do
	{
		rc = recv(sock_fd, ident_response, sizeof(ident_response) - 1, 0);
	} while (rc < 0 && errno == EINTR);

	if (rc < 0)
	{
		ereport(LOG,
				(errcode_for_socket_access(),
				 errmsg("could not receive response from Ident server at address \"%s\", port %s: %m",
						remote_addr_s, ident_port)));
		ident_return = false;
		goto ident_inet_done;
	}

	ident_response[rc] = '\0';
	ident_return = interpret_ident_response(ident_response, ident_user);
	if (!ident_return)
		ereport(LOG,
			(errmsg("invalidly formatted response from Ident server: \"%s\"",
					ident_response)));

ident_inet_done:
	if (sock_fd != PGINVALID_SOCKET)
		closesocket(sock_fd);
	pg_freeaddrinfo_all(remote_addr.addr.ss_family, ident_serv);
	pg_freeaddrinfo_all(local_addr.addr.ss_family, la);

	if (ident_return)
		/* Success! Check the usermap */
		return check_usermap(port->hba->usermap, port->user_name, ident_user, false);
	return STATUS_ERROR;
}

/*
 *	Ask kernel about the credentials of the connecting process,
 *	determine the symbolic name of the corresponding user, and check
 *	if valid per the usermap.
 *
 *	Iff authorized, return STATUS_OK, otherwise return STATUS_ERROR.
 */
#ifdef HAVE_UNIX_SOCKETS

static int
auth_peer(hbaPort *port)
{
	char		ident_user[IDENT_USERNAME_MAX + 1];
	uid_t		uid;
	gid_t		gid;
	struct passwd *pw;

	if (getpeereid(port->sock, &uid, &gid) != 0)
	{
		/* Provide special error message if getpeereid is a stub */
		if (errno == ENOSYS)
			ereport(LOG,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			errmsg("peer authentication is not supported on this platform")));
		else
			ereport(LOG,
					(errcode_for_socket_access(),
					 errmsg("could not get peer credentials: %m")));
		return STATUS_ERROR;
	}

	errno = 0;					/* clear errno before call */
	pw = getpwuid(uid);
	if (!pw)
	{
		ereport(LOG,
				(errmsg("could not look up local user ID %ld: %s",
						(long) uid,
						errno ? strerror(errno) : _("user does not exist"))));
		return STATUS_ERROR;
	}

	strlcpy(ident_user, pw->pw_name, IDENT_USERNAME_MAX + 1);

	return check_usermap(port->hba->usermap, port->user_name, ident_user, false);
}
#endif   /* HAVE_UNIX_SOCKETS */


/*----------------------------------------------------------------
 * PAM authentication system
 *----------------------------------------------------------------
 */
#ifdef USE_PAM

/*
 * PAM conversation function
 */

static int
pam_passwd_conv_proc(int num_msg, const struct pam_message ** msg,
					 struct pam_response ** resp, void *appdata_ptr)
{
	char	   *passwd;
	struct pam_response *reply;
	int			i;

	if (appdata_ptr)
		passwd = (char *) appdata_ptr;
	else
	{
		/*
		 * Workaround for Solaris 2.6 where the PAM library is broken and does
		 * not pass appdata_ptr to the conversation routine
		 */
		passwd = pam_passwd;
	}

	*resp = NULL;				/* in case of error exit */

	if (num_msg <= 0 || num_msg > PAM_MAX_NUM_MSG)
		return PAM_CONV_ERR;

	/*
	 * Explicitly not using palloc here - PAM will free this memory in
	 * pam_end()
	 */
	if ((reply = calloc(num_msg, sizeof(struct pam_response))) == NULL)
	{
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
		return PAM_CONV_ERR;
	}

	for (i = 0; i < num_msg; i++)
	{
		switch (msg[i]->msg_style)
		{
			case PAM_PROMPT_ECHO_OFF:
				if (strlen(passwd) == 0)
				{
					/*
					 * Password wasn't passed to PAM the first time around -
					 * let's go ask the client to send a password, which we
					 * then stuff into PAM.
					 */
					sendAuthRequest(pam_port_cludge, AUTH_REQ_PASSWORD);
					passwd = recv_password_packet(pam_port_cludge);
					if (passwd == NULL)
					{
						/*
						 * Client didn't want to send password.  We
						 * intentionally do not log anything about this.
						 */
						goto fail;
					}
					if (strlen(passwd) == 0)
					{
						ereport(LOG,
							  (errmsg("empty password returned by client")));
						goto fail;
					}
				}
				if ((reply[i].resp = strdup(passwd)) == NULL)
					goto fail;
				reply[i].resp_retcode = PAM_SUCCESS;
				break;
			case PAM_ERROR_MSG:
				ereport(LOG,
						(errmsg("error from underlying PAM layer: %s",
								msg[i]->msg)));
				/* FALL THROUGH */
			case PAM_TEXT_INFO:
				/* we don't bother to log TEXT_INFO messages */
				if ((reply[i].resp = strdup("")) == NULL)
					goto fail;
				reply[i].resp_retcode = PAM_SUCCESS;
				break;
			default:
				elog(LOG, "unsupported PAM conversation %d/\"%s\"",
					 msg[i]->msg_style,
					 msg[i]->msg ? msg[i]->msg : "(none)");
				goto fail;
		}
	}

	*resp = reply;
	return PAM_SUCCESS;

fail:
	/* free up whatever we allocated */
	for (i = 0; i < num_msg; i++)
	{
		if (reply[i].resp != NULL)
			free(reply[i].resp);
	}
	free(reply);

	return PAM_CONV_ERR;
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
	 * We can't entirely rely on PAM to pass through appdata --- it appears
	 * not to work on at least Solaris 2.6.  So use these ugly static
	 * variables instead.
	 */
	pam_passwd = password;
	pam_port_cludge = port;

	/*
	 * Set the application data portion of the conversation struct.  This is
	 * later used inside the PAM conversation to pass the password to the
	 * authentication module.
	 */
	pam_passw_conv.appdata_ptr = (char *) password;		/* from password above,
														 * not allocated */

	/* Optionally, one can set the service name in pg_hba.conf */
	if (port->hba->pamservice && port->hba->pamservice[0] != '\0')
		retval = pam_start(port->hba->pamservice, "pgsql@",
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



/*----------------------------------------------------------------
 * LDAP authentication system
 *----------------------------------------------------------------
 */
#ifdef USE_LDAP

/*
 * Initialize a connection to the LDAP server, including setting up
 * TLS if requested.
 */
static int
InitializeLDAPConnection(Port *port, LDAP **ldap)
{
	int			ldapversion = LDAP_VERSION3;
	int			r;

	*ldap = ldap_init(port->hba->ldapserver, port->hba->ldapport);
	if (!*ldap)
	{
#ifndef WIN32
		ereport(LOG,
				(errmsg("could not initialize LDAP: %m")));
#else
		ereport(LOG,
				(errmsg("could not initialize LDAP: error code %d",
						(int) LdapGetLastError())));
#endif
		return STATUS_ERROR;
	}

	if ((r = ldap_set_option(*ldap, LDAP_OPT_PROTOCOL_VERSION, &ldapversion)) != LDAP_SUCCESS)
	{
		ldap_unbind(*ldap);
		ereport(LOG,
				(errmsg("could not set LDAP protocol version: %s", ldap_err2string(r))));
		return STATUS_ERROR;
	}

	if (port->hba->ldaptls)
	{
#ifndef WIN32
		if ((r = ldap_start_tls_s(*ldap, NULL, NULL)) != LDAP_SUCCESS)
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
				ldap_unbind(*ldap);
				ereport(LOG,
						(errmsg("could not load wldap32.dll")));
				return STATUS_ERROR;
			}
			_ldap_start_tls_sA = (__ldap_start_tls_sA) GetProcAddress(ldaphandle, "ldap_start_tls_sA");
			if (_ldap_start_tls_sA == NULL)
			{
				ldap_unbind(*ldap);
				ereport(LOG,
						(errmsg("could not load function _ldap_start_tls_sA in wldap32.dll"),
						 errdetail("LDAP over SSL is not supported on this platform.")));
				return STATUS_ERROR;
			}

			/*
			 * Leak LDAP handle on purpose, because we need the library to
			 * stay open. This is ok because it will only ever be leaked once
			 * per process and is automatically cleaned up on process exit.
			 */
		}
		if ((r = _ldap_start_tls_sA(*ldap, NULL, NULL, NULL, NULL)) != LDAP_SUCCESS)
#endif
		{
			ldap_unbind(*ldap);
			ereport(LOG,
					(errmsg("could not start LDAP TLS session: %s", ldap_err2string(r))));
			return STATUS_ERROR;
		}
	}

	return STATUS_OK;
}

/*
 * Perform LDAP authentication
 */
static int
CheckLDAPAuth(Port *port)
{
	char	   *passwd;
	LDAP	   *ldap;
	int			r;
	char	   *fulluser;

	if (!port->hba->ldapserver || port->hba->ldapserver[0] == '\0')
	{
		ereport(LOG,
				(errmsg("LDAP server not specified")));
		return STATUS_ERROR;
	}

	if (port->hba->ldapport == 0)
		port->hba->ldapport = LDAP_PORT;

	sendAuthRequest(port, AUTH_REQ_PASSWORD);

	passwd = recv_password_packet(port);
	if (passwd == NULL)
		return STATUS_EOF;		/* client wouldn't send password */

	if (strlen(passwd) == 0)
	{
		ereport(LOG,
				(errmsg("empty password returned by client")));
		return STATUS_ERROR;
	}

	if (InitializeLDAPConnection(port, &ldap) == STATUS_ERROR)
		/* Error message already sent */
		return STATUS_ERROR;

	if (port->hba->ldapbasedn)
	{
		/*
		 * First perform an LDAP search to find the DN for the user we are
		 * trying to log in as.
		 */
		char	   *filter;
		LDAPMessage *search_message;
		LDAPMessage *entry;
		char	   *attributes[2];
		char	   *dn;
		char	   *c;
		int			count;

		/*
		 * Disallow any characters that we would otherwise need to escape,
		 * since they aren't really reasonable in a username anyway. Allowing
		 * them would make it possible to inject any kind of custom filters in
		 * the LDAP filter.
		 */
		for (c = port->user_name; *c; c++)
		{
			if (*c == '*' ||
				*c == '(' ||
				*c == ')' ||
				*c == '\\' ||
				*c == '/')
			{
				ereport(LOG,
						(errmsg("invalid character in user name for LDAP authentication")));
				return STATUS_ERROR;
			}
		}

		/*
		 * Bind with a pre-defined username/password (if available) for
		 * searching. If none is specified, this turns into an anonymous bind.
		 */
		r = ldap_simple_bind_s(ldap,
						  port->hba->ldapbinddn ? port->hba->ldapbinddn : "",
				 port->hba->ldapbindpasswd ? port->hba->ldapbindpasswd : "");
		if (r != LDAP_SUCCESS)
		{
			ereport(LOG,
					(errmsg("could not perform initial LDAP bind for ldapbinddn \"%s\" on server \"%s\": %s",
							port->hba->ldapbinddn, port->hba->ldapserver, ldap_err2string(r))));
			return STATUS_ERROR;
		}

		/* Fetch just one attribute, else *all* attributes are returned */
		attributes[0] = port->hba->ldapsearchattribute ? port->hba->ldapsearchattribute : "uid";
		attributes[1] = NULL;

		filter = psprintf("(%s=%s)",
						  attributes[0],
						  port->user_name);

		r = ldap_search_s(ldap,
						  port->hba->ldapbasedn,
						  port->hba->ldapscope,
						  filter,
						  attributes,
						  0,
						  &search_message);

		if (r != LDAP_SUCCESS)
		{
			ereport(LOG,
					(errmsg("could not search LDAP for filter \"%s\" on server \"%s\": %s",
						filter, port->hba->ldapserver, ldap_err2string(r))));
			pfree(filter);
			return STATUS_ERROR;
		}

		count = ldap_count_entries(ldap, search_message);
		if (count != 1)
		{
			if (count == 0)
				ereport(LOG,
				 (errmsg("LDAP user \"%s\" does not exist", port->user_name),
				  errdetail("LDAP search for filter \"%s\" on server \"%s\" returned no entries.",
							filter, port->hba->ldapserver)));
			else
				ereport(LOG,
				  (errmsg("LDAP user \"%s\" is not unique", port->user_name),
				   errdetail_plural("LDAP search for filter \"%s\" on server \"%s\" returned %d entry.",
									"LDAP search for filter \"%s\" on server \"%s\" returned %d entries.",
									count,
									filter, port->hba->ldapserver, count)));

			pfree(filter);
			ldap_msgfree(search_message);
			return STATUS_ERROR;
		}

		entry = ldap_first_entry(ldap, search_message);
		dn = ldap_get_dn(ldap, entry);
		if (dn == NULL)
		{
			int			error;

			(void) ldap_get_option(ldap, LDAP_OPT_ERROR_NUMBER, &error);
			ereport(LOG,
					(errmsg("could not get dn for the first entry matching \"%s\" on server \"%s\": %s",
					filter, port->hba->ldapserver, ldap_err2string(error))));
			pfree(filter);
			ldap_msgfree(search_message);
			return STATUS_ERROR;
		}
		fulluser = pstrdup(dn);

		pfree(filter);
		ldap_memfree(dn);
		ldap_msgfree(search_message);

		/* Unbind and disconnect from the LDAP server */
		r = ldap_unbind_s(ldap);
		if (r != LDAP_SUCCESS)
		{
			int			error;

			(void) ldap_get_option(ldap, LDAP_OPT_ERROR_NUMBER, &error);
			ereport(LOG,
					(errmsg("could not unbind after searching for user \"%s\" on server \"%s\": %s",
				  fulluser, port->hba->ldapserver, ldap_err2string(error))));
			pfree(fulluser);
			return STATUS_ERROR;
		}

		/*
		 * Need to re-initialize the LDAP connection, so that we can bind to
		 * it with a different username.
		 */
		if (InitializeLDAPConnection(port, &ldap) == STATUS_ERROR)
		{
			pfree(fulluser);

			/* Error message already sent */
			return STATUS_ERROR;
		}
	}
	else
		fulluser = psprintf("%s%s%s",
						  port->hba->ldapprefix ? port->hba->ldapprefix : "",
							port->user_name,
						 port->hba->ldapsuffix ? port->hba->ldapsuffix : "");

	r = ldap_simple_bind_s(ldap, fulluser, passwd);
	ldap_unbind(ldap);

	if (r != LDAP_SUCCESS)
	{
		ereport(LOG,
			(errmsg("LDAP login failed for user \"%s\" on server \"%s\": %s",
					fulluser, port->hba->ldapserver, ldap_err2string(r))));
		pfree(fulluser);
		return STATUS_ERROR;
	}

	pfree(fulluser);

	return STATUS_OK;
}
#endif   /* USE_LDAP */


/*----------------------------------------------------------------
 * SSL client certificate authentication
 *----------------------------------------------------------------
 */
#ifdef USE_SSL
static int
CheckCertAuth(Port *port)
{
	Assert(port->ssl);

	/* Make sure we have received a username in the certificate */
	if (port->peer_cn == NULL ||
		strlen(port->peer_cn) <= 0)
	{
		ereport(LOG,
				(errmsg("certificate authentication failed for user \"%s\": client certificate contains no user name",
						port->user_name)));
		return STATUS_ERROR;
	}

	/* Just pass the certificate CN to the usermap check */
	return check_usermap(port->hba->usermap, port->user_name, port->peer_cn, false);
}
#endif


/*----------------------------------------------------------------
 * RADIUS authentication
 *----------------------------------------------------------------
 */

/*
 * RADIUS authentication is described in RFC2865 (and several
 * others).
 */

#define RADIUS_VECTOR_LENGTH 16
#define RADIUS_HEADER_LENGTH 20

typedef struct
{
	uint8		attribute;
	uint8		length;
	uint8		data[1];
} radius_attribute;

typedef struct
{
	uint8		code;
	uint8		id;
	uint16		length;
	uint8		vector[RADIUS_VECTOR_LENGTH];
} radius_packet;

/* RADIUS packet types */
#define RADIUS_ACCESS_REQUEST	1
#define RADIUS_ACCESS_ACCEPT	2
#define RADIUS_ACCESS_REJECT	3

/* RAIDUS attributes */
#define RADIUS_USER_NAME		1
#define RADIUS_PASSWORD			2
#define RADIUS_SERVICE_TYPE		6
#define RADIUS_NAS_IDENTIFIER	32

/* RADIUS service types */
#define RADIUS_AUTHENTICATE_ONLY	8

/* Maximum size of a RADIUS packet we will create or accept */
#define RADIUS_BUFFER_SIZE 1024

/* Seconds to wait - XXX: should be in a config variable! */
#define RADIUS_TIMEOUT 3

static void
radius_add_attribute(radius_packet *packet, uint8 type, const unsigned char *data, int len)
{
	radius_attribute *attr;

	if (packet->length + len > RADIUS_BUFFER_SIZE)
	{
		/*
		 * With remotely realistic data, this can never happen. But catch it
		 * just to make sure we don't overrun a buffer. We'll just skip adding
		 * the broken attribute, which will in the end cause authentication to
		 * fail.
		 */
		elog(WARNING,
			 "Adding attribute code %d with length %d to radius packet would create oversize packet, ignoring",
			 type, len);
		return;

	}

	attr = (radius_attribute *) ((unsigned char *) packet + packet->length);
	attr->attribute = type;
	attr->length = len + 2;		/* total size includes type and length */
	memcpy(attr->data, data, len);
	packet->length += attr->length;
}

static int
CheckRADIUSAuth(Port *port)
{
	char	   *passwd;
	char	   *identifier = "postgresql";
	char		radius_buffer[RADIUS_BUFFER_SIZE];
	char		receive_buffer[RADIUS_BUFFER_SIZE];
	radius_packet *packet = (radius_packet *) radius_buffer;
	radius_packet *receivepacket = (radius_packet *) receive_buffer;
	int32		service = htonl(RADIUS_AUTHENTICATE_ONLY);
	uint8	   *cryptvector;
	uint8		encryptedpassword[RADIUS_VECTOR_LENGTH];
	int			packetlength;
	pgsocket	sock;

#ifdef HAVE_IPV6
	struct sockaddr_in6 localaddr;
	struct sockaddr_in6 remoteaddr;
#else
	struct sockaddr_in localaddr;
	struct sockaddr_in remoteaddr;
#endif
	struct addrinfo hint;
	struct addrinfo *serveraddrs;
	char		portstr[128];
	ACCEPT_TYPE_ARG3 addrsize;
	fd_set		fdset;
	struct timeval endtime;
	int			i,
				r;

	/* Make sure struct alignment is correct */
	Assert(offsetof(radius_packet, vector) == 4);

	/* Verify parameters */
	if (!port->hba->radiusserver || port->hba->radiusserver[0] == '\0')
	{
		ereport(LOG,
				(errmsg("RADIUS server not specified")));
		return STATUS_ERROR;
	}

	if (!port->hba->radiussecret || port->hba->radiussecret[0] == '\0')
	{
		ereport(LOG,
				(errmsg("RADIUS secret not specified")));
		return STATUS_ERROR;
	}

	if (port->hba->radiusport == 0)
		port->hba->radiusport = 1812;

	MemSet(&hint, 0, sizeof(hint));
	hint.ai_socktype = SOCK_DGRAM;
	hint.ai_family = AF_UNSPEC;
	snprintf(portstr, sizeof(portstr), "%d", port->hba->radiusport);

	r = pg_getaddrinfo_all(port->hba->radiusserver, portstr, &hint, &serveraddrs);
	if (r || !serveraddrs)
	{
		ereport(LOG,
				(errmsg("could not translate RADIUS server name \"%s\" to address: %s",
						port->hba->radiusserver, gai_strerror(r))));
		if (serveraddrs)
			pg_freeaddrinfo_all(hint.ai_family, serveraddrs);
		return STATUS_ERROR;
	}
	/* XXX: add support for multiple returned addresses? */

	if (port->hba->radiusidentifier && port->hba->radiusidentifier[0])
		identifier = port->hba->radiusidentifier;

	/* Send regular password request to client, and get the response */
	sendAuthRequest(port, AUTH_REQ_PASSWORD);

	passwd = recv_password_packet(port);
	if (passwd == NULL)
		return STATUS_EOF;		/* client wouldn't send password */

	if (strlen(passwd) == 0)
	{
		ereport(LOG,
				(errmsg("empty password returned by client")));
		return STATUS_ERROR;
	}

	if (strlen(passwd) > RADIUS_VECTOR_LENGTH)
	{
		ereport(LOG,
				(errmsg("RADIUS authentication does not support passwords longer than 16 characters")));
		return STATUS_ERROR;
	}

	/* Construct RADIUS packet */
	packet->code = RADIUS_ACCESS_REQUEST;
	packet->length = RADIUS_HEADER_LENGTH;
#ifdef USE_SSL
	if (RAND_bytes(packet->vector, RADIUS_VECTOR_LENGTH) != 1)
	{
		ereport(LOG,
				(errmsg("could not generate random encryption vector")));
		return STATUS_ERROR;
	}
#else
	for (i = 0; i < RADIUS_VECTOR_LENGTH; i++)
		/* Use a lower strengh random number of OpenSSL is not available */
		packet->vector[i] = random() % 255;
#endif
	packet->id = packet->vector[0];
	radius_add_attribute(packet, RADIUS_SERVICE_TYPE, (unsigned char *) &service, sizeof(service));
	radius_add_attribute(packet, RADIUS_USER_NAME, (unsigned char *) port->user_name, strlen(port->user_name));
	radius_add_attribute(packet, RADIUS_NAS_IDENTIFIER, (unsigned char *) identifier, strlen(identifier));

	/*
	 * RADIUS password attributes are calculated as: e[0] = p[0] XOR
	 * MD5(secret + vector)
	 */
	cryptvector = palloc(RADIUS_VECTOR_LENGTH + strlen(port->hba->radiussecret));
	memcpy(cryptvector, port->hba->radiussecret, strlen(port->hba->radiussecret));
	memcpy(cryptvector + strlen(port->hba->radiussecret), packet->vector, RADIUS_VECTOR_LENGTH);
	if (!pg_md5_binary(cryptvector, RADIUS_VECTOR_LENGTH + strlen(port->hba->radiussecret), encryptedpassword))
	{
		ereport(LOG,
				(errmsg("could not perform MD5 encryption of password")));
		pfree(cryptvector);
		return STATUS_ERROR;
	}
	pfree(cryptvector);
	for (i = 0; i < RADIUS_VECTOR_LENGTH; i++)
	{
		if (i < strlen(passwd))
			encryptedpassword[i] = passwd[i] ^ encryptedpassword[i];
		else
			encryptedpassword[i] = '\0' ^ encryptedpassword[i];
	}
	radius_add_attribute(packet, RADIUS_PASSWORD, encryptedpassword, RADIUS_VECTOR_LENGTH);

	/* Length need to be in network order on the wire */
	packetlength = packet->length;
	packet->length = htons(packet->length);

	sock = socket(serveraddrs[0].ai_family, SOCK_DGRAM, 0);
	if (sock == PGINVALID_SOCKET)
	{
		ereport(LOG,
				(errmsg("could not create RADIUS socket: %m")));
		pg_freeaddrinfo_all(hint.ai_family, serveraddrs);
		return STATUS_ERROR;
	}

	memset(&localaddr, 0, sizeof(localaddr));
#ifdef HAVE_IPV6
	localaddr.sin6_family = serveraddrs[0].ai_family;
	localaddr.sin6_addr = in6addr_any;
	if (localaddr.sin6_family == AF_INET6)
		addrsize = sizeof(struct sockaddr_in6);
	else
		addrsize = sizeof(struct sockaddr_in);
#else
	localaddr.sin_family = serveraddrs[0].ai_family;
	localaddr.sin_addr.s_addr = INADDR_ANY;
	addrsize = sizeof(struct sockaddr_in);
#endif
	if (bind(sock, (struct sockaddr *) & localaddr, addrsize))
	{
		ereport(LOG,
				(errmsg("could not bind local RADIUS socket: %m")));
		closesocket(sock);
		pg_freeaddrinfo_all(hint.ai_family, serveraddrs);
		return STATUS_ERROR;
	}

	if (sendto(sock, radius_buffer, packetlength, 0,
			   serveraddrs[0].ai_addr, serveraddrs[0].ai_addrlen) < 0)
	{
		ereport(LOG,
				(errmsg("could not send RADIUS packet: %m")));
		closesocket(sock);
		pg_freeaddrinfo_all(hint.ai_family, serveraddrs);
		return STATUS_ERROR;
	}

	/* Don't need the server address anymore */
	pg_freeaddrinfo_all(hint.ai_family, serveraddrs);

	/*
	 * Figure out at what time we should time out. We can't just use a single
	 * call to select() with a timeout, since somebody can be sending invalid
	 * packets to our port thus causing us to retry in a loop and never time
	 * out.
	 */
	gettimeofday(&endtime, NULL);
	endtime.tv_sec += RADIUS_TIMEOUT;

	while (true)
	{
		struct timeval timeout;
		struct timeval now;
		int64		timeoutval;

		gettimeofday(&now, NULL);
		timeoutval = (endtime.tv_sec * 1000000 + endtime.tv_usec) - (now.tv_sec * 1000000 + now.tv_usec);
		if (timeoutval <= 0)
		{
			ereport(LOG,
					(errmsg("timeout waiting for RADIUS response")));
			closesocket(sock);
			return STATUS_ERROR;
		}
		timeout.tv_sec = timeoutval / 1000000;
		timeout.tv_usec = timeoutval % 1000000;

		FD_ZERO(&fdset);
		FD_SET(sock, &fdset);

		r = select(sock + 1, &fdset, NULL, NULL, &timeout);
		if (r < 0)
		{
			if (errno == EINTR)
				continue;

			/* Anything else is an actual error */
			ereport(LOG,
					(errmsg("could not check status on RADIUS socket: %m")));
			closesocket(sock);
			return STATUS_ERROR;
		}
		if (r == 0)
		{
			ereport(LOG,
					(errmsg("timeout waiting for RADIUS response")));
			closesocket(sock);
			return STATUS_ERROR;
		}

		/*
		 * Attempt to read the response packet, and verify the contents.
		 *
		 * Any packet that's not actually a RADIUS packet, or otherwise does
		 * not validate as an explicit reject, is just ignored and we retry
		 * for another packet (until we reach the timeout). This is to avoid
		 * the possibility to denial-of-service the login by flooding the
		 * server with invalid packets on the port that we're expecting the
		 * RADIUS response on.
		 */

		addrsize = sizeof(remoteaddr);
		packetlength = recvfrom(sock, receive_buffer, RADIUS_BUFFER_SIZE, 0,
								(struct sockaddr *) & remoteaddr, &addrsize);
		if (packetlength < 0)
		{
			ereport(LOG,
					(errmsg("could not read RADIUS response: %m")));
			return STATUS_ERROR;
		}

#ifdef HAVE_IPV6
		if (remoteaddr.sin6_port != htons(port->hba->radiusport))
#else
		if (remoteaddr.sin_port != htons(port->hba->radiusport))
#endif
		{
#ifdef HAVE_IPV6
			ereport(LOG,
				  (errmsg("RADIUS response was sent from incorrect port: %d",
						  ntohs(remoteaddr.sin6_port))));
#else
			ereport(LOG,
				  (errmsg("RADIUS response was sent from incorrect port: %d",
						  ntohs(remoteaddr.sin_port))));
#endif
			continue;
		}

		if (packetlength < RADIUS_HEADER_LENGTH)
		{
			ereport(LOG,
					(errmsg("RADIUS response too short: %d", packetlength)));
			continue;
		}

		if (packetlength != ntohs(receivepacket->length))
		{
			ereport(LOG,
					(errmsg("RADIUS response has corrupt length: %d (actual length %d)",
							ntohs(receivepacket->length), packetlength)));
			continue;
		}

		if (packet->id != receivepacket->id)
		{
			ereport(LOG,
					(errmsg("RADIUS response is to a different request: %d (should be %d)",
							receivepacket->id, packet->id)));
			continue;
		}

		/*
		 * Verify the response authenticator, which is calculated as
		 * MD5(Code+ID+Length+RequestAuthenticator+Attributes+Secret)
		 */
		cryptvector = palloc(packetlength + strlen(port->hba->radiussecret));

		memcpy(cryptvector, receivepacket, 4);	/* code+id+length */
		memcpy(cryptvector + 4, packet->vector, RADIUS_VECTOR_LENGTH);	/* request
																		 * authenticator, from
																		 * original packet */
		if (packetlength > RADIUS_HEADER_LENGTH)		/* there may be no
														 * attributes at all */
			memcpy(cryptvector + RADIUS_HEADER_LENGTH, receive_buffer + RADIUS_HEADER_LENGTH, packetlength - RADIUS_HEADER_LENGTH);
		memcpy(cryptvector + packetlength, port->hba->radiussecret, strlen(port->hba->radiussecret));

		if (!pg_md5_binary(cryptvector,
						   packetlength + strlen(port->hba->radiussecret),
						   encryptedpassword))
		{
			ereport(LOG,
			(errmsg("could not perform MD5 encryption of received packet")));
			pfree(cryptvector);
			continue;
		}
		pfree(cryptvector);

		if (memcmp(receivepacket->vector, encryptedpassword, RADIUS_VECTOR_LENGTH) != 0)
		{
			ereport(LOG,
					(errmsg("RADIUS response has incorrect MD5 signature")));
			continue;
		}

		if (receivepacket->code == RADIUS_ACCESS_ACCEPT)
		{
			closesocket(sock);
			return STATUS_OK;
		}
		else if (receivepacket->code == RADIUS_ACCESS_REJECT)
		{
			closesocket(sock);
			return STATUS_ERROR;
		}
		else
		{
			ereport(LOG,
			 (errmsg("RADIUS response has invalid code (%d) for user \"%s\"",
					 receivepacket->code, port->user_name)));
			continue;
		}
	}							/* while (true) */
}
