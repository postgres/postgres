/*-------------------------------------------------------------------------
 *
 * libpq-be-fe-helpers.h
 *	  Helper functions for using libpq in extensions
 *
 * Code built directly into the backend is not allowed to link to libpq
 * directly. Extension code is allowed to use libpq however. However, libpq
 * used in extensions has to be careful to block inside libpq, otherwise
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
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
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
#include "utils/wait_event.h"


static inline void libpqsrv_connect_prepare(void);
static inline void libpqsrv_connect_internal(PGconn *conn, uint32 wait_event_info);


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
				 errhint("Raise the server's max_files_per_process and/or \"ulimit -n\" limits.")));
#else
		ereport(ERROR,
				(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
				 errmsg("could not establish connection"),
				 errdetail("There are too many open files on the local server."),
				 errhint("Raise the server's max_files_per_process setting.")));
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

#endif							/* LIBPQ_BE_FE_HELPERS_H */
