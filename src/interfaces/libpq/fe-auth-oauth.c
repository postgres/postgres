/*-------------------------------------------------------------------------
 *
 * fe-auth-oauth.c
 *	   The front-end (client) implementation of OAuth/OIDC authentication
 *	   using the SASL OAUTHBEARER mechanism.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-auth-oauth.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "common/base64.h"
#include "common/hmac.h"
#include "common/jsonapi.h"
#include "common/oauth-common.h"
#include "fe-auth.h"
#include "fe-auth-oauth.h"
#include "mb/pg_wchar.h"

/* The exported OAuth callback mechanism. */
static void *oauth_init(PGconn *conn, const char *password,
						const char *sasl_mechanism);
static SASLStatus oauth_exchange(void *opaq, bool final,
								 char *input, int inputlen,
								 char **output, int *outputlen);
static bool oauth_channel_bound(void *opaq);
static void oauth_free(void *opaq);

const pg_fe_sasl_mech pg_oauth_mech = {
	oauth_init,
	oauth_exchange,
	oauth_channel_bound,
	oauth_free,
};

/*
 * Initializes mechanism state for OAUTHBEARER.
 *
 * For a full description of the API, see libpq/fe-auth-sasl.h.
 */
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
	Assert(strcmp(sasl_mechanism, OAUTHBEARER_NAME) == 0);

	state = calloc(1, sizeof(*state));
	if (!state)
		return NULL;

	state->step = FE_OAUTH_INIT;
	state->conn = conn;

	return state;
}

/*
 * Frees the state allocated by oauth_init().
 *
 * This handles only mechanism state tied to the connection lifetime; state
 * stored in state->async_ctx is freed up either immediately after the
 * authentication handshake succeeds, or before the mechanism is cleaned up on
 * failure. See pg_fe_cleanup_oauth_flow() and cleanup_user_oauth_flow().
 */
static void
oauth_free(void *opaq)
{
	fe_oauth_state *state = opaq;

	/* Any async authentication state should have been cleaned up already. */
	Assert(!state->async_ctx);

	free(state);
}

#define kvsep "\x01"

/*
 * Constructs an OAUTHBEARER client initial response (RFC 7628, Sec. 3.1).
 *
 * If discover is true, the initial response will contain a request for the
 * server's required OAuth parameters (Sec. 4.3). Otherwise, conn->token must
 * be set; it will be sent as the connection's bearer token.
 *
 * Returns the response as a null-terminated string, or NULL on error.
 */
static char *
client_initial_response(PGconn *conn, bool discover)
{
	static const char *const resp_format = "n,," kvsep "auth=%s%s" kvsep kvsep;

	PQExpBufferData buf;
	const char *authn_scheme;
	char	   *response = NULL;
	const char *token = conn->oauth_token;

	if (discover)
	{
		/* Parameter discovery uses a completely empty auth value. */
		authn_scheme = token = "";
	}
	else
	{
		/*
		 * Use a Bearer authentication scheme (RFC 6750, Sec. 2.1). A trailing
		 * space is used as a separator.
		 */
		authn_scheme = "Bearer ";

		/* conn->token must have been set in this case. */
		if (!token)
		{
			Assert(false);
			libpq_append_conn_error(conn,
									"internal error: no OAuth token was set for the connection");
			return NULL;
		}
	}

	initPQExpBuffer(&buf);
	appendPQExpBuffer(&buf, resp_format, authn_scheme, token);

	if (!PQExpBufferDataBroken(buf))
		response = strdup(buf.data);
	termPQExpBuffer(&buf);

	if (!response)
		libpq_append_conn_error(conn, "out of memory");

	return response;
}

/*
 * JSON Parser (for the OAUTHBEARER error result)
 */

/* Relevant JSON fields in the error result object. */
#define ERROR_STATUS_FIELD "status"
#define ERROR_SCOPE_FIELD "scope"
#define ERROR_OPENID_CONFIGURATION_FIELD "openid-configuration"

struct json_ctx
{
	char	   *errmsg;			/* any non-NULL value stops all processing */
	PQExpBufferData errbuf;		/* backing memory for errmsg */
	int			nested;			/* nesting level (zero is the top) */

	const char *target_field_name;	/* points to a static allocation */
	char	  **target_field;	/* see below */

	/* target_field, if set, points to one of the following: */
	char	   *status;
	char	   *scope;
	char	   *discovery_uri;
};

#define oauth_json_has_error(ctx) \
	(PQExpBufferDataBroken((ctx)->errbuf) || (ctx)->errmsg)

#define oauth_json_set_error(ctx, ...) \
	do { \
		appendPQExpBuffer(&(ctx)->errbuf, __VA_ARGS__); \
		(ctx)->errmsg = (ctx)->errbuf.data; \
	} while (0)

static JsonParseErrorType
oauth_json_object_start(void *state)
{
	struct json_ctx *ctx = state;

	if (ctx->target_field)
	{
		Assert(ctx->nested == 1);

		oauth_json_set_error(ctx,
							 libpq_gettext("field \"%s\" must be a string"),
							 ctx->target_field_name);
	}

	++ctx->nested;
	return oauth_json_has_error(ctx) ? JSON_SEM_ACTION_FAILED : JSON_SUCCESS;
}

static JsonParseErrorType
oauth_json_object_end(void *state)
{
	struct json_ctx *ctx = state;

	--ctx->nested;
	return JSON_SUCCESS;
}

static JsonParseErrorType
oauth_json_object_field_start(void *state, char *name, bool isnull)
{
	struct json_ctx *ctx = state;

	/* Only top-level keys are considered. */
	if (ctx->nested == 1)
	{
		if (strcmp(name, ERROR_STATUS_FIELD) == 0)
		{
			ctx->target_field_name = ERROR_STATUS_FIELD;
			ctx->target_field = &ctx->status;
		}
		else if (strcmp(name, ERROR_SCOPE_FIELD) == 0)
		{
			ctx->target_field_name = ERROR_SCOPE_FIELD;
			ctx->target_field = &ctx->scope;
		}
		else if (strcmp(name, ERROR_OPENID_CONFIGURATION_FIELD) == 0)
		{
			ctx->target_field_name = ERROR_OPENID_CONFIGURATION_FIELD;
			ctx->target_field = &ctx->discovery_uri;
		}
	}

	return JSON_SUCCESS;
}

static JsonParseErrorType
oauth_json_array_start(void *state)
{
	struct json_ctx *ctx = state;

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

	return oauth_json_has_error(ctx) ? JSON_SEM_ACTION_FAILED : JSON_SUCCESS;
}

static JsonParseErrorType
oauth_json_scalar(void *state, char *token, JsonTokenType type)
{
	struct json_ctx *ctx = state;

	if (!ctx->nested)
	{
		ctx->errmsg = libpq_gettext("top-level element must be an object");
		return JSON_SEM_ACTION_FAILED;
	}

	if (ctx->target_field)
	{
		if (ctx->nested != 1)
		{
			/*
			 * ctx->target_field should not have been set for nested keys.
			 * Assert and don't continue any further for production builds.
			 */
			Assert(false);
			oauth_json_set_error(ctx,
								 "internal error: target scalar found at nesting level %d during OAUTHBEARER parsing",
								 ctx->nested);
			return JSON_SEM_ACTION_FAILED;
		}

		/*
		 * We don't allow duplicate field names; error out if the target has
		 * already been set.
		 */
		if (*ctx->target_field)
		{
			oauth_json_set_error(ctx,
								 libpq_gettext("field \"%s\" is duplicated"),
								 ctx->target_field_name);
			return JSON_SEM_ACTION_FAILED;
		}

		/* The only fields we support are strings. */
		if (type != JSON_TOKEN_STRING)
		{
			oauth_json_set_error(ctx,
								 libpq_gettext("field \"%s\" must be a string"),
								 ctx->target_field_name);
			return JSON_SEM_ACTION_FAILED;
		}

		*ctx->target_field = strdup(token);
		if (!*ctx->target_field)
			return JSON_OUT_OF_MEMORY;

		ctx->target_field = NULL;
		ctx->target_field_name = NULL;
	}
	else
	{
		/* otherwise we just ignore it */
	}

	return JSON_SUCCESS;
}

#define HTTPS_SCHEME "https://"
#define HTTP_SCHEME "http://"

/* We support both well-known suffixes defined by RFC 8414. */
#define WK_PREFIX "/.well-known/"
#define OPENID_WK_SUFFIX "openid-configuration"
#define OAUTH_WK_SUFFIX "oauth-authorization-server"

/*
 * Derives an issuer identifier from one of our recognized .well-known URIs,
 * using the rules in RFC 8414.
 */
static char *
issuer_from_well_known_uri(PGconn *conn, const char *wkuri)
{
	const char *authority_start = NULL;
	const char *wk_start;
	const char *wk_end;
	char	   *issuer;
	ptrdiff_t	start_offset,
				end_offset;
	size_t		end_len;

	/*
	 * https:// is required for issuer identifiers (RFC 8414, Sec. 2; OIDC
	 * Discovery 1.0, Sec. 3). This is a case-insensitive comparison at this
	 * level (but issuer identifier comparison at the level above this is
	 * case-sensitive, so in practice it's probably moot).
	 */
	if (pg_strncasecmp(wkuri, HTTPS_SCHEME, strlen(HTTPS_SCHEME)) == 0)
		authority_start = wkuri + strlen(HTTPS_SCHEME);

	if (!authority_start
		&& oauth_unsafe_debugging_enabled()
		&& pg_strncasecmp(wkuri, HTTP_SCHEME, strlen(HTTP_SCHEME)) == 0)
	{
		/* Allow http:// for testing only. */
		authority_start = wkuri + strlen(HTTP_SCHEME);
	}

	if (!authority_start)
	{
		libpq_append_conn_error(conn,
								"OAuth discovery URI \"%s\" must use HTTPS",
								wkuri);
		return NULL;
	}

	/*
	 * Well-known URIs in general may support queries and fragments, but the
	 * two types we support here do not. (They must be constructed from the
	 * components of issuer identifiers, which themselves may not contain any
	 * queries or fragments.)
	 *
	 * It's important to check this first, to avoid getting tricked later by a
	 * prefix buried inside a query or fragment.
	 */
	if (strpbrk(authority_start, "?#") != NULL)
	{
		libpq_append_conn_error(conn,
								"OAuth discovery URI \"%s\" must not contain query or fragment components",
								wkuri);
		return NULL;
	}

	/*
	 * Find the start of the .well-known prefix. IETF rules (RFC 8615) state
	 * this must be at the beginning of the path component, but OIDC defined
	 * it at the end instead (OIDC Discovery 1.0, Sec. 4), so we have to
	 * search for it anywhere.
	 */
	wk_start = strstr(authority_start, WK_PREFIX);
	if (!wk_start)
	{
		libpq_append_conn_error(conn,
								"OAuth discovery URI \"%s\" is not a .well-known URI",
								wkuri);
		return NULL;
	}

	/*
	 * Now find the suffix type. We only support the two defined in OIDC
	 * Discovery 1.0 and RFC 8414.
	 */
	wk_end = wk_start + strlen(WK_PREFIX);

	if (strncmp(wk_end, OPENID_WK_SUFFIX, strlen(OPENID_WK_SUFFIX)) == 0)
		wk_end += strlen(OPENID_WK_SUFFIX);
	else if (strncmp(wk_end, OAUTH_WK_SUFFIX, strlen(OAUTH_WK_SUFFIX)) == 0)
		wk_end += strlen(OAUTH_WK_SUFFIX);
	else
		wk_end = NULL;

	/*
	 * Even if there's a match, we still need to check to make sure the suffix
	 * takes up the entire path segment, to weed out constructions like
	 * "/.well-known/openid-configuration-bad".
	 */
	if (!wk_end || (*wk_end != '/' && *wk_end != '\0'))
	{
		libpq_append_conn_error(conn,
								"OAuth discovery URI \"%s\" uses an unsupported .well-known suffix",
								wkuri);
		return NULL;
	}

	/*
	 * Finally, make sure the .well-known components are provided either as a
	 * prefix (IETF style) or as a postfix (OIDC style). In other words,
	 * "https://localhost/a/.well-known/openid-configuration/b" is not allowed
	 * to claim association with "https://localhost/a/b".
	 */
	if (*wk_end != '\0')
	{
		/*
		 * It's not at the end, so it's required to be at the beginning at the
		 * path. Find the starting slash.
		 */
		const char *path_start;

		path_start = strchr(authority_start, '/');
		Assert(path_start);		/* otherwise we wouldn't have found WK_PREFIX */

		if (wk_start != path_start)
		{
			libpq_append_conn_error(conn,
									"OAuth discovery URI \"%s\" uses an invalid format",
									wkuri);
			return NULL;
		}
	}

	/* Checks passed! Now build the issuer. */
	issuer = strdup(wkuri);
	if (!issuer)
	{
		libpq_append_conn_error(conn, "out of memory");
		return NULL;
	}

	/*
	 * The .well-known components are from [wk_start, wk_end). Remove those to
	 * form the issuer ID, by shifting the path suffix (which may be empty)
	 * leftwards.
	 */
	start_offset = wk_start - wkuri;
	end_offset = wk_end - wkuri;
	end_len = strlen(wk_end) + 1;	/* move the NULL terminator too */

	memmove(issuer + start_offset, issuer + end_offset, end_len);

	return issuer;
}

/*
 * Parses the server error result (RFC 7628, Sec. 3.2.2) contained in msg and
 * stores any discovered openid_configuration and scope settings for the
 * connection.
 */
static bool
handle_oauth_sasl_error(PGconn *conn, const char *msg, int msglen)
{
	JsonLexContext lex = {0};
	JsonSemAction sem = {0};
	JsonParseErrorType err;
	struct json_ctx ctx = {0};
	char	   *errmsg = NULL;
	bool		success = false;

	Assert(conn->oauth_issuer_id);	/* ensured by setup_oauth_parameters() */

	/* Sanity check. */
	if (strlen(msg) != msglen)
	{
		libpq_append_conn_error(conn,
								"server's error message contained an embedded NULL, and was discarded");
		return false;
	}

	/*
	 * pg_parse_json doesn't validate the incoming UTF-8, so we have to check
	 * that up front.
	 */
	if (pg_encoding_verifymbstr(PG_UTF8, msg, msglen) != msglen)
	{
		libpq_append_conn_error(conn,
								"server's error response is not valid UTF-8");
		return false;
	}

	makeJsonLexContextCstringLen(&lex, msg, msglen, PG_UTF8, true);
	setJsonLexContextOwnsTokens(&lex, true);	/* must not leak on error */

	initPQExpBuffer(&ctx.errbuf);
	sem.semstate = &ctx;

	sem.object_start = oauth_json_object_start;
	sem.object_end = oauth_json_object_end;
	sem.object_field_start = oauth_json_object_field_start;
	sem.array_start = oauth_json_array_start;
	sem.scalar = oauth_json_scalar;

	err = pg_parse_json(&lex, &sem);

	if (err == JSON_SEM_ACTION_FAILED)
	{
		if (PQExpBufferDataBroken(ctx.errbuf))
			errmsg = libpq_gettext("out of memory");
		else if (ctx.errmsg)
			errmsg = ctx.errmsg;
		else
		{
			/*
			 * Developer error: one of the action callbacks didn't call
			 * oauth_json_set_error() before erroring out.
			 */
			Assert(oauth_json_has_error(&ctx));
			errmsg = "<unexpected empty error>";
		}
	}
	else if (err != JSON_SUCCESS)
		errmsg = json_errdetail(err, &lex);

	if (errmsg)
		libpq_append_conn_error(conn,
								"failed to parse server's error response: %s",
								errmsg);

	/* Don't need the error buffer or the JSON lexer anymore. */
	termPQExpBuffer(&ctx.errbuf);
	freeJsonLexContext(&lex);

	if (errmsg)
		goto cleanup;

	if (ctx.discovery_uri)
	{
		char	   *discovery_issuer;

		/*
		 * The URI MUST correspond to our existing issuer, to avoid mix-ups.
		 *
		 * Issuer comparison is done byte-wise, rather than performing any URL
		 * normalization; this follows the suggestions for issuer comparison
		 * in RFC 9207 Sec. 2.4 (which requires simple string comparison) and
		 * vastly simplifies things. Since this is the key protection against
		 * a rogue server sending the client to an untrustworthy location,
		 * simpler is better.
		 */
		discovery_issuer = issuer_from_well_known_uri(conn, ctx.discovery_uri);
		if (!discovery_issuer)
			goto cleanup;		/* error message already set */

		if (strcmp(conn->oauth_issuer_id, discovery_issuer) != 0)
		{
			libpq_append_conn_error(conn,
									"server's discovery document at %s (issuer \"%s\") is incompatible with oauth_issuer (%s)",
									ctx.discovery_uri, discovery_issuer,
									conn->oauth_issuer_id);

			free(discovery_issuer);
			goto cleanup;
		}

		free(discovery_issuer);

		if (!conn->oauth_discovery_uri)
		{
			conn->oauth_discovery_uri = ctx.discovery_uri;
			ctx.discovery_uri = NULL;
		}
		else
		{
			/* This must match the URI we'd previously determined. */
			if (strcmp(conn->oauth_discovery_uri, ctx.discovery_uri) != 0)
			{
				libpq_append_conn_error(conn,
										"server's discovery document has moved to %s (previous location was %s)",
										ctx.discovery_uri,
										conn->oauth_discovery_uri);
				goto cleanup;
			}
		}
	}

	if (ctx.scope)
	{
		/* Servers may not override a previously set oauth_scope. */
		if (!conn->oauth_scope)
		{
			conn->oauth_scope = ctx.scope;
			ctx.scope = NULL;
		}
	}

	if (!ctx.status)
	{
		libpq_append_conn_error(conn,
								"server sent error response without a status");
		goto cleanup;
	}

	if (strcmp(ctx.status, "invalid_token") != 0)
	{
		/*
		 * invalid_token is the only error code we'll automatically retry for;
		 * otherwise, just bail out now.
		 */
		libpq_append_conn_error(conn,
								"server rejected OAuth bearer token: %s",
								ctx.status);
		goto cleanup;
	}

	success = true;

cleanup:
	free(ctx.status);
	free(ctx.scope);
	free(ctx.discovery_uri);

	return success;
}

/*
 * Callback implementation of conn->async_auth() for a user-defined OAuth flow.
 * Delegates the retrieval of the token to the application's async callback.
 *
 * This will be called multiple times as needed; the application is responsible
 * for setting an altsock to signal and returning the correct PGRES_POLLING_*
 * statuses for use by PQconnectPoll().
 */
static PostgresPollingStatusType
run_user_oauth_flow(PGconn *conn)
{
	fe_oauth_state *state = conn->sasl_state;
	PGoauthBearerRequest *request = state->async_ctx;
	PostgresPollingStatusType status;

	if (!request->async)
	{
		libpq_append_conn_error(conn,
								"user-defined OAuth flow provided neither a token nor an async callback");
		return PGRES_POLLING_FAILED;
	}

	status = request->async(conn, request, &conn->altsock);
	if (status == PGRES_POLLING_FAILED)
	{
		libpq_append_conn_error(conn, "user-defined OAuth flow failed");
		return status;
	}
	else if (status == PGRES_POLLING_OK)
	{
		/*
		 * We already have a token, so copy it into the conn. (We can't hold
		 * onto the original string, since it may not be safe for us to free()
		 * it.)
		 */
		if (!request->token)
		{
			libpq_append_conn_error(conn,
									"user-defined OAuth flow did not provide a token");
			return PGRES_POLLING_FAILED;
		}

		conn->oauth_token = strdup(request->token);
		if (!conn->oauth_token)
		{
			libpq_append_conn_error(conn, "out of memory");
			return PGRES_POLLING_FAILED;
		}

		return PGRES_POLLING_OK;
	}

	/* The hook wants the client to poll the altsock. Make sure it set one. */
	if (conn->altsock == PGINVALID_SOCKET)
	{
		libpq_append_conn_error(conn,
								"user-defined OAuth flow did not provide a socket for polling");
		return PGRES_POLLING_FAILED;
	}

	return status;
}

/*
 * Cleanup callback for the async user flow. Delegates most of its job to the
 * user-provided cleanup implementation, then disconnects the altsock.
 */
static void
cleanup_user_oauth_flow(PGconn *conn)
{
	fe_oauth_state *state = conn->sasl_state;
	PGoauthBearerRequest *request = state->async_ctx;

	Assert(request);

	if (request->cleanup)
		request->cleanup(conn, request);
	conn->altsock = PGINVALID_SOCKET;

	free(request);
	state->async_ctx = NULL;
}

/*
 * Chooses an OAuth client flow for the connection, which will retrieve a Bearer
 * token for presentation to the server.
 *
 * If the application has registered a custom flow handler using
 * PQAUTHDATA_OAUTH_BEARER_TOKEN, it may either return a token immediately (e.g.
 * if it has one cached for immediate use), or set up for a series of
 * asynchronous callbacks which will be managed by run_user_oauth_flow().
 *
 * If the default handler is used instead, a Device Authorization flow is used
 * for the connection if support has been compiled in. (See
 * fe-auth-oauth-curl.c for implementation details.)
 *
 * If neither a custom handler nor the builtin flow is available, the connection
 * fails here.
 */
static bool
setup_token_request(PGconn *conn, fe_oauth_state *state)
{
	int			res;
	PGoauthBearerRequest request = {
		.openid_configuration = conn->oauth_discovery_uri,
		.scope = conn->oauth_scope,
	};

	Assert(request.openid_configuration);

	/* The client may have overridden the OAuth flow. */
	res = PQauthDataHook(PQAUTHDATA_OAUTH_BEARER_TOKEN, conn, &request);
	if (res > 0)
	{
		PGoauthBearerRequest *request_copy;

		if (request.token)
		{
			/*
			 * We already have a token, so copy it into the conn. (We can't
			 * hold onto the original string, since it may not be safe for us
			 * to free() it.)
			 */
			conn->oauth_token = strdup(request.token);
			if (!conn->oauth_token)
			{
				libpq_append_conn_error(conn, "out of memory");
				goto fail;
			}

			/* short-circuit */
			if (request.cleanup)
				request.cleanup(conn, &request);
			return true;
		}

		request_copy = malloc(sizeof(*request_copy));
		if (!request_copy)
		{
			libpq_append_conn_error(conn, "out of memory");
			goto fail;
		}

		*request_copy = request;

		conn->async_auth = run_user_oauth_flow;
		conn->cleanup_async_auth = cleanup_user_oauth_flow;
		state->async_ctx = request_copy;
	}
	else if (res < 0)
	{
		libpq_append_conn_error(conn, "user-defined OAuth flow failed");
		goto fail;
	}
	else
	{
#if USE_LIBCURL
		/* Hand off to our built-in OAuth flow. */
		conn->async_auth = pg_fe_run_oauth_flow;
		conn->cleanup_async_auth = pg_fe_cleanup_oauth_flow;

#else
		libpq_append_conn_error(conn, "no custom OAuth flows are available, and libpq was not built with libcurl support");
		goto fail;

#endif
	}

	return true;

fail:
	if (request.cleanup)
		request.cleanup(conn, &request);
	return false;
}

/*
 * Fill in our issuer identifier (and discovery URI, if possible) using the
 * connection parameters. If conn->oauth_discovery_uri can't be populated in
 * this function, it will be requested from the server.
 */
static bool
setup_oauth_parameters(PGconn *conn)
{
	/*
	 * This is the only function that sets conn->oauth_issuer_id. If a
	 * previous connection attempt has already computed it, don't overwrite it
	 * or the discovery URI. (There's no reason for them to change once
	 * they're set, and handle_oauth_sasl_error() will fail the connection if
	 * the server attempts to switch them on us later.)
	 */
	if (conn->oauth_issuer_id)
		return true;

	/*---
	 * To talk to a server, we require the user to provide issuer and client
	 * identifiers.
	 *
	 * While it's possible for an OAuth client to support multiple issuers, it
	 * requires additional effort to make sure the flows in use are safe -- to
	 * quote RFC 9207,
	 *
	 *     OAuth clients that interact with only one authorization server are
	 *     not vulnerable to mix-up attacks. However, when such clients decide
	 *     to add support for a second authorization server in the future, they
	 *     become vulnerable and need to apply countermeasures to mix-up
	 *     attacks.
	 *
	 * For now, we allow only one.
	 */
	if (!conn->oauth_issuer || !conn->oauth_client_id)
	{
		libpq_append_conn_error(conn,
								"server requires OAuth authentication, but oauth_issuer and oauth_client_id are not both set");
		return false;
	}

	/*
	 * oauth_issuer is interpreted differently if it's a well-known discovery
	 * URI rather than just an issuer identifier.
	 */
	if (strstr(conn->oauth_issuer, WK_PREFIX) != NULL)
	{
		/*
		 * Convert the URI back to an issuer identifier. (This also performs
		 * validation of the URI format.)
		 */
		conn->oauth_issuer_id = issuer_from_well_known_uri(conn,
														   conn->oauth_issuer);
		if (!conn->oauth_issuer_id)
			return false;		/* error message already set */

		conn->oauth_discovery_uri = strdup(conn->oauth_issuer);
		if (!conn->oauth_discovery_uri)
		{
			libpq_append_conn_error(conn, "out of memory");
			return false;
		}
	}
	else
	{
		/*
		 * Treat oauth_issuer as an issuer identifier. We'll ask the server
		 * for the discovery URI.
		 */
		conn->oauth_issuer_id = strdup(conn->oauth_issuer);
		if (!conn->oauth_issuer_id)
		{
			libpq_append_conn_error(conn, "out of memory");
			return false;
		}
	}

	return true;
}

/*
 * Implements the OAUTHBEARER SASL exchange (RFC 7628, Sec. 3.2).
 *
 * If the necessary OAuth parameters are set up on the connection, this will run
 * the client flow asynchronously and present the resulting token to the server.
 * Otherwise, an empty discovery response will be sent and any parameters sent
 * back by the server will be stored for a second attempt.
 *
 * For a full description of the API, see libpq/sasl.h.
 */
static SASLStatus
oauth_exchange(void *opaq, bool final,
			   char *input, int inputlen,
			   char **output, int *outputlen)
{
	fe_oauth_state *state = opaq;
	PGconn	   *conn = state->conn;
	bool		discover = false;

	*output = NULL;
	*outputlen = 0;

	switch (state->step)
	{
		case FE_OAUTH_INIT:
			/* We begin in the initial response phase. */
			Assert(inputlen == -1);

			if (!setup_oauth_parameters(conn))
				return SASL_FAILED;

			if (conn->oauth_token)
			{
				/*
				 * A previous connection already fetched the token; we'll use
				 * it below.
				 */
			}
			else if (conn->oauth_discovery_uri)
			{
				/*
				 * We don't have a token, but we have a discovery URI already
				 * stored. Decide whether we're using a user-provided OAuth
				 * flow or the one we have built in.
				 */
				if (!setup_token_request(conn, state))
					return SASL_FAILED;

				if (conn->oauth_token)
				{
					/*
					 * A really smart user implementation may have already
					 * given us the token (e.g. if there was an unexpired copy
					 * already cached), and we can use it immediately.
					 */
				}
				else
				{
					/*
					 * Otherwise, we'll have to hand the connection over to
					 * our OAuth implementation.
					 *
					 * This could take a while, since it generally involves a
					 * user in the loop. To avoid consuming the server's
					 * authentication timeout, we'll continue this handshake
					 * to the end, so that the server can close its side of
					 * the connection. We'll open a second connection later
					 * once we've retrieved a token.
					 */
					discover = true;
				}
			}
			else
			{
				/*
				 * If we don't have a token, and we don't have a discovery URI
				 * to be able to request a token, we ask the server for one
				 * explicitly.
				 */
				discover = true;
			}

			/*
			 * Generate an initial response. This either contains a token, if
			 * we have one, or an empty discovery response which is doomed to
			 * fail.
			 */
			*output = client_initial_response(conn, discover);
			if (!*output)
				return SASL_FAILED;

			*outputlen = strlen(*output);
			state->step = FE_OAUTH_BEARER_SENT;

			if (conn->oauth_token)
			{
				/*
				 * For the purposes of require_auth, our side of
				 * authentication is done at this point; the server will
				 * either accept the connection or send an error. Unlike
				 * SCRAM, there is no additional server data to check upon
				 * success.
				 */
				conn->client_finished_auth = true;
			}

			return SASL_CONTINUE;

		case FE_OAUTH_BEARER_SENT:
			if (final)
			{
				/*
				 * OAUTHBEARER does not make use of additional data with a
				 * successful SASL exchange, so we shouldn't get an
				 * AuthenticationSASLFinal message.
				 */
				libpq_append_conn_error(conn,
										"server sent unexpected additional OAuth data");
				return SASL_FAILED;
			}

			/*
			 * An error message was sent by the server. Respond with the
			 * required dummy message (RFC 7628, sec. 3.2.3).
			 */
			*output = strdup(kvsep);
			if (unlikely(!*output))
			{
				libpq_append_conn_error(conn, "out of memory");
				return SASL_FAILED;
			}
			*outputlen = strlen(*output);	/* == 1 */

			/* Grab the settings from discovery. */
			if (!handle_oauth_sasl_error(conn, input, inputlen))
				return SASL_FAILED;

			if (conn->oauth_token)
			{
				/*
				 * The server rejected our token. Continue onwards towards the
				 * expected FATAL message, but mark our state to catch any
				 * unexpected "success" from the server.
				 */
				state->step = FE_OAUTH_SERVER_ERROR;
				return SASL_CONTINUE;
			}

			if (!conn->async_auth)
			{
				/*
				 * No OAuth flow is set up yet. Did we get enough information
				 * from the server to create one?
				 */
				if (!conn->oauth_discovery_uri)
				{
					libpq_append_conn_error(conn,
											"server requires OAuth authentication, but no discovery metadata was provided");
					return SASL_FAILED;
				}

				/* Yes. Set up the flow now. */
				if (!setup_token_request(conn, state))
					return SASL_FAILED;

				if (conn->oauth_token)
				{
					/*
					 * A token was available in a custom flow's cache. Skip
					 * the asynchronous processing.
					 */
					goto reconnect;
				}
			}

			/*
			 * Time to retrieve a token. This involves a number of HTTP
			 * connections and timed waits, so we escape the synchronous auth
			 * processing and tell PQconnectPoll to transfer control to our
			 * async implementation.
			 */
			Assert(conn->async_auth);	/* should have been set already */
			state->step = FE_OAUTH_REQUESTING_TOKEN;
			return SASL_ASYNC;

		case FE_OAUTH_REQUESTING_TOKEN:

			/*
			 * We've returned successfully from token retrieval. Double-check
			 * that we have what we need for the next connection.
			 */
			if (!conn->oauth_token)
			{
				Assert(false);	/* should have failed before this point! */
				libpq_append_conn_error(conn,
										"internal error: OAuth flow did not set a token");
				return SASL_FAILED;
			}

			goto reconnect;

		case FE_OAUTH_SERVER_ERROR:

			/*
			 * After an error, the server should send an error response to
			 * fail the SASL handshake, which is handled in higher layers.
			 *
			 * If we get here, the server either sent *another* challenge
			 * which isn't defined in the RFC, or completed the handshake
			 * successfully after telling us it was going to fail. Neither is
			 * acceptable.
			 */
			libpq_append_conn_error(conn,
									"server sent additional OAuth data after error");
			return SASL_FAILED;

		default:
			libpq_append_conn_error(conn, "invalid OAuth exchange state");
			break;
	}

	Assert(false);				/* should never get here */
	return SASL_FAILED;

reconnect:

	/*
	 * Despite being a failure from the point of view of SASL, we have enough
	 * information to restart with a new connection.
	 */
	libpq_append_conn_error(conn, "retrying connection with new bearer token");
	conn->oauth_want_retry = true;
	return SASL_FAILED;
}

static bool
oauth_channel_bound(void *opaq)
{
	/* This mechanism does not support channel binding. */
	return false;
}

/*
 * Fully clears out any stored OAuth token. This is done proactively upon
 * successful connection as well as during pqClosePGconn().
 */
void
pqClearOAuthToken(PGconn *conn)
{
	if (!conn->oauth_token)
		return;

	explicit_bzero(conn->oauth_token, strlen(conn->oauth_token));
	free(conn->oauth_token);
	conn->oauth_token = NULL;
}

/*
 * Returns true if the PGOAUTHDEBUG=UNSAFE flag is set in the environment.
 */
bool
oauth_unsafe_debugging_enabled(void)
{
	const char *env = getenv("PGOAUTHDEBUG");

	return (env && strcmp(env, "UNSAFE") == 0);
}
