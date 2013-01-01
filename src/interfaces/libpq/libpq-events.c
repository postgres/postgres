/*-------------------------------------------------------------------------
 *
 * libpq-events.c
 *	  functions for supporting the libpq "events" API
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/libpq-events.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "libpq-fe.h"
#include "libpq-int.h"


/*
 * Registers an event proc with the given PGconn.
 *
 * The same proc can't be registered more than once in a PGconn.  This
 * restriction is required because we use the proc address to identify
 * the event for purposes such as PQinstanceData().
 *
 * The name argument is used within error messages to aid in debugging.
 * A name must be supplied, but it needn't be unique.  The string is
 * copied, so the passed value needn't be long-lived.
 *
 * The passThrough argument is an application specific pointer and can be set
 * to NULL if not required.  It is passed through to the event proc whenever
 * the event proc is called, and is not otherwise touched by libpq.
 *
 * The function returns a non-zero if successful.  If the function fails,
 * zero is returned.
 */
int
PQregisterEventProc(PGconn *conn, PGEventProc proc,
					const char *name, void *passThrough)
{
	int			i;
	PGEventRegister regevt;

	if (!proc || !conn || !name || !*name)
		return FALSE;			/* bad arguments */

	for (i = 0; i < conn->nEvents; i++)
	{
		if (conn->events[i].proc == proc)
			return FALSE;		/* already registered */
	}

	if (conn->nEvents >= conn->eventArraySize)
	{
		PGEvent    *e;
		int			newSize;

		newSize = conn->eventArraySize ? conn->eventArraySize * 2 : 8;
		if (conn->events)
			e = (PGEvent *) realloc(conn->events, newSize * sizeof(PGEvent));
		else
			e = (PGEvent *) malloc(newSize * sizeof(PGEvent));

		if (!e)
			return FALSE;

		conn->eventArraySize = newSize;
		conn->events = e;
	}

	conn->events[conn->nEvents].proc = proc;
	conn->events[conn->nEvents].name = strdup(name);
	if (!conn->events[conn->nEvents].name)
		return FALSE;
	conn->events[conn->nEvents].passThrough = passThrough;
	conn->events[conn->nEvents].data = NULL;
	conn->events[conn->nEvents].resultInitialized = FALSE;
	conn->nEvents++;

	regevt.conn = conn;
	if (!proc(PGEVT_REGISTER, &regevt, passThrough))
	{
		conn->nEvents--;
		free(conn->events[conn->nEvents].name);
		return FALSE;
	}

	return TRUE;
}

/*
 * Set some "instance data" for an event within a PGconn.
 * Returns nonzero on success, zero on failure.
 */
int
PQsetInstanceData(PGconn *conn, PGEventProc proc, void *data)
{
	int			i;

	if (!conn || !proc)
		return FALSE;

	for (i = 0; i < conn->nEvents; i++)
	{
		if (conn->events[i].proc == proc)
		{
			conn->events[i].data = data;
			return TRUE;
		}
	}

	return FALSE;
}

/*
 * Obtain the "instance data", if any, for the event.
 */
void *
PQinstanceData(const PGconn *conn, PGEventProc proc)
{
	int			i;

	if (!conn || !proc)
		return NULL;

	for (i = 0; i < conn->nEvents; i++)
	{
		if (conn->events[i].proc == proc)
			return conn->events[i].data;
	}

	return NULL;
}

/*
 * Set some "instance data" for an event within a PGresult.
 * Returns nonzero on success, zero on failure.
 */
int
PQresultSetInstanceData(PGresult *result, PGEventProc proc, void *data)
{
	int			i;

	if (!result || !proc)
		return FALSE;

	for (i = 0; i < result->nEvents; i++)
	{
		if (result->events[i].proc == proc)
		{
			result->events[i].data = data;
			return TRUE;
		}
	}

	return FALSE;
}

/*
 * Obtain the "instance data", if any, for the event.
 */
void *
PQresultInstanceData(const PGresult *result, PGEventProc proc)
{
	int			i;

	if (!result || !proc)
		return NULL;

	for (i = 0; i < result->nEvents; i++)
		if (result->events[i].proc == proc)
			return result->events[i].data;

	return NULL;
}

/*
 * Fire RESULTCREATE events for an application-created PGresult.
 *
 * The conn argument can be NULL if event procedures won't use it.
 */
int
PQfireResultCreateEvents(PGconn *conn, PGresult *res)
{
	int			i;

	if (!res)
		return FALSE;

	for (i = 0; i < res->nEvents; i++)
	{
		if (!res->events[i].resultInitialized)
		{
			PGEventResultCreate evt;

			evt.conn = conn;
			evt.result = res;
			if (!res->events[i].proc(PGEVT_RESULTCREATE, &evt,
									 res->events[i].passThrough))
				return FALSE;

			res->events[i].resultInitialized = TRUE;
		}
	}

	return TRUE;
}
