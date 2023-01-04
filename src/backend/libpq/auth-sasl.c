/*-------------------------------------------------------------------------
 *
 * auth-sasl.c
 *	  Routines to handle authentication via SASL
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/libpq/auth-sasl.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq/auth.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "libpq/sasl.h"

/*
 * Maximum accepted size of SASL messages.
 *
 * The messages that the server or libpq generate are much smaller than this,
 * but have some headroom.
 */
#define PG_MAX_SASL_MESSAGE_LENGTH	1024

/*
 * Perform a SASL exchange with a libpq client, using a specific mechanism
 * implementation.
 *
 * shadow_pass is an optional pointer to the stored secret of the role
 * authenticated, from pg_authid.rolpassword.  For mechanisms that use
 * shadowed passwords, a NULL pointer here means that an entry could not
 * be found for the role (or the user does not exist), and the mechanism
 * should fail the authentication exchange.
 *
 * Mechanisms must take care not to reveal to the client that a user entry
 * does not exist; ideally, the external failure mode is identical to that
 * of an incorrect password.  Mechanisms may instead use the logdetail
 * output parameter to internally differentiate between failure cases and
 * assist debugging by the server admin.
 *
 * A mechanism is not required to utilize a shadow entry, or even a password
 * system at all; for these cases, shadow_pass may be ignored and the caller
 * should just pass NULL.
 */
int
CheckSASLAuth(const pg_be_sasl_mech *mech, Port *port, char *shadow_pass,
			  const char **logdetail)
{
	StringInfoData sasl_mechs;
	int			mtype;
	StringInfoData buf;
	void	   *opaq = NULL;
	char	   *output = NULL;
	int			outputlen = 0;
	const char *input;
	int			inputlen;
	int			result;
	bool		initial;

	/*
	 * Send the SASL authentication request to user.  It includes the list of
	 * authentication mechanisms that are supported.
	 */
	initStringInfo(&sasl_mechs);

	mech->get_mechanisms(port, &sasl_mechs);
	/* Put another '\0' to mark that list is finished. */
	appendStringInfoChar(&sasl_mechs, '\0');

	sendAuthRequest(port, AUTH_REQ_SASL, sasl_mechs.data, sasl_mechs.len);
	pfree(sasl_mechs.data);

	/*
	 * Loop through SASL message exchange.  This exchange can consist of
	 * multiple messages sent in both directions.  First message is always
	 * from the client.  All messages from client to server are password
	 * packets (type 'p').
	 */
	initial = true;
	do
	{
		pq_startmsgread();
		mtype = pq_getbyte();
		if (mtype != 'p')
		{
			/* Only log error if client didn't disconnect. */
			if (mtype != EOF)
			{
				ereport(ERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("expected SASL response, got message type %d",
								mtype)));
			}
			else
				return STATUS_EOF;
		}

		/* Get the actual SASL message */
		initStringInfo(&buf);
		if (pq_getmessage(&buf, PG_MAX_SASL_MESSAGE_LENGTH))
		{
			/* EOF - pq_getmessage already logged error */
			pfree(buf.data);
			return STATUS_ERROR;
		}

		elog(DEBUG4, "processing received SASL response of length %d", buf.len);

		/*
		 * The first SASLInitialResponse message is different from the others.
		 * It indicates which SASL mechanism the client selected, and contains
		 * an optional Initial Client Response payload.  The subsequent
		 * SASLResponse messages contain just the SASL payload.
		 */
		if (initial)
		{
			const char *selected_mech;

			selected_mech = pq_getmsgrawstring(&buf);

			/*
			 * Initialize the status tracker for message exchanges.
			 *
			 * If the user doesn't exist, or doesn't have a valid password, or
			 * it's expired, we still go through the motions of SASL
			 * authentication, but tell the authentication method that the
			 * authentication is "doomed". That is, it's going to fail, no
			 * matter what.
			 *
			 * This is because we don't want to reveal to an attacker what
			 * usernames are valid, nor which users have a valid password.
			 */
			opaq = mech->init(port, selected_mech, shadow_pass);

			inputlen = pq_getmsgint(&buf, 4);
			if (inputlen == -1)
				input = NULL;
			else
				input = pq_getmsgbytes(&buf, inputlen);

			initial = false;
		}
		else
		{
			inputlen = buf.len;
			input = pq_getmsgbytes(&buf, buf.len);
		}
		pq_getmsgend(&buf);

		/*
		 * The StringInfo guarantees that there's a \0 byte after the
		 * response.
		 */
		Assert(input == NULL || input[inputlen] == '\0');

		/*
		 * Hand the incoming message to the mechanism implementation.
		 */
		result = mech->exchange(opaq, input, inputlen,
								&output, &outputlen,
								logdetail);

		/* input buffer no longer used */
		pfree(buf.data);

		if (output)
		{
			/*
			 * PG_SASL_EXCHANGE_FAILURE with some output is forbidden by SASL.
			 * Make sure here that the mechanism used got that right.
			 */
			if (result == PG_SASL_EXCHANGE_FAILURE)
				elog(ERROR, "output message found after SASL exchange failure");

			/*
			 * Negotiation generated data to be sent to the client.
			 */
			elog(DEBUG4, "sending SASL challenge of length %d", outputlen);

			if (result == PG_SASL_EXCHANGE_SUCCESS)
				sendAuthRequest(port, AUTH_REQ_SASL_FIN, output, outputlen);
			else
				sendAuthRequest(port, AUTH_REQ_SASL_CONT, output, outputlen);

			pfree(output);
		}
	} while (result == PG_SASL_EXCHANGE_CONTINUE);

	/* Oops, Something bad happened */
	if (result != PG_SASL_EXCHANGE_SUCCESS)
	{
		return STATUS_ERROR;
	}

	return STATUS_OK;
}
