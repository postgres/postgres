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
#include "common/string.h"
#include "dumputils.h"
#include "fe_utils/string_utils.h"
#include "parallel.h"
#include "pg_backup_archiver.h"
#include "pg_backup_db.h"
#include "pg_backup_utils.h"

static void _check_database_version(ArchiveHandle *AH);
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
		pg_fatal("could not get \"server_version\" from libpq");

	AH->public.remoteVersionStr = pg_strdup(remoteversion_str);
	AH->public.remoteVersion = remoteversion;
	if (!AH->archiveRemoteVersion)
		AH->archiveRemoteVersion = AH->public.remoteVersionStr;

	if (remoteversion != PG_VERSION_NUM
		&& (remoteversion < AH->public.minRemoteVersion ||
			remoteversion > AH->public.maxRemoteVersion))
	{
		pg_log_error("aborting because of server version mismatch");
		pg_log_error_detail("server version: %s; %s version: %s",
							remoteversion_str, progname, PG_VERSION);
		exit(1);
	}

	/*
	 * Check if server is in recovery mode, which means we are on a hot
	 * standby.
	 */
	res = ExecuteSqlQueryForSingleRow((Archive *) AH,
									  "SELECT pg_catalog.pg_is_in_recovery()");
	AH->public.isStandby = (strcmp(PQgetvalue(res, 0, 0), "t") == 0);
	PQclear(res);
}

/*
 * Reconnect to the server.  If dbname is not NULL, use that database,
 * else the one associated with the archive handle.
 */
void
ReconnectToServer(ArchiveHandle *AH, const char *dbname)
{
	PGconn	   *oldConn = AH->connection;
	RestoreOptions *ropt = AH->public.ropt;

	/*
	 * Save the dbname, if given, in override_dbname so that it will also
	 * affect any later reconnection attempt.
	 */
	if (dbname)
		ropt->cparams.override_dbname = pg_strdup(dbname);

	/*
	 * Note: we want to establish the new connection, and in particular update
	 * ArchiveHandle's connCancel, before closing old connection.  Otherwise
	 * an ill-timed SIGINT could try to access a dead connection.
	 */
	AH->connection = NULL;		/* dodge error check in ConnectDatabase */

	ConnectDatabase((Archive *) AH, &ropt->cparams, true);

	PQfinish(oldConn);
}

/*
 * Make, or remake, a database connection with the given parameters.
 *
 * The resulting connection handle is stored in AHX->connection.
 *
 * An interactive password prompt is automatically issued if required.
 * We store the results of that in AHX->savedPassword.
 * Note: it's not really all that sensible to use a single-entry password
 * cache if the username keeps changing.  In current usage, however, the
 * username never does change, so one savedPassword is sufficient.
 */
void
ConnectDatabase(Archive *AHX,
				const ConnParams *cparams,
				bool isReconnect)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;
	trivalue	prompt_password;
	char	   *password;
	bool		new_pass;

	if (AH->connection)
		pg_fatal("already connected to a database");

	/* Never prompt for a password during a reconnection */
	prompt_password = isReconnect ? TRI_NO : cparams->promptPassword;

	password = AH->savedPassword;

	if (prompt_password == TRI_YES && password == NULL)
		password = simple_prompt("Password: ", false);

	/*
	 * Start the connection.  Loop until we have a password if requested by
	 * backend.
	 */
	do
	{
		const char *keywords[8];
		const char *values[8];
		int			i = 0;

		/*
		 * If dbname is a connstring, its entries can override the other
		 * values obtained from cparams; but in turn, override_dbname can
		 * override the dbname component of it.
		 */
		keywords[i] = "host";
		values[i++] = cparams->pghost;
		keywords[i] = "port";
		values[i++] = cparams->pgport;
		keywords[i] = "user";
		values[i++] = cparams->username;
		keywords[i] = "password";
		values[i++] = password;
		keywords[i] = "dbname";
		values[i++] = cparams->dbname;
		if (cparams->override_dbname)
		{
			keywords[i] = "dbname";
			values[i++] = cparams->override_dbname;
		}
		keywords[i] = "fallback_application_name";
		values[i++] = progname;
		keywords[i] = NULL;
		values[i++] = NULL;
		Assert(i <= lengthof(keywords));

		new_pass = false;
		AH->connection = PQconnectdbParams(keywords, values, true);

		if (!AH->connection)
			pg_fatal("could not connect to database");

		if (PQstatus(AH->connection) == CONNECTION_BAD &&
			PQconnectionNeedsPassword(AH->connection) &&
			password == NULL &&
			prompt_password != TRI_NO)
		{
			PQfinish(AH->connection);
			password = simple_prompt("Password: ", false);
			new_pass = true;
		}
	} while (new_pass);

	/* check to see that the backend connection was successfully made */
	if (PQstatus(AH->connection) == CONNECTION_BAD)
	{
		if (isReconnect)
			pg_fatal("reconnection failed: %s",
					 PQerrorMessage(AH->connection));
		else
			pg_fatal("%s",
					 PQerrorMessage(AH->connection));
	}

	/* Start strict; later phases may override this. */
	PQclear(ExecuteSqlQueryForSingleRow((Archive *) AH,
										ALWAYS_SECURE_SEARCH_PATH_SQL));

	if (password && password != AH->savedPassword)
		free(password);

	/*
	 * We want to remember connection's actual password, whether or not we got
	 * it by prompting.  So we don't just store the password variable.
	 */
	if (PQconnectionUsedPassword(AH->connection))
	{
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
		 * helpful during pg_fatal().
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
	pg_log_info("%s", message);
}

/* Like pg_fatal(), but with a complaint about a particular query. */
static void
die_on_query_failure(ArchiveHandle *AH, const char *query)
{
	pg_log_error("query failed: %s",
				 PQerrorMessage(AH->connection));
	pg_log_error_detail("Query was: %s", query);
	exit(1);
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
		pg_fatal(ngettext("query returned %d row instead of one: %s",
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
			pg_fatal("error returned by PQputCopyData: %s",
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
			pg_fatal("error returned by PQputCopyEnd: %s",
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

/*
 * Issue per-blob commands for the large object(s) listed in the TocEntry
 *
 * The TocEntry's defn string is assumed to consist of large object OIDs,
 * one per line.  Wrap these in the given SQL command fragments and issue
 * the commands.  (cmdEnd need not include a semicolon.)
 */
void
IssueCommandPerBlob(ArchiveHandle *AH, TocEntry *te,
					const char *cmdBegin, const char *cmdEnd)
{
	/* Make a writable copy of the command string */
	char	   *buf = pg_strdup(te->defn);
	RestoreOptions *ropt = AH->public.ropt;
	char	   *st;
	char	   *en;

	st = buf;
	while ((en = strchr(st, '\n')) != NULL)
	{
		*en++ = '\0';
		ahprintf(AH, "%s%s%s;\n", cmdBegin, st, cmdEnd);

		/* In --transaction-size mode, count each command as an action */
		if (ropt && ropt->txn_size > 0)
		{
			if (++AH->txnCount >= ropt->txn_size)
			{
				if (AH->connection)
				{
					CommitTransaction(&AH->public);
					StartTransaction(&AH->public);
				}
				else
					ahprintf(AH, "COMMIT;\nBEGIN;\n\n");
				AH->txnCount = 0;
			}
		}

		st = en;
	}
	ahprintf(AH, "\n");
	pg_free(buf);
}

/*
 * Process a "LARGE OBJECTS" ACL TocEntry.
 *
 * To save space in the dump file, the TocEntry contains only one copy
 * of the required GRANT/REVOKE commands, written to apply to the first
 * blob in the group (although we do not depend on that detail here).
 * We must expand the text to generate commands for all the blobs listed
 * in the associated BLOB METADATA entry.
 */
void
IssueACLPerBlob(ArchiveHandle *AH, TocEntry *te)
{
	TocEntry   *blobte = getTocEntryByDumpId(AH, te->dependencies[0]);
	char	   *buf;
	char	   *st;
	char	   *st2;
	char	   *en;
	bool		inquotes;

	if (!blobte)
		pg_fatal("could not find entry for ID %d", te->dependencies[0]);
	Assert(strcmp(blobte->desc, "BLOB METADATA") == 0);

	/* Make a writable copy of the ACL commands string */
	buf = pg_strdup(te->defn);

	/*
	 * We have to parse out the commands sufficiently to locate the blob OIDs
	 * and find the command-ending semicolons.  The commands should not
	 * contain anything hard to parse except for double-quoted role names,
	 * which are easy to ignore.  Once we've split apart the first and second
	 * halves of a command, apply IssueCommandPerBlob.  (This means the
	 * updates on the blobs are interleaved if there's multiple commands, but
	 * that should cause no trouble.)
	 */
	inquotes = false;
	st = en = buf;
	st2 = NULL;
	while (*en)
	{
		/* Ignore double-quoted material */
		if (*en == '"')
			inquotes = !inquotes;
		if (inquotes)
		{
			en++;
			continue;
		}
		/* If we found "LARGE OBJECT", that's the end of the first half */
		if (strncmp(en, "LARGE OBJECT ", 13) == 0)
		{
			/* Terminate the first-half string */
			en += 13;
			Assert(isdigit((unsigned char) *en));
			*en++ = '\0';
			/* Skip the rest of the blob OID */
			while (isdigit((unsigned char) *en))
				en++;
			/* Second half starts here */
			Assert(st2 == NULL);
			st2 = en;
		}
		/* If we found semicolon, that's the end of the second half */
		else if (*en == ';')
		{
			/* Terminate the second-half string */
			*en++ = '\0';
			Assert(st2 != NULL);
			/* Issue this command for each blob */
			IssueCommandPerBlob(AH, blobte, st, st2);
			/* For neatness, skip whitespace before the next command */
			while (isspace((unsigned char) *en))
				en++;
			/* Reset for new command */
			st = en;
			st2 = NULL;
		}
		else
			en++;
	}
	pg_free(buf);
}

void
DropLOIfExists(ArchiveHandle *AH, Oid oid)
{
	ahprintf(AH,
			 "SELECT pg_catalog.lo_unlink(oid) "
			 "FROM pg_catalog.pg_largeobject_metadata "
			 "WHERE oid = '%u';\n",
			 oid);
}
