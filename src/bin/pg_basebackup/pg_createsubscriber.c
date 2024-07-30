/*-------------------------------------------------------------------------
 *
 * pg_createsubscriber.c
 *	  Create a new logical replica from a standby server
 *
 * Copyright (C) 2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/bin/pg_basebackup/pg_createsubscriber.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>

#include "catalog/pg_authid_d.h"
#include "common/connect.h"
#include "common/controldata_utils.h"
#include "common/file_perm.h"
#include "common/logging.h"
#include "common/pg_prng.h"
#include "common/restricted_token.h"
#include "fe_utils/recovery_gen.h"
#include "fe_utils/simple_list.h"
#include "fe_utils/string_utils.h"
#include "getopt_long.h"

#define	DEFAULT_SUB_PORT	"50432"

/* Command-line options */
struct CreateSubscriberOptions
{
	char	   *config_file;	/* configuration file */
	char	   *pub_conninfo_str;	/* publisher connection string */
	char	   *socket_dir;		/* directory for Unix-domain socket, if any */
	char	   *sub_port;		/* subscriber port number */
	const char *sub_username;	/* subscriber username */
	SimpleStringList database_names;	/* list of database names */
	SimpleStringList pub_names; /* list of publication names */
	SimpleStringList sub_names; /* list of subscription names */
	SimpleStringList replslot_names;	/* list of replication slot names */
	int			recovery_timeout;	/* stop recovery after this time */
};

struct LogicalRepInfo
{
	char	   *dbname;			/* database name */
	char	   *pubconninfo;	/* publisher connection string */
	char	   *subconninfo;	/* subscriber connection string */
	char	   *pubname;		/* publication name */
	char	   *subname;		/* subscription name */
	char	   *replslotname;	/* replication slot name */

	bool		made_replslot;	/* replication slot was created */
	bool		made_publication;	/* publication was created */
};

static void cleanup_objects_atexit(void);
static void usage();
static char *get_base_conninfo(const char *conninfo, char **dbname);
static char *get_sub_conninfo(const struct CreateSubscriberOptions *opt);
static char *get_exec_path(const char *argv0, const char *progname);
static void check_data_directory(const char *datadir);
static char *concat_conninfo_dbname(const char *conninfo, const char *dbname);
static struct LogicalRepInfo *store_pub_sub_info(const struct CreateSubscriberOptions *opt,
												 const char *pub_base_conninfo,
												 const char *sub_base_conninfo);
static PGconn *connect_database(const char *conninfo, bool exit_on_error);
static void disconnect_database(PGconn *conn, bool exit_on_error);
static uint64 get_primary_sysid(const char *conninfo);
static uint64 get_standby_sysid(const char *datadir);
static void modify_subscriber_sysid(const struct CreateSubscriberOptions *opt);
static bool server_is_in_recovery(PGconn *conn);
static char *generate_object_name(PGconn *conn);
static void check_publisher(const struct LogicalRepInfo *dbinfo);
static char *setup_publisher(struct LogicalRepInfo *dbinfo);
static void check_subscriber(const struct LogicalRepInfo *dbinfo);
static void setup_subscriber(struct LogicalRepInfo *dbinfo,
							 const char *consistent_lsn);
static void setup_recovery(const struct LogicalRepInfo *dbinfo, const char *datadir,
						   const char *lsn);
static void drop_primary_replication_slot(struct LogicalRepInfo *dbinfo,
										  const char *slotname);
static void drop_failover_replication_slots(struct LogicalRepInfo *dbinfo);
static char *create_logical_replication_slot(PGconn *conn,
											 struct LogicalRepInfo *dbinfo);
static void drop_replication_slot(PGconn *conn, struct LogicalRepInfo *dbinfo,
								  const char *slot_name);
static void pg_ctl_status(const char *pg_ctl_cmd, int rc);
static void start_standby_server(const struct CreateSubscriberOptions *opt,
								 bool restricted_access,
								 bool restrict_logical_worker);
static void stop_standby_server(const char *datadir);
static void wait_for_end_recovery(const char *conninfo,
								  const struct CreateSubscriberOptions *opt);
static void create_publication(PGconn *conn, struct LogicalRepInfo *dbinfo);
static void drop_publication(PGconn *conn, struct LogicalRepInfo *dbinfo);
static void create_subscription(PGconn *conn, const struct LogicalRepInfo *dbinfo);
static void set_replication_progress(PGconn *conn, const struct LogicalRepInfo *dbinfo,
									 const char *lsn);
static void enable_subscription(PGconn *conn, const struct LogicalRepInfo *dbinfo);
static void check_and_drop_existing_subscriptions(PGconn *conn,
												  const struct LogicalRepInfo *dbinfo);
static void drop_existing_subscriptions(PGconn *conn, const char *subname,
										const char *dbname);

#define	USEC_PER_SEC	1000000
#define	WAIT_INTERVAL	1		/* 1 second */

static const char *progname;

static char *primary_slot_name = NULL;
static bool dry_run = false;

static bool success = false;

static struct LogicalRepInfo *dbinfo;
static int	num_dbs = 0;		/* number of specified databases */
static int	num_pubs = 0;		/* number of specified publications */
static int	num_subs = 0;		/* number of specified subscriptions */
static int	num_replslots = 0;	/* number of specified replication slots */

static pg_prng_state prng_state;

static char *pg_ctl_path = NULL;
static char *pg_resetwal_path = NULL;

/* standby / subscriber data directory */
static char *subscriber_dir = NULL;

static bool recovery_ended = false;
static bool standby_running = false;

enum WaitPMResult
{
	POSTMASTER_READY,
	POSTMASTER_STILL_STARTING
};


/*
 * Cleanup objects that were created by pg_createsubscriber if there is an
 * error.
 *
 * Publications and replication slots are created on primary. Depending on the
 * step it failed, it should remove the already created objects if it is
 * possible (sometimes it won't work due to a connection issue).
 * There is no cleanup on the target server. The steps on the target server are
 * executed *after* promotion, hence, at this point, a failure means recreate
 * the physical replica and start again.
 */
static void
cleanup_objects_atexit(void)
{
	if (success)
		return;

	/*
	 * If the server is promoted, there is no way to use the current setup
	 * again. Warn the user that a new replication setup should be done before
	 * trying again.
	 */
	if (recovery_ended)
	{
		pg_log_warning("failed after the end of recovery");
		pg_log_warning_hint("The target server cannot be used as a physical replica anymore.  "
							"You must recreate the physical replica before continuing.");
	}

	for (int i = 0; i < num_dbs; i++)
	{
		if (dbinfo[i].made_publication || dbinfo[i].made_replslot)
		{
			PGconn	   *conn;

			conn = connect_database(dbinfo[i].pubconninfo, false);
			if (conn != NULL)
			{
				if (dbinfo[i].made_publication)
					drop_publication(conn, &dbinfo[i]);
				if (dbinfo[i].made_replslot)
					drop_replication_slot(conn, &dbinfo[i], dbinfo[i].replslotname);
				disconnect_database(conn, false);
			}
			else
			{
				/*
				 * If a connection could not be established, inform the user
				 * that some objects were left on primary and should be
				 * removed before trying again.
				 */
				if (dbinfo[i].made_publication)
				{
					pg_log_warning("publication \"%s\" in database \"%s\" on primary might be left behind",
								   dbinfo[i].pubname, dbinfo[i].dbname);
					pg_log_warning_hint("Consider dropping this publication before trying again.");
				}
				if (dbinfo[i].made_replslot)
				{
					pg_log_warning("replication slot \"%s\" in database \"%s\" on primary might be left behind",
								   dbinfo[i].replslotname, dbinfo[i].dbname);
					pg_log_warning_hint("Drop this replication slot soon to avoid retention of WAL files.");
				}
			}
		}
	}

	if (standby_running)
		stop_standby_server(subscriber_dir);
}

static void
usage(void)
{
	printf(_("%s creates a new logical replica from a standby server.\n\n"),
		   progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]...\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -d, --database=DBNAME           database to create a subscription\n"));
	printf(_("  -D, --pgdata=DATADIR            location for the subscriber data directory\n"));
	printf(_("  -n, --dry-run                   dry run, just show what would be done\n"));
	printf(_("  -p, --subscriber-port=PORT      subscriber port number (default %s)\n"), DEFAULT_SUB_PORT);
	printf(_("  -P, --publisher-server=CONNSTR  publisher connection string\n"));
	printf(_("  -s, --socket-directory=DIR      socket directory to use (default current directory)\n"));
	printf(_("  -t, --recovery-timeout=SECS     seconds to wait for recovery to end\n"));
	printf(_("  -U, --subscriber-username=NAME  subscriber username\n"));
	printf(_("  -v, --verbose                   output verbose messages\n"));
	printf(_("      --config-file=FILENAME      use specified main server configuration\n"
			 "                                  file when running target cluster\n"));
	printf(_("      --publication=NAME          publication name\n"));
	printf(_("      --replication-slot=NAME     replication slot name\n"));
	printf(_("      --subscription=NAME         subscription name\n"));
	printf(_("  -V, --version                   output version information, then exit\n"));
	printf(_("  -?, --help                      show this help, then exit\n"));
	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}

/*
 * Subroutine to append "keyword=value" to a connection string,
 * with proper quoting of the value.  (We assume keywords don't need that.)
 */
static void
appendConnStrItem(PQExpBuffer buf, const char *keyword, const char *val)
{
	if (buf->len > 0)
		appendPQExpBufferChar(buf, ' ');
	appendPQExpBufferStr(buf, keyword);
	appendPQExpBufferChar(buf, '=');
	appendConnStrVal(buf, val);
}

/*
 * Validate a connection string. Returns a base connection string that is a
 * connection string without a database name.
 *
 * Since we might process multiple databases, each database name will be
 * appended to this base connection string to provide a final connection
 * string. If the second argument (dbname) is not null, returns dbname if the
 * provided connection string contains it.
 *
 * It is the caller's responsibility to free the returned connection string and
 * dbname.
 */
static char *
get_base_conninfo(const char *conninfo, char **dbname)
{
	PQExpBuffer buf;
	PQconninfoOption *conn_opts;
	PQconninfoOption *conn_opt;
	char	   *errmsg = NULL;
	char	   *ret;

	conn_opts = PQconninfoParse(conninfo, &errmsg);
	if (conn_opts == NULL)
	{
		pg_log_error("could not parse connection string: %s", errmsg);
		PQfreemem(errmsg);
		return NULL;
	}

	buf = createPQExpBuffer();
	for (conn_opt = conn_opts; conn_opt->keyword != NULL; conn_opt++)
	{
		if (conn_opt->val != NULL && conn_opt->val[0] != '\0')
		{
			if (strcmp(conn_opt->keyword, "dbname") == 0)
			{
				if (dbname)
					*dbname = pg_strdup(conn_opt->val);
				continue;
			}
			appendConnStrItem(buf, conn_opt->keyword, conn_opt->val);
		}
	}

	ret = pg_strdup(buf->data);

	destroyPQExpBuffer(buf);
	PQconninfoFree(conn_opts);

	return ret;
}

/*
 * Build a subscriber connection string. Only a few parameters are supported
 * since it starts a server with restricted access.
 */
static char *
get_sub_conninfo(const struct CreateSubscriberOptions *opt)
{
	PQExpBuffer buf = createPQExpBuffer();
	char	   *ret;

	appendConnStrItem(buf, "port", opt->sub_port);
#if !defined(WIN32)
	appendConnStrItem(buf, "host", opt->socket_dir);
#endif
	if (opt->sub_username != NULL)
		appendConnStrItem(buf, "user", opt->sub_username);
	appendConnStrItem(buf, "fallback_application_name", progname);

	ret = pg_strdup(buf->data);

	destroyPQExpBuffer(buf);

	return ret;
}

/*
 * Verify if a PostgreSQL binary (progname) is available in the same directory as
 * pg_createsubscriber and it has the same version.  It returns the absolute
 * path of the progname.
 */
static char *
get_exec_path(const char *argv0, const char *progname)
{
	char	   *versionstr;
	char	   *exec_path;
	int			ret;

	versionstr = psprintf("%s (PostgreSQL) %s\n", progname, PG_VERSION);
	exec_path = pg_malloc(MAXPGPATH);
	ret = find_other_exec(argv0, progname, versionstr, exec_path);

	if (ret < 0)
	{
		char		full_path[MAXPGPATH];

		if (find_my_exec(argv0, full_path) < 0)
			strlcpy(full_path, progname, sizeof(full_path));

		if (ret == -1)
			pg_fatal("program \"%s\" is needed by %s but was not found in the same directory as \"%s\"",
					 progname, "pg_createsubscriber", full_path);
		else
			pg_fatal("program \"%s\" was found by \"%s\" but was not the same version as %s",
					 progname, full_path, "pg_createsubscriber");
	}

	pg_log_debug("%s path is:  %s", progname, exec_path);

	return exec_path;
}

/*
 * Is it a cluster directory? These are preliminary checks. It is far from
 * making an accurate check. If it is not a clone from the publisher, it will
 * eventually fail in a future step.
 */
static void
check_data_directory(const char *datadir)
{
	struct stat statbuf;
	char		versionfile[MAXPGPATH];

	pg_log_info("checking if directory \"%s\" is a cluster data directory",
				datadir);

	if (stat(datadir, &statbuf) != 0)
	{
		if (errno == ENOENT)
			pg_fatal("data directory \"%s\" does not exist", datadir);
		else
			pg_fatal("could not access directory \"%s\": %m", datadir);
	}

	snprintf(versionfile, MAXPGPATH, "%s/PG_VERSION", datadir);
	if (stat(versionfile, &statbuf) != 0 && errno == ENOENT)
	{
		pg_fatal("directory \"%s\" is not a database cluster directory",
				 datadir);
	}
}

/*
 * Append database name into a base connection string.
 *
 * dbname is the only parameter that changes so it is not included in the base
 * connection string. This function concatenates dbname to build a "real"
 * connection string.
 */
static char *
concat_conninfo_dbname(const char *conninfo, const char *dbname)
{
	PQExpBuffer buf = createPQExpBuffer();
	char	   *ret;

	Assert(conninfo != NULL);

	appendPQExpBufferStr(buf, conninfo);
	appendConnStrItem(buf, "dbname", dbname);

	ret = pg_strdup(buf->data);
	destroyPQExpBuffer(buf);

	return ret;
}

/*
 * Store publication and subscription information.
 *
 * If publication, replication slot and subscription names were specified,
 * store it here. Otherwise, a generated name will be assigned to the object in
 * setup_publisher().
 */
static struct LogicalRepInfo *
store_pub_sub_info(const struct CreateSubscriberOptions *opt,
				   const char *pub_base_conninfo,
				   const char *sub_base_conninfo)
{
	struct LogicalRepInfo *dbinfo;
	SimpleStringListCell *pubcell = NULL;
	SimpleStringListCell *subcell = NULL;
	SimpleStringListCell *replslotcell = NULL;
	int			i = 0;

	dbinfo = pg_malloc_array(struct LogicalRepInfo, num_dbs);

	if (num_pubs > 0)
		pubcell = opt->pub_names.head;
	if (num_subs > 0)
		subcell = opt->sub_names.head;
	if (num_replslots > 0)
		replslotcell = opt->replslot_names.head;

	for (SimpleStringListCell *cell = opt->database_names.head; cell; cell = cell->next)
	{
		char	   *conninfo;

		/* Fill publisher attributes */
		conninfo = concat_conninfo_dbname(pub_base_conninfo, cell->val);
		dbinfo[i].pubconninfo = conninfo;
		dbinfo[i].dbname = cell->val;
		if (num_pubs > 0)
			dbinfo[i].pubname = pubcell->val;
		else
			dbinfo[i].pubname = NULL;
		if (num_replslots > 0)
			dbinfo[i].replslotname = replslotcell->val;
		else
			dbinfo[i].replslotname = NULL;
		dbinfo[i].made_replslot = false;
		dbinfo[i].made_publication = false;
		/* Fill subscriber attributes */
		conninfo = concat_conninfo_dbname(sub_base_conninfo, cell->val);
		dbinfo[i].subconninfo = conninfo;
		if (num_subs > 0)
			dbinfo[i].subname = subcell->val;
		else
			dbinfo[i].subname = NULL;
		/* Other fields will be filled later */

		pg_log_debug("publisher(%d): publication: %s ; replication slot: %s ; connection string: %s", i,
					 dbinfo[i].pubname ? dbinfo[i].pubname : "(auto)",
					 dbinfo[i].replslotname ? dbinfo[i].replslotname : "(auto)",
					 dbinfo[i].pubconninfo);
		pg_log_debug("subscriber(%d): subscription: %s ; connection string: %s", i,
					 dbinfo[i].subname ? dbinfo[i].subname : "(auto)",
					 dbinfo[i].subconninfo);

		if (num_pubs > 0)
			pubcell = pubcell->next;
		if (num_subs > 0)
			subcell = subcell->next;
		if (num_replslots > 0)
			replslotcell = replslotcell->next;

		i++;
	}

	return dbinfo;
}

/*
 * Open a new connection. If exit_on_error is true, it has an undesired
 * condition and it should exit immediately.
 */
static PGconn *
connect_database(const char *conninfo, bool exit_on_error)
{
	PGconn	   *conn;
	PGresult   *res;

	conn = PQconnectdb(conninfo);
	if (PQstatus(conn) != CONNECTION_OK)
	{
		pg_log_error("connection to database failed: %s",
					 PQerrorMessage(conn));
		PQfinish(conn);

		if (exit_on_error)
			exit(1);
		return NULL;
	}

	/* Secure search_path */
	res = PQexec(conn, ALWAYS_SECURE_SEARCH_PATH_SQL);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("could not clear search_path: %s",
					 PQresultErrorMessage(res));
		PQclear(res);
		PQfinish(conn);

		if (exit_on_error)
			exit(1);
		return NULL;
	}
	PQclear(res);

	return conn;
}

/*
 * Close the connection. If exit_on_error is true, it has an undesired
 * condition and it should exit immediately.
 */
static void
disconnect_database(PGconn *conn, bool exit_on_error)
{
	Assert(conn != NULL);

	PQfinish(conn);

	if (exit_on_error)
		exit(1);
}

/*
 * Obtain the system identifier using the provided connection. It will be used
 * to compare if a data directory is a clone of another one.
 */
static uint64
get_primary_sysid(const char *conninfo)
{
	PGconn	   *conn;
	PGresult   *res;
	uint64		sysid;

	pg_log_info("getting system identifier from publisher");

	conn = connect_database(conninfo, true);

	res = PQexec(conn, "SELECT system_identifier FROM pg_catalog.pg_control_system()");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("could not get system identifier: %s",
					 PQresultErrorMessage(res));
		disconnect_database(conn, true);
	}
	if (PQntuples(res) != 1)
	{
		pg_log_error("could not get system identifier: got %d rows, expected %d row",
					 PQntuples(res), 1);
		disconnect_database(conn, true);
	}

	sysid = strtou64(PQgetvalue(res, 0, 0), NULL, 10);

	pg_log_info("system identifier is %llu on publisher",
				(unsigned long long) sysid);

	PQclear(res);
	disconnect_database(conn, false);

	return sysid;
}

/*
 * Obtain the system identifier from control file. It will be used to compare
 * if a data directory is a clone of another one. This routine is used locally
 * and avoids a connection.
 */
static uint64
get_standby_sysid(const char *datadir)
{
	ControlFileData *cf;
	bool		crc_ok;
	uint64		sysid;

	pg_log_info("getting system identifier from subscriber");

	cf = get_controlfile(datadir, &crc_ok);
	if (!crc_ok)
		pg_fatal("control file appears to be corrupt");

	sysid = cf->system_identifier;

	pg_log_info("system identifier is %llu on subscriber",
				(unsigned long long) sysid);

	pg_free(cf);

	return sysid;
}

/*
 * Modify the system identifier. Since a standby server preserves the system
 * identifier, it makes sense to change it to avoid situations in which WAL
 * files from one of the systems might be used in the other one.
 */
static void
modify_subscriber_sysid(const struct CreateSubscriberOptions *opt)
{
	ControlFileData *cf;
	bool		crc_ok;
	struct timeval tv;

	char	   *cmd_str;

	pg_log_info("modifying system identifier of subscriber");

	cf = get_controlfile(subscriber_dir, &crc_ok);
	if (!crc_ok)
		pg_fatal("control file appears to be corrupt");

	/*
	 * Select a new system identifier.
	 *
	 * XXX this code was extracted from BootStrapXLOG().
	 */
	gettimeofday(&tv, NULL);
	cf->system_identifier = ((uint64) tv.tv_sec) << 32;
	cf->system_identifier |= ((uint64) tv.tv_usec) << 12;
	cf->system_identifier |= getpid() & 0xFFF;

	if (!dry_run)
		update_controlfile(subscriber_dir, cf, true);

	pg_log_info("system identifier is %llu on subscriber",
				(unsigned long long) cf->system_identifier);

	pg_log_info("running pg_resetwal on the subscriber");

	cmd_str = psprintf("\"%s\" -D \"%s\" > \"%s\"", pg_resetwal_path,
					   subscriber_dir, DEVNULL);

	pg_log_debug("pg_resetwal command is: %s", cmd_str);

	if (!dry_run)
	{
		int			rc = system(cmd_str);

		if (rc == 0)
			pg_log_info("subscriber successfully changed the system identifier");
		else
			pg_fatal("subscriber failed to change system identifier: exit code: %d", rc);
	}

	pg_free(cf);
}

/*
 * Generate an object name using a prefix, database oid and a random integer.
 * It is used in case the user does not specify an object name (publication,
 * subscription, replication slot).
 */
static char *
generate_object_name(PGconn *conn)
{
	PGresult   *res;
	Oid			oid;
	uint32		rand;
	char	   *objname;

	res = PQexec(conn,
				 "SELECT oid FROM pg_catalog.pg_database "
				 "WHERE datname = pg_catalog.current_database()");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("could not obtain database OID: %s",
					 PQresultErrorMessage(res));
		disconnect_database(conn, true);
	}

	if (PQntuples(res) != 1)
	{
		pg_log_error("could not obtain database OID: got %d rows, expected %d row",
					 PQntuples(res), 1);
		disconnect_database(conn, true);
	}

	/* Database OID */
	oid = strtoul(PQgetvalue(res, 0, 0), NULL, 10);

	PQclear(res);

	/* Random unsigned integer */
	rand = pg_prng_uint32(&prng_state);

	/*
	 * Build the object name. The name must not exceed NAMEDATALEN - 1. This
	 * current schema uses a maximum of 40 characters (20 + 10 + 1 + 8 +
	 * '\0').
	 */
	objname = psprintf("pg_createsubscriber_%u_%x", oid, rand);

	return objname;
}

/*
 * Create the publications and replication slots in preparation for logical
 * replication. Returns the LSN from latest replication slot. It will be the
 * replication start point that is used to adjust the subscriptions (see
 * set_replication_progress).
 */
static char *
setup_publisher(struct LogicalRepInfo *dbinfo)
{
	char	   *lsn = NULL;

	pg_prng_seed(&prng_state, (uint64) (getpid() ^ time(NULL)));

	for (int i = 0; i < num_dbs; i++)
	{
		PGconn	   *conn;
		char	   *genname = NULL;

		conn = connect_database(dbinfo[i].pubconninfo, true);

		/*
		 * If an object name was not specified as command-line options, assign
		 * a generated object name. The replication slot has a different rule.
		 * The subscription name is assigned to the replication slot name if
		 * no replication slot is specified. It follows the same rule as
		 * CREATE SUBSCRIPTION.
		 */
		if (num_pubs == 0 || num_subs == 0 || num_replslots == 0)
			genname = generate_object_name(conn);
		if (num_pubs == 0)
			dbinfo[i].pubname = pg_strdup(genname);
		if (num_subs == 0)
			dbinfo[i].subname = pg_strdup(genname);
		if (num_replslots == 0)
			dbinfo[i].replslotname = pg_strdup(dbinfo[i].subname);

		/*
		 * Create publication on publisher. This step should be executed
		 * *before* promoting the subscriber to avoid any transactions between
		 * consistent LSN and the new publication rows (such transactions
		 * wouldn't see the new publication rows resulting in an error).
		 */
		create_publication(conn, &dbinfo[i]);

		/* Create replication slot on publisher */
		if (lsn)
			pg_free(lsn);
		lsn = create_logical_replication_slot(conn, &dbinfo[i]);
		if (lsn != NULL || dry_run)
			pg_log_info("create replication slot \"%s\" on publisher",
						dbinfo[i].replslotname);
		else
			exit(1);

		/*
		 * Since we are using the LSN returned by the last replication slot as
		 * recovery_target_lsn, this LSN is ahead of the current WAL position
		 * and the recovery waits until the publisher writes a WAL record to
		 * reach the target and ends the recovery. On idle systems, this wait
		 * time is unpredictable and could lead to failure in promoting the
		 * subscriber. To avoid that, insert a harmless WAL record.
		 */
		if (i == num_dbs - 1 && !dry_run)
		{
			PGresult   *res;

			res = PQexec(conn, "SELECT pg_log_standby_snapshot()");
			if (PQresultStatus(res) != PGRES_TUPLES_OK)
			{
				pg_log_error("could not write an additional WAL record: %s",
							 PQresultErrorMessage(res));
				disconnect_database(conn, true);
			}
			PQclear(res);
		}

		disconnect_database(conn, false);
	}

	return lsn;
}

/*
 * Is recovery still in progress?
 */
static bool
server_is_in_recovery(PGconn *conn)
{
	PGresult   *res;
	int			ret;

	res = PQexec(conn, "SELECT pg_catalog.pg_is_in_recovery()");

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("could not obtain recovery progress: %s",
					 PQresultErrorMessage(res));
		disconnect_database(conn, true);
	}


	ret = strcmp("t", PQgetvalue(res, 0, 0));

	PQclear(res);

	return ret == 0;
}

/*
 * Is the primary server ready for logical replication?
 *
 * XXX Does it not allow a synchronous replica?
 */
static void
check_publisher(const struct LogicalRepInfo *dbinfo)
{
	PGconn	   *conn;
	PGresult   *res;
	bool		failed = false;

	char	   *wal_level;
	int			max_repslots;
	int			cur_repslots;
	int			max_walsenders;
	int			cur_walsenders;
	int			max_prepared_transactions;

	pg_log_info("checking settings on publisher");

	conn = connect_database(dbinfo[0].pubconninfo, true);

	/*
	 * If the primary server is in recovery (i.e. cascading replication),
	 * objects (publication) cannot be created because it is read only.
	 */
	if (server_is_in_recovery(conn))
	{
		pg_log_error("primary server cannot be in recovery");
		disconnect_database(conn, true);
	}

	/*------------------------------------------------------------------------
	 * Logical replication requires a few parameters to be set on publisher.
	 * Since these parameters are not a requirement for physical replication,
	 * we should check it to make sure it won't fail.
	 *
	 * - wal_level = logical
	 * - max_replication_slots >= current + number of dbs to be converted
	 * - max_wal_senders >= current + number of dbs to be converted
	 * -----------------------------------------------------------------------
	 */
	res = PQexec(conn,
				 "SELECT pg_catalog.current_setting('wal_level'),"
				 " pg_catalog.current_setting('max_replication_slots'),"
				 " (SELECT count(*) FROM pg_catalog.pg_replication_slots),"
				 " pg_catalog.current_setting('max_wal_senders'),"
				 " (SELECT count(*) FROM pg_catalog.pg_stat_activity WHERE backend_type = 'walsender'),"
				 " pg_catalog.current_setting('max_prepared_transactions')");

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("could not obtain publisher settings: %s",
					 PQresultErrorMessage(res));
		disconnect_database(conn, true);
	}

	wal_level = pg_strdup(PQgetvalue(res, 0, 0));
	max_repslots = atoi(PQgetvalue(res, 0, 1));
	cur_repslots = atoi(PQgetvalue(res, 0, 2));
	max_walsenders = atoi(PQgetvalue(res, 0, 3));
	cur_walsenders = atoi(PQgetvalue(res, 0, 4));
	max_prepared_transactions = atoi(PQgetvalue(res, 0, 5));

	PQclear(res);

	pg_log_debug("publisher: wal_level: %s", wal_level);
	pg_log_debug("publisher: max_replication_slots: %d", max_repslots);
	pg_log_debug("publisher: current replication slots: %d", cur_repslots);
	pg_log_debug("publisher: max_wal_senders: %d", max_walsenders);
	pg_log_debug("publisher: current wal senders: %d", cur_walsenders);
	pg_log_debug("publisher: max_prepared_transactions: %d",
				 max_prepared_transactions);

	disconnect_database(conn, false);

	if (strcmp(wal_level, "logical") != 0)
	{
		pg_log_error("publisher requires wal_level >= \"logical\"");
		failed = true;
	}

	if (max_repslots - cur_repslots < num_dbs)
	{
		pg_log_error("publisher requires %d replication slots, but only %d remain",
					 num_dbs, max_repslots - cur_repslots);
		pg_log_error_hint("Increase the configuration parameter \"%s\" to at least %d.",
						  "max_replication_slots", cur_repslots + num_dbs);
		failed = true;
	}

	if (max_walsenders - cur_walsenders < num_dbs)
	{
		pg_log_error("publisher requires %d wal sender processes, but only %d remain",
					 num_dbs, max_walsenders - cur_walsenders);
		pg_log_error_hint("Increase the configuration parameter \"%s\" to at least %d.",
						  "max_wal_senders", cur_walsenders + num_dbs);
		failed = true;
	}

	if (max_prepared_transactions != 0)
	{
		pg_log_warning("two_phase option will not be enabled for slots");
		pg_log_warning_detail("Subscriptions will be created with the two_phase option disabled.  "
							  "Prepared transactions will be replicated at COMMIT PREPARED.");
	}

	pg_free(wal_level);

	if (failed)
		exit(1);
}

/*
 * Is the standby server ready for logical replication?
 *
 * XXX Does it not allow a time-delayed replica?
 *
 * XXX In a cascaded replication scenario (P -> S -> C), if the target server
 * is S, it cannot detect there is a replica (server C) because server S starts
 * accepting only local connections and server C cannot connect to it. Hence,
 * there is not a reliable way to provide a suitable error saying the server C
 * will be broken at the end of this process (due to pg_resetwal).
 */
static void
check_subscriber(const struct LogicalRepInfo *dbinfo)
{
	PGconn	   *conn;
	PGresult   *res;
	bool		failed = false;

	int			max_lrworkers;
	int			max_repslots;
	int			max_wprocs;

	pg_log_info("checking settings on subscriber");

	conn = connect_database(dbinfo[0].subconninfo, true);

	/* The target server must be a standby */
	if (!server_is_in_recovery(conn))
	{
		pg_log_error("target server must be a standby");
		disconnect_database(conn, true);
	}

	/*------------------------------------------------------------------------
	 * Logical replication requires a few parameters to be set on subscriber.
	 * Since these parameters are not a requirement for physical replication,
	 * we should check it to make sure it won't fail.
	 *
	 * - max_replication_slots >= number of dbs to be converted
	 * - max_logical_replication_workers >= number of dbs to be converted
	 * - max_worker_processes >= 1 + number of dbs to be converted
	 *------------------------------------------------------------------------
	 */
	res = PQexec(conn,
				 "SELECT setting FROM pg_catalog.pg_settings WHERE name IN ("
				 "'max_logical_replication_workers', "
				 "'max_replication_slots', "
				 "'max_worker_processes', "
				 "'primary_slot_name') "
				 "ORDER BY name");

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("could not obtain subscriber settings: %s",
					 PQresultErrorMessage(res));
		disconnect_database(conn, true);
	}

	max_lrworkers = atoi(PQgetvalue(res, 0, 0));
	max_repslots = atoi(PQgetvalue(res, 1, 0));
	max_wprocs = atoi(PQgetvalue(res, 2, 0));
	if (strcmp(PQgetvalue(res, 3, 0), "") != 0)
		primary_slot_name = pg_strdup(PQgetvalue(res, 3, 0));

	pg_log_debug("subscriber: max_logical_replication_workers: %d",
				 max_lrworkers);
	pg_log_debug("subscriber: max_replication_slots: %d", max_repslots);
	pg_log_debug("subscriber: max_worker_processes: %d", max_wprocs);
	if (primary_slot_name)
		pg_log_debug("subscriber: primary_slot_name: %s", primary_slot_name);

	PQclear(res);

	disconnect_database(conn, false);

	if (max_repslots < num_dbs)
	{
		pg_log_error("subscriber requires %d replication slots, but only %d remain",
					 num_dbs, max_repslots);
		pg_log_error_hint("Increase the configuration parameter \"%s\" to at least %d.",
						  "max_replication_slots", num_dbs);
		failed = true;
	}

	if (max_lrworkers < num_dbs)
	{
		pg_log_error("subscriber requires %d logical replication workers, but only %d remain",
					 num_dbs, max_lrworkers);
		pg_log_error_hint("Increase the configuration parameter \"%s\" to at least %d.",
						  "max_logical_replication_workers", num_dbs);
		failed = true;
	}

	if (max_wprocs < num_dbs + 1)
	{
		pg_log_error("subscriber requires %d worker processes, but only %d remain",
					 num_dbs + 1, max_wprocs);
		pg_log_error_hint("Increase the configuration parameter \"%s\" to at least %d.",
						  "max_worker_processes", num_dbs + 1);
		failed = true;
	}

	if (failed)
		exit(1);
}

/*
 * Drop a specified subscription. This is to avoid duplicate subscriptions on
 * the primary (publisher node) and the newly created subscriber. We
 * shouldn't drop the associated slot as that would be used by the publisher
 * node.
 */
static void
drop_existing_subscriptions(PGconn *conn, const char *subname, const char *dbname)
{
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;

	Assert(conn != NULL);

	/*
	 * Construct a query string. These commands are allowed to be executed
	 * within a transaction.
	 */
	appendPQExpBuffer(query, "ALTER SUBSCRIPTION %s DISABLE;",
					  subname);
	appendPQExpBuffer(query, " ALTER SUBSCRIPTION %s SET (slot_name = NONE);",
					  subname);
	appendPQExpBuffer(query, " DROP SUBSCRIPTION %s;", subname);

	pg_log_info("dropping subscription \"%s\" in database \"%s\"",
				subname, dbname);

	if (!dry_run)
	{
		res = PQexec(conn, query->data);

		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			pg_log_error("could not drop a subscription \"%s\" settings: %s",
						 subname, PQresultErrorMessage(res));
			disconnect_database(conn, true);
		}

		PQclear(res);
	}

	destroyPQExpBuffer(query);
}

/*
 * Retrieve and drop the pre-existing subscriptions.
 */
static void
check_and_drop_existing_subscriptions(PGconn *conn,
									  const struct LogicalRepInfo *dbinfo)
{
	PQExpBuffer query = createPQExpBuffer();
	char	   *dbname;
	PGresult   *res;

	Assert(conn != NULL);

	dbname = PQescapeLiteral(conn, dbinfo->dbname, strlen(dbinfo->dbname));

	appendPQExpBuffer(query,
					  "SELECT s.subname FROM pg_catalog.pg_subscription s "
					  "INNER JOIN pg_catalog.pg_database d ON (s.subdbid = d.oid) "
					  "WHERE d.datname = %s",
					  dbname);
	res = PQexec(conn, query->data);

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("could not obtain pre-existing subscriptions: %s",
					 PQresultErrorMessage(res));
		disconnect_database(conn, true);
	}

	for (int i = 0; i < PQntuples(res); i++)
		drop_existing_subscriptions(conn, PQgetvalue(res, i, 0),
									dbinfo->dbname);

	PQclear(res);
	destroyPQExpBuffer(query);
}

/*
 * Create the subscriptions, adjust the initial location for logical
 * replication and enable the subscriptions. That's the last step for logical
 * replication setup.
 */
static void
setup_subscriber(struct LogicalRepInfo *dbinfo, const char *consistent_lsn)
{
	for (int i = 0; i < num_dbs; i++)
	{
		PGconn	   *conn;

		/* Connect to subscriber. */
		conn = connect_database(dbinfo[i].subconninfo, true);

		/*
		 * We don't need the pre-existing subscriptions on the newly formed
		 * subscriber. They can connect to other publisher nodes and either
		 * get some unwarranted data or can lead to ERRORs in connecting to
		 * such nodes.
		 */
		check_and_drop_existing_subscriptions(conn, &dbinfo[i]);

		/*
		 * Since the publication was created before the consistent LSN, it is
		 * available on the subscriber when the physical replica is promoted.
		 * Remove publications from the subscriber because it has no use.
		 */
		drop_publication(conn, &dbinfo[i]);

		create_subscription(conn, &dbinfo[i]);

		/* Set the replication progress to the correct LSN */
		set_replication_progress(conn, &dbinfo[i], consistent_lsn);

		/* Enable subscription */
		enable_subscription(conn, &dbinfo[i]);

		disconnect_database(conn, false);
	}
}

/*
 * Write the required recovery parameters.
 */
static void
setup_recovery(const struct LogicalRepInfo *dbinfo, const char *datadir, const char *lsn)
{
	PGconn	   *conn;
	PQExpBuffer recoveryconfcontents;

	/*
	 * Despite of the recovery parameters will be written to the subscriber,
	 * use a publisher connection. The primary_conninfo is generated using the
	 * connection settings.
	 */
	conn = connect_database(dbinfo[0].pubconninfo, true);

	/*
	 * Write recovery parameters.
	 *
	 * The subscriber is not running yet. In dry run mode, the recovery
	 * parameters *won't* be written. An invalid LSN is used for printing
	 * purposes. Additional recovery parameters are added here. It avoids
	 * unexpected behavior such as end of recovery as soon as a consistent
	 * state is reached (recovery_target) and failure due to multiple recovery
	 * targets (name, time, xid, LSN).
	 */
	recoveryconfcontents = GenerateRecoveryConfig(conn, NULL, NULL);
	appendPQExpBuffer(recoveryconfcontents, "recovery_target = ''\n");
	appendPQExpBuffer(recoveryconfcontents,
					  "recovery_target_timeline = 'latest'\n");
	appendPQExpBuffer(recoveryconfcontents,
					  "recovery_target_inclusive = true\n");
	appendPQExpBuffer(recoveryconfcontents,
					  "recovery_target_action = promote\n");
	appendPQExpBuffer(recoveryconfcontents, "recovery_target_name = ''\n");
	appendPQExpBuffer(recoveryconfcontents, "recovery_target_time = ''\n");
	appendPQExpBuffer(recoveryconfcontents, "recovery_target_xid = ''\n");

	if (dry_run)
	{
		appendPQExpBuffer(recoveryconfcontents, "# dry run mode");
		appendPQExpBuffer(recoveryconfcontents,
						  "recovery_target_lsn = '%X/%X'\n",
						  LSN_FORMAT_ARGS((XLogRecPtr) InvalidXLogRecPtr));
	}
	else
	{
		appendPQExpBuffer(recoveryconfcontents, "recovery_target_lsn = '%s'\n",
						  lsn);
		WriteRecoveryConfig(conn, datadir, recoveryconfcontents);
	}
	disconnect_database(conn, false);

	pg_log_debug("recovery parameters:\n%s", recoveryconfcontents->data);
}

/*
 * Drop physical replication slot on primary if the standby was using it. After
 * the transformation, it has no use.
 *
 * XXX we might not fail here. Instead, we provide a warning so the user
 * eventually drops this replication slot later.
 */
static void
drop_primary_replication_slot(struct LogicalRepInfo *dbinfo, const char *slotname)
{
	PGconn	   *conn;

	/* Replication slot does not exist, do nothing */
	if (!primary_slot_name)
		return;

	conn = connect_database(dbinfo[0].pubconninfo, false);
	if (conn != NULL)
	{
		drop_replication_slot(conn, &dbinfo[0], slotname);
		disconnect_database(conn, false);
	}
	else
	{
		pg_log_warning("could not drop replication slot \"%s\" on primary",
					   slotname);
		pg_log_warning_hint("Drop this replication slot soon to avoid retention of WAL files.");
	}
}

/*
 * Drop failover replication slots on subscriber. After the transformation,
 * they have no use.
 *
 * XXX We do not fail here. Instead, we provide a warning so the user can drop
 * them later.
 */
static void
drop_failover_replication_slots(struct LogicalRepInfo *dbinfo)
{
	PGconn	   *conn;
	PGresult   *res;

	conn = connect_database(dbinfo[0].subconninfo, false);
	if (conn != NULL)
	{
		/* Get failover replication slot names */
		res = PQexec(conn,
					 "SELECT slot_name FROM pg_catalog.pg_replication_slots WHERE failover");

		if (PQresultStatus(res) == PGRES_TUPLES_OK)
		{
			/* Remove failover replication slots from subscriber */
			for (int i = 0; i < PQntuples(res); i++)
				drop_replication_slot(conn, &dbinfo[0], PQgetvalue(res, i, 0));
		}
		else
		{
			pg_log_warning("could not obtain failover replication slot information: %s",
						   PQresultErrorMessage(res));
			pg_log_warning_hint("Drop the failover replication slots on subscriber soon to avoid retention of WAL files.");
		}

		PQclear(res);
		disconnect_database(conn, false);
	}
	else
	{
		pg_log_warning("could not drop failover replication slot");
		pg_log_warning_hint("Drop the failover replication slots on subscriber soon to avoid retention of WAL files.");
	}
}

/*
 * Create a logical replication slot and returns a LSN.
 *
 * CreateReplicationSlot() is not used because it does not provide the one-row
 * result set that contains the LSN.
 */
static char *
create_logical_replication_slot(PGconn *conn, struct LogicalRepInfo *dbinfo)
{
	PQExpBuffer str = createPQExpBuffer();
	PGresult   *res = NULL;
	const char *slot_name = dbinfo->replslotname;
	char	   *slot_name_esc;
	char	   *lsn = NULL;

	Assert(conn != NULL);

	pg_log_info("creating the replication slot \"%s\" in database \"%s\"",
				slot_name, dbinfo->dbname);

	slot_name_esc = PQescapeLiteral(conn, slot_name, strlen(slot_name));

	appendPQExpBuffer(str,
					  "SELECT lsn FROM pg_catalog.pg_create_logical_replication_slot(%s, 'pgoutput', false, false, false)",
					  slot_name_esc);

	pg_free(slot_name_esc);

	pg_log_debug("command is: %s", str->data);

	if (!dry_run)
	{
		res = PQexec(conn, str->data);
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			pg_log_error("could not create replication slot \"%s\" in database \"%s\": %s",
						 slot_name, dbinfo->dbname,
						 PQresultErrorMessage(res));
			PQclear(res);
			destroyPQExpBuffer(str);
			return NULL;
		}

		lsn = pg_strdup(PQgetvalue(res, 0, 0));
		PQclear(res);
	}

	/* For cleanup purposes */
	dbinfo->made_replslot = true;

	destroyPQExpBuffer(str);

	return lsn;
}

static void
drop_replication_slot(PGconn *conn, struct LogicalRepInfo *dbinfo,
					  const char *slot_name)
{
	PQExpBuffer str = createPQExpBuffer();
	char	   *slot_name_esc;
	PGresult   *res;

	Assert(conn != NULL);

	pg_log_info("dropping the replication slot \"%s\" in database \"%s\"",
				slot_name, dbinfo->dbname);

	slot_name_esc = PQescapeLiteral(conn, slot_name, strlen(slot_name));

	appendPQExpBuffer(str, "SELECT pg_catalog.pg_drop_replication_slot(%s)", slot_name_esc);

	pg_free(slot_name_esc);

	pg_log_debug("command is: %s", str->data);

	if (!dry_run)
	{
		res = PQexec(conn, str->data);
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			pg_log_error("could not drop replication slot \"%s\" in database \"%s\": %s",
						 slot_name, dbinfo->dbname, PQresultErrorMessage(res));
			dbinfo->made_replslot = false;	/* don't try again. */
		}

		PQclear(res);
	}

	destroyPQExpBuffer(str);
}

/*
 * Reports a suitable message if pg_ctl fails.
 */
static void
pg_ctl_status(const char *pg_ctl_cmd, int rc)
{
	if (rc != 0)
	{
		if (WIFEXITED(rc))
		{
			pg_log_error("pg_ctl failed with exit code %d", WEXITSTATUS(rc));
		}
		else if (WIFSIGNALED(rc))
		{
#if defined(WIN32)
			pg_log_error("pg_ctl was terminated by exception 0x%X",
						 WTERMSIG(rc));
			pg_log_error_detail("See C include file \"ntstatus.h\" for a description of the hexadecimal value.");
#else
			pg_log_error("pg_ctl was terminated by signal %d: %s",
						 WTERMSIG(rc), pg_strsignal(WTERMSIG(rc)));
#endif
		}
		else
		{
			pg_log_error("pg_ctl exited with unrecognized status %d", rc);
		}

		pg_log_error_detail("The failed command was: %s", pg_ctl_cmd);
		exit(1);
	}
}

static void
start_standby_server(const struct CreateSubscriberOptions *opt, bool restricted_access,
					 bool restrict_logical_worker)
{
	PQExpBuffer pg_ctl_cmd = createPQExpBuffer();
	int			rc;

	appendPQExpBuffer(pg_ctl_cmd, "\"%s\" start -D ", pg_ctl_path);
	appendShellString(pg_ctl_cmd, subscriber_dir);
	appendPQExpBuffer(pg_ctl_cmd, " -s -o \"-c sync_replication_slots=off\"");
	if (restricted_access)
	{
		appendPQExpBuffer(pg_ctl_cmd, " -o \"-p %s\"", opt->sub_port);
#if !defined(WIN32)

		/*
		 * An empty listen_addresses list means the server does not listen on
		 * any IP interfaces; only Unix-domain sockets can be used to connect
		 * to the server. Prevent external connections to minimize the chance
		 * of failure.
		 */
		appendPQExpBufferStr(pg_ctl_cmd, " -o \"-c listen_addresses='' -c unix_socket_permissions=0700");
		if (opt->socket_dir)
			appendPQExpBuffer(pg_ctl_cmd, " -c unix_socket_directories='%s'",
							  opt->socket_dir);
		appendPQExpBufferChar(pg_ctl_cmd, '"');
#endif
	}
	if (opt->config_file != NULL)
		appendPQExpBuffer(pg_ctl_cmd, " -o \"-c config_file=%s\"",
						  opt->config_file);

	/* Suppress to start logical replication if requested */
	if (restrict_logical_worker)
		appendPQExpBuffer(pg_ctl_cmd, " -o \"-c max_logical_replication_workers=0\"");

	pg_log_debug("pg_ctl command is: %s", pg_ctl_cmd->data);
	rc = system(pg_ctl_cmd->data);
	pg_ctl_status(pg_ctl_cmd->data, rc);
	standby_running = true;
	destroyPQExpBuffer(pg_ctl_cmd);
	pg_log_info("server was started");
}

static void
stop_standby_server(const char *datadir)
{
	char	   *pg_ctl_cmd;
	int			rc;

	pg_ctl_cmd = psprintf("\"%s\" stop -D \"%s\" -s", pg_ctl_path,
						  datadir);
	pg_log_debug("pg_ctl command is: %s", pg_ctl_cmd);
	rc = system(pg_ctl_cmd);
	pg_ctl_status(pg_ctl_cmd, rc);
	standby_running = false;
	pg_log_info("server was stopped");
}

/*
 * Returns after the server finishes the recovery process.
 *
 * If recovery_timeout option is set, terminate abnormally without finishing
 * the recovery process. By default, it waits forever.
 *
 * XXX Is the recovery process still in progress? When recovery process has a
 * better progress reporting mechanism, it should be added here.
 */
static void
wait_for_end_recovery(const char *conninfo, const struct CreateSubscriberOptions *opt)
{
	PGconn	   *conn;
	int			status = POSTMASTER_STILL_STARTING;
	int			timer = 0;

	pg_log_info("waiting for the target server to reach the consistent state");

	conn = connect_database(conninfo, true);

	for (;;)
	{
		bool		in_recovery = server_is_in_recovery(conn);

		/*
		 * Does the recovery process finish? In dry run mode, there is no
		 * recovery mode. Bail out as the recovery process has ended.
		 */
		if (!in_recovery || dry_run)
		{
			status = POSTMASTER_READY;
			recovery_ended = true;
			break;
		}

		/* Bail out after recovery_timeout seconds if this option is set */
		if (opt->recovery_timeout > 0 && timer >= opt->recovery_timeout)
		{
			stop_standby_server(subscriber_dir);
			pg_log_error("recovery timed out");
			disconnect_database(conn, true);
		}

		/* Keep waiting */
		pg_usleep(WAIT_INTERVAL * USEC_PER_SEC);

		timer += WAIT_INTERVAL;
	}

	disconnect_database(conn, false);

	if (status == POSTMASTER_STILL_STARTING)
		pg_fatal("server did not end recovery");

	pg_log_info("target server reached the consistent state");
	pg_log_info_hint("If pg_createsubscriber fails after this point, you must recreate the physical replica before continuing.");
}

/*
 * Create a publication that includes all tables in the database.
 */
static void
create_publication(PGconn *conn, struct LogicalRepInfo *dbinfo)
{
	PQExpBuffer str = createPQExpBuffer();
	PGresult   *res;
	char	   *ipubname_esc;
	char	   *spubname_esc;

	Assert(conn != NULL);

	ipubname_esc = PQescapeIdentifier(conn, dbinfo->pubname, strlen(dbinfo->pubname));
	spubname_esc = PQescapeLiteral(conn, dbinfo->pubname, strlen(dbinfo->pubname));

	/* Check if the publication already exists */
	appendPQExpBuffer(str,
					  "SELECT 1 FROM pg_catalog.pg_publication "
					  "WHERE pubname = %s",
					  spubname_esc);
	res = PQexec(conn, str->data);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("could not obtain publication information: %s",
					 PQresultErrorMessage(res));
		disconnect_database(conn, true);
	}

	if (PQntuples(res) == 1)
	{
		/*
		 * Unfortunately, if it reaches this code path, it will always fail
		 * (unless you decide to change the existing publication name). That's
		 * bad but it is very unlikely that the user will choose a name with
		 * pg_createsubscriber_ prefix followed by the exact database oid and
		 * a random number.
		 */
		pg_log_error("publication \"%s\" already exists", dbinfo->pubname);
		pg_log_error_hint("Consider renaming this publication before continuing.");
		disconnect_database(conn, true);
	}

	PQclear(res);
	resetPQExpBuffer(str);

	pg_log_info("creating publication \"%s\" in database \"%s\"",
				dbinfo->pubname, dbinfo->dbname);

	appendPQExpBuffer(str, "CREATE PUBLICATION %s FOR ALL TABLES",
					  ipubname_esc);

	pg_log_debug("command is: %s", str->data);

	if (!dry_run)
	{
		res = PQexec(conn, str->data);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			pg_log_error("could not create publication \"%s\" in database \"%s\": %s",
						 dbinfo->pubname, dbinfo->dbname, PQresultErrorMessage(res));
			disconnect_database(conn, true);
		}
		PQclear(res);
	}

	/* For cleanup purposes */
	dbinfo->made_publication = true;

	pg_free(ipubname_esc);
	pg_free(spubname_esc);
	destroyPQExpBuffer(str);
}

/*
 * Remove publication if it couldn't finish all steps.
 */
static void
drop_publication(PGconn *conn, struct LogicalRepInfo *dbinfo)
{
	PQExpBuffer str = createPQExpBuffer();
	PGresult   *res;
	char	   *pubname_esc;

	Assert(conn != NULL);

	pubname_esc = PQescapeIdentifier(conn, dbinfo->pubname, strlen(dbinfo->pubname));

	pg_log_info("dropping publication \"%s\" in database \"%s\"",
				dbinfo->pubname, dbinfo->dbname);

	appendPQExpBuffer(str, "DROP PUBLICATION %s", pubname_esc);

	pg_free(pubname_esc);

	pg_log_debug("command is: %s", str->data);

	if (!dry_run)
	{
		res = PQexec(conn, str->data);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			pg_log_error("could not drop publication \"%s\" in database \"%s\": %s",
						 dbinfo->pubname, dbinfo->dbname, PQresultErrorMessage(res));
			dbinfo->made_publication = false;	/* don't try again. */

			/*
			 * Don't disconnect and exit here. This routine is used by primary
			 * (cleanup publication / replication slot due to an error) and
			 * subscriber (remove the replicated publications). In both cases,
			 * it can continue and provide instructions for the user to remove
			 * it later if cleanup fails.
			 */
		}
		PQclear(res);
	}

	destroyPQExpBuffer(str);
}

/*
 * Create a subscription with some predefined options.
 *
 * A replication slot was already created in a previous step. Let's use it.  It
 * is not required to copy data. The subscription will be created but it will
 * not be enabled now. That's because the replication progress must be set and
 * the replication origin name (one of the function arguments) contains the
 * subscription OID in its name. Once the subscription is created,
 * set_replication_progress() can obtain the chosen origin name and set up its
 * initial location.
 */
static void
create_subscription(PGconn *conn, const struct LogicalRepInfo *dbinfo)
{
	PQExpBuffer str = createPQExpBuffer();
	PGresult   *res;
	char	   *pubname_esc;
	char	   *subname_esc;
	char	   *pubconninfo_esc;
	char	   *replslotname_esc;

	Assert(conn != NULL);

	pubname_esc = PQescapeIdentifier(conn, dbinfo->pubname, strlen(dbinfo->pubname));
	subname_esc = PQescapeIdentifier(conn, dbinfo->subname, strlen(dbinfo->subname));
	pubconninfo_esc = PQescapeLiteral(conn, dbinfo->pubconninfo, strlen(dbinfo->pubconninfo));
	replslotname_esc = PQescapeLiteral(conn, dbinfo->replslotname, strlen(dbinfo->replslotname));

	pg_log_info("creating subscription \"%s\" in database \"%s\"",
				dbinfo->subname, dbinfo->dbname);

	appendPQExpBuffer(str,
					  "CREATE SUBSCRIPTION %s CONNECTION %s PUBLICATION %s "
					  "WITH (create_slot = false, enabled = false, "
					  "slot_name = %s, copy_data = false)",
					  subname_esc, pubconninfo_esc, pubname_esc, replslotname_esc);

	pg_free(pubname_esc);
	pg_free(subname_esc);
	pg_free(pubconninfo_esc);
	pg_free(replslotname_esc);

	pg_log_debug("command is: %s", str->data);

	if (!dry_run)
	{
		res = PQexec(conn, str->data);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			pg_log_error("could not create subscription \"%s\" in database \"%s\": %s",
						 dbinfo->subname, dbinfo->dbname, PQresultErrorMessage(res));
			disconnect_database(conn, true);
		}
		PQclear(res);
	}

	destroyPQExpBuffer(str);
}

/*
 * Sets the replication progress to the consistent LSN.
 *
 * The subscriber caught up to the consistent LSN provided by the last
 * replication slot that was created. The goal is to set up the initial
 * location for the logical replication that is the exact LSN that the
 * subscriber was promoted. Once the subscription is enabled it will start
 * streaming from that location onwards.  In dry run mode, the subscription OID
 * and LSN are set to invalid values for printing purposes.
 */
static void
set_replication_progress(PGconn *conn, const struct LogicalRepInfo *dbinfo, const char *lsn)
{
	PQExpBuffer str = createPQExpBuffer();
	PGresult   *res;
	Oid			suboid;
	char	   *subname;
	char	   *dbname;
	char	   *originname;
	char	   *lsnstr;

	Assert(conn != NULL);

	subname = PQescapeLiteral(conn, dbinfo->subname, strlen(dbinfo->subname));
	dbname = PQescapeLiteral(conn, dbinfo->dbname, strlen(dbinfo->dbname));

	appendPQExpBuffer(str,
					  "SELECT s.oid FROM pg_catalog.pg_subscription s "
					  "INNER JOIN pg_catalog.pg_database d ON (s.subdbid = d.oid) "
					  "WHERE s.subname = %s AND d.datname = %s",
					  subname, dbname);

	res = PQexec(conn, str->data);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("could not obtain subscription OID: %s",
					 PQresultErrorMessage(res));
		disconnect_database(conn, true);
	}

	if (PQntuples(res) != 1 && !dry_run)
	{
		pg_log_error("could not obtain subscription OID: got %d rows, expected %d row",
					 PQntuples(res), 1);
		disconnect_database(conn, true);
	}

	if (dry_run)
	{
		suboid = InvalidOid;
		lsnstr = psprintf("%X/%X", LSN_FORMAT_ARGS((XLogRecPtr) InvalidXLogRecPtr));
	}
	else
	{
		suboid = strtoul(PQgetvalue(res, 0, 0), NULL, 10);
		lsnstr = psprintf("%s", lsn);
	}

	PQclear(res);

	/*
	 * The origin name is defined as pg_%u. %u is the subscription OID. See
	 * ApplyWorkerMain().
	 */
	originname = psprintf("pg_%u", suboid);

	pg_log_info("setting the replication progress (node name \"%s\" ; LSN %s) in database \"%s\"",
				originname, lsnstr, dbinfo->dbname);

	resetPQExpBuffer(str);
	appendPQExpBuffer(str,
					  "SELECT pg_catalog.pg_replication_origin_advance('%s', '%s')",
					  originname, lsnstr);

	pg_log_debug("command is: %s", str->data);

	if (!dry_run)
	{
		res = PQexec(conn, str->data);
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			pg_log_error("could not set replication progress for the subscription \"%s\": %s",
						 dbinfo->subname, PQresultErrorMessage(res));
			disconnect_database(conn, true);
		}
		PQclear(res);
	}

	pg_free(subname);
	pg_free(dbname);
	pg_free(originname);
	pg_free(lsnstr);
	destroyPQExpBuffer(str);
}

/*
 * Enables the subscription.
 *
 * The subscription was created in a previous step but it was disabled. After
 * adjusting the initial logical replication location, enable the subscription.
 */
static void
enable_subscription(PGconn *conn, const struct LogicalRepInfo *dbinfo)
{
	PQExpBuffer str = createPQExpBuffer();
	PGresult   *res;
	char	   *subname;

	Assert(conn != NULL);

	subname = PQescapeIdentifier(conn, dbinfo->subname, strlen(dbinfo->subname));

	pg_log_info("enabling subscription \"%s\" in database \"%s\"",
				dbinfo->subname, dbinfo->dbname);

	appendPQExpBuffer(str, "ALTER SUBSCRIPTION %s ENABLE", subname);

	pg_log_debug("command is: %s", str->data);

	if (!dry_run)
	{
		res = PQexec(conn, str->data);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			pg_log_error("could not enable subscription \"%s\": %s",
						 dbinfo->subname, PQresultErrorMessage(res));
			disconnect_database(conn, true);
		}

		PQclear(res);
	}

	pg_free(subname);
	destroyPQExpBuffer(str);
}

int
main(int argc, char **argv)
{
	static struct option long_options[] =
	{
		{"database", required_argument, NULL, 'd'},
		{"pgdata", required_argument, NULL, 'D'},
		{"dry-run", no_argument, NULL, 'n'},
		{"subscriber-port", required_argument, NULL, 'p'},
		{"publisher-server", required_argument, NULL, 'P'},
		{"socket-directory", required_argument, NULL, 's'},
		{"recovery-timeout", required_argument, NULL, 't'},
		{"subscriber-username", required_argument, NULL, 'U'},
		{"verbose", no_argument, NULL, 'v'},
		{"version", no_argument, NULL, 'V'},
		{"help", no_argument, NULL, '?'},
		{"config-file", required_argument, NULL, 1},
		{"publication", required_argument, NULL, 2},
		{"replication-slot", required_argument, NULL, 3},
		{"subscription", required_argument, NULL, 4},
		{NULL, 0, NULL, 0}
	};

	struct CreateSubscriberOptions opt = {0};

	int			c;
	int			option_index;

	char	   *pub_base_conninfo;
	char	   *sub_base_conninfo;
	char	   *dbname_conninfo = NULL;

	uint64		pub_sysid;
	uint64		sub_sysid;
	struct stat statbuf;

	char	   *consistent_lsn;

	char		pidfile[MAXPGPATH];

	pg_logging_init(argv[0]);
	pg_logging_set_level(PG_LOG_WARNING);
	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_createsubscriber"));

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage();
			exit(0);
		}
		else if (strcmp(argv[1], "-V") == 0
				 || strcmp(argv[1], "--version") == 0)
		{
			puts("pg_createsubscriber (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	/* Default settings */
	subscriber_dir = NULL;
	opt.config_file = NULL;
	opt.pub_conninfo_str = NULL;
	opt.socket_dir = NULL;
	opt.sub_port = DEFAULT_SUB_PORT;
	opt.sub_username = NULL;
	opt.database_names = (SimpleStringList)
	{
		0
	};
	opt.recovery_timeout = 0;

	/*
	 * Don't allow it to be run as root. It uses pg_ctl which does not allow
	 * it either.
	 */
#ifndef WIN32
	if (geteuid() == 0)
	{
		pg_log_error("cannot be executed by \"root\"");
		pg_log_error_hint("You must run %s as the PostgreSQL superuser.",
						  progname);
		exit(1);
	}
#endif

	get_restricted_token();

	while ((c = getopt_long(argc, argv, "d:D:np:P:s:t:U:v",
							long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'd':
				if (!simple_string_list_member(&opt.database_names, optarg))
				{
					simple_string_list_append(&opt.database_names, optarg);
					num_dbs++;
				}
				else
				{
					pg_log_error("duplicate database \"%s\"", optarg);
					exit(1);
				}
				break;
			case 'D':
				subscriber_dir = pg_strdup(optarg);
				canonicalize_path(subscriber_dir);
				break;
			case 'n':
				dry_run = true;
				break;
			case 'p':
				opt.sub_port = pg_strdup(optarg);
				break;
			case 'P':
				opt.pub_conninfo_str = pg_strdup(optarg);
				break;
			case 's':
				opt.socket_dir = pg_strdup(optarg);
				canonicalize_path(opt.socket_dir);
				break;
			case 't':
				opt.recovery_timeout = atoi(optarg);
				break;
			case 'U':
				opt.sub_username = pg_strdup(optarg);
				break;
			case 'v':
				pg_logging_increase_verbosity();
				break;
			case 1:
				opt.config_file = pg_strdup(optarg);
				break;
			case 2:
				if (!simple_string_list_member(&opt.pub_names, optarg))
				{
					simple_string_list_append(&opt.pub_names, optarg);
					num_pubs++;
				}
				else
				{
					pg_log_error("duplicate publication \"%s\"", optarg);
					exit(1);
				}
				break;
			case 3:
				if (!simple_string_list_member(&opt.replslot_names, optarg))
				{
					simple_string_list_append(&opt.replslot_names, optarg);
					num_replslots++;
				}
				else
				{
					pg_log_error("duplicate replication slot \"%s\"", optarg);
					exit(1);
				}
				break;
			case 4:
				if (!simple_string_list_member(&opt.sub_names, optarg))
				{
					simple_string_list_append(&opt.sub_names, optarg);
					num_subs++;
				}
				else
				{
					pg_log_error("duplicate subscription \"%s\"", optarg);
					exit(1);
				}
				break;
			default:
				/* getopt_long already emitted a complaint */
				pg_log_error_hint("Try \"%s --help\" for more information.", progname);
				exit(1);
		}
	}

	/* Any non-option arguments? */
	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	/* Required arguments */
	if (subscriber_dir == NULL)
	{
		pg_log_error("no subscriber data directory specified");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	/* If socket directory is not provided, use the current directory */
	if (opt.socket_dir == NULL)
	{
		char		cwd[MAXPGPATH];

		if (!getcwd(cwd, MAXPGPATH))
			pg_fatal("could not determine current directory");
		opt.socket_dir = pg_strdup(cwd);
		canonicalize_path(opt.socket_dir);
	}

	/*
	 * Parse connection string. Build a base connection string that might be
	 * reused by multiple databases.
	 */
	if (opt.pub_conninfo_str == NULL)
	{
		/*
		 * TODO use primary_conninfo (if available) from subscriber and
		 * extract publisher connection string. Assume that there are
		 * identical entries for physical and logical replication. If there is
		 * not, we would fail anyway.
		 */
		pg_log_error("no publisher connection string specified");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}
	pg_log_info("validating connection string on publisher");
	pub_base_conninfo = get_base_conninfo(opt.pub_conninfo_str,
										  &dbname_conninfo);
	if (pub_base_conninfo == NULL)
		exit(1);

	pg_log_info("validating connection string on subscriber");
	sub_base_conninfo = get_sub_conninfo(&opt);

	if (opt.database_names.head == NULL)
	{
		pg_log_info("no database was specified");

		/*
		 * If --database option is not provided, try to obtain the dbname from
		 * the publisher conninfo. If dbname parameter is not available, error
		 * out.
		 */
		if (dbname_conninfo)
		{
			simple_string_list_append(&opt.database_names, dbname_conninfo);
			num_dbs++;

			pg_log_info("database \"%s\" was extracted from the publisher connection string",
						dbname_conninfo);
		}
		else
		{
			pg_log_error("no database name specified");
			pg_log_error_hint("Try \"%s --help\" for more information.",
							  progname);
			exit(1);
		}
	}

	/* Number of object names must match number of databases */
	if (num_pubs > 0 && num_pubs != num_dbs)
	{
		pg_log_error("wrong number of publication names");
		pg_log_error_hint("Number of publication names (%d) must match number of database names (%d).",
						  num_pubs, num_dbs);
		exit(1);
	}
	if (num_subs > 0 && num_subs != num_dbs)
	{
		pg_log_error("wrong number of subscription names");
		pg_log_error_hint("Number of subscription names (%d) must match number of database names (%d).",
						  num_subs, num_dbs);
		exit(1);
	}
	if (num_replslots > 0 && num_replslots != num_dbs)
	{
		pg_log_error("wrong number of replication slot names");
		pg_log_error_hint("Number of replication slot names (%d) must match number of database names (%d).",
						  num_replslots, num_dbs);
		exit(1);
	}

	/* Get the absolute path of pg_ctl and pg_resetwal on the subscriber */
	pg_ctl_path = get_exec_path(argv[0], "pg_ctl");
	pg_resetwal_path = get_exec_path(argv[0], "pg_resetwal");

	/* Rudimentary check for a data directory */
	check_data_directory(subscriber_dir);

	/*
	 * Store database information for publisher and subscriber. It should be
	 * called before atexit() because its return is used in the
	 * cleanup_objects_atexit().
	 */
	dbinfo = store_pub_sub_info(&opt, pub_base_conninfo, sub_base_conninfo);

	/* Register a function to clean up objects in case of failure */
	atexit(cleanup_objects_atexit);

	/*
	 * Check if the subscriber data directory has the same system identifier
	 * than the publisher data directory.
	 */
	pub_sysid = get_primary_sysid(dbinfo[0].pubconninfo);
	sub_sysid = get_standby_sysid(subscriber_dir);
	if (pub_sysid != sub_sysid)
		pg_fatal("subscriber data directory is not a copy of the source database cluster");

	/* Subscriber PID file */
	snprintf(pidfile, MAXPGPATH, "%s/postmaster.pid", subscriber_dir);

	/*
	 * The standby server must not be running. If the server is started under
	 * service manager and pg_createsubscriber stops it, the service manager
	 * might react to this action and start the server again. Therefore,
	 * refuse to proceed if the server is running to avoid possible failures.
	 */
	if (stat(pidfile, &statbuf) == 0)
	{
		pg_log_error("standby is up and running");
		pg_log_error_hint("Stop the standby and try again.");
		exit(1);
	}

	/*
	 * Start a short-lived standby server with temporary parameters (provided
	 * by command-line options). The goal is to avoid connections during the
	 * transformation steps.
	 */
	pg_log_info("starting the standby with command-line options");
	start_standby_server(&opt, true, false);

	/* Check if the standby server is ready for logical replication */
	check_subscriber(dbinfo);

	/* Check if the primary server is ready for logical replication */
	check_publisher(dbinfo);

	/*
	 * Stop the target server. The recovery process requires that the server
	 * reaches a consistent state before targeting the recovery stop point.
	 * Make sure a consistent state is reached (stop the target server
	 * guarantees it) *before* creating the replication slots in
	 * setup_publisher().
	 */
	pg_log_info("stopping the subscriber");
	stop_standby_server(subscriber_dir);

	/* Create the required objects for each database on publisher */
	consistent_lsn = setup_publisher(dbinfo);

	/* Write the required recovery parameters */
	setup_recovery(dbinfo, subscriber_dir, consistent_lsn);

	/*
	 * Start subscriber so the recovery parameters will take effect. Wait
	 * until accepting connections. We don't want to start logical replication
	 * during setup.
	 */
	pg_log_info("starting the subscriber");
	start_standby_server(&opt, true, true);

	/* Waiting the subscriber to be promoted */
	wait_for_end_recovery(dbinfo[0].subconninfo, &opt);

	/*
	 * Create the subscription for each database on subscriber. It does not
	 * enable it immediately because it needs to adjust the replication start
	 * point to the LSN reported by setup_publisher().  It also cleans up
	 * publications created by this tool and replication to the standby.
	 */
	setup_subscriber(dbinfo, consistent_lsn);

	/* Remove primary_slot_name if it exists on primary */
	drop_primary_replication_slot(dbinfo, primary_slot_name);

	/* Remove failover replication slots if they exist on subscriber */
	drop_failover_replication_slots(dbinfo);

	/* Stop the subscriber */
	pg_log_info("stopping the subscriber");
	stop_standby_server(subscriber_dir);

	/* Change system identifier from subscriber */
	modify_subscriber_sysid(&opt);

	success = true;

	pg_log_info("Done!");

	return 0;
}
