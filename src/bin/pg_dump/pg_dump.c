/*-------------------------------------------------------------------------
 *
 * pg_dump.c
 *	  pg_dump is a utility for dumping out a postgres database
 *	  into a script file.
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	pg_dump will read the system catalogs in a database and dump out a
 *	script that reproduces the schema in terms of SQL that is understood
 *	by PostgreSQL
 *
 *	Note that pg_dump runs in a transaction-snapshot mode transaction,
 *	so it sees a consistent snapshot of the database including system
 *	catalogs. However, it relies in part on various specialized backend
 *	functions like pg_get_indexdef(), and those things tend to look at
 *	the currently committed state.  So it is possible to get 'cache
 *	lookup failed' error if someone performs DDL changes while a dump is
 *	happening. The window for this sort of thing is from the acquisition
 *	of the transaction snapshot to getSchemaData() (when pg_dump acquires
 *	AccessShareLock on every table it intends to dump). It isn't very large,
 *	but it can happen.
 *
 *	http://archives.postgresql.org/pgsql-bugs/2010-02/msg00187.php
 *
 * IDENTIFICATION
 *	  src/bin/pg_dump/pg_dump.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <unistd.h>
#include <ctype.h>
#include <limits.h>
#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif

#include "access/attnum.h"
#include "access/sysattr.h"
#include "access/transam.h"
#include "catalog/pg_aggregate_d.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_attribute_d.h"
#include "catalog/pg_cast_d.h"
#include "catalog/pg_class_d.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_default_acl_d.h"
#include "catalog/pg_largeobject_d.h"
#include "catalog/pg_largeobject_metadata_d.h"
#include "catalog/pg_proc_d.h"
#include "catalog/pg_trigger_d.h"
#include "catalog/pg_type_d.h"
#include "common/connect.h"
#include "dumputils.h"
#include "fe_utils/string_utils.h"
#include "getopt_long.h"
#include "libpq/libpq-fs.h"
#include "parallel.h"
#include "pg_backup_db.h"
#include "pg_backup_utils.h"
#include "pg_dump.h"
#include "storage/block.h"

typedef struct
{
	const char *descr;			/* comment for an object */
	Oid			classoid;		/* object class (catalog OID) */
	Oid			objoid;			/* object OID */
	int			objsubid;		/* subobject (table column #) */
} CommentItem;

typedef struct
{
	const char *provider;		/* label provider of this security label */
	const char *label;			/* security label for an object */
	Oid			classoid;		/* object class (catalog OID) */
	Oid			objoid;			/* object OID */
	int			objsubid;		/* subobject (table column #) */
} SecLabelItem;

typedef enum OidOptions
{
	zeroIsError = 1,
	zeroAsStar = 2,
	zeroAsNone = 4
} OidOptions;

/* global decls */
static bool dosync = true;		/* Issue fsync() to make dump durable on disk. */

/* subquery used to convert user ID (eg, datdba) to user name */
static const char *username_subquery;

/*
 * For 8.0 and earlier servers, pulled from pg_database, for 8.1+ we use
 * FirstNormalObjectId - 1.
 */
static Oid	g_last_builtin_oid; /* value of the last builtin oid */

/* The specified names/patterns should to match at least one entity */
static int	strict_names = 0;

/*
 * Object inclusion/exclusion lists
 *
 * The string lists record the patterns given by command-line switches,
 * which we then convert to lists of OIDs of matching objects.
 */
static SimpleStringList schema_include_patterns = {NULL, NULL};
static SimpleOidList schema_include_oids = {NULL, NULL};
static SimpleStringList schema_exclude_patterns = {NULL, NULL};
static SimpleOidList schema_exclude_oids = {NULL, NULL};

static SimpleStringList table_include_patterns = {NULL, NULL};
static SimpleOidList table_include_oids = {NULL, NULL};
static SimpleStringList table_exclude_patterns = {NULL, NULL};
static SimpleOidList table_exclude_oids = {NULL, NULL};
static SimpleStringList tabledata_exclude_patterns = {NULL, NULL};
static SimpleOidList tabledata_exclude_oids = {NULL, NULL};
static SimpleStringList foreign_servers_include_patterns = {NULL, NULL};
static SimpleOidList foreign_servers_include_oids = {NULL, NULL};

static const CatalogId nilCatalogId = {0, 0};

/* override for standard extra_float_digits setting */
static bool have_extra_float_digits = false;
static int	extra_float_digits;

/*
 * The default number of rows per INSERT when
 * --inserts is specified without --rows-per-insert
 */
#define DUMP_DEFAULT_ROWS_PER_INSERT 1

/*
 * Macro for producing quoted, schema-qualified name of a dumpable object.
 */
#define fmtQualifiedDumpable(obj) \
	fmtQualifiedId((obj)->dobj.namespace->dobj.name, \
				   (obj)->dobj.name)

static void help(const char *progname);
static void setup_connection(Archive *AH,
							 const char *dumpencoding, const char *dumpsnapshot,
							 char *use_role);
static ArchiveFormat parseArchiveFormat(const char *format, ArchiveMode *mode);
static void expand_schema_name_patterns(Archive *fout,
										SimpleStringList *patterns,
										SimpleOidList *oids,
										bool strict_names);
static void expand_foreign_server_name_patterns(Archive *fout,
												SimpleStringList *patterns,
												SimpleOidList *oids);
static void expand_table_name_patterns(Archive *fout,
									   SimpleStringList *patterns,
									   SimpleOidList *oids,
									   bool strict_names);
static NamespaceInfo *findNamespace(Oid nsoid);
static void dumpTableData(Archive *fout, TableDataInfo *tdinfo);
static void refreshMatViewData(Archive *fout, TableDataInfo *tdinfo);
static void guessConstraintInheritance(TableInfo *tblinfo, int numTables);
static void dumpComment(Archive *fout, const char *type, const char *name,
						const char *namespace, const char *owner,
						CatalogId catalogId, int subid, DumpId dumpId);
static int	findComments(Archive *fout, Oid classoid, Oid objoid,
						 CommentItem **items);
static int	collectComments(Archive *fout, CommentItem **items);
static void dumpSecLabel(Archive *fout, const char *type, const char *name,
						 const char *namespace, const char *owner,
						 CatalogId catalogId, int subid, DumpId dumpId);
static int	findSecLabels(Archive *fout, Oid classoid, Oid objoid,
						  SecLabelItem **items);
static int	collectSecLabels(Archive *fout, SecLabelItem **items);
static void dumpDumpableObject(Archive *fout, DumpableObject *dobj);
static void dumpNamespace(Archive *fout, NamespaceInfo *nspinfo);
static void dumpExtension(Archive *fout, ExtensionInfo *extinfo);
static void dumpType(Archive *fout, TypeInfo *tyinfo);
static void dumpBaseType(Archive *fout, TypeInfo *tyinfo);
static void dumpEnumType(Archive *fout, TypeInfo *tyinfo);
static void dumpRangeType(Archive *fout, TypeInfo *tyinfo);
static void dumpUndefinedType(Archive *fout, TypeInfo *tyinfo);
static void dumpDomain(Archive *fout, TypeInfo *tyinfo);
static void dumpCompositeType(Archive *fout, TypeInfo *tyinfo);
static void dumpCompositeTypeColComments(Archive *fout, TypeInfo *tyinfo);
static void dumpShellType(Archive *fout, ShellTypeInfo *stinfo);
static void dumpProcLang(Archive *fout, ProcLangInfo *plang);
static void dumpFunc(Archive *fout, FuncInfo *finfo);
static void dumpCast(Archive *fout, CastInfo *cast);
static void dumpTransform(Archive *fout, TransformInfo *transform);
static void dumpOpr(Archive *fout, OprInfo *oprinfo);
static void dumpAccessMethod(Archive *fout, AccessMethodInfo *oprinfo);
static void dumpOpclass(Archive *fout, OpclassInfo *opcinfo);
static void dumpOpfamily(Archive *fout, OpfamilyInfo *opfinfo);
static void dumpCollation(Archive *fout, CollInfo *collinfo);
static void dumpConversion(Archive *fout, ConvInfo *convinfo);
static void dumpRule(Archive *fout, RuleInfo *rinfo);
static void dumpAgg(Archive *fout, AggInfo *agginfo);
static void dumpTrigger(Archive *fout, TriggerInfo *tginfo);
static void dumpEventTrigger(Archive *fout, EventTriggerInfo *evtinfo);
static void dumpTable(Archive *fout, TableInfo *tbinfo);
static void dumpTableSchema(Archive *fout, TableInfo *tbinfo);
static void dumpAttrDef(Archive *fout, AttrDefInfo *adinfo);
static void dumpSequence(Archive *fout, TableInfo *tbinfo);
static void dumpSequenceData(Archive *fout, TableDataInfo *tdinfo);
static void dumpIndex(Archive *fout, IndxInfo *indxinfo);
static void dumpIndexAttach(Archive *fout, IndexAttachInfo *attachinfo);
static void dumpStatisticsExt(Archive *fout, StatsExtInfo *statsextinfo);
static void dumpConstraint(Archive *fout, ConstraintInfo *coninfo);
static void dumpTableConstraintComment(Archive *fout, ConstraintInfo *coninfo);
static void dumpTSParser(Archive *fout, TSParserInfo *prsinfo);
static void dumpTSDictionary(Archive *fout, TSDictInfo *dictinfo);
static void dumpTSTemplate(Archive *fout, TSTemplateInfo *tmplinfo);
static void dumpTSConfig(Archive *fout, TSConfigInfo *cfginfo);
static void dumpForeignDataWrapper(Archive *fout, FdwInfo *fdwinfo);
static void dumpForeignServer(Archive *fout, ForeignServerInfo *srvinfo);
static void dumpUserMappings(Archive *fout,
							 const char *servername, const char *namespace,
							 const char *owner, CatalogId catalogId, DumpId dumpId);
static void dumpDefaultACL(Archive *fout, DefaultACLInfo *daclinfo);

static DumpId dumpACL(Archive *fout, DumpId objDumpId, DumpId altDumpId,
					  const char *type, const char *name, const char *subname,
					  const char *nspname, const char *owner,
					  const char *acls, const char *racls,
					  const char *initacls, const char *initracls);

static void getDependencies(Archive *fout);
static void BuildArchiveDependencies(Archive *fout);
static void findDumpableDependencies(ArchiveHandle *AH, DumpableObject *dobj,
									 DumpId **dependencies, int *nDeps, int *allocDeps);

static DumpableObject *createBoundaryObjects(void);
static void addBoundaryDependencies(DumpableObject **dobjs, int numObjs,
									DumpableObject *boundaryObjs);

static void addConstrChildIdxDeps(DumpableObject *dobj, IndxInfo *refidx);
static void getDomainConstraints(Archive *fout, TypeInfo *tyinfo);
static void getTableData(DumpOptions *dopt, TableInfo *tblinfo, int numTables, char relkind);
static void makeTableDataInfo(DumpOptions *dopt, TableInfo *tbinfo);
static void buildMatViewRefreshDependencies(Archive *fout);
static void getTableDataFKConstraints(void);
static char *format_function_arguments(FuncInfo *finfo, char *funcargs,
									   bool is_agg);
static char *format_function_arguments_old(Archive *fout,
										   FuncInfo *finfo, int nallargs,
										   char **allargtypes,
										   char **argmodes,
										   char **argnames);
static char *format_function_signature(Archive *fout,
									   FuncInfo *finfo, bool honor_quotes);
static char *convertRegProcReference(const char *proc);
static char *getFormattedOperatorName(const char *oproid);
static char *convertTSFunction(Archive *fout, Oid funcOid);
static Oid	findLastBuiltinOid_V71(Archive *fout);
static char *getFormattedTypeName(Archive *fout, Oid oid, OidOptions opts);
static void getBlobs(Archive *fout);
static void dumpBlob(Archive *fout, BlobInfo *binfo);
static int	dumpBlobs(Archive *fout, void *arg);
static void dumpPolicy(Archive *fout, PolicyInfo *polinfo);
static void dumpPublication(Archive *fout, PublicationInfo *pubinfo);
static void dumpPublicationTable(Archive *fout, PublicationRelInfo *pubrinfo);
static void dumpSubscription(Archive *fout, SubscriptionInfo *subinfo);
static void dumpDatabase(Archive *AH);
static void dumpDatabaseConfig(Archive *AH, PQExpBuffer outbuf,
							   const char *dbname, Oid dboid);
static void dumpEncoding(Archive *AH);
static void dumpStdStrings(Archive *AH);
static void dumpSearchPath(Archive *AH);
static void binary_upgrade_set_type_oids_by_type_oid(Archive *fout,
													 PQExpBuffer upgrade_buffer,
													 Oid pg_type_oid,
													 bool force_array_type);
static void binary_upgrade_set_type_oids_by_rel_oid(Archive *fout,
													PQExpBuffer upgrade_buffer, Oid pg_rel_oid);
static void binary_upgrade_set_pg_class_oids(Archive *fout,
											 PQExpBuffer upgrade_buffer,
											 Oid pg_class_oid, bool is_index);
static void binary_upgrade_extension_member(PQExpBuffer upgrade_buffer,
											DumpableObject *dobj,
											const char *objtype,
											const char *objname,
											const char *objnamespace);
static const char *getAttrName(int attrnum, TableInfo *tblInfo);
static const char *fmtCopyColumnList(const TableInfo *ti, PQExpBuffer buffer);
static bool nonemptyReloptions(const char *reloptions);
static void appendIndexCollationVersion(PQExpBuffer buffer, IndxInfo *indxinfo,
										int enc, bool coll_unknown,
										Archive *fount);
static void appendReloptionsArrayAH(PQExpBuffer buffer, const char *reloptions,
									const char *prefix, Archive *fout);
static char *get_synchronized_snapshot(Archive *fout);
static void setupDumpWorker(Archive *AHX);
static TableInfo *getRootTableInfo(TableInfo *tbinfo);


int
main(int argc, char **argv)
{
	int			c;
	const char *filename = NULL;
	const char *format = "p";
	TableInfo  *tblinfo;
	int			numTables;
	DumpableObject **dobjs;
	int			numObjs;
	DumpableObject *boundaryObjs;
	int			i;
	int			optindex;
	char	   *endptr;
	RestoreOptions *ropt;
	Archive    *fout;			/* the script file */
	bool		g_verbose = false;
	const char *dumpencoding = NULL;
	const char *dumpsnapshot = NULL;
	char	   *use_role = NULL;
	long		rowsPerInsert;
	int			numWorkers = 1;
	int			compressLevel = -1;
	int			plainText = 0;
	ArchiveFormat archiveFormat = archUnknown;
	ArchiveMode archiveMode;

	static DumpOptions dopt;

	static struct option long_options[] = {
		{"data-only", no_argument, NULL, 'a'},
		{"blobs", no_argument, NULL, 'b'},
		{"no-blobs", no_argument, NULL, 'B'},
		{"clean", no_argument, NULL, 'c'},
		{"create", no_argument, NULL, 'C'},
		{"dbname", required_argument, NULL, 'd'},
		{"file", required_argument, NULL, 'f'},
		{"format", required_argument, NULL, 'F'},
		{"host", required_argument, NULL, 'h'},
		{"jobs", 1, NULL, 'j'},
		{"no-reconnect", no_argument, NULL, 'R'},
		{"no-owner", no_argument, NULL, 'O'},
		{"port", required_argument, NULL, 'p'},
		{"schema", required_argument, NULL, 'n'},
		{"exclude-schema", required_argument, NULL, 'N'},
		{"schema-only", no_argument, NULL, 's'},
		{"superuser", required_argument, NULL, 'S'},
		{"table", required_argument, NULL, 't'},
		{"exclude-table", required_argument, NULL, 'T'},
		{"no-password", no_argument, NULL, 'w'},
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
		{"attribute-inserts", no_argument, &dopt.column_inserts, 1},
		{"binary-upgrade", no_argument, &dopt.binary_upgrade, 1},
		{"column-inserts", no_argument, &dopt.column_inserts, 1},
		{"disable-dollar-quoting", no_argument, &dopt.disable_dollar_quoting, 1},
		{"disable-triggers", no_argument, &dopt.disable_triggers, 1},
		{"enable-row-security", no_argument, &dopt.enable_row_security, 1},
		{"exclude-table-data", required_argument, NULL, 4},
		{"extra-float-digits", required_argument, NULL, 8},
		{"if-exists", no_argument, &dopt.if_exists, 1},
		{"inserts", no_argument, NULL, 9},
		{"lock-wait-timeout", required_argument, NULL, 2},
		{"no-tablespaces", no_argument, &dopt.outputNoTablespaces, 1},
		{"quote-all-identifiers", no_argument, &quote_all_identifiers, 1},
		{"load-via-partition-root", no_argument, &dopt.load_via_partition_root, 1},
		{"role", required_argument, NULL, 3},
		{"section", required_argument, NULL, 5},
		{"serializable-deferrable", no_argument, &dopt.serializable_deferrable, 1},
		{"snapshot", required_argument, NULL, 6},
		{"strict-names", no_argument, &strict_names, 1},
		{"use-set-session-authorization", no_argument, &dopt.use_setsessauth, 1},
		{"no-comments", no_argument, &dopt.no_comments, 1},
		{"no-publications", no_argument, &dopt.no_publications, 1},
		{"no-security-labels", no_argument, &dopt.no_security_labels, 1},
		{"no-synchronized-snapshots", no_argument, &dopt.no_synchronized_snapshots, 1},
		{"no-unlogged-table-data", no_argument, &dopt.no_unlogged_table_data, 1},
		{"no-subscriptions", no_argument, &dopt.no_subscriptions, 1},
		{"no-sync", no_argument, NULL, 7},
		{"on-conflict-do-nothing", no_argument, &dopt.do_nothing, 1},
		{"rows-per-insert", required_argument, NULL, 10},
		{"include-foreign-data", required_argument, NULL, 11},
		{"index-collation-versions-unknown", no_argument, &dopt.coll_unknown, 1},

		{NULL, 0, NULL, 0}
	};

	pg_logging_init(argv[0]);
	pg_logging_set_level(PG_LOG_WARNING);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_dump"));

	/*
	 * Initialize what we need for parallel execution, especially for thread
	 * support on Windows.
	 */
	init_parallel_dump_utils();

	progname = get_progname(argv[0]);

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			help(progname);
			exit_nicely(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_dump (PostgreSQL) " PG_VERSION);
			exit_nicely(0);
		}
	}

	InitDumpOptions(&dopt);

	while ((c = getopt_long(argc, argv, "abBcCd:E:f:F:h:j:n:N:Op:RsS:t:T:U:vwWxZ:",
							long_options, &optindex)) != -1)
	{
		switch (c)
		{
			case 'a':			/* Dump data only */
				dopt.dataOnly = true;
				break;

			case 'b':			/* Dump blobs */
				dopt.outputBlobs = true;
				break;

			case 'B':			/* Don't dump blobs */
				dopt.dontOutputBlobs = true;
				break;

			case 'c':			/* clean (i.e., drop) schema prior to create */
				dopt.outputClean = 1;
				break;

			case 'C':			/* Create DB */
				dopt.outputCreateDB = 1;
				break;

			case 'd':			/* database name */
				dopt.cparams.dbname = pg_strdup(optarg);
				break;

			case 'E':			/* Dump encoding */
				dumpencoding = pg_strdup(optarg);
				break;

			case 'f':
				filename = pg_strdup(optarg);
				break;

			case 'F':
				format = pg_strdup(optarg);
				break;

			case 'h':			/* server host */
				dopt.cparams.pghost = pg_strdup(optarg);
				break;

			case 'j':			/* number of dump jobs */
				numWorkers = atoi(optarg);
				break;

			case 'n':			/* include schema(s) */
				simple_string_list_append(&schema_include_patterns, optarg);
				dopt.include_everything = false;
				break;

			case 'N':			/* exclude schema(s) */
				simple_string_list_append(&schema_exclude_patterns, optarg);
				break;

			case 'O':			/* Don't reconnect to match owner */
				dopt.outputNoOwner = 1;
				break;

			case 'p':			/* server port */
				dopt.cparams.pgport = pg_strdup(optarg);
				break;

			case 'R':
				/* no-op, still accepted for backwards compatibility */
				break;

			case 's':			/* dump schema only */
				dopt.schemaOnly = true;
				break;

			case 'S':			/* Username for superuser in plain text output */
				dopt.outputSuperuser = pg_strdup(optarg);
				break;

			case 't':			/* include table(s) */
				simple_string_list_append(&table_include_patterns, optarg);
				dopt.include_everything = false;
				break;

			case 'T':			/* exclude table(s) */
				simple_string_list_append(&table_exclude_patterns, optarg);
				break;

			case 'U':
				dopt.cparams.username = pg_strdup(optarg);
				break;

			case 'v':			/* verbose */
				g_verbose = true;
				pg_logging_increase_verbosity();
				break;

			case 'w':
				dopt.cparams.promptPassword = TRI_NO;
				break;

			case 'W':
				dopt.cparams.promptPassword = TRI_YES;
				break;

			case 'x':			/* skip ACL dump */
				dopt.aclsSkip = true;
				break;

			case 'Z':			/* Compression Level */
				compressLevel = atoi(optarg);
				if (compressLevel < 0 || compressLevel > 9)
				{
					pg_log_error("compression level must be in range 0..9");
					exit_nicely(1);
				}
				break;

			case 0:
				/* This covers the long options. */
				break;

			case 2:				/* lock-wait-timeout */
				dopt.lockWaitTimeout = pg_strdup(optarg);
				break;

			case 3:				/* SET ROLE */
				use_role = pg_strdup(optarg);
				break;

			case 4:				/* exclude table(s) data */
				simple_string_list_append(&tabledata_exclude_patterns, optarg);
				break;

			case 5:				/* section */
				set_dump_section(optarg, &dopt.dumpSections);
				break;

			case 6:				/* snapshot */
				dumpsnapshot = pg_strdup(optarg);
				break;

			case 7:				/* no-sync */
				dosync = false;
				break;

			case 8:
				have_extra_float_digits = true;
				extra_float_digits = atoi(optarg);
				if (extra_float_digits < -15 || extra_float_digits > 3)
				{
					pg_log_error("extra_float_digits must be in range -15..3");
					exit_nicely(1);
				}
				break;

			case 9:				/* inserts */

				/*
				 * dump_inserts also stores --rows-per-insert, careful not to
				 * overwrite that.
				 */
				if (dopt.dump_inserts == 0)
					dopt.dump_inserts = DUMP_DEFAULT_ROWS_PER_INSERT;
				break;

			case 10:			/* rows per insert */
				errno = 0;
				rowsPerInsert = strtol(optarg, &endptr, 10);

				if (endptr == optarg || *endptr != '\0' ||
					rowsPerInsert <= 0 || rowsPerInsert > INT_MAX ||
					errno == ERANGE)
				{
					pg_log_error("rows-per-insert must be in range %d..%d",
								 1, INT_MAX);
					exit_nicely(1);
				}
				dopt.dump_inserts = (int) rowsPerInsert;
				break;

			case 11:			/* include foreign data */
				simple_string_list_append(&foreign_servers_include_patterns,
										  optarg);
				break;

			default:
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
				exit_nicely(1);
		}
	}

	/*
	 * Non-option argument specifies database name as long as it wasn't
	 * already specified with -d / --dbname
	 */
	if (optind < argc && dopt.cparams.dbname == NULL)
		dopt.cparams.dbname = argv[optind++];

	/* Complain if any arguments remain */
	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit_nicely(1);
	}

	/* --column-inserts implies --inserts */
	if (dopt.column_inserts && dopt.dump_inserts == 0)
		dopt.dump_inserts = DUMP_DEFAULT_ROWS_PER_INSERT;

	/*
	 * Binary upgrade mode implies dumping sequence data even in schema-only
	 * mode.  This is not exposed as a separate option, but kept separate
	 * internally for clarity.
	 */
	if (dopt.binary_upgrade)
		dopt.sequence_data = 1;

	if (dopt.dataOnly && dopt.schemaOnly)
	{
		pg_log_error("options -s/--schema-only and -a/--data-only cannot be used together");
		exit_nicely(1);
	}

	if (dopt.schemaOnly && foreign_servers_include_patterns.head != NULL)
		fatal("options -s/--schema-only and --include-foreign-data cannot be used together");

	if (numWorkers > 1 && foreign_servers_include_patterns.head != NULL)
		fatal("option --include-foreign-data is not supported with parallel backup");

	if (dopt.dataOnly && dopt.outputClean)
	{
		pg_log_error("options -c/--clean and -a/--data-only cannot be used together");
		exit_nicely(1);
	}

	if (dopt.if_exists && !dopt.outputClean)
		fatal("option --if-exists requires option -c/--clean");

	/*
	 * --inserts are already implied above if --column-inserts or
	 * --rows-per-insert were specified.
	 */
	if (dopt.do_nothing && dopt.dump_inserts == 0)
		fatal("option --on-conflict-do-nothing requires option --inserts, --rows-per-insert, or --column-inserts");

	/* Identify archive format to emit */
	archiveFormat = parseArchiveFormat(format, &archiveMode);

	/* archiveFormat specific setup */
	if (archiveFormat == archNull)
		plainText = 1;

	/* Custom and directory formats are compressed by default, others not */
	if (compressLevel == -1)
	{
#ifdef HAVE_LIBZ
		if (archiveFormat == archCustom || archiveFormat == archDirectory)
			compressLevel = Z_DEFAULT_COMPRESSION;
		else
#endif
			compressLevel = 0;
	}

#ifndef HAVE_LIBZ
	if (compressLevel != 0)
		pg_log_warning("requested compression not available in this installation -- archive will be uncompressed");
	compressLevel = 0;
#endif

	/*
	 * If emitting an archive format, we always want to emit a DATABASE item,
	 * in case --create is specified at pg_restore time.
	 */
	if (!plainText)
		dopt.outputCreateDB = 1;

	/*
	 * On Windows we can only have at most MAXIMUM_WAIT_OBJECTS (= 64 usually)
	 * parallel jobs because that's the maximum limit for the
	 * WaitForMultipleObjects() call.
	 */
	if (numWorkers <= 0
#ifdef WIN32
		|| numWorkers > MAXIMUM_WAIT_OBJECTS
#endif
		)
		fatal("invalid number of parallel jobs");

	/* Parallel backup only in the directory archive format so far */
	if (archiveFormat != archDirectory && numWorkers > 1)
		fatal("parallel backup only supported by the directory format");

	/* Unknown collation versions only relevant in binary upgrade mode */
	if (dopt.coll_unknown && !dopt.binary_upgrade)
		fatal("option --index-collation-versions-unknown only works in binary upgrade mode");

	/* Open the output file */
	fout = CreateArchive(filename, archiveFormat, compressLevel, dosync,
						 archiveMode, setupDumpWorker);

	/* Make dump options accessible right away */
	SetArchiveOptions(fout, &dopt, NULL);

	/* Register the cleanup hook */
	on_exit_close_archive(fout);

	/* Let the archiver know how noisy to be */
	fout->verbose = g_verbose;


	/*
	 * We allow the server to be back to 8.0, and up to any minor release of
	 * our own major version.  (See also version check in pg_dumpall.c.)
	 */
	fout->minRemoteVersion = 80000;
	fout->maxRemoteVersion = (PG_VERSION_NUM / 100) * 100 + 99;

	fout->numWorkers = numWorkers;

	/*
	 * Open the database using the Archiver, so it knows about it. Errors mean
	 * death.
	 */
	ConnectDatabase(fout, &dopt.cparams, false);
	setup_connection(fout, dumpencoding, dumpsnapshot, use_role);

	/*
	 * Disable security label support if server version < v9.1.x (prevents
	 * access to nonexistent pg_seclabel catalog)
	 */
	if (fout->remoteVersion < 90100)
		dopt.no_security_labels = 1;

	/*
	 * On hot standbys, never try to dump unlogged table data, since it will
	 * just throw an error.
	 */
	if (fout->isStandby)
		dopt.no_unlogged_table_data = true;

	/* Select the appropriate subquery to convert user IDs to names */
	if (fout->remoteVersion >= 80100)
		username_subquery = "SELECT rolname FROM pg_catalog.pg_roles WHERE oid =";
	else
		username_subquery = "SELECT usename FROM pg_catalog.pg_user WHERE usesysid =";

	/* check the version for the synchronized snapshots feature */
	if (numWorkers > 1 && fout->remoteVersion < 90200
		&& !dopt.no_synchronized_snapshots)
		fatal("Synchronized snapshots are not supported by this server version.\n"
			  "Run with --no-synchronized-snapshots instead if you do not need\n"
			  "synchronized snapshots.");

	/* check the version when a snapshot is explicitly specified by user */
	if (dumpsnapshot && fout->remoteVersion < 90200)
		fatal("Exported snapshots are not supported by this server version.");

	/*
	 * Find the last built-in OID, if needed (prior to 8.1)
	 *
	 * With 8.1 and above, we can just use FirstNormalObjectId - 1.
	 */
	if (fout->remoteVersion < 80100)
		g_last_builtin_oid = findLastBuiltinOid_V71(fout);
	else
		g_last_builtin_oid = FirstNormalObjectId - 1;

	pg_log_info("last built-in OID is %u", g_last_builtin_oid);

	/* Expand schema selection patterns into OID lists */
	if (schema_include_patterns.head != NULL)
	{
		expand_schema_name_patterns(fout, &schema_include_patterns,
									&schema_include_oids,
									strict_names);
		if (schema_include_oids.head == NULL)
			fatal("no matching schemas were found");
	}
	expand_schema_name_patterns(fout, &schema_exclude_patterns,
								&schema_exclude_oids,
								false);
	/* non-matching exclusion patterns aren't an error */

	/* Expand table selection patterns into OID lists */
	if (table_include_patterns.head != NULL)
	{
		expand_table_name_patterns(fout, &table_include_patterns,
								   &table_include_oids,
								   strict_names);
		if (table_include_oids.head == NULL)
			fatal("no matching tables were found");
	}
	expand_table_name_patterns(fout, &table_exclude_patterns,
							   &table_exclude_oids,
							   false);

	expand_table_name_patterns(fout, &tabledata_exclude_patterns,
							   &tabledata_exclude_oids,
							   false);

	expand_foreign_server_name_patterns(fout, &foreign_servers_include_patterns,
										&foreign_servers_include_oids);

	/* non-matching exclusion patterns aren't an error */

	/*
	 * Dumping blobs is the default for dumps where an inclusion switch is not
	 * used (an "include everything" dump).  -B can be used to exclude blobs
	 * from those dumps.  -b can be used to include blobs even when an
	 * inclusion switch is used.
	 *
	 * -s means "schema only" and blobs are data, not schema, so we never
	 * include blobs when -s is used.
	 */
	if (dopt.include_everything && !dopt.schemaOnly && !dopt.dontOutputBlobs)
		dopt.outputBlobs = true;

	/*
	 * Now scan the database and create DumpableObject structs for all the
	 * objects we intend to dump.
	 */
	tblinfo = getSchemaData(fout, &numTables);

	if (fout->remoteVersion < 80400)
		guessConstraintInheritance(tblinfo, numTables);

	if (!dopt.schemaOnly)
	{
		getTableData(&dopt, tblinfo, numTables, 0);
		buildMatViewRefreshDependencies(fout);
		if (dopt.dataOnly)
			getTableDataFKConstraints();
	}

	if (dopt.schemaOnly && dopt.sequence_data)
		getTableData(&dopt, tblinfo, numTables, RELKIND_SEQUENCE);

	/*
	 * In binary-upgrade mode, we do not have to worry about the actual blob
	 * data or the associated metadata that resides in the pg_largeobject and
	 * pg_largeobject_metadata tables, respectively.
	 *
	 * However, we do need to collect blob information as there may be
	 * comments or other information on blobs that we do need to dump out.
	 */
	if (dopt.outputBlobs || dopt.binary_upgrade)
		getBlobs(fout);

	/*
	 * Collect dependency data to assist in ordering the objects.
	 */
	getDependencies(fout);

	/* Lastly, create dummy objects to represent the section boundaries */
	boundaryObjs = createBoundaryObjects();

	/* Get pointers to all the known DumpableObjects */
	getDumpableObjects(&dobjs, &numObjs);

	/*
	 * Add dummy dependencies to enforce the dump section ordering.
	 */
	addBoundaryDependencies(dobjs, numObjs, boundaryObjs);

	/*
	 * Sort the objects into a safe dump order (no forward references).
	 *
	 * We rely on dependency information to help us determine a safe order, so
	 * the initial sort is mostly for cosmetic purposes: we sort by name to
	 * ensure that logically identical schemas will dump identically.
	 */
	sortDumpableObjectsByTypeName(dobjs, numObjs);

	sortDumpableObjects(dobjs, numObjs,
						boundaryObjs[0].dumpId, boundaryObjs[1].dumpId);

	/*
	 * Create archive TOC entries for all the objects to be dumped, in a safe
	 * order.
	 */

	/* First the special ENCODING, STDSTRINGS, and SEARCHPATH entries. */
	dumpEncoding(fout);
	dumpStdStrings(fout);
	dumpSearchPath(fout);

	/* The database items are always next, unless we don't want them at all */
	if (dopt.outputCreateDB)
		dumpDatabase(fout);

	/* Now the rearrangeable objects. */
	for (i = 0; i < numObjs; i++)
		dumpDumpableObject(fout, dobjs[i]);

	/*
	 * Set up options info to ensure we dump what we want.
	 */
	ropt = NewRestoreOptions();
	ropt->filename = filename;

	/* if you change this list, see dumpOptionsFromRestoreOptions */
	ropt->cparams.dbname = dopt.cparams.dbname ? pg_strdup(dopt.cparams.dbname) : NULL;
	ropt->cparams.pgport = dopt.cparams.pgport ? pg_strdup(dopt.cparams.pgport) : NULL;
	ropt->cparams.pghost = dopt.cparams.pghost ? pg_strdup(dopt.cparams.pghost) : NULL;
	ropt->cparams.username = dopt.cparams.username ? pg_strdup(dopt.cparams.username) : NULL;
	ropt->cparams.promptPassword = dopt.cparams.promptPassword;
	ropt->dropSchema = dopt.outputClean;
	ropt->dataOnly = dopt.dataOnly;
	ropt->schemaOnly = dopt.schemaOnly;
	ropt->if_exists = dopt.if_exists;
	ropt->column_inserts = dopt.column_inserts;
	ropt->dumpSections = dopt.dumpSections;
	ropt->aclsSkip = dopt.aclsSkip;
	ropt->superuser = dopt.outputSuperuser;
	ropt->createDB = dopt.outputCreateDB;
	ropt->noOwner = dopt.outputNoOwner;
	ropt->noTablespace = dopt.outputNoTablespaces;
	ropt->disable_triggers = dopt.disable_triggers;
	ropt->use_setsessauth = dopt.use_setsessauth;
	ropt->disable_dollar_quoting = dopt.disable_dollar_quoting;
	ropt->dump_inserts = dopt.dump_inserts;
	ropt->no_comments = dopt.no_comments;
	ropt->no_publications = dopt.no_publications;
	ropt->no_security_labels = dopt.no_security_labels;
	ropt->no_subscriptions = dopt.no_subscriptions;
	ropt->lockWaitTimeout = dopt.lockWaitTimeout;
	ropt->include_everything = dopt.include_everything;
	ropt->enable_row_security = dopt.enable_row_security;
	ropt->sequence_data = dopt.sequence_data;
	ropt->binary_upgrade = dopt.binary_upgrade;

	if (compressLevel == -1)
		ropt->compression = 0;
	else
		ropt->compression = compressLevel;

	ropt->suppressDumpWarnings = true;	/* We've already shown them */

	SetArchiveOptions(fout, &dopt, ropt);

	/* Mark which entries should be output */
	ProcessArchiveRestoreOptions(fout);

	/*
	 * The archive's TOC entries are now marked as to which ones will actually
	 * be output, so we can set up their dependency lists properly. This isn't
	 * necessary for plain-text output, though.
	 */
	if (!plainText)
		BuildArchiveDependencies(fout);

	/*
	 * And finally we can do the actual output.
	 *
	 * Note: for non-plain-text output formats, the output file is written
	 * inside CloseArchive().  This is, um, bizarre; but not worth changing
	 * right now.
	 */
	if (plainText)
		RestoreArchive(fout);

	CloseArchive(fout);

	exit_nicely(0);
}


static void
help(const char *progname)
{
	printf(_("%s dumps a database as a text file or to other formats.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... [DBNAME]\n"), progname);

	printf(_("\nGeneral options:\n"));
	printf(_("  -f, --file=FILENAME          output file or directory name\n"));
	printf(_("  -F, --format=c|d|t|p         output file format (custom, directory, tar,\n"
			 "                               plain text (default))\n"));
	printf(_("  -j, --jobs=NUM               use this many parallel jobs to dump\n"));
	printf(_("  -v, --verbose                verbose mode\n"));
	printf(_("  -V, --version                output version information, then exit\n"));
	printf(_("  -Z, --compress=0-9           compression level for compressed formats\n"));
	printf(_("  --lock-wait-timeout=TIMEOUT  fail after waiting TIMEOUT for a table lock\n"));
	printf(_("  --no-sync                    do not wait for changes to be written safely to disk\n"));
	printf(_("  -?, --help                   show this help, then exit\n"));

	printf(_("\nOptions controlling the output content:\n"));
	printf(_("  -a, --data-only              dump only the data, not the schema\n"));
	printf(_("  -b, --blobs                  include large objects in dump\n"));
	printf(_("  -B, --no-blobs               exclude large objects in dump\n"));
	printf(_("  -c, --clean                  clean (drop) database objects before recreating\n"));
	printf(_("  -C, --create                 include commands to create database in dump\n"));
	printf(_("  -E, --encoding=ENCODING      dump the data in encoding ENCODING\n"));
	printf(_("  -n, --schema=PATTERN         dump the specified schema(s) only\n"));
	printf(_("  -N, --exclude-schema=PATTERN do NOT dump the specified schema(s)\n"));
	printf(_("  -O, --no-owner               skip restoration of object ownership in\n"
			 "                               plain-text format\n"));
	printf(_("  -s, --schema-only            dump only the schema, no data\n"));
	printf(_("  -S, --superuser=NAME         superuser user name to use in plain-text format\n"));
	printf(_("  -t, --table=PATTERN          dump the specified table(s) only\n"));
	printf(_("  -T, --exclude-table=PATTERN  do NOT dump the specified table(s)\n"));
	printf(_("  -x, --no-privileges          do not dump privileges (grant/revoke)\n"));
	printf(_("  --binary-upgrade             for use by upgrade utilities only\n"));
	printf(_("  --column-inserts             dump data as INSERT commands with column names\n"));
	printf(_("  --disable-dollar-quoting     disable dollar quoting, use SQL standard quoting\n"));
	printf(_("  --disable-triggers           disable triggers during data-only restore\n"));
	printf(_("  --enable-row-security        enable row security (dump only content user has\n"
			 "                               access to)\n"));
	printf(_("  --exclude-table-data=PATTERN do NOT dump data for the specified table(s)\n"));
	printf(_("  --extra-float-digits=NUM     override default setting for extra_float_digits\n"));
	printf(_("  --if-exists                  use IF EXISTS when dropping objects\n"));
	printf(_("  --include-foreign-data=PATTERN\n"
			 "                               include data of foreign tables on foreign\n"
			 "                               servers matching PATTERN\n"));
	printf(_("  --inserts                    dump data as INSERT commands, rather than COPY\n"));
	printf(_("  --load-via-partition-root    load partitions via the root table\n"));
	printf(_("  --no-comments                do not dump comments\n"));
	printf(_("  --no-publications            do not dump publications\n"));
	printf(_("  --no-security-labels         do not dump security label assignments\n"));
	printf(_("  --no-subscriptions           do not dump subscriptions\n"));
	printf(_("  --no-synchronized-snapshots  do not use synchronized snapshots in parallel jobs\n"));
	printf(_("  --no-tablespaces             do not dump tablespace assignments\n"));
	printf(_("  --no-unlogged-table-data     do not dump unlogged table data\n"));
	printf(_("  --on-conflict-do-nothing     add ON CONFLICT DO NOTHING to INSERT commands\n"));
	printf(_("  --quote-all-identifiers      quote all identifiers, even if not key words\n"));
	printf(_("  --rows-per-insert=NROWS      number of rows per INSERT; implies --inserts\n"));
	printf(_("  --section=SECTION            dump named section (pre-data, data, or post-data)\n"));
	printf(_("  --serializable-deferrable    wait until the dump can run without anomalies\n"));
	printf(_("  --snapshot=SNAPSHOT          use given snapshot for the dump\n"));
	printf(_("  --strict-names               require table and/or schema include patterns to\n"
			 "                               match at least one entity each\n"));
	printf(_("  --use-set-session-authorization\n"
			 "                               use SET SESSION AUTHORIZATION commands instead of\n"
			 "                               ALTER OWNER commands to set ownership\n"));

	printf(_("\nConnection options:\n"));
	printf(_("  -d, --dbname=DBNAME      database to dump\n"));
	printf(_("  -h, --host=HOSTNAME      database server host or socket directory\n"));
	printf(_("  -p, --port=PORT          database server port number\n"));
	printf(_("  -U, --username=NAME      connect as specified database user\n"));
	printf(_("  -w, --no-password        never prompt for password\n"));
	printf(_("  -W, --password           force password prompt (should happen automatically)\n"));
	printf(_("  --role=ROLENAME          do SET ROLE before dump\n"));

	printf(_("\nIf no database name is supplied, then the PGDATABASE environment\n"
			 "variable value is used.\n\n"));
	printf(_("Report bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}

static void
setup_connection(Archive *AH, const char *dumpencoding,
				 const char *dumpsnapshot, char *use_role)
{
	DumpOptions *dopt = AH->dopt;
	PGconn	   *conn = GetConnection(AH);
	const char *std_strings;

	PQclear(ExecuteSqlQueryForSingleRow(AH, ALWAYS_SECURE_SEARCH_PATH_SQL));

	/*
	 * Set the client encoding if requested.
	 */
	if (dumpencoding)
	{
		if (PQsetClientEncoding(conn, dumpencoding) < 0)
			fatal("invalid client encoding \"%s\" specified",
				  dumpencoding);
	}

	/*
	 * Get the active encoding and the standard_conforming_strings setting, so
	 * we know how to escape strings.
	 */
	AH->encoding = PQclientEncoding(conn);

	std_strings = PQparameterStatus(conn, "standard_conforming_strings");
	AH->std_strings = (std_strings && strcmp(std_strings, "on") == 0);

	/*
	 * Set the role if requested.  In a parallel dump worker, we'll be passed
	 * use_role == NULL, but AH->use_role is already set (if user specified it
	 * originally) and we should use that.
	 */
	if (!use_role && AH->use_role)
		use_role = AH->use_role;

	/* Set the role if requested */
	if (use_role && AH->remoteVersion >= 80100)
	{
		PQExpBuffer query = createPQExpBuffer();

		appendPQExpBuffer(query, "SET ROLE %s", fmtId(use_role));
		ExecuteSqlStatement(AH, query->data);
		destroyPQExpBuffer(query);

		/* save it for possible later use by parallel workers */
		if (!AH->use_role)
			AH->use_role = pg_strdup(use_role);
	}

	/* Set the datestyle to ISO to ensure the dump's portability */
	ExecuteSqlStatement(AH, "SET DATESTYLE = ISO");

	/* Likewise, avoid using sql_standard intervalstyle */
	if (AH->remoteVersion >= 80400)
		ExecuteSqlStatement(AH, "SET INTERVALSTYLE = POSTGRES");

	/*
	 * Use an explicitly specified extra_float_digits if it has been provided.
	 * Otherwise, set extra_float_digits so that we can dump float data
	 * exactly (given correctly implemented float I/O code, anyway).
	 */
	if (have_extra_float_digits)
	{
		PQExpBuffer q = createPQExpBuffer();

		appendPQExpBuffer(q, "SET extra_float_digits TO %d",
						  extra_float_digits);
		ExecuteSqlStatement(AH, q->data);
		destroyPQExpBuffer(q);
	}
	else if (AH->remoteVersion >= 90000)
		ExecuteSqlStatement(AH, "SET extra_float_digits TO 3");
	else
		ExecuteSqlStatement(AH, "SET extra_float_digits TO 2");

	/*
	 * If synchronized scanning is supported, disable it, to prevent
	 * unpredictable changes in row ordering across a dump and reload.
	 */
	if (AH->remoteVersion >= 80300)
		ExecuteSqlStatement(AH, "SET synchronize_seqscans TO off");

	/*
	 * Disable timeouts if supported.
	 */
	ExecuteSqlStatement(AH, "SET statement_timeout = 0");
	if (AH->remoteVersion >= 90300)
		ExecuteSqlStatement(AH, "SET lock_timeout = 0");
	if (AH->remoteVersion >= 90600)
		ExecuteSqlStatement(AH, "SET idle_in_transaction_session_timeout = 0");

	/*
	 * Quote all identifiers, if requested.
	 */
	if (quote_all_identifiers && AH->remoteVersion >= 90100)
		ExecuteSqlStatement(AH, "SET quote_all_identifiers = true");

	/*
	 * Adjust row-security mode, if supported.
	 */
	if (AH->remoteVersion >= 90500)
	{
		if (dopt->enable_row_security)
			ExecuteSqlStatement(AH, "SET row_security = on");
		else
			ExecuteSqlStatement(AH, "SET row_security = off");
	}

	/*
	 * Start transaction-snapshot mode transaction to dump consistent data.
	 */
	ExecuteSqlStatement(AH, "BEGIN");
	if (AH->remoteVersion >= 90100)
	{
		/*
		 * To support the combination of serializable_deferrable with the jobs
		 * option we use REPEATABLE READ for the worker connections that are
		 * passed a snapshot.  As long as the snapshot is acquired in a
		 * SERIALIZABLE, READ ONLY, DEFERRABLE transaction, its use within a
		 * REPEATABLE READ transaction provides the appropriate integrity
		 * guarantees.  This is a kluge, but safe for back-patching.
		 */
		if (dopt->serializable_deferrable && AH->sync_snapshot_id == NULL)
			ExecuteSqlStatement(AH,
								"SET TRANSACTION ISOLATION LEVEL "
								"SERIALIZABLE, READ ONLY, DEFERRABLE");
		else
			ExecuteSqlStatement(AH,
								"SET TRANSACTION ISOLATION LEVEL "
								"REPEATABLE READ, READ ONLY");
	}
	else
	{
		ExecuteSqlStatement(AH,
							"SET TRANSACTION ISOLATION LEVEL "
							"SERIALIZABLE, READ ONLY");
	}

	/*
	 * If user specified a snapshot to use, select that.  In a parallel dump
	 * worker, we'll be passed dumpsnapshot == NULL, but AH->sync_snapshot_id
	 * is already set (if the server can handle it) and we should use that.
	 */
	if (dumpsnapshot)
		AH->sync_snapshot_id = pg_strdup(dumpsnapshot);

	if (AH->sync_snapshot_id)
	{
		PQExpBuffer query = createPQExpBuffer();

		appendPQExpBufferStr(query, "SET TRANSACTION SNAPSHOT ");
		appendStringLiteralConn(query, AH->sync_snapshot_id, conn);
		ExecuteSqlStatement(AH, query->data);
		destroyPQExpBuffer(query);
	}
	else if (AH->numWorkers > 1 &&
			 AH->remoteVersion >= 90200 &&
			 !dopt->no_synchronized_snapshots)
	{
		if (AH->isStandby && AH->remoteVersion < 100000)
			fatal("Synchronized snapshots on standby servers are not supported by this server version.\n"
				  "Run with --no-synchronized-snapshots instead if you do not need\n"
				  "synchronized snapshots.");


		AH->sync_snapshot_id = get_synchronized_snapshot(AH);
	}
}

/* Set up connection for a parallel worker process */
static void
setupDumpWorker(Archive *AH)
{
	/*
	 * We want to re-select all the same values the leader connection is
	 * using.  We'll have inherited directly-usable values in
	 * AH->sync_snapshot_id and AH->use_role, but we need to translate the
	 * inherited encoding value back to a string to pass to setup_connection.
	 */
	setup_connection(AH,
					 pg_encoding_to_char(AH->encoding),
					 NULL,
					 NULL);
}

static char *
get_synchronized_snapshot(Archive *fout)
{
	char	   *query = "SELECT pg_catalog.pg_export_snapshot()";
	char	   *result;
	PGresult   *res;

	res = ExecuteSqlQueryForSingleRow(fout, query);
	result = pg_strdup(PQgetvalue(res, 0, 0));
	PQclear(res);

	return result;
}

static ArchiveFormat
parseArchiveFormat(const char *format, ArchiveMode *mode)
{
	ArchiveFormat archiveFormat;

	*mode = archModeWrite;

	if (pg_strcasecmp(format, "a") == 0 || pg_strcasecmp(format, "append") == 0)
	{
		/* This is used by pg_dumpall, and is not documented */
		archiveFormat = archNull;
		*mode = archModeAppend;
	}
	else if (pg_strcasecmp(format, "c") == 0)
		archiveFormat = archCustom;
	else if (pg_strcasecmp(format, "custom") == 0)
		archiveFormat = archCustom;
	else if (pg_strcasecmp(format, "d") == 0)
		archiveFormat = archDirectory;
	else if (pg_strcasecmp(format, "directory") == 0)
		archiveFormat = archDirectory;
	else if (pg_strcasecmp(format, "p") == 0)
		archiveFormat = archNull;
	else if (pg_strcasecmp(format, "plain") == 0)
		archiveFormat = archNull;
	else if (pg_strcasecmp(format, "t") == 0)
		archiveFormat = archTar;
	else if (pg_strcasecmp(format, "tar") == 0)
		archiveFormat = archTar;
	else
		fatal("invalid output format \"%s\" specified", format);
	return archiveFormat;
}

/*
 * Find the OIDs of all schemas matching the given list of patterns,
 * and append them to the given OID list.
 */
static void
expand_schema_name_patterns(Archive *fout,
							SimpleStringList *patterns,
							SimpleOidList *oids,
							bool strict_names)
{
	PQExpBuffer query;
	PGresult   *res;
	SimpleStringListCell *cell;
	int			i;

	if (patterns->head == NULL)
		return;					/* nothing to do */

	query = createPQExpBuffer();

	/*
	 * The loop below runs multiple SELECTs might sometimes result in
	 * duplicate entries in the OID list, but we don't care.
	 */

	for (cell = patterns->head; cell; cell = cell->next)
	{
		appendPQExpBufferStr(query,
							 "SELECT oid FROM pg_catalog.pg_namespace n\n");
		processSQLNamePattern(GetConnection(fout), query, cell->val, false,
							  false, NULL, "n.nspname", NULL, NULL);

		res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);
		if (strict_names && PQntuples(res) == 0)
			fatal("no matching schemas were found for pattern \"%s\"", cell->val);

		for (i = 0; i < PQntuples(res); i++)
		{
			simple_oid_list_append(oids, atooid(PQgetvalue(res, i, 0)));
		}

		PQclear(res);
		resetPQExpBuffer(query);
	}

	destroyPQExpBuffer(query);
}

/*
 * Find the OIDs of all foreign servers matching the given list of patterns,
 * and append them to the given OID list.
 */
static void
expand_foreign_server_name_patterns(Archive *fout,
									SimpleStringList *patterns,
									SimpleOidList *oids)
{
	PQExpBuffer query;
	PGresult   *res;
	SimpleStringListCell *cell;
	int			i;

	if (patterns->head == NULL)
		return;					/* nothing to do */

	query = createPQExpBuffer();

	/*
	 * The loop below runs multiple SELECTs might sometimes result in
	 * duplicate entries in the OID list, but we don't care.
	 */

	for (cell = patterns->head; cell; cell = cell->next)
	{
		appendPQExpBufferStr(query,
							 "SELECT oid FROM pg_catalog.pg_foreign_server s\n");
		processSQLNamePattern(GetConnection(fout), query, cell->val, false,
							  false, NULL, "s.srvname", NULL, NULL);

		res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);
		if (PQntuples(res) == 0)
			fatal("no matching foreign servers were found for pattern \"%s\"", cell->val);

		for (i = 0; i < PQntuples(res); i++)
			simple_oid_list_append(oids, atooid(PQgetvalue(res, i, 0)));

		PQclear(res);
		resetPQExpBuffer(query);
	}

	destroyPQExpBuffer(query);
}

/*
 * Find the OIDs of all tables matching the given list of patterns,
 * and append them to the given OID list. See also expand_dbname_patterns()
 * in pg_dumpall.c
 */
static void
expand_table_name_patterns(Archive *fout,
						   SimpleStringList *patterns, SimpleOidList *oids,
						   bool strict_names)
{
	PQExpBuffer query;
	PGresult   *res;
	SimpleStringListCell *cell;
	int			i;

	if (patterns->head == NULL)
		return;					/* nothing to do */

	query = createPQExpBuffer();

	/*
	 * this might sometimes result in duplicate entries in the OID list, but
	 * we don't care.
	 */

	for (cell = patterns->head; cell; cell = cell->next)
	{
		/*
		 * Query must remain ABSOLUTELY devoid of unqualified names.  This
		 * would be unnecessary given a pg_table_is_visible() variant taking a
		 * search_path argument.
		 */
		appendPQExpBuffer(query,
						  "SELECT c.oid"
						  "\nFROM pg_catalog.pg_class c"
						  "\n     LEFT JOIN pg_catalog.pg_namespace n"
						  "\n     ON n.oid OPERATOR(pg_catalog.=) c.relnamespace"
						  "\nWHERE c.relkind OPERATOR(pg_catalog.=) ANY"
						  "\n    (array['%c', '%c', '%c', '%c', '%c', '%c'])\n",
						  RELKIND_RELATION, RELKIND_SEQUENCE, RELKIND_VIEW,
						  RELKIND_MATVIEW, RELKIND_FOREIGN_TABLE,
						  RELKIND_PARTITIONED_TABLE);
		processSQLNamePattern(GetConnection(fout), query, cell->val, true,
							  false, "n.nspname", "c.relname", NULL,
							  "pg_catalog.pg_table_is_visible(c.oid)");

		ExecuteSqlStatement(fout, "RESET search_path");
		res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);
		PQclear(ExecuteSqlQueryForSingleRow(fout,
											ALWAYS_SECURE_SEARCH_PATH_SQL));
		if (strict_names && PQntuples(res) == 0)
			fatal("no matching tables were found for pattern \"%s\"", cell->val);

		for (i = 0; i < PQntuples(res); i++)
		{
			simple_oid_list_append(oids, atooid(PQgetvalue(res, i, 0)));
		}

		PQclear(res);
		resetPQExpBuffer(query);
	}

	destroyPQExpBuffer(query);
}

/*
 * checkExtensionMembership
 *		Determine whether object is an extension member, and if so,
 *		record an appropriate dependency and set the object's dump flag.
 *
 * It's important to call this for each object that could be an extension
 * member.  Generally, we integrate this with determining the object's
 * to-be-dumped-ness, since extension membership overrides other rules for that.
 *
 * Returns true if object is an extension member, else false.
 */
static bool
checkExtensionMembership(DumpableObject *dobj, Archive *fout)
{
	ExtensionInfo *ext = findOwningExtension(dobj->catId);

	if (ext == NULL)
		return false;

	dobj->ext_member = true;

	/* Record dependency so that getDependencies needn't deal with that */
	addObjectDependency(dobj, ext->dobj.dumpId);

	/*
	 * In 9.6 and above, mark the member object to have any non-initial ACL,
	 * policies, and security labels dumped.
	 *
	 * Note that any initial ACLs (see pg_init_privs) will be removed when we
	 * extract the information about the object.  We don't provide support for
	 * initial policies and security labels and it seems unlikely for those to
	 * ever exist, but we may have to revisit this later.
	 *
	 * Prior to 9.6, we do not include any extension member components.
	 *
	 * In binary upgrades, we still dump all components of the members
	 * individually, since the idea is to exactly reproduce the database
	 * contents rather than replace the extension contents with something
	 * different.
	 */
	if (fout->dopt->binary_upgrade)
		dobj->dump = ext->dobj.dump;
	else
	{
		if (fout->remoteVersion < 90600)
			dobj->dump = DUMP_COMPONENT_NONE;
		else
			dobj->dump = ext->dobj.dump_contains & (DUMP_COMPONENT_ACL |
													DUMP_COMPONENT_SECLABEL |
													DUMP_COMPONENT_POLICY);
	}

	return true;
}

/*
 * selectDumpableNamespace: policy-setting subroutine
 *		Mark a namespace as to be dumped or not
 */
static void
selectDumpableNamespace(NamespaceInfo *nsinfo, Archive *fout)
{
	/*
	 * If specific tables are being dumped, do not dump any complete
	 * namespaces. If specific namespaces are being dumped, dump just those
	 * namespaces. Otherwise, dump all non-system namespaces.
	 */
	if (table_include_oids.head != NULL)
		nsinfo->dobj.dump_contains = nsinfo->dobj.dump = DUMP_COMPONENT_NONE;
	else if (schema_include_oids.head != NULL)
		nsinfo->dobj.dump_contains = nsinfo->dobj.dump =
			simple_oid_list_member(&schema_include_oids,
								   nsinfo->dobj.catId.oid) ?
			DUMP_COMPONENT_ALL : DUMP_COMPONENT_NONE;
	else if (fout->remoteVersion >= 90600 &&
			 strcmp(nsinfo->dobj.name, "pg_catalog") == 0)
	{
		/*
		 * In 9.6 and above, we dump out any ACLs defined in pg_catalog, if
		 * they are interesting (and not the original ACLs which were set at
		 * initdb time, see pg_init_privs).
		 */
		nsinfo->dobj.dump_contains = nsinfo->dobj.dump = DUMP_COMPONENT_ACL;
	}
	else if (strncmp(nsinfo->dobj.name, "pg_", 3) == 0 ||
			 strcmp(nsinfo->dobj.name, "information_schema") == 0)
	{
		/* Other system schemas don't get dumped */
		nsinfo->dobj.dump_contains = nsinfo->dobj.dump = DUMP_COMPONENT_NONE;
	}
	else if (strcmp(nsinfo->dobj.name, "public") == 0)
	{
		/*
		 * The public schema is a strange beast that sits in a sort of
		 * no-mans-land between being a system object and a user object.  We
		 * don't want to dump creation or comment commands for it, because
		 * that complicates matters for non-superuser use of pg_dump.  But we
		 * should dump any ACL changes that have occurred for it, and of
		 * course we should dump contained objects.
		 */
		nsinfo->dobj.dump = DUMP_COMPONENT_ACL;
		nsinfo->dobj.dump_contains = DUMP_COMPONENT_ALL;
	}
	else
		nsinfo->dobj.dump_contains = nsinfo->dobj.dump = DUMP_COMPONENT_ALL;

	/*
	 * In any case, a namespace can be excluded by an exclusion switch
	 */
	if (nsinfo->dobj.dump_contains &&
		simple_oid_list_member(&schema_exclude_oids,
							   nsinfo->dobj.catId.oid))
		nsinfo->dobj.dump_contains = nsinfo->dobj.dump = DUMP_COMPONENT_NONE;

	/*
	 * If the schema belongs to an extension, allow extension membership to
	 * override the dump decision for the schema itself.  However, this does
	 * not change dump_contains, so this won't change what we do with objects
	 * within the schema.  (If they belong to the extension, they'll get
	 * suppressed by it, otherwise not.)
	 */
	(void) checkExtensionMembership(&nsinfo->dobj, fout);
}

/*
 * selectDumpableTable: policy-setting subroutine
 *		Mark a table as to be dumped or not
 */
static void
selectDumpableTable(TableInfo *tbinfo, Archive *fout)
{
	if (checkExtensionMembership(&tbinfo->dobj, fout))
		return;					/* extension membership overrides all else */

	/*
	 * If specific tables are being dumped, dump just those tables; else, dump
	 * according to the parent namespace's dump flag.
	 */
	if (table_include_oids.head != NULL)
		tbinfo->dobj.dump = simple_oid_list_member(&table_include_oids,
												   tbinfo->dobj.catId.oid) ?
			DUMP_COMPONENT_ALL : DUMP_COMPONENT_NONE;
	else
		tbinfo->dobj.dump = tbinfo->dobj.namespace->dobj.dump_contains;

	/*
	 * In any case, a table can be excluded by an exclusion switch
	 */
	if (tbinfo->dobj.dump &&
		simple_oid_list_member(&table_exclude_oids,
							   tbinfo->dobj.catId.oid))
		tbinfo->dobj.dump = DUMP_COMPONENT_NONE;
}

/*
 * selectDumpableType: policy-setting subroutine
 *		Mark a type as to be dumped or not
 *
 * If it's a table's rowtype or an autogenerated array type, we also apply a
 * special type code to facilitate sorting into the desired order.  (We don't
 * want to consider those to be ordinary types because that would bring tables
 * up into the datatype part of the dump order.)  We still set the object's
 * dump flag; that's not going to cause the dummy type to be dumped, but we
 * need it so that casts involving such types will be dumped correctly -- see
 * dumpCast.  This means the flag should be set the same as for the underlying
 * object (the table or base type).
 */
static void
selectDumpableType(TypeInfo *tyinfo, Archive *fout)
{
	/* skip complex types, except for standalone composite types */
	if (OidIsValid(tyinfo->typrelid) &&
		tyinfo->typrelkind != RELKIND_COMPOSITE_TYPE)
	{
		TableInfo  *tytable = findTableByOid(tyinfo->typrelid);

		tyinfo->dobj.objType = DO_DUMMY_TYPE;
		if (tytable != NULL)
			tyinfo->dobj.dump = tytable->dobj.dump;
		else
			tyinfo->dobj.dump = DUMP_COMPONENT_NONE;
		return;
	}

	/* skip auto-generated array types */
	if (tyinfo->isArray)
	{
		tyinfo->dobj.objType = DO_DUMMY_TYPE;

		/*
		 * Fall through to set the dump flag; we assume that the subsequent
		 * rules will do the same thing as they would for the array's base
		 * type.  (We cannot reliably look up the base type here, since
		 * getTypes may not have processed it yet.)
		 */
	}

	if (checkExtensionMembership(&tyinfo->dobj, fout))
		return;					/* extension membership overrides all else */

	/* Dump based on if the contents of the namespace are being dumped */
	tyinfo->dobj.dump = tyinfo->dobj.namespace->dobj.dump_contains;
}

/*
 * selectDumpableDefaultACL: policy-setting subroutine
 *		Mark a default ACL as to be dumped or not
 *
 * For per-schema default ACLs, dump if the schema is to be dumped.
 * Otherwise dump if we are dumping "everything".  Note that dataOnly
 * and aclsSkip are checked separately.
 */
static void
selectDumpableDefaultACL(DefaultACLInfo *dinfo, DumpOptions *dopt)
{
	/* Default ACLs can't be extension members */

	if (dinfo->dobj.namespace)
		/* default ACLs are considered part of the namespace */
		dinfo->dobj.dump = dinfo->dobj.namespace->dobj.dump_contains;
	else
		dinfo->dobj.dump = dopt->include_everything ?
			DUMP_COMPONENT_ALL : DUMP_COMPONENT_NONE;
}

/*
 * selectDumpableCast: policy-setting subroutine
 *		Mark a cast as to be dumped or not
 *
 * Casts do not belong to any particular namespace (since they haven't got
 * names), nor do they have identifiable owners.  To distinguish user-defined
 * casts from built-in ones, we must resort to checking whether the cast's
 * OID is in the range reserved for initdb.
 */
static void
selectDumpableCast(CastInfo *cast, Archive *fout)
{
	if (checkExtensionMembership(&cast->dobj, fout))
		return;					/* extension membership overrides all else */

	/*
	 * This would be DUMP_COMPONENT_ACL for from-initdb casts, but they do not
	 * support ACLs currently.
	 */
	if (cast->dobj.catId.oid <= (Oid) g_last_builtin_oid)
		cast->dobj.dump = DUMP_COMPONENT_NONE;
	else
		cast->dobj.dump = fout->dopt->include_everything ?
			DUMP_COMPONENT_ALL : DUMP_COMPONENT_NONE;
}

/*
 * selectDumpableProcLang: policy-setting subroutine
 *		Mark a procedural language as to be dumped or not
 *
 * Procedural languages do not belong to any particular namespace.  To
 * identify built-in languages, we must resort to checking whether the
 * language's OID is in the range reserved for initdb.
 */
static void
selectDumpableProcLang(ProcLangInfo *plang, Archive *fout)
{
	if (checkExtensionMembership(&plang->dobj, fout))
		return;					/* extension membership overrides all else */

	/*
	 * Only include procedural languages when we are dumping everything.
	 *
	 * For from-initdb procedural languages, only include ACLs, as we do for
	 * the pg_catalog namespace.  We need this because procedural languages do
	 * not live in any namespace.
	 */
	if (!fout->dopt->include_everything)
		plang->dobj.dump = DUMP_COMPONENT_NONE;
	else
	{
		if (plang->dobj.catId.oid <= (Oid) g_last_builtin_oid)
			plang->dobj.dump = fout->remoteVersion < 90600 ?
				DUMP_COMPONENT_NONE : DUMP_COMPONENT_ACL;
		else
			plang->dobj.dump = DUMP_COMPONENT_ALL;
	}
}

/*
 * selectDumpableAccessMethod: policy-setting subroutine
 *		Mark an access method as to be dumped or not
 *
 * Access methods do not belong to any particular namespace.  To identify
 * built-in access methods, we must resort to checking whether the
 * method's OID is in the range reserved for initdb.
 */
static void
selectDumpableAccessMethod(AccessMethodInfo *method, Archive *fout)
{
	if (checkExtensionMembership(&method->dobj, fout))
		return;					/* extension membership overrides all else */

	/*
	 * This would be DUMP_COMPONENT_ACL for from-initdb access methods, but
	 * they do not support ACLs currently.
	 */
	if (method->dobj.catId.oid <= (Oid) g_last_builtin_oid)
		method->dobj.dump = DUMP_COMPONENT_NONE;
	else
		method->dobj.dump = fout->dopt->include_everything ?
			DUMP_COMPONENT_ALL : DUMP_COMPONENT_NONE;
}

/*
 * selectDumpableExtension: policy-setting subroutine
 *		Mark an extension as to be dumped or not
 *
 * Built-in extensions should be skipped except for checking ACLs, since we
 * assume those will already be installed in the target database.  We identify
 * such extensions by their having OIDs in the range reserved for initdb.
 * We dump all user-added extensions by default, or none of them if
 * include_everything is false (i.e., a --schema or --table switch was given).
 */
static void
selectDumpableExtension(ExtensionInfo *extinfo, DumpOptions *dopt)
{
	/*
	 * Use DUMP_COMPONENT_ACL for built-in extensions, to allow users to
	 * change permissions on their member objects, if they wish to, and have
	 * those changes preserved.
	 */
	if (extinfo->dobj.catId.oid <= (Oid) g_last_builtin_oid)
		extinfo->dobj.dump = extinfo->dobj.dump_contains = DUMP_COMPONENT_ACL;
	else
		extinfo->dobj.dump = extinfo->dobj.dump_contains =
			dopt->include_everything ? DUMP_COMPONENT_ALL :
			DUMP_COMPONENT_NONE;
}

/*
 * selectDumpablePublicationTable: policy-setting subroutine
 *		Mark a publication table as to be dumped or not
 *
 * Publication tables have schemas, but those are ignored in decision making,
 * because publications are only dumped when we are dumping everything.
 */
static void
selectDumpablePublicationTable(DumpableObject *dobj, Archive *fout)
{
	if (checkExtensionMembership(dobj, fout))
		return;					/* extension membership overrides all else */

	dobj->dump = fout->dopt->include_everything ?
		DUMP_COMPONENT_ALL : DUMP_COMPONENT_NONE;
}

/*
 * selectDumpableObject: policy-setting subroutine
 *		Mark a generic dumpable object as to be dumped or not
 *
 * Use this only for object types without a special-case routine above.
 */
static void
selectDumpableObject(DumpableObject *dobj, Archive *fout)
{
	if (checkExtensionMembership(dobj, fout))
		return;					/* extension membership overrides all else */

	/*
	 * Default policy is to dump if parent namespace is dumpable, or for
	 * non-namespace-associated items, dump if we're dumping "everything".
	 */
	if (dobj->namespace)
		dobj->dump = dobj->namespace->dobj.dump_contains;
	else
		dobj->dump = fout->dopt->include_everything ?
			DUMP_COMPONENT_ALL : DUMP_COMPONENT_NONE;
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
	PQExpBuffer q = createPQExpBuffer();

	/*
	 * Note: can't use getThreadLocalPQExpBuffer() here, we're calling fmtId
	 * which uses it already.
	 */
	PQExpBuffer clistBuf = createPQExpBuffer();
	PGconn	   *conn = GetConnection(fout);
	PGresult   *res;
	int			ret;
	char	   *copybuf;
	const char *column_list;

	pg_log_info("dumping contents of table \"%s.%s\"",
				tbinfo->dobj.namespace->dobj.name, classname);

	/*
	 * Specify the column list explicitly so that we have no possibility of
	 * retrieving data in the wrong column order.  (The default column
	 * ordering of COPY will not be what we want in certain corner cases
	 * involving ADD COLUMN and inheritance.)
	 */
	column_list = fmtCopyColumnList(tbinfo, clistBuf);

	/*
	 * Use COPY (SELECT ...) TO when dumping a foreign table's data, and when
	 * a filter condition was specified.  For other cases a simple COPY
	 * suffices.
	 */
	if (tdinfo->filtercond || tbinfo->relkind == RELKIND_FOREIGN_TABLE)
	{
		/* Note: this syntax is only supported in 8.2 and up */
		appendPQExpBufferStr(q, "COPY (SELECT ");
		/* klugery to get rid of parens in column list */
		if (strlen(column_list) > 2)
		{
			appendPQExpBufferStr(q, column_list + 1);
			q->data[q->len - 1] = ' ';
		}
		else
			appendPQExpBufferStr(q, "* ");

		appendPQExpBuffer(q, "FROM %s %s) TO stdout;",
						  fmtQualifiedDumpable(tbinfo),
						  tdinfo->filtercond ? tdinfo->filtercond : "");
	}
	else
	{
		appendPQExpBuffer(q, "COPY %s %s TO stdout;",
						  fmtQualifiedDumpable(tbinfo),
						  column_list);
	}
	res = ExecuteSqlQuery(fout, q->data, PGRES_COPY_OUT);
	PQclear(res);
	destroyPQExpBuffer(clistBuf);

	for (;;)
	{
		ret = PQgetCopyData(conn, &copybuf, 0);

		if (ret < 0)
			break;				/* done or error */

		if (copybuf)
		{
			WriteData(fout, copybuf, ret);
			PQfreemem(copybuf);
		}

		/* ----------
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
		 * If throttle is non-zero, then
		 *		See how long since the last sleep.
		 *		Work out how long to sleep (based on ratio).
		 *		If sleep is more than 100ms, then
		 *			sleep
		 *			reset timer
		 *		EndIf
		 * EndIf
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
		 * ----------
		 */
	}
	archprintf(fout, "\\.\n\n\n");

	if (ret == -2)
	{
		/* copy data transfer failed */
		pg_log_error("Dumping the contents of table \"%s\" failed: PQgetCopyData() failed.", classname);
		pg_log_error("Error message from server: %s", PQerrorMessage(conn));
		pg_log_error("The command was: %s", q->data);
		exit_nicely(1);
	}

	/* Check command status and return to normal libpq state */
	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		pg_log_error("Dumping the contents of table \"%s\" failed: PQgetResult() failed.", classname);
		pg_log_error("Error message from server: %s", PQerrorMessage(conn));
		pg_log_error("The command was: %s", q->data);
		exit_nicely(1);
	}
	PQclear(res);

	/* Do this to ensure we've pumped libpq back to idle state */
	if (PQgetResult(conn) != NULL)
		pg_log_warning("unexpected extra results during COPY of table \"%s\"",
					   classname);

	destroyPQExpBuffer(q);
	return 1;
}

/*
 * Dump table data using INSERT commands.
 *
 * Caution: when we restore from an archive file direct to database, the
 * INSERT commands emitted by this function have to be parsed by
 * pg_backup_db.c's ExecuteSimpleCommands(), which will not handle comments,
 * E'' strings, or dollar-quoted strings.  So don't emit anything like that.
 */
static int
dumpTableData_insert(Archive *fout, void *dcontext)
{
	TableDataInfo *tdinfo = (TableDataInfo *) dcontext;
	TableInfo  *tbinfo = tdinfo->tdtable;
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer insertStmt = NULL;
	PGresult   *res;
	int			nfields;
	int			rows_per_statement = dopt->dump_inserts;
	int			rows_this_statement = 0;

	appendPQExpBuffer(q, "DECLARE _pg_dump_cursor CURSOR FOR "
					  "SELECT * FROM ONLY %s",
					  fmtQualifiedDumpable(tbinfo));
	if (tdinfo->filtercond)
		appendPQExpBuffer(q, " %s", tdinfo->filtercond);

	ExecuteSqlStatement(fout, q->data);

	while (1)
	{
		res = ExecuteSqlQuery(fout, "FETCH 100 FROM _pg_dump_cursor",
							  PGRES_TUPLES_OK);
		nfields = PQnfields(res);

		/*
		 * First time through, we build as much of the INSERT statement as
		 * possible in "insertStmt", which we can then just print for each
		 * statement. If the table happens to have zero columns then this will
		 * be a complete statement, otherwise it will end in "VALUES" and be
		 * ready to have the row's column values printed.
		 */
		if (insertStmt == NULL)
		{
			TableInfo  *targettab;

			insertStmt = createPQExpBuffer();

			/*
			 * When load-via-partition-root is set, get the root table name
			 * for the partition table, so that we can reload data through the
			 * root table.
			 */
			if (dopt->load_via_partition_root && tbinfo->ispartition)
				targettab = getRootTableInfo(tbinfo);
			else
				targettab = tbinfo;

			appendPQExpBuffer(insertStmt, "INSERT INTO %s ",
							  fmtQualifiedDumpable(targettab));

			/* corner case for zero-column table */
			if (nfields == 0)
			{
				appendPQExpBufferStr(insertStmt, "DEFAULT VALUES;\n");
			}
			else
			{
				/* append the list of column names if required */
				if (dopt->column_inserts)
				{
					appendPQExpBufferChar(insertStmt, '(');
					for (int field = 0; field < nfields; field++)
					{
						if (field > 0)
							appendPQExpBufferStr(insertStmt, ", ");
						appendPQExpBufferStr(insertStmt,
											 fmtId(PQfname(res, field)));
					}
					appendPQExpBufferStr(insertStmt, ") ");
				}

				if (tbinfo->needs_override)
					appendPQExpBufferStr(insertStmt, "OVERRIDING SYSTEM VALUE ");

				appendPQExpBufferStr(insertStmt, "VALUES");
			}
		}

		for (int tuple = 0; tuple < PQntuples(res); tuple++)
		{
			/* Write the INSERT if not in the middle of a multi-row INSERT. */
			if (rows_this_statement == 0)
				archputs(insertStmt->data, fout);

			/*
			 * If it is zero-column table then we've already written the
			 * complete statement, which will mean we've disobeyed
			 * --rows-per-insert when it's set greater than 1.  We do support
			 * a way to make this multi-row with: SELECT UNION ALL SELECT
			 * UNION ALL ... but that's non-standard so we should avoid it
			 * given that using INSERTs is mostly only ever needed for
			 * cross-database exports.
			 */
			if (nfields == 0)
				continue;

			/* Emit a row heading */
			if (rows_per_statement == 1)
				archputs(" (", fout);
			else if (rows_this_statement > 0)
				archputs(",\n\t(", fout);
			else
				archputs("\n\t(", fout);

			for (int field = 0; field < nfields; field++)
			{
				if (field > 0)
					archputs(", ", fout);
				if (tbinfo->attgenerated[field])
				{
					archputs("DEFAULT", fout);
					continue;
				}
				if (PQgetisnull(res, tuple, field))
				{
					archputs("NULL", fout);
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
							 * scanner unquoted (e.g., 'NaN').  Note that
							 * strtod() and friends might accept NaN, so we
							 * can't use that to test.
							 *
							 * In reality we only need to defend against
							 * infinity and NaN, so we need not get too crazy
							 * about pattern matching here.
							 */
							const char *s = PQgetvalue(res, tuple, field);

							if (strspn(s, "0123456789 +-eE.") == strlen(s))
								archputs(s, fout);
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
							archputs("true", fout);
						else
							archputs("false", fout);
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

			/* Terminate the row ... */
			archputs(")", fout);

			/* ... and the statement, if the target no. of rows is reached */
			if (++rows_this_statement >= rows_per_statement)
			{
				if (dopt->do_nothing)
					archputs(" ON CONFLICT DO NOTHING;\n", fout);
				else
					archputs(";\n", fout);
				/* Reset the row counter */
				rows_this_statement = 0;
			}
		}

		if (PQntuples(res) <= 0)
		{
			PQclear(res);
			break;
		}
		PQclear(res);
	}

	/* Terminate any statements that didn't make the row count. */
	if (rows_this_statement > 0)
	{
		if (dopt->do_nothing)
			archputs(" ON CONFLICT DO NOTHING;\n", fout);
		else
			archputs(";\n", fout);
	}

	archputs("\n\n", fout);

	ExecuteSqlStatement(fout, "CLOSE _pg_dump_cursor");

	destroyPQExpBuffer(q);
	if (insertStmt != NULL)
		destroyPQExpBuffer(insertStmt);

	return 1;
}

/*
 * getRootTableInfo:
 *     get the root TableInfo for the given partition table.
 */
static TableInfo *
getRootTableInfo(TableInfo *tbinfo)
{
	TableInfo  *parentTbinfo;

	Assert(tbinfo->ispartition);
	Assert(tbinfo->numParents == 1);

	parentTbinfo = tbinfo->parents[0];
	while (parentTbinfo->ispartition)
	{
		Assert(parentTbinfo->numParents == 1);
		parentTbinfo = parentTbinfo->parents[0];
	}

	return parentTbinfo;
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
	DumpOptions *dopt = fout->dopt;
	TableInfo  *tbinfo = tdinfo->tdtable;
	PQExpBuffer copyBuf = createPQExpBuffer();
	PQExpBuffer clistBuf = createPQExpBuffer();
	DataDumperPtr dumpFn;
	char	   *copyStmt;
	const char *copyFrom;

	/* We had better have loaded per-column details about this table */
	Assert(tbinfo->interesting);

	if (dopt->dump_inserts == 0)
	{
		/* Dump/restore using COPY */
		dumpFn = dumpTableData_copy;

		/*
		 * When load-via-partition-root is set, get the root table name for
		 * the partition table, so that we can reload data through the root
		 * table.
		 */
		if (dopt->load_via_partition_root && tbinfo->ispartition)
		{
			TableInfo  *parentTbinfo;

			parentTbinfo = getRootTableInfo(tbinfo);
			copyFrom = fmtQualifiedDumpable(parentTbinfo);
		}
		else
			copyFrom = fmtQualifiedDumpable(tbinfo);

		/* must use 2 steps here 'cause fmtId is nonreentrant */
		appendPQExpBuffer(copyBuf, "COPY %s ",
						  copyFrom);
		appendPQExpBuffer(copyBuf, "%s FROM stdin;\n",
						  fmtCopyColumnList(tbinfo, clistBuf));
		copyStmt = copyBuf->data;
	}
	else
	{
		/* Restore using INSERT */
		dumpFn = dumpTableData_insert;
		copyStmt = NULL;
	}

	/*
	 * Note: although the TableDataInfo is a full DumpableObject, we treat its
	 * dependency on its table as "special" and pass it to ArchiveEntry now.
	 * See comments for BuildArchiveDependencies.
	 */
	if (tdinfo->dobj.dump & DUMP_COMPONENT_DATA)
	{
		TocEntry   *te;

		te = ArchiveEntry(fout, tdinfo->dobj.catId, tdinfo->dobj.dumpId,
						  ARCHIVE_OPTS(.tag = tbinfo->dobj.name,
									   .namespace = tbinfo->dobj.namespace->dobj.name,
									   .owner = tbinfo->rolname,
									   .description = "TABLE DATA",
									   .section = SECTION_DATA,
									   .copyStmt = copyStmt,
									   .deps = &(tbinfo->dobj.dumpId),
									   .nDeps = 1,
									   .dumpFn = dumpFn,
									   .dumpArg = tdinfo));

		/*
		 * Set the TocEntry's dataLength in case we are doing a parallel dump
		 * and want to order dump jobs by table size.  We choose to measure
		 * dataLength in table pages during dump, so no scaling is needed.
		 * However, relpages is declared as "integer" in pg_class, and hence
		 * also in TableInfo, but it's really BlockNumber a/k/a unsigned int.
		 * Cast so that we get the right interpretation of table sizes
		 * exceeding INT_MAX pages.
		 */
		te->dataLength = (BlockNumber) tbinfo->relpages;
	}

	destroyPQExpBuffer(copyBuf);
	destroyPQExpBuffer(clistBuf);
}

/*
 * refreshMatViewData -
 *	  load or refresh the contents of a single materialized view
 *
 * Actually, this just makes an ArchiveEntry for the REFRESH MATERIALIZED VIEW
 * statement.
 */
static void
refreshMatViewData(Archive *fout, TableDataInfo *tdinfo)
{
	TableInfo  *tbinfo = tdinfo->tdtable;
	PQExpBuffer q;

	/* If the materialized view is not flagged as populated, skip this. */
	if (!tbinfo->relispopulated)
		return;

	q = createPQExpBuffer();

	appendPQExpBuffer(q, "REFRESH MATERIALIZED VIEW %s;\n",
					  fmtQualifiedDumpable(tbinfo));

	if (tdinfo->dobj.dump & DUMP_COMPONENT_DATA)
		ArchiveEntry(fout,
					 tdinfo->dobj.catId,	/* catalog ID */
					 tdinfo->dobj.dumpId,	/* dump ID */
					 ARCHIVE_OPTS(.tag = tbinfo->dobj.name,
								  .namespace = tbinfo->dobj.namespace->dobj.name,
								  .owner = tbinfo->rolname,
								  .description = "MATERIALIZED VIEW DATA",
								  .section = SECTION_POST_DATA,
								  .createStmt = q->data,
								  .deps = tdinfo->dobj.dependencies,
								  .nDeps = tdinfo->dobj.nDeps));

	destroyPQExpBuffer(q);
}

/*
 * getTableData -
 *	  set up dumpable objects representing the contents of tables
 */
static void
getTableData(DumpOptions *dopt, TableInfo *tblinfo, int numTables, char relkind)
{
	int			i;

	for (i = 0; i < numTables; i++)
	{
		if (tblinfo[i].dobj.dump & DUMP_COMPONENT_DATA &&
			(!relkind || tblinfo[i].relkind == relkind))
			makeTableDataInfo(dopt, &(tblinfo[i]));
	}
}

/*
 * Make a dumpable object for the data of this specific table
 *
 * Note: we make a TableDataInfo if and only if we are going to dump the
 * table data; the "dump" flag in such objects isn't used.
 */
static void
makeTableDataInfo(DumpOptions *dopt, TableInfo *tbinfo)
{
	TableDataInfo *tdinfo;

	/*
	 * Nothing to do if we already decided to dump the table.  This will
	 * happen for "config" tables.
	 */
	if (tbinfo->dataObj != NULL)
		return;

	/* Skip VIEWs (no data to dump) */
	if (tbinfo->relkind == RELKIND_VIEW)
		return;
	/* Skip FOREIGN TABLEs (no data to dump) unless requested explicitly */
	if (tbinfo->relkind == RELKIND_FOREIGN_TABLE &&
		(foreign_servers_include_oids.head == NULL ||
		 !simple_oid_list_member(&foreign_servers_include_oids,
								 tbinfo->foreign_server)))
		return;
	/* Skip partitioned tables (data in partitions) */
	if (tbinfo->relkind == RELKIND_PARTITIONED_TABLE)
		return;

	/* Don't dump data in unlogged tables, if so requested */
	if (tbinfo->relpersistence == RELPERSISTENCE_UNLOGGED &&
		dopt->no_unlogged_table_data)
		return;

	/* Check that the data is not explicitly excluded */
	if (simple_oid_list_member(&tabledata_exclude_oids,
							   tbinfo->dobj.catId.oid))
		return;

	/* OK, let's dump it */
	tdinfo = (TableDataInfo *) pg_malloc(sizeof(TableDataInfo));

	if (tbinfo->relkind == RELKIND_MATVIEW)
		tdinfo->dobj.objType = DO_REFRESH_MATVIEW;
	else if (tbinfo->relkind == RELKIND_SEQUENCE)
		tdinfo->dobj.objType = DO_SEQUENCE_SET;
	else
		tdinfo->dobj.objType = DO_TABLE_DATA;

	/*
	 * Note: use tableoid 0 so that this object won't be mistaken for
	 * something that pg_depend entries apply to.
	 */
	tdinfo->dobj.catId.tableoid = 0;
	tdinfo->dobj.catId.oid = tbinfo->dobj.catId.oid;
	AssignDumpId(&tdinfo->dobj);
	tdinfo->dobj.name = tbinfo->dobj.name;
	tdinfo->dobj.namespace = tbinfo->dobj.namespace;
	tdinfo->tdtable = tbinfo;
	tdinfo->filtercond = NULL;	/* might get set later */
	addObjectDependency(&tdinfo->dobj, tbinfo->dobj.dumpId);

	tbinfo->dataObj = tdinfo;

	/* Make sure that we'll collect per-column info for this table. */
	tbinfo->interesting = true;
}

/*
 * The refresh for a materialized view must be dependent on the refresh for
 * any materialized view that this one is dependent on.
 *
 * This must be called after all the objects are created, but before they are
 * sorted.
 */
static void
buildMatViewRefreshDependencies(Archive *fout)
{
	PQExpBuffer query;
	PGresult   *res;
	int			ntups,
				i;
	int			i_classid,
				i_objid,
				i_refobjid;

	/* No Mat Views before 9.3. */
	if (fout->remoteVersion < 90300)
		return;

	query = createPQExpBuffer();

	appendPQExpBufferStr(query, "WITH RECURSIVE w AS "
						 "( "
						 "SELECT d1.objid, d2.refobjid, c2.relkind AS refrelkind "
						 "FROM pg_depend d1 "
						 "JOIN pg_class c1 ON c1.oid = d1.objid "
						 "AND c1.relkind = " CppAsString2(RELKIND_MATVIEW)
						 " JOIN pg_rewrite r1 ON r1.ev_class = d1.objid "
						 "JOIN pg_depend d2 ON d2.classid = 'pg_rewrite'::regclass "
						 "AND d2.objid = r1.oid "
						 "AND d2.refobjid <> d1.objid "
						 "JOIN pg_class c2 ON c2.oid = d2.refobjid "
						 "AND c2.relkind IN (" CppAsString2(RELKIND_MATVIEW) ","
						 CppAsString2(RELKIND_VIEW) ") "
						 "WHERE d1.classid = 'pg_class'::regclass "
						 "UNION "
						 "SELECT w.objid, d3.refobjid, c3.relkind "
						 "FROM w "
						 "JOIN pg_rewrite r3 ON r3.ev_class = w.refobjid "
						 "JOIN pg_depend d3 ON d3.classid = 'pg_rewrite'::regclass "
						 "AND d3.objid = r3.oid "
						 "AND d3.refobjid <> w.refobjid "
						 "JOIN pg_class c3 ON c3.oid = d3.refobjid "
						 "AND c3.relkind IN (" CppAsString2(RELKIND_MATVIEW) ","
						 CppAsString2(RELKIND_VIEW) ") "
						 ") "
						 "SELECT 'pg_class'::regclass::oid AS classid, objid, refobjid "
						 "FROM w "
						 "WHERE refrelkind = " CppAsString2(RELKIND_MATVIEW));

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_classid = PQfnumber(res, "classid");
	i_objid = PQfnumber(res, "objid");
	i_refobjid = PQfnumber(res, "refobjid");

	for (i = 0; i < ntups; i++)
	{
		CatalogId	objId;
		CatalogId	refobjId;
		DumpableObject *dobj;
		DumpableObject *refdobj;
		TableInfo  *tbinfo;
		TableInfo  *reftbinfo;

		objId.tableoid = atooid(PQgetvalue(res, i, i_classid));
		objId.oid = atooid(PQgetvalue(res, i, i_objid));
		refobjId.tableoid = objId.tableoid;
		refobjId.oid = atooid(PQgetvalue(res, i, i_refobjid));

		dobj = findObjectByCatalogId(objId);
		if (dobj == NULL)
			continue;

		Assert(dobj->objType == DO_TABLE);
		tbinfo = (TableInfo *) dobj;
		Assert(tbinfo->relkind == RELKIND_MATVIEW);
		dobj = (DumpableObject *) tbinfo->dataObj;
		if (dobj == NULL)
			continue;
		Assert(dobj->objType == DO_REFRESH_MATVIEW);

		refdobj = findObjectByCatalogId(refobjId);
		if (refdobj == NULL)
			continue;

		Assert(refdobj->objType == DO_TABLE);
		reftbinfo = (TableInfo *) refdobj;
		Assert(reftbinfo->relkind == RELKIND_MATVIEW);
		refdobj = (DumpableObject *) reftbinfo->dataObj;
		if (refdobj == NULL)
			continue;
		Assert(refdobj->objType == DO_REFRESH_MATVIEW);

		addObjectDependency(dobj, refdobj->dumpId);

		if (!reftbinfo->relispopulated)
			tbinfo->relispopulated = false;
	}

	PQclear(res);

	destroyPQExpBuffer(query);
}

/*
 * getTableDataFKConstraints -
 *	  add dump-order dependencies reflecting foreign key constraints
 *
 * This code is executed only in a data-only dump --- in schema+data dumps
 * we handle foreign key issues by not creating the FK constraints until
 * after the data is loaded.  In a data-only dump, however, we want to
 * order the table data objects in such a way that a table's referenced
 * tables are restored first.  (In the presence of circular references or
 * self-references this may be impossible; we'll detect and complain about
 * that during the dependency sorting step.)
 */
static void
getTableDataFKConstraints(void)
{
	DumpableObject **dobjs;
	int			numObjs;
	int			i;

	/* Search through all the dumpable objects for FK constraints */
	getDumpableObjects(&dobjs, &numObjs);
	for (i = 0; i < numObjs; i++)
	{
		if (dobjs[i]->objType == DO_FK_CONSTRAINT)
		{
			ConstraintInfo *cinfo = (ConstraintInfo *) dobjs[i];
			TableInfo  *ftable;

			/* Not interesting unless both tables are to be dumped */
			if (cinfo->contable == NULL ||
				cinfo->contable->dataObj == NULL)
				continue;
			ftable = findTableByOid(cinfo->confrelid);
			if (ftable == NULL ||
				ftable->dataObj == NULL)
				continue;

			/*
			 * Okay, make referencing table's TABLE_DATA object depend on the
			 * referenced table's TABLE_DATA object.
			 */
			addObjectDependency(&cinfo->contable->dataObj->dobj,
								ftable->dataObj->dobj.dumpId);
		}
	}
	free(dobjs);
}


/*
 * guessConstraintInheritance:
 *	In pre-8.4 databases, we can't tell for certain which constraints
 *	are inherited.  We assume a CHECK constraint is inherited if its name
 *	matches the name of any constraint in the parent.  Originally this code
 *	tried to compare the expression texts, but that can fail for various
 *	reasons --- for example, if the parent and child tables are in different
 *	schemas, reverse-listing of function calls may produce different text
 *	(schema-qualified or not) depending on search path.
 *
 *	In 8.4 and up we can rely on the conislocal field to decide which
 *	constraints must be dumped; much safer.
 *
 *	This function assumes all conislocal flags were initialized to true.
 *	It clears the flag on anything that seems to be inherited.
 */
static void
guessConstraintInheritance(TableInfo *tblinfo, int numTables)
{
	int			i,
				j,
				k;

	for (i = 0; i < numTables; i++)
	{
		TableInfo  *tbinfo = &(tblinfo[i]);
		int			numParents;
		TableInfo **parents;
		TableInfo  *parent;

		/* Sequences and views never have parents */
		if (tbinfo->relkind == RELKIND_SEQUENCE ||
			tbinfo->relkind == RELKIND_VIEW)
			continue;

		/* Don't bother computing anything for non-target tables, either */
		if (!(tbinfo->dobj.dump & DUMP_COMPONENT_DEFINITION))
			continue;

		numParents = tbinfo->numParents;
		parents = tbinfo->parents;

		if (numParents == 0)
			continue;			/* nothing to see here, move along */

		/* scan for inherited CHECK constraints */
		for (j = 0; j < tbinfo->ncheck; j++)
		{
			ConstraintInfo *constr;

			constr = &(tbinfo->checkexprs[j]);

			for (k = 0; k < numParents; k++)
			{
				int			l;

				parent = parents[k];
				for (l = 0; l < parent->ncheck; l++)
				{
					ConstraintInfo *pconstr = &(parent->checkexprs[l]);

					if (strcmp(pconstr->dobj.name, constr->dobj.name) == 0)
					{
						constr->conislocal = false;
						break;
					}
				}
				if (!constr->conislocal)
					break;
			}
		}
	}
}


/*
 * dumpDatabase:
 *	dump the database definition
 */
static void
dumpDatabase(Archive *fout)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer dbQry = createPQExpBuffer();
	PQExpBuffer delQry = createPQExpBuffer();
	PQExpBuffer creaQry = createPQExpBuffer();
	PQExpBuffer labelq = createPQExpBuffer();
	PGconn	   *conn = GetConnection(fout);
	PGresult   *res;
	int			i_tableoid,
				i_oid,
				i_datname,
				i_dba,
				i_encoding,
				i_collate,
				i_ctype,
				i_frozenxid,
				i_minmxid,
				i_datacl,
				i_rdatacl,
				i_datistemplate,
				i_datconnlimit,
				i_tablespace;
	CatalogId	dbCatId;
	DumpId		dbDumpId;
	const char *datname,
			   *dba,
			   *encoding,
			   *collate,
			   *ctype,
			   *datacl,
			   *rdatacl,
			   *datistemplate,
			   *datconnlimit,
			   *tablespace;
	uint32		frozenxid,
				minmxid;
	char	   *qdatname;

	pg_log_info("saving database definition");

	/*
	 * Fetch the database-level properties for this database.
	 *
	 * The order in which privileges are in the ACL string (the order they
	 * have been GRANT'd in, which the backend maintains) must be preserved to
	 * ensure that GRANTs WITH GRANT OPTION and subsequent GRANTs based on
	 * those are dumped in the correct order.  Note that initial privileges
	 * (pg_init_privs) are not supported on databases, so this logic cannot
	 * make use of buildACLQueries().
	 */
	if (fout->remoteVersion >= 90600)
	{
		appendPQExpBuffer(dbQry, "SELECT tableoid, oid, datname, "
						  "(%s datdba) AS dba, "
						  "pg_encoding_to_char(encoding) AS encoding, "
						  "datcollate, datctype, datfrozenxid, datminmxid, "
						  "(SELECT array_agg(acl ORDER BY row_n) FROM "
						  "  (SELECT acl, row_n FROM "
						  "     unnest(coalesce(datacl,acldefault('d',datdba))) "
						  "     WITH ORDINALITY AS perm(acl,row_n) "
						  "   WHERE NOT EXISTS ( "
						  "     SELECT 1 "
						  "     FROM unnest(acldefault('d',datdba)) "
						  "       AS init(init_acl) "
						  "     WHERE acl = init_acl)) AS datacls) "
						  " AS datacl, "
						  "(SELECT array_agg(acl ORDER BY row_n) FROM "
						  "  (SELECT acl, row_n FROM "
						  "     unnest(acldefault('d',datdba)) "
						  "     WITH ORDINALITY AS initp(acl,row_n) "
						  "   WHERE NOT EXISTS ( "
						  "     SELECT 1 "
						  "     FROM unnest(coalesce(datacl,acldefault('d',datdba))) "
						  "       AS permp(orig_acl) "
						  "     WHERE acl = orig_acl)) AS rdatacls) "
						  " AS rdatacl, "
						  "datistemplate, datconnlimit, "
						  "(SELECT spcname FROM pg_tablespace t WHERE t.oid = dattablespace) AS tablespace, "
						  "shobj_description(oid, 'pg_database') AS description "

						  "FROM pg_database "
						  "WHERE datname = current_database()",
						  username_subquery);
	}
	else if (fout->remoteVersion >= 90300)
	{
		appendPQExpBuffer(dbQry, "SELECT tableoid, oid, datname, "
						  "(%s datdba) AS dba, "
						  "pg_encoding_to_char(encoding) AS encoding, "
						  "datcollate, datctype, datfrozenxid, datminmxid, "
						  "datacl, '' as rdatacl, datistemplate, datconnlimit, "
						  "(SELECT spcname FROM pg_tablespace t WHERE t.oid = dattablespace) AS tablespace, "
						  "shobj_description(oid, 'pg_database') AS description "

						  "FROM pg_database "
						  "WHERE datname = current_database()",
						  username_subquery);
	}
	else if (fout->remoteVersion >= 80400)
	{
		appendPQExpBuffer(dbQry, "SELECT tableoid, oid, datname, "
						  "(%s datdba) AS dba, "
						  "pg_encoding_to_char(encoding) AS encoding, "
						  "datcollate, datctype, datfrozenxid, 0 AS datminmxid, "
						  "datacl, '' as rdatacl, datistemplate, datconnlimit, "
						  "(SELECT spcname FROM pg_tablespace t WHERE t.oid = dattablespace) AS tablespace, "
						  "shobj_description(oid, 'pg_database') AS description "

						  "FROM pg_database "
						  "WHERE datname = current_database()",
						  username_subquery);
	}
	else if (fout->remoteVersion >= 80200)
	{
		appendPQExpBuffer(dbQry, "SELECT tableoid, oid, datname, "
						  "(%s datdba) AS dba, "
						  "pg_encoding_to_char(encoding) AS encoding, "
						  "NULL AS datcollate, NULL AS datctype, datfrozenxid, 0 AS datminmxid, "
						  "datacl, '' as rdatacl, datistemplate, datconnlimit, "
						  "(SELECT spcname FROM pg_tablespace t WHERE t.oid = dattablespace) AS tablespace, "
						  "shobj_description(oid, 'pg_database') AS description "

						  "FROM pg_database "
						  "WHERE datname = current_database()",
						  username_subquery);
	}
	else
	{
		appendPQExpBuffer(dbQry, "SELECT tableoid, oid, datname, "
						  "(%s datdba) AS dba, "
						  "pg_encoding_to_char(encoding) AS encoding, "
						  "NULL AS datcollate, NULL AS datctype, datfrozenxid, 0 AS datminmxid, "
						  "datacl, '' as rdatacl, datistemplate, "
						  "-1 as datconnlimit, "
						  "(SELECT spcname FROM pg_tablespace t WHERE t.oid = dattablespace) AS tablespace "
						  "FROM pg_database "
						  "WHERE datname = current_database()",
						  username_subquery);
	}

	res = ExecuteSqlQueryForSingleRow(fout, dbQry->data);

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_datname = PQfnumber(res, "datname");
	i_dba = PQfnumber(res, "dba");
	i_encoding = PQfnumber(res, "encoding");
	i_collate = PQfnumber(res, "datcollate");
	i_ctype = PQfnumber(res, "datctype");
	i_frozenxid = PQfnumber(res, "datfrozenxid");
	i_minmxid = PQfnumber(res, "datminmxid");
	i_datacl = PQfnumber(res, "datacl");
	i_rdatacl = PQfnumber(res, "rdatacl");
	i_datistemplate = PQfnumber(res, "datistemplate");
	i_datconnlimit = PQfnumber(res, "datconnlimit");
	i_tablespace = PQfnumber(res, "tablespace");

	dbCatId.tableoid = atooid(PQgetvalue(res, 0, i_tableoid));
	dbCatId.oid = atooid(PQgetvalue(res, 0, i_oid));
	datname = PQgetvalue(res, 0, i_datname);
	dba = PQgetvalue(res, 0, i_dba);
	encoding = PQgetvalue(res, 0, i_encoding);
	collate = PQgetvalue(res, 0, i_collate);
	ctype = PQgetvalue(res, 0, i_ctype);
	frozenxid = atooid(PQgetvalue(res, 0, i_frozenxid));
	minmxid = atooid(PQgetvalue(res, 0, i_minmxid));
	datacl = PQgetvalue(res, 0, i_datacl);
	rdatacl = PQgetvalue(res, 0, i_rdatacl);
	datistemplate = PQgetvalue(res, 0, i_datistemplate);
	datconnlimit = PQgetvalue(res, 0, i_datconnlimit);
	tablespace = PQgetvalue(res, 0, i_tablespace);

	qdatname = pg_strdup(fmtId(datname));

	/*
	 * Prepare the CREATE DATABASE command.  We must specify encoding, locale,
	 * and tablespace since those can't be altered later.  Other DB properties
	 * are left to the DATABASE PROPERTIES entry, so that they can be applied
	 * after reconnecting to the target DB.
	 */
	appendPQExpBuffer(creaQry, "CREATE DATABASE %s WITH TEMPLATE = template0",
					  qdatname);
	if (strlen(encoding) > 0)
	{
		appendPQExpBufferStr(creaQry, " ENCODING = ");
		appendStringLiteralAH(creaQry, encoding, fout);
	}
	if (strlen(collate) > 0 && strcmp(collate, ctype) == 0)
	{
		appendPQExpBufferStr(creaQry, " LOCALE = ");
		appendStringLiteralAH(creaQry, collate, fout);
	}
	else
	{
		if (strlen(collate) > 0)
		{
			appendPQExpBufferStr(creaQry, " LC_COLLATE = ");
			appendStringLiteralAH(creaQry, collate, fout);
		}
		if (strlen(ctype) > 0)
		{
			appendPQExpBufferStr(creaQry, " LC_CTYPE = ");
			appendStringLiteralAH(creaQry, ctype, fout);
		}
	}

	/*
	 * Note: looking at dopt->outputNoTablespaces here is completely the wrong
	 * thing; the decision whether to specify a tablespace should be left till
	 * pg_restore, so that pg_restore --no-tablespaces applies.  Ideally we'd
	 * label the DATABASE entry with the tablespace and let the normal
	 * tablespace selection logic work ... but CREATE DATABASE doesn't pay
	 * attention to default_tablespace, so that won't work.
	 */
	if (strlen(tablespace) > 0 && strcmp(tablespace, "pg_default") != 0 &&
		!dopt->outputNoTablespaces)
		appendPQExpBuffer(creaQry, " TABLESPACE = %s",
						  fmtId(tablespace));
	appendPQExpBufferStr(creaQry, ";\n");

	appendPQExpBuffer(delQry, "DROP DATABASE %s;\n",
					  qdatname);

	dbDumpId = createDumpId();

	ArchiveEntry(fout,
				 dbCatId,		/* catalog ID */
				 dbDumpId,		/* dump ID */
				 ARCHIVE_OPTS(.tag = datname,
							  .owner = dba,
							  .description = "DATABASE",
							  .section = SECTION_PRE_DATA,
							  .createStmt = creaQry->data,
							  .dropStmt = delQry->data));

	/* Compute correct tag for archive entry */
	appendPQExpBuffer(labelq, "DATABASE %s", qdatname);

	/* Dump DB comment if any */
	if (fout->remoteVersion >= 80200)
	{
		/*
		 * 8.2 and up keep comments on shared objects in a shared table, so we
		 * cannot use the dumpComment() code used for other database objects.
		 * Be careful that the ArchiveEntry parameters match that function.
		 */
		char	   *comment = PQgetvalue(res, 0, PQfnumber(res, "description"));

		if (comment && *comment && !dopt->no_comments)
		{
			resetPQExpBuffer(dbQry);

			/*
			 * Generates warning when loaded into a differently-named
			 * database.
			 */
			appendPQExpBuffer(dbQry, "COMMENT ON DATABASE %s IS ", qdatname);
			appendStringLiteralAH(dbQry, comment, fout);
			appendPQExpBufferStr(dbQry, ";\n");

			ArchiveEntry(fout, nilCatalogId, createDumpId(),
						 ARCHIVE_OPTS(.tag = labelq->data,
									  .owner = dba,
									  .description = "COMMENT",
									  .section = SECTION_NONE,
									  .createStmt = dbQry->data,
									  .deps = &dbDumpId,
									  .nDeps = 1));
		}
	}
	else
	{
		dumpComment(fout, "DATABASE", qdatname, NULL, dba,
					dbCatId, 0, dbDumpId);
	}

	/* Dump DB security label, if enabled */
	if (!dopt->no_security_labels && fout->remoteVersion >= 90200)
	{
		PGresult   *shres;
		PQExpBuffer seclabelQry;

		seclabelQry = createPQExpBuffer();

		buildShSecLabelQuery("pg_database", dbCatId.oid, seclabelQry);
		shres = ExecuteSqlQuery(fout, seclabelQry->data, PGRES_TUPLES_OK);
		resetPQExpBuffer(seclabelQry);
		emitShSecLabels(conn, shres, seclabelQry, "DATABASE", datname);
		if (seclabelQry->len > 0)
			ArchiveEntry(fout, nilCatalogId, createDumpId(),
						 ARCHIVE_OPTS(.tag = labelq->data,
									  .owner = dba,
									  .description = "SECURITY LABEL",
									  .section = SECTION_NONE,
									  .createStmt = seclabelQry->data,
									  .deps = &dbDumpId,
									  .nDeps = 1));
		destroyPQExpBuffer(seclabelQry);
		PQclear(shres);
	}

	/*
	 * Dump ACL if any.  Note that we do not support initial privileges
	 * (pg_init_privs) on databases.
	 */
	dumpACL(fout, dbDumpId, InvalidDumpId, "DATABASE",
			qdatname, NULL, NULL,
			dba, datacl, rdatacl, "", "");

	/*
	 * Now construct a DATABASE PROPERTIES archive entry to restore any
	 * non-default database-level properties.  (The reason this must be
	 * separate is that we cannot put any additional commands into the TOC
	 * entry that has CREATE DATABASE.  pg_restore would execute such a group
	 * in an implicit transaction block, and the backend won't allow CREATE
	 * DATABASE in that context.)
	 */
	resetPQExpBuffer(creaQry);
	resetPQExpBuffer(delQry);

	if (strlen(datconnlimit) > 0 && strcmp(datconnlimit, "-1") != 0)
		appendPQExpBuffer(creaQry, "ALTER DATABASE %s CONNECTION LIMIT = %s;\n",
						  qdatname, datconnlimit);

	if (strcmp(datistemplate, "t") == 0)
	{
		appendPQExpBuffer(creaQry, "ALTER DATABASE %s IS_TEMPLATE = true;\n",
						  qdatname);

		/*
		 * The backend won't accept DROP DATABASE on a template database.  We
		 * can deal with that by removing the template marking before the DROP
		 * gets issued.  We'd prefer to use ALTER DATABASE IF EXISTS here, but
		 * since no such command is currently supported, fake it with a direct
		 * UPDATE on pg_database.
		 */
		appendPQExpBufferStr(delQry, "UPDATE pg_catalog.pg_database "
							 "SET datistemplate = false WHERE datname = ");
		appendStringLiteralAH(delQry, datname, fout);
		appendPQExpBufferStr(delQry, ";\n");
	}

	/* Add database-specific SET options */
	dumpDatabaseConfig(fout, creaQry, datname, dbCatId.oid);

	/*
	 * We stick this binary-upgrade query into the DATABASE PROPERTIES archive
	 * entry, too, for lack of a better place.
	 */
	if (dopt->binary_upgrade)
	{
		appendPQExpBufferStr(creaQry, "\n-- For binary upgrade, set datfrozenxid and datminmxid.\n");
		appendPQExpBuffer(creaQry, "UPDATE pg_catalog.pg_database\n"
						  "SET datfrozenxid = '%u', datminmxid = '%u'\n"
						  "WHERE datname = ",
						  frozenxid, minmxid);
		appendStringLiteralAH(creaQry, datname, fout);
		appendPQExpBufferStr(creaQry, ";\n");
	}

	if (creaQry->len > 0)
		ArchiveEntry(fout, nilCatalogId, createDumpId(),
					 ARCHIVE_OPTS(.tag = datname,
								  .owner = dba,
								  .description = "DATABASE PROPERTIES",
								  .section = SECTION_PRE_DATA,
								  .createStmt = creaQry->data,
								  .dropStmt = delQry->data,
								  .deps = &dbDumpId));

	/*
	 * pg_largeobject comes from the old system intact, so set its
	 * relfrozenxids and relminmxids.
	 */
	if (dopt->binary_upgrade)
	{
		PGresult   *lo_res;
		PQExpBuffer loFrozenQry = createPQExpBuffer();
		PQExpBuffer loOutQry = createPQExpBuffer();
		int			i_relfrozenxid,
					i_relminmxid;

		/*
		 * pg_largeobject
		 */
		if (fout->remoteVersion >= 90300)
			appendPQExpBuffer(loFrozenQry, "SELECT relfrozenxid, relminmxid\n"
							  "FROM pg_catalog.pg_class\n"
							  "WHERE oid = %u;\n",
							  LargeObjectRelationId);
		else
			appendPQExpBuffer(loFrozenQry, "SELECT relfrozenxid, 0 AS relminmxid\n"
							  "FROM pg_catalog.pg_class\n"
							  "WHERE oid = %u;\n",
							  LargeObjectRelationId);

		lo_res = ExecuteSqlQueryForSingleRow(fout, loFrozenQry->data);

		i_relfrozenxid = PQfnumber(lo_res, "relfrozenxid");
		i_relminmxid = PQfnumber(lo_res, "relminmxid");

		appendPQExpBufferStr(loOutQry, "\n-- For binary upgrade, set pg_largeobject relfrozenxid and relminmxid\n");
		appendPQExpBuffer(loOutQry, "UPDATE pg_catalog.pg_class\n"
						  "SET relfrozenxid = '%u', relminmxid = '%u'\n"
						  "WHERE oid = %u;\n",
						  atooid(PQgetvalue(lo_res, 0, i_relfrozenxid)),
						  atooid(PQgetvalue(lo_res, 0, i_relminmxid)),
						  LargeObjectRelationId);
		ArchiveEntry(fout, nilCatalogId, createDumpId(),
					 ARCHIVE_OPTS(.tag = "pg_largeobject",
								  .description = "pg_largeobject",
								  .section = SECTION_PRE_DATA,
								  .createStmt = loOutQry->data));

		PQclear(lo_res);

		destroyPQExpBuffer(loFrozenQry);
		destroyPQExpBuffer(loOutQry);
	}

	PQclear(res);

	free(qdatname);
	destroyPQExpBuffer(dbQry);
	destroyPQExpBuffer(delQry);
	destroyPQExpBuffer(creaQry);
	destroyPQExpBuffer(labelq);
}

/*
 * Collect any database-specific or role-and-database-specific SET options
 * for this database, and append them to outbuf.
 */
static void
dumpDatabaseConfig(Archive *AH, PQExpBuffer outbuf,
				   const char *dbname, Oid dboid)
{
	PGconn	   *conn = GetConnection(AH);
	PQExpBuffer buf = createPQExpBuffer();
	PGresult   *res;
	int			count = 1;

	/*
	 * First collect database-specific options.  Pre-8.4 server versions lack
	 * unnest(), so we do this the hard way by querying once per subscript.
	 */
	for (;;)
	{
		if (AH->remoteVersion >= 90000)
			printfPQExpBuffer(buf, "SELECT setconfig[%d] FROM pg_db_role_setting "
							  "WHERE setrole = 0 AND setdatabase = '%u'::oid",
							  count, dboid);
		else
			printfPQExpBuffer(buf, "SELECT datconfig[%d] FROM pg_database WHERE oid = '%u'::oid", count, dboid);

		res = ExecuteSqlQuery(AH, buf->data, PGRES_TUPLES_OK);

		if (PQntuples(res) == 1 &&
			!PQgetisnull(res, 0, 0))
		{
			makeAlterConfigCommand(conn, PQgetvalue(res, 0, 0),
								   "DATABASE", dbname, NULL, NULL,
								   outbuf);
			PQclear(res);
			count++;
		}
		else
		{
			PQclear(res);
			break;
		}
	}

	/* Now look for role-and-database-specific options */
	if (AH->remoteVersion >= 90000)
	{
		/* Here we can assume we have unnest() */
		printfPQExpBuffer(buf, "SELECT rolname, unnest(setconfig) "
						  "FROM pg_db_role_setting s, pg_roles r "
						  "WHERE setrole = r.oid AND setdatabase = '%u'::oid",
						  dboid);

		res = ExecuteSqlQuery(AH, buf->data, PGRES_TUPLES_OK);

		if (PQntuples(res) > 0)
		{
			int			i;

			for (i = 0; i < PQntuples(res); i++)
				makeAlterConfigCommand(conn, PQgetvalue(res, i, 1),
									   "ROLE", PQgetvalue(res, i, 0),
									   "DATABASE", dbname,
									   outbuf);
		}

		PQclear(res);
	}

	destroyPQExpBuffer(buf);
}

/*
 * dumpEncoding: put the correct encoding into the archive
 */
static void
dumpEncoding(Archive *AH)
{
	const char *encname = pg_encoding_to_char(AH->encoding);
	PQExpBuffer qry = createPQExpBuffer();

	pg_log_info("saving encoding = %s", encname);

	appendPQExpBufferStr(qry, "SET client_encoding = ");
	appendStringLiteralAH(qry, encname, AH);
	appendPQExpBufferStr(qry, ";\n");

	ArchiveEntry(AH, nilCatalogId, createDumpId(),
				 ARCHIVE_OPTS(.tag = "ENCODING",
							  .description = "ENCODING",
							  .section = SECTION_PRE_DATA,
							  .createStmt = qry->data));

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

	pg_log_info("saving standard_conforming_strings = %s",
				stdstrings);

	appendPQExpBuffer(qry, "SET standard_conforming_strings = '%s';\n",
					  stdstrings);

	ArchiveEntry(AH, nilCatalogId, createDumpId(),
				 ARCHIVE_OPTS(.tag = "STDSTRINGS",
							  .description = "STDSTRINGS",
							  .section = SECTION_PRE_DATA,
							  .createStmt = qry->data));

	destroyPQExpBuffer(qry);
}

/*
 * dumpSearchPath: record the active search_path in the archive
 */
static void
dumpSearchPath(Archive *AH)
{
	PQExpBuffer qry = createPQExpBuffer();
	PQExpBuffer path = createPQExpBuffer();
	PGresult   *res;
	char	  **schemanames = NULL;
	int			nschemanames = 0;
	int			i;

	/*
	 * We use the result of current_schemas(), not the search_path GUC,
	 * because that might contain wildcards such as "$user", which won't
	 * necessarily have the same value during restore.  Also, this way avoids
	 * listing schemas that may appear in search_path but not actually exist,
	 * which seems like a prudent exclusion.
	 */
	res = ExecuteSqlQueryForSingleRow(AH,
									  "SELECT pg_catalog.current_schemas(false)");

	if (!parsePGArray(PQgetvalue(res, 0, 0), &schemanames, &nschemanames))
		fatal("could not parse result of current_schemas()");

	/*
	 * We use set_config(), not a simple "SET search_path" command, because
	 * the latter has less-clean behavior if the search path is empty.  While
	 * that's likely to get fixed at some point, it seems like a good idea to
	 * be as backwards-compatible as possible in what we put into archives.
	 */
	for (i = 0; i < nschemanames; i++)
	{
		if (i > 0)
			appendPQExpBufferStr(path, ", ");
		appendPQExpBufferStr(path, fmtId(schemanames[i]));
	}

	appendPQExpBufferStr(qry, "SELECT pg_catalog.set_config('search_path', ");
	appendStringLiteralAH(qry, path->data, AH);
	appendPQExpBufferStr(qry, ", false);\n");

	pg_log_info("saving search_path = %s", path->data);

	ArchiveEntry(AH, nilCatalogId, createDumpId(),
				 ARCHIVE_OPTS(.tag = "SEARCHPATH",
							  .description = "SEARCHPATH",
							  .section = SECTION_PRE_DATA,
							  .createStmt = qry->data));

	/* Also save it in AH->searchpath, in case we're doing plain text dump */
	AH->searchpath = pg_strdup(qry->data);

	if (schemanames)
		free(schemanames);
	PQclear(res);
	destroyPQExpBuffer(qry);
	destroyPQExpBuffer(path);
}


/*
 * getBlobs:
 *	Collect schema-level data about large objects
 */
static void
getBlobs(Archive *fout)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer blobQry = createPQExpBuffer();
	BlobInfo   *binfo;
	DumpableObject *bdata;
	PGresult   *res;
	int			ntups;
	int			i;
	int			i_oid;
	int			i_lomowner;
	int			i_lomacl;
	int			i_rlomacl;
	int			i_initlomacl;
	int			i_initrlomacl;

	pg_log_info("reading large objects");

	/* Fetch BLOB OIDs, and owner/ACL data if >= 9.0 */
	if (fout->remoteVersion >= 90600)
	{
		PQExpBuffer acl_subquery = createPQExpBuffer();
		PQExpBuffer racl_subquery = createPQExpBuffer();
		PQExpBuffer init_acl_subquery = createPQExpBuffer();
		PQExpBuffer init_racl_subquery = createPQExpBuffer();

		buildACLQueries(acl_subquery, racl_subquery, init_acl_subquery,
						init_racl_subquery, "l.lomacl", "l.lomowner", "'L'",
						dopt->binary_upgrade);

		appendPQExpBuffer(blobQry,
						  "SELECT l.oid, (%s l.lomowner) AS rolname, "
						  "%s AS lomacl, "
						  "%s AS rlomacl, "
						  "%s AS initlomacl, "
						  "%s AS initrlomacl "
						  "FROM pg_largeobject_metadata l "
						  "LEFT JOIN pg_init_privs pip ON "
						  "(l.oid = pip.objoid "
						  "AND pip.classoid = 'pg_largeobject'::regclass "
						  "AND pip.objsubid = 0) ",
						  username_subquery,
						  acl_subquery->data,
						  racl_subquery->data,
						  init_acl_subquery->data,
						  init_racl_subquery->data);

		destroyPQExpBuffer(acl_subquery);
		destroyPQExpBuffer(racl_subquery);
		destroyPQExpBuffer(init_acl_subquery);
		destroyPQExpBuffer(init_racl_subquery);
	}
	else if (fout->remoteVersion >= 90000)
		appendPQExpBuffer(blobQry,
						  "SELECT oid, (%s lomowner) AS rolname, lomacl, "
						  "NULL AS rlomacl, NULL AS initlomacl, "
						  "NULL AS initrlomacl "
						  " FROM pg_largeobject_metadata",
						  username_subquery);
	else
		appendPQExpBufferStr(blobQry,
							 "SELECT DISTINCT loid AS oid, "
							 "NULL::name AS rolname, NULL::oid AS lomacl, "
							 "NULL::oid AS rlomacl, NULL::oid AS initlomacl, "
							 "NULL::oid AS initrlomacl "
							 " FROM pg_largeobject");

	res = ExecuteSqlQuery(fout, blobQry->data, PGRES_TUPLES_OK);

	i_oid = PQfnumber(res, "oid");
	i_lomowner = PQfnumber(res, "rolname");
	i_lomacl = PQfnumber(res, "lomacl");
	i_rlomacl = PQfnumber(res, "rlomacl");
	i_initlomacl = PQfnumber(res, "initlomacl");
	i_initrlomacl = PQfnumber(res, "initrlomacl");

	ntups = PQntuples(res);

	/*
	 * Each large object has its own BLOB archive entry.
	 */
	binfo = (BlobInfo *) pg_malloc(ntups * sizeof(BlobInfo));

	for (i = 0; i < ntups; i++)
	{
		binfo[i].dobj.objType = DO_BLOB;
		binfo[i].dobj.catId.tableoid = LargeObjectRelationId;
		binfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&binfo[i].dobj);

		binfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_oid));
		binfo[i].rolname = pg_strdup(PQgetvalue(res, i, i_lomowner));
		binfo[i].blobacl = pg_strdup(PQgetvalue(res, i, i_lomacl));
		binfo[i].rblobacl = pg_strdup(PQgetvalue(res, i, i_rlomacl));
		binfo[i].initblobacl = pg_strdup(PQgetvalue(res, i, i_initlomacl));
		binfo[i].initrblobacl = pg_strdup(PQgetvalue(res, i, i_initrlomacl));

		if (PQgetisnull(res, i, i_lomacl) &&
			PQgetisnull(res, i, i_rlomacl) &&
			PQgetisnull(res, i, i_initlomacl) &&
			PQgetisnull(res, i, i_initrlomacl))
			binfo[i].dobj.dump &= ~DUMP_COMPONENT_ACL;

		/*
		 * In binary-upgrade mode for blobs, we do *not* dump out the blob
		 * data, as it will be copied by pg_upgrade, which simply copies the
		 * pg_largeobject table. We *do* however dump out anything but the
		 * data, as pg_upgrade copies just pg_largeobject, but not
		 * pg_largeobject_metadata, after the dump is restored.
		 */
		if (dopt->binary_upgrade)
			binfo[i].dobj.dump &= ~DUMP_COMPONENT_DATA;
	}

	/*
	 * If we have any large objects, a "BLOBS" archive entry is needed. This
	 * is just a placeholder for sorting; it carries no data now.
	 */
	if (ntups > 0)
	{
		bdata = (DumpableObject *) pg_malloc(sizeof(DumpableObject));
		bdata->objType = DO_BLOB_DATA;
		bdata->catId = nilCatalogId;
		AssignDumpId(bdata);
		bdata->name = pg_strdup("BLOBS");
	}

	PQclear(res);
	destroyPQExpBuffer(blobQry);
}

/*
 * dumpBlob
 *
 * dump the definition (metadata) of the given large object
 */
static void
dumpBlob(Archive *fout, BlobInfo *binfo)
{
	PQExpBuffer cquery = createPQExpBuffer();
	PQExpBuffer dquery = createPQExpBuffer();

	appendPQExpBuffer(cquery,
					  "SELECT pg_catalog.lo_create('%s');\n",
					  binfo->dobj.name);

	appendPQExpBuffer(dquery,
					  "SELECT pg_catalog.lo_unlink('%s');\n",
					  binfo->dobj.name);

	if (binfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, binfo->dobj.catId, binfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = binfo->dobj.name,
								  .owner = binfo->rolname,
								  .description = "BLOB",
								  .section = SECTION_PRE_DATA,
								  .createStmt = cquery->data,
								  .dropStmt = dquery->data));

	/* Dump comment if any */
	if (binfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "LARGE OBJECT", binfo->dobj.name,
					NULL, binfo->rolname,
					binfo->dobj.catId, 0, binfo->dobj.dumpId);

	/* Dump security label if any */
	if (binfo->dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpSecLabel(fout, "LARGE OBJECT", binfo->dobj.name,
					 NULL, binfo->rolname,
					 binfo->dobj.catId, 0, binfo->dobj.dumpId);

	/* Dump ACL if any */
	if (binfo->blobacl && (binfo->dobj.dump & DUMP_COMPONENT_ACL))
		dumpACL(fout, binfo->dobj.dumpId, InvalidDumpId, "LARGE OBJECT",
				binfo->dobj.name, NULL,
				NULL, binfo->rolname, binfo->blobacl, binfo->rblobacl,
				binfo->initblobacl, binfo->initrblobacl);

	destroyPQExpBuffer(cquery);
	destroyPQExpBuffer(dquery);
}

/*
 * dumpBlobs:
 *	dump the data contents of all large objects
 */
static int
dumpBlobs(Archive *fout, void *arg)
{
	const char *blobQry;
	const char *blobFetchQry;
	PGconn	   *conn = GetConnection(fout);
	PGresult   *res;
	char		buf[LOBBUFSIZE];
	int			ntups;
	int			i;
	int			cnt;

	pg_log_info("saving large objects");

	/*
	 * Currently, we re-fetch all BLOB OIDs using a cursor.  Consider scanning
	 * the already-in-memory dumpable objects instead...
	 */
	if (fout->remoteVersion >= 90000)
		blobQry =
			"DECLARE bloboid CURSOR FOR "
			"SELECT oid FROM pg_largeobject_metadata ORDER BY 1";
	else
		blobQry =
			"DECLARE bloboid CURSOR FOR "
			"SELECT DISTINCT loid FROM pg_largeobject ORDER BY 1";

	ExecuteSqlStatement(fout, blobQry);

	/* Command to fetch from cursor */
	blobFetchQry = "FETCH 1000 IN bloboid";

	do
	{
		/* Do a fetch */
		res = ExecuteSqlQuery(fout, blobFetchQry, PGRES_TUPLES_OK);

		/* Process the tuples, if any */
		ntups = PQntuples(res);
		for (i = 0; i < ntups; i++)
		{
			Oid			blobOid;
			int			loFd;

			blobOid = atooid(PQgetvalue(res, i, 0));
			/* Open the BLOB */
			loFd = lo_open(conn, blobOid, INV_READ);
			if (loFd == -1)
				fatal("could not open large object %u: %s",
					  blobOid, PQerrorMessage(conn));

			StartBlob(fout, blobOid);

			/* Now read it in chunks, sending data to archive */
			do
			{
				cnt = lo_read(conn, loFd, buf, LOBBUFSIZE);
				if (cnt < 0)
					fatal("error reading large object %u: %s",
						  blobOid, PQerrorMessage(conn));

				WriteData(fout, buf, cnt);
			} while (cnt > 0);

			lo_close(conn, loFd);

			EndBlob(fout, blobOid);
		}

		PQclear(res);
	} while (ntups > 0);

	return 1;
}

/*
 * getPolicies
 *	  get information about policies on a dumpable table.
 */
void
getPolicies(Archive *fout, TableInfo tblinfo[], int numTables)
{
	PQExpBuffer query;
	PGresult   *res;
	PolicyInfo *polinfo;
	int			i_oid;
	int			i_tableoid;
	int			i_polname;
	int			i_polcmd;
	int			i_polpermissive;
	int			i_polroles;
	int			i_polqual;
	int			i_polwithcheck;
	int			i,
				j,
				ntups;

	if (fout->remoteVersion < 90500)
		return;

	query = createPQExpBuffer();

	for (i = 0; i < numTables; i++)
	{
		TableInfo  *tbinfo = &tblinfo[i];

		/* Ignore row security on tables not to be dumped */
		if (!(tbinfo->dobj.dump & DUMP_COMPONENT_POLICY))
			continue;

		pg_log_info("reading row security enabled for table \"%s.%s\"",
					tbinfo->dobj.namespace->dobj.name,
					tbinfo->dobj.name);

		/*
		 * Get row security enabled information for the table. We represent
		 * RLS being enabled on a table by creating a PolicyInfo object with
		 * null polname.
		 */
		if (tbinfo->rowsec)
		{
			/*
			 * Note: use tableoid 0 so that this object won't be mistaken for
			 * something that pg_depend entries apply to.
			 */
			polinfo = pg_malloc(sizeof(PolicyInfo));
			polinfo->dobj.objType = DO_POLICY;
			polinfo->dobj.catId.tableoid = 0;
			polinfo->dobj.catId.oid = tbinfo->dobj.catId.oid;
			AssignDumpId(&polinfo->dobj);
			polinfo->dobj.namespace = tbinfo->dobj.namespace;
			polinfo->dobj.name = pg_strdup(tbinfo->dobj.name);
			polinfo->poltable = tbinfo;
			polinfo->polname = NULL;
			polinfo->polcmd = '\0';
			polinfo->polpermissive = 0;
			polinfo->polroles = NULL;
			polinfo->polqual = NULL;
			polinfo->polwithcheck = NULL;
		}

		pg_log_info("reading policies for table \"%s.%s\"",
					tbinfo->dobj.namespace->dobj.name,
					tbinfo->dobj.name);

		resetPQExpBuffer(query);

		/* Get the policies for the table. */
		if (fout->remoteVersion >= 100000)
			appendPQExpBuffer(query,
							  "SELECT oid, tableoid, pol.polname, pol.polcmd, pol.polpermissive, "
							  "CASE WHEN pol.polroles = '{0}' THEN NULL ELSE "
							  "   pg_catalog.array_to_string(ARRAY(SELECT pg_catalog.quote_ident(rolname) from pg_catalog.pg_roles WHERE oid = ANY(pol.polroles)), ', ') END AS polroles, "
							  "pg_catalog.pg_get_expr(pol.polqual, pol.polrelid) AS polqual, "
							  "pg_catalog.pg_get_expr(pol.polwithcheck, pol.polrelid) AS polwithcheck "
							  "FROM pg_catalog.pg_policy pol "
							  "WHERE polrelid = '%u'",
							  tbinfo->dobj.catId.oid);
		else
			appendPQExpBuffer(query,
							  "SELECT oid, tableoid, pol.polname, pol.polcmd, 't' as polpermissive, "
							  "CASE WHEN pol.polroles = '{0}' THEN NULL ELSE "
							  "   pg_catalog.array_to_string(ARRAY(SELECT pg_catalog.quote_ident(rolname) from pg_catalog.pg_roles WHERE oid = ANY(pol.polroles)), ', ') END AS polroles, "
							  "pg_catalog.pg_get_expr(pol.polqual, pol.polrelid) AS polqual, "
							  "pg_catalog.pg_get_expr(pol.polwithcheck, pol.polrelid) AS polwithcheck "
							  "FROM pg_catalog.pg_policy pol "
							  "WHERE polrelid = '%u'",
							  tbinfo->dobj.catId.oid);
		res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

		ntups = PQntuples(res);

		if (ntups == 0)
		{
			/*
			 * No explicit policies to handle (only the default-deny policy,
			 * which is handled as part of the table definition).  Clean up
			 * and return.
			 */
			PQclear(res);
			continue;
		}

		i_oid = PQfnumber(res, "oid");
		i_tableoid = PQfnumber(res, "tableoid");
		i_polname = PQfnumber(res, "polname");
		i_polcmd = PQfnumber(res, "polcmd");
		i_polpermissive = PQfnumber(res, "polpermissive");
		i_polroles = PQfnumber(res, "polroles");
		i_polqual = PQfnumber(res, "polqual");
		i_polwithcheck = PQfnumber(res, "polwithcheck");

		polinfo = pg_malloc(ntups * sizeof(PolicyInfo));

		for (j = 0; j < ntups; j++)
		{
			polinfo[j].dobj.objType = DO_POLICY;
			polinfo[j].dobj.catId.tableoid =
				atooid(PQgetvalue(res, j, i_tableoid));
			polinfo[j].dobj.catId.oid = atooid(PQgetvalue(res, j, i_oid));
			AssignDumpId(&polinfo[j].dobj);
			polinfo[j].dobj.namespace = tbinfo->dobj.namespace;
			polinfo[j].poltable = tbinfo;
			polinfo[j].polname = pg_strdup(PQgetvalue(res, j, i_polname));
			polinfo[j].dobj.name = pg_strdup(polinfo[j].polname);

			polinfo[j].polcmd = *(PQgetvalue(res, j, i_polcmd));
			polinfo[j].polpermissive = *(PQgetvalue(res, j, i_polpermissive)) == 't';

			if (PQgetisnull(res, j, i_polroles))
				polinfo[j].polroles = NULL;
			else
				polinfo[j].polroles = pg_strdup(PQgetvalue(res, j, i_polroles));

			if (PQgetisnull(res, j, i_polqual))
				polinfo[j].polqual = NULL;
			else
				polinfo[j].polqual = pg_strdup(PQgetvalue(res, j, i_polqual));

			if (PQgetisnull(res, j, i_polwithcheck))
				polinfo[j].polwithcheck = NULL;
			else
				polinfo[j].polwithcheck
					= pg_strdup(PQgetvalue(res, j, i_polwithcheck));
		}
		PQclear(res);
	}
	destroyPQExpBuffer(query);
}

/*
 * dumpPolicy
 *	  dump the definition of the given policy
 */
static void
dumpPolicy(Archive *fout, PolicyInfo *polinfo)
{
	DumpOptions *dopt = fout->dopt;
	TableInfo  *tbinfo = polinfo->poltable;
	PQExpBuffer query;
	PQExpBuffer delqry;
	PQExpBuffer polprefix;
	char	   *qtabname;
	const char *cmd;
	char	   *tag;

	if (dopt->dataOnly)
		return;

	/*
	 * If polname is NULL, then this record is just indicating that ROW LEVEL
	 * SECURITY is enabled for the table. Dump as ALTER TABLE <table> ENABLE
	 * ROW LEVEL SECURITY.
	 */
	if (polinfo->polname == NULL)
	{
		query = createPQExpBuffer();

		appendPQExpBuffer(query, "ALTER TABLE %s ENABLE ROW LEVEL SECURITY;",
						  fmtQualifiedDumpable(tbinfo));

		/*
		 * We must emit the ROW SECURITY object's dependency on its table
		 * explicitly, because it will not match anything in pg_depend (unlike
		 * the case for other PolicyInfo objects).
		 */
		if (polinfo->dobj.dump & DUMP_COMPONENT_POLICY)
			ArchiveEntry(fout, polinfo->dobj.catId, polinfo->dobj.dumpId,
						 ARCHIVE_OPTS(.tag = polinfo->dobj.name,
									  .namespace = polinfo->dobj.namespace->dobj.name,
									  .owner = tbinfo->rolname,
									  .description = "ROW SECURITY",
									  .section = SECTION_POST_DATA,
									  .createStmt = query->data,
									  .deps = &(tbinfo->dobj.dumpId),
									  .nDeps = 1));

		destroyPQExpBuffer(query);
		return;
	}

	if (polinfo->polcmd == '*')
		cmd = "";
	else if (polinfo->polcmd == 'r')
		cmd = " FOR SELECT";
	else if (polinfo->polcmd == 'a')
		cmd = " FOR INSERT";
	else if (polinfo->polcmd == 'w')
		cmd = " FOR UPDATE";
	else if (polinfo->polcmd == 'd')
		cmd = " FOR DELETE";
	else
	{
		pg_log_error("unexpected policy command type: %c",
					 polinfo->polcmd);
		exit_nicely(1);
	}

	query = createPQExpBuffer();
	delqry = createPQExpBuffer();
	polprefix = createPQExpBuffer();

	qtabname = pg_strdup(fmtId(tbinfo->dobj.name));

	appendPQExpBuffer(query, "CREATE POLICY %s", fmtId(polinfo->polname));

	appendPQExpBuffer(query, " ON %s%s%s", fmtQualifiedDumpable(tbinfo),
					  !polinfo->polpermissive ? " AS RESTRICTIVE" : "", cmd);

	if (polinfo->polroles != NULL)
		appendPQExpBuffer(query, " TO %s", polinfo->polroles);

	if (polinfo->polqual != NULL)
		appendPQExpBuffer(query, " USING (%s)", polinfo->polqual);

	if (polinfo->polwithcheck != NULL)
		appendPQExpBuffer(query, " WITH CHECK (%s)", polinfo->polwithcheck);

	appendPQExpBufferStr(query, ";\n");

	appendPQExpBuffer(delqry, "DROP POLICY %s", fmtId(polinfo->polname));
	appendPQExpBuffer(delqry, " ON %s;\n", fmtQualifiedDumpable(tbinfo));

	appendPQExpBuffer(polprefix, "POLICY %s ON",
					  fmtId(polinfo->polname));

	tag = psprintf("%s %s", tbinfo->dobj.name, polinfo->dobj.name);

	if (polinfo->dobj.dump & DUMP_COMPONENT_POLICY)
		ArchiveEntry(fout, polinfo->dobj.catId, polinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tag,
								  .namespace = polinfo->dobj.namespace->dobj.name,
								  .owner = tbinfo->rolname,
								  .description = "POLICY",
								  .section = SECTION_POST_DATA,
								  .createStmt = query->data,
								  .dropStmt = delqry->data));

	if (polinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, polprefix->data, qtabname,
					tbinfo->dobj.namespace->dobj.name, tbinfo->rolname,
					polinfo->dobj.catId, 0, polinfo->dobj.dumpId);

	free(tag);
	destroyPQExpBuffer(query);
	destroyPQExpBuffer(delqry);
	destroyPQExpBuffer(polprefix);
	free(qtabname);
}

/*
 * getPublications
 *	  get information about publications
 */
void
getPublications(Archive *fout)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer query;
	PGresult   *res;
	PublicationInfo *pubinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_pubname;
	int			i_rolname;
	int			i_puballtables;
	int			i_pubinsert;
	int			i_pubupdate;
	int			i_pubdelete;
	int			i_pubtruncate;
	int			i_pubviaroot;
	int			i,
				ntups;

	if (dopt->no_publications || fout->remoteVersion < 100000)
		return;

	query = createPQExpBuffer();

	resetPQExpBuffer(query);

	/* Get the publications. */
	if (fout->remoteVersion >= 130000)
		appendPQExpBuffer(query,
						  "SELECT p.tableoid, p.oid, p.pubname, "
						  "(%s p.pubowner) AS rolname, "
						  "p.puballtables, p.pubinsert, p.pubupdate, p.pubdelete, p.pubtruncate, p.pubviaroot "
						  "FROM pg_publication p",
						  username_subquery);
	else if (fout->remoteVersion >= 110000)
		appendPQExpBuffer(query,
						  "SELECT p.tableoid, p.oid, p.pubname, "
						  "(%s p.pubowner) AS rolname, "
						  "p.puballtables, p.pubinsert, p.pubupdate, p.pubdelete, p.pubtruncate, false AS pubviaroot "
						  "FROM pg_publication p",
						  username_subquery);
	else
		appendPQExpBuffer(query,
						  "SELECT p.tableoid, p.oid, p.pubname, "
						  "(%s p.pubowner) AS rolname, "
						  "p.puballtables, p.pubinsert, p.pubupdate, p.pubdelete, false AS pubtruncate, false AS pubviaroot "
						  "FROM pg_publication p",
						  username_subquery);

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_pubname = PQfnumber(res, "pubname");
	i_rolname = PQfnumber(res, "rolname");
	i_puballtables = PQfnumber(res, "puballtables");
	i_pubinsert = PQfnumber(res, "pubinsert");
	i_pubupdate = PQfnumber(res, "pubupdate");
	i_pubdelete = PQfnumber(res, "pubdelete");
	i_pubtruncate = PQfnumber(res, "pubtruncate");
	i_pubviaroot = PQfnumber(res, "pubviaroot");

	pubinfo = pg_malloc(ntups * sizeof(PublicationInfo));

	for (i = 0; i < ntups; i++)
	{
		pubinfo[i].dobj.objType = DO_PUBLICATION;
		pubinfo[i].dobj.catId.tableoid =
			atooid(PQgetvalue(res, i, i_tableoid));
		pubinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&pubinfo[i].dobj);
		pubinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_pubname));
		pubinfo[i].rolname = pg_strdup(PQgetvalue(res, i, i_rolname));
		pubinfo[i].puballtables =
			(strcmp(PQgetvalue(res, i, i_puballtables), "t") == 0);
		pubinfo[i].pubinsert =
			(strcmp(PQgetvalue(res, i, i_pubinsert), "t") == 0);
		pubinfo[i].pubupdate =
			(strcmp(PQgetvalue(res, i, i_pubupdate), "t") == 0);
		pubinfo[i].pubdelete =
			(strcmp(PQgetvalue(res, i, i_pubdelete), "t") == 0);
		pubinfo[i].pubtruncate =
			(strcmp(PQgetvalue(res, i, i_pubtruncate), "t") == 0);
		pubinfo[i].pubviaroot =
			(strcmp(PQgetvalue(res, i, i_pubviaroot), "t") == 0);

		if (strlen(pubinfo[i].rolname) == 0)
			pg_log_warning("owner of publication \"%s\" appears to be invalid",
						   pubinfo[i].dobj.name);

		/* Decide whether we want to dump it */
		selectDumpableObject(&(pubinfo[i].dobj), fout);
	}
	PQclear(res);

	destroyPQExpBuffer(query);
}

/*
 * dumpPublication
 *	  dump the definition of the given publication
 */
static void
dumpPublication(Archive *fout, PublicationInfo *pubinfo)
{
	PQExpBuffer delq;
	PQExpBuffer query;
	char	   *qpubname;
	bool		first = true;

	if (!(pubinfo->dobj.dump & DUMP_COMPONENT_DEFINITION))
		return;

	delq = createPQExpBuffer();
	query = createPQExpBuffer();

	qpubname = pg_strdup(fmtId(pubinfo->dobj.name));

	appendPQExpBuffer(delq, "DROP PUBLICATION %s;\n",
					  qpubname);

	appendPQExpBuffer(query, "CREATE PUBLICATION %s",
					  qpubname);

	if (pubinfo->puballtables)
		appendPQExpBufferStr(query, " FOR ALL TABLES");

	appendPQExpBufferStr(query, " WITH (publish = '");
	if (pubinfo->pubinsert)
	{
		appendPQExpBufferStr(query, "insert");
		first = false;
	}

	if (pubinfo->pubupdate)
	{
		if (!first)
			appendPQExpBufferStr(query, ", ");

		appendPQExpBufferStr(query, "update");
		first = false;
	}

	if (pubinfo->pubdelete)
	{
		if (!first)
			appendPQExpBufferStr(query, ", ");

		appendPQExpBufferStr(query, "delete");
		first = false;
	}

	if (pubinfo->pubtruncate)
	{
		if (!first)
			appendPQExpBufferStr(query, ", ");

		appendPQExpBufferStr(query, "truncate");
		first = false;
	}

	appendPQExpBufferStr(query, "'");

	if (pubinfo->pubviaroot)
		appendPQExpBufferStr(query, ", publish_via_partition_root = true");

	appendPQExpBufferStr(query, ");\n");

	ArchiveEntry(fout, pubinfo->dobj.catId, pubinfo->dobj.dumpId,
				 ARCHIVE_OPTS(.tag = pubinfo->dobj.name,
							  .owner = pubinfo->rolname,
							  .description = "PUBLICATION",
							  .section = SECTION_POST_DATA,
							  .createStmt = query->data,
							  .dropStmt = delq->data));

	if (pubinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "PUBLICATION", qpubname,
					NULL, pubinfo->rolname,
					pubinfo->dobj.catId, 0, pubinfo->dobj.dumpId);

	if (pubinfo->dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpSecLabel(fout, "PUBLICATION", qpubname,
					 NULL, pubinfo->rolname,
					 pubinfo->dobj.catId, 0, pubinfo->dobj.dumpId);

	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(query);
	free(qpubname);
}

/*
 * getPublicationTables
 *	  get information about publication membership for dumpable tables.
 */
void
getPublicationTables(Archive *fout, TableInfo tblinfo[], int numTables)
{
	PQExpBuffer query;
	PGresult   *res;
	PublicationRelInfo *pubrinfo;
	DumpOptions *dopt = fout->dopt;
	int			i_tableoid;
	int			i_oid;
	int			i_pubname;
	int			i,
				j,
				ntups;

	if (dopt->no_publications || fout->remoteVersion < 100000)
		return;

	query = createPQExpBuffer();

	for (i = 0; i < numTables; i++)
	{
		TableInfo  *tbinfo = &tblinfo[i];

		/*
		 * Only regular and partitioned tables can be added to publications.
		 */
		if (tbinfo->relkind != RELKIND_RELATION &&
			tbinfo->relkind != RELKIND_PARTITIONED_TABLE)
			continue;

		/*
		 * Ignore publication membership of tables whose definitions are not
		 * to be dumped.
		 */
		if (!(tbinfo->dobj.dump & DUMP_COMPONENT_DEFINITION))
			continue;

		pg_log_info("reading publication membership for table \"%s.%s\"",
					tbinfo->dobj.namespace->dobj.name,
					tbinfo->dobj.name);

		resetPQExpBuffer(query);

		/* Get the publication membership for the table. */
		appendPQExpBuffer(query,
						  "SELECT pr.tableoid, pr.oid, p.pubname "
						  "FROM pg_publication_rel pr, pg_publication p "
						  "WHERE pr.prrelid = '%u'"
						  "  AND p.oid = pr.prpubid",
						  tbinfo->dobj.catId.oid);
		res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

		ntups = PQntuples(res);

		if (ntups == 0)
		{
			/*
			 * Table is not member of any publications. Clean up and return.
			 */
			PQclear(res);
			continue;
		}

		i_tableoid = PQfnumber(res, "tableoid");
		i_oid = PQfnumber(res, "oid");
		i_pubname = PQfnumber(res, "pubname");

		pubrinfo = pg_malloc(ntups * sizeof(PublicationRelInfo));

		for (j = 0; j < ntups; j++)
		{
			pubrinfo[j].dobj.objType = DO_PUBLICATION_REL;
			pubrinfo[j].dobj.catId.tableoid =
				atooid(PQgetvalue(res, j, i_tableoid));
			pubrinfo[j].dobj.catId.oid = atooid(PQgetvalue(res, j, i_oid));
			AssignDumpId(&pubrinfo[j].dobj);
			pubrinfo[j].dobj.namespace = tbinfo->dobj.namespace;
			pubrinfo[j].dobj.name = tbinfo->dobj.name;
			pubrinfo[j].pubname = pg_strdup(PQgetvalue(res, j, i_pubname));
			pubrinfo[j].pubtable = tbinfo;

			/* Decide whether we want to dump it */
			selectDumpablePublicationTable(&(pubrinfo[j].dobj), fout);
		}
		PQclear(res);
	}
	destroyPQExpBuffer(query);
}

/*
 * dumpPublicationTable
 *	  dump the definition of the given publication table mapping
 */
static void
dumpPublicationTable(Archive *fout, PublicationRelInfo *pubrinfo)
{
	TableInfo  *tbinfo = pubrinfo->pubtable;
	PQExpBuffer query;
	char	   *tag;

	if (!(pubrinfo->dobj.dump & DUMP_COMPONENT_DEFINITION))
		return;

	tag = psprintf("%s %s", pubrinfo->pubname, tbinfo->dobj.name);

	query = createPQExpBuffer();

	appendPQExpBuffer(query, "ALTER PUBLICATION %s ADD TABLE ONLY",
					  fmtId(pubrinfo->pubname));
	appendPQExpBuffer(query, " %s;\n",
					  fmtQualifiedDumpable(tbinfo));

	/*
	 * There is no point in creating drop query as the drop is done by table
	 * drop.
	 */
	ArchiveEntry(fout, pubrinfo->dobj.catId, pubrinfo->dobj.dumpId,
				 ARCHIVE_OPTS(.tag = tag,
							  .namespace = tbinfo->dobj.namespace->dobj.name,
							  .description = "PUBLICATION TABLE",
							  .section = SECTION_POST_DATA,
							  .createStmt = query->data));

	free(tag);
	destroyPQExpBuffer(query);
}

/*
 * Is the currently connected user a superuser?
 */
static bool
is_superuser(Archive *fout)
{
	ArchiveHandle *AH = (ArchiveHandle *) fout;
	const char *val;

	val = PQparameterStatus(AH->connection, "is_superuser");

	if (val && strcmp(val, "on") == 0)
		return true;

	return false;
}

/*
 * getSubscriptions
 *	  get information about subscriptions
 */
void
getSubscriptions(Archive *fout)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer query;
	PGresult   *res;
	SubscriptionInfo *subinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_subname;
	int			i_rolname;
	int			i_substream;
	int			i_subconninfo;
	int			i_subslotname;
	int			i_subsynccommit;
	int			i_subpublications;
	int			i_subbinary;
	int			i,
				ntups;

	if (dopt->no_subscriptions || fout->remoteVersion < 100000)
		return;

	if (!is_superuser(fout))
	{
		int			n;

		res = ExecuteSqlQuery(fout,
							  "SELECT count(*) FROM pg_subscription "
							  "WHERE subdbid = (SELECT oid FROM pg_database"
							  "                 WHERE datname = current_database())",
							  PGRES_TUPLES_OK);
		n = atoi(PQgetvalue(res, 0, 0));
		if (n > 0)
			pg_log_warning("subscriptions not dumped because current user is not a superuser");
		PQclear(res);
		return;
	}

	query = createPQExpBuffer();

	/* Get the subscriptions in current database. */
	appendPQExpBuffer(query,
					  "SELECT s.tableoid, s.oid, s.subname,\n"
					  " (%s s.subowner) AS rolname,\n"
					  " s.subconninfo, s.subslotname, s.subsynccommit,\n"
					  " s.subpublications,\n",
					  username_subquery);

	if (fout->remoteVersion >= 140000)
		appendPQExpBufferStr(query, " s.subbinary,\n");
	else
		appendPQExpBufferStr(query, " false AS subbinary,\n");

	if (fout->remoteVersion >= 140000)
		appendPQExpBufferStr(query, " s.substream\n");
	else
		appendPQExpBufferStr(query, " false AS substream\n");

	appendPQExpBufferStr(query,
						 "FROM pg_subscription s\n"
						 "WHERE s.subdbid = (SELECT oid FROM pg_database\n"
						 "                   WHERE datname = current_database())");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_subname = PQfnumber(res, "subname");
	i_rolname = PQfnumber(res, "rolname");
	i_subconninfo = PQfnumber(res, "subconninfo");
	i_subslotname = PQfnumber(res, "subslotname");
	i_subsynccommit = PQfnumber(res, "subsynccommit");
	i_subpublications = PQfnumber(res, "subpublications");
	i_subbinary = PQfnumber(res, "subbinary");
	i_substream = PQfnumber(res, "substream");

	subinfo = pg_malloc(ntups * sizeof(SubscriptionInfo));

	for (i = 0; i < ntups; i++)
	{
		subinfo[i].dobj.objType = DO_SUBSCRIPTION;
		subinfo[i].dobj.catId.tableoid =
			atooid(PQgetvalue(res, i, i_tableoid));
		subinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&subinfo[i].dobj);
		subinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_subname));
		subinfo[i].rolname = pg_strdup(PQgetvalue(res, i, i_rolname));
		subinfo[i].subconninfo = pg_strdup(PQgetvalue(res, i, i_subconninfo));
		if (PQgetisnull(res, i, i_subslotname))
			subinfo[i].subslotname = NULL;
		else
			subinfo[i].subslotname = pg_strdup(PQgetvalue(res, i, i_subslotname));
		subinfo[i].subsynccommit =
			pg_strdup(PQgetvalue(res, i, i_subsynccommit));
		subinfo[i].subpublications =
			pg_strdup(PQgetvalue(res, i, i_subpublications));
		subinfo[i].subbinary =
			pg_strdup(PQgetvalue(res, i, i_subbinary));
		subinfo[i].substream =
			pg_strdup(PQgetvalue(res, i, i_substream));

		if (strlen(subinfo[i].rolname) == 0)
			pg_log_warning("owner of subscription \"%s\" appears to be invalid",
						   subinfo[i].dobj.name);

		/* Decide whether we want to dump it */
		selectDumpableObject(&(subinfo[i].dobj), fout);
	}
	PQclear(res);

	destroyPQExpBuffer(query);
}

/*
 * dumpSubscription
 *	  dump the definition of the given subscription
 */
static void
dumpSubscription(Archive *fout, SubscriptionInfo *subinfo)
{
	PQExpBuffer delq;
	PQExpBuffer query;
	PQExpBuffer publications;
	char	   *qsubname;
	char	  **pubnames = NULL;
	int			npubnames = 0;
	int			i;

	if (!(subinfo->dobj.dump & DUMP_COMPONENT_DEFINITION))
		return;

	delq = createPQExpBuffer();
	query = createPQExpBuffer();

	qsubname = pg_strdup(fmtId(subinfo->dobj.name));

	appendPQExpBuffer(delq, "DROP SUBSCRIPTION %s;\n",
					  qsubname);

	appendPQExpBuffer(query, "CREATE SUBSCRIPTION %s CONNECTION ",
					  qsubname);
	appendStringLiteralAH(query, subinfo->subconninfo, fout);

	/* Build list of quoted publications and append them to query. */
	if (!parsePGArray(subinfo->subpublications, &pubnames, &npubnames))
		fatal("could not parse subpublications array");

	publications = createPQExpBuffer();
	for (i = 0; i < npubnames; i++)
	{
		if (i > 0)
			appendPQExpBufferStr(publications, ", ");

		appendPQExpBufferStr(publications, fmtId(pubnames[i]));
	}

	appendPQExpBuffer(query, " PUBLICATION %s WITH (connect = false, slot_name = ", publications->data);
	if (subinfo->subslotname)
		appendStringLiteralAH(query, subinfo->subslotname, fout);
	else
		appendPQExpBufferStr(query, "NONE");

	if (strcmp(subinfo->subbinary, "t") == 0)
		appendPQExpBufferStr(query, ", binary = true");

	if (strcmp(subinfo->substream, "f") != 0)
		appendPQExpBufferStr(query, ", streaming = on");

	if (strcmp(subinfo->subsynccommit, "off") != 0)
		appendPQExpBuffer(query, ", synchronous_commit = %s", fmtId(subinfo->subsynccommit));

	appendPQExpBufferStr(query, ");\n");

	ArchiveEntry(fout, subinfo->dobj.catId, subinfo->dobj.dumpId,
				 ARCHIVE_OPTS(.tag = subinfo->dobj.name,
							  .owner = subinfo->rolname,
							  .description = "SUBSCRIPTION",
							  .section = SECTION_POST_DATA,
							  .createStmt = query->data,
							  .dropStmt = delq->data));

	if (subinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "SUBSCRIPTION", qsubname,
					NULL, subinfo->rolname,
					subinfo->dobj.catId, 0, subinfo->dobj.dumpId);

	if (subinfo->dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpSecLabel(fout, "SUBSCRIPTION", qsubname,
					 NULL, subinfo->rolname,
					 subinfo->dobj.catId, 0, subinfo->dobj.dumpId);

	destroyPQExpBuffer(publications);
	if (pubnames)
		free(pubnames);

	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(query);
	free(qsubname);
}

/*
 * Given a "create query", append as many ALTER ... DEPENDS ON EXTENSION as
 * the object needs.
 */
static void
append_depends_on_extension(Archive *fout,
							PQExpBuffer create,
							DumpableObject *dobj,
							const char *catalog,
							const char *keyword,
							const char *objname)
{
	if (dobj->depends_on_ext)
	{
		char	   *nm;
		PGresult   *res;
		PQExpBuffer query;
		int			ntups;
		int			i_extname;
		int			i;

		/* dodge fmtId() non-reentrancy */
		nm = pg_strdup(objname);

		query = createPQExpBuffer();
		appendPQExpBuffer(query,
						  "SELECT e.extname "
						  "FROM pg_catalog.pg_depend d, pg_catalog.pg_extension e "
						  "WHERE d.refobjid = e.oid AND classid = '%s'::pg_catalog.regclass "
						  "AND objid = '%u'::pg_catalog.oid AND deptype = 'x' "
						  "AND refclassid = 'pg_catalog.pg_extension'::pg_catalog.regclass",
						  catalog,
						  dobj->catId.oid);
		res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);
		ntups = PQntuples(res);
		i_extname = PQfnumber(res, "extname");
		for (i = 0; i < ntups; i++)
		{
			appendPQExpBuffer(create, "ALTER %s %s DEPENDS ON EXTENSION %s;\n",
							  keyword, nm,
							  fmtId(PQgetvalue(res, i, i_extname)));
		}

		PQclear(res);
		destroyPQExpBuffer(query);
		pg_free(nm);
	}
}


static void
binary_upgrade_set_type_oids_by_type_oid(Archive *fout,
										 PQExpBuffer upgrade_buffer,
										 Oid pg_type_oid,
										 bool force_array_type)
{
	PQExpBuffer upgrade_query = createPQExpBuffer();
	PGresult   *res;
	Oid			pg_type_array_oid;

	appendPQExpBufferStr(upgrade_buffer, "\n-- For binary upgrade, must preserve pg_type oid\n");
	appendPQExpBuffer(upgrade_buffer,
					  "SELECT pg_catalog.binary_upgrade_set_next_pg_type_oid('%u'::pg_catalog.oid);\n\n",
					  pg_type_oid);

	/* we only support old >= 8.3 for binary upgrades */
	appendPQExpBuffer(upgrade_query,
					  "SELECT typarray "
					  "FROM pg_catalog.pg_type "
					  "WHERE oid = '%u'::pg_catalog.oid;",
					  pg_type_oid);

	res = ExecuteSqlQueryForSingleRow(fout, upgrade_query->data);

	pg_type_array_oid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typarray")));

	PQclear(res);

	if (!OidIsValid(pg_type_array_oid) && force_array_type)
	{
		/*
		 * If the old version didn't assign an array type, but the new version
		 * does, we must select an unused type OID to assign.  This currently
		 * only happens for domains, when upgrading pre-v11 to v11 and up.
		 *
		 * Note: local state here is kind of ugly, but we must have some,
		 * since we mustn't choose the same unused OID more than once.
		 */
		static Oid	next_possible_free_oid = FirstNormalObjectId;
		bool		is_dup;

		do
		{
			++next_possible_free_oid;
			printfPQExpBuffer(upgrade_query,
							  "SELECT EXISTS(SELECT 1 "
							  "FROM pg_catalog.pg_type "
							  "WHERE oid = '%u'::pg_catalog.oid);",
							  next_possible_free_oid);
			res = ExecuteSqlQueryForSingleRow(fout, upgrade_query->data);
			is_dup = (PQgetvalue(res, 0, 0)[0] == 't');
			PQclear(res);
		} while (is_dup);

		pg_type_array_oid = next_possible_free_oid;
	}

	if (OidIsValid(pg_type_array_oid))
	{
		appendPQExpBufferStr(upgrade_buffer,
							 "\n-- For binary upgrade, must preserve pg_type array oid\n");
		appendPQExpBuffer(upgrade_buffer,
						  "SELECT pg_catalog.binary_upgrade_set_next_array_pg_type_oid('%u'::pg_catalog.oid);\n\n",
						  pg_type_array_oid);
	}

	destroyPQExpBuffer(upgrade_query);
}

static void
binary_upgrade_set_type_oids_by_rel_oid(Archive *fout,
										PQExpBuffer upgrade_buffer,
										Oid pg_rel_oid)
{
	PQExpBuffer upgrade_query = createPQExpBuffer();
	PGresult   *upgrade_res;
	Oid			pg_type_oid;

	appendPQExpBuffer(upgrade_query,
					  "SELECT c.reltype AS crel "
					  "FROM pg_catalog.pg_class c "
					  "WHERE c.oid = '%u'::pg_catalog.oid;",
					  pg_rel_oid);

	upgrade_res = ExecuteSqlQueryForSingleRow(fout, upgrade_query->data);

	pg_type_oid = atooid(PQgetvalue(upgrade_res, 0, PQfnumber(upgrade_res, "crel")));

	if (OidIsValid(pg_type_oid))
		binary_upgrade_set_type_oids_by_type_oid(fout, upgrade_buffer,
												 pg_type_oid, false);

	PQclear(upgrade_res);
	destroyPQExpBuffer(upgrade_query);
}

static void
binary_upgrade_set_pg_class_oids(Archive *fout,
								 PQExpBuffer upgrade_buffer, Oid pg_class_oid,
								 bool is_index)
{
	appendPQExpBufferStr(upgrade_buffer,
						 "\n-- For binary upgrade, must preserve pg_class oids\n");

	if (!is_index)
	{
		PQExpBuffer upgrade_query = createPQExpBuffer();
		PGresult   *upgrade_res;
		Oid			pg_class_reltoastrelid;
		char		pg_class_relkind;
		Oid			pg_index_indexrelid;

		appendPQExpBuffer(upgrade_buffer,
						  "SELECT pg_catalog.binary_upgrade_set_next_heap_pg_class_oid('%u'::pg_catalog.oid);\n",
						  pg_class_oid);

		/*
		 * Preserve the OIDs of the table's toast table and index, if any.
		 * Indexes cannot have toast tables, so we need not make this probe in
		 * the index code path.
		 *
		 * One complexity is that the current table definition might not
		 * require the creation of a TOAST table, but the old database might
		 * have a TOAST table that was created earlier, before some wide
		 * columns were dropped.  By setting the TOAST oid we force creation
		 * of the TOAST heap and index by the new backend, so we can copy the
		 * files during binary upgrade without worrying about this case.
		 */
		appendPQExpBuffer(upgrade_query,
						  "SELECT c.reltoastrelid, c.relkind, i.indexrelid "
						  "FROM pg_catalog.pg_class c LEFT JOIN "
						  "pg_catalog.pg_index i ON (c.reltoastrelid = i.indrelid AND i.indisvalid) "
						  "WHERE c.oid = '%u'::pg_catalog.oid;",
						  pg_class_oid);

		upgrade_res = ExecuteSqlQueryForSingleRow(fout, upgrade_query->data);

		pg_class_reltoastrelid = atooid(PQgetvalue(upgrade_res, 0,
												   PQfnumber(upgrade_res, "reltoastrelid")));
		pg_class_relkind = *PQgetvalue(upgrade_res, 0,
									   PQfnumber(upgrade_res, "relkind"));
		pg_index_indexrelid = atooid(PQgetvalue(upgrade_res, 0,
												PQfnumber(upgrade_res, "indexrelid")));

		/*
		 * In a pre-v12 database, partitioned tables might be marked as having
		 * toast tables, but we should ignore them if so.
		 */
		if (OidIsValid(pg_class_reltoastrelid) &&
			pg_class_relkind != RELKIND_PARTITIONED_TABLE)
		{
			appendPQExpBuffer(upgrade_buffer,
							  "SELECT pg_catalog.binary_upgrade_set_next_toast_pg_class_oid('%u'::pg_catalog.oid);\n",
							  pg_class_reltoastrelid);

			/* every toast table has an index */
			appendPQExpBuffer(upgrade_buffer,
							  "SELECT pg_catalog.binary_upgrade_set_next_index_pg_class_oid('%u'::pg_catalog.oid);\n",
							  pg_index_indexrelid);
		}

		PQclear(upgrade_res);
		destroyPQExpBuffer(upgrade_query);
	}
	else
		appendPQExpBuffer(upgrade_buffer,
						  "SELECT pg_catalog.binary_upgrade_set_next_index_pg_class_oid('%u'::pg_catalog.oid);\n",
						  pg_class_oid);

	appendPQExpBufferChar(upgrade_buffer, '\n');
}

/*
 * If the DumpableObject is a member of an extension, add a suitable
 * ALTER EXTENSION ADD command to the creation commands in upgrade_buffer.
 *
 * For somewhat historical reasons, objname should already be quoted,
 * but not objnamespace (if any).
 */
static void
binary_upgrade_extension_member(PQExpBuffer upgrade_buffer,
								DumpableObject *dobj,
								const char *objtype,
								const char *objname,
								const char *objnamespace)
{
	DumpableObject *extobj = NULL;
	int			i;

	if (!dobj->ext_member)
		return;

	/*
	 * Find the parent extension.  We could avoid this search if we wanted to
	 * add a link field to DumpableObject, but the space costs of that would
	 * be considerable.  We assume that member objects could only have a
	 * direct dependency on their own extension, not any others.
	 */
	for (i = 0; i < dobj->nDeps; i++)
	{
		extobj = findObjectByDumpId(dobj->dependencies[i]);
		if (extobj && extobj->objType == DO_EXTENSION)
			break;
		extobj = NULL;
	}
	if (extobj == NULL)
		fatal("could not find parent extension for %s %s",
			  objtype, objname);

	appendPQExpBufferStr(upgrade_buffer,
						 "\n-- For binary upgrade, handle extension membership the hard way\n");
	appendPQExpBuffer(upgrade_buffer, "ALTER EXTENSION %s ADD %s ",
					  fmtId(extobj->name),
					  objtype);
	if (objnamespace && *objnamespace)
		appendPQExpBuffer(upgrade_buffer, "%s.", fmtId(objnamespace));
	appendPQExpBuffer(upgrade_buffer, "%s;\n", objname);
}

/*
 * getNamespaces:
 *	  read all namespaces in the system catalogs and return them in the
 * NamespaceInfo* structure
 *
 *	numNamespaces is set to the number of namespaces read in
 */
NamespaceInfo *
getNamespaces(Archive *fout, int *numNamespaces)
{
	DumpOptions *dopt = fout->dopt;
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
	int			i_rnspacl;
	int			i_initnspacl;
	int			i_initrnspacl;

	query = createPQExpBuffer();

	/*
	 * we fetch all namespaces including system ones, so that every object we
	 * read in can be linked to a containing namespace.
	 */
	if (fout->remoteVersion >= 90600)
	{
		PQExpBuffer acl_subquery = createPQExpBuffer();
		PQExpBuffer racl_subquery = createPQExpBuffer();
		PQExpBuffer init_acl_subquery = createPQExpBuffer();
		PQExpBuffer init_racl_subquery = createPQExpBuffer();

		buildACLQueries(acl_subquery, racl_subquery, init_acl_subquery,
						init_racl_subquery, "n.nspacl", "n.nspowner", "'n'",
						dopt->binary_upgrade);

		appendPQExpBuffer(query, "SELECT n.tableoid, n.oid, n.nspname, "
						  "(%s nspowner) AS rolname, "
						  "%s as nspacl, "
						  "%s as rnspacl, "
						  "%s as initnspacl, "
						  "%s as initrnspacl "
						  "FROM pg_namespace n "
						  "LEFT JOIN pg_init_privs pip "
						  "ON (n.oid = pip.objoid "
						  "AND pip.classoid = 'pg_namespace'::regclass "
						  "AND pip.objsubid = 0",
						  username_subquery,
						  acl_subquery->data,
						  racl_subquery->data,
						  init_acl_subquery->data,
						  init_racl_subquery->data);

		appendPQExpBufferStr(query, ") ");

		destroyPQExpBuffer(acl_subquery);
		destroyPQExpBuffer(racl_subquery);
		destroyPQExpBuffer(init_acl_subquery);
		destroyPQExpBuffer(init_racl_subquery);
	}
	else
		appendPQExpBuffer(query, "SELECT tableoid, oid, nspname, "
						  "(%s nspowner) AS rolname, "
						  "nspacl, NULL as rnspacl, "
						  "NULL AS initnspacl, NULL as initrnspacl "
						  "FROM pg_namespace",
						  username_subquery);

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	nsinfo = (NamespaceInfo *) pg_malloc(ntups * sizeof(NamespaceInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_nspname = PQfnumber(res, "nspname");
	i_rolname = PQfnumber(res, "rolname");
	i_nspacl = PQfnumber(res, "nspacl");
	i_rnspacl = PQfnumber(res, "rnspacl");
	i_initnspacl = PQfnumber(res, "initnspacl");
	i_initrnspacl = PQfnumber(res, "initrnspacl");

	for (i = 0; i < ntups; i++)
	{
		nsinfo[i].dobj.objType = DO_NAMESPACE;
		nsinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		nsinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&nsinfo[i].dobj);
		nsinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_nspname));
		nsinfo[i].rolname = pg_strdup(PQgetvalue(res, i, i_rolname));
		nsinfo[i].nspacl = pg_strdup(PQgetvalue(res, i, i_nspacl));
		nsinfo[i].rnspacl = pg_strdup(PQgetvalue(res, i, i_rnspacl));
		nsinfo[i].initnspacl = pg_strdup(PQgetvalue(res, i, i_initnspacl));
		nsinfo[i].initrnspacl = pg_strdup(PQgetvalue(res, i, i_initrnspacl));

		/* Decide whether to dump this namespace */
		selectDumpableNamespace(&nsinfo[i], fout);

		/*
		 * Do not try to dump ACL if the ACL is empty or the default.
		 *
		 * This is useful because, for some schemas/objects, the only
		 * component we are going to try and dump is the ACL and if we can
		 * remove that then 'dump' goes to zero/false and we don't consider
		 * this object for dumping at all later on.
		 */
		if (PQgetisnull(res, i, i_nspacl) && PQgetisnull(res, i, i_rnspacl) &&
			PQgetisnull(res, i, i_initnspacl) &&
			PQgetisnull(res, i, i_initrnspacl))
			nsinfo[i].dobj.dump &= ~DUMP_COMPONENT_ACL;

		if (strlen(nsinfo[i].rolname) == 0)
			pg_log_warning("owner of schema \"%s\" appears to be invalid",
						   nsinfo[i].dobj.name);
	}

	PQclear(res);
	destroyPQExpBuffer(query);

	*numNamespaces = ntups;

	return nsinfo;
}

/*
 * findNamespace:
 *		given a namespace OID, look up the info read by getNamespaces
 */
static NamespaceInfo *
findNamespace(Oid nsoid)
{
	NamespaceInfo *nsinfo;

	nsinfo = findNamespaceByOid(nsoid);
	if (nsinfo == NULL)
		fatal("schema with OID %u does not exist", nsoid);
	return nsinfo;
}

/*
 * getExtensions:
 *	  read all extensions in the system catalogs and return them in the
 * ExtensionInfo* structure
 *
 *	numExtensions is set to the number of extensions read in
 */
ExtensionInfo *
getExtensions(Archive *fout, int *numExtensions)
{
	DumpOptions *dopt = fout->dopt;
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query;
	ExtensionInfo *extinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_extname;
	int			i_nspname;
	int			i_extrelocatable;
	int			i_extversion;
	int			i_extconfig;
	int			i_extcondition;

	/*
	 * Before 9.1, there are no extensions.
	 */
	if (fout->remoteVersion < 90100)
	{
		*numExtensions = 0;
		return NULL;
	}

	query = createPQExpBuffer();

	appendPQExpBufferStr(query, "SELECT x.tableoid, x.oid, "
						 "x.extname, n.nspname, x.extrelocatable, x.extversion, x.extconfig, x.extcondition "
						 "FROM pg_extension x "
						 "JOIN pg_namespace n ON n.oid = x.extnamespace");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	extinfo = (ExtensionInfo *) pg_malloc(ntups * sizeof(ExtensionInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_extname = PQfnumber(res, "extname");
	i_nspname = PQfnumber(res, "nspname");
	i_extrelocatable = PQfnumber(res, "extrelocatable");
	i_extversion = PQfnumber(res, "extversion");
	i_extconfig = PQfnumber(res, "extconfig");
	i_extcondition = PQfnumber(res, "extcondition");

	for (i = 0; i < ntups; i++)
	{
		extinfo[i].dobj.objType = DO_EXTENSION;
		extinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		extinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&extinfo[i].dobj);
		extinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_extname));
		extinfo[i].namespace = pg_strdup(PQgetvalue(res, i, i_nspname));
		extinfo[i].relocatable = *(PQgetvalue(res, i, i_extrelocatable)) == 't';
		extinfo[i].extversion = pg_strdup(PQgetvalue(res, i, i_extversion));
		extinfo[i].extconfig = pg_strdup(PQgetvalue(res, i, i_extconfig));
		extinfo[i].extcondition = pg_strdup(PQgetvalue(res, i, i_extcondition));

		/* Decide whether we want to dump it */
		selectDumpableExtension(&(extinfo[i]), dopt);
	}

	PQclear(res);
	destroyPQExpBuffer(query);

	*numExtensions = ntups;

	return extinfo;
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
getTypes(Archive *fout, int *numTypes)
{
	DumpOptions *dopt = fout->dopt;
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	TypeInfo   *tyinfo;
	ShellTypeInfo *stinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_typname;
	int			i_typnamespace;
	int			i_typacl;
	int			i_rtypacl;
	int			i_inittypacl;
	int			i_initrtypacl;
	int			i_rolname;
	int			i_typelem;
	int			i_typrelid;
	int			i_typrelkind;
	int			i_typtype;
	int			i_typisdefined;
	int			i_isarray;

	/*
	 * we include even the built-in types because those may be used as array
	 * elements by user-defined types
	 *
	 * we filter out the built-in types when we dump out the types
	 *
	 * same approach for undefined (shell) types and array types
	 *
	 * Note: as of 8.3 we can reliably detect whether a type is an
	 * auto-generated array type by checking the element type's typarray.
	 * (Before that the test is capable of generating false positives.) We
	 * still check for name beginning with '_', though, so as to avoid the
	 * cost of the subselect probe for all standard types.  This would have to
	 * be revisited if the backend ever allows renaming of array types.
	 */

	if (fout->remoteVersion >= 90600)
	{
		PQExpBuffer acl_subquery = createPQExpBuffer();
		PQExpBuffer racl_subquery = createPQExpBuffer();
		PQExpBuffer initacl_subquery = createPQExpBuffer();
		PQExpBuffer initracl_subquery = createPQExpBuffer();

		buildACLQueries(acl_subquery, racl_subquery, initacl_subquery,
						initracl_subquery, "t.typacl", "t.typowner", "'T'",
						dopt->binary_upgrade);

		appendPQExpBuffer(query, "SELECT t.tableoid, t.oid, t.typname, "
						  "t.typnamespace, "
						  "%s AS typacl, "
						  "%s AS rtypacl, "
						  "%s AS inittypacl, "
						  "%s AS initrtypacl, "
						  "(%s t.typowner) AS rolname, "
						  "t.typelem, t.typrelid, "
						  "CASE WHEN t.typrelid = 0 THEN ' '::\"char\" "
						  "ELSE (SELECT relkind FROM pg_class WHERE oid = t.typrelid) END AS typrelkind, "
						  "t.typtype, t.typisdefined, "
						  "t.typname[0] = '_' AND t.typelem != 0 AND "
						  "(SELECT typarray FROM pg_type te WHERE oid = t.typelem) = t.oid AS isarray "
						  "FROM pg_type t "
						  "LEFT JOIN pg_init_privs pip ON "
						  "(t.oid = pip.objoid "
						  "AND pip.classoid = 'pg_type'::regclass "
						  "AND pip.objsubid = 0) ",
						  acl_subquery->data,
						  racl_subquery->data,
						  initacl_subquery->data,
						  initracl_subquery->data,
						  username_subquery);

		destroyPQExpBuffer(acl_subquery);
		destroyPQExpBuffer(racl_subquery);
		destroyPQExpBuffer(initacl_subquery);
		destroyPQExpBuffer(initracl_subquery);
	}
	else if (fout->remoteVersion >= 90200)
	{
		appendPQExpBuffer(query, "SELECT tableoid, oid, typname, "
						  "typnamespace, typacl, NULL as rtypacl, "
						  "NULL AS inittypacl, NULL AS initrtypacl, "
						  "(%s typowner) AS rolname, "
						  "typelem, typrelid, "
						  "CASE WHEN typrelid = 0 THEN ' '::\"char\" "
						  "ELSE (SELECT relkind FROM pg_class WHERE oid = typrelid) END AS typrelkind, "
						  "typtype, typisdefined, "
						  "typname[0] = '_' AND typelem != 0 AND "
						  "(SELECT typarray FROM pg_type te WHERE oid = pg_type.typelem) = oid AS isarray "
						  "FROM pg_type",
						  username_subquery);
	}
	else if (fout->remoteVersion >= 80300)
	{
		appendPQExpBuffer(query, "SELECT tableoid, oid, typname, "
						  "typnamespace, NULL AS typacl, NULL as rtypacl, "
						  "NULL AS inittypacl, NULL AS initrtypacl, "
						  "(%s typowner) AS rolname, "
						  "typelem, typrelid, "
						  "CASE WHEN typrelid = 0 THEN ' '::\"char\" "
						  "ELSE (SELECT relkind FROM pg_class WHERE oid = typrelid) END AS typrelkind, "
						  "typtype, typisdefined, "
						  "typname[0] = '_' AND typelem != 0 AND "
						  "(SELECT typarray FROM pg_type te WHERE oid = pg_type.typelem) = oid AS isarray "
						  "FROM pg_type",
						  username_subquery);
	}
	else
	{
		appendPQExpBuffer(query, "SELECT tableoid, oid, typname, "
						  "typnamespace, NULL AS typacl, NULL as rtypacl, "
						  "NULL AS inittypacl, NULL AS initrtypacl, "
						  "(%s typowner) AS rolname, "
						  "typelem, typrelid, "
						  "CASE WHEN typrelid = 0 THEN ' '::\"char\" "
						  "ELSE (SELECT relkind FROM pg_class WHERE oid = typrelid) END AS typrelkind, "
						  "typtype, typisdefined, "
						  "typname[0] = '_' AND typelem != 0 AS isarray "
						  "FROM pg_type",
						  username_subquery);
	}

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	tyinfo = (TypeInfo *) pg_malloc(ntups * sizeof(TypeInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_typname = PQfnumber(res, "typname");
	i_typnamespace = PQfnumber(res, "typnamespace");
	i_typacl = PQfnumber(res, "typacl");
	i_rtypacl = PQfnumber(res, "rtypacl");
	i_inittypacl = PQfnumber(res, "inittypacl");
	i_initrtypacl = PQfnumber(res, "initrtypacl");
	i_rolname = PQfnumber(res, "rolname");
	i_typelem = PQfnumber(res, "typelem");
	i_typrelid = PQfnumber(res, "typrelid");
	i_typrelkind = PQfnumber(res, "typrelkind");
	i_typtype = PQfnumber(res, "typtype");
	i_typisdefined = PQfnumber(res, "typisdefined");
	i_isarray = PQfnumber(res, "isarray");

	for (i = 0; i < ntups; i++)
	{
		tyinfo[i].dobj.objType = DO_TYPE;
		tyinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		tyinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&tyinfo[i].dobj);
		tyinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_typname));
		tyinfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_typnamespace)));
		tyinfo[i].rolname = pg_strdup(PQgetvalue(res, i, i_rolname));
		tyinfo[i].typacl = pg_strdup(PQgetvalue(res, i, i_typacl));
		tyinfo[i].rtypacl = pg_strdup(PQgetvalue(res, i, i_rtypacl));
		tyinfo[i].inittypacl = pg_strdup(PQgetvalue(res, i, i_inittypacl));
		tyinfo[i].initrtypacl = pg_strdup(PQgetvalue(res, i, i_initrtypacl));
		tyinfo[i].typelem = atooid(PQgetvalue(res, i, i_typelem));
		tyinfo[i].typrelid = atooid(PQgetvalue(res, i, i_typrelid));
		tyinfo[i].typrelkind = *PQgetvalue(res, i, i_typrelkind);
		tyinfo[i].typtype = *PQgetvalue(res, i, i_typtype);
		tyinfo[i].shellType = NULL;

		if (strcmp(PQgetvalue(res, i, i_typisdefined), "t") == 0)
			tyinfo[i].isDefined = true;
		else
			tyinfo[i].isDefined = false;

		if (strcmp(PQgetvalue(res, i, i_isarray), "t") == 0)
			tyinfo[i].isArray = true;
		else
			tyinfo[i].isArray = false;

		/* Decide whether we want to dump it */
		selectDumpableType(&tyinfo[i], fout);

		/* Do not try to dump ACL if no ACL exists. */
		if (PQgetisnull(res, i, i_typacl) && PQgetisnull(res, i, i_rtypacl) &&
			PQgetisnull(res, i, i_inittypacl) &&
			PQgetisnull(res, i, i_initrtypacl))
			tyinfo[i].dobj.dump &= ~DUMP_COMPONENT_ACL;

		/*
		 * If it's a domain, fetch info about its constraints, if any
		 */
		tyinfo[i].nDomChecks = 0;
		tyinfo[i].domChecks = NULL;
		if ((tyinfo[i].dobj.dump & DUMP_COMPONENT_DEFINITION) &&
			tyinfo[i].typtype == TYPTYPE_DOMAIN)
			getDomainConstraints(fout, &(tyinfo[i]));

		/*
		 * If it's a base type, make a DumpableObject representing a shell
		 * definition of the type.  We will need to dump that ahead of the I/O
		 * functions for the type.  Similarly, range types need a shell
		 * definition in case they have a canonicalize function.
		 *
		 * Note: the shell type doesn't have a catId.  You might think it
		 * should copy the base type's catId, but then it might capture the
		 * pg_depend entries for the type, which we don't want.
		 */
		if ((tyinfo[i].dobj.dump & DUMP_COMPONENT_DEFINITION) &&
			(tyinfo[i].typtype == TYPTYPE_BASE ||
			 tyinfo[i].typtype == TYPTYPE_RANGE))
		{
			stinfo = (ShellTypeInfo *) pg_malloc(sizeof(ShellTypeInfo));
			stinfo->dobj.objType = DO_SHELL_TYPE;
			stinfo->dobj.catId = nilCatalogId;
			AssignDumpId(&stinfo->dobj);
			stinfo->dobj.name = pg_strdup(tyinfo[i].dobj.name);
			stinfo->dobj.namespace = tyinfo[i].dobj.namespace;
			stinfo->baseType = &(tyinfo[i]);
			tyinfo[i].shellType = stinfo;

			/*
			 * Initially mark the shell type as not to be dumped.  We'll only
			 * dump it if the I/O or canonicalize functions need to be dumped;
			 * this is taken care of while sorting dependencies.
			 */
			stinfo->dobj.dump = DUMP_COMPONENT_NONE;
		}

		if (strlen(tyinfo[i].rolname) == 0)
			pg_log_warning("owner of data type \"%s\" appears to be invalid",
						   tyinfo[i].dobj.name);
	}

	*numTypes = ntups;

	PQclear(res);

	destroyPQExpBuffer(query);

	return tyinfo;
}

/*
 * getOperators:
 *	  read all operators in the system catalogs and return them in the
 * OprInfo* structure
 *
 *	numOprs is set to the number of operators read in
 */
OprInfo *
getOperators(Archive *fout, int *numOprs)
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
	int			i_oprkind;
	int			i_oprcode;

	/*
	 * find all operators, including builtin operators; we filter out
	 * system-defined operators at dump-out time.
	 */

	appendPQExpBuffer(query, "SELECT tableoid, oid, oprname, "
					  "oprnamespace, "
					  "(%s oprowner) AS rolname, "
					  "oprkind, "
					  "oprcode::oid AS oprcode "
					  "FROM pg_operator",
					  username_subquery);

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numOprs = ntups;

	oprinfo = (OprInfo *) pg_malloc(ntups * sizeof(OprInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_oprname = PQfnumber(res, "oprname");
	i_oprnamespace = PQfnumber(res, "oprnamespace");
	i_rolname = PQfnumber(res, "rolname");
	i_oprkind = PQfnumber(res, "oprkind");
	i_oprcode = PQfnumber(res, "oprcode");

	for (i = 0; i < ntups; i++)
	{
		oprinfo[i].dobj.objType = DO_OPERATOR;
		oprinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		oprinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&oprinfo[i].dobj);
		oprinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_oprname));
		oprinfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_oprnamespace)));
		oprinfo[i].rolname = pg_strdup(PQgetvalue(res, i, i_rolname));
		oprinfo[i].oprkind = (PQgetvalue(res, i, i_oprkind))[0];
		oprinfo[i].oprcode = atooid(PQgetvalue(res, i, i_oprcode));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(oprinfo[i].dobj), fout);

		/* Operators do not currently have ACLs. */
		oprinfo[i].dobj.dump &= ~DUMP_COMPONENT_ACL;

		if (strlen(oprinfo[i].rolname) == 0)
			pg_log_warning("owner of operator \"%s\" appears to be invalid",
						   oprinfo[i].dobj.name);
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return oprinfo;
}

/*
 * getCollations:
 *	  read all collations in the system catalogs and return them in the
 * CollInfo* structure
 *
 *	numCollations is set to the number of collations read in
 */
CollInfo *
getCollations(Archive *fout, int *numCollations)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query;
	CollInfo   *collinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_collname;
	int			i_collnamespace;
	int			i_rolname;

	/* Collations didn't exist pre-9.1 */
	if (fout->remoteVersion < 90100)
	{
		*numCollations = 0;
		return NULL;
	}

	query = createPQExpBuffer();

	/*
	 * find all collations, including builtin collations; we filter out
	 * system-defined collations at dump-out time.
	 */

	appendPQExpBuffer(query, "SELECT tableoid, oid, collname, "
					  "collnamespace, "
					  "(%s collowner) AS rolname "
					  "FROM pg_collation",
					  username_subquery);

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numCollations = ntups;

	collinfo = (CollInfo *) pg_malloc(ntups * sizeof(CollInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_collname = PQfnumber(res, "collname");
	i_collnamespace = PQfnumber(res, "collnamespace");
	i_rolname = PQfnumber(res, "rolname");

	for (i = 0; i < ntups; i++)
	{
		collinfo[i].dobj.objType = DO_COLLATION;
		collinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		collinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&collinfo[i].dobj);
		collinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_collname));
		collinfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_collnamespace)));
		collinfo[i].rolname = pg_strdup(PQgetvalue(res, i, i_rolname));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(collinfo[i].dobj), fout);

		/* Collations do not currently have ACLs. */
		collinfo[i].dobj.dump &= ~DUMP_COMPONENT_ACL;
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return collinfo;
}

/*
 * getConversions:
 *	  read all conversions in the system catalogs and return them in the
 * ConvInfo* structure
 *
 *	numConversions is set to the number of conversions read in
 */
ConvInfo *
getConversions(Archive *fout, int *numConversions)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query;
	ConvInfo   *convinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_conname;
	int			i_connamespace;
	int			i_rolname;

	query = createPQExpBuffer();

	/*
	 * find all conversions, including builtin conversions; we filter out
	 * system-defined conversions at dump-out time.
	 */

	appendPQExpBuffer(query, "SELECT tableoid, oid, conname, "
					  "connamespace, "
					  "(%s conowner) AS rolname "
					  "FROM pg_conversion",
					  username_subquery);

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numConversions = ntups;

	convinfo = (ConvInfo *) pg_malloc(ntups * sizeof(ConvInfo));

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
		convinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_conname));
		convinfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_connamespace)));
		convinfo[i].rolname = pg_strdup(PQgetvalue(res, i, i_rolname));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(convinfo[i].dobj), fout);

		/* Conversions do not currently have ACLs. */
		convinfo[i].dobj.dump &= ~DUMP_COMPONENT_ACL;
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return convinfo;
}

/*
 * getAccessMethods:
 *	  read all user-defined access methods in the system catalogs and return
 *	  them in the AccessMethodInfo* structure
 *
 *	numAccessMethods is set to the number of access methods read in
 */
AccessMethodInfo *
getAccessMethods(Archive *fout, int *numAccessMethods)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query;
	AccessMethodInfo *aminfo;
	int			i_tableoid;
	int			i_oid;
	int			i_amname;
	int			i_amhandler;
	int			i_amtype;

	/* Before 9.6, there are no user-defined access methods */
	if (fout->remoteVersion < 90600)
	{
		*numAccessMethods = 0;
		return NULL;
	}

	query = createPQExpBuffer();

	/* Select all access methods from pg_am table */
	appendPQExpBufferStr(query, "SELECT tableoid, oid, amname, amtype, "
						 "amhandler::pg_catalog.regproc AS amhandler "
						 "FROM pg_am");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numAccessMethods = ntups;

	aminfo = (AccessMethodInfo *) pg_malloc(ntups * sizeof(AccessMethodInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_amname = PQfnumber(res, "amname");
	i_amhandler = PQfnumber(res, "amhandler");
	i_amtype = PQfnumber(res, "amtype");

	for (i = 0; i < ntups; i++)
	{
		aminfo[i].dobj.objType = DO_ACCESS_METHOD;
		aminfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		aminfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&aminfo[i].dobj);
		aminfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_amname));
		aminfo[i].dobj.namespace = NULL;
		aminfo[i].amhandler = pg_strdup(PQgetvalue(res, i, i_amhandler));
		aminfo[i].amtype = *(PQgetvalue(res, i, i_amtype));

		/* Decide whether we want to dump it */
		selectDumpableAccessMethod(&(aminfo[i]), fout);

		/* Access methods do not currently have ACLs. */
		aminfo[i].dobj.dump &= ~DUMP_COMPONENT_ACL;
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return aminfo;
}


/*
 * getOpclasses:
 *	  read all opclasses in the system catalogs and return them in the
 * OpclassInfo* structure
 *
 *	numOpclasses is set to the number of opclasses read in
 */
OpclassInfo *
getOpclasses(Archive *fout, int *numOpclasses)
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

	appendPQExpBuffer(query, "SELECT tableoid, oid, opcname, "
					  "opcnamespace, "
					  "(%s opcowner) AS rolname "
					  "FROM pg_opclass",
					  username_subquery);

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numOpclasses = ntups;

	opcinfo = (OpclassInfo *) pg_malloc(ntups * sizeof(OpclassInfo));

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
		opcinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_opcname));
		opcinfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_opcnamespace)));
		opcinfo[i].rolname = pg_strdup(PQgetvalue(res, i, i_rolname));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(opcinfo[i].dobj), fout);

		/* Op Classes do not currently have ACLs. */
		opcinfo[i].dobj.dump &= ~DUMP_COMPONENT_ACL;

		if (strlen(opcinfo[i].rolname) == 0)
			pg_log_warning("owner of operator class \"%s\" appears to be invalid",
						   opcinfo[i].dobj.name);
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return opcinfo;
}

/*
 * getOpfamilies:
 *	  read all opfamilies in the system catalogs and return them in the
 * OpfamilyInfo* structure
 *
 *	numOpfamilies is set to the number of opfamilies read in
 */
OpfamilyInfo *
getOpfamilies(Archive *fout, int *numOpfamilies)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query;
	OpfamilyInfo *opfinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_opfname;
	int			i_opfnamespace;
	int			i_rolname;

	/* Before 8.3, there is no separate concept of opfamilies */
	if (fout->remoteVersion < 80300)
	{
		*numOpfamilies = 0;
		return NULL;
	}

	query = createPQExpBuffer();

	/*
	 * find all opfamilies, including builtin opfamilies; we filter out
	 * system-defined opfamilies at dump-out time.
	 */

	appendPQExpBuffer(query, "SELECT tableoid, oid, opfname, "
					  "opfnamespace, "
					  "(%s opfowner) AS rolname "
					  "FROM pg_opfamily",
					  username_subquery);

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numOpfamilies = ntups;

	opfinfo = (OpfamilyInfo *) pg_malloc(ntups * sizeof(OpfamilyInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_opfname = PQfnumber(res, "opfname");
	i_opfnamespace = PQfnumber(res, "opfnamespace");
	i_rolname = PQfnumber(res, "rolname");

	for (i = 0; i < ntups; i++)
	{
		opfinfo[i].dobj.objType = DO_OPFAMILY;
		opfinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		opfinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&opfinfo[i].dobj);
		opfinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_opfname));
		opfinfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_opfnamespace)));
		opfinfo[i].rolname = pg_strdup(PQgetvalue(res, i, i_rolname));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(opfinfo[i].dobj), fout);

		/* Extensions do not currently have ACLs. */
		opfinfo[i].dobj.dump &= ~DUMP_COMPONENT_ACL;

		if (strlen(opfinfo[i].rolname) == 0)
			pg_log_warning("owner of operator family \"%s\" appears to be invalid",
						   opfinfo[i].dobj.name);
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return opfinfo;
}

/*
 * getAggregates:
 *	  read all the user-defined aggregates in the system catalogs and
 * return them in the AggInfo* structure
 *
 * numAggs is set to the number of aggregates read in
 */
AggInfo *
getAggregates(Archive *fout, int *numAggs)
{
	DumpOptions *dopt = fout->dopt;
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
	int			i_raggacl;
	int			i_initaggacl;
	int			i_initraggacl;

	/*
	 * Find all interesting aggregates.  See comment in getFuncs() for the
	 * rationale behind the filtering logic.
	 */
	if (fout->remoteVersion >= 90600)
	{
		PQExpBuffer acl_subquery = createPQExpBuffer();
		PQExpBuffer racl_subquery = createPQExpBuffer();
		PQExpBuffer initacl_subquery = createPQExpBuffer();
		PQExpBuffer initracl_subquery = createPQExpBuffer();
		const char *agg_check;

		buildACLQueries(acl_subquery, racl_subquery, initacl_subquery,
						initracl_subquery, "p.proacl", "p.proowner", "'f'",
						dopt->binary_upgrade);

		agg_check = (fout->remoteVersion >= 110000 ? "p.prokind = 'a'"
					 : "p.proisagg");

		appendPQExpBuffer(query, "SELECT p.tableoid, p.oid, "
						  "p.proname AS aggname, "
						  "p.pronamespace AS aggnamespace, "
						  "p.pronargs, p.proargtypes, "
						  "(%s p.proowner) AS rolname, "
						  "%s AS aggacl, "
						  "%s AS raggacl, "
						  "%s AS initaggacl, "
						  "%s AS initraggacl "
						  "FROM pg_proc p "
						  "LEFT JOIN pg_init_privs pip ON "
						  "(p.oid = pip.objoid "
						  "AND pip.classoid = 'pg_proc'::regclass "
						  "AND pip.objsubid = 0) "
						  "WHERE %s AND ("
						  "p.pronamespace != "
						  "(SELECT oid FROM pg_namespace "
						  "WHERE nspname = 'pg_catalog') OR "
						  "p.proacl IS DISTINCT FROM pip.initprivs",
						  username_subquery,
						  acl_subquery->data,
						  racl_subquery->data,
						  initacl_subquery->data,
						  initracl_subquery->data,
						  agg_check);
		if (dopt->binary_upgrade)
			appendPQExpBufferStr(query,
								 " OR EXISTS(SELECT 1 FROM pg_depend WHERE "
								 "classid = 'pg_proc'::regclass AND "
								 "objid = p.oid AND "
								 "refclassid = 'pg_extension'::regclass AND "
								 "deptype = 'e')");
		appendPQExpBufferChar(query, ')');

		destroyPQExpBuffer(acl_subquery);
		destroyPQExpBuffer(racl_subquery);
		destroyPQExpBuffer(initacl_subquery);
		destroyPQExpBuffer(initracl_subquery);
	}
	else if (fout->remoteVersion >= 80200)
	{
		appendPQExpBuffer(query, "SELECT tableoid, oid, proname AS aggname, "
						  "pronamespace AS aggnamespace, "
						  "pronargs, proargtypes, "
						  "(%s proowner) AS rolname, "
						  "proacl AS aggacl, "
						  "NULL AS raggacl, "
						  "NULL AS initaggacl, NULL AS initraggacl "
						  "FROM pg_proc p "
						  "WHERE proisagg AND ("
						  "pronamespace != "
						  "(SELECT oid FROM pg_namespace "
						  "WHERE nspname = 'pg_catalog')",
						  username_subquery);
		if (dopt->binary_upgrade && fout->remoteVersion >= 90100)
			appendPQExpBufferStr(query,
								 " OR EXISTS(SELECT 1 FROM pg_depend WHERE "
								 "classid = 'pg_proc'::regclass AND "
								 "objid = p.oid AND "
								 "refclassid = 'pg_extension'::regclass AND "
								 "deptype = 'e')");
		appendPQExpBufferChar(query, ')');
	}
	else
	{
		appendPQExpBuffer(query, "SELECT tableoid, oid, proname AS aggname, "
						  "pronamespace AS aggnamespace, "
						  "CASE WHEN proargtypes[0] = 'pg_catalog.\"any\"'::pg_catalog.regtype THEN 0 ELSE 1 END AS pronargs, "
						  "proargtypes, "
						  "(%s proowner) AS rolname, "
						  "proacl AS aggacl, "
						  "NULL AS raggacl, "
						  "NULL AS initaggacl, NULL AS initraggacl "
						  "FROM pg_proc "
						  "WHERE proisagg "
						  "AND pronamespace != "
						  "(SELECT oid FROM pg_namespace WHERE nspname = 'pg_catalog')",
						  username_subquery);
	}

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numAggs = ntups;

	agginfo = (AggInfo *) pg_malloc(ntups * sizeof(AggInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_aggname = PQfnumber(res, "aggname");
	i_aggnamespace = PQfnumber(res, "aggnamespace");
	i_pronargs = PQfnumber(res, "pronargs");
	i_proargtypes = PQfnumber(res, "proargtypes");
	i_rolname = PQfnumber(res, "rolname");
	i_aggacl = PQfnumber(res, "aggacl");
	i_raggacl = PQfnumber(res, "raggacl");
	i_initaggacl = PQfnumber(res, "initaggacl");
	i_initraggacl = PQfnumber(res, "initraggacl");

	for (i = 0; i < ntups; i++)
	{
		agginfo[i].aggfn.dobj.objType = DO_AGG;
		agginfo[i].aggfn.dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		agginfo[i].aggfn.dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&agginfo[i].aggfn.dobj);
		agginfo[i].aggfn.dobj.name = pg_strdup(PQgetvalue(res, i, i_aggname));
		agginfo[i].aggfn.dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_aggnamespace)));
		agginfo[i].aggfn.rolname = pg_strdup(PQgetvalue(res, i, i_rolname));
		if (strlen(agginfo[i].aggfn.rolname) == 0)
			pg_log_warning("owner of aggregate function \"%s\" appears to be invalid",
						   agginfo[i].aggfn.dobj.name);
		agginfo[i].aggfn.lang = InvalidOid; /* not currently interesting */
		agginfo[i].aggfn.prorettype = InvalidOid;	/* not saved */
		agginfo[i].aggfn.proacl = pg_strdup(PQgetvalue(res, i, i_aggacl));
		agginfo[i].aggfn.rproacl = pg_strdup(PQgetvalue(res, i, i_raggacl));
		agginfo[i].aggfn.initproacl = pg_strdup(PQgetvalue(res, i, i_initaggacl));
		agginfo[i].aggfn.initrproacl = pg_strdup(PQgetvalue(res, i, i_initraggacl));
		agginfo[i].aggfn.nargs = atoi(PQgetvalue(res, i, i_pronargs));
		if (agginfo[i].aggfn.nargs == 0)
			agginfo[i].aggfn.argtypes = NULL;
		else
		{
			agginfo[i].aggfn.argtypes = (Oid *) pg_malloc(agginfo[i].aggfn.nargs * sizeof(Oid));
			parseOidArray(PQgetvalue(res, i, i_proargtypes),
						  agginfo[i].aggfn.argtypes,
						  agginfo[i].aggfn.nargs);
		}

		/* Decide whether we want to dump it */
		selectDumpableObject(&(agginfo[i].aggfn.dobj), fout);

		/* Do not try to dump ACL if no ACL exists. */
		if (PQgetisnull(res, i, i_aggacl) && PQgetisnull(res, i, i_raggacl) &&
			PQgetisnull(res, i, i_initaggacl) &&
			PQgetisnull(res, i, i_initraggacl))
			agginfo[i].aggfn.dobj.dump &= ~DUMP_COMPONENT_ACL;
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
getFuncs(Archive *fout, int *numFuncs)
{
	DumpOptions *dopt = fout->dopt;
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
	int			i_rproacl;
	int			i_initproacl;
	int			i_initrproacl;

	/*
	 * Find all interesting functions.  This is a bit complicated:
	 *
	 * 1. Always exclude aggregates; those are handled elsewhere.
	 *
	 * 2. Always exclude functions that are internally dependent on something
	 * else, since presumably those will be created as a result of creating
	 * the something else.  This currently acts only to suppress constructor
	 * functions for range types (so we only need it in 9.2 and up).  Note
	 * this is OK only because the constructors don't have any dependencies
	 * the range type doesn't have; otherwise we might not get creation
	 * ordering correct.
	 *
	 * 3. Otherwise, we normally exclude functions in pg_catalog.  However, if
	 * they're members of extensions and we are in binary-upgrade mode then
	 * include them, since we want to dump extension members individually in
	 * that mode.  Also, if they are used by casts or transforms then we need
	 * to gather the information about them, though they won't be dumped if
	 * they are built-in.  Also, in 9.6 and up, include functions in
	 * pg_catalog if they have an ACL different from what's shown in
	 * pg_init_privs.
	 */
	if (fout->remoteVersion >= 90600)
	{
		PQExpBuffer acl_subquery = createPQExpBuffer();
		PQExpBuffer racl_subquery = createPQExpBuffer();
		PQExpBuffer initacl_subquery = createPQExpBuffer();
		PQExpBuffer initracl_subquery = createPQExpBuffer();
		const char *not_agg_check;

		buildACLQueries(acl_subquery, racl_subquery, initacl_subquery,
						initracl_subquery, "p.proacl", "p.proowner", "'f'",
						dopt->binary_upgrade);

		not_agg_check = (fout->remoteVersion >= 110000 ? "p.prokind <> 'a'"
						 : "NOT p.proisagg");

		appendPQExpBuffer(query,
						  "SELECT p.tableoid, p.oid, p.proname, p.prolang, "
						  "p.pronargs, p.proargtypes, p.prorettype, "
						  "%s AS proacl, "
						  "%s AS rproacl, "
						  "%s AS initproacl, "
						  "%s AS initrproacl, "
						  "p.pronamespace, "
						  "(%s p.proowner) AS rolname "
						  "FROM pg_proc p "
						  "LEFT JOIN pg_init_privs pip ON "
						  "(p.oid = pip.objoid "
						  "AND pip.classoid = 'pg_proc'::regclass "
						  "AND pip.objsubid = 0) "
						  "WHERE %s"
						  "\n  AND NOT EXISTS (SELECT 1 FROM pg_depend "
						  "WHERE classid = 'pg_proc'::regclass AND "
						  "objid = p.oid AND deptype = 'i')"
						  "\n  AND ("
						  "\n  pronamespace != "
						  "(SELECT oid FROM pg_namespace "
						  "WHERE nspname = 'pg_catalog')"
						  "\n  OR EXISTS (SELECT 1 FROM pg_cast"
						  "\n  WHERE pg_cast.oid > %u "
						  "\n  AND p.oid = pg_cast.castfunc)"
						  "\n  OR EXISTS (SELECT 1 FROM pg_transform"
						  "\n  WHERE pg_transform.oid > %u AND "
						  "\n  (p.oid = pg_transform.trffromsql"
						  "\n  OR p.oid = pg_transform.trftosql))",
						  acl_subquery->data,
						  racl_subquery->data,
						  initacl_subquery->data,
						  initracl_subquery->data,
						  username_subquery,
						  not_agg_check,
						  g_last_builtin_oid,
						  g_last_builtin_oid);
		if (dopt->binary_upgrade)
			appendPQExpBufferStr(query,
								 "\n  OR EXISTS(SELECT 1 FROM pg_depend WHERE "
								 "classid = 'pg_proc'::regclass AND "
								 "objid = p.oid AND "
								 "refclassid = 'pg_extension'::regclass AND "
								 "deptype = 'e')");
		appendPQExpBufferStr(query,
							 "\n  OR p.proacl IS DISTINCT FROM pip.initprivs");
		appendPQExpBufferChar(query, ')');

		destroyPQExpBuffer(acl_subquery);
		destroyPQExpBuffer(racl_subquery);
		destroyPQExpBuffer(initacl_subquery);
		destroyPQExpBuffer(initracl_subquery);
	}
	else
	{
		appendPQExpBuffer(query,
						  "SELECT tableoid, oid, proname, prolang, "
						  "pronargs, proargtypes, prorettype, proacl, "
						  "NULL as rproacl, "
						  "NULL as initproacl, NULL AS initrproacl, "
						  "pronamespace, "
						  "(%s proowner) AS rolname "
						  "FROM pg_proc p "
						  "WHERE NOT proisagg",
						  username_subquery);
		if (fout->remoteVersion >= 90200)
			appendPQExpBufferStr(query,
								 "\n  AND NOT EXISTS (SELECT 1 FROM pg_depend "
								 "WHERE classid = 'pg_proc'::regclass AND "
								 "objid = p.oid AND deptype = 'i')");
		appendPQExpBuffer(query,
						  "\n  AND ("
						  "\n  pronamespace != "
						  "(SELECT oid FROM pg_namespace "
						  "WHERE nspname = 'pg_catalog')"
						  "\n  OR EXISTS (SELECT 1 FROM pg_cast"
						  "\n  WHERE pg_cast.oid > '%u'::oid"
						  "\n  AND p.oid = pg_cast.castfunc)",
						  g_last_builtin_oid);

		if (fout->remoteVersion >= 90500)
			appendPQExpBuffer(query,
							  "\n  OR EXISTS (SELECT 1 FROM pg_transform"
							  "\n  WHERE pg_transform.oid > '%u'::oid"
							  "\n  AND (p.oid = pg_transform.trffromsql"
							  "\n  OR p.oid = pg_transform.trftosql))",
							  g_last_builtin_oid);

		if (dopt->binary_upgrade && fout->remoteVersion >= 90100)
			appendPQExpBufferStr(query,
								 "\n  OR EXISTS(SELECT 1 FROM pg_depend WHERE "
								 "classid = 'pg_proc'::regclass AND "
								 "objid = p.oid AND "
								 "refclassid = 'pg_extension'::regclass AND "
								 "deptype = 'e')");
		appendPQExpBufferChar(query, ')');
	}

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	*numFuncs = ntups;

	finfo = (FuncInfo *) pg_malloc0(ntups * sizeof(FuncInfo));

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
	i_rproacl = PQfnumber(res, "rproacl");
	i_initproacl = PQfnumber(res, "initproacl");
	i_initrproacl = PQfnumber(res, "initrproacl");

	for (i = 0; i < ntups; i++)
	{
		finfo[i].dobj.objType = DO_FUNC;
		finfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		finfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&finfo[i].dobj);
		finfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_proname));
		finfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_pronamespace)));
		finfo[i].rolname = pg_strdup(PQgetvalue(res, i, i_rolname));
		finfo[i].lang = atooid(PQgetvalue(res, i, i_prolang));
		finfo[i].prorettype = atooid(PQgetvalue(res, i, i_prorettype));
		finfo[i].proacl = pg_strdup(PQgetvalue(res, i, i_proacl));
		finfo[i].rproacl = pg_strdup(PQgetvalue(res, i, i_rproacl));
		finfo[i].initproacl = pg_strdup(PQgetvalue(res, i, i_initproacl));
		finfo[i].initrproacl = pg_strdup(PQgetvalue(res, i, i_initrproacl));
		finfo[i].nargs = atoi(PQgetvalue(res, i, i_pronargs));
		if (finfo[i].nargs == 0)
			finfo[i].argtypes = NULL;
		else
		{
			finfo[i].argtypes = (Oid *) pg_malloc(finfo[i].nargs * sizeof(Oid));
			parseOidArray(PQgetvalue(res, i, i_proargtypes),
						  finfo[i].argtypes, finfo[i].nargs);
		}

		/* Decide whether we want to dump it */
		selectDumpableObject(&(finfo[i].dobj), fout);

		/* Do not try to dump ACL if no ACL exists. */
		if (PQgetisnull(res, i, i_proacl) && PQgetisnull(res, i, i_rproacl) &&
			PQgetisnull(res, i, i_initproacl) &&
			PQgetisnull(res, i, i_initrproacl))
			finfo[i].dobj.dump &= ~DUMP_COMPONENT_ACL;

		if (strlen(finfo[i].rolname) == 0)
			pg_log_warning("owner of function \"%s\" appears to be invalid",
						   finfo[i].dobj.name);
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return finfo;
}

/*
 * getTables
 *	  read all the tables (no indexes)
 * in the system catalogs return them in the TableInfo* structure
 *
 * numTables is set to the number of tables read in
 */
TableInfo *
getTables(Archive *fout, int *numTables)
{
	DumpOptions *dopt = fout->dopt;
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	TableInfo  *tblinfo;
	int			i_reltableoid;
	int			i_reloid;
	int			i_relname;
	int			i_relnamespace;
	int			i_relkind;
	int			i_relacl;
	int			i_rrelacl;
	int			i_initrelacl;
	int			i_initrrelacl;
	int			i_rolname;
	int			i_relchecks;
	int			i_relhastriggers;
	int			i_relhasindex;
	int			i_relhasrules;
	int			i_relrowsec;
	int			i_relforcerowsec;
	int			i_relhasoids;
	int			i_relfrozenxid;
	int			i_relminmxid;
	int			i_toastoid;
	int			i_toastfrozenxid;
	int			i_toastminmxid;
	int			i_relpersistence;
	int			i_relispopulated;
	int			i_relreplident;
	int			i_owning_tab;
	int			i_owning_col;
	int			i_reltablespace;
	int			i_reloptions;
	int			i_checkoption;
	int			i_toastreloptions;
	int			i_reloftype;
	int			i_relpages;
	int			i_foreignserver;
	int			i_is_identity_sequence;
	int			i_changed_acl;
	int			i_partkeydef;
	int			i_ispartition;
	int			i_partbound;
	int			i_amname;

	/*
	 * Find all the tables and table-like objects.
	 *
	 * We include system catalogs, so that we can work if a user table is
	 * defined to inherit from a system catalog (pretty weird, but...)
	 *
	 * We ignore relations that are not ordinary tables, sequences, views,
	 * materialized views, composite types, or foreign tables.
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
	 *
	 * We purposefully ignore toast OIDs for partitioned tables; the reason is
	 * that versions 10 and 11 have them, but 12 does not, so emitting them
	 * causes the upgrade to fail.
	 */

	if (fout->remoteVersion >= 90600)
	{
		char	   *partkeydef = "NULL";
		char	   *ispartition = "false";
		char	   *partbound = "NULL";
		char	   *relhasoids = "c.relhasoids";

		PQExpBuffer acl_subquery = createPQExpBuffer();
		PQExpBuffer racl_subquery = createPQExpBuffer();
		PQExpBuffer initacl_subquery = createPQExpBuffer();
		PQExpBuffer initracl_subquery = createPQExpBuffer();

		PQExpBuffer attacl_subquery = createPQExpBuffer();
		PQExpBuffer attracl_subquery = createPQExpBuffer();
		PQExpBuffer attinitacl_subquery = createPQExpBuffer();
		PQExpBuffer attinitracl_subquery = createPQExpBuffer();

		/*
		 * Collect the information about any partitioned tables, which were
		 * added in PG10.
		 */

		if (fout->remoteVersion >= 100000)
		{
			partkeydef = "pg_get_partkeydef(c.oid)";
			ispartition = "c.relispartition";
			partbound = "pg_get_expr(c.relpartbound, c.oid)";
		}

		/* In PG12 upwards WITH OIDS does not exist anymore. */
		if (fout->remoteVersion >= 120000)
			relhasoids = "'f'::bool";

		/*
		 * Left join to pick up dependency info linking sequences to their
		 * owning column, if any (note this dependency is AUTO as of 8.2)
		 *
		 * Left join to detect if any privileges are still as-set-at-init, in
		 * which case we won't dump out ACL commands for those.
		 */

		buildACLQueries(acl_subquery, racl_subquery, initacl_subquery,
						initracl_subquery, "c.relacl", "c.relowner",
						"CASE WHEN c.relkind = " CppAsString2(RELKIND_SEQUENCE)
						" THEN 's' ELSE 'r' END::\"char\"",
						dopt->binary_upgrade);

		buildACLQueries(attacl_subquery, attracl_subquery, attinitacl_subquery,
						attinitracl_subquery, "at.attacl", "c.relowner", "'c'",
						dopt->binary_upgrade);

		appendPQExpBuffer(query,
						  "SELECT c.tableoid, c.oid, c.relname, "
						  "%s AS relacl, %s as rrelacl, "
						  "%s AS initrelacl, %s as initrrelacl, "
						  "c.relkind, c.relnamespace, "
						  "(%s c.relowner) AS rolname, "
						  "c.relchecks, c.relhastriggers, "
						  "c.relhasindex, c.relhasrules, %s AS relhasoids, "
						  "c.relrowsecurity, c.relforcerowsecurity, "
						  "c.relfrozenxid, c.relminmxid, tc.oid AS toid, "
						  "tc.relfrozenxid AS tfrozenxid, "
						  "tc.relminmxid AS tminmxid, "
						  "c.relpersistence, c.relispopulated, "
						  "c.relreplident, c.relpages, am.amname, "
						  "CASE WHEN c.relkind = 'f' THEN "
						  "(SELECT ftserver FROM pg_catalog.pg_foreign_table WHERE ftrelid = c.oid) "
						  "ELSE 0 END AS foreignserver, "
						  "CASE WHEN c.reloftype <> 0 THEN c.reloftype::pg_catalog.regtype ELSE NULL END AS reloftype, "
						  "d.refobjid AS owning_tab, "
						  "d.refobjsubid AS owning_col, "
						  "(SELECT spcname FROM pg_tablespace t WHERE t.oid = c.reltablespace) AS reltablespace, "
						  "array_remove(array_remove(c.reloptions,'check_option=local'),'check_option=cascaded') AS reloptions, "
						  "CASE WHEN 'check_option=local' = ANY (c.reloptions) THEN 'LOCAL'::text "
						  "WHEN 'check_option=cascaded' = ANY (c.reloptions) THEN 'CASCADED'::text ELSE NULL END AS checkoption, "
						  "tc.reloptions AS toast_reloptions, "
						  "c.relkind = '%c' AND EXISTS (SELECT 1 FROM pg_depend WHERE classid = 'pg_class'::regclass AND objid = c.oid AND objsubid = 0 AND refclassid = 'pg_class'::regclass AND deptype = 'i') AS is_identity_sequence, "
						  "EXISTS (SELECT 1 FROM pg_attribute at LEFT JOIN pg_init_privs pip ON "
						  "(c.oid = pip.objoid "
						  "AND pip.classoid = 'pg_class'::regclass "
						  "AND pip.objsubid = at.attnum)"
						  "WHERE at.attrelid = c.oid AND ("
						  "%s IS NOT NULL "
						  "OR %s IS NOT NULL "
						  "OR %s IS NOT NULL "
						  "OR %s IS NOT NULL"
						  "))"
						  "AS changed_acl, "
						  "%s AS partkeydef, "
						  "%s AS ispartition, "
						  "%s AS partbound "
						  "FROM pg_class c "
						  "LEFT JOIN pg_depend d ON "
						  "(c.relkind = '%c' AND "
						  "d.classid = c.tableoid AND d.objid = c.oid AND "
						  "d.objsubid = 0 AND "
						  "d.refclassid = c.tableoid AND d.deptype IN ('a', 'i')) "
						  "LEFT JOIN pg_class tc ON (c.reltoastrelid = tc.oid AND c.relkind <> '%c') "
						  "LEFT JOIN pg_am am ON (c.relam = am.oid) "
						  "LEFT JOIN pg_init_privs pip ON "
						  "(c.oid = pip.objoid "
						  "AND pip.classoid = 'pg_class'::regclass "
						  "AND pip.objsubid = 0) "
						  "WHERE c.relkind in ('%c', '%c', '%c', '%c', '%c', '%c', '%c') "
						  "ORDER BY c.oid",
						  acl_subquery->data,
						  racl_subquery->data,
						  initacl_subquery->data,
						  initracl_subquery->data,
						  username_subquery,
						  relhasoids,
						  RELKIND_SEQUENCE,
						  attacl_subquery->data,
						  attracl_subquery->data,
						  attinitacl_subquery->data,
						  attinitracl_subquery->data,
						  partkeydef,
						  ispartition,
						  partbound,
						  RELKIND_SEQUENCE,
						  RELKIND_PARTITIONED_TABLE,
						  RELKIND_RELATION, RELKIND_SEQUENCE,
						  RELKIND_VIEW, RELKIND_COMPOSITE_TYPE,
						  RELKIND_MATVIEW, RELKIND_FOREIGN_TABLE,
						  RELKIND_PARTITIONED_TABLE);

		destroyPQExpBuffer(acl_subquery);
		destroyPQExpBuffer(racl_subquery);
		destroyPQExpBuffer(initacl_subquery);
		destroyPQExpBuffer(initracl_subquery);

		destroyPQExpBuffer(attacl_subquery);
		destroyPQExpBuffer(attracl_subquery);
		destroyPQExpBuffer(attinitacl_subquery);
		destroyPQExpBuffer(attinitracl_subquery);
	}
	else if (fout->remoteVersion >= 90500)
	{
		/*
		 * Left join to pick up dependency info linking sequences to their
		 * owning column, if any (note this dependency is AUTO as of 8.2)
		 */
		appendPQExpBuffer(query,
						  "SELECT c.tableoid, c.oid, c.relname, "
						  "c.relacl, NULL as rrelacl, "
						  "NULL AS initrelacl, NULL AS initrrelacl, "
						  "c.relkind, "
						  "c.relnamespace, "
						  "(%s c.relowner) AS rolname, "
						  "c.relchecks, c.relhastriggers, "
						  "c.relhasindex, c.relhasrules, c.relhasoids, "
						  "c.relrowsecurity, c.relforcerowsecurity, "
						  "c.relfrozenxid, c.relminmxid, tc.oid AS toid, "
						  "tc.relfrozenxid AS tfrozenxid, "
						  "tc.relminmxid AS tminmxid, "
						  "c.relpersistence, c.relispopulated, "
						  "c.relreplident, c.relpages, "
						  "NULL AS amname, "
						  "CASE WHEN c.relkind = 'f' THEN "
						  "(SELECT ftserver FROM pg_catalog.pg_foreign_table WHERE ftrelid = c.oid) "
						  "ELSE 0 END AS foreignserver, "
						  "CASE WHEN c.reloftype <> 0 THEN c.reloftype::pg_catalog.regtype ELSE NULL END AS reloftype, "
						  "d.refobjid AS owning_tab, "
						  "d.refobjsubid AS owning_col, "
						  "(SELECT spcname FROM pg_tablespace t WHERE t.oid = c.reltablespace) AS reltablespace, "
						  "array_remove(array_remove(c.reloptions,'check_option=local'),'check_option=cascaded') AS reloptions, "
						  "CASE WHEN 'check_option=local' = ANY (c.reloptions) THEN 'LOCAL'::text "
						  "WHEN 'check_option=cascaded' = ANY (c.reloptions) THEN 'CASCADED'::text ELSE NULL END AS checkoption, "
						  "tc.reloptions AS toast_reloptions, "
						  "NULL AS changed_acl, "
						  "NULL AS partkeydef, "
						  "false AS ispartition, "
						  "NULL AS partbound "
						  "FROM pg_class c "
						  "LEFT JOIN pg_depend d ON "
						  "(c.relkind = '%c' AND "
						  "d.classid = c.tableoid AND d.objid = c.oid AND "
						  "d.objsubid = 0 AND "
						  "d.refclassid = c.tableoid AND d.deptype = 'a') "
						  "LEFT JOIN pg_class tc ON (c.reltoastrelid = tc.oid) "
						  "WHERE c.relkind in ('%c', '%c', '%c', '%c', '%c', '%c') "
						  "ORDER BY c.oid",
						  username_subquery,
						  RELKIND_SEQUENCE,
						  RELKIND_RELATION, RELKIND_SEQUENCE,
						  RELKIND_VIEW, RELKIND_COMPOSITE_TYPE,
						  RELKIND_MATVIEW, RELKIND_FOREIGN_TABLE);
	}
	else if (fout->remoteVersion >= 90400)
	{
		/*
		 * Left join to pick up dependency info linking sequences to their
		 * owning column, if any (note this dependency is AUTO as of 8.2)
		 */
		appendPQExpBuffer(query,
						  "SELECT c.tableoid, c.oid, c.relname, "
						  "c.relacl, NULL as rrelacl, "
						  "NULL AS initrelacl, NULL AS initrrelacl, "
						  "c.relkind, "
						  "c.relnamespace, "
						  "(%s c.relowner) AS rolname, "
						  "c.relchecks, c.relhastriggers, "
						  "c.relhasindex, c.relhasrules, c.relhasoids, "
						  "'f'::bool AS relrowsecurity, "
						  "'f'::bool AS relforcerowsecurity, "
						  "c.relfrozenxid, c.relminmxid, tc.oid AS toid, "
						  "tc.relfrozenxid AS tfrozenxid, "
						  "tc.relminmxid AS tminmxid, "
						  "c.relpersistence, c.relispopulated, "
						  "c.relreplident, c.relpages, "
						  "NULL AS amname, "
						  "CASE WHEN c.relkind = 'f' THEN "
						  "(SELECT ftserver FROM pg_catalog.pg_foreign_table WHERE ftrelid = c.oid) "
						  "ELSE 0 END AS foreignserver, "
						  "CASE WHEN c.reloftype <> 0 THEN c.reloftype::pg_catalog.regtype ELSE NULL END AS reloftype, "
						  "d.refobjid AS owning_tab, "
						  "d.refobjsubid AS owning_col, "
						  "(SELECT spcname FROM pg_tablespace t WHERE t.oid = c.reltablespace) AS reltablespace, "
						  "array_remove(array_remove(c.reloptions,'check_option=local'),'check_option=cascaded') AS reloptions, "
						  "CASE WHEN 'check_option=local' = ANY (c.reloptions) THEN 'LOCAL'::text "
						  "WHEN 'check_option=cascaded' = ANY (c.reloptions) THEN 'CASCADED'::text ELSE NULL END AS checkoption, "
						  "tc.reloptions AS toast_reloptions, "
						  "NULL AS changed_acl, "
						  "NULL AS partkeydef, "
						  "false AS ispartition, "
						  "NULL AS partbound "
						  "FROM pg_class c "
						  "LEFT JOIN pg_depend d ON "
						  "(c.relkind = '%c' AND "
						  "d.classid = c.tableoid AND d.objid = c.oid AND "
						  "d.objsubid = 0 AND "
						  "d.refclassid = c.tableoid AND d.deptype = 'a') "
						  "LEFT JOIN pg_class tc ON (c.reltoastrelid = tc.oid) "
						  "WHERE c.relkind in ('%c', '%c', '%c', '%c', '%c', '%c') "
						  "ORDER BY c.oid",
						  username_subquery,
						  RELKIND_SEQUENCE,
						  RELKIND_RELATION, RELKIND_SEQUENCE,
						  RELKIND_VIEW, RELKIND_COMPOSITE_TYPE,
						  RELKIND_MATVIEW, RELKIND_FOREIGN_TABLE);
	}
	else if (fout->remoteVersion >= 90300)
	{
		/*
		 * Left join to pick up dependency info linking sequences to their
		 * owning column, if any (note this dependency is AUTO as of 8.2)
		 */
		appendPQExpBuffer(query,
						  "SELECT c.tableoid, c.oid, c.relname, "
						  "c.relacl, NULL as rrelacl, "
						  "NULL AS initrelacl, NULL AS initrrelacl, "
						  "c.relkind, "
						  "c.relnamespace, "
						  "(%s c.relowner) AS rolname, "
						  "c.relchecks, c.relhastriggers, "
						  "c.relhasindex, c.relhasrules, c.relhasoids, "
						  "'f'::bool AS relrowsecurity, "
						  "'f'::bool AS relforcerowsecurity, "
						  "c.relfrozenxid, c.relminmxid, tc.oid AS toid, "
						  "tc.relfrozenxid AS tfrozenxid, "
						  "tc.relminmxid AS tminmxid, "
						  "c.relpersistence, c.relispopulated, "
						  "'d' AS relreplident, c.relpages, "
						  "NULL AS amname, "
						  "CASE WHEN c.relkind = 'f' THEN "
						  "(SELECT ftserver FROM pg_catalog.pg_foreign_table WHERE ftrelid = c.oid) "
						  "ELSE 0 END AS foreignserver, "
						  "CASE WHEN c.reloftype <> 0 THEN c.reloftype::pg_catalog.regtype ELSE NULL END AS reloftype, "
						  "d.refobjid AS owning_tab, "
						  "d.refobjsubid AS owning_col, "
						  "(SELECT spcname FROM pg_tablespace t WHERE t.oid = c.reltablespace) AS reltablespace, "
						  "array_remove(array_remove(c.reloptions,'check_option=local'),'check_option=cascaded') AS reloptions, "
						  "CASE WHEN 'check_option=local' = ANY (c.reloptions) THEN 'LOCAL'::text "
						  "WHEN 'check_option=cascaded' = ANY (c.reloptions) THEN 'CASCADED'::text ELSE NULL END AS checkoption, "
						  "tc.reloptions AS toast_reloptions, "
						  "NULL AS changed_acl, "
						  "NULL AS partkeydef, "
						  "false AS ispartition, "
						  "NULL AS partbound "
						  "FROM pg_class c "
						  "LEFT JOIN pg_depend d ON "
						  "(c.relkind = '%c' AND "
						  "d.classid = c.tableoid AND d.objid = c.oid AND "
						  "d.objsubid = 0 AND "
						  "d.refclassid = c.tableoid AND d.deptype = 'a') "
						  "LEFT JOIN pg_class tc ON (c.reltoastrelid = tc.oid) "
						  "WHERE c.relkind in ('%c', '%c', '%c', '%c', '%c', '%c') "
						  "ORDER BY c.oid",
						  username_subquery,
						  RELKIND_SEQUENCE,
						  RELKIND_RELATION, RELKIND_SEQUENCE,
						  RELKIND_VIEW, RELKIND_COMPOSITE_TYPE,
						  RELKIND_MATVIEW, RELKIND_FOREIGN_TABLE);
	}
	else if (fout->remoteVersion >= 90100)
	{
		/*
		 * Left join to pick up dependency info linking sequences to their
		 * owning column, if any (note this dependency is AUTO as of 8.2)
		 */
		appendPQExpBuffer(query,
						  "SELECT c.tableoid, c.oid, c.relname, "
						  "c.relacl, NULL as rrelacl, "
						  "NULL AS initrelacl, NULL AS initrrelacl, "
						  "c.relkind, "
						  "c.relnamespace, "
						  "(%s c.relowner) AS rolname, "
						  "c.relchecks, c.relhastriggers, "
						  "c.relhasindex, c.relhasrules, c.relhasoids, "
						  "'f'::bool AS relrowsecurity, "
						  "'f'::bool AS relforcerowsecurity, "
						  "c.relfrozenxid, 0 AS relminmxid, tc.oid AS toid, "
						  "tc.relfrozenxid AS tfrozenxid, "
						  "0 AS tminmxid, "
						  "c.relpersistence, 't' as relispopulated, "
						  "'d' AS relreplident, c.relpages, "
						  "NULL AS amname, "
						  "CASE WHEN c.relkind = 'f' THEN "
						  "(SELECT ftserver FROM pg_catalog.pg_foreign_table WHERE ftrelid = c.oid) "
						  "ELSE 0 END AS foreignserver, "
						  "CASE WHEN c.reloftype <> 0 THEN c.reloftype::pg_catalog.regtype ELSE NULL END AS reloftype, "
						  "d.refobjid AS owning_tab, "
						  "d.refobjsubid AS owning_col, "
						  "(SELECT spcname FROM pg_tablespace t WHERE t.oid = c.reltablespace) AS reltablespace, "
						  "c.reloptions AS reloptions, "
						  "tc.reloptions AS toast_reloptions, "
						  "NULL AS changed_acl, "
						  "NULL AS partkeydef, "
						  "false AS ispartition, "
						  "NULL AS partbound "
						  "FROM pg_class c "
						  "LEFT JOIN pg_depend d ON "
						  "(c.relkind = '%c' AND "
						  "d.classid = c.tableoid AND d.objid = c.oid AND "
						  "d.objsubid = 0 AND "
						  "d.refclassid = c.tableoid AND d.deptype = 'a') "
						  "LEFT JOIN pg_class tc ON (c.reltoastrelid = tc.oid) "
						  "WHERE c.relkind in ('%c', '%c', '%c', '%c', '%c', '%c') "
						  "ORDER BY c.oid",
						  username_subquery,
						  RELKIND_SEQUENCE,
						  RELKIND_RELATION, RELKIND_SEQUENCE,
						  RELKIND_VIEW, RELKIND_COMPOSITE_TYPE,
						  RELKIND_MATVIEW, RELKIND_FOREIGN_TABLE);
	}
	else if (fout->remoteVersion >= 90000)
	{
		/*
		 * Left join to pick up dependency info linking sequences to their
		 * owning column, if any (note this dependency is AUTO as of 8.2)
		 */
		appendPQExpBuffer(query,
						  "SELECT c.tableoid, c.oid, c.relname, "
						  "c.relacl, NULL as rrelacl, "
						  "NULL AS initrelacl, NULL AS initrrelacl, "
						  "c.relkind, "
						  "c.relnamespace, "
						  "(%s c.relowner) AS rolname, "
						  "c.relchecks, c.relhastriggers, "
						  "c.relhasindex, c.relhasrules, c.relhasoids, "
						  "'f'::bool AS relrowsecurity, "
						  "'f'::bool AS relforcerowsecurity, "
						  "c.relfrozenxid, 0 AS relminmxid, tc.oid AS toid, "
						  "tc.relfrozenxid AS tfrozenxid, "
						  "0 AS tminmxid, "
						  "'p' AS relpersistence, 't' as relispopulated, "
						  "'d' AS relreplident, c.relpages, "
						  "NULL AS amname, "
						  "NULL AS foreignserver, "
						  "CASE WHEN c.reloftype <> 0 THEN c.reloftype::pg_catalog.regtype ELSE NULL END AS reloftype, "
						  "d.refobjid AS owning_tab, "
						  "d.refobjsubid AS owning_col, "
						  "(SELECT spcname FROM pg_tablespace t WHERE t.oid = c.reltablespace) AS reltablespace, "
						  "c.reloptions AS reloptions, "
						  "tc.reloptions AS toast_reloptions, "
						  "NULL AS changed_acl, "
						  "NULL AS partkeydef, "
						  "false AS ispartition, "
						  "NULL AS partbound "
						  "FROM pg_class c "
						  "LEFT JOIN pg_depend d ON "
						  "(c.relkind = '%c' AND "
						  "d.classid = c.tableoid AND d.objid = c.oid AND "
						  "d.objsubid = 0 AND "
						  "d.refclassid = c.tableoid AND d.deptype = 'a') "
						  "LEFT JOIN pg_class tc ON (c.reltoastrelid = tc.oid) "
						  "WHERE c.relkind in ('%c', '%c', '%c', '%c') "
						  "ORDER BY c.oid",
						  username_subquery,
						  RELKIND_SEQUENCE,
						  RELKIND_RELATION, RELKIND_SEQUENCE,
						  RELKIND_VIEW, RELKIND_COMPOSITE_TYPE);
	}
	else if (fout->remoteVersion >= 80400)
	{
		/*
		 * Left join to pick up dependency info linking sequences to their
		 * owning column, if any (note this dependency is AUTO as of 8.2)
		 */
		appendPQExpBuffer(query,
						  "SELECT c.tableoid, c.oid, c.relname, "
						  "c.relacl, NULL as rrelacl, "
						  "NULL AS initrelacl, NULL AS initrrelacl, "
						  "c.relkind, "
						  "c.relnamespace, "
						  "(%s c.relowner) AS rolname, "
						  "c.relchecks, c.relhastriggers, "
						  "c.relhasindex, c.relhasrules, c.relhasoids, "
						  "'f'::bool AS relrowsecurity, "
						  "'f'::bool AS relforcerowsecurity, "
						  "c.relfrozenxid, 0 AS relminmxid, tc.oid AS toid, "
						  "tc.relfrozenxid AS tfrozenxid, "
						  "0 AS tminmxid, "
						  "'p' AS relpersistence, 't' as relispopulated, "
						  "'d' AS relreplident, c.relpages, "
						  "NULL AS amname, "
						  "NULL AS foreignserver, "
						  "NULL AS reloftype, "
						  "d.refobjid AS owning_tab, "
						  "d.refobjsubid AS owning_col, "
						  "(SELECT spcname FROM pg_tablespace t WHERE t.oid = c.reltablespace) AS reltablespace, "
						  "c.reloptions AS reloptions, "
						  "tc.reloptions AS toast_reloptions, "
						  "NULL AS changed_acl, "
						  "NULL AS partkeydef, "
						  "false AS ispartition, "
						  "NULL AS partbound "
						  "FROM pg_class c "
						  "LEFT JOIN pg_depend d ON "
						  "(c.relkind = '%c' AND "
						  "d.classid = c.tableoid AND d.objid = c.oid AND "
						  "d.objsubid = 0 AND "
						  "d.refclassid = c.tableoid AND d.deptype = 'a') "
						  "LEFT JOIN pg_class tc ON (c.reltoastrelid = tc.oid) "
						  "WHERE c.relkind in ('%c', '%c', '%c', '%c') "
						  "ORDER BY c.oid",
						  username_subquery,
						  RELKIND_SEQUENCE,
						  RELKIND_RELATION, RELKIND_SEQUENCE,
						  RELKIND_VIEW, RELKIND_COMPOSITE_TYPE);
	}
	else if (fout->remoteVersion >= 80200)
	{
		/*
		 * Left join to pick up dependency info linking sequences to their
		 * owning column, if any (note this dependency is AUTO as of 8.2)
		 */
		appendPQExpBuffer(query,
						  "SELECT c.tableoid, c.oid, c.relname, "
						  "c.relacl, NULL as rrelacl, "
						  "NULL AS initrelacl, NULL AS initrrelacl, "
						  "c.relkind, "
						  "c.relnamespace, "
						  "(%s c.relowner) AS rolname, "
						  "c.relchecks, (c.reltriggers <> 0) AS relhastriggers, "
						  "c.relhasindex, c.relhasrules, c.relhasoids, "
						  "'f'::bool AS relrowsecurity, "
						  "'f'::bool AS relforcerowsecurity, "
						  "c.relfrozenxid, 0 AS relminmxid, tc.oid AS toid, "
						  "tc.relfrozenxid AS tfrozenxid, "
						  "0 AS tminmxid, "
						  "'p' AS relpersistence, 't' as relispopulated, "
						  "'d' AS relreplident, c.relpages, "
						  "NULL AS amname, "
						  "NULL AS foreignserver, "
						  "NULL AS reloftype, "
						  "d.refobjid AS owning_tab, "
						  "d.refobjsubid AS owning_col, "
						  "(SELECT spcname FROM pg_tablespace t WHERE t.oid = c.reltablespace) AS reltablespace, "
						  "c.reloptions AS reloptions, "
						  "NULL AS toast_reloptions, "
						  "NULL AS changed_acl, "
						  "NULL AS partkeydef, "
						  "false AS ispartition, "
						  "NULL AS partbound "
						  "FROM pg_class c "
						  "LEFT JOIN pg_depend d ON "
						  "(c.relkind = '%c' AND "
						  "d.classid = c.tableoid AND d.objid = c.oid AND "
						  "d.objsubid = 0 AND "
						  "d.refclassid = c.tableoid AND d.deptype = 'a') "
						  "LEFT JOIN pg_class tc ON (c.reltoastrelid = tc.oid) "
						  "WHERE c.relkind in ('%c', '%c', '%c', '%c') "
						  "ORDER BY c.oid",
						  username_subquery,
						  RELKIND_SEQUENCE,
						  RELKIND_RELATION, RELKIND_SEQUENCE,
						  RELKIND_VIEW, RELKIND_COMPOSITE_TYPE);
	}
	else
	{
		/*
		 * Left join to pick up dependency info linking sequences to their
		 * owning column, if any
		 */
		appendPQExpBuffer(query,
						  "SELECT c.tableoid, c.oid, relname, "
						  "relacl, NULL as rrelacl, "
						  "NULL AS initrelacl, NULL AS initrrelacl, "
						  "relkind, relnamespace, "
						  "(%s relowner) AS rolname, "
						  "relchecks, (reltriggers <> 0) AS relhastriggers, "
						  "relhasindex, relhasrules, relhasoids, "
						  "'f'::bool AS relrowsecurity, "
						  "'f'::bool AS relforcerowsecurity, "
						  "0 AS relfrozenxid, 0 AS relminmxid,"
						  "0 AS toid, "
						  "0 AS tfrozenxid, 0 AS tminmxid,"
						  "'p' AS relpersistence, 't' as relispopulated, "
						  "'d' AS relreplident, relpages, "
						  "NULL AS amname, "
						  "NULL AS foreignserver, "
						  "NULL AS reloftype, "
						  "d.refobjid AS owning_tab, "
						  "d.refobjsubid AS owning_col, "
						  "(SELECT spcname FROM pg_tablespace t WHERE t.oid = c.reltablespace) AS reltablespace, "
						  "NULL AS reloptions, "
						  "NULL AS toast_reloptions, "
						  "NULL AS changed_acl, "
						  "NULL AS partkeydef, "
						  "false AS ispartition, "
						  "NULL AS partbound "
						  "FROM pg_class c "
						  "LEFT JOIN pg_depend d ON "
						  "(c.relkind = '%c' AND "
						  "d.classid = c.tableoid AND d.objid = c.oid AND "
						  "d.objsubid = 0 AND "
						  "d.refclassid = c.tableoid AND d.deptype = 'i') "
						  "WHERE relkind in ('%c', '%c', '%c', '%c') "
						  "ORDER BY c.oid",
						  username_subquery,
						  RELKIND_SEQUENCE,
						  RELKIND_RELATION, RELKIND_SEQUENCE,
						  RELKIND_VIEW, RELKIND_COMPOSITE_TYPE);
	}

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

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
	tblinfo = (TableInfo *) pg_malloc0(ntups * sizeof(TableInfo));

	i_reltableoid = PQfnumber(res, "tableoid");
	i_reloid = PQfnumber(res, "oid");
	i_relname = PQfnumber(res, "relname");
	i_relnamespace = PQfnumber(res, "relnamespace");
	i_relacl = PQfnumber(res, "relacl");
	i_rrelacl = PQfnumber(res, "rrelacl");
	i_initrelacl = PQfnumber(res, "initrelacl");
	i_initrrelacl = PQfnumber(res, "initrrelacl");
	i_relkind = PQfnumber(res, "relkind");
	i_rolname = PQfnumber(res, "rolname");
	i_relchecks = PQfnumber(res, "relchecks");
	i_relhastriggers = PQfnumber(res, "relhastriggers");
	i_relhasindex = PQfnumber(res, "relhasindex");
	i_relhasrules = PQfnumber(res, "relhasrules");
	i_relrowsec = PQfnumber(res, "relrowsecurity");
	i_relforcerowsec = PQfnumber(res, "relforcerowsecurity");
	i_relhasoids = PQfnumber(res, "relhasoids");
	i_relfrozenxid = PQfnumber(res, "relfrozenxid");
	i_relminmxid = PQfnumber(res, "relminmxid");
	i_toastoid = PQfnumber(res, "toid");
	i_toastfrozenxid = PQfnumber(res, "tfrozenxid");
	i_toastminmxid = PQfnumber(res, "tminmxid");
	i_relpersistence = PQfnumber(res, "relpersistence");
	i_relispopulated = PQfnumber(res, "relispopulated");
	i_relreplident = PQfnumber(res, "relreplident");
	i_relpages = PQfnumber(res, "relpages");
	i_foreignserver = PQfnumber(res, "foreignserver");
	i_owning_tab = PQfnumber(res, "owning_tab");
	i_owning_col = PQfnumber(res, "owning_col");
	i_reltablespace = PQfnumber(res, "reltablespace");
	i_reloptions = PQfnumber(res, "reloptions");
	i_checkoption = PQfnumber(res, "checkoption");
	i_toastreloptions = PQfnumber(res, "toast_reloptions");
	i_reloftype = PQfnumber(res, "reloftype");
	i_is_identity_sequence = PQfnumber(res, "is_identity_sequence");
	i_changed_acl = PQfnumber(res, "changed_acl");
	i_partkeydef = PQfnumber(res, "partkeydef");
	i_ispartition = PQfnumber(res, "ispartition");
	i_partbound = PQfnumber(res, "partbound");
	i_amname = PQfnumber(res, "amname");

	if (dopt->lockWaitTimeout)
	{
		/*
		 * Arrange to fail instead of waiting forever for a table lock.
		 *
		 * NB: this coding assumes that the only queries issued within the
		 * following loop are LOCK TABLEs; else the timeout may be undesirably
		 * applied to other things too.
		 */
		resetPQExpBuffer(query);
		appendPQExpBufferStr(query, "SET statement_timeout = ");
		appendStringLiteralConn(query, dopt->lockWaitTimeout, GetConnection(fout));
		ExecuteSqlStatement(fout, query->data);
	}

	for (i = 0; i < ntups; i++)
	{
		tblinfo[i].dobj.objType = DO_TABLE;
		tblinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_reltableoid));
		tblinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_reloid));
		AssignDumpId(&tblinfo[i].dobj);
		tblinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_relname));
		tblinfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_relnamespace)));
		tblinfo[i].rolname = pg_strdup(PQgetvalue(res, i, i_rolname));
		tblinfo[i].relacl = pg_strdup(PQgetvalue(res, i, i_relacl));
		tblinfo[i].rrelacl = pg_strdup(PQgetvalue(res, i, i_rrelacl));
		tblinfo[i].initrelacl = pg_strdup(PQgetvalue(res, i, i_initrelacl));
		tblinfo[i].initrrelacl = pg_strdup(PQgetvalue(res, i, i_initrrelacl));
		tblinfo[i].relkind = *(PQgetvalue(res, i, i_relkind));
		tblinfo[i].relpersistence = *(PQgetvalue(res, i, i_relpersistence));
		tblinfo[i].hasindex = (strcmp(PQgetvalue(res, i, i_relhasindex), "t") == 0);
		tblinfo[i].hasrules = (strcmp(PQgetvalue(res, i, i_relhasrules), "t") == 0);
		tblinfo[i].hastriggers = (strcmp(PQgetvalue(res, i, i_relhastriggers), "t") == 0);
		tblinfo[i].rowsec = (strcmp(PQgetvalue(res, i, i_relrowsec), "t") == 0);
		tblinfo[i].forcerowsec = (strcmp(PQgetvalue(res, i, i_relforcerowsec), "t") == 0);
		tblinfo[i].hasoids = (strcmp(PQgetvalue(res, i, i_relhasoids), "t") == 0);
		tblinfo[i].relispopulated = (strcmp(PQgetvalue(res, i, i_relispopulated), "t") == 0);
		tblinfo[i].relreplident = *(PQgetvalue(res, i, i_relreplident));
		tblinfo[i].relpages = atoi(PQgetvalue(res, i, i_relpages));
		tblinfo[i].frozenxid = atooid(PQgetvalue(res, i, i_relfrozenxid));
		tblinfo[i].minmxid = atooid(PQgetvalue(res, i, i_relminmxid));
		tblinfo[i].toast_oid = atooid(PQgetvalue(res, i, i_toastoid));
		tblinfo[i].toast_frozenxid = atooid(PQgetvalue(res, i, i_toastfrozenxid));
		tblinfo[i].toast_minmxid = atooid(PQgetvalue(res, i, i_toastminmxid));
		if (PQgetisnull(res, i, i_reloftype))
			tblinfo[i].reloftype = NULL;
		else
			tblinfo[i].reloftype = pg_strdup(PQgetvalue(res, i, i_reloftype));
		tblinfo[i].ncheck = atoi(PQgetvalue(res, i, i_relchecks));
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
		tblinfo[i].reltablespace = pg_strdup(PQgetvalue(res, i, i_reltablespace));
		tblinfo[i].reloptions = pg_strdup(PQgetvalue(res, i, i_reloptions));
		if (i_checkoption == -1 || PQgetisnull(res, i, i_checkoption))
			tblinfo[i].checkoption = NULL;
		else
			tblinfo[i].checkoption = pg_strdup(PQgetvalue(res, i, i_checkoption));
		tblinfo[i].toast_reloptions = pg_strdup(PQgetvalue(res, i, i_toastreloptions));
		if (PQgetisnull(res, i, i_amname))
			tblinfo[i].amname = NULL;
		else
			tblinfo[i].amname = pg_strdup(PQgetvalue(res, i, i_amname));

		/* other fields were zeroed above */

		/*
		 * Decide whether we want to dump this table.
		 */
		if (tblinfo[i].relkind == RELKIND_COMPOSITE_TYPE)
			tblinfo[i].dobj.dump = DUMP_COMPONENT_NONE;
		else
			selectDumpableTable(&tblinfo[i], fout);

		/*
		 * If the table-level and all column-level ACLs for this table are
		 * unchanged, then we don't need to worry about including the ACLs for
		 * this table.  If any column-level ACLs have been changed, the
		 * 'changed_acl' column from the query will indicate that.
		 *
		 * This can result in a significant performance improvement in cases
		 * where we are only looking to dump out the ACL (eg: pg_catalog).
		 */
		if (PQgetisnull(res, i, i_relacl) && PQgetisnull(res, i, i_rrelacl) &&
			PQgetisnull(res, i, i_initrelacl) &&
			PQgetisnull(res, i, i_initrrelacl) &&
			strcmp(PQgetvalue(res, i, i_changed_acl), "f") == 0)
			tblinfo[i].dobj.dump &= ~DUMP_COMPONENT_ACL;

		tblinfo[i].interesting = tblinfo[i].dobj.dump ? true : false;
		tblinfo[i].dummy_view = false;	/* might get set during sort */
		tblinfo[i].postponed_def = false;	/* might get set during sort */

		tblinfo[i].is_identity_sequence = (i_is_identity_sequence >= 0 &&
										   strcmp(PQgetvalue(res, i, i_is_identity_sequence), "t") == 0);

		/* Partition key string or NULL */
		tblinfo[i].partkeydef = pg_strdup(PQgetvalue(res, i, i_partkeydef));
		tblinfo[i].ispartition = (strcmp(PQgetvalue(res, i, i_ispartition), "t") == 0);
		tblinfo[i].partbound = pg_strdup(PQgetvalue(res, i, i_partbound));

		/* foreign server */
		tblinfo[i].foreign_server = atooid(PQgetvalue(res, i, i_foreignserver));

		/*
		 * Read-lock target tables to make sure they aren't DROPPED or altered
		 * in schema before we get around to dumping them.
		 *
		 * Note that we don't explicitly lock parents of the target tables; we
		 * assume our lock on the child is enough to prevent schema
		 * alterations to parent tables.
		 *
		 * NOTE: it'd be kinda nice to lock other relations too, not only
		 * plain or partitioned tables, but the backend doesn't presently
		 * allow that.
		 *
		 * We only need to lock the table for certain components; see
		 * pg_dump.h
		 */
		if (tblinfo[i].dobj.dump &&
			(tblinfo[i].relkind == RELKIND_RELATION ||
			 tblinfo->relkind == RELKIND_PARTITIONED_TABLE) &&
			(tblinfo[i].dobj.dump & DUMP_COMPONENTS_REQUIRING_LOCK))
		{
			resetPQExpBuffer(query);
			appendPQExpBuffer(query,
							  "LOCK TABLE %s IN ACCESS SHARE MODE",
							  fmtQualifiedDumpable(&tblinfo[i]));
			ExecuteSqlStatement(fout, query->data);
		}

		/* Emit notice if join for owner failed */
		if (strlen(tblinfo[i].rolname) == 0)
			pg_log_warning("owner of table \"%s\" appears to be invalid",
						   tblinfo[i].dobj.name);
	}

	if (dopt->lockWaitTimeout)
	{
		ExecuteSqlStatement(fout, "SET statement_timeout = 0");
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return tblinfo;
}

/*
 * getOwnedSeqs
 *	  identify owned sequences and mark them as dumpable if owning table is
 *
 * We used to do this in getTables(), but it's better to do it after the
 * index used by findTableByOid() has been set up.
 */
void
getOwnedSeqs(Archive *fout, TableInfo tblinfo[], int numTables)
{
	int			i;

	/*
	 * Force sequences that are "owned" by table columns to be dumped whenever
	 * their owning table is being dumped.
	 */
	for (i = 0; i < numTables; i++)
	{
		TableInfo  *seqinfo = &tblinfo[i];
		TableInfo  *owning_tab;

		if (!OidIsValid(seqinfo->owning_tab))
			continue;			/* not an owned sequence */

		owning_tab = findTableByOid(seqinfo->owning_tab);
		if (owning_tab == NULL)
			fatal("failed sanity check, parent table with OID %u of sequence with OID %u not found",
				  seqinfo->owning_tab, seqinfo->dobj.catId.oid);

		/*
		 * Only dump identity sequences if we're going to dump the table that
		 * it belongs to.
		 */
		if (owning_tab->dobj.dump == DUMP_COMPONENT_NONE &&
			seqinfo->is_identity_sequence)
		{
			seqinfo->dobj.dump = DUMP_COMPONENT_NONE;
			continue;
		}

		/*
		 * Otherwise we need to dump the components that are being dumped for
		 * the table and any components which the sequence is explicitly
		 * marked with.
		 *
		 * We can't simply use the set of components which are being dumped
		 * for the table as the table might be in an extension (and only the
		 * non-extension components, eg: ACLs if changed, security labels, and
		 * policies, are being dumped) while the sequence is not (and
		 * therefore the definition and other components should also be
		 * dumped).
		 *
		 * If the sequence is part of the extension then it should be properly
		 * marked by checkExtensionMembership() and this will be a no-op as
		 * the table will be equivalently marked.
		 */
		seqinfo->dobj.dump = seqinfo->dobj.dump | owning_tab->dobj.dump;

		if (seqinfo->dobj.dump != DUMP_COMPONENT_NONE)
			seqinfo->interesting = true;
	}
}

/*
 * getInherits
 *	  read all the inheritance information
 * from the system catalogs return them in the InhInfo* structure
 *
 * numInherits is set to the number of pairs read in
 */
InhInfo *
getInherits(Archive *fout, int *numInherits)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	InhInfo    *inhinfo;

	int			i_inhrelid;
	int			i_inhparent;

	/*
	 * Find all the inheritance information, excluding implicit inheritance
	 * via partitioning.
	 */
	appendPQExpBufferStr(query, "SELECT inhrelid, inhparent FROM pg_inherits");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	*numInherits = ntups;

	inhinfo = (InhInfo *) pg_malloc(ntups * sizeof(InhInfo));

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
getIndexes(Archive *fout, TableInfo tblinfo[], int numTables)
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
				i_parentidx,
				i_indexdef,
				i_indnkeyatts,
				i_indnatts,
				i_indkey,
				i_indisclustered,
				i_indisreplident,
				i_contype,
				i_conname,
				i_condeferrable,
				i_condeferred,
				i_contableoid,
				i_conoid,
				i_condef,
				i_tablespace,
				i_indreloptions,
				i_indstatcols,
				i_indstatvals,
				i_inddependcollnames,
				i_inddependcollversions;
	int			ntups;

	for (i = 0; i < numTables; i++)
	{
		TableInfo  *tbinfo = &tblinfo[i];

		if (!tbinfo->hasindex)
			continue;

		/*
		 * Ignore indexes of tables whose definitions are not to be dumped.
		 *
		 * We also need indexes on partitioned tables which have partitions to
		 * be dumped, in order to dump the indexes on the partitions.
		 */
		if (!(tbinfo->dobj.dump & DUMP_COMPONENT_DEFINITION) &&
			!tbinfo->interesting)
			continue;

		pg_log_info("reading indexes for table \"%s.%s\"",
					tbinfo->dobj.namespace->dobj.name,
					tbinfo->dobj.name);

		/*
		 * The point of the messy-looking outer join is to find a constraint
		 * that is related by an internal dependency link to the index. If we
		 * find one, create a CONSTRAINT entry linked to the INDEX entry.  We
		 * assume an index won't have more than one internal dependency.
		 *
		 * As of 9.0 we don't need to look at pg_depend but can check for a
		 * match to pg_constraint.conindid.  The check on conrelid is
		 * redundant but useful because that column is indexed while conindid
		 * is not.
		 */
		resetPQExpBuffer(query);
		if (fout->remoteVersion >= 140000)
		{
			appendPQExpBuffer(query,
							  "SELECT t.tableoid, t.oid, "
							  "t.relname AS indexname, "
							  "inh.inhparent AS parentidx, "
							  "pg_catalog.pg_get_indexdef(i.indexrelid) AS indexdef, "
							  "i.indnkeyatts AS indnkeyatts, "
							  "i.indnatts AS indnatts, "
							  "i.indkey, i.indisclustered, "
							  "i.indisreplident, "
							  "c.contype, c.conname, "
							  "c.condeferrable, c.condeferred, "
							  "c.tableoid AS contableoid, "
							  "c.oid AS conoid, "
							  "pg_catalog.pg_get_constraintdef(c.oid, false) AS condef, "
							  "(SELECT spcname FROM pg_catalog.pg_tablespace s WHERE s.oid = t.reltablespace) AS tablespace, "
							  "t.reloptions AS indreloptions, "
							  "(SELECT pg_catalog.array_agg(attnum ORDER BY attnum) "
							  "  FROM pg_catalog.pg_attribute "
							  "  WHERE attrelid = i.indexrelid AND "
							  "    attstattarget >= 0) AS indstatcols,"
							  "(SELECT pg_catalog.array_agg(attstattarget ORDER BY attnum) "
							  "  FROM pg_catalog.pg_attribute "
							  "  WHERE attrelid = i.indexrelid AND "
							  "    attstattarget >= 0) AS indstatvals, "
							  "(SELECT pg_catalog.array_agg(quote_ident(ns.nspname) || '.' || quote_ident(c.collname) ORDER BY refobjid) "
							  "  FROM pg_catalog.pg_depend d "
							  "  JOIN pg_catalog.pg_collation c ON (c.oid = d.refobjid) "
							  "  JOIN pg_catalog.pg_namespace ns ON (c.collnamespace = ns.oid) "
							  "  WHERE d.classid = 'pg_catalog.pg_class'::regclass AND "
							  "    d.objid = i.indexrelid AND "
							  "    d.objsubid = 0 AND "
							  "    d.refclassid = 'pg_catalog.pg_collation'::regclass AND "
							  "    d.refobjversion IS NOT NULL) AS inddependcollnames, "
							  "(SELECT pg_catalog.array_agg(quote_literal(refobjversion) ORDER BY refobjid) "
							  "  FROM pg_catalog.pg_depend "
							  "  WHERE classid = 'pg_catalog.pg_class'::regclass AND "
							  "    objid = i.indexrelid AND "
							  "    objsubid = 0 AND "
							  "    refclassid = 'pg_catalog.pg_collation'::regclass AND "
							  "    refobjversion IS NOT NULL) AS inddependcollversions "
							  "FROM pg_catalog.pg_index i "
							  "JOIN pg_catalog.pg_class t ON (t.oid = i.indexrelid) "
							  "JOIN pg_catalog.pg_class t2 ON (t2.oid = i.indrelid) "
							  "LEFT JOIN pg_catalog.pg_constraint c "
							  "ON (i.indrelid = c.conrelid AND "
							  "i.indexrelid = c.conindid AND "
							  "c.contype IN ('p','u','x')) "
							  "LEFT JOIN pg_catalog.pg_inherits inh "
							  "ON (inh.inhrelid = indexrelid) "
							  "WHERE i.indrelid = '%u'::pg_catalog.oid "
							  "AND (i.indisvalid OR t2.relkind = 'p') "
							  "AND i.indisready "
							  "ORDER BY indexname",
							  tbinfo->dobj.catId.oid);
		}
		else if (fout->remoteVersion >= 110000)
		{
			appendPQExpBuffer(query,
							  "SELECT t.tableoid, t.oid, "
							  "t.relname AS indexname, "
							  "inh.inhparent AS parentidx, "
							  "pg_catalog.pg_get_indexdef(i.indexrelid) AS indexdef, "
							  "i.indnkeyatts AS indnkeyatts, "
							  "i.indnatts AS indnatts, "
							  "i.indkey, i.indisclustered, "
							  "i.indisreplident, "
							  "c.contype, c.conname, "
							  "c.condeferrable, c.condeferred, "
							  "c.tableoid AS contableoid, "
							  "c.oid AS conoid, "
							  "pg_catalog.pg_get_constraintdef(c.oid, false) AS condef, "
							  "(SELECT spcname FROM pg_catalog.pg_tablespace s WHERE s.oid = t.reltablespace) AS tablespace, "
							  "t.reloptions AS indreloptions, "
							  "(SELECT pg_catalog.array_agg(attnum ORDER BY attnum) "
							  "  FROM pg_catalog.pg_attribute "
							  "  WHERE attrelid = i.indexrelid AND "
							  "    attstattarget >= 0) AS indstatcols,"
							  "(SELECT pg_catalog.array_agg(attstattarget ORDER BY attnum) "
							  "  FROM pg_catalog.pg_attribute "
							  "  WHERE attrelid = i.indexrelid AND "
							  "    attstattarget >= 0) AS indstatvals, "
							  "'{}' AS inddependcollnames, "
							  "'{}' AS inddependcollversions "
							  "FROM pg_catalog.pg_index i "
							  "JOIN pg_catalog.pg_class t ON (t.oid = i.indexrelid) "
							  "JOIN pg_catalog.pg_class t2 ON (t2.oid = i.indrelid) "
							  "LEFT JOIN pg_catalog.pg_constraint c "
							  "ON (i.indrelid = c.conrelid AND "
							  "i.indexrelid = c.conindid AND "
							  "c.contype IN ('p','u','x')) "
							  "LEFT JOIN pg_catalog.pg_inherits inh "
							  "ON (inh.inhrelid = indexrelid) "
							  "WHERE i.indrelid = '%u'::pg_catalog.oid "
							  "AND (i.indisvalid OR t2.relkind = 'p') "
							  "AND i.indisready "
							  "ORDER BY indexname",
							  tbinfo->dobj.catId.oid);
		}
		else if (fout->remoteVersion >= 90400)
		{
			/*
			 * the test on indisready is necessary in 9.2, and harmless in
			 * earlier/later versions
			 */
			appendPQExpBuffer(query,
							  "SELECT t.tableoid, t.oid, "
							  "t.relname AS indexname, "
							  "0 AS parentidx, "
							  "pg_catalog.pg_get_indexdef(i.indexrelid) AS indexdef, "
							  "i.indnatts AS indnkeyatts, "
							  "i.indnatts AS indnatts, "
							  "i.indkey, i.indisclustered, "
							  "i.indisreplident, "
							  "c.contype, c.conname, "
							  "c.condeferrable, c.condeferred, "
							  "c.tableoid AS contableoid, "
							  "c.oid AS conoid, "
							  "pg_catalog.pg_get_constraintdef(c.oid, false) AS condef, "
							  "(SELECT spcname FROM pg_catalog.pg_tablespace s WHERE s.oid = t.reltablespace) AS tablespace, "
							  "t.reloptions AS indreloptions, "
							  "'' AS indstatcols, "
							  "'' AS indstatvals, "
							  "'{}' AS inddependcollnames, "
							  "'{}' AS inddependcollversions "
							  "FROM pg_catalog.pg_index i "
							  "JOIN pg_catalog.pg_class t ON (t.oid = i.indexrelid) "
							  "LEFT JOIN pg_catalog.pg_constraint c "
							  "ON (i.indrelid = c.conrelid AND "
							  "i.indexrelid = c.conindid AND "
							  "c.contype IN ('p','u','x')) "
							  "WHERE i.indrelid = '%u'::pg_catalog.oid "
							  "AND i.indisvalid AND i.indisready "
							  "ORDER BY indexname",
							  tbinfo->dobj.catId.oid);
		}
		else if (fout->remoteVersion >= 90000)
		{
			/*
			 * the test on indisready is necessary in 9.2, and harmless in
			 * earlier/later versions
			 */
			appendPQExpBuffer(query,
							  "SELECT t.tableoid, t.oid, "
							  "t.relname AS indexname, "
							  "0 AS parentidx, "
							  "pg_catalog.pg_get_indexdef(i.indexrelid) AS indexdef, "
							  "i.indnatts AS indnkeyatts, "
							  "i.indnatts AS indnatts, "
							  "i.indkey, i.indisclustered, "
							  "false AS indisreplident, "
							  "c.contype, c.conname, "
							  "c.condeferrable, c.condeferred, "
							  "c.tableoid AS contableoid, "
							  "c.oid AS conoid, "
							  "pg_catalog.pg_get_constraintdef(c.oid, false) AS condef, "
							  "(SELECT spcname FROM pg_catalog.pg_tablespace s WHERE s.oid = t.reltablespace) AS tablespace, "
							  "t.reloptions AS indreloptions, "
							  "'' AS indstatcols, "
							  "'' AS indstatvals, "
							  "'{}' AS inddependcollnames, "
							  "'{}' AS inddependcollversions "
							  "FROM pg_catalog.pg_index i "
							  "JOIN pg_catalog.pg_class t ON (t.oid = i.indexrelid) "
							  "LEFT JOIN pg_catalog.pg_constraint c "
							  "ON (i.indrelid = c.conrelid AND "
							  "i.indexrelid = c.conindid AND "
							  "c.contype IN ('p','u','x')) "
							  "WHERE i.indrelid = '%u'::pg_catalog.oid "
							  "AND i.indisvalid AND i.indisready "
							  "ORDER BY indexname",
							  tbinfo->dobj.catId.oid);
		}
		else if (fout->remoteVersion >= 80200)
		{
			appendPQExpBuffer(query,
							  "SELECT t.tableoid, t.oid, "
							  "t.relname AS indexname, "
							  "0 AS parentidx, "
							  "pg_catalog.pg_get_indexdef(i.indexrelid) AS indexdef, "
							  "i.indnatts AS indnkeyatts, "
							  "i.indnatts AS indnatts, "
							  "i.indkey, i.indisclustered, "
							  "false AS indisreplident, "
							  "c.contype, c.conname, "
							  "c.condeferrable, c.condeferred, "
							  "c.tableoid AS contableoid, "
							  "c.oid AS conoid, "
							  "null AS condef, "
							  "(SELECT spcname FROM pg_catalog.pg_tablespace s WHERE s.oid = t.reltablespace) AS tablespace, "
							  "t.reloptions AS indreloptions, "
							  "'' AS indstatcols, "
							  "'' AS indstatvals, "
							  "'{}' AS inddependcollnames, "
							  "'{}' AS inddependcollversions "
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
							  "AND i.indisvalid "
							  "ORDER BY indexname",
							  tbinfo->dobj.catId.oid);
		}
		else
		{
			appendPQExpBuffer(query,
							  "SELECT t.tableoid, t.oid, "
							  "t.relname AS indexname, "
							  "0 AS parentidx, "
							  "pg_catalog.pg_get_indexdef(i.indexrelid) AS indexdef, "
							  "t.relnatts AS indnkeyatts, "
							  "t.relnatts AS indnatts, "
							  "i.indkey, i.indisclustered, "
							  "false AS indisreplident, "
							  "c.contype, c.conname, "
							  "c.condeferrable, c.condeferred, "
							  "c.tableoid AS contableoid, "
							  "c.oid AS conoid, "
							  "null AS condef, "
							  "(SELECT spcname FROM pg_catalog.pg_tablespace s WHERE s.oid = t.reltablespace) AS tablespace, "
							  "null AS indreloptions, "
							  "'' AS indstatcols, "
							  "'' AS indstatvals, "
							  "'{}' AS inddependcollnames, "
							  "'{}' AS inddependcollversions "
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

		res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

		ntups = PQntuples(res);

		i_tableoid = PQfnumber(res, "tableoid");
		i_oid = PQfnumber(res, "oid");
		i_indexname = PQfnumber(res, "indexname");
		i_parentidx = PQfnumber(res, "parentidx");
		i_indexdef = PQfnumber(res, "indexdef");
		i_indnkeyatts = PQfnumber(res, "indnkeyatts");
		i_indnatts = PQfnumber(res, "indnatts");
		i_indkey = PQfnumber(res, "indkey");
		i_indisclustered = PQfnumber(res, "indisclustered");
		i_indisreplident = PQfnumber(res, "indisreplident");
		i_contype = PQfnumber(res, "contype");
		i_conname = PQfnumber(res, "conname");
		i_condeferrable = PQfnumber(res, "condeferrable");
		i_condeferred = PQfnumber(res, "condeferred");
		i_contableoid = PQfnumber(res, "contableoid");
		i_conoid = PQfnumber(res, "conoid");
		i_condef = PQfnumber(res, "condef");
		i_tablespace = PQfnumber(res, "tablespace");
		i_indreloptions = PQfnumber(res, "indreloptions");
		i_indstatcols = PQfnumber(res, "indstatcols");
		i_indstatvals = PQfnumber(res, "indstatvals");
		i_inddependcollnames = PQfnumber(res, "inddependcollnames");
		i_inddependcollversions = PQfnumber(res, "inddependcollversions");

		tbinfo->indexes = indxinfo =
			(IndxInfo *) pg_malloc(ntups * sizeof(IndxInfo));
		constrinfo = (ConstraintInfo *) pg_malloc(ntups * sizeof(ConstraintInfo));
		tbinfo->numIndexes = ntups;

		for (j = 0; j < ntups; j++)
		{
			char		contype;

			indxinfo[j].dobj.objType = DO_INDEX;
			indxinfo[j].dobj.catId.tableoid = atooid(PQgetvalue(res, j, i_tableoid));
			indxinfo[j].dobj.catId.oid = atooid(PQgetvalue(res, j, i_oid));
			AssignDumpId(&indxinfo[j].dobj);
			indxinfo[j].dobj.dump = tbinfo->dobj.dump;
			indxinfo[j].dobj.name = pg_strdup(PQgetvalue(res, j, i_indexname));
			indxinfo[j].dobj.namespace = tbinfo->dobj.namespace;
			indxinfo[j].indextable = tbinfo;
			indxinfo[j].indexdef = pg_strdup(PQgetvalue(res, j, i_indexdef));
			indxinfo[j].indnkeyattrs = atoi(PQgetvalue(res, j, i_indnkeyatts));
			indxinfo[j].indnattrs = atoi(PQgetvalue(res, j, i_indnatts));
			indxinfo[j].tablespace = pg_strdup(PQgetvalue(res, j, i_tablespace));
			indxinfo[j].indreloptions = pg_strdup(PQgetvalue(res, j, i_indreloptions));
			indxinfo[j].indstatcols = pg_strdup(PQgetvalue(res, j, i_indstatcols));
			indxinfo[j].indstatvals = pg_strdup(PQgetvalue(res, j, i_indstatvals));
			indxinfo[j].inddependcollnames = pg_strdup(PQgetvalue(res, j, i_inddependcollnames));
			indxinfo[j].inddependcollversions = pg_strdup(PQgetvalue(res, j, i_inddependcollversions));
			indxinfo[j].indkeys = (Oid *) pg_malloc(indxinfo[j].indnattrs * sizeof(Oid));
			parseOidArray(PQgetvalue(res, j, i_indkey),
						  indxinfo[j].indkeys, indxinfo[j].indnattrs);
			indxinfo[j].indisclustered = (PQgetvalue(res, j, i_indisclustered)[0] == 't');
			indxinfo[j].indisreplident = (PQgetvalue(res, j, i_indisreplident)[0] == 't');
			indxinfo[j].parentidx = atooid(PQgetvalue(res, j, i_parentidx));
			indxinfo[j].partattaches = (SimplePtrList)
			{
				NULL, NULL
			};
			contype = *(PQgetvalue(res, j, i_contype));

			if (contype == 'p' || contype == 'u' || contype == 'x')
			{
				/*
				 * If we found a constraint matching the index, create an
				 * entry for it.
				 */
				constrinfo[j].dobj.objType = DO_CONSTRAINT;
				constrinfo[j].dobj.catId.tableoid = atooid(PQgetvalue(res, j, i_contableoid));
				constrinfo[j].dobj.catId.oid = atooid(PQgetvalue(res, j, i_conoid));
				AssignDumpId(&constrinfo[j].dobj);
				constrinfo[j].dobj.dump = tbinfo->dobj.dump;
				constrinfo[j].dobj.name = pg_strdup(PQgetvalue(res, j, i_conname));
				constrinfo[j].dobj.namespace = tbinfo->dobj.namespace;
				constrinfo[j].contable = tbinfo;
				constrinfo[j].condomain = NULL;
				constrinfo[j].contype = contype;
				if (contype == 'x')
					constrinfo[j].condef = pg_strdup(PQgetvalue(res, j, i_condef));
				else
					constrinfo[j].condef = NULL;
				constrinfo[j].confrelid = InvalidOid;
				constrinfo[j].conindex = indxinfo[j].dobj.dumpId;
				constrinfo[j].condeferrable = *(PQgetvalue(res, j, i_condeferrable)) == 't';
				constrinfo[j].condeferred = *(PQgetvalue(res, j, i_condeferred)) == 't';
				constrinfo[j].conislocal = true;
				constrinfo[j].separate = true;

				indxinfo[j].indexconstraint = constrinfo[j].dobj.dumpId;
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
 * getExtendedStatistics
 *	  get information about extended-statistics objects.
 *
 * Note: extended statistics data is not returned directly to the caller, but
 * it does get entered into the DumpableObject tables.
 */
void
getExtendedStatistics(Archive *fout)
{
	PQExpBuffer query;
	PGresult   *res;
	StatsExtInfo *statsextinfo;
	int			ntups;
	int			i_tableoid;
	int			i_oid;
	int			i_stxname;
	int			i_stxnamespace;
	int			i_rolname;
	int			i_stattarget;
	int			i;

	/* Extended statistics were new in v10 */
	if (fout->remoteVersion < 100000)
		return;

	query = createPQExpBuffer();

	if (fout->remoteVersion < 130000)
		appendPQExpBuffer(query, "SELECT tableoid, oid, stxname, "
						  "stxnamespace, (%s stxowner) AS rolname, (-1) AS stxstattarget "
						  "FROM pg_catalog.pg_statistic_ext",
						  username_subquery);
	else
		appendPQExpBuffer(query, "SELECT tableoid, oid, stxname, "
						  "stxnamespace, (%s stxowner) AS rolname, stxstattarget "
						  "FROM pg_catalog.pg_statistic_ext",
						  username_subquery);

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_stxname = PQfnumber(res, "stxname");
	i_stxnamespace = PQfnumber(res, "stxnamespace");
	i_rolname = PQfnumber(res, "rolname");
	i_stattarget = PQfnumber(res, "stxstattarget");

	statsextinfo = (StatsExtInfo *) pg_malloc(ntups * sizeof(StatsExtInfo));

	for (i = 0; i < ntups; i++)
	{
		statsextinfo[i].dobj.objType = DO_STATSEXT;
		statsextinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		statsextinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&statsextinfo[i].dobj);
		statsextinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_stxname));
		statsextinfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_stxnamespace)));
		statsextinfo[i].rolname = pg_strdup(PQgetvalue(res, i, i_rolname));
		statsextinfo[i].stattarget = atoi(PQgetvalue(res, i, i_stattarget));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(statsextinfo[i].dobj), fout);

		/* Stats objects do not currently have ACLs. */
		statsextinfo[i].dobj.dump &= ~DUMP_COMPONENT_ACL;
	}

	PQclear(res);
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
getConstraints(Archive *fout, TableInfo tblinfo[], int numTables)
{
	int			i,
				j;
	ConstraintInfo *constrinfo;
	PQExpBuffer query;
	PGresult   *res;
	int			i_contableoid,
				i_conoid,
				i_conname,
				i_confrelid,
				i_conindid,
				i_condef;
	int			ntups;

	query = createPQExpBuffer();

	for (i = 0; i < numTables; i++)
	{
		TableInfo  *tbinfo = &tblinfo[i];

		/*
		 * For partitioned tables, foreign keys have no triggers so they must
		 * be included anyway in case some foreign keys are defined.
		 */
		if ((!tbinfo->hastriggers &&
			 tbinfo->relkind != RELKIND_PARTITIONED_TABLE) ||
			!(tbinfo->dobj.dump & DUMP_COMPONENT_DEFINITION))
			continue;

		pg_log_info("reading foreign key constraints for table \"%s.%s\"",
					tbinfo->dobj.namespace->dobj.name,
					tbinfo->dobj.name);

		resetPQExpBuffer(query);
		if (fout->remoteVersion >= 110000)
			appendPQExpBuffer(query,
							  "SELECT tableoid, oid, conname, confrelid, conindid, "
							  "pg_catalog.pg_get_constraintdef(oid) AS condef "
							  "FROM pg_catalog.pg_constraint "
							  "WHERE conrelid = '%u'::pg_catalog.oid "
							  "AND conparentid = 0 "
							  "AND contype = 'f'",
							  tbinfo->dobj.catId.oid);
		else
			appendPQExpBuffer(query,
							  "SELECT tableoid, oid, conname, confrelid, 0 as conindid, "
							  "pg_catalog.pg_get_constraintdef(oid) AS condef "
							  "FROM pg_catalog.pg_constraint "
							  "WHERE conrelid = '%u'::pg_catalog.oid "
							  "AND contype = 'f'",
							  tbinfo->dobj.catId.oid);
		res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

		ntups = PQntuples(res);

		i_contableoid = PQfnumber(res, "tableoid");
		i_conoid = PQfnumber(res, "oid");
		i_conname = PQfnumber(res, "conname");
		i_confrelid = PQfnumber(res, "confrelid");
		i_conindid = PQfnumber(res, "conindid");
		i_condef = PQfnumber(res, "condef");

		constrinfo = (ConstraintInfo *) pg_malloc(ntups * sizeof(ConstraintInfo));

		for (j = 0; j < ntups; j++)
		{
			TableInfo  *reftable;

			constrinfo[j].dobj.objType = DO_FK_CONSTRAINT;
			constrinfo[j].dobj.catId.tableoid = atooid(PQgetvalue(res, j, i_contableoid));
			constrinfo[j].dobj.catId.oid = atooid(PQgetvalue(res, j, i_conoid));
			AssignDumpId(&constrinfo[j].dobj);
			constrinfo[j].dobj.name = pg_strdup(PQgetvalue(res, j, i_conname));
			constrinfo[j].dobj.namespace = tbinfo->dobj.namespace;
			constrinfo[j].contable = tbinfo;
			constrinfo[j].condomain = NULL;
			constrinfo[j].contype = 'f';
			constrinfo[j].condef = pg_strdup(PQgetvalue(res, j, i_condef));
			constrinfo[j].confrelid = atooid(PQgetvalue(res, j, i_confrelid));
			constrinfo[j].conindex = 0;
			constrinfo[j].condeferrable = false;
			constrinfo[j].condeferred = false;
			constrinfo[j].conislocal = true;
			constrinfo[j].separate = true;

			/*
			 * Restoring an FK that points to a partitioned table requires
			 * that all partition indexes have been attached beforehand.
			 * Ensure that happens by making the constraint depend on each
			 * index partition attach object.
			 */
			reftable = findTableByOid(constrinfo[j].confrelid);
			if (reftable && reftable->relkind == RELKIND_PARTITIONED_TABLE)
			{
				Oid			indexOid = atooid(PQgetvalue(res, j, i_conindid));

				if (indexOid != InvalidOid)
				{
					for (int k = 0; k < reftable->numIndexes; k++)
					{
						IndxInfo   *refidx;

						/* not our index? */
						if (reftable->indexes[k].dobj.catId.oid != indexOid)
							continue;

						refidx = &reftable->indexes[k];
						addConstrChildIdxDeps(&constrinfo[j].dobj, refidx);
						break;
					}
				}
			}
		}

		PQclear(res);
	}

	destroyPQExpBuffer(query);
}

/*
 * addConstrChildIdxDeps
 *
 * Recursive subroutine for getConstraints
 *
 * Given an object representing a foreign key constraint and an index on the
 * partitioned table it references, mark the constraint object as dependent
 * on the DO_INDEX_ATTACH object of each index partition, recursively
 * drilling down to their partitions if any.  This ensures that the FK is not
 * restored until the index is fully marked valid.
 */
static void
addConstrChildIdxDeps(DumpableObject *dobj, IndxInfo *refidx)
{
	SimplePtrListCell *cell;

	Assert(dobj->objType == DO_FK_CONSTRAINT);

	for (cell = refidx->partattaches.head; cell; cell = cell->next)
	{
		IndexAttachInfo *attach = (IndexAttachInfo *) cell->ptr;

		addObjectDependency(dobj, attach->dobj.dumpId);

		if (attach->partitionIdx->partattaches.head != NULL)
			addConstrChildIdxDeps(dobj, attach->partitionIdx);
	}
}

/*
 * getDomainConstraints
 *
 * Get info about constraints on a domain.
 */
static void
getDomainConstraints(Archive *fout, TypeInfo *tyinfo)
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

	query = createPQExpBuffer();

	if (fout->remoteVersion >= 90100)
		appendPQExpBuffer(query, "SELECT tableoid, oid, conname, "
						  "pg_catalog.pg_get_constraintdef(oid) AS consrc, "
						  "convalidated "
						  "FROM pg_catalog.pg_constraint "
						  "WHERE contypid = '%u'::pg_catalog.oid "
						  "ORDER BY conname",
						  tyinfo->dobj.catId.oid);

	else
		appendPQExpBuffer(query, "SELECT tableoid, oid, conname, "
						  "pg_catalog.pg_get_constraintdef(oid) AS consrc, "
						  "true as convalidated "
						  "FROM pg_catalog.pg_constraint "
						  "WHERE contypid = '%u'::pg_catalog.oid "
						  "ORDER BY conname",
						  tyinfo->dobj.catId.oid);

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_conname = PQfnumber(res, "conname");
	i_consrc = PQfnumber(res, "consrc");

	constrinfo = (ConstraintInfo *) pg_malloc(ntups * sizeof(ConstraintInfo));

	tyinfo->nDomChecks = ntups;
	tyinfo->domChecks = constrinfo;

	for (i = 0; i < ntups; i++)
	{
		bool		validated = PQgetvalue(res, i, 4)[0] == 't';

		constrinfo[i].dobj.objType = DO_CONSTRAINT;
		constrinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		constrinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&constrinfo[i].dobj);
		constrinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_conname));
		constrinfo[i].dobj.namespace = tyinfo->dobj.namespace;
		constrinfo[i].contable = NULL;
		constrinfo[i].condomain = tyinfo;
		constrinfo[i].contype = 'c';
		constrinfo[i].condef = pg_strdup(PQgetvalue(res, i, i_consrc));
		constrinfo[i].confrelid = InvalidOid;
		constrinfo[i].conindex = 0;
		constrinfo[i].condeferrable = false;
		constrinfo[i].condeferred = false;
		constrinfo[i].conislocal = true;

		constrinfo[i].separate = !validated;

		/*
		 * Make the domain depend on the constraint, ensuring it won't be
		 * output till any constraint dependencies are OK.  If the constraint
		 * has not been validated, it's going to be dumped after the domain
		 * anyway, so this doesn't matter.
		 */
		if (validated)
			addObjectDependency(&tyinfo->dobj,
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
getRules(Archive *fout, int *numRules)
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
	int			i_ev_enabled;

	if (fout->remoteVersion >= 80300)
	{
		appendPQExpBufferStr(query, "SELECT "
							 "tableoid, oid, rulename, "
							 "ev_class AS ruletable, ev_type, is_instead, "
							 "ev_enabled "
							 "FROM pg_rewrite "
							 "ORDER BY oid");
	}
	else
	{
		appendPQExpBufferStr(query, "SELECT "
							 "tableoid, oid, rulename, "
							 "ev_class AS ruletable, ev_type, is_instead, "
							 "'O'::char AS ev_enabled "
							 "FROM pg_rewrite "
							 "ORDER BY oid");
	}

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	*numRules = ntups;

	ruleinfo = (RuleInfo *) pg_malloc(ntups * sizeof(RuleInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_rulename = PQfnumber(res, "rulename");
	i_ruletable = PQfnumber(res, "ruletable");
	i_ev_type = PQfnumber(res, "ev_type");
	i_is_instead = PQfnumber(res, "is_instead");
	i_ev_enabled = PQfnumber(res, "ev_enabled");

	for (i = 0; i < ntups; i++)
	{
		Oid			ruletableoid;

		ruleinfo[i].dobj.objType = DO_RULE;
		ruleinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		ruleinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&ruleinfo[i].dobj);
		ruleinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_rulename));
		ruletableoid = atooid(PQgetvalue(res, i, i_ruletable));
		ruleinfo[i].ruletable = findTableByOid(ruletableoid);
		if (ruleinfo[i].ruletable == NULL)
			fatal("failed sanity check, parent table with OID %u of pg_rewrite entry with OID %u not found",
				  ruletableoid, ruleinfo[i].dobj.catId.oid);
		ruleinfo[i].dobj.namespace = ruleinfo[i].ruletable->dobj.namespace;
		ruleinfo[i].dobj.dump = ruleinfo[i].ruletable->dobj.dump;
		ruleinfo[i].ev_type = *(PQgetvalue(res, i, i_ev_type));
		ruleinfo[i].is_instead = *(PQgetvalue(res, i, i_is_instead)) == 't';
		ruleinfo[i].ev_enabled = *(PQgetvalue(res, i, i_ev_enabled));
		if (ruleinfo[i].ruletable)
		{
			/*
			 * If the table is a view or materialized view, force its ON
			 * SELECT rule to be sorted before the view itself --- this
			 * ensures that any dependencies for the rule affect the table's
			 * positioning. Other rules are forced to appear after their
			 * table.
			 */
			if ((ruleinfo[i].ruletable->relkind == RELKIND_VIEW ||
				 ruleinfo[i].ruletable->relkind == RELKIND_MATVIEW) &&
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
getTriggers(Archive *fout, TableInfo tblinfo[], int numTables)
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
				i_tginitdeferred,
				i_tgdef;
	int			ntups;

	for (i = 0; i < numTables; i++)
	{
		TableInfo  *tbinfo = &tblinfo[i];

		if (!tbinfo->hastriggers ||
			!(tbinfo->dobj.dump & DUMP_COMPONENT_DEFINITION))
			continue;

		pg_log_info("reading triggers for table \"%s.%s\"",
					tbinfo->dobj.namespace->dobj.name,
					tbinfo->dobj.name);

		resetPQExpBuffer(query);
		if (fout->remoteVersion >= 90000)
		{
			/*
			 * NB: think not to use pretty=true in pg_get_triggerdef.  It
			 * could result in non-forward-compatible dumps of WHEN clauses
			 * due to under-parenthesization.
			 */
			appendPQExpBuffer(query,
							  "SELECT tgname, "
							  "tgfoid::pg_catalog.regproc AS tgfname, "
							  "pg_catalog.pg_get_triggerdef(oid, false) AS tgdef, "
							  "tgenabled, tableoid, oid "
							  "FROM pg_catalog.pg_trigger t "
							  "WHERE tgrelid = '%u'::pg_catalog.oid "
							  "AND NOT tgisinternal",
							  tbinfo->dobj.catId.oid);
		}
		else if (fout->remoteVersion >= 80300)
		{
			/*
			 * We ignore triggers that are tied to a foreign-key constraint
			 */
			appendPQExpBuffer(query,
							  "SELECT tgname, "
							  "tgfoid::pg_catalog.regproc AS tgfname, "
							  "tgtype, tgnargs, tgargs, tgenabled, "
							  "tgisconstraint, tgconstrname, tgdeferrable, "
							  "tgconstrrelid, tginitdeferred, tableoid, oid, "
							  "tgconstrrelid::pg_catalog.regclass AS tgconstrrelname "
							  "FROM pg_catalog.pg_trigger t "
							  "WHERE tgrelid = '%u'::pg_catalog.oid "
							  "AND tgconstraint = 0",
							  tbinfo->dobj.catId.oid);
		}
		else
		{
			/*
			 * We ignore triggers that are tied to a foreign-key constraint,
			 * but in these versions we have to grovel through pg_constraint
			 * to find out
			 */
			appendPQExpBuffer(query,
							  "SELECT tgname, "
							  "tgfoid::pg_catalog.regproc AS tgfname, "
							  "tgtype, tgnargs, tgargs, tgenabled, "
							  "tgisconstraint, tgconstrname, tgdeferrable, "
							  "tgconstrrelid, tginitdeferred, tableoid, oid, "
							  "tgconstrrelid::pg_catalog.regclass AS tgconstrrelname "
							  "FROM pg_catalog.pg_trigger t "
							  "WHERE tgrelid = '%u'::pg_catalog.oid "
							  "AND (NOT tgisconstraint "
							  " OR NOT EXISTS"
							  "  (SELECT 1 FROM pg_catalog.pg_depend d "
							  "   JOIN pg_catalog.pg_constraint c ON (d.refclassid = c.tableoid AND d.refobjid = c.oid) "
							  "   WHERE d.classid = t.tableoid AND d.objid = t.oid AND d.deptype = 'i' AND c.contype = 'f'))",
							  tbinfo->dobj.catId.oid);
		}

		res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

		ntups = PQntuples(res);

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
		i_tgdef = PQfnumber(res, "tgdef");

		tginfo = (TriggerInfo *) pg_malloc(ntups * sizeof(TriggerInfo));

		tbinfo->numTriggers = ntups;
		tbinfo->triggers = tginfo;

		for (j = 0; j < ntups; j++)
		{
			tginfo[j].dobj.objType = DO_TRIGGER;
			tginfo[j].dobj.catId.tableoid = atooid(PQgetvalue(res, j, i_tableoid));
			tginfo[j].dobj.catId.oid = atooid(PQgetvalue(res, j, i_oid));
			AssignDumpId(&tginfo[j].dobj);
			tginfo[j].dobj.name = pg_strdup(PQgetvalue(res, j, i_tgname));
			tginfo[j].dobj.namespace = tbinfo->dobj.namespace;
			tginfo[j].tgtable = tbinfo;
			tginfo[j].tgenabled = *(PQgetvalue(res, j, i_tgenabled));
			if (i_tgdef >= 0)
			{
				tginfo[j].tgdef = pg_strdup(PQgetvalue(res, j, i_tgdef));

				/* remaining fields are not valid if we have tgdef */
				tginfo[j].tgfname = NULL;
				tginfo[j].tgtype = 0;
				tginfo[j].tgnargs = 0;
				tginfo[j].tgargs = NULL;
				tginfo[j].tgisconstraint = false;
				tginfo[j].tgdeferrable = false;
				tginfo[j].tginitdeferred = false;
				tginfo[j].tgconstrname = NULL;
				tginfo[j].tgconstrrelid = InvalidOid;
				tginfo[j].tgconstrrelname = NULL;
			}
			else
			{
				tginfo[j].tgdef = NULL;

				tginfo[j].tgfname = pg_strdup(PQgetvalue(res, j, i_tgfname));
				tginfo[j].tgtype = atoi(PQgetvalue(res, j, i_tgtype));
				tginfo[j].tgnargs = atoi(PQgetvalue(res, j, i_tgnargs));
				tginfo[j].tgargs = pg_strdup(PQgetvalue(res, j, i_tgargs));
				tginfo[j].tgisconstraint = *(PQgetvalue(res, j, i_tgisconstraint)) == 't';
				tginfo[j].tgdeferrable = *(PQgetvalue(res, j, i_tgdeferrable)) == 't';
				tginfo[j].tginitdeferred = *(PQgetvalue(res, j, i_tginitdeferred)) == 't';

				if (tginfo[j].tgisconstraint)
				{
					tginfo[j].tgconstrname = pg_strdup(PQgetvalue(res, j, i_tgconstrname));
					tginfo[j].tgconstrrelid = atooid(PQgetvalue(res, j, i_tgconstrrelid));
					if (OidIsValid(tginfo[j].tgconstrrelid))
					{
						if (PQgetisnull(res, j, i_tgconstrrelname))
							fatal("query produced null referenced table name for foreign key trigger \"%s\" on table \"%s\" (OID of table: %u)",
								  tginfo[j].dobj.name,
								  tbinfo->dobj.name,
								  tginfo[j].tgconstrrelid);
						tginfo[j].tgconstrrelname = pg_strdup(PQgetvalue(res, j, i_tgconstrrelname));
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
		}

		PQclear(res);
	}

	destroyPQExpBuffer(query);
}

/*
 * getEventTriggers
 *	  get information about event triggers
 */
EventTriggerInfo *
getEventTriggers(Archive *fout, int *numEventTriggers)
{
	int			i;
	PQExpBuffer query;
	PGresult   *res;
	EventTriggerInfo *evtinfo;
	int			i_tableoid,
				i_oid,
				i_evtname,
				i_evtevent,
				i_evtowner,
				i_evttags,
				i_evtfname,
				i_evtenabled;
	int			ntups;

	/* Before 9.3, there are no event triggers */
	if (fout->remoteVersion < 90300)
	{
		*numEventTriggers = 0;
		return NULL;
	}

	query = createPQExpBuffer();

	appendPQExpBuffer(query,
					  "SELECT e.tableoid, e.oid, evtname, evtenabled, "
					  "evtevent, (%s evtowner) AS evtowner, "
					  "array_to_string(array("
					  "select quote_literal(x) "
					  " from unnest(evttags) as t(x)), ', ') as evttags, "
					  "e.evtfoid::regproc as evtfname "
					  "FROM pg_event_trigger e "
					  "ORDER BY e.oid",
					  username_subquery);

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	*numEventTriggers = ntups;

	evtinfo = (EventTriggerInfo *) pg_malloc(ntups * sizeof(EventTriggerInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_evtname = PQfnumber(res, "evtname");
	i_evtevent = PQfnumber(res, "evtevent");
	i_evtowner = PQfnumber(res, "evtowner");
	i_evttags = PQfnumber(res, "evttags");
	i_evtfname = PQfnumber(res, "evtfname");
	i_evtenabled = PQfnumber(res, "evtenabled");

	for (i = 0; i < ntups; i++)
	{
		evtinfo[i].dobj.objType = DO_EVENT_TRIGGER;
		evtinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		evtinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&evtinfo[i].dobj);
		evtinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_evtname));
		evtinfo[i].evtname = pg_strdup(PQgetvalue(res, i, i_evtname));
		evtinfo[i].evtevent = pg_strdup(PQgetvalue(res, i, i_evtevent));
		evtinfo[i].evtowner = pg_strdup(PQgetvalue(res, i, i_evtowner));
		evtinfo[i].evttags = pg_strdup(PQgetvalue(res, i, i_evttags));
		evtinfo[i].evtfname = pg_strdup(PQgetvalue(res, i, i_evtfname));
		evtinfo[i].evtenabled = *(PQgetvalue(res, i, i_evtenabled));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(evtinfo[i].dobj), fout);

		/* Event Triggers do not currently have ACLs. */
		evtinfo[i].dobj.dump &= ~DUMP_COMPONENT_ACL;
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return evtinfo;
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
getProcLangs(Archive *fout, int *numProcLangs)
{
	DumpOptions *dopt = fout->dopt;
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
	int			i_laninline;
	int			i_lanvalidator;
	int			i_lanacl;
	int			i_rlanacl;
	int			i_initlanacl;
	int			i_initrlanacl;
	int			i_lanowner;

	if (fout->remoteVersion >= 90600)
	{
		PQExpBuffer acl_subquery = createPQExpBuffer();
		PQExpBuffer racl_subquery = createPQExpBuffer();
		PQExpBuffer initacl_subquery = createPQExpBuffer();
		PQExpBuffer initracl_subquery = createPQExpBuffer();

		buildACLQueries(acl_subquery, racl_subquery, initacl_subquery,
						initracl_subquery, "l.lanacl", "l.lanowner", "'l'",
						dopt->binary_upgrade);

		/* pg_language has a laninline column */
		appendPQExpBuffer(query, "SELECT l.tableoid, l.oid, "
						  "l.lanname, l.lanpltrusted, l.lanplcallfoid, "
						  "l.laninline, l.lanvalidator, "
						  "%s AS lanacl, "
						  "%s AS rlanacl, "
						  "%s AS initlanacl, "
						  "%s AS initrlanacl, "
						  "(%s l.lanowner) AS lanowner "
						  "FROM pg_language l "
						  "LEFT JOIN pg_init_privs pip ON "
						  "(l.oid = pip.objoid "
						  "AND pip.classoid = 'pg_language'::regclass "
						  "AND pip.objsubid = 0) "
						  "WHERE l.lanispl "
						  "ORDER BY l.oid",
						  acl_subquery->data,
						  racl_subquery->data,
						  initacl_subquery->data,
						  initracl_subquery->data,
						  username_subquery);

		destroyPQExpBuffer(acl_subquery);
		destroyPQExpBuffer(racl_subquery);
		destroyPQExpBuffer(initacl_subquery);
		destroyPQExpBuffer(initracl_subquery);
	}
	else if (fout->remoteVersion >= 90000)
	{
		/* pg_language has a laninline column */
		appendPQExpBuffer(query, "SELECT tableoid, oid, "
						  "lanname, lanpltrusted, lanplcallfoid, "
						  "laninline, lanvalidator, lanacl, NULL AS rlanacl, "
						  "NULL AS initlanacl, NULL AS initrlanacl, "
						  "(%s lanowner) AS lanowner "
						  "FROM pg_language "
						  "WHERE lanispl "
						  "ORDER BY oid",
						  username_subquery);
	}
	else if (fout->remoteVersion >= 80300)
	{
		/* pg_language has a lanowner column */
		appendPQExpBuffer(query, "SELECT tableoid, oid, "
						  "lanname, lanpltrusted, lanplcallfoid, "
						  "0 AS laninline, lanvalidator, lanacl, "
						  "NULL AS rlanacl, "
						  "NULL AS initlanacl, NULL AS initrlanacl, "
						  "(%s lanowner) AS lanowner "
						  "FROM pg_language "
						  "WHERE lanispl "
						  "ORDER BY oid",
						  username_subquery);
	}
	else if (fout->remoteVersion >= 80100)
	{
		/* Languages are owned by the bootstrap superuser, OID 10 */
		appendPQExpBuffer(query, "SELECT tableoid, oid, "
						  "lanname, lanpltrusted, lanplcallfoid, "
						  "0 AS laninline, lanvalidator, lanacl, "
						  "NULL AS rlanacl, "
						  "NULL AS initlanacl, NULL AS initrlanacl, "
						  "(%s '10') AS lanowner "
						  "FROM pg_language "
						  "WHERE lanispl "
						  "ORDER BY oid",
						  username_subquery);
	}
	else
	{
		/* Languages are owned by the bootstrap superuser, sysid 1 */
		appendPQExpBuffer(query, "SELECT tableoid, oid, "
						  "lanname, lanpltrusted, lanplcallfoid, "
						  "0 AS laninline, lanvalidator, lanacl, "
						  "NULL AS rlanacl, "
						  "NULL AS initlanacl, NULL AS initrlanacl, "
						  "(%s '1') AS lanowner "
						  "FROM pg_language "
						  "WHERE lanispl "
						  "ORDER BY oid",
						  username_subquery);
	}

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	*numProcLangs = ntups;

	planginfo = (ProcLangInfo *) pg_malloc(ntups * sizeof(ProcLangInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_lanname = PQfnumber(res, "lanname");
	i_lanpltrusted = PQfnumber(res, "lanpltrusted");
	i_lanplcallfoid = PQfnumber(res, "lanplcallfoid");
	i_laninline = PQfnumber(res, "laninline");
	i_lanvalidator = PQfnumber(res, "lanvalidator");
	i_lanacl = PQfnumber(res, "lanacl");
	i_rlanacl = PQfnumber(res, "rlanacl");
	i_initlanacl = PQfnumber(res, "initlanacl");
	i_initrlanacl = PQfnumber(res, "initrlanacl");
	i_lanowner = PQfnumber(res, "lanowner");

	for (i = 0; i < ntups; i++)
	{
		planginfo[i].dobj.objType = DO_PROCLANG;
		planginfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		planginfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&planginfo[i].dobj);

		planginfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_lanname));
		planginfo[i].lanpltrusted = *(PQgetvalue(res, i, i_lanpltrusted)) == 't';
		planginfo[i].lanplcallfoid = atooid(PQgetvalue(res, i, i_lanplcallfoid));
		planginfo[i].laninline = atooid(PQgetvalue(res, i, i_laninline));
		planginfo[i].lanvalidator = atooid(PQgetvalue(res, i, i_lanvalidator));
		planginfo[i].lanacl = pg_strdup(PQgetvalue(res, i, i_lanacl));
		planginfo[i].rlanacl = pg_strdup(PQgetvalue(res, i, i_rlanacl));
		planginfo[i].initlanacl = pg_strdup(PQgetvalue(res, i, i_initlanacl));
		planginfo[i].initrlanacl = pg_strdup(PQgetvalue(res, i, i_initrlanacl));
		planginfo[i].lanowner = pg_strdup(PQgetvalue(res, i, i_lanowner));

		/* Decide whether we want to dump it */
		selectDumpableProcLang(&(planginfo[i]), fout);

		/* Do not try to dump ACL if no ACL exists. */
		if (PQgetisnull(res, i, i_lanacl) && PQgetisnull(res, i, i_rlanacl) &&
			PQgetisnull(res, i, i_initlanacl) &&
			PQgetisnull(res, i, i_initrlanacl))
			planginfo[i].dobj.dump &= ~DUMP_COMPONENT_ACL;
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
getCasts(Archive *fout, int *numCasts)
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
	int			i_castmethod;

	if (fout->remoteVersion >= 80400)
	{
		appendPQExpBufferStr(query, "SELECT tableoid, oid, "
							 "castsource, casttarget, castfunc, castcontext, "
							 "castmethod "
							 "FROM pg_cast ORDER BY 3,4");
	}
	else
	{
		appendPQExpBufferStr(query, "SELECT tableoid, oid, "
							 "castsource, casttarget, castfunc, castcontext, "
							 "CASE WHEN castfunc = 0 THEN 'b' ELSE 'f' END AS castmethod "
							 "FROM pg_cast ORDER BY 3,4");
	}

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	*numCasts = ntups;

	castinfo = (CastInfo *) pg_malloc(ntups * sizeof(CastInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_castsource = PQfnumber(res, "castsource");
	i_casttarget = PQfnumber(res, "casttarget");
	i_castfunc = PQfnumber(res, "castfunc");
	i_castcontext = PQfnumber(res, "castcontext");
	i_castmethod = PQfnumber(res, "castmethod");

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
		castinfo[i].castmethod = *(PQgetvalue(res, i, i_castmethod));

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

		/* Decide whether we want to dump it */
		selectDumpableCast(&(castinfo[i]), fout);

		/* Casts do not currently have ACLs. */
		castinfo[i].dobj.dump &= ~DUMP_COMPONENT_ACL;
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return castinfo;
}

static char *
get_language_name(Archive *fout, Oid langid)
{
	PQExpBuffer query;
	PGresult   *res;
	char	   *lanname;

	query = createPQExpBuffer();
	appendPQExpBuffer(query, "SELECT lanname FROM pg_language WHERE oid = %u", langid);
	res = ExecuteSqlQueryForSingleRow(fout, query->data);
	lanname = pg_strdup(fmtId(PQgetvalue(res, 0, 0)));
	destroyPQExpBuffer(query);
	PQclear(res);

	return lanname;
}

/*
 * getTransforms
 *	  get basic information about every transform in the system
 *
 * numTransforms is set to the number of transforms read in
 */
TransformInfo *
getTransforms(Archive *fout, int *numTransforms)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query;
	TransformInfo *transforminfo;
	int			i_tableoid;
	int			i_oid;
	int			i_trftype;
	int			i_trflang;
	int			i_trffromsql;
	int			i_trftosql;

	/* Transforms didn't exist pre-9.5 */
	if (fout->remoteVersion < 90500)
	{
		*numTransforms = 0;
		return NULL;
	}

	query = createPQExpBuffer();

	appendPQExpBufferStr(query, "SELECT tableoid, oid, "
						 "trftype, trflang, trffromsql::oid, trftosql::oid "
						 "FROM pg_transform "
						 "ORDER BY 3,4");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	*numTransforms = ntups;

	transforminfo = (TransformInfo *) pg_malloc(ntups * sizeof(TransformInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_trftype = PQfnumber(res, "trftype");
	i_trflang = PQfnumber(res, "trflang");
	i_trffromsql = PQfnumber(res, "trffromsql");
	i_trftosql = PQfnumber(res, "trftosql");

	for (i = 0; i < ntups; i++)
	{
		PQExpBufferData namebuf;
		TypeInfo   *typeInfo;
		char	   *lanname;

		transforminfo[i].dobj.objType = DO_TRANSFORM;
		transforminfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		transforminfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&transforminfo[i].dobj);
		transforminfo[i].trftype = atooid(PQgetvalue(res, i, i_trftype));
		transforminfo[i].trflang = atooid(PQgetvalue(res, i, i_trflang));
		transforminfo[i].trffromsql = atooid(PQgetvalue(res, i, i_trffromsql));
		transforminfo[i].trftosql = atooid(PQgetvalue(res, i, i_trftosql));

		/*
		 * Try to name transform as concatenation of type and language name.
		 * This is only used for purposes of sorting.  If we fail to find
		 * either, the name will be an empty string.
		 */
		initPQExpBuffer(&namebuf);
		typeInfo = findTypeByOid(transforminfo[i].trftype);
		lanname = get_language_name(fout, transforminfo[i].trflang);
		if (typeInfo && lanname)
			appendPQExpBuffer(&namebuf, "%s %s",
							  typeInfo->dobj.name, lanname);
		transforminfo[i].dobj.name = namebuf.data;
		free(lanname);

		/* Decide whether we want to dump it */
		selectDumpableObject(&(transforminfo[i].dobj), fout);
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return transforminfo;
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
getTableAttrs(Archive *fout, TableInfo *tblinfo, int numTables)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q = createPQExpBuffer();

	for (int i = 0; i < numTables; i++)
	{
		TableInfo  *tbinfo = &tblinfo[i];
		PGresult   *res;
		int			ntups;
		bool		hasdefaults;

		/* Don't bother to collect info for sequences */
		if (tbinfo->relkind == RELKIND_SEQUENCE)
			continue;

		/* Don't bother with uninteresting tables, either */
		if (!tbinfo->interesting)
			continue;

		/* find all the user attributes and their types */

		/*
		 * we must read the attribute names in attribute number order! because
		 * we will use the attnum to index into the attnames array later.
		 */
		pg_log_info("finding the columns and types of table \"%s.%s\"",
					tbinfo->dobj.namespace->dobj.name,
					tbinfo->dobj.name);

		resetPQExpBuffer(q);

		appendPQExpBufferStr(q,
							 "SELECT\n"
							 "a.attnum,\n"
							 "a.attname,\n"
							 "a.atttypmod,\n"
							 "a.attstattarget,\n"
							 "a.attstorage,\n"
							 "t.typstorage,\n"
							 "a.attnotnull,\n"
							 "a.atthasdef,\n"
							 "a.attisdropped,\n"
							 "a.attlen,\n"
							 "a.attalign,\n"
							 "a.attislocal,\n"
							 "pg_catalog.format_type(t.oid, a.atttypmod) AS atttypname,\n");

		if (fout->remoteVersion >= 90000)
			appendPQExpBufferStr(q,
								 "array_to_string(a.attoptions, ', ') AS attoptions,\n");
		else
			appendPQExpBufferStr(q,
								 "'' AS attoptions,\n");

		if (fout->remoteVersion >= 90100)
		{
			/*
			 * Since we only want to dump COLLATE clauses for attributes whose
			 * collation is different from their type's default, we use a CASE
			 * here to suppress uninteresting attcollations cheaply.
			 */
			appendPQExpBufferStr(q,
								 "CASE WHEN a.attcollation <> t.typcollation "
								 "THEN a.attcollation ELSE 0 END AS attcollation,\n");
		}
		else
			appendPQExpBufferStr(q,
								 "0 AS attcollation,\n");

		if (fout->remoteVersion >= 90200)
			appendPQExpBufferStr(q,
								 "pg_catalog.array_to_string(ARRAY("
								 "SELECT pg_catalog.quote_ident(option_name) || "
								 "' ' || pg_catalog.quote_literal(option_value) "
								 "FROM pg_catalog.pg_options_to_table(attfdwoptions) "
								 "ORDER BY option_name"
								 "), E',\n    ') AS attfdwoptions,\n");
		else
			appendPQExpBufferStr(q,
								 "'' AS attfdwoptions,\n");

		if (fout->remoteVersion >= 100000)
			appendPQExpBufferStr(q,
								 "a.attidentity,\n");
		else
			appendPQExpBufferStr(q,
								 "'' AS attidentity,\n");

		if (fout->remoteVersion >= 110000)
			appendPQExpBufferStr(q,
								 "CASE WHEN a.atthasmissing AND NOT a.attisdropped "
								 "THEN a.attmissingval ELSE null END AS attmissingval,\n");
		else
			appendPQExpBufferStr(q,
								 "NULL AS attmissingval,\n");

		if (fout->remoteVersion >= 120000)
			appendPQExpBufferStr(q,
								 "a.attgenerated\n");
		else
			appendPQExpBufferStr(q,
								 "'' AS attgenerated\n");

		/* need left join here to not fail on dropped columns ... */
		appendPQExpBuffer(q,
						  "FROM pg_catalog.pg_attribute a LEFT JOIN pg_catalog.pg_type t "
						  "ON a.atttypid = t.oid\n"
						  "WHERE a.attrelid = '%u'::pg_catalog.oid "
						  "AND a.attnum > 0::pg_catalog.int2\n"
						  "ORDER BY a.attnum",
						  tbinfo->dobj.catId.oid);

		res = ExecuteSqlQuery(fout, q->data, PGRES_TUPLES_OK);

		ntups = PQntuples(res);

		tbinfo->numatts = ntups;
		tbinfo->attnames = (char **) pg_malloc(ntups * sizeof(char *));
		tbinfo->atttypnames = (char **) pg_malloc(ntups * sizeof(char *));
		tbinfo->atttypmod = (int *) pg_malloc(ntups * sizeof(int));
		tbinfo->attstattarget = (int *) pg_malloc(ntups * sizeof(int));
		tbinfo->attstorage = (char *) pg_malloc(ntups * sizeof(char));
		tbinfo->typstorage = (char *) pg_malloc(ntups * sizeof(char));
		tbinfo->attidentity = (char *) pg_malloc(ntups * sizeof(char));
		tbinfo->attgenerated = (char *) pg_malloc(ntups * sizeof(char));
		tbinfo->attisdropped = (bool *) pg_malloc(ntups * sizeof(bool));
		tbinfo->attlen = (int *) pg_malloc(ntups * sizeof(int));
		tbinfo->attalign = (char *) pg_malloc(ntups * sizeof(char));
		tbinfo->attislocal = (bool *) pg_malloc(ntups * sizeof(bool));
		tbinfo->attoptions = (char **) pg_malloc(ntups * sizeof(char *));
		tbinfo->attcollation = (Oid *) pg_malloc(ntups * sizeof(Oid));
		tbinfo->attfdwoptions = (char **) pg_malloc(ntups * sizeof(char *));
		tbinfo->attmissingval = (char **) pg_malloc(ntups * sizeof(char *));
		tbinfo->notnull = (bool *) pg_malloc(ntups * sizeof(bool));
		tbinfo->inhNotNull = (bool *) pg_malloc(ntups * sizeof(bool));
		tbinfo->attrdefs = (AttrDefInfo **) pg_malloc(ntups * sizeof(AttrDefInfo *));
		hasdefaults = false;

		for (int j = 0; j < ntups; j++)
		{
			if (j + 1 != atoi(PQgetvalue(res, j, PQfnumber(res, "attnum"))))
				fatal("invalid column numbering in table \"%s\"",
					  tbinfo->dobj.name);
			tbinfo->attnames[j] = pg_strdup(PQgetvalue(res, j, PQfnumber(res, "attname")));
			tbinfo->atttypnames[j] = pg_strdup(PQgetvalue(res, j, PQfnumber(res, "atttypname")));
			tbinfo->atttypmod[j] = atoi(PQgetvalue(res, j, PQfnumber(res, "atttypmod")));
			tbinfo->attstattarget[j] = atoi(PQgetvalue(res, j, PQfnumber(res, "attstattarget")));
			tbinfo->attstorage[j] = *(PQgetvalue(res, j, PQfnumber(res, "attstorage")));
			tbinfo->typstorage[j] = *(PQgetvalue(res, j, PQfnumber(res, "typstorage")));
			tbinfo->attidentity[j] = *(PQgetvalue(res, j, PQfnumber(res, "attidentity")));
			tbinfo->attgenerated[j] = *(PQgetvalue(res, j, PQfnumber(res, "attgenerated")));
			tbinfo->needs_override = tbinfo->needs_override || (tbinfo->attidentity[j] == ATTRIBUTE_IDENTITY_ALWAYS);
			tbinfo->attisdropped[j] = (PQgetvalue(res, j, PQfnumber(res, "attisdropped"))[0] == 't');
			tbinfo->attlen[j] = atoi(PQgetvalue(res, j, PQfnumber(res, "attlen")));
			tbinfo->attalign[j] = *(PQgetvalue(res, j, PQfnumber(res, "attalign")));
			tbinfo->attislocal[j] = (PQgetvalue(res, j, PQfnumber(res, "attislocal"))[0] == 't');
			tbinfo->notnull[j] = (PQgetvalue(res, j, PQfnumber(res, "attnotnull"))[0] == 't');
			tbinfo->attoptions[j] = pg_strdup(PQgetvalue(res, j, PQfnumber(res, "attoptions")));
			tbinfo->attcollation[j] = atooid(PQgetvalue(res, j, PQfnumber(res, "attcollation")));
			tbinfo->attfdwoptions[j] = pg_strdup(PQgetvalue(res, j, PQfnumber(res, "attfdwoptions")));
			tbinfo->attmissingval[j] = pg_strdup(PQgetvalue(res, j, PQfnumber(res, "attmissingval")));
			tbinfo->attrdefs[j] = NULL; /* fix below */
			if (PQgetvalue(res, j, PQfnumber(res, "atthasdef"))[0] == 't')
				hasdefaults = true;
			/* these flags will be set in flagInhAttrs() */
			tbinfo->inhNotNull[j] = false;
		}

		PQclear(res);

		/*
		 * Get info about column defaults.  This is skipped for a data-only
		 * dump, as it is only needed for table schemas.
		 */
		if (!dopt->dataOnly && hasdefaults)
		{
			AttrDefInfo *attrdefs;
			int			numDefaults;

			pg_log_info("finding default expressions of table \"%s.%s\"",
						tbinfo->dobj.namespace->dobj.name,
						tbinfo->dobj.name);

			printfPQExpBuffer(q, "SELECT tableoid, oid, adnum, "
							  "pg_catalog.pg_get_expr(adbin, adrelid) AS adsrc "
							  "FROM pg_catalog.pg_attrdef "
							  "WHERE adrelid = '%u'::pg_catalog.oid",
							  tbinfo->dobj.catId.oid);

			res = ExecuteSqlQuery(fout, q->data, PGRES_TUPLES_OK);

			numDefaults = PQntuples(res);
			attrdefs = (AttrDefInfo *) pg_malloc(numDefaults * sizeof(AttrDefInfo));

			for (int j = 0; j < numDefaults; j++)
			{
				int			adnum;

				adnum = atoi(PQgetvalue(res, j, 2));

				if (adnum <= 0 || adnum > ntups)
					fatal("invalid adnum value %d for table \"%s\"",
						  adnum, tbinfo->dobj.name);

				/*
				 * dropped columns shouldn't have defaults, but just in case,
				 * ignore 'em
				 */
				if (tbinfo->attisdropped[adnum - 1])
					continue;

				attrdefs[j].dobj.objType = DO_ATTRDEF;
				attrdefs[j].dobj.catId.tableoid = atooid(PQgetvalue(res, j, 0));
				attrdefs[j].dobj.catId.oid = atooid(PQgetvalue(res, j, 1));
				AssignDumpId(&attrdefs[j].dobj);
				attrdefs[j].adtable = tbinfo;
				attrdefs[j].adnum = adnum;
				attrdefs[j].adef_expr = pg_strdup(PQgetvalue(res, j, 3));

				attrdefs[j].dobj.name = pg_strdup(tbinfo->dobj.name);
				attrdefs[j].dobj.namespace = tbinfo->dobj.namespace;

				attrdefs[j].dobj.dump = tbinfo->dobj.dump;

				/*
				 * Defaults on a VIEW must always be dumped as separate ALTER
				 * TABLE commands.  Defaults on regular tables are dumped as
				 * part of the CREATE TABLE if possible, which it won't be if
				 * the column is not going to be emitted explicitly.
				 */
				if (tbinfo->relkind == RELKIND_VIEW)
				{
					attrdefs[j].separate = true;
				}
				else if (!shouldPrintColumn(dopt, tbinfo, adnum - 1))
				{
					/* column will be suppressed, print default separately */
					attrdefs[j].separate = true;
				}
				else
				{
					attrdefs[j].separate = false;

					/*
					 * Mark the default as needing to appear before the table,
					 * so that any dependencies it has must be emitted before
					 * the CREATE TABLE.  If this is not possible, we'll
					 * change to "separate" mode while sorting dependencies.
					 */
					addObjectDependency(&tbinfo->dobj,
										attrdefs[j].dobj.dumpId);
				}

				tbinfo->attrdefs[adnum - 1] = &attrdefs[j];
			}
			PQclear(res);
		}

		/*
		 * Get info about table CHECK constraints.  This is skipped for a
		 * data-only dump, as it is only needed for table schemas.
		 */
		if (tbinfo->ncheck > 0 && !dopt->dataOnly)
		{
			ConstraintInfo *constrs;
			int			numConstrs;

			pg_log_info("finding check constraints for table \"%s.%s\"",
						tbinfo->dobj.namespace->dobj.name,
						tbinfo->dobj.name);

			resetPQExpBuffer(q);
			if (fout->remoteVersion >= 90200)
			{
				/*
				 * convalidated is new in 9.2 (actually, it is there in 9.1,
				 * but it wasn't ever false for check constraints until 9.2).
				 */
				appendPQExpBuffer(q, "SELECT tableoid, oid, conname, "
								  "pg_catalog.pg_get_constraintdef(oid) AS consrc, "
								  "conislocal, convalidated "
								  "FROM pg_catalog.pg_constraint "
								  "WHERE conrelid = '%u'::pg_catalog.oid "
								  "   AND contype = 'c' "
								  "ORDER BY conname",
								  tbinfo->dobj.catId.oid);
			}
			else if (fout->remoteVersion >= 80400)
			{
				/* conislocal is new in 8.4 */
				appendPQExpBuffer(q, "SELECT tableoid, oid, conname, "
								  "pg_catalog.pg_get_constraintdef(oid) AS consrc, "
								  "conislocal, true AS convalidated "
								  "FROM pg_catalog.pg_constraint "
								  "WHERE conrelid = '%u'::pg_catalog.oid "
								  "   AND contype = 'c' "
								  "ORDER BY conname",
								  tbinfo->dobj.catId.oid);
			}
			else
			{
				appendPQExpBuffer(q, "SELECT tableoid, oid, conname, "
								  "pg_catalog.pg_get_constraintdef(oid) AS consrc, "
								  "true AS conislocal, true AS convalidated "
								  "FROM pg_catalog.pg_constraint "
								  "WHERE conrelid = '%u'::pg_catalog.oid "
								  "   AND contype = 'c' "
								  "ORDER BY conname",
								  tbinfo->dobj.catId.oid);
			}

			res = ExecuteSqlQuery(fout, q->data, PGRES_TUPLES_OK);

			numConstrs = PQntuples(res);
			if (numConstrs != tbinfo->ncheck)
			{
				pg_log_error(ngettext("expected %d check constraint on table \"%s\" but found %d",
									  "expected %d check constraints on table \"%s\" but found %d",
									  tbinfo->ncheck),
							 tbinfo->ncheck, tbinfo->dobj.name, numConstrs);
				pg_log_error("(The system catalogs might be corrupted.)");
				exit_nicely(1);
			}

			constrs = (ConstraintInfo *) pg_malloc(numConstrs * sizeof(ConstraintInfo));
			tbinfo->checkexprs = constrs;

			for (int j = 0; j < numConstrs; j++)
			{
				bool		validated = PQgetvalue(res, j, 5)[0] == 't';

				constrs[j].dobj.objType = DO_CONSTRAINT;
				constrs[j].dobj.catId.tableoid = atooid(PQgetvalue(res, j, 0));
				constrs[j].dobj.catId.oid = atooid(PQgetvalue(res, j, 1));
				AssignDumpId(&constrs[j].dobj);
				constrs[j].dobj.name = pg_strdup(PQgetvalue(res, j, 2));
				constrs[j].dobj.namespace = tbinfo->dobj.namespace;
				constrs[j].contable = tbinfo;
				constrs[j].condomain = NULL;
				constrs[j].contype = 'c';
				constrs[j].condef = pg_strdup(PQgetvalue(res, j, 3));
				constrs[j].confrelid = InvalidOid;
				constrs[j].conindex = 0;
				constrs[j].condeferrable = false;
				constrs[j].condeferred = false;
				constrs[j].conislocal = (PQgetvalue(res, j, 4)[0] == 't');

				/*
				 * An unvalidated constraint needs to be dumped separately, so
				 * that potentially-violating existing data is loaded before
				 * the constraint.
				 */
				constrs[j].separate = !validated;

				constrs[j].dobj.dump = tbinfo->dobj.dump;

				/*
				 * Mark the constraint as needing to appear before the table
				 * --- this is so that any other dependencies of the
				 * constraint will be emitted before we try to create the
				 * table.  If the constraint is to be dumped separately, it
				 * will be dumped after data is loaded anyway, so don't do it.
				 * (There's an automatic dependency in the opposite direction
				 * anyway, so don't need to add one manually here.)
				 */
				if (!constrs[j].separate)
					addObjectDependency(&tbinfo->dobj,
										constrs[j].dobj.dumpId);

				/*
				 * If the constraint is inherited, this will be detected later
				 * (in pre-8.4 databases).  We also detect later if the
				 * constraint must be split out from the table definition.
				 */
			}
			PQclear(res);
		}
	}

	destroyPQExpBuffer(q);
}

/*
 * Test whether a column should be printed as part of table's CREATE TABLE.
 * Column number is zero-based.
 *
 * Normally this is always true, but it's false for dropped columns, as well
 * as those that were inherited without any local definition.  (If we print
 * such a column it will mistakenly get pg_attribute.attislocal set to true.)
 * For partitions, it's always true, because we want the partitions to be
 * created independently and ATTACH PARTITION used afterwards.
 *
 * In binary_upgrade mode, we must print all columns and fix the attislocal/
 * attisdropped state later, so as to keep control of the physical column
 * order.
 *
 * This function exists because there are scattered nonobvious places that
 * must be kept in sync with this decision.
 */
bool
shouldPrintColumn(DumpOptions *dopt, TableInfo *tbinfo, int colno)
{
	if (dopt->binary_upgrade)
		return true;
	if (tbinfo->attisdropped[colno])
		return false;
	return (tbinfo->attislocal[colno] || tbinfo->ispartition);
}


/*
 * getTSParsers:
 *	  read all text search parsers in the system catalogs and return them
 *	  in the TSParserInfo* structure
 *
 *	numTSParsers is set to the number of parsers read in
 */
TSParserInfo *
getTSParsers(Archive *fout, int *numTSParsers)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query;
	TSParserInfo *prsinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_prsname;
	int			i_prsnamespace;
	int			i_prsstart;
	int			i_prstoken;
	int			i_prsend;
	int			i_prsheadline;
	int			i_prslextype;

	/* Before 8.3, there is no built-in text search support */
	if (fout->remoteVersion < 80300)
	{
		*numTSParsers = 0;
		return NULL;
	}

	query = createPQExpBuffer();

	/*
	 * find all text search objects, including builtin ones; we filter out
	 * system-defined objects at dump-out time.
	 */

	appendPQExpBufferStr(query, "SELECT tableoid, oid, prsname, prsnamespace, "
						 "prsstart::oid, prstoken::oid, "
						 "prsend::oid, prsheadline::oid, prslextype::oid "
						 "FROM pg_ts_parser");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numTSParsers = ntups;

	prsinfo = (TSParserInfo *) pg_malloc(ntups * sizeof(TSParserInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_prsname = PQfnumber(res, "prsname");
	i_prsnamespace = PQfnumber(res, "prsnamespace");
	i_prsstart = PQfnumber(res, "prsstart");
	i_prstoken = PQfnumber(res, "prstoken");
	i_prsend = PQfnumber(res, "prsend");
	i_prsheadline = PQfnumber(res, "prsheadline");
	i_prslextype = PQfnumber(res, "prslextype");

	for (i = 0; i < ntups; i++)
	{
		prsinfo[i].dobj.objType = DO_TSPARSER;
		prsinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		prsinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&prsinfo[i].dobj);
		prsinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_prsname));
		prsinfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_prsnamespace)));
		prsinfo[i].prsstart = atooid(PQgetvalue(res, i, i_prsstart));
		prsinfo[i].prstoken = atooid(PQgetvalue(res, i, i_prstoken));
		prsinfo[i].prsend = atooid(PQgetvalue(res, i, i_prsend));
		prsinfo[i].prsheadline = atooid(PQgetvalue(res, i, i_prsheadline));
		prsinfo[i].prslextype = atooid(PQgetvalue(res, i, i_prslextype));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(prsinfo[i].dobj), fout);

		/* Text Search Parsers do not currently have ACLs. */
		prsinfo[i].dobj.dump &= ~DUMP_COMPONENT_ACL;
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return prsinfo;
}

/*
 * getTSDictionaries:
 *	  read all text search dictionaries in the system catalogs and return them
 *	  in the TSDictInfo* structure
 *
 *	numTSDicts is set to the number of dictionaries read in
 */
TSDictInfo *
getTSDictionaries(Archive *fout, int *numTSDicts)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query;
	TSDictInfo *dictinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_dictname;
	int			i_dictnamespace;
	int			i_rolname;
	int			i_dicttemplate;
	int			i_dictinitoption;

	/* Before 8.3, there is no built-in text search support */
	if (fout->remoteVersion < 80300)
	{
		*numTSDicts = 0;
		return NULL;
	}

	query = createPQExpBuffer();

	appendPQExpBuffer(query, "SELECT tableoid, oid, dictname, "
					  "dictnamespace, (%s dictowner) AS rolname, "
					  "dicttemplate, dictinitoption "
					  "FROM pg_ts_dict",
					  username_subquery);

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numTSDicts = ntups;

	dictinfo = (TSDictInfo *) pg_malloc(ntups * sizeof(TSDictInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_dictname = PQfnumber(res, "dictname");
	i_dictnamespace = PQfnumber(res, "dictnamespace");
	i_rolname = PQfnumber(res, "rolname");
	i_dictinitoption = PQfnumber(res, "dictinitoption");
	i_dicttemplate = PQfnumber(res, "dicttemplate");

	for (i = 0; i < ntups; i++)
	{
		dictinfo[i].dobj.objType = DO_TSDICT;
		dictinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		dictinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&dictinfo[i].dobj);
		dictinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_dictname));
		dictinfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_dictnamespace)));
		dictinfo[i].rolname = pg_strdup(PQgetvalue(res, i, i_rolname));
		dictinfo[i].dicttemplate = atooid(PQgetvalue(res, i, i_dicttemplate));
		if (PQgetisnull(res, i, i_dictinitoption))
			dictinfo[i].dictinitoption = NULL;
		else
			dictinfo[i].dictinitoption = pg_strdup(PQgetvalue(res, i, i_dictinitoption));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(dictinfo[i].dobj), fout);

		/* Text Search Dictionaries do not currently have ACLs. */
		dictinfo[i].dobj.dump &= ~DUMP_COMPONENT_ACL;
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return dictinfo;
}

/*
 * getTSTemplates:
 *	  read all text search templates in the system catalogs and return them
 *	  in the TSTemplateInfo* structure
 *
 *	numTSTemplates is set to the number of templates read in
 */
TSTemplateInfo *
getTSTemplates(Archive *fout, int *numTSTemplates)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query;
	TSTemplateInfo *tmplinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_tmplname;
	int			i_tmplnamespace;
	int			i_tmplinit;
	int			i_tmpllexize;

	/* Before 8.3, there is no built-in text search support */
	if (fout->remoteVersion < 80300)
	{
		*numTSTemplates = 0;
		return NULL;
	}

	query = createPQExpBuffer();

	appendPQExpBufferStr(query, "SELECT tableoid, oid, tmplname, "
						 "tmplnamespace, tmplinit::oid, tmpllexize::oid "
						 "FROM pg_ts_template");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numTSTemplates = ntups;

	tmplinfo = (TSTemplateInfo *) pg_malloc(ntups * sizeof(TSTemplateInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_tmplname = PQfnumber(res, "tmplname");
	i_tmplnamespace = PQfnumber(res, "tmplnamespace");
	i_tmplinit = PQfnumber(res, "tmplinit");
	i_tmpllexize = PQfnumber(res, "tmpllexize");

	for (i = 0; i < ntups; i++)
	{
		tmplinfo[i].dobj.objType = DO_TSTEMPLATE;
		tmplinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		tmplinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&tmplinfo[i].dobj);
		tmplinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_tmplname));
		tmplinfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_tmplnamespace)));
		tmplinfo[i].tmplinit = atooid(PQgetvalue(res, i, i_tmplinit));
		tmplinfo[i].tmpllexize = atooid(PQgetvalue(res, i, i_tmpllexize));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(tmplinfo[i].dobj), fout);

		/* Text Search Templates do not currently have ACLs. */
		tmplinfo[i].dobj.dump &= ~DUMP_COMPONENT_ACL;
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return tmplinfo;
}

/*
 * getTSConfigurations:
 *	  read all text search configurations in the system catalogs and return
 *	  them in the TSConfigInfo* structure
 *
 *	numTSConfigs is set to the number of configurations read in
 */
TSConfigInfo *
getTSConfigurations(Archive *fout, int *numTSConfigs)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query;
	TSConfigInfo *cfginfo;
	int			i_tableoid;
	int			i_oid;
	int			i_cfgname;
	int			i_cfgnamespace;
	int			i_rolname;
	int			i_cfgparser;

	/* Before 8.3, there is no built-in text search support */
	if (fout->remoteVersion < 80300)
	{
		*numTSConfigs = 0;
		return NULL;
	}

	query = createPQExpBuffer();

	appendPQExpBuffer(query, "SELECT tableoid, oid, cfgname, "
					  "cfgnamespace, (%s cfgowner) AS rolname, cfgparser "
					  "FROM pg_ts_config",
					  username_subquery);

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numTSConfigs = ntups;

	cfginfo = (TSConfigInfo *) pg_malloc(ntups * sizeof(TSConfigInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_cfgname = PQfnumber(res, "cfgname");
	i_cfgnamespace = PQfnumber(res, "cfgnamespace");
	i_rolname = PQfnumber(res, "rolname");
	i_cfgparser = PQfnumber(res, "cfgparser");

	for (i = 0; i < ntups; i++)
	{
		cfginfo[i].dobj.objType = DO_TSCONFIG;
		cfginfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		cfginfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&cfginfo[i].dobj);
		cfginfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_cfgname));
		cfginfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_cfgnamespace)));
		cfginfo[i].rolname = pg_strdup(PQgetvalue(res, i, i_rolname));
		cfginfo[i].cfgparser = atooid(PQgetvalue(res, i, i_cfgparser));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(cfginfo[i].dobj), fout);

		/* Text Search Configurations do not currently have ACLs. */
		cfginfo[i].dobj.dump &= ~DUMP_COMPONENT_ACL;
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return cfginfo;
}

/*
 * getForeignDataWrappers:
 *	  read all foreign-data wrappers in the system catalogs and return
 *	  them in the FdwInfo* structure
 *
 *	numForeignDataWrappers is set to the number of fdws read in
 */
FdwInfo *
getForeignDataWrappers(Archive *fout, int *numForeignDataWrappers)
{
	DumpOptions *dopt = fout->dopt;
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query;
	FdwInfo    *fdwinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_fdwname;
	int			i_rolname;
	int			i_fdwhandler;
	int			i_fdwvalidator;
	int			i_fdwacl;
	int			i_rfdwacl;
	int			i_initfdwacl;
	int			i_initrfdwacl;
	int			i_fdwoptions;

	/* Before 8.4, there are no foreign-data wrappers */
	if (fout->remoteVersion < 80400)
	{
		*numForeignDataWrappers = 0;
		return NULL;
	}

	query = createPQExpBuffer();

	if (fout->remoteVersion >= 90600)
	{
		PQExpBuffer acl_subquery = createPQExpBuffer();
		PQExpBuffer racl_subquery = createPQExpBuffer();
		PQExpBuffer initacl_subquery = createPQExpBuffer();
		PQExpBuffer initracl_subquery = createPQExpBuffer();

		buildACLQueries(acl_subquery, racl_subquery, initacl_subquery,
						initracl_subquery, "f.fdwacl", "f.fdwowner", "'F'",
						dopt->binary_upgrade);

		appendPQExpBuffer(query, "SELECT f.tableoid, f.oid, f.fdwname, "
						  "(%s f.fdwowner) AS rolname, "
						  "f.fdwhandler::pg_catalog.regproc, "
						  "f.fdwvalidator::pg_catalog.regproc, "
						  "%s AS fdwacl, "
						  "%s AS rfdwacl, "
						  "%s AS initfdwacl, "
						  "%s AS initrfdwacl, "
						  "array_to_string(ARRAY("
						  "SELECT quote_ident(option_name) || ' ' || "
						  "quote_literal(option_value) "
						  "FROM pg_options_to_table(f.fdwoptions) "
						  "ORDER BY option_name"
						  "), E',\n    ') AS fdwoptions "
						  "FROM pg_foreign_data_wrapper f "
						  "LEFT JOIN pg_init_privs pip ON "
						  "(f.oid = pip.objoid "
						  "AND pip.classoid = 'pg_foreign_data_wrapper'::regclass "
						  "AND pip.objsubid = 0) ",
						  username_subquery,
						  acl_subquery->data,
						  racl_subquery->data,
						  initacl_subquery->data,
						  initracl_subquery->data);

		destroyPQExpBuffer(acl_subquery);
		destroyPQExpBuffer(racl_subquery);
		destroyPQExpBuffer(initacl_subquery);
		destroyPQExpBuffer(initracl_subquery);
	}
	else if (fout->remoteVersion >= 90100)
	{
		appendPQExpBuffer(query, "SELECT tableoid, oid, fdwname, "
						  "(%s fdwowner) AS rolname, "
						  "fdwhandler::pg_catalog.regproc, "
						  "fdwvalidator::pg_catalog.regproc, fdwacl, "
						  "NULL as rfdwacl, "
						  "NULL as initfdwacl, NULL AS initrfdwacl, "
						  "array_to_string(ARRAY("
						  "SELECT quote_ident(option_name) || ' ' || "
						  "quote_literal(option_value) "
						  "FROM pg_options_to_table(fdwoptions) "
						  "ORDER BY option_name"
						  "), E',\n    ') AS fdwoptions "
						  "FROM pg_foreign_data_wrapper",
						  username_subquery);
	}
	else
	{
		appendPQExpBuffer(query, "SELECT tableoid, oid, fdwname, "
						  "(%s fdwowner) AS rolname, "
						  "'-' AS fdwhandler, "
						  "fdwvalidator::pg_catalog.regproc, fdwacl, "
						  "NULL as rfdwacl, "
						  "NULL as initfdwacl, NULL AS initrfdwacl, "
						  "array_to_string(ARRAY("
						  "SELECT quote_ident(option_name) || ' ' || "
						  "quote_literal(option_value) "
						  "FROM pg_options_to_table(fdwoptions) "
						  "ORDER BY option_name"
						  "), E',\n    ') AS fdwoptions "
						  "FROM pg_foreign_data_wrapper",
						  username_subquery);
	}

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numForeignDataWrappers = ntups;

	fdwinfo = (FdwInfo *) pg_malloc(ntups * sizeof(FdwInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_fdwname = PQfnumber(res, "fdwname");
	i_rolname = PQfnumber(res, "rolname");
	i_fdwhandler = PQfnumber(res, "fdwhandler");
	i_fdwvalidator = PQfnumber(res, "fdwvalidator");
	i_fdwacl = PQfnumber(res, "fdwacl");
	i_rfdwacl = PQfnumber(res, "rfdwacl");
	i_initfdwacl = PQfnumber(res, "initfdwacl");
	i_initrfdwacl = PQfnumber(res, "initrfdwacl");
	i_fdwoptions = PQfnumber(res, "fdwoptions");

	for (i = 0; i < ntups; i++)
	{
		fdwinfo[i].dobj.objType = DO_FDW;
		fdwinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		fdwinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&fdwinfo[i].dobj);
		fdwinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_fdwname));
		fdwinfo[i].dobj.namespace = NULL;
		fdwinfo[i].rolname = pg_strdup(PQgetvalue(res, i, i_rolname));
		fdwinfo[i].fdwhandler = pg_strdup(PQgetvalue(res, i, i_fdwhandler));
		fdwinfo[i].fdwvalidator = pg_strdup(PQgetvalue(res, i, i_fdwvalidator));
		fdwinfo[i].fdwoptions = pg_strdup(PQgetvalue(res, i, i_fdwoptions));
		fdwinfo[i].fdwacl = pg_strdup(PQgetvalue(res, i, i_fdwacl));
		fdwinfo[i].rfdwacl = pg_strdup(PQgetvalue(res, i, i_rfdwacl));
		fdwinfo[i].initfdwacl = pg_strdup(PQgetvalue(res, i, i_initfdwacl));
		fdwinfo[i].initrfdwacl = pg_strdup(PQgetvalue(res, i, i_initrfdwacl));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(fdwinfo[i].dobj), fout);

		/* Do not try to dump ACL if no ACL exists. */
		if (PQgetisnull(res, i, i_fdwacl) && PQgetisnull(res, i, i_rfdwacl) &&
			PQgetisnull(res, i, i_initfdwacl) &&
			PQgetisnull(res, i, i_initrfdwacl))
			fdwinfo[i].dobj.dump &= ~DUMP_COMPONENT_ACL;
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return fdwinfo;
}

/*
 * getForeignServers:
 *	  read all foreign servers in the system catalogs and return
 *	  them in the ForeignServerInfo * structure
 *
 *	numForeignServers is set to the number of servers read in
 */
ForeignServerInfo *
getForeignServers(Archive *fout, int *numForeignServers)
{
	DumpOptions *dopt = fout->dopt;
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query;
	ForeignServerInfo *srvinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_srvname;
	int			i_rolname;
	int			i_srvfdw;
	int			i_srvtype;
	int			i_srvversion;
	int			i_srvacl;
	int			i_rsrvacl;
	int			i_initsrvacl;
	int			i_initrsrvacl;
	int			i_srvoptions;

	/* Before 8.4, there are no foreign servers */
	if (fout->remoteVersion < 80400)
	{
		*numForeignServers = 0;
		return NULL;
	}

	query = createPQExpBuffer();

	if (fout->remoteVersion >= 90600)
	{
		PQExpBuffer acl_subquery = createPQExpBuffer();
		PQExpBuffer racl_subquery = createPQExpBuffer();
		PQExpBuffer initacl_subquery = createPQExpBuffer();
		PQExpBuffer initracl_subquery = createPQExpBuffer();

		buildACLQueries(acl_subquery, racl_subquery, initacl_subquery,
						initracl_subquery, "f.srvacl", "f.srvowner", "'S'",
						dopt->binary_upgrade);

		appendPQExpBuffer(query, "SELECT f.tableoid, f.oid, f.srvname, "
						  "(%s f.srvowner) AS rolname, "
						  "f.srvfdw, f.srvtype, f.srvversion, "
						  "%s AS srvacl, "
						  "%s AS rsrvacl, "
						  "%s AS initsrvacl, "
						  "%s AS initrsrvacl, "
						  "array_to_string(ARRAY("
						  "SELECT quote_ident(option_name) || ' ' || "
						  "quote_literal(option_value) "
						  "FROM pg_options_to_table(f.srvoptions) "
						  "ORDER BY option_name"
						  "), E',\n    ') AS srvoptions "
						  "FROM pg_foreign_server f "
						  "LEFT JOIN pg_init_privs pip "
						  "ON (f.oid = pip.objoid "
						  "AND pip.classoid = 'pg_foreign_server'::regclass "
						  "AND pip.objsubid = 0) ",
						  username_subquery,
						  acl_subquery->data,
						  racl_subquery->data,
						  initacl_subquery->data,
						  initracl_subquery->data);

		destroyPQExpBuffer(acl_subquery);
		destroyPQExpBuffer(racl_subquery);
		destroyPQExpBuffer(initacl_subquery);
		destroyPQExpBuffer(initracl_subquery);
	}
	else
	{
		appendPQExpBuffer(query, "SELECT tableoid, oid, srvname, "
						  "(%s srvowner) AS rolname, "
						  "srvfdw, srvtype, srvversion, srvacl, "
						  "NULL AS rsrvacl, "
						  "NULL AS initsrvacl, NULL AS initrsrvacl, "
						  "array_to_string(ARRAY("
						  "SELECT quote_ident(option_name) || ' ' || "
						  "quote_literal(option_value) "
						  "FROM pg_options_to_table(srvoptions) "
						  "ORDER BY option_name"
						  "), E',\n    ') AS srvoptions "
						  "FROM pg_foreign_server",
						  username_subquery);
	}

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numForeignServers = ntups;

	srvinfo = (ForeignServerInfo *) pg_malloc(ntups * sizeof(ForeignServerInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_srvname = PQfnumber(res, "srvname");
	i_rolname = PQfnumber(res, "rolname");
	i_srvfdw = PQfnumber(res, "srvfdw");
	i_srvtype = PQfnumber(res, "srvtype");
	i_srvversion = PQfnumber(res, "srvversion");
	i_srvacl = PQfnumber(res, "srvacl");
	i_rsrvacl = PQfnumber(res, "rsrvacl");
	i_initsrvacl = PQfnumber(res, "initsrvacl");
	i_initrsrvacl = PQfnumber(res, "initrsrvacl");
	i_srvoptions = PQfnumber(res, "srvoptions");

	for (i = 0; i < ntups; i++)
	{
		srvinfo[i].dobj.objType = DO_FOREIGN_SERVER;
		srvinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		srvinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&srvinfo[i].dobj);
		srvinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_srvname));
		srvinfo[i].dobj.namespace = NULL;
		srvinfo[i].rolname = pg_strdup(PQgetvalue(res, i, i_rolname));
		srvinfo[i].srvfdw = atooid(PQgetvalue(res, i, i_srvfdw));
		srvinfo[i].srvtype = pg_strdup(PQgetvalue(res, i, i_srvtype));
		srvinfo[i].srvversion = pg_strdup(PQgetvalue(res, i, i_srvversion));
		srvinfo[i].srvoptions = pg_strdup(PQgetvalue(res, i, i_srvoptions));
		srvinfo[i].srvacl = pg_strdup(PQgetvalue(res, i, i_srvacl));
		srvinfo[i].rsrvacl = pg_strdup(PQgetvalue(res, i, i_rsrvacl));
		srvinfo[i].initsrvacl = pg_strdup(PQgetvalue(res, i, i_initsrvacl));
		srvinfo[i].initrsrvacl = pg_strdup(PQgetvalue(res, i, i_initrsrvacl));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(srvinfo[i].dobj), fout);

		/* Do not try to dump ACL if no ACL exists. */
		if (PQgetisnull(res, i, i_srvacl) && PQgetisnull(res, i, i_rsrvacl) &&
			PQgetisnull(res, i, i_initsrvacl) &&
			PQgetisnull(res, i, i_initrsrvacl))
			srvinfo[i].dobj.dump &= ~DUMP_COMPONENT_ACL;
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return srvinfo;
}

/*
 * getDefaultACLs:
 *	  read all default ACL information in the system catalogs and return
 *	  them in the DefaultACLInfo structure
 *
 *	numDefaultACLs is set to the number of ACLs read in
 */
DefaultACLInfo *
getDefaultACLs(Archive *fout, int *numDefaultACLs)
{
	DumpOptions *dopt = fout->dopt;
	DefaultACLInfo *daclinfo;
	PQExpBuffer query;
	PGresult   *res;
	int			i_oid;
	int			i_tableoid;
	int			i_defaclrole;
	int			i_defaclnamespace;
	int			i_defaclobjtype;
	int			i_defaclacl;
	int			i_rdefaclacl;
	int			i_initdefaclacl;
	int			i_initrdefaclacl;
	int			i,
				ntups;

	if (fout->remoteVersion < 90000)
	{
		*numDefaultACLs = 0;
		return NULL;
	}

	query = createPQExpBuffer();

	if (fout->remoteVersion >= 90600)
	{
		PQExpBuffer acl_subquery = createPQExpBuffer();
		PQExpBuffer racl_subquery = createPQExpBuffer();
		PQExpBuffer initacl_subquery = createPQExpBuffer();
		PQExpBuffer initracl_subquery = createPQExpBuffer();

		buildACLQueries(acl_subquery, racl_subquery, initacl_subquery,
						initracl_subquery, "defaclacl", "defaclrole",
						"CASE WHEN defaclobjtype = 'S' THEN 's' ELSE defaclobjtype END::\"char\"",
						dopt->binary_upgrade);

		appendPQExpBuffer(query, "SELECT d.oid, d.tableoid, "
						  "(%s d.defaclrole) AS defaclrole, "
						  "d.defaclnamespace, "
						  "d.defaclobjtype, "
						  "%s AS defaclacl, "
						  "%s AS rdefaclacl, "
						  "%s AS initdefaclacl, "
						  "%s AS initrdefaclacl "
						  "FROM pg_default_acl d "
						  "LEFT JOIN pg_init_privs pip ON "
						  "(d.oid = pip.objoid "
						  "AND pip.classoid = 'pg_default_acl'::regclass "
						  "AND pip.objsubid = 0) ",
						  username_subquery,
						  acl_subquery->data,
						  racl_subquery->data,
						  initacl_subquery->data,
						  initracl_subquery->data);

		destroyPQExpBuffer(acl_subquery);
		destroyPQExpBuffer(racl_subquery);
		destroyPQExpBuffer(initacl_subquery);
		destroyPQExpBuffer(initracl_subquery);
	}
	else
	{
		appendPQExpBuffer(query, "SELECT oid, tableoid, "
						  "(%s defaclrole) AS defaclrole, "
						  "defaclnamespace, "
						  "defaclobjtype, "
						  "defaclacl, "
						  "NULL AS rdefaclacl, "
						  "NULL AS initdefaclacl, "
						  "NULL AS initrdefaclacl "
						  "FROM pg_default_acl",
						  username_subquery);
	}

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numDefaultACLs = ntups;

	daclinfo = (DefaultACLInfo *) pg_malloc(ntups * sizeof(DefaultACLInfo));

	i_oid = PQfnumber(res, "oid");
	i_tableoid = PQfnumber(res, "tableoid");
	i_defaclrole = PQfnumber(res, "defaclrole");
	i_defaclnamespace = PQfnumber(res, "defaclnamespace");
	i_defaclobjtype = PQfnumber(res, "defaclobjtype");
	i_defaclacl = PQfnumber(res, "defaclacl");
	i_rdefaclacl = PQfnumber(res, "rdefaclacl");
	i_initdefaclacl = PQfnumber(res, "initdefaclacl");
	i_initrdefaclacl = PQfnumber(res, "initrdefaclacl");

	for (i = 0; i < ntups; i++)
	{
		Oid			nspid = atooid(PQgetvalue(res, i, i_defaclnamespace));

		daclinfo[i].dobj.objType = DO_DEFAULT_ACL;
		daclinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		daclinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&daclinfo[i].dobj);
		/* cheesy ... is it worth coming up with a better object name? */
		daclinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_defaclobjtype));

		if (nspid != InvalidOid)
			daclinfo[i].dobj.namespace = findNamespace(nspid);
		else
			daclinfo[i].dobj.namespace = NULL;

		daclinfo[i].defaclrole = pg_strdup(PQgetvalue(res, i, i_defaclrole));
		daclinfo[i].defaclobjtype = *(PQgetvalue(res, i, i_defaclobjtype));
		daclinfo[i].defaclacl = pg_strdup(PQgetvalue(res, i, i_defaclacl));
		daclinfo[i].rdefaclacl = pg_strdup(PQgetvalue(res, i, i_rdefaclacl));
		daclinfo[i].initdefaclacl = pg_strdup(PQgetvalue(res, i, i_initdefaclacl));
		daclinfo[i].initrdefaclacl = pg_strdup(PQgetvalue(res, i, i_initrdefaclacl));

		/* Decide whether we want to dump it */
		selectDumpableDefaultACL(&(daclinfo[i]), dopt);
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return daclinfo;
}

/*
 * dumpComment --
 *
 * This routine is used to dump any comments associated with the
 * object handed to this routine. The routine takes the object type
 * and object name (ready to print, except for schema decoration), plus
 * the namespace and owner of the object (for labeling the ArchiveEntry),
 * plus catalog ID and subid which are the lookup key for pg_description,
 * plus the dump ID for the object (for setting a dependency).
 * If a matching pg_description entry is found, it is dumped.
 *
 * Note: in some cases, such as comments for triggers and rules, the "type"
 * string really looks like, e.g., "TRIGGER name ON".  This is a bit of a hack
 * but it doesn't seem worth complicating the API for all callers to make
 * it cleaner.
 *
 * Note: although this routine takes a dumpId for dependency purposes,
 * that purpose is just to mark the dependency in the emitted dump file
 * for possible future use by pg_restore.  We do NOT use it for determining
 * ordering of the comment in the dump file, because this routine is called
 * after dependency sorting occurs.  This routine should be called just after
 * calling ArchiveEntry() for the specified object.
 */
static void
dumpComment(Archive *fout, const char *type, const char *name,
			const char *namespace, const char *owner,
			CatalogId catalogId, int subid, DumpId dumpId)
{
	DumpOptions *dopt = fout->dopt;
	CommentItem *comments;
	int			ncomments;

	/* do nothing, if --no-comments is supplied */
	if (dopt->no_comments)
		return;

	/* Comments are schema not data ... except blob comments are data */
	if (strcmp(type, "LARGE OBJECT") != 0)
	{
		if (dopt->dataOnly)
			return;
	}
	else
	{
		/* We do dump blob comments in binary-upgrade mode */
		if (dopt->schemaOnly && !dopt->binary_upgrade)
			return;
	}

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
		PQExpBuffer tag = createPQExpBuffer();

		appendPQExpBuffer(query, "COMMENT ON %s ", type);
		if (namespace && *namespace)
			appendPQExpBuffer(query, "%s.", fmtId(namespace));
		appendPQExpBuffer(query, "%s IS ", name);
		appendStringLiteralAH(query, comments->descr, fout);
		appendPQExpBufferStr(query, ";\n");

		appendPQExpBuffer(tag, "%s %s", type, name);

		/*
		 * We mark comments as SECTION_NONE because they really belong in the
		 * same section as their parent, whether that is pre-data or
		 * post-data.
		 */
		ArchiveEntry(fout, nilCatalogId, createDumpId(),
					 ARCHIVE_OPTS(.tag = tag->data,
								  .namespace = namespace,
								  .owner = owner,
								  .description = "COMMENT",
								  .section = SECTION_NONE,
								  .createStmt = query->data,
								  .deps = &dumpId,
								  .nDeps = 1));

		destroyPQExpBuffer(query);
		destroyPQExpBuffer(tag);
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
	DumpOptions *dopt = fout->dopt;
	CommentItem *comments;
	int			ncomments;
	PQExpBuffer query;
	PQExpBuffer tag;

	/* do nothing, if --no-comments is supplied */
	if (dopt->no_comments)
		return;

	/* Comments are SCHEMA not data */
	if (dopt->dataOnly)
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
	tag = createPQExpBuffer();

	while (ncomments > 0)
	{
		const char *descr = comments->descr;
		int			objsubid = comments->objsubid;

		if (objsubid == 0)
		{
			resetPQExpBuffer(tag);
			appendPQExpBuffer(tag, "%s %s", reltypename,
							  fmtId(tbinfo->dobj.name));

			resetPQExpBuffer(query);
			appendPQExpBuffer(query, "COMMENT ON %s %s IS ", reltypename,
							  fmtQualifiedDumpable(tbinfo));
			appendStringLiteralAH(query, descr, fout);
			appendPQExpBufferStr(query, ";\n");

			ArchiveEntry(fout, nilCatalogId, createDumpId(),
						 ARCHIVE_OPTS(.tag = tag->data,
									  .namespace = tbinfo->dobj.namespace->dobj.name,
									  .owner = tbinfo->rolname,
									  .description = "COMMENT",
									  .section = SECTION_NONE,
									  .createStmt = query->data,
									  .deps = &(tbinfo->dobj.dumpId),
									  .nDeps = 1));
		}
		else if (objsubid > 0 && objsubid <= tbinfo->numatts)
		{
			resetPQExpBuffer(tag);
			appendPQExpBuffer(tag, "COLUMN %s.",
							  fmtId(tbinfo->dobj.name));
			appendPQExpBufferStr(tag, fmtId(tbinfo->attnames[objsubid - 1]));

			resetPQExpBuffer(query);
			appendPQExpBuffer(query, "COMMENT ON COLUMN %s.",
							  fmtQualifiedDumpable(tbinfo));
			appendPQExpBuffer(query, "%s IS ",
							  fmtId(tbinfo->attnames[objsubid - 1]));
			appendStringLiteralAH(query, descr, fout);
			appendPQExpBufferStr(query, ";\n");

			ArchiveEntry(fout, nilCatalogId, createDumpId(),
						 ARCHIVE_OPTS(.tag = tag->data,
									  .namespace = tbinfo->dobj.namespace->dobj.name,
									  .owner = tbinfo->rolname,
									  .description = "COMMENT",
									  .section = SECTION_NONE,
									  .createStmt = query->data,
									  .deps = &(tbinfo->dobj.dumpId),
									  .nDeps = 1));
		}

		comments++;
		ncomments--;
	}

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(tag);
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

	query = createPQExpBuffer();

	appendPQExpBufferStr(query, "SELECT description, classoid, objoid, objsubid "
						 "FROM pg_catalog.pg_description "
						 "ORDER BY classoid, objoid, objsubid");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	/* Construct lookup table containing OIDs in numeric form */

	i_description = PQfnumber(res, "description");
	i_classoid = PQfnumber(res, "classoid");
	i_objoid = PQfnumber(res, "objoid");
	i_objsubid = PQfnumber(res, "objsubid");

	ntups = PQntuples(res);

	comments = (CommentItem *) pg_malloc(ntups * sizeof(CommentItem));

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
		case DO_EXTENSION:
			dumpExtension(fout, (ExtensionInfo *) dobj);
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
		case DO_ACCESS_METHOD:
			dumpAccessMethod(fout, (AccessMethodInfo *) dobj);
			break;
		case DO_OPCLASS:
			dumpOpclass(fout, (OpclassInfo *) dobj);
			break;
		case DO_OPFAMILY:
			dumpOpfamily(fout, (OpfamilyInfo *) dobj);
			break;
		case DO_COLLATION:
			dumpCollation(fout, (CollInfo *) dobj);
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
		case DO_INDEX_ATTACH:
			dumpIndexAttach(fout, (IndexAttachInfo *) dobj);
			break;
		case DO_STATSEXT:
			dumpStatisticsExt(fout, (StatsExtInfo *) dobj);
			break;
		case DO_REFRESH_MATVIEW:
			refreshMatViewData(fout, (TableDataInfo *) dobj);
			break;
		case DO_RULE:
			dumpRule(fout, (RuleInfo *) dobj);
			break;
		case DO_TRIGGER:
			dumpTrigger(fout, (TriggerInfo *) dobj);
			break;
		case DO_EVENT_TRIGGER:
			dumpEventTrigger(fout, (EventTriggerInfo *) dobj);
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
		case DO_TRANSFORM:
			dumpTransform(fout, (TransformInfo *) dobj);
			break;
		case DO_SEQUENCE_SET:
			dumpSequenceData(fout, (TableDataInfo *) dobj);
			break;
		case DO_TABLE_DATA:
			dumpTableData(fout, (TableDataInfo *) dobj);
			break;
		case DO_DUMMY_TYPE:
			/* table rowtypes and array types are never dumped separately */
			break;
		case DO_TSPARSER:
			dumpTSParser(fout, (TSParserInfo *) dobj);
			break;
		case DO_TSDICT:
			dumpTSDictionary(fout, (TSDictInfo *) dobj);
			break;
		case DO_TSTEMPLATE:
			dumpTSTemplate(fout, (TSTemplateInfo *) dobj);
			break;
		case DO_TSCONFIG:
			dumpTSConfig(fout, (TSConfigInfo *) dobj);
			break;
		case DO_FDW:
			dumpForeignDataWrapper(fout, (FdwInfo *) dobj);
			break;
		case DO_FOREIGN_SERVER:
			dumpForeignServer(fout, (ForeignServerInfo *) dobj);
			break;
		case DO_DEFAULT_ACL:
			dumpDefaultACL(fout, (DefaultACLInfo *) dobj);
			break;
		case DO_BLOB:
			dumpBlob(fout, (BlobInfo *) dobj);
			break;
		case DO_BLOB_DATA:
			if (dobj->dump & DUMP_COMPONENT_DATA)
			{
				TocEntry   *te;

				te = ArchiveEntry(fout, dobj->catId, dobj->dumpId,
								  ARCHIVE_OPTS(.tag = dobj->name,
											   .description = "BLOBS",
											   .section = SECTION_DATA,
											   .dumpFn = dumpBlobs));

				/*
				 * Set the TocEntry's dataLength in case we are doing a
				 * parallel dump and want to order dump jobs by table size.
				 * (We need some size estimate for every TocEntry with a
				 * DataDumper function.)  We don't currently have any cheap
				 * way to estimate the size of blobs, but it doesn't matter;
				 * let's just set the size to a large value so parallel dumps
				 * will launch this job first.  If there's lots of blobs, we
				 * win, and if there aren't, we don't lose much.  (If you want
				 * to improve on this, really what you should be thinking
				 * about is allowing blob dumping to be parallelized, not just
				 * getting a smarter estimate for the single TOC entry.)
				 */
				te->dataLength = MaxBlockNumber;
			}
			break;
		case DO_POLICY:
			dumpPolicy(fout, (PolicyInfo *) dobj);
			break;
		case DO_PUBLICATION:
			dumpPublication(fout, (PublicationInfo *) dobj);
			break;
		case DO_PUBLICATION_REL:
			dumpPublicationTable(fout, (PublicationRelInfo *) dobj);
			break;
		case DO_SUBSCRIPTION:
			dumpSubscription(fout, (SubscriptionInfo *) dobj);
			break;
		case DO_PRE_DATA_BOUNDARY:
		case DO_POST_DATA_BOUNDARY:
			/* never dumped, nothing to do */
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
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *qnspname;

	/* Skip if not to be dumped */
	if (!nspinfo->dobj.dump || dopt->dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	qnspname = pg_strdup(fmtId(nspinfo->dobj.name));

	appendPQExpBuffer(delq, "DROP SCHEMA %s;\n", qnspname);

	appendPQExpBuffer(q, "CREATE SCHEMA %s;\n", qnspname);

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &nspinfo->dobj,
										"SCHEMA", qnspname, NULL);

	if (nspinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, nspinfo->dobj.catId, nspinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = nspinfo->dobj.name,
								  .owner = nspinfo->rolname,
								  .description = "SCHEMA",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* Dump Schema Comments and Security Labels */
	if (nspinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "SCHEMA", qnspname,
					NULL, nspinfo->rolname,
					nspinfo->dobj.catId, 0, nspinfo->dobj.dumpId);

	if (nspinfo->dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpSecLabel(fout, "SCHEMA", qnspname,
					 NULL, nspinfo->rolname,
					 nspinfo->dobj.catId, 0, nspinfo->dobj.dumpId);

	if (nspinfo->dobj.dump & DUMP_COMPONENT_ACL)
		dumpACL(fout, nspinfo->dobj.dumpId, InvalidDumpId, "SCHEMA",
				qnspname, NULL, NULL,
				nspinfo->rolname, nspinfo->nspacl, nspinfo->rnspacl,
				nspinfo->initnspacl, nspinfo->initrnspacl);

	free(qnspname);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
}

/*
 * dumpExtension
 *	  writes out to fout the queries to recreate an extension
 */
static void
dumpExtension(Archive *fout, ExtensionInfo *extinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *qextname;

	/* Skip if not to be dumped */
	if (!extinfo->dobj.dump || dopt->dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	qextname = pg_strdup(fmtId(extinfo->dobj.name));

	appendPQExpBuffer(delq, "DROP EXTENSION %s;\n", qextname);

	if (!dopt->binary_upgrade)
	{
		/*
		 * In a regular dump, we simply create the extension, intentionally
		 * not specifying a version, so that the destination installation's
		 * default version is used.
		 *
		 * Use of IF NOT EXISTS here is unlike our behavior for other object
		 * types; but there are various scenarios in which it's convenient to
		 * manually create the desired extension before restoring, so we
		 * prefer to allow it to exist already.
		 */
		appendPQExpBuffer(q, "CREATE EXTENSION IF NOT EXISTS %s WITH SCHEMA %s;\n",
						  qextname, fmtId(extinfo->namespace));
	}
	else
	{
		/*
		 * In binary-upgrade mode, it's critical to reproduce the state of the
		 * database exactly, so our procedure is to create an empty extension,
		 * restore all the contained objects normally, and add them to the
		 * extension one by one.  This function performs just the first of
		 * those steps.  binary_upgrade_extension_member() takes care of
		 * adding member objects as they're created.
		 */
		int			i;
		int			n;

		appendPQExpBufferStr(q, "-- For binary upgrade, create an empty extension and insert objects into it\n");

		/*
		 * We unconditionally create the extension, so we must drop it if it
		 * exists.  This could happen if the user deleted 'plpgsql' and then
		 * readded it, causing its oid to be greater than g_last_builtin_oid.
		 */
		appendPQExpBuffer(q, "DROP EXTENSION IF EXISTS %s;\n", qextname);

		appendPQExpBufferStr(q,
							 "SELECT pg_catalog.binary_upgrade_create_empty_extension(");
		appendStringLiteralAH(q, extinfo->dobj.name, fout);
		appendPQExpBufferStr(q, ", ");
		appendStringLiteralAH(q, extinfo->namespace, fout);
		appendPQExpBufferStr(q, ", ");
		appendPQExpBuffer(q, "%s, ", extinfo->relocatable ? "true" : "false");
		appendStringLiteralAH(q, extinfo->extversion, fout);
		appendPQExpBufferStr(q, ", ");

		/*
		 * Note that we're pushing extconfig (an OID array) back into
		 * pg_extension exactly as-is.  This is OK because pg_class OIDs are
		 * preserved in binary upgrade.
		 */
		if (strlen(extinfo->extconfig) > 2)
			appendStringLiteralAH(q, extinfo->extconfig, fout);
		else
			appendPQExpBufferStr(q, "NULL");
		appendPQExpBufferStr(q, ", ");
		if (strlen(extinfo->extcondition) > 2)
			appendStringLiteralAH(q, extinfo->extcondition, fout);
		else
			appendPQExpBufferStr(q, "NULL");
		appendPQExpBufferStr(q, ", ");
		appendPQExpBufferStr(q, "ARRAY[");
		n = 0;
		for (i = 0; i < extinfo->dobj.nDeps; i++)
		{
			DumpableObject *extobj;

			extobj = findObjectByDumpId(extinfo->dobj.dependencies[i]);
			if (extobj && extobj->objType == DO_EXTENSION)
			{
				if (n++ > 0)
					appendPQExpBufferChar(q, ',');
				appendStringLiteralAH(q, extobj->name, fout);
			}
		}
		appendPQExpBufferStr(q, "]::pg_catalog.text[]");
		appendPQExpBufferStr(q, ");\n");
	}

	if (extinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, extinfo->dobj.catId, extinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = extinfo->dobj.name,
								  .description = "EXTENSION",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* Dump Extension Comments and Security Labels */
	if (extinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "EXTENSION", qextname,
					NULL, "",
					extinfo->dobj.catId, 0, extinfo->dobj.dumpId);

	if (extinfo->dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpSecLabel(fout, "EXTENSION", qextname,
					 NULL, "",
					 extinfo->dobj.catId, 0, extinfo->dobj.dumpId);

	free(qextname);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
}

/*
 * dumpType
 *	  writes out to fout the queries to recreate a user-defined type
 */
static void
dumpType(Archive *fout, TypeInfo *tyinfo)
{
	DumpOptions *dopt = fout->dopt;

	/* Skip if not to be dumped */
	if (!tyinfo->dobj.dump || dopt->dataOnly)
		return;

	/* Dump out in proper style */
	if (tyinfo->typtype == TYPTYPE_BASE)
		dumpBaseType(fout, tyinfo);
	else if (tyinfo->typtype == TYPTYPE_DOMAIN)
		dumpDomain(fout, tyinfo);
	else if (tyinfo->typtype == TYPTYPE_COMPOSITE)
		dumpCompositeType(fout, tyinfo);
	else if (tyinfo->typtype == TYPTYPE_ENUM)
		dumpEnumType(fout, tyinfo);
	else if (tyinfo->typtype == TYPTYPE_RANGE)
		dumpRangeType(fout, tyinfo);
	else if (tyinfo->typtype == TYPTYPE_PSEUDO && !tyinfo->isDefined)
		dumpUndefinedType(fout, tyinfo);
	else
		pg_log_warning("typtype of data type \"%s\" appears to be invalid",
					   tyinfo->dobj.name);
}

/*
 * dumpEnumType
 *	  writes out to fout the queries to recreate a user-defined enum type
 */
static void
dumpEnumType(Archive *fout, TypeInfo *tyinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;
	int			num,
				i;
	Oid			enum_oid;
	char	   *qtypname;
	char	   *qualtypname;
	char	   *label;

	if (fout->remoteVersion >= 90100)
		appendPQExpBuffer(query, "SELECT oid, enumlabel "
						  "FROM pg_catalog.pg_enum "
						  "WHERE enumtypid = '%u'"
						  "ORDER BY enumsortorder",
						  tyinfo->dobj.catId.oid);
	else
		appendPQExpBuffer(query, "SELECT oid, enumlabel "
						  "FROM pg_catalog.pg_enum "
						  "WHERE enumtypid = '%u'"
						  "ORDER BY oid",
						  tyinfo->dobj.catId.oid);

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	num = PQntuples(res);

	qtypname = pg_strdup(fmtId(tyinfo->dobj.name));
	qualtypname = pg_strdup(fmtQualifiedDumpable(tyinfo));

	/*
	 * CASCADE shouldn't be required here as for normal types since the I/O
	 * functions are generic and do not get dropped.
	 */
	appendPQExpBuffer(delq, "DROP TYPE %s;\n", qualtypname);

	if (dopt->binary_upgrade)
		binary_upgrade_set_type_oids_by_type_oid(fout, q,
												 tyinfo->dobj.catId.oid,
												 false);

	appendPQExpBuffer(q, "CREATE TYPE %s AS ENUM (",
					  qualtypname);

	if (!dopt->binary_upgrade)
	{
		/* Labels with server-assigned oids */
		for (i = 0; i < num; i++)
		{
			label = PQgetvalue(res, i, PQfnumber(res, "enumlabel"));
			if (i > 0)
				appendPQExpBufferChar(q, ',');
			appendPQExpBufferStr(q, "\n    ");
			appendStringLiteralAH(q, label, fout);
		}
	}

	appendPQExpBufferStr(q, "\n);\n");

	if (dopt->binary_upgrade)
	{
		/* Labels with dump-assigned (preserved) oids */
		for (i = 0; i < num; i++)
		{
			enum_oid = atooid(PQgetvalue(res, i, PQfnumber(res, "oid")));
			label = PQgetvalue(res, i, PQfnumber(res, "enumlabel"));

			if (i == 0)
				appendPQExpBufferStr(q, "\n-- For binary upgrade, must preserve pg_enum oids\n");
			appendPQExpBuffer(q,
							  "SELECT pg_catalog.binary_upgrade_set_next_pg_enum_oid('%u'::pg_catalog.oid);\n",
							  enum_oid);
			appendPQExpBuffer(q, "ALTER TYPE %s ADD VALUE ", qualtypname);
			appendStringLiteralAH(q, label, fout);
			appendPQExpBufferStr(q, ";\n\n");
		}
	}

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &tyinfo->dobj,
										"TYPE", qtypname,
										tyinfo->dobj.namespace->dobj.name);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, tyinfo->dobj.catId, tyinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tyinfo->dobj.name,
								  .namespace = tyinfo->dobj.namespace->dobj.name,
								  .owner = tyinfo->rolname,
								  .description = "TYPE",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* Dump Type Comments and Security Labels */
	if (tyinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "TYPE", qtypname,
					tyinfo->dobj.namespace->dobj.name, tyinfo->rolname,
					tyinfo->dobj.catId, 0, tyinfo->dobj.dumpId);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpSecLabel(fout, "TYPE", qtypname,
					 tyinfo->dobj.namespace->dobj.name, tyinfo->rolname,
					 tyinfo->dobj.catId, 0, tyinfo->dobj.dumpId);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_ACL)
		dumpACL(fout, tyinfo->dobj.dumpId, InvalidDumpId, "TYPE",
				qtypname, NULL,
				tyinfo->dobj.namespace->dobj.name,
				tyinfo->rolname, tyinfo->typacl, tyinfo->rtypacl,
				tyinfo->inittypacl, tyinfo->initrtypacl);

	PQclear(res);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(query);
	free(qtypname);
	free(qualtypname);
}

/*
 * dumpRangeType
 *	  writes out to fout the queries to recreate a user-defined range type
 */
static void
dumpRangeType(Archive *fout, TypeInfo *tyinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;
	Oid			collationOid;
	char	   *qtypname;
	char	   *qualtypname;
	char	   *procname;

	appendPQExpBuffer(query,
					  "SELECT pg_catalog.format_type(rngsubtype, NULL) AS rngsubtype, "
					  "opc.opcname AS opcname, "
					  "(SELECT nspname FROM pg_catalog.pg_namespace nsp "
					  "  WHERE nsp.oid = opc.opcnamespace) AS opcnsp, "
					  "opc.opcdefault, "
					  "CASE WHEN rngcollation = st.typcollation THEN 0 "
					  "     ELSE rngcollation END AS collation, "
					  "rngcanonical, rngsubdiff "
					  "FROM pg_catalog.pg_range r, pg_catalog.pg_type st, "
					  "     pg_catalog.pg_opclass opc "
					  "WHERE st.oid = rngsubtype AND opc.oid = rngsubopc AND "
					  "rngtypid = '%u'",
					  tyinfo->dobj.catId.oid);

	res = ExecuteSqlQueryForSingleRow(fout, query->data);

	qtypname = pg_strdup(fmtId(tyinfo->dobj.name));
	qualtypname = pg_strdup(fmtQualifiedDumpable(tyinfo));

	/*
	 * CASCADE shouldn't be required here as for normal types since the I/O
	 * functions are generic and do not get dropped.
	 */
	appendPQExpBuffer(delq, "DROP TYPE %s;\n", qualtypname);

	if (dopt->binary_upgrade)
		binary_upgrade_set_type_oids_by_type_oid(fout, q,
												 tyinfo->dobj.catId.oid,
												 false);

	appendPQExpBuffer(q, "CREATE TYPE %s AS RANGE (",
					  qualtypname);

	appendPQExpBuffer(q, "\n    subtype = %s",
					  PQgetvalue(res, 0, PQfnumber(res, "rngsubtype")));

	/* print subtype_opclass only if not default for subtype */
	if (PQgetvalue(res, 0, PQfnumber(res, "opcdefault"))[0] != 't')
	{
		char	   *opcname = PQgetvalue(res, 0, PQfnumber(res, "opcname"));
		char	   *nspname = PQgetvalue(res, 0, PQfnumber(res, "opcnsp"));

		appendPQExpBuffer(q, ",\n    subtype_opclass = %s.",
						  fmtId(nspname));
		appendPQExpBufferStr(q, fmtId(opcname));
	}

	collationOid = atooid(PQgetvalue(res, 0, PQfnumber(res, "collation")));
	if (OidIsValid(collationOid))
	{
		CollInfo   *coll = findCollationByOid(collationOid);

		if (coll)
			appendPQExpBuffer(q, ",\n    collation = %s",
							  fmtQualifiedDumpable(coll));
	}

	procname = PQgetvalue(res, 0, PQfnumber(res, "rngcanonical"));
	if (strcmp(procname, "-") != 0)
		appendPQExpBuffer(q, ",\n    canonical = %s", procname);

	procname = PQgetvalue(res, 0, PQfnumber(res, "rngsubdiff"));
	if (strcmp(procname, "-") != 0)
		appendPQExpBuffer(q, ",\n    subtype_diff = %s", procname);

	appendPQExpBufferStr(q, "\n);\n");

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &tyinfo->dobj,
										"TYPE", qtypname,
										tyinfo->dobj.namespace->dobj.name);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, tyinfo->dobj.catId, tyinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tyinfo->dobj.name,
								  .namespace = tyinfo->dobj.namespace->dobj.name,
								  .owner = tyinfo->rolname,
								  .description = "TYPE",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* Dump Type Comments and Security Labels */
	if (tyinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "TYPE", qtypname,
					tyinfo->dobj.namespace->dobj.name, tyinfo->rolname,
					tyinfo->dobj.catId, 0, tyinfo->dobj.dumpId);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpSecLabel(fout, "TYPE", qtypname,
					 tyinfo->dobj.namespace->dobj.name, tyinfo->rolname,
					 tyinfo->dobj.catId, 0, tyinfo->dobj.dumpId);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_ACL)
		dumpACL(fout, tyinfo->dobj.dumpId, InvalidDumpId, "TYPE",
				qtypname, NULL,
				tyinfo->dobj.namespace->dobj.name,
				tyinfo->rolname, tyinfo->typacl, tyinfo->rtypacl,
				tyinfo->inittypacl, tyinfo->initrtypacl);

	PQclear(res);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(query);
	free(qtypname);
	free(qualtypname);
}

/*
 * dumpUndefinedType
 *	  writes out to fout the queries to recreate a !typisdefined type
 *
 * This is a shell type, but we use different terminology to distinguish
 * this case from where we have to emit a shell type definition to break
 * circular dependencies.  An undefined type shouldn't ever have anything
 * depending on it.
 */
static void
dumpUndefinedType(Archive *fout, TypeInfo *tyinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	char	   *qtypname;
	char	   *qualtypname;

	qtypname = pg_strdup(fmtId(tyinfo->dobj.name));
	qualtypname = pg_strdup(fmtQualifiedDumpable(tyinfo));

	appendPQExpBuffer(delq, "DROP TYPE %s;\n", qualtypname);

	if (dopt->binary_upgrade)
		binary_upgrade_set_type_oids_by_type_oid(fout, q,
												 tyinfo->dobj.catId.oid,
												 false);

	appendPQExpBuffer(q, "CREATE TYPE %s;\n",
					  qualtypname);

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &tyinfo->dobj,
										"TYPE", qtypname,
										tyinfo->dobj.namespace->dobj.name);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, tyinfo->dobj.catId, tyinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tyinfo->dobj.name,
								  .namespace = tyinfo->dobj.namespace->dobj.name,
								  .owner = tyinfo->rolname,
								  .description = "TYPE",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* Dump Type Comments and Security Labels */
	if (tyinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "TYPE", qtypname,
					tyinfo->dobj.namespace->dobj.name, tyinfo->rolname,
					tyinfo->dobj.catId, 0, tyinfo->dobj.dumpId);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpSecLabel(fout, "TYPE", qtypname,
					 tyinfo->dobj.namespace->dobj.name, tyinfo->rolname,
					 tyinfo->dobj.catId, 0, tyinfo->dobj.dumpId);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_ACL)
		dumpACL(fout, tyinfo->dobj.dumpId, InvalidDumpId, "TYPE",
				qtypname, NULL,
				tyinfo->dobj.namespace->dobj.name,
				tyinfo->rolname, tyinfo->typacl, tyinfo->rtypacl,
				tyinfo->inittypacl, tyinfo->initrtypacl);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	free(qtypname);
	free(qualtypname);
}

/*
 * dumpBaseType
 *	  writes out to fout the queries to recreate a user-defined base type
 */
static void
dumpBaseType(Archive *fout, TypeInfo *tyinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;
	char	   *qtypname;
	char	   *qualtypname;
	char	   *typlen;
	char	   *typinput;
	char	   *typoutput;
	char	   *typreceive;
	char	   *typsend;
	char	   *typmodin;
	char	   *typmodout;
	char	   *typanalyze;
	Oid			typreceiveoid;
	Oid			typsendoid;
	Oid			typmodinoid;
	Oid			typmodoutoid;
	Oid			typanalyzeoid;
	char	   *typcategory;
	char	   *typispreferred;
	char	   *typdelim;
	char	   *typbyval;
	char	   *typalign;
	char	   *typstorage;
	char	   *typcollatable;
	char	   *typdefault;
	bool		typdefault_is_literal = false;

	/* Fetch type-specific details */
	if (fout->remoteVersion >= 90100)
	{
		appendPQExpBuffer(query, "SELECT typlen, "
						  "typinput, typoutput, typreceive, typsend, "
						  "typmodin, typmodout, typanalyze, "
						  "typreceive::pg_catalog.oid AS typreceiveoid, "
						  "typsend::pg_catalog.oid AS typsendoid, "
						  "typmodin::pg_catalog.oid AS typmodinoid, "
						  "typmodout::pg_catalog.oid AS typmodoutoid, "
						  "typanalyze::pg_catalog.oid AS typanalyzeoid, "
						  "typcategory, typispreferred, "
						  "typdelim, typbyval, typalign, typstorage, "
						  "(typcollation <> 0) AS typcollatable, "
						  "pg_catalog.pg_get_expr(typdefaultbin, 0) AS typdefaultbin, typdefault "
						  "FROM pg_catalog.pg_type "
						  "WHERE oid = '%u'::pg_catalog.oid",
						  tyinfo->dobj.catId.oid);
	}
	else if (fout->remoteVersion >= 80400)
	{
		appendPQExpBuffer(query, "SELECT typlen, "
						  "typinput, typoutput, typreceive, typsend, "
						  "typmodin, typmodout, typanalyze, "
						  "typreceive::pg_catalog.oid AS typreceiveoid, "
						  "typsend::pg_catalog.oid AS typsendoid, "
						  "typmodin::pg_catalog.oid AS typmodinoid, "
						  "typmodout::pg_catalog.oid AS typmodoutoid, "
						  "typanalyze::pg_catalog.oid AS typanalyzeoid, "
						  "typcategory, typispreferred, "
						  "typdelim, typbyval, typalign, typstorage, "
						  "false AS typcollatable, "
						  "pg_catalog.pg_get_expr(typdefaultbin, 0) AS typdefaultbin, typdefault "
						  "FROM pg_catalog.pg_type "
						  "WHERE oid = '%u'::pg_catalog.oid",
						  tyinfo->dobj.catId.oid);
	}
	else if (fout->remoteVersion >= 80300)
	{
		/* Before 8.4, pg_get_expr does not allow 0 for its second arg */
		appendPQExpBuffer(query, "SELECT typlen, "
						  "typinput, typoutput, typreceive, typsend, "
						  "typmodin, typmodout, typanalyze, "
						  "typreceive::pg_catalog.oid AS typreceiveoid, "
						  "typsend::pg_catalog.oid AS typsendoid, "
						  "typmodin::pg_catalog.oid AS typmodinoid, "
						  "typmodout::pg_catalog.oid AS typmodoutoid, "
						  "typanalyze::pg_catalog.oid AS typanalyzeoid, "
						  "'U' AS typcategory, false AS typispreferred, "
						  "typdelim, typbyval, typalign, typstorage, "
						  "false AS typcollatable, "
						  "pg_catalog.pg_get_expr(typdefaultbin, 'pg_catalog.pg_type'::pg_catalog.regclass) AS typdefaultbin, typdefault "
						  "FROM pg_catalog.pg_type "
						  "WHERE oid = '%u'::pg_catalog.oid",
						  tyinfo->dobj.catId.oid);
	}
	else
	{
		appendPQExpBuffer(query, "SELECT typlen, "
						  "typinput, typoutput, typreceive, typsend, "
						  "'-' AS typmodin, '-' AS typmodout, "
						  "typanalyze, "
						  "typreceive::pg_catalog.oid AS typreceiveoid, "
						  "typsend::pg_catalog.oid AS typsendoid, "
						  "0 AS typmodinoid, 0 AS typmodoutoid, "
						  "typanalyze::pg_catalog.oid AS typanalyzeoid, "
						  "'U' AS typcategory, false AS typispreferred, "
						  "typdelim, typbyval, typalign, typstorage, "
						  "false AS typcollatable, "
						  "pg_catalog.pg_get_expr(typdefaultbin, 'pg_catalog.pg_type'::pg_catalog.regclass) AS typdefaultbin, typdefault "
						  "FROM pg_catalog.pg_type "
						  "WHERE oid = '%u'::pg_catalog.oid",
						  tyinfo->dobj.catId.oid);
	}

	res = ExecuteSqlQueryForSingleRow(fout, query->data);

	typlen = PQgetvalue(res, 0, PQfnumber(res, "typlen"));
	typinput = PQgetvalue(res, 0, PQfnumber(res, "typinput"));
	typoutput = PQgetvalue(res, 0, PQfnumber(res, "typoutput"));
	typreceive = PQgetvalue(res, 0, PQfnumber(res, "typreceive"));
	typsend = PQgetvalue(res, 0, PQfnumber(res, "typsend"));
	typmodin = PQgetvalue(res, 0, PQfnumber(res, "typmodin"));
	typmodout = PQgetvalue(res, 0, PQfnumber(res, "typmodout"));
	typanalyze = PQgetvalue(res, 0, PQfnumber(res, "typanalyze"));
	typreceiveoid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typreceiveoid")));
	typsendoid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typsendoid")));
	typmodinoid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typmodinoid")));
	typmodoutoid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typmodoutoid")));
	typanalyzeoid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typanalyzeoid")));
	typcategory = PQgetvalue(res, 0, PQfnumber(res, "typcategory"));
	typispreferred = PQgetvalue(res, 0, PQfnumber(res, "typispreferred"));
	typdelim = PQgetvalue(res, 0, PQfnumber(res, "typdelim"));
	typbyval = PQgetvalue(res, 0, PQfnumber(res, "typbyval"));
	typalign = PQgetvalue(res, 0, PQfnumber(res, "typalign"));
	typstorage = PQgetvalue(res, 0, PQfnumber(res, "typstorage"));
	typcollatable = PQgetvalue(res, 0, PQfnumber(res, "typcollatable"));
	if (!PQgetisnull(res, 0, PQfnumber(res, "typdefaultbin")))
		typdefault = PQgetvalue(res, 0, PQfnumber(res, "typdefaultbin"));
	else if (!PQgetisnull(res, 0, PQfnumber(res, "typdefault")))
	{
		typdefault = PQgetvalue(res, 0, PQfnumber(res, "typdefault"));
		typdefault_is_literal = true;	/* it needs quotes */
	}
	else
		typdefault = NULL;

	qtypname = pg_strdup(fmtId(tyinfo->dobj.name));
	qualtypname = pg_strdup(fmtQualifiedDumpable(tyinfo));

	/*
	 * The reason we include CASCADE is that the circular dependency between
	 * the type and its I/O functions makes it impossible to drop the type any
	 * other way.
	 */
	appendPQExpBuffer(delq, "DROP TYPE %s CASCADE;\n", qualtypname);

	/*
	 * We might already have a shell type, but setting pg_type_oid is
	 * harmless, and in any case we'd better set the array type OID.
	 */
	if (dopt->binary_upgrade)
		binary_upgrade_set_type_oids_by_type_oid(fout, q,
												 tyinfo->dobj.catId.oid,
												 false);

	appendPQExpBuffer(q,
					  "CREATE TYPE %s (\n"
					  "    INTERNALLENGTH = %s",
					  qualtypname,
					  (strcmp(typlen, "-1") == 0) ? "variable" : typlen);

	/* regproc result is sufficiently quoted already */
	appendPQExpBuffer(q, ",\n    INPUT = %s", typinput);
	appendPQExpBuffer(q, ",\n    OUTPUT = %s", typoutput);
	if (OidIsValid(typreceiveoid))
		appendPQExpBuffer(q, ",\n    RECEIVE = %s", typreceive);
	if (OidIsValid(typsendoid))
		appendPQExpBuffer(q, ",\n    SEND = %s", typsend);
	if (OidIsValid(typmodinoid))
		appendPQExpBuffer(q, ",\n    TYPMOD_IN = %s", typmodin);
	if (OidIsValid(typmodoutoid))
		appendPQExpBuffer(q, ",\n    TYPMOD_OUT = %s", typmodout);
	if (OidIsValid(typanalyzeoid))
		appendPQExpBuffer(q, ",\n    ANALYZE = %s", typanalyze);

	if (strcmp(typcollatable, "t") == 0)
		appendPQExpBufferStr(q, ",\n    COLLATABLE = true");

	if (typdefault != NULL)
	{
		appendPQExpBufferStr(q, ",\n    DEFAULT = ");
		if (typdefault_is_literal)
			appendStringLiteralAH(q, typdefault, fout);
		else
			appendPQExpBufferStr(q, typdefault);
	}

	if (OidIsValid(tyinfo->typelem))
	{
		char	   *elemType;

		elemType = getFormattedTypeName(fout, tyinfo->typelem, zeroIsError);
		appendPQExpBuffer(q, ",\n    ELEMENT = %s", elemType);
		free(elemType);
	}

	if (strcmp(typcategory, "U") != 0)
	{
		appendPQExpBufferStr(q, ",\n    CATEGORY = ");
		appendStringLiteralAH(q, typcategory, fout);
	}

	if (strcmp(typispreferred, "t") == 0)
		appendPQExpBufferStr(q, ",\n    PREFERRED = true");

	if (typdelim && strcmp(typdelim, ",") != 0)
	{
		appendPQExpBufferStr(q, ",\n    DELIMITER = ");
		appendStringLiteralAH(q, typdelim, fout);
	}

	if (*typalign == TYPALIGN_CHAR)
		appendPQExpBufferStr(q, ",\n    ALIGNMENT = char");
	else if (*typalign == TYPALIGN_SHORT)
		appendPQExpBufferStr(q, ",\n    ALIGNMENT = int2");
	else if (*typalign == TYPALIGN_INT)
		appendPQExpBufferStr(q, ",\n    ALIGNMENT = int4");
	else if (*typalign == TYPALIGN_DOUBLE)
		appendPQExpBufferStr(q, ",\n    ALIGNMENT = double");

	if (*typstorage == TYPSTORAGE_PLAIN)
		appendPQExpBufferStr(q, ",\n    STORAGE = plain");
	else if (*typstorage == TYPSTORAGE_EXTERNAL)
		appendPQExpBufferStr(q, ",\n    STORAGE = external");
	else if (*typstorage == TYPSTORAGE_EXTENDED)
		appendPQExpBufferStr(q, ",\n    STORAGE = extended");
	else if (*typstorage == TYPSTORAGE_MAIN)
		appendPQExpBufferStr(q, ",\n    STORAGE = main");

	if (strcmp(typbyval, "t") == 0)
		appendPQExpBufferStr(q, ",\n    PASSEDBYVALUE");

	appendPQExpBufferStr(q, "\n);\n");

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &tyinfo->dobj,
										"TYPE", qtypname,
										tyinfo->dobj.namespace->dobj.name);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, tyinfo->dobj.catId, tyinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tyinfo->dobj.name,
								  .namespace = tyinfo->dobj.namespace->dobj.name,
								  .owner = tyinfo->rolname,
								  .description = "TYPE",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* Dump Type Comments and Security Labels */
	if (tyinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "TYPE", qtypname,
					tyinfo->dobj.namespace->dobj.name, tyinfo->rolname,
					tyinfo->dobj.catId, 0, tyinfo->dobj.dumpId);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpSecLabel(fout, "TYPE", qtypname,
					 tyinfo->dobj.namespace->dobj.name, tyinfo->rolname,
					 tyinfo->dobj.catId, 0, tyinfo->dobj.dumpId);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_ACL)
		dumpACL(fout, tyinfo->dobj.dumpId, InvalidDumpId, "TYPE",
				qtypname, NULL,
				tyinfo->dobj.namespace->dobj.name,
				tyinfo->rolname, tyinfo->typacl, tyinfo->rtypacl,
				tyinfo->inittypacl, tyinfo->initrtypacl);

	PQclear(res);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(query);
	free(qtypname);
	free(qualtypname);
}

/*
 * dumpDomain
 *	  writes out to fout the queries to recreate a user-defined domain
 */
static void
dumpDomain(Archive *fout, TypeInfo *tyinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;
	int			i;
	char	   *qtypname;
	char	   *qualtypname;
	char	   *typnotnull;
	char	   *typdefn;
	char	   *typdefault;
	Oid			typcollation;
	bool		typdefault_is_literal = false;

	/* Fetch domain specific details */
	if (fout->remoteVersion >= 90100)
	{
		/* typcollation is new in 9.1 */
		appendPQExpBuffer(query, "SELECT t.typnotnull, "
						  "pg_catalog.format_type(t.typbasetype, t.typtypmod) AS typdefn, "
						  "pg_catalog.pg_get_expr(t.typdefaultbin, 'pg_catalog.pg_type'::pg_catalog.regclass) AS typdefaultbin, "
						  "t.typdefault, "
						  "CASE WHEN t.typcollation <> u.typcollation "
						  "THEN t.typcollation ELSE 0 END AS typcollation "
						  "FROM pg_catalog.pg_type t "
						  "LEFT JOIN pg_catalog.pg_type u ON (t.typbasetype = u.oid) "
						  "WHERE t.oid = '%u'::pg_catalog.oid",
						  tyinfo->dobj.catId.oid);
	}
	else
	{
		appendPQExpBuffer(query, "SELECT typnotnull, "
						  "pg_catalog.format_type(typbasetype, typtypmod) AS typdefn, "
						  "pg_catalog.pg_get_expr(typdefaultbin, 'pg_catalog.pg_type'::pg_catalog.regclass) AS typdefaultbin, "
						  "typdefault, 0 AS typcollation "
						  "FROM pg_catalog.pg_type "
						  "WHERE oid = '%u'::pg_catalog.oid",
						  tyinfo->dobj.catId.oid);
	}

	res = ExecuteSqlQueryForSingleRow(fout, query->data);

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
	typcollation = atooid(PQgetvalue(res, 0, PQfnumber(res, "typcollation")));

	if (dopt->binary_upgrade)
		binary_upgrade_set_type_oids_by_type_oid(fout, q,
												 tyinfo->dobj.catId.oid,
												 true); /* force array type */

	qtypname = pg_strdup(fmtId(tyinfo->dobj.name));
	qualtypname = pg_strdup(fmtQualifiedDumpable(tyinfo));

	appendPQExpBuffer(q,
					  "CREATE DOMAIN %s AS %s",
					  qualtypname,
					  typdefn);

	/* Print collation only if different from base type's collation */
	if (OidIsValid(typcollation))
	{
		CollInfo   *coll;

		coll = findCollationByOid(typcollation);
		if (coll)
			appendPQExpBuffer(q, " COLLATE %s", fmtQualifiedDumpable(coll));
	}

	if (typnotnull[0] == 't')
		appendPQExpBufferStr(q, " NOT NULL");

	if (typdefault != NULL)
	{
		appendPQExpBufferStr(q, " DEFAULT ");
		if (typdefault_is_literal)
			appendStringLiteralAH(q, typdefault, fout);
		else
			appendPQExpBufferStr(q, typdefault);
	}

	PQclear(res);

	/*
	 * Add any CHECK constraints for the domain
	 */
	for (i = 0; i < tyinfo->nDomChecks; i++)
	{
		ConstraintInfo *domcheck = &(tyinfo->domChecks[i]);

		if (!domcheck->separate)
			appendPQExpBuffer(q, "\n\tCONSTRAINT %s %s",
							  fmtId(domcheck->dobj.name), domcheck->condef);
	}

	appendPQExpBufferStr(q, ";\n");

	appendPQExpBuffer(delq, "DROP DOMAIN %s;\n", qualtypname);

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &tyinfo->dobj,
										"DOMAIN", qtypname,
										tyinfo->dobj.namespace->dobj.name);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, tyinfo->dobj.catId, tyinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tyinfo->dobj.name,
								  .namespace = tyinfo->dobj.namespace->dobj.name,
								  .owner = tyinfo->rolname,
								  .description = "DOMAIN",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* Dump Domain Comments and Security Labels */
	if (tyinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "DOMAIN", qtypname,
					tyinfo->dobj.namespace->dobj.name, tyinfo->rolname,
					tyinfo->dobj.catId, 0, tyinfo->dobj.dumpId);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpSecLabel(fout, "DOMAIN", qtypname,
					 tyinfo->dobj.namespace->dobj.name, tyinfo->rolname,
					 tyinfo->dobj.catId, 0, tyinfo->dobj.dumpId);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_ACL)
		dumpACL(fout, tyinfo->dobj.dumpId, InvalidDumpId, "TYPE",
				qtypname, NULL,
				tyinfo->dobj.namespace->dobj.name,
				tyinfo->rolname, tyinfo->typacl, tyinfo->rtypacl,
				tyinfo->inittypacl, tyinfo->initrtypacl);

	/* Dump any per-constraint comments */
	for (i = 0; i < tyinfo->nDomChecks; i++)
	{
		ConstraintInfo *domcheck = &(tyinfo->domChecks[i]);
		PQExpBuffer conprefix = createPQExpBuffer();

		appendPQExpBuffer(conprefix, "CONSTRAINT %s ON DOMAIN",
						  fmtId(domcheck->dobj.name));

		if (tyinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
			dumpComment(fout, conprefix->data, qtypname,
						tyinfo->dobj.namespace->dobj.name,
						tyinfo->rolname,
						domcheck->dobj.catId, 0, tyinfo->dobj.dumpId);

		destroyPQExpBuffer(conprefix);
	}

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(query);
	free(qtypname);
	free(qualtypname);
}

/*
 * dumpCompositeType
 *	  writes out to fout the queries to recreate a user-defined stand-alone
 *	  composite type
 */
static void
dumpCompositeType(Archive *fout, TypeInfo *tyinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer dropped = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;
	char	   *qtypname;
	char	   *qualtypname;
	int			ntups;
	int			i_attname;
	int			i_atttypdefn;
	int			i_attlen;
	int			i_attalign;
	int			i_attisdropped;
	int			i_attcollation;
	int			i;
	int			actual_atts;

	/* Fetch type specific details */
	if (fout->remoteVersion >= 90100)
	{
		/*
		 * attcollation is new in 9.1.  Since we only want to dump COLLATE
		 * clauses for attributes whose collation is different from their
		 * type's default, we use a CASE here to suppress uninteresting
		 * attcollations cheaply.  atttypid will be 0 for dropped columns;
		 * collation does not matter for those.
		 */
		appendPQExpBuffer(query, "SELECT a.attname, "
						  "pg_catalog.format_type(a.atttypid, a.atttypmod) AS atttypdefn, "
						  "a.attlen, a.attalign, a.attisdropped, "
						  "CASE WHEN a.attcollation <> at.typcollation "
						  "THEN a.attcollation ELSE 0 END AS attcollation "
						  "FROM pg_catalog.pg_type ct "
						  "JOIN pg_catalog.pg_attribute a ON a.attrelid = ct.typrelid "
						  "LEFT JOIN pg_catalog.pg_type at ON at.oid = a.atttypid "
						  "WHERE ct.oid = '%u'::pg_catalog.oid "
						  "ORDER BY a.attnum ",
						  tyinfo->dobj.catId.oid);
	}
	else
	{
		/*
		 * Since ALTER TYPE could not drop columns until 9.1, attisdropped
		 * should always be false.
		 */
		appendPQExpBuffer(query, "SELECT a.attname, "
						  "pg_catalog.format_type(a.atttypid, a.atttypmod) AS atttypdefn, "
						  "a.attlen, a.attalign, a.attisdropped, "
						  "0 AS attcollation "
						  "FROM pg_catalog.pg_type ct, pg_catalog.pg_attribute a "
						  "WHERE ct.oid = '%u'::pg_catalog.oid "
						  "AND a.attrelid = ct.typrelid "
						  "ORDER BY a.attnum ",
						  tyinfo->dobj.catId.oid);
	}

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_attname = PQfnumber(res, "attname");
	i_atttypdefn = PQfnumber(res, "atttypdefn");
	i_attlen = PQfnumber(res, "attlen");
	i_attalign = PQfnumber(res, "attalign");
	i_attisdropped = PQfnumber(res, "attisdropped");
	i_attcollation = PQfnumber(res, "attcollation");

	if (dopt->binary_upgrade)
	{
		binary_upgrade_set_type_oids_by_type_oid(fout, q,
												 tyinfo->dobj.catId.oid,
												 false);
		binary_upgrade_set_pg_class_oids(fout, q, tyinfo->typrelid, false);
	}

	qtypname = pg_strdup(fmtId(tyinfo->dobj.name));
	qualtypname = pg_strdup(fmtQualifiedDumpable(tyinfo));

	appendPQExpBuffer(q, "CREATE TYPE %s AS (",
					  qualtypname);

	actual_atts = 0;
	for (i = 0; i < ntups; i++)
	{
		char	   *attname;
		char	   *atttypdefn;
		char	   *attlen;
		char	   *attalign;
		bool		attisdropped;
		Oid			attcollation;

		attname = PQgetvalue(res, i, i_attname);
		atttypdefn = PQgetvalue(res, i, i_atttypdefn);
		attlen = PQgetvalue(res, i, i_attlen);
		attalign = PQgetvalue(res, i, i_attalign);
		attisdropped = (PQgetvalue(res, i, i_attisdropped)[0] == 't');
		attcollation = atooid(PQgetvalue(res, i, i_attcollation));

		if (attisdropped && !dopt->binary_upgrade)
			continue;

		/* Format properly if not first attr */
		if (actual_atts++ > 0)
			appendPQExpBufferChar(q, ',');
		appendPQExpBufferStr(q, "\n\t");

		if (!attisdropped)
		{
			appendPQExpBuffer(q, "%s %s", fmtId(attname), atttypdefn);

			/* Add collation if not default for the column type */
			if (OidIsValid(attcollation))
			{
				CollInfo   *coll;

				coll = findCollationByOid(attcollation);
				if (coll)
					appendPQExpBuffer(q, " COLLATE %s",
									  fmtQualifiedDumpable(coll));
			}
		}
		else
		{
			/*
			 * This is a dropped attribute and we're in binary_upgrade mode.
			 * Insert a placeholder for it in the CREATE TYPE command, and set
			 * length and alignment with direct UPDATE to the catalogs
			 * afterwards. See similar code in dumpTableSchema().
			 */
			appendPQExpBuffer(q, "%s INTEGER /* dummy */", fmtId(attname));

			/* stash separately for insertion after the CREATE TYPE */
			appendPQExpBufferStr(dropped,
								 "\n-- For binary upgrade, recreate dropped column.\n");
			appendPQExpBuffer(dropped, "UPDATE pg_catalog.pg_attribute\n"
							  "SET attlen = %s, "
							  "attalign = '%s', attbyval = false\n"
							  "WHERE attname = ", attlen, attalign);
			appendStringLiteralAH(dropped, attname, fout);
			appendPQExpBufferStr(dropped, "\n  AND attrelid = ");
			appendStringLiteralAH(dropped, qualtypname, fout);
			appendPQExpBufferStr(dropped, "::pg_catalog.regclass;\n");

			appendPQExpBuffer(dropped, "ALTER TYPE %s ",
							  qualtypname);
			appendPQExpBuffer(dropped, "DROP ATTRIBUTE %s;\n",
							  fmtId(attname));
		}
	}
	appendPQExpBufferStr(q, "\n);\n");
	appendPQExpBufferStr(q, dropped->data);

	appendPQExpBuffer(delq, "DROP TYPE %s;\n", qualtypname);

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &tyinfo->dobj,
										"TYPE", qtypname,
										tyinfo->dobj.namespace->dobj.name);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, tyinfo->dobj.catId, tyinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tyinfo->dobj.name,
								  .namespace = tyinfo->dobj.namespace->dobj.name,
								  .owner = tyinfo->rolname,
								  .description = "TYPE",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));


	/* Dump Type Comments and Security Labels */
	if (tyinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "TYPE", qtypname,
					tyinfo->dobj.namespace->dobj.name, tyinfo->rolname,
					tyinfo->dobj.catId, 0, tyinfo->dobj.dumpId);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpSecLabel(fout, "TYPE", qtypname,
					 tyinfo->dobj.namespace->dobj.name, tyinfo->rolname,
					 tyinfo->dobj.catId, 0, tyinfo->dobj.dumpId);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_ACL)
		dumpACL(fout, tyinfo->dobj.dumpId, InvalidDumpId, "TYPE",
				qtypname, NULL,
				tyinfo->dobj.namespace->dobj.name,
				tyinfo->rolname, tyinfo->typacl, tyinfo->rtypacl,
				tyinfo->inittypacl, tyinfo->initrtypacl);

	PQclear(res);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(dropped);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(query);
	free(qtypname);
	free(qualtypname);

	/* Dump any per-column comments */
	if (tyinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpCompositeTypeColComments(fout, tyinfo);
}

/*
 * dumpCompositeTypeColComments
 *	  writes out to fout the queries to recreate comments on the columns of
 *	  a user-defined stand-alone composite type
 */
static void
dumpCompositeTypeColComments(Archive *fout, TypeInfo *tyinfo)
{
	CommentItem *comments;
	int			ncomments;
	PGresult   *res;
	PQExpBuffer query;
	PQExpBuffer target;
	Oid			pgClassOid;
	int			i;
	int			ntups;
	int			i_attname;
	int			i_attnum;

	/* do nothing, if --no-comments is supplied */
	if (fout->dopt->no_comments)
		return;

	query = createPQExpBuffer();

	appendPQExpBuffer(query,
					  "SELECT c.tableoid, a.attname, a.attnum "
					  "FROM pg_catalog.pg_class c, pg_catalog.pg_attribute a "
					  "WHERE c.oid = '%u' AND c.oid = a.attrelid "
					  "  AND NOT a.attisdropped "
					  "ORDER BY a.attnum ",
					  tyinfo->typrelid);

	/* Fetch column attnames */
	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	if (ntups < 1)
	{
		PQclear(res);
		destroyPQExpBuffer(query);
		return;
	}

	pgClassOid = atooid(PQgetvalue(res, 0, PQfnumber(res, "tableoid")));

	/* Search for comments associated with type's pg_class OID */
	ncomments = findComments(fout,
							 pgClassOid,
							 tyinfo->typrelid,
							 &comments);

	/* If no comments exist, we're done */
	if (ncomments <= 0)
	{
		PQclear(res);
		destroyPQExpBuffer(query);
		return;
	}

	/* Build COMMENT ON statements */
	target = createPQExpBuffer();

	i_attnum = PQfnumber(res, "attnum");
	i_attname = PQfnumber(res, "attname");
	while (ncomments > 0)
	{
		const char *attname;

		attname = NULL;
		for (i = 0; i < ntups; i++)
		{
			if (atoi(PQgetvalue(res, i, i_attnum)) == comments->objsubid)
			{
				attname = PQgetvalue(res, i, i_attname);
				break;
			}
		}
		if (attname)			/* just in case we don't find it */
		{
			const char *descr = comments->descr;

			resetPQExpBuffer(target);
			appendPQExpBuffer(target, "COLUMN %s.",
							  fmtId(tyinfo->dobj.name));
			appendPQExpBufferStr(target, fmtId(attname));

			resetPQExpBuffer(query);
			appendPQExpBuffer(query, "COMMENT ON COLUMN %s.",
							  fmtQualifiedDumpable(tyinfo));
			appendPQExpBuffer(query, "%s IS ", fmtId(attname));
			appendStringLiteralAH(query, descr, fout);
			appendPQExpBufferStr(query, ";\n");

			ArchiveEntry(fout, nilCatalogId, createDumpId(),
						 ARCHIVE_OPTS(.tag = target->data,
									  .namespace = tyinfo->dobj.namespace->dobj.name,
									  .owner = tyinfo->rolname,
									  .description = "COMMENT",
									  .section = SECTION_NONE,
									  .createStmt = query->data,
									  .deps = &(tyinfo->dobj.dumpId),
									  .nDeps = 1));
		}

		comments++;
		ncomments--;
	}

	PQclear(res);
	destroyPQExpBuffer(query);
	destroyPQExpBuffer(target);
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
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;

	/* Skip if not to be dumped */
	if (!stinfo->dobj.dump || dopt->dataOnly)
		return;

	q = createPQExpBuffer();

	/*
	 * Note the lack of a DROP command for the shell type; any required DROP
	 * is driven off the base type entry, instead.  This interacts with
	 * _printTocEntry()'s use of the presence of a DROP command to decide
	 * whether an entry needs an ALTER OWNER command.  We don't want to alter
	 * the shell type's owner immediately on creation; that should happen only
	 * after it's filled in, otherwise the backend complains.
	 */

	if (dopt->binary_upgrade)
		binary_upgrade_set_type_oids_by_type_oid(fout, q,
												 stinfo->baseType->dobj.catId.oid,
												 false);

	appendPQExpBuffer(q, "CREATE TYPE %s;\n",
					  fmtQualifiedDumpable(stinfo));

	if (stinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, stinfo->dobj.catId, stinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = stinfo->dobj.name,
								  .namespace = stinfo->dobj.namespace->dobj.name,
								  .owner = stinfo->baseType->rolname,
								  .description = "SHELL TYPE",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data));

	destroyPQExpBuffer(q);
}

/*
 * dumpProcLang
 *		  writes out to fout the queries to recreate a user-defined
 *		  procedural language
 */
static void
dumpProcLang(Archive *fout, ProcLangInfo *plang)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer defqry;
	PQExpBuffer delqry;
	bool		useParams;
	char	   *qlanname;
	FuncInfo   *funcInfo;
	FuncInfo   *inlineInfo = NULL;
	FuncInfo   *validatorInfo = NULL;

	/* Skip if not to be dumped */
	if (!plang->dobj.dump || dopt->dataOnly)
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

	if (OidIsValid(plang->laninline))
	{
		inlineInfo = findFuncByOid(plang->laninline);
		if (inlineInfo != NULL && !inlineInfo->dobj.dump)
			inlineInfo = NULL;
	}

	if (OidIsValid(plang->lanvalidator))
	{
		validatorInfo = findFuncByOid(plang->lanvalidator);
		if (validatorInfo != NULL && !validatorInfo->dobj.dump)
			validatorInfo = NULL;
	}

	/*
	 * If the functions are dumpable then emit a complete CREATE LANGUAGE with
	 * parameters.  Otherwise, we'll write a parameterless command, which will
	 * be interpreted as CREATE EXTENSION.
	 */
	useParams = (funcInfo != NULL &&
				 (inlineInfo != NULL || !OidIsValid(plang->laninline)) &&
				 (validatorInfo != NULL || !OidIsValid(plang->lanvalidator)));

	defqry = createPQExpBuffer();
	delqry = createPQExpBuffer();

	qlanname = pg_strdup(fmtId(plang->dobj.name));

	appendPQExpBuffer(delqry, "DROP PROCEDURAL LANGUAGE %s;\n",
					  qlanname);

	if (useParams)
	{
		appendPQExpBuffer(defqry, "CREATE %sPROCEDURAL LANGUAGE %s",
						  plang->lanpltrusted ? "TRUSTED " : "",
						  qlanname);
		appendPQExpBuffer(defqry, " HANDLER %s",
						  fmtQualifiedDumpable(funcInfo));
		if (OidIsValid(plang->laninline))
			appendPQExpBuffer(defqry, " INLINE %s",
							  fmtQualifiedDumpable(inlineInfo));
		if (OidIsValid(plang->lanvalidator))
			appendPQExpBuffer(defqry, " VALIDATOR %s",
							  fmtQualifiedDumpable(validatorInfo));
	}
	else
	{
		/*
		 * If not dumping parameters, then use CREATE OR REPLACE so that the
		 * command will not fail if the language is preinstalled in the target
		 * database.
		 *
		 * Modern servers will interpret this as CREATE EXTENSION IF NOT
		 * EXISTS; perhaps we should emit that instead?  But it might just add
		 * confusion.
		 */
		appendPQExpBuffer(defqry, "CREATE OR REPLACE PROCEDURAL LANGUAGE %s",
						  qlanname);
	}
	appendPQExpBufferStr(defqry, ";\n");

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(defqry, &plang->dobj,
										"LANGUAGE", qlanname, NULL);

	if (plang->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, plang->dobj.catId, plang->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = plang->dobj.name,
								  .owner = plang->lanowner,
								  .description = "PROCEDURAL LANGUAGE",
								  .section = SECTION_PRE_DATA,
								  .createStmt = defqry->data,
								  .dropStmt = delqry->data,
								  ));

	/* Dump Proc Lang Comments and Security Labels */
	if (plang->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "LANGUAGE", qlanname,
					NULL, plang->lanowner,
					plang->dobj.catId, 0, plang->dobj.dumpId);

	if (plang->dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpSecLabel(fout, "LANGUAGE", qlanname,
					 NULL, plang->lanowner,
					 plang->dobj.catId, 0, plang->dobj.dumpId);

	if (plang->lanpltrusted && plang->dobj.dump & DUMP_COMPONENT_ACL)
		dumpACL(fout, plang->dobj.dumpId, InvalidDumpId, "LANGUAGE",
				qlanname, NULL, NULL,
				plang->lanowner, plang->lanacl, plang->rlanacl,
				plang->initlanacl, plang->initrlanacl);

	free(qlanname);

	destroyPQExpBuffer(defqry);
	destroyPQExpBuffer(delqry);
}

/*
 * format_function_arguments: generate function name and argument list
 *
 * This is used when we can rely on pg_get_function_arguments to format
 * the argument list.  Note, however, that pg_get_function_arguments
 * does not special-case zero-argument aggregates.
 */
static char *
format_function_arguments(FuncInfo *finfo, char *funcargs, bool is_agg)
{
	PQExpBufferData fn;

	initPQExpBuffer(&fn);
	appendPQExpBufferStr(&fn, fmtId(finfo->dobj.name));
	if (is_agg && finfo->nargs == 0)
		appendPQExpBufferStr(&fn, "(*)");
	else
		appendPQExpBuffer(&fn, "(%s)", funcargs);
	return fn.data;
}

/*
 * format_function_arguments_old: generate function name and argument list
 *
 * The argument type names are qualified if needed.  The function name
 * is never qualified.
 *
 * This is used only with pre-8.4 servers, so we aren't expecting to see
 * VARIADIC or TABLE arguments, nor are there any defaults for arguments.
 *
 * Any or all of allargtypes, argmodes, argnames may be NULL.
 */
static char *
format_function_arguments_old(Archive *fout,
							  FuncInfo *finfo, int nallargs,
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
		typname = getFormattedTypeName(fout, typid, zeroIsError);

		if (argmodes)
		{
			switch (argmodes[j][0])
			{
				case PROARGMODE_IN:
					argmode = "";
					break;
				case PROARGMODE_OUT:
					argmode = "OUT ";
					break;
				case PROARGMODE_INOUT:
					argmode = "INOUT ";
					break;
				default:
					pg_log_warning("bogus value in proargmodes array");
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
	appendPQExpBufferChar(&fn, ')');
	return fn.data;
}

/*
 * format_function_signature: generate function name and argument list
 *
 * This is like format_function_arguments_old except that only a minimal
 * list of input argument types is generated; this is sufficient to
 * reference the function, but not to define it.
 *
 * If honor_quotes is false then the function name is never quoted.
 * This is appropriate for use in TOC tags, but not in SQL commands.
 */
static char *
format_function_signature(Archive *fout, FuncInfo *finfo, bool honor_quotes)
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

		if (j > 0)
			appendPQExpBufferStr(&fn, ", ");

		typname = getFormattedTypeName(fout, finfo->argtypes[j],
									   zeroIsError);
		appendPQExpBufferStr(&fn, typname);
		free(typname);
	}
	appendPQExpBufferChar(&fn, ')');
	return fn.data;
}


/*
 * dumpFunc:
 *	  dump out one function
 */
static void
dumpFunc(Archive *fout, FuncInfo *finfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer query;
	PQExpBuffer q;
	PQExpBuffer delqry;
	PQExpBuffer asPart;
	PGresult   *res;
	char	   *funcsig;		/* identity signature */
	char	   *funcfullsig = NULL; /* full signature */
	char	   *funcsig_tag;
	char	   *proretset;
	char	   *prosrc;
	char	   *probin;
	char	   *funcargs;
	char	   *funciargs;
	char	   *funcresult;
	char	   *proallargtypes;
	char	   *proargmodes;
	char	   *proargnames;
	char	   *protrftypes;
	char	   *prokind;
	char	   *provolatile;
	char	   *proisstrict;
	char	   *prosecdef;
	char	   *proleakproof;
	char	   *proconfig;
	char	   *procost;
	char	   *prorows;
	char	   *prosupport;
	char	   *proparallel;
	char	   *lanname;
	char	   *rettypename;
	int			nallargs;
	char	  **allargtypes = NULL;
	char	  **argmodes = NULL;
	char	  **argnames = NULL;
	char	  **configitems = NULL;
	int			nconfigitems = 0;
	const char *keyword;
	int			i;

	/* Skip if not to be dumped */
	if (!finfo->dobj.dump || dopt->dataOnly)
		return;

	query = createPQExpBuffer();
	q = createPQExpBuffer();
	delqry = createPQExpBuffer();
	asPart = createPQExpBuffer();

	/* Fetch function-specific details */
	appendPQExpBufferStr(query,
						 "SELECT\n"
						 "proretset,\n"
						 "prosrc,\n"
						 "probin,\n"
						 "provolatile,\n"
						 "proisstrict,\n"
						 "prosecdef,\n"
						 "(SELECT lanname FROM pg_catalog.pg_language WHERE oid = prolang) AS lanname,\n");

	if (fout->remoteVersion >= 80300)
		appendPQExpBufferStr(query,
							 "proconfig,\n"
							 "procost,\n"
							 "prorows,\n");
	else
		appendPQExpBufferStr(query,
							 "null AS proconfig,\n"
							 "0 AS procost,\n"
							 "0 AS prorows,\n");

	if (fout->remoteVersion >= 80400)
	{
		/*
		 * In 8.4 and up we rely on pg_get_function_arguments and
		 * pg_get_function_result instead of examining proallargtypes etc.
		 */
		appendPQExpBufferStr(query,
							 "pg_catalog.pg_get_function_arguments(oid) AS funcargs,\n"
							 "pg_catalog.pg_get_function_identity_arguments(oid) AS funciargs,\n"
							 "pg_catalog.pg_get_function_result(oid) AS funcresult,\n");
	}
	else if (fout->remoteVersion >= 80100)
		appendPQExpBufferStr(query,
							 "proallargtypes,\n"
							 "proargmodes,\n"
							 "proargnames,\n");
	else
		appendPQExpBufferStr(query,
							 "null AS proallargtypes,\n"
							 "null AS proargmodes,\n"
							 "proargnames,\n");

	if (fout->remoteVersion >= 90200)
		appendPQExpBufferStr(query,
							 "proleakproof,\n");
	else
		appendPQExpBufferStr(query,
							 "false AS proleakproof,\n");

	if (fout->remoteVersion >= 90500)
		appendPQExpBufferStr(query,
							 "array_to_string(protrftypes, ' ') AS protrftypes,\n");

	if (fout->remoteVersion >= 90600)
		appendPQExpBufferStr(query,
							 "proparallel,\n");
	else
		appendPQExpBufferStr(query,
							 "'u' AS proparallel,\n");

	if (fout->remoteVersion >= 110000)
		appendPQExpBufferStr(query,
							 "prokind,\n");
	else if (fout->remoteVersion >= 80400)
		appendPQExpBufferStr(query,
							 "CASE WHEN proiswindow THEN 'w' ELSE 'f' END AS prokind,\n");
	else
		appendPQExpBufferStr(query,
							 "'f' AS prokind,\n");

	if (fout->remoteVersion >= 120000)
		appendPQExpBufferStr(query,
							 "prosupport\n");
	else
		appendPQExpBufferStr(query,
							 "'-' AS prosupport\n");

	appendPQExpBuffer(query,
					  "FROM pg_catalog.pg_proc "
					  "WHERE oid = '%u'::pg_catalog.oid",
					  finfo->dobj.catId.oid);

	res = ExecuteSqlQueryForSingleRow(fout, query->data);

	proretset = PQgetvalue(res, 0, PQfnumber(res, "proretset"));
	prosrc = PQgetvalue(res, 0, PQfnumber(res, "prosrc"));
	probin = PQgetvalue(res, 0, PQfnumber(res, "probin"));
	if (fout->remoteVersion >= 80400)
	{
		funcargs = PQgetvalue(res, 0, PQfnumber(res, "funcargs"));
		funciargs = PQgetvalue(res, 0, PQfnumber(res, "funciargs"));
		funcresult = PQgetvalue(res, 0, PQfnumber(res, "funcresult"));
		proallargtypes = proargmodes = proargnames = NULL;
	}
	else
	{
		proallargtypes = PQgetvalue(res, 0, PQfnumber(res, "proallargtypes"));
		proargmodes = PQgetvalue(res, 0, PQfnumber(res, "proargmodes"));
		proargnames = PQgetvalue(res, 0, PQfnumber(res, "proargnames"));
		funcargs = funciargs = funcresult = NULL;
	}
	if (PQfnumber(res, "protrftypes") != -1)
		protrftypes = PQgetvalue(res, 0, PQfnumber(res, "protrftypes"));
	else
		protrftypes = NULL;
	prokind = PQgetvalue(res, 0, PQfnumber(res, "prokind"));
	provolatile = PQgetvalue(res, 0, PQfnumber(res, "provolatile"));
	proisstrict = PQgetvalue(res, 0, PQfnumber(res, "proisstrict"));
	prosecdef = PQgetvalue(res, 0, PQfnumber(res, "prosecdef"));
	proleakproof = PQgetvalue(res, 0, PQfnumber(res, "proleakproof"));
	proconfig = PQgetvalue(res, 0, PQfnumber(res, "proconfig"));
	procost = PQgetvalue(res, 0, PQfnumber(res, "procost"));
	prorows = PQgetvalue(res, 0, PQfnumber(res, "prorows"));
	prosupport = PQgetvalue(res, 0, PQfnumber(res, "prosupport"));
	proparallel = PQgetvalue(res, 0, PQfnumber(res, "proparallel"));
	lanname = PQgetvalue(res, 0, PQfnumber(res, "lanname"));

	/*
	 * See backend/commands/functioncmds.c for details of how the 'AS' clause
	 * is used.  In 8.4 and up, an unused probin is NULL (here ""); previous
	 * versions would set it to "-".  There are no known cases in which prosrc
	 * is unused, so the tests below for "-" are probably useless.
	 */
	if (probin[0] != '\0' && strcmp(probin, "-") != 0)
	{
		appendPQExpBufferStr(asPart, "AS ");
		appendStringLiteralAH(asPart, probin, fout);
		if (strcmp(prosrc, "-") != 0)
		{
			appendPQExpBufferStr(asPart, ", ");

			/*
			 * where we have bin, use dollar quoting if allowed and src
			 * contains quote or backslash; else use regular quoting.
			 */
			if (dopt->disable_dollar_quoting ||
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
			appendPQExpBufferStr(asPart, "AS ");
			/* with no bin, dollar quote src unconditionally if allowed */
			if (dopt->disable_dollar_quoting)
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
			pg_log_warning("could not parse proallargtypes array");
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
			pg_log_warning("could not parse proargmodes array");
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
			pg_log_warning("could not parse proargnames array");
			if (argnames)
				free(argnames);
			argnames = NULL;
		}
	}

	if (proconfig && *proconfig)
	{
		if (!parsePGArray(proconfig, &configitems, &nconfigitems))
			fatal("could not parse proconfig array");
	}
	else
	{
		configitems = NULL;
		nconfigitems = 0;
	}

	if (funcargs)
	{
		/* 8.4 or later; we rely on server-side code for most of the work */
		funcfullsig = format_function_arguments(finfo, funcargs, false);
		funcsig = format_function_arguments(finfo, funciargs, false);
	}
	else
		/* pre-8.4, do it ourselves */
		funcsig = format_function_arguments_old(fout,
												finfo, nallargs, allargtypes,
												argmodes, argnames);

	funcsig_tag = format_function_signature(fout, finfo, false);

	if (prokind[0] == PROKIND_PROCEDURE)
		keyword = "PROCEDURE";
	else
		keyword = "FUNCTION";	/* works for window functions too */

	appendPQExpBuffer(delqry, "DROP %s %s.%s;\n",
					  keyword,
					  fmtId(finfo->dobj.namespace->dobj.name),
					  funcsig);

	appendPQExpBuffer(q, "CREATE %s %s.%s",
					  keyword,
					  fmtId(finfo->dobj.namespace->dobj.name),
					  funcfullsig ? funcfullsig :
					  funcsig);

	if (prokind[0] == PROKIND_PROCEDURE)
		 /* no result type to output */ ;
	else if (funcresult)
		appendPQExpBuffer(q, " RETURNS %s", funcresult);
	else
	{
		rettypename = getFormattedTypeName(fout, finfo->prorettype,
										   zeroIsError);
		appendPQExpBuffer(q, " RETURNS %s%s",
						  (proretset[0] == 't') ? "SETOF " : "",
						  rettypename);
		free(rettypename);
	}

	appendPQExpBuffer(q, "\n    LANGUAGE %s", fmtId(lanname));

	if (protrftypes != NULL && strcmp(protrftypes, "") != 0)
	{
		Oid		   *typeids = palloc(FUNC_MAX_ARGS * sizeof(Oid));
		int			i;

		appendPQExpBufferStr(q, " TRANSFORM ");
		parseOidArray(protrftypes, typeids, FUNC_MAX_ARGS);
		for (i = 0; typeids[i]; i++)
		{
			if (i != 0)
				appendPQExpBufferStr(q, ", ");
			appendPQExpBuffer(q, "FOR TYPE %s",
							  getFormattedTypeName(fout, typeids[i], zeroAsNone));
		}
	}

	if (prokind[0] == PROKIND_WINDOW)
		appendPQExpBufferStr(q, " WINDOW");

	if (provolatile[0] != PROVOLATILE_VOLATILE)
	{
		if (provolatile[0] == PROVOLATILE_IMMUTABLE)
			appendPQExpBufferStr(q, " IMMUTABLE");
		else if (provolatile[0] == PROVOLATILE_STABLE)
			appendPQExpBufferStr(q, " STABLE");
		else if (provolatile[0] != PROVOLATILE_VOLATILE)
			fatal("unrecognized provolatile value for function \"%s\"",
				  finfo->dobj.name);
	}

	if (proisstrict[0] == 't')
		appendPQExpBufferStr(q, " STRICT");

	if (prosecdef[0] == 't')
		appendPQExpBufferStr(q, " SECURITY DEFINER");

	if (proleakproof[0] == 't')
		appendPQExpBufferStr(q, " LEAKPROOF");

	/*
	 * COST and ROWS are emitted only if present and not default, so as not to
	 * break backwards-compatibility of the dump without need.  Keep this code
	 * in sync with the defaults in functioncmds.c.
	 */
	if (strcmp(procost, "0") != 0)
	{
		if (strcmp(lanname, "internal") == 0 || strcmp(lanname, "c") == 0)
		{
			/* default cost is 1 */
			if (strcmp(procost, "1") != 0)
				appendPQExpBuffer(q, " COST %s", procost);
		}
		else
		{
			/* default cost is 100 */
			if (strcmp(procost, "100") != 0)
				appendPQExpBuffer(q, " COST %s", procost);
		}
	}
	if (proretset[0] == 't' &&
		strcmp(prorows, "0") != 0 && strcmp(prorows, "1000") != 0)
		appendPQExpBuffer(q, " ROWS %s", prorows);

	if (strcmp(prosupport, "-") != 0)
	{
		/* We rely on regprocout to provide quoting and qualification */
		appendPQExpBuffer(q, " SUPPORT %s", prosupport);
	}

	if (proparallel[0] != PROPARALLEL_UNSAFE)
	{
		if (proparallel[0] == PROPARALLEL_SAFE)
			appendPQExpBufferStr(q, " PARALLEL SAFE");
		else if (proparallel[0] == PROPARALLEL_RESTRICTED)
			appendPQExpBufferStr(q, " PARALLEL RESTRICTED");
		else if (proparallel[0] != PROPARALLEL_UNSAFE)
			fatal("unrecognized proparallel value for function \"%s\"",
				  finfo->dobj.name);
	}

	for (i = 0; i < nconfigitems; i++)
	{
		/* we feel free to scribble on configitems[] here */
		char	   *configitem = configitems[i];
		char	   *pos;

		pos = strchr(configitem, '=');
		if (pos == NULL)
			continue;
		*pos++ = '\0';
		appendPQExpBuffer(q, "\n    SET %s TO ", fmtId(configitem));

		/*
		 * Variables that are marked GUC_LIST_QUOTE were already fully quoted
		 * by flatten_set_variable_args() before they were put into the
		 * proconfig array.  However, because the quoting rules used there
		 * aren't exactly like SQL's, we have to break the list value apart
		 * and then quote the elements as string literals.  (The elements may
		 * be double-quoted as-is, but we can't just feed them to the SQL
		 * parser; it would do the wrong thing with elements that are
		 * zero-length or longer than NAMEDATALEN.)
		 *
		 * Variables that are not so marked should just be emitted as simple
		 * string literals.  If the variable is not known to
		 * variable_is_guc_list_quote(), we'll do that; this makes it unsafe
		 * to use GUC_LIST_QUOTE for extension variables.
		 */
		if (variable_is_guc_list_quote(configitem))
		{
			char	  **namelist;
			char	  **nameptr;

			/* Parse string into list of identifiers */
			/* this shouldn't fail really */
			if (SplitGUCList(pos, ',', &namelist))
			{
				for (nameptr = namelist; *nameptr; nameptr++)
				{
					if (nameptr != namelist)
						appendPQExpBufferStr(q, ", ");
					appendStringLiteralAH(q, *nameptr, fout);
				}
			}
			pg_free(namelist);
		}
		else
			appendStringLiteralAH(q, pos, fout);
	}

	appendPQExpBuffer(q, "\n    %s;\n", asPart->data);

	append_depends_on_extension(fout, q, &finfo->dobj,
								"pg_catalog.pg_proc", keyword,
								psprintf("%s.%s",
										 fmtId(finfo->dobj.namespace->dobj.name),
										 funcsig));

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &finfo->dobj,
										keyword, funcsig,
										finfo->dobj.namespace->dobj.name);

	if (finfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, finfo->dobj.catId, finfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = funcsig_tag,
								  .namespace = finfo->dobj.namespace->dobj.name,
								  .owner = finfo->rolname,
								  .description = keyword,
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delqry->data));

	/* Dump Function Comments and Security Labels */
	if (finfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, keyword, funcsig,
					finfo->dobj.namespace->dobj.name, finfo->rolname,
					finfo->dobj.catId, 0, finfo->dobj.dumpId);

	if (finfo->dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpSecLabel(fout, keyword, funcsig,
					 finfo->dobj.namespace->dobj.name, finfo->rolname,
					 finfo->dobj.catId, 0, finfo->dobj.dumpId);

	if (finfo->dobj.dump & DUMP_COMPONENT_ACL)
		dumpACL(fout, finfo->dobj.dumpId, InvalidDumpId, keyword,
				funcsig, NULL,
				finfo->dobj.namespace->dobj.name,
				finfo->rolname, finfo->proacl, finfo->rproacl,
				finfo->initproacl, finfo->initrproacl);

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delqry);
	destroyPQExpBuffer(asPart);
	free(funcsig);
	if (funcfullsig)
		free(funcfullsig);
	free(funcsig_tag);
	if (allargtypes)
		free(allargtypes);
	if (argmodes)
		free(argmodes);
	if (argnames)
		free(argnames);
	if (configitems)
		free(configitems);
}


/*
 * Dump a user-defined cast
 */
static void
dumpCast(Archive *fout, CastInfo *cast)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer defqry;
	PQExpBuffer delqry;
	PQExpBuffer labelq;
	PQExpBuffer castargs;
	FuncInfo   *funcInfo = NULL;
	char	   *sourceType;
	char	   *targetType;

	/* Skip if not to be dumped */
	if (!cast->dobj.dump || dopt->dataOnly)
		return;

	/* Cannot dump if we don't have the cast function's info */
	if (OidIsValid(cast->castfunc))
	{
		funcInfo = findFuncByOid(cast->castfunc);
		if (funcInfo == NULL)
			fatal("could not find function definition for function with OID %u",
				  cast->castfunc);
	}

	defqry = createPQExpBuffer();
	delqry = createPQExpBuffer();
	labelq = createPQExpBuffer();
	castargs = createPQExpBuffer();

	sourceType = getFormattedTypeName(fout, cast->castsource, zeroAsNone);
	targetType = getFormattedTypeName(fout, cast->casttarget, zeroAsNone);
	appendPQExpBuffer(delqry, "DROP CAST (%s AS %s);\n",
					  sourceType, targetType);

	appendPQExpBuffer(defqry, "CREATE CAST (%s AS %s) ",
					  sourceType, targetType);

	switch (cast->castmethod)
	{
		case COERCION_METHOD_BINARY:
			appendPQExpBufferStr(defqry, "WITHOUT FUNCTION");
			break;
		case COERCION_METHOD_INOUT:
			appendPQExpBufferStr(defqry, "WITH INOUT");
			break;
		case COERCION_METHOD_FUNCTION:
			if (funcInfo)
			{
				char	   *fsig = format_function_signature(fout, funcInfo, true);

				/*
				 * Always qualify the function name (format_function_signature
				 * won't qualify it).
				 */
				appendPQExpBuffer(defqry, "WITH FUNCTION %s.%s",
								  fmtId(funcInfo->dobj.namespace->dobj.name), fsig);
				free(fsig);
			}
			else
				pg_log_warning("bogus value in pg_cast.castfunc or pg_cast.castmethod field");
			break;
		default:
			pg_log_warning("bogus value in pg_cast.castmethod field");
	}

	if (cast->castcontext == 'a')
		appendPQExpBufferStr(defqry, " AS ASSIGNMENT");
	else if (cast->castcontext == 'i')
		appendPQExpBufferStr(defqry, " AS IMPLICIT");
	appendPQExpBufferStr(defqry, ";\n");

	appendPQExpBuffer(labelq, "CAST (%s AS %s)",
					  sourceType, targetType);

	appendPQExpBuffer(castargs, "(%s AS %s)",
					  sourceType, targetType);

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(defqry, &cast->dobj,
										"CAST", castargs->data, NULL);

	if (cast->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, cast->dobj.catId, cast->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = labelq->data,
								  .description = "CAST",
								  .section = SECTION_PRE_DATA,
								  .createStmt = defqry->data,
								  .dropStmt = delqry->data));

	/* Dump Cast Comments */
	if (cast->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "CAST", castargs->data,
					NULL, "",
					cast->dobj.catId, 0, cast->dobj.dumpId);

	free(sourceType);
	free(targetType);

	destroyPQExpBuffer(defqry);
	destroyPQExpBuffer(delqry);
	destroyPQExpBuffer(labelq);
	destroyPQExpBuffer(castargs);
}

/*
 * Dump a transform
 */
static void
dumpTransform(Archive *fout, TransformInfo *transform)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer defqry;
	PQExpBuffer delqry;
	PQExpBuffer labelq;
	PQExpBuffer transformargs;
	FuncInfo   *fromsqlFuncInfo = NULL;
	FuncInfo   *tosqlFuncInfo = NULL;
	char	   *lanname;
	char	   *transformType;

	/* Skip if not to be dumped */
	if (!transform->dobj.dump || dopt->dataOnly)
		return;

	/* Cannot dump if we don't have the transform functions' info */
	if (OidIsValid(transform->trffromsql))
	{
		fromsqlFuncInfo = findFuncByOid(transform->trffromsql);
		if (fromsqlFuncInfo == NULL)
			fatal("could not find function definition for function with OID %u",
				  transform->trffromsql);
	}
	if (OidIsValid(transform->trftosql))
	{
		tosqlFuncInfo = findFuncByOid(transform->trftosql);
		if (tosqlFuncInfo == NULL)
			fatal("could not find function definition for function with OID %u",
				  transform->trftosql);
	}

	defqry = createPQExpBuffer();
	delqry = createPQExpBuffer();
	labelq = createPQExpBuffer();
	transformargs = createPQExpBuffer();

	lanname = get_language_name(fout, transform->trflang);
	transformType = getFormattedTypeName(fout, transform->trftype, zeroAsNone);

	appendPQExpBuffer(delqry, "DROP TRANSFORM FOR %s LANGUAGE %s;\n",
					  transformType, lanname);

	appendPQExpBuffer(defqry, "CREATE TRANSFORM FOR %s LANGUAGE %s (",
					  transformType, lanname);

	if (!transform->trffromsql && !transform->trftosql)
		pg_log_warning("bogus transform definition, at least one of trffromsql and trftosql should be nonzero");

	if (transform->trffromsql)
	{
		if (fromsqlFuncInfo)
		{
			char	   *fsig = format_function_signature(fout, fromsqlFuncInfo, true);

			/*
			 * Always qualify the function name (format_function_signature
			 * won't qualify it).
			 */
			appendPQExpBuffer(defqry, "FROM SQL WITH FUNCTION %s.%s",
							  fmtId(fromsqlFuncInfo->dobj.namespace->dobj.name), fsig);
			free(fsig);
		}
		else
			pg_log_warning("bogus value in pg_transform.trffromsql field");
	}

	if (transform->trftosql)
	{
		if (transform->trffromsql)
			appendPQExpBufferStr(defqry, ", ");

		if (tosqlFuncInfo)
		{
			char	   *fsig = format_function_signature(fout, tosqlFuncInfo, true);

			/*
			 * Always qualify the function name (format_function_signature
			 * won't qualify it).
			 */
			appendPQExpBuffer(defqry, "TO SQL WITH FUNCTION %s.%s",
							  fmtId(tosqlFuncInfo->dobj.namespace->dobj.name), fsig);
			free(fsig);
		}
		else
			pg_log_warning("bogus value in pg_transform.trftosql field");
	}

	appendPQExpBufferStr(defqry, ");\n");

	appendPQExpBuffer(labelq, "TRANSFORM FOR %s LANGUAGE %s",
					  transformType, lanname);

	appendPQExpBuffer(transformargs, "FOR %s LANGUAGE %s",
					  transformType, lanname);

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(defqry, &transform->dobj,
										"TRANSFORM", transformargs->data, NULL);

	if (transform->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, transform->dobj.catId, transform->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = labelq->data,
								  .description = "TRANSFORM",
								  .section = SECTION_PRE_DATA,
								  .createStmt = defqry->data,
								  .dropStmt = delqry->data,
								  .deps = transform->dobj.dependencies,
								  .nDeps = transform->dobj.nDeps));

	/* Dump Transform Comments */
	if (transform->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "TRANSFORM", transformargs->data,
					NULL, "",
					transform->dobj.catId, 0, transform->dobj.dumpId);

	free(lanname);
	free(transformType);
	destroyPQExpBuffer(defqry);
	destroyPQExpBuffer(delqry);
	destroyPQExpBuffer(labelq);
	destroyPQExpBuffer(transformargs);
}


/*
 * dumpOpr
 *	  write out a single operator definition
 */
static void
dumpOpr(Archive *fout, OprInfo *oprinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer query;
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer oprid;
	PQExpBuffer details;
	PGresult   *res;
	int			i_oprkind;
	int			i_oprcode;
	int			i_oprleft;
	int			i_oprright;
	int			i_oprcom;
	int			i_oprnegate;
	int			i_oprrest;
	int			i_oprjoin;
	int			i_oprcanmerge;
	int			i_oprcanhash;
	char	   *oprkind;
	char	   *oprcode;
	char	   *oprleft;
	char	   *oprright;
	char	   *oprcom;
	char	   *oprnegate;
	char	   *oprrest;
	char	   *oprjoin;
	char	   *oprcanmerge;
	char	   *oprcanhash;
	char	   *oprregproc;
	char	   *oprref;

	/* Skip if not to be dumped */
	if (!oprinfo->dobj.dump || dopt->dataOnly)
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

	if (fout->remoteVersion >= 80300)
	{
		appendPQExpBuffer(query, "SELECT oprkind, "
						  "oprcode::pg_catalog.regprocedure, "
						  "oprleft::pg_catalog.regtype, "
						  "oprright::pg_catalog.regtype, "
						  "oprcom, "
						  "oprnegate, "
						  "oprrest::pg_catalog.regprocedure, "
						  "oprjoin::pg_catalog.regprocedure, "
						  "oprcanmerge, oprcanhash "
						  "FROM pg_catalog.pg_operator "
						  "WHERE oid = '%u'::pg_catalog.oid",
						  oprinfo->dobj.catId.oid);
	}
	else
	{
		appendPQExpBuffer(query, "SELECT oprkind, "
						  "oprcode::pg_catalog.regprocedure, "
						  "oprleft::pg_catalog.regtype, "
						  "oprright::pg_catalog.regtype, "
						  "oprcom, "
						  "oprnegate, "
						  "oprrest::pg_catalog.regprocedure, "
						  "oprjoin::pg_catalog.regprocedure, "
						  "(oprlsortop != 0) AS oprcanmerge, "
						  "oprcanhash "
						  "FROM pg_catalog.pg_operator "
						  "WHERE oid = '%u'::pg_catalog.oid",
						  oprinfo->dobj.catId.oid);
	}

	res = ExecuteSqlQueryForSingleRow(fout, query->data);

	i_oprkind = PQfnumber(res, "oprkind");
	i_oprcode = PQfnumber(res, "oprcode");
	i_oprleft = PQfnumber(res, "oprleft");
	i_oprright = PQfnumber(res, "oprright");
	i_oprcom = PQfnumber(res, "oprcom");
	i_oprnegate = PQfnumber(res, "oprnegate");
	i_oprrest = PQfnumber(res, "oprrest");
	i_oprjoin = PQfnumber(res, "oprjoin");
	i_oprcanmerge = PQfnumber(res, "oprcanmerge");
	i_oprcanhash = PQfnumber(res, "oprcanhash");

	oprkind = PQgetvalue(res, 0, i_oprkind);
	oprcode = PQgetvalue(res, 0, i_oprcode);
	oprleft = PQgetvalue(res, 0, i_oprleft);
	oprright = PQgetvalue(res, 0, i_oprright);
	oprcom = PQgetvalue(res, 0, i_oprcom);
	oprnegate = PQgetvalue(res, 0, i_oprnegate);
	oprrest = PQgetvalue(res, 0, i_oprrest);
	oprjoin = PQgetvalue(res, 0, i_oprjoin);
	oprcanmerge = PQgetvalue(res, 0, i_oprcanmerge);
	oprcanhash = PQgetvalue(res, 0, i_oprcanhash);

	/* In PG14 upwards postfix operator support does not exist anymore. */
	if (strcmp(oprkind, "r") == 0)
		pg_log_warning("postfix operators are not supported anymore (operator \"%s\")",
					   oprcode);

	oprregproc = convertRegProcReference(oprcode);
	if (oprregproc)
	{
		appendPQExpBuffer(details, "    FUNCTION = %s", oprregproc);
		free(oprregproc);
	}

	appendPQExpBuffer(oprid, "%s (",
					  oprinfo->dobj.name);

	/*
	 * right unary means there's a left arg and left unary means there's a
	 * right arg.  (Although the "r" case is dead code for PG14 and later,
	 * continue to support it in case we're dumping from an old server.)
	 */
	if (strcmp(oprkind, "r") == 0 ||
		strcmp(oprkind, "b") == 0)
	{
		appendPQExpBuffer(details, ",\n    LEFTARG = %s", oprleft);
		appendPQExpBufferStr(oprid, oprleft);
	}
	else
		appendPQExpBufferStr(oprid, "NONE");

	if (strcmp(oprkind, "l") == 0 ||
		strcmp(oprkind, "b") == 0)
	{
		appendPQExpBuffer(details, ",\n    RIGHTARG = %s", oprright);
		appendPQExpBuffer(oprid, ", %s)", oprright);
	}
	else
		appendPQExpBufferStr(oprid, ", NONE)");

	oprref = getFormattedOperatorName(oprcom);
	if (oprref)
	{
		appendPQExpBuffer(details, ",\n    COMMUTATOR = %s", oprref);
		free(oprref);
	}

	oprref = getFormattedOperatorName(oprnegate);
	if (oprref)
	{
		appendPQExpBuffer(details, ",\n    NEGATOR = %s", oprref);
		free(oprref);
	}

	if (strcmp(oprcanmerge, "t") == 0)
		appendPQExpBufferStr(details, ",\n    MERGES");

	if (strcmp(oprcanhash, "t") == 0)
		appendPQExpBufferStr(details, ",\n    HASHES");

	oprregproc = convertRegProcReference(oprrest);
	if (oprregproc)
	{
		appendPQExpBuffer(details, ",\n    RESTRICT = %s", oprregproc);
		free(oprregproc);
	}

	oprregproc = convertRegProcReference(oprjoin);
	if (oprregproc)
	{
		appendPQExpBuffer(details, ",\n    JOIN = %s", oprregproc);
		free(oprregproc);
	}

	appendPQExpBuffer(delq, "DROP OPERATOR %s.%s;\n",
					  fmtId(oprinfo->dobj.namespace->dobj.name),
					  oprid->data);

	appendPQExpBuffer(q, "CREATE OPERATOR %s.%s (\n%s\n);\n",
					  fmtId(oprinfo->dobj.namespace->dobj.name),
					  oprinfo->dobj.name, details->data);

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &oprinfo->dobj,
										"OPERATOR", oprid->data,
										oprinfo->dobj.namespace->dobj.name);

	if (oprinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, oprinfo->dobj.catId, oprinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = oprinfo->dobj.name,
								  .namespace = oprinfo->dobj.namespace->dobj.name,
								  .owner = oprinfo->rolname,
								  .description = "OPERATOR",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* Dump Operator Comments */
	if (oprinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "OPERATOR", oprid->data,
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
 * Returns allocated string of what to print, or NULL if function references
 * is InvalidOid. Returned string is expected to be free'd by the caller.
 *
 * The input is a REGPROCEDURE display; we have to strip the argument-types
 * part.
 */
static char *
convertRegProcReference(const char *proc)
{
	char	   *name;
	char	   *paren;
	bool		inquote;

	/* In all cases "-" means a null reference */
	if (strcmp(proc, "-") == 0)
		return NULL;

	name = pg_strdup(proc);
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

/*
 * getFormattedOperatorName - retrieve the operator name for the
 * given operator OID (presented in string form).
 *
 * Returns an allocated string, or NULL if the given OID is invalid.
 * Caller is responsible for free'ing result string.
 *
 * What we produce has the format "OPERATOR(schema.oprname)".  This is only
 * useful in commands where the operator's argument types can be inferred from
 * context.  We always schema-qualify the name, though.  The predecessor to
 * this code tried to skip the schema qualification if possible, but that led
 * to wrong results in corner cases, such as if an operator and its negator
 * are in different schemas.
 */
static char *
getFormattedOperatorName(const char *oproid)
{
	OprInfo    *oprInfo;

	/* In all cases "0" means a null reference */
	if (strcmp(oproid, "0") == 0)
		return NULL;

	oprInfo = findOprByOid(atooid(oproid));
	if (oprInfo == NULL)
	{
		pg_log_warning("could not find operator with OID %s",
					   oproid);
		return NULL;
	}

	return psprintf("OPERATOR(%s.%s)",
					fmtId(oprInfo->dobj.namespace->dobj.name),
					oprInfo->dobj.name);
}

/*
 * Convert a function OID obtained from pg_ts_parser or pg_ts_template
 *
 * It is sufficient to use REGPROC rather than REGPROCEDURE, since the
 * argument lists of these functions are predetermined.  Note that the
 * caller should ensure we are in the proper schema, because the results
 * are search path dependent!
 */
static char *
convertTSFunction(Archive *fout, Oid funcOid)
{
	char	   *result;
	char		query[128];
	PGresult   *res;

	snprintf(query, sizeof(query),
			 "SELECT '%u'::pg_catalog.regproc", funcOid);
	res = ExecuteSqlQueryForSingleRow(fout, query);

	result = pg_strdup(PQgetvalue(res, 0, 0));

	PQclear(res);

	return result;
}

/*
 * dumpAccessMethod
 *	  write out a single access method definition
 */
static void
dumpAccessMethod(Archive *fout, AccessMethodInfo *aminfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *qamname;

	/* Skip if not to be dumped */
	if (!aminfo->dobj.dump || dopt->dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	qamname = pg_strdup(fmtId(aminfo->dobj.name));

	appendPQExpBuffer(q, "CREATE ACCESS METHOD %s ", qamname);

	switch (aminfo->amtype)
	{
		case AMTYPE_INDEX:
			appendPQExpBufferStr(q, "TYPE INDEX ");
			break;
		case AMTYPE_TABLE:
			appendPQExpBufferStr(q, "TYPE TABLE ");
			break;
		default:
			pg_log_warning("invalid type \"%c\" of access method \"%s\"",
						   aminfo->amtype, qamname);
			destroyPQExpBuffer(q);
			destroyPQExpBuffer(delq);
			free(qamname);
			return;
	}

	appendPQExpBuffer(q, "HANDLER %s;\n", aminfo->amhandler);

	appendPQExpBuffer(delq, "DROP ACCESS METHOD %s;\n",
					  qamname);

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &aminfo->dobj,
										"ACCESS METHOD", qamname, NULL);

	if (aminfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, aminfo->dobj.catId, aminfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = aminfo->dobj.name,
								  .description = "ACCESS METHOD",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* Dump Access Method Comments */
	if (aminfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "ACCESS METHOD", qamname,
					NULL, "",
					aminfo->dobj.catId, 0, aminfo->dobj.dumpId);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	free(qamname);
}

/*
 * dumpOpclass
 *	  write out a single operator class definition
 */
static void
dumpOpclass(Archive *fout, OpclassInfo *opcinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer query;
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer nameusing;
	PGresult   *res;
	int			ntups;
	int			i_opcintype;
	int			i_opckeytype;
	int			i_opcdefault;
	int			i_opcfamily;
	int			i_opcfamilyname;
	int			i_opcfamilynsp;
	int			i_amname;
	int			i_amopstrategy;
	int			i_amopreqcheck;
	int			i_amopopr;
	int			i_sortfamily;
	int			i_sortfamilynsp;
	int			i_amprocnum;
	int			i_amproc;
	int			i_amproclefttype;
	int			i_amprocrighttype;
	char	   *opcintype;
	char	   *opckeytype;
	char	   *opcdefault;
	char	   *opcfamily;
	char	   *opcfamilyname;
	char	   *opcfamilynsp;
	char	   *amname;
	char	   *amopstrategy;
	char	   *amopreqcheck;
	char	   *amopopr;
	char	   *sortfamily;
	char	   *sortfamilynsp;
	char	   *amprocnum;
	char	   *amproc;
	char	   *amproclefttype;
	char	   *amprocrighttype;
	bool		needComma;
	int			i;

	/* Skip if not to be dumped */
	if (!opcinfo->dobj.dump || dopt->dataOnly)
		return;

	query = createPQExpBuffer();
	q = createPQExpBuffer();
	delq = createPQExpBuffer();
	nameusing = createPQExpBuffer();

	/* Get additional fields from the pg_opclass row */
	if (fout->remoteVersion >= 80300)
	{
		appendPQExpBuffer(query, "SELECT opcintype::pg_catalog.regtype, "
						  "opckeytype::pg_catalog.regtype, "
						  "opcdefault, opcfamily, "
						  "opfname AS opcfamilyname, "
						  "nspname AS opcfamilynsp, "
						  "(SELECT amname FROM pg_catalog.pg_am WHERE oid = opcmethod) AS amname "
						  "FROM pg_catalog.pg_opclass c "
						  "LEFT JOIN pg_catalog.pg_opfamily f ON f.oid = opcfamily "
						  "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = opfnamespace "
						  "WHERE c.oid = '%u'::pg_catalog.oid",
						  opcinfo->dobj.catId.oid);
	}
	else
	{
		appendPQExpBuffer(query, "SELECT opcintype::pg_catalog.regtype, "
						  "opckeytype::pg_catalog.regtype, "
						  "opcdefault, NULL AS opcfamily, "
						  "NULL AS opcfamilyname, "
						  "NULL AS opcfamilynsp, "
						  "(SELECT amname FROM pg_catalog.pg_am WHERE oid = opcamid) AS amname "
						  "FROM pg_catalog.pg_opclass "
						  "WHERE oid = '%u'::pg_catalog.oid",
						  opcinfo->dobj.catId.oid);
	}

	res = ExecuteSqlQueryForSingleRow(fout, query->data);

	i_opcintype = PQfnumber(res, "opcintype");
	i_opckeytype = PQfnumber(res, "opckeytype");
	i_opcdefault = PQfnumber(res, "opcdefault");
	i_opcfamily = PQfnumber(res, "opcfamily");
	i_opcfamilyname = PQfnumber(res, "opcfamilyname");
	i_opcfamilynsp = PQfnumber(res, "opcfamilynsp");
	i_amname = PQfnumber(res, "amname");

	/* opcintype may still be needed after we PQclear res */
	opcintype = pg_strdup(PQgetvalue(res, 0, i_opcintype));
	opckeytype = PQgetvalue(res, 0, i_opckeytype);
	opcdefault = PQgetvalue(res, 0, i_opcdefault);
	/* opcfamily will still be needed after we PQclear res */
	opcfamily = pg_strdup(PQgetvalue(res, 0, i_opcfamily));
	opcfamilyname = PQgetvalue(res, 0, i_opcfamilyname);
	opcfamilynsp = PQgetvalue(res, 0, i_opcfamilynsp);
	/* amname will still be needed after we PQclear res */
	amname = pg_strdup(PQgetvalue(res, 0, i_amname));

	appendPQExpBuffer(delq, "DROP OPERATOR CLASS %s",
					  fmtQualifiedDumpable(opcinfo));
	appendPQExpBuffer(delq, " USING %s;\n",
					  fmtId(amname));

	/* Build the fixed portion of the CREATE command */
	appendPQExpBuffer(q, "CREATE OPERATOR CLASS %s\n    ",
					  fmtQualifiedDumpable(opcinfo));
	if (strcmp(opcdefault, "t") == 0)
		appendPQExpBufferStr(q, "DEFAULT ");
	appendPQExpBuffer(q, "FOR TYPE %s USING %s",
					  opcintype,
					  fmtId(amname));
	if (strlen(opcfamilyname) > 0)
	{
		appendPQExpBufferStr(q, " FAMILY ");
		appendPQExpBuffer(q, "%s.", fmtId(opcfamilynsp));
		appendPQExpBufferStr(q, fmtId(opcfamilyname));
	}
	appendPQExpBufferStr(q, " AS\n    ");

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
	 *
	 * Print only those opfamily members that are tied to the opclass by
	 * pg_depend entries.
	 *
	 * XXX RECHECK is gone as of 8.4, but we'll still print it if dumping an
	 * older server's opclass in which it is used.  This is to avoid
	 * hard-to-detect breakage if a newer pg_dump is used to dump from an
	 * older server and then reload into that old version.  This can go away
	 * once 8.3 is so old as to not be of interest to anyone.
	 */
	resetPQExpBuffer(query);

	if (fout->remoteVersion >= 90100)
	{
		appendPQExpBuffer(query, "SELECT amopstrategy, false AS amopreqcheck, "
						  "amopopr::pg_catalog.regoperator, "
						  "opfname AS sortfamily, "
						  "nspname AS sortfamilynsp "
						  "FROM pg_catalog.pg_amop ao JOIN pg_catalog.pg_depend ON "
						  "(classid = 'pg_catalog.pg_amop'::pg_catalog.regclass AND objid = ao.oid) "
						  "LEFT JOIN pg_catalog.pg_opfamily f ON f.oid = amopsortfamily "
						  "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = opfnamespace "
						  "WHERE refclassid = 'pg_catalog.pg_opclass'::pg_catalog.regclass "
						  "AND refobjid = '%u'::pg_catalog.oid "
						  "AND amopfamily = '%s'::pg_catalog.oid "
						  "ORDER BY amopstrategy",
						  opcinfo->dobj.catId.oid,
						  opcfamily);
	}
	else if (fout->remoteVersion >= 80400)
	{
		appendPQExpBuffer(query, "SELECT amopstrategy, false AS amopreqcheck, "
						  "amopopr::pg_catalog.regoperator, "
						  "NULL AS sortfamily, "
						  "NULL AS sortfamilynsp "
						  "FROM pg_catalog.pg_amop ao, pg_catalog.pg_depend "
						  "WHERE refclassid = 'pg_catalog.pg_opclass'::pg_catalog.regclass "
						  "AND refobjid = '%u'::pg_catalog.oid "
						  "AND classid = 'pg_catalog.pg_amop'::pg_catalog.regclass "
						  "AND objid = ao.oid "
						  "ORDER BY amopstrategy",
						  opcinfo->dobj.catId.oid);
	}
	else if (fout->remoteVersion >= 80300)
	{
		appendPQExpBuffer(query, "SELECT amopstrategy, amopreqcheck, "
						  "amopopr::pg_catalog.regoperator, "
						  "NULL AS sortfamily, "
						  "NULL AS sortfamilynsp "
						  "FROM pg_catalog.pg_amop ao, pg_catalog.pg_depend "
						  "WHERE refclassid = 'pg_catalog.pg_opclass'::pg_catalog.regclass "
						  "AND refobjid = '%u'::pg_catalog.oid "
						  "AND classid = 'pg_catalog.pg_amop'::pg_catalog.regclass "
						  "AND objid = ao.oid "
						  "ORDER BY amopstrategy",
						  opcinfo->dobj.catId.oid);
	}
	else
	{
		/*
		 * Here, we print all entries since there are no opfamilies and hence
		 * no loose operators to worry about.
		 */
		appendPQExpBuffer(query, "SELECT amopstrategy, amopreqcheck, "
						  "amopopr::pg_catalog.regoperator, "
						  "NULL AS sortfamily, "
						  "NULL AS sortfamilynsp "
						  "FROM pg_catalog.pg_amop "
						  "WHERE amopclaid = '%u'::pg_catalog.oid "
						  "ORDER BY amopstrategy",
						  opcinfo->dobj.catId.oid);
	}

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_amopstrategy = PQfnumber(res, "amopstrategy");
	i_amopreqcheck = PQfnumber(res, "amopreqcheck");
	i_amopopr = PQfnumber(res, "amopopr");
	i_sortfamily = PQfnumber(res, "sortfamily");
	i_sortfamilynsp = PQfnumber(res, "sortfamilynsp");

	for (i = 0; i < ntups; i++)
	{
		amopstrategy = PQgetvalue(res, i, i_amopstrategy);
		amopreqcheck = PQgetvalue(res, i, i_amopreqcheck);
		amopopr = PQgetvalue(res, i, i_amopopr);
		sortfamily = PQgetvalue(res, i, i_sortfamily);
		sortfamilynsp = PQgetvalue(res, i, i_sortfamilynsp);

		if (needComma)
			appendPQExpBufferStr(q, " ,\n    ");

		appendPQExpBuffer(q, "OPERATOR %s %s",
						  amopstrategy, amopopr);

		if (strlen(sortfamily) > 0)
		{
			appendPQExpBufferStr(q, " FOR ORDER BY ");
			appendPQExpBuffer(q, "%s.", fmtId(sortfamilynsp));
			appendPQExpBufferStr(q, fmtId(sortfamily));
		}

		if (strcmp(amopreqcheck, "t") == 0)
			appendPQExpBufferStr(q, " RECHECK");

		needComma = true;
	}

	PQclear(res);

	/*
	 * Now fetch and print the FUNCTION entries (pg_amproc rows).
	 *
	 * Print only those opfamily members that are tied to the opclass by
	 * pg_depend entries.
	 *
	 * We print the amproclefttype/amprocrighttype even though in most cases
	 * the backend could deduce the right values, because of the corner case
	 * of a btree sort support function for a cross-type comparison.  That's
	 * only allowed in 9.2 and later, but for simplicity print them in all
	 * versions that have the columns.
	 */
	resetPQExpBuffer(query);

	if (fout->remoteVersion >= 80300)
	{
		appendPQExpBuffer(query, "SELECT amprocnum, "
						  "amproc::pg_catalog.regprocedure, "
						  "amproclefttype::pg_catalog.regtype, "
						  "amprocrighttype::pg_catalog.regtype "
						  "FROM pg_catalog.pg_amproc ap, pg_catalog.pg_depend "
						  "WHERE refclassid = 'pg_catalog.pg_opclass'::pg_catalog.regclass "
						  "AND refobjid = '%u'::pg_catalog.oid "
						  "AND classid = 'pg_catalog.pg_amproc'::pg_catalog.regclass "
						  "AND objid = ap.oid "
						  "ORDER BY amprocnum",
						  opcinfo->dobj.catId.oid);
	}
	else
	{
		appendPQExpBuffer(query, "SELECT amprocnum, "
						  "amproc::pg_catalog.regprocedure, "
						  "'' AS amproclefttype, "
						  "'' AS amprocrighttype "
						  "FROM pg_catalog.pg_amproc "
						  "WHERE amopclaid = '%u'::pg_catalog.oid "
						  "ORDER BY amprocnum",
						  opcinfo->dobj.catId.oid);
	}

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_amprocnum = PQfnumber(res, "amprocnum");
	i_amproc = PQfnumber(res, "amproc");
	i_amproclefttype = PQfnumber(res, "amproclefttype");
	i_amprocrighttype = PQfnumber(res, "amprocrighttype");

	for (i = 0; i < ntups; i++)
	{
		amprocnum = PQgetvalue(res, i, i_amprocnum);
		amproc = PQgetvalue(res, i, i_amproc);
		amproclefttype = PQgetvalue(res, i, i_amproclefttype);
		amprocrighttype = PQgetvalue(res, i, i_amprocrighttype);

		if (needComma)
			appendPQExpBufferStr(q, " ,\n    ");

		appendPQExpBuffer(q, "FUNCTION %s", amprocnum);

		if (*amproclefttype && *amprocrighttype)
			appendPQExpBuffer(q, " (%s, %s)", amproclefttype, amprocrighttype);

		appendPQExpBuffer(q, " %s", amproc);

		needComma = true;
	}

	PQclear(res);

	/*
	 * If needComma is still false it means we haven't added anything after
	 * the AS keyword.  To avoid printing broken SQL, append a dummy STORAGE
	 * clause with the same datatype.  This isn't sanctioned by the
	 * documentation, but actually DefineOpClass will treat it as a no-op.
	 */
	if (!needComma)
		appendPQExpBuffer(q, "STORAGE %s", opcintype);

	appendPQExpBufferStr(q, ";\n");

	appendPQExpBufferStr(nameusing, fmtId(opcinfo->dobj.name));
	appendPQExpBuffer(nameusing, " USING %s",
					  fmtId(amname));

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &opcinfo->dobj,
										"OPERATOR CLASS", nameusing->data,
										opcinfo->dobj.namespace->dobj.name);

	if (opcinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, opcinfo->dobj.catId, opcinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = opcinfo->dobj.name,
								  .namespace = opcinfo->dobj.namespace->dobj.name,
								  .owner = opcinfo->rolname,
								  .description = "OPERATOR CLASS",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* Dump Operator Class Comments */
	if (opcinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "OPERATOR CLASS", nameusing->data,
					opcinfo->dobj.namespace->dobj.name, opcinfo->rolname,
					opcinfo->dobj.catId, 0, opcinfo->dobj.dumpId);

	free(opcintype);
	free(opcfamily);
	free(amname);
	destroyPQExpBuffer(query);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(nameusing);
}

/*
 * dumpOpfamily
 *	  write out a single operator family definition
 *
 * Note: this also dumps any "loose" operator members that aren't bound to a
 * specific opclass within the opfamily.
 */
static void
dumpOpfamily(Archive *fout, OpfamilyInfo *opfinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer query;
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer nameusing;
	PGresult   *res;
	PGresult   *res_ops;
	PGresult   *res_procs;
	int			ntups;
	int			i_amname;
	int			i_amopstrategy;
	int			i_amopreqcheck;
	int			i_amopopr;
	int			i_sortfamily;
	int			i_sortfamilynsp;
	int			i_amprocnum;
	int			i_amproc;
	int			i_amproclefttype;
	int			i_amprocrighttype;
	char	   *amname;
	char	   *amopstrategy;
	char	   *amopreqcheck;
	char	   *amopopr;
	char	   *sortfamily;
	char	   *sortfamilynsp;
	char	   *amprocnum;
	char	   *amproc;
	char	   *amproclefttype;
	char	   *amprocrighttype;
	bool		needComma;
	int			i;

	/* Skip if not to be dumped */
	if (!opfinfo->dobj.dump || dopt->dataOnly)
		return;

	query = createPQExpBuffer();
	q = createPQExpBuffer();
	delq = createPQExpBuffer();
	nameusing = createPQExpBuffer();

	/*
	 * Fetch only those opfamily members that are tied directly to the
	 * opfamily by pg_depend entries.
	 *
	 * XXX RECHECK is gone as of 8.4, but we'll still print it if dumping an
	 * older server's opclass in which it is used.  This is to avoid
	 * hard-to-detect breakage if a newer pg_dump is used to dump from an
	 * older server and then reload into that old version.  This can go away
	 * once 8.3 is so old as to not be of interest to anyone.
	 */
	if (fout->remoteVersion >= 90100)
	{
		appendPQExpBuffer(query, "SELECT amopstrategy, false AS amopreqcheck, "
						  "amopopr::pg_catalog.regoperator, "
						  "opfname AS sortfamily, "
						  "nspname AS sortfamilynsp "
						  "FROM pg_catalog.pg_amop ao JOIN pg_catalog.pg_depend ON "
						  "(classid = 'pg_catalog.pg_amop'::pg_catalog.regclass AND objid = ao.oid) "
						  "LEFT JOIN pg_catalog.pg_opfamily f ON f.oid = amopsortfamily "
						  "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = opfnamespace "
						  "WHERE refclassid = 'pg_catalog.pg_opfamily'::pg_catalog.regclass "
						  "AND refobjid = '%u'::pg_catalog.oid "
						  "AND amopfamily = '%u'::pg_catalog.oid "
						  "ORDER BY amopstrategy",
						  opfinfo->dobj.catId.oid,
						  opfinfo->dobj.catId.oid);
	}
	else if (fout->remoteVersion >= 80400)
	{
		appendPQExpBuffer(query, "SELECT amopstrategy, false AS amopreqcheck, "
						  "amopopr::pg_catalog.regoperator, "
						  "NULL AS sortfamily, "
						  "NULL AS sortfamilynsp "
						  "FROM pg_catalog.pg_amop ao, pg_catalog.pg_depend "
						  "WHERE refclassid = 'pg_catalog.pg_opfamily'::pg_catalog.regclass "
						  "AND refobjid = '%u'::pg_catalog.oid "
						  "AND classid = 'pg_catalog.pg_amop'::pg_catalog.regclass "
						  "AND objid = ao.oid "
						  "ORDER BY amopstrategy",
						  opfinfo->dobj.catId.oid);
	}
	else
	{
		appendPQExpBuffer(query, "SELECT amopstrategy, amopreqcheck, "
						  "amopopr::pg_catalog.regoperator, "
						  "NULL AS sortfamily, "
						  "NULL AS sortfamilynsp "
						  "FROM pg_catalog.pg_amop ao, pg_catalog.pg_depend "
						  "WHERE refclassid = 'pg_catalog.pg_opfamily'::pg_catalog.regclass "
						  "AND refobjid = '%u'::pg_catalog.oid "
						  "AND classid = 'pg_catalog.pg_amop'::pg_catalog.regclass "
						  "AND objid = ao.oid "
						  "ORDER BY amopstrategy",
						  opfinfo->dobj.catId.oid);
	}

	res_ops = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	resetPQExpBuffer(query);

	appendPQExpBuffer(query, "SELECT amprocnum, "
					  "amproc::pg_catalog.regprocedure, "
					  "amproclefttype::pg_catalog.regtype, "
					  "amprocrighttype::pg_catalog.regtype "
					  "FROM pg_catalog.pg_amproc ap, pg_catalog.pg_depend "
					  "WHERE refclassid = 'pg_catalog.pg_opfamily'::pg_catalog.regclass "
					  "AND refobjid = '%u'::pg_catalog.oid "
					  "AND classid = 'pg_catalog.pg_amproc'::pg_catalog.regclass "
					  "AND objid = ap.oid "
					  "ORDER BY amprocnum",
					  opfinfo->dobj.catId.oid);

	res_procs = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	/* Get additional fields from the pg_opfamily row */
	resetPQExpBuffer(query);

	appendPQExpBuffer(query, "SELECT "
					  "(SELECT amname FROM pg_catalog.pg_am WHERE oid = opfmethod) AS amname "
					  "FROM pg_catalog.pg_opfamily "
					  "WHERE oid = '%u'::pg_catalog.oid",
					  opfinfo->dobj.catId.oid);

	res = ExecuteSqlQueryForSingleRow(fout, query->data);

	i_amname = PQfnumber(res, "amname");

	/* amname will still be needed after we PQclear res */
	amname = pg_strdup(PQgetvalue(res, 0, i_amname));

	appendPQExpBuffer(delq, "DROP OPERATOR FAMILY %s",
					  fmtQualifiedDumpable(opfinfo));
	appendPQExpBuffer(delq, " USING %s;\n",
					  fmtId(amname));

	/* Build the fixed portion of the CREATE command */
	appendPQExpBuffer(q, "CREATE OPERATOR FAMILY %s",
					  fmtQualifiedDumpable(opfinfo));
	appendPQExpBuffer(q, " USING %s;\n",
					  fmtId(amname));

	PQclear(res);

	/* Do we need an ALTER to add loose members? */
	if (PQntuples(res_ops) > 0 || PQntuples(res_procs) > 0)
	{
		appendPQExpBuffer(q, "ALTER OPERATOR FAMILY %s",
						  fmtQualifiedDumpable(opfinfo));
		appendPQExpBuffer(q, " USING %s ADD\n    ",
						  fmtId(amname));

		needComma = false;

		/*
		 * Now fetch and print the OPERATOR entries (pg_amop rows).
		 */
		ntups = PQntuples(res_ops);

		i_amopstrategy = PQfnumber(res_ops, "amopstrategy");
		i_amopreqcheck = PQfnumber(res_ops, "amopreqcheck");
		i_amopopr = PQfnumber(res_ops, "amopopr");
		i_sortfamily = PQfnumber(res_ops, "sortfamily");
		i_sortfamilynsp = PQfnumber(res_ops, "sortfamilynsp");

		for (i = 0; i < ntups; i++)
		{
			amopstrategy = PQgetvalue(res_ops, i, i_amopstrategy);
			amopreqcheck = PQgetvalue(res_ops, i, i_amopreqcheck);
			amopopr = PQgetvalue(res_ops, i, i_amopopr);
			sortfamily = PQgetvalue(res_ops, i, i_sortfamily);
			sortfamilynsp = PQgetvalue(res_ops, i, i_sortfamilynsp);

			if (needComma)
				appendPQExpBufferStr(q, " ,\n    ");

			appendPQExpBuffer(q, "OPERATOR %s %s",
							  amopstrategy, amopopr);

			if (strlen(sortfamily) > 0)
			{
				appendPQExpBufferStr(q, " FOR ORDER BY ");
				appendPQExpBuffer(q, "%s.", fmtId(sortfamilynsp));
				appendPQExpBufferStr(q, fmtId(sortfamily));
			}

			if (strcmp(amopreqcheck, "t") == 0)
				appendPQExpBufferStr(q, " RECHECK");

			needComma = true;
		}

		/*
		 * Now fetch and print the FUNCTION entries (pg_amproc rows).
		 */
		ntups = PQntuples(res_procs);

		i_amprocnum = PQfnumber(res_procs, "amprocnum");
		i_amproc = PQfnumber(res_procs, "amproc");
		i_amproclefttype = PQfnumber(res_procs, "amproclefttype");
		i_amprocrighttype = PQfnumber(res_procs, "amprocrighttype");

		for (i = 0; i < ntups; i++)
		{
			amprocnum = PQgetvalue(res_procs, i, i_amprocnum);
			amproc = PQgetvalue(res_procs, i, i_amproc);
			amproclefttype = PQgetvalue(res_procs, i, i_amproclefttype);
			amprocrighttype = PQgetvalue(res_procs, i, i_amprocrighttype);

			if (needComma)
				appendPQExpBufferStr(q, " ,\n    ");

			appendPQExpBuffer(q, "FUNCTION %s (%s, %s) %s",
							  amprocnum, amproclefttype, amprocrighttype,
							  amproc);

			needComma = true;
		}

		appendPQExpBufferStr(q, ";\n");
	}

	appendPQExpBufferStr(nameusing, fmtId(opfinfo->dobj.name));
	appendPQExpBuffer(nameusing, " USING %s",
					  fmtId(amname));

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &opfinfo->dobj,
										"OPERATOR FAMILY", nameusing->data,
										opfinfo->dobj.namespace->dobj.name);

	if (opfinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, opfinfo->dobj.catId, opfinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = opfinfo->dobj.name,
								  .namespace = opfinfo->dobj.namespace->dobj.name,
								  .owner = opfinfo->rolname,
								  .description = "OPERATOR FAMILY",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* Dump Operator Family Comments */
	if (opfinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "OPERATOR FAMILY", nameusing->data,
					opfinfo->dobj.namespace->dobj.name, opfinfo->rolname,
					opfinfo->dobj.catId, 0, opfinfo->dobj.dumpId);

	free(amname);
	PQclear(res_ops);
	PQclear(res_procs);
	destroyPQExpBuffer(query);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(nameusing);
}

/*
 * dumpCollation
 *	  write out a single collation definition
 */
static void
dumpCollation(Archive *fout, CollInfo *collinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer query;
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *qcollname;
	PGresult   *res;
	int			i_collprovider;
	int			i_collisdeterministic;
	int			i_collcollate;
	int			i_collctype;
	const char *collprovider;
	const char *collcollate;
	const char *collctype;

	/* Skip if not to be dumped */
	if (!collinfo->dobj.dump || dopt->dataOnly)
		return;

	query = createPQExpBuffer();
	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	qcollname = pg_strdup(fmtId(collinfo->dobj.name));

	/* Get collation-specific details */
	appendPQExpBufferStr(query, "SELECT ");

	if (fout->remoteVersion >= 100000)
		appendPQExpBufferStr(query,
							 "collprovider, ");
	else
		appendPQExpBufferStr(query,
							 "'c' AS collprovider, ");

	if (fout->remoteVersion >= 120000)
		appendPQExpBufferStr(query,
							 "collisdeterministic, ");
	else
		appendPQExpBufferStr(query,
							 "true AS collisdeterministic, ");

	appendPQExpBuffer(query,
					  "collcollate, "
					  "collctype "
					  "FROM pg_catalog.pg_collation c "
					  "WHERE c.oid = '%u'::pg_catalog.oid",
					  collinfo->dobj.catId.oid);

	res = ExecuteSqlQueryForSingleRow(fout, query->data);

	i_collprovider = PQfnumber(res, "collprovider");
	i_collisdeterministic = PQfnumber(res, "collisdeterministic");
	i_collcollate = PQfnumber(res, "collcollate");
	i_collctype = PQfnumber(res, "collctype");

	collprovider = PQgetvalue(res, 0, i_collprovider);
	collcollate = PQgetvalue(res, 0, i_collcollate);
	collctype = PQgetvalue(res, 0, i_collctype);

	appendPQExpBuffer(delq, "DROP COLLATION %s;\n",
					  fmtQualifiedDumpable(collinfo));

	appendPQExpBuffer(q, "CREATE COLLATION %s (",
					  fmtQualifiedDumpable(collinfo));

	appendPQExpBufferStr(q, "provider = ");
	if (collprovider[0] == 'c')
		appendPQExpBufferStr(q, "libc");
	else if (collprovider[0] == 'i')
		appendPQExpBufferStr(q, "icu");
	else if (collprovider[0] == 'd')
		/* to allow dumping pg_catalog; not accepted on input */
		appendPQExpBufferStr(q, "default");
	else
		fatal("unrecognized collation provider: %s",
			  collprovider);

	if (strcmp(PQgetvalue(res, 0, i_collisdeterministic), "f") == 0)
		appendPQExpBufferStr(q, ", deterministic = false");

	if (strcmp(collcollate, collctype) == 0)
	{
		appendPQExpBufferStr(q, ", locale = ");
		appendStringLiteralAH(q, collcollate, fout);
	}
	else
	{
		appendPQExpBufferStr(q, ", lc_collate = ");
		appendStringLiteralAH(q, collcollate, fout);
		appendPQExpBufferStr(q, ", lc_ctype = ");
		appendStringLiteralAH(q, collctype, fout);
	}

	appendPQExpBufferStr(q, ");\n");

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &collinfo->dobj,
										"COLLATION", qcollname,
										collinfo->dobj.namespace->dobj.name);

	if (collinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, collinfo->dobj.catId, collinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = collinfo->dobj.name,
								  .namespace = collinfo->dobj.namespace->dobj.name,
								  .owner = collinfo->rolname,
								  .description = "COLLATION",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* Dump Collation Comments */
	if (collinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "COLLATION", qcollname,
					collinfo->dobj.namespace->dobj.name, collinfo->rolname,
					collinfo->dobj.catId, 0, collinfo->dobj.dumpId);

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	free(qcollname);
}

/*
 * dumpConversion
 *	  write out a single conversion definition
 */
static void
dumpConversion(Archive *fout, ConvInfo *convinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer query;
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *qconvname;
	PGresult   *res;
	int			i_conforencoding;
	int			i_contoencoding;
	int			i_conproc;
	int			i_condefault;
	const char *conforencoding;
	const char *contoencoding;
	const char *conproc;
	bool		condefault;

	/* Skip if not to be dumped */
	if (!convinfo->dobj.dump || dopt->dataOnly)
		return;

	query = createPQExpBuffer();
	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	qconvname = pg_strdup(fmtId(convinfo->dobj.name));

	/* Get conversion-specific details */
	appendPQExpBuffer(query, "SELECT "
					  "pg_catalog.pg_encoding_to_char(conforencoding) AS conforencoding, "
					  "pg_catalog.pg_encoding_to_char(contoencoding) AS contoencoding, "
					  "conproc, condefault "
					  "FROM pg_catalog.pg_conversion c "
					  "WHERE c.oid = '%u'::pg_catalog.oid",
					  convinfo->dobj.catId.oid);

	res = ExecuteSqlQueryForSingleRow(fout, query->data);

	i_conforencoding = PQfnumber(res, "conforencoding");
	i_contoencoding = PQfnumber(res, "contoencoding");
	i_conproc = PQfnumber(res, "conproc");
	i_condefault = PQfnumber(res, "condefault");

	conforencoding = PQgetvalue(res, 0, i_conforencoding);
	contoencoding = PQgetvalue(res, 0, i_contoencoding);
	conproc = PQgetvalue(res, 0, i_conproc);
	condefault = (PQgetvalue(res, 0, i_condefault)[0] == 't');

	appendPQExpBuffer(delq, "DROP CONVERSION %s;\n",
					  fmtQualifiedDumpable(convinfo));

	appendPQExpBuffer(q, "CREATE %sCONVERSION %s FOR ",
					  (condefault) ? "DEFAULT " : "",
					  fmtQualifiedDumpable(convinfo));
	appendStringLiteralAH(q, conforencoding, fout);
	appendPQExpBufferStr(q, " TO ");
	appendStringLiteralAH(q, contoencoding, fout);
	/* regproc output is already sufficiently quoted */
	appendPQExpBuffer(q, " FROM %s;\n", conproc);

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &convinfo->dobj,
										"CONVERSION", qconvname,
										convinfo->dobj.namespace->dobj.name);

	if (convinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, convinfo->dobj.catId, convinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = convinfo->dobj.name,
								  .namespace = convinfo->dobj.namespace->dobj.name,
								  .owner = convinfo->rolname,
								  .description = "CONVERSION",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* Dump Conversion Comments */
	if (convinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "CONVERSION", qconvname,
					convinfo->dobj.namespace->dobj.name, convinfo->rolname,
					convinfo->dobj.catId, 0, convinfo->dobj.dumpId);

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	free(qconvname);
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
		appendPQExpBufferStr(&buf, fmtId(agginfo->aggfn.dobj.name));
	else
		appendPQExpBufferStr(&buf, agginfo->aggfn.dobj.name);

	if (agginfo->aggfn.nargs == 0)
		appendPQExpBufferStr(&buf, "(*)");
	else
	{
		appendPQExpBufferChar(&buf, '(');
		for (j = 0; j < agginfo->aggfn.nargs; j++)
		{
			char	   *typname;

			typname = getFormattedTypeName(fout, agginfo->aggfn.argtypes[j],
										   zeroIsError);

			appendPQExpBuffer(&buf, "%s%s",
							  (j > 0) ? ", " : "",
							  typname);
			free(typname);
		}
		appendPQExpBufferChar(&buf, ')');
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
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer query;
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer details;
	char	   *aggsig;			/* identity signature */
	char	   *aggfullsig = NULL;	/* full signature */
	char	   *aggsig_tag;
	PGresult   *res;
	int			i_agginitval;
	int			i_aggminitval;
	const char *aggtransfn;
	const char *aggfinalfn;
	const char *aggcombinefn;
	const char *aggserialfn;
	const char *aggdeserialfn;
	const char *aggmtransfn;
	const char *aggminvtransfn;
	const char *aggmfinalfn;
	bool		aggfinalextra;
	bool		aggmfinalextra;
	char		aggfinalmodify;
	char		aggmfinalmodify;
	const char *aggsortop;
	char	   *aggsortconvop;
	char		aggkind;
	const char *aggtranstype;
	const char *aggtransspace;
	const char *aggmtranstype;
	const char *aggmtransspace;
	const char *agginitval;
	const char *aggminitval;
	const char *proparallel;
	char		defaultfinalmodify;

	/* Skip if not to be dumped */
	if (!agginfo->aggfn.dobj.dump || dopt->dataOnly)
		return;

	query = createPQExpBuffer();
	q = createPQExpBuffer();
	delq = createPQExpBuffer();
	details = createPQExpBuffer();

	/* Get aggregate-specific details */
	appendPQExpBufferStr(query,
						 "SELECT\n"
						 "aggtransfn,\n"
						 "aggfinalfn,\n"
						 "aggtranstype::pg_catalog.regtype,\n"
						 "agginitval,\n");

	if (fout->remoteVersion >= 80100)
		appendPQExpBufferStr(query,
							 "aggsortop,\n");
	else
		appendPQExpBufferStr(query,
							 "0 AS aggsortop,\n");

	if (fout->remoteVersion >= 80400)
		appendPQExpBufferStr(query,
							 "pg_catalog.pg_get_function_arguments(p.oid) AS funcargs,\n"
							 "pg_catalog.pg_get_function_identity_arguments(p.oid) AS funciargs,\n");

	if (fout->remoteVersion >= 90400)
		appendPQExpBufferStr(query,
							 "aggkind,\n"
							 "aggmtransfn,\n"
							 "aggminvtransfn,\n"
							 "aggmfinalfn,\n"
							 "aggmtranstype::pg_catalog.regtype,\n"
							 "aggfinalextra,\n"
							 "aggmfinalextra,\n"
							 "aggtransspace,\n"
							 "aggmtransspace,\n"
							 "aggminitval,\n");
	else
		appendPQExpBufferStr(query,
							 "'n' AS aggkind,\n"
							 "'-' AS aggmtransfn,\n"
							 "'-' AS aggminvtransfn,\n"
							 "'-' AS aggmfinalfn,\n"
							 "0 AS aggmtranstype,\n"
							 "false AS aggfinalextra,\n"
							 "false AS aggmfinalextra,\n"
							 "0 AS aggtransspace,\n"
							 "0 AS aggmtransspace,\n"
							 "NULL AS aggminitval,\n");

	if (fout->remoteVersion >= 90600)
		appendPQExpBufferStr(query,
							 "aggcombinefn,\n"
							 "aggserialfn,\n"
							 "aggdeserialfn,\n"
							 "proparallel,\n");
	else
		appendPQExpBufferStr(query,
							 "'-' AS aggcombinefn,\n"
							 "'-' AS aggserialfn,\n"
							 "'-' AS aggdeserialfn,\n"
							 "'u' AS proparallel,\n");

	if (fout->remoteVersion >= 110000)
		appendPQExpBufferStr(query,
							 "aggfinalmodify,\n"
							 "aggmfinalmodify\n");
	else
		appendPQExpBufferStr(query,
							 "'0' AS aggfinalmodify,\n"
							 "'0' AS aggmfinalmodify\n");

	appendPQExpBuffer(query,
					  "FROM pg_catalog.pg_aggregate a, pg_catalog.pg_proc p "
					  "WHERE a.aggfnoid = p.oid "
					  "AND p.oid = '%u'::pg_catalog.oid",
					  agginfo->aggfn.dobj.catId.oid);

	res = ExecuteSqlQueryForSingleRow(fout, query->data);

	i_agginitval = PQfnumber(res, "agginitval");
	i_aggminitval = PQfnumber(res, "aggminitval");

	aggtransfn = PQgetvalue(res, 0, PQfnumber(res, "aggtransfn"));
	aggfinalfn = PQgetvalue(res, 0, PQfnumber(res, "aggfinalfn"));
	aggcombinefn = PQgetvalue(res, 0, PQfnumber(res, "aggcombinefn"));
	aggserialfn = PQgetvalue(res, 0, PQfnumber(res, "aggserialfn"));
	aggdeserialfn = PQgetvalue(res, 0, PQfnumber(res, "aggdeserialfn"));
	aggmtransfn = PQgetvalue(res, 0, PQfnumber(res, "aggmtransfn"));
	aggminvtransfn = PQgetvalue(res, 0, PQfnumber(res, "aggminvtransfn"));
	aggmfinalfn = PQgetvalue(res, 0, PQfnumber(res, "aggmfinalfn"));
	aggfinalextra = (PQgetvalue(res, 0, PQfnumber(res, "aggfinalextra"))[0] == 't');
	aggmfinalextra = (PQgetvalue(res, 0, PQfnumber(res, "aggmfinalextra"))[0] == 't');
	aggfinalmodify = PQgetvalue(res, 0, PQfnumber(res, "aggfinalmodify"))[0];
	aggmfinalmodify = PQgetvalue(res, 0, PQfnumber(res, "aggmfinalmodify"))[0];
	aggsortop = PQgetvalue(res, 0, PQfnumber(res, "aggsortop"));
	aggkind = PQgetvalue(res, 0, PQfnumber(res, "aggkind"))[0];
	aggtranstype = PQgetvalue(res, 0, PQfnumber(res, "aggtranstype"));
	aggtransspace = PQgetvalue(res, 0, PQfnumber(res, "aggtransspace"));
	aggmtranstype = PQgetvalue(res, 0, PQfnumber(res, "aggmtranstype"));
	aggmtransspace = PQgetvalue(res, 0, PQfnumber(res, "aggmtransspace"));
	agginitval = PQgetvalue(res, 0, i_agginitval);
	aggminitval = PQgetvalue(res, 0, i_aggminitval);
	proparallel = PQgetvalue(res, 0, PQfnumber(res, "proparallel"));

	if (fout->remoteVersion >= 80400)
	{
		/* 8.4 or later; we rely on server-side code for most of the work */
		char	   *funcargs;
		char	   *funciargs;

		funcargs = PQgetvalue(res, 0, PQfnumber(res, "funcargs"));
		funciargs = PQgetvalue(res, 0, PQfnumber(res, "funciargs"));
		aggfullsig = format_function_arguments(&agginfo->aggfn, funcargs, true);
		aggsig = format_function_arguments(&agginfo->aggfn, funciargs, true);
	}
	else
		/* pre-8.4, do it ourselves */
		aggsig = format_aggregate_signature(agginfo, fout, true);

	aggsig_tag = format_aggregate_signature(agginfo, fout, false);

	/* identify default modify flag for aggkind (must match DefineAggregate) */
	defaultfinalmodify = (aggkind == AGGKIND_NORMAL) ? AGGMODIFY_READ_ONLY : AGGMODIFY_READ_WRITE;
	/* replace omitted flags for old versions */
	if (aggfinalmodify == '0')
		aggfinalmodify = defaultfinalmodify;
	if (aggmfinalmodify == '0')
		aggmfinalmodify = defaultfinalmodify;

	/* regproc and regtype output is already sufficiently quoted */
	appendPQExpBuffer(details, "    SFUNC = %s,\n    STYPE = %s",
					  aggtransfn, aggtranstype);

	if (strcmp(aggtransspace, "0") != 0)
	{
		appendPQExpBuffer(details, ",\n    SSPACE = %s",
						  aggtransspace);
	}

	if (!PQgetisnull(res, 0, i_agginitval))
	{
		appendPQExpBufferStr(details, ",\n    INITCOND = ");
		appendStringLiteralAH(details, agginitval, fout);
	}

	if (strcmp(aggfinalfn, "-") != 0)
	{
		appendPQExpBuffer(details, ",\n    FINALFUNC = %s",
						  aggfinalfn);
		if (aggfinalextra)
			appendPQExpBufferStr(details, ",\n    FINALFUNC_EXTRA");
		if (aggfinalmodify != defaultfinalmodify)
		{
			switch (aggfinalmodify)
			{
				case AGGMODIFY_READ_ONLY:
					appendPQExpBufferStr(details, ",\n    FINALFUNC_MODIFY = READ_ONLY");
					break;
				case AGGMODIFY_SHAREABLE:
					appendPQExpBufferStr(details, ",\n    FINALFUNC_MODIFY = SHAREABLE");
					break;
				case AGGMODIFY_READ_WRITE:
					appendPQExpBufferStr(details, ",\n    FINALFUNC_MODIFY = READ_WRITE");
					break;
				default:
					fatal("unrecognized aggfinalmodify value for aggregate \"%s\"",
						  agginfo->aggfn.dobj.name);
					break;
			}
		}
	}

	if (strcmp(aggcombinefn, "-") != 0)
		appendPQExpBuffer(details, ",\n    COMBINEFUNC = %s", aggcombinefn);

	if (strcmp(aggserialfn, "-") != 0)
		appendPQExpBuffer(details, ",\n    SERIALFUNC = %s", aggserialfn);

	if (strcmp(aggdeserialfn, "-") != 0)
		appendPQExpBuffer(details, ",\n    DESERIALFUNC = %s", aggdeserialfn);

	if (strcmp(aggmtransfn, "-") != 0)
	{
		appendPQExpBuffer(details, ",\n    MSFUNC = %s,\n    MINVFUNC = %s,\n    MSTYPE = %s",
						  aggmtransfn,
						  aggminvtransfn,
						  aggmtranstype);
	}

	if (strcmp(aggmtransspace, "0") != 0)
	{
		appendPQExpBuffer(details, ",\n    MSSPACE = %s",
						  aggmtransspace);
	}

	if (!PQgetisnull(res, 0, i_aggminitval))
	{
		appendPQExpBufferStr(details, ",\n    MINITCOND = ");
		appendStringLiteralAH(details, aggminitval, fout);
	}

	if (strcmp(aggmfinalfn, "-") != 0)
	{
		appendPQExpBuffer(details, ",\n    MFINALFUNC = %s",
						  aggmfinalfn);
		if (aggmfinalextra)
			appendPQExpBufferStr(details, ",\n    MFINALFUNC_EXTRA");
		if (aggmfinalmodify != defaultfinalmodify)
		{
			switch (aggmfinalmodify)
			{
				case AGGMODIFY_READ_ONLY:
					appendPQExpBufferStr(details, ",\n    MFINALFUNC_MODIFY = READ_ONLY");
					break;
				case AGGMODIFY_SHAREABLE:
					appendPQExpBufferStr(details, ",\n    MFINALFUNC_MODIFY = SHAREABLE");
					break;
				case AGGMODIFY_READ_WRITE:
					appendPQExpBufferStr(details, ",\n    MFINALFUNC_MODIFY = READ_WRITE");
					break;
				default:
					fatal("unrecognized aggmfinalmodify value for aggregate \"%s\"",
						  agginfo->aggfn.dobj.name);
					break;
			}
		}
	}

	aggsortconvop = getFormattedOperatorName(aggsortop);
	if (aggsortconvop)
	{
		appendPQExpBuffer(details, ",\n    SORTOP = %s",
						  aggsortconvop);
		free(aggsortconvop);
	}

	if (aggkind == AGGKIND_HYPOTHETICAL)
		appendPQExpBufferStr(details, ",\n    HYPOTHETICAL");

	if (proparallel[0] != PROPARALLEL_UNSAFE)
	{
		if (proparallel[0] == PROPARALLEL_SAFE)
			appendPQExpBufferStr(details, ",\n    PARALLEL = safe");
		else if (proparallel[0] == PROPARALLEL_RESTRICTED)
			appendPQExpBufferStr(details, ",\n    PARALLEL = restricted");
		else if (proparallel[0] != PROPARALLEL_UNSAFE)
			fatal("unrecognized proparallel value for function \"%s\"",
				  agginfo->aggfn.dobj.name);
	}

	appendPQExpBuffer(delq, "DROP AGGREGATE %s.%s;\n",
					  fmtId(agginfo->aggfn.dobj.namespace->dobj.name),
					  aggsig);

	appendPQExpBuffer(q, "CREATE AGGREGATE %s.%s (\n%s\n);\n",
					  fmtId(agginfo->aggfn.dobj.namespace->dobj.name),
					  aggfullsig ? aggfullsig : aggsig, details->data);

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &agginfo->aggfn.dobj,
										"AGGREGATE", aggsig,
										agginfo->aggfn.dobj.namespace->dobj.name);

	if (agginfo->aggfn.dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, agginfo->aggfn.dobj.catId,
					 agginfo->aggfn.dobj.dumpId,
					 ARCHIVE_OPTS(.tag = aggsig_tag,
								  .namespace = agginfo->aggfn.dobj.namespace->dobj.name,
								  .owner = agginfo->aggfn.rolname,
								  .description = "AGGREGATE",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* Dump Aggregate Comments */
	if (agginfo->aggfn.dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "AGGREGATE", aggsig,
					agginfo->aggfn.dobj.namespace->dobj.name,
					agginfo->aggfn.rolname,
					agginfo->aggfn.dobj.catId, 0, agginfo->aggfn.dobj.dumpId);

	if (agginfo->aggfn.dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpSecLabel(fout, "AGGREGATE", aggsig,
					 agginfo->aggfn.dobj.namespace->dobj.name,
					 agginfo->aggfn.rolname,
					 agginfo->aggfn.dobj.catId, 0, agginfo->aggfn.dobj.dumpId);

	/*
	 * Since there is no GRANT ON AGGREGATE syntax, we have to make the ACL
	 * command look like a function's GRANT; in particular this affects the
	 * syntax for zero-argument aggregates and ordered-set aggregates.
	 */
	free(aggsig);

	aggsig = format_function_signature(fout, &agginfo->aggfn, true);

	if (agginfo->aggfn.dobj.dump & DUMP_COMPONENT_ACL)
		dumpACL(fout, agginfo->aggfn.dobj.dumpId, InvalidDumpId,
				"FUNCTION", aggsig, NULL,
				agginfo->aggfn.dobj.namespace->dobj.name,
				agginfo->aggfn.rolname, agginfo->aggfn.proacl,
				agginfo->aggfn.rproacl,
				agginfo->aggfn.initproacl, agginfo->aggfn.initrproacl);

	free(aggsig);
	if (aggfullsig)
		free(aggfullsig);
	free(aggsig_tag);

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(details);
}

/*
 * dumpTSParser
 *	  write out a single text search parser
 */
static void
dumpTSParser(Archive *fout, TSParserInfo *prsinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *qprsname;

	/* Skip if not to be dumped */
	if (!prsinfo->dobj.dump || dopt->dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	qprsname = pg_strdup(fmtId(prsinfo->dobj.name));

	appendPQExpBuffer(q, "CREATE TEXT SEARCH PARSER %s (\n",
					  fmtQualifiedDumpable(prsinfo));

	appendPQExpBuffer(q, "    START = %s,\n",
					  convertTSFunction(fout, prsinfo->prsstart));
	appendPQExpBuffer(q, "    GETTOKEN = %s,\n",
					  convertTSFunction(fout, prsinfo->prstoken));
	appendPQExpBuffer(q, "    END = %s,\n",
					  convertTSFunction(fout, prsinfo->prsend));
	if (prsinfo->prsheadline != InvalidOid)
		appendPQExpBuffer(q, "    HEADLINE = %s,\n",
						  convertTSFunction(fout, prsinfo->prsheadline));
	appendPQExpBuffer(q, "    LEXTYPES = %s );\n",
					  convertTSFunction(fout, prsinfo->prslextype));

	appendPQExpBuffer(delq, "DROP TEXT SEARCH PARSER %s;\n",
					  fmtQualifiedDumpable(prsinfo));

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &prsinfo->dobj,
										"TEXT SEARCH PARSER", qprsname,
										prsinfo->dobj.namespace->dobj.name);

	if (prsinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, prsinfo->dobj.catId, prsinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = prsinfo->dobj.name,
								  .namespace = prsinfo->dobj.namespace->dobj.name,
								  .description = "TEXT SEARCH PARSER",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* Dump Parser Comments */
	if (prsinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "TEXT SEARCH PARSER", qprsname,
					prsinfo->dobj.namespace->dobj.name, "",
					prsinfo->dobj.catId, 0, prsinfo->dobj.dumpId);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	free(qprsname);
}

/*
 * dumpTSDictionary
 *	  write out a single text search dictionary
 */
static void
dumpTSDictionary(Archive *fout, TSDictInfo *dictinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer query;
	char	   *qdictname;
	PGresult   *res;
	char	   *nspname;
	char	   *tmplname;

	/* Skip if not to be dumped */
	if (!dictinfo->dobj.dump || dopt->dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();
	query = createPQExpBuffer();

	qdictname = pg_strdup(fmtId(dictinfo->dobj.name));

	/* Fetch name and namespace of the dictionary's template */
	appendPQExpBuffer(query, "SELECT nspname, tmplname "
					  "FROM pg_ts_template p, pg_namespace n "
					  "WHERE p.oid = '%u' AND n.oid = tmplnamespace",
					  dictinfo->dicttemplate);
	res = ExecuteSqlQueryForSingleRow(fout, query->data);
	nspname = PQgetvalue(res, 0, 0);
	tmplname = PQgetvalue(res, 0, 1);

	appendPQExpBuffer(q, "CREATE TEXT SEARCH DICTIONARY %s (\n",
					  fmtQualifiedDumpable(dictinfo));

	appendPQExpBufferStr(q, "    TEMPLATE = ");
	appendPQExpBuffer(q, "%s.", fmtId(nspname));
	appendPQExpBufferStr(q, fmtId(tmplname));

	PQclear(res);

	/* the dictinitoption can be dumped straight into the command */
	if (dictinfo->dictinitoption)
		appendPQExpBuffer(q, ",\n    %s", dictinfo->dictinitoption);

	appendPQExpBufferStr(q, " );\n");

	appendPQExpBuffer(delq, "DROP TEXT SEARCH DICTIONARY %s;\n",
					  fmtQualifiedDumpable(dictinfo));

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &dictinfo->dobj,
										"TEXT SEARCH DICTIONARY", qdictname,
										dictinfo->dobj.namespace->dobj.name);

	if (dictinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, dictinfo->dobj.catId, dictinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = dictinfo->dobj.name,
								  .namespace = dictinfo->dobj.namespace->dobj.name,
								  .owner = dictinfo->rolname,
								  .description = "TEXT SEARCH DICTIONARY",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* Dump Dictionary Comments */
	if (dictinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "TEXT SEARCH DICTIONARY", qdictname,
					dictinfo->dobj.namespace->dobj.name, dictinfo->rolname,
					dictinfo->dobj.catId, 0, dictinfo->dobj.dumpId);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(query);
	free(qdictname);
}

/*
 * dumpTSTemplate
 *	  write out a single text search template
 */
static void
dumpTSTemplate(Archive *fout, TSTemplateInfo *tmplinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *qtmplname;

	/* Skip if not to be dumped */
	if (!tmplinfo->dobj.dump || dopt->dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	qtmplname = pg_strdup(fmtId(tmplinfo->dobj.name));

	appendPQExpBuffer(q, "CREATE TEXT SEARCH TEMPLATE %s (\n",
					  fmtQualifiedDumpable(tmplinfo));

	if (tmplinfo->tmplinit != InvalidOid)
		appendPQExpBuffer(q, "    INIT = %s,\n",
						  convertTSFunction(fout, tmplinfo->tmplinit));
	appendPQExpBuffer(q, "    LEXIZE = %s );\n",
					  convertTSFunction(fout, tmplinfo->tmpllexize));

	appendPQExpBuffer(delq, "DROP TEXT SEARCH TEMPLATE %s;\n",
					  fmtQualifiedDumpable(tmplinfo));

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &tmplinfo->dobj,
										"TEXT SEARCH TEMPLATE", qtmplname,
										tmplinfo->dobj.namespace->dobj.name);

	if (tmplinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, tmplinfo->dobj.catId, tmplinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tmplinfo->dobj.name,
								  .namespace = tmplinfo->dobj.namespace->dobj.name,
								  .description = "TEXT SEARCH TEMPLATE",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* Dump Template Comments */
	if (tmplinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "TEXT SEARCH TEMPLATE", qtmplname,
					tmplinfo->dobj.namespace->dobj.name, "",
					tmplinfo->dobj.catId, 0, tmplinfo->dobj.dumpId);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	free(qtmplname);
}

/*
 * dumpTSConfig
 *	  write out a single text search configuration
 */
static void
dumpTSConfig(Archive *fout, TSConfigInfo *cfginfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer query;
	char	   *qcfgname;
	PGresult   *res;
	char	   *nspname;
	char	   *prsname;
	int			ntups,
				i;
	int			i_tokenname;
	int			i_dictname;

	/* Skip if not to be dumped */
	if (!cfginfo->dobj.dump || dopt->dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();
	query = createPQExpBuffer();

	qcfgname = pg_strdup(fmtId(cfginfo->dobj.name));

	/* Fetch name and namespace of the config's parser */
	appendPQExpBuffer(query, "SELECT nspname, prsname "
					  "FROM pg_ts_parser p, pg_namespace n "
					  "WHERE p.oid = '%u' AND n.oid = prsnamespace",
					  cfginfo->cfgparser);
	res = ExecuteSqlQueryForSingleRow(fout, query->data);
	nspname = PQgetvalue(res, 0, 0);
	prsname = PQgetvalue(res, 0, 1);

	appendPQExpBuffer(q, "CREATE TEXT SEARCH CONFIGURATION %s (\n",
					  fmtQualifiedDumpable(cfginfo));

	appendPQExpBuffer(q, "    PARSER = %s.", fmtId(nspname));
	appendPQExpBuffer(q, "%s );\n", fmtId(prsname));

	PQclear(res);

	resetPQExpBuffer(query);
	appendPQExpBuffer(query,
					  "SELECT\n"
					  "  ( SELECT alias FROM pg_catalog.ts_token_type('%u'::pg_catalog.oid) AS t\n"
					  "    WHERE t.tokid = m.maptokentype ) AS tokenname,\n"
					  "  m.mapdict::pg_catalog.regdictionary AS dictname\n"
					  "FROM pg_catalog.pg_ts_config_map AS m\n"
					  "WHERE m.mapcfg = '%u'\n"
					  "ORDER BY m.mapcfg, m.maptokentype, m.mapseqno",
					  cfginfo->cfgparser, cfginfo->dobj.catId.oid);

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);
	ntups = PQntuples(res);

	i_tokenname = PQfnumber(res, "tokenname");
	i_dictname = PQfnumber(res, "dictname");

	for (i = 0; i < ntups; i++)
	{
		char	   *tokenname = PQgetvalue(res, i, i_tokenname);
		char	   *dictname = PQgetvalue(res, i, i_dictname);

		if (i == 0 ||
			strcmp(tokenname, PQgetvalue(res, i - 1, i_tokenname)) != 0)
		{
			/* starting a new token type, so start a new command */
			if (i > 0)
				appendPQExpBufferStr(q, ";\n");
			appendPQExpBuffer(q, "\nALTER TEXT SEARCH CONFIGURATION %s\n",
							  fmtQualifiedDumpable(cfginfo));
			/* tokenname needs quoting, dictname does NOT */
			appendPQExpBuffer(q, "    ADD MAPPING FOR %s WITH %s",
							  fmtId(tokenname), dictname);
		}
		else
			appendPQExpBuffer(q, ", %s", dictname);
	}

	if (ntups > 0)
		appendPQExpBufferStr(q, ";\n");

	PQclear(res);

	appendPQExpBuffer(delq, "DROP TEXT SEARCH CONFIGURATION %s;\n",
					  fmtQualifiedDumpable(cfginfo));

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &cfginfo->dobj,
										"TEXT SEARCH CONFIGURATION", qcfgname,
										cfginfo->dobj.namespace->dobj.name);

	if (cfginfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, cfginfo->dobj.catId, cfginfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = cfginfo->dobj.name,
								  .namespace = cfginfo->dobj.namespace->dobj.name,
								  .owner = cfginfo->rolname,
								  .description = "TEXT SEARCH CONFIGURATION",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* Dump Configuration Comments */
	if (cfginfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "TEXT SEARCH CONFIGURATION", qcfgname,
					cfginfo->dobj.namespace->dobj.name, cfginfo->rolname,
					cfginfo->dobj.catId, 0, cfginfo->dobj.dumpId);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(query);
	free(qcfgname);
}

/*
 * dumpForeignDataWrapper
 *	  write out a single foreign-data wrapper definition
 */
static void
dumpForeignDataWrapper(Archive *fout, FdwInfo *fdwinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *qfdwname;

	/* Skip if not to be dumped */
	if (!fdwinfo->dobj.dump || dopt->dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	qfdwname = pg_strdup(fmtId(fdwinfo->dobj.name));

	appendPQExpBuffer(q, "CREATE FOREIGN DATA WRAPPER %s",
					  qfdwname);

	if (strcmp(fdwinfo->fdwhandler, "-") != 0)
		appendPQExpBuffer(q, " HANDLER %s", fdwinfo->fdwhandler);

	if (strcmp(fdwinfo->fdwvalidator, "-") != 0)
		appendPQExpBuffer(q, " VALIDATOR %s", fdwinfo->fdwvalidator);

	if (strlen(fdwinfo->fdwoptions) > 0)
		appendPQExpBuffer(q, " OPTIONS (\n    %s\n)", fdwinfo->fdwoptions);

	appendPQExpBufferStr(q, ";\n");

	appendPQExpBuffer(delq, "DROP FOREIGN DATA WRAPPER %s;\n",
					  qfdwname);

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &fdwinfo->dobj,
										"FOREIGN DATA WRAPPER", qfdwname,
										NULL);

	if (fdwinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, fdwinfo->dobj.catId, fdwinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = fdwinfo->dobj.name,
								  .owner = fdwinfo->rolname,
								  .description = "FOREIGN DATA WRAPPER",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* Dump Foreign Data Wrapper Comments */
	if (fdwinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "FOREIGN DATA WRAPPER", qfdwname,
					NULL, fdwinfo->rolname,
					fdwinfo->dobj.catId, 0, fdwinfo->dobj.dumpId);

	/* Handle the ACL */
	if (fdwinfo->dobj.dump & DUMP_COMPONENT_ACL)
		dumpACL(fout, fdwinfo->dobj.dumpId, InvalidDumpId,
				"FOREIGN DATA WRAPPER", qfdwname, NULL,
				NULL, fdwinfo->rolname,
				fdwinfo->fdwacl, fdwinfo->rfdwacl,
				fdwinfo->initfdwacl, fdwinfo->initrfdwacl);

	free(qfdwname);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
}

/*
 * dumpForeignServer
 *	  write out a foreign server definition
 */
static void
dumpForeignServer(Archive *fout, ForeignServerInfo *srvinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer query;
	PGresult   *res;
	char	   *qsrvname;
	char	   *fdwname;

	/* Skip if not to be dumped */
	if (!srvinfo->dobj.dump || dopt->dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();
	query = createPQExpBuffer();

	qsrvname = pg_strdup(fmtId(srvinfo->dobj.name));

	/* look up the foreign-data wrapper */
	appendPQExpBuffer(query, "SELECT fdwname "
					  "FROM pg_foreign_data_wrapper w "
					  "WHERE w.oid = '%u'",
					  srvinfo->srvfdw);
	res = ExecuteSqlQueryForSingleRow(fout, query->data);
	fdwname = PQgetvalue(res, 0, 0);

	appendPQExpBuffer(q, "CREATE SERVER %s", qsrvname);
	if (srvinfo->srvtype && strlen(srvinfo->srvtype) > 0)
	{
		appendPQExpBufferStr(q, " TYPE ");
		appendStringLiteralAH(q, srvinfo->srvtype, fout);
	}
	if (srvinfo->srvversion && strlen(srvinfo->srvversion) > 0)
	{
		appendPQExpBufferStr(q, " VERSION ");
		appendStringLiteralAH(q, srvinfo->srvversion, fout);
	}

	appendPQExpBufferStr(q, " FOREIGN DATA WRAPPER ");
	appendPQExpBufferStr(q, fmtId(fdwname));

	if (srvinfo->srvoptions && strlen(srvinfo->srvoptions) > 0)
		appendPQExpBuffer(q, " OPTIONS (\n    %s\n)", srvinfo->srvoptions);

	appendPQExpBufferStr(q, ";\n");

	appendPQExpBuffer(delq, "DROP SERVER %s;\n",
					  qsrvname);

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &srvinfo->dobj,
										"SERVER", qsrvname, NULL);

	if (srvinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, srvinfo->dobj.catId, srvinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = srvinfo->dobj.name,
								  .owner = srvinfo->rolname,
								  .description = "SERVER",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* Dump Foreign Server Comments */
	if (srvinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "SERVER", qsrvname,
					NULL, srvinfo->rolname,
					srvinfo->dobj.catId, 0, srvinfo->dobj.dumpId);

	/* Handle the ACL */
	if (srvinfo->dobj.dump & DUMP_COMPONENT_ACL)
		dumpACL(fout, srvinfo->dobj.dumpId, InvalidDumpId,
				"FOREIGN SERVER", qsrvname, NULL,
				NULL, srvinfo->rolname,
				srvinfo->srvacl, srvinfo->rsrvacl,
				srvinfo->initsrvacl, srvinfo->initrsrvacl);

	/* Dump user mappings */
	if (srvinfo->dobj.dump & DUMP_COMPONENT_USERMAP)
		dumpUserMappings(fout,
						 srvinfo->dobj.name, NULL,
						 srvinfo->rolname,
						 srvinfo->dobj.catId, srvinfo->dobj.dumpId);

	free(qsrvname);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(query);
}

/*
 * dumpUserMappings
 *
 * This routine is used to dump any user mappings associated with the
 * server handed to this routine. Should be called after ArchiveEntry()
 * for the server.
 */
static void
dumpUserMappings(Archive *fout,
				 const char *servername, const char *namespace,
				 const char *owner,
				 CatalogId catalogId, DumpId dumpId)
{
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer query;
	PQExpBuffer tag;
	PGresult   *res;
	int			ntups;
	int			i_usename;
	int			i_umoptions;
	int			i;

	q = createPQExpBuffer();
	tag = createPQExpBuffer();
	delq = createPQExpBuffer();
	query = createPQExpBuffer();

	/*
	 * We read from the publicly accessible view pg_user_mappings, so as not
	 * to fail if run by a non-superuser.  Note that the view will show
	 * umoptions as null if the user hasn't got privileges for the associated
	 * server; this means that pg_dump will dump such a mapping, but with no
	 * OPTIONS clause.  A possible alternative is to skip such mappings
	 * altogether, but it's not clear that that's an improvement.
	 */
	appendPQExpBuffer(query,
					  "SELECT usename, "
					  "array_to_string(ARRAY("
					  "SELECT quote_ident(option_name) || ' ' || "
					  "quote_literal(option_value) "
					  "FROM pg_options_to_table(umoptions) "
					  "ORDER BY option_name"
					  "), E',\n    ') AS umoptions "
					  "FROM pg_user_mappings "
					  "WHERE srvid = '%u' "
					  "ORDER BY usename",
					  catalogId.oid);

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	i_usename = PQfnumber(res, "usename");
	i_umoptions = PQfnumber(res, "umoptions");

	for (i = 0; i < ntups; i++)
	{
		char	   *usename;
		char	   *umoptions;

		usename = PQgetvalue(res, i, i_usename);
		umoptions = PQgetvalue(res, i, i_umoptions);

		resetPQExpBuffer(q);
		appendPQExpBuffer(q, "CREATE USER MAPPING FOR %s", fmtId(usename));
		appendPQExpBuffer(q, " SERVER %s", fmtId(servername));

		if (umoptions && strlen(umoptions) > 0)
			appendPQExpBuffer(q, " OPTIONS (\n    %s\n)", umoptions);

		appendPQExpBufferStr(q, ";\n");

		resetPQExpBuffer(delq);
		appendPQExpBuffer(delq, "DROP USER MAPPING FOR %s", fmtId(usename));
		appendPQExpBuffer(delq, " SERVER %s;\n", fmtId(servername));

		resetPQExpBuffer(tag);
		appendPQExpBuffer(tag, "USER MAPPING %s SERVER %s",
						  usename, servername);

		ArchiveEntry(fout, nilCatalogId, createDumpId(),
					 ARCHIVE_OPTS(.tag = tag->data,
								  .namespace = namespace,
								  .owner = owner,
								  .description = "USER MAPPING",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));
	}

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(tag);
	destroyPQExpBuffer(q);
}

/*
 * Write out default privileges information
 */
static void
dumpDefaultACL(Archive *fout, DefaultACLInfo *daclinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PQExpBuffer tag;
	const char *type;

	/* Skip if not to be dumped */
	if (!daclinfo->dobj.dump || dopt->dataOnly || dopt->aclsSkip)
		return;

	q = createPQExpBuffer();
	tag = createPQExpBuffer();

	switch (daclinfo->defaclobjtype)
	{
		case DEFACLOBJ_RELATION:
			type = "TABLES";
			break;
		case DEFACLOBJ_SEQUENCE:
			type = "SEQUENCES";
			break;
		case DEFACLOBJ_FUNCTION:
			type = "FUNCTIONS";
			break;
		case DEFACLOBJ_TYPE:
			type = "TYPES";
			break;
		case DEFACLOBJ_NAMESPACE:
			type = "SCHEMAS";
			break;
		default:
			/* shouldn't get here */
			fatal("unrecognized object type in default privileges: %d",
				  (int) daclinfo->defaclobjtype);
			type = "";			/* keep compiler quiet */
	}

	appendPQExpBuffer(tag, "DEFAULT PRIVILEGES FOR %s", type);

	/* build the actual command(s) for this tuple */
	if (!buildDefaultACLCommands(type,
								 daclinfo->dobj.namespace != NULL ?
								 daclinfo->dobj.namespace->dobj.name : NULL,
								 daclinfo->defaclacl,
								 daclinfo->rdefaclacl,
								 daclinfo->initdefaclacl,
								 daclinfo->initrdefaclacl,
								 daclinfo->defaclrole,
								 fout->remoteVersion,
								 q))
		fatal("could not parse default ACL list (%s)",
			  daclinfo->defaclacl);

	if (daclinfo->dobj.dump & DUMP_COMPONENT_ACL)
		ArchiveEntry(fout, daclinfo->dobj.catId, daclinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tag->data,
								  .namespace = daclinfo->dobj.namespace ?
								  daclinfo->dobj.namespace->dobj.name : NULL,
								  .owner = daclinfo->defaclrole,
								  .description = "DEFAULT ACL",
								  .section = SECTION_POST_DATA,
								  .createStmt = q->data));

	destroyPQExpBuffer(tag);
	destroyPQExpBuffer(q);
}

/*----------
 * Write out grant/revoke information
 *
 * 'objDumpId' is the dump ID of the underlying object.
 * 'altDumpId' can be a second dumpId that the ACL entry must also depend on,
 *		or InvalidDumpId if there is no need for a second dependency.
 * 'type' must be one of
 *		TABLE, SEQUENCE, FUNCTION, LANGUAGE, SCHEMA, DATABASE, TABLESPACE,
 *		FOREIGN DATA WRAPPER, SERVER, or LARGE OBJECT.
 * 'name' is the formatted name of the object.  Must be quoted etc. already.
 * 'subname' is the formatted name of the sub-object, if any.  Must be quoted.
 *		(Currently we assume that subname is only provided for table columns.)
 * 'nspname' is the namespace the object is in (NULL if none).
 * 'owner' is the owner, NULL if there is no owner (for languages).
 * 'acls' contains the ACL string of the object from the appropriate system
 * 		catalog field; it will be passed to buildACLCommands for building the
 * 		appropriate GRANT commands.
 * 'racls' contains the ACL string of any initial-but-now-revoked ACLs of the
 * 		object; it will be passed to buildACLCommands for building the
 * 		appropriate REVOKE commands.
 * 'initacls' In binary-upgrade mode, ACL string of the object's initial
 * 		privileges, to be recorded into pg_init_privs
 * 'initracls' In binary-upgrade mode, ACL string of the object's
 * 		revoked-from-default privileges, to be recorded into pg_init_privs
 *
 * NB: initacls/initracls are needed because extensions can set privileges on
 * an object during the extension's script file and we record those into
 * pg_init_privs as that object's initial privileges.
 *
 * Returns the dump ID assigned to the ACL TocEntry, or InvalidDumpId if
 * no ACL entry was created.
 *----------
 */
static DumpId
dumpACL(Archive *fout, DumpId objDumpId, DumpId altDumpId,
		const char *type, const char *name, const char *subname,
		const char *nspname, const char *owner,
		const char *acls, const char *racls,
		const char *initacls, const char *initracls)
{
	DumpId		aclDumpId = InvalidDumpId;
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer sql;

	/* Do nothing if ACL dump is not enabled */
	if (dopt->aclsSkip)
		return InvalidDumpId;

	/* --data-only skips ACLs *except* BLOB ACLs */
	if (dopt->dataOnly && strcmp(type, "LARGE OBJECT") != 0)
		return InvalidDumpId;

	sql = createPQExpBuffer();

	/*
	 * Check to see if this object has had any initial ACLs included for it.
	 * If so, we are in binary upgrade mode and these are the ACLs to turn
	 * into GRANT and REVOKE statements to set and record the initial
	 * privileges for an extension object.  Let the backend know that these
	 * are to be recorded by calling binary_upgrade_set_record_init_privs()
	 * before and after.
	 */
	if (strlen(initacls) != 0 || strlen(initracls) != 0)
	{
		appendPQExpBufferStr(sql, "SELECT pg_catalog.binary_upgrade_set_record_init_privs(true);\n");
		if (!buildACLCommands(name, subname, nspname, type,
							  initacls, initracls, owner,
							  "", fout->remoteVersion, sql))
			fatal("could not parse initial GRANT ACL list (%s) or initial REVOKE ACL list (%s) for object \"%s\" (%s)",
				  initacls, initracls, name, type);
		appendPQExpBufferStr(sql, "SELECT pg_catalog.binary_upgrade_set_record_init_privs(false);\n");
	}

	if (!buildACLCommands(name, subname, nspname, type,
						  acls, racls, owner,
						  "", fout->remoteVersion, sql))
		fatal("could not parse GRANT ACL list (%s) or REVOKE ACL list (%s) for object \"%s\" (%s)",
			  acls, racls, name, type);

	if (sql->len > 0)
	{
		PQExpBuffer tag = createPQExpBuffer();
		DumpId		aclDeps[2];
		int			nDeps = 0;

		if (subname)
			appendPQExpBuffer(tag, "COLUMN %s.%s", name, subname);
		else
			appendPQExpBuffer(tag, "%s %s", type, name);

		aclDeps[nDeps++] = objDumpId;
		if (altDumpId != InvalidDumpId)
			aclDeps[nDeps++] = altDumpId;

		aclDumpId = createDumpId();

		ArchiveEntry(fout, nilCatalogId, aclDumpId,
					 ARCHIVE_OPTS(.tag = tag->data,
								  .namespace = nspname,
								  .owner = owner,
								  .description = "ACL",
								  .section = SECTION_NONE,
								  .createStmt = sql->data,
								  .deps = aclDeps,
								  .nDeps = nDeps));

		destroyPQExpBuffer(tag);
	}

	destroyPQExpBuffer(sql);

	return aclDumpId;
}

/*
 * dumpSecLabel
 *
 * This routine is used to dump any security labels associated with the
 * object handed to this routine. The routine takes the object type
 * and object name (ready to print, except for schema decoration), plus
 * the namespace and owner of the object (for labeling the ArchiveEntry),
 * plus catalog ID and subid which are the lookup key for pg_seclabel,
 * plus the dump ID for the object (for setting a dependency).
 * If a matching pg_seclabel entry is found, it is dumped.
 *
 * Note: although this routine takes a dumpId for dependency purposes,
 * that purpose is just to mark the dependency in the emitted dump file
 * for possible future use by pg_restore.  We do NOT use it for determining
 * ordering of the label in the dump file, because this routine is called
 * after dependency sorting occurs.  This routine should be called just after
 * calling ArchiveEntry() for the specified object.
 */
static void
dumpSecLabel(Archive *fout, const char *type, const char *name,
			 const char *namespace, const char *owner,
			 CatalogId catalogId, int subid, DumpId dumpId)
{
	DumpOptions *dopt = fout->dopt;
	SecLabelItem *labels;
	int			nlabels;
	int			i;
	PQExpBuffer query;

	/* do nothing, if --no-security-labels is supplied */
	if (dopt->no_security_labels)
		return;

	/* Security labels are schema not data ... except blob labels are data */
	if (strcmp(type, "LARGE OBJECT") != 0)
	{
		if (dopt->dataOnly)
			return;
	}
	else
	{
		/* We do dump blob security labels in binary-upgrade mode */
		if (dopt->schemaOnly && !dopt->binary_upgrade)
			return;
	}

	/* Search for security labels associated with catalogId, using table */
	nlabels = findSecLabels(fout, catalogId.tableoid, catalogId.oid, &labels);

	query = createPQExpBuffer();

	for (i = 0; i < nlabels; i++)
	{
		/*
		 * Ignore label entries for which the subid doesn't match.
		 */
		if (labels[i].objsubid != subid)
			continue;

		appendPQExpBuffer(query,
						  "SECURITY LABEL FOR %s ON %s ",
						  fmtId(labels[i].provider), type);
		if (namespace && *namespace)
			appendPQExpBuffer(query, "%s.", fmtId(namespace));
		appendPQExpBuffer(query, "%s IS ", name);
		appendStringLiteralAH(query, labels[i].label, fout);
		appendPQExpBufferStr(query, ";\n");
	}

	if (query->len > 0)
	{
		PQExpBuffer tag = createPQExpBuffer();

		appendPQExpBuffer(tag, "%s %s", type, name);
		ArchiveEntry(fout, nilCatalogId, createDumpId(),
					 ARCHIVE_OPTS(.tag = tag->data,
								  .namespace = namespace,
								  .owner = owner,
								  .description = "SECURITY LABEL",
								  .section = SECTION_NONE,
								  .createStmt = query->data,
								  .deps = &dumpId,
								  .nDeps = 1));
		destroyPQExpBuffer(tag);
	}

	destroyPQExpBuffer(query);
}

/*
 * dumpTableSecLabel
 *
 * As above, but dump security label for both the specified table (or view)
 * and its columns.
 */
static void
dumpTableSecLabel(Archive *fout, TableInfo *tbinfo, const char *reltypename)
{
	DumpOptions *dopt = fout->dopt;
	SecLabelItem *labels;
	int			nlabels;
	int			i;
	PQExpBuffer query;
	PQExpBuffer target;

	/* do nothing, if --no-security-labels is supplied */
	if (dopt->no_security_labels)
		return;

	/* SecLabel are SCHEMA not data */
	if (dopt->dataOnly)
		return;

	/* Search for comments associated with relation, using table */
	nlabels = findSecLabels(fout,
							tbinfo->dobj.catId.tableoid,
							tbinfo->dobj.catId.oid,
							&labels);

	/* If security labels exist, build SECURITY LABEL statements */
	if (nlabels <= 0)
		return;

	query = createPQExpBuffer();
	target = createPQExpBuffer();

	for (i = 0; i < nlabels; i++)
	{
		const char *colname;
		const char *provider = labels[i].provider;
		const char *label = labels[i].label;
		int			objsubid = labels[i].objsubid;

		resetPQExpBuffer(target);
		if (objsubid == 0)
		{
			appendPQExpBuffer(target, "%s %s", reltypename,
							  fmtQualifiedDumpable(tbinfo));
		}
		else
		{
			colname = getAttrName(objsubid, tbinfo);
			/* first fmtXXX result must be consumed before calling again */
			appendPQExpBuffer(target, "COLUMN %s",
							  fmtQualifiedDumpable(tbinfo));
			appendPQExpBuffer(target, ".%s", fmtId(colname));
		}
		appendPQExpBuffer(query, "SECURITY LABEL FOR %s ON %s IS ",
						  fmtId(provider), target->data);
		appendStringLiteralAH(query, label, fout);
		appendPQExpBufferStr(query, ";\n");
	}
	if (query->len > 0)
	{
		resetPQExpBuffer(target);
		appendPQExpBuffer(target, "%s %s", reltypename,
						  fmtId(tbinfo->dobj.name));
		ArchiveEntry(fout, nilCatalogId, createDumpId(),
					 ARCHIVE_OPTS(.tag = target->data,
								  .namespace = tbinfo->dobj.namespace->dobj.name,
								  .owner = tbinfo->rolname,
								  .description = "SECURITY LABEL",
								  .section = SECTION_NONE,
								  .createStmt = query->data,
								  .deps = &(tbinfo->dobj.dumpId),
								  .nDeps = 1));
	}
	destroyPQExpBuffer(query);
	destroyPQExpBuffer(target);
}

/*
 * findSecLabels
 *
 * Find the security label(s), if any, associated with the given object.
 * All the objsubid values associated with the given classoid/objoid are
 * found with one search.
 */
static int
findSecLabels(Archive *fout, Oid classoid, Oid objoid, SecLabelItem **items)
{
	/* static storage for table of security labels */
	static SecLabelItem *labels = NULL;
	static int	nlabels = -1;

	SecLabelItem *middle = NULL;
	SecLabelItem *low;
	SecLabelItem *high;
	int			nmatch;

	/* Get security labels if we didn't already */
	if (nlabels < 0)
		nlabels = collectSecLabels(fout, &labels);

	if (nlabels <= 0)			/* no labels, so no match is possible */
	{
		*items = NULL;
		return 0;
	}

	/*
	 * Do binary search to find some item matching the object.
	 */
	low = &labels[0];
	high = &labels[nlabels - 1];
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
 * collectSecLabels
 *
 * Construct a table of all security labels available for database objects.
 * It's much faster to pull them all at once.
 *
 * The table is sorted by classoid/objid/objsubid for speed in lookup.
 */
static int
collectSecLabels(Archive *fout, SecLabelItem **items)
{
	PGresult   *res;
	PQExpBuffer query;
	int			i_label;
	int			i_provider;
	int			i_classoid;
	int			i_objoid;
	int			i_objsubid;
	int			ntups;
	int			i;
	SecLabelItem *labels;

	query = createPQExpBuffer();

	appendPQExpBufferStr(query,
						 "SELECT label, provider, classoid, objoid, objsubid "
						 "FROM pg_catalog.pg_seclabel "
						 "ORDER BY classoid, objoid, objsubid");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	/* Construct lookup table containing OIDs in numeric form */
	i_label = PQfnumber(res, "label");
	i_provider = PQfnumber(res, "provider");
	i_classoid = PQfnumber(res, "classoid");
	i_objoid = PQfnumber(res, "objoid");
	i_objsubid = PQfnumber(res, "objsubid");

	ntups = PQntuples(res);

	labels = (SecLabelItem *) pg_malloc(ntups * sizeof(SecLabelItem));

	for (i = 0; i < ntups; i++)
	{
		labels[i].label = PQgetvalue(res, i, i_label);
		labels[i].provider = PQgetvalue(res, i, i_provider);
		labels[i].classoid = atooid(PQgetvalue(res, i, i_classoid));
		labels[i].objoid = atooid(PQgetvalue(res, i, i_objoid));
		labels[i].objsubid = atoi(PQgetvalue(res, i, i_objsubid));
	}

	/* Do NOT free the PGresult since we are keeping pointers into it */
	destroyPQExpBuffer(query);

	*items = labels;
	return ntups;
}

/*
 * dumpTable
 *	  write out to fout the declarations (not data) of a user-defined table
 */
static void
dumpTable(Archive *fout, TableInfo *tbinfo)
{
	DumpOptions *dopt = fout->dopt;
	DumpId		tableAclDumpId = InvalidDumpId;
	char	   *namecopy;

	/*
	 * noop if we are not dumping anything about this table, or if we are
	 * doing a data-only dump
	 */
	if (!tbinfo->dobj.dump || dopt->dataOnly)
		return;

	if (tbinfo->relkind == RELKIND_SEQUENCE)
		dumpSequence(fout, tbinfo);
	else
		dumpTableSchema(fout, tbinfo);

	/* Handle the ACL here */
	namecopy = pg_strdup(fmtId(tbinfo->dobj.name));
	if (tbinfo->dobj.dump & DUMP_COMPONENT_ACL)
	{
		const char *objtype =
		(tbinfo->relkind == RELKIND_SEQUENCE) ? "SEQUENCE" : "TABLE";

		tableAclDumpId =
			dumpACL(fout, tbinfo->dobj.dumpId, InvalidDumpId,
					objtype, namecopy, NULL,
					tbinfo->dobj.namespace->dobj.name, tbinfo->rolname,
					tbinfo->relacl, tbinfo->rrelacl,
					tbinfo->initrelacl, tbinfo->initrrelacl);
	}

	/*
	 * Handle column ACLs, if any.  Note: we pull these with a separate query
	 * rather than trying to fetch them during getTableAttrs, so that we won't
	 * miss ACLs on system columns.
	 */
	if (fout->remoteVersion >= 80400 && tbinfo->dobj.dump & DUMP_COMPONENT_ACL)
	{
		PQExpBuffer query = createPQExpBuffer();
		PGresult   *res;
		int			i;

		if (fout->remoteVersion >= 90600)
		{
			PQExpBuffer acl_subquery = createPQExpBuffer();
			PQExpBuffer racl_subquery = createPQExpBuffer();
			PQExpBuffer initacl_subquery = createPQExpBuffer();
			PQExpBuffer initracl_subquery = createPQExpBuffer();

			buildACLQueries(acl_subquery, racl_subquery, initacl_subquery,
							initracl_subquery, "at.attacl", "c.relowner", "'c'",
							dopt->binary_upgrade);

			appendPQExpBuffer(query,
							  "SELECT at.attname, "
							  "%s AS attacl, "
							  "%s AS rattacl, "
							  "%s AS initattacl, "
							  "%s AS initrattacl "
							  "FROM pg_catalog.pg_attribute at "
							  "JOIN pg_catalog.pg_class c ON (at.attrelid = c.oid) "
							  "LEFT JOIN pg_catalog.pg_init_privs pip ON "
							  "(at.attrelid = pip.objoid "
							  "AND pip.classoid = 'pg_catalog.pg_class'::pg_catalog.regclass "
							  "AND at.attnum = pip.objsubid) "
							  "WHERE at.attrelid = '%u'::pg_catalog.oid AND "
							  "NOT at.attisdropped "
							  "AND ("
							  "%s IS NOT NULL OR "
							  "%s IS NOT NULL OR "
							  "%s IS NOT NULL OR "
							  "%s IS NOT NULL)"
							  "ORDER BY at.attnum",
							  acl_subquery->data,
							  racl_subquery->data,
							  initacl_subquery->data,
							  initracl_subquery->data,
							  tbinfo->dobj.catId.oid,
							  acl_subquery->data,
							  racl_subquery->data,
							  initacl_subquery->data,
							  initracl_subquery->data);

			destroyPQExpBuffer(acl_subquery);
			destroyPQExpBuffer(racl_subquery);
			destroyPQExpBuffer(initacl_subquery);
			destroyPQExpBuffer(initracl_subquery);
		}
		else
		{
			appendPQExpBuffer(query,
							  "SELECT attname, attacl, NULL as rattacl, "
							  "NULL AS initattacl, NULL AS initrattacl "
							  "FROM pg_catalog.pg_attribute "
							  "WHERE attrelid = '%u'::pg_catalog.oid AND NOT attisdropped "
							  "AND attacl IS NOT NULL "
							  "ORDER BY attnum",
							  tbinfo->dobj.catId.oid);
		}

		res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

		for (i = 0; i < PQntuples(res); i++)
		{
			char	   *attname = PQgetvalue(res, i, 0);
			char	   *attacl = PQgetvalue(res, i, 1);
			char	   *rattacl = PQgetvalue(res, i, 2);
			char	   *initattacl = PQgetvalue(res, i, 3);
			char	   *initrattacl = PQgetvalue(res, i, 4);
			char	   *attnamecopy;

			attnamecopy = pg_strdup(fmtId(attname));

			/*
			 * Column's GRANT type is always TABLE.  Each column ACL depends
			 * on the table-level ACL, since we can restore column ACLs in
			 * parallel but the table-level ACL has to be done first.
			 */
			dumpACL(fout, tbinfo->dobj.dumpId, tableAclDumpId,
					"TABLE", namecopy, attnamecopy,
					tbinfo->dobj.namespace->dobj.name, tbinfo->rolname,
					attacl, rattacl, initattacl, initrattacl);
			free(attnamecopy);
		}
		PQclear(res);
		destroyPQExpBuffer(query);
	}

	free(namecopy);
}

/*
 * Create the AS clause for a view or materialized view. The semicolon is
 * stripped because a materialized view must add a WITH NO DATA clause.
 *
 * This returns a new buffer which must be freed by the caller.
 */
static PQExpBuffer
createViewAsClause(Archive *fout, TableInfo *tbinfo)
{
	PQExpBuffer query = createPQExpBuffer();
	PQExpBuffer result = createPQExpBuffer();
	PGresult   *res;
	int			len;

	/* Fetch the view definition */
	appendPQExpBuffer(query,
					  "SELECT pg_catalog.pg_get_viewdef('%u'::pg_catalog.oid) AS viewdef",
					  tbinfo->dobj.catId.oid);

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	if (PQntuples(res) != 1)
	{
		if (PQntuples(res) < 1)
			fatal("query to obtain definition of view \"%s\" returned no data",
				  tbinfo->dobj.name);
		else
			fatal("query to obtain definition of view \"%s\" returned more than one definition",
				  tbinfo->dobj.name);
	}

	len = PQgetlength(res, 0, 0);

	if (len == 0)
		fatal("definition of view \"%s\" appears to be empty (length zero)",
			  tbinfo->dobj.name);

	/* Strip off the trailing semicolon so that other things may follow. */
	Assert(PQgetvalue(res, 0, 0)[len - 1] == ';');
	appendBinaryPQExpBuffer(result, PQgetvalue(res, 0, 0), len - 1);

	PQclear(res);
	destroyPQExpBuffer(query);

	return result;
}

/*
 * Create a dummy AS clause for a view.  This is used when the real view
 * definition has to be postponed because of circular dependencies.
 * We must duplicate the view's external properties -- column names and types
 * (including collation) -- so that it works for subsequent references.
 *
 * This returns a new buffer which must be freed by the caller.
 */
static PQExpBuffer
createDummyViewAsClause(Archive *fout, TableInfo *tbinfo)
{
	PQExpBuffer result = createPQExpBuffer();
	int			j;

	appendPQExpBufferStr(result, "SELECT");

	for (j = 0; j < tbinfo->numatts; j++)
	{
		if (j > 0)
			appendPQExpBufferChar(result, ',');
		appendPQExpBufferStr(result, "\n    ");

		appendPQExpBuffer(result, "NULL::%s", tbinfo->atttypnames[j]);

		/*
		 * Must add collation if not default for the type, because CREATE OR
		 * REPLACE VIEW won't change it
		 */
		if (OidIsValid(tbinfo->attcollation[j]))
		{
			CollInfo   *coll;

			coll = findCollationByOid(tbinfo->attcollation[j]);
			if (coll)
				appendPQExpBuffer(result, " COLLATE %s",
								  fmtQualifiedDumpable(coll));
		}

		appendPQExpBuffer(result, " AS %s", fmtId(tbinfo->attnames[j]));
	}

	return result;
}

/*
 * dumpTableSchema
 *	  write the declaration (not data) of one user-defined table or view
 */
static void
dumpTableSchema(Archive *fout, TableInfo *tbinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	char	   *qrelname;
	char	   *qualrelname;
	int			numParents;
	TableInfo **parents;
	int			actual_atts;	/* number of attrs in this CREATE statement */
	const char *reltypename;
	char	   *storage;
	int			j,
				k;

	/* We had better have loaded per-column details about this table */
	Assert(tbinfo->interesting);

	qrelname = pg_strdup(fmtId(tbinfo->dobj.name));
	qualrelname = pg_strdup(fmtQualifiedDumpable(tbinfo));

	if (tbinfo->hasoids)
		pg_log_warning("WITH OIDS is not supported anymore (table \"%s\")",
					   qrelname);

	if (dopt->binary_upgrade)
		binary_upgrade_set_type_oids_by_rel_oid(fout, q,
												tbinfo->dobj.catId.oid);

	/* Is it a table or a view? */
	if (tbinfo->relkind == RELKIND_VIEW)
	{
		PQExpBuffer result;

		/*
		 * Note: keep this code in sync with the is_view case in dumpRule()
		 */

		reltypename = "VIEW";

		appendPQExpBuffer(delq, "DROP VIEW %s;\n", qualrelname);

		if (dopt->binary_upgrade)
			binary_upgrade_set_pg_class_oids(fout, q,
											 tbinfo->dobj.catId.oid, false);

		appendPQExpBuffer(q, "CREATE VIEW %s", qualrelname);

		if (tbinfo->dummy_view)
			result = createDummyViewAsClause(fout, tbinfo);
		else
		{
			if (nonemptyReloptions(tbinfo->reloptions))
			{
				appendPQExpBufferStr(q, " WITH (");
				appendReloptionsArrayAH(q, tbinfo->reloptions, "", fout);
				appendPQExpBufferChar(q, ')');
			}
			result = createViewAsClause(fout, tbinfo);
		}
		appendPQExpBuffer(q, " AS\n%s", result->data);
		destroyPQExpBuffer(result);

		if (tbinfo->checkoption != NULL && !tbinfo->dummy_view)
			appendPQExpBuffer(q, "\n  WITH %s CHECK OPTION", tbinfo->checkoption);
		appendPQExpBufferStr(q, ";\n");
	}
	else
	{
		char	   *ftoptions = NULL;
		char	   *srvname = NULL;
		char	   *foreign = "";

		switch (tbinfo->relkind)
		{
			case RELKIND_FOREIGN_TABLE:
				{
					PQExpBuffer query = createPQExpBuffer();
					PGresult   *res;
					int			i_srvname;
					int			i_ftoptions;

					reltypename = "FOREIGN TABLE";

					/* retrieve name of foreign server and generic options */
					appendPQExpBuffer(query,
									  "SELECT fs.srvname, "
									  "pg_catalog.array_to_string(ARRAY("
									  "SELECT pg_catalog.quote_ident(option_name) || "
									  "' ' || pg_catalog.quote_literal(option_value) "
									  "FROM pg_catalog.pg_options_to_table(ftoptions) "
									  "ORDER BY option_name"
									  "), E',\n    ') AS ftoptions "
									  "FROM pg_catalog.pg_foreign_table ft "
									  "JOIN pg_catalog.pg_foreign_server fs "
									  "ON (fs.oid = ft.ftserver) "
									  "WHERE ft.ftrelid = '%u'",
									  tbinfo->dobj.catId.oid);
					res = ExecuteSqlQueryForSingleRow(fout, query->data);
					i_srvname = PQfnumber(res, "srvname");
					i_ftoptions = PQfnumber(res, "ftoptions");
					srvname = pg_strdup(PQgetvalue(res, 0, i_srvname));
					ftoptions = pg_strdup(PQgetvalue(res, 0, i_ftoptions));
					PQclear(res);
					destroyPQExpBuffer(query);

					foreign = "FOREIGN ";
					break;
				}
			case RELKIND_MATVIEW:
				reltypename = "MATERIALIZED VIEW";
				break;
			default:
				reltypename = "TABLE";
		}

		numParents = tbinfo->numParents;
		parents = tbinfo->parents;

		appendPQExpBuffer(delq, "DROP %s %s;\n", reltypename, qualrelname);

		if (dopt->binary_upgrade)
			binary_upgrade_set_pg_class_oids(fout, q,
											 tbinfo->dobj.catId.oid, false);

		appendPQExpBuffer(q, "CREATE %s%s %s",
						  tbinfo->relpersistence == RELPERSISTENCE_UNLOGGED ?
						  "UNLOGGED " : "",
						  reltypename,
						  qualrelname);

		/*
		 * Attach to type, if reloftype; except in case of a binary upgrade,
		 * we dump the table normally and attach it to the type afterward.
		 */
		if (tbinfo->reloftype && !dopt->binary_upgrade)
			appendPQExpBuffer(q, " OF %s", tbinfo->reloftype);

		if (tbinfo->relkind != RELKIND_MATVIEW)
		{
			/* Dump the attributes */
			actual_atts = 0;
			for (j = 0; j < tbinfo->numatts; j++)
			{
				/*
				 * Normally, dump if it's locally defined in this table, and
				 * not dropped.  But for binary upgrade, we'll dump all the
				 * columns, and then fix up the dropped and nonlocal cases
				 * below.
				 */
				if (shouldPrintColumn(dopt, tbinfo, j))
				{
					bool		print_default;
					bool		print_notnull;

					/*
					 * Default value --- suppress if to be printed separately.
					 */
					print_default = (tbinfo->attrdefs[j] != NULL &&
									 !tbinfo->attrdefs[j]->separate);

					/*
					 * Not Null constraint --- suppress if inherited, except
					 * if partition, or in binary-upgrade case where that
					 * won't work.
					 */
					print_notnull = (tbinfo->notnull[j] &&
									 (!tbinfo->inhNotNull[j] ||
									  tbinfo->ispartition || dopt->binary_upgrade));

					/*
					 * Skip column if fully defined by reloftype, except in
					 * binary upgrade
					 */
					if (tbinfo->reloftype && !print_default && !print_notnull &&
						!dopt->binary_upgrade)
						continue;

					/* Format properly if not first attr */
					if (actual_atts == 0)
						appendPQExpBufferStr(q, " (");
					else
						appendPQExpBufferChar(q, ',');
					appendPQExpBufferStr(q, "\n    ");
					actual_atts++;

					/* Attribute name */
					appendPQExpBufferStr(q, fmtId(tbinfo->attnames[j]));

					if (tbinfo->attisdropped[j])
					{
						/*
						 * ALTER TABLE DROP COLUMN clears
						 * pg_attribute.atttypid, so we will not have gotten a
						 * valid type name; insert INTEGER as a stopgap. We'll
						 * clean things up later.
						 */
						appendPQExpBufferStr(q, " INTEGER /* dummy */");
						/* and skip to the next column */
						continue;
					}

					/*
					 * Attribute type; print it except when creating a typed
					 * table ('OF type_name'), but in binary-upgrade mode,
					 * print it in that case too.
					 */
					if (dopt->binary_upgrade || !tbinfo->reloftype)
					{
						appendPQExpBuffer(q, " %s",
										  tbinfo->atttypnames[j]);
					}

					if (print_default)
					{
						if (tbinfo->attgenerated[j] == ATTRIBUTE_GENERATED_STORED)
							appendPQExpBuffer(q, " GENERATED ALWAYS AS (%s) STORED",
											  tbinfo->attrdefs[j]->adef_expr);
						else
							appendPQExpBuffer(q, " DEFAULT %s",
											  tbinfo->attrdefs[j]->adef_expr);
					}


					if (print_notnull)
						appendPQExpBufferStr(q, " NOT NULL");

					/* Add collation if not default for the type */
					if (OidIsValid(tbinfo->attcollation[j]))
					{
						CollInfo   *coll;

						coll = findCollationByOid(tbinfo->attcollation[j]);
						if (coll)
							appendPQExpBuffer(q, " COLLATE %s",
											  fmtQualifiedDumpable(coll));
					}
				}
			}

			/*
			 * Add non-inherited CHECK constraints, if any.
			 *
			 * For partitions, we need to include check constraints even if
			 * they're not defined locally, because the ALTER TABLE ATTACH
			 * PARTITION that we'll emit later expects the constraint to be
			 * there.  (No need to fix conislocal: ATTACH PARTITION does that)
			 */
			for (j = 0; j < tbinfo->ncheck; j++)
			{
				ConstraintInfo *constr = &(tbinfo->checkexprs[j]);

				if (constr->separate ||
					(!constr->conislocal && !tbinfo->ispartition))
					continue;

				if (actual_atts == 0)
					appendPQExpBufferStr(q, " (\n    ");
				else
					appendPQExpBufferStr(q, ",\n    ");

				appendPQExpBuffer(q, "CONSTRAINT %s ",
								  fmtId(constr->dobj.name));
				appendPQExpBufferStr(q, constr->condef);

				actual_atts++;
			}

			if (actual_atts)
				appendPQExpBufferStr(q, "\n)");
			else if (!(tbinfo->reloftype && !dopt->binary_upgrade))
			{
				/*
				 * No attributes? we must have a parenthesized attribute list,
				 * even though empty, when not using the OF TYPE syntax.
				 */
				appendPQExpBufferStr(q, " (\n)");
			}

			/*
			 * Emit the INHERITS clause (not for partitions), except in
			 * binary-upgrade mode.
			 */
			if (numParents > 0 && !tbinfo->ispartition &&
				!dopt->binary_upgrade)
			{
				appendPQExpBufferStr(q, "\nINHERITS (");
				for (k = 0; k < numParents; k++)
				{
					TableInfo  *parentRel = parents[k];

					if (k > 0)
						appendPQExpBufferStr(q, ", ");
					appendPQExpBufferStr(q, fmtQualifiedDumpable(parentRel));
				}
				appendPQExpBufferChar(q, ')');
			}

			if (tbinfo->relkind == RELKIND_PARTITIONED_TABLE)
				appendPQExpBuffer(q, "\nPARTITION BY %s", tbinfo->partkeydef);

			if (tbinfo->relkind == RELKIND_FOREIGN_TABLE)
				appendPQExpBuffer(q, "\nSERVER %s", fmtId(srvname));
		}

		if (nonemptyReloptions(tbinfo->reloptions) ||
			nonemptyReloptions(tbinfo->toast_reloptions))
		{
			bool		addcomma = false;

			appendPQExpBufferStr(q, "\nWITH (");
			if (nonemptyReloptions(tbinfo->reloptions))
			{
				addcomma = true;
				appendReloptionsArrayAH(q, tbinfo->reloptions, "", fout);
			}
			if (nonemptyReloptions(tbinfo->toast_reloptions))
			{
				if (addcomma)
					appendPQExpBufferStr(q, ", ");
				appendReloptionsArrayAH(q, tbinfo->toast_reloptions, "toast.",
										fout);
			}
			appendPQExpBufferChar(q, ')');
		}

		/* Dump generic options if any */
		if (ftoptions && ftoptions[0])
			appendPQExpBuffer(q, "\nOPTIONS (\n    %s\n)", ftoptions);

		/*
		 * For materialized views, create the AS clause just like a view. At
		 * this point, we always mark the view as not populated.
		 */
		if (tbinfo->relkind == RELKIND_MATVIEW)
		{
			PQExpBuffer result;

			result = createViewAsClause(fout, tbinfo);
			appendPQExpBuffer(q, " AS\n%s\n  WITH NO DATA;\n",
							  result->data);
			destroyPQExpBuffer(result);
		}
		else
			appendPQExpBufferStr(q, ";\n");

		/* Materialized views can depend on extensions */
		if (tbinfo->relkind == RELKIND_MATVIEW)
			append_depends_on_extension(fout, q, &tbinfo->dobj,
										"pg_catalog.pg_class",
										tbinfo->relkind == RELKIND_MATVIEW ?
										"MATERIALIZED VIEW" : "INDEX",
										qualrelname);

		/*
		 * in binary upgrade mode, update the catalog with any missing values
		 * that might be present.
		 */
		if (dopt->binary_upgrade)
		{
			for (j = 0; j < tbinfo->numatts; j++)
			{
				if (tbinfo->attmissingval[j][0] != '\0')
				{
					appendPQExpBufferStr(q, "\n-- set missing value.\n");
					appendPQExpBufferStr(q,
										 "SELECT pg_catalog.binary_upgrade_set_missing_value(");
					appendStringLiteralAH(q, qualrelname, fout);
					appendPQExpBufferStr(q, "::pg_catalog.regclass,");
					appendStringLiteralAH(q, tbinfo->attnames[j], fout);
					appendPQExpBufferStr(q, ",");
					appendStringLiteralAH(q, tbinfo->attmissingval[j], fout);
					appendPQExpBufferStr(q, ");\n\n");
				}
			}
		}

		/*
		 * To create binary-compatible heap files, we have to ensure the same
		 * physical column order, including dropped columns, as in the
		 * original.  Therefore, we create dropped columns above and drop them
		 * here, also updating their attlen/attalign values so that the
		 * dropped column can be skipped properly.  (We do not bother with
		 * restoring the original attbyval setting.)  Also, inheritance
		 * relationships are set up by doing ALTER TABLE INHERIT rather than
		 * using an INHERITS clause --- the latter would possibly mess up the
		 * column order.  That also means we have to take care about setting
		 * attislocal correctly, plus fix up any inherited CHECK constraints.
		 * Analogously, we set up typed tables using ALTER TABLE / OF here.
		 *
		 * We process foreign and partitioned tables here, even though they
		 * lack heap storage, because they can participate in inheritance
		 * relationships and we want this stuff to be consistent across the
		 * inheritance tree.  We can exclude indexes, toast tables, sequences
		 * and matviews, even though they have storage, because we don't
		 * support altering or dropping columns in them, nor can they be part
		 * of inheritance trees.
		 */
		if (dopt->binary_upgrade &&
			(tbinfo->relkind == RELKIND_RELATION ||
			 tbinfo->relkind == RELKIND_FOREIGN_TABLE ||
			 tbinfo->relkind == RELKIND_PARTITIONED_TABLE))
		{
			for (j = 0; j < tbinfo->numatts; j++)
			{
				if (tbinfo->attisdropped[j])
				{
					appendPQExpBufferStr(q, "\n-- For binary upgrade, recreate dropped column.\n");
					appendPQExpBuffer(q, "UPDATE pg_catalog.pg_attribute\n"
									  "SET attlen = %d, "
									  "attalign = '%c', attbyval = false\n"
									  "WHERE attname = ",
									  tbinfo->attlen[j],
									  tbinfo->attalign[j]);
					appendStringLiteralAH(q, tbinfo->attnames[j], fout);
					appendPQExpBufferStr(q, "\n  AND attrelid = ");
					appendStringLiteralAH(q, qualrelname, fout);
					appendPQExpBufferStr(q, "::pg_catalog.regclass;\n");

					if (tbinfo->relkind == RELKIND_RELATION ||
						tbinfo->relkind == RELKIND_PARTITIONED_TABLE)
						appendPQExpBuffer(q, "ALTER TABLE ONLY %s ",
										  qualrelname);
					else
						appendPQExpBuffer(q, "ALTER FOREIGN TABLE ONLY %s ",
										  qualrelname);
					appendPQExpBuffer(q, "DROP COLUMN %s;\n",
									  fmtId(tbinfo->attnames[j]));
				}
				else if (!tbinfo->attislocal[j])
				{
					appendPQExpBufferStr(q, "\n-- For binary upgrade, recreate inherited column.\n");
					appendPQExpBufferStr(q, "UPDATE pg_catalog.pg_attribute\n"
										 "SET attislocal = false\n"
										 "WHERE attname = ");
					appendStringLiteralAH(q, tbinfo->attnames[j], fout);
					appendPQExpBufferStr(q, "\n  AND attrelid = ");
					appendStringLiteralAH(q, qualrelname, fout);
					appendPQExpBufferStr(q, "::pg_catalog.regclass;\n");
				}
			}

			/*
			 * Add inherited CHECK constraints, if any.
			 *
			 * For partitions, they were already dumped, and conislocal
			 * doesn't need fixing.
			 */
			for (k = 0; k < tbinfo->ncheck; k++)
			{
				ConstraintInfo *constr = &(tbinfo->checkexprs[k]);

				if (constr->separate || constr->conislocal || tbinfo->ispartition)
					continue;

				appendPQExpBufferStr(q, "\n-- For binary upgrade, set up inherited constraint.\n");
				appendPQExpBuffer(q, "ALTER %sTABLE ONLY %s ADD CONSTRAINT %s %s;\n",
								  foreign, qualrelname,
								  fmtId(constr->dobj.name),
								  constr->condef);
				appendPQExpBufferStr(q, "UPDATE pg_catalog.pg_constraint\n"
									 "SET conislocal = false\n"
									 "WHERE contype = 'c' AND conname = ");
				appendStringLiteralAH(q, constr->dobj.name, fout);
				appendPQExpBufferStr(q, "\n  AND conrelid = ");
				appendStringLiteralAH(q, qualrelname, fout);
				appendPQExpBufferStr(q, "::pg_catalog.regclass;\n");
			}

			if (numParents > 0 && !tbinfo->ispartition)
			{
				appendPQExpBufferStr(q, "\n-- For binary upgrade, set up inheritance this way.\n");
				for (k = 0; k < numParents; k++)
				{
					TableInfo  *parentRel = parents[k];

					appendPQExpBuffer(q, "ALTER %sTABLE ONLY %s INHERIT %s;\n", foreign,
									  qualrelname,
									  fmtQualifiedDumpable(parentRel));
				}
			}

			if (tbinfo->reloftype)
			{
				appendPQExpBufferStr(q, "\n-- For binary upgrade, set up typed tables this way.\n");
				appendPQExpBuffer(q, "ALTER TABLE ONLY %s OF %s;\n",
								  qualrelname,
								  tbinfo->reloftype);
			}
		}

		/*
		 * For partitioned tables, emit the ATTACH PARTITION clause.  Note
		 * that we always want to create partitions this way instead of using
		 * CREATE TABLE .. PARTITION OF, mainly to preserve a possible column
		 * layout discrepancy with the parent, but also to ensure it gets the
		 * correct tablespace setting if it differs from the parent's.
		 */
		if (tbinfo->ispartition)
		{
			/* With partitions there can only be one parent */
			if (tbinfo->numParents != 1)
				fatal("invalid number of parents %d for table \"%s\"",
					  tbinfo->numParents, tbinfo->dobj.name);

			/* Perform ALTER TABLE on the parent */
			appendPQExpBuffer(q,
							  "ALTER TABLE ONLY %s ATTACH PARTITION %s %s;\n",
							  fmtQualifiedDumpable(parents[0]),
							  qualrelname, tbinfo->partbound);
		}

		/*
		 * In binary_upgrade mode, arrange to restore the old relfrozenxid and
		 * relminmxid of all vacuumable relations.  (While vacuum.c processes
		 * TOAST tables semi-independently, here we see them only as children
		 * of other relations; so this "if" lacks RELKIND_TOASTVALUE, and the
		 * child toast table is handled below.)
		 */
		if (dopt->binary_upgrade &&
			(tbinfo->relkind == RELKIND_RELATION ||
			 tbinfo->relkind == RELKIND_MATVIEW))
		{
			appendPQExpBufferStr(q, "\n-- For binary upgrade, set heap's relfrozenxid and relminmxid\n");
			appendPQExpBuffer(q, "UPDATE pg_catalog.pg_class\n"
							  "SET relfrozenxid = '%u', relminmxid = '%u'\n"
							  "WHERE oid = ",
							  tbinfo->frozenxid, tbinfo->minmxid);
			appendStringLiteralAH(q, qualrelname, fout);
			appendPQExpBufferStr(q, "::pg_catalog.regclass;\n");

			if (tbinfo->toast_oid)
			{
				/*
				 * The toast table will have the same OID at restore, so we
				 * can safely target it by OID.
				 */
				appendPQExpBufferStr(q, "\n-- For binary upgrade, set toast's relfrozenxid and relminmxid\n");
				appendPQExpBuffer(q, "UPDATE pg_catalog.pg_class\n"
								  "SET relfrozenxid = '%u', relminmxid = '%u'\n"
								  "WHERE oid = '%u';\n",
								  tbinfo->toast_frozenxid,
								  tbinfo->toast_minmxid, tbinfo->toast_oid);
			}
		}

		/*
		 * In binary_upgrade mode, restore matviews' populated status by
		 * poking pg_class directly.  This is pretty ugly, but we can't use
		 * REFRESH MATERIALIZED VIEW since it's possible that some underlying
		 * matview is not populated even though this matview is; in any case,
		 * we want to transfer the matview's heap storage, not run REFRESH.
		 */
		if (dopt->binary_upgrade && tbinfo->relkind == RELKIND_MATVIEW &&
			tbinfo->relispopulated)
		{
			appendPQExpBufferStr(q, "\n-- For binary upgrade, mark materialized view as populated\n");
			appendPQExpBufferStr(q, "UPDATE pg_catalog.pg_class\n"
								 "SET relispopulated = 't'\n"
								 "WHERE oid = ");
			appendStringLiteralAH(q, qualrelname, fout);
			appendPQExpBufferStr(q, "::pg_catalog.regclass;\n");
		}

		/*
		 * Dump additional per-column properties that we can't handle in the
		 * main CREATE TABLE command.
		 */
		for (j = 0; j < tbinfo->numatts; j++)
		{
			/* None of this applies to dropped columns */
			if (tbinfo->attisdropped[j])
				continue;

			/*
			 * If we didn't dump the column definition explicitly above, and
			 * it is NOT NULL and did not inherit that property from a parent,
			 * we have to mark it separately.
			 */
			if (!shouldPrintColumn(dopt, tbinfo, j) &&
				tbinfo->notnull[j] && !tbinfo->inhNotNull[j])
				appendPQExpBuffer(q,
								  "ALTER %sTABLE ONLY %s ALTER COLUMN %s SET NOT NULL;\n",
								  foreign, qualrelname,
								  fmtId(tbinfo->attnames[j]));

			/*
			 * Dump per-column statistics information. We only issue an ALTER
			 * TABLE statement if the attstattarget entry for this column is
			 * non-negative (i.e. it's not the default value)
			 */
			if (tbinfo->attstattarget[j] >= 0)
				appendPQExpBuffer(q, "ALTER %sTABLE ONLY %s ALTER COLUMN %s SET STATISTICS %d;\n",
								  foreign, qualrelname,
								  fmtId(tbinfo->attnames[j]),
								  tbinfo->attstattarget[j]);

			/*
			 * Dump per-column storage information.  The statement is only
			 * dumped if the storage has been changed from the type's default.
			 */
			if (tbinfo->attstorage[j] != tbinfo->typstorage[j])
			{
				switch (tbinfo->attstorage[j])
				{
					case TYPSTORAGE_PLAIN:
						storage = "PLAIN";
						break;
					case TYPSTORAGE_EXTERNAL:
						storage = "EXTERNAL";
						break;
					case TYPSTORAGE_EXTENDED:
						storage = "EXTENDED";
						break;
					case TYPSTORAGE_MAIN:
						storage = "MAIN";
						break;
					default:
						storage = NULL;
				}

				/*
				 * Only dump the statement if it's a storage type we recognize
				 */
				if (storage != NULL)
					appendPQExpBuffer(q, "ALTER %sTABLE ONLY %s ALTER COLUMN %s SET STORAGE %s;\n",
									  foreign, qualrelname,
									  fmtId(tbinfo->attnames[j]),
									  storage);
			}

			/*
			 * Dump per-column attributes.
			 */
			if (tbinfo->attoptions[j][0] != '\0')
				appendPQExpBuffer(q, "ALTER %sTABLE ONLY %s ALTER COLUMN %s SET (%s);\n",
								  foreign, qualrelname,
								  fmtId(tbinfo->attnames[j]),
								  tbinfo->attoptions[j]);

			/*
			 * Dump per-column fdw options.
			 */
			if (tbinfo->relkind == RELKIND_FOREIGN_TABLE &&
				tbinfo->attfdwoptions[j][0] != '\0')
				appendPQExpBuffer(q,
								  "ALTER FOREIGN TABLE %s ALTER COLUMN %s OPTIONS (\n"
								  "    %s\n"
								  ");\n",
								  qualrelname,
								  fmtId(tbinfo->attnames[j]),
								  tbinfo->attfdwoptions[j]);
		}

		if (ftoptions)
			free(ftoptions);
		if (srvname)
			free(srvname);
	}

	/*
	 * dump properties we only have ALTER TABLE syntax for
	 */
	if ((tbinfo->relkind == RELKIND_RELATION ||
		 tbinfo->relkind == RELKIND_PARTITIONED_TABLE ||
		 tbinfo->relkind == RELKIND_MATVIEW) &&
		tbinfo->relreplident != REPLICA_IDENTITY_DEFAULT)
	{
		if (tbinfo->relreplident == REPLICA_IDENTITY_INDEX)
		{
			/* nothing to do, will be set when the index is dumped */
		}
		else if (tbinfo->relreplident == REPLICA_IDENTITY_NOTHING)
		{
			appendPQExpBuffer(q, "\nALTER TABLE ONLY %s REPLICA IDENTITY NOTHING;\n",
							  qualrelname);
		}
		else if (tbinfo->relreplident == REPLICA_IDENTITY_FULL)
		{
			appendPQExpBuffer(q, "\nALTER TABLE ONLY %s REPLICA IDENTITY FULL;\n",
							  qualrelname);
		}
	}

	if (tbinfo->forcerowsec)
		appendPQExpBuffer(q, "\nALTER TABLE ONLY %s FORCE ROW LEVEL SECURITY;\n",
						  qualrelname);

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &tbinfo->dobj,
										reltypename, qrelname,
										tbinfo->dobj.namespace->dobj.name);

	if (tbinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
	{
		char	   *tableam = NULL;

		if (tbinfo->relkind == RELKIND_RELATION ||
			tbinfo->relkind == RELKIND_MATVIEW)
			tableam = tbinfo->amname;

		ArchiveEntry(fout, tbinfo->dobj.catId, tbinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tbinfo->dobj.name,
								  .namespace = tbinfo->dobj.namespace->dobj.name,
								  .tablespace = (tbinfo->relkind == RELKIND_VIEW) ?
								  NULL : tbinfo->reltablespace,
								  .tableam = tableam,
								  .owner = tbinfo->rolname,
								  .description = reltypename,
								  .section = tbinfo->postponed_def ?
								  SECTION_POST_DATA : SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));
	}

	/* Dump Table Comments */
	if (tbinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpTableComment(fout, tbinfo, reltypename);

	/* Dump Table Security Labels */
	if (tbinfo->dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpTableSecLabel(fout, tbinfo, reltypename);

	/* Dump comments on inlined table constraints */
	for (j = 0; j < tbinfo->ncheck; j++)
	{
		ConstraintInfo *constr = &(tbinfo->checkexprs[j]);

		if (constr->separate || !constr->conislocal)
			continue;

		if (tbinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
			dumpTableConstraintComment(fout, constr);
	}

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	free(qrelname);
	free(qualrelname);
}

/*
 * dumpAttrDef --- dump an attribute's default-value declaration
 */
static void
dumpAttrDef(Archive *fout, AttrDefInfo *adinfo)
{
	DumpOptions *dopt = fout->dopt;
	TableInfo  *tbinfo = adinfo->adtable;
	int			adnum = adinfo->adnum;
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *qualrelname;
	char	   *tag;
	char	   *foreign;

	/* Skip if table definition not to be dumped */
	if (!tbinfo->dobj.dump || dopt->dataOnly)
		return;

	/* Skip if not "separate"; it was dumped in the table's definition */
	if (!adinfo->separate)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	qualrelname = pg_strdup(fmtQualifiedDumpable(tbinfo));

	foreign = tbinfo->relkind == RELKIND_FOREIGN_TABLE ? "FOREIGN " : "";

	appendPQExpBuffer(q,
					  "ALTER %sTABLE ONLY %s ALTER COLUMN %s SET DEFAULT %s;\n",
					  foreign, qualrelname, fmtId(tbinfo->attnames[adnum - 1]),
					  adinfo->adef_expr);

	appendPQExpBuffer(delq, "ALTER %sTABLE %s ALTER COLUMN %s DROP DEFAULT;\n",
					  foreign, qualrelname,
					  fmtId(tbinfo->attnames[adnum - 1]));

	tag = psprintf("%s %s", tbinfo->dobj.name, tbinfo->attnames[adnum - 1]);

	if (adinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, adinfo->dobj.catId, adinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tag,
								  .namespace = tbinfo->dobj.namespace->dobj.name,
								  .owner = tbinfo->rolname,
								  .description = "DEFAULT",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	free(tag);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	free(qualrelname);
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
	fatal("invalid column number %d for table \"%s\"",
		  attrnum, tblInfo->dobj.name);
	return NULL;				/* keep compiler quiet */
}

/*
 * dumpIndex
 *	  write out to fout a user-defined index
 */
static void
dumpIndex(Archive *fout, IndxInfo *indxinfo)
{
	DumpOptions *dopt = fout->dopt;
	TableInfo  *tbinfo = indxinfo->indextable;
	bool		is_constraint = (indxinfo->indexconstraint != 0);
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *qindxname;
	char	   *qqindxname;

	if (dopt->dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	qindxname = pg_strdup(fmtId(indxinfo->dobj.name));
	qqindxname = pg_strdup(fmtQualifiedDumpable(indxinfo));

	/*
	 * If there's an associated constraint, don't dump the index per se, but
	 * do dump any comment, or in binary upgrade mode dependency on a
	 * collation version for it.  (This is safe because dependency ordering
	 * will have ensured the constraint is emitted first.)	Note that the
	 * emitted comment has to be shown as depending on the constraint, not the
	 * index, in such cases.
	 */
	if (!is_constraint)
	{
		char	   *indstatcols = indxinfo->indstatcols;
		char	   *indstatvals = indxinfo->indstatvals;
		char	  **indstatcolsarray = NULL;
		char	  **indstatvalsarray = NULL;
		int			nstatcols = 0;
		int			nstatvals = 0;

		if (dopt->binary_upgrade)
			binary_upgrade_set_pg_class_oids(fout, q,
											 indxinfo->dobj.catId.oid, true);

		/* Plain secondary index */
		appendPQExpBuffer(q, "%s;\n", indxinfo->indexdef);

		/*
		 * Append ALTER TABLE commands as needed to set properties that we
		 * only have ALTER TABLE syntax for.  Keep this in sync with the
		 * similar code in dumpConstraint!
		 */

		/* If the index is clustered, we need to record that. */
		if (indxinfo->indisclustered)
		{
			appendPQExpBuffer(q, "\nALTER TABLE %s CLUSTER",
							  fmtQualifiedDumpable(tbinfo));
			/* index name is not qualified in this syntax */
			appendPQExpBuffer(q, " ON %s;\n",
							  qindxname);
		}

		/*
		 * If the index has any statistics on some of its columns, generate
		 * the associated ALTER INDEX queries.
		 */
		if (strlen(indstatcols) != 0 || strlen(indstatvals) != 0)
		{
			int			j;

			if (!parsePGArray(indstatcols, &indstatcolsarray, &nstatcols))
				fatal("could not parse index statistic columns");
			if (!parsePGArray(indstatvals, &indstatvalsarray, &nstatvals))
				fatal("could not parse index statistic values");
			if (nstatcols != nstatvals)
				fatal("mismatched number of columns and values for index stats");

			for (j = 0; j < nstatcols; j++)
			{
				appendPQExpBuffer(q, "ALTER INDEX %s ", qqindxname);

				/*
				 * Note that this is a column number, so no quotes should be
				 * used.
				 */
				appendPQExpBuffer(q, "ALTER COLUMN %s ",
								  indstatcolsarray[j]);
				appendPQExpBuffer(q, "SET STATISTICS %s;\n",
								  indstatvalsarray[j]);
			}
		}

		/* Indexes can depend on extensions */
		append_depends_on_extension(fout, q, &indxinfo->dobj,
									"pg_catalog.pg_class",
									"INDEX", qqindxname);

		if (dopt->binary_upgrade)
			appendIndexCollationVersion(q, indxinfo, fout->encoding,
										dopt->coll_unknown, fout);

		/* If the index defines identity, we need to record that. */
		if (indxinfo->indisreplident)
		{
			appendPQExpBuffer(q, "\nALTER TABLE ONLY %s REPLICA IDENTITY USING",
							  fmtQualifiedDumpable(tbinfo));
			/* index name is not qualified in this syntax */
			appendPQExpBuffer(q, " INDEX %s;\n",
							  qindxname);
		}

		appendPQExpBuffer(delq, "DROP INDEX %s;\n", qqindxname);

		if (indxinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
			ArchiveEntry(fout, indxinfo->dobj.catId, indxinfo->dobj.dumpId,
						 ARCHIVE_OPTS(.tag = indxinfo->dobj.name,
									  .namespace = tbinfo->dobj.namespace->dobj.name,
									  .tablespace = indxinfo->tablespace,
									  .owner = tbinfo->rolname,
									  .description = "INDEX",
									  .section = SECTION_POST_DATA,
									  .createStmt = q->data,
									  .dropStmt = delq->data));

		if (indstatcolsarray)
			free(indstatcolsarray);
		if (indstatvalsarray)
			free(indstatvalsarray);
	}
	else if (dopt->binary_upgrade)
	{
		appendIndexCollationVersion(q, indxinfo, fout->encoding,
									dopt->coll_unknown, fout);

		if (indxinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
			ArchiveEntry(fout, indxinfo->dobj.catId, indxinfo->dobj.dumpId,
						 ARCHIVE_OPTS(.tag = indxinfo->dobj.name,
									  .namespace = tbinfo->dobj.namespace->dobj.name,
									  .tablespace = indxinfo->tablespace,
									  .owner = tbinfo->rolname,
									  .description = "INDEX",
									  .section = SECTION_POST_DATA,
									  .createStmt = q->data));
	}

	/* Dump Index Comments */
	if (indxinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "INDEX", qindxname,
					tbinfo->dobj.namespace->dobj.name,
					tbinfo->rolname,
					indxinfo->dobj.catId, 0,
					is_constraint ? indxinfo->indexconstraint :
					indxinfo->dobj.dumpId);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	free(qindxname);
	free(qqindxname);
}

/*
 * dumpIndexAttach
 *	  write out to fout a partitioned-index attachment clause
 */
static void
dumpIndexAttach(Archive *fout, IndexAttachInfo *attachinfo)
{
	if (fout->dopt->dataOnly)
		return;

	if (attachinfo->partitionIdx->dobj.dump & DUMP_COMPONENT_DEFINITION)
	{
		PQExpBuffer q = createPQExpBuffer();

		appendPQExpBuffer(q, "ALTER INDEX %s ",
						  fmtQualifiedDumpable(attachinfo->parentIdx));
		appendPQExpBuffer(q, "ATTACH PARTITION %s;\n",
						  fmtQualifiedDumpable(attachinfo->partitionIdx));

		ArchiveEntry(fout, attachinfo->dobj.catId, attachinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = attachinfo->dobj.name,
								  .namespace = attachinfo->dobj.namespace->dobj.name,
								  .description = "INDEX ATTACH",
								  .section = SECTION_POST_DATA,
								  .createStmt = q->data));

		destroyPQExpBuffer(q);
	}
}

/*
 * dumpStatisticsExt
 *	  write out to fout an extended statistics object
 */
static void
dumpStatisticsExt(Archive *fout, StatsExtInfo *statsextinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer query;
	char	   *qstatsextname;
	PGresult   *res;
	char	   *stxdef;

	/* Skip if not to be dumped */
	if (!statsextinfo->dobj.dump || dopt->dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();
	query = createPQExpBuffer();

	qstatsextname = pg_strdup(fmtId(statsextinfo->dobj.name));

	appendPQExpBuffer(query, "SELECT "
					  "pg_catalog.pg_get_statisticsobjdef('%u'::pg_catalog.oid)",
					  statsextinfo->dobj.catId.oid);

	res = ExecuteSqlQueryForSingleRow(fout, query->data);

	stxdef = PQgetvalue(res, 0, 0);

	/* Result of pg_get_statisticsobjdef is complete except for semicolon */
	appendPQExpBuffer(q, "%s;\n", stxdef);

	/*
	 * We only issue an ALTER STATISTICS statement if the stxstattarget entry
	 * for this statistics object is non-negative (i.e. it's not the default
	 * value).
	 */
	if (statsextinfo->stattarget >= 0)
	{
		appendPQExpBuffer(q, "ALTER STATISTICS %s ",
						  fmtQualifiedDumpable(statsextinfo));
		appendPQExpBuffer(q, "SET STATISTICS %d;\n",
						  statsextinfo->stattarget);
	}

	appendPQExpBuffer(delq, "DROP STATISTICS %s;\n",
					  fmtQualifiedDumpable(statsextinfo));

	if (statsextinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, statsextinfo->dobj.catId,
					 statsextinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = statsextinfo->dobj.name,
								  .namespace = statsextinfo->dobj.namespace->dobj.name,
								  .owner = statsextinfo->rolname,
								  .description = "STATISTICS",
								  .section = SECTION_POST_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* Dump Statistics Comments */
	if (statsextinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "STATISTICS", qstatsextname,
					statsextinfo->dobj.namespace->dobj.name,
					statsextinfo->rolname,
					statsextinfo->dobj.catId, 0,
					statsextinfo->dobj.dumpId);

	PQclear(res);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(query);
	free(qstatsextname);
}

/*
 * dumpConstraint
 *	  write out to fout a user-defined constraint
 */
static void
dumpConstraint(Archive *fout, ConstraintInfo *coninfo)
{
	DumpOptions *dopt = fout->dopt;
	TableInfo  *tbinfo = coninfo->contable;
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *tag = NULL;
	char	   *foreign;

	/* Skip if not to be dumped */
	if (!coninfo->dobj.dump || dopt->dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	foreign = tbinfo &&
		tbinfo->relkind == RELKIND_FOREIGN_TABLE ? "FOREIGN " : "";

	if (coninfo->contype == 'p' ||
		coninfo->contype == 'u' ||
		coninfo->contype == 'x')
	{
		/* Index-related constraint */
		IndxInfo   *indxinfo;
		int			k;

		indxinfo = (IndxInfo *) findObjectByDumpId(coninfo->conindex);

		if (indxinfo == NULL)
			fatal("missing index for constraint \"%s\"",
				  coninfo->dobj.name);

		if (dopt->binary_upgrade)
			binary_upgrade_set_pg_class_oids(fout, q,
											 indxinfo->dobj.catId.oid, true);

		appendPQExpBuffer(q, "ALTER %sTABLE ONLY %s\n", foreign,
						  fmtQualifiedDumpable(tbinfo));
		appendPQExpBuffer(q, "    ADD CONSTRAINT %s ",
						  fmtId(coninfo->dobj.name));

		if (coninfo->condef)
		{
			/* pg_get_constraintdef should have provided everything */
			appendPQExpBuffer(q, "%s;\n", coninfo->condef);
		}
		else
		{
			appendPQExpBuffer(q, "%s (",
							  coninfo->contype == 'p' ? "PRIMARY KEY" : "UNIQUE");
			for (k = 0; k < indxinfo->indnkeyattrs; k++)
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

			if (indxinfo->indnkeyattrs < indxinfo->indnattrs)
				appendPQExpBufferStr(q, ") INCLUDE (");

			for (k = indxinfo->indnkeyattrs; k < indxinfo->indnattrs; k++)
			{
				int			indkey = (int) indxinfo->indkeys[k];
				const char *attname;

				if (indkey == InvalidAttrNumber)
					break;
				attname = getAttrName(indkey, tbinfo);

				appendPQExpBuffer(q, "%s%s",
								  (k == indxinfo->indnkeyattrs) ? "" : ", ",
								  fmtId(attname));
			}

			appendPQExpBufferChar(q, ')');

			if (nonemptyReloptions(indxinfo->indreloptions))
			{
				appendPQExpBufferStr(q, " WITH (");
				appendReloptionsArrayAH(q, indxinfo->indreloptions, "", fout);
				appendPQExpBufferChar(q, ')');
			}

			if (coninfo->condeferrable)
			{
				appendPQExpBufferStr(q, " DEFERRABLE");
				if (coninfo->condeferred)
					appendPQExpBufferStr(q, " INITIALLY DEFERRED");
			}

			appendPQExpBufferStr(q, ";\n");
		}

		/*
		 * Append ALTER TABLE commands as needed to set properties that we
		 * only have ALTER TABLE syntax for.  Keep this in sync with the
		 * similar code in dumpIndex!
		 */

		/* If the index is clustered, we need to record that. */
		if (indxinfo->indisclustered)
		{
			appendPQExpBuffer(q, "\nALTER TABLE %s CLUSTER",
							  fmtQualifiedDumpable(tbinfo));
			/* index name is not qualified in this syntax */
			appendPQExpBuffer(q, " ON %s;\n",
							  fmtId(indxinfo->dobj.name));
		}

		/* If the index defines identity, we need to record that. */
		if (indxinfo->indisreplident)
		{
			appendPQExpBuffer(q, "\nALTER TABLE ONLY %s REPLICA IDENTITY USING",
							  fmtQualifiedDumpable(tbinfo));
			/* index name is not qualified in this syntax */
			appendPQExpBuffer(q, " INDEX %s;\n",
							  fmtId(indxinfo->dobj.name));
		}

		/* Indexes can depend on extensions */
		append_depends_on_extension(fout, q, &indxinfo->dobj,
									"pg_catalog.pg_class", "INDEX",
									fmtQualifiedDumpable(indxinfo));

		appendPQExpBuffer(delq, "ALTER %sTABLE ONLY %s ", foreign,
						  fmtQualifiedDumpable(tbinfo));
		appendPQExpBuffer(delq, "DROP CONSTRAINT %s;\n",
						  fmtId(coninfo->dobj.name));

		tag = psprintf("%s %s", tbinfo->dobj.name, coninfo->dobj.name);

		if (coninfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
			ArchiveEntry(fout, coninfo->dobj.catId, coninfo->dobj.dumpId,
						 ARCHIVE_OPTS(.tag = tag,
									  .namespace = tbinfo->dobj.namespace->dobj.name,
									  .tablespace = indxinfo->tablespace,
									  .owner = tbinfo->rolname,
									  .description = "CONSTRAINT",
									  .section = SECTION_POST_DATA,
									  .createStmt = q->data,
									  .dropStmt = delq->data));
	}
	else if (coninfo->contype == 'f')
	{
		char	   *only;

		/*
		 * Foreign keys on partitioned tables are always declared as
		 * inheriting to partitions; for all other cases, emit them as
		 * applying ONLY directly to the named table, because that's how they
		 * work for regular inherited tables.
		 */
		only = tbinfo->relkind == RELKIND_PARTITIONED_TABLE ? "" : "ONLY ";

		/*
		 * XXX Potentially wrap in a 'SET CONSTRAINTS OFF' block so that the
		 * current table data is not processed
		 */
		appendPQExpBuffer(q, "ALTER %sTABLE %s%s\n", foreign,
						  only, fmtQualifiedDumpable(tbinfo));
		appendPQExpBuffer(q, "    ADD CONSTRAINT %s %s;\n",
						  fmtId(coninfo->dobj.name),
						  coninfo->condef);

		appendPQExpBuffer(delq, "ALTER %sTABLE %s%s ", foreign,
						  only, fmtQualifiedDumpable(tbinfo));
		appendPQExpBuffer(delq, "DROP CONSTRAINT %s;\n",
						  fmtId(coninfo->dobj.name));

		tag = psprintf("%s %s", tbinfo->dobj.name, coninfo->dobj.name);

		if (coninfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
			ArchiveEntry(fout, coninfo->dobj.catId, coninfo->dobj.dumpId,
						 ARCHIVE_OPTS(.tag = tag,
									  .namespace = tbinfo->dobj.namespace->dobj.name,
									  .owner = tbinfo->rolname,
									  .description = "FK CONSTRAINT",
									  .section = SECTION_POST_DATA,
									  .createStmt = q->data,
									  .dropStmt = delq->data));
	}
	else if (coninfo->contype == 'c' && tbinfo)
	{
		/* CHECK constraint on a table */

		/* Ignore if not to be dumped separately, or if it was inherited */
		if (coninfo->separate && coninfo->conislocal)
		{
			/* not ONLY since we want it to propagate to children */
			appendPQExpBuffer(q, "ALTER %sTABLE %s\n", foreign,
							  fmtQualifiedDumpable(tbinfo));
			appendPQExpBuffer(q, "    ADD CONSTRAINT %s %s;\n",
							  fmtId(coninfo->dobj.name),
							  coninfo->condef);

			appendPQExpBuffer(delq, "ALTER %sTABLE %s ", foreign,
							  fmtQualifiedDumpable(tbinfo));
			appendPQExpBuffer(delq, "DROP CONSTRAINT %s;\n",
							  fmtId(coninfo->dobj.name));

			tag = psprintf("%s %s", tbinfo->dobj.name, coninfo->dobj.name);

			if (coninfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
				ArchiveEntry(fout, coninfo->dobj.catId, coninfo->dobj.dumpId,
							 ARCHIVE_OPTS(.tag = tag,
										  .namespace = tbinfo->dobj.namespace->dobj.name,
										  .owner = tbinfo->rolname,
										  .description = "CHECK CONSTRAINT",
										  .section = SECTION_POST_DATA,
										  .createStmt = q->data,
										  .dropStmt = delq->data));
		}
	}
	else if (coninfo->contype == 'c' && tbinfo == NULL)
	{
		/* CHECK constraint on a domain */
		TypeInfo   *tyinfo = coninfo->condomain;

		/* Ignore if not to be dumped separately */
		if (coninfo->separate)
		{
			appendPQExpBuffer(q, "ALTER DOMAIN %s\n",
							  fmtQualifiedDumpable(tyinfo));
			appendPQExpBuffer(q, "    ADD CONSTRAINT %s %s;\n",
							  fmtId(coninfo->dobj.name),
							  coninfo->condef);

			appendPQExpBuffer(delq, "ALTER DOMAIN %s ",
							  fmtQualifiedDumpable(tyinfo));
			appendPQExpBuffer(delq, "DROP CONSTRAINT %s;\n",
							  fmtId(coninfo->dobj.name));

			tag = psprintf("%s %s", tyinfo->dobj.name, coninfo->dobj.name);

			if (coninfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
				ArchiveEntry(fout, coninfo->dobj.catId, coninfo->dobj.dumpId,
							 ARCHIVE_OPTS(.tag = tag,
										  .namespace = tyinfo->dobj.namespace->dobj.name,
										  .owner = tyinfo->rolname,
										  .description = "CHECK CONSTRAINT",
										  .section = SECTION_POST_DATA,
										  .createStmt = q->data,
										  .dropStmt = delq->data));
		}
	}
	else
	{
		fatal("unrecognized constraint type: %c",
			  coninfo->contype);
	}

	/* Dump Constraint Comments --- only works for table constraints */
	if (tbinfo && coninfo->separate &&
		coninfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpTableConstraintComment(fout, coninfo);

	free(tag);
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
	PQExpBuffer conprefix = createPQExpBuffer();
	char	   *qtabname;

	qtabname = pg_strdup(fmtId(tbinfo->dobj.name));

	appendPQExpBuffer(conprefix, "CONSTRAINT %s ON",
					  fmtId(coninfo->dobj.name));

	if (coninfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, conprefix->data, qtabname,
					tbinfo->dobj.namespace->dobj.name,
					tbinfo->rolname,
					coninfo->dobj.catId, 0,
					coninfo->separate ? coninfo->dobj.dumpId : tbinfo->dobj.dumpId);

	destroyPQExpBuffer(conprefix);
	free(qtabname);
}

/*
 * findLastBuiltinOid_V71 -
 *
 * find the last built in oid
 *
 * For 7.1 through 8.0, we do this by retrieving datlastsysoid from the
 * pg_database entry for the current database.  (Note: current_database()
 * requires 7.3; pg_dump requires 8.0 now.)
 */
static Oid
findLastBuiltinOid_V71(Archive *fout)
{
	PGresult   *res;
	Oid			last_oid;

	res = ExecuteSqlQueryForSingleRow(fout,
									  "SELECT datlastsysoid FROM pg_database WHERE datname = current_database()");
	last_oid = atooid(PQgetvalue(res, 0, PQfnumber(res, "datlastsysoid")));
	PQclear(res);

	return last_oid;
}

/*
 * dumpSequence
 *	  write the declaration (not data) of one user-defined sequence
 */
static void
dumpSequence(Archive *fout, TableInfo *tbinfo)
{
	DumpOptions *dopt = fout->dopt;
	PGresult   *res;
	char	   *startv,
			   *incby,
			   *maxv,
			   *minv,
			   *cache,
			   *seqtype;
	bool		cycled;
	bool		is_ascending;
	int64		default_minv,
				default_maxv;
	char		bufm[32],
				bufx[32];
	PQExpBuffer query = createPQExpBuffer();
	PQExpBuffer delqry = createPQExpBuffer();
	char	   *qseqname;

	qseqname = pg_strdup(fmtId(tbinfo->dobj.name));

	if (fout->remoteVersion >= 100000)
	{
		appendPQExpBuffer(query,
						  "SELECT format_type(seqtypid, NULL), "
						  "seqstart, seqincrement, "
						  "seqmax, seqmin, "
						  "seqcache, seqcycle "
						  "FROM pg_catalog.pg_sequence "
						  "WHERE seqrelid = '%u'::oid",
						  tbinfo->dobj.catId.oid);
	}
	else if (fout->remoteVersion >= 80400)
	{
		/*
		 * Before PostgreSQL 10, sequence metadata is in the sequence itself.
		 *
		 * Note: it might seem that 'bigint' potentially needs to be
		 * schema-qualified, but actually that's a keyword.
		 */
		appendPQExpBuffer(query,
						  "SELECT 'bigint' AS sequence_type, "
						  "start_value, increment_by, max_value, min_value, "
						  "cache_value, is_cycled FROM %s",
						  fmtQualifiedDumpable(tbinfo));
	}
	else
	{
		appendPQExpBuffer(query,
						  "SELECT 'bigint' AS sequence_type, "
						  "0 AS start_value, increment_by, max_value, min_value, "
						  "cache_value, is_cycled FROM %s",
						  fmtQualifiedDumpable(tbinfo));
	}

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	if (PQntuples(res) != 1)
	{
		pg_log_error(ngettext("query to get data of sequence \"%s\" returned %d row (expected 1)",
							  "query to get data of sequence \"%s\" returned %d rows (expected 1)",
							  PQntuples(res)),
					 tbinfo->dobj.name, PQntuples(res));
		exit_nicely(1);
	}

	seqtype = PQgetvalue(res, 0, 0);
	startv = PQgetvalue(res, 0, 1);
	incby = PQgetvalue(res, 0, 2);
	maxv = PQgetvalue(res, 0, 3);
	minv = PQgetvalue(res, 0, 4);
	cache = PQgetvalue(res, 0, 5);
	cycled = (strcmp(PQgetvalue(res, 0, 6), "t") == 0);

	/* Calculate default limits for a sequence of this type */
	is_ascending = (incby[0] != '-');
	if (strcmp(seqtype, "smallint") == 0)
	{
		default_minv = is_ascending ? 1 : PG_INT16_MIN;
		default_maxv = is_ascending ? PG_INT16_MAX : -1;
	}
	else if (strcmp(seqtype, "integer") == 0)
	{
		default_minv = is_ascending ? 1 : PG_INT32_MIN;
		default_maxv = is_ascending ? PG_INT32_MAX : -1;
	}
	else if (strcmp(seqtype, "bigint") == 0)
	{
		default_minv = is_ascending ? 1 : PG_INT64_MIN;
		default_maxv = is_ascending ? PG_INT64_MAX : -1;
	}
	else
	{
		fatal("unrecognized sequence type: %s", seqtype);
		default_minv = default_maxv = 0;	/* keep compiler quiet */
	}

	/*
	 * 64-bit strtol() isn't very portable, so convert the limits to strings
	 * and compare that way.
	 */
	snprintf(bufm, sizeof(bufm), INT64_FORMAT, default_minv);
	snprintf(bufx, sizeof(bufx), INT64_FORMAT, default_maxv);

	/* Don't print minv/maxv if they match the respective default limit */
	if (strcmp(minv, bufm) == 0)
		minv = NULL;
	if (strcmp(maxv, bufx) == 0)
		maxv = NULL;

	/*
	 * Identity sequences are not to be dropped separately.
	 */
	if (!tbinfo->is_identity_sequence)
	{
		appendPQExpBuffer(delqry, "DROP SEQUENCE %s;\n",
						  fmtQualifiedDumpable(tbinfo));
	}

	resetPQExpBuffer(query);

	if (dopt->binary_upgrade)
	{
		binary_upgrade_set_pg_class_oids(fout, query,
										 tbinfo->dobj.catId.oid, false);

		/*
		 * In older PG versions a sequence will have a pg_type entry, but v14
		 * and up don't use that, so don't attempt to preserve the type OID.
		 */
	}

	if (tbinfo->is_identity_sequence)
	{
		TableInfo  *owning_tab = findTableByOid(tbinfo->owning_tab);

		appendPQExpBuffer(query,
						  "ALTER TABLE %s ",
						  fmtQualifiedDumpable(owning_tab));
		appendPQExpBuffer(query,
						  "ALTER COLUMN %s ADD GENERATED ",
						  fmtId(owning_tab->attnames[tbinfo->owning_col - 1]));
		if (owning_tab->attidentity[tbinfo->owning_col - 1] == ATTRIBUTE_IDENTITY_ALWAYS)
			appendPQExpBufferStr(query, "ALWAYS");
		else if (owning_tab->attidentity[tbinfo->owning_col - 1] == ATTRIBUTE_IDENTITY_BY_DEFAULT)
			appendPQExpBufferStr(query, "BY DEFAULT");
		appendPQExpBuffer(query, " AS IDENTITY (\n    SEQUENCE NAME %s\n",
						  fmtQualifiedDumpable(tbinfo));
	}
	else
	{
		appendPQExpBuffer(query,
						  "CREATE SEQUENCE %s\n",
						  fmtQualifiedDumpable(tbinfo));

		if (strcmp(seqtype, "bigint") != 0)
			appendPQExpBuffer(query, "    AS %s\n", seqtype);
	}

	if (fout->remoteVersion >= 80400)
		appendPQExpBuffer(query, "    START WITH %s\n", startv);

	appendPQExpBuffer(query, "    INCREMENT BY %s\n", incby);

	if (minv)
		appendPQExpBuffer(query, "    MINVALUE %s\n", minv);
	else
		appendPQExpBufferStr(query, "    NO MINVALUE\n");

	if (maxv)
		appendPQExpBuffer(query, "    MAXVALUE %s\n", maxv);
	else
		appendPQExpBufferStr(query, "    NO MAXVALUE\n");

	appendPQExpBuffer(query,
					  "    CACHE %s%s",
					  cache, (cycled ? "\n    CYCLE" : ""));

	if (tbinfo->is_identity_sequence)
		appendPQExpBufferStr(query, "\n);\n");
	else
		appendPQExpBufferStr(query, ";\n");

	/* binary_upgrade:	no need to clear TOAST table oid */

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(query, &tbinfo->dobj,
										"SEQUENCE", qseqname,
										tbinfo->dobj.namespace->dobj.name);

	if (tbinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, tbinfo->dobj.catId, tbinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tbinfo->dobj.name,
								  .namespace = tbinfo->dobj.namespace->dobj.name,
								  .owner = tbinfo->rolname,
								  .description = "SEQUENCE",
								  .section = SECTION_PRE_DATA,
								  .createStmt = query->data,
								  .dropStmt = delqry->data));

	/*
	 * If the sequence is owned by a table column, emit the ALTER for it as a
	 * separate TOC entry immediately following the sequence's own entry. It's
	 * OK to do this rather than using full sorting logic, because the
	 * dependency that tells us it's owned will have forced the table to be
	 * created first.  We can't just include the ALTER in the TOC entry
	 * because it will fail if we haven't reassigned the sequence owner to
	 * match the table's owner.
	 *
	 * We need not schema-qualify the table reference because both sequence
	 * and table must be in the same schema.
	 */
	if (OidIsValid(tbinfo->owning_tab) && !tbinfo->is_identity_sequence)
	{
		TableInfo  *owning_tab = findTableByOid(tbinfo->owning_tab);

		if (owning_tab == NULL)
			fatal("failed sanity check, parent table with OID %u of sequence with OID %u not found",
				  tbinfo->owning_tab, tbinfo->dobj.catId.oid);

		if (owning_tab->dobj.dump & DUMP_COMPONENT_DEFINITION)
		{
			resetPQExpBuffer(query);
			appendPQExpBuffer(query, "ALTER SEQUENCE %s",
							  fmtQualifiedDumpable(tbinfo));
			appendPQExpBuffer(query, " OWNED BY %s",
							  fmtQualifiedDumpable(owning_tab));
			appendPQExpBuffer(query, ".%s;\n",
							  fmtId(owning_tab->attnames[tbinfo->owning_col - 1]));

			if (tbinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
				ArchiveEntry(fout, nilCatalogId, createDumpId(),
							 ARCHIVE_OPTS(.tag = tbinfo->dobj.name,
										  .namespace = tbinfo->dobj.namespace->dobj.name,
										  .owner = tbinfo->rolname,
										  .description = "SEQUENCE OWNED BY",
										  .section = SECTION_PRE_DATA,
										  .createStmt = query->data,
										  .deps = &(tbinfo->dobj.dumpId),
										  .nDeps = 1));
		}
	}

	/* Dump Sequence Comments and Security Labels */
	if (tbinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "SEQUENCE", qseqname,
					tbinfo->dobj.namespace->dobj.name, tbinfo->rolname,
					tbinfo->dobj.catId, 0, tbinfo->dobj.dumpId);

	if (tbinfo->dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpSecLabel(fout, "SEQUENCE", qseqname,
					 tbinfo->dobj.namespace->dobj.name, tbinfo->rolname,
					 tbinfo->dobj.catId, 0, tbinfo->dobj.dumpId);

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(delqry);
	free(qseqname);
}

/*
 * dumpSequenceData
 *	  write the data of one user-defined sequence
 */
static void
dumpSequenceData(Archive *fout, TableDataInfo *tdinfo)
{
	TableInfo  *tbinfo = tdinfo->tdtable;
	PGresult   *res;
	char	   *last;
	bool		called;
	PQExpBuffer query = createPQExpBuffer();

	appendPQExpBuffer(query,
					  "SELECT last_value, is_called FROM %s",
					  fmtQualifiedDumpable(tbinfo));

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	if (PQntuples(res) != 1)
	{
		pg_log_error(ngettext("query to get data of sequence \"%s\" returned %d row (expected 1)",
							  "query to get data of sequence \"%s\" returned %d rows (expected 1)",
							  PQntuples(res)),
					 tbinfo->dobj.name, PQntuples(res));
		exit_nicely(1);
	}

	last = PQgetvalue(res, 0, 0);
	called = (strcmp(PQgetvalue(res, 0, 1), "t") == 0);

	resetPQExpBuffer(query);
	appendPQExpBufferStr(query, "SELECT pg_catalog.setval(");
	appendStringLiteralAH(query, fmtQualifiedDumpable(tbinfo), fout);
	appendPQExpBuffer(query, ", %s, %s);\n",
					  last, (called ? "true" : "false"));

	if (tdinfo->dobj.dump & DUMP_COMPONENT_DATA)
		ArchiveEntry(fout, nilCatalogId, createDumpId(),
					 ARCHIVE_OPTS(.tag = tbinfo->dobj.name,
								  .namespace = tbinfo->dobj.namespace->dobj.name,
								  .owner = tbinfo->rolname,
								  .description = "SEQUENCE SET",
								  .section = SECTION_DATA,
								  .createStmt = query->data,
								  .deps = &(tbinfo->dobj.dumpId),
								  .nDeps = 1));

	PQclear(res);

	destroyPQExpBuffer(query);
}

/*
 * dumpTrigger
 *	  write the declaration of one user-defined table trigger
 */
static void
dumpTrigger(Archive *fout, TriggerInfo *tginfo)
{
	DumpOptions *dopt = fout->dopt;
	TableInfo  *tbinfo = tginfo->tgtable;
	PQExpBuffer query;
	PQExpBuffer delqry;
	PQExpBuffer trigprefix;
	PQExpBuffer trigidentity;
	char	   *qtabname;
	char	   *tgargs;
	size_t		lentgargs;
	const char *p;
	int			findx;
	char	   *tag;

	/*
	 * we needn't check dobj.dump because TriggerInfo wouldn't have been
	 * created in the first place for non-dumpable triggers
	 */
	if (dopt->dataOnly)
		return;

	query = createPQExpBuffer();
	delqry = createPQExpBuffer();
	trigprefix = createPQExpBuffer();
	trigidentity = createPQExpBuffer();

	qtabname = pg_strdup(fmtId(tbinfo->dobj.name));

	appendPQExpBuffer(trigidentity, "%s ", fmtId(tginfo->dobj.name));
	appendPQExpBuffer(trigidentity, "ON %s", fmtQualifiedDumpable(tbinfo));

	appendPQExpBuffer(delqry, "DROP TRIGGER %s;\n", trigidentity->data);

	if (tginfo->tgdef)
	{
		appendPQExpBuffer(query, "%s;\n", tginfo->tgdef);
	}
	else
	{
		if (tginfo->tgisconstraint)
		{
			appendPQExpBufferStr(query, "CREATE CONSTRAINT TRIGGER ");
			appendPQExpBufferStr(query, fmtId(tginfo->tgconstrname));
		}
		else
		{
			appendPQExpBufferStr(query, "CREATE TRIGGER ");
			appendPQExpBufferStr(query, fmtId(tginfo->dobj.name));
		}
		appendPQExpBufferStr(query, "\n    ");

		/* Trigger type */
		if (TRIGGER_FOR_BEFORE(tginfo->tgtype))
			appendPQExpBufferStr(query, "BEFORE");
		else if (TRIGGER_FOR_AFTER(tginfo->tgtype))
			appendPQExpBufferStr(query, "AFTER");
		else if (TRIGGER_FOR_INSTEAD(tginfo->tgtype))
			appendPQExpBufferStr(query, "INSTEAD OF");
		else
		{
			pg_log_error("unexpected tgtype value: %d", tginfo->tgtype);
			exit_nicely(1);
		}

		findx = 0;
		if (TRIGGER_FOR_INSERT(tginfo->tgtype))
		{
			appendPQExpBufferStr(query, " INSERT");
			findx++;
		}
		if (TRIGGER_FOR_DELETE(tginfo->tgtype))
		{
			if (findx > 0)
				appendPQExpBufferStr(query, " OR DELETE");
			else
				appendPQExpBufferStr(query, " DELETE");
			findx++;
		}
		if (TRIGGER_FOR_UPDATE(tginfo->tgtype))
		{
			if (findx > 0)
				appendPQExpBufferStr(query, " OR UPDATE");
			else
				appendPQExpBufferStr(query, " UPDATE");
			findx++;
		}
		if (TRIGGER_FOR_TRUNCATE(tginfo->tgtype))
		{
			if (findx > 0)
				appendPQExpBufferStr(query, " OR TRUNCATE");
			else
				appendPQExpBufferStr(query, " TRUNCATE");
			findx++;
		}
		appendPQExpBuffer(query, " ON %s\n",
						  fmtQualifiedDumpable(tbinfo));

		if (tginfo->tgisconstraint)
		{
			if (OidIsValid(tginfo->tgconstrrelid))
			{
				/* regclass output is already quoted */
				appendPQExpBuffer(query, "    FROM %s\n    ",
								  tginfo->tgconstrrelname);
			}
			if (!tginfo->tgdeferrable)
				appendPQExpBufferStr(query, "NOT ");
			appendPQExpBufferStr(query, "DEFERRABLE INITIALLY ");
			if (tginfo->tginitdeferred)
				appendPQExpBufferStr(query, "DEFERRED\n");
			else
				appendPQExpBufferStr(query, "IMMEDIATE\n");
		}

		if (TRIGGER_FOR_ROW(tginfo->tgtype))
			appendPQExpBufferStr(query, "    FOR EACH ROW\n    ");
		else
			appendPQExpBufferStr(query, "    FOR EACH STATEMENT\n    ");

		/* regproc output is already sufficiently quoted */
		appendPQExpBuffer(query, "EXECUTE FUNCTION %s(",
						  tginfo->tgfname);

		tgargs = (char *) PQunescapeBytea((unsigned char *) tginfo->tgargs,
										  &lentgargs);
		p = tgargs;
		for (findx = 0; findx < tginfo->tgnargs; findx++)
		{
			/* find the embedded null that terminates this trigger argument */
			size_t		tlen = strlen(p);

			if (p + tlen >= tgargs + lentgargs)
			{
				/* hm, not found before end of bytea value... */
				pg_log_error("invalid argument string (%s) for trigger \"%s\" on table \"%s\"",
							 tginfo->tgargs,
							 tginfo->dobj.name,
							 tbinfo->dobj.name);
				exit_nicely(1);
			}

			if (findx > 0)
				appendPQExpBufferStr(query, ", ");
			appendStringLiteralAH(query, p, fout);
			p += tlen + 1;
		}
		free(tgargs);
		appendPQExpBufferStr(query, ");\n");
	}

	/* Triggers can depend on extensions */
	append_depends_on_extension(fout, query, &tginfo->dobj,
								"pg_catalog.pg_trigger", "TRIGGER",
								trigidentity->data);

	if (tginfo->tgenabled != 't' && tginfo->tgenabled != 'O')
	{
		appendPQExpBuffer(query, "\nALTER %sTABLE %s ",
						  tbinfo->relkind == RELKIND_FOREIGN_TABLE ? "FOREIGN " : "",
						  fmtQualifiedDumpable(tbinfo));
		switch (tginfo->tgenabled)
		{
			case 'D':
			case 'f':
				appendPQExpBufferStr(query, "DISABLE");
				break;
			case 'A':
				appendPQExpBufferStr(query, "ENABLE ALWAYS");
				break;
			case 'R':
				appendPQExpBufferStr(query, "ENABLE REPLICA");
				break;
			default:
				appendPQExpBufferStr(query, "ENABLE");
				break;
		}
		appendPQExpBuffer(query, " TRIGGER %s;\n",
						  fmtId(tginfo->dobj.name));
	}

	appendPQExpBuffer(trigprefix, "TRIGGER %s ON",
					  fmtId(tginfo->dobj.name));

	tag = psprintf("%s %s", tbinfo->dobj.name, tginfo->dobj.name);

	if (tginfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, tginfo->dobj.catId, tginfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tag,
								  .namespace = tbinfo->dobj.namespace->dobj.name,
								  .owner = tbinfo->rolname,
								  .description = "TRIGGER",
								  .section = SECTION_POST_DATA,
								  .createStmt = query->data,
								  .dropStmt = delqry->data));

	if (tginfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, trigprefix->data, qtabname,
					tbinfo->dobj.namespace->dobj.name, tbinfo->rolname,
					tginfo->dobj.catId, 0, tginfo->dobj.dumpId);

	free(tag);
	destroyPQExpBuffer(query);
	destroyPQExpBuffer(delqry);
	destroyPQExpBuffer(trigprefix);
	destroyPQExpBuffer(trigidentity);
	free(qtabname);
}

/*
 * dumpEventTrigger
 *	  write the declaration of one user-defined event trigger
 */
static void
dumpEventTrigger(Archive *fout, EventTriggerInfo *evtinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer query;
	PQExpBuffer delqry;
	char	   *qevtname;

	/* Skip if not to be dumped */
	if (!evtinfo->dobj.dump || dopt->dataOnly)
		return;

	query = createPQExpBuffer();
	delqry = createPQExpBuffer();

	qevtname = pg_strdup(fmtId(evtinfo->dobj.name));

	appendPQExpBufferStr(query, "CREATE EVENT TRIGGER ");
	appendPQExpBufferStr(query, qevtname);
	appendPQExpBufferStr(query, " ON ");
	appendPQExpBufferStr(query, fmtId(evtinfo->evtevent));

	if (strcmp("", evtinfo->evttags) != 0)
	{
		appendPQExpBufferStr(query, "\n         WHEN TAG IN (");
		appendPQExpBufferStr(query, evtinfo->evttags);
		appendPQExpBufferChar(query, ')');
	}

	appendPQExpBufferStr(query, "\n   EXECUTE FUNCTION ");
	appendPQExpBufferStr(query, evtinfo->evtfname);
	appendPQExpBufferStr(query, "();\n");

	if (evtinfo->evtenabled != 'O')
	{
		appendPQExpBuffer(query, "\nALTER EVENT TRIGGER %s ",
						  qevtname);
		switch (evtinfo->evtenabled)
		{
			case 'D':
				appendPQExpBufferStr(query, "DISABLE");
				break;
			case 'A':
				appendPQExpBufferStr(query, "ENABLE ALWAYS");
				break;
			case 'R':
				appendPQExpBufferStr(query, "ENABLE REPLICA");
				break;
			default:
				appendPQExpBufferStr(query, "ENABLE");
				break;
		}
		appendPQExpBufferStr(query, ";\n");
	}

	appendPQExpBuffer(delqry, "DROP EVENT TRIGGER %s;\n",
					  qevtname);

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(query, &evtinfo->dobj,
										"EVENT TRIGGER", qevtname, NULL);

	if (evtinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, evtinfo->dobj.catId, evtinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = evtinfo->dobj.name,
								  .owner = evtinfo->evtowner,
								  .description = "EVENT TRIGGER",
								  .section = SECTION_POST_DATA,
								  .createStmt = query->data,
								  .dropStmt = delqry->data));

	if (evtinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "EVENT TRIGGER", qevtname,
					NULL, evtinfo->evtowner,
					evtinfo->dobj.catId, 0, evtinfo->dobj.dumpId);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(delqry);
	free(qevtname);
}

/*
 * dumpRule
 *		Dump a rule
 */
static void
dumpRule(Archive *fout, RuleInfo *rinfo)
{
	DumpOptions *dopt = fout->dopt;
	TableInfo  *tbinfo = rinfo->ruletable;
	bool		is_view;
	PQExpBuffer query;
	PQExpBuffer cmd;
	PQExpBuffer delcmd;
	PQExpBuffer ruleprefix;
	char	   *qtabname;
	PGresult   *res;
	char	   *tag;

	/* Skip if not to be dumped */
	if (!rinfo->dobj.dump || dopt->dataOnly)
		return;

	/*
	 * If it is an ON SELECT rule that is created implicitly by CREATE VIEW,
	 * we do not want to dump it as a separate object.
	 */
	if (!rinfo->separate)
		return;

	/*
	 * If it's an ON SELECT rule, we want to print it as a view definition,
	 * instead of a rule.
	 */
	is_view = (rinfo->ev_type == '1' && rinfo->is_instead);

	query = createPQExpBuffer();
	cmd = createPQExpBuffer();
	delcmd = createPQExpBuffer();
	ruleprefix = createPQExpBuffer();

	qtabname = pg_strdup(fmtId(tbinfo->dobj.name));

	if (is_view)
	{
		PQExpBuffer result;

		/*
		 * We need OR REPLACE here because we'll be replacing a dummy view.
		 * Otherwise this should look largely like the regular view dump code.
		 */
		appendPQExpBuffer(cmd, "CREATE OR REPLACE VIEW %s",
						  fmtQualifiedDumpable(tbinfo));
		if (nonemptyReloptions(tbinfo->reloptions))
		{
			appendPQExpBufferStr(cmd, " WITH (");
			appendReloptionsArrayAH(cmd, tbinfo->reloptions, "", fout);
			appendPQExpBufferChar(cmd, ')');
		}
		result = createViewAsClause(fout, tbinfo);
		appendPQExpBuffer(cmd, " AS\n%s", result->data);
		destroyPQExpBuffer(result);
		if (tbinfo->checkoption != NULL)
			appendPQExpBuffer(cmd, "\n  WITH %s CHECK OPTION",
							  tbinfo->checkoption);
		appendPQExpBufferStr(cmd, ";\n");
	}
	else
	{
		/* In the rule case, just print pg_get_ruledef's result verbatim */
		appendPQExpBuffer(query,
						  "SELECT pg_catalog.pg_get_ruledef('%u'::pg_catalog.oid)",
						  rinfo->dobj.catId.oid);

		res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

		if (PQntuples(res) != 1)
		{
			pg_log_error("query to get rule \"%s\" for table \"%s\" failed: wrong number of rows returned",
						 rinfo->dobj.name, tbinfo->dobj.name);
			exit_nicely(1);
		}

		printfPQExpBuffer(cmd, "%s\n", PQgetvalue(res, 0, 0));

		PQclear(res);
	}

	/*
	 * Add the command to alter the rules replication firing semantics if it
	 * differs from the default.
	 */
	if (rinfo->ev_enabled != 'O')
	{
		appendPQExpBuffer(cmd, "ALTER TABLE %s ", fmtQualifiedDumpable(tbinfo));
		switch (rinfo->ev_enabled)
		{
			case 'A':
				appendPQExpBuffer(cmd, "ENABLE ALWAYS RULE %s;\n",
								  fmtId(rinfo->dobj.name));
				break;
			case 'R':
				appendPQExpBuffer(cmd, "ENABLE REPLICA RULE %s;\n",
								  fmtId(rinfo->dobj.name));
				break;
			case 'D':
				appendPQExpBuffer(cmd, "DISABLE RULE %s;\n",
								  fmtId(rinfo->dobj.name));
				break;
		}
	}

	if (is_view)
	{
		/*
		 * We can't DROP a view's ON SELECT rule.  Instead, use CREATE OR
		 * REPLACE VIEW to replace the rule with something with minimal
		 * dependencies.
		 */
		PQExpBuffer result;

		appendPQExpBuffer(delcmd, "CREATE OR REPLACE VIEW %s",
						  fmtQualifiedDumpable(tbinfo));
		result = createDummyViewAsClause(fout, tbinfo);
		appendPQExpBuffer(delcmd, " AS\n%s;\n", result->data);
		destroyPQExpBuffer(result);
	}
	else
	{
		appendPQExpBuffer(delcmd, "DROP RULE %s ",
						  fmtId(rinfo->dobj.name));
		appendPQExpBuffer(delcmd, "ON %s;\n",
						  fmtQualifiedDumpable(tbinfo));
	}

	appendPQExpBuffer(ruleprefix, "RULE %s ON",
					  fmtId(rinfo->dobj.name));

	tag = psprintf("%s %s", tbinfo->dobj.name, rinfo->dobj.name);

	if (rinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, rinfo->dobj.catId, rinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tag,
								  .namespace = tbinfo->dobj.namespace->dobj.name,
								  .owner = tbinfo->rolname,
								  .description = "RULE",
								  .section = SECTION_POST_DATA,
								  .createStmt = cmd->data,
								  .dropStmt = delcmd->data));

	/* Dump rule comments */
	if (rinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, ruleprefix->data, qtabname,
					tbinfo->dobj.namespace->dobj.name,
					tbinfo->rolname,
					rinfo->dobj.catId, 0, rinfo->dobj.dumpId);

	free(tag);
	destroyPQExpBuffer(query);
	destroyPQExpBuffer(cmd);
	destroyPQExpBuffer(delcmd);
	destroyPQExpBuffer(ruleprefix);
	free(qtabname);
}

/*
 * getExtensionMembership --- obtain extension membership data
 *
 * We need to identify objects that are extension members as soon as they're
 * loaded, so that we can correctly determine whether they need to be dumped.
 * Generally speaking, extension member objects will get marked as *not* to
 * be dumped, as they will be recreated by the single CREATE EXTENSION
 * command.  However, in binary upgrade mode we still need to dump the members
 * individually.
 */
void
getExtensionMembership(Archive *fout, ExtensionInfo extinfo[],
					   int numExtensions)
{
	PQExpBuffer query;
	PGresult   *res;
	int			ntups,
				nextmembers,
				i;
	int			i_classid,
				i_objid,
				i_refobjid;
	ExtensionMemberId *extmembers;
	ExtensionInfo *ext;

	/* Nothing to do if no extensions */
	if (numExtensions == 0)
		return;

	query = createPQExpBuffer();

	/* refclassid constraint is redundant but may speed the search */
	appendPQExpBufferStr(query, "SELECT "
						 "classid, objid, refobjid "
						 "FROM pg_depend "
						 "WHERE refclassid = 'pg_extension'::regclass "
						 "AND deptype = 'e' "
						 "ORDER BY 3");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_classid = PQfnumber(res, "classid");
	i_objid = PQfnumber(res, "objid");
	i_refobjid = PQfnumber(res, "refobjid");

	extmembers = (ExtensionMemberId *) pg_malloc(ntups * sizeof(ExtensionMemberId));
	nextmembers = 0;

	/*
	 * Accumulate data into extmembers[].
	 *
	 * Since we ordered the SELECT by referenced ID, we can expect that
	 * multiple entries for the same extension will appear together; this
	 * saves on searches.
	 */
	ext = NULL;

	for (i = 0; i < ntups; i++)
	{
		CatalogId	objId;
		Oid			extId;

		objId.tableoid = atooid(PQgetvalue(res, i, i_classid));
		objId.oid = atooid(PQgetvalue(res, i, i_objid));
		extId = atooid(PQgetvalue(res, i, i_refobjid));

		if (ext == NULL ||
			ext->dobj.catId.oid != extId)
			ext = findExtensionByOid(extId);

		if (ext == NULL)
		{
			/* shouldn't happen */
			pg_log_warning("could not find referenced extension %u", extId);
			continue;
		}

		extmembers[nextmembers].catId = objId;
		extmembers[nextmembers].ext = ext;
		nextmembers++;
	}

	PQclear(res);

	/* Remember the data for use later */
	setExtensionMembership(extmembers, nextmembers);

	destroyPQExpBuffer(query);
}

/*
 * processExtensionTables --- deal with extension configuration tables
 *
 * There are two parts to this process:
 *
 * 1. Identify and create dump records for extension configuration tables.
 *
 *	  Extensions can mark tables as "configuration", which means that the user
 *	  is able and expected to modify those tables after the extension has been
 *	  loaded.  For these tables, we dump out only the data- the structure is
 *	  expected to be handled at CREATE EXTENSION time, including any indexes or
 *	  foreign keys, which brings us to-
 *
 * 2. Record FK dependencies between configuration tables.
 *
 *	  Due to the FKs being created at CREATE EXTENSION time and therefore before
 *	  the data is loaded, we have to work out what the best order for reloading
 *	  the data is, to avoid FK violations when the tables are restored.  This is
 *	  not perfect- we can't handle circular dependencies and if any exist they
 *	  will cause an invalid dump to be produced (though at least all of the data
 *	  is included for a user to manually restore).  This is currently documented
 *	  but perhaps we can provide a better solution in the future.
 */
void
processExtensionTables(Archive *fout, ExtensionInfo extinfo[],
					   int numExtensions)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer query;
	PGresult   *res;
	int			ntups,
				i;
	int			i_conrelid,
				i_confrelid;

	/* Nothing to do if no extensions */
	if (numExtensions == 0)
		return;

	/*
	 * Identify extension configuration tables and create TableDataInfo
	 * objects for them, ensuring their data will be dumped even though the
	 * tables themselves won't be.
	 *
	 * Note that we create TableDataInfo objects even in schemaOnly mode, ie,
	 * user data in a configuration table is treated like schema data. This
	 * seems appropriate since system data in a config table would get
	 * reloaded by CREATE EXTENSION.
	 */
	for (i = 0; i < numExtensions; i++)
	{
		ExtensionInfo *curext = &(extinfo[i]);
		char	   *extconfig = curext->extconfig;
		char	   *extcondition = curext->extcondition;
		char	  **extconfigarray = NULL;
		char	  **extconditionarray = NULL;
		int			nconfigitems = 0;
		int			nconditionitems = 0;

		if (strlen(extconfig) != 0 || strlen(extcondition) != 0)
		{
			int			j;

			if (!parsePGArray(extconfig, &extconfigarray, &nconfigitems))
				fatal("could not parse extension configuration array");
			if (!parsePGArray(extcondition, &extconditionarray, &nconditionitems))
				fatal("could not parse extension condition array");
			if (nconfigitems != nconditionitems)
				fatal("mismatched number of configurations and conditions for extension");

			for (j = 0; j < nconfigitems; j++)
			{
				TableInfo  *configtbl;
				Oid			configtbloid = atooid(extconfigarray[j]);
				bool		dumpobj =
				curext->dobj.dump & DUMP_COMPONENT_DEFINITION;

				configtbl = findTableByOid(configtbloid);
				if (configtbl == NULL)
					continue;

				/*
				 * Tables of not-to-be-dumped extensions shouldn't be dumped
				 * unless the table or its schema is explicitly included
				 */
				if (!(curext->dobj.dump & DUMP_COMPONENT_DEFINITION))
				{
					/* check table explicitly requested */
					if (table_include_oids.head != NULL &&
						simple_oid_list_member(&table_include_oids,
											   configtbloid))
						dumpobj = true;

					/* check table's schema explicitly requested */
					if (configtbl->dobj.namespace->dobj.dump &
						DUMP_COMPONENT_DATA)
						dumpobj = true;
				}

				/* check table excluded by an exclusion switch */
				if (table_exclude_oids.head != NULL &&
					simple_oid_list_member(&table_exclude_oids,
										   configtbloid))
					dumpobj = false;

				/* check schema excluded by an exclusion switch */
				if (simple_oid_list_member(&schema_exclude_oids,
										   configtbl->dobj.namespace->dobj.catId.oid))
					dumpobj = false;

				if (dumpobj)
				{
					makeTableDataInfo(dopt, configtbl);
					if (configtbl->dataObj != NULL)
					{
						if (strlen(extconditionarray[j]) > 0)
							configtbl->dataObj->filtercond = pg_strdup(extconditionarray[j]);
					}
				}
			}
		}
		if (extconfigarray)
			free(extconfigarray);
		if (extconditionarray)
			free(extconditionarray);
	}

	/*
	 * Now that all the TableDataInfo objects have been created for all the
	 * extensions, check their FK dependencies and register them to try and
	 * dump the data out in an order that they can be restored in.
	 *
	 * Note that this is not a problem for user tables as their FKs are
	 * recreated after the data has been loaded.
	 */

	query = createPQExpBuffer();

	printfPQExpBuffer(query,
					  "SELECT conrelid, confrelid "
					  "FROM pg_constraint "
					  "JOIN pg_depend ON (objid = confrelid) "
					  "WHERE contype = 'f' "
					  "AND refclassid = 'pg_extension'::regclass "
					  "AND classid = 'pg_class'::regclass;");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);
	ntups = PQntuples(res);

	i_conrelid = PQfnumber(res, "conrelid");
	i_confrelid = PQfnumber(res, "confrelid");

	/* Now get the dependencies and register them */
	for (i = 0; i < ntups; i++)
	{
		Oid			conrelid,
					confrelid;
		TableInfo  *reftable,
				   *contable;

		conrelid = atooid(PQgetvalue(res, i, i_conrelid));
		confrelid = atooid(PQgetvalue(res, i, i_confrelid));
		contable = findTableByOid(conrelid);
		reftable = findTableByOid(confrelid);

		if (reftable == NULL ||
			reftable->dataObj == NULL ||
			contable == NULL ||
			contable->dataObj == NULL)
			continue;

		/*
		 * Make referencing TABLE_DATA object depend on the referenced table's
		 * TABLE_DATA object.
		 */
		addObjectDependency(&contable->dataObj->dobj,
							reftable->dataObj->dobj.dumpId);
	}
	PQclear(res);
	destroyPQExpBuffer(query);
}

/*
 * getDependencies --- obtain available dependency data
 */
static void
getDependencies(Archive *fout)
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

	pg_log_info("reading dependency data");

	query = createPQExpBuffer();

	/*
	 * Messy query to collect the dependency data we need.  Note that we
	 * ignore the sub-object column, so that dependencies of or on a column
	 * look the same as dependencies of or on a whole table.
	 *
	 * PIN dependencies aren't interesting, and EXTENSION dependencies were
	 * already processed by getExtensionMembership.
	 */
	appendPQExpBufferStr(query, "SELECT "
						 "classid, objid, refclassid, refobjid, deptype "
						 "FROM pg_depend "
						 "WHERE deptype != 'p' AND deptype != 'e'\n");

	/*
	 * Since we don't treat pg_amop entries as separate DumpableObjects, we
	 * have to translate their dependencies into dependencies of their parent
	 * opfamily.  Ignore internal dependencies though, as those will point to
	 * their parent opclass, which we needn't consider here (and if we did,
	 * it'd just result in circular dependencies).  Also, "loose" opfamily
	 * entries will have dependencies on their parent opfamily, which we
	 * should drop since they'd likewise become useless self-dependencies.
	 * (But be sure to keep deps on *other* opfamilies; see amopsortfamily.)
	 *
	 * Skip this for pre-8.3 source servers: pg_opfamily doesn't exist there,
	 * and the (known) cases where it would matter to have these dependencies
	 * can't arise anyway.
	 */
	if (fout->remoteVersion >= 80300)
	{
		appendPQExpBufferStr(query, "UNION ALL\n"
							 "SELECT 'pg_opfamily'::regclass AS classid, amopfamily AS objid, refclassid, refobjid, deptype "
							 "FROM pg_depend d, pg_amop o "
							 "WHERE deptype NOT IN ('p', 'e', 'i') AND "
							 "classid = 'pg_amop'::regclass AND objid = o.oid "
							 "AND NOT (refclassid = 'pg_opfamily'::regclass AND amopfamily = refobjid)\n");

		/* Likewise for pg_amproc entries */
		appendPQExpBufferStr(query, "UNION ALL\n"
							 "SELECT 'pg_opfamily'::regclass AS classid, amprocfamily AS objid, refclassid, refobjid, deptype "
							 "FROM pg_depend d, pg_amproc p "
							 "WHERE deptype NOT IN ('p', 'e', 'i') AND "
							 "classid = 'pg_amproc'::regclass AND objid = p.oid "
							 "AND NOT (refclassid = 'pg_opfamily'::regclass AND amprocfamily = refobjid)\n");
	}

	/* Sort the output for efficiency below */
	appendPQExpBufferStr(query, "ORDER BY 1,2");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

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
			pg_log_warning("no referencing object %u %u",
						   objId.tableoid, objId.oid);
#endif
			continue;
		}

		refdobj = findObjectByCatalogId(refobjId);

		if (refdobj == NULL)
		{
#ifdef NOT_USED
			pg_log_warning("no referenced object %u %u",
						   refobjId.tableoid, refobjId.oid);
#endif
			continue;
		}

		/*
		 * For 'x' dependencies, mark the object for later; we still add the
		 * normal dependency, for possible ordering purposes.  Currently
		 * pg_dump_sort.c knows to put extensions ahead of all object types
		 * that could possibly depend on them, but this is safer.
		 */
		if (deptype == 'x')
			dobj->depends_on_ext = true;

		/*
		 * Ordinarily, table rowtypes have implicit dependencies on their
		 * tables.  However, for a composite type the implicit dependency goes
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
 * createBoundaryObjects - create dummy DumpableObjects to represent
 * dump section boundaries.
 */
static DumpableObject *
createBoundaryObjects(void)
{
	DumpableObject *dobjs;

	dobjs = (DumpableObject *) pg_malloc(2 * sizeof(DumpableObject));

	dobjs[0].objType = DO_PRE_DATA_BOUNDARY;
	dobjs[0].catId = nilCatalogId;
	AssignDumpId(dobjs + 0);
	dobjs[0].name = pg_strdup("PRE-DATA BOUNDARY");

	dobjs[1].objType = DO_POST_DATA_BOUNDARY;
	dobjs[1].catId = nilCatalogId;
	AssignDumpId(dobjs + 1);
	dobjs[1].name = pg_strdup("POST-DATA BOUNDARY");

	return dobjs;
}

/*
 * addBoundaryDependencies - add dependencies as needed to enforce the dump
 * section boundaries.
 */
static void
addBoundaryDependencies(DumpableObject **dobjs, int numObjs,
						DumpableObject *boundaryObjs)
{
	DumpableObject *preDataBound = boundaryObjs + 0;
	DumpableObject *postDataBound = boundaryObjs + 1;
	int			i;

	for (i = 0; i < numObjs; i++)
	{
		DumpableObject *dobj = dobjs[i];

		/*
		 * The classification of object types here must match the SECTION_xxx
		 * values assigned during subsequent ArchiveEntry calls!
		 */
		switch (dobj->objType)
		{
			case DO_NAMESPACE:
			case DO_EXTENSION:
			case DO_TYPE:
			case DO_SHELL_TYPE:
			case DO_FUNC:
			case DO_AGG:
			case DO_OPERATOR:
			case DO_ACCESS_METHOD:
			case DO_OPCLASS:
			case DO_OPFAMILY:
			case DO_COLLATION:
			case DO_CONVERSION:
			case DO_TABLE:
			case DO_ATTRDEF:
			case DO_PROCLANG:
			case DO_CAST:
			case DO_DUMMY_TYPE:
			case DO_TSPARSER:
			case DO_TSDICT:
			case DO_TSTEMPLATE:
			case DO_TSCONFIG:
			case DO_FDW:
			case DO_FOREIGN_SERVER:
			case DO_TRANSFORM:
			case DO_BLOB:
				/* Pre-data objects: must come before the pre-data boundary */
				addObjectDependency(preDataBound, dobj->dumpId);
				break;
			case DO_TABLE_DATA:
			case DO_SEQUENCE_SET:
			case DO_BLOB_DATA:
				/* Data objects: must come between the boundaries */
				addObjectDependency(dobj, preDataBound->dumpId);
				addObjectDependency(postDataBound, dobj->dumpId);
				break;
			case DO_INDEX:
			case DO_INDEX_ATTACH:
			case DO_STATSEXT:
			case DO_REFRESH_MATVIEW:
			case DO_TRIGGER:
			case DO_EVENT_TRIGGER:
			case DO_DEFAULT_ACL:
			case DO_POLICY:
			case DO_PUBLICATION:
			case DO_PUBLICATION_REL:
			case DO_SUBSCRIPTION:
				/* Post-data objects: must come after the post-data boundary */
				addObjectDependency(dobj, postDataBound->dumpId);
				break;
			case DO_RULE:
				/* Rules are post-data, but only if dumped separately */
				if (((RuleInfo *) dobj)->separate)
					addObjectDependency(dobj, postDataBound->dumpId);
				break;
			case DO_CONSTRAINT:
			case DO_FK_CONSTRAINT:
				/* Constraints are post-data, but only if dumped separately */
				if (((ConstraintInfo *) dobj)->separate)
					addObjectDependency(dobj, postDataBound->dumpId);
				break;
			case DO_PRE_DATA_BOUNDARY:
				/* nothing to do */
				break;
			case DO_POST_DATA_BOUNDARY:
				/* must come after the pre-data boundary */
				addObjectDependency(dobj, preDataBound->dumpId);
				break;
		}
	}
}


/*
 * BuildArchiveDependencies - create dependency data for archive TOC entries
 *
 * The raw dependency data obtained by getDependencies() is not terribly
 * useful in an archive dump, because in many cases there are dependency
 * chains linking through objects that don't appear explicitly in the dump.
 * For example, a view will depend on its _RETURN rule while the _RETURN rule
 * will depend on other objects --- but the rule will not appear as a separate
 * object in the dump.  We need to adjust the view's dependencies to include
 * whatever the rule depends on that is included in the dump.
 *
 * Just to make things more complicated, there are also "special" dependencies
 * such as the dependency of a TABLE DATA item on its TABLE, which we must
 * not rearrange because pg_restore knows that TABLE DATA only depends on
 * its table.  In these cases we must leave the dependencies strictly as-is
 * even if they refer to not-to-be-dumped objects.
 *
 * To handle this, the convention is that "special" dependencies are created
 * during ArchiveEntry calls, and an archive TOC item that has any such
 * entries will not be touched here.  Otherwise, we recursively search the
 * DumpableObject data structures to build the correct dependencies for each
 * archive TOC item.
 */
static void
BuildArchiveDependencies(Archive *fout)
{
	ArchiveHandle *AH = (ArchiveHandle *) fout;
	TocEntry   *te;

	/* Scan all TOC entries in the archive */
	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		DumpableObject *dobj;
		DumpId	   *dependencies;
		int			nDeps;
		int			allocDeps;

		/* No need to process entries that will not be dumped */
		if (te->reqs == 0)
			continue;
		/* Ignore entries that already have "special" dependencies */
		if (te->nDeps > 0)
			continue;
		/* Otherwise, look up the item's original DumpableObject, if any */
		dobj = findObjectByDumpId(te->dumpId);
		if (dobj == NULL)
			continue;
		/* No work if it has no dependencies */
		if (dobj->nDeps <= 0)
			continue;
		/* Set up work array */
		allocDeps = 64;
		dependencies = (DumpId *) pg_malloc(allocDeps * sizeof(DumpId));
		nDeps = 0;
		/* Recursively find all dumpable dependencies */
		findDumpableDependencies(AH, dobj,
								 &dependencies, &nDeps, &allocDeps);
		/* And save 'em ... */
		if (nDeps > 0)
		{
			dependencies = (DumpId *) pg_realloc(dependencies,
												 nDeps * sizeof(DumpId));
			te->dependencies = dependencies;
			te->nDeps = nDeps;
		}
		else
			free(dependencies);
	}
}

/* Recursive search subroutine for BuildArchiveDependencies */
static void
findDumpableDependencies(ArchiveHandle *AH, DumpableObject *dobj,
						 DumpId **dependencies, int *nDeps, int *allocDeps)
{
	int			i;

	/*
	 * Ignore section boundary objects: if we search through them, we'll
	 * report lots of bogus dependencies.
	 */
	if (dobj->objType == DO_PRE_DATA_BOUNDARY ||
		dobj->objType == DO_POST_DATA_BOUNDARY)
		return;

	for (i = 0; i < dobj->nDeps; i++)
	{
		DumpId		depid = dobj->dependencies[i];

		if (TocIDRequired(AH, depid) != 0)
		{
			/* Object will be dumped, so just reference it as a dependency */
			if (*nDeps >= *allocDeps)
			{
				*allocDeps *= 2;
				*dependencies = (DumpId *) pg_realloc(*dependencies,
													  *allocDeps * sizeof(DumpId));
			}
			(*dependencies)[*nDeps] = depid;
			(*nDeps)++;
		}
		else
		{
			/*
			 * Object will not be dumped, so recursively consider its deps. We
			 * rely on the assumption that sortDumpableObjects already broke
			 * any dependency loops, else we might recurse infinitely.
			 */
			DumpableObject *otherdobj = findObjectByDumpId(depid);

			if (otherdobj)
				findDumpableDependencies(AH, otherdobj,
										 dependencies, nDeps, allocDeps);
		}
	}
}


/*
 * getFormattedTypeName - retrieve a nicely-formatted type name for the
 * given type OID.
 *
 * This does not guarantee to schema-qualify the output, so it should not
 * be used to create the target object name for CREATE or ALTER commands.
 *
 * TODO: there might be some value in caching the results.
 */
static char *
getFormattedTypeName(Archive *fout, Oid oid, OidOptions opts)
{
	char	   *result;
	PQExpBuffer query;
	PGresult   *res;

	if (oid == 0)
	{
		if ((opts & zeroAsStar) != 0)
			return pg_strdup("*");
		else if ((opts & zeroAsNone) != 0)
			return pg_strdup("NONE");
	}

	query = createPQExpBuffer();
	appendPQExpBuffer(query, "SELECT pg_catalog.format_type('%u'::pg_catalog.oid, NULL)",
					  oid);

	res = ExecuteSqlQueryForSingleRow(fout, query->data);

	/* result of format_type is already quoted */
	result = pg_strdup(PQgetvalue(res, 0, 0));

	PQclear(res);
	destroyPQExpBuffer(query);

	return result;
}

/*
 * Return a column list clause for the given relation.
 *
 * Special case: if there are no undropped columns in the relation, return
 * "", not an invalid "()" column list.
 */
static const char *
fmtCopyColumnList(const TableInfo *ti, PQExpBuffer buffer)
{
	int			numatts = ti->numatts;
	char	  **attnames = ti->attnames;
	bool	   *attisdropped = ti->attisdropped;
	char	   *attgenerated = ti->attgenerated;
	bool		needComma;
	int			i;

	appendPQExpBufferChar(buffer, '(');
	needComma = false;
	for (i = 0; i < numatts; i++)
	{
		if (attisdropped[i])
			continue;
		if (attgenerated[i])
			continue;
		if (needComma)
			appendPQExpBufferStr(buffer, ", ");
		appendPQExpBufferStr(buffer, fmtId(attnames[i]));
		needComma = true;
	}

	if (!needComma)
		return "";				/* no undropped columns */

	appendPQExpBufferChar(buffer, ')');
	return buffer->data;
}

/*
 * Check if a reloptions array is nonempty.
 */
static bool
nonemptyReloptions(const char *reloptions)
{
	/* Don't want to print it if it's just "{}" */
	return (reloptions != NULL && strlen(reloptions) > 2);
}

/*
 * Generate UPDATE statements to import the collation versions into the new
 * cluster, during a binary upgrade.
 */
static void
appendIndexCollationVersion(PQExpBuffer buffer, IndxInfo *indxinfo, int enc,
							bool coll_unknown, Archive *fout)
{
	char	   *inddependcollnames = indxinfo->inddependcollnames;
	char	   *inddependcollversions = indxinfo->inddependcollversions;
	char	  **inddependcollnamesarray;
	char	  **inddependcollversionsarray;
	int			ninddependcollnames;
	int			ninddependcollversions;

	/*
	 * By default, the new cluster's index will have pg_depends rows with
	 * current collation versions, meaning that we assume the index isn't
	 * corrupted if importing from a release that didn't record versions.
	 * However, if --index-collation-versions-unknown was passed in, then we
	 * assume such indexes might be corrupted, and clobber versions with
	 * 'unknown' to trigger version warnings.
	 */
	if (coll_unknown)
	{
		appendPQExpBuffer(buffer,
						  "\n-- For binary upgrade, clobber new index's collation versions\n");
		appendPQExpBuffer(buffer,
						  "UPDATE pg_catalog.pg_depend SET refobjversion = 'unknown' WHERE objid = '%u'::pg_catalog.oid AND refclassid = 'pg_catalog.pg_collation'::regclass AND refobjversion IS NOT NULL;\n",
						  indxinfo->dobj.catId.oid);
	}

	/* Restore the versions that were recorded by the old cluster (if any). */
	if (strlen(inddependcollnames) == 0 && strlen(inddependcollversions) == 0)
	{
		ninddependcollnames = ninddependcollversions = 0;
		inddependcollnamesarray = inddependcollversionsarray = NULL;
	}
	else
	{
		if (!parsePGArray(inddependcollnames,
						  &inddependcollnamesarray,
						  &ninddependcollnames))
			fatal("could not parse index collation name array");
		if (!parsePGArray(inddependcollversions,
						  &inddependcollversionsarray,
						  &ninddependcollversions))
			fatal("could not parse index collation version array");
	}

	if (ninddependcollnames != ninddependcollversions)
		fatal("mismatched number of collation names and versions for index");

	if (ninddependcollnames > 0)
		appendPQExpBufferStr(buffer,
							 "\n-- For binary upgrade, restore old index's collation versions\n");
	for (int i = 0; i < ninddependcollnames; i++)
	{
		/*
		 * Import refobjversion from the old cluster, being careful to resolve
		 * the collation OID by name in the new cluster.
		 */
		appendPQExpBuffer(buffer,
						  "UPDATE pg_catalog.pg_depend SET refobjversion = %s WHERE objid = '%u'::pg_catalog.oid AND refclassid = 'pg_catalog.pg_collation'::regclass AND refobjversion IS NOT NULL AND refobjid = ",
						  inddependcollversionsarray[i],
						  indxinfo->dobj.catId.oid);
		appendStringLiteralAH(buffer, inddependcollnamesarray[i], fout);
		appendPQExpBuffer(buffer, "::regcollation;\n");
	}

	if (inddependcollnamesarray)
		free(inddependcollnamesarray);
	if (inddependcollversionsarray)
		free(inddependcollversionsarray);
}

/*
 * Format a reloptions array and append it to the given buffer.
 *
 * "prefix" is prepended to the option names; typically it's "" or "toast.".
 */
static void
appendReloptionsArrayAH(PQExpBuffer buffer, const char *reloptions,
						const char *prefix, Archive *fout)
{
	bool		res;

	res = appendReloptionsArray(buffer, reloptions, prefix, fout->encoding,
								fout->std_strings);
	if (!res)
		pg_log_warning("could not parse reloptions array");
}
