/*-------------------------------------------------------------------------
 *
 * pg_backup_db.c
 *
 *	Implements the basic DB functions used by the archiver.
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/bin/pg_dump/pg_backup_db.c,v 1.50 2003/10/03 20:10:59 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "pg_backup.h"
#include "pg_backup_archiver.h"
#include "pg_backup_db.h"
#include "dumputils.h"

#include <unistd.h>
#include <ctype.h>

#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif

#include "libpq-fe.h"
#include "libpq/libpq-fs.h"
#ifndef HAVE_STRDUP
#include "strdup.h"
#endif

static const char *modulename = gettext_noop("archiver (db)");

static void _check_database_version(ArchiveHandle *AH, bool ignoreVersion);
static PGconn *_connectDB(ArchiveHandle *AH, const char *newdbname, const char *newUser);
static int	_executeSqlCommand(ArchiveHandle *AH, PGconn *conn, PQExpBuffer qry, char *desc);
static void notice_processor(void *arg, const char *message);
static char *_sendSQLLine(ArchiveHandle *AH, char *qry, char *eos);
static char *_sendCopyLine(ArchiveHandle *AH, char *qry, char *eos);


static int
_parse_version(ArchiveHandle *AH, const char *versionString)
{
	int			v;

	v = parse_version(versionString);
	if (v < 0)
		die_horribly(AH, modulename, "could not parse version string \"%s\"\n", versionString);

	return v;
}

static void
_check_database_version(ArchiveHandle *AH, bool ignoreVersion)
{
	int			myversion;
	const char *remoteversion_str;
	int			remoteversion;

	myversion = _parse_version(AH, PG_VERSION);

	remoteversion_str = PQparameterStatus(AH->connection, "server_version");
	if (!remoteversion_str)
		die_horribly(AH, modulename, "could not get server_version from libpq\n");

	remoteversion = _parse_version(AH, remoteversion_str);

	AH->public.remoteVersion = remoteversion;

	if (myversion != remoteversion
		&& (remoteversion < AH->public.minRemoteVersion ||
			remoteversion > AH->public.maxRemoteVersion))
	{
		write_msg(NULL, "server version: %s; %s version: %s\n",
				  remoteversion_str, progname, PG_VERSION);
		if (ignoreVersion)
			write_msg(NULL, "proceeding despite version mismatch\n");
		else
			die_horribly(AH, NULL, "aborting because of version mismatch  (Use the -i option to proceed anyway.)\n");
	}
}

/*
 * Reconnect to the server.  If dbname is not NULL, use that database,
 * else the one associated with the archive handle.  If username is
 * not NULL, use that user name, else the one from the handle.	If
 * both the database and the user and match the existing connection
 * already, nothing will be done.
 *
 * Returns 1 in any case.
 */
int
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

	/* Let's see if the request is already satisfied */
	if (strcmp(newusername, PQuser(AH->connection)) == 0
		&& strcmp(newdbname, PQdb(AH->connection)) == 0)
		return 1;

	newConn = _connectDB(AH, newdbname, newusername);

	PQfinish(AH->connection);
	AH->connection = newConn;

	return 1;
}

/*
 * Connect to the db again.
 */
static PGconn *
_connectDB(ArchiveHandle *AH, const char *reqdb, const char *requser)
{
	int			need_pass;
	PGconn	   *newConn;
	char	   *password = NULL;
	int			badPwd = 0;
	int			noPwd = 0;
	char	   *newdb;
	char	   *newuser;

	if (!reqdb)
		newdb = PQdb(AH->connection);
	else
		newdb = (char *) reqdb;

	if (!requser || (strlen(requser) == 0))
		newuser = PQuser(AH->connection);
	else
		newuser = (char *) requser;

	ahlog(AH, 1, "connecting to database \"%s\" as user \"%s\"\n", newdb, newuser);

	if (AH->requirePassword)
	{
		password = simple_prompt("Password: ", 100, false);
		if (password == NULL)
			die_horribly(AH, modulename, "out of memory\n");
	}

	do
	{
		need_pass = false;
		newConn = PQsetdbLogin(PQhost(AH->connection), PQport(AH->connection),
							   NULL, NULL, newdb,
							   newuser, password);
		if (!newConn)
			die_horribly(AH, modulename, "failed to reconnect to database\n");

		if (PQstatus(newConn) == CONNECTION_BAD)
		{
			noPwd = (strcmp(PQerrorMessage(newConn),
							"fe_sendauth: no password supplied\n") == 0);
			badPwd = (strncmp(PQerrorMessage(newConn),
					"Password authentication failed for user", 39) == 0);

			if (noPwd || badPwd)
			{

				if (badPwd)
					fprintf(stderr, "Password incorrect\n");

				fprintf(stderr, "Connecting to %s as %s\n",
						PQdb(AH->connection), newuser);

				need_pass = true;
				if (password)
					free(password);
				password = simple_prompt("Password: ", 100, false);
			}
			else
				die_horribly(AH, modulename, "could not reconnect to database: %s",
							 PQerrorMessage(newConn));
		}
	} while (need_pass);

	if (password)
		free(password);

	/* check for version mismatch */
	_check_database_version(AH, true);

	PQsetNoticeProcessor(newConn, notice_processor, NULL);

	return newConn;
}


/*
 * Make a database connection with the given parameters.  The
 * connection handle is returned, the parameters are stored in AHX.
 * An interactive password prompt is automatically issued if required.
 */
PGconn *
ConnectDatabase(Archive *AHX,
				const char *dbname,
				const char *pghost,
				const char *pgport,
				const char *username,
				const int reqPwd,
				const int ignoreVersion)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;
	char	   *password = NULL;
	bool		need_pass = false;

	if (AH->connection)
		die_horribly(AH, modulename, "already connected to a database\n");

	if (reqPwd)
	{
		password = simple_prompt("Password: ", 100, false);
		if (password == NULL)
			die_horribly(AH, modulename, "out of memory\n");
		AH->requirePassword = true;
	}
	else
		AH->requirePassword = false;

	/*
	 * Start the connection.  Loop until we have a password if requested
	 * by backend.
	 */
	do
	{
		need_pass = false;
		AH->connection = PQsetdbLogin(pghost, pgport, NULL, NULL,
									  dbname, username, password);

		if (!AH->connection)
			die_horribly(AH, modulename, "failed to connect to database\n");

		if (PQstatus(AH->connection) == CONNECTION_BAD &&
			strcmp(PQerrorMessage(AH->connection), "fe_sendauth: no password supplied\n") == 0 &&
			!feof(stdin))
		{
			PQfinish(AH->connection);
			need_pass = true;
			free(password);
			password = NULL;
			password = simple_prompt("Password: ", 100, false);
		}
	} while (need_pass);

	if (password)
		free(password);

	/* check to see that the backend connection was successfully made */
	if (PQstatus(AH->connection) == CONNECTION_BAD)
		die_horribly(AH, modulename, "connection to database \"%s\" failed: %s",
				   PQdb(AH->connection), PQerrorMessage(AH->connection));

	/* check for version mismatch */
	_check_database_version(AH, ignoreVersion);

	PQsetNoticeProcessor(AH->connection, notice_processor, NULL);

	return AH->connection;
}


static void
notice_processor(void *arg, const char *message)
{
	write_msg(NULL, "%s", message);
}


/* Public interface */
/* Convenience function to send a query. Monitors result to handle COPY statements */
int
ExecuteSqlCommand(ArchiveHandle *AH, PQExpBuffer qry, char *desc, bool use_blob)
{
	if (use_blob)
		return _executeSqlCommand(AH, AH->blobConnection, qry, desc);
	else
		return _executeSqlCommand(AH, AH->connection, qry, desc);
}

/*
 * Handle command execution. This is used to execute a command on more than one connection,
 * but the 'pgCopyIn' setting assumes the COPY commands are ONLY executed on the primary
 * setting...an error will be raised otherwise.
 */
static int
_executeSqlCommand(ArchiveHandle *AH, PGconn *conn, PQExpBuffer qry, char *desc)
{
	PGresult   *res;

	/* fprintf(stderr, "Executing: '%s'\n\n", qry->data); */
	res = PQexec(conn, qry->data);
	if (!res)
		die_horribly(AH, modulename, "%s: no result from server\n", desc);

	if (PQresultStatus(res) != PGRES_COMMAND_OK && PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		if (PQresultStatus(res) == PGRES_COPY_IN)
		{
			if (conn != AH->connection)
				die_horribly(AH, modulename, "COPY command executed in non-primary connection\n");

			AH->pgCopyIn = 1;
		}
		else
			die_horribly(AH, modulename, "%s: %s",
						 desc, PQerrorMessage(AH->connection));
	}

	PQclear(res);

	return strlen(qry->data);
}

/*
 * Used by ExecuteSqlCommandBuf to send one buffered line when running a COPY command.
 */
static char *
_sendCopyLine(ArchiveHandle *AH, char *qry, char *eos)
{
	size_t		loc;			/* Location of next newline */
	int			pos = 0;		/* Current position */
	int			sPos = 0;		/* Last pos of a slash char */
	int			isEnd = 0;

	/* loop to find unquoted newline ending the line of COPY data */
	for (;;)
	{
		loc = strcspn(&qry[pos], "\n") + pos;

		/* If no match, then wait */
		if (loc >= (eos - qry)) /* None found */
		{
			appendBinaryPQExpBuffer(AH->pgCopyBuf, qry, (eos - qry));
			return eos;
		}

		/*
		 * fprintf(stderr, "Found cr at %d, prev char was %c, next was
		 * %c\n", loc, qry[loc-1], qry[loc+1]);
		 */

		/* Count the number of preceding slashes */
		sPos = loc;
		while (sPos > 0 && qry[sPos - 1] == '\\')
			sPos--;

		sPos = loc - sPos;

		/*
		 * If an odd number of preceding slashes, then \n was escaped so
		 * set the next search pos, and loop (if any left).
		 */
		if ((sPos & 1) == 1)
		{
			/* fprintf(stderr, "cr was escaped\n"); */
			pos = loc + 1;
			if (pos >= (eos - qry))
			{
				appendBinaryPQExpBuffer(AH->pgCopyBuf, qry, (eos - qry));
				return eos;
			}
		}
		else
			break;
	}

	/* We found an unquoted newline */
	qry[loc] = '\0';
	appendPQExpBuffer(AH->pgCopyBuf, "%s\n", qry);
	isEnd = (strcmp(AH->pgCopyBuf->data, "\\.\n") == 0);

	/*---------
	 * fprintf(stderr, "Sending '%s' via
	 *		COPY (at end = %d)\n\n", AH->pgCopyBuf->data, isEnd);
	 *---------
	 */

	if (PQputline(AH->connection, AH->pgCopyBuf->data) != 0)
		die_horribly(AH, modulename, "error returned by PQputline\n");

	resetPQExpBuffer(AH->pgCopyBuf);

	/*
	 * fprintf(stderr, "Buffer is '%s'\n", AH->pgCopyBuf->data);
	 */

	if (isEnd)
	{
		if (PQendcopy(AH->connection) != 0)
			die_horribly(AH, modulename, "error returned by PQendcopy\n");

		AH->pgCopyIn = 0;
	}

	return qry + loc + 1;
}

/*
 * Used by ExecuteSqlCommandBuf to send one buffered line of SQL (not data for the copy command).
 */
static char *
_sendSQLLine(ArchiveHandle *AH, char *qry, char *eos)
{
	int			pos = 0;		/* Current position */

	/*
	 * The following is a mini state machine to assess the end of an SQL
	 * statement. It really only needs to parse good SQL, or at least
	 * that's the theory... End-of-statement is assumed to be an unquoted,
	 * un commented semi-colon.
	 */

	/*
	 * fprintf(stderr, "Buffer at start is: '%s'\n\n", AH->sqlBuf->data);
	 */

	for (pos = 0; pos < (eos - qry); pos++)
	{
		appendPQExpBufferChar(AH->sqlBuf, qry[pos]);
		/* fprintf(stderr, " %c",qry[pos]); */

		switch (AH->sqlparse.state)
		{

			case SQL_SCAN:		/* Default state == 0, set in _allocAH */
				if (qry[pos] == ';' && AH->sqlparse.braceDepth == 0)
				{
					/* Send It & reset the buffer */

					/*
					 * fprintf(stderr, "    sending: '%s'\n\n",
					 * AH->sqlBuf->data);
					 */
					ExecuteSqlCommand(AH, AH->sqlBuf, "could not execute query", false);
					resetPQExpBuffer(AH->sqlBuf);
					AH->sqlparse.lastChar = '\0';

					/*
					 * Remove any following newlines - so that embedded
					 * COPY commands don't get a starting newline.
					 */
					pos++;
					for (; pos < (eos - qry) && qry[pos] == '\n'; pos++);

					/* We've got our line, so exit */
					return qry + pos;
				}
				else
				{
					if (qry[pos] == '"' || qry[pos] == '\'')
					{
						/* fprintf(stderr,"[startquote]\n"); */
						AH->sqlparse.state = SQL_IN_QUOTE;
						AH->sqlparse.quoteChar = qry[pos];
						AH->sqlparse.backSlash = 0;
					}
					else if (qry[pos] == '-' && AH->sqlparse.lastChar == '-')
						AH->sqlparse.state = SQL_IN_SQL_COMMENT;
					else if (qry[pos] == '*' && AH->sqlparse.lastChar == '/')
						AH->sqlparse.state = SQL_IN_EXT_COMMENT;
					else if (qry[pos] == '(')
						AH->sqlparse.braceDepth++;
					else if (qry[pos] == ')')
						AH->sqlparse.braceDepth--;

					AH->sqlparse.lastChar = qry[pos];
				}
				break;

			case SQL_IN_SQL_COMMENT:
				if (qry[pos] == '\n')
					AH->sqlparse.state = SQL_SCAN;
				break;

			case SQL_IN_EXT_COMMENT:
				if (AH->sqlparse.lastChar == '*' && qry[pos] == '/')
					AH->sqlparse.state = SQL_SCAN;
				break;

			case SQL_IN_QUOTE:
				if (!AH->sqlparse.backSlash && AH->sqlparse.quoteChar == qry[pos])
				{
					/* fprintf(stderr,"[endquote]\n"); */
					AH->sqlparse.state = SQL_SCAN;
				}
				else
				{

					if (qry[pos] == '\\')
					{
						if (AH->sqlparse.lastChar == '\\')
							AH->sqlparse.backSlash = !AH->sqlparse.backSlash;
						else
							AH->sqlparse.backSlash = 1;
					}
					else
						AH->sqlparse.backSlash = 0;
				}
				break;

		}
		AH->sqlparse.lastChar = qry[pos];
		/* fprintf(stderr, "\n"); */
	}

	/*
	 * If we get here, we've processed entire string with no complete SQL
	 * stmt
	 */
	return eos;
}


/* Convenience function to send one or more queries. Monitors result to handle COPY statements */
int
ExecuteSqlCommandBuf(ArchiveHandle *AH, void *qryv, size_t bufLen)
{
	char	   *qry = (char *) qryv;
	char	   *eos = qry + bufLen;

	/*
	 * fprintf(stderr, "\n\n*****\n
	 * Buffer:\n\n%s\n*******************\n\n", qry);
	 */

	/* Could switch between command and COPY IN mode at each line */
	while (qry < eos)
	{
		if (AH->pgCopyIn)
			qry = _sendCopyLine(AH, qry, eos);
		else
			qry = _sendSQLLine(AH, qry, eos);
	}

	return 1;
}

void
FixupBlobRefs(ArchiveHandle *AH, TocEntry *te)
{
	PQExpBuffer tblName;
	PQExpBuffer tblQry;
	PGresult   *res,
			   *uRes;
	int			i,
				n;

	if (strcmp(te->tag, BLOB_XREF_TABLE) == 0)
		return;

	tblName = createPQExpBuffer();
	tblQry = createPQExpBuffer();

	if (te->namespace && strlen(te->namespace) > 0)
		appendPQExpBuffer(tblName, "%s.",
						  fmtId(te->namespace));
	appendPQExpBuffer(tblName, "%s",
					  fmtId(te->tag));

	appendPQExpBuffer(tblQry,
					  "SELECT a.attname, t.typname FROM "
					  "pg_catalog.pg_attribute a, pg_catalog.pg_type t "
		 "WHERE a.attnum > 0 AND a.attrelid = '%s'::pg_catalog.regclass "
				 "AND a.atttypid = t.oid AND t.typname in ('oid', 'lo')",
					  tblName->data);

	res = PQexec(AH->blobConnection, tblQry->data);
	if (!res)
		die_horribly(AH, modulename, "could not find OID columns of table \"%s\": %s",
					 te->tag, PQerrorMessage(AH->connection));

	if ((n = PQntuples(res)) == 0)
	{
		/* nothing to do */
		ahlog(AH, 1, "no OID type columns in table %s\n", te->tag);
	}

	for (i = 0; i < n; i++)
	{
		char	   *attr;
		char	   *typname;
		bool		typeisoid;

		attr = PQgetvalue(res, i, 0);
		typname = PQgetvalue(res, i, 1);

		typeisoid = (strcmp(typname, "oid") == 0);

		ahlog(AH, 1, "fixing large object cross-references for %s.%s\n",
			  te->tag, attr);

		resetPQExpBuffer(tblQry);

		/*
		 * Note: we use explicit typename() cast style here because if we
		 * are dealing with a dump from a pre-7.3 database containing LO
		 * columns, the dump probably will not have CREATE CAST commands
		 * for lo<->oid conversions.  What it will have is functions,
		 * which we will invoke as functions.
		 */

		/* Can't use fmtId more than once per call... */
		appendPQExpBuffer(tblQry,
						  "UPDATE %s SET %s = ",
						  tblName->data, fmtId(attr));
		if (typeisoid)
			appendPQExpBuffer(tblQry,
							  "%s.newOid",
							  BLOB_XREF_TABLE);
		else
			appendPQExpBuffer(tblQry,
							  "%s(%s.newOid)",
							  fmtId(typname),
							  BLOB_XREF_TABLE);
		appendPQExpBuffer(tblQry,
						  " FROM %s WHERE %s.oldOid = ",
						  BLOB_XREF_TABLE,
						  BLOB_XREF_TABLE);
		if (typeisoid)
			appendPQExpBuffer(tblQry,
							  "%s.%s",
							  tblName->data, fmtId(attr));
		else
			appendPQExpBuffer(tblQry,
							  "oid(%s.%s)",
							  tblName->data, fmtId(attr));

		ahlog(AH, 10, "SQL: %s\n", tblQry->data);

		uRes = PQexec(AH->blobConnection, tblQry->data);
		if (!uRes)
			die_horribly(AH, modulename,
					"could not update column \"%s\" of table \"%s\": %s",
					  attr, te->tag, PQerrorMessage(AH->blobConnection));

		if (PQresultStatus(uRes) != PGRES_COMMAND_OK)
			die_horribly(AH, modulename,
				"error while updating column \"%s\" of table \"%s\": %s",
					  attr, te->tag, PQerrorMessage(AH->blobConnection));

		PQclear(uRes);
	}

	PQclear(res);
	destroyPQExpBuffer(tblName);
	destroyPQExpBuffer(tblQry);
}

/**********
 *	Convenient SQL calls
 **********/
void
CreateBlobXrefTable(ArchiveHandle *AH)
{
	PQExpBuffer qry = createPQExpBuffer();

	/* IF we don't have a BLOB connection, then create one */
	if (!AH->blobConnection)
		AH->blobConnection = _connectDB(AH, NULL, NULL);

	ahlog(AH, 1, "creating table for large object cross-references\n");

	appendPQExpBuffer(qry, "Create Temporary Table %s(oldOid pg_catalog.oid, newOid pg_catalog.oid);", BLOB_XREF_TABLE);

	ExecuteSqlCommand(AH, qry, "could not create large object cross-reference table", true);

	resetPQExpBuffer(qry);

	appendPQExpBuffer(qry, "Create Unique Index %s_ix on %s(oldOid)", BLOB_XREF_TABLE, BLOB_XREF_TABLE);
	ExecuteSqlCommand(AH, qry, "could not create index on large object cross-reference table", true);

	destroyPQExpBuffer(qry);
}

void
InsertBlobXref(ArchiveHandle *AH, Oid old, Oid new)
{
	PQExpBuffer qry = createPQExpBuffer();

	appendPQExpBuffer(qry,
				   "Insert Into %s(oldOid, newOid) Values ('%u', '%u');",
					  BLOB_XREF_TABLE, old, new);

	ExecuteSqlCommand(AH, qry, "could not create large object cross-reference entry", true);

	destroyPQExpBuffer(qry);
}

void
StartTransaction(ArchiveHandle *AH)
{
	PQExpBuffer qry = createPQExpBuffer();

	appendPQExpBuffer(qry, "Begin;");

	ExecuteSqlCommand(AH, qry, "could not start database transaction", false);
	AH->txActive = true;

	destroyPQExpBuffer(qry);
}

void
StartTransactionXref(ArchiveHandle *AH)
{
	PQExpBuffer qry = createPQExpBuffer();

	appendPQExpBuffer(qry, "Begin;");

	ExecuteSqlCommand(AH, qry,
					  "could not start transaction for large object cross-references", true);
	AH->blobTxActive = true;

	destroyPQExpBuffer(qry);
}

void
CommitTransaction(ArchiveHandle *AH)
{
	PQExpBuffer qry = createPQExpBuffer();

	appendPQExpBuffer(qry, "Commit;");

	ExecuteSqlCommand(AH, qry, "could not commit database transaction", false);
	AH->txActive = false;

	destroyPQExpBuffer(qry);
}

void
CommitTransactionXref(ArchiveHandle *AH)
{
	PQExpBuffer qry = createPQExpBuffer();

	appendPQExpBuffer(qry, "Commit;");

	ExecuteSqlCommand(AH, qry, "could not commit transaction for large object cross-references", true);
	AH->blobTxActive = false;

	destroyPQExpBuffer(qry);
}
