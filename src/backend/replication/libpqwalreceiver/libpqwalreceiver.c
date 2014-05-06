/*-------------------------------------------------------------------------
 *
 * libpqwalreceiver.c
 *
 * This file contains the libpq-specific parts of walreceiver. It's
 * loaded as a dynamic module to avoid linking the main server binary with
 * libpq.
 *
 * Portions Copyright (c) 2010-2010, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/replication/libpqwalreceiver/libpqwalreceiver.c,v 1.12 2010/07/06 19:18:57 momjian Exp $
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
static bool libpqrcv_connect(char *conninfo, XLogRecPtr startpoint);
static bool libpqrcv_receive(int timeout, unsigned char *type,
				 char **buffer, int *len);
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
	if (walrcv_connect != NULL || walrcv_receive != NULL ||
		walrcv_disconnect != NULL)
		elog(ERROR, "libpqwalreceiver already loaded");
	walrcv_connect = libpqrcv_connect;
	walrcv_receive = libpqrcv_receive;
	walrcv_disconnect = libpqrcv_disconnect;
}

/*
 * Establish the connection to the primary server for XLOG streaming
 */
static bool
libpqrcv_connect(char *conninfo, XLogRecPtr startpoint)
{
	char		conninfo_repl[MAXCONNINFO + 37];
	char	   *primary_sysid;
	char		standby_sysid[32];
	TimeLineID	primary_tli;
	TimeLineID	standby_tli;
	PGresult   *res;
	char		cmd[64];

	/*
	 * Connect using deliberately undocumented parameter: replication. The
	 * database name is ignored by the server in replication mode, but specify
	 * "replication" for .pgpass lookup.
	 */
	snprintf(conninfo_repl, sizeof(conninfo_repl),
			 "%s dbname=replication replication=true",
			 conninfo);

	streamConn = PQconnectdb(conninfo_repl);
	if (PQstatus(streamConn) != CONNECTION_OK)
		ereport(ERROR,
				(errmsg("could not connect to the primary server: %s",
						PQerrorMessage(streamConn))));

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
	primary_sysid = PQgetvalue(res, 0, 0);
	primary_tli = pg_atoi(PQgetvalue(res, 0, 1), 4, 0);

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

	/*
	 * Confirm that the current timeline of the primary is the same as the
	 * recovery target timeline.
	 */
	standby_tli = GetRecoveryTargetTLI();
	PQclear(res);
	if (primary_tli != standby_tli)
		ereport(ERROR,
				(errmsg("timeline %u of the primary does not match recovery target timeline %u",
						primary_tli, standby_tli)));
	ThisTimeLineID = primary_tli;

	/* Start streaming from the point requested by startup process */
	snprintf(cmd, sizeof(cmd), "START_REPLICATION %X/%X",
			 startpoint.xlogid, startpoint.xrecoff);
	res = libpqrcv_PQexec(cmd);
	if (PQresultStatus(res) != PGRES_COPY_OUT)
	{
		PQclear(res);
		ereport(ERROR,
				(errmsg("could not start WAL streaming: %s",
						PQerrorMessage(streamConn))));
	}
	PQclear(res);

	ereport(LOG,
		(errmsg("streaming replication successfully connected to primary")));

	return true;
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
				 errmsg("socket not open")));

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
 *	 True if data was received. *type, *buffer and *len are set to
 *	 the type of the received data, buffer holding it, and length,
 *	 respectively.
 *
 *	 False if no data was available within timeout, or wait was interrupted
 *	 by signal.
 *
 * The buffer returned is only valid until the next call of this function or
 * libpq_connect/disconnect.
 *
 * ereports on error.
 */
static bool
libpqrcv_receive(int timeout, unsigned char *type, char **buffer, int *len)
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
				return false;
		}

		if (PQconsumeInput(streamConn) == 0)
			ereport(ERROR,
					(errmsg("could not receive data from WAL stream: %s",
							PQerrorMessage(streamConn))));

		/* Now that we've consumed some input, try again */
		rawlen = PQgetCopyData(streamConn, &recvBuf, 1);
		if (rawlen == 0)
			return false;
	}
	if (rawlen == -1)			/* end-of-streaming or error */
	{
		PGresult   *res;

		res = PQgetResult(streamConn);
		if (PQresultStatus(res) == PGRES_COMMAND_OK)
		{
			PQclear(res);
			ereport(ERROR,
					(errmsg("replication terminated by primary server")));
		}
		PQclear(res);
		ereport(ERROR,
				(errmsg("could not receive data from WAL stream: %s",
						PQerrorMessage(streamConn))));
	}
	if (rawlen < -1)
		ereport(ERROR,
				(errmsg("could not receive data from WAL stream: %s",
						PQerrorMessage(streamConn))));

	/* Return received messages to caller */
	*type = *((unsigned char *) recvBuf);
	*buffer = recvBuf + sizeof(*type);
	*len = rawlen - sizeof(*type);

	return true;
}
