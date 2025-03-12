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
extern PGDLLIMPORT uint32 log_connections;
extern PGDLLIMPORT char *log_connections_string;

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

/*
 * Granular control over which messages to log for the log_connections GUC.
 *
 * RECEIPT, AUTHENTICATION, and AUTHORIZATION are different aspects of
 * connection establishment and backend setup for which we may emit a log
 * message.
 *
 * ALL is a convenience alias equivalent to all of the above aspects.
 *
 * ON is backwards compatibility alias for the connection aspects that were
 * logged in Postgres versions < 18.
 */
typedef enum LogConnectionOption
{
	LOG_CONNECTION_RECEIPT = (1 << 0),
	LOG_CONNECTION_AUTHENTICATION = (1 << 1),
	LOG_CONNECTION_AUTHORIZATION = (1 << 2),
	LOG_CONNECTION_ON =
		LOG_CONNECTION_RECEIPT |
		LOG_CONNECTION_AUTHENTICATION |
		LOG_CONNECTION_AUTHORIZATION,
	LOG_CONNECTION_ALL =
		LOG_CONNECTION_RECEIPT |
		LOG_CONNECTION_AUTHENTICATION |
		LOG_CONNECTION_AUTHORIZATION,
} LogConnectionOption;

extern void BackendMain(const void *startup_data, size_t startup_data_len) pg_attribute_noreturn();

#endif							/* BACKEND_STARTUP_H */
