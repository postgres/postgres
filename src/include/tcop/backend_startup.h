/*-------------------------------------------------------------------------
 *
 * backend_startup.h
 *	  prototypes for backend_startup.c.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/tcop/backend_startup.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BACKEND_STARTUP_H
#define BACKEND_STARTUP_H

/* GUCs */
extern PGDLLIMPORT bool Trace_connection_negotiation;

/*
 * CAC_state is passed from postmaster to the backend process, to indicate
 * whether the connection should be accepted, or if the process should just
 * send an error to the client and close the connection.  Note that the
 * connection can fail for various reasons even if postmaster passed CAC_OK.
 */
typedef enum CAC_state
{
	CAC_OK,
	CAC_STARTUP,
	CAC_SHUTDOWN,
	CAC_RECOVERY,
	CAC_NOTCONSISTENT,
	CAC_TOOMANY,
} CAC_state;

/* Information passed from postmaster to backend process in 'startup_data' */
typedef struct BackendStartupData
{
	CAC_state	canAcceptConnections;
} BackendStartupData;

extern void BackendMain(const void *startup_data, size_t startup_data_len) pg_attribute_noreturn();

#endif							/* BACKEND_STARTUP_H */
