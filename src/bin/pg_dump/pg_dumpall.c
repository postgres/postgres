/*-------------------------------------------------------------------------
 *
 * pg_dumpall.c
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * $PostgreSQL: pgsql/src/bin/pg_dump/pg_dumpall.c,v 1.85.2.1 2007/05/15 20:20:24 alvherre Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <time.h>
#include <unistd.h>

#ifdef ENABLE_NLS
#include <locale.h>
#endif

#include "getopt_long.h"

#ifndef HAVE_INT_OPTRESET
int			optreset;
#endif

#include "dumputils.h"
#include "mb/pg_wchar.h"


/* version string we expect back from pg_dump */
#define PGDUMP_VERSIONSTR "pg_dump (PostgreSQL) " PG_VERSION "\n"


static const char *progname;

static void help(void);

static void dumpRoles(PGconn *conn);
static void dumpRoleMembership(PGconn *conn);
static void dumpGroups(PGconn *conn);
static void dumpTablespaces(PGconn *conn);
static void dumpCreateDB(PGconn *conn);
static void dumpDatabaseConfig(PGconn *conn, const char *dbname);
static void dumpUserConfig(PGconn *conn, const char *username);
static void makeAlterConfigCommand(PGconn *conn, const char *arrayitem,
					   const char *type, const char *name);
static void dumpDatabases(PGconn *conn);
static void dumpTimestamp(char *msg);

static int	runPgDump(const char *dbname);
static PGconn *connectDatabase(const char *dbname, const char *pghost, const char *pgport,
			  const char *pguser, bool require_password, bool fail_on_error);
static PGresult *executeQuery(PGconn *conn, const char *query);
static void executeCommand(PGconn *conn, const char *query);

static char pg_dump_bin[MAXPGPATH];
static PQExpBuffer pgdumpopts;
static bool output_clean = false;
static bool skip_acls = false;
static bool verbose = false;
static bool ignoreVersion = false;

static int	disable_dollar_quoting = 0;
static int	disable_triggers = 0;
static int	use_setsessauth = 0;
static int	server_version;


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
	int			encoding;
	const char *std_strings;
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
		 * the following options don't have an equivalent short option letter
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

	if ((ret = find_other_exec(argv[0], "pg_dump", PGDUMP_VERSIONSTR,
							   pg_dump_bin)) < 0)
	{
		char		full_path[MAXPGPATH];

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
				ignoreVersion = true;
				appendPQExpBuffer(pgdumpopts, " -i");
				break;

			case 'o':
				appendPQExpBuffer(pgdumpopts, " -o");
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
				/* -X is a deprecated alternative to long options */
				if (strcmp(optarg, "disable-dollar-quoting") == 0)
					appendPQExpBuffer(pgdumpopts, " --disable-dollar-quoting");
				else if (strcmp(optarg, "disable-triggers") == 0)
					appendPQExpBuffer(pgdumpopts, " --disable-triggers");
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
		appendPQExpBuffer(pgdumpopts, " --disable-dollar-quoting");
	if (disable_triggers)
		appendPQExpBuffer(pgdumpopts, " --disable-triggers");
	if (use_setsessauth)
		appendPQExpBuffer(pgdumpopts, " --use-set-session-authorization");

	if (optind < argc)
	{
		fprintf(stderr, _("%s: too many command-line arguments (first is \"%s\")\n"),
				progname, argv[optind]);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	/*
	 * First try to connect to database "postgres", and failing that
	 * "template1".  "postgres" is the preferred choice for 8.1 and later
	 * servers, but it usually will not exist on older ones.
	 */
	conn = connectDatabase("postgres", pghost, pgport, pguser,
						   force_password, false);
	if (!conn)
		conn = connectDatabase("template1", pghost, pgport, pguser,
							   force_password, true);

	/*
	 * Get the active encoding and the standard_conforming_strings setting, so
	 * we know how to escape strings.
	 */
	encoding = PQclientEncoding(conn);
	std_strings = PQparameterStatus(conn, "standard_conforming_strings");
	if (!std_strings)
		std_strings = "off";

	printf("--\n-- PostgreSQL database cluster dump\n--\n\n");
	if (verbose)
		dumpTimestamp("Started on");

	printf("\\connect postgres\n\n");

	if (!data_only)
	{
		/* Replicate encoding and std_strings in output */
		printf("SET client_encoding = '%s';\n",
			   pg_encoding_to_char(encoding));
		printf("SET standard_conforming_strings = %s;\n", std_strings);
		if (strcmp(std_strings, "off") == 0)
			printf("SET escape_string_warning = 'off';\n");
		printf("\n");

		/* Dump roles (users) */
		dumpRoles(conn);

		/* Dump role memberships --- need different method for pre-8.1 */
		if (server_version >= 80100)
			dumpRoleMembership(conn);
		else
			dumpGroups(conn);

		/* Dump tablespaces */
		if (server_version >= 80000)
			dumpTablespaces(conn);

		/* Dump CREATE DATABASE commands */
		if (!globals_only)
			dumpCreateDB(conn);
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
	printf(_("  --disable-dollar-quoting\n"
			 "                           disable dollar quoting, use SQL standard quoting\n"));
	printf(_("  --disable-triggers       disable triggers during data-only restore\n"));
	printf(_("  --use-set-session-authorization\n"
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
 * Dump roles
 */
static void
dumpRoles(PGconn *conn)
{
	PQExpBuffer buf = createPQExpBuffer();
	PGresult   *res;
	int			i_rolname,
				i_rolsuper,
				i_rolinherit,
				i_rolcreaterole,
				i_rolcreatedb,
				i_rolcatupdate,
				i_rolcanlogin,
				i_rolconnlimit,
				i_rolpassword,
				i_rolvaliduntil,
				i_rolcomment;
	int			i;

	/* note: rolconfig is dumped later */
	if (server_version >= 80200)
		printfPQExpBuffer(buf,
						  "SELECT rolname, rolsuper, rolinherit, "
						  "rolcreaterole, rolcreatedb, rolcatupdate, "
						  "rolcanlogin, rolconnlimit, rolpassword, "
						  "rolvaliduntil, "
			  "pg_catalog.shobj_description(oid, 'pg_authid') as rolcomment "
						  "FROM pg_authid "
						  "ORDER BY 1");
	else if (server_version >= 80100)
		printfPQExpBuffer(buf,
						  "SELECT rolname, rolsuper, rolinherit, "
						  "rolcreaterole, rolcreatedb, rolcatupdate, "
						  "rolcanlogin, rolconnlimit, rolpassword, "
						  "rolvaliduntil, null as rolcomment "
						  "FROM pg_authid "
						  "ORDER BY 1");
	else
		printfPQExpBuffer(buf,
						  "SELECT usename as rolname, "
						  "usesuper as rolsuper, "
						  "true as rolinherit, "
						  "usesuper as rolcreaterole, "
						  "usecreatedb as rolcreatedb, "
						  "usecatupd as rolcatupdate, "
						  "true as rolcanlogin, "
						  "-1 as rolconnlimit, "
						  "passwd as rolpassword, "
						  "valuntil as rolvaliduntil, "
						  "null as rolcomment "
						  "FROM pg_shadow "
						  "UNION ALL "
						  "SELECT groname as rolname, "
						  "false as rolsuper, "
						  "true as rolinherit, "
						  "false as rolcreaterole, "
						  "false as rolcreatedb, "
						  "false as rolcatupdate, "
						  "false as rolcanlogin, "
						  "-1 as rolconnlimit, "
						  "null::text as rolpassword, "
						  "null::abstime as rolvaliduntil, "
						  "null as rolcomment "
						  "FROM pg_group "
						  "WHERE NOT EXISTS (SELECT 1 FROM pg_shadow "
						  " WHERE usename = groname) "
						  "ORDER BY 1");

	res = executeQuery(conn, buf->data);

	i_rolname = PQfnumber(res, "rolname");
	i_rolsuper = PQfnumber(res, "rolsuper");
	i_rolinherit = PQfnumber(res, "rolinherit");
	i_rolcreaterole = PQfnumber(res, "rolcreaterole");
	i_rolcreatedb = PQfnumber(res, "rolcreatedb");
	i_rolcatupdate = PQfnumber(res, "rolcatupdate");
	i_rolcanlogin = PQfnumber(res, "rolcanlogin");
	i_rolconnlimit = PQfnumber(res, "rolconnlimit");
	i_rolpassword = PQfnumber(res, "rolpassword");
	i_rolvaliduntil = PQfnumber(res, "rolvaliduntil");
	i_rolcomment = PQfnumber(res, "rolcomment");

	if (PQntuples(res) > 0)
		printf("--\n-- Roles\n--\n\n");

	for (i = 0; i < PQntuples(res); i++)
	{
		const char *rolename;

		rolename = PQgetvalue(res, i, i_rolname);

		resetPQExpBuffer(buf);

		if (output_clean)
			appendPQExpBuffer(buf, "DROP ROLE %s;\n", fmtId(rolename));

		/*
		 * We dump CREATE ROLE followed by ALTER ROLE to ensure that the role
		 * will acquire the right properties even if it already exists. (The
		 * above DROP may therefore seem redundant, but it isn't really,
		 * because this technique doesn't get rid of role memberships.)
		 */
		appendPQExpBuffer(buf, "CREATE ROLE %s;\n", fmtId(rolename));
		appendPQExpBuffer(buf, "ALTER ROLE %s WITH", fmtId(rolename));

		if (strcmp(PQgetvalue(res, i, i_rolsuper), "t") == 0)
			appendPQExpBuffer(buf, " SUPERUSER");
		else
			appendPQExpBuffer(buf, " NOSUPERUSER");

		if (strcmp(PQgetvalue(res, i, i_rolinherit), "t") == 0)
			appendPQExpBuffer(buf, " INHERIT");
		else
			appendPQExpBuffer(buf, " NOINHERIT");

		if (strcmp(PQgetvalue(res, i, i_rolcreaterole), "t") == 0)
			appendPQExpBuffer(buf, " CREATEROLE");
		else
			appendPQExpBuffer(buf, " NOCREATEROLE");

		if (strcmp(PQgetvalue(res, i, i_rolcreatedb), "t") == 0)
			appendPQExpBuffer(buf, " CREATEDB");
		else
			appendPQExpBuffer(buf, " NOCREATEDB");

		if (strcmp(PQgetvalue(res, i, i_rolcanlogin), "t") == 0)
			appendPQExpBuffer(buf, " LOGIN");
		else
			appendPQExpBuffer(buf, " NOLOGIN");

		if (strcmp(PQgetvalue(res, i, i_rolconnlimit), "-1") != 0)
			appendPQExpBuffer(buf, " CONNECTION LIMIT %s",
							  PQgetvalue(res, i, i_rolconnlimit));

		if (!PQgetisnull(res, i, i_rolpassword))
		{
			appendPQExpBuffer(buf, " PASSWORD ");
			appendStringLiteralConn(buf, PQgetvalue(res, i, i_rolpassword), conn);
		}

		if (!PQgetisnull(res, i, i_rolvaliduntil))
			appendPQExpBuffer(buf, " VALID UNTIL '%s'",
							  PQgetvalue(res, i, i_rolvaliduntil));

		appendPQExpBuffer(buf, ";\n");

		if (!PQgetisnull(res, i, i_rolcomment))
		{
			appendPQExpBuffer(buf, "COMMENT ON ROLE %s IS ", fmtId(rolename));
			appendStringLiteralConn(buf, PQgetvalue(res, i, i_rolcomment), conn);
			appendPQExpBuffer(buf, ";\n");
		}

		printf("%s", buf->data);

		if (server_version >= 70300)
			dumpUserConfig(conn, rolename);
	}

	PQclear(res);

	printf("\n\n");

	destroyPQExpBuffer(buf);
}


/*
 * Dump role memberships.  This code is used for 8.1 and later servers.
 *
 * Note: we expect dumpRoles already created all the roles, but there is
 * no membership yet.
 */
static void
dumpRoleMembership(PGconn *conn)
{
	PGresult   *res;
	int			i;

	res = executeQuery(conn, "SELECT ur.rolname AS roleid, "
					   "um.rolname AS member, "
					   "a.admin_option, "
					   "ug.rolname AS grantor "
					   "FROM pg_auth_members a "
					   "LEFT JOIN pg_authid ur on ur.oid = a.roleid "
					   "LEFT JOIN pg_authid um on um.oid = a.member "
					   "LEFT JOIN pg_authid ug on ug.oid = a.grantor "
					   "ORDER BY 1,2,3");

	if (PQntuples(res) > 0)
		printf("--\n-- Role memberships\n--\n\n");

	for (i = 0; i < PQntuples(res); i++)
	{
		char	   *roleid = PQgetvalue(res, i, 0);
		char	   *member = PQgetvalue(res, i, 1);
		char	   *option = PQgetvalue(res, i, 2);

		printf("GRANT %s", fmtId(roleid));
		printf(" TO %s", fmtId(member));
		if (*option == 't')
			printf(" WITH ADMIN OPTION");

		/*
		 * We don't track the grantor very carefully in the backend, so cope
		 * with the possibility that it has been dropped.
		 */
		if (!PQgetisnull(res, i, 3))
		{
			char	*grantor = PQgetvalue(res, i, 3);

			printf(" GRANTED BY %s", fmtId(grantor));
		}
		printf(";\n");
	}

	PQclear(res);

	printf("\n\n");
}

/*
 * Dump group memberships from a pre-8.1 server.  It's annoying that we
 * can't share any useful amount of code with the post-8.1 case, but
 * the catalog representations are too different.
 *
 * Note: we expect dumpRoles already created all the roles, but there is
 * no membership yet.
 */
static void
dumpGroups(PGconn *conn)
{
	PQExpBuffer buf = createPQExpBuffer();
	PGresult   *res;
	int			i;

	res = executeQuery(conn,
					   "SELECT groname, grolist FROM pg_group ORDER BY 1");

	if (PQntuples(res) > 0)
		printf("--\n-- Role memberships\n--\n\n");

	for (i = 0; i < PQntuples(res); i++)
	{
		char	   *groname = PQgetvalue(res, i, 0);
		char	   *grolist = PQgetvalue(res, i, 1);
		PGresult   *res2;
		int			j;

		/*
		 * Array representation is {1,2,3} ... convert to (1,2,3)
		 */
		if (strlen(grolist) < 3)
			continue;

		grolist = strdup(grolist);
		grolist[0] = '(';
		grolist[strlen(grolist) - 1] = ')';
		printfPQExpBuffer(buf,
						  "SELECT usename FROM pg_shadow "
						  "WHERE usesysid IN %s ORDER BY 1",
						  grolist);
		free(grolist);

		res2 = executeQuery(conn, buf->data);

		for (j = 0; j < PQntuples(res2); j++)
		{
			char	   *usename = PQgetvalue(res2, j, 0);

			/*
			 * Don't try to grant a role to itself; can happen if old
			 * installation has identically named user and group.
			 */
			if (strcmp(groname, usename) == 0)
				continue;

			printf("GRANT %s", fmtId(groname));
			printf(" TO %s;\n", fmtId(usename));
		}

		PQclear(res2);
	}

	PQclear(res);
	destroyPQExpBuffer(buf);

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
	if (server_version >= 80200)
		res = executeQuery(conn, "SELECT spcname, "
						 "pg_catalog.pg_get_userbyid(spcowner) AS spcowner, "
						   "spclocation, spcacl, "
						"pg_catalog.shobj_description(oid, 'pg_tablespace') "
						   "FROM pg_catalog.pg_tablespace "
						   "WHERE spcname !~ '^pg_' "
						   "ORDER BY 1");
	else
		res = executeQuery(conn, "SELECT spcname, "
						 "pg_catalog.pg_get_userbyid(spcowner) AS spcowner, "
						   "spclocation, spcacl, "
						   "null "
						   "FROM pg_catalog.pg_tablespace "
						   "WHERE spcname !~ '^pg_' "
						   "ORDER BY 1");

	if (PQntuples(res) > 0)
		printf("--\n-- Tablespaces\n--\n\n");

	for (i = 0; i < PQntuples(res); i++)
	{
		PQExpBuffer buf = createPQExpBuffer();
		char	   *spcname = PQgetvalue(res, i, 0);
		char	   *spcowner = PQgetvalue(res, i, 1);
		char	   *spclocation = PQgetvalue(res, i, 2);
		char	   *spcacl = PQgetvalue(res, i, 3);
		char	   *spccomment = PQgetvalue(res, i, 4);
		char	   *fspcname;

		/* needed for buildACLCommands() */
		fspcname = strdup(fmtId(spcname));

		if (output_clean)
			appendPQExpBuffer(buf, "DROP TABLESPACE %s;\n", fspcname);

		appendPQExpBuffer(buf, "CREATE TABLESPACE %s", fspcname);
		appendPQExpBuffer(buf, " OWNER %s", fmtId(spcowner));

		appendPQExpBuffer(buf, " LOCATION ");
		appendStringLiteralConn(buf, spclocation, conn);
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

		if (spccomment && strlen(spccomment))
		{
			appendPQExpBuffer(buf, "COMMENT ON TABLESPACE %s IS ", fspcname);
			appendStringLiteralConn(buf, spccomment, conn);
			appendPQExpBuffer(buf, ";\n");
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
	PQExpBuffer buf = createPQExpBuffer();
	PGresult   *res;
	int			i;

	printf("--\n-- Database creation\n--\n\n");

	if (server_version >= 80100)
		res = executeQuery(conn,
						   "SELECT datname, "
						   "coalesce(rolname, (select rolname from pg_authid where oid=(select datdba from pg_database where datname='template0'))), "
						   "pg_encoding_to_char(d.encoding), "
						   "datistemplate, datacl, datconnlimit, "
						   "(SELECT spcname FROM pg_tablespace t WHERE t.oid = d.dattablespace) AS dattablespace "
			  "FROM pg_database d LEFT JOIN pg_authid u ON (datdba = u.oid) "
						   "WHERE datallowconn ORDER BY 1");
	else if (server_version >= 80000)
		res = executeQuery(conn,
						   "SELECT datname, "
						   "coalesce(usename, (select usename from pg_shadow where usesysid=(select datdba from pg_database where datname='template0'))), "
						   "pg_encoding_to_char(d.encoding), "
						   "datistemplate, datacl, -1 as datconnlimit, "
						   "(SELECT spcname FROM pg_tablespace t WHERE t.oid = d.dattablespace) AS dattablespace "
		   "FROM pg_database d LEFT JOIN pg_shadow u ON (datdba = usesysid) "
						   "WHERE datallowconn ORDER BY 1");
	else if (server_version >= 70300)
		res = executeQuery(conn,
						   "SELECT datname, "
						   "coalesce(usename, (select usename from pg_shadow where usesysid=(select datdba from pg_database where datname='template0'))), "
						   "pg_encoding_to_char(d.encoding), "
						   "datistemplate, datacl, -1 as datconnlimit, "
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
						   "datistemplate, '' as datacl, -1 as datconnlimit, "
						   "'pg_default' AS dattablespace "
						   "FROM pg_database d "
						   "WHERE datallowconn ORDER BY 1");
	else
	{
		/*
		 * Note: 7.0 fails to cope with sub-select in COALESCE, so just deal
		 * with getting a NULL by not printing any OWNER clause.
		 */
		res = executeQuery(conn,
						   "SELECT datname, "
					"(select usename from pg_shadow where usesysid=datdba), "
						   "pg_encoding_to_char(d.encoding), "
						   "'f' as datistemplate, "
						   "'' as datacl, -1 as datconnlimit, "
						   "'pg_default' AS dattablespace "
						   "FROM pg_database d "
						   "ORDER BY 1");
	}

	for (i = 0; i < PQntuples(res); i++)
	{
		char	   *dbname = PQgetvalue(res, i, 0);
		char	   *dbowner = PQgetvalue(res, i, 1);
		char	   *dbencoding = PQgetvalue(res, i, 2);
		char	   *dbistemplate = PQgetvalue(res, i, 3);
		char	   *dbacl = PQgetvalue(res, i, 4);
		char	   *dbconnlimit = PQgetvalue(res, i, 5);
		char	   *dbtablespace = PQgetvalue(res, i, 6);
		char	   *fdbname;

		fdbname = strdup(fmtId(dbname));

		resetPQExpBuffer(buf);

		/*
		 * Skip the CREATE DATABASE commands for "template1" and "postgres",
		 * since they are presumably already there in the destination cluster.
		 * We do want to emit their ACLs and config options if any, however.
		 */
		if (strcmp(dbname, "template1") != 0 &&
			strcmp(dbname, "postgres") != 0)
		{
			if (output_clean)
				appendPQExpBuffer(buf, "DROP DATABASE %s;\n", fdbname);

			appendPQExpBuffer(buf, "CREATE DATABASE %s", fdbname);

			appendPQExpBuffer(buf, " WITH TEMPLATE = template0");

			if (strlen(dbowner) != 0)
				appendPQExpBuffer(buf, " OWNER = %s", fmtId(dbowner));

			appendPQExpBuffer(buf, " ENCODING = ");
			appendStringLiteralConn(buf, dbencoding, conn);

			/*
			 * Output tablespace if it isn't the default.  For default, it
			 * uses the default from the template database.  If tablespace is
			 * specified and tablespace creation failed earlier, (e.g. no such
			 * directory), the database creation will fail too.  One solution
			 * would be to use 'SET default_tablespace' like we do in pg_dump
			 * for setting non-default database locations.
			 */
			if (strcmp(dbtablespace, "pg_default") != 0)
				appendPQExpBuffer(buf, " TABLESPACE = %s",
								  fmtId(dbtablespace));

			if (strcmp(dbconnlimit, "-1") != 0)
				appendPQExpBuffer(buf, " CONNECTION LIMIT = %s",
								  dbconnlimit);

			appendPQExpBuffer(buf, ";\n");

			if (strcmp(dbistemplate, "t") == 0)
			{
				appendPQExpBuffer(buf, "UPDATE pg_database SET datistemplate = 't' WHERE datname = ");
				appendStringLiteralConn(buf, dbname, conn);
				appendPQExpBuffer(buf, ";\n");
			}
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

		if (server_version >= 70300)
			dumpDatabaseConfig(conn, dbname);

		free(fdbname);
	}

	PQclear(res);
	destroyPQExpBuffer(buf);

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
		appendStringLiteralConn(buf, dbname, conn);
		appendPQExpBuffer(buf, ";");

		res = executeQuery(conn, buf->data);
		if (!PQgetisnull(res, 0, 0))
		{
			makeAlterConfigCommand(conn, PQgetvalue(res, 0, 0),
								   "DATABASE", dbname);
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

		if (server_version >= 80100)
			printfPQExpBuffer(buf, "SELECT rolconfig[%d] FROM pg_authid WHERE rolname = ", count);
		else
			printfPQExpBuffer(buf, "SELECT useconfig[%d] FROM pg_shadow WHERE usename = ", count);
		appendStringLiteralConn(buf, username, conn);

		res = executeQuery(conn, buf->data);
		if (PQntuples(res) == 1 &&
			!PQgetisnull(res, 0, 0))
		{
			makeAlterConfigCommand(conn, PQgetvalue(res, 0, 0),
								   "ROLE", username);
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
makeAlterConfigCommand(PGconn *conn, const char *arrayitem,
					   const char *type, const char *name)
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
	 * Some GUC variable names are 'LIST' type and hence must not be quoted.
	 */
	if (pg_strcasecmp(mine, "DateStyle") == 0
		|| pg_strcasecmp(mine, "search_path") == 0)
		appendPQExpBuffer(buf, "%s", pos + 1);
	else
		appendStringLiteralConn(buf, pos + 1, conn);
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
	 * Strangely enough, this is the only place we pass a database name on the
	 * command line, except "postgres" which doesn't need quoting.
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
 *
 * If fail_on_error is false, we return NULL without printing any message
 * on failure, but preserve any prompted password for the next try.
 */
static PGconn *
connectDatabase(const char *dbname, const char *pghost, const char *pgport,
				const char *pguser, bool require_password, bool fail_on_error)
{
	PGconn	   *conn;
	bool		need_pass = false;
	const char *remoteversion_str;
	int			my_version;
	static char *password = NULL;

	if (require_password && !password)
		password = simple_prompt("Password: ", 100, false);

	/*
	 * Start the connection.  Loop until we have a password if requested by
	 * backend.
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
			if (password)
				free(password);
			password = NULL;
			password = simple_prompt("Password: ", 100, false);
		}
	} while (need_pass);

	/* check to see that the backend connection was successfully made */
	if (PQstatus(conn) == CONNECTION_BAD)
	{
		if (fail_on_error)
		{
			fprintf(stderr,
					_("%s: could not connect to database \"%s\": %s\n"),
					progname, dbname, PQerrorMessage(conn));
			exit(1);
		}
		else
		{
			PQfinish(conn);
			return NULL;
		}
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

	my_version = parse_version(PG_VERSION);
	if (my_version < 0)
	{
		fprintf(stderr, _("%s: could not parse version \"%s\"\n"),
				progname, PG_VERSION);
		exit(1);
	}

	if (my_version != server_version
		&& (server_version < 70000		/* we can handle back to 7.0 */
			|| server_version > my_version))
	{
		fprintf(stderr, _("server version: %s; %s version: %s\n"),
				remoteversion_str, progname, PG_VERSION);
		if (ignoreVersion)
			fprintf(stderr, _("proceeding despite version mismatch\n"));
		else
		{
			fprintf(stderr, _("aborting because of version mismatch  (Use the -i option to proceed anyway.)\n"));
			exit(1);
		}
	}

	/*
	 * On 7.3 and later, make sure we are not fooled by non-system schemas in
	 * the search path.
	 */
	if (server_version >= 70300)
		executeCommand(conn, "SET search_path = pg_catalog");

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
		fprintf(stderr, _("%s: query failed: %s"),
				progname, PQerrorMessage(conn));
		fprintf(stderr, _("%s: query was: %s\n"),
				progname, query);
		PQfinish(conn);
		exit(1);
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

	if (verbose)
		fprintf(stderr, _("%s: executing %s\n"), progname, query);

	res = PQexec(conn, query);
	if (!res ||
		PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, _("%s: query failed: %s"),
				progname, PQerrorMessage(conn));
		fprintf(stderr, _("%s: query was: %s\n"),
				progname, query);
		PQfinish(conn);
		exit(1);
	}

	PQclear(res);
}


/*
 * dumpTimestamp
 */
static void
dumpTimestamp(char *msg)
{
	char		buf[256];
	time_t		now = time(NULL);

	/*
	 * We don't print the timezone on Win32, because the names are long and
	 * localized, which means they may contain characters in various random
	 * encodings; this has been seen to cause encoding errors when reading
	 * the dump script.
	 */
	if (strftime(buf, sizeof(buf),
#ifndef WIN32
				 "%Y-%m-%d %H:%M:%S %Z",
#else
				 "%Y-%m-%d %H:%M:%S",
#endif
				 localtime(&now)) != 0)
		printf("-- %s %s\n\n", msg, buf);
}
