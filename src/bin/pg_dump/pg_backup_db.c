/*-------------------------------------------------------------------------
 *
 *
*-------------------------------------------------------------------------
 */

#include <unistd.h>				/* for getopt() */
#include <ctype.h>

#include "postgres.h"

#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif

#include "access/attnum.h"
#include "access/htup.h"
#include "catalog/pg_index.h"
#include "catalog/pg_language.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"

#include "libpq-fe.h"
#include <libpq/libpq-fs.h>
#ifndef HAVE_STRDUP
#include "strdup.h"
#endif

#include "pg_dump.h"
#include "pg_backup.h"
#include "pg_backup_archiver.h"
#include "pg_backup_db.h"

static const char	*progname = "Archiver(db)";

static void 	_prompt_for_password(char *username, char *password);
static void 	_check_database_version(ArchiveHandle *AH, bool ignoreVersion);
static PGconn*	_connectDB(ArchiveHandle *AH, const char* newdbname, char *newUser);
static int		_executeSqlCommand(ArchiveHandle* AH, PGconn *conn, PQExpBuffer qry, char *desc);


static void
_prompt_for_password(char *username, char *password)
{
	char		buf[512];
	int			length;

#ifdef HAVE_TERMIOS_H
	struct termios t_orig,
				   t;
#endif

	/*
	 * Allow for forcing a specific username
	 */
	if (strlen(username) == 0)
	{
		fprintf(stderr, "Username: ");
		fflush(stderr);
		fgets(username, 100, stdin);
		length = strlen(username);
		/* skip rest of the line */
		if (length > 0 && username[length - 1] != '\n')
		{
			do
			{
				fgets(buf, 512, stdin);
			} while (buf[strlen(buf) - 1] != '\n');
		}
		if (length > 0 && username[length - 1] == '\n')
			username[length - 1] = '\0';
	}

#ifdef HAVE_TERMIOS_H
	tcgetattr(0, &t);
	t_orig = t;
	t.c_lflag &= ~ECHO;
	tcsetattr(0, TCSADRAIN, &t);
#endif
	fprintf(stderr, "Password: ");
	fflush(stderr);
	fgets(password, 100, stdin);
#ifdef HAVE_TERMIOS_H
	tcsetattr(0, TCSADRAIN, &t_orig);
#endif

	length = strlen(password);
	/* skip rest of the line */
	if (length > 0 && password[length - 1] != '\n')
	{
		do
		{
			fgets(buf, 512, stdin);
		} while (buf[strlen(buf) - 1] != '\n');
	}
	if (length > 0 && password[length - 1] == '\n')
		password[length - 1] = '\0';

	fprintf(stderr, "\n\n");
}


static void
_check_database_version(ArchiveHandle *AH, bool ignoreVersion)
{
	PGresult   *res;
	double      myversion;
	const char *remoteversion_str;
	double      remoteversion;
	PGconn		*conn = AH->connection;

	myversion = strtod(PG_VERSION, NULL);
	res = PQexec(conn, "SELECT version()");
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK ||
		PQntuples(res) != 1)

		die_horribly(AH, "check_database_version(): command failed.  "
				"Explanation from backend: '%s'.\n", PQerrorMessage(conn));

	remoteversion_str = PQgetvalue(res, 0, 0);
	remoteversion = strtod(remoteversion_str + 11, NULL);
	if (myversion != remoteversion)
	{
		fprintf(stderr, "Database version: %s\n%s version: %s\n",
				remoteversion_str, progname, PG_VERSION);
		if (ignoreVersion)
			fprintf(stderr, "Proceeding despite version mismatch.\n");
		else
			die_horribly(AH, "Aborting because of version mismatch.\n"
				    "Use --ignore-version if you think it's safe to proceed anyway.\n");	
	}
	PQclear(res);
}

/* 
 * Check if a given user is a superuser.
 */
int UserIsSuperuser(ArchiveHandle *AH, char* user)
{
	PQExpBuffer			qry = createPQExpBuffer();
	PGresult			*res;
	int					i_usesuper;
	int					ntups;
	int					isSuper;

	/* Get the superuser setting */
	appendPQExpBuffer(qry, "select usesuper from pg_user where usename = '%s'", user);
	res = PQexec(AH->connection, qry->data);

	if (!res)
		die_horribly(AH, "%s: null result checking superuser status of %s.\n",
					progname, user);

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		die_horribly(AH, "%s: Could not check superuser status of %s. Explanation from backend: %s\n",
					progname, user, PQerrorMessage(AH->connection));

	ntups = PQntuples(res);

	if (ntups == 0)
		isSuper = 0;
	else
	{
		i_usesuper = PQfnumber(res, "usesuper");
		isSuper = (strcmp(PQgetvalue(res, 0, i_usesuper), "t") == 0);
	}
	PQclear(res);

	return isSuper;
}

int ConnectedUserIsSuperuser(ArchiveHandle *AH)
{
	return UserIsSuperuser(AH, PQuser(AH->connection));
}

char* ConnectedUser(ArchiveHandle *AH)
{
	return PQuser(AH->connection);
}

/*
 * Reconnect the DB associated with the archive handle 
 */
int ReconnectDatabase(ArchiveHandle *AH, const char* newdbname, char *newUser)
{
	PGconn		*newConn;
	char		*dbname;

	if (!newdbname || (strcmp(newdbname, "-") == 0) )
		dbname = PQdb(AH->connection);
	else
		dbname = (char*)newdbname;

	/* Let's see if the request is already satisfied */
	if (strcmp(PQuser(AH->connection), newUser) == 0 && strcmp(newdbname, PQdb(AH->connection)) == 0)
		return 1;

	newConn = _connectDB(AH, dbname, newUser);

	PQfinish(AH->connection);
	AH->connection = newConn;
	strcpy(AH->username, newUser);

	return 1;
}

/*
 * Connect to the db again.
 */
static PGconn* _connectDB(ArchiveHandle *AH, const char* reqdb, char *requser)
{
	int			need_pass;
	PGconn		*newConn;
	char		password[100];
	char		*pwparam = NULL;
	int			badPwd = 0;
	int			noPwd = 0;
	char		*newdb;
	char		*newuser;

	if (!reqdb || (strcmp(reqdb, "-") == 0) )
		newdb = PQdb(AH->connection);
	else
		newdb = (char*)reqdb;

	if (!requser || (strlen(requser) == 0))
		newuser = PQuser(AH->connection);
	else
		newuser = (char*)requser;

	ahlog(AH, 1, "Connecting to %s as %s\n", newdb, newuser);

	do
	{
		need_pass = false;
		newConn = PQsetdbLogin(PQhost(AH->connection), PQport(AH->connection),
								NULL, NULL, newdb, 
								newuser, pwparam);
		if (!newConn)
			die_horribly(AH, "%s: Failed to reconnect (PQsetdbLogin failed).\n", progname);

		if (PQstatus(newConn) == CONNECTION_BAD)
		{
			noPwd = (strcmp(PQerrorMessage(newConn), "fe_sendauth: no password supplied\n") == 0);
			badPwd = (strncmp(PQerrorMessage(newConn), "Password authentication failed for user", 39)
						== 0);

			if (noPwd || badPwd) 
			{

				if (badPwd)
					fprintf(stderr, "Password incorrect\n");

				fprintf(stderr, "Connecting to %s as %s\n", PQdb(AH->connection), newuser);

				need_pass = true;
				_prompt_for_password(newuser, password);
				pwparam = password; 
			}
			else
				die_horribly(AH, "%s: Could not reconnect. %s\n", progname, PQerrorMessage(newConn));
		}

	} while (need_pass);

	return newConn;
}


PGconn* ConnectDatabase(Archive *AHX, 
		const char* 	dbname,
		const char* 	pghost,
		const char* 	pgport,
		const int		reqPwd,
		const int		ignoreVersion)
{
	ArchiveHandle	*AH = (ArchiveHandle*)AHX;
	char			connect_string[512] = "";
	char			tmp_string[128];
	char			password[100];

	if (AH->connection)
		die_horribly(AH, "%s: already connected to database\n", progname);

	if (!dbname && !(dbname = getenv("PGDATABASE")) ) 
		die_horribly(AH, "%s: no database name specified\n", progname);

	AH->dbname = strdup(dbname);

	if (pghost != NULL)
	{
		AH->pghost = strdup(pghost);
		sprintf(tmp_string, "host=%s ", AH->pghost);
		strcat(connect_string, tmp_string);
	}
	else
	    AH->pghost = NULL;

	if (pgport != NULL)
	{
		AH->pgport = strdup(pgport);
		sprintf(tmp_string, "port=%s ", AH->pgport);
		strcat(connect_string, tmp_string);
	}
	else
	    AH->pgport = NULL;

	sprintf(tmp_string, "dbname=%s ", AH->dbname);
	strcat(connect_string, tmp_string);

	if (reqPwd)
	{
		AH->username[0] = '\0';
		_prompt_for_password(AH->username, password);
		strcat(connect_string, "authtype=password ");
		sprintf(tmp_string, "user=%s ", AH->username);
		strcat(connect_string, tmp_string);
		sprintf(tmp_string, "password=%s ", password);
		strcat(connect_string, tmp_string);
		MemSet(tmp_string, 0, sizeof(tmp_string));
		MemSet(password, 0, sizeof(password));
	}
	AH->connection = PQconnectdb(connect_string);
	MemSet(connect_string, 0, sizeof(connect_string));

	/* check to see that the backend connection was successfully made */
	if (PQstatus(AH->connection) == CONNECTION_BAD)
		die_horribly(AH, "Connection to database '%s' failed.\n%s\n",
						AH->dbname, PQerrorMessage(AH->connection));

	/* check for version mismatch */
	_check_database_version(AH, ignoreVersion);

	/*
     * AH->currUser = PQuser(AH->connection);
	 *	
	 * Removed because it prevented an initial \connect
	 * when dumping to SQL in pg_dump.
	 */

	return AH->connection;
}

/* Public interface */
/* Convenience function to send a query. Monitors result to handle COPY statements */
int ExecuteSqlCommand(ArchiveHandle* AH, PQExpBuffer qry, char *desc)
{
	return _executeSqlCommand(AH, AH->connection, qry, desc);
}

/* 
 * Handle command execution. This is used to execute a command on more than one connection,
 * but the 'pgCopyIn' setting assumes the COPY commands are ONLY executed on the primary
 * setting...an error will be raised otherwise.
 */
static int _executeSqlCommand(ArchiveHandle* AH, PGconn *conn, PQExpBuffer qry, char *desc)
{
	PGresult		*res;

	/* fprintf(stderr, "Executing: '%s'\n\n", qry->data); */
	res = PQexec(conn, qry->data);
	if (!res)
		die_horribly(AH, "%s: %s. No result from backend.\n", progname, desc);

    if (PQresultStatus(res) != PGRES_COMMAND_OK && PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		if (PQresultStatus(res) == PGRES_COPY_IN)
		{
			if (conn != AH->connection)
				die_horribly(AH, "%s: COPY command execute in non-primary connection.\n", progname);

			AH->pgCopyIn = 1;
		}
		else 
			die_horribly(AH, "%s: %s. Code = %d. Explanation from backend: '%s'.\n",
						progname, desc, PQresultStatus(res), PQerrorMessage(AH->connection));
	}

	PQclear(res);

	return strlen(qry->data);
}

/* Convenience function to send one or more queries. Monitors result to handle COPY statements */
int ExecuteSqlCommandBuf(ArchiveHandle* AH, void *qryv, int bufLen)
{
	int				loc;
	int				pos = 0;
	int				sPos = 0;
	char			*qry = (char*)qryv;
	int				isEnd = 0;
	char			*eos = qry + bufLen;

	/* fprintf(stderr, "\n\n*****\n Buffer:\n\n%s\n*******************\n\n", qry); */

	/* If we're in COPY IN mode, then just break it into lines and send... */
	if (AH->pgCopyIn) {
		for(;;) {

			/* Find a lf */
			loc = strcspn(&qry[pos], "\n") + pos;
			pos = 0;

			/* If no match, then wait */
			if (loc >= (eos - qry)) /* None found */
			{
				appendBinaryPQExpBuffer(AH->pgCopyBuf, qry, (eos - qry));
				break;
			};

		    /* fprintf(stderr, "Found cr at %d, prev char was %c, next was %c\n", loc, qry[loc-1], qry[loc+1]); */
	
			/* Count the number of preceding slashes */
			sPos = loc;
			while (sPos > 0 && qry[sPos-1] == '\\')
				sPos--;

			sPos = loc - sPos;

			/* If an odd number of preceding slashes, then \n was escaped 
			 * so set the next search pos, and restart (if any left).
			 */
			if ((sPos & 1) == 1)
			{
				/* fprintf(stderr, "cr was escaped\n"); */
				pos = loc + 1;
				if (pos >= (eos - qry))
				{
					appendBinaryPQExpBuffer(AH->pgCopyBuf, qry, (eos - qry));
					break;
				}
			}
			else
			{
				/* We got a good cr */
				qry[loc] = '\0';
				appendPQExpBuffer(AH->pgCopyBuf, "%s\n", qry);
				qry += loc + 1; 
				isEnd = (strcmp(AH->pgCopyBuf->data, "\\.\n") == 0);

				/* fprintf(stderr, "Sending '%s' via COPY (at end = %d)\n\n", AH->pgCopyBuf->data, isEnd); */ 
				
				PQputline(AH->connection, AH->pgCopyBuf->data);

				resetPQExpBuffer(AH->pgCopyBuf);

				/* fprintf(stderr, "Buffer is '%s'\n", AH->pgCopyBuf->data); */

				if(isEnd) {
					PQendcopy(AH->connection);
					AH->pgCopyIn = 0;
					break;
				}

			}

			/* Make sure we're not past the original buffer end */
			if (qry >= eos)
				break;

		}
	}

	/* We may have finished Copy In, and have a non-empty buffer */
	if (!AH->pgCopyIn) {

		/* 
		 * The following is a mini state machine to assess then of of an SQL statement.
		 * It really only needs to parse good SQL, or at least that's the theory...
		 * End-of-statement is assumed to be an unquoted, un commented semi-colon.
		 */

		/* fprintf(stderr, "Buffer at start is: '%s'\n\n", AH->sqlBuf->data); */

		for(pos=0; pos < (eos - qry); pos++)
		{
			appendPQExpBufferChar(AH->sqlBuf, qry[pos]);
			/* fprintf(stderr, " %c",qry[pos]); */

			switch (AH->sqlparse.state) {

				case SQL_SCAN: /* Default state == 0, set in _allocAH */

					if (qry[pos] == ';' && AH->sqlparse.braceDepth == 0)
					{
						/* Send It & reset the buffer */
						/* fprintf(stderr, "    sending: '%s'\n\n", AH->sqlBuf->data); */
						ExecuteSqlCommand(AH, AH->sqlBuf, "Could not execute query");
						resetPQExpBuffer(AH->sqlBuf);
						AH->sqlparse.lastChar = '\0';
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
						{
							AH->sqlparse.state = SQL_IN_SQL_COMMENT;
						} 
						else if (qry[pos] == '*' && AH->sqlparse.lastChar == '/')
						{
							AH->sqlparse.state = SQL_IN_EXT_COMMENT;
						} 
						else if ( qry[pos] == '(' )
						{
							AH->sqlparse.braceDepth++;
						}
						else if (qry[pos] == ')')
						{
							AH->sqlparse.braceDepth--;
						}

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
							} else {
								AH->sqlparse.backSlash = 0;
						}
					}
					break;

			}
			AH->sqlparse.lastChar = qry[pos];
			/* fprintf(stderr, "\n"); */
		}

	}

	return 1;
}

void FixupBlobRefs(ArchiveHandle *AH, char *tablename)
{
	PQExpBuffer		tblQry = createPQExpBuffer();
	PGresult		*res, *uRes;
	int				i, n;
	char			*attr;

	for(i=0 ; i < strlen(tablename) ; i++)
		tablename[i] = tolower(tablename[i]);

	if (strcmp(tablename, BLOB_XREF_TABLE) == 0)
		return;

	appendPQExpBuffer(tblQry, "SELECT a.attname FROM pg_class c, pg_attribute a, pg_type t "
								" WHERE a.attnum > 0 AND a.attrelid = c.oid AND a.atttypid = t.oid "
								" AND t.typname = 'oid' AND c.relname = '%s';", tablename);

	res = PQexec(AH->blobConnection, tblQry->data);
	if (!res)
		die_horribly(AH, "%s: could not find OID attrs of %s. Explanation from backend '%s'\n",
						progname, tablename, PQerrorMessage(AH->connection));

	if ((n = PQntuples(res)) == 0) {
		/* We're done */
		ahlog(AH, 1, "No OID attributes in table %s\n", tablename);
		PQclear(res);
		return;
	}

	for (i = 0 ; i < n ; i++)
	{
		attr = PQgetvalue(res, i, 0);

		ahlog(AH, 1, " - %s.%s\n", tablename, attr);

		resetPQExpBuffer(tblQry);

		/*
		 * We should use coalesce here (rather than 'exists'), but it seems to 
		 * be broken in 7.0.2 (weird optimizer strategy)
		 */
		appendPQExpBuffer(tblQry, "UPDATE \"%s\" SET \"%s\" = ",tablename, attr);
		appendPQExpBuffer(tblQry, " (SELECT x.newOid FROM \"%s\" x WHERE x.oldOid = \"%s\".\"%s\")",
									BLOB_XREF_TABLE, tablename, attr);
		appendPQExpBuffer(tblQry, " where exists"
									"(select * from %s x where x.oldOid = \"%s\".\"%s\");",
									BLOB_XREF_TABLE, tablename, attr);

		ahlog(AH, 10, " - sql:\n%s\n", tblQry->data);

		uRes = PQexec(AH->blobConnection, tblQry->data);
		if (!uRes)
			die_horribly(AH, "%s: could not update attr %s of table %s. Explanation from backend '%s'\n",
								progname, attr, tablename, PQerrorMessage(AH->blobConnection));

		if ( PQresultStatus(uRes) != PGRES_COMMAND_OK )
			die_horribly(AH, "%s: error while updating attr %s of table %s (result = %d)."
								" Explanation from backend '%s'\n",
								progname, attr, tablename, PQresultStatus(uRes), 
								PQerrorMessage(AH->blobConnection));

		PQclear(uRes);
	}

	PQclear(res);

}

/**********
 *	Convenient SQL calls
 **********/
void CreateBlobXrefTable(ArchiveHandle* AH)
{
	PQExpBuffer		qry = createPQExpBuffer();

	/* IF we don't have a BLOB connection, then create one */
	if (!AH->blobConnection)
	{
		AH->blobConnection = _connectDB(AH, NULL, NULL);
	}

	ahlog(AH, 1, "Creating table for BLOBS xrefs\n");

	appendPQExpBuffer(qry, "Create Temporary Table %s(oldOid oid, newOid oid);", BLOB_XREF_TABLE);

	_executeSqlCommand(AH, AH->blobConnection, qry, "can not create BLOB xref table '" BLOB_XREF_TABLE "'");

	resetPQExpBuffer(qry);

	appendPQExpBuffer(qry, "Create Unique Index %s_ix on %s(oldOid)", BLOB_XREF_TABLE, BLOB_XREF_TABLE);
	_executeSqlCommand(AH, AH->blobConnection, qry, "can not create index on BLOB xref table '" BLOB_XREF_TABLE "'");
}

void InsertBlobXref(ArchiveHandle* AH, int old, int new)
{
	PQExpBuffer 	qry = createPQExpBuffer();

	appendPQExpBuffer(qry, "Insert Into %s(oldOid, newOid) Values (%d, %d);", BLOB_XREF_TABLE, old, new);

	_executeSqlCommand(AH, AH->blobConnection, qry, "can not create BLOB xref entry");
}

void StartTransaction(ArchiveHandle* AH)
{
	PQExpBuffer		qry = createPQExpBuffer();

	appendPQExpBuffer(qry, "Begin;");

	ExecuteSqlCommand(AH, qry, "can not start database transaction");
}

void CommitTransaction(ArchiveHandle* AH)
{
    PQExpBuffer     qry = createPQExpBuffer();

    appendPQExpBuffer(qry, "Commit;");

    ExecuteSqlCommand(AH, qry, "can not commit database transaction");
}


