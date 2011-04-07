/*
 *	server.c
 *
 *	database server functions
 *
 *	Copyright (c) 2010-2011, PostgreSQL Global Development Group
 *	contrib/pg_upgrade/server.c
 */

#include "pg_upgrade.h"

#define POSTMASTER_UPTIME 20

#define STARTUP_WARNING_TRIES 2


static pgpid_t get_postmaster_pid(const char *datadir);
static bool test_server_conn(ClusterInfo *cluster, int timeout);


/*
 * connectToServer()
 *
 *	Connects to the desired database on the designated server.
 *	If the connection attempt fails, this function logs an error
 *	message and calls exit() to kill the program.
 */
PGconn *
connectToServer(ClusterInfo *cluster, const char *db_name)
{
	unsigned short port = cluster->port;
	char		connectString[MAXPGPATH];
	PGconn	   *conn;

	snprintf(connectString, sizeof(connectString),
		 "dbname = '%s' user = '%s' port = %d", db_name, os_info.user, port);

	conn = PQconnectdb(connectString);

	if (conn == NULL || PQstatus(conn) != CONNECTION_OK)
	{
		pg_log(PG_REPORT, "Connection to database failed: %s\n",
			   PQerrorMessage(conn));

		if (conn)
			PQfinish(conn);

		printf("Failure, exiting\n");
		exit(1);
	}

	return conn;
}


/*
 * executeQueryOrDie()
 *
 *	Formats a query string from the given arguments and executes the
 *	resulting query.  If the query fails, this function logs an error
 *	message and calls exit() to kill the program.
 */
PGresult *
executeQueryOrDie(PGconn *conn, const char *fmt,...)
{
	static char command[8192];
	va_list		args;
	PGresult   *result;
	ExecStatusType status;

	va_start(args, fmt);
	vsnprintf(command, sizeof(command), fmt, args);
	va_end(args);

	pg_log(PG_DEBUG, "executing: %s\n", command);
	result = PQexec(conn, command);
	status = PQresultStatus(result);

	if ((status != PGRES_TUPLES_OK) && (status != PGRES_COMMAND_OK))
	{
		pg_log(PG_REPORT, "DB command failed\n%s\n%s\n", command,
			   PQerrorMessage(conn));
		PQclear(result);
		PQfinish(conn);
		printf("Failure, exiting\n");
		exit(1);
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
get_postmaster_pid(const char *datadir)
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
		pg_log(PG_FATAL, "%s: invalid data in PID file \"%s\"\n",
			   os_info.progname, pid_file);
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
get_major_server_version(ClusterInfo *cluster)
{
	const char *datadir = cluster->pgdata;
	FILE	   *version_fd;
	char		ver_filename[MAXPGPATH];
	int			integer_version = 0;
	int			fractional_version = 0;

	snprintf(ver_filename, sizeof(ver_filename), "%s/PG_VERSION", datadir);
	if ((version_fd = fopen(ver_filename, "r")) == NULL)
		return 0;

	if (fscanf(version_fd, "%63s", cluster->major_version_str) == 0 ||
		sscanf(cluster->major_version_str, "%d.%d", &integer_version,
			   &fractional_version) != 2)
		pg_log(PG_FATAL, "could not get version from %s\n", datadir);

	fclose(version_fd);

	return (100 * integer_version + fractional_version) * 100;
}


static void
#ifdef HAVE_ATEXIT
stop_postmaster_atexit(void)
#else
stop_postmaster_on_exit(int exitstatus, void *arg)
#endif
{
	stop_postmaster(true, true);

}


void
start_postmaster(ClusterInfo *cluster, bool quiet)
{
	char		cmd[MAXPGPATH];
	const char *bindir;
	const char *datadir;
	unsigned short port;
	bool		exit_hook_registered = false;

	bindir = cluster->bindir;
	datadir = cluster->pgdata;
	port = cluster->port;

	if (!exit_hook_registered)
	{
#ifdef HAVE_ATEXIT
		atexit(stop_postmaster_atexit);
#else
		on_exit(stop_postmaster_on_exit);
#endif
		exit_hook_registered = true;
	}

	/*
	 * On Win32, we can't send both pg_upgrade output and pg_ctl output to the
	 * same file because we get the error: "The process cannot access the file
	 * because it is being used by another process." so we have to send all
	 * other output to 'nul'.
	 *
	 * Using autovacuum=off disables cleanup vacuum and analyze, but
	 * freeze vacuums can still happen, so we set
	 * autovacuum_freeze_max_age to its maximum.  We assume all datfrozenxid
	 * and relfrozen values are less than a gap of 2000000000 from the current
	 * xid counter, so autovacuum will not touch them.
	 */	
	snprintf(cmd, sizeof(cmd),
			 SYSTEMQUOTE "\"%s/pg_ctl\" -l \"%s\" -D \"%s\" "
			 "-o \"-p %d -c autovacuum=off "
			 "-c autovacuum_freeze_max_age=2000000000\" "
			 "start >> \"%s\" 2>&1" SYSTEMQUOTE,
			 bindir,
#ifndef WIN32
			 log_opts.filename, datadir, port, log_opts.filename);
#else
			 DEVNULL, datadir, port, DEVNULL);
#endif
	exec_prog(true, "%s", cmd);

	/* wait for the server to start properly */

	if (test_server_conn(cluster, POSTMASTER_UPTIME) == false)
		pg_log(PG_FATAL, " Unable to start %s postmaster with the command: %s\nPerhaps pg_hba.conf was not set to \"trust\".",
			   CLUSTER_NAME(cluster), cmd);

	if ((os_info.postmasterPID = get_postmaster_pid(datadir)) == 0)
		pg_log(PG_FATAL, " Unable to get postmaster pid\n");
	os_info.running_cluster = cluster;
}


void
stop_postmaster(bool fast, bool quiet)
{
	char		cmd[MAXPGPATH];
	const char *bindir;
	const char *datadir;

	if (os_info.running_cluster == &old_cluster)
	{
		bindir = old_cluster.bindir;
		datadir = old_cluster.pgdata;
	}
	else if (os_info.running_cluster == &new_cluster)
	{
		bindir = new_cluster.bindir;
		datadir = new_cluster.pgdata;
	}
	else
		return;					/* no cluster running */

	/* See comment in start_postmaster() about why win32 output is ignored. */
	snprintf(cmd, sizeof(cmd),
			 SYSTEMQUOTE "\"%s/pg_ctl\" -l \"%s\" -D \"%s\" %s stop >> "
			 "\"%s\" 2>&1" SYSTEMQUOTE,
			 bindir,
#ifndef WIN32
			 log_opts.filename, datadir, fast ? "-m fast" : "", log_opts.filename);
#else
			 DEVNULL, datadir, fast ? "-m fast" : "", DEVNULL);
#endif
	exec_prog(fast ? false : true, "%s", cmd);

	os_info.postmasterPID = 0;
	os_info.running_cluster = NULL;
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
test_server_conn(ClusterInfo *cluster, int timeout)
{
	unsigned short port = cluster->port;
	PGconn	   *conn = NULL;
	char		con_opts[MAX_STRING];
	int			tries;
	bool		ret = false;

	snprintf(con_opts, sizeof(con_opts),
		  "dbname = 'template1' user = '%s' port = %d ", os_info.user, port);

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
			prep_status("Trying to start %s server ",
						CLUSTER_NAME(cluster));
		else if (tries > STARTUP_WARNING_TRIES)
			pg_log(PG_REPORT, ".");
	}

	if (tries > STARTUP_WARNING_TRIES)
		check_ok();

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
check_for_libpq_envvars(void)
{
	PQconninfoOption *option;
	PQconninfoOption *start;
	bool		found = false;

	/* Get valid libpq env vars from the PQconndefaults function */

	start = PQconndefaults();

	for (option = start; option->keyword != NULL; option++)
	{
		if (option->envvar)
		{
			const char *value;

			if (strcmp(option->envvar, "PGCLIENTENCODING") == 0)
				continue;

			value = getenv(option->envvar);
			if (value && strlen(value) > 0)
			{
				found = true;

				pg_log(PG_WARNING,
					   "libpq env var %-20s is currently set to: %s\n", option->envvar, value);
			}
		}
	}

	/* Free the memory that libpq allocated on our behalf */
	PQconninfoFree(start);

	if (found)
		pg_log(PG_FATAL,
			   "libpq env vars have been found and listed above, please unset them for pg_upgrade\n");
}
