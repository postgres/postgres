/*-------------------------------------------------------------------------
 *
 * streamutil.c - utility functions for pg_basebackup, pg_receivewal and
 *					pg_recvlogical
 *
 * Author: Magnus Hagander <magnus@hagander.net>
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/streamutil.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <sys/time.h>
#include <unistd.h>

/* local includes */
#include "receivelog.h"
#include "streamutil.h"

#include "access/xlog_internal.h"
#include "common/fe_memutils.h"
#include "common/file_perm.h"
#include "common/logging.h"
#include "datatype/timestamp.h"
#include "fe_utils/connect.h"
#include "port/pg_bswap.h"
#include "pqexpbuffer.h"

#define ERRCODE_DUPLICATE_OBJECT  "42710"

uint32		WalSegSz;

static bool RetrieveDataDirCreatePerm(PGconn *conn);

/* SHOW command for replication connection was introduced in version 10 */
#define MINIMUM_VERSION_FOR_SHOW_CMD 100000

/*
 * Group access is supported from version 11.
 */
#define MINIMUM_VERSION_FOR_GROUP_ACCESS 110000

const char *progname;
char	   *connection_string = NULL;
char	   *dbhost = NULL;
char	   *dbuser = NULL;
char	   *dbport = NULL;
char	   *dbname = NULL;
int			dbgetpassword = 0;	/* 0=auto, -1=never, 1=always */
static bool have_password = false;
static char password[100];
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

	/* pg_recvlogical uses dbname only; others use connection_string only. */
	Assert(dbname == NULL || connection_string == NULL);

	/*
	 * Merge the connection info inputs given in form of connection string,
	 * options and default values (dbname=replication, replication=true, etc.)
	 * Explicitly discard any dbname value in the connection string;
	 * otherwise, PQconnectdbParams() would interpret that value as being
	 * itself a connection string.
	 */
	i = 0;
	if (connection_string)
	{
		conn_opts = PQconninfoParse(connection_string, &err_msg);
		if (conn_opts == NULL)
		{
			pg_log_error("%s", err_msg);
			exit(1);
		}

		for (conn_opt = conn_opts; conn_opt->keyword != NULL; conn_opt++)
		{
			if (conn_opt->val != NULL && conn_opt->val[0] != '\0' &&
				strcmp(conn_opt->keyword, "dbname") != 0)
				argcount++;
		}

		keywords = pg_malloc0((argcount + 1) * sizeof(*keywords));
		values = pg_malloc0((argcount + 1) * sizeof(*values));

		for (conn_opt = conn_opts; conn_opt->keyword != NULL; conn_opt++)
		{
			if (conn_opt->val != NULL && conn_opt->val[0] != '\0' &&
				strcmp(conn_opt->keyword, "dbname") != 0)
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
	need_password = (dbgetpassword == 1 && !have_password);

	do
	{
		/* Get a new password if appropriate */
		if (need_password)
		{
			simple_prompt("Password: ", password, sizeof(password), false);
			have_password = true;
			need_password = false;
		}

		/* Use (or reuse, on a subsequent connection) password if we have it */
		if (have_password)
		{
			keywords[i] = "password";
			values[i] = password;
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
			pg_log_error("could not connect to server");
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
		pg_log_error("%s", PQerrorMessage(tmpconn));
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
	 * Set always-secure search path, so malicious users can't get control.
	 * The capacity to run normal SQL queries was added in PostgreSQL 10, so
	 * the search path cannot be changed (by us or attackers) on earlier
	 * versions.
	 */
	if (dbname != NULL && PQserverVersion(tmpconn) >= 100000)
	{
		PGresult   *res;

		res = PQexec(tmpconn, ALWAYS_SECURE_SEARCH_PATH_SQL);
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			pg_log_error("could not clear search_path: %s",
						 PQerrorMessage(tmpconn));
			PQclear(res);
			PQfinish(tmpconn);
			exit(1);
		}
		PQclear(res);
	}

	/*
	 * Ensure we have the same value of integer_datetimes (now always "on") as
	 * the server we are connecting to.
	 */
	tmpparam = PQparameterStatus(tmpconn, "integer_datetimes");
	if (!tmpparam)
	{
		pg_log_error("could not determine server setting for integer_datetimes");
		PQfinish(tmpconn);
		exit(1);
	}

	if (strcmp(tmpparam, "on") != 0)
	{
		pg_log_error("integer_datetimes compile flag does not match server");
		PQfinish(tmpconn);
		exit(1);
	}

	/*
	 * Retrieve the source data directory mode and use it to construct a umask
	 * for creating directories and files.
	 */
	if (!RetrieveDataDirCreatePerm(tmpconn))
	{
		PQfinish(tmpconn);
		exit(1);
	}

	return tmpconn;
}

/*
 * From version 10, explicitly set wal segment size using SHOW wal_segment_size
 * since ControlFile is not accessible here.
 */
bool
RetrieveWalSegSize(PGconn *conn)
{
	PGresult   *res;
	char		xlog_unit[3];
	int			xlog_val,
				multiplier = 1;

	/* check connection existence */
	Assert(conn != NULL);

	/* for previous versions set the default xlog seg size */
	if (PQserverVersion(conn) < MINIMUM_VERSION_FOR_SHOW_CMD)
	{
		WalSegSz = DEFAULT_XLOG_SEG_SIZE;
		return true;
	}

	res = PQexec(conn, "SHOW wal_segment_size");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("could not send replication command \"%s\": %s",
					 "SHOW wal_segment_size", PQerrorMessage(conn));

		PQclear(res);
		return false;
	}
	if (PQntuples(res) != 1 || PQnfields(res) < 1)
	{
		pg_log_error("could not fetch WAL segment size: got %d rows and %d fields, expected %d rows and %d or more fields",
					 PQntuples(res), PQnfields(res), 1, 1);

		PQclear(res);
		return false;
	}

	/* fetch xlog value and unit from the result */
	if (sscanf(PQgetvalue(res, 0, 0), "%d%2s", &xlog_val, xlog_unit) != 2)
	{
		pg_log_error("WAL segment size could not be parsed");
		PQclear(res);
		return false;
	}

	PQclear(res);

	/* set the multiplier based on unit to convert xlog_val to bytes */
	if (strcmp(xlog_unit, "MB") == 0)
		multiplier = 1024 * 1024;
	else if (strcmp(xlog_unit, "GB") == 0)
		multiplier = 1024 * 1024 * 1024;

	/* convert and set WalSegSz */
	WalSegSz = xlog_val * multiplier;

	if (!IsValidWalSegSize(WalSegSz))
	{
		pg_log_error(ngettext("WAL segment size must be a power of two between 1 MB and 1 GB, but the remote server reported a value of %d byte",
							  "WAL segment size must be a power of two between 1 MB and 1 GB, but the remote server reported a value of %d bytes",
							  WalSegSz),
					 WalSegSz);
		return false;
	}

	return true;
}

/*
 * RetrieveDataDirCreatePerm
 *
 * This function is used to determine the privileges on the server's PG data
 * directory and, based on that, set what the permissions will be for
 * directories and files we create.
 *
 * PG11 added support for (optionally) group read/execute rights to be set on
 * the data directory.  Prior to PG11, only the owner was allowed to have rights
 * on the data directory.
 */
static bool
RetrieveDataDirCreatePerm(PGconn *conn)
{
	PGresult   *res;
	int			data_directory_mode;

	/* check connection existence */
	Assert(conn != NULL);

	/* for previous versions leave the default group access */
	if (PQserverVersion(conn) < MINIMUM_VERSION_FOR_GROUP_ACCESS)
		return true;

	res = PQexec(conn, "SHOW data_directory_mode");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("could not send replication command \"%s\": %s",
					 "SHOW data_directory_mode", PQerrorMessage(conn));

		PQclear(res);
		return false;
	}
	if (PQntuples(res) != 1 || PQnfields(res) < 1)
	{
		pg_log_error("could not fetch group access flag: got %d rows and %d fields, expected %d rows and %d or more fields",
					 PQntuples(res), PQnfields(res), 1, 1);

		PQclear(res);
		return false;
	}

	if (sscanf(PQgetvalue(res, 0, 0), "%o", &data_directory_mode) != 1)
	{
		pg_log_error("group access flag could not be parsed: %s",
					 PQgetvalue(res, 0, 0));

		PQclear(res);
		return false;
	}

	SetDataDirectoryCreatePerm(data_directory_mode);

	PQclear(res);
	return true;
}

/*
 * Run IDENTIFY_SYSTEM through a given connection and give back to caller
 * some result information if requested:
 * - System identifier
 * - Current timeline ID
 * - Start LSN position
 * - Database name (NULL in servers prior to 9.4)
 */
bool
RunIdentifySystem(PGconn *conn, char **sysid, TimeLineID *starttli,
				  XLogRecPtr *startpos, char **db_name)
{
	PGresult   *res;
	uint32		hi,
				lo;

	/* Check connection existence */
	Assert(conn != NULL);

	res = PQexec(conn, "IDENTIFY_SYSTEM");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("could not send replication command \"%s\": %s",
					 "IDENTIFY_SYSTEM", PQerrorMessage(conn));

		PQclear(res);
		return false;
	}
	if (PQntuples(res) != 1 || PQnfields(res) < 3)
	{
		pg_log_error("could not identify system: got %d rows and %d fields, expected %d rows and %d or more fields",
					 PQntuples(res), PQnfields(res), 1, 3);

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
			pg_log_error("could not parse write-ahead log location \"%s\"",
						 PQgetvalue(res, 0, 2));

			PQclear(res);
			return false;
		}
		*startpos = ((uint64) hi) << 32 | lo;
	}

	/* Get database name, only available in 9.4 and newer versions */
	if (db_name != NULL)
	{
		*db_name = NULL;
		if (PQserverVersion(conn) >= 90400)
		{
			if (PQnfields(res) < 4)
			{
				pg_log_error("could not identify system: got %d rows and %d fields, expected %d rows and %d or more fields",
							 PQntuples(res), PQnfields(res), 1, 4);

				PQclear(res);
				return false;
			}
			if (!PQgetisnull(res, 0, 3))
				*db_name = pg_strdup(PQgetvalue(res, 0, 3));
		}
	}

	PQclear(res);
	return true;
}

/*
 * Create a replication slot for the given connection. This function
 * returns true in case of success.
 */
bool
CreateReplicationSlot(PGconn *conn, const char *slot_name, const char *plugin,
					  bool is_temporary, bool is_physical, bool reserve_wal,
					  bool slot_exists_ok)
{
	PQExpBuffer query;
	PGresult   *res;

	query = createPQExpBuffer();

	Assert((is_physical && plugin == NULL) ||
		   (!is_physical && plugin != NULL));
	Assert(slot_name != NULL);

	/* Build query */
	appendPQExpBuffer(query, "CREATE_REPLICATION_SLOT \"%s\"", slot_name);
	if (is_temporary)
		appendPQExpBuffer(query, " TEMPORARY");
	if (is_physical)
	{
		appendPQExpBuffer(query, " PHYSICAL");
		if (reserve_wal)
			appendPQExpBuffer(query, " RESERVE_WAL");
	}
	else
	{
		appendPQExpBuffer(query, " LOGICAL \"%s\"", plugin);
		if (PQserverVersion(conn) >= 100000)
			/* pg_recvlogical doesn't use an exported snapshot, so suppress */
			appendPQExpBuffer(query, " NOEXPORT_SNAPSHOT");
	}

	res = PQexec(conn, query->data);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		const char *sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);

		if (slot_exists_ok &&
			sqlstate &&
			strcmp(sqlstate, ERRCODE_DUPLICATE_OBJECT) == 0)
		{
			destroyPQExpBuffer(query);
			PQclear(res);
			return true;
		}
		else
		{
			pg_log_error("could not send replication command \"%s\": %s",
						 query->data, PQerrorMessage(conn));

			destroyPQExpBuffer(query);
			PQclear(res);
			return false;
		}
	}

	if (PQntuples(res) != 1 || PQnfields(res) != 4)
	{
		pg_log_error("could not create replication slot \"%s\": got %d rows and %d fields, expected %d rows and %d fields",
					 slot_name,
					 PQntuples(res), PQnfields(res), 1, 4);

		destroyPQExpBuffer(query);
		PQclear(res);
		return false;
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
		pg_log_error("could not send replication command \"%s\": %s",
					 query->data, PQerrorMessage(conn));

		destroyPQExpBuffer(query);
		PQclear(res);
		return false;
	}

	if (PQntuples(res) != 0 || PQnfields(res) != 0)
	{
		pg_log_error("could not drop replication slot \"%s\": got %d rows and %d fields, expected %d rows and %d fields",
					 slot_name,
					 PQntuples(res), PQnfields(res), 0, 0);

		destroyPQExpBuffer(query);
		PQclear(res);
		return false;
	}

	destroyPQExpBuffer(query);
	PQclear(res);
	return true;
}


/*
 * Frontend version of GetCurrentTimestamp(), since we are not linked with
 * backend code.
 */
TimestampTz
feGetCurrentTimestamp(void)
{
	TimestampTz result;
	struct timeval tp;

	gettimeofday(&tp, NULL);

	result = (TimestampTz) tp.tv_sec -
		((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);
	result = (result * USECS_PER_SEC) + tp.tv_usec;

	return result;
}

/*
 * Frontend version of TimestampDifference(), since we are not linked with
 * backend code.
 */
void
feTimestampDifference(TimestampTz start_time, TimestampTz stop_time,
					  long *secs, int *microsecs)
{
	TimestampTz diff = stop_time - start_time;

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
feTimestampDifferenceExceeds(TimestampTz start_time,
							 TimestampTz stop_time,
							 int msec)
{
	TimestampTz diff = stop_time - start_time;

	return (diff >= msec * INT64CONST(1000));
}

/*
 * Converts an int64 to network byte order.
 */
void
fe_sendint64(int64 i, char *buf)
{
	uint64		n64 = pg_hton64(i);

	memcpy(buf, &n64, sizeof(n64));
}

/*
 * Converts an int64 from network byte order to native format.
 */
int64
fe_recvint64(char *buf)
{
	uint64		n64;

	memcpy(&n64, buf, sizeof(n64));

	return pg_ntoh64(n64);
}
