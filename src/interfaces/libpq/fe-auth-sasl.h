/*-------------------------------------------------------------------------
 *
 * fe-auth-sasl.h
 *	  Defines the SASL mechanism interface for libpq.
 *
 * Each SASL mechanism defines a frontend and a backend callback structure.
 * This is not part of the public API for applications.
 *
 * See src/include/libpq/sasl.h for the backend counterpart.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/libpq/fe-auth-sasl.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef FE_AUTH_SASL_H
#define FE_AUTH_SASL_H

#include "libpq-fe.h"

/*
 * Possible states for the SASL exchange, see the comment on exchange for an
 * explanation of these.
 */
typedef enum
{
	SASL_COMPLETE = 0,
	SASL_FAILED,
	SASL_CONTINUE,
} SASLStatus;

/*
 * Frontend SASL mechanism callbacks.
 *
 * To implement a frontend mechanism, declare a pg_fe_sasl_mech struct with
 * appropriate callback implementations, then hook it into conn->sasl during
 * pg_SASL_init()'s mechanism negotiation.
 */
typedef struct pg_fe_sasl_mech
{
	/*-------
	 * init()
	 *
	 * Initializes mechanism-specific state for a connection.  This
	 * callback must return a pointer to its allocated state, which will
	 * be passed as-is as the first argument to the other callbacks.
	 * the free() callback is called to release any state resources.
	 *
	 * If state allocation fails, the implementation should return NULL to
	 * fail the authentication exchange.
	 *
	 * Input parameters:
	 *
	 *   conn:     The connection to the server
	 *
	 *   password: The user's supplied password for the current connection
	 *
	 *   mech:     The mechanism name in use, for implementations that may
	 *			   advertise more than one name (such as *-PLUS variants).
	 *-------
	 */
	void	   *(*init) (PGconn *conn, const char *password, const char *mech);

	/*--------
	 * exchange()
	 *
	 * Produces a client response to a server challenge.  As a special case
	 * for client-first SASL mechanisms, exchange() is called with a NULL
	 * server response once at the start of the authentication exchange to
	 * generate an initial response. Returns a SASLStatus indicating the
	 * state and status of the exchange.
	 *
	 * Input parameters:
	 *
	 *	state:	   The opaque mechanism state returned by init()
	 *
	 *	input:	   The challenge data sent by the server, or NULL when
	 *			   generating a client-first initial response (that is, when
	 *			   the server expects the client to send a message to start
	 *			   the exchange).  This is guaranteed to be null-terminated
	 *			   for safety, but SASL allows embedded nulls in challenges,
	 *			   so mechanisms must be careful to check inputlen.
	 *
	 *	inputlen:  The length of the challenge data sent by the server, or -1
	 *             during client-first initial response generation.
	 *
	 * Output parameters, to be set by the callback function:
	 *
	 *	output:	   A malloc'd buffer containing the client's response to
	 *			   the server (can be empty), or NULL if the exchange should
	 *			   be aborted.  (The callback should return SASL_FAILED in the
	 *			   latter case.)
	 *
	 *	outputlen: The length (0 or higher) of the client response buffer,
	 *			   ignored if output is NULL.
	 *
	 * Return value:
	 *
	 *	SASL_CONTINUE:	The output buffer is filled with a client response.
	 *					Additional server challenge is expected
	 *	SASL_COMPLETE:	The SASL exchange has completed successfully.
	 *	SASL_FAILED:	The exchange has failed and the connection should be
	 *					dropped.
	 *--------
	 */
	SASLStatus	(*exchange) (void *state, char *input, int inputlen,
							 char **output, int *outputlen);

	/*--------
	 * channel_bound()
	 *
	 * Returns true if the connection has an established channel binding.  A
	 * mechanism implementation must ensure that a SASL exchange has actually
	 * been completed, in addition to checking that channel binding is in use.
	 *
	 * Mechanisms that do not implement channel binding may simply return
	 * false.
	 *
	 * Input parameters:
	 *
	 *	state:    The opaque mechanism state returned by init()
	 *--------
	 */
	bool		(*channel_bound) (void *state);

	/*--------
	 * free()
	 *
	 * Frees the state allocated by init(). This is called when the connection
	 * is dropped, not when the exchange is completed.
	 *
	 * Input parameters:
	 *
	 *   state:    The opaque mechanism state returned by init()
	 *--------
	 */
	void		(*free) (void *state);

} pg_fe_sasl_mech;

#endif							/* FE_AUTH_SASL_H */
