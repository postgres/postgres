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

/* GUC */
char *oauth_validator_command;

static void  oauth_get_mechanisms(Port *port, StringInfo buf);
static void *oauth_init(Port *port, const char *selected_mech, const char *shadow_pass);
static int   oauth_exchange(void *opaq, const char *input, int inputlen,
							char **output, int *outputlen, char **logdetail);

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
	const char *issuer;
	const char *scope;
};

static char *sanitize_char(char c);
static char *parse_kvpairs_for_auth(char **input);
static void generate_error_response(struct oauth_ctx *ctx, char **output, int *outputlen);
static bool validate(Port *port, const char *auth, char **logdetail);
static bool run_validator_command(Port *port, const char *token);
static bool check_exit(FILE **fh, const char *command);
static bool unset_cloexec(int fd);
static bool username_ok_for_shell(const char *username);

#define KVSEP 0x01
#define AUTH_KEY "auth"
#define BEARER_SCHEME "Bearer "

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

	if (strcmp(selected_mech, OAUTHBEARER_NAME))
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("client selected an invalid SASL authentication mechanism")));

	ctx = palloc0(sizeof(*ctx));

	ctx->state = OAUTH_STATE_INIT;
	ctx->port = port;

	Assert(port->hba);
	ctx->issuer = port->hba->oauth_issuer;
	ctx->scope = port->hba->oauth_scope;

	return ctx;
}

static int
oauth_exchange(void *opaq, const char *input, int inputlen,
			   char **output, int *outputlen, char **logdetail)
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

	/*
	 * The admin needs to set an issuer and scope for OAuth to work. There's not
	 * really a way to hide this from the user, either, because we can't choose
	 * a "default" issuer, so be honest in the failure message.
	 *
	 * TODO: see if there's a better place to fail, earlier than this.
	 */
	if (!ctx->issuer || !ctx->scope)
		ereport(FATAL,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("OAuth is not properly configured for this user"),
				 errdetail_log("The issuer and scope parameters must be set in pg_hba.conf.")));


	initStringInfo(&buf);

	/*
	 * TODO: JSON escaping
	 */
	appendStringInfo(&buf,
		"{ "
			"\"status\": \"invalid_token\", "
			"\"openid-configuration\": \"%s/.well-known/openid-configuration\","
			"\"scope\": \"%s\" "
		"}",
		ctx->issuer, ctx->scope);

	*output = buf.data;
	*outputlen = buf.len;
}

static bool
validate(Port *port, const char *auth, char **logdetail)
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

	/*
	 * Make sure the token contains only allowed characters. Tokens may end with
	 * any number of '=' characters.
	 */
	span = strspn(token, b64_set);
	while (token[span] == '=')
		span++;

	if (token[span] != '\0')
	{
		/*
		 * This error message could be more helpful by printing the problematic
		 * character(s), but that'd be a bit like printing a piece of someone's
		 * password into the logs.
		 */
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("malformed OAUTHBEARER message"),
				 errdetail("Bearer token is not in the correct format.")));
		return false;
	}

	/* Have the validator check the token. */
	if (!run_validator_command(port, token))
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
	ret = check_usermap(port->hba->usermap, port->user_name, MyClientConnectionInfo.authn_id,
						false);
	return (ret == STATUS_OK);
}

static bool
run_validator_command(Port *port, const char *token)
{
	bool		success = false;
	int			rc;
	int			pipefd[2];
	int			rfd = -1;
	int			wfd = -1;

	StringInfoData command = { 0 };
	char	   *p;
	FILE	   *fh = NULL;

	ssize_t		written;
	char	   *line = NULL;
	size_t		size = 0;
	ssize_t		len;

	Assert(oauth_validator_command);

	if (!oauth_validator_command[0])
	{
		ereport(COMMERROR,
				(errmsg("oauth_validator_command is not set"),
				 errhint("To allow OAuth authenticated connections, set "
						 "oauth_validator_command in postgresql.conf.")));
		return false;
	}

	/*
	 * Since popen() is unidirectional, open up a pipe for the other direction.
	 * Use CLOEXEC to ensure that our write end doesn't accidentally get copied
	 * into child processes, which would prevent us from closing it cleanly.
	 *
	 * XXX this is ugly. We should just read from the child process's stdout,
	 * but that's a lot more code.
	 * XXX by bypassing the popen API, we open the potential of process
	 * deadlock. Clearly document child process requirements (i.e. the child
	 * MUST read all data off of the pipe before writing anything).
	 * TODO: port to Windows using _pipe().
	 */
	rc = pipe2(pipefd, O_CLOEXEC);
	if (rc < 0)
	{
		ereport(COMMERROR,
				(errcode_for_file_access(),
				 errmsg("could not create child pipe: %m")));
		return false;
	}

	rfd = pipefd[0];
	wfd = pipefd[1];

	/* Allow the read pipe be passed to the child. */
	if (!unset_cloexec(rfd))
	{
		/* error message was already logged */
		goto cleanup;
	}

	/*
	 * Construct the command, substituting any recognized %-specifiers:
	 *
	 *   %f: the file descriptor of the input pipe
	 *   %r: the role that the client wants to assume (port->user_name)
	 *   %%: a literal '%'
	 */
	initStringInfo(&command);

	for (p = oauth_validator_command; *p; p++)
	{
		if (p[0] == '%')
		{
			switch (p[1])
			{
				case 'f':
					appendStringInfo(&command, "%d", rfd);
					p++;
					break;
				case 'r':
					/*
					 * TODO: decide how this string should be escaped. The role
					 * is controlled by the client, so if we don't escape it,
					 * command injections are inevitable.
					 *
					 * This is probably an indication that the role name needs
					 * to be communicated to the validator process in some other
					 * way. For this proof of concept, just be incredibly strict
					 * about the characters that are allowed in user names.
					 */
					if (!username_ok_for_shell(port->user_name))
						goto cleanup;

					appendStringInfoString(&command, port->user_name);
					p++;
					break;
				case '%':
					appendStringInfoChar(&command, '%');
					p++;
					break;
				default:
					appendStringInfoChar(&command, p[0]);
			}
		}
		else
			appendStringInfoChar(&command, p[0]);
	}

	/* Execute the command. */
	fh = OpenPipeStream(command.data, "re");
	/* TODO: handle failures */

	/* We don't need the read end of the pipe anymore. */
	close(rfd);
	rfd = -1;

	/* Give the command the token to validate. */
	written = write(wfd, token, strlen(token));
	if (written != strlen(token))
	{
		/* TODO must loop for short writes, EINTR et al */
		ereport(COMMERROR,
				(errcode_for_file_access(),
				 errmsg("could not write token to child pipe: %m")));
		goto cleanup;
	}

	close(wfd);
	wfd = -1;

	/*
	 * Read the command's response.
	 *
	 * TODO: getline() is probably too new to use, unfortunately.
	 * TODO: loop over all lines
	 */
	if ((len = getline(&line, &size, fh)) >= 0)
	{
		/* TODO: fail if the authn_id doesn't end with a newline */
		if (len > 0)
			line[len - 1] = '\0';

		set_authn_id(port, line);
	}
	else if (ferror(fh))
	{
		ereport(COMMERROR,
				(errcode_for_file_access(),
				 errmsg("could not read from command \"%s\": %m",
						command.data)));
		goto cleanup;
	}

	/* Make sure the command exits cleanly. */
	if (!check_exit(&fh, command.data))
	{
		/* error message already logged */
		goto cleanup;
	}

	/* Done. */
	success = true;

cleanup:
	if (line)
		free(line);

	/*
	 * In the successful case, the pipe fds are already closed. For the error
	 * case, always close out the pipe before waiting for the command, to
	 * prevent deadlock.
	 */
	if (rfd >= 0)
		close(rfd);
	if (wfd >= 0)
		close(wfd);

	if (fh)
	{
		Assert(!success);
		check_exit(&fh, command.data);
	}

	if (command.data)
		pfree(command.data);

	return success;
}

static bool
check_exit(FILE **fh, const char *command)
{
	int rc;

	rc = ClosePipeStream(*fh);
	*fh = NULL;

	if (rc == -1)
	{
		/* pclose() itself failed. */
		ereport(COMMERROR,
				(errcode_for_file_access(),
				 errmsg("could not close pipe to command \"%s\": %m",
						command)));
	}
	else if (rc != 0)
	{
		char *reason = wait_result_to_str(rc);

		ereport(COMMERROR,
				(errmsg("failed to execute command \"%s\": %s",
						command, reason)));

		pfree(reason);
	}

	return (rc == 0);
}

static bool
unset_cloexec(int fd)
{
	int			flags;
	int			rc;

	flags = fcntl(fd, F_GETFD);
	if (flags == -1)
	{
		ereport(COMMERROR,
				(errcode_for_file_access(),
				 errmsg("could not get fd flags for child pipe: %m")));
		return false;
	}

	rc = fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC);
	if (rc < 0)
	{
		ereport(COMMERROR,
				(errcode_for_file_access(),
				 errmsg("could not unset FD_CLOEXEC for child pipe: %m")));
		return false;
	}

	return true;
}

/*
 * XXX This should go away eventually and be replaced with either a proper
 * escape or a different strategy for communication with the validator command.
 */
static bool
username_ok_for_shell(const char *username)
{
	/* This set is borrowed from fe_utils' appendShellStringNoError(). */
	static const char * const allowed = "abcdefghijklmnopqrstuvwxyz"
										"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
										"0123456789-_./:";
	size_t	span;

	Assert(username && username[0]); /* should have already been checked */

	span = strspn(username, allowed);
	if (username[span] != '\0')
	{
		ereport(COMMERROR,
				(errmsg("PostgreSQL user name contains unsafe characters and cannot be passed to the OAuth validator")));
		return false;
	}

	return true;
}
