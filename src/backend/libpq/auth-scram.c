/*-------------------------------------------------------------------------
 *
 * auth-scram.c
 *	  Server-side implementation of the SASL SCRAM-SHA-256 mechanism.
 *
 * See the following RFCs for more details:
 * - RFC 5802: https://tools.ietf.org/html/rfc5802
 * - RFC 7677: https://tools.ietf.org/html/rfc7677
 *
 * Here are some differences:
 *
 * - Username from the authentication exchange is not used. The client
 *	 should send an empty string as the username.
 * - Password is not processed with the SASLprep algorithm.
 * - Channel binding is not supported yet.
 *
 * The password stored in pg_authid consists of the salt, iteration count,
 * StoredKey and ServerKey.
 *
 * On error handling:
 *
 * Don't reveal user information to an unauthenticated client.  We don't
 * want an attacker to be able to probe whether a particular username is
 * valid.  In SCRAM, the server has to read the salt and iteration count
 * from the user's password verifier, and send it to the client.  To avoid
 * revealing whether a user exists, when the client tries to authenticate
 * with a username that doesn't exist, or doesn't have a valid SCRAM
 * verifier in pg_authid, we create a fake salt and iteration count
 * on-the-fly, and proceed with the authentication with that.  In the end,
 * we'll reject the attempt, as if an incorrect password was given.  When
 * we are performing a "mock" authentication, the 'doomed' flag in
 * scram_state is set.
 *
 * In the error messages, avoid printing strings from the client, unless
 * you check that they are pure ASCII.  We don't want an unauthenticated
 * attacker to be able to spam the logs with characters that are not valid
 * to the encoding being used, whatever that is.  We cannot avoid that in
 * general, after logging in, but let's do what we can here.
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/libpq/auth-scram.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/xlog.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_control.h"
#include "common/base64.h"
#include "common/scram-common.h"
#include "common/sha2.h"
#include "libpq/auth.h"
#include "libpq/crypt.h"
#include "libpq/scram.h"
#include "miscadmin.h"
#include "utils/backend_random.h"
#include "utils/builtins.h"
#include "utils/timestamp.h"

/*
 * Status data for a SCRAM authentication exchange.  This should be kept
 * internal to this file.
 */
typedef enum
{
	SCRAM_AUTH_INIT,
	SCRAM_AUTH_SALT_SENT,
	SCRAM_AUTH_FINISHED
} scram_state_enum;

typedef struct
{
	scram_state_enum state;

	const char *username;		/* username from startup packet */

	char	   *salt;			/* base64-encoded */
	int			iterations;
	uint8		StoredKey[SCRAM_KEY_LEN];
	uint8		ServerKey[SCRAM_KEY_LEN];

	/* Fields of the first message from client */
	char	   *client_first_message_bare;
	char	   *client_username;
	char	   *client_nonce;

	/* Fields from the last message from client */
	char	   *client_final_message_without_proof;
	char	   *client_final_nonce;
	char		ClientProof[SCRAM_KEY_LEN];

	/* Fields generated in the server */
	char	   *server_first_message;
	char	   *server_nonce;

	/*
	 * If something goes wrong during the authentication, or we are performing
	 * a "mock" authentication (see comments at top of file), the 'doomed'
	 * flag is set.  A reason for the failure, for the server log, is put in
	 * 'logdetail'.
	 */
	bool		doomed;
	char	   *logdetail;
} scram_state;

static void read_client_first_message(scram_state *state, char *input);
static void read_client_final_message(scram_state *state, char *input);
static char *build_server_first_message(scram_state *state);
static char *build_server_final_message(scram_state *state);
static bool verify_client_proof(scram_state *state);
static bool verify_final_nonce(scram_state *state);
static bool parse_scram_verifier(const char *verifier, char **salt,
					 int *iterations, uint8 *stored_key, uint8 *server_key);
static void mock_scram_verifier(const char *username, char **salt, int *iterations,
					uint8 *stored_key, uint8 *server_key);
static bool is_scram_printable(char *p);
static char *sanitize_char(char c);
static char *scram_MockSalt(const char *username);

/*
 * pg_be_scram_init
 *
 * Initialize a new SCRAM authentication exchange status tracker.  This
 * needs to be called before doing any exchange.  It will be filled later
 * after the beginning of the exchange with verifier data.
 *
 * 'username' is the provided by the client.  'shadow_pass' is the role's
 * password verifier, from pg_authid.rolpassword.  If 'doomed' is true, the
 * authentication must fail, as if an incorrect password was given.
 * 'shadow_pass' may be NULL, when 'doomed' is set.
 */
void *
pg_be_scram_init(const char *username, const char *shadow_pass, bool doomed)
{
	scram_state *state;
	int			password_type;

	state = (scram_state *) palloc0(sizeof(scram_state));
	state->state = SCRAM_AUTH_INIT;
	state->username = username;

	/*
	 * Perform sanity checks on the provided password after catalog lookup.
	 * The authentication is bound to fail if the lookup itself failed or if
	 * the password stored is MD5-encrypted.  Authentication is possible for
	 * users with a valid plain password though.
	 */

	if (shadow_pass == NULL || doomed)
		password_type = -1;
	else
		password_type = get_password_type(shadow_pass);

	if (password_type == PASSWORD_TYPE_SCRAM)
	{
		if (!parse_scram_verifier(shadow_pass, &state->salt, &state->iterations,
								  state->StoredKey, state->ServerKey))
		{
			/*
			 * The password looked like a SCRAM verifier, but could not be
			 * parsed.
			 */
			elog(LOG, "invalid SCRAM verifier for user \"%s\"", username);
			doomed = true;
		}
	}
	else if (password_type == PASSWORD_TYPE_PLAINTEXT)
	{
		char	   *verifier;

		/*
		 * The password provided is in plain format, in which case a fresh
		 * SCRAM verifier can be generated and used for the rest of the
		 * processing.
		 */
		verifier = scram_build_verifier(username, shadow_pass, 0);

		(void) parse_scram_verifier(verifier, &state->salt, &state->iterations,
									state->StoredKey, state->ServerKey);
		pfree(verifier);
	}
	else
		doomed = true;

	if (doomed)
	{
		/*
		 * We don't have a valid SCRAM verifier, nor could we generate one, or
		 * the caller requested us to perform a dummy authentication.
		 *
		 * The authentication is bound to fail, but to avoid revealing
		 * information to the attacker, go through the motions with a fake
		 * SCRAM verifier, and fail as if the password was incorrect.
		 */
		state->logdetail = psprintf(_("User \"%s\" does not have a valid SCRAM verifier."),
									state->username);
		mock_scram_verifier(username, &state->salt, &state->iterations,
							state->StoredKey, state->ServerKey);
	}
	state->doomed = doomed;

	return state;
}

/*
 * Continue a SCRAM authentication exchange.
 *
 * The next message to send to client is saved in "output", for a length
 * of "outputlen".  In the case of an error, optionally store a palloc'd
 * string at *logdetail that will be sent to the postmaster log (but not
 * the client).
 */
int
pg_be_scram_exchange(void *opaq, char *input, int inputlen,
					 char **output, int *outputlen, char **logdetail)
{
	scram_state *state = (scram_state *) opaq;
	int			result;

	*output = NULL;

	/*
	 * Check that the input length agrees with the string length of the input.
	 * We can ignore inputlen after this.
	 */
	if (inputlen == 0)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 (errmsg("malformed SCRAM message (empty message)"))));
	if (inputlen != strlen(input))
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 (errmsg("malformed SCRAM message (length mismatch)"))));

	switch (state->state)
	{
		case SCRAM_AUTH_INIT:

			/*
			 * Initialization phase.  Receive the first message from client
			 * and be sure that it parsed correctly.  Then send the challenge
			 * to the client.
			 */
			read_client_first_message(state, input);

			/* prepare message to send challenge */
			*output = build_server_first_message(state);

			state->state = SCRAM_AUTH_SALT_SENT;
			result = SASL_EXCHANGE_CONTINUE;
			break;

		case SCRAM_AUTH_SALT_SENT:

			/*
			 * Final phase for the server.  Receive the response to the
			 * challenge previously sent, verify, and let the client know that
			 * everything went well (or not).
			 */
			read_client_final_message(state, input);

			if (!verify_final_nonce(state))
				ereport(ERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
					   (errmsg("invalid SCRAM response (nonce mismatch)"))));

			/*
			 * Now check the final nonce and the client proof.
			 *
			 * If we performed a "mock" authentication that we knew would fail
			 * from the get go, this is where we fail.
			 *
			 * NB: the order of these checks is intentional.  We calculate the
			 * client proof even in a mock authentication, even though it's
			 * bound to fail, to thwart timing attacks to determine if a role
			 * with the given name exists or not.
			 */
			if (!verify_client_proof(state) || state->doomed)
			{
				/*
				 * Signal invalid-proof, although the real reason might also
				 * be e.g. that the password has expired, or the user doesn't
				 * exist.  "e=other-error" might be more correct, but
				 * "e=invalid-proof" is more likely to give a nice error
				 * message to the user.
				 */
				*output = psprintf("e=invalid-proof");
				result = SASL_EXCHANGE_FAILURE;
				break;
			}

			/* Build final message for client */
			*output = build_server_final_message(state);

			/* Success! */
			result = SASL_EXCHANGE_SUCCESS;
			state->state = SCRAM_AUTH_FINISHED;
			break;

		default:
			elog(ERROR, "invalid SCRAM exchange state");
			result = SASL_EXCHANGE_FAILURE;
	}

	if (result == SASL_EXCHANGE_FAILURE && state->logdetail && logdetail)
		*logdetail = state->logdetail;

	if (*output)
		*outputlen = strlen(*output);

	return result;
}

/*
 * Construct a verifier string for SCRAM, stored in pg_authid.rolpassword.
 *
 * If iterations is 0, default number of iterations is used.  The result is
 * palloc'd, so caller is responsible for freeing it.
 */
char *
scram_build_verifier(const char *username, const char *password,
					 int iterations)
{
	uint8		keybuf[SCRAM_KEY_LEN + 1];
	char		storedkey_hex[SCRAM_KEY_LEN * 2 + 1];
	char		serverkey_hex[SCRAM_KEY_LEN * 2 + 1];
	char		salt[SCRAM_SALT_LEN];
	char	   *encoded_salt;
	int			encoded_len;

	if (iterations <= 0)
		iterations = SCRAM_ITERATIONS_DEFAULT;

	if (!pg_backend_random(salt, SCRAM_SALT_LEN))
	{
		ereport(LOG,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not generate random salt")));
		return NULL;
	}

	encoded_salt = palloc(pg_b64_enc_len(SCRAM_SALT_LEN) + 1);
	encoded_len = pg_b64_encode(salt, SCRAM_SALT_LEN, encoded_salt);
	encoded_salt[encoded_len] = '\0';

	/* Calculate StoredKey, and encode it in hex */
	scram_ClientOrServerKey(password, salt, SCRAM_SALT_LEN,
							iterations, SCRAM_CLIENT_KEY_NAME, keybuf);
	scram_H(keybuf, SCRAM_KEY_LEN, keybuf);		/* StoredKey */
	(void) hex_encode((const char *) keybuf, SCRAM_KEY_LEN, storedkey_hex);
	storedkey_hex[SCRAM_KEY_LEN * 2] = '\0';

	/* And same for ServerKey */
	scram_ClientOrServerKey(password, salt, SCRAM_SALT_LEN, iterations,
							SCRAM_SERVER_KEY_NAME, keybuf);
	(void) hex_encode((const char *) keybuf, SCRAM_KEY_LEN, serverkey_hex);
	serverkey_hex[SCRAM_KEY_LEN * 2] = '\0';

	return psprintf("scram-sha-256:%s:%d:%s:%s", encoded_salt, iterations, storedkey_hex, serverkey_hex);
}

/*
 * Verify a plaintext password against a SCRAM verifier.  This is used when
 * performing plaintext password authentication for a user that has a SCRAM
 * verifier stored in pg_authid.
 */
bool
scram_verify_plain_password(const char *username, const char *password,
							const char *verifier)
{
	char	   *encoded_salt;
	char	   *salt;
	int			saltlen;
	int			iterations;
	uint8		stored_key[SCRAM_KEY_LEN];
	uint8		server_key[SCRAM_KEY_LEN];
	uint8		computed_key[SCRAM_KEY_LEN];

	if (!parse_scram_verifier(verifier, &encoded_salt, &iterations,
							  stored_key, server_key))
	{
		/*
		 * The password looked like a SCRAM verifier, but could not be
		 * parsed.
		 */
		elog(LOG, "invalid SCRAM verifier for user \"%s\"", username);
		return false;
	}

	salt = palloc(pg_b64_dec_len(strlen(encoded_salt)));
	saltlen = pg_b64_decode(encoded_salt, strlen(encoded_salt), salt);
	if (saltlen == -1)
	{
		elog(LOG, "invalid SCRAM verifier for user \"%s\"", username);
		return false;
	}

	/* Compute Server key based on the user-supplied plaintext password */
	scram_ClientOrServerKey(password, salt, saltlen, iterations,
							SCRAM_SERVER_KEY_NAME, computed_key);

	/*
	 * Compare the verifier's Server Key with the one computed from the
	 * user-supplied password.
	 */
	return memcmp(computed_key, server_key, SCRAM_KEY_LEN) == 0;
}

/*
 * Check if given verifier can be used for SCRAM authentication.
 *
 * Returns true if it is a SCRAM verifier, and false otherwise.
 */
bool
is_scram_verifier(const char *verifier)
{
	char	   *salt = NULL;
	int			iterations;
	uint8		stored_key[SCRAM_KEY_LEN];
	uint8		server_key[SCRAM_KEY_LEN];
	bool		result;

	result = parse_scram_verifier(verifier, &salt, &iterations, stored_key, server_key);
	if (salt)
		pfree(salt);

	return result;
}


/*
 * Parse and validate format of given SCRAM verifier.
 *
 * Returns true if the SCRAM verifier has been parsed, and false otherwise.
 */
static bool
parse_scram_verifier(const char *verifier, char **salt, int *iterations,
					 uint8 *stored_key, uint8 *server_key)
{
	char	   *v;
	char	   *p;

	/*
	 * The verifier is of form:
	 *
	 * scram-sha-256:<salt>:<iterations>:<storedkey>:<serverkey>
	 */
	if (strncmp(verifier, "scram-sha-256:", strlen("scram-sha-256:")) != 0)
		return false;

	v = pstrdup(verifier + strlen("scram-sha-256:"));

	/* salt */
	if ((p = strtok(v, ":")) == NULL)
		goto invalid_verifier;
	*salt = pstrdup(p);

	/* iterations */
	if ((p = strtok(NULL, ":")) == NULL)
		goto invalid_verifier;
	errno = 0;
	*iterations = strtol(p, &p, SCRAM_ITERATION_LEN);
	if (*p || errno != 0)
		goto invalid_verifier;

	/* storedkey */
	if ((p = strtok(NULL, ":")) == NULL)
		goto invalid_verifier;
	if (strlen(p) != SCRAM_KEY_LEN * 2)
		goto invalid_verifier;

	hex_decode(p, SCRAM_KEY_LEN * 2, (char *) stored_key);

	/* serverkey */
	if ((p = strtok(NULL, ":")) == NULL)
		goto invalid_verifier;
	if (strlen(p) != SCRAM_KEY_LEN * 2)
		goto invalid_verifier;
	hex_decode(p, SCRAM_KEY_LEN * 2, (char *) server_key);

	pfree(v);
	return true;

invalid_verifier:
	pfree(v);
	return false;
}

static void
mock_scram_verifier(const char *username, char **salt, int *iterations,
					uint8 *stored_key, uint8 *server_key)
{
	char	   *raw_salt;
	char	   *encoded_salt;
	int			encoded_len;

	/* Generate deterministic salt */
	raw_salt = scram_MockSalt(username);

	encoded_salt = (char *) palloc(pg_b64_enc_len(SCRAM_SALT_LEN) + 1);
	encoded_len = pg_b64_encode(raw_salt, SCRAM_SALT_LEN, encoded_salt);
	encoded_salt[encoded_len] = '\0';

	*salt = encoded_salt;
	*iterations = SCRAM_ITERATIONS_DEFAULT;

	/* StoredKey and ServerKey are not used in a doomed authentication */
	memset(stored_key, 0, SCRAM_KEY_LEN);
	memset(server_key, 0, SCRAM_KEY_LEN);
}

/*
 * Read the value in a given SASL exchange message for given attribute.
 */
static char *
read_attr_value(char **input, char attr)
{
	char	   *begin = *input;
	char	   *end;

	if (*begin != attr)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
		(errmsg("malformed SCRAM message (attribute '%c' expected, %s found)",
				attr, sanitize_char(*begin)))));
	begin++;

	if (*begin != '=')
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
		 (errmsg("malformed SCRAM message (expected = in attr %c)", attr))));
	begin++;

	end = begin;
	while (*end && *end != ',')
		end++;

	if (*end)
	{
		*end = '\0';
		*input = end + 1;
	}
	else
		*input = end;

	return begin;
}

static bool
is_scram_printable(char *p)
{
	/*------
	 * Printable characters, as defined by SCRAM spec: (RFC 5802)
	 *
	 *	printable		= %x21-2B / %x2D-7E
	 *					  ;; Printable ASCII except ",".
	 *					  ;; Note that any "printable" is also
	 *					  ;; a valid "value".
	 *------
	 */
	for (; *p; p++)
	{
		if (*p < 0x21 || *p > 0x7E || *p == 0x2C /* comma */ )
			return false;
	}
	return true;
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
		snprintf(buf, sizeof(buf), "0x%02x", c);
	return buf;
}

/*
 * Read the next attribute and value in a SASL exchange message.
 *
 * Returns NULL if there is attribute.
 */
static char *
read_any_attr(char **input, char *attr_p)
{
	char	   *begin = *input;
	char	   *end;
	char		attr = *begin;

	/*------
	 * attr-val		   = ALPHA "=" value
	 *					 ;; Generic syntax of any attribute sent
	 *					 ;; by server or client
	 *------
	 */
	if (!((attr >= 'A' && attr <= 'Z') ||
		  (attr >= 'a' && attr <= 'z')))
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 (errmsg("malformed SCRAM message (attribute expected, invalid char %s found)",
						 sanitize_char(attr)))));
	if (attr_p)
		*attr_p = attr;
	begin++;

	if (*begin != '=')
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
		 (errmsg("malformed SCRAM message (expected = in attr %c)", attr))));
	begin++;

	end = begin;
	while (*end && *end != ',')
		end++;

	if (*end)
	{
		*end = '\0';
		*input = end + 1;
	}
	else
		*input = end;

	return begin;
}

/*
 * Read and parse the first message from client in the context of a SASL
 * authentication exchange message.
 *
 * At this stage, any errors will be reported directly with ereport(ERROR).
 */
static void
read_client_first_message(scram_state *state, char *input)
{
	input = pstrdup(input);

	/*------
	 * The syntax for the client-first-message is: (RFC 5802)
	 *
	 * saslname		   = 1*(value-safe-char / "=2C" / "=3D")
	 *					 ;; Conforms to <value>.
	 *
	 * authzid		   = "a=" saslname
	 *					 ;; Protocol specific.
	 *
	 * cb-name		   = 1*(ALPHA / DIGIT / "." / "-")
	 *					  ;; See RFC 5056, Section 7.
	 *					  ;; E.g., "tls-server-end-point" or
	 *					  ;; "tls-unique".
	 *
	 * gs2-cbind-flag  = ("p=" cb-name) / "n" / "y"
	 *					 ;; "n" -> client doesn't support channel binding.
	 *					 ;; "y" -> client does support channel binding
	 *					 ;;		   but thinks the server does not.
	 *					 ;; "p" -> client requires channel binding.
	 *					 ;; The selected channel binding follows "p=".
	 *
	 * gs2-header	   = gs2-cbind-flag "," [ authzid ] ","
	 *					 ;; GS2 header for SCRAM
	 *					 ;; (the actual GS2 header includes an optional
	 *					 ;; flag to indicate that the GSS mechanism is not
	 *					 ;; "standard", but since SCRAM is "standard", we
	 *					 ;; don't include that flag).
	 *
	 * username		   = "n=" saslname
	 *					 ;; Usernames are prepared using SASLprep.
	 *
	 * reserved-mext  = "m=" 1*(value-char)
	 *					 ;; Reserved for signaling mandatory extensions.
	 *					 ;; The exact syntax will be defined in
	 *					 ;; the future.
	 *
	 * nonce		   = "r=" c-nonce [s-nonce]
	 *					 ;; Second part provided by server.
	 *
	 * c-nonce		   = printable
	 *
	 * client-first-message-bare =
	 *					 [reserved-mext ","]
	 *					 username "," nonce ["," extensions]
	 *
	 * client-first-message =
	 *					 gs2-header client-first-message-bare
	 *
	 * For example:
	 * n,,n=user,r=fyko+d2lbbFgONRv9qkxdawL
	 *
	 * The "n,," in the beginning means that the client doesn't support
	 * channel binding, and no authzid is given.  "n=user" is the username.
	 * However, in PostgreSQL the username is sent in the startup packet, and
	 * the username in the SCRAM exchange is ignored.  libpq always sends it
	 * as an empty string.  The last part, "r=fyko+d2lbbFgONRv9qkxdawL" is
	 * the client nonce.
	 *------
	 */

	/* read gs2-cbind-flag */
	switch (*input)
	{
		case 'n':
			/* Client does not support channel binding */
			input++;
			break;
		case 'y':
			/* Client supports channel binding, but we're not doing it today */
			input++;
			break;
		case 'p':

			/*
			 * Client requires channel binding.  We don't support it.
			 *
			 * RFC 5802 specifies a particular error code,
			 * e=server-does-support-channel-binding, for this.  But it can
			 * only be sent in the server-final message, and we don't want to
			 * go through the motions of the authentication, knowing it will
			 * fail, just to send that error message.
			 */
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("client requires SCRAM channel binding, but it is not supported")));
		default:
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 (errmsg("malformed SCRAM message (unexpected channel-binding flag %s)",
							 sanitize_char(*input)))));
	}
	if (*input != ',')
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("malformed SCRAM message (comma expected, got %s)",
						sanitize_char(*input))));
	input++;

	/*
	 * Forbid optional authzid (authorization identity).  We don't support it.
	 */
	if (*input == 'a')
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("client uses authorization identity, but it is not supported")));
	if (*input != ',')
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("malformed SCRAM message (unexpected attribute %s in client-first-message)",
						sanitize_char(*input))));
	input++;

	state->client_first_message_bare = pstrdup(input);

	/*
	 * Any mandatory extensions would go here.  We don't support any.
	 *
	 * RFC 5802 specifies error code "e=extensions-not-supported" for this,
	 * but it can only be sent in the server-final message.  We prefer to fail
	 * immediately (which the RFC also allows).
	 */
	if (*input == 'm')
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("client requires mandatory SCRAM extension")));

	/*
	 * Read username.  Note: this is ignored.  We use the username from the
	 * startup message instead, still it is kept around if provided as it
	 * proves to be useful for debugging purposes.
	 */
	state->client_username = read_attr_value(&input, 'n');

	/* read nonce and check that it is made of only printable characters */
	state->client_nonce = read_attr_value(&input, 'r');
	if (!is_scram_printable(state->client_nonce))
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("non-printable characters in SCRAM nonce")));

	/*
	 * There can be any number of optional extensions after this.  We don't
	 * support any extensions, so ignore them.
	 */
	while (*input != '\0')
		read_any_attr(&input, NULL);

	/* success! */
}

/*
 * Verify the final nonce contained in the last message received from
 * client in an exchange.
 */
static bool
verify_final_nonce(scram_state *state)
{
	int			client_nonce_len = strlen(state->client_nonce);
	int			server_nonce_len = strlen(state->server_nonce);
	int			final_nonce_len = strlen(state->client_final_nonce);

	if (final_nonce_len != client_nonce_len + server_nonce_len)
		return false;
	if (memcmp(state->client_final_nonce, state->client_nonce, client_nonce_len) != 0)
		return false;
	if (memcmp(state->client_final_nonce + client_nonce_len, state->server_nonce, server_nonce_len) != 0)
		return false;

	return true;
}

/*
 * Verify the client proof contained in the last message received from
 * client in an exchange.
 */
static bool
verify_client_proof(scram_state *state)
{
	uint8		ClientSignature[SCRAM_KEY_LEN];
	uint8		ClientKey[SCRAM_KEY_LEN];
	uint8		client_StoredKey[SCRAM_KEY_LEN];
	scram_HMAC_ctx ctx;
	int			i;

	/* calculate ClientSignature */
	scram_HMAC_init(&ctx, state->StoredKey, SCRAM_KEY_LEN);
	scram_HMAC_update(&ctx,
					  state->client_first_message_bare,
					  strlen(state->client_first_message_bare));
	scram_HMAC_update(&ctx, ",", 1);
	scram_HMAC_update(&ctx,
					  state->server_first_message,
					  strlen(state->server_first_message));
	scram_HMAC_update(&ctx, ",", 1);
	scram_HMAC_update(&ctx,
					  state->client_final_message_without_proof,
					  strlen(state->client_final_message_without_proof));
	scram_HMAC_final(ClientSignature, &ctx);

	/* Extract the ClientKey that the client calculated from the proof */
	for (i = 0; i < SCRAM_KEY_LEN; i++)
		ClientKey[i] = state->ClientProof[i] ^ ClientSignature[i];

	/* Hash it one more time, and compare with StoredKey */
	scram_H(ClientKey, SCRAM_KEY_LEN, client_StoredKey);

	if (memcmp(client_StoredKey, state->StoredKey, SCRAM_KEY_LEN) != 0)
		return false;

	return true;
}

/*
 * Build the first server-side message sent to the client in a SASL
 * communication exchange.
 */
static char *
build_server_first_message(scram_state *state)
{
	/*------
	 * The syntax for the server-first-message is: (RFC 5802)
	 *
	 * server-first-message =
	 *					 [reserved-mext ","] nonce "," salt ","
	 *					 iteration-count ["," extensions]
	 *
	 * nonce		   = "r=" c-nonce [s-nonce]
	 *					 ;; Second part provided by server.
	 *
	 * c-nonce		   = printable
	 *
	 * s-nonce		   = printable
	 *
	 * salt			   = "s=" base64
	 *
	 * iteration-count = "i=" posit-number
	 *					 ;; A positive number.
	 *
	 * Example:
	 *
	 * r=fyko+d2lbbFgONRv9qkxdawL3rfcNHYJY1ZVvWVs7j,s=QSXCR+Q6sek8bf92,i=4096
	 *------
	 */

	/*
	 * Per the spec, the nonce may consist of any printable ASCII characters.
	 * For convenience, however, we don't use the whole range available,
	 * rather, we generate some random bytes, and base64 encode them.
	 */
	char		raw_nonce[SCRAM_RAW_NONCE_LEN];
	int			encoded_len;

	if (!pg_backend_random(raw_nonce, SCRAM_RAW_NONCE_LEN))
		ereport(COMMERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not generate random nonce")));

	state->server_nonce = palloc(pg_b64_enc_len(SCRAM_RAW_NONCE_LEN) + 1);
	encoded_len = pg_b64_encode(raw_nonce, SCRAM_RAW_NONCE_LEN, state->server_nonce);
	state->server_nonce[encoded_len] = '\0';

	state->server_first_message =
		psprintf("r=%s%s,s=%s,i=%u",
				 state->client_nonce, state->server_nonce,
				 state->salt, state->iterations);

	return state->server_first_message;
}


/*
 * Read and parse the final message received from client.
 */
static void
read_client_final_message(scram_state *state, char *input)
{
	char		attr;
	char	   *channel_binding;
	char	   *value;
	char	   *begin,
			   *proof;
	char	   *p;
	char	   *client_proof;

	begin = p = pstrdup(input);

	/*------
	 * The syntax for the server-first-message is: (RFC 5802)
	 *
	 * gs2-header	   = gs2-cbind-flag "," [ authzid ] ","
	 *					 ;; GS2 header for SCRAM
	 *					 ;; (the actual GS2 header includes an optional
	 *					 ;; flag to indicate that the GSS mechanism is not
	 *					 ;; "standard", but since SCRAM is "standard", we
	 *					 ;; don't include that flag).
	 *
	 * cbind-input	 = gs2-header [ cbind-data ]
	 *					 ;; cbind-data MUST be present for
	 *					 ;; gs2-cbind-flag of "p" and MUST be absent
	 *					 ;; for "y" or "n".
	 *
	 * channel-binding = "c=" base64
	 *					 ;; base64 encoding of cbind-input.
	 *
	 * proof		   = "p=" base64
	 *
	 * client-final-message-without-proof =
	 *					 channel-binding "," nonce [","
	 *					 extensions]
	 *
	 * client-final-message =
	 *					 client-final-message-without-proof "," proof
	 *------
	 */

	/*
	 * Read channel-binding.  We don't support channel binding, so it's
	 * expected to always be "biws", which is "n,,", base64-encoded.
	 */
	channel_binding = read_attr_value(&p, 'c');
	if (strcmp(channel_binding, "biws") != 0)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 (errmsg("unexpected SCRAM channel-binding attribute in client-final-message"))));
	state->client_final_nonce = read_attr_value(&p, 'r');

	/* ignore optional extensions */
	do
	{
		proof = p - 1;
		value = read_any_attr(&p, &attr);
	} while (attr != 'p');

	client_proof = palloc(pg_b64_dec_len(strlen(value)));
	if (pg_b64_decode(value, strlen(value), client_proof) != SCRAM_KEY_LEN)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 (errmsg("malformed SCRAM message (malformed proof in client-final-message"))));
	memcpy(state->ClientProof, client_proof, SCRAM_KEY_LEN);
	pfree(client_proof);

	if (*p != '\0')
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 (errmsg("malformed SCRAM message (garbage at end of client-final-message)"))));

	state->client_final_message_without_proof = palloc(proof - begin + 1);
	memcpy(state->client_final_message_without_proof, input, proof - begin);
	state->client_final_message_without_proof[proof - begin] = '\0';
}

/*
 * Build the final server-side message of an exchange.
 */
static char *
build_server_final_message(scram_state *state)
{
	uint8		ServerSignature[SCRAM_KEY_LEN];
	char	   *server_signature_base64;
	int			siglen;
	scram_HMAC_ctx ctx;

	/* calculate ServerSignature */
	scram_HMAC_init(&ctx, state->ServerKey, SCRAM_KEY_LEN);
	scram_HMAC_update(&ctx,
					  state->client_first_message_bare,
					  strlen(state->client_first_message_bare));
	scram_HMAC_update(&ctx, ",", 1);
	scram_HMAC_update(&ctx,
					  state->server_first_message,
					  strlen(state->server_first_message));
	scram_HMAC_update(&ctx, ",", 1);
	scram_HMAC_update(&ctx,
					  state->client_final_message_without_proof,
					  strlen(state->client_final_message_without_proof));
	scram_HMAC_final(ServerSignature, &ctx);

	server_signature_base64 = palloc(pg_b64_enc_len(SCRAM_KEY_LEN) + 1);
	siglen = pg_b64_encode((const char *) ServerSignature,
						   SCRAM_KEY_LEN, server_signature_base64);
	server_signature_base64[siglen] = '\0';

	/*------
	 * The syntax for the server-final-message is: (RFC 5802)
	 *
	 * verifier		   = "v=" base64
	 *					 ;; base-64 encoded ServerSignature.
	 *
	 * server-final-message = (server-error / verifier)
	 *					 ["," extensions]
	 *
	 *------
	 */
	return psprintf("v=%s", server_signature_base64);
}


/*
 * Determinisitcally generate salt for mock authentication, using a SHA256
 * hash based on the username and a cluster-level secret key.  Returns a
 * pointer to a static buffer of size SCRAM_SALT_LEN.
 */
static char *
scram_MockSalt(const char *username)
{
	pg_sha256_ctx ctx;
	static uint8 sha_digest[PG_SHA256_DIGEST_LENGTH];
	char	   *mock_auth_nonce = GetMockAuthenticationNonce();

	/*
	 * Generate salt using a SHA256 hash of the username and the cluster's
	 * mock authentication nonce.  (This works as long as the salt length is
	 * not larger the SHA256 digest length. If the salt is smaller, the caller
	 * will just ignore the extra data))
	 */
	StaticAssertStmt(PG_SHA256_DIGEST_LENGTH >= SCRAM_SALT_LEN,
					 "salt length greater than SHA256 digest length");

	pg_sha256_init(&ctx);
	pg_sha256_update(&ctx, (uint8 *) username, strlen(username));
	pg_sha256_update(&ctx, (uint8 *) mock_auth_nonce, MOCK_AUTH_NONCE_LEN);
	pg_sha256_final(&ctx, sha_digest);

	return (char *) sha_digest;
}
