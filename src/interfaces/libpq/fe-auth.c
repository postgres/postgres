/*-------------------------------------------------------------------------
 *
 * fe-auth.c
 *	   The front-end (client) authorization routines
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-auth.c
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
#include <limits.h>
#include <sys/param.h>			/* for MAXHOSTNAMELEN on most */
#include <sys/socket.h>
#ifdef HAVE_SYS_UCRED_H
#include <sys/ucred.h>
#endif
#ifndef  MAXHOSTNAMELEN
#include <netdb.h>				/* for MAXHOSTNAMELEN on some */
#endif
#endif

#include "common/md5.h"
#include "common/scram-common.h"
#include "fe-auth.h"
#include "fe-auth-sasl.h"
#include "libpq-fe.h"

#ifdef ENABLE_GSS
/*
 * GSSAPI authentication system.
 */

#include "fe-gssapi-common.h"

/*
 * Continue GSS authentication with next token as needed.
 */
static int
pg_GSS_continue(PGconn *conn, int payloadlen)
{
	OM_uint32	maj_stat,
				min_stat,
				lmin_s,
				gss_flags = GSS_C_MUTUAL_FLAG;
	gss_buffer_desc ginbuf;
	gss_buffer_desc goutbuf;

	/*
	 * On first call, there's no input token. On subsequent calls, read the
	 * input token into a GSS buffer.
	 */
	if (conn->gctx != GSS_C_NO_CONTEXT)
	{
		ginbuf.length = payloadlen;
		ginbuf.value = malloc(payloadlen);
		if (!ginbuf.value)
		{
			libpq_append_conn_error(conn, "out of memory allocating GSSAPI buffer (%d)",
									payloadlen);
			return STATUS_ERROR;
		}
		if (pqGetnchar(ginbuf.value, payloadlen, conn))
		{
			/*
			 * Shouldn't happen, because the caller should've ensured that the
			 * whole message is already in the input buffer.
			 */
			free(ginbuf.value);
			return STATUS_ERROR;
		}
	}
	else
	{
		ginbuf.length = 0;
		ginbuf.value = NULL;
	}

	/* finished parsing, trace server-to-client message */
	if (conn->Pfdebug)
		pqTraceOutputMessage(conn, conn->inBuffer + conn->inStart, false);

	/* Only try to acquire credentials if GSS delegation isn't disabled. */
	if (!pg_GSS_have_cred_cache(&conn->gcred))
		conn->gcred = GSS_C_NO_CREDENTIAL;

	if (conn->gssdelegation && conn->gssdelegation[0] == '1')
		gss_flags |= GSS_C_DELEG_FLAG;

	maj_stat = gss_init_sec_context(&min_stat,
									conn->gcred,
									&conn->gctx,
									conn->gtarg_nam,
									GSS_C_NO_OID,
									gss_flags,
									0,
									GSS_C_NO_CHANNEL_BINDINGS,
									(ginbuf.value == NULL) ? GSS_C_NO_BUFFER : &ginbuf,
									NULL,
									&goutbuf,
									NULL,
									NULL);

	free(ginbuf.value);

	if (goutbuf.length != 0)
	{
		/*
		 * GSS generated data to send to the server. We don't care if it's the
		 * first or subsequent packet, just send the same kind of password
		 * packet.
		 */
		conn->current_auth_response = AUTH_RESPONSE_GSS;
		if (pqPacketSend(conn, PqMsg_GSSResponse,
						 goutbuf.value, goutbuf.length) != STATUS_OK)
		{
			gss_release_buffer(&lmin_s, &goutbuf);
			return STATUS_ERROR;
		}
	}
	gss_release_buffer(&lmin_s, &goutbuf);

	if (maj_stat != GSS_S_COMPLETE && maj_stat != GSS_S_CONTINUE_NEEDED)
	{
		pg_GSS_error(libpq_gettext("GSSAPI continuation error"),
					 conn,
					 maj_stat, min_stat);
		gss_release_name(&lmin_s, &conn->gtarg_nam);
		if (conn->gctx)
			gss_delete_sec_context(&lmin_s, &conn->gctx, GSS_C_NO_BUFFER);
		return STATUS_ERROR;
	}

	if (maj_stat == GSS_S_COMPLETE)
	{
		conn->client_finished_auth = true;
		gss_release_name(&lmin_s, &conn->gtarg_nam);
		conn->gssapi_used = true;
	}

	return STATUS_OK;
}

/*
 * Send initial GSS authentication token
 */
static int
pg_GSS_startup(PGconn *conn, int payloadlen)
{
	int			ret;
	char	   *host = conn->connhost[conn->whichhost].host;

	if (!(host && host[0] != '\0'))
	{
		libpq_append_conn_error(conn, "host name must be specified");
		return STATUS_ERROR;
	}

	if (conn->gctx)
	{
		libpq_append_conn_error(conn, "duplicate GSS authentication request");
		return STATUS_ERROR;
	}

	ret = pg_GSS_load_servicename(conn);
	if (ret != STATUS_OK)
		return ret;

	/*
	 * Initial packet is the same as a continuation packet with no initial
	 * context.
	 */
	conn->gctx = GSS_C_NO_CONTEXT;

	return pg_GSS_continue(conn, payloadlen);
}
#endif							/* ENABLE_GSS */


#ifdef ENABLE_SSPI
/*
 * SSPI authentication system (Windows only)
 */

static void
pg_SSPI_error(PGconn *conn, const char *mprefix, SECURITY_STATUS r)
{
	char		sysmsg[256];

	if (FormatMessage(FORMAT_MESSAGE_IGNORE_INSERTS |
					  FORMAT_MESSAGE_FROM_SYSTEM,
					  NULL, r, 0,
					  sysmsg, sizeof(sysmsg), NULL) == 0)
		appendPQExpBuffer(&conn->errorMessage, "%s: SSPI error %x\n",
						  mprefix, (unsigned int) r);
	else
		appendPQExpBuffer(&conn->errorMessage, "%s: %s (%x)\n",
						  mprefix, sysmsg, (unsigned int) r);
}

/*
 * Continue SSPI authentication with next token as needed.
 */
static int
pg_SSPI_continue(PGconn *conn, int payloadlen)
{
	SECURITY_STATUS r;
	CtxtHandle	newContext;
	ULONG		contextAttr;
	SecBufferDesc inbuf;
	SecBufferDesc outbuf;
	SecBuffer	OutBuffers[1];
	SecBuffer	InBuffers[1];
	char	   *inputbuf = NULL;

	if (conn->sspictx != NULL)
	{
		/*
		 * On runs other than the first we have some data to send. Put this
		 * data in a SecBuffer type structure.
		 */
		inputbuf = malloc(payloadlen);
		if (!inputbuf)
		{
			libpq_append_conn_error(conn, "out of memory allocating SSPI buffer (%d)",
									payloadlen);
			return STATUS_ERROR;
		}
		if (pqGetnchar(inputbuf, payloadlen, conn))
		{
			/*
			 * Shouldn't happen, because the caller should've ensured that the
			 * whole message is already in the input buffer.
			 */
			free(inputbuf);
			return STATUS_ERROR;
		}

		inbuf.ulVersion = SECBUFFER_VERSION;
		inbuf.cBuffers = 1;
		inbuf.pBuffers = InBuffers;
		InBuffers[0].pvBuffer = inputbuf;
		InBuffers[0].cbBuffer = payloadlen;
		InBuffers[0].BufferType = SECBUFFER_TOKEN;
	}

	/* finished parsing, trace server-to-client message */
	if (conn->Pfdebug)
		pqTraceOutputMessage(conn, conn->inBuffer + conn->inStart, false);

	OutBuffers[0].pvBuffer = NULL;
	OutBuffers[0].BufferType = SECBUFFER_TOKEN;
	OutBuffers[0].cbBuffer = 0;
	outbuf.cBuffers = 1;
	outbuf.pBuffers = OutBuffers;
	outbuf.ulVersion = SECBUFFER_VERSION;

	r = InitializeSecurityContext(conn->sspicred,
								  conn->sspictx,
								  conn->sspitarget,
								  ISC_REQ_ALLOCATE_MEMORY,
								  0,
								  SECURITY_NETWORK_DREP,
								  (conn->sspictx == NULL) ? NULL : &inbuf,
								  0,
								  &newContext,
								  &outbuf,
								  &contextAttr,
								  NULL);

	/* we don't need the input anymore */
	free(inputbuf);

	if (r != SEC_E_OK && r != SEC_I_CONTINUE_NEEDED)
	{
		pg_SSPI_error(conn, libpq_gettext("SSPI continuation error"), r);

		return STATUS_ERROR;
	}

	if (conn->sspictx == NULL)
	{
		/* On first run, transfer retrieved context handle */
		conn->sspictx = malloc(sizeof(CtxtHandle));
		if (conn->sspictx == NULL)
		{
			libpq_append_conn_error(conn, "out of memory");
			return STATUS_ERROR;
		}
		memcpy(conn->sspictx, &newContext, sizeof(CtxtHandle));
	}

	/*
	 * If SSPI returned any data to be sent to the server (as it normally
	 * would), send this data as a password packet.
	 */
	if (outbuf.cBuffers > 0)
	{
		if (outbuf.cBuffers != 1)
		{
			/*
			 * This should never happen, at least not for Kerberos
			 * authentication. Keep check in case it shows up with other
			 * authentication methods later.
			 */
			appendPQExpBufferStr(&conn->errorMessage,
								 "SSPI returned invalid number of output buffers\n");
			return STATUS_ERROR;
		}

		/*
		 * If the negotiation is complete, there may be zero bytes to send.
		 * The server is at this point not expecting any more data, so don't
		 * send it.
		 */
		if (outbuf.pBuffers[0].cbBuffer > 0)
		{
			conn->current_auth_response = AUTH_RESPONSE_GSS;
			if (pqPacketSend(conn, PqMsg_GSSResponse,
							 outbuf.pBuffers[0].pvBuffer, outbuf.pBuffers[0].cbBuffer))
			{
				FreeContextBuffer(outbuf.pBuffers[0].pvBuffer);
				return STATUS_ERROR;
			}
		}
		FreeContextBuffer(outbuf.pBuffers[0].pvBuffer);
	}

	if (r == SEC_E_OK)
		conn->client_finished_auth = true;

	/* Cleanup is handled by the code in freePGconn() */
	return STATUS_OK;
}

/*
 * Send initial SSPI authentication token.
 * If use_negotiate is 0, use kerberos authentication package which is
 * compatible with Unix. If use_negotiate is 1, use the negotiate package
 * which supports both kerberos and NTLM, but is not compatible with Unix.
 */
static int
pg_SSPI_startup(PGconn *conn, int use_negotiate, int payloadlen)
{
	SECURITY_STATUS r;
	TimeStamp	expire;
	char	   *host = conn->connhost[conn->whichhost].host;

	if (conn->sspictx)
	{
		libpq_append_conn_error(conn, "duplicate SSPI authentication request");
		return STATUS_ERROR;
	}

	/*
	 * Retrieve credentials handle
	 */
	conn->sspicred = malloc(sizeof(CredHandle));
	if (conn->sspicred == NULL)
	{
		libpq_append_conn_error(conn, "out of memory");
		return STATUS_ERROR;
	}

	r = AcquireCredentialsHandle(NULL,
								 use_negotiate ? "negotiate" : "kerberos",
								 SECPKG_CRED_OUTBOUND,
								 NULL,
								 NULL,
								 NULL,
								 NULL,
								 conn->sspicred,
								 &expire);
	if (r != SEC_E_OK)
	{
		pg_SSPI_error(conn, libpq_gettext("could not acquire SSPI credentials"), r);
		free(conn->sspicred);
		conn->sspicred = NULL;
		return STATUS_ERROR;
	}

	/*
	 * Compute target principal name. SSPI has a different format from GSSAPI,
	 * but not more complex. We can skip the @REALM part, because Windows will
	 * fill that in for us automatically.
	 */
	if (!(host && host[0] != '\0'))
	{
		libpq_append_conn_error(conn, "host name must be specified");
		return STATUS_ERROR;
	}
	conn->sspitarget = malloc(strlen(conn->krbsrvname) + strlen(host) + 2);
	if (!conn->sspitarget)
	{
		libpq_append_conn_error(conn, "out of memory");
		return STATUS_ERROR;
	}
	sprintf(conn->sspitarget, "%s/%s", conn->krbsrvname, host);

	/*
	 * Indicate that we're in SSPI authentication mode to make sure that
	 * pg_SSPI_continue is called next time in the negotiation.
	 */
	conn->usesspi = 1;

	return pg_SSPI_continue(conn, payloadlen);
}
#endif							/* ENABLE_SSPI */

/*
 * Initialize SASL authentication exchange.
 */
static int
pg_SASL_init(PGconn *conn, int payloadlen)
{
	char	   *initialresponse = NULL;
	int			initialresponselen;
	const char *selected_mechanism;
	PQExpBufferData mechanism_buf;
	char	   *password = NULL;
	SASLStatus	status;

	initPQExpBuffer(&mechanism_buf);

	if (conn->channel_binding[0] == 'r' &&	/* require */
		!conn->ssl_in_use)
	{
		libpq_append_conn_error(conn, "channel binding required, but SSL not in use");
		goto error;
	}

	if (conn->sasl_state)
	{
		libpq_append_conn_error(conn, "duplicate SASL authentication request");
		goto error;
	}

	/*
	 * Parse the list of SASL authentication mechanisms in the
	 * AuthenticationSASL message, and select the best mechanism that we
	 * support. Mechanisms are listed by order of decreasing importance.
	 */
	selected_mechanism = NULL;
	for (;;)
	{
		if (pqGets(&mechanism_buf, conn))
		{
			appendPQExpBufferStr(&conn->errorMessage,
								 "fe_sendauth: invalid authentication request from server: invalid list of authentication mechanisms\n");
			goto error;
		}
		if (PQExpBufferDataBroken(mechanism_buf))
			goto oom_error;

		/* An empty string indicates end of list */
		if (mechanism_buf.data[0] == '\0')
			break;

		/*
		 * Select the mechanism to use.  Pick SCRAM-SHA-256-PLUS over anything
		 * else if a channel binding type is set and if the client supports it
		 * (and did not set channel_binding=disable). Pick SCRAM-SHA-256 if
		 * nothing else has already been picked.  If we add more mechanisms, a
		 * more refined priority mechanism might become necessary.
		 */
		if (strcmp(mechanism_buf.data, SCRAM_SHA_256_PLUS_NAME) == 0)
		{
			if (conn->ssl_in_use)
			{
				/* The server has offered SCRAM-SHA-256-PLUS. */

#ifdef USE_SSL
				/*
				 * The client supports channel binding, which is chosen if
				 * channel_binding is not disabled.
				 */
				if (conn->channel_binding[0] != 'd')	/* disable */
				{
					selected_mechanism = SCRAM_SHA_256_PLUS_NAME;
					conn->sasl = &pg_scram_mech;
					conn->password_needed = true;
				}
#else
				/*
				 * The client does not support channel binding.  If it is
				 * required, complain immediately instead of the error below
				 * which would be confusing as the server is publishing
				 * SCRAM-SHA-256-PLUS.
				 */
				if (conn->channel_binding[0] == 'r')	/* require */
				{
					libpq_append_conn_error(conn, "channel binding is required, but client does not support it");
					goto error;
				}
#endif
			}
			else
			{
				/*
				 * The server offered SCRAM-SHA-256-PLUS, but the connection
				 * is not SSL-encrypted. That's not sane. Perhaps SSL was
				 * stripped by a proxy? There's no point in continuing,
				 * because the server will reject the connection anyway if we
				 * try authenticate without channel binding even though both
				 * the client and server supported it. The SCRAM exchange
				 * checks for that, to prevent downgrade attacks.
				 */
				libpq_append_conn_error(conn, "server offered SCRAM-SHA-256-PLUS authentication over a non-SSL connection");
				goto error;
			}
		}
		else if (strcmp(mechanism_buf.data, SCRAM_SHA_256_NAME) == 0 &&
				 !selected_mechanism)
		{
			selected_mechanism = SCRAM_SHA_256_NAME;
			conn->sasl = &pg_scram_mech;
			conn->password_needed = true;
		}
	}

	if (!selected_mechanism)
	{
		libpq_append_conn_error(conn, "none of the server's SASL authentication mechanisms are supported");
		goto error;
	}

	if (conn->channel_binding[0] == 'r' &&	/* require */
		strcmp(selected_mechanism, SCRAM_SHA_256_PLUS_NAME) != 0)
	{
		libpq_append_conn_error(conn, "channel binding is required, but server did not offer an authentication method that supports channel binding");
		goto error;
	}

	/*
	 * Now that the SASL mechanism has been chosen for the exchange,
	 * initialize its state information.
	 */

	/*
	 * First, select the password to use for the exchange, complaining if
	 * there isn't one and the selected SASL mechanism needs it.
	 */
	if (conn->password_needed)
	{
		password = conn->connhost[conn->whichhost].password;
		if (password == NULL)
			password = conn->pgpass;
		if (password == NULL || password[0] == '\0')
		{
			appendPQExpBufferStr(&conn->errorMessage,
								 PQnoPasswordSupplied);
			goto error;
		}
	}

	/* finished parsing, trace server-to-client message */
	if (conn->Pfdebug)
		pqTraceOutputMessage(conn, conn->inBuffer + conn->inStart, false);

	Assert(conn->sasl);

	/*
	 * Initialize the SASL state information with all the information gathered
	 * during the initial exchange.
	 *
	 * Note: Only tls-unique is supported for the moment.
	 */
	conn->sasl_state = conn->sasl->init(conn,
										password,
										selected_mechanism);
	if (!conn->sasl_state)
		goto oom_error;

	/* Get the mechanism-specific Initial Client Response, if any */
	status = conn->sasl->exchange(conn->sasl_state,
								  NULL, -1,
								  &initialresponse, &initialresponselen);

	if (status == SASL_FAILED)
		goto error;

	/*
	 * Build a SASLInitialResponse message, and send it.
	 */
	if (pqPutMsgStart(PqMsg_SASLInitialResponse, conn))
		goto error;
	if (pqPuts(selected_mechanism, conn))
		goto error;
	if (initialresponse)
	{
		if (pqPutInt(initialresponselen, 4, conn))
			goto error;
		if (pqPutnchar(initialresponse, initialresponselen, conn))
			goto error;
	}
	conn->current_auth_response = AUTH_RESPONSE_SASL_INITIAL;
	if (pqPutMsgEnd(conn))
		goto error;

	if (pqFlush(conn))
		goto error;

	termPQExpBuffer(&mechanism_buf);
	free(initialresponse);

	return STATUS_OK;

error:
	termPQExpBuffer(&mechanism_buf);
	free(initialresponse);
	return STATUS_ERROR;

oom_error:
	termPQExpBuffer(&mechanism_buf);
	free(initialresponse);
	libpq_append_conn_error(conn, "out of memory");
	return STATUS_ERROR;
}

/*
 * Exchange a message for SASL communication protocol with the backend.
 * This should be used after calling pg_SASL_init to set up the status of
 * the protocol.
 */
static int
pg_SASL_continue(PGconn *conn, int payloadlen, bool final)
{
	char	   *output;
	int			outputlen;
	int			res;
	char	   *challenge;
	SASLStatus	status;

	/* Read the SASL challenge from the AuthenticationSASLContinue message. */
	challenge = malloc(payloadlen + 1);
	if (!challenge)
	{
		libpq_append_conn_error(conn, "out of memory allocating SASL buffer (%d)",
								payloadlen);
		return STATUS_ERROR;
	}

	if (pqGetnchar(challenge, payloadlen, conn))
	{
		free(challenge);
		return STATUS_ERROR;
	}

	/* finished parsing, trace server-to-client message */
	if (conn->Pfdebug)
		pqTraceOutputMessage(conn, conn->inBuffer + conn->inStart, false);

	/* For safety and convenience, ensure the buffer is NULL-terminated. */
	challenge[payloadlen] = '\0';

	status = conn->sasl->exchange(conn->sasl_state,
								  challenge, payloadlen,
								  &output, &outputlen);
	free(challenge);			/* don't need the input anymore */

	if (final && status == SASL_CONTINUE)
	{
		if (outputlen != 0)
			free(output);

		libpq_append_conn_error(conn, "AuthenticationSASLFinal received from server, but SASL authentication was not completed");
		return STATUS_ERROR;
	}

	/*
	 * If the exchange is not completed yet, we need to make sure that the
	 * SASL mechanism has generated a message to send back.
	 */
	if (output == NULL && status == SASL_CONTINUE)
	{
		libpq_append_conn_error(conn, "no client response found after SASL exchange success");
		return STATUS_ERROR;
	}

	/*
	 * SASL allows zero-length responses, so this check uses "output" and not
	 * "outputlen" to allow the case of an empty message.
	 */
	if (output)
	{
		/*
		 * Send the SASL response to the server.
		 */
		conn->current_auth_response = AUTH_RESPONSE_SASL;
		res = pqPacketSend(conn, PqMsg_SASLResponse, output, outputlen);
		free(output);

		if (res != STATUS_OK)
			return STATUS_ERROR;
	}

	if (status == SASL_FAILED)
		return STATUS_ERROR;

	return STATUS_OK;
}

static int
pg_password_sendauth(PGconn *conn, const char *password, AuthRequest areq)
{
	int			ret;
	char	   *crypt_pwd = NULL;
	const char *pwd_to_send;
	char		md5Salt[4];

	/* Read the salt from the AuthenticationMD5Password message. */
	if (areq == AUTH_REQ_MD5)
	{
		if (pqGetnchar(md5Salt, 4, conn))
			return STATUS_ERROR;	/* shouldn't happen */
	}

	/* finished parsing, trace server-to-client message */
	if (conn->Pfdebug)
		pqTraceOutputMessage(conn, conn->inBuffer + conn->inStart, false);

	/* Encrypt the password if needed. */

	switch (areq)
	{
		case AUTH_REQ_MD5:
			{
				char	   *crypt_pwd2;
				const char *errstr = NULL;

				/* Allocate enough space for two MD5 hashes */
				crypt_pwd = malloc(2 * (MD5_PASSWD_LEN + 1));
				if (!crypt_pwd)
				{
					libpq_append_conn_error(conn, "out of memory");
					return STATUS_ERROR;
				}

				crypt_pwd2 = crypt_pwd + MD5_PASSWD_LEN + 1;
				if (!pg_md5_encrypt(password, conn->pguser,
									strlen(conn->pguser), crypt_pwd2,
									&errstr))
				{
					libpq_append_conn_error(conn, "could not encrypt password: %s", errstr);
					free(crypt_pwd);
					return STATUS_ERROR;
				}
				if (!pg_md5_encrypt(crypt_pwd2 + strlen("md5"), md5Salt,
									4, crypt_pwd, &errstr))
				{
					libpq_append_conn_error(conn, "could not encrypt password: %s", errstr);
					free(crypt_pwd);
					return STATUS_ERROR;
				}

				pwd_to_send = crypt_pwd;
				break;
			}
		case AUTH_REQ_PASSWORD:
			pwd_to_send = password;
			break;
		default:
			return STATUS_ERROR;
	}
	conn->current_auth_response = AUTH_RESPONSE_PASSWORD;
	ret = pqPacketSend(conn, PqMsg_PasswordMessage,
					   pwd_to_send, strlen(pwd_to_send) + 1);
	free(crypt_pwd);
	return ret;
}

/*
 * Translate a disallowed AuthRequest code into an error message.
 */
static const char *
auth_method_description(AuthRequest areq)
{
	switch (areq)
	{
		case AUTH_REQ_PASSWORD:
			return libpq_gettext("server requested a cleartext password");
		case AUTH_REQ_MD5:
			return libpq_gettext("server requested a hashed password");
		case AUTH_REQ_GSS:
		case AUTH_REQ_GSS_CONT:
			return libpq_gettext("server requested GSSAPI authentication");
		case AUTH_REQ_SSPI:
			return libpq_gettext("server requested SSPI authentication");
		case AUTH_REQ_SASL:
		case AUTH_REQ_SASL_CONT:
		case AUTH_REQ_SASL_FIN:
			return libpq_gettext("server requested SASL authentication");
	}

	return libpq_gettext("server requested an unknown authentication type");
}

/*
 * Convenience macro for checking the allowed_auth_methods bitmask.  Caller
 * must ensure that type is not greater than 31 (high bit of the bitmask).
 */
#define auth_method_allowed(conn, type) \
	(((conn)->allowed_auth_methods & (1 << (type))) != 0)

/*
 * Verify that the authentication request is expected, given the connection
 * parameters. This is especially important when the client wishes to
 * authenticate the server before any sensitive information is exchanged.
 */
static bool
check_expected_areq(AuthRequest areq, PGconn *conn)
{
	bool		result = true;
	const char *reason = NULL;

	StaticAssertDecl((sizeof(conn->allowed_auth_methods) * CHAR_BIT) > AUTH_REQ_MAX,
					 "AUTH_REQ_MAX overflows the allowed_auth_methods bitmask");

	if (conn->sslcertmode[0] == 'r' /* require */
		&& areq == AUTH_REQ_OK)
	{
		/*
		 * Trade off a little bit of complexity to try to get these error
		 * messages as precise as possible.
		 */
		if (!conn->ssl_cert_requested)
		{
			libpq_append_conn_error(conn, "server did not request an SSL certificate");
			return false;
		}
		else if (!conn->ssl_cert_sent)
		{
			libpq_append_conn_error(conn, "server accepted connection without a valid SSL certificate");
			return false;
		}
	}

	/*
	 * If the user required a specific auth method, or specified an allowed
	 * set, then reject all others here, and make sure the server actually
	 * completes an authentication exchange.
	 */
	if (conn->require_auth)
	{
		switch (areq)
		{
			case AUTH_REQ_OK:

				/*
				 * Check to make sure we've actually finished our exchange (or
				 * else that the user has allowed an authentication-less
				 * connection).
				 *
				 * If the user has allowed both SCRAM and unauthenticated
				 * (trust) connections, then this check will silently accept
				 * partial SCRAM exchanges, where a misbehaving server does
				 * not provide its verifier before sending an OK.  This is
				 * consistent with historical behavior, but it may be a point
				 * to revisit in the future, since it could allow a server
				 * that doesn't know the user's password to silently harvest
				 * material for a brute force attack.
				 */
				if (!conn->auth_required || conn->client_finished_auth)
					break;

				/*
				 * No explicit authentication request was made by the server
				 * -- or perhaps it was made and not completed, in the case of
				 * SCRAM -- but there is one special case to check.  If the
				 * user allowed "gss", then a GSS-encrypted channel also
				 * satisfies the check.
				 */
#ifdef ENABLE_GSS
				if (auth_method_allowed(conn, AUTH_REQ_GSS) && conn->gssenc)
				{
					/*
					 * If implicit GSS auth has already been performed via GSS
					 * encryption, we don't need to have performed an
					 * AUTH_REQ_GSS exchange.  This allows require_auth=gss to
					 * be combined with gssencmode, since there won't be an
					 * explicit authentication request in that case.
					 */
				}
				else
#endif
				{
					reason = libpq_gettext("server did not complete authentication");
					result = false;
				}

				break;

			case AUTH_REQ_PASSWORD:
			case AUTH_REQ_MD5:
			case AUTH_REQ_GSS:
			case AUTH_REQ_GSS_CONT:
			case AUTH_REQ_SSPI:
			case AUTH_REQ_SASL:
			case AUTH_REQ_SASL_CONT:
			case AUTH_REQ_SASL_FIN:

				/*
				 * We don't handle these with the default case, to avoid
				 * bit-shifting past the end of the allowed_auth_methods mask
				 * if the server sends an unexpected AuthRequest.
				 */
				result = auth_method_allowed(conn, areq);
				break;

			default:
				result = false;
				break;
		}
	}

	if (!result)
	{
		if (!reason)
			reason = auth_method_description(areq);

		libpq_append_conn_error(conn, "authentication method requirement \"%s\" failed: %s",
								conn->require_auth, reason);
		return result;
	}

	/*
	 * When channel_binding=require, we must protect against two cases: (1) we
	 * must not respond to non-SASL authentication requests, which might leak
	 * information such as the client's password; and (2) even if we receive
	 * AUTH_REQ_OK, we still must ensure that channel binding has happened in
	 * order to authenticate the server.
	 */
	if (conn->channel_binding[0] == 'r' /* require */ )
	{
		switch (areq)
		{
			case AUTH_REQ_SASL:
			case AUTH_REQ_SASL_CONT:
			case AUTH_REQ_SASL_FIN:
				break;
			case AUTH_REQ_OK:
				if (!conn->sasl || !conn->sasl->channel_bound(conn->sasl_state))
				{
					libpq_append_conn_error(conn, "channel binding required, but server authenticated client without channel binding");
					result = false;
				}
				break;
			default:
				libpq_append_conn_error(conn, "channel binding required but not supported by server's authentication request");
				result = false;
				break;
		}
	}

	return result;
}

/*
 * pg_fe_sendauth
 *		client demux routine for processing an authentication request
 *
 * The server has sent us an authentication challenge (or OK). Send an
 * appropriate response. The caller has ensured that the whole message is
 * now in the input buffer, and has already read the type and length of
 * it. We are responsible for reading any remaining extra data, specific
 * to the authentication method. 'payloadlen' is the remaining length in
 * the message.
 */
int
pg_fe_sendauth(AuthRequest areq, int payloadlen, PGconn *conn)
{
	int			oldmsglen;

	if (!check_expected_areq(areq, conn))
		return STATUS_ERROR;

	switch (areq)
	{
		case AUTH_REQ_OK:
			break;

		case AUTH_REQ_KRB4:
			libpq_append_conn_error(conn, "Kerberos 4 authentication not supported");
			return STATUS_ERROR;

		case AUTH_REQ_KRB5:
			libpq_append_conn_error(conn, "Kerberos 5 authentication not supported");
			return STATUS_ERROR;

#if defined(ENABLE_GSS) || defined(ENABLE_SSPI)
		case AUTH_REQ_GSS:
#if !defined(ENABLE_SSPI)
			/* no native SSPI, so use GSSAPI library for it */
		case AUTH_REQ_SSPI:
#endif
			{
				int			r;

				pglock_thread();

				/*
				 * If we have both GSS and SSPI support compiled in, use SSPI
				 * support by default. This is overridable by a connection
				 * string parameter. Note that when using SSPI we still leave
				 * the negotiate parameter off, since we want SSPI to use the
				 * GSSAPI kerberos protocol. For actual SSPI negotiate
				 * protocol, we use AUTH_REQ_SSPI.
				 */
#if defined(ENABLE_GSS) && defined(ENABLE_SSPI)
				if (conn->gsslib && (pg_strcasecmp(conn->gsslib, "gssapi") == 0))
					r = pg_GSS_startup(conn, payloadlen);
				else
					r = pg_SSPI_startup(conn, 0, payloadlen);
#elif defined(ENABLE_GSS) && !defined(ENABLE_SSPI)
				r = pg_GSS_startup(conn, payloadlen);
#elif !defined(ENABLE_GSS) && defined(ENABLE_SSPI)
				r = pg_SSPI_startup(conn, 0, payloadlen);
#endif
				if (r != STATUS_OK)
				{
					/* Error message already filled in. */
					pgunlock_thread();
					return STATUS_ERROR;
				}
				pgunlock_thread();
			}
			break;

		case AUTH_REQ_GSS_CONT:
			{
				int			r;

				pglock_thread();
#if defined(ENABLE_GSS) && defined(ENABLE_SSPI)
				if (conn->usesspi)
					r = pg_SSPI_continue(conn, payloadlen);
				else
					r = pg_GSS_continue(conn, payloadlen);
#elif defined(ENABLE_GSS) && !defined(ENABLE_SSPI)
				r = pg_GSS_continue(conn, payloadlen);
#elif !defined(ENABLE_GSS) && defined(ENABLE_SSPI)
				r = pg_SSPI_continue(conn, payloadlen);
#endif
				if (r != STATUS_OK)
				{
					/* Error message already filled in. */
					pgunlock_thread();
					return STATUS_ERROR;
				}
				pgunlock_thread();
			}
			break;
#else							/* defined(ENABLE_GSS) || defined(ENABLE_SSPI) */
			/* No GSSAPI *or* SSPI support */
		case AUTH_REQ_GSS:
		case AUTH_REQ_GSS_CONT:
			libpq_append_conn_error(conn, "GSSAPI authentication not supported");
			return STATUS_ERROR;
#endif							/* defined(ENABLE_GSS) || defined(ENABLE_SSPI) */

#ifdef ENABLE_SSPI
		case AUTH_REQ_SSPI:

			/*
			 * SSPI has its own startup message so libpq can decide which
			 * method to use. Indicate to pg_SSPI_startup that we want SSPI
			 * negotiation instead of Kerberos.
			 */
			pglock_thread();
			if (pg_SSPI_startup(conn, 1, payloadlen) != STATUS_OK)
			{
				/* Error message already filled in. */
				pgunlock_thread();
				return STATUS_ERROR;
			}
			pgunlock_thread();
			break;
#else

			/*
			 * No SSPI support. However, if we have GSSAPI but not SSPI
			 * support, AUTH_REQ_SSPI will have been handled in the codepath
			 * for AUTH_REQ_GSS above, so don't duplicate the case label in
			 * that case.
			 */
#if !defined(ENABLE_GSS)
		case AUTH_REQ_SSPI:
			libpq_append_conn_error(conn, "SSPI authentication not supported");
			return STATUS_ERROR;
#endif							/* !define(ENABLE_GSS) */
#endif							/* ENABLE_SSPI */


		case AUTH_REQ_CRYPT:
			libpq_append_conn_error(conn, "Crypt authentication not supported");
			return STATUS_ERROR;

		case AUTH_REQ_MD5:
		case AUTH_REQ_PASSWORD:
			{
				char	   *password;

				conn->password_needed = true;
				password = conn->connhost[conn->whichhost].password;
				if (password == NULL)
					password = conn->pgpass;
				if (password == NULL || password[0] == '\0')
				{
					appendPQExpBufferStr(&conn->errorMessage,
										 PQnoPasswordSupplied);
					return STATUS_ERROR;
				}
				if (pg_password_sendauth(conn, password, areq) != STATUS_OK)
				{
					appendPQExpBufferStr(&conn->errorMessage,
										 "fe_sendauth: error sending password authentication\n");
					return STATUS_ERROR;
				}

				/* We expect no further authentication requests. */
				conn->client_finished_auth = true;
				break;
			}

		case AUTH_REQ_SASL:

			/*
			 * The request contains the name (as assigned by IANA) of the
			 * authentication mechanism.
			 */
			if (pg_SASL_init(conn, payloadlen) != STATUS_OK)
			{
				/* pg_SASL_init already set the error message */
				return STATUS_ERROR;
			}
			break;

		case AUTH_REQ_SASL_CONT:
		case AUTH_REQ_SASL_FIN:
			if (conn->sasl_state == NULL)
			{
				appendPQExpBufferStr(&conn->errorMessage,
									 "fe_sendauth: invalid authentication request from server: AUTH_REQ_SASL_CONT without AUTH_REQ_SASL\n");
				return STATUS_ERROR;
			}
			oldmsglen = conn->errorMessage.len;
			if (pg_SASL_continue(conn, payloadlen,
								 (areq == AUTH_REQ_SASL_FIN)) != STATUS_OK)
			{
				/* Use this message if pg_SASL_continue didn't supply one */
				if (conn->errorMessage.len == oldmsglen)
					appendPQExpBufferStr(&conn->errorMessage,
										 "fe_sendauth: error in SASL authentication\n");
				return STATUS_ERROR;
			}
			break;

		default:
			libpq_append_conn_error(conn, "authentication method %u not supported", areq);
			return STATUS_ERROR;
	}

	return STATUS_OK;
}


/*
 * pg_fe_getusername
 *
 * Returns a pointer to malloc'd space containing the name of the
 * specified user_id.  If there is an error, return NULL, and append
 * a suitable error message to *errorMessage if that's not NULL.
 *
 * Caution: on Windows, the user_id argument is ignored, and we always
 * fetch the current user's name.
 */
char *
pg_fe_getusername(uid_t user_id, PQExpBuffer errorMessage)
{
	char	   *result = NULL;
	const char *name = NULL;

#ifdef WIN32
	/* Microsoft recommends buffer size of UNLEN+1, where UNLEN = 256 */
	char		username[256 + 1];
	DWORD		namesize = sizeof(username);
#else
	char		pwdbuf[BUFSIZ];
#endif

#ifdef WIN32
	if (GetUserName(username, &namesize))
		name = username;
	else if (errorMessage)
		libpq_append_error(errorMessage,
						   "user name lookup failure: error code %lu",
						   GetLastError());
#else
	if (pg_get_user_name(user_id, pwdbuf, sizeof(pwdbuf)))
		name = pwdbuf;
	else if (errorMessage)
		appendPQExpBuffer(errorMessage, "%s\n", pwdbuf);
#endif

	if (name)
	{
		result = strdup(name);
		if (result == NULL && errorMessage)
			libpq_append_error(errorMessage, "out of memory");
	}

	return result;
}

/*
 * pg_fe_getauthname
 *
 * Returns a pointer to malloc'd space containing whatever name the user
 * has authenticated to the system.  If there is an error, return NULL,
 * and append a suitable error message to *errorMessage if that's not NULL.
 */
char *
pg_fe_getauthname(PQExpBuffer errorMessage)
{
#ifdef WIN32
	return pg_fe_getusername(0, errorMessage);
#else
	return pg_fe_getusername(geteuid(), errorMessage);
#endif
}


/*
 * PQencryptPassword -- exported routine to encrypt a password with MD5
 *
 * This function is equivalent to calling PQencryptPasswordConn with
 * "md5" as the encryption method, except that this doesn't require
 * a connection object.  This function is deprecated, use
 * PQencryptPasswordConn instead.
 */
char *
PQencryptPassword(const char *passwd, const char *user)
{
	char	   *crypt_pwd;
	const char *errstr = NULL;

	crypt_pwd = malloc(MD5_PASSWD_LEN + 1);
	if (!crypt_pwd)
		return NULL;

	if (!pg_md5_encrypt(passwd, user, strlen(user), crypt_pwd, &errstr))
	{
		free(crypt_pwd);
		return NULL;
	}

	return crypt_pwd;
}

/*
 * PQencryptPasswordConn -- exported routine to encrypt a password
 *
 * This is intended to be used by client applications that wish to send
 * commands like ALTER USER joe PASSWORD 'pwd'.  The password need not
 * be sent in cleartext if it is encrypted on the client side.  This is
 * good because it ensures the cleartext password won't end up in logs,
 * pg_stat displays, etc.  We export the function so that clients won't
 * be dependent on low-level details like whether the encryption is MD5
 * or something else.
 *
 * Arguments are a connection object, the cleartext password, the SQL
 * name of the user it is for, and a string indicating the algorithm to
 * use for encrypting the password.  If algorithm is NULL, this queries
 * the server for the current 'password_encryption' value.  If you wish
 * to avoid that, e.g. to avoid blocking, you can execute
 * 'show password_encryption' yourself before calling this function, and
 * pass it as the algorithm.
 *
 * Return value is a malloc'd string.  The client may assume the string
 * doesn't contain any special characters that would require escaping.
 * On error, an error message is stored in the connection object, and
 * returns NULL.
 */
char *
PQencryptPasswordConn(PGconn *conn, const char *passwd, const char *user,
					  const char *algorithm)
{
#define MAX_ALGORITHM_NAME_LEN 50
	char		algobuf[MAX_ALGORITHM_NAME_LEN + 1];
	char	   *crypt_pwd = NULL;

	if (!conn)
		return NULL;

	pqClearConnErrorState(conn);

	/* If no algorithm was given, ask the server. */
	if (algorithm == NULL)
	{
		PGresult   *res;
		char	   *val;

		res = PQexec(conn, "show password_encryption");
		if (res == NULL)
		{
			/* PQexec() should've set conn->errorMessage already */
			return NULL;
		}
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			/* PQexec() should've set conn->errorMessage already */
			PQclear(res);
			return NULL;
		}
		if (PQntuples(res) != 1 || PQnfields(res) != 1)
		{
			PQclear(res);
			libpq_append_conn_error(conn, "unexpected shape of result set returned for SHOW");
			return NULL;
		}
		val = PQgetvalue(res, 0, 0);

		if (strlen(val) > MAX_ALGORITHM_NAME_LEN)
		{
			PQclear(res);
			libpq_append_conn_error(conn, "\"password_encryption\" value too long");
			return NULL;
		}
		strcpy(algobuf, val);
		PQclear(res);

		algorithm = algobuf;
	}

	/*
	 * Also accept "on" and "off" as aliases for "md5", because
	 * password_encryption was a boolean before PostgreSQL 10.  We refuse to
	 * send the password in plaintext even if it was "off".
	 */
	if (strcmp(algorithm, "on") == 0 ||
		strcmp(algorithm, "off") == 0)
		algorithm = "md5";

	/*
	 * Ok, now we know what algorithm to use
	 */
	if (strcmp(algorithm, "scram-sha-256") == 0)
	{
		const char *errstr = NULL;

		crypt_pwd = pg_fe_scram_build_secret(passwd,
											 conn->scram_sha_256_iterations,
											 &errstr);
		if (!crypt_pwd)
			libpq_append_conn_error(conn, "could not encrypt password: %s", errstr);
	}
	else if (strcmp(algorithm, "md5") == 0)
	{
		crypt_pwd = malloc(MD5_PASSWD_LEN + 1);
		if (crypt_pwd)
		{
			const char *errstr = NULL;

			if (!pg_md5_encrypt(passwd, user, strlen(user), crypt_pwd, &errstr))
			{
				libpq_append_conn_error(conn, "could not encrypt password: %s", errstr);
				free(crypt_pwd);
				crypt_pwd = NULL;
			}
		}
		else
			libpq_append_conn_error(conn, "out of memory");
	}
	else
	{
		libpq_append_conn_error(conn, "unrecognized password encryption algorithm \"%s\"",
								algorithm);
		return NULL;
	}

	return crypt_pwd;
}

/*
 * PQchangePassword -- exported routine to change a password
 *
 * This is intended to be used by client applications that wish to
 * change the password for a user. The password is not sent in
 * cleartext because it is encrypted on the client side. This is
 * good because it ensures the cleartext password is never known by
 * the server, and therefore won't end up in logs, pg_stat displays,
 * etc. The password encryption is performed by PQencryptPasswordConn(),
 * which is passed a NULL for the algorithm argument. Hence encryption
 * is done according to the server's password_encryption
 * setting. We export the function so that clients won't be dependent
 * on the implementation specific details with respect to how the
 * server changes passwords.
 *
 * Arguments are a connection object, the SQL name of the target user,
 * and the cleartext password.
 *
 * Return value is the PGresult of the executed ALTER USER statement
 * or NULL if we never get there. The caller is responsible to PQclear()
 * the returned PGresult.
 *
 * PQresultStatus() should be called to check the return value for errors,
 * and PQerrorMessage() used to get more information about such errors.
 */
PGresult *
PQchangePassword(PGconn *conn, const char *user, const char *passwd)
{
	char	   *encrypted_password = PQencryptPasswordConn(conn, passwd,
														   user, NULL);

	if (!encrypted_password)
	{
		/* PQencryptPasswordConn() already registered the error */
		return NULL;
	}
	else
	{
		char	   *fmtpw = PQescapeLiteral(conn, encrypted_password,
											strlen(encrypted_password));

		/* no longer needed, so clean up now */
		PQfreemem(encrypted_password);

		if (!fmtpw)
		{
			/* PQescapeLiteral() already registered the error */
			return NULL;
		}
		else
		{
			char	   *fmtuser = PQescapeIdentifier(conn, user, strlen(user));

			if (!fmtuser)
			{
				/* PQescapeIdentifier() already registered the error */
				PQfreemem(fmtpw);
				return NULL;
			}
			else
			{
				PQExpBufferData buf;
				PGresult   *res;

				initPQExpBuffer(&buf);
				printfPQExpBuffer(&buf, "ALTER USER %s PASSWORD %s",
								  fmtuser, fmtpw);

				res = PQexec(conn, buf.data);

				/* clean up */
				termPQExpBuffer(&buf);
				PQfreemem(fmtuser);
				PQfreemem(fmtpw);

				return res;
			}
		}
	}
}
