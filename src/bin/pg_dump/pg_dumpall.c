/*-------------------------------------------------------------------------
 *
 * pg_dumpall.c
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * pg_dumpall forces all pg_dump output to be text, since it also outputs
 * text into the same output stream.
 *
 * src/bin/pg_dump/pg_dumpall.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <time.h>
#include <unistd.h>

#include "catalog/pg_authid_d.h"
#include "common/connect.h"
#include "common/file_utils.h"
#include "common/hashfn_unstable.h"
#include "common/logging.h"
#include "common/string.h"
#include "dumputils.h"
#include "fe_utils/string_utils.h"
#include "filter.h"
#include "getopt_long.h"
#include "pg_backup.h"

/* version string we expect back from pg_dump */
#define PGDUMP_VERSIONSTR "pg_dump (PostgreSQL) " PG_VERSION "\n"

typedef struct
{
	uint32		status;
	uint32		hashval;
	char	   *rolename;
} RoleNameEntry;

#define SH_PREFIX	rolename
#define SH_ELEMENT_TYPE	RoleNameEntry
#define SH_KEY_TYPE	char *
#define SH_KEY		rolename
#define SH_HASH_KEY(tb, key)	hash_string(key)
#define SH_EQUAL(tb, a, b)		(strcmp(a, b) == 0)
#define SH_STORE_HASH
#define SH_GET_HASH(tb, a)		(a)->hashval
#define SH_SCOPE	static inline
#define SH_RAW_ALLOCATOR	pg_malloc0
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

static void help(void);

static void dropRoles(PGconn *conn);
static void dumpRoles(PGconn *conn);
static void dumpRoleMembership(PGconn *conn);
static void dumpRoleGUCPrivs(PGconn *conn);
static void dropTablespaces(PGconn *conn);
static void dumpTablespaces(PGconn *conn);
static void dropDBs(PGconn *conn);
static void dumpUserConfig(PGconn *conn, const char *username);
static void dumpDatabases(PGconn *conn);
static void dumpTimestamp(const char *msg);
static int	runPgDump(const char *dbname, const char *create_opts);
static void buildShSecLabels(PGconn *conn,
							 const char *catalog_name, Oid objectId,
							 const char *objtype, const char *objname,
							 PQExpBuffer buffer);
static PGconn *connectDatabase(const char *dbname,
							   const char *connection_string, const char *pghost,
							   const char *pgport, const char *pguser,
							   trivalue prompt_password, bool fail_on_error);
static char *constructConnStr(const char **keywords, const char **values);
static PGresult *executeQuery(PGconn *conn, const char *query);
static void executeCommand(PGconn *conn, const char *query);
static void expand_dbname_patterns(PGconn *conn, SimpleStringList *patterns,
								   SimpleStringList *names);
static void read_dumpall_filters(const char *filename, SimpleStringList *pattern);

static char pg_dump_bin[MAXPGPATH];
static const char *progname;
static PQExpBuffer pgdumpopts;
static char *connstr = "";
static bool output_clean = false;
static bool skip_acls = false;
static bool verbose = false;
static bool dosync = true;

static int	binary_upgrade = 0;
static int	column_inserts = 0;
static int	disable_dollar_quoting = 0;
static int	disable_triggers = 0;
static int	if_exists = 0;
static int	inserts = 0;
static int	no_table_access_method = 0;
static int	no_tablespaces = 0;
static int	use_setsessauth = 0;
static int	no_comments = 0;
static int	no_publications = 0;
static int	no_security_labels = 0;
static int	no_subscriptions = 0;
static int	no_toast_compression = 0;
static int	no_unlogged_table_data = 0;
static int	no_role_passwords = 0;
static int	server_version;
static int	load_via_partition_root = 0;
static int	on_conflict_do_nothing = 0;

static char role_catalog[10];
#define PG_AUTHID "pg_authid"
#define PG_ROLES  "pg_roles "

static FILE *OPF;
static char *filename = NULL;

static SimpleStringList database_exclude_patterns = {NULL, NULL};
static SimpleStringList database_exclude_names = {NULL, NULL};

#define exit_nicely(code) exit(code)

int
main(int argc, char *argv[])
{
	static struct option long_options[] = {
		{"data-only", no_argument, NULL, 'a'},
		{"clean", no_argument, NULL, 'c'},
		{"encoding", required_argument, NULL, 'E'},
		{"file", required_argument, NULL, 'f'},
		{"globals-only", no_argument, NULL, 'g'},
		{"host", required_argument, NULL, 'h'},
		{"dbname", required_argument, NULL, 'd'},
		{"database", required_argument, NULL, 'l'},
		{"no-owner", no_argument, NULL, 'O'},
		{"port", required_argument, NULL, 'p'},
		{"roles-only", no_argument, NULL, 'r'},
		{"schema-only", no_argument, NULL, 's'},
		{"superuser", required_argument, NULL, 'S'},
		{"tablespaces-only", no_argument, NULL, 't'},
		{"username", required_argument, NULL, 'U'},
		{"verbose", no_argument, NULL, 'v'},
		{"no-password", no_argument, NULL, 'w'},
		{"password", no_argument, NULL, 'W'},
		{"no-privileges", no_argument, NULL, 'x'},
		{"no-acl", no_argument, NULL, 'x'},

		/*
		 * the following options don't have an equivalent short option letter
		 */
		{"attribute-inserts", no_argument, &column_inserts, 1},
		{"binary-upgrade", no_argument, &binary_upgrade, 1},
		{"column-inserts", no_argument, &column_inserts, 1},
		{"disable-dollar-quoting", no_argument, &disable_dollar_quoting, 1},
		{"disable-triggers", no_argument, &disable_triggers, 1},
		{"exclude-database", required_argument, NULL, 6},
		{"extra-float-digits", required_argument, NULL, 5},
		{"if-exists", no_argument, &if_exists, 1},
		{"inserts", no_argument, &inserts, 1},
		{"lock-wait-timeout", required_argument, NULL, 2},
		{"no-table-access-method", no_argument, &no_table_access_method, 1},
		{"no-tablespaces", no_argument, &no_tablespaces, 1},
		{"quote-all-identifiers", no_argument, &quote_all_identifiers, 1},
		{"load-via-partition-root", no_argument, &load_via_partition_root, 1},
		{"role", required_argument, NULL, 3},
		{"use-set-session-authorization", no_argument, &use_setsessauth, 1},
		{"no-comments", no_argument, &no_comments, 1},
		{"no-publications", no_argument, &no_publications, 1},
		{"no-role-passwords", no_argument, &no_role_passwords, 1},
		{"no-security-labels", no_argument, &no_security_labels, 1},
		{"no-subscriptions", no_argument, &no_subscriptions, 1},
		{"no-sync", no_argument, NULL, 4},
		{"no-toast-compression", no_argument, &no_toast_compression, 1},
		{"no-unlogged-table-data", no_argument, &no_unlogged_table_data, 1},
		{"on-conflict-do-nothing", no_argument, &on_conflict_do_nothing, 1},
		{"rows-per-insert", required_argument, NULL, 7},
		{"filter", required_argument, NULL, 8},

		{NULL, 0, NULL, 0}
	};

	char	   *pghost = NULL;
	char	   *pgport = NULL;
	char	   *pguser = NULL;
	char	   *pgdb = NULL;
	char	   *use_role = NULL;
	const char *dumpencoding = NULL;
	trivalue	prompt_password = TRI_DEFAULT;
	bool		data_only = false;
	bool		globals_only = false;
	bool		roles_only = false;
	bool		tablespaces_only = false;
	PGconn	   *conn;
	int			encoding;
	const char *std_strings;
	int			c,
				ret;
	int			optindex;

	pg_logging_init(argv[0]);
	pg_logging_set_level(PG_LOG_WARNING);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_dump"));
	progname = get_progname(argv[0]);

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			help();
			exit_nicely(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_dumpall (PostgreSQL) " PG_VERSION);
			exit_nicely(0);
		}
	}

	if ((ret = find_other_exec(argv[0], "pg_dump", PGDUMP_VERSIONSTR,
							   pg_dump_bin)) < 0)
	{
		char		full_path[MAXPGPATH];

		if (find_my_exec(argv[0], full_path) < 0)
			strlcpy(full_path, progname, sizeof(full_path));

		if (ret == -1)
			pg_fatal("program \"%s\" is needed by %s but was not found in the same directory as \"%s\"",
					 "pg_dump", progname, full_path);
		else
			pg_fatal("program \"%s\" was found by \"%s\" but was not the same version as %s",
					 "pg_dump", full_path, progname);
	}

	pgdumpopts = createPQExpBuffer();

	while ((c = getopt_long(argc, argv, "acd:E:f:gh:l:Op:rsS:tU:vwWx", long_options, &optindex)) != -1)
	{
		switch (c)
		{
			case 'a':
				data_only = true;
				appendPQExpBufferStr(pgdumpopts, " -a");
				break;

			case 'c':
				output_clean = true;
				break;

			case 'd':
				connstr = pg_strdup(optarg);
				break;

			case 'E':
				dumpencoding = pg_strdup(optarg);
				appendPQExpBufferStr(pgdumpopts, " -E ");
				appendShellString(pgdumpopts, optarg);
				break;

			case 'f':
				filename = pg_strdup(optarg);
				appendPQExpBufferStr(pgdumpopts, " -f ");
				appendShellString(pgdumpopts, filename);
				break;

			case 'g':
				globals_only = true;
				break;

			case 'h':
				pghost = pg_strdup(optarg);
				break;

			case 'l':
				pgdb = pg_strdup(optarg);
				break;

			case 'O':
				appendPQExpBufferStr(pgdumpopts, " -O");
				break;

			case 'p':
				pgport = pg_strdup(optarg);
				break;

			case 'r':
				roles_only = true;
				break;

			case 's':
				appendPQExpBufferStr(pgdumpopts, " -s");
				break;

			case 'S':
				appendPQExpBufferStr(pgdumpopts, " -S ");
				appendShellString(pgdumpopts, optarg);
				break;

			case 't':
				tablespaces_only = true;
				break;

			case 'U':
				pguser = pg_strdup(optarg);
				break;

			case 'v':
				verbose = true;
				pg_logging_increase_verbosity();
				appendPQExpBufferStr(pgdumpopts, " -v");
				break;

			case 'w':
				prompt_password = TRI_NO;
				appendPQExpBufferStr(pgdumpopts, " -w");
				break;

			case 'W':
				prompt_password = TRI_YES;
				appendPQExpBufferStr(pgdumpopts, " -W");
				break;

			case 'x':
				skip_acls = true;
				appendPQExpBufferStr(pgdumpopts, " -x");
				break;

			case 0:
				break;

			case 2:
				appendPQExpBufferStr(pgdumpopts, " --lock-wait-timeout ");
				appendShellString(pgdumpopts, optarg);
				break;

			case 3:
				use_role = pg_strdup(optarg);
				appendPQExpBufferStr(pgdumpopts, " --role ");
				appendShellString(pgdumpopts, use_role);
				break;

			case 4:
				dosync = false;
				appendPQExpBufferStr(pgdumpopts, " --no-sync");
				break;

			case 5:
				appendPQExpBufferStr(pgdumpopts, " --extra-float-digits ");
				appendShellString(pgdumpopts, optarg);
				break;

			case 6:
				simple_string_list_append(&database_exclude_patterns, optarg);
				break;

			case 7:
				appendPQExpBufferStr(pgdumpopts, " --rows-per-insert ");
				appendShellString(pgdumpopts, optarg);
				break;

			case 8:
				read_dumpall_filters(optarg, &database_exclude_patterns);
				break;

			default:
				/* getopt_long already emitted a complaint */
				pg_log_error_hint("Try \"%s --help\" for more information.", progname);
				exit_nicely(1);
		}
	}

	/* Complain if any arguments remain */
	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit_nicely(1);
	}

	if (database_exclude_patterns.head != NULL &&
		(globals_only || roles_only || tablespaces_only))
	{
		pg_log_error("option --exclude-database cannot be used together with -g/--globals-only, -r/--roles-only, or -t/--tablespaces-only");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit_nicely(1);
	}

	/* Make sure the user hasn't specified a mix of globals-only options */
	if (globals_only && roles_only)
	{
		pg_log_error("options -g/--globals-only and -r/--roles-only cannot be used together");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit_nicely(1);
	}

	if (globals_only && tablespaces_only)
	{
		pg_log_error("options -g/--globals-only and -t/--tablespaces-only cannot be used together");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit_nicely(1);
	}

	if (if_exists && !output_clean)
		pg_fatal("option --if-exists requires option -c/--clean");

	if (roles_only && tablespaces_only)
	{
		pg_log_error("options -r/--roles-only and -t/--tablespaces-only cannot be used together");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit_nicely(1);
	}

	/*
	 * If password values are not required in the dump, switch to using
	 * pg_roles which is equally useful, just more likely to have unrestricted
	 * access than pg_authid.
	 */
	if (no_role_passwords)
		sprintf(role_catalog, "%s", PG_ROLES);
	else
		sprintf(role_catalog, "%s", PG_AUTHID);

	/* Add long options to the pg_dump argument list */
	if (binary_upgrade)
		appendPQExpBufferStr(pgdumpopts, " --binary-upgrade");
	if (column_inserts)
		appendPQExpBufferStr(pgdumpopts, " --column-inserts");
	if (disable_dollar_quoting)
		appendPQExpBufferStr(pgdumpopts, " --disable-dollar-quoting");
	if (disable_triggers)
		appendPQExpBufferStr(pgdumpopts, " --disable-triggers");
	if (inserts)
		appendPQExpBufferStr(pgdumpopts, " --inserts");
	if (no_table_access_method)
		appendPQExpBufferStr(pgdumpopts, " --no-table-access-method");
	if (no_tablespaces)
		appendPQExpBufferStr(pgdumpopts, " --no-tablespaces");
	if (quote_all_identifiers)
		appendPQExpBufferStr(pgdumpopts, " --quote-all-identifiers");
	if (load_via_partition_root)
		appendPQExpBufferStr(pgdumpopts, " --load-via-partition-root");
	if (use_setsessauth)
		appendPQExpBufferStr(pgdumpopts, " --use-set-session-authorization");
	if (no_comments)
		appendPQExpBufferStr(pgdumpopts, " --no-comments");
	if (no_publications)
		appendPQExpBufferStr(pgdumpopts, " --no-publications");
	if (no_security_labels)
		appendPQExpBufferStr(pgdumpopts, " --no-security-labels");
	if (no_subscriptions)
		appendPQExpBufferStr(pgdumpopts, " --no-subscriptions");
	if (no_toast_compression)
		appendPQExpBufferStr(pgdumpopts, " --no-toast-compression");
	if (no_unlogged_table_data)
		appendPQExpBufferStr(pgdumpopts, " --no-unlogged-table-data");
	if (on_conflict_do_nothing)
		appendPQExpBufferStr(pgdumpopts, " --on-conflict-do-nothing");

	/*
	 * If there was a database specified on the command line, use that,
	 * otherwise try to connect to database "postgres", and failing that
	 * "template1".
	 */
	if (pgdb)
	{
		conn = connectDatabase(pgdb, connstr, pghost, pgport, pguser,
							   prompt_password, false);

		if (!conn)
			pg_fatal("could not connect to database \"%s\"", pgdb);
	}
	else
	{
		conn = connectDatabase("postgres", connstr, pghost, pgport, pguser,
							   prompt_password, false);
		if (!conn)
			conn = connectDatabase("template1", connstr, pghost, pgport, pguser,
								   prompt_password, true);

		if (!conn)
		{
			pg_log_error("could not connect to databases \"postgres\" or \"template1\"\n"
						 "Please specify an alternative database.");
			pg_log_error_hint("Try \"%s --help\" for more information.", progname);
			exit_nicely(1);
		}
	}

	/*
	 * Get a list of database names that match the exclude patterns
	 */
	expand_dbname_patterns(conn, &database_exclude_patterns,
						   &database_exclude_names);

	/*
	 * Open the output file if required, otherwise use stdout
	 */
	if (filename)
	{
		OPF = fopen(filename, PG_BINARY_W);
		if (!OPF)
			pg_fatal("could not open output file \"%s\": %m",
					 filename);
	}
	else
		OPF = stdout;

	/*
	 * Set the client encoding if requested.
	 */
	if (dumpencoding)
	{
		if (PQsetClientEncoding(conn, dumpencoding) < 0)
			pg_fatal("invalid client encoding \"%s\" specified",
					 dumpencoding);
	}

	/*
	 * Get the active encoding and the standard_conforming_strings setting, so
	 * we know how to escape strings.
	 */
	encoding = PQclientEncoding(conn);
	setFmtEncoding(encoding);
	std_strings = PQparameterStatus(conn, "standard_conforming_strings");
	if (!std_strings)
		std_strings = "off";

	/* Set the role if requested */
	if (use_role)
	{
		PQExpBuffer query = createPQExpBuffer();

		appendPQExpBuffer(query, "SET ROLE %s", fmtId(use_role));
		executeCommand(conn, query->data);
		destroyPQExpBuffer(query);
	}

	/* Force quoting of all identifiers if requested. */
	if (quote_all_identifiers)
		executeCommand(conn, "SET quote_all_identifiers = true");

	fprintf(OPF, "--\n-- PostgreSQL database cluster dump\n--\n\n");
	if (verbose)
		dumpTimestamp("Started on");

	/*
	 * We used to emit \connect postgres here, but that served no purpose
	 * other than to break things for installations without a postgres
	 * database.  Everything we're restoring here is a global, so whichever
	 * database we're connected to at the moment is fine.
	 */

	/* Restore will need to write to the target cluster */
	fprintf(OPF, "SET default_transaction_read_only = off;\n\n");

	/* Replicate encoding and std_strings in output */
	fprintf(OPF, "SET client_encoding = '%s';\n",
			pg_encoding_to_char(encoding));
	fprintf(OPF, "SET standard_conforming_strings = %s;\n", std_strings);
	if (strcmp(std_strings, "off") == 0)
		fprintf(OPF, "SET escape_string_warning = off;\n");
	fprintf(OPF, "\n");

	if (!data_only)
	{
		/*
		 * If asked to --clean, do that first.  We can avoid detailed
		 * dependency analysis because databases never depend on each other,
		 * and tablespaces never depend on each other.  Roles could have
		 * grants to each other, but DROP ROLE will clean those up silently.
		 */
		if (output_clean)
		{
			if (!globals_only && !roles_only && !tablespaces_only)
				dropDBs(conn);

			if (!roles_only && !no_tablespaces)
				dropTablespaces(conn);

			if (!tablespaces_only)
				dropRoles(conn);
		}

		/*
		 * Now create objects as requested.  Be careful that option logic here
		 * is the same as for drops above.
		 */
		if (!tablespaces_only)
		{
			/* Dump roles (users) */
			dumpRoles(conn);

			/* Dump role memberships */
			dumpRoleMembership(conn);

			/* Dump role GUC privileges */
			if (server_version >= 150000 && !skip_acls)
				dumpRoleGUCPrivs(conn);
		}

		/* Dump tablespaces */
		if (!roles_only && !no_tablespaces)
			dumpTablespaces(conn);
	}

	if (!globals_only && !roles_only && !tablespaces_only)
		dumpDatabases(conn);

	PQfinish(conn);

	if (verbose)
		dumpTimestamp("Completed on");
	fprintf(OPF, "--\n-- PostgreSQL database cluster dump complete\n--\n\n");

	if (filename)
	{
		fclose(OPF);

		/* sync the resulting file, errors are not fatal */
		if (dosync)
			(void) fsync_fname(filename, false);
	}

	exit_nicely(0);
}


static void
help(void)
{
	printf(_("%s extracts a PostgreSQL database cluster into an SQL script file.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]...\n"), progname);

	printf(_("\nGeneral options:\n"));
	printf(_("  -f, --file=FILENAME          output file name\n"));
	printf(_("  -v, --verbose                verbose mode\n"));
	printf(_("  -V, --version                output version information, then exit\n"));
	printf(_("  --lock-wait-timeout=TIMEOUT  fail after waiting TIMEOUT for a table lock\n"));
	printf(_("  -?, --help                   show this help, then exit\n"));
	printf(_("\nOptions controlling the output content:\n"));
	printf(_("  -a, --data-only              dump only the data, not the schema\n"));
	printf(_("  -c, --clean                  clean (drop) databases before recreating\n"));
	printf(_("  -E, --encoding=ENCODING      dump the data in encoding ENCODING\n"));
	printf(_("  -g, --globals-only           dump only global objects, no databases\n"));
	printf(_("  -O, --no-owner               skip restoration of object ownership\n"));
	printf(_("  -r, --roles-only             dump only roles, no databases or tablespaces\n"));
	printf(_("  -s, --schema-only            dump only the schema, no data\n"));
	printf(_("  -S, --superuser=NAME         superuser user name to use in the dump\n"));
	printf(_("  -t, --tablespaces-only       dump only tablespaces, no databases or roles\n"));
	printf(_("  -x, --no-privileges          do not dump privileges (grant/revoke)\n"));
	printf(_("  --binary-upgrade             for use by upgrade utilities only\n"));
	printf(_("  --column-inserts             dump data as INSERT commands with column names\n"));
	printf(_("  --disable-dollar-quoting     disable dollar quoting, use SQL standard quoting\n"));
	printf(_("  --disable-triggers           disable triggers during data-only restore\n"));
	printf(_("  --exclude-database=PATTERN   exclude databases whose name matches PATTERN\n"));
	printf(_("  --extra-float-digits=NUM     override default setting for extra_float_digits\n"));
	printf(_("  --filter=FILENAME            exclude databases based on expressions in FILENAME\n"));
	printf(_("  --if-exists                  use IF EXISTS when dropping objects\n"));
	printf(_("  --inserts                    dump data as INSERT commands, rather than COPY\n"));
	printf(_("  --load-via-partition-root    load partitions via the root table\n"));
	printf(_("  --no-comments                do not dump comment commands\n"));
	printf(_("  --no-publications            do not dump publications\n"));
	printf(_("  --no-role-passwords          do not dump passwords for roles\n"));
	printf(_("  --no-security-labels         do not dump security label assignments\n"));
	printf(_("  --no-subscriptions           do not dump subscriptions\n"));
	printf(_("  --no-sync                    do not wait for changes to be written safely to disk\n"));
	printf(_("  --no-table-access-method     do not dump table access methods\n"));
	printf(_("  --no-tablespaces             do not dump tablespace assignments\n"));
	printf(_("  --no-toast-compression       do not dump TOAST compression methods\n"));
	printf(_("  --no-unlogged-table-data     do not dump unlogged table data\n"));
	printf(_("  --on-conflict-do-nothing     add ON CONFLICT DO NOTHING to INSERT commands\n"));
	printf(_("  --quote-all-identifiers      quote all identifiers, even if not key words\n"));
	printf(_("  --rows-per-insert=NROWS      number of rows per INSERT; implies --inserts\n"));
	printf(_("  --use-set-session-authorization\n"
			 "                               use SET SESSION AUTHORIZATION commands instead of\n"
			 "                               ALTER OWNER commands to set ownership\n"));

	printf(_("\nConnection options:\n"));
	printf(_("  -d, --dbname=CONNSTR     connect using connection string\n"));
	printf(_("  -h, --host=HOSTNAME      database server host or socket directory\n"));
	printf(_("  -l, --database=DBNAME    alternative default database\n"));
	printf(_("  -p, --port=PORT          database server port number\n"));
	printf(_("  -U, --username=NAME      connect as specified database user\n"));
	printf(_("  -w, --no-password        never prompt for password\n"));
	printf(_("  -W, --password           force password prompt (should happen automatically)\n"));
	printf(_("  --role=ROLENAME          do SET ROLE before dump\n"));

	printf(_("\nIf -f/--file is not used, then the SQL script will be written to the standard\n"
			 "output.\n\n"));
	printf(_("Report bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}


/*
 * Drop roles
 */
static void
dropRoles(PGconn *conn)
{
	PQExpBuffer buf = createPQExpBuffer();
	PGresult   *res;
	int			i_rolname;
	int			i;

	if (server_version >= 90600)
		printfPQExpBuffer(buf,
						  "SELECT rolname "
						  "FROM %s "
						  "WHERE rolname !~ '^pg_' "
						  "ORDER BY 1", role_catalog);
	else
		printfPQExpBuffer(buf,
						  "SELECT rolname "
						  "FROM %s "
						  "ORDER BY 1", role_catalog);

	res = executeQuery(conn, buf->data);

	i_rolname = PQfnumber(res, "rolname");

	if (PQntuples(res) > 0)
		fprintf(OPF, "--\n-- Drop roles\n--\n\n");

	for (i = 0; i < PQntuples(res); i++)
	{
		const char *rolename;

		rolename = PQgetvalue(res, i, i_rolname);

		fprintf(OPF, "DROP ROLE %s%s;\n",
				if_exists ? "IF EXISTS " : "",
				fmtId(rolename));
	}

	PQclear(res);
	destroyPQExpBuffer(buf);

	fprintf(OPF, "\n\n");
}

/*
 * Dump roles
 */
static void
dumpRoles(PGconn *conn)
{
	PQExpBuffer buf = createPQExpBuffer();
	PGresult   *res;
	int			i_oid,
				i_rolname,
				i_rolsuper,
				i_rolinherit,
				i_rolcreaterole,
				i_rolcreatedb,
				i_rolcanlogin,
				i_rolconnlimit,
				i_rolpassword,
				i_rolvaliduntil,
				i_rolreplication,
				i_rolbypassrls,
				i_rolcomment,
				i_is_current_user;
	int			i;

	/*
	 * Notes: rolconfig is dumped later, and pg_authid must be used for
	 * extracting rolcomment regardless of role_catalog.
	 */
	if (server_version >= 90600)
		printfPQExpBuffer(buf,
						  "SELECT oid, rolname, rolsuper, rolinherit, "
						  "rolcreaterole, rolcreatedb, "
						  "rolcanlogin, rolconnlimit, rolpassword, "
						  "rolvaliduntil, rolreplication, rolbypassrls, "
						  "pg_catalog.shobj_description(oid, 'pg_authid') as rolcomment, "
						  "rolname = current_user AS is_current_user "
						  "FROM %s "
						  "WHERE rolname !~ '^pg_' "
						  "ORDER BY 2", role_catalog);
	else if (server_version >= 90500)
		printfPQExpBuffer(buf,
						  "SELECT oid, rolname, rolsuper, rolinherit, "
						  "rolcreaterole, rolcreatedb, "
						  "rolcanlogin, rolconnlimit, rolpassword, "
						  "rolvaliduntil, rolreplication, rolbypassrls, "
						  "pg_catalog.shobj_description(oid, 'pg_authid') as rolcomment, "
						  "rolname = current_user AS is_current_user "
						  "FROM %s "
						  "ORDER BY 2", role_catalog);
	else
		printfPQExpBuffer(buf,
						  "SELECT oid, rolname, rolsuper, rolinherit, "
						  "rolcreaterole, rolcreatedb, "
						  "rolcanlogin, rolconnlimit, rolpassword, "
						  "rolvaliduntil, rolreplication, "
						  "false as rolbypassrls, "
						  "pg_catalog.shobj_description(oid, 'pg_authid') as rolcomment, "
						  "rolname = current_user AS is_current_user "
						  "FROM %s "
						  "ORDER BY 2", role_catalog);

	res = executeQuery(conn, buf->data);

	i_oid = PQfnumber(res, "oid");
	i_rolname = PQfnumber(res, "rolname");
	i_rolsuper = PQfnumber(res, "rolsuper");
	i_rolinherit = PQfnumber(res, "rolinherit");
	i_rolcreaterole = PQfnumber(res, "rolcreaterole");
	i_rolcreatedb = PQfnumber(res, "rolcreatedb");
	i_rolcanlogin = PQfnumber(res, "rolcanlogin");
	i_rolconnlimit = PQfnumber(res, "rolconnlimit");
	i_rolpassword = PQfnumber(res, "rolpassword");
	i_rolvaliduntil = PQfnumber(res, "rolvaliduntil");
	i_rolreplication = PQfnumber(res, "rolreplication");
	i_rolbypassrls = PQfnumber(res, "rolbypassrls");
	i_rolcomment = PQfnumber(res, "rolcomment");
	i_is_current_user = PQfnumber(res, "is_current_user");

	if (PQntuples(res) > 0)
		fprintf(OPF, "--\n-- Roles\n--\n\n");

	for (i = 0; i < PQntuples(res); i++)
	{
		const char *rolename;
		Oid			auth_oid;

		auth_oid = atooid(PQgetvalue(res, i, i_oid));
		rolename = PQgetvalue(res, i, i_rolname);

		if (strncmp(rolename, "pg_", 3) == 0)
		{
			pg_log_warning("role name starting with \"pg_\" skipped (%s)",
						   rolename);
			continue;
		}

		resetPQExpBuffer(buf);

		if (binary_upgrade)
		{
			appendPQExpBufferStr(buf, "\n-- For binary upgrade, must preserve pg_authid.oid\n");
			appendPQExpBuffer(buf,
							  "SELECT pg_catalog.binary_upgrade_set_next_pg_authid_oid('%u'::pg_catalog.oid);\n\n",
							  auth_oid);
		}

		/*
		 * We dump CREATE ROLE followed by ALTER ROLE to ensure that the role
		 * will acquire the right properties even if it already exists (ie, it
		 * won't hurt for the CREATE to fail).  This is particularly important
		 * for the role we are connected as, since even with --clean we will
		 * have failed to drop it.  binary_upgrade cannot generate any errors,
		 * so we assume the current role is already created.
		 */
		if (!binary_upgrade ||
			strcmp(PQgetvalue(res, i, i_is_current_user), "f") == 0)
			appendPQExpBuffer(buf, "CREATE ROLE %s;\n", fmtId(rolename));
		appendPQExpBuffer(buf, "ALTER ROLE %s WITH", fmtId(rolename));

		if (strcmp(PQgetvalue(res, i, i_rolsuper), "t") == 0)
			appendPQExpBufferStr(buf, " SUPERUSER");
		else
			appendPQExpBufferStr(buf, " NOSUPERUSER");

		if (strcmp(PQgetvalue(res, i, i_rolinherit), "t") == 0)
			appendPQExpBufferStr(buf, " INHERIT");
		else
			appendPQExpBufferStr(buf, " NOINHERIT");

		if (strcmp(PQgetvalue(res, i, i_rolcreaterole), "t") == 0)
			appendPQExpBufferStr(buf, " CREATEROLE");
		else
			appendPQExpBufferStr(buf, " NOCREATEROLE");

		if (strcmp(PQgetvalue(res, i, i_rolcreatedb), "t") == 0)
			appendPQExpBufferStr(buf, " CREATEDB");
		else
			appendPQExpBufferStr(buf, " NOCREATEDB");

		if (strcmp(PQgetvalue(res, i, i_rolcanlogin), "t") == 0)
			appendPQExpBufferStr(buf, " LOGIN");
		else
			appendPQExpBufferStr(buf, " NOLOGIN");

		if (strcmp(PQgetvalue(res, i, i_rolreplication), "t") == 0)
			appendPQExpBufferStr(buf, " REPLICATION");
		else
			appendPQExpBufferStr(buf, " NOREPLICATION");

		if (strcmp(PQgetvalue(res, i, i_rolbypassrls), "t") == 0)
			appendPQExpBufferStr(buf, " BYPASSRLS");
		else
			appendPQExpBufferStr(buf, " NOBYPASSRLS");

		if (strcmp(PQgetvalue(res, i, i_rolconnlimit), "-1") != 0)
			appendPQExpBuffer(buf, " CONNECTION LIMIT %s",
							  PQgetvalue(res, i, i_rolconnlimit));


		if (!PQgetisnull(res, i, i_rolpassword) && !no_role_passwords)
		{
			appendPQExpBufferStr(buf, " PASSWORD ");
			appendStringLiteralConn(buf, PQgetvalue(res, i, i_rolpassword), conn);
		}

		if (!PQgetisnull(res, i, i_rolvaliduntil))
			appendPQExpBuffer(buf, " VALID UNTIL '%s'",
							  PQgetvalue(res, i, i_rolvaliduntil));

		appendPQExpBufferStr(buf, ";\n");

		if (!no_comments && !PQgetisnull(res, i, i_rolcomment))
		{
			appendPQExpBuffer(buf, "COMMENT ON ROLE %s IS ", fmtId(rolename));
			appendStringLiteralConn(buf, PQgetvalue(res, i, i_rolcomment), conn);
			appendPQExpBufferStr(buf, ";\n");
		}

		if (!no_security_labels)
			buildShSecLabels(conn, "pg_authid", auth_oid,
							 "ROLE", rolename,
							 buf);

		fprintf(OPF, "%s", buf->data);
	}

	/*
	 * Dump configuration settings for roles after all roles have been dumped.
	 * We do it this way because config settings for roles could mention the
	 * names of other roles.
	 */
	if (PQntuples(res) > 0)
		fprintf(OPF, "\n--\n-- User Configurations\n--\n");

	for (i = 0; i < PQntuples(res); i++)
		dumpUserConfig(conn, PQgetvalue(res, i, i_rolname));

	PQclear(res);

	fprintf(OPF, "\n\n");

	destroyPQExpBuffer(buf);
}


/*
 * Dump role memberships.
 *
 * Note: we expect dumpRoles already created all the roles, but there is
 * no membership yet.
 */
static void
dumpRoleMembership(PGconn *conn)
{
	PQExpBuffer buf = createPQExpBuffer();
	PQExpBuffer optbuf = createPQExpBuffer();
	PGresult   *res;
	int			start = 0,
				end,
				total;
	bool		dump_grantors;
	bool		dump_grant_options;
	int			i_inherit_option;
	int			i_set_option;

	/*
	 * Previous versions of PostgreSQL didn't used to track the grantor very
	 * carefully in the backend, and the grantor could be any user even if
	 * they didn't have ADMIN OPTION on the role, or a user that no longer
	 * existed. To avoid dump and restore failures, don't dump the grantor
	 * when talking to an old server version.
	 */
	dump_grantors = (PQserverVersion(conn) >= 160000);

	/*
	 * Previous versions of PostgreSQL also did not have grant-level options.
	 */
	dump_grant_options = (server_version >= 160000);

	/* Generate and execute query. */
	printfPQExpBuffer(buf, "SELECT ur.rolname AS role, "
					  "um.rolname AS member, "
					  "ug.oid AS grantorid, "
					  "ug.rolname AS grantor, "
					  "a.admin_option");
	if (dump_grant_options)
		appendPQExpBufferStr(buf, ", a.inherit_option, a.set_option");
	appendPQExpBuffer(buf, " FROM pg_auth_members a "
					  "LEFT JOIN %s ur on ur.oid = a.roleid "
					  "LEFT JOIN %s um on um.oid = a.member "
					  "LEFT JOIN %s ug on ug.oid = a.grantor "
					  "WHERE NOT (ur.rolname ~ '^pg_' AND um.rolname ~ '^pg_')"
					  "ORDER BY 1,2,4", role_catalog, role_catalog, role_catalog);
	res = executeQuery(conn, buf->data);
	i_inherit_option = PQfnumber(res, "inherit_option");
	i_set_option = PQfnumber(res, "set_option");

	if (PQntuples(res) > 0)
		fprintf(OPF, "--\n-- Role memberships\n--\n\n");

	/*
	 * We can't dump these GRANT commands in arbitrary order, because a role
	 * that is named as a grantor must already have ADMIN OPTION on the role
	 * for which it is granting permissions, except for the bootstrap
	 * superuser, who can always be named as the grantor.
	 *
	 * We handle this by considering these grants role by role. For each role,
	 * we initially consider the only allowable grantor to be the bootstrap
	 * superuser. Every time we grant ADMIN OPTION on the role to some user,
	 * that user also becomes an allowable grantor. We make repeated passes
	 * over the grants for the role, each time dumping those whose grantors
	 * are allowable and which we haven't done yet. Eventually this should let
	 * us dump all the grants.
	 */
	total = PQntuples(res);
	while (start < total)
	{
		char	   *role = PQgetvalue(res, start, 0);
		int			i;
		bool	   *done;
		int			remaining;
		int			prev_remaining = 0;
		rolename_hash *ht;

		/* All memberships for a single role should be adjacent. */
		for (end = start; end < total; ++end)
		{
			char	   *otherrole;

			otherrole = PQgetvalue(res, end, 0);
			if (strcmp(role, otherrole) != 0)
				break;
		}

		role = PQgetvalue(res, start, 0);
		remaining = end - start;
		done = pg_malloc0(remaining * sizeof(bool));
		ht = rolename_create(remaining, NULL);

		/*
		 * Make repeated passes over the grants for this role until all have
		 * been dumped.
		 */
		while (remaining > 0)
		{
			/*
			 * We should make progress on every iteration, because a notional
			 * graph whose vertices are grants and whose edges point from
			 * grantors to members should be connected and acyclic. If we fail
			 * to make progress, either we or the server have messed up.
			 */
			if (remaining == prev_remaining)
			{
				pg_log_error("could not find a legal dump ordering for memberships in role \"%s\"",
							 role);
				PQfinish(conn);
				exit_nicely(1);
			}
			prev_remaining = remaining;

			/* Make one pass over the grants for this role. */
			for (i = start; i < end; ++i)
			{
				char	   *member;
				char	   *admin_option;
				char	   *grantorid;
				char	   *grantor;
				char	   *set_option = "true";
				bool		found;

				/* If we already did this grant, don't do it again. */
				if (done[i - start])
					continue;

				member = PQgetvalue(res, i, 1);
				grantorid = PQgetvalue(res, i, 2);
				grantor = PQgetvalue(res, i, 3);
				admin_option = PQgetvalue(res, i, 4);
				if (dump_grant_options)
					set_option = PQgetvalue(res, i, i_set_option);

				/*
				 * If we're not dumping grantors or if the grantor is the
				 * bootstrap superuser, it's fine to dump this now. Otherwise,
				 * it's got to be someone who has already been granted ADMIN
				 * OPTION.
				 */
				if (dump_grantors &&
					atooid(grantorid) != BOOTSTRAP_SUPERUSERID &&
					rolename_lookup(ht, grantor) == NULL)
					continue;

				/* Remember that we did this so that we don't do it again. */
				done[i - start] = true;
				--remaining;

				/*
				 * If ADMIN OPTION is being granted, remember that grants
				 * listing this member as the grantor can now be dumped.
				 */
				if (*admin_option == 't')
					rolename_insert(ht, member, &found);

				/* Generate the actual GRANT statement. */
				resetPQExpBuffer(optbuf);
				fprintf(OPF, "GRANT %s", fmtId(role));
				fprintf(OPF, " TO %s", fmtId(member));
				if (*admin_option == 't')
					appendPQExpBufferStr(optbuf, "ADMIN OPTION");
				if (dump_grant_options)
				{
					char	   *inherit_option;

					if (optbuf->data[0] != '\0')
						appendPQExpBufferStr(optbuf, ", ");
					inherit_option = PQgetvalue(res, i, i_inherit_option);
					appendPQExpBuffer(optbuf, "INHERIT %s",
									  *inherit_option == 't' ?
									  "TRUE" : "FALSE");
				}
				if (*set_option != 't')
				{
					if (optbuf->data[0] != '\0')
						appendPQExpBufferStr(optbuf, ", ");
					appendPQExpBuffer(optbuf, "SET FALSE");
				}
				if (optbuf->data[0] != '\0')
					fprintf(OPF, " WITH %s", optbuf->data);
				if (dump_grantors)
					fprintf(OPF, " GRANTED BY %s", fmtId(grantor));
				fprintf(OPF, ";\n");
			}
		}

		rolename_destroy(ht);
		pg_free(done);
		start = end;
	}

	PQclear(res);
	destroyPQExpBuffer(buf);

	fprintf(OPF, "\n\n");
}


/*
 * Dump role configuration parameter privileges.  This code is used for 15.0
 * and later servers.
 *
 * Note: we expect dumpRoles already created all the roles, but there are
 * no per-role configuration parameter privileges yet.
 */
static void
dumpRoleGUCPrivs(PGconn *conn)
{
	PGresult   *res;
	int			i;

	/*
	 * Get all parameters that have non-default acls defined.
	 */
	res = executeQuery(conn, "SELECT parname, "
					   "pg_catalog.pg_get_userbyid(" CppAsString2(BOOTSTRAP_SUPERUSERID) ") AS parowner, "
					   "paracl, "
					   "pg_catalog.acldefault('p', " CppAsString2(BOOTSTRAP_SUPERUSERID) ") AS acldefault "
					   "FROM pg_catalog.pg_parameter_acl "
					   "ORDER BY 1");

	if (PQntuples(res) > 0)
		fprintf(OPF, "--\n-- Role privileges on configuration parameters\n--\n\n");

	for (i = 0; i < PQntuples(res); i++)
	{
		PQExpBuffer buf = createPQExpBuffer();
		char	   *parname = PQgetvalue(res, i, 0);
		char	   *parowner = PQgetvalue(res, i, 1);
		char	   *paracl = PQgetvalue(res, i, 2);
		char	   *acldefault = PQgetvalue(res, i, 3);
		char	   *fparname;

		/* needed for buildACLCommands() */
		fparname = pg_strdup(fmtId(parname));

		if (!buildACLCommands(fparname, NULL, NULL, "PARAMETER",
							  paracl, acldefault,
							  parowner, "", server_version, buf))
		{
			pg_log_error("could not parse ACL list (%s) for parameter \"%s\"",
						 paracl, parname);
			PQfinish(conn);
			exit_nicely(1);
		}

		fprintf(OPF, "%s", buf->data);

		free(fparname);
		destroyPQExpBuffer(buf);
	}

	PQclear(res);
	fprintf(OPF, "\n\n");
}


/*
 * Drop tablespaces.
 */
static void
dropTablespaces(PGconn *conn)
{
	PGresult   *res;
	int			i;

	/*
	 * Get all tablespaces except built-in ones (which we assume are named
	 * pg_xxx)
	 */
	res = executeQuery(conn, "SELECT spcname "
					   "FROM pg_catalog.pg_tablespace "
					   "WHERE spcname !~ '^pg_' "
					   "ORDER BY 1");

	if (PQntuples(res) > 0)
		fprintf(OPF, "--\n-- Drop tablespaces\n--\n\n");

	for (i = 0; i < PQntuples(res); i++)
	{
		char	   *spcname = PQgetvalue(res, i, 0);

		fprintf(OPF, "DROP TABLESPACE %s%s;\n",
				if_exists ? "IF EXISTS " : "",
				fmtId(spcname));
	}

	PQclear(res);

	fprintf(OPF, "\n\n");
}

/*
 * Dump tablespaces.
 */
static void
dumpTablespaces(PGconn *conn)
{
	PGresult   *res;
	int			i;

	/*
	 * Get all tablespaces except built-in ones (which we assume are named
	 * pg_xxx)
	 */
	res = executeQuery(conn, "SELECT oid, spcname, "
					   "pg_catalog.pg_get_userbyid(spcowner) AS spcowner, "
					   "pg_catalog.pg_tablespace_location(oid), "
					   "spcacl, acldefault('t', spcowner) AS acldefault, "
					   "array_to_string(spcoptions, ', '),"
					   "pg_catalog.shobj_description(oid, 'pg_tablespace') "
					   "FROM pg_catalog.pg_tablespace "
					   "WHERE spcname !~ '^pg_' "
					   "ORDER BY 1");

	if (PQntuples(res) > 0)
		fprintf(OPF, "--\n-- Tablespaces\n--\n\n");

	for (i = 0; i < PQntuples(res); i++)
	{
		PQExpBuffer buf = createPQExpBuffer();
		Oid			spcoid = atooid(PQgetvalue(res, i, 0));
		char	   *spcname = PQgetvalue(res, i, 1);
		char	   *spcowner = PQgetvalue(res, i, 2);
		char	   *spclocation = PQgetvalue(res, i, 3);
		char	   *spcacl = PQgetvalue(res, i, 4);
		char	   *acldefault = PQgetvalue(res, i, 5);
		char	   *spcoptions = PQgetvalue(res, i, 6);
		char	   *spccomment = PQgetvalue(res, i, 7);
		char	   *fspcname;

		/* needed for buildACLCommands() */
		fspcname = pg_strdup(fmtId(spcname));

		if (binary_upgrade)
		{
			appendPQExpBufferStr(buf, "\n-- For binary upgrade, must preserve pg_tablespace oid\n");
			appendPQExpBuffer(buf, "SELECT pg_catalog.binary_upgrade_set_next_pg_tablespace_oid('%u'::pg_catalog.oid);\n", spcoid);
		}

		appendPQExpBuffer(buf, "CREATE TABLESPACE %s", fspcname);
		appendPQExpBuffer(buf, " OWNER %s", fmtId(spcowner));

		appendPQExpBufferStr(buf, " LOCATION ");

		/*
		 * In-place tablespaces use a relative path, and need to be dumped
		 * with an empty string as location.
		 */
		if (is_absolute_path(spclocation))
			appendStringLiteralConn(buf, spclocation, conn);
		else
			appendStringLiteralConn(buf, "", conn);

		appendPQExpBufferStr(buf, ";\n");

		if (spcoptions && spcoptions[0] != '\0')
			appendPQExpBuffer(buf, "ALTER TABLESPACE %s SET (%s);\n",
							  fspcname, spcoptions);

		/* tablespaces can't have initprivs */

		if (!skip_acls &&
			!buildACLCommands(fspcname, NULL, NULL, "TABLESPACE",
							  spcacl, acldefault,
							  spcowner, "", server_version, buf))
		{
			pg_log_error("could not parse ACL list (%s) for tablespace \"%s\"",
						 spcacl, spcname);
			PQfinish(conn);
			exit_nicely(1);
		}

		if (!no_comments && spccomment && spccomment[0] != '\0')
		{
			appendPQExpBuffer(buf, "COMMENT ON TABLESPACE %s IS ", fspcname);
			appendStringLiteralConn(buf, spccomment, conn);
			appendPQExpBufferStr(buf, ";\n");
		}

		if (!no_security_labels)
			buildShSecLabels(conn, "pg_tablespace", spcoid,
							 "TABLESPACE", spcname,
							 buf);

		fprintf(OPF, "%s", buf->data);

		free(fspcname);
		destroyPQExpBuffer(buf);
	}

	PQclear(res);
	fprintf(OPF, "\n\n");
}


/*
 * Dump commands to drop each database.
 */
static void
dropDBs(PGconn *conn)
{
	PGresult   *res;
	int			i;

	/*
	 * Skip databases marked not datallowconn, since we'd be unable to connect
	 * to them anyway.  This must agree with dumpDatabases().
	 */
	res = executeQuery(conn,
					   "SELECT datname "
					   "FROM pg_database d "
					   "WHERE datallowconn AND datconnlimit != -2 "
					   "ORDER BY datname");

	if (PQntuples(res) > 0)
		fprintf(OPF, "--\n-- Drop databases (except postgres and template1)\n--\n\n");

	for (i = 0; i < PQntuples(res); i++)
	{
		char	   *dbname = PQgetvalue(res, i, 0);

		/*
		 * Skip "postgres" and "template1"; dumpDatabases() will deal with
		 * them specially.  Also, be sure to skip "template0", even if for
		 * some reason it's not marked !datallowconn.
		 */
		if (strcmp(dbname, "template1") != 0 &&
			strcmp(dbname, "template0") != 0 &&
			strcmp(dbname, "postgres") != 0)
		{
			fprintf(OPF, "DROP DATABASE %s%s;\n",
					if_exists ? "IF EXISTS " : "",
					fmtId(dbname));
		}
	}

	PQclear(res);

	fprintf(OPF, "\n\n");
}


/*
 * Dump user-specific configuration
 */
static void
dumpUserConfig(PGconn *conn, const char *username)
{
	PQExpBuffer buf = createPQExpBuffer();
	PGresult   *res;

	printfPQExpBuffer(buf, "SELECT unnest(setconfig) FROM pg_db_role_setting "
					  "WHERE setdatabase = 0 AND setrole = "
					  "(SELECT oid FROM %s WHERE rolname = ",
					  role_catalog);
	appendStringLiteralConn(buf, username, conn);
	appendPQExpBufferChar(buf, ')');

	res = executeQuery(conn, buf->data);

	if (PQntuples(res) > 0)
		fprintf(OPF, "\n--\n-- User Config \"%s\"\n--\n\n", username);

	for (int i = 0; i < PQntuples(res); i++)
	{
		resetPQExpBuffer(buf);
		makeAlterConfigCommand(conn, PQgetvalue(res, i, 0),
							   "ROLE", username, NULL, NULL,
							   buf);
		fprintf(OPF, "%s", buf->data);
	}

	PQclear(res);

	destroyPQExpBuffer(buf);
}

/*
 * Find a list of database names that match the given patterns.
 * See also expand_table_name_patterns() in pg_dump.c
 */
static void
expand_dbname_patterns(PGconn *conn,
					   SimpleStringList *patterns,
					   SimpleStringList *names)
{
	PQExpBuffer query;
	PGresult   *res;

	if (patterns->head == NULL)
		return;					/* nothing to do */

	query = createPQExpBuffer();

	/*
	 * The loop below runs multiple SELECTs, which might sometimes result in
	 * duplicate entries in the name list, but we don't care, since all we're
	 * going to do is test membership of the list.
	 */

	for (SimpleStringListCell *cell = patterns->head; cell; cell = cell->next)
	{
		int			dotcnt;

		appendPQExpBufferStr(query,
							 "SELECT datname FROM pg_catalog.pg_database n\n");
		processSQLNamePattern(conn, query, cell->val, false,
							  false, NULL, "datname", NULL, NULL, NULL,
							  &dotcnt);

		if (dotcnt > 0)
		{
			pg_log_error("improper qualified name (too many dotted names): %s",
						 cell->val);
			PQfinish(conn);
			exit_nicely(1);
		}

		res = executeQuery(conn, query->data);
		for (int i = 0; i < PQntuples(res); i++)
		{
			simple_string_list_append(names, PQgetvalue(res, i, 0));
		}

		PQclear(res);
		resetPQExpBuffer(query);
	}

	destroyPQExpBuffer(query);
}

/*
 * Dump contents of databases.
 */
static void
dumpDatabases(PGconn *conn)
{
	PGresult   *res;
	int			i;

	/*
	 * Skip databases marked not datallowconn, since we'd be unable to connect
	 * to them anyway.  This must agree with dropDBs().
	 *
	 * We arrange for template1 to be processed first, then we process other
	 * DBs in alphabetical order.  If we just did them all alphabetically, we
	 * might find ourselves trying to drop the "postgres" database while still
	 * connected to it.  This makes trying to run the restore script while
	 * connected to "template1" a bad idea, but there's no fixed order that
	 * doesn't have some failure mode with --clean.
	 */
	res = executeQuery(conn,
					   "SELECT datname "
					   "FROM pg_database d "
					   "WHERE datallowconn AND datconnlimit != -2 "
					   "ORDER BY (datname <> 'template1'), datname");

	if (PQntuples(res) > 0)
		fprintf(OPF, "--\n-- Databases\n--\n\n");

	for (i = 0; i < PQntuples(res); i++)
	{
		char	   *dbname = PQgetvalue(res, i, 0);
		const char *create_opts;
		int			ret;

		/* Skip template0, even if it's not marked !datallowconn. */
		if (strcmp(dbname, "template0") == 0)
			continue;

		/* Skip any explicitly excluded database */
		if (simple_string_list_member(&database_exclude_names, dbname))
		{
			pg_log_info("excluding database \"%s\"", dbname);
			continue;
		}

		pg_log_info("dumping database \"%s\"", dbname);

		fprintf(OPF, "--\n-- Database \"%s\" dump\n--\n\n", dbname);

		/*
		 * We assume that "template1" and "postgres" already exist in the
		 * target installation.  dropDBs() won't have removed them, for fear
		 * of removing the DB the restore script is initially connected to. If
		 * --clean was specified, tell pg_dump to drop and recreate them;
		 * otherwise we'll merely restore their contents.  Other databases
		 * should simply be created.
		 */
		if (strcmp(dbname, "template1") == 0 || strcmp(dbname, "postgres") == 0)
		{
			if (output_clean)
				create_opts = "--clean --create";
			else
			{
				create_opts = "";
				/* Since pg_dump won't emit a \connect command, we must */
				fprintf(OPF, "\\connect %s\n\n", dbname);
			}
		}
		else
			create_opts = "--create";

		if (filename)
			fclose(OPF);

		ret = runPgDump(dbname, create_opts);
		if (ret != 0)
			pg_fatal("pg_dump failed on database \"%s\", exiting", dbname);

		if (filename)
		{
			OPF = fopen(filename, PG_BINARY_A);
			if (!OPF)
				pg_fatal("could not re-open the output file \"%s\": %m",
						 filename);
		}
	}

	PQclear(res);
}



/*
 * Run pg_dump on dbname, with specified options.
 */
static int
runPgDump(const char *dbname, const char *create_opts)
{
	PQExpBufferData connstrbuf;
	PQExpBufferData cmd;
	int			ret;

	initPQExpBuffer(&connstrbuf);
	initPQExpBuffer(&cmd);

	printfPQExpBuffer(&cmd, "\"%s\" %s %s", pg_dump_bin,
					  pgdumpopts->data, create_opts);

	/*
	 * If we have a filename, use the undocumented plain-append pg_dump
	 * format.
	 */
	if (filename)
		appendPQExpBufferStr(&cmd, " -Fa ");
	else
		appendPQExpBufferStr(&cmd, " -Fp ");

	/*
	 * Append the database name to the already-constructed stem of connection
	 * string.
	 */
	appendPQExpBuffer(&connstrbuf, "%s dbname=", connstr);
	appendConnStrVal(&connstrbuf, dbname);

	appendShellString(&cmd, connstrbuf.data);

	pg_log_info("running \"%s\"", cmd.data);

	fflush(NULL);

	ret = system(cmd.data);

	termPQExpBuffer(&cmd);
	termPQExpBuffer(&connstrbuf);

	return ret;
}

/*
 * buildShSecLabels
 *
 * Build SECURITY LABEL command(s) for a shared object
 *
 * The caller has to provide object type and identity in two separate formats:
 * catalog_name (e.g., "pg_database") and object OID, as well as
 * type name (e.g., "DATABASE") and object name (not pre-quoted).
 *
 * The command(s) are appended to "buffer".
 */
static void
buildShSecLabels(PGconn *conn, const char *catalog_name, Oid objectId,
				 const char *objtype, const char *objname,
				 PQExpBuffer buffer)
{
	PQExpBuffer sql = createPQExpBuffer();
	PGresult   *res;

	buildShSecLabelQuery(catalog_name, objectId, sql);
	res = executeQuery(conn, sql->data);
	emitShSecLabels(conn, res, buffer, objtype, objname);

	PQclear(res);
	destroyPQExpBuffer(sql);
}

/*
 * Make a database connection with the given parameters.  An
 * interactive password prompt is automatically issued if required.
 *
 * If fail_on_error is false, we return NULL without printing any message
 * on failure, but preserve any prompted password for the next try.
 *
 * On success, the global variable 'connstr' is set to a connection string
 * containing the options used.
 */
static PGconn *
connectDatabase(const char *dbname, const char *connection_string,
				const char *pghost, const char *pgport, const char *pguser,
				trivalue prompt_password, bool fail_on_error)
{
	PGconn	   *conn;
	bool		new_pass;
	const char *remoteversion_str;
	int			my_version;
	const char **keywords = NULL;
	const char **values = NULL;
	PQconninfoOption *conn_opts = NULL;
	static char *password = NULL;

	if (prompt_password == TRI_YES && !password)
		password = simple_prompt("Password: ", false);

	/*
	 * Start the connection.  Loop until we have a password if requested by
	 * backend.
	 */
	do
	{
		int			argcount = 6;
		PQconninfoOption *conn_opt;
		char	   *err_msg = NULL;
		int			i = 0;

		free(keywords);
		free(values);
		PQconninfoFree(conn_opts);

		/*
		 * Merge the connection info inputs given in form of connection string
		 * and other options.  Explicitly discard any dbname value in the
		 * connection string; otherwise, PQconnectdbParams() would interpret
		 * that value as being itself a connection string.
		 */
		if (connection_string)
		{
			conn_opts = PQconninfoParse(connection_string, &err_msg);
			if (conn_opts == NULL)
				pg_fatal("%s", err_msg);

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

		if (pghost)
		{
			keywords[i] = "host";
			values[i] = pghost;
			i++;
		}
		if (pgport)
		{
			keywords[i] = "port";
			values[i] = pgport;
			i++;
		}
		if (pguser)
		{
			keywords[i] = "user";
			values[i] = pguser;
			i++;
		}
		if (password)
		{
			keywords[i] = "password";
			values[i] = password;
			i++;
		}
		if (dbname)
		{
			keywords[i] = "dbname";
			values[i] = dbname;
			i++;
		}
		keywords[i] = "fallback_application_name";
		values[i] = progname;
		i++;

		new_pass = false;
		conn = PQconnectdbParams(keywords, values, true);

		if (!conn)
			pg_fatal("could not connect to database \"%s\"", dbname);

		if (PQstatus(conn) == CONNECTION_BAD &&
			PQconnectionNeedsPassword(conn) &&
			!password &&
			prompt_password != TRI_NO)
		{
			PQfinish(conn);
			password = simple_prompt("Password: ", false);
			new_pass = true;
		}
	} while (new_pass);

	/* check to see that the backend connection was successfully made */
	if (PQstatus(conn) == CONNECTION_BAD)
	{
		if (fail_on_error)
			pg_fatal("%s", PQerrorMessage(conn));
		else
		{
			PQfinish(conn);

			free(keywords);
			free(values);
			PQconninfoFree(conn_opts);

			return NULL;
		}
	}

	/*
	 * Ok, connected successfully. Remember the options used, in the form of a
	 * connection string.
	 */
	connstr = constructConnStr(keywords, values);

	free(keywords);
	free(values);
	PQconninfoFree(conn_opts);

	/* Check version */
	remoteversion_str = PQparameterStatus(conn, "server_version");
	if (!remoteversion_str)
		pg_fatal("could not get server version");
	server_version = PQserverVersion(conn);
	if (server_version == 0)
		pg_fatal("could not parse server version \"%s\"",
				 remoteversion_str);

	my_version = PG_VERSION_NUM;

	/*
	 * We allow the server to be back to 9.2, and up to any minor release of
	 * our own major version.  (See also version check in pg_dump.c.)
	 */
	if (my_version != server_version
		&& (server_version < 90200 ||
			(server_version / 100) > (my_version / 100)))
	{
		pg_log_error("aborting because of server version mismatch");
		pg_log_error_detail("server version: %s; %s version: %s",
							remoteversion_str, progname, PG_VERSION);
		exit_nicely(1);
	}

	PQclear(executeQuery(conn, ALWAYS_SECURE_SEARCH_PATH_SQL));

	return conn;
}

/* ----------
 * Construct a connection string from the given keyword/value pairs. It is
 * used to pass the connection options to the pg_dump subprocess.
 *
 * The following parameters are excluded:
 *	dbname		- varies in each pg_dump invocation
 *	password	- it's not secure to pass a password on the command line
 *	fallback_application_name - we'll let pg_dump set it
 * ----------
 */
static char *
constructConnStr(const char **keywords, const char **values)
{
	PQExpBuffer buf = createPQExpBuffer();
	char	   *connstr;
	int			i;
	bool		firstkeyword = true;

	/* Construct a new connection string in key='value' format. */
	for (i = 0; keywords[i] != NULL; i++)
	{
		if (strcmp(keywords[i], "dbname") == 0 ||
			strcmp(keywords[i], "password") == 0 ||
			strcmp(keywords[i], "fallback_application_name") == 0)
			continue;

		if (!firstkeyword)
			appendPQExpBufferChar(buf, ' ');
		firstkeyword = false;
		appendPQExpBuffer(buf, "%s=", keywords[i]);
		appendConnStrVal(buf, values[i]);
	}

	connstr = pg_strdup(buf->data);
	destroyPQExpBuffer(buf);
	return connstr;
}

/*
 * Run a query, return the results, exit program on failure.
 */
static PGresult *
executeQuery(PGconn *conn, const char *query)
{
	PGresult   *res;

	pg_log_info("executing %s", query);

	res = PQexec(conn, query);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("query failed: %s", PQerrorMessage(conn));
		pg_log_error_detail("Query was: %s", query);
		PQfinish(conn);
		exit_nicely(1);
	}

	return res;
}

/*
 * As above for a SQL command (which returns nothing).
 */
static void
executeCommand(PGconn *conn, const char *query)
{
	PGresult   *res;

	pg_log_info("executing %s", query);

	res = PQexec(conn, query);
	if (!res ||
		PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		pg_log_error("query failed: %s", PQerrorMessage(conn));
		pg_log_error_detail("Query was: %s", query);
		PQfinish(conn);
		exit_nicely(1);
	}

	PQclear(res);
}


/*
 * dumpTimestamp
 */
static void
dumpTimestamp(const char *msg)
{
	char		buf[64];
	time_t		now = time(NULL);

	if (strftime(buf, sizeof(buf), PGDUMP_STRFTIME_FMT, localtime(&now)) != 0)
		fprintf(OPF, "-- %s %s\n\n", msg, buf);
}

/*
 * read_dumpall_filters - retrieve database identifier patterns from file
 *
 * Parse the specified filter file for include and exclude patterns, and add
 * them to the relevant lists.  If the filename is "-" then filters will be
 * read from STDIN rather than a file.
 *
 * At the moment, the only allowed filter is for database exclusion.
 */
static void
read_dumpall_filters(const char *filename, SimpleStringList *pattern)
{
	FilterStateData fstate;
	char	   *objname;
	FilterCommandType comtype;
	FilterObjectType objtype;

	filter_init(&fstate, filename, exit);

	while (filter_read_item(&fstate, &objname, &comtype, &objtype))
	{
		if (comtype == FILTER_COMMAND_TYPE_INCLUDE)
		{
			pg_log_filter_error(&fstate, _("%s filter for \"%s\" is not allowed"),
								"include",
								filter_object_type_name(objtype));
			exit_nicely(1);
		}

		switch (objtype)
		{
			case FILTER_OBJECT_TYPE_NONE:
				break;
			case FILTER_OBJECT_TYPE_FUNCTION:
			case FILTER_OBJECT_TYPE_INDEX:
			case FILTER_OBJECT_TYPE_TABLE_DATA:
			case FILTER_OBJECT_TYPE_TABLE_DATA_AND_CHILDREN:
			case FILTER_OBJECT_TYPE_TRIGGER:
			case FILTER_OBJECT_TYPE_EXTENSION:
			case FILTER_OBJECT_TYPE_FOREIGN_DATA:
			case FILTER_OBJECT_TYPE_SCHEMA:
			case FILTER_OBJECT_TYPE_TABLE:
			case FILTER_OBJECT_TYPE_TABLE_AND_CHILDREN:
				pg_log_filter_error(&fstate, _("unsupported filter object"));
				exit_nicely(1);
				break;

			case FILTER_OBJECT_TYPE_DATABASE:
				simple_string_list_append(pattern, objname);
				break;
		}

		if (objname)
			free(objname);
	}

	filter_free(&fstate);
}
