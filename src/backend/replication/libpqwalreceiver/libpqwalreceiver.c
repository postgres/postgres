/*-------------------------------------------------------------------------
 *
 * libpqwalreceiver.c
 *
 * This file contains the libpq-specific parts of walreceiver. It's
 * loaded as a dynamic module to avoid linking the main server binary with
 * libpq.
 *
 * Portions Copyright (c) 2010-2016, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/replication/libpqwalreceiver/libpqwalreceiver.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <sys/time.h>

#include "libpq-fe.h"
#include "access/xlog.h"
#include "miscadmin.h"
#include "replication/walreceiver.h"
#include "utils/builtins.h"

#ifdef HAVE_POLL_H
#include <poll.h>
#endif
#ifdef HAVE_SYS_POLL_H
#include <sys/poll.h>
#endif
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

PG_MODULE_MAGIC;

void		_PG_init(void);

/* Current connection to the primary, if any */
static PGconn *streamConn = NULL;

/* Buffer for currently read records */
static char *recvBuf = NULL;

/* Prototypes for interface functions */
static void libpqrcv_connect(char *conninfo);
static void libpqrcv_identify_system(TimeLineID *primary_tli);
static void libpqrcv_readtimelinehistoryfile(TimeLineID tli, char **filename, char **content, int *len);
static bool libpqrcv_startstreaming(TimeLineID tli, XLogRecPtr startpoint,
						char *slotname);
static void libpqrcv_endstreaming(TimeLineID *next_tli);
static int	libpqrcv_receive(int timeout, char **buffer);
static void libpqrcv_send(const char *buffer, int nbytes);
static void libpqrcv_disconnect(void);

/* Prototypes for private functions */
static bool libpq_select(int timeout_ms);
static PGresult *libpqrcv_PQexec(const char *query);

/*
 * Module load callback
 */
void
_PG_init(void)
{
	/* Tell walreceiver how to reach us */
	if (walrcv_connect != NULL || walrcv_identify_system != NULL ||
		walrcv_readtimelinehistoryfile != NULL ||
		walrcv_startstreaming != NULL || walrcv_endstreaming != NULL ||
		walrcv_receive != NULL || walrcv_send != NULL ||
		walrcv_disconnect != NULL)
		elog(ERROR, "libpqwalreceiver already loaded");
	walrcv_connect = libpqrcv_connect;
	walrcv_identify_system = libpqrcv_identify_system;
	walrcv_readtimelinehistoryfile = libpqrcv_readtimelinehistoryfile;
	walrcv_startstreaming = libpqrcv_startstreaming;
	walrcv_endstreaming = libpqrcv_endstreaming;
	walrcv_receive = libpqrcv_receive;
	walrcv_send = libpqrcv_send;
	walrcv_disconnect = libpqrcv_disconnect;
}

/*
 * Establish the connection to the primary server for XLOG streaming
 */
static void
libpqrcv_connect(char *conninfo)
{
	const char *keys[5];
	const char *vals[5];

	/*
	 * We use the expand_dbname parameter to process the connection string (or
	 * URI), and pass some extra options. The deliberately undocumented
	 * parameter "replication=true" makes it a replication connection. The
	 * database name is ignored by the server in replication mode, but specify
	 * "replication" for .pgpass lookup.
	 */
	keys[0] = "dbname";
	vals[0] = conninfo;
	keys[1] = "replication";
	vals[1] = "true";
	keys[2] = "dbname";
	vals[2] = "replication";
	keys[3] = "fallback_application_name";
	vals[3] = "walreceiver";
	keys[4] = NULL;
	vals[4] = NULL;

	streamConn = PQconnectdbParams(keys, vals, /* expand_dbname = */ true);
	if (PQstatus(streamConn) != CONNECTION_OK)
		ereport(ERROR,
				(errmsg("could not connect to the primary server: %s",
						PQerrorMessage(streamConn))));
}

/*
 * Check that primary's system identifier matches ours, and fetch the current
 * timeline ID of the primary.
 */
static void
libpqrcv_identify_system(TimeLineID *primary_tli)
{
	PGresult   *res;
	char	   *primary_sysid;
	char		standby_sysid[32];

	/*
	 * Get the system identifier and timeline ID as a DataRow message from the
	 * primary server.
	 */
	res = libpqrcv_PQexec("IDENTIFY_SYSTEM");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		PQclear(res);
		ereport(ERROR,
				(errmsg("could not receive database system identifier and timeline ID from "
						"the primary server: %s",
						PQerrorMessage(streamConn))));
	}
	if (PQnfields(res) < 3 || PQntuples(res) != 1)
	{
		int			ntuples = PQntuples(res);
		int			nfields = PQnfields(res);

		PQclear(res);
		ereport(ERROR,
				(errmsg("invalid response from primary server"),
				 errdetail("Could not identify system: got %d rows and %d fields, expected %d rows and %d or more fields.",
						   ntuples, nfields, 3, 1)));
	}
	primary_sysid = PQgetvalue(res, 0, 0);
	*primary_tli = pg_atoi(PQgetvalue(res, 0, 1), 4, 0);

	/*
	 * Confirm that the system identifier of the primary is the same as ours.
	 */
	snprintf(standby_sysid, sizeof(standby_sysid), UINT64_FORMAT,
			 GetSystemIdentifier());
	if (strcmp(primary_sysid, standby_sysid) != 0)
	{
		primary_sysid = pstrdup(primary_sysid);
		PQclear(res);
		ereport(ERROR,
				(errmsg("database system identifier differs between the primary and standby"),
				 errdetail("The primary's identifier is %s, the standby's identifier is %s.",
						   primary_sysid, standby_sysid)));
	}
	PQclear(res);
}

/*
 * Start streaming WAL data from given startpoint and timeline.
 *
 * Returns true if we switched successfully to copy-both mode. False
 * means the server received the command and executed it successfully, but
 * didn't switch to copy-mode.  That means that there was no WAL on the
 * requested timeline and starting point, because the server switched to
 * another timeline at or before the requested starting point. On failure,
 * throws an ERROR.
 */
static bool
libpqrcv_startstreaming(TimeLineID tli, XLogRecPtr startpoint, char *slotname)
{
	char		cmd[256];
	PGresult   *res;

	/* Start streaming from the point requested by startup process */
	if (slotname != NULL)
		snprintf(cmd, sizeof(cmd),
				 "START_REPLICATION SLOT \"%s\" %X/%X TIMELINE %u", slotname,
				 (uint32) (startpoint >> 32), (uint32) startpoint, tli);
	else
		snprintf(cmd, sizeof(cmd),
				 "START_REPLICATION %X/%X TIMELINE %u",
				 (uint32) (startpoint >> 32), (uint32) startpoint, tli);
	res = libpqrcv_PQexec(cmd);

	if (PQresultStatus(res) == PGRES_COMMAND_OK)
	{
		PQclear(res);
		return false;
	}
	else if (PQresultStatus(res) != PGRES_COPY_BOTH)
	{
		PQclear(res);
		ereport(ERROR,
				(errmsg("could not start WAL streaming: %s",
						PQerrorMessage(streamConn))));
	}
	PQclear(res);
	return true;
}

/*
 * Stop streaming WAL data. Returns the next timeline's ID in *next_tli, as
 * reported by the server, or 0 if it did not report it.
 */
static void
libpqrcv_endstreaming(TimeLineID *next_tli)
{
	PGresult   *res;

	if (PQputCopyEnd(streamConn, NULL) <= 0 || PQflush(streamConn))
		ereport(ERROR,
			(errmsg("could not send end-of-streaming message to primary: %s",
					PQerrorMessage(streamConn))));

	/*
	 * After COPY is finished, we should receive a result set indicating the
	 * next timeline's ID, or just CommandComplete if the server was shut
	 * down.
	 *
	 * If we had not yet received CopyDone from the backend, PGRES_COPY_IN
	 * would also be possible. However, at the moment this function is only
	 * called after receiving CopyDone from the backend - the walreceiver
	 * never terminates replication on its own initiative.
	 */
	res = PQgetResult(streamConn);
	if (PQresultStatus(res) == PGRES_TUPLES_OK)
	{
		/*
		 * Read the next timeline's ID. The server also sends the timeline's
		 * starting point, but it is ignored.
		 */
		if (PQnfields(res) < 2 || PQntuples(res) != 1)
			ereport(ERROR,
					(errmsg("unexpected result set after end-of-streaming")));
		*next_tli = pg_atoi(PQgetvalue(res, 0, 0), sizeof(uint32), 0);
		PQclear(res);

		/* the result set should be followed by CommandComplete */
		res = PQgetResult(streamConn);
	}
	else
		*next_tli = 0;

	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		ereport(ERROR,
				(errmsg("error reading result of streaming command: %s",
						PQerrorMessage(streamConn))));
	PQclear(res);

	/* Verify that there are no more results */
	res = PQgetResult(streamConn);
	if (res != NULL)
		ereport(ERROR,
				(errmsg("unexpected result after CommandComplete: %s",
						PQerrorMessage(streamConn))));
}

/*
 * Fetch the timeline history file for 'tli' from primary.
 */
static void
libpqrcv_readtimelinehistoryfile(TimeLineID tli,
								 char **filename, char **content, int *len)
{
	PGresult   *res;
	char		cmd[64];

	/*
	 * Request the primary to send over the history file for given timeline.
	 */
	snprintf(cmd, sizeof(cmd), "TIMELINE_HISTORY %u", tli);
	res = libpqrcv_PQexec(cmd);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		PQclear(res);
		ereport(ERROR,
				(errmsg("could not receive timeline history file from "
						"the primary server: %s",
						PQerrorMessage(streamConn))));
	}
	if (PQnfields(res) != 2 || PQntuples(res) != 1)
	{
		int			ntuples = PQntuples(res);
		int			nfields = PQnfields(res);

		PQclear(res);
		ereport(ERROR,
				(errmsg("invalid response from primary server"),
				 errdetail("Expected 1 tuple with 2 fields, got %d tuples with %d fields.",
						   ntuples, nfields)));
	}
	*filename = pstrdup(PQgetvalue(res, 0, 0));

	*len = PQgetlength(res, 0, 1);
	*content = palloc(*len);
	memcpy(*content, PQgetvalue(res, 0, 1), *len);
	PQclear(res);
}

/*
 * Wait until we can read WAL stream, or timeout.
 *
 * Returns true if data has become available for reading, false if timed out
 * or interrupted by signal.
 *
 * This is based on pqSocketCheck.
 */
static bool
libpq_select(int timeout_ms)
{
	int			ret;

	Assert(streamConn != NULL);
	if (PQsocket(streamConn) < 0)
		ereport(ERROR,
				(errcode_for_socket_access(),
				 errmsg("invalid socket: %s", PQerrorMessage(streamConn))));

	/* We use poll(2) if available, otherwise select(2) */
	{
#ifdef HAVE_POLL
		struct pollfd input_fd;

		input_fd.fd = PQsocket(streamConn);
		input_fd.events = POLLIN | POLLERR;
		input_fd.revents = 0;

		ret = poll(&input_fd, 1, timeout_ms);
#else							/* !HAVE_POLL */

		fd_set		input_mask;
		struct timeval timeout;
		struct timeval *ptr_timeout;

		FD_ZERO(&input_mask);
		FD_SET(PQsocket(streamConn), &input_mask);

		if (timeout_ms < 0)
			ptr_timeout = NULL;
		else
		{
			timeout.tv_sec = timeout_ms / 1000;
			timeout.tv_usec = (timeout_ms % 1000) * 1000;
			ptr_timeout = &timeout;
		}

		ret = select(PQsocket(streamConn) + 1, &input_mask,
					 NULL, NULL, ptr_timeout);
#endif   /* HAVE_POLL */
	}

	if (ret == 0 || (ret < 0 && errno == EINTR))
		return false;
	if (ret < 0)
		ereport(ERROR,
				(errcode_for_socket_access(),
				 errmsg("select() failed: %m")));
	return true;
}

/*
 * Send a query and wait for the results by using the asynchronous libpq
 * functions and the backend version of select().
 *
 * We must not use the regular blocking libpq functions like PQexec()
 * since they are uninterruptible by signals on some platforms, such as
 * Windows.
 *
 * We must also not use vanilla select() here since it cannot handle the
 * signal emulation layer on Windows.
 *
 * The function is modeled on PQexec() in libpq, but only implements
 * those parts that are in use in the walreceiver.
 *
 * Queries are always executed on the connection in streamConn.
 */
static PGresult *
libpqrcv_PQexec(const char *query)
{
	PGresult   *result = NULL;
	PGresult   *lastResult = NULL;

	/*
	 * PQexec() silently discards any prior query results on the connection.
	 * This is not required for walreceiver since it's expected that walsender
	 * won't generate any such junk results.
	 */

	/*
	 * Submit a query. Since we don't use non-blocking mode, this also can
	 * block. But its risk is relatively small, so we ignore that for now.
	 */
	if (!PQsendQuery(streamConn, query))
		return NULL;

	for (;;)
	{
		/*
		 * Receive data until PQgetResult is ready to get the result without
		 * blocking.
		 */
		while (PQisBusy(streamConn))
		{
			/*
			 * We don't need to break down the sleep into smaller increments,
			 * and check for interrupts after each nap, since we can just
			 * elog(FATAL) within SIGTERM signal handler if the signal arrives
			 * in the middle of establishment of replication connection.
			 */
			if (!libpq_select(-1))
				continue;		/* interrupted */
			if (PQconsumeInput(streamConn) == 0)
				return NULL;	/* trouble */
		}

		/*
		 * Emulate the PQexec()'s behavior of returning the last result when
		 * there are many. Since walsender will never generate multiple
		 * results, we skip the concatenation of error messages.
		 */
		result = PQgetResult(streamConn);
		if (result == NULL)
			break;				/* query is complete */

		PQclear(lastResult);
		lastResult = result;

		if (PQresultStatus(lastResult) == PGRES_COPY_IN ||
			PQresultStatus(lastResult) == PGRES_COPY_OUT ||
			PQresultStatus(lastResult) == PGRES_COPY_BOTH ||
			PQstatus(streamConn) == CONNECTION_BAD)
			break;
	}

	return lastResult;
}

/*
 * Disconnect connection to primary, if any.
 */
static void
libpqrcv_disconnect(void)
{
	PQfinish(streamConn);
	streamConn = NULL;
}

/*
 * Receive a message available from XLOG stream, blocking for
 * maximum of 'timeout' ms.
 *
 * Returns:
 *
 *	 If data was received, returns the length of the data. *buffer is set to
 *	 point to a buffer holding the received message. The buffer is only valid
 *	 until the next libpqrcv_* call.
 *
 *	 0 if no data was available within timeout, or wait was interrupted
 *	 by signal.
 *
 *	 -1 if the server ended the COPY.
 *
 * ereports on error.
 */
static int
libpqrcv_receive(int timeout, char **buffer)
{
	int			rawlen;

	if (recvBuf != NULL)
		PQfreemem(recvBuf);
	recvBuf = NULL;

	/* Try to receive a CopyData message */
	rawlen = PQgetCopyData(streamConn, &recvBuf, 1);
	if (rawlen == 0)
	{
		/*
		 * No data available yet. If the caller requested to block, wait for
		 * more data to arrive.
		 */
		if (timeout > 0)
		{
			if (!libpq_select(timeout))
				return 0;
		}

		if (PQconsumeInput(streamConn) == 0)
			ereport(ERROR,
					(errmsg("could not receive data from WAL stream: %s",
							PQerrorMessage(streamConn))));

		/* Now that we've consumed some input, try again */
		rawlen = PQgetCopyData(streamConn, &recvBuf, 1);
		if (rawlen == 0)
			return 0;
	}
	if (rawlen == -1)			/* end-of-streaming or error */
	{
		PGresult   *res;

		res = PQgetResult(streamConn);
		if (PQresultStatus(res) == PGRES_COMMAND_OK ||
			PQresultStatus(res) == PGRES_COPY_IN)
		{
			PQclear(res);
			return -1;
		}
		else
		{
			PQclear(res);
			ereport(ERROR,
					(errmsg("could not receive data from WAL stream: %s",
							PQerrorMessage(streamConn))));
		}
	}
	if (rawlen < -1)
		ereport(ERROR,
				(errmsg("could not receive data from WAL stream: %s",
						PQerrorMessage(streamConn))));

	/* Return received messages to caller */
	*buffer = recvBuf;
	return rawlen;
}

/*
 * Send a message to XLOG stream.
 *
 * ereports on error.
 */
static void
libpqrcv_send(const char *buffer, int nbytes)
{
	if (PQputCopyData(streamConn, buffer, nbytes) <= 0 ||
		PQflush(streamConn))
		ereport(ERROR,
				(errmsg("could not send data to WAL stream: %s",
						PQerrorMessage(streamConn))));
}
