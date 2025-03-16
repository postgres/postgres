/*-------------------------------------------------------------------------
 *
 * pg_dump.c
 *	  pg_dump is a utility for dumping out a postgres database
 *	  into a script file.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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
#include "catalog/pg_authid_d.h"
#include "catalog/pg_cast_d.h"
#include "catalog/pg_class_d.h"
#include "catalog/pg_default_acl_d.h"
#include "catalog/pg_largeobject_d.h"
#include "catalog/pg_proc_d.h"
#include "catalog/pg_publication_d.h"
#include "catalog/pg_subscription_d.h"
#include "catalog/pg_type_d.h"
#include "common/connect.h"
#include "common/int.h"
#include "common/relpath.h"
#include "common/shortest_dec.h"
#include "compress_io.h"
#include "dumputils.h"
#include "fe_utils/option_utils.h"
#include "fe_utils/string_utils.h"
#include "filter.h"
#include "getopt_long.h"
#include "libpq/libpq-fs.h"
#include "parallel.h"
#include "pg_backup_db.h"
#include "pg_backup_utils.h"
#include "pg_dump.h"
#include "storage/block.h"

typedef struct
{
	Oid			roleoid;		/* role's OID */
	const char *rolename;		/* role's name */
} RoleNameItem;

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

typedef struct
{
	Oid			oid;			/* object OID */
	char		relkind;		/* object kind */
	RelFileNumber relfilenumber;	/* object filenode */
	Oid			toast_oid;		/* toast table OID */
	RelFileNumber toast_relfilenumber;	/* toast table filenode */
	Oid			toast_index_oid;	/* toast table index OID */
	RelFileNumber toast_index_relfilenumber;	/* toast table index filenode */
} BinaryUpgradeClassOidItem;

/* sequence types */
typedef enum SeqType
{
	SEQTYPE_SMALLINT,
	SEQTYPE_INTEGER,
	SEQTYPE_BIGINT,
} SeqType;

static const char *const SeqTypeNames[] =
{
	[SEQTYPE_SMALLINT] = "smallint",
	[SEQTYPE_INTEGER] = "integer",
	[SEQTYPE_BIGINT] = "bigint",
};

StaticAssertDecl(lengthof(SeqTypeNames) == (SEQTYPE_BIGINT + 1),
				 "array length mismatch");

typedef struct
{
	Oid			oid;			/* sequence OID */
	SeqType		seqtype;		/* data type of sequence */
	bool		cycled;			/* whether sequence cycles */
	int64		minv;			/* minimum value */
	int64		maxv;			/* maximum value */
	int64		startv;			/* start value */
	int64		incby;			/* increment value */
	int64		cache;			/* cache size */
	int64		last_value;		/* last value of sequence */
	bool		is_called;		/* whether nextval advances before returning */
} SequenceItem;

typedef enum OidOptions
{
	zeroIsError = 1,
	zeroAsStar = 2,
	zeroAsNone = 4,
} OidOptions;

/* global decls */
static bool dosync = true;		/* Issue fsync() to make dump durable on disk. */

static Oid	g_last_builtin_oid; /* value of the last builtin oid */

/* The specified names/patterns should to match at least one entity */
static int	strict_names = 0;

static pg_compress_algorithm compression_algorithm = PG_COMPRESSION_NONE;

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
static SimpleStringList table_include_patterns_and_children = {NULL, NULL};
static SimpleOidList table_include_oids = {NULL, NULL};
static SimpleStringList table_exclude_patterns = {NULL, NULL};
static SimpleStringList table_exclude_patterns_and_children = {NULL, NULL};
static SimpleOidList table_exclude_oids = {NULL, NULL};
static SimpleStringList tabledata_exclude_patterns = {NULL, NULL};
static SimpleStringList tabledata_exclude_patterns_and_children = {NULL, NULL};
static SimpleOidList tabledata_exclude_oids = {NULL, NULL};

static SimpleStringList foreign_servers_include_patterns = {NULL, NULL};
static SimpleOidList foreign_servers_include_oids = {NULL, NULL};

static SimpleStringList extension_include_patterns = {NULL, NULL};
static SimpleOidList extension_include_oids = {NULL, NULL};

static SimpleStringList extension_exclude_patterns = {NULL, NULL};
static SimpleOidList extension_exclude_oids = {NULL, NULL};

static const CatalogId nilCatalogId = {0, 0};

/* override for standard extra_float_digits setting */
static bool have_extra_float_digits = false;
static int	extra_float_digits;

/* sorted table of role names */
static RoleNameItem *rolenames = NULL;
static int	nrolenames = 0;

/* sorted table of comments */
static CommentItem *comments = NULL;
static int	ncomments = 0;

/* sorted table of security labels */
static SecLabelItem *seclabels = NULL;
static int	nseclabels = 0;

/* sorted table of pg_class information for binary upgrade */
static BinaryUpgradeClassOidItem *binaryUpgradeClassOids = NULL;
static int	nbinaryUpgradeClassOids = 0;

/* sorted table of sequences */
static SequenceItem *sequences = NULL;
static int	nsequences = 0;

/*
 * The default number of rows per INSERT when
 * --inserts is specified without --rows-per-insert
 */
#define DUMP_DEFAULT_ROWS_PER_INSERT 1

/*
 * Maximum number of large objects to group into a single ArchiveEntry.
 * At some point we might want to make this user-controllable, but for now
 * a hard-wired setting will suffice.
 */
#define MAX_BLOBS_PER_ARCHIVE_ENTRY 1000

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
static void expand_extension_name_patterns(Archive *fout,
										   SimpleStringList *patterns,
										   SimpleOidList *oids,
										   bool strict_names);
static void expand_foreign_server_name_patterns(Archive *fout,
												SimpleStringList *patterns,
												SimpleOidList *oids);
static void expand_table_name_patterns(Archive *fout,
									   SimpleStringList *patterns,
									   SimpleOidList *oids,
									   bool strict_names,
									   bool with_child_tables);
static void prohibit_crossdb_refs(PGconn *conn, const char *dbname,
								  const char *pattern);

static NamespaceInfo *findNamespace(Oid nsoid);
static void dumpTableData(Archive *fout, const TableDataInfo *tdinfo);
static void refreshMatViewData(Archive *fout, const TableDataInfo *tdinfo);
static const char *getRoleName(const char *roleoid_str);
static void collectRoleNames(Archive *fout);
static void getAdditionalACLs(Archive *fout);
static void dumpCommentExtended(Archive *fout, const char *type,
								const char *name, const char *namespace,
								const char *owner, CatalogId catalogId,
								int subid, DumpId dumpId,
								const char *initdb_comment);
static inline void dumpComment(Archive *fout, const char *type,
							   const char *name, const char *namespace,
							   const char *owner, CatalogId catalogId,
							   int subid, DumpId dumpId);
static int	findComments(Oid classoid, Oid objoid, CommentItem **items);
static void collectComments(Archive *fout);
static void dumpSecLabel(Archive *fout, const char *type, const char *name,
						 const char *namespace, const char *owner,
						 CatalogId catalogId, int subid, DumpId dumpId);
static int	findSecLabels(Oid classoid, Oid objoid, SecLabelItem **items);
static void collectSecLabels(Archive *fout);
static void dumpDumpableObject(Archive *fout, DumpableObject *dobj);
static void dumpNamespace(Archive *fout, const NamespaceInfo *nspinfo);
static void dumpExtension(Archive *fout, const ExtensionInfo *extinfo);
static void dumpType(Archive *fout, const TypeInfo *tyinfo);
static void dumpBaseType(Archive *fout, const TypeInfo *tyinfo);
static void dumpEnumType(Archive *fout, const TypeInfo *tyinfo);
static void dumpRangeType(Archive *fout, const TypeInfo *tyinfo);
static void dumpUndefinedType(Archive *fout, const TypeInfo *tyinfo);
static void dumpDomain(Archive *fout, const TypeInfo *tyinfo);
static void dumpCompositeType(Archive *fout, const TypeInfo *tyinfo);
static void dumpCompositeTypeColComments(Archive *fout, const TypeInfo *tyinfo,
										 PGresult *res);
static void dumpShellType(Archive *fout, const ShellTypeInfo *stinfo);
static void dumpProcLang(Archive *fout, const ProcLangInfo *plang);
static void dumpFunc(Archive *fout, const FuncInfo *finfo);
static void dumpCast(Archive *fout, const CastInfo *cast);
static void dumpTransform(Archive *fout, const TransformInfo *transform);
static void dumpOpr(Archive *fout, const OprInfo *oprinfo);
static void dumpAccessMethod(Archive *fout, const AccessMethodInfo *aminfo);
static void dumpOpclass(Archive *fout, const OpclassInfo *opcinfo);
static void dumpOpfamily(Archive *fout, const OpfamilyInfo *opfinfo);
static void dumpCollation(Archive *fout, const CollInfo *collinfo);
static void dumpConversion(Archive *fout, const ConvInfo *convinfo);
static void dumpRule(Archive *fout, const RuleInfo *rinfo);
static void dumpAgg(Archive *fout, const AggInfo *agginfo);
static void dumpTrigger(Archive *fout, const TriggerInfo *tginfo);
static void dumpEventTrigger(Archive *fout, const EventTriggerInfo *evtinfo);
static void dumpTable(Archive *fout, const TableInfo *tbinfo);
static void dumpTableSchema(Archive *fout, const TableInfo *tbinfo);
static void dumpTableAttach(Archive *fout, const TableAttachInfo *attachinfo);
static void dumpAttrDef(Archive *fout, const AttrDefInfo *adinfo);
static void collectSequences(Archive *fout);
static void dumpSequence(Archive *fout, const TableInfo *tbinfo);
static void dumpSequenceData(Archive *fout, const TableDataInfo *tdinfo);
static void dumpIndex(Archive *fout, const IndxInfo *indxinfo);
static void dumpIndexAttach(Archive *fout, const IndexAttachInfo *attachinfo);
static void dumpStatisticsExt(Archive *fout, const StatsExtInfo *statsextinfo);
static void dumpConstraint(Archive *fout, const ConstraintInfo *coninfo);
static void dumpTableConstraintComment(Archive *fout, const ConstraintInfo *coninfo);
static void dumpTSParser(Archive *fout, const TSParserInfo *prsinfo);
static void dumpTSDictionary(Archive *fout, const TSDictInfo *dictinfo);
static void dumpTSTemplate(Archive *fout, const TSTemplateInfo *tmplinfo);
static void dumpTSConfig(Archive *fout, const TSConfigInfo *cfginfo);
static void dumpForeignDataWrapper(Archive *fout, const FdwInfo *fdwinfo);
static void dumpForeignServer(Archive *fout, const ForeignServerInfo *srvinfo);
static void dumpUserMappings(Archive *fout,
							 const char *servername, const char *namespace,
							 const char *owner, CatalogId catalogId, DumpId dumpId);
static void dumpDefaultACL(Archive *fout, const DefaultACLInfo *daclinfo);

static DumpId dumpACL(Archive *fout, DumpId objDumpId, DumpId altDumpId,
					  const char *type, const char *name, const char *subname,
					  const char *nspname, const char *tag, const char *owner,
					  const DumpableAcl *dacl);

static void getDependencies(Archive *fout);
static void BuildArchiveDependencies(Archive *fout);
static void findDumpableDependencies(ArchiveHandle *AH, const DumpableObject *dobj,
									 DumpId **dependencies, int *nDeps, int *allocDeps);

static DumpableObject *createBoundaryObjects(void);
static void addBoundaryDependencies(DumpableObject **dobjs, int numObjs,
									DumpableObject *boundaryObjs);

static void addConstrChildIdxDeps(DumpableObject *dobj, const IndxInfo *refidx);
static void getDomainConstraints(Archive *fout, TypeInfo *tyinfo);
static void getTableData(DumpOptions *dopt, TableInfo *tblinfo, int numTables, char relkind);
static void makeTableDataInfo(DumpOptions *dopt, TableInfo *tbinfo);
static void buildMatViewRefreshDependencies(Archive *fout);
static void getTableDataFKConstraints(void);
static void determineNotNullFlags(Archive *fout, PGresult *res, int r,
								  TableInfo *tbinfo, int j,
								  int i_notnull_name, int i_notnull_noinherit,
								  int i_notnull_islocal);
static char *format_function_arguments(const FuncInfo *finfo, const char *funcargs,
									   bool is_agg);
static char *format_function_signature(Archive *fout,
									   const FuncInfo *finfo, bool honor_quotes);
static char *convertRegProcReference(const char *proc);
static char *getFormattedOperatorName(const char *oproid);
static char *convertTSFunction(Archive *fout, Oid funcOid);
static const char *getFormattedTypeName(Archive *fout, Oid oid, OidOptions opts);
static void getLOs(Archive *fout);
static void dumpLO(Archive *fout, const LoInfo *loinfo);
static int	dumpLOs(Archive *fout, const void *arg);
static void dumpPolicy(Archive *fout, const PolicyInfo *polinfo);
static void dumpPublication(Archive *fout, const PublicationInfo *pubinfo);
static void dumpPublicationTable(Archive *fout, const PublicationRelInfo *pubrinfo);
static void dumpSubscription(Archive *fout, const SubscriptionInfo *subinfo);
static void dumpSubscriptionTable(Archive *fout, const SubRelInfo *subrinfo);
static void dumpDatabase(Archive *fout);
static void dumpDatabaseConfig(Archive *AH, PQExpBuffer outbuf,
							   const char *dbname, Oid dboid);
static void dumpEncoding(Archive *AH);
static void dumpStdStrings(Archive *AH);
static void dumpSearchPath(Archive *AH);
static void binary_upgrade_set_type_oids_by_type_oid(Archive *fout,
													 PQExpBuffer upgrade_buffer,
													 Oid pg_type_oid,
													 bool force_array_type,
													 bool include_multirange_type);
static void binary_upgrade_set_type_oids_by_rel(Archive *fout,
												PQExpBuffer upgrade_buffer,
												const TableInfo *tbinfo);
static void collectBinaryUpgradeClassOids(Archive *fout);
static void binary_upgrade_set_pg_class_oids(Archive *fout,
											 PQExpBuffer upgrade_buffer,
											 Oid pg_class_oid);
static void binary_upgrade_extension_member(PQExpBuffer upgrade_buffer,
											const DumpableObject *dobj,
											const char *objtype,
											const char *objname,
											const char *objnamespace);
static const char *getAttrName(int attrnum, const TableInfo *tblInfo);
static const char *fmtCopyColumnList(const TableInfo *ti, PQExpBuffer buffer);
static bool nonemptyReloptions(const char *reloptions);
static void appendReloptionsArrayAH(PQExpBuffer buffer, const char *reloptions,
									const char *prefix, Archive *fout);
static char *get_synchronized_snapshot(Archive *fout);
static void set_restrict_relation_kind(Archive *AH, const char *value);
static void setupDumpWorker(Archive *AH);
static TableInfo *getRootTableInfo(const TableInfo *tbinfo);
static bool forcePartitionRootLoad(const TableInfo *tbinfo);
static void read_dump_filters(const char *filename, DumpOptions *dopt);


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
	RestoreOptions *ropt;
	Archive    *fout;			/* the script file */
	bool		g_verbose = false;
	const char *dumpencoding = NULL;
	const char *dumpsnapshot = NULL;
	char	   *use_role = NULL;
	int			numWorkers = 1;
	int			plainText = 0;
	ArchiveFormat archiveFormat = archUnknown;
	ArchiveMode archiveMode;
	pg_compress_specification compression_spec = {0};
	char	   *compression_detail = NULL;
	char	   *compression_algorithm_str = "none";
	char	   *error_detail = NULL;
	bool		user_compression_defined = false;
	DataDirSyncMethod sync_method = DATA_DIR_SYNC_METHOD_FSYNC;
	bool		data_only = false;
	bool		schema_only = false;
	bool		statistics_only = false;
	bool		no_data = false;
	bool		no_schema = false;
	bool		no_statistics = false;

	static DumpOptions dopt;

	static struct option long_options[] = {
		{"data-only", no_argument, NULL, 'a'},
		{"blobs", no_argument, NULL, 'b'},
		{"large-objects", no_argument, NULL, 'b'},
		{"no-blobs", no_argument, NULL, 'B'},
		{"no-large-objects", no_argument, NULL, 'B'},
		{"clean", no_argument, NULL, 'c'},
		{"create", no_argument, NULL, 'C'},
		{"dbname", required_argument, NULL, 'd'},
		{"extension", required_argument, NULL, 'e'},
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
		{"no-table-access-method", no_argument, &dopt.outputNoTableAm, 1},
		{"no-tablespaces", no_argument, &dopt.outputNoTablespaces, 1},
		{"quote-all-identifiers", no_argument, &quote_all_identifiers, 1},
		{"load-via-partition-root", no_argument, &dopt.load_via_partition_root, 1},
		{"role", required_argument, NULL, 3},
		{"section", required_argument, NULL, 5},
		{"serializable-deferrable", no_argument, &dopt.serializable_deferrable, 1},
		{"snapshot", required_argument, NULL, 6},
		{"statistics-only", no_argument, NULL, 18},
		{"strict-names", no_argument, &strict_names, 1},
		{"use-set-session-authorization", no_argument, &dopt.use_setsessauth, 1},
		{"no-comments", no_argument, &dopt.no_comments, 1},
		{"no-data", no_argument, NULL, 19},
		{"no-policies", no_argument, &dopt.no_policies, 1},
		{"no-publications", no_argument, &dopt.no_publications, 1},
		{"no-schema", no_argument, NULL, 20},
		{"no-security-labels", no_argument, &dopt.no_security_labels, 1},
		{"no-statistics", no_argument, NULL, 21},
		{"no-subscriptions", no_argument, &dopt.no_subscriptions, 1},
		{"no-toast-compression", no_argument, &dopt.no_toast_compression, 1},
		{"no-unlogged-table-data", no_argument, &dopt.no_unlogged_table_data, 1},
		{"no-sync", no_argument, NULL, 7},
		{"on-conflict-do-nothing", no_argument, &dopt.do_nothing, 1},
		{"rows-per-insert", required_argument, NULL, 10},
		{"include-foreign-data", required_argument, NULL, 11},
		{"table-and-children", required_argument, NULL, 12},
		{"exclude-table-and-children", required_argument, NULL, 13},
		{"exclude-table-data-and-children", required_argument, NULL, 14},
		{"sync-method", required_argument, NULL, 15},
		{"filter", required_argument, NULL, 16},
		{"exclude-extension", required_argument, NULL, 17},

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

	while ((c = getopt_long(argc, argv, "abBcCd:e:E:f:F:h:j:n:N:Op:RsS:t:T:U:vwWxXZ:",
							long_options, &optindex)) != -1)
	{
		switch (c)
		{
			case 'a':			/* Dump data only */
				data_only = true;
				break;

			case 'b':			/* Dump LOs */
				dopt.outputLOs = true;
				break;

			case 'B':			/* Don't dump LOs */
				dopt.dontOutputLOs = true;
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

			case 'e':			/* include extension(s) */
				simple_string_list_append(&extension_include_patterns, optarg);
				dopt.include_everything = false;
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
				if (!option_parse_int(optarg, "-j/--jobs", 1,
									  PG_MAX_JOBS,
									  &numWorkers))
					exit_nicely(1);
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
				schema_only = true;
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

			case 'Z':			/* Compression */
				parse_compress_options(optarg, &compression_algorithm_str,
									   &compression_detail);
				user_compression_defined = true;
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
				if (!option_parse_int(optarg, "--extra-float-digits", -15, 3,
									  &extra_float_digits))
					exit_nicely(1);
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
				if (!option_parse_int(optarg, "--rows-per-insert", 1, INT_MAX,
									  &dopt.dump_inserts))
					exit_nicely(1);
				break;

			case 11:			/* include foreign data */
				simple_string_list_append(&foreign_servers_include_patterns,
										  optarg);
				break;

			case 12:			/* include table(s) and their children */
				simple_string_list_append(&table_include_patterns_and_children,
										  optarg);
				dopt.include_everything = false;
				break;

			case 13:			/* exclude table(s) and their children */
				simple_string_list_append(&table_exclude_patterns_and_children,
										  optarg);
				break;

			case 14:			/* exclude data of table(s) and children */
				simple_string_list_append(&tabledata_exclude_patterns_and_children,
										  optarg);
				break;

			case 15:
				if (!parse_sync_method(optarg, &sync_method))
					exit_nicely(1);
				break;

			case 16:			/* read object filters from file */
				read_dump_filters(optarg, &dopt);
				break;

			case 17:			/* exclude extension(s) */
				simple_string_list_append(&extension_exclude_patterns,
										  optarg);
				break;

			case 18:
				statistics_only = true;
				break;

			case 19:
				no_data = true;
				break;

			case 20:
				no_schema = true;
				break;

			case 21:
				no_statistics = true;
				break;

			default:
				/* getopt_long already emitted a complaint */
				pg_log_error_hint("Try \"%s --help\" for more information.", progname);
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
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
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

	if (data_only && schema_only)
		pg_fatal("options -s/--schema-only and -a/--data-only cannot be used together");
	if (schema_only && statistics_only)
		pg_fatal("options -s/--schema-only and --statistics-only cannot be used together");
	if (data_only && statistics_only)
		pg_fatal("options -a/--data-only and --statistics-only cannot be used together");

	if (data_only && no_data)
		pg_fatal("options -a/--data-only and --no-data cannot be used together");
	if (schema_only && no_schema)
		pg_fatal("options -s/--schema-only and --no-schema cannot be used together");
	if (statistics_only && no_statistics)
		pg_fatal("options --statistics-only and --no-statistics cannot be used together");

	if (schema_only && foreign_servers_include_patterns.head != NULL)
		pg_fatal("options -s/--schema-only and --include-foreign-data cannot be used together");

	if (numWorkers > 1 && foreign_servers_include_patterns.head != NULL)
		pg_fatal("option --include-foreign-data is not supported with parallel backup");

	if (data_only && dopt.outputClean)
		pg_fatal("options -c/--clean and -a/--data-only cannot be used together");

	if (dopt.if_exists && !dopt.outputClean)
		pg_fatal("option --if-exists requires option -c/--clean");

	/* set derivative flags */
	dopt.dumpData = data_only || (!schema_only && !statistics_only && !no_data);
	dopt.dumpSchema = schema_only || (!data_only && !statistics_only && !no_schema);
	dopt.dumpStatistics = statistics_only || (!data_only && !schema_only && !no_statistics);

	/*
	 * --inserts are already implied above if --column-inserts or
	 * --rows-per-insert were specified.
	 */
	if (dopt.do_nothing && dopt.dump_inserts == 0)
		pg_fatal("option --on-conflict-do-nothing requires option --inserts, --rows-per-insert, or --column-inserts");

	/* Identify archive format to emit */
	archiveFormat = parseArchiveFormat(format, &archiveMode);

	/* archiveFormat specific setup */
	if (archiveFormat == archNull)
		plainText = 1;

	/*
	 * Custom and directory formats are compressed by default with gzip when
	 * available, not the others.  If gzip is not available, no compression is
	 * done by default.
	 */
	if ((archiveFormat == archCustom || archiveFormat == archDirectory) &&
		!user_compression_defined)
	{
#ifdef HAVE_LIBZ
		compression_algorithm_str = "gzip";
#else
		compression_algorithm_str = "none";
#endif
	}

	/*
	 * Compression options
	 */
	if (!parse_compress_algorithm(compression_algorithm_str,
								  &compression_algorithm))
		pg_fatal("unrecognized compression algorithm: \"%s\"",
				 compression_algorithm_str);

	parse_compress_specification(compression_algorithm, compression_detail,
								 &compression_spec);
	error_detail = validate_compress_specification(&compression_spec);
	if (error_detail != NULL)
		pg_fatal("invalid compression specification: %s",
				 error_detail);

	error_detail = supports_compression(compression_spec);
	if (error_detail != NULL)
		pg_fatal("%s", error_detail);

	/*
	 * Disable support for zstd workers for now - these are based on
	 * threading, and it's unclear how it interacts with parallel dumps on
	 * platforms where that relies on threads too (e.g. Windows).
	 */
	if (compression_spec.options & PG_COMPRESSION_OPTION_WORKERS)
		pg_log_warning("compression option \"%s\" is not currently supported by pg_dump",
					   "workers");

	/*
	 * If emitting an archive format, we always want to emit a DATABASE item,
	 * in case --create is specified at pg_restore time.
	 */
	if (!plainText)
		dopt.outputCreateDB = 1;

	/* Parallel backup only in the directory archive format so far */
	if (archiveFormat != archDirectory && numWorkers > 1)
		pg_fatal("parallel backup only supported by the directory format");

	/* Open the output file */
	fout = CreateArchive(filename, archiveFormat, compression_spec,
						 dosync, archiveMode, setupDumpWorker, sync_method);

	/* Make dump options accessible right away */
	SetArchiveOptions(fout, &dopt, NULL);

	/* Register the cleanup hook */
	on_exit_close_archive(fout);

	/* Let the archiver know how noisy to be */
	fout->verbose = g_verbose;


	/*
	 * We allow the server to be back to 9.2, and up to any minor release of
	 * our own major version.  (See also version check in pg_dumpall.c.)
	 */
	fout->minRemoteVersion = 90200;
	fout->maxRemoteVersion = (PG_VERSION_NUM / 100) * 100 + 99;

	fout->numWorkers = numWorkers;

	/*
	 * Open the database using the Archiver, so it knows about it. Errors mean
	 * death.
	 */
	ConnectDatabase(fout, &dopt.cparams, false);
	setup_connection(fout, dumpencoding, dumpsnapshot, use_role);

	/*
	 * On hot standbys, never try to dump unlogged table data, since it will
	 * just throw an error.
	 */
	if (fout->isStandby)
		dopt.no_unlogged_table_data = true;

	/*
	 * Find the last built-in OID, if needed (prior to 8.1)
	 *
	 * With 8.1 and above, we can just use FirstNormalObjectId - 1.
	 */
	g_last_builtin_oid = FirstNormalObjectId - 1;

	pg_log_info("last built-in OID is %u", g_last_builtin_oid);

	/* Expand schema selection patterns into OID lists */
	if (schema_include_patterns.head != NULL)
	{
		expand_schema_name_patterns(fout, &schema_include_patterns,
									&schema_include_oids,
									strict_names);
		if (schema_include_oids.head == NULL)
			pg_fatal("no matching schemas were found");
	}
	expand_schema_name_patterns(fout, &schema_exclude_patterns,
								&schema_exclude_oids,
								false);
	/* non-matching exclusion patterns aren't an error */

	/* Expand table selection patterns into OID lists */
	expand_table_name_patterns(fout, &table_include_patterns,
							   &table_include_oids,
							   strict_names, false);
	expand_table_name_patterns(fout, &table_include_patterns_and_children,
							   &table_include_oids,
							   strict_names, true);
	if ((table_include_patterns.head != NULL ||
		 table_include_patterns_and_children.head != NULL) &&
		table_include_oids.head == NULL)
		pg_fatal("no matching tables were found");

	expand_table_name_patterns(fout, &table_exclude_patterns,
							   &table_exclude_oids,
							   false, false);
	expand_table_name_patterns(fout, &table_exclude_patterns_and_children,
							   &table_exclude_oids,
							   false, true);

	expand_table_name_patterns(fout, &tabledata_exclude_patterns,
							   &tabledata_exclude_oids,
							   false, false);
	expand_table_name_patterns(fout, &tabledata_exclude_patterns_and_children,
							   &tabledata_exclude_oids,
							   false, true);

	expand_foreign_server_name_patterns(fout, &foreign_servers_include_patterns,
										&foreign_servers_include_oids);

	/* non-matching exclusion patterns aren't an error */

	/* Expand extension selection patterns into OID lists */
	if (extension_include_patterns.head != NULL)
	{
		expand_extension_name_patterns(fout, &extension_include_patterns,
									   &extension_include_oids,
									   strict_names);
		if (extension_include_oids.head == NULL)
			pg_fatal("no matching extensions were found");
	}
	expand_extension_name_patterns(fout, &extension_exclude_patterns,
								   &extension_exclude_oids,
								   false);
	/* non-matching exclusion patterns aren't an error */

	/*
	 * Dumping LOs is the default for dumps where an inclusion switch is not
	 * used (an "include everything" dump).  -B can be used to exclude LOs
	 * from those dumps.  -b can be used to include LOs even when an inclusion
	 * switch is used.
	 *
	 * -s means "schema only" and LOs are data, not schema, so we never
	 * include LOs when -s is used.
	 */
	if (dopt.include_everything && dopt.dumpData && !dopt.dontOutputLOs)
		dopt.outputLOs = true;

	/*
	 * Collect role names so we can map object owner OIDs to names.
	 */
	collectRoleNames(fout);

	/*
	 * Now scan the database and create DumpableObject structs for all the
	 * objects we intend to dump.
	 */
	tblinfo = getSchemaData(fout, &numTables);

	if (dopt.dumpData)
	{
		getTableData(&dopt, tblinfo, numTables, 0);
		buildMatViewRefreshDependencies(fout);
		if (!dopt.dumpSchema)
			getTableDataFKConstraints();
	}

	if (!dopt.dumpData && dopt.sequence_data)
		getTableData(&dopt, tblinfo, numTables, RELKIND_SEQUENCE);

	/*
	 * In binary-upgrade mode, we do not have to worry about the actual LO
	 * data or the associated metadata that resides in the pg_largeobject and
	 * pg_largeobject_metadata tables, respectively.
	 *
	 * However, we do need to collect LO information as there may be comments
	 * or other information on LOs that we do need to dump out.
	 */
	if (dopt.outputLOs || dopt.binary_upgrade)
		getLOs(fout);

	/*
	 * Collect dependency data to assist in ordering the objects.
	 */
	getDependencies(fout);

	/*
	 * Collect ACLs, comments, and security labels, if wanted.
	 */
	if (!dopt.aclsSkip)
		getAdditionalACLs(fout);
	if (!dopt.no_comments)
		collectComments(fout);
	if (!dopt.no_security_labels)
		collectSecLabels(fout);

	/* For binary upgrade mode, collect required pg_class information. */
	if (dopt.binary_upgrade)
		collectBinaryUpgradeClassOids(fout);

	/* Collect sequence information. */
	collectSequences(fout);

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

	/*
	 * First the special entries for ENCODING, STDSTRINGS, and SEARCHPATH.
	 */
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
	ropt->dumpData = dopt.dumpData;
	ropt->dumpSchema = dopt.dumpSchema;
	ropt->dumpStatistics = dopt.dumpStatistics;
	ropt->if_exists = dopt.if_exists;
	ropt->column_inserts = dopt.column_inserts;
	ropt->dumpSections = dopt.dumpSections;
	ropt->aclsSkip = dopt.aclsSkip;
	ropt->superuser = dopt.outputSuperuser;
	ropt->createDB = dopt.outputCreateDB;
	ropt->noOwner = dopt.outputNoOwner;
	ropt->noTableAm = dopt.outputNoTableAm;
	ropt->noTablespace = dopt.outputNoTablespaces;
	ropt->disable_triggers = dopt.disable_triggers;
	ropt->use_setsessauth = dopt.use_setsessauth;
	ropt->disable_dollar_quoting = dopt.disable_dollar_quoting;
	ropt->dump_inserts = dopt.dump_inserts;
	ropt->no_comments = dopt.no_comments;
	ropt->no_policies = dopt.no_policies;
	ropt->no_publications = dopt.no_publications;
	ropt->no_security_labels = dopt.no_security_labels;
	ropt->no_subscriptions = dopt.no_subscriptions;
	ropt->lockWaitTimeout = dopt.lockWaitTimeout;
	ropt->include_everything = dopt.include_everything;
	ropt->enable_row_security = dopt.enable_row_security;
	ropt->sequence_data = dopt.sequence_data;
	ropt->binary_upgrade = dopt.binary_upgrade;

	ropt->compression_spec = compression_spec;

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
	printf(_("  -Z, --compress=METHOD[:DETAIL]\n"
			 "                               compress as specified\n"));
	printf(_("  --lock-wait-timeout=TIMEOUT  fail after waiting TIMEOUT for a table lock\n"));
	printf(_("  --no-sync                    do not wait for changes to be written safely to disk\n"));
	printf(_("  --sync-method=METHOD         set method for syncing files to disk\n"));
	printf(_("  -?, --help                   show this help, then exit\n"));

	printf(_("\nOptions controlling the output content:\n"));
	printf(_("  -a, --data-only              dump only the data, not the schema or statistics\n"));
	printf(_("  -b, --large-objects          include large objects in dump\n"));
	printf(_("  --blobs                      (same as --large-objects, deprecated)\n"));
	printf(_("  -B, --no-large-objects       exclude large objects in dump\n"));
	printf(_("  --no-blobs                   (same as --no-large-objects, deprecated)\n"));
	printf(_("  -c, --clean                  clean (drop) database objects before recreating\n"));
	printf(_("  -C, --create                 include commands to create database in dump\n"));
	printf(_("  -e, --extension=PATTERN      dump the specified extension(s) only\n"));
	printf(_("  -E, --encoding=ENCODING      dump the data in encoding ENCODING\n"));
	printf(_("  -n, --schema=PATTERN         dump the specified schema(s) only\n"));
	printf(_("  -N, --exclude-schema=PATTERN do NOT dump the specified schema(s)\n"));
	printf(_("  -O, --no-owner               skip restoration of object ownership in\n"
			 "                               plain-text format\n"));
	printf(_("  -s, --schema-only            dump only the schema, no data or statistics\n"));
	printf(_("  -S, --superuser=NAME         superuser user name to use in plain-text format\n"));
	printf(_("  -t, --table=PATTERN          dump only the specified table(s)\n"));
	printf(_("  -T, --exclude-table=PATTERN  do NOT dump the specified table(s)\n"));
	printf(_("  -x, --no-privileges          do not dump privileges (grant/revoke)\n"));
	printf(_("  --binary-upgrade             for use by upgrade utilities only\n"));
	printf(_("  --column-inserts             dump data as INSERT commands with column names\n"));
	printf(_("  --disable-dollar-quoting     disable dollar quoting, use SQL standard quoting\n"));
	printf(_("  --disable-triggers           disable triggers during data-only restore\n"));
	printf(_("  --enable-row-security        enable row security (dump only content user has\n"
			 "                               access to)\n"));
	printf(_("  --exclude-extension=PATTERN  do NOT dump the specified extension(s)\n"));
	printf(_("  --exclude-table-and-children=PATTERN\n"
			 "                               do NOT dump the specified table(s), including\n"
			 "                               child and partition tables\n"));
	printf(_("  --exclude-table-data=PATTERN do NOT dump data for the specified table(s)\n"));
	printf(_("  --exclude-table-data-and-children=PATTERN\n"
			 "                               do NOT dump data for the specified table(s),\n"
			 "                               including child and partition tables\n"));
	printf(_("  --extra-float-digits=NUM     override default setting for extra_float_digits\n"));
	printf(_("  --filter=FILENAME            include or exclude objects and data from dump\n"
			 "                               based on expressions in FILENAME\n"));
	printf(_("  --if-exists                  use IF EXISTS when dropping objects\n"));
	printf(_("  --include-foreign-data=PATTERN\n"
			 "                               include data of foreign tables on foreign\n"
			 "                               servers matching PATTERN\n"));
	printf(_("  --inserts                    dump data as INSERT commands, rather than COPY\n"));
	printf(_("  --load-via-partition-root    load partitions via the root table\n"));
	printf(_("  --no-comments                do not dump comment commands\n"));
	printf(_("  --no-data                    do not dump data\n"));
	printf(_("  --no-policies                do not dump row security policies\n"));
	printf(_("  --no-publications            do not dump publications\n"));
	printf(_("  --no-schema                  do not dump schema\n"));
	printf(_("  --no-security-labels         do not dump security label assignments\n"));
	printf(_("  --no-statistics              do not dump statistics\n"));
	printf(_("  --no-subscriptions           do not dump subscriptions\n"));
	printf(_("  --no-table-access-method     do not dump table access methods\n"));
	printf(_("  --no-tablespaces             do not dump tablespace assignments\n"));
	printf(_("  --no-toast-compression       do not dump TOAST compression methods\n"));
	printf(_("  --no-unlogged-table-data     do not dump unlogged table data\n"));
	printf(_("  --on-conflict-do-nothing     add ON CONFLICT DO NOTHING to INSERT commands\n"));
	printf(_("  --quote-all-identifiers      quote all identifiers, even if not key words\n"));
	printf(_("  --rows-per-insert=NROWS      number of rows per INSERT; implies --inserts\n"));
	printf(_("  --section=SECTION            dump named section (pre-data, data, or post-data)\n"));
	printf(_("  --serializable-deferrable    wait until the dump can run without anomalies\n"));
	printf(_("  --snapshot=SNAPSHOT          use given snapshot for the dump\n"));
	printf(_("  --statistics-only            dump only the statistics, not schema or data\n"));
	printf(_("  --strict-names               require table and/or schema include patterns to\n"
			 "                               match at least one entity each\n"));
	printf(_("  --table-and-children=PATTERN dump only the specified table(s), including\n"
			 "                               child and partition tables\n"));
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
			pg_fatal("invalid client encoding \"%s\" specified",
					 dumpencoding);
	}

	/*
	 * Get the active encoding and the standard_conforming_strings setting, so
	 * we know how to escape strings.
	 */
	AH->encoding = PQclientEncoding(conn);
	setFmtEncoding(AH->encoding);

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
	if (use_role)
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
	else
		ExecuteSqlStatement(AH, "SET extra_float_digits TO 3");

	/*
	 * Disable synchronized scanning, to prevent unpredictable changes in row
	 * ordering across a dump and reload.
	 */
	ExecuteSqlStatement(AH, "SET synchronize_seqscans TO off");

	/*
	 * Disable timeouts if supported.
	 */
	ExecuteSqlStatement(AH, "SET statement_timeout = 0");
	if (AH->remoteVersion >= 90300)
		ExecuteSqlStatement(AH, "SET lock_timeout = 0");
	if (AH->remoteVersion >= 90600)
		ExecuteSqlStatement(AH, "SET idle_in_transaction_session_timeout = 0");
	if (AH->remoteVersion >= 170000)
		ExecuteSqlStatement(AH, "SET transaction_timeout = 0");

	/*
	 * Quote all identifiers, if requested.
	 */
	if (quote_all_identifiers)
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
	 * For security reasons, we restrict the expansion of non-system views and
	 * access to foreign tables during the pg_dump process. This restriction
	 * is adjusted when dumping foreign table data.
	 */
	set_restrict_relation_kind(AH, "view, foreign-table");

	/*
	 * Initialize prepared-query state to "nothing prepared".  We do this here
	 * so that a parallel dump worker will have its own state.
	 */
	AH->is_prepared = (bool *) pg_malloc0(NUM_PREP_QUERIES * sizeof(bool));

	/*
	 * Start transaction-snapshot mode transaction to dump consistent data.
	 */
	ExecuteSqlStatement(AH, "BEGIN");

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
	else if (AH->numWorkers > 1)
	{
		if (AH->isStandby && AH->remoteVersion < 100000)
			pg_fatal("parallel dumps from standby servers are not supported by this server version");
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
		pg_fatal("invalid output format \"%s\" specified", format);
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
		PQExpBufferData dbbuf;
		int			dotcnt;

		appendPQExpBufferStr(query,
							 "SELECT oid FROM pg_catalog.pg_namespace n\n");
		initPQExpBuffer(&dbbuf);
		processSQLNamePattern(GetConnection(fout), query, cell->val, false,
							  false, NULL, "n.nspname", NULL, NULL, &dbbuf,
							  &dotcnt);
		if (dotcnt > 1)
			pg_fatal("improper qualified name (too many dotted names): %s",
					 cell->val);
		else if (dotcnt == 1)
			prohibit_crossdb_refs(GetConnection(fout), dbbuf.data, cell->val);
		termPQExpBuffer(&dbbuf);

		res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);
		if (strict_names && PQntuples(res) == 0)
			pg_fatal("no matching schemas were found for pattern \"%s\"", cell->val);

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
 * Find the OIDs of all extensions matching the given list of patterns,
 * and append them to the given OID list.
 */
static void
expand_extension_name_patterns(Archive *fout,
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
		int			dotcnt;

		appendPQExpBufferStr(query,
							 "SELECT oid FROM pg_catalog.pg_extension e\n");
		processSQLNamePattern(GetConnection(fout), query, cell->val, false,
							  false, NULL, "e.extname", NULL, NULL, NULL,
							  &dotcnt);
		if (dotcnt > 0)
			pg_fatal("improper qualified name (too many dotted names): %s",
					 cell->val);

		res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);
		if (strict_names && PQntuples(res) == 0)
			pg_fatal("no matching extensions were found for pattern \"%s\"", cell->val);

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
		int			dotcnt;

		appendPQExpBufferStr(query,
							 "SELECT oid FROM pg_catalog.pg_foreign_server s\n");
		processSQLNamePattern(GetConnection(fout), query, cell->val, false,
							  false, NULL, "s.srvname", NULL, NULL, NULL,
							  &dotcnt);
		if (dotcnt > 0)
			pg_fatal("improper qualified name (too many dotted names): %s",
					 cell->val);

		res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);
		if (PQntuples(res) == 0)
			pg_fatal("no matching foreign servers were found for pattern \"%s\"", cell->val);

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
						   bool strict_names, bool with_child_tables)
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
		PQExpBufferData dbbuf;
		int			dotcnt;

		/*
		 * Query must remain ABSOLUTELY devoid of unqualified names.  This
		 * would be unnecessary given a pg_table_is_visible() variant taking a
		 * search_path argument.
		 *
		 * For with_child_tables, we start with the basic query's results and
		 * recursively search the inheritance tree to add child tables.
		 */
		if (with_child_tables)
		{
			appendPQExpBuffer(query, "WITH RECURSIVE partition_tree (relid) AS (\n");
		}

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
		initPQExpBuffer(&dbbuf);
		processSQLNamePattern(GetConnection(fout), query, cell->val, true,
							  false, "n.nspname", "c.relname", NULL,
							  "pg_catalog.pg_table_is_visible(c.oid)", &dbbuf,
							  &dotcnt);
		if (dotcnt > 2)
			pg_fatal("improper relation name (too many dotted names): %s",
					 cell->val);
		else if (dotcnt == 2)
			prohibit_crossdb_refs(GetConnection(fout), dbbuf.data, cell->val);
		termPQExpBuffer(&dbbuf);

		if (with_child_tables)
		{
			appendPQExpBuffer(query, "UNION"
							  "\nSELECT i.inhrelid"
							  "\nFROM partition_tree p"
							  "\n     JOIN pg_catalog.pg_inherits i"
							  "\n     ON p.relid OPERATOR(pg_catalog.=) i.inhparent"
							  "\n)"
							  "\nSELECT relid FROM partition_tree");
		}

		ExecuteSqlStatement(fout, "RESET search_path");
		res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);
		PQclear(ExecuteSqlQueryForSingleRow(fout,
											ALWAYS_SECURE_SEARCH_PATH_SQL));
		if (strict_names && PQntuples(res) == 0)
			pg_fatal("no matching tables were found for pattern \"%s\"", cell->val);

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
 * Verifies that the connected database name matches the given database name,
 * and if not, dies with an error about the given pattern.
 *
 * The 'dbname' argument should be a literal name parsed from 'pattern'.
 */
static void
prohibit_crossdb_refs(PGconn *conn, const char *dbname, const char *pattern)
{
	const char *db;

	db = PQdb(conn);
	if (db == NULL)
		pg_fatal("You are currently not connected to a database.");

	if (strcmp(db, dbname) != 0)
		pg_fatal("cross-database references are not implemented: %s",
				 pattern);
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
	 * In 9.6 and above, mark the member object to have any non-initial ACLs
	 * dumped.  (Any initial ACLs will be removed later, using data from
	 * pg_init_privs, so that we'll dump only the delta from the extension's
	 * initial setup.)
	 *
	 * Prior to 9.6, we do not include any extension member components.
	 *
	 * In binary upgrades, we still dump all components of the members
	 * individually, since the idea is to exactly reproduce the database
	 * contents rather than replace the extension contents with something
	 * different.
	 *
	 * Note: it might be interesting someday to implement storage and delta
	 * dumping of extension members' RLS policies and/or security labels.
	 * However there is a pitfall for RLS policies: trying to dump them
	 * requires getting a lock on their tables, and the calling user might not
	 * have privileges for that.  We need no lock to examine a table's ACLs,
	 * so the current feature doesn't have a problem of that sort.
	 */
	if (fout->dopt->binary_upgrade)
		dobj->dump = ext->dobj.dump;
	else
	{
		if (fout->remoteVersion < 90600)
			dobj->dump = DUMP_COMPONENT_NONE;
		else
			dobj->dump = ext->dobj.dump_contains & (DUMP_COMPONENT_ACL);
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
	 * DUMP_COMPONENT_DEFINITION typically implies a CREATE SCHEMA statement
	 * and (for --clean) a DROP SCHEMA statement.  (In the absence of
	 * DUMP_COMPONENT_DEFINITION, this value is irrelevant.)
	 */
	nsinfo->create = true;

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
		 * no-mans-land between being a system object and a user object.
		 * CREATE SCHEMA would fail, so its DUMP_COMPONENT_DEFINITION is just
		 * a comment and an indication of ownership.  If the owner is the
		 * default, omit that superfluous DUMP_COMPONENT_DEFINITION.  Before
		 * v15, the default owner was BOOTSTRAP_SUPERUSERID.
		 */
		nsinfo->create = false;
		nsinfo->dobj.dump = DUMP_COMPONENT_ALL;
		if (nsinfo->nspowner == ROLE_PG_DATABASE_OWNER)
			nsinfo->dobj.dump &= ~DUMP_COMPONENT_DEFINITION;
		nsinfo->dobj.dump_contains = DUMP_COMPONENT_ALL;

		/*
		 * Also, make like it has a comment even if it doesn't; this is so
		 * that we'll emit a command to drop the comment, if appropriate.
		 * (Without this, we'd not call dumpCommentExtended for it.)
		 */
		nsinfo->dobj.components |= DUMP_COMPONENT_COMMENT;
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

	/* skip auto-generated array and multirange types */
	if (tyinfo->isArray || tyinfo->isMultirange)
	{
		tyinfo->dobj.objType = DO_DUMMY_TYPE;

		/*
		 * Fall through to set the dump flag; we assume that the subsequent
		 * rules will do the same thing as they would for the array's base
		 * type or multirange's range type.  (We cannot reliably look up the
		 * base type here, since getTypes may not have processed it yet.)
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
 * Otherwise dump if we are dumping "everything".  Note that dumpSchema
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
 * We dump all user-added extensions by default.  No extensions are dumped
 * if include_everything is false (i.e., a --schema or --table switch was
 * given), except if --extension specifies a list of extensions to dump.
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
	{
		/* check if there is a list of extensions to dump */
		if (extension_include_oids.head != NULL)
			extinfo->dobj.dump = extinfo->dobj.dump_contains =
				simple_oid_list_member(&extension_include_oids,
									   extinfo->dobj.catId.oid) ?
				DUMP_COMPONENT_ALL : DUMP_COMPONENT_NONE;
		else
			extinfo->dobj.dump = extinfo->dobj.dump_contains =
				dopt->include_everything ?
				DUMP_COMPONENT_ALL : DUMP_COMPONENT_NONE;

		/* check that the extension is not explicitly excluded */
		if (extinfo->dobj.dump &&
			simple_oid_list_member(&extension_exclude_oids,
								   extinfo->dobj.catId.oid))
			extinfo->dobj.dump = extinfo->dobj.dump_contains = DUMP_COMPONENT_NONE;
	}
}

/*
 * selectDumpablePublicationObject: policy-setting subroutine
 *		Mark a publication object as to be dumped or not
 *
 * A publication can have schemas and tables which have schemas, but those are
 * ignored in decision making, because publications are only dumped when we are
 * dumping everything.
 */
static void
selectDumpablePublicationObject(DumpableObject *dobj, Archive *fout)
{
	if (checkExtensionMembership(dobj, fout))
		return;					/* extension membership overrides all else */

	dobj->dump = fout->dopt->include_everything ?
		DUMP_COMPONENT_ALL : DUMP_COMPONENT_NONE;
}

/*
 * selectDumpableStatisticsObject: policy-setting subroutine
 *		Mark an extended statistics object as to be dumped or not
 *
 * We dump an extended statistics object if the schema it's in and the table
 * it's for are being dumped.  (This'll need more thought if statistics
 * objects ever support cross-table stats.)
 */
static void
selectDumpableStatisticsObject(StatsExtInfo *sobj, Archive *fout)
{
	if (checkExtensionMembership(&sobj->dobj, fout))
		return;					/* extension membership overrides all else */

	sobj->dobj.dump = sobj->dobj.namespace->dobj.dump_contains;
	if (sobj->stattable == NULL ||
		!(sobj->stattable->dobj.dump & DUMP_COMPONENT_DEFINITION))
		sobj->dobj.dump = DUMP_COMPONENT_NONE;
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
dumpTableData_copy(Archive *fout, const void *dcontext)
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
		/* Temporary allows to access to foreign tables to dump data */
		if (tbinfo->relkind == RELKIND_FOREIGN_TABLE)
			set_restrict_relation_kind(fout, "view");

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
		pg_log_error_detail("Error message from server: %s", PQerrorMessage(conn));
		pg_log_error_detail("Command was: %s", q->data);
		exit_nicely(1);
	}

	/* Check command status and return to normal libpq state */
	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		pg_log_error("Dumping the contents of table \"%s\" failed: PQgetResult() failed.", classname);
		pg_log_error_detail("Error message from server: %s", PQerrorMessage(conn));
		pg_log_error_detail("Command was: %s", q->data);
		exit_nicely(1);
	}
	PQclear(res);

	/* Do this to ensure we've pumped libpq back to idle state */
	if (PQgetResult(conn) != NULL)
		pg_log_warning("unexpected extra results during COPY of table \"%s\"",
					   classname);

	destroyPQExpBuffer(q);

	/* Revert back the setting */
	if (tbinfo->relkind == RELKIND_FOREIGN_TABLE)
		set_restrict_relation_kind(fout, "view, foreign-table");

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
dumpTableData_insert(Archive *fout, const void *dcontext)
{
	TableDataInfo *tdinfo = (TableDataInfo *) dcontext;
	TableInfo  *tbinfo = tdinfo->tdtable;
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer insertStmt = NULL;
	char	   *attgenerated;
	PGresult   *res;
	int			nfields,
				i;
	int			rows_per_statement = dopt->dump_inserts;
	int			rows_this_statement = 0;

	/* Temporary allows to access to foreign tables to dump data */
	if (tbinfo->relkind == RELKIND_FOREIGN_TABLE)
		set_restrict_relation_kind(fout, "view");

	/*
	 * If we're going to emit INSERTs with column names, the most efficient
	 * way to deal with generated columns is to exclude them entirely.  For
	 * INSERTs without column names, we have to emit DEFAULT rather than the
	 * actual column value --- but we can save a few cycles by fetching nulls
	 * rather than the uninteresting-to-us value.
	 */
	attgenerated = (char *) pg_malloc(tbinfo->numatts * sizeof(char));
	appendPQExpBufferStr(q, "DECLARE _pg_dump_cursor CURSOR FOR SELECT ");
	nfields = 0;
	for (i = 0; i < tbinfo->numatts; i++)
	{
		if (tbinfo->attisdropped[i])
			continue;
		if (tbinfo->attgenerated[i] && dopt->column_inserts)
			continue;
		if (nfields > 0)
			appendPQExpBufferStr(q, ", ");
		if (tbinfo->attgenerated[i])
			appendPQExpBufferStr(q, "NULL");
		else
			appendPQExpBufferStr(q, fmtId(tbinfo->attnames[i]));
		attgenerated[nfields] = tbinfo->attgenerated[i];
		nfields++;
	}
	/* Servers before 9.4 will complain about zero-column SELECT */
	if (nfields == 0)
		appendPQExpBufferStr(q, "NULL");
	appendPQExpBuffer(q, " FROM ONLY %s",
					  fmtQualifiedDumpable(tbinfo));
	if (tdinfo->filtercond)
		appendPQExpBuffer(q, " %s", tdinfo->filtercond);

	ExecuteSqlStatement(fout, q->data);

	while (1)
	{
		res = ExecuteSqlQuery(fout, "FETCH 100 FROM _pg_dump_cursor",
							  PGRES_TUPLES_OK);

		/* cross-check field count, allowing for dummy NULL if any */
		if (nfields != PQnfields(res) &&
			!(nfields == 0 && PQnfields(res) == 1))
			pg_fatal("wrong number of fields retrieved from table \"%s\"",
					 tbinfo->dobj.name);

		/*
		 * First time through, we build as much of the INSERT statement as
		 * possible in "insertStmt", which we can then just print for each
		 * statement. If the table happens to have zero dumpable columns then
		 * this will be a complete statement, otherwise it will end in
		 * "VALUES" and be ready to have the row's column values printed.
		 */
		if (insertStmt == NULL)
		{
			TableInfo  *targettab;

			insertStmt = createPQExpBuffer();

			/*
			 * When load-via-partition-root is set or forced, get the root
			 * table name for the partition table, so that we can reload data
			 * through the root table.
			 */
			if (tbinfo->ispartition &&
				(dopt->load_via_partition_root ||
				 forcePartitionRootLoad(tbinfo)))
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
				if (attgenerated[field])
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
	free(attgenerated);

	/* Revert back the setting */
	if (tbinfo->relkind == RELKIND_FOREIGN_TABLE)
		set_restrict_relation_kind(fout, "view, foreign-table");

	return 1;
}

/*
 * getRootTableInfo:
 *     get the root TableInfo for the given partition table.
 */
static TableInfo *
getRootTableInfo(const TableInfo *tbinfo)
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
 * forcePartitionRootLoad
 *     Check if we must force load_via_partition_root for this partition.
 *
 * This is required if any level of ancestral partitioned table has an
 * unsafe partitioning scheme.
 */
static bool
forcePartitionRootLoad(const TableInfo *tbinfo)
{
	TableInfo  *parentTbinfo;

	Assert(tbinfo->ispartition);
	Assert(tbinfo->numParents == 1);

	parentTbinfo = tbinfo->parents[0];
	if (parentTbinfo->unsafe_partitions)
		return true;
	while (parentTbinfo->ispartition)
	{
		Assert(parentTbinfo->numParents == 1);
		parentTbinfo = parentTbinfo->parents[0];
		if (parentTbinfo->unsafe_partitions)
			return true;
	}

	return false;
}

/*
 * dumpTableData -
 *	  dump the contents of a single table
 *
 * Actually, this just makes an ArchiveEntry for the table contents.
 */
static void
dumpTableData(Archive *fout, const TableDataInfo *tdinfo)
{
	DumpOptions *dopt = fout->dopt;
	TableInfo  *tbinfo = tdinfo->tdtable;
	PQExpBuffer copyBuf = createPQExpBuffer();
	PQExpBuffer clistBuf = createPQExpBuffer();
	DataDumperPtr dumpFn;
	char	   *tdDefn = NULL;
	char	   *copyStmt;
	const char *copyFrom;

	/* We had better have loaded per-column details about this table */
	Assert(tbinfo->interesting);

	/*
	 * When load-via-partition-root is set or forced, get the root table name
	 * for the partition table, so that we can reload data through the root
	 * table.  Then construct a comment to be inserted into the TOC entry's
	 * defn field, so that such cases can be identified reliably.
	 */
	if (tbinfo->ispartition &&
		(dopt->load_via_partition_root ||
		 forcePartitionRootLoad(tbinfo)))
	{
		TableInfo  *parentTbinfo;

		parentTbinfo = getRootTableInfo(tbinfo);
		copyFrom = fmtQualifiedDumpable(parentTbinfo);
		printfPQExpBuffer(copyBuf, "-- load via partition root %s",
						  copyFrom);
		tdDefn = pg_strdup(copyBuf->data);
	}
	else
		copyFrom = fmtQualifiedDumpable(tbinfo);

	if (dopt->dump_inserts == 0)
	{
		/* Dump/restore using COPY */
		dumpFn = dumpTableData_copy;
		/* must use 2 steps here 'cause fmtId is nonreentrant */
		printfPQExpBuffer(copyBuf, "COPY %s ",
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
									   .createStmt = tdDefn,
									   .copyStmt = copyStmt,
									   .deps = &(tbinfo->dobj.dumpId),
									   .nDeps = 1,
									   .dumpFn = dumpFn,
									   .dumpArg = tdinfo));

		/*
		 * Set the TocEntry's dataLength in case we are doing a parallel dump
		 * and want to order dump jobs by table size.  We choose to measure
		 * dataLength in table pages (including TOAST pages) during dump, so
		 * no scaling is needed.
		 *
		 * However, relpages is declared as "integer" in pg_class, and hence
		 * also in TableInfo, but it's really BlockNumber a/k/a unsigned int.
		 * Cast so that we get the right interpretation of table sizes
		 * exceeding INT_MAX pages.
		 */
		te->dataLength = (BlockNumber) tbinfo->relpages;
		te->dataLength += (BlockNumber) tbinfo->toastpages;

		/*
		 * If pgoff_t is only 32 bits wide, the above refinement is useless,
		 * and instead we'd better worry about integer overflow.  Clamp to
		 * INT_MAX if the correct result exceeds that.
		 */
		if (sizeof(te->dataLength) == 4 &&
			(tbinfo->relpages < 0 || tbinfo->toastpages < 0 ||
			 te->dataLength < 0))
			te->dataLength = INT_MAX;
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
refreshMatViewData(Archive *fout, const TableDataInfo *tdinfo)
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
 * table data; the "dump" field in such objects isn't very interesting.
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

	/* A TableDataInfo contains data, of course */
	tdinfo->dobj.components |= DUMP_COMPONENT_DATA;

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
				i_datdba,
				i_encoding,
				i_datlocprovider,
				i_collate,
				i_ctype,
				i_datlocale,
				i_daticurules,
				i_frozenxid,
				i_minmxid,
				i_datacl,
				i_acldefault,
				i_datistemplate,
				i_datconnlimit,
				i_datcollversion,
				i_tablespace;
	CatalogId	dbCatId;
	DumpId		dbDumpId;
	DumpableAcl dbdacl;
	const char *datname,
			   *dba,
			   *encoding,
			   *datlocprovider,
			   *collate,
			   *ctype,
			   *locale,
			   *icurules,
			   *datistemplate,
			   *datconnlimit,
			   *tablespace;
	uint32		frozenxid,
				minmxid;
	char	   *qdatname;

	pg_log_info("saving database definition");

	/*
	 * Fetch the database-level properties for this database.
	 */
	appendPQExpBufferStr(dbQry, "SELECT tableoid, oid, datname, "
						 "datdba, "
						 "pg_encoding_to_char(encoding) AS encoding, "
						 "datcollate, datctype, datfrozenxid, "
						 "datacl, acldefault('d', datdba) AS acldefault, "
						 "datistemplate, datconnlimit, ");
	if (fout->remoteVersion >= 90300)
		appendPQExpBufferStr(dbQry, "datminmxid, ");
	else
		appendPQExpBufferStr(dbQry, "0 AS datminmxid, ");
	if (fout->remoteVersion >= 170000)
		appendPQExpBufferStr(dbQry, "datlocprovider, datlocale, datcollversion, ");
	else if (fout->remoteVersion >= 150000)
		appendPQExpBufferStr(dbQry, "datlocprovider, daticulocale AS datlocale, datcollversion, ");
	else
		appendPQExpBufferStr(dbQry, "'c' AS datlocprovider, NULL AS datlocale, NULL AS datcollversion, ");
	if (fout->remoteVersion >= 160000)
		appendPQExpBufferStr(dbQry, "daticurules, ");
	else
		appendPQExpBufferStr(dbQry, "NULL AS daticurules, ");
	appendPQExpBufferStr(dbQry,
						 "(SELECT spcname FROM pg_tablespace t WHERE t.oid = dattablespace) AS tablespace, "
						 "shobj_description(oid, 'pg_database') AS description "
						 "FROM pg_database "
						 "WHERE datname = current_database()");

	res = ExecuteSqlQueryForSingleRow(fout, dbQry->data);

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_datname = PQfnumber(res, "datname");
	i_datdba = PQfnumber(res, "datdba");
	i_encoding = PQfnumber(res, "encoding");
	i_datlocprovider = PQfnumber(res, "datlocprovider");
	i_collate = PQfnumber(res, "datcollate");
	i_ctype = PQfnumber(res, "datctype");
	i_datlocale = PQfnumber(res, "datlocale");
	i_daticurules = PQfnumber(res, "daticurules");
	i_frozenxid = PQfnumber(res, "datfrozenxid");
	i_minmxid = PQfnumber(res, "datminmxid");
	i_datacl = PQfnumber(res, "datacl");
	i_acldefault = PQfnumber(res, "acldefault");
	i_datistemplate = PQfnumber(res, "datistemplate");
	i_datconnlimit = PQfnumber(res, "datconnlimit");
	i_datcollversion = PQfnumber(res, "datcollversion");
	i_tablespace = PQfnumber(res, "tablespace");

	dbCatId.tableoid = atooid(PQgetvalue(res, 0, i_tableoid));
	dbCatId.oid = atooid(PQgetvalue(res, 0, i_oid));
	datname = PQgetvalue(res, 0, i_datname);
	dba = getRoleName(PQgetvalue(res, 0, i_datdba));
	encoding = PQgetvalue(res, 0, i_encoding);
	datlocprovider = PQgetvalue(res, 0, i_datlocprovider);
	collate = PQgetvalue(res, 0, i_collate);
	ctype = PQgetvalue(res, 0, i_ctype);
	if (!PQgetisnull(res, 0, i_datlocale))
		locale = PQgetvalue(res, 0, i_datlocale);
	else
		locale = NULL;
	if (!PQgetisnull(res, 0, i_daticurules))
		icurules = PQgetvalue(res, 0, i_daticurules);
	else
		icurules = NULL;
	frozenxid = atooid(PQgetvalue(res, 0, i_frozenxid));
	minmxid = atooid(PQgetvalue(res, 0, i_minmxid));
	dbdacl.acl = PQgetvalue(res, 0, i_datacl);
	dbdacl.acldefault = PQgetvalue(res, 0, i_acldefault);
	datistemplate = PQgetvalue(res, 0, i_datistemplate);
	datconnlimit = PQgetvalue(res, 0, i_datconnlimit);
	tablespace = PQgetvalue(res, 0, i_tablespace);

	qdatname = pg_strdup(fmtId(datname));

	/*
	 * Prepare the CREATE DATABASE command.  We must specify OID (if we want
	 * to preserve that), as well as the encoding, locale, and tablespace
	 * since those can't be altered later.  Other DB properties are left to
	 * the DATABASE PROPERTIES entry, so that they can be applied after
	 * reconnecting to the target DB.
	 *
	 * For binary upgrade, we use the FILE_COPY strategy because testing has
	 * shown it to be faster.  When the server is in binary upgrade mode, it
	 * will also skip the checkpoints this strategy ordinarily performs.
	 */
	if (dopt->binary_upgrade)
	{
		appendPQExpBuffer(creaQry,
						  "CREATE DATABASE %s WITH TEMPLATE = template0 "
						  "OID = %u STRATEGY = FILE_COPY",
						  qdatname, dbCatId.oid);
	}
	else
	{
		appendPQExpBuffer(creaQry, "CREATE DATABASE %s WITH TEMPLATE = template0",
						  qdatname);
	}
	if (strlen(encoding) > 0)
	{
		appendPQExpBufferStr(creaQry, " ENCODING = ");
		appendStringLiteralAH(creaQry, encoding, fout);
	}

	appendPQExpBufferStr(creaQry, " LOCALE_PROVIDER = ");
	if (datlocprovider[0] == 'b')
		appendPQExpBufferStr(creaQry, "builtin");
	else if (datlocprovider[0] == 'c')
		appendPQExpBufferStr(creaQry, "libc");
	else if (datlocprovider[0] == 'i')
		appendPQExpBufferStr(creaQry, "icu");
	else
		pg_fatal("unrecognized locale provider: %s",
				 datlocprovider);

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
	if (locale)
	{
		if (datlocprovider[0] == 'b')
			appendPQExpBufferStr(creaQry, " BUILTIN_LOCALE = ");
		else
			appendPQExpBufferStr(creaQry, " ICU_LOCALE = ");

		appendStringLiteralAH(creaQry, locale, fout);
	}

	if (icurules)
	{
		appendPQExpBufferStr(creaQry, " ICU_RULES = ");
		appendStringLiteralAH(creaQry, icurules, fout);
	}

	/*
	 * For binary upgrade, carry over the collation version.  For normal
	 * dump/restore, omit the version, so that it is computed upon restore.
	 */
	if (dopt->binary_upgrade)
	{
		if (!PQgetisnull(res, 0, i_datcollversion))
		{
			appendPQExpBufferStr(creaQry, " COLLATION_VERSION = ");
			appendStringLiteralAH(creaQry,
								  PQgetvalue(res, 0, i_datcollversion),
								  fout);
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

	/* Dump DB security label, if enabled */
	if (!dopt->no_security_labels)
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
	dbdacl.privtype = 0;
	dbdacl.initprivs = NULL;

	dumpACL(fout, dbDumpId, InvalidDumpId, "DATABASE",
			qdatname, NULL, NULL,
			NULL, dba, &dbdacl);

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

	/*
	 * We do not restore pg_database.dathasloginevt because it is set
	 * automatically on login event trigger creation.
	 */

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
	 * relfrozenxids, relminmxids and relfilenode.
	 */
	if (dopt->binary_upgrade)
	{
		PGresult   *lo_res;
		PQExpBuffer loFrozenQry = createPQExpBuffer();
		PQExpBuffer loOutQry = createPQExpBuffer();
		PQExpBuffer loHorizonQry = createPQExpBuffer();
		int			ii_relfrozenxid,
					ii_relfilenode,
					ii_oid,
					ii_relminmxid;

		/*
		 * pg_largeobject
		 */
		if (fout->remoteVersion >= 90300)
			appendPQExpBuffer(loFrozenQry, "SELECT relfrozenxid, relminmxid, relfilenode, oid\n"
							  "FROM pg_catalog.pg_class\n"
							  "WHERE oid IN (%u, %u);\n",
							  LargeObjectRelationId, LargeObjectLOidPNIndexId);
		else
			appendPQExpBuffer(loFrozenQry, "SELECT relfrozenxid, 0 AS relminmxid, relfilenode, oid\n"
							  "FROM pg_catalog.pg_class\n"
							  "WHERE oid IN (%u, %u);\n",
							  LargeObjectRelationId, LargeObjectLOidPNIndexId);

		lo_res = ExecuteSqlQuery(fout, loFrozenQry->data, PGRES_TUPLES_OK);

		ii_relfrozenxid = PQfnumber(lo_res, "relfrozenxid");
		ii_relminmxid = PQfnumber(lo_res, "relminmxid");
		ii_relfilenode = PQfnumber(lo_res, "relfilenode");
		ii_oid = PQfnumber(lo_res, "oid");

		appendPQExpBufferStr(loHorizonQry, "\n-- For binary upgrade, set pg_largeobject relfrozenxid and relminmxid\n");
		appendPQExpBufferStr(loOutQry, "\n-- For binary upgrade, preserve pg_largeobject and index relfilenodes\n");
		for (int i = 0; i < PQntuples(lo_res); ++i)
		{
			Oid			oid;
			RelFileNumber relfilenumber;

			appendPQExpBuffer(loHorizonQry, "UPDATE pg_catalog.pg_class\n"
							  "SET relfrozenxid = '%u', relminmxid = '%u'\n"
							  "WHERE oid = %u;\n",
							  atooid(PQgetvalue(lo_res, i, ii_relfrozenxid)),
							  atooid(PQgetvalue(lo_res, i, ii_relminmxid)),
							  atooid(PQgetvalue(lo_res, i, ii_oid)));

			oid = atooid(PQgetvalue(lo_res, i, ii_oid));
			relfilenumber = atooid(PQgetvalue(lo_res, i, ii_relfilenode));

			if (oid == LargeObjectRelationId)
				appendPQExpBuffer(loOutQry,
								  "SELECT pg_catalog.binary_upgrade_set_next_heap_relfilenode('%u'::pg_catalog.oid);\n",
								  relfilenumber);
			else if (oid == LargeObjectLOidPNIndexId)
				appendPQExpBuffer(loOutQry,
								  "SELECT pg_catalog.binary_upgrade_set_next_index_relfilenode('%u'::pg_catalog.oid);\n",
								  relfilenumber);
		}

		appendPQExpBufferStr(loOutQry,
							 "TRUNCATE pg_catalog.pg_largeobject;\n");
		appendPQExpBufferStr(loOutQry, loHorizonQry->data);

		ArchiveEntry(fout, nilCatalogId, createDumpId(),
					 ARCHIVE_OPTS(.tag = "pg_largeobject",
								  .description = "pg_largeobject",
								  .section = SECTION_PRE_DATA,
								  .createStmt = loOutQry->data));

		PQclear(lo_res);

		destroyPQExpBuffer(loFrozenQry);
		destroyPQExpBuffer(loHorizonQry);
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

	/* First collect database-specific options */
	printfPQExpBuffer(buf, "SELECT unnest(setconfig) FROM pg_db_role_setting "
					  "WHERE setrole = 0 AND setdatabase = '%u'::oid",
					  dboid);

	res = ExecuteSqlQuery(AH, buf->data, PGRES_TUPLES_OK);

	for (int i = 0; i < PQntuples(res); i++)
		makeAlterConfigCommand(conn, PQgetvalue(res, i, 0),
							   "DATABASE", dbname, NULL, NULL,
							   outbuf);

	PQclear(res);

	/* Now look for role-and-database-specific options */
	printfPQExpBuffer(buf, "SELECT rolname, unnest(setconfig) "
					  "FROM pg_db_role_setting s, pg_roles r "
					  "WHERE setrole = r.oid AND setdatabase = '%u'::oid",
					  dboid);

	res = ExecuteSqlQuery(AH, buf->data, PGRES_TUPLES_OK);

	for (int i = 0; i < PQntuples(res); i++)
		makeAlterConfigCommand(conn, PQgetvalue(res, i, 1),
							   "ROLE", PQgetvalue(res, i, 0),
							   "DATABASE", dbname,
							   outbuf);

	PQclear(res);

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

	pg_log_info("saving \"standard_conforming_strings = %s\"",
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
		pg_fatal("could not parse result of current_schemas()");

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

	pg_log_info("saving \"search_path = %s\"", path->data);

	ArchiveEntry(AH, nilCatalogId, createDumpId(),
				 ARCHIVE_OPTS(.tag = "SEARCHPATH",
							  .description = "SEARCHPATH",
							  .section = SECTION_PRE_DATA,
							  .createStmt = qry->data));

	/* Also save it in AH->searchpath, in case we're doing plain text dump */
	AH->searchpath = pg_strdup(qry->data);

	free(schemanames);
	PQclear(res);
	destroyPQExpBuffer(qry);
	destroyPQExpBuffer(path);
}


/*
 * getLOs:
 *	Collect schema-level data about large objects
 */
static void
getLOs(Archive *fout)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer loQry = createPQExpBuffer();
	PGresult   *res;
	int			ntups;
	int			i;
	int			n;
	int			i_oid;
	int			i_lomowner;
	int			i_lomacl;
	int			i_acldefault;

	pg_log_info("reading large objects");

	/*
	 * Fetch LO OIDs and owner/ACL data.  Order the data so that all the blobs
	 * with the same owner/ACL appear together.
	 */
	appendPQExpBufferStr(loQry,
						 "SELECT oid, lomowner, lomacl, "
						 "acldefault('L', lomowner) AS acldefault "
						 "FROM pg_largeobject_metadata "
						 "ORDER BY lomowner, lomacl::pg_catalog.text, oid");

	res = ExecuteSqlQuery(fout, loQry->data, PGRES_TUPLES_OK);

	i_oid = PQfnumber(res, "oid");
	i_lomowner = PQfnumber(res, "lomowner");
	i_lomacl = PQfnumber(res, "lomacl");
	i_acldefault = PQfnumber(res, "acldefault");

	ntups = PQntuples(res);

	/*
	 * Group the blobs into suitably-sized groups that have the same owner and
	 * ACL setting, and build a metadata and a data DumpableObject for each
	 * group.  (If we supported initprivs for blobs, we'd have to insist that
	 * groups also share initprivs settings, since the DumpableObject only has
	 * room for one.)  i is the index of the first tuple in the current group,
	 * and n is the number of tuples we include in the group.
	 */
	for (i = 0; i < ntups; i += n)
	{
		Oid			thisoid = atooid(PQgetvalue(res, i, i_oid));
		char	   *thisowner = PQgetvalue(res, i, i_lomowner);
		char	   *thisacl = PQgetvalue(res, i, i_lomacl);
		LoInfo	   *loinfo;
		DumpableObject *lodata;
		char		namebuf[64];

		/* Scan to find first tuple not to be included in group */
		n = 1;
		while (n < MAX_BLOBS_PER_ARCHIVE_ENTRY && i + n < ntups)
		{
			if (strcmp(thisowner, PQgetvalue(res, i + n, i_lomowner)) != 0 ||
				strcmp(thisacl, PQgetvalue(res, i + n, i_lomacl)) != 0)
				break;
			n++;
		}

		/* Build the metadata DumpableObject */
		loinfo = (LoInfo *) pg_malloc(offsetof(LoInfo, looids) + n * sizeof(Oid));

		loinfo->dobj.objType = DO_LARGE_OBJECT;
		loinfo->dobj.catId.tableoid = LargeObjectRelationId;
		loinfo->dobj.catId.oid = thisoid;
		AssignDumpId(&loinfo->dobj);

		if (n > 1)
			snprintf(namebuf, sizeof(namebuf), "%u..%u", thisoid,
					 atooid(PQgetvalue(res, i + n - 1, i_oid)));
		else
			snprintf(namebuf, sizeof(namebuf), "%u", thisoid);
		loinfo->dobj.name = pg_strdup(namebuf);
		loinfo->dacl.acl = pg_strdup(thisacl);
		loinfo->dacl.acldefault = pg_strdup(PQgetvalue(res, i, i_acldefault));
		loinfo->dacl.privtype = 0;
		loinfo->dacl.initprivs = NULL;
		loinfo->rolname = getRoleName(thisowner);
		loinfo->numlos = n;
		loinfo->looids[0] = thisoid;
		/* Collect OIDs of the remaining blobs in this group */
		for (int k = 1; k < n; k++)
		{
			CatalogId	extraID;

			loinfo->looids[k] = atooid(PQgetvalue(res, i + k, i_oid));

			/* Make sure we can look up loinfo by any of the blobs' OIDs */
			extraID.tableoid = LargeObjectRelationId;
			extraID.oid = loinfo->looids[k];
			recordAdditionalCatalogID(extraID, &loinfo->dobj);
		}

		/* LOs have data */
		loinfo->dobj.components |= DUMP_COMPONENT_DATA;

		/* Mark whether LO group has a non-empty ACL */
		if (!PQgetisnull(res, i, i_lomacl))
			loinfo->dobj.components |= DUMP_COMPONENT_ACL;

		/*
		 * In binary-upgrade mode for LOs, we do *not* dump out the LO data,
		 * as it will be copied by pg_upgrade, which simply copies the
		 * pg_largeobject table. We *do* however dump out anything but the
		 * data, as pg_upgrade copies just pg_largeobject, but not
		 * pg_largeobject_metadata, after the dump is restored.
		 */
		if (dopt->binary_upgrade)
			loinfo->dobj.dump &= ~DUMP_COMPONENT_DATA;

		/*
		 * Create a "BLOBS" data item for the group, too. This is just a
		 * placeholder for sorting; it carries no data now.
		 */
		lodata = (DumpableObject *) pg_malloc(sizeof(DumpableObject));
		lodata->objType = DO_LARGE_OBJECT_DATA;
		lodata->catId = nilCatalogId;
		AssignDumpId(lodata);
		lodata->name = pg_strdup(namebuf);
		lodata->components |= DUMP_COMPONENT_DATA;
		/* Set up explicit dependency from data to metadata */
		lodata->dependencies = (DumpId *) pg_malloc(sizeof(DumpId));
		lodata->dependencies[0] = loinfo->dobj.dumpId;
		lodata->nDeps = lodata->allocDeps = 1;
	}

	PQclear(res);
	destroyPQExpBuffer(loQry);
}

/*
 * dumpLO
 *
 * dump the definition (metadata) of the given large object group
 */
static void
dumpLO(Archive *fout, const LoInfo *loinfo)
{
	PQExpBuffer cquery = createPQExpBuffer();

	/*
	 * The "definition" is just a newline-separated list of OIDs.  We need to
	 * put something into the dropStmt too, but it can just be a comment.
	 */
	for (int i = 0; i < loinfo->numlos; i++)
		appendPQExpBuffer(cquery, "%u\n", loinfo->looids[i]);

	if (loinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, loinfo->dobj.catId, loinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = loinfo->dobj.name,
								  .owner = loinfo->rolname,
								  .description = "BLOB METADATA",
								  .section = SECTION_DATA,
								  .createStmt = cquery->data,
								  .dropStmt = "-- dummy"));

	/*
	 * Dump per-blob comments and seclabels if any.  We assume these are rare
	 * enough that it's okay to generate retail TOC entries for them.
	 */
	if (loinfo->dobj.dump & (DUMP_COMPONENT_COMMENT |
							 DUMP_COMPONENT_SECLABEL))
	{
		for (int i = 0; i < loinfo->numlos; i++)
		{
			CatalogId	catId;
			char		namebuf[32];

			/* Build identifying info for this blob */
			catId.tableoid = loinfo->dobj.catId.tableoid;
			catId.oid = loinfo->looids[i];
			snprintf(namebuf, sizeof(namebuf), "%u", loinfo->looids[i]);

			if (loinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
				dumpComment(fout, "LARGE OBJECT", namebuf,
							NULL, loinfo->rolname,
							catId, 0, loinfo->dobj.dumpId);

			if (loinfo->dobj.dump & DUMP_COMPONENT_SECLABEL)
				dumpSecLabel(fout, "LARGE OBJECT", namebuf,
							 NULL, loinfo->rolname,
							 catId, 0, loinfo->dobj.dumpId);
		}
	}

	/*
	 * Dump the ACLs if any (remember that all blobs in the group will have
	 * the same ACL).  If there's just one blob, dump a simple ACL entry; if
	 * there's more, make a "LARGE OBJECTS" entry that really contains only
	 * the ACL for the first blob.  _printTocEntry() will be cued by the tag
	 * string to emit a mutated version for each blob.
	 */
	if (loinfo->dobj.dump & DUMP_COMPONENT_ACL)
	{
		char		namebuf[32];

		/* Build identifying info for the first blob */
		snprintf(namebuf, sizeof(namebuf), "%u", loinfo->looids[0]);

		if (loinfo->numlos > 1)
		{
			char		tagbuf[64];

			snprintf(tagbuf, sizeof(tagbuf), "LARGE OBJECTS %u..%u",
					 loinfo->looids[0], loinfo->looids[loinfo->numlos - 1]);

			dumpACL(fout, loinfo->dobj.dumpId, InvalidDumpId,
					"LARGE OBJECT", namebuf, NULL, NULL,
					tagbuf, loinfo->rolname, &loinfo->dacl);
		}
		else
		{
			dumpACL(fout, loinfo->dobj.dumpId, InvalidDumpId,
					"LARGE OBJECT", namebuf, NULL, NULL,
					NULL, loinfo->rolname, &loinfo->dacl);
		}
	}

	destroyPQExpBuffer(cquery);
}

/*
 * dumpLOs:
 *	dump the data contents of the large objects in the given group
 */
static int
dumpLOs(Archive *fout, const void *arg)
{
	const LoInfo *loinfo = (const LoInfo *) arg;
	PGconn	   *conn = GetConnection(fout);
	char		buf[LOBBUFSIZE];

	pg_log_info("saving large objects \"%s\"", loinfo->dobj.name);

	for (int i = 0; i < loinfo->numlos; i++)
	{
		Oid			loOid = loinfo->looids[i];
		int			loFd;
		int			cnt;

		/* Open the LO */
		loFd = lo_open(conn, loOid, INV_READ);
		if (loFd == -1)
			pg_fatal("could not open large object %u: %s",
					 loOid, PQerrorMessage(conn));

		StartLO(fout, loOid);

		/* Now read it in chunks, sending data to archive */
		do
		{
			cnt = lo_read(conn, loFd, buf, LOBBUFSIZE);
			if (cnt < 0)
				pg_fatal("error reading large object %u: %s",
						 loOid, PQerrorMessage(conn));

			WriteData(fout, buf, cnt);
		} while (cnt > 0);

		lo_close(conn, loFd);

		EndLO(fout, loOid);
	}

	return 1;
}

/*
 * getPolicies
 *	  get information about all RLS policies on dumpable tables.
 */
void
getPolicies(Archive *fout, TableInfo tblinfo[], int numTables)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer query;
	PQExpBuffer tbloids;
	PGresult   *res;
	PolicyInfo *polinfo;
	int			i_oid;
	int			i_tableoid;
	int			i_polrelid;
	int			i_polname;
	int			i_polcmd;
	int			i_polpermissive;
	int			i_polroles;
	int			i_polqual;
	int			i_polwithcheck;
	int			i,
				j,
				ntups;

	/* No policies before 9.5 */
	if (fout->remoteVersion < 90500)
		return;

	/* Skip if --no-policies was specified */
	if (dopt->no_policies)
		return;

	query = createPQExpBuffer();
	tbloids = createPQExpBuffer();

	/*
	 * Identify tables of interest, and check which ones have RLS enabled.
	 */
	appendPQExpBufferChar(tbloids, '{');
	for (i = 0; i < numTables; i++)
	{
		TableInfo  *tbinfo = &tblinfo[i];

		/* Ignore row security on tables not to be dumped */
		if (!(tbinfo->dobj.dump & DUMP_COMPONENT_POLICY))
			continue;

		/* It can't have RLS or policies if it's not a table */
		if (tbinfo->relkind != RELKIND_RELATION &&
			tbinfo->relkind != RELKIND_PARTITIONED_TABLE)
			continue;

		/* Add it to the list of table OIDs to be probed below */
		if (tbloids->len > 1)	/* do we have more than the '{'? */
			appendPQExpBufferChar(tbloids, ',');
		appendPQExpBuffer(tbloids, "%u", tbinfo->dobj.catId.oid);

		/* Is RLS enabled?  (That's separate from whether it has policies) */
		if (tbinfo->rowsec)
		{
			tbinfo->dobj.components |= DUMP_COMPONENT_POLICY;

			/*
			 * We represent RLS being enabled on a table by creating a
			 * PolicyInfo object with null polname.
			 *
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
	}
	appendPQExpBufferChar(tbloids, '}');

	/*
	 * Now, read all RLS policies belonging to the tables of interest, and
	 * create PolicyInfo objects for them.  (Note that we must filter the
	 * results server-side not locally, because we dare not apply pg_get_expr
	 * to tables we don't have lock on.)
	 */
	pg_log_info("reading row-level security policies");

	printfPQExpBuffer(query,
					  "SELECT pol.oid, pol.tableoid, pol.polrelid, pol.polname, pol.polcmd, ");
	if (fout->remoteVersion >= 100000)
		appendPQExpBufferStr(query, "pol.polpermissive, ");
	else
		appendPQExpBufferStr(query, "'t' as polpermissive, ");
	appendPQExpBuffer(query,
					  "CASE WHEN pol.polroles = '{0}' THEN NULL ELSE "
					  "   pg_catalog.array_to_string(ARRAY(SELECT pg_catalog.quote_ident(rolname) from pg_catalog.pg_roles WHERE oid = ANY(pol.polroles)), ', ') END AS polroles, "
					  "pg_catalog.pg_get_expr(pol.polqual, pol.polrelid) AS polqual, "
					  "pg_catalog.pg_get_expr(pol.polwithcheck, pol.polrelid) AS polwithcheck "
					  "FROM unnest('%s'::pg_catalog.oid[]) AS src(tbloid)\n"
					  "JOIN pg_catalog.pg_policy pol ON (src.tbloid = pol.polrelid)",
					  tbloids->data);

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	if (ntups > 0)
	{
		i_oid = PQfnumber(res, "oid");
		i_tableoid = PQfnumber(res, "tableoid");
		i_polrelid = PQfnumber(res, "polrelid");
		i_polname = PQfnumber(res, "polname");
		i_polcmd = PQfnumber(res, "polcmd");
		i_polpermissive = PQfnumber(res, "polpermissive");
		i_polroles = PQfnumber(res, "polroles");
		i_polqual = PQfnumber(res, "polqual");
		i_polwithcheck = PQfnumber(res, "polwithcheck");

		polinfo = pg_malloc(ntups * sizeof(PolicyInfo));

		for (j = 0; j < ntups; j++)
		{
			Oid			polrelid = atooid(PQgetvalue(res, j, i_polrelid));
			TableInfo  *tbinfo = findTableByOid(polrelid);

			tbinfo->dobj.components |= DUMP_COMPONENT_POLICY;

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
	}

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(tbloids);
}

/*
 * dumpPolicy
 *	  dump the definition of the given policy
 */
static void
dumpPolicy(Archive *fout, const PolicyInfo *polinfo)
{
	DumpOptions *dopt = fout->dopt;
	TableInfo  *tbinfo = polinfo->poltable;
	PQExpBuffer query;
	PQExpBuffer delqry;
	PQExpBuffer polprefix;
	char	   *qtabname;
	const char *cmd;
	char	   *tag;

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
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
		if (polinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
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
		pg_fatal("unexpected policy command type: %c",
				 polinfo->polcmd);

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

	if (polinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
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
	int			i_pubowner;
	int			i_puballtables;
	int			i_pubinsert;
	int			i_pubupdate;
	int			i_pubdelete;
	int			i_pubtruncate;
	int			i_pubviaroot;
	int			i_pubgencols;
	int			i,
				ntups;

	if (dopt->no_publications || fout->remoteVersion < 100000)
		return;

	query = createPQExpBuffer();

	/* Get the publications. */
	appendPQExpBufferStr(query, "SELECT p.tableoid, p.oid, p.pubname, "
						 "p.pubowner, p.puballtables, p.pubinsert, "
						 "p.pubupdate, p.pubdelete, ");

	if (fout->remoteVersion >= 110000)
		appendPQExpBufferStr(query, "p.pubtruncate, ");
	else
		appendPQExpBufferStr(query, "false AS pubtruncate, ");

	if (fout->remoteVersion >= 130000)
		appendPQExpBufferStr(query, "p.pubviaroot, ");
	else
		appendPQExpBufferStr(query, "false AS pubviaroot, ");

	if (fout->remoteVersion >= 180000)
		appendPQExpBufferStr(query, "p.pubgencols ");
	else
		appendPQExpBuffer(query, "'%c' AS pubgencols ", PUBLISH_GENCOLS_NONE);

	appendPQExpBufferStr(query, "FROM pg_publication p");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	if (ntups == 0)
		goto cleanup;

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_pubname = PQfnumber(res, "pubname");
	i_pubowner = PQfnumber(res, "pubowner");
	i_puballtables = PQfnumber(res, "puballtables");
	i_pubinsert = PQfnumber(res, "pubinsert");
	i_pubupdate = PQfnumber(res, "pubupdate");
	i_pubdelete = PQfnumber(res, "pubdelete");
	i_pubtruncate = PQfnumber(res, "pubtruncate");
	i_pubviaroot = PQfnumber(res, "pubviaroot");
	i_pubgencols = PQfnumber(res, "pubgencols");

	pubinfo = pg_malloc(ntups * sizeof(PublicationInfo));

	for (i = 0; i < ntups; i++)
	{
		pubinfo[i].dobj.objType = DO_PUBLICATION;
		pubinfo[i].dobj.catId.tableoid =
			atooid(PQgetvalue(res, i, i_tableoid));
		pubinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&pubinfo[i].dobj);
		pubinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_pubname));
		pubinfo[i].rolname = getRoleName(PQgetvalue(res, i, i_pubowner));
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
		pubinfo[i].pubgencols_type =
			*(PQgetvalue(res, i, i_pubgencols));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(pubinfo[i].dobj), fout);
	}

cleanup:
	PQclear(res);

	destroyPQExpBuffer(query);
}

/*
 * dumpPublication
 *	  dump the definition of the given publication
 */
static void
dumpPublication(Archive *fout, const PublicationInfo *pubinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer delq;
	PQExpBuffer query;
	char	   *qpubname;
	bool		first = true;

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
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

	appendPQExpBufferChar(query, '\'');

	if (pubinfo->pubviaroot)
		appendPQExpBufferStr(query, ", publish_via_partition_root = true");

	if (pubinfo->pubgencols_type == PUBLISH_GENCOLS_STORED)
		appendPQExpBufferStr(query, ", publish_generated_columns = stored");

	appendPQExpBufferStr(query, ");\n");

	if (pubinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
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
 * getPublicationNamespaces
 *	  get information about publication membership for dumpable schemas.
 */
void
getPublicationNamespaces(Archive *fout)
{
	PQExpBuffer query;
	PGresult   *res;
	PublicationSchemaInfo *pubsinfo;
	DumpOptions *dopt = fout->dopt;
	int			i_tableoid;
	int			i_oid;
	int			i_pnpubid;
	int			i_pnnspid;
	int			i,
				j,
				ntups;

	if (dopt->no_publications || fout->remoteVersion < 150000)
		return;

	query = createPQExpBuffer();

	/* Collect all publication membership info. */
	appendPQExpBufferStr(query,
						 "SELECT tableoid, oid, pnpubid, pnnspid "
						 "FROM pg_catalog.pg_publication_namespace");
	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_pnpubid = PQfnumber(res, "pnpubid");
	i_pnnspid = PQfnumber(res, "pnnspid");

	/* this allocation may be more than we need */
	pubsinfo = pg_malloc(ntups * sizeof(PublicationSchemaInfo));
	j = 0;

	for (i = 0; i < ntups; i++)
	{
		Oid			pnpubid = atooid(PQgetvalue(res, i, i_pnpubid));
		Oid			pnnspid = atooid(PQgetvalue(res, i, i_pnnspid));
		PublicationInfo *pubinfo;
		NamespaceInfo *nspinfo;

		/*
		 * Ignore any entries for which we aren't interested in either the
		 * publication or the rel.
		 */
		pubinfo = findPublicationByOid(pnpubid);
		if (pubinfo == NULL)
			continue;
		nspinfo = findNamespaceByOid(pnnspid);
		if (nspinfo == NULL)
			continue;

		/* OK, make a DumpableObject for this relationship */
		pubsinfo[j].dobj.objType = DO_PUBLICATION_TABLE_IN_SCHEMA;
		pubsinfo[j].dobj.catId.tableoid =
			atooid(PQgetvalue(res, i, i_tableoid));
		pubsinfo[j].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&pubsinfo[j].dobj);
		pubsinfo[j].dobj.namespace = nspinfo->dobj.namespace;
		pubsinfo[j].dobj.name = nspinfo->dobj.name;
		pubsinfo[j].publication = pubinfo;
		pubsinfo[j].pubschema = nspinfo;

		/* Decide whether we want to dump it */
		selectDumpablePublicationObject(&(pubsinfo[j].dobj), fout);

		j++;
	}

	PQclear(res);
	destroyPQExpBuffer(query);
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
	int			i_prpubid;
	int			i_prrelid;
	int			i_prrelqual;
	int			i_prattrs;
	int			i,
				j,
				ntups;

	if (dopt->no_publications || fout->remoteVersion < 100000)
		return;

	query = createPQExpBuffer();

	/* Collect all publication membership info. */
	if (fout->remoteVersion >= 150000)
		appendPQExpBufferStr(query,
							 "SELECT tableoid, oid, prpubid, prrelid, "
							 "pg_catalog.pg_get_expr(prqual, prrelid) AS prrelqual, "
							 "(CASE\n"
							 "  WHEN pr.prattrs IS NOT NULL THEN\n"
							 "    (SELECT array_agg(attname)\n"
							 "       FROM\n"
							 "         pg_catalog.generate_series(0, pg_catalog.array_upper(pr.prattrs::pg_catalog.int2[], 1)) s,\n"
							 "         pg_catalog.pg_attribute\n"
							 "      WHERE attrelid = pr.prrelid AND attnum = prattrs[s])\n"
							 "  ELSE NULL END) prattrs "
							 "FROM pg_catalog.pg_publication_rel pr");
	else
		appendPQExpBufferStr(query,
							 "SELECT tableoid, oid, prpubid, prrelid, "
							 "NULL AS prrelqual, NULL AS prattrs "
							 "FROM pg_catalog.pg_publication_rel");
	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_prpubid = PQfnumber(res, "prpubid");
	i_prrelid = PQfnumber(res, "prrelid");
	i_prrelqual = PQfnumber(res, "prrelqual");
	i_prattrs = PQfnumber(res, "prattrs");

	/* this allocation may be more than we need */
	pubrinfo = pg_malloc(ntups * sizeof(PublicationRelInfo));
	j = 0;

	for (i = 0; i < ntups; i++)
	{
		Oid			prpubid = atooid(PQgetvalue(res, i, i_prpubid));
		Oid			prrelid = atooid(PQgetvalue(res, i, i_prrelid));
		PublicationInfo *pubinfo;
		TableInfo  *tbinfo;

		/*
		 * Ignore any entries for which we aren't interested in either the
		 * publication or the rel.
		 */
		pubinfo = findPublicationByOid(prpubid);
		if (pubinfo == NULL)
			continue;
		tbinfo = findTableByOid(prrelid);
		if (tbinfo == NULL)
			continue;

		/* OK, make a DumpableObject for this relationship */
		pubrinfo[j].dobj.objType = DO_PUBLICATION_REL;
		pubrinfo[j].dobj.catId.tableoid =
			atooid(PQgetvalue(res, i, i_tableoid));
		pubrinfo[j].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&pubrinfo[j].dobj);
		pubrinfo[j].dobj.namespace = tbinfo->dobj.namespace;
		pubrinfo[j].dobj.name = tbinfo->dobj.name;
		pubrinfo[j].publication = pubinfo;
		pubrinfo[j].pubtable = tbinfo;
		if (PQgetisnull(res, i, i_prrelqual))
			pubrinfo[j].pubrelqual = NULL;
		else
			pubrinfo[j].pubrelqual = pg_strdup(PQgetvalue(res, i, i_prrelqual));

		if (!PQgetisnull(res, i, i_prattrs))
		{
			char	  **attnames;
			int			nattnames;
			PQExpBuffer attribs;

			if (!parsePGArray(PQgetvalue(res, i, i_prattrs),
							  &attnames, &nattnames))
				pg_fatal("could not parse %s array", "prattrs");
			attribs = createPQExpBuffer();
			for (int k = 0; k < nattnames; k++)
			{
				if (k > 0)
					appendPQExpBufferStr(attribs, ", ");

				appendPQExpBufferStr(attribs, fmtId(attnames[k]));
			}
			pubrinfo[j].pubrattrs = attribs->data;
			free(attribs);		/* but not attribs->data */
			free(attnames);
		}
		else
			pubrinfo[j].pubrattrs = NULL;

		/* Decide whether we want to dump it */
		selectDumpablePublicationObject(&(pubrinfo[j].dobj), fout);

		j++;
	}

	PQclear(res);
	destroyPQExpBuffer(query);
}

/*
 * dumpPublicationNamespace
 *	  dump the definition of the given publication schema mapping.
 */
static void
dumpPublicationNamespace(Archive *fout, const PublicationSchemaInfo *pubsinfo)
{
	DumpOptions *dopt = fout->dopt;
	NamespaceInfo *schemainfo = pubsinfo->pubschema;
	PublicationInfo *pubinfo = pubsinfo->publication;
	PQExpBuffer query;
	char	   *tag;

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
		return;

	tag = psprintf("%s %s", pubinfo->dobj.name, schemainfo->dobj.name);

	query = createPQExpBuffer();

	appendPQExpBuffer(query, "ALTER PUBLICATION %s ", fmtId(pubinfo->dobj.name));
	appendPQExpBuffer(query, "ADD TABLES IN SCHEMA %s;\n", fmtId(schemainfo->dobj.name));

	/*
	 * There is no point in creating drop query as the drop is done by schema
	 * drop.
	 */
	if (pubsinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, pubsinfo->dobj.catId, pubsinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tag,
								  .namespace = schemainfo->dobj.name,
								  .owner = pubinfo->rolname,
								  .description = "PUBLICATION TABLES IN SCHEMA",
								  .section = SECTION_POST_DATA,
								  .createStmt = query->data));

	/* These objects can't currently have comments or seclabels */

	free(tag);
	destroyPQExpBuffer(query);
}

/*
 * dumpPublicationTable
 *	  dump the definition of the given publication table mapping
 */
static void
dumpPublicationTable(Archive *fout, const PublicationRelInfo *pubrinfo)
{
	DumpOptions *dopt = fout->dopt;
	PublicationInfo *pubinfo = pubrinfo->publication;
	TableInfo  *tbinfo = pubrinfo->pubtable;
	PQExpBuffer query;
	char	   *tag;

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
		return;

	tag = psprintf("%s %s", pubinfo->dobj.name, tbinfo->dobj.name);

	query = createPQExpBuffer();

	appendPQExpBuffer(query, "ALTER PUBLICATION %s ADD TABLE ONLY",
					  fmtId(pubinfo->dobj.name));
	appendPQExpBuffer(query, " %s",
					  fmtQualifiedDumpable(tbinfo));

	if (pubrinfo->pubrattrs)
		appendPQExpBuffer(query, " (%s)", pubrinfo->pubrattrs);

	if (pubrinfo->pubrelqual)
	{
		/*
		 * It's necessary to add parentheses around the expression because
		 * pg_get_expr won't supply the parentheses for things like WHERE
		 * TRUE.
		 */
		appendPQExpBuffer(query, " WHERE (%s)", pubrinfo->pubrelqual);
	}
	appendPQExpBufferStr(query, ";\n");

	/*
	 * There is no point in creating a drop query as the drop is done by table
	 * drop.  (If you think to change this, see also _printTocEntry().)
	 * Although this object doesn't really have ownership as such, set the
	 * owner field anyway to ensure that the command is run by the correct
	 * role at restore time.
	 */
	if (pubrinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, pubrinfo->dobj.catId, pubrinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tag,
								  .namespace = tbinfo->dobj.namespace->dobj.name,
								  .owner = pubinfo->rolname,
								  .description = "PUBLICATION TABLE",
								  .section = SECTION_POST_DATA,
								  .createStmt = query->data));

	/* These objects can't currently have comments or seclabels */

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
 * Set the given value to restrict_nonsystem_relation_kind value. Since
 * restrict_nonsystem_relation_kind is introduced in minor version releases,
 * the setting query is effective only where available.
 */
static void
set_restrict_relation_kind(Archive *AH, const char *value)
{
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;

	appendPQExpBuffer(query,
					  "SELECT set_config(name, '%s', false) "
					  "FROM pg_settings "
					  "WHERE name = 'restrict_nonsystem_relation_kind'",
					  value);
	res = ExecuteSqlQuery(AH, query->data, PGRES_TUPLES_OK);

	PQclear(res);
	destroyPQExpBuffer(query);
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
	int			i_subowner;
	int			i_subbinary;
	int			i_substream;
	int			i_subtwophasestate;
	int			i_subdisableonerr;
	int			i_subpasswordrequired;
	int			i_subrunasowner;
	int			i_subconninfo;
	int			i_subslotname;
	int			i_subsynccommit;
	int			i_subpublications;
	int			i_suborigin;
	int			i_suboriginremotelsn;
	int			i_subenabled;
	int			i_subfailover;
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
	appendPQExpBufferStr(query,
						 "SELECT s.tableoid, s.oid, s.subname,\n"
						 " s.subowner,\n"
						 " s.subconninfo, s.subslotname, s.subsynccommit,\n"
						 " s.subpublications,\n");

	if (fout->remoteVersion >= 140000)
		appendPQExpBufferStr(query, " s.subbinary,\n");
	else
		appendPQExpBufferStr(query, " false AS subbinary,\n");

	if (fout->remoteVersion >= 140000)
		appendPQExpBufferStr(query, " s.substream,\n");
	else
		appendPQExpBufferStr(query, " 'f' AS substream,\n");

	if (fout->remoteVersion >= 150000)
		appendPQExpBufferStr(query,
							 " s.subtwophasestate,\n"
							 " s.subdisableonerr,\n");
	else
		appendPQExpBuffer(query,
						  " '%c' AS subtwophasestate,\n"
						  " false AS subdisableonerr,\n",
						  LOGICALREP_TWOPHASE_STATE_DISABLED);

	if (fout->remoteVersion >= 160000)
		appendPQExpBufferStr(query,
							 " s.subpasswordrequired,\n"
							 " s.subrunasowner,\n"
							 " s.suborigin,\n");
	else
		appendPQExpBuffer(query,
						  " 't' AS subpasswordrequired,\n"
						  " 't' AS subrunasowner,\n"
						  " '%s' AS suborigin,\n",
						  LOGICALREP_ORIGIN_ANY);

	if (dopt->binary_upgrade && fout->remoteVersion >= 170000)
		appendPQExpBufferStr(query, " o.remote_lsn AS suboriginremotelsn,\n"
							 " s.subenabled,\n");
	else
		appendPQExpBufferStr(query, " NULL AS suboriginremotelsn,\n"
							 " false AS subenabled,\n");

	if (fout->remoteVersion >= 170000)
		appendPQExpBufferStr(query,
							 " s.subfailover\n");
	else
		appendPQExpBuffer(query,
						  " false AS subfailover\n");

	appendPQExpBufferStr(query,
						 "FROM pg_subscription s\n");

	if (dopt->binary_upgrade && fout->remoteVersion >= 170000)
		appendPQExpBufferStr(query,
							 "LEFT JOIN pg_catalog.pg_replication_origin_status o \n"
							 "    ON o.external_id = 'pg_' || s.oid::text \n");

	appendPQExpBufferStr(query,
						 "WHERE s.subdbid = (SELECT oid FROM pg_database\n"
						 "                   WHERE datname = current_database())");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	/*
	 * Get subscription fields. We don't include subskiplsn in the dump as
	 * after restoring the dump this value may no longer be relevant.
	 */
	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_subname = PQfnumber(res, "subname");
	i_subowner = PQfnumber(res, "subowner");
	i_subenabled = PQfnumber(res, "subenabled");
	i_subbinary = PQfnumber(res, "subbinary");
	i_substream = PQfnumber(res, "substream");
	i_subtwophasestate = PQfnumber(res, "subtwophasestate");
	i_subdisableonerr = PQfnumber(res, "subdisableonerr");
	i_subpasswordrequired = PQfnumber(res, "subpasswordrequired");
	i_subrunasowner = PQfnumber(res, "subrunasowner");
	i_subfailover = PQfnumber(res, "subfailover");
	i_subconninfo = PQfnumber(res, "subconninfo");
	i_subslotname = PQfnumber(res, "subslotname");
	i_subsynccommit = PQfnumber(res, "subsynccommit");
	i_subpublications = PQfnumber(res, "subpublications");
	i_suborigin = PQfnumber(res, "suborigin");
	i_suboriginremotelsn = PQfnumber(res, "suboriginremotelsn");

	subinfo = pg_malloc(ntups * sizeof(SubscriptionInfo));

	for (i = 0; i < ntups; i++)
	{
		subinfo[i].dobj.objType = DO_SUBSCRIPTION;
		subinfo[i].dobj.catId.tableoid =
			atooid(PQgetvalue(res, i, i_tableoid));
		subinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&subinfo[i].dobj);
		subinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_subname));
		subinfo[i].rolname = getRoleName(PQgetvalue(res, i, i_subowner));

		subinfo[i].subenabled =
			(strcmp(PQgetvalue(res, i, i_subenabled), "t") == 0);
		subinfo[i].subbinary =
			(strcmp(PQgetvalue(res, i, i_subbinary), "t") == 0);
		subinfo[i].substream = *(PQgetvalue(res, i, i_substream));
		subinfo[i].subtwophasestate = *(PQgetvalue(res, i, i_subtwophasestate));
		subinfo[i].subdisableonerr =
			(strcmp(PQgetvalue(res, i, i_subdisableonerr), "t") == 0);
		subinfo[i].subpasswordrequired =
			(strcmp(PQgetvalue(res, i, i_subpasswordrequired), "t") == 0);
		subinfo[i].subrunasowner =
			(strcmp(PQgetvalue(res, i, i_subrunasowner), "t") == 0);
		subinfo[i].subfailover =
			(strcmp(PQgetvalue(res, i, i_subfailover), "t") == 0);
		subinfo[i].subconninfo =
			pg_strdup(PQgetvalue(res, i, i_subconninfo));
		if (PQgetisnull(res, i, i_subslotname))
			subinfo[i].subslotname = NULL;
		else
			subinfo[i].subslotname =
				pg_strdup(PQgetvalue(res, i, i_subslotname));
		subinfo[i].subsynccommit =
			pg_strdup(PQgetvalue(res, i, i_subsynccommit));
		subinfo[i].subpublications =
			pg_strdup(PQgetvalue(res, i, i_subpublications));
		subinfo[i].suborigin = pg_strdup(PQgetvalue(res, i, i_suborigin));
		if (PQgetisnull(res, i, i_suboriginremotelsn))
			subinfo[i].suboriginremotelsn = NULL;
		else
			subinfo[i].suboriginremotelsn =
				pg_strdup(PQgetvalue(res, i, i_suboriginremotelsn));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(subinfo[i].dobj), fout);
	}
	PQclear(res);

	destroyPQExpBuffer(query);
}

/*
 * getSubscriptionTables
 *	  Get information about subscription membership for dumpable tables. This
 *    will be used only in binary-upgrade mode for PG17 or later versions.
 */
void
getSubscriptionTables(Archive *fout)
{
	DumpOptions *dopt = fout->dopt;
	SubscriptionInfo *subinfo = NULL;
	SubRelInfo *subrinfo;
	PGresult   *res;
	int			i_srsubid;
	int			i_srrelid;
	int			i_srsubstate;
	int			i_srsublsn;
	int			ntups;
	Oid			last_srsubid = InvalidOid;

	if (dopt->no_subscriptions || !dopt->binary_upgrade ||
		fout->remoteVersion < 170000)
		return;

	res = ExecuteSqlQuery(fout,
						  "SELECT srsubid, srrelid, srsubstate, srsublsn "
						  "FROM pg_catalog.pg_subscription_rel "
						  "ORDER BY srsubid",
						  PGRES_TUPLES_OK);
	ntups = PQntuples(res);
	if (ntups == 0)
		goto cleanup;

	/* Get pg_subscription_rel attributes */
	i_srsubid = PQfnumber(res, "srsubid");
	i_srrelid = PQfnumber(res, "srrelid");
	i_srsubstate = PQfnumber(res, "srsubstate");
	i_srsublsn = PQfnumber(res, "srsublsn");

	subrinfo = pg_malloc(ntups * sizeof(SubRelInfo));
	for (int i = 0; i < ntups; i++)
	{
		Oid			cur_srsubid = atooid(PQgetvalue(res, i, i_srsubid));
		Oid			relid = atooid(PQgetvalue(res, i, i_srrelid));
		TableInfo  *tblinfo;

		/*
		 * If we switched to a new subscription, check if the subscription
		 * exists.
		 */
		if (cur_srsubid != last_srsubid)
		{
			subinfo = findSubscriptionByOid(cur_srsubid);
			if (subinfo == NULL)
				pg_fatal("subscription with OID %u does not exist", cur_srsubid);

			last_srsubid = cur_srsubid;
		}

		tblinfo = findTableByOid(relid);
		if (tblinfo == NULL)
			pg_fatal("failed sanity check, table with OID %u not found",
					 relid);

		/* OK, make a DumpableObject for this relationship */
		subrinfo[i].dobj.objType = DO_SUBSCRIPTION_REL;
		subrinfo[i].dobj.catId.tableoid = relid;
		subrinfo[i].dobj.catId.oid = cur_srsubid;
		AssignDumpId(&subrinfo[i].dobj);
		subrinfo[i].dobj.name = pg_strdup(subinfo->dobj.name);
		subrinfo[i].tblinfo = tblinfo;
		subrinfo[i].srsubstate = PQgetvalue(res, i, i_srsubstate)[0];
		if (PQgetisnull(res, i, i_srsublsn))
			subrinfo[i].srsublsn = NULL;
		else
			subrinfo[i].srsublsn = pg_strdup(PQgetvalue(res, i, i_srsublsn));

		subrinfo[i].subinfo = subinfo;

		/* Decide whether we want to dump it */
		selectDumpableObject(&(subrinfo[i].dobj), fout);
	}

cleanup:
	PQclear(res);
}

/*
 * dumpSubscriptionTable
 *	  Dump the definition of the given subscription table mapping. This will be
 *    used only in binary-upgrade mode for PG17 or later versions.
 */
static void
dumpSubscriptionTable(Archive *fout, const SubRelInfo *subrinfo)
{
	DumpOptions *dopt = fout->dopt;
	SubscriptionInfo *subinfo = subrinfo->subinfo;
	PQExpBuffer query;
	char	   *tag;

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
		return;

	Assert(fout->dopt->binary_upgrade && fout->remoteVersion >= 170000);

	tag = psprintf("%s %s", subinfo->dobj.name, subrinfo->dobj.name);

	query = createPQExpBuffer();

	if (subinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
	{
		/*
		 * binary_upgrade_add_sub_rel_state will add the subscription relation
		 * to pg_subscription_rel table. This will be used only in
		 * binary-upgrade mode.
		 */
		appendPQExpBufferStr(query,
							 "\n-- For binary upgrade, must preserve the subscriber table.\n");
		appendPQExpBufferStr(query,
							 "SELECT pg_catalog.binary_upgrade_add_sub_rel_state(");
		appendStringLiteralAH(query, subrinfo->dobj.name, fout);
		appendPQExpBuffer(query,
						  ", %u, '%c'",
						  subrinfo->tblinfo->dobj.catId.oid,
						  subrinfo->srsubstate);

		if (subrinfo->srsublsn && subrinfo->srsublsn[0] != '\0')
			appendPQExpBuffer(query, ", '%s'", subrinfo->srsublsn);
		else
			appendPQExpBuffer(query, ", NULL");

		appendPQExpBufferStr(query, ");\n");
	}

	/*
	 * There is no point in creating a drop query as the drop is done by table
	 * drop.  (If you think to change this, see also _printTocEntry().)
	 * Although this object doesn't really have ownership as such, set the
	 * owner field anyway to ensure that the command is run by the correct
	 * role at restore time.
	 */
	if (subrinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, subrinfo->dobj.catId, subrinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tag,
								  .namespace = subrinfo->tblinfo->dobj.namespace->dobj.name,
								  .owner = subinfo->rolname,
								  .description = "SUBSCRIPTION TABLE",
								  .section = SECTION_POST_DATA,
								  .createStmt = query->data));

	/* These objects can't currently have comments or seclabels */

	free(tag);
	destroyPQExpBuffer(query);
}

/*
 * dumpSubscription
 *	  dump the definition of the given subscription
 */
static void
dumpSubscription(Archive *fout, const SubscriptionInfo *subinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer delq;
	PQExpBuffer query;
	PQExpBuffer publications;
	char	   *qsubname;
	char	  **pubnames = NULL;
	int			npubnames = 0;
	int			i;

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
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
		pg_fatal("could not parse %s array", "subpublications");

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

	if (subinfo->subbinary)
		appendPQExpBufferStr(query, ", binary = true");

	if (subinfo->substream == LOGICALREP_STREAM_ON)
		appendPQExpBufferStr(query, ", streaming = on");
	else if (subinfo->substream == LOGICALREP_STREAM_PARALLEL)
		appendPQExpBufferStr(query, ", streaming = parallel");
	else
		appendPQExpBufferStr(query, ", streaming = off");

	if (subinfo->subtwophasestate != LOGICALREP_TWOPHASE_STATE_DISABLED)
		appendPQExpBufferStr(query, ", two_phase = on");

	if (subinfo->subdisableonerr)
		appendPQExpBufferStr(query, ", disable_on_error = true");

	if (!subinfo->subpasswordrequired)
		appendPQExpBuffer(query, ", password_required = false");

	if (subinfo->subrunasowner)
		appendPQExpBufferStr(query, ", run_as_owner = true");

	if (subinfo->subfailover)
		appendPQExpBufferStr(query, ", failover = true");

	if (strcmp(subinfo->subsynccommit, "off") != 0)
		appendPQExpBuffer(query, ", synchronous_commit = %s", fmtId(subinfo->subsynccommit));

	if (pg_strcasecmp(subinfo->suborigin, LOGICALREP_ORIGIN_ANY) != 0)
		appendPQExpBuffer(query, ", origin = %s", subinfo->suborigin);

	appendPQExpBufferStr(query, ");\n");

	/*
	 * In binary-upgrade mode, we allow the replication to continue after the
	 * upgrade.
	 */
	if (dopt->binary_upgrade && fout->remoteVersion >= 170000)
	{
		if (subinfo->suboriginremotelsn)
		{
			/*
			 * Preserve the remote_lsn for the subscriber's replication
			 * origin. This value is required to start the replication from
			 * the position before the upgrade. This value will be stale if
			 * the publisher gets upgraded before the subscriber node.
			 * However, this shouldn't be a problem as the upgrade of the
			 * publisher ensures that all the transactions were replicated
			 * before upgrading it.
			 */
			appendPQExpBufferStr(query,
								 "\n-- For binary upgrade, must preserve the remote_lsn for the subscriber's replication origin.\n");
			appendPQExpBufferStr(query,
								 "SELECT pg_catalog.binary_upgrade_replorigin_advance(");
			appendStringLiteralAH(query, subinfo->dobj.name, fout);
			appendPQExpBuffer(query, ", '%s');\n", subinfo->suboriginremotelsn);
		}

		if (subinfo->subenabled)
		{
			/*
			 * Enable the subscription to allow the replication to continue
			 * after the upgrade.
			 */
			appendPQExpBufferStr(query,
								 "\n-- For binary upgrade, must preserve the subscriber's running state.\n");
			appendPQExpBuffer(query, "ALTER SUBSCRIPTION %s ENABLE;\n", qsubname);
		}
	}

	if (subinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
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
							const DumpableObject *dobj,
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
			appendPQExpBuffer(create, "\nALTER %s %s DEPENDS ON EXTENSION %s;",
							  keyword, nm,
							  fmtId(PQgetvalue(res, i, i_extname)));
		}

		PQclear(res);
		destroyPQExpBuffer(query);
		pg_free(nm);
	}
}

static Oid
get_next_possible_free_pg_type_oid(Archive *fout, PQExpBuffer upgrade_query)
{
	/*
	 * If the old version didn't assign an array type, but the new version
	 * does, we must select an unused type OID to assign.  This currently only
	 * happens for domains, when upgrading pre-v11 to v11 and up.
	 *
	 * Note: local state here is kind of ugly, but we must have some, since we
	 * mustn't choose the same unused OID more than once.
	 */
	static Oid	next_possible_free_oid = FirstNormalObjectId;
	PGresult   *res;
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

	return next_possible_free_oid;
}

static void
binary_upgrade_set_type_oids_by_type_oid(Archive *fout,
										 PQExpBuffer upgrade_buffer,
										 Oid pg_type_oid,
										 bool force_array_type,
										 bool include_multirange_type)
{
	PQExpBuffer upgrade_query = createPQExpBuffer();
	PGresult   *res;
	Oid			pg_type_array_oid;
	Oid			pg_type_multirange_oid;
	Oid			pg_type_multirange_array_oid;
	TypeInfo   *tinfo;

	appendPQExpBufferStr(upgrade_buffer, "\n-- For binary upgrade, must preserve pg_type oid\n");
	appendPQExpBuffer(upgrade_buffer,
					  "SELECT pg_catalog.binary_upgrade_set_next_pg_type_oid('%u'::pg_catalog.oid);\n\n",
					  pg_type_oid);

	tinfo = findTypeByOid(pg_type_oid);
	if (tinfo)
		pg_type_array_oid = tinfo->typarray;
	else
		pg_type_array_oid = InvalidOid;

	if (!OidIsValid(pg_type_array_oid) && force_array_type)
		pg_type_array_oid = get_next_possible_free_pg_type_oid(fout, upgrade_query);

	if (OidIsValid(pg_type_array_oid))
	{
		appendPQExpBufferStr(upgrade_buffer,
							 "\n-- For binary upgrade, must preserve pg_type array oid\n");
		appendPQExpBuffer(upgrade_buffer,
						  "SELECT pg_catalog.binary_upgrade_set_next_array_pg_type_oid('%u'::pg_catalog.oid);\n\n",
						  pg_type_array_oid);
	}

	/*
	 * Pre-set the multirange type oid and its own array type oid.
	 */
	if (include_multirange_type)
	{
		if (fout->remoteVersion >= 140000)
		{
			printfPQExpBuffer(upgrade_query,
							  "SELECT t.oid, t.typarray "
							  "FROM pg_catalog.pg_type t "
							  "JOIN pg_catalog.pg_range r "
							  "ON t.oid = r.rngmultitypid "
							  "WHERE r.rngtypid = '%u'::pg_catalog.oid;",
							  pg_type_oid);

			res = ExecuteSqlQueryForSingleRow(fout, upgrade_query->data);

			pg_type_multirange_oid = atooid(PQgetvalue(res, 0, PQfnumber(res, "oid")));
			pg_type_multirange_array_oid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typarray")));

			PQclear(res);
		}
		else
		{
			pg_type_multirange_oid = get_next_possible_free_pg_type_oid(fout, upgrade_query);
			pg_type_multirange_array_oid = get_next_possible_free_pg_type_oid(fout, upgrade_query);
		}

		appendPQExpBufferStr(upgrade_buffer,
							 "\n-- For binary upgrade, must preserve multirange pg_type oid\n");
		appendPQExpBuffer(upgrade_buffer,
						  "SELECT pg_catalog.binary_upgrade_set_next_multirange_pg_type_oid('%u'::pg_catalog.oid);\n\n",
						  pg_type_multirange_oid);
		appendPQExpBufferStr(upgrade_buffer,
							 "\n-- For binary upgrade, must preserve multirange pg_type array oid\n");
		appendPQExpBuffer(upgrade_buffer,
						  "SELECT pg_catalog.binary_upgrade_set_next_multirange_array_pg_type_oid('%u'::pg_catalog.oid);\n\n",
						  pg_type_multirange_array_oid);
	}

	destroyPQExpBuffer(upgrade_query);
}

static void
binary_upgrade_set_type_oids_by_rel(Archive *fout,
									PQExpBuffer upgrade_buffer,
									const TableInfo *tbinfo)
{
	Oid			pg_type_oid = tbinfo->reltype;

	if (OidIsValid(pg_type_oid))
		binary_upgrade_set_type_oids_by_type_oid(fout, upgrade_buffer,
												 pg_type_oid, false, false);
}

/*
 * bsearch() comparator for BinaryUpgradeClassOidItem
 */
static int
BinaryUpgradeClassOidItemCmp(const void *p1, const void *p2)
{
	BinaryUpgradeClassOidItem v1 = *((const BinaryUpgradeClassOidItem *) p1);
	BinaryUpgradeClassOidItem v2 = *((const BinaryUpgradeClassOidItem *) p2);

	return pg_cmp_u32(v1.oid, v2.oid);
}

/*
 * collectBinaryUpgradeClassOids
 *
 * Construct a table of pg_class information required for
 * binary_upgrade_set_pg_class_oids().  The table is sorted by OID for speed in
 * lookup.
 */
static void
collectBinaryUpgradeClassOids(Archive *fout)
{
	PGresult   *res;
	const char *query;

	query = "SELECT c.oid, c.relkind, c.relfilenode, c.reltoastrelid, "
		"ct.relfilenode, i.indexrelid, cti.relfilenode "
		"FROM pg_catalog.pg_class c LEFT JOIN pg_catalog.pg_index i "
		"ON (c.reltoastrelid = i.indrelid AND i.indisvalid) "
		"LEFT JOIN pg_catalog.pg_class ct ON (c.reltoastrelid = ct.oid) "
		"LEFT JOIN pg_catalog.pg_class AS cti ON (i.indexrelid = cti.oid) "
		"ORDER BY c.oid;";

	res = ExecuteSqlQuery(fout, query, PGRES_TUPLES_OK);

	nbinaryUpgradeClassOids = PQntuples(res);
	binaryUpgradeClassOids = (BinaryUpgradeClassOidItem *)
		pg_malloc(nbinaryUpgradeClassOids * sizeof(BinaryUpgradeClassOidItem));

	for (int i = 0; i < nbinaryUpgradeClassOids; i++)
	{
		binaryUpgradeClassOids[i].oid = atooid(PQgetvalue(res, i, 0));
		binaryUpgradeClassOids[i].relkind = *PQgetvalue(res, i, 1);
		binaryUpgradeClassOids[i].relfilenumber = atooid(PQgetvalue(res, i, 2));
		binaryUpgradeClassOids[i].toast_oid = atooid(PQgetvalue(res, i, 3));
		binaryUpgradeClassOids[i].toast_relfilenumber = atooid(PQgetvalue(res, i, 4));
		binaryUpgradeClassOids[i].toast_index_oid = atooid(PQgetvalue(res, i, 5));
		binaryUpgradeClassOids[i].toast_index_relfilenumber = atooid(PQgetvalue(res, i, 6));
	}

	PQclear(res);
}

static void
binary_upgrade_set_pg_class_oids(Archive *fout,
								 PQExpBuffer upgrade_buffer, Oid pg_class_oid)
{
	BinaryUpgradeClassOidItem key = {0};
	BinaryUpgradeClassOidItem *entry;

	Assert(binaryUpgradeClassOids);

	/*
	 * Preserve the OID and relfilenumber of the table, table's index, table's
	 * toast table and toast table's index if any.
	 *
	 * One complexity is that the current table definition might not require
	 * the creation of a TOAST table, but the old database might have a TOAST
	 * table that was created earlier, before some wide columns were dropped.
	 * By setting the TOAST oid we force creation of the TOAST heap and index
	 * by the new backend, so we can copy the files during binary upgrade
	 * without worrying about this case.
	 */
	key.oid = pg_class_oid;
	entry = bsearch(&key, binaryUpgradeClassOids, nbinaryUpgradeClassOids,
					sizeof(BinaryUpgradeClassOidItem),
					BinaryUpgradeClassOidItemCmp);

	appendPQExpBufferStr(upgrade_buffer,
						 "\n-- For binary upgrade, must preserve pg_class oids and relfilenodes\n");

	if (entry->relkind != RELKIND_INDEX &&
		entry->relkind != RELKIND_PARTITIONED_INDEX)
	{
		appendPQExpBuffer(upgrade_buffer,
						  "SELECT pg_catalog.binary_upgrade_set_next_heap_pg_class_oid('%u'::pg_catalog.oid);\n",
						  pg_class_oid);

		/*
		 * Not every relation has storage. Also, in a pre-v12 database,
		 * partitioned tables have a relfilenumber, which should not be
		 * preserved when upgrading.
		 */
		if (RelFileNumberIsValid(entry->relfilenumber) &&
			entry->relkind != RELKIND_PARTITIONED_TABLE)
			appendPQExpBuffer(upgrade_buffer,
							  "SELECT pg_catalog.binary_upgrade_set_next_heap_relfilenode('%u'::pg_catalog.oid);\n",
							  entry->relfilenumber);

		/*
		 * In a pre-v12 database, partitioned tables might be marked as having
		 * toast tables, but we should ignore them if so.
		 */
		if (OidIsValid(entry->toast_oid) &&
			entry->relkind != RELKIND_PARTITIONED_TABLE)
		{
			appendPQExpBuffer(upgrade_buffer,
							  "SELECT pg_catalog.binary_upgrade_set_next_toast_pg_class_oid('%u'::pg_catalog.oid);\n",
							  entry->toast_oid);
			appendPQExpBuffer(upgrade_buffer,
							  "SELECT pg_catalog.binary_upgrade_set_next_toast_relfilenode('%u'::pg_catalog.oid);\n",
							  entry->toast_relfilenumber);

			/* every toast table has an index */
			appendPQExpBuffer(upgrade_buffer,
							  "SELECT pg_catalog.binary_upgrade_set_next_index_pg_class_oid('%u'::pg_catalog.oid);\n",
							  entry->toast_index_oid);
			appendPQExpBuffer(upgrade_buffer,
							  "SELECT pg_catalog.binary_upgrade_set_next_index_relfilenode('%u'::pg_catalog.oid);\n",
							  entry->toast_index_relfilenumber);
		}
	}
	else
	{
		/* Preserve the OID and relfilenumber of the index */
		appendPQExpBuffer(upgrade_buffer,
						  "SELECT pg_catalog.binary_upgrade_set_next_index_pg_class_oid('%u'::pg_catalog.oid);\n",
						  pg_class_oid);
		appendPQExpBuffer(upgrade_buffer,
						  "SELECT pg_catalog.binary_upgrade_set_next_index_relfilenode('%u'::pg_catalog.oid);\n",
						  entry->relfilenumber);
	}

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
								const DumpableObject *dobj,
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
		pg_fatal("could not find parent extension for %s %s",
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
 *	  get information about all namespaces in the system catalogs
 */
void
getNamespaces(Archive *fout)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query;
	NamespaceInfo *nsinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_nspname;
	int			i_nspowner;
	int			i_nspacl;
	int			i_acldefault;

	query = createPQExpBuffer();

	/*
	 * we fetch all namespaces including system ones, so that every object we
	 * read in can be linked to a containing namespace.
	 */
	appendPQExpBufferStr(query, "SELECT n.tableoid, n.oid, n.nspname, "
						 "n.nspowner, "
						 "n.nspacl, "
						 "acldefault('n', n.nspowner) AS acldefault "
						 "FROM pg_namespace n");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	nsinfo = (NamespaceInfo *) pg_malloc(ntups * sizeof(NamespaceInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_nspname = PQfnumber(res, "nspname");
	i_nspowner = PQfnumber(res, "nspowner");
	i_nspacl = PQfnumber(res, "nspacl");
	i_acldefault = PQfnumber(res, "acldefault");

	for (i = 0; i < ntups; i++)
	{
		const char *nspowner;

		nsinfo[i].dobj.objType = DO_NAMESPACE;
		nsinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		nsinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&nsinfo[i].dobj);
		nsinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_nspname));
		nsinfo[i].dacl.acl = pg_strdup(PQgetvalue(res, i, i_nspacl));
		nsinfo[i].dacl.acldefault = pg_strdup(PQgetvalue(res, i, i_acldefault));
		nsinfo[i].dacl.privtype = 0;
		nsinfo[i].dacl.initprivs = NULL;
		nspowner = PQgetvalue(res, i, i_nspowner);
		nsinfo[i].nspowner = atooid(nspowner);
		nsinfo[i].rolname = getRoleName(nspowner);

		/* Decide whether to dump this namespace */
		selectDumpableNamespace(&nsinfo[i], fout);

		/* Mark whether namespace has an ACL */
		if (!PQgetisnull(res, i, i_nspacl))
			nsinfo[i].dobj.components |= DUMP_COMPONENT_ACL;

		/*
		 * We ignore any pg_init_privs.initprivs entry for the public schema
		 * and assume a predetermined default, for several reasons.  First,
		 * dropping and recreating the schema removes its pg_init_privs entry,
		 * but an empty destination database starts with this ACL nonetheless.
		 * Second, we support dump/reload of public schema ownership changes.
		 * ALTER SCHEMA OWNER filters nspacl through aclnewowner(), but
		 * initprivs continues to reflect the initial owner.  Hence,
		 * synthesize the value that nspacl will have after the restore's
		 * ALTER SCHEMA OWNER.  Third, this makes the destination database
		 * match the source's ACL, even if the latter was an initdb-default
		 * ACL, which changed in v15.  An upgrade pulls in changes to most
		 * system object ACLs that the DBA had not customized.  We've made the
		 * public schema depart from that, because changing its ACL so easily
		 * breaks applications.
		 */
		if (strcmp(nsinfo[i].dobj.name, "public") == 0)
		{
			PQExpBuffer aclarray = createPQExpBuffer();
			PQExpBuffer aclitem = createPQExpBuffer();

			/* Standard ACL as of v15 is {owner=UC/owner,=U/owner} */
			appendPQExpBufferChar(aclarray, '{');
			quoteAclUserName(aclitem, nsinfo[i].rolname);
			appendPQExpBufferStr(aclitem, "=UC/");
			quoteAclUserName(aclitem, nsinfo[i].rolname);
			appendPGArray(aclarray, aclitem->data);
			resetPQExpBuffer(aclitem);
			appendPQExpBufferStr(aclitem, "=U/");
			quoteAclUserName(aclitem, nsinfo[i].rolname);
			appendPGArray(aclarray, aclitem->data);
			appendPQExpBufferChar(aclarray, '}');

			nsinfo[i].dacl.privtype = 'i';
			nsinfo[i].dacl.initprivs = pstrdup(aclarray->data);
			nsinfo[i].dobj.components |= DUMP_COMPONENT_ACL;

			destroyPQExpBuffer(aclarray);
			destroyPQExpBuffer(aclitem);
		}
	}

	PQclear(res);
	destroyPQExpBuffer(query);
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
		pg_fatal("schema with OID %u does not exist", nsoid);
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
	ExtensionInfo *extinfo = NULL;
	int			i_tableoid;
	int			i_oid;
	int			i_extname;
	int			i_nspname;
	int			i_extrelocatable;
	int			i_extversion;
	int			i_extconfig;
	int			i_extcondition;

	query = createPQExpBuffer();

	appendPQExpBufferStr(query, "SELECT x.tableoid, x.oid, "
						 "x.extname, n.nspname, x.extrelocatable, x.extversion, x.extconfig, x.extcondition "
						 "FROM pg_extension x "
						 "JOIN pg_namespace n ON n.oid = x.extnamespace");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	if (ntups == 0)
		goto cleanup;

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

cleanup:
	PQclear(res);
	destroyPQExpBuffer(query);

	*numExtensions = ntups;

	return extinfo;
}

/*
 * getTypes:
 *	  get information about all types in the system catalogs
 *
 * NB: this must run after getFuncs() because we assume we can do
 * findFuncByOid().
 */
void
getTypes(Archive *fout)
{
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
	int			i_acldefault;
	int			i_typowner;
	int			i_typelem;
	int			i_typrelid;
	int			i_typrelkind;
	int			i_typtype;
	int			i_typisdefined;
	int			i_isarray;
	int			i_typarray;

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
	appendPQExpBufferStr(query, "SELECT tableoid, oid, typname, "
						 "typnamespace, typacl, "
						 "acldefault('T', typowner) AS acldefault, "
						 "typowner, "
						 "typelem, typrelid, typarray, "
						 "CASE WHEN typrelid = 0 THEN ' '::\"char\" "
						 "ELSE (SELECT relkind FROM pg_class WHERE oid = typrelid) END AS typrelkind, "
						 "typtype, typisdefined, "
						 "typname[0] = '_' AND typelem != 0 AND "
						 "(SELECT typarray FROM pg_type te WHERE oid = pg_type.typelem) = oid AS isarray "
						 "FROM pg_type");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	tyinfo = (TypeInfo *) pg_malloc(ntups * sizeof(TypeInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_typname = PQfnumber(res, "typname");
	i_typnamespace = PQfnumber(res, "typnamespace");
	i_typacl = PQfnumber(res, "typacl");
	i_acldefault = PQfnumber(res, "acldefault");
	i_typowner = PQfnumber(res, "typowner");
	i_typelem = PQfnumber(res, "typelem");
	i_typrelid = PQfnumber(res, "typrelid");
	i_typrelkind = PQfnumber(res, "typrelkind");
	i_typtype = PQfnumber(res, "typtype");
	i_typisdefined = PQfnumber(res, "typisdefined");
	i_isarray = PQfnumber(res, "isarray");
	i_typarray = PQfnumber(res, "typarray");

	for (i = 0; i < ntups; i++)
	{
		tyinfo[i].dobj.objType = DO_TYPE;
		tyinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		tyinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&tyinfo[i].dobj);
		tyinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_typname));
		tyinfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_typnamespace)));
		tyinfo[i].dacl.acl = pg_strdup(PQgetvalue(res, i, i_typacl));
		tyinfo[i].dacl.acldefault = pg_strdup(PQgetvalue(res, i, i_acldefault));
		tyinfo[i].dacl.privtype = 0;
		tyinfo[i].dacl.initprivs = NULL;
		tyinfo[i].ftypname = NULL;	/* may get filled later */
		tyinfo[i].rolname = getRoleName(PQgetvalue(res, i, i_typowner));
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

		tyinfo[i].typarray = atooid(PQgetvalue(res, i, i_typarray));

		if (tyinfo[i].typtype == TYPTYPE_MULTIRANGE)
			tyinfo[i].isMultirange = true;
		else
			tyinfo[i].isMultirange = false;

		/* Decide whether we want to dump it */
		selectDumpableType(&tyinfo[i], fout);

		/* Mark whether type has an ACL */
		if (!PQgetisnull(res, i, i_typacl))
			tyinfo[i].dobj.components |= DUMP_COMPONENT_ACL;

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
	}

	PQclear(res);

	destroyPQExpBuffer(query);
}

/*
 * getOperators:
 *	  get information about all operators in the system catalogs
 */
void
getOperators(Archive *fout)
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
	int			i_oprowner;
	int			i_oprkind;
	int			i_oprcode;

	/*
	 * find all operators, including builtin operators; we filter out
	 * system-defined operators at dump-out time.
	 */

	appendPQExpBufferStr(query, "SELECT tableoid, oid, oprname, "
						 "oprnamespace, "
						 "oprowner, "
						 "oprkind, "
						 "oprcode::oid AS oprcode "
						 "FROM pg_operator");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	oprinfo = (OprInfo *) pg_malloc(ntups * sizeof(OprInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_oprname = PQfnumber(res, "oprname");
	i_oprnamespace = PQfnumber(res, "oprnamespace");
	i_oprowner = PQfnumber(res, "oprowner");
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
		oprinfo[i].rolname = getRoleName(PQgetvalue(res, i, i_oprowner));
		oprinfo[i].oprkind = (PQgetvalue(res, i, i_oprkind))[0];
		oprinfo[i].oprcode = atooid(PQgetvalue(res, i, i_oprcode));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(oprinfo[i].dobj), fout);
	}

	PQclear(res);

	destroyPQExpBuffer(query);
}

/*
 * getCollations:
 *	  get information about all collations in the system catalogs
 */
void
getCollations(Archive *fout)
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
	int			i_collowner;

	query = createPQExpBuffer();

	/*
	 * find all collations, including builtin collations; we filter out
	 * system-defined collations at dump-out time.
	 */

	appendPQExpBufferStr(query, "SELECT tableoid, oid, collname, "
						 "collnamespace, "
						 "collowner "
						 "FROM pg_collation");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	collinfo = (CollInfo *) pg_malloc(ntups * sizeof(CollInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_collname = PQfnumber(res, "collname");
	i_collnamespace = PQfnumber(res, "collnamespace");
	i_collowner = PQfnumber(res, "collowner");

	for (i = 0; i < ntups; i++)
	{
		collinfo[i].dobj.objType = DO_COLLATION;
		collinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		collinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&collinfo[i].dobj);
		collinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_collname));
		collinfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_collnamespace)));
		collinfo[i].rolname = getRoleName(PQgetvalue(res, i, i_collowner));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(collinfo[i].dobj), fout);
	}

	PQclear(res);

	destroyPQExpBuffer(query);
}

/*
 * getConversions:
 *	  get information about all conversions in the system catalogs
 */
void
getConversions(Archive *fout)
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
	int			i_conowner;

	query = createPQExpBuffer();

	/*
	 * find all conversions, including builtin conversions; we filter out
	 * system-defined conversions at dump-out time.
	 */

	appendPQExpBufferStr(query, "SELECT tableoid, oid, conname, "
						 "connamespace, "
						 "conowner "
						 "FROM pg_conversion");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	convinfo = (ConvInfo *) pg_malloc(ntups * sizeof(ConvInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_conname = PQfnumber(res, "conname");
	i_connamespace = PQfnumber(res, "connamespace");
	i_conowner = PQfnumber(res, "conowner");

	for (i = 0; i < ntups; i++)
	{
		convinfo[i].dobj.objType = DO_CONVERSION;
		convinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		convinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&convinfo[i].dobj);
		convinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_conname));
		convinfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_connamespace)));
		convinfo[i].rolname = getRoleName(PQgetvalue(res, i, i_conowner));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(convinfo[i].dobj), fout);
	}

	PQclear(res);

	destroyPQExpBuffer(query);
}

/*
 * getAccessMethods:
 *	  get information about all user-defined access methods
 */
void
getAccessMethods(Archive *fout)
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
		return;

	query = createPQExpBuffer();

	/* Select all access methods from pg_am table */
	appendPQExpBufferStr(query, "SELECT tableoid, oid, amname, amtype, "
						 "amhandler::pg_catalog.regproc AS amhandler "
						 "FROM pg_am");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

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
	}

	PQclear(res);

	destroyPQExpBuffer(query);
}


/*
 * getOpclasses:
 *	  get information about all opclasses in the system catalogs
 */
void
getOpclasses(Archive *fout)
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
	int			i_opcowner;

	/*
	 * find all opclasses, including builtin opclasses; we filter out
	 * system-defined opclasses at dump-out time.
	 */

	appendPQExpBufferStr(query, "SELECT tableoid, oid, opcname, "
						 "opcnamespace, "
						 "opcowner "
						 "FROM pg_opclass");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	opcinfo = (OpclassInfo *) pg_malloc(ntups * sizeof(OpclassInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_opcname = PQfnumber(res, "opcname");
	i_opcnamespace = PQfnumber(res, "opcnamespace");
	i_opcowner = PQfnumber(res, "opcowner");

	for (i = 0; i < ntups; i++)
	{
		opcinfo[i].dobj.objType = DO_OPCLASS;
		opcinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		opcinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&opcinfo[i].dobj);
		opcinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_opcname));
		opcinfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_opcnamespace)));
		opcinfo[i].rolname = getRoleName(PQgetvalue(res, i, i_opcowner));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(opcinfo[i].dobj), fout);
	}

	PQclear(res);

	destroyPQExpBuffer(query);
}

/*
 * getOpfamilies:
 *	  get information about all opfamilies in the system catalogs
 */
void
getOpfamilies(Archive *fout)
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
	int			i_opfowner;

	query = createPQExpBuffer();

	/*
	 * find all opfamilies, including builtin opfamilies; we filter out
	 * system-defined opfamilies at dump-out time.
	 */

	appendPQExpBufferStr(query, "SELECT tableoid, oid, opfname, "
						 "opfnamespace, "
						 "opfowner "
						 "FROM pg_opfamily");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	opfinfo = (OpfamilyInfo *) pg_malloc(ntups * sizeof(OpfamilyInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_opfname = PQfnumber(res, "opfname");
	i_opfnamespace = PQfnumber(res, "opfnamespace");
	i_opfowner = PQfnumber(res, "opfowner");

	for (i = 0; i < ntups; i++)
	{
		opfinfo[i].dobj.objType = DO_OPFAMILY;
		opfinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		opfinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&opfinfo[i].dobj);
		opfinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_opfname));
		opfinfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_opfnamespace)));
		opfinfo[i].rolname = getRoleName(PQgetvalue(res, i, i_opfowner));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(opfinfo[i].dobj), fout);
	}

	PQclear(res);

	destroyPQExpBuffer(query);
}

/*
 * getAggregates:
 *	  get information about all user-defined aggregates in the system catalogs
 */
void
getAggregates(Archive *fout)
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
	int			i_proowner;
	int			i_aggacl;
	int			i_acldefault;

	/*
	 * Find all interesting aggregates.  See comment in getFuncs() for the
	 * rationale behind the filtering logic.
	 */
	if (fout->remoteVersion >= 90600)
	{
		const char *agg_check;

		agg_check = (fout->remoteVersion >= 110000 ? "p.prokind = 'a'"
					 : "p.proisagg");

		appendPQExpBuffer(query, "SELECT p.tableoid, p.oid, "
						  "p.proname AS aggname, "
						  "p.pronamespace AS aggnamespace, "
						  "p.pronargs, p.proargtypes, "
						  "p.proowner, "
						  "p.proacl AS aggacl, "
						  "acldefault('f', p.proowner) AS acldefault "
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
						  agg_check);
		if (dopt->binary_upgrade)
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
		appendPQExpBufferStr(query, "SELECT tableoid, oid, proname AS aggname, "
							 "pronamespace AS aggnamespace, "
							 "pronargs, proargtypes, "
							 "proowner, "
							 "proacl AS aggacl, "
							 "acldefault('f', proowner) AS acldefault "
							 "FROM pg_proc p "
							 "WHERE proisagg AND ("
							 "pronamespace != "
							 "(SELECT oid FROM pg_namespace "
							 "WHERE nspname = 'pg_catalog')");
		if (dopt->binary_upgrade)
			appendPQExpBufferStr(query,
								 " OR EXISTS(SELECT 1 FROM pg_depend WHERE "
								 "classid = 'pg_proc'::regclass AND "
								 "objid = p.oid AND "
								 "refclassid = 'pg_extension'::regclass AND "
								 "deptype = 'e')");
		appendPQExpBufferChar(query, ')');
	}

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	agginfo = (AggInfo *) pg_malloc(ntups * sizeof(AggInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_aggname = PQfnumber(res, "aggname");
	i_aggnamespace = PQfnumber(res, "aggnamespace");
	i_pronargs = PQfnumber(res, "pronargs");
	i_proargtypes = PQfnumber(res, "proargtypes");
	i_proowner = PQfnumber(res, "proowner");
	i_aggacl = PQfnumber(res, "aggacl");
	i_acldefault = PQfnumber(res, "acldefault");

	for (i = 0; i < ntups; i++)
	{
		agginfo[i].aggfn.dobj.objType = DO_AGG;
		agginfo[i].aggfn.dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		agginfo[i].aggfn.dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&agginfo[i].aggfn.dobj);
		agginfo[i].aggfn.dobj.name = pg_strdup(PQgetvalue(res, i, i_aggname));
		agginfo[i].aggfn.dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_aggnamespace)));
		agginfo[i].aggfn.dacl.acl = pg_strdup(PQgetvalue(res, i, i_aggacl));
		agginfo[i].aggfn.dacl.acldefault = pg_strdup(PQgetvalue(res, i, i_acldefault));
		agginfo[i].aggfn.dacl.privtype = 0;
		agginfo[i].aggfn.dacl.initprivs = NULL;
		agginfo[i].aggfn.rolname = getRoleName(PQgetvalue(res, i, i_proowner));
		agginfo[i].aggfn.lang = InvalidOid; /* not currently interesting */
		agginfo[i].aggfn.prorettype = InvalidOid;	/* not saved */
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
		agginfo[i].aggfn.postponed_def = false; /* might get set during sort */

		/* Decide whether we want to dump it */
		selectDumpableObject(&(agginfo[i].aggfn.dobj), fout);

		/* Mark whether aggregate has an ACL */
		if (!PQgetisnull(res, i, i_aggacl))
			agginfo[i].aggfn.dobj.components |= DUMP_COMPONENT_ACL;
	}

	PQclear(res);

	destroyPQExpBuffer(query);
}

/*
 * getFuncs:
 *	  get information about all user-defined functions in the system catalogs
 */
void
getFuncs(Archive *fout)
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
	int			i_proowner;
	int			i_prolang;
	int			i_pronargs;
	int			i_proargtypes;
	int			i_prorettype;
	int			i_proacl;
	int			i_acldefault;

	/*
	 * Find all interesting functions.  This is a bit complicated:
	 *
	 * 1. Always exclude aggregates; those are handled elsewhere.
	 *
	 * 2. Always exclude functions that are internally dependent on something
	 * else, since presumably those will be created as a result of creating
	 * the something else.  This currently acts only to suppress constructor
	 * functions for range types.  Note this is OK only because the
	 * constructors don't have any dependencies the range type doesn't have;
	 * otherwise we might not get creation ordering correct.
	 *
	 * 3. Otherwise, we normally exclude functions in pg_catalog.  However, if
	 * they're members of extensions and we are in binary-upgrade mode then
	 * include them, since we want to dump extension members individually in
	 * that mode.  Also, if they are used by casts or transforms then we need
	 * to gather the information about them, though they won't be dumped if
	 * they are built-in.  Also, in 9.6 and up, include functions in
	 * pg_catalog if they have an ACL different from what's shown in
	 * pg_init_privs (so we have to join to pg_init_privs; annoying).
	 */
	if (fout->remoteVersion >= 90600)
	{
		const char *not_agg_check;

		not_agg_check = (fout->remoteVersion >= 110000 ? "p.prokind <> 'a'"
						 : "NOT p.proisagg");

		appendPQExpBuffer(query,
						  "SELECT p.tableoid, p.oid, p.proname, p.prolang, "
						  "p.pronargs, p.proargtypes, p.prorettype, "
						  "p.proacl, "
						  "acldefault('f', p.proowner) AS acldefault, "
						  "p.pronamespace, "
						  "p.proowner "
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
	}
	else
	{
		appendPQExpBuffer(query,
						  "SELECT tableoid, oid, proname, prolang, "
						  "pronargs, proargtypes, prorettype, proacl, "
						  "acldefault('f', proowner) AS acldefault, "
						  "pronamespace, "
						  "proowner "
						  "FROM pg_proc p "
						  "WHERE NOT proisagg"
						  "\n  AND NOT EXISTS (SELECT 1 FROM pg_depend "
						  "WHERE classid = 'pg_proc'::regclass AND "
						  "objid = p.oid AND deptype = 'i')"
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

		if (dopt->binary_upgrade)
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

	finfo = (FuncInfo *) pg_malloc0(ntups * sizeof(FuncInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_proname = PQfnumber(res, "proname");
	i_pronamespace = PQfnumber(res, "pronamespace");
	i_proowner = PQfnumber(res, "proowner");
	i_prolang = PQfnumber(res, "prolang");
	i_pronargs = PQfnumber(res, "pronargs");
	i_proargtypes = PQfnumber(res, "proargtypes");
	i_prorettype = PQfnumber(res, "prorettype");
	i_proacl = PQfnumber(res, "proacl");
	i_acldefault = PQfnumber(res, "acldefault");

	for (i = 0; i < ntups; i++)
	{
		finfo[i].dobj.objType = DO_FUNC;
		finfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		finfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&finfo[i].dobj);
		finfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_proname));
		finfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_pronamespace)));
		finfo[i].dacl.acl = pg_strdup(PQgetvalue(res, i, i_proacl));
		finfo[i].dacl.acldefault = pg_strdup(PQgetvalue(res, i, i_acldefault));
		finfo[i].dacl.privtype = 0;
		finfo[i].dacl.initprivs = NULL;
		finfo[i].rolname = getRoleName(PQgetvalue(res, i, i_proowner));
		finfo[i].lang = atooid(PQgetvalue(res, i, i_prolang));
		finfo[i].prorettype = atooid(PQgetvalue(res, i, i_prorettype));
		finfo[i].nargs = atoi(PQgetvalue(res, i, i_pronargs));
		if (finfo[i].nargs == 0)
			finfo[i].argtypes = NULL;
		else
		{
			finfo[i].argtypes = (Oid *) pg_malloc(finfo[i].nargs * sizeof(Oid));
			parseOidArray(PQgetvalue(res, i, i_proargtypes),
						  finfo[i].argtypes, finfo[i].nargs);
		}
		finfo[i].postponed_def = false; /* might get set during sort */

		/* Decide whether we want to dump it */
		selectDumpableObject(&(finfo[i].dobj), fout);

		/* Mark whether function has an ACL */
		if (!PQgetisnull(res, i, i_proacl))
			finfo[i].dobj.components |= DUMP_COMPONENT_ACL;
	}

	PQclear(res);

	destroyPQExpBuffer(query);
}

/*
 * getRelationStatistics
 *    register the statistics object as a dependent of the relation.
 *
 * reltuples is passed as a string to avoid complexities in converting from/to
 * floating point.
 */
static RelStatsInfo *
getRelationStatistics(Archive *fout, DumpableObject *rel, int32 relpages,
					  char *reltuples, int32 relallvisible, char relkind,
					  char **indAttNames, int nindAttNames)
{
	if (!fout->dopt->dumpStatistics)
		return NULL;

	if ((relkind == RELKIND_RELATION) ||
		(relkind == RELKIND_PARTITIONED_TABLE) ||
		(relkind == RELKIND_INDEX) ||
		(relkind == RELKIND_PARTITIONED_INDEX) ||
		(relkind == RELKIND_MATVIEW))
	{
		RelStatsInfo *info = pg_malloc0(sizeof(RelStatsInfo));
		DumpableObject *dobj = &info->dobj;

		dobj->objType = DO_REL_STATS;
		dobj->catId.tableoid = 0;
		dobj->catId.oid = 0;
		AssignDumpId(dobj);
		dobj->dependencies = (DumpId *) pg_malloc(sizeof(DumpId));
		dobj->dependencies[0] = rel->dumpId;
		dobj->nDeps = 1;
		dobj->allocDeps = 1;
		dobj->components |= DUMP_COMPONENT_STATISTICS;
		dobj->name = pg_strdup(rel->name);
		dobj->namespace = rel->namespace;
		info->relpages = relpages;
		info->reltuples = pstrdup(reltuples);
		info->relallvisible = relallvisible;
		info->relkind = relkind;
		info->indAttNames = indAttNames;
		info->nindAttNames = nindAttNames;
		info->postponed_def = false;

		return info;
	}
	return NULL;
}

/*
 * getTables
 *	  read all the tables (no indexes) in the system catalogs,
 *	  and return them as an array of TableInfo structures
 *
 * *numTables is set to the number of tables read in
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
	int			i_reltype;
	int			i_relowner;
	int			i_relchecks;
	int			i_relhasindex;
	int			i_relhasrules;
	int			i_relpages;
	int			i_reltuples;
	int			i_relallvisible;
	int			i_toastpages;
	int			i_owning_tab;
	int			i_owning_col;
	int			i_reltablespace;
	int			i_relhasoids;
	int			i_relhastriggers;
	int			i_relpersistence;
	int			i_relispopulated;
	int			i_relreplident;
	int			i_relrowsec;
	int			i_relforcerowsec;
	int			i_relfrozenxid;
	int			i_toastfrozenxid;
	int			i_toastoid;
	int			i_relminmxid;
	int			i_toastminmxid;
	int			i_reloptions;
	int			i_checkoption;
	int			i_toastreloptions;
	int			i_reloftype;
	int			i_foreignserver;
	int			i_amname;
	int			i_is_identity_sequence;
	int			i_relacl;
	int			i_acldefault;
	int			i_ispartition;

	/*
	 * Find all the tables and table-like objects.
	 *
	 * We must fetch all tables in this phase because otherwise we cannot
	 * correctly identify inherited columns, owned sequences, etc.
	 *
	 * We include system catalogs, so that we can work if a user table is
	 * defined to inherit from a system catalog (pretty weird, but...)
	 *
	 * Note: in this phase we should collect only a minimal amount of
	 * information about each table, basically just enough to decide if it is
	 * interesting.  In particular, since we do not yet have lock on any user
	 * table, we MUST NOT invoke any server-side data collection functions
	 * (for instance, pg_get_partkeydef()).  Those are likely to fail or give
	 * wrong answers if any concurrent DDL is happening.
	 */

	appendPQExpBufferStr(query,
						 "SELECT c.tableoid, c.oid, c.relname, "
						 "c.relnamespace, c.relkind, c.reltype, "
						 "c.relowner, "
						 "c.relchecks, "
						 "c.relhasindex, c.relhasrules, c.relpages, "
						 "c.reltuples, c.relallvisible, c.relhastriggers, "
						 "c.relpersistence, "
						 "c.reloftype, "
						 "c.relacl, "
						 "acldefault(CASE WHEN c.relkind = " CppAsString2(RELKIND_SEQUENCE)
						 " THEN 's'::\"char\" ELSE 'r'::\"char\" END, c.relowner) AS acldefault, "
						 "CASE WHEN c.relkind = " CppAsString2(RELKIND_FOREIGN_TABLE) " THEN "
						 "(SELECT ftserver FROM pg_catalog.pg_foreign_table WHERE ftrelid = c.oid) "
						 "ELSE 0 END AS foreignserver, "
						 "c.relfrozenxid, tc.relfrozenxid AS tfrozenxid, "
						 "tc.oid AS toid, "
						 "tc.relpages AS toastpages, "
						 "tc.reloptions AS toast_reloptions, "
						 "d.refobjid AS owning_tab, "
						 "d.refobjsubid AS owning_col, "
						 "tsp.spcname AS reltablespace, ");

	if (fout->remoteVersion >= 120000)
		appendPQExpBufferStr(query,
							 "false AS relhasoids, ");
	else
		appendPQExpBufferStr(query,
							 "c.relhasoids, ");

	if (fout->remoteVersion >= 90300)
		appendPQExpBufferStr(query,
							 "c.relispopulated, ");
	else
		appendPQExpBufferStr(query,
							 "'t' as relispopulated, ");

	if (fout->remoteVersion >= 90400)
		appendPQExpBufferStr(query,
							 "c.relreplident, ");
	else
		appendPQExpBufferStr(query,
							 "'d' AS relreplident, ");

	if (fout->remoteVersion >= 90500)
		appendPQExpBufferStr(query,
							 "c.relrowsecurity, c.relforcerowsecurity, ");
	else
		appendPQExpBufferStr(query,
							 "false AS relrowsecurity, "
							 "false AS relforcerowsecurity, ");

	if (fout->remoteVersion >= 90300)
		appendPQExpBufferStr(query,
							 "c.relminmxid, tc.relminmxid AS tminmxid, ");
	else
		appendPQExpBufferStr(query,
							 "0 AS relminmxid, 0 AS tminmxid, ");

	if (fout->remoteVersion >= 90300)
		appendPQExpBufferStr(query,
							 "array_remove(array_remove(c.reloptions,'check_option=local'),'check_option=cascaded') AS reloptions, "
							 "CASE WHEN 'check_option=local' = ANY (c.reloptions) THEN 'LOCAL'::text "
							 "WHEN 'check_option=cascaded' = ANY (c.reloptions) THEN 'CASCADED'::text ELSE NULL END AS checkoption, ");
	else
		appendPQExpBufferStr(query,
							 "c.reloptions, NULL AS checkoption, ");

	if (fout->remoteVersion >= 90600)
		appendPQExpBufferStr(query,
							 "am.amname, ");
	else
		appendPQExpBufferStr(query,
							 "NULL AS amname, ");

	if (fout->remoteVersion >= 90600)
		appendPQExpBufferStr(query,
							 "(d.deptype = 'i') IS TRUE AS is_identity_sequence, ");
	else
		appendPQExpBufferStr(query,
							 "false AS is_identity_sequence, ");

	if (fout->remoteVersion >= 100000)
		appendPQExpBufferStr(query,
							 "c.relispartition AS ispartition ");
	else
		appendPQExpBufferStr(query,
							 "false AS ispartition ");

	/*
	 * Left join to pg_depend to pick up dependency info linking sequences to
	 * their owning column, if any (note this dependency is AUTO except for
	 * identity sequences, where it's INTERNAL). Also join to pg_tablespace to
	 * collect the spcname.
	 */
	appendPQExpBufferStr(query,
						 "\nFROM pg_class c\n"
						 "LEFT JOIN pg_depend d ON "
						 "(c.relkind = " CppAsString2(RELKIND_SEQUENCE) " AND "
						 "d.classid = 'pg_class'::regclass AND d.objid = c.oid AND "
						 "d.objsubid = 0 AND "
						 "d.refclassid = 'pg_class'::regclass AND d.deptype IN ('a', 'i'))\n"
						 "LEFT JOIN pg_tablespace tsp ON (tsp.oid = c.reltablespace)\n");

	/*
	 * In 9.6 and up, left join to pg_am to pick up the amname.
	 */
	if (fout->remoteVersion >= 90600)
		appendPQExpBufferStr(query,
							 "LEFT JOIN pg_am am ON (c.relam = am.oid)\n");

	/*
	 * We purposefully ignore toast OIDs for partitioned tables; the reason is
	 * that versions 10 and 11 have them, but later versions do not, so
	 * emitting them causes the upgrade to fail.
	 */
	appendPQExpBufferStr(query,
						 "LEFT JOIN pg_class tc ON (c.reltoastrelid = tc.oid"
						 " AND tc.relkind = " CppAsString2(RELKIND_TOASTVALUE)
						 " AND c.relkind <> " CppAsString2(RELKIND_PARTITIONED_TABLE) ")\n");

	/*
	 * Restrict to interesting relkinds (in particular, not indexes).  Not all
	 * relkinds are possible in older servers, but it's not worth the trouble
	 * to emit a version-dependent list.
	 *
	 * Composite-type table entries won't be dumped as such, but we have to
	 * make a DumpableObject for them so that we can track dependencies of the
	 * composite type (pg_depend entries for columns of the composite type
	 * link to the pg_class entry not the pg_type entry).
	 */
	appendPQExpBufferStr(query,
						 "WHERE c.relkind IN ("
						 CppAsString2(RELKIND_RELATION) ", "
						 CppAsString2(RELKIND_SEQUENCE) ", "
						 CppAsString2(RELKIND_VIEW) ", "
						 CppAsString2(RELKIND_COMPOSITE_TYPE) ", "
						 CppAsString2(RELKIND_MATVIEW) ", "
						 CppAsString2(RELKIND_FOREIGN_TABLE) ", "
						 CppAsString2(RELKIND_PARTITIONED_TABLE) ")\n"
						 "ORDER BY c.oid");

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
	i_relkind = PQfnumber(res, "relkind");
	i_reltype = PQfnumber(res, "reltype");
	i_relowner = PQfnumber(res, "relowner");
	i_relchecks = PQfnumber(res, "relchecks");
	i_relhasindex = PQfnumber(res, "relhasindex");
	i_relhasrules = PQfnumber(res, "relhasrules");
	i_relpages = PQfnumber(res, "relpages");
	i_reltuples = PQfnumber(res, "reltuples");
	i_relallvisible = PQfnumber(res, "relallvisible");
	i_toastpages = PQfnumber(res, "toastpages");
	i_owning_tab = PQfnumber(res, "owning_tab");
	i_owning_col = PQfnumber(res, "owning_col");
	i_reltablespace = PQfnumber(res, "reltablespace");
	i_relhasoids = PQfnumber(res, "relhasoids");
	i_relhastriggers = PQfnumber(res, "relhastriggers");
	i_relpersistence = PQfnumber(res, "relpersistence");
	i_relispopulated = PQfnumber(res, "relispopulated");
	i_relreplident = PQfnumber(res, "relreplident");
	i_relrowsec = PQfnumber(res, "relrowsecurity");
	i_relforcerowsec = PQfnumber(res, "relforcerowsecurity");
	i_relfrozenxid = PQfnumber(res, "relfrozenxid");
	i_toastfrozenxid = PQfnumber(res, "tfrozenxid");
	i_toastoid = PQfnumber(res, "toid");
	i_relminmxid = PQfnumber(res, "relminmxid");
	i_toastminmxid = PQfnumber(res, "tminmxid");
	i_reloptions = PQfnumber(res, "reloptions");
	i_checkoption = PQfnumber(res, "checkoption");
	i_toastreloptions = PQfnumber(res, "toast_reloptions");
	i_reloftype = PQfnumber(res, "reloftype");
	i_foreignserver = PQfnumber(res, "foreignserver");
	i_amname = PQfnumber(res, "amname");
	i_is_identity_sequence = PQfnumber(res, "is_identity_sequence");
	i_relacl = PQfnumber(res, "relacl");
	i_acldefault = PQfnumber(res, "acldefault");
	i_ispartition = PQfnumber(res, "ispartition");

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

	resetPQExpBuffer(query);

	for (i = 0; i < ntups; i++)
	{
		int32		relallvisible = atoi(PQgetvalue(res, i, i_relallvisible));

		tblinfo[i].dobj.objType = DO_TABLE;
		tblinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_reltableoid));
		tblinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_reloid));
		AssignDumpId(&tblinfo[i].dobj);
		tblinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_relname));
		tblinfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_relnamespace)));
		tblinfo[i].dacl.acl = pg_strdup(PQgetvalue(res, i, i_relacl));
		tblinfo[i].dacl.acldefault = pg_strdup(PQgetvalue(res, i, i_acldefault));
		tblinfo[i].dacl.privtype = 0;
		tblinfo[i].dacl.initprivs = NULL;
		tblinfo[i].relkind = *(PQgetvalue(res, i, i_relkind));
		tblinfo[i].reltype = atooid(PQgetvalue(res, i, i_reltype));
		tblinfo[i].rolname = getRoleName(PQgetvalue(res, i, i_relowner));
		tblinfo[i].ncheck = atoi(PQgetvalue(res, i, i_relchecks));
		tblinfo[i].hasindex = (strcmp(PQgetvalue(res, i, i_relhasindex), "t") == 0);
		tblinfo[i].hasrules = (strcmp(PQgetvalue(res, i, i_relhasrules), "t") == 0);
		tblinfo[i].relpages = atoi(PQgetvalue(res, i, i_relpages));
		if (PQgetisnull(res, i, i_toastpages))
			tblinfo[i].toastpages = 0;
		else
			tblinfo[i].toastpages = atoi(PQgetvalue(res, i, i_toastpages));
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
		tblinfo[i].hasoids = (strcmp(PQgetvalue(res, i, i_relhasoids), "t") == 0);
		tblinfo[i].hastriggers = (strcmp(PQgetvalue(res, i, i_relhastriggers), "t") == 0);
		tblinfo[i].relpersistence = *(PQgetvalue(res, i, i_relpersistence));
		tblinfo[i].relispopulated = (strcmp(PQgetvalue(res, i, i_relispopulated), "t") == 0);
		tblinfo[i].relreplident = *(PQgetvalue(res, i, i_relreplident));
		tblinfo[i].rowsec = (strcmp(PQgetvalue(res, i, i_relrowsec), "t") == 0);
		tblinfo[i].forcerowsec = (strcmp(PQgetvalue(res, i, i_relforcerowsec), "t") == 0);
		tblinfo[i].frozenxid = atooid(PQgetvalue(res, i, i_relfrozenxid));
		tblinfo[i].toast_frozenxid = atooid(PQgetvalue(res, i, i_toastfrozenxid));
		tblinfo[i].toast_oid = atooid(PQgetvalue(res, i, i_toastoid));
		tblinfo[i].minmxid = atooid(PQgetvalue(res, i, i_relminmxid));
		tblinfo[i].toast_minmxid = atooid(PQgetvalue(res, i, i_toastminmxid));
		tblinfo[i].reloptions = pg_strdup(PQgetvalue(res, i, i_reloptions));
		if (PQgetisnull(res, i, i_checkoption))
			tblinfo[i].checkoption = NULL;
		else
			tblinfo[i].checkoption = pg_strdup(PQgetvalue(res, i, i_checkoption));
		tblinfo[i].toast_reloptions = pg_strdup(PQgetvalue(res, i, i_toastreloptions));
		tblinfo[i].reloftype = atooid(PQgetvalue(res, i, i_reloftype));
		tblinfo[i].foreign_server = atooid(PQgetvalue(res, i, i_foreignserver));
		if (PQgetisnull(res, i, i_amname))
			tblinfo[i].amname = NULL;
		else
			tblinfo[i].amname = pg_strdup(PQgetvalue(res, i, i_amname));
		tblinfo[i].is_identity_sequence = (strcmp(PQgetvalue(res, i, i_is_identity_sequence), "t") == 0);
		tblinfo[i].ispartition = (strcmp(PQgetvalue(res, i, i_ispartition), "t") == 0);

		/* other fields were zeroed above */

		/*
		 * Decide whether we want to dump this table.
		 */
		if (tblinfo[i].relkind == RELKIND_COMPOSITE_TYPE)
			tblinfo[i].dobj.dump = DUMP_COMPONENT_NONE;
		else
			selectDumpableTable(&tblinfo[i], fout);

		/*
		 * Now, consider the table "interesting" if we need to dump its
		 * definition, data or its statistics.  Later on, we'll skip a lot of
		 * data collection for uninteresting tables.
		 *
		 * Note: the "interesting" flag will also be set by flagInhTables for
		 * parents of interesting tables, so that we collect necessary
		 * inheritance info even when the parents are not themselves being
		 * dumped.  This is the main reason why we need an "interesting" flag
		 * that's separate from the components-to-dump bitmask.
		 */
		tblinfo[i].interesting = (tblinfo[i].dobj.dump &
								  (DUMP_COMPONENT_DEFINITION |
								   DUMP_COMPONENT_DATA |
								   DUMP_COMPONENT_STATISTICS)) != 0;

		tblinfo[i].dummy_view = false;	/* might get set during sort */
		tblinfo[i].postponed_def = false;	/* might get set during sort */

		/* Tables have data */
		tblinfo[i].dobj.components |= DUMP_COMPONENT_DATA;

		/* Mark whether table has an ACL */
		if (!PQgetisnull(res, i, i_relacl))
			tblinfo[i].dobj.components |= DUMP_COMPONENT_ACL;
		tblinfo[i].hascolumnACLs = false;	/* may get set later */

		/* Add statistics */
		if (tblinfo[i].interesting)
			getRelationStatistics(fout, &tblinfo[i].dobj, tblinfo[i].relpages,
								  PQgetvalue(res, i, i_reltuples),
								  relallvisible, tblinfo[i].relkind, NULL, 0);

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
		if ((tblinfo[i].dobj.dump & DUMP_COMPONENTS_REQUIRING_LOCK) &&
			(tblinfo[i].relkind == RELKIND_RELATION ||
			 tblinfo[i].relkind == RELKIND_PARTITIONED_TABLE))
		{
			/*
			 * Tables are locked in batches.  When dumping from a remote
			 * server this can save a significant amount of time by reducing
			 * the number of round trips.
			 */
			if (query->len == 0)
				appendPQExpBuffer(query, "LOCK TABLE %s",
								  fmtQualifiedDumpable(&tblinfo[i]));
			else
			{
				appendPQExpBuffer(query, ", %s",
								  fmtQualifiedDumpable(&tblinfo[i]));

				/* Arbitrarily end a batch when query length reaches 100K. */
				if (query->len >= 100000)
				{
					/* Lock another batch of tables. */
					appendPQExpBufferStr(query, " IN ACCESS SHARE MODE");
					ExecuteSqlStatement(fout, query->data);
					resetPQExpBuffer(query);
				}
			}
		}
	}

	if (query->len != 0)
	{
		/* Lock the tables in the last batch. */
		appendPQExpBufferStr(query, " IN ACCESS SHARE MODE");
		ExecuteSqlStatement(fout, query->data);
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
			pg_fatal("failed sanity check, parent table with OID %u of sequence with OID %u not found",
					 seqinfo->owning_tab, seqinfo->dobj.catId.oid);

		/*
		 * For an identity sequence, dump exactly the same components for the
		 * sequence as for the owning table.  This is important because we
		 * treat the identity sequence as an integral part of the table.  For
		 * example, there is not any DDL command that allows creation of such
		 * a sequence independently of the table.
		 *
		 * For other owned sequences such as serial sequences, we need to dump
		 * the components that are being dumped for the table and any
		 * components that the sequence is explicitly marked with.
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
		if (seqinfo->is_identity_sequence)
			seqinfo->dobj.dump = owning_tab->dobj.dump;
		else
			seqinfo->dobj.dump |= owning_tab->dobj.dump;

		/* Make sure that necessary data is available if we're dumping it */
		if (seqinfo->dobj.dump != DUMP_COMPONENT_NONE)
		{
			seqinfo->interesting = true;
			owning_tab->interesting = true;
		}
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

	/* find all the inheritance information */
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
 * getPartitioningInfo
 *	  get information about partitioning
 *
 * For the most part, we only collect partitioning info about tables we
 * intend to dump.  However, this function has to consider all partitioned
 * tables in the database, because we need to know about parents of partitions
 * we are going to dump even if the parents themselves won't be dumped.
 *
 * Specifically, what we need to know is whether each partitioned table
 * has an "unsafe" partitioning scheme that requires us to force
 * load-via-partition-root mode for its children.  Currently the only case
 * for which we force that is hash partitioning on enum columns, since the
 * hash codes depend on enum value OIDs which won't be replicated across
 * dump-and-reload.  There are other cases in which load-via-partition-root
 * might be necessary, but we expect users to cope with them.
 */
void
getPartitioningInfo(Archive *fout)
{
	PQExpBuffer query;
	PGresult   *res;
	int			ntups;

	/* hash partitioning didn't exist before v11 */
	if (fout->remoteVersion < 110000)
		return;
	/* needn't bother if not dumping data */
	if (!fout->dopt->dumpData)
		return;

	query = createPQExpBuffer();

	/*
	 * Unsafe partitioning schemes are exactly those for which hash enum_ops
	 * appears among the partition opclasses.  We needn't check partstrat.
	 *
	 * Note that this query may well retrieve info about tables we aren't
	 * going to dump and hence have no lock on.  That's okay since we need not
	 * invoke any unsafe server-side functions.
	 */
	appendPQExpBufferStr(query,
						 "SELECT partrelid FROM pg_partitioned_table WHERE\n"
						 "(SELECT c.oid FROM pg_opclass c JOIN pg_am a "
						 "ON c.opcmethod = a.oid\n"
						 "WHERE opcname = 'enum_ops' "
						 "AND opcnamespace = 'pg_catalog'::regnamespace "
						 "AND amname = 'hash') = ANY(partclass)");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	for (int i = 0; i < ntups; i++)
	{
		Oid			tabrelid = atooid(PQgetvalue(res, i, 0));
		TableInfo  *tbinfo;

		tbinfo = findTableByOid(tabrelid);
		if (tbinfo == NULL)
			pg_fatal("failed sanity check, table OID %u appearing in pg_partitioned_table not found",
					 tabrelid);
		tbinfo->unsafe_partitions = true;
	}

	PQclear(res);

	destroyPQExpBuffer(query);
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
	PQExpBuffer query = createPQExpBuffer();
	PQExpBuffer tbloids = createPQExpBuffer();
	PGresult   *res;
	int			ntups;
	int			curtblindx;
	IndxInfo   *indxinfo;
	int			i_tableoid,
				i_oid,
				i_indrelid,
				i_indexname,
				i_relpages,
				i_reltuples,
				i_relallvisible,
				i_parentidx,
				i_indexdef,
				i_indnkeyatts,
				i_indnatts,
				i_indkey,
				i_indisclustered,
				i_indisreplident,
				i_indnullsnotdistinct,
				i_contype,
				i_conname,
				i_condeferrable,
				i_condeferred,
				i_conperiod,
				i_contableoid,
				i_conoid,
				i_condef,
				i_indattnames,
				i_tablespace,
				i_indreloptions,
				i_indstatcols,
				i_indstatvals;

	/*
	 * We want to perform just one query against pg_index.  However, we
	 * mustn't try to select every row of the catalog and then sort it out on
	 * the client side, because some of the server-side functions we need
	 * would be unsafe to apply to tables we don't have lock on.  Hence, we
	 * build an array of the OIDs of tables we care about (and now have lock
	 * on!), and use a WHERE clause to constrain which rows are selected.
	 */
	appendPQExpBufferChar(tbloids, '{');
	for (int i = 0; i < numTables; i++)
	{
		TableInfo  *tbinfo = &tblinfo[i];

		if (!tbinfo->hasindex)
			continue;

		/*
		 * We can ignore indexes of uninteresting tables.
		 */
		if (!tbinfo->interesting)
			continue;

		/* OK, we need info for this table */
		if (tbloids->len > 1)	/* do we have more than the '{'? */
			appendPQExpBufferChar(tbloids, ',');
		appendPQExpBuffer(tbloids, "%u", tbinfo->dobj.catId.oid);
	}
	appendPQExpBufferChar(tbloids, '}');

	appendPQExpBufferStr(query,
						 "SELECT t.tableoid, t.oid, i.indrelid, "
						 "t.relname AS indexname, "
						 "t.relpages, t.reltuples, t.relallvisible, "
						 "pg_catalog.pg_get_indexdef(i.indexrelid) AS indexdef, "
						 "i.indkey, i.indisclustered, "
						 "c.contype, c.conname, "
						 "c.condeferrable, c.condeferred, "
						 "c.tableoid AS contableoid, "
						 "c.oid AS conoid, "
						 "pg_catalog.pg_get_constraintdef(c.oid, false) AS condef, "
						 "CASE WHEN i.indexprs IS NOT NULL THEN "
						 "(SELECT pg_catalog.array_agg(attname ORDER BY attnum)"
						 "  FROM pg_catalog.pg_attribute "
						 "  WHERE attrelid = i.indexrelid) "
						 "ELSE NULL END AS indattnames, "
						 "(SELECT spcname FROM pg_catalog.pg_tablespace s WHERE s.oid = t.reltablespace) AS tablespace, "
						 "t.reloptions AS indreloptions, ");


	if (fout->remoteVersion >= 90400)
		appendPQExpBufferStr(query,
							 "i.indisreplident, ");
	else
		appendPQExpBufferStr(query,
							 "false AS indisreplident, ");

	if (fout->remoteVersion >= 110000)
		appendPQExpBufferStr(query,
							 "inh.inhparent AS parentidx, "
							 "i.indnkeyatts AS indnkeyatts, "
							 "i.indnatts AS indnatts, "
							 "(SELECT pg_catalog.array_agg(attnum ORDER BY attnum) "
							 "  FROM pg_catalog.pg_attribute "
							 "  WHERE attrelid = i.indexrelid AND "
							 "    attstattarget >= 0) AS indstatcols, "
							 "(SELECT pg_catalog.array_agg(attstattarget ORDER BY attnum) "
							 "  FROM pg_catalog.pg_attribute "
							 "  WHERE attrelid = i.indexrelid AND "
							 "    attstattarget >= 0) AS indstatvals, ");
	else
		appendPQExpBufferStr(query,
							 "0 AS parentidx, "
							 "i.indnatts AS indnkeyatts, "
							 "i.indnatts AS indnatts, "
							 "'' AS indstatcols, "
							 "'' AS indstatvals, ");

	if (fout->remoteVersion >= 150000)
		appendPQExpBufferStr(query,
							 "i.indnullsnotdistinct, ");
	else
		appendPQExpBufferStr(query,
							 "false AS indnullsnotdistinct, ");

	if (fout->remoteVersion >= 180000)
		appendPQExpBufferStr(query,
							 "c.conperiod ");
	else
		appendPQExpBufferStr(query,
							 "NULL AS conperiod ");

	/*
	 * The point of the messy-looking outer join is to find a constraint that
	 * is related by an internal dependency link to the index. If we find one,
	 * create a CONSTRAINT entry linked to the INDEX entry.  We assume an
	 * index won't have more than one internal dependency.
	 *
	 * Note: the check on conrelid is redundant, but useful because that
	 * column is indexed while conindid is not.
	 */
	if (fout->remoteVersion >= 110000)
	{
		appendPQExpBuffer(query,
						  "FROM unnest('%s'::pg_catalog.oid[]) AS src(tbloid)\n"
						  "JOIN pg_catalog.pg_index i ON (src.tbloid = i.indrelid) "
						  "JOIN pg_catalog.pg_class t ON (t.oid = i.indexrelid) "
						  "JOIN pg_catalog.pg_class t2 ON (t2.oid = i.indrelid) "
						  "LEFT JOIN pg_catalog.pg_constraint c "
						  "ON (i.indrelid = c.conrelid AND "
						  "i.indexrelid = c.conindid AND "
						  "c.contype IN ('p','u','x')) "
						  "LEFT JOIN pg_catalog.pg_inherits inh "
						  "ON (inh.inhrelid = indexrelid) "
						  "WHERE (i.indisvalid OR t2.relkind = 'p') "
						  "AND i.indisready "
						  "ORDER BY i.indrelid, indexname",
						  tbloids->data);
	}
	else
	{
		/*
		 * the test on indisready is necessary in 9.2, and harmless in
		 * earlier/later versions
		 */
		appendPQExpBuffer(query,
						  "FROM unnest('%s'::pg_catalog.oid[]) AS src(tbloid)\n"
						  "JOIN pg_catalog.pg_index i ON (src.tbloid = i.indrelid) "
						  "JOIN pg_catalog.pg_class t ON (t.oid = i.indexrelid) "
						  "LEFT JOIN pg_catalog.pg_constraint c "
						  "ON (i.indrelid = c.conrelid AND "
						  "i.indexrelid = c.conindid AND "
						  "c.contype IN ('p','u','x')) "
						  "WHERE i.indisvalid AND i.indisready "
						  "ORDER BY i.indrelid, indexname",
						  tbloids->data);
	}

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_indrelid = PQfnumber(res, "indrelid");
	i_indexname = PQfnumber(res, "indexname");
	i_relpages = PQfnumber(res, "relpages");
	i_reltuples = PQfnumber(res, "reltuples");
	i_relallvisible = PQfnumber(res, "relallvisible");
	i_parentidx = PQfnumber(res, "parentidx");
	i_indexdef = PQfnumber(res, "indexdef");
	i_indnkeyatts = PQfnumber(res, "indnkeyatts");
	i_indnatts = PQfnumber(res, "indnatts");
	i_indkey = PQfnumber(res, "indkey");
	i_indisclustered = PQfnumber(res, "indisclustered");
	i_indisreplident = PQfnumber(res, "indisreplident");
	i_indnullsnotdistinct = PQfnumber(res, "indnullsnotdistinct");
	i_contype = PQfnumber(res, "contype");
	i_conname = PQfnumber(res, "conname");
	i_condeferrable = PQfnumber(res, "condeferrable");
	i_condeferred = PQfnumber(res, "condeferred");
	i_conperiod = PQfnumber(res, "conperiod");
	i_contableoid = PQfnumber(res, "contableoid");
	i_conoid = PQfnumber(res, "conoid");
	i_condef = PQfnumber(res, "condef");
	i_indattnames = PQfnumber(res, "indattnames");
	i_tablespace = PQfnumber(res, "tablespace");
	i_indreloptions = PQfnumber(res, "indreloptions");
	i_indstatcols = PQfnumber(res, "indstatcols");
	i_indstatvals = PQfnumber(res, "indstatvals");

	indxinfo = (IndxInfo *) pg_malloc(ntups * sizeof(IndxInfo));

	/*
	 * Outer loop iterates once per table, not once per row.  Incrementing of
	 * j is handled by the inner loop.
	 */
	curtblindx = -1;
	for (int j = 0; j < ntups;)
	{
		Oid			indrelid = atooid(PQgetvalue(res, j, i_indrelid));
		TableInfo  *tbinfo = NULL;
		char	  **indAttNames = NULL;
		int			nindAttNames = 0;
		int			numinds;

		/* Count rows for this table */
		for (numinds = 1; numinds < ntups - j; numinds++)
			if (atooid(PQgetvalue(res, j + numinds, i_indrelid)) != indrelid)
				break;

		/*
		 * Locate the associated TableInfo; we rely on tblinfo[] being in OID
		 * order.
		 */
		while (++curtblindx < numTables)
		{
			tbinfo = &tblinfo[curtblindx];
			if (tbinfo->dobj.catId.oid == indrelid)
				break;
		}
		if (curtblindx >= numTables)
			pg_fatal("unrecognized table OID %u", indrelid);
		/* cross-check that we only got requested tables */
		if (!tbinfo->hasindex ||
			!tbinfo->interesting)
			pg_fatal("unexpected index data for table \"%s\"",
					 tbinfo->dobj.name);

		/* Save data for this table */
		tbinfo->indexes = indxinfo + j;
		tbinfo->numIndexes = numinds;

		for (int c = 0; c < numinds; c++, j++)
		{
			char		contype;
			char		indexkind;
			RelStatsInfo *relstats;
			int32		relpages = atoi(PQgetvalue(res, j, i_relpages));
			int32		relallvisible = atoi(PQgetvalue(res, j, i_relallvisible));

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
			indxinfo[j].indkeys = (Oid *) pg_malloc(indxinfo[j].indnattrs * sizeof(Oid));
			parseOidArray(PQgetvalue(res, j, i_indkey),
						  indxinfo[j].indkeys, indxinfo[j].indnattrs);
			indxinfo[j].indisclustered = (PQgetvalue(res, j, i_indisclustered)[0] == 't');
			indxinfo[j].indisreplident = (PQgetvalue(res, j, i_indisreplident)[0] == 't');
			indxinfo[j].indnullsnotdistinct = (PQgetvalue(res, j, i_indnullsnotdistinct)[0] == 't');
			indxinfo[j].parentidx = atooid(PQgetvalue(res, j, i_parentidx));
			indxinfo[j].partattaches = (SimplePtrList)
			{
				NULL, NULL
			};

			if (indxinfo[j].parentidx == 0)
				indexkind = RELKIND_INDEX;
			else
				indexkind = RELKIND_PARTITIONED_INDEX;

			if (!PQgetisnull(res, j, i_indattnames))
			{
				if (!parsePGArray(PQgetvalue(res, j, i_indattnames),
								  &indAttNames, &nindAttNames))
					pg_fatal("could not parse %s array", "indattnames");
			}

			relstats = getRelationStatistics(fout, &indxinfo[j].dobj, relpages,
											 PQgetvalue(res, j, i_reltuples),
											 relallvisible, indexkind,
											 indAttNames, nindAttNames);

			contype = *(PQgetvalue(res, j, i_contype));
			if (contype == 'p' || contype == 'u' || contype == 'x')
			{
				/*
				 * If we found a constraint matching the index, create an
				 * entry for it.
				 */
				ConstraintInfo *constrinfo;

				constrinfo = (ConstraintInfo *) pg_malloc(sizeof(ConstraintInfo));
				constrinfo->dobj.objType = DO_CONSTRAINT;
				constrinfo->dobj.catId.tableoid = atooid(PQgetvalue(res, j, i_contableoid));
				constrinfo->dobj.catId.oid = atooid(PQgetvalue(res, j, i_conoid));
				AssignDumpId(&constrinfo->dobj);
				constrinfo->dobj.dump = tbinfo->dobj.dump;
				constrinfo->dobj.name = pg_strdup(PQgetvalue(res, j, i_conname));
				constrinfo->dobj.namespace = tbinfo->dobj.namespace;
				constrinfo->contable = tbinfo;
				constrinfo->condomain = NULL;
				constrinfo->contype = contype;
				if (contype == 'x')
					constrinfo->condef = pg_strdup(PQgetvalue(res, j, i_condef));
				else
					constrinfo->condef = NULL;
				constrinfo->confrelid = InvalidOid;
				constrinfo->conindex = indxinfo[j].dobj.dumpId;
				constrinfo->condeferrable = *(PQgetvalue(res, j, i_condeferrable)) == 't';
				constrinfo->condeferred = *(PQgetvalue(res, j, i_condeferred)) == 't';
				constrinfo->conperiod = *(PQgetvalue(res, j, i_conperiod)) == 't';
				constrinfo->conislocal = true;
				constrinfo->separate = true;

				indxinfo[j].indexconstraint = constrinfo->dobj.dumpId;
				if (relstats != NULL)
					addObjectDependency(&relstats->dobj, constrinfo->dobj.dumpId);
			}
			else
			{
				/* Plain secondary index */
				indxinfo[j].indexconstraint = 0;
			}
		}
	}

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(tbloids);
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
	int			i_stxowner;
	int			i_stxrelid;
	int			i_stattarget;
	int			i;

	/* Extended statistics were new in v10 */
	if (fout->remoteVersion < 100000)
		return;

	query = createPQExpBuffer();

	if (fout->remoteVersion < 130000)
		appendPQExpBufferStr(query, "SELECT tableoid, oid, stxname, "
							 "stxnamespace, stxowner, stxrelid, NULL AS stxstattarget "
							 "FROM pg_catalog.pg_statistic_ext");
	else
		appendPQExpBufferStr(query, "SELECT tableoid, oid, stxname, "
							 "stxnamespace, stxowner, stxrelid, stxstattarget "
							 "FROM pg_catalog.pg_statistic_ext");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_stxname = PQfnumber(res, "stxname");
	i_stxnamespace = PQfnumber(res, "stxnamespace");
	i_stxowner = PQfnumber(res, "stxowner");
	i_stxrelid = PQfnumber(res, "stxrelid");
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
		statsextinfo[i].rolname = getRoleName(PQgetvalue(res, i, i_stxowner));
		statsextinfo[i].stattable =
			findTableByOid(atooid(PQgetvalue(res, i, i_stxrelid)));
		if (PQgetisnull(res, i, i_stattarget))
			statsextinfo[i].stattarget = -1;
		else
			statsextinfo[i].stattarget = atoi(PQgetvalue(res, i, i_stattarget));

		/* Decide whether we want to dump it */
		selectDumpableStatisticsObject(&(statsextinfo[i]), fout);
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
	PQExpBuffer query = createPQExpBuffer();
	PQExpBuffer tbloids = createPQExpBuffer();
	PGresult   *res;
	int			ntups;
	int			curtblindx;
	TableInfo  *tbinfo = NULL;
	ConstraintInfo *constrinfo;
	int			i_contableoid,
				i_conoid,
				i_conrelid,
				i_conname,
				i_confrelid,
				i_conindid,
				i_condef;

	/*
	 * We want to perform just one query against pg_constraint.  However, we
	 * mustn't try to select every row of the catalog and then sort it out on
	 * the client side, because some of the server-side functions we need
	 * would be unsafe to apply to tables we don't have lock on.  Hence, we
	 * build an array of the OIDs of tables we care about (and now have lock
	 * on!), and use a WHERE clause to constrain which rows are selected.
	 */
	appendPQExpBufferChar(tbloids, '{');
	for (int i = 0; i < numTables; i++)
	{
		TableInfo  *tinfo = &tblinfo[i];

		/*
		 * For partitioned tables, foreign keys have no triggers so they must
		 * be included anyway in case some foreign keys are defined.
		 */
		if ((!tinfo->hastriggers &&
			 tinfo->relkind != RELKIND_PARTITIONED_TABLE) ||
			!(tinfo->dobj.dump & DUMP_COMPONENT_DEFINITION))
			continue;

		/* OK, we need info for this table */
		if (tbloids->len > 1)	/* do we have more than the '{'? */
			appendPQExpBufferChar(tbloids, ',');
		appendPQExpBuffer(tbloids, "%u", tinfo->dobj.catId.oid);
	}
	appendPQExpBufferChar(tbloids, '}');

	appendPQExpBufferStr(query,
						 "SELECT c.tableoid, c.oid, "
						 "conrelid, conname, confrelid, ");
	if (fout->remoteVersion >= 110000)
		appendPQExpBufferStr(query, "conindid, ");
	else
		appendPQExpBufferStr(query, "0 AS conindid, ");
	appendPQExpBuffer(query,
					  "pg_catalog.pg_get_constraintdef(c.oid) AS condef\n"
					  "FROM unnest('%s'::pg_catalog.oid[]) AS src(tbloid)\n"
					  "JOIN pg_catalog.pg_constraint c ON (src.tbloid = c.conrelid)\n"
					  "WHERE contype = 'f' ",
					  tbloids->data);
	if (fout->remoteVersion >= 110000)
		appendPQExpBufferStr(query,
							 "AND conparentid = 0 ");
	appendPQExpBufferStr(query,
						 "ORDER BY conrelid, conname");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_contableoid = PQfnumber(res, "tableoid");
	i_conoid = PQfnumber(res, "oid");
	i_conrelid = PQfnumber(res, "conrelid");
	i_conname = PQfnumber(res, "conname");
	i_confrelid = PQfnumber(res, "confrelid");
	i_conindid = PQfnumber(res, "conindid");
	i_condef = PQfnumber(res, "condef");

	constrinfo = (ConstraintInfo *) pg_malloc(ntups * sizeof(ConstraintInfo));

	curtblindx = -1;
	for (int j = 0; j < ntups; j++)
	{
		Oid			conrelid = atooid(PQgetvalue(res, j, i_conrelid));
		TableInfo  *reftable;

		/*
		 * Locate the associated TableInfo; we rely on tblinfo[] being in OID
		 * order.
		 */
		if (tbinfo == NULL || tbinfo->dobj.catId.oid != conrelid)
		{
			while (++curtblindx < numTables)
			{
				tbinfo = &tblinfo[curtblindx];
				if (tbinfo->dobj.catId.oid == conrelid)
					break;
			}
			if (curtblindx >= numTables)
				pg_fatal("unrecognized table OID %u", conrelid);
		}

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
		 * Restoring an FK that points to a partitioned table requires that
		 * all partition indexes have been attached beforehand. Ensure that
		 * happens by making the constraint depend on each index partition
		 * attach object.
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

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(tbloids);
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
addConstrChildIdxDeps(DumpableObject *dobj, const IndxInfo *refidx)
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
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;
	int			i_tableoid,
				i_oid,
				i_conname,
				i_consrc;
	int			ntups;

	if (!fout->is_prepared[PREPQUERY_GETDOMAINCONSTRAINTS])
	{
		/* Set up query for constraint-specific details */
		appendPQExpBufferStr(query,
							 "PREPARE getDomainConstraints(pg_catalog.oid) AS\n"
							 "SELECT tableoid, oid, conname, "
							 "pg_catalog.pg_get_constraintdef(oid) AS consrc, "
							 "convalidated "
							 "FROM pg_catalog.pg_constraint "
							 "WHERE contypid = $1 AND contype = 'c' "
							 "ORDER BY conname");

		ExecuteSqlStatement(fout, query->data);

		fout->is_prepared[PREPQUERY_GETDOMAINCONSTRAINTS] = true;
	}

	printfPQExpBuffer(query,
					  "EXECUTE getDomainConstraints('%u')",
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
 */
void
getRules(Archive *fout)
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

	appendPQExpBufferStr(query, "SELECT "
						 "tableoid, oid, rulename, "
						 "ev_class AS ruletable, ev_type, is_instead, "
						 "ev_enabled "
						 "FROM pg_rewrite "
						 "ORDER BY oid");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

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
			pg_fatal("failed sanity check, parent table with OID %u of pg_rewrite entry with OID %u not found",
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
	PQExpBuffer query = createPQExpBuffer();
	PQExpBuffer tbloids = createPQExpBuffer();
	PGresult   *res;
	int			ntups;
	int			curtblindx;
	TriggerInfo *tginfo;
	int			i_tableoid,
				i_oid,
				i_tgrelid,
				i_tgname,
				i_tgenabled,
				i_tgispartition,
				i_tgdef;

	/*
	 * We want to perform just one query against pg_trigger.  However, we
	 * mustn't try to select every row of the catalog and then sort it out on
	 * the client side, because some of the server-side functions we need
	 * would be unsafe to apply to tables we don't have lock on.  Hence, we
	 * build an array of the OIDs of tables we care about (and now have lock
	 * on!), and use a WHERE clause to constrain which rows are selected.
	 */
	appendPQExpBufferChar(tbloids, '{');
	for (int i = 0; i < numTables; i++)
	{
		TableInfo  *tbinfo = &tblinfo[i];

		if (!tbinfo->hastriggers ||
			!(tbinfo->dobj.dump & DUMP_COMPONENT_DEFINITION))
			continue;

		/* OK, we need info for this table */
		if (tbloids->len > 1)	/* do we have more than the '{'? */
			appendPQExpBufferChar(tbloids, ',');
		appendPQExpBuffer(tbloids, "%u", tbinfo->dobj.catId.oid);
	}
	appendPQExpBufferChar(tbloids, '}');

	if (fout->remoteVersion >= 150000)
	{
		/*
		 * NB: think not to use pretty=true in pg_get_triggerdef.  It could
		 * result in non-forward-compatible dumps of WHEN clauses due to
		 * under-parenthesization.
		 *
		 * NB: We need to see partition triggers in case the tgenabled flag
		 * has been changed from the parent.
		 */
		appendPQExpBuffer(query,
						  "SELECT t.tgrelid, t.tgname, "
						  "pg_catalog.pg_get_triggerdef(t.oid, false) AS tgdef, "
						  "t.tgenabled, t.tableoid, t.oid, "
						  "t.tgparentid <> 0 AS tgispartition\n"
						  "FROM unnest('%s'::pg_catalog.oid[]) AS src(tbloid)\n"
						  "JOIN pg_catalog.pg_trigger t ON (src.tbloid = t.tgrelid) "
						  "LEFT JOIN pg_catalog.pg_trigger u ON (u.oid = t.tgparentid) "
						  "WHERE ((NOT t.tgisinternal AND t.tgparentid = 0) "
						  "OR t.tgenabled != u.tgenabled) "
						  "ORDER BY t.tgrelid, t.tgname",
						  tbloids->data);
	}
	else if (fout->remoteVersion >= 130000)
	{
		/*
		 * NB: think not to use pretty=true in pg_get_triggerdef.  It could
		 * result in non-forward-compatible dumps of WHEN clauses due to
		 * under-parenthesization.
		 *
		 * NB: We need to see tgisinternal triggers in partitions, in case the
		 * tgenabled flag has been changed from the parent.
		 */
		appendPQExpBuffer(query,
						  "SELECT t.tgrelid, t.tgname, "
						  "pg_catalog.pg_get_triggerdef(t.oid, false) AS tgdef, "
						  "t.tgenabled, t.tableoid, t.oid, t.tgisinternal as tgispartition\n"
						  "FROM unnest('%s'::pg_catalog.oid[]) AS src(tbloid)\n"
						  "JOIN pg_catalog.pg_trigger t ON (src.tbloid = t.tgrelid) "
						  "LEFT JOIN pg_catalog.pg_trigger u ON (u.oid = t.tgparentid) "
						  "WHERE (NOT t.tgisinternal OR t.tgenabled != u.tgenabled) "
						  "ORDER BY t.tgrelid, t.tgname",
						  tbloids->data);
	}
	else if (fout->remoteVersion >= 110000)
	{
		/*
		 * NB: We need to see tgisinternal triggers in partitions, in case the
		 * tgenabled flag has been changed from the parent. No tgparentid in
		 * version 11-12, so we have to match them via pg_depend.
		 *
		 * See above about pretty=true in pg_get_triggerdef.
		 */
		appendPQExpBuffer(query,
						  "SELECT t.tgrelid, t.tgname, "
						  "pg_catalog.pg_get_triggerdef(t.oid, false) AS tgdef, "
						  "t.tgenabled, t.tableoid, t.oid, t.tgisinternal as tgispartition "
						  "FROM unnest('%s'::pg_catalog.oid[]) AS src(tbloid)\n"
						  "JOIN pg_catalog.pg_trigger t ON (src.tbloid = t.tgrelid) "
						  "LEFT JOIN pg_catalog.pg_depend AS d ON "
						  " d.classid = 'pg_catalog.pg_trigger'::pg_catalog.regclass AND "
						  " d.refclassid = 'pg_catalog.pg_trigger'::pg_catalog.regclass AND "
						  " d.objid = t.oid "
						  "LEFT JOIN pg_catalog.pg_trigger AS pt ON pt.oid = refobjid "
						  "WHERE (NOT t.tgisinternal OR t.tgenabled != pt.tgenabled) "
						  "ORDER BY t.tgrelid, t.tgname",
						  tbloids->data);
	}
	else
	{
		/* See above about pretty=true in pg_get_triggerdef */
		appendPQExpBuffer(query,
						  "SELECT t.tgrelid, t.tgname, "
						  "pg_catalog.pg_get_triggerdef(t.oid, false) AS tgdef, "
						  "t.tgenabled, false as tgispartition, "
						  "t.tableoid, t.oid "
						  "FROM unnest('%s'::pg_catalog.oid[]) AS src(tbloid)\n"
						  "JOIN pg_catalog.pg_trigger t ON (src.tbloid = t.tgrelid) "
						  "WHERE NOT tgisinternal "
						  "ORDER BY t.tgrelid, t.tgname",
						  tbloids->data);
	}

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_tgrelid = PQfnumber(res, "tgrelid");
	i_tgname = PQfnumber(res, "tgname");
	i_tgenabled = PQfnumber(res, "tgenabled");
	i_tgispartition = PQfnumber(res, "tgispartition");
	i_tgdef = PQfnumber(res, "tgdef");

	tginfo = (TriggerInfo *) pg_malloc(ntups * sizeof(TriggerInfo));

	/*
	 * Outer loop iterates once per table, not once per row.  Incrementing of
	 * j is handled by the inner loop.
	 */
	curtblindx = -1;
	for (int j = 0; j < ntups;)
	{
		Oid			tgrelid = atooid(PQgetvalue(res, j, i_tgrelid));
		TableInfo  *tbinfo = NULL;
		int			numtrigs;

		/* Count rows for this table */
		for (numtrigs = 1; numtrigs < ntups - j; numtrigs++)
			if (atooid(PQgetvalue(res, j + numtrigs, i_tgrelid)) != tgrelid)
				break;

		/*
		 * Locate the associated TableInfo; we rely on tblinfo[] being in OID
		 * order.
		 */
		while (++curtblindx < numTables)
		{
			tbinfo = &tblinfo[curtblindx];
			if (tbinfo->dobj.catId.oid == tgrelid)
				break;
		}
		if (curtblindx >= numTables)
			pg_fatal("unrecognized table OID %u", tgrelid);

		/* Save data for this table */
		tbinfo->triggers = tginfo + j;
		tbinfo->numTriggers = numtrigs;

		for (int c = 0; c < numtrigs; c++, j++)
		{
			tginfo[j].dobj.objType = DO_TRIGGER;
			tginfo[j].dobj.catId.tableoid = atooid(PQgetvalue(res, j, i_tableoid));
			tginfo[j].dobj.catId.oid = atooid(PQgetvalue(res, j, i_oid));
			AssignDumpId(&tginfo[j].dobj);
			tginfo[j].dobj.name = pg_strdup(PQgetvalue(res, j, i_tgname));
			tginfo[j].dobj.namespace = tbinfo->dobj.namespace;
			tginfo[j].tgtable = tbinfo;
			tginfo[j].tgenabled = *(PQgetvalue(res, j, i_tgenabled));
			tginfo[j].tgispartition = *(PQgetvalue(res, j, i_tgispartition)) == 't';
			tginfo[j].tgdef = pg_strdup(PQgetvalue(res, j, i_tgdef));
		}
	}

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(tbloids);
}

/*
 * getEventTriggers
 *	  get information about event triggers
 */
void
getEventTriggers(Archive *fout)
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
		return;

	query = createPQExpBuffer();

	appendPQExpBufferStr(query,
						 "SELECT e.tableoid, e.oid, evtname, evtenabled, "
						 "evtevent, evtowner, "
						 "array_to_string(array("
						 "select quote_literal(x) "
						 " from unnest(evttags) as t(x)), ', ') as evttags, "
						 "e.evtfoid::regproc as evtfname "
						 "FROM pg_event_trigger e "
						 "ORDER BY e.oid");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

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
		evtinfo[i].evtowner = getRoleName(PQgetvalue(res, i, i_evtowner));
		evtinfo[i].evttags = pg_strdup(PQgetvalue(res, i, i_evttags));
		evtinfo[i].evtfname = pg_strdup(PQgetvalue(res, i, i_evtfname));
		evtinfo[i].evtenabled = *(PQgetvalue(res, i, i_evtenabled));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(evtinfo[i].dobj), fout);
	}

	PQclear(res);

	destroyPQExpBuffer(query);
}

/*
 * getProcLangs
 *	  get basic information about every procedural language in the system
 *
 * NB: this must run after getFuncs() because we assume we can do
 * findFuncByOid().
 */
void
getProcLangs(Archive *fout)
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
	int			i_laninline;
	int			i_lanvalidator;
	int			i_lanacl;
	int			i_acldefault;
	int			i_lanowner;

	appendPQExpBufferStr(query, "SELECT tableoid, oid, "
						 "lanname, lanpltrusted, lanplcallfoid, "
						 "laninline, lanvalidator, "
						 "lanacl, "
						 "acldefault('l', lanowner) AS acldefault, "
						 "lanowner "
						 "FROM pg_language "
						 "WHERE lanispl "
						 "ORDER BY oid");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	planginfo = (ProcLangInfo *) pg_malloc(ntups * sizeof(ProcLangInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_lanname = PQfnumber(res, "lanname");
	i_lanpltrusted = PQfnumber(res, "lanpltrusted");
	i_lanplcallfoid = PQfnumber(res, "lanplcallfoid");
	i_laninline = PQfnumber(res, "laninline");
	i_lanvalidator = PQfnumber(res, "lanvalidator");
	i_lanacl = PQfnumber(res, "lanacl");
	i_acldefault = PQfnumber(res, "acldefault");
	i_lanowner = PQfnumber(res, "lanowner");

	for (i = 0; i < ntups; i++)
	{
		planginfo[i].dobj.objType = DO_PROCLANG;
		planginfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		planginfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&planginfo[i].dobj);

		planginfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_lanname));
		planginfo[i].dacl.acl = pg_strdup(PQgetvalue(res, i, i_lanacl));
		planginfo[i].dacl.acldefault = pg_strdup(PQgetvalue(res, i, i_acldefault));
		planginfo[i].dacl.privtype = 0;
		planginfo[i].dacl.initprivs = NULL;
		planginfo[i].lanpltrusted = *(PQgetvalue(res, i, i_lanpltrusted)) == 't';
		planginfo[i].lanplcallfoid = atooid(PQgetvalue(res, i, i_lanplcallfoid));
		planginfo[i].laninline = atooid(PQgetvalue(res, i, i_laninline));
		planginfo[i].lanvalidator = atooid(PQgetvalue(res, i, i_lanvalidator));
		planginfo[i].lanowner = getRoleName(PQgetvalue(res, i, i_lanowner));

		/* Decide whether we want to dump it */
		selectDumpableProcLang(&(planginfo[i]), fout);

		/* Mark whether language has an ACL */
		if (!PQgetisnull(res, i, i_lanacl))
			planginfo[i].dobj.components |= DUMP_COMPONENT_ACL;
	}

	PQclear(res);

	destroyPQExpBuffer(query);
}

/*
 * getCasts
 *	  get basic information about most casts in the system
 *
 * Skip casts from a range to its multirange, since we'll create those
 * automatically.
 */
void
getCasts(Archive *fout)
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

	if (fout->remoteVersion >= 140000)
	{
		appendPQExpBufferStr(query, "SELECT tableoid, oid, "
							 "castsource, casttarget, castfunc, castcontext, "
							 "castmethod "
							 "FROM pg_cast c "
							 "WHERE NOT EXISTS ( "
							 "SELECT 1 FROM pg_range r "
							 "WHERE c.castsource = r.rngtypid "
							 "AND c.casttarget = r.rngmultitypid "
							 ") "
							 "ORDER BY 3,4");
	}
	else
	{
		appendPQExpBufferStr(query, "SELECT tableoid, oid, "
							 "castsource, casttarget, castfunc, castcontext, "
							 "castmethod "
							 "FROM pg_cast ORDER BY 3,4");
	}

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

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
	}

	PQclear(res);

	destroyPQExpBuffer(query);
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
 */
void
getTransforms(Archive *fout)
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
		return;

	query = createPQExpBuffer();

	appendPQExpBufferStr(query, "SELECT tableoid, oid, "
						 "trftype, trflang, trffromsql::oid, trftosql::oid "
						 "FROM pg_transform "
						 "ORDER BY 3,4");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

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
}

/*
 * getTableAttrs -
 *	  for each interesting table, read info about its attributes
 *	  (names, types, default values, CHECK constraints, etc)
 *
 *	modifies tblinfo
 */
void
getTableAttrs(Archive *fout, TableInfo *tblinfo, int numTables)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer tbloids = createPQExpBuffer();
	PQExpBuffer checkoids = createPQExpBuffer();
	PGresult   *res;
	int			ntups;
	int			curtblindx;
	int			i_attrelid;
	int			i_attnum;
	int			i_attname;
	int			i_atttypname;
	int			i_attstattarget;
	int			i_attstorage;
	int			i_typstorage;
	int			i_attidentity;
	int			i_attgenerated;
	int			i_attisdropped;
	int			i_attlen;
	int			i_attalign;
	int			i_attislocal;
	int			i_notnull_name;
	int			i_notnull_noinherit;
	int			i_notnull_islocal;
	int			i_attoptions;
	int			i_attcollation;
	int			i_attcompression;
	int			i_attfdwoptions;
	int			i_attmissingval;
	int			i_atthasdef;

	/*
	 * We want to perform just one query against pg_attribute, and then just
	 * one against pg_attrdef (for DEFAULTs) and two against pg_constraint
	 * (for CHECK constraints and for NOT NULL constraints).  However, we
	 * mustn't try to select every row of those catalogs and then sort it out
	 * on the client side, because some of the server-side functions we need
	 * would be unsafe to apply to tables we don't have lock on.  Hence, we
	 * build an array of the OIDs of tables we care about (and now have lock
	 * on!), and use a WHERE clause to constrain which rows are selected.
	 */
	appendPQExpBufferChar(tbloids, '{');
	appendPQExpBufferChar(checkoids, '{');
	for (int i = 0; i < numTables; i++)
	{
		TableInfo  *tbinfo = &tblinfo[i];

		/* Don't bother to collect info for sequences */
		if (tbinfo->relkind == RELKIND_SEQUENCE)
			continue;

		/* Don't bother with uninteresting tables, either */
		if (!tbinfo->interesting)
			continue;

		/* OK, we need info for this table */
		if (tbloids->len > 1)	/* do we have more than the '{'? */
			appendPQExpBufferChar(tbloids, ',');
		appendPQExpBuffer(tbloids, "%u", tbinfo->dobj.catId.oid);

		if (tbinfo->ncheck > 0)
		{
			/* Also make a list of the ones with check constraints */
			if (checkoids->len > 1) /* do we have more than the '{'? */
				appendPQExpBufferChar(checkoids, ',');
			appendPQExpBuffer(checkoids, "%u", tbinfo->dobj.catId.oid);
		}
	}
	appendPQExpBufferChar(tbloids, '}');
	appendPQExpBufferChar(checkoids, '}');

	/*
	 * Find all the user attributes and their types.
	 *
	 * Since we only want to dump COLLATE clauses for attributes whose
	 * collation is different from their type's default, we use a CASE here to
	 * suppress uninteresting attcollations cheaply.
	 */
	appendPQExpBufferStr(q,
						 "SELECT\n"
						 "a.attrelid,\n"
						 "a.attnum,\n"
						 "a.attname,\n"
						 "a.attstattarget,\n"
						 "a.attstorage,\n"
						 "t.typstorage,\n"
						 "a.atthasdef,\n"
						 "a.attisdropped,\n"
						 "a.attlen,\n"
						 "a.attalign,\n"
						 "a.attislocal,\n"
						 "pg_catalog.format_type(t.oid, a.atttypmod) AS atttypname,\n"
						 "array_to_string(a.attoptions, ', ') AS attoptions,\n"
						 "CASE WHEN a.attcollation <> t.typcollation "
						 "THEN a.attcollation ELSE 0 END AS attcollation,\n"
						 "pg_catalog.array_to_string(ARRAY("
						 "SELECT pg_catalog.quote_ident(option_name) || "
						 "' ' || pg_catalog.quote_literal(option_value) "
						 "FROM pg_catalog.pg_options_to_table(attfdwoptions) "
						 "ORDER BY option_name"
						 "), E',\n    ') AS attfdwoptions,\n");

	/*
	 * Find out any NOT NULL markings for each column.  In 18 and up we read
	 * pg_constraint to obtain the constraint name.  notnull_noinherit is set
	 * according to the NO INHERIT property.  For versions prior to 18, we
	 * store an empty string as the name when a constraint is marked as
	 * attnotnull (this cues dumpTableSchema to print the NOT NULL clause
	 * without a name); also, such cases are never NO INHERIT.
	 *
	 * We track in notnull_islocal whether the constraint was defined directly
	 * in this table or via an ancestor, for binary upgrade.  flagInhAttrs
	 * might modify this later for servers older than 18; it's also in charge
	 * of determining the correct inhcount.
	 */
	if (fout->remoteVersion >= 180000)
		appendPQExpBufferStr(q,
							 "co.conname AS notnull_name,\n"
							 "co.connoinherit AS notnull_noinherit,\n"
							 "co.conislocal AS notnull_islocal,\n");
	else
		appendPQExpBufferStr(q,
							 "CASE WHEN a.attnotnull THEN '' ELSE NULL END AS notnull_name,\n"
							 "false AS notnull_noinherit,\n"
							 "a.attislocal AS notnull_islocal,\n");

	if (fout->remoteVersion >= 140000)
		appendPQExpBufferStr(q,
							 "a.attcompression AS attcompression,\n");
	else
		appendPQExpBufferStr(q,
							 "'' AS attcompression,\n");

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

	/* need left join to pg_type to not fail on dropped columns ... */
	appendPQExpBuffer(q,
					  "FROM unnest('%s'::pg_catalog.oid[]) AS src(tbloid)\n"
					  "JOIN pg_catalog.pg_attribute a ON (src.tbloid = a.attrelid) "
					  "LEFT JOIN pg_catalog.pg_type t "
					  "ON (a.atttypid = t.oid)\n",
					  tbloids->data);

	/*
	 * In versions 18 and up, we need pg_constraint for explicit NOT NULL
	 * entries.  Also, we need to know if the NOT NULL for each column is
	 * backing a primary key.
	 */
	if (fout->remoteVersion >= 180000)
		appendPQExpBufferStr(q,
							 " LEFT JOIN pg_catalog.pg_constraint co ON "
							 "(a.attrelid = co.conrelid\n"
							 "   AND co.contype = 'n' AND "
							 "co.conkey = array[a.attnum])\n");

	appendPQExpBufferStr(q,
						 "WHERE a.attnum > 0::pg_catalog.int2\n"
						 "ORDER BY a.attrelid, a.attnum");

	res = ExecuteSqlQuery(fout, q->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_attrelid = PQfnumber(res, "attrelid");
	i_attnum = PQfnumber(res, "attnum");
	i_attname = PQfnumber(res, "attname");
	i_atttypname = PQfnumber(res, "atttypname");
	i_attstattarget = PQfnumber(res, "attstattarget");
	i_attstorage = PQfnumber(res, "attstorage");
	i_typstorage = PQfnumber(res, "typstorage");
	i_attidentity = PQfnumber(res, "attidentity");
	i_attgenerated = PQfnumber(res, "attgenerated");
	i_attisdropped = PQfnumber(res, "attisdropped");
	i_attlen = PQfnumber(res, "attlen");
	i_attalign = PQfnumber(res, "attalign");
	i_attislocal = PQfnumber(res, "attislocal");
	i_notnull_name = PQfnumber(res, "notnull_name");
	i_notnull_noinherit = PQfnumber(res, "notnull_noinherit");
	i_notnull_islocal = PQfnumber(res, "notnull_islocal");
	i_attoptions = PQfnumber(res, "attoptions");
	i_attcollation = PQfnumber(res, "attcollation");
	i_attcompression = PQfnumber(res, "attcompression");
	i_attfdwoptions = PQfnumber(res, "attfdwoptions");
	i_attmissingval = PQfnumber(res, "attmissingval");
	i_atthasdef = PQfnumber(res, "atthasdef");

	/* Within the next loop, we'll accumulate OIDs of tables with defaults */
	resetPQExpBuffer(tbloids);
	appendPQExpBufferChar(tbloids, '{');

	/*
	 * Outer loop iterates once per table, not once per row.  Incrementing of
	 * r is handled by the inner loop.
	 */
	curtblindx = -1;
	for (int r = 0; r < ntups;)
	{
		Oid			attrelid = atooid(PQgetvalue(res, r, i_attrelid));
		TableInfo  *tbinfo = NULL;
		int			numatts;
		bool		hasdefaults;

		/* Count rows for this table */
		for (numatts = 1; numatts < ntups - r; numatts++)
			if (atooid(PQgetvalue(res, r + numatts, i_attrelid)) != attrelid)
				break;

		/*
		 * Locate the associated TableInfo; we rely on tblinfo[] being in OID
		 * order.
		 */
		while (++curtblindx < numTables)
		{
			tbinfo = &tblinfo[curtblindx];
			if (tbinfo->dobj.catId.oid == attrelid)
				break;
		}
		if (curtblindx >= numTables)
			pg_fatal("unrecognized table OID %u", attrelid);
		/* cross-check that we only got requested tables */
		if (tbinfo->relkind == RELKIND_SEQUENCE ||
			!tbinfo->interesting)
			pg_fatal("unexpected column data for table \"%s\"",
					 tbinfo->dobj.name);

		/* Save data for this table */
		tbinfo->numatts = numatts;
		tbinfo->attnames = (char **) pg_malloc(numatts * sizeof(char *));
		tbinfo->atttypnames = (char **) pg_malloc(numatts * sizeof(char *));
		tbinfo->attstattarget = (int *) pg_malloc(numatts * sizeof(int));
		tbinfo->attstorage = (char *) pg_malloc(numatts * sizeof(char));
		tbinfo->typstorage = (char *) pg_malloc(numatts * sizeof(char));
		tbinfo->attidentity = (char *) pg_malloc(numatts * sizeof(char));
		tbinfo->attgenerated = (char *) pg_malloc(numatts * sizeof(char));
		tbinfo->attisdropped = (bool *) pg_malloc(numatts * sizeof(bool));
		tbinfo->attlen = (int *) pg_malloc(numatts * sizeof(int));
		tbinfo->attalign = (char *) pg_malloc(numatts * sizeof(char));
		tbinfo->attislocal = (bool *) pg_malloc(numatts * sizeof(bool));
		tbinfo->attoptions = (char **) pg_malloc(numatts * sizeof(char *));
		tbinfo->attcollation = (Oid *) pg_malloc(numatts * sizeof(Oid));
		tbinfo->attcompression = (char *) pg_malloc(numatts * sizeof(char));
		tbinfo->attfdwoptions = (char **) pg_malloc(numatts * sizeof(char *));
		tbinfo->attmissingval = (char **) pg_malloc(numatts * sizeof(char *));
		tbinfo->notnull_constrs = (char **) pg_malloc(numatts * sizeof(char *));
		tbinfo->notnull_noinh = (bool *) pg_malloc(numatts * sizeof(bool));
		tbinfo->notnull_islocal = (bool *) pg_malloc(numatts * sizeof(bool));
		tbinfo->attrdefs = (AttrDefInfo **) pg_malloc(numatts * sizeof(AttrDefInfo *));
		hasdefaults = false;

		for (int j = 0; j < numatts; j++, r++)
		{
			if (j + 1 != atoi(PQgetvalue(res, r, i_attnum)))
				pg_fatal("invalid column numbering in table \"%s\"",
						 tbinfo->dobj.name);
			tbinfo->attnames[j] = pg_strdup(PQgetvalue(res, r, i_attname));
			tbinfo->atttypnames[j] = pg_strdup(PQgetvalue(res, r, i_atttypname));
			if (PQgetisnull(res, r, i_attstattarget))
				tbinfo->attstattarget[j] = -1;
			else
				tbinfo->attstattarget[j] = atoi(PQgetvalue(res, r, i_attstattarget));
			tbinfo->attstorage[j] = *(PQgetvalue(res, r, i_attstorage));
			tbinfo->typstorage[j] = *(PQgetvalue(res, r, i_typstorage));
			tbinfo->attidentity[j] = *(PQgetvalue(res, r, i_attidentity));
			tbinfo->attgenerated[j] = *(PQgetvalue(res, r, i_attgenerated));
			tbinfo->needs_override = tbinfo->needs_override || (tbinfo->attidentity[j] == ATTRIBUTE_IDENTITY_ALWAYS);
			tbinfo->attisdropped[j] = (PQgetvalue(res, r, i_attisdropped)[0] == 't');
			tbinfo->attlen[j] = atoi(PQgetvalue(res, r, i_attlen));
			tbinfo->attalign[j] = *(PQgetvalue(res, r, i_attalign));
			tbinfo->attislocal[j] = (PQgetvalue(res, r, i_attislocal)[0] == 't');

			/* Handle not-null constraint name and flags */
			determineNotNullFlags(fout, res, r,
								  tbinfo, j,
								  i_notnull_name, i_notnull_noinherit,
								  i_notnull_islocal);

			tbinfo->attoptions[j] = pg_strdup(PQgetvalue(res, r, i_attoptions));
			tbinfo->attcollation[j] = atooid(PQgetvalue(res, r, i_attcollation));
			tbinfo->attcompression[j] = *(PQgetvalue(res, r, i_attcompression));
			tbinfo->attfdwoptions[j] = pg_strdup(PQgetvalue(res, r, i_attfdwoptions));
			tbinfo->attmissingval[j] = pg_strdup(PQgetvalue(res, r, i_attmissingval));
			tbinfo->attrdefs[j] = NULL; /* fix below */
			if (PQgetvalue(res, r, i_atthasdef)[0] == 't')
				hasdefaults = true;
		}

		if (hasdefaults)
		{
			/* Collect OIDs of interesting tables that have defaults */
			if (tbloids->len > 1)	/* do we have more than the '{'? */
				appendPQExpBufferChar(tbloids, ',');
			appendPQExpBuffer(tbloids, "%u", tbinfo->dobj.catId.oid);
		}
	}

	PQclear(res);

	/*
	 * Now get info about column defaults.  This is skipped for a data-only
	 * dump, as it is only needed for table schemas.
	 */
	if (dopt->dumpSchema && tbloids->len > 1)
	{
		AttrDefInfo *attrdefs;
		int			numDefaults;
		TableInfo  *tbinfo = NULL;

		pg_log_info("finding table default expressions");

		appendPQExpBufferChar(tbloids, '}');

		printfPQExpBuffer(q, "SELECT a.tableoid, a.oid, adrelid, adnum, "
						  "pg_catalog.pg_get_expr(adbin, adrelid) AS adsrc\n"
						  "FROM unnest('%s'::pg_catalog.oid[]) AS src(tbloid)\n"
						  "JOIN pg_catalog.pg_attrdef a ON (src.tbloid = a.adrelid)\n"
						  "ORDER BY a.adrelid, a.adnum",
						  tbloids->data);

		res = ExecuteSqlQuery(fout, q->data, PGRES_TUPLES_OK);

		numDefaults = PQntuples(res);
		attrdefs = (AttrDefInfo *) pg_malloc(numDefaults * sizeof(AttrDefInfo));

		curtblindx = -1;
		for (int j = 0; j < numDefaults; j++)
		{
			Oid			adtableoid = atooid(PQgetvalue(res, j, 0));
			Oid			adoid = atooid(PQgetvalue(res, j, 1));
			Oid			adrelid = atooid(PQgetvalue(res, j, 2));
			int			adnum = atoi(PQgetvalue(res, j, 3));
			char	   *adsrc = PQgetvalue(res, j, 4);

			/*
			 * Locate the associated TableInfo; we rely on tblinfo[] being in
			 * OID order.
			 */
			if (tbinfo == NULL || tbinfo->dobj.catId.oid != adrelid)
			{
				while (++curtblindx < numTables)
				{
					tbinfo = &tblinfo[curtblindx];
					if (tbinfo->dobj.catId.oid == adrelid)
						break;
				}
				if (curtblindx >= numTables)
					pg_fatal("unrecognized table OID %u", adrelid);
			}

			if (adnum <= 0 || adnum > tbinfo->numatts)
				pg_fatal("invalid adnum value %d for table \"%s\"",
						 adnum, tbinfo->dobj.name);

			/*
			 * dropped columns shouldn't have defaults, but just in case,
			 * ignore 'em
			 */
			if (tbinfo->attisdropped[adnum - 1])
				continue;

			attrdefs[j].dobj.objType = DO_ATTRDEF;
			attrdefs[j].dobj.catId.tableoid = adtableoid;
			attrdefs[j].dobj.catId.oid = adoid;
			AssignDumpId(&attrdefs[j].dobj);
			attrdefs[j].adtable = tbinfo;
			attrdefs[j].adnum = adnum;
			attrdefs[j].adef_expr = pg_strdup(adsrc);

			attrdefs[j].dobj.name = pg_strdup(tbinfo->dobj.name);
			attrdefs[j].dobj.namespace = tbinfo->dobj.namespace;

			attrdefs[j].dobj.dump = tbinfo->dobj.dump;

			/*
			 * Figure out whether the default/generation expression should be
			 * dumped as part of the main CREATE TABLE (or similar) command or
			 * as a separate ALTER TABLE (or similar) command. The preference
			 * is to put it into the CREATE command, but in some cases that's
			 * not possible.
			 */
			if (tbinfo->attgenerated[adnum - 1])
			{
				/*
				 * Column generation expressions cannot be dumped separately,
				 * because there is no syntax for it.  By setting separate to
				 * false here we prevent the "default" from being processed as
				 * its own dumpable object.  Later, flagInhAttrs() will mark
				 * it as not to be dumped at all, if possible (that is, if it
				 * can be inherited from a parent).
				 */
				attrdefs[j].separate = false;
			}
			else if (tbinfo->relkind == RELKIND_VIEW)
			{
				/*
				 * Defaults on a VIEW must always be dumped as separate ALTER
				 * TABLE commands.
				 */
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
			}

			if (!attrdefs[j].separate)
			{
				/*
				 * Mark the default as needing to appear before the table, so
				 * that any dependencies it has must be emitted before the
				 * CREATE TABLE.  If this is not possible, we'll change to
				 * "separate" mode while sorting dependencies.
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
	if (dopt->dumpSchema && checkoids->len > 2)
	{
		ConstraintInfo *constrs;
		int			numConstrs;
		int			i_tableoid;
		int			i_oid;
		int			i_conrelid;
		int			i_conname;
		int			i_consrc;
		int			i_conislocal;
		int			i_convalidated;

		pg_log_info("finding table check constraints");

		resetPQExpBuffer(q);
		appendPQExpBuffer(q,
						  "SELECT c.tableoid, c.oid, conrelid, conname, "
						  "pg_catalog.pg_get_constraintdef(c.oid) AS consrc, "
						  "conislocal, convalidated "
						  "FROM unnest('%s'::pg_catalog.oid[]) AS src(tbloid)\n"
						  "JOIN pg_catalog.pg_constraint c ON (src.tbloid = c.conrelid)\n"
						  "WHERE contype = 'c' "
						  "ORDER BY c.conrelid, c.conname",
						  checkoids->data);

		res = ExecuteSqlQuery(fout, q->data, PGRES_TUPLES_OK);

		numConstrs = PQntuples(res);
		constrs = (ConstraintInfo *) pg_malloc(numConstrs * sizeof(ConstraintInfo));

		i_tableoid = PQfnumber(res, "tableoid");
		i_oid = PQfnumber(res, "oid");
		i_conrelid = PQfnumber(res, "conrelid");
		i_conname = PQfnumber(res, "conname");
		i_consrc = PQfnumber(res, "consrc");
		i_conislocal = PQfnumber(res, "conislocal");
		i_convalidated = PQfnumber(res, "convalidated");

		/* As above, this loop iterates once per table, not once per row */
		curtblindx = -1;
		for (int j = 0; j < numConstrs;)
		{
			Oid			conrelid = atooid(PQgetvalue(res, j, i_conrelid));
			TableInfo  *tbinfo = NULL;
			int			numcons;

			/* Count rows for this table */
			for (numcons = 1; numcons < numConstrs - j; numcons++)
				if (atooid(PQgetvalue(res, j + numcons, i_conrelid)) != conrelid)
					break;

			/*
			 * Locate the associated TableInfo; we rely on tblinfo[] being in
			 * OID order.
			 */
			while (++curtblindx < numTables)
			{
				tbinfo = &tblinfo[curtblindx];
				if (tbinfo->dobj.catId.oid == conrelid)
					break;
			}
			if (curtblindx >= numTables)
				pg_fatal("unrecognized table OID %u", conrelid);

			if (numcons != tbinfo->ncheck)
			{
				pg_log_error(ngettext("expected %d check constraint on table \"%s\" but found %d",
									  "expected %d check constraints on table \"%s\" but found %d",
									  tbinfo->ncheck),
							 tbinfo->ncheck, tbinfo->dobj.name, numcons);
				pg_log_error_hint("The system catalogs might be corrupted.");
				exit_nicely(1);
			}

			tbinfo->checkexprs = constrs + j;

			for (int c = 0; c < numcons; c++, j++)
			{
				bool		validated = PQgetvalue(res, j, i_convalidated)[0] == 't';

				constrs[j].dobj.objType = DO_CONSTRAINT;
				constrs[j].dobj.catId.tableoid = atooid(PQgetvalue(res, j, i_tableoid));
				constrs[j].dobj.catId.oid = atooid(PQgetvalue(res, j, i_oid));
				AssignDumpId(&constrs[j].dobj);
				constrs[j].dobj.name = pg_strdup(PQgetvalue(res, j, i_conname));
				constrs[j].dobj.namespace = tbinfo->dobj.namespace;
				constrs[j].contable = tbinfo;
				constrs[j].condomain = NULL;
				constrs[j].contype = 'c';
				constrs[j].condef = pg_strdup(PQgetvalue(res, j, i_consrc));
				constrs[j].confrelid = InvalidOid;
				constrs[j].conindex = 0;
				constrs[j].condeferrable = false;
				constrs[j].condeferred = false;
				constrs[j].conislocal = (PQgetvalue(res, j, i_conislocal)[0] == 't');

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
				 * We will detect later whether the constraint must be split
				 * out from the table definition.
				 */
			}
		}

		PQclear(res);
	}

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(tbloids);
	destroyPQExpBuffer(checkoids);
}

/*
 * Based on the getTableAttrs query's row corresponding to one column, set
 * the name and flags to handle a not-null constraint for that column in
 * the tbinfo struct.
 *
 * Result row 'r' is for tbinfo's attribute 'j'.
 *
 * There are three possibilities:
 * 1) the column has no not-null constraints. In that case, ->notnull_constrs
 *    (the constraint name) remains NULL.
 * 2) The column has a constraint with no name (this is the case when
 *    constraints come from pre-18 servers).  In this case, ->notnull_constrs
 *    is set to the empty string; dumpTableSchema will print just "NOT NULL".
 * 3) The column has a constraint with a known name; in that case
 *    notnull_constrs carries that name and dumpTableSchema will print
 *    "CONSTRAINT the_name NOT NULL".  However, if the name is the default
 *    (table_column_not_null), there's no need to print that name in the dump,
 *    so notnull_constrs is set to the empty string and it behaves as the case
 *    above.
 *
 * In a child table that inherits from a parent already containing NOT NULL
 * constraints and the columns in the child don't have their own NOT NULL
 * declarations, we suppress printing constraints in the child: the
 * constraints are acquired at the point where the child is attached to the
 * parent.  This is tracked in ->notnull_islocal (which is set in flagInhAttrs
 * for servers pre-18).
 *
 * Any of these constraints might have the NO INHERIT bit.  If so we set
 * ->notnull_noinh and NO INHERIT will be printed by dumpTableSchema.
 *
 * In case 3 above, the name comparison is a bit of a hack; it actually fails
 * to do the right thing in all but the trivial case.  However, the downside
 * of getting it wrong is simply that the name is printed rather than
 * suppressed, so it's not a big deal.
 */
static void
determineNotNullFlags(Archive *fout, PGresult *res, int r,
					  TableInfo *tbinfo, int j,
					  int i_notnull_name, int i_notnull_noinherit,
					  int i_notnull_islocal)
{
	DumpOptions *dopt = fout->dopt;

	/*
	 * notnull_noinh is straight from the query result. notnull_islocal also,
	 * though flagInhAttrs may change that one later in versions < 18.
	 */
	tbinfo->notnull_noinh[j] = PQgetvalue(res, r, i_notnull_noinherit)[0] == 't';
	tbinfo->notnull_islocal[j] = PQgetvalue(res, r, i_notnull_islocal)[0] == 't';

	/*
	 * Determine a constraint name to use.  If the column is not marked not-
	 * null, we set NULL which cues ... to do nothing.  An empty string says
	 * to print an unnamed NOT NULL, and anything else is a constraint name to
	 * use.
	 */
	if (fout->remoteVersion < 180000)
	{
		/*
		 * < 18 doesn't have not-null names, so an unnamed constraint is
		 * sufficient.
		 */
		if (PQgetisnull(res, r, i_notnull_name))
			tbinfo->notnull_constrs[j] = NULL;
		else
			tbinfo->notnull_constrs[j] = "";
	}
	else
	{
		if (PQgetisnull(res, r, i_notnull_name))
			tbinfo->notnull_constrs[j] = NULL;
		else
		{
			/*
			 * In binary upgrade of inheritance child tables, must have a
			 * constraint name that we can UPDATE later.
			 */
			if (dopt->binary_upgrade &&
				!tbinfo->ispartition &&
				!tbinfo->notnull_islocal)
			{
				tbinfo->notnull_constrs[j] =
					pstrdup(PQgetvalue(res, r, i_notnull_name));
			}
			else
			{
				char	   *default_name;

				/* XXX should match ChooseConstraintName better */
				default_name = psprintf("%s_%s_not_null", tbinfo->dobj.name,
										tbinfo->attnames[j]);
				if (strcmp(default_name,
						   PQgetvalue(res, r, i_notnull_name)) == 0)
					tbinfo->notnull_constrs[j] = "";
				else
				{
					tbinfo->notnull_constrs[j] =
						pstrdup(PQgetvalue(res, r, i_notnull_name));
				}
				free(default_name);
			}
		}
	}
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
shouldPrintColumn(const DumpOptions *dopt, const TableInfo *tbinfo, int colno)
{
	if (dopt->binary_upgrade)
		return true;
	if (tbinfo->attisdropped[colno])
		return false;
	return (tbinfo->attislocal[colno] || tbinfo->ispartition);
}


/*
 * getTSParsers:
 *	  get information about all text search parsers in the system catalogs
 */
void
getTSParsers(Archive *fout)
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
	}

	PQclear(res);

	destroyPQExpBuffer(query);
}

/*
 * getTSDictionaries:
 *	  get information about all text search dictionaries in the system catalogs
 */
void
getTSDictionaries(Archive *fout)
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
	int			i_dictowner;
	int			i_dicttemplate;
	int			i_dictinitoption;

	query = createPQExpBuffer();

	appendPQExpBufferStr(query, "SELECT tableoid, oid, dictname, "
						 "dictnamespace, dictowner, "
						 "dicttemplate, dictinitoption "
						 "FROM pg_ts_dict");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	dictinfo = (TSDictInfo *) pg_malloc(ntups * sizeof(TSDictInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_dictname = PQfnumber(res, "dictname");
	i_dictnamespace = PQfnumber(res, "dictnamespace");
	i_dictowner = PQfnumber(res, "dictowner");
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
		dictinfo[i].rolname = getRoleName(PQgetvalue(res, i, i_dictowner));
		dictinfo[i].dicttemplate = atooid(PQgetvalue(res, i, i_dicttemplate));
		if (PQgetisnull(res, i, i_dictinitoption))
			dictinfo[i].dictinitoption = NULL;
		else
			dictinfo[i].dictinitoption = pg_strdup(PQgetvalue(res, i, i_dictinitoption));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(dictinfo[i].dobj), fout);
	}

	PQclear(res);

	destroyPQExpBuffer(query);
}

/*
 * getTSTemplates:
 *	  get information about all text search templates in the system catalogs
 */
void
getTSTemplates(Archive *fout)
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

	query = createPQExpBuffer();

	appendPQExpBufferStr(query, "SELECT tableoid, oid, tmplname, "
						 "tmplnamespace, tmplinit::oid, tmpllexize::oid "
						 "FROM pg_ts_template");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

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
	}

	PQclear(res);

	destroyPQExpBuffer(query);
}

/*
 * getTSConfigurations:
 *	  get information about all text search configurations
 */
void
getTSConfigurations(Archive *fout)
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
	int			i_cfgowner;
	int			i_cfgparser;

	query = createPQExpBuffer();

	appendPQExpBufferStr(query, "SELECT tableoid, oid, cfgname, "
						 "cfgnamespace, cfgowner, cfgparser "
						 "FROM pg_ts_config");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	cfginfo = (TSConfigInfo *) pg_malloc(ntups * sizeof(TSConfigInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_cfgname = PQfnumber(res, "cfgname");
	i_cfgnamespace = PQfnumber(res, "cfgnamespace");
	i_cfgowner = PQfnumber(res, "cfgowner");
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
		cfginfo[i].rolname = getRoleName(PQgetvalue(res, i, i_cfgowner));
		cfginfo[i].cfgparser = atooid(PQgetvalue(res, i, i_cfgparser));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(cfginfo[i].dobj), fout);
	}

	PQclear(res);

	destroyPQExpBuffer(query);
}

/*
 * getForeignDataWrappers:
 *	  get information about all foreign-data wrappers in the system catalogs
 */
void
getForeignDataWrappers(Archive *fout)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query;
	FdwInfo    *fdwinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_fdwname;
	int			i_fdwowner;
	int			i_fdwhandler;
	int			i_fdwvalidator;
	int			i_fdwacl;
	int			i_acldefault;
	int			i_fdwoptions;

	query = createPQExpBuffer();

	appendPQExpBufferStr(query, "SELECT tableoid, oid, fdwname, "
						 "fdwowner, "
						 "fdwhandler::pg_catalog.regproc, "
						 "fdwvalidator::pg_catalog.regproc, "
						 "fdwacl, "
						 "acldefault('F', fdwowner) AS acldefault, "
						 "array_to_string(ARRAY("
						 "SELECT quote_ident(option_name) || ' ' || "
						 "quote_literal(option_value) "
						 "FROM pg_options_to_table(fdwoptions) "
						 "ORDER BY option_name"
						 "), E',\n    ') AS fdwoptions "
						 "FROM pg_foreign_data_wrapper");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	fdwinfo = (FdwInfo *) pg_malloc(ntups * sizeof(FdwInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_fdwname = PQfnumber(res, "fdwname");
	i_fdwowner = PQfnumber(res, "fdwowner");
	i_fdwhandler = PQfnumber(res, "fdwhandler");
	i_fdwvalidator = PQfnumber(res, "fdwvalidator");
	i_fdwacl = PQfnumber(res, "fdwacl");
	i_acldefault = PQfnumber(res, "acldefault");
	i_fdwoptions = PQfnumber(res, "fdwoptions");

	for (i = 0; i < ntups; i++)
	{
		fdwinfo[i].dobj.objType = DO_FDW;
		fdwinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		fdwinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&fdwinfo[i].dobj);
		fdwinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_fdwname));
		fdwinfo[i].dobj.namespace = NULL;
		fdwinfo[i].dacl.acl = pg_strdup(PQgetvalue(res, i, i_fdwacl));
		fdwinfo[i].dacl.acldefault = pg_strdup(PQgetvalue(res, i, i_acldefault));
		fdwinfo[i].dacl.privtype = 0;
		fdwinfo[i].dacl.initprivs = NULL;
		fdwinfo[i].rolname = getRoleName(PQgetvalue(res, i, i_fdwowner));
		fdwinfo[i].fdwhandler = pg_strdup(PQgetvalue(res, i, i_fdwhandler));
		fdwinfo[i].fdwvalidator = pg_strdup(PQgetvalue(res, i, i_fdwvalidator));
		fdwinfo[i].fdwoptions = pg_strdup(PQgetvalue(res, i, i_fdwoptions));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(fdwinfo[i].dobj), fout);

		/* Mark whether FDW has an ACL */
		if (!PQgetisnull(res, i, i_fdwacl))
			fdwinfo[i].dobj.components |= DUMP_COMPONENT_ACL;
	}

	PQclear(res);

	destroyPQExpBuffer(query);
}

/*
 * getForeignServers:
 *	  get information about all foreign servers in the system catalogs
 */
void
getForeignServers(Archive *fout)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query;
	ForeignServerInfo *srvinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_srvname;
	int			i_srvowner;
	int			i_srvfdw;
	int			i_srvtype;
	int			i_srvversion;
	int			i_srvacl;
	int			i_acldefault;
	int			i_srvoptions;

	query = createPQExpBuffer();

	appendPQExpBufferStr(query, "SELECT tableoid, oid, srvname, "
						 "srvowner, "
						 "srvfdw, srvtype, srvversion, srvacl, "
						 "acldefault('S', srvowner) AS acldefault, "
						 "array_to_string(ARRAY("
						 "SELECT quote_ident(option_name) || ' ' || "
						 "quote_literal(option_value) "
						 "FROM pg_options_to_table(srvoptions) "
						 "ORDER BY option_name"
						 "), E',\n    ') AS srvoptions "
						 "FROM pg_foreign_server");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	srvinfo = (ForeignServerInfo *) pg_malloc(ntups * sizeof(ForeignServerInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_srvname = PQfnumber(res, "srvname");
	i_srvowner = PQfnumber(res, "srvowner");
	i_srvfdw = PQfnumber(res, "srvfdw");
	i_srvtype = PQfnumber(res, "srvtype");
	i_srvversion = PQfnumber(res, "srvversion");
	i_srvacl = PQfnumber(res, "srvacl");
	i_acldefault = PQfnumber(res, "acldefault");
	i_srvoptions = PQfnumber(res, "srvoptions");

	for (i = 0; i < ntups; i++)
	{
		srvinfo[i].dobj.objType = DO_FOREIGN_SERVER;
		srvinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		srvinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&srvinfo[i].dobj);
		srvinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_srvname));
		srvinfo[i].dobj.namespace = NULL;
		srvinfo[i].dacl.acl = pg_strdup(PQgetvalue(res, i, i_srvacl));
		srvinfo[i].dacl.acldefault = pg_strdup(PQgetvalue(res, i, i_acldefault));
		srvinfo[i].dacl.privtype = 0;
		srvinfo[i].dacl.initprivs = NULL;
		srvinfo[i].rolname = getRoleName(PQgetvalue(res, i, i_srvowner));
		srvinfo[i].srvfdw = atooid(PQgetvalue(res, i, i_srvfdw));
		srvinfo[i].srvtype = pg_strdup(PQgetvalue(res, i, i_srvtype));
		srvinfo[i].srvversion = pg_strdup(PQgetvalue(res, i, i_srvversion));
		srvinfo[i].srvoptions = pg_strdup(PQgetvalue(res, i, i_srvoptions));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(srvinfo[i].dobj), fout);

		/* Servers have user mappings */
		srvinfo[i].dobj.components |= DUMP_COMPONENT_USERMAP;

		/* Mark whether server has an ACL */
		if (!PQgetisnull(res, i, i_srvacl))
			srvinfo[i].dobj.components |= DUMP_COMPONENT_ACL;
	}

	PQclear(res);

	destroyPQExpBuffer(query);
}

/*
 * getDefaultACLs:
 *	  get information about all default ACL information in the system catalogs
 */
void
getDefaultACLs(Archive *fout)
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
	int			i_acldefault;
	int			i,
				ntups;

	query = createPQExpBuffer();

	/*
	 * Global entries (with defaclnamespace=0) replace the hard-wired default
	 * ACL for their object type.  We should dump them as deltas from the
	 * default ACL, since that will be used as a starting point for
	 * interpreting the ALTER DEFAULT PRIVILEGES commands.  On the other hand,
	 * non-global entries can only add privileges not revoke them.  We must
	 * dump those as-is (i.e., as deltas from an empty ACL).
	 *
	 * We can use defaclobjtype as the object type for acldefault(), except
	 * for the case of 'S' (DEFACLOBJ_SEQUENCE) which must be converted to
	 * 's'.
	 */
	appendPQExpBufferStr(query,
						 "SELECT oid, tableoid, "
						 "defaclrole, "
						 "defaclnamespace, "
						 "defaclobjtype, "
						 "defaclacl, "
						 "CASE WHEN defaclnamespace = 0 THEN "
						 "acldefault(CASE WHEN defaclobjtype = 'S' "
						 "THEN 's'::\"char\" ELSE defaclobjtype END, "
						 "defaclrole) ELSE '{}' END AS acldefault "
						 "FROM pg_default_acl");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	daclinfo = (DefaultACLInfo *) pg_malloc(ntups * sizeof(DefaultACLInfo));

	i_oid = PQfnumber(res, "oid");
	i_tableoid = PQfnumber(res, "tableoid");
	i_defaclrole = PQfnumber(res, "defaclrole");
	i_defaclnamespace = PQfnumber(res, "defaclnamespace");
	i_defaclobjtype = PQfnumber(res, "defaclobjtype");
	i_defaclacl = PQfnumber(res, "defaclacl");
	i_acldefault = PQfnumber(res, "acldefault");

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

		daclinfo[i].dacl.acl = pg_strdup(PQgetvalue(res, i, i_defaclacl));
		daclinfo[i].dacl.acldefault = pg_strdup(PQgetvalue(res, i, i_acldefault));
		daclinfo[i].dacl.privtype = 0;
		daclinfo[i].dacl.initprivs = NULL;
		daclinfo[i].defaclrole = getRoleName(PQgetvalue(res, i, i_defaclrole));
		daclinfo[i].defaclobjtype = *(PQgetvalue(res, i, i_defaclobjtype));

		/* Default ACLs are ACLs, of course */
		daclinfo[i].dobj.components |= DUMP_COMPONENT_ACL;

		/* Decide whether we want to dump it */
		selectDumpableDefaultACL(&(daclinfo[i]), dopt);
	}

	PQclear(res);

	destroyPQExpBuffer(query);
}

/*
 * getRoleName -- look up the name of a role, given its OID
 *
 * In current usage, we don't expect failures, so error out for a bad OID.
 */
static const char *
getRoleName(const char *roleoid_str)
{
	Oid			roleoid = atooid(roleoid_str);

	/*
	 * Do binary search to find the appropriate item.
	 */
	if (nrolenames > 0)
	{
		RoleNameItem *low = &rolenames[0];
		RoleNameItem *high = &rolenames[nrolenames - 1];

		while (low <= high)
		{
			RoleNameItem *middle = low + (high - low) / 2;

			if (roleoid < middle->roleoid)
				high = middle - 1;
			else if (roleoid > middle->roleoid)
				low = middle + 1;
			else
				return middle->rolename;	/* found a match */
		}
	}

	pg_fatal("role with OID %u does not exist", roleoid);
	return NULL;				/* keep compiler quiet */
}

/*
 * collectRoleNames --
 *
 * Construct a table of all known roles.
 * The table is sorted by OID for speed in lookup.
 */
static void
collectRoleNames(Archive *fout)
{
	PGresult   *res;
	const char *query;
	int			i;

	query = "SELECT oid, rolname FROM pg_catalog.pg_roles ORDER BY 1";

	res = ExecuteSqlQuery(fout, query, PGRES_TUPLES_OK);

	nrolenames = PQntuples(res);

	rolenames = (RoleNameItem *) pg_malloc(nrolenames * sizeof(RoleNameItem));

	for (i = 0; i < nrolenames; i++)
	{
		rolenames[i].roleoid = atooid(PQgetvalue(res, i, 0));
		rolenames[i].rolename = pg_strdup(PQgetvalue(res, i, 1));
	}

	PQclear(res);
}

/*
 * getAdditionalACLs
 *
 * We have now created all the DumpableObjects, and collected the ACL data
 * that appears in the directly-associated catalog entries.  However, there's
 * more ACL-related info to collect.  If any of a table's columns have ACLs,
 * we must set the TableInfo's DUMP_COMPONENT_ACL components flag, as well as
 * its hascolumnACLs flag (we won't store the ACLs themselves here, though).
 * Also, in versions having the pg_init_privs catalog, read that and load the
 * information into the relevant DumpableObjects.
 */
static void
getAdditionalACLs(Archive *fout)
{
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;
	int			ntups,
				i;

	/* Check for per-column ACLs */
	appendPQExpBufferStr(query,
						 "SELECT DISTINCT attrelid FROM pg_attribute "
						 "WHERE attacl IS NOT NULL");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	for (i = 0; i < ntups; i++)
	{
		Oid			relid = atooid(PQgetvalue(res, i, 0));
		TableInfo  *tblinfo;

		tblinfo = findTableByOid(relid);
		/* OK to ignore tables we haven't got a DumpableObject for */
		if (tblinfo)
		{
			tblinfo->dobj.components |= DUMP_COMPONENT_ACL;
			tblinfo->hascolumnACLs = true;
		}
	}
	PQclear(res);

	/* Fetch initial-privileges data */
	if (fout->remoteVersion >= 90600)
	{
		printfPQExpBuffer(query,
						  "SELECT objoid, classoid, objsubid, privtype, initprivs "
						  "FROM pg_init_privs");

		res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

		ntups = PQntuples(res);
		for (i = 0; i < ntups; i++)
		{
			Oid			objoid = atooid(PQgetvalue(res, i, 0));
			Oid			classoid = atooid(PQgetvalue(res, i, 1));
			int			objsubid = atoi(PQgetvalue(res, i, 2));
			char		privtype = *(PQgetvalue(res, i, 3));
			char	   *initprivs = PQgetvalue(res, i, 4);
			CatalogId	objId;
			DumpableObject *dobj;

			objId.tableoid = classoid;
			objId.oid = objoid;
			dobj = findObjectByCatalogId(objId);
			/* OK to ignore entries we haven't got a DumpableObject for */
			if (dobj)
			{
				/* Cope with sub-object initprivs */
				if (objsubid != 0)
				{
					if (dobj->objType == DO_TABLE)
					{
						/* For a column initprivs, set the table's ACL flags */
						dobj->components |= DUMP_COMPONENT_ACL;
						((TableInfo *) dobj)->hascolumnACLs = true;
					}
					else
						pg_log_warning("unsupported pg_init_privs entry: %u %u %d",
									   classoid, objoid, objsubid);
					continue;
				}

				/*
				 * We ignore any pg_init_privs.initprivs entry for the public
				 * schema, as explained in getNamespaces().
				 */
				if (dobj->objType == DO_NAMESPACE &&
					strcmp(dobj->name, "public") == 0)
					continue;

				/* Else it had better be of a type we think has ACLs */
				if (dobj->objType == DO_NAMESPACE ||
					dobj->objType == DO_TYPE ||
					dobj->objType == DO_FUNC ||
					dobj->objType == DO_AGG ||
					dobj->objType == DO_TABLE ||
					dobj->objType == DO_PROCLANG ||
					dobj->objType == DO_FDW ||
					dobj->objType == DO_FOREIGN_SERVER)
				{
					DumpableObjectWithAcl *daobj = (DumpableObjectWithAcl *) dobj;

					daobj->dacl.privtype = privtype;
					daobj->dacl.initprivs = pstrdup(initprivs);
				}
				else
					pg_log_warning("unsupported pg_init_privs entry: %u %u %d",
								   classoid, objoid, objsubid);
			}
		}
		PQclear(res);
	}

	destroyPQExpBuffer(query);
}

/*
 * dumpCommentExtended --
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
dumpCommentExtended(Archive *fout, const char *type,
					const char *name, const char *namespace,
					const char *owner, CatalogId catalogId,
					int subid, DumpId dumpId,
					const char *initdb_comment)
{
	DumpOptions *dopt = fout->dopt;
	CommentItem *comments;
	int			ncomments;

	/* do nothing, if --no-comments is supplied */
	if (dopt->no_comments)
		return;

	/* Comments are schema not data ... except LO comments are data */
	if (strcmp(type, "LARGE OBJECT") != 0)
	{
		if (!dopt->dumpSchema)
			return;
	}
	else
	{
		/* We do dump LO comments in binary-upgrade mode */
		if (!dopt->dumpData && !dopt->binary_upgrade)
			return;
	}

	/* Search for comments associated with catalogId, using table */
	ncomments = findComments(catalogId.tableoid, catalogId.oid,
							 &comments);

	/* Is there one matching the subid? */
	while (ncomments > 0)
	{
		if (comments->objsubid == subid)
			break;
		comments++;
		ncomments--;
	}

	if (initdb_comment != NULL)
	{
		static CommentItem empty_comment = {.descr = ""};

		/*
		 * initdb creates this object with a comment.  Skip dumping the
		 * initdb-provided comment, which would complicate matters for
		 * non-superuser use of pg_dump.  When the DBA has removed initdb's
		 * comment, replicate that.
		 */
		if (ncomments == 0)
		{
			comments = &empty_comment;
			ncomments = 1;
		}
		else if (strcmp(comments->descr, initdb_comment) == 0)
			ncomments = 0;
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
 * dumpComment --
 *
 * Typical simplification of the above function.
 */
static inline void
dumpComment(Archive *fout, const char *type,
			const char *name, const char *namespace,
			const char *owner, CatalogId catalogId,
			int subid, DumpId dumpId)
{
	dumpCommentExtended(fout, type, name, namespace, owner,
						catalogId, subid, dumpId, NULL);
}

/*
 * appendNamedArgument --
 *
 * Convenience routine for constructing parameters of the form:
 * 'paraname', 'value'::type
 */
static void
appendNamedArgument(PQExpBuffer out, Archive *fout, const char *argname,
					const char *argtype, const char *argval)
{
	appendPQExpBufferStr(out, ",\n\t");

	appendStringLiteralAH(out, argname, fout);
	appendPQExpBufferStr(out, ", ");

	appendStringLiteralAH(out, argval, fout);
	appendPQExpBuffer(out, "::%s", argtype);
}

/*
 * Decide which section to use based on the relkind of the parent object.
 *
 * NB: materialized views may be postponed from SECTION_PRE_DATA to
 * SECTION_POST_DATA to resolve some kinds of dependency problems. If so, the
 * matview stats will also be postponed to SECTION_POST_DATA. See
 * repairMatViewBoundaryMultiLoop().
 */
static teSection
statisticsDumpSection(const RelStatsInfo *rsinfo)
{
	switch (rsinfo->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_PARTITIONED_TABLE:
		case RELKIND_MATVIEW:
			return SECTION_DATA;
		case RELKIND_INDEX:
		case RELKIND_PARTITIONED_INDEX:
			return SECTION_POST_DATA;
		default:
			pg_fatal("cannot dump statistics for relation kind '%c'",
					 rsinfo->relkind);
	}

	return 0;					/* keep compiler quiet */
}

/*
 * dumpRelationStats --
 *
 * Dump command to import stats into the relation on the new database.
 */
static void
dumpRelationStats(Archive *fout, const RelStatsInfo *rsinfo)
{
	const DumpableObject *dobj = &rsinfo->dobj;
	PGresult   *res;
	PQExpBuffer query;
	PQExpBuffer out;
	DumpId	   *deps = NULL;
	int			ndeps = 0;
	char	   *qualified_name;
	int			i_attname;
	int			i_inherited;
	int			i_null_frac;
	int			i_avg_width;
	int			i_n_distinct;
	int			i_most_common_vals;
	int			i_most_common_freqs;
	int			i_histogram_bounds;
	int			i_correlation;
	int			i_most_common_elems;
	int			i_most_common_elem_freqs;
	int			i_elem_count_histogram;
	int			i_range_length_histogram;
	int			i_range_empty_frac;
	int			i_range_bounds_histogram;

	/* nothing to do if we are not dumping statistics */
	if (!fout->dopt->dumpStatistics)
		return;

	/* dependent on the relation definition, if doing schema */
	if (fout->dopt->dumpSchema)
	{
		deps = dobj->dependencies;
		ndeps = dobj->nDeps;
	}

	query = createPQExpBuffer();
	if (!fout->is_prepared[PREPQUERY_GETATTRIBUTESTATS])
	{
		appendPQExpBufferStr(query,
							 "PREPARE getAttributeStats(pg_catalog.name, pg_catalog.name) AS\n"
							 "SELECT s.attname, s.inherited, "
							 "s.null_frac, s.avg_width, s.n_distinct, "
							 "s.most_common_vals, s.most_common_freqs, "
							 "s.histogram_bounds, s.correlation, "
							 "s.most_common_elems, s.most_common_elem_freqs, "
							 "s.elem_count_histogram, ");

		if (fout->remoteVersion >= 170000)
			appendPQExpBufferStr(query,
								 "s.range_length_histogram, "
								 "s.range_empty_frac, "
								 "s.range_bounds_histogram ");
		else
			appendPQExpBufferStr(query,
								 "NULL AS range_length_histogram,"
								 "NULL AS range_empty_frac,"
								 "NULL AS range_bounds_histogram ");

		appendPQExpBufferStr(query,
							 "FROM pg_catalog.pg_stats s "
							 "WHERE s.schemaname = $1 "
							 "AND s.tablename = $2 "
							 "ORDER BY s.attname, s.inherited");

		ExecuteSqlStatement(fout, query->data);

		fout->is_prepared[PREPQUERY_GETATTRIBUTESTATS] = true;
		resetPQExpBuffer(query);
	}

	out = createPQExpBuffer();

	qualified_name = pg_strdup(fmtQualifiedDumpable(rsinfo));

	/* restore relation stats */
	appendPQExpBufferStr(out, "SELECT * FROM pg_catalog.pg_restore_relation_stats(\n");
	appendPQExpBuffer(out, "\t'version', '%u'::integer,\n",
					  fout->remoteVersion);
	appendPQExpBufferStr(out, "\t'relation', ");
	appendStringLiteralAH(out, qualified_name, fout);
	appendPQExpBufferStr(out, "::regclass,\n");
	appendPQExpBuffer(out, "\t'relpages', '%d'::integer,\n", rsinfo->relpages);
	appendPQExpBuffer(out, "\t'reltuples', '%s'::real,\n", rsinfo->reltuples);
	appendPQExpBuffer(out, "\t'relallvisible', '%d'::integer\n);\n",
					  rsinfo->relallvisible);

	/* fetch attribute stats */
	appendPQExpBufferStr(query, "EXECUTE getAttributeStats(");
	appendStringLiteralAH(query, dobj->namespace->dobj.name, fout);
	appendPQExpBufferStr(query, ", ");
	appendStringLiteralAH(query, dobj->name, fout);
	appendPQExpBufferStr(query, ");");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	i_attname = PQfnumber(res, "attname");
	i_inherited = PQfnumber(res, "inherited");
	i_null_frac = PQfnumber(res, "null_frac");
	i_avg_width = PQfnumber(res, "avg_width");
	i_n_distinct = PQfnumber(res, "n_distinct");
	i_most_common_vals = PQfnumber(res, "most_common_vals");
	i_most_common_freqs = PQfnumber(res, "most_common_freqs");
	i_histogram_bounds = PQfnumber(res, "histogram_bounds");
	i_correlation = PQfnumber(res, "correlation");
	i_most_common_elems = PQfnumber(res, "most_common_elems");
	i_most_common_elem_freqs = PQfnumber(res, "most_common_elem_freqs");
	i_elem_count_histogram = PQfnumber(res, "elem_count_histogram");
	i_range_length_histogram = PQfnumber(res, "range_length_histogram");
	i_range_empty_frac = PQfnumber(res, "range_empty_frac");
	i_range_bounds_histogram = PQfnumber(res, "range_bounds_histogram");

	/* restore attribute stats */
	for (int rownum = 0; rownum < PQntuples(res); rownum++)
	{
		const char *attname;

		appendPQExpBufferStr(out, "SELECT * FROM pg_catalog.pg_restore_attribute_stats(\n");
		appendPQExpBuffer(out, "\t'version', '%u'::integer,\n",
						  fout->remoteVersion);
		appendPQExpBufferStr(out, "\t'relation', ");
		appendStringLiteralAH(out, qualified_name, fout);
		appendPQExpBufferStr(out, "::regclass");

		if (PQgetisnull(res, rownum, i_attname))
			pg_fatal("attname cannot be NULL");
		attname = PQgetvalue(res, rownum, i_attname);

		/*
		 * Indexes look up attname in indAttNames to derive attnum, all others
		 * use attname directly.  We must specify attnum for indexes, since
		 * their attnames are not necessarily stable across dump/reload.
		 */
		if (rsinfo->nindAttNames == 0)
			appendNamedArgument(out, fout, "attname", "name", attname);
		else
		{
			bool		found = false;

			for (int i = 0; i < rsinfo->nindAttNames; i++)
			{
				if (strcmp(attname, rsinfo->indAttNames[i]) == 0)
				{
					appendPQExpBuffer(out, ",\n\t'attnum', '%d'::smallint",
									  i + 1);
					found = true;
					break;
				}
			}

			if (!found)
				pg_fatal("could not find index attname \"%s\"", attname);
		}

		if (!PQgetisnull(res, rownum, i_inherited))
			appendNamedArgument(out, fout, "inherited", "boolean",
								PQgetvalue(res, rownum, i_inherited));
		if (!PQgetisnull(res, rownum, i_null_frac))
			appendNamedArgument(out, fout, "null_frac", "real",
								PQgetvalue(res, rownum, i_null_frac));
		if (!PQgetisnull(res, rownum, i_avg_width))
			appendNamedArgument(out, fout, "avg_width", "integer",
								PQgetvalue(res, rownum, i_avg_width));
		if (!PQgetisnull(res, rownum, i_n_distinct))
			appendNamedArgument(out, fout, "n_distinct", "real",
								PQgetvalue(res, rownum, i_n_distinct));
		if (!PQgetisnull(res, rownum, i_most_common_vals))
			appendNamedArgument(out, fout, "most_common_vals", "text",
								PQgetvalue(res, rownum, i_most_common_vals));
		if (!PQgetisnull(res, rownum, i_most_common_freqs))
			appendNamedArgument(out, fout, "most_common_freqs", "real[]",
								PQgetvalue(res, rownum, i_most_common_freqs));
		if (!PQgetisnull(res, rownum, i_histogram_bounds))
			appendNamedArgument(out, fout, "histogram_bounds", "text",
								PQgetvalue(res, rownum, i_histogram_bounds));
		if (!PQgetisnull(res, rownum, i_correlation))
			appendNamedArgument(out, fout, "correlation", "real",
								PQgetvalue(res, rownum, i_correlation));
		if (!PQgetisnull(res, rownum, i_most_common_elems))
			appendNamedArgument(out, fout, "most_common_elems", "text",
								PQgetvalue(res, rownum, i_most_common_elems));
		if (!PQgetisnull(res, rownum, i_most_common_elem_freqs))
			appendNamedArgument(out, fout, "most_common_elem_freqs", "real[]",
								PQgetvalue(res, rownum, i_most_common_elem_freqs));
		if (!PQgetisnull(res, rownum, i_elem_count_histogram))
			appendNamedArgument(out, fout, "elem_count_histogram", "real[]",
								PQgetvalue(res, rownum, i_elem_count_histogram));
		if (fout->remoteVersion >= 170000)
		{
			if (!PQgetisnull(res, rownum, i_range_length_histogram))
				appendNamedArgument(out, fout, "range_length_histogram", "text",
									PQgetvalue(res, rownum, i_range_length_histogram));
			if (!PQgetisnull(res, rownum, i_range_empty_frac))
				appendNamedArgument(out, fout, "range_empty_frac", "real",
									PQgetvalue(res, rownum, i_range_empty_frac));
			if (!PQgetisnull(res, rownum, i_range_bounds_histogram))
				appendNamedArgument(out, fout, "range_bounds_histogram", "text",
									PQgetvalue(res, rownum, i_range_bounds_histogram));
		}
		appendPQExpBufferStr(out, "\n);\n");
	}

	PQclear(res);

	ArchiveEntry(fout, nilCatalogId, createDumpId(),
				 ARCHIVE_OPTS(.tag = dobj->name,
							  .namespace = dobj->namespace->dobj.name,
							  .description = "STATISTICS DATA",
							  .section = rsinfo->postponed_def ?
							  SECTION_POST_DATA : statisticsDumpSection(rsinfo),
							  .createStmt = out->data,
							  .deps = deps,
							  .nDeps = ndeps));

	free(qualified_name);
	destroyPQExpBuffer(out);
	destroyPQExpBuffer(query);
}

/*
 * dumpTableComment --
 *
 * As above, but dump comments for both the specified table (or view)
 * and its columns.
 */
static void
dumpTableComment(Archive *fout, const TableInfo *tbinfo,
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
	if (!dopt->dumpSchema)
		return;

	/* Search for comments associated with relation, using table */
	ncomments = findComments(tbinfo->dobj.catId.tableoid,
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
findComments(Oid classoid, Oid objoid, CommentItem **items)
{
	CommentItem *middle = NULL;
	CommentItem *low;
	CommentItem *high;
	int			nmatch;

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
 * Construct a table of all comments available for database objects;
 * also set the has-comment component flag for each relevant object.
 *
 * We used to do per-object queries for the comments, but it's much faster
 * to pull them all over at once, and on most databases the memory cost
 * isn't high.
 *
 * The table is sorted by classoid/objid/objsubid for speed in lookup.
 */
static void
collectComments(Archive *fout)
{
	PGresult   *res;
	PQExpBuffer query;
	int			i_description;
	int			i_classoid;
	int			i_objoid;
	int			i_objsubid;
	int			ntups;
	int			i;
	DumpableObject *dobj;

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
	ncomments = 0;
	dobj = NULL;

	for (i = 0; i < ntups; i++)
	{
		CatalogId	objId;
		int			subid;

		objId.tableoid = atooid(PQgetvalue(res, i, i_classoid));
		objId.oid = atooid(PQgetvalue(res, i, i_objoid));
		subid = atoi(PQgetvalue(res, i, i_objsubid));

		/* We needn't remember comments that don't match any dumpable object */
		if (dobj == NULL ||
			dobj->catId.tableoid != objId.tableoid ||
			dobj->catId.oid != objId.oid)
			dobj = findObjectByCatalogId(objId);
		if (dobj == NULL)
			continue;

		/*
		 * Comments on columns of composite types are linked to the type's
		 * pg_class entry, but we need to set the DUMP_COMPONENT_COMMENT flag
		 * in the type's own DumpableObject.
		 */
		if (subid != 0 && dobj->objType == DO_TABLE &&
			((TableInfo *) dobj)->relkind == RELKIND_COMPOSITE_TYPE)
		{
			TypeInfo   *cTypeInfo;

			cTypeInfo = findTypeByOid(((TableInfo *) dobj)->reltype);
			if (cTypeInfo)
				cTypeInfo->dobj.components |= DUMP_COMPONENT_COMMENT;
		}
		else
			dobj->components |= DUMP_COMPONENT_COMMENT;

		comments[ncomments].descr = pg_strdup(PQgetvalue(res, i, i_description));
		comments[ncomments].classoid = objId.tableoid;
		comments[ncomments].objoid = objId.oid;
		comments[ncomments].objsubid = subid;
		ncomments++;
	}

	PQclear(res);
	destroyPQExpBuffer(query);
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
	/*
	 * Clear any dump-request bits for components that don't exist for this
	 * object.  (This makes it safe to initially use DUMP_COMPONENT_ALL as the
	 * request for every kind of object.)
	 */
	dobj->dump &= dobj->components;

	/* Now, short-circuit if there's nothing to be done here. */
	if (dobj->dump == 0)
		return;

	switch (dobj->objType)
	{
		case DO_NAMESPACE:
			dumpNamespace(fout, (const NamespaceInfo *) dobj);
			break;
		case DO_EXTENSION:
			dumpExtension(fout, (const ExtensionInfo *) dobj);
			break;
		case DO_TYPE:
			dumpType(fout, (const TypeInfo *) dobj);
			break;
		case DO_SHELL_TYPE:
			dumpShellType(fout, (const ShellTypeInfo *) dobj);
			break;
		case DO_FUNC:
			dumpFunc(fout, (const FuncInfo *) dobj);
			break;
		case DO_AGG:
			dumpAgg(fout, (const AggInfo *) dobj);
			break;
		case DO_OPERATOR:
			dumpOpr(fout, (const OprInfo *) dobj);
			break;
		case DO_ACCESS_METHOD:
			dumpAccessMethod(fout, (const AccessMethodInfo *) dobj);
			break;
		case DO_OPCLASS:
			dumpOpclass(fout, (const OpclassInfo *) dobj);
			break;
		case DO_OPFAMILY:
			dumpOpfamily(fout, (const OpfamilyInfo *) dobj);
			break;
		case DO_COLLATION:
			dumpCollation(fout, (const CollInfo *) dobj);
			break;
		case DO_CONVERSION:
			dumpConversion(fout, (const ConvInfo *) dobj);
			break;
		case DO_TABLE:
			dumpTable(fout, (const TableInfo *) dobj);
			break;
		case DO_TABLE_ATTACH:
			dumpTableAttach(fout, (const TableAttachInfo *) dobj);
			break;
		case DO_ATTRDEF:
			dumpAttrDef(fout, (const AttrDefInfo *) dobj);
			break;
		case DO_INDEX:
			dumpIndex(fout, (const IndxInfo *) dobj);
			break;
		case DO_INDEX_ATTACH:
			dumpIndexAttach(fout, (const IndexAttachInfo *) dobj);
			break;
		case DO_STATSEXT:
			dumpStatisticsExt(fout, (const StatsExtInfo *) dobj);
			break;
		case DO_REFRESH_MATVIEW:
			refreshMatViewData(fout, (const TableDataInfo *) dobj);
			break;
		case DO_RULE:
			dumpRule(fout, (const RuleInfo *) dobj);
			break;
		case DO_TRIGGER:
			dumpTrigger(fout, (const TriggerInfo *) dobj);
			break;
		case DO_EVENT_TRIGGER:
			dumpEventTrigger(fout, (const EventTriggerInfo *) dobj);
			break;
		case DO_CONSTRAINT:
			dumpConstraint(fout, (const ConstraintInfo *) dobj);
			break;
		case DO_FK_CONSTRAINT:
			dumpConstraint(fout, (const ConstraintInfo *) dobj);
			break;
		case DO_PROCLANG:
			dumpProcLang(fout, (const ProcLangInfo *) dobj);
			break;
		case DO_CAST:
			dumpCast(fout, (const CastInfo *) dobj);
			break;
		case DO_TRANSFORM:
			dumpTransform(fout, (const TransformInfo *) dobj);
			break;
		case DO_SEQUENCE_SET:
			dumpSequenceData(fout, (const TableDataInfo *) dobj);
			break;
		case DO_TABLE_DATA:
			dumpTableData(fout, (const TableDataInfo *) dobj);
			break;
		case DO_DUMMY_TYPE:
			/* table rowtypes and array types are never dumped separately */
			break;
		case DO_TSPARSER:
			dumpTSParser(fout, (const TSParserInfo *) dobj);
			break;
		case DO_TSDICT:
			dumpTSDictionary(fout, (const TSDictInfo *) dobj);
			break;
		case DO_TSTEMPLATE:
			dumpTSTemplate(fout, (const TSTemplateInfo *) dobj);
			break;
		case DO_TSCONFIG:
			dumpTSConfig(fout, (const TSConfigInfo *) dobj);
			break;
		case DO_FDW:
			dumpForeignDataWrapper(fout, (const FdwInfo *) dobj);
			break;
		case DO_FOREIGN_SERVER:
			dumpForeignServer(fout, (const ForeignServerInfo *) dobj);
			break;
		case DO_DEFAULT_ACL:
			dumpDefaultACL(fout, (const DefaultACLInfo *) dobj);
			break;
		case DO_LARGE_OBJECT:
			dumpLO(fout, (const LoInfo *) dobj);
			break;
		case DO_LARGE_OBJECT_DATA:
			if (dobj->dump & DUMP_COMPONENT_DATA)
			{
				LoInfo	   *loinfo;
				TocEntry   *te;

				loinfo = (LoInfo *) findObjectByDumpId(dobj->dependencies[0]);
				if (loinfo == NULL)
					pg_fatal("missing metadata for large objects \"%s\"",
							 dobj->name);

				te = ArchiveEntry(fout, dobj->catId, dobj->dumpId,
								  ARCHIVE_OPTS(.tag = dobj->name,
											   .owner = loinfo->rolname,
											   .description = "BLOBS",
											   .section = SECTION_DATA,
											   .deps = dobj->dependencies,
											   .nDeps = dobj->nDeps,
											   .dumpFn = dumpLOs,
											   .dumpArg = loinfo));

				/*
				 * Set the TocEntry's dataLength in case we are doing a
				 * parallel dump and want to order dump jobs by table size.
				 * (We need some size estimate for every TocEntry with a
				 * DataDumper function.)  We don't currently have any cheap
				 * way to estimate the size of LOs, but fortunately it doesn't
				 * matter too much as long as we get large batches of LOs
				 * processed reasonably early.  Assume 8K per blob.
				 */
				te->dataLength = loinfo->numlos * (pgoff_t) 8192;
			}
			break;
		case DO_POLICY:
			dumpPolicy(fout, (const PolicyInfo *) dobj);
			break;
		case DO_PUBLICATION:
			dumpPublication(fout, (const PublicationInfo *) dobj);
			break;
		case DO_PUBLICATION_REL:
			dumpPublicationTable(fout, (const PublicationRelInfo *) dobj);
			break;
		case DO_PUBLICATION_TABLE_IN_SCHEMA:
			dumpPublicationNamespace(fout,
									 (const PublicationSchemaInfo *) dobj);
			break;
		case DO_SUBSCRIPTION:
			dumpSubscription(fout, (const SubscriptionInfo *) dobj);
			break;
		case DO_SUBSCRIPTION_REL:
			dumpSubscriptionTable(fout, (const SubRelInfo *) dobj);
			break;
		case DO_REL_STATS:
			dumpRelationStats(fout, (const RelStatsInfo *) dobj);
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
dumpNamespace(Archive *fout, const NamespaceInfo *nspinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *qnspname;

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	qnspname = pg_strdup(fmtId(nspinfo->dobj.name));

	if (nspinfo->create)
	{
		appendPQExpBuffer(delq, "DROP SCHEMA %s;\n", qnspname);
		appendPQExpBuffer(q, "CREATE SCHEMA %s;\n", qnspname);
	}
	else
	{
		/* see selectDumpableNamespace() */
		appendPQExpBufferStr(delq,
							 "-- *not* dropping schema, since initdb creates it\n");
		appendPQExpBufferStr(q,
							 "-- *not* creating schema, since initdb creates it\n");
	}

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
	{
		const char *initdb_comment = NULL;

		if (!nspinfo->create && strcmp(qnspname, "public") == 0)
			initdb_comment = "standard public schema";
		dumpCommentExtended(fout, "SCHEMA", qnspname,
							NULL, nspinfo->rolname,
							nspinfo->dobj.catId, 0, nspinfo->dobj.dumpId,
							initdb_comment);
	}

	if (nspinfo->dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpSecLabel(fout, "SCHEMA", qnspname,
					 NULL, nspinfo->rolname,
					 nspinfo->dobj.catId, 0, nspinfo->dobj.dumpId);

	if (nspinfo->dobj.dump & DUMP_COMPONENT_ACL)
		dumpACL(fout, nspinfo->dobj.dumpId, InvalidDumpId, "SCHEMA",
				qnspname, NULL, NULL,
				NULL, nspinfo->rolname, &nspinfo->dacl);

	free(qnspname);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
}

/*
 * dumpExtension
 *	  writes out to fout the queries to recreate an extension
 */
static void
dumpExtension(Archive *fout, const ExtensionInfo *extinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *qextname;

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
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
dumpType(Archive *fout, const TypeInfo *tyinfo)
{
	DumpOptions *dopt = fout->dopt;

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
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
dumpEnumType(Archive *fout, const TypeInfo *tyinfo)
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
	int			i_enumlabel;
	int			i_oid;

	if (!fout->is_prepared[PREPQUERY_DUMPENUMTYPE])
	{
		/* Set up query for enum-specific details */
		appendPQExpBufferStr(query,
							 "PREPARE dumpEnumType(pg_catalog.oid) AS\n"
							 "SELECT oid, enumlabel "
							 "FROM pg_catalog.pg_enum "
							 "WHERE enumtypid = $1 "
							 "ORDER BY enumsortorder");

		ExecuteSqlStatement(fout, query->data);

		fout->is_prepared[PREPQUERY_DUMPENUMTYPE] = true;
	}

	printfPQExpBuffer(query,
					  "EXECUTE dumpEnumType('%u')",
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
												 false, false);

	appendPQExpBuffer(q, "CREATE TYPE %s AS ENUM (",
					  qualtypname);

	if (!dopt->binary_upgrade)
	{
		i_enumlabel = PQfnumber(res, "enumlabel");

		/* Labels with server-assigned oids */
		for (i = 0; i < num; i++)
		{
			label = PQgetvalue(res, i, i_enumlabel);
			if (i > 0)
				appendPQExpBufferChar(q, ',');
			appendPQExpBufferStr(q, "\n    ");
			appendStringLiteralAH(q, label, fout);
		}
	}

	appendPQExpBufferStr(q, "\n);\n");

	if (dopt->binary_upgrade)
	{
		i_oid = PQfnumber(res, "oid");
		i_enumlabel = PQfnumber(res, "enumlabel");

		/* Labels with dump-assigned (preserved) oids */
		for (i = 0; i < num; i++)
		{
			enum_oid = atooid(PQgetvalue(res, i, i_oid));
			label = PQgetvalue(res, i, i_enumlabel);

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
				NULL, tyinfo->rolname, &tyinfo->dacl);

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
dumpRangeType(Archive *fout, const TypeInfo *tyinfo)
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

	if (!fout->is_prepared[PREPQUERY_DUMPRANGETYPE])
	{
		/* Set up query for range-specific details */
		appendPQExpBufferStr(query,
							 "PREPARE dumpRangeType(pg_catalog.oid) AS\n");

		appendPQExpBufferStr(query,
							 "SELECT ");

		if (fout->remoteVersion >= 140000)
			appendPQExpBufferStr(query,
								 "pg_catalog.format_type(rngmultitypid, NULL) AS rngmultitype, ");
		else
			appendPQExpBufferStr(query,
								 "NULL AS rngmultitype, ");

		appendPQExpBufferStr(query,
							 "pg_catalog.format_type(rngsubtype, NULL) AS rngsubtype, "
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
							 "rngtypid = $1");

		ExecuteSqlStatement(fout, query->data);

		fout->is_prepared[PREPQUERY_DUMPRANGETYPE] = true;
	}

	printfPQExpBuffer(query,
					  "EXECUTE dumpRangeType('%u')",
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
												 false, true);

	appendPQExpBuffer(q, "CREATE TYPE %s AS RANGE (",
					  qualtypname);

	appendPQExpBuffer(q, "\n    subtype = %s",
					  PQgetvalue(res, 0, PQfnumber(res, "rngsubtype")));

	if (!PQgetisnull(res, 0, PQfnumber(res, "rngmultitype")))
		appendPQExpBuffer(q, ",\n    multirange_type_name = %s",
						  PQgetvalue(res, 0, PQfnumber(res, "rngmultitype")));

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
				NULL, tyinfo->rolname, &tyinfo->dacl);

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
dumpUndefinedType(Archive *fout, const TypeInfo *tyinfo)
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
												 false, false);

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
				NULL, tyinfo->rolname, &tyinfo->dacl);

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
dumpBaseType(Archive *fout, const TypeInfo *tyinfo)
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
	char	   *typsubscript;
	Oid			typreceiveoid;
	Oid			typsendoid;
	Oid			typmodinoid;
	Oid			typmodoutoid;
	Oid			typanalyzeoid;
	Oid			typsubscriptoid;
	char	   *typcategory;
	char	   *typispreferred;
	char	   *typdelim;
	char	   *typbyval;
	char	   *typalign;
	char	   *typstorage;
	char	   *typcollatable;
	char	   *typdefault;
	bool		typdefault_is_literal = false;

	if (!fout->is_prepared[PREPQUERY_DUMPBASETYPE])
	{
		/* Set up query for type-specific details */
		appendPQExpBufferStr(query,
							 "PREPARE dumpBaseType(pg_catalog.oid) AS\n"
							 "SELECT typlen, "
							 "typinput, typoutput, typreceive, typsend, "
							 "typreceive::pg_catalog.oid AS typreceiveoid, "
							 "typsend::pg_catalog.oid AS typsendoid, "
							 "typanalyze, "
							 "typanalyze::pg_catalog.oid AS typanalyzeoid, "
							 "typdelim, typbyval, typalign, typstorage, "
							 "typmodin, typmodout, "
							 "typmodin::pg_catalog.oid AS typmodinoid, "
							 "typmodout::pg_catalog.oid AS typmodoutoid, "
							 "typcategory, typispreferred, "
							 "(typcollation <> 0) AS typcollatable, "
							 "pg_catalog.pg_get_expr(typdefaultbin, 0) AS typdefaultbin, typdefault, ");

		if (fout->remoteVersion >= 140000)
			appendPQExpBufferStr(query,
								 "typsubscript, "
								 "typsubscript::pg_catalog.oid AS typsubscriptoid ");
		else
			appendPQExpBufferStr(query,
								 "'-' AS typsubscript, 0 AS typsubscriptoid ");

		appendPQExpBufferStr(query, "FROM pg_catalog.pg_type "
							 "WHERE oid = $1");

		ExecuteSqlStatement(fout, query->data);

		fout->is_prepared[PREPQUERY_DUMPBASETYPE] = true;
	}

	printfPQExpBuffer(query,
					  "EXECUTE dumpBaseType('%u')",
					  tyinfo->dobj.catId.oid);

	res = ExecuteSqlQueryForSingleRow(fout, query->data);

	typlen = PQgetvalue(res, 0, PQfnumber(res, "typlen"));
	typinput = PQgetvalue(res, 0, PQfnumber(res, "typinput"));
	typoutput = PQgetvalue(res, 0, PQfnumber(res, "typoutput"));
	typreceive = PQgetvalue(res, 0, PQfnumber(res, "typreceive"));
	typsend = PQgetvalue(res, 0, PQfnumber(res, "typsend"));
	typmodin = PQgetvalue(res, 0, PQfnumber(res, "typmodin"));
	typmodout = PQgetvalue(res, 0, PQfnumber(res, "typmodout"));
	typanalyze = PQgetvalue(res, 0, PQfnumber(res, "typanalyze"));
	typsubscript = PQgetvalue(res, 0, PQfnumber(res, "typsubscript"));
	typreceiveoid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typreceiveoid")));
	typsendoid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typsendoid")));
	typmodinoid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typmodinoid")));
	typmodoutoid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typmodoutoid")));
	typanalyzeoid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typanalyzeoid")));
	typsubscriptoid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typsubscriptoid")));
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
												 false, false);

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

	if (OidIsValid(typsubscriptoid))
		appendPQExpBuffer(q, ",\n    SUBSCRIPT = %s", typsubscript);

	if (OidIsValid(tyinfo->typelem))
		appendPQExpBuffer(q, ",\n    ELEMENT = %s",
						  getFormattedTypeName(fout, tyinfo->typelem,
											   zeroIsError));

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
				NULL, tyinfo->rolname, &tyinfo->dacl);

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
dumpDomain(Archive *fout, const TypeInfo *tyinfo)
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

	if (!fout->is_prepared[PREPQUERY_DUMPDOMAIN])
	{
		/* Set up query for domain-specific details */
		appendPQExpBufferStr(query,
							 "PREPARE dumpDomain(pg_catalog.oid) AS\n");

		appendPQExpBufferStr(query, "SELECT t.typnotnull, "
							 "pg_catalog.format_type(t.typbasetype, t.typtypmod) AS typdefn, "
							 "pg_catalog.pg_get_expr(t.typdefaultbin, 'pg_catalog.pg_type'::pg_catalog.regclass) AS typdefaultbin, "
							 "t.typdefault, "
							 "CASE WHEN t.typcollation <> u.typcollation "
							 "THEN t.typcollation ELSE 0 END AS typcollation "
							 "FROM pg_catalog.pg_type t "
							 "LEFT JOIN pg_catalog.pg_type u ON (t.typbasetype = u.oid) "
							 "WHERE t.oid = $1");

		ExecuteSqlStatement(fout, query->data);

		fout->is_prepared[PREPQUERY_DUMPDOMAIN] = true;
	}

	printfPQExpBuffer(query,
					  "EXECUTE dumpDomain('%u')",
					  tyinfo->dobj.catId.oid);

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
												 true,	/* force array type */
												 false);	/* force multirange type */

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
				NULL, tyinfo->rolname, &tyinfo->dacl);

	/* Dump any per-constraint comments */
	for (i = 0; i < tyinfo->nDomChecks; i++)
	{
		ConstraintInfo *domcheck = &(tyinfo->domChecks[i]);
		PQExpBuffer conprefix = createPQExpBuffer();

		appendPQExpBuffer(conprefix, "CONSTRAINT %s ON DOMAIN",
						  fmtId(domcheck->dobj.name));

		if (domcheck->dobj.dump & DUMP_COMPONENT_COMMENT)
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
dumpCompositeType(Archive *fout, const TypeInfo *tyinfo)
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

	if (!fout->is_prepared[PREPQUERY_DUMPCOMPOSITETYPE])
	{
		/*
		 * Set up query for type-specific details.
		 *
		 * Since we only want to dump COLLATE clauses for attributes whose
		 * collation is different from their type's default, we use a CASE
		 * here to suppress uninteresting attcollations cheaply.  atttypid
		 * will be 0 for dropped columns; collation does not matter for those.
		 */
		appendPQExpBufferStr(query,
							 "PREPARE dumpCompositeType(pg_catalog.oid) AS\n"
							 "SELECT a.attname, a.attnum, "
							 "pg_catalog.format_type(a.atttypid, a.atttypmod) AS atttypdefn, "
							 "a.attlen, a.attalign, a.attisdropped, "
							 "CASE WHEN a.attcollation <> at.typcollation "
							 "THEN a.attcollation ELSE 0 END AS attcollation "
							 "FROM pg_catalog.pg_type ct "
							 "JOIN pg_catalog.pg_attribute a ON a.attrelid = ct.typrelid "
							 "LEFT JOIN pg_catalog.pg_type at ON at.oid = a.atttypid "
							 "WHERE ct.oid = $1 "
							 "ORDER BY a.attnum");

		ExecuteSqlStatement(fout, query->data);

		fout->is_prepared[PREPQUERY_DUMPCOMPOSITETYPE] = true;
	}

	printfPQExpBuffer(query,
					  "EXECUTE dumpCompositeType('%u')",
					  tyinfo->dobj.catId.oid);

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
												 false, false);
		binary_upgrade_set_pg_class_oids(fout, q, tyinfo->typrelid);
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
				NULL, tyinfo->rolname, &tyinfo->dacl);

	/* Dump any per-column comments */
	if (tyinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpCompositeTypeColComments(fout, tyinfo, res);

	PQclear(res);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(dropped);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(query);
	free(qtypname);
	free(qualtypname);
}

/*
 * dumpCompositeTypeColComments
 *	  writes out to fout the queries to recreate comments on the columns of
 *	  a user-defined stand-alone composite type.
 *
 * The caller has already made a query to collect the names and attnums
 * of the type's columns, so we just pass that result into here rather
 * than reading them again.
 */
static void
dumpCompositeTypeColComments(Archive *fout, const TypeInfo *tyinfo,
							 PGresult *res)
{
	CommentItem *comments;
	int			ncomments;
	PQExpBuffer query;
	PQExpBuffer target;
	int			i;
	int			ntups;
	int			i_attname;
	int			i_attnum;
	int			i_attisdropped;

	/* do nothing, if --no-comments is supplied */
	if (fout->dopt->no_comments)
		return;

	/* Search for comments associated with type's pg_class OID */
	ncomments = findComments(RelationRelationId, tyinfo->typrelid,
							 &comments);

	/* If no comments exist, we're done */
	if (ncomments <= 0)
		return;

	/* Build COMMENT ON statements */
	query = createPQExpBuffer();
	target = createPQExpBuffer();

	ntups = PQntuples(res);
	i_attnum = PQfnumber(res, "attnum");
	i_attname = PQfnumber(res, "attname");
	i_attisdropped = PQfnumber(res, "attisdropped");
	while (ncomments > 0)
	{
		const char *attname;

		attname = NULL;
		for (i = 0; i < ntups; i++)
		{
			if (atoi(PQgetvalue(res, i, i_attnum)) == comments->objsubid &&
				PQgetvalue(res, i, i_attisdropped)[0] != 't')
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
dumpShellType(Archive *fout, const ShellTypeInfo *stinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
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
												 false, false);

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
dumpProcLang(Archive *fout, const ProcLangInfo *plang)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer defqry;
	PQExpBuffer delqry;
	bool		useParams;
	char	   *qlanname;
	FuncInfo   *funcInfo;
	FuncInfo   *inlineInfo = NULL;
	FuncInfo   *validatorInfo = NULL;

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
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
				NULL, plang->lanowner, &plang->dacl);

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
format_function_arguments(const FuncInfo *finfo, const char *funcargs, bool is_agg)
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
 * format_function_signature: generate function name and argument list
 *
 * Only a minimal list of input argument types is generated; this is
 * sufficient to reference the function, but not to define it.
 *
 * If honor_quotes is false then the function name is never quoted.
 * This is appropriate for use in TOC tags, but not in SQL commands.
 */
static char *
format_function_signature(Archive *fout, const FuncInfo *finfo, bool honor_quotes)
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
		if (j > 0)
			appendPQExpBufferStr(&fn, ", ");

		appendPQExpBufferStr(&fn,
							 getFormattedTypeName(fout, finfo->argtypes[j],
												  zeroIsError));
	}
	appendPQExpBufferChar(&fn, ')');
	return fn.data;
}


/*
 * dumpFunc:
 *	  dump out one function
 */
static void
dumpFunc(Archive *fout, const FuncInfo *finfo)
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
	char	   *qual_funcsig;
	char	   *proretset;
	char	   *prosrc;
	char	   *probin;
	char	   *prosqlbody;
	char	   *funcargs;
	char	   *funciargs;
	char	   *funcresult;
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
	char	  **configitems = NULL;
	int			nconfigitems = 0;
	const char *keyword;

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
		return;

	query = createPQExpBuffer();
	q = createPQExpBuffer();
	delqry = createPQExpBuffer();
	asPart = createPQExpBuffer();

	if (!fout->is_prepared[PREPQUERY_DUMPFUNC])
	{
		/* Set up query for function-specific details */
		appendPQExpBufferStr(query,
							 "PREPARE dumpFunc(pg_catalog.oid) AS\n");

		appendPQExpBufferStr(query,
							 "SELECT\n"
							 "proretset,\n"
							 "prosrc,\n"
							 "probin,\n"
							 "provolatile,\n"
							 "proisstrict,\n"
							 "prosecdef,\n"
							 "lanname,\n"
							 "proconfig,\n"
							 "procost,\n"
							 "prorows,\n"
							 "pg_catalog.pg_get_function_arguments(p.oid) AS funcargs,\n"
							 "pg_catalog.pg_get_function_identity_arguments(p.oid) AS funciargs,\n"
							 "pg_catalog.pg_get_function_result(p.oid) AS funcresult,\n"
							 "proleakproof,\n");

		if (fout->remoteVersion >= 90500)
			appendPQExpBufferStr(query,
								 "array_to_string(protrftypes, ' ') AS protrftypes,\n");
		else
			appendPQExpBufferStr(query,
								 "NULL AS protrftypes,\n");

		if (fout->remoteVersion >= 90600)
			appendPQExpBufferStr(query,
								 "proparallel,\n");
		else
			appendPQExpBufferStr(query,
								 "'u' AS proparallel,\n");

		if (fout->remoteVersion >= 110000)
			appendPQExpBufferStr(query,
								 "prokind,\n");
		else
			appendPQExpBufferStr(query,
								 "CASE WHEN proiswindow THEN 'w' ELSE 'f' END AS prokind,\n");

		if (fout->remoteVersion >= 120000)
			appendPQExpBufferStr(query,
								 "prosupport,\n");
		else
			appendPQExpBufferStr(query,
								 "'-' AS prosupport,\n");

		if (fout->remoteVersion >= 140000)
			appendPQExpBufferStr(query,
								 "pg_get_function_sqlbody(p.oid) AS prosqlbody\n");
		else
			appendPQExpBufferStr(query,
								 "NULL AS prosqlbody\n");

		appendPQExpBufferStr(query,
							 "FROM pg_catalog.pg_proc p, pg_catalog.pg_language l\n"
							 "WHERE p.oid = $1 "
							 "AND l.oid = p.prolang");

		ExecuteSqlStatement(fout, query->data);

		fout->is_prepared[PREPQUERY_DUMPFUNC] = true;
	}

	printfPQExpBuffer(query,
					  "EXECUTE dumpFunc('%u')",
					  finfo->dobj.catId.oid);

	res = ExecuteSqlQueryForSingleRow(fout, query->data);

	proretset = PQgetvalue(res, 0, PQfnumber(res, "proretset"));
	if (PQgetisnull(res, 0, PQfnumber(res, "prosqlbody")))
	{
		prosrc = PQgetvalue(res, 0, PQfnumber(res, "prosrc"));
		probin = PQgetvalue(res, 0, PQfnumber(res, "probin"));
		prosqlbody = NULL;
	}
	else
	{
		prosrc = NULL;
		probin = NULL;
		prosqlbody = PQgetvalue(res, 0, PQfnumber(res, "prosqlbody"));
	}
	funcargs = PQgetvalue(res, 0, PQfnumber(res, "funcargs"));
	funciargs = PQgetvalue(res, 0, PQfnumber(res, "funciargs"));
	funcresult = PQgetvalue(res, 0, PQfnumber(res, "funcresult"));
	protrftypes = PQgetvalue(res, 0, PQfnumber(res, "protrftypes"));
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
	 * is used.
	 */
	if (prosqlbody)
	{
		appendPQExpBufferStr(asPart, prosqlbody);
	}
	else if (probin[0] != '\0')
	{
		appendPQExpBufferStr(asPart, "AS ");
		appendStringLiteralAH(asPart, probin, fout);
		if (prosrc[0] != '\0')
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
		appendPQExpBufferStr(asPart, "AS ");
		/* with no bin, dollar quote src unconditionally if allowed */
		if (dopt->disable_dollar_quoting)
			appendStringLiteralAH(asPart, prosrc, fout);
		else
			appendStringLiteralDQ(asPart, prosrc, NULL);
	}

	if (*proconfig)
	{
		if (!parsePGArray(proconfig, &configitems, &nconfigitems))
			pg_fatal("could not parse %s array", "proconfig");
	}
	else
	{
		configitems = NULL;
		nconfigitems = 0;
	}

	funcfullsig = format_function_arguments(finfo, funcargs, false);
	funcsig = format_function_arguments(finfo, funciargs, false);

	funcsig_tag = format_function_signature(fout, finfo, false);

	qual_funcsig = psprintf("%s.%s",
							fmtId(finfo->dobj.namespace->dobj.name),
							funcsig);

	if (prokind[0] == PROKIND_PROCEDURE)
		keyword = "PROCEDURE";
	else
		keyword = "FUNCTION";	/* works for window functions too */

	appendPQExpBuffer(delqry, "DROP %s %s;\n",
					  keyword, qual_funcsig);

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
		appendPQExpBuffer(q, " RETURNS %s%s",
						  (proretset[0] == 't') ? "SETOF " : "",
						  getFormattedTypeName(fout, finfo->prorettype,
											   zeroIsError));

	appendPQExpBuffer(q, "\n    LANGUAGE %s", fmtId(lanname));

	if (*protrftypes)
	{
		Oid		   *typeids = pg_malloc(FUNC_MAX_ARGS * sizeof(Oid));
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

		free(typeids);
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
			pg_fatal("unrecognized provolatile value for function \"%s\"",
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
			pg_fatal("unrecognized proparallel value for function \"%s\"",
					 finfo->dobj.name);
	}

	for (int i = 0; i < nconfigitems; i++)
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
								qual_funcsig);

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
								  .section = finfo->postponed_def ?
								  SECTION_POST_DATA : SECTION_PRE_DATA,
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
				NULL, finfo->rolname, &finfo->dacl);

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delqry);
	destroyPQExpBuffer(asPart);
	free(funcsig);
	free(funcfullsig);
	free(funcsig_tag);
	free(qual_funcsig);
	free(configitems);
}


/*
 * Dump a user-defined cast
 */
static void
dumpCast(Archive *fout, const CastInfo *cast)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer defqry;
	PQExpBuffer delqry;
	PQExpBuffer labelq;
	PQExpBuffer castargs;
	FuncInfo   *funcInfo = NULL;
	const char *sourceType;
	const char *targetType;

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
		return;

	/* Cannot dump if we don't have the cast function's info */
	if (OidIsValid(cast->castfunc))
	{
		funcInfo = findFuncByOid(cast->castfunc);
		if (funcInfo == NULL)
			pg_fatal("could not find function definition for function with OID %u",
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

	destroyPQExpBuffer(defqry);
	destroyPQExpBuffer(delqry);
	destroyPQExpBuffer(labelq);
	destroyPQExpBuffer(castargs);
}

/*
 * Dump a transform
 */
static void
dumpTransform(Archive *fout, const TransformInfo *transform)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer defqry;
	PQExpBuffer delqry;
	PQExpBuffer labelq;
	PQExpBuffer transformargs;
	FuncInfo   *fromsqlFuncInfo = NULL;
	FuncInfo   *tosqlFuncInfo = NULL;
	char	   *lanname;
	const char *transformType;

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
		return;

	/* Cannot dump if we don't have the transform functions' info */
	if (OidIsValid(transform->trffromsql))
	{
		fromsqlFuncInfo = findFuncByOid(transform->trffromsql);
		if (fromsqlFuncInfo == NULL)
			pg_fatal("could not find function definition for function with OID %u",
					 transform->trffromsql);
	}
	if (OidIsValid(transform->trftosql))
	{
		tosqlFuncInfo = findFuncByOid(transform->trftosql);
		if (tosqlFuncInfo == NULL)
			pg_fatal("could not find function definition for function with OID %u",
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
dumpOpr(Archive *fout, const OprInfo *oprinfo)
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

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
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

	if (!fout->is_prepared[PREPQUERY_DUMPOPR])
	{
		/* Set up query for operator-specific details */
		appendPQExpBufferStr(query,
							 "PREPARE dumpOpr(pg_catalog.oid) AS\n"
							 "SELECT oprkind, "
							 "oprcode::pg_catalog.regprocedure, "
							 "oprleft::pg_catalog.regtype, "
							 "oprright::pg_catalog.regtype, "
							 "oprcom, "
							 "oprnegate, "
							 "oprrest::pg_catalog.regprocedure, "
							 "oprjoin::pg_catalog.regprocedure, "
							 "oprcanmerge, oprcanhash "
							 "FROM pg_catalog.pg_operator "
							 "WHERE oid = $1");

		ExecuteSqlStatement(fout, query->data);

		fout->is_prepared[PREPQUERY_DUMPOPR] = true;
	}

	printfPQExpBuffer(query,
					  "EXECUTE dumpOpr('%u')",
					  oprinfo->dobj.catId.oid);

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
dumpAccessMethod(Archive *fout, const AccessMethodInfo *aminfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *qamname;

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
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
dumpOpclass(Archive *fout, const OpclassInfo *opcinfo)
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
	char	   *amopopr;
	char	   *sortfamily;
	char	   *sortfamilynsp;
	char	   *amprocnum;
	char	   *amproc;
	char	   *amproclefttype;
	char	   *amprocrighttype;
	bool		needComma;
	int			i;

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
		return;

	query = createPQExpBuffer();
	q = createPQExpBuffer();
	delq = createPQExpBuffer();
	nameusing = createPQExpBuffer();

	/* Get additional fields from the pg_opclass row */
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
	 */
	resetPQExpBuffer(query);
	appendPQExpBuffer(query, "SELECT amopstrategy, "
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

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_amopstrategy = PQfnumber(res, "amopstrategy");
	i_amopopr = PQfnumber(res, "amopopr");
	i_sortfamily = PQfnumber(res, "sortfamily");
	i_sortfamilynsp = PQfnumber(res, "sortfamilynsp");

	for (i = 0; i < ntups; i++)
	{
		amopstrategy = PQgetvalue(res, i, i_amopstrategy);
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
	 * of a btree sort support function for a cross-type comparison.
	 */
	resetPQExpBuffer(query);

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
dumpOpfamily(Archive *fout, const OpfamilyInfo *opfinfo)
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
	int			i_amopopr;
	int			i_sortfamily;
	int			i_sortfamilynsp;
	int			i_amprocnum;
	int			i_amproc;
	int			i_amproclefttype;
	int			i_amprocrighttype;
	char	   *amname;
	char	   *amopstrategy;
	char	   *amopopr;
	char	   *sortfamily;
	char	   *sortfamilynsp;
	char	   *amprocnum;
	char	   *amproc;
	char	   *amproclefttype;
	char	   *amprocrighttype;
	bool		needComma;
	int			i;

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
		return;

	query = createPQExpBuffer();
	q = createPQExpBuffer();
	delq = createPQExpBuffer();
	nameusing = createPQExpBuffer();

	/*
	 * Fetch only those opfamily members that are tied directly to the
	 * opfamily by pg_depend entries.
	 */
	appendPQExpBuffer(query, "SELECT amopstrategy, "
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
		i_amopopr = PQfnumber(res_ops, "amopopr");
		i_sortfamily = PQfnumber(res_ops, "sortfamily");
		i_sortfamilynsp = PQfnumber(res_ops, "sortfamilynsp");

		for (i = 0; i < ntups; i++)
		{
			amopstrategy = PQgetvalue(res_ops, i, i_amopstrategy);
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
dumpCollation(Archive *fout, const CollInfo *collinfo)
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
	int			i_colllocale;
	int			i_collicurules;
	const char *collprovider;
	const char *collcollate;
	const char *collctype;
	const char *colllocale;
	const char *collicurules;

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
		return;

	query = createPQExpBuffer();
	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	qcollname = pg_strdup(fmtId(collinfo->dobj.name));

	/* Get collation-specific details */
	appendPQExpBufferStr(query, "SELECT ");

	if (fout->remoteVersion >= 100000)
		appendPQExpBufferStr(query,
							 "collprovider, "
							 "collversion, ");
	else
		appendPQExpBufferStr(query,
							 "'c' AS collprovider, "
							 "NULL AS collversion, ");

	if (fout->remoteVersion >= 120000)
		appendPQExpBufferStr(query,
							 "collisdeterministic, ");
	else
		appendPQExpBufferStr(query,
							 "true AS collisdeterministic, ");

	if (fout->remoteVersion >= 170000)
		appendPQExpBufferStr(query,
							 "colllocale, ");
	else if (fout->remoteVersion >= 150000)
		appendPQExpBufferStr(query,
							 "colliculocale AS colllocale, ");
	else
		appendPQExpBufferStr(query,
							 "NULL AS colllocale, ");

	if (fout->remoteVersion >= 160000)
		appendPQExpBufferStr(query,
							 "collicurules, ");
	else
		appendPQExpBufferStr(query,
							 "NULL AS collicurules, ");

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
	i_colllocale = PQfnumber(res, "colllocale");
	i_collicurules = PQfnumber(res, "collicurules");

	collprovider = PQgetvalue(res, 0, i_collprovider);

	if (!PQgetisnull(res, 0, i_collcollate))
		collcollate = PQgetvalue(res, 0, i_collcollate);
	else
		collcollate = NULL;

	if (!PQgetisnull(res, 0, i_collctype))
		collctype = PQgetvalue(res, 0, i_collctype);
	else
		collctype = NULL;

	/*
	 * Before version 15, collcollate and collctype were of type NAME and
	 * non-nullable. Treat empty strings as NULL for consistency.
	 */
	if (fout->remoteVersion < 150000)
	{
		if (collcollate[0] == '\0')
			collcollate = NULL;
		if (collctype[0] == '\0')
			collctype = NULL;
	}

	if (!PQgetisnull(res, 0, i_colllocale))
		colllocale = PQgetvalue(res, 0, i_colllocale);
	else
		colllocale = NULL;

	if (!PQgetisnull(res, 0, i_collicurules))
		collicurules = PQgetvalue(res, 0, i_collicurules);
	else
		collicurules = NULL;

	appendPQExpBuffer(delq, "DROP COLLATION %s;\n",
					  fmtQualifiedDumpable(collinfo));

	appendPQExpBuffer(q, "CREATE COLLATION %s (",
					  fmtQualifiedDumpable(collinfo));

	appendPQExpBufferStr(q, "provider = ");
	if (collprovider[0] == 'b')
		appendPQExpBufferStr(q, "builtin");
	else if (collprovider[0] == 'c')
		appendPQExpBufferStr(q, "libc");
	else if (collprovider[0] == 'i')
		appendPQExpBufferStr(q, "icu");
	else if (collprovider[0] == 'd')
		/* to allow dumping pg_catalog; not accepted on input */
		appendPQExpBufferStr(q, "default");
	else
		pg_fatal("unrecognized collation provider: %s",
				 collprovider);

	if (strcmp(PQgetvalue(res, 0, i_collisdeterministic), "f") == 0)
		appendPQExpBufferStr(q, ", deterministic = false");

	if (collprovider[0] == 'd')
	{
		if (collcollate || collctype || colllocale || collicurules)
			pg_log_warning("invalid collation \"%s\"", qcollname);

		/* no locale -- the default collation cannot be reloaded anyway */
	}
	else if (collprovider[0] == 'b')
	{
		if (collcollate || collctype || !colllocale || collicurules)
			pg_log_warning("invalid collation \"%s\"", qcollname);

		appendPQExpBufferStr(q, ", locale = ");
		appendStringLiteralAH(q, colllocale ? colllocale : "",
							  fout);
	}
	else if (collprovider[0] == 'i')
	{
		if (fout->remoteVersion >= 150000)
		{
			if (collcollate || collctype || !colllocale)
				pg_log_warning("invalid collation \"%s\"", qcollname);

			appendPQExpBufferStr(q, ", locale = ");
			appendStringLiteralAH(q, colllocale ? colllocale : "",
								  fout);
		}
		else
		{
			if (!collcollate || !collctype || colllocale ||
				strcmp(collcollate, collctype) != 0)
				pg_log_warning("invalid collation \"%s\"", qcollname);

			appendPQExpBufferStr(q, ", locale = ");
			appendStringLiteralAH(q, collcollate ? collcollate : "", fout);
		}

		if (collicurules)
		{
			appendPQExpBufferStr(q, ", rules = ");
			appendStringLiteralAH(q, collicurules ? collicurules : "", fout);
		}
	}
	else if (collprovider[0] == 'c')
	{
		if (colllocale || collicurules || !collcollate || !collctype)
			pg_log_warning("invalid collation \"%s\"", qcollname);

		if (collcollate && collctype && strcmp(collcollate, collctype) == 0)
		{
			appendPQExpBufferStr(q, ", locale = ");
			appendStringLiteralAH(q, collcollate ? collcollate : "", fout);
		}
		else
		{
			appendPQExpBufferStr(q, ", lc_collate = ");
			appendStringLiteralAH(q, collcollate ? collcollate : "", fout);
			appendPQExpBufferStr(q, ", lc_ctype = ");
			appendStringLiteralAH(q, collctype ? collctype : "", fout);
		}
	}
	else
		pg_fatal("unrecognized collation provider: %s", collprovider);

	/*
	 * For binary upgrade, carry over the collation version.  For normal
	 * dump/restore, omit the version, so that it is computed upon restore.
	 */
	if (dopt->binary_upgrade)
	{
		int			i_collversion;

		i_collversion = PQfnumber(res, "collversion");
		if (!PQgetisnull(res, 0, i_collversion))
		{
			appendPQExpBufferStr(q, ", version = ");
			appendStringLiteralAH(q,
								  PQgetvalue(res, 0, i_collversion),
								  fout);
		}
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
dumpConversion(Archive *fout, const ConvInfo *convinfo)
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

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
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
format_aggregate_signature(const AggInfo *agginfo, Archive *fout, bool honor_quotes)
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
			appendPQExpBuffer(&buf, "%s%s",
							  (j > 0) ? ", " : "",
							  getFormattedTypeName(fout,
												   agginfo->aggfn.argtypes[j],
												   zeroIsError));
		appendPQExpBufferChar(&buf, ')');
	}
	return buf.data;
}

/*
 * dumpAgg
 *	  write out a single aggregate definition
 */
static void
dumpAgg(Archive *fout, const AggInfo *agginfo)
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

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
		return;

	query = createPQExpBuffer();
	q = createPQExpBuffer();
	delq = createPQExpBuffer();
	details = createPQExpBuffer();

	if (!fout->is_prepared[PREPQUERY_DUMPAGG])
	{
		/* Set up query for aggregate-specific details */
		appendPQExpBufferStr(query,
							 "PREPARE dumpAgg(pg_catalog.oid) AS\n");

		appendPQExpBufferStr(query,
							 "SELECT "
							 "aggtransfn,\n"
							 "aggfinalfn,\n"
							 "aggtranstype::pg_catalog.regtype,\n"
							 "agginitval,\n"
							 "aggsortop,\n"
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

		appendPQExpBufferStr(query,
							 "FROM pg_catalog.pg_aggregate a, pg_catalog.pg_proc p "
							 "WHERE a.aggfnoid = p.oid "
							 "AND p.oid = $1");

		ExecuteSqlStatement(fout, query->data);

		fout->is_prepared[PREPQUERY_DUMPAGG] = true;
	}

	printfPQExpBuffer(query,
					  "EXECUTE dumpAgg('%u')",
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

	{
		char	   *funcargs;
		char	   *funciargs;

		funcargs = PQgetvalue(res, 0, PQfnumber(res, "funcargs"));
		funciargs = PQgetvalue(res, 0, PQfnumber(res, "funciargs"));
		aggfullsig = format_function_arguments(&agginfo->aggfn, funcargs, true);
		aggsig = format_function_arguments(&agginfo->aggfn, funciargs, true);
	}

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
					pg_fatal("unrecognized aggfinalmodify value for aggregate \"%s\"",
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
					pg_fatal("unrecognized aggmfinalmodify value for aggregate \"%s\"",
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
			pg_fatal("unrecognized proparallel value for function \"%s\"",
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
				NULL, agginfo->aggfn.rolname, &agginfo->aggfn.dacl);

	free(aggsig);
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
dumpTSParser(Archive *fout, const TSParserInfo *prsinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *qprsname;

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
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
dumpTSDictionary(Archive *fout, const TSDictInfo *dictinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer query;
	char	   *qdictname;
	PGresult   *res;
	char	   *nspname;
	char	   *tmplname;

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
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
dumpTSTemplate(Archive *fout, const TSTemplateInfo *tmplinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *qtmplname;

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
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
dumpTSConfig(Archive *fout, const TSConfigInfo *cfginfo)
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

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
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
dumpForeignDataWrapper(Archive *fout, const FdwInfo *fdwinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *qfdwname;

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
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
				"FOREIGN DATA WRAPPER", qfdwname, NULL, NULL,
				NULL, fdwinfo->rolname, &fdwinfo->dacl);

	free(qfdwname);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
}

/*
 * dumpForeignServer
 *	  write out a foreign server definition
 */
static void
dumpForeignServer(Archive *fout, const ForeignServerInfo *srvinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer query;
	PGresult   *res;
	char	   *qsrvname;
	char	   *fdwname;

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
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
				"FOREIGN SERVER", qsrvname, NULL, NULL,
				NULL, srvinfo->rolname, &srvinfo->dacl);

	/* Dump user mappings */
	if (srvinfo->dobj.dump & DUMP_COMPONENT_USERMAP)
		dumpUserMappings(fout,
						 srvinfo->dobj.name, NULL,
						 srvinfo->rolname,
						 srvinfo->dobj.catId, srvinfo->dobj.dumpId);

	PQclear(res);

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
dumpDefaultACL(Archive *fout, const DefaultACLInfo *daclinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PQExpBuffer tag;
	const char *type;

	/* Do nothing if not dumping schema, or if we're skipping ACLs */
	if (!dopt->dumpSchema || dopt->aclsSkip)
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
			pg_fatal("unrecognized object type in default privileges: %d",
					 (int) daclinfo->defaclobjtype);
			type = "";			/* keep compiler quiet */
	}

	appendPQExpBuffer(tag, "DEFAULT PRIVILEGES FOR %s", type);

	/* build the actual command(s) for this tuple */
	if (!buildDefaultACLCommands(type,
								 daclinfo->dobj.namespace != NULL ?
								 daclinfo->dobj.namespace->dobj.name : NULL,
								 daclinfo->dacl.acl,
								 daclinfo->dacl.acldefault,
								 daclinfo->defaclrole,
								 fout->remoteVersion,
								 q))
		pg_fatal("could not parse default ACL list (%s)",
				 daclinfo->dacl.acl);

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
 * 'tag' is the tag to use for the ACL TOC entry; typically, this is NULL
 *		to use the default for the object type.
 * 'owner' is the owner, NULL if there is no owner (for languages).
 * 'dacl' is the DumpableAcl struct for the object.
 *
 * Returns the dump ID assigned to the ACL TocEntry, or InvalidDumpId if
 * no ACL entry was created.
 *----------
 */
static DumpId
dumpACL(Archive *fout, DumpId objDumpId, DumpId altDumpId,
		const char *type, const char *name, const char *subname,
		const char *nspname, const char *tag, const char *owner,
		const DumpableAcl *dacl)
{
	DumpId		aclDumpId = InvalidDumpId;
	DumpOptions *dopt = fout->dopt;
	const char *acls = dacl->acl;
	const char *acldefault = dacl->acldefault;
	char		privtype = dacl->privtype;
	const char *initprivs = dacl->initprivs;
	const char *baseacls;
	PQExpBuffer sql;

	/* Do nothing if ACL dump is not enabled */
	if (dopt->aclsSkip)
		return InvalidDumpId;

	/* --data-only skips ACLs *except* large object ACLs */
	if (!dopt->dumpSchema && strcmp(type, "LARGE OBJECT") != 0)
		return InvalidDumpId;

	sql = createPQExpBuffer();

	/*
	 * In binary upgrade mode, we don't run an extension's script but instead
	 * dump out the objects independently and then recreate them.  To preserve
	 * any initial privileges which were set on extension objects, we need to
	 * compute the set of GRANT and REVOKE commands necessary to get from the
	 * default privileges of an object to its initial privileges as recorded
	 * in pg_init_privs.
	 *
	 * At restore time, we apply these commands after having called
	 * binary_upgrade_set_record_init_privs(true).  That tells the backend to
	 * copy the results into pg_init_privs.  This is how we preserve the
	 * contents of that catalog across binary upgrades.
	 */
	if (dopt->binary_upgrade && privtype == 'e' &&
		initprivs && *initprivs != '\0')
	{
		appendPQExpBufferStr(sql, "SELECT pg_catalog.binary_upgrade_set_record_init_privs(true);\n");
		if (!buildACLCommands(name, subname, nspname, type,
							  initprivs, acldefault, owner,
							  "", fout->remoteVersion, sql))
			pg_fatal("could not parse initial ACL list (%s) or default (%s) for object \"%s\" (%s)",
					 initprivs, acldefault, name, type);
		appendPQExpBufferStr(sql, "SELECT pg_catalog.binary_upgrade_set_record_init_privs(false);\n");
	}

	/*
	 * Now figure the GRANT and REVOKE commands needed to get to the object's
	 * actual current ACL, starting from the initprivs if given, else from the
	 * object-type-specific default.  Also, while buildACLCommands will assume
	 * that a NULL/empty acls string means it needn't do anything, what that
	 * actually represents is the object-type-specific default; so we need to
	 * substitute the acldefault string to get the right results in that case.
	 */
	if (initprivs && *initprivs != '\0')
	{
		baseacls = initprivs;
		if (acls == NULL || *acls == '\0')
			acls = acldefault;
	}
	else
		baseacls = acldefault;

	if (!buildACLCommands(name, subname, nspname, type,
						  acls, baseacls, owner,
						  "", fout->remoteVersion, sql))
		pg_fatal("could not parse ACL list (%s) or default (%s) for object \"%s\" (%s)",
				 acls, baseacls, name, type);

	if (sql->len > 0)
	{
		PQExpBuffer tagbuf = createPQExpBuffer();
		DumpId		aclDeps[2];
		int			nDeps = 0;

		if (tag)
			appendPQExpBufferStr(tagbuf, tag);
		else if (subname)
			appendPQExpBuffer(tagbuf, "COLUMN %s.%s", name, subname);
		else
			appendPQExpBuffer(tagbuf, "%s %s", type, name);

		aclDeps[nDeps++] = objDumpId;
		if (altDumpId != InvalidDumpId)
			aclDeps[nDeps++] = altDumpId;

		aclDumpId = createDumpId();

		ArchiveEntry(fout, nilCatalogId, aclDumpId,
					 ARCHIVE_OPTS(.tag = tagbuf->data,
								  .namespace = nspname,
								  .owner = owner,
								  .description = "ACL",
								  .section = SECTION_NONE,
								  .createStmt = sql->data,
								  .deps = aclDeps,
								  .nDeps = nDeps));

		destroyPQExpBuffer(tagbuf);
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

	/*
	 * Security labels are schema not data ... except large object labels are
	 * data
	 */
	if (strcmp(type, "LARGE OBJECT") != 0)
	{
		if (!dopt->dumpSchema)
			return;
	}
	else
	{
		/* We do dump large object security labels in binary-upgrade mode */
		if (!dopt->dumpData && !dopt->binary_upgrade)
			return;
	}

	/* Search for security labels associated with catalogId, using table */
	nlabels = findSecLabels(catalogId.tableoid, catalogId.oid, &labels);

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
dumpTableSecLabel(Archive *fout, const TableInfo *tbinfo, const char *reltypename)
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
	if (!dopt->dumpSchema)
		return;

	/* Search for comments associated with relation, using table */
	nlabels = findSecLabels(tbinfo->dobj.catId.tableoid,
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
findSecLabels(Oid classoid, Oid objoid, SecLabelItem **items)
{
	SecLabelItem *middle = NULL;
	SecLabelItem *low;
	SecLabelItem *high;
	int			nmatch;

	if (nseclabels <= 0)		/* no labels, so no match is possible */
	{
		*items = NULL;
		return 0;
	}

	/*
	 * Do binary search to find some item matching the object.
	 */
	low = &seclabels[0];
	high = &seclabels[nseclabels - 1];
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
 * Construct a table of all security labels available for database objects;
 * also set the has-seclabel component flag for each relevant object.
 *
 * The table is sorted by classoid/objid/objsubid for speed in lookup.
 */
static void
collectSecLabels(Archive *fout)
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
	DumpableObject *dobj;

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

	seclabels = (SecLabelItem *) pg_malloc(ntups * sizeof(SecLabelItem));
	nseclabels = 0;
	dobj = NULL;

	for (i = 0; i < ntups; i++)
	{
		CatalogId	objId;
		int			subid;

		objId.tableoid = atooid(PQgetvalue(res, i, i_classoid));
		objId.oid = atooid(PQgetvalue(res, i, i_objoid));
		subid = atoi(PQgetvalue(res, i, i_objsubid));

		/* We needn't remember labels that don't match any dumpable object */
		if (dobj == NULL ||
			dobj->catId.tableoid != objId.tableoid ||
			dobj->catId.oid != objId.oid)
			dobj = findObjectByCatalogId(objId);
		if (dobj == NULL)
			continue;

		/*
		 * Labels on columns of composite types are linked to the type's
		 * pg_class entry, but we need to set the DUMP_COMPONENT_SECLABEL flag
		 * in the type's own DumpableObject.
		 */
		if (subid != 0 && dobj->objType == DO_TABLE &&
			((TableInfo *) dobj)->relkind == RELKIND_COMPOSITE_TYPE)
		{
			TypeInfo   *cTypeInfo;

			cTypeInfo = findTypeByOid(((TableInfo *) dobj)->reltype);
			if (cTypeInfo)
				cTypeInfo->dobj.components |= DUMP_COMPONENT_SECLABEL;
		}
		else
			dobj->components |= DUMP_COMPONENT_SECLABEL;

		seclabels[nseclabels].label = pg_strdup(PQgetvalue(res, i, i_label));
		seclabels[nseclabels].provider = pg_strdup(PQgetvalue(res, i, i_provider));
		seclabels[nseclabels].classoid = objId.tableoid;
		seclabels[nseclabels].objoid = objId.oid;
		seclabels[nseclabels].objsubid = subid;
		nseclabels++;
	}

	PQclear(res);
	destroyPQExpBuffer(query);
}

/*
 * dumpTable
 *	  write out to fout the declarations (not data) of a user-defined table
 */
static void
dumpTable(Archive *fout, const TableInfo *tbinfo)
{
	DumpOptions *dopt = fout->dopt;
	DumpId		tableAclDumpId = InvalidDumpId;
	char	   *namecopy;

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
		return;

	if (tbinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
	{
		if (tbinfo->relkind == RELKIND_SEQUENCE)
			dumpSequence(fout, tbinfo);
		else
			dumpTableSchema(fout, tbinfo);
	}

	/* Handle the ACL here */
	namecopy = pg_strdup(fmtId(tbinfo->dobj.name));
	if (tbinfo->dobj.dump & DUMP_COMPONENT_ACL)
	{
		const char *objtype =
			(tbinfo->relkind == RELKIND_SEQUENCE) ? "SEQUENCE" : "TABLE";

		tableAclDumpId =
			dumpACL(fout, tbinfo->dobj.dumpId, InvalidDumpId,
					objtype, namecopy, NULL,
					tbinfo->dobj.namespace->dobj.name,
					NULL, tbinfo->rolname, &tbinfo->dacl);
	}

	/*
	 * Handle column ACLs, if any.  Note: we pull these with a separate query
	 * rather than trying to fetch them during getTableAttrs, so that we won't
	 * miss ACLs on system columns.  Doing it this way also allows us to dump
	 * ACLs for catalogs that we didn't mark "interesting" back in getTables.
	 */
	if ((tbinfo->dobj.dump & DUMP_COMPONENT_ACL) && tbinfo->hascolumnACLs)
	{
		PQExpBuffer query = createPQExpBuffer();
		PGresult   *res;
		int			i;

		if (!fout->is_prepared[PREPQUERY_GETCOLUMNACLS])
		{
			/* Set up query for column ACLs */
			appendPQExpBufferStr(query,
								 "PREPARE getColumnACLs(pg_catalog.oid) AS\n");

			if (fout->remoteVersion >= 90600)
			{
				/*
				 * In principle we should call acldefault('c', relowner) to
				 * get the default ACL for a column.  However, we don't
				 * currently store the numeric OID of the relowner in
				 * TableInfo.  We could convert the owner name using regrole,
				 * but that creates a risk of failure due to concurrent role
				 * renames.  Given that the default ACL for columns is empty
				 * and is likely to stay that way, it's not worth extra cycles
				 * and risk to avoid hard-wiring that knowledge here.
				 */
				appendPQExpBufferStr(query,
									 "SELECT at.attname, "
									 "at.attacl, "
									 "'{}' AS acldefault, "
									 "pip.privtype, pip.initprivs "
									 "FROM pg_catalog.pg_attribute at "
									 "LEFT JOIN pg_catalog.pg_init_privs pip ON "
									 "(at.attrelid = pip.objoid "
									 "AND pip.classoid = 'pg_catalog.pg_class'::pg_catalog.regclass "
									 "AND at.attnum = pip.objsubid) "
									 "WHERE at.attrelid = $1 AND "
									 "NOT at.attisdropped "
									 "AND (at.attacl IS NOT NULL OR pip.initprivs IS NOT NULL) "
									 "ORDER BY at.attnum");
			}
			else
			{
				appendPQExpBufferStr(query,
									 "SELECT attname, attacl, '{}' AS acldefault, "
									 "NULL AS privtype, NULL AS initprivs "
									 "FROM pg_catalog.pg_attribute "
									 "WHERE attrelid = $1 AND NOT attisdropped "
									 "AND attacl IS NOT NULL "
									 "ORDER BY attnum");
			}

			ExecuteSqlStatement(fout, query->data);

			fout->is_prepared[PREPQUERY_GETCOLUMNACLS] = true;
		}

		printfPQExpBuffer(query,
						  "EXECUTE getColumnACLs('%u')",
						  tbinfo->dobj.catId.oid);

		res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

		for (i = 0; i < PQntuples(res); i++)
		{
			char	   *attname = PQgetvalue(res, i, 0);
			char	   *attacl = PQgetvalue(res, i, 1);
			char	   *acldefault = PQgetvalue(res, i, 2);
			char		privtype = *(PQgetvalue(res, i, 3));
			char	   *initprivs = PQgetvalue(res, i, 4);
			DumpableAcl coldacl;
			char	   *attnamecopy;

			coldacl.acl = attacl;
			coldacl.acldefault = acldefault;
			coldacl.privtype = privtype;
			coldacl.initprivs = initprivs;
			attnamecopy = pg_strdup(fmtId(attname));

			/*
			 * Column's GRANT type is always TABLE.  Each column ACL depends
			 * on the table-level ACL, since we can restore column ACLs in
			 * parallel but the table-level ACL has to be done first.
			 */
			dumpACL(fout, tbinfo->dobj.dumpId, tableAclDumpId,
					"TABLE", namecopy, attnamecopy,
					tbinfo->dobj.namespace->dobj.name,
					NULL, tbinfo->rolname, &coldacl);
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
createViewAsClause(Archive *fout, const TableInfo *tbinfo)
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
			pg_fatal("query to obtain definition of view \"%s\" returned no data",
					 tbinfo->dobj.name);
		else
			pg_fatal("query to obtain definition of view \"%s\" returned more than one definition",
					 tbinfo->dobj.name);
	}

	len = PQgetlength(res, 0, 0);

	if (len == 0)
		pg_fatal("definition of view \"%s\" appears to be empty (length zero)",
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
createDummyViewAsClause(Archive *fout, const TableInfo *tbinfo)
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
dumpTableSchema(Archive *fout, const TableInfo *tbinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	PQExpBuffer extra = createPQExpBuffer();
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
		binary_upgrade_set_type_oids_by_rel(fout, q, tbinfo);

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
											 tbinfo->dobj.catId.oid);

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
		char	   *partkeydef = NULL;
		char	   *ftoptions = NULL;
		char	   *srvname = NULL;
		const char *foreign = "";

		/*
		 * Set reltypename, and collect any relkind-specific data that we
		 * didn't fetch during getTables().
		 */
		switch (tbinfo->relkind)
		{
			case RELKIND_PARTITIONED_TABLE:
				{
					PQExpBuffer query = createPQExpBuffer();
					PGresult   *res;

					reltypename = "TABLE";

					/* retrieve partition key definition */
					appendPQExpBuffer(query,
									  "SELECT pg_get_partkeydef('%u')",
									  tbinfo->dobj.catId.oid);
					res = ExecuteSqlQueryForSingleRow(fout, query->data);
					partkeydef = pg_strdup(PQgetvalue(res, 0, 0));
					PQclear(res);
					destroyPQExpBuffer(query);
					break;
				}
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
				break;
		}

		numParents = tbinfo->numParents;
		parents = tbinfo->parents;

		appendPQExpBuffer(delq, "DROP %s %s;\n", reltypename, qualrelname);

		if (dopt->binary_upgrade)
			binary_upgrade_set_pg_class_oids(fout, q,
											 tbinfo->dobj.catId.oid);

		/*
		 * PostgreSQL 18 has disabled UNLOGGED for partitioned tables, so
		 * ignore it when dumping if it was set in this case.
		 */
		appendPQExpBuffer(q, "CREATE %s%s %s",
						  (tbinfo->relpersistence == RELPERSISTENCE_UNLOGGED &&
						   tbinfo->relkind != RELKIND_PARTITIONED_TABLE) ?
						  "UNLOGGED " : "",
						  reltypename,
						  qualrelname);

		/*
		 * Attach to type, if reloftype; except in case of a binary upgrade,
		 * we dump the table normally and attach it to the type afterward.
		 */
		if (OidIsValid(tbinfo->reloftype) && !dopt->binary_upgrade)
			appendPQExpBuffer(q, " OF %s",
							  getFormattedTypeName(fout, tbinfo->reloftype,
												   zeroIsError));

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
					 * Default value --- suppress if to be printed separately
					 * or not at all.
					 */
					print_default = (tbinfo->attrdefs[j] != NULL &&
									 tbinfo->attrdefs[j]->dobj.dump &&
									 !tbinfo->attrdefs[j]->separate);

					/*
					 * Not Null constraint --- print it if it is locally
					 * defined, or if binary upgrade.  (In the latter case, we
					 * reset conislocal below.)
					 */
					print_notnull = (tbinfo->notnull_constrs[j] != NULL &&
									 (tbinfo->notnull_islocal[j] ||
									  dopt->binary_upgrade ||
									  tbinfo->ispartition));

					/*
					 * Skip column if fully defined by reloftype, except in
					 * binary upgrade
					 */
					if (OidIsValid(tbinfo->reloftype) &&
						!print_default && !print_notnull &&
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
					if (dopt->binary_upgrade || !OidIsValid(tbinfo->reloftype))
					{
						appendPQExpBuffer(q, " %s",
										  tbinfo->atttypnames[j]);
					}

					if (print_default)
					{
						if (tbinfo->attgenerated[j] == ATTRIBUTE_GENERATED_STORED)
							appendPQExpBuffer(q, " GENERATED ALWAYS AS (%s) STORED",
											  tbinfo->attrdefs[j]->adef_expr);
						else if (tbinfo->attgenerated[j] == ATTRIBUTE_GENERATED_VIRTUAL)
							appendPQExpBuffer(q, " GENERATED ALWAYS AS (%s)",
											  tbinfo->attrdefs[j]->adef_expr);
						else
							appendPQExpBuffer(q, " DEFAULT %s",
											  tbinfo->attrdefs[j]->adef_expr);
					}

					print_notnull = (tbinfo->notnull_constrs[j] != NULL &&
									 (tbinfo->notnull_islocal[j] ||
									  dopt->binary_upgrade ||
									  tbinfo->ispartition));

					if (print_notnull)
					{
						if (tbinfo->notnull_constrs[j][0] == '\0')
							appendPQExpBufferStr(q, " NOT NULL");
						else
							appendPQExpBuffer(q, " CONSTRAINT %s NOT NULL",
											  fmtId(tbinfo->notnull_constrs[j]));

						if (tbinfo->notnull_noinh[j])
							appendPQExpBufferStr(q, " NO INHERIT");
					}

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

				/*
				 * On the other hand, if we choose not to print a column
				 * (likely because it is created by inheritance), but the
				 * column has a locally-defined not-null constraint, we need
				 * to dump the constraint as a standalone object.
				 *
				 * This syntax isn't SQL-conforming, but if you wanted
				 * standard output you wouldn't be creating non-standard
				 * objects to begin with.
				 */
				if (!shouldPrintColumn(dopt, tbinfo, j) &&
					!tbinfo->attisdropped[j] &&
					tbinfo->notnull_constrs[j] != NULL &&
					tbinfo->notnull_islocal[j])
				{
					/* Format properly if not first attr */
					if (actual_atts == 0)
						appendPQExpBufferStr(q, " (");
					else
						appendPQExpBufferChar(q, ',');
					appendPQExpBufferStr(q, "\n    ");
					actual_atts++;

					if (tbinfo->notnull_constrs[j][0] == '\0')
						appendPQExpBuffer(q, "NOT NULL %s",
										  fmtId(tbinfo->attnames[j]));
					else
						appendPQExpBuffer(q, "CONSTRAINT %s NOT NULL %s",
										  tbinfo->notnull_constrs[j],
										  fmtId(tbinfo->attnames[j]));
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
			else if (!(OidIsValid(tbinfo->reloftype) && !dopt->binary_upgrade))
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
				appendPQExpBuffer(q, "\nPARTITION BY %s", partkeydef);

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
										"MATERIALIZED VIEW",
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
					appendPQExpBufferChar(q, ',');
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
			bool		firstitem;
			bool		firstitem_extra;

			/*
			 * Drop any dropped columns.  Merge the pg_attribute manipulations
			 * into a single SQL command, so that we don't cause repeated
			 * relcache flushes on the target table.  Otherwise we risk O(N^2)
			 * relcache bloat while dropping N columns.
			 */
			resetPQExpBuffer(extra);
			firstitem = true;
			for (j = 0; j < tbinfo->numatts; j++)
			{
				if (tbinfo->attisdropped[j])
				{
					if (firstitem)
					{
						appendPQExpBufferStr(q, "\n-- For binary upgrade, recreate dropped columns.\n"
											 "UPDATE pg_catalog.pg_attribute\n"
											 "SET attlen = v.dlen, "
											 "attalign = v.dalign, "
											 "attbyval = false\n"
											 "FROM (VALUES ");
						firstitem = false;
					}
					else
						appendPQExpBufferStr(q, ",\n             ");
					appendPQExpBufferChar(q, '(');
					appendStringLiteralAH(q, tbinfo->attnames[j], fout);
					appendPQExpBuffer(q, ", %d, '%c')",
									  tbinfo->attlen[j],
									  tbinfo->attalign[j]);
					/* The ALTER ... DROP COLUMN commands must come after */
					appendPQExpBuffer(extra, "ALTER %sTABLE ONLY %s ",
									  foreign, qualrelname);
					appendPQExpBuffer(extra, "DROP COLUMN %s;\n",
									  fmtId(tbinfo->attnames[j]));
				}
			}
			if (!firstitem)
			{
				appendPQExpBufferStr(q, ") v(dname, dlen, dalign)\n"
									 "WHERE attrelid = ");
				appendStringLiteralAH(q, qualrelname, fout);
				appendPQExpBufferStr(q, "::pg_catalog.regclass\n"
									 "  AND attname = v.dname;\n");
				/* Now we can issue the actual DROP COLUMN commands */
				appendBinaryPQExpBuffer(q, extra->data, extra->len);
			}

			/*
			 * Fix up inherited columns.  As above, do the pg_attribute
			 * manipulations in a single SQL command.
			 */
			firstitem = true;
			for (j = 0; j < tbinfo->numatts; j++)
			{
				if (!tbinfo->attisdropped[j] &&
					!tbinfo->attislocal[j])
				{
					if (firstitem)
					{
						appendPQExpBufferStr(q, "\n-- For binary upgrade, recreate inherited columns.\n");
						appendPQExpBufferStr(q, "UPDATE pg_catalog.pg_attribute\n"
											 "SET attislocal = false\n"
											 "WHERE attrelid = ");
						appendStringLiteralAH(q, qualrelname, fout);
						appendPQExpBufferStr(q, "::pg_catalog.regclass\n"
											 "  AND attname IN (");
						firstitem = false;
					}
					else
						appendPQExpBufferStr(q, ", ");
					appendStringLiteralAH(q, tbinfo->attnames[j], fout);
				}
			}
			if (!firstitem)
				appendPQExpBufferStr(q, ");\n");

			/*
			 * Fix up not-null constraints that come from inheritance.  As
			 * above, do the pg_constraint manipulations in a single SQL
			 * command.  (Actually, two in special cases, if we're doing an
			 * upgrade from < 18).
			 */
			firstitem = true;
			firstitem_extra = true;
			resetPQExpBuffer(extra);
			for (j = 0; j < tbinfo->numatts; j++)
			{
				/*
				 * If a not-null constraint comes from inheritance, reset
				 * conislocal.  The inhcount is fixed by ALTER TABLE INHERIT,
				 * below.  Special hack: in versions < 18, columns with no
				 * local definition need their constraint to be matched by
				 * column number in conkeys instead of by constraint name,
				 * because the latter is not available.  (We distinguish the
				 * case because the constraint name is the empty string.)
				 */
				if (tbinfo->notnull_constrs[j] != NULL &&
					!tbinfo->notnull_islocal[j])
				{
					if (tbinfo->notnull_constrs[j][0] != '\0')
					{
						if (firstitem)
						{
							appendPQExpBufferStr(q, "UPDATE pg_catalog.pg_constraint\n"
												 "SET conislocal = false\n"
												 "WHERE contype = 'n' AND conrelid = ");
							appendStringLiteralAH(q, qualrelname, fout);
							appendPQExpBufferStr(q, "::pg_catalog.regclass AND\n"
												 "conname IN (");
							firstitem = false;
						}
						else
							appendPQExpBufferStr(q, ", ");
						appendStringLiteralAH(q, tbinfo->notnull_constrs[j], fout);
					}
					else
					{
						if (firstitem_extra)
						{
							appendPQExpBufferStr(extra, "UPDATE pg_catalog.pg_constraint\n"
												 "SET conislocal = false\n"
												 "WHERE contype = 'n' AND conrelid = ");
							appendStringLiteralAH(extra, qualrelname, fout);
							appendPQExpBufferStr(extra, "::pg_catalog.regclass AND\n"
												 "conkey IN (");
							firstitem_extra = false;
						}
						else
							appendPQExpBufferStr(extra, ", ");
						appendPQExpBuffer(extra, "'{%d}'", j + 1);
					}
				}
			}
			if (!firstitem)
				appendPQExpBufferStr(q, ");\n");
			if (!firstitem_extra)
				appendPQExpBufferStr(extra, ");\n");

			if (extra->len > 0)
				appendBinaryPQExpBuffer(q, extra->data, extra->len);

			/*
			 * Add inherited CHECK constraints, if any.
			 *
			 * For partitions, they were already dumped, and conislocal
			 * doesn't need fixing.
			 *
			 * As above, issue only one direct manipulation of pg_constraint.
			 * Although it is tempting to merge the ALTER ADD CONSTRAINT
			 * commands into one as well, refrain for now due to concern about
			 * possible backend memory bloat if there are many such
			 * constraints.
			 */
			resetPQExpBuffer(extra);
			firstitem = true;
			for (k = 0; k < tbinfo->ncheck; k++)
			{
				ConstraintInfo *constr = &(tbinfo->checkexprs[k]);

				if (constr->separate || constr->conislocal || tbinfo->ispartition)
					continue;

				if (firstitem)
					appendPQExpBufferStr(q, "\n-- For binary upgrade, set up inherited constraints.\n");
				appendPQExpBuffer(q, "ALTER %sTABLE ONLY %s ADD CONSTRAINT %s %s;\n",
								  foreign, qualrelname,
								  fmtId(constr->dobj.name),
								  constr->condef);
				/* Update pg_constraint after all the ALTER TABLEs */
				if (firstitem)
				{
					appendPQExpBufferStr(extra, "UPDATE pg_catalog.pg_constraint\n"
										 "SET conislocal = false\n"
										 "WHERE contype = 'c' AND conrelid = ");
					appendStringLiteralAH(extra, qualrelname, fout);
					appendPQExpBufferStr(extra, "::pg_catalog.regclass\n");
					appendPQExpBufferStr(extra, "  AND conname IN (");
					firstitem = false;
				}
				else
					appendPQExpBufferStr(extra, ", ");
				appendStringLiteralAH(extra, constr->dobj.name, fout);
			}
			if (!firstitem)
			{
				appendPQExpBufferStr(extra, ");\n");
				appendBinaryPQExpBuffer(q, extra->data, extra->len);
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

			if (OidIsValid(tbinfo->reloftype))
			{
				appendPQExpBufferStr(q, "\n-- For binary upgrade, set up typed tables this way.\n");
				appendPQExpBuffer(q, "ALTER TABLE ONLY %s OF %s;\n",
								  qualrelname,
								  getFormattedTypeName(fout, tbinfo->reloftype,
													   zeroIsError));
			}
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
			 * Dump per-column statistics information. We only issue an ALTER
			 * TABLE statement if the attstattarget entry for this column is
			 * not the default value.
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
			 * Dump per-column compression, if it's been set.
			 */
			if (!dopt->no_toast_compression)
			{
				const char *cmname;

				switch (tbinfo->attcompression[j])
				{
					case 'p':
						cmname = "pglz";
						break;
					case 'l':
						cmname = "lz4";
						break;
					default:
						cmname = NULL;
						break;
				}

				if (cmname != NULL)
					appendPQExpBuffer(q, "ALTER %sTABLE ONLY %s ALTER COLUMN %s SET COMPRESSION %s;\n",
									  foreign, qualrelname,
									  fmtId(tbinfo->attnames[j]),
									  cmname);
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
								  "ALTER FOREIGN TABLE ONLY %s ALTER COLUMN %s OPTIONS (\n"
								  "    %s\n"
								  ");\n",
								  qualrelname,
								  fmtId(tbinfo->attnames[j]),
								  tbinfo->attfdwoptions[j]);
		}						/* end loop over columns */

		free(partkeydef);
		free(ftoptions);
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
		char	   *tablespace = NULL;
		char	   *tableam = NULL;

		/*
		 * _selectTablespace() relies on tablespace-enabled objects in the
		 * default tablespace to have a tablespace of "" (empty string) versus
		 * non-tablespace-enabled objects to have a tablespace of NULL.
		 * getTables() sets tbinfo->reltablespace to "" for the default
		 * tablespace (not NULL).
		 */
		if (RELKIND_HAS_TABLESPACE(tbinfo->relkind))
			tablespace = tbinfo->reltablespace;

		if (RELKIND_HAS_TABLE_AM(tbinfo->relkind) ||
			tbinfo->relkind == RELKIND_PARTITIONED_TABLE)
			tableam = tbinfo->amname;

		ArchiveEntry(fout, tbinfo->dobj.catId, tbinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tbinfo->dobj.name,
								  .namespace = tbinfo->dobj.namespace->dobj.name,
								  .tablespace = tablespace,
								  .tableam = tableam,
								  .relkind = tbinfo->relkind,
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

		if (constr->dobj.dump & DUMP_COMPONENT_COMMENT)
			dumpTableConstraintComment(fout, constr);
	}

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(extra);
	free(qrelname);
	free(qualrelname);
}

/*
 * dumpTableAttach
 *	  write to fout the commands to attach a child partition
 *
 * Child partitions are always made by creating them separately
 * and then using ATTACH PARTITION, rather than using
 * CREATE TABLE ... PARTITION OF.  This is important for preserving
 * any possible discrepancy in column layout, to allow assigning the
 * correct tablespace if different, and so that it's possible to restore
 * a partition without restoring its parent.  (You'll get an error from
 * the ATTACH PARTITION command, but that can be ignored, or skipped
 * using "pg_restore -L" if you prefer.)  The last point motivates
 * treating ATTACH PARTITION as a completely separate ArchiveEntry
 * rather than emitting it within the child partition's ArchiveEntry.
 */
static void
dumpTableAttach(Archive *fout, const TableAttachInfo *attachinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PGresult   *res;
	char	   *partbound;

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
		return;

	q = createPQExpBuffer();

	if (!fout->is_prepared[PREPQUERY_DUMPTABLEATTACH])
	{
		/* Set up query for partbound details */
		appendPQExpBufferStr(q,
							 "PREPARE dumpTableAttach(pg_catalog.oid) AS\n");

		appendPQExpBufferStr(q,
							 "SELECT pg_get_expr(c.relpartbound, c.oid) "
							 "FROM pg_class c "
							 "WHERE c.oid = $1");

		ExecuteSqlStatement(fout, q->data);

		fout->is_prepared[PREPQUERY_DUMPTABLEATTACH] = true;
	}

	printfPQExpBuffer(q,
					  "EXECUTE dumpTableAttach('%u')",
					  attachinfo->partitionTbl->dobj.catId.oid);

	res = ExecuteSqlQueryForSingleRow(fout, q->data);
	partbound = PQgetvalue(res, 0, 0);

	/* Perform ALTER TABLE on the parent */
	printfPQExpBuffer(q,
					  "ALTER TABLE ONLY %s ",
					  fmtQualifiedDumpable(attachinfo->parentTbl));
	appendPQExpBuffer(q,
					  "ATTACH PARTITION %s %s;\n",
					  fmtQualifiedDumpable(attachinfo->partitionTbl),
					  partbound);

	/*
	 * There is no point in creating a drop query as the drop is done by table
	 * drop.  (If you think to change this, see also _printTocEntry().)
	 * Although this object doesn't really have ownership as such, set the
	 * owner field anyway to ensure that the command is run by the correct
	 * role at restore time.
	 */
	ArchiveEntry(fout, attachinfo->dobj.catId, attachinfo->dobj.dumpId,
				 ARCHIVE_OPTS(.tag = attachinfo->dobj.name,
							  .namespace = attachinfo->dobj.namespace->dobj.name,
							  .owner = attachinfo->partitionTbl->rolname,
							  .description = "TABLE ATTACH",
							  .section = SECTION_PRE_DATA,
							  .createStmt = q->data));

	PQclear(res);
	destroyPQExpBuffer(q);
}

/*
 * dumpAttrDef --- dump an attribute's default-value declaration
 */
static void
dumpAttrDef(Archive *fout, const AttrDefInfo *adinfo)
{
	DumpOptions *dopt = fout->dopt;
	TableInfo  *tbinfo = adinfo->adtable;
	int			adnum = adinfo->adnum;
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *qualrelname;
	char	   *tag;
	char	   *foreign;

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
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
getAttrName(int attrnum, const TableInfo *tblInfo)
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
	pg_fatal("invalid column number %d for table \"%s\"",
			 attrnum, tblInfo->dobj.name);
	return NULL;				/* keep compiler quiet */
}

/*
 * dumpIndex
 *	  write out to fout a user-defined index
 */
static void
dumpIndex(Archive *fout, const IndxInfo *indxinfo)
{
	DumpOptions *dopt = fout->dopt;
	TableInfo  *tbinfo = indxinfo->indextable;
	bool		is_constraint = (indxinfo->indexconstraint != 0);
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *qindxname;
	char	   *qqindxname;

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	qindxname = pg_strdup(fmtId(indxinfo->dobj.name));
	qqindxname = pg_strdup(fmtQualifiedDumpable(indxinfo));

	/*
	 * If there's an associated constraint, don't dump the index per se, but
	 * do dump any comment for it.  (This is safe because dependency ordering
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
											 indxinfo->dobj.catId.oid);

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
				pg_fatal("could not parse index statistic columns");
			if (!parsePGArray(indstatvals, &indstatvalsarray, &nstatvals))
				pg_fatal("could not parse index statistic values");
			if (nstatcols != nstatvals)
				pg_fatal("mismatched number of columns and values for index statistics");

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

		free(indstatcolsarray);
		free(indstatvalsarray);
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
dumpIndexAttach(Archive *fout, const IndexAttachInfo *attachinfo)
{
	/* Do nothing if not dumping schema */
	if (!fout->dopt->dumpSchema)
		return;

	if (attachinfo->partitionIdx->dobj.dump & DUMP_COMPONENT_DEFINITION)
	{
		PQExpBuffer q = createPQExpBuffer();

		appendPQExpBuffer(q, "ALTER INDEX %s ",
						  fmtQualifiedDumpable(attachinfo->parentIdx));
		appendPQExpBuffer(q, "ATTACH PARTITION %s;\n",
						  fmtQualifiedDumpable(attachinfo->partitionIdx));

		/*
		 * There is no point in creating a drop query as the drop is done by
		 * index drop.  (If you think to change this, see also
		 * _printTocEntry().)  Although this object doesn't really have
		 * ownership as such, set the owner field anyway to ensure that the
		 * command is run by the correct role at restore time.
		 */
		ArchiveEntry(fout, attachinfo->dobj.catId, attachinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = attachinfo->dobj.name,
								  .namespace = attachinfo->dobj.namespace->dobj.name,
								  .owner = attachinfo->parentIdx->indextable->rolname,
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
dumpStatisticsExt(Archive *fout, const StatsExtInfo *statsextinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer query;
	char	   *qstatsextname;
	PGresult   *res;
	char	   *stxdef;

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
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
	 * for this statistics object is not the default value.
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
dumpConstraint(Archive *fout, const ConstraintInfo *coninfo)
{
	DumpOptions *dopt = fout->dopt;
	TableInfo  *tbinfo = coninfo->contable;
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *tag = NULL;
	char	   *foreign;

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
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
			pg_fatal("missing index for constraint \"%s\"",
					 coninfo->dobj.name);

		if (dopt->binary_upgrade)
			binary_upgrade_set_pg_class_oids(fout, q,
											 indxinfo->dobj.catId.oid);

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
			appendPQExpBufferStr(q,
								 coninfo->contype == 'p' ? "PRIMARY KEY" : "UNIQUE");

			/*
			 * PRIMARY KEY constraints should not be using NULLS NOT DISTINCT
			 * indexes. Being able to create this was fixed, but we need to
			 * make the index distinct in order to be able to restore the
			 * dump.
			 */
			if (indxinfo->indnullsnotdistinct && coninfo->contype != 'p')
				appendPQExpBufferStr(q, " NULLS NOT DISTINCT");
			appendPQExpBufferStr(q, " (");
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
			if (coninfo->conperiod)
				appendPQExpBufferStr(q, " WITHOUT OVERLAPS");

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
		pg_fatal("unrecognized constraint type: %c",
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
dumpTableConstraintComment(Archive *fout, const ConstraintInfo *coninfo)
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

static inline SeqType
parse_sequence_type(const char *name)
{
	for (int i = 0; i < lengthof(SeqTypeNames); i++)
	{
		if (strcmp(SeqTypeNames[i], name) == 0)
			return (SeqType) i;
	}

	pg_fatal("unrecognized sequence type: %s", name);
	return (SeqType) 0;			/* keep compiler quiet */
}

/*
 * bsearch() comparator for SequenceItem
 */
static int
SequenceItemCmp(const void *p1, const void *p2)
{
	SequenceItem v1 = *((const SequenceItem *) p1);
	SequenceItem v2 = *((const SequenceItem *) p2);

	return pg_cmp_u32(v1.oid, v2.oid);
}

/*
 * collectSequences
 *
 * Construct a table of sequence information.  This table is sorted by OID for
 * speed in lookup.
 */
static void
collectSequences(Archive *fout)
{
	PGresult   *res;
	const char *query;

	/*
	 * Before Postgres 10, sequence metadata is in the sequence itself.  With
	 * some extra effort, we might be able to use the sorted table for those
	 * versions, but for now it seems unlikely to be worth it.
	 *
	 * Since version 18, we can gather the sequence data in this query with
	 * pg_get_sequence_data(), but we only do so for non-schema-only dumps.
	 */
	if (fout->remoteVersion < 100000)
		return;
	else if (fout->remoteVersion < 180000 ||
			 (!fout->dopt->dumpData && !fout->dopt->sequence_data))
		query = "SELECT seqrelid, format_type(seqtypid, NULL), "
			"seqstart, seqincrement, "
			"seqmax, seqmin, "
			"seqcache, seqcycle, "
			"NULL, 'f' "
			"FROM pg_catalog.pg_sequence "
			"ORDER BY seqrelid";
	else
		query = "SELECT seqrelid, format_type(seqtypid, NULL), "
			"seqstart, seqincrement, "
			"seqmax, seqmin, "
			"seqcache, seqcycle, "
			"last_value, is_called "
			"FROM pg_catalog.pg_sequence, "
			"pg_get_sequence_data(seqrelid) "
			"ORDER BY seqrelid;";

	res = ExecuteSqlQuery(fout, query, PGRES_TUPLES_OK);

	nsequences = PQntuples(res);
	sequences = (SequenceItem *) pg_malloc(nsequences * sizeof(SequenceItem));

	for (int i = 0; i < nsequences; i++)
	{
		sequences[i].oid = atooid(PQgetvalue(res, i, 0));
		sequences[i].seqtype = parse_sequence_type(PQgetvalue(res, i, 1));
		sequences[i].startv = strtoi64(PQgetvalue(res, i, 2), NULL, 10);
		sequences[i].incby = strtoi64(PQgetvalue(res, i, 3), NULL, 10);
		sequences[i].maxv = strtoi64(PQgetvalue(res, i, 4), NULL, 10);
		sequences[i].minv = strtoi64(PQgetvalue(res, i, 5), NULL, 10);
		sequences[i].cache = strtoi64(PQgetvalue(res, i, 6), NULL, 10);
		sequences[i].cycled = (strcmp(PQgetvalue(res, i, 7), "t") == 0);
		sequences[i].last_value = strtoi64(PQgetvalue(res, i, 8), NULL, 10);
		sequences[i].is_called = (strcmp(PQgetvalue(res, i, 9), "t") == 0);
	}

	PQclear(res);
}

/*
 * dumpSequence
 *	  write the declaration (not data) of one user-defined sequence
 */
static void
dumpSequence(Archive *fout, const TableInfo *tbinfo)
{
	DumpOptions *dopt = fout->dopt;
	SequenceItem *seq;
	bool		is_ascending;
	int64		default_minv,
				default_maxv;
	PQExpBuffer query = createPQExpBuffer();
	PQExpBuffer delqry = createPQExpBuffer();
	char	   *qseqname;
	TableInfo  *owning_tab = NULL;

	qseqname = pg_strdup(fmtId(tbinfo->dobj.name));

	/*
	 * For versions >= 10, the sequence information is gathered in a sorted
	 * table before any calls to dumpSequence().  See collectSequences() for
	 * more information.
	 */
	if (fout->remoteVersion >= 100000)
	{
		SequenceItem key = {0};

		Assert(sequences);

		key.oid = tbinfo->dobj.catId.oid;
		seq = bsearch(&key, sequences, nsequences,
					  sizeof(SequenceItem), SequenceItemCmp);
	}
	else
	{
		PGresult   *res;

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

		res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

		if (PQntuples(res) != 1)
			pg_fatal(ngettext("query to get data of sequence \"%s\" returned %d row (expected 1)",
							  "query to get data of sequence \"%s\" returned %d rows (expected 1)",
							  PQntuples(res)),
					 tbinfo->dobj.name, PQntuples(res));

		seq = pg_malloc0(sizeof(SequenceItem));
		seq->seqtype = parse_sequence_type(PQgetvalue(res, 0, 0));
		seq->startv = strtoi64(PQgetvalue(res, 0, 1), NULL, 10);
		seq->incby = strtoi64(PQgetvalue(res, 0, 2), NULL, 10);
		seq->maxv = strtoi64(PQgetvalue(res, 0, 3), NULL, 10);
		seq->minv = strtoi64(PQgetvalue(res, 0, 4), NULL, 10);
		seq->cache = strtoi64(PQgetvalue(res, 0, 5), NULL, 10);
		seq->cycled = (strcmp(PQgetvalue(res, 0, 6), "t") == 0);

		PQclear(res);
	}

	/* Calculate default limits for a sequence of this type */
	is_ascending = (seq->incby >= 0);
	if (seq->seqtype == SEQTYPE_SMALLINT)
	{
		default_minv = is_ascending ? 1 : PG_INT16_MIN;
		default_maxv = is_ascending ? PG_INT16_MAX : -1;
	}
	else if (seq->seqtype == SEQTYPE_INTEGER)
	{
		default_minv = is_ascending ? 1 : PG_INT32_MIN;
		default_maxv = is_ascending ? PG_INT32_MAX : -1;
	}
	else if (seq->seqtype == SEQTYPE_BIGINT)
	{
		default_minv = is_ascending ? 1 : PG_INT64_MIN;
		default_maxv = is_ascending ? PG_INT64_MAX : -1;
	}
	else
	{
		pg_fatal("unrecognized sequence type: %d", seq->seqtype);
		default_minv = default_maxv = 0;	/* keep compiler quiet */
	}

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
										 tbinfo->dobj.catId.oid);

		/*
		 * In older PG versions a sequence will have a pg_type entry, but v14
		 * and up don't use that, so don't attempt to preserve the type OID.
		 */
	}

	if (tbinfo->is_identity_sequence)
	{
		owning_tab = findTableByOid(tbinfo->owning_tab);

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

		/*
		 * Emit persistence option only if it's different from the owning
		 * table's.  This avoids using this new syntax unnecessarily.
		 */
		if (tbinfo->relpersistence != owning_tab->relpersistence)
			appendPQExpBuffer(query, "    %s\n",
							  tbinfo->relpersistence == RELPERSISTENCE_UNLOGGED ?
							  "UNLOGGED" : "LOGGED");
	}
	else
	{
		appendPQExpBuffer(query,
						  "CREATE %sSEQUENCE %s\n",
						  tbinfo->relpersistence == RELPERSISTENCE_UNLOGGED ?
						  "UNLOGGED " : "",
						  fmtQualifiedDumpable(tbinfo));

		if (seq->seqtype != SEQTYPE_BIGINT)
			appendPQExpBuffer(query, "    AS %s\n", SeqTypeNames[seq->seqtype]);
	}

	appendPQExpBuffer(query, "    START WITH " INT64_FORMAT "\n", seq->startv);

	appendPQExpBuffer(query, "    INCREMENT BY " INT64_FORMAT "\n", seq->incby);

	if (seq->minv != default_minv)
		appendPQExpBuffer(query, "    MINVALUE " INT64_FORMAT "\n", seq->minv);
	else
		appendPQExpBufferStr(query, "    NO MINVALUE\n");

	if (seq->maxv != default_maxv)
		appendPQExpBuffer(query, "    MAXVALUE " INT64_FORMAT "\n", seq->maxv);
	else
		appendPQExpBufferStr(query, "    NO MAXVALUE\n");

	appendPQExpBuffer(query,
					  "    CACHE " INT64_FORMAT "%s",
					  seq->cache, (seq->cycled ? "\n    CYCLE" : ""));

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
		owning_tab = findTableByOid(tbinfo->owning_tab);

		if (owning_tab == NULL)
			pg_fatal("failed sanity check, parent table with OID %u of sequence with OID %u not found",
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

	if (fout->remoteVersion < 100000)
		pg_free(seq);
	destroyPQExpBuffer(query);
	destroyPQExpBuffer(delqry);
	free(qseqname);
}

/*
 * dumpSequenceData
 *	  write the data of one user-defined sequence
 */
static void
dumpSequenceData(Archive *fout, const TableDataInfo *tdinfo)
{
	TableInfo  *tbinfo = tdinfo->tdtable;
	int64		last;
	bool		called;
	PQExpBuffer query = createPQExpBuffer();

	/*
	 * For versions >= 18, the sequence information is gathered in the sorted
	 * array before any calls to dumpSequenceData().  See collectSequences()
	 * for more information.
	 *
	 * For older versions, we have to query the sequence relations
	 * individually.
	 */
	if (fout->remoteVersion < 180000)
	{
		PGresult   *res;

		appendPQExpBuffer(query,
						  "SELECT last_value, is_called FROM %s",
						  fmtQualifiedDumpable(tbinfo));

		res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

		if (PQntuples(res) != 1)
			pg_fatal(ngettext("query to get data of sequence \"%s\" returned %d row (expected 1)",
							  "query to get data of sequence \"%s\" returned %d rows (expected 1)",
							  PQntuples(res)),
					 tbinfo->dobj.name, PQntuples(res));

		last = strtoi64(PQgetvalue(res, 0, 0), NULL, 10);
		called = (strcmp(PQgetvalue(res, 0, 1), "t") == 0);

		PQclear(res);
	}
	else
	{
		SequenceItem key = {0};
		SequenceItem *entry;

		Assert(sequences);
		Assert(tbinfo->dobj.catId.oid);

		key.oid = tbinfo->dobj.catId.oid;
		entry = bsearch(&key, sequences, nsequences,
						sizeof(SequenceItem), SequenceItemCmp);

		last = entry->last_value;
		called = entry->is_called;
	}

	resetPQExpBuffer(query);
	appendPQExpBufferStr(query, "SELECT pg_catalog.setval(");
	appendStringLiteralAH(query, fmtQualifiedDumpable(tbinfo), fout);
	appendPQExpBuffer(query, ", " INT64_FORMAT ", %s);\n",
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

	destroyPQExpBuffer(query);
}

/*
 * dumpTrigger
 *	  write the declaration of one user-defined table trigger
 */
static void
dumpTrigger(Archive *fout, const TriggerInfo *tginfo)
{
	DumpOptions *dopt = fout->dopt;
	TableInfo  *tbinfo = tginfo->tgtable;
	PQExpBuffer query;
	PQExpBuffer delqry;
	PQExpBuffer trigprefix;
	PQExpBuffer trigidentity;
	char	   *qtabname;
	char	   *tag;

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
		return;

	query = createPQExpBuffer();
	delqry = createPQExpBuffer();
	trigprefix = createPQExpBuffer();
	trigidentity = createPQExpBuffer();

	qtabname = pg_strdup(fmtId(tbinfo->dobj.name));

	appendPQExpBuffer(trigidentity, "%s ", fmtId(tginfo->dobj.name));
	appendPQExpBuffer(trigidentity, "ON %s", fmtQualifiedDumpable(tbinfo));

	appendPQExpBuffer(query, "%s;\n", tginfo->tgdef);
	appendPQExpBuffer(delqry, "DROP TRIGGER %s;\n", trigidentity->data);

	/* Triggers can depend on extensions */
	append_depends_on_extension(fout, query, &tginfo->dobj,
								"pg_catalog.pg_trigger", "TRIGGER",
								trigidentity->data);

	if (tginfo->tgispartition)
	{
		Assert(tbinfo->ispartition);

		/*
		 * Partition triggers only appear here because their 'tgenabled' flag
		 * differs from its parent's.  The trigger is created already, so
		 * remove the CREATE and replace it with an ALTER.  (Clear out the
		 * DROP query too, so that pg_dump --create does not cause errors.)
		 */
		resetPQExpBuffer(query);
		resetPQExpBuffer(delqry);
		appendPQExpBuffer(query, "\nALTER %sTABLE %s ",
						  tbinfo->relkind == RELKIND_FOREIGN_TABLE ? "FOREIGN " : "",
						  fmtQualifiedDumpable(tbinfo));
		switch (tginfo->tgenabled)
		{
			case 'f':
			case 'D':
				appendPQExpBufferStr(query, "DISABLE");
				break;
			case 't':
			case 'O':
				appendPQExpBufferStr(query, "ENABLE");
				break;
			case 'R':
				appendPQExpBufferStr(query, "ENABLE REPLICA");
				break;
			case 'A':
				appendPQExpBufferStr(query, "ENABLE ALWAYS");
				break;
		}
		appendPQExpBuffer(query, " TRIGGER %s;\n",
						  fmtId(tginfo->dobj.name));
	}
	else if (tginfo->tgenabled != 't' && tginfo->tgenabled != 'O')
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
dumpEventTrigger(Archive *fout, const EventTriggerInfo *evtinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer query;
	PQExpBuffer delqry;
	char	   *qevtname;

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
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
dumpRule(Archive *fout, const RuleInfo *rinfo)
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

	/* Do nothing if not dumping schema */
	if (!dopt->dumpSchema)
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
			pg_fatal("query to get rule \"%s\" for table \"%s\" failed: wrong number of rows returned",
					 rinfo->dobj.name, tbinfo->dobj.name);

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
				i;
	int			i_classid,
				i_objid,
				i_refobjid;
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

	/*
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

		recordExtensionMembership(objId, ext);
	}

	PQclear(res);

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
	 * Note that we create TableDataInfo objects even in schema-only mode, ie,
	 * user data in a configuration table is treated like schema data. This
	 * seems appropriate since system data in a config table would get
	 * reloaded by CREATE EXTENSION.  If the extension is not listed in the
	 * list of extensions to be included, none of its data is dumped.
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

		/*
		 * Check if this extension is listed as to include in the dump.  If
		 * not, any table data associated with it is discarded.
		 */
		if (extension_include_oids.head != NULL &&
			!simple_oid_list_member(&extension_include_oids,
									curext->dobj.catId.oid))
			continue;

		/*
		 * Check if this extension is listed as to exclude in the dump.  If
		 * yes, any table data associated with it is discarded.
		 */
		if (extension_exclude_oids.head != NULL &&
			simple_oid_list_member(&extension_exclude_oids,
								   curext->dobj.catId.oid))
			continue;

		if (strlen(extconfig) != 0 || strlen(extcondition) != 0)
		{
			int			j;

			if (!parsePGArray(extconfig, &extconfigarray, &nconfigitems))
				pg_fatal("could not parse %s array", "extconfig");
			if (!parsePGArray(extcondition, &extconditionarray, &nconditionitems))
				pg_fatal("could not parse %s array", "extcondition");
			if (nconfigitems != nconditionitems)
				pg_fatal("mismatched number of configurations and conditions for extension");

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
	 */
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
			case DO_TABLE_ATTACH:
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
				/* Pre-data objects: must come before the pre-data boundary */
				addObjectDependency(preDataBound, dobj->dumpId);
				break;
			case DO_TABLE_DATA:
			case DO_SEQUENCE_SET:
			case DO_LARGE_OBJECT:
			case DO_LARGE_OBJECT_DATA:
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
			case DO_PUBLICATION_TABLE_IN_SCHEMA:
			case DO_SUBSCRIPTION:
			case DO_SUBSCRIPTION_REL:
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
			case DO_REL_STATS:
				/* stats section varies by parent object type, DATA or POST */
				if (statisticsDumpSection((RelStatsInfo *) dobj) == SECTION_DATA)
				{
					addObjectDependency(dobj, preDataBound->dumpId);
					addObjectDependency(postDataBound, dobj->dumpId);
				}
				else
					addObjectDependency(dobj, postDataBound->dumpId);
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
findDumpableDependencies(ArchiveHandle *AH, const DumpableObject *dobj,
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
 * Note that the result is cached and must not be freed by the caller.
 */
static const char *
getFormattedTypeName(Archive *fout, Oid oid, OidOptions opts)
{
	TypeInfo   *typeInfo;
	char	   *result;
	PQExpBuffer query;
	PGresult   *res;

	if (oid == 0)
	{
		if ((opts & zeroAsStar) != 0)
			return "*";
		else if ((opts & zeroAsNone) != 0)
			return "NONE";
	}

	/* see if we have the result cached in the type's TypeInfo record */
	typeInfo = findTypeByOid(oid);
	if (typeInfo && typeInfo->ftypname)
		return typeInfo->ftypname;

	query = createPQExpBuffer();
	appendPQExpBuffer(query, "SELECT pg_catalog.format_type('%u'::pg_catalog.oid, NULL)",
					  oid);

	res = ExecuteSqlQueryForSingleRow(fout, query->data);

	/* result of format_type is already quoted */
	result = pg_strdup(PQgetvalue(res, 0, 0));

	PQclear(res);
	destroyPQExpBuffer(query);

	/*
	 * Cache the result for re-use in later requests, if possible.  If we
	 * don't have a TypeInfo for the type, the string will be leaked once the
	 * caller is done with it ... but that case really should not happen, so
	 * leaking if it does seems acceptable.
	 */
	if (typeInfo)
		typeInfo->ftypname = result;

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
		pg_log_warning("could not parse %s array", "reloptions");
}

/*
 * read_dump_filters - retrieve object identifier patterns from file
 *
 * Parse the specified filter file for include and exclude patterns, and add
 * them to the relevant lists.  If the filename is "-" then filters will be
 * read from STDIN rather than a file.
 */
static void
read_dump_filters(const char *filename, DumpOptions *dopt)
{
	FilterStateData fstate;
	char	   *objname;
	FilterCommandType comtype;
	FilterObjectType objtype;

	filter_init(&fstate, filename, exit_nicely);

	while (filter_read_item(&fstate, &objname, &comtype, &objtype))
	{
		if (comtype == FILTER_COMMAND_TYPE_INCLUDE)
		{
			switch (objtype)
			{
				case FILTER_OBJECT_TYPE_NONE:
					break;
				case FILTER_OBJECT_TYPE_DATABASE:
				case FILTER_OBJECT_TYPE_FUNCTION:
				case FILTER_OBJECT_TYPE_INDEX:
				case FILTER_OBJECT_TYPE_TABLE_DATA:
				case FILTER_OBJECT_TYPE_TABLE_DATA_AND_CHILDREN:
				case FILTER_OBJECT_TYPE_TRIGGER:
					pg_log_filter_error(&fstate, _("%s filter for \"%s\" is not allowed"),
										"include",
										filter_object_type_name(objtype));
					exit_nicely(1);
					break;		/* unreachable */

				case FILTER_OBJECT_TYPE_EXTENSION:
					simple_string_list_append(&extension_include_patterns, objname);
					break;
				case FILTER_OBJECT_TYPE_FOREIGN_DATA:
					simple_string_list_append(&foreign_servers_include_patterns, objname);
					break;
				case FILTER_OBJECT_TYPE_SCHEMA:
					simple_string_list_append(&schema_include_patterns, objname);
					dopt->include_everything = false;
					break;
				case FILTER_OBJECT_TYPE_TABLE:
					simple_string_list_append(&table_include_patterns, objname);
					dopt->include_everything = false;
					break;
				case FILTER_OBJECT_TYPE_TABLE_AND_CHILDREN:
					simple_string_list_append(&table_include_patterns_and_children,
											  objname);
					dopt->include_everything = false;
					break;
			}
		}
		else if (comtype == FILTER_COMMAND_TYPE_EXCLUDE)
		{
			switch (objtype)
			{
				case FILTER_OBJECT_TYPE_NONE:
					break;
				case FILTER_OBJECT_TYPE_DATABASE:
				case FILTER_OBJECT_TYPE_FUNCTION:
				case FILTER_OBJECT_TYPE_INDEX:
				case FILTER_OBJECT_TYPE_TRIGGER:
				case FILTER_OBJECT_TYPE_FOREIGN_DATA:
					pg_log_filter_error(&fstate, _("%s filter for \"%s\" is not allowed"),
										"exclude",
										filter_object_type_name(objtype));
					exit_nicely(1);
					break;

				case FILTER_OBJECT_TYPE_EXTENSION:
					simple_string_list_append(&extension_exclude_patterns, objname);
					break;
				case FILTER_OBJECT_TYPE_TABLE_DATA:
					simple_string_list_append(&tabledata_exclude_patterns,
											  objname);
					break;
				case FILTER_OBJECT_TYPE_TABLE_DATA_AND_CHILDREN:
					simple_string_list_append(&tabledata_exclude_patterns_and_children,
											  objname);
					break;
				case FILTER_OBJECT_TYPE_SCHEMA:
					simple_string_list_append(&schema_exclude_patterns, objname);
					break;
				case FILTER_OBJECT_TYPE_TABLE:
					simple_string_list_append(&table_exclude_patterns, objname);
					break;
				case FILTER_OBJECT_TYPE_TABLE_AND_CHILDREN:
					simple_string_list_append(&table_exclude_patterns_and_children,
											  objname);
					break;
			}
		}
		else
		{
			Assert(comtype == FILTER_COMMAND_TYPE_NONE);
			Assert(objtype == FILTER_OBJECT_TYPE_NONE);
		}

		if (objname)
			free(objname);
	}

	filter_free(&fstate);
}
