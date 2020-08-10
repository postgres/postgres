/*-------------------------------------------------------------------------
 *
 * libpqwalreceiver.c
 *
 * This file contains the libpq-specific parts of walreceiver. It's
 * loaded as a dynamic module to avoid linking the main server binary with
 * libpq.
 *
 * Portions Copyright (c) 2010-2020, PostgreSQL Global Development Group
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

#include "access/xlog.h"
#include "catalog/pg_type.h"
#include "common/connect.h"
#include "funcapi.h"
#include "libpq-fe.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "pqexpbuffer.h"
#include "replication/walreceiver.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/pg_lsn.h"
#include "utils/tuplestore.h"

PG_MODULE_MAGIC;

void		_PG_init(void);

struct WalReceiverConn
{
	/* Current connection to the primary, if any */
	PGconn	   *streamConn;
	/* Used to remember if the connection is logical or physical */
	bool		logical;
	/* Buffer for currently read records */
	char	   *recvBuf;
};

/* Prototypes for interface functions */
static WalReceiverConn *libpqrcv_connect(const char *conninfo,
										 bool logical, const char *appname,
										 char **err);
static void libpqrcv_check_conninfo(const char *conninfo);
static char *libpqrcv_get_conninfo(WalReceiverConn *conn);
static void libpqrcv_get_senderinfo(WalReceiverConn *conn,
									char **sender_host, int *sender_port);
static char *libpqrcv_identify_system(WalReceiverConn *conn,
									  TimeLineID *primary_tli);
static int	libpqrcv_server_version(WalReceiverConn *conn);
static void libpqrcv_readtimelinehistoryfile(WalReceiverConn *conn,
											 TimeLineID tli, char **filename,
											 char **content, int *len);
static bool libpqrcv_startstreaming(WalReceiverConn *conn,
									const WalRcvStreamOptions *options);
static void libpqrcv_endstreaming(WalReceiverConn *conn,
								  TimeLineID *next_tli);
static int	libpqrcv_receive(WalReceiverConn *conn, char **buffer,
							 pgsocket *wait_fd);
static void libpqrcv_send(WalReceiverConn *conn, const char *buffer,
						  int nbytes);
static char *libpqrcv_create_slot(WalReceiverConn *conn,
								  const char *slotname,
								  bool temporary,
								  CRSSnapshotAction snapshot_action,
								  XLogRecPtr *lsn);
static pid_t libpqrcv_get_backend_pid(WalReceiverConn *conn);
static WalRcvExecResult *libpqrcv_exec(WalReceiverConn *conn,
									   const char *query,
									   const int nRetTypes,
									   const Oid *retTypes);
static void libpqrcv_disconnect(WalReceiverConn *conn);

static WalReceiverFunctionsType PQWalReceiverFunctions = {
	libpqrcv_connect,
	libpqrcv_check_conninfo,
	libpqrcv_get_conninfo,
	libpqrcv_get_senderinfo,
	libpqrcv_identify_system,
	libpqrcv_server_version,
	libpqrcv_readtimelinehistoryfile,
	libpqrcv_startstreaming,
	libpqrcv_endstreaming,
	libpqrcv_receive,
	libpqrcv_send,
	libpqrcv_create_slot,
	libpqrcv_get_backend_pid,
	libpqrcv_exec,
	libpqrcv_disconnect
};

/* Prototypes for private functions */
static PGresult *libpqrcv_PQexec(PGconn *streamConn, const char *query);
static PGresult *libpqrcv_PQgetResult(PGconn *streamConn);
static char *stringlist_to_identifierstr(PGconn *conn, List *strings);

/*
 * Module initialization function
 */
void
_PG_init(void)
{
	if (WalReceiverFunctions != NULL)
		elog(ERROR, "libpqwalreceiver already loaded");
	WalReceiverFunctions = &PQWalReceiverFunctions;
}

/*
 * Establish the connection to the primary server for XLOG streaming
 *
 * Returns NULL on error and fills the err with palloc'ed error message.
 */
static WalReceiverConn *
libpqrcv_connect(const char *conninfo, bool logical, const char *appname,
				 char **err)
{
	WalReceiverConn *conn;
	PostgresPollingStatusType status;
	const char *keys[5];
	const char *vals[5];
	int			i = 0;

	/*
	 * We use the expand_dbname parameter to process the connection string (or
	 * URI), and pass some extra options.
	 */
	keys[i] = "dbname";
	vals[i] = conninfo;
	keys[++i] = "replication";
	vals[i] = logical ? "database" : "true";
	if (!logical)
	{
		/*
		 * The database name is ignored by the server in replication mode, but
		 * specify "replication" for .pgpass lookup.
		 */
		keys[++i] = "dbname";
		vals[i] = "replication";
	}
	keys[++i] = "fallback_application_name";
	vals[i] = appname;
	if (logical)
	{
		keys[++i] = "client_encoding";
		vals[i] = GetDatabaseEncodingName();
	}
	keys[++i] = NULL;
	vals[i] = NULL;

	Assert(i < sizeof(keys));

	conn = palloc0(sizeof(WalReceiverConn));
	conn->streamConn = PQconnectStartParams(keys, vals,
											 /* expand_dbname = */ true);
	if (PQstatus(conn->streamConn) == CONNECTION_BAD)
	{
		*err = pchomp(PQerrorMessage(conn->streamConn));
		return NULL;
	}

	/*
	 * Poll connection until we have OK or FAILED status.
	 *
	 * Per spec for PQconnectPoll, first wait till socket is write-ready.
	 */
	status = PGRES_POLLING_WRITING;
	do
	{
		int			io_flag;
		int			rc;

		if (status == PGRES_POLLING_READING)
			io_flag = WL_SOCKET_READABLE;
#ifdef WIN32
		/* Windows needs a different test while waiting for connection-made */
		else if (PQstatus(conn->streamConn) == CONNECTION_STARTED)
			io_flag = WL_SOCKET_CONNECTED;
#endif
		else
			io_flag = WL_SOCKET_WRITEABLE;

		rc = WaitLatchOrSocket(MyLatch,
							   WL_EXIT_ON_PM_DEATH | WL_LATCH_SET | io_flag,
							   PQsocket(conn->streamConn),
							   0,
							   WAIT_EVENT_LIBPQWALRECEIVER_CONNECT);

		/* Interrupted? */
		if (rc & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			ProcessWalRcvInterrupts();
		}

		/* If socket is ready, advance the libpq state machine */
		if (rc & io_flag)
			status = PQconnectPoll(conn->streamConn);
	} while (status != PGRES_POLLING_OK && status != PGRES_POLLING_FAILED);

	if (PQstatus(conn->streamConn) != CONNECTION_OK)
	{
		*err = pchomp(PQerrorMessage(conn->streamConn));
		return NULL;
	}

	if (logical)
	{
		PGresult   *res;

		res = libpqrcv_PQexec(conn->streamConn,
							  ALWAYS_SECURE_SEARCH_PATH_SQL);
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			PQclear(res);
			ereport(ERROR,
					(errmsg("could not clear search path: %s",
							pchomp(PQerrorMessage(conn->streamConn)))));
		}
		PQclear(res);
	}

	conn->logical = logical;

	return conn;
}

/*
 * Validate connection info string (just try to parse it)
 */
static void
libpqrcv_check_conninfo(const char *conninfo)
{
	PQconninfoOption *opts = NULL;
	char	   *err = NULL;

	opts = PQconninfoParse(conninfo, &err);
	if (opts == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("invalid connection string syntax: %s", err)));

	PQconninfoFree(opts);
}

/*
 * Return a user-displayable conninfo string.  Any security-sensitive fields
 * are obfuscated.
 */
static char *
libpqrcv_get_conninfo(WalReceiverConn *conn)
{
	PQconninfoOption *conn_opts;
	PQconninfoOption *conn_opt;
	PQExpBufferData buf;
	char	   *retval;

	Assert(conn->streamConn != NULL);

	initPQExpBuffer(&buf);
	conn_opts = PQconninfo(conn->streamConn);

	if (conn_opts == NULL)
		ereport(ERROR,
				(errmsg("could not parse connection string: %s",
						_("out of memory"))));

	/* build a clean connection string from pieces */
	for (conn_opt = conn_opts; conn_opt->keyword != NULL; conn_opt++)
	{
		bool		obfuscate;

		/* Skip debug and empty options */
		if (strchr(conn_opt->dispchar, 'D') ||
			conn_opt->val == NULL ||
			conn_opt->val[0] == '\0')
			continue;

		/* Obfuscate security-sensitive options */
		obfuscate = strchr(conn_opt->dispchar, '*') != NULL;

		appendPQExpBuffer(&buf, "%s%s=%s",
						  buf.len == 0 ? "" : " ",
						  conn_opt->keyword,
						  obfuscate ? "********" : conn_opt->val);
	}

	PQconninfoFree(conn_opts);

	retval = PQExpBufferDataBroken(buf) ? NULL : pstrdup(buf.data);
	termPQExpBuffer(&buf);
	return retval;
}

/*
 * Provides information of sender this WAL receiver is connected to.
 */
static void
libpqrcv_get_senderinfo(WalReceiverConn *conn, char **sender_host,
						int *sender_port)
{
	char	   *ret = NULL;

	*sender_host = NULL;
	*sender_port = 0;

	Assert(conn->streamConn != NULL);

	ret = PQhost(conn->streamConn);
	if (ret && strlen(ret) != 0)
		*sender_host = pstrdup(ret);

	ret = PQport(conn->streamConn);
	if (ret && strlen(ret) != 0)
		*sender_port = atoi(ret);
}

/*
 * Check that primary's system identifier matches ours, and fetch the current
 * timeline ID of the primary.
 */
static char *
libpqrcv_identify_system(WalReceiverConn *conn, TimeLineID *primary_tli)
{
	PGresult   *res;
	char	   *primary_sysid;

	/*
	 * Get the system identifier and timeline ID as a DataRow message from the
	 * primary server.
	 */
	res = libpqrcv_PQexec(conn->streamConn, "IDENTIFY_SYSTEM");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		PQclear(res);
		ereport(ERROR,
				(errmsg("could not receive database system identifier and timeline ID from "
						"the primary server: %s",
						pchomp(PQerrorMessage(conn->streamConn)))));
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
	primary_sysid = pstrdup(PQgetvalue(res, 0, 0));
	*primary_tli = pg_strtoint32(PQgetvalue(res, 0, 1));
	PQclear(res);

	return primary_sysid;
}

/*
 * Thin wrapper around libpq to obtain server version.
 */
static int
libpqrcv_server_version(WalReceiverConn *conn)
{
	return PQserverVersion(conn->streamConn);
}

/*
 * Start streaming WAL data from given streaming options.
 *
 * Returns true if we switched successfully to copy-both mode. False
 * means the server received the command and executed it successfully, but
 * didn't switch to copy-mode.  That means that there was no WAL on the
 * requested timeline and starting point, because the server switched to
 * another timeline at or before the requested starting point. On failure,
 * throws an ERROR.
 */
static bool
libpqrcv_startstreaming(WalReceiverConn *conn,
						const WalRcvStreamOptions *options)
{
	StringInfoData cmd;
	PGresult   *res;

	Assert(options->logical == conn->logical);
	Assert(options->slotname || !options->logical);

	initStringInfo(&cmd);

	/* Build the command. */
	appendStringInfoString(&cmd, "START_REPLICATION");
	if (options->slotname != NULL)
		appendStringInfo(&cmd, " SLOT \"%s\"",
						 options->slotname);

	if (options->logical)
		appendStringInfoString(&cmd, " LOGICAL");

	appendStringInfo(&cmd, " %X/%X",
					 (uint32) (options->startpoint >> 32),
					 (uint32) options->startpoint);

	/*
	 * Additional options are different depending on if we are doing logical
	 * or physical replication.
	 */
	if (options->logical)
	{
		char	   *pubnames_str;
		List	   *pubnames;
		char	   *pubnames_literal;

		appendStringInfoString(&cmd, " (");

		appendStringInfo(&cmd, "proto_version '%u'",
						 options->proto.logical.proto_version);

		pubnames = options->proto.logical.publication_names;
		pubnames_str = stringlist_to_identifierstr(conn->streamConn, pubnames);
		if (!pubnames_str)
			ereport(ERROR,
					(errmsg("could not start WAL streaming: %s",
							pchomp(PQerrorMessage(conn->streamConn)))));
		pubnames_literal = PQescapeLiteral(conn->streamConn, pubnames_str,
										   strlen(pubnames_str));
		if (!pubnames_literal)
			ereport(ERROR,
					(errmsg("could not start WAL streaming: %s",
							pchomp(PQerrorMessage(conn->streamConn)))));
		appendStringInfo(&cmd, ", publication_names %s", pubnames_literal);
		PQfreemem(pubnames_literal);
		pfree(pubnames_str);

		appendStringInfoChar(&cmd, ')');
	}
	else
		appendStringInfo(&cmd, " TIMELINE %u",
						 options->proto.physical.startpointTLI);

	/* Start streaming. */
	res = libpqrcv_PQexec(conn->streamConn, cmd.data);
	pfree(cmd.data);

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
						pchomp(PQerrorMessage(conn->streamConn)))));
	}
	PQclear(res);
	return true;
}

/*
 * Stop streaming WAL data. Returns the next timeline's ID in *next_tli, as
 * reported by the server, or 0 if it did not report it.
 */
static void
libpqrcv_endstreaming(WalReceiverConn *conn, TimeLineID *next_tli)
{
	PGresult   *res;

	/*
	 * Send copy-end message.  As in libpqrcv_PQexec, this could theoretically
	 * block, but the risk seems small.
	 */
	if (PQputCopyEnd(conn->streamConn, NULL) <= 0 ||
		PQflush(conn->streamConn))
		ereport(ERROR,
				(errmsg("could not send end-of-streaming message to primary: %s",
						pchomp(PQerrorMessage(conn->streamConn)))));

	*next_tli = 0;

	/*
	 * After COPY is finished, we should receive a result set indicating the
	 * next timeline's ID, or just CommandComplete if the server was shut
	 * down.
	 *
	 * If we had not yet received CopyDone from the backend, PGRES_COPY_OUT is
	 * also possible in case we aborted the copy in mid-stream.
	 */
	res = libpqrcv_PQgetResult(conn->streamConn);
	if (PQresultStatus(res) == PGRES_TUPLES_OK)
	{
		/*
		 * Read the next timeline's ID. The server also sends the timeline's
		 * starting point, but it is ignored.
		 */
		if (PQnfields(res) < 2 || PQntuples(res) != 1)
			ereport(ERROR,
					(errmsg("unexpected result set after end-of-streaming")));
		*next_tli = pg_strtoint32(PQgetvalue(res, 0, 0));
		PQclear(res);

		/* the result set should be followed by CommandComplete */
		res = libpqrcv_PQgetResult(conn->streamConn);
	}
	else if (PQresultStatus(res) == PGRES_COPY_OUT)
	{
		PQclear(res);

		/* End the copy */
		if (PQendcopy(conn->streamConn))
			ereport(ERROR,
					(errmsg("error while shutting down streaming COPY: %s",
							pchomp(PQerrorMessage(conn->streamConn)))));

		/* CommandComplete should follow */
		res = libpqrcv_PQgetResult(conn->streamConn);
	}

	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		ereport(ERROR,
				(errmsg("error reading result of streaming command: %s",
						pchomp(PQerrorMessage(conn->streamConn)))));
	PQclear(res);

	/* Verify that there are no more results */
	res = libpqrcv_PQgetResult(conn->streamConn);
	if (res != NULL)
		ereport(ERROR,
				(errmsg("unexpected result after CommandComplete: %s",
						pchomp(PQerrorMessage(conn->streamConn)))));
}

/*
 * Fetch the timeline history file for 'tli' from primary.
 */
static void
libpqrcv_readtimelinehistoryfile(WalReceiverConn *conn,
								 TimeLineID tli, char **filename,
								 char **content, int *len)
{
	PGresult   *res;
	char		cmd[64];

	Assert(!conn->logical);

	/*
	 * Request the primary to send over the history file for given timeline.
	 */
	snprintf(cmd, sizeof(cmd), "TIMELINE_HISTORY %u", tli);
	res = libpqrcv_PQexec(conn->streamConn, cmd);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		PQclear(res);
		ereport(ERROR,
				(errmsg("could not receive timeline history file from "
						"the primary server: %s",
						pchomp(PQerrorMessage(conn->streamConn)))));
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
 * Send a query and wait for the results by using the asynchronous libpq
 * functions and socket readiness events.
 *
 * We must not use the regular blocking libpq functions like PQexec()
 * since they are uninterruptible by signals on some platforms, such as
 * Windows.
 *
 * The function is modeled on PQexec() in libpq, but only implements
 * those parts that are in use in the walreceiver api.
 *
 * May return NULL, rather than an error result, on failure.
 */
static PGresult *
libpqrcv_PQexec(PGconn *streamConn, const char *query)
{
	PGresult   *lastResult = NULL;

	/*
	 * PQexec() silently discards any prior query results on the connection.
	 * This is not required for this function as it's expected that the caller
	 * (which is this library in all cases) will behave correctly and we don't
	 * have to be backwards compatible with old libpq.
	 */

	/*
	 * Submit the query.  Since we don't use non-blocking mode, this could
	 * theoretically block.  In practice, since we don't send very long query
	 * strings, the risk seems negligible.
	 */
	if (!PQsendQuery(streamConn, query))
		return NULL;

	for (;;)
	{
		/* Wait for, and collect, the next PGresult. */
		PGresult   *result;

		result = libpqrcv_PQgetResult(streamConn);
		if (result == NULL)
			break;				/* query is complete, or failure */

		/*
		 * Emulate PQexec()'s behavior of returning the last result when there
		 * are many.  We are fine with returning just last error message.
		 */
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
 * Perform the equivalent of PQgetResult(), but watch for interrupts.
 */
static PGresult *
libpqrcv_PQgetResult(PGconn *streamConn)
{
	/*
	 * Collect data until PQgetResult is ready to get the result without
	 * blocking.
	 */
	while (PQisBusy(streamConn))
	{
		int			rc;

		/*
		 * We don't need to break down the sleep into smaller increments,
		 * since we'll get interrupted by signals and can handle any
		 * interrupts here.
		 */
		rc = WaitLatchOrSocket(MyLatch,
							   WL_EXIT_ON_PM_DEATH | WL_SOCKET_READABLE |
							   WL_LATCH_SET,
							   PQsocket(streamConn),
							   0,
							   WAIT_EVENT_LIBPQWALRECEIVER_RECEIVE);

		/* Interrupted? */
		if (rc & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			ProcessWalRcvInterrupts();
		}

		/* Consume whatever data is available from the socket */
		if (PQconsumeInput(streamConn) == 0)
		{
			/* trouble; return NULL */
			return NULL;
		}
	}

	/* Now we can collect and return the next PGresult */
	return PQgetResult(streamConn);
}

/*
 * Disconnect connection to primary, if any.
 */
static void
libpqrcv_disconnect(WalReceiverConn *conn)
{
	PQfinish(conn->streamConn);
	if (conn->recvBuf != NULL)
		PQfreemem(conn->recvBuf);
	pfree(conn);
}

/*
 * Receive a message available from XLOG stream.
 *
 * Returns:
 *
 *	 If data was received, returns the length of the data. *buffer is set to
 *	 point to a buffer holding the received message. The buffer is only valid
 *	 until the next libpqrcv_* call.
 *
 *	 If no data was available immediately, returns 0, and *wait_fd is set to a
 *	 socket descriptor which can be waited on before trying again.
 *
 *	 -1 if the server ended the COPY.
 *
 * ereports on error.
 */
static int
libpqrcv_receive(WalReceiverConn *conn, char **buffer,
				 pgsocket *wait_fd)
{
	int			rawlen;

	if (conn->recvBuf != NULL)
		PQfreemem(conn->recvBuf);
	conn->recvBuf = NULL;

	/* Try to receive a CopyData message */
	rawlen = PQgetCopyData(conn->streamConn, &conn->recvBuf, 1);
	if (rawlen == 0)
	{
		/* Try consuming some data. */
		if (PQconsumeInput(conn->streamConn) == 0)
			ereport(ERROR,
					(errmsg("could not receive data from WAL stream: %s",
							pchomp(PQerrorMessage(conn->streamConn)))));

		/* Now that we've consumed some input, try again */
		rawlen = PQgetCopyData(conn->streamConn, &conn->recvBuf, 1);
		if (rawlen == 0)
		{
			/* Tell caller to try again when our socket is ready. */
			*wait_fd = PQsocket(conn->streamConn);
			return 0;
		}
	}
	if (rawlen == -1)			/* end-of-streaming or error */
	{
		PGresult   *res;

		res = libpqrcv_PQgetResult(conn->streamConn);
		if (PQresultStatus(res) == PGRES_COMMAND_OK)
		{
			PQclear(res);

			/* Verify that there are no more results. */
			res = libpqrcv_PQgetResult(conn->streamConn);
			if (res != NULL)
			{
				PQclear(res);

				/*
				 * If the other side closed the connection orderly (otherwise
				 * we'd seen an error, or PGRES_COPY_IN) don't report an error
				 * here, but let callers deal with it.
				 */
				if (PQstatus(conn->streamConn) == CONNECTION_BAD)
					return -1;

				ereport(ERROR,
						(errmsg("unexpected result after CommandComplete: %s",
								PQerrorMessage(conn->streamConn))));
			}

			return -1;
		}
		else if (PQresultStatus(res) == PGRES_COPY_IN)
		{
			PQclear(res);
			return -1;
		}
		else
		{
			PQclear(res);
			ereport(ERROR,
					(errmsg("could not receive data from WAL stream: %s",
							pchomp(PQerrorMessage(conn->streamConn)))));
		}
	}
	if (rawlen < -1)
		ereport(ERROR,
				(errmsg("could not receive data from WAL stream: %s",
						pchomp(PQerrorMessage(conn->streamConn)))));

	/* Return received messages to caller */
	*buffer = conn->recvBuf;
	return rawlen;
}

/*
 * Send a message to XLOG stream.
 *
 * ereports on error.
 */
static void
libpqrcv_send(WalReceiverConn *conn, const char *buffer, int nbytes)
{
	if (PQputCopyData(conn->streamConn, buffer, nbytes) <= 0 ||
		PQflush(conn->streamConn))
		ereport(ERROR,
				(errmsg("could not send data to WAL stream: %s",
						pchomp(PQerrorMessage(conn->streamConn)))));
}

/*
 * Create new replication slot.
 * Returns the name of the exported snapshot for logical slot or NULL for
 * physical slot.
 */
static char *
libpqrcv_create_slot(WalReceiverConn *conn, const char *slotname,
					 bool temporary, CRSSnapshotAction snapshot_action,
					 XLogRecPtr *lsn)
{
	PGresult   *res;
	StringInfoData cmd;
	char	   *snapshot;

	initStringInfo(&cmd);

	appendStringInfo(&cmd, "CREATE_REPLICATION_SLOT \"%s\"", slotname);

	if (temporary)
		appendStringInfoString(&cmd, " TEMPORARY");

	if (conn->logical)
	{
		appendStringInfoString(&cmd, " LOGICAL pgoutput");
		switch (snapshot_action)
		{
			case CRS_EXPORT_SNAPSHOT:
				appendStringInfoString(&cmd, " EXPORT_SNAPSHOT");
				break;
			case CRS_NOEXPORT_SNAPSHOT:
				appendStringInfoString(&cmd, " NOEXPORT_SNAPSHOT");
				break;
			case CRS_USE_SNAPSHOT:
				appendStringInfoString(&cmd, " USE_SNAPSHOT");
				break;
		}
	}
	else
	{
		appendStringInfoString(&cmd, " PHYSICAL RESERVE_WAL");
	}

	res = libpqrcv_PQexec(conn->streamConn, cmd.data);
	pfree(cmd.data);

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		PQclear(res);
		ereport(ERROR,
				(errmsg("could not create replication slot \"%s\": %s",
						slotname, pchomp(PQerrorMessage(conn->streamConn)))));
	}

	if (lsn)
		*lsn = DatumGetLSN(DirectFunctionCall1Coll(pg_lsn_in, InvalidOid,
												   CStringGetDatum(PQgetvalue(res, 0, 1))));

	if (!PQgetisnull(res, 0, 2))
		snapshot = pstrdup(PQgetvalue(res, 0, 2));
	else
		snapshot = NULL;

	PQclear(res);

	return snapshot;
}

/*
 * Return PID of remote backend process.
 */
static pid_t
libpqrcv_get_backend_pid(WalReceiverConn *conn)
{
	return PQbackendPID(conn->streamConn);
}

/*
 * Convert tuple query result to tuplestore.
 */
static void
libpqrcv_processTuples(PGresult *pgres, WalRcvExecResult *walres,
					   const int nRetTypes, const Oid *retTypes)
{
	int			tupn;
	int			coln;
	int			nfields = PQnfields(pgres);
	HeapTuple	tuple;
	AttInMetadata *attinmeta;
	MemoryContext rowcontext;
	MemoryContext oldcontext;

	/* Make sure we got expected number of fields. */
	if (nfields != nRetTypes)
		ereport(ERROR,
				(errmsg("invalid query response"),
				 errdetail("Expected %d fields, got %d fields.",
						   nRetTypes, nfields)));

	walres->tuplestore = tuplestore_begin_heap(true, false, work_mem);

	/* Create tuple descriptor corresponding to expected result. */
	walres->tupledesc = CreateTemplateTupleDesc(nRetTypes);
	for (coln = 0; coln < nRetTypes; coln++)
		TupleDescInitEntry(walres->tupledesc, (AttrNumber) coln + 1,
						   PQfname(pgres, coln), retTypes[coln], -1, 0);
	attinmeta = TupleDescGetAttInMetadata(walres->tupledesc);

	/* No point in doing more here if there were no tuples returned. */
	if (PQntuples(pgres) == 0)
		return;

	/* Create temporary context for local allocations. */
	rowcontext = AllocSetContextCreate(CurrentMemoryContext,
									   "libpqrcv query result context",
									   ALLOCSET_DEFAULT_SIZES);

	/* Process returned rows. */
	for (tupn = 0; tupn < PQntuples(pgres); tupn++)
	{
		char	   *cstrs[MaxTupleAttributeNumber];

		ProcessWalRcvInterrupts();

		/* Do the allocations in temporary context. */
		oldcontext = MemoryContextSwitchTo(rowcontext);

		/*
		 * Fill cstrs with null-terminated strings of column values.
		 */
		for (coln = 0; coln < nfields; coln++)
		{
			if (PQgetisnull(pgres, tupn, coln))
				cstrs[coln] = NULL;
			else
				cstrs[coln] = PQgetvalue(pgres, tupn, coln);
		}

		/* Convert row to a tuple, and add it to the tuplestore */
		tuple = BuildTupleFromCStrings(attinmeta, cstrs);
		tuplestore_puttuple(walres->tuplestore, tuple);

		/* Clean up */
		MemoryContextSwitchTo(oldcontext);
		MemoryContextReset(rowcontext);
	}

	MemoryContextDelete(rowcontext);
}

/*
 * Public interface for sending generic queries (and commands).
 *
 * This can only be called from process connected to database.
 */
static WalRcvExecResult *
libpqrcv_exec(WalReceiverConn *conn, const char *query,
			  const int nRetTypes, const Oid *retTypes)
{
	PGresult   *pgres = NULL;
	WalRcvExecResult *walres = palloc0(sizeof(WalRcvExecResult));

	if (MyDatabaseId == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("the query interface requires a database connection")));

	pgres = libpqrcv_PQexec(conn->streamConn, query);

	switch (PQresultStatus(pgres))
	{
		case PGRES_SINGLE_TUPLE:
		case PGRES_TUPLES_OK:
			walres->status = WALRCV_OK_TUPLES;
			libpqrcv_processTuples(pgres, walres, nRetTypes, retTypes);
			break;

		case PGRES_COPY_IN:
			walres->status = WALRCV_OK_COPY_IN;
			break;

		case PGRES_COPY_OUT:
			walres->status = WALRCV_OK_COPY_OUT;
			break;

		case PGRES_COPY_BOTH:
			walres->status = WALRCV_OK_COPY_BOTH;
			break;

		case PGRES_COMMAND_OK:
			walres->status = WALRCV_OK_COMMAND;
			break;

			/* Empty query is considered error. */
		case PGRES_EMPTY_QUERY:
			walres->status = WALRCV_ERROR;
			walres->err = _("empty query");
			break;

		case PGRES_NONFATAL_ERROR:
		case PGRES_FATAL_ERROR:
		case PGRES_BAD_RESPONSE:
			walres->status = WALRCV_ERROR;
			walres->err = pchomp(PQerrorMessage(conn->streamConn));
			break;
	}

	PQclear(pgres);

	return walres;
}

/*
 * Given a List of strings, return it as single comma separated
 * string, quoting identifiers as needed.
 *
 * This is essentially the reverse of SplitIdentifierString.
 *
 * The caller should free the result.
 */
static char *
stringlist_to_identifierstr(PGconn *conn, List *strings)
{
	ListCell   *lc;
	StringInfoData res;
	bool		first = true;

	initStringInfo(&res);

	foreach(lc, strings)
	{
		char	   *val = strVal(lfirst(lc));
		char	   *val_escaped;

		if (first)
			first = false;
		else
			appendStringInfoChar(&res, ',');

		val_escaped = PQescapeIdentifier(conn, val, strlen(val));
		if (!val_escaped)
		{
			free(res.data);
			return NULL;
		}
		appendStringInfoString(&res, val_escaped);
		PQfreemem(val_escaped);
	}

	return res.data;
}
