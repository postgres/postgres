/*
 *	server.c
 *
 *	database server functions
 *
 *	Copyright (c) 2010-2016, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/server.c
 */

#include "postgres_fe.h"

#include "pg_upgrade.h"


static PGconn *get_db_conn(ClusterInfo *cluster, const char *db_name);


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
	PGconn	   *conn = get_db_conn(cluster, db_name);

	if (conn == NULL || PQstatus(conn) != CONNECTION_OK)
	{
		pg_log(PG_REPORT, "connection to database failed: %s\n",
			   PQerrorMessage(conn));

		if (conn)
			PQfinish(conn);

		printf("Failure, exiting\n");
		exit(1);
	}

	return conn;
}


/*
 * get_db_conn()
 *
 * get database connection, using named database + standard params for cluster
 */
static PGconn *
get_db_conn(ClusterInfo *cluster, const char *db_name)
{
	char		conn_opts[2 * NAMEDATALEN + MAXPGPATH + 100];

	if (cluster->sockdir)
		snprintf(conn_opts, sizeof(conn_opts),
				 "dbname = '%s' user = '%s' host = '%s' port = %d",
				 db_name, os_info.user, cluster->sockdir, cluster->port);
	else
		snprintf(conn_opts, sizeof(conn_opts),
				 "dbname = '%s' user = '%s' port = %d",
				 db_name, os_info.user, cluster->port);

	return PQconnectdb(conn_opts);
}


/*
 * cluster_conn_opts()
 *
 * Return standard command-line options for connecting to this cluster when
 * using psql, pg_dump, etc.  Ideally this would match what get_db_conn()
 * sets, but the utilities we need aren't very consistent about the treatment
 * of database name options, so we leave that out.
 *
 * Note result is in static storage, so use it right away.
 */
char *
cluster_conn_opts(ClusterInfo *cluster)
{
	static char conn_opts[MAXPGPATH + NAMEDATALEN + 100];

	if (cluster->sockdir)
		snprintf(conn_opts, sizeof(conn_opts),
				 "--host \"%s\" --port %d --username \"%s\"",
				 cluster->sockdir, cluster->port, os_info.user);
	else
		snprintf(conn_opts, sizeof(conn_opts),
				 "--port %d --username \"%s\"",
				 cluster->port, os_info.user);

	return conn_opts;
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
	static char query[QUERY_ALLOC];
	va_list		args;
	PGresult   *result;
	ExecStatusType status;

	va_start(args, fmt);
	vsnprintf(query, sizeof(query), fmt, args);
	va_end(args);

	pg_log(PG_VERBOSE, "executing: %s\n", query);
	result = PQexec(conn, query);
	status = PQresultStatus(result);

	if ((status != PGRES_TUPLES_OK) && (status != PGRES_COMMAND_OK))
	{
		pg_log(PG_REPORT, "SQL command failed\n%s\n%s\n", query,
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
 * get_major_server_version()
 *
 * gets the version (in unsigned int form) for the given datadir. Assumes
 * that datadir is an absolute path to a valid pgdata directory. The version
 * is retrieved by reading the PG_VERSION file.
 */
uint32
get_major_server_version(ClusterInfo *cluster)
{
	FILE	   *version_fd;
	char		ver_filename[MAXPGPATH];
	int			integer_version = 0;
	int			fractional_version = 0;

	snprintf(ver_filename, sizeof(ver_filename), "%s/PG_VERSION",
			 cluster->pgdata);
	if ((version_fd = fopen(ver_filename, "r")) == NULL)
		pg_fatal("could not open version file: %s\n", ver_filename);

	if (fscanf(version_fd, "%63s", cluster->major_version_str) == 0 ||
		sscanf(cluster->major_version_str, "%d.%d", &integer_version,
			   &fractional_version) != 2)
		pg_fatal("could not get version from %s\n", cluster->pgdata);

	fclose(version_fd);

	return (100 * integer_version + fractional_version) * 100;
}


static void
stop_postmaster_atexit(void)
{
	stop_postmaster(true);
}


bool
start_postmaster(ClusterInfo *cluster, bool throw_error)
{
	char		cmd[MAXPGPATH * 4 + 1000];
	PGconn	   *conn;
	bool		exit_hook_registered = false;
	bool		pg_ctl_return = false;
	char		socket_string[MAXPGPATH + 200];

	if (!exit_hook_registered)
	{
		atexit(stop_postmaster_atexit);
		exit_hook_registered = true;
	}

	socket_string[0] = '\0';

#ifdef HAVE_UNIX_SOCKETS
	/* prevent TCP/IP connections, restrict socket access */
	strcat(socket_string,
		   " -c listen_addresses='' -c unix_socket_permissions=0700");

	/* Have a sockdir?	Tell the postmaster. */
	if (cluster->sockdir)
		snprintf(socket_string + strlen(socket_string),
				 sizeof(socket_string) - strlen(socket_string),
				 " -c %s='%s'",
				 (GET_MAJOR_VERSION(cluster->major_version) < 903) ?
				 "unix_socket_directory" : "unix_socket_directories",
				 cluster->sockdir);
#endif

	/*
	 * Since PG 9.1, we have used -b to disable autovacuum.  For earlier
	 * releases, setting autovacuum=off disables cleanup vacuum and analyze,
	 * but freeze vacuums can still happen, so we set
	 * autovacuum_freeze_max_age to its maximum.
	 * (autovacuum_multixact_freeze_max_age was introduced after 9.1, so there
	 * is no need to set that.)  We assume all datfrozenxid and relfrozenxid
	 * values are less than a gap of 2000000000 from the current xid counter,
	 * so autovacuum will not touch them.
	 *
	 * Turn off durability requirements to improve object creation speed, and
	 * we only modify the new cluster, so only use it there.  If there is a
	 * crash, the new cluster has to be recreated anyway.  fsync=off is a big
	 * win on ext4.
	 */
	snprintf(cmd, sizeof(cmd),
		  "\"%s/pg_ctl\" -w -l \"%s\" -D \"%s\" -o \"-p %d%s%s %s%s\" start",
		  cluster->bindir, SERVER_LOG_FILE, cluster->pgconfig, cluster->port,
			 (cluster->controldata.cat_ver >=
			  BINARY_UPGRADE_SERVER_FLAG_CAT_VER) ? " -b" :
			 " -c autovacuum=off -c autovacuum_freeze_max_age=2000000000",
			 (cluster == &new_cluster) ?
	  " -c synchronous_commit=off -c fsync=off -c full_page_writes=off" : "",
			 cluster->pgopts ? cluster->pgopts : "", socket_string);

	/*
	 * Don't throw an error right away, let connecting throw the error because
	 * it might supply a reason for the failure.
	 */
	pg_ctl_return = exec_prog(SERVER_START_LOG_FILE,
	/* pass both file names if they differ */
							  (strcmp(SERVER_LOG_FILE,
									  SERVER_START_LOG_FILE) != 0) ?
							  SERVER_LOG_FILE : NULL,
							  false,
							  "%s", cmd);

	/* Did it fail and we are just testing if the server could be started? */
	if (!pg_ctl_return && !throw_error)
		return false;

	/*
	 * We set this here to make sure atexit() shuts down the server, but only
	 * if we started the server successfully.  We do it before checking for
	 * connectivity in case the server started but there is a connectivity
	 * failure.  If pg_ctl did not return success, we will exit below.
	 *
	 * Pre-9.1 servers do not have PQping(), so we could be leaving the server
	 * running if authentication was misconfigured, so someday we might went
	 * to be more aggressive about doing server shutdowns even if pg_ctl
	 * fails, but now (2013-08-14) it seems prudent to be cautious.  We don't
	 * want to shutdown a server that might have been accidentally started
	 * during the upgrade.
	 */
	if (pg_ctl_return)
		os_info.running_cluster = cluster;

	/*
	 * pg_ctl -w might have failed because the server couldn't be started, or
	 * there might have been a connection problem in _checking_ if the server
	 * has started.  Therefore, even if pg_ctl failed, we continue and test
	 * for connectivity in case we get a connection reason for the failure.
	 */
	if ((conn = get_db_conn(cluster, "template1")) == NULL ||
		PQstatus(conn) != CONNECTION_OK)
	{
		pg_log(PG_REPORT, "\nconnection to database failed: %s\n",
			   PQerrorMessage(conn));
		if (conn)
			PQfinish(conn);
		pg_fatal("could not connect to %s postmaster started with the command:\n"
				 "%s\n",
				 CLUSTER_NAME(cluster), cmd);
	}
	PQfinish(conn);

	/*
	 * If pg_ctl failed, and the connection didn't fail, and throw_error is
	 * enabled, fail now.  This could happen if the server was already
	 * running.
	 */
	if (!pg_ctl_return)
		pg_fatal("pg_ctl failed to start the %s server, or connection failed\n",
				 CLUSTER_NAME(cluster));

	return true;
}


void
stop_postmaster(bool fast)
{
	ClusterInfo *cluster;

	if (os_info.running_cluster == &old_cluster)
		cluster = &old_cluster;
	else if (os_info.running_cluster == &new_cluster)
		cluster = &new_cluster;
	else
		return;					/* no cluster running */

	exec_prog(SERVER_STOP_LOG_FILE, NULL, !fast,
			  "\"%s/pg_ctl\" -w -D \"%s\" -o \"%s\" %s stop",
			  cluster->bindir, cluster->pgconfig,
			  cluster->pgopts ? cluster->pgopts : "",
			  fast ? "-m fast" : "");

	os_info.running_cluster = NULL;
}


/*
 * check_pghost_envvar()
 *
 * Tests that PGHOST does not point to a non-local server
 */
void
check_pghost_envvar(void)
{
	PQconninfoOption *option;
	PQconninfoOption *start;

	/* Get valid libpq env vars from the PQconndefaults function */

	start = PQconndefaults();

	if (!start)
		pg_fatal("out of memory\n");

	for (option = start; option->keyword != NULL; option++)
	{
		if (option->envvar && (strcmp(option->envvar, "PGHOST") == 0 ||
							   strcmp(option->envvar, "PGHOSTADDR") == 0))
		{
			const char *value = getenv(option->envvar);

			if (value && strlen(value) > 0 &&
			/* check for 'local' host values */
				(strcmp(value, "localhost") != 0 && strcmp(value, "127.0.0.1") != 0 &&
				 strcmp(value, "::1") != 0 && value[0] != '/'))
				pg_fatal("libpq environment variable %s has a non-local server value: %s\n",
						 option->envvar, value);
		}
	}

	/* Free the memory that libpq allocated on our behalf */
	PQconninfoFree(start);
}
