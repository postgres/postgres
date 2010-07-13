/*
 *	server.c
 *
 *	database server functions
 *
 *	Copyright (c) 2010, PostgreSQL Global Development Group
 *	$PostgreSQL: pgsql/contrib/pg_upgrade/server.c,v 1.8.2.1 2010/07/13 20:15:51 momjian Exp $
 */

#include "pg_upgrade.h"

#define POSTMASTER_UPTIME 20

#define STARTUP_WARNING_TRIES 2


static pgpid_t get_postmaster_pid(migratorContext *ctx, const char *datadir);
static bool test_server_conn(migratorContext *ctx, int timeout,
				 Cluster whichCluster);


/*
 * connectToServer()
 *
 *	Connects to the desired database on the designated server.
 *	If the connection attempt fails, this function logs an error
 *	message and calls exit_nicely() to kill the program.
 */
PGconn *
connectToServer(migratorContext *ctx, const char *db_name,
				Cluster whichCluster)
{
	char		connectString[MAXPGPATH];
	unsigned short port = (whichCluster == CLUSTER_OLD) ?
	ctx->old.port : ctx->new.port;
	PGconn	   *conn;

	snprintf(connectString, sizeof(connectString),
			 "dbname = '%s' user = '%s' port = %d", db_name, ctx->user, port);

	conn = PQconnectdb(connectString);

	if (conn == NULL || PQstatus(conn) != CONNECTION_OK)
	{
		pg_log(ctx, PG_REPORT, "Connection to database failed: %s\n",
			   PQerrorMessage(conn));

		if (conn)
			PQfinish(conn);

		exit_nicely(ctx, true);
	}

	return conn;
}


/*
 * executeQueryOrDie()
 *
 *	Formats a query string from the given arguments and executes the
 *	resulting query.  If the query fails, this function logs an error
 *	message and calls exit_nicely() to kill the program.
 */
PGresult *
executeQueryOrDie(migratorContext *ctx, PGconn *conn, const char *fmt,...)
{
	static char command[8192];
	va_list		args;
	PGresult   *result;
	ExecStatusType status;

	va_start(args, fmt);
	vsnprintf(command, sizeof(command), fmt, args);
	va_end(args);

	pg_log(ctx, PG_DEBUG, "executing: %s\n", command);
	result = PQexec(conn, command);
	status = PQresultStatus(result);

	if ((status != PGRES_TUPLES_OK) && (status != PGRES_COMMAND_OK))
	{
		pg_log(ctx, PG_REPORT, "DB command failed\n%s\n%s\n", command,
			   PQerrorMessage(conn));
		PQclear(result);
		PQfinish(conn);
		exit_nicely(ctx, true);
		return NULL;			/* Never get here, but keeps compiler happy */
	}
	else
		return result;
}


/*
 * get_postmaster_pid()
 *
 * Returns the pid of the postmaster running on datadir. pid is retrieved
 * from the postmaster.pid file
 */
static pgpid_t
get_postmaster_pid(migratorContext *ctx, const char *datadir)
{
	FILE	   *pidf;
	long		pid;
	char		pid_file[MAXPGPATH];

	snprintf(pid_file, sizeof(pid_file), "%s/postmaster.pid", datadir);
	pidf = fopen(pid_file, "r");

	if (pidf == NULL)
		return (pgpid_t) 0;

	if (fscanf(pidf, "%ld", &pid) != 1)
	{
		fclose(pidf);
		pg_log(ctx, PG_FATAL, "%s: invalid data in PID file \"%s\"\n",
			   ctx->progname, pid_file);
	}

	fclose(pidf);

	return (pgpid_t) pid;
}


/*
 * get_major_server_version()
 *
 * gets the version (in unsigned int form) for the given "datadir". Assumes
 * that datadir is an absolute path to a valid pgdata directory. The version
 * is retrieved by reading the PG_VERSION file.
 */
uint32
get_major_server_version(migratorContext *ctx, char **verstr, Cluster whichCluster)
{
	const char *datadir = whichCluster == CLUSTER_OLD ?
	ctx->old.pgdata : ctx->new.pgdata;
	FILE	   *version_fd;
	char		ver_file[MAXPGPATH];
	int			integer_version = 0;
	int			fractional_version = 0;

	*verstr = pg_malloc(ctx, 64);

	snprintf(ver_file, sizeof(ver_file), "%s/PG_VERSION", datadir);
	if ((version_fd = fopen(ver_file, "r")) == NULL)
		return 0;

	if (fscanf(version_fd, "%63s", *verstr) == 0 ||
		sscanf(*verstr, "%d.%d", &integer_version, &fractional_version) != 2)
	{
		pg_log(ctx, PG_FATAL, "could not get version from %s\n", datadir);
		fclose(version_fd);
		return 0;
	}

	return (100 * integer_version + fractional_version) * 100;
}


void
start_postmaster(migratorContext *ctx, Cluster whichCluster, bool quiet)
{
	char		cmd[MAXPGPATH];
	const char *bindir;
	const char *datadir;
	unsigned short port;

	if (whichCluster == CLUSTER_OLD)
	{
		bindir = ctx->old.bindir;
		datadir = ctx->old.pgdata;
		port = ctx->old.port;
	}
	else
	{
		bindir = ctx->new.bindir;
		datadir = ctx->new.pgdata;
		port = ctx->new.port;
	}

	/*
	 * On Win32, we can't send both pg_upgrade output and pg_ctl output to the
	 * same file because we get the error: "The process cannot access the file
	 * because it is being used by another process." so we have to send all other
	 * output to 'nul'.
	 */
	snprintf(cmd, sizeof(cmd),
			 SYSTEMQUOTE "\"%s/pg_ctl\" -l \"%s\" -D \"%s\" "
			 "-o \"-p %d -c autovacuum=off "
			 "-c autovacuum_freeze_max_age=2000000000\" "
			 "start >> \"%s\" 2>&1" SYSTEMQUOTE,
			 bindir,
#ifndef WIN32
			 ctx->logfile, datadir, port, ctx->logfile);
#else
			 DEVNULL, datadir, port, DEVNULL);
#endif
	exec_prog(ctx, true, "%s", cmd);

	/* wait for the server to start properly */

	if (test_server_conn(ctx, POSTMASTER_UPTIME, whichCluster) == false)
		pg_log(ctx, PG_FATAL, " Unable to start %s postmaster with the command: %s\nPerhaps pg_hba.conf was not set to \"trust\".",
			   CLUSTERNAME(whichCluster), cmd);

	if ((ctx->postmasterPID = get_postmaster_pid(ctx, datadir)) == 0)
		pg_log(ctx, PG_FATAL, " Unable to get postmaster pid\n");
	ctx->running_cluster = whichCluster;
}


void
stop_postmaster(migratorContext *ctx, bool fast, bool quiet)
{
	char		cmd[MAXPGPATH];
	const char *bindir;
	const char *datadir;

	if (ctx->running_cluster == CLUSTER_OLD)
	{
		bindir = ctx->old.bindir;
		datadir = ctx->old.pgdata;
	}
	else if (ctx->running_cluster == CLUSTER_NEW)
	{
		bindir = ctx->new.bindir;
		datadir = ctx->new.pgdata;
	}
	else
		return;					/* no cluster running */

	/* See comment in start_postmaster() about why win32 output is ignored. */
	snprintf(cmd, sizeof(cmd),
			 SYSTEMQUOTE "\"%s/pg_ctl\" -l \"%s\" -D \"%s\" %s stop >> "
			 "\"%s\" 2>&1" SYSTEMQUOTE,
			 bindir,
#ifndef WIN32
			 ctx->logfile, datadir, fast ? "-m fast" : "", ctx->logfile);
#else
			 DEVNULL, datadir, fast ? "-m fast" : "", DEVNULL);
#endif
	exec_prog(ctx, fast ? false : true, "%s", cmd);

	ctx->postmasterPID = 0;
	ctx->running_cluster = NONE;
}


/*
 * test_server_conn()
 *
 * tests whether postmaster is running or not by trying to connect
 * to it. If connection is unsuccessfull we do a sleep of 1 sec and then
 * try the connection again. This process continues "timeout" times.
 *
 * Returns true if the connection attempt was successfull, false otherwise.
 */
static bool
test_server_conn(migratorContext *ctx, int timeout, Cluster whichCluster)
{
	PGconn	   *conn = NULL;
	char		con_opts[MAX_STRING];
	int			tries;
	unsigned short port = (whichCluster == CLUSTER_OLD) ?
	ctx->old.port : ctx->new.port;
	bool		ret = false;

	snprintf(con_opts, sizeof(con_opts),
			 "dbname = 'template1' user = '%s' port = %d ", ctx->user, port);

	for (tries = 0; tries < timeout; tries++)
	{
		sleep(1);
		if ((conn = PQconnectdb(con_opts)) != NULL &&
			PQstatus(conn) == CONNECTION_OK)
		{
			PQfinish(conn);
			ret = true;
			break;
		}

		if (tries == STARTUP_WARNING_TRIES)
			prep_status(ctx, "Trying to start %s server ",
						CLUSTERNAME(whichCluster));
		else if (tries > STARTUP_WARNING_TRIES)
			pg_log(ctx, PG_REPORT, ".");
	}

	if (tries > STARTUP_WARNING_TRIES)
		check_ok(ctx);

	return ret;
}


/*
 * check_for_libpq_envvars()
 *
 * tests whether any libpq environment variables are set.
 * Since pg_upgrade connects to both the old and the new server,
 * it is potentially dangerous to have any of these set.
 *
 * If any are found, will log them and cancel.
 */
void
check_for_libpq_envvars(migratorContext *ctx)
{
	PQconninfoOption *option;
	PQconninfoOption *start;
	bool		found = false;

	/* Get valid libpq env vars from the PQconndefaults function */

	start = option = PQconndefaults();

	while (option->keyword != NULL)
	{
		const char *value;

		if (option->envvar && (value = getenv(option->envvar)) && strlen(value) > 0)
		{
			found = true;

			pg_log(ctx, PG_WARNING,
				   "libpq env var %-20s is currently set to: %s\n", option->envvar, value);
		}

		option++;
	}

	/* Free the memory that libpq allocated on our behalf */
	PQconninfoFree(start);

	if (found)
		pg_log(ctx, PG_FATAL,
			   "libpq env vars have been found and listed above, please unset them for pg_upgrade\n");
}
