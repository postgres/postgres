/*-------------------------------------------------------------------------
 *
 * pg_backup_db.c
 *
 *	Implements the basic DB functions used by the archiver.
 *
 * IDENTIFICATION
 *	  src/bin/pg_dump/pg_backup_db.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <unistd.h>
#include <ctype.h>
#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif

#include "common/connect.h"
#include "dumputils.h"
#include "fe_utils/string_utils.h"
#include "parallel.h"
#include "pg_backup_archiver.h"
#include "pg_backup_db.h"
#include "pg_backup_utils.h"

static void _check_database_version(ArchiveHandle *AH);
static PGconn *_connectDB(ArchiveHandle *AH, const char *newdbname, const char *newUser);
static void notice_processor(void *arg, const char *message);

static void
_check_database_version(ArchiveHandle *AH)
{
	const char *remoteversion_str;
	int			remoteversion;
	PGresult   *res;

	remoteversion_str = PQparameterStatus(AH->connection, "server_version");
	remoteversion = PQserverVersion(AH->connection);
	if (remoteversion == 0 || !remoteversion_str)
		fatal("could not get server_version from libpq");

	AH->public.remoteVersionStr = pg_strdup(remoteversion_str);
	AH->public.remoteVersion = remoteversion;
	if (!AH->archiveRemoteVersion)
		AH->archiveRemoteVersion = AH->public.remoteVersionStr;

	if (remoteversion != PG_VERSION_NUM
		&& (remoteversion < AH->public.minRemoteVersion ||
			remoteversion > AH->public.maxRemoteVersion))
	{
		pg_log_error("server version: %s; %s version: %s",
					 remoteversion_str, progname, PG_VERSION);
		fatal("aborting because of server version mismatch");
	}

	/*
	 * When running against 9.0 or later, check if we are in recovery mode,
	 * which means we are on a hot standby.
	 */
	if (remoteversion >= 90000)
	{
		res = ExecuteSqlQueryForSingleRow((Archive *) AH, "SELECT pg_catalog.pg_is_in_recovery()");

		AH->public.isStandby = (strcmp(PQgetvalue(res, 0, 0), "t") == 0);
		PQclear(res);
	}
	else
		AH->public.isStandby = false;
}

/*
 * Reconnect to the server.  If dbname is not NULL, use that database,
 * else the one associated with the archive handle.  If username is
 * not NULL, use that user name, else the one from the handle.
 */
void
ReconnectToServer(ArchiveHandle *AH, const char *dbname, const char *username)
{
	PGconn	   *newConn;
	const char *newdbname;
	const char *newusername;

	if (!dbname)
		newdbname = PQdb(AH->connection);
	else
		newdbname = dbname;

	if (!username)
		newusername = PQuser(AH->connection);
	else
		newusername = username;

	newConn = _connectDB(AH, newdbname, newusername);

	/* Update ArchiveHandle's connCancel before closing old connection */
	set_archive_cancel_info(AH, newConn);

	PQfinish(AH->connection);
	AH->connection = newConn;

	/* Start strict; later phases may override this. */
	PQclear(ExecuteSqlQueryForSingleRow((Archive *) AH,
										ALWAYS_SECURE_SEARCH_PATH_SQL));
}

/*
 * Connect to the db again.
 *
 * Note: it's not really all that sensible to use a single-entry password
 * cache if the username keeps changing.  In current usage, however, the
 * username never does change, so one savedPassword is sufficient.  We do
 * update the cache on the off chance that the password has changed since the
 * start of the run.
 */
static PGconn *
_connectDB(ArchiveHandle *AH, const char *reqdb, const char *requser)
{
	PQExpBufferData connstr;
	PGconn	   *newConn;
	const char *newdb;
	const char *newuser;
	char	   *password;
	char		passbuf[100];
	bool		new_pass;

	if (!reqdb)
		newdb = PQdb(AH->connection);
	else
		newdb = reqdb;

	if (!requser || strlen(requser) == 0)
		newuser = PQuser(AH->connection);
	else
		newuser = requser;

	pg_log_info("connecting to database \"%s\" as user \"%s\"",
				newdb, newuser);

	password = AH->savedPassword;

	if (AH->promptPassword == TRI_YES && password == NULL)
	{
		simple_prompt("Password: ", passbuf, sizeof(passbuf), false);
		password = passbuf;
	}

	initPQExpBuffer(&connstr);
	appendPQExpBufferStr(&connstr, "dbname=");
	appendConnStrVal(&connstr, newdb);

	do
	{
		const char *keywords[7];
		const char *values[7];

		keywords[0] = "host";
		values[0] = PQhost(AH->connection);
		keywords[1] = "port";
		values[1] = PQport(AH->connection);
		keywords[2] = "user";
		values[2] = newuser;
		keywords[3] = "password";
		values[3] = password;
		keywords[4] = "dbname";
		values[4] = connstr.data;
		keywords[5] = "fallback_application_name";
		values[5] = progname;
		keywords[6] = NULL;
		values[6] = NULL;

		new_pass = false;
		newConn = PQconnectdbParams(keywords, values, true);

		if (!newConn)
			fatal("could not reconnect to database");

		if (PQstatus(newConn) == CONNECTION_BAD)
		{
			if (!PQconnectionNeedsPassword(newConn))
				fatal("could not reconnect to database: %s",
					  PQerrorMessage(newConn));
			PQfinish(newConn);

			if (password)
				fprintf(stderr, "Password incorrect\n");

			fprintf(stderr, "Connecting to %s as %s\n",
					newdb, newuser);

			if (AH->promptPassword != TRI_NO)
			{
				simple_prompt("Password: ", passbuf, sizeof(passbuf), false);
				password = passbuf;
			}
			else
				fatal("connection needs password");

			new_pass = true;
		}
	} while (new_pass);

	/*
	 * We want to remember connection's actual password, whether or not we got
	 * it by prompting.  So we don't just store the password variable.
	 */
	if (PQconnectionUsedPassword(newConn))
	{
		if (AH->savedPassword)
			free(AH->savedPassword);
		AH->savedPassword = pg_strdup(PQpass(newConn));
	}

	termPQExpBuffer(&connstr);

	/* check for version mismatch */
	_check_database_version(AH);

	PQsetNoticeProcessor(newConn, notice_processor, NULL);

	return newConn;
}


/*
 * Make a database connection with the given parameters.  The
 * connection handle is returned, the parameters are stored in AHX.
 * An interactive password prompt is automatically issued if required.
 *
 * Note: it's not really all that sensible to use a single-entry password
 * cache if the username keeps changing.  In current usage, however, the
 * username never does change, so one savedPassword is sufficient.
 */
void
ConnectDatabase(Archive *AHX,
				const char *dbname,
				const char *pghost,
				const char *pgport,
				const char *username,
				trivalue prompt_password)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;
	char	   *password;
	char		passbuf[100];
	bool		new_pass;

	if (AH->connection)
		fatal("already connected to a database");

	password = AH->savedPassword;

	if (prompt_password == TRI_YES && password == NULL)
	{
		simple_prompt("Password: ", passbuf, sizeof(passbuf), false);
		password = passbuf;
	}
	AH->promptPassword = prompt_password;

	/*
	 * Start the connection.  Loop until we have a password if requested by
	 * backend.
	 */
	do
	{
		const char *keywords[7];
		const char *values[7];

		keywords[0] = "host";
		values[0] = pghost;
		keywords[1] = "port";
		values[1] = pgport;
		keywords[2] = "user";
		values[2] = username;
		keywords[3] = "password";
		values[3] = password;
		keywords[4] = "dbname";
		values[4] = dbname;
		keywords[5] = "fallback_application_name";
		values[5] = progname;
		keywords[6] = NULL;
		values[6] = NULL;

		new_pass = false;
		AH->connection = PQconnectdbParams(keywords, values, true);

		if (!AH->connection)
			fatal("could not connect to database");

		if (PQstatus(AH->connection) == CONNECTION_BAD &&
			PQconnectionNeedsPassword(AH->connection) &&
			password == NULL &&
			prompt_password != TRI_NO)
		{
			PQfinish(AH->connection);
			simple_prompt("Password: ", passbuf, sizeof(passbuf), false);
			password = passbuf;
			new_pass = true;
		}
	} while (new_pass);

	/* check to see that the backend connection was successfully made */
	if (PQstatus(AH->connection) == CONNECTION_BAD)
		fatal("connection to database \"%s\" failed: %s",
			  PQdb(AH->connection) ? PQdb(AH->connection) : "",
			  PQerrorMessage(AH->connection));

	/* Start strict; later phases may override this. */
	PQclear(ExecuteSqlQueryForSingleRow((Archive *) AH,
										ALWAYS_SECURE_SEARCH_PATH_SQL));

	/*
	 * We want to remember connection's actual password, whether or not we got
	 * it by prompting.  So we don't just store the password variable.
	 */
	if (PQconnectionUsedPassword(AH->connection))
	{
		if (AH->savedPassword)
			free(AH->savedPassword);
		AH->savedPassword = pg_strdup(PQpass(AH->connection));
	}

	/* check for version mismatch */
	_check_database_version(AH);

	PQsetNoticeProcessor(AH->connection, notice_processor, NULL);

	/* arrange for SIGINT to issue a query cancel on this connection */
	set_archive_cancel_info(AH, AH->connection);
}

/*
 * Close the connection to the database and also cancel off the query if we
 * have one running.
 */
void
DisconnectDatabase(Archive *AHX)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;
	char		errbuf[1];

	if (!AH->connection)
		return;

	if (AH->connCancel)
	{
		/*
		 * If we have an active query, send a cancel before closing, ignoring
		 * any errors.  This is of no use for a normal exit, but might be
		 * helpful during fatal().
		 */
		if (PQtransactionStatus(AH->connection) == PQTRANS_ACTIVE)
			(void) PQcancel(AH->connCancel, errbuf, sizeof(errbuf));

		/*
		 * Prevent signal handler from sending a cancel after this.
		 */
		set_archive_cancel_info(AH, NULL);
	}

	PQfinish(AH->connection);
	AH->connection = NULL;
}

PGconn *
GetConnection(Archive *AHX)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;

	return AH->connection;
}

static void
notice_processor(void *arg, const char *message)
{
	pg_log_generic(PG_LOG_INFO, "%s", message);
}

/* Like fatal(), but with a complaint about a particular query. */
static void
die_on_query_failure(ArchiveHandle *AH, const char *query)
{
	pg_log_error("query failed: %s",
				 PQerrorMessage(AH->connection));
	fatal("query was: %s", query);
}

void
ExecuteSqlStatement(Archive *AHX, const char *query)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;
	PGresult   *res;

	res = PQexec(AH->connection, query);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		die_on_query_failure(AH, query);
	PQclear(res);
}

PGresult *
ExecuteSqlQuery(Archive *AHX, const char *query, ExecStatusType status)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;
	PGresult   *res;

	res = PQexec(AH->connection, query);
	if (PQresultStatus(res) != status)
		die_on_query_failure(AH, query);
	return res;
}

/*
 * Execute an SQL query and verify that we got exactly one row back.
 */
PGresult *
ExecuteSqlQueryForSingleRow(Archive *fout, const char *query)
{
	PGresult   *res;
	int			ntups;

	res = ExecuteSqlQuery(fout, query, PGRES_TUPLES_OK);

	/* Expecting a single result only */
	ntups = PQntuples(res);
	if (ntups != 1)
		fatal(ngettext("query returned %d row instead of one: %s",
					   "query returned %d rows instead of one: %s",
					   ntups),
			  ntups, query);

	return res;
}

/*
 * Convenience function to send a query.
 * Monitors result to detect COPY statements
 */
static void
ExecuteSqlCommand(ArchiveHandle *AH, const char *qry, const char *desc)
{
	PGconn	   *conn = AH->connection;
	PGresult   *res;

#ifdef NOT_USED
	fprintf(stderr, "Executing: '%s'\n\n", qry);
#endif
	res = PQexec(conn, qry);

	switch (PQresultStatus(res))
	{
		case PGRES_COMMAND_OK:
		case PGRES_TUPLES_OK:
		case PGRES_EMPTY_QUERY:
			/* A-OK */
			break;
		case PGRES_COPY_IN:
			/* Assume this is an expected result */
			AH->pgCopyIn = true;
			break;
		default:
			/* trouble */
			warn_or_exit_horribly(AH, "%s: %sCommand was: %s",
								  desc, PQerrorMessage(conn), qry);
			break;
	}

	PQclear(res);
}


/*
 * Process non-COPY table data (that is, INSERT commands).
 *
 * The commands have been run together as one long string for compressibility,
 * and we are receiving them in bufferloads with arbitrary boundaries, so we
 * have to locate command boundaries and save partial commands across calls.
 * All state must be kept in AH->sqlparse, not in local variables of this
 * routine.  We assume that AH->sqlparse was filled with zeroes when created.
 *
 * We have to lex the data to the extent of identifying literals and quoted
 * identifiers, so that we can recognize statement-terminating semicolons.
 * We assume that INSERT data will not contain SQL comments, E'' literals,
 * or dollar-quoted strings, so this is much simpler than a full SQL lexer.
 *
 * Note: when restoring from a pre-9.0 dump file, this code is also used to
 * process BLOB COMMENTS data, which has the same problem of containing
 * multiple SQL commands that might be split across bufferloads.  Fortunately,
 * that data won't contain anything complicated to lex either.
 */
static void
ExecuteSimpleCommands(ArchiveHandle *AH, const char *buf, size_t bufLen)
{
	const char *qry = buf;
	const char *eos = buf + bufLen;

	/* initialize command buffer if first time through */
	if (AH->sqlparse.curCmd == NULL)
		AH->sqlparse.curCmd = createPQExpBuffer();

	for (; qry < eos; qry++)
	{
		char		ch = *qry;

		/* For neatness, we skip any newlines between commands */
		if (!(ch == '\n' && AH->sqlparse.curCmd->len == 0))
			appendPQExpBufferChar(AH->sqlparse.curCmd, ch);

		switch (AH->sqlparse.state)
		{
			case SQL_SCAN:		/* Default state == 0, set in _allocAH */
				if (ch == ';')
				{
					/*
					 * We've found the end of a statement. Send it and reset
					 * the buffer.
					 */
					ExecuteSqlCommand(AH, AH->sqlparse.curCmd->data,
									  "could not execute query");
					resetPQExpBuffer(AH->sqlparse.curCmd);
				}
				else if (ch == '\'')
				{
					AH->sqlparse.state = SQL_IN_SINGLE_QUOTE;
					AH->sqlparse.backSlash = false;
				}
				else if (ch == '"')
				{
					AH->sqlparse.state = SQL_IN_DOUBLE_QUOTE;
				}
				break;

			case SQL_IN_SINGLE_QUOTE:
				/* We needn't handle '' specially */
				if (ch == '\'' && !AH->sqlparse.backSlash)
					AH->sqlparse.state = SQL_SCAN;
				else if (ch == '\\' && !AH->public.std_strings)
					AH->sqlparse.backSlash = !AH->sqlparse.backSlash;
				else
					AH->sqlparse.backSlash = false;
				break;

			case SQL_IN_DOUBLE_QUOTE:
				/* We needn't handle "" specially */
				if (ch == '"')
					AH->sqlparse.state = SQL_SCAN;
				break;
		}
	}
}


/*
 * Implement ahwrite() for direct-to-DB restore
 */
int
ExecuteSqlCommandBuf(Archive *AHX, const char *buf, size_t bufLen)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;

	if (AH->outputKind == OUTPUT_COPYDATA)
	{
		/*
		 * COPY data.
		 *
		 * We drop the data on the floor if libpq has failed to enter COPY
		 * mode; this allows us to behave reasonably when trying to continue
		 * after an error in a COPY command.
		 */
		if (AH->pgCopyIn &&
			PQputCopyData(AH->connection, buf, bufLen) <= 0)
			fatal("error returned by PQputCopyData: %s",
				  PQerrorMessage(AH->connection));
	}
	else if (AH->outputKind == OUTPUT_OTHERDATA)
	{
		/*
		 * Table data expressed as INSERT commands; or, in old dump files,
		 * BLOB COMMENTS data (which is expressed as COMMENT ON commands).
		 */
		ExecuteSimpleCommands(AH, buf, bufLen);
	}
	else
	{
		/*
		 * General SQL commands; we assume that commands will not be split
		 * across calls.
		 *
		 * In most cases the data passed to us will be a null-terminated
		 * string, but if it's not, we have to add a trailing null.
		 */
		if (buf[bufLen] == '\0')
			ExecuteSqlCommand(AH, buf, "could not execute query");
		else
		{
			char	   *str = (char *) pg_malloc(bufLen + 1);

			memcpy(str, buf, bufLen);
			str[bufLen] = '\0';
			ExecuteSqlCommand(AH, str, "could not execute query");
			free(str);
		}
	}

	return bufLen;
}

/*
 * Terminate a COPY operation during direct-to-DB restore
 */
void
EndDBCopyMode(Archive *AHX, const char *tocEntryTag)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;

	if (AH->pgCopyIn)
	{
		PGresult   *res;

		if (PQputCopyEnd(AH->connection, NULL) <= 0)
			fatal("error returned by PQputCopyEnd: %s",
				  PQerrorMessage(AH->connection));

		/* Check command status and return to normal libpq state */
		res = PQgetResult(AH->connection);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
			warn_or_exit_horribly(AH, "COPY failed for table \"%s\": %s",
								  tocEntryTag, PQerrorMessage(AH->connection));
		PQclear(res);

		/* Do this to ensure we've pumped libpq back to idle state */
		if (PQgetResult(AH->connection) != NULL)
			pg_log_warning("unexpected extra results during COPY of table \"%s\"",
						   tocEntryTag);

		AH->pgCopyIn = false;
	}
}

void
StartTransaction(Archive *AHX)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;

	ExecuteSqlCommand(AH, "BEGIN", "could not start database transaction");
}

void
CommitTransaction(Archive *AHX)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;

	ExecuteSqlCommand(AH, "COMMIT", "could not commit database transaction");
}

void
DropBlobIfExists(ArchiveHandle *AH, Oid oid)
{
	/*
	 * If we are not restoring to a direct database connection, we have to
	 * guess about how to detect whether the blob exists.  Assume new-style.
	 */
	if (AH->connection == NULL ||
		PQserverVersion(AH->connection) >= 90000)
	{
		ahprintf(AH,
				 "SELECT pg_catalog.lo_unlink(oid) "
				 "FROM pg_catalog.pg_largeobject_metadata "
				 "WHERE oid = '%u';\n",
				 oid);
	}
	else
	{
		/* Restoring to pre-9.0 server, so do it the old way */
		ahprintf(AH,
				 "SELECT CASE WHEN EXISTS("
				 "SELECT 1 FROM pg_catalog.pg_largeobject WHERE loid = '%u'"
				 ") THEN pg_catalog.lo_unlink('%u') END;\n",
				 oid, oid);
	}
}
