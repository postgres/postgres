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

#include "utils/timestamp.h"

/* GUCs */
extern PGDLLIMPORT bool Trace_connection_negotiation;
extern PGDLLIMPORT uint32 log_connections;
extern PGDLLIMPORT char *log_connections_string;

/* Other globals */
extern PGDLLIMPORT struct ConnectionTiming conn_timing;

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

	/*
	 * Time at which the connection client socket is created. Only used for
	 * client and wal sender connections.
	 */
	TimestampTz socket_created;

	/*
	 * Time at which the postmaster initiates process creation -- either
	 * through fork or otherwise. Only used for client and wal sender
	 * connections.
	 */
	TimestampTz fork_started;
} BackendStartupData;

/*
 * Granular control over which messages to log for the log_connections GUC.
 *
 * RECEIPT, AUTHENTICATION, AUTHORIZATION, and SETUP_DURATIONS are different
 * aspects of connection establishment and backend setup for which we may emit
 * a log message.
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
	LOG_CONNECTION_SETUP_DURATIONS = (1 << 3),
	LOG_CONNECTION_ON =
		LOG_CONNECTION_RECEIPT |
		LOG_CONNECTION_AUTHENTICATION |
		LOG_CONNECTION_AUTHORIZATION,
	LOG_CONNECTION_ALL =
		LOG_CONNECTION_RECEIPT |
		LOG_CONNECTION_AUTHENTICATION |
		LOG_CONNECTION_AUTHORIZATION |
		LOG_CONNECTION_SETUP_DURATIONS,
} LogConnectionOption;

/*
 * A collection of timings of various stages of connection establishment and
 * setup for client backends and WAL senders.
 *
 * Used to emit the setup_durations log message for the log_connections GUC.
 */
typedef struct ConnectionTiming
{
	/*
	 * The time at which the client socket is created and the time at which
	 * the connection is fully set up and first ready for query. Together
	 * these represent the total connection establishment and setup time.
	 */
	TimestampTz socket_create;
	TimestampTz ready_for_use;

	/* Time at which process creation was initiated */
	TimestampTz fork_start;

	/* Time at which process creation was completed */
	TimestampTz fork_end;

	/* Time at which authentication started */
	TimestampTz auth_start;

	/* Time at which authentication was finished */
	TimestampTz auth_end;
} ConnectionTiming;

pg_noreturn extern void BackendMain(const void *startup_data, size_t startup_data_len);

#endif							/* BACKEND_STARTUP_H */
