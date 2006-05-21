/*-------------------------------------------------------------------------
 *
 * pg_dump.c
 *	  pg_dump is a utility for dumping out a postgres database
 *	  into a script file.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	pg_dump will read the system catalogs in a database and dump out a
 *	script that reproduces the schema in terms of SQL that is understood
 *	by PostgreSQL
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/bin/pg_dump/pg_dump.c,v 1.355.2.8 2006/05/21 19:57:07 momjian Exp $
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

#ifndef HAVE_STRDUP
#include "strdup.h"
#endif

#include "getopt_long.h"

#ifndef HAVE_OPTRESET
int			optreset;
#endif

#include "access/attnum.h"
#include "access/htup.h"
#include "catalog/pg_class.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"

#include "commands/sequence.h"

#include "libpq-fe.h"
#include "libpq/libpq-fs.h"

#include "pg_dump.h"
#include "pg_backup.h"
#include "pg_backup_archiver.h"
#include "dumputils.h"

#define _(x) gettext((x))

typedef struct _dumpContext
{
	TableInfo  *tblinfo;
	int			tblidx;
	bool		oids;
} DumpContext;

static void help(const char *progname);
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
static void dumpOneCompositeType(Archive *fout, TypeInfo *tinfo);
static void dumpOneTable(Archive *fout, TableInfo *tbinfo,
			 TableInfo *g_tblinfo);
static void dumpOneSequence(Archive *fout, TableInfo *tbinfo,
				const bool schemaOnly, const bool dataOnly);

static void dumpTableACL(Archive *fout, TableInfo *tbinfo);
static void dumpFuncACL(Archive *fout, FuncInfo *finfo);
static void dumpAggACL(Archive *fout, AggInfo *finfo);
static void dumpACL(Archive *fout, const char *type, const char *name,
		const char *tag, const char *nspname,
		const char *owner, const char *acl, const char *objoid);

static void dumpConstraints(Archive *fout, TableInfo *tblinfo, int numTables);
static void dumpTriggers(Archive *fout, TableInfo *tblinfo, int numTables);
static void dumpRules(Archive *fout, TableInfo *tblinfo, int numTables);
static char *format_function_signature(FuncInfo *finfo, bool honor_quotes);
static void dumpOneFunc(Archive *fout, FuncInfo *finfo);
static void dumpOneOpr(Archive *fout, OprInfo *oprinfo,
		   OprInfo *g_oprinfo, int numOperators);
static const char *convertRegProcReference(const char *proc);
static const char *convertOperatorReference(const char *opr,
						 OprInfo *g_oprinfo, int numOperators);
static void dumpOneOpclass(Archive *fout, OpclassInfo *opcinfo);
static void dumpOneAgg(Archive *fout, AggInfo *agginfo);
static Oid	findLastBuiltinOid_V71(const char *);
static Oid	findLastBuiltinOid_V70(void);
static void setMaxOid(Archive *fout);
static void selectSourceSchema(const char *schemaName);
static char *getFormattedTypeName(const char *oid, OidOptions opts);
static char *myFormatType(const char *typname, int32 typmod);
static const char *fmtQualifiedId(const char *schema, const char *id);
static int	dumpBlobs(Archive *AH, char *, void *);
static int	dumpDatabase(Archive *AH);
static void dumpEncoding(Archive *AH);
static const char *getAttrName(int attrnum, TableInfo *tblInfo);
static const char *fmtCopyColumnList(const TableInfo *ti);

extern char *optarg;
extern int	optind,
			opterr;

/* global decls */
bool		g_verbose;			/* User wants verbose narration of our
								 * activities. */
Archive    *g_fout;				/* the script file */
PGconn	   *g_conn;				/* the database connection */

/* various user-settable parameters */
bool		dumpData;			/* dump data using proper insert strings */
bool		attrNames;			/* put attr names into insert strings */
bool		schemaOnly;
bool		dataOnly;
bool		aclsSkip;

/* obsolete as of 7.3: */
static Oid	g_last_builtin_oid; /* value of the last builtin oid */

static char *selectTableName = NULL;	/* name of a single table to dump */
static char *selectSchemaName = NULL;	/* name of a single schema to dump */

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
	PGresult   *res;
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

#ifdef ENABLE_NLS
	setlocale(LC_ALL, "");
	bindtextdomain("pg_dump", LOCALEDIR);
	textdomain("pg_dump");
#endif

	g_verbose = false;

	strcpy(g_comment_start, "-- ");
	g_comment_end[0] = '\0';
	strcpy(g_opaque_type, "opaque");

	dataOnly = schemaOnly = dumpData = attrNames = false;

	progname = get_progname(argv[0]);

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

	while ((c = getopt_long(argc, argv, "abcCdDf:F:h:in:oOp:RsS:t:uU:vWxX:Z:",
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

			case 'n':			/* Dump data for this schema only */
				selectSchemaName = strdup(optarg);
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

			case 'S':			/* Username for superuser in plain text
								 * output */
				outputSuperuser = strdup(optarg);
				break;

			case 't':			/* Dump data for this table only */
				selectTableName = strdup(optarg);
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
					/* no-op, still allowed for compatibility */ ;
				else if (strcmp(optarg, "disable-triggers") == 0)
					disable_triggers = 1;
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
				/* This covers the long options equivalent to -X xxx. */

			case 0:
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

	if (outputBlobs && selectTableName != NULL)
	{
		write_msg(NULL, "large-object output not supported for a single table\n");
		write_msg(NULL, "use a full dump instead\n");
		exit(1);
	}

	if (outputBlobs && selectSchemaName != NULL)
	{
		write_msg(NULL, "large-object output not supported for a single schema\n");
		write_msg(NULL, "use a full dump instead\n");
		exit(1);
	}

	if (dumpData == true && oids == true)
	{
		write_msg(NULL, "INSERT (-d, -D) and OID (-o) options cannot be used together\n");
		write_msg(NULL, "(The INSERT command cannot set OIDs.)\n");
		exit(1);
	}

	if (outputBlobs == true && (format[0] == 'p' || format[0] == 'P'))
	{
		write_msg(NULL, "large-object output is not supported for plain-text dump files\n");
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
	 * Open the database using the Archiver, so it knows about it. Errors
	 * mean death.
	 */
	g_conn = ConnectDatabase(g_fout, dbname, pghost, pgport,
							 username, force_password, ignore_version);

	/*
	 * Start serializable transaction to dump consistent data.
	 */
	res = PQexec(g_conn, "BEGIN");
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
		exit_horribly(g_fout, NULL, "BEGIN command failed: %s",
					  PQerrorMessage(g_conn));
	PQclear(res);

	res = PQexec(g_conn, "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE");
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
		exit_horribly(g_fout, NULL, "could not set transaction isolation level to serializable: %s",
					  PQerrorMessage(g_conn));
	PQclear(res);

	/* Set the datestyle to ISO to ensure the dump's portability */
	res = PQexec(g_conn, "SET DATESTYLE = ISO");
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
		exit_horribly(g_fout, NULL, "could not set datestyle to ISO: %s",
					  PQerrorMessage(g_conn));
	PQclear(res);

	/*
	 * If supported, set extra_float_digits so that we can dump float data
	 * exactly (given correctly implemented float I/O code, anyway)
	 */
	if (g_fout->remoteVersion >= 70400)
	{
		res = PQexec(g_conn, "SET extra_float_digits TO 2");
		if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
			exit_horribly(g_fout, NULL, "could not set extra_float_digits: %s",
						  PQerrorMessage(g_conn));
		PQclear(res);
	}

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

	/* First the special encoding entry. */
	dumpEncoding(g_fout);

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
		dumpConstraints(g_fout, tblinfo, numTables);
		dumpTriggers(g_fout, tblinfo, numTables);
		dumpRules(g_fout, tblinfo, numTables);
	}

	/* Now sort the output nicely: by OID within object types */
	SortTocByOID(g_fout);
	SortTocByObjectType(g_fout);

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
	printf(_("  -a, --data-only          dump only the data, not the schema\n"));
	printf(_("  -b, --blobs              include large objects in dump\n"));
	printf(_("  -c, --clean              clean (drop) schema prior to create\n"));
	printf(_("  -C, --create             include commands to create database in dump\n"));
	printf(_("  -d, --inserts            dump data as INSERT, rather than COPY, commands\n"));
	printf(_("  -D, --column-inserts     dump data as INSERT commands with column names\n"));
	printf(_("  -n, --schema=SCHEMA      dump the named schema only\n"));
	printf(_("  -o, --oids               include OIDs in dump\n"));
	printf(_("  -O, --no-owner           do not output commands to set object ownership\n"
			 "                           in plain text format\n"));
	printf(_("  -s, --schema-only        dump only the schema, no data\n"));
	printf(_("  -S, --superuser=NAME     specify the superuser user name to use in\n"
			 "                           plain text format\n"));
	printf(_("  -t, --table=TABLE        dump the named table only\n"));
	printf(_("  -x, --no-privileges      do not dump privileges (grant/revoke)\n"));
	printf(_("  -X disable-triggers, --disable-triggers\n"
			 "                           disable triggers during data-only restore\n"));

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
 * selectDumpableNamespace: policy-setting subroutine
 *		Mark a namespace as to be dumped or not
 */
static void
selectDumpableNamespace(NamespaceInfo *nsinfo)
{
	/*
	 * If a specific table is being dumped, do not dump any complete
	 * namespaces.	If a specific namespace is being dumped, dump just
	 * that namespace. Otherwise, dump all non-system namespaces.
	 */
	if (selectTableName != NULL)
		nsinfo->dump = false;
	else if (selectSchemaName != NULL)
	{
		if (strcmp(nsinfo->nspname, selectSchemaName) == 0)
			nsinfo->dump = true;
		else
			nsinfo->dump = false;
	}
	else if (strncmp(nsinfo->nspname, "pg_", 3) == 0 ||
			 strcmp(nsinfo->nspname, "information_schema") == 0)
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
	 * tablename has been specified, dump matching table name; else, do
	 * not dump.
	 */
	tbinfo->dump = false;
	if (tbinfo->relnamespace->dump)
		tbinfo->dump = true;
	else if (selectTableName != NULL &&
			 strcmp(tbinfo->relname, selectTableName) == 0)
	{
		/* If both -s and -t specified, must match both to dump */
		if (selectSchemaName == NULL)
			tbinfo->dump = true;
		else if (strcmp(tbinfo->relnamespace->nspname, selectSchemaName) == 0)
			tbinfo->dump = true;
	}
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
	const char *column_list;

	if (g_verbose)
		write_msg(NULL, "dumping contents of table %s\n", classname);

	/*
	 * Make sure we are in proper schema.  We will qualify the table name
	 * below anyway (in case its name conflicts with a pg_catalog table);
	 * but this ensures reproducible results in case the table contains
	 * regproc, regclass, etc columns.
	 */
	selectSourceSchema(tbinfo->relnamespace->nspname);

	/*
	 * If possible, specify the column list explicitly so that we have no
	 * possibility of retrieving data in the wrong column order.  (The
	 * default column ordering of COPY will not be what we want in certain
	 * corner cases involving ADD COLUMN and inheritance.)
	 */
	if (g_fout->remoteVersion >= 70300)
		column_list = fmtCopyColumnList(tbinfo);
	else
		column_list = "";		/* can't select columns in COPY */

	if (oids && hasoids)
	{
		appendPQExpBuffer(q, "COPY %s %s WITH OIDS TO stdout;",
						  fmtQualifiedId(tbinfo->relnamespace->nspname,
										 classname),
						  column_list);
	}
	else
	{
		appendPQExpBuffer(q, "COPY %s %s TO stdout;",
						  fmtQualifiedId(tbinfo->relnamespace->nspname,
										 classname),
						  column_list);
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
		 * There was considerable discussion in late July, 2000 regarding
		 * slowing down pg_dump when backing up large tables. Users with
		 * both slow & fast (muti-processor) machines experienced
		 * performance degradation when doing a backup.
		 *
		 * Initial attempts based on sleeping for a number of ms for each ms
		 * of work were deemed too complex, then a simple 'sleep in each
		 * loop' implementation was suggested. The latter failed because
		 * the loop was too tight. Finally, the following was implemented:
		 *
		 * If throttle is non-zero, then See how long since the last sleep.
		 * Work out how long to sleep (based on ratio). If sleep is more
		 * than 100ms, then sleep reset timer EndIf EndIf
		 *
		 * where the throttle value was the number of ms to sleep per ms of
		 * work. The calculation was done in each loop.
		 *
		 * Most of the hard work is done in the backend, and this solution
		 * still did not work particularly well: on slow machines, the
		 * ratio was 50:1, and on medium paced machines, 1:1, and on fast
		 * multi-processor machines, it had little or no effect, for
		 * reasons that were unclear.
		 *
		 * Further discussion ensued, and the proposal was dropped.
		 *
		 * For those people who want this feature, it can be implemented
		 * using gettimeofday in each loop, calculating the time since
		 * last sleep, multiplying that by the sleep ratio, then if the
		 * result is more than a preset 'minimum sleep time' (say 100ms),
		 * call the 'select' function to sleep for a subsecond period ie.
		 *
		 * select(0, NULL, NULL, NULL, &tvi);
		 *
		 * This will return after the interval specified in the structure
		 * tvi. Finally, call gettimeofday again to save the 'last sleep
		 * time'.
		 */
	}
	archprintf(fout, "\\.\n\n\n");

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
	int			nfields;
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
				archprintf(fout, "%s", q->data);
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
							 * These types are printed without quotes
							 * unless they contain values that aren't
							 * accepted by the scanner unquoted (e.g.,
							 * 'NaN').	Note that strtod() and friends
							 * might accept NaN, so we can't use that to
							 * test.
							 *
							 * In reality we only need to defend against
							 * infinity and NaN, so we need not get too
							 * crazy about pattern matching here.
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
						appendStringLiteral(q, PQgetvalue(res, tuple, field), false);
						archprintf(fout, "%s", q->data);
						break;
				}
			}
			archprintf(fout, ");\n");
		}
	} while (PQntuples(res) > 0);

	archprintf(fout, "\n\n");
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
 * DumpClasses -
 *	  dump the contents of all the classes.
 */
static void
dumpClasses(const TableInfo *tblinfo, const int numTables, Archive *fout,
			const bool oids)
{
	PQExpBuffer copyBuf = createPQExpBuffer();
	DataDumperPtr dumpFn;
	DumpContext *dumpCtx;
	char	   *copyStmt;
	int			i;

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

			dumpCtx = (DumpContext *) calloc(1, sizeof(DumpContext));
			dumpCtx->tblinfo = (TableInfo *) tblinfo;
			dumpCtx->tblidx = i;
			dumpCtx->oids = oids;

			if (!dumpData)
			{
				/* Dump/restore using COPY */
				dumpFn = dumpClasses_nodumpData;
				resetPQExpBuffer(copyBuf);
				/* must use 2 steps here 'cause fmtId is nonreentrant */
				appendPQExpBuffer(copyBuf, "COPY %s ",
								  fmtId(tblinfo[i].relname));
				appendPQExpBuffer(copyBuf, "%s %sFROM stdin;\n",
								  fmtCopyColumnList(&(tblinfo[i])),
					   (oids && tblinfo[i].hasoids) ? "WITH OIDS " : "");
				copyStmt = copyBuf->data;
			}
			else
			{
				/* Restore using INSERT */
				dumpFn = dumpClasses_dumpData;
				copyStmt = NULL;
			}

			ArchiveEntry(fout, tblinfo[i].oid, tblinfo[i].relname,
						 tblinfo[i].relnamespace->nspname,
						 tblinfo[i].usename,
						 "TABLE DATA", NULL, "", "", copyStmt,
						 dumpFn, dumpCtx);
		}
	}

	destroyPQExpBuffer(copyBuf);
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
	if (g_fout->remoteVersion >= 70100)
	{
		appendPQExpBuffer(dbQry, "SELECT "
						  "(SELECT usename FROM pg_user WHERE usesysid = datdba) as dba, "
						  "pg_encoding_to_char(encoding) as encoding, "
						  "datpath "
						  "FROM pg_database "
						  "WHERE datname = ");
		appendStringLiteral(dbQry, datname, true);
	}
	else
	{
		/*
		 * In 7.0, datpath is either the same as datname, or the user-given
		 * location with "/" and the datname appended.  We must strip this
		 * junk off to produce a correct LOCATION value.
		 */
		appendPQExpBuffer(dbQry, "SELECT "
						  "(SELECT usename FROM pg_user WHERE usesysid = datdba) as dba, "
						  "pg_encoding_to_char(encoding) as encoding, "
						  "CASE WHEN length(datpath) > length(datname) THEN "
						  "substr(datpath,1,length(datpath)-length(datname)-1) "
						  "ELSE '' END as datpath "
						  "FROM pg_database "
						  "WHERE datname = ");
		appendStringLiteral(dbQry, datname, true);
	}

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
					  fmtId(datname));
	if (strlen(datpath) > 0)
	{
		appendPQExpBuffer(creaQry, " LOCATION = ");
		appendStringLiteral(creaQry, datpath, true);
	}
	if (strlen(encoding) > 0)
	{
		appendPQExpBuffer(creaQry, " ENCODING = ");
		appendStringLiteral(creaQry, encoding, true);
	}
	appendPQExpBuffer(creaQry, ";\n");

	appendPQExpBuffer(delQry, "DROP DATABASE %s;\n",
					  fmtId(datname));

	ArchiveEntry(AH, "0",		/* OID */
				 datname,		/* Name */
				 NULL,			/* Namespace */
				 dba,			/* Owner */
				 "DATABASE",	/* Desc */
				 NULL,			/* Deps */
				 creaQry->data, /* Create */
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
 * dumpEncoding: put the correct encoding into the archive
 */
static void
dumpEncoding(Archive *AH)
{
	PQExpBuffer qry;
	PGresult   *res;

	/* Can't read the encoding from pre-7.3 servers (SHOW isn't a query) */
	if (AH->remoteVersion < 70300)
		return;

	if (g_verbose)
		write_msg(NULL, "saving encoding\n");

	qry = createPQExpBuffer();

	appendPQExpBuffer(qry, "SHOW client_encoding");

	res = PQexec(g_conn, qry->data);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK ||
		PQntuples(res) != 1)
	{
		write_msg(NULL, "SQL command failed\n");
		write_msg(NULL, "Error message from server: %s", PQerrorMessage(g_conn));
		write_msg(NULL, "The command was: %s\n", qry->data);
		exit_nicely();
	}

	resetPQExpBuffer(qry);

	appendPQExpBuffer(qry, "SET client_encoding = ");
	appendStringLiteral(qry, PQgetvalue(res, 0, 0), true);
	appendPQExpBuffer(qry, ";\n");

	ArchiveEntry(AH, "0",		/* OID */
				 "ENCODING",	/* Name */
				 NULL,			/* Namespace */
				 "",			/* Owner */
				 "ENCODING",	/* Desc */
				 NULL,			/* Deps */
				 qry->data,		/* Create */
				 "",			/* Del */
				 NULL,			/* Copy */
				 NULL,			/* Dumper */
				 NULL);			/* Dumper Arg */

	PQclear(res);

	destroyPQExpBuffer(qry);
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
		write_msg(NULL, "query to obtain list of schemas failed: %s", PQerrorMessage(g_conn));
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
			write_msg(NULL, "WARNING: owner of schema \"%s\" appears to be invalid\n",
					  nsinfo[i].nspname);
	}

	/*
	 * If the user attempted to dump a specific namespace, check to ensure
	 * that the specified namespace actually exists.
	 */
	if (selectSchemaName)
	{
		for (i = 0; i < ntups; i++)
			if (strcmp(nsinfo[i].nspname, selectSchemaName) == 0)
				break;

		/* Didn't find a match */
		if (i == ntups)
		{
			write_msg(NULL, "specified schema \"%s\" does not exist\n",
					  selectSchemaName);
			exit_nicely();
		}
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
findNamespace(const char *nsoid, const char *objoid)
{
	int			i;

	if (g_fout->remoteVersion >= 70300)
	{
		for (i = 0; i < g_numNamespaces; i++)
		{
			NamespaceInfo *nsinfo = &g_namespaces[i];

			if (strcmp(nsoid, nsinfo->oid) == 0)
				return nsinfo;
		}
		write_msg(NULL, "schema with OID %s does not exist\n", nsoid);
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
	int			i_typrelkind;
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
						  "typelem, typrelid, "
						  "CASE WHEN typrelid = 0 THEN ' '::\"char\" "
						  "ELSE (SELECT relkind FROM pg_class WHERE oid = typrelid) END as typrelkind, "
						  "typtype, typisdefined "
						  "FROM pg_type");
	}
	else
	{
		appendPQExpBuffer(query, "SELECT pg_type.oid, typname, "
						  "0::oid as typnamespace, "
						  "(select usename from pg_user where typowner = usesysid) as usename, "
						  "typelem, typrelid, "
						  "CASE WHEN typrelid = 0 THEN ' '::\"char\" "
						  "ELSE (SELECT relkind FROM pg_class WHERE oid = typrelid) END as typrelkind, "
						  "typtype, typisdefined "
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
	i_typrelkind = PQfnumber(res, "typrelkind");
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
		tinfo[i].typrelkind = *PQgetvalue(res, i, i_typrelkind);
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
			write_msg(NULL, "WARNING: owner of data type \"%s\" appears to be invalid\n",
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
	 * find all operators, including builtin operators; we filter out
	 * system-defined operators at dump-out time.
	 */

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	if (g_fout->remoteVersion >= 70300)
	{
		appendPQExpBuffer(query, "SELECT pg_operator.oid, oprname, "
						  "oprnamespace, "
						  "(select usename from pg_user where oprowner = usesysid) as usename, "
						  "oprcode::oid as oprcode "
						  "from pg_operator");
	}
	else
	{
		appendPQExpBuffer(query, "SELECT pg_operator.oid, oprname, "
						  "0::oid as oprnamespace, "
						  "(select usename from pg_user where oprowner = usesysid) as usename, "
						  "oprcode::oid as oprcode "
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
	int			i_oid;
	int			i_opcname;
	int			i_opcnamespace;
	int			i_usename;

	/*
	 * find all opclasses, including builtin opclasses; we filter out
	 * system-defined opclasses at dump-out time.
	 */

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	if (g_fout->remoteVersion >= 70300)
	{
		appendPQExpBuffer(query, "SELECT pg_opclass.oid, opcname, "
						  "opcnamespace, "
						  "(select usename from pg_user where opcowner = usesysid) as usename "
						  "from pg_opclass");
	}
	else
	{
		appendPQExpBuffer(query, "SELECT pg_opclass.oid, opcname, "
						  "0::oid as opcnamespace, "
						  "''::name as usename "
						  "from pg_opclass");
	}

	res = PQexec(g_conn, query->data);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to obtain list of operator classes failed: %s", PQerrorMessage(g_conn));
		exit_nicely();
	}

	ntups = PQntuples(res);
	*numOpclasses = ntups;

	opcinfo = (OpclassInfo *) malloc(ntups * sizeof(OpclassInfo));

	i_oid = PQfnumber(res, "oid");
	i_opcname = PQfnumber(res, "opcname");
	i_opcnamespace = PQfnumber(res, "opcnamespace");
	i_usename = PQfnumber(res, "usename");

	for (i = 0; i < ntups; i++)
	{
		opcinfo[i].oid = strdup(PQgetvalue(res, i, i_oid));
		opcinfo[i].opcname = strdup(PQgetvalue(res, i, i_opcname));
		opcinfo[i].opcnamespace = findNamespace(PQgetvalue(res, i, i_opcnamespace),
												opcinfo[i].oid);
		opcinfo[i].usename = strdup(PQgetvalue(res, i, i_usename));

		if (g_fout->remoteVersion >= 70300)
		{
			if (strlen(opcinfo[i].usename) == 0)
				write_msg(NULL, "WARNING: owner of operator class \"%s\" appears to be invalid\n",
						  opcinfo[i].opcname);
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
		agginfo[i].anybasetype = false; /* computed when it's dumped */
		agginfo[i].fmtbasetype = NULL;	/* computed when it's dumped */
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

	finfo = (FuncInfo *) calloc(ntups, sizeof(FuncInfo));

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
	int			i_owning_tab;
	int			i_owning_col;

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	/*
	 * Find all the tables (including views and sequences).
	 *
	 * We include system catalogs, so that we can work if a user table is
	 * defined to inherit from a system catalog (pretty weird, but...)
	 *
	 * We ignore tables that are not type 'r' (ordinary relation) or 'S'
	 * (sequence) or 'v' (view).
	 *
	 * Note: in this phase we should collect only a minimal amount of
	 * information about each table, basically just enough to decide if it
	 * is interesting.	We must fetch all tables in this phase because
	 * otherwise we cannot correctly identify inherited columns, serial
	 * columns, etc.
	 */

	if (g_fout->remoteVersion >= 70300)
	{
		/*
		 * Left join to pick up dependency info linking sequences to their
		 * serial column, if any
		 */
		appendPQExpBuffer(query,
						  "SELECT c.oid, relname, relacl, relkind, "
						  "relnamespace, "
						  "(select usename from pg_user where relowner = usesysid) as usename, "
						  "relchecks, reltriggers, "
						  "relhasindex, relhasrules, relhasoids, "
						  "d.refobjid as owning_tab, "
						  "d.refobjsubid as owning_col "
						  "from pg_class c "
						  "left join pg_depend d on "
						  "(c.relkind = '%c' and "
						"d.classid = c.tableoid and d.objid = c.oid and "
						  "d.objsubid = 0 and "
						"d.refclassid = c.tableoid and d.deptype = 'i') "
						  "where relkind in ('%c', '%c', '%c') "
						  "order by c.oid",
						  RELKIND_SEQUENCE,
					   RELKIND_RELATION, RELKIND_SEQUENCE, RELKIND_VIEW);
	}
	else if (g_fout->remoteVersion >= 70200)
	{
		appendPQExpBuffer(query,
						"SELECT pg_class.oid, relname, relacl, relkind, "
						  "0::oid as relnamespace, "
						  "(select usename from pg_user where relowner = usesysid) as usename, "
						  "relchecks, reltriggers, "
						  "relhasindex, relhasrules, relhasoids, "
						  "NULL::oid as owning_tab, "
						  "NULL::int4 as owning_col "
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
						  "relhasindex, relhasrules, "
						  "'t'::bool as relhasoids, "
						  "NULL::oid as owning_tab, "
						  "NULL::int4 as owning_col "
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
						  "relhasindex, relhasrules, "
						  "'t'::bool as relhasoids, "
						  "NULL::oid as owning_tab, "
						  "NULL::int4 as owning_col "
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
	tblinfo = (TableInfo *) calloc(ntups, sizeof(TableInfo));

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
	i_owning_tab = PQfnumber(res, "owning_tab");
	i_owning_col = PQfnumber(res, "owning_col");

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
		if (PQgetisnull(res, i, i_owning_tab))
		{
			tblinfo[i].owning_tab = NULL;
			tblinfo[i].owning_col = 0;
		}
		else
		{
			tblinfo[i].owning_tab = strdup(PQgetvalue(res, i, i_owning_tab));
			tblinfo[i].owning_col = atoi(PQgetvalue(res, i, i_owning_col));
		}

		/* other fields were zeroed above */

		/*
		 * Decide whether we want to dump this table.  Sequences owned by
		 * serial columns are never dumpable on their own; we will
		 * transpose their owning table's dump flag to them below.
		 */
		if (tblinfo[i].owning_tab == NULL)
			selectDumpableTable(&tblinfo[i]);
		else
			tblinfo[i].dump = false;
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
				write_msg(NULL, "attempt to lock table \"%s\" failed: %s",
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

	/*
	 * If the user is attempting to dump a specific table, check to ensure
	 * that the specified table actually exists.  (This is a bit simplistic
	 * since we don't fully check the combination of -n and -t switches.)
	 */
	if (selectTableName)
	{
		for (i = 0; i < ntups; i++)
			if (strcmp(tblinfo[i].relname, selectTableName) == 0)
				break;

		/* Didn't find a match */
		if (i == ntups)
		{
			write_msg(NULL, "specified table \"%s\" does not exist\n",
					  selectTableName);
			exit_nicely();
		}
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
				j,
				k;
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
		selectSourceSchema(tbinfo->relnamespace->nspname);

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
			write_msg(NULL, "finding the columns and types of table \"%s\"\n",
					  tbinfo->relname);

		resetPQExpBuffer(q);

		if (g_fout->remoteVersion >= 70300)
		{
			/* need left join here to not fail on dropped columns ... */
			appendPQExpBuffer(q, "SELECT a.attnum, a.attname, a.atttypmod, a.attstattarget, a.attstorage, t.typstorage, "
			  "a.attnotnull, a.atthasdef, a.attisdropped, a.attislocal, "
			   "pg_catalog.format_type(t.oid,a.atttypmod) as atttypname "
							  "from pg_catalog.pg_attribute a left join pg_catalog.pg_type t "
							  "on a.atttypid = t.oid "
							  "where a.attrelid = '%s'::pg_catalog.oid "
							  "and a.attnum > 0::pg_catalog.int2 "
							  "order by a.attrelid, a.attnum",
							  tbinfo->oid);
		}
		else if (g_fout->remoteVersion >= 70100)
		{
			/*
			 * attstattarget doesn't exist in 7.1.  It does exist in 7.2,
			 * but we don't dump it because we can't tell whether it's
			 * been explicitly set or was just a default.
			 */
			appendPQExpBuffer(q, "SELECT a.attnum, a.attname, a.atttypmod, -1 as attstattarget, a.attstorage, t.typstorage, "
							  "a.attnotnull, a.atthasdef, false as attisdropped, null as attislocal, "
						  "format_type(t.oid,a.atttypmod) as atttypname "
							  "from pg_attribute a left join pg_type t "
							  "on a.atttypid = t.oid "
							  "where a.attrelid = '%s'::oid "
							  "and a.attnum > 0::int2 "
							  "order by a.attrelid, a.attnum",
							  tbinfo->oid);
		}
		else
		{
			/* format_type not available before 7.1 */
			appendPQExpBuffer(q, "SELECT attnum, attname, atttypmod, -1 as attstattarget, attstorage, attstorage as typstorage, "
							  "attnotnull, atthasdef, false as attisdropped, null as attislocal, "
							  "(select typname from pg_type where oid = atttypid) as atttypname "
							  "from pg_attribute a "
							  "where attrelid = '%s'::oid "
							  "and attnum > 0::int2 "
							  "order by attrelid, attnum",
							  tbinfo->oid);
		}

		res = PQexec(g_conn, q->data);
		if (!res ||
			PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			write_msg(NULL, "query to get table columns failed: %s", PQerrorMessage(g_conn));
			exit_nicely();
		}

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
		tbinfo->attisserial = (bool *) malloc(ntups * sizeof(bool));
		tbinfo->notnull = (bool *) malloc(ntups * sizeof(bool));
		tbinfo->adef_expr = (char **) malloc(ntups * sizeof(char *));
		tbinfo->inhAttrs = (bool *) malloc(ntups * sizeof(bool));
		tbinfo->inhAttrDef = (bool *) malloc(ntups * sizeof(bool));
		tbinfo->inhNotNull = (bool *) malloc(ntups * sizeof(bool));
		hasdefaults = false;

		for (j = 0; j < ntups; j++)
		{
			if (j + 1 != atoi(PQgetvalue(res, j, i_attnum)))
			{
				write_msg(NULL, "invalid column numbering in table \"%s\"\n",
						  tbinfo->relname);
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
			tbinfo->attisserial[j] = false;		/* fix below */
			tbinfo->notnull[j] = (PQgetvalue(res, j, i_attnotnull)[0] == 't');
			tbinfo->adef_expr[j] = NULL;		/* fix below */
			if (PQgetvalue(res, j, i_atthasdef)[0] == 't')
				hasdefaults = true;
			/* these flags will be set in flagInhAttrs() */
			tbinfo->inhAttrs[j] = false;
			tbinfo->inhAttrDef[j] = false;
			tbinfo->inhNotNull[j] = false;
		}

		PQclear(res);

		if (hasdefaults)
		{
			int			numDefaults;

			if (g_verbose)
				write_msg(NULL, "finding default expressions of table \"%s\"\n",
						  tbinfo->relname);

			resetPQExpBuffer(q);
			if (g_fout->remoteVersion >= 70300)
			{
				appendPQExpBuffer(q, "SELECT adnum, "
					   "pg_catalog.pg_get_expr(adbin, adrelid) AS adsrc "
								  "FROM pg_catalog.pg_attrdef "
								  "WHERE adrelid = '%s'::pg_catalog.oid",
								  tbinfo->oid);
			}
			else if (g_fout->remoteVersion >= 70200)
			{
				appendPQExpBuffer(q, "SELECT adnum, "
								  "pg_get_expr(adbin, adrelid) AS adsrc "
								  "FROM pg_attrdef "
								  "WHERE adrelid = '%s'::oid",
								  tbinfo->oid);
			}
			else
			{
				/* no pg_get_expr, so must rely on adsrc */
				appendPQExpBuffer(q, "SELECT adnum, adsrc FROM pg_attrdef "
								  "WHERE adrelid = '%s'::oid",
								  tbinfo->oid);
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
				int			adnum = atoi(PQgetvalue(res, j, 0));

				if (adnum <= 0 || adnum > ntups)
				{
					write_msg(NULL, "invalid adnum value %d for table \"%s\"\n",
							  adnum, tbinfo->relname);
					exit_nicely();
				}
				tbinfo->adef_expr[adnum - 1] = strdup(PQgetvalue(res, j, 1));
			}
			PQclear(res);
		}

		/*
		 * Check to see if any columns are serial columns.	Our first
		 * quick filter is that it must be integer or bigint with a
		 * default.  If so, we scan to see if we found a sequence linked
		 * to this column. If we did, mark the column and sequence
		 * appropriately.
		 */
		for (j = 0; j < ntups; j++)
		{
			/*
			 * Note assumption that format_type will show these types as
			 * exactly "integer" and "bigint" regardless of schema path.
			 * This is correct in 7.3 but needs to be watched.
			 */
			if (strcmp(tbinfo->atttypnames[j], "integer") != 0 &&
				strcmp(tbinfo->atttypnames[j], "bigint") != 0)
				continue;
			if (tbinfo->adef_expr[j] == NULL)
				continue;
			for (k = 0; k < numTables; k++)
			{
				TableInfo  *seqinfo = &tblinfo[k];

				if (seqinfo->owning_tab != NULL &&
					strcmp(seqinfo->owning_tab, tbinfo->oid) == 0 &&
					seqinfo->owning_col == j + 1)
				{
					/*
					 * Found a match.  Copy the table's interesting and
					 * dumpable flags to the sequence.
					 */
					tbinfo->attisserial[j] = true;
					seqinfo->interesting = tbinfo->interesting;
					seqinfo->dump = tbinfo->dump;
					break;
				}
			}
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

	/* Build query to find comment */

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

	/* Execute query */

	res = PQexec(g_conn, query->data);
	if (!res || PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to get comment on OID %s failed: %s",
				  oid, PQerrorMessage(g_conn));
		exit_nicely();
	}

	/* If a comment exists, build COMMENT ON statement */

	if (PQntuples(res) == 1)
	{
		i_description = PQfnumber(res, "description");
		resetPQExpBuffer(query);
		appendPQExpBuffer(query, "COMMENT ON %s IS ", target);
		appendStringLiteral(query, PQgetvalue(res, 0, i_description), false);
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

	/* Build query to find comments */

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

	/* Execute query */

	res = PQexec(g_conn, query->data);
	if (!res || PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to get comments on table %s failed: %s",
				  tbinfo->relname, PQerrorMessage(g_conn));
		exit_nicely();
	}
	i_description = PQfnumber(res, "description");
	i_objsubid = PQfnumber(res, "objsubid");

	/* If comments exist, build COMMENT ON statements */

	ntups = PQntuples(res);
	for (i = 0; i < ntups; i++)
	{
		const char *descr = PQgetvalue(res, i, i_description);
		int			objsubid = atoi(PQgetvalue(res, i, i_objsubid));

		if (objsubid == 0)
		{
			resetPQExpBuffer(target);
			appendPQExpBuffer(target, "%s %s", reltypename,
							  fmtId(tbinfo->relname));

			resetPQExpBuffer(query);
			appendPQExpBuffer(query, "COMMENT ON %s IS ", target->data);
			appendStringLiteral(query, descr, false);
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
							  fmtId(tbinfo->relname));
			appendPQExpBuffer(target, "%s",
							  fmtId(tbinfo->attnames[objsubid - 1]));

			resetPQExpBuffer(query);
			appendPQExpBuffer(query, "COMMENT ON %s IS ", target->data);
			appendStringLiteral(query, descr, false);
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

	/* Build query to find comment */

	query = createPQExpBuffer();
	appendPQExpBuffer(query, "SELECT oid FROM pg_database WHERE datname = ");
	appendStringLiteral(query, PQdb(g_conn), true);

	/* Execute query */

	res = PQexec(g_conn, query->data);
	if (!res || PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to get database OID failed: %s",
				  PQerrorMessage(g_conn));
		exit_nicely();
	}

	/* If a comment exists, build COMMENT ON statement */

	if (PQntuples(res) != 0)
	{
		i_oid = PQfnumber(res, "oid");
		resetPQExpBuffer(query);
		appendPQExpBuffer(query, "DATABASE %s", fmtId(PQdb(g_conn)));
		dumpComment(fout, query->data, NULL, "",
					PQgetvalue(res, 0, i_oid), "pg_database", 0, NULL);
	}

	PQclear(res);
	destroyPQExpBuffer(query);
}

/*
 * dumpNamespaces
 *	  writes out to fout the queries to recreate user-defined namespaces
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

		qnspname = strdup(fmtId(nspinfo->nspname));

		/*
		 * If it's the PUBLIC namespace, don't emit a CREATE SCHEMA record
		 * for it, since we expect PUBLIC to exist already in the
		 * destination database.  But do emit ACL in case it's not standard,
		 * likewise comment.
		 *
		 * Note that ownership is shown in the AUTHORIZATION clause,
		 * while the archive entry is listed with empty owner (causing
		 * it to be emitted with SET SESSION AUTHORIZATION DEFAULT).
		 * This seems the best way of dealing with schemas owned by
		 * users without CREATE SCHEMA privilege.  Further hacking has
		 * to be applied for --no-owner mode, though!
		 */
		if (strcmp(nspinfo->nspname, "public") != 0)
		{
			resetPQExpBuffer(q);
			resetPQExpBuffer(delq);

			appendPQExpBuffer(delq, "DROP SCHEMA %s;\n", qnspname);

			appendPQExpBuffer(q, "CREATE SCHEMA %s AUTHORIZATION %s;\n",
							  qnspname, fmtId(nspinfo->usename));

			ArchiveEntry(fout, nspinfo->oid, nspinfo->nspname,
						 NULL, "", "SCHEMA", NULL,
						 q->data, delq->data, NULL, NULL, NULL);
		}

		/* Dump Schema Comments */
		resetPQExpBuffer(q);
		appendPQExpBuffer(q, "SCHEMA %s", qnspname);
		dumpComment(fout, q->data,
					NULL, nspinfo->usename,
					nspinfo->oid, "pg_namespace", 0, NULL);

		dumpACL(fout, "SCHEMA", qnspname, nspinfo->nspname, NULL,
				nspinfo->usename, nspinfo->nspacl,
				nspinfo->oid);

		free(qnspname);
	}

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
}

/*
 * dumpOneBaseType
 *	  writes out to fout the queries to recreate a user-defined base type
 *	  as requested by dumpTypes
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
	if (fout->remoteVersion >= 70400)
	{
		appendPQExpBuffer(query, "SELECT typlen, "
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
	else if (fout->remoteVersion >= 70300)
	{
		appendPQExpBuffer(query, "SELECT typlen, "
						  "typinput, typoutput, "
						  "'-' as typreceive, '-' as typsend, "
						  "typinput::pg_catalog.oid as typinputoid, "
						  "typoutput::pg_catalog.oid as typoutputoid, "
						  "0 as typreceiveoid, 0 as typsendoid, "
						  "typdelim, typdefault, typbyval, typalign, "
						  "typstorage "
						  "FROM pg_catalog.pg_type "
						  "WHERE oid = '%s'::pg_catalog.oid",
						  tinfo->oid);
	}
	else if (fout->remoteVersion >= 70100)
	{
		/*
		 * Note: although pre-7.3 catalogs contain typreceive and typsend,
		 * ignore them because they are not right.
		 */
		appendPQExpBuffer(query, "SELECT typlen, "
						  "typinput, typoutput, "
						  "'-' as typreceive, '-' as typsend, "
						  "typinput::oid as typinputoid, "
						  "typoutput::oid as typoutputoid, "
						  "0 as typreceiveoid, 0 as typsendoid, "
						  "typdelim, typdefault, typbyval, typalign, "
						  "typstorage "
						  "FROM pg_type "
						  "WHERE oid = '%s'::oid",
						  tinfo->oid);
	}
	else
	{
		appendPQExpBuffer(query, "SELECT typlen, "
						  "typinput, typoutput, "
						  "'-' as typreceive, '-' as typsend, "
						  "typinput::oid as typinputoid, "
						  "typoutput::oid as typoutputoid, "
						  "0 as typreceiveoid, 0 as typsendoid, "
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
		write_msg(NULL, "query to obtain information on data type \"%s\" failed: %s",
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
	 * sure there are dependency entries for this.	But don't include
	 * dependencies if the functions aren't going to be dumped.
	 */
	funcInd = findFuncByOid(g_finfo, numFuncs, typinputoid);
	if (funcInd >= 0 && g_finfo[funcInd].pronamespace->dump)
		(*deps)[depIdx++] = strdup(typinputoid);

	funcInd = findFuncByOid(g_finfo, numFuncs, typoutputoid);
	if (funcInd >= 0 && g_finfo[funcInd].pronamespace->dump)
		(*deps)[depIdx++] = strdup(typoutputoid);

	if (strcmp(typreceiveoid, "0") != 0)
	{
		funcInd = findFuncByOid(g_finfo, numFuncs, typreceiveoid);
		if (funcInd >= 0 && g_finfo[funcInd].pronamespace->dump)
			(*deps)[depIdx++] = strdup(typreceiveoid);
	}

	if (strcmp(typsendoid, "0") != 0)
	{
		funcInd = findFuncByOid(g_finfo, numFuncs, typsendoid);
		if (funcInd >= 0 && g_finfo[funcInd].pronamespace->dump)
			(*deps)[depIdx++] = strdup(typsendoid);
	}

	/*
	 * DROP must be fully qualified in case same name appears in
	 * pg_catalog
	 */
	appendPQExpBuffer(delq, "DROP TYPE %s.",
					  fmtId(tinfo->typnamespace->nspname));
	appendPQExpBuffer(delq, "%s CASCADE;\n",
					  fmtId(tinfo->typname));

	appendPQExpBuffer(q,
					  "CREATE TYPE %s (\n"
					  "    INTERNALLENGTH = %s",
					  fmtId(tinfo->typname),
					  (strcmp(typlen, "-1") == 0) ? "variable" : typlen);

	if (fout->remoteVersion >= 70300)
	{
		/* regproc result is correctly quoted in 7.3 */
		appendPQExpBuffer(q, ",\n    INPUT = %s", typinput);
		appendPQExpBuffer(q, ",\n    OUTPUT = %s", typoutput);
		if (strcmp(typreceiveoid, "0") != 0)
			appendPQExpBuffer(q, ",\n    RECEIVE = %s", typreceive);
		if (strcmp(typsendoid, "0") != 0)
			appendPQExpBuffer(q, ",\n    SEND = %s", typsend);
	}
	else
	{
		/* regproc delivers an unquoted name before 7.3 */
		/* cannot combine these because fmtId uses static result area */
		appendPQExpBuffer(q, ",\n    INPUT = %s", fmtId(typinput));
		appendPQExpBuffer(q, ",\n    OUTPUT = %s", fmtId(typoutput));
		/* no chance that receive/send need be printed */
	}

	if (typdefault != NULL)
	{
		appendPQExpBuffer(q, ",\n    DEFAULT = ");
		appendStringLiteral(q, typdefault, true);
	}

	if (tinfo->isArray)
	{
		char	   *elemType;

		/* reselect schema in case changed by function dump */
		selectSourceSchema(tinfo->typnamespace->nspname);
		elemType = getFormattedTypeName(tinfo->typelem, zeroAsOpaque);
		appendPQExpBuffer(q, ",\n    ELEMENT = %s", elemType);
		free(elemType);

		(*deps)[depIdx++] = strdup(tinfo->typelem);
	}

	if (typdelim && strcmp(typdelim, ",") != 0)
	{
		appendPQExpBuffer(q, ",\n    DELIMITER = ");
		appendStringLiteral(q, typdelim, true);
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

	(*deps)[depIdx++] = NULL;	/* End of List */

	ArchiveEntry(fout, tinfo->oid, tinfo->typname,
				 tinfo->typnamespace->nspname,
				 tinfo->usename, "TYPE", deps,
				 q->data, delq->data, NULL, NULL, NULL);

	/* Dump Type Comments */
	resetPQExpBuffer(q);

	appendPQExpBuffer(q, "TYPE %s", fmtId(tinfo->typname));
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
 *	  writes out to fout the queries to recreate a user-defined domain
 *	  as requested by dumpTypes
 */
static void
dumpOneDomain(Archive *fout, TypeInfo *tinfo)
{
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	PQExpBuffer query = createPQExpBuffer();
	PQExpBuffer chkquery = createPQExpBuffer();
	PGresult   *res;
	int			ntups;
	int			i;
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

	/*
	 * DROP must be fully qualified in case same name appears in
	 * pg_catalog
	 */
	appendPQExpBuffer(delq, "DROP DOMAIN %s.",
					  fmtId(tinfo->typnamespace->nspname));
	appendPQExpBuffer(delq, "%s;\n",
					  fmtId(tinfo->typname));

	appendPQExpBuffer(q,
					  "CREATE DOMAIN %s AS %s",
					  fmtId(tinfo->typname),
					  typdefn);

	/* Depends on the base type */
	(*deps)[depIdx++] = strdup(typbasetype);

	if (typnotnull[0] == 't')
		appendPQExpBuffer(q, " NOT NULL");

	if (typdefault)
		appendPQExpBuffer(q, " DEFAULT %s", typdefault);

	PQclear(res);

	/*
	 * Fetch and process CHECK constraints for the domain
	 */
	if (g_fout->remoteVersion >= 70400)
		appendPQExpBuffer(chkquery, "SELECT conname, "
						"pg_catalog.pg_get_constraintdef(oid) AS consrc "
						  "FROM pg_catalog.pg_constraint "
						  "WHERE contypid = '%s'::pg_catalog.oid",
						  tinfo->oid);
	else
		appendPQExpBuffer(chkquery, "SELECT conname, 'CHECK (' || consrc || ')' AS consrc "
						  "FROM pg_catalog.pg_constraint "
						  "WHERE contypid = '%s'::pg_catalog.oid",
						  tinfo->oid);

	res = PQexec(g_conn, chkquery->data);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to obtain domain constraint information failed: %s",
				  PQerrorMessage(g_conn));
		exit_nicely();
	}

	ntups = PQntuples(res);
	for (i = 0; i < ntups; i++)
	{
		char	   *conname;
		char	   *consrc;

		conname = PQgetvalue(res, i, PQfnumber(res, "conname"));
		consrc = PQgetvalue(res, i, PQfnumber(res, "consrc"));

		appendPQExpBuffer(q, "\n\tCONSTRAINT %s %s",
						  fmtId(conname), consrc);
	}

	appendPQExpBuffer(q, ";\n");

	(*deps)[depIdx++] = NULL;	/* End of List */

	ArchiveEntry(fout, tinfo->oid, tinfo->typname,
				 tinfo->typnamespace->nspname,
				 tinfo->usename, "DOMAIN", deps,
				 q->data, delq->data, NULL, NULL, NULL);

	/* Dump Domain Comments */
	resetPQExpBuffer(q);

	appendPQExpBuffer(q, "DOMAIN %s", fmtId(tinfo->typname));
	dumpComment(fout, q->data,
				tinfo->typnamespace->nspname, tinfo->usename,
				tinfo->oid, "pg_type", 0, NULL);

	PQclear(res);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(query);
	destroyPQExpBuffer(chkquery);
}

/*
 * dumpOneCompositeType
 *	  writes out to fout the queries to recreate a user-defined stand-alone
 *	  composite type as requested by dumpTypes
 */
static void
dumpOneCompositeType(Archive *fout, TypeInfo *tinfo)
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
	selectSourceSchema(tinfo->typnamespace->nspname);

	/* Fetch type specific details */
	/* We assume here that remoteVersion must be at least 70300 */

	appendPQExpBuffer(query, "SELECT a.attname, "
		 "pg_catalog.format_type(a.atttypid, a.atttypmod) as atttypdefn "
				  "FROM pg_catalog.pg_type t, pg_catalog.pg_attribute a "
					  "WHERE t.oid = '%s'::pg_catalog.oid "
					  "AND a.attrelid = t.typrelid "
					  "AND NOT a.attisdropped "
					  "ORDER BY a.attnum ",
					  tinfo->oid);

	res = PQexec(g_conn, query->data);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to obtain data type information failed: %s", PQerrorMessage(g_conn));
		exit_nicely();
	}

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
					  fmtId(tinfo->typname));

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
	 * DROP must be fully qualified in case same name appears in
	 * pg_catalog
	 */
	appendPQExpBuffer(delq, "DROP TYPE %s.",
					  fmtId(tinfo->typnamespace->nspname));
	appendPQExpBuffer(delq, "%s;\n",
					  fmtId(tinfo->typname));

	ArchiveEntry(fout, tinfo->oid, tinfo->typname,
				 tinfo->typnamespace->nspname,
				 tinfo->usename, "TYPE", NULL,
				 q->data, delq->data, NULL, NULL, NULL);

	/* Dump Type Comments */
	resetPQExpBuffer(q);

	appendPQExpBuffer(q, "TYPE %s", fmtId(tinfo->typname));
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

		/* skip complex types, except for standalone composite types */
		if (atooid(tinfo[i].typrelid) != 0 && tinfo[i].typrelkind != 'c')
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
		else if (tinfo[i].typtype == 'c')
			dumpOneCompositeType(fout, &tinfo[i]);
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
	int			i_lanacl = -1;
	char	   *lanoid;
	char	   *lanname;
	bool	    lanpltrusted;
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
		lanpltrusted = (PQgetvalue(res, i, i_lanpltrusted)[0] == 't');
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

		/*
		 * Current theory is to dump PLs iff their underlying functions
		 * will be dumped (are in a dumpable namespace, or have a
		 * non-system OID in pre-7.3 databases).  Actually, we treat the
		 * PL itself as being in the underlying function's namespace,
		 * though it isn't really.  This avoids searchpath problems for
		 * the HANDLER clause.
		 *
		 * If the underlying function is in the pg_catalog namespace,
		 * we won't have loaded it into finfo[] at all; therefore,
		 * treat failure to find it in finfo[] as indicating we shouldn't
		 * dump it, not as an error condition.  Ditto for the validator.
		 */

		fidx = findFuncByOid(finfo, numFuncs, lanplcallfoid);
		if (fidx < 0)
			continue;

		if (!finfo[fidx].pronamespace->dump)
			continue;

		if (strcmp(lanvalidator, "0") != 0)
		{
			vidx = findFuncByOid(finfo, numFuncs, lanvalidator);
			if (vidx < 0)
				continue;
		}

		resetPQExpBuffer(defqry);
		resetPQExpBuffer(delqry);

		/* Make a dependency to ensure function is dumped first */
		deps = malloc(sizeof(char *) * 10);
		depIdx = 0;

		(*deps)[depIdx++] = strdup(lanplcallfoid);

		appendPQExpBuffer(delqry, "DROP PROCEDURAL LANGUAGE %s;\n",
						  fmtId(lanname));

		appendPQExpBuffer(defqry, "CREATE %sPROCEDURAL LANGUAGE %s",
						  lanpltrusted ?
						  "TRUSTED " : "",
						  fmtId(lanname));
		appendPQExpBuffer(defqry, " HANDLER %s",
						  fmtId(finfo[fidx].proname));
		if (strcmp(lanvalidator, "0") != 0)
		{
			appendPQExpBuffer(defqry, " VALIDATOR ");
			/* Cope with possibility that validator is in different schema */
			if (finfo[vidx].pronamespace != finfo[fidx].pronamespace)
				appendPQExpBuffer(defqry, "%s.",
							   fmtId(finfo[vidx].pronamespace->nspname));
			appendPQExpBuffer(defqry, "%s",
							  fmtId(finfo[vidx].proname));
			(*deps)[depIdx++] = strdup(lanvalidator);
		}
		appendPQExpBuffer(defqry, ";\n");

		(*deps)[depIdx++] = NULL;		/* End of List */

		ArchiveEntry(fout, lanoid, lanname,
					 finfo[fidx].pronamespace->nspname, "",
					 "PROCEDURAL LANGUAGE", deps,
					 defqry->data, delqry->data, NULL, NULL, NULL);

		if (!aclsSkip && lanpltrusted)
		{
			char	   *tmp = strdup(fmtId(lanname));

			dumpACL(fout, "ACL LANGUAGE", tmp, lanname,
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
		appendPQExpBuffer(&fn, "%s(", fmtId(finfo->proname));
	else
		appendPQExpBuffer(&fn, "%s(", finfo->proname);
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


static void
dumpFuncACL(Archive *fout, FuncInfo *finfo)
{
	char	   *funcsig,
			   *funcsig_tag;

	funcsig = format_function_signature(finfo, true);
	funcsig_tag = format_function_signature(finfo, false);
	dumpACL(fout, "FUNCTION", funcsig, funcsig_tag,
			finfo->pronamespace->nspname,
			finfo->usename, finfo->proacl, finfo->oid);
	free(funcsig);
	free(funcsig_tag);
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
	char	   *funcsig_tag = NULL;
	int			ntups;
	char	   *proretset;
	char	   *prosrc;
	char	   *probin;
	char	   *provolatile;
	char	   *proisstrict;
	char	   *prosecdef;
	char	   *lanname;
	char	   *rettypename;
	char	   *funcproclang;	/* Boolean : is this function a PLang
								 * handler ? */

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
						  "provolatile, proisstrict, prosecdef, "
						  "(SELECT lanname FROM pg_catalog.pg_language WHERE oid = prolang) as lanname, "
						  "exists (SELECT 'x' FROM pg_catalog.pg_language WHERE lanplcallfoid = pg_catalog.pg_proc.oid) as funcproclang "
						  "FROM pg_catalog.pg_proc "
						  "WHERE oid = '%s'::pg_catalog.oid",
						  finfo->oid);
	}
	else if (g_fout->remoteVersion >= 70100)
	{
		appendPQExpBuffer(query,
						  "SELECT proretset, prosrc, probin, "
		 "case when proiscachable then 'i' else 'v' end as provolatile, "
						  "proisstrict, "
						  "'f'::boolean as prosecdef, "
						  "(SELECT lanname FROM pg_language WHERE oid = prolang) as lanname, "
						  "exists (SELECT 'x' FROM pg_language WHERE lanplcallfoid = pg_proc.oid) as funcproclang "
						  "FROM pg_proc "
						  "WHERE oid = '%s'::oid",
						  finfo->oid);
	}
	else
	{
		appendPQExpBuffer(query,
						  "SELECT proretset, prosrc, probin, "
		 "case when proiscachable then 'i' else 'v' end as provolatile, "
						  "'f'::boolean as proisstrict, "
						  "'f'::boolean as prosecdef, "
						  "(SELECT lanname FROM pg_language WHERE oid = prolang) as lanname, "
						  "exists (SELECT 'x' FROM pg_language WHERE lanplcallfoid = pg_proc.oid) as funcproclang "
						  "FROM pg_proc "
						  "WHERE oid = '%s'::oid",
						  finfo->oid);
	}

	res = PQexec(g_conn, query->data);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to obtain information on function \"%s\" failed: %s",
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
	proisstrict = PQgetvalue(res, 0, PQfnumber(res, "proisstrict"));
	prosecdef = PQgetvalue(res, 0, PQfnumber(res, "prosecdef"));
	lanname = PQgetvalue(res, 0, PQfnumber(res, "lanname"));
	funcproclang = PQgetvalue(res, 0, PQfnumber(res, "funcproclang"));

	/*
	 * See backend/commands/define.c for details of how the 'AS' clause is
	 * used.
	 */
	if (strcmp(probin, "-") != 0)
	{
		appendPQExpBuffer(asPart, "AS ");
		appendStringLiteral(asPart, probin, true);
		if (strcmp(prosrc, "-") != 0)
		{
			appendPQExpBuffer(asPart, ", ");
			appendStringLiteral(asPart, prosrc, false);
		}
	}
	else
	{
		if (strcmp(prosrc, "-") != 0)
		{
			appendPQExpBuffer(asPart, "AS ");
			appendStringLiteral(asPart, prosrc, false);
		}
	}

	funcsig = format_function_signature(finfo, true);
	funcsig_tag = format_function_signature(finfo, false);

	/*
	 * DROP must be fully qualified in case same name appears in
	 * pg_catalog
	 */
	appendPQExpBuffer(delqry, "DROP FUNCTION %s.%s;\n",
					  fmtId(finfo->pronamespace->nspname),
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
					  finfo->proname);
			exit_nicely();
		}
	}

	if (proisstrict[0] == 't')
		appendPQExpBuffer(q, " STRICT");

	if (prosecdef[0] == 't')
		appendPQExpBuffer(q, " SECURITY DEFINER");

	appendPQExpBuffer(q, ";\n");

	ArchiveEntry(fout, finfo->oid, funcsig_tag,
				 finfo->pronamespace->nspname,
				 finfo->usename, strcmp(funcproclang, "t") ? "FUNCTION" : "FUNC PROCEDURAL LANGUAGE", NULL,
				 q->data, delqry->data,
				 NULL, NULL, NULL);

	/* Dump Function Comments */

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
	free(funcsig_tag);
}


/*
 * Dump all casts
 */
void
dumpCasts(Archive *fout,
		  FuncInfo *finfo, int numFuncs,
		  TypeInfo *tinfo, int numTypes)
{
	PGresult   *res;
	PQExpBuffer query = createPQExpBuffer();
	PQExpBuffer defqry = createPQExpBuffer();
	PQExpBuffer delqry = createPQExpBuffer();
	PQExpBuffer castsig = createPQExpBuffer();
	int			ntups;
	int			i;

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	if (fout->remoteVersion >= 70300)
		appendPQExpBuffer(query, "SELECT oid, castsource, casttarget, castfunc, castcontext FROM pg_cast ORDER BY 1,2,3;");
	else
		appendPQExpBuffer(query, "SELECT p.oid, t1.oid, t2.oid, p.oid, true FROM pg_type t1, pg_type t2, pg_proc p WHERE p.pronargs = 1 AND p.proargtypes[0] = t1.oid AND p.prorettype = t2.oid AND p.proname = t2.typname ORDER BY 1,2,3;");

	res = PQexec(g_conn, query->data);
	if (!res || PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to obtain list of casts failed: %s",
				  PQerrorMessage(g_conn));
		exit_nicely();
	}
	ntups = PQntuples(res);

	for (i = 0; i < ntups; i++)
	{
		char	   *castoid = PQgetvalue(res, i, 0);
		char	   *castsource = PQgetvalue(res, i, 1);
		char	   *casttarget = PQgetvalue(res, i, 2);
		char	   *castfunc = PQgetvalue(res, i, 3);
		char	   *castcontext = PQgetvalue(res, i, 4);
		int			fidx = -1;
		const char *((*deps)[]);
		int			source_idx;
		int			target_idx;

		if (strcmp(castfunc, "0") != 0)
			fidx = findFuncByOid(finfo, numFuncs, castfunc);

		/*
		 * As per discussion we dump casts if one or more of the underlying
		 * objects (the conversion function and the two data types) are not
		 * builtin AND if all of the non-builtin objects namespaces are
		 * included in the dump. Builtin meaning, the namespace name does
		 * not start with "pg_".
		 */
		source_idx = findTypeByOid(tinfo, numTypes, castsource);
		target_idx = findTypeByOid(tinfo, numTypes, casttarget);

		/*
		 * Skip this cast if all objects are from pg_
		 */
		if ((fidx < 0 || strncmp(finfo[fidx].pronamespace->nspname, "pg_", 3) == 0) &&
				strncmp(tinfo[source_idx].typnamespace->nspname, "pg_", 3) == 0 &&
				strncmp(tinfo[target_idx].typnamespace->nspname, "pg_", 3) == 0)
			continue;

		/*
		 * Skip cast if function isn't from pg_ and that namespace is
		 * not dumped.
		 */
		if (fidx >= 0 && 
				strncmp(finfo[fidx].pronamespace->nspname, "pg_", 3) != 0 &&
				!finfo[fidx].pronamespace->dump)
			continue;

		/*
		 * Same for the Source type
		 */
		if (strncmp(tinfo[source_idx].typnamespace->nspname, "pg_", 3) != 0 &&
				!tinfo[source_idx].typnamespace->dump)
			continue;

		/*
		 * and the target type.
		 */
		if (strncmp(tinfo[target_idx].typnamespace->nspname, "pg_", 3) != 0 &&
				!tinfo[target_idx].typnamespace->dump)
			continue;

		/* Make a dependency to ensure function is dumped first */
		if (fidx >= 0)
		{
			deps = malloc(sizeof(char *) * 2);

			(*deps)[0] = strdup(castfunc);
			(*deps)[1] = NULL;	/* End of List */
		}
		else
			deps = NULL;

		resetPQExpBuffer(defqry);
		resetPQExpBuffer(delqry);
		resetPQExpBuffer(castsig);

		appendPQExpBuffer(delqry, "DROP CAST (%s AS %s);\n",
						  getFormattedTypeName(castsource, zeroAsNone),
						  getFormattedTypeName(casttarget, zeroAsNone));

		appendPQExpBuffer(defqry, "CREATE CAST (%s AS %s) ",
						  getFormattedTypeName(castsource, zeroAsNone),
						  getFormattedTypeName(casttarget, zeroAsNone));

		if (strcmp(castfunc, "0") == 0)
			appendPQExpBuffer(defqry, "WITHOUT FUNCTION");
		else
		{
			/*
			 * Always qualify the function name, in case it is not in
			 * pg_catalog schema (format_function_signature won't qualify it).
			 */
			appendPQExpBuffer(defqry, "WITH FUNCTION %s.",
							  fmtId(finfo[fidx].pronamespace->nspname));
			appendPQExpBuffer(defqry, "%s",
							  format_function_signature(&finfo[fidx], true));
		}

		if (strcmp(castcontext, "a") == 0)
			appendPQExpBuffer(defqry, " AS ASSIGNMENT");
		else if (strcmp(castcontext, "i") == 0)
			appendPQExpBuffer(defqry, " AS IMPLICIT");
		appendPQExpBuffer(defqry, ";\n");

		appendPQExpBuffer(castsig, "CAST (%s AS %s)",
						  getFormattedTypeName(castsource, zeroAsNone),
						  getFormattedTypeName(casttarget, zeroAsNone));

		ArchiveEntry(fout, castoid,
					 castsig->data,
					 tinfo[source_idx].typnamespace->nspname, "",
					 "CAST", deps,
					 defqry->data, delqry->data,
					 NULL, NULL, NULL);
	}

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(defqry);
	destroyPQExpBuffer(delqry);
	destroyPQExpBuffer(castsig);
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

	appendPQExpBuffer(details, "    PROCEDURE = %s",
					  convertRegProcReference(oprcode));

	appendPQExpBuffer(oprid, "%s (",
					  oprinfo->oprname);

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

	name = convertOperatorReference(oprcom, g_oprinfo, numOperators);
	if (name)
		appendPQExpBuffer(details, ",\n    COMMUTATOR = %s", name);

	name = convertOperatorReference(oprnegate, g_oprinfo, numOperators);
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

	name = convertOperatorReference(oprlsortop, g_oprinfo, numOperators);
	if (name)
		appendPQExpBuffer(details, ",\n    SORT1 = %s", name);

	name = convertOperatorReference(oprrsortop, g_oprinfo, numOperators);
	if (name)
		appendPQExpBuffer(details, ",\n    SORT2 = %s", name);

	name = convertOperatorReference(oprltcmpop, g_oprinfo, numOperators);
	if (name)
		appendPQExpBuffer(details, ",\n    LTCMP = %s", name);

	name = convertOperatorReference(oprgtcmpop, g_oprinfo, numOperators);
	if (name)
		appendPQExpBuffer(details, ",\n    GTCMP = %s", name);

	/*
	 * DROP must be fully qualified in case same name appears in
	 * pg_catalog
	 */
	appendPQExpBuffer(delq, "DROP OPERATOR %s.%s;\n",
					  fmtId(oprinfo->oprnamespace->nspname),
					  oprid->data);

	appendPQExpBuffer(q, "CREATE OPERATOR %s (\n%s\n);\n",
					  oprinfo->oprname, details->data);

	ArchiveEntry(fout, oprinfo->oid, oprinfo->oprname,
				 oprinfo->oprnamespace->nspname, oprinfo->usename,
				 "OPERATOR", NULL,
				 q->data, delq->data,
				 NULL, NULL, NULL);

	/* Dump Operator Comments */

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
convertOperatorReference(const char *opr,
						 OprInfo *g_oprinfo, int numOperators)
{
	char	   *name;

	/* In all cases "0" means a null reference */
	if (strcmp(opr, "0") == 0)
		return NULL;

	if (g_fout->remoteVersion >= 70300)
	{
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

	name = findOprByOid(g_oprinfo, numOperators, opr);
	if (name == NULL)
		write_msg(NULL, "WARNING: could not find operator with OID %s\n",
				  opr);
	return name;
}


/*
 * dumpOpclasses
 *	  writes out to fout the queries to recreate all the user-defined
 *	  operator classes
 */
void
dumpOpclasses(Archive *fout, OpclassInfo *opcinfo, int numOpclasses)
{
	int			i;

	for (i = 0; i < numOpclasses; i++)
	{
		/* Dump only opclasses in dumpable namespaces */
		if (!opcinfo[i].opcnamespace->dump)
			continue;

		/* OK, dump it */
		dumpOneOpclass(fout, &opcinfo[i]);
	}
}

/*
 * dumpOneOpclass
 *	  write out a single operator class definition
 */
static void
dumpOneOpclass(Archive *fout, OpclassInfo *opcinfo)
{
	PQExpBuffer query = createPQExpBuffer();
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
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

	/*
	 * XXX currently we do not implement dumping of operator classes from
	 * pre-7.3 databases.  This could be done but it seems not worth the
	 * trouble.
	 */
	if (g_fout->remoteVersion < 70300)
		return;

	/* Make sure we are in proper schema so regoperator works correctly */
	selectSourceSchema(opcinfo->opcnamespace->nspname);

	/* Get additional fields from the pg_opclass row */
	appendPQExpBuffer(query, "SELECT opcintype::pg_catalog.regtype, "
					  "opckeytype::pg_catalog.regtype, "
					  "opcdefault, "
	"(SELECT amname FROM pg_catalog.pg_am WHERE oid = opcamid) AS amname "
					  "FROM pg_catalog.pg_opclass "
					  "WHERE oid = '%s'::pg_catalog.oid",
					  opcinfo->oid);

	res = PQexec(g_conn, query->data);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to obtain operator class details failed: %s", PQerrorMessage(g_conn));
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

	i_opcintype = PQfnumber(res, "opcintype");
	i_opckeytype = PQfnumber(res, "opckeytype");
	i_opcdefault = PQfnumber(res, "opcdefault");
	i_amname = PQfnumber(res, "amname");

	opcintype = PQgetvalue(res, 0, i_opcintype);
	opckeytype = PQgetvalue(res, 0, i_opckeytype);
	opcdefault = PQgetvalue(res, 0, i_opcdefault);
	amname = PQgetvalue(res, 0, i_amname);

	/*
	 * DROP must be fully qualified in case same name appears in
	 * pg_catalog
	 */
	appendPQExpBuffer(delq, "DROP OPERATOR CLASS %s",
					  fmtId(opcinfo->opcnamespace->nspname));
	appendPQExpBuffer(delq, ".%s",
					  fmtId(opcinfo->opcname));
	appendPQExpBuffer(delq, " USING %s;\n",
					  fmtId(amname));

	/* Build the fixed portion of the CREATE command */
	appendPQExpBuffer(q, "CREATE OPERATOR CLASS %s\n    ",
					  fmtId(opcinfo->opcname));
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
					  "WHERE amopclaid = '%s'::pg_catalog.oid "
					  "ORDER BY amopstrategy",
					  opcinfo->oid);

	res = PQexec(g_conn, query->data);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to obtain operator class operators failed: %s", PQerrorMessage(g_conn));
		exit_nicely();
	}

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
					  "WHERE amopclaid = '%s'::pg_catalog.oid "
					  "ORDER BY amprocnum",
					  opcinfo->oid);

	res = PQexec(g_conn, query->data);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to obtain operator class functions failed: %s", PQerrorMessage(g_conn));
		exit_nicely();
	}

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

	ArchiveEntry(fout, opcinfo->oid, opcinfo->opcname,
				 opcinfo->opcnamespace->nspname, opcinfo->usename,
				 "OPERATOR CLASS", NULL,
				 q->data, delq->data,
				 NULL, NULL, NULL);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
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

	initPQExpBuffer(&buf);
	if (honor_quotes)
		appendPQExpBuffer(&buf, "%s",
						  fmtId(agginfo->aggname));
	else
		appendPQExpBuffer(&buf, "%s", agginfo->aggname);

	/* If using regtype or format_type, fmtbasetype is already quoted */
	if (fout->remoteVersion >= 70100)
	{
		if (agginfo->anybasetype)
			appendPQExpBuffer(&buf, "(*)");
		else
			appendPQExpBuffer(&buf, "(%s)", agginfo->fmtbasetype);
	}
	else
	{
		if (agginfo->anybasetype)
			appendPQExpBuffer(&buf, "(*)");
		else
			appendPQExpBuffer(&buf, "(%s)",
							  fmtId(agginfo->fmtbasetype));
	}

	return buf.data;
}


static void
dumpAggACL(Archive *fout, AggInfo *finfo)
{
	char	   *aggsig,
			   *aggsig_tag;

	aggsig = format_aggregate_signature(finfo, fout, true);
	aggsig_tag = format_aggregate_signature(finfo, fout, false);
	dumpACL(fout, "FUNCTION", aggsig, aggsig_tag,
			finfo->aggnamespace->nspname,
			finfo->usename, finfo->aggacl, finfo->oid);
	free(aggsig);
	free(aggsig_tag);
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
	char	   *aggsig_tag;
	PGresult   *res;
	int			ntups;
	int			i_aggtransfn;
	int			i_aggfinalfn;
	int			i_aggtranstype;
	int			i_agginitval;
	int			i_anybasetype;
	int			i_fmtbasetype;
	int			i_convertok;
	const char *aggtransfn;
	const char *aggfinalfn;
	const char *aggtranstype;
	const char *agginitval;
	bool		convertok;

	/* Make sure we are in proper schema */
	selectSourceSchema(agginfo->aggnamespace->nspname);

	/* Get aggregate-specific details */
	if (g_fout->remoteVersion >= 70300)
	{
		appendPQExpBuffer(query, "SELECT aggtransfn, "
						  "aggfinalfn, aggtranstype::pg_catalog.regtype, "
						  "agginitval, "
						  "proargtypes[0] = 'pg_catalog.\"any\"'::pg_catalog.regtype as anybasetype, "
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
						  "aggbasetype = 0 as anybasetype, "
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
						  "aggbasetype = 0 as anybasetype, "
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
	i_anybasetype = PQfnumber(res, "anybasetype");
	i_fmtbasetype = PQfnumber(res, "fmtbasetype");
	i_convertok = PQfnumber(res, "convertok");

	aggtransfn = PQgetvalue(res, 0, i_aggtransfn);
	aggfinalfn = PQgetvalue(res, 0, i_aggfinalfn);
	aggtranstype = PQgetvalue(res, 0, i_aggtranstype);
	agginitval = PQgetvalue(res, 0, i_agginitval);
	/* we save anybasetype so that dumpAggACL can use it later */
	agginfo->anybasetype = (PQgetvalue(res, 0, i_anybasetype)[0] == 't');
	/* we save fmtbasetype so that dumpAggACL can use it later */
	agginfo->fmtbasetype = strdup(PQgetvalue(res, 0, i_fmtbasetype));
	convertok = (PQgetvalue(res, 0, i_convertok)[0] == 't');

	aggsig = format_aggregate_signature(agginfo, g_fout, true);
	aggsig_tag = format_aggregate_signature(agginfo, g_fout, false);

	if (!convertok)
	{
		write_msg(NULL, "WARNING: aggregate function %s could not be dumped correctly for this database version; ignored\n",
				  aggsig);

		appendPQExpBuffer(q, "-- WARNING: aggregate function %s could not be dumped correctly for this database version; ignored\n",
						  aggsig);
		ArchiveEntry(fout, agginfo->oid, aggsig_tag,
					 agginfo->aggnamespace->nspname, agginfo->usename,
					 "WARNING", NULL,
					 q->data, "" /* Del */ ,
					 NULL, NULL, NULL);
		return;
	}

	if (g_fout->remoteVersion >= 70300)
	{
		/* If using 7.3's regproc or regtype, data is already quoted */
		appendPQExpBuffer(details, "    BASETYPE = %s,\n    SFUNC = %s,\n    STYPE = %s",
						  agginfo->anybasetype ? "'any'" :
						  agginfo->fmtbasetype,
						  aggtransfn,
						  aggtranstype);
	}
	else if (g_fout->remoteVersion >= 70100)
	{
		/* format_type quotes, regproc does not */
		appendPQExpBuffer(details, "    BASETYPE = %s,\n    SFUNC = %s,\n    STYPE = %s",
						  agginfo->anybasetype ? "'any'" :
						  agginfo->fmtbasetype,
						  fmtId(aggtransfn),
						  aggtranstype);
	}
	else
	{
		/* need quotes all around */
		appendPQExpBuffer(details, "    BASETYPE = %s,\n",
						  agginfo->anybasetype ? "'any'" :
						  fmtId(agginfo->fmtbasetype));
		appendPQExpBuffer(details, "    SFUNC = %s,\n",
						  fmtId(aggtransfn));
		appendPQExpBuffer(details, "    STYPE = %s",
						  fmtId(aggtranstype));
	}

	if (!PQgetisnull(res, 0, i_agginitval))
	{
		appendPQExpBuffer(details, ",\n    INITCOND = ");
		appendStringLiteral(details, agginitval, true);
	}

	if (strcmp(aggfinalfn, "-") != 0)
	{
		appendPQExpBuffer(details, ",\n    FINALFUNC = %s",
						  aggfinalfn);
	}

	/*
	 * DROP must be fully qualified in case same name appears in
	 * pg_catalog
	 */
	appendPQExpBuffer(delq, "DROP AGGREGATE %s.%s;\n",
					  fmtId(agginfo->aggnamespace->nspname),
					  aggsig);

	appendPQExpBuffer(q, "CREATE AGGREGATE %s (\n%s\n);\n",
					  fmtId(agginfo->aggname),
					  details->data);

	ArchiveEntry(fout, agginfo->oid, aggsig_tag,
				 agginfo->aggnamespace->nspname, agginfo->usename,
				 "AGGREGATE", NULL,
				 q->data, delq->data,
				 NULL, NULL, NULL);

	/* Dump Aggregate Comments */

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
	free(aggsig_tag);
}


/*----------
 * Write out grant/revoke information
 *
 * 'type' must be TABLE, FUNCTION, LANGUAGE, or SCHEMA.
 * 'name' is the formatted name of the object.	Must be quoted etc. already.
 * 'tag' is the tag for the archive entry (typ. unquoted name of object).
 * 'nspname' is the namespace the object is in (NULL if none).
 * 'owner' is the owner, NULL if there is no owner (for languages).
 * 'acls' is the string read out of the fooacl system catalog field;
 * it will be parsed here.
 * 'objoid' is the OID of the object for purposes of ordering.
 *----------
 */
static void
dumpACL(Archive *fout, const char *type, const char *name,
		const char *tag, const char *nspname, const char *owner,
		const char *acls, const char *objoid)
{
	PQExpBuffer sql;

	/*
	 * acl_lang is a flag only true if we are dumping language's ACL, so
	 * we can set 'type' to a value that is suitable to build SQL requests
	 * as for other types.
	 */
	bool		acl_lang = false;

	if (!strcmp(type, "ACL LANGUAGE"))
	{
		type = "LANGUAGE";
		acl_lang = true;
	}

	sql = createPQExpBuffer();

	if (!buildACLCommands(name, type, acls, owner, fout->remoteVersion, sql))
	{
		write_msg(NULL, "could not parse ACL list (%s) for object \"%s\" (%s)\n",
				  acls, name, type);
		exit_nicely();
	}

	if (sql->len > 0)
		ArchiveEntry(fout, objoid, tag, nspname,
					 owner ? owner : "",
					 acl_lang ? "ACL LANGUAGE" : "ACL",
					 NULL, sql->data, "", NULL, NULL, NULL);

	destroyPQExpBuffer(sql);
}


static void
dumpTableACL(Archive *fout, TableInfo *tbinfo)
{
	char	   *namecopy = strdup(fmtId(tbinfo->relname));
	char	   *dumpoid;

	/*
	 * Choose OID to use for sorting ACL into position.  For a view, sort
	 * by the view OID; for a serial sequence, sort by the owning table's
	 * OID; otherwise use the table's own OID.
	 */
	if (tbinfo->viewoid != NULL)
		dumpoid = tbinfo->viewoid;
	else if (tbinfo->owning_tab != NULL)
		dumpoid = tbinfo->owning_tab;
	else
		dumpoid = tbinfo->oid;

	dumpACL(fout, "TABLE", namecopy, tbinfo->relname,
		  tbinfo->relnamespace->nspname, tbinfo->usename, tbinfo->relacl,
			dumpoid);

	free(namecopy);
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

	/*
	 * Dump non-serial sequences first, in case they are referenced in
	 * table defn's
	 */
	for (i = 0; i < numTables; i++)
	{
		TableInfo  *tbinfo = &tblinfo[i];

		if (tbinfo->relkind != RELKIND_SEQUENCE)
			continue;

		if (tbinfo->dump && tbinfo->owning_tab == NULL)
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
			TableInfo  *tbinfo = &tblinfo[i];

			if (tbinfo->relkind == RELKIND_SEQUENCE)	/* already dumped */
				continue;

			if (tbinfo->dump)
			{
				dumpOneTable(fout, tbinfo, tblinfo);
				if (!aclsSkip)
					dumpTableACL(fout, tbinfo);
			}
		}
	}

	/*
	 * Dump serial sequences last (we will not emit any CREATE commands,
	 * but we do have to think about ACLs and setval operations).
	 */
	for (i = 0; i < numTables; i++)
	{
		TableInfo  *tbinfo = &tblinfo[i];

		if (tbinfo->relkind != RELKIND_SEQUENCE)
			continue;

		if (tbinfo->dump && tbinfo->owning_tab != NULL)
		{
			dumpOneSequence(fout, tbinfo, schemaOnly, dataOnly);
			if (!dataOnly && !aclsSkip)
				dumpTableACL(fout, tbinfo);
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
	char	   *storage;
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
			appendStringLiteral(query, tbinfo->relname, true);
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
			write_msg(NULL, "query to obtain definition of view \"%s\" returned null OID\n",
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

		/*
		 * DROP must be fully qualified in case same name appears in
		 * pg_catalog
		 */
		appendPQExpBuffer(delq, "DROP VIEW %s.",
						  fmtId(tbinfo->relnamespace->nspname));
		appendPQExpBuffer(delq, "%s;\n",
						  fmtId(tbinfo->relname));

		appendPQExpBuffer(q, "CREATE VIEW %s AS\n    %s\n",
						  fmtId(tbinfo->relname), viewdef);

		PQclear(res);

		/*
		 * Views can have default values -- however, they must be
		 * specified in an ALTER TABLE command after the view has been
		 * created, not in the view definition itself.
		 */
		for (j = 0; j < tbinfo->numatts; j++)
		{
			if (tbinfo->adef_expr[j] != NULL && !tbinfo->inhAttrDef[j])
			{
				appendPQExpBuffer(q, "ALTER TABLE %s ",
								  fmtId(tbinfo->relname));
				appendPQExpBuffer(q, "ALTER COLUMN %s SET DEFAULT %s;\n",
								  fmtId(tbinfo->attnames[j]),
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

		/*
		 * DROP must be fully qualified in case same name appears in
		 * pg_catalog
		 */
		appendPQExpBuffer(delq, "DROP TABLE %s.",
						  fmtId(tbinfo->relnamespace->nspname));
		appendPQExpBuffer(delq, "%s;\n",
						  fmtId(tbinfo->relname));

		appendPQExpBuffer(q, "CREATE TABLE %s (",
						  fmtId(tbinfo->relname));
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
					char	   *typname = tbinfo->atttypnames[j];

					if (tbinfo->attisserial[j])
					{
						if (strcmp(typname, "integer") == 0)
							typname = "serial";
						else if (strcmp(typname, "bigint") == 0)
							typname = "bigserial";
					}
					appendPQExpBuffer(q, "%s", typname);
				}
				else
				{
					/* If no format_type, fake it */
					appendPQExpBuffer(q, "%s",
									  myFormatType(tbinfo->atttypnames[j],
												   tbinfo->atttypmod[j]));
				}

				/* Default value --- suppress if inherited or serial */
				if (tbinfo->adef_expr[j] != NULL &&
					!tbinfo->inhAttrDef[j] &&
					!tbinfo->attisserial[j])
					appendPQExpBuffer(q, " DEFAULT %s",
									  tbinfo->adef_expr[j]);

				/*
				 * Not Null constraint --- suppress if inherited
				 *
				 * Note: we could suppress this for serial columns since
				 * SERIAL implies NOT NULL.  We choose not to for forward
				 * compatibility, since there has been some talk of making
				 * SERIAL not imply NOT NULL, in which case the explicit
				 * specification would be needed.
				 */
				if (tbinfo->notnull[j] && !tbinfo->inhNotNull[j])
					appendPQExpBuffer(q, " NOT NULL");

				actual_atts++;
			}
		}

		/*
		 * Add non-inherited CHECK constraints, if any. If a constraint
		 * matches by name and condition with a constraint belonging to a
		 * parent class (OR conditions match and both names start with
		 * '$'), we assume it was inherited.
		 */
		if (tbinfo->ncheck > 0)
		{
			PGresult   *res2;
			int			i_conname,
						i_consrc;
			int			ntups2;

			if (g_verbose)
				write_msg(NULL, "finding check constraints for table \"%s\"\n",
						  tbinfo->relname);

			resetPQExpBuffer(query);
			if (g_fout->remoteVersion >= 70400)
				appendPQExpBuffer(query, "SELECT conname, "
					" pg_catalog.pg_get_constraintdef(c1.oid) AS consrc "
								  " from pg_catalog.pg_constraint c1 "
								" where conrelid = '%s'::pg_catalog.oid "
								  "   and contype = 'c' "
								  "   and not exists "
								  "  (select 1 from "
								  "    pg_catalog.pg_constraint c2, "
								  "    pg_catalog.pg_inherits i "
								  "    where i.inhrelid = c1.conrelid "
								  "      and (c2.conname = c1.conname "
								  "          or (c2.conname[0] = '$' "
								  "              and c1.conname[0] = '$')"
								  "          )"
					 "      and pg_catalog.pg_get_constraintdef(c2.oid) "
								  "		     = pg_catalog.pg_get_constraintdef(c1.oid) "
								  "      and c2.conrelid = i.inhparent) "
								  " order by conname ",
								  tbinfo->oid);
			else if (g_fout->remoteVersion >= 70300)
				appendPQExpBuffer(query, "SELECT conname, "
								  " 'CHECK (' || consrc || ')' AS consrc"
								  " from pg_catalog.pg_constraint c1"
								" where conrelid = '%s'::pg_catalog.oid "
								  "   and contype = 'c' "
								  "   and not exists "
								  "  (select 1 from "
								  "    pg_catalog.pg_constraint c2, "
								  "    pg_catalog.pg_inherits i "
								  "    where i.inhrelid = c1.conrelid "
								  "      and (c2.conname = c1.conname "
								  "          or (c2.conname[0] = '$' "
								  "              and c1.conname[0] = '$')"
								  "          )"
								  "      and c2.consrc = c1.consrc "
								  "      and c2.conrelid = i.inhparent) "
								  " order by conname ",
								  tbinfo->oid);
			else
				appendPQExpBuffer(query, "SELECT rcname as conname,"
								  " 'CHECK (' || rcsrc || ')' as consrc"
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

			i_conname = PQfnumber(res2, "conname");
			i_consrc = PQfnumber(res2, "consrc");

			for (j = 0; j < ntups2; j++)
			{
				const char *name = PQgetvalue(res2, j, i_conname);
				const char *expr = PQgetvalue(res2, j, i_consrc);

				if (actual_atts + j > 0)
					appendPQExpBuffer(q, ",\n    ");

				appendPQExpBuffer(q, "CONSTRAINT %s ",
								  fmtId(name));
				appendPQExpBuffer(q, "%s", expr);
			}
			PQclear(res2);
		}

		/*
		 * Primary Key: In versions of PostgreSQL prior to 7.2, we needed
		 * to include the primary key in the table definition. However,
		 * this is not ideal because it creates an index on the table,
		 * which makes COPY slower. As of release 7.2, we can add primary
		 * keys to a table after it has been created, using ALTER TABLE;
		 * see dumpIndexes() for more information. Therefore, we ignore
		 * primary keys in this function.
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
								fmtId(parentRel->relnamespace->nspname));
				appendPQExpBuffer(q, "%s",
								  fmtId(parentRel->relname));
			}
			appendPQExpBuffer(q, ")");
		}

		if (!tbinfo->hasoids)
			appendPQExpBuffer(q, " WITHOUT OIDS");

		appendPQExpBuffer(q, ";\n");

		/* Loop dumping statistics and storage statements */
		for (j = 0; j < tbinfo->numatts; j++)
		{
			/*
			 * Dump per-column statistics information. We only issue an
			 * ALTER TABLE statement if the attstattarget entry for this
			 * column is non-negative (i.e. it's not the default value)
			 */
			if (tbinfo->attstattarget[j] >= 0 &&
				!tbinfo->attisdropped[j])
			{
				appendPQExpBuffer(q, "ALTER TABLE ONLY %s ",
								  fmtId(tbinfo->relname));
				appendPQExpBuffer(q, "ALTER COLUMN %s ",
								  fmtId(tbinfo->attnames[j]));
				appendPQExpBuffer(q, "SET STATISTICS %d;\n",
								  tbinfo->attstattarget[j]);
			}

			/*
			 * Dump per-column storage information.  The statement is only
			 * dumped if the storage has been changed from the type's
			 * default.
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
				 * Only dump the statement if it's a storage type we
				 * recognize
				 */
				if (storage != NULL)
				{
					appendPQExpBuffer(q, "ALTER TABLE ONLY %s ",
									  fmtId(tbinfo->relname));
					appendPQExpBuffer(q, "ALTER COLUMN %s ",
									  fmtId(tbinfo->attnames[j]));
					appendPQExpBuffer(q, "SET STORAGE %s;\n",
									  storage);
				}
			}
		}
	}

	ArchiveEntry(fout, objoid, tbinfo->relname,
				 tbinfo->relnamespace->nspname, tbinfo->usename,
				 reltypename, NULL, q->data, delq->data,
				 NULL, NULL, NULL);

	/* Dump Table Comments */
	dumpTableComment(fout, tbinfo, reltypename, commentDeps);

	/* commentDeps now belongs to the archive entry ... don't free it! */

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
	write_msg(NULL, "invalid column number %d for table \"%s\"\n",
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
	int			i_contype;
	int			i_conoid;
	int			i_indkey;
	int			i_indisclustered;
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

		/*
		 * The point of the messy-looking outer join is to find a
		 * constraint that is related by an internal dependency link to
		 * the index. If we find one, we emit an ADD CONSTRAINT command
		 * instead of a CREATE INDEX command.  We assume an index won't
		 * have more than one internal dependency.
		 */
		resetPQExpBuffer(query);
		if (g_fout->remoteVersion >= 70300)
			appendPQExpBuffer(query,
							  "SELECT i.indexrelid as indexreloid, "
					   "coalesce(c.conname, t.relname) as indexrelname, "
				 "pg_catalog.pg_get_indexdef(i.indexrelid) as indexdef, "
							  "i.indkey, i.indisclustered, "
							  "t.relnatts as indnkeys, "
							  "coalesce(c.contype, '0') as contype, "
							  "coalesce(c.oid, '0') as conoid "
							  "FROM pg_catalog.pg_index i "
				  "JOIN pg_catalog.pg_class t ON (t.oid = i.indexrelid) "
							  "LEFT JOIN pg_catalog.pg_depend d "
							  "ON (d.classid = t.tableoid "
							  "AND d.objid = t.oid "
							  "AND d.deptype = 'i') "
							  "LEFT JOIN pg_catalog.pg_constraint c "
							  "ON (d.refclassid = c.tableoid "
							  "AND d.refobjid = c.oid) "
							  "WHERE i.indrelid = '%s'::pg_catalog.oid "
							  "ORDER BY indexrelname",
							  tbinfo->oid);
		else
			appendPQExpBuffer(query,
							  "SELECT i.indexrelid as indexreloid, "
							  "t.relname as indexrelname, "
							"pg_get_indexdef(i.indexrelid) as indexdef, "
							  "i.indkey, false as indisclustered, "
							  "t.relnatts as indnkeys, "
							  "CASE WHEN i.indisprimary THEN 'p'::char "
							  "ELSE '0'::char END as contype, "
							  "0::oid as conoid "
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
		i_contype = PQfnumber(res, "contype");
		i_conoid = PQfnumber(res, "conoid");
		i_indkey = PQfnumber(res, "indkey");
		i_indisclustered = PQfnumber(res, "indisclustered");
		i_indnkeys = PQfnumber(res, "indnkeys");

		for (j = 0; j < ntups; j++)
		{
			const char *indexreloid = PQgetvalue(res, j, i_indexreloid);
			const char *indexrelname = PQgetvalue(res, j, i_indexrelname);
			const char *indexdef = PQgetvalue(res, j, i_indexdef);
			char		contype = *(PQgetvalue(res, j, i_contype));
			const char *conoid = PQgetvalue(res, j, i_conoid);
			bool		indisclustered = (PQgetvalue(res, j, i_indisclustered)[0] == 't');

			resetPQExpBuffer(q);
			resetPQExpBuffer(delq);

			if (contype == 'p' || contype == 'u')
			{
				/*
				 * If we found a constraint matching the index, emit ADD
				 * CONSTRAINT not CREATE INDEX.
				 *
				 * In a pre-7.3 database, we take this path iff the index was
				 * marked indisprimary.
				 */
				int			indnkeys = atoi(PQgetvalue(res, j, i_indnkeys));
				char	  **indkeys = (char **) malloc(indnkeys * sizeof(char *));
				int			k;

				parseNumericArray(PQgetvalue(res, j, i_indkey),
								  indkeys, indnkeys);

				appendPQExpBuffer(q, "ALTER TABLE ONLY %s\n",
								  fmtId(tbinfo->relname));
				appendPQExpBuffer(q, "    ADD CONSTRAINT %s %s (",
								  fmtId(indexrelname),
							  contype == 'p' ? "PRIMARY KEY" : "UNIQUE");

				for (k = 0; k < indnkeys; k++)
				{
					int			indkey = atoi(indkeys[k]);
					const char *attname;

					if (indkey == InvalidAttrNumber)
						break;
					attname = getAttrName(indkey, tbinfo);

					appendPQExpBuffer(q, "%s%s",
									  (k == 0) ? "" : ", ",
									  fmtId(attname));
				}

				appendPQExpBuffer(q, ");\n");

				/* If the index is clustered, we need to record that. */
				if (indisclustered)
				{
					appendPQExpBuffer(q, "\nALTER TABLE %s CLUSTER",
									  fmtId(tbinfo->relname));
					appendPQExpBuffer(q, " ON %s;\n",
									  fmtId(indexrelname));
				}

				/*
				 * DROP must be fully qualified in case same name appears
				 * in pg_catalog
				 */
				appendPQExpBuffer(delq, "ALTER TABLE ONLY %s.",
								  fmtId(tbinfo->relnamespace->nspname));
				appendPQExpBuffer(delq, "%s ",
								  fmtId(tbinfo->relname));
				appendPQExpBuffer(delq, "DROP CONSTRAINT %s;\n",
								  fmtId(indexrelname));

				ArchiveEntry(fout, indexreloid,
							 indexrelname,
							 tbinfo->relnamespace->nspname,
							 tbinfo->usename,
							 "CONSTRAINT", NULL,
							 q->data, delq->data,
							 NULL, NULL, NULL);

				for (k = 0; k < indnkeys; k++)
					free(indkeys[k]);
				free(indkeys);

				/* Dump Constraint Comments */
				resetPQExpBuffer(q);
				appendPQExpBuffer(q, "CONSTRAINT %s ",
								  fmtId(indexrelname));
				appendPQExpBuffer(q, "ON %s",
								  fmtId(tbinfo->relname));
				dumpComment(fout, q->data,
							tbinfo->relnamespace->nspname,
							tbinfo->usename,
							conoid, "pg_constraint", 0, NULL);
			}
			else
			{
				/* Plain secondary index */
				appendPQExpBuffer(q, "%s;\n", indexdef);

				/* If the index is clustered, we need to record that. */
				if (indisclustered)
				{
					appendPQExpBuffer(q, "\nALTER TABLE %s CLUSTER",
									  fmtId(tbinfo->relname));
					appendPQExpBuffer(q, " ON %s;\n",
									  fmtId(indexrelname));
				}

				/*
				 * DROP must be fully qualified in case same name appears
				 * in pg_catalog
				 */
				appendPQExpBuffer(delq, "DROP INDEX %s.",
								  fmtId(tbinfo->relnamespace->nspname));
				appendPQExpBuffer(delq, "%s;\n",
								  fmtId(indexrelname));

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
							  fmtId(indexrelname));
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
		write_msg(NULL, "inserted invalid OID\n");
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
		write_msg(NULL, "maximum system OID is %u\n", max_oid);
	snprintf(sql, sizeof(sql),
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
	appendStringLiteral(query, dbname, true);

	res = PQexec(g_conn, query->data);
	if (res == NULL ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "error in finding the last system OID: %s", PQerrorMessage(g_conn));
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
 *
 * For 7.0, we do this by assuming that the last thing that initdb does is to
 * create the pg_indexes view.  This sucks in general, but seeing that 7.0.x
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
	if (res == NULL ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "error in finding the template1 database: %s", PQerrorMessage(g_conn));
		exit_nicely();
	}
	ntups = PQntuples(res);
	if (ntups < 1)
	{
		write_msg(NULL, "could not find entry for database template1 in table pg_database\n");
		exit_nicely();
	}
	if (ntups > 1)
	{
		write_msg(NULL, "found more than one entry for database template1 in table pg_database\n");
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
	selectSourceSchema(tbinfo->relnamespace->nspname);

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
					  fmtId(tbinfo->relname));

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
	 * Add a basic CREATE SEQUENCE statement (use last_val for start if
	 * called is false, else use min_val for start_val).  Skip this if the
	 * sequence came from a SERIAL column.
	 *
	 * Add a 'SETVAL(seq, last_val, iscalled)' at restore-time iff we load
	 * data.  We do this for serial sequences too.
	 */

	if (!dataOnly && tbinfo->owning_tab == NULL)
	{
		resetPQExpBuffer(delqry);

		/*
		 * DROP must be fully qualified in case same name appears in
		 * pg_catalog
		 */
		appendPQExpBuffer(delqry, "DROP SEQUENCE %s.",
						  fmtId(tbinfo->relnamespace->nspname));
		appendPQExpBuffer(delqry, "%s;\n",
						  fmtId(tbinfo->relname));

		resetPQExpBuffer(query);
		appendPQExpBuffer(query,
						  "CREATE SEQUENCE %s\n",
						  fmtId(tbinfo->relname));

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
						  "    CACHE %s%s;\n",
						  cache, (cycled ? "\n    CYCLE" : ""));

		ArchiveEntry(fout, tbinfo->oid, tbinfo->relname,
					 tbinfo->relnamespace->nspname, tbinfo->usename,
					 "SEQUENCE", NULL,
					 query->data, delqry->data,
					 NULL, NULL, NULL);
	}

	if (!schemaOnly)
	{
		resetPQExpBuffer(query);
		appendPQExpBuffer(query, "SELECT pg_catalog.setval(");
		appendStringLiteral(query, fmtId(tbinfo->relname), true);
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
		appendPQExpBuffer(query, "SEQUENCE %s", fmtId(tbinfo->relname));
		dumpComment(fout, query->data,
					tbinfo->relnamespace->nspname, tbinfo->usename,
					tbinfo->oid, "pg_class", 0, NULL);
	}

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(delqry);
}

/*
 * dumpConstraints
 *
 * Dump out constraints after all table creation statements in
 * an alter table format.  Currently handles foreign keys only.
 * (Unique and primary key constraints are handled with indexes,
 * while check constraints are merged into the table definition.)
 *
 * XXX Potentially wrap in a 'SET CONSTRAINTS OFF' block so that
 * the current table data is not processed
 */
static void
dumpConstraints(Archive *fout, TableInfo *tblinfo, int numTables)
{
	int			i,
				j;
	PQExpBuffer query;
	PQExpBuffer delqry;
	PGresult   *res;
	int			i_condef,
				i_conoid,
				i_conname;
	int			ntups;

	/* pg_constraint was created in 7.3, so nothing to do if older */
	if (g_fout->remoteVersion < 70300)
		return;

	query = createPQExpBuffer();
	delqry = createPQExpBuffer();

	for (i = 0; i < numTables; i++)
	{
		TableInfo  *tbinfo = &tblinfo[i];

		if (tbinfo->ntrig == 0 || !tbinfo->dump)
			continue;

		if (g_verbose)
			write_msg(NULL, "dumping foreign key constraints for table \"%s\"\n",
					  tbinfo->relname);

		/*
		 * select table schema to ensure regproc name is qualified if
		 * needed
		 */
		selectSourceSchema(tbinfo->relnamespace->nspname);

		resetPQExpBuffer(query);
		appendPQExpBuffer(query,
						  "SELECT oid, conname, "
						"pg_catalog.pg_get_constraintdef(oid) as condef "
						  "FROM pg_catalog.pg_constraint "
						  "WHERE conrelid = '%s'::pg_catalog.oid "
						  "AND contype = 'f'",
						  tbinfo->oid);
		res = PQexec(g_conn, query->data);
		if (!res ||
			PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			write_msg(NULL, "query to obtain list of foreign key definitions failed: %s", PQerrorMessage(g_conn));
			exit_nicely();
		}
		ntups = PQntuples(res);

		i_conoid = PQfnumber(res, "oid");
		i_conname = PQfnumber(res, "conname");
		i_condef = PQfnumber(res, "condef");

		for (j = 0; j < ntups; j++)
		{
			const char *conOid = PQgetvalue(res, j, i_conoid);
			const char *conName = PQgetvalue(res, j, i_conname);
			const char *conDef = PQgetvalue(res, j, i_condef);

			resetPQExpBuffer(query);
			appendPQExpBuffer(query, "ALTER TABLE ONLY %s\n",
							  fmtId(tbinfo->relname));
			appendPQExpBuffer(query, "    ADD CONSTRAINT %s %s;\n",
							  fmtId(conName),
							  conDef);

			/*
			 * DROP must be fully qualified in case same name appears in
			 * pg_catalog
			 */
			resetPQExpBuffer(delqry);
			appendPQExpBuffer(delqry, "ALTER TABLE ONLY %s.",
							  fmtId(tbinfo->relnamespace->nspname));
			appendPQExpBuffer(delqry, "%s ",
							  fmtId(tbinfo->relname));
			appendPQExpBuffer(delqry, "DROP CONSTRAINT %s;\n",
							  fmtId(conName));

			ArchiveEntry(fout, conOid,
						 conName,
						 tbinfo->relnamespace->nspname,
						 tbinfo->usename,
						 "FK CONSTRAINT", NULL,
						 query->data, delqry->data,
						 NULL, NULL, NULL);

			resetPQExpBuffer(query);
			appendPQExpBuffer(query, "CONSTRAINT %s ",
							  fmtId(conName));
			appendPQExpBuffer(query, "ON %s",
							  fmtId(tbinfo->relname));

			dumpComment(fout, query->data,
						tbinfo->relnamespace->nspname, tbinfo->usename,
						conOid, "pg_constraint", 0, NULL);
		}

		PQclear(res);
	}

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
		TableInfo  *tbinfo = &tblinfo[i];

		if (tbinfo->ntrig == 0 || !tbinfo->dump)
			continue;

		if (g_verbose)
			write_msg(NULL, "dumping triggers for table \"%s\"\n",
					  tbinfo->relname);

		/*
		 * select table schema to ensure regproc name is qualified if
		 * needed
		 */
		selectSourceSchema(tbinfo->relnamespace->nspname);

		resetPQExpBuffer(query);
		if (g_fout->remoteVersion >= 70300)
		{
			/*
			 * We ignore triggers that are tied to a foreign-key
			 * constraint
			 */
			appendPQExpBuffer(query,
							  "SELECT tgname, "
							  "tgfoid::pg_catalog.regproc as tgfname, "
							  "tgtype, tgnargs, tgargs, "
						   "tgisconstraint, tgconstrname, tgdeferrable, "
							  "tgconstrrelid, tginitdeferred, oid, "
				 "tgconstrrelid::pg_catalog.regclass as tgconstrrelname "
							  "from pg_catalog.pg_trigger t "
							  "where tgrelid = '%s'::pg_catalog.oid "
							  "and (not tgisconstraint "
							  " OR NOT EXISTS"
							  "  (SELECT 1 FROM pg_catalog.pg_depend d "
							  "   JOIN pg_catalog.pg_constraint c ON (d.refclassid = c.tableoid AND d.refobjid = c.oid) "
							  "   WHERE d.classid = t.tableoid AND d.objid = t.oid AND d.deptype = 'i' AND c.contype = 'f'))",
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

		/*
		 * We may have less triggers than recorded due to constraint
		 * triggers which are dumped by dumpConstraints
		 */
		if (ntups > tbinfo->ntrig)
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

			/*
			 * DROP must be fully qualified in case same name appears in
			 * pg_catalog
			 */
			appendPQExpBuffer(delqry, "DROP TRIGGER %s ",
							  fmtId(tgname));
			appendPQExpBuffer(delqry, "ON %s.",
							  fmtId(tbinfo->relnamespace->nspname));
			appendPQExpBuffer(delqry, "%s;\n",
							  fmtId(tbinfo->relname));

			resetPQExpBuffer(query);
			if (tgisconstraint)
			{
				appendPQExpBuffer(query, "CREATE CONSTRAINT TRIGGER ");
				appendPQExpBufferStr(query, fmtId(PQgetvalue(res, j, i_tgconstrname)));
			}
			else
			{
				appendPQExpBuffer(query, "CREATE TRIGGER ");
				appendPQExpBufferStr(query, fmtId(tgname));
			}
			appendPQExpBuffer(query, "\n    ");
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
			appendPQExpBuffer(query, " ON %s\n",
							  fmtId(tbinfo->relname));

			if (tgisconstraint)
			{
				tgconstrrelid = PQgetvalue(res, j, i_tgconstrrelid);

				if (strcmp(tgconstrrelid, "0") != 0)
				{

					if (PQgetisnull(res, j, i_tgconstrrelname))
					{
						write_msg(NULL, "query produced null referenced table name for foreign key trigger \"%s\" on table \"%s\" (OID of table: %s)\n",
								  tgname, tbinfo->relname, tgconstrrelid);
						exit_nicely();
					}

					/* If we are using regclass, name is already quoted */
					if (g_fout->remoteVersion >= 70300)
						appendPQExpBuffer(query, "    FROM %s\n    ",
								  PQgetvalue(res, j, i_tgconstrrelname));
					else
						appendPQExpBuffer(query, "    FROM %s\n    ",
						   fmtId(PQgetvalue(res, j, i_tgconstrrelname)));
				}
				if (!tgdeferrable)
					appendPQExpBuffer(query, "NOT ");
				appendPQExpBuffer(query, "DEFERRABLE INITIALLY ");
				if (tginitdeferred)
					appendPQExpBuffer(query, "DEFERRED\n");
				else
					appendPQExpBuffer(query, "IMMEDIATE\n");

			}

			if (TRIGGER_FOR_ROW(tgtype))
				appendPQExpBuffer(query, "    FOR EACH ROW\n    ");
			else
				appendPQExpBuffer(query, "    FOR EACH STATEMENT\n    ");

			/* In 7.3, result of regproc is already quoted */
			if (g_fout->remoteVersion >= 70300)
				appendPQExpBuffer(query, "EXECUTE PROCEDURE %s(",
								  tgfname);
			else
				appendPQExpBuffer(query, "EXECUTE PROCEDURE %s(",
								  fmtId(tgfname));
			for (findx = 0; findx < tgnargs; findx++)
			{
				const char *s;

				for (p = tgargs;;)
				{
					p = strchr(p, '\\');
					if (p == NULL)
					{
						write_msg(NULL, "invalid argument string (%s) for trigger \"%s\" on table \"%s\"\n",
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
						appendPQExpBufferChar(query, *s);
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
							  fmtId(tgname));
			appendPQExpBuffer(query, "ON %s",
							  fmtId(tbinfo->relname));

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
	PQExpBuffer cmd = createPQExpBuffer();
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
		TableInfo  *tbinfo = &tblinfo[t];

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
			 * We include pg_rules in the cross since it filters out all
			 * view rules (pjw 15-Sep-2000).
			 */
			appendPQExpBuffer(query, "SELECT definition,"
							  "   pg_rewrite.oid, pg_rewrite.rulename "
							  "FROM pg_rewrite, pg_class, pg_rules "
							  "WHERE pg_class.relname = ");
			appendStringLiteral(query, tbinfo->relname, true);
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
			printfPQExpBuffer(cmd, "%s\n", PQgetvalue(res, i, i_definition));
			ArchiveEntry(fout, PQgetvalue(res, i, i_oid),
						 PQgetvalue(res, i, i_rulename),
						 tbinfo->relnamespace->nspname,
						 tbinfo->usename,
						 "RULE", NULL,
						 cmd->data,
						 "",	/* Del */
						 NULL, NULL, NULL);

			/* Dump rule comments */

			resetPQExpBuffer(query);
			appendPQExpBuffer(query, "RULE %s", fmtId(PQgetvalue(res, i, i_rulename)));
			appendPQExpBuffer(query, " ON %s", fmtId(tbinfo->relname));
			dumpComment(fout, query->data,
						tbinfo->relnamespace->nspname,
						tbinfo->usename,
						PQgetvalue(res, i, i_oid), "pg_rewrite", 0, NULL);

		}

		PQclear(res);
	}

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(cmd);
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
					  fmtId(schemaName));
	if (strcmp(schemaName, "pg_catalog") != 0)
		appendPQExpBuffer(query, ", pg_catalog");
	res = PQexec(g_conn, query->data);
	if (!res ||
		PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		write_msg(NULL, "command to set search_path failed: %s",
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
		write_msg(NULL, "query to obtain name of data type %s failed: %s",
				  oid, PQerrorMessage(g_conn));
		exit_nicely();
	}

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
	bool	isarray = false;
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
	 * char is an internal single-byte data type; Let's make sure we force
	 * it through with quotes. - thomas 1998-12-13
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
