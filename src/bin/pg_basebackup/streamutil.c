/*-------------------------------------------------------------------------
 *
 * streamutil.c - utility functions for pg_basebackup and pg_receivelog
 *
 * Author: Magnus Hagander <magnus@hagander.net>
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/streamutil.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

/* for ntohl/htonl */
#include <netinet/in.h>
#include <arpa/inet.h>

/* local includes */
#include "receivelog.h"
#include "streamutil.h"

#include "pqexpbuffer.h"
#include "common/fe_memutils.h"
#include "datatype/timestamp.h"

const char *progname;
char	   *connection_string = NULL;
char	   *dbhost = NULL;
char	   *dbuser = NULL;
char	   *dbport = NULL;
char	   *replication_slot = NULL;
char	   *dbname = NULL;
int			dbgetpassword = 0;	/* 0=auto, -1=never, 1=always */
static char *dbpassword = NULL;
PGconn	   *conn = NULL;

/*
 * Connect to the server. Returns a valid PGconn pointer if connected,
 * or NULL on non-permanent error. On permanent error, the function will
 * call exit(1) directly.
 */
PGconn *
GetConnection(void)
{
	PGconn	   *tmpconn;
	int			argcount = 7;	/* dbname, replication, fallback_app_name,
								 * host, user, port, password */
	int			i;
	const char **keywords;
	const char **values;
	const char *tmpparam;
	bool		need_password;
	PQconninfoOption *conn_opts = NULL;
	PQconninfoOption *conn_opt;
	char	   *err_msg = NULL;

	/*
	 * Merge the connection info inputs given in form of connection string,
	 * options and default values (dbname=replication, replication=true, etc.)
	 */
	i = 0;
	if (connection_string)
	{
		conn_opts = PQconninfoParse(connection_string, &err_msg);
		if (conn_opts == NULL)
		{
			fprintf(stderr, "%s: %s", progname, err_msg);
			exit(1);
		}

		for (conn_opt = conn_opts; conn_opt->keyword != NULL; conn_opt++)
		{
			if (conn_opt->val != NULL && conn_opt->val[0] != '\0')
				argcount++;
		}

		keywords = pg_malloc0((argcount + 1) * sizeof(*keywords));
		values = pg_malloc0((argcount + 1) * sizeof(*values));

		for (conn_opt = conn_opts; conn_opt->keyword != NULL; conn_opt++)
		{
			if (conn_opt->val != NULL && conn_opt->val[0] != '\0')
			{
				keywords[i] = conn_opt->keyword;
				values[i] = conn_opt->val;
				i++;
			}
		}
	}
	else
	{
		keywords = pg_malloc0((argcount + 1) * sizeof(*keywords));
		values = pg_malloc0((argcount + 1) * sizeof(*values));
	}

	keywords[i] = "dbname";
	values[i] = dbname == NULL ? "replication" : dbname;
	i++;
	keywords[i] = "replication";
	values[i] = dbname == NULL ? "true" : "database";
	i++;
	keywords[i] = "fallback_application_name";
	values[i] = progname;
	i++;

	if (dbhost)
	{
		keywords[i] = "host";
		values[i] = dbhost;
		i++;
	}
	if (dbuser)
	{
		keywords[i] = "user";
		values[i] = dbuser;
		i++;
	}
	if (dbport)
	{
		keywords[i] = "port";
		values[i] = dbport;
		i++;
	}

	/* If -W was given, force prompt for password, but only the first time */
	need_password = (dbgetpassword == 1 && dbpassword == NULL);

	do
	{
		/* Get a new password if appropriate */
		if (need_password)
		{
			if (dbpassword)
				free(dbpassword);
			dbpassword = simple_prompt(_("Password: "), 100, false);
			need_password = false;
		}

		/* Use (or reuse, on a subsequent connection) password if we have it */
		if (dbpassword)
		{
			keywords[i] = "password";
			values[i] = dbpassword;
		}
		else
		{
			keywords[i] = NULL;
			values[i] = NULL;
		}

		tmpconn = PQconnectdbParams(keywords, values, true);

		/*
		 * If there is too little memory even to allocate the PGconn object
		 * and PQconnectdbParams returns NULL, we call exit(1) directly.
		 */
		if (!tmpconn)
		{
			fprintf(stderr, _("%s: could not connect to server\n"),
					progname);
			exit(1);
		}

		/* If we need a password and -w wasn't given, loop back and get one */
		if (PQstatus(tmpconn) == CONNECTION_BAD &&
			PQconnectionNeedsPassword(tmpconn) &&
			dbgetpassword != -1)
		{
			PQfinish(tmpconn);
			need_password = true;
		}
	}
	while (need_password);

	if (PQstatus(tmpconn) != CONNECTION_OK)
	{
		fprintf(stderr, _("%s: could not connect to server: %s\n"),
				progname, PQerrorMessage(tmpconn));
		PQfinish(tmpconn);
		free(values);
		free(keywords);
		if (conn_opts)
			PQconninfoFree(conn_opts);
		return NULL;
	}

	/* Connection ok! */
	free(values);
	free(keywords);
	if (conn_opts)
		PQconninfoFree(conn_opts);

	/*
	 * Ensure we have the same value of integer timestamps as the server we
	 * are connecting to.
	 */
	tmpparam = PQparameterStatus(tmpconn, "integer_datetimes");
	if (!tmpparam)
	{
		fprintf(stderr,
		 _("%s: could not determine server setting for integer_datetimes\n"),
				progname);
		PQfinish(tmpconn);
		exit(1);
	}

#ifdef HAVE_INT64_TIMESTAMP
	if (strcmp(tmpparam, "on") != 0)
#else
	if (strcmp(tmpparam, "off") != 0)
#endif
	{
		fprintf(stderr,
			 _("%s: integer_datetimes compile flag does not match server\n"),
				progname);
		PQfinish(tmpconn);
		exit(1);
	}

	return tmpconn;
}

/*
 * Run IDENTIFY_SYSTEM through a given connection and give back to caller
 * some result information if requested:
 * - Start LSN position
 * - Current timeline ID
 * - System identifier
 * - Plugin name
 */
bool
RunIdentifySystem(PGconn *conn, char **sysid, TimeLineID *starttli,
				  XLogRecPtr *startpos, char **db_name)
{
	PGresult   *res;
	uint32		hi, lo;

	/* Check connection existence */
	Assert(conn != NULL);

	res = PQexec(conn, "IDENTIFY_SYSTEM");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, _("%s: could not send replication command \"%s\": %s"),
				progname, "IDENTIFY_SYSTEM", PQerrorMessage(conn));

		PQclear(res);
		return false;
	}
	if (PQntuples(res) != 1 || PQnfields(res) < 3)
	{
		fprintf(stderr,
				_("%s: could not identify system: got %d rows and %d fields, expected %d rows and %d or more fields\n"),
				progname, PQntuples(res), PQnfields(res), 1, 3);

		PQclear(res);
		return false;
	}

	/* Get system identifier */
	if (sysid != NULL)
		*sysid = pg_strdup(PQgetvalue(res, 0, 0));

	/* Get timeline ID to start streaming from */
	if (starttli != NULL)
		*starttli = atoi(PQgetvalue(res, 0, 1));

	/* Get LSN start position if necessary */
	if (startpos != NULL)
	{
		if (sscanf(PQgetvalue(res, 0, 2), "%X/%X", &hi, &lo) != 2)
		{
			fprintf(stderr,
					_("%s: could not parse transaction log location \"%s\"\n"),
					progname, PQgetvalue(res, 0, 2));

			PQclear(res);
			return false;
		}
		*startpos = ((uint64) hi) << 32 | lo;
	}

	/* Get database name, only available in 9.4 and newer versions */
	if  (db_name != NULL)
	{
		if (PQnfields(res) < 4)
			fprintf(stderr,
					_("%s: could not identify system: got %d rows and %d fields, expected %d rows and %d or more fields\n"),
					progname, PQntuples(res), PQnfields(res), 1, 4);

		if (PQgetisnull(res, 0, 3))
			*db_name =  NULL;
		else
			*db_name = pg_strdup(PQgetvalue(res, 0, 3));
	}

	PQclear(res);
	return true;
}

/*
 * Create a replication slot for the given connection. This function
 * returns true in case of success as well as the start position
 * obtained after the slot creation.
 */
bool
CreateReplicationSlot(PGconn *conn, const char *slot_name, const char *plugin,
					  XLogRecPtr *startpos, bool is_physical)
{
	PQExpBuffer query;
	PGresult   *res;

	query = createPQExpBuffer();

	Assert((is_physical && plugin == NULL) ||
		   (!is_physical && plugin != NULL));
	Assert(slot_name != NULL);

	/* Build query */
	if (is_physical)
		appendPQExpBuffer(query, "CREATE_REPLICATION_SLOT \"%s\" PHYSICAL",
						  slot_name);
	else
		appendPQExpBuffer(query, "CREATE_REPLICATION_SLOT \"%s\" LOGICAL \"%s\"",
						  slot_name, plugin);

	res = PQexec(conn, query->data);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, _("%s: could not send replication command \"%s\": %s"),
				progname, query->data, PQerrorMessage(conn));

		destroyPQExpBuffer(query);
		PQclear(res);
		return false;
	}

	if (PQntuples(res) != 1 || PQnfields(res) != 4)
	{
		fprintf(stderr,
				_("%s: could not create replication slot \"%s\": got %d rows and %d fields, expected %d rows and %d fields\n"),
				progname, slot_name,
				PQntuples(res), PQnfields(res), 1, 4);

		destroyPQExpBuffer(query);
		PQclear(res);
		return false;
	}

	/* Get LSN start position if necessary */
	if (startpos != NULL)
	{
		uint32		hi, lo;

		if (sscanf(PQgetvalue(res, 0, 1), "%X/%X", &hi, &lo) != 2)
		{
			fprintf(stderr,
					_("%s: could not parse transaction log location \"%s\"\n"),
					progname, PQgetvalue(res, 0, 1));

			destroyPQExpBuffer(query);
			PQclear(res);
			return false;
		}
		*startpos = ((uint64) hi) << 32 | lo;
	}

	destroyPQExpBuffer(query);
	PQclear(res);
	return true;
}

/*
 * Drop a replication slot for the given connection. This function
 * returns true in case of success.
 */
bool
DropReplicationSlot(PGconn *conn, const char *slot_name)
{
	PQExpBuffer query;
	PGresult   *res;

	Assert(slot_name != NULL);

	query = createPQExpBuffer();

	/* Build query */
	appendPQExpBuffer(query, "DROP_REPLICATION_SLOT \"%s\"",
					  slot_name);
	res = PQexec(conn, query->data);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, _("%s: could not send replication command \"%s\": %s"),
				progname, query->data, PQerrorMessage(conn));

		destroyPQExpBuffer(query);
		PQclear(res);
		return false;
	}

	if (PQntuples(res) != 0 || PQnfields(res) != 0)
	{
		fprintf(stderr,
				_("%s: could not drop replication slot \"%s\": got %d rows and %d fields, expected %d rows and %d fields\n"),
				progname, slot_name,
				PQntuples(res), PQnfields(res), 0, 0);

		destroyPQExpBuffer(query);
		PQclear(res);
		return false;
	}

	PQclear(res);
	return true;
}


/*
 * Frontend version of GetCurrentTimestamp(), since we are not linked with
 * backend code. The replication protocol always uses integer timestamps,
 * regardless of the server setting.
 */
int64
feGetCurrentTimestamp(void)
{
	int64		result;
	struct timeval tp;

	gettimeofday(&tp, NULL);

	result = (int64) tp.tv_sec -
		((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);

	result = (result * USECS_PER_SEC) + tp.tv_usec;

	return result;
}

/*
 * Frontend version of TimestampDifference(), since we are not linked with
 * backend code.
 */
void
feTimestampDifference(int64 start_time, int64 stop_time,
					  long *secs, int *microsecs)
{
	int64		diff = stop_time - start_time;

	if (diff <= 0)
	{
		*secs = 0;
		*microsecs = 0;
	}
	else
	{
		*secs = (long) (diff / USECS_PER_SEC);
		*microsecs = (int) (diff % USECS_PER_SEC);
	}
}

/*
 * Frontend version of TimestampDifferenceExceeds(), since we are not
 * linked with backend code.
 */
bool
feTimestampDifferenceExceeds(int64 start_time,
							 int64 stop_time,
							 int msec)
{
	int64		diff = stop_time - start_time;

	return (diff >= msec * INT64CONST(1000));
}

/*
 * Converts an int64 to network byte order.
 */
void
fe_sendint64(int64 i, char *buf)
{
	uint32		n32;

	/* High order half first, since we're doing MSB-first */
	n32 = (uint32) (i >> 32);
	n32 = htonl(n32);
	memcpy(&buf[0], &n32, 4);

	/* Now the low order half */
	n32 = (uint32) i;
	n32 = htonl(n32);
	memcpy(&buf[4], &n32, 4);
}

/*
 * Converts an int64 from network byte order to native format.
 */
int64
fe_recvint64(char *buf)
{
	int64		result;
	uint32		h32;
	uint32		l32;

	memcpy(&h32, buf, 4);
	memcpy(&l32, buf + 4, 4);
	h32 = ntohl(h32);
	l32 = ntohl(l32);

	result = h32;
	result <<= 32;
	result |= l32;

	return result;
}
