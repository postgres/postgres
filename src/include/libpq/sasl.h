/*-------------------------------------------------------------------------
 *
 * sasl.h
 *	  Defines the SASL mechanism interface for the backend.
 *
 * Each SASL mechanism defines a frontend and a backend callback structure.
 *
 * See src/interfaces/libpq/fe-auth-sasl.h for the frontend counterpart.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/sasl.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PG_SASL_H
#define PG_SASL_H

#include "lib/stringinfo.h"
#include "libpq/libpq-be.h"

/* Status codes for message exchange */
#define PG_SASL_EXCHANGE_CONTINUE		0
#define PG_SASL_EXCHANGE_SUCCESS		1
#define PG_SASL_EXCHANGE_FAILURE		2

/*
 * Maximum accepted size of SASL messages.
 *
 * The messages that the server or libpq generate are much smaller than this,
 * but have some headroom.
 */
#define PG_MAX_SASL_MESSAGE_LENGTH	1024

/*
 * Backend SASL mechanism callbacks and metadata.
 *
 * To implement a backend mechanism, declare a pg_be_sasl_mech struct with
 * appropriate callback implementations.  Then pass the mechanism to
 * CheckSASLAuth() during ClientAuthentication(), once the server has decided
 * which authentication method to use.
 */
typedef struct pg_be_sasl_mech
{
	/*---------
	 * get_mechanisms()
	 *
	 * Retrieves the list of SASL mechanism names supported by this
	 * implementation.
	 *
	 * Input parameters:
	 *
	 *	port: The client Port
	 *
	 * Output parameters:
	 *
	 *	buf:  A StringInfo buffer that the callback should populate with
	 *		  supported mechanism names.  The names are appended into this
	 *		  StringInfo, each one ending with '\0' bytes.
	 *---------
	 */
	void		(*get_mechanisms) (Port *port, StringInfo buf);

	/*---------
	 * init()
	 *
	 * Initializes mechanism-specific state for a connection. This callback
	 * must return a pointer to its allocated state, which will be passed
	 * as-is as the first argument to the other callbacks.
	 *
	 * Input parameters:
	 *
	 *	port:        The client Port.
	 *
	 *	mech:        The actual mechanism name in use by the client.
	 *
	 *	shadow_pass: The stored secret for the role being authenticated, or
	 *				 NULL if one does not exist.  Mechanisms that do not use
	 *				 shadow entries may ignore this parameter.  If a
	 *				 mechanism uses shadow entries but shadow_pass is NULL,
	 *				 the implementation must continue the exchange as if the
	 *				 user existed and the password did not match, to avoid
	 *				 disclosing valid user names.
	 *---------
	 */
	void	   *(*init) (Port *port, const char *mech, const char *shadow_pass);

	/*---------
	 * exchange()
	 *
	 * Produces a server challenge to be sent to the client.  The callback
	 * must return one of the PG_SASL_EXCHANGE_* values, depending on
	 * whether the exchange continues, has finished successfully, or has
	 * failed.
	 *
	 * Input parameters:
	 *
	 *	state:	  The opaque mechanism state returned by init()
	 *
	 *	input:	  The response data sent by the client, or NULL if the
	 *			  mechanism is client-first but the client did not send an
	 *			  initial response.  (This can only happen during the first
	 *			  message from the client.)  This is guaranteed to be
	 *			  null-terminated for safety, but SASL allows embedded
	 *			  nulls in responses, so mechanisms must be careful to
	 *            check inputlen.
	 *
	 *	inputlen: The length of the challenge data sent by the server, or
	 *			  -1 if the client did not send an initial response
	 *
	 * Output parameters, to be set by the callback function:
	 *
	 *	output:    A palloc'd buffer containing either the server's next
	 *			   challenge (if PG_SASL_EXCHANGE_CONTINUE is returned) or
	 *			   the server's outcome data (if PG_SASL_EXCHANGE_SUCCESS is
	 *			   returned and the mechanism requires data to be sent during
	 *			   a successful outcome).  The callback should set this to
	 *			   NULL if the exchange is over and no output should be sent,
	 *			   which should correspond to either PG_SASL_EXCHANGE_FAILURE
	 *			   or a PG_SASL_EXCHANGE_SUCCESS with no outcome data.
	 *
	 *  outputlen: The length of the challenge data.  Ignored if *output is
	 *			   NULL.
	 *
	 *	logdetail: Set to an optional DETAIL message to be printed to the
	 *			   server log, to disambiguate failure modes.  (The client
	 *			   will only ever see the same generic authentication
	 *			   failure message.) Ignored if the exchange is completed
	 *			   with PG_SASL_EXCHANGE_SUCCESS.
	 *---------
	 */
	int			(*exchange) (void *state,
							 const char *input, int inputlen,
							 char **output, int *outputlen,
							 const char **logdetail);

	/* The maximum size allowed for client SASLResponses. */
	int			max_message_length;
} pg_be_sasl_mech;

/* Common implementation for auth.c */
extern int	CheckSASLAuth(const pg_be_sasl_mech *mech, Port *port,
						  char *shadow_pass, const char **logdetail);

#endif							/* PG_SASL_H */
