/*-------------------------------------------------------------------------
 *
 * pg_dumpall
 *
 * Portions Copyright (c) 1996-2004, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * $PostgreSQL: pgsql/src/bin/pg_dump/pg_dumpall.c,v 1.56 2004/12/03 18:48:19 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <unistd.h>
#ifdef ENABLE_NLS
#include <locale.h>
#endif
#ifndef HAVE_STRDUP
#include "strdup.h"
#endif
#include <errno.h>
#include <time.h>

#include "getopt_long.h"

#ifndef HAVE_INT_OPTRESET
int			optreset;
#endif

#include "dumputils.h"
#include "libpq-fe.h"
#include "pg_backup.h"
#include "pqexpbuffer.h"

#define _(x) gettext((x))

/* version string we expect back from postgres */
#define PG_VERSIONSTR "pg_dump (PostgreSQL) " PG_VERSION "\n"


static const char *progname;

static void help(void);

static void dumpUsers(PGconn *conn, bool initdbonly);
static void dumpGroups(PGconn *conn);
static void dumpTablespaces(PGconn *conn);
static void dumpCreateDB(PGconn *conn);
static void dumpDatabaseConfig(PGconn *conn, const char *dbname);
static void dumpUserConfig(PGconn *conn, const char *username);
static void makeAlterConfigCommand(const char *arrayitem, const char *type, const char *name);
static void dumpDatabases(PGconn *conn);
static void dumpTimestamp(char *msg);

static int	runPgDump(const char *dbname);
static PGconn *connectDatabase(const char *dbname, const char *pghost, const char *pgport,
				const char *pguser, bool require_password);
static PGresult *executeQuery(PGconn *conn, const char *query);

char		pg_dump_bin[MAXPGPATH];
PQExpBuffer pgdumpopts;
bool		output_clean = false;
bool		skip_acls = false;
bool		verbose = false;
int			server_version;

/* flags for -X long options */
int			disable_dollar_quoting = 0;
int			disable_triggers = 0;
int			use_setsessauth = 0;

int
main(int argc, char *argv[])
{
	char	   *pghost = NULL;
	char	   *pgport = NULL;
	char	   *pguser = NULL;
	bool		force_password = false;
	bool		data_only = false;
	bool		globals_only = false;
	bool		schema_only = false;
	PGconn	   *conn;
	int			c,
				ret;

	static struct option long_options[] = {
		{"data-only", no_argument, NULL, 'a'},
		{"clean", no_argument, NULL, 'c'},
		{"inserts", no_argument, NULL, 'd'},
		{"attribute-inserts", no_argument, NULL, 'D'},
		{"column-inserts", no_argument, NULL, 'D'},
		{"globals-only", no_argument, NULL, 'g'},
		{"host", required_argument, NULL, 'h'},
		{"ignore-version", no_argument, NULL, 'i'},
		{"oids", no_argument, NULL, 'o'},
		{"no-owner", no_argument, NULL, 'O'},
		{"port", required_argument, NULL, 'p'},
		{"password", no_argument, NULL, 'W'},
		{"schema-only", no_argument, NULL, 's'},
		{"superuser", required_argument, NULL, 'S'},
		{"username", required_argument, NULL, 'U'},
		{"verbose", no_argument, NULL, 'v'},
		{"no-privileges", no_argument, NULL, 'x'},
		{"no-acl", no_argument, NULL, 'x'},

		/*
		 * the following options don't have an equivalent short option
		 * letter, but are available as '-X long-name'
		 */
		{"disable-dollar-quoting", no_argument, &disable_dollar_quoting, 1},
		{"disable-triggers", no_argument, &disable_triggers, 1},
		{"use-set-session-authorization", no_argument, &use_setsessauth, 1},

		{NULL, 0, NULL, 0}
	};

	int			optindex;

	set_pglocale_pgservice(argv[0], "pg_dump");

	progname = get_progname(argv[0]);

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			help();
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_dumpall (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	if ((ret = find_other_exec(argv[0], "pg_dump", PG_VERSIONSTR,
							   pg_dump_bin)) < 0)
	{
		char full_path[MAXPGPATH];

		if (find_my_exec(argv[0], full_path) < 0)
			StrNCpy(full_path, progname, MAXPGPATH);

		if (ret == -1)
			fprintf(stderr,
					_("The program \"pg_dump\" is needed by %s "
					  "but was not found in the\n"
					  "same directory as \"%s\".\n"
					  "Check your installation.\n"),
					progname, full_path);
		else
			fprintf(stderr,
					_("The program \"pg_dump\" was found by \"%s\"\n"
					  "but was not the same version as %s.\n"
					  "Check your installation.\n"),
					full_path, progname);
		exit(1);
	}

	pgdumpopts = createPQExpBuffer();

	while ((c = getopt_long(argc, argv, "acdDgh:ioOp:sS:U:vWxX:", long_options, &optindex)) != -1)
	{
		switch (c)
		{
			case 'a':
				data_only = true;
				appendPQExpBuffer(pgdumpopts, " -a");
				break;

			case 'c':
				output_clean = true;
				break;

			case 'd':
			case 'D':
				appendPQExpBuffer(pgdumpopts, " -%c", c);
				break;

			case 'g':
				globals_only = true;
				break;

			case 'h':
				pghost = optarg;
#ifndef WIN32
				appendPQExpBuffer(pgdumpopts, " -h '%s'", pghost);
#else
                                appendPQExpBuffer(pgdumpopts, " -h \"%s\"", pghost);
#endif

				break;



			case 'i':
			case 'o':
				appendPQExpBuffer(pgdumpopts, " -%c", c);
				break;

			case 'O':
				appendPQExpBuffer(pgdumpopts, " -O");
				break;

			case 'p':
				pgport = optarg;
#ifndef WIN32
				appendPQExpBuffer(pgdumpopts, " -p '%s'", pgport);
#else
                                appendPQExpBuffer(pgdumpopts, " -p \"%s\"", pgport);
#endif
				break;

			case 's':
				schema_only = true;
				appendPQExpBuffer(pgdumpopts, " -s");
				break;

			case 'S':
#ifndef WIN32
				appendPQExpBuffer(pgdumpopts, " -S '%s'", optarg);
#else
                                appendPQExpBuffer(pgdumpopts, " -S \"%s\"", optarg);
#endif
				break;

			case 'U':
				pguser = optarg;
#ifndef WIN32
				appendPQExpBuffer(pgdumpopts, " -U '%s'", pguser);
#else
                                appendPQExpBuffer(pgdumpopts, " -U \"%s\"", pguser);
#endif
				break;

			case 'v':
				verbose = true;
				appendPQExpBuffer(pgdumpopts, " -v");
				break;

			case 'W':
				force_password = true;
				appendPQExpBuffer(pgdumpopts, " -W");
				break;

			case 'x':
				skip_acls = true;
				appendPQExpBuffer(pgdumpopts, " -x");
				break;

			case 'X':
				if (strcmp(optarg, "disable-dollar-quoting") == 0)
					appendPQExpBuffer(pgdumpopts, " -X disable-dollar-quoting");
				else if (strcmp(optarg, "disable-triggers") == 0)
					appendPQExpBuffer(pgdumpopts, " -X disable-triggers");
				else if (strcmp(optarg, "use-set-session-authorization") == 0)
					 /* no-op, still allowed for compatibility */ ;
				else
				{
					fprintf(stderr,
							_("%s: invalid -X option -- %s\n"),
							progname, optarg);
					fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
					exit(1);
				}
				break;

			case 0:
				break;

			default:
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
				exit(1);
		}
	}

	/* Add long options to the pg_dump argument list */
	if (disable_dollar_quoting)
		appendPQExpBuffer(pgdumpopts, " -X disable-dollar-quoting");
	if (disable_triggers)
		appendPQExpBuffer(pgdumpopts, " -X disable-triggers");
	if (use_setsessauth)
		appendPQExpBuffer(pgdumpopts, " -X use-set-session-authorization");

	if (optind < argc)
	{
		fprintf(stderr, _("%s: too many command-line arguments (first is \"%s\")\n"),
				progname, argv[optind]);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}


	conn = connectDatabase("template1", pghost, pgport, pguser, force_password);

	printf("--\n-- PostgreSQL database cluster dump\n--\n\n");
	if (verbose)
		dumpTimestamp("Started on");

	printf("\\connect \"template1\"\n\n");

	if (!data_only)
	{
		/* Dump all users excluding the initdb user */
		dumpUsers(conn, false);
		dumpGroups(conn);
		if (server_version >= 80000)
			dumpTablespaces(conn);
		if (!globals_only)
			dumpCreateDB(conn);
		/* Dump alter command for initdb user */
		dumpUsers(conn, true);
	}

	if (!globals_only)
		dumpDatabases(conn);

	PQfinish(conn);

	if (verbose)
		dumpTimestamp("Completed on");
	printf("--\n-- PostgreSQL database cluster dump complete\n--\n\n");

	exit(0);
}



static void
help(void)
{
	printf(_("%s extracts a PostgreSQL database cluster into an SQL script file.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]...\n"), progname);

	printf(_("\nGeneral options:\n"));
	printf(_("  -i, --ignore-version     proceed even when server version mismatches\n"
			 "                           pg_dumpall version\n"));
	printf(_("  --help                   show this help, then exit\n"));
	printf(_("  --version                output version information, then exit\n"));
	printf(_("\nOptions controlling the output content:\n"));
	printf(_("  -a, --data-only          dump only the data, not the schema\n"));
	printf(_("  -c, --clean              clean (drop) databases prior to create\n"));
	printf(_("  -d, --inserts            dump data as INSERT, rather than COPY, commands\n"));
	printf(_("  -D, --column-inserts     dump data as INSERT commands with column names\n"));
	printf(_("  -g, --globals-only       dump only global objects, no databases\n"));
	printf(_("  -o, --oids               include OIDs in dump\n"));
	printf(_("  -O, --no-owner           skip restoration of object ownership\n"));
	printf(_("  -s, --schema-only        dump only the schema, no data\n"));
	printf(_("  -S, --superuser=NAME     specify the superuser user name to use in the dump\n"));
	printf(_("  -x, --no-privileges      do not dump privileges (grant/revoke)\n"));
	printf(_("  -X disable-dollar-quoting, --disable-dollar-quoting\n"
			 "                           disable dollar quoting, use SQL standard quoting\n"));
	printf(_("  -X disable-triggers, --disable-triggers\n"
			 "                           disable triggers during data-only restore\n"));
	printf(_("  -X use-set-session-authorization, --use-set-session-authorization\n"
			 "                           use SESSION AUTHORIZATION commands instead of\n"
			 "                           OWNER TO commands\n"));

	printf(_("\nConnection options:\n"));
	printf(_("  -h, --host=HOSTNAME      database server host or socket directory\n"));
	printf(_("  -p, --port=PORT          database server port number\n"));
	printf(_("  -U, --username=NAME      connect as specified database user\n"));
	printf(_("  -W, --password           force password prompt (should happen automatically)\n"));

	printf(_("\nThe SQL script will be written to the standard output.\n\n"));
	printf(_("Report bugs to <pgsql-bugs@postgresql.org>.\n"));
}



/*
 * Dump users
 * Is able to dump all non initdb users or just the initdb user.
 */
static void
dumpUsers(PGconn *conn, bool initdbonly)
{
	PGresult   *res;
	int			i;

	if (server_version >= 70100)
		res = executeQuery(conn,
						"SELECT usename, usesysid, passwd, usecreatedb, "
						   "usesuper, valuntil, "
						   "(usesysid = (SELECT datdba FROM pg_database WHERE datname = 'template0')) AS clusterowner "
						   "FROM pg_shadow");
	else
		res = executeQuery(conn,
						"SELECT usename, usesysid, passwd, usecreatedb, "
						   "usesuper, valuntil, "
						   "(usesysid = (SELECT datdba FROM pg_database WHERE datname = 'template1')) AS clusterowner "
						   "FROM pg_shadow");

	if (PQntuples(res) > 0 || (!initdbonly && output_clean))
		printf("--\n-- Users\n--\n\n");
	if (!initdbonly && output_clean)
		printf("DELETE FROM pg_shadow WHERE usesysid <> (SELECT datdba FROM pg_database WHERE datname = 'template0');\n\n");

	for (i = 0; i < PQntuples(res); i++)
	{
		const char *username;
		bool		clusterowner;
		PQExpBuffer buf = createPQExpBuffer();

		username = PQgetvalue(res, i, 0);
		clusterowner = (strcmp(PQgetvalue(res, i, 6), "t") == 0);

		/* Check which pass we're on */
		if ((initdbonly && !clusterowner) || (!initdbonly && clusterowner))
			continue;

		/*
		 * Dump ALTER USER for the cluster owner and CREATE USER for all
		 * other users
		 */
		if (!clusterowner)
			appendPQExpBuffer(buf, "CREATE USER %s WITH SYSID %s",
							  fmtId(username),
							  PQgetvalue(res, i, 1));
		else
			appendPQExpBuffer(buf, "ALTER USER %s WITH",
							  fmtId(username));

		if (!PQgetisnull(res, i, 2))
		{
			appendPQExpBuffer(buf, " PASSWORD ");
			appendStringLiteral(buf, PQgetvalue(res, i, 2), true);
		}

		if (strcmp(PQgetvalue(res, i, 3), "t") == 0)
			appendPQExpBuffer(buf, " CREATEDB");
		else
			appendPQExpBuffer(buf, " NOCREATEDB");

		if (strcmp(PQgetvalue(res, i, 4), "t") == 0)
			appendPQExpBuffer(buf, " CREATEUSER");
		else
			appendPQExpBuffer(buf, " NOCREATEUSER");

		if (!PQgetisnull(res, i, 5))
			appendPQExpBuffer(buf, " VALID UNTIL '%s'",
							  PQgetvalue(res, i, 5));

		appendPQExpBuffer(buf, ";\n");

		printf("%s", buf->data);
		destroyPQExpBuffer(buf);

		if (server_version >= 70300)
			dumpUserConfig(conn, username);
	}

	PQclear(res);
	printf("\n\n");
}



/*
 * Dump groups.
 */
static void
dumpGroups(PGconn *conn)
{
	PGresult   *res;
	int			i;

	res = executeQuery(conn, "SELECT groname, grosysid, grolist FROM pg_group");

	if (PQntuples(res) > 0 || output_clean)
		printf("--\n-- Groups\n--\n\n");
	if (output_clean)
		printf("DELETE FROM pg_group;\n\n");

	for (i = 0; i < PQntuples(res); i++)
	{
		PQExpBuffer buf = createPQExpBuffer();
		char	   *val;
		char	   *tok;

		appendPQExpBuffer(buf, "CREATE GROUP %s WITH SYSID %s;\n",
						  fmtId(PQgetvalue(res, i, 0)),
						  PQgetvalue(res, i, 1));

		val = strdup(PQgetvalue(res, i, 2));
		tok = strtok(val, ",{}");
		while (tok)
		{
			PGresult   *res2;
			PQExpBuffer buf2 = createPQExpBuffer();
			int			j;

			appendPQExpBuffer(buf2, "SELECT usename FROM pg_shadow WHERE usesysid = %s;", tok);
			res2 = executeQuery(conn, buf2->data);
			destroyPQExpBuffer(buf2);

			for (j = 0; j < PQntuples(res2); j++)
			{
				appendPQExpBuffer(buf, "ALTER GROUP %s ", fmtId(PQgetvalue(res, i, 0)));
				appendPQExpBuffer(buf, "ADD USER %s;\n", fmtId(PQgetvalue(res2, j, 0)));
			}

			PQclear(res2);

			tok = strtok(NULL, "{},");
		}
		free(val);

		printf("%s", buf->data);
		destroyPQExpBuffer(buf);
	}

	PQclear(res);
	printf("\n\n");
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
	res = executeQuery(conn, "SELECT spcname, "
					 "pg_catalog.pg_get_userbyid(spcowner) AS spcowner, "
					   "spclocation, spcacl "
					   "FROM pg_catalog.pg_tablespace "
					   "WHERE spcname NOT LIKE 'pg\\_%'");

	if (PQntuples(res) > 0)
		printf("--\n-- Tablespaces\n--\n\n");

	for (i = 0; i < PQntuples(res); i++)
	{
		PQExpBuffer buf = createPQExpBuffer();
		char	   *spcname = PQgetvalue(res, i, 0);
		char	   *spcowner = PQgetvalue(res, i, 1);
		char	   *spclocation = PQgetvalue(res, i, 2);
		char	   *spcacl = PQgetvalue(res, i, 3);
		char	   *fspcname;

		/* needed for buildACLCommands() */
		fspcname = strdup(fmtId(spcname));

		if (output_clean)
			appendPQExpBuffer(buf, "DROP TABLESPACE %s;\n", fspcname);

		appendPQExpBuffer(buf, "CREATE TABLESPACE %s", fspcname);
		appendPQExpBuffer(buf, " OWNER %s", fmtId(spcowner));

		appendPQExpBuffer(buf, " LOCATION ");
		appendStringLiteral(buf, spclocation, true);
		appendPQExpBuffer(buf, ";\n");

		if (!skip_acls &&
			!buildACLCommands(fspcname, "TABLESPACE", spcacl, spcowner,
							  server_version, buf))
		{
			fprintf(stderr, _("%s: could not parse ACL list (%s) for tablespace \"%s\"\n"),
					progname, spcacl, fspcname);
			PQfinish(conn);
			exit(1);
		}

		printf("%s", buf->data);

		free(fspcname);
		destroyPQExpBuffer(buf);
	}

	PQclear(res);
	printf("\n\n");
}

/*
 * Dump commands to create each database.
 *
 * To minimize the number of reconnections (and possibly ensuing
 * password prompts) required by the output script, we emit all CREATE
 * DATABASE commands during the initial phase of the script, and then
 * run pg_dump for each database to dump the contents of that
 * database.  We skip databases marked not datallowconn, since we'd be
 * unable to connect to them anyway (and besides, we don't want to
 * dump template0).
 */
static void
dumpCreateDB(PGconn *conn)
{
	PGresult   *res;
	int			i;

	printf("--\n-- Database creation\n--\n\n");

	if (server_version >= 80000)
		res = executeQuery(conn,
						   "SELECT datname, "
						   "coalesce(usename, (select usename from pg_shadow where usesysid=(select datdba from pg_database where datname='template0'))), "
						   "pg_encoding_to_char(d.encoding), "
						   "datistemplate, datacl, "
						   "(SELECT spcname FROM pg_tablespace t WHERE t.oid = d.dattablespace) AS dattablespace "
		"FROM pg_database d LEFT JOIN pg_shadow u ON (datdba = usesysid) "
						   "WHERE datallowconn ORDER BY 1");
	else if (server_version >= 70300)
		res = executeQuery(conn,
						   "SELECT datname, "
						   "coalesce(usename, (select usename from pg_shadow where usesysid=(select datdba from pg_database where datname='template0'))), "
						   "pg_encoding_to_char(d.encoding), "
						   "datistemplate, datacl, "
						   "'pg_default' AS dattablespace "
		"FROM pg_database d LEFT JOIN pg_shadow u ON (datdba = usesysid) "
						   "WHERE datallowconn ORDER BY 1");
	else if (server_version >= 70100)
		res = executeQuery(conn,
						   "SELECT datname, "
						   "coalesce("
				"(select usename from pg_shadow where usesysid=datdba), "
						   "(select usename from pg_shadow where usesysid=(select datdba from pg_database where datname='template0'))), "
						   "pg_encoding_to_char(d.encoding), "
						   "datistemplate, '' as datacl, "
						   "'pg_default' AS dattablespace "
						   "FROM pg_database d "
						   "WHERE datallowconn ORDER BY 1");
	else
	{
		/*
		 * Note: 7.0 fails to cope with sub-select in COALESCE, so just
		 * deal with getting a NULL by not printing any OWNER clause.
		 */
		res = executeQuery(conn,
						   "SELECT datname, "
				"(select usename from pg_shadow where usesysid=datdba), "
						   "pg_encoding_to_char(d.encoding), "
						   "'f' as datistemplate, "
						   "'' as datacl, "
						   "'pg_default' AS dattablespace "
						   "FROM pg_database d "
						   "ORDER BY 1");
	}

	for (i = 0; i < PQntuples(res); i++)
	{
		PQExpBuffer buf;
		char	   *dbname = PQgetvalue(res, i, 0);
		char	   *dbowner = PQgetvalue(res, i, 1);
		char	   *dbencoding = PQgetvalue(res, i, 2);
		char	   *dbistemplate = PQgetvalue(res, i, 3);
		char	   *dbacl = PQgetvalue(res, i, 4);
		char	   *dbtablespace = PQgetvalue(res, i, 5);
		char	   *fdbname;

		if (strcmp(dbname, "template1") == 0)
			continue;

		buf = createPQExpBuffer();

		/* needed for buildACLCommands() */
		fdbname = strdup(fmtId(dbname));

		if (output_clean)
			appendPQExpBuffer(buf, "DROP DATABASE %s;\n", fdbname);

		appendPQExpBuffer(buf, "CREATE DATABASE %s", fdbname);

		appendPQExpBuffer(buf, " WITH TEMPLATE = template0");

		if (strlen(dbowner) != 0)
			appendPQExpBuffer(buf, " OWNER = %s",
							  fmtId(dbowner));

		appendPQExpBuffer(buf, " ENCODING = ");
		appendStringLiteral(buf, dbencoding, true);

		/* Output tablespace if it isn't default */
		if (strcmp(dbtablespace, "pg_default") != 0)
			appendPQExpBuffer(buf, " TABLESPACE = %s",
							  fmtId(dbtablespace));

		appendPQExpBuffer(buf, ";\n");

		if (strcmp(dbistemplate, "t") == 0)
		{
			appendPQExpBuffer(buf, "UPDATE pg_database SET datistemplate = 't' WHERE datname = ");
			appendStringLiteral(buf, dbname, true);
			appendPQExpBuffer(buf, ";\n");
		}

		if (!skip_acls &&
			!buildACLCommands(fdbname, "DATABASE", dbacl, dbowner,
							  server_version, buf))
		{
			fprintf(stderr, _("%s: could not parse ACL list (%s) for database \"%s\"\n"),
					progname, dbacl, fdbname);
			PQfinish(conn);
			exit(1);
		}

		printf("%s", buf->data);
		destroyPQExpBuffer(buf);
		free(fdbname);

		if (server_version >= 70300)
			dumpDatabaseConfig(conn, dbname);
	}

	PQclear(res);
	printf("\n\n");
}



/*
 * Dump database-specific configuration
 */
static void
dumpDatabaseConfig(PGconn *conn, const char *dbname)
{
	PQExpBuffer buf = createPQExpBuffer();
	int			count = 1;

	for (;;)
	{
		PGresult   *res;

		printfPQExpBuffer(buf, "SELECT datconfig[%d] FROM pg_database WHERE datname = ", count);
		appendStringLiteral(buf, dbname, true);
		appendPQExpBuffer(buf, ";");

		res = executeQuery(conn, buf->data);
		if (!PQgetisnull(res, 0, 0))
		{
			makeAlterConfigCommand(PQgetvalue(res, 0, 0), "DATABASE", dbname);
			PQclear(res);
			count++;
		}
		else
		{
			PQclear(res);
			break;
		}
	}

	destroyPQExpBuffer(buf);
}



/*
 * Dump user-specific configuration
 */
static void
dumpUserConfig(PGconn *conn, const char *username)
{
	PQExpBuffer buf = createPQExpBuffer();
	int			count = 1;

	for (;;)
	{
		PGresult   *res;

		printfPQExpBuffer(buf, "SELECT useconfig[%d] FROM pg_shadow WHERE usename = ", count);
		appendStringLiteral(buf, username, true);
		appendPQExpBuffer(buf, ";");

		res = executeQuery(conn, buf->data);
		if (!PQgetisnull(res, 0, 0))
		{
			makeAlterConfigCommand(PQgetvalue(res, 0, 0), "USER", username);
			PQclear(res);
			count++;
		}
		else
		{
			PQclear(res);
			break;
		}
	}

	destroyPQExpBuffer(buf);
}



/*
 * Helper function for dumpXXXConfig().
 */
static void
makeAlterConfigCommand(const char *arrayitem, const char *type, const char *name)
{
	char	   *pos;
	char	   *mine;
	PQExpBuffer buf = createPQExpBuffer();

	mine = strdup(arrayitem);
	pos = strchr(mine, '=');
	if (pos == NULL)
		return;

	*pos = 0;
	appendPQExpBuffer(buf, "ALTER %s %s ", type, fmtId(name));
	appendPQExpBuffer(buf, "SET %s TO ", fmtId(mine));

	/*
	 * Some GUC variable names are 'LIST' type and hence must not be
	 * quoted.
	 */
	if (strcasecmp(mine, "DateStyle") == 0
		|| strcasecmp(mine, "search_path") == 0)
		appendPQExpBuffer(buf, "%s", pos + 1);
	else
		appendStringLiteral(buf, pos + 1, false);
	appendPQExpBuffer(buf, ";\n");

	printf("%s", buf->data);
	destroyPQExpBuffer(buf);
	free(mine);
}



/*
 * Dump contents of databases.
 */
static void
dumpDatabases(PGconn *conn)
{
	PGresult   *res;
	int			i;

	if (server_version >= 70100)
		res = executeQuery(conn, "SELECT datname FROM pg_database WHERE datallowconn ORDER BY 1");
	else
		res = executeQuery(conn, "SELECT datname FROM pg_database ORDER BY 1");

	for (i = 0; i < PQntuples(res); i++)
	{
		int			ret;

		char	   *dbname = PQgetvalue(res, i, 0);

		if (verbose)
			fprintf(stderr, _("%s: dumping database \"%s\"...\n"), progname, dbname);

		printf("\\connect %s\n\n", fmtId(dbname));
		ret = runPgDump(dbname);
		if (ret != 0)
		{
			fprintf(stderr, _("%s: pg_dump failed on database \"%s\", exiting\n"), progname, dbname);
			exit(1);
		}
	}

	PQclear(res);
}



/*
 * Run pg_dump on dbname.
 */
static int
runPgDump(const char *dbname)
{
	PQExpBuffer cmd = createPQExpBuffer();
	const char *p;
	int			ret;

	/*
	 * Win32 has to use double-quotes for args, rather than single quotes.
	 * Strangely enough, this is the only place we pass a database name on
	 * the command line, except template1 that doesn't need quoting.
	 */
#ifndef WIN32
	appendPQExpBuffer(cmd, "%s\"%s\" %s -Fp '", SYSTEMQUOTE, pg_dump_bin,
#else
	appendPQExpBuffer(cmd, "%s\"%s\" %s -Fp \"", SYSTEMQUOTE, pg_dump_bin,
#endif
					  pgdumpopts->data);

	/* Shell quoting is not quite like SQL quoting, so can't use fmtId */
	for (p = dbname; *p; p++)
	{
#ifndef WIN32
		if (*p == '\'')
			appendPQExpBuffer(cmd, "'\"'\"'");
#else
		if (*p == '"')
			appendPQExpBuffer(cmd, "\\\"");
#endif
		else
			appendPQExpBufferChar(cmd, *p);
	}

#ifndef WIN32
	appendPQExpBufferChar(cmd, '\'');
#else
	appendPQExpBufferChar(cmd, '"');
#endif

	appendPQExpBuffer(cmd, "%s", SYSTEMQUOTE);

	if (verbose)
		fprintf(stderr, _("%s: running \"%s\"\n"), progname, cmd->data);

	fflush(stdout);
	fflush(stderr);

	ret = system(cmd->data);

	destroyPQExpBuffer(cmd);

	return ret;
}



/*
 * Make a database connection with the given parameters.  An
 * interactive password prompt is automatically issued if required.
 */
static PGconn *
connectDatabase(const char *dbname, const char *pghost, const char *pgport,
				const char *pguser, bool require_password)
{
	PGconn	   *conn;
	char	   *password = NULL;
	bool		need_pass = false;
	const char *remoteversion_str;

	if (require_password)
		password = simple_prompt("Password: ", 100, false);

	/*
	 * Start the connection.  Loop until we have a password if requested
	 * by backend.
	 */
	do
	{
		need_pass = false;
		conn = PQsetdbLogin(pghost, pgport, NULL, NULL, dbname, pguser, password);

		if (!conn)
		{
			fprintf(stderr, _("%s: could not connect to database \"%s\"\n"),
					progname, dbname);
			exit(1);
		}

		if (PQstatus(conn) == CONNECTION_BAD &&
			strcmp(PQerrorMessage(conn), PQnoPasswordSupplied) == 0 &&
			!feof(stdin))
		{
			PQfinish(conn);
			need_pass = true;
			free(password);
			password = NULL;
			password = simple_prompt("Password: ", 100, false);
		}
	} while (need_pass);

	if (password)
		free(password);

	/* check to see that the backend connection was successfully made */
	if (PQstatus(conn) == CONNECTION_BAD)
	{
		fprintf(stderr, _("%s: could not connect to database \"%s\": %s\n"),
				progname, dbname, PQerrorMessage(conn));
		exit(1);
	}

	remoteversion_str = PQparameterStatus(conn, "server_version");
	if (!remoteversion_str)
	{
		fprintf(stderr, _("%s: could not get server version\n"), progname);
		exit(1);
	}
	server_version = parse_version(remoteversion_str);
	if (server_version < 0)
	{
		fprintf(stderr, _("%s: could not parse server version \"%s\"\n"),
				progname, remoteversion_str);
		exit(1);
	}

	return conn;
}



/*
 * Run a query, return the results, exit program on failure.
 */
static PGresult *
executeQuery(PGconn *conn, const char *query)
{
	PGresult   *res;

	if (verbose)
		fprintf(stderr, _("%s: executing %s\n"), progname, query);

	res = PQexec(conn, query);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, _("%s: query failed: %s"), progname, PQerrorMessage(conn));
		fprintf(stderr, _("%s: query was: %s\n"), progname, query);
		PQfinish(conn);
		exit(1);
	}

	return res;
}


/*
 * dumpTimestamp
 */
static void
dumpTimestamp(char *msg)
{
	char		buf[256];
	time_t		now = time(NULL);

	if (strftime(buf, 256, "%Y-%m-%d %H:%M:%S %Z", localtime(&now)) != 0)
		printf("-- %s %s\n\n", msg, buf);
}
