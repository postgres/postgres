/*-------------------------------------------------------------------------
 *
 * auth-oauth.c
 *	  Server-side implementation of the SASL OAUTHBEARER mechanism.
 *
 * See the following RFC for more details:
 * - RFC 7628: https://datatracker.ietf.org/doc/html/rfc7628
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
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
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "libpq/auth.h"
#include "libpq/hba.h"
#include "libpq/oauth.h"
#include "libpq/sasl.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "utils/json.h"
#include "utils/varlena.h"

/* GUC */
char	   *oauth_validator_libraries_string = NULL;

static void oauth_get_mechanisms(Port *port, StringInfo buf);
static void *oauth_init(Port *port, const char *selected_mech, const char *shadow_pass);
static int	oauth_exchange(void *opaq, const char *input, int inputlen,
						   char **output, int *outputlen, const char **logdetail);

static void load_validator_library(const char *libname);
static void shutdown_validator_library(void *arg);
static bool check_validator_hba_options(Port *port, const char **logdetail);

static ValidatorModuleState *validator_module_state;
static const OAuthValidatorCallbacks *ValidatorCallbacks;

static MemoryContext ValidatorMemoryContext;
static List *ValidatorOptions;
static bool ValidatorOptionsChecked;

/* Mechanism declaration */
const pg_be_sasl_mech pg_be_oauth_mech = {
	.get_mechanisms = oauth_get_mechanisms,
	.init = oauth_init,
	.exchange = oauth_exchange,

	.max_message_length = PG_MAX_AUTH_TOKEN_LENGTH,
};

/* Valid states for the oauth_exchange() machine. */
enum oauth_state
{
	OAUTH_STATE_INIT = 0,
	OAUTH_STATE_ERROR,
	OAUTH_STATE_ERROR_DISCOVERY,
	OAUTH_STATE_FINISHED,
};

/* Mechanism callback state. */
struct oauth_ctx
{
	enum oauth_state state;
	Port	   *port;
	const char *issuer;
	const char *scope;
};

static char *sanitize_char(char c);
static char *parse_kvpairs_for_auth(char **input);
static void generate_error_response(struct oauth_ctx *ctx, char **output, int *outputlen);
static bool validate(Port *port, const char *auth, const char **logdetail);

/* Constants seen in an OAUTHBEARER client initial response. */
#define KVSEP 0x01				/* separator byte for key/value pairs */
#define AUTH_KEY "auth"			/* key containing the Authorization header */
#define BEARER_SCHEME "Bearer " /* required header scheme (case-insensitive!) */

/*
 * Retrieves the OAUTHBEARER mechanism list (currently a single item).
 *
 * For a full description of the API, see libpq/sasl.h.
 */
static void
oauth_get_mechanisms(Port *port, StringInfo buf)
{
	/* Only OAUTHBEARER is supported. */
	appendStringInfoString(buf, OAUTHBEARER_NAME);
	appendStringInfoChar(buf, '\0');
}

/*
 * Initializes mechanism state and loads the configured validator module.
 *
 * For a full description of the API, see libpq/sasl.h.
 */
static void *
oauth_init(Port *port, const char *selected_mech, const char *shadow_pass)
{
	struct oauth_ctx *ctx;

	if (strcmp(selected_mech, OAUTHBEARER_NAME) != 0)
		ereport(ERROR,
				errcode(ERRCODE_PROTOCOL_VIOLATION),
				errmsg("client selected an invalid SASL authentication mechanism"));

	/* Save our memory context for later use by client API calls. */
	ValidatorMemoryContext = CurrentMemoryContext;

	ctx = palloc0_object(struct oauth_ctx);

	ctx->state = OAUTH_STATE_INIT;
	ctx->port = port;

	Assert(port->hba);
	ctx->issuer = port->hba->oauth_issuer;
	ctx->scope = port->hba->oauth_scope;

	load_validator_library(port->hba->oauth_validator);

	return ctx;
}

/*
 * Implements the OAUTHBEARER SASL exchange (RFC 7628, Sec. 3.2). This pulls
 * apart the client initial response and validates the Bearer token. It also
 * handles the dummy error response for a failed handshake, as described in
 * Sec. 3.2.3.
 *
 * For a full description of the API, see libpq/sasl.h.
 */
static int
oauth_exchange(void *opaq, const char *input, int inputlen,
			   char **output, int *outputlen, const char **logdetail)
{
	char	   *input_copy;
	char	   *p;
	char		cbind_flag;
	char	   *auth;
	int			status;

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
				errcode(ERRCODE_PROTOCOL_VIOLATION),
				errmsg("malformed OAUTHBEARER message"),
				errdetail("The message is empty."));
	if (inputlen != strlen(input))
		ereport(ERROR,
				errcode(ERRCODE_PROTOCOL_VIOLATION),
				errmsg("malformed OAUTHBEARER message"),
				errdetail("Message length does not match input length."));

	switch (ctx->state)
	{
		case OAUTH_STATE_INIT:
			/* Handle this case below. */
			break;

		case OAUTH_STATE_ERROR:
		case OAUTH_STATE_ERROR_DISCOVERY:

			/*
			 * Only one response is valid for the client during authentication
			 * failure: a single kvsep.
			 */
			if (inputlen != 1 || *input != KVSEP)
				ereport(ERROR,
						errcode(ERRCODE_PROTOCOL_VIOLATION),
						errmsg("malformed OAUTHBEARER message"),
						errdetail("Client did not send a kvsep response."));

			/*
			 * The (failed) handshake is now complete. Don't report discovery
			 * requests in the server log unless the log level is high enough.
			 */
			if (ctx->state == OAUTH_STATE_ERROR_DISCOVERY)
			{
				ereport(DEBUG1, errmsg("OAuth issuer discovery requested"));

				ctx->state = OAUTH_STATE_FINISHED;
				return PG_SASL_EXCHANGE_ABANDONED;
			}

			/* We're not in discovery, so this is just a normal auth failure. */
			ctx->state = OAUTH_STATE_FINISHED;
			return PG_SASL_EXCHANGE_FAILURE;

		default:
			elog(ERROR, "invalid OAUTHBEARER exchange state");
			return PG_SASL_EXCHANGE_FAILURE;
	}

	/* Handle the client's initial message. */
	p = input_copy = pstrdup(input);

	/*
	 * OAUTHBEARER does not currently define a channel binding (so there is no
	 * OAUTHBEARER-PLUS, and we do not accept a 'p' specifier). We accept a
	 * 'y' specifier purely for the remote chance that a future specification
	 * could define one; then future clients can still interoperate with this
	 * server implementation. 'n' is the expected case.
	 */
	cbind_flag = *p;
	switch (cbind_flag)
	{
		case 'p':
			ereport(ERROR,
					errcode(ERRCODE_PROTOCOL_VIOLATION),
					errmsg("malformed OAUTHBEARER message"),
					errdetail("The server does not support channel binding for OAuth, but the client message includes channel binding data."));
			break;

		case 'y':				/* fall through */
		case 'n':
			p++;
			if (*p != ',')
				ereport(ERROR,
						errcode(ERRCODE_PROTOCOL_VIOLATION),
						errmsg("malformed OAUTHBEARER message"),
						errdetail("Comma expected, but found character \"%s\".",
								  sanitize_char(*p)));
			p++;
			break;

		default:
			ereport(ERROR,
					errcode(ERRCODE_PROTOCOL_VIOLATION),
					errmsg("malformed OAUTHBEARER message"),
					errdetail("Unexpected channel-binding flag \"%s\".",
							  sanitize_char(cbind_flag)));
	}

	/*
	 * Forbid optional authzid (authorization identity).  We don't support it.
	 */
	if (*p == 'a')
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("client uses authorization identity, but it is not supported"));
	if (*p != ',')
		ereport(ERROR,
				errcode(ERRCODE_PROTOCOL_VIOLATION),
				errmsg("malformed OAUTHBEARER message"),
				errdetail("Unexpected attribute \"%s\" in client-first-message.",
						  sanitize_char(*p)));
	p++;

	/* All remaining fields are separated by the RFC's kvsep (\x01). */
	if (*p != KVSEP)
		ereport(ERROR,
				errcode(ERRCODE_PROTOCOL_VIOLATION),
				errmsg("malformed OAUTHBEARER message"),
				errdetail("Key-value separator expected, but found character \"%s\".",
						  sanitize_char(*p)));
	p++;

	auth = parse_kvpairs_for_auth(&p);
	if (!auth)
		ereport(ERROR,
				errcode(ERRCODE_PROTOCOL_VIOLATION),
				errmsg("malformed OAUTHBEARER message"),
				errdetail("Message does not contain an auth value."));

	/* We should be at the end of our message. */
	if (*p)
		ereport(ERROR,
				errcode(ERRCODE_PROTOCOL_VIOLATION),
				errmsg("malformed OAUTHBEARER message"),
				errdetail("Message contains additional data after the final terminator."));

	/*
	 * Make sure all custom HBA options are understood by the validator before
	 * continuing, since we couldn't check them during server start/reload.
	 */
	if (!check_validator_hba_options(ctx->port, logdetail))
	{
		ctx->state = OAUTH_STATE_FINISHED;
		return PG_SASL_EXCHANGE_FAILURE;
	}

	if (auth[0] == '\0')
	{
		/*
		 * An empty auth value represents a discovery request; the client
		 * expects it to fail.  Skip validation entirely and move directly to
		 * the error response.
		 */
		generate_error_response(ctx, output, outputlen);

		ctx->state = OAUTH_STATE_ERROR_DISCOVERY;
		status = PG_SASL_EXCHANGE_CONTINUE;
	}
	else if (!validate(ctx->port, auth, logdetail))
	{
		generate_error_response(ctx, output, outputlen);

		ctx->state = OAUTH_STATE_ERROR;
		status = PG_SASL_EXCHANGE_CONTINUE;
	}
	else
	{
		ctx->state = OAUTH_STATE_FINISHED;
		status = PG_SASL_EXCHANGE_SUCCESS;
	}

	/* Don't let extra copies of the bearer token hang around. */
	explicit_bzero(input_copy, inputlen);

	return status;
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
 * Performs syntactic validation of a key and value from the initial client
 * response. (Semantic validation of interesting values must be performed
 * later.)
 */
static void
validate_kvpair(const char *key, const char *val)
{
	/*-----
	 * From Sec 3.1:
	 *     key            = 1*(ALPHA)
	 */
	static const char *key_allowed_set =
		"abcdefghijklmnopqrstuvwxyz"
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ";

	size_t		span;

	if (!key[0])
		ereport(ERROR,
				errcode(ERRCODE_PROTOCOL_VIOLATION),
				errmsg("malformed OAUTHBEARER message"),
				errdetail("Message contains an empty key name."));

	span = strspn(key, key_allowed_set);
	if (key[span] != '\0')
		ereport(ERROR,
				errcode(ERRCODE_PROTOCOL_VIOLATION),
				errmsg("malformed OAUTHBEARER message"),
				errdetail("Message contains an invalid key name."));

	/*-----
	 * From Sec 3.1:
	 *     value          = *(VCHAR / SP / HTAB / CR / LF )
	 *
	 * The VCHAR (visible character) class is large; a loop is more
	 * straightforward than strspn().
	 */
	for (; *val; ++val)
	{
		if (0x21 <= *val && *val <= 0x7E)
			continue;			/* VCHAR */

		switch (*val)
		{
			case ' ':
			case '\t':
			case '\r':
			case '\n':
				continue;		/* SP, HTAB, CR, LF */

			default:
				ereport(ERROR,
						errcode(ERRCODE_PROTOCOL_VIOLATION),
						errmsg("malformed OAUTHBEARER message"),
						errdetail("Message contains an invalid value."));
		}
	}
}

/*
 * Consumes all kvpairs in an OAUTHBEARER exchange message. If the "auth" key is
 * found, its value is returned.
 */
static char *
parse_kvpairs_for_auth(char **input)
{
	char	   *pos = *input;
	char	   *auth = NULL;

	/*----
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
		char	   *end;
		char	   *sep;
		char	   *key;
		char	   *value;

		/*
		 * Find the end of this kvpair. Note that input is null-terminated by
		 * the SASL code, so the strchr() is bounded.
		 */
		end = strchr(pos, KVSEP);
		if (!end)
			ereport(ERROR,
					errcode(ERRCODE_PROTOCOL_VIOLATION),
					errmsg("malformed OAUTHBEARER message"),
					errdetail("Message contains an unterminated key/value pair."));
		*end = '\0';

		if (pos == end)
		{
			/* Empty kvpair, signifying the end of the list. */
			*input = pos + 1;
			return auth;
		}

		/*
		 * Find the end of the key name.
		 */
		sep = strchr(pos, '=');
		if (!sep)
			ereport(ERROR,
					errcode(ERRCODE_PROTOCOL_VIOLATION),
					errmsg("malformed OAUTHBEARER message"),
					errdetail("Message contains a key without a value."));
		*sep = '\0';

		/* Both key and value are now safely terminated. */
		key = pos;
		value = sep + 1;
		validate_kvpair(key, value);

		if (strcmp(key, AUTH_KEY) == 0)
		{
			if (auth)
				ereport(ERROR,
						errcode(ERRCODE_PROTOCOL_VIOLATION),
						errmsg("malformed OAUTHBEARER message"),
						errdetail("Message contains multiple auth values."));

			auth = value;
		}
		else
		{
			/*
			 * The RFC also defines the host and port keys, but they are not
			 * required for OAUTHBEARER and we do not use them. Also, per Sec.
			 * 3.1, any key/value pairs we don't recognize must be ignored.
			 */
		}

		/* Move to the next pair. */
		pos = end + 1;
	}

	ereport(ERROR,
			errcode(ERRCODE_PROTOCOL_VIOLATION),
			errmsg("malformed OAUTHBEARER message"),
			errdetail("Message did not contain a final terminator."));

	pg_unreachable();
	return NULL;
}

/*
 * Builds the JSON response for failed authentication (RFC 7628, Sec. 3.2.2).
 * This contains the required scopes for entry and a pointer to the OAuth/OpenID
 * discovery document, which the client may use to conduct its OAuth flow.
 */
static void
generate_error_response(struct oauth_ctx *ctx, char **output, int *outputlen)
{
	StringInfoData buf;
	StringInfoData issuer;

	/*
	 * The admin needs to set an issuer and scope for OAuth to work. There's
	 * not really a way to hide this from the user, either, because we can't
	 * choose a "default" issuer, so be honest in the failure message. (In
	 * practice such configurations are rejected during HBA parsing.)
	 */
	if (!ctx->issuer || !ctx->scope)
		ereport(FATAL,
				errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("OAuth is not properly configured for this user"),
				errdetail_log("The issuer and scope parameters must be set in pg_hba.conf."));

	/*
	 * Build a default .well-known URI based on our issuer, unless the HBA has
	 * already provided one.
	 */
	initStringInfo(&issuer);
	appendStringInfoString(&issuer, ctx->issuer);
	if (strstr(ctx->issuer, "/.well-known/") == NULL)
		appendStringInfoString(&issuer, "/.well-known/openid-configuration");

	initStringInfo(&buf);

	/*
	 * Escaping the string here is belt-and-suspenders defensive programming
	 * since escapable characters aren't valid in either the issuer URI or the
	 * scope list, but the HBA doesn't enforce that yet.
	 */
	appendStringInfoString(&buf, "{ \"status\": \"invalid_token\", ");

	appendStringInfoString(&buf, "\"openid-configuration\": ");
	escape_json(&buf, issuer.data);
	pfree(issuer.data);

	appendStringInfoString(&buf, ", \"scope\": ");
	escape_json(&buf, ctx->scope);

	appendStringInfoString(&buf, " }");

	*output = buf.data;
	*outputlen = buf.len;
}

/*-----
 * Validates the provided Authorization header and returns the token from
 * within it. NULL is returned on validation failure.
 *
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
 * header format; RFC 9110 Sec. 11), the "Bearer" scheme string must be
 * compared case-insensitively. (This is not mentioned in RFC 6750, but the
 * OAUTHBEARER spec points it out: RFC 7628 Sec. 4.)
 *
 * Invalid formats are technically a protocol violation, but we shouldn't
 * reflect any information about the sensitive Bearer token back to the
 * client; log at COMMERROR instead.
 */
static const char *
validate_token_format(const char *header)
{
	size_t		span;
	const char *token;
	static const char *const b64token_allowed_set =
		"abcdefghijklmnopqrstuvwxyz"
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
		"0123456789-._~+/";

	/* Missing auth headers should be handled by the caller. */
	Assert(header);
	/* Empty auth (discovery) should be handled before calling validate(). */
	Assert(header[0] != '\0');

	if (pg_strncasecmp(header, BEARER_SCHEME, strlen(BEARER_SCHEME)))
	{
		ereport(COMMERROR,
				errcode(ERRCODE_PROTOCOL_VIOLATION),
				errmsg("malformed OAuth bearer token"),
				errdetail_log("Client response indicated a non-Bearer authentication scheme."));
		return NULL;
	}

	/* Pull the bearer token out of the auth value. */
	token = header + strlen(BEARER_SCHEME);

	/* Swallow any additional spaces. */
	while (*token == ' ')
		token++;

	/* Tokens must not be empty. */
	if (!*token)
	{
		ereport(COMMERROR,
				errcode(ERRCODE_PROTOCOL_VIOLATION),
				errmsg("malformed OAuth bearer token"),
				errdetail_log("Bearer token is empty."));
		return NULL;
	}

	/*
	 * Make sure the token contains only allowed characters. Tokens may end
	 * with any number of '=' characters.
	 */
	span = strspn(token, b64token_allowed_set);
	while (token[span] == '=')
		span++;

	if (token[span] != '\0')
	{
		/*
		 * This error message could be more helpful by printing the
		 * problematic character(s), but that'd be a bit like printing a piece
		 * of someone's password into the logs.
		 */
		ereport(COMMERROR,
				errcode(ERRCODE_PROTOCOL_VIOLATION),
				errmsg("malformed OAuth bearer token"),
				errdetail_log("Bearer token is not in the correct format."));
		return NULL;
	}

	return token;
}

/*
 * Checks that the "auth" kvpair in the client response contains a syntactically
 * valid Bearer token, then passes it along to the loaded validator module for
 * authorization. Returns true if validation succeeds.
 */
static bool
validate(Port *port, const char *auth, const char **logdetail)
{
	int			map_status;
	ValidatorModuleResult *ret;
	const char *token;
	bool		status;

	/* Ensure that we have a correct token to validate */
	if (!(token = validate_token_format(auth)))
		return false;

	/*
	 * Ensure that we have a validation library loaded, this should always be
	 * the case and an error here is indicative of a bug.
	 */
	if (!ValidatorCallbacks || !ValidatorCallbacks->validate_cb)
		ereport(FATAL,
				errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("validation of OAuth token requested without a validator loaded"));

	/* Call the validation function from the validator module */
	ret = palloc0_object(ValidatorModuleResult);
	if (!ValidatorCallbacks->validate_cb(validator_module_state, token,
										 port->user_name, ret))
	{
		ereport(WARNING,
				errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("internal error in OAuth validator module"),
				ret->error_detail ? errdetail_log("%s", ret->error_detail) : 0);

		*logdetail = ret->error_detail;
		return false;
	}

	/*
	 * Log any authentication results even if the token isn't authorized; it
	 * might be useful for auditing or troubleshooting.
	 */
	if (ret->authn_id)
		set_authn_id(port, ret->authn_id);

	if (!ret->authorized)
	{
		if (ret->error_detail)
			*logdetail = ret->error_detail;
		else
			*logdetail = _("Validator failed to authorize the provided token.");

		status = false;
		goto cleanup;
	}

	if (port->hba->oauth_skip_usermap)
	{
		/*
		 * If the validator is our authorization authority, we're done.
		 * Authentication may or may not have been performed depending on the
		 * validator implementation; all that matters is that the validator
		 * says the user can log in with the target role.
		 */
		status = true;
		goto cleanup;
	}

	/* Make sure the validator authenticated the user. */
	if (ret->authn_id == NULL || ret->authn_id[0] == '\0')
	{
		*logdetail = _("Validator provided no identity.");

		status = false;
		goto cleanup;
	}

	/* Finally, check the user map. */
	map_status = check_usermap(port->hba->usermap, port->user_name,
							   MyClientConnectionInfo.authn_id, false);
	status = (map_status == STATUS_OK);

cleanup:

	/*
	 * Clear and free the validation result from the validator module once
	 * we're done with it.
	 */
	if (ret->authn_id != NULL)
		pfree(ret->authn_id);
	pfree(ret);

	return status;
}

/*
 * load_validator_library
 *
 * Load the configured validator library in order to perform token validation.
 * There is no built-in fallback since validation is implementation specific. If
 * no validator library is configured, or if it fails to load, then error out
 * since token validation won't be possible.
 */
static void
load_validator_library(const char *libname)
{
	OAuthValidatorModuleInit validator_init;
	MemoryContextCallback *mcb;

	/*
	 * The presence, and validity, of libname has already been established by
	 * check_oauth_validator so we don't need to perform more than Assert
	 * level checking here.
	 */
	Assert(libname && *libname);

	validator_init = (OAuthValidatorModuleInit)
		load_external_function(libname, "_PG_oauth_validator_module_init",
							   false, NULL);

	/*
	 * The validator init function is required since it will set the callbacks
	 * for the validator library.
	 */
	if (validator_init == NULL)
		ereport(ERROR,
				errmsg("%s module \"%s\" must define the symbol %s",
					   "OAuth validator", libname, "_PG_oauth_validator_module_init"));

	ValidatorCallbacks = (*validator_init) ();
	Assert(ValidatorCallbacks);

	/*
	 * Check the magic number, to protect against break-glass scenarios where
	 * the ABI must change within a major version. load_external_function()
	 * already checks for compatibility across major versions.
	 */
	if (ValidatorCallbacks->magic != PG_OAUTH_VALIDATOR_MAGIC)
		ereport(ERROR,
				errmsg("%s module \"%s\": magic number mismatch",
					   "OAuth validator", libname),
				errdetail("Server has magic number 0x%08X, module has 0x%08X.",
						  PG_OAUTH_VALIDATOR_MAGIC, ValidatorCallbacks->magic));

	/*
	 * Make sure all required callbacks are present in the ValidatorCallbacks
	 * structure. Right now only the validation callback is required.
	 */
	if (ValidatorCallbacks->validate_cb == NULL)
		ereport(ERROR,
				errmsg("%s module \"%s\" must provide a %s callback",
					   "OAuth validator", libname, "validate_cb"));

	/* Allocate memory for validator library private state data */
	validator_module_state = palloc0_object(ValidatorModuleState);
	validator_module_state->sversion = PG_VERSION_NUM;

	if (ValidatorCallbacks->startup_cb != NULL)
		ValidatorCallbacks->startup_cb(validator_module_state);

	/* Shut down the library before cleaning up its state. */
	mcb = palloc0_object(MemoryContextCallback);
	mcb->func = shutdown_validator_library;

	MemoryContextRegisterResetCallback(CurrentMemoryContext, mcb);
}

/*
 * Call the validator module's shutdown callback, if one is provided. This is
 * invoked during memory context reset.
 */
static void
shutdown_validator_library(void *arg)
{
	if (ValidatorCallbacks->shutdown_cb != NULL)
		ValidatorCallbacks->shutdown_cb(validator_module_state);

	/* The backing memory for this is about to disappear. */
	ValidatorOptions = NIL;
}

/*
 * Ensure an OAuth validator named in the HBA is permitted by the configuration.
 *
 * If the validator is currently unset and exactly one library is declared in
 * oauth_validator_libraries, then that library will be used as the validator.
 * Otherwise the name must be present in the list of oauth_validator_libraries.
 */
bool
check_oauth_validator(HbaLine *hbaline, int elevel, char **err_msg)
{
	int			line_num = hbaline->linenumber;
	const char *file_name = hbaline->sourcefile;
	char	   *rawstring;
	List	   *elemlist = NIL;

	*err_msg = NULL;

	if (oauth_validator_libraries_string[0] == '\0')
	{
		ereport(elevel,
				errcode(ERRCODE_CONFIG_FILE_ERROR),
				errmsg("oauth_validator_libraries must be set for authentication method %s",
					   "oauth"),
				errcontext("line %d of configuration file \"%s\"",
						   line_num, file_name));
		*err_msg = psprintf("oauth_validator_libraries must be set for authentication method %s",
							"oauth");
		return false;
	}

	/* SplitDirectoriesString needs a modifiable copy */
	rawstring = pstrdup(oauth_validator_libraries_string);

	if (!SplitDirectoriesString(rawstring, ',', &elemlist))
	{
		/* syntax error in list */
		ereport(elevel,
				errcode(ERRCODE_CONFIG_FILE_ERROR),
				errmsg("invalid list syntax in parameter \"%s\"",
					   "oauth_validator_libraries"));
		*err_msg = psprintf("invalid list syntax in parameter \"%s\"",
							"oauth_validator_libraries");
		goto done;
	}

	if (!hbaline->oauth_validator)
	{
		if (elemlist->length == 1)
		{
			hbaline->oauth_validator = pstrdup(linitial(elemlist));
			goto done;
		}

		ereport(elevel,
				errcode(ERRCODE_CONFIG_FILE_ERROR),
				errmsg("authentication method \"oauth\" requires argument \"validator\" to be set when oauth_validator_libraries contains multiple options"),
				errcontext("line %d of configuration file \"%s\"",
						   line_num, file_name));
		*err_msg = "authentication method \"oauth\" requires argument \"validator\" to be set when oauth_validator_libraries contains multiple options";
		goto done;
	}

	foreach_ptr(char, allowed, elemlist)
	{
		if (strcmp(allowed, hbaline->oauth_validator) == 0)
			goto done;
	}

	ereport(elevel,
			errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			errmsg("validator \"%s\" is not permitted by %s",
				   hbaline->oauth_validator, "oauth_validator_libraries"),
			errcontext("line %d of configuration file \"%s\"",
					   line_num, file_name));
	*err_msg = psprintf("validator \"%s\" is not permitted by %s",
						hbaline->oauth_validator, "oauth_validator_libraries");

done:
	list_free_deep(elemlist);
	pfree(rawstring);

	return (*err_msg == NULL);
}

/*
 * Client APIs for validator implementations
 *
 * Since we're currently not threaded, we only allow one validator in the
 * process at a time. So we can make use of globals for now instead of looking
 * up information using the state pointer. We probably shouldn't assume that the
 * module hasn't temporarily changed memory contexts on us, though; functions
 * here should defensively use an appropriate context when making global
 * allocations.
 */

/*
 * Adds to the list of allowed validator.* HBA options. Used during the
 * startup_cb.
 */
void
RegisterOAuthHBAOptions(ValidatorModuleState *state, int num,
						const char *opts[])
{
	MemoryContext oldcontext;

	if (!state)
	{
		Assert(false);
		return;
	}

	oldcontext = MemoryContextSwitchTo(ValidatorMemoryContext);

	for (int i = 0; i < num; i++)
	{
		if (!valid_oauth_hba_option_name(opts[i]))
		{
			/*
			 * The user can't set this option in the HBA, so GetOAuthHBAOption
			 * would always return NULL.
			 */
			ereport(WARNING,
					errmsg("HBA option name \"%s\" is invalid and will be ignored",
						   opts[i]),
			/* translator: the second %s is a function name */
					errcontext("validator module \"%s\", in call to %s",
							   MyProcPort->hba->oauth_validator,
							   "RegisterOAuthHBAOptions"));
			continue;
		}

		ValidatorOptions = lappend(ValidatorOptions, pstrdup(opts[i]));
	}

	MemoryContextSwitchTo(oldcontext);

	/*
	 * Wait to validate the HBA against the registered options until later
	 * (see check_validator_hba_options()).
	 *
	 * Delaying allows the validator to make multiple registration calls, to
	 * append to the list; it lets us make the check in a place where we can
	 * report the error without leaking details to the client; and it avoids
	 * exporting the order of operations between HBA matching and the
	 * startup_cb call as an API guarantee. (The last issue may become
	 * relevant with a threaded model.)
	 */
}

/*
 * Restrict the names available to custom HBA options, so that we don't
 * accidentally prevent future syntax extensions to HBA files.
 */
bool
valid_oauth_hba_option_name(const char *name)
{
	/*
	 * This list is not incredibly principled, since the goal is just to bound
	 * compatibility guarantees for our HBA parser. Alphanumerics seem
	 * obviously fine, and it's difficult to argue against the punctuation
	 * that's already included in some HBA option names and identifiers.
	 */
	static const char *name_allowed_set =
		"abcdefghijklmnopqrstuvwxyz"
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
		"0123456789_-";

	size_t		span;

	if (!name[0])
		return false;

	span = strspn(name, name_allowed_set);
	return name[span] == '\0';
}

/*
 * Verifies that all validator.* HBA options specified by the user were actually
 * registered by the validator library in use.
 */
static bool
check_validator_hba_options(Port *port, const char **logdetail)
{
	HbaLine    *hba = port->hba;

	foreach_ptr(char, key, hba->oauth_opt_keys)
	{
		bool		found = false;

		/* O(n^2) shouldn't be a problem here in practice. */
		foreach_ptr(char, optname, ValidatorOptions)
		{
			if (strcmp(key, optname) == 0)
			{
				found = true;
				break;
			}
		}

		if (!found)
		{
			/*
			 * Unknown option name. Mirror the error messages in hba.c here,
			 * keeping in mind that the original "validator." prefix was
			 * stripped from the key during parsing.
			 *
			 * Since this is affecting live connections, which is unusual for
			 * HBA, be noisy with a WARNING. (Warnings aren't sent to clients
			 * prior to successful authentication, so this won't disclose the
			 * server config.) It'll duplicate some of the information in the
			 * logdetail, but that should make it hard to miss the connection
			 * between the two.
			 */
			char	   *name = psprintf("validator.%s", key);

			*logdetail = psprintf(_("unrecognized authentication option name: \"%s\""),
								  name);
			ereport(WARNING,
					errcode(ERRCODE_CONFIG_FILE_ERROR),
					errmsg("unrecognized authentication option name: \"%s\"",
						   name),
			/* translator: the first %s is the name of the module */
					errdetail("The installed validator module (\"%s\") did not define an option named \"%s\".",
							  hba->oauth_validator, key),
					errhint("All OAuth connections matching this line will fail. Correct the option and reload the server configuration."),
					errcontext("line %d of configuration file \"%s\"",
							   hba->linenumber, hba->sourcefile));

			return false;
		}
	}

	ValidatorOptionsChecked = true; /* unfetter GetOAuthHBAOption() */
	return true;
}

/*
 * Retrieves the setting for a validator.* HBA option, or NULL if not found.
 * This may only be used during the validate_cb and shutdown_cb.
 */
const char *
GetOAuthHBAOption(const ValidatorModuleState *state, const char *optname)
{
	HbaLine    *hba = MyProcPort->hba;
	ListCell   *lc_k;
	ListCell   *lc_v;
	const char *ret = NULL;

	if (!ValidatorOptionsChecked)
	{
		/*
		 * Prevent the startup_cb from retrieving HBA options that it has just
		 * registered. This probably seems strange -- why refuse to hand out
		 * information we already know? -- but this lets us reserve the
		 * ability to perform the startup_cb call earlier, before we know
		 * which HBA line is matched by a connection, without breaking this
		 * API.
		 */
		return NULL;
	}

	if (!state || !hba)
	{
		Assert(false);
		return NULL;
	}

	Assert(list_length(hba->oauth_opt_keys) == list_length(hba->oauth_opt_vals));

	forboth(lc_k, hba->oauth_opt_keys, lc_v, hba->oauth_opt_vals)
	{
		const char *key = lfirst(lc_k);
		const char *val = lfirst(lc_v);

		if (strcmp(key, optname) == 0)
		{
			/*
			 * Don't return yet -- when regular HBA options are specified more
			 * than once, the last one wins. Do the same for these options.
			 */
			ret = val;
		}
	}

	return ret;
}
