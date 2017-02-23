/*-------------------------------------------------------------------------
 *
 * libpqwalreceiver.c
 *
 * This file contains the libpq-specific parts of walreceiver. It's
 * loaded as a dynamic module to avoid linking the main server binary with
 * libpq.
 *
 * Portions Copyright (c) 2010-2017, PostgreSQL Global Development Group
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
#include "pqexpbuffer.h"
#include "access/xlog.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "replication/logicalproto.h"
#include "replication/walreceiver.h"
#include "storage/proc.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"

PG_MODULE_MAGIC;

void		_PG_init(void);

struct WalReceiverConn
{
	/* Current connection to the primary, if any */
	PGconn *streamConn;
	/* Used to remember if the connection is logical or physical */
	bool	logical;
	/* Buffer for currently read records */
	char   *recvBuf;
};

/* Prototypes for interface functions */
static WalReceiverConn *libpqrcv_connect(const char *conninfo,
										 bool logical, const char *appname,
										 char **err);
static void libpqrcv_check_conninfo(const char *conninfo);
static char *libpqrcv_get_conninfo(WalReceiverConn *conn);
static char *libpqrcv_identify_system(WalReceiverConn *conn,
									  TimeLineID *primary_tli,
									  int *server_version);
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
								  XLogRecPtr *lsn);
static bool libpqrcv_command(WalReceiverConn *conn,
							 const char *cmd, char **err);
static void libpqrcv_disconnect(WalReceiverConn *conn);

static WalReceiverFunctionsType PQWalReceiverFunctions = {
	libpqrcv_connect,
	libpqrcv_check_conninfo,
	libpqrcv_get_conninfo,
	libpqrcv_identify_system,
	libpqrcv_readtimelinehistoryfile,
	libpqrcv_startstreaming,
	libpqrcv_endstreaming,
	libpqrcv_receive,
	libpqrcv_send,
	libpqrcv_create_slot,
	libpqrcv_command,
	libpqrcv_disconnect
};

/* Prototypes for private functions */
static PGresult *libpqrcv_PQexec(PGconn *streamConn, const char *query);
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
	const char *keys[5];
	const char *vals[5];
	int			i = 0;

	/*
	 * We use the expand_dbname parameter to process the connection string (or
	 * URI), and pass some extra options. The deliberately undocumented
	 * parameter "replication=true" makes it a replication connection. The
	 * database name is ignored by the server in replication mode, but specify
	 * "replication" for .pgpass lookup.
	 */
	keys[i] = "dbname";
	vals[i] = conninfo;
	keys[++i] = "replication";
	vals[i] = logical ? "database" : "true";
	if (!logical)
	{
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
	conn->streamConn = PQconnectdbParams(keys, vals, /* expand_dbname = */ true);
	if (PQstatus(conn->streamConn) != CONNECTION_OK)
	{
		*err = pstrdup(PQerrorMessage(conn->streamConn));
		return NULL;
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
	PQconninfoOption   *opts = NULL;
	char			   *err = NULL;

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
 * Check that primary's system identifier matches ours, and fetch the current
 * timeline ID of the primary.
 */
static char *
libpqrcv_identify_system(WalReceiverConn *conn, TimeLineID *primary_tli,
						 int *server_version)
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
						PQerrorMessage(conn->streamConn))));
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
	*primary_tli = pg_atoi(PQgetvalue(res, 0, 1), 4, 0);
	PQclear(res);

	*server_version = PQserverVersion(conn->streamConn);

	return primary_sysid;
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
		appendStringInfo(&cmd, " LOGICAL");

	appendStringInfo(&cmd, " %X/%X",
					 (uint32) (options->startpoint >> 32),
					 (uint32) options->startpoint);

	/*
	 * Additional options are different depending on if we are doing logical
	 * or physical replication.
	 */
	if (options->logical)
	{
		char   *pubnames_str;
		List   *pubnames;
		char   *pubnames_literal;

		appendStringInfoString(&cmd, " (");

		appendStringInfo(&cmd, "proto_version '%u'",
						 options->proto.logical.proto_version);

		pubnames = options->proto.logical.publication_names;
		pubnames_str = stringlist_to_identifierstr(conn->streamConn, pubnames);
		if (!pubnames_str)
			ereport(ERROR,
					(errmsg("could not start WAL streaming: %s",
							PQerrorMessage(conn->streamConn))));
		pubnames_literal = PQescapeLiteral(conn->streamConn, pubnames_str,
										   strlen(pubnames_str));
		if (!pubnames_literal)
			ereport(ERROR,
					(errmsg("could not start WAL streaming: %s",
							PQerrorMessage(conn->streamConn))));
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
						PQerrorMessage(conn->streamConn))));
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

	if (PQputCopyEnd(conn->streamConn, NULL) <= 0 ||
		PQflush(conn->streamConn))
		ereport(ERROR,
			(errmsg("could not send end-of-streaming message to primary: %s",
					PQerrorMessage(conn->streamConn))));

	*next_tli = 0;

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
	res = PQgetResult(conn->streamConn);
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
		res = PQgetResult(conn->streamConn);
	}
	else if (PQresultStatus(res) == PGRES_COPY_OUT)
	{
		PQclear(res);

		/* End the copy */
		PQendcopy(conn->streamConn);

		/* CommandComplete should follow */
		res = PQgetResult(conn->streamConn);
	}

	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		ereport(ERROR,
				(errmsg("error reading result of streaming command: %s",
						PQerrorMessage(conn->streamConn))));
	PQclear(res);

	/* Verify that there are no more results */
	res = PQgetResult(conn->streamConn);
	if (res != NULL)
		ereport(ERROR,
				(errmsg("unexpected result after CommandComplete: %s",
						PQerrorMessage(conn->streamConn))));
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
						PQerrorMessage(conn->streamConn))));
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
 * those parts that are in use in the walreceiver.
 *
 * Queries are always executed on the connection in streamConn.
 */
static PGresult *
libpqrcv_PQexec(PGconn *streamConn, const char *query)
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
			int			rc;

			/*
			 * We don't need to break down the sleep into smaller increments,
			 * since we'll get interrupted by signals and can either handle
			 * interrupts here or elog(FATAL) within SIGTERM signal handler if
			 * the signal arrives in the middle of establishment of
			 * replication connection.
			 */
			ResetLatch(&MyProc->procLatch);
			rc = WaitLatchOrSocket(&MyProc->procLatch,
								   WL_POSTMASTER_DEATH | WL_SOCKET_READABLE |
								   WL_LATCH_SET,
								   PQsocket(streamConn),
								   0,
								   WAIT_EVENT_LIBPQWALRECEIVER_READ);
			if (rc & WL_POSTMASTER_DEATH)
				exit(1);

			/* interrupted */
			if (rc & WL_LATCH_SET)
			{
				CHECK_FOR_INTERRUPTS();
				continue;
			}
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
							PQerrorMessage(conn->streamConn))));

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

		res = PQgetResult(conn->streamConn);
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
							PQerrorMessage(conn->streamConn))));
		}
	}
	if (rawlen < -1)
		ereport(ERROR,
				(errmsg("could not receive data from WAL stream: %s",
						PQerrorMessage(conn->streamConn))));

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
						PQerrorMessage(conn->streamConn))));
}

/*
 * Create new replication slot.
 * Returns the name of the exported snapshot for logical slot or NULL for
 * physical slot.
 */
static char *
libpqrcv_create_slot(WalReceiverConn *conn, const char *slotname,
					 bool temporary, XLogRecPtr *lsn)
{
	PGresult	   *res;
	StringInfoData	cmd;
	char		   *snapshot;

	initStringInfo(&cmd);

	appendStringInfo(&cmd, "CREATE_REPLICATION_SLOT \"%s\" ", slotname);

	if (temporary)
		appendStringInfo(&cmd, "TEMPORARY ");

	if (conn->logical)
		appendStringInfo(&cmd, "LOGICAL pgoutput");

	res = libpqrcv_PQexec(conn->streamConn, cmd.data);
	pfree(cmd.data);

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		PQclear(res);
		ereport(ERROR,
				(errmsg("could not create replication slot \"%s\": %s",
						slotname, PQerrorMessage(conn->streamConn))));
	}

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
 * Run command.
 *
 * Returns if the command has succeeded and fills the err with palloced
 * error message if not.
 */
static bool
libpqrcv_command(WalReceiverConn *conn, const char *cmd, char **err)
{
	PGresult	   *res;

	res = libpqrcv_PQexec(conn->streamConn, cmd);

	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		PQclear(res);
		*err = pstrdup(PQerrorMessage(conn->streamConn));
		return false;
	}

	PQclear(res);

	return true;
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
	ListCell *lc;
	StringInfoData res;
	bool first = true;

	initStringInfo(&res);

	foreach (lc, strings)
	{
		char *val = strVal(lfirst(lc));
		char *val_escaped;

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
