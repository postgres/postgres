/*-------------------------------------------------------------------------
 *
 * libpq-be-fe-helpers.h
 *	  Helper functions for using libpq in extensions
 *
 * Code built directly into the backend is not allowed to link to libpq
 * directly. Extension code is allowed to use libpq however. However, libpq
 * used in extensions has to be careful not to block inside libpq, otherwise
 * interrupts will not be processed, leading to issues like unresolvable
 * deadlocks. Backend code also needs to take care to acquire/release an
 * external fd for the connection, otherwise fd.c's accounting of fd's is
 * broken.
 *
 * This file provides helper functions to make it easier to comply with these
 * rules. It is a header only library as it needs to be linked into each
 * extension using libpq, and it seems too small to be worth adding a
 * dedicated static library for.
 *
 * TODO: For historical reasons the connections established here are not put
 * into non-blocking mode. That can lead to blocking even when only the async
 * libpq functions are used. This should be fixed.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/libpq-be-fe-helpers.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LIBPQ_BE_FE_HELPERS_H
#define LIBPQ_BE_FE_HELPERS_H

/*
 * Despite the name, BUILDING_DLL is set only when building code directly part
 * of the backend. Which also is where libpq isn't allowed to be
 * used. Obviously this doesn't protect against libpq-fe.h getting included
 * otherwise, but perhaps still protects against a few mistakes...
 */
#ifdef BUILDING_DLL
#error "libpq may not be used code directly built into the backend"
#endif

#include "libpq-fe.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/latch.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"


static inline void libpqsrv_connect_prepare(void);
static inline void libpqsrv_connect_internal(PGconn *conn, uint32 wait_event_info);
static inline PGresult *libpqsrv_get_result_last(PGconn *conn, uint32 wait_event_info);
static inline PGresult *libpqsrv_get_result(PGconn *conn, uint32 wait_event_info);


/*
 * PQconnectdb() wrapper that reserves a file descriptor and processes
 * interrupts during connection establishment.
 *
 * Throws an error if AcquireExternalFD() fails, but does not throw if
 * connection establishment itself fails. Callers need to use PQstatus() to
 * check if connection establishment succeeded.
 */
static inline PGconn *
libpqsrv_connect(const char *conninfo, uint32 wait_event_info)
{
	PGconn	   *conn = NULL;

	libpqsrv_connect_prepare();

	conn = PQconnectStart(conninfo);

	libpqsrv_connect_internal(conn, wait_event_info);

	return conn;
}

/*
 * Like libpqsrv_connect(), except that this is a wrapper for
 * PQconnectdbParams().
  */
static inline PGconn *
libpqsrv_connect_params(const char *const *keywords,
						const char *const *values,
						int expand_dbname,
						uint32 wait_event_info)
{
	PGconn	   *conn = NULL;

	libpqsrv_connect_prepare();

	conn = PQconnectStartParams(keywords, values, expand_dbname);

	libpqsrv_connect_internal(conn, wait_event_info);

	return conn;
}

/*
 * PQfinish() wrapper that additionally releases the reserved file descriptor.
 *
 * It is allowed to call this with a NULL pgconn iff NULL was returned by
 * libpqsrv_connect*.
 */
static inline void
libpqsrv_disconnect(PGconn *conn)
{
	/*
	 * If no connection was established, we haven't reserved an FD for it (or
	 * already released it). This rule makes it easier to write PG_CATCH()
	 * handlers for this facility's users.
	 *
	 * See also libpqsrv_connect_internal().
	 */
	if (conn == NULL)
		return;

	ReleaseExternalFD();
	PQfinish(conn);
}


/* internal helper functions follow */


/*
 * Helper function for all connection establishment functions.
 */
static inline void
libpqsrv_connect_prepare(void)
{
	/*
	 * We must obey fd.c's limit on non-virtual file descriptors.  Assume that
	 * a PGconn represents one long-lived FD.  (Doing this here also ensures
	 * that VFDs are closed if needed to make room.)
	 */
	if (!AcquireExternalFD())
	{
#ifndef WIN32					/* can't write #if within ereport() macro */
		ereport(ERROR,
				(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
				 errmsg("could not establish connection"),
				 errdetail("There are too many open files on the local server."),
				 errhint("Raise the server's \"max_files_per_process\" and/or \"ulimit -n\" limits.")));
#else
		ereport(ERROR,
				(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
				 errmsg("could not establish connection"),
				 errdetail("There are too many open files on the local server."),
				 errhint("Raise the server's \"max_files_per_process\" setting.")));
#endif
	}
}

/*
 * Helper function for all connection establishment functions.
 */
static inline void
libpqsrv_connect_internal(PGconn *conn, uint32 wait_event_info)
{
	/*
	 * With conn == NULL libpqsrv_disconnect() wouldn't release the FD. So do
	 * that here.
	 */
	if (conn == NULL)
	{
		ReleaseExternalFD();
		return;
	}

	/*
	 * Can't wait without a socket. Note that we don't want to close the libpq
	 * connection yet, so callers can emit a useful error.
	 */
	if (PQstatus(conn) == CONNECTION_BAD)
		return;

	/*
	 * WaitLatchOrSocket() can conceivably fail, handle that case here instead
	 * of requiring all callers to do so.
	 */
	PG_TRY();
	{
		PostgresPollingStatusType status;

		/*
		 * Poll connection until we have OK or FAILED status.
		 *
		 * Per spec for PQconnectPoll, first wait till socket is write-ready.
		 */
		status = PGRES_POLLING_WRITING;
		while (status != PGRES_POLLING_OK && status != PGRES_POLLING_FAILED)
		{
			int			io_flag;
			int			rc;

			if (status == PGRES_POLLING_READING)
				io_flag = WL_SOCKET_READABLE;
#ifdef WIN32

			/*
			 * Windows needs a different test while waiting for
			 * connection-made
			 */
			else if (PQstatus(conn) == CONNECTION_STARTED)
				io_flag = WL_SOCKET_CONNECTED;
#endif
			else
				io_flag = WL_SOCKET_WRITEABLE;

			rc = WaitLatchOrSocket(MyLatch,
								   WL_EXIT_ON_PM_DEATH | WL_LATCH_SET | io_flag,
								   PQsocket(conn),
								   0,
								   wait_event_info);

			/* Interrupted? */
			if (rc & WL_LATCH_SET)
			{
				ResetLatch(MyLatch);
				CHECK_FOR_INTERRUPTS();
			}

			/* If socket is ready, advance the libpq state machine */
			if (rc & io_flag)
				status = PQconnectPoll(conn);
		}
	}
	PG_CATCH();
	{
		/*
		 * If an error is thrown here, the callers won't call
		 * libpqsrv_disconnect() with a conn, so release resources
		 * immediately.
		 */
		ReleaseExternalFD();
		PQfinish(conn);

		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * PQexec() wrapper that processes interrupts.
 *
 * Unless PQsetnonblocking(conn, 1) is in effect, this can't process
 * interrupts while pushing the query text to the server.  Consider that
 * setting if query strings can be long relative to TCP buffer size.
 *
 * This has the preconditions of PQsendQuery(), not those of PQexec().  Most
 * notably, PQexec() would silently discard any prior query results.
 */
static inline PGresult *
libpqsrv_exec(PGconn *conn, const char *query, uint32 wait_event_info)
{
	if (!PQsendQuery(conn, query))
		return NULL;
	return libpqsrv_get_result_last(conn, wait_event_info);
}

/*
 * PQexecParams() wrapper that processes interrupts.
 *
 * See notes at libpqsrv_exec().
 */
static inline PGresult *
libpqsrv_exec_params(PGconn *conn,
					 const char *command,
					 int nParams,
					 const Oid *paramTypes,
					 const char *const *paramValues,
					 const int *paramLengths,
					 const int *paramFormats,
					 int resultFormat,
					 uint32 wait_event_info)
{
	if (!PQsendQueryParams(conn, command, nParams, paramTypes, paramValues,
						   paramLengths, paramFormats, resultFormat))
		return NULL;
	return libpqsrv_get_result_last(conn, wait_event_info);
}

/*
 * Like PQexec(), loop over PQgetResult() until it returns NULL or another
 * terminal state.  Return the last non-NULL result or the terminal state.
 */
static inline PGresult *
libpqsrv_get_result_last(PGconn *conn, uint32 wait_event_info)
{
	PGresult   *volatile lastResult = NULL;

	/* In what follows, do not leak any PGresults on an error. */
	PG_TRY();
	{
		for (;;)
		{
			/* Wait for, and collect, the next PGresult. */
			PGresult   *result;

			result = libpqsrv_get_result(conn, wait_event_info);
			if (result == NULL)
				break;			/* query is complete, or failure */

			/*
			 * Emulate PQexec()'s behavior of returning the last result when
			 * there are many.
			 */
			PQclear(lastResult);
			lastResult = result;

			if (PQresultStatus(lastResult) == PGRES_COPY_IN ||
				PQresultStatus(lastResult) == PGRES_COPY_OUT ||
				PQresultStatus(lastResult) == PGRES_COPY_BOTH ||
				PQstatus(conn) == CONNECTION_BAD)
				break;
		}
	}
	PG_CATCH();
	{
		PQclear(lastResult);
		PG_RE_THROW();
	}
	PG_END_TRY();

	return lastResult;
}

/*
 * Perform the equivalent of PQgetResult(), but watch for interrupts.
 */
static inline PGresult *
libpqsrv_get_result(PGconn *conn, uint32 wait_event_info)
{
	/*
	 * Collect data until PQgetResult is ready to get the result without
	 * blocking.
	 */
	while (PQisBusy(conn))
	{
		int			rc;

		rc = WaitLatchOrSocket(MyLatch,
							   WL_EXIT_ON_PM_DEATH | WL_LATCH_SET |
							   WL_SOCKET_READABLE,
							   PQsocket(conn),
							   0,
							   wait_event_info);

		/* Interrupted? */
		if (rc & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}

		/* Consume whatever data is available from the socket */
		if (PQconsumeInput(conn) == 0)
		{
			/* trouble; expect PQgetResult() to return NULL */
			break;
		}
	}

	/* Now we can collect and return the next PGresult */
	return PQgetResult(conn);
}

/*
 * Submit a cancel request to the given connection, waiting only until
 * the given time.
 *
 * We sleep interruptibly until we receive confirmation that the cancel
 * request has been accepted, and if it is, return NULL; if the cancel
 * request fails, return an error message string (which is not to be
 * freed).
 *
 * For other problems (to wit: OOM when strdup'ing an error message from
 * libpq), this function can ereport(ERROR).
 *
 * Note: this function leaks a string's worth of memory when reporting
 * libpq errors.  Make sure to call it in a transient memory context.
 */
static inline const char *
libpqsrv_cancel(PGconn *conn, TimestampTz endtime)
{
	PGcancelConn *cancel_conn;
	const char *error = NULL;

	cancel_conn = PQcancelCreate(conn);
	if (cancel_conn == NULL)
		return "out of memory";

	/* In what follows, do not leak any PGcancelConn on any errors. */

	PG_TRY();
	{
		if (!PQcancelStart(cancel_conn))
		{
			error = pchomp(PQcancelErrorMessage(cancel_conn));
			goto exit;
		}

		for (;;)
		{
			PostgresPollingStatusType pollres;
			TimestampTz now;
			long		cur_timeout;
			int			waitEvents = WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH;

			pollres = PQcancelPoll(cancel_conn);
			if (pollres == PGRES_POLLING_OK)
				break;			/* success! */

			/* If timeout has expired, give up, else get sleep time. */
			now = GetCurrentTimestamp();
			cur_timeout = TimestampDifferenceMilliseconds(now, endtime);
			if (cur_timeout <= 0)
			{
				error = "cancel request timed out";
				break;
			}

			switch (pollres)
			{
				case PGRES_POLLING_READING:
					waitEvents |= WL_SOCKET_READABLE;
					break;
				case PGRES_POLLING_WRITING:
					waitEvents |= WL_SOCKET_WRITEABLE;
					break;
				default:
					error = pchomp(PQcancelErrorMessage(cancel_conn));
					goto exit;
			}

			/* Sleep until there's something to do */
			WaitLatchOrSocket(MyLatch, waitEvents, PQcancelSocket(cancel_conn),
							  cur_timeout, PG_WAIT_CLIENT);

			ResetLatch(MyLatch);

			CHECK_FOR_INTERRUPTS();
		}
exit:	;
	}
	PG_FINALLY();
	{
		PQcancelFinish(cancel_conn);
	}
	PG_END_TRY();

	return error;
}

#endif							/* LIBPQ_BE_FE_HELPERS_H */
