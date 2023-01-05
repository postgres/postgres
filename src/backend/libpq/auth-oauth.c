/*-------------------------------------------------------------------------
 *
 * auth-oauth.c
 *	  Server-side implementation of the SASL OAUTHBEARER mechanism.
 *
 * See the following RFC for more details:
 * - RFC 7628: https://tools.ietf.org/html/rfc7628
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/libpq/auth-oauth.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <fcntl.h>

#include "common/oauth-common.h"
#include "lib/stringinfo.h"
#include "libpq/auth.h"
#include "libpq/hba.h"
#include "libpq/oauth.h"
#include "libpq/sasl.h"
#include "storage/fd.h"
#include "miscadmin.h"
#include "utils/memutils.h"

static OAuthProvider* oauth_provider = NULL;

/*----------------------------------------------------------------
 * OAuth Authentication
 *----------------------------------------------------------------
 */
static List *oauth_providers = NIL;

static void  oauth_get_mechanisms(Port *port, StringInfo buf);
static void *oauth_init(Port *port, const char *selected_mech, const char *shadow_pass);
static int   oauth_exchange(void *opaq, const char *input, int inputlen,
							char **output, int *outputlen, const char **logdetail);
static bool
validate(Port *port, const char *auth, const char **logdetail);

/* Mechanism declaration */
const pg_be_sasl_mech pg_be_oauth_mech = {
	oauth_get_mechanisms,
	oauth_init,
	oauth_exchange,

	PG_MAX_AUTH_TOKEN_LENGTH,
};


typedef enum
{
	OAUTH_STATE_INIT = 0,
	OAUTH_STATE_ERROR,
	OAUTH_STATE_FINISHED,
} oauth_state;

struct oauth_ctx
{
	oauth_state	state;
	Port	   *port;
	const char *discovery_uri;
	const char *scope;
};

static char *sanitize_char(char c);
static char *parse_kvpairs_for_auth(char **input);
static void generate_error_response(struct oauth_ctx *ctx, char **output, int *outputlen);
static bool run_oauth_provider(Port *port, const char *token);

#define KVSEP 0x01
#define AUTH_KEY "auth"
#define BEARER_SCHEME "Bearer "

/*----------------------------------------------------------------
 * OAuth Token Validator
 *----------------------------------------------------------------
 */

/*
 * RegisterOAuthProvider registers a OAuth Token Validator to be
 * used for oauth token validation. It validates the token and adds the validator
 * name and it's hooks to a list of loaded token validator. The right validator's
 * hooks can then be called based on the validator name specified in
 * pg_hba.conf.
 *
 * This function should be called in _PG_init() by any extension looking to
 * add a custom authentication method.
 */
void
RegisterOAuthProvider(
	const char *provider_name,
	OAuthProviderCheck_hook_type OAuthProviderCheck_hook,
	OAuthProviderError_hook_type OAuthProviderError_hook,
	OAuthProviderOptions_hook_type OAuthProviderOptions_hook
)
{	
	if (!process_shared_preload_libraries_in_progress)
	{
		ereport(ERROR,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			errmsg("RegisterOAuthProvider can only be called by a shared_preload_library")));
		return;
	}

	MemoryContext oldcxt;
	if (oauth_provider == NULL)
	{
		oldcxt = MemoryContextSwitchTo(TopMemoryContext);
		oauth_provider = palloc(sizeof(OAuthProvider));
		oauth_provider->name = pstrdup(provider_name);
		oauth_provider->oauth_provider_hook = OAuthProviderCheck_hook;
		oauth_provider->oauth_error_hook = OAuthProviderError_hook;
		oauth_provider->oauth_options_hook = OAuthProviderOptions_hook;
		oauth_providers = lappend(oauth_providers, oauth_provider);
		MemoryContextSwitchTo(oldcxt);	
	}
	else
	{
		if (oauth_provider && oauth_provider->name)
		{
			ereport(ERROR,
				(errmsg("OAuth provider \"%s\" is already loaded.",
					oauth_provider->name)));
		}
		else
		{
			ereport(ERROR,
				(errmsg("OAuth provider is already loaded.")));
		}
	}
}

/*
 * Returns the oauth provider (which includes it's
 * callback functions) based on name specified.
 */
OAuthProvider *get_provider_by_name(const char *name)
{
	ListCell *lc;
	foreach(lc, oauth_providers)
	{
		OAuthProvider *provider = (OAuthProvider *) lfirst(lc);		
		if (strcmp(provider->name, name) == 0)
		{
			return provider;
		}
	}

	return NULL;
}

static void
oauth_get_mechanisms(Port *port, StringInfo buf)
{
	/* Only OAUTHBEARER is supported. */
	appendStringInfoString(buf, OAUTHBEARER_NAME);
	appendStringInfoChar(buf, '\0');
}

static void *
oauth_init(Port *port, const char *selected_mech, const char *shadow_pass)
{
	struct oauth_ctx *ctx;
	
	OAuthProviderOptions *oauth_options = oauth_provider->oauth_options_hook(port);
	if (strcmp(selected_mech, OAUTHBEARER_NAME))
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("client selected an invalid SASL authentication mechanism")));

	ctx = palloc0(sizeof(*ctx));

	ctx->state = OAUTH_STATE_INIT;
	ctx->port = port;
	ctx->scope = oauth_options->scope;
	ctx->discovery_uri = oauth_options->oauth_discovery_uri;

	return ctx;
}

static int
oauth_exchange(void *opaq, const char *input, int inputlen,
			   char **output, int *outputlen, const char **logdetail)
{
	char   *p;
	char	cbind_flag;
	char   *auth;

	struct oauth_ctx *ctx = opaq;

	*output = NULL;
	*outputlen = -1;

	/*
	 * If the client didn't include an "Initial Client Response" in the
	 * SASLInitialResponse message, send an empty challenge, to which the
	 * client will respond with the same data that usually comes in the
	 * Initial Client Response.
	 */
	if (input == NULL)
	{
		Assert(ctx->state == OAUTH_STATE_INIT);

		*output = pstrdup("");
		*outputlen = 0;
		return PG_SASL_EXCHANGE_CONTINUE;
	}

	/*
	 * Check that the input length agrees with the string length of the input.
	 */
	if (inputlen == 0)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("malformed OAUTHBEARER message"),
				 errdetail("The message is empty.")));
	if (inputlen != strlen(input))
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("malformed OAUTHBEARER message"),
				 errdetail("Message length does not match input length.")));

	switch (ctx->state)
	{
		case OAUTH_STATE_INIT:
			/* Handle this case below. */
			break;

		case OAUTH_STATE_ERROR:
			/*
			 * Only one response is valid for the client during authentication
			 * failure: a single kvsep.
			 */
			if (inputlen != 1 || *input != KVSEP)
				ereport(ERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("malformed OAUTHBEARER message"),
						 errdetail("Client did not send a kvsep response.")));

			/* The (failed) handshake is now complete. */
			ctx->state = OAUTH_STATE_FINISHED;
			return PG_SASL_EXCHANGE_FAILURE;

		default:
			elog(ERROR, "invalid OAUTHBEARER exchange state");
			return PG_SASL_EXCHANGE_FAILURE;
	}

	/* Handle the client's initial message. */
	p = pstrdup(input);

	/*
	 * OAUTHBEARER does not currently define a channel binding (so there is no
	 * OAUTHBEARER-PLUS, and we do not accept a 'p' specifier). We accept a 'y'
	 * specifier purely for the remote chance that a future specification could
	 * define one; then future clients can still interoperate with this server
	 * implementation. 'n' is the expected case.
	 */
	cbind_flag = *p;
	switch (cbind_flag)
	{
		case 'p':
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("malformed OAUTHBEARER message"),
					 errdetail("The server does not support channel binding for OAuth, but the client message includes channel binding data.")));
			break;

		case 'y': /* fall through */
		case 'n':
			p++;
			if (*p != ',')
				ereport(ERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("malformed OAUTHBEARER message"),
						 errdetail("Comma expected, but found character %s.",
								   sanitize_char(*p))));
			p++;
			break;

		default:
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("malformed OAUTHBEARER message"),
					 errdetail("Unexpected channel-binding flag %s.",
							   sanitize_char(cbind_flag))));
	}

	/*
	 * Forbid optional authzid (authorization identity).  We don't support it.
	 */
	if (*p == 'a')
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("client uses authorization identity, but it is not supported")));
	if (*p != ',')
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("malformed OAUTHBEARER message"),
				 errdetail("Unexpected attribute %s in client-first-message.",
						   sanitize_char(*p))));
	p++;

	/* All remaining fields are separated by the RFC's kvsep (\x01). */
	if (*p != KVSEP)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("malformed OAUTHBEARER message"),
				 errdetail("Key-value separator expected, but found character %s.",
						   sanitize_char(*p))));
	p++;

	auth = parse_kvpairs_for_auth(&p);
	if (!auth)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("malformed OAUTHBEARER message"),
				 errdetail("Message does not contain an auth value.")));

	/* We should be at the end of our message. */
	if (*p)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("malformed OAUTHBEARER message"),
				 errdetail("Message contains additional data after the final terminator.")));

	if (!validate(ctx->port, auth, logdetail))
	{
		generate_error_response(ctx, output, outputlen);

		ctx->state = OAUTH_STATE_ERROR;
		return PG_SASL_EXCHANGE_CONTINUE;
	}

	ctx->state = OAUTH_STATE_FINISHED;
	return PG_SASL_EXCHANGE_SUCCESS;
}

/*
 * Convert an arbitrary byte to printable form.  For error messages.
 *
 * If it's a printable ASCII character, print it as a single character.
 * otherwise, print it in hex.
 *
 * The returned pointer points to a static buffer.
 */
static char *
sanitize_char(char c)
{
	static char buf[5];

	if (c >= 0x21 && c <= 0x7E)
		snprintf(buf, sizeof(buf), "'%c'", c);
	else
		snprintf(buf, sizeof(buf), "0x%02x", (unsigned char) c);
	return buf;
}

/*
 * Consumes all kvpairs in an OAUTHBEARER exchange message. If the "auth" key is
 * found, its value is returned.
 */
static char *
parse_kvpairs_for_auth(char **input)
{
	char   *pos = *input;
	char   *auth = NULL;

	/*
	 * The relevant ABNF, from Sec. 3.1:
	 *
	 *     kvsep          = %x01
	 *     key            = 1*(ALPHA)
	 *     value          = *(VCHAR / SP / HTAB / CR / LF )
	 *     kvpair         = key "=" value kvsep
	 *   ;;gs2-header     = See RFC 5801
	 *     client-resp    = (gs2-header kvsep *kvpair kvsep) / kvsep
	 *
	 * By the time we reach this code, the gs2-header and initial kvsep have
	 * already been validated. We start at the beginning of the first kvpair.
	 */

	while (*pos)
	{
		char   *end;
		char   *sep;
		char   *key;
		char   *value;

		/*
		 * Find the end of this kvpair. Note that input is null-terminated by
		 * the SASL code, so the strchr() is bounded.
		 */
		end = strchr(pos, KVSEP);
		if (!end)
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("malformed OAUTHBEARER message"),
					 errdetail("Message contains an unterminated key/value pair.")));
		*end = '\0';

		if (pos == end)
		{
			/* Empty kvpair, signifying the end of the list. */
			*input = pos + 1;
			return auth;
		}

		/*
		 * Find the end of the key name.
		 *
		 * TODO further validate the key/value grammar? empty keys, bad chars...
		 */
		sep = strchr(pos, '=');
		if (!sep)
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("malformed OAUTHBEARER message"),
					 errdetail("Message contains a key without a value.")));
		*sep = '\0';

		/* Both key and value are now safely terminated. */
		key = pos;
		value = sep + 1;

		if (!strcmp(key, AUTH_KEY))
		{
			if (auth)
				ereport(ERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("malformed OAUTHBEARER message"),
						 errdetail("Message contains multiple auth values.")));

			auth = value;
		}
		else
		{
			/*
			 * The RFC also defines the host and port keys, but they are not
			 * required for OAUTHBEARER and we do not use them. Also, per
			 * Sec. 3.1, any key/value pairs we don't recognize must be ignored.
			 */
		}

		/* Move to the next pair. */
		pos = end + 1;
	}

	ereport(ERROR,
			(errcode(ERRCODE_PROTOCOL_VIOLATION),
			 errmsg("malformed OAUTHBEARER message"),
			 errdetail("Message did not contain a final terminator.")));

	return NULL; /* unreachable */
}

static void
generate_error_response(struct oauth_ctx *ctx, char **output, int *outputlen)
{
	StringInfoData	buf;
	initStringInfo(&buf);

	/*
	 * TODO: JSON escaping
	 */
	appendStringInfo(&buf,
		"{ "
			"\"status\": \"invalid_token\", "
			"\"openid-configuration\": \"%s\","
			"\"scope\": \"%s\" "
		"}",
		ctx->discovery_uri, ctx->scope);

	*output = buf.data;
	*outputlen = buf.len;
}

static bool
run_oauth_provider(Port *port, const char *token)
{
	int result = oauth_provider->oauth_provider_hook(port, token);
	if(result == STATUS_OK)
	{
		set_authn_id(port, port->user_name);
		return true;
	}
	return false;
}

static bool
validate(Port *port, const char *auth, const char **logdetail)
{
	static const char * const b64_set = "abcdefghijklmnopqrstuvwxyz"
										"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
										"0123456789-._~+/";

	const char *token;
	size_t		span;
	int			ret;

	/* TODO: handle logdetail when the test framework can check it */

	/*
	 * Only Bearer tokens are accepted. The ABNF is defined in RFC 6750, Sec.
	 * 2.1:
	 *
	 *      b64token    = 1*( ALPHA / DIGIT /
	 *                        "-" / "." / "_" / "~" / "+" / "/" ) *"="
	 *      credentials = "Bearer" 1*SP b64token
	 *
	 * The "credentials" construction is what we receive in our auth value.
	 *
	 * Since that spec is subordinate to HTTP (i.e. the HTTP Authorization
	 * header format; RFC 7235 Sec. 2), the "Bearer" scheme string must be
	 * compared case-insensitively. (This is not mentioned in RFC 6750, but it's
	 * pointed out in RFC 7628 Sec. 4.)
	 *
	 * TODO: handle the Authorization spec, RFC 7235 Sec. 2.1.
	 */
	if (strncasecmp(auth, BEARER_SCHEME, strlen(BEARER_SCHEME)))
		return false;

	/* Pull the bearer token out of the auth value. */
	token = auth + strlen(BEARER_SCHEME);

	/* Swallow any additional spaces. */
	while (*token == ' ')
		token++;

	/*
	 * Before invoking the validator command, sanity-check the token format to
	 * avoid any injection attacks later in the chain. Invalid formats are
	 * technically a protocol violation, but don't reflect any information about
	 * the sensitive Bearer token back to the client; log at COMMERROR instead.
	 */

	/* Tokens must not be empty. */
	if (!*token)
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("malformed OAUTHBEARER message"),
				 errdetail("Bearer token is empty.")));
		return false;
	}

	/* Have the validator check the token. */
	if (!run_oauth_provider(port, token))
		return false;

	if (port->hba->oauth_skip_usermap)
	{
		/*
		 * If the validator is our authorization authority, we're done.
		 * Authentication may or may not have been performed depending on the
		 * validator implementation; all that matters is that the validator says
		 * the user can log in with the target role.
		 */
		return true;
	}

	/* Make sure the validator authenticated the user. */
	if (!MyClientConnectionInfo.authn_id)
	{
		/* TODO: use logdetail; reduce message duplication */
		ereport(LOG,
				(errmsg("OAuth bearer authentication failed for user \"%s\": validator provided no identity",
						port->user_name)));
		return false;
	}

	/* Finally, check the user map. */
	ret = check_usermap(port->hba->usermap, port->user_name,
						MyClientConnectionInfo.authn_id, false);
	return (ret == STATUS_OK);
}
