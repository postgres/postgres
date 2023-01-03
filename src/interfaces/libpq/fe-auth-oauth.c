/*-------------------------------------------------------------------------
 *
 * fe-auth-oauth.c
 *	   The front-end (client) implementation of OAuth/OIDC authentication.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-auth-oauth.c
 *
 *-------------------------------------------------------------------------
 */

#include <iddawc.h>

#include "postgres_fe.h"

#include "common/base64.h"
#include "common/hmac.h"
#include "common/jsonapi.h"
#include "common/oauth-common.h"
#include "fe-auth.h"
#include "mb/pg_wchar.h"

/* The exported OAuth callback mechanism. */
static void *oauth_init(PGconn *conn, const char *password,
						const char *sasl_mechanism);
static void oauth_exchange(void *opaq, bool final,
						   char *input, int inputlen,
						   char **output, int *outputlen,
						   bool *done, bool *success);
static bool oauth_channel_bound(void *opaq);
static void oauth_free(void *opaq);

const pg_fe_sasl_mech pg_oauth_mech = {
	oauth_init,
	oauth_exchange,
	oauth_channel_bound,
	oauth_free,
};

typedef enum
{
	FE_OAUTH_INIT,
	FE_OAUTH_BEARER_SENT,
	FE_OAUTH_SERVER_ERROR,
} fe_oauth_state_enum;

typedef struct
{
	fe_oauth_state_enum state;

	PGconn	   *conn;
} fe_oauth_state;

static void *
oauth_init(PGconn *conn, const char *password,
		   const char *sasl_mechanism)
{
	fe_oauth_state *state;

	/*
	 * We only support one SASL mechanism here; anything else is programmer
	 * error.
	 */
	Assert(sasl_mechanism != NULL);
	Assert(!strcmp(sasl_mechanism, OAUTHBEARER_NAME));

	state = malloc(sizeof(*state));
	if (!state)
		return NULL;

	state->state = FE_OAUTH_INIT;
	state->conn = conn;

	return state;
}

static const char *
iddawc_error_string(int errcode)
{
	switch (errcode)
	{
		case I_OK:
			return "I_OK";

		case I_ERROR:
			return "I_ERROR";

		case I_ERROR_PARAM:
			return "I_ERROR_PARAM";

		case I_ERROR_MEMORY:
			return "I_ERROR_MEMORY";

		case I_ERROR_UNAUTHORIZED:
			return "I_ERROR_UNAUTHORIZED";

		case I_ERROR_SERVER:
			return "I_ERROR_SERVER";
	}

	return "<unknown>";
}

static void
iddawc_error(PGconn *conn, int errcode, const char *msg)
{
	appendPQExpBufferStr(&conn->errorMessage, libpq_gettext(msg));
	appendPQExpBuffer(&conn->errorMessage,
					  libpq_gettext(" (iddawc error %s)\n"),
					  iddawc_error_string(errcode));
}

static void
iddawc_request_error(PGconn *conn, struct _i_session *i, int err, const char *msg)
{
	const char *error_code;
	const char *desc;

	appendPQExpBuffer(&conn->errorMessage, "%s: ", libpq_gettext(msg));

	error_code = i_get_str_parameter(i, I_OPT_ERROR);
	if (!error_code)
	{
		/*
		 * The server didn't give us any useful information, so just print the
		 * error code.
		 */
		appendPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("(iddawc error %s)\n"),
						  iddawc_error_string(err));
		return;
	}

	/* If the server gave a string description, print that too. */
	desc = i_get_str_parameter(i, I_OPT_ERROR_DESCRIPTION);
	if (desc)
		appendPQExpBuffer(&conn->errorMessage, "%s ", desc);

	appendPQExpBuffer(&conn->errorMessage, "(%s)\n", error_code);
}

static char *
get_auth_token(PGconn *conn)
{
	PQExpBuffer	token_buf = NULL;
	struct _i_session session;
	int			err;
	int			auth_method;
	bool		user_prompted = false;
	const char *verification_uri;
	const char *user_code;
	const char *access_token;
	const char *token_type;
	char	   *token = NULL;

	if (!conn->oauth_discovery_uri)
		return strdup(""); /* ask the server for one */

	i_init_session(&session);

	if (!conn->oauth_client_id)
	{
		/* We can't talk to a server without a client identifier. */
		appendPQExpBufferStr(&conn->errorMessage,
							 libpq_gettext("no oauth_client_id is set for the connection"));
		goto cleanup;
	}

	token_buf = createPQExpBuffer();

	if (!token_buf)
		goto cleanup;

	err = i_set_str_parameter(&session, I_OPT_OPENID_CONFIG_ENDPOINT, conn->oauth_discovery_uri);
	if (err)
	{
		iddawc_error(conn, err, "failed to set OpenID config endpoint");
		goto cleanup;
	}

	err = i_get_openid_config(&session);
	if (err)
	{
		iddawc_error(conn, err, "failed to fetch OpenID discovery document");
		goto cleanup;
	}

	if (!i_get_str_parameter(&session, I_OPT_TOKEN_ENDPOINT))
	{
		appendPQExpBufferStr(&conn->errorMessage,
							 libpq_gettext("issuer has no token endpoint"));
		goto cleanup;
	}

	if (!i_get_str_parameter(&session, I_OPT_DEVICE_AUTHORIZATION_ENDPOINT))
	{
		appendPQExpBufferStr(&conn->errorMessage,
							 libpq_gettext("issuer does not support device authorization"));
		goto cleanup;
	}

	err = i_set_response_type(&session, I_RESPONSE_TYPE_DEVICE_CODE);
	if (err)
	{
		iddawc_error(conn, err, "failed to set device code response type");
		goto cleanup;
	}

	auth_method = I_TOKEN_AUTH_METHOD_NONE;
	if (conn->oauth_client_secret && *conn->oauth_client_secret)
		auth_method = I_TOKEN_AUTH_METHOD_SECRET_BASIC;

	err = i_set_parameter_list(&session,
		I_OPT_CLIENT_ID, conn->oauth_client_id,
		I_OPT_CLIENT_SECRET, conn->oauth_client_secret,
		I_OPT_TOKEN_METHOD, auth_method,
		I_OPT_SCOPE, conn->oauth_scope,
		I_OPT_NONE
	);
	if (err)
	{
		iddawc_error(conn, err, "failed to set client identifier");
		goto cleanup;
	}

	err = i_run_device_auth_request(&session);
	if (err)
	{
		iddawc_request_error(conn, &session, err,
							"failed to obtain device authorization");
		goto cleanup;
	}

	verification_uri = i_get_str_parameter(&session, I_OPT_DEVICE_AUTH_VERIFICATION_URI);
	if (!verification_uri)
	{
		appendPQExpBufferStr(&conn->errorMessage,
							 libpq_gettext("issuer did not provide a verification URI"));
		goto cleanup;
	}

	user_code = i_get_str_parameter(&session, I_OPT_DEVICE_AUTH_USER_CODE);
	if (!user_code)
	{
		appendPQExpBufferStr(&conn->errorMessage,
							 libpq_gettext("issuer did not provide a user code"));
		goto cleanup;
	}

	/*
	 * Poll the token endpoint until either the user logs in and authorizes the
	 * use of a token, or a hard failure occurs. We perform one ping _before_
	 * prompting the user, so that we don't make them do the work of logging in
	 * only to find that the token endpoint is completely unreachable.
	 */
	err = i_run_token_request(&session);
	while (err)
	{
		const char *error_code;
		uint		interval;

		error_code = i_get_str_parameter(&session, I_OPT_ERROR);

		/*
		 * authorization_pending and slow_down are the only acceptable errors;
		 * anything else and we bail.
		 */
		if (!error_code || (strcmp(error_code, "authorization_pending")
							&& strcmp(error_code, "slow_down")))
		{
			iddawc_request_error(conn, &session, err,
								"OAuth token retrieval failed");
			goto cleanup;
		}

		if (!user_prompted)
		{
			/*
			 * Now that we know the token endpoint isn't broken, give the user
			 * the login instructions.
			 */
			pqInternalNotice(&conn->noticeHooks,
							 "Visit %s and enter the code: %s",
							 verification_uri, user_code);

			user_prompted = true;
		}

		/*
		 * We are required to wait between polls; the server tells us how long.
		 * TODO: if interval's not set, we need to default to five seconds
		 * TODO: sanity check the interval
		 */
		interval = i_get_int_parameter(&session, I_OPT_DEVICE_AUTH_INTERVAL);

		/*
		 * A slow_down error requires us to permanently increase our retry
		 * interval by five seconds. RFC 8628, Sec. 3.5.
		 */
		if (!strcmp(error_code, "slow_down"))
		{
			interval += 5;
			i_set_int_parameter(&session, I_OPT_DEVICE_AUTH_INTERVAL, interval);
		}

		sleep(interval);

		/*
		 * XXX Reset the error code before every call, because iddawc won't do
		 * that for us. This matters if the server first sends a "pending" error
		 * code, then later hard-fails without sending an error code to
		 * overwrite the first one.
		 *
		 * That we have to do this at all seems like a bug in iddawc.
		 */
		i_set_str_parameter(&session, I_OPT_ERROR, NULL);

		err = i_run_token_request(&session);
	}

	access_token = i_get_str_parameter(&session, I_OPT_ACCESS_TOKEN);
	token_type = i_get_str_parameter(&session, I_OPT_TOKEN_TYPE);

	if (!access_token || !token_type || strcasecmp(token_type, "Bearer"))
	{
		appendPQExpBufferStr(&conn->errorMessage,
							 libpq_gettext("issuer did not provide a bearer token"));
		goto cleanup;
	}

	appendPQExpBufferStr(token_buf, "Bearer ");
	appendPQExpBufferStr(token_buf, access_token);

	if (PQExpBufferBroken(token_buf))
		goto cleanup;

	token = strdup(token_buf->data);

cleanup:
	if (token_buf)
		destroyPQExpBuffer(token_buf);
	i_clean_session(&session);

	return token;
}

#define kvsep "\x01"

static char *
client_initial_response(PGconn *conn)
{
	static const char * const resp_format = "n,," kvsep "auth=%s" kvsep kvsep;

	PQExpBuffer	token_buf;
	PQExpBuffer	discovery_buf = NULL;
	char	   *token = NULL;
	char	   *response = NULL;

	token_buf = createPQExpBuffer();
	if (!token_buf)
		goto cleanup;

	/*
	 * If we don't yet have a discovery URI, but the user gave us an explicit
	 * issuer, use the .well-known discovery URI for that issuer.
	 */
	if (!conn->oauth_discovery_uri && conn->oauth_issuer)
	{
		discovery_buf = createPQExpBuffer();
		if (!discovery_buf)
			goto cleanup;

		appendPQExpBufferStr(discovery_buf, conn->oauth_issuer);
		appendPQExpBufferStr(discovery_buf, "/.well-known/openid-configuration");

		if (PQExpBufferBroken(discovery_buf))
			goto cleanup;

		conn->oauth_discovery_uri = strdup(discovery_buf->data);
	}

	token = get_auth_token(conn);
	if (!token)
		goto cleanup;

	appendPQExpBuffer(token_buf, resp_format, token);
	if (PQExpBufferBroken(token_buf))
		goto cleanup;

	response = strdup(token_buf->data);

cleanup:
	if (token)
		free(token);
	if (discovery_buf)
		destroyPQExpBuffer(discovery_buf);
	if (token_buf)
		destroyPQExpBuffer(token_buf);

	return response;
}

#define ERROR_STATUS_FIELD "status"
#define ERROR_SCOPE_FIELD "scope"
#define ERROR_OPENID_CONFIGURATION_FIELD "openid-configuration"

struct json_ctx
{
	char		   *errmsg; /* any non-NULL value stops all processing */
	PQExpBufferData errbuf; /* backing memory for errmsg */
	int				nested; /* nesting level (zero is the top) */

	const char	   *target_field_name; /* points to a static allocation */
	char		  **target_field;      /* see below */

	/* target_field, if set, points to one of the following: */
	char		   *status;
	char		   *scope;
	char		   *discovery_uri;
};

#define oauth_json_has_error(ctx) \
	(PQExpBufferDataBroken((ctx)->errbuf) || (ctx)->errmsg)

#define oauth_json_set_error(ctx, ...) \
	do { \
		appendPQExpBuffer(&(ctx)->errbuf, __VA_ARGS__); \
		(ctx)->errmsg = (ctx)->errbuf.data; \
	} while (0)

static void
oauth_json_object_start(void *state)
{
	struct json_ctx	   *ctx = state;

	if (oauth_json_has_error(ctx))
		return; /* short-circuit */

	if (ctx->target_field)
	{
		Assert(ctx->nested == 1);

		oauth_json_set_error(ctx,
							 libpq_gettext("field \"%s\" must be a string"),
							 ctx->target_field_name);
	}

	++ctx->nested;
}

static void
oauth_json_object_end(void *state)
{
	struct json_ctx	   *ctx = state;

	if (oauth_json_has_error(ctx))
		return; /* short-circuit */

	--ctx->nested;
}

static void
oauth_json_object_field_start(void *state, char *name, bool isnull)
{
	struct json_ctx	   *ctx = state;

	if (oauth_json_has_error(ctx))
	{
		/* short-circuit */
		free(name);
		return;
	}

	if (ctx->nested == 1)
	{
		if (!strcmp(name, ERROR_STATUS_FIELD))
		{
			ctx->target_field_name = ERROR_STATUS_FIELD;
			ctx->target_field = &ctx->status;
		}
		else if (!strcmp(name, ERROR_SCOPE_FIELD))
		{
			ctx->target_field_name = ERROR_SCOPE_FIELD;
			ctx->target_field = &ctx->scope;
		}
		else if (!strcmp(name, ERROR_OPENID_CONFIGURATION_FIELD))
		{
			ctx->target_field_name = ERROR_OPENID_CONFIGURATION_FIELD;
			ctx->target_field = &ctx->discovery_uri;
		}
	}

	free(name);
}

static void
oauth_json_array_start(void *state)
{
	struct json_ctx	   *ctx = state;

	if (oauth_json_has_error(ctx))
		return; /* short-circuit */

	if (!ctx->nested)
	{
		ctx->errmsg = libpq_gettext("top-level element must be an object");
	}
	else if (ctx->target_field)
	{
		Assert(ctx->nested == 1);

		oauth_json_set_error(ctx,
							 libpq_gettext("field \"%s\" must be a string"),
							 ctx->target_field_name);
	}
}

static void
oauth_json_scalar(void *state, char *token, JsonTokenType type)
{
	struct json_ctx	   *ctx = state;

	if (oauth_json_has_error(ctx))
	{
		/* short-circuit */
		free(token);
		return;
	}

	if (!ctx->nested)
	{
		ctx->errmsg = libpq_gettext("top-level element must be an object");
	}
	else if (ctx->target_field)
	{
		Assert(ctx->nested == 1);

		if (type == JSON_TOKEN_STRING)
		{
			*ctx->target_field = token;

			ctx->target_field = NULL;
			ctx->target_field_name = NULL;

			return; /* don't free the token we're using */
		}

		oauth_json_set_error(ctx,
							 libpq_gettext("field \"%s\" must be a string"),
							 ctx->target_field_name);
	}

	free(token);
}

static bool
handle_oauth_sasl_error(PGconn *conn, char *msg, int msglen)
{
	JsonLexContext		lex = {0};
	JsonSemAction		sem = {0};
	JsonParseErrorType	err;
	struct json_ctx		ctx = {0};
	char			   *errmsg = NULL;

	/* Sanity check. */
	if (strlen(msg) != msglen)
	{
		appendPQExpBufferStr(&conn->errorMessage,
							 libpq_gettext("server's error message contained an embedded NULL"));
		return false;
	}

	initJsonLexContextCstringLen(&lex, msg, msglen, PG_UTF8, true);

	initPQExpBuffer(&ctx.errbuf);
	sem.semstate = &ctx;

	sem.object_start = oauth_json_object_start;
	sem.object_end = oauth_json_object_end;
	sem.object_field_start = oauth_json_object_field_start;
	sem.array_start = oauth_json_array_start;
	sem.scalar = oauth_json_scalar;

	err = pg_parse_json(&lex, &sem);

	if (err != JSON_SUCCESS)
	{
		errmsg = json_errdetail(err, &lex);
	}
	else if (PQExpBufferDataBroken(ctx.errbuf))
	{
		errmsg = libpq_gettext("out of memory");
	}
	else if (ctx.errmsg)
	{
		errmsg = ctx.errmsg;
	}

	if (errmsg)
		appendPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("failed to parse server's error response: %s"),
						  errmsg);

	/* Don't need the error buffer or the JSON lexer anymore. */
	termPQExpBuffer(&ctx.errbuf);
	termJsonLexContext(&lex);

	if (errmsg)
		return false;

	/* TODO: what if these override what the user already specified? */
	if (ctx.discovery_uri)
	{
		if (conn->oauth_discovery_uri)
			free(conn->oauth_discovery_uri);

		conn->oauth_discovery_uri = ctx.discovery_uri;
	}

	if (ctx.scope)
	{
		if (conn->oauth_scope)
			free(conn->oauth_scope);

		conn->oauth_scope = ctx.scope;
	}
	/* TODO: missing error scope should clear any existing connection scope */

	if (!ctx.status)
	{
		appendPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("server sent error response without a status"));
		return false;
	}

	if (!strcmp(ctx.status, "invalid_token"))
	{
		/*
		 * invalid_token is the only error code we'll automatically retry for,
		 * but only if we have enough information to do so.
		 */
		if (conn->oauth_discovery_uri)
			conn->oauth_want_retry = true;
	}
	/* TODO: include status in hard failure message */

	return true;
}

static void
oauth_exchange(void *opaq, bool final,
			   char *input, int inputlen,
			   char **output, int *outputlen,
			   bool *done, bool *success)
{
	fe_oauth_state *state = opaq;
	PGconn	   *conn = state->conn;

	*done = false;
	*success = false;
	*output = NULL;
	*outputlen = 0;

	switch (state->state)
	{
		case FE_OAUTH_INIT:
			Assert(inputlen == -1);

			*output = client_initial_response(conn);
			if (!*output)
				goto error;

			*outputlen = strlen(*output);
			state->state = FE_OAUTH_BEARER_SENT;

			break;

		case FE_OAUTH_BEARER_SENT:
			if (final)
			{
				/* TODO: ensure there is no message content here. */
				*done = true;
				*success = true;

				break;
			}

			/*
			 * Error message sent by the server.
			 */
			if (!handle_oauth_sasl_error(conn, input, inputlen))
				goto error;

			/*
			 * Respond with the required dummy message (RFC 7628, sec. 3.2.3).
			 */
			*output = strdup(kvsep);
			*outputlen = strlen(*output); /* == 1 */

			state->state = FE_OAUTH_SERVER_ERROR;
			break;

		case FE_OAUTH_SERVER_ERROR:
			/*
			 * After an error, the server should send an error response to fail
			 * the SASL handshake, which is handled in higher layers.
			 *
			 * If we get here, the server either sent *another* challenge which
			 * isn't defined in the RFC, or completed the handshake successfully
			 * after telling us it was going to fail. Neither is acceptable.
			 */
			appendPQExpBufferStr(&conn->errorMessage,
								 libpq_gettext("server sent additional OAuth data after error\n"));
			goto error;

		default:
			appendPQExpBufferStr(&conn->errorMessage,
								 libpq_gettext("invalid OAuth exchange state\n"));
			goto error;
	}

	return;

error:
	*done = true;
	*success = false;
}

static bool
oauth_channel_bound(void *opaq)
{
	/* This mechanism does not support channel binding. */
	return false;
}

static void
oauth_free(void *opaq)
{
	fe_oauth_state *state = opaq;

	free(state);
}
