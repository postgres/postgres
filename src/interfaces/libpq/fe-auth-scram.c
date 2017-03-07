/*-------------------------------------------------------------------------
 *
 * fe-auth-scram.c
 *	   The front-end (client) implementation of SCRAM authentication.
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-auth-scram.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "common/base64.h"
#include "common/scram-common.h"
#include "fe-auth.h"

/* These are needed for getpid(), in the fallback implementation */
#ifndef HAVE_STRONG_RANDOM
#include <sys/types.h>
#include <unistd.h>
#endif

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
	const char *username;
	const char *password;

	/* We construct these */
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
	char		ServerProof[SCRAM_KEY_LEN];
} fe_scram_state;

static bool read_server_first_message(fe_scram_state *state, char *input,
						  PQExpBuffer errormessage);
static bool read_server_final_message(fe_scram_state *state, char *input,
						  PQExpBuffer errormessage);
static char *build_client_first_message(fe_scram_state *state,
						   PQExpBuffer errormessage);
static char *build_client_final_message(fe_scram_state *state,
						   PQExpBuffer errormessage);
static bool verify_server_proof(fe_scram_state *state);
static void calculate_client_proof(fe_scram_state *state,
					   const char *client_final_message_without_proof,
					   uint8 *result);
static bool pg_frontend_random(char *dst, int len);

/*
 * Initialize SCRAM exchange status.
 */
void *
pg_fe_scram_init(const char *username, const char *password)
{
	fe_scram_state *state;

	state = (fe_scram_state *) malloc(sizeof(fe_scram_state));
	if (!state)
		return NULL;
	memset(state, 0, sizeof(fe_scram_state));
	state->state = FE_SCRAM_INIT;
	state->username = username;
	state->password = password;

	return state;
}

/*
 * Free SCRAM exchange status
 */
void
pg_fe_scram_free(void *opaq)
{
	fe_scram_state *state = (fe_scram_state *) opaq;

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
void
pg_fe_scram_exchange(void *opaq, char *input, int inputlen,
					 char **output, int *outputlen,
					 bool *done, bool *success, PQExpBuffer errorMessage)
{
	fe_scram_state *state = (fe_scram_state *) opaq;

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
			printfPQExpBuffer(errorMessage,
				 libpq_gettext("malformed SCRAM message (empty message)\n"));
			goto error;
		}
		if (inputlen != strlen(input))
		{
			printfPQExpBuffer(errorMessage,
			   libpq_gettext("malformed SCRAM message (length mismatch)\n"));
			goto error;
		}
	}

	switch (state->state)
	{
		case FE_SCRAM_INIT:
			/* Begin the SCRAM handshake, by sending client nonce */
			*output = build_client_first_message(state, errorMessage);
			if (*output == NULL)
				goto error;

			*outputlen = strlen(*output);
			*done = false;
			state->state = FE_SCRAM_NONCE_SENT;
			break;

		case FE_SCRAM_NONCE_SENT:
			/* Receive salt and server nonce, send response. */
			if (!read_server_first_message(state, input, errorMessage))
				goto error;

			*output = build_client_final_message(state, errorMessage);
			if (*output == NULL)
				goto error;

			*outputlen = strlen(*output);
			*done = false;
			state->state = FE_SCRAM_PROOF_SENT;
			break;

		case FE_SCRAM_PROOF_SENT:
			/* Receive server proof */
			if (!read_server_final_message(state, input, errorMessage))
				goto error;

			/*
			 * Verify server proof, to make sure we're talking to the genuine
			 * server.  XXX: A fake server could simply not require
			 * authentication, though.  There is currently no option in libpq
			 * to reject a connection, if SCRAM authentication did not happen.
			 */
			if (verify_server_proof(state))
				*success = true;
			else
			{
				*success = false;
				printfPQExpBuffer(errorMessage,
								  libpq_gettext("invalid server proof\n"));
			}
			*done = true;
			state->state = FE_SCRAM_FINISHED;
			break;

		default:
			/* shouldn't happen */
			printfPQExpBuffer(errorMessage,
							libpq_gettext("invalid SCRAM exchange state\n"));
			goto error;
	}
	return;

error:
	*done = true;
	*success = false;
	return;
}

/*
 * Read value for an attribute part of a SASL message.
 */
static char *
read_attr_value(char **input, char attr, PQExpBuffer errorMessage)
{
	char	   *begin = *input;
	char	   *end;

	if (*begin != attr)
	{
		printfPQExpBuffer(errorMessage,
					libpq_gettext("malformed SCRAM message (%c expected)\n"),
						  attr);
		return NULL;
	}
	begin++;

	if (*begin != '=')
	{
		printfPQExpBuffer(errorMessage,
		libpq_gettext("malformed SCRAM message (expected = in attr '%c')\n"),
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
build_client_first_message(fe_scram_state *state, PQExpBuffer errormessage)
{
	char		raw_nonce[SCRAM_RAW_NONCE_LEN + 1];
	char	   *buf;
	char		buflen;
	int			encoded_len;

	/*
	 * Generate a "raw" nonce.  This is converted to ASCII-printable form by
	 * base64-encoding it.
	 */
	if (!pg_frontend_random(raw_nonce, SCRAM_RAW_NONCE_LEN))
	{
		printfPQExpBuffer(errormessage,
						  libpq_gettext("failed to generate nonce\n"));
		return NULL;
	}

	state->client_nonce = malloc(pg_b64_enc_len(SCRAM_RAW_NONCE_LEN) + 1);
	if (state->client_nonce == NULL)
	{
		printfPQExpBuffer(errormessage,
						  libpq_gettext("out of memory\n"));
		return NULL;
	}
	encoded_len = pg_b64_encode(raw_nonce, SCRAM_RAW_NONCE_LEN, state->client_nonce);
	state->client_nonce[encoded_len] = '\0';

	/*
	 * Generate message.  The username is left empty as the backend uses the
	 * value provided by the startup packet.  Also, as this username is not
	 * prepared with SASLprep, the message parsing would fail if it includes
	 * '=' or ',' characters.
	 */
	buflen = 8 + strlen(state->client_nonce) + 1;
	buf = malloc(buflen);
	if (buf == NULL)
	{
		printfPQExpBuffer(errormessage,
						  libpq_gettext("out of memory\n"));
		return NULL;
	}
	snprintf(buf, buflen, "n,,n=,r=%s", state->client_nonce);

	state->client_first_message_bare = strdup(buf + 3);
	if (!state->client_first_message_bare)
	{
		free(buf);
		printfPQExpBuffer(errormessage,
						  libpq_gettext("out of memory\n"));
		return NULL;
	}

	return buf;
}

/*
 * Build the final exchange message sent from the client.
 */
static char *
build_client_final_message(fe_scram_state *state, PQExpBuffer errormessage)
{
	PQExpBufferData buf;
	uint8		client_proof[SCRAM_KEY_LEN];
	char	   *result;

	initPQExpBuffer(&buf);

	/*
	 * Construct client-final-message-without-proof.  We need to remember it
	 * for verifying the server proof in the final step of authentication.
	 */
	appendPQExpBuffer(&buf, "c=biws,r=%s", state->nonce);
	if (PQExpBufferDataBroken(buf))
		goto oom_error;

	state->client_final_message_without_proof = strdup(buf.data);
	if (state->client_final_message_without_proof == NULL)
		goto oom_error;

	/* Append proof to it, to form client-final-message. */
	calculate_client_proof(state,
						   state->client_final_message_without_proof,
						   client_proof);

	appendPQExpBuffer(&buf, ",p=");
	if (!enlargePQExpBuffer(&buf, pg_b64_enc_len(SCRAM_KEY_LEN)))
		goto oom_error;
	buf.len += pg_b64_encode((char *) client_proof,
							 SCRAM_KEY_LEN,
							 buf.data + buf.len);
	buf.data[buf.len] = '\0';

	result = strdup(buf.data);
	if (result == NULL)
		goto oom_error;

	termPQExpBuffer(&buf);
	return result;

oom_error:
	termPQExpBuffer(&buf);
	printfPQExpBuffer(errormessage,
					  libpq_gettext("out of memory\n"));
	return NULL;
}

/*
 * Read the first exchange message coming from the server.
 */
static bool
read_server_first_message(fe_scram_state *state, char *input,
						  PQExpBuffer errormessage)
{
	char	   *iterations_str;
	char	   *endptr;
	char	   *encoded_salt;
	char	   *nonce;

	state->server_first_message = strdup(input);
	if (state->server_first_message == NULL)
	{
		printfPQExpBuffer(errormessage,
						  libpq_gettext("out of memory\n"));
		return false;
	}

	/* parse the message */
	nonce = read_attr_value(&input, 'r', errormessage);
	if (nonce == NULL)
	{
		/* read_attr_value() has generated an error string */
		return false;
	}

	/* Verify immediately that the server used our part of the nonce */
	if (strncmp(nonce, state->client_nonce, strlen(state->client_nonce)) != 0)
	{
		printfPQExpBuffer(errormessage,
				 libpq_gettext("invalid SCRAM response (nonce mismatch)\n"));
		return false;
	}

	state->nonce = strdup(nonce);
	if (state->nonce == NULL)
	{
		printfPQExpBuffer(errormessage,
						  libpq_gettext("out of memory\n"));
		return false;
	}

	encoded_salt = read_attr_value(&input, 's', errormessage);
	if (encoded_salt == NULL)
	{
		/* read_attr_value() has generated an error string */
		return false;
	}
	state->salt = malloc(pg_b64_dec_len(strlen(encoded_salt)));
	if (state->salt == NULL)
	{
		printfPQExpBuffer(errormessage,
						  libpq_gettext("out of memory\n"));
		return false;
	}
	state->saltlen = pg_b64_decode(encoded_salt,
								   strlen(encoded_salt),
								   state->salt);

	iterations_str = read_attr_value(&input, 'i', errormessage);
	if (iterations_str == NULL)
	{
		/* read_attr_value() has generated an error string */
		return false;
	}
	state->iterations = strtol(iterations_str, &endptr, SCRAM_ITERATION_LEN);
	if (*endptr != '\0' || state->iterations < 1)
	{
		printfPQExpBuffer(errormessage,
		libpq_gettext("malformed SCRAM message (invalid iteration count)\n"));
		return false;
	}

	if (*input != '\0')
		printfPQExpBuffer(errormessage,
						  libpq_gettext("malformed SCRAM message (garbage at end of server-first-message)\n"));

	return true;
}

/*
 * Read the final exchange message coming from the server.
 */
static bool
read_server_final_message(fe_scram_state *state,
						  char *input,
						  PQExpBuffer errormessage)
{
	char	   *encoded_server_proof;
	int			server_proof_len;

	state->server_final_message = strdup(input);
	if (!state->server_final_message)
	{
		printfPQExpBuffer(errormessage,
						  libpq_gettext("out of memory\n"));
		return false;
	}

	/* Check for error result. */
	if (*input == 'e')
	{
		char	   *errmsg = read_attr_value(&input, 'e', errormessage);

		printfPQExpBuffer(errormessage,
		  libpq_gettext("error received from server in SASL exchange: %s\n"),
						  errmsg);
		return false;
	}

	/* Parse the message. */
	encoded_server_proof = read_attr_value(&input, 'v', errormessage);
	if (encoded_server_proof == NULL)
	{
		/* read_attr_value() has generated an error message */
		return false;
	}

	if (*input != '\0')
		printfPQExpBuffer(errormessage,
						  libpq_gettext("malformed SCRAM message (garbage at end of server-final-message)\n"));

	server_proof_len = pg_b64_decode(encoded_server_proof,
									 strlen(encoded_server_proof),
									 state->ServerProof);
	if (server_proof_len != SCRAM_KEY_LEN)
	{
		printfPQExpBuffer(errormessage,
		  libpq_gettext("malformed SCRAM message (invalid server proof)\n"));
		return false;
	}

	return true;
}

/*
 * Calculate the client proof, part of the final exchange message sent
 * by the client.
 */
static void
calculate_client_proof(fe_scram_state *state,
					   const char *client_final_message_without_proof,
					   uint8 *result)
{
	uint8		StoredKey[SCRAM_KEY_LEN];
	uint8		ClientKey[SCRAM_KEY_LEN];
	uint8		ClientSignature[SCRAM_KEY_LEN];
	int			i;
	scram_HMAC_ctx ctx;

	scram_ClientOrServerKey(state->password, state->salt, state->saltlen,
						state->iterations, SCRAM_CLIENT_KEY_NAME, ClientKey);
	scram_H(ClientKey, SCRAM_KEY_LEN, StoredKey);

	scram_HMAC_init(&ctx, StoredKey, SCRAM_KEY_LEN);
	scram_HMAC_update(&ctx,
					  state->client_first_message_bare,
					  strlen(state->client_first_message_bare));
	scram_HMAC_update(&ctx, ",", 1);
	scram_HMAC_update(&ctx,
					  state->server_first_message,
					  strlen(state->server_first_message));
	scram_HMAC_update(&ctx, ",", 1);
	scram_HMAC_update(&ctx,
					  client_final_message_without_proof,
					  strlen(client_final_message_without_proof));
	scram_HMAC_final(ClientSignature, &ctx);

	for (i = 0; i < SCRAM_KEY_LEN; i++)
		result[i] = ClientKey[i] ^ ClientSignature[i];
}

/*
 * Validate the server proof, received as part of the final exchange message
 * received from the server.
 */
static bool
verify_server_proof(fe_scram_state *state)
{
	uint8		ServerSignature[SCRAM_KEY_LEN];
	uint8		ServerKey[SCRAM_KEY_LEN];
	scram_HMAC_ctx ctx;

	scram_ClientOrServerKey(state->password, state->salt, state->saltlen,
							state->iterations, SCRAM_SERVER_KEY_NAME,
							ServerKey);

	/* calculate ServerSignature */
	scram_HMAC_init(&ctx, ServerKey, SCRAM_KEY_LEN);
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

	if (memcmp(ServerSignature, state->ServerProof, SCRAM_KEY_LEN) != 0)
		return false;

	return true;
}

/*
 * Random number generator.
 */
static bool
pg_frontend_random(char *dst, int len)
{
#ifdef HAVE_STRONG_RANDOM
	return pg_strong_random(dst, len);
#else
	int			i;
	char	   *end = dst + len;

	static unsigned short seed[3];
	static int	mypid = 0;

	pglock_thread();

	if (mypid != getpid())
	{
		struct timeval now;

		gettimeofday(&now, NULL);

		seed[0] = now.tv_sec ^ getpid();
		seed[1] = (unsigned short) (now.tv_usec);
		seed[2] = (unsigned short) (now.tv_usec >> 16);
	}

	for (i = 0; dst < end; i++)
	{
		uint32		r;
		int			j;

		/*
		 * pg_jrand48 returns a 32-bit integer.  Fill the next 4 bytes from
		 * it.
		 */
		r = (uint32) pg_jrand48(seed);

		for (j = 0; j < 4 && dst < end; j++)
		{
			*(dst++) = (char) (r & 0xFF);
			r >>= 8;
		}
	}

	pgunlock_thread();

	return true;
#endif
}
