/*-------------------------------------------------------------------------
 *
 * pg_dump.c
 *	  pg_dump is a utility for dumping out a postgres database
 *	  into a script file.
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	pg_dump will read the system catalogs in a database and
 *	dump out a script that reproduces
 *	the schema of the database in terms of
 *		  user-defined types
 *		  user-defined functions
 *		  tables
 *		  indexes
 *		  aggregates
 *		  operators
 *		  privileges
 *
 * the output script is SQL that is understood by PostgreSQL
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/bin/pg_dump/pg_dump.c,v 1.269 2002/07/04 03:04:54 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

/*
 * Although this is not a backend module, we must include postgres.h anyway
 * so that we can include a bunch of backend include files.  pg_dump has
 * never pretended to be very independent of the backend anyhow ...
 */
#include "postgres.h"

#include <unistd.h>				/* for getopt() */
#include <ctype.h>
#ifdef ENABLE_NLS
#include <locale.h>
#endif
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif
#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif

#ifndef HAVE_STRDUP
#include "strdup.h"
#endif

#include "access/attnum.h"
#include "access/htup.h"
#include "catalog/pg_class.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"

#include "libpq-fe.h"
#include "libpq/libpq-fs.h"

#include "pg_dump.h"
#include "pg_backup.h"
#include "pg_backup_archiver.h"


typedef enum _formatLiteralOptions
{
	CONV_ALL = 0,
	PASS_LFTAB = 3				/* NOTE: 1 and 2 are reserved in case we
								 * want to make a mask. */
	/* We could make this a bit mask for control chars, but I don't */
	/* see any value in making it more complex...the current code */
	/* only checks for 'opts == CONV_ALL' anyway. */
} formatLiteralOptions;

typedef struct _dumpContext
{
	TableInfo  *tblinfo;
	int			tblidx;
	bool		oids;
} DumpContext;

static void help(const char *progname);
static int	parse_version(const char *versionString);
static NamespaceInfo *findNamespace(const char *nsoid, const char *objoid);
static void dumpClasses(const TableInfo *tblinfo, const int numTables,
						Archive *fout, const bool oids);
static void dumpComment(Archive *fout, const char *target,
						const char *namespace, const char *owner,
						const char *oid, const char *classname, int subid,
						const char *((*deps)[]));
static void dumpOneBaseType(Archive *fout, TypeInfo *tinfo,
							FuncInfo *g_finfo, int numFuncs,
							TypeInfo *g_tinfo, int numTypes);
static void dumpOneDomain(Archive *fout, TypeInfo *tinfo);
static void dumpOneTable(Archive *fout, TableInfo *tbinfo,
						 TableInfo *g_tblinfo);
static void dumpOneSequence(Archive *fout, TableInfo *tbinfo,
							const bool schemaOnly, const bool dataOnly);

static void dumpTableACL(Archive *fout, TableInfo *tbinfo);
static void dumpFuncACL(Archive *fout, FuncInfo *finfo);
static void dumpAggACL(Archive *fout, AggInfo *finfo);
static void dumpACL(Archive *fout, const char *type, const char *name,
					const char *name_noquotes, const char *nspname,
					const char *usename, const char *acl, const char *objoid);

static void dumpTriggers(Archive *fout, TableInfo *tblinfo, int numTables);
static void dumpRules(Archive *fout, TableInfo *tblinfo, int numTables);
static void formatStringLiteral(PQExpBuffer buf, const char *str,
								const formatLiteralOptions opts);
static char *format_function_signature(FuncInfo *finfo, bool honor_quotes);
static void dumpOneFunc(Archive *fout, FuncInfo *finfo);
static void dumpOneOpr(Archive *fout, OprInfo *oprinfo,
					   OprInfo *g_oprinfo, int numOperators);
static const char *convertRegProcReference(const char *proc);
static const char *convertOperatorReference(const char *opr,
						OprInfo *g_oprinfo, int numOperators);
static void dumpOneAgg(Archive *fout, AggInfo *agginfo);
static Oid	findLastBuiltinOid_V71(const char *);
static Oid	findLastBuiltinOid_V70(void);
static void setMaxOid(Archive *fout);
static void selectSourceSchema(const char *schemaName);
static char *getFormattedTypeName(const char *oid, OidOptions opts);
static char *myFormatType(const char *typname, int32 typmod);
static const char *fmtQualifiedId(const char *schema, const char *id);

static void AddAcl(char *aclbuf, const char *keyword);
static char *GetPrivileges(Archive *AH, const char *s, const char *type);

static int	dumpBlobs(Archive *AH, char *, void *);
static int	dumpDatabase(Archive *AH);
static const char *getAttrName(int attrnum, TableInfo *tblInfo);

extern char *optarg;
extern int	optind,
			opterr;

/* global decls */
bool		g_verbose;			/* User wants verbose narration of our
								 * activities. */
Archive    *g_fout;				/* the script file */
PGconn	   *g_conn;				/* the database connection */

/* various user-settable parameters */
bool		force_quotes;		/* User wants to suppress double-quotes */
bool		dumpData;			/* dump data using proper insert strings */
bool		attrNames;			/* put attr names into insert strings */
bool		schemaOnly;
bool		dataOnly;
bool		aclsSkip;

/* obsolete as of 7.3: */
static Oid	g_last_builtin_oid; /* value of the last builtin oid */

static char *selectTablename = NULL;	/* name of a single table to dump */

char		g_opaque_type[10];	/* name for the opaque type */

/* placeholders for the delimiters for comments */
char		g_comment_start[10];
char		g_comment_end[10];

/* these are to avoid passing around info for findNamespace() */
static NamespaceInfo *g_namespaces;
static int	g_numNamespaces;


int
main(int argc, char **argv)
{
	int			c;
	const char *filename = NULL;
	const char *format = "p";
	const char *dbname = NULL;
	const char *pghost = NULL;
	const char *pgport = NULL;
	const char *username = NULL;
	bool		oids = false;
	TableInfo  *tblinfo;
	int			numTables;
	bool		force_password = false;
	int			compressLevel = -1;
	bool		ignore_version = false;
	int			plainText = 0;
	int			outputClean = 0;
	int			outputCreate = 0;
	int			outputBlobs = 0;
	int			outputNoOwner = 0;
	int			outputNoReconnect = 0;
	static int	use_setsessauth = 0;
	static int	disable_triggers = 0;
	char	   *outputSuperuser = NULL;

	RestoreOptions *ropt;

#ifdef HAVE_GETOPT_LONG
	static struct option long_options[] = {
		{"data-only", no_argument, NULL, 'a'},
		{"blobs", no_argument, NULL, 'b'},
		{"clean", no_argument, NULL, 'c'},
		{"create", no_argument, NULL, 'C'},
		{"file", required_argument, NULL, 'f'},
		{"format", required_argument, NULL, 'F'},
		{"inserts", no_argument, NULL, 'd'},
		{"attribute-inserts", no_argument, NULL, 'D'},
		{"column-inserts", no_argument, NULL, 'D'},
		{"host", required_argument, NULL, 'h'},
		{"ignore-version", no_argument, NULL, 'i'},
		{"no-reconnect", no_argument, NULL, 'R'},
		{"no-quotes", no_argument, NULL, 'n'},
		{"quotes", no_argument, NULL, 'N'},
		{"oids", no_argument, NULL, 'o'},
		{"no-owner", no_argument, NULL, 'O'},
		{"port", required_argument, NULL, 'p'},
		{"schema-only", no_argument, NULL, 's'},
		{"superuser", required_argument, NULL, 'S'},
		{"table", required_argument, NULL, 't'},
		{"password", no_argument, NULL, 'W'},
		{"username", required_argument, NULL, 'U'},
		{"verbose", no_argument, NULL, 'v'},
		{"no-privileges", no_argument, NULL, 'x'},
		{"no-acl", no_argument, NULL, 'x'},
		{"compress", required_argument, NULL, 'Z'},
		{"help", no_argument, NULL, '?'},
		{"version", no_argument, NULL, 'V'},

		/*
		 * the following options don't have an equivalent short option
		 * letter, but are available as '-X long-name'
		 */
		{"use-set-session-authorization", no_argument, &use_setsessauth, 1},
		{"disable-triggers", no_argument, &disable_triggers, 1},

		{NULL, 0, NULL, 0}
	};
	int			optindex;
#endif

#ifdef ENABLE_NLS
	setlocale(LC_ALL, "");
	bindtextdomain("pg_dump", LOCALEDIR);
	textdomain("pg_dump");
#endif

	g_verbose = false;
	force_quotes = true;

	strcpy(g_comment_start, "-- ");
	g_comment_end[0] = '\0';
	strcpy(g_opaque_type, "opaque");

	dataOnly = schemaOnly = dumpData = attrNames = false;

	if (!strrchr(argv[0], '/'))
		progname = argv[0];
	else
		progname = strrchr(argv[0], '/') + 1;

	/* Set default options based on progname */
	if (strcmp(progname, "pg_backup") == 0)
	{
		format = "c";
		outputBlobs = true;
	}

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			help(progname);
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_dump (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

#ifdef HAVE_GETOPT_LONG
	while ((c = getopt_long(argc, argv, "abcCdDf:F:h:inNoOp:RsS:t:uU:vWxX:zZ:V?", long_options, &optindex)) != -1)
#else
	while ((c = getopt(argc, argv, "abcCdDf:F:h:inNoOp:RsS:t:uU:vWxX:zZ:V?-")) != -1)
#endif

	{
		switch (c)
		{
			case 'a':			/* Dump data only */
				dataOnly = true;
				break;

			case 'b':			/* Dump blobs */
				outputBlobs = true;
				break;

			case 'c':			/* clean (i.e., drop) schema prior to
								 * create */
				outputClean = 1;
				break;

			case 'C':			/* Create DB */

				outputCreate = 1;
				break;

			case 'd':			/* dump data as proper insert strings */
				dumpData = true;
				break;

			case 'D':			/* dump data as proper insert strings with
								 * attr names */
				dumpData = true;
				attrNames = true;
				break;

			case 'f':
				filename = optarg;
				break;

			case 'F':
				format = optarg;
				break;

			case 'h':			/* server host */
				pghost = optarg;
				break;

			case 'i':			/* ignore database version mismatch */
				ignore_version = true;
				break;

			case 'n':			/* Do not force double-quotes on
								 * identifiers */
				force_quotes = false;
				break;

			case 'N':			/* Force double-quotes on identifiers */
				force_quotes = true;
				break;

			case 'o':			/* Dump oids */
				oids = true;
				break;


			case 'O':			/* Don't reconnect to match owner */
				outputNoOwner = 1;
				break;

			case 'p':			/* server port */
				pgport = optarg;
				break;

			case 'R':			/* No reconnect */
				outputNoReconnect = 1;
				break;

			case 's':			/* dump schema only */
				schemaOnly = true;
				break;

			case 'S':			/* Username for superuser in plain text
								 * output */
				outputSuperuser = strdup(optarg);
				break;

			case 't':			/* Dump data for this table only */
				{
					int			i;

					selectTablename = strdup(optarg);

					/*
					 * quoted string? Then strip quotes and preserve
					 * case...
					 */
					if (selectTablename[0] == '"')
					{
						char	*endptr;

						endptr = selectTablename + strlen(selectTablename) - 1;
						if (*endptr == '"')
							*endptr = '\0';
						strcpy(selectTablename, &selectTablename[1]);
					}
					else
					{
						/* otherwise, convert table name to lowercase... */
						for (i = 0; selectTablename[i]; i++)
							if (isupper((unsigned char) selectTablename[i]))
								selectTablename[i] = tolower((unsigned char) selectTablename[i]);

						/*
						 * '*' is a special case meaning ALL tables, but
						 * only if unquoted
						 */
						if (strcmp(selectTablename, "*") == 0)
							selectTablename[0] = '\0';
					}
				}
				break;

			case 'u':
				force_password = true;
				username = simple_prompt("User name: ", 100, true);
				break;

			case 'U':
				username = optarg;
				break;

			case 'v':			/* verbose */
				g_verbose = true;
				break;

			case 'W':
				force_password = true;
				break;

			case 'x':			/* skip ACL dump */
				aclsSkip = true;
				break;

				/*
				 * Option letters were getting scarce, so I invented this
				 * new scheme: '-X feature' turns on some feature. Compare
				 * to the -f option in GCC.  You should also add an
				 * equivalent GNU-style option --feature.  Features that
				 * require arguments should use '-X feature=foo'.
				 */
			case 'X':
				if (strcmp(optarg, "use-set-session-authorization") == 0)
					use_setsessauth = 1;
				else if (strcmp(optarg, "disable-triggers") == 0)
					disable_triggers = 1;
				else
				{
					fprintf(stderr,
							gettext("%s: invalid -X option -- %s\n"),
							progname, optarg);
					fprintf(stderr, gettext("Try '%s --help' for more information.\n"), progname);
					exit(1);
				}
				break;
			case 'Z':			/* Compression Level */
				compressLevel = atoi(optarg);
				break;

#ifndef HAVE_GETOPT_LONG
			case '-':
				fprintf(stderr,
						gettext("%s was compiled without support for long options.\n"
						 "Use --help for help on invocation options.\n"),
						progname);
				exit(1);
				break;
#else
				/* This covers the long options equivalent to -X xxx. */
			case 0:
				break;
#endif
			default:
				fprintf(stderr, gettext("Try '%s --help' for more information.\n"), progname);
				exit(1);
		}
	}

	if (optind < (argc - 1))
	{
		fprintf(stderr,
			gettext("%s: too many command line options (first is '%s')\n"
					"Try '%s --help' for more information.\n"),
				progname, argv[optind + 1], progname);
		exit(1);
	}

	/* Get the target database name */
	if (optind < argc)
		dbname = argv[optind];
	else
		dbname = getenv("PGDATABASE");
	if (!dbname)
	{
		write_msg(NULL, "no database name specified\n");
		exit(1);
	}

	if (dataOnly && schemaOnly)
	{
		write_msg(NULL, "The options \"schema only\" (-s) and \"data only\" (-a) cannot be used together.\n");
		exit(1);
	}

	if (outputBlobs && selectTablename != NULL && strlen(selectTablename) > 0)
	{
		write_msg(NULL, "Large object output is not supported for a single table.\n");
		write_msg(NULL, "Use all tables or a full dump instead.\n");
		exit(1);
	}

	if (dumpData == true && oids == true)
	{
		write_msg(NULL, "INSERT (-d, -D) and OID (-o) options cannot be used together.\n");
		write_msg(NULL, "(The INSERT command cannot set oids.)\n");
		exit(1);
	}

	if (outputBlobs == true && (format[0] == 'p' || format[0] == 'P'))
	{
		write_msg(NULL, "large object output is not supported for plain text dump files.\n");
		write_msg(NULL, "(Use a different output format.)\n");
		exit(1);
	}

	/* open the output file */
	switch (format[0])
	{

		case 'c':
		case 'C':
			g_fout = CreateArchive(filename, archCustom, compressLevel);
			break;

		case 'f':
		case 'F':
			g_fout = CreateArchive(filename, archFiles, compressLevel);
			break;

		case 'p':
		case 'P':
			plainText = 1;
			g_fout = CreateArchive(filename, archNull, 0);
			break;

		case 't':
		case 'T':
			g_fout = CreateArchive(filename, archTar, compressLevel);
			break;

		default:
			write_msg(NULL, "invalid output format '%s' specified\n", format);
			exit(1);
	}

	if (g_fout == NULL)
	{
		write_msg(NULL, "could not open output file %s for writing\n", filename);
		exit(1);
	}

	/* Let the archiver know how noisy to be */
	g_fout->verbose = g_verbose;

	/*
	 * Open the database using the Archiver, so it knows about it. Errors
	 * mean death.
	 */
	g_fout->minRemoteVersion = 70000;	/* we can handle back to 7.0 */
	g_fout->maxRemoteVersion = parse_version(PG_VERSION);
	g_conn = ConnectDatabase(g_fout, dbname, pghost, pgport, username, force_password, ignore_version);

	/*
	 * Start serializable transaction to dump consistent data
	 */
	{
		PGresult   *res;

		res = PQexec(g_conn, "begin");
		if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
			exit_horribly(g_fout, NULL, "BEGIN command failed: %s",
						  PQerrorMessage(g_conn));

		PQclear(res);
		res = PQexec(g_conn, "set transaction isolation level serializable");
		if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
			exit_horribly(g_fout, NULL, "could not set transaction isolation level to serializable: %s",
						  PQerrorMessage(g_conn));

		PQclear(res);
	}

	if (g_fout->remoteVersion < 70300)
	{
		if (g_fout->remoteVersion >= 70100)
			g_last_builtin_oid = findLastBuiltinOid_V71(dbname);
		else
			g_last_builtin_oid = findLastBuiltinOid_V70();
		if (g_verbose)
			write_msg(NULL, "last built-in oid is %u\n", g_last_builtin_oid);
	}

	/* Dump the database definition */
	if (!dataOnly)
		dumpDatabase(g_fout);

	if (oids == true)
		setMaxOid(g_fout);

	tblinfo = dumpSchema(g_fout, &numTables, aclsSkip, schemaOnly, dataOnly);

	if (!schemaOnly)
		dumpClasses(tblinfo, numTables, g_fout, oids);

	if (outputBlobs)
		ArchiveEntry(g_fout, "0", "BLOBS", NULL, "",
					 "BLOBS", NULL, "", "", NULL, dumpBlobs, NULL);

	if (!dataOnly)				/* dump indexes and triggers at the end
								 * for performance */
	{
		dumpTriggers(g_fout, tblinfo, numTables);
		dumpRules(g_fout, tblinfo, numTables);
	}

	/* Now sort the output nicely */
	SortTocByOID(g_fout);
	MoveToStart(g_fout, "SCHEMA");
	MoveToStart(g_fout, "DATABASE");
	MoveToEnd(g_fout, "TABLE DATA");
	MoveToEnd(g_fout, "BLOBS");
	MoveToEnd(g_fout, "INDEX");
	MoveToEnd(g_fout, "CONSTRAINT");
	MoveToEnd(g_fout, "TRIGGER");
	MoveToEnd(g_fout, "RULE");
	MoveToEnd(g_fout, "SEQUENCE SET");

	/*
	 * Moving all comments to end is annoying, but must do it for comments
	 * on stuff we just moved, and we don't seem to have quite enough
	 * dependency structure to get it really right...
	 */
	MoveToEnd(g_fout, "COMMENT");

	if (plainText)
	{
		ropt = NewRestoreOptions();
		ropt->filename = (char *) filename;
		ropt->dropSchema = outputClean;
		ropt->aclsSkip = aclsSkip;
		ropt->superuser = outputSuperuser;
		ropt->create = outputCreate;
		ropt->noOwner = outputNoOwner;
		ropt->noReconnect = outputNoReconnect;
		ropt->use_setsessauth = use_setsessauth;
		ropt->disable_triggers = disable_triggers;

		if (compressLevel == -1)
			ropt->compression = 0;
		else
			ropt->compression = compressLevel;

		ropt->suppressDumpWarnings = true;		/* We've already shown
												 * them */

		RestoreArchive(g_fout, ropt);
	}

	CloseArchive(g_fout);

	PQfinish(g_conn);
	exit(0);
}


static void
help(const char *progname)
{
	printf(gettext("%s dumps a database as a text file or to other formats.\n\n"), progname);
	puts(gettext("Usage:"));
	printf(gettext("  %s [options] dbname\n\n"), progname);
	puts(gettext("Options:"));

#ifdef HAVE_GETOPT_LONG
	puts(gettext(
		"  -a, --data-only          dump only the data, not the schema\n"
		"  -b, --blobs              include large objects in dump\n"
		"  -c, --clean              clean (drop) schema prior to create\n"
		"  -C, --create             include commands to create database in dump\n"
		"  -d, --inserts            dump data as INSERT, rather than COPY, commands\n"
		"  -D, --column-inserts     dump data as INSERT commands with column names\n"
		"  -f, --file=FILENAME      output file name\n"
		"  -F, --format {c|t|p}     output file format (custom, tar, plain text)\n"
		"  -h, --host=HOSTNAME      database server host name\n"
		"  -i, --ignore-version     proceed even when server version mismatches\n"
		"                           pg_dump version\n"
		"  -n, --no-quotes          suppress most quotes around identifiers\n"
		"  -N, --quotes             enable most quotes around identifiers\n"
		"  -o, --oids               include oids in dump\n"
		"  -O, --no-owner           do not output \\connect commands in plain\n"
		"                           text format\n"
		"  -p, --port=PORT          database server port number\n"
		"  -R, --no-reconnect       disable ALL reconnections to the database in\n"
		"                           plain text format\n"
		"  -s, --schema-only        dump only the schema, no data\n"
		"  -S, --superuser=NAME     specify the superuser user name to use in\n"
		"                           plain text format\n"
		"  -t, --table=TABLE        dump this table only (* for all)\n"
		"  -U, --username=NAME      connect as specified database user\n"
		"  -v, --verbose            verbose mode\n"
		"  -W, --password           force password prompt (should happen automatically)\n"
		"  -x, --no-privileges      do not dump privileges (grant/revoke)\n"
		"  -X use-set-session-authorization, --use-set-session-authorization\n"
		"                           output SET SESSION AUTHORIZATION commands rather\n"
		"                           than \\connect commands\n"
		"  -X disable-triggers, --disable-triggers\n"
		"                           disable triggers during data-only restore\n"
		"  -Z, --compress {0-9}     compression level for compressed formats\n"
	));
#else
	puts(gettext(
		"  -a                       dump only the data, not the schema\n"
		"  -b                       include large objects in dump\n"
		"  -c                       clean (drop) schema prior to create\n"
		"  -C                       include commands to create database in dump\n"
		"  -d                       dump data as INSERT, rather than COPY, commands\n"
		"  -D                       dump data as INSERT commands with column names\n"
		"  -f FILENAME              output file name\n"
		"  -F {c|t|p}               output file format (custom, tar, plain text)\n"
		"  -h HOSTNAME              database server host name\n"
		"  -i                       proceed even when server version mismatches\n"
		"                           pg_dump version\n"
		"  -n                       suppress most quotes around identifiers\n"
		"  -N                       enable most quotes around identifiers\n"
		"  -o                       include oids in dump\n"
		"  -O                       do not output \\connect commands in plain\n"
		"                           text format\n"
		"  -p PORT                  database server port number\n"
		"  -R                       disable ALL reconnections to the database in\n"
		"                           plain text format\n"
		"  -s                       dump only the schema, no data\n"
		"  -S NAME                  specify the superuser user name to use in\n"
		"                           plain text format\n"
		"  -t TABLE                 dump this table only (* for all)\n"
		"  -U NAME                  connect as specified database user\n"
		"  -v                       verbose mode\n"
		"  -W                       force password prompt (should happen automatically)\n"
		"  -x                       do not dump privileges (grant/revoke)\n"
		"  -X use-set-session-authorization\n"
		"                           output SET SESSION AUTHORIZATION commands rather\n"
		"                           than \\connect commands\n"
		"  -X disable-triggers      disable triggers during data-only restore\n"
		"  -Z {0-9}                 compression level for compressed formats\n"
	));
#endif
	puts(gettext("If no database name is not supplied, then the PGDATABASE environment\n"
				 "variable value is used.\n\n"
				 "Report bugs to <pgsql-bugs@postgresql.org>."));
}

static int
parse_version(const char *versionString)
{
	int			cnt;
	int			vmaj,
				vmin,
				vrev;

	cnt = sscanf(versionString, "%d.%d.%d", &vmaj, &vmin, &vrev);

	if (cnt < 2)
	{
		write_msg(NULL, "unable to parse version string \"%s\"\n", versionString);
		exit(1);
	}

	if (cnt == 2)
		vrev = 0;

	return (100 * vmaj + vmin) * 100 + vrev;
}

void
exit_nicely(void)
{
	PQfinish(g_conn);
	if (g_verbose)
		write_msg(NULL, "*** aborted because of error\n");
	exit(1);
}

/*
 * selectDumpableNamespace: policy-setting subroutine
 *		Mark a namespace as to be dumped or not
 */
static void
selectDumpableNamespace(NamespaceInfo *nsinfo)
{
	/*
	 * If a specific table is being dumped, do not dump any complete
	 * namespaces.  Otherwise, dump all non-system namespaces.
	 */
	if (selectTablename != NULL)
		nsinfo->dump = false;
	else if (strncmp(nsinfo->nspname, "pg_", 3) == 0)
		nsinfo->dump = false;
	else
		nsinfo->dump = true;
}

/*
 * selectDumpableTable: policy-setting subroutine
 *		Mark a table as to be dumped or not
 */
static void
selectDumpableTable(TableInfo *tbinfo)
{
	/*
	 * Always dump if dumping parent namespace; else, if a particular
	 * tablename has been specified, dump matching table name; else,
	 * do not dump.
	 */
	if (tbinfo->relnamespace->dump)
		tbinfo->dump = true;
	else if (selectTablename != NULL)
		tbinfo->dump = (strcmp(tbinfo->relname, selectTablename) == 0);
	else
		tbinfo->dump = false;
}

/*
 *	Dump a table's contents for loading using the COPY command
 *	- this routine is called by the Archiver when it wants the table
 *	  to be dumped.
 */

#define COPYBUFSIZ		8192

static int
dumpClasses_nodumpData(Archive *fout, char *oid, void *dctxv)
{
	const DumpContext *dctx = (DumpContext *) dctxv;
	TableInfo  *tbinfo = &dctx->tblinfo[dctx->tblidx];
	const char *classname = tbinfo->relname;
	const bool	hasoids = tbinfo->hasoids;
	const bool	oids = dctx->oids;
	PQExpBuffer q = createPQExpBuffer();
	PGresult   *res;
	int			ret;
	bool		copydone;
	char		copybuf[COPYBUFSIZ];

	if (g_verbose)
		write_msg(NULL, "dumping out the contents of table %s\n", classname);

	/*
	 * Make sure we are in proper schema.  We will qualify the table name
	 * below anyway (in case its name conflicts with a pg_catalog table);
	 * but this ensures reproducible results in case the table contains
	 * regproc, regclass, etc columns.
	 */
	selectSourceSchema(tbinfo->relnamespace->nspname);

	if (oids && hasoids)
	{
		appendPQExpBuffer(q, "COPY %s WITH OIDS TO stdout;",
						  fmtQualifiedId(tbinfo->relnamespace->nspname,
										 classname));
	}
	else
	{
		appendPQExpBuffer(q, "COPY %s TO stdout;",
						  fmtQualifiedId(tbinfo->relnamespace->nspname,
										 classname));
	}
	res = PQexec(g_conn, q->data);
	if (!res ||
		PQresultStatus(res) == PGRES_FATAL_ERROR)
	{
		write_msg(NULL, "SQL command to dump the contents of table \"%s\" failed\n",
				  classname);
		write_msg(NULL, "Error message from server: %s", PQerrorMessage(g_conn));
		write_msg(NULL, "The command was: %s\n", q->data);
		exit_nicely();
	}
	if (PQresultStatus(res) != PGRES_COPY_OUT)
	{
		write_msg(NULL, "SQL command to dump the contents of table \"%s\" executed abnormally.\n",
				  classname);
		write_msg(NULL, "The server returned status %d when %d was expected.\n",
				  PQresultStatus(res), PGRES_COPY_OUT);
		write_msg(NULL, "The command was: %s\n", q->data);
		exit_nicely();
	}

	copydone = false;

	while (!copydone)
	{
		ret = PQgetline(g_conn, copybuf, COPYBUFSIZ);

		if (copybuf[0] == '\\' &&
			copybuf[1] == '.' &&
			copybuf[2] == '\0')
		{
			copydone = true;	/* don't print this... */
		}
		else
		{
			archputs(copybuf, fout);
			switch (ret)
			{
				case EOF:
					copydone = true;
					/* FALLTHROUGH */
				case 0:
					archputc('\n', fout);
					break;
				case 1:
					break;
			}
		}

		/*
		 * THROTTLE:
		 *
		 * There was considerable discussion in late July, 2000
		 * regarding slowing down pg_dump when backing up large
		 * tables. Users with both slow & fast (muti-processor)
		 * machines experienced performance degradation when doing
		 * a backup.
		 *
		 * Initial attempts based on sleeping for a number of ms for
		 * each ms of work were deemed too complex, then a simple
		 * 'sleep in each loop' implementation was suggested. The
		 * latter failed because the loop was too tight. Finally,
		 * the following was implemented:
		 *
		 * If throttle is non-zero, then See how long since the last
		 * sleep. Work out how long to sleep (based on ratio). If
		 * sleep is more than 100ms, then sleep reset timer EndIf
		 * EndIf
		 *
		 * where the throttle value was the number of ms to sleep per
		 * ms of work. The calculation was done in each loop.
		 *
		 * Most of the hard work is done in the backend, and this
		 * solution still did not work particularly well: on slow
		 * machines, the ratio was 50:1, and on medium paced
		 * machines, 1:1, and on fast multi-processor machines, it
		 * had little or no effect, for reasons that were unclear.
		 *
		 * Further discussion ensued, and the proposal was dropped.
		 *
		 * For those people who want this feature, it can be
		 * implemented using gettimeofday in each loop,
		 * calculating the time since last sleep, multiplying that
		 * by the sleep ratio, then if the result is more than a
		 * preset 'minimum sleep time' (say 100ms), call the
		 * 'select' function to sleep for a subsecond period ie.
		 *
		 * select(0, NULL, NULL, NULL, &tvi);
		 *
		 * This will return after the interval specified in the
		 * structure tvi. Fianally, call gettimeofday again to
		 * save the 'last sleep time'.
		 */
	}
	archprintf(fout, "\\.\n");

	ret = PQendcopy(g_conn);
	if (ret != 0)
	{
		write_msg(NULL, "SQL command to dump the contents of table \"%s\" failed: PQendcopy() failed.\n", classname);
		write_msg(NULL, "Error message from server: %s", PQerrorMessage(g_conn));
		write_msg(NULL, "The command was: %s\n", q->data);
		exit_nicely();
	}

	PQclear(res);
	destroyPQExpBuffer(q);
	return 1;
}

static int
dumpClasses_dumpData(Archive *fout, char *oid, void *dctxv)
{
	const DumpContext *dctx = (DumpContext *) dctxv;
	TableInfo  *tbinfo = &dctx->tblinfo[dctx->tblidx];
	const char *classname = tbinfo->relname;
	PQExpBuffer q = createPQExpBuffer();
	PGresult   *res;
	int			tuple;
	int			field;

	/*
	 * Make sure we are in proper schema.  We will qualify the table name
	 * below anyway (in case its name conflicts with a pg_catalog table);
	 * but this ensures reproducible results in case the table contains
	 * regproc, regclass, etc columns.
	 */
	selectSourceSchema(tbinfo->relnamespace->nspname);

	if (fout->remoteVersion >= 70100)
	{
		appendPQExpBuffer(q, "DECLARE _pg_dump_cursor CURSOR FOR "
						  "SELECT * FROM ONLY %s",
						  fmtQualifiedId(tbinfo->relnamespace->nspname,
										 classname));
	}
	else
	{
		appendPQExpBuffer(q, "DECLARE _pg_dump_cursor CURSOR FOR "
						  "SELECT * FROM %s",
						  fmtQualifiedId(tbinfo->relnamespace->nspname,
										 classname));
	}

	res = PQexec(g_conn, q->data);
	if (!res ||
		PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		write_msg(NULL, "dumpClasses(): SQL command failed\n");
		write_msg(NULL, "Error message from server: %s", PQerrorMessage(g_conn));
		write_msg(NULL, "The command was: %s\n", q->data);
		exit_nicely();
	}

	do
	{
		PQclear(res);

		res = PQexec(g_conn, "FETCH 100 FROM _pg_dump_cursor");
		if (!res ||
			PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			write_msg(NULL, "dumpClasses(): SQL command failed\n");
			write_msg(NULL, "Error message from server: %s", PQerrorMessage(g_conn));
			write_msg(NULL, "The command was: FETCH 100 FROM _pg_dump_cursor\n");
			exit_nicely();
		}

		for (tuple = 0; tuple < PQntuples(res); tuple++)
		{
			archprintf(fout, "INSERT INTO %s ", fmtId(classname, force_quotes));
			if (attrNames == true)
			{
				resetPQExpBuffer(q);
				appendPQExpBuffer(q, "(");
				for (field = 0; field < PQnfields(res); field++)
				{
					if (field > 0)
						appendPQExpBuffer(q, ",");
					appendPQExpBuffer(q, fmtId(PQfname(res, field), force_quotes));
				}
				appendPQExpBuffer(q, ") ");
				archprintf(fout, "%s", q->data);
			}
			archprintf(fout, "VALUES (");
			for (field = 0; field < PQnfields(res); field++)
			{
				if (field > 0)
					archprintf(fout, ",");
				if (PQgetisnull(res, tuple, field))
				{
					archprintf(fout, "NULL");
					continue;
				}
				switch (PQftype(res, field))
				{
					case INT2OID:
					case INT4OID:
					case OIDOID:		/* int types */
					case FLOAT4OID:
					case FLOAT8OID:		/* float types */
						/* These types are printed without quotes */
						archprintf(fout, "%s",
								   PQgetvalue(res, tuple, field));
						break;
					case BITOID:
					case VARBITOID:
						archprintf(fout, "B'%s'",
								   PQgetvalue(res, tuple, field));
						break;
					default:

						/*
						 * All other types are printed as string literals,
						 * with appropriate escaping of special
						 * characters.
						 */
						resetPQExpBuffer(q);
						formatStringLiteral(q, PQgetvalue(res, tuple, field), CONV_ALL);
						archprintf(fout, "%s", q->data);
						break;
				}
			}
			archprintf(fout, ");\n");
		}

	} while (PQntuples(res) > 0);
	PQclear(res);

	res = PQexec(g_conn, "CLOSE _pg_dump_cursor");
	if (!res ||
		PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		write_msg(NULL, "dumpClasses(): SQL command failed\n");
		write_msg(NULL, "Error message from server: %s", PQerrorMessage(g_conn));
		write_msg(NULL, "The command was: CLOSE _pg_dump_cursor\n");
		exit_nicely();
	}
	PQclear(res);

	destroyPQExpBuffer(q);
	return 1;
}

/*
 * Convert a string value to an SQL string literal,
 * with appropriate escaping of special characters.
 * Quote mark ' goes to '' per SQL standard, other
 * stuff goes to \ sequences.
 * The literal is appended to the given PQExpBuffer.
 */
static void
formatStringLiteral(PQExpBuffer buf, const char *str, const formatLiteralOptions opts)
{
	appendPQExpBufferChar(buf, '\'');
	while (*str)
	{
		char		ch = *str++;

		if (ch == '\\' || ch == '\'')
		{
			appendPQExpBufferChar(buf, ch);		/* double these */
			appendPQExpBufferChar(buf, ch);
		}
		else if ((unsigned char) ch < (unsigned char) ' ' &&
				 (opts == CONV_ALL
				  || (ch != '\n' && ch != '\t')
				  ))
		{
			/*
			 * generate octal escape for control chars other than
			 * whitespace
			 */
			appendPQExpBufferChar(buf, '\\');
			appendPQExpBufferChar(buf, ((ch >> 6) & 3) + '0');
			appendPQExpBufferChar(buf, ((ch >> 3) & 7) + '0');
			appendPQExpBufferChar(buf, (ch & 7) + '0');
		}
		else
			appendPQExpBufferChar(buf, ch);
	}
	appendPQExpBufferChar(buf, '\'');
}

/*
 * DumpClasses -
 *	  dump the contents of all the classes.
 */
static void
dumpClasses(const TableInfo *tblinfo, const int numTables, Archive *fout,
			const bool oids)
{
	int			i;
	DataDumperPtr dumpFn;
	DumpContext *dumpCtx;
	char		copyBuf[512];
	char	   *copyStmt;

	for (i = 0; i < numTables; i++)
	{
		const char *classname = tblinfo[i].relname;

		/* Skip VIEW relations */
		if (tblinfo[i].relkind == RELKIND_VIEW)
			continue;

		if (tblinfo[i].relkind == RELKIND_SEQUENCE)		/* already dumped */
			continue;

		if (tblinfo[i].dump)
		{
			if (g_verbose)
				write_msg(NULL, "preparing to dump the contents of table %s\n",
						  classname);

			dumpCtx = (DumpContext *) malloc(sizeof(DumpContext));
			dumpCtx->tblinfo = (TableInfo *) tblinfo;
			dumpCtx->tblidx = i;
			dumpCtx->oids = oids;

			if (!dumpData)
			{
				/* Dump/restore using COPY */
				dumpFn = dumpClasses_nodumpData;
				sprintf(copyBuf, "COPY %s %sFROM stdin;\n",
						fmtId(tblinfo[i].relname, force_quotes),
						(oids && tblinfo[i].hasoids) ? "WITH OIDS " : "");
				copyStmt = copyBuf;
			}
			else
			{
				/* Restore using INSERT */
				dumpFn = dumpClasses_dumpData;
				copyStmt = NULL;
			}

			ArchiveEntry(fout, tblinfo[i].oid, tblinfo[i].relname,
						 tblinfo[i].relnamespace->nspname, tblinfo[i].usename,
						 "TABLE DATA", NULL, "", "", copyStmt,
						 dumpFn, dumpCtx);
		}
	}
}


/*
 * dumpDatabase:
 *	dump the database definition
 */
static int
dumpDatabase(Archive *AH)
{
	PQExpBuffer dbQry = createPQExpBuffer();
	PQExpBuffer delQry = createPQExpBuffer();
	PQExpBuffer creaQry = createPQExpBuffer();
	PGresult   *res;
	int			ntups;
	int			i_dba,
				i_encoding,
				i_datpath;
	const char *datname,
			   *dba,
			   *encoding,
			   *datpath;

	datname = PQdb(g_conn);

	if (g_verbose)
		write_msg(NULL, "saving database definition\n");

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	/* Get the database owner and parameters from pg_database */
	appendPQExpBuffer(dbQry, "select (select usename from pg_user where usesysid = datdba) as dba,"
					  " encoding, datpath from pg_database"
					  " where datname = ");
	formatStringLiteral(dbQry, datname, CONV_ALL);

	res = PQexec(g_conn, dbQry->data);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "SQL command failed\n");
		write_msg(NULL, "Error message from server: %s", PQerrorMessage(g_conn));
		write_msg(NULL, "The command was: %s\n", dbQry->data);
		exit_nicely();
	}

	ntups = PQntuples(res);

	if (ntups <= 0)
	{
		write_msg(NULL, "missing pg_database entry for database \"%s\"\n",
				  datname);
		exit_nicely();
	}

	if (ntups != 1)
	{
		write_msg(NULL, "query returned more than one (%d) pg_database entry for database \"%s\"\n",
				  ntups, datname);
		exit_nicely();
	}

	i_dba = PQfnumber(res, "dba");
	i_encoding = PQfnumber(res, "encoding");
	i_datpath = PQfnumber(res, "datpath");
	dba = PQgetvalue(res, 0, i_dba);
	encoding = PQgetvalue(res, 0, i_encoding);
	datpath = PQgetvalue(res, 0, i_datpath);

	appendPQExpBuffer(creaQry, "CREATE DATABASE %s WITH TEMPLATE = template0",
					  fmtId(datname, force_quotes));
	if (strlen(encoding) > 0)
		appendPQExpBuffer(creaQry, " ENCODING = %s", encoding);
	if (strlen(datpath) > 0)
		appendPQExpBuffer(creaQry, " LOCATION = '%s'", datpath);
	appendPQExpBuffer(creaQry, ";\n");

	appendPQExpBuffer(delQry, "DROP DATABASE %s;\n",
					  fmtId(datname, force_quotes));

	ArchiveEntry(AH, "0",		/* OID */
				 datname,		/* Name */
				 NULL,			/* Namespace */
				 dba,			/* Owner */
				 "DATABASE",	/* Desc */
				 NULL,			/* Deps */
				 creaQry->data,	/* Create */
				 delQry->data,	/* Del */
				 NULL,			/* Copy */
				 NULL,			/* Dumper */
				 NULL);			/* Dumper Arg */

	PQclear(res);

	destroyPQExpBuffer(dbQry);
	destroyPQExpBuffer(delQry);
	destroyPQExpBuffer(creaQry);

	return 1;
}


/*
 * dumpBlobs:
 *	dump all blobs
 *
 */

#define loBufSize 16384
#define loFetchSize 1000

static int
dumpBlobs(Archive *AH, char *junkOid, void *junkVal)
{
	PQExpBuffer oidQry = createPQExpBuffer();
	PQExpBuffer oidFetchQry = createPQExpBuffer();
	PGresult   *res;
	int			i;
	int			loFd;
	char		buf[loBufSize];
	int			cnt;
	Oid			blobOid;

	if (g_verbose)
		write_msg(NULL, "saving large objects\n");

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	/* Cursor to get all BLOB tables */
	if (AH->remoteVersion >= 70100)
		appendPQExpBuffer(oidQry, "Declare blobOid Cursor for SELECT DISTINCT loid FROM pg_largeobject");
	else
		appendPQExpBuffer(oidQry, "Declare blobOid Cursor for SELECT oid from pg_class where relkind = 'l'");

	res = PQexec(g_conn, oidQry->data);
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		write_msg(NULL, "dumpBlobs(): cursor declaration failed: %s", PQerrorMessage(g_conn));
		exit_nicely();
	}

	/* Fetch for cursor */
	appendPQExpBuffer(oidFetchQry, "Fetch %d in blobOid", loFetchSize);

	do
	{
		/* Do a fetch */
		PQclear(res);
		res = PQexec(g_conn, oidFetchQry->data);

		if (!res || PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			write_msg(NULL, "dumpBlobs(): fetch from cursor failed: %s",
					  PQerrorMessage(g_conn));
			exit_nicely();
		}

		/* Process the tuples, if any */
		for (i = 0; i < PQntuples(res); i++)
		{
			blobOid = atooid(PQgetvalue(res, i, 0));
			/* Open the BLOB */
			loFd = lo_open(g_conn, blobOid, INV_READ);
			if (loFd == -1)
			{
				write_msg(NULL, "dumpBlobs(): could not open large object: %s",
						  PQerrorMessage(g_conn));
				exit_nicely();
			}

			StartBlob(AH, blobOid);

			/* Now read it in chunks, sending data to archive */
			do
			{
				cnt = lo_read(g_conn, loFd, buf, loBufSize);
				if (cnt < 0)
				{
					write_msg(NULL, "dumpBlobs(): error reading large object: %s",
							  PQerrorMessage(g_conn));
					exit_nicely();
				}

				WriteData(AH, buf, cnt);

			} while (cnt > 0);

			lo_close(g_conn, loFd);

			EndBlob(AH, blobOid);

		}
	} while (PQntuples(res) > 0);

	destroyPQExpBuffer(oidQry);
	destroyPQExpBuffer(oidFetchQry);

	return 1;
}

/*
 * getNamespaces:
 *	  read all namespaces in the system catalogs and return them in the
 * NamespaceInfo* structure
 *
 *	numNamespaces is set to the number of namespaces read in
 */
NamespaceInfo *
getNamespaces(int *numNamespaces)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query;
	NamespaceInfo *nsinfo;
	int			i_oid;
	int			i_nspname;
	int			i_usename;
	int			i_nspacl;

	/*
	 * Before 7.3, there are no real namespaces; create two dummy entries,
	 * one for user stuff and one for system stuff.
	 */
	if (g_fout->remoteVersion < 70300)
	{
		nsinfo = (NamespaceInfo *) malloc(2 * sizeof(NamespaceInfo));

		nsinfo[0].oid = strdup("0");
		nsinfo[0].nspname = strdup("");
		nsinfo[0].usename = strdup("");
		nsinfo[0].nspacl = strdup("");

		selectDumpableNamespace(&nsinfo[0]);

		nsinfo[1].oid = strdup("1");
		nsinfo[1].nspname = strdup("pg_catalog");
		nsinfo[1].usename = strdup("");
		nsinfo[1].nspacl = strdup("");

		selectDumpableNamespace(&nsinfo[1]);

		g_namespaces = nsinfo;
		g_numNamespaces = *numNamespaces = 2;

		return nsinfo;
	}

	query = createPQExpBuffer();

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	/*
	 * we fetch all namespaces including system ones, so that every object
	 * we read in can be linked to a containing namespace.
	 */
	appendPQExpBuffer(query, "SELECT oid, nspname, "
						  "(select usename from pg_user where nspowner = usesysid) as usename, "
						  "nspacl "
						  "FROM pg_namespace");

	res = PQexec(g_conn, query->data);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to obtain list of namespaces failed: %s", PQerrorMessage(g_conn));
		exit_nicely();
	}

	ntups = PQntuples(res);

	nsinfo = (NamespaceInfo *) malloc(ntups * sizeof(NamespaceInfo));

	i_oid = PQfnumber(res, "oid");
	i_nspname = PQfnumber(res, "nspname");
	i_usename = PQfnumber(res, "usename");
	i_nspacl = PQfnumber(res, "nspacl");

	for (i = 0; i < ntups; i++)
	{
		nsinfo[i].oid = strdup(PQgetvalue(res, i, i_oid));
		nsinfo[i].nspname = strdup(PQgetvalue(res, i, i_nspname));
		nsinfo[i].usename = strdup(PQgetvalue(res, i, i_usename));
		nsinfo[i].nspacl = strdup(PQgetvalue(res, i, i_nspacl));

		/* Decide whether to dump this namespace */
		selectDumpableNamespace(&nsinfo[i]);

		if (strlen(nsinfo[i].usename) == 0)
			write_msg(NULL, "WARNING: owner of namespace %s appears to be invalid\n",
					  nsinfo[i].nspname);
	}

	PQclear(res);
	destroyPQExpBuffer(query);

	g_namespaces = nsinfo;
	g_numNamespaces = *numNamespaces = ntups;

	return nsinfo;
}

/*
 * findNamespace:
 *		given a namespace OID and an object OID, look up the info read by
 *		getNamespaces
 *
 * NB: for pre-7.3 source database, we use object OID to guess whether it's
 * a system object or not.  In 7.3 and later there is no guessing.
 */
static NamespaceInfo *
findNamespace(const char *nsoid, const char *objoid)
{
	int			i;

	if (g_fout->remoteVersion >= 70300)
	{
		for (i = 0; i < g_numNamespaces; i++)
		{
			NamespaceInfo  *nsinfo = &g_namespaces[i];

			if (strcmp(nsoid, nsinfo->oid) == 0)
				return nsinfo;
		}
		write_msg(NULL, "Failed to find namespace with OID %s.\n", nsoid);
		exit_nicely();
	}
	else
	{
		/* This code depends on the layout set up by getNamespaces. */
		if (atooid(objoid) > g_last_builtin_oid)
			i = 0;				/* user object */
		else
			i = 1;				/* system object */
		return &g_namespaces[i];
	}

	return NULL;				/* keep compiler quiet */
}

/*
 * getTypes:
 *	  read all types in the system catalogs and return them in the
 * TypeInfo* structure
 *
 *	numTypes is set to the number of types read in
 */
TypeInfo *
getTypes(int *numTypes)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	TypeInfo   *tinfo;
	int			i_oid;
	int			i_typname;
	int			i_typnamespace;
	int			i_usename;
	int			i_typelem;
	int			i_typrelid;
	int			i_typtype;
	int			i_typisdefined;

	/*
	 * we include even the built-in types because those may be used as
	 * array elements by user-defined types
	 *
	 * we filter out the built-in types when we dump out the types
	 *
	 * same approach for undefined (shell) types
	 */

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	if (g_fout->remoteVersion >= 70300)
	{
		appendPQExpBuffer(query, "SELECT pg_type.oid, typname, "
						  "typnamespace, "
						  "(select usename from pg_user where typowner = usesysid) as usename, "
						  "typelem, typrelid, typtype, typisdefined "
						  "FROM pg_type");
	}
	else
	{
		appendPQExpBuffer(query, "SELECT pg_type.oid, typname, "
						  "0::oid as typnamespace, "
						  "(select usename from pg_user where typowner = usesysid) as usename, "
						  "typelem, typrelid, typtype, typisdefined "
						  "FROM pg_type");
	}

	res = PQexec(g_conn, query->data);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to obtain list of data types failed: %s", PQerrorMessage(g_conn));
		exit_nicely();
	}

	ntups = PQntuples(res);

	tinfo = (TypeInfo *) malloc(ntups * sizeof(TypeInfo));

	i_oid = PQfnumber(res, "oid");
	i_typname = PQfnumber(res, "typname");
	i_typnamespace = PQfnumber(res, "typnamespace");
	i_usename = PQfnumber(res, "usename");
	i_typelem = PQfnumber(res, "typelem");
	i_typrelid = PQfnumber(res, "typrelid");
	i_typtype = PQfnumber(res, "typtype");
	i_typisdefined = PQfnumber(res, "typisdefined");

	for (i = 0; i < ntups; i++)
	{
		tinfo[i].oid = strdup(PQgetvalue(res, i, i_oid));
		tinfo[i].typname = strdup(PQgetvalue(res, i, i_typname));
		tinfo[i].typnamespace = findNamespace(PQgetvalue(res, i, i_typnamespace),
											  tinfo[i].oid);
		tinfo[i].usename = strdup(PQgetvalue(res, i, i_usename));
		tinfo[i].typelem = strdup(PQgetvalue(res, i, i_typelem));
		tinfo[i].typrelid = strdup(PQgetvalue(res, i, i_typrelid));
		tinfo[i].typtype = *PQgetvalue(res, i, i_typtype);

		/*
		 * check for user-defined array types, omit system generated ones
		 */
		if ((strcmp(tinfo[i].typelem, "0") != 0) &&
			tinfo[i].typname[0] != '_')
			tinfo[i].isArray = true;
		else
			tinfo[i].isArray = false;

		if (strcmp(PQgetvalue(res, i, i_typisdefined), "t") == 0)
			tinfo[i].isDefined = true;
		else
			tinfo[i].isDefined = false;

		if (strlen(tinfo[i].usename) == 0 && tinfo[i].isDefined)
			write_msg(NULL, "WARNING: owner of data type %s appears to be invalid\n",
					  tinfo[i].typname);
	}

	*numTypes = ntups;

	PQclear(res);

	destroyPQExpBuffer(query);

	return tinfo;
}

/*
 * getOperators:
 *	  read all operators in the system catalogs and return them in the
 * OprInfo* structure
 *
 *	numOprs is set to the number of operators read in
 */
OprInfo *
getOperators(int *numOprs)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	OprInfo    *oprinfo;
	int			i_oid;
	int			i_oprname;
	int			i_oprnamespace;
	int			i_usename;
	int			i_oprcode;

	/*
	 * find all operators, including builtin operators;
	 * we filter out system-defined operators at dump-out time.
	 */

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	if (g_fout->remoteVersion >= 70300)
	{
		appendPQExpBuffer(query, "SELECT pg_operator.oid, oprname, "
						  "oprnamespace, "
						  "(select usename from pg_user where oprowner = usesysid) as usename, "
						  "oprcode::oid "
						  "from pg_operator");
	}
	else
	{
		appendPQExpBuffer(query, "SELECT pg_operator.oid, oprname, "
						  "0::oid as oprnamespace, "
						  "(select usename from pg_user where oprowner = usesysid) as usename, "
						  "oprcode::oid "
						  "from pg_operator");
	}

	res = PQexec(g_conn, query->data);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to obtain list of operators failed: %s", PQerrorMessage(g_conn));
		exit_nicely();
	}

	ntups = PQntuples(res);
	*numOprs = ntups;

	oprinfo = (OprInfo *) malloc(ntups * sizeof(OprInfo));

	i_oid = PQfnumber(res, "oid");
	i_oprname = PQfnumber(res, "oprname");
	i_oprnamespace = PQfnumber(res, "oprnamespace");
	i_usename = PQfnumber(res, "usename");
	i_oprcode = PQfnumber(res, "oprcode");

	for (i = 0; i < ntups; i++)
	{
		oprinfo[i].oid = strdup(PQgetvalue(res, i, i_oid));
		oprinfo[i].oprname = strdup(PQgetvalue(res, i, i_oprname));
		oprinfo[i].oprnamespace = findNamespace(PQgetvalue(res, i, i_oprnamespace),
												oprinfo[i].oid);
		oprinfo[i].usename = strdup(PQgetvalue(res, i, i_usename));
		oprinfo[i].oprcode = strdup(PQgetvalue(res, i, i_oprcode));

		if (strlen(oprinfo[i].usename) == 0)
			write_msg(NULL, "WARNING: owner of operator \"%s\" appears to be invalid\n",
					  oprinfo[i].oprname);
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return oprinfo;
}

/*
 * getAggregates:
 *	  read all the user-defined aggregates in the system catalogs and
 * return them in the AggInfo* structure
 *
 * numAggs is set to the number of aggregates read in
 */
AggInfo *
getAggregates(int *numAggs)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	AggInfo    *agginfo;

	int			i_oid;
	int			i_aggname;
	int			i_aggnamespace;
	int			i_aggbasetype;
	int			i_usename;
	int			i_aggacl;

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	/* find all user-defined aggregates */

	if (g_fout->remoteVersion >= 70300)
	{
		appendPQExpBuffer(query, "SELECT pg_proc.oid, proname as aggname, "
						  "pronamespace as aggnamespace, "
						  "proargtypes[0] as aggbasetype, "
						  "(select usename from pg_user where proowner = usesysid) as usename, "
						  "proacl as aggacl "
						  "FROM pg_proc "
						  "WHERE proisagg "
						  "AND pronamespace != "
						  "(select oid from pg_namespace where nspname = 'pg_catalog')");
	}
	else
	{
		appendPQExpBuffer(query, "SELECT pg_aggregate.oid, aggname, "
						  "0::oid as aggnamespace, "
						  "aggbasetype, "
						  "(select usename from pg_user where aggowner = usesysid) as usename, "
						  "'{=X}' as aggacl "
						  "from pg_aggregate "
						  "where oid > '%u'::oid",
						  g_last_builtin_oid);
	}

	res = PQexec(g_conn, query->data);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to obtain list of aggregate functions failed: %s",
				  PQerrorMessage(g_conn));
		exit_nicely();
	}

	ntups = PQntuples(res);
	*numAggs = ntups;

	agginfo = (AggInfo *) malloc(ntups * sizeof(AggInfo));

	i_oid = PQfnumber(res, "oid");
	i_aggname = PQfnumber(res, "aggname");
	i_aggnamespace = PQfnumber(res, "aggnamespace");
	i_aggbasetype = PQfnumber(res, "aggbasetype");
	i_usename = PQfnumber(res, "usename");
	i_aggacl = PQfnumber(res, "aggacl");

	for (i = 0; i < ntups; i++)
	{
		agginfo[i].oid = strdup(PQgetvalue(res, i, i_oid));
		agginfo[i].aggname = strdup(PQgetvalue(res, i, i_aggname));
		agginfo[i].aggnamespace = findNamespace(PQgetvalue(res, i, i_aggnamespace),
												agginfo[i].oid);
		agginfo[i].aggbasetype = strdup(PQgetvalue(res, i, i_aggbasetype));
		agginfo[i].usename = strdup(PQgetvalue(res, i, i_usename));
		if (strlen(agginfo[i].usename) == 0)
			write_msg(NULL, "WARNING: owner of aggregate function \"%s\" appears to be invalid\n",
					  agginfo[i].aggname);
		agginfo[i].aggacl = strdup(PQgetvalue(res, i, i_aggacl));
		agginfo[i].fmtbasetype = NULL; /* computed when it's dumped */
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return agginfo;
}

/*
 * getFuncs:
 *	  read all the user-defined functions in the system catalogs and
 * return them in the FuncInfo* structure
 *
 * numFuncs is set to the number of functions read in
 */
FuncInfo *
getFuncs(int *numFuncs)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	FuncInfo   *finfo;

	int			i_oid;
	int			i_proname;
	int			i_pronamespace;
	int			i_usename;
	int			i_prolang;
	int			i_pronargs;
	int			i_proargtypes;
	int			i_prorettype;
	int			i_proacl;

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	/* find all user-defined funcs */

	if (g_fout->remoteVersion >= 70300)
	{
		appendPQExpBuffer(query,
						  "SELECT pg_proc.oid, proname, prolang, "
						  "pronargs, proargtypes, prorettype, proacl, "
						  "pronamespace, "
						  "(select usename from pg_user where proowner = usesysid) as usename "
						  "FROM pg_proc "
						  "WHERE NOT proisagg "
						  "AND pronamespace != "
						  "(select oid from pg_namespace where nspname = 'pg_catalog')");
	}
	else
	{
		appendPQExpBuffer(query,
						  "SELECT pg_proc.oid, proname, prolang, "
						  "pronargs, proargtypes, prorettype, "
						  "'{=X}' as proacl, "
						  "0::oid as pronamespace, "
						  "(select usename from pg_user where proowner = usesysid) as usename "
						  "FROM pg_proc "
						  "where pg_proc.oid > '%u'::oid",
						  g_last_builtin_oid);
	}

	res = PQexec(g_conn, query->data);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to obtain list of functions failed: %s",
				  PQerrorMessage(g_conn));
		exit_nicely();
	}

	ntups = PQntuples(res);

	*numFuncs = ntups;

	finfo = (FuncInfo *) malloc(ntups * sizeof(FuncInfo));

	memset((char *) finfo, 0, ntups * sizeof(FuncInfo));

	i_oid = PQfnumber(res, "oid");
	i_proname = PQfnumber(res, "proname");
	i_pronamespace = PQfnumber(res, "pronamespace");
	i_usename = PQfnumber(res, "usename");
	i_prolang = PQfnumber(res, "prolang");
	i_pronargs = PQfnumber(res, "pronargs");
	i_proargtypes = PQfnumber(res, "proargtypes");
	i_prorettype = PQfnumber(res, "prorettype");
	i_proacl = PQfnumber(res, "proacl");

	for (i = 0; i < ntups; i++)
	{
		finfo[i].oid = strdup(PQgetvalue(res, i, i_oid));
		finfo[i].proname = strdup(PQgetvalue(res, i, i_proname));
		finfo[i].pronamespace = findNamespace(PQgetvalue(res, i, i_pronamespace),
											  finfo[i].oid);
		finfo[i].usename = strdup(PQgetvalue(res, i, i_usename));
		finfo[i].lang = atooid(PQgetvalue(res, i, i_prolang));
		finfo[i].prorettype = strdup(PQgetvalue(res, i, i_prorettype));
		finfo[i].proacl = strdup(PQgetvalue(res, i, i_proacl));
		finfo[i].nargs = atoi(PQgetvalue(res, i, i_pronargs));
		if (finfo[i].nargs == 0)
			finfo[i].argtypes = NULL;
		else
		{
			finfo[i].argtypes = malloc(finfo[i].nargs * sizeof(finfo[i].argtypes[0]));
			parseNumericArray(PQgetvalue(res, i, i_proargtypes),
							  finfo[i].argtypes,
							  finfo[i].nargs);
		}

		finfo[i].dumped = false;

		if (strlen(finfo[i].usename) == 0)
			write_msg(NULL, "WARNING: owner of function \"%s\" appears to be invalid\n",
					  finfo[i].proname);
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return finfo;
}

/*
 * getTables
 *	  read all the user-defined tables (no indexes, no catalogs)
 * in the system catalogs return them in the TableInfo* structure
 *
 * numTables is set to the number of tables read in
 */
TableInfo *
getTables(int *numTables)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	PQExpBuffer delqry = createPQExpBuffer();
	PQExpBuffer lockquery = createPQExpBuffer();
	TableInfo  *tblinfo;

	int			i_reloid;
	int			i_relname;
	int			i_relnamespace;
	int			i_relkind;
	int			i_relacl;
	int			i_usename;
	int			i_relchecks;
	int			i_reltriggers;
	int			i_relhasindex;
	int			i_relhasrules;
	int			i_relhasoids;

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	/*
	 * Find all the tables (including views and sequences).
	 *
	 * We include system catalogs, so that we can work if a user table
	 * is defined to inherit from a system catalog (pretty weird, but...)
	 *
	 * We ignore tables that are not type 'r' (ordinary relation) or 'S'
	 * (sequence) or 'v' (view).
	 *
	 * Note: in this phase we should collect only a minimal amount of
	 * information about each table, basically just enough to decide if
	 * it is interesting.
	 */

	if (g_fout->remoteVersion >= 70300)
	{
		appendPQExpBuffer(query,
						  "SELECT pg_class.oid, relname, relacl, relkind, "
						  "relnamespace, "

						  "(select usename from pg_user where relowner = usesysid) as usename, "
						  "relchecks, reltriggers, "
						  "relhasindex, relhasrules, relhasoids "
						  "from pg_class "
						  "where relkind in ('%c', '%c', '%c') "
						  "order by oid",
						  RELKIND_RELATION, RELKIND_SEQUENCE, RELKIND_VIEW);
	}
	else if (g_fout->remoteVersion >= 70200)
	{
		appendPQExpBuffer(query,
						  "SELECT pg_class.oid, relname, relacl, relkind, "
						  "0::oid as relnamespace, "
						  "(select usename from pg_user where relowner = usesysid) as usename, "
						  "relchecks, reltriggers, "
						  "relhasindex, relhasrules, relhasoids "
						  "from pg_class "
						  "where relkind in ('%c', '%c', '%c') "
						  "order by oid",
					   RELKIND_RELATION, RELKIND_SEQUENCE, RELKIND_VIEW);
	}
	else if (g_fout->remoteVersion >= 70100)
	{
		/* all tables have oids in 7.1 */
		appendPQExpBuffer(query,
						"SELECT pg_class.oid, relname, relacl, relkind, "
						  "0::oid as relnamespace, "
						  "(select usename from pg_user where relowner = usesysid) as usename, "
						  "relchecks, reltriggers, "
						  "relhasindex, relhasrules, 't'::bool as relhasoids "
						  "from pg_class "
						  "where relkind in ('%c', '%c', '%c') "
						  "order by oid",
					   RELKIND_RELATION, RELKIND_SEQUENCE, RELKIND_VIEW);
	}
	else
	{
		/*
		 * Before 7.1, view relkind was not set to 'v', so we must check
		 * if we have a view by looking for a rule in pg_rewrite.
		 */
		appendPQExpBuffer(query,
						  "SELECT c.oid, relname, relacl, "
						  "CASE WHEN relhasrules and relkind = 'r' "
				  "  and EXISTS(SELECT rulename FROM pg_rewrite r WHERE "
				  "             r.ev_class = c.oid AND r.ev_type = '1') "
						  "THEN '%c'::\"char\" "
						  "ELSE relkind END AS relkind,"
						  "0::oid as relnamespace, "
						  "(select usename from pg_user where relowner = usesysid) as usename, "
						  "relchecks, reltriggers, "
						  "relhasindex, relhasrules, 't'::bool as relhasoids "
						  "from pg_class c "
						  "where relkind in ('%c', '%c') "
						  "order by oid",
						  RELKIND_VIEW,
						  RELKIND_RELATION, RELKIND_SEQUENCE);
	}

	res = PQexec(g_conn, query->data);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to obtain list of tables failed: %s",
				  PQerrorMessage(g_conn));
		exit_nicely();
	}

	ntups = PQntuples(res);

	*numTables = ntups;

	/*
	 * Extract data from result and lock dumpable tables.  We do the
	 * locking before anything else, to minimize the window wherein a
	 * table could disappear under us.
	 *
	 * Note that we have to save info about all tables here, even when
	 * dumping only one, because we don't yet know which tables might be
	 * inheritance ancestors of the target table.
	 */
	tblinfo = (TableInfo *) malloc(ntups * sizeof(TableInfo));
	memset(tblinfo, 0, ntups * sizeof(TableInfo));

	i_reloid = PQfnumber(res, "oid");
	i_relname = PQfnumber(res, "relname");
	i_relnamespace = PQfnumber(res, "relnamespace");
	i_relacl = PQfnumber(res, "relacl");
	i_relkind = PQfnumber(res, "relkind");
	i_usename = PQfnumber(res, "usename");
	i_relchecks = PQfnumber(res, "relchecks");
	i_reltriggers = PQfnumber(res, "reltriggers");
	i_relhasindex = PQfnumber(res, "relhasindex");
	i_relhasrules = PQfnumber(res, "relhasrules");
	i_relhasoids = PQfnumber(res, "relhasoids");

	for (i = 0; i < ntups; i++)
	{
		tblinfo[i].oid = strdup(PQgetvalue(res, i, i_reloid));
		tblinfo[i].relname = strdup(PQgetvalue(res, i, i_relname));
		tblinfo[i].relnamespace = findNamespace(PQgetvalue(res, i, i_relnamespace),
												tblinfo[i].oid);
		tblinfo[i].usename = strdup(PQgetvalue(res, i, i_usename));
		tblinfo[i].relacl = strdup(PQgetvalue(res, i, i_relacl));
		tblinfo[i].relkind = *(PQgetvalue(res, i, i_relkind));
		tblinfo[i].hasindex = (strcmp(PQgetvalue(res, i, i_relhasindex), "t") == 0);
		tblinfo[i].hasrules = (strcmp(PQgetvalue(res, i, i_relhasrules), "t") == 0);
		tblinfo[i].hasoids = (strcmp(PQgetvalue(res, i, i_relhasoids), "t") == 0);
		tblinfo[i].ncheck = atoi(PQgetvalue(res, i, i_relchecks));
		tblinfo[i].ntrig = atoi(PQgetvalue(res, i, i_reltriggers));

		/* other fields were zeroed above */

		/*
		 * Decide whether we want to dump this table.
		 */
		selectDumpableTable(&tblinfo[i]);
		tblinfo[i].interesting = tblinfo[i].dump;

		/*
		 * Read-lock target tables to make sure they aren't DROPPED or
		 * altered in schema before we get around to dumping them.
		 *
		 * Note that we don't explicitly lock parents of the target tables;
		 * we assume our lock on the child is enough to prevent schema
		 * alterations to parent tables.
		 *
		 * NOTE: it'd be kinda nice to lock views and sequences too, not only
		 * plain tables, but the backend doesn't presently allow that.
		 */
		if (tblinfo[i].dump && tblinfo[i].relkind == RELKIND_RELATION)
		{
			PGresult   *lres;

			resetPQExpBuffer(lockquery);
			appendPQExpBuffer(lockquery,
							  "LOCK TABLE %s IN ACCESS SHARE MODE",
							  fmtQualifiedId(tblinfo[i].relnamespace->nspname,
											 tblinfo[i].relname));
			lres = PQexec(g_conn, lockquery->data);
			if (!lres || PQresultStatus(lres) != PGRES_COMMAND_OK)
			{
				write_msg(NULL, "Attempt to lock table \"%s\" failed.  %s",
						  tblinfo[i].relname, PQerrorMessage(g_conn));
				exit_nicely();
			}
			PQclear(lres);
		}

		/* Emit notice if join for owner failed */
		if (strlen(tblinfo[i].usename) == 0)
			write_msg(NULL, "WARNING: owner of table \"%s\" appears to be invalid\n",
					  tblinfo[i].relname);
	}

	PQclear(res);
	destroyPQExpBuffer(query);
	destroyPQExpBuffer(delqry);
	destroyPQExpBuffer(lockquery);

	return tblinfo;
}

/*
 * getInherits
 *	  read all the inheritance information
 * from the system catalogs return them in the InhInfo* structure
 *
 * numInherits is set to the number of pairs read in
 */
InhInfo *
getInherits(int *numInherits)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	InhInfo    *inhinfo;

	int			i_inhrelid;
	int			i_inhparent;

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	/* find all the inheritance information */

	appendPQExpBuffer(query, "SELECT inhrelid, inhparent from pg_inherits");

	res = PQexec(g_conn, query->data);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to obtain inheritance relationships failed: %s",
				  PQerrorMessage(g_conn));
		exit_nicely();
	}

	ntups = PQntuples(res);

	*numInherits = ntups;

	inhinfo = (InhInfo *) malloc(ntups * sizeof(InhInfo));

	i_inhrelid = PQfnumber(res, "inhrelid");
	i_inhparent = PQfnumber(res, "inhparent");

	for (i = 0; i < ntups; i++)
	{
		inhinfo[i].inhrelid = strdup(PQgetvalue(res, i, i_inhrelid));
		inhinfo[i].inhparent = strdup(PQgetvalue(res, i, i_inhparent));
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return inhinfo;
}

/*
 * getTableAttrs -
 *	  for each interesting table, read its attributes types and names
 *
 * this is implemented in a very inefficient way right now, looping
 * through the tblinfo and doing a join per table to find the attrs and their
 * types
 *
 *	modifies tblinfo
 */
void
getTableAttrs(TableInfo *tblinfo, int numTables)
{
	int			i,
				j;
	PQExpBuffer q = createPQExpBuffer();
	int			i_attname;
	int			i_atttypname;
	int			i_atttypmod;
	int			i_attnotnull;
	int			i_atthasdef;
	PGresult   *res;
	int			ntups;
	bool		hasdefaults;

	for (i = 0; i < numTables; i++)
	{
		/* Don't bother to collect info for sequences */
		if (tblinfo[i].relkind == RELKIND_SEQUENCE)
			continue;

		/* Don't bother with uninteresting tables, either */
		if (!tblinfo[i].interesting)
			continue;

		/*
		 * Make sure we are in proper schema for this table; this allows
		 * correct retrieval of formatted type names and default exprs
		 */
		selectSourceSchema(tblinfo[i].relnamespace->nspname);

		/* find all the user attributes and their types */

		/*
		 * we must read the attribute names in attribute number order!
		 * because we will use the attnum to index into the attnames array
		 * later.  We actually ask to order by "attrelid, attnum" because
		 * (at least up to 7.3) the planner is not smart enough to realize
		 * it needn't re-sort the output of an indexscan on
		 * pg_attribute_relid_attnum_index.
		 */
		if (g_verbose)
			write_msg(NULL, "finding the columns and types for table %s\n",
					  tblinfo[i].relname);

		resetPQExpBuffer(q);

		if (g_fout->remoteVersion >= 70300)
		{
			appendPQExpBuffer(q, "SELECT attnum, attname, atttypmod, "
							  "attnotnull, atthasdef, "
							  "pg_catalog.format_type(atttypid,atttypmod) as atttypname "
							  "from pg_catalog.pg_attribute a "
							  "where attrelid = '%s'::pg_catalog.oid "
							  "and attnum > 0::pg_catalog.int2 "
							  "order by attrelid, attnum",
							  tblinfo[i].oid);
		}
		else if (g_fout->remoteVersion >= 70100)
		{
			appendPQExpBuffer(q, "SELECT attnum, attname, atttypmod, "
							  "attnotnull, atthasdef, "
							  "format_type(atttypid,atttypmod) as atttypname "
							  "from pg_attribute a "
							  "where attrelid = '%s'::oid "
							  "and attnum > 0::int2 "
							  "order by attrelid, attnum",
							  tblinfo[i].oid);
		}
		else
		{
			/* format_type not available before 7.1 */
			appendPQExpBuffer(q, "SELECT attnum, attname, atttypmod, "
							  "attnotnull, atthasdef, "
							  "(select typname from pg_type where oid = atttypid) as atttypname "
							  "from pg_attribute a "
							  "where attrelid = '%s'::oid "
							  "and attnum > 0::int2 "
							  "order by attrelid, attnum",
							  tblinfo[i].oid);
		}

		res = PQexec(g_conn, q->data);
		if (!res ||
			PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			write_msg(NULL, "query to get table columns failed: %s", PQerrorMessage(g_conn));
			exit_nicely();
		}

		ntups = PQntuples(res);

		i_attname = PQfnumber(res, "attname");
		i_atttypname = PQfnumber(res, "atttypname");
		i_atttypmod = PQfnumber(res, "atttypmod");
		i_attnotnull = PQfnumber(res, "attnotnull");
		i_atthasdef = PQfnumber(res, "atthasdef");

		tblinfo[i].numatts = ntups;
		tblinfo[i].attnames = (char **) malloc(ntups * sizeof(char *));
		tblinfo[i].atttypnames = (char **) malloc(ntups * sizeof(char *));
		tblinfo[i].atttypmod = (int *) malloc(ntups * sizeof(int));
		tblinfo[i].notnull = (bool *) malloc(ntups * sizeof(bool));
		tblinfo[i].adef_expr = (char **) malloc(ntups * sizeof(char *));
		tblinfo[i].inhAttrs = (bool *) malloc(ntups * sizeof(bool));
		tblinfo[i].inhAttrDef = (bool *) malloc(ntups * sizeof(bool));
		tblinfo[i].inhNotNull = (bool *) malloc(ntups * sizeof(bool));
		hasdefaults = false;

		for (j = 0; j < ntups; j++)
		{
			tblinfo[i].attnames[j] = strdup(PQgetvalue(res, j, i_attname));
			tblinfo[i].atttypnames[j] = strdup(PQgetvalue(res, j, i_atttypname));
			tblinfo[i].atttypmod[j] = atoi(PQgetvalue(res, j, i_atttypmod));
			tblinfo[i].notnull[j] = (PQgetvalue(res, j, i_attnotnull)[0] == 't');
			tblinfo[i].adef_expr[j] = NULL;	/* fix below */
			if (PQgetvalue(res, j, i_atthasdef)[0] == 't')
				hasdefaults = true;
			/* these flags will be set in flagInhAttrs() */
			tblinfo[i].inhAttrs[j] = false;
			tblinfo[i].inhAttrDef[j] = false;
			tblinfo[i].inhNotNull[j] = false;
		}

		PQclear(res);

		if (hasdefaults)
		{
			int			numDefaults;

			if (g_verbose)
				write_msg(NULL, "finding DEFAULT expressions for table %s\n",
						  tblinfo[i].relname);

			resetPQExpBuffer(q);
			if (g_fout->remoteVersion >= 70300)
			{
				appendPQExpBuffer(q, "SELECT adnum, "
								  "pg_catalog.pg_get_expr(adbin, adrelid) AS adsrc "
								  "FROM pg_catalog.pg_attrdef "
								  "WHERE adrelid = '%s'::pg_catalog.oid",
								  tblinfo[i].oid);
			}
			else if (g_fout->remoteVersion >= 70200)
			{
				appendPQExpBuffer(q, "SELECT adnum, "
								  "pg_get_expr(adbin, adrelid) AS adsrc "
								  "FROM pg_attrdef "
								  "WHERE adrelid = '%s'::oid",
								  tblinfo[i].oid);
			}
			else
			{
				/* no pg_get_expr, so must rely on adsrc */
				appendPQExpBuffer(q, "SELECT adnum, adsrc FROM pg_attrdef "
								  "WHERE adrelid = '%s'::oid",
								  tblinfo[i].oid);
			}
			res = PQexec(g_conn, q->data);
			if (!res ||
				PQresultStatus(res) != PGRES_TUPLES_OK)
			{
				write_msg(NULL, "query to get column default values failed: %s",
						  PQerrorMessage(g_conn));
				exit_nicely();
			}

			numDefaults = PQntuples(res);
			for (j = 0; j < numDefaults; j++)
			{
				int		adnum = atoi(PQgetvalue(res, j, 0));

				if (adnum <= 0 || adnum > ntups)
				{
					write_msg(NULL, "bogus adnum value %d for table %s\n",
							  adnum, tblinfo[i].relname);
					exit_nicely();
				}
				tblinfo[i].adef_expr[adnum-1] = strdup(PQgetvalue(res, j, 1));
			}
			PQclear(res);
		}
	}

	destroyPQExpBuffer(q);
}


/*
 * dumpComment --
 *
 * This routine is used to dump any comments associated with the
 * oid handed to this routine. The routine takes a constant character
 * string for the target part of the comment-creation command, plus
 * the namespace and owner of the object (for labeling the ArchiveEntry),
 * plus OID, class name, and subid which are the lookup key for pg_description.
 * If a matching pg_description entry is found, it is dumped.
 * Additional dependencies can be passed for the comment, too --- this is
 * needed for VIEWs, whose comments are filed under the table OID but
 * which are dumped in order by their rule OID.
 */

static void
dumpComment(Archive *fout, const char *target,
			const char *namespace, const char *owner,
			const char *oid, const char *classname, int subid,
			const char *((*deps)[]))
{
	PGresult   *res;
	PQExpBuffer query;
	int			i_description;

	/* Comments are SCHEMA not data */
	if (dataOnly)
		return;

	/*
	 * Note we do NOT change source schema here; preserve the caller's
	 * setting, instead.
	 */

	/*** Build query to find comment ***/

	query = createPQExpBuffer();

	if (fout->remoteVersion >= 70300)
	{
		appendPQExpBuffer(query, "SELECT description FROM pg_catalog.pg_description "
						  "WHERE objoid = '%s'::pg_catalog.oid and classoid = "
						  "'pg_catalog.%s'::pg_catalog.regclass "
						  "and objsubid = %d",
						  oid, classname, subid);
	}
	else if (fout->remoteVersion >= 70200)
	{
		appendPQExpBuffer(query, "SELECT description FROM pg_description "
						  "WHERE objoid = '%s'::oid and classoid = "
					   "(SELECT oid FROM pg_class where relname = '%s') "
						  "and objsubid = %d",
						  oid, classname, subid);
	}
	else
	{
		/* Note: this will fail to find attribute comments in pre-7.2... */
		appendPQExpBuffer(query, "SELECT description FROM pg_description WHERE objoid = '%s'::oid", oid);
	}

	/*** Execute query ***/

	res = PQexec(g_conn, query->data);
	if (!res || PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to get comment on oid %s failed: %s",
				  oid, PQerrorMessage(g_conn));
		exit_nicely();
	}

	/*** If a comment exists, build COMMENT ON statement ***/

	if (PQntuples(res) == 1)
	{
		i_description = PQfnumber(res, "description");
		resetPQExpBuffer(query);
		appendPQExpBuffer(query, "COMMENT ON %s IS ", target);
		formatStringLiteral(query, PQgetvalue(res, 0, i_description),
							PASS_LFTAB);
		appendPQExpBuffer(query, ";\n");

		ArchiveEntry(fout, oid, target, namespace, owner,
					 "COMMENT", deps,
					 query->data, "", NULL, NULL, NULL);
	}

	PQclear(res);
	destroyPQExpBuffer(query);
}

/*
 * dumpTableComment --
 *
 * As above, but dump comments for both the specified table (or view)
 * and its columns.  For speed, we want to do this with only one query.
 */
static void
dumpTableComment(Archive *fout, TableInfo *tbinfo,
				 const char *reltypename,
				 const char *((*deps)[]))
{
	PGresult   *res;
	PQExpBuffer query;
	PQExpBuffer target;
	int			i_description;
	int			i_objsubid;
	int			ntups;
	int			i;

	/* Comments are SCHEMA not data */
	if (dataOnly)
		return;

	/*
	 * Note we do NOT change source schema here; preserve the caller's
	 * setting, instead.
	 */

	/*** Build query to find comments ***/

	query = createPQExpBuffer();
	target = createPQExpBuffer();

	if (fout->remoteVersion >= 70300)
	{
		appendPQExpBuffer(query, "SELECT description, objsubid FROM pg_catalog.pg_description "
						  "WHERE objoid = '%s'::pg_catalog.oid and classoid = "
						  "'pg_catalog.pg_class'::pg_catalog.regclass "
						  "ORDER BY objoid, classoid, objsubid",
						  tbinfo->oid);
	}
	else if (fout->remoteVersion >= 70200)
	{
		appendPQExpBuffer(query, "SELECT description, objsubid FROM pg_description "
						  "WHERE objoid = '%s'::oid and classoid = "
					   "(SELECT oid FROM pg_class where relname = 'pg_class') "
						  "ORDER BY objoid, classoid, objsubid",
						  tbinfo->oid);
	}
	else
	{
		/* Note: this will fail to find attribute comments in pre-7.2... */
		appendPQExpBuffer(query, "SELECT description, 0 as objsubid FROM pg_description WHERE objoid = '%s'::oid", tbinfo->oid);
	}

	/*** Execute query ***/

	res = PQexec(g_conn, query->data);
	if (!res || PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to get comments on table %s failed: %s",
				  tbinfo->relname, PQerrorMessage(g_conn));
		exit_nicely();
	}
	i_description = PQfnumber(res, "description");
	i_objsubid = PQfnumber(res, "objsubid");

	/*** If comments exist, build COMMENT ON statements ***/

	ntups = PQntuples(res);
	for (i = 0; i < ntups; i++)
	{
		const char *descr = PQgetvalue(res, i, i_description);
		int objsubid = atoi(PQgetvalue(res, i, i_objsubid));

		if (objsubid == 0)
		{
			resetPQExpBuffer(target);
			appendPQExpBuffer(target, "%s %s", reltypename,
							  fmtId(tbinfo->relname, force_quotes));

			resetPQExpBuffer(query);
			appendPQExpBuffer(query, "COMMENT ON %s IS ", target->data);
			formatStringLiteral(query, descr, PASS_LFTAB);
			appendPQExpBuffer(query, ";\n");

			ArchiveEntry(fout, tbinfo->oid, target->data,
						 tbinfo->relnamespace->nspname, tbinfo->usename,
						 "COMMENT", deps,
						 query->data, "", NULL, NULL, NULL);
		}
		else if (objsubid > 0 && objsubid <= tbinfo->numatts)
		{
			resetPQExpBuffer(target);
			appendPQExpBuffer(target, "COLUMN %s.",
							  fmtId(tbinfo->relname, force_quotes));
			appendPQExpBuffer(target, "%s",
							  fmtId(tbinfo->attnames[objsubid-1],
									force_quotes));

			resetPQExpBuffer(query);
			appendPQExpBuffer(query, "COMMENT ON %s IS ", target->data);
			formatStringLiteral(query, descr, PASS_LFTAB);
			appendPQExpBuffer(query, ";\n");

			ArchiveEntry(fout, tbinfo->oid, target->data,
						 tbinfo->relnamespace->nspname, tbinfo->usename,
						 "COMMENT", deps,
						 query->data, "", NULL, NULL, NULL);
		}
	}

	PQclear(res);
	destroyPQExpBuffer(query);
	destroyPQExpBuffer(target);
}

/*
 * dumpDBComment --
 *
 * This routine is used to dump any comments associated with the
 * database to which we are currently connected.
 */
void
dumpDBComment(Archive *fout)
{
	PGresult   *res;
	PQExpBuffer query;
	int			i_oid;

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	/*** Build query to find comment ***/

	query = createPQExpBuffer();
	appendPQExpBuffer(query, "SELECT oid FROM pg_database WHERE datname = ");
	formatStringLiteral(query, PQdb(g_conn), CONV_ALL);

	/*** Execute query ***/

	res = PQexec(g_conn, query->data);
	if (!res || PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to get database oid failed: %s",
				  PQerrorMessage(g_conn));
		exit_nicely();
	}

	/*** If a comment exists, build COMMENT ON statement ***/

	if (PQntuples(res) != 0)
	{
		i_oid = PQfnumber(res, "oid");
		resetPQExpBuffer(query);
		appendPQExpBuffer(query, "DATABASE %s", fmtId(PQdb(g_conn), force_quotes));
		dumpComment(fout, query->data, NULL, "",
					PQgetvalue(res, 0, i_oid), "pg_database", 0, NULL);
	}

	PQclear(res);
	destroyPQExpBuffer(query);
}

/*
 * dumpNamespaces
 *    writes out to fout the queries to recreate user-defined namespaces
 */
void
dumpNamespaces(Archive *fout, NamespaceInfo *nsinfo, int numNamespaces)
{
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	int			i;
	char	   *qnspname;

	for (i = 0; i < numNamespaces; i++)
	{
		NamespaceInfo *nspinfo = &nsinfo[i];

		/* skip if not to be dumped */
		if (!nspinfo->dump)
			continue;

		/* don't dump dummy namespace from pre-7.3 source */
		if (strlen(nspinfo->nspname) == 0)
			continue;

		qnspname = strdup(fmtId(nspinfo->nspname, force_quotes));

		/*
		 * If it's the PUBLIC namespace, don't emit a CREATE SCHEMA
		 * record for it, since we expect PUBLIC to exist already in
		 * the destination database.  And emit ACL info only if the ACL
		 * isn't the standard value for PUBLIC.
		 */
		if (strcmp(nspinfo->nspname, "public") == 0)
		{
			if (!aclsSkip && strcmp(nspinfo->nspacl, "{=UC}") != 0)
				dumpACL(fout, "SCHEMA", qnspname, nspinfo->nspname, NULL,
						nspinfo->usename, nspinfo->nspacl,
						nspinfo->oid);
		}
		else
		{
			resetPQExpBuffer(q);
			resetPQExpBuffer(delq);

			appendPQExpBuffer(delq, "DROP SCHEMA %s;\n", qnspname);

			appendPQExpBuffer(q, "CREATE SCHEMA %s;\n", qnspname);

			ArchiveEntry(fout, nspinfo->oid, nspinfo->nspname,
						 NULL,
						 nspinfo->usename, "SCHEMA", NULL,
						 q->data, delq->data, NULL, NULL, NULL);

			/*** Dump Schema Comments ***/
			resetPQExpBuffer(q);
			appendPQExpBuffer(q, "SCHEMA %s", qnspname);
			dumpComment(fout, q->data,
						NULL, nspinfo->usename,
						nspinfo->oid, "pg_namespace", 0, NULL);

			if (!aclsSkip)
				dumpACL(fout, "SCHEMA", qnspname, nspinfo->nspname, NULL,
						nspinfo->usename, nspinfo->nspacl,
						nspinfo->oid);
		}

		free(qnspname);
	}

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
}

/*
 * dumpOneBaseType
 *    writes out to fout the queries to recreate a user-defined base type
 *    as requested by dumpTypes
 */
static void
dumpOneBaseType(Archive *fout, TypeInfo *tinfo,
				FuncInfo *g_finfo, int numFuncs,
				TypeInfo *g_tinfo, int numTypes)
{
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;
	int			ntups;
	int			funcInd;
	char	   *typlen;
	char	   *typprtlen;
	char	   *typinput;
	char	   *typoutput;
	char	   *typreceive;
	char	   *typsend;
	char	   *typinputoid;
	char	   *typoutputoid;
	char	   *typreceiveoid;
	char	   *typsendoid;
	char	   *typdelim;
	char	   *typdefault;
	char	   *typbyval;
	char	   *typalign;
	char	   *typstorage;
	const char *((*deps)[]);
	int			depIdx = 0;

	deps = malloc(sizeof(char *) * 10);

	/* Set proper schema search path so regproc references list correctly */
	selectSourceSchema(tinfo->typnamespace->nspname);

	/* Fetch type-specific details */
	if (fout->remoteVersion >= 70300)
	{
		appendPQExpBuffer(query, "SELECT typlen, typprtlen, "
						  "typinput, typoutput, typreceive, typsend, "
						  "typinput::pg_catalog.oid as typinputoid, "
						  "typoutput::pg_catalog.oid as typoutputoid, "
						  "typreceive::pg_catalog.oid as typreceiveoid, "
						  "typsend::pg_catalog.oid as typsendoid, "
						  "typdelim, typdefault, typbyval, typalign, "
						  "typstorage "
						  "FROM pg_catalog.pg_type "
						  "WHERE oid = '%s'::pg_catalog.oid",
						  tinfo->oid);
	}
	else if (fout->remoteVersion >= 70100)
	{
		appendPQExpBuffer(query, "SELECT typlen, typprtlen, "
						  "typinput, typoutput, typreceive, typsend, "
						  "typinput::oid as typinputoid, "
						  "typoutput::oid as typoutputoid, "
						  "typreceive::oid as typreceiveoid, "
						  "typsend::oid as typsendoid, "
						  "typdelim, typdefault, typbyval, typalign, "
						  "typstorage "
						  "FROM pg_type "
						  "WHERE oid = '%s'::oid",
						  tinfo->oid);
	}
	else
	{
		appendPQExpBuffer(query, "SELECT typlen, typprtlen, "
						  "typinput, typoutput, typreceive, typsend, "
						  "typinput::oid as typinputoid, "
						  "typoutput::oid as typoutputoid, "
						  "typreceive::oid as typreceiveoid, "
						  "typsend::oid as typsendoid, "
						  "typdelim, typdefault, typbyval, typalign, "
						  "'p'::char as typstorage "
						  "FROM pg_type "
						  "WHERE oid = '%s'::oid",
						  tinfo->oid);
	}

	res = PQexec(g_conn, query->data);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to obtain type information for %s failed: %s",
				  tinfo->typname, PQerrorMessage(g_conn));
		exit_nicely();
	}

	/* Expecting a single result only */
	ntups = PQntuples(res);
	if (ntups != 1)
	{
		write_msg(NULL, "Got %d rows instead of one from: %s",
				  ntups, query->data);
		exit_nicely();
	}

	typlen = PQgetvalue(res, 0, PQfnumber(res, "typlen"));
	typprtlen = PQgetvalue(res, 0, PQfnumber(res, "typprtlen"));
	typinput = PQgetvalue(res, 0, PQfnumber(res, "typinput"));
	typoutput = PQgetvalue(res, 0, PQfnumber(res, "typoutput"));
	typreceive = PQgetvalue(res, 0, PQfnumber(res, "typreceive"));
	typsend = PQgetvalue(res, 0, PQfnumber(res, "typsend"));
	typinputoid = PQgetvalue(res, 0, PQfnumber(res, "typinputoid"));
	typoutputoid = PQgetvalue(res, 0, PQfnumber(res, "typoutputoid"));
	typreceiveoid = PQgetvalue(res, 0, PQfnumber(res, "typreceiveoid"));
	typsendoid = PQgetvalue(res, 0, PQfnumber(res, "typsendoid"));
	typdelim = PQgetvalue(res, 0, PQfnumber(res, "typdelim"));
	if (PQgetisnull(res, 0, PQfnumber(res, "typdefault")))
		typdefault = NULL;
	else
		typdefault = PQgetvalue(res, 0, PQfnumber(res, "typdefault"));
	typbyval = PQgetvalue(res, 0, PQfnumber(res, "typbyval"));
	typalign = PQgetvalue(res, 0, PQfnumber(res, "typalign"));
	typstorage = PQgetvalue(res, 0, PQfnumber(res, "typstorage"));

	/*
	 * Before we create a type, we need to create the input and output
	 * functions for it, if they haven't been created already.  So make
	 * sure there are dependency entries for this.  But don't include
	 * dependencies if the functions aren't going to be dumped.
	 */
	funcInd = findFuncByOid(g_finfo, numFuncs, typinputoid);
	if (funcInd >= 0 && g_finfo[funcInd].pronamespace->dump)
		(*deps)[depIdx++] = strdup(typinputoid);

	funcInd = findFuncByOid(g_finfo, numFuncs, typoutputoid);
	if (funcInd >= 0 && g_finfo[funcInd].pronamespace->dump)
		(*deps)[depIdx++] = strdup(typoutputoid);

	if (strcmp(typreceiveoid, typinputoid) != 0)
	{
		funcInd = findFuncByOid(g_finfo, numFuncs, typreceiveoid);
		if (funcInd >= 0 && g_finfo[funcInd].pronamespace->dump)
			(*deps)[depIdx++] = strdup(typreceiveoid);
	}

	if (strcmp(typsendoid, typoutputoid) != 0)
	{
		funcInd = findFuncByOid(g_finfo, numFuncs, typsendoid);
		if (funcInd >= 0 && g_finfo[funcInd].pronamespace->dump)
			(*deps)[depIdx++] = strdup(typsendoid);
	}

	/* DROP must be fully qualified in case same name appears in pg_catalog */
	appendPQExpBuffer(delq, "DROP TYPE %s.",
					  fmtId(tinfo->typnamespace->nspname, force_quotes));
	appendPQExpBuffer(delq, "%s;\n",
					  fmtId(tinfo->typname, force_quotes));

	appendPQExpBuffer(q,
					  "CREATE TYPE %s "
					  "( internallength = %s, externallength = %s,",
					  fmtId(tinfo->typname, force_quotes),
					  (strcmp(typlen, "-1") == 0) ? "variable" : typlen,
					  (strcmp(typprtlen, "-1") == 0) ? "variable" : typprtlen);

	if (fout->remoteVersion >= 70300)
	{
		/* regproc result is correctly quoted in 7.3 */
		appendPQExpBuffer(q, " input = %s, output = %s, "
						  "send = %s, receive = %s",
						  typinput, typoutput, typsend, typreceive);
	}
	else
	{
		/* regproc delivers an unquoted name before 7.3 */
		/* cannot combine these because fmtId uses static result area */
		appendPQExpBuffer(q, " input = %s,",
						  fmtId(typinput, force_quotes));
		appendPQExpBuffer(q, " output = %s,",
						  fmtId(typoutput, force_quotes));
		appendPQExpBuffer(q, " send = %s,",
						  fmtId(typsend, force_quotes));
		appendPQExpBuffer(q, " receive = %s",
						  fmtId(typreceive, force_quotes));
	}

	if (typdefault != NULL)
	{
		appendPQExpBuffer(q, ", default = ");
		formatStringLiteral(q, typdefault, CONV_ALL);
	}

	if (tinfo->isArray)
	{
		char	   *elemType;

		/* reselect schema in case changed by function dump */
		selectSourceSchema(tinfo->typnamespace->nspname);
		elemType = getFormattedTypeName(tinfo->typelem, zeroAsOpaque);
		appendPQExpBuffer(q, ", element = %s, delimiter = ", elemType);
		formatStringLiteral(q, typdelim, CONV_ALL);
		free(elemType);

		(*deps)[depIdx++] = strdup(tinfo->typelem);
	}

	if (strcmp(typalign, "c") == 0)
		appendPQExpBuffer(q, ", alignment = char");
	else if (strcmp(typalign, "s") == 0)
		appendPQExpBuffer(q, ", alignment = int2");
	else if (strcmp(typalign, "i") == 0)
		appendPQExpBuffer(q, ", alignment = int4");
	else if (strcmp(typalign, "d") == 0)
		appendPQExpBuffer(q, ", alignment = double");

	if (strcmp(typstorage, "p") == 0)
		appendPQExpBuffer(q, ", storage = plain");
	else if (strcmp(typstorage, "e") == 0)
		appendPQExpBuffer(q, ", storage = external");
	else if (strcmp(typstorage, "x") == 0)
		appendPQExpBuffer(q, ", storage = extended");
	else if (strcmp(typstorage, "m") == 0)
		appendPQExpBuffer(q, ", storage = main");

	if (strcmp(typbyval, "t") == 0)
		appendPQExpBuffer(q, ", passedbyvalue);\n");
	else
		appendPQExpBuffer(q, ");\n");

	(*deps)[depIdx++] = NULL;		/* End of List */

	ArchiveEntry(fout, tinfo->oid, tinfo->typname,
				 tinfo->typnamespace->nspname,
				 tinfo->usename, "TYPE", deps,
				 q->data, delq->data, NULL, NULL, NULL);

	/*** Dump Type Comments ***/
	resetPQExpBuffer(q);

	appendPQExpBuffer(q, "TYPE %s", fmtId(tinfo->typname, force_quotes));
	dumpComment(fout, q->data,
				tinfo->typnamespace->nspname, tinfo->usename,
				tinfo->oid, "pg_type", 0, NULL);

	PQclear(res);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(query);
}

/*
 * dumpOneDomain
 *    writes out to fout the queries to recreate a user-defined domain
 *    as requested by dumpTypes
 */
static void
dumpOneDomain(Archive *fout, TypeInfo *tinfo)
{
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;
	int			ntups;
	char	   *typnotnull;
	char	   *typdefn;
	char	   *typdefault;
	char	   *typbasetype;
	const char *((*deps)[]);
	int			depIdx = 0;

	deps = malloc(sizeof(char *) * 10);

	/* Set proper schema search path so type references list correctly */
	selectSourceSchema(tinfo->typnamespace->nspname);

	/* Fetch domain specific details */
	/* We assume here that remoteVersion must be at least 70300 */
	appendPQExpBuffer(query, "SELECT typnotnull, "
					  "pg_catalog.format_type(typbasetype, typtypmod) as typdefn, "
					  "typdefault, typbasetype "
					  "FROM pg_catalog.pg_type "
					  "WHERE oid = '%s'::pg_catalog.oid",
					  tinfo->oid);

	res = PQexec(g_conn, query->data);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to obtain domain information failed: %s", PQerrorMessage(g_conn));
		exit_nicely();
	}

	/* Expecting a single result only */
	ntups = PQntuples(res);
	if (ntups != 1)
	{
		write_msg(NULL, "Got %d rows instead of one from: %s",
				  ntups, query->data);
		exit_nicely();
	}

	typnotnull = PQgetvalue(res, 0, PQfnumber(res, "typnotnull"));
	typdefn = PQgetvalue(res, 0, PQfnumber(res, "typdefn"));
	if (PQgetisnull(res, 0, PQfnumber(res, "typdefault")))
		typdefault = NULL;
	else
		typdefault = PQgetvalue(res, 0, PQfnumber(res, "typdefault"));
	typbasetype = PQgetvalue(res, 0, PQfnumber(res, "typbasetype"));

	/* DROP must be fully qualified in case same name appears in pg_catalog */
	appendPQExpBuffer(delq, "DROP DOMAIN %s.",
					  fmtId(tinfo->typnamespace->nspname, force_quotes));
	appendPQExpBuffer(delq, "%s RESTRICT;\n",
					  fmtId(tinfo->typname, force_quotes));

	appendPQExpBuffer(q,
					  "CREATE DOMAIN %s AS %s",
					  fmtId(tinfo->typname, force_quotes),
					  typdefn);

	/* Depends on the base type */
	(*deps)[depIdx++] = strdup(typbasetype);

	if (typnotnull[0] == 't')
		appendPQExpBuffer(q, " NOT NULL");

	if (typdefault)
		appendPQExpBuffer(q, " DEFAULT %s", typdefault);

	appendPQExpBuffer(q, ";\n");

	(*deps)[depIdx++] = NULL;		/* End of List */

	ArchiveEntry(fout, tinfo->oid, tinfo->typname,
				 tinfo->typnamespace->nspname,
				 tinfo->usename, "DOMAIN", deps,
				 q->data, delq->data, NULL, NULL, NULL);

	/*** Dump Domain Comments ***/
	resetPQExpBuffer(q);

	appendPQExpBuffer(q, "DOMAIN %s", fmtId(tinfo->typname, force_quotes));
	dumpComment(fout, q->data,
				tinfo->typnamespace->nspname, tinfo->usename,
				tinfo->oid, "pg_type", 0, NULL);

	PQclear(res);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(query);
}

/*
 * dumpTypes
 *	  writes out to fout the queries to recreate all the user-defined types
 */
void
dumpTypes(Archive *fout, FuncInfo *finfo, int numFuncs,
		  TypeInfo *tinfo, int numTypes)
{
	int			i;

	for (i = 0; i < numTypes; i++)
	{
		/* Dump only types in dumpable namespaces */
		if (!tinfo[i].typnamespace->dump)
			continue;

		/* skip relation types */
		if (atooid(tinfo[i].typrelid) != 0)
			continue;

		/* skip undefined placeholder types */
		if (!tinfo[i].isDefined)
			continue;

		/* skip all array types that start w/ underscore */
		if ((tinfo[i].typname[0] == '_') &&
			atooid(tinfo[i].typelem) != 0)
			continue;

		/* Dump out in proper style */
		if (tinfo[i].typtype == 'b')
			dumpOneBaseType(fout, &tinfo[i],
							finfo, numFuncs, tinfo, numTypes);
		else if (tinfo[i].typtype == 'd')
			dumpOneDomain(fout, &tinfo[i]);
	}
}

/*
 * dumpProcLangs
 *		  writes out to fout the queries to recreate user-defined procedural languages
 */
void
dumpProcLangs(Archive *fout, FuncInfo finfo[], int numFuncs)
{
	PGresult   *res;
	PQExpBuffer query = createPQExpBuffer();
	PQExpBuffer defqry = createPQExpBuffer();
	PQExpBuffer delqry = createPQExpBuffer();
	int			ntups;
	int			i_oid;
	int			i_lanname;
	int			i_lanpltrusted;
	int			i_lanplcallfoid;
	int			i_lanvalidator = -1;
	int			i_lancompiler;
	int			i_lanacl = -1;
	char	   *lanoid;
	char	   *lanname;
	char	   *lancompiler;
	char	   *lanacl;
	const char *lanplcallfoid;
	const char *lanvalidator;
	const char *((*deps)[]);
	int			depIdx;
	int			i,
				fidx,
				vidx = -1;

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	appendPQExpBuffer(query, "SELECT oid, * FROM pg_language "
					  "WHERE lanispl "
					  "ORDER BY oid");
	res = PQexec(g_conn, query->data);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to obtain list of procedural languages failed: %s",
				  PQerrorMessage(g_conn));
		exit_nicely();
	}
	ntups = PQntuples(res);

	i_lanname = PQfnumber(res, "lanname");
	i_lanpltrusted = PQfnumber(res, "lanpltrusted");
	i_lanplcallfoid = PQfnumber(res, "lanplcallfoid");
	i_lancompiler = PQfnumber(res, "lancompiler");
	i_oid = PQfnumber(res, "oid");
	if (fout->remoteVersion >= 70300)
	{
		i_lanvalidator = PQfnumber(res, "lanvalidator");
		i_lanacl = PQfnumber(res, "lanacl");
	}

	for (i = 0; i < ntups; i++)
	{
		lanoid = PQgetvalue(res, i, i_oid);
		lanplcallfoid = PQgetvalue(res, i, i_lanplcallfoid);
		lanname = PQgetvalue(res, i, i_lanname);
		lancompiler = PQgetvalue(res, i, i_lancompiler);
		if (fout->remoteVersion >= 70300)
		{
			lanvalidator = PQgetvalue(res, i, i_lanvalidator);
			lanacl = PQgetvalue(res, i, i_lanacl);
		}
		else
		{
			lanvalidator = "0";
			lanacl = "{=U}";
		}

		fidx = findFuncByOid(finfo, numFuncs, lanplcallfoid);
		if (fidx < 0)
		{
			write_msg(NULL, "handler procedure for procedural language %s not found\n",
					  lanname);
			exit_nicely();
		}

		if (strcmp(lanvalidator, "0") != 0)
		{
			vidx = findFuncByOid(finfo, numFuncs, lanvalidator);
			if (vidx < 0)
			{
				write_msg(NULL, "validator procedure for procedural language %s not found\n",
						  lanname);
				exit_nicely();
			}
		}

		/*
		 * Current theory is to dump PLs iff their underlying functions
		 * will be dumped (are in a dumpable namespace, or have a non-system
		 * OID in pre-7.3 databases).  Actually, we treat the PL itself
		 * as being in the underlying function's namespace, though it
		 * isn't really.  This avoids searchpath problems for the HANDLER
		 * clause.
		 */
		if (!finfo[fidx].pronamespace->dump)
			continue;

		resetPQExpBuffer(defqry);
		resetPQExpBuffer(delqry);

		/* Make a dependency to ensure function is dumped first */
		deps = malloc(sizeof(char *) * (2 + (strcmp(lanvalidator, "0")!=0) ? 1 : 0));
		depIdx = 0;

		(*deps)[depIdx++] = strdup(lanplcallfoid);

		appendPQExpBuffer(delqry, "DROP PROCEDURAL LANGUAGE %s;\n",
						  fmtId(lanname, force_quotes));

		appendPQExpBuffer(defqry, "CREATE %sPROCEDURAL LANGUAGE %s",
						  (PQgetvalue(res, i, i_lanpltrusted)[0] == 't') ?
						  "TRUSTED " : "",
						  fmtId(lanname, force_quotes));
		appendPQExpBuffer(defqry, " HANDLER %s",
						  fmtId(finfo[fidx].proname, force_quotes));
		if (strcmp(lanvalidator, "0")!=0)
		{
			appendPQExpBuffer(defqry, " VALIDATOR ");
			/* Cope with possibility that validator is in different schema */
			if (finfo[vidx].pronamespace != finfo[fidx].pronamespace)
				appendPQExpBuffer(defqry, "%s.",
								  fmtId(finfo[vidx].pronamespace->nspname,
										force_quotes));
			appendPQExpBuffer(defqry, "%s",
							  fmtId(finfo[vidx].proname, force_quotes));
			(*deps)[depIdx++] = strdup(lanvalidator);
		}
		appendPQExpBuffer(defqry, ";\n");

		(*deps)[depIdx++] = NULL;		/* End of List */

		ArchiveEntry(fout, lanoid, lanname,
					 finfo[fidx].pronamespace->nspname, "",
					 "PROCEDURAL LANGUAGE", deps,
					 defqry->data, delqry->data, NULL, NULL, NULL);

		if (!aclsSkip)
		{
			char *tmp = strdup(fmtId(lanname, force_quotes));
			dumpACL(fout, "LANGUAGE", tmp, lanname,
					finfo[fidx].pronamespace->nspname,
					NULL, lanacl, lanoid);
			free(tmp);
		}
	}

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(defqry);
	destroyPQExpBuffer(delqry);
}

/*
 * dumpFuncs
 *	  writes out to fout the queries to recreate all the user-defined functions
 */
void
dumpFuncs(Archive *fout, FuncInfo finfo[], int numFuncs)
{
	int			i;

	for (i = 0; i < numFuncs; i++)
	{
		/* Dump only funcs in dumpable namespaces */
		if (!finfo[i].pronamespace->dump)
			continue;

		dumpOneFunc(fout, &finfo[i]);
		if (!aclsSkip)
			dumpFuncACL(fout, &finfo[i]);
	}
}

/*
 * format_function_signature: generate function name and argument list
 *
 * The argument type names are qualified if needed.  The function name
 * is never qualified.
 */
static char *
format_function_signature(FuncInfo *finfo, bool honor_quotes)
{
	PQExpBufferData fn;
	int			j;

	initPQExpBuffer(&fn);
	if (honor_quotes)
		appendPQExpBuffer(&fn, "%s(", fmtId(finfo->proname, force_quotes));
	else
		appendPQExpBuffer(&fn, "%s(", finfo->proname);
	for (j = 0; j < finfo->nargs; j++)
	{
		char	   *typname;

		typname = getFormattedTypeName(finfo->argtypes[j], zeroAsOpaque);
		appendPQExpBuffer(&fn, "%s%s",
						  (j > 0) ? "," : "",
						  typname);
		free(typname);
	}
	appendPQExpBuffer(&fn, ")");
	return fn.data;
}


static void
dumpFuncACL(Archive *fout, FuncInfo *finfo)
{
	char *funcsig, *funcsig_noquotes;

	funcsig = format_function_signature(finfo, true);
	funcsig_noquotes = format_function_signature(finfo, false);
	dumpACL(fout, "FUNCTION", funcsig, funcsig_noquotes,
			finfo->pronamespace->nspname,
			finfo->usename, finfo->proacl, finfo->oid);
	free(funcsig);
	free(funcsig_noquotes);
}


/*
 * dumpOneFunc:
 *	  dump out only one function
 */
static void
dumpOneFunc(Archive *fout, FuncInfo *finfo)
{
	PQExpBuffer query = createPQExpBuffer();
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delqry = createPQExpBuffer();
	PQExpBuffer asPart = createPQExpBuffer();
	PGresult   *res = NULL;
	char	   *funcsig = NULL;
	char	   *funcsig_noquotes = NULL;
	int			ntups;
	char	   *proretset;
	char	   *prosrc;
	char	   *probin;
	char	   *provolatile;
	char	   *proimplicit;
	char	   *proisstrict;
	char	   *prosecdef;
	char	   *lanname;
	char	   *rettypename;

	if (finfo->dumped)
		goto done;

	finfo->dumped = true;

	/* Set proper schema search path so type references list correctly */
	selectSourceSchema(finfo->pronamespace->nspname);

	/* Fetch function-specific details */
	if (g_fout->remoteVersion >= 70300)
	{
		appendPQExpBuffer(query,
						  "SELECT proretset, prosrc, probin, "
						  "provolatile, proimplicit, proisstrict, prosecdef, "
						  "(SELECT lanname FROM pg_catalog.pg_language WHERE oid = prolang) as lanname "
						  "FROM pg_catalog.pg_proc "
						  "WHERE oid = '%s'::pg_catalog.oid",
						  finfo->oid);
	}
	else if (g_fout->remoteVersion >= 70100)
	{
		appendPQExpBuffer(query,
						  "SELECT proretset, prosrc, probin, "
						  "case when proiscachable then 'i' else 'v' end as provolatile, "
						  "'f'::boolean as proimplicit, "
						  "proisstrict, "
						  "'f'::boolean as prosecdef, "
						  "(SELECT lanname FROM pg_language WHERE oid = prolang) as lanname "
						  "FROM pg_proc "
						  "WHERE oid = '%s'::oid",
						  finfo->oid);
	}
	else
	{
		appendPQExpBuffer(query,
						  "SELECT proretset, prosrc, probin, "
						  "case when proiscachable then 'i' else 'v' end as provolatile, "
						  "'f'::boolean as proimplicit, "
						  "'f'::boolean as proisstrict, "
						  "'f'::boolean as prosecdef, "
						  "(SELECT lanname FROM pg_language WHERE oid = prolang) as lanname "
						  "FROM pg_proc "
						  "WHERE oid = '%s'::oid",
						  finfo->oid);
	}

	res = PQexec(g_conn, query->data);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to obtain function information for %s failed: %s",
				  finfo->proname, PQerrorMessage(g_conn));
		exit_nicely();
	}

	/* Expecting a single result only */
	ntups = PQntuples(res);
	if (ntups != 1)
	{
		write_msg(NULL, "Got %d rows instead of one from: %s",
				  ntups, query->data);
		exit_nicely();
	}

	proretset = PQgetvalue(res, 0, PQfnumber(res, "proretset"));
	prosrc = PQgetvalue(res, 0, PQfnumber(res, "prosrc"));
	probin = PQgetvalue(res, 0, PQfnumber(res, "probin"));
	provolatile = PQgetvalue(res, 0, PQfnumber(res, "provolatile"));
	proimplicit = PQgetvalue(res, 0, PQfnumber(res, "proimplicit"));
	proisstrict = PQgetvalue(res, 0, PQfnumber(res, "proisstrict"));
	prosecdef = PQgetvalue(res, 0, PQfnumber(res, "prosecdef"));
	lanname = PQgetvalue(res, 0, PQfnumber(res, "lanname"));

	/*
	 * See backend/commands/define.c for details of how the 'AS' clause is
	 * used.
	 */
	if (strcmp(probin, "-") != 0)
	{
		appendPQExpBuffer(asPart, "AS ");
		formatStringLiteral(asPart, probin, CONV_ALL);
		if (strcmp(prosrc, "-") != 0)
		{
			appendPQExpBuffer(asPart, ", ");
			formatStringLiteral(asPart, prosrc, PASS_LFTAB);
		}
	}
	else
	{
		if (strcmp(prosrc, "-") != 0)
		{
			appendPQExpBuffer(asPart, "AS ");
			formatStringLiteral(asPart, prosrc, PASS_LFTAB);
		}
	}

	funcsig = format_function_signature(finfo, true);
	funcsig_noquotes = format_function_signature(finfo, false);

	/* DROP must be fully qualified in case same name appears in pg_catalog */
	appendPQExpBuffer(delqry, "DROP FUNCTION %s.%s;\n",
					  fmtId(finfo->pronamespace->nspname, force_quotes),
					  funcsig);

	rettypename = getFormattedTypeName(finfo->prorettype, zeroAsOpaque);

	appendPQExpBuffer(q, "CREATE FUNCTION %s ", funcsig);
	appendPQExpBuffer(q, "RETURNS %s%s %s LANGUAGE %s",
					  (proretset[0] == 't') ? "SETOF " : "",
					  rettypename,
					  asPart->data,
					  fmtId(lanname, force_quotes));

	free(rettypename);

	if (provolatile[0] != PROVOLATILE_VOLATILE)
	{
		if (provolatile[0] == PROVOLATILE_IMMUTABLE)
			appendPQExpBuffer(q, " IMMUTABLE");
		else if (provolatile[0] == PROVOLATILE_STABLE)
			appendPQExpBuffer(q, " STABLE");
		else if (provolatile[0] != PROVOLATILE_VOLATILE)
		{
			write_msg(NULL, "Unexpected provolatile value for function %s\n",
					  finfo->proname);
			exit_nicely();
		}
	}

	if (proimplicit[0] == 't')
		appendPQExpBuffer(q, " IMPLICIT CAST");

	if (proisstrict[0] == 't')
		appendPQExpBuffer(q, " STRICT");

	if (prosecdef[0] == 't')
		appendPQExpBuffer(q, " SECURITY DEFINER");

	appendPQExpBuffer(q, ";\n");

	ArchiveEntry(fout, finfo->oid, funcsig_noquotes,
				 finfo->pronamespace->nspname,
				 finfo->usename, "FUNCTION", NULL,
				 q->data, delqry->data,
				 NULL, NULL, NULL);

	/*** Dump Function Comments ***/

	resetPQExpBuffer(q);
	appendPQExpBuffer(q, "FUNCTION %s", funcsig);
	dumpComment(fout, q->data,
				finfo->pronamespace->nspname, finfo->usename,
				finfo->oid, "pg_proc", 0, NULL);

done:
	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delqry);
	destroyPQExpBuffer(asPart);
	free(funcsig);
	free(funcsig_noquotes);
}

/*
 * dumpOprs
 *	  writes out to fout the queries to recreate all the user-defined operators
 */
void
dumpOprs(Archive *fout, OprInfo *oprinfo, int numOperators)
{
	int			i;

	for (i = 0; i < numOperators; i++)
	{
		/* Dump only operators in dumpable namespaces */
		if (!oprinfo[i].oprnamespace->dump)
			continue;

		/*
		 * some operators are invalid because they were the result of user
		 * defining operators before commutators exist
		 */
		if (strcmp(oprinfo[i].oprcode, "0") == 0)
			continue;

		/* OK, dump it */
		dumpOneOpr(fout, &oprinfo[i],
				   oprinfo, numOperators);
	}
}

/*
 * dumpOneOpr
 *	  write out a single operator definition
 */
static void
dumpOneOpr(Archive *fout, OprInfo *oprinfo,
		   OprInfo *g_oprinfo, int numOperators)
{
	PQExpBuffer query = createPQExpBuffer();
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	PQExpBuffer oprid = createPQExpBuffer();
	PQExpBuffer details = createPQExpBuffer();
	const char *name;
	PGresult   *res;
	int			ntups;
	int			i_oprkind;
	int			i_oprcode;
	int			i_oprleft;
	int			i_oprright;
	int			i_oprcom;
	int			i_oprnegate;
	int			i_oprrest;
	int			i_oprjoin;
	int			i_oprcanhash;
	int			i_oprlsortop;
	int			i_oprrsortop;
	int			i_oprltcmpop;
	int			i_oprgtcmpop;
	char	   *oprkind;
	char	   *oprcode;
	char	   *oprleft;
	char	   *oprright;
	char	   *oprcom;
	char	   *oprnegate;
	char	   *oprrest;
	char	   *oprjoin;
	char	   *oprcanhash;
	char	   *oprlsortop;
	char	   *oprrsortop;
	char	   *oprltcmpop;
	char	   *oprgtcmpop;

	/* Make sure we are in proper schema so regoperator works correctly */
	selectSourceSchema(oprinfo->oprnamespace->nspname);

	if (g_fout->remoteVersion >= 70300)
	{
		appendPQExpBuffer(query, "SELECT oprkind, "
						  "oprcode::pg_catalog.regprocedure, "
						  "oprleft::pg_catalog.regtype, "
						  "oprright::pg_catalog.regtype, "
						  "oprcom::pg_catalog.regoperator, "
						  "oprnegate::pg_catalog.regoperator, "
						  "oprrest::pg_catalog.regprocedure, "
						  "oprjoin::pg_catalog.regprocedure, "
						  "oprcanhash, "
						  "oprlsortop::pg_catalog.regoperator, "
						  "oprrsortop::pg_catalog.regoperator, "
						  "oprltcmpop::pg_catalog.regoperator, "
						  "oprgtcmpop::pg_catalog.regoperator "
						  "from pg_catalog.pg_operator "
						  "where oid = '%s'::pg_catalog.oid",
						  oprinfo->oid);
	}
	else if (g_fout->remoteVersion >= 70100)
	{
		appendPQExpBuffer(query, "SELECT oprkind, oprcode, "
						  "CASE WHEN oprleft = 0 THEN '-' "
						  "ELSE format_type(oprleft, NULL) END as oprleft, "
						  "CASE WHEN oprright = 0 THEN '-' "
						  "ELSE format_type(oprright, NULL) END as oprright, "
						  "oprcom, oprnegate, oprrest, oprjoin, "
						  "oprcanhash, oprlsortop, oprrsortop, "
						  "0 as oprltcmpop, 0 as oprgtcmpop "
						  "from pg_operator "
						  "where oid = '%s'::oid",
						  oprinfo->oid);
	}
	else
	{
		appendPQExpBuffer(query, "SELECT oprkind, oprcode, "
						  "CASE WHEN oprleft = 0 THEN '-'::name "
						  "ELSE (select typname from pg_type where oid = oprleft) END as oprleft, "
						  "CASE WHEN oprright = 0 THEN '-'::name "
						  "ELSE (select typname from pg_type where oid = oprright) END as oprright, "
						  "oprcom, oprnegate, oprrest, oprjoin, "
						  "oprcanhash, oprlsortop, oprrsortop, "
						  "0 as oprltcmpop, 0 as oprgtcmpop "
						  "from pg_operator "
						  "where oid = '%s'::oid",
						  oprinfo->oid);
	}

	res = PQexec(g_conn, query->data);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to obtain list of operators failed: %s", PQerrorMessage(g_conn));
		exit_nicely();
	}

	/* Expecting a single result only */
	ntups = PQntuples(res);
	if (ntups != 1)
	{
		write_msg(NULL, "Got %d rows instead of one from: %s",
				  ntups, query->data);
		exit_nicely();
	}

	i_oprkind = PQfnumber(res, "oprkind");
	i_oprcode = PQfnumber(res, "oprcode");
	i_oprleft = PQfnumber(res, "oprleft");
	i_oprright = PQfnumber(res, "oprright");
	i_oprcom = PQfnumber(res, "oprcom");
	i_oprnegate = PQfnumber(res, "oprnegate");
	i_oprrest = PQfnumber(res, "oprrest");
	i_oprjoin = PQfnumber(res, "oprjoin");
	i_oprcanhash = PQfnumber(res, "oprcanhash");
	i_oprlsortop = PQfnumber(res, "oprlsortop");
	i_oprrsortop = PQfnumber(res, "oprrsortop");
	i_oprltcmpop = PQfnumber(res, "oprltcmpop");
	i_oprgtcmpop = PQfnumber(res, "oprgtcmpop");

	oprkind = PQgetvalue(res, 0, i_oprkind);
	oprcode = PQgetvalue(res, 0, i_oprcode);
	oprleft = PQgetvalue(res, 0, i_oprleft);
	oprright = PQgetvalue(res, 0, i_oprright);
	oprcom = PQgetvalue(res, 0, i_oprcom);
	oprnegate = PQgetvalue(res, 0, i_oprnegate);
	oprrest = PQgetvalue(res, 0, i_oprrest);
	oprjoin = PQgetvalue(res, 0, i_oprjoin);
	oprcanhash = PQgetvalue(res, 0, i_oprcanhash);
	oprlsortop = PQgetvalue(res, 0, i_oprlsortop);
	oprrsortop = PQgetvalue(res, 0, i_oprrsortop);
	oprltcmpop = PQgetvalue(res, 0, i_oprltcmpop);
	oprgtcmpop = PQgetvalue(res, 0, i_oprgtcmpop);

	appendPQExpBuffer(details, "PROCEDURE = %s ",
					  convertRegProcReference(oprcode));

	appendPQExpBuffer(oprid, "%s (",
					  oprinfo->oprname);

	/*
	 * right unary means there's a left arg and left unary means
	 * there's a right arg
	 */
	if (strcmp(oprkind, "r") == 0 ||
		strcmp(oprkind, "b") == 0)
	{
		if (g_fout->remoteVersion >= 70100)
			name = oprleft;
		else
			name = fmtId(oprleft, force_quotes);
		appendPQExpBuffer(details, ",\n\tLEFTARG = %s ", name);
		appendPQExpBuffer(oprid, "%s", name);
	}
	else
		appendPQExpBuffer(oprid, "NONE");

	if (strcmp(oprkind, "l") == 0 ||
		strcmp(oprkind, "b") == 0)
	{
		if (g_fout->remoteVersion >= 70100)
			name = oprright;
		else
			name = fmtId(oprright, force_quotes);
		appendPQExpBuffer(details, ",\n\tRIGHTARG = %s ", name);
		appendPQExpBuffer(oprid, ", %s)", name);
	}
	else
		appendPQExpBuffer(oprid, ", NONE)");

	name = convertOperatorReference(oprcom, g_oprinfo, numOperators);
	if (name)
		appendPQExpBuffer(details, ",\n\tCOMMUTATOR = %s ", name);

	name = convertOperatorReference(oprnegate, g_oprinfo, numOperators);
	if (name)
		appendPQExpBuffer(details, ",\n\tNEGATOR = %s ", name);

	if (strcmp(oprcanhash, "t") == 0)
		appendPQExpBuffer(details, ",\n\tHASHES");

	name = convertRegProcReference(oprrest);
	if (name)
		appendPQExpBuffer(details, ",\n\tRESTRICT = %s ", name);

	name = convertRegProcReference(oprjoin);
	if (name)
		appendPQExpBuffer(details, ",\n\tJOIN = %s ", name);

	name = convertOperatorReference(oprlsortop, g_oprinfo, numOperators);
	if (name)
		appendPQExpBuffer(details, ",\n\tSORT1 = %s ", name);

	name = convertOperatorReference(oprrsortop, g_oprinfo, numOperators);
	if (name)
		appendPQExpBuffer(details, ",\n\tSORT2 = %s ", name);

	name = convertOperatorReference(oprltcmpop, g_oprinfo, numOperators);
	if (name)
		appendPQExpBuffer(details, ",\n\tLTCMP = %s ", name);

	name = convertOperatorReference(oprgtcmpop, g_oprinfo, numOperators);
	if (name)
		appendPQExpBuffer(details, ",\n\tGTCMP = %s ", name);

	/* DROP must be fully qualified in case same name appears in pg_catalog */
	appendPQExpBuffer(delq, "DROP OPERATOR %s.%s;\n",
					  fmtId(oprinfo->oprnamespace->nspname, force_quotes),
					  oprid->data);

	appendPQExpBuffer(q, "CREATE OPERATOR %s (%s);\n",
					  oprinfo->oprname, details->data);

	ArchiveEntry(fout, oprinfo->oid, oprinfo->oprname,
				 oprinfo->oprnamespace->nspname, oprinfo->usename,
				 "OPERATOR", NULL,
				 q->data, delq->data,
				 NULL, NULL, NULL);

	/*** Dump Operator Comments ***/

	resetPQExpBuffer(q);
	appendPQExpBuffer(q, "OPERATOR %s", oprid->data);
	dumpComment(fout, q->data,
				oprinfo->oprnamespace->nspname, oprinfo->usename,
				oprinfo->oid, "pg_operator", 0, NULL);

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(oprid);
	destroyPQExpBuffer(details);
}

/*
 * Convert a function reference obtained from pg_operator
 *
 * Returns what to print, or NULL if function references is InvalidOid
 *
 * In 7.3 the input is a REGPROCEDURE display; we have to strip the
 * argument-types part.  In prior versions, the input is a REGPROC display.
 */
static const char *
convertRegProcReference(const char *proc)
{
	/* In all cases "-" means a null reference */
	if (strcmp(proc, "-") == 0)
		return NULL;

	if (g_fout->remoteVersion >= 70300)
	{
		char   *name;
		char   *paren;
		bool	inquote;

		name = strdup(proc);
		/* find non-double-quoted left paren */
		inquote = false;
		for (paren = name; *paren; paren++)
		{
			if (*paren == '(' && !inquote)
			{
				*paren = '\0';
				break;
			}
			if (*paren == '"')
				inquote = !inquote;
		}
		return name;
	}

	/* REGPROC before 7.3 does not quote its result */
	return fmtId(proc, false);
}

/*
 * Convert an operator cross-reference obtained from pg_operator
 *
 * Returns what to print, or NULL to print nothing
 *
 * In 7.3 the input is a REGOPERATOR display; we have to strip the
 * argument-types part.  In prior versions, the input is just a
 * numeric OID, which we search our operator list for.
 */
static const char *
convertOperatorReference(const char *opr,
						 OprInfo *g_oprinfo, int numOperators)
{
	char   *name;

	/* In all cases "0" means a null reference */
	if (strcmp(opr, "0") == 0)
		return NULL;

	if (g_fout->remoteVersion >= 70300)
	{
		char   *paren;
		bool	inquote;

		name = strdup(opr);
		/* find non-double-quoted left paren */
		inquote = false;
		for (paren = name; *paren; paren++)
		{
			if (*paren == '(' && !inquote)
			{
				*paren = '\0';
				break;
			}
			if (*paren == '"')
				inquote = !inquote;
		}
		return name;
	}

	name = findOprByOid(g_oprinfo, numOperators, opr);
	if (name == NULL)
		write_msg(NULL, "WARNING: cannot find operator with OID %s\n",
				  opr);
	return name;
}

/*
 * dumpAggs
 *	  writes out to fout the queries to create all the user-defined aggregates
 */
void
dumpAggs(Archive *fout, AggInfo agginfo[], int numAggs)
{
	int			i;

	for (i = 0; i < numAggs; i++)
	{
		/* Dump only aggs in dumpable namespaces */
		if (!agginfo[i].aggnamespace->dump)
			continue;

		dumpOneAgg(fout, &agginfo[i]);
		if (!aclsSkip)
			dumpAggACL(fout, &agginfo[i]);
	}
}


/*
 * format_aggregate_signature: generate aggregate name and argument list
 *
 * The argument type names are qualified if needed.  The aggregate name
 * is never qualified.
 */
static char *
format_aggregate_signature(AggInfo *agginfo, Archive *fout, bool honor_quotes)
{
	PQExpBufferData buf;
	bool anybasetype;

	initPQExpBuffer(&buf);
	if (honor_quotes)
		appendPQExpBuffer(&buf, "%s",
					  fmtId(agginfo->aggname, force_quotes));
	else
		appendPQExpBuffer(&buf, "%s", agginfo->aggname);

	anybasetype = (strcmp(agginfo->aggbasetype, "0") == 0);

	/* If using regtype or format_type, fmtbasetype is already quoted */
	if (fout->remoteVersion >= 70100)
	{
		if (anybasetype)
			appendPQExpBuffer(&buf, "(*)");
		else
			appendPQExpBuffer(&buf, "(%s)", agginfo->fmtbasetype);
	}
	else
	{
		if (anybasetype)
			appendPQExpBuffer(&buf, "(*)");
		else
			appendPQExpBuffer(&buf, "(%s)",
							  fmtId(agginfo->fmtbasetype, force_quotes));
	}

	return buf.data;
}


static void
dumpAggACL(Archive *fout, AggInfo *finfo)
{
	char *aggsig, *aggsig_noquotes;

	aggsig = format_aggregate_signature(finfo, fout, true);
	aggsig_noquotes = format_aggregate_signature(finfo, fout, false);
	dumpACL(fout, "FUNCTION", aggsig, aggsig_noquotes,
			finfo->aggnamespace->nspname,
			finfo->usename, finfo->aggacl, finfo->oid);
	free(aggsig);
	free(aggsig_noquotes);
}


/*
 * dumpOneAgg
 *	  write out a single aggregate definition
 */
static void
dumpOneAgg(Archive *fout, AggInfo *agginfo)
{
	PQExpBuffer query = createPQExpBuffer();
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	PQExpBuffer details = createPQExpBuffer();
	char	   *aggsig;
	char	   *aggsig_noquotes;
	PGresult   *res;
	int			ntups;
	int			i_aggtransfn;
	int			i_aggfinalfn;
	int			i_aggtranstype;
	int			i_agginitval;
	int			i_fmtbasetype;
	int			i_convertok;
	const char *aggtransfn;
	const char *aggfinalfn;
	const char *aggtranstype;
	const char *agginitval;
	bool		convertok;
	bool		anybasetype;

	/* Make sure we are in proper schema */
	selectSourceSchema(agginfo->aggnamespace->nspname);

	/* Get aggregate-specific details */
	if (g_fout->remoteVersion >= 70300)
	{
		appendPQExpBuffer(query, "SELECT aggtransfn, "
						  "aggfinalfn, aggtranstype::pg_catalog.regtype, "
						  "agginitval, "
						  "proargtypes[0]::pg_catalog.regtype as fmtbasetype, "
						  "'t'::boolean as convertok "
						  "from pg_catalog.pg_aggregate a, pg_catalog.pg_proc p "
						  "where a.aggfnoid = p.oid "
						  "and p.oid = '%s'::pg_catalog.oid",
						  agginfo->oid);
	}
	else if (g_fout->remoteVersion >= 70100)
	{
		appendPQExpBuffer(query, "SELECT aggtransfn, aggfinalfn, "
						  "format_type(aggtranstype, NULL) as aggtranstype, "
						  "agginitval, "
						  "CASE WHEN aggbasetype = 0 THEN '-' "
						  "ELSE format_type(aggbasetype, NULL) END as fmtbasetype, "
						  "'t'::boolean as convertok "
						  "from pg_aggregate "
						  "where oid = '%s'::oid",
						  agginfo->oid);
	}
	else
	{
		appendPQExpBuffer(query, "SELECT aggtransfn1 as aggtransfn, "
						  "aggfinalfn, "
						  "(select typname from pg_type where oid = aggtranstype1) as aggtranstype, "
						  "agginitval1 as agginitval, "
						  "(select typname from pg_type where oid = aggbasetype) as fmtbasetype, "
						  "(aggtransfn2 = 0 and aggtranstype2 = 0 and agginitval2 is null) as convertok "
						  "from pg_aggregate "
						  "where oid = '%s'::oid",
						  agginfo->oid);
	}

	res = PQexec(g_conn, query->data);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to obtain list of aggregate functions failed: %s",
				  PQerrorMessage(g_conn));
		exit_nicely();
	}

	/* Expecting a single result only */
	ntups = PQntuples(res);
	if (ntups != 1)
	{
		write_msg(NULL, "Got %d rows instead of one from: %s",
				  ntups, query->data);
		exit_nicely();
	}

	i_aggtransfn = PQfnumber(res, "aggtransfn");
	i_aggfinalfn = PQfnumber(res, "aggfinalfn");
	i_aggtranstype = PQfnumber(res, "aggtranstype");
	i_agginitval = PQfnumber(res, "agginitval");
	i_fmtbasetype = PQfnumber(res, "fmtbasetype");
	i_convertok = PQfnumber(res, "convertok");

	aggtransfn = PQgetvalue(res, 0, i_aggtransfn);
	aggfinalfn = PQgetvalue(res, 0, i_aggfinalfn);
	aggtranstype = PQgetvalue(res, 0, i_aggtranstype);
	agginitval = PQgetvalue(res, 0, i_agginitval);
	/* we save fmtbasetype so that dumpAggACL can use it later */
	agginfo->fmtbasetype = strdup(PQgetvalue(res, 0, i_fmtbasetype));
	convertok = (PQgetvalue(res, 0, i_convertok)[0] == 't');

	aggsig = format_aggregate_signature(agginfo, g_fout, true);
 	aggsig_noquotes = format_aggregate_signature(agginfo, g_fout, false);

	if (!convertok)
	{
		write_msg(NULL, "WARNING: aggregate function %s could not be dumped correctly for this database version; ignored\n",
				  aggsig);

		appendPQExpBuffer(q, "-- WARNING: aggregate function %s could not be dumped correctly for this database version; ignored\n",
						  aggsig);
		ArchiveEntry(fout, agginfo->oid, aggsig_noquotes,
					 agginfo->aggnamespace->nspname, agginfo->usename,
					 "WARNING", NULL,
					 q->data, "" /* Del */ ,
					 NULL, NULL, NULL);
		return;
	}

	anybasetype = (strcmp(agginfo->aggbasetype, "0") == 0);

	if (g_fout->remoteVersion >= 70300)
	{
		/* If using 7.3's regproc or regtype, data is already quoted */
		appendPQExpBuffer(details, "BASETYPE = %s, SFUNC = %s, STYPE = %s",
						  anybasetype ? "'any'" : agginfo->fmtbasetype,
						  aggtransfn,
						  aggtranstype);
	}
	else if (g_fout->remoteVersion >= 70100)
	{
		/* format_type quotes, regproc does not */
		appendPQExpBuffer(details, "BASETYPE = %s, SFUNC = %s, STYPE = %s",
						  anybasetype ? "'any'" : agginfo->fmtbasetype,
						  fmtId(aggtransfn, force_quotes),
						  aggtranstype);
	}
	else
	{
		/* need quotes all around */
		appendPQExpBuffer(details, "BASETYPE = %s, ",
						  anybasetype ? "'any'" :
						  fmtId(agginfo->fmtbasetype, force_quotes));
		appendPQExpBuffer(details, "SFUNC = %s, ",
						  fmtId(aggtransfn, force_quotes));
		appendPQExpBuffer(details, "STYPE = %s",
						  fmtId(aggtranstype, force_quotes));
	}

	if (!PQgetisnull(res, 0, i_agginitval))
	{
		appendPQExpBuffer(details, ", INITCOND = ");
		formatStringLiteral(details, agginitval, CONV_ALL);
	}

	if (strcmp(aggfinalfn, "-") != 0)
	{
		appendPQExpBuffer(details, ", FINALFUNC = %s",
						  aggfinalfn);
	}

	/* DROP must be fully qualified in case same name appears in pg_catalog */
	appendPQExpBuffer(delq, "DROP AGGREGATE %s.%s;\n",
					  fmtId(agginfo->aggnamespace->nspname, force_quotes),
					  aggsig);

	appendPQExpBuffer(q, "CREATE AGGREGATE %s ( %s );\n",
					  fmtId(agginfo->aggname, force_quotes),
					  details->data);

	ArchiveEntry(fout, agginfo->oid, aggsig_noquotes,
				 agginfo->aggnamespace->nspname, agginfo->usename,
				 "AGGREGATE", NULL,
				 q->data, delq->data,
				 NULL, NULL, NULL);

	/*** Dump Aggregate Comments ***/

	resetPQExpBuffer(q);
	appendPQExpBuffer(q, "AGGREGATE %s", aggsig);
	if (g_fout->remoteVersion >= 70300)
		dumpComment(fout, q->data,
					agginfo->aggnamespace->nspname, agginfo->usename,
					agginfo->oid, "pg_proc", 0, NULL);
	else
		dumpComment(fout, q->data,
					agginfo->aggnamespace->nspname, agginfo->usename,
					agginfo->oid, "pg_aggregate", 0, NULL);

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(details);
	free(aggsig);
	free(aggsig_noquotes);
}


/*
 * These are some support functions to fix the acl problem of pg_dump
 *
 * Matthew C. Aycock 12/02/97
 */

/* Append a keyword to a keyword list, inserting comma if needed.
 * Caller must make aclbuf big enough for all possible keywords.
 */
static void
AddAcl(char *aclbuf, const char *keyword)
{
	if (*aclbuf)
		strcat(aclbuf, ",");
	strcat(aclbuf, keyword);
}

/*
 * This will take a string of privilege code letters and return a malloced,
 * comma delimited string of keywords for GRANT.
 *
 * Note: for cross-version compatibility, it's important to use ALL when
 * appropriate.
 */
static char *
GetPrivileges(Archive *AH, const char *s, const char *type)
{
	char		aclbuf[100];
	bool		all = true;

	aclbuf[0] = '\0';

#define CONVERT_PRIV(code,keywd) \
	if (strchr(s, code)) \
		AddAcl(aclbuf, keywd); \
	else \
		all = false

	if (strcmp(type, "TABLE")==0)
	{
		CONVERT_PRIV('a', "INSERT");
		CONVERT_PRIV('r', "SELECT");
		CONVERT_PRIV('R', "RULE");

		if (AH->remoteVersion >= 70200)
		{
			CONVERT_PRIV('w', "UPDATE");
			CONVERT_PRIV('d', "DELETE");
			CONVERT_PRIV('x', "REFERENCES");
			CONVERT_PRIV('t', "TRIGGER");
		}
		else
		{
			/* 7.0 and 7.1 have a simpler worldview */
			CONVERT_PRIV('w', "UPDATE,DELETE");
		}
	}
	else if (strcmp(type, "FUNCTION")==0)
	{
		CONVERT_PRIV('X', "EXECUTE");
	}
	else if (strcmp(type, "LANGUAGE")==0)
	{
		CONVERT_PRIV('U', "USAGE");
	}
	else if (strcmp(type, "SCHEMA")==0)
	{
		CONVERT_PRIV('C', "CREATE");
		CONVERT_PRIV('U', "USAGE");
	}
	else
		abort();

#undef CONVERT_PRIV

	if (all)
		return strdup("ALL");
	else
		return strdup(aclbuf);
}


/*
 * Write out grant/revoke information
 *
 * 'type' must be TABLE, FUNCTION, LANGUAGE, or SCHEMA.  'name' is the
 * formatted name of the object.  Must be quoted etc. already.
 * 'nspname' is the namespace the object is in (NULL if none).
 * 'usename' is the owner, NULL if there is no owner (for languages).
 * 'acls' is the string read out of the fooacl system catalog field;
 * it will be parsed here.
 * 'objoid' is the OID of the object for purposes of ordering.
 */
static void
dumpACL(Archive *fout, const char *type, const char *name,
		const char *name_noquotes, const char *nspname, const char *usename,
		const char *acls, const char *objoid)
{
	char	   *aclbuf,
			   *tok,
			   *eqpos,
			   *priv;
	PQExpBuffer sql;
	bool		found_owner_privs = false;

	if (strlen(acls) == 0)
		return;					/* object has default permissions */

	sql = createPQExpBuffer();

	/* Make a working copy of acls so we can use strtok */
	aclbuf = strdup(acls);

	/* Scan comma-separated ACL items */
	for (tok = strtok(aclbuf, ","); tok != NULL; tok = strtok(NULL, ","))
	{
		/*
		 * Token may start with '{' and/or '"'.  Actually only the start
		 * of the string should have '{', but we don't verify that.
		 */
		if (*tok == '{')
			tok++;
		if (*tok == '"')
			tok++;

		/* User name is string up to = in tok */
		eqpos = strchr(tok, '=');
		if (!eqpos)
		{
			write_msg(NULL, "could not parse ACL list ('%s') for %s %s\n",
					  acls, type, name);
			exit_nicely();
		}
		*eqpos = '\0';			/* it's ok to clobber aclbuf */

		/*
		 * Parse the privileges (right-hand side).
		 */
		priv = GetPrivileges(fout, eqpos + 1, type);

		if (*priv)
		{
			if (usename && strcmp(tok, usename) == 0)
			{
				/*
				 * For the owner, the default privilege level is ALL.
				 */
				found_owner_privs = true;
				if (strcmp(priv, "ALL") != 0)
				{
					/* NB: only one fmtId per appendPQExpBuffer! */
					appendPQExpBuffer(sql, "REVOKE ALL ON %s %s FROM ",
									  type, name);
					appendPQExpBuffer(sql, "%s;\n", fmtId(tok, force_quotes));
					appendPQExpBuffer(sql, "GRANT %s ON %s %s TO ",
									  priv, type, name);
					appendPQExpBuffer(sql, "%s;\n", fmtId(tok, force_quotes));
				}
			}
			else
			{
				/*
				 * Otherwise can assume we are starting from no privs.
				 */
				appendPQExpBuffer(sql, "GRANT %s ON %s %s TO ",
								  priv, type, name);
				if (eqpos == tok)
				{
					/* Empty left-hand side means "PUBLIC" */
					appendPQExpBuffer(sql, "PUBLIC;\n");
				}
				else if (strncmp(tok, "group ", strlen("group ")) == 0)
					appendPQExpBuffer(sql, "GROUP %s;\n",
									  fmtId(tok + strlen("group "),
											force_quotes));
				else
					appendPQExpBuffer(sql, "%s;\n", fmtId(tok, force_quotes));
			}
		}
		else
		{
			/* No privileges.  Issue explicit REVOKE for safety. */
			appendPQExpBuffer(sql, "REVOKE ALL ON %s %s FROM ",
							  type, name);
			if (eqpos == tok)
			{
				/* Empty left-hand side means "PUBLIC" */
				appendPQExpBuffer(sql, "PUBLIC;\n");
			}
			else if (strncmp(tok, "group ", strlen("group ")) == 0)
				appendPQExpBuffer(sql, "GROUP %s;\n",
								  fmtId(tok + strlen("group "),
										force_quotes));
			else
				appendPQExpBuffer(sql, "%s;\n", fmtId(tok, force_quotes));
		}
		free(priv);
	}

	/*
	 * If we didn't find any owner privs, the owner must have revoked 'em all
	 */
	if (!found_owner_privs && usename)
	{
		appendPQExpBuffer(sql, "REVOKE ALL ON %s %s FROM ",
						  type, name);
		appendPQExpBuffer(sql, "%s;\n", fmtId(usename, force_quotes));
	}

	ArchiveEntry(fout, objoid, name_noquotes, nspname, usename ? usename : "",
				 "ACL", NULL, sql->data, "", NULL, NULL, NULL);

	free(aclbuf);
	destroyPQExpBuffer(sql);
}


static void
dumpTableACL(Archive *fout, TableInfo *tbinfo)
{
	char *tmp = strdup(fmtId(tbinfo->relname, force_quotes));
	dumpACL(fout, "TABLE", tmp, tbinfo->relname,
			tbinfo->relnamespace->nspname, tbinfo->usename, tbinfo->relacl,
			tbinfo->viewoid != NULL ? tbinfo->viewoid : tbinfo->oid);
	free(tmp);
}


/*
 * dumpTables:
 *	  write out to fout the declarations (not data) of all user-defined tables
 */
void
dumpTables(Archive *fout, TableInfo tblinfo[], int numTables,
		   const bool aclsSkip, const bool schemaOnly, const bool dataOnly)
{
	int			i;

	/* Dump sequences first, in case they are referenced in table defn's */
	for (i = 0; i < numTables; i++)
	{
		TableInfo	   *tbinfo = &tblinfo[i];

		if (tbinfo->relkind != RELKIND_SEQUENCE)
			continue;
		if (tbinfo->dump)
		{
			dumpOneSequence(fout, tbinfo, schemaOnly, dataOnly);
			if (!dataOnly && !aclsSkip)
				dumpTableACL(fout, tbinfo);
		}
	}

	if (!dataOnly)
	{
		for (i = 0; i < numTables; i++)
		{
			TableInfo	   *tbinfo = &tblinfo[i];

			if (tbinfo->relkind == RELKIND_SEQUENCE) /* already dumped */
				continue;

			if (tbinfo->dump)
			{
				dumpOneTable(fout, tbinfo, tblinfo);
				if (!aclsSkip)
					dumpTableACL(fout, tbinfo);
			}
		}
	}
}

/*
 * dumpOneTable
 *	  write the declaration (not data) of one user-defined table or view
 */
static void
dumpOneTable(Archive *fout, TableInfo *tbinfo, TableInfo *g_tblinfo)
{
	PQExpBuffer query = createPQExpBuffer();
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	PGresult   *res;
	int			numParents;
	int		   *parentIndexes;
	int			actual_atts;	/* number of attrs in this CREATE statment */
	char	   *reltypename;
	char	   *objoid;
	const char *((*commentDeps)[]);
	int			j,
				k;

	/* Make sure we are in proper schema */
	selectSourceSchema(tbinfo->relnamespace->nspname);

	/* Is it a table or a view? */
	if (tbinfo->relkind == RELKIND_VIEW)
	{
		char	   *viewdef;

		reltypename = "VIEW";

		/* Fetch the view definition */
		if (g_fout->remoteVersion >= 70300)
		{
			/* Beginning in 7.3, viewname is not unique; use OID */
			appendPQExpBuffer(query, "SELECT pg_catalog.pg_get_viewdef(ev_class) as viewdef, "
							  "oid as view_oid"
							  " from pg_catalog.pg_rewrite where"
							  " ev_class = '%s'::pg_catalog.oid and"
							  " rulename = '_RETURN';",
							  tbinfo->oid);
		}
		else
		{
			appendPQExpBuffer(query, "SELECT definition as viewdef, "
							  "(select oid from pg_rewrite where "
							  " rulename=('_RET' || viewname)::name) as view_oid"
							  " from pg_views where viewname = ");
			formatStringLiteral(query, tbinfo->relname, CONV_ALL);
			appendPQExpBuffer(query, ";");
		}

		res = PQexec(g_conn, query->data);
		if (!res || PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			write_msg(NULL, "query to obtain definition of view \"%s\" failed: %s",
					  tbinfo->relname, PQerrorMessage(g_conn));
			exit_nicely();
		}

		if (PQntuples(res) != 1)
		{
			if (PQntuples(res) < 1)
				write_msg(NULL, "query to obtain definition of view \"%s\" returned no data\n",
						  tbinfo->relname);
			else
				write_msg(NULL, "query to obtain definition of view \"%s\" returned more than one definition\n",
						  tbinfo->relname);
			exit_nicely();
		}

		if (PQgetisnull(res, 0, 1))
		{
			write_msg(NULL, "query to obtain definition of view \"%s\" returned NULL oid\n",
					  tbinfo->relname);
			exit_nicely();
		}

		viewdef = PQgetvalue(res, 0, 0);

		if (strlen(viewdef) == 0)
		{
			write_msg(NULL, "definition of view \"%s\" appears to be empty (length zero)\n",
					  tbinfo->relname);
			exit_nicely();
		}

		/* We use the OID of the view rule as the object OID */
		objoid = strdup(PQgetvalue(res, 0, 1));
		/* Save it for use by dumpACL, too */
		tbinfo->viewoid = objoid;

		/* DROP must be fully qualified in case same name appears in pg_catalog */
		appendPQExpBuffer(delq, "DROP VIEW %s.",
						  fmtId(tbinfo->relnamespace->nspname, force_quotes));
		appendPQExpBuffer(delq, "%s;\n",
						  fmtId(tbinfo->relname, force_quotes));

		appendPQExpBuffer(q, "CREATE VIEW %s AS %s\n",
						  fmtId(tbinfo->relname, force_quotes), viewdef);

		PQclear(res);

		/*
		 * Views can have default values -- however, they must be
		 * specified in an ALTER TABLE command after the view has
		 * been created, not in the view definition itself.
		 */
		for (j = 0; j < tbinfo->numatts; j++)
		{
			if (tbinfo->adef_expr[j] != NULL && !tbinfo->inhAttrDef[j])
			{
				appendPQExpBuffer(q, "ALTER TABLE %s ",
								  fmtId(tbinfo->relname, force_quotes));
				appendPQExpBuffer(q, "ALTER COLUMN %s SET DEFAULT %s;\n",
								  fmtId(tbinfo->attnames[j], force_quotes),
								  tbinfo->adef_expr[j]);
			}
		}

		commentDeps = malloc(sizeof(char *) * 2);
		(*commentDeps)[0] = strdup(objoid);
		(*commentDeps)[1] = NULL;		/* end of list */
	}
	else
	{
		reltypename = "TABLE";
		objoid = tbinfo->oid;
		commentDeps = NULL;
		numParents = tbinfo->numParents;
		parentIndexes = tbinfo->parentIndexes;

		/* DROP must be fully qualified in case same name appears in pg_catalog */
		appendPQExpBuffer(delq, "DROP TABLE %s.",
						  fmtId(tbinfo->relnamespace->nspname, force_quotes));
		appendPQExpBuffer(delq, "%s;\n",
						  fmtId(tbinfo->relname, force_quotes));

		appendPQExpBuffer(q, "CREATE TABLE %s (\n\t",
						  fmtId(tbinfo->relname, force_quotes));
		actual_atts = 0;
		for (j = 0; j < tbinfo->numatts; j++)
		{
			/* Is this one of the table's own attrs ? */
			if (!tbinfo->inhAttrs[j])
			{
				/* Format properly if not first attr */
				if (actual_atts > 0)
					appendPQExpBuffer(q, ",\n\t");

				/* Attr name & type */
				appendPQExpBuffer(q, "%s ",
								  fmtId(tbinfo->attnames[j], force_quotes));

				/* If no format_type, fake it */
				if (g_fout->remoteVersion >= 70100)
					appendPQExpBuffer(q, "%s", tbinfo->atttypnames[j]);
				else
					appendPQExpBuffer(q, "%s",
									  myFormatType(tbinfo->atttypnames[j],
												   tbinfo->atttypmod[j]));

				/* Default value */
				if (tbinfo->adef_expr[j] != NULL && !tbinfo->inhAttrDef[j])
					appendPQExpBuffer(q, " DEFAULT %s",
									  tbinfo->adef_expr[j]);

				/* Not Null constraint */
				if (tbinfo->notnull[j] && !tbinfo->inhNotNull[j])
					appendPQExpBuffer(q, " NOT NULL");

				actual_atts++;
			}
		}

		/*
		 * Add non-inherited CHECK constraints, if any. If a
		 * constraint matches by name and condition with a constraint
		 * belonging to a parent class (OR conditions match and both names
		 * start with '$'), we assume it was inherited.
		 */
		if (tbinfo->ncheck > 0)
		{
			PGresult   *res2;
			int			i_rcname,
						i_rcsrc;
			int			ntups2;

			if (g_verbose)
				write_msg(NULL, "finding CHECK constraints for table %s\n",
						  tbinfo->relname);

			resetPQExpBuffer(query);
			if (g_fout->remoteVersion >= 70300)
				appendPQExpBuffer(query, "SELECT rcname, rcsrc"
								  " from pg_catalog.pg_relcheck c1"
								  " where rcrelid = '%s'::pg_catalog.oid "
								  "   and not exists "
								  "  (select 1 from "
								  "    pg_catalog.pg_relcheck c2, "
								  "    pg_catalog.pg_inherits i "
								  "    where i.inhrelid = c1.rcrelid "
								  "      and (c2.rcname = c1.rcname "
								  "          or (c2.rcname[0] = '$' "
								  "              and c1.rcname[0] = '$')"
								  "          )"
								  "      and c2.rcsrc = c1.rcsrc "
								  "      and c2.rcrelid = i.inhparent) "
								  " order by rcname ",
								  tbinfo->oid);
			else
				appendPQExpBuffer(query, "SELECT rcname, rcsrc"
								  " from pg_relcheck c1"
								  " where rcrelid = '%s'::oid "
								  "   and not exists "
								  "  (select 1 from pg_relcheck c2, "
								  "    pg_inherits i "
								  "    where i.inhrelid = c1.rcrelid "
								  "      and (c2.rcname = c1.rcname "
								  "          or (c2.rcname[0] = '$' "
								  "              and c1.rcname[0] = '$')"
								  "          )"
								  "      and c2.rcsrc = c1.rcsrc "
								  "      and c2.rcrelid = i.inhparent) "
								  " order by rcname ",
								  tbinfo->oid);
			res2 = PQexec(g_conn, query->data);
			if (!res2 ||
				PQresultStatus(res2) != PGRES_TUPLES_OK)
			{
				write_msg(NULL, "query to obtain check constraints failed: %s", PQerrorMessage(g_conn));
				exit_nicely();
			}
			ntups2 = PQntuples(res2);
			if (ntups2 > tbinfo->ncheck)
			{
				write_msg(NULL, "expected %d check constraints on table \"%s\" but found %d\n",
						  tbinfo->ncheck, tbinfo->relname, ntups2);
				write_msg(NULL, "(The system catalogs might be corrupted.)\n");
				exit_nicely();
			}

			i_rcname = PQfnumber(res2, "rcname");
			i_rcsrc = PQfnumber(res2, "rcsrc");

			for (j = 0; j < ntups2; j++)
			{
				const char *name = PQgetvalue(res2, j, i_rcname);
				const char *expr = PQgetvalue(res2, j, i_rcsrc);

				if (actual_atts + j > 0)
					appendPQExpBuffer(q, ",\n\t");

				if (name[0] != '$')
					appendPQExpBuffer(q, "CONSTRAINT %s ",
									  fmtId(name, force_quotes));
				appendPQExpBuffer(q, "CHECK (%s)", expr);
			}
			PQclear(res2);
		}

		/*
		 * Primary Key: In versions of PostgreSQL prior to 7.2, we
		 * needed to include the primary key in the table definition.
		 * However, this is not ideal because it creates an index
		 * on the table, which makes COPY slower. As of release 7.2,
		 * we can add primary keys to a table after it has been created,
		 * using ALTER TABLE; see dumpIndexes() for more information.
		 * Therefore, we ignore primary keys in this function.
		 */

		appendPQExpBuffer(q, "\n)");

		if (numParents > 0)
		{
			appendPQExpBuffer(q, "\nINHERITS (");
			for (k = 0; k < numParents; k++)
			{
				TableInfo  *parentRel = &g_tblinfo[parentIndexes[k]];

				if (k > 0)
					appendPQExpBuffer(q, ", ");
				if (parentRel->relnamespace != tbinfo->relnamespace)
					appendPQExpBuffer(q, "%s.",
									  fmtId(parentRel->relnamespace->nspname,
											force_quotes));
				appendPQExpBuffer(q, "%s",
								  fmtId(parentRel->relname, force_quotes));
			}
			appendPQExpBuffer(q, ")");
		}

		if (!tbinfo->hasoids)
			appendPQExpBuffer(q, " WITHOUT OIDS");

		appendPQExpBuffer(q, ";\n");
	}

	ArchiveEntry(fout, objoid, tbinfo->relname,
				 tbinfo->relnamespace->nspname, tbinfo->usename,
				 reltypename, NULL, q->data, delq->data,
				 NULL, NULL, NULL);

	/* Dump Table Comments */
	dumpTableComment(fout, tbinfo, reltypename, commentDeps);

	if (commentDeps)
	{
		for (j = 0; (*commentDeps)[j] != NULL; j++)
		{
			free((void *) (*commentDeps)[j]);
		}
		free(commentDeps);
	}

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
}

/*
 * getAttrName: extract the correct name for an attribute
 *
 * The array tblInfo->attnames[] only provides names of user attributes;
 * if a system attribute number is supplied, we have to fake it.
 * We also do a little bit of bounds checking for safety's sake.
 */
static const char *
getAttrName(int attrnum, TableInfo *tblInfo)
{
	if (attrnum > 0 && attrnum <= tblInfo->numatts)
		return tblInfo->attnames[attrnum - 1];
	switch (attrnum)
	{
		case SelfItemPointerAttributeNumber:
			return "ctid";
		case ObjectIdAttributeNumber:
			return "oid";
		case MinTransactionIdAttributeNumber:
			return "xmin";
		case MinCommandIdAttributeNumber:
			return "cmin";
		case MaxTransactionIdAttributeNumber:
			return "xmax";
		case MaxCommandIdAttributeNumber:
			return "cmax";
		case TableOidAttributeNumber:
			return "tableoid";
	}
	write_msg(NULL, "getAttrName(): invalid column number %d for table %s\n",
			  attrnum, tblInfo->relname);
	exit_nicely();
	return NULL;				/* keep compiler quiet */
}

/*
 * dumpIndexes:
 *	  write out to fout all the user-defined indexes for dumpable tables
 */
void
dumpIndexes(Archive *fout, TableInfo *tblinfo, int numTables)
{
	int			i,
				j;
	PQExpBuffer query = createPQExpBuffer();
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	PGresult   *res;
	int			ntups;
	int			i_indexreloid;
	int			i_indexrelname;
	int			i_indexdef;
	int			i_indisprimary;
	int			i_indkey;
	int			i_indnkeys;

	for (i = 0; i < numTables; i++)
	{
		TableInfo  *tbinfo = &tblinfo[i];

		/* Only plain tables have indexes */
		if (tbinfo->relkind != RELKIND_RELATION || !tbinfo->hasindex)
			continue;

		if (!tbinfo->dump)
			continue;

		/* Make sure we are in proper schema so indexdef is right */
		selectSourceSchema(tbinfo->relnamespace->nspname);

		resetPQExpBuffer(query);
		if (g_fout->remoteVersion >= 70300)
			appendPQExpBuffer(query,
							  "SELECT i.indexrelid as indexreloid, "
							  "t.relname as indexrelname, "
							  "pg_catalog.pg_get_indexdef(i.indexrelid) as indexdef, "
							  "i.indisprimary, i.indkey, "
							  "t.relnatts as indnkeys "
							  "FROM pg_catalog.pg_index i, "
							  "pg_catalog.pg_class t "
							  "WHERE t.oid = i.indexrelid "
							  "AND i.indrelid = '%s'::pg_catalog.oid "
							  "ORDER BY indexrelname",
							  tbinfo->oid);
		else
			appendPQExpBuffer(query,
							  "SELECT i.indexrelid as indexreloid, "
							  "t.relname as indexrelname, "
							  "pg_get_indexdef(i.indexrelid) as indexdef, "
							  "i.indisprimary, i.indkey, "
							  "t.relnatts as indnkeys "
							  "FROM pg_index i, pg_class t "
							  "WHERE t.oid = i.indexrelid "
							  "AND i.indrelid = '%s'::oid "
							  "ORDER BY indexrelname",
							  tbinfo->oid);

		res = PQexec(g_conn, query->data);
		if (!res ||
			PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			write_msg(NULL, "query to obtain list of indexes failed: %s", PQerrorMessage(g_conn));
			exit_nicely();
		}

		ntups = PQntuples(res);

		i_indexreloid = PQfnumber(res, "indexreloid");
		i_indexrelname = PQfnumber(res, "indexrelname");
		i_indexdef = PQfnumber(res, "indexdef");
		i_indisprimary = PQfnumber(res, "indisprimary");
		i_indkey = PQfnumber(res, "indkey");
		i_indnkeys = PQfnumber(res, "indnkeys");

		for (j = 0; j < ntups; j++)
		{
			const char *indexreloid = PQgetvalue(res, j, i_indexreloid);
			const char *indexrelname = PQgetvalue(res, j, i_indexrelname);
			const char *indexdef = PQgetvalue(res, j, i_indexdef);
			const char *indisprimary = PQgetvalue(res, j, i_indisprimary);

			resetPQExpBuffer(q);
			resetPQExpBuffer(delq);

			if (strcmp(indisprimary, "t") == 0)
			{
				/* Handle PK indexes specially */
				int indnkeys = atoi(PQgetvalue(res, j, i_indnkeys));
				char **indkeys = (char **) malloc(indnkeys * sizeof(char *));
				int			k;

				parseNumericArray(PQgetvalue(res, j, i_indkey),
								  indkeys, indnkeys);

				appendPQExpBuffer(q, "ALTER TABLE %s ADD ",
								  fmtId(tbinfo->relname, force_quotes));
				appendPQExpBuffer(q, "CONSTRAINT %s PRIMARY KEY (",
								  fmtId(indexrelname, force_quotes));

				for (k = 0; k < indnkeys; k++)
				{
					int			indkey = atoi(indkeys[k]);
					const char *attname;

					if (indkey == InvalidAttrNumber)
						break;
					attname = getAttrName(indkey, tbinfo);

					appendPQExpBuffer(q, "%s%s",
									  (k == 0) ? "" : ", ",
									  fmtId(attname, force_quotes));
				}

				appendPQExpBuffer(q, ");\n");

				ArchiveEntry(fout, indexreloid,
							 indexrelname,
							 tbinfo->relnamespace->nspname,
							 tbinfo->usename,
							 "CONSTRAINT", NULL,
							 q->data, "",
							 NULL, NULL, NULL);

				free(indkeys);
			}
			else
			{
				/* Plain secondary index */
				appendPQExpBuffer(q, "%s;\n", indexdef);

				/* DROP must be fully qualified in case same name appears in pg_catalog */
				appendPQExpBuffer(delq, "DROP INDEX %s.",
								  fmtId(tbinfo->relnamespace->nspname, force_quotes));
				appendPQExpBuffer(delq, "%s;\n",
								  fmtId(indexrelname, force_quotes));

				ArchiveEntry(fout, indexreloid,
							 indexrelname,
							 tbinfo->relnamespace->nspname,
							 tbinfo->usename,
							 "INDEX", NULL,
							 q->data, delq->data,
							 NULL, NULL, NULL);
			}

			/* Dump Index Comments */
			resetPQExpBuffer(q);
			appendPQExpBuffer(q, "INDEX %s",
							  fmtId(indexrelname, force_quotes));
			dumpComment(fout, q->data,
						tbinfo->relnamespace->nspname,
						tbinfo->usename,
						indexreloid, "pg_class", 0, NULL);
		}

		PQclear(res);
	}

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
}

/*
 * setMaxOid -
 * find the maximum oid and generate a COPY statement to set it
*/

static void
setMaxOid(Archive *fout)
{
	PGresult   *res;
	Oid			max_oid;
	char		sql[1024];

	res = PQexec(g_conn, "CREATE TEMPORARY TABLE pgdump_oid (dummy integer)");
	if (!res ||
		PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		write_msg(NULL, "could not create pgdump_oid table: %s", PQerrorMessage(g_conn));
		exit_nicely();
	}
	PQclear(res);
	res = PQexec(g_conn, "INSERT INTO pgdump_oid VALUES (0)");
	if (!res ||
		PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		write_msg(NULL, "could not insert into pgdump_oid table: %s", PQerrorMessage(g_conn));
		exit_nicely();
	}
	max_oid = PQoidValue(res);
	if (max_oid == 0)
	{
		write_msg(NULL, "inserted invalid oid\n");
		exit_nicely();
	}
	PQclear(res);
	res = PQexec(g_conn, "DROP TABLE pgdump_oid;");
	if (!res ||
		PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		write_msg(NULL, "could not drop pgdump_oid table: %s", PQerrorMessage(g_conn));
		exit_nicely();
	}
	PQclear(res);
	if (g_verbose)
		write_msg(NULL, "maximum system oid is %u\n", max_oid);
	snprintf(sql, 1024,
			 "CREATE TEMPORARY TABLE pgdump_oid (dummy integer);\n"
			 "COPY pgdump_oid WITH OIDS FROM stdin;\n"
			 "%u\t0\n"
			 "\\.\n"
			 "DROP TABLE pgdump_oid;\n",
			 max_oid);

	ArchiveEntry(fout, "0", "Max OID", NULL, "",
				 "<Init>", NULL,
				 sql, "",
				 NULL, NULL, NULL);
}

/*
 * findLastBuiltInOid -
 * find the last built in oid
 * we do this by retrieving datlastsysoid from the pg_database entry for this database,
 */

static Oid
findLastBuiltinOid_V71(const char *dbname)
{
	PGresult   *res;
	int			ntups;
	Oid			last_oid;
	PQExpBuffer query = createPQExpBuffer();

	resetPQExpBuffer(query);
	appendPQExpBuffer(query, "SELECT datlastsysoid from pg_database where datname = ");
	formatStringLiteral(query, dbname, CONV_ALL);

	res = PQexec(g_conn, query->data);
	if (res == NULL ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "error in finding the last system oid: %s", PQerrorMessage(g_conn));
		exit_nicely();
	}
	ntups = PQntuples(res);
	if (ntups < 1)
	{
		write_msg(NULL, "missing pg_database entry for this database\n");
		exit_nicely();
	}
	if (ntups > 1)
	{
		write_msg(NULL, "found more than one pg_database entry for this database\n");
		exit_nicely();
	}
	last_oid = atooid(PQgetvalue(res, 0, PQfnumber(res, "datlastsysoid")));
	PQclear(res);
	destroyPQExpBuffer(query);
	return last_oid;
}

/*
 * findLastBuiltInOid -
 * find the last built in oid
 * we do this by looking up the oid of 'template1' in pg_database,
 * this is probably not foolproof but comes close
*/

static Oid
findLastBuiltinOid_V70(void)
{
	PGresult   *res;
	int			ntups;
	int			last_oid;

	res = PQexec(g_conn,
			  "SELECT oid from pg_database where datname = 'template1'");
	if (res == NULL ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "error in finding the template1 database: %s", PQerrorMessage(g_conn));
		exit_nicely();
	}
	ntups = PQntuples(res);
	if (ntups < 1)
	{
		write_msg(NULL, "could not find template1 database entry in the pg_database table\n");
		exit_nicely();
	}
	if (ntups > 1)
	{
		write_msg(NULL, "found more than one template1 database entry in the pg_database table\n");
		exit_nicely();
	}
	last_oid = atooid(PQgetvalue(res, 0, PQfnumber(res, "oid")));
	PQclear(res);
	return last_oid;
}

static void
dumpOneSequence(Archive *fout, TableInfo *tbinfo,
				const bool schemaOnly, const bool dataOnly)
{
	PGresult   *res;
	char	   *last,
			   *incby,
			   *maxv,
			   *minv,
			   *cache;
	bool		cycled,
				called;
	PQExpBuffer query = createPQExpBuffer();
	PQExpBuffer delqry = createPQExpBuffer();

	/* Make sure we are in proper schema */
	selectSourceSchema(tbinfo->relnamespace->nspname);

	appendPQExpBuffer(query,
			"SELECT sequence_name, last_value, increment_by, max_value, "
				  "min_value, cache_value, is_cycled, is_called from %s",
					  fmtId(tbinfo->relname, force_quotes));

	res = PQexec(g_conn, query->data);
	if (!res || PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to get data of sequence \"%s\" failed: %s", tbinfo->relname, PQerrorMessage(g_conn));
		exit_nicely();
	}

	if (PQntuples(res) != 1)
	{
		write_msg(NULL, "query to get data of sequence \"%s\" returned %d rows (expected 1)\n",
				  tbinfo->relname, PQntuples(res));
		exit_nicely();
	}

	/* Disable this check: it fails if sequence has been renamed */
#ifdef NOT_USED
	if (strcmp(PQgetvalue(res, 0, 0), tbinfo->relname) != 0)
	{
		write_msg(NULL, "query to get data of sequence \"%s\" returned name \"%s\"\n",
				  tbinfo->relname, PQgetvalue(res, 0, 0));
		exit_nicely();
	}
#endif

	last = PQgetvalue(res, 0, 1);
	incby = PQgetvalue(res, 0, 2);
	maxv = PQgetvalue(res, 0, 3);
	minv = PQgetvalue(res, 0, 4);
	cache = PQgetvalue(res, 0, 5);
	cycled = (strcmp(PQgetvalue(res, 0, 6), "t") == 0);
	called = (strcmp(PQgetvalue(res, 0, 7), "t") == 0);

	/*
	 * The logic we use for restoring sequences is as follows: -   Add a
	 * basic CREATE SEQUENCE statement (use last_val for start if called
	 * is false, else use min_val for start_val).
	 *
	 * Add a 'SETVAL(seq, last_val, iscalled)' at restore-time iff we load
	 * data
	 */

	if (!dataOnly)
	{
		resetPQExpBuffer(delqry);

		/* DROP must be fully qualified in case same name appears in pg_catalog */
		appendPQExpBuffer(delqry, "DROP SEQUENCE %s.",
						  fmtId(tbinfo->relnamespace->nspname, force_quotes));
		appendPQExpBuffer(delqry, "%s;\n",
						  fmtId(tbinfo->relname, force_quotes));

		resetPQExpBuffer(query);
		appendPQExpBuffer(query,
						  "CREATE SEQUENCE %s start %s increment %s "
						  "maxvalue %s minvalue %s cache %s%s;\n",
						  fmtId(tbinfo->relname, force_quotes),
						  (called ? minv : last),
						  incby, maxv, minv, cache,
						  (cycled ? " cycle" : ""));

		ArchiveEntry(fout, tbinfo->oid, tbinfo->relname,
					 tbinfo->relnamespace->nspname, tbinfo->usename,
					 "SEQUENCE", NULL,
					 query->data, delqry->data,
					 NULL, NULL, NULL);
	}

	if (!schemaOnly)
	{
		resetPQExpBuffer(query);
		appendPQExpBuffer(query, "SELECT pg_catalog.setval (");
		formatStringLiteral(query, fmtId(tbinfo->relname, force_quotes), CONV_ALL);
		appendPQExpBuffer(query, ", %s, %s);\n",
						  last, (called ? "true" : "false"));

		ArchiveEntry(fout, tbinfo->oid, tbinfo->relname,
					 tbinfo->relnamespace->nspname, tbinfo->usename,
					 "SEQUENCE SET", NULL,
					 query->data, "" /* Del */ ,
					 NULL, NULL, NULL);
	}

	if (!dataOnly)
	{
		/* Dump Sequence Comments */

		resetPQExpBuffer(query);
		appendPQExpBuffer(query, "SEQUENCE %s", fmtId(tbinfo->relname, force_quotes));
		dumpComment(fout, query->data,
					tbinfo->relnamespace->nspname, tbinfo->usename,
					tbinfo->oid, "pg_class", 0, NULL);
	}

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(delqry);
}


static void
dumpTriggers(Archive *fout, TableInfo *tblinfo, int numTables)
{
	int			i,
				j;
	PQExpBuffer query = createPQExpBuffer();
	PQExpBuffer delqry = createPQExpBuffer();
	PGresult   *res;
	int			i_tgoid,
				i_tgname,
				i_tgfname,
				i_tgtype,
				i_tgnargs,
				i_tgargs,
				i_tgisconstraint,
				i_tgconstrname,
				i_tgdeferrable,
				i_tgconstrrelid,
				i_tgconstrrelname,
				i_tginitdeferred;
	int			ntups;

	for (i = 0; i < numTables; i++)
	{
		TableInfo	   *tbinfo = &tblinfo[i];

		if (tbinfo->ntrig == 0 || !tbinfo->dump)
			continue;

		if (g_verbose)
			write_msg(NULL, "dumping triggers for table %s\n",
					  tbinfo->relname);

		/* select table schema to ensure regproc name is qualified if needed */
		selectSourceSchema(tbinfo->relnamespace->nspname);

		resetPQExpBuffer(query);
		if (g_fout->remoteVersion >= 70300)
		{
			appendPQExpBuffer(query,
							  "SELECT tgname, "
							  "tgfoid::pg_catalog.regproc as tgfname, "
							  "tgtype, tgnargs, tgargs, "
							  "tgisconstraint, tgconstrname, tgdeferrable, "
							  "tgconstrrelid, tginitdeferred, oid, "
							  "tgconstrrelid::pg_catalog.regclass as tgconstrrelname "
							  "from pg_catalog.pg_trigger "
							  "where tgrelid = '%s'::pg_catalog.oid",
							  tbinfo->oid);
		}
		else
		{
			appendPQExpBuffer(query,
							  "SELECT tgname, tgfoid::regproc as tgfname, "
							  "tgtype, tgnargs, tgargs, "
							  "tgisconstraint, tgconstrname, tgdeferrable, "
							  "tgconstrrelid, tginitdeferred, oid, "
							  "(select relname from pg_class where oid = tgconstrrelid) "
							  "		as tgconstrrelname "
							  "from pg_trigger "
							  "where tgrelid = '%s'::oid",
							  tbinfo->oid);
		}
		res = PQexec(g_conn, query->data);
		if (!res ||
			PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			write_msg(NULL, "query to obtain list of triggers failed: %s", PQerrorMessage(g_conn));
			exit_nicely();
		}
		ntups = PQntuples(res);
		if (ntups != tbinfo->ntrig)
		{
			write_msg(NULL, "expected %d triggers on table \"%s\" but found %d\n",
					  tbinfo->ntrig, tbinfo->relname, ntups);
			exit_nicely();
		}
		i_tgname = PQfnumber(res, "tgname");
		i_tgfname = PQfnumber(res, "tgfname");
		i_tgtype = PQfnumber(res, "tgtype");
		i_tgnargs = PQfnumber(res, "tgnargs");
		i_tgargs = PQfnumber(res, "tgargs");
		i_tgoid = PQfnumber(res, "oid");
		i_tgisconstraint = PQfnumber(res, "tgisconstraint");
		i_tgconstrname = PQfnumber(res, "tgconstrname");
		i_tgdeferrable = PQfnumber(res, "tgdeferrable");
		i_tgconstrrelid = PQfnumber(res, "tgconstrrelid");
		i_tgconstrrelname = PQfnumber(res, "tgconstrrelname");
		i_tginitdeferred = PQfnumber(res, "tginitdeferred");

		for (j = 0; j < ntups; j++)
		{
			const char *tgoid = PQgetvalue(res, j, i_tgoid);
			char	   *tgname = PQgetvalue(res, j, i_tgname);
			const char *tgfname = PQgetvalue(res, j, i_tgfname);
			int2		tgtype = atoi(PQgetvalue(res, j, i_tgtype));
			int			tgnargs = atoi(PQgetvalue(res, j, i_tgnargs));
			const char *tgargs = PQgetvalue(res, j, i_tgargs);
			int			tgisconstraint;
			int			tgdeferrable;
			int			tginitdeferred;
			char	   *tgconstrrelid;
			const char *p;
			int			findx;

			if (strcmp(PQgetvalue(res, j, i_tgisconstraint), "f") == 0)
				tgisconstraint = 0;
			else
				tgisconstraint = 1;

			if (strcmp(PQgetvalue(res, j, i_tgdeferrable), "f") == 0)
				tgdeferrable = 0;
			else
				tgdeferrable = 1;

			if (strcmp(PQgetvalue(res, j, i_tginitdeferred), "f") == 0)
				tginitdeferred = 0;
			else
				tginitdeferred = 1;

			resetPQExpBuffer(delqry);
			/* DROP must be fully qualified in case same name appears in pg_catalog */
			appendPQExpBuffer(delqry, "DROP TRIGGER %s ",
							  fmtId(tgname, force_quotes));
			appendPQExpBuffer(delqry, "ON %s.",
							  fmtId(tbinfo->relnamespace->nspname, force_quotes));
			appendPQExpBuffer(delqry, "%s;\n",
							  fmtId(tbinfo->relname, force_quotes));

			resetPQExpBuffer(query);
			if (tgisconstraint)
			{
				appendPQExpBuffer(query, "CREATE CONSTRAINT TRIGGER ");
				appendPQExpBuffer(query, fmtId(PQgetvalue(res, j, i_tgconstrname), force_quotes));
			}
			else
			{
				appendPQExpBuffer(query, "CREATE TRIGGER ");
				appendPQExpBuffer(query, fmtId(tgname, force_quotes));
			}
			appendPQExpBufferChar(query, ' ');
			/* Trigger type */
			findx = 0;
			if (TRIGGER_FOR_BEFORE(tgtype))
				appendPQExpBuffer(query, "BEFORE");
			else
				appendPQExpBuffer(query, "AFTER");
			if (TRIGGER_FOR_INSERT(tgtype))
			{
				appendPQExpBuffer(query, " INSERT");
				findx++;
			}
			if (TRIGGER_FOR_DELETE(tgtype))
			{
				if (findx > 0)
					appendPQExpBuffer(query, " OR DELETE");
				else
					appendPQExpBuffer(query, " DELETE");
				findx++;
			}
			if (TRIGGER_FOR_UPDATE(tgtype))
			{
				if (findx > 0)
					appendPQExpBuffer(query, " OR UPDATE");
				else
					appendPQExpBuffer(query, " UPDATE");
			}
			appendPQExpBuffer(query, " ON %s ",
							  fmtId(tbinfo->relname, force_quotes));

			if (tgisconstraint)
			{
				tgconstrrelid = PQgetvalue(res, j, i_tgconstrrelid);

				if (strcmp(tgconstrrelid, "0") != 0)
				{

					if (PQgetisnull(res, j, i_tgconstrrelname))
					{
						write_msg(NULL, "query produced NULL referenced table name for foreign key trigger \"%s\" on table \"%s\" (oid of table: %s)\n",
								  tgname, tbinfo->relname, tgconstrrelid);
						exit_nicely();
					}

					/* If we are using regclass, name is already quoted */
					if (g_fout->remoteVersion >= 70300)
						appendPQExpBuffer(query, " FROM %s",
										  PQgetvalue(res, j, i_tgconstrrelname));
					else
						appendPQExpBuffer(query, " FROM %s",
										  fmtId(PQgetvalue(res, j, i_tgconstrrelname), force_quotes));
				}
				if (!tgdeferrable)
					appendPQExpBuffer(query, " NOT");
				appendPQExpBuffer(query, " DEFERRABLE INITIALLY ");
				if (tginitdeferred)
					appendPQExpBuffer(query, "DEFERRED");
				else
					appendPQExpBuffer(query, "IMMEDIATE");

			}

			appendPQExpBuffer(query, " FOR EACH ROW");
			/* In 7.3, result of regproc is already quoted */
			if (g_fout->remoteVersion >= 70300)
				appendPQExpBuffer(query, " EXECUTE PROCEDURE %s (",
								  tgfname);
			else
				appendPQExpBuffer(query, " EXECUTE PROCEDURE %s (",
								  fmtId(tgfname, force_quotes));
			for (findx = 0; findx < tgnargs; findx++)
			{
				const char *s;

				for (p = tgargs;;)
				{
					p = strchr(p, '\\');
					if (p == NULL)
					{
						write_msg(NULL, "bad argument string (%s) for trigger \"%s\" on table \"%s\"\n",
								  PQgetvalue(res, j, i_tgargs),
								  tgname,
								  tbinfo->relname);
						exit_nicely();
					}
					p++;
					if (*p == '\\')
					{
						p++;
						continue;
					}
					if (p[0] == '0' && p[1] == '0' && p[2] == '0')
						break;
				}
				p--;
				appendPQExpBufferChar(query, '\'');
				for (s = tgargs; s < p;)
				{
					if (*s == '\'')
						appendPQExpBufferChar(query, '\\');
					appendPQExpBufferChar(query, *s++);
				}
				appendPQExpBufferChar(query, '\'');
				appendPQExpBuffer(query, (findx < tgnargs - 1) ? ", " : "");
				tgargs = p + 4;
			}
			appendPQExpBuffer(query, ");\n");

			ArchiveEntry(fout, tgoid,
						 tgname,
						 tbinfo->relnamespace->nspname,
						 tbinfo->usename,
						 "TRIGGER", NULL,
						 query->data, delqry->data,
						 NULL, NULL, NULL);

			resetPQExpBuffer(query);
			appendPQExpBuffer(query, "TRIGGER %s ",
							  fmtId(tgname, force_quotes));
			appendPQExpBuffer(query, "ON %s",
							  fmtId(tbinfo->relname, force_quotes));

			dumpComment(fout, query->data,
						tbinfo->relnamespace->nspname, tbinfo->usename,
						tgoid, "pg_trigger", 0, NULL);
		}

		PQclear(res);
	}

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(delqry);
}


static void
dumpRules(Archive *fout, TableInfo *tblinfo, int numTables)
{
	PGresult   *res;
	int			nrules;
	int			i,
				t;
	PQExpBuffer query = createPQExpBuffer();
	int			i_definition;
	int			i_oid;
	int			i_rulename;

	if (g_verbose)
		write_msg(NULL, "dumping out rules\n");

	/*
	 * For each table we dump
	 */
	for (t = 0; t < numTables; t++)
	{
		TableInfo	   *tbinfo = &tblinfo[t];

		if (!tbinfo->hasrules || !tbinfo->dump)
			continue;

		/* Make sure we are in proper schema */
		selectSourceSchema(tbinfo->relnamespace->nspname);

		/*
		 * Get all rules defined for this table, except view select rules
		 */
		resetPQExpBuffer(query);

		if (g_fout->remoteVersion >= 70300)
		{
			appendPQExpBuffer(query,
							  "SELECT pg_catalog.pg_get_ruledef(oid) AS definition,"
							  " oid, rulename "
							  "FROM pg_catalog.pg_rewrite "
							  "WHERE ev_class = '%s'::pg_catalog.oid "
							  "AND rulename != '_RETURN' "
							  "ORDER BY oid",
							  tbinfo->oid);
		}
		else
		{
			/*
			 * We include pg_rules in the cross since it filters out all view
			 * rules (pjw 15-Sep-2000).
			 */
			appendPQExpBuffer(query, "SELECT definition,"
							  "   pg_rewrite.oid, pg_rewrite.rulename "
							  "FROM pg_rewrite, pg_class, pg_rules "
							  "WHERE pg_class.relname = ");
			formatStringLiteral(query, tbinfo->relname, CONV_ALL);
			appendPQExpBuffer(query,
							  "    AND pg_rewrite.ev_class = pg_class.oid "
							  "    AND pg_rules.tablename = pg_class.relname "
							  "    AND pg_rules.rulename = pg_rewrite.rulename "
							  "ORDER BY pg_rewrite.oid");
		}

		res = PQexec(g_conn, query->data);
		if (!res ||
			PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			write_msg(NULL, "query to get rules associated with table \"%s\" failed: %s",
					  tbinfo->relname, PQerrorMessage(g_conn));
			exit_nicely();
		}

		nrules = PQntuples(res);
		i_definition = PQfnumber(res, "definition");
		i_oid = PQfnumber(res, "oid");
		i_rulename = PQfnumber(res, "rulename");

		/*
		 * Dump them out
		 */

		for (i = 0; i < nrules; i++)
		{
			ArchiveEntry(fout, PQgetvalue(res, i, i_oid),
						 PQgetvalue(res, i, i_rulename),
						 tbinfo->relnamespace->nspname,
						 tbinfo->usename,
						 "RULE", NULL,
						 PQgetvalue(res, i, i_definition),
						 "",	/* Del */
						 NULL, NULL, NULL);

			/* Dump rule comments */

			resetPQExpBuffer(query);
			appendPQExpBuffer(query, "RULE %s", fmtId(PQgetvalue(res, i, i_rulename), force_quotes));
			appendPQExpBuffer(query, " ON %s", fmtId(tbinfo->relname, force_quotes));
			dumpComment(fout, query->data,
						tbinfo->relnamespace->nspname,
						tbinfo->usename,
						PQgetvalue(res, i, i_oid), "pg_rewrite", 0, NULL);

		}

		PQclear(res);
	}

	destroyPQExpBuffer(query);
}

/*
 * selectSourceSchema - make the specified schema the active search path
 * in the source database.
 *
 * NB: pg_catalog is explicitly searched after the specified schema;
 * so user names are only qualified if they are cross-schema references,
 * and system names are only qualified if they conflict with a user name
 * in the current schema.
 *
 * Whenever the selected schema is not pg_catalog, be careful to qualify
 * references to system catalogs and types in our emitted commands!
 */
static void
selectSourceSchema(const char *schemaName)
{
	static char	   *curSchemaName = NULL;
	PQExpBuffer query;
	PGresult   *res;

	/* Not relevant if fetching from pre-7.3 DB */
	if (g_fout->remoteVersion < 70300)
		return;
	/* Ignore null schema names */
	if (schemaName == NULL || *schemaName == '\0')
		return;
	/* Optimize away repeated selection of same schema */
	if (curSchemaName && strcmp(curSchemaName, schemaName) == 0)
		return;

	query = createPQExpBuffer();
	appendPQExpBuffer(query, "SET search_path = %s",
					  fmtId(schemaName, force_quotes));
	if (strcmp(schemaName, "pg_catalog") != 0)
		appendPQExpBuffer(query, ", pg_catalog");
	res = PQexec(g_conn, query->data);
	if (!res ||
		PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		write_msg(NULL, "query to set search_path failed: %s",
				  PQerrorMessage(g_conn));
		exit_nicely();
	}
	PQclear(res);
	destroyPQExpBuffer(query);

	if (curSchemaName)
		free(curSchemaName);
	curSchemaName = strdup(schemaName);
}

/*
 * getFormattedTypeName - retrieve a nicely-formatted type name for the
 * given type name.
 *
 * NB: in 7.3 and up the result may depend on the currently-selected
 * schema; this is why we don't try to cache the names.
 */
static char *
getFormattedTypeName(const char *oid, OidOptions opts)
{
	char	   *result;
	PQExpBuffer query;
	PGresult   *res;
	int			ntups;

	if (atooid(oid) == 0)
	{
		if ((opts & zeroAsOpaque) != 0)
			return strdup(g_opaque_type);
		else if ((opts & zeroAsAny) != 0)
			return strdup("'any'");
		else if ((opts & zeroAsStar) != 0)
			return strdup("*");
		else if ((opts & zeroAsNone) != 0)
			return strdup("NONE");
	}

	query = createPQExpBuffer();
	if (g_fout->remoteVersion >= 70300)
	{
		appendPQExpBuffer(query, "SELECT pg_catalog.format_type('%s'::pg_catalog.oid, NULL)",
						  oid);
	}
	else if (g_fout->remoteVersion >= 70100)
	{
		appendPQExpBuffer(query, "SELECT format_type('%s'::oid, NULL)",
						  oid);
	}
	else
	{
		appendPQExpBuffer(query, "SELECT typname "
						  "FROM pg_type "
						  "WHERE oid = '%s'::oid",
						  oid);
	}

	res = PQexec(g_conn, query->data);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to obtain type name for %s failed: %s",
				  oid, PQerrorMessage(g_conn));
		exit_nicely();
	}

	/* Expecting a single result only */
	ntups = PQntuples(res);
	if (ntups != 1)
	{
		write_msg(NULL, "Got %d rows instead of one from: %s",
				  ntups, query->data);
		exit_nicely();
	}

	if (g_fout->remoteVersion >= 70100)
	{
		/* already quoted */
		result = strdup(PQgetvalue(res, 0, 0));
	}
	else
	{
		/* may need to quote it */
		result = strdup(fmtId(PQgetvalue(res, 0, 0), false));
	}

	PQclear(res);
	destroyPQExpBuffer(query);

	return result;
}

/*
 * myFormatType --- local implementation of format_type for use with 7.0.
 */
static char *
myFormatType(const char *typname, int32 typmod)
{
	char	   *result;
	PQExpBuffer buf = createPQExpBuffer();

	/* Show lengths on bpchar and varchar */
	if (!strcmp(typname, "bpchar"))
	{
		int			len = (typmod - VARHDRSZ);

		appendPQExpBuffer(buf, "character");
		if (len > 1)
			appendPQExpBuffer(buf, "(%d)",
							  typmod - VARHDRSZ);
	}
	else if (!strcmp(typname, "varchar"))
	{
		appendPQExpBuffer(buf, "character varying");
		if (typmod != -1)
			appendPQExpBuffer(buf, "(%d)",
							  typmod - VARHDRSZ);
	}
	else if (!strcmp(typname, "numeric"))
	{
		appendPQExpBuffer(buf, "numeric");
		if (typmod != -1)
		{
			int32		tmp_typmod;
			int			precision;
			int			scale;

			tmp_typmod = typmod - VARHDRSZ;
			precision = (tmp_typmod >> 16) & 0xffff;
			scale = tmp_typmod & 0xffff;
			appendPQExpBuffer(buf, "(%d,%d)",
							  precision, scale);
		}
	}
	/*
	 * char is an internal single-byte data type; Let's make sure we force
	 * it through with quotes. - thomas 1998-12-13
	 */
	else if (!strcmp(typname, "char"))
	{
		appendPQExpBuffer(buf, "%s", fmtId(typname, true));
	}
	else
	{
		appendPQExpBuffer(buf, "%s", fmtId(typname, false));
	}

	result = strdup(buf->data);
	destroyPQExpBuffer(buf);

	return result;
}

/*
 * fmtQualifiedId - convert a qualified name to the proper format for
 * the source database.
 *
 * Like fmtId, use the result before calling again.
 */
static const char *
fmtQualifiedId(const char *schema, const char *id)
{
	static PQExpBuffer id_return = NULL;

	if (id_return)				/* first time through? */
		resetPQExpBuffer(id_return);
	else
		id_return = createPQExpBuffer();

	/* Suppress schema name if fetching from pre-7.3 DB */
	if (g_fout->remoteVersion >= 70300 && schema && *schema)
	{
		appendPQExpBuffer(id_return, "%s.",
						  fmtId(schema, force_quotes));
	}
	appendPQExpBuffer(id_return, "%s",
					  fmtId(id, force_quotes));

	return id_return->data;
}
