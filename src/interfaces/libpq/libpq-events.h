/*-------------------------------------------------------------------------
 *
 * libpq-events.h
 *	  This file contains definitions that are useful to applications
 *	  that invoke the libpq "events" API, but are not interesting to
 *	  ordinary users of libpq.
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/libpq/libpq-events.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef LIBPQ_EVENTS_H
#define LIBPQ_EVENTS_H

#include "libpq-fe.h"

#ifdef __cplusplus
extern		"C"
{
#endif

/* Callback Event Ids */
typedef enum
{
	PGEVT_REGISTER,
	PGEVT_CONNRESET,
	PGEVT_CONNDESTROY,
	PGEVT_RESULTCREATE,
	PGEVT_RESULTCOPY,
	PGEVT_RESULTDESTROY
} PGEventId;

typedef struct
{
	PGconn	   *conn;
} PGEventRegister;

typedef struct
{
	PGconn	   *conn;
} PGEventConnReset;

typedef struct
{
	PGconn	   *conn;
} PGEventConnDestroy;

typedef struct
{
	PGconn	   *conn;
	PGresult   *result;
} PGEventResultCreate;

typedef struct
{
	const PGresult *src;
	PGresult   *dest;
} PGEventResultCopy;

typedef struct
{
	PGresult   *result;
} PGEventResultDestroy;

typedef int (*PGEventProc) (PGEventId evtId, void *evtInfo, void *passThrough);

/* Registers an event proc with the given PGconn. */
extern int PQregisterEventProc(PGconn *conn, PGEventProc proc,
					const char *name, void *passThrough);

/* Sets the PGconn instance data for the provided proc to data. */
extern int	PQsetInstanceData(PGconn *conn, PGEventProc proc, void *data);

/* Gets the PGconn instance data for the provided proc. */
extern void *PQinstanceData(const PGconn *conn, PGEventProc proc);

/* Sets the PGresult instance data for the provided proc to data. */
extern int	PQresultSetInstanceData(PGresult *result, PGEventProc proc, void *data);

/* Gets the PGresult instance data for the provided proc. */
extern void *PQresultInstanceData(const PGresult *result, PGEventProc proc);

/* Fires RESULTCREATE events for an application-created PGresult. */
extern int	PQfireResultCreateEvents(PGconn *conn, PGresult *res);

#ifdef __cplusplus
}
#endif

#endif   /* LIBPQ_EVENTS_H */
