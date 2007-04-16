/*-------------------------------------------------------------------------
 *
 * pg_dump.c
 *	  pg_dump is a utility for dumping out a postgres database
 *	  into a script file.
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	pg_dump will read the system catalogs in a database and dump out a
 *	script that reproduces the schema in terms of SQL that is understood
 *	by PostgreSQL
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/bin/pg_dump/pg_dump.c,v 1.453.2.1 2007/04/16 18:42:17 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

/*
 * Although this is not a backend module, we must include postgres.h anyway
 * so that we can include a bunch of backend include files.  pg_dump has
 * never pretended to be very independent of the backend anyhow ...
 */
#include "postgres.h"

#include <unistd.h>

#include <ctype.h>
#ifdef ENABLE_NLS
#include <locale.h>
#endif
#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif

#include "getopt_long.h"

#ifndef HAVE_INT_OPTRESET
int			optreset;
#endif

#include "access/htup.h"
#include "catalog/pg_class.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "commands/sequence.h"
#include "libpq/libpq-fs.h"
#include "mb/pg_wchar.h"

#include "pg_backup_archiver.h"
#include "dumputils.h"

extern char *optarg;
extern int	optind,
			opterr;


typedef struct
{
	const char *descr;			/* comment for an object */
	Oid			classoid;		/* object class (catalog OID) */
	Oid			objoid;			/* object OID */
	int			objsubid;		/* subobject (table column #) */
} CommentItem;


/* global decls */
bool		g_verbose;			/* User wants verbose narration of our
								 * activities. */
Archive    *g_fout;				/* the script file */
PGconn	   *g_conn;				/* the database connection */

/* various user-settable parameters */
bool		dumpInserts;		/* dump data using proper insert strings */
bool		attrNames;			/* put attr names into insert strings */
bool		schemaOnly;
bool		dataOnly;
bool		aclsSkip;

/* subquery used to convert user ID (eg, datdba) to user name */
static const char *username_subquery;

/* obsolete as of 7.3: */
static Oid	g_last_builtin_oid; /* value of the last builtin oid */

/*
 * Object inclusion/exclusion lists
 *
 * The string lists record the patterns given by command-line switches,
 * which we then convert to lists of OIDs of matching objects.
 */
static SimpleStringList schema_include_patterns = { NULL, NULL };
static SimpleOidList schema_include_oids = { NULL, NULL };
static SimpleStringList schema_exclude_patterns = { NULL, NULL };
static SimpleOidList schema_exclude_oids = { NULL, NULL };

static SimpleStringList table_include_patterns = { NULL, NULL };
static SimpleOidList table_include_oids = { NULL, NULL };
static SimpleStringList table_exclude_patterns = { NULL, NULL };
static SimpleOidList table_exclude_oids = { NULL, NULL };

/* default, if no "inclusion" switches appear, is to dump everything */
static bool include_everything = true;

char		g_opaque_type[10];	/* name for the opaque type */

/* placeholders for the delimiters for comments */
char		g_comment_start[10];
char		g_comment_end[10];

static const CatalogId nilCatalogId = {0, 0};

/* these are to avoid passing around info for findNamespace() */
static NamespaceInfo *g_namespaces;
static int	g_numNamespaces;

/* flag to turn on/off dollar quoting */
static int	disable_dollar_quoting = 0;


static void help(const char *progname);
static void expand_schema_name_patterns(SimpleStringList *patterns,
										SimpleOidList *oids);
static void expand_table_name_patterns(SimpleStringList *patterns,
									   SimpleOidList *oids);
static NamespaceInfo *findNamespace(Oid nsoid, Oid objoid);
static void dumpTableData(Archive *fout, TableDataInfo *tdinfo);
static void dumpComment(Archive *fout, const char *target,
			const char *namespace, const char *owner,
			CatalogId catalogId, int subid, DumpId dumpId);
static int findComments(Archive *fout, Oid classoid, Oid objoid,
			 CommentItem **items);
static int	collectComments(Archive *fout, CommentItem **items);
static void dumpDumpableObject(Archive *fout, DumpableObject *dobj);
static void dumpNamespace(Archive *fout, NamespaceInfo *nspinfo);
static void dumpType(Archive *fout, TypeInfo *tinfo);
static void dumpBaseType(Archive *fout, TypeInfo *tinfo);
static void dumpDomain(Archive *fout, TypeInfo *tinfo);
static void dumpCompositeType(Archive *fout, TypeInfo *tinfo);
static void dumpShellType(Archive *fout, ShellTypeInfo *stinfo);
static void dumpProcLang(Archive *fout, ProcLangInfo *plang);
static void dumpFunc(Archive *fout, FuncInfo *finfo);
static void dumpCast(Archive *fout, CastInfo *cast);
static void dumpOpr(Archive *fout, OprInfo *oprinfo);
static void dumpOpclass(Archive *fout, OpclassInfo *opcinfo);
static void dumpConversion(Archive *fout, ConvInfo *convinfo);
static void dumpRule(Archive *fout, RuleInfo *rinfo);
static void dumpAgg(Archive *fout, AggInfo *agginfo);
static void dumpTrigger(Archive *fout, TriggerInfo *tginfo);
static void dumpTable(Archive *fout, TableInfo *tbinfo);
static void dumpTableSchema(Archive *fout, TableInfo *tbinfo);
static void dumpAttrDef(Archive *fout, AttrDefInfo *adinfo);
static void dumpSequence(Archive *fout, TableInfo *tbinfo);
static void dumpIndex(Archive *fout, IndxInfo *indxinfo);
static void dumpConstraint(Archive *fout, ConstraintInfo *coninfo);
static void dumpTableConstraintComment(Archive *fout, ConstraintInfo *coninfo);

static void dumpACL(Archive *fout, CatalogId objCatId, DumpId objDumpId,
		const char *type, const char *name,
		const char *tag, const char *nspname, const char *owner,
		const char *acls);

static void getDependencies(void);
static void getDomainConstraints(TypeInfo *tinfo);
static void getTableData(TableInfo *tblinfo, int numTables, bool oids);
static char *format_function_arguments(FuncInfo *finfo, int nallargs,
						  char **allargtypes,
						  char **argmodes,
						  char **argnames);
static char *format_function_signature(FuncInfo *finfo, bool honor_quotes);
static const char *convertRegProcReference(const char *proc);
static const char *convertOperatorReference(const char *opr);
static Oid	findLastBuiltinOid_V71(const char *);
static Oid	findLastBuiltinOid_V70(void);
static void selectSourceSchema(const char *schemaName);
static char *getFormattedTypeName(Oid oid, OidOptions opts);
static char *myFormatType(const char *typname, int32 typmod);
static const char *fmtQualifiedId(const char *schema, const char *id);
static bool hasBlobs(Archive *AH);
static int	dumpBlobs(Archive *AH, void *arg);
static int	dumpBlobComments(Archive *AH, void *arg);
static void dumpDatabase(Archive *AH);
static void dumpEncoding(Archive *AH);
static void dumpStdStrings(Archive *AH);
static const char *getAttrName(int attrnum, TableInfo *tblInfo);
static const char *fmtCopyColumnList(const TableInfo *ti);
static void do_sql_command(PGconn *conn, const char *query);
static void check_sql_result(PGresult *res, PGconn *conn, const char *query,
				 ExecStatusType expected);


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
	const char *dumpencoding = NULL;
	const char *std_strings;
	bool		oids = false;
	TableInfo  *tblinfo;
	int			numTables;
	DumpableObject **dobjs;
	int			numObjs;
	int			i;
	bool		force_password = false;
	int			compressLevel = -1;
	bool		ignore_version = false;
	int			plainText = 0;
	int			outputClean = 0;
	int			outputCreate = 0;
	bool		outputBlobs = false;
	int			outputNoOwner = 0;
	static int	use_setsessauth = 0;
	static int	disable_triggers = 0;
	char	   *outputSuperuser = NULL;

	RestoreOptions *ropt;

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
		{"oids", no_argument, NULL, 'o'},
		{"no-owner", no_argument, NULL, 'O'},
		{"port", required_argument, NULL, 'p'},
		{"schema", required_argument, NULL, 'n'},
		{"exclude-schema", required_argument, NULL, 'N'},
		{"schema-only", no_argument, NULL, 's'},
		{"superuser", required_argument, NULL, 'S'},
		{"table", required_argument, NULL, 't'},
		{"exclude-table", required_argument, NULL, 'T'},
		{"password", no_argument, NULL, 'W'},
		{"username", required_argument, NULL, 'U'},
		{"verbose", no_argument, NULL, 'v'},
		{"no-privileges", no_argument, NULL, 'x'},
		{"no-acl", no_argument, NULL, 'x'},
		{"compress", required_argument, NULL, 'Z'},
		{"encoding", required_argument, NULL, 'E'},
		{"help", no_argument, NULL, '?'},
		{"version", no_argument, NULL, 'V'},

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

	g_verbose = false;

	strcpy(g_comment_start, "-- ");
	g_comment_end[0] = '\0';
	strcpy(g_opaque_type, "opaque");

	dataOnly = schemaOnly = dumpInserts = attrNames = false;

	progname = get_progname(argv[0]);

	/* Set default options based on progname */
	if (strcmp(progname, "pg_backup") == 0)
		format = "c";

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

	while ((c = getopt_long(argc, argv, "abcCdDE:f:F:h:in:N:oOp:RsS:t:T:uU:vWxX:Z:",
							long_options, &optindex)) != -1)
	{
		switch (c)
		{
			case 'a':			/* Dump data only */
				dataOnly = true;
				break;

			case 'b':			/* Dump blobs */
				outputBlobs = true;
				break;

			case 'c':			/* clean (i.e., drop) schema prior to create */
				outputClean = 1;
				break;

			case 'C':			/* Create DB */
				outputCreate = 1;
				break;

			case 'd':			/* dump data as proper insert strings */
				dumpInserts = true;
				break;

			case 'D':			/* dump data as proper insert strings with
								 * attr names */
				dumpInserts = true;
				attrNames = true;
				break;

			case 'E':			/* Dump encoding */
				dumpencoding = optarg;
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

			case 'n':			/* include schema(s) */
				simple_string_list_append(&schema_include_patterns, optarg);
				include_everything = false;
				break;

			case 'N':			/* exclude schema(s) */
				simple_string_list_append(&schema_exclude_patterns, optarg);
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

			case 'R':
				/* no-op, still accepted for backwards compatibility */
				break;

			case 's':			/* dump schema only */
				schemaOnly = true;
				break;

			case 'S':			/* Username for superuser in plain text output */
				outputSuperuser = strdup(optarg);
				break;

			case 't':			/* include table(s) */
				simple_string_list_append(&table_include_patterns, optarg);
				include_everything = false;
				break;

			case 'T':			/* exclude table(s) */
				simple_string_list_append(&table_exclude_patterns, optarg);
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

			case 'X':
				/* -X is a deprecated alternative to long options */
				if (strcmp(optarg, "disable-dollar-quoting") == 0)
					disable_dollar_quoting = 1;
				else if (strcmp(optarg, "disable-triggers") == 0)
					disable_triggers = 1;
				else if (strcmp(optarg, "use-set-session-authorization") == 0)
					use_setsessauth = 1;
				else
				{
					fprintf(stderr,
							_("%s: invalid -X option -- %s\n"),
							progname, optarg);
					fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
					exit(1);
				}
				break;

			case 'Z':			/* Compression Level */
				compressLevel = atoi(optarg);
				break;

			case 0:
				/* This covers the long options equivalent to -X xxx. */
				break;

			default:
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
				exit(1);
		}
	}

	if (optind < (argc - 1))
	{
		fprintf(stderr, _("%s: too many command-line arguments (first is \"%s\")\n"),
				progname, argv[optind + 1]);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	/* Get database name from command line */
	if (optind < argc)
		dbname = argv[optind];

	if (dataOnly && schemaOnly)
	{
		write_msg(NULL, "options \"schema only\" (-s) and \"data only\" (-a) cannot be used together\n");
		exit(1);
	}

	if (dataOnly && outputClean)
	{
		write_msg(NULL, "options \"clean\" (-c) and \"data only\" (-a) cannot be used together\n");
		exit(1);
	}

	if (dumpInserts == true && oids == true)
	{
		write_msg(NULL, "INSERT (-d, -D) and OID (-o) options cannot be used together\n");
		write_msg(NULL, "(The INSERT command cannot set OIDs.)\n");
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
			write_msg(NULL, "invalid output format \"%s\" specified\n", format);
			exit(1);
	}

	if (g_fout == NULL)
	{
		write_msg(NULL, "could not open output file \"%s\" for writing\n", filename);
		exit(1);
	}

	/* Let the archiver know how noisy to be */
	g_fout->verbose = g_verbose;

	g_fout->minRemoteVersion = 70000;	/* we can handle back to 7.0 */
	g_fout->maxRemoteVersion = parse_version(PG_VERSION);
	if (g_fout->maxRemoteVersion < 0)
	{
		write_msg(NULL, "could not parse version string \"%s\"\n", PG_VERSION);
		exit(1);
	}

	/*
	 * Open the database using the Archiver, so it knows about it. Errors mean
	 * death.
	 */
	g_conn = ConnectDatabase(g_fout, dbname, pghost, pgport,
							 username, force_password, ignore_version);

	/* Set the client encoding if requested */
	if (dumpencoding)
	{
		if (PQsetClientEncoding(g_conn, dumpencoding) < 0)
		{
			write_msg(NULL, "invalid client encoding \"%s\" specified\n",
					  dumpencoding);
			exit(1);
		}
	}

	/*
	 * Get the active encoding and the standard_conforming_strings setting, so
	 * we know how to escape strings.
	 */
	g_fout->encoding = PQclientEncoding(g_conn);

	std_strings = PQparameterStatus(g_conn, "standard_conforming_strings");
	g_fout->std_strings = (std_strings && strcmp(std_strings, "on") == 0);

	/* Set the datestyle to ISO to ensure the dump's portability */
	do_sql_command(g_conn, "SET DATESTYLE = ISO");

	/*
	 * Start serializable transaction to dump consistent data.
	 */
	do_sql_command(g_conn, "BEGIN");

	do_sql_command(g_conn, "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE");

	/* Select the appropriate subquery to convert user IDs to names */
	if (g_fout->remoteVersion >= 80100)
		username_subquery = "SELECT rolname FROM pg_catalog.pg_roles WHERE oid =";
	else if (g_fout->remoteVersion >= 70300)
		username_subquery = "SELECT usename FROM pg_catalog.pg_user WHERE usesysid =";
	else
		username_subquery = "SELECT usename FROM pg_user WHERE usesysid =";

	/*
	 * If supported, set extra_float_digits so that we can dump float data
	 * exactly (given correctly implemented float I/O code, anyway)
	 */
	if (g_fout->remoteVersion >= 70400)
		do_sql_command(g_conn, "SET extra_float_digits TO 2");

	/* Find the last built-in OID, if needed */
	if (g_fout->remoteVersion < 70300)
	{
		if (g_fout->remoteVersion >= 70100)
			g_last_builtin_oid = findLastBuiltinOid_V71(PQdb(g_conn));
		else
			g_last_builtin_oid = findLastBuiltinOid_V70();
		if (g_verbose)
			write_msg(NULL, "last built-in OID is %u\n", g_last_builtin_oid);
	}

	/* Expand schema selection patterns into OID lists */
	if (schema_include_patterns.head != NULL)
	{
		expand_schema_name_patterns(&schema_include_patterns,
									&schema_include_oids);
		if (schema_include_oids.head == NULL)
		{
			write_msg(NULL, "No matching schemas were found\n");
			exit_nicely();
		}
	}
	expand_schema_name_patterns(&schema_exclude_patterns,
								&schema_exclude_oids);
	/* non-matching exclusion patterns aren't an error */

	/* Expand table selection patterns into OID lists */
	if (table_include_patterns.head != NULL)
	{
		expand_table_name_patterns(&table_include_patterns,
								   &table_include_oids);
		if (table_include_oids.head == NULL)
		{
			write_msg(NULL, "No matching tables were found\n");
			exit_nicely();
		}
	}
	expand_table_name_patterns(&table_exclude_patterns,
							   &table_exclude_oids);
	/* non-matching exclusion patterns aren't an error */

	/*
	 * Dumping blobs is now default unless we saw an inclusion switch or -s
	 * ... but even if we did see one of these, -b turns it back on.
	 */
	if (include_everything && !schemaOnly)
		outputBlobs = true;

	/*
	 * Now scan the database and create DumpableObject structs for all the
	 * objects we intend to dump.
	 */
	tblinfo = getSchemaData(&numTables);

	if (!schemaOnly)
		getTableData(tblinfo, numTables, oids);

	if (outputBlobs && hasBlobs(g_fout))
	{
		/* Add placeholders to allow correct sorting of blobs */
		DumpableObject *blobobj;

		blobobj = (DumpableObject *) malloc(sizeof(DumpableObject));
		blobobj->objType = DO_BLOBS;
		blobobj->catId = nilCatalogId;
		AssignDumpId(blobobj);
		blobobj->name = strdup("BLOBS");

		blobobj = (DumpableObject *) malloc(sizeof(DumpableObject));
		blobobj->objType = DO_BLOB_COMMENTS;
		blobobj->catId = nilCatalogId;
		AssignDumpId(blobobj);
		blobobj->name = strdup("BLOB COMMENTS");
	}

	/*
	 * Collect dependency data to assist in ordering the objects.
	 */
	getDependencies();

	/*
	 * Sort the objects into a safe dump order (no forward references).
	 *
	 * In 7.3 or later, we can rely on dependency information to help us
	 * determine a safe order, so the initial sort is mostly for cosmetic
	 * purposes: we sort by name to ensure that logically identical schemas
	 * will dump identically.  Before 7.3 we don't have dependencies and we
	 * use OID ordering as an (unreliable) guide to creation order.
	 */
	getDumpableObjects(&dobjs, &numObjs);

	if (g_fout->remoteVersion >= 70300)
		sortDumpableObjectsByTypeName(dobjs, numObjs);
	else
		sortDumpableObjectsByTypeOid(dobjs, numObjs);

	sortDumpableObjects(dobjs, numObjs);

	/*
	 * Create archive TOC entries for all the objects to be dumped, in a safe
	 * order.
	 */

	/* First the special ENCODING and STDSTRINGS entries. */
	dumpEncoding(g_fout);
	dumpStdStrings(g_fout);

	/* The database item is always next, unless we don't want it at all */
	if (include_everything && !dataOnly)
		dumpDatabase(g_fout);

	/* Now the rearrangeable objects. */
	for (i = 0; i < numObjs; i++)
		dumpDumpableObject(g_fout, dobjs[i]);

	/*
	 * And finally we can do the actual output.
	 */
	if (plainText)
	{
		ropt = NewRestoreOptions();
		ropt->filename = (char *) filename;
		ropt->dropSchema = outputClean;
		ropt->aclsSkip = aclsSkip;
		ropt->superuser = outputSuperuser;
		ropt->create = outputCreate;
		ropt->noOwner = outputNoOwner;
		ropt->disable_triggers = disable_triggers;
		ropt->use_setsessauth = use_setsessauth;
		ropt->dataOnly = dataOnly;

		if (compressLevel == -1)
			ropt->compression = 0;
		else
			ropt->compression = compressLevel;

		ropt->suppressDumpWarnings = true;		/* We've already shown them */

		RestoreArchive(g_fout, ropt);
	}

	CloseArchive(g_fout);

	PQfinish(g_conn);

	exit(0);
}


static void
help(const char *progname)
{
	printf(_("%s dumps a database as a text file or to other formats.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... [DBNAME]\n"), progname);

	printf(_("\nGeneral options:\n"));
	printf(_("  -f, --file=FILENAME      output file name\n"));
	printf(_("  -F, --format=c|t|p       output file format (custom, tar, plain text)\n"));
	printf(_("  -i, --ignore-version     proceed even when server version mismatches\n"
			 "                           pg_dump version\n"));
	printf(_("  -v, --verbose            verbose mode\n"));
	printf(_("  -Z, --compress=0-9       compression level for compressed formats\n"));
	printf(_("  --help                   show this help, then exit\n"));
	printf(_("  --version                output version information, then exit\n"));

	printf(_("\nOptions controlling the output content:\n"));
	printf(_("  -a, --data-only             dump only the data, not the schema\n"));
	printf(_("  -b, --blobs                 include large objects in dump\n"));
	printf(_("  -c, --clean                 clean (drop) schema prior to create\n"));
	printf(_("  -C, --create                include commands to create database in dump\n"));
	printf(_("  -d, --inserts               dump data as INSERT commands, rather than COPY\n"));
	printf(_("  -D, --column-inserts        dump data as INSERT commands with column names\n"));
	printf(_("  -E, --encoding=ENCODING     dump the data in encoding ENCODING\n"));
	printf(_("  -n, --schema=SCHEMA         dump the named schema(s) only\n"));
	printf(_("  -N, --exclude-schema=SCHEMA do NOT dump the named schema(s)\n"));
	printf(_("  -o, --oids                  include OIDs in dump\n"));
	printf(_("  -O, --no-owner              skip restoration of object ownership\n"
			 "                              in plain text format\n"));
	printf(_("  -s, --schema-only           dump only the schema, no data\n"));
	printf(_("  -S, --superuser=NAME        specify the superuser user name to use in\n"
			 "                              plain text format\n"));
	printf(_("  -t, --table=TABLE           dump the named table(s) only\n"));
	printf(_("  -T, --exclude-table=TABLE   do NOT dump the named table(s)\n"));
	printf(_("  -x, --no-privileges         do not dump privileges (grant/revoke)\n"));
	printf(_("  --disable-dollar-quoting    disable dollar quoting, use SQL standard quoting\n"));
	printf(_("  --disable-triggers          disable triggers during data-only restore\n"));
	printf(_("  --use-set-session-authorization\n"
			 "                              use SESSION AUTHORIZATION commands instead of\n"
			 "                              ALTER OWNER commands to set ownership\n"));

	printf(_("\nConnection options:\n"));
	printf(_("  -h, --host=HOSTNAME      database server host or socket directory\n"));
	printf(_("  -p, --port=PORT          database server port number\n"));
	printf(_("  -U, --username=NAME      connect as specified database user\n"));
	printf(_("  -W, --password           force password prompt (should happen automatically)\n"));

	printf(_("\nIf no database name is supplied, then the PGDATABASE environment\n"
			 "variable value is used.\n\n"));
	printf(_("Report bugs to <pgsql-bugs@postgresql.org>.\n"));
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
 * Find the OIDs of all schemas matching the given list of patterns,
 * and append them to the given OID list.
 */
static void
expand_schema_name_patterns(SimpleStringList *patterns, SimpleOidList *oids)
{
	PQExpBuffer query;
	PGresult   *res;
	SimpleStringListCell *cell;
	int			i;

	if (patterns->head == NULL)
		return;					/* nothing to do */

	if (g_fout->remoteVersion < 70300)
	{
		write_msg(NULL, "server version must be at least 7.3 to use schema selection switches\n");
		exit_nicely();
	}

	query = createPQExpBuffer();

	/*
	 * We use UNION ALL rather than UNION; this might sometimes result in
	 * duplicate entries in the OID list, but we don't care.
	 */

	for (cell = patterns->head; cell; cell = cell->next)
	{
		if (cell != patterns->head)
			appendPQExpBuffer(query, "UNION ALL\n");
		appendPQExpBuffer(query,
						  "SELECT oid FROM pg_catalog.pg_namespace n\n");
		processSQLNamePattern(g_conn, query, cell->val, false, false,
							  NULL, "n.nspname", NULL,
							  NULL);
	}

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	for (i = 0; i < PQntuples(res); i++)
	{
		simple_oid_list_append(oids, atooid(PQgetvalue(res, i, 0)));
	}

	PQclear(res);
	destroyPQExpBuffer(query);
}

/*
 * Find the OIDs of all tables matching the given list of patterns,
 * and append them to the given OID list.
 */
static void
expand_table_name_patterns(SimpleStringList *patterns, SimpleOidList *oids)
{
	PQExpBuffer query;
	PGresult   *res;
	SimpleStringListCell *cell;
	int			i;

	if (patterns->head == NULL)
		return;					/* nothing to do */

	query = createPQExpBuffer();

	/*
	 * We use UNION ALL rather than UNION; this might sometimes result in
	 * duplicate entries in the OID list, but we don't care.
	 */

	for (cell = patterns->head; cell; cell = cell->next)
	{
		if (cell != patterns->head)
			appendPQExpBuffer(query, "UNION ALL\n");
		appendPQExpBuffer(query,
						  "SELECT c.oid"
						  "\nFROM pg_catalog.pg_class c"
						  "\n     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace"
						  "\nWHERE c.relkind in ('%c', '%c', '%c')\n",
						  RELKIND_RELATION, RELKIND_SEQUENCE, RELKIND_VIEW);
		processSQLNamePattern(g_conn, query, cell->val, true, false,
							  "n.nspname", "c.relname", NULL,
							  "pg_catalog.pg_table_is_visible(c.oid)");
	}

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	for (i = 0; i < PQntuples(res); i++)
	{
		simple_oid_list_append(oids, atooid(PQgetvalue(res, i, 0)));
	}

	PQclear(res);
	destroyPQExpBuffer(query);
}

/*
 * selectDumpableNamespace: policy-setting subroutine
 *		Mark a namespace as to be dumped or not
 */
static void
selectDumpableNamespace(NamespaceInfo *nsinfo)
{
	/*
	 * If specific tables are being dumped, do not dump any complete
	 * namespaces. If specific namespaces are being dumped, dump just those
	 * namespaces. Otherwise, dump all non-system namespaces.
	 */
	if (table_include_oids.head != NULL)
		nsinfo->dobj.dump = false;
	else if (schema_include_oids.head != NULL)
		nsinfo->dobj.dump = simple_oid_list_member(&schema_include_oids,
												   nsinfo->dobj.catId.oid);
	else if (strncmp(nsinfo->dobj.name, "pg_", 3) == 0 ||
			 strcmp(nsinfo->dobj.name, "information_schema") == 0)
		nsinfo->dobj.dump = false;
	else
		nsinfo->dobj.dump = true;
	/*
	 * In any case, a namespace can be excluded by an exclusion switch
	 */
	if (nsinfo->dobj.dump &&
		simple_oid_list_member(&schema_exclude_oids,
							   nsinfo->dobj.catId.oid))
		nsinfo->dobj.dump = false;
}

/*
 * selectDumpableTable: policy-setting subroutine
 *		Mark a table as to be dumped or not
 */
static void
selectDumpableTable(TableInfo *tbinfo)
{
	/*
	 * If specific tables are being dumped, dump just those tables;
	 * else, dump according to the parent namespace's dump flag.
	 */
	if (table_include_oids.head != NULL)
		tbinfo->dobj.dump = simple_oid_list_member(&table_include_oids,
												   tbinfo->dobj.catId.oid);
	else
		tbinfo->dobj.dump = tbinfo->dobj.namespace->dobj.dump;
	/*
	 * In any case, a table can be excluded by an exclusion switch
	 */
	if (tbinfo->dobj.dump &&
		simple_oid_list_member(&table_exclude_oids,
							   tbinfo->dobj.catId.oid))
		tbinfo->dobj.dump = false;
}

/*
 * selectDumpableType: policy-setting subroutine
 *		Mark a type as to be dumped or not
 */
static void
selectDumpableType(TypeInfo *tinfo)
{
	/* Dump only types in dumpable namespaces */
	if (!tinfo->dobj.namespace->dobj.dump)
		tinfo->dobj.dump = false;

	/* skip complex types, except for standalone composite types */
	/* (note: this test should now be unnecessary) */
	else if (OidIsValid(tinfo->typrelid) &&
			 tinfo->typrelkind != RELKIND_COMPOSITE_TYPE)
		tinfo->dobj.dump = false;

	/* skip undefined placeholder types */
	else if (!tinfo->isDefined)
		tinfo->dobj.dump = false;

	/* skip all array types that start w/ underscore */
	else if ((tinfo->dobj.name[0] == '_') &&
			 OidIsValid(tinfo->typelem))
		tinfo->dobj.dump = false;

	else
		tinfo->dobj.dump = true;
}

/*
 * selectDumpableObject: policy-setting subroutine
 *		Mark a generic dumpable object as to be dumped or not
 *
 * Use this only for object types without a special-case routine above.
 */
static void
selectDumpableObject(DumpableObject *dobj)
{
	/*
	 * Default policy is to dump if parent namespace is dumpable, or always
	 * for non-namespace-associated items.
	 */
	if (dobj->namespace)
		dobj->dump = dobj->namespace->dobj.dump;
	else
		dobj->dump = true;
}

/*
 *	Dump a table's contents for loading using the COPY command
 *	- this routine is called by the Archiver when it wants the table
 *	  to be dumped.
 */

static int
dumpTableData_copy(Archive *fout, void *dcontext)
{
	TableDataInfo *tdinfo = (TableDataInfo *) dcontext;
	TableInfo  *tbinfo = tdinfo->tdtable;
	const char *classname = tbinfo->dobj.name;
	const bool	hasoids = tbinfo->hasoids;
	const bool	oids = tdinfo->oids;
	PQExpBuffer q = createPQExpBuffer();
	PGresult   *res;
	int			ret;
	char	   *copybuf;
	const char *column_list;

	if (g_verbose)
		write_msg(NULL, "dumping contents of table %s\n", classname);

	/*
	 * Make sure we are in proper schema.  We will qualify the table name
	 * below anyway (in case its name conflicts with a pg_catalog table); but
	 * this ensures reproducible results in case the table contains regproc,
	 * regclass, etc columns.
	 */
	selectSourceSchema(tbinfo->dobj.namespace->dobj.name);

	/*
	 * If possible, specify the column list explicitly so that we have no
	 * possibility of retrieving data in the wrong column order.  (The default
	 * column ordering of COPY will not be what we want in certain corner
	 * cases involving ADD COLUMN and inheritance.)
	 */
	if (g_fout->remoteVersion >= 70300)
		column_list = fmtCopyColumnList(tbinfo);
	else
		column_list = "";		/* can't select columns in COPY */

	if (oids && hasoids)
	{
		appendPQExpBuffer(q, "COPY %s %s WITH OIDS TO stdout;",
						  fmtQualifiedId(tbinfo->dobj.namespace->dobj.name,
										 classname),
						  column_list);
	}
	else
	{
		appendPQExpBuffer(q, "COPY %s %s TO stdout;",
						  fmtQualifiedId(tbinfo->dobj.namespace->dobj.name,
										 classname),
						  column_list);
	}
	res = PQexec(g_conn, q->data);
	check_sql_result(res, g_conn, q->data, PGRES_COPY_OUT);
	PQclear(res);

	for (;;)
	{
		ret = PQgetCopyData(g_conn, &copybuf, 0);

		if (ret < 0)
			break;				/* done or error */

		if (copybuf)
		{
			WriteData(fout, copybuf, ret);
			PQfreemem(copybuf);
		}

		/*
		 * THROTTLE:
		 *
		 * There was considerable discussion in late July, 2000 regarding
		 * slowing down pg_dump when backing up large tables. Users with both
		 * slow & fast (multi-processor) machines experienced performance
		 * degradation when doing a backup.
		 *
		 * Initial attempts based on sleeping for a number of ms for each ms
		 * of work were deemed too complex, then a simple 'sleep in each loop'
		 * implementation was suggested. The latter failed because the loop
		 * was too tight. Finally, the following was implemented:
		 *
		 * If throttle is non-zero, then See how long since the last sleep.
		 * Work out how long to sleep (based on ratio). If sleep is more than
		 * 100ms, then sleep reset timer EndIf EndIf
		 *
		 * where the throttle value was the number of ms to sleep per ms of
		 * work. The calculation was done in each loop.
		 *
		 * Most of the hard work is done in the backend, and this solution
		 * still did not work particularly well: on slow machines, the ratio
		 * was 50:1, and on medium paced machines, 1:1, and on fast
		 * multi-processor machines, it had little or no effect, for reasons
		 * that were unclear.
		 *
		 * Further discussion ensued, and the proposal was dropped.
		 *
		 * For those people who want this feature, it can be implemented using
		 * gettimeofday in each loop, calculating the time since last sleep,
		 * multiplying that by the sleep ratio, then if the result is more
		 * than a preset 'minimum sleep time' (say 100ms), call the 'select'
		 * function to sleep for a subsecond period ie.
		 *
		 * select(0, NULL, NULL, NULL, &tvi);
		 *
		 * This will return after the interval specified in the structure tvi.
		 * Finally, call gettimeofday again to save the 'last sleep time'.
		 */
	}
	archprintf(fout, "\\.\n\n\n");

	if (ret == -2)
	{
		/* copy data transfer failed */
		write_msg(NULL, "Dumping the contents of table \"%s\" failed: PQgetCopyData() failed.\n", classname);
		write_msg(NULL, "Error message from server: %s", PQerrorMessage(g_conn));
		write_msg(NULL, "The command was: %s\n", q->data);
		exit_nicely();
	}

	/* Check command status and return to normal libpq state */
	res = PQgetResult(g_conn);
	check_sql_result(res, g_conn, q->data, PGRES_COMMAND_OK);
	PQclear(res);

	destroyPQExpBuffer(q);
	return 1;
}

static int
dumpTableData_insert(Archive *fout, void *dcontext)
{
	TableDataInfo *tdinfo = (TableDataInfo *) dcontext;
	TableInfo  *tbinfo = tdinfo->tdtable;
	const char *classname = tbinfo->dobj.name;
	PQExpBuffer q = createPQExpBuffer();
	PGresult   *res;
	int			tuple;
	int			nfields;
	int			field;

	/*
	 * Make sure we are in proper schema.  We will qualify the table name
	 * below anyway (in case its name conflicts with a pg_catalog table); but
	 * this ensures reproducible results in case the table contains regproc,
	 * regclass, etc columns.
	 */
	selectSourceSchema(tbinfo->dobj.namespace->dobj.name);

	if (fout->remoteVersion >= 70100)
	{
		appendPQExpBuffer(q, "DECLARE _pg_dump_cursor CURSOR FOR "
						  "SELECT * FROM ONLY %s",
						  fmtQualifiedId(tbinfo->dobj.namespace->dobj.name,
										 classname));
	}
	else
	{
		appendPQExpBuffer(q, "DECLARE _pg_dump_cursor CURSOR FOR "
						  "SELECT * FROM %s",
						  fmtQualifiedId(tbinfo->dobj.namespace->dobj.name,
										 classname));
	}

	res = PQexec(g_conn, q->data);
	check_sql_result(res, g_conn, q->data, PGRES_COMMAND_OK);

	do
	{
		PQclear(res);

		res = PQexec(g_conn, "FETCH 100 FROM _pg_dump_cursor");
		check_sql_result(res, g_conn, "FETCH 100 FROM _pg_dump_cursor",
						 PGRES_TUPLES_OK);
		nfields = PQnfields(res);
		for (tuple = 0; tuple < PQntuples(res); tuple++)
		{
			archprintf(fout, "INSERT INTO %s ", fmtId(classname));
			if (nfields == 0)
			{
				/* corner case for zero-column table */
				archprintf(fout, "DEFAULT VALUES;\n");
				continue;
			}
			if (attrNames == true)
			{
				resetPQExpBuffer(q);
				appendPQExpBuffer(q, "(");
				for (field = 0; field < nfields; field++)
				{
					if (field > 0)
						appendPQExpBuffer(q, ", ");
					appendPQExpBufferStr(q, fmtId(PQfname(res, field)));
				}
				appendPQExpBuffer(q, ") ");
				archputs(q->data, fout);
			}
			archprintf(fout, "VALUES (");
			for (field = 0; field < nfields; field++)
			{
				if (field > 0)
					archprintf(fout, ", ");
				if (PQgetisnull(res, tuple, field))
				{
					archprintf(fout, "NULL");
					continue;
				}

				/* XXX This code is partially duplicated in ruleutils.c */
				switch (PQftype(res, field))
				{
					case INT2OID:
					case INT4OID:
					case INT8OID:
					case OIDOID:
					case FLOAT4OID:
					case FLOAT8OID:
					case NUMERICOID:
						{
							/*
							 * These types are printed without quotes unless
							 * they contain values that aren't accepted by the
							 * scanner unquoted (e.g., 'NaN').	Note that
							 * strtod() and friends might accept NaN, so we
							 * can't use that to test.
							 *
							 * In reality we only need to defend against
							 * infinity and NaN, so we need not get too crazy
							 * about pattern matching here.
							 */
							const char *s = PQgetvalue(res, tuple, field);

							if (strspn(s, "0123456789 +-eE.") == strlen(s))
								archprintf(fout, "%s", s);
							else
								archprintf(fout, "'%s'", s);
						}
						break;

					case BITOID:
					case VARBITOID:
						archprintf(fout, "B'%s'",
								   PQgetvalue(res, tuple, field));
						break;

					case BOOLOID:
						if (strcmp(PQgetvalue(res, tuple, field), "t") == 0)
							archprintf(fout, "true");
						else
							archprintf(fout, "false");
						break;

					default:
						/* All other types are printed as string literals. */
						resetPQExpBuffer(q);
						appendStringLiteralAH(q,
											  PQgetvalue(res, tuple, field),
											  fout);
						archputs(q->data, fout);
						break;
				}
			}
			archprintf(fout, ");\n");
		}
	} while (PQntuples(res) > 0);

	PQclear(res);

	archprintf(fout, "\n\n");

	do_sql_command(g_conn, "CLOSE _pg_dump_cursor");

	destroyPQExpBuffer(q);
	return 1;
}


/*
 * dumpTableData -
 *	  dump the contents of a single table
 *
 * Actually, this just makes an ArchiveEntry for the table contents.
 */
static void
dumpTableData(Archive *fout, TableDataInfo *tdinfo)
{
	TableInfo  *tbinfo = tdinfo->tdtable;
	PQExpBuffer copyBuf = createPQExpBuffer();
	DataDumperPtr dumpFn;
	char	   *copyStmt;

	if (!dumpInserts)
	{
		/* Dump/restore using COPY */
		dumpFn = dumpTableData_copy;
		/* must use 2 steps here 'cause fmtId is nonreentrant */
		appendPQExpBuffer(copyBuf, "COPY %s ",
						  fmtId(tbinfo->dobj.name));
		appendPQExpBuffer(copyBuf, "%s %sFROM stdin;\n",
						  fmtCopyColumnList(tbinfo),
					  (tdinfo->oids && tbinfo->hasoids) ? "WITH OIDS " : "");
		copyStmt = copyBuf->data;
	}
	else
	{
		/* Restore using INSERT */
		dumpFn = dumpTableData_insert;
		copyStmt = NULL;
	}

	ArchiveEntry(fout, tdinfo->dobj.catId, tdinfo->dobj.dumpId,
				 tbinfo->dobj.name,
				 tbinfo->dobj.namespace->dobj.name,
				 NULL,
				 tbinfo->rolname, false,
				 "TABLE DATA", "", "", copyStmt,
				 tdinfo->dobj.dependencies, tdinfo->dobj.nDeps,
				 dumpFn, tdinfo);

	destroyPQExpBuffer(copyBuf);
}

/*
 * getTableData -
 *	  set up dumpable objects representing the contents of tables
 */
static void
getTableData(TableInfo *tblinfo, int numTables, bool oids)
{
	int			i;

	for (i = 0; i < numTables; i++)
	{
		/* Skip VIEWs (no data to dump) */
		if (tblinfo[i].relkind == RELKIND_VIEW)
			continue;
		/* Skip SEQUENCEs (handled elsewhere) */
		if (tblinfo[i].relkind == RELKIND_SEQUENCE)
			continue;

		if (tblinfo[i].dobj.dump)
		{
			TableDataInfo *tdinfo;

			tdinfo = (TableDataInfo *) malloc(sizeof(TableDataInfo));

			tdinfo->dobj.objType = DO_TABLE_DATA;

			/*
			 * Note: use tableoid 0 so that this object won't be mistaken for
			 * something that pg_depend entries apply to.
			 */
			tdinfo->dobj.catId.tableoid = 0;
			tdinfo->dobj.catId.oid = tblinfo[i].dobj.catId.oid;
			AssignDumpId(&tdinfo->dobj);
			tdinfo->dobj.name = tblinfo[i].dobj.name;
			tdinfo->dobj.namespace = tblinfo[i].dobj.namespace;
			tdinfo->tdtable = &(tblinfo[i]);
			tdinfo->oids = oids;
			addObjectDependency(&tdinfo->dobj, tblinfo[i].dobj.dumpId);
		}
	}
}


/*
 * dumpDatabase:
 *	dump the database definition
 */
static void
dumpDatabase(Archive *AH)
{
	PQExpBuffer dbQry = createPQExpBuffer();
	PQExpBuffer delQry = createPQExpBuffer();
	PQExpBuffer creaQry = createPQExpBuffer();
	PGresult   *res;
	int			ntups;
	int			i_tableoid,
				i_oid,
				i_dba,
				i_encoding,
				i_tablespace;
	CatalogId	dbCatId;
	DumpId		dbDumpId;
	const char *datname,
			   *dba,
			   *encoding,
			   *tablespace;

	datname = PQdb(g_conn);

	if (g_verbose)
		write_msg(NULL, "saving database definition\n");

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	/* Get the database owner and parameters from pg_database */
	if (g_fout->remoteVersion >= 80200)
	{
		appendPQExpBuffer(dbQry, "SELECT tableoid, oid, "
						  "(%s datdba) as dba, "
						  "pg_encoding_to_char(encoding) as encoding, "
						  "(SELECT spcname FROM pg_tablespace t WHERE t.oid = dattablespace) as tablespace, "
					  "shobj_description(oid, 'pg_database') as description "

						  "FROM pg_database "
						  "WHERE datname = ",
						  username_subquery);
		appendStringLiteralAH(dbQry, datname, AH);
	}
	else if (g_fout->remoteVersion >= 80000)
	{
		appendPQExpBuffer(dbQry, "SELECT tableoid, oid, "
						  "(%s datdba) as dba, "
						  "pg_encoding_to_char(encoding) as encoding, "
						  "(SELECT spcname FROM pg_tablespace t WHERE t.oid = dattablespace) as tablespace "
						  "FROM pg_database "
						  "WHERE datname = ",
						  username_subquery);
		appendStringLiteralAH(dbQry, datname, AH);
	}
	else if (g_fout->remoteVersion >= 70100)
	{
		appendPQExpBuffer(dbQry, "SELECT tableoid, oid, "
						  "(%s datdba) as dba, "
						  "pg_encoding_to_char(encoding) as encoding, "
						  "NULL as tablespace "
						  "FROM pg_database "
						  "WHERE datname = ",
						  username_subquery);
		appendStringLiteralAH(dbQry, datname, AH);
	}
	else
	{
		appendPQExpBuffer(dbQry, "SELECT "
						  "(SELECT oid FROM pg_class WHERE relname = 'pg_database') AS tableoid, "
						  "oid, "
						  "(%s datdba) as dba, "
						  "pg_encoding_to_char(encoding) as encoding, "
						  "NULL as tablespace "
						  "FROM pg_database "
						  "WHERE datname = ",
						  username_subquery);
		appendStringLiteralAH(dbQry, datname, AH);
	}

	res = PQexec(g_conn, dbQry->data);
	check_sql_result(res, g_conn, dbQry->data, PGRES_TUPLES_OK);

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

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_dba = PQfnumber(res, "dba");
	i_encoding = PQfnumber(res, "encoding");
	i_tablespace = PQfnumber(res, "tablespace");

	dbCatId.tableoid = atooid(PQgetvalue(res, 0, i_tableoid));
	dbCatId.oid = atooid(PQgetvalue(res, 0, i_oid));
	dba = PQgetvalue(res, 0, i_dba);
	encoding = PQgetvalue(res, 0, i_encoding);
	tablespace = PQgetvalue(res, 0, i_tablespace);

	appendPQExpBuffer(creaQry, "CREATE DATABASE %s WITH TEMPLATE = template0",
					  fmtId(datname));
	if (strlen(encoding) > 0)
	{
		appendPQExpBuffer(creaQry, " ENCODING = ");
		appendStringLiteralAH(creaQry, encoding, AH);
	}
	if (strlen(tablespace) > 0 && strcmp(tablespace, "pg_default") != 0)
		appendPQExpBuffer(creaQry, " TABLESPACE = %s",
						  fmtId(tablespace));
	appendPQExpBuffer(creaQry, ";\n");

	appendPQExpBuffer(delQry, "DROP DATABASE %s;\n",
					  fmtId(datname));

	dbDumpId = createDumpId();

	ArchiveEntry(AH,
				 dbCatId,		/* catalog ID */
				 dbDumpId,		/* dump ID */
				 datname,		/* Name */
				 NULL,			/* Namespace */
				 NULL,			/* Tablespace */
				 dba,			/* Owner */
				 false,			/* with oids */
				 "DATABASE",	/* Desc */
				 creaQry->data, /* Create */
				 delQry->data,	/* Del */
				 NULL,			/* Copy */
				 NULL,			/* Deps */
				 0,				/* # Deps */
				 NULL,			/* Dumper */
				 NULL);			/* Dumper Arg */

	/* Dump DB comment if any */
	if (g_fout->remoteVersion >= 80200)
	{
		/*
		 * 8.2 keeps comments on shared objects in a shared table, so we
		 * cannot use the dumpComment used for other database objects.
		 */
		char	   *comment = PQgetvalue(res, 0, PQfnumber(res, "description"));

		if (comment && strlen(comment))
		{
			resetPQExpBuffer(dbQry);
			appendPQExpBuffer(dbQry, "COMMENT ON DATABASE %s IS ", fmtId(datname));
			appendStringLiteralAH(dbQry, comment, AH);
			appendPQExpBuffer(dbQry, ";\n");

			ArchiveEntry(AH, dbCatId, createDumpId(), datname, NULL, NULL,
						 dba, false, "COMMENT", dbQry->data, "", NULL,
						 &dbDumpId, 1, NULL, NULL);
		}
	}
	else
	{
		resetPQExpBuffer(dbQry);
		appendPQExpBuffer(dbQry, "DATABASE %s", fmtId(datname));
		dumpComment(AH, dbQry->data, NULL, "",
					dbCatId, 0, dbDumpId);
	}

	PQclear(res);

	destroyPQExpBuffer(dbQry);
	destroyPQExpBuffer(delQry);
	destroyPQExpBuffer(creaQry);
}


/*
 * dumpEncoding: put the correct encoding into the archive
 */
static void
dumpEncoding(Archive *AH)
{
	const char *encname = pg_encoding_to_char(AH->encoding);
	PQExpBuffer qry = createPQExpBuffer();

	if (g_verbose)
		write_msg(NULL, "saving encoding = %s\n", encname);

	appendPQExpBuffer(qry, "SET client_encoding = ");
	appendStringLiteralAH(qry, encname, AH);
	appendPQExpBuffer(qry, ";\n");

	ArchiveEntry(AH, nilCatalogId, createDumpId(),
				 "ENCODING", NULL, NULL, "",
				 false, "ENCODING", qry->data, "", NULL,
				 NULL, 0,
				 NULL, NULL);

	destroyPQExpBuffer(qry);
}


/*
 * dumpStdStrings: put the correct escape string behavior into the archive
 */
static void
dumpStdStrings(Archive *AH)
{
	const char *stdstrings = AH->std_strings ? "on" : "off";
	PQExpBuffer qry = createPQExpBuffer();

	if (g_verbose)
		write_msg(NULL, "saving standard_conforming_strings = %s\n",
				  stdstrings);

	appendPQExpBuffer(qry, "SET standard_conforming_strings = '%s';\n",
					  stdstrings);

	ArchiveEntry(AH, nilCatalogId, createDumpId(),
				 "STDSTRINGS", NULL, NULL, "",
				 false, "STDSTRINGS", qry->data, "", NULL,
				 NULL, 0,
				 NULL, NULL);

	destroyPQExpBuffer(qry);
}


/*
 * hasBlobs:
 *	Test whether database contains any large objects
 */
static bool
hasBlobs(Archive *AH)
{
	bool		result;
	const char *blobQry;
	PGresult   *res;

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	/* Check for BLOB OIDs */
	if (AH->remoteVersion >= 70100)
		blobQry = "SELECT loid FROM pg_largeobject LIMIT 1";
	else
		blobQry = "SELECT oid FROM pg_class WHERE relkind = 'l' LIMIT 1";

	res = PQexec(g_conn, blobQry);
	check_sql_result(res, g_conn, blobQry, PGRES_TUPLES_OK);

	result = PQntuples(res) > 0;

	PQclear(res);

	return result;
}

/*
 * dumpBlobs:
 *	dump all blobs
 */
static int
dumpBlobs(Archive *AH, void *arg)
{
	const char *blobQry;
	const char *blobFetchQry;
	PGresult   *res;
	char		buf[LOBBUFSIZE];
	int			i;
	int			cnt;

	if (g_verbose)
		write_msg(NULL, "saving large objects\n");

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	/* Cursor to get all BLOB OIDs */
	if (AH->remoteVersion >= 70100)
		blobQry = "DECLARE bloboid CURSOR FOR SELECT DISTINCT loid FROM pg_largeobject";
	else
		blobQry = "DECLARE bloboid CURSOR FOR SELECT oid FROM pg_class WHERE relkind = 'l'";

	res = PQexec(g_conn, blobQry);
	check_sql_result(res, g_conn, blobQry, PGRES_COMMAND_OK);

	/* Command to fetch from cursor */
	blobFetchQry = "FETCH 1000 IN bloboid";

	do
	{
		PQclear(res);

		/* Do a fetch */
		res = PQexec(g_conn, blobFetchQry);
		check_sql_result(res, g_conn, blobFetchQry, PGRES_TUPLES_OK);

		/* Process the tuples, if any */
		for (i = 0; i < PQntuples(res); i++)
		{
			Oid			blobOid;
			int			loFd;

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
				cnt = lo_read(g_conn, loFd, buf, LOBBUFSIZE);
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

	PQclear(res);

	return 1;
}

/*
 * dumpBlobComments
 *	dump all blob comments
 *
 * Since we don't provide any way to be selective about dumping blobs,
 * there's no need to be selective about their comments either.  We put
 * all the comments into one big TOC entry.
 */
static int
dumpBlobComments(Archive *AH, void *arg)
{
	const char *blobQry;
	const char *blobFetchQry;
	PQExpBuffer commentcmd = createPQExpBuffer();
	PGresult   *res;
	int			i;

	if (g_verbose)
		write_msg(NULL, "saving large object comments\n");

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	/* Cursor to get all BLOB comments */
	if (AH->remoteVersion >= 70200)
		blobQry = "DECLARE blobcmt CURSOR FOR SELECT loid, obj_description(loid, 'pg_largeobject') FROM (SELECT DISTINCT loid FROM pg_largeobject) ss";
	else if (AH->remoteVersion >= 70100)
		blobQry = "DECLARE blobcmt CURSOR FOR SELECT loid, obj_description(loid) FROM (SELECT DISTINCT loid FROM pg_largeobject) ss";
	else
		blobQry = "DECLARE blobcmt CURSOR FOR SELECT oid, (SELECT description FROM pg_description pd WHERE pd.objoid=pc.oid) FROM pg_class pc WHERE relkind = 'l'";

	res = PQexec(g_conn, blobQry);
	check_sql_result(res, g_conn, blobQry, PGRES_COMMAND_OK);

	/* Command to fetch from cursor */
	blobFetchQry = "FETCH 100 IN blobcmt";

	do
	{
		PQclear(res);

		/* Do a fetch */
		res = PQexec(g_conn, blobFetchQry);
		check_sql_result(res, g_conn, blobFetchQry, PGRES_TUPLES_OK);

		/* Process the tuples, if any */
		for (i = 0; i < PQntuples(res); i++)
		{
			Oid			blobOid;
			char	   *comment;

			/* ignore blobs without comments */
			if (PQgetisnull(res, i, 1))
				continue;

			blobOid = atooid(PQgetvalue(res, i, 0));
			comment = PQgetvalue(res, i, 1);

			printfPQExpBuffer(commentcmd, "COMMENT ON LARGE OBJECT %u IS ",
							  blobOid);
			appendStringLiteralAH(commentcmd, comment, AH);
			appendPQExpBuffer(commentcmd, ";\n");

			archputs(commentcmd->data, AH);
		}
	} while (PQntuples(res) > 0);

	PQclear(res);

	archputs("\n", AH);

	destroyPQExpBuffer(commentcmd);

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
	int			i_tableoid;
	int			i_oid;
	int			i_nspname;
	int			i_rolname;
	int			i_nspacl;

	/*
	 * Before 7.3, there are no real namespaces; create two dummy entries, one
	 * for user stuff and one for system stuff.
	 */
	if (g_fout->remoteVersion < 70300)
	{
		nsinfo = (NamespaceInfo *) malloc(2 * sizeof(NamespaceInfo));

		nsinfo[0].dobj.objType = DO_NAMESPACE;
		nsinfo[0].dobj.catId.tableoid = 0;
		nsinfo[0].dobj.catId.oid = 0;
		AssignDumpId(&nsinfo[0].dobj);
		nsinfo[0].dobj.name = strdup("public");
		nsinfo[0].rolname = strdup("");
		nsinfo[0].nspacl = strdup("");

		selectDumpableNamespace(&nsinfo[0]);

		nsinfo[1].dobj.objType = DO_NAMESPACE;
		nsinfo[1].dobj.catId.tableoid = 0;
		nsinfo[1].dobj.catId.oid = 1;
		AssignDumpId(&nsinfo[1].dobj);
		nsinfo[1].dobj.name = strdup("pg_catalog");
		nsinfo[1].rolname = strdup("");
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
	 * we fetch all namespaces including system ones, so that every object we
	 * read in can be linked to a containing namespace.
	 */
	appendPQExpBuffer(query, "SELECT tableoid, oid, nspname, "
					  "(%s nspowner) as rolname, "
					  "nspacl FROM pg_namespace",
					  username_subquery);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	nsinfo = (NamespaceInfo *) malloc(ntups * sizeof(NamespaceInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_nspname = PQfnumber(res, "nspname");
	i_rolname = PQfnumber(res, "rolname");
	i_nspacl = PQfnumber(res, "nspacl");

	for (i = 0; i < ntups; i++)
	{
		nsinfo[i].dobj.objType = DO_NAMESPACE;
		nsinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		nsinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&nsinfo[i].dobj);
		nsinfo[i].dobj.name = strdup(PQgetvalue(res, i, i_nspname));
		nsinfo[i].rolname = strdup(PQgetvalue(res, i, i_rolname));
		nsinfo[i].nspacl = strdup(PQgetvalue(res, i, i_nspacl));

		/* Decide whether to dump this namespace */
		selectDumpableNamespace(&nsinfo[i]);

		if (strlen(nsinfo[i].rolname) == 0)
			write_msg(NULL, "WARNING: owner of schema \"%s\" appears to be invalid\n",
					  nsinfo[i].dobj.name);
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
 * a system object or not.	In 7.3 and later there is no guessing.
 */
static NamespaceInfo *
findNamespace(Oid nsoid, Oid objoid)
{
	int			i;

	if (g_fout->remoteVersion >= 70300)
	{
		for (i = 0; i < g_numNamespaces; i++)
		{
			NamespaceInfo *nsinfo = &g_namespaces[i];

			if (nsoid == nsinfo->dobj.catId.oid)
				return nsinfo;
		}
		write_msg(NULL, "schema with OID %u does not exist\n", nsoid);
		exit_nicely();
	}
	else
	{
		/* This code depends on the layout set up by getNamespaces. */
		if (objoid > g_last_builtin_oid)
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
 *
 * NB: this must run after getFuncs() because we assume we can do
 * findFuncByOid().
 */
TypeInfo *
getTypes(int *numTypes)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	TypeInfo   *tinfo;
	ShellTypeInfo *stinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_typname;
	int			i_typnamespace;
	int			i_rolname;
	int			i_typinput;
	int			i_typoutput;
	int			i_typelem;
	int			i_typrelid;
	int			i_typrelkind;
	int			i_typtype;
	int			i_typisdefined;

	/*
	 * we include even the built-in types because those may be used as array
	 * elements by user-defined types
	 *
	 * we filter out the built-in types when we dump out the types
	 *
	 * same approach for undefined (shell) types
	 */

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	if (g_fout->remoteVersion >= 70300)
	{
		appendPQExpBuffer(query, "SELECT tableoid, oid, typname, "
						  "typnamespace, "
						  "(%s typowner) as rolname, "
						  "typinput::oid as typinput, "
						  "typoutput::oid as typoutput, typelem, typrelid, "
						  "CASE WHEN typrelid = 0 THEN ' '::\"char\" "
						  "ELSE (SELECT relkind FROM pg_class WHERE oid = typrelid) END as typrelkind, "
						  "typtype, typisdefined "
						  "FROM pg_type",
						  username_subquery);
	}
	else if (g_fout->remoteVersion >= 70100)
	{
		appendPQExpBuffer(query, "SELECT tableoid, oid, typname, "
						  "0::oid as typnamespace, "
						  "(%s typowner) as rolname, "
						  "typinput::oid as typinput, "
						  "typoutput::oid as typoutput, typelem, typrelid, "
						  "CASE WHEN typrelid = 0 THEN ' '::\"char\" "
						  "ELSE (SELECT relkind FROM pg_class WHERE oid = typrelid) END as typrelkind, "
						  "typtype, typisdefined "
						  "FROM pg_type",
						  username_subquery);
	}
	else
	{
		appendPQExpBuffer(query, "SELECT "
		 "(SELECT oid FROM pg_class WHERE relname = 'pg_type') AS tableoid, "
						  "oid, typname, "
						  "0::oid as typnamespace, "
						  "(%s typowner) as rolname, "
						  "typinput::oid as typinput, "
						  "typoutput::oid as typoutput, typelem, typrelid, "
						  "CASE WHEN typrelid = 0 THEN ' '::\"char\" "
						  "ELSE (SELECT relkind FROM pg_class WHERE oid = typrelid) END as typrelkind, "
						  "typtype, typisdefined "
						  "FROM pg_type",
						  username_subquery);
	}

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	tinfo = (TypeInfo *) malloc(ntups * sizeof(TypeInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_typname = PQfnumber(res, "typname");
	i_typnamespace = PQfnumber(res, "typnamespace");
	i_rolname = PQfnumber(res, "rolname");
	i_typinput = PQfnumber(res, "typinput");
	i_typoutput = PQfnumber(res, "typoutput");
	i_typelem = PQfnumber(res, "typelem");
	i_typrelid = PQfnumber(res, "typrelid");
	i_typrelkind = PQfnumber(res, "typrelkind");
	i_typtype = PQfnumber(res, "typtype");
	i_typisdefined = PQfnumber(res, "typisdefined");

	for (i = 0; i < ntups; i++)
	{
		tinfo[i].dobj.objType = DO_TYPE;
		tinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		tinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&tinfo[i].dobj);
		tinfo[i].dobj.name = strdup(PQgetvalue(res, i, i_typname));
		tinfo[i].dobj.namespace = findNamespace(atooid(PQgetvalue(res, i, i_typnamespace)),
												tinfo[i].dobj.catId.oid);
		tinfo[i].rolname = strdup(PQgetvalue(res, i, i_rolname));
		tinfo[i].typelem = atooid(PQgetvalue(res, i, i_typelem));
		tinfo[i].typrelid = atooid(PQgetvalue(res, i, i_typrelid));
		tinfo[i].typrelkind = *PQgetvalue(res, i, i_typrelkind);
		tinfo[i].typtype = *PQgetvalue(res, i, i_typtype);
		tinfo[i].shellType = NULL;

		/*
		 * If it's a table's rowtype, use special type code to facilitate
		 * sorting into the desired order.	(We don't want to consider it an
		 * ordinary type because that would bring the table up into the
		 * datatype part of the dump order.)
		 */
		if (OidIsValid(tinfo[i].typrelid) &&
			tinfo[i].typrelkind != RELKIND_COMPOSITE_TYPE)
			tinfo[i].dobj.objType = DO_TABLE_TYPE;

		/*
		 * check for user-defined array types, omit system generated ones
		 */
		if (OidIsValid(tinfo[i].typelem) &&
			tinfo[i].dobj.name[0] != '_')
			tinfo[i].isArray = true;
		else
			tinfo[i].isArray = false;

		if (strcmp(PQgetvalue(res, i, i_typisdefined), "t") == 0)
			tinfo[i].isDefined = true;
		else
			tinfo[i].isDefined = false;

		/* Decide whether we want to dump it */
		selectDumpableType(&tinfo[i]);

		/*
		 * If it's a domain, fetch info about its constraints, if any
		 */
		tinfo[i].nDomChecks = 0;
		tinfo[i].domChecks = NULL;
		if (tinfo[i].dobj.dump && tinfo[i].typtype == 'd')
			getDomainConstraints(&(tinfo[i]));

		/*
		 * If it's a base type, make a DumpableObject representing a shell
		 * definition of the type.	We will need to dump that ahead of the I/O
		 * functions for the type.
		 *
		 * Note: the shell type doesn't have a catId.  You might think it
		 * should copy the base type's catId, but then it might capture the
		 * pg_depend entries for the type, which we don't want.
		 */
		if (tinfo[i].dobj.dump && tinfo[i].typtype == 'b')
		{
			stinfo = (ShellTypeInfo *) malloc(sizeof(ShellTypeInfo));
			stinfo->dobj.objType = DO_SHELL_TYPE;
			stinfo->dobj.catId = nilCatalogId;
			AssignDumpId(&stinfo->dobj);
			stinfo->dobj.name = strdup(tinfo[i].dobj.name);
			stinfo->dobj.namespace = tinfo[i].dobj.namespace;
			stinfo->baseType = &(tinfo[i]);
			tinfo[i].shellType = stinfo;

			/*
			 * Initially mark the shell type as not to be dumped.  We'll only
			 * dump it if the I/O functions need to be dumped; this is taken
			 * care of while sorting dependencies.
			 */
			stinfo->dobj.dump = false;

			/*
			 * However, if dumping from pre-7.3, there will be no dependency
			 * info so we have to fake it here.  We only need to worry about
			 * typinput and typoutput since the other functions only exist
			 * post-7.3.
			 */
			if (g_fout->remoteVersion < 70300)
			{
				Oid			typinput;
				Oid			typoutput;
				FuncInfo   *funcInfo;

				typinput = atooid(PQgetvalue(res, i, i_typinput));
				typoutput = atooid(PQgetvalue(res, i, i_typoutput));

				funcInfo = findFuncByOid(typinput);
				if (funcInfo && funcInfo->dobj.dump)
				{
					/* base type depends on function */
					addObjectDependency(&tinfo[i].dobj,
										funcInfo->dobj.dumpId);
					/* function depends on shell type */
					addObjectDependency(&funcInfo->dobj,
										stinfo->dobj.dumpId);
					/* mark shell type as to be dumped */
					stinfo->dobj.dump = true;
				}

				funcInfo = findFuncByOid(typoutput);
				if (funcInfo && funcInfo->dobj.dump)
				{
					/* base type depends on function */
					addObjectDependency(&tinfo[i].dobj,
										funcInfo->dobj.dumpId);
					/* function depends on shell type */
					addObjectDependency(&funcInfo->dobj,
										stinfo->dobj.dumpId);
					/* mark shell type as to be dumped */
					stinfo->dobj.dump = true;
				}
			}
		}

		if (strlen(tinfo[i].rolname) == 0 && tinfo[i].isDefined)
			write_msg(NULL, "WARNING: owner of data type \"%s\" appears to be invalid\n",
					  tinfo[i].dobj.name);
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
	int			i_tableoid;
	int			i_oid;
	int			i_oprname;
	int			i_oprnamespace;
	int			i_rolname;
	int			i_oprcode;

	/*
	 * find all operators, including builtin operators; we filter out
	 * system-defined operators at dump-out time.
	 */

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	if (g_fout->remoteVersion >= 70300)
	{
		appendPQExpBuffer(query, "SELECT tableoid, oid, oprname, "
						  "oprnamespace, "
						  "(%s oprowner) as rolname, "
						  "oprcode::oid as oprcode "
						  "FROM pg_operator",
						  username_subquery);
	}
	else if (g_fout->remoteVersion >= 70100)
	{
		appendPQExpBuffer(query, "SELECT tableoid, oid, oprname, "
						  "0::oid as oprnamespace, "
						  "(%s oprowner) as rolname, "
						  "oprcode::oid as oprcode "
						  "FROM pg_operator",
						  username_subquery);
	}
	else
	{
		appendPQExpBuffer(query, "SELECT "
						  "(SELECT oid FROM pg_class WHERE relname = 'pg_operator') AS tableoid, "
						  "oid, oprname, "
						  "0::oid as oprnamespace, "
						  "(%s oprowner) as rolname, "
						  "oprcode::oid as oprcode "
						  "FROM pg_operator",
						  username_subquery);
	}

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numOprs = ntups;

	oprinfo = (OprInfo *) malloc(ntups * sizeof(OprInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_oprname = PQfnumber(res, "oprname");
	i_oprnamespace = PQfnumber(res, "oprnamespace");
	i_rolname = PQfnumber(res, "rolname");
	i_oprcode = PQfnumber(res, "oprcode");

	for (i = 0; i < ntups; i++)
	{
		oprinfo[i].dobj.objType = DO_OPERATOR;
		oprinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		oprinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&oprinfo[i].dobj);
		oprinfo[i].dobj.name = strdup(PQgetvalue(res, i, i_oprname));
		oprinfo[i].dobj.namespace = findNamespace(atooid(PQgetvalue(res, i, i_oprnamespace)),
												  oprinfo[i].dobj.catId.oid);
		oprinfo[i].rolname = strdup(PQgetvalue(res, i, i_rolname));
		oprinfo[i].oprcode = atooid(PQgetvalue(res, i, i_oprcode));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(oprinfo[i].dobj));

		if (strlen(oprinfo[i].rolname) == 0)
			write_msg(NULL, "WARNING: owner of operator \"%s\" appears to be invalid\n",
					  oprinfo[i].dobj.name);
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return oprinfo;
}

/*
 * getConversions:
 *	  read all conversions in the system catalogs and return them in the
 * ConvInfo* structure
 *
 *	numConversions is set to the number of conversions read in
 */
ConvInfo *
getConversions(int *numConversions)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	ConvInfo   *convinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_conname;
	int			i_connamespace;
	int			i_rolname;

	/* Conversions didn't exist pre-7.3 */
	if (g_fout->remoteVersion < 70300)
	{
		*numConversions = 0;
		return NULL;
	}

	/*
	 * find all conversions, including builtin conversions; we filter out
	 * system-defined conversions at dump-out time.
	 */

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	appendPQExpBuffer(query, "SELECT tableoid, oid, conname, "
					  "connamespace, "
					  "(%s conowner) as rolname "
					  "FROM pg_conversion",
					  username_subquery);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numConversions = ntups;

	convinfo = (ConvInfo *) malloc(ntups * sizeof(ConvInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_conname = PQfnumber(res, "conname");
	i_connamespace = PQfnumber(res, "connamespace");
	i_rolname = PQfnumber(res, "rolname");

	for (i = 0; i < ntups; i++)
	{
		convinfo[i].dobj.objType = DO_CONVERSION;
		convinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		convinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&convinfo[i].dobj);
		convinfo[i].dobj.name = strdup(PQgetvalue(res, i, i_conname));
		convinfo[i].dobj.namespace = findNamespace(atooid(PQgetvalue(res, i, i_connamespace)),
												 convinfo[i].dobj.catId.oid);
		convinfo[i].rolname = strdup(PQgetvalue(res, i, i_rolname));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(convinfo[i].dobj));
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return convinfo;
}

/*
 * getOpclasses:
 *	  read all opclasses in the system catalogs and return them in the
 * OpclassInfo* structure
 *
 *	numOpclasses is set to the number of opclasses read in
 */
OpclassInfo *
getOpclasses(int *numOpclasses)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	OpclassInfo *opcinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_opcname;
	int			i_opcnamespace;
	int			i_rolname;

	/*
	 * find all opclasses, including builtin opclasses; we filter out
	 * system-defined opclasses at dump-out time.
	 */

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	if (g_fout->remoteVersion >= 70300)
	{
		appendPQExpBuffer(query, "SELECT tableoid, oid, opcname, "
						  "opcnamespace, "
						  "(%s opcowner) as rolname "
						  "FROM pg_opclass",
						  username_subquery);
	}
	else if (g_fout->remoteVersion >= 70100)
	{
		appendPQExpBuffer(query, "SELECT tableoid, oid, opcname, "
						  "0::oid as opcnamespace, "
						  "''::name as rolname "
						  "FROM pg_opclass");
	}
	else
	{
		appendPQExpBuffer(query, "SELECT "
						  "(SELECT oid FROM pg_class WHERE relname = 'pg_opclass') AS tableoid, "
						  "oid, opcname, "
						  "0::oid as opcnamespace, "
						  "''::name as rolname "
						  "FROM pg_opclass");
	}

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numOpclasses = ntups;

	opcinfo = (OpclassInfo *) malloc(ntups * sizeof(OpclassInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_opcname = PQfnumber(res, "opcname");
	i_opcnamespace = PQfnumber(res, "opcnamespace");
	i_rolname = PQfnumber(res, "rolname");

	for (i = 0; i < ntups; i++)
	{
		opcinfo[i].dobj.objType = DO_OPCLASS;
		opcinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		opcinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&opcinfo[i].dobj);
		opcinfo[i].dobj.name = strdup(PQgetvalue(res, i, i_opcname));
		opcinfo[i].dobj.namespace = findNamespace(atooid(PQgetvalue(res, i, i_opcnamespace)),
												  opcinfo[i].dobj.catId.oid);
		opcinfo[i].rolname = strdup(PQgetvalue(res, i, i_rolname));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(opcinfo[i].dobj));

		if (g_fout->remoteVersion >= 70300)
		{
			if (strlen(opcinfo[i].rolname) == 0)
				write_msg(NULL, "WARNING: owner of operator class \"%s\" appears to be invalid\n",
						  opcinfo[i].dobj.name);
		}
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return opcinfo;
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
	int			i_tableoid;
	int			i_oid;
	int			i_aggname;
	int			i_aggnamespace;
	int			i_pronargs;
	int			i_proargtypes;
	int			i_rolname;
	int			i_aggacl;

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	/* find all user-defined aggregates */

	if (g_fout->remoteVersion >= 80200)
	{
		appendPQExpBuffer(query, "SELECT tableoid, oid, proname as aggname, "
						  "pronamespace as aggnamespace, "
						  "pronargs, proargtypes, "
						  "(%s proowner) as rolname, "
						  "proacl as aggacl "
						  "FROM pg_proc "
						  "WHERE proisagg "
						  "AND pronamespace != "
			   "(select oid from pg_namespace where nspname = 'pg_catalog')",
						  username_subquery);
	}
	else if (g_fout->remoteVersion >= 70300)
	{
		appendPQExpBuffer(query, "SELECT tableoid, oid, proname as aggname, "
						  "pronamespace as aggnamespace, "
						  "CASE WHEN proargtypes[0] = 'pg_catalog.\"any\"'::pg_catalog.regtype THEN 0 ELSE 1 END as pronargs, "
						  "proargtypes, "
						  "(%s proowner) as rolname, "
						  "proacl as aggacl "
						  "FROM pg_proc "
						  "WHERE proisagg "
						  "AND pronamespace != "
			   "(select oid from pg_namespace where nspname = 'pg_catalog')",
						  username_subquery);
	}
	else if (g_fout->remoteVersion >= 70100)
	{
		appendPQExpBuffer(query, "SELECT tableoid, oid, aggname, "
						  "0::oid as aggnamespace, "
				  "CASE WHEN aggbasetype = 0 THEN 0 ELSE 1 END as pronargs, "
						  "aggbasetype as proargtypes, "
						  "(%s aggowner) as rolname, "
						  "'{=X}' as aggacl "
						  "FROM pg_aggregate "
						  "where oid > '%u'::oid",
						  username_subquery,
						  g_last_builtin_oid);
	}
	else
	{
		appendPQExpBuffer(query, "SELECT "
						  "(SELECT oid FROM pg_class WHERE relname = 'pg_aggregate') AS tableoid, "
						  "oid, aggname, "
						  "0::oid as aggnamespace, "
				  "CASE WHEN aggbasetype = 0 THEN 0 ELSE 1 END as pronargs, "
						  "aggbasetype as proargtypes, "
						  "(%s aggowner) as rolname, "
						  "'{=X}' as aggacl "
						  "FROM pg_aggregate "
						  "where oid > '%u'::oid",
						  username_subquery,
						  g_last_builtin_oid);
	}

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numAggs = ntups;

	agginfo = (AggInfo *) malloc(ntups * sizeof(AggInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_aggname = PQfnumber(res, "aggname");
	i_aggnamespace = PQfnumber(res, "aggnamespace");
	i_pronargs = PQfnumber(res, "pronargs");
	i_proargtypes = PQfnumber(res, "proargtypes");
	i_rolname = PQfnumber(res, "rolname");
	i_aggacl = PQfnumber(res, "aggacl");

	for (i = 0; i < ntups; i++)
	{
		agginfo[i].aggfn.dobj.objType = DO_AGG;
		agginfo[i].aggfn.dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		agginfo[i].aggfn.dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&agginfo[i].aggfn.dobj);
		agginfo[i].aggfn.dobj.name = strdup(PQgetvalue(res, i, i_aggname));
		agginfo[i].aggfn.dobj.namespace = findNamespace(atooid(PQgetvalue(res, i, i_aggnamespace)),
											agginfo[i].aggfn.dobj.catId.oid);
		agginfo[i].aggfn.rolname = strdup(PQgetvalue(res, i, i_rolname));
		if (strlen(agginfo[i].aggfn.rolname) == 0)
			write_msg(NULL, "WARNING: owner of aggregate function \"%s\" appears to be invalid\n",
					  agginfo[i].aggfn.dobj.name);
		agginfo[i].aggfn.lang = InvalidOid;		/* not currently interesting */
		agginfo[i].aggfn.prorettype = InvalidOid;		/* not saved */
		agginfo[i].aggfn.proacl = strdup(PQgetvalue(res, i, i_aggacl));
		agginfo[i].aggfn.nargs = atoi(PQgetvalue(res, i, i_pronargs));
		if (agginfo[i].aggfn.nargs == 0)
			agginfo[i].aggfn.argtypes = NULL;
		else
		{
			agginfo[i].aggfn.argtypes = (Oid *) malloc(agginfo[i].aggfn.nargs * sizeof(Oid));
			if (g_fout->remoteVersion >= 70300)
				parseOidArray(PQgetvalue(res, i, i_proargtypes),
							  agginfo[i].aggfn.argtypes,
							  agginfo[i].aggfn.nargs);
			else
				/* it's just aggbasetype */
				agginfo[i].aggfn.argtypes[0] = atooid(PQgetvalue(res, i, i_proargtypes));
		}

		/* Decide whether we want to dump it */
		selectDumpableObject(&(agginfo[i].aggfn.dobj));
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
	int			i_tableoid;
	int			i_oid;
	int			i_proname;
	int			i_pronamespace;
	int			i_rolname;
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
						  "SELECT tableoid, oid, proname, prolang, "
						  "pronargs, proargtypes, prorettype, proacl, "
						  "pronamespace, "
						  "(%s proowner) as rolname "
						  "FROM pg_proc "
						  "WHERE NOT proisagg "
						  "AND pronamespace != "
						  "(select oid from pg_namespace"
						  " where nspname = 'pg_catalog')",
						  username_subquery);
	}
	else if (g_fout->remoteVersion >= 70100)
	{
		appendPQExpBuffer(query,
						  "SELECT tableoid, oid, proname, prolang, "
						  "pronargs, proargtypes, prorettype, "
						  "'{=X}' as proacl, "
						  "0::oid as pronamespace, "
						  "(%s proowner) as rolname "
						  "FROM pg_proc "
						  "where pg_proc.oid > '%u'::oid",
						  username_subquery,
						  g_last_builtin_oid);
	}
	else
	{
		appendPQExpBuffer(query,
						  "SELECT "
						  "(SELECT oid FROM pg_class "
						  " WHERE relname = 'pg_proc') AS tableoid, "
						  "oid, proname, prolang, "
						  "pronargs, proargtypes, prorettype, "
						  "'{=X}' as proacl, "
						  "0::oid as pronamespace, "
						  "(%s proowner) as rolname "
						  "FROM pg_proc "
						  "where pg_proc.oid > '%u'::oid",
						  username_subquery,
						  g_last_builtin_oid);
	}

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	*numFuncs = ntups;

	finfo = (FuncInfo *) calloc(ntups, sizeof(FuncInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_proname = PQfnumber(res, "proname");
	i_pronamespace = PQfnumber(res, "pronamespace");
	i_rolname = PQfnumber(res, "rolname");
	i_prolang = PQfnumber(res, "prolang");
	i_pronargs = PQfnumber(res, "pronargs");
	i_proargtypes = PQfnumber(res, "proargtypes");
	i_prorettype = PQfnumber(res, "prorettype");
	i_proacl = PQfnumber(res, "proacl");

	for (i = 0; i < ntups; i++)
	{
		finfo[i].dobj.objType = DO_FUNC;
		finfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		finfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&finfo[i].dobj);
		finfo[i].dobj.name = strdup(PQgetvalue(res, i, i_proname));
		finfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_pronamespace)),
						  finfo[i].dobj.catId.oid);
		finfo[i].rolname = strdup(PQgetvalue(res, i, i_rolname));
		finfo[i].lang = atooid(PQgetvalue(res, i, i_prolang));
		finfo[i].prorettype = atooid(PQgetvalue(res, i, i_prorettype));
		finfo[i].proacl = strdup(PQgetvalue(res, i, i_proacl));
		finfo[i].nargs = atoi(PQgetvalue(res, i, i_pronargs));
		if (finfo[i].nargs == 0)
			finfo[i].argtypes = NULL;
		else
		{
			finfo[i].argtypes = (Oid *) malloc(finfo[i].nargs * sizeof(Oid));
			parseOidArray(PQgetvalue(res, i, i_proargtypes),
						  finfo[i].argtypes, finfo[i].nargs);
		}

		/* Decide whether we want to dump it */
		selectDumpableObject(&(finfo[i].dobj));

		if (strlen(finfo[i].rolname) == 0)
			write_msg(NULL,
				 "WARNING: owner of function \"%s\" appears to be invalid\n",
					  finfo[i].dobj.name);
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
	int			i_reltableoid;
	int			i_reloid;
	int			i_relname;
	int			i_relnamespace;
	int			i_relkind;
	int			i_relacl;
	int			i_rolname;
	int			i_relchecks;
	int			i_reltriggers;
	int			i_relhasindex;
	int			i_relhasrules;
	int			i_relhasoids;
	int			i_owning_tab;
	int			i_owning_col;
	int			i_reltablespace;
	int			i_reloptions;

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	/*
	 * Find all the tables (including views and sequences).
	 *
	 * We include system catalogs, so that we can work if a user table is
	 * defined to inherit from a system catalog (pretty weird, but...)
	 *
	 * We ignore tables that are not type 'r' (ordinary relation), 'S'
	 * (sequence), 'v' (view), or 'c' (composite type).
	 *
	 * Composite-type table entries won't be dumped as such, but we have to
	 * make a DumpableObject for them so that we can track dependencies of the
	 * composite type (pg_depend entries for columns of the composite type
	 * link to the pg_class entry not the pg_type entry).
	 *
	 * Note: in this phase we should collect only a minimal amount of
	 * information about each table, basically just enough to decide if it is
	 * interesting. We must fetch all tables in this phase because otherwise
	 * we cannot correctly identify inherited columns, owned sequences, etc.
	 */

	if (g_fout->remoteVersion >= 80200)
	{
		/*
		 * Left join to pick up dependency info linking sequences to their
		 * owning column, if any (note this dependency is AUTO as of 8.2)
		 */
		appendPQExpBuffer(query,
						  "SELECT c.tableoid, c.oid, relname, "
						  "relacl, relkind, relnamespace, "
						  "(%s relowner) as rolname, "
						  "relchecks, reltriggers, "
						  "relhasindex, relhasrules, relhasoids, "
						  "d.refobjid as owning_tab, "
						  "d.refobjsubid as owning_col, "
						  "(SELECT spcname FROM pg_tablespace t WHERE t.oid = c.reltablespace) AS reltablespace, "
						  "array_to_string(c.reloptions, ', ') as reloptions "
						  "from pg_class c "
						  "left join pg_depend d on "
						  "(c.relkind = '%c' and "
						  "d.classid = c.tableoid and d.objid = c.oid and "
						  "d.objsubid = 0 and "
						  "d.refclassid = c.tableoid and d.deptype = 'a') "
						  "where relkind in ('%c', '%c', '%c', '%c') "
						  "order by c.oid",
						  username_subquery,
						  RELKIND_SEQUENCE,
						  RELKIND_RELATION, RELKIND_SEQUENCE,
						  RELKIND_VIEW, RELKIND_COMPOSITE_TYPE);
	}
	else if (g_fout->remoteVersion >= 80000)
	{
		/*
		 * Left join to pick up dependency info linking sequences to their
		 * owning column, if any
		 */
		appendPQExpBuffer(query,
						  "SELECT c.tableoid, c.oid, relname, "
						  "relacl, relkind, relnamespace, "
						  "(%s relowner) as rolname, "
						  "relchecks, reltriggers, "
						  "relhasindex, relhasrules, relhasoids, "
						  "d.refobjid as owning_tab, "
						  "d.refobjsubid as owning_col, "
						  "(SELECT spcname FROM pg_tablespace t WHERE t.oid = c.reltablespace) AS reltablespace, "
						  "NULL as reloptions "
						  "from pg_class c "
						  "left join pg_depend d on "
						  "(c.relkind = '%c' and "
						  "d.classid = c.tableoid and d.objid = c.oid and "
						  "d.objsubid = 0 and "
						  "d.refclassid = c.tableoid and d.deptype = 'i') "
						  "where relkind in ('%c', '%c', '%c', '%c') "
						  "order by c.oid",
						  username_subquery,
						  RELKIND_SEQUENCE,
						  RELKIND_RELATION, RELKIND_SEQUENCE,
						  RELKIND_VIEW, RELKIND_COMPOSITE_TYPE);
	}
	else if (g_fout->remoteVersion >= 70300)
	{
		/*
		 * Left join to pick up dependency info linking sequences to their
		 * owning column, if any
		 */
		appendPQExpBuffer(query,
						  "SELECT c.tableoid, c.oid, relname, "
						  "relacl, relkind, relnamespace, "
						  "(%s relowner) as rolname, "
						  "relchecks, reltriggers, "
						  "relhasindex, relhasrules, relhasoids, "
						  "d.refobjid as owning_tab, "
						  "d.refobjsubid as owning_col, "
						  "NULL as reltablespace, "
						  "NULL as reloptions "
						  "from pg_class c "
						  "left join pg_depend d on "
						  "(c.relkind = '%c' and "
						  "d.classid = c.tableoid and d.objid = c.oid and "
						  "d.objsubid = 0 and "
						  "d.refclassid = c.tableoid and d.deptype = 'i') "
						  "where relkind in ('%c', '%c', '%c', '%c') "
						  "order by c.oid",
						  username_subquery,
						  RELKIND_SEQUENCE,
						  RELKIND_RELATION, RELKIND_SEQUENCE,
						  RELKIND_VIEW, RELKIND_COMPOSITE_TYPE);
	}
	else if (g_fout->remoteVersion >= 70200)
	{
		appendPQExpBuffer(query,
						  "SELECT tableoid, oid, relname, relacl, relkind, "
						  "0::oid as relnamespace, "
						  "(%s relowner) as rolname, "
						  "relchecks, reltriggers, "
						  "relhasindex, relhasrules, relhasoids, "
						  "NULL::oid as owning_tab, "
						  "NULL::int4 as owning_col, "
						  "NULL as reltablespace, "
						  "NULL as reloptions "
						  "from pg_class "
						  "where relkind in ('%c', '%c', '%c') "
						  "order by oid",
						  username_subquery,
						  RELKIND_RELATION, RELKIND_SEQUENCE, RELKIND_VIEW);
	}
	else if (g_fout->remoteVersion >= 70100)
	{
		/* all tables have oids in 7.1 */
		appendPQExpBuffer(query,
						  "SELECT tableoid, oid, relname, relacl, relkind, "
						  "0::oid as relnamespace, "
						  "(%s relowner) as rolname, "
						  "relchecks, reltriggers, "
						  "relhasindex, relhasrules, "
						  "'t'::bool as relhasoids, "
						  "NULL::oid as owning_tab, "
						  "NULL::int4 as owning_col, "
						  "NULL as reltablespace, "
						  "NULL as reloptions "
						  "from pg_class "
						  "where relkind in ('%c', '%c', '%c') "
						  "order by oid",
						  username_subquery,
						  RELKIND_RELATION, RELKIND_SEQUENCE, RELKIND_VIEW);
	}
	else
	{
		/*
		 * Before 7.1, view relkind was not set to 'v', so we must check if we
		 * have a view by looking for a rule in pg_rewrite.
		 */
		appendPQExpBuffer(query,
						  "SELECT "
		"(SELECT oid FROM pg_class WHERE relname = 'pg_class') AS tableoid, "
						  "oid, relname, relacl, "
						  "CASE WHEN relhasrules and relkind = 'r' "
					  "  and EXISTS(SELECT rulename FROM pg_rewrite r WHERE "
					  "             r.ev_class = c.oid AND r.ev_type = '1') "
						  "THEN '%c'::\"char\" "
						  "ELSE relkind END AS relkind,"
						  "0::oid as relnamespace, "
						  "(%s relowner) as rolname, "
						  "relchecks, reltriggers, "
						  "relhasindex, relhasrules, "
						  "'t'::bool as relhasoids, "
						  "NULL::oid as owning_tab, "
						  "NULL::int4 as owning_col, "
						  "NULL as reltablespace, "
						  "NULL as reloptions "
						  "from pg_class c "
						  "where relkind in ('%c', '%c') "
						  "order by oid",
						  RELKIND_VIEW,
						  username_subquery,
						  RELKIND_RELATION, RELKIND_SEQUENCE);
	}

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	*numTables = ntups;

	/*
	 * Extract data from result and lock dumpable tables.  We do the locking
	 * before anything else, to minimize the window wherein a table could
	 * disappear under us.
	 *
	 * Note that we have to save info about all tables here, even when dumping
	 * only one, because we don't yet know which tables might be inheritance
	 * ancestors of the target table.
	 */
	tblinfo = (TableInfo *) calloc(ntups, sizeof(TableInfo));

	i_reltableoid = PQfnumber(res, "tableoid");
	i_reloid = PQfnumber(res, "oid");
	i_relname = PQfnumber(res, "relname");
	i_relnamespace = PQfnumber(res, "relnamespace");
	i_relacl = PQfnumber(res, "relacl");
	i_relkind = PQfnumber(res, "relkind");
	i_rolname = PQfnumber(res, "rolname");
	i_relchecks = PQfnumber(res, "relchecks");
	i_reltriggers = PQfnumber(res, "reltriggers");
	i_relhasindex = PQfnumber(res, "relhasindex");
	i_relhasrules = PQfnumber(res, "relhasrules");
	i_relhasoids = PQfnumber(res, "relhasoids");
	i_owning_tab = PQfnumber(res, "owning_tab");
	i_owning_col = PQfnumber(res, "owning_col");
	i_reltablespace = PQfnumber(res, "reltablespace");
	i_reloptions = PQfnumber(res, "reloptions");

	for (i = 0; i < ntups; i++)
	{
		tblinfo[i].dobj.objType = DO_TABLE;
		tblinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_reltableoid));
		tblinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_reloid));
		AssignDumpId(&tblinfo[i].dobj);
		tblinfo[i].dobj.name = strdup(PQgetvalue(res, i, i_relname));
		tblinfo[i].dobj.namespace = findNamespace(atooid(PQgetvalue(res, i, i_relnamespace)),
												  tblinfo[i].dobj.catId.oid);
		tblinfo[i].rolname = strdup(PQgetvalue(res, i, i_rolname));
		tblinfo[i].relacl = strdup(PQgetvalue(res, i, i_relacl));
		tblinfo[i].relkind = *(PQgetvalue(res, i, i_relkind));
		tblinfo[i].hasindex = (strcmp(PQgetvalue(res, i, i_relhasindex), "t") == 0);
		tblinfo[i].hasrules = (strcmp(PQgetvalue(res, i, i_relhasrules), "t") == 0);
		tblinfo[i].hasoids = (strcmp(PQgetvalue(res, i, i_relhasoids), "t") == 0);
		tblinfo[i].ncheck = atoi(PQgetvalue(res, i, i_relchecks));
		tblinfo[i].ntrig = atoi(PQgetvalue(res, i, i_reltriggers));
		if (PQgetisnull(res, i, i_owning_tab))
		{
			tblinfo[i].owning_tab = InvalidOid;
			tblinfo[i].owning_col = 0;
		}
		else
		{
			tblinfo[i].owning_tab = atooid(PQgetvalue(res, i, i_owning_tab));
			tblinfo[i].owning_col = atoi(PQgetvalue(res, i, i_owning_col));
		}
		tblinfo[i].reltablespace = strdup(PQgetvalue(res, i, i_reltablespace));
		tblinfo[i].reloptions = strdup(PQgetvalue(res, i, i_reloptions));

		/* other fields were zeroed above */

		/*
		 * Decide whether we want to dump this table.
		 */
		if (tblinfo[i].relkind == RELKIND_COMPOSITE_TYPE)
			tblinfo[i].dobj.dump = false;
		else
			selectDumpableTable(&tblinfo[i]);
		tblinfo[i].interesting = tblinfo[i].dobj.dump;

		/*
		 * Read-lock target tables to make sure they aren't DROPPED or altered
		 * in schema before we get around to dumping them.
		 *
		 * Note that we don't explicitly lock parents of the target tables; we
		 * assume our lock on the child is enough to prevent schema
		 * alterations to parent tables.
		 *
		 * NOTE: it'd be kinda nice to lock views and sequences too, not only
		 * plain tables, but the backend doesn't presently allow that.
		 */
		if (tblinfo[i].dobj.dump && tblinfo[i].relkind == RELKIND_RELATION)
		{
			resetPQExpBuffer(lockquery);
			appendPQExpBuffer(lockquery,
							  "LOCK TABLE %s IN ACCESS SHARE MODE",
						 fmtQualifiedId(tblinfo[i].dobj.namespace->dobj.name,
										tblinfo[i].dobj.name));
			do_sql_command(g_conn, lockquery->data);
		}

		/* Emit notice if join for owner failed */
		if (strlen(tblinfo[i].rolname) == 0)
			write_msg(NULL, "WARNING: owner of table \"%s\" appears to be invalid\n",
					  tblinfo[i].dobj.name);
	}

	PQclear(res);

	/*
	 * Force sequences that are "owned" by table columns to be dumped whenever
	 * their owning table is being dumped.
	 */
	for (i = 0; i < ntups; i++)
	{
		TableInfo  *seqinfo = &tblinfo[i];
		int			j;

		if (!OidIsValid(seqinfo->owning_tab))
			continue;			/* not an owned sequence */
		if (seqinfo->dobj.dump)
			continue;			/* no need to search */

		/* can't use findTableByOid yet, unfortunately */
		for (j = 0; j < ntups; j++)
		{
			if (tblinfo[j].dobj.catId.oid == seqinfo->owning_tab)
			{
				if (tblinfo[j].dobj.dump)
				{
					seqinfo->interesting = true;
					seqinfo->dobj.dump = true;
				}
				break;
			}
		}
	}

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
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	*numInherits = ntups;

	inhinfo = (InhInfo *) malloc(ntups * sizeof(InhInfo));

	i_inhrelid = PQfnumber(res, "inhrelid");
	i_inhparent = PQfnumber(res, "inhparent");

	for (i = 0; i < ntups; i++)
	{
		inhinfo[i].inhrelid = atooid(PQgetvalue(res, i, i_inhrelid));
		inhinfo[i].inhparent = atooid(PQgetvalue(res, i, i_inhparent));
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return inhinfo;
}

/*
 * getIndexes
 *	  get information about every index on a dumpable table
 *
 * Note: index data is not returned directly to the caller, but it
 * does get entered into the DumpableObject tables.
 */
void
getIndexes(TableInfo tblinfo[], int numTables)
{
	int			i,
				j;
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;
	IndxInfo   *indxinfo;
	ConstraintInfo *constrinfo;
	int			i_tableoid,
				i_oid,
				i_indexname,
				i_indexdef,
				i_indnkeys,
				i_indkey,
				i_indisclustered,
				i_contype,
				i_conname,
				i_contableoid,
				i_conoid,
				i_tablespace,
				i_options;
	int			ntups;

	for (i = 0; i < numTables; i++)
	{
		TableInfo  *tbinfo = &tblinfo[i];

		/* Only plain tables have indexes */
		if (tbinfo->relkind != RELKIND_RELATION || !tbinfo->hasindex)
			continue;

		/* Ignore indexes of tables not to be dumped */
		if (!tbinfo->dobj.dump)
			continue;

		if (g_verbose)
			write_msg(NULL, "reading indexes for table \"%s\"\n",
					  tbinfo->dobj.name);

		/* Make sure we are in proper schema so indexdef is right */
		selectSourceSchema(tbinfo->dobj.namespace->dobj.name);

		/*
		 * The point of the messy-looking outer join is to find a constraint
		 * that is related by an internal dependency link to the index. If we
		 * find one, create a CONSTRAINT entry linked to the INDEX entry.  We
		 * assume an index won't have more than one internal dependency.
		 */
		resetPQExpBuffer(query);
		if (g_fout->remoteVersion >= 80200)
		{
			appendPQExpBuffer(query,
							  "SELECT t.tableoid, t.oid, "
							  "t.relname as indexname, "
					 "pg_catalog.pg_get_indexdef(i.indexrelid) as indexdef, "
							  "t.relnatts as indnkeys, "
							  "i.indkey, i.indisclustered, "
							  "c.contype, c.conname, "
							  "c.tableoid as contableoid, "
							  "c.oid as conoid, "
							  "(SELECT spcname FROM pg_catalog.pg_tablespace s WHERE s.oid = t.reltablespace) as tablespace, "
							"array_to_string(t.reloptions, ', ') as options "
							  "FROM pg_catalog.pg_index i "
					  "JOIN pg_catalog.pg_class t ON (t.oid = i.indexrelid) "
							  "LEFT JOIN pg_catalog.pg_depend d "
							  "ON (d.classid = t.tableoid "
							  "AND d.objid = t.oid "
							  "AND d.deptype = 'i') "
							  "LEFT JOIN pg_catalog.pg_constraint c "
							  "ON (d.refclassid = c.tableoid "
							  "AND d.refobjid = c.oid) "
							  "WHERE i.indrelid = '%u'::pg_catalog.oid "
							  "ORDER BY indexname",
							  tbinfo->dobj.catId.oid);
		}
		else if (g_fout->remoteVersion >= 80000)
		{
			appendPQExpBuffer(query,
							  "SELECT t.tableoid, t.oid, "
							  "t.relname as indexname, "
					 "pg_catalog.pg_get_indexdef(i.indexrelid) as indexdef, "
							  "t.relnatts as indnkeys, "
							  "i.indkey, i.indisclustered, "
							  "c.contype, c.conname, "
							  "c.tableoid as contableoid, "
							  "c.oid as conoid, "
							  "(SELECT spcname FROM pg_catalog.pg_tablespace s WHERE s.oid = t.reltablespace) as tablespace, "
							  "null as options "
							  "FROM pg_catalog.pg_index i "
					  "JOIN pg_catalog.pg_class t ON (t.oid = i.indexrelid) "
							  "LEFT JOIN pg_catalog.pg_depend d "
							  "ON (d.classid = t.tableoid "
							  "AND d.objid = t.oid "
							  "AND d.deptype = 'i') "
							  "LEFT JOIN pg_catalog.pg_constraint c "
							  "ON (d.refclassid = c.tableoid "
							  "AND d.refobjid = c.oid) "
							  "WHERE i.indrelid = '%u'::pg_catalog.oid "
							  "ORDER BY indexname",
							  tbinfo->dobj.catId.oid);
		}
		else if (g_fout->remoteVersion >= 70300)
		{
			appendPQExpBuffer(query,
							  "SELECT t.tableoid, t.oid, "
							  "t.relname as indexname, "
					 "pg_catalog.pg_get_indexdef(i.indexrelid) as indexdef, "
							  "t.relnatts as indnkeys, "
							  "i.indkey, i.indisclustered, "
							  "c.contype, c.conname, "
							  "c.tableoid as contableoid, "
							  "c.oid as conoid, "
							  "NULL as tablespace, "
							  "null as options "
							  "FROM pg_catalog.pg_index i "
					  "JOIN pg_catalog.pg_class t ON (t.oid = i.indexrelid) "
							  "LEFT JOIN pg_catalog.pg_depend d "
							  "ON (d.classid = t.tableoid "
							  "AND d.objid = t.oid "
							  "AND d.deptype = 'i') "
							  "LEFT JOIN pg_catalog.pg_constraint c "
							  "ON (d.refclassid = c.tableoid "
							  "AND d.refobjid = c.oid) "
							  "WHERE i.indrelid = '%u'::pg_catalog.oid "
							  "ORDER BY indexname",
							  tbinfo->dobj.catId.oid);
		}
		else if (g_fout->remoteVersion >= 70100)
		{
			appendPQExpBuffer(query,
							  "SELECT t.tableoid, t.oid, "
							  "t.relname as indexname, "
							  "pg_get_indexdef(i.indexrelid) as indexdef, "
							  "t.relnatts as indnkeys, "
							  "i.indkey, false as indisclustered, "
							  "CASE WHEN i.indisprimary THEN 'p'::char "
							  "ELSE '0'::char END as contype, "
							  "t.relname as conname, "
							  "0::oid as contableoid, "
							  "t.oid as conoid, "
							  "NULL as tablespace, "
							  "null as options "
							  "FROM pg_index i, pg_class t "
							  "WHERE t.oid = i.indexrelid "
							  "AND i.indrelid = '%u'::oid "
							  "ORDER BY indexname",
							  tbinfo->dobj.catId.oid);
		}
		else
		{
			appendPQExpBuffer(query,
							  "SELECT "
							  "(SELECT oid FROM pg_class WHERE relname = 'pg_class') AS tableoid, "
							  "t.oid, "
							  "t.relname as indexname, "
							  "pg_get_indexdef(i.indexrelid) as indexdef, "
							  "t.relnatts as indnkeys, "
							  "i.indkey, false as indisclustered, "
							  "CASE WHEN i.indisprimary THEN 'p'::char "
							  "ELSE '0'::char END as contype, "
							  "t.relname as conname, "
							  "0::oid as contableoid, "
							  "t.oid as conoid, "
							  "NULL as tablespace, "
							  "null as options "
							  "FROM pg_index i, pg_class t "
							  "WHERE t.oid = i.indexrelid "
							  "AND i.indrelid = '%u'::oid "
							  "ORDER BY indexname",
							  tbinfo->dobj.catId.oid);
		}

		res = PQexec(g_conn, query->data);
		check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

		ntups = PQntuples(res);

		i_tableoid = PQfnumber(res, "tableoid");
		i_oid = PQfnumber(res, "oid");
		i_indexname = PQfnumber(res, "indexname");
		i_indexdef = PQfnumber(res, "indexdef");
		i_indnkeys = PQfnumber(res, "indnkeys");
		i_indkey = PQfnumber(res, "indkey");
		i_indisclustered = PQfnumber(res, "indisclustered");
		i_contype = PQfnumber(res, "contype");
		i_conname = PQfnumber(res, "conname");
		i_contableoid = PQfnumber(res, "contableoid");
		i_conoid = PQfnumber(res, "conoid");
		i_tablespace = PQfnumber(res, "tablespace");
		i_options = PQfnumber(res, "options");

		indxinfo = (IndxInfo *) malloc(ntups * sizeof(IndxInfo));
		constrinfo = (ConstraintInfo *) malloc(ntups * sizeof(ConstraintInfo));

		for (j = 0; j < ntups; j++)
		{
			char		contype;

			indxinfo[j].dobj.objType = DO_INDEX;
			indxinfo[j].dobj.catId.tableoid = atooid(PQgetvalue(res, j, i_tableoid));
			indxinfo[j].dobj.catId.oid = atooid(PQgetvalue(res, j, i_oid));
			AssignDumpId(&indxinfo[j].dobj);
			indxinfo[j].dobj.name = strdup(PQgetvalue(res, j, i_indexname));
			indxinfo[j].dobj.namespace = tbinfo->dobj.namespace;
			indxinfo[j].indextable = tbinfo;
			indxinfo[j].indexdef = strdup(PQgetvalue(res, j, i_indexdef));
			indxinfo[j].indnkeys = atoi(PQgetvalue(res, j, i_indnkeys));
			indxinfo[j].tablespace = strdup(PQgetvalue(res, j, i_tablespace));
			indxinfo[j].options = strdup(PQgetvalue(res, j, i_options));

			/*
			 * In pre-7.4 releases, indkeys may contain more entries than
			 * indnkeys says (since indnkeys will be 1 for a functional
			 * index).	We don't actually care about this case since we don't
			 * examine indkeys except for indexes associated with PRIMARY and
			 * UNIQUE constraints, which are never functional indexes. But we
			 * have to allocate enough space to keep parseOidArray from
			 * complaining.
			 */
			indxinfo[j].indkeys = (Oid *) malloc(INDEX_MAX_KEYS * sizeof(Oid));
			parseOidArray(PQgetvalue(res, j, i_indkey),
						  indxinfo[j].indkeys, INDEX_MAX_KEYS);
			indxinfo[j].indisclustered = (PQgetvalue(res, j, i_indisclustered)[0] == 't');
			contype = *(PQgetvalue(res, j, i_contype));

			if (contype == 'p' || contype == 'u')
			{
				/*
				 * If we found a constraint matching the index, create an
				 * entry for it.
				 *
				 * In a pre-7.3 database, we take this path iff the index was
				 * marked indisprimary.
				 */
				constrinfo[j].dobj.objType = DO_CONSTRAINT;
				constrinfo[j].dobj.catId.tableoid = atooid(PQgetvalue(res, j, i_contableoid));
				constrinfo[j].dobj.catId.oid = atooid(PQgetvalue(res, j, i_conoid));
				AssignDumpId(&constrinfo[j].dobj);
				constrinfo[j].dobj.name = strdup(PQgetvalue(res, j, i_conname));
				constrinfo[j].dobj.namespace = tbinfo->dobj.namespace;
				constrinfo[j].contable = tbinfo;
				constrinfo[j].condomain = NULL;
				constrinfo[j].contype = contype;
				constrinfo[j].condef = NULL;
				constrinfo[j].conindex = indxinfo[j].dobj.dumpId;
				constrinfo[j].coninherited = false;
				constrinfo[j].separate = true;

				indxinfo[j].indexconstraint = constrinfo[j].dobj.dumpId;

				/* If pre-7.3 DB, better make sure table comes first */
				addObjectDependency(&constrinfo[j].dobj,
									tbinfo->dobj.dumpId);
			}
			else
			{
				/* Plain secondary index */
				indxinfo[j].indexconstraint = 0;
			}
		}

		PQclear(res);
	}

	destroyPQExpBuffer(query);
}

/*
 * getConstraints
 *
 * Get info about constraints on dumpable tables.
 *
 * Currently handles foreign keys only.
 * Unique and primary key constraints are handled with indexes,
 * while check constraints are processed in getTableAttrs().
 */
void
getConstraints(TableInfo tblinfo[], int numTables)
{
	int			i,
				j;
	ConstraintInfo *constrinfo;
	PQExpBuffer query;
	PGresult   *res;
	int			i_condef,
				i_contableoid,
				i_conoid,
				i_conname;
	int			ntups;

	/* pg_constraint was created in 7.3, so nothing to do if older */
	if (g_fout->remoteVersion < 70300)
		return;

	query = createPQExpBuffer();

	for (i = 0; i < numTables; i++)
	{
		TableInfo  *tbinfo = &tblinfo[i];

		if (tbinfo->ntrig == 0 || !tbinfo->dobj.dump)
			continue;

		if (g_verbose)
			write_msg(NULL, "reading foreign key constraints for table \"%s\"\n",
					  tbinfo->dobj.name);

		/*
		 * select table schema to ensure constraint expr is qualified if
		 * needed
		 */
		selectSourceSchema(tbinfo->dobj.namespace->dobj.name);

		resetPQExpBuffer(query);
		appendPQExpBuffer(query,
						  "SELECT tableoid, oid, conname, "
						  "pg_catalog.pg_get_constraintdef(oid) as condef "
						  "FROM pg_catalog.pg_constraint "
						  "WHERE conrelid = '%u'::pg_catalog.oid "
						  "AND contype = 'f'",
						  tbinfo->dobj.catId.oid);
		res = PQexec(g_conn, query->data);
		check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

		ntups = PQntuples(res);

		i_contableoid = PQfnumber(res, "tableoid");
		i_conoid = PQfnumber(res, "oid");
		i_conname = PQfnumber(res, "conname");
		i_condef = PQfnumber(res, "condef");

		constrinfo = (ConstraintInfo *) malloc(ntups * sizeof(ConstraintInfo));

		for (j = 0; j < ntups; j++)
		{
			constrinfo[j].dobj.objType = DO_FK_CONSTRAINT;
			constrinfo[j].dobj.catId.tableoid = atooid(PQgetvalue(res, j, i_contableoid));
			constrinfo[j].dobj.catId.oid = atooid(PQgetvalue(res, j, i_conoid));
			AssignDumpId(&constrinfo[j].dobj);
			constrinfo[j].dobj.name = strdup(PQgetvalue(res, j, i_conname));
			constrinfo[j].dobj.namespace = tbinfo->dobj.namespace;
			constrinfo[j].contable = tbinfo;
			constrinfo[j].condomain = NULL;
			constrinfo[j].contype = 'f';
			constrinfo[j].condef = strdup(PQgetvalue(res, j, i_condef));
			constrinfo[j].conindex = 0;
			constrinfo[j].coninherited = false;
			constrinfo[j].separate = true;
		}

		PQclear(res);
	}

	destroyPQExpBuffer(query);
}

/*
 * getDomainConstraints
 *
 * Get info about constraints on a domain.
 */
static void
getDomainConstraints(TypeInfo *tinfo)
{
	int			i;
	ConstraintInfo *constrinfo;
	PQExpBuffer query;
	PGresult   *res;
	int			i_tableoid,
				i_oid,
				i_conname,
				i_consrc;
	int			ntups;

	/* pg_constraint was created in 7.3, so nothing to do if older */
	if (g_fout->remoteVersion < 70300)
		return;

	/*
	 * select appropriate schema to ensure names in constraint are properly
	 * qualified
	 */
	selectSourceSchema(tinfo->dobj.namespace->dobj.name);

	query = createPQExpBuffer();

	if (g_fout->remoteVersion >= 70400)
		appendPQExpBuffer(query, "SELECT tableoid, oid, conname, "
						  "pg_catalog.pg_get_constraintdef(oid) AS consrc "
						  "FROM pg_catalog.pg_constraint "
						  "WHERE contypid = '%u'::pg_catalog.oid "
						  "ORDER BY conname",
						  tinfo->dobj.catId.oid);
	else
		appendPQExpBuffer(query, "SELECT tableoid, oid, conname, "
						  "'CHECK (' || consrc || ')' AS consrc "
						  "FROM pg_catalog.pg_constraint "
						  "WHERE contypid = '%u'::pg_catalog.oid "
						  "ORDER BY conname",
						  tinfo->dobj.catId.oid);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_conname = PQfnumber(res, "conname");
	i_consrc = PQfnumber(res, "consrc");

	constrinfo = (ConstraintInfo *) malloc(ntups * sizeof(ConstraintInfo));

	tinfo->nDomChecks = ntups;
	tinfo->domChecks = constrinfo;

	for (i = 0; i < ntups; i++)
	{
		constrinfo[i].dobj.objType = DO_CONSTRAINT;
		constrinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		constrinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&constrinfo[i].dobj);
		constrinfo[i].dobj.name = strdup(PQgetvalue(res, i, i_conname));
		constrinfo[i].dobj.namespace = tinfo->dobj.namespace;
		constrinfo[i].contable = NULL;
		constrinfo[i].condomain = tinfo;
		constrinfo[i].contype = 'c';
		constrinfo[i].condef = strdup(PQgetvalue(res, i, i_consrc));
		constrinfo[i].conindex = 0;
		constrinfo[i].coninherited = false;
		constrinfo[i].separate = false;

		/*
		 * Make the domain depend on the constraint, ensuring it won't be
		 * output till any constraint dependencies are OK.
		 */
		addObjectDependency(&tinfo->dobj,
							constrinfo[i].dobj.dumpId);
	}

	PQclear(res);

	destroyPQExpBuffer(query);
}

/*
 * getRules
 *	  get basic information about every rule in the system
 *
 * numRules is set to the number of rules read in
 */
RuleInfo *
getRules(int *numRules)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	RuleInfo   *ruleinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_rulename;
	int			i_ruletable;
	int			i_ev_type;
	int			i_is_instead;

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	if (g_fout->remoteVersion >= 70100)
	{
		appendPQExpBuffer(query, "SELECT "
						  "tableoid, oid, rulename, "
						  "ev_class as ruletable, ev_type, is_instead "
						  "FROM pg_rewrite "
						  "ORDER BY oid");
	}
	else
	{
		appendPQExpBuffer(query, "SELECT "
						  "(SELECT oid FROM pg_class WHERE relname = 'pg_rewrite') AS tableoid, "
						  "oid, rulename, "
						  "ev_class as ruletable, ev_type, is_instead "
						  "FROM pg_rewrite "
						  "ORDER BY oid");
	}

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	*numRules = ntups;

	ruleinfo = (RuleInfo *) malloc(ntups * sizeof(RuleInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_rulename = PQfnumber(res, "rulename");
	i_ruletable = PQfnumber(res, "ruletable");
	i_ev_type = PQfnumber(res, "ev_type");
	i_is_instead = PQfnumber(res, "is_instead");

	for (i = 0; i < ntups; i++)
	{
		Oid			ruletableoid;

		ruleinfo[i].dobj.objType = DO_RULE;
		ruleinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		ruleinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&ruleinfo[i].dobj);
		ruleinfo[i].dobj.name = strdup(PQgetvalue(res, i, i_rulename));
		ruletableoid = atooid(PQgetvalue(res, i, i_ruletable));
		ruleinfo[i].ruletable = findTableByOid(ruletableoid);
		if (ruleinfo[i].ruletable == NULL)
		{
			write_msg(NULL, "failed sanity check, parent table OID %u of pg_rewrite entry OID %u not found\n",
					  ruletableoid,
					  ruleinfo[i].dobj.catId.oid);
			exit_nicely();
		}
		ruleinfo[i].dobj.namespace = ruleinfo[i].ruletable->dobj.namespace;
		ruleinfo[i].dobj.dump = ruleinfo[i].ruletable->dobj.dump;
		ruleinfo[i].ev_type = *(PQgetvalue(res, i, i_ev_type));
		ruleinfo[i].is_instead = *(PQgetvalue(res, i, i_is_instead)) == 't';
		if (ruleinfo[i].ruletable)
		{
			/*
			 * If the table is a view, force its ON SELECT rule to be sorted
			 * before the view itself --- this ensures that any dependencies
			 * for the rule affect the table's positioning. Other rules are
			 * forced to appear after their table.
			 */
			if (ruleinfo[i].ruletable->relkind == RELKIND_VIEW &&
				ruleinfo[i].ev_type == '1' && ruleinfo[i].is_instead)
			{
				addObjectDependency(&ruleinfo[i].ruletable->dobj,
									ruleinfo[i].dobj.dumpId);
				/* We'll merge the rule into CREATE VIEW, if possible */
				ruleinfo[i].separate = false;
			}
			else
			{
				addObjectDependency(&ruleinfo[i].dobj,
									ruleinfo[i].ruletable->dobj.dumpId);
				ruleinfo[i].separate = true;
			}
		}
		else
			ruleinfo[i].separate = true;
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return ruleinfo;
}

/*
 * getTriggers
 *	  get information about every trigger on a dumpable table
 *
 * Note: trigger data is not returned directly to the caller, but it
 * does get entered into the DumpableObject tables.
 */
void
getTriggers(TableInfo tblinfo[], int numTables)
{
	int			i,
				j;
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;
	TriggerInfo *tginfo;
	int			i_tableoid,
				i_oid,
				i_tgname,
				i_tgfname,
				i_tgtype,
				i_tgnargs,
				i_tgargs,
				i_tgisconstraint,
				i_tgconstrname,
				i_tgconstrrelid,
				i_tgconstrrelname,
				i_tgenabled,
				i_tgdeferrable,
				i_tginitdeferred;
	int			ntups;

	for (i = 0; i < numTables; i++)
	{
		TableInfo  *tbinfo = &tblinfo[i];

		if (tbinfo->ntrig == 0 || !tbinfo->dobj.dump)
			continue;

		if (g_verbose)
			write_msg(NULL, "reading triggers for table \"%s\"\n",
					  tbinfo->dobj.name);

		/*
		 * select table schema to ensure regproc name is qualified if needed
		 */
		selectSourceSchema(tbinfo->dobj.namespace->dobj.name);

		resetPQExpBuffer(query);
		if (g_fout->remoteVersion >= 70300)
		{
			/*
			 * We ignore triggers that are tied to a foreign-key constraint
			 */
			appendPQExpBuffer(query,
							  "SELECT tgname, "
							  "tgfoid::pg_catalog.regproc as tgfname, "
							  "tgtype, tgnargs, tgargs, tgenabled, "
							  "tgisconstraint, tgconstrname, tgdeferrable, "
							  "tgconstrrelid, tginitdeferred, tableoid, oid, "
					 "tgconstrrelid::pg_catalog.regclass as tgconstrrelname "
							  "from pg_catalog.pg_trigger t "
							  "where tgrelid = '%u'::pg_catalog.oid "
							  "and (not tgisconstraint "
							  " OR NOT EXISTS"
							  "  (SELECT 1 FROM pg_catalog.pg_depend d "
							  "   JOIN pg_catalog.pg_constraint c ON (d.refclassid = c.tableoid AND d.refobjid = c.oid) "
							  "   WHERE d.classid = t.tableoid AND d.objid = t.oid AND d.deptype = 'i' AND c.contype = 'f'))",
							  tbinfo->dobj.catId.oid);
		}
		else if (g_fout->remoteVersion >= 70100)
		{
			appendPQExpBuffer(query,
							  "SELECT tgname, tgfoid::regproc as tgfname, "
							  "tgtype, tgnargs, tgargs, tgenabled, "
							  "tgisconstraint, tgconstrname, tgdeferrable, "
							  "tgconstrrelid, tginitdeferred, tableoid, oid, "
				  "(select relname from pg_class where oid = tgconstrrelid) "
							  "		as tgconstrrelname "
							  "from pg_trigger "
							  "where tgrelid = '%u'::oid",
							  tbinfo->dobj.catId.oid);
		}
		else
		{
			appendPQExpBuffer(query,
							  "SELECT tgname, tgfoid::regproc as tgfname, "
							  "tgtype, tgnargs, tgargs, tgenabled, "
							  "tgisconstraint, tgconstrname, tgdeferrable, "
							  "tgconstrrelid, tginitdeferred, "
							  "(SELECT oid FROM pg_class WHERE relname = 'pg_trigger') AS tableoid, "

							  "oid, "
				  "(select relname from pg_class where oid = tgconstrrelid) "
							  "		as tgconstrrelname "
							  "from pg_trigger "
							  "where tgrelid = '%u'::oid",
							  tbinfo->dobj.catId.oid);
		}
		res = PQexec(g_conn, query->data);
		check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

		ntups = PQntuples(res);

		/*
		 * We may have less triggers than recorded due to having ignored
		 * foreign-key triggers
		 */
		if (ntups > tbinfo->ntrig)
		{
			write_msg(NULL, "expected %d triggers on table \"%s\" but found %d\n",
					  tbinfo->ntrig, tbinfo->dobj.name, ntups);
			exit_nicely();
		}
		i_tableoid = PQfnumber(res, "tableoid");
		i_oid = PQfnumber(res, "oid");
		i_tgname = PQfnumber(res, "tgname");
		i_tgfname = PQfnumber(res, "tgfname");
		i_tgtype = PQfnumber(res, "tgtype");
		i_tgnargs = PQfnumber(res, "tgnargs");
		i_tgargs = PQfnumber(res, "tgargs");
		i_tgisconstraint = PQfnumber(res, "tgisconstraint");
		i_tgconstrname = PQfnumber(res, "tgconstrname");
		i_tgconstrrelid = PQfnumber(res, "tgconstrrelid");
		i_tgconstrrelname = PQfnumber(res, "tgconstrrelname");
		i_tgenabled = PQfnumber(res, "tgenabled");
		i_tgdeferrable = PQfnumber(res, "tgdeferrable");
		i_tginitdeferred = PQfnumber(res, "tginitdeferred");

		tginfo = (TriggerInfo *) malloc(ntups * sizeof(TriggerInfo));

		for (j = 0; j < ntups; j++)
		{
			tginfo[j].dobj.objType = DO_TRIGGER;
			tginfo[j].dobj.catId.tableoid = atooid(PQgetvalue(res, j, i_tableoid));
			tginfo[j].dobj.catId.oid = atooid(PQgetvalue(res, j, i_oid));
			AssignDumpId(&tginfo[j].dobj);
			tginfo[j].dobj.name = strdup(PQgetvalue(res, j, i_tgname));
			tginfo[j].dobj.namespace = tbinfo->dobj.namespace;
			tginfo[j].tgtable = tbinfo;
			tginfo[j].tgfname = strdup(PQgetvalue(res, j, i_tgfname));
			tginfo[j].tgtype = atoi(PQgetvalue(res, j, i_tgtype));
			tginfo[j].tgnargs = atoi(PQgetvalue(res, j, i_tgnargs));
			tginfo[j].tgargs = strdup(PQgetvalue(res, j, i_tgargs));
			tginfo[j].tgisconstraint = *(PQgetvalue(res, j, i_tgisconstraint)) == 't';
			tginfo[j].tgenabled = *(PQgetvalue(res, j, i_tgenabled)) == 't';
			tginfo[j].tgdeferrable = *(PQgetvalue(res, j, i_tgdeferrable)) == 't';
			tginfo[j].tginitdeferred = *(PQgetvalue(res, j, i_tginitdeferred)) == 't';

			if (tginfo[j].tgisconstraint)
			{
				tginfo[j].tgconstrname = strdup(PQgetvalue(res, j, i_tgconstrname));
				tginfo[j].tgconstrrelid = atooid(PQgetvalue(res, j, i_tgconstrrelid));
				if (OidIsValid(tginfo[j].tgconstrrelid))
				{
					if (PQgetisnull(res, j, i_tgconstrrelname))
					{
						write_msg(NULL, "query produced null referenced table name for foreign key trigger \"%s\" on table \"%s\" (OID of table: %u)\n",
								  tginfo[j].dobj.name, tbinfo->dobj.name,
								  tginfo[j].tgconstrrelid);
						exit_nicely();
					}
					tginfo[j].tgconstrrelname = strdup(PQgetvalue(res, j, i_tgconstrrelname));
				}
				else
					tginfo[j].tgconstrrelname = NULL;
			}
			else
			{
				tginfo[j].tgconstrname = NULL;
				tginfo[j].tgconstrrelid = InvalidOid;
				tginfo[j].tgconstrrelname = NULL;
			}
		}

		PQclear(res);
	}

	destroyPQExpBuffer(query);
}

/*
 * getProcLangs
 *	  get basic information about every procedural language in the system
 *
 * numProcLangs is set to the number of langs read in
 *
 * NB: this must run after getFuncs() because we assume we can do
 * findFuncByOid().
 */
ProcLangInfo *
getProcLangs(int *numProcLangs)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	ProcLangInfo *planginfo;
	int			i_tableoid;
	int			i_oid;
	int			i_lanname;
	int			i_lanpltrusted;
	int			i_lanplcallfoid;
	int			i_lanvalidator;
	int			i_lanacl;
	int			i_lanowner;

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	if (g_fout->remoteVersion >= 80100)
	{
		/* Languages are owned by the bootstrap superuser, OID 10 */
		appendPQExpBuffer(query, "SELECT tableoid, oid, *, "
						  "(%s '10') as lanowner "
						  "FROM pg_language "
						  "WHERE lanispl "
						  "ORDER BY oid",
						  username_subquery);
	}
	else if (g_fout->remoteVersion >= 70400)
	{
		/* Languages are owned by the bootstrap superuser, sysid 1 */
		appendPQExpBuffer(query, "SELECT tableoid, oid, *, "
						  "(%s '1') as lanowner "
						  "FROM pg_language "
						  "WHERE lanispl "
						  "ORDER BY oid",
						  username_subquery);
	}
	else if (g_fout->remoteVersion >= 70100)
	{
		/* No clear notion of an owner at all before 7.4 ... */
		appendPQExpBuffer(query, "SELECT tableoid, oid, * FROM pg_language "
						  "WHERE lanispl "
						  "ORDER BY oid");
	}
	else
	{
		appendPQExpBuffer(query, "SELECT "
						  "(SELECT oid FROM pg_class WHERE relname = 'pg_language') AS tableoid, "
						  "oid, * FROM pg_language "
						  "WHERE lanispl "
						  "ORDER BY oid");
	}

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	*numProcLangs = ntups;

	planginfo = (ProcLangInfo *) malloc(ntups * sizeof(ProcLangInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_lanname = PQfnumber(res, "lanname");
	i_lanpltrusted = PQfnumber(res, "lanpltrusted");
	i_lanplcallfoid = PQfnumber(res, "lanplcallfoid");
	/* these may fail and return -1: */
	i_lanvalidator = PQfnumber(res, "lanvalidator");
	i_lanacl = PQfnumber(res, "lanacl");
	i_lanowner = PQfnumber(res, "lanowner");

	for (i = 0; i < ntups; i++)
	{
		planginfo[i].dobj.objType = DO_PROCLANG;
		planginfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		planginfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&planginfo[i].dobj);

		planginfo[i].dobj.name = strdup(PQgetvalue(res, i, i_lanname));
		planginfo[i].lanpltrusted = *(PQgetvalue(res, i, i_lanpltrusted)) == 't';
		planginfo[i].lanplcallfoid = atooid(PQgetvalue(res, i, i_lanplcallfoid));
		if (i_lanvalidator >= 0)
			planginfo[i].lanvalidator = atooid(PQgetvalue(res, i, i_lanvalidator));
		else
			planginfo[i].lanvalidator = InvalidOid;
		if (i_lanacl >= 0)
			planginfo[i].lanacl = strdup(PQgetvalue(res, i, i_lanacl));
		else
			planginfo[i].lanacl = strdup("{=U}");
		if (i_lanowner >= 0)
			planginfo[i].lanowner = strdup(PQgetvalue(res, i, i_lanowner));
		else
			planginfo[i].lanowner = strdup("");

		if (g_fout->remoteVersion < 70300)
		{
			/*
			 * We need to make a dependency to ensure the function will be
			 * dumped first.  (In 7.3 and later the regular dependency
			 * mechanism will handle this for us.)
			 */
			FuncInfo   *funcInfo = findFuncByOid(planginfo[i].lanplcallfoid);

			if (funcInfo)
				addObjectDependency(&planginfo[i].dobj,
									funcInfo->dobj.dumpId);
		}
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return planginfo;
}

/*
 * getCasts
 *	  get basic information about every cast in the system
 *
 * numCasts is set to the number of casts read in
 */
CastInfo *
getCasts(int *numCasts)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	CastInfo   *castinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_castsource;
	int			i_casttarget;
	int			i_castfunc;
	int			i_castcontext;

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	if (g_fout->remoteVersion >= 70300)
	{
		appendPQExpBuffer(query, "SELECT tableoid, oid, "
						  "castsource, casttarget, castfunc, castcontext "
						  "FROM pg_cast ORDER BY 3,4");
	}
	else
	{
		appendPQExpBuffer(query, "SELECT 0 as tableoid, p.oid, "
						  "t1.oid as castsource, t2.oid as casttarget, "
						  "p.oid as castfunc, 'e' as castcontext "
						  "FROM pg_type t1, pg_type t2, pg_proc p "
						  "WHERE p.pronargs = 1 AND "
						  "p.proargtypes[0] = t1.oid AND "
						  "p.prorettype = t2.oid AND p.proname = t2.typname "
						  "ORDER BY 3,4");
	}

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	*numCasts = ntups;

	castinfo = (CastInfo *) malloc(ntups * sizeof(CastInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_castsource = PQfnumber(res, "castsource");
	i_casttarget = PQfnumber(res, "casttarget");
	i_castfunc = PQfnumber(res, "castfunc");
	i_castcontext = PQfnumber(res, "castcontext");

	for (i = 0; i < ntups; i++)
	{
		PQExpBufferData namebuf;
		TypeInfo   *sTypeInfo;
		TypeInfo   *tTypeInfo;

		castinfo[i].dobj.objType = DO_CAST;
		castinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		castinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&castinfo[i].dobj);
		castinfo[i].castsource = atooid(PQgetvalue(res, i, i_castsource));
		castinfo[i].casttarget = atooid(PQgetvalue(res, i, i_casttarget));
		castinfo[i].castfunc = atooid(PQgetvalue(res, i, i_castfunc));
		castinfo[i].castcontext = *(PQgetvalue(res, i, i_castcontext));

		/*
		 * Try to name cast as concatenation of typnames.  This is only used
		 * for purposes of sorting.  If we fail to find either type, the name
		 * will be an empty string.
		 */
		initPQExpBuffer(&namebuf);
		sTypeInfo = findTypeByOid(castinfo[i].castsource);
		tTypeInfo = findTypeByOid(castinfo[i].casttarget);
		if (sTypeInfo && tTypeInfo)
			appendPQExpBuffer(&namebuf, "%s %s",
							  sTypeInfo->dobj.name, tTypeInfo->dobj.name);
		castinfo[i].dobj.name = namebuf.data;

		if (OidIsValid(castinfo[i].castfunc))
		{
			/*
			 * We need to make a dependency to ensure the function will be
			 * dumped first.  (In 7.3 and later the regular dependency
			 * mechanism will handle this for us.)
			 */
			FuncInfo   *funcInfo;

			funcInfo = findFuncByOid(castinfo[i].castfunc);
			if (funcInfo)
				addObjectDependency(&castinfo[i].dobj,
									funcInfo->dobj.dumpId);
		}
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return castinfo;
}

/*
 * getTableAttrs -
 *	  for each interesting table, read info about its attributes
 *	  (names, types, default values, CHECK constraints, etc)
 *
 * This is implemented in a very inefficient way right now, looping
 * through the tblinfo and doing a join per table to find the attrs and their
 * types.  However, because we want type names and so forth to be named
 * relative to the schema of each table, we couldn't do it in just one
 * query.  (Maybe one query per schema?)
 *
 *	modifies tblinfo
 */
void
getTableAttrs(TableInfo *tblinfo, int numTables)
{
	int			i,
				j;
	PQExpBuffer q = createPQExpBuffer();
	int			i_attnum;
	int			i_attname;
	int			i_atttypname;
	int			i_atttypmod;
	int			i_attstattarget;
	int			i_attstorage;
	int			i_typstorage;
	int			i_attnotnull;
	int			i_atthasdef;
	int			i_attisdropped;
	int			i_attislocal;
	PGresult   *res;
	int			ntups;
	bool		hasdefaults;

	for (i = 0; i < numTables; i++)
	{
		TableInfo  *tbinfo = &tblinfo[i];

		/* Don't bother to collect info for sequences */
		if (tbinfo->relkind == RELKIND_SEQUENCE)
			continue;

		/* Don't bother with uninteresting tables, either */
		if (!tbinfo->interesting)
			continue;

		/*
		 * Make sure we are in proper schema for this table; this allows
		 * correct retrieval of formatted type names and default exprs
		 */
		selectSourceSchema(tbinfo->dobj.namespace->dobj.name);

		/* find all the user attributes and their types */

		/*
		 * we must read the attribute names in attribute number order! because
		 * we will use the attnum to index into the attnames array later.  We
		 * actually ask to order by "attrelid, attnum" because (at least up to
		 * 7.3) the planner is not smart enough to realize it needn't re-sort
		 * the output of an indexscan on pg_attribute_relid_attnum_index.
		 */
		if (g_verbose)
			write_msg(NULL, "finding the columns and types of table \"%s\"\n",
					  tbinfo->dobj.name);

		resetPQExpBuffer(q);

		if (g_fout->remoteVersion >= 70300)
		{
			/* need left join here to not fail on dropped columns ... */
			appendPQExpBuffer(q, "SELECT a.attnum, a.attname, a.atttypmod, a.attstattarget, a.attstorage, t.typstorage, "
				  "a.attnotnull, a.atthasdef, a.attisdropped, a.attislocal, "
				   "pg_catalog.format_type(t.oid,a.atttypmod) as atttypname "
			 "from pg_catalog.pg_attribute a left join pg_catalog.pg_type t "
							  "on a.atttypid = t.oid "
							  "where a.attrelid = '%u'::pg_catalog.oid "
							  "and a.attnum > 0::pg_catalog.int2 "
							  "order by a.attrelid, a.attnum",
							  tbinfo->dobj.catId.oid);
		}
		else if (g_fout->remoteVersion >= 70100)
		{
			/*
			 * attstattarget doesn't exist in 7.1.  It does exist in 7.2, but
			 * we don't dump it because we can't tell whether it's been
			 * explicitly set or was just a default.
			 */
			appendPQExpBuffer(q, "SELECT a.attnum, a.attname, a.atttypmod, -1 as attstattarget, a.attstorage, t.typstorage, "
							  "a.attnotnull, a.atthasdef, false as attisdropped, false as attislocal, "
							  "format_type(t.oid,a.atttypmod) as atttypname "
							  "from pg_attribute a left join pg_type t "
							  "on a.atttypid = t.oid "
							  "where a.attrelid = '%u'::oid "
							  "and a.attnum > 0::int2 "
							  "order by a.attrelid, a.attnum",
							  tbinfo->dobj.catId.oid);
		}
		else
		{
			/* format_type not available before 7.1 */
			appendPQExpBuffer(q, "SELECT attnum, attname, atttypmod, -1 as attstattarget, attstorage, attstorage as typstorage, "
							  "attnotnull, atthasdef, false as attisdropped, false as attislocal, "
							  "(select typname from pg_type where oid = atttypid) as atttypname "
							  "from pg_attribute a "
							  "where attrelid = '%u'::oid "
							  "and attnum > 0::int2 "
							  "order by attrelid, attnum",
							  tbinfo->dobj.catId.oid);
		}

		res = PQexec(g_conn, q->data);
		check_sql_result(res, g_conn, q->data, PGRES_TUPLES_OK);

		ntups = PQntuples(res);

		i_attnum = PQfnumber(res, "attnum");
		i_attname = PQfnumber(res, "attname");
		i_atttypname = PQfnumber(res, "atttypname");
		i_atttypmod = PQfnumber(res, "atttypmod");
		i_attstattarget = PQfnumber(res, "attstattarget");
		i_attstorage = PQfnumber(res, "attstorage");
		i_typstorage = PQfnumber(res, "typstorage");
		i_attnotnull = PQfnumber(res, "attnotnull");
		i_atthasdef = PQfnumber(res, "atthasdef");
		i_attisdropped = PQfnumber(res, "attisdropped");
		i_attislocal = PQfnumber(res, "attislocal");

		tbinfo->numatts = ntups;
		tbinfo->attnames = (char **) malloc(ntups * sizeof(char *));
		tbinfo->atttypnames = (char **) malloc(ntups * sizeof(char *));
		tbinfo->atttypmod = (int *) malloc(ntups * sizeof(int));
		tbinfo->attstattarget = (int *) malloc(ntups * sizeof(int));
		tbinfo->attstorage = (char *) malloc(ntups * sizeof(char));
		tbinfo->typstorage = (char *) malloc(ntups * sizeof(char));
		tbinfo->attisdropped = (bool *) malloc(ntups * sizeof(bool));
		tbinfo->attislocal = (bool *) malloc(ntups * sizeof(bool));
		tbinfo->notnull = (bool *) malloc(ntups * sizeof(bool));
		tbinfo->attrdefs = (AttrDefInfo **) malloc(ntups * sizeof(AttrDefInfo *));
		tbinfo->inhAttrs = (bool *) malloc(ntups * sizeof(bool));
		tbinfo->inhAttrDef = (bool *) malloc(ntups * sizeof(bool));
		tbinfo->inhNotNull = (bool *) malloc(ntups * sizeof(bool));
		hasdefaults = false;

		for (j = 0; j < ntups; j++)
		{
			if (j + 1 != atoi(PQgetvalue(res, j, i_attnum)))
			{
				write_msg(NULL, "invalid column numbering in table \"%s\"\n",
						  tbinfo->dobj.name);
				exit_nicely();
			}
			tbinfo->attnames[j] = strdup(PQgetvalue(res, j, i_attname));
			tbinfo->atttypnames[j] = strdup(PQgetvalue(res, j, i_atttypname));
			tbinfo->atttypmod[j] = atoi(PQgetvalue(res, j, i_atttypmod));
			tbinfo->attstattarget[j] = atoi(PQgetvalue(res, j, i_attstattarget));
			tbinfo->attstorage[j] = *(PQgetvalue(res, j, i_attstorage));
			tbinfo->typstorage[j] = *(PQgetvalue(res, j, i_typstorage));
			tbinfo->attisdropped[j] = (PQgetvalue(res, j, i_attisdropped)[0] == 't');
			tbinfo->attislocal[j] = (PQgetvalue(res, j, i_attislocal)[0] == 't');
			tbinfo->notnull[j] = (PQgetvalue(res, j, i_attnotnull)[0] == 't');
			tbinfo->attrdefs[j] = NULL; /* fix below */
			if (PQgetvalue(res, j, i_atthasdef)[0] == 't')
				hasdefaults = true;
			/* these flags will be set in flagInhAttrs() */
			tbinfo->inhAttrs[j] = false;
			tbinfo->inhAttrDef[j] = false;
			tbinfo->inhNotNull[j] = false;
		}

		PQclear(res);

		/*
		 * Get info about column defaults
		 */
		if (hasdefaults)
		{
			AttrDefInfo *attrdefs;
			int			numDefaults;

			if (g_verbose)
				write_msg(NULL, "finding default expressions of table \"%s\"\n",
						  tbinfo->dobj.name);

			resetPQExpBuffer(q);
			if (g_fout->remoteVersion >= 70300)
			{
				appendPQExpBuffer(q, "SELECT tableoid, oid, adnum, "
						   "pg_catalog.pg_get_expr(adbin, adrelid) AS adsrc "
								  "FROM pg_catalog.pg_attrdef "
								  "WHERE adrelid = '%u'::pg_catalog.oid",
								  tbinfo->dobj.catId.oid);
			}
			else if (g_fout->remoteVersion >= 70200)
			{
				/* 7.2 did not have OIDs in pg_attrdef */
				appendPQExpBuffer(q, "SELECT tableoid, 0 as oid, adnum, "
								  "pg_get_expr(adbin, adrelid) AS adsrc "
								  "FROM pg_attrdef "
								  "WHERE adrelid = '%u'::oid",
								  tbinfo->dobj.catId.oid);
			}
			else if (g_fout->remoteVersion >= 70100)
			{
				/* no pg_get_expr, so must rely on adsrc */
				appendPQExpBuffer(q, "SELECT tableoid, oid, adnum, adsrc "
								  "FROM pg_attrdef "
								  "WHERE adrelid = '%u'::oid",
								  tbinfo->dobj.catId.oid);
			}
			else
			{
				/* no pg_get_expr, no tableoid either */
				appendPQExpBuffer(q, "SELECT "
								  "(SELECT oid FROM pg_class WHERE relname = 'pg_attrdef') AS tableoid, "
								  "oid, adnum, adsrc "
								  "FROM pg_attrdef "
								  "WHERE adrelid = '%u'::oid",
								  tbinfo->dobj.catId.oid);
			}
			res = PQexec(g_conn, q->data);
			check_sql_result(res, g_conn, q->data, PGRES_TUPLES_OK);

			numDefaults = PQntuples(res);
			attrdefs = (AttrDefInfo *) malloc(numDefaults * sizeof(AttrDefInfo));

			for (j = 0; j < numDefaults; j++)
			{
				int			adnum;

				attrdefs[j].dobj.objType = DO_ATTRDEF;
				attrdefs[j].dobj.catId.tableoid = atooid(PQgetvalue(res, j, 0));
				attrdefs[j].dobj.catId.oid = atooid(PQgetvalue(res, j, 1));
				AssignDumpId(&attrdefs[j].dobj);
				attrdefs[j].adtable = tbinfo;
				attrdefs[j].adnum = adnum = atoi(PQgetvalue(res, j, 2));
				attrdefs[j].adef_expr = strdup(PQgetvalue(res, j, 3));

				attrdefs[j].dobj.name = strdup(tbinfo->dobj.name);
				attrdefs[j].dobj.namespace = tbinfo->dobj.namespace;

				attrdefs[j].dobj.dump = tbinfo->dobj.dump;

				/*
				 * Defaults on a VIEW must always be dumped as separate ALTER
				 * TABLE commands.	Defaults on regular tables are dumped as
				 * part of the CREATE TABLE if possible.  To check if it's
				 * safe, we mark the default as needing to appear before the
				 * CREATE.
				 */
				if (tbinfo->relkind == RELKIND_VIEW)
				{
					attrdefs[j].separate = true;
					/* needed in case pre-7.3 DB: */
					addObjectDependency(&attrdefs[j].dobj,
										tbinfo->dobj.dumpId);
				}
				else
				{
					attrdefs[j].separate = false;
					addObjectDependency(&tbinfo->dobj,
										attrdefs[j].dobj.dumpId);
				}

				if (adnum <= 0 || adnum > ntups)
				{
					write_msg(NULL, "invalid adnum value %d for table \"%s\"\n",
							  adnum, tbinfo->dobj.name);
					exit_nicely();
				}
				tbinfo->attrdefs[adnum - 1] = &attrdefs[j];
			}
			PQclear(res);
		}

		/*
		 * Get info about table CHECK constraints
		 */
		if (tbinfo->ncheck > 0)
		{
			ConstraintInfo *constrs;
			int			numConstrs;

			if (g_verbose)
				write_msg(NULL, "finding check constraints for table \"%s\"\n",
						  tbinfo->dobj.name);

			resetPQExpBuffer(q);
			if (g_fout->remoteVersion >= 70400)
			{
				appendPQExpBuffer(q, "SELECT tableoid, oid, conname, "
							"pg_catalog.pg_get_constraintdef(oid) AS consrc "
								  "FROM pg_catalog.pg_constraint "
								  "WHERE conrelid = '%u'::pg_catalog.oid "
								  "   AND contype = 'c' "
								  "ORDER BY conname",
								  tbinfo->dobj.catId.oid);
			}
			else if (g_fout->remoteVersion >= 70300)
			{
				/* no pg_get_constraintdef, must use consrc */
				appendPQExpBuffer(q, "SELECT tableoid, oid, conname, "
								  "'CHECK (' || consrc || ')' AS consrc "
								  "FROM pg_catalog.pg_constraint "
								  "WHERE conrelid = '%u'::pg_catalog.oid "
								  "   AND contype = 'c' "
								  "ORDER BY conname",
								  tbinfo->dobj.catId.oid);
			}
			else if (g_fout->remoteVersion >= 70200)
			{
				/* 7.2 did not have OIDs in pg_relcheck */
				appendPQExpBuffer(q, "SELECT tableoid, 0 as oid, "
								  "rcname AS conname, "
								  "'CHECK (' || rcsrc || ')' AS consrc "
								  "FROM pg_relcheck "
								  "WHERE rcrelid = '%u'::oid "
								  "ORDER BY rcname",
								  tbinfo->dobj.catId.oid);
			}
			else if (g_fout->remoteVersion >= 70100)
			{
				appendPQExpBuffer(q, "SELECT tableoid, oid, "
								  "rcname AS conname, "
								  "'CHECK (' || rcsrc || ')' AS consrc "
								  "FROM pg_relcheck "
								  "WHERE rcrelid = '%u'::oid "
								  "ORDER BY rcname",
								  tbinfo->dobj.catId.oid);
			}
			else
			{
				/* no tableoid in 7.0 */
				appendPQExpBuffer(q, "SELECT "
								  "(SELECT oid FROM pg_class WHERE relname = 'pg_relcheck') AS tableoid, "
								  "oid, rcname AS conname, "
								  "'CHECK (' || rcsrc || ')' AS consrc "
								  "FROM pg_relcheck "
								  "WHERE rcrelid = '%u'::oid "
								  "ORDER BY rcname",
								  tbinfo->dobj.catId.oid);
			}
			res = PQexec(g_conn, q->data);
			check_sql_result(res, g_conn, q->data, PGRES_TUPLES_OK);

			numConstrs = PQntuples(res);
			if (numConstrs != tbinfo->ncheck)
			{
				write_msg(NULL, "expected %d check constraints on table \"%s\" but found %d\n",
						  tbinfo->ncheck, tbinfo->dobj.name, numConstrs);
				write_msg(NULL, "(The system catalogs might be corrupted.)\n");
				exit_nicely();
			}

			constrs = (ConstraintInfo *) malloc(numConstrs * sizeof(ConstraintInfo));
			tbinfo->checkexprs = constrs;

			for (j = 0; j < numConstrs; j++)
			{
				constrs[j].dobj.objType = DO_CONSTRAINT;
				constrs[j].dobj.catId.tableoid = atooid(PQgetvalue(res, j, 0));
				constrs[j].dobj.catId.oid = atooid(PQgetvalue(res, j, 1));
				AssignDumpId(&constrs[j].dobj);
				constrs[j].dobj.name = strdup(PQgetvalue(res, j, 2));
				constrs[j].dobj.namespace = tbinfo->dobj.namespace;
				constrs[j].contable = tbinfo;
				constrs[j].condomain = NULL;
				constrs[j].contype = 'c';
				constrs[j].condef = strdup(PQgetvalue(res, j, 3));
				constrs[j].conindex = 0;
				constrs[j].coninherited = false;
				constrs[j].separate = false;

				constrs[j].dobj.dump = tbinfo->dobj.dump;

				/*
				 * Mark the constraint as needing to appear before the table
				 * --- this is so that any other dependencies of the
				 * constraint will be emitted before we try to create the
				 * table.
				 */
				addObjectDependency(&tbinfo->dobj,
									constrs[j].dobj.dumpId);

				/*
				 * If the constraint is inherited, this will be detected
				 * later.  We also detect later if the constraint must be
				 * split out from the table definition.
				 */
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
 * object handed to this routine. The routine takes a constant character
 * string for the target part of the comment-creation command, plus
 * the namespace and owner of the object (for labeling the ArchiveEntry),
 * plus catalog ID and subid which are the lookup key for pg_description,
 * plus the dump ID for the object (for setting a dependency).
 * If a matching pg_description entry is found, it is dumped.
 *
 * Note: although this routine takes a dumpId for dependency purposes,
 * that purpose is just to mark the dependency in the emitted dump file
 * for possible future use by pg_restore.  We do NOT use it for determining
 * ordering of the comment in the dump file, because this routine is called
 * after dependency sorting occurs.  This routine should be called just after
 * calling ArchiveEntry() for the specified object.
 */
static void
dumpComment(Archive *fout, const char *target,
			const char *namespace, const char *owner,
			CatalogId catalogId, int subid, DumpId dumpId)
{
	CommentItem *comments;
	int			ncomments;

	/* Comments are SCHEMA not data */
	if (dataOnly)
		return;

	/* Search for comments associated with catalogId, using table */
	ncomments = findComments(fout, catalogId.tableoid, catalogId.oid,
							 &comments);

	/* Is there one matching the subid? */
	while (ncomments > 0)
	{
		if (comments->objsubid == subid)
			break;
		comments++;
		ncomments--;
	}

	/* If a comment exists, build COMMENT ON statement */
	if (ncomments > 0)
	{
		PQExpBuffer query = createPQExpBuffer();

		appendPQExpBuffer(query, "COMMENT ON %s IS ", target);
		appendStringLiteralAH(query, comments->descr, fout);
		appendPQExpBuffer(query, ";\n");

		ArchiveEntry(fout, nilCatalogId, createDumpId(),
					 target, namespace, NULL, owner, false,
					 "COMMENT", query->data, "", NULL,
					 &(dumpId), 1,
					 NULL, NULL);

		destroyPQExpBuffer(query);
	}
}

/*
 * dumpTableComment --
 *
 * As above, but dump comments for both the specified table (or view)
 * and its columns.
 */
static void
dumpTableComment(Archive *fout, TableInfo *tbinfo,
				 const char *reltypename)
{
	CommentItem *comments;
	int			ncomments;
	PQExpBuffer query;
	PQExpBuffer target;

	/* Comments are SCHEMA not data */
	if (dataOnly)
		return;

	/* Search for comments associated with relation, using table */
	ncomments = findComments(fout,
							 tbinfo->dobj.catId.tableoid,
							 tbinfo->dobj.catId.oid,
							 &comments);

	/* If comments exist, build COMMENT ON statements */
	if (ncomments <= 0)
		return;

	query = createPQExpBuffer();
	target = createPQExpBuffer();

	while (ncomments > 0)
	{
		const char *descr = comments->descr;
		int			objsubid = comments->objsubid;

		if (objsubid == 0)
		{
			resetPQExpBuffer(target);
			appendPQExpBuffer(target, "%s %s", reltypename,
							  fmtId(tbinfo->dobj.name));

			resetPQExpBuffer(query);
			appendPQExpBuffer(query, "COMMENT ON %s IS ", target->data);
			appendStringLiteralAH(query, descr, fout);
			appendPQExpBuffer(query, ";\n");

			ArchiveEntry(fout, nilCatalogId, createDumpId(),
						 target->data,
						 tbinfo->dobj.namespace->dobj.name,
						 NULL,
						 tbinfo->rolname,
						 false, "COMMENT", query->data, "", NULL,
						 &(tbinfo->dobj.dumpId), 1,
						 NULL, NULL);
		}
		else if (objsubid > 0 && objsubid <= tbinfo->numatts)
		{
			resetPQExpBuffer(target);
			appendPQExpBuffer(target, "COLUMN %s.",
							  fmtId(tbinfo->dobj.name));
			appendPQExpBuffer(target, "%s",
							  fmtId(tbinfo->attnames[objsubid - 1]));

			resetPQExpBuffer(query);
			appendPQExpBuffer(query, "COMMENT ON %s IS ", target->data);
			appendStringLiteralAH(query, descr, fout);
			appendPQExpBuffer(query, ";\n");

			ArchiveEntry(fout, nilCatalogId, createDumpId(),
						 target->data,
						 tbinfo->dobj.namespace->dobj.name,
						 NULL,
						 tbinfo->rolname,
						 false, "COMMENT", query->data, "", NULL,
						 &(tbinfo->dobj.dumpId), 1,
						 NULL, NULL);
		}

		comments++;
		ncomments--;
	}

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(target);
}

/*
 * findComments --
 *
 * Find the comment(s), if any, associated with the given object.  All the
 * objsubid values associated with the given classoid/objoid are found with
 * one search.
 */
static int
findComments(Archive *fout, Oid classoid, Oid objoid,
			 CommentItem **items)
{
	/* static storage for table of comments */
	static CommentItem *comments = NULL;
	static int	ncomments = -1;

	CommentItem *middle = NULL;
	CommentItem *low;
	CommentItem *high;
	int			nmatch;

	/* Get comments if we didn't already */
	if (ncomments < 0)
		ncomments = collectComments(fout, &comments);

	/*
	 * Pre-7.2, pg_description does not contain classoid, so collectComments
	 * just stores a zero.	If there's a collision on object OID, well, you
	 * get duplicate comments.
	 */
	if (fout->remoteVersion < 70200)
		classoid = 0;

	/*
	 * Do binary search to find some item matching the object.
	 */
	low = &comments[0];
	high = &comments[ncomments - 1];
	while (low <= high)
	{
		middle = low + (high - low) / 2;

		if (classoid < middle->classoid)
			high = middle - 1;
		else if (classoid > middle->classoid)
			low = middle + 1;
		else if (objoid < middle->objoid)
			high = middle - 1;
		else if (objoid > middle->objoid)
			low = middle + 1;
		else
			break;				/* found a match */
	}

	if (low > high)				/* no matches */
	{
		*items = NULL;
		return 0;
	}

	/*
	 * Now determine how many items match the object.  The search loop
	 * invariant still holds: only items between low and high inclusive could
	 * match.
	 */
	nmatch = 1;
	while (middle > low)
	{
		if (classoid != middle[-1].classoid ||
			objoid != middle[-1].objoid)
			break;
		middle--;
		nmatch++;
	}

	*items = middle;

	middle += nmatch;
	while (middle <= high)
	{
		if (classoid != middle->classoid ||
			objoid != middle->objoid)
			break;
		middle++;
		nmatch++;
	}

	return nmatch;
}

/*
 * collectComments --
 *
 * Construct a table of all comments available for database objects.
 * We used to do per-object queries for the comments, but it's much faster
 * to pull them all over at once, and on most databases the memory cost
 * isn't high.
 *
 * The table is sorted by classoid/objid/objsubid for speed in lookup.
 */
static int
collectComments(Archive *fout, CommentItem **items)
{
	PGresult   *res;
	PQExpBuffer query;
	int			i_description;
	int			i_classoid;
	int			i_objoid;
	int			i_objsubid;
	int			ntups;
	int			i;
	CommentItem *comments;

	/*
	 * Note we do NOT change source schema here; preserve the caller's
	 * setting, instead.
	 */

	query = createPQExpBuffer();

	if (fout->remoteVersion >= 70300)
	{
		appendPQExpBuffer(query, "SELECT description, classoid, objoid, objsubid "
						  "FROM pg_catalog.pg_description "
						  "ORDER BY classoid, objoid, objsubid");
	}
	else if (fout->remoteVersion >= 70200)
	{
		appendPQExpBuffer(query, "SELECT description, classoid, objoid, objsubid "
						  "FROM pg_description "
						  "ORDER BY classoid, objoid, objsubid");
	}
	else
	{
		/* Note: this will fail to find attribute comments in pre-7.2... */
		appendPQExpBuffer(query, "SELECT description, 0 as classoid, objoid, 0 as objsubid "
						  "FROM pg_description "
						  "ORDER BY objoid");
	}

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	/* Construct lookup table containing OIDs in numeric form */

	i_description = PQfnumber(res, "description");
	i_classoid = PQfnumber(res, "classoid");
	i_objoid = PQfnumber(res, "objoid");
	i_objsubid = PQfnumber(res, "objsubid");

	ntups = PQntuples(res);

	comments = (CommentItem *) malloc(ntups * sizeof(CommentItem));

	for (i = 0; i < ntups; i++)
	{
		comments[i].descr = PQgetvalue(res, i, i_description);
		comments[i].classoid = atooid(PQgetvalue(res, i, i_classoid));
		comments[i].objoid = atooid(PQgetvalue(res, i, i_objoid));
		comments[i].objsubid = atoi(PQgetvalue(res, i, i_objsubid));
	}

	/* Do NOT free the PGresult since we are keeping pointers into it */
	destroyPQExpBuffer(query);

	*items = comments;
	return ntups;
}

/*
 * dumpDumpableObject
 *
 * This routine and its subsidiaries are responsible for creating
 * ArchiveEntries (TOC objects) for each object to be dumped.
 */
static void
dumpDumpableObject(Archive *fout, DumpableObject *dobj)
{
	switch (dobj->objType)
	{
		case DO_NAMESPACE:
			dumpNamespace(fout, (NamespaceInfo *) dobj);
			break;
		case DO_TYPE:
			dumpType(fout, (TypeInfo *) dobj);
			break;
		case DO_SHELL_TYPE:
			dumpShellType(fout, (ShellTypeInfo *) dobj);
			break;
		case DO_FUNC:
			dumpFunc(fout, (FuncInfo *) dobj);
			break;
		case DO_AGG:
			dumpAgg(fout, (AggInfo *) dobj);
			break;
		case DO_OPERATOR:
			dumpOpr(fout, (OprInfo *) dobj);
			break;
		case DO_OPCLASS:
			dumpOpclass(fout, (OpclassInfo *) dobj);
			break;
		case DO_CONVERSION:
			dumpConversion(fout, (ConvInfo *) dobj);
			break;
		case DO_TABLE:
			dumpTable(fout, (TableInfo *) dobj);
			break;
		case DO_ATTRDEF:
			dumpAttrDef(fout, (AttrDefInfo *) dobj);
			break;
		case DO_INDEX:
			dumpIndex(fout, (IndxInfo *) dobj);
			break;
		case DO_RULE:
			dumpRule(fout, (RuleInfo *) dobj);
			break;
		case DO_TRIGGER:
			dumpTrigger(fout, (TriggerInfo *) dobj);
			break;
		case DO_CONSTRAINT:
			dumpConstraint(fout, (ConstraintInfo *) dobj);
			break;
		case DO_FK_CONSTRAINT:
			dumpConstraint(fout, (ConstraintInfo *) dobj);
			break;
		case DO_PROCLANG:
			dumpProcLang(fout, (ProcLangInfo *) dobj);
			break;
		case DO_CAST:
			dumpCast(fout, (CastInfo *) dobj);
			break;
		case DO_TABLE_DATA:
			dumpTableData(fout, (TableDataInfo *) dobj);
			break;
		case DO_TABLE_TYPE:
			/* table rowtypes are never dumped separately */
			break;
		case DO_BLOBS:
			ArchiveEntry(fout, dobj->catId, dobj->dumpId,
						 dobj->name, NULL, NULL, "",
						 false, "BLOBS", "", "", NULL,
						 NULL, 0,
						 dumpBlobs, NULL);
			break;
		case DO_BLOB_COMMENTS:
			ArchiveEntry(fout, dobj->catId, dobj->dumpId,
						 dobj->name, NULL, NULL, "",
						 false, "BLOB COMMENTS", "", "", NULL,
						 NULL, 0,
						 dumpBlobComments, NULL);
			break;
	}
}

/*
 * dumpNamespace
 *	  writes out to fout the queries to recreate a user-defined namespace
 */
static void
dumpNamespace(Archive *fout, NamespaceInfo *nspinfo)
{
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *qnspname;

	/* Skip if not to be dumped */
	if (!nspinfo->dobj.dump || dataOnly)
		return;

	/* don't dump dummy namespace from pre-7.3 source */
	if (strlen(nspinfo->dobj.name) == 0)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	qnspname = strdup(fmtId(nspinfo->dobj.name));

	appendPQExpBuffer(delq, "DROP SCHEMA %s;\n", qnspname);

	appendPQExpBuffer(q, "CREATE SCHEMA %s;\n", qnspname);

	ArchiveEntry(fout, nspinfo->dobj.catId, nspinfo->dobj.dumpId,
				 nspinfo->dobj.name,
				 NULL, NULL,
				 nspinfo->rolname,
				 false, "SCHEMA", q->data, delq->data, NULL,
				 nspinfo->dobj.dependencies, nspinfo->dobj.nDeps,
				 NULL, NULL);

	/* Dump Schema Comments */
	resetPQExpBuffer(q);
	appendPQExpBuffer(q, "SCHEMA %s", qnspname);
	dumpComment(fout, q->data,
				NULL, nspinfo->rolname,
				nspinfo->dobj.catId, 0, nspinfo->dobj.dumpId);

	dumpACL(fout, nspinfo->dobj.catId, nspinfo->dobj.dumpId, "SCHEMA",
			qnspname, nspinfo->dobj.name, NULL,
			nspinfo->rolname, nspinfo->nspacl);

	free(qnspname);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
}

/*
 * dumpType
 *	  writes out to fout the queries to recreate a user-defined type
 */
static void
dumpType(Archive *fout, TypeInfo *tinfo)
{
	/* Skip if not to be dumped */
	if (!tinfo->dobj.dump || dataOnly)
		return;

	/* Dump out in proper style */
	if (tinfo->typtype == 'b')
		dumpBaseType(fout, tinfo);
	else if (tinfo->typtype == 'd')
		dumpDomain(fout, tinfo);
	else if (tinfo->typtype == 'c')
		dumpCompositeType(fout, tinfo);
}

/*
 * dumpBaseType
 *	  writes out to fout the queries to recreate a user-defined base type
 */
static void
dumpBaseType(Archive *fout, TypeInfo *tinfo)
{
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;
	int			ntups;
	char	   *typlen;
	char	   *typinput;
	char	   *typoutput;
	char	   *typreceive;
	char	   *typsend;
	char	   *typanalyze;
	Oid			typinputoid;
	Oid			typoutputoid;
	Oid			typreceiveoid;
	Oid			typsendoid;
	Oid			typanalyzeoid;
	char	   *typdelim;
	char	   *typbyval;
	char	   *typalign;
	char	   *typstorage;
	char	   *typdefault;
	bool		typdefault_is_literal = false;

	/* Set proper schema search path so regproc references list correctly */
	selectSourceSchema(tinfo->dobj.namespace->dobj.name);

	/* Fetch type-specific details */
	if (fout->remoteVersion >= 80000)
	{
		appendPQExpBuffer(query, "SELECT typlen, "
						  "typinput, typoutput, typreceive, typsend, "
						  "typanalyze, "
						  "typinput::pg_catalog.oid as typinputoid, "
						  "typoutput::pg_catalog.oid as typoutputoid, "
						  "typreceive::pg_catalog.oid as typreceiveoid, "
						  "typsend::pg_catalog.oid as typsendoid, "
						  "typanalyze::pg_catalog.oid as typanalyzeoid, "
						  "typdelim, typbyval, typalign, typstorage, "
						  "pg_catalog.pg_get_expr(typdefaultbin, 'pg_catalog.pg_type'::pg_catalog.regclass) as typdefaultbin, typdefault "
						  "FROM pg_catalog.pg_type "
						  "WHERE oid = '%u'::pg_catalog.oid",
						  tinfo->dobj.catId.oid);
	}
	else if (fout->remoteVersion >= 70400)
	{
		appendPQExpBuffer(query, "SELECT typlen, "
						  "typinput, typoutput, typreceive, typsend, "
						  "'-' as typanalyze, "
						  "typinput::pg_catalog.oid as typinputoid, "
						  "typoutput::pg_catalog.oid as typoutputoid, "
						  "typreceive::pg_catalog.oid as typreceiveoid, "
						  "typsend::pg_catalog.oid as typsendoid, "
						  "0 as typanalyzeoid, "
						  "typdelim, typbyval, typalign, typstorage, "
						  "pg_catalog.pg_get_expr(typdefaultbin, 'pg_catalog.pg_type'::pg_catalog.regclass) as typdefaultbin, typdefault "
						  "FROM pg_catalog.pg_type "
						  "WHERE oid = '%u'::pg_catalog.oid",
						  tinfo->dobj.catId.oid);
	}
	else if (fout->remoteVersion >= 70300)
	{
		appendPQExpBuffer(query, "SELECT typlen, "
						  "typinput, typoutput, "
						  "'-' as typreceive, '-' as typsend, "
						  "'-' as typanalyze, "
						  "typinput::pg_catalog.oid as typinputoid, "
						  "typoutput::pg_catalog.oid as typoutputoid, "
						  "0 as typreceiveoid, 0 as typsendoid, "
						  "0 as typanalyzeoid, "
						  "typdelim, typbyval, typalign, typstorage, "
						  "pg_catalog.pg_get_expr(typdefaultbin, 'pg_catalog.pg_type'::pg_catalog.regclass) as typdefaultbin, typdefault "
						  "FROM pg_catalog.pg_type "
						  "WHERE oid = '%u'::pg_catalog.oid",
						  tinfo->dobj.catId.oid);
	}
	else if (fout->remoteVersion >= 70200)
	{
		/*
		 * Note: although pre-7.3 catalogs contain typreceive and typsend,
		 * ignore them because they are not right.
		 */
		appendPQExpBuffer(query, "SELECT typlen, "
						  "typinput, typoutput, "
						  "'-' as typreceive, '-' as typsend, "
						  "'-' as typanalyze, "
						  "typinput::oid as typinputoid, "
						  "typoutput::oid as typoutputoid, "
						  "0 as typreceiveoid, 0 as typsendoid, "
						  "0 as typanalyzeoid, "
						  "typdelim, typbyval, typalign, typstorage, "
						  "NULL as typdefaultbin, typdefault "
						  "FROM pg_type "
						  "WHERE oid = '%u'::oid",
						  tinfo->dobj.catId.oid);
	}
	else if (fout->remoteVersion >= 70100)
	{
		/*
		 * Ignore pre-7.2 typdefault; the field exists but has an unusable
		 * representation.
		 */
		appendPQExpBuffer(query, "SELECT typlen, "
						  "typinput, typoutput, "
						  "'-' as typreceive, '-' as typsend, "
						  "'-' as typanalyze, "
						  "typinput::oid as typinputoid, "
						  "typoutput::oid as typoutputoid, "
						  "0 as typreceiveoid, 0 as typsendoid, "
						  "0 as typanalyzeoid, "
						  "typdelim, typbyval, typalign, typstorage, "
						  "NULL as typdefaultbin, NULL as typdefault "
						  "FROM pg_type "
						  "WHERE oid = '%u'::oid",
						  tinfo->dobj.catId.oid);
	}
	else
	{
		appendPQExpBuffer(query, "SELECT typlen, "
						  "typinput, typoutput, "
						  "'-' as typreceive, '-' as typsend, "
						  "'-' as typanalyze, "
						  "typinput::oid as typinputoid, "
						  "typoutput::oid as typoutputoid, "
						  "0 as typreceiveoid, 0 as typsendoid, "
						  "0 as typanalyzeoid, "
						  "typdelim, typbyval, typalign, "
						  "'p'::char as typstorage, "
						  "NULL as typdefaultbin, NULL as typdefault "
						  "FROM pg_type "
						  "WHERE oid = '%u'::oid",
						  tinfo->dobj.catId.oid);
	}

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	/* Expecting a single result only */
	ntups = PQntuples(res);
	if (ntups != 1)
	{
		write_msg(NULL, "Got %d rows instead of one from: %s",
				  ntups, query->data);
		exit_nicely();
	}

	typlen = PQgetvalue(res, 0, PQfnumber(res, "typlen"));
	typinput = PQgetvalue(res, 0, PQfnumber(res, "typinput"));
	typoutput = PQgetvalue(res, 0, PQfnumber(res, "typoutput"));
	typreceive = PQgetvalue(res, 0, PQfnumber(res, "typreceive"));
	typsend = PQgetvalue(res, 0, PQfnumber(res, "typsend"));
	typanalyze = PQgetvalue(res, 0, PQfnumber(res, "typanalyze"));
	typinputoid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typinputoid")));
	typoutputoid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typoutputoid")));
	typreceiveoid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typreceiveoid")));
	typsendoid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typsendoid")));
	typanalyzeoid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typanalyzeoid")));
	typdelim = PQgetvalue(res, 0, PQfnumber(res, "typdelim"));
	typbyval = PQgetvalue(res, 0, PQfnumber(res, "typbyval"));
	typalign = PQgetvalue(res, 0, PQfnumber(res, "typalign"));
	typstorage = PQgetvalue(res, 0, PQfnumber(res, "typstorage"));
	if (!PQgetisnull(res, 0, PQfnumber(res, "typdefaultbin")))
		typdefault = PQgetvalue(res, 0, PQfnumber(res, "typdefaultbin"));
	else if (!PQgetisnull(res, 0, PQfnumber(res, "typdefault")))
	{
		typdefault = PQgetvalue(res, 0, PQfnumber(res, "typdefault"));
		typdefault_is_literal = true;	/* it needs quotes */
	}
	else
		typdefault = NULL;

	/*
	 * DROP must be fully qualified in case same name appears in pg_catalog.
	 * The reason we include CASCADE is that the circular dependency between
	 * the type and its I/O functions makes it impossible to drop the type any
	 * other way.
	 */
	appendPQExpBuffer(delq, "DROP TYPE %s.",
					  fmtId(tinfo->dobj.namespace->dobj.name));
	appendPQExpBuffer(delq, "%s CASCADE;\n",
					  fmtId(tinfo->dobj.name));

	appendPQExpBuffer(q,
					  "CREATE TYPE %s (\n"
					  "    INTERNALLENGTH = %s",
					  fmtId(tinfo->dobj.name),
					  (strcmp(typlen, "-1") == 0) ? "variable" : typlen);

	if (fout->remoteVersion >= 70300)
	{
		/* regproc result is correctly quoted as of 7.3 */
		appendPQExpBuffer(q, ",\n    INPUT = %s", typinput);
		appendPQExpBuffer(q, ",\n    OUTPUT = %s", typoutput);
		if (OidIsValid(typreceiveoid))
			appendPQExpBuffer(q, ",\n    RECEIVE = %s", typreceive);
		if (OidIsValid(typsendoid))
			appendPQExpBuffer(q, ",\n    SEND = %s", typsend);
		if (OidIsValid(typanalyzeoid))
			appendPQExpBuffer(q, ",\n    ANALYZE = %s", typanalyze);
	}
	else
	{
		/* regproc delivers an unquoted name before 7.3 */
		/* cannot combine these because fmtId uses static result area */
		appendPQExpBuffer(q, ",\n    INPUT = %s", fmtId(typinput));
		appendPQExpBuffer(q, ",\n    OUTPUT = %s", fmtId(typoutput));
		/* no chance that receive/send/analyze need be printed */
	}

	if (typdefault != NULL)
	{
		appendPQExpBuffer(q, ",\n    DEFAULT = ");
		if (typdefault_is_literal)
			appendStringLiteralAH(q, typdefault, fout);
		else
			appendPQExpBufferStr(q, typdefault);
	}

	if (tinfo->isArray)
	{
		char	   *elemType;

		/* reselect schema in case changed by function dump */
		selectSourceSchema(tinfo->dobj.namespace->dobj.name);
		elemType = getFormattedTypeName(tinfo->typelem, zeroAsOpaque);
		appendPQExpBuffer(q, ",\n    ELEMENT = %s", elemType);
		free(elemType);
	}

	if (typdelim && strcmp(typdelim, ",") != 0)
	{
		appendPQExpBuffer(q, ",\n    DELIMITER = ");
		appendStringLiteralAH(q, typdelim, fout);
	}

	if (strcmp(typalign, "c") == 0)
		appendPQExpBuffer(q, ",\n    ALIGNMENT = char");
	else if (strcmp(typalign, "s") == 0)
		appendPQExpBuffer(q, ",\n    ALIGNMENT = int2");
	else if (strcmp(typalign, "i") == 0)
		appendPQExpBuffer(q, ",\n    ALIGNMENT = int4");
	else if (strcmp(typalign, "d") == 0)
		appendPQExpBuffer(q, ",\n    ALIGNMENT = double");

	if (strcmp(typstorage, "p") == 0)
		appendPQExpBuffer(q, ",\n    STORAGE = plain");
	else if (strcmp(typstorage, "e") == 0)
		appendPQExpBuffer(q, ",\n    STORAGE = external");
	else if (strcmp(typstorage, "x") == 0)
		appendPQExpBuffer(q, ",\n    STORAGE = extended");
	else if (strcmp(typstorage, "m") == 0)
		appendPQExpBuffer(q, ",\n    STORAGE = main");

	if (strcmp(typbyval, "t") == 0)
		appendPQExpBuffer(q, ",\n    PASSEDBYVALUE");

	appendPQExpBuffer(q, "\n);\n");

	ArchiveEntry(fout, tinfo->dobj.catId, tinfo->dobj.dumpId,
				 tinfo->dobj.name,
				 tinfo->dobj.namespace->dobj.name,
				 NULL,
				 tinfo->rolname, false,
				 "TYPE", q->data, delq->data, NULL,
				 tinfo->dobj.dependencies, tinfo->dobj.nDeps,
				 NULL, NULL);

	/* Dump Type Comments */
	resetPQExpBuffer(q);

	appendPQExpBuffer(q, "TYPE %s", fmtId(tinfo->dobj.name));
	dumpComment(fout, q->data,
				tinfo->dobj.namespace->dobj.name, tinfo->rolname,
				tinfo->dobj.catId, 0, tinfo->dobj.dumpId);

	PQclear(res);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(query);
}

/*
 * dumpDomain
 *	  writes out to fout the queries to recreate a user-defined domain
 */
static void
dumpDomain(Archive *fout, TypeInfo *tinfo)
{
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;
	int			ntups;
	int			i;
	char	   *typnotnull;
	char	   *typdefn;
	char	   *typdefault;
	bool		typdefault_is_literal = false;

	/* Set proper schema search path so type references list correctly */
	selectSourceSchema(tinfo->dobj.namespace->dobj.name);

	/* Fetch domain specific details */
	/* We assume here that remoteVersion must be at least 70300 */
	appendPQExpBuffer(query, "SELECT typnotnull, "
				"pg_catalog.format_type(typbasetype, typtypmod) as typdefn, "
					  "pg_catalog.pg_get_expr(typdefaultbin, 'pg_catalog.pg_type'::pg_catalog.regclass) as typdefaultbin, typdefault "
					  "FROM pg_catalog.pg_type "
					  "WHERE oid = '%u'::pg_catalog.oid",
					  tinfo->dobj.catId.oid);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

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
	if (!PQgetisnull(res, 0, PQfnumber(res, "typdefaultbin")))
		typdefault = PQgetvalue(res, 0, PQfnumber(res, "typdefaultbin"));
	else if (!PQgetisnull(res, 0, PQfnumber(res, "typdefault")))
	{
		typdefault = PQgetvalue(res, 0, PQfnumber(res, "typdefault"));
		typdefault_is_literal = true;	/* it needs quotes */
	}
	else
		typdefault = NULL;

	appendPQExpBuffer(q,
					  "CREATE DOMAIN %s AS %s",
					  fmtId(tinfo->dobj.name),
					  typdefn);

	if (typnotnull[0] == 't')
		appendPQExpBuffer(q, " NOT NULL");

	if (typdefault != NULL)
	{
		appendPQExpBuffer(q, " DEFAULT ");
		if (typdefault_is_literal)
			appendStringLiteralAH(q, typdefault, fout);
		else
			appendPQExpBufferStr(q, typdefault);
	}

	PQclear(res);

	/*
	 * Add any CHECK constraints for the domain
	 */
	for (i = 0; i < tinfo->nDomChecks; i++)
	{
		ConstraintInfo *domcheck = &(tinfo->domChecks[i]);

		if (!domcheck->separate)
			appendPQExpBuffer(q, "\n\tCONSTRAINT %s %s",
							  fmtId(domcheck->dobj.name), domcheck->condef);
	}

	appendPQExpBuffer(q, ";\n");

	/*
	 * DROP must be fully qualified in case same name appears in pg_catalog
	 */
	appendPQExpBuffer(delq, "DROP DOMAIN %s.",
					  fmtId(tinfo->dobj.namespace->dobj.name));
	appendPQExpBuffer(delq, "%s;\n",
					  fmtId(tinfo->dobj.name));

	ArchiveEntry(fout, tinfo->dobj.catId, tinfo->dobj.dumpId,
				 tinfo->dobj.name,
				 tinfo->dobj.namespace->dobj.name,
				 NULL,
				 tinfo->rolname, false,
				 "DOMAIN", q->data, delq->data, NULL,
				 tinfo->dobj.dependencies, tinfo->dobj.nDeps,
				 NULL, NULL);

	/* Dump Domain Comments */
	resetPQExpBuffer(q);

	appendPQExpBuffer(q, "DOMAIN %s", fmtId(tinfo->dobj.name));
	dumpComment(fout, q->data,
				tinfo->dobj.namespace->dobj.name, tinfo->rolname,
				tinfo->dobj.catId, 0, tinfo->dobj.dumpId);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(query);
}

/*
 * dumpCompositeType
 *	  writes out to fout the queries to recreate a user-defined stand-alone
 *	  composite type
 */
static void
dumpCompositeType(Archive *fout, TypeInfo *tinfo)
{
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;
	int			ntups;
	int			i_attname;
	int			i_atttypdefn;
	int			i;

	/* Set proper schema search path so type references list correctly */
	selectSourceSchema(tinfo->dobj.namespace->dobj.name);

	/* Fetch type specific details */
	/* We assume here that remoteVersion must be at least 70300 */

	appendPQExpBuffer(query, "SELECT a.attname, "
			 "pg_catalog.format_type(a.atttypid, a.atttypmod) as atttypdefn "
					  "FROM pg_catalog.pg_type t, pg_catalog.pg_attribute a "
					  "WHERE t.oid = '%u'::pg_catalog.oid "
					  "AND a.attrelid = t.typrelid "
					  "AND NOT a.attisdropped "
					  "ORDER BY a.attnum ",
					  tinfo->dobj.catId.oid);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	/* Expecting at least a single result */
	ntups = PQntuples(res);
	if (ntups < 1)
	{
		write_msg(NULL, "query yielded no rows: %s\n", query->data);
		exit_nicely();
	}

	i_attname = PQfnumber(res, "attname");
	i_atttypdefn = PQfnumber(res, "atttypdefn");

	appendPQExpBuffer(q, "CREATE TYPE %s AS (",
					  fmtId(tinfo->dobj.name));

	for (i = 0; i < ntups; i++)
	{
		char	   *attname;
		char	   *atttypdefn;

		attname = PQgetvalue(res, i, i_attname);
		atttypdefn = PQgetvalue(res, i, i_atttypdefn);

		appendPQExpBuffer(q, "\n\t%s %s", fmtId(attname), atttypdefn);
		if (i < ntups - 1)
			appendPQExpBuffer(q, ",");
	}
	appendPQExpBuffer(q, "\n);\n");

	/*
	 * DROP must be fully qualified in case same name appears in pg_catalog
	 */
	appendPQExpBuffer(delq, "DROP TYPE %s.",
					  fmtId(tinfo->dobj.namespace->dobj.name));
	appendPQExpBuffer(delq, "%s;\n",
					  fmtId(tinfo->dobj.name));

	ArchiveEntry(fout, tinfo->dobj.catId, tinfo->dobj.dumpId,
				 tinfo->dobj.name,
				 tinfo->dobj.namespace->dobj.name,
				 NULL,
				 tinfo->rolname, false,
				 "TYPE", q->data, delq->data, NULL,
				 tinfo->dobj.dependencies, tinfo->dobj.nDeps,
				 NULL, NULL);


	/* Dump Type Comments */
	resetPQExpBuffer(q);

	appendPQExpBuffer(q, "TYPE %s", fmtId(tinfo->dobj.name));
	dumpComment(fout, q->data,
				tinfo->dobj.namespace->dobj.name, tinfo->rolname,
				tinfo->dobj.catId, 0, tinfo->dobj.dumpId);

	PQclear(res);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(query);
}

/*
 * dumpShellType
 *	  writes out to fout the queries to create a shell type
 *
 * We dump a shell definition in advance of the I/O functions for the type.
 */
static void
dumpShellType(Archive *fout, ShellTypeInfo *stinfo)
{
	PQExpBuffer q;

	/* Skip if not to be dumped */
	if (!stinfo->dobj.dump || dataOnly)
		return;

	q = createPQExpBuffer();

	/*
	 * Note the lack of a DROP command for the shell type; any required DROP
	 * is driven off the base type entry, instead.	This interacts with
	 * _printTocEntry()'s use of the presence of a DROP command to decide
	 * whether an entry needs an ALTER OWNER command.  We don't want to alter
	 * the shell type's owner immediately on creation; that should happen only
	 * after it's filled in, otherwise the backend complains.
	 */

	appendPQExpBuffer(q, "CREATE TYPE %s;\n",
					  fmtId(stinfo->dobj.name));

	ArchiveEntry(fout, stinfo->dobj.catId, stinfo->dobj.dumpId,
				 stinfo->dobj.name,
				 stinfo->dobj.namespace->dobj.name,
				 NULL,
				 stinfo->baseType->rolname, false,
				 "SHELL TYPE", q->data, "", NULL,
				 stinfo->dobj.dependencies, stinfo->dobj.nDeps,
				 NULL, NULL);

	destroyPQExpBuffer(q);
}

/*
 * Determine whether we want to dump definitions for procedural languages.
 * Since the languages themselves don't have schemas, we can't rely on
 * the normal schema-based selection mechanism.  We choose to dump them
 * whenever neither --schema nor --table was given.  (Before 8.1, we used
 * the dump flag of the PL's call handler function, but in 8.1 this will
 * probably always be false since call handlers are created in pg_catalog.)
 *
 * For some backwards compatibility with the older behavior, we forcibly
 * dump a PL if its handler function (and validator if any) are in a
 * dumpable namespace.	That case is not checked here.
 */
static bool
shouldDumpProcLangs(void)
{
	if (!include_everything)
		return false;
	/* And they're schema not data */
	if (dataOnly)
		return false;
	return true;
}

/*
 * dumpProcLang
 *		  writes out to fout the queries to recreate a user-defined
 *		  procedural language
 */
static void
dumpProcLang(Archive *fout, ProcLangInfo *plang)
{
	PQExpBuffer defqry;
	PQExpBuffer delqry;
	bool		useParams;
	char	   *qlanname;
	char	   *lanschema;
	FuncInfo   *funcInfo;
	FuncInfo   *validatorInfo = NULL;

	if (dataOnly)
		return;

	/*
	 * Try to find the support function(s).  It is not an error if we don't
	 * find them --- if the functions are in the pg_catalog schema, as is
	 * standard in 8.1 and up, then we won't have loaded them. (In this case
	 * we will emit a parameterless CREATE LANGUAGE command, which will
	 * require PL template knowledge in the backend to reload.)
	 */

	funcInfo = findFuncByOid(plang->lanplcallfoid);
	if (funcInfo != NULL && !funcInfo->dobj.dump)
		funcInfo = NULL;		/* treat not-dumped same as not-found */

	if (OidIsValid(plang->lanvalidator))
	{
		validatorInfo = findFuncByOid(plang->lanvalidator);
		if (validatorInfo != NULL && !validatorInfo->dobj.dump)
			validatorInfo = NULL;
	}

	/*
	 * If the functions are dumpable then emit a traditional CREATE LANGUAGE
	 * with parameters.  Otherwise, dump only if shouldDumpProcLangs() says to
	 * dump it.
	 */
	useParams = (funcInfo != NULL &&
				 (validatorInfo != NULL || !OidIsValid(plang->lanvalidator)));

	if (!useParams && !shouldDumpProcLangs())
		return;

	defqry = createPQExpBuffer();
	delqry = createPQExpBuffer();

	qlanname = strdup(fmtId(plang->dobj.name));

	/*
	 * If dumping a HANDLER clause, treat the language as being in the handler
	 * function's schema; this avoids cluttering the HANDLER clause. Otherwise
	 * it doesn't really have a schema.
	 */
	if (useParams)
		lanschema = funcInfo->dobj.namespace->dobj.name;
	else
		lanschema = NULL;

	appendPQExpBuffer(delqry, "DROP PROCEDURAL LANGUAGE %s;\n",
					  qlanname);

	appendPQExpBuffer(defqry, "CREATE %sPROCEDURAL LANGUAGE %s",
					  (useParams && plang->lanpltrusted) ? "TRUSTED " : "",
					  qlanname);
	if (useParams)
	{
		appendPQExpBuffer(defqry, " HANDLER %s",
						  fmtId(funcInfo->dobj.name));
		if (OidIsValid(plang->lanvalidator))
		{
			appendPQExpBuffer(defqry, " VALIDATOR ");
			/* Cope with possibility that validator is in different schema */
			if (validatorInfo->dobj.namespace != funcInfo->dobj.namespace)
				appendPQExpBuffer(defqry, "%s.",
							fmtId(validatorInfo->dobj.namespace->dobj.name));
			appendPQExpBuffer(defqry, "%s",
							  fmtId(validatorInfo->dobj.name));
		}
	}
	appendPQExpBuffer(defqry, ";\n");

	ArchiveEntry(fout, plang->dobj.catId, plang->dobj.dumpId,
				 plang->dobj.name,
				 lanschema, NULL, plang->lanowner,
				 false, "PROCEDURAL LANGUAGE",
				 defqry->data, delqry->data, NULL,
				 plang->dobj.dependencies, plang->dobj.nDeps,
				 NULL, NULL);

	/* Dump Proc Lang Comments */
	resetPQExpBuffer(defqry);
	appendPQExpBuffer(defqry, "LANGUAGE %s", qlanname);
	dumpComment(fout, defqry->data,
				NULL, "",
				plang->dobj.catId, 0, plang->dobj.dumpId);

	if (plang->lanpltrusted)
		dumpACL(fout, plang->dobj.catId, plang->dobj.dumpId, "LANGUAGE",
				qlanname, plang->dobj.name,
				lanschema,
				plang->lanowner, plang->lanacl);

	free(qlanname);

	destroyPQExpBuffer(defqry);
	destroyPQExpBuffer(delqry);
}

/*
 * format_function_arguments: generate function name and argument list
 *
 * The argument type names are qualified if needed.  The function name
 * is never qualified.
 *
 * Any or all of allargtypes, argmodes, argnames may be NULL.
 */
static char *
format_function_arguments(FuncInfo *finfo, int nallargs,
						  char **allargtypes,
						  char **argmodes,
						  char **argnames)
{
	PQExpBufferData fn;
	int			j;

	initPQExpBuffer(&fn);
	appendPQExpBuffer(&fn, "%s(", fmtId(finfo->dobj.name));
	for (j = 0; j < nallargs; j++)
	{
		Oid			typid;
		char	   *typname;
		const char *argmode;
		const char *argname;

		typid = allargtypes ? atooid(allargtypes[j]) : finfo->argtypes[j];
		typname = getFormattedTypeName(typid, zeroAsOpaque);

		if (argmodes)
		{
			switch (argmodes[j][0])
			{
				case 'i':
					argmode = "";
					break;
				case 'o':
					argmode = "OUT ";
					break;
				case 'b':
					argmode = "INOUT ";
					break;
				default:
					write_msg(NULL, "WARNING: bogus value in proargmodes array\n");
					argmode = "";
					break;
			}
		}
		else
			argmode = "";

		argname = argnames ? argnames[j] : (char *) NULL;
		if (argname && argname[0] == '\0')
			argname = NULL;

		appendPQExpBuffer(&fn, "%s%s%s%s%s",
						  (j > 0) ? ", " : "",
						  argmode,
						  argname ? fmtId(argname) : "",
						  argname ? " " : "",
						  typname);
		free(typname);
	}
	appendPQExpBuffer(&fn, ")");
	return fn.data;
}

/*
 * format_function_signature: generate function name and argument list
 *
 * This is like format_function_arguments except that only a minimal
 * list of input argument types is generated; this is sufficient to
 * reference the function, but not to define it.
 *
 * If honor_quotes is false then the function name is never quoted.
 * This is appropriate for use in TOC tags, but not in SQL commands.
 */
static char *
format_function_signature(FuncInfo *finfo, bool honor_quotes)
{
	PQExpBufferData fn;
	int			j;

	initPQExpBuffer(&fn);
	if (honor_quotes)
		appendPQExpBuffer(&fn, "%s(", fmtId(finfo->dobj.name));
	else
		appendPQExpBuffer(&fn, "%s(", finfo->dobj.name);
	for (j = 0; j < finfo->nargs; j++)
	{
		char	   *typname;

		typname = getFormattedTypeName(finfo->argtypes[j], zeroAsOpaque);

		appendPQExpBuffer(&fn, "%s%s",
						  (j > 0) ? ", " : "",
						  typname);
		free(typname);
	}
	appendPQExpBuffer(&fn, ")");
	return fn.data;
}


/*
 * dumpFunc:
 *	  dump out one function
 */
static void
dumpFunc(Archive *fout, FuncInfo *finfo)
{
	PQExpBuffer query;
	PQExpBuffer q;
	PQExpBuffer delqry;
	PQExpBuffer asPart;
	PGresult   *res;
	char	   *funcsig;
	char	   *funcsig_tag;
	int			ntups;
	char	   *proretset;
	char	   *prosrc;
	char	   *probin;
	char	   *proallargtypes;
	char	   *proargmodes;
	char	   *proargnames;
	char	   *provolatile;
	char	   *proisstrict;
	char	   *prosecdef;
	char	   *lanname;
	char	   *rettypename;
	int			nallargs;
	char	  **allargtypes = NULL;
	char	  **argmodes = NULL;
	char	  **argnames = NULL;

	/* Skip if not to be dumped */
	if (!finfo->dobj.dump || dataOnly)
		return;

	query = createPQExpBuffer();
	q = createPQExpBuffer();
	delqry = createPQExpBuffer();
	asPart = createPQExpBuffer();

	/* Set proper schema search path so type references list correctly */
	selectSourceSchema(finfo->dobj.namespace->dobj.name);

	/* Fetch function-specific details */
	if (g_fout->remoteVersion >= 80100)
	{
		appendPQExpBuffer(query,
						  "SELECT proretset, prosrc, probin, "
						  "proallargtypes, proargmodes, proargnames, "
						  "provolatile, proisstrict, prosecdef, "
						  "(SELECT lanname FROM pg_catalog.pg_language WHERE oid = prolang) as lanname "
						  "FROM pg_catalog.pg_proc "
						  "WHERE oid = '%u'::pg_catalog.oid",
						  finfo->dobj.catId.oid);
	}
	else if (g_fout->remoteVersion >= 80000)
	{
		appendPQExpBuffer(query,
						  "SELECT proretset, prosrc, probin, "
						  "null as proallargtypes, "
						  "null as proargmodes, "
						  "proargnames, "
						  "provolatile, proisstrict, prosecdef, "
						  "(SELECT lanname FROM pg_catalog.pg_language WHERE oid = prolang) as lanname "
						  "FROM pg_catalog.pg_proc "
						  "WHERE oid = '%u'::pg_catalog.oid",
						  finfo->dobj.catId.oid);
	}
	else if (g_fout->remoteVersion >= 70300)
	{
		appendPQExpBuffer(query,
						  "SELECT proretset, prosrc, probin, "
						  "null as proallargtypes, "
						  "null as proargmodes, "
						  "null as proargnames, "
						  "provolatile, proisstrict, prosecdef, "
						  "(SELECT lanname FROM pg_catalog.pg_language WHERE oid = prolang) as lanname "
						  "FROM pg_catalog.pg_proc "
						  "WHERE oid = '%u'::pg_catalog.oid",
						  finfo->dobj.catId.oid);
	}
	else if (g_fout->remoteVersion >= 70100)
	{
		appendPQExpBuffer(query,
						  "SELECT proretset, prosrc, probin, "
						  "null as proallargtypes, "
						  "null as proargmodes, "
						  "null as proargnames, "
			 "case when proiscachable then 'i' else 'v' end as provolatile, "
						  "proisstrict, "
						  "'f'::boolean as prosecdef, "
		  "(SELECT lanname FROM pg_language WHERE oid = prolang) as lanname "
						  "FROM pg_proc "
						  "WHERE oid = '%u'::oid",
						  finfo->dobj.catId.oid);
	}
	else
	{
		appendPQExpBuffer(query,
						  "SELECT proretset, prosrc, probin, "
						  "null as proallargtypes, "
						  "null as proargmodes, "
						  "null as proargnames, "
			 "case when proiscachable then 'i' else 'v' end as provolatile, "
						  "'f'::boolean as proisstrict, "
						  "'f'::boolean as prosecdef, "
		  "(SELECT lanname FROM pg_language WHERE oid = prolang) as lanname "
						  "FROM pg_proc "
						  "WHERE oid = '%u'::oid",
						  finfo->dobj.catId.oid);
	}

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

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
	proallargtypes = PQgetvalue(res, 0, PQfnumber(res, "proallargtypes"));
	proargmodes = PQgetvalue(res, 0, PQfnumber(res, "proargmodes"));
	proargnames = PQgetvalue(res, 0, PQfnumber(res, "proargnames"));
	provolatile = PQgetvalue(res, 0, PQfnumber(res, "provolatile"));
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
		appendStringLiteralAH(asPart, probin, fout);
		if (strcmp(prosrc, "-") != 0)
		{
			appendPQExpBuffer(asPart, ", ");

			/*
			 * where we have bin, use dollar quoting if allowed and src
			 * contains quote or backslash; else use regular quoting.
			 */
			if (disable_dollar_quoting ||
			  (strchr(prosrc, '\'') == NULL && strchr(prosrc, '\\') == NULL))
				appendStringLiteralAH(asPart, prosrc, fout);
			else
				appendStringLiteralDQ(asPart, prosrc, NULL);
		}
	}
	else
	{
		if (strcmp(prosrc, "-") != 0)
		{
			appendPQExpBuffer(asPart, "AS ");
			/* with no bin, dollar quote src unconditionally if allowed */
			if (disable_dollar_quoting)
				appendStringLiteralAH(asPart, prosrc, fout);
			else
				appendStringLiteralDQ(asPart, prosrc, NULL);
		}
	}

	nallargs = finfo->nargs;	/* unless we learn different from allargs */

	if (proallargtypes && *proallargtypes)
	{
		int			nitems = 0;

		if (!parsePGArray(proallargtypes, &allargtypes, &nitems) ||
			nitems < finfo->nargs)
		{
			write_msg(NULL, "WARNING: could not parse proallargtypes array\n");
			if (allargtypes)
				free(allargtypes);
			allargtypes = NULL;
		}
		else
			nallargs = nitems;
	}

	if (proargmodes && *proargmodes)
	{
		int			nitems = 0;

		if (!parsePGArray(proargmodes, &argmodes, &nitems) ||
			nitems != nallargs)
		{
			write_msg(NULL, "WARNING: could not parse proargmodes array\n");
			if (argmodes)
				free(argmodes);
			argmodes = NULL;
		}
	}

	if (proargnames && *proargnames)
	{
		int			nitems = 0;

		if (!parsePGArray(proargnames, &argnames, &nitems) ||
			nitems != nallargs)
		{
			write_msg(NULL, "WARNING: could not parse proargnames array\n");
			if (argnames)
				free(argnames);
			argnames = NULL;
		}
	}

	funcsig = format_function_arguments(finfo, nallargs, allargtypes,
										argmodes, argnames);
	funcsig_tag = format_function_signature(finfo, false);

	/*
	 * DROP must be fully qualified in case same name appears in pg_catalog
	 */
	appendPQExpBuffer(delqry, "DROP FUNCTION %s.%s;\n",
					  fmtId(finfo->dobj.namespace->dobj.name),
					  funcsig);

	rettypename = getFormattedTypeName(finfo->prorettype, zeroAsOpaque);

	appendPQExpBuffer(q, "CREATE FUNCTION %s ", funcsig);
	appendPQExpBuffer(q, "RETURNS %s%s\n    %s\n    LANGUAGE %s",
					  (proretset[0] == 't') ? "SETOF " : "",
					  rettypename,
					  asPart->data,
					  fmtId(lanname));

	free(rettypename);

	if (provolatile[0] != PROVOLATILE_VOLATILE)
	{
		if (provolatile[0] == PROVOLATILE_IMMUTABLE)
			appendPQExpBuffer(q, " IMMUTABLE");
		else if (provolatile[0] == PROVOLATILE_STABLE)
			appendPQExpBuffer(q, " STABLE");
		else if (provolatile[0] != PROVOLATILE_VOLATILE)
		{
			write_msg(NULL, "unrecognized provolatile value for function \"%s\"\n",
					  finfo->dobj.name);
			exit_nicely();
		}
	}

	if (proisstrict[0] == 't')
		appendPQExpBuffer(q, " STRICT");

	if (prosecdef[0] == 't')
		appendPQExpBuffer(q, " SECURITY DEFINER");

	appendPQExpBuffer(q, ";\n");

	ArchiveEntry(fout, finfo->dobj.catId, finfo->dobj.dumpId,
				 funcsig_tag,
				 finfo->dobj.namespace->dobj.name,
				 NULL,
				 finfo->rolname, false,
				 "FUNCTION", q->data, delqry->data, NULL,
				 finfo->dobj.dependencies, finfo->dobj.nDeps,
				 NULL, NULL);

	/* Dump Function Comments */
	resetPQExpBuffer(q);
	appendPQExpBuffer(q, "FUNCTION %s", funcsig);
	dumpComment(fout, q->data,
				finfo->dobj.namespace->dobj.name, finfo->rolname,
				finfo->dobj.catId, 0, finfo->dobj.dumpId);

	dumpACL(fout, finfo->dobj.catId, finfo->dobj.dumpId, "FUNCTION",
			funcsig, funcsig_tag,
			finfo->dobj.namespace->dobj.name,
			finfo->rolname, finfo->proacl);

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delqry);
	destroyPQExpBuffer(asPart);
	free(funcsig);
	free(funcsig_tag);
	if (allargtypes)
		free(allargtypes);
	if (argmodes)
		free(argmodes);
	if (argnames)
		free(argnames);
}


/*
 * Dump a user-defined cast
 */
static void
dumpCast(Archive *fout, CastInfo *cast)
{
	PQExpBuffer defqry;
	PQExpBuffer delqry;
	PQExpBuffer castsig;
	FuncInfo   *funcInfo = NULL;
	TypeInfo   *sourceInfo;
	TypeInfo   *targetInfo;

	if (dataOnly)
		return;

	if (OidIsValid(cast->castfunc))
	{
		funcInfo = findFuncByOid(cast->castfunc);
		if (funcInfo == NULL)
			return;
	}

	/*
	 * As per discussion we dump casts if one or more of the underlying
	 * objects (the conversion function and the two data types) are not
	 * builtin AND if all of the non-builtin objects are included in the dump.
	 * Builtin meaning, the namespace name does not start with "pg_".
	 */
	sourceInfo = findTypeByOid(cast->castsource);
	targetInfo = findTypeByOid(cast->casttarget);

	if (sourceInfo == NULL || targetInfo == NULL)
		return;

	/*
	 * Skip this cast if all objects are from pg_
	 */
	if ((funcInfo == NULL ||
		 strncmp(funcInfo->dobj.namespace->dobj.name, "pg_", 3) == 0) &&
		strncmp(sourceInfo->dobj.namespace->dobj.name, "pg_", 3) == 0 &&
		strncmp(targetInfo->dobj.namespace->dobj.name, "pg_", 3) == 0)
		return;

	/*
	 * Skip cast if function isn't from pg_ and is not to be dumped.
	 */
	if (funcInfo &&
		strncmp(funcInfo->dobj.namespace->dobj.name, "pg_", 3) != 0 &&
		!funcInfo->dobj.dump)
		return;

	/*
	 * Same for the source type
	 */
	if (strncmp(sourceInfo->dobj.namespace->dobj.name, "pg_", 3) != 0 &&
		!sourceInfo->dobj.dump)
		return;

	/*
	 * and the target type.
	 */
	if (strncmp(targetInfo->dobj.namespace->dobj.name, "pg_", 3) != 0 &&
		!targetInfo->dobj.dump)
		return;

	/* Make sure we are in proper schema (needed for getFormattedTypeName) */
	selectSourceSchema("pg_catalog");

	defqry = createPQExpBuffer();
	delqry = createPQExpBuffer();
	castsig = createPQExpBuffer();

	appendPQExpBuffer(delqry, "DROP CAST (%s AS %s);\n",
					  getFormattedTypeName(cast->castsource, zeroAsNone),
					  getFormattedTypeName(cast->casttarget, zeroAsNone));

	appendPQExpBuffer(defqry, "CREATE CAST (%s AS %s) ",
					  getFormattedTypeName(cast->castsource, zeroAsNone),
					  getFormattedTypeName(cast->casttarget, zeroAsNone));

	if (!OidIsValid(cast->castfunc))
		appendPQExpBuffer(defqry, "WITHOUT FUNCTION");
	else
	{
		/*
		 * Always qualify the function name, in case it is not in pg_catalog
		 * schema (format_function_signature won't qualify it).
		 */
		appendPQExpBuffer(defqry, "WITH FUNCTION %s.",
						  fmtId(funcInfo->dobj.namespace->dobj.name));
		appendPQExpBuffer(defqry, "%s",
						  format_function_signature(funcInfo, true));
	}

	if (cast->castcontext == 'a')
		appendPQExpBuffer(defqry, " AS ASSIGNMENT");
	else if (cast->castcontext == 'i')
		appendPQExpBuffer(defqry, " AS IMPLICIT");
	appendPQExpBuffer(defqry, ";\n");

	appendPQExpBuffer(castsig, "CAST (%s AS %s)",
					  getFormattedTypeName(cast->castsource, zeroAsNone),
					  getFormattedTypeName(cast->casttarget, zeroAsNone));

	ArchiveEntry(fout, cast->dobj.catId, cast->dobj.dumpId,
				 castsig->data,
				 "pg_catalog", NULL, "",
				 false, "CAST", defqry->data, delqry->data, NULL,
				 cast->dobj.dependencies, cast->dobj.nDeps,
				 NULL, NULL);

	/* Dump Cast Comments */
	resetPQExpBuffer(defqry);
	appendPQExpBuffer(defqry, "CAST (%s AS %s)",
					  getFormattedTypeName(cast->castsource, zeroAsNone),
					  getFormattedTypeName(cast->casttarget, zeroAsNone));
	dumpComment(fout, defqry->data,
				NULL, "",
				cast->dobj.catId, 0, cast->dobj.dumpId);

	destroyPQExpBuffer(defqry);
	destroyPQExpBuffer(delqry);
	destroyPQExpBuffer(castsig);
}

/*
 * dumpOpr
 *	  write out a single operator definition
 */
static void
dumpOpr(Archive *fout, OprInfo *oprinfo)
{
	PQExpBuffer query;
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer oprid;
	PQExpBuffer details;
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

	/* Skip if not to be dumped */
	if (!oprinfo->dobj.dump || dataOnly)
		return;

	/*
	 * some operators are invalid because they were the result of user
	 * defining operators before commutators exist
	 */
	if (!OidIsValid(oprinfo->oprcode))
		return;

	query = createPQExpBuffer();
	q = createPQExpBuffer();
	delq = createPQExpBuffer();
	oprid = createPQExpBuffer();
	details = createPQExpBuffer();

	/* Make sure we are in proper schema so regoperator works correctly */
	selectSourceSchema(oprinfo->dobj.namespace->dobj.name);

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
						  "where oid = '%u'::pg_catalog.oid",
						  oprinfo->dobj.catId.oid);
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
						  "where oid = '%u'::oid",
						  oprinfo->dobj.catId.oid);
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
						  "where oid = '%u'::oid",
						  oprinfo->dobj.catId.oid);
	}

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

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

	appendPQExpBuffer(details, "    PROCEDURE = %s",
					  convertRegProcReference(oprcode));

	appendPQExpBuffer(oprid, "%s (",
					  oprinfo->dobj.name);

	/*
	 * right unary means there's a left arg and left unary means there's a
	 * right arg
	 */
	if (strcmp(oprkind, "r") == 0 ||
		strcmp(oprkind, "b") == 0)
	{
		if (g_fout->remoteVersion >= 70100)
			name = oprleft;
		else
			name = fmtId(oprleft);
		appendPQExpBuffer(details, ",\n    LEFTARG = %s", name);
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
			name = fmtId(oprright);
		appendPQExpBuffer(details, ",\n    RIGHTARG = %s", name);
		appendPQExpBuffer(oprid, ", %s)", name);
	}
	else
		appendPQExpBuffer(oprid, ", NONE)");

	name = convertOperatorReference(oprcom);
	if (name)
		appendPQExpBuffer(details, ",\n    COMMUTATOR = %s", name);

	name = convertOperatorReference(oprnegate);
	if (name)
		appendPQExpBuffer(details, ",\n    NEGATOR = %s", name);

	if (strcmp(oprcanhash, "t") == 0)
		appendPQExpBuffer(details, ",\n    HASHES");

	name = convertRegProcReference(oprrest);
	if (name)
		appendPQExpBuffer(details, ",\n    RESTRICT = %s", name);

	name = convertRegProcReference(oprjoin);
	if (name)
		appendPQExpBuffer(details, ",\n    JOIN = %s", name);

	name = convertOperatorReference(oprlsortop);
	if (name)
		appendPQExpBuffer(details, ",\n    SORT1 = %s", name);

	name = convertOperatorReference(oprrsortop);
	if (name)
		appendPQExpBuffer(details, ",\n    SORT2 = %s", name);

	name = convertOperatorReference(oprltcmpop);
	if (name)
		appendPQExpBuffer(details, ",\n    LTCMP = %s", name);

	name = convertOperatorReference(oprgtcmpop);
	if (name)
		appendPQExpBuffer(details, ",\n    GTCMP = %s", name);

	/*
	 * DROP must be fully qualified in case same name appears in pg_catalog
	 */
	appendPQExpBuffer(delq, "DROP OPERATOR %s.%s;\n",
					  fmtId(oprinfo->dobj.namespace->dobj.name),
					  oprid->data);

	appendPQExpBuffer(q, "CREATE OPERATOR %s (\n%s\n);\n",
					  oprinfo->dobj.name, details->data);

	ArchiveEntry(fout, oprinfo->dobj.catId, oprinfo->dobj.dumpId,
				 oprinfo->dobj.name,
				 oprinfo->dobj.namespace->dobj.name,
				 NULL,
				 oprinfo->rolname,
				 false, "OPERATOR", q->data, delq->data, NULL,
				 oprinfo->dobj.dependencies, oprinfo->dobj.nDeps,
				 NULL, NULL);

	/* Dump Operator Comments */
	resetPQExpBuffer(q);
	appendPQExpBuffer(q, "OPERATOR %s", oprid->data);
	dumpComment(fout, q->data,
				oprinfo->dobj.namespace->dobj.name, oprinfo->rolname,
				oprinfo->dobj.catId, 0, oprinfo->dobj.dumpId);

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
		char	   *name;
		char	   *paren;
		bool		inquote;

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
	return fmtId(proc);
}

/*
 * Convert an operator cross-reference obtained from pg_operator
 *
 * Returns what to print, or NULL to print nothing
 *
 * In 7.3 and up the input is a REGOPERATOR display; we have to strip the
 * argument-types part, and add OPERATOR() decoration if the name is
 * schema-qualified.  In older versions, the input is just a numeric OID,
 * which we search our operator list for.
 */
static const char *
convertOperatorReference(const char *opr)
{
	OprInfo    *oprInfo;

	/* In all cases "0" means a null reference */
	if (strcmp(opr, "0") == 0)
		return NULL;

	if (g_fout->remoteVersion >= 70300)
	{
		char	   *name;
		char	   *oname;
		char	   *ptr;
		bool		inquote;
		bool		sawdot;

		name = strdup(opr);
		/* find non-double-quoted left paren, and check for non-quoted dot */
		inquote = false;
		sawdot = false;
		for (ptr = name; *ptr; ptr++)
		{
			if (*ptr == '"')
				inquote = !inquote;
			else if (*ptr == '.' && !inquote)
				sawdot = true;
			else if (*ptr == '(' && !inquote)
			{
				*ptr = '\0';
				break;
			}
		}
		/* If not schema-qualified, don't need to add OPERATOR() */
		if (!sawdot)
			return name;
		oname = malloc(strlen(name) + 11);
		sprintf(oname, "OPERATOR(%s)", name);
		free(name);
		return oname;
	}

	oprInfo = findOprByOid(atooid(opr));
	if (oprInfo == NULL)
	{
		write_msg(NULL, "WARNING: could not find operator with OID %s\n",
				  opr);
		return NULL;
	}
	return oprInfo->dobj.name;
}

/*
 * dumpOpclass
 *	  write out a single operator class definition
 */
static void
dumpOpclass(Archive *fout, OpclassInfo *opcinfo)
{
	PQExpBuffer query;
	PQExpBuffer q;
	PQExpBuffer delq;
	PGresult   *res;
	int			ntups;
	int			i_opcintype;
	int			i_opckeytype;
	int			i_opcdefault;
	int			i_amname;
	int			i_amopstrategy;
	int			i_amopreqcheck;
	int			i_amopopr;
	int			i_amprocnum;
	int			i_amproc;
	char	   *opcintype;
	char	   *opckeytype;
	char	   *opcdefault;
	char	   *amname;
	char	   *amopstrategy;
	char	   *amopreqcheck;
	char	   *amopopr;
	char	   *amprocnum;
	char	   *amproc;
	bool		needComma;
	int			i;

	/* Skip if not to be dumped */
	if (!opcinfo->dobj.dump || dataOnly)
		return;

	/*
	 * XXX currently we do not implement dumping of operator classes from
	 * pre-7.3 databases.  This could be done but it seems not worth the
	 * trouble.
	 */
	if (g_fout->remoteVersion < 70300)
		return;

	query = createPQExpBuffer();
	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	/* Make sure we are in proper schema so regoperator works correctly */
	selectSourceSchema(opcinfo->dobj.namespace->dobj.name);

	/* Get additional fields from the pg_opclass row */
	appendPQExpBuffer(query, "SELECT opcintype::pg_catalog.regtype, "
					  "opckeytype::pg_catalog.regtype, "
					  "opcdefault, "
	   "(SELECT amname FROM pg_catalog.pg_am WHERE oid = opcamid) AS amname "
					  "FROM pg_catalog.pg_opclass "
					  "WHERE oid = '%u'::pg_catalog.oid",
					  opcinfo->dobj.catId.oid);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	/* Expecting a single result only */
	ntups = PQntuples(res);
	if (ntups != 1)
	{
		write_msg(NULL, "Got %d rows instead of one from: %s",
				  ntups, query->data);
		exit_nicely();
	}

	i_opcintype = PQfnumber(res, "opcintype");
	i_opckeytype = PQfnumber(res, "opckeytype");
	i_opcdefault = PQfnumber(res, "opcdefault");
	i_amname = PQfnumber(res, "amname");

	opcintype = PQgetvalue(res, 0, i_opcintype);
	opckeytype = PQgetvalue(res, 0, i_opckeytype);
	opcdefault = PQgetvalue(res, 0, i_opcdefault);
	/* amname will still be needed after we PQclear res */
	amname = strdup(PQgetvalue(res, 0, i_amname));

	/*
	 * DROP must be fully qualified in case same name appears in pg_catalog
	 */
	appendPQExpBuffer(delq, "DROP OPERATOR CLASS %s",
					  fmtId(opcinfo->dobj.namespace->dobj.name));
	appendPQExpBuffer(delq, ".%s",
					  fmtId(opcinfo->dobj.name));
	appendPQExpBuffer(delq, " USING %s;\n",
					  fmtId(amname));

	/* Build the fixed portion of the CREATE command */
	appendPQExpBuffer(q, "CREATE OPERATOR CLASS %s\n    ",
					  fmtId(opcinfo->dobj.name));
	if (strcmp(opcdefault, "t") == 0)
		appendPQExpBuffer(q, "DEFAULT ");
	appendPQExpBuffer(q, "FOR TYPE %s USING %s AS\n    ",
					  opcintype,
					  fmtId(amname));

	needComma = false;

	if (strcmp(opckeytype, "-") != 0)
	{
		appendPQExpBuffer(q, "STORAGE %s",
						  opckeytype);
		needComma = true;
	}

	PQclear(res);

	/*
	 * Now fetch and print the OPERATOR entries (pg_amop rows).
	 */
	resetPQExpBuffer(query);

	appendPQExpBuffer(query, "SELECT amopstrategy, amopreqcheck, "
					  "amopopr::pg_catalog.regoperator "
					  "FROM pg_catalog.pg_amop "
					  "WHERE amopclaid = '%u'::pg_catalog.oid "
					  "ORDER BY amopstrategy",
					  opcinfo->dobj.catId.oid);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_amopstrategy = PQfnumber(res, "amopstrategy");
	i_amopreqcheck = PQfnumber(res, "amopreqcheck");
	i_amopopr = PQfnumber(res, "amopopr");

	for (i = 0; i < ntups; i++)
	{
		amopstrategy = PQgetvalue(res, i, i_amopstrategy);
		amopreqcheck = PQgetvalue(res, i, i_amopreqcheck);
		amopopr = PQgetvalue(res, i, i_amopopr);

		if (needComma)
			appendPQExpBuffer(q, " ,\n    ");

		appendPQExpBuffer(q, "OPERATOR %s %s",
						  amopstrategy, amopopr);
		if (strcmp(amopreqcheck, "t") == 0)
			appendPQExpBuffer(q, " RECHECK");

		needComma = true;
	}

	PQclear(res);

	/*
	 * Now fetch and print the FUNCTION entries (pg_amproc rows).
	 */
	resetPQExpBuffer(query);

	appendPQExpBuffer(query, "SELECT amprocnum, "
					  "amproc::pg_catalog.regprocedure "
					  "FROM pg_catalog.pg_amproc "
					  "WHERE amopclaid = '%u'::pg_catalog.oid "
					  "ORDER BY amprocnum",
					  opcinfo->dobj.catId.oid);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_amprocnum = PQfnumber(res, "amprocnum");
	i_amproc = PQfnumber(res, "amproc");

	for (i = 0; i < ntups; i++)
	{
		amprocnum = PQgetvalue(res, i, i_amprocnum);
		amproc = PQgetvalue(res, i, i_amproc);

		if (needComma)
			appendPQExpBuffer(q, " ,\n    ");

		appendPQExpBuffer(q, "FUNCTION %s %s",
						  amprocnum, amproc);

		needComma = true;
	}

	PQclear(res);

	appendPQExpBuffer(q, ";\n");

	ArchiveEntry(fout, opcinfo->dobj.catId, opcinfo->dobj.dumpId,
				 opcinfo->dobj.name,
				 opcinfo->dobj.namespace->dobj.name,
				 NULL,
				 opcinfo->rolname,
				 false, "OPERATOR CLASS", q->data, delq->data, NULL,
				 opcinfo->dobj.dependencies, opcinfo->dobj.nDeps,
				 NULL, NULL);

	/* Dump Operator Class Comments */
	resetPQExpBuffer(q);
	appendPQExpBuffer(q, "OPERATOR CLASS %s",
					  fmtId(opcinfo->dobj.name));
	appendPQExpBuffer(q, " USING %s",
					  fmtId(amname));
	dumpComment(fout, q->data,
				NULL, opcinfo->rolname,
				opcinfo->dobj.catId, 0, opcinfo->dobj.dumpId);

	free(amname);
	destroyPQExpBuffer(query);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
}

/*
 * dumpConversion
 *	  write out a single conversion definition
 */
static void
dumpConversion(Archive *fout, ConvInfo *convinfo)
{
	PQExpBuffer query;
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer details;
	PGresult   *res;
	int			ntups;
	int			i_conname;
	int			i_conforencoding;
	int			i_contoencoding;
	int			i_conproc;
	int			i_condefault;
	const char *conname;
	const char *conforencoding;
	const char *contoencoding;
	const char *conproc;
	bool		condefault;

	/* Skip if not to be dumped */
	if (!convinfo->dobj.dump || dataOnly)
		return;

	query = createPQExpBuffer();
	q = createPQExpBuffer();
	delq = createPQExpBuffer();
	details = createPQExpBuffer();

	/* Make sure we are in proper schema */
	selectSourceSchema(convinfo->dobj.namespace->dobj.name);

	/* Get conversion-specific details */
	appendPQExpBuffer(query, "SELECT conname, "
		 "pg_catalog.pg_encoding_to_char(conforencoding) AS conforencoding, "
		   "pg_catalog.pg_encoding_to_char(contoencoding) AS contoencoding, "
					  "conproc, condefault "
					  "FROM pg_catalog.pg_conversion c "
					  "WHERE c.oid = '%u'::pg_catalog.oid",
					  convinfo->dobj.catId.oid);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	/* Expecting a single result only */
	ntups = PQntuples(res);
	if (ntups != 1)
	{
		write_msg(NULL, "Got %d rows instead of one from: %s",
				  ntups, query->data);
		exit_nicely();
	}

	i_conname = PQfnumber(res, "conname");
	i_conforencoding = PQfnumber(res, "conforencoding");
	i_contoencoding = PQfnumber(res, "contoencoding");
	i_conproc = PQfnumber(res, "conproc");
	i_condefault = PQfnumber(res, "condefault");

	conname = PQgetvalue(res, 0, i_conname);
	conforencoding = PQgetvalue(res, 0, i_conforencoding);
	contoencoding = PQgetvalue(res, 0, i_contoencoding);
	conproc = PQgetvalue(res, 0, i_conproc);
	condefault = (PQgetvalue(res, 0, i_condefault)[0] == 't');

	/*
	 * DROP must be fully qualified in case same name appears in pg_catalog
	 */
	appendPQExpBuffer(delq, "DROP CONVERSION %s",
					  fmtId(convinfo->dobj.namespace->dobj.name));
	appendPQExpBuffer(delq, ".%s;\n",
					  fmtId(convinfo->dobj.name));

	appendPQExpBuffer(q, "CREATE %sCONVERSION %s FOR ",
					  (condefault) ? "DEFAULT " : "",
					  fmtId(convinfo->dobj.name));
	appendStringLiteralAH(q, conforencoding, fout);
	appendPQExpBuffer(q, " TO ");
	appendStringLiteralAH(q, contoencoding, fout);
	/* regproc is automatically quoted in 7.3 and above */
	appendPQExpBuffer(q, " FROM %s;\n", conproc);

	ArchiveEntry(fout, convinfo->dobj.catId, convinfo->dobj.dumpId,
				 convinfo->dobj.name,
				 convinfo->dobj.namespace->dobj.name,
				 NULL,
				 convinfo->rolname,
				 false, "CONVERSION", q->data, delq->data, NULL,
				 convinfo->dobj.dependencies, convinfo->dobj.nDeps,
				 NULL, NULL);

	/* Dump Conversion Comments */
	resetPQExpBuffer(q);
	appendPQExpBuffer(q, "CONVERSION %s", fmtId(convinfo->dobj.name));
	dumpComment(fout, q->data,
				convinfo->dobj.namespace->dobj.name, convinfo->rolname,
				convinfo->dobj.catId, 0, convinfo->dobj.dumpId);

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(details);
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
	int			j;

	initPQExpBuffer(&buf);
	if (honor_quotes)
		appendPQExpBuffer(&buf, "%s",
						  fmtId(agginfo->aggfn.dobj.name));
	else
		appendPQExpBuffer(&buf, "%s", agginfo->aggfn.dobj.name);

	if (agginfo->aggfn.nargs == 0)
		appendPQExpBuffer(&buf, "(*)");
	else
	{
		appendPQExpBuffer(&buf, "(");
		for (j = 0; j < agginfo->aggfn.nargs; j++)
		{
			char	   *typname;

			typname = getFormattedTypeName(agginfo->aggfn.argtypes[j], zeroAsOpaque);

			appendPQExpBuffer(&buf, "%s%s",
							  (j > 0) ? ", " : "",
							  typname);
			free(typname);
		}
		appendPQExpBuffer(&buf, ")");
	}
	return buf.data;
}

/*
 * dumpAgg
 *	  write out a single aggregate definition
 */
static void
dumpAgg(Archive *fout, AggInfo *agginfo)
{
	PQExpBuffer query;
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer details;
	char	   *aggsig;
	char	   *aggsig_tag;
	PGresult   *res;
	int			ntups;
	int			i_aggtransfn;
	int			i_aggfinalfn;
	int			i_aggsortop;
	int			i_aggtranstype;
	int			i_agginitval;
	int			i_convertok;
	const char *aggtransfn;
	const char *aggfinalfn;
	const char *aggsortop;
	const char *aggtranstype;
	const char *agginitval;
	bool		convertok;

	/* Skip if not to be dumped */
	if (!agginfo->aggfn.dobj.dump || dataOnly)
		return;

	query = createPQExpBuffer();
	q = createPQExpBuffer();
	delq = createPQExpBuffer();
	details = createPQExpBuffer();

	/* Make sure we are in proper schema */
	selectSourceSchema(agginfo->aggfn.dobj.namespace->dobj.name);

	/* Get aggregate-specific details */
	if (g_fout->remoteVersion >= 80100)
	{
		appendPQExpBuffer(query, "SELECT aggtransfn, "
						  "aggfinalfn, aggtranstype::pg_catalog.regtype, "
						  "aggsortop::pg_catalog.regoperator, "
						  "agginitval, "
						  "'t'::boolean as convertok "
					  "from pg_catalog.pg_aggregate a, pg_catalog.pg_proc p "
						  "where a.aggfnoid = p.oid "
						  "and p.oid = '%u'::pg_catalog.oid",
						  agginfo->aggfn.dobj.catId.oid);
	}
	else if (g_fout->remoteVersion >= 70300)
	{
		appendPQExpBuffer(query, "SELECT aggtransfn, "
						  "aggfinalfn, aggtranstype::pg_catalog.regtype, "
						  "0 as aggsortop, "
						  "agginitval, "
						  "'t'::boolean as convertok "
					  "from pg_catalog.pg_aggregate a, pg_catalog.pg_proc p "
						  "where a.aggfnoid = p.oid "
						  "and p.oid = '%u'::pg_catalog.oid",
						  agginfo->aggfn.dobj.catId.oid);
	}
	else if (g_fout->remoteVersion >= 70100)
	{
		appendPQExpBuffer(query, "SELECT aggtransfn, aggfinalfn, "
						  "format_type(aggtranstype, NULL) as aggtranstype, "
						  "0 as aggsortop, "
						  "agginitval, "
						  "'t'::boolean as convertok "
						  "from pg_aggregate "
						  "where oid = '%u'::oid",
						  agginfo->aggfn.dobj.catId.oid);
	}
	else
	{
		appendPQExpBuffer(query, "SELECT aggtransfn1 as aggtransfn, "
						  "aggfinalfn, "
						  "(select typname from pg_type where oid = aggtranstype1) as aggtranstype, "
						  "0 as aggsortop, "
						  "agginitval1 as agginitval, "
						  "(aggtransfn2 = 0 and aggtranstype2 = 0 and agginitval2 is null) as convertok "
						  "from pg_aggregate "
						  "where oid = '%u'::oid",
						  agginfo->aggfn.dobj.catId.oid);
	}

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

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
	i_aggsortop = PQfnumber(res, "aggsortop");
	i_aggtranstype = PQfnumber(res, "aggtranstype");
	i_agginitval = PQfnumber(res, "agginitval");
	i_convertok = PQfnumber(res, "convertok");

	aggtransfn = PQgetvalue(res, 0, i_aggtransfn);
	aggfinalfn = PQgetvalue(res, 0, i_aggfinalfn);
	aggsortop = PQgetvalue(res, 0, i_aggsortop);
	aggtranstype = PQgetvalue(res, 0, i_aggtranstype);
	agginitval = PQgetvalue(res, 0, i_agginitval);
	convertok = (PQgetvalue(res, 0, i_convertok)[0] == 't');

	aggsig = format_aggregate_signature(agginfo, fout, true);
	aggsig_tag = format_aggregate_signature(agginfo, fout, false);

	if (!convertok)
	{
		write_msg(NULL, "WARNING: aggregate function %s could not be dumped correctly for this database version; ignored\n",
				  aggsig);
		return;
	}

	if (g_fout->remoteVersion >= 70300)
	{
		/* If using 7.3's regproc or regtype, data is already quoted */
		appendPQExpBuffer(details, "    SFUNC = %s,\n    STYPE = %s",
						  aggtransfn,
						  aggtranstype);
	}
	else if (g_fout->remoteVersion >= 70100)
	{
		/* format_type quotes, regproc does not */
		appendPQExpBuffer(details, "    SFUNC = %s,\n    STYPE = %s",
						  fmtId(aggtransfn),
						  aggtranstype);
	}
	else
	{
		/* need quotes all around */
		appendPQExpBuffer(details, "    SFUNC = %s,\n",
						  fmtId(aggtransfn));
		appendPQExpBuffer(details, "    STYPE = %s",
						  fmtId(aggtranstype));
	}

	if (!PQgetisnull(res, 0, i_agginitval))
	{
		appendPQExpBuffer(details, ",\n    INITCOND = ");
		appendStringLiteralAH(details, agginitval, fout);
	}

	if (strcmp(aggfinalfn, "-") != 0)
	{
		appendPQExpBuffer(details, ",\n    FINALFUNC = %s",
						  aggfinalfn);
	}

	aggsortop = convertOperatorReference(aggsortop);
	if (aggsortop)
	{
		appendPQExpBuffer(details, ",\n    SORTOP = %s",
						  aggsortop);
	}

	/*
	 * DROP must be fully qualified in case same name appears in pg_catalog
	 */
	appendPQExpBuffer(delq, "DROP AGGREGATE %s.%s;\n",
					  fmtId(agginfo->aggfn.dobj.namespace->dobj.name),
					  aggsig);

	appendPQExpBuffer(q, "CREATE AGGREGATE %s (\n%s\n);\n",
					  aggsig, details->data);

	ArchiveEntry(fout, agginfo->aggfn.dobj.catId, agginfo->aggfn.dobj.dumpId,
				 aggsig_tag,
				 agginfo->aggfn.dobj.namespace->dobj.name,
				 NULL,
				 agginfo->aggfn.rolname,
				 false, "AGGREGATE", q->data, delq->data, NULL,
				 agginfo->aggfn.dobj.dependencies, agginfo->aggfn.dobj.nDeps,
				 NULL, NULL);

	/* Dump Aggregate Comments */
	resetPQExpBuffer(q);
	appendPQExpBuffer(q, "AGGREGATE %s", aggsig);
	dumpComment(fout, q->data,
			agginfo->aggfn.dobj.namespace->dobj.name, agginfo->aggfn.rolname,
				agginfo->aggfn.dobj.catId, 0, agginfo->aggfn.dobj.dumpId);

	/*
	 * Since there is no GRANT ON AGGREGATE syntax, we have to make the ACL
	 * command look like a function's GRANT; in particular this affects the
	 * syntax for zero-argument aggregates.
	 */
	free(aggsig);
	free(aggsig_tag);

	aggsig = format_function_signature(&agginfo->aggfn, true);
	aggsig_tag = format_function_signature(&agginfo->aggfn, false);

	dumpACL(fout, agginfo->aggfn.dobj.catId, agginfo->aggfn.dobj.dumpId,
			"FUNCTION",
			aggsig, aggsig_tag,
			agginfo->aggfn.dobj.namespace->dobj.name,
			agginfo->aggfn.rolname, agginfo->aggfn.proacl);

	free(aggsig);
	free(aggsig_tag);

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(details);
}


/*----------
 * Write out grant/revoke information
 *
 * 'objCatId' is the catalog ID of the underlying object.
 * 'objDumpId' is the dump ID of the underlying object.
 * 'type' must be TABLE, FUNCTION, LANGUAGE, SCHEMA, DATABASE, or TABLESPACE.
 * 'name' is the formatted name of the object.	Must be quoted etc. already.
 * 'tag' is the tag for the archive entry (typ. unquoted name of object).
 * 'nspname' is the namespace the object is in (NULL if none).
 * 'owner' is the owner, NULL if there is no owner (for languages).
 * 'acls' is the string read out of the fooacl system catalog field;
 * it will be parsed here.
 *----------
 */
static void
dumpACL(Archive *fout, CatalogId objCatId, DumpId objDumpId,
		const char *type, const char *name,
		const char *tag, const char *nspname, const char *owner,
		const char *acls)
{
	PQExpBuffer sql;

	/* Do nothing if ACL dump is not enabled */
	if (dataOnly || aclsSkip)
		return;

	sql = createPQExpBuffer();

	if (!buildACLCommands(name, type, acls, owner, fout->remoteVersion, sql))
	{
		write_msg(NULL, "could not parse ACL list (%s) for object \"%s\" (%s)\n",
				  acls, name, type);
		exit_nicely();
	}

	if (sql->len > 0)
		ArchiveEntry(fout, nilCatalogId, createDumpId(),
					 tag, nspname,
					 NULL,
					 owner ? owner : "",
					 false, "ACL", sql->data, "", NULL,
					 &(objDumpId), 1,
					 NULL, NULL);

	destroyPQExpBuffer(sql);
}

/*
 * dumpTable
 *	  write out to fout the declarations (not data) of a user-defined table
 */
static void
dumpTable(Archive *fout, TableInfo *tbinfo)
{
	char	   *namecopy;

	if (tbinfo->dobj.dump)
	{
		if (tbinfo->relkind == RELKIND_SEQUENCE)
			dumpSequence(fout, tbinfo);
		else if (!dataOnly)
			dumpTableSchema(fout, tbinfo);

		/* Handle the ACL here */
		namecopy = strdup(fmtId(tbinfo->dobj.name));
		dumpACL(fout, tbinfo->dobj.catId, tbinfo->dobj.dumpId,
				(tbinfo->relkind == RELKIND_SEQUENCE) ? "SEQUENCE" : "TABLE",
				namecopy, tbinfo->dobj.name,
				tbinfo->dobj.namespace->dobj.name, tbinfo->rolname,
				tbinfo->relacl);
		free(namecopy);
	}
}

/*
 * dumpTableSchema
 *	  write the declaration (not data) of one user-defined table or view
 */
static void
dumpTableSchema(Archive *fout, TableInfo *tbinfo)
{
	PQExpBuffer query = createPQExpBuffer();
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	PGresult   *res;
	int			numParents;
	TableInfo **parents;
	int			actual_atts;	/* number of attrs in this CREATE statment */
	char	   *reltypename;
	char	   *storage;
	int			j,
				k;

	/* Make sure we are in proper schema */
	selectSourceSchema(tbinfo->dobj.namespace->dobj.name);

	/* Is it a table or a view? */
	if (tbinfo->relkind == RELKIND_VIEW)
	{
		char	   *viewdef;

		reltypename = "VIEW";

		/* Fetch the view definition */
		if (g_fout->remoteVersion >= 70300)
		{
			/* Beginning in 7.3, viewname is not unique; rely on OID */
			appendPQExpBuffer(query,
							  "SELECT pg_catalog.pg_get_viewdef('%u'::pg_catalog.oid) as viewdef",
							  tbinfo->dobj.catId.oid);
		}
		else
		{
			appendPQExpBuffer(query, "SELECT definition as viewdef "
							  " from pg_views where viewname = ");
			appendStringLiteralAH(query, tbinfo->dobj.name, fout);
			appendPQExpBuffer(query, ";");
		}

		res = PQexec(g_conn, query->data);
		check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

		if (PQntuples(res) != 1)
		{
			if (PQntuples(res) < 1)
				write_msg(NULL, "query to obtain definition of view \"%s\" returned no data\n",
						  tbinfo->dobj.name);
			else
				write_msg(NULL, "query to obtain definition of view \"%s\" returned more than one definition\n",
						  tbinfo->dobj.name);
			exit_nicely();
		}

		viewdef = PQgetvalue(res, 0, 0);

		if (strlen(viewdef) == 0)
		{
			write_msg(NULL, "definition of view \"%s\" appears to be empty (length zero)\n",
					  tbinfo->dobj.name);
			exit_nicely();
		}

		/*
		 * DROP must be fully qualified in case same name appears in
		 * pg_catalog
		 */
		appendPQExpBuffer(delq, "DROP VIEW %s.",
						  fmtId(tbinfo->dobj.namespace->dobj.name));
		appendPQExpBuffer(delq, "%s;\n",
						  fmtId(tbinfo->dobj.name));

		appendPQExpBuffer(q, "CREATE VIEW %s AS\n    %s\n",
						  fmtId(tbinfo->dobj.name), viewdef);

		PQclear(res);
	}
	else
	{
		reltypename = "TABLE";
		numParents = tbinfo->numParents;
		parents = tbinfo->parents;

		/*
		 * DROP must be fully qualified in case same name appears in
		 * pg_catalog
		 */
		appendPQExpBuffer(delq, "DROP TABLE %s.",
						  fmtId(tbinfo->dobj.namespace->dobj.name));
		appendPQExpBuffer(delq, "%s;\n",
						  fmtId(tbinfo->dobj.name));

		appendPQExpBuffer(q, "CREATE TABLE %s (",
						  fmtId(tbinfo->dobj.name));
		actual_atts = 0;
		for (j = 0; j < tbinfo->numatts; j++)
		{
			/* Is this one of the table's own attrs, and not dropped ? */
			if (!tbinfo->inhAttrs[j] && !tbinfo->attisdropped[j])
			{
				/* Format properly if not first attr */
				if (actual_atts > 0)
					appendPQExpBuffer(q, ",");
				appendPQExpBuffer(q, "\n    ");

				/* Attribute name */
				appendPQExpBuffer(q, "%s ",
								  fmtId(tbinfo->attnames[j]));

				/* Attribute type */
				if (g_fout->remoteVersion >= 70100)
				{
					appendPQExpBuffer(q, "%s",
									  tbinfo->atttypnames[j]);
				}
				else
				{
					/* If no format_type, fake it */
					appendPQExpBuffer(q, "%s",
									  myFormatType(tbinfo->atttypnames[j],
												   tbinfo->atttypmod[j]));
				}

				/*
				 * Default value --- suppress if inherited or to be printed
				 * separately.
				 */
				if (tbinfo->attrdefs[j] != NULL &&
					!tbinfo->inhAttrDef[j] &&
					!tbinfo->attrdefs[j]->separate)
					appendPQExpBuffer(q, " DEFAULT %s",
									  tbinfo->attrdefs[j]->adef_expr);

				/*
				 * Not Null constraint --- suppress if inherited
				 */
				if (tbinfo->notnull[j] && !tbinfo->inhNotNull[j])
					appendPQExpBuffer(q, " NOT NULL");

				actual_atts++;
			}
		}

		/*
		 * Add non-inherited CHECK constraints, if any.
		 */
		for (j = 0; j < tbinfo->ncheck; j++)
		{
			ConstraintInfo *constr = &(tbinfo->checkexprs[j]);

			if (constr->coninherited || constr->separate)
				continue;

			if (actual_atts > 0)
				appendPQExpBuffer(q, ",\n    ");

			appendPQExpBuffer(q, "CONSTRAINT %s ",
							  fmtId(constr->dobj.name));
			appendPQExpBuffer(q, "%s", constr->condef);

			actual_atts++;
		}

		appendPQExpBuffer(q, "\n)");

		if (numParents > 0)
		{
			appendPQExpBuffer(q, "\nINHERITS (");
			for (k = 0; k < numParents; k++)
			{
				TableInfo  *parentRel = parents[k];

				if (k > 0)
					appendPQExpBuffer(q, ", ");
				if (parentRel->dobj.namespace != tbinfo->dobj.namespace)
					appendPQExpBuffer(q, "%s.",
								fmtId(parentRel->dobj.namespace->dobj.name));
				appendPQExpBuffer(q, "%s",
								  fmtId(parentRel->dobj.name));
			}
			appendPQExpBuffer(q, ")");
		}

		if (tbinfo->reloptions && strlen(tbinfo->reloptions) > 0)
			appendPQExpBuffer(q, "\nWITH (%s)", tbinfo->reloptions);

		appendPQExpBuffer(q, ";\n");

		/* Loop dumping statistics and storage statements */
		for (j = 0; j < tbinfo->numatts; j++)
		{
			/*
			 * Dump per-column statistics information. We only issue an ALTER
			 * TABLE statement if the attstattarget entry for this column is
			 * non-negative (i.e. it's not the default value)
			 */
			if (tbinfo->attstattarget[j] >= 0 &&
				!tbinfo->attisdropped[j])
			{
				appendPQExpBuffer(q, "ALTER TABLE ONLY %s ",
								  fmtId(tbinfo->dobj.name));
				appendPQExpBuffer(q, "ALTER COLUMN %s ",
								  fmtId(tbinfo->attnames[j]));
				appendPQExpBuffer(q, "SET STATISTICS %d;\n",
								  tbinfo->attstattarget[j]);
			}

			/*
			 * Dump per-column storage information.  The statement is only
			 * dumped if the storage has been changed from the type's default.
			 */
			if (!tbinfo->attisdropped[j] && tbinfo->attstorage[j] != tbinfo->typstorage[j])
			{
				switch (tbinfo->attstorage[j])
				{
					case 'p':
						storage = "PLAIN";
						break;
					case 'e':
						storage = "EXTERNAL";
						break;
					case 'm':
						storage = "MAIN";
						break;
					case 'x':
						storage = "EXTENDED";
						break;
					default:
						storage = NULL;
				}

				/*
				 * Only dump the statement if it's a storage type we recognize
				 */
				if (storage != NULL)
				{
					appendPQExpBuffer(q, "ALTER TABLE ONLY %s ",
									  fmtId(tbinfo->dobj.name));
					appendPQExpBuffer(q, "ALTER COLUMN %s ",
									  fmtId(tbinfo->attnames[j]));
					appendPQExpBuffer(q, "SET STORAGE %s;\n",
									  storage);
				}
			}
		}
	}

	ArchiveEntry(fout, tbinfo->dobj.catId, tbinfo->dobj.dumpId,
				 tbinfo->dobj.name,
				 tbinfo->dobj.namespace->dobj.name,
			(tbinfo->relkind == RELKIND_VIEW) ? NULL : tbinfo->reltablespace,
				 tbinfo->rolname,
			   (strcmp(reltypename, "TABLE") == 0) ? tbinfo->hasoids : false,
				 reltypename, q->data, delq->data, NULL,
				 tbinfo->dobj.dependencies, tbinfo->dobj.nDeps,
				 NULL, NULL);

	/* Dump Table Comments */
	dumpTableComment(fout, tbinfo, reltypename);

	/* Dump comments on inlined table constraints */
	for (j = 0; j < tbinfo->ncheck; j++)
	{
		ConstraintInfo *constr = &(tbinfo->checkexprs[j]);

		if (constr->coninherited || constr->separate)
			continue;

		dumpTableConstraintComment(fout, constr);
	}

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
}

/*
 * dumpAttrDef --- dump an attribute's default-value declaration
 */
static void
dumpAttrDef(Archive *fout, AttrDefInfo *adinfo)
{
	TableInfo  *tbinfo = adinfo->adtable;
	int			adnum = adinfo->adnum;
	PQExpBuffer q;
	PQExpBuffer delq;

	/* Only print it if "separate" mode is selected */
	if (!tbinfo->dobj.dump || !adinfo->separate || dataOnly)
		return;

	/* Don't print inherited defaults, either */
	if (tbinfo->inhAttrDef[adnum - 1])
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	appendPQExpBuffer(q, "ALTER TABLE %s ",
					  fmtId(tbinfo->dobj.name));
	appendPQExpBuffer(q, "ALTER COLUMN %s SET DEFAULT %s;\n",
					  fmtId(tbinfo->attnames[adnum - 1]),
					  adinfo->adef_expr);

	/*
	 * DROP must be fully qualified in case same name appears in pg_catalog
	 */
	appendPQExpBuffer(delq, "ALTER TABLE %s.",
					  fmtId(tbinfo->dobj.namespace->dobj.name));
	appendPQExpBuffer(delq, "%s ",
					  fmtId(tbinfo->dobj.name));
	appendPQExpBuffer(delq, "ALTER COLUMN %s DROP DEFAULT;\n",
					  fmtId(tbinfo->attnames[adnum - 1]));

	ArchiveEntry(fout, adinfo->dobj.catId, adinfo->dobj.dumpId,
				 tbinfo->attnames[adnum - 1],
				 tbinfo->dobj.namespace->dobj.name,
				 NULL,
				 tbinfo->rolname,
				 false, "DEFAULT", q->data, delq->data, NULL,
				 adinfo->dobj.dependencies, adinfo->dobj.nDeps,
				 NULL, NULL);

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
	write_msg(NULL, "invalid column number %d for table \"%s\"\n",
			  attrnum, tblInfo->dobj.name);
	exit_nicely();
	return NULL;				/* keep compiler quiet */
}

/*
 * dumpIndex
 *	  write out to fout a user-defined index
 */
static void
dumpIndex(Archive *fout, IndxInfo *indxinfo)
{
	TableInfo  *tbinfo = indxinfo->indextable;
	PQExpBuffer q;
	PQExpBuffer delq;

	if (dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	/*
	 * If there's an associated constraint, don't dump the index per se, but
	 * do dump any comment for it.	(This is safe because dependency ordering
	 * will have ensured the constraint is emitted first.)
	 */
	if (indxinfo->indexconstraint == 0)
	{
		/* Plain secondary index */
		appendPQExpBuffer(q, "%s;\n", indxinfo->indexdef);

		/* If the index is clustered, we need to record that. */
		if (indxinfo->indisclustered)
		{
			appendPQExpBuffer(q, "\nALTER TABLE %s CLUSTER",
							  fmtId(tbinfo->dobj.name));
			appendPQExpBuffer(q, " ON %s;\n",
							  fmtId(indxinfo->dobj.name));
		}

		/*
		 * DROP must be fully qualified in case same name appears in
		 * pg_catalog
		 */
		appendPQExpBuffer(delq, "DROP INDEX %s.",
						  fmtId(tbinfo->dobj.namespace->dobj.name));
		appendPQExpBuffer(delq, "%s;\n",
						  fmtId(indxinfo->dobj.name));

		ArchiveEntry(fout, indxinfo->dobj.catId, indxinfo->dobj.dumpId,
					 indxinfo->dobj.name,
					 tbinfo->dobj.namespace->dobj.name,
					 indxinfo->tablespace,
					 tbinfo->rolname, false,
					 "INDEX", q->data, delq->data, NULL,
					 indxinfo->dobj.dependencies, indxinfo->dobj.nDeps,
					 NULL, NULL);
	}

	/* Dump Index Comments */
	resetPQExpBuffer(q);
	appendPQExpBuffer(q, "INDEX %s",
					  fmtId(indxinfo->dobj.name));
	dumpComment(fout, q->data,
				tbinfo->dobj.namespace->dobj.name,
				tbinfo->rolname,
				indxinfo->dobj.catId, 0, indxinfo->dobj.dumpId);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
}

/*
 * dumpConstraint
 *	  write out to fout a user-defined constraint
 */
static void
dumpConstraint(Archive *fout, ConstraintInfo *coninfo)
{
	TableInfo  *tbinfo = coninfo->contable;
	PQExpBuffer q;
	PQExpBuffer delq;

	/* Skip if not to be dumped */
	if (!coninfo->dobj.dump || dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	if (coninfo->contype == 'p' || coninfo->contype == 'u')
	{
		/* Index-related constraint */
		IndxInfo   *indxinfo;
		int			k;

		indxinfo = (IndxInfo *) findObjectByDumpId(coninfo->conindex);

		if (indxinfo == NULL)
		{
			write_msg(NULL, "missing index for constraint \"%s\"\n",
					  coninfo->dobj.name);
			exit_nicely();
		}

		appendPQExpBuffer(q, "ALTER TABLE ONLY %s\n",
						  fmtId(tbinfo->dobj.name));
		appendPQExpBuffer(q, "    ADD CONSTRAINT %s %s (",
						  fmtId(coninfo->dobj.name),
						  coninfo->contype == 'p' ? "PRIMARY KEY" : "UNIQUE");

		for (k = 0; k < indxinfo->indnkeys; k++)
		{
			int			indkey = (int) indxinfo->indkeys[k];
			const char *attname;

			if (indkey == InvalidAttrNumber)
				break;
			attname = getAttrName(indkey, tbinfo);

			appendPQExpBuffer(q, "%s%s",
							  (k == 0) ? "" : ", ",
							  fmtId(attname));
		}

		appendPQExpBuffer(q, ")");

		if (indxinfo->options && strlen(indxinfo->options) > 0)
			appendPQExpBuffer(q, " WITH (%s)", indxinfo->options);

		appendPQExpBuffer(q, ";\n");

		/* If the index is clustered, we need to record that. */
		if (indxinfo->indisclustered)
		{
			appendPQExpBuffer(q, "\nALTER TABLE %s CLUSTER",
							  fmtId(tbinfo->dobj.name));
			appendPQExpBuffer(q, " ON %s;\n",
							  fmtId(indxinfo->dobj.name));
		}

		/*
		 * DROP must be fully qualified in case same name appears in
		 * pg_catalog
		 */
		appendPQExpBuffer(delq, "ALTER TABLE ONLY %s.",
						  fmtId(tbinfo->dobj.namespace->dobj.name));
		appendPQExpBuffer(delq, "%s ",
						  fmtId(tbinfo->dobj.name));
		appendPQExpBuffer(delq, "DROP CONSTRAINT %s;\n",
						  fmtId(coninfo->dobj.name));

		ArchiveEntry(fout, coninfo->dobj.catId, coninfo->dobj.dumpId,
					 coninfo->dobj.name,
					 tbinfo->dobj.namespace->dobj.name,
					 indxinfo->tablespace,
					 tbinfo->rolname, false,
					 "CONSTRAINT", q->data, delq->data, NULL,
					 coninfo->dobj.dependencies, coninfo->dobj.nDeps,
					 NULL, NULL);
	}
	else if (coninfo->contype == 'f')
	{
		/*
		 * XXX Potentially wrap in a 'SET CONSTRAINTS OFF' block so that the
		 * current table data is not processed
		 */
		appendPQExpBuffer(q, "ALTER TABLE ONLY %s\n",
						  fmtId(tbinfo->dobj.name));
		appendPQExpBuffer(q, "    ADD CONSTRAINT %s %s;\n",
						  fmtId(coninfo->dobj.name),
						  coninfo->condef);

		/*
		 * DROP must be fully qualified in case same name appears in
		 * pg_catalog
		 */
		appendPQExpBuffer(delq, "ALTER TABLE ONLY %s.",
						  fmtId(tbinfo->dobj.namespace->dobj.name));
		appendPQExpBuffer(delq, "%s ",
						  fmtId(tbinfo->dobj.name));
		appendPQExpBuffer(delq, "DROP CONSTRAINT %s;\n",
						  fmtId(coninfo->dobj.name));

		ArchiveEntry(fout, coninfo->dobj.catId, coninfo->dobj.dumpId,
					 coninfo->dobj.name,
					 tbinfo->dobj.namespace->dobj.name,
					 NULL,
					 tbinfo->rolname, false,
					 "FK CONSTRAINT", q->data, delq->data, NULL,
					 coninfo->dobj.dependencies, coninfo->dobj.nDeps,
					 NULL, NULL);
	}
	else if (coninfo->contype == 'c' && tbinfo)
	{
		/* CHECK constraint on a table */

		/* Ignore if not to be dumped separately */
		if (coninfo->separate)
		{
			/* not ONLY since we want it to propagate to children */
			appendPQExpBuffer(q, "ALTER TABLE %s\n",
							  fmtId(tbinfo->dobj.name));
			appendPQExpBuffer(q, "    ADD CONSTRAINT %s %s;\n",
							  fmtId(coninfo->dobj.name),
							  coninfo->condef);

			/*
			 * DROP must be fully qualified in case same name appears in
			 * pg_catalog
			 */
			appendPQExpBuffer(delq, "ALTER TABLE %s.",
							  fmtId(tbinfo->dobj.namespace->dobj.name));
			appendPQExpBuffer(delq, "%s ",
							  fmtId(tbinfo->dobj.name));
			appendPQExpBuffer(delq, "DROP CONSTRAINT %s;\n",
							  fmtId(coninfo->dobj.name));

			ArchiveEntry(fout, coninfo->dobj.catId, coninfo->dobj.dumpId,
						 coninfo->dobj.name,
						 tbinfo->dobj.namespace->dobj.name,
						 NULL,
						 tbinfo->rolname, false,
						 "CHECK CONSTRAINT", q->data, delq->data, NULL,
						 coninfo->dobj.dependencies, coninfo->dobj.nDeps,
						 NULL, NULL);
		}
	}
	else if (coninfo->contype == 'c' && tbinfo == NULL)
	{
		/* CHECK constraint on a domain */
		TypeInfo   *tinfo = coninfo->condomain;

		/* Ignore if not to be dumped separately */
		if (coninfo->separate)
		{
			appendPQExpBuffer(q, "ALTER DOMAIN %s\n",
							  fmtId(tinfo->dobj.name));
			appendPQExpBuffer(q, "    ADD CONSTRAINT %s %s;\n",
							  fmtId(coninfo->dobj.name),
							  coninfo->condef);

			/*
			 * DROP must be fully qualified in case same name appears in
			 * pg_catalog
			 */
			appendPQExpBuffer(delq, "ALTER DOMAIN %s.",
							  fmtId(tinfo->dobj.namespace->dobj.name));
			appendPQExpBuffer(delq, "%s ",
							  fmtId(tinfo->dobj.name));
			appendPQExpBuffer(delq, "DROP CONSTRAINT %s;\n",
							  fmtId(coninfo->dobj.name));

			ArchiveEntry(fout, coninfo->dobj.catId, coninfo->dobj.dumpId,
						 coninfo->dobj.name,
						 tinfo->dobj.namespace->dobj.name,
						 NULL,
						 tinfo->rolname, false,
						 "CHECK CONSTRAINT", q->data, delq->data, NULL,
						 coninfo->dobj.dependencies, coninfo->dobj.nDeps,
						 NULL, NULL);
		}
	}
	else
	{
		write_msg(NULL, "unrecognized constraint type: %c\n", coninfo->contype);
		exit_nicely();
	}

	/* Dump Constraint Comments --- only works for table constraints */
	if (tbinfo && coninfo->separate)
		dumpTableConstraintComment(fout, coninfo);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
}

/*
 * dumpTableConstraintComment --- dump a constraint's comment if any
 *
 * This is split out because we need the function in two different places
 * depending on whether the constraint is dumped as part of CREATE TABLE
 * or as a separate ALTER command.
 */
static void
dumpTableConstraintComment(Archive *fout, ConstraintInfo *coninfo)
{
	TableInfo  *tbinfo = coninfo->contable;
	PQExpBuffer q = createPQExpBuffer();

	appendPQExpBuffer(q, "CONSTRAINT %s ",
					  fmtId(coninfo->dobj.name));
	appendPQExpBuffer(q, "ON %s",
					  fmtId(tbinfo->dobj.name));
	dumpComment(fout, q->data,
				tbinfo->dobj.namespace->dobj.name,
				tbinfo->rolname,
				coninfo->dobj.catId, 0,
			 coninfo->separate ? coninfo->dobj.dumpId : tbinfo->dobj.dumpId);

	destroyPQExpBuffer(q);
}

/*
 * findLastBuiltInOid -
 * find the last built in oid
 *
 * For 7.1 and 7.2, we do this by retrieving datlastsysoid from the
 * pg_database entry for the current database
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
	appendStringLiteralAH(query, dbname, g_fout);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

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
 *
 * For 7.0, we do this by assuming that the last thing that initdb does is to
 * create the pg_indexes view.	This sucks in general, but seeing that 7.0.x
 * initdb won't be changing anymore, it'll do.
 */
static Oid
findLastBuiltinOid_V70(void)
{
	PGresult   *res;
	int			ntups;
	int			last_oid;

	res = PQexec(g_conn,
				 "SELECT oid FROM pg_class WHERE relname = 'pg_indexes'");
	check_sql_result(res, g_conn,
					 "SELECT oid FROM pg_class WHERE relname = 'pg_indexes'",
					 PGRES_TUPLES_OK);
	ntups = PQntuples(res);
	if (ntups < 1)
	{
		write_msg(NULL, "could not find entry for pg_indexes in pg_class\n");
		exit_nicely();
	}
	if (ntups > 1)
	{
		write_msg(NULL, "found more than one entry for pg_indexes in pg_class\n");
		exit_nicely();
	}
	last_oid = atooid(PQgetvalue(res, 0, PQfnumber(res, "oid")));
	PQclear(res);
	return last_oid;
}

static void
dumpSequence(Archive *fout, TableInfo *tbinfo)
{
	PGresult   *res;
	char	   *last,
			   *incby,
			   *maxv = NULL,
			   *minv = NULL,
			   *cache;
	char		bufm[100],
				bufx[100];
	bool		cycled,
				called;
	PQExpBuffer query = createPQExpBuffer();
	PQExpBuffer delqry = createPQExpBuffer();

	/* Make sure we are in proper schema */
	selectSourceSchema(tbinfo->dobj.namespace->dobj.name);

	snprintf(bufm, sizeof(bufm), INT64_FORMAT, SEQ_MINVALUE);
	snprintf(bufx, sizeof(bufx), INT64_FORMAT, SEQ_MAXVALUE);

	appendPQExpBuffer(query,
					  "SELECT sequence_name, last_value, increment_by, "
				   "CASE WHEN increment_by > 0 AND max_value = %s THEN NULL "
				   "     WHEN increment_by < 0 AND max_value = -1 THEN NULL "
					  "     ELSE max_value "
					  "END AS max_value, "
					"CASE WHEN increment_by > 0 AND min_value = 1 THEN NULL "
				   "     WHEN increment_by < 0 AND min_value = %s THEN NULL "
					  "     ELSE min_value "
					  "END AS min_value, "
					  "cache_value, is_cycled, is_called from %s",
					  bufx, bufm,
					  fmtId(tbinfo->dobj.name));

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	if (PQntuples(res) != 1)
	{
		write_msg(NULL, "query to get data of sequence \"%s\" returned %d rows (expected 1)\n",
				  tbinfo->dobj.name, PQntuples(res));
		exit_nicely();
	}

	/* Disable this check: it fails if sequence has been renamed */
#ifdef NOT_USED
	if (strcmp(PQgetvalue(res, 0, 0), tbinfo->dobj.name) != 0)
	{
		write_msg(NULL, "query to get data of sequence \"%s\" returned name \"%s\"\n",
				  tbinfo->dobj.name, PQgetvalue(res, 0, 0));
		exit_nicely();
	}
#endif

	last = PQgetvalue(res, 0, 1);
	incby = PQgetvalue(res, 0, 2);
	if (!PQgetisnull(res, 0, 3))
		maxv = PQgetvalue(res, 0, 3);
	if (!PQgetisnull(res, 0, 4))
		minv = PQgetvalue(res, 0, 4);
	cache = PQgetvalue(res, 0, 5);
	cycled = (strcmp(PQgetvalue(res, 0, 6), "t") == 0);
	called = (strcmp(PQgetvalue(res, 0, 7), "t") == 0);

	/*
	 * The logic we use for restoring sequences is as follows:
	 *
	 * Add a CREATE SEQUENCE statement as part of a "schema" dump (use
	 * last_val for start if called is false, else use min_val for start_val).
	 * Also, if the sequence is owned by a column, add an ALTER SEQUENCE
	 * OWNED BY command for it.
	 *
	 * Add a 'SETVAL(seq, last_val, iscalled)' as part of a "data" dump.
	 */
	if (!dataOnly)
	{
		resetPQExpBuffer(delqry);

		/*
		 * DROP must be fully qualified in case same name appears in
		 * pg_catalog
		 */
		appendPQExpBuffer(delqry, "DROP SEQUENCE %s.",
						  fmtId(tbinfo->dobj.namespace->dobj.name));
		appendPQExpBuffer(delqry, "%s;\n",
						  fmtId(tbinfo->dobj.name));

		resetPQExpBuffer(query);
		appendPQExpBuffer(query,
						  "CREATE SEQUENCE %s\n",
						  fmtId(tbinfo->dobj.name));

		if (!called)
			appendPQExpBuffer(query, "    START WITH %s\n", last);

		appendPQExpBuffer(query, "    INCREMENT BY %s\n", incby);

		if (maxv)
			appendPQExpBuffer(query, "    MAXVALUE %s\n", maxv);
		else
			appendPQExpBuffer(query, "    NO MAXVALUE\n");

		if (minv)
			appendPQExpBuffer(query, "    MINVALUE %s\n", minv);
		else
			appendPQExpBuffer(query, "    NO MINVALUE\n");

		appendPQExpBuffer(query,
						  "    CACHE %s%s",
						  cache, (cycled ? "\n    CYCLE" : ""));

		appendPQExpBuffer(query, ";\n");

		ArchiveEntry(fout, tbinfo->dobj.catId, tbinfo->dobj.dumpId,
					 tbinfo->dobj.name,
					 tbinfo->dobj.namespace->dobj.name,
					 NULL,
					 tbinfo->rolname,
					 false, "SEQUENCE", query->data, delqry->data, NULL,
					 tbinfo->dobj.dependencies, tbinfo->dobj.nDeps,
					 NULL, NULL);

		/*
		 * If the sequence is owned by a table column, emit the ALTER for it
		 * as a separate TOC entry immediately following the sequence's own
		 * entry.  It's OK to do this rather than using full sorting logic,
		 * because the dependency that tells us it's owned will have forced
		 * the table to be created first.  We can't just include the ALTER in
		 * the TOC entry because it will fail if we haven't reassigned the
		 * sequence owner to match the table's owner.
		 *
		 * We need not schema-qualify the table reference because both
		 * sequence and table must be in the same schema.
		 */
		if (OidIsValid(tbinfo->owning_tab))
		{
			TableInfo  *owning_tab = findTableByOid(tbinfo->owning_tab);

			if (owning_tab && owning_tab->dobj.dump)
			{
				resetPQExpBuffer(query);
				appendPQExpBuffer(query, "ALTER SEQUENCE %s",
								  fmtId(tbinfo->dobj.name));
				appendPQExpBuffer(query, " OWNED BY %s",
								  fmtId(owning_tab->dobj.name));
				appendPQExpBuffer(query, ".%s;\n",
						fmtId(owning_tab->attnames[tbinfo->owning_col - 1]));

				ArchiveEntry(fout, nilCatalogId, createDumpId(),
							 tbinfo->dobj.name,
							 tbinfo->dobj.namespace->dobj.name,
							 NULL,
							 tbinfo->rolname,
						   false, "SEQUENCE OWNED BY", query->data, "", NULL,
							 &(tbinfo->dobj.dumpId), 1,
							 NULL, NULL);
			}
		}

		/* Dump Sequence Comments */
		resetPQExpBuffer(query);
		appendPQExpBuffer(query, "SEQUENCE %s", fmtId(tbinfo->dobj.name));
		dumpComment(fout, query->data,
					tbinfo->dobj.namespace->dobj.name, tbinfo->rolname,
					tbinfo->dobj.catId, 0, tbinfo->dobj.dumpId);
	}

	if (!schemaOnly)
	{
		resetPQExpBuffer(query);
		appendPQExpBuffer(query, "SELECT pg_catalog.setval(");
		appendStringLiteralAH(query, fmtId(tbinfo->dobj.name), fout);
		appendPQExpBuffer(query, ", %s, %s);\n",
						  last, (called ? "true" : "false"));

		ArchiveEntry(fout, nilCatalogId, createDumpId(),
					 tbinfo->dobj.name,
					 tbinfo->dobj.namespace->dobj.name,
					 NULL,
					 tbinfo->rolname,
					 false, "SEQUENCE SET", query->data, "", NULL,
					 &(tbinfo->dobj.dumpId), 1,
					 NULL, NULL);
	}

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(delqry);
}

static void
dumpTrigger(Archive *fout, TriggerInfo *tginfo)
{
	TableInfo  *tbinfo = tginfo->tgtable;
	PQExpBuffer query;
	PQExpBuffer delqry;
	const char *p;
	int			findx;

	if (dataOnly)
		return;

	query = createPQExpBuffer();
	delqry = createPQExpBuffer();

	/*
	 * DROP must be fully qualified in case same name appears in pg_catalog
	 */
	appendPQExpBuffer(delqry, "DROP TRIGGER %s ",
					  fmtId(tginfo->dobj.name));
	appendPQExpBuffer(delqry, "ON %s.",
					  fmtId(tbinfo->dobj.namespace->dobj.name));
	appendPQExpBuffer(delqry, "%s;\n",
					  fmtId(tbinfo->dobj.name));

	if (tginfo->tgisconstraint)
	{
		appendPQExpBuffer(query, "CREATE CONSTRAINT TRIGGER ");
		appendPQExpBufferStr(query, fmtId(tginfo->tgconstrname));
	}
	else
	{
		appendPQExpBuffer(query, "CREATE TRIGGER ");
		appendPQExpBufferStr(query, fmtId(tginfo->dobj.name));
	}
	appendPQExpBuffer(query, "\n    ");

	/* Trigger type */
	findx = 0;
	if (TRIGGER_FOR_BEFORE(tginfo->tgtype))
		appendPQExpBuffer(query, "BEFORE");
	else
		appendPQExpBuffer(query, "AFTER");
	if (TRIGGER_FOR_INSERT(tginfo->tgtype))
	{
		appendPQExpBuffer(query, " INSERT");
		findx++;
	}
	if (TRIGGER_FOR_DELETE(tginfo->tgtype))
	{
		if (findx > 0)
			appendPQExpBuffer(query, " OR DELETE");
		else
			appendPQExpBuffer(query, " DELETE");
		findx++;
	}
	if (TRIGGER_FOR_UPDATE(tginfo->tgtype))
	{
		if (findx > 0)
			appendPQExpBuffer(query, " OR UPDATE");
		else
			appendPQExpBuffer(query, " UPDATE");
	}
	appendPQExpBuffer(query, " ON %s\n",
					  fmtId(tbinfo->dobj.name));

	if (tginfo->tgisconstraint)
	{
		if (OidIsValid(tginfo->tgconstrrelid))
		{
			/* If we are using regclass, name is already quoted */
			if (g_fout->remoteVersion >= 70300)
				appendPQExpBuffer(query, "    FROM %s\n    ",
								  tginfo->tgconstrrelname);
			else
				appendPQExpBuffer(query, "    FROM %s\n    ",
								  fmtId(tginfo->tgconstrrelname));
		}
		if (!tginfo->tgdeferrable)
			appendPQExpBuffer(query, "NOT ");
		appendPQExpBuffer(query, "DEFERRABLE INITIALLY ");
		if (tginfo->tginitdeferred)
			appendPQExpBuffer(query, "DEFERRED\n");
		else
			appendPQExpBuffer(query, "IMMEDIATE\n");
	}

	if (TRIGGER_FOR_ROW(tginfo->tgtype))
		appendPQExpBuffer(query, "    FOR EACH ROW\n    ");
	else
		appendPQExpBuffer(query, "    FOR EACH STATEMENT\n    ");

	/* In 7.3, result of regproc is already quoted */
	if (g_fout->remoteVersion >= 70300)
		appendPQExpBuffer(query, "EXECUTE PROCEDURE %s(",
						  tginfo->tgfname);
	else
		appendPQExpBuffer(query, "EXECUTE PROCEDURE %s(",
						  fmtId(tginfo->tgfname));

	p = tginfo->tgargs;
	for (findx = 0; findx < tginfo->tgnargs; findx++)
	{
		const char *s = p;

		/* Set 'p' to end of arg string. marked by '\000' */
		for (;;)
		{
			p = strchr(p, '\\');
			if (p == NULL)
			{
				write_msg(NULL, "invalid argument string (%s) for trigger \"%s\" on table \"%s\"\n",
						  tginfo->tgargs,
						  tginfo->dobj.name,
						  tbinfo->dobj.name);
				exit_nicely();
			}
			p++;
			if (*p == '\\')		/* is it '\\'? */
			{
				p++;
				continue;
			}
			if (p[0] == '0' && p[1] == '0' && p[2] == '0')		/* is it '\000'? */
				break;
		}
		p--;

		appendPQExpBufferChar(query, '\'');
		while (s < p)
		{
			if (*s == '\'')
				appendPQExpBufferChar(query, '\'');

			/*
			 * bytea unconditionally doubles backslashes, so we suppress the
			 * doubling for standard_conforming_strings.
			 */
			if (fout->std_strings && *s == '\\' && s[1] == '\\')
				s++;
			appendPQExpBufferChar(query, *s++);
		}
		appendPQExpBufferChar(query, '\'');
		appendPQExpBuffer(query,
						  (findx < tginfo->tgnargs - 1) ? ", " : "");
		p = p + 4;
	}
	appendPQExpBuffer(query, ");\n");

	if (!tginfo->tgenabled)
	{
		appendPQExpBuffer(query, "\nALTER TABLE %s ",
						  fmtId(tbinfo->dobj.name));
		appendPQExpBuffer(query, "DISABLE TRIGGER %s;\n",
						  fmtId(tginfo->dobj.name));
	}

	ArchiveEntry(fout, tginfo->dobj.catId, tginfo->dobj.dumpId,
				 tginfo->dobj.name,
				 tbinfo->dobj.namespace->dobj.name,
				 NULL,
				 tbinfo->rolname, false,
				 "TRIGGER", query->data, delqry->data, NULL,
				 tginfo->dobj.dependencies, tginfo->dobj.nDeps,
				 NULL, NULL);

	resetPQExpBuffer(query);
	appendPQExpBuffer(query, "TRIGGER %s ",
					  fmtId(tginfo->dobj.name));
	appendPQExpBuffer(query, "ON %s",
					  fmtId(tbinfo->dobj.name));

	dumpComment(fout, query->data,
				tbinfo->dobj.namespace->dobj.name, tbinfo->rolname,
				tginfo->dobj.catId, 0, tginfo->dobj.dumpId);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(delqry);
}

/*
 * dumpRule
 *		Dump a rule
 */
static void
dumpRule(Archive *fout, RuleInfo *rinfo)
{
	TableInfo  *tbinfo = rinfo->ruletable;
	PQExpBuffer query;
	PQExpBuffer cmd;
	PQExpBuffer delcmd;
	PGresult   *res;

	/* Skip if not to be dumped */
	if (!rinfo->dobj.dump || dataOnly)
		return;

	/*
	 * If it is an ON SELECT rule that is created implicitly by CREATE VIEW,
	 * we do not want to dump it as a separate object.
	 */
	if (!rinfo->separate)
		return;

	/*
	 * Make sure we are in proper schema.
	 */
	selectSourceSchema(tbinfo->dobj.namespace->dobj.name);

	query = createPQExpBuffer();
	cmd = createPQExpBuffer();
	delcmd = createPQExpBuffer();

	if (g_fout->remoteVersion >= 70300)
	{
		appendPQExpBuffer(query,
						  "SELECT pg_catalog.pg_get_ruledef('%u'::pg_catalog.oid) AS definition",
						  rinfo->dobj.catId.oid);
	}
	else
	{
		/* Rule name was unique before 7.3 ... */
		appendPQExpBuffer(query,
						  "SELECT pg_get_ruledef('%s') AS definition",
						  rinfo->dobj.name);
	}

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	if (PQntuples(res) != 1)
	{
		write_msg(NULL, "query to get rule \"%s\" for table \"%s\" failed: wrong number of rows returned",
				  rinfo->dobj.name, tbinfo->dobj.name);
		exit_nicely();
	}

	printfPQExpBuffer(cmd, "%s\n", PQgetvalue(res, 0, 0));

	/*
	 * DROP must be fully qualified in case same name appears in pg_catalog
	 */
	appendPQExpBuffer(delcmd, "DROP RULE %s ",
					  fmtId(rinfo->dobj.name));
	appendPQExpBuffer(delcmd, "ON %s.",
					  fmtId(tbinfo->dobj.namespace->dobj.name));
	appendPQExpBuffer(delcmd, "%s;\n",
					  fmtId(tbinfo->dobj.name));

	ArchiveEntry(fout, rinfo->dobj.catId, rinfo->dobj.dumpId,
				 rinfo->dobj.name,
				 tbinfo->dobj.namespace->dobj.name,
				 NULL,
				 tbinfo->rolname, false,
				 "RULE", cmd->data, delcmd->data, NULL,
				 rinfo->dobj.dependencies, rinfo->dobj.nDeps,
				 NULL, NULL);

	/* Dump rule comments */
	resetPQExpBuffer(query);
	appendPQExpBuffer(query, "RULE %s",
					  fmtId(rinfo->dobj.name));
	appendPQExpBuffer(query, " ON %s",
					  fmtId(tbinfo->dobj.name));
	dumpComment(fout, query->data,
				tbinfo->dobj.namespace->dobj.name,
				tbinfo->rolname,
				rinfo->dobj.catId, 0, rinfo->dobj.dumpId);

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(cmd);
	destroyPQExpBuffer(delcmd);
}

/*
 * getDependencies --- obtain available dependency data
 */
static void
getDependencies(void)
{
	PQExpBuffer query;
	PGresult   *res;
	int			ntups,
				i;
	int			i_classid,
				i_objid,
				i_refclassid,
				i_refobjid,
				i_deptype;
	DumpableObject *dobj,
			   *refdobj;

	/* No dependency info available before 7.3 */
	if (g_fout->remoteVersion < 70300)
		return;

	if (g_verbose)
		write_msg(NULL, "reading dependency data\n");

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	query = createPQExpBuffer();

	appendPQExpBuffer(query, "SELECT "
					  "classid, objid, refclassid, refobjid, deptype "
					  "FROM pg_depend "
					  "WHERE deptype != 'p' "
					  "ORDER BY 1,2");

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_classid = PQfnumber(res, "classid");
	i_objid = PQfnumber(res, "objid");
	i_refclassid = PQfnumber(res, "refclassid");
	i_refobjid = PQfnumber(res, "refobjid");
	i_deptype = PQfnumber(res, "deptype");

	/*
	 * Since we ordered the SELECT by referencing ID, we can expect that
	 * multiple entries for the same object will appear together; this saves
	 * on searches.
	 */
	dobj = NULL;

	for (i = 0; i < ntups; i++)
	{
		CatalogId	objId;
		CatalogId	refobjId;
		char		deptype;

		objId.tableoid = atooid(PQgetvalue(res, i, i_classid));
		objId.oid = atooid(PQgetvalue(res, i, i_objid));
		refobjId.tableoid = atooid(PQgetvalue(res, i, i_refclassid));
		refobjId.oid = atooid(PQgetvalue(res, i, i_refobjid));
		deptype = *(PQgetvalue(res, i, i_deptype));

		if (dobj == NULL ||
			dobj->catId.tableoid != objId.tableoid ||
			dobj->catId.oid != objId.oid)
			dobj = findObjectByCatalogId(objId);

		/*
		 * Failure to find objects mentioned in pg_depend is not unexpected,
		 * since for example we don't collect info about TOAST tables.
		 */
		if (dobj == NULL)
		{
#ifdef NOT_USED
			fprintf(stderr, "no referencing object %u %u\n",
					objId.tableoid, objId.oid);
#endif
			continue;
		}

		refdobj = findObjectByCatalogId(refobjId);

		if (refdobj == NULL)
		{
#ifdef NOT_USED
			fprintf(stderr, "no referenced object %u %u\n",
					refobjId.tableoid, refobjId.oid);
#endif
			continue;
		}

		/*
		 * Ordinarily, table rowtypes have implicit dependencies on their
		 * tables.	However, for a composite type the implicit dependency goes
		 * the other way in pg_depend; which is the right thing for DROP but
		 * it doesn't produce the dependency ordering we need. So in that one
		 * case, we reverse the direction of the dependency.
		 */
		if (deptype == 'i' &&
			dobj->objType == DO_TABLE &&
			refdobj->objType == DO_TYPE)
			addObjectDependency(refdobj, dobj->dumpId);
		else
			/* normal case */
			addObjectDependency(dobj, refdobj->dumpId);
	}

	PQclear(res);

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
	static char *curSchemaName = NULL;
	PQExpBuffer query;

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
					  fmtId(schemaName));
	if (strcmp(schemaName, "pg_catalog") != 0)
		appendPQExpBuffer(query, ", pg_catalog");

	do_sql_command(g_conn, query->data);

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
getFormattedTypeName(Oid oid, OidOptions opts)
{
	char	   *result;
	PQExpBuffer query;
	PGresult   *res;
	int			ntups;

	if (oid == 0)
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
		appendPQExpBuffer(query, "SELECT pg_catalog.format_type('%u'::pg_catalog.oid, NULL)",
						  oid);
	}
	else if (g_fout->remoteVersion >= 70100)
	{
		appendPQExpBuffer(query, "SELECT format_type('%u'::oid, NULL)",
						  oid);
	}
	else
	{
		appendPQExpBuffer(query, "SELECT typname "
						  "FROM pg_type "
						  "WHERE oid = '%u'::oid",
						  oid);
	}

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	/* Expecting a single result only */
	ntups = PQntuples(res);
	if (ntups != 1)
	{
		write_msg(NULL, "query yielded %d rows instead of one: %s\n",
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
		result = strdup(fmtId(PQgetvalue(res, 0, 0)));
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
	bool		isarray = false;
	PQExpBuffer buf = createPQExpBuffer();

	/* Handle array types */
	if (typname[0] == '_')
	{
		isarray = true;
		typname++;
	}

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
	 * char is an internal single-byte data type; Let's make sure we force it
	 * through with quotes. - thomas 1998-12-13
	 */
	else if (strcmp(typname, "char") == 0)
		appendPQExpBuffer(buf, "\"char\"");
	else
		appendPQExpBuffer(buf, "%s", fmtId(typname));

	/* Append array qualifier for array types */
	if (isarray)
		appendPQExpBuffer(buf, "[]");

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
						  fmtId(schema));
	}
	appendPQExpBuffer(id_return, "%s",
					  fmtId(id));

	return id_return->data;
}

/*
 * Return a column list clause for the given relation.
 *
 * Special case: if there are no undropped columns in the relation, return
 * "", not an invalid "()" column list.
 */
static const char *
fmtCopyColumnList(const TableInfo *ti)
{
	static PQExpBuffer q = NULL;
	int			numatts = ti->numatts;
	char	  **attnames = ti->attnames;
	bool	   *attisdropped = ti->attisdropped;
	bool		needComma;
	int			i;

	if (q)						/* first time through? */
		resetPQExpBuffer(q);
	else
		q = createPQExpBuffer();

	appendPQExpBuffer(q, "(");
	needComma = false;
	for (i = 0; i < numatts; i++)
	{
		if (attisdropped[i])
			continue;
		if (needComma)
			appendPQExpBuffer(q, ", ");
		appendPQExpBuffer(q, "%s", fmtId(attnames[i]));
		needComma = true;
	}

	if (!needComma)
		return "";				/* no undropped columns */

	appendPQExpBuffer(q, ")");
	return q->data;
}

/*
 * Convenience subroutine to execute a SQL command and check for
 * COMMAND_OK status.
 */
static void
do_sql_command(PGconn *conn, const char *query)
{
	PGresult   *res;

	res = PQexec(conn, query);
	check_sql_result(res, conn, query, PGRES_COMMAND_OK);
	PQclear(res);
}

/*
 * Convenience subroutine to verify a SQL command succeeded,
 * and exit with a useful error message if not.
 */
static void
check_sql_result(PGresult *res, PGconn *conn, const char *query,
				 ExecStatusType expected)
{
	const char *err;

	if (res && PQresultStatus(res) == expected)
		return;					/* A-OK */

	write_msg(NULL, "SQL command failed\n");
	if (res)
		err = PQresultErrorMessage(res);
	else
		err = PQerrorMessage(conn);
	write_msg(NULL, "Error message from server: %s", err);
	write_msg(NULL, "The command was: %s\n", query);
	exit_nicely();
}
