/*-------------------------------------------------------------------------
 *
 * pg_backup_db.c
 *
 *	Implements the basic DB functions used by the archiver.
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/bin/pg_dump/pg_backup_db.c,v 1.62 2005/06/21 20:45:44 tgl Exp $
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
static void notice_processor(void *arg, const char *message);
static char *_sendSQLLine(ArchiveHandle *AH, char *qry, char *eos);
static char *_sendCopyLine(ArchiveHandle *AH, char *qry, char *eos);

static int	_isIdentChar(unsigned char c);
static int	_isDQChar(unsigned char c, int atStart);

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
					 dbname, PQerrorMessage(AH->connection));

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
ExecuteSqlCommand(ArchiveHandle *AH, PQExpBuffer qry, char *desc)
{
	PGconn	   *conn = AH->connection;
	PGresult   *res;
	char		errStmt[DB_MAX_ERR_STMT];

	/* fprintf(stderr, "Executing: '%s'\n\n", qry->data); */
	res = PQexec(conn, qry->data);
	if (!res)
		die_horribly(AH, modulename, "%s: no result from server\n", desc);

	if (PQresultStatus(res) != PGRES_COMMAND_OK && PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		if (PQresultStatus(res) == PGRES_COPY_IN)
		{
			AH->pgCopyIn = 1;
		}
		else
		{
			strncpy(errStmt, qry->data, DB_MAX_ERR_STMT);
			if (errStmt[DB_MAX_ERR_STMT - 1] != '\0')
			{
				errStmt[DB_MAX_ERR_STMT - 4] = '.';
				errStmt[DB_MAX_ERR_STMT - 3] = '.';
				errStmt[DB_MAX_ERR_STMT - 2] = '.';
				errStmt[DB_MAX_ERR_STMT - 1] = '\0';
			}
			warn_or_die_horribly(AH, modulename, "%s: %s    Command was: %s\n",
								 desc, PQerrorMessage(AH->connection),
								 errStmt);
		}
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
	char	   *sqlPtr;
	int			consumed;
	int			startDT = 0;

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

		/* Loop until character consumed */
		do
		{
			/*
			 * If a character needs to be scanned in a different state,
			 * consumed can be set to 0 to avoid advancing. Care must be
			 * taken to ensure internal state is not damaged.
			 */
			consumed = 1;

			switch (AH->sqlparse.state)
			{

				case SQL_SCAN:	/* Default state == 0, set in _allocAH */
					if (qry[pos] == ';' && AH->sqlparse.braceDepth == 0)
					{
						/*
						 * We've got the end of a statement. Send It &
						 * reset the buffer.
						 */

						/*
						 * fprintf(stderr, "    sending: '%s'\n\n",
						 * AH->sqlBuf->data);
						 */
						ExecuteSqlCommand(AH, AH->sqlBuf, "could not execute query");
						resetPQExpBuffer(AH->sqlBuf);
						AH->sqlparse.lastChar = '\0';

						/*
						 * Remove any following newlines - so that
						 * embedded COPY commands don't get a starting
						 * newline.
						 */
						pos++;
						for (; pos < (eos - qry) && qry[pos] == '\n'; pos++);

						/* We've got our line, so exit */
						return qry + pos;
					}
					else
					{
						/*
						 * Look for normal boring quote chars, or
						 * dollar-quotes. We make the assumption that
						 * $-quotes will not have an ident character
						 * before them in all pg_dump output.
						 */
						if (qry[pos] == '"'
							|| qry[pos] == '\''
							|| (qry[pos] == '$' && _isIdentChar(AH->sqlparse.lastChar) == 0)
							)
						{
							/* fprintf(stderr,"[startquote]\n"); */
							AH->sqlparse.state = SQL_IN_QUOTE;
							AH->sqlparse.quoteChar = qry[pos];
							AH->sqlparse.backSlash = 0;
							if (qry[pos] == '$')
							{
								/* override the state */
								AH->sqlparse.state = SQL_IN_DOLLARTAG;
								/* Used for checking first char of tag */
								startDT = 1;
								/* We store the tag for later comparison. */
								AH->sqlparse.tagBuf = createPQExpBuffer();
								/* Get leading $ */
								appendPQExpBufferChar(AH->sqlparse.tagBuf, qry[pos]);
							}
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

				case SQL_IN_DOLLARTAG:

					/*
					 * Like a quote, we look for a closing char *but* we
					 * only allow a very limited set of contained chars,
					 * and no escape chars. If invalid chars are found, we
					 * abort tag processing.
					 */

					if (qry[pos] == '$')
					{
						/* fprintf(stderr,"[endquote]\n"); */
						/* Get trailing $ */
						appendPQExpBufferChar(AH->sqlparse.tagBuf, qry[pos]);
						AH->sqlparse.state = SQL_IN_DOLLARQUOTE;
					}
					else
					{
						if (_isDQChar(qry[pos], startDT))
						{
							/* Valid, so add */
							appendPQExpBufferChar(AH->sqlparse.tagBuf, qry[pos]);
						}
						else
						{
							/*
							 * Jump back to 'scan' state, we're not really
							 * in a tag, and valid tag chars do not
							 * include the various chars we look for in
							 * this state machine, so it's safe to just
							 * jump from this state back to SCAN. We set
							 * consumed = 0 so that this char gets
							 * rescanned in new state.
							 */
							destroyPQExpBuffer(AH->sqlparse.tagBuf);
							AH->sqlparse.state = SQL_SCAN;
							consumed = 0;
						}
					}
					startDT = 0;
					break;


				case SQL_IN_DOLLARQUOTE:

					/*
					 * Comparing the entire string backwards each time is
					 * NOT efficient, but dollar quotes in pg_dump are
					 * small and the code is a lot simpler.
					 */
					sqlPtr = AH->sqlBuf->data + AH->sqlBuf->len - AH->sqlparse.tagBuf->len;

					if (strncmp(AH->sqlparse.tagBuf->data, sqlPtr, AH->sqlparse.tagBuf->len) == 0)
					{
						/* End of $-quote */
						AH->sqlparse.state = SQL_SCAN;
						destroyPQExpBuffer(AH->sqlparse.tagBuf);
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

		} while (consumed == 0);

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
StartTransaction(ArchiveHandle *AH)
{
	PQExpBuffer qry = createPQExpBuffer();

	appendPQExpBuffer(qry, "BEGIN");

	ExecuteSqlCommand(AH, qry, "could not start database transaction");

	destroyPQExpBuffer(qry);
}

void
CommitTransaction(ArchiveHandle *AH)
{
	PQExpBuffer qry = createPQExpBuffer();

	appendPQExpBuffer(qry, "COMMIT");

	ExecuteSqlCommand(AH, qry, "could not commit database transaction");

	destroyPQExpBuffer(qry);
}

static int
_isIdentChar(unsigned char c)
{
	if ((c >= 'a' && c <= 'z')
		|| (c >= 'A' && c <= 'Z')
		|| (c >= '0' && c <= '9')
		|| (c == '_')
		|| (c == '$')
		|| (c >= (unsigned char) '\200')		/* no need to check <=
												 * \377 */
		)
		return 1;
	else
		return 0;
}

static int
_isDQChar(unsigned char c, int atStart)
{
	if ((c >= 'a' && c <= 'z')
		|| (c >= 'A' && c <= 'Z')
		|| (c == '_')
		|| (atStart == 0 && c >= '0' && c <= '9')
		|| (c >= (unsigned char) '\200')		/* no need to check <=
												 * \377 */
		)
		return 1;
	else
		return 0;
}
