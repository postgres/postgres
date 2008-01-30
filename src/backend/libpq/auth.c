/*-------------------------------------------------------------------------
 *
 * auth.c
 *	  Routines to handle network authentication
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/libpq/auth.c,v 1.163 2008/01/30 04:11:19 tgl Exp $
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
#include <unistd.h>

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
char	   *pg_krb_realm = NULL;

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
	char	   *cp;

	if (get_role_line(port->user_name) == NULL)
		return STATUS_ERROR;

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

	cp = strchr(kusername, '@');
	if (cp)
	{
		*cp = '\0';
		cp++;

		if (pg_krb_realm != NULL && strlen(pg_krb_realm))
		{
			/* Match realm against configured */
			if (pg_krb_caseins_users)
				ret = pg_strcasecmp(pg_krb_realm, cp);
			else
				ret = strcmp(pg_krb_realm, cp);

			if (ret)
			{
				elog(DEBUG2,
					 "krb5 realm (%s) and configured realm (%s) don't match",
					 cp, pg_krb_realm);

				krb5_free_ticket(pg_krb5_context, ticket);
				krb5_auth_con_free(pg_krb5_context, auth_context);
				return STATUS_ERROR;
			}
		}
	}
	else if (pg_krb_realm && strlen(pg_krb_realm))
	{
		elog(DEBUG2,
			 "krb5 did not return realm but realm matching was requested");

		krb5_free_ticket(pg_krb5_context, ticket);
		krb5_auth_con_free(pg_krb5_context, auth_context);
		return STATUS_ERROR;
	}

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

#ifdef ENABLE_GSS
/*----------------------------------------------------------------
 * GSSAPI authentication system
 *----------------------------------------------------------------
 */

#if defined(HAVE_GSSAPI_H)
#include <gssapi.h>
#else
#include <gssapi/gssapi.h>
#endif

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
	OM_uint32	lmaj_s,
				lmin_s,
				msg_ctx;
	char		msg_major[128],
				msg_minor[128];

	/* Fetch major status message */
	msg_ctx = 0;
	lmaj_s = gss_display_status(&lmin_s, maj_stat, GSS_C_GSS_CODE,
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
	lmaj_s = gss_display_status(&lmin_s, min_stat, GSS_C_MECH_CODE,
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
			 errdetail("%s: %s", msg_major, msg_minor)));
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
			size_t	kt_len = strlen(pg_krb_server_keyfile) + 14;
			char   *kt_path = malloc(kt_len);

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
		if (pq_getmessage(&buf, 2000))
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
			OM_uint32	lmin_s;

			elog(DEBUG4, "sending GSS response token of length %u",
				 (unsigned int) port->gss->outbuf.length);

			sendAuthRequest(port, AUTH_REQ_GSS_CONT);

			gss_release_buffer(&lmin_s, &port->gss->outbuf);
		}

		if (maj_stat != GSS_S_COMPLETE && maj_stat != GSS_S_CONTINUE_NEEDED)
		{
			OM_uint32	lmin_s;

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

		*cp = '\0';
		cp++;

		if (pg_krb_realm != NULL && strlen(pg_krb_realm))
		{
			/*
			 * Match the realm part of the name first
			 */
			if (pg_krb_caseins_users)
				ret = pg_strcasecmp(pg_krb_realm, cp);
			else
				ret = strcmp(pg_krb_realm, cp);

			if (ret)
			{
				/* GSS realm does not match */
				elog(DEBUG2,
				   "GSSAPI realm (%s) and configured realm (%s) don't match",
					 cp, pg_krb_realm);
				gss_release_buffer(&lmin_s, &gbuf);
				return STATUS_ERROR;
			}
		}
	}
	else if (pg_krb_realm && strlen(pg_krb_realm))
	{
		elog(DEBUG2,
			 "GSSAPI did not return realm but realm matching was requested");

		gss_release_buffer(&lmin_s, &gbuf);
		return STATUS_ERROR;
	}

	if (pg_krb_caseins_users)
		ret = pg_strcasecmp(port->user_name, gbuf.value);
	else
		ret = strcmp(port->user_name, gbuf.value);

	if (ret)
	{
		/* GSS name and PGUSER are not equivalent */
		elog(DEBUG2,
			 "provided username (%s) and GSSAPI username (%s) don't match",
			 port->user_name, (char *) gbuf.value);

		gss_release_buffer(&lmin_s, &gbuf);
		return STATUS_ERROR;
	}

	gss_release_buffer(&lmin_s, &gbuf);

	return STATUS_OK;
}
#else							/* no ENABLE_GSS */
static int
pg_GSS_recvauth(Port *port)
{
	ereport(LOG,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("GSSAPI not implemented on this server")));
	return STATUS_ERROR;
}
#endif   /* ENABLE_GSS */

#ifdef ENABLE_SSPI
static void
pg_SSPI_error(int severity, char *errmsg, SECURITY_STATUS r)
{
	char		sysmsg[256];

	if (FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, NULL, r, 0, sysmsg, sizeof(sysmsg), NULL) == 0)
		ereport(severity,
				(errmsg_internal("%s", errmsg),
				 errdetail("SSPI error %x", (unsigned int) r)));
	else
		ereport(severity,
				(errmsg_internal("%s", errmsg),
				 errdetail("%s (%x)", sysmsg, (unsigned int) r)));
}

typedef		SECURITY_STATUS
			(WINAPI * QUERY_SECURITY_CONTEXT_TOKEN_FN) (
													   PCtxtHandle, void **);

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
		pg_SSPI_error(ERROR,
			   gettext_noop("could not acquire SSPI credentials handle"), r);

	/*
	 * Loop through SSPI message exchange. This exchange can consist of
	 * multiple messags sent in both directions. First message is always from
	 * the client. All messages from client to server are password packets
	 * (type 'p').
	 */
	do
	{
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
		if (pq_getmessage(&buf, 2000))
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
				  gettext_noop("could not accept SSPI security context"), r);
		}

		if (sspictx == NULL)
		{
			sspictx = malloc(sizeof(CtxtHandle));
			if (sspictx == NULL)
				ereport(ERROR,
						(errmsg("out of memory")));

			memcpy(sspictx, &newctx, sizeof(CtxtHandle));
		}

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
				(errmsg_internal("could not load secur32.dll: %d",
								 (int) GetLastError())));

	_QuerySecurityContextToken = (QUERY_SECURITY_CONTEXT_TOKEN_FN)
		GetProcAddress(secur32, "QuerySecurityContextToken");
	if (_QuerySecurityContextToken == NULL)
	{
		FreeLibrary(secur32);
		ereport(ERROR,
				(errmsg_internal("could not locate QuerySecurityContextToken in secur32.dll: %d",
								 (int) GetLastError())));
	}

	r = (_QuerySecurityContextToken) (sspictx, &token);
	if (r != SEC_E_OK)
	{
		FreeLibrary(secur32);
		pg_SSPI_error(ERROR,
			   gettext_noop("could not get security token from context"), r);
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
			 (errmsg_internal("could not get token user size: error code %d",
							  (int) GetLastError())));

	tokenuser = malloc(retlen);
	if (tokenuser == NULL)
		ereport(ERROR,
				(errmsg("out of memory")));

	if (!GetTokenInformation(token, TokenUser, tokenuser, retlen, &retlen))
		ereport(ERROR,
				(errmsg_internal("could not get user token: error code %d",
								 (int) GetLastError())));

	if (!LookupAccountSid(NULL, tokenuser->User.Sid, accountname, &accountnamesize,
						  domainname, &domainnamesize, &accountnameuse))
		ereport(ERROR,
			  (errmsg_internal("could not lookup acconut sid: error code %d",
							   (int) GetLastError())));

	free(tokenuser);

	/*
	 * Compare realm/domain if requested. In SSPI, always compare case
	 * insensitive.
	 */
	if (pg_krb_realm && strlen(pg_krb_realm))
	{
		if (pg_strcasecmp(pg_krb_realm, domainname))
		{
			elog(DEBUG2,
				 "SSPI domain (%s) and configured domain (%s) don't match",
				 domainname, pg_krb_realm);

			return STATUS_ERROR;
		}
	}

	/*
	 * We have the username (without domain/realm) in accountname, compare to
	 * the supplied value. In SSPI, always compare case insensitive.
	 */
	if (pg_strcasecmp(port->user_name, accountname))
	{
		/* GSS name and PGUSER are not equivalent */
		elog(DEBUG2,
			 "provided username (%s) and SSPI username (%s) don't match",
			 port->user_name, accountname);

		return STATUS_ERROR;
	}

	return STATUS_OK;
}
#else							/* no ENABLE_SSPI */
static int
pg_SSPI_recvauth(Port *port)
{
	ereport(LOG,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("SSPI not implemented on this server")));
	return STATUS_ERROR;
}
#endif   /* ENABLE_SSPI */


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
		case uaGSS:
			errstr = gettext_noop("GSSAPI authentication failed for user \"%s\"");
			break;
		case uaSSPI:
			errstr = gettext_noop("SSPI authentication failed for user \"%s\"");
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

		case uaGSS:
			sendAuthRequest(port, AUTH_REQ_GSS);
			status = pg_GSS_recvauth(port);
			break;

		case uaSSPI:
			sendAuthRequest(port, AUTH_REQ_SSPI);
			status = pg_SSPI_recvauth(port);
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
	 * Crack the LDAP url. We do a very trivial parse..
	 * ldap[s]://<server>[:<port>]/<basedn>[;prefix[;suffix]]
	 */

	server[0] = '\0';
	basedn[0] = '\0';
	prefix[0] = '\0';
	suffix[0] = '\0';

	/* ldap, including port number */
	r = sscanf(port->auth_arg,
			   "ldap://%127[^:]:%d/%127[^;];%127[^;];%127s",
			   server, &ldapport, basedn, prefix, suffix);
	if (r < 3)
	{
		/* ldaps, including port number */
		r = sscanf(port->auth_arg,
				   "ldaps://%127[^:]:%d/%127[^;];%127[^;];%127s",
				   server, &ldapport, basedn, prefix, suffix);
		if (r >= 3)
			ssl = true;
	}
	if (r < 3)
	{
		/* ldap, no port number */
		r = sscanf(port->auth_arg,
				   "ldap://%127[^/]/%127[^;];%127[^;];%127s",
				   server, basedn, prefix, suffix);
	}
	if (r < 2)
	{
		/* ldaps, no port number */
		r = sscanf(port->auth_arg,
				   "ldaps://%127[^/]/%127[^;];%127[^;];%127s",
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
			 * Leak LDAP handle on purpose, because we need the library to
			 * stay open. This is ok because it will only ever be leaked once
			 * per process and is automatically cleaned up on process exit.
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
