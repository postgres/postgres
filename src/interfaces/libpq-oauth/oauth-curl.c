/*-------------------------------------------------------------------------
 *
 * oauth-curl.c
 *	   The libcurl implementation of OAuth/OIDC authentication, using the
 *	   OAuth Device Authorization Grant (RFC 8628).
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq-oauth/oauth-curl.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <curl/curl.h>
#include <math.h>
#include <unistd.h>

#if defined(HAVE_SYS_EPOLL_H)
#include <sys/epoll.h>
#include <sys/timerfd.h>
#elif defined(HAVE_SYS_EVENT_H)
#include <sys/event.h>
#else
#error libpq-oauth is not supported on this platform
#endif

#include "common/jsonapi.h"
#include "fe-auth-oauth.h"
#include "mb/pg_wchar.h"
#include "oauth-curl.h"

#ifdef USE_DYNAMIC_OAUTH

/*
 * The module build is decoupled from libpq-int.h, to try to avoid inadvertent
 * ABI breaks during minor version bumps. Replacements for the missing internals
 * are provided by oauth-utils.
 */
#include "oauth-utils.h"

#else							/* !USE_DYNAMIC_OAUTH */

/*
 * Static builds may rely on PGconn offsets directly. Keep these aligned with
 * the bank of callbacks in oauth-utils.h.
 */
#include "libpq-int.h"

#define conn_errorMessage(CONN) (&CONN->errorMessage)
#define conn_oauth_client_id(CONN) (CONN->oauth_client_id)
#define conn_oauth_client_secret(CONN) (CONN->oauth_client_secret)
#define conn_oauth_discovery_uri(CONN) (CONN->oauth_discovery_uri)
#define conn_oauth_issuer_id(CONN) (CONN->oauth_issuer_id)
#define conn_oauth_scope(CONN) (CONN->oauth_scope)
#define conn_sasl_state(CONN) (CONN->sasl_state)

#define set_conn_altsock(CONN, VAL) do { CONN->altsock = VAL; } while (0)
#define set_conn_oauth_token(CONN, VAL) do { CONN->oauth_token = VAL; } while (0)

#endif							/* USE_DYNAMIC_OAUTH */

/* One final guardrail against accidental inclusion... */
#if defined(USE_DYNAMIC_OAUTH) && defined(LIBPQ_INT_H)
#error do not rely on libpq-int.h in dynamic builds of libpq-oauth
#endif

/*
 * It's generally prudent to set a maximum response size to buffer in memory,
 * but it's less clear what size to choose. The biggest of our expected
 * responses is the server metadata JSON, which will only continue to grow in
 * size; the number of IANA-registered parameters in that document is up to 78
 * as of February 2025.
 *
 * Even if every single parameter were to take up 2k on average (a previously
 * common limit on the size of a URL), 256k gives us 128 parameter values before
 * we give up. (That's almost certainly complete overkill in practice; 2-4k
 * appears to be common among popular providers at the moment.)
 */
#define MAX_OAUTH_RESPONSE_SIZE (256 * 1024)

/*
 * Similarly, a limit on the maximum JSON nesting level keeps a server from
 * running us out of stack space. A common nesting level in practice is 2 (for a
 * top-level object containing arrays of strings). As of May 2025, the maximum
 * depth for standard server metadata appears to be 6, if the document contains
 * a full JSON Web Key Set in its "jwks" parameter.
 *
 * Since it's easy to nest JSON, and the number of parameters and key types
 * keeps growing, take a healthy buffer of 16. (If this ever proves to be a
 * problem in practice, we may want to switch over to the incremental JSON
 * parser instead of playing with this parameter.)
 */
#define MAX_OAUTH_NESTING_LEVEL 16

/*
 * Parsed JSON Representations
 *
 * As a general rule, we parse and cache only the fields we're currently using.
 * When adding new fields, ensure the corresponding free_*() function is updated
 * too.
 */

/*
 * The OpenID Provider configuration (alternatively named "authorization server
 * metadata") jointly described by OpenID Connect Discovery 1.0 and RFC 8414:
 *
 *     https://openid.net/specs/openid-connect-discovery-1_0.html
 *     https://www.rfc-editor.org/rfc/rfc8414#section-3.2
 */
struct provider
{
	char	   *issuer;
	char	   *token_endpoint;
	char	   *device_authorization_endpoint;
	struct curl_slist *grant_types_supported;
};

static void
free_provider(struct provider *provider)
{
	free(provider->issuer);
	free(provider->token_endpoint);
	free(provider->device_authorization_endpoint);
	curl_slist_free_all(provider->grant_types_supported);
}

/*
 * The Device Authorization response, described by RFC 8628:
 *
 *     https://www.rfc-editor.org/rfc/rfc8628#section-3.2
 */
struct device_authz
{
	char	   *device_code;
	char	   *user_code;
	char	   *verification_uri;
	char	   *verification_uri_complete;
	char	   *expires_in_str;
	char	   *interval_str;

	/* Fields below are parsed from the corresponding string above. */
	int			expires_in;
	int			interval;
};

static void
free_device_authz(struct device_authz *authz)
{
	free(authz->device_code);
	free(authz->user_code);
	free(authz->verification_uri);
	free(authz->verification_uri_complete);
	free(authz->expires_in_str);
	free(authz->interval_str);
}

/*
 * The Token Endpoint error response, as described by RFC 6749:
 *
 *     https://www.rfc-editor.org/rfc/rfc6749#section-5.2
 *
 * Note that this response type can also be returned from the Device
 * Authorization Endpoint.
 */
struct token_error
{
	char	   *error;
	char	   *error_description;
};

static void
free_token_error(struct token_error *err)
{
	free(err->error);
	free(err->error_description);
}

/*
 * The Access Token response, as described by RFC 6749:
 *
 *     https://www.rfc-editor.org/rfc/rfc6749#section-4.1.4
 *
 * During the Device Authorization flow, several temporary errors are expected
 * as part of normal operation. To make it easy to handle these in the happy
 * path, this contains an embedded token_error that is filled in if needed.
 */
struct token
{
	/* for successful responses */
	char	   *access_token;
	char	   *token_type;

	/* for error responses */
	struct token_error err;
};

static void
free_token(struct token *tok)
{
	free(tok->access_token);
	free(tok->token_type);
	free_token_error(&tok->err);
}

/*
 * Asynchronous State
 */

/* States for the overall async machine. */
enum OAuthStep
{
	OAUTH_STEP_INIT = 0,
	OAUTH_STEP_DISCOVERY,
	OAUTH_STEP_DEVICE_AUTHORIZATION,
	OAUTH_STEP_TOKEN_REQUEST,
	OAUTH_STEP_WAIT_INTERVAL,
};

/*
 * The async_ctx holds onto state that needs to persist across multiple calls
 * to pg_fe_run_oauth_flow(). Almost everything interacts with this in some
 * way.
 */
struct async_ctx
{
	enum OAuthStep step;		/* where are we in the flow? */

	int			timerfd;		/* descriptor for signaling async timeouts */
	pgsocket	mux;			/* the multiplexer socket containing all
								 * descriptors tracked by libcurl, plus the
								 * timerfd */
	CURLM	   *curlm;			/* top-level multi handle for libcurl
								 * operations */
	CURL	   *curl;			/* the (single) easy handle for serial
								 * requests */

	struct curl_slist *headers; /* common headers for all requests */
	PQExpBufferData work_data;	/* scratch buffer for general use (remember to
								 * clear out prior contents first!) */

	/*------
	 * Since a single logical operation may stretch across multiple calls to
	 * our entry point, errors have three parts:
	 *
	 * - errctx:	an optional static string, describing the global operation
	 *				currently in progress. It'll be translated for you.
	 *
	 * - errbuf:	contains the actual error message. Generally speaking, use
	 *				actx_error[_str] to manipulate this. This must be filled
	 *				with something useful on an error.
	 *
	 * - curl_err:	an optional static error buffer used by libcurl to put
	 *				detailed information about failures. Unfortunately
	 *				untranslatable.
	 *
	 * These pieces will be combined into a single error message looking
	 * something like the following, with errctx and/or curl_err omitted when
	 * absent:
	 *
	 *     connection to server ... failed: errctx: errbuf (libcurl: curl_err)
	 */
	const char *errctx;			/* not freed; must point to static allocation */
	PQExpBufferData errbuf;
	char		curl_err[CURL_ERROR_SIZE];

	/*
	 * These documents need to survive over multiple calls, and are therefore
	 * cached directly in the async_ctx.
	 */
	struct provider provider;
	struct device_authz authz;

	int			running;		/* is asynchronous work in progress? */
	bool		user_prompted;	/* have we already sent the authz prompt? */
	bool		used_basic_auth;	/* did we send a client secret? */
	bool		debugging;		/* can we give unsafe developer assistance? */
};

/*
 * Tears down the Curl handles and frees the async_ctx.
 */
static void
free_async_ctx(PGconn *conn, struct async_ctx *actx)
{
	/*
	 * In general, none of the error cases below should ever happen if we have
	 * no bugs above. But if we do hit them, surfacing those errors somehow
	 * might be the only way to have a chance to debug them.
	 *
	 * TODO: At some point it'd be nice to have a standard way to warn about
	 * teardown failures. Appending to the connection's error message only
	 * helps if the bug caused a connection failure; otherwise it'll be
	 * buried...
	 */

	if (actx->curlm && actx->curl)
	{
		CURLMcode	err = curl_multi_remove_handle(actx->curlm, actx->curl);

		if (err)
			libpq_append_conn_error(conn,
									"libcurl easy handle removal failed: %s",
									curl_multi_strerror(err));
	}

	if (actx->curl)
	{
		/*
		 * curl_multi_cleanup() doesn't free any associated easy handles; we
		 * need to do that separately. We only ever have one easy handle per
		 * multi handle.
		 */
		curl_easy_cleanup(actx->curl);
	}

	if (actx->curlm)
	{
		CURLMcode	err = curl_multi_cleanup(actx->curlm);

		if (err)
			libpq_append_conn_error(conn,
									"libcurl multi handle cleanup failed: %s",
									curl_multi_strerror(err));
	}

	free_provider(&actx->provider);
	free_device_authz(&actx->authz);

	curl_slist_free_all(actx->headers);
	termPQExpBuffer(&actx->work_data);
	termPQExpBuffer(&actx->errbuf);

	if (actx->mux != PGINVALID_SOCKET)
		close(actx->mux);
	if (actx->timerfd >= 0)
		close(actx->timerfd);

	free(actx);
}

/*
 * Release resources used for the asynchronous exchange and disconnect the
 * altsock.
 *
 * This is called either at the end of a successful authentication, or during
 * pqDropConnection(), so we won't leak resources even if PQconnectPoll() never
 * calls us back.
 */
void
pg_fe_cleanup_oauth_flow(PGconn *conn)
{
	fe_oauth_state *state = conn_sasl_state(conn);

	if (state->async_ctx)
	{
		free_async_ctx(conn, state->async_ctx);
		state->async_ctx = NULL;
	}

	set_conn_altsock(conn, PGINVALID_SOCKET);
}

/*
 * Macros for manipulating actx->errbuf. actx_error() translates and formats a
 * string for you; actx_error_str() appends a string directly without
 * translation.
 */

#define actx_error(ACTX, FMT, ...) \
	appendPQExpBuffer(&(ACTX)->errbuf, libpq_gettext(FMT), ##__VA_ARGS__)

#define actx_error_str(ACTX, S) \
	appendPQExpBufferStr(&(ACTX)->errbuf, S)

/*
 * Macros for getting and setting state for the connection's two libcurl
 * handles, so you don't have to write out the error handling every time.
 */

#define CHECK_MSETOPT(ACTX, OPT, VAL, FAILACTION) \
	do { \
		struct async_ctx *_actx = (ACTX); \
		CURLMcode	_setopterr = curl_multi_setopt(_actx->curlm, OPT, VAL); \
		if (_setopterr) { \
			actx_error(_actx, "failed to set %s on OAuth connection: %s",\
					   #OPT, curl_multi_strerror(_setopterr)); \
			FAILACTION; \
		} \
	} while (0)

#define CHECK_SETOPT(ACTX, OPT, VAL, FAILACTION) \
	do { \
		struct async_ctx *_actx = (ACTX); \
		CURLcode	_setopterr = curl_easy_setopt(_actx->curl, OPT, VAL); \
		if (_setopterr) { \
			actx_error(_actx, "failed to set %s on OAuth connection: %s",\
					   #OPT, curl_easy_strerror(_setopterr)); \
			FAILACTION; \
		} \
	} while (0)

#define CHECK_GETINFO(ACTX, INFO, OUT, FAILACTION) \
	do { \
		struct async_ctx *_actx = (ACTX); \
		CURLcode	_getinfoerr = curl_easy_getinfo(_actx->curl, INFO, OUT); \
		if (_getinfoerr) { \
			actx_error(_actx, "failed to get %s from OAuth response: %s",\
					   #INFO, curl_easy_strerror(_getinfoerr)); \
			FAILACTION; \
		} \
	} while (0)

/*
 * General JSON Parsing for OAuth Responses
 */

/*
 * Represents a single name/value pair in a JSON object. This is the primary
 * interface to parse_oauth_json().
 *
 * All fields are stored internally as strings or lists of strings, so clients
 * have to explicitly parse other scalar types (though they will have gone
 * through basic lexical validation). Storing nested objects is not currently
 * supported, nor is parsing arrays of anything other than strings.
 */
struct json_field
{
	const char *name;			/* name (key) of the member */

	JsonTokenType type;			/* currently supports JSON_TOKEN_STRING,
								 * JSON_TOKEN_NUMBER, and
								 * JSON_TOKEN_ARRAY_START */
	union
	{
		char	  **scalar;		/* for all scalar types */
		struct curl_slist **array;	/* for type == JSON_TOKEN_ARRAY_START */
	}			target;

	bool		required;		/* REQUIRED field, or just OPTIONAL? */
};

/* Documentation macros for json_field.required. */
#define PG_OAUTH_REQUIRED true
#define PG_OAUTH_OPTIONAL false

/* Parse state for parse_oauth_json(). */
struct oauth_parse
{
	PQExpBuffer errbuf;			/* detail message for JSON_SEM_ACTION_FAILED */
	int			nested;			/* nesting level (zero is the top) */

	const struct json_field *fields;	/* field definition array */
	const struct json_field *active;	/* points inside the fields array */
};

#define oauth_parse_set_error(ctx, fmt, ...) \
	appendPQExpBuffer((ctx)->errbuf, libpq_gettext(fmt), ##__VA_ARGS__)

static void
report_type_mismatch(struct oauth_parse *ctx)
{
	char	   *msgfmt;

	Assert(ctx->active);

	/*
	 * At the moment, the only fields we're interested in are strings,
	 * numbers, and arrays of strings.
	 */
	switch (ctx->active->type)
	{
		case JSON_TOKEN_STRING:
			msgfmt = "field \"%s\" must be a string";
			break;

		case JSON_TOKEN_NUMBER:
			msgfmt = "field \"%s\" must be a number";
			break;

		case JSON_TOKEN_ARRAY_START:
			msgfmt = "field \"%s\" must be an array of strings";
			break;

		default:
			Assert(false);
			msgfmt = "field \"%s\" has unexpected type";
	}

	oauth_parse_set_error(ctx, msgfmt, ctx->active->name);
}

static JsonParseErrorType
oauth_json_object_start(void *state)
{
	struct oauth_parse *ctx = state;

	if (ctx->active)
	{
		/*
		 * Currently, none of the fields we're interested in can be or contain
		 * objects, so we can reject this case outright.
		 */
		report_type_mismatch(ctx);
		return JSON_SEM_ACTION_FAILED;
	}

	++ctx->nested;
	if (ctx->nested > MAX_OAUTH_NESTING_LEVEL)
	{
		oauth_parse_set_error(ctx, "JSON is too deeply nested");
		return JSON_SEM_ACTION_FAILED;
	}

	return JSON_SUCCESS;
}

static JsonParseErrorType
oauth_json_object_field_start(void *state, char *name, bool isnull)
{
	struct oauth_parse *ctx = state;

	/* We care only about the top-level fields. */
	if (ctx->nested == 1)
	{
		const struct json_field *field = ctx->fields;

		/*
		 * We should never start parsing a new field while a previous one is
		 * still active.
		 */
		if (ctx->active)
		{
			Assert(false);
			oauth_parse_set_error(ctx,
								  "internal error: started field '%s' before field '%s' was finished",
								  name, ctx->active->name);
			return JSON_SEM_ACTION_FAILED;
		}

		while (field->name)
		{
			if (strcmp(name, field->name) == 0)
			{
				ctx->active = field;
				break;
			}

			++field;
		}

		/*
		 * We don't allow duplicate field names; error out if the target has
		 * already been set.
		 */
		if (ctx->active)
		{
			field = ctx->active;

			if ((field->type == JSON_TOKEN_ARRAY_START && *field->target.array)
				|| (field->type != JSON_TOKEN_ARRAY_START && *field->target.scalar))
			{
				oauth_parse_set_error(ctx, "field \"%s\" is duplicated",
									  field->name);
				return JSON_SEM_ACTION_FAILED;
			}
		}
	}

	return JSON_SUCCESS;
}

static JsonParseErrorType
oauth_json_object_end(void *state)
{
	struct oauth_parse *ctx = state;

	--ctx->nested;

	/*
	 * All fields should be fully processed by the end of the top-level
	 * object.
	 */
	if (!ctx->nested && ctx->active)
	{
		Assert(false);
		oauth_parse_set_error(ctx,
							  "internal error: field '%s' still active at end of object",
							  ctx->active->name);
		return JSON_SEM_ACTION_FAILED;
	}

	return JSON_SUCCESS;
}

static JsonParseErrorType
oauth_json_array_start(void *state)
{
	struct oauth_parse *ctx = state;

	if (!ctx->nested)
	{
		oauth_parse_set_error(ctx, "top-level element must be an object");
		return JSON_SEM_ACTION_FAILED;
	}

	if (ctx->active)
	{
		if (ctx->active->type != JSON_TOKEN_ARRAY_START
		/* The arrays we care about must not have arrays as values. */
			|| ctx->nested > 1)
		{
			report_type_mismatch(ctx);
			return JSON_SEM_ACTION_FAILED;
		}
	}

	++ctx->nested;
	if (ctx->nested > MAX_OAUTH_NESTING_LEVEL)
	{
		oauth_parse_set_error(ctx, "JSON is too deeply nested");
		return JSON_SEM_ACTION_FAILED;
	}

	return JSON_SUCCESS;
}

static JsonParseErrorType
oauth_json_array_end(void *state)
{
	struct oauth_parse *ctx = state;

	if (ctx->active)
	{
		/*
		 * Clear the target (which should be an array inside the top-level
		 * object). For this to be safe, no target arrays can contain other
		 * arrays; we check for that in the array_start callback.
		 */
		if (ctx->nested != 2 || ctx->active->type != JSON_TOKEN_ARRAY_START)
		{
			Assert(false);
			oauth_parse_set_error(ctx,
								  "internal error: found unexpected array end while parsing field '%s'",
								  ctx->active->name);
			return JSON_SEM_ACTION_FAILED;
		}

		ctx->active = NULL;
	}

	--ctx->nested;
	return JSON_SUCCESS;
}

static JsonParseErrorType
oauth_json_scalar(void *state, char *token, JsonTokenType type)
{
	struct oauth_parse *ctx = state;

	if (!ctx->nested)
	{
		oauth_parse_set_error(ctx, "top-level element must be an object");
		return JSON_SEM_ACTION_FAILED;
	}

	if (ctx->active)
	{
		const struct json_field *field = ctx->active;
		JsonTokenType expected = field->type;

		/* Make sure this matches what the active field expects. */
		if (expected == JSON_TOKEN_ARRAY_START)
		{
			/* Are we actually inside an array? */
			if (ctx->nested < 2)
			{
				report_type_mismatch(ctx);
				return JSON_SEM_ACTION_FAILED;
			}

			/* Currently, arrays can only contain strings. */
			expected = JSON_TOKEN_STRING;
		}

		if (type != expected)
		{
			report_type_mismatch(ctx);
			return JSON_SEM_ACTION_FAILED;
		}

		if (field->type != JSON_TOKEN_ARRAY_START)
		{
			/* Ensure that we're parsing the top-level keys... */
			if (ctx->nested != 1)
			{
				Assert(false);
				oauth_parse_set_error(ctx,
									  "internal error: scalar target found at nesting level %d",
									  ctx->nested);
				return JSON_SEM_ACTION_FAILED;
			}

			/* ...and that a result has not already been set. */
			if (*field->target.scalar)
			{
				Assert(false);
				oauth_parse_set_error(ctx,
									  "internal error: scalar field '%s' would be assigned twice",
									  ctx->active->name);
				return JSON_SEM_ACTION_FAILED;
			}

			*field->target.scalar = strdup(token);
			if (!*field->target.scalar)
				return JSON_OUT_OF_MEMORY;

			ctx->active = NULL;

			return JSON_SUCCESS;
		}
		else
		{
			struct curl_slist *temp;

			/* The target array should be inside the top-level object. */
			if (ctx->nested != 2)
			{
				Assert(false);
				oauth_parse_set_error(ctx,
									  "internal error: array member found at nesting level %d",
									  ctx->nested);
				return JSON_SEM_ACTION_FAILED;
			}

			/* Note that curl_slist_append() makes a copy of the token. */
			temp = curl_slist_append(*field->target.array, token);
			if (!temp)
				return JSON_OUT_OF_MEMORY;

			*field->target.array = temp;
		}
	}
	else
	{
		/* otherwise we just ignore it */
	}

	return JSON_SUCCESS;
}

/*
 * Checks the Content-Type header against the expected type. Parameters are
 * allowed but ignored.
 */
static bool
check_content_type(struct async_ctx *actx, const char *type)
{
	const size_t type_len = strlen(type);
	char	   *content_type;

	CHECK_GETINFO(actx, CURLINFO_CONTENT_TYPE, &content_type, return false);

	if (!content_type)
	{
		actx_error(actx, "no content type was provided");
		return false;
	}

	/*
	 * We need to perform a length limited comparison and not compare the
	 * whole string.
	 */
	if (pg_strncasecmp(content_type, type, type_len) != 0)
		goto fail;

	/* On an exact match, we're done. */
	Assert(strlen(content_type) >= type_len);
	if (content_type[type_len] == '\0')
		return true;

	/*
	 * Only a semicolon (optionally preceded by HTTP optional whitespace) is
	 * acceptable after the prefix we checked. This marks the start of media
	 * type parameters, which we currently have no use for.
	 */
	for (size_t i = type_len; content_type[i]; ++i)
	{
		switch (content_type[i])
		{
			case ';':
				return true;	/* success! */

			case ' ':
			case '\t':
				/* HTTP optional whitespace allows only spaces and htabs. */
				break;

			default:
				goto fail;
		}
	}

fail:
	actx_error(actx, "unexpected content type: \"%s\"", content_type);
	return false;
}

/*
 * A helper function for general JSON parsing. fields is the array of field
 * definitions with their backing pointers. The response will be parsed from
 * actx->curl and actx->work_data (as set up by start_request()), and any
 * parsing errors will be placed into actx->errbuf.
 */
static bool
parse_oauth_json(struct async_ctx *actx, const struct json_field *fields)
{
	PQExpBuffer resp = &actx->work_data;
	JsonLexContext lex = {0};
	JsonSemAction sem = {0};
	JsonParseErrorType err;
	struct oauth_parse ctx = {0};
	bool		success = false;

	if (!check_content_type(actx, "application/json"))
		return false;

	if (strlen(resp->data) != resp->len)
	{
		actx_error(actx, "response contains embedded NULLs");
		return false;
	}

	/*
	 * pg_parse_json doesn't validate the incoming UTF-8, so we have to check
	 * that up front.
	 */
	if (pg_encoding_verifymbstr(PG_UTF8, resp->data, resp->len) != resp->len)
	{
		actx_error(actx, "response is not valid UTF-8");
		return false;
	}

	makeJsonLexContextCstringLen(&lex, resp->data, resp->len, PG_UTF8, true);
	setJsonLexContextOwnsTokens(&lex, true);	/* must not leak on error */

	ctx.errbuf = &actx->errbuf;
	ctx.fields = fields;
	sem.semstate = &ctx;

	sem.object_start = oauth_json_object_start;
	sem.object_field_start = oauth_json_object_field_start;
	sem.object_end = oauth_json_object_end;
	sem.array_start = oauth_json_array_start;
	sem.array_end = oauth_json_array_end;
	sem.scalar = oauth_json_scalar;

	err = pg_parse_json(&lex, &sem);

	if (err != JSON_SUCCESS)
	{
		/*
		 * For JSON_SEM_ACTION_FAILED, we've already written the error
		 * message. Other errors come directly from pg_parse_json(), already
		 * translated.
		 */
		if (err != JSON_SEM_ACTION_FAILED)
			actx_error_str(actx, json_errdetail(err, &lex));

		goto cleanup;
	}

	/* Check all required fields. */
	while (fields->name)
	{
		if (fields->required
			&& !*fields->target.scalar
			&& !*fields->target.array)
		{
			actx_error(actx, "field \"%s\" is missing", fields->name);
			goto cleanup;
		}

		fields++;
	}

	success = true;

cleanup:
	freeJsonLexContext(&lex);
	return success;
}

/*
 * JSON Parser Definitions
 */

/*
 * Parses authorization server metadata. Fields are defined by OIDC Discovery
 * 1.0 and RFC 8414.
 */
static bool
parse_provider(struct async_ctx *actx, struct provider *provider)
{
	struct json_field fields[] = {
		{"issuer", JSON_TOKEN_STRING, {&provider->issuer}, PG_OAUTH_REQUIRED},
		{"token_endpoint", JSON_TOKEN_STRING, {&provider->token_endpoint}, PG_OAUTH_REQUIRED},

		/*----
		 * The following fields are technically REQUIRED, but we don't use
		 * them anywhere yet:
		 *
		 * - jwks_uri
		 * - response_types_supported
		 * - subject_types_supported
		 * - id_token_signing_alg_values_supported
		 */

		{"device_authorization_endpoint", JSON_TOKEN_STRING, {&provider->device_authorization_endpoint}, PG_OAUTH_OPTIONAL},
		{"grant_types_supported", JSON_TOKEN_ARRAY_START, {.array = &provider->grant_types_supported}, PG_OAUTH_OPTIONAL},

		{0},
	};

	return parse_oauth_json(actx, fields);
}

/*
 * Parses a valid JSON number into a double. The input must have come from
 * pg_parse_json(), so that we know the lexer has validated it; there's no
 * in-band signal for invalid formats.
 */
static double
parse_json_number(const char *s)
{
	double		parsed;
	int			cnt;

	/*
	 * The JSON lexer has already validated the number, which is stricter than
	 * the %f format, so we should be good to use sscanf().
	 */
	cnt = sscanf(s, "%lf", &parsed);

	if (cnt != 1)
	{
		/*
		 * Either the lexer screwed up or our assumption above isn't true, and
		 * either way a developer needs to take a look.
		 */
		Assert(false);
		return 0;
	}

	return parsed;
}

/*
 * Parses the "interval" JSON number, corresponding to the number of seconds to
 * wait between token endpoint requests.
 *
 * RFC 8628 is pretty silent on sanity checks for the interval. As a matter of
 * practicality, round any fractional intervals up to the next second, and clamp
 * the result at a minimum of one. (Zero-second intervals would result in an
 * expensive network polling loop.) Tests may remove the lower bound with
 * PGOAUTHDEBUG, for improved performance.
 */
static int
parse_interval(struct async_ctx *actx, const char *interval_str)
{
	double		parsed;

	parsed = parse_json_number(interval_str);
	parsed = ceil(parsed);

	if (parsed < 1)
		return actx->debugging ? 0 : 1;

	else if (parsed >= INT_MAX)
		return INT_MAX;

	return parsed;
}

/*
 * Parses the "expires_in" JSON number, corresponding to the number of seconds
 * remaining in the lifetime of the device code request.
 *
 * Similar to parse_interval, but we have even fewer requirements for reasonable
 * values since we don't use the expiration time directly (it's passed to the
 * PQAUTHDATA_PROMPT_OAUTH_DEVICE hook, in case the application wants to do
 * something with it). We simply round down and clamp to int range.
 */
static int
parse_expires_in(struct async_ctx *actx, const char *expires_in_str)
{
	double		parsed;

	parsed = parse_json_number(expires_in_str);
	parsed = floor(parsed);

	if (parsed >= INT_MAX)
		return INT_MAX;
	else if (parsed <= INT_MIN)
		return INT_MIN;

	return parsed;
}

/*
 * Parses the Device Authorization Response (RFC 8628, Sec. 3.2).
 */
static bool
parse_device_authz(struct async_ctx *actx, struct device_authz *authz)
{
	struct json_field fields[] = {
		{"device_code", JSON_TOKEN_STRING, {&authz->device_code}, PG_OAUTH_REQUIRED},
		{"user_code", JSON_TOKEN_STRING, {&authz->user_code}, PG_OAUTH_REQUIRED},
		{"verification_uri", JSON_TOKEN_STRING, {&authz->verification_uri}, PG_OAUTH_REQUIRED},
		{"expires_in", JSON_TOKEN_NUMBER, {&authz->expires_in_str}, PG_OAUTH_REQUIRED},

		/*
		 * Some services (Google, Azure) spell verification_uri differently.
		 * We accept either.
		 */
		{"verification_url", JSON_TOKEN_STRING, {&authz->verification_uri}, PG_OAUTH_REQUIRED},

		/*
		 * There is no evidence of verification_uri_complete being spelled
		 * with "url" instead with any service provider, so only support
		 * "uri".
		 */
		{"verification_uri_complete", JSON_TOKEN_STRING, {&authz->verification_uri_complete}, PG_OAUTH_OPTIONAL},
		{"interval", JSON_TOKEN_NUMBER, {&authz->interval_str}, PG_OAUTH_OPTIONAL},

		{0},
	};

	if (!parse_oauth_json(actx, fields))
		return false;

	/*
	 * Parse our numeric fields. Lexing has already completed by this time, so
	 * we at least know they're valid JSON numbers.
	 */
	if (authz->interval_str)
		authz->interval = parse_interval(actx, authz->interval_str);
	else
	{
		/*
		 * RFC 8628 specifies 5 seconds as the default value if the server
		 * doesn't provide an interval.
		 */
		authz->interval = 5;
	}

	Assert(authz->expires_in_str);	/* ensured by parse_oauth_json() */
	authz->expires_in = parse_expires_in(actx, authz->expires_in_str);

	return true;
}

/*
 * Parses the device access token error response (RFC 8628, Sec. 3.5, which
 * uses the error response defined in RFC 6749, Sec. 5.2).
 */
static bool
parse_token_error(struct async_ctx *actx, struct token_error *err)
{
	bool		result;
	struct json_field fields[] = {
		{"error", JSON_TOKEN_STRING, {&err->error}, PG_OAUTH_REQUIRED},

		{"error_description", JSON_TOKEN_STRING, {&err->error_description}, PG_OAUTH_OPTIONAL},

		{0},
	};

	result = parse_oauth_json(actx, fields);

	/*
	 * Since token errors are parsed during other active error paths, only
	 * override the errctx if parsing explicitly fails.
	 */
	if (!result)
		actx->errctx = "failed to parse token error response";

	return result;
}

/*
 * Constructs a message from the token error response and puts it into
 * actx->errbuf.
 */
static void
record_token_error(struct async_ctx *actx, const struct token_error *err)
{
	if (err->error_description)
		appendPQExpBuffer(&actx->errbuf, "%s ", err->error_description);
	else
	{
		/*
		 * Try to get some more helpful detail into the error string. A 401
		 * status in particular implies that the oauth_client_secret is
		 * missing or wrong.
		 */
		long		response_code;

		CHECK_GETINFO(actx, CURLINFO_RESPONSE_CODE, &response_code, response_code = 0);

		if (response_code == 401)
		{
			actx_error(actx, actx->used_basic_auth
					   ? "provider rejected the oauth_client_secret"
					   : "provider requires client authentication, and no oauth_client_secret is set");
			actx_error_str(actx, " ");
		}
	}

	appendPQExpBuffer(&actx->errbuf, "(%s)", err->error);
}

/*
 * Parses the device access token response (RFC 8628, Sec. 3.5, which uses the
 * success response defined in RFC 6749, Sec. 5.1).
 */
static bool
parse_access_token(struct async_ctx *actx, struct token *tok)
{
	struct json_field fields[] = {
		{"access_token", JSON_TOKEN_STRING, {&tok->access_token}, PG_OAUTH_REQUIRED},
		{"token_type", JSON_TOKEN_STRING, {&tok->token_type}, PG_OAUTH_REQUIRED},

		/*---
		 * We currently have no use for the following OPTIONAL fields:
		 *
		 * - expires_in: This will be important for maintaining a token cache,
		 *               but we do not yet implement one.
		 *
		 * - refresh_token: Ditto.
		 *
		 * - scope: This is only sent when the authorization server sees fit to
		 *          change our scope request. It's not clear what we should do
		 *          about this; either it's been done as a matter of policy, or
		 *          the user has explicitly denied part of the authorization,
		 *          and either way the server-side validator is in a better
		 *          place to complain if the change isn't acceptable.
		 */

		{0},
	};

	return parse_oauth_json(actx, fields);
}

/*
 * libcurl Multi Setup/Callbacks
 */

/*
 * Sets up the actx->mux, which is the altsock that PQconnectPoll clients will
 * select() on instead of the Postgres socket during OAuth negotiation.
 *
 * This is just an epoll set or kqueue abstracting multiple other descriptors.
 * For epoll, the timerfd is always part of the set; it's just disabled when
 * we're not using it. For kqueue, the "timerfd" is actually a second kqueue
 * instance which is only added to the set when needed.
 */
static bool
setup_multiplexer(struct async_ctx *actx)
{
#if defined(HAVE_SYS_EPOLL_H)
	struct epoll_event ev = {.events = EPOLLIN};

	actx->mux = epoll_create1(EPOLL_CLOEXEC);
	if (actx->mux < 0)
	{
		actx_error(actx, "failed to create epoll set: %m");
		return false;
	}

	actx->timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
	if (actx->timerfd < 0)
	{
		actx_error(actx, "failed to create timerfd: %m");
		return false;
	}

	if (epoll_ctl(actx->mux, EPOLL_CTL_ADD, actx->timerfd, &ev) < 0)
	{
		actx_error(actx, "failed to add timerfd to epoll set: %m");
		return false;
	}

	return true;
#elif defined(HAVE_SYS_EVENT_H)
	actx->mux = kqueue();
	if (actx->mux < 0)
	{
		/*- translator: the term "kqueue" (kernel queue) should not be translated */
		actx_error(actx, "failed to create kqueue: %m");
		return false;
	}

	/*
	 * Originally, we set EVFILT_TIMER directly on the top-level multiplexer.
	 * This makes it difficult to implement timer_expired(), though, so now we
	 * set EVFILT_TIMER on a separate actx->timerfd, which is chained to
	 * actx->mux while the timer is active.
	 */
	actx->timerfd = kqueue();
	if (actx->timerfd < 0)
	{
		actx_error(actx, "failed to create timer kqueue: %m");
		return false;
	}

	return true;
#else
#error setup_multiplexer is not implemented on this platform
#endif
}

/*
 * Adds and removes sockets from the multiplexer set, as directed by the
 * libcurl multi handle.
 */
static int
register_socket(CURL *curl, curl_socket_t socket, int what, void *ctx,
				void *socketp)
{
	struct async_ctx *actx = ctx;

#if defined(HAVE_SYS_EPOLL_H)
	struct epoll_event ev = {0};
	int			res;
	int			op = EPOLL_CTL_ADD;

	switch (what)
	{
		case CURL_POLL_IN:
			ev.events = EPOLLIN;
			break;

		case CURL_POLL_OUT:
			ev.events = EPOLLOUT;
			break;

		case CURL_POLL_INOUT:
			ev.events = EPOLLIN | EPOLLOUT;
			break;

		case CURL_POLL_REMOVE:
			op = EPOLL_CTL_DEL;
			break;

		default:
			actx_error(actx, "unknown libcurl socket operation: %d", what);
			return -1;
	}

	res = epoll_ctl(actx->mux, op, socket, &ev);
	if (res < 0 && errno == EEXIST)
	{
		/* We already had this socket in the poll set. */
		op = EPOLL_CTL_MOD;
		res = epoll_ctl(actx->mux, op, socket, &ev);
	}

	if (res < 0)
	{
		switch (op)
		{
			case EPOLL_CTL_ADD:
				actx_error(actx, "could not add to epoll set: %m");
				break;

			case EPOLL_CTL_DEL:
				actx_error(actx, "could not delete from epoll set: %m");
				break;

			default:
				actx_error(actx, "could not update epoll set: %m");
		}

		return -1;
	}

	return 0;
#elif defined(HAVE_SYS_EVENT_H)
	struct kevent ev[2] = {0};
	struct kevent ev_out[2];
	struct timespec timeout = {0};
	int			nev = 0;
	int			res;

	switch (what)
	{
		case CURL_POLL_IN:
			EV_SET(&ev[nev], socket, EVFILT_READ, EV_ADD | EV_RECEIPT, 0, 0, 0);
			nev++;
			break;

		case CURL_POLL_OUT:
			EV_SET(&ev[nev], socket, EVFILT_WRITE, EV_ADD | EV_RECEIPT, 0, 0, 0);
			nev++;
			break;

		case CURL_POLL_INOUT:
			EV_SET(&ev[nev], socket, EVFILT_READ, EV_ADD | EV_RECEIPT, 0, 0, 0);
			nev++;
			EV_SET(&ev[nev], socket, EVFILT_WRITE, EV_ADD | EV_RECEIPT, 0, 0, 0);
			nev++;
			break;

		case CURL_POLL_REMOVE:

			/*
			 * We don't know which of these is currently registered, perhaps
			 * both, so we try to remove both.  This means we need to tolerate
			 * ENOENT below.
			 */
			EV_SET(&ev[nev], socket, EVFILT_READ, EV_DELETE | EV_RECEIPT, 0, 0, 0);
			nev++;
			EV_SET(&ev[nev], socket, EVFILT_WRITE, EV_DELETE | EV_RECEIPT, 0, 0, 0);
			nev++;
			break;

		default:
			actx_error(actx, "unknown libcurl socket operation: %d", what);
			return -1;
	}

	res = kevent(actx->mux, ev, nev, ev_out, lengthof(ev_out), &timeout);
	if (res < 0)
	{
		actx_error(actx, "could not modify kqueue: %m");
		return -1;
	}

	/*
	 * We can't use the simple errno version of kevent, because we need to
	 * skip over ENOENT while still allowing a second change to be processed.
	 * So we need a longer-form error checking loop.
	 */
	for (int i = 0; i < res; ++i)
	{
		/*
		 * EV_RECEIPT should guarantee one EV_ERROR result for every change,
		 * whether successful or not. Failed entries contain a non-zero errno
		 * in the data field.
		 */
		Assert(ev_out[i].flags & EV_ERROR);

		errno = ev_out[i].data;
		if (errno && errno != ENOENT)
		{
			switch (what)
			{
				case CURL_POLL_REMOVE:
					actx_error(actx, "could not delete from kqueue: %m");
					break;
				default:
					actx_error(actx, "could not add to kqueue: %m");
			}
			return -1;
		}
	}

	return 0;
#else
#error register_socket is not implemented on this platform
#endif
}

/*
 * Enables or disables the timer in the multiplexer set. The timeout value is
 * in milliseconds (negative values disable the timer).
 *
 * For epoll, rather than continually adding and removing the timer, we keep it
 * in the set at all times and just disarm it when it's not needed. For kqueue,
 * the timer is removed completely when disabled to prevent stale timeouts from
 * remaining in the queue.
 *
 * To meet Curl requirements for the CURLMOPT_TIMERFUNCTION, implementations of
 * set_timer must handle repeated calls by fully discarding any previous running
 * or expired timer.
 */
static bool
set_timer(struct async_ctx *actx, long timeout)
{
#if defined(HAVE_SYS_EPOLL_H)
	struct itimerspec spec = {0};

	if (timeout < 0)
	{
		/* the zero itimerspec will disarm the timer below */
	}
	else if (timeout == 0)
	{
		/*
		 * A zero timeout means libcurl wants us to call back immediately.
		 * That's not technically an option for timerfd, but we can make the
		 * timeout ridiculously short.
		 */
		spec.it_value.tv_nsec = 1;
	}
	else
	{
		spec.it_value.tv_sec = timeout / 1000;
		spec.it_value.tv_nsec = (timeout % 1000) * 1000000;
	}

	if (timerfd_settime(actx->timerfd, 0 /* no flags */ , &spec, NULL) < 0)
	{
		actx_error(actx, "setting timerfd to %ld: %m", timeout);
		return false;
	}

	return true;
#elif defined(HAVE_SYS_EVENT_H)
	struct kevent ev;

#ifdef __NetBSD__

	/*
	 * Work around NetBSD's rejection of zero timeouts (EINVAL), a bit like
	 * timerfd above.
	 */
	if (timeout == 0)
		timeout = 1;
#endif

	/*
	 * Always disable the timer, and remove it from the multiplexer, to clear
	 * out any already-queued events. (On some BSDs, adding an EVFILT_TIMER to
	 * a kqueue that already has one will clear stale events, but not on
	 * macOS.)
	 *
	 * If there was no previous timer set, the kevent calls will result in
	 * ENOENT, which is fine.
	 */
	EV_SET(&ev, 1, EVFILT_TIMER, EV_DELETE, 0, 0, 0);
	if (kevent(actx->timerfd, &ev, 1, NULL, 0, NULL) < 0 && errno != ENOENT)
	{
		actx_error(actx, "deleting kqueue timer: %m");
		return false;
	}

	EV_SET(&ev, actx->timerfd, EVFILT_READ, EV_DELETE, 0, 0, 0);
	if (kevent(actx->mux, &ev, 1, NULL, 0, NULL) < 0 && errno != ENOENT)
	{
		actx_error(actx, "removing kqueue timer from multiplexer: %m");
		return false;
	}

	/* If we're not adding a timer, we're done. */
	if (timeout < 0)
		return true;

	EV_SET(&ev, 1, EVFILT_TIMER, (EV_ADD | EV_ONESHOT), 0, timeout, 0);
	if (kevent(actx->timerfd, &ev, 1, NULL, 0, NULL) < 0)
	{
		actx_error(actx, "setting kqueue timer to %ld: %m", timeout);
		return false;
	}

	EV_SET(&ev, actx->timerfd, EVFILT_READ, EV_ADD, 0, 0, 0);
	if (kevent(actx->mux, &ev, 1, NULL, 0, NULL) < 0)
	{
		actx_error(actx, "adding kqueue timer to multiplexer: %m");
		return false;
	}

	return true;
#else
#error set_timer is not implemented on this platform
#endif
}

/*
 * Returns 1 if the timeout in the multiplexer set has expired since the last
 * call to set_timer(), 0 if the timer is still running, or -1 (with an
 * actx_error() report) if the timer cannot be queried.
 */
static int
timer_expired(struct async_ctx *actx)
{
#if defined(HAVE_SYS_EPOLL_H)
	struct itimerspec spec = {0};

	if (timerfd_gettime(actx->timerfd, &spec) < 0)
	{
		actx_error(actx, "getting timerfd value: %m");
		return -1;
	}

	/*
	 * This implementation assumes we're using single-shot timers. If you
	 * change to using intervals, you'll need to reimplement this function
	 * too, possibly with the read() or select() interfaces for timerfd.
	 */
	Assert(spec.it_interval.tv_sec == 0
		   && spec.it_interval.tv_nsec == 0);

	/* If the remaining time to expiration is zero, we're done. */
	return (spec.it_value.tv_sec == 0
			&& spec.it_value.tv_nsec == 0);
#elif defined(HAVE_SYS_EVENT_H)
	int			res;

	/* Is the timer queue ready? */
	res = PQsocketPoll(actx->timerfd, 1 /* forRead */ , 0, 0);
	if (res < 0)
	{
		actx_error(actx, "checking kqueue for timeout: %m");
		return -1;
	}

	return (res > 0);
#else
#error timer_expired is not implemented on this platform
#endif
}

/*
 * Adds or removes timeouts from the multiplexer set, as directed by the
 * libcurl multi handle.
 */
static int
register_timer(CURLM *curlm, long timeout, void *ctx)
{
	struct async_ctx *actx = ctx;

	/*
	 * There might be an optimization opportunity here: if timeout == 0, we
	 * could signal drive_request to immediately call
	 * curl_multi_socket_action, rather than returning all the way up the
	 * stack only to come right back. But it's not clear that the additional
	 * code complexity is worth it.
	 */
	if (!set_timer(actx, timeout))
		return -1;				/* actx_error already called */

	return 0;
}

/*
 * Prints Curl request debugging information to stderr.
 *
 * Note that this will expose a number of critical secrets, so users have to opt
 * into this (see PGOAUTHDEBUG).
 */
static int
debug_callback(CURL *handle, curl_infotype type, char *data, size_t size,
			   void *clientp)
{
	const char *prefix;
	bool		printed_prefix = false;
	PQExpBufferData buf;

	/* Prefixes are modeled off of the default libcurl debug output. */
	switch (type)
	{
		case CURLINFO_TEXT:
			prefix = "*";
			break;

		case CURLINFO_HEADER_IN:	/* fall through */
		case CURLINFO_DATA_IN:
			prefix = "<";
			break;

		case CURLINFO_HEADER_OUT:	/* fall through */
		case CURLINFO_DATA_OUT:
			prefix = ">";
			break;

		default:
			return 0;
	}

	initPQExpBuffer(&buf);

	/*
	 * Split the output into lines for readability; sometimes multiple headers
	 * are included in a single call. We also don't allow unprintable ASCII
	 * through without a basic <XX> escape.
	 */
	for (int i = 0; i < size; i++)
	{
		char		c = data[i];

		if (!printed_prefix)
		{
			appendPQExpBuffer(&buf, "[libcurl] %s ", prefix);
			printed_prefix = true;
		}

		if (c >= 0x20 && c <= 0x7E)
			appendPQExpBufferChar(&buf, c);
		else if ((type == CURLINFO_HEADER_IN
				  || type == CURLINFO_HEADER_OUT
				  || type == CURLINFO_TEXT)
				 && (c == '\r' || c == '\n'))
		{
			/*
			 * Don't bother emitting <0D><0A> for headers and text; it's not
			 * helpful noise.
			 */
		}
		else
			appendPQExpBuffer(&buf, "<%02X>", c);

		if (c == '\n')
		{
			appendPQExpBufferChar(&buf, c);
			printed_prefix = false;
		}
	}

	if (printed_prefix)
		appendPQExpBufferChar(&buf, '\n');	/* finish the line */

	fprintf(stderr, "%s", buf.data);
	termPQExpBuffer(&buf);
	return 0;
}

/*
 * Initializes the two libcurl handles in the async_ctx. The multi handle,
 * actx->curlm, is what drives the asynchronous engine and tells us what to do
 * next. The easy handle, actx->curl, encapsulates the state for a single
 * request/response. It's added to the multi handle as needed, during
 * start_request().
 */
static bool
setup_curl_handles(struct async_ctx *actx)
{
	/*
	 * Create our multi handle. This encapsulates the entire conversation with
	 * libcurl for this connection.
	 */
	actx->curlm = curl_multi_init();
	if (!actx->curlm)
	{
		/* We don't get a lot of feedback on the failure reason. */
		actx_error(actx, "failed to create libcurl multi handle");
		return false;
	}

	/*
	 * The multi handle tells us what to wait on using two callbacks. These
	 * will manipulate actx->mux as needed.
	 */
	CHECK_MSETOPT(actx, CURLMOPT_SOCKETFUNCTION, register_socket, return false);
	CHECK_MSETOPT(actx, CURLMOPT_SOCKETDATA, actx, return false);
	CHECK_MSETOPT(actx, CURLMOPT_TIMERFUNCTION, register_timer, return false);
	CHECK_MSETOPT(actx, CURLMOPT_TIMERDATA, actx, return false);

	/*
	 * Set up an easy handle. All of our requests are made serially, so we
	 * only ever need to keep track of one.
	 */
	actx->curl = curl_easy_init();
	if (!actx->curl)
	{
		actx_error(actx, "failed to create libcurl handle");
		return false;
	}

	/*
	 * Multi-threaded applications must set CURLOPT_NOSIGNAL. This requires us
	 * to handle the possibility of SIGPIPE ourselves using pq_block_sigpipe;
	 * see pg_fe_run_oauth_flow().
	 *
	 * NB: If libcurl is not built against a friendly DNS resolver (c-ares or
	 * threaded), setting this option prevents DNS lookups from timing out
	 * correctly. We warn about this situation at configure time.
	 *
	 * TODO: Perhaps there's a clever way to warn the user about synchronous
	 * DNS at runtime too? It's not immediately clear how to do that in a
	 * helpful way: for many standard single-threaded use cases, the user
	 * might not care at all, so spraying warnings to stderr would probably do
	 * more harm than good.
	 */
	CHECK_SETOPT(actx, CURLOPT_NOSIGNAL, 1L, return false);

	if (actx->debugging)
	{
		/*
		 * Set a callback for retrieving error information from libcurl, the
		 * function only takes effect when CURLOPT_VERBOSE has been set so
		 * make sure the order is kept.
		 */
		CHECK_SETOPT(actx, CURLOPT_DEBUGFUNCTION, debug_callback, return false);
		CHECK_SETOPT(actx, CURLOPT_VERBOSE, 1L, return false);
	}

	CHECK_SETOPT(actx, CURLOPT_ERRORBUFFER, actx->curl_err, return false);

	/*
	 * Only HTTPS is allowed. (Debug mode additionally allows HTTP; this is
	 * intended for testing only.)
	 *
	 * There's a bit of unfortunate complexity around the choice of
	 * CURLoption. CURLOPT_PROTOCOLS is deprecated in modern Curls, but its
	 * replacement didn't show up until relatively recently.
	 */
	{
#if CURL_AT_LEAST_VERSION(7, 85, 0)
		const CURLoption popt = CURLOPT_PROTOCOLS_STR;
		const char *protos = "https";
		const char *const unsafe = "https,http";
#else
		const CURLoption popt = CURLOPT_PROTOCOLS;
		long		protos = CURLPROTO_HTTPS;
		const long	unsafe = CURLPROTO_HTTPS | CURLPROTO_HTTP;
#endif

		if (actx->debugging)
			protos = unsafe;

		CHECK_SETOPT(actx, popt, protos, return false);
	}

	/*
	 * If we're in debug mode, allow the developer to change the trusted CA
	 * list. For now, this is not something we expose outside of the UNSAFE
	 * mode, because it's not clear that it's useful in production: both libpq
	 * and the user's browser must trust the same authorization servers for
	 * the flow to work at all, so any changes to the roots are likely to be
	 * done system-wide.
	 */
	if (actx->debugging)
	{
		const char *env;

		if ((env = getenv("PGOAUTHCAFILE")) != NULL)
			CHECK_SETOPT(actx, CURLOPT_CAINFO, env, return false);
	}

	/*
	 * Suppress the Accept header to make our request as minimal as possible.
	 * (Ideally we would set it to "application/json" instead, but OpenID is
	 * pretty strict when it comes to provider behavior, so we have to check
	 * what comes back anyway.)
	 */
	actx->headers = curl_slist_append(actx->headers, "Accept:");
	if (actx->headers == NULL)
	{
		actx_error(actx, "out of memory");
		return false;
	}
	CHECK_SETOPT(actx, CURLOPT_HTTPHEADER, actx->headers, return false);

	return true;
}

/*
 * Generic HTTP Request Handlers
 */

/*
 * Response callback from libcurl which appends the response body into
 * actx->work_data (see start_request()). The maximum size of the data is
 * defined by CURL_MAX_WRITE_SIZE which by default is 16kb (and can only be
 * changed by recompiling libcurl).
 */
static size_t
append_data(char *buf, size_t size, size_t nmemb, void *userdata)
{
	struct async_ctx *actx = userdata;
	PQExpBuffer resp = &actx->work_data;
	size_t		len = size * nmemb;

	/* In case we receive data over the threshold, abort the transfer */
	if ((resp->len + len) > MAX_OAUTH_RESPONSE_SIZE)
	{
		actx_error(actx, "response is too large");
		return 0;
	}

	/* The data passed from libcurl is not null-terminated */
	appendBinaryPQExpBuffer(resp, buf, len);

	/*
	 * Signal an error in order to abort the transfer in case we ran out of
	 * memory in accepting the data.
	 */
	if (PQExpBufferBroken(resp))
	{
		actx_error(actx, "out of memory");
		return 0;
	}

	return len;
}

/*
 * Begins an HTTP request on the multi handle. The caller should have set up all
 * request-specific options on actx->curl first. The server's response body will
 * be accumulated in actx->work_data (which will be reset, so don't store
 * anything important there across this call).
 *
 * Once a request is queued, it can be driven to completion via drive_request().
 * If actx->running is zero upon return, the request has already finished and
 * drive_request() can be called without returning control to the client.
 */
static bool
start_request(struct async_ctx *actx)
{
	CURLMcode	err;

	resetPQExpBuffer(&actx->work_data);
	CHECK_SETOPT(actx, CURLOPT_WRITEFUNCTION, append_data, return false);
	CHECK_SETOPT(actx, CURLOPT_WRITEDATA, actx, return false);

	err = curl_multi_add_handle(actx->curlm, actx->curl);
	if (err)
	{
		actx_error(actx, "failed to queue HTTP request: %s",
				   curl_multi_strerror(err));
		return false;
	}

	/*
	 * actx->running tracks the number of running handles, so we can
	 * immediately call back if no waiting is needed.
	 *
	 * Even though this is nominally an asynchronous process, there are some
	 * operations that can synchronously fail by this point (e.g. connections
	 * to closed local ports) or even synchronously succeed if the stars align
	 * (all the libcurl connection caches hit and the server is fast).
	 */
	err = curl_multi_socket_action(actx->curlm, CURL_SOCKET_TIMEOUT, 0, &actx->running);
	if (err)
	{
		actx_error(actx, "asynchronous HTTP request failed: %s",
				   curl_multi_strerror(err));
		return false;
	}

	return true;
}

/*
 * CURL_IGNORE_DEPRECATION was added in 7.87.0. If it's not defined, we can make
 * it a no-op.
 */
#ifndef CURL_IGNORE_DEPRECATION
#define CURL_IGNORE_DEPRECATION(x) x
#endif

/*
 * Drives the multi handle towards completion. The caller should have already
 * set up an asynchronous request via start_request().
 */
static PostgresPollingStatusType
drive_request(struct async_ctx *actx)
{
	CURLMcode	err;
	CURLMsg    *msg;
	int			msgs_left;
	bool		done;

	if (actx->running)
	{
		/*---
		 * There's an async request in progress. Pump the multi handle.
		 *
		 * curl_multi_socket_all() is officially deprecated, because it's
		 * inefficient and pointless if your event loop has already handed you
		 * the exact sockets that are ready. But that's not our use case --
		 * our client has no way to tell us which sockets are ready. (They
		 * don't even know there are sockets to begin with.)
		 *
		 * We can grab the list of triggered events from the multiplexer
		 * ourselves, but that's effectively what curl_multi_socket_all() is
		 * going to do. And there are currently no plans for the Curl project
		 * to remove or break this API, so ignore the deprecation. See
		 *
		 *    https://curl.se/mail/lib-2024-11/0028.html
		 *
		 */
		CURL_IGNORE_DEPRECATION(
			err = curl_multi_socket_all(actx->curlm, &actx->running);
		)

		if (err)
		{
			actx_error(actx, "asynchronous HTTP request failed: %s",
					   curl_multi_strerror(err));
			return PGRES_POLLING_FAILED;
		}

		if (actx->running)
		{
			/* We'll come back again. */
			return PGRES_POLLING_READING;
		}
	}

	done = false;
	while ((msg = curl_multi_info_read(actx->curlm, &msgs_left)) != NULL)
	{
		if (msg->msg != CURLMSG_DONE)
		{
			/*
			 * Future libcurl versions may define new message types; we don't
			 * know how to handle them, so we'll ignore them.
			 */
			continue;
		}

		/* First check the status of the request itself. */
		if (msg->data.result != CURLE_OK)
		{
			/*
			 * If a more specific error hasn't already been reported, use
			 * libcurl's description.
			 */
			if (actx->errbuf.len == 0)
				actx_error_str(actx, curl_easy_strerror(msg->data.result));

			return PGRES_POLLING_FAILED;
		}

		/* Now remove the finished handle; we'll add it back later if needed. */
		err = curl_multi_remove_handle(actx->curlm, msg->easy_handle);
		if (err)
		{
			actx_error(actx, "libcurl easy handle removal failed: %s",
					   curl_multi_strerror(err));
			return PGRES_POLLING_FAILED;
		}

		done = true;
	}

	/* Sanity check. */
	if (!done)
	{
		actx_error(actx, "no result was retrieved for the finished handle");
		return PGRES_POLLING_FAILED;
	}

	return PGRES_POLLING_OK;
}

/*
 * URL-Encoding Helpers
 */

/*
 * Encodes a string using the application/x-www-form-urlencoded format, and
 * appends it to the given buffer.
 */
static void
append_urlencoded(PQExpBuffer buf, const char *s)
{
	char	   *escaped;
	char	   *haystack;
	char	   *match;

	/* The first parameter to curl_easy_escape is deprecated by Curl */
	escaped = curl_easy_escape(NULL, s, 0);
	if (!escaped)
	{
		termPQExpBuffer(buf);	/* mark the buffer broken */
		return;
	}

	/*
	 * curl_easy_escape() almost does what we want, but we need the
	 * query-specific flavor which uses '+' instead of '%20' for spaces. The
	 * Curl command-line tool does this with a simple search-and-replace, so
	 * follow its lead.
	 */
	haystack = escaped;

	while ((match = strstr(haystack, "%20")) != NULL)
	{
		/* Append the unmatched portion, followed by the plus sign. */
		appendBinaryPQExpBuffer(buf, haystack, match - haystack);
		appendPQExpBufferChar(buf, '+');

		/* Keep searching after the match. */
		haystack = match + 3 /* strlen("%20") */ ;
	}

	/* Push the remainder of the string onto the buffer. */
	appendPQExpBufferStr(buf, haystack);

	curl_free(escaped);
}

/*
 * Convenience wrapper for encoding a single string. Returns NULL on allocation
 * failure.
 */
static char *
urlencode(const char *s)
{
	PQExpBufferData buf;

	initPQExpBuffer(&buf);
	append_urlencoded(&buf, s);

	return PQExpBufferDataBroken(buf) ? NULL : buf.data;
}

/*
 * Appends a key/value pair to the end of an application/x-www-form-urlencoded
 * list.
 */
static void
build_urlencoded(PQExpBuffer buf, const char *key, const char *value)
{
	if (buf->len)
		appendPQExpBufferChar(buf, '&');

	append_urlencoded(buf, key);
	appendPQExpBufferChar(buf, '=');
	append_urlencoded(buf, value);
}

/*
 * Specific HTTP Request Handlers
 *
 * This is finally the beginning of the actual application logic. Generally
 * speaking, a single request consists of a start_* and a finish_* step, with
 * drive_request() pumping the machine in between.
 */

/*
 * Queue an OpenID Provider Configuration Request:
 *
 *     https://openid.net/specs/openid-connect-discovery-1_0.html#ProviderConfigurationRequest
 *     https://www.rfc-editor.org/rfc/rfc8414#section-3.1
 *
 * This is done first to get the endpoint URIs we need to contact and to make
 * sure the provider provides a device authorization flow. finish_discovery()
 * will fill in actx->provider.
 */
static bool
start_discovery(struct async_ctx *actx, const char *discovery_uri)
{
	CHECK_SETOPT(actx, CURLOPT_HTTPGET, 1L, return false);
	CHECK_SETOPT(actx, CURLOPT_URL, discovery_uri, return false);

	return start_request(actx);
}

static bool
finish_discovery(struct async_ctx *actx)
{
	long		response_code;

	/*----
	 * Now check the response. OIDC Discovery 1.0 is pretty strict:
	 *
	 *     A successful response MUST use the 200 OK HTTP status code and
	 *     return a JSON object using the application/json content type that
	 *     contains a set of Claims as its members that are a subset of the
	 *     Metadata values defined in Section 3.
	 *
	 * Compared to standard HTTP semantics, this makes life easy -- we don't
	 * need to worry about redirections (which would call the Issuer host
	 * validation into question), or non-authoritative responses, or any other
	 * complications.
	 */
	CHECK_GETINFO(actx, CURLINFO_RESPONSE_CODE, &response_code, return false);

	if (response_code != 200)
	{
		actx_error(actx, "unexpected response code %ld", response_code);
		return false;
	}

	/*
	 * Pull the fields we care about from the document.
	 */
	actx->errctx = "failed to parse OpenID discovery document";
	if (!parse_provider(actx, &actx->provider))
		return false;			/* error message already set */

	/*
	 * Fill in any defaults for OPTIONAL/RECOMMENDED fields we care about.
	 */
	if (!actx->provider.grant_types_supported)
	{
		/*
		 * Per Section 3, the default is ["authorization_code", "implicit"].
		 */
		struct curl_slist *temp = actx->provider.grant_types_supported;

		temp = curl_slist_append(temp, "authorization_code");
		if (temp)
		{
			temp = curl_slist_append(temp, "implicit");
		}

		if (!temp)
		{
			actx_error(actx, "out of memory");
			return false;
		}

		actx->provider.grant_types_supported = temp;
	}

	return true;
}

/*
 * Ensure that the discovery document is provided by the expected issuer.
 * Currently, issuers are statically configured in the connection string.
 */
static bool
check_issuer(struct async_ctx *actx, PGconn *conn)
{
	const struct provider *provider = &actx->provider;
	const char *oauth_issuer_id = conn_oauth_issuer_id(conn);

	Assert(oauth_issuer_id);	/* ensured by setup_oauth_parameters() */
	Assert(provider->issuer);	/* ensured by parse_provider() */

	/*---
	 * We require strict equality for issuer identifiers -- no path or case
	 * normalization, no substitution of default ports and schemes, etc. This
	 * is done to match the rules in OIDC Discovery Sec. 4.3 for config
	 * validation:
	 *
	 *    The issuer value returned MUST be identical to the Issuer URL that
	 *    was used as the prefix to /.well-known/openid-configuration to
	 *    retrieve the configuration information.
	 *
	 * as well as the rules set out in RFC 9207 for avoiding mix-up attacks:
	 *
	 *    Clients MUST then [...] compare the result to the issuer identifier
	 *    of the authorization server where the authorization request was
	 *    sent to. This comparison MUST use simple string comparison as defined
	 *    in Section 6.2.1 of [RFC3986].
	 */
	if (strcmp(oauth_issuer_id, provider->issuer) != 0)
	{
		actx_error(actx,
				   "the issuer identifier (%s) does not match oauth_issuer (%s)",
				   provider->issuer, oauth_issuer_id);
		return false;
	}

	return true;
}

#define HTTPS_SCHEME "https://"
#define OAUTH_GRANT_TYPE_DEVICE_CODE "urn:ietf:params:oauth:grant-type:device_code"

/*
 * Ensure that the provider supports the Device Authorization flow (i.e. it
 * provides an authorization endpoint, and both the token and authorization
 * endpoint URLs seem reasonable).
 */
static bool
check_for_device_flow(struct async_ctx *actx)
{
	const struct provider *provider = &actx->provider;

	Assert(provider->issuer);	/* ensured by parse_provider() */
	Assert(provider->token_endpoint);	/* ensured by parse_provider() */

	if (!provider->device_authorization_endpoint)
	{
		actx_error(actx,
				   "issuer \"%s\" does not provide a device authorization endpoint",
				   provider->issuer);
		return false;
	}

	/*
	 * The original implementation checked that OAUTH_GRANT_TYPE_DEVICE_CODE
	 * was present in the discovery document's grant_types_supported list. MS
	 * Entra does not advertise this grant type, though, and since it doesn't
	 * make sense to stand up a device_authorization_endpoint without also
	 * accepting device codes at the token_endpoint, that's the only thing we
	 * currently require.
	 */

	/*
	 * Although libcurl will fail later if the URL contains an unsupported
	 * scheme, that error message is going to be a bit opaque. This is a
	 * decent time to bail out if we're not using HTTPS for the endpoints
	 * we'll use for the flow.
	 */
	if (!actx->debugging)
	{
		if (pg_strncasecmp(provider->device_authorization_endpoint,
						   HTTPS_SCHEME, strlen(HTTPS_SCHEME)) != 0)
		{
			actx_error(actx,
					   "device authorization endpoint \"%s\" must use HTTPS",
					   provider->device_authorization_endpoint);
			return false;
		}

		if (pg_strncasecmp(provider->token_endpoint,
						   HTTPS_SCHEME, strlen(HTTPS_SCHEME)) != 0)
		{
			actx_error(actx,
					   "token endpoint \"%s\" must use HTTPS",
					   provider->token_endpoint);
			return false;
		}
	}

	return true;
}

/*
 * Adds the client ID (and secret, if provided) to the current request, using
 * either HTTP headers or the request body.
 */
static bool
add_client_identification(struct async_ctx *actx, PQExpBuffer reqbody, PGconn *conn)
{
	const char *oauth_client_id = conn_oauth_client_id(conn);
	const char *oauth_client_secret = conn_oauth_client_secret(conn);

	bool		success = false;
	char	   *username = NULL;
	char	   *password = NULL;

	if (oauth_client_secret)	/* Zero-length secrets are permitted! */
	{
		/*----
		 * Use HTTP Basic auth to send the client_id and secret. Per RFC 6749,
		 * Sec. 2.3.1,
		 *
		 *   Including the client credentials in the request-body using the
		 *   two parameters is NOT RECOMMENDED and SHOULD be limited to
		 *   clients unable to directly utilize the HTTP Basic authentication
		 *   scheme (or other password-based HTTP authentication schemes).
		 *
		 * Additionally:
		 *
		 *   The client identifier is encoded using the
		 *   "application/x-www-form-urlencoded" encoding algorithm per Appendix
		 *   B, and the encoded value is used as the username; the client
		 *   password is encoded using the same algorithm and used as the
		 *   password.
		 *
		 * (Appendix B modifies application/x-www-form-urlencoded by requiring
		 * an initial UTF-8 encoding step. Since the client ID and secret must
		 * both be 7-bit ASCII -- RFC 6749 Appendix A -- we don't worry about
		 * that in this function.)
		 *
		 * client_id is not added to the request body in this case. Not only
		 * would it be redundant, but some providers in the wild (e.g. Okta)
		 * refuse to accept it.
		 */
		username = urlencode(oauth_client_id);
		password = urlencode(oauth_client_secret);

		if (!username || !password)
		{
			actx_error(actx, "out of memory");
			goto cleanup;
		}

		CHECK_SETOPT(actx, CURLOPT_HTTPAUTH, CURLAUTH_BASIC, goto cleanup);
		CHECK_SETOPT(actx, CURLOPT_USERNAME, username, goto cleanup);
		CHECK_SETOPT(actx, CURLOPT_PASSWORD, password, goto cleanup);

		actx->used_basic_auth = true;
	}
	else
	{
		/*
		 * If we're not otherwise authenticating, client_id is REQUIRED in the
		 * request body.
		 */
		build_urlencoded(reqbody, "client_id", oauth_client_id);

		CHECK_SETOPT(actx, CURLOPT_HTTPAUTH, CURLAUTH_NONE, goto cleanup);
		actx->used_basic_auth = false;
	}

	success = true;

cleanup:
	free(username);
	free(password);

	return success;
}

/*
 * Queue a Device Authorization Request:
 *
 *     https://www.rfc-editor.org/rfc/rfc8628#section-3.1
 *
 * This is the second step. We ask the provider to verify the end user out of
 * band and authorize us to act on their behalf; it will give us the required
 * nonces for us to later poll the request status, which we'll grab in
 * finish_device_authz().
 */
static bool
start_device_authz(struct async_ctx *actx, PGconn *conn)
{
	const char *oauth_scope = conn_oauth_scope(conn);
	const char *device_authz_uri = actx->provider.device_authorization_endpoint;
	PQExpBuffer work_buffer = &actx->work_data;

	Assert(conn_oauth_client_id(conn)); /* ensured by setup_oauth_parameters() */
	Assert(device_authz_uri);	/* ensured by check_for_device_flow() */

	/* Construct our request body. */
	resetPQExpBuffer(work_buffer);
	if (oauth_scope && oauth_scope[0])
		build_urlencoded(work_buffer, "scope", oauth_scope);

	if (!add_client_identification(actx, work_buffer, conn))
		return false;

	if (PQExpBufferBroken(work_buffer))
	{
		actx_error(actx, "out of memory");
		return false;
	}

	/* Make our request. */
	CHECK_SETOPT(actx, CURLOPT_URL, device_authz_uri, return false);
	CHECK_SETOPT(actx, CURLOPT_COPYPOSTFIELDS, work_buffer->data, return false);

	return start_request(actx);
}

static bool
finish_device_authz(struct async_ctx *actx)
{
	long		response_code;

	CHECK_GETINFO(actx, CURLINFO_RESPONSE_CODE, &response_code, return false);

	/*
	 * Per RFC 8628, Section 3, a successful device authorization response
	 * uses 200 OK.
	 */
	if (response_code == 200)
	{
		actx->errctx = "failed to parse device authorization";
		if (!parse_device_authz(actx, &actx->authz))
			return false;		/* error message already set */

		return true;
	}

	/*
	 * The device authorization endpoint uses the same error response as the
	 * token endpoint, so the error handling roughly follows
	 * finish_token_request(). The key difference is that an error here is
	 * immediately fatal.
	 */
	if (response_code == 400 || response_code == 401)
	{
		struct token_error err = {0};

		if (!parse_token_error(actx, &err))
		{
			free_token_error(&err);
			return false;
		}

		/* Copy the token error into the context error buffer */
		record_token_error(actx, &err);

		free_token_error(&err);
		return false;
	}

	/* Any other response codes are considered invalid */
	actx_error(actx, "unexpected response code %ld", response_code);
	return false;
}

/*
 * Queue an Access Token Request:
 *
 *     https://www.rfc-editor.org/rfc/rfc6749#section-4.1.3
 *
 * This is the final step. We continually poll the token endpoint to see if the
 * user has authorized us yet. finish_token_request() will pull either the token
 * or a (ideally temporary) error status from the provider.
 */
static bool
start_token_request(struct async_ctx *actx, PGconn *conn)
{
	const char *token_uri = actx->provider.token_endpoint;
	const char *device_code = actx->authz.device_code;
	PQExpBuffer work_buffer = &actx->work_data;

	Assert(conn_oauth_client_id(conn)); /* ensured by setup_oauth_parameters() */
	Assert(token_uri);			/* ensured by parse_provider() */
	Assert(device_code);		/* ensured by parse_device_authz() */

	/* Construct our request body. */
	resetPQExpBuffer(work_buffer);
	build_urlencoded(work_buffer, "device_code", device_code);
	build_urlencoded(work_buffer, "grant_type", OAUTH_GRANT_TYPE_DEVICE_CODE);

	if (!add_client_identification(actx, work_buffer, conn))
		return false;

	if (PQExpBufferBroken(work_buffer))
	{
		actx_error(actx, "out of memory");
		return false;
	}

	/* Make our request. */
	CHECK_SETOPT(actx, CURLOPT_URL, token_uri, return false);
	CHECK_SETOPT(actx, CURLOPT_COPYPOSTFIELDS, work_buffer->data, return false);

	return start_request(actx);
}

static bool
finish_token_request(struct async_ctx *actx, struct token *tok)
{
	long		response_code;

	CHECK_GETINFO(actx, CURLINFO_RESPONSE_CODE, &response_code, return false);

	/*
	 * Per RFC 6749, Section 5, a successful response uses 200 OK.
	 */
	if (response_code == 200)
	{
		actx->errctx = "failed to parse access token response";
		if (!parse_access_token(actx, tok))
			return false;		/* error message already set */

		return true;
	}

	/*
	 * An error response uses either 400 Bad Request or 401 Unauthorized.
	 * There are references online to implementations using 403 for error
	 * return which would violate the specification. For now we stick to the
	 * specification but we might have to revisit this.
	 */
	if (response_code == 400 || response_code == 401)
	{
		if (!parse_token_error(actx, &tok->err))
			return false;

		return true;
	}

	/* Any other response codes are considered invalid */
	actx_error(actx, "unexpected response code %ld", response_code);
	return false;
}

/*
 * Finishes the token request and examines the response. If the flow has
 * completed, a valid token will be returned via the parameter list. Otherwise,
 * the token parameter remains unchanged, and the caller needs to wait for
 * another interval (which will have been increased in response to a slow_down
 * message from the server) before starting a new token request.
 *
 * False is returned only for permanent error conditions.
 */
static bool
handle_token_response(struct async_ctx *actx, char **token)
{
	bool		success = false;
	struct token tok = {0};
	const struct token_error *err;

	if (!finish_token_request(actx, &tok))
		goto token_cleanup;

	/* A successful token request gives either a token or an in-band error. */
	Assert(tok.access_token || tok.err.error);

	if (tok.access_token)
	{
		*token = tok.access_token;
		tok.access_token = NULL;

		success = true;
		goto token_cleanup;
	}

	/*
	 * authorization_pending and slow_down are the only acceptable errors;
	 * anything else and we bail. These are defined in RFC 8628, Sec. 3.5.
	 */
	err = &tok.err;
	if (strcmp(err->error, "authorization_pending") != 0 &&
		strcmp(err->error, "slow_down") != 0)
	{
		record_token_error(actx, err);
		goto token_cleanup;
	}

	/*
	 * A slow_down error requires us to permanently increase our retry
	 * interval by five seconds.
	 */
	if (strcmp(err->error, "slow_down") == 0)
	{
		int			prev_interval = actx->authz.interval;

		actx->authz.interval += 5;
		if (actx->authz.interval < prev_interval)
		{
			actx_error(actx, "slow_down interval overflow");
			goto token_cleanup;
		}
	}

	success = true;

token_cleanup:
	free_token(&tok);
	return success;
}

/*
 * Displays a device authorization prompt for action by the end user, either via
 * the PQauthDataHook, or by a message on standard error if no hook is set.
 */
static bool
prompt_user(struct async_ctx *actx, PGconn *conn)
{
	int			res;
	PGpromptOAuthDevice prompt = {
		.verification_uri = actx->authz.verification_uri,
		.user_code = actx->authz.user_code,
		.verification_uri_complete = actx->authz.verification_uri_complete,
		.expires_in = actx->authz.expires_in,
	};
	PQauthDataHook_type hook = PQgetAuthDataHook();

	res = hook(PQAUTHDATA_PROMPT_OAUTH_DEVICE, conn, &prompt);

	if (!res)
	{
		/*
		 * translator: The first %s is a URL for the user to visit in a
		 * browser, and the second %s is a code to be copy-pasted there.
		 */
		fprintf(stderr, libpq_gettext("Visit %s and enter the code: %s\n"),
				prompt.verification_uri, prompt.user_code);
	}
	else if (res < 0)
	{
		actx_error(actx, "device prompt failed");
		return false;
	}

	return true;
}

/*
 * Calls curl_global_init() in a thread-safe way.
 *
 * libcurl has stringent requirements for the thread context in which you call
 * curl_global_init(), because it's going to try initializing a bunch of other
 * libraries (OpenSSL, Winsock, etc). Recent versions of libcurl have improved
 * the thread-safety situation, but there's a chicken-and-egg problem at
 * runtime: you can't check the thread safety until you've initialized libcurl,
 * which you can't do from within a thread unless you know it's thread-safe...
 *
 * Returns true if initialization was successful. Successful or not, this
 * function will not try to reinitialize Curl on successive calls.
 */
static bool
initialize_curl(PGconn *conn)
{
	/*
	 * Don't let the compiler play tricks with this variable. In the
	 * HAVE_THREADSAFE_CURL_GLOBAL_INIT case, we don't care if two threads
	 * enter simultaneously, but we do care if this gets set transiently to
	 * PG_BOOL_YES/NO in cases where that's not the final answer.
	 */
	static volatile PGTernaryBool init_successful = PG_BOOL_UNKNOWN;
#if HAVE_THREADSAFE_CURL_GLOBAL_INIT
	curl_version_info_data *info;
#endif

#if !HAVE_THREADSAFE_CURL_GLOBAL_INIT

	/*
	 * Lock around the whole function. If a libpq client performs its own work
	 * with libcurl, it must either ensure that Curl is initialized safely
	 * before calling us (in which case our call will be a no-op), or else it
	 * must guard its own calls to curl_global_init() with a registered
	 * threadlock handler. See PQregisterThreadLock().
	 */
	pglock_thread();
#endif

	/*
	 * Skip initialization if we've already done it. (Curl tracks the number
	 * of calls; there's no point in incrementing the counter every time we
	 * connect.)
	 */
	if (init_successful == PG_BOOL_YES)
		goto done;
	else if (init_successful == PG_BOOL_NO)
	{
		libpq_append_conn_error(conn,
								"curl_global_init previously failed during OAuth setup");
		goto done;
	}

	/*
	 * We know we've already initialized Winsock by this point (see
	 * pqMakeEmptyPGconn()), so we should be able to safely skip that bit. But
	 * we have to tell libcurl to initialize everything else, because other
	 * pieces of our client executable may already be using libcurl for their
	 * own purposes. If we initialize libcurl with only a subset of its
	 * features, we could break those other clients nondeterministically, and
	 * that would probably be a nightmare to debug.
	 *
	 * If some other part of the program has already called this, it's a
	 * no-op.
	 */
	if (curl_global_init(CURL_GLOBAL_ALL & ~CURL_GLOBAL_WIN32) != CURLE_OK)
	{
		libpq_append_conn_error(conn,
								"curl_global_init failed during OAuth setup");
		init_successful = PG_BOOL_NO;
		goto done;
	}

#if HAVE_THREADSAFE_CURL_GLOBAL_INIT

	/*
	 * If we determined at configure time that the Curl installation is
	 * thread-safe, our job here is much easier. We simply initialize above
	 * without any locking (concurrent or duplicated calls are fine in that
	 * situation), then double-check to make sure the runtime setting agrees,
	 * to try to catch silent downgrades.
	 */
	info = curl_version_info(CURLVERSION_NOW);
	if (!(info->features & CURL_VERSION_THREADSAFE))
	{
		/*
		 * In a downgrade situation, the damage is already done. Curl global
		 * state may be corrupted. Be noisy.
		 */
		libpq_append_conn_error(conn, "libcurl is no longer thread-safe\n"
								"\tCurl initialization was reported thread-safe when libpq\n"
								"\twas compiled, but the currently installed version of\n"
								"\tlibcurl reports that it is not. Recompile libpq against\n"
								"\tthe installed version of libcurl.");
		init_successful = PG_BOOL_NO;
		goto done;
	}
#endif

	init_successful = PG_BOOL_YES;

done:
#if !HAVE_THREADSAFE_CURL_GLOBAL_INIT
	pgunlock_thread();
#endif
	return (init_successful == PG_BOOL_YES);
}

/*
 * The core nonblocking libcurl implementation. This will be called several
 * times to pump the async engine.
 *
 * The architecture is based on PQconnectPoll(). The first half drives the
 * connection state forward as necessary, returning if we're not ready to
 * proceed to the next step yet. The second half performs the actual transition
 * between states.
 *
 * You can trace the overall OAuth flow through the second half. It's linear
 * until we get to the end, where we flip back and forth between
 * OAUTH_STEP_TOKEN_REQUEST and OAUTH_STEP_WAIT_INTERVAL to regularly ping the
 * provider.
 */
static PostgresPollingStatusType
pg_fe_run_oauth_flow_impl(PGconn *conn)
{
	fe_oauth_state *state = conn_sasl_state(conn);
	struct async_ctx *actx;
	char	   *oauth_token = NULL;
	PQExpBuffer errbuf;

	if (!initialize_curl(conn))
		return PGRES_POLLING_FAILED;

	if (!state->async_ctx)
	{
		/*
		 * Create our asynchronous state, and hook it into the upper-level
		 * OAuth state immediately, so any failures below won't leak the
		 * context allocation.
		 */
		actx = calloc(1, sizeof(*actx));
		if (!actx)
		{
			libpq_append_conn_error(conn, "out of memory");
			return PGRES_POLLING_FAILED;
		}

		actx->mux = PGINVALID_SOCKET;
		actx->timerfd = -1;

		/* Should we enable unsafe features? */
		actx->debugging = oauth_unsafe_debugging_enabled();

		state->async_ctx = actx;

		initPQExpBuffer(&actx->work_data);
		initPQExpBuffer(&actx->errbuf);

		if (!setup_multiplexer(actx))
			goto error_return;

		if (!setup_curl_handles(actx))
			goto error_return;
	}

	actx = state->async_ctx;

	do
	{
		/* By default, the multiplexer is the altsock. Reassign as desired. */
		set_conn_altsock(conn, actx->mux);

		switch (actx->step)
		{
			case OAUTH_STEP_INIT:
				break;

			case OAUTH_STEP_DISCOVERY:
			case OAUTH_STEP_DEVICE_AUTHORIZATION:
			case OAUTH_STEP_TOKEN_REQUEST:
				{
					PostgresPollingStatusType status;

					status = drive_request(actx);

					if (status == PGRES_POLLING_FAILED)
						goto error_return;
					else if (status != PGRES_POLLING_OK)
					{
						/* not done yet */
						return status;
					}

					break;
				}

			case OAUTH_STEP_WAIT_INTERVAL:

				/*
				 * The client application is supposed to wait until our timer
				 * expires before calling PQconnectPoll() again, but that
				 * might not happen. To avoid sending a token request early,
				 * check the timer before continuing.
				 */
				if (!timer_expired(actx))
				{
					set_conn_altsock(conn, actx->timerfd);
					return PGRES_POLLING_READING;
				}

				/* Disable the expired timer. */
				if (!set_timer(actx, -1))
					goto error_return;

				break;
		}

		/*
		 * Each case here must ensure that actx->running is set while we're
		 * waiting on some asynchronous work. Most cases rely on
		 * start_request() to do that for them.
		 */
		switch (actx->step)
		{
			case OAUTH_STEP_INIT:
				actx->errctx = "failed to fetch OpenID discovery document";
				if (!start_discovery(actx, conn_oauth_discovery_uri(conn)))
					goto error_return;

				actx->step = OAUTH_STEP_DISCOVERY;
				break;

			case OAUTH_STEP_DISCOVERY:
				if (!finish_discovery(actx))
					goto error_return;

				if (!check_issuer(actx, conn))
					goto error_return;

				actx->errctx = "cannot run OAuth device authorization";
				if (!check_for_device_flow(actx))
					goto error_return;

				actx->errctx = "failed to obtain device authorization";
				if (!start_device_authz(actx, conn))
					goto error_return;

				actx->step = OAUTH_STEP_DEVICE_AUTHORIZATION;
				break;

			case OAUTH_STEP_DEVICE_AUTHORIZATION:
				if (!finish_device_authz(actx))
					goto error_return;

				actx->errctx = "failed to obtain access token";
				if (!start_token_request(actx, conn))
					goto error_return;

				actx->step = OAUTH_STEP_TOKEN_REQUEST;
				break;

			case OAUTH_STEP_TOKEN_REQUEST:
				if (!handle_token_response(actx, &oauth_token))
					goto error_return;

				/*
				 * Hook any oauth_token into the PGconn immediately so that
				 * the allocation isn't lost in case of an error.
				 */
				set_conn_oauth_token(conn, oauth_token);

				if (!actx->user_prompted)
				{
					/*
					 * Now that we know the token endpoint isn't broken, give
					 * the user the login instructions.
					 */
					if (!prompt_user(actx, conn))
						goto error_return;

					actx->user_prompted = true;
				}

				if (oauth_token)
					break;		/* done! */

				/*
				 * Wait for the required interval before issuing the next
				 * request.
				 */
				if (!set_timer(actx, actx->authz.interval * 1000))
					goto error_return;

				/*
				 * No Curl requests are running, so we can simplify by having
				 * the client wait directly on the timerfd rather than the
				 * multiplexer.
				 */
				set_conn_altsock(conn, actx->timerfd);

				actx->step = OAUTH_STEP_WAIT_INTERVAL;
				actx->running = 1;
				break;

			case OAUTH_STEP_WAIT_INTERVAL:
				actx->errctx = "failed to obtain access token";
				if (!start_token_request(actx, conn))
					goto error_return;

				actx->step = OAUTH_STEP_TOKEN_REQUEST;
				break;
		}

		/*
		 * The vast majority of the time, if we don't have a token at this
		 * point, actx->running will be set. But there are some corner cases
		 * where we can immediately loop back around; see start_request().
		 */
	} while (!oauth_token && !actx->running);

	/* If we've stored a token, we're done. Otherwise come back later. */
	return oauth_token ? PGRES_POLLING_OK : PGRES_POLLING_READING;

error_return:
	errbuf = conn_errorMessage(conn);

	/*
	 * Assemble the three parts of our error: context, body, and detail. See
	 * also the documentation for struct async_ctx.
	 */
	if (actx->errctx)
		appendPQExpBuffer(errbuf, "%s: ", libpq_gettext(actx->errctx));

	if (PQExpBufferDataBroken(actx->errbuf))
		appendPQExpBufferStr(errbuf, libpq_gettext("out of memory"));
	else
		appendPQExpBufferStr(errbuf, actx->errbuf.data);

	if (actx->curl_err[0])
	{
		appendPQExpBuffer(errbuf, " (libcurl: %s)", actx->curl_err);

		/* Sometimes libcurl adds a newline to the error buffer. :( */
		if (errbuf->len >= 2 && errbuf->data[errbuf->len - 2] == '\n')
		{
			errbuf->data[errbuf->len - 2] = ')';
			errbuf->data[errbuf->len - 1] = '\0';
			errbuf->len--;
		}
	}

	appendPQExpBufferChar(errbuf, '\n');

	return PGRES_POLLING_FAILED;
}

/*
 * The top-level entry point. This is a convenient place to put necessary
 * wrapper logic before handing off to the true implementation, above.
 */
PostgresPollingStatusType
pg_fe_run_oauth_flow(PGconn *conn)
{
	PostgresPollingStatusType result;
#ifndef WIN32
	sigset_t	osigset;
	bool		sigpipe_pending;
	bool		masked;

	/*---
	 * Ignore SIGPIPE on this thread during all Curl processing.
	 *
	 * Because we support multiple threads, we have to set up libcurl with
	 * CURLOPT_NOSIGNAL, which disables its default global handling of
	 * SIGPIPE. From the Curl docs:
	 *
	 *     libcurl makes an effort to never cause such SIGPIPE signals to
	 *     trigger, but some operating systems have no way to avoid them and
	 *     even on those that have there are some corner cases when they may
	 *     still happen, contrary to our desire.
	 *
	 * Note that libcurl is also at the mercy of its DNS resolution and SSL
	 * libraries; if any of them forget a MSG_NOSIGNAL then we're in trouble.
	 * Modern platforms and libraries seem to get it right, so this is a
	 * difficult corner case to exercise in practice, and unfortunately it's
	 * not really clear whether it's necessary in all cases.
	 */
	masked = (pq_block_sigpipe(&osigset, &sigpipe_pending) == 0);
#endif

	result = pg_fe_run_oauth_flow_impl(conn);

#ifndef WIN32
	if (masked)
	{
		/*
		 * Undo the SIGPIPE mask. Assume we may have gotten EPIPE (we have no
		 * way of knowing at this level).
		 */
		pq_reset_sigpipe(&osigset, sigpipe_pending, true /* EPIPE, maybe */ );
	}
#endif

	return result;
}
