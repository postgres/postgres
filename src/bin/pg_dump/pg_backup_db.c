/*-------------------------------------------------------------------------
 *
 * pg_backup_db.c
 *
 *	Implements the basic DB functions used by the archiver.
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/bin/pg_dump/pg_backup_db.c,v 1.75.2.1 2008/08/16 02:25:17 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "pg_backup_db.h"
#include "dumputils.h"

#include <unistd.h>

#include <ctype.h>

#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif


static const char *modulename = gettext_noop("archiver (db)");

static void _check_database_version(ArchiveHandle *AH, bool ignoreVersion);
static PGconn *_connectDB(ArchiveHandle *AH, const char *newdbname, const char *newUser);
static void notice_processor(void *arg, const char *message);
static char *_sendSQLLine(ArchiveHandle *AH, char *qry, char *eos);
static char *_sendCopyLine(ArchiveHandle *AH, char *qry, char *eos);

static bool _isIdentChar(unsigned char c);
static bool _isDQChar(unsigned char c, bool atStart);

#define DB_MAX_ERR_STMT 128

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

	AH->public.remoteVersionStr = strdup(remoteversion_str);
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
 * both the database and the user match the existing connection already,
 * nothing will be done.
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
	if (strcmp(newdbname, PQdb(AH->connection)) == 0 &&
		strcmp(newusername, PQuser(AH->connection)) == 0)
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
							PQnoPasswordSupplied) == 0);
			badPwd = (strncmp(PQerrorMessage(newConn),
						"Password authentication failed for user", 39) == 0);

			if (noPwd || badPwd)
			{
				if (badPwd)
					fprintf(stderr, "Password incorrect\n");

				fprintf(stderr, "Connecting to %s as %s\n",
						newdb, newuser);

				need_pass = true;
				if (password)
					free(password);
				password = simple_prompt("Password: ", 100, false);
			}
			else
				die_horribly(AH, modulename, "could not reconnect to database: %s",
							 PQerrorMessage(newConn));
			PQfinish(newConn);
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
	 * Start the connection.  Loop until we have a password if requested by
	 * backend.
	 */
	do
	{
		need_pass = false;
		AH->connection = PQsetdbLogin(pghost, pgport, NULL, NULL,
									  dbname, username, password);

		if (!AH->connection)
			die_horribly(AH, modulename, "failed to connect to database\n");

		if (PQstatus(AH->connection) == CONNECTION_BAD &&
		 strcmp(PQerrorMessage(AH->connection), PQnoPasswordSupplied) == 0 &&
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
static void
ExecuteSqlCommand(ArchiveHandle *AH, const char *qry, const char *desc)
{
	PGconn	   *conn = AH->connection;
	PGresult   *res;
	char		errStmt[DB_MAX_ERR_STMT];

#ifdef NOT_USED
	 fprintf(stderr, "Executing: '%s'\n\n", qry);
#endif
	res = PQexec(conn, qry);

	switch (PQresultStatus(res))
	{
		case PGRES_COMMAND_OK:
		case PGRES_TUPLES_OK:
			/* A-OK */
			break;
		case PGRES_COPY_IN:
			/* Assume this is an expected result */
			AH->pgCopyIn = true;
			break;
		default:
			/* trouble */
			strncpy(errStmt, qry, DB_MAX_ERR_STMT);
			if (errStmt[DB_MAX_ERR_STMT - 1] != '\0')
			{
				errStmt[DB_MAX_ERR_STMT - 4] = '.';
				errStmt[DB_MAX_ERR_STMT - 3] = '.';
				errStmt[DB_MAX_ERR_STMT - 2] = '.';
				errStmt[DB_MAX_ERR_STMT - 1] = '\0';
			}
			warn_or_die_horribly(AH, modulename, "%s: %s    Command was: %s\n",
								 desc, PQerrorMessage(conn), errStmt);
			break;
	}

	PQclear(res);
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
		 * fprintf(stderr, "Found cr at %d, prev char was %c, next was %c\n",
		 * loc, qry[loc-1], qry[loc+1]);
		 */

		/* Count the number of preceding slashes */
		sPos = loc;
		while (sPos > 0 && qry[sPos - 1] == '\\')
			sPos--;

		sPos = loc - sPos;

		/*
		 * If an odd number of preceding slashes, then \n was escaped so set
		 * the next search pos, and loop (if any left).
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

	/*
	 * Note that we drop the data on the floor if libpq has failed to enter
	 * COPY mode; this allows us to behave reasonably when trying to continue
	 * after an error in a COPY command.
	 */
	if (AH->pgCopyIn &&
		PQputCopyData(AH->connection, AH->pgCopyBuf->data,
					  AH->pgCopyBuf->len) <= 0)
		die_horribly(AH, modulename, "error returned by PQputCopyData: %s",
					 PQerrorMessage(AH->connection));

	resetPQExpBuffer(AH->pgCopyBuf);

	if (isEnd && AH->pgCopyIn)
	{
		PGresult   *res;

		if (PQputCopyEnd(AH->connection, NULL) <= 0)
			die_horribly(AH, modulename, "error returned by PQputCopyEnd: %s",
						 PQerrorMessage(AH->connection));

		/* Check command status and return to normal libpq state */
		res = PQgetResult(AH->connection);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
			warn_or_die_horribly(AH, modulename, "COPY failed: %s",
								 PQerrorMessage(AH->connection));
		PQclear(res);

		AH->pgCopyIn = false;
	}

	return qry + loc + 1;
}

/*
 * Used by ExecuteSqlCommandBuf to send one buffered line of SQL
 * (not data for the copy command).
 */
static char *
_sendSQLLine(ArchiveHandle *AH, char *qry, char *eos)
{
	/*
	 * The following is a mini state machine to assess the end of an SQL
	 * statement. It really only needs to parse good SQL, or at least that's
	 * the theory... End-of-statement is assumed to be an unquoted,
	 * un-commented semi-colon that's not within any parentheses.
	 *
	 * Note: the input can be split into bufferloads at arbitrary boundaries.
	 * Therefore all state must be kept in AH->sqlparse, not in local
	 * variables of this routine.  We assume that AH->sqlparse was filled with
	 * zeroes when created.
	 */
	for (; qry < eos; qry++)
	{
		switch (AH->sqlparse.state)
		{
			case SQL_SCAN:		/* Default state == 0, set in _allocAH */
				if (*qry == ';' && AH->sqlparse.braceDepth == 0)
				{
					/*
					 * We've found the end of a statement. Send it and reset
					 * the buffer.
					 */
					appendPQExpBufferChar(AH->sqlBuf, ';');		/* inessential */
					ExecuteSqlCommand(AH, AH->sqlBuf->data,
									  "could not execute query");
					resetPQExpBuffer(AH->sqlBuf);
					AH->sqlparse.lastChar = '\0';

					/*
					 * Remove any following newlines - so that embedded COPY
					 * commands don't get a starting newline.
					 */
					qry++;
					while (qry < eos && *qry == '\n')
						qry++;

					/* We've finished one line, so exit */
					return qry;
				}
				else if (*qry == '\'')
				{
					if (AH->sqlparse.lastChar == 'E')
						AH->sqlparse.state = SQL_IN_E_QUOTE;
					else
						AH->sqlparse.state = SQL_IN_SINGLE_QUOTE;
					AH->sqlparse.backSlash = false;
				}
				else if (*qry == '"')
				{
					AH->sqlparse.state = SQL_IN_DOUBLE_QUOTE;
				}

				/*
				 * Look for dollar-quotes. We make the assumption that
				 * $-quotes will not have an ident character just before them
				 * in pg_dump output.  XXX is this good enough?
				 */
				else if (*qry == '$' && !_isIdentChar(AH->sqlparse.lastChar))
				{
					AH->sqlparse.state = SQL_IN_DOLLAR_TAG;
					/* initialize separate buffer with possible tag */
					if (AH->sqlparse.tagBuf == NULL)
						AH->sqlparse.tagBuf = createPQExpBuffer();
					else
						resetPQExpBuffer(AH->sqlparse.tagBuf);
					appendPQExpBufferChar(AH->sqlparse.tagBuf, *qry);
				}
				else if (*qry == '-' && AH->sqlparse.lastChar == '-')
					AH->sqlparse.state = SQL_IN_SQL_COMMENT;
				else if (*qry == '*' && AH->sqlparse.lastChar == '/')
					AH->sqlparse.state = SQL_IN_EXT_COMMENT;
				else if (*qry == '(')
					AH->sqlparse.braceDepth++;
				else if (*qry == ')')
					AH->sqlparse.braceDepth--;
				break;

			case SQL_IN_SQL_COMMENT:
				if (*qry == '\n')
					AH->sqlparse.state = SQL_SCAN;
				break;

			case SQL_IN_EXT_COMMENT:

				/*
				 * This isn't fully correct, because we don't account for
				 * nested slash-stars, but pg_dump never emits such.
				 */
				if (AH->sqlparse.lastChar == '*' && *qry == '/')
					AH->sqlparse.state = SQL_SCAN;
				break;

			case SQL_IN_SINGLE_QUOTE:
				/* We needn't handle '' specially */
				if (*qry == '\'' && !AH->sqlparse.backSlash)
					AH->sqlparse.state = SQL_SCAN;
				else if (*qry == '\\')
					AH->sqlparse.backSlash = !AH->sqlparse.backSlash;
				else
					AH->sqlparse.backSlash = false;
				break;

			case SQL_IN_E_QUOTE:

				/*
				 * Eventually we will need to handle '' specially, because
				 * after E'...''... we should still be in E_QUOTE state.
				 *
				 * XXX problem: how do we tell whether the dump was made by a
				 * version that thinks backslashes aren't special in non-E
				 * literals??
				 */
				if (*qry == '\'' && !AH->sqlparse.backSlash)
					AH->sqlparse.state = SQL_SCAN;
				else if (*qry == '\\')
					AH->sqlparse.backSlash = !AH->sqlparse.backSlash;
				else
					AH->sqlparse.backSlash = false;
				break;

			case SQL_IN_DOUBLE_QUOTE:
				/* We needn't handle "" specially */
				if (*qry == '"')
					AH->sqlparse.state = SQL_SCAN;
				break;

			case SQL_IN_DOLLAR_TAG:
				if (*qry == '$')
				{
					/* Do not add the closing $ to tagBuf */
					AH->sqlparse.state = SQL_IN_DOLLAR_QUOTE;
					AH->sqlparse.minTagEndPos = AH->sqlBuf->len + AH->sqlparse.tagBuf->len + 1;
				}
				else if (_isDQChar(*qry, (AH->sqlparse.tagBuf->len == 1)))
				{
					/* Valid, so add to tag */
					appendPQExpBufferChar(AH->sqlparse.tagBuf, *qry);
				}
				else
				{
					/*
					 * Ooops, we're not really in a dollar-tag.  Valid tag
					 * chars do not include the various chars we look for in
					 * this state machine, so it's safe to just jump from this
					 * state back to SCAN.	We have to back up the qry pointer
					 * so that the current character gets rescanned in SCAN
					 * state; and then "continue" so that the bottom-of-loop
					 * actions aren't done yet.
					 */
					AH->sqlparse.state = SQL_SCAN;
					qry--;
					continue;
				}
				break;

			case SQL_IN_DOLLAR_QUOTE:

				/*
				 * If we are at a $, see whether what precedes it matches
				 * tagBuf.	(Remember that the trailing $ of the tag was not
				 * added to tagBuf.)  However, don't compare until we have
				 * enough data to be a possible match --- this is needed to
				 * avoid false match on '$a$a$...'
				 */
				if (*qry == '$' &&
					AH->sqlBuf->len >= AH->sqlparse.minTagEndPos &&
					strcmp(AH->sqlparse.tagBuf->data,
						   AH->sqlBuf->data + AH->sqlBuf->len - AH->sqlparse.tagBuf->len) == 0)
					AH->sqlparse.state = SQL_SCAN;
				break;
		}

		appendPQExpBufferChar(AH->sqlBuf, *qry);
		AH->sqlparse.lastChar = *qry;
	}

	/*
	 * If we get here, we've processed entire bufferload with no complete SQL
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
	 * fprintf(stderr, "\n\n*****\n Buffer:\n\n%s\n*******************\n\n",
	 * qry);
	 */

	/* Could switch between command and COPY IN mode at each line */
	while (qry < eos)
	{
		/*
		 * If libpq is in CopyIn mode *or* if the archive structure shows we
		 * are sending COPY data, treat the data as COPY data.	The pgCopyIn
		 * check is only needed for backwards compatibility with ancient
		 * archive files that might just issue a COPY command without marking
		 * it properly.  Note that in an archive entry that has a copyStmt,
		 * all data up to the end of the entry will go to _sendCopyLine, and
		 * therefore will be dropped if libpq has failed to enter COPY mode.
		 * Also, if a "\." data terminator is found, anything remaining in the
		 * archive entry will be dropped.
		 */
		if (AH->pgCopyIn || AH->writingCopyData)
			qry = _sendCopyLine(AH, qry, eos);
		else
			qry = _sendSQLLine(AH, qry, eos);
	}

	return 1;
}

void
StartTransaction(ArchiveHandle *AH)
{
	ExecuteSqlCommand(AH, "BEGIN", "could not start database transaction");
}

void
CommitTransaction(ArchiveHandle *AH)
{
	ExecuteSqlCommand(AH, "COMMIT", "could not commit database transaction");
}

static bool
_isIdentChar(unsigned char c)
{
	if ((c >= 'a' && c <= 'z')
		|| (c >= 'A' && c <= 'Z')
		|| (c >= '0' && c <= '9')
		|| (c == '_')
		|| (c == '$')
		|| (c >= (unsigned char) '\200')		/* no need to check <= \377 */
		)
		return true;
	else
		return false;
}

static bool
_isDQChar(unsigned char c, bool atStart)
{
	if ((c >= 'a' && c <= 'z')
		|| (c >= 'A' && c <= 'Z')
		|| (c == '_')
		|| (!atStart && c >= '0' && c <= '9')
		|| (c >= (unsigned char) '\200')		/* no need to check <= \377 */
		)
		return true;
	else
		return false;
}
