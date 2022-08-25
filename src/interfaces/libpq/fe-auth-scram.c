/*-------------------------------------------------------------------------
 *
 * fe-auth-scram.c
 *	   The front-end (client) implementation of SCRAM authentication.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-auth-scram.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "common/base64.h"
#include "common/hmac.h"
#include "common/saslprep.h"
#include "common/scram-common.h"
#include "fe-auth.h"


/* The exported SCRAM callback mechanism. */
static void *scram_init(PGconn *conn, const char *password,
						const char *sasl_mechanism);
static void scram_exchange(void *opaq, char *input, int inputlen,
						   char **output, int *outputlen,
						   bool *done, bool *success);
static bool scram_channel_bound(void *opaq);
static void scram_free(void *opaq);

const pg_fe_sasl_mech pg_scram_mech = {
	scram_init,
	scram_exchange,
	scram_channel_bound,
	scram_free
};

/*
 * Status of exchange messages used for SCRAM authentication via the
 * SASL protocol.
 */
typedef enum
{
	FE_SCRAM_INIT,
	FE_SCRAM_NONCE_SENT,
	FE_SCRAM_PROOF_SENT,
	FE_SCRAM_FINISHED
} fe_scram_state_enum;

typedef struct
{
	fe_scram_state_enum state;

	/* These are supplied by the user */
	PGconn	   *conn;
	char	   *password;
	char	   *sasl_mechanism;

	/* We construct these */
	uint8		SaltedPassword[SCRAM_KEY_LEN];
	char	   *client_nonce;
	char	   *client_first_message_bare;
	char	   *client_final_message_without_proof;

	/* These come from the server-first message */
	char	   *server_first_message;
	char	   *salt;
	int			saltlen;
	int			iterations;
	char	   *nonce;

	/* These come from the server-final message */
	char	   *server_final_message;
	char		ServerSignature[SCRAM_KEY_LEN];
} fe_scram_state;

static bool read_server_first_message(fe_scram_state *state, char *input);
static bool read_server_final_message(fe_scram_state *state, char *input);
static char *build_client_first_message(fe_scram_state *state);
static char *build_client_final_message(fe_scram_state *state);
static bool verify_server_signature(fe_scram_state *state, bool *match,
									const char **errstr);
static bool calculate_client_proof(fe_scram_state *state,
								   const char *client_final_message_without_proof,
								   uint8 *result, const char **errstr);

/*
 * Initialize SCRAM exchange status.
 */
static void *
scram_init(PGconn *conn,
		   const char *password,
		   const char *sasl_mechanism)
{
	fe_scram_state *state;
	char	   *prep_password;
	pg_saslprep_rc rc;

	Assert(sasl_mechanism != NULL);

	state = (fe_scram_state *) malloc(sizeof(fe_scram_state));
	if (!state)
		return NULL;
	memset(state, 0, sizeof(fe_scram_state));
	state->conn = conn;
	state->state = FE_SCRAM_INIT;
	state->sasl_mechanism = strdup(sasl_mechanism);

	if (!state->sasl_mechanism)
	{
		free(state);
		return NULL;
	}

	/* Normalize the password with SASLprep, if possible */
	rc = pg_saslprep(password, &prep_password);
	if (rc == SASLPREP_OOM)
	{
		free(state->sasl_mechanism);
		free(state);
		return NULL;
	}
	if (rc != SASLPREP_SUCCESS)
	{
		prep_password = strdup(password);
		if (!prep_password)
		{
			free(state->sasl_mechanism);
			free(state);
			return NULL;
		}
	}
	state->password = prep_password;

	return state;
}

/*
 * Return true if channel binding was employed and the SCRAM exchange
 * completed. This should be used after a successful exchange to determine
 * whether the server authenticated itself to the client.
 *
 * Note that the caller must also ensure that the exchange was actually
 * successful.
 */
static bool
scram_channel_bound(void *opaq)
{
	fe_scram_state *state = (fe_scram_state *) opaq;

	/* no SCRAM exchange done */
	if (state == NULL)
		return false;

	/* SCRAM exchange not completed */
	if (state->state != FE_SCRAM_FINISHED)
		return false;

	/* channel binding mechanism not used */
	if (strcmp(state->sasl_mechanism, SCRAM_SHA_256_PLUS_NAME) != 0)
		return false;

	/* all clear! */
	return true;
}

/*
 * Free SCRAM exchange status
 */
static void
scram_free(void *opaq)
{
	fe_scram_state *state = (fe_scram_state *) opaq;

	if (state->password)
		free(state->password);
	if (state->sasl_mechanism)
		free(state->sasl_mechanism);

	/* client messages */
	if (state->client_nonce)
		free(state->client_nonce);
	if (state->client_first_message_bare)
		free(state->client_first_message_bare);
	if (state->client_final_message_without_proof)
		free(state->client_final_message_without_proof);

	/* first message from server */
	if (state->server_first_message)
		free(state->server_first_message);
	if (state->salt)
		free(state->salt);
	if (state->nonce)
		free(state->nonce);

	/* final message from server */
	if (state->server_final_message)
		free(state->server_final_message);

	free(state);
}

/*
 * Exchange a SCRAM message with backend.
 */
static void
scram_exchange(void *opaq, char *input, int inputlen,
			   char **output, int *outputlen,
			   bool *done, bool *success)
{
	fe_scram_state *state = (fe_scram_state *) opaq;
	PGconn	   *conn = state->conn;
	const char *errstr = NULL;

	*done = false;
	*success = false;
	*output = NULL;
	*outputlen = 0;

	/*
	 * Check that the input length agrees with the string length of the input.
	 * We can ignore inputlen after this.
	 */
	if (state->state != FE_SCRAM_INIT)
	{
		if (inputlen == 0)
		{
			appendPQExpBufferStr(&conn->errorMessage,
								 libpq_gettext("malformed SCRAM message (empty message)\n"));
			goto error;
		}
		if (inputlen != strlen(input))
		{
			appendPQExpBufferStr(&conn->errorMessage,
								 libpq_gettext("malformed SCRAM message (length mismatch)\n"));
			goto error;
		}
	}

	switch (state->state)
	{
		case FE_SCRAM_INIT:
			/* Begin the SCRAM handshake, by sending client nonce */
			*output = build_client_first_message(state);
			if (*output == NULL)
				goto error;

			*outputlen = strlen(*output);
			*done = false;
			state->state = FE_SCRAM_NONCE_SENT;
			break;

		case FE_SCRAM_NONCE_SENT:
			/* Receive salt and server nonce, send response. */
			if (!read_server_first_message(state, input))
				goto error;

			*output = build_client_final_message(state);
			if (*output == NULL)
				goto error;

			*outputlen = strlen(*output);
			*done = false;
			state->state = FE_SCRAM_PROOF_SENT;
			break;

		case FE_SCRAM_PROOF_SENT:
			/* Receive server signature */
			if (!read_server_final_message(state, input))
				goto error;

			/*
			 * Verify server signature, to make sure we're talking to the
			 * genuine server.
			 */
			if (!verify_server_signature(state, success, &errstr))
			{
				appendPQExpBuffer(&conn->errorMessage,
								  libpq_gettext("could not verify server signature: %s\n"), errstr);
				goto error;
			}

			if (!*success)
			{
				appendPQExpBufferStr(&conn->errorMessage,
									 libpq_gettext("incorrect server signature\n"));
			}
			*done = true;
			state->state = FE_SCRAM_FINISHED;
			break;

		default:
			/* shouldn't happen */
			appendPQExpBufferStr(&conn->errorMessage,
								 libpq_gettext("invalid SCRAM exchange state\n"));
			goto error;
	}
	return;

error:
	*done = true;
	*success = false;
}

/*
 * Read value for an attribute part of a SCRAM message.
 *
 * The buffer at **input is destructively modified, and *input is
 * advanced over the "attr=value" string and any following comma.
 *
 * On failure, append an error message to *errorMessage and return NULL.
 */
static char *
read_attr_value(char **input, char attr, PQExpBuffer errorMessage)
{
	char	   *begin = *input;
	char	   *end;

	if (*begin != attr)
	{
		appendPQExpBuffer(errorMessage,
						  libpq_gettext("malformed SCRAM message (attribute \"%c\" expected)\n"),
						  attr);
		return NULL;
	}
	begin++;

	if (*begin != '=')
	{
		appendPQExpBuffer(errorMessage,
						  libpq_gettext("malformed SCRAM message (expected character \"=\" for attribute \"%c\")\n"),
						  attr);
		return NULL;
	}
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
 * Build the first exchange message sent by the client.
 */
static char *
build_client_first_message(fe_scram_state *state)
{
	PGconn	   *conn = state->conn;
	char		raw_nonce[SCRAM_RAW_NONCE_LEN + 1];
	char	   *result;
	int			channel_info_len;
	int			encoded_len;
	PQExpBufferData buf;

	/*
	 * Generate a "raw" nonce.  This is converted to ASCII-printable form by
	 * base64-encoding it.
	 */
	if (!pg_strong_random(raw_nonce, SCRAM_RAW_NONCE_LEN))
	{
		appendPQExpBufferStr(&conn->errorMessage,
							 libpq_gettext("could not generate nonce\n"));
		return NULL;
	}

	encoded_len = pg_b64_enc_len(SCRAM_RAW_NONCE_LEN);
	/* don't forget the zero-terminator */
	state->client_nonce = malloc(encoded_len + 1);
	if (state->client_nonce == NULL)
	{
		appendPQExpBufferStr(&conn->errorMessage,
							 libpq_gettext("out of memory\n"));
		return NULL;
	}
	encoded_len = pg_b64_encode(raw_nonce, SCRAM_RAW_NONCE_LEN,
								state->client_nonce, encoded_len);
	if (encoded_len < 0)
	{
		appendPQExpBufferStr(&conn->errorMessage,
							 libpq_gettext("could not encode nonce\n"));
		return NULL;
	}
	state->client_nonce[encoded_len] = '\0';

	/*
	 * Generate message.  The username is left empty as the backend uses the
	 * value provided by the startup packet.  Also, as this username is not
	 * prepared with SASLprep, the message parsing would fail if it includes
	 * '=' or ',' characters.
	 */

	initPQExpBuffer(&buf);

	/*
	 * First build the gs2-header with channel binding information.
	 */
	if (strcmp(state->sasl_mechanism, SCRAM_SHA_256_PLUS_NAME) == 0)
	{
		Assert(conn->ssl_in_use);
		appendPQExpBufferStr(&buf, "p=tls-server-end-point");
	}
#ifdef HAVE_PGTLS_GET_PEER_CERTIFICATE_HASH
	else if (conn->channel_binding[0] != 'd' && /* disable */
			 conn->ssl_in_use)
	{
		/*
		 * Client supports channel binding, but thinks the server does not.
		 */
		appendPQExpBufferChar(&buf, 'y');
	}
#endif
	else
	{
		/*
		 * Client does not support channel binding, or has disabled it.
		 */
		appendPQExpBufferChar(&buf, 'n');
	}

	if (PQExpBufferDataBroken(buf))
		goto oom_error;

	channel_info_len = buf.len;

	appendPQExpBuffer(&buf, ",,n=,r=%s", state->client_nonce);
	if (PQExpBufferDataBroken(buf))
		goto oom_error;

	/*
	 * The first message content needs to be saved without channel binding
	 * information.
	 */
	state->client_first_message_bare = strdup(buf.data + channel_info_len + 2);
	if (!state->client_first_message_bare)
		goto oom_error;

	result = strdup(buf.data);
	if (result == NULL)
		goto oom_error;

	termPQExpBuffer(&buf);
	return result;

oom_error:
	termPQExpBuffer(&buf);
	appendPQExpBufferStr(&conn->errorMessage,
						 libpq_gettext("out of memory\n"));
	return NULL;
}

/*
 * Build the final exchange message sent from the client.
 */
static char *
build_client_final_message(fe_scram_state *state)
{
	PQExpBufferData buf;
	PGconn	   *conn = state->conn;
	uint8		client_proof[SCRAM_KEY_LEN];
	char	   *result;
	int			encoded_len;
	const char *errstr = NULL;

	initPQExpBuffer(&buf);

	/*
	 * Construct client-final-message-without-proof.  We need to remember it
	 * for verifying the server proof in the final step of authentication.
	 *
	 * The channel binding flag handling (p/y/n) must be consistent with
	 * build_client_first_message(), because the server will check that it's
	 * the same flag both times.
	 */
	if (strcmp(state->sasl_mechanism, SCRAM_SHA_256_PLUS_NAME) == 0)
	{
#ifdef HAVE_PGTLS_GET_PEER_CERTIFICATE_HASH
		char	   *cbind_data = NULL;
		size_t		cbind_data_len = 0;
		size_t		cbind_header_len;
		char	   *cbind_input;
		size_t		cbind_input_len;
		int			encoded_cbind_len;

		/* Fetch hash data of server's SSL certificate */
		cbind_data =
			pgtls_get_peer_certificate_hash(state->conn,
											&cbind_data_len);
		if (cbind_data == NULL)
		{
			/* error message is already set on error */
			termPQExpBuffer(&buf);
			return NULL;
		}

		appendPQExpBufferStr(&buf, "c=");

		/* p=type,, */
		cbind_header_len = strlen("p=tls-server-end-point,,");
		cbind_input_len = cbind_header_len + cbind_data_len;
		cbind_input = malloc(cbind_input_len);
		if (!cbind_input)
		{
			free(cbind_data);
			goto oom_error;
		}
		memcpy(cbind_input, "p=tls-server-end-point,,", cbind_header_len);
		memcpy(cbind_input + cbind_header_len, cbind_data, cbind_data_len);

		encoded_cbind_len = pg_b64_enc_len(cbind_input_len);
		if (!enlargePQExpBuffer(&buf, encoded_cbind_len))
		{
			free(cbind_data);
			free(cbind_input);
			goto oom_error;
		}
		encoded_cbind_len = pg_b64_encode(cbind_input, cbind_input_len,
										  buf.data + buf.len,
										  encoded_cbind_len);
		if (encoded_cbind_len < 0)
		{
			free(cbind_data);
			free(cbind_input);
			termPQExpBuffer(&buf);
			appendPQExpBufferStr(&conn->errorMessage,
								 "could not encode cbind data for channel binding\n");
			return NULL;
		}
		buf.len += encoded_cbind_len;
		buf.data[buf.len] = '\0';

		free(cbind_data);
		free(cbind_input);
#else
		/*
		 * Chose channel binding, but the SSL library doesn't support it.
		 * Shouldn't happen.
		 */
		termPQExpBuffer(&buf);
		appendPQExpBufferStr(&conn->errorMessage,
							 "channel binding not supported by this build\n");
		return NULL;
#endif							/* HAVE_PGTLS_GET_PEER_CERTIFICATE_HASH */
	}
#ifdef HAVE_PGTLS_GET_PEER_CERTIFICATE_HASH
	else if (conn->channel_binding[0] != 'd' && /* disable */
			 conn->ssl_in_use)
		appendPQExpBufferStr(&buf, "c=eSws");	/* base64 of "y,," */
#endif
	else
		appendPQExpBufferStr(&buf, "c=biws");	/* base64 of "n,," */

	if (PQExpBufferDataBroken(buf))
		goto oom_error;

	appendPQExpBuffer(&buf, ",r=%s", state->nonce);
	if (PQExpBufferDataBroken(buf))
		goto oom_error;

	state->client_final_message_without_proof = strdup(buf.data);
	if (state->client_final_message_without_proof == NULL)
		goto oom_error;

	/* Append proof to it, to form client-final-message. */
	if (!calculate_client_proof(state,
								state->client_final_message_without_proof,
								client_proof, &errstr))
	{
		termPQExpBuffer(&buf);
		appendPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("could not calculate client proof: %s\n"),
						  errstr);
		return NULL;
	}

	appendPQExpBufferStr(&buf, ",p=");
	encoded_len = pg_b64_enc_len(SCRAM_KEY_LEN);
	if (!enlargePQExpBuffer(&buf, encoded_len))
		goto oom_error;
	encoded_len = pg_b64_encode((char *) client_proof,
								SCRAM_KEY_LEN,
								buf.data + buf.len,
								encoded_len);
	if (encoded_len < 0)
	{
		termPQExpBuffer(&buf);
		appendPQExpBufferStr(&conn->errorMessage,
							 libpq_gettext("could not encode client proof\n"));
		return NULL;
	}
	buf.len += encoded_len;
	buf.data[buf.len] = '\0';

	result = strdup(buf.data);
	if (result == NULL)
		goto oom_error;

	termPQExpBuffer(&buf);
	return result;

oom_error:
	termPQExpBuffer(&buf);
	appendPQExpBufferStr(&conn->errorMessage,
						 libpq_gettext("out of memory\n"));
	return NULL;
}

/*
 * Read the first exchange message coming from the server.
 */
static bool
read_server_first_message(fe_scram_state *state, char *input)
{
	PGconn	   *conn = state->conn;
	char	   *iterations_str;
	char	   *endptr;
	char	   *encoded_salt;
	char	   *nonce;
	int			decoded_salt_len;

	state->server_first_message = strdup(input);
	if (state->server_first_message == NULL)
	{
		appendPQExpBufferStr(&conn->errorMessage,
							 libpq_gettext("out of memory\n"));
		return false;
	}

	/* parse the message */
	nonce = read_attr_value(&input, 'r',
							&conn->errorMessage);
	if (nonce == NULL)
	{
		/* read_attr_value() has appended an error string */
		return false;
	}

	/* Verify immediately that the server used our part of the nonce */
	if (strlen(nonce) < strlen(state->client_nonce) ||
		memcmp(nonce, state->client_nonce, strlen(state->client_nonce)) != 0)
	{
		appendPQExpBufferStr(&conn->errorMessage,
							 libpq_gettext("invalid SCRAM response (nonce mismatch)\n"));
		return false;
	}

	state->nonce = strdup(nonce);
	if (state->nonce == NULL)
	{
		appendPQExpBufferStr(&conn->errorMessage,
							 libpq_gettext("out of memory\n"));
		return false;
	}

	encoded_salt = read_attr_value(&input, 's', &conn->errorMessage);
	if (encoded_salt == NULL)
	{
		/* read_attr_value() has appended an error string */
		return false;
	}
	decoded_salt_len = pg_b64_dec_len(strlen(encoded_salt));
	state->salt = malloc(decoded_salt_len);
	if (state->salt == NULL)
	{
		appendPQExpBufferStr(&conn->errorMessage,
							 libpq_gettext("out of memory\n"));
		return false;
	}
	state->saltlen = pg_b64_decode(encoded_salt,
								   strlen(encoded_salt),
								   state->salt,
								   decoded_salt_len);
	if (state->saltlen < 0)
	{
		appendPQExpBufferStr(&conn->errorMessage,
							 libpq_gettext("malformed SCRAM message (invalid salt)\n"));
		return false;
	}

	iterations_str = read_attr_value(&input, 'i', &conn->errorMessage);
	if (iterations_str == NULL)
	{
		/* read_attr_value() has appended an error string */
		return false;
	}
	state->iterations = strtol(iterations_str, &endptr, 10);
	if (*endptr != '\0' || state->iterations < 1)
	{
		appendPQExpBufferStr(&conn->errorMessage,
							 libpq_gettext("malformed SCRAM message (invalid iteration count)\n"));
		return false;
	}

	if (*input != '\0')
		appendPQExpBufferStr(&conn->errorMessage,
							 libpq_gettext("malformed SCRAM message (garbage at end of server-first-message)\n"));

	return true;
}

/*
 * Read the final exchange message coming from the server.
 */
static bool
read_server_final_message(fe_scram_state *state, char *input)
{
	PGconn	   *conn = state->conn;
	char	   *encoded_server_signature;
	char	   *decoded_server_signature;
	int			server_signature_len;

	state->server_final_message = strdup(input);
	if (!state->server_final_message)
	{
		appendPQExpBufferStr(&conn->errorMessage,
							 libpq_gettext("out of memory\n"));
		return false;
	}

	/* Check for error result. */
	if (*input == 'e')
	{
		char	   *errmsg = read_attr_value(&input, 'e',
											 &conn->errorMessage);

		if (errmsg == NULL)
		{
			/* read_attr_value() has appended an error message */
			return false;
		}
		appendPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("error received from server in SCRAM exchange: %s\n"),
						  errmsg);
		return false;
	}

	/* Parse the message. */
	encoded_server_signature = read_attr_value(&input, 'v',
											   &conn->errorMessage);
	if (encoded_server_signature == NULL)
	{
		/* read_attr_value() has appended an error message */
		return false;
	}

	if (*input != '\0')
		appendPQExpBufferStr(&conn->errorMessage,
							 libpq_gettext("malformed SCRAM message (garbage at end of server-final-message)\n"));

	server_signature_len = pg_b64_dec_len(strlen(encoded_server_signature));
	decoded_server_signature = malloc(server_signature_len);
	if (!decoded_server_signature)
	{
		appendPQExpBufferStr(&conn->errorMessage,
							 libpq_gettext("out of memory\n"));
		return false;
	}

	server_signature_len = pg_b64_decode(encoded_server_signature,
										 strlen(encoded_server_signature),
										 decoded_server_signature,
										 server_signature_len);
	if (server_signature_len != SCRAM_KEY_LEN)
	{
		free(decoded_server_signature);
		appendPQExpBufferStr(&conn->errorMessage,
							 libpq_gettext("malformed SCRAM message (invalid server signature)\n"));
		return false;
	}
	memcpy(state->ServerSignature, decoded_server_signature, SCRAM_KEY_LEN);
	free(decoded_server_signature);

	return true;
}

/*
 * Calculate the client proof, part of the final exchange message sent
 * by the client.  Returns true on success, false on failure with *errstr
 * pointing to a message about the error details.
 */
static bool
calculate_client_proof(fe_scram_state *state,
					   const char *client_final_message_without_proof,
					   uint8 *result, const char **errstr)
{
	uint8		StoredKey[SCRAM_KEY_LEN];
	uint8		ClientKey[SCRAM_KEY_LEN];
	uint8		ClientSignature[SCRAM_KEY_LEN];
	int			i;
	pg_hmac_ctx *ctx;

	ctx = pg_hmac_create(PG_SHA256);
	if (ctx == NULL)
	{
		*errstr = pg_hmac_error(NULL);	/* returns OOM */
		return false;
	}

	/*
	 * Calculate SaltedPassword, and store it in 'state' so that we can reuse
	 * it later in verify_server_signature.
	 */
	if (scram_SaltedPassword(state->password, state->salt, state->saltlen,
							 state->iterations, state->SaltedPassword,
							 errstr) < 0 ||
		scram_ClientKey(state->SaltedPassword, ClientKey, errstr) < 0 ||
		scram_H(ClientKey, SCRAM_KEY_LEN, StoredKey, errstr) < 0)
	{
		/* errstr is already filled here */
		pg_hmac_free(ctx);
		return false;
	}

	if (pg_hmac_init(ctx, StoredKey, SCRAM_KEY_LEN) < 0 ||
		pg_hmac_update(ctx,
					   (uint8 *) state->client_first_message_bare,
					   strlen(state->client_first_message_bare)) < 0 ||
		pg_hmac_update(ctx, (uint8 *) ",", 1) < 0 ||
		pg_hmac_update(ctx,
					   (uint8 *) state->server_first_message,
					   strlen(state->server_first_message)) < 0 ||
		pg_hmac_update(ctx, (uint8 *) ",", 1) < 0 ||
		pg_hmac_update(ctx,
					   (uint8 *) client_final_message_without_proof,
					   strlen(client_final_message_without_proof)) < 0 ||
		pg_hmac_final(ctx, ClientSignature, sizeof(ClientSignature)) < 0)
	{
		*errstr = pg_hmac_error(ctx);
		pg_hmac_free(ctx);
		return false;
	}

	for (i = 0; i < SCRAM_KEY_LEN; i++)
		result[i] = ClientKey[i] ^ ClientSignature[i];

	pg_hmac_free(ctx);
	return true;
}

/*
 * Validate the server signature, received as part of the final exchange
 * message received from the server.  *match tracks if the server signature
 * matched or not. Returns true if the server signature got verified, and
 * false for a processing error with *errstr pointing to a message about the
 * error details.
 */
static bool
verify_server_signature(fe_scram_state *state, bool *match,
						const char **errstr)
{
	uint8		expected_ServerSignature[SCRAM_KEY_LEN];
	uint8		ServerKey[SCRAM_KEY_LEN];
	pg_hmac_ctx *ctx;

	ctx = pg_hmac_create(PG_SHA256);
	if (ctx == NULL)
	{
		*errstr = pg_hmac_error(NULL);	/* returns OOM */
		return false;
	}

	if (scram_ServerKey(state->SaltedPassword, ServerKey, errstr) < 0)
	{
		/* errstr is filled already */
		pg_hmac_free(ctx);
		return false;
	}

	/* calculate ServerSignature */
	if (pg_hmac_init(ctx, ServerKey, SCRAM_KEY_LEN) < 0 ||
		pg_hmac_update(ctx,
					   (uint8 *) state->client_first_message_bare,
					   strlen(state->client_first_message_bare)) < 0 ||
		pg_hmac_update(ctx, (uint8 *) ",", 1) < 0 ||
		pg_hmac_update(ctx,
					   (uint8 *) state->server_first_message,
					   strlen(state->server_first_message)) < 0 ||
		pg_hmac_update(ctx, (uint8 *) ",", 1) < 0 ||
		pg_hmac_update(ctx,
					   (uint8 *) state->client_final_message_without_proof,
					   strlen(state->client_final_message_without_proof)) < 0 ||
		pg_hmac_final(ctx, expected_ServerSignature,
					  sizeof(expected_ServerSignature)) < 0)
	{
		*errstr = pg_hmac_error(ctx);
		pg_hmac_free(ctx);
		return false;
	}

	pg_hmac_free(ctx);

	/* signature processed, so now check after it */
	if (memcmp(expected_ServerSignature, state->ServerSignature, SCRAM_KEY_LEN) != 0)
		*match = false;
	else
		*match = true;

	return true;
}

/*
 * Build a new SCRAM secret.
 *
 * On error, returns NULL and sets *errstr to point to a message about the
 * error details.
 */
char *
pg_fe_scram_build_secret(const char *password, const char **errstr)
{
	char	   *prep_password;
	pg_saslprep_rc rc;
	char		saltbuf[SCRAM_DEFAULT_SALT_LEN];
	char	   *result;

	/*
	 * Normalize the password with SASLprep.  If that doesn't work, because
	 * the password isn't valid UTF-8 or contains prohibited characters, just
	 * proceed with the original password.  (See comments at top of file.)
	 */
	rc = pg_saslprep(password, &prep_password);
	if (rc == SASLPREP_OOM)
	{
		*errstr = libpq_gettext("out of memory");
		return NULL;
	}
	if (rc == SASLPREP_SUCCESS)
		password = (const char *) prep_password;

	/* Generate a random salt */
	if (!pg_strong_random(saltbuf, SCRAM_DEFAULT_SALT_LEN))
	{
		*errstr = libpq_gettext("could not generate random salt");
		if (prep_password)
			free(prep_password);
		return NULL;
	}

	result = scram_build_secret(saltbuf, SCRAM_DEFAULT_SALT_LEN,
								SCRAM_DEFAULT_ITERATIONS, password,
								errstr);

	if (prep_password)
		free(prep_password);

	return result;
}
