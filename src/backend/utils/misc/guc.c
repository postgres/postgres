/*--------------------------------------------------------------------
 * guc.c
 *
 * Support for grand unified configuration scheme, including SET
 * command, configuration file, and command line options.
 * See src/backend/utils/misc/README for more information.
 *
 *
 * Copyright (c) 2000-2009, PostgreSQL Global Development Group
 * Written by Peter Eisentraut <peter_e@gmx.net>.
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/misc/guc.c,v 1.499 2009/04/02 19:57:19 momjian Exp $
 *
 *--------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <float.h>
#include <math.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef HAVE_SYSLOG
#include <syslog.h>
#endif

#include "access/gin.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "commands/async.h"
#include "commands/prepare.h"
#include "commands/vacuum.h"
#include "commands/variable.h"
#include "commands/trigger.h"
#include "funcapi.h"
#include "libpq/auth.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "optimizer/cost.h"
#include "optimizer/geqo.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "parser/gramparse.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "parser/scansup.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "postmaster/bgwriter.h"
#include "postmaster/postmaster.h"
#include "postmaster/syslogger.h"
#include "postmaster/walwriter.h"
#include "regex/regex.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "tcop/tcopprot.h"
#include "tsearch/ts_cache.h"
#include "utils/builtins.h"
#include "utils/guc_tables.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/plancache.h"
#include "utils/portal.h"
#include "utils/ps_status.h"
#include "utils/tzparser.h"
#include "utils/xml.h"

#ifndef PG_KRB_SRVTAB
#define PG_KRB_SRVTAB ""
#endif
#ifndef PG_KRB_SRVNAM
#define PG_KRB_SRVNAM ""
#endif

#define CONFIG_FILENAME "postgresql.conf"
#define HBA_FILENAME	"pg_hba.conf"
#define IDENT_FILENAME	"pg_ident.conf"

#ifdef EXEC_BACKEND
#define CONFIG_EXEC_PARAMS "global/config_exec_params"
#define CONFIG_EXEC_PARAMS_NEW "global/config_exec_params.new"
#endif

/* upper limit for GUC variables measured in kilobytes of memory */
#if SIZEOF_SIZE_T > 4
#define MAX_KILOBYTES	INT_MAX
#else
#define MAX_KILOBYTES	(INT_MAX / 1024)
#endif

#define KB_PER_MB (1024)
#define KB_PER_GB (1024*1024)

#define MS_PER_S 1000
#define S_PER_MIN 60
#define MS_PER_MIN (1000 * 60)
#define MIN_PER_H 60
#define S_PER_H (60 * 60)
#define MS_PER_H (1000 * 60 * 60)
#define MIN_PER_D (60 * 24)
#define S_PER_D (60 * 60 * 24)
#define MS_PER_D (1000 * 60 * 60 * 24)

/* XXX these should appear in other modules' header files */
extern bool Log_disconnections;
extern int	CommitDelay;
extern int	CommitSiblings;
extern char *default_tablespace;
extern char *temp_tablespaces;
extern bool synchronize_seqscans;
extern bool fullPageWrites;

#ifdef TRACE_SORT
extern bool trace_sort;
#endif
#ifdef TRACE_SYNCSCAN
extern bool trace_syncscan;
#endif
#ifdef DEBUG_BOUNDED_SORT
extern bool optimize_bounded_sort;
#endif

#ifdef USE_SSL
extern char *SSLCipherSuites;
#endif

static void set_config_sourcefile(const char *name, char *sourcefile,
					  int sourceline);

static const char *assign_log_destination(const char *value,
					   bool doit, GucSource source);

#ifdef HAVE_SYSLOG
static int	syslog_facility = LOG_LOCAL0;

static bool assign_syslog_facility(int newval,
				    bool doit, GucSource source);
static const char *assign_syslog_ident(const char *ident,
					bool doit, GucSource source);
#endif

static bool assign_session_replication_role(int newval, bool doit,
								GucSource source);
static const char *show_num_temp_buffers(void);
static bool assign_phony_autocommit(bool newval, bool doit, GucSource source);
static const char *assign_custom_variable_classes(const char *newval, bool doit,
							   GucSource source);
static bool assign_debug_assertions(bool newval, bool doit, GucSource source);
static bool assign_ssl(bool newval, bool doit, GucSource source);
static bool assign_stage_log_stats(bool newval, bool doit, GucSource source);
static bool assign_log_stats(bool newval, bool doit, GucSource source);
static bool assign_transaction_read_only(bool newval, bool doit, GucSource source);
static const char *assign_canonical_path(const char *newval, bool doit, GucSource source);
static const char *assign_timezone_abbreviations(const char *newval, bool doit, GucSource source);
static const char *show_archive_command(void);
static bool assign_tcp_keepalives_idle(int newval, bool doit, GucSource source);
static bool assign_tcp_keepalives_interval(int newval, bool doit, GucSource source);
static bool assign_tcp_keepalives_count(int newval, bool doit, GucSource source);
static const char *show_tcp_keepalives_idle(void);
static const char *show_tcp_keepalives_interval(void);
static const char *show_tcp_keepalives_count(void);
static bool assign_maxconnections(int newval, bool doit, GucSource source);
static bool assign_autovacuum_max_workers(int newval, bool doit, GucSource source);
static bool assign_effective_io_concurrency(int newval, bool doit, GucSource source);
static const char *assign_pgstat_temp_directory(const char *newval, bool doit, GucSource source);

static char *config_enum_get_options(struct config_enum *record, 
									 const char *prefix, const char *suffix,
									 const char *separator);


/*
 * Options for enum values defined in this module.
 *
 * NOTE! Option values may not contain double quotes!
 */

/*
 * We have different sets for client and server message level options because
 * they sort slightly different (see "log" level)
 */
static const struct config_enum_entry client_message_level_options[] = {
	{"debug", DEBUG2, true},
	{"debug5", DEBUG5, false},
	{"debug4", DEBUG4, false},
	{"debug3", DEBUG3, false},
	{"debug2", DEBUG2, false},
	{"debug1", DEBUG1, false},
	{"log", LOG, false},
	{"info", INFO, true},
	{"notice", NOTICE, false},
	{"warning", WARNING, false},
	{"error", ERROR, false},
	{"fatal", FATAL, true},
	{"panic", PANIC, true},
	{NULL, 0, false}
};

static const struct config_enum_entry server_message_level_options[] = {
	{"debug", DEBUG2, true},
	{"debug5", DEBUG5, false},
	{"debug4", DEBUG4, false},
	{"debug3", DEBUG3, false},
	{"debug2", DEBUG2, false},
	{"debug1", DEBUG1, false},
	{"info", INFO, false},
	{"notice", NOTICE, false},
	{"warning", WARNING, false},
	{"error", ERROR, false},
	{"log", LOG, false},
	{"fatal", FATAL, false},
	{"panic", PANIC, false},
	{NULL, 0, false}
};

static const struct config_enum_entry intervalstyle_options[] = {
	{"postgres", INTSTYLE_POSTGRES, false},
	{"postgres_verbose", INTSTYLE_POSTGRES_VERBOSE, false},
	{"sql_standard", INTSTYLE_SQL_STANDARD, false},
	{"iso_8601", INTSTYLE_ISO_8601, false},
	{NULL, 0, false}
};

static const struct config_enum_entry log_error_verbosity_options[] = {
	{"terse", PGERROR_TERSE, false},
	{"default", PGERROR_DEFAULT, false},
	{"verbose", PGERROR_VERBOSE, false},
	{NULL, 0, false}
};

static const struct config_enum_entry log_statement_options[] = {
	{"none", LOGSTMT_NONE, false},
	{"ddl", LOGSTMT_DDL, false},
	{"mod", LOGSTMT_MOD, false},
	{"all", LOGSTMT_ALL, false},
	{NULL, 0, false}
};

static const struct config_enum_entry regex_flavor_options[] = {
    {"advanced", REG_ADVANCED, false},
    {"extended", REG_EXTENDED, false},
    {"basic", REG_BASIC, false},
    {NULL, 0, false}
};

static const struct config_enum_entry isolation_level_options[] = {
	{"serializable", XACT_SERIALIZABLE, false},
	{"repeatable read", XACT_REPEATABLE_READ, false},
	{"read committed", XACT_READ_COMMITTED, false},
	{"read uncommitted", XACT_READ_UNCOMMITTED, false},
	{NULL, 0}
};

static const struct config_enum_entry session_replication_role_options[] = {
	{"origin", SESSION_REPLICATION_ROLE_ORIGIN, false},
	{"replica", SESSION_REPLICATION_ROLE_REPLICA, false},
	{"local", SESSION_REPLICATION_ROLE_LOCAL, false},
	{NULL, 0, false}
};

#ifdef HAVE_SYSLOG
static const struct config_enum_entry syslog_facility_options[] = {
	{"local0", LOG_LOCAL0, false},
	{"local1", LOG_LOCAL1, false},
	{"local2", LOG_LOCAL2, false},
	{"local3", LOG_LOCAL3, false},
	{"local4", LOG_LOCAL4, false},
	{"local5", LOG_LOCAL5, false},
	{"local6", LOG_LOCAL6, false},
	{"local7", LOG_LOCAL7, false},
	{NULL, 0}
};
#endif

static const struct config_enum_entry track_function_options[] = {
	{"none", TRACK_FUNC_OFF, false},
	{"pl", TRACK_FUNC_PL, false},
	{"all", TRACK_FUNC_ALL, false},
	{NULL, 0, false}
};

static const struct config_enum_entry xmlbinary_options[] = {
	{"base64", XMLBINARY_BASE64, false},
	{"hex", XMLBINARY_HEX, false},
	{NULL, 0, false}
};

static const struct config_enum_entry xmloption_options[] = {
	{"content", XMLOPTION_CONTENT, false},
	{"document", XMLOPTION_DOCUMENT, false},
	{NULL, 0, false}
};

/*
 * Although only "on", "off", and "safe_encoding" are documented, we
 * accept all the likely variants of "on" and "off".
 */
static const struct config_enum_entry backslash_quote_options[] = {
	{"safe_encoding", BACKSLASH_QUOTE_SAFE_ENCODING, false},
	{"on", BACKSLASH_QUOTE_ON, false},
	{"off", BACKSLASH_QUOTE_OFF, false},
	{"true", BACKSLASH_QUOTE_ON, true},
	{"false", BACKSLASH_QUOTE_OFF, true},
	{"yes", BACKSLASH_QUOTE_ON, true},
	{"no", BACKSLASH_QUOTE_OFF, true},
	{"1", BACKSLASH_QUOTE_ON, true},
	{"0", BACKSLASH_QUOTE_OFF, true},
	{NULL, 0, false}
};

/*
 * Although only "on", "off", and "partition" are documented, we
 * accept all the likely variants of "on" and "off".
 */
static const struct config_enum_entry constraint_exclusion_options[] = {
	{"partition", CONSTRAINT_EXCLUSION_PARTITION, false},
	{"on", CONSTRAINT_EXCLUSION_ON, false},
	{"off", CONSTRAINT_EXCLUSION_OFF, false},
	{"true", CONSTRAINT_EXCLUSION_ON, true},
	{"false", CONSTRAINT_EXCLUSION_OFF, true},
	{"yes", CONSTRAINT_EXCLUSION_ON, true},
	{"no", CONSTRAINT_EXCLUSION_OFF, true},
	{"1", CONSTRAINT_EXCLUSION_ON, true},
	{"0", CONSTRAINT_EXCLUSION_OFF, true},
	{NULL, 0, false}
};

/*
 * Options for enum values stored in other modules
 */
extern const struct config_enum_entry sync_method_options[];

/*
 * GUC option variables that are exported from this module
 */
#ifdef USE_ASSERT_CHECKING
bool		assert_enabled = true;
#else
bool		assert_enabled = false;
#endif
bool		log_duration = false;
bool		Debug_print_plan = false;
bool		Debug_print_parse = false;
bool		Debug_print_rewritten = false;
bool		Debug_pretty_print = true;

bool		log_parser_stats = false;
bool		log_planner_stats = false;
bool		log_executor_stats = false;
bool		log_statement_stats = false;		/* this is sort of all three
												 * above together */
bool		log_btree_build_stats = false;

bool		check_function_bodies = true;
bool		default_with_oids = false;
bool		SQL_inheritance = true;

bool		Password_encryption = true;

int			log_min_error_statement = ERROR;
int			log_min_messages = WARNING;
int			client_min_messages = NOTICE;
int			log_min_duration_statement = -1;
int			log_temp_files = -1;

int			num_temp_buffers = 1000;

char	   *ConfigFileName;
char	   *HbaFileName;
char	   *IdentFileName;
char	   *external_pid_file;

char	   *pgstat_temp_directory;

int			tcp_keepalives_idle;
int			tcp_keepalives_interval;
int			tcp_keepalives_count;

/*
 * These variables are all dummies that don't do anything, except in some
 * cases provide the value for SHOW to display.  The real state is elsewhere
 * and is kept in sync by assign_hooks.
 */
static char *log_destination_string;

#ifdef HAVE_SYSLOG
static char *syslog_ident_str;
#endif
static bool phony_autocommit;
static bool session_auth_is_superuser;
static double phony_random_seed;
static char *client_encoding_string;
static char *datestyle_string;
static char *locale_collate;
static char *locale_ctype;
static char *server_encoding_string;
static char *server_version_string;
static int	server_version_num;
static char *timezone_string;
static char *log_timezone_string;
static char *timezone_abbreviations_string;
static char *XactIsoLevel_string;
static char *data_directory;
static char *custom_variable_classes;
static int	max_function_args;
static int	max_index_keys;
static int	max_identifier_length;
static int	block_size;
static int	segment_size;
static int	wal_block_size;
static int	wal_segment_size;
static bool integer_datetimes;
static int	effective_io_concurrency;

/* should be static, but commands/variable.c needs to get at these */
char	   *role_string;
char	   *session_authorization_string;


/*
 * Displayable names for context types (enum GucContext)
 *
 * Note: these strings are deliberately not localized.
 */
const char *const GucContext_Names[] =
{
	 /* PGC_INTERNAL */ "internal",
	 /* PGC_POSTMASTER */ "postmaster",
	 /* PGC_SIGHUP */ "sighup",
	 /* PGC_BACKEND */ "backend",
	 /* PGC_SUSET */ "superuser",
	 /* PGC_USERSET */ "user"
};

/*
 * Displayable names for source types (enum GucSource)
 *
 * Note: these strings are deliberately not localized.
 */
const char *const GucSource_Names[] =
{
	 /* PGC_S_DEFAULT */ "default",
	 /* PGC_S_ENV_VAR */ "environment variable",
	 /* PGC_S_FILE */ "configuration file",
	 /* PGC_S_ARGV */ "command line",
	 /* PGC_S_DATABASE */ "database",
	 /* PGC_S_USER */ "user",
	 /* PGC_S_CLIENT */ "client",
	 /* PGC_S_OVERRIDE */ "override",
	 /* PGC_S_INTERACTIVE */ "interactive",
	 /* PGC_S_TEST */ "test",
	 /* PGC_S_SESSION */ "session"
};

/*
 * Displayable names for the groupings defined in enum config_group
 */
const char *const config_group_names[] =
{
	/* UNGROUPED */
	gettext_noop("Ungrouped"),
	/* FILE_LOCATIONS */
	gettext_noop("File Locations"),
	/* CONN_AUTH */
	gettext_noop("Connections and Authentication"),
	/* CONN_AUTH_SETTINGS */
	gettext_noop("Connections and Authentication / Connection Settings"),
	/* CONN_AUTH_SECURITY */
	gettext_noop("Connections and Authentication / Security and Authentication"),
	/* RESOURCES */
	gettext_noop("Resource Usage"),
	/* RESOURCES_MEM */
	gettext_noop("Resource Usage / Memory"),
	/* RESOURCES_KERNEL */
	gettext_noop("Resource Usage / Kernel Resources"),
	/* WAL */
	gettext_noop("Write-Ahead Log"),
	/* WAL_SETTINGS */
	gettext_noop("Write-Ahead Log / Settings"),
	/* WAL_CHECKPOINTS */
	gettext_noop("Write-Ahead Log / Checkpoints"),
	/* QUERY_TUNING */
	gettext_noop("Query Tuning"),
	/* QUERY_TUNING_METHOD */
	gettext_noop("Query Tuning / Planner Method Configuration"),
	/* QUERY_TUNING_COST */
	gettext_noop("Query Tuning / Planner Cost Constants"),
	/* QUERY_TUNING_GEQO */
	gettext_noop("Query Tuning / Genetic Query Optimizer"),
	/* QUERY_TUNING_OTHER */
	gettext_noop("Query Tuning / Other Planner Options"),
	/* LOGGING */
	gettext_noop("Reporting and Logging"),
	/* LOGGING_WHERE */
	gettext_noop("Reporting and Logging / Where to Log"),
	/* LOGGING_WHEN */
	gettext_noop("Reporting and Logging / When to Log"),
	/* LOGGING_WHAT */
	gettext_noop("Reporting and Logging / What to Log"),
	/* STATS */
	gettext_noop("Statistics"),
	/* STATS_MONITORING */
	gettext_noop("Statistics / Monitoring"),
	/* STATS_COLLECTOR */
	gettext_noop("Statistics / Query and Index Statistics Collector"),
	/* AUTOVACUUM */
	gettext_noop("Autovacuum"),
	/* CLIENT_CONN */
	gettext_noop("Client Connection Defaults"),
	/* CLIENT_CONN_STATEMENT */
	gettext_noop("Client Connection Defaults / Statement Behavior"),
	/* CLIENT_CONN_LOCALE */
	gettext_noop("Client Connection Defaults / Locale and Formatting"),
	/* CLIENT_CONN_OTHER */
	gettext_noop("Client Connection Defaults / Other Defaults"),
	/* LOCK_MANAGEMENT */
	gettext_noop("Lock Management"),
	/* COMPAT_OPTIONS */
	gettext_noop("Version and Platform Compatibility"),
	/* COMPAT_OPTIONS_PREVIOUS */
	gettext_noop("Version and Platform Compatibility / Previous PostgreSQL Versions"),
	/* COMPAT_OPTIONS_CLIENT */
	gettext_noop("Version and Platform Compatibility / Other Platforms and Clients"),
	/* PRESET_OPTIONS */
	gettext_noop("Preset Options"),
	/* CUSTOM_OPTIONS */
	gettext_noop("Customized Options"),
	/* DEVELOPER_OPTIONS */
	gettext_noop("Developer Options"),
	/* help_config wants this array to be null-terminated */
	NULL
};

/*
 * Displayable names for GUC variable types (enum config_type)
 *
 * Note: these strings are deliberately not localized.
 */
const char *const config_type_names[] =
{
	 /* PGC_BOOL */ "bool",
	 /* PGC_INT */ "integer",
	 /* PGC_REAL */ "real",
	 /* PGC_STRING */ "string",
	 /* PGC_ENUM */ "enum"
};


/*
 * Contents of GUC tables
 *
 * See src/backend/utils/misc/README for design notes.
 *
 * TO ADD AN OPTION:
 *
 * 1. Declare a global variable of type bool, int, double, or char*
 *	  and make use of it.
 *
 * 2. Decide at what times it's safe to set the option. See guc.h for
 *	  details.
 *
 * 3. Decide on a name, a default value, upper and lower bounds (if
 *	  applicable), etc.
 *
 * 4. Add a record below.
 *
 * 5. Add it to src/backend/utils/misc/postgresql.conf.sample, if
 *	  appropriate.
 *
 * 6. Don't forget to document the option (at least in config.sgml).
 *
 * 7. If it's a new GUC_LIST option you must edit pg_dumpall.c to ensure
 *	  it is not single quoted at dump time.
 */


/******** option records follow ********/

static struct config_bool ConfigureNamesBool[] =
{
	{
		{"enable_seqscan", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of sequential-scan plans."),
			NULL
		},
		&enable_seqscan,
		true, NULL, NULL
	},
	{
		{"enable_indexscan", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of index-scan plans."),
			NULL
		},
		&enable_indexscan,
		true, NULL, NULL
	},
	{
		{"enable_bitmapscan", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of bitmap-scan plans."),
			NULL
		},
		&enable_bitmapscan,
		true, NULL, NULL
	},
	{
		{"enable_tidscan", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of TID scan plans."),
			NULL
		},
		&enable_tidscan,
		true, NULL, NULL
	},
	{
		{"enable_sort", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of explicit sort steps."),
			NULL
		},
		&enable_sort,
		true, NULL, NULL
	},
	{
		{"enable_hashagg", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of hashed aggregation plans."),
			NULL
		},
		&enable_hashagg,
		true, NULL, NULL
	},
	{
		{"enable_nestloop", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of nested-loop join plans."),
			NULL
		},
		&enable_nestloop,
		true, NULL, NULL
	},
	{
		{"enable_mergejoin", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of merge join plans."),
			NULL
		},
		&enable_mergejoin,
		true, NULL, NULL
	},
	{
		{"enable_hashjoin", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of hash join plans."),
			NULL
		},
		&enable_hashjoin,
		true, NULL, NULL
	},
	{
		{"geqo", PGC_USERSET, QUERY_TUNING_GEQO,
			gettext_noop("Enables genetic query optimization."),
			gettext_noop("This algorithm attempts to do planning without "
						 "exhaustive searching.")
		},
		&enable_geqo,
		true, NULL, NULL
	},
	{
		/* Not for general use --- used by SET SESSION AUTHORIZATION */
		{"is_superuser", PGC_INTERNAL, UNGROUPED,
			gettext_noop("Shows whether the current user is a superuser."),
			NULL,
			GUC_REPORT | GUC_NO_SHOW_ALL | GUC_NO_RESET_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&session_auth_is_superuser,
		false, NULL, NULL
	},
	{
		{"ssl", PGC_POSTMASTER, CONN_AUTH_SECURITY,
			gettext_noop("Enables SSL connections."),
			NULL
		},
		&EnableSSL,
		false, assign_ssl, NULL
	},
	{
		{"fsync", PGC_SIGHUP, WAL_SETTINGS,
			gettext_noop("Forces synchronization of updates to disk."),
			gettext_noop("The server will use the fsync() system call in several places to make "
			"sure that updates are physically written to disk. This insures "
						 "that a database cluster will recover to a consistent state after "
						 "an operating system or hardware crash.")
		},
		&enableFsync,
		true, NULL, NULL
	},
	{
		{"synchronous_commit", PGC_USERSET, WAL_SETTINGS,
			gettext_noop("Sets immediate fsync at commit."),
			NULL
		},
		&XactSyncCommit,
		true, NULL, NULL
	},
	{
		{"zero_damaged_pages", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("Continues processing past damaged page headers."),
			gettext_noop("Detection of a damaged page header normally causes PostgreSQL to "
				"report an error, aborting the current transaction. Setting "
						 "zero_damaged_pages to true causes the system to instead report a "
						 "warning, zero out the damaged page, and continue processing. This "
						 "behavior will destroy data, namely all the rows on the damaged page."),
			GUC_NOT_IN_SAMPLE
		},
		&zero_damaged_pages,
		false, NULL, NULL
	},
	{
		{"full_page_writes", PGC_SIGHUP, WAL_SETTINGS,
			gettext_noop("Writes full pages to WAL when first modified after a checkpoint."),
			gettext_noop("A page write in process during an operating system crash might be "
						 "only partially written to disk.  During recovery, the row changes "
			  "stored in WAL are not enough to recover.  This option writes "
						 "pages when first modified after a checkpoint to WAL so full recovery "
						 "is possible.")
		},
		&fullPageWrites,
		true, NULL, NULL
	},
	{
		{"silent_mode", PGC_POSTMASTER, LOGGING_WHEN,
			gettext_noop("Runs the server silently."),
			gettext_noop("If this parameter is set, the server will automatically run in the "
				 "background and any controlling terminals are dissociated.")
		},
		&SilentMode,
		false, NULL, NULL
	},
	{
		{"log_checkpoints", PGC_SIGHUP, LOGGING_WHAT,
			gettext_noop("Logs each checkpoint."),
			NULL
		},
		&log_checkpoints,
		false, NULL, NULL
	},
	{
		{"log_connections", PGC_BACKEND, LOGGING_WHAT,
			gettext_noop("Logs each successful connection."),
			NULL
		},
		&Log_connections,
		false, NULL, NULL
	},
	{
		{"log_disconnections", PGC_BACKEND, LOGGING_WHAT,
			gettext_noop("Logs end of a session, including duration."),
			NULL
		},
		&Log_disconnections,
		false, NULL, NULL
	},
	{
		{"debug_assertions", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Turns on various assertion checks."),
			gettext_noop("This is a debugging aid."),
			GUC_NOT_IN_SAMPLE
		},
		&assert_enabled,
#ifdef USE_ASSERT_CHECKING
		true,
#else
		false,
#endif
		assign_debug_assertions, NULL
	},
	{
		/* currently undocumented, so don't show in SHOW ALL */
		{"exit_on_error", PGC_USERSET, UNGROUPED,
			gettext_noop("No description available."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE
		},
		&ExitOnAnyError,
		false, NULL, NULL
	},
	{
		{"log_duration", PGC_SUSET, LOGGING_WHAT,
			gettext_noop("Logs the duration of each completed SQL statement."),
			NULL
		},
		&log_duration,
		false, NULL, NULL
	},
	{
		{"debug_print_parse", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Logs each query's parse tree."),
			NULL
		},
		&Debug_print_parse,
		false, NULL, NULL
	},
	{
		{"debug_print_rewritten", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Logs each query's rewritten parse tree."),
			NULL
		},
		&Debug_print_rewritten,
		false, NULL, NULL
	},
	{
		{"debug_print_plan", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Logs each query's execution plan."),
			NULL
		},
		&Debug_print_plan,
		false, NULL, NULL
	},
	{
		{"debug_pretty_print", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Indents parse and plan tree displays."),
			NULL
		},
		&Debug_pretty_print,
		true, NULL, NULL
	},
	{
		{"log_parser_stats", PGC_SUSET, STATS_MONITORING,
			gettext_noop("Writes parser performance statistics to the server log."),
			NULL
		},
		&log_parser_stats,
		false, assign_stage_log_stats, NULL
	},
	{
		{"log_planner_stats", PGC_SUSET, STATS_MONITORING,
			gettext_noop("Writes planner performance statistics to the server log."),
			NULL
		},
		&log_planner_stats,
		false, assign_stage_log_stats, NULL
	},
	{
		{"log_executor_stats", PGC_SUSET, STATS_MONITORING,
			gettext_noop("Writes executor performance statistics to the server log."),
			NULL
		},
		&log_executor_stats,
		false, assign_stage_log_stats, NULL
	},
	{
		{"log_statement_stats", PGC_SUSET, STATS_MONITORING,
			gettext_noop("Writes cumulative performance statistics to the server log."),
			NULL
		},
		&log_statement_stats,
		false, assign_log_stats, NULL
	},
#ifdef BTREE_BUILD_STATS
	{
		{"log_btree_build_stats", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("No description available."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&log_btree_build_stats,
		false, NULL, NULL
	},
#endif

	{
		{"track_activities", PGC_SUSET, STATS_COLLECTOR,
			gettext_noop("Collects information about executing commands."),
			gettext_noop("Enables the collection of information on the currently "
						 "executing command of each session, along with "
						 "the time at which that command began execution.")
		},
		&pgstat_track_activities,
		true, NULL, NULL
	},
	{
		{"track_counts", PGC_SUSET, STATS_COLLECTOR,
			gettext_noop("Collects statistics on database activity."),
			NULL
		},
		&pgstat_track_counts,
		true, NULL, NULL
	},

	{
		{"update_process_title", PGC_SUSET, STATS_COLLECTOR,
			gettext_noop("Updates the process title to show the active SQL command."),
			gettext_noop("Enables updating of the process title every time a new SQL command is received by the server.")
		},
		&update_process_title,
		true, NULL, NULL
	},

	{
		{"autovacuum", PGC_SIGHUP, AUTOVACUUM,
			gettext_noop("Starts the autovacuum subprocess."),
			NULL
		},
		&autovacuum_start_daemon,
		true, NULL, NULL
	},

	{
		{"trace_notify", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Generates debugging output for LISTEN and NOTIFY."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&Trace_notify,
		false, NULL, NULL
	},

#ifdef LOCK_DEBUG
	{
		{"trace_locks", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("No description available."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&Trace_locks,
		false, NULL, NULL
	},
	{
		{"trace_userlocks", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("No description available."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&Trace_userlocks,
		false, NULL, NULL
	},
	{
		{"trace_lwlocks", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("No description available."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&Trace_lwlocks,
		false, NULL, NULL
	},
	{
		{"debug_deadlocks", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("No description available."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&Debug_deadlocks,
		false, NULL, NULL
	},
#endif

	{
		{"log_lock_waits", PGC_SUSET, LOGGING_WHAT,
			gettext_noop("Logs long lock waits."),
			NULL
		},
		&log_lock_waits,
		false, NULL, NULL
	},

	{
		{"log_hostname", PGC_SIGHUP, LOGGING_WHAT,
			gettext_noop("Logs the host name in the connection logs."),
			gettext_noop("By default, connection logs only show the IP address "
						 "of the connecting host. If you want them to show the host name you "
			  "can turn this on, but depending on your host name resolution "
			   "setup it might impose a non-negligible performance penalty.")
		},
		&log_hostname,
		false, NULL, NULL
	},
	{
		{"sql_inheritance", PGC_USERSET, COMPAT_OPTIONS_PREVIOUS,
			gettext_noop("Causes subtables to be included by default in various commands."),
			NULL
		},
		&SQL_inheritance,
		true, NULL, NULL
	},
	{
		{"password_encryption", PGC_USERSET, CONN_AUTH_SECURITY,
			gettext_noop("Encrypt passwords."),
			gettext_noop("When a password is specified in CREATE USER or "
			   "ALTER USER without writing either ENCRYPTED or UNENCRYPTED, "
						 "this parameter determines whether the password is to be encrypted.")
		},
		&Password_encryption,
		true, NULL, NULL
	},
	{
		{"transform_null_equals", PGC_USERSET, COMPAT_OPTIONS_CLIENT,
			gettext_noop("Treats \"expr=NULL\" as \"expr IS NULL\"."),
			gettext_noop("When turned on, expressions of the form expr = NULL "
			   "(or NULL = expr) are treated as expr IS NULL, that is, they "
				"return true if expr evaluates to the null value, and false "
			   "otherwise. The correct behavior of expr = NULL is to always "
						 "return null (unknown).")
		},
		&Transform_null_equals,
		false, NULL, NULL
	},
	{
		{"db_user_namespace", PGC_SIGHUP, CONN_AUTH_SECURITY,
			gettext_noop("Enables per-database user names."),
			NULL
		},
		&Db_user_namespace,
		false, NULL, NULL
	},
	{
		/* only here for backwards compatibility */
		{"autocommit", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("This parameter doesn't do anything."),
			gettext_noop("It's just here so that we won't choke on SET AUTOCOMMIT TO ON from 7.3-vintage clients."),
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE
		},
		&phony_autocommit,
		true, assign_phony_autocommit, NULL
	},
	{
		{"default_transaction_read_only", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets the default read-only status of new transactions."),
			NULL
		},
		&DefaultXactReadOnly,
		false, NULL, NULL
	},
	{
		{"transaction_read_only", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets the current transaction's read-only status."),
			NULL,
			GUC_NO_RESET_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&XactReadOnly,
		false, assign_transaction_read_only, NULL
	},
	{
		{"add_missing_from", PGC_USERSET, COMPAT_OPTIONS_PREVIOUS,
			gettext_noop("Automatically adds missing table references to FROM clauses."),
			NULL
		},
		&add_missing_from,
		false, NULL, NULL
	},
	{
		{"check_function_bodies", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Check function bodies during CREATE FUNCTION."),
			NULL
		},
		&check_function_bodies,
		true, NULL, NULL
	},
	{
		{"array_nulls", PGC_USERSET, COMPAT_OPTIONS_PREVIOUS,
			gettext_noop("Enable input of NULL elements in arrays."),
			gettext_noop("When turned on, unquoted NULL in an array input "
						 "value means a null value; "
						 "otherwise it is taken literally.")
		},
		&Array_nulls,
		true, NULL, NULL
	},
	{
		{"default_with_oids", PGC_USERSET, COMPAT_OPTIONS_PREVIOUS,
			gettext_noop("Create new tables with OIDs by default."),
			NULL
		},
		&default_with_oids,
		false, NULL, NULL
	},
	{
		{"logging_collector", PGC_POSTMASTER, LOGGING_WHERE,
			gettext_noop("Start a subprocess to capture stderr output and/or csvlogs into log files."),
			NULL
		},
		&Logging_collector,
		false, NULL, NULL
	},
	{
		{"log_truncate_on_rotation", PGC_SIGHUP, LOGGING_WHERE,
			gettext_noop("Truncate existing log files of same name during log rotation."),
			NULL
		},
		&Log_truncate_on_rotation,
		false, NULL, NULL
	},

#ifdef TRACE_SORT
	{
		{"trace_sort", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Emit information about resource usage in sorting."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&trace_sort,
		false, NULL, NULL
	},
#endif

#ifdef TRACE_SYNCSCAN
	/* this is undocumented because not exposed in a standard build */
	{
		{"trace_syncscan", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Generate debugging output for synchronized scanning."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&trace_syncscan,
		false, NULL, NULL
	},
#endif

#ifdef DEBUG_BOUNDED_SORT
	/* this is undocumented because not exposed in a standard build */
	{
		{
			"optimize_bounded_sort", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enable bounded sorting using heap sort."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&optimize_bounded_sort,
		true, NULL, NULL
	},
#endif

#ifdef WAL_DEBUG
	{
		{"wal_debug", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("Emit WAL-related debugging output."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&XLOG_DEBUG,
		false, NULL, NULL
	},
#endif

	{
		{"integer_datetimes", PGC_INTERNAL, PRESET_OPTIONS,
			gettext_noop("Datetimes are integer based."),
			NULL,
			GUC_REPORT | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&integer_datetimes,
#ifdef HAVE_INT64_TIMESTAMP
		true, NULL, NULL
#else
		false, NULL, NULL
#endif
	},

	{
		{"krb_caseins_users", PGC_SIGHUP, CONN_AUTH_SECURITY,
			gettext_noop("Sets whether Kerberos and GSSAPI user names should be treated as case-insensitive."),
			NULL
		},
		&pg_krb_caseins_users,
		false, NULL, NULL
	},

	{
		{"escape_string_warning", PGC_USERSET, COMPAT_OPTIONS_PREVIOUS,
			gettext_noop("Warn about backslash escapes in ordinary string literals."),
			NULL
		},
		&escape_string_warning,
		true, NULL, NULL
	},

	{
		{"standard_conforming_strings", PGC_USERSET, COMPAT_OPTIONS_PREVIOUS,
			gettext_noop("Causes '...' strings to treat backslashes literally."),
			NULL,
			GUC_REPORT
		},
		&standard_conforming_strings,
		false, NULL, NULL
	},

	{
		{"synchronize_seqscans", PGC_USERSET, COMPAT_OPTIONS_PREVIOUS,
			gettext_noop("Enable synchronized sequential scans."),
			NULL
		},
		&synchronize_seqscans,
		true, NULL, NULL
	},

	{
		{"archive_mode", PGC_POSTMASTER, WAL_SETTINGS,
			gettext_noop("Allows archiving of WAL files using archive_command."),
			NULL
		},
		&XLogArchiveMode,
		false, NULL, NULL
	},

	{
		{"allow_system_table_mods", PGC_POSTMASTER, DEVELOPER_OPTIONS,
			gettext_noop("Allows modifications of the structure of system tables."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&allowSystemTableMods,
		false, NULL, NULL
	},

	{
		{"ignore_system_indexes", PGC_BACKEND, DEVELOPER_OPTIONS,
			gettext_noop("Disables reading from system indexes."),
			gettext_noop("It does not prevent updating the indexes, so it is safe "
						 "to use.  The worst consequence is slowness."),
			GUC_NOT_IN_SAMPLE
		},
		&IgnoreSystemIndexes,
		false, NULL, NULL
	},

	/* End-of-list marker */
	{
		{NULL, 0, 0, NULL, NULL}, NULL, false, NULL, NULL
	}
};


static struct config_int ConfigureNamesInt[] =
{
	{
		{"archive_timeout", PGC_SIGHUP, WAL_SETTINGS,
			gettext_noop("Forces a switch to the next xlog file if a "
						 "new file has not been started within N seconds."),
			NULL,
			GUC_UNIT_S
		},
		&XLogArchiveTimeout,
		0, 0, INT_MAX, NULL, NULL
	},
	{
		{"post_auth_delay", PGC_BACKEND, DEVELOPER_OPTIONS,
			gettext_noop("Waits N seconds on connection startup after authentication."),
			gettext_noop("This allows attaching a debugger to the process."),
			GUC_NOT_IN_SAMPLE | GUC_UNIT_S
		},
		&PostAuthDelay,
		0, 0, INT_MAX, NULL, NULL
	},
	{
		{"default_statistics_target", PGC_USERSET, QUERY_TUNING_OTHER,
			gettext_noop("Sets the default statistics target."),
			gettext_noop("This applies to table columns that have not had a "
				"column-specific target set via ALTER TABLE SET STATISTICS.")
		},
		&default_statistics_target,
		100, 1, 10000, NULL, NULL
	},
	{
		{"from_collapse_limit", PGC_USERSET, QUERY_TUNING_OTHER,
			gettext_noop("Sets the FROM-list size beyond which subqueries "
						 "are not collapsed."),
			gettext_noop("The planner will merge subqueries into upper "
				"queries if the resulting FROM list would have no more than "
						 "this many items.")
		},
		&from_collapse_limit,
		8, 1, INT_MAX, NULL, NULL
	},
	{
		{"join_collapse_limit", PGC_USERSET, QUERY_TUNING_OTHER,
			gettext_noop("Sets the FROM-list size beyond which JOIN "
						 "constructs are not flattened."),
			gettext_noop("The planner will flatten explicit JOIN "
						 "constructs into lists of FROM items whenever a "
						 "list of no more than this many items would result.")
		},
		&join_collapse_limit,
		8, 1, INT_MAX, NULL, NULL
	},
	{
		{"geqo_threshold", PGC_USERSET, QUERY_TUNING_GEQO,
			gettext_noop("Sets the threshold of FROM items beyond which GEQO is used."),
			NULL
		},
		&geqo_threshold,
		12, 2, INT_MAX, NULL, NULL
	},
	{
		{"geqo_effort", PGC_USERSET, QUERY_TUNING_GEQO,
			gettext_noop("GEQO: effort is used to set the default for other GEQO parameters."),
			NULL
		},
		&Geqo_effort,
		DEFAULT_GEQO_EFFORT, MIN_GEQO_EFFORT, MAX_GEQO_EFFORT, NULL, NULL
	},
	{
		{"geqo_pool_size", PGC_USERSET, QUERY_TUNING_GEQO,
			gettext_noop("GEQO: number of individuals in the population."),
			gettext_noop("Zero selects a suitable default value.")
		},
		&Geqo_pool_size,
		0, 0, INT_MAX, NULL, NULL
	},
	{
		{"geqo_generations", PGC_USERSET, QUERY_TUNING_GEQO,
			gettext_noop("GEQO: number of iterations of the algorithm."),
			gettext_noop("Zero selects a suitable default value.")
		},
		&Geqo_generations,
		0, 0, INT_MAX, NULL, NULL
	},

	{
		/* This is PGC_SIGHUP so all backends have the same value. */
		{"deadlock_timeout", PGC_SIGHUP, LOCK_MANAGEMENT,
			gettext_noop("Sets the time to wait on a lock before checking for deadlock."),
			NULL,
			GUC_UNIT_MS
		},
		&DeadlockTimeout,
		1000, 1, INT_MAX / 1000, NULL, NULL
	},

	/*
	 * Note: MaxBackends is limited to INT_MAX/4 because some places compute
	 * 4*MaxBackends without any overflow check.  This check is made in
	 * assign_maxconnections, since MaxBackends is computed as MaxConnections
	 * plus autovacuum_max_workers.
	 *
	 * Likewise we have to limit NBuffers to INT_MAX/2.
	 */
	{
		{"max_connections", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
			gettext_noop("Sets the maximum number of concurrent connections."),
			NULL
		},
		&MaxConnections,
		100, 1, INT_MAX / 4, assign_maxconnections, NULL
	},

	{
		{"superuser_reserved_connections", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
			gettext_noop("Sets the number of connection slots reserved for superusers."),
			NULL
		},
		&ReservedBackends,
		3, 0, INT_MAX / 4, NULL, NULL
	},

	{
		{"shared_buffers", PGC_POSTMASTER, RESOURCES_MEM,
			gettext_noop("Sets the number of shared memory buffers used by the server."),
			NULL,
			GUC_UNIT_BLOCKS
		},
		&NBuffers,
		1024, 16, INT_MAX / 2, NULL, NULL
	},

	{
		{"temp_buffers", PGC_USERSET, RESOURCES_MEM,
			gettext_noop("Sets the maximum number of temporary buffers used by each session."),
			NULL,
			GUC_UNIT_BLOCKS
		},
		&num_temp_buffers,
		1024, 100, INT_MAX / 2, NULL, show_num_temp_buffers
	},

	{
		{"port", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
			gettext_noop("Sets the TCP port the server listens on."),
			NULL
		},
		&PostPortNumber,
		DEF_PGPORT, 1, 65535, NULL, NULL
	},

	{
		{"unix_socket_permissions", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
			gettext_noop("Sets the access permissions of the Unix-domain socket."),
			gettext_noop("Unix-domain sockets use the usual Unix file system "
						 "permission set. The parameter value is expected to be a numeric mode "
						 "specification in the form accepted by the chmod and umask system "
						 "calls. (To use the customary octal format the number must start with "
						 "a 0 (zero).)")
		},
		&Unix_socket_permissions,
		0777, 0000, 0777, NULL, NULL
	},

	{
		{"work_mem", PGC_USERSET, RESOURCES_MEM,
			gettext_noop("Sets the maximum memory to be used for query workspaces."),
			gettext_noop("This much memory can be used by each internal "
						 "sort operation and hash table before switching to "
						 "temporary disk files."),
			GUC_UNIT_KB
		},
		&work_mem,
		1024, 64, MAX_KILOBYTES, NULL, NULL
	},

	{
		{"maintenance_work_mem", PGC_USERSET, RESOURCES_MEM,
			gettext_noop("Sets the maximum memory to be used for maintenance operations."),
			gettext_noop("This includes operations such as VACUUM and CREATE INDEX."),
			GUC_UNIT_KB
		},
		&maintenance_work_mem,
		16384, 1024, MAX_KILOBYTES, NULL, NULL
	},

	{
		{"max_stack_depth", PGC_SUSET, RESOURCES_MEM,
			gettext_noop("Sets the maximum stack depth, in kilobytes."),
			NULL,
			GUC_UNIT_KB
		},
		&max_stack_depth,
		100, 100, MAX_KILOBYTES, assign_max_stack_depth, NULL
	},

	{
		{"vacuum_cost_page_hit", PGC_USERSET, RESOURCES,
			gettext_noop("Vacuum cost for a page found in the buffer cache."),
			NULL
		},
		&VacuumCostPageHit,
		1, 0, 10000, NULL, NULL
	},

	{
		{"vacuum_cost_page_miss", PGC_USERSET, RESOURCES,
			gettext_noop("Vacuum cost for a page not found in the buffer cache."),
			NULL
		},
		&VacuumCostPageMiss,
		10, 0, 10000, NULL, NULL
	},

	{
		{"vacuum_cost_page_dirty", PGC_USERSET, RESOURCES,
			gettext_noop("Vacuum cost for a page dirtied by vacuum."),
			NULL
		},
		&VacuumCostPageDirty,
		20, 0, 10000, NULL, NULL
	},

	{
		{"vacuum_cost_limit", PGC_USERSET, RESOURCES,
			gettext_noop("Vacuum cost amount available before napping."),
			NULL
		},
		&VacuumCostLimit,
		200, 1, 10000, NULL, NULL
	},

	{
		{"vacuum_cost_delay", PGC_USERSET, RESOURCES,
			gettext_noop("Vacuum cost delay in milliseconds."),
			NULL,
			GUC_UNIT_MS
		},
		&VacuumCostDelay,
		0, 0, 100, NULL, NULL
	},

	{
		{"autovacuum_vacuum_cost_delay", PGC_SIGHUP, AUTOVACUUM,
			gettext_noop("Vacuum cost delay in milliseconds, for autovacuum."),
			NULL,
			GUC_UNIT_MS
		},
		&autovacuum_vac_cost_delay,
		20, -1, 100, NULL, NULL
	},

	{
		{"autovacuum_vacuum_cost_limit", PGC_SIGHUP, AUTOVACUUM,
			gettext_noop("Vacuum cost amount available before napping, for autovacuum."),
			NULL
		},
		&autovacuum_vac_cost_limit,
		-1, -1, 10000, NULL, NULL
	},

	{
		{"max_files_per_process", PGC_POSTMASTER, RESOURCES_KERNEL,
			gettext_noop("Sets the maximum number of simultaneously open files for each server process."),
			NULL
		},
		&max_files_per_process,
		1000, 25, INT_MAX, NULL, NULL
	},

	{
		{"max_prepared_transactions", PGC_POSTMASTER, RESOURCES,
			gettext_noop("Sets the maximum number of simultaneously prepared transactions."),
			NULL
		},
		&max_prepared_xacts,
		5, 0, INT_MAX, NULL, NULL
	},

#ifdef LOCK_DEBUG
	{
		{"trace_lock_oidmin", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("No description available."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&Trace_lock_oidmin,
		FirstNormalObjectId, 0, INT_MAX, NULL, NULL
	},
	{
		{"trace_lock_table", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("No description available."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&Trace_lock_table,
		0, 0, INT_MAX, NULL, NULL
	},
#endif

	{
		{"statement_timeout", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets the maximum allowed duration of any statement."),
			gettext_noop("A value of 0 turns off the timeout."),
			GUC_UNIT_MS
		},
		&StatementTimeout,
		0, 0, INT_MAX, NULL, NULL
	},

	{
		{"vacuum_freeze_min_age", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Minimum age at which VACUUM should freeze a table row."),
			NULL
		},
		&vacuum_freeze_min_age,
		50000000, 0, 1000000000, NULL, NULL
	},

	{
		{"vacuum_freeze_table_age", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Age at which VACUUM should scan whole table to freeze tuples."),
			NULL
		},
		&vacuum_freeze_table_age,
		150000000, 0, 2000000000, NULL, NULL
	},

	{
		{"max_locks_per_transaction", PGC_POSTMASTER, LOCK_MANAGEMENT,
			gettext_noop("Sets the maximum number of locks per transaction."),
			gettext_noop("The shared lock table is sized on the assumption that "
			  "at most max_locks_per_transaction * max_connections distinct "
						 "objects will need to be locked at any one time.")
		},
		&max_locks_per_xact,
		64, 10, INT_MAX, NULL, NULL
	},

	{
		{"authentication_timeout", PGC_SIGHUP, CONN_AUTH_SECURITY,
			gettext_noop("Sets the maximum allowed time to complete client authentication."),
			NULL,
			GUC_UNIT_S
		},
		&AuthenticationTimeout,
		60, 1, 600, NULL, NULL
	},

	{
		/* Not for general use */
		{"pre_auth_delay", PGC_SIGHUP, DEVELOPER_OPTIONS,
			gettext_noop("Waits N seconds on connection startup before authentication."),
			gettext_noop("This allows attaching a debugger to the process."),
			GUC_NOT_IN_SAMPLE | GUC_UNIT_S
		},
		&PreAuthDelay,
		0, 0, 60, NULL, NULL
	},

	{
		{"checkpoint_segments", PGC_SIGHUP, WAL_CHECKPOINTS,
			gettext_noop("Sets the maximum distance in log segments between automatic WAL checkpoints."),
			NULL
		},
		&CheckPointSegments,
		3, 1, INT_MAX, NULL, NULL
	},

	{
		{"checkpoint_timeout", PGC_SIGHUP, WAL_CHECKPOINTS,
			gettext_noop("Sets the maximum time between automatic WAL checkpoints."),
			NULL,
			GUC_UNIT_S
		},
		&CheckPointTimeout,
		300, 30, 3600, NULL, NULL
	},

	{
		{"checkpoint_warning", PGC_SIGHUP, WAL_CHECKPOINTS,
			gettext_noop("Enables warnings if checkpoint segments are filled more "
						 "frequently than this."),
			gettext_noop("Write a message to the server log if checkpoints "
			"caused by the filling of checkpoint segment files happens more "
						 "frequently than this number of seconds. Zero turns off the warning."),
			GUC_UNIT_S
		},
		&CheckPointWarning,
		30, 0, INT_MAX, NULL, NULL
	},

	{
		{"wal_buffers", PGC_POSTMASTER, WAL_SETTINGS,
			gettext_noop("Sets the number of disk-page buffers in shared memory for WAL."),
			NULL,
			GUC_UNIT_XBLOCKS
		},
		&XLOGbuffers,
		8, 4, INT_MAX, NULL, NULL
	},

	{
		{"wal_writer_delay", PGC_SIGHUP, WAL_SETTINGS,
			gettext_noop("WAL writer sleep time between WAL flushes."),
			NULL,
			GUC_UNIT_MS
		},
		&WalWriterDelay,
		200, 1, 10000, NULL, NULL
	},

	{
		{"commit_delay", PGC_USERSET, WAL_SETTINGS,
			gettext_noop("Sets the delay in microseconds between transaction commit and "
						 "flushing WAL to disk."),
			NULL
		},
		&CommitDelay,
		0, 0, 100000, NULL, NULL
	},

	{
		{"commit_siblings", PGC_USERSET, WAL_SETTINGS,
			gettext_noop("Sets the minimum concurrent open transactions before performing "
						 "commit_delay."),
			NULL
		},
		&CommitSiblings,
		5, 1, 1000, NULL, NULL
	},

	{
		{"extra_float_digits", PGC_USERSET, CLIENT_CONN_LOCALE,
			gettext_noop("Sets the number of digits displayed for floating-point values."),
			gettext_noop("This affects real, double precision, and geometric data types. "
			 "The parameter value is added to the standard number of digits "
						 "(FLT_DIG or DBL_DIG as appropriate).")
		},
		&extra_float_digits,
		0, -15, 2, NULL, NULL
	},

	{
		{"log_min_duration_statement", PGC_SUSET, LOGGING_WHEN,
			gettext_noop("Sets the minimum execution time above which "
						 "statements will be logged."),
			gettext_noop("Zero prints all queries. -1 turns this feature off."),
			GUC_UNIT_MS
		},
		&log_min_duration_statement,
		-1, -1, INT_MAX / 1000, NULL, NULL
	},

	{
		{"log_autovacuum_min_duration", PGC_SIGHUP, LOGGING_WHAT,
			gettext_noop("Sets the minimum execution time above which "
						 "autovacuum actions will be logged."),
			gettext_noop("Zero prints all actions. -1 turns autovacuum logging off."),
			GUC_UNIT_MS
		},
		&Log_autovacuum_min_duration,
		-1, -1, INT_MAX / 1000, NULL, NULL
	},

	{
		{"bgwriter_delay", PGC_SIGHUP, RESOURCES,
			gettext_noop("Background writer sleep time between rounds."),
			NULL,
			GUC_UNIT_MS
		},
		&BgWriterDelay,
		200, 10, 10000, NULL, NULL
	},

	{
		{"bgwriter_lru_maxpages", PGC_SIGHUP, RESOURCES,
			gettext_noop("Background writer maximum number of LRU pages to flush per round."),
			NULL
		},
		&bgwriter_lru_maxpages,
		100, 0, 1000, NULL, NULL
	},

	{
		{"effective_io_concurrency",
#ifdef USE_PREFETCH
		PGC_USERSET,
#else
		PGC_INTERNAL,
#endif
		RESOURCES,
			gettext_noop("Number of simultaneous requests that can be handled efficiently by the disk subsystem."),
			gettext_noop("For RAID arrays, this should be approximately the number of drive spindles in the array.")
		},
		&effective_io_concurrency,
#ifdef USE_PREFETCH
		1, 0, 1000,
#else
		0, 0, 0,
#endif
		assign_effective_io_concurrency, NULL
	},

	{
		{"log_rotation_age", PGC_SIGHUP, LOGGING_WHERE,
			gettext_noop("Automatic log file rotation will occur after N minutes."),
			NULL,
			GUC_UNIT_MIN
		},
		&Log_RotationAge,
		HOURS_PER_DAY * MINS_PER_HOUR, 0, INT_MAX / MINS_PER_HOUR, NULL, NULL
	},

	{
		{"log_rotation_size", PGC_SIGHUP, LOGGING_WHERE,
			gettext_noop("Automatic log file rotation will occur after N kilobytes."),
			NULL,
			GUC_UNIT_KB
		},
		&Log_RotationSize,
		10 * 1024, 0, INT_MAX / 1024, NULL, NULL
	},

	{
		{"max_function_args", PGC_INTERNAL, PRESET_OPTIONS,
			gettext_noop("Shows the maximum number of function arguments."),
			NULL,
			GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&max_function_args,
		FUNC_MAX_ARGS, FUNC_MAX_ARGS, FUNC_MAX_ARGS, NULL, NULL
	},

	{
		{"max_index_keys", PGC_INTERNAL, PRESET_OPTIONS,
			gettext_noop("Shows the maximum number of index keys."),
			NULL,
			GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&max_index_keys,
		INDEX_MAX_KEYS, INDEX_MAX_KEYS, INDEX_MAX_KEYS, NULL, NULL
	},

	{
		{"max_identifier_length", PGC_INTERNAL, PRESET_OPTIONS,
			gettext_noop("Shows the maximum identifier length."),
			NULL,
			GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&max_identifier_length,
		NAMEDATALEN - 1, NAMEDATALEN - 1, NAMEDATALEN - 1, NULL, NULL
	},

	{
		{"block_size", PGC_INTERNAL, PRESET_OPTIONS,
			gettext_noop("Shows the size of a disk block."),
			NULL,
			GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&block_size,
		BLCKSZ, BLCKSZ, BLCKSZ, NULL, NULL
	},

	{
		{"segment_size", PGC_INTERNAL, PRESET_OPTIONS,
		    gettext_noop("Shows the number of pages per disk file."),
		    NULL,
		    GUC_UNIT_BLOCKS | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&segment_size,
		RELSEG_SIZE, RELSEG_SIZE, RELSEG_SIZE, NULL, NULL
	},

	{
		{"wal_block_size", PGC_INTERNAL, PRESET_OPTIONS,
			gettext_noop("Shows the block size in the write ahead log."),
			NULL,
			GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&wal_block_size,
		XLOG_BLCKSZ, XLOG_BLCKSZ, XLOG_BLCKSZ, NULL, NULL
	},

	{
		{"wal_segment_size", PGC_INTERNAL, PRESET_OPTIONS,
			gettext_noop("Shows the number of pages per write ahead log segment."),
			NULL,
			GUC_UNIT_XBLOCKS | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&wal_segment_size,
		(XLOG_SEG_SIZE / XLOG_BLCKSZ), 
		(XLOG_SEG_SIZE / XLOG_BLCKSZ), 
		(XLOG_SEG_SIZE / XLOG_BLCKSZ),
		NULL, NULL
	},

	{
		{"autovacuum_naptime", PGC_SIGHUP, AUTOVACUUM,
			gettext_noop("Time to sleep between autovacuum runs."),
			NULL,
			GUC_UNIT_S
		},
		&autovacuum_naptime,
		60, 1, INT_MAX / 1000, NULL, NULL
	},
	{
		{"autovacuum_vacuum_threshold", PGC_SIGHUP, AUTOVACUUM,
			gettext_noop("Minimum number of tuple updates or deletes prior to vacuum."),
			NULL
		},
		&autovacuum_vac_thresh,
		50, 0, INT_MAX, NULL, NULL
	},
	{
		{"autovacuum_analyze_threshold", PGC_SIGHUP, AUTOVACUUM,
			gettext_noop("Minimum number of tuple inserts, updates or deletes prior to analyze."),
			NULL
		},
		&autovacuum_anl_thresh,
		50, 0, INT_MAX, NULL, NULL
	},
	{
		/* see varsup.c for why this is PGC_POSTMASTER not PGC_SIGHUP */
		{"autovacuum_freeze_max_age", PGC_POSTMASTER, AUTOVACUUM,
			gettext_noop("Age at which to autovacuum a table to prevent transaction ID wraparound."),
			NULL
		},
		&autovacuum_freeze_max_age,
		200000000, 100000000, 2000000000, NULL, NULL
	},
	{
		/* see max_connections */
		{"autovacuum_max_workers", PGC_POSTMASTER, AUTOVACUUM,
			gettext_noop("Sets the maximum number of simultaneously running autovacuum worker processes."),
			NULL
		},
		&autovacuum_max_workers,
		3, 1, INT_MAX / 4, assign_autovacuum_max_workers, NULL
	},

	{
		{"tcp_keepalives_idle", PGC_USERSET, CLIENT_CONN_OTHER,
			gettext_noop("Time between issuing TCP keepalives."),
			gettext_noop("A value of 0 uses the system default."),
			GUC_UNIT_S
		},
		&tcp_keepalives_idle,
		0, 0, INT_MAX, assign_tcp_keepalives_idle, show_tcp_keepalives_idle
	},

	{
		{"tcp_keepalives_interval", PGC_USERSET, CLIENT_CONN_OTHER,
			gettext_noop("Time between TCP keepalive retransmits."),
			gettext_noop("A value of 0 uses the system default."),
			GUC_UNIT_S
		},
		&tcp_keepalives_interval,
		0, 0, INT_MAX, assign_tcp_keepalives_interval, show_tcp_keepalives_interval
	},

	{
		{"tcp_keepalives_count", PGC_USERSET, CLIENT_CONN_OTHER,
			gettext_noop("Maximum number of TCP keepalive retransmits."),
			gettext_noop("This controls the number of consecutive keepalive retransmits that can be "
						 "lost before a connection is considered dead. A value of 0 uses the "
						 "system default."),
		},
		&tcp_keepalives_count,
		0, 0, INT_MAX, assign_tcp_keepalives_count, show_tcp_keepalives_count
	},

	{
		{"gin_fuzzy_search_limit", PGC_USERSET, CLIENT_CONN_OTHER,
			gettext_noop("Sets the maximum allowed result for exact search by GIN."),
			NULL,
			0
		},
		&GinFuzzySearchLimit,
		0, 0, INT_MAX, NULL, NULL
	},

	{
		{"effective_cache_size", PGC_USERSET, QUERY_TUNING_COST,
			gettext_noop("Sets the planner's assumption about the size of the disk cache."),
			gettext_noop("That is, the portion of the kernel's disk cache that "
						 "will be used for PostgreSQL data files. This is measured in disk "
						 "pages, which are normally 8 kB each."),
			GUC_UNIT_BLOCKS,
		},
		&effective_cache_size,
		DEFAULT_EFFECTIVE_CACHE_SIZE, 1, INT_MAX, NULL, NULL
	},

	{
		/* Can't be set in postgresql.conf */
		{"server_version_num", PGC_INTERNAL, PRESET_OPTIONS,
			gettext_noop("Shows the server version as an integer."),
			NULL,
			GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&server_version_num,
		PG_VERSION_NUM, PG_VERSION_NUM, PG_VERSION_NUM, NULL, NULL
	},

	{
		{"log_temp_files", PGC_SUSET, LOGGING_WHAT,
			gettext_noop("Log the use of temporary files larger than this number of kilobytes."),
			gettext_noop("Zero logs all files. The default is -1 (turning this feature off)."),
			GUC_UNIT_KB
		},
		&log_temp_files,
		-1, -1, INT_MAX, NULL, NULL
	},

	{
		{"track_activity_query_size", PGC_POSTMASTER, RESOURCES_MEM,
			gettext_noop("Sets the size reserved for pg_stat_activity.current_query, in bytes."),
			NULL,
		},
		&pgstat_track_activity_query_size,
		1024, 100, 102400, NULL, NULL
	},

	/* End-of-list marker */
	{
		{NULL, 0, 0, NULL, NULL}, NULL, 0, 0, 0, NULL, NULL
	}
};


static struct config_real ConfigureNamesReal[] =
{
	{
		{"seq_page_cost", PGC_USERSET, QUERY_TUNING_COST,
			gettext_noop("Sets the planner's estimate of the cost of a "
						 "sequentially fetched disk page."),
			NULL
		},
		&seq_page_cost,
		DEFAULT_SEQ_PAGE_COST, 0, DBL_MAX, NULL, NULL
	},
	{
		{"random_page_cost", PGC_USERSET, QUERY_TUNING_COST,
			gettext_noop("Sets the planner's estimate of the cost of a "
						 "nonsequentially fetched disk page."),
			NULL
		},
		&random_page_cost,
		DEFAULT_RANDOM_PAGE_COST, 0, DBL_MAX, NULL, NULL
	},
	{
		{"cpu_tuple_cost", PGC_USERSET, QUERY_TUNING_COST,
			gettext_noop("Sets the planner's estimate of the cost of "
						 "processing each tuple (row)."),
			NULL
		},
		&cpu_tuple_cost,
		DEFAULT_CPU_TUPLE_COST, 0, DBL_MAX, NULL, NULL
	},
	{
		{"cpu_index_tuple_cost", PGC_USERSET, QUERY_TUNING_COST,
			gettext_noop("Sets the planner's estimate of the cost of "
						 "processing each index entry during an index scan."),
			NULL
		},
		&cpu_index_tuple_cost,
		DEFAULT_CPU_INDEX_TUPLE_COST, 0, DBL_MAX, NULL, NULL
	},
	{
		{"cpu_operator_cost", PGC_USERSET, QUERY_TUNING_COST,
			gettext_noop("Sets the planner's estimate of the cost of "
						 "processing each operator or function call."),
			NULL
		},
		&cpu_operator_cost,
		DEFAULT_CPU_OPERATOR_COST, 0, DBL_MAX, NULL, NULL
	},

	{
		{"cursor_tuple_fraction", PGC_USERSET, QUERY_TUNING_OTHER,
			gettext_noop("Sets the planner's estimate of the fraction of "
						 "a cursor's rows that will be retrieved."),
			NULL
		},
		&cursor_tuple_fraction,
		DEFAULT_CURSOR_TUPLE_FRACTION, 0.0, 1.0, NULL, NULL
	},

	{
		{"geqo_selection_bias", PGC_USERSET, QUERY_TUNING_GEQO,
			gettext_noop("GEQO: selective pressure within the population."),
			NULL
		},
		&Geqo_selection_bias,
		DEFAULT_GEQO_SELECTION_BIAS, MIN_GEQO_SELECTION_BIAS,
		MAX_GEQO_SELECTION_BIAS, NULL, NULL
	},

	{
		{"bgwriter_lru_multiplier", PGC_SIGHUP, RESOURCES,
			gettext_noop("Multiple of the average buffer usage to free per round."),
			NULL
		},
		&bgwriter_lru_multiplier,
		2.0, 0.0, 10.0, NULL, NULL
	},

	{
		{"seed", PGC_USERSET, UNGROUPED,
			gettext_noop("Sets the seed for random-number generation."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NO_RESET_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&phony_random_seed,
		0.0, -1.0, 1.0, assign_random_seed, show_random_seed
	},

	{
		{"autovacuum_vacuum_scale_factor", PGC_SIGHUP, AUTOVACUUM,
			gettext_noop("Number of tuple updates or deletes prior to vacuum as a fraction of reltuples."),
			NULL
		},
		&autovacuum_vac_scale,
		0.2, 0.0, 100.0, NULL, NULL
	},
	{
		{"autovacuum_analyze_scale_factor", PGC_SIGHUP, AUTOVACUUM,
			gettext_noop("Number of tuple inserts, updates or deletes prior to analyze as a fraction of reltuples."),
			NULL
		},
		&autovacuum_anl_scale,
		0.1, 0.0, 100.0, NULL, NULL
	},

	{
		{"checkpoint_completion_target", PGC_SIGHUP, WAL_CHECKPOINTS,
			gettext_noop("Time spent flushing dirty buffers during checkpoint, as fraction of checkpoint interval."),
			NULL
		},
		&CheckPointCompletionTarget,
		0.5, 0.0, 1.0, NULL, NULL
	},

	/* End-of-list marker */
	{
		{NULL, 0, 0, NULL, NULL}, NULL, 0.0, 0.0, 0.0, NULL, NULL
	}
};


static struct config_string ConfigureNamesString[] =
{
	{
		{"archive_command", PGC_SIGHUP, WAL_SETTINGS,
			gettext_noop("Sets the shell command that will be called to archive a WAL file."),
			NULL
		},
		&XLogArchiveCommand,
		"", NULL, show_archive_command
	},

	{
		{"client_encoding", PGC_USERSET, CLIENT_CONN_LOCALE,
			gettext_noop("Sets the client's character set encoding."),
			NULL,
			GUC_IS_NAME | GUC_REPORT
		},
		&client_encoding_string,
		"SQL_ASCII", assign_client_encoding, NULL
	},

	{
		{"log_line_prefix", PGC_SIGHUP, LOGGING_WHAT,
			gettext_noop("Controls information prefixed to each log line."),
			gettext_noop("If blank, no prefix is used.")
		},
		&Log_line_prefix,
		"", NULL, NULL
	},

	{
		{"log_timezone", PGC_SIGHUP, LOGGING_WHAT,
			gettext_noop("Sets the time zone to use in log messages."),
			NULL
		},
		&log_timezone_string,
		"UNKNOWN", assign_log_timezone, show_log_timezone
	},

	{
		{"DateStyle", PGC_USERSET, CLIENT_CONN_LOCALE,
			gettext_noop("Sets the display format for date and time values."),
			gettext_noop("Also controls interpretation of ambiguous "
						 "date inputs."),
			GUC_LIST_INPUT | GUC_REPORT
		},
		&datestyle_string,
		"ISO, MDY", assign_datestyle, NULL
	},

	{
		{"default_tablespace", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets the default tablespace to create tables and indexes in."),
			gettext_noop("An empty string selects the database's default tablespace."),
			GUC_IS_NAME
		},
		&default_tablespace,
		"", assign_default_tablespace, NULL
	},

	{
		{"temp_tablespaces", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets the tablespace(s) to use for temporary tables and sort files."),
			NULL,
			GUC_LIST_INPUT | GUC_LIST_QUOTE
		},
		&temp_tablespaces,
		"", assign_temp_tablespaces, NULL
	},

	{
		{"dynamic_library_path", PGC_SUSET, CLIENT_CONN_OTHER,
			gettext_noop("Sets the path for dynamically loadable modules."),
			gettext_noop("If a dynamically loadable module needs to be opened and "
						 "the specified name does not have a directory component (i.e., the "
						 "name does not contain a slash), the system will search this path for "
						 "the specified file."),
			GUC_SUPERUSER_ONLY
		},
		&Dynamic_library_path,
		"$libdir", NULL, NULL
	},

	{
		{"krb_server_keyfile", PGC_SIGHUP, CONN_AUTH_SECURITY,
			gettext_noop("Sets the location of the Kerberos server key file."),
			NULL,
			GUC_SUPERUSER_ONLY
		},
		&pg_krb_server_keyfile,
		PG_KRB_SRVTAB, NULL, NULL
	},

	{
		{"krb_srvname", PGC_SIGHUP, CONN_AUTH_SECURITY,
			gettext_noop("Sets the name of the Kerberos service."),
			NULL
		},
		&pg_krb_srvnam,
		PG_KRB_SRVNAM, NULL, NULL
	},

	{
		{"bonjour_name", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
			gettext_noop("Sets the Bonjour broadcast service name."),
			NULL
		},
		&bonjour_name,
		"", NULL, NULL
	},

	/* See main.c about why defaults for LC_foo are not all alike */

	{
		{"lc_collate", PGC_INTERNAL, CLIENT_CONN_LOCALE,
			gettext_noop("Shows the collation order locale."),
			NULL,
			GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&locale_collate,
		"C", NULL, NULL
	},

	{
		{"lc_ctype", PGC_INTERNAL, CLIENT_CONN_LOCALE,
			gettext_noop("Shows the character classification and case conversion locale."),
			NULL,
			GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&locale_ctype,
		"C", NULL, NULL
	},

	{
		{"lc_messages", PGC_SUSET, CLIENT_CONN_LOCALE,
			gettext_noop("Sets the language in which messages are displayed."),
			NULL
		},
		&locale_messages,
		"", locale_messages_assign, NULL
	},

	{
		{"lc_monetary", PGC_USERSET, CLIENT_CONN_LOCALE,
			gettext_noop("Sets the locale for formatting monetary amounts."),
			NULL
		},
		&locale_monetary,
		"C", locale_monetary_assign, NULL
	},

	{
		{"lc_numeric", PGC_USERSET, CLIENT_CONN_LOCALE,
			gettext_noop("Sets the locale for formatting numbers."),
			NULL
		},
		&locale_numeric,
		"C", locale_numeric_assign, NULL
	},

	{
		{"lc_time", PGC_USERSET, CLIENT_CONN_LOCALE,
			gettext_noop("Sets the locale for formatting date and time values."),
			NULL
		},
		&locale_time,
		"C", locale_time_assign, NULL
	},

	{
		{"shared_preload_libraries", PGC_POSTMASTER, RESOURCES_KERNEL,
			gettext_noop("Lists shared libraries to preload into server."),
			NULL,
			GUC_LIST_INPUT | GUC_LIST_QUOTE | GUC_SUPERUSER_ONLY
		},
		&shared_preload_libraries_string,
		"", NULL, NULL
	},

	{
		{"local_preload_libraries", PGC_BACKEND, CLIENT_CONN_OTHER,
			gettext_noop("Lists shared libraries to preload into each backend."),
			NULL,
			GUC_LIST_INPUT | GUC_LIST_QUOTE
		},
		&local_preload_libraries_string,
		"", NULL, NULL
	},

	{
		{"search_path", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets the schema search order for names that are not schema-qualified."),
			NULL,
			GUC_LIST_INPUT | GUC_LIST_QUOTE
		},
		&namespace_search_path,
		"\"$user\",public", assign_search_path, NULL
	},

	{
		/* Can't be set in postgresql.conf */
		{"server_encoding", PGC_INTERNAL, CLIENT_CONN_LOCALE,
			gettext_noop("Sets the server (database) character set encoding."),
			NULL,
			GUC_IS_NAME | GUC_REPORT | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&server_encoding_string,
		"SQL_ASCII", NULL, NULL
	},

	{
		/* Can't be set in postgresql.conf */
		{"server_version", PGC_INTERNAL, PRESET_OPTIONS,
			gettext_noop("Shows the server version."),
			NULL,
			GUC_REPORT | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&server_version_string,
		PG_VERSION, NULL, NULL
	},

	{
		/* Not for general use --- used by SET ROLE */
		{"role", PGC_USERSET, UNGROUPED,
			gettext_noop("Sets the current role."),
			NULL,
			GUC_IS_NAME | GUC_NO_SHOW_ALL | GUC_NO_RESET_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&role_string,
		"none", assign_role, show_role
	},

	{
		/* Not for general use --- used by SET SESSION AUTHORIZATION */
		{"session_authorization", PGC_USERSET, UNGROUPED,
			gettext_noop("Sets the session user name."),
			NULL,
			GUC_IS_NAME | GUC_REPORT | GUC_NO_SHOW_ALL | GUC_NO_RESET_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&session_authorization_string,
		NULL, assign_session_authorization, show_session_authorization
	},

	{
		{"log_destination", PGC_SIGHUP, LOGGING_WHERE,
			gettext_noop("Sets the destination for server log output."),
			gettext_noop("Valid values are combinations of \"stderr\", "
						 "\"syslog\", \"csvlog\", and \"eventlog\", "
						 "depending on the platform."),
			GUC_LIST_INPUT
		},
		&log_destination_string,
		"stderr", assign_log_destination, NULL
	},
	{
		{"log_directory", PGC_SIGHUP, LOGGING_WHERE,
			gettext_noop("Sets the destination directory for log files."),
			gettext_noop("Can be specified as relative to the data directory "
						 "or as absolute path."),
			GUC_SUPERUSER_ONLY
		},
		&Log_directory,
		"pg_log", assign_canonical_path, NULL
	},
	{
		{"log_filename", PGC_SIGHUP, LOGGING_WHERE,
			gettext_noop("Sets the file name pattern for log files."),
			NULL,
			GUC_SUPERUSER_ONLY
		},
		&Log_filename,
		"postgresql-%Y-%m-%d_%H%M%S.log", NULL, NULL
	},

#ifdef HAVE_SYSLOG
	{
		{"syslog_ident", PGC_SIGHUP, LOGGING_WHERE,
			gettext_noop("Sets the program name used to identify PostgreSQL "
						 "messages in syslog."),
			NULL
		},
		&syslog_ident_str,
		"postgres", assign_syslog_ident, NULL
	},
#endif

	{
		{"TimeZone", PGC_USERSET, CLIENT_CONN_LOCALE,
			gettext_noop("Sets the time zone for displaying and interpreting time stamps."),
			NULL,
			GUC_REPORT
		},
		&timezone_string,
		"UNKNOWN", assign_timezone, show_timezone
	},
	{
		{"timezone_abbreviations", PGC_USERSET, CLIENT_CONN_LOCALE,
			gettext_noop("Selects a file of time zone abbreviations."),
			NULL
		},
		&timezone_abbreviations_string,
		"UNKNOWN", assign_timezone_abbreviations, NULL
	},

	{
		{"transaction_isolation", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets the current transaction's isolation level."),
			NULL,
			GUC_NO_RESET_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&XactIsoLevel_string,
		NULL, assign_XactIsoLevel, show_XactIsoLevel
	},

	{
		{"unix_socket_group", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
			gettext_noop("Sets the owning group of the Unix-domain socket."),
			gettext_noop("The owning user of the socket is always the user "
						 "that starts the server.")
		},
		&Unix_socket_group,
		"", NULL, NULL
	},

	{
		{"unix_socket_directory", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
			gettext_noop("Sets the directory where the Unix-domain socket will be created."),
			NULL,
			GUC_SUPERUSER_ONLY
		},
		&UnixSocketDir,
		"", assign_canonical_path, NULL
	},

	{
		{"listen_addresses", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
			gettext_noop("Sets the host name or IP address(es) to listen to."),
			NULL,
			GUC_LIST_INPUT
		},
		&ListenAddresses,
		"localhost", NULL, NULL
	},

	{
		{"custom_variable_classes", PGC_SIGHUP, CUSTOM_OPTIONS,
			gettext_noop("Sets the list of known custom variable classes."),
			NULL,
			GUC_LIST_INPUT | GUC_LIST_QUOTE
		},
		&custom_variable_classes,
		NULL, assign_custom_variable_classes, NULL
	},

	{
		{"data_directory", PGC_POSTMASTER, FILE_LOCATIONS,
			gettext_noop("Sets the server's data directory."),
			NULL,
			GUC_SUPERUSER_ONLY
		},
		&data_directory,
		NULL, NULL, NULL
	},

	{
		{"config_file", PGC_POSTMASTER, FILE_LOCATIONS,
			gettext_noop("Sets the server's main configuration file."),
			NULL,
			GUC_DISALLOW_IN_FILE | GUC_SUPERUSER_ONLY
		},
		&ConfigFileName,
		NULL, NULL, NULL
	},

	{
		{"hba_file", PGC_POSTMASTER, FILE_LOCATIONS,
			gettext_noop("Sets the server's \"hba\" configuration file."),
			NULL,
			GUC_SUPERUSER_ONLY
		},
		&HbaFileName,
		NULL, NULL, NULL
	},

	{
		{"ident_file", PGC_POSTMASTER, FILE_LOCATIONS,
			gettext_noop("Sets the server's \"ident\" configuration file."),
			NULL,
			GUC_SUPERUSER_ONLY
		},
		&IdentFileName,
		NULL, NULL, NULL
	},

	{
		{"external_pid_file", PGC_POSTMASTER, FILE_LOCATIONS,
			gettext_noop("Writes the postmaster PID to the specified file."),
			NULL,
			GUC_SUPERUSER_ONLY
		},
		&external_pid_file,
		NULL, assign_canonical_path, NULL
	},

	{
		{"stats_temp_directory", PGC_SIGHUP, STATS_COLLECTOR,
			gettext_noop("Writes temporary statistics files to the specified directory."),
			NULL,
			GUC_SUPERUSER_ONLY
		},
		&pgstat_temp_directory,
		"pg_stat_tmp", assign_pgstat_temp_directory, NULL
	},

	{
		{"default_text_search_config", PGC_USERSET, CLIENT_CONN_LOCALE,
			gettext_noop("Sets default text search configuration."),
			NULL
		},
		&TSCurrentConfig,
		"pg_catalog.simple", assignTSCurrentConfig, NULL
	},

#ifdef USE_SSL
	{
		{"ssl_ciphers", PGC_POSTMASTER, CONN_AUTH_SECURITY,
			gettext_noop("Sets the list of allowed SSL ciphers."),
			NULL,
			GUC_SUPERUSER_ONLY
		},
		&SSLCipherSuites,
		"ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH", NULL, NULL
	},
#endif   /* USE_SSL */

	/* End-of-list marker */
	{
		{NULL, 0, 0, NULL, NULL}, NULL, NULL, NULL, NULL
	}
};


static struct config_enum ConfigureNamesEnum[] =
{
	{
		{"backslash_quote", PGC_USERSET, COMPAT_OPTIONS_PREVIOUS,
			gettext_noop("Sets whether \"\\'\" is allowed in string literals."),
			NULL
		},
		&backslash_quote,
		BACKSLASH_QUOTE_SAFE_ENCODING, backslash_quote_options, NULL, NULL
	},

	{
		{"client_min_messages", PGC_USERSET, LOGGING_WHEN,
			gettext_noop("Sets the message levels that are sent to the client."),
			gettext_noop("Each level includes all the levels that follow it. The later"
						 " the level, the fewer messages are sent.")
		},
		&client_min_messages,
		NOTICE, client_message_level_options, NULL, NULL
	},

	{
		{"constraint_exclusion", PGC_USERSET, QUERY_TUNING_OTHER,
			gettext_noop("Enables the planner to use constraints to optimize queries."),
			gettext_noop("Table scans will be skipped if their constraints"
						 " guarantee that no rows match the query.")
		},
		&constraint_exclusion,
		CONSTRAINT_EXCLUSION_PARTITION, constraint_exclusion_options,
		NULL, NULL
	},

	{
		{"default_transaction_isolation", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets the transaction isolation level of each new transaction."),
			NULL
		},
		&DefaultXactIsoLevel,
		XACT_READ_COMMITTED, isolation_level_options, NULL, NULL
	},

 	{
		{"IntervalStyle", PGC_USERSET, CLIENT_CONN_LOCALE,
			gettext_noop("Sets the display format for interval values."),
			NULL,
			GUC_REPORT
		},
		&IntervalStyle,
		INTSTYLE_POSTGRES, intervalstyle_options, NULL, NULL
	},

	{
		{"log_error_verbosity", PGC_SUSET, LOGGING_WHEN,
			gettext_noop("Sets the verbosity of logged messages."),
			NULL
		},
		&Log_error_verbosity,
		PGERROR_DEFAULT, log_error_verbosity_options, NULL, NULL
	},

	{
		{"log_min_messages", PGC_SUSET, LOGGING_WHEN,
			gettext_noop("Sets the message levels that are logged."),
			gettext_noop("Each level includes all the levels that follow it. The later"
						 " the level, the fewer messages are sent.")
		},
		&log_min_messages,
		WARNING, server_message_level_options, NULL, NULL
	},

	{
		{"log_min_error_statement", PGC_SUSET, LOGGING_WHEN,
			gettext_noop("Causes all statements generating error at or above this level to be logged."),
			gettext_noop("Each level includes all the levels that follow it. The later"
						 " the level, the fewer messages are sent.")
		},
		&log_min_error_statement,
		ERROR, server_message_level_options, NULL, NULL
	},

	{
		{"log_statement", PGC_SUSET, LOGGING_WHAT,
			gettext_noop("Sets the type of statements logged."),
			NULL
		},
		&log_statement,
		LOGSTMT_NONE, log_statement_options, NULL, NULL
	},

#ifdef HAVE_SYSLOG
	{
		{"syslog_facility", PGC_SIGHUP, LOGGING_WHERE,
			gettext_noop("Sets the syslog \"facility\" to be used when syslog enabled."),
			NULL
		},
		&syslog_facility,
		LOG_LOCAL0, syslog_facility_options, assign_syslog_facility, NULL
	},
#endif

	{
		{"regex_flavor", PGC_USERSET, COMPAT_OPTIONS_PREVIOUS,
			gettext_noop("Sets the regular expression \"flavor\"."),
			NULL
		},
		&regex_flavor,
		REG_ADVANCED, regex_flavor_options, NULL, NULL
	},

	{
		{"session_replication_role", PGC_SUSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets the session's behavior for triggers and rewrite rules."),
			NULL
		},
		&SessionReplicationRole,
		SESSION_REPLICATION_ROLE_ORIGIN, session_replication_role_options,
		assign_session_replication_role, NULL
	},

	{
		{"track_functions", PGC_SUSET, STATS_COLLECTOR,
			gettext_noop("Collects function-level statistics on database activity."),
			NULL
		},
		&pgstat_track_functions,
		TRACK_FUNC_OFF, track_function_options, NULL, NULL
	},

	{
		{"wal_sync_method", PGC_SIGHUP, WAL_SETTINGS,
			gettext_noop("Selects the method used for forcing WAL updates to disk."),
			NULL
		},
		&sync_method,
		DEFAULT_SYNC_METHOD, sync_method_options, 
		assign_xlog_sync_method, NULL
	},

	{
		{"xmlbinary", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets how binary values are to be encoded in XML."),
			NULL
		},
		&xmlbinary,
		XMLBINARY_BASE64, xmlbinary_options, NULL, NULL
	},

	{
		{"xmloption", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets whether XML data in implicit parsing and serialization "
						 "operations is to be considered as documents or content fragments."),
			NULL
		},
		&xmloption,
		XMLOPTION_CONTENT, xmloption_options, NULL, NULL
	},


	/* End-of-list marker */
	{
		{NULL, 0, 0, NULL, NULL}, NULL, 0, NULL, NULL, NULL
	}
};

/******** end of options list ********/


/*
 * To allow continued support of obsolete names for GUC variables, we apply
 * the following mappings to any unrecognized name.  Note that an old name
 * should be mapped to a new one only if the new variable has very similar
 * semantics to the old.
 */
static const char *const map_old_guc_names[] = {
	"sort_mem", "work_mem",
	"vacuum_mem", "maintenance_work_mem",
	NULL
};


/*
 * Actual lookup of variables is done through this single, sorted array.
 */
static struct config_generic **guc_variables;

/* Current number of variables contained in the vector */
static int	num_guc_variables;

/* Vector capacity */
static int	size_guc_variables;


static bool guc_dirty;			/* TRUE if need to do commit/abort work */

static bool reporting_enabled;	/* TRUE to enable GUC_REPORT */

static int	GUCNestLevel = 0;	/* 1 when in main transaction */


static int	guc_var_compare(const void *a, const void *b);
static int	guc_name_compare(const char *namea, const char *nameb);
static void InitializeOneGUCOption(struct config_generic *gconf);
static void push_old_value(struct config_generic * gconf, GucAction action);
static void ReportGUCOption(struct config_generic * record);
static void ShowGUCConfigOption(const char *name, DestReceiver *dest);
static void ShowAllGUCConfig(DestReceiver *dest);
static char *_ShowOption(struct config_generic * record, bool use_units);
static bool is_newvalue_equal(struct config_generic * record, const char *newvalue);


/*
 * Some infrastructure for checking malloc/strdup/realloc calls
 */
static void *
guc_malloc(int elevel, size_t size)
{
	void	   *data;

	data = malloc(size);
	if (data == NULL)
		ereport(elevel,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	return data;
}

static void *
guc_realloc(int elevel, void *old, size_t size)
{
	void	   *data;

	data = realloc(old, size);
	if (data == NULL)
		ereport(elevel,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	return data;
}

static char *
guc_strdup(int elevel, const char *src)
{
	char	   *data;

	data = strdup(src);
	if (data == NULL)
		ereport(elevel,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	return data;
}


/*
 * Support for assigning to a field of a string GUC item.  Free the prior
 * value if it's not referenced anywhere else in the item (including stacked
 * states).
 */
static void
set_string_field(struct config_string * conf, char **field, char *newval)
{
	char	   *oldval = *field;
	GucStack   *stack;

	/* Do the assignment */
	*field = newval;

	/* Exit if any duplicate references, or if old value was NULL anyway */
	if (oldval == NULL ||
		oldval == *(conf->variable) ||
		oldval == conf->reset_val ||
		oldval == conf->boot_val)
		return;
	for (stack = conf->gen.stack; stack; stack = stack->prev)
	{
		if (oldval == stack->prior.stringval ||
			oldval == stack->masked.stringval)
			return;
	}

	/* Not used anymore, so free it */
	free(oldval);
}

/*
 * Detect whether strval is referenced anywhere in a GUC string item
 */
static bool
string_field_used(struct config_string * conf, char *strval)
{
	GucStack   *stack;

	if (strval == *(conf->variable) ||
		strval == conf->reset_val ||
		strval == conf->boot_val)
		return true;
	for (stack = conf->gen.stack; stack; stack = stack->prev)
	{
		if (strval == stack->prior.stringval ||
			strval == stack->masked.stringval)
			return true;
	}
	return false;
}

/*
 * Support for copying a variable's active value into a stack entry
 */
static void
set_stack_value(struct config_generic * gconf, union config_var_value * val)
{
	switch (gconf->vartype)
	{
		case PGC_BOOL:
			val->boolval =
				*((struct config_bool *) gconf)->variable;
			break;
		case PGC_INT:
			val->intval =
				*((struct config_int *) gconf)->variable;
			break;
		case PGC_REAL:
			val->realval =
				*((struct config_real *) gconf)->variable;
			break;
		case PGC_STRING:
			/* we assume stringval is NULL if not valid */
			set_string_field((struct config_string *) gconf,
							 &(val->stringval),
							 *((struct config_string *) gconf)->variable);
			break;
		case PGC_ENUM:
			val->enumval = 
				*((struct config_enum *) gconf)->variable;
			break;
	}
}

/*
 * Support for discarding a no-longer-needed value in a stack entry
 */
static void
discard_stack_value(struct config_generic * gconf, union config_var_value * val)
{
	switch (gconf->vartype)
	{
		case PGC_BOOL:
		case PGC_INT:
		case PGC_REAL:
		case PGC_ENUM:
			/* no need to do anything */
			break;
		case PGC_STRING:
			set_string_field((struct config_string *) gconf,
							 &(val->stringval),
							 NULL);
			break;
	}
}


/*
 * Fetch the sorted array pointer (exported for help_config.c's use ONLY)
 */
struct config_generic **
get_guc_variables(void)
{
	return guc_variables;
}


/*
 * Build the sorted array.	This is split out so that it could be
 * re-executed after startup (eg, we could allow loadable modules to
 * add vars, and then we'd need to re-sort).
 */
void
build_guc_variables(void)
{
	int			size_vars;
	int			num_vars = 0;
	struct config_generic **guc_vars;
	int			i;

	for (i = 0; ConfigureNamesBool[i].gen.name; i++)
	{
		struct config_bool *conf = &ConfigureNamesBool[i];

		/* Rather than requiring vartype to be filled in by hand, do this: */
		conf->gen.vartype = PGC_BOOL;
		num_vars++;
	}

	for (i = 0; ConfigureNamesInt[i].gen.name; i++)
	{
		struct config_int *conf = &ConfigureNamesInt[i];

		conf->gen.vartype = PGC_INT;
		num_vars++;
	}

	for (i = 0; ConfigureNamesReal[i].gen.name; i++)
	{
		struct config_real *conf = &ConfigureNamesReal[i];

		conf->gen.vartype = PGC_REAL;
		num_vars++;
	}

	for (i = 0; ConfigureNamesString[i].gen.name; i++)
	{
		struct config_string *conf = &ConfigureNamesString[i];

		conf->gen.vartype = PGC_STRING;
		num_vars++;
	}

	for (i = 0; ConfigureNamesEnum[i].gen.name; i++)
	{
		struct config_enum *conf = &ConfigureNamesEnum[i];

		conf->gen.vartype = PGC_ENUM;
		num_vars++;
	}

	/*
	 * Create table with 20% slack
	 */
	size_vars = num_vars + num_vars / 4;

	guc_vars = (struct config_generic **)
		guc_malloc(FATAL, size_vars * sizeof(struct config_generic *));

	num_vars = 0;

	for (i = 0; ConfigureNamesBool[i].gen.name; i++)
		guc_vars[num_vars++] = &ConfigureNamesBool[i].gen;

	for (i = 0; ConfigureNamesInt[i].gen.name; i++)
		guc_vars[num_vars++] = &ConfigureNamesInt[i].gen;

	for (i = 0; ConfigureNamesReal[i].gen.name; i++)
		guc_vars[num_vars++] = &ConfigureNamesReal[i].gen;

	for (i = 0; ConfigureNamesString[i].gen.name; i++)
		guc_vars[num_vars++] = &ConfigureNamesString[i].gen;

	for (i = 0; ConfigureNamesEnum[i].gen.name; i++)
		guc_vars[num_vars++] = &ConfigureNamesEnum[i].gen;

	if (guc_variables)
		free(guc_variables);
	guc_variables = guc_vars;
	num_guc_variables = num_vars;
	size_guc_variables = size_vars;
	qsort((void *) guc_variables, num_guc_variables,
		  sizeof(struct config_generic *), guc_var_compare);
}

/*
 * Add a new GUC variable to the list of known variables. The
 * list is expanded if needed.
 */
static bool
add_guc_variable(struct config_generic * var, int elevel)
{
	if (num_guc_variables + 1 >= size_guc_variables)
	{
		/*
		 * Increase the vector by 25%
		 */
		int			size_vars = size_guc_variables + size_guc_variables / 4;
		struct config_generic **guc_vars;

		if (size_vars == 0)
		{
			size_vars = 100;
			guc_vars = (struct config_generic **)
				guc_malloc(elevel, size_vars * sizeof(struct config_generic *));
		}
		else
		{
			guc_vars = (struct config_generic **)
				guc_realloc(elevel, guc_variables, size_vars * sizeof(struct config_generic *));
		}

		if (guc_vars == NULL)
			return false;		/* out of memory */

		guc_variables = guc_vars;
		size_guc_variables = size_vars;
	}
	guc_variables[num_guc_variables++] = var;
	qsort((void *) guc_variables, num_guc_variables,
		  sizeof(struct config_generic *), guc_var_compare);
	return true;
}

/*
 * Create and add a placeholder variable. It's presumed to belong
 * to a valid custom variable class at this point.
 */
static struct config_generic *
add_placeholder_variable(const char *name, int elevel)
{
	size_t		sz = sizeof(struct config_string) + sizeof(char *);
	struct config_string *var;
	struct config_generic *gen;

	var = (struct config_string *) guc_malloc(elevel, sz);
	if (var == NULL)
		return NULL;
	memset(var, 0, sz);
	gen = &var->gen;

	gen->name = guc_strdup(elevel, name);
	if (gen->name == NULL)
	{
		free(var);
		return NULL;
	}

	gen->context = PGC_USERSET;
	gen->group = CUSTOM_OPTIONS;
	gen->short_desc = "GUC placeholder variable";
	gen->flags = GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | GUC_CUSTOM_PLACEHOLDER;
	gen->vartype = PGC_STRING;

	/*
	 * The char* is allocated at the end of the struct since we have no
	 * 'static' place to point to.	Note that the current value, as well as
	 * the boot and reset values, start out NULL.
	 */
	var->variable = (char **) (var + 1);

	if (!add_guc_variable((struct config_generic *) var, elevel))
	{
		free((void *) gen->name);
		free(var);
		return NULL;
	}

	return gen;
}

/*
 * Detect whether the portion of "name" before dotPos matches any custom
 * variable class name listed in custom_var_classes.  The latter must be
 * formatted the way that assign_custom_variable_classes does it, ie,
 * no whitespace.  NULL is valid for custom_var_classes.
 */
static bool
is_custom_class(const char *name, int dotPos, const char *custom_var_classes)
{
	bool		result = false;
	const char *ccs = custom_var_classes;

	if (ccs != NULL)
	{
		const char *start = ccs;

		for (;; ++ccs)
		{
			char		c = *ccs;

			if (c == '\0' || c == ',')
			{
				if (dotPos == ccs - start && strncmp(start, name, dotPos) == 0)
				{
					result = true;
					break;
				}
				if (c == '\0')
					break;
				start = ccs + 1;
			}
		}
	}
	return result;
}

/*
 * Look up option NAME.  If it exists, return a pointer to its record,
 * else return NULL.  If create_placeholders is TRUE, we'll create a
 * placeholder record for a valid-looking custom variable name.
 */
static struct config_generic *
find_option(const char *name, bool create_placeholders, int elevel)
{
	const char **key = &name;
	struct config_generic **res;
	int			i;

	Assert(name);

	/*
	 * By equating const char ** with struct config_generic *, we are assuming
	 * the name field is first in config_generic.
	 */
	res = (struct config_generic **) bsearch((void *) &key,
											 (void *) guc_variables,
											 num_guc_variables,
											 sizeof(struct config_generic *),
											 guc_var_compare);
	if (res)
		return *res;

	/*
	 * See if the name is an obsolete name for a variable.	We assume that the
	 * set of supported old names is short enough that a brute-force search is
	 * the best way.
	 */
	for (i = 0; map_old_guc_names[i] != NULL; i += 2)
	{
		if (guc_name_compare(name, map_old_guc_names[i]) == 0)
			return find_option(map_old_guc_names[i + 1], false, elevel);
	}

	if (create_placeholders)
	{
		/*
		 * Check if the name is qualified, and if so, check if the qualifier
		 * matches any custom variable class.  If so, add a placeholder.
		 */
		const char *dot = strchr(name, GUC_QUALIFIER_SEPARATOR);

		if (dot != NULL &&
			is_custom_class(name, dot - name, custom_variable_classes))
			return add_placeholder_variable(name, elevel);
	}

	/* Unknown name */
	return NULL;
}


/*
 * comparator for qsorting and bsearching guc_variables array
 */
static int
guc_var_compare(const void *a, const void *b)
{
	struct config_generic *confa = *(struct config_generic **) a;
	struct config_generic *confb = *(struct config_generic **) b;

	return guc_name_compare(confa->name, confb->name);
}

/*
 * the bare comparison function for GUC names
 */
static int
guc_name_compare(const char *namea, const char *nameb)
{
	/*
	 * The temptation to use strcasecmp() here must be resisted, because the
	 * array ordering has to remain stable across setlocale() calls. So, build
	 * our own with a simple ASCII-only downcasing.
	 */
	while (*namea && *nameb)
	{
		char		cha = *namea++;
		char		chb = *nameb++;

		if (cha >= 'A' && cha <= 'Z')
			cha += 'a' - 'A';
		if (chb >= 'A' && chb <= 'Z')
			chb += 'a' - 'A';
		if (cha != chb)
			return cha - chb;
	}
	if (*namea)
		return 1;				/* a is longer */
	if (*nameb)
		return -1;				/* b is longer */
	return 0;
}


/*
 * Initialize GUC options during program startup.
 *
 * Note that we cannot read the config file yet, since we have not yet
 * processed command-line switches.
 */
void
InitializeGUCOptions(void)
{
	int			i;
	char	   *env;
	long		stack_rlimit;

	/*
	 * Before log_line_prefix could possibly receive a nonempty setting, make
	 * sure that timezone processing is minimally alive (see elog.c).
	 */
	pg_timezone_pre_initialize();

	/*
	 * Build sorted array of all GUC variables.
	 */
	build_guc_variables();

	/*
	 * Load all variables with their compiled-in defaults, and initialize
	 * status fields as needed.
	 */
	for (i = 0; i < num_guc_variables; i++)
	{
		InitializeOneGUCOption(guc_variables[i]);
	}

	guc_dirty = false;

	reporting_enabled = false;

	/*
	 * Prevent any attempt to override the transaction modes from
	 * non-interactive sources.
	 */
	SetConfigOption("transaction_isolation", "default",
					PGC_POSTMASTER, PGC_S_OVERRIDE);
	SetConfigOption("transaction_read_only", "no",
					PGC_POSTMASTER, PGC_S_OVERRIDE);

	/*
	 * For historical reasons, some GUC parameters can receive defaults from
	 * environment variables.  Process those settings.	NB: if you add or
	 * remove anything here, see also ProcessConfigFile().
	 */

	env = getenv("PGPORT");
	if (env != NULL)
		SetConfigOption("port", env, PGC_POSTMASTER, PGC_S_ENV_VAR);

	env = getenv("PGDATESTYLE");
	if (env != NULL)
		SetConfigOption("datestyle", env, PGC_POSTMASTER, PGC_S_ENV_VAR);

	env = getenv("PGCLIENTENCODING");
	if (env != NULL)
		SetConfigOption("client_encoding", env, PGC_POSTMASTER, PGC_S_ENV_VAR);

	/*
	 * rlimit isn't exactly an "environment variable", but it behaves about
	 * the same.  If we can identify the platform stack depth rlimit, increase
	 * default stack depth setting up to whatever is safe (but at most 2MB).
	 */
	stack_rlimit = get_stack_depth_rlimit();
	if (stack_rlimit > 0)
	{
		int			new_limit = (stack_rlimit - STACK_DEPTH_SLOP) / 1024L;

		if (new_limit > 100)
		{
			char		limbuf[16];

			new_limit = Min(new_limit, 2048);
			sprintf(limbuf, "%d", new_limit);
			SetConfigOption("max_stack_depth", limbuf,
							PGC_POSTMASTER, PGC_S_ENV_VAR);
		}
	}
}

/*
 * Initialize one GUC option variable to its compiled-in default.
 */
static void
InitializeOneGUCOption(struct config_generic *gconf)
{
	gconf->status = 0;
	gconf->reset_source = PGC_S_DEFAULT;
	gconf->source = PGC_S_DEFAULT;
	gconf->stack = NULL;
	gconf->sourcefile = NULL;
	gconf->sourceline = 0;

	switch (gconf->vartype)
	{
		case PGC_BOOL:
		{
			struct config_bool *conf = (struct config_bool *) gconf;

			if (conf->assign_hook)
				if (!(*conf->assign_hook) (conf->boot_val, true,
										   PGC_S_DEFAULT))
					elog(FATAL, "failed to initialize %s to %d",
						 conf->gen.name, (int) conf->boot_val);
			*conf->variable = conf->reset_val = conf->boot_val;
			break;
		}
		case PGC_INT:
		{
			struct config_int *conf = (struct config_int *) gconf;

			Assert(conf->boot_val >= conf->min);
			Assert(conf->boot_val <= conf->max);
			if (conf->assign_hook)
				if (!(*conf->assign_hook) (conf->boot_val, true,
										   PGC_S_DEFAULT))
					elog(FATAL, "failed to initialize %s to %d",
						 conf->gen.name, conf->boot_val);
			*conf->variable = conf->reset_val = conf->boot_val;
			break;
		}
		case PGC_REAL:
		{
			struct config_real *conf = (struct config_real *) gconf;

			Assert(conf->boot_val >= conf->min);
			Assert(conf->boot_val <= conf->max);
			if (conf->assign_hook)
				if (!(*conf->assign_hook) (conf->boot_val, true,
										   PGC_S_DEFAULT))
					elog(FATAL, "failed to initialize %s to %g",
						 conf->gen.name, conf->boot_val);
			*conf->variable = conf->reset_val = conf->boot_val;
			break;
		}
		case PGC_STRING:
		{
			struct config_string *conf = (struct config_string *) gconf;
			char	   *str;

			*conf->variable = NULL;
			conf->reset_val = NULL;

			if (conf->boot_val == NULL)
			{
				/* leave the value NULL, do not call assign hook */
				break;
			}

			str = guc_strdup(FATAL, conf->boot_val);
			conf->reset_val = str;

			if (conf->assign_hook)
			{
				const char *newstr;

				newstr = (*conf->assign_hook) (str, true,
											   PGC_S_DEFAULT);
				if (newstr == NULL)
				{
					elog(FATAL, "failed to initialize %s to \"%s\"",
						 conf->gen.name, str);
				}
				else if (newstr != str)
				{
					free(str);

					/*
					 * See notes in set_config_option about casting
					 */
					str = (char *) newstr;
					conf->reset_val = str;
				}
			}
			*conf->variable = str;
			break;
		}
		case PGC_ENUM:
		{
			struct config_enum *conf = (struct config_enum *) gconf;

			if (conf->assign_hook)
				if (!(*conf->assign_hook) (conf->boot_val, true,
										   PGC_S_DEFAULT))
					elog(FATAL, "failed to initialize %s to %s",
						 conf->gen.name, 
						 config_enum_lookup_by_value(conf, conf->boot_val));
			*conf->variable = conf->reset_val = conf->boot_val;
			break;
		}
	}
}


/*
 * Select the configuration files and data directory to be used, and
 * do the initial read of postgresql.conf.
 *
 * This is called after processing command-line switches.
 *		userDoption is the -D switch value if any (NULL if unspecified).
 *		progname is just for use in error messages.
 *
 * Returns true on success; on failure, prints a suitable error message
 * to stderr and returns false.
 */
bool
SelectConfigFiles(const char *userDoption, const char *progname)
{
	char	   *configdir;
	char	   *fname;
	struct stat stat_buf;

	/* configdir is -D option, or $PGDATA if no -D */
	if (userDoption)
		configdir = make_absolute_path(userDoption);
	else
		configdir = make_absolute_path(getenv("PGDATA"));

	/*
	 * Find the configuration file: if config_file was specified on the
	 * command line, use it, else use configdir/postgresql.conf.  In any case
	 * ensure the result is an absolute path, so that it will be interpreted
	 * the same way by future backends.
	 */
	if (ConfigFileName)
		fname = make_absolute_path(ConfigFileName);
	else if (configdir)
	{
		fname = guc_malloc(FATAL,
						   strlen(configdir) + strlen(CONFIG_FILENAME) + 2);
		sprintf(fname, "%s/%s", configdir, CONFIG_FILENAME);
	}
	else
	{
		write_stderr("%s does not know where to find the server configuration file.\n"
					 "You must specify the --config-file or -D invocation "
					 "option or set the PGDATA environment variable.\n",
					 progname);
		return false;
	}

	/*
	 * Set the ConfigFileName GUC variable to its final value, ensuring that
	 * it can't be overridden later.
	 */
	SetConfigOption("config_file", fname, PGC_POSTMASTER, PGC_S_OVERRIDE);
	free(fname);

	/*
	 * Now read the config file for the first time.
	 */
	if (stat(ConfigFileName, &stat_buf) != 0)
	{
		write_stderr("%s cannot access the server configuration file \"%s\": %s\n",
					 progname, ConfigFileName, strerror(errno));
		return false;
	}

	ProcessConfigFile(PGC_POSTMASTER);

	/*
	 * If the data_directory GUC variable has been set, use that as DataDir;
	 * otherwise use configdir if set; else punt.
	 *
	 * Note: SetDataDir will copy and absolute-ize its argument, so we don't
	 * have to.
	 */
	if (data_directory)
		SetDataDir(data_directory);
	else if (configdir)
		SetDataDir(configdir);
	else
	{
		write_stderr("%s does not know where to find the database system data.\n"
					 "This can be specified as \"data_directory\" in \"%s\", "
					 "or by the -D invocation option, or by the "
					 "PGDATA environment variable.\n",
					 progname, ConfigFileName);
		return false;
	}

	/*
	 * Reflect the final DataDir value back into the data_directory GUC var.
	 * (If you are wondering why we don't just make them a single variable,
	 * it's because the EXEC_BACKEND case needs DataDir to be transmitted to
	 * child backends specially.  XXX is that still true?  Given that we now
	 * chdir to DataDir, EXEC_BACKEND can read the config file without knowing
	 * DataDir in advance.)
	 */
	SetConfigOption("data_directory", DataDir, PGC_POSTMASTER, PGC_S_OVERRIDE);

	/*
	 * Figure out where pg_hba.conf is, and make sure the path is absolute.
	 */
	if (HbaFileName)
		fname = make_absolute_path(HbaFileName);
	else if (configdir)
	{
		fname = guc_malloc(FATAL,
						   strlen(configdir) + strlen(HBA_FILENAME) + 2);
		sprintf(fname, "%s/%s", configdir, HBA_FILENAME);
	}
	else
	{
		write_stderr("%s does not know where to find the \"hba\" configuration file.\n"
					 "This can be specified as \"hba_file\" in \"%s\", "
					 "or by the -D invocation option, or by the "
					 "PGDATA environment variable.\n",
					 progname, ConfigFileName);
		return false;
	}
	SetConfigOption("hba_file", fname, PGC_POSTMASTER, PGC_S_OVERRIDE);
	free(fname);

	/*
	 * Likewise for pg_ident.conf.
	 */
	if (IdentFileName)
		fname = make_absolute_path(IdentFileName);
	else if (configdir)
	{
		fname = guc_malloc(FATAL,
						   strlen(configdir) + strlen(IDENT_FILENAME) + 2);
		sprintf(fname, "%s/%s", configdir, IDENT_FILENAME);
	}
	else
	{
		write_stderr("%s does not know where to find the \"ident\" configuration file.\n"
					 "This can be specified as \"ident_file\" in \"%s\", "
					 "or by the -D invocation option, or by the "
					 "PGDATA environment variable.\n",
					 progname, ConfigFileName);
		return false;
	}
	SetConfigOption("ident_file", fname, PGC_POSTMASTER, PGC_S_OVERRIDE);
	free(fname);

	free(configdir);

	return true;
}


/*
 * Reset all options to their saved default values (implements RESET ALL)
 */
void
ResetAllOptions(void)
{
	int			i;

	for (i = 0; i < num_guc_variables; i++)
	{
		struct config_generic *gconf = guc_variables[i];

		/* Don't reset non-SET-able values */
		if (gconf->context != PGC_SUSET &&
			gconf->context != PGC_USERSET)
			continue;
		/* Don't reset if special exclusion from RESET ALL */
		if (gconf->flags & GUC_NO_RESET_ALL)
			continue;
		/* No need to reset if wasn't SET */
		if (gconf->source <= PGC_S_OVERRIDE)
			continue;

		/* Save old value to support transaction abort */
		push_old_value(gconf, GUC_ACTION_SET);

		switch (gconf->vartype)
		{
			case PGC_BOOL:
				{
					struct config_bool *conf = (struct config_bool *) gconf;

					if (conf->assign_hook)
						if (!(*conf->assign_hook) (conf->reset_val, true,
												   PGC_S_SESSION))
							elog(ERROR, "failed to reset %s to %d",
								 conf->gen.name, (int) conf->reset_val);
					*conf->variable = conf->reset_val;
					break;
				}
			case PGC_INT:
				{
					struct config_int *conf = (struct config_int *) gconf;

					if (conf->assign_hook)
						if (!(*conf->assign_hook) (conf->reset_val, true,
												   PGC_S_SESSION))
							elog(ERROR, "failed to reset %s to %d",
								 conf->gen.name, conf->reset_val);
					*conf->variable = conf->reset_val;
					break;
				}
			case PGC_REAL:
				{
					struct config_real *conf = (struct config_real *) gconf;

					if (conf->assign_hook)
						if (!(*conf->assign_hook) (conf->reset_val, true,
												   PGC_S_SESSION))
							elog(ERROR, "failed to reset %s to %g",
								 conf->gen.name, conf->reset_val);
					*conf->variable = conf->reset_val;
					break;
				}
			case PGC_STRING:
				{
					struct config_string *conf = (struct config_string *) gconf;
					char	   *str;

					/* We need not strdup here */
					str = conf->reset_val;

					if (conf->assign_hook && str)
					{
						const char *newstr;

						newstr = (*conf->assign_hook) (str, true,
													   PGC_S_SESSION);
						if (newstr == NULL)
							elog(ERROR, "failed to reset %s to \"%s\"",
								 conf->gen.name, str);
						else if (newstr != str)
						{
							/*
							 * See notes in set_config_option about casting
							 */
							str = (char *) newstr;
						}
					}

					set_string_field(conf, conf->variable, str);
					break;
				}
			case PGC_ENUM:
				{
					struct config_enum *conf = (struct config_enum *) gconf;

					if (conf->assign_hook)
						if (!(*conf->assign_hook) (conf->reset_val, true,
												   PGC_S_SESSION))
							elog(ERROR, "failed to reset %s to %s",
								 conf->gen.name,
								 config_enum_lookup_by_value(conf, conf->reset_val));
					*conf->variable = conf->reset_val;
					break;
				}
		}

		gconf->source = gconf->reset_source;

		if (gconf->flags & GUC_REPORT)
			ReportGUCOption(gconf);
	}
}


/*
 * push_old_value
 *		Push previous state during transactional assignment to a GUC variable.
 */
static void
push_old_value(struct config_generic * gconf, GucAction action)
{
	GucStack   *stack;

	/* If we're not inside a nest level, do nothing */
	if (GUCNestLevel == 0)
		return;

	/* Do we already have a stack entry of the current nest level? */
	stack = gconf->stack;
	if (stack && stack->nest_level >= GUCNestLevel)
	{
		/* Yes, so adjust its state if necessary */
		Assert(stack->nest_level == GUCNestLevel);
		switch (action)
		{
			case GUC_ACTION_SET:
				/* SET overrides any prior action at same nest level */
				if (stack->state == GUC_SET_LOCAL)
				{
					/* must discard old masked value */
					discard_stack_value(gconf, &stack->masked);
				}
				stack->state = GUC_SET;
				break;
			case GUC_ACTION_LOCAL:
				if (stack->state == GUC_SET)
				{
					/* SET followed by SET LOCAL, remember SET's value */
					set_stack_value(gconf, &stack->masked);
					stack->state = GUC_SET_LOCAL;
				}
				/* in all other cases, no change to stack entry */
				break;
			case GUC_ACTION_SAVE:
				/* Could only have a prior SAVE of same variable */
				Assert(stack->state == GUC_SAVE);
				break;
		}
		Assert(guc_dirty);		/* must be set already */
		return;
	}

	/*
	 * Push a new stack entry
	 *
	 * We keep all the stack entries in TopTransactionContext for simplicity.
	 */
	stack = (GucStack *) MemoryContextAllocZero(TopTransactionContext,
												sizeof(GucStack));

	stack->prev = gconf->stack;
	stack->nest_level = GUCNestLevel;
	switch (action)
	{
		case GUC_ACTION_SET:
			stack->state = GUC_SET;
			break;
		case GUC_ACTION_LOCAL:
			stack->state = GUC_LOCAL;
			break;
		case GUC_ACTION_SAVE:
			stack->state = GUC_SAVE;
			break;
	}
	stack->source = gconf->source;
	set_stack_value(gconf, &stack->prior);

	gconf->stack = stack;

	/* Ensure we remember to pop at end of xact */
	guc_dirty = true;
}


/*
 * Do GUC processing at main transaction start.
 */
void
AtStart_GUC(void)
{
	/*
	 * The nest level should be 0 between transactions; if it isn't, somebody
	 * didn't call AtEOXact_GUC, or called it with the wrong nestLevel.  We
	 * throw a warning but make no other effort to clean up.
	 */
	if (GUCNestLevel != 0)
		elog(WARNING, "GUC nest level = %d at transaction start",
			 GUCNestLevel);
	GUCNestLevel = 1;
}

/*
 * Enter a new nesting level for GUC values.  This is called at subtransaction
 * start and when entering a function that has proconfig settings.	NOTE that
 * we must not risk error here, else subtransaction start will be unhappy.
 */
int
NewGUCNestLevel(void)
{
	return ++GUCNestLevel;
}

/*
 * Do GUC processing at transaction or subtransaction commit or abort, or
 * when exiting a function that has proconfig settings.  (The name is thus
 * a bit of a misnomer; perhaps it should be ExitGUCNestLevel or some such.)
 * During abort, we discard all GUC settings that were applied at nesting
 * levels >= nestLevel.  nestLevel == 1 corresponds to the main transaction.
 */
void
AtEOXact_GUC(bool isCommit, int nestLevel)
{
	bool		still_dirty;
	int			i;

	Assert(nestLevel > 0 && nestLevel <= GUCNestLevel);

	/* Quick exit if nothing's changed in this transaction */
	if (!guc_dirty)
	{
		GUCNestLevel = nestLevel - 1;
		return;
	}

	still_dirty = false;
	for (i = 0; i < num_guc_variables; i++)
	{
		struct config_generic *gconf = guc_variables[i];
		GucStack   *stack;

		/*
		 * Process and pop each stack entry within the nest level.	To
		 * simplify fmgr_security_definer(), we allow failure exit from a
		 * function-with-SET-options to be recovered at the surrounding
		 * transaction or subtransaction abort; so there could be more than
		 * one stack entry to pop.
		 */
		while ((stack = gconf->stack) != NULL &&
			   stack->nest_level >= nestLevel)
		{
			GucStack   *prev = stack->prev;
			bool		restorePrior = false;
			bool		restoreMasked = false;
			bool		changed;

			/*
			 * In this next bit, if we don't set either restorePrior or
			 * restoreMasked, we must "discard" any unwanted fields of the
			 * stack entries to avoid leaking memory.  If we do set one of
			 * those flags, unused fields will be cleaned up after restoring.
			 */
			if (!isCommit)		/* if abort, always restore prior value */
				restorePrior = true;
			else if (stack->state == GUC_SAVE)
				restorePrior = true;
			else if (stack->nest_level == 1)
			{
				/* transaction commit */
				if (stack->state == GUC_SET_LOCAL)
					restoreMasked = true;
				else if (stack->state == GUC_SET)
				{
					/* we keep the current active value */
					discard_stack_value(gconf, &stack->prior);
				}
				else	/* must be GUC_LOCAL */
					restorePrior = true;
			}
			else if (prev == NULL ||
					 prev->nest_level < stack->nest_level - 1)
			{
				/* decrement entry's level and do not pop it */
				stack->nest_level--;
				continue;
			}
			else
			{
				/*
				 * We have to merge this stack entry into prev. See README for
				 * discussion of this bit.
				 */
				switch (stack->state)
				{
					case GUC_SAVE:
						Assert(false);	/* can't get here */

					case GUC_SET:
						/* next level always becomes SET */
						discard_stack_value(gconf, &stack->prior);
						if (prev->state == GUC_SET_LOCAL)
							discard_stack_value(gconf, &prev->masked);
						prev->state = GUC_SET;
						break;

					case GUC_LOCAL:
						if (prev->state == GUC_SET)
						{
							/* LOCAL migrates down */
							prev->masked = stack->prior;
							prev->state = GUC_SET_LOCAL;
						}
						else
						{
							/* else just forget this stack level */
							discard_stack_value(gconf, &stack->prior);
						}
						break;

					case GUC_SET_LOCAL:
						/* prior state at this level no longer wanted */
						discard_stack_value(gconf, &stack->prior);
						/* copy down the masked state */
						if (prev->state == GUC_SET_LOCAL)
							discard_stack_value(gconf, &prev->masked);
						prev->masked = stack->masked;
						prev->state = GUC_SET_LOCAL;
						break;
				}
			}

			changed = false;

			if (restorePrior || restoreMasked)
			{
				/* Perform appropriate restoration of the stacked value */
				union config_var_value newvalue;
				GucSource	newsource;

				if (restoreMasked)
				{
					newvalue = stack->masked;
					newsource = PGC_S_SESSION;
				}
				else
				{
					newvalue = stack->prior;
					newsource = stack->source;
				}

				switch (gconf->vartype)
				{
					case PGC_BOOL:
						{
							struct config_bool *conf = (struct config_bool *) gconf;
							bool		newval = newvalue.boolval;

							if (*conf->variable != newval)
							{
								if (conf->assign_hook)
									if (!(*conf->assign_hook) (newval,
													   true, PGC_S_OVERRIDE))
										elog(LOG, "failed to commit %s as %d",
											 conf->gen.name, (int) newval);
								*conf->variable = newval;
								changed = true;
							}
							break;
						}
					case PGC_INT:
						{
							struct config_int *conf = (struct config_int *) gconf;
							int			newval = newvalue.intval;

							if (*conf->variable != newval)
							{
								if (conf->assign_hook)
									if (!(*conf->assign_hook) (newval,
													   true, PGC_S_OVERRIDE))
										elog(LOG, "failed to commit %s as %d",
											 conf->gen.name, newval);
								*conf->variable = newval;
								changed = true;
							}
							break;
						}
					case PGC_REAL:
						{
							struct config_real *conf = (struct config_real *) gconf;
							double		newval = newvalue.realval;

							if (*conf->variable != newval)
							{
								if (conf->assign_hook)
									if (!(*conf->assign_hook) (newval,
													   true, PGC_S_OVERRIDE))
										elog(LOG, "failed to commit %s as %g",
											 conf->gen.name, newval);
								*conf->variable = newval;
								changed = true;
							}
							break;
						}
					case PGC_STRING:
						{
							struct config_string *conf = (struct config_string *) gconf;
							char	   *newval = newvalue.stringval;

							if (*conf->variable != newval)
							{
								if (conf->assign_hook && newval)
								{
									const char *newstr;

									newstr = (*conf->assign_hook) (newval, true,
															 PGC_S_OVERRIDE);
									if (newstr == NULL)
										elog(LOG, "failed to commit %s as \"%s\"",
											 conf->gen.name, newval);
									else if (newstr != newval)
									{
										/*
										 * If newval should now be freed,
										 * it'll be taken care of below.
										 *
										 * See notes in set_config_option
										 * about casting
										 */
										newval = (char *) newstr;
									}
								}

								set_string_field(conf, conf->variable, newval);
								changed = true;
							}

							/*
							 * Release stacked values if not used anymore. We
							 * could use discard_stack_value() here, but since
							 * we have type-specific code anyway, might as
							 * well inline it.
							 */
							set_string_field(conf, &stack->prior.stringval, NULL);
							set_string_field(conf, &stack->masked.stringval, NULL);
							break;
						}
					case PGC_ENUM:
						{
							struct config_enum *conf = (struct config_enum *) gconf;
							int newval = newvalue.enumval;

							if (*conf->variable != newval)
							{
								if (conf->assign_hook)
									if (!(*conf->assign_hook) (newval,
															   true, PGC_S_OVERRIDE))
										elog(LOG, "failed to commit %s as %s",
											 conf->gen.name,
											 config_enum_lookup_by_value(conf, newval));
								*conf->variable = newval;
								changed = true;
							}
							break;
						}
				}

				gconf->source = newsource;
			}

			/* Finish popping the state stack */
			gconf->stack = prev;
			pfree(stack);

			/* Report new value if we changed it */
			if (changed && (gconf->flags & GUC_REPORT))
				ReportGUCOption(gconf);
		}						/* end of stack-popping loop */

		if (stack != NULL)
			still_dirty = true;
	}

	/* If there are no remaining stack entries, we can reset guc_dirty */
	guc_dirty = still_dirty;

	/* Update nesting level */
	GUCNestLevel = nestLevel - 1;
}


/*
 * Start up automatic reporting of changes to variables marked GUC_REPORT.
 * This is executed at completion of backend startup.
 */
void
BeginReportingGUCOptions(void)
{
	int			i;

	/*
	 * Don't do anything unless talking to an interactive frontend of protocol
	 * 3.0 or later.
	 */
	if (whereToSendOutput != DestRemote ||
		PG_PROTOCOL_MAJOR(FrontendProtocol) < 3)
		return;

	reporting_enabled = true;

	/* Transmit initial values of interesting variables */
	for (i = 0; i < num_guc_variables; i++)
	{
		struct config_generic *conf = guc_variables[i];

		if (conf->flags & GUC_REPORT)
			ReportGUCOption(conf);
	}
}

/*
 * ReportGUCOption: if appropriate, transmit option value to frontend
 */
static void
ReportGUCOption(struct config_generic * record)
{
	if (reporting_enabled && (record->flags & GUC_REPORT))
	{
		char	   *val = _ShowOption(record, false);
		StringInfoData msgbuf;

		pq_beginmessage(&msgbuf, 'S');
		pq_sendstring(&msgbuf, record->name);
		pq_sendstring(&msgbuf, val);
		pq_endmessage(&msgbuf);

		pfree(val);
	}
}

/*
 * Try to parse value as an integer.  The accepted formats are the
 * usual decimal, octal, or hexadecimal formats, optionally followed by
 * a unit name if "flags" indicates a unit is allowed.
 *
 * If the string parses okay, return true, else false.
 * If okay and result is not NULL, return the value in *result.
 * If not okay and hintmsg is not NULL, *hintmsg is set to a suitable
 *	HINT message, or NULL if no hint provided.
 */
bool
parse_int(const char *value, int *result, int flags, const char **hintmsg)
{
	int64		val;
	char	   *endptr;

	/* To suppress compiler warnings, always set output params */
	if (result)
		*result = 0;
	if (hintmsg)
		*hintmsg = NULL;

	/* We assume here that int64 is at least as wide as long */
	errno = 0;
	val = strtol(value, &endptr, 0);

	if (endptr == value)
		return false;			/* no HINT for integer syntax error */

	if (errno == ERANGE || val != (int64) ((int32) val))
	{
		if (hintmsg)
			*hintmsg = gettext_noop("Value exceeds integer range.");
		return false;
	}

	/* allow whitespace between integer and unit */
	while (isspace((unsigned char) *endptr))
		endptr++;

	/* Handle possible unit */
	if (*endptr != '\0')
	{
		/*
		 * Note: the multiple-switch coding technique here is a bit tedious,
		 * but seems necessary to avoid intermediate-value overflows.
		 *
		 * If INT64_IS_BUSTED (ie, it's really int32) we will fail to detect
		 * overflow due to units conversion, but there are few enough such
		 * machines that it does not seem worth trying to be smarter.
		 */
		if (flags & GUC_UNIT_MEMORY)
		{
			/* Set hint for use if no match or trailing garbage */
			if (hintmsg)
				*hintmsg = gettext_noop("Valid units for this parameter are \"kB\", \"MB\", and \"GB\".");

#if BLCKSZ < 1024 || BLCKSZ > (1024*1024)
#error BLCKSZ must be between 1KB and 1MB
#endif
#if XLOG_BLCKSZ < 1024 || XLOG_BLCKSZ > (1024*1024)
#error XLOG_BLCKSZ must be between 1KB and 1MB
#endif

			if (strncmp(endptr, "kB", 2) == 0)
			{
				endptr += 2;
				switch (flags & GUC_UNIT_MEMORY)
				{
					case GUC_UNIT_BLOCKS:
						val /= (BLCKSZ / 1024);
						break;
					case GUC_UNIT_XBLOCKS:
						val /= (XLOG_BLCKSZ / 1024);
						break;
				}
			}
			else if (strncmp(endptr, "MB", 2) == 0)
			{
				endptr += 2;
				switch (flags & GUC_UNIT_MEMORY)
				{
					case GUC_UNIT_KB:
						val *= KB_PER_MB;
						break;
					case GUC_UNIT_BLOCKS:
						val *= KB_PER_MB / (BLCKSZ / 1024);
						break;
					case GUC_UNIT_XBLOCKS:
						val *= KB_PER_MB / (XLOG_BLCKSZ / 1024);
						break;
				}
			}
			else if (strncmp(endptr, "GB", 2) == 0)
			{
				endptr += 2;
				switch (flags & GUC_UNIT_MEMORY)
				{
					case GUC_UNIT_KB:
						val *= KB_PER_GB;
						break;
					case GUC_UNIT_BLOCKS:
						val *= KB_PER_GB / (BLCKSZ / 1024);
						break;
					case GUC_UNIT_XBLOCKS:
						val *= KB_PER_GB / (XLOG_BLCKSZ / 1024);
						break;
				}
			}
		}
		else if (flags & GUC_UNIT_TIME)
		{
			/* Set hint for use if no match or trailing garbage */
			if (hintmsg)
				*hintmsg = gettext_noop("Valid units for this parameter are \"ms\", \"s\", \"min\", \"h\", and \"d\".");

			if (strncmp(endptr, "ms", 2) == 0)
			{
				endptr += 2;
				switch (flags & GUC_UNIT_TIME)
				{
					case GUC_UNIT_S:
						val /= MS_PER_S;
						break;
					case GUC_UNIT_MIN:
						val /= MS_PER_MIN;
						break;
				}
			}
			else if (strncmp(endptr, "s", 1) == 0)
			{
				endptr += 1;
				switch (flags & GUC_UNIT_TIME)
				{
					case GUC_UNIT_MS:
						val *= MS_PER_S;
						break;
					case GUC_UNIT_MIN:
						val /= S_PER_MIN;
						break;
				}
			}
			else if (strncmp(endptr, "min", 3) == 0)
			{
				endptr += 3;
				switch (flags & GUC_UNIT_TIME)
				{
					case GUC_UNIT_MS:
						val *= MS_PER_MIN;
						break;
					case GUC_UNIT_S:
						val *= S_PER_MIN;
						break;
				}
			}
			else if (strncmp(endptr, "h", 1) == 0)
			{
				endptr += 1;
				switch (flags & GUC_UNIT_TIME)
				{
					case GUC_UNIT_MS:
						val *= MS_PER_H;
						break;
					case GUC_UNIT_S:
						val *= S_PER_H;
						break;
					case GUC_UNIT_MIN:
						val *= MIN_PER_H;
						break;
				}
			}
			else if (strncmp(endptr, "d", 1) == 0)
			{
				endptr += 1;
				switch (flags & GUC_UNIT_TIME)
				{
					case GUC_UNIT_MS:
						val *= MS_PER_D;
						break;
					case GUC_UNIT_S:
						val *= S_PER_D;
						break;
					case GUC_UNIT_MIN:
						val *= MIN_PER_D;
						break;
				}
			}
		}

		/* allow whitespace after unit */
		while (isspace((unsigned char) *endptr))
			endptr++;

		if (*endptr != '\0')
			return false;		/* appropriate hint, if any, already set */

		/* Check for overflow due to units conversion */
		if (val != (int64) ((int32) val))
		{
			if (hintmsg)
				*hintmsg = gettext_noop("Value exceeds integer range.");
			return false;
		}
	}

	if (result)
		*result = (int) val;
	return true;
}



/*
 * Try to parse value as a floating point number in the usual format.
 * If the string parses okay, return true, else false.
 * If okay and result is not NULL, return the value in *result.
 */
bool
parse_real(const char *value, double *result)
{
	double		val;
	char	   *endptr;

	if (result)
		*result = 0;			/* suppress compiler warning */

	errno = 0;
	val = strtod(value, &endptr);
	if (endptr == value || errno == ERANGE)
		return false;

	/* allow whitespace after number */
	while (isspace((unsigned char) *endptr))
		endptr++;
	if (*endptr != '\0')
		return false;

	if (result)
		*result = val;
	return true;
}


/*
 * Lookup the name for an enum option with the selected value.
 * Should only ever be called with known-valid values, so throws
 * an elog(ERROR) if the enum option is not found.
 *
 * The returned string is a pointer to static data and not
 * allocated for modification.
 */
const char *
config_enum_lookup_by_value(struct config_enum *record, int val)
{
	const struct config_enum_entry *entry;

	for (entry = record->options; entry && entry->name; entry++)
	{
		if (entry->val == val)
			return entry->name;
	}

	elog(ERROR, "could not find enum option %d for %s",
		 val, record->gen.name);
	return NULL;		/* silence compiler */
}


/*
 * Lookup the value for an enum option with the selected name
 * (case-insensitive).
 * If the enum option is found, sets the retval value and returns
 * true. If it's not found, return FALSE and retval is set to 0.
 */
bool
config_enum_lookup_by_name(struct config_enum *record, const char *value,
						   int *retval)
{
	const struct config_enum_entry *entry;

	for (entry = record->options; entry && entry->name; entry++)
	{
		if (pg_strcasecmp(value, entry->name) == 0)
		{
			*retval = entry->val;
			return TRUE;
		}
	}

	*retval = 0;
	return FALSE;
}


/*
 * Return a list of all available options for an enum, excluding
 * hidden ones, separated by the given separator.
 * If prefix is non-NULL, it is added before the first enum value.
 * If suffix is non-NULL, it is added to the end of the string.
 */
static char *
config_enum_get_options(struct config_enum *record, const char *prefix,
						const char *suffix, const char *separator)
{
	const struct config_enum_entry *entry;
	StringInfoData	retstr;
	int			seplen;

	initStringInfo(&retstr);
	appendStringInfoString(&retstr, prefix);
	
	seplen = strlen(separator);
	for (entry = record->options; entry && entry->name; entry++)
	{
		if (!entry->hidden)
		{
			appendStringInfoString(&retstr, entry->name);
			appendBinaryStringInfo(&retstr, separator, seplen);
		}
	}

	/*
	 * All the entries may have been hidden, leaving the string empty
	 * if no prefix was given. This indicates a broken GUC setup, since
	 * there is no use for an enum without any values, so we just check
	 * to make sure we don't write to invalid memory instead of actually
	 * trying to do something smart with it.
	 */
	if (retstr.len >= seplen)
	{
		/* Replace final separator */
		retstr.data[retstr.len - seplen] = '\0';
		retstr.len -= seplen;
	}

	appendStringInfoString(&retstr, suffix);

	return retstr.data;
}

/*
 * Call a GucStringAssignHook function, being careful to free the
 * "newval" string if the hook ereports.
 *
 * This is split out of set_config_option just to avoid the "volatile"
 * qualifiers that would otherwise have to be plastered all over.
 */
static const char *
call_string_assign_hook(GucStringAssignHook assign_hook,
						char *newval, bool doit, GucSource source)
{
	const char *result;

	PG_TRY();
	{
		result = (*assign_hook) (newval, doit, source);
	}
	PG_CATCH();
	{
		free(newval);
		PG_RE_THROW();
	}
	PG_END_TRY();

	return result;
}


/*
 * Sets option `name' to given value. The value should be a string
 * which is going to be parsed and converted to the appropriate data
 * type.  The context and source parameters indicate in which context this
 * function is being called so it can apply the access restrictions
 * properly.
 *
 * If value is NULL, set the option to its default value (normally the
 * reset_val, but if source == PGC_S_DEFAULT we instead use the boot_val).
 *
 * action indicates whether to set the value globally in the session, locally
 * to the current top transaction, or just for the duration of a function call.
 *
 * If changeVal is false then don't really set the option but do all
 * the checks to see if it would work.
 *
 * If there is an error (non-existing option, invalid value) then an
 * ereport(ERROR) is thrown *unless* this is called in a context where we
 * don't want to ereport (currently, startup or SIGHUP config file reread).
 * In that case we write a suitable error message via ereport(LOG) and
 * return false. This is working around the deficiencies in the ereport
 * mechanism, so don't blame me.  In all other cases, the function
 * returns true, including cases where the input is valid but we chose
 * not to apply it because of context or source-priority considerations.
 *
 * See also SetConfigOption for an external interface.
 */
bool
set_config_option(const char *name, const char *value,
				  GucContext context, GucSource source,
				  GucAction action, bool changeVal)
{
	struct config_generic *record;
	int			elevel;
	bool		makeDefault;

	if (context == PGC_SIGHUP || source == PGC_S_DEFAULT)
	{
		/*
		 * To avoid cluttering the log, only the postmaster bleats loudly
		 * about problems with the config file.
		 */
		elevel = IsUnderPostmaster ? DEBUG3 : LOG;
	}
	else if (source == PGC_S_DATABASE || source == PGC_S_USER)
		elevel = WARNING;
	else
		elevel = ERROR;

	record = find_option(name, true, elevel);
	if (record == NULL)
	{
		ereport(elevel,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
			   errmsg("unrecognized configuration parameter \"%s\"", name)));
		return false;
	}

	/*
	 * If source is postgresql.conf, mark the found record with
	 * GUC_IS_IN_FILE. This is for the convenience of ProcessConfigFile.  Note
	 * that we do it even if changeVal is false, since ProcessConfigFile wants
	 * the marking to occur during its testing pass.
	 */
	if (source == PGC_S_FILE)
		record->status |= GUC_IS_IN_FILE;

	/*
	 * Check if the option can be set at this time. See guc.h for the precise
	 * rules. Note that we don't want to throw errors if we're in the SIGHUP
	 * context. In that case we just ignore the attempt and return true.
	 */
	switch (record->context)
	{
		case PGC_INTERNAL:
			if (context == PGC_SIGHUP)
				return true;
			if (context != PGC_INTERNAL)
			{
				ereport(elevel,
						(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
						 errmsg("parameter \"%s\" cannot be changed",
								name)));
				return false;
			}
			break;
		case PGC_POSTMASTER:
			if (context == PGC_SIGHUP)
			{
				/*
				 * We are reading a PGC_POSTMASTER var from postgresql.conf.
				 * We can't change the setting, so give a warning if the DBA
				 * tries to change it.	(Throwing an error would be more
				 * consistent, but seems overly rigid.)
				 */
				if (changeVal && !is_newvalue_equal(record, value))
					ereport(elevel,
							(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
							 errmsg("attempted change of parameter \"%s\" ignored",
									name),
							 errdetail("This parameter cannot be changed after server start.")));
				return true;
			}
			if (context != PGC_POSTMASTER)
			{
				ereport(elevel,
						(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
						 errmsg("attempted change of parameter \"%s\" ignored",
								name),
						 errdetail("This parameter cannot be changed after server start.")));
				return false;
			}
			break;
		case PGC_SIGHUP:
			if (context != PGC_SIGHUP && context != PGC_POSTMASTER)
			{
				ereport(elevel,
						(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
						 errmsg("parameter \"%s\" cannot be changed now",
								name)));
				return false;
			}

			/*
			 * Hmm, the idea of the SIGHUP context is "ought to be global, but
			 * can be changed after postmaster start". But there's nothing
			 * that prevents a crafty administrator from sending SIGHUP
			 * signals to individual backends only.
			 */
			break;
		case PGC_BACKEND:
			if (context == PGC_SIGHUP)
			{
				/*
				 * If a PGC_BACKEND parameter is changed in the config file,
				 * we want to accept the new value in the postmaster (whence
				 * it will propagate to subsequently-started backends), but
				 * ignore it in existing backends.	This is a tad klugy, but
				 * necessary because we don't re-read the config file during
				 * backend start.
				 */
				if (IsUnderPostmaster)
					return true;
			}
			else if (context != PGC_BACKEND && context != PGC_POSTMASTER)
			{
				ereport(elevel,
						(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
						 errmsg("parameter \"%s\" cannot be set after connection start",
								name)));
				return false;
			}
			break;
		case PGC_SUSET:
			if (context == PGC_USERSET || context == PGC_BACKEND)
			{
				ereport(elevel,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to set parameter \"%s\"",
								name)));
				return false;
			}
			break;
		case PGC_USERSET:
			/* always okay */
			break;
	}

	/*
	 * Should we set reset/stacked values?	(If so, the behavior is not
	 * transactional.)	This is done either when we get a default value from
	 * the database's/user's/client's default settings or when we reset a
	 * value to its default.
	 */
	makeDefault = changeVal && (source <= PGC_S_OVERRIDE) &&
		((value != NULL) || source == PGC_S_DEFAULT);

	/*
	 * Ignore attempted set if overridden by previously processed setting.
	 * However, if changeVal is false then plow ahead anyway since we are
	 * trying to find out if the value is potentially good, not actually use
	 * it. Also keep going if makeDefault is true, since we may want to set
	 * the reset/stacked values even if we can't set the variable itself.
	 */
	if (record->source > source)
	{
		if (changeVal && !makeDefault)
		{
			elog(DEBUG3, "\"%s\": setting ignored because previous source is higher priority",
				 name);
			return true;
		}
		changeVal = false;
	}

	/*
	 * Evaluate value and set variable.
	 */
	switch (record->vartype)
	{
		case PGC_BOOL:
			{
				struct config_bool *conf = (struct config_bool *) record;
				bool		newval;

				if (value)
				{
					if (!parse_bool(value, &newval))
					{
						ereport(elevel,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						  errmsg("parameter \"%s\" requires a Boolean value",
								 name)));
						return false;
					}
				}
				else if (source == PGC_S_DEFAULT)
					newval = conf->boot_val;
				else
				{
					newval = conf->reset_val;
					source = conf->gen.reset_source;
				}

				/* Save old value to support transaction abort */
				if (changeVal && !makeDefault)
					push_old_value(&conf->gen, action);

				if (conf->assign_hook)
					if (!(*conf->assign_hook) (newval, changeVal, source))
					{
						ereport(elevel,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid value for parameter \"%s\": %d",
									name, (int) newval)));
						return false;
					}

				if (changeVal)
				{
					*conf->variable = newval;
					conf->gen.source = source;
				}
				if (makeDefault)
				{
					GucStack   *stack;

					if (conf->gen.reset_source <= source)
					{
						conf->reset_val = newval;
						conf->gen.reset_source = source;
					}
					for (stack = conf->gen.stack; stack; stack = stack->prev)
					{
						if (stack->source <= source)
						{
							stack->prior.boolval = newval;
							stack->source = source;
						}
					}
				}
				break;
			}

		case PGC_INT:
			{
				struct config_int *conf = (struct config_int *) record;
				int			newval;

				if (value)
				{
					const char *hintmsg;

					if (!parse_int(value, &newval, conf->gen.flags, &hintmsg))
					{
						ereport(elevel,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid value for parameter \"%s\": \"%s\"",
								name, value),
								 hintmsg ? errhint("%s", _(hintmsg)) : 0));
						return false;
					}
					if (newval < conf->min || newval > conf->max)
					{
						ereport(elevel,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("%d is outside the valid range for parameter \"%s\" (%d .. %d)",
										newval, name, conf->min, conf->max)));
						return false;
					}
				}
				else if (source == PGC_S_DEFAULT)
					newval = conf->boot_val;
				else
				{
					newval = conf->reset_val;
					source = conf->gen.reset_source;
				}

				/* Save old value to support transaction abort */
				if (changeVal && !makeDefault)
					push_old_value(&conf->gen, action);

				if (conf->assign_hook)
					if (!(*conf->assign_hook) (newval, changeVal, source))
					{
						ereport(elevel,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid value for parameter \"%s\": %d",
									name, newval)));
						return false;
					}

				if (changeVal)
				{
					*conf->variable = newval;
					conf->gen.source = source;
				}
				if (makeDefault)
				{
					GucStack   *stack;

					if (conf->gen.reset_source <= source)
					{
						conf->reset_val = newval;
						conf->gen.reset_source = source;
					}
					for (stack = conf->gen.stack; stack; stack = stack->prev)
					{
						if (stack->source <= source)
						{
							stack->prior.intval = newval;
							stack->source = source;
						}
					}
				}
				break;
			}

		case PGC_REAL:
			{
				struct config_real *conf = (struct config_real *) record;
				double		newval;

				if (value)
				{
					if (!parse_real(value, &newval))
					{
						ereport(elevel,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						  errmsg("parameter \"%s\" requires a numeric value",
								 name)));
						return false;
					}
					if (newval < conf->min || newval > conf->max)
					{
						ereport(elevel,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("%g is outside the valid range for parameter \"%s\" (%g .. %g)",
										newval, name, conf->min, conf->max)));
						return false;
					}
				}
				else if (source == PGC_S_DEFAULT)
					newval = conf->boot_val;
				else
				{
					newval = conf->reset_val;
					source = conf->gen.reset_source;
				}

				/* Save old value to support transaction abort */
				if (changeVal && !makeDefault)
					push_old_value(&conf->gen, action);

				if (conf->assign_hook)
					if (!(*conf->assign_hook) (newval, changeVal, source))
					{
						ereport(elevel,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid value for parameter \"%s\": %g",
									name, newval)));
						return false;
					}

				if (changeVal)
				{
					*conf->variable = newval;
					conf->gen.source = source;
				}
				if (makeDefault)
				{
					GucStack   *stack;

					if (conf->gen.reset_source <= source)
					{
						conf->reset_val = newval;
						conf->gen.reset_source = source;
					}
					for (stack = conf->gen.stack; stack; stack = stack->prev)
					{
						if (stack->source <= source)
						{
							stack->prior.realval = newval;
							stack->source = source;
						}
					}
				}
				break;
			}

		case PGC_STRING:
			{
				struct config_string *conf = (struct config_string *) record;
				char	   *newval;

				if (value)
				{
					newval = guc_strdup(elevel, value);
					if (newval == NULL)
						return false;

					/*
					 * The only sort of "parsing" check we need to do is apply
					 * truncation if GUC_IS_NAME.
					 */
					if (conf->gen.flags & GUC_IS_NAME)
						truncate_identifier(newval, strlen(newval), true);
				}
				else if (source == PGC_S_DEFAULT)
				{
					if (conf->boot_val == NULL)
						newval = NULL;
					else
					{
						newval = guc_strdup(elevel, conf->boot_val);
						if (newval == NULL)
							return false;
					}
				}
				else
				{
					/*
					 * We could possibly avoid strdup here, but easier to make
					 * this case work the same as the normal assignment case;
					 * note the possible free of newval below.
					 */
					if (conf->reset_val == NULL)
						newval = NULL;
					else
					{
						newval = guc_strdup(elevel, conf->reset_val);
						if (newval == NULL)
							return false;
					}
					source = conf->gen.reset_source;
				}

				/* Save old value to support transaction abort */
				if (changeVal && !makeDefault)
					push_old_value(&conf->gen, action);

				if (conf->assign_hook && newval)
				{
					const char *hookresult;

					/*
					 * If the hook ereports, we have to make sure we free
					 * newval, else it will be a permanent memory leak.
					 */
					hookresult = call_string_assign_hook(conf->assign_hook,
														 newval,
														 changeVal,
														 source);
					if (hookresult == NULL)
					{
						free(newval);
						ereport(elevel,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid value for parameter \"%s\": \"%s\"",
								name, value ? value : "")));
						return false;
					}
					else if (hookresult != newval)
					{
						free(newval);

						/*
						 * Having to cast away const here is annoying, but the
						 * alternative is to declare assign_hooks as returning
						 * char*, which would mean they'd have to cast away
						 * const, or as both taking and returning char*, which
						 * doesn't seem attractive either --- we don't want
						 * them to scribble on the passed str.
						 */
						newval = (char *) hookresult;
					}
				}

				if (changeVal)
				{
					set_string_field(conf, conf->variable, newval);
					conf->gen.source = source;
				}
				if (makeDefault)
				{
					GucStack   *stack;

					if (conf->gen.reset_source <= source)
					{
						set_string_field(conf, &conf->reset_val, newval);
						conf->gen.reset_source = source;
					}
					for (stack = conf->gen.stack; stack; stack = stack->prev)
					{
						if (stack->source <= source)
						{
							set_string_field(conf, &stack->prior.stringval,
											 newval);
							stack->source = source;
						}
					}
				}
				/* Perhaps we didn't install newval anywhere */
				if (newval && !string_field_used(conf, newval))
					free(newval);
				break;
			}
		case PGC_ENUM:
			{
				struct config_enum *conf = (struct config_enum *) record;
				int			newval;

				if (value)
				{
					if (!config_enum_lookup_by_name(conf, value, &newval))
					{
						char *hintmsg;
					   
						hintmsg	= config_enum_get_options(conf,
														  "Available values: ",
														  ".", ", ");

						ereport(elevel,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("invalid value for parameter \"%s\": \"%s\"",
										name, value),
								 hintmsg ? errhint("%s", _(hintmsg)) : 0));

						if (hintmsg)
							pfree(hintmsg);
						return false;
					}
				}
				else if (source == PGC_S_DEFAULT)
					newval = conf->boot_val;
				else
				{
					newval = conf->reset_val;
					source = conf->gen.reset_source;
				}

				/* Save old value to support transaction abort */
				if (changeVal && !makeDefault)
					push_old_value(&conf->gen, action);

				if (conf->assign_hook)
					if (!(*conf->assign_hook) (newval, changeVal, source))
					{
						ereport(elevel,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("invalid value for parameter \"%s\": \"%s\"",
										name, 
										config_enum_lookup_by_value(conf, newval))));
						return false;
					}

				if (changeVal)
				{
					*conf->variable = newval;
					conf->gen.source = source;
				}
				if (makeDefault)
				{
					GucStack   *stack;

					if (conf->gen.reset_source <= source)
					{
						conf->reset_val = newval;
						conf->gen.reset_source = source;
					}
					for (stack = conf->gen.stack; stack; stack = stack->prev)
					{
						if (stack->source <= source)
						{
							stack->prior.enumval = newval;
							stack->source = source;
						}
					}
				}
				break;
			}
	}

	if (changeVal && (record->flags & GUC_REPORT))
		ReportGUCOption(record);

	return true;
}


/*
 * Set the fields for source file and line number the setting came from.
 */
static void
set_config_sourcefile(const char *name, char *sourcefile, int sourceline)
{
	struct config_generic *record;
	int			elevel;

	/*
	 * To avoid cluttering the log, only the postmaster bleats loudly
	 * about problems with the config file.
	 */
	elevel = IsUnderPostmaster ? DEBUG3 : LOG;

	record = find_option(name, true, elevel);
	/* should not happen */
	if (record == NULL)
		elog(ERROR, "unrecognized configuration parameter \"%s\"", name);

	sourcefile = guc_strdup(elevel, sourcefile);
	if (record->sourcefile)
		free(record->sourcefile);
	record->sourcefile = sourcefile;
	record->sourceline = sourceline;
}

/*
 * Set a config option to the given value. See also set_config_option,
 * this is just the wrapper to be called from outside GUC.	NB: this
 * is used only for non-transactional operations.
 *
 * Note: there is no support here for setting source file/line, as it
 * is currently not needed.
 */
void
SetConfigOption(const char *name, const char *value,
				GucContext context, GucSource source)
{
	(void) set_config_option(name, value, context, source,
							 GUC_ACTION_SET, true);
}



/*
 * Fetch the current value of the option `name'. If the option doesn't exist,
 * throw an ereport and don't return.
 *
 * The string is *not* allocated for modification and is really only
 * valid until the next call to configuration related functions.
 */
const char *
GetConfigOption(const char *name)
{
	struct config_generic *record;
	static char buffer[256];

	record = find_option(name, false, ERROR);
	if (record == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
			   errmsg("unrecognized configuration parameter \"%s\"", name)));
	if ((record->flags & GUC_SUPERUSER_ONLY) && !superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to examine \"%s\"", name)));

	switch (record->vartype)
	{
		case PGC_BOOL:
			return *((struct config_bool *) record)->variable ? "on" : "off";

		case PGC_INT:
			snprintf(buffer, sizeof(buffer), "%d",
					 *((struct config_int *) record)->variable);
			return buffer;

		case PGC_REAL:
			snprintf(buffer, sizeof(buffer), "%g",
					 *((struct config_real *) record)->variable);
			return buffer;

		case PGC_STRING:
			return *((struct config_string *) record)->variable;

		case PGC_ENUM:
			return config_enum_lookup_by_value((struct config_enum *) record,
											*((struct config_enum *) record)->variable);
	}
	return NULL;
}

/*
 * Get the RESET value associated with the given option.
 *
 * Note: this is not re-entrant, due to use of static result buffer;
 * not to mention that a string variable could have its reset_val changed.
 * Beware of assuming the result value is good for very long.
 */
const char *
GetConfigOptionResetString(const char *name)
{
	struct config_generic *record;
	static char buffer[256];

	record = find_option(name, false, ERROR);
	if (record == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
			   errmsg("unrecognized configuration parameter \"%s\"", name)));
	if ((record->flags & GUC_SUPERUSER_ONLY) && !superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to examine \"%s\"", name)));

	switch (record->vartype)
	{
		case PGC_BOOL:
			return ((struct config_bool *) record)->reset_val ? "on" : "off";

		case PGC_INT:
			snprintf(buffer, sizeof(buffer), "%d",
					 ((struct config_int *) record)->reset_val);
			return buffer;

		case PGC_REAL:
			snprintf(buffer, sizeof(buffer), "%g",
					 ((struct config_real *) record)->reset_val);
			return buffer;

		case PGC_STRING:
			return ((struct config_string *) record)->reset_val;

		case PGC_ENUM:
			return config_enum_lookup_by_value((struct config_enum *) record,
										    ((struct config_enum *) record)->reset_val);
	}
	return NULL;
}

/*
 * Detect whether the given configuration option can only be set by
 * a superuser.
 */
bool
IsSuperuserConfigOption(const char *name)
{
	struct config_generic *record;

	record = find_option(name, false, ERROR);
	/* On an unrecognized name, don't error, just return false. */
	if (record == NULL)
		return false;
	return (record->context == PGC_SUSET);
}


/*
 * GUC_complaint_elevel
 *		Get the ereport error level to use in an assign_hook's error report.
 *
 * This should be used by assign hooks that want to emit a custom error
 * report (in addition to the generic "invalid value for option FOO" that
 * guc.c will provide).  Note that the result might be ERROR or a lower
 * level, so the caller must be prepared for control to return from ereport,
 * or not.  If control does return, return false/NULL from the hook function.
 *
 * At some point it'd be nice to replace this with a mechanism that allows
 * the custom message to become the DETAIL line of guc.c's generic message.
 */
int
GUC_complaint_elevel(GucSource source)
{
	int			elevel;

	if (source == PGC_S_FILE)
	{
		/*
		 * To avoid cluttering the log, only the postmaster bleats loudly
		 * about problems with the config file.
		 */
		elevel = IsUnderPostmaster ? DEBUG3 : LOG;
	}
	else if (source == PGC_S_OVERRIDE)
	{
		/*
		 * If we're a postmaster child, this is probably "undo" during
		 * transaction abort, so we don't want to clutter the log.  There's
		 * a small chance of a real problem with an OVERRIDE setting,
		 * though, so suppressing the message entirely wouldn't be desirable.
		 */
		elevel = IsUnderPostmaster ? DEBUG5 : LOG;
	}
	else if (source < PGC_S_INTERACTIVE)
		elevel = LOG;
	else
		elevel = ERROR;

	return elevel;
}


/*
 * flatten_set_variable_args
 *		Given a parsenode List as emitted by the grammar for SET,
 *		convert to the flat string representation used by GUC.
 *
 * We need to be told the name of the variable the args are for, because
 * the flattening rules vary (ugh).
 *
 * The result is NULL if args is NIL (ie, SET ... TO DEFAULT), otherwise
 * a palloc'd string.
 */
static char *
flatten_set_variable_args(const char *name, List *args)
{
	struct config_generic *record;
	int			flags;
	StringInfoData buf;
	ListCell   *l;

	/* Fast path if just DEFAULT */
	if (args == NIL)
		return NULL;

	/* Else get flags for the variable */
	record = find_option(name, true, ERROR);
	if (record == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
			   errmsg("unrecognized configuration parameter \"%s\"", name)));

	flags = record->flags;

	/* Complain if list input and non-list variable */
	if ((flags & GUC_LIST_INPUT) == 0 &&
		list_length(args) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("SET %s takes only one argument", name)));

	initStringInfo(&buf);

	/*
	 * Each list member may be a plain A_Const node, or an A_Const within a
	 * TypeCast; the latter case is supported only for ConstInterval
	 * arguments (for SET TIME ZONE).
	 */
	foreach(l, args)
	{
		Node       *arg = (Node *) lfirst(l);
		char	   *val;
		TypeName   *typename = NULL;
		A_Const	   *con;

		if (l != list_head(args))
			appendStringInfo(&buf, ", ");

		if (IsA(arg, TypeCast))
		{
			TypeCast *tc = (TypeCast *) arg;

			arg = tc->arg;
			typename = tc->typename;
		}

		if (!IsA(arg, A_Const))
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(arg));
		con = (A_Const *) arg;

		switch (nodeTag(&con->val))
		{
			case T_Integer:
				appendStringInfo(&buf, "%ld", intVal(&con->val));
				break;
			case T_Float:
				/* represented as a string, so just copy it */
				appendStringInfoString(&buf, strVal(&con->val));
				break;
			case T_String:
				val = strVal(&con->val);
				if (typename != NULL)
				{
					/*
					 * Must be a ConstInterval argument for TIME ZONE. Coerce
					 * to interval and back to normalize the value and account
					 * for any typmod.
					 */
					Oid			typoid;
					int32		typmod;
					Datum		interval;
					char	   *intervalout;

					typoid = typenameTypeId(NULL, typename, &typmod);
					Assert(typoid == INTERVALOID);

					interval =
						DirectFunctionCall3(interval_in,
											CStringGetDatum(val),
											ObjectIdGetDatum(InvalidOid),
											Int32GetDatum(typmod));

					intervalout =
						DatumGetCString(DirectFunctionCall1(interval_out,
															interval));
					appendStringInfo(&buf, "INTERVAL '%s'", intervalout);
				}
				else
				{
					/*
					 * Plain string literal or identifier.	For quote mode,
					 * quote it if it's not a vanilla identifier.
					 */
					if (flags & GUC_LIST_QUOTE)
						appendStringInfoString(&buf, quote_identifier(val));
					else
						appendStringInfoString(&buf, val);
				}
				break;
			default:
				elog(ERROR, "unrecognized node type: %d",
					 (int) nodeTag(&con->val));
				break;
		}
	}

	return buf.data;
}


/*
 * SET command
 */
void
ExecSetVariableStmt(VariableSetStmt *stmt)
{
	GucAction	action = stmt->is_local ? GUC_ACTION_LOCAL : GUC_ACTION_SET;

	switch (stmt->kind)
	{
		case VAR_SET_VALUE:
		case VAR_SET_CURRENT:
			set_config_option(stmt->name,
							  ExtractSetVariableArgs(stmt),
							  (superuser() ? PGC_SUSET : PGC_USERSET),
							  PGC_S_SESSION,
							  action,
							  true);
			break;
		case VAR_SET_MULTI:

			/*
			 * Special case for special SQL syntax that effectively sets more
			 * than one variable per statement.
			 */
			if (strcmp(stmt->name, "TRANSACTION") == 0)
			{
				ListCell   *head;

				foreach(head, stmt->args)
				{
					DefElem    *item = (DefElem *) lfirst(head);

					if (strcmp(item->defname, "transaction_isolation") == 0)
						SetPGVariable("transaction_isolation",
									  list_make1(item->arg), stmt->is_local);
					else if (strcmp(item->defname, "transaction_read_only") == 0)
						SetPGVariable("transaction_read_only",
									  list_make1(item->arg), stmt->is_local);
					else
						elog(ERROR, "unexpected SET TRANSACTION element: %s",
							 item->defname);
				}
			}
			else if (strcmp(stmt->name, "SESSION CHARACTERISTICS") == 0)
			{
				ListCell   *head;

				foreach(head, stmt->args)
				{
					DefElem    *item = (DefElem *) lfirst(head);

					if (strcmp(item->defname, "transaction_isolation") == 0)
						SetPGVariable("default_transaction_isolation",
									  list_make1(item->arg), stmt->is_local);
					else if (strcmp(item->defname, "transaction_read_only") == 0)
						SetPGVariable("default_transaction_read_only",
									  list_make1(item->arg), stmt->is_local);
					else
						elog(ERROR, "unexpected SET SESSION element: %s",
							 item->defname);
				}
			}
			else
				elog(ERROR, "unexpected SET MULTI element: %s",
					 stmt->name);
			break;
		case VAR_SET_DEFAULT:
		case VAR_RESET:
			set_config_option(stmt->name,
							  NULL,
							  (superuser() ? PGC_SUSET : PGC_USERSET),
							  PGC_S_SESSION,
							  action,
							  true);
			break;
		case VAR_RESET_ALL:
			ResetAllOptions();
			break;
	}
}

/*
 * Get the value to assign for a VariableSetStmt, or NULL if it's RESET.
 * The result is palloc'd.
 *
 * This is exported for use by actions such as ALTER ROLE SET.
 */
char *
ExtractSetVariableArgs(VariableSetStmt *stmt)
{
	switch (stmt->kind)
	{
		case VAR_SET_VALUE:
			return flatten_set_variable_args(stmt->name, stmt->args);
		case VAR_SET_CURRENT:
			return GetConfigOptionByName(stmt->name, NULL);
		default:
			return NULL;
	}
}

/*
 * SetPGVariable - SET command exported as an easily-C-callable function.
 *
 * This provides access to SET TO value, as well as SET TO DEFAULT (expressed
 * by passing args == NIL), but not SET FROM CURRENT functionality.
 */
void
SetPGVariable(const char *name, List *args, bool is_local)
{
	char	   *argstring = flatten_set_variable_args(name, args);

	/* Note SET DEFAULT (argstring == NULL) is equivalent to RESET */
	set_config_option(name,
					  argstring,
					  (superuser() ? PGC_SUSET : PGC_USERSET),
					  PGC_S_SESSION,
					  is_local ? GUC_ACTION_LOCAL : GUC_ACTION_SET,
					  true);
}

/*
 * SET command wrapped as a SQL callable function.
 */
Datum
set_config_by_name(PG_FUNCTION_ARGS)
{
	char	   *name;
	char	   *value;
	char	   *new_value;
	bool		is_local;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("SET requires parameter name")));

	/* Get the GUC variable name */
	name = TextDatumGetCString(PG_GETARG_DATUM(0));

	/* Get the desired value or set to NULL for a reset request */
	if (PG_ARGISNULL(1))
		value = NULL;
	else
		value = TextDatumGetCString(PG_GETARG_DATUM(1));

	/*
	 * Get the desired state of is_local. Default to false if provided value
	 * is NULL
	 */
	if (PG_ARGISNULL(2))
		is_local = false;
	else
		is_local = PG_GETARG_BOOL(2);

	/* Note SET DEFAULT (argstring == NULL) is equivalent to RESET */
	set_config_option(name,
					  value,
					  (superuser() ? PGC_SUSET : PGC_USERSET),
					  PGC_S_SESSION,
					  is_local ? GUC_ACTION_LOCAL : GUC_ACTION_SET,
					  true);

	/* get the new current value */
	new_value = GetConfigOptionByName(name, NULL);

	/* Convert return string to text */
	PG_RETURN_TEXT_P(cstring_to_text(new_value));
}


/*
 * Common code for DefineCustomXXXVariable subroutines: allocate the
 * new variable's config struct and fill in generic fields.
 */
static struct config_generic *
init_custom_variable(const char *name,
					 const char *short_desc,
					 const char *long_desc,
					 GucContext context,
					 int flags,
					 enum config_type type,
					 size_t sz)
{
	struct config_generic *gen;

	/*
	 * Only allow custom PGC_POSTMASTER variables to be created during
	 * shared library preload; any later than that, we can't ensure that
	 * the value doesn't change after startup.  This is a fatal elog if it
	 * happens; just erroring out isn't safe because we don't know what
	 * the calling loadable module might already have hooked into.
	 */
	if (context == PGC_POSTMASTER &&
		!process_shared_preload_libraries_in_progress)
		elog(FATAL, "cannot create PGC_POSTMASTER variables after startup");

	gen = (struct config_generic *) guc_malloc(ERROR, sz);
	memset(gen, 0, sz);

	gen->name = guc_strdup(ERROR, name);
	gen->context = context;
	gen->group = CUSTOM_OPTIONS;
	gen->short_desc = short_desc;
	gen->long_desc = long_desc;
	gen->flags = flags;
	gen->vartype = type;

	return gen;
}

/*
 * Common code for DefineCustomXXXVariable subroutines: insert the new
 * variable into the GUC variable array, replacing any placeholder.
 */
static void
define_custom_variable(struct config_generic * variable)
{
	const char *name = variable->name;
	const char **nameAddr = &name;
	const char *value;
	struct config_string *pHolder;
	GucContext phcontext;
	struct config_generic **res;

	/*
	 * See if there's a placeholder by the same name.
	 */
	res = (struct config_generic **) bsearch((void *) &nameAddr,
											 (void *) guc_variables,
											 num_guc_variables,
											 sizeof(struct config_generic *),
											 guc_var_compare);
	if (res == NULL)
	{
		/*
		 * No placeholder to replace, so we can just add it ... but first,
		 * make sure it's initialized to its default value.
		 */
		InitializeOneGUCOption(variable);
		add_guc_variable(variable, ERROR);
		return;
	}

	/*
	 * This better be a placeholder
	 */
	if (((*res)->flags & GUC_CUSTOM_PLACEHOLDER) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("attempt to redefine parameter \"%s\"", name)));

	Assert((*res)->vartype == PGC_STRING);
	pHolder = (struct config_string *) (*res);

	/*
	 * First, set the variable to its default value.  We must do this even
	 * though we intend to immediately apply a new value, since it's possible
	 * that the new value is invalid.
	 */
	InitializeOneGUCOption(variable);

	/*
	 * Replace the placeholder. We aren't changing the name, so no re-sorting
	 * is necessary
	 */
	*res = variable;

	/*
	 * Infer context for assignment based on source of existing value.
	 * We can't tell this with exact accuracy, but we can at least do
	 * something reasonable in typical cases.
	 */
	switch (pHolder->gen.source)
	{
		case PGC_S_DEFAULT:
		case PGC_S_ENV_VAR:
		case PGC_S_FILE:
		case PGC_S_ARGV:
			/*
			 * If we got past the check in init_custom_variable, we can
			 * safely assume that any existing value for a PGC_POSTMASTER
			 * variable was set in postmaster context.
			 */
			if (variable->context == PGC_POSTMASTER)
				phcontext = PGC_POSTMASTER;
			else
				phcontext = PGC_SIGHUP;
			break;
		case PGC_S_DATABASE:
		case PGC_S_USER:
		case PGC_S_CLIENT:
		case PGC_S_SESSION:
		default:
			phcontext = PGC_USERSET;
			break;
	}

	/*
	 * Assign the string value stored in the placeholder to the real variable.
	 *
	 * XXX this is not really good enough --- it should be a nontransactional
	 * assignment, since we don't want it to roll back if the current xact
	 * fails later.  (Or do we?)
	 */
	value = *pHolder->variable;

	if (value)
	{
		if (set_config_option(name, value,
							  phcontext, pHolder->gen.source,
							  GUC_ACTION_SET, true))
		{
			/* Also copy over any saved source-location information */
			if (pHolder->gen.sourcefile)
				set_config_sourcefile(name, pHolder->gen.sourcefile,
									  pHolder->gen.sourceline);
		}
	}

	/*
	 * Free up as much as we conveniently can of the placeholder structure
	 * (this neglects any stack items...)
	 */
	set_string_field(pHolder, pHolder->variable, NULL);
	set_string_field(pHolder, &pHolder->reset_val, NULL);

	free(pHolder);
}

void
DefineCustomBoolVariable(const char *name,
						 const char *short_desc,
						 const char *long_desc,
						 bool *valueAddr,
						 bool bootValue,
						 GucContext context,
						 int flags,
						 GucBoolAssignHook assign_hook,
						 GucShowHook show_hook)
{
	struct config_bool *var;

	var = (struct config_bool *)
		init_custom_variable(name, short_desc, long_desc, context, flags,
							 PGC_BOOL, sizeof(struct config_bool));
	var->variable = valueAddr;
	var->boot_val = bootValue;
	var->reset_val = bootValue;
	var->assign_hook = assign_hook;
	var->show_hook = show_hook;
	define_custom_variable(&var->gen);
}

void
DefineCustomIntVariable(const char *name,
						const char *short_desc,
						const char *long_desc,
						int *valueAddr,
						int bootValue,
						int minValue,
						int maxValue,
						GucContext context,
						int flags,
						GucIntAssignHook assign_hook,
						GucShowHook show_hook)
{
	struct config_int *var;

	var = (struct config_int *)
		init_custom_variable(name, short_desc, long_desc, context, flags,
							 PGC_INT, sizeof(struct config_int));
	var->variable = valueAddr;
	var->boot_val = bootValue;
	var->reset_val = bootValue;
	var->min = minValue;
	var->max = maxValue;
	var->assign_hook = assign_hook;
	var->show_hook = show_hook;
	define_custom_variable(&var->gen);
}

void
DefineCustomRealVariable(const char *name,
						 const char *short_desc,
						 const char *long_desc,
						 double *valueAddr,
						 double bootValue,
						 double minValue,
						 double maxValue,
						 GucContext context,
						 int flags,
						 GucRealAssignHook assign_hook,
						 GucShowHook show_hook)
{
	struct config_real *var;

	var = (struct config_real *)
		init_custom_variable(name, short_desc, long_desc, context, flags,
							 PGC_REAL, sizeof(struct config_real));
	var->variable = valueAddr;
	var->boot_val = bootValue;
	var->reset_val = bootValue;
	var->min = minValue;
	var->max = maxValue;
	var->assign_hook = assign_hook;
	var->show_hook = show_hook;
	define_custom_variable(&var->gen);
}

void
DefineCustomStringVariable(const char *name,
						   const char *short_desc,
						   const char *long_desc,
						   char **valueAddr,
						   const char *bootValue,
						   GucContext context,
						   int flags,
						   GucStringAssignHook assign_hook,
						   GucShowHook show_hook)
{
	struct config_string *var;

	var = (struct config_string *)
		init_custom_variable(name, short_desc, long_desc, context, flags,
							 PGC_STRING, sizeof(struct config_string));
	var->variable = valueAddr;
	var->boot_val = bootValue;
	/* we could probably do without strdup, but keep it like normal case */
	if (var->boot_val)
		var->reset_val = guc_strdup(ERROR, var->boot_val);
	var->assign_hook = assign_hook;
	var->show_hook = show_hook;
	define_custom_variable(&var->gen);
}

void
DefineCustomEnumVariable(const char *name,
						 const char *short_desc,
						 const char *long_desc,
						 int *valueAddr,
						 int bootValue,
						 const struct config_enum_entry *options,
						 GucContext context,
						 int flags,
						 GucEnumAssignHook assign_hook,
						 GucShowHook show_hook)
{
	struct config_enum *var;

	var = (struct config_enum *)
		init_custom_variable(name, short_desc, long_desc, context, flags,
							 PGC_ENUM, sizeof(struct config_enum));
	var->variable = valueAddr;
	var->boot_val = bootValue;
	var->reset_val = bootValue;
	var->options = options;
	var->assign_hook = assign_hook;
	var->show_hook = show_hook;
	define_custom_variable(&var->gen);
}

void
EmitWarningsOnPlaceholders(const char *className)
{
	int			classLen = strlen(className);
	int			i;

	for (i = 0; i < num_guc_variables; i++)
	{
		struct config_generic *var = guc_variables[i];

		if ((var->flags & GUC_CUSTOM_PLACEHOLDER) != 0 &&
			strncmp(className, var->name, classLen) == 0 &&
			var->name[classLen] == GUC_QUALIFIER_SEPARATOR)
		{
			ereport(WARNING,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("unrecognized configuration parameter \"%s\"",
							var->name)));
		}
	}
}


/*
 * SHOW command
 */
void
GetPGVariable(const char *name, DestReceiver *dest)
{
	if (guc_name_compare(name, "all") == 0)
		ShowAllGUCConfig(dest);
	else
		ShowGUCConfigOption(name, dest);
}

TupleDesc
GetPGVariableResultDesc(const char *name)
{
	TupleDesc	tupdesc;

	if (guc_name_compare(name, "all") == 0)
	{
		/* need a tuple descriptor representing three TEXT columns */
		tupdesc = CreateTemplateTupleDesc(3, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "name",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "setting",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "description",
						   TEXTOID, -1, 0);
	}
	else
	{
		const char *varname;

		/* Get the canonical spelling of name */
		(void) GetConfigOptionByName(name, &varname);

		/* need a tuple descriptor representing a single TEXT column */
		tupdesc = CreateTemplateTupleDesc(1, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, varname,
						   TEXTOID, -1, 0);
	}
	return tupdesc;
}


/*
 * SHOW command
 */
static void
ShowGUCConfigOption(const char *name, DestReceiver *dest)
{
	TupOutputState *tstate;
	TupleDesc	tupdesc;
	const char *varname;
	char	   *value;

	/* Get the value and canonical spelling of name */
	value = GetConfigOptionByName(name, &varname);

	/* need a tuple descriptor representing a single TEXT column */
	tupdesc = CreateTemplateTupleDesc(1, false);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, varname,
					   TEXTOID, -1, 0);

	/* prepare for projection of tuples */
	tstate = begin_tup_output_tupdesc(dest, tupdesc);

	/* Send it */
	do_text_output_oneline(tstate, value);

	end_tup_output(tstate);
}

/*
 * SHOW ALL command
 */
static void
ShowAllGUCConfig(DestReceiver *dest)
{
	bool		am_superuser = superuser();
	int			i;
	TupOutputState *tstate;
	TupleDesc	tupdesc;
	char	   *values[3];

	/* need a tuple descriptor representing three TEXT columns */
	tupdesc = CreateTemplateTupleDesc(3, false);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "name",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "setting",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "description",
					   TEXTOID, -1, 0);


	/* prepare for projection of tuples */
	tstate = begin_tup_output_tupdesc(dest, tupdesc);

	for (i = 0; i < num_guc_variables; i++)
	{
		struct config_generic *conf = guc_variables[i];

		if ((conf->flags & GUC_NO_SHOW_ALL) ||
			((conf->flags & GUC_SUPERUSER_ONLY) && !am_superuser))
			continue;

		/* assign to the values array */
		values[0] = (char *) conf->name;
		values[1] = _ShowOption(conf, true);
		values[2] = (char *) conf->short_desc;

		/* send it to dest */
		do_tup_output(tstate, values);

		/* clean up */
		if (values[1] != NULL)
			pfree(values[1]);
	}

	end_tup_output(tstate);
}

/*
 * Return GUC variable value by name; optionally return canonical
 * form of name.  Return value is palloc'd.
 */
char *
GetConfigOptionByName(const char *name, const char **varname)
{
	struct config_generic *record;

	record = find_option(name, false, ERROR);
	if (record == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
			   errmsg("unrecognized configuration parameter \"%s\"", name)));
	if ((record->flags & GUC_SUPERUSER_ONLY) && !superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to examine \"%s\"", name)));

	if (varname)
		*varname = record->name;

	return _ShowOption(record, true);
}

/*
 * Return GUC variable value by variable number; optionally return canonical
 * form of name.  Return value is palloc'd.
 */
void
GetConfigOptionByNum(int varnum, const char **values, bool *noshow)
{
	char		buffer[256];
	struct config_generic *conf;

	/* check requested variable number valid */
	Assert((varnum >= 0) && (varnum < num_guc_variables));

	conf = guc_variables[varnum];

	if (noshow)
	{
		if ((conf->flags & GUC_NO_SHOW_ALL) ||
			((conf->flags & GUC_SUPERUSER_ONLY) && !superuser()))
			*noshow = true;
		else
			*noshow = false;
	}

	/* first get the generic attributes */

	/* name */
	values[0] = conf->name;

	/* setting : use _ShowOption in order to avoid duplicating the logic */
	values[1] = _ShowOption(conf, false);

	/* unit */
	if (conf->vartype == PGC_INT)
	{
		static char buf[8];

		switch (conf->flags & (GUC_UNIT_MEMORY | GUC_UNIT_TIME))
		{
			case GUC_UNIT_KB:
				values[2] = "kB";
				break;
			case GUC_UNIT_BLOCKS:
				snprintf(buf, sizeof(buf), "%dkB", BLCKSZ / 1024);
				values[2] = buf;
				break;
			case GUC_UNIT_XBLOCKS:
				snprintf(buf, sizeof(buf), "%dkB", XLOG_BLCKSZ / 1024);
				values[2] = buf;
				break;
			case GUC_UNIT_MS:
				values[2] = "ms";
				break;
			case GUC_UNIT_S:
				values[2] = "s";
				break;
			case GUC_UNIT_MIN:
				values[2] = "min";
				break;
			default:
				values[2] = "";
				break;
		}
	}
	else
		values[2] = NULL;

	/* group */
	values[3] = config_group_names[conf->group];

	/* short_desc */
	values[4] = conf->short_desc;

	/* extra_desc */
	values[5] = conf->long_desc;

	/* context */
	values[6] = GucContext_Names[conf->context];

	/* vartype */
	values[7] = config_type_names[conf->vartype];

	/* source */
	values[8] = GucSource_Names[conf->source];

	/* now get the type specifc attributes */
	switch (conf->vartype)
	{
		case PGC_BOOL:
			{
				struct config_bool *lconf = (struct config_bool *) conf;

				/* min_val */
				values[9] = NULL;

				/* max_val */
				values[10] = NULL;

				/* enumvals */
				values[11] = NULL;

				/* boot_val */
				values[12] = pstrdup(lconf->boot_val ? "on" : "off");

				/* reset_val */
				values[13] = pstrdup(lconf->reset_val ? "on" : "off");
			}
			break;

		case PGC_INT:
			{
				struct config_int *lconf = (struct config_int *) conf;

				/* min_val */
				snprintf(buffer, sizeof(buffer), "%d", lconf->min);
				values[9] = pstrdup(buffer);

				/* max_val */
				snprintf(buffer, sizeof(buffer), "%d", lconf->max);
				values[10] = pstrdup(buffer);

				/* enumvals */
				values[11] = NULL;

 				/* boot_val */
 				snprintf(buffer, sizeof(buffer), "%d", lconf->boot_val);
 				values[12] = pstrdup(buffer);

 				/* reset_val */
 				snprintf(buffer, sizeof(buffer), "%d", lconf->reset_val);
 				values[13] = pstrdup(buffer);
			}
			break;

		case PGC_REAL:
			{
				struct config_real *lconf = (struct config_real *) conf;

				/* min_val */
				snprintf(buffer, sizeof(buffer), "%g", lconf->min);
				values[9] = pstrdup(buffer);

				/* max_val */
				snprintf(buffer, sizeof(buffer), "%g", lconf->max);
				values[10] = pstrdup(buffer);

				/* enumvals */
				values[11] = NULL;

 				/* boot_val */
 				snprintf(buffer, sizeof(buffer), "%g", lconf->boot_val);
 				values[12] = pstrdup(buffer);

 				/* reset_val */
 				snprintf(buffer, sizeof(buffer), "%g", lconf->reset_val);
 				values[13] = pstrdup(buffer);
			}
			break;

		case PGC_STRING:
			{
 				struct config_string *lconf = (struct config_string *) conf;

				/* min_val */
				values[9] = NULL;

				/* max_val */
				values[10] = NULL;

				/* enumvals */
				values[11] = NULL;

 				/* boot_val */
 				if (lconf->boot_val == NULL)
 					values[12] = NULL;
				else
					values[12] = pstrdup(lconf->boot_val);

 				/* reset_val */
 				if (lconf->reset_val == NULL)
 					values[13] = NULL;
				else
					values[13] = pstrdup(lconf->reset_val);
			}
			break;

		case PGC_ENUM:
			{
 				struct config_enum *lconf = (struct config_enum *) conf;

				/* min_val */
				values[9] = NULL;

				/* max_val */
				values[10] = NULL;

				/* enumvals */
				/* NOTE! enumvals with double quotes in them are not supported! */
				values[11] = config_enum_get_options((struct config_enum *) conf,
													 "{\"", "\"}", "\",\"");

 				/* boot_val */
				values[12] = pstrdup(config_enum_lookup_by_value(lconf,
																 lconf->boot_val));

 				/* reset_val */
				values[13] = pstrdup(config_enum_lookup_by_value(lconf,
																 lconf->reset_val));
			}
			break;

		default:
			{
				/*
				 * should never get here, but in case we do, set 'em to NULL
				 */

				/* min_val */
				values[9] = NULL;

				/* max_val */
				values[10] = NULL;

				/* enumvals */
				values[11] = NULL;

 				/* boot_val */
 				values[12] = NULL;

 				/* reset_val */
 				values[13] = NULL;
			}
			break;
	}

	/* 
	 * If the setting came from a config file, set the source location.
	 * For security reasons, we don't show source file/line number for
	 * non-superusers.
	 */
	if (conf->source == PGC_S_FILE && superuser())
	{
		values[14] = conf->sourcefile;
		snprintf(buffer, sizeof(buffer), "%d", conf->sourceline);
		values[15] = pstrdup(buffer);
	}
	else
	{
		values[14] = NULL;
		values[15] = NULL;
	}
}

/*
 * Return the total number of GUC variables
 */
int
GetNumConfigOptions(void)
{
	return num_guc_variables;
}

/*
 * show_config_by_name - equiv to SHOW X command but implemented as
 * a function.
 */
Datum
show_config_by_name(PG_FUNCTION_ARGS)
{
	char	   *varname;
	char	   *varval;

	/* Get the GUC variable name */
	varname = TextDatumGetCString(PG_GETARG_DATUM(0));

	/* Get the value */
	varval = GetConfigOptionByName(varname, NULL);

	/* Convert to text */
	PG_RETURN_TEXT_P(cstring_to_text(varval));
}

/*
 * show_all_settings - equiv to SHOW ALL command but implemented as
 * a Table Function.
 */
#define NUM_PG_SETTINGS_ATTS	16

Datum
show_all_settings(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	TupleDesc	tupdesc;
	int			call_cntr;
	int			max_calls;
	AttInMetadata *attinmeta;
	MemoryContext oldcontext;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * switch to memory context appropriate for multiple function calls
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/*
		 * need a tuple descriptor representing NUM_PG_SETTINGS_ATTS columns
		 * of the appropriate types
		 */
		tupdesc = CreateTemplateTupleDesc(NUM_PG_SETTINGS_ATTS, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "name",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "setting",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "unit",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 4, "category",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 5, "short_desc",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 6, "extra_desc",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 7, "context",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 8, "vartype",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 9, "source",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 10, "min_val",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 11, "max_val",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 12, "enumvals",
						   TEXTARRAYOID, -1, 0);
 		TupleDescInitEntry(tupdesc, (AttrNumber) 13, "boot_val",
 						   TEXTOID, -1, 0);
 		TupleDescInitEntry(tupdesc, (AttrNumber) 14, "reset_val",
 						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 15, "sourcefile",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 16, "sourceline",
						   INT4OID, -1, 0);

		/*
		 * Generate attribute metadata needed later to produce tuples from raw
		 * C strings
		 */
		attinmeta = TupleDescGetAttInMetadata(tupdesc);
		funcctx->attinmeta = attinmeta;

		/* total number of tuples to be returned */
		funcctx->max_calls = GetNumConfigOptions();

		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();

	call_cntr = funcctx->call_cntr;
	max_calls = funcctx->max_calls;
	attinmeta = funcctx->attinmeta;

	if (call_cntr < max_calls)	/* do when there is more left to send */
	{
		char	   *values[NUM_PG_SETTINGS_ATTS];
		bool		noshow;
		HeapTuple	tuple;
		Datum		result;

		/*
		 * Get the next visible GUC variable name and value
		 */
		do
		{
			GetConfigOptionByNum(call_cntr, (const char **) values, &noshow);
			if (noshow)
			{
				/* bump the counter and get the next config setting */
				call_cntr = ++funcctx->call_cntr;

				/* make sure we haven't gone too far now */
				if (call_cntr >= max_calls)
					SRF_RETURN_DONE(funcctx);
			}
		} while (noshow);

		/* build a tuple */
		tuple = BuildTupleFromCStrings(attinmeta, values);

		/* make the tuple into a datum */
		result = HeapTupleGetDatum(tuple);

		SRF_RETURN_NEXT(funcctx, result);
	}
	else
	{
		/* do when there is no more left */
		SRF_RETURN_DONE(funcctx);
	}
}

static char *
_ShowOption(struct config_generic * record, bool use_units)
{
	char		buffer[256];
	const char *val;

	switch (record->vartype)
	{
		case PGC_BOOL:
			{
				struct config_bool *conf = (struct config_bool *) record;

				if (conf->show_hook)
					val = (*conf->show_hook) ();
				else
					val = *conf->variable ? "on" : "off";
			}
			break;

		case PGC_INT:
			{
				struct config_int *conf = (struct config_int *) record;

				if (conf->show_hook)
					val = (*conf->show_hook) ();
				else
				{
					/*
					 * Use int64 arithmetic to avoid overflows in units
					 * conversion.  If INT64_IS_BUSTED we might overflow
					 * anyway and print bogus answers, but there are few
					 * enough such machines that it doesn't seem worth
					 * trying harder.
					 */
					int64		result = *conf->variable;
					const char *unit;

					if (use_units && result > 0 &&
						(record->flags & GUC_UNIT_MEMORY))
					{
						switch (record->flags & GUC_UNIT_MEMORY)
						{
							case GUC_UNIT_BLOCKS:
								result *= BLCKSZ / 1024;
								break;
							case GUC_UNIT_XBLOCKS:
								result *= XLOG_BLCKSZ / 1024;
								break;
						}

						if (result % KB_PER_GB == 0)
						{
							result /= KB_PER_GB;
							unit = "GB";
						}
						else if (result % KB_PER_MB == 0)
						{
							result /= KB_PER_MB;
							unit = "MB";
						}
						else
						{
							unit = "kB";
						}
					}
					else if (use_units && result > 0 &&
							 (record->flags & GUC_UNIT_TIME))
					{
						switch (record->flags & GUC_UNIT_TIME)
						{
							case GUC_UNIT_S:
								result *= MS_PER_S;
								break;
							case GUC_UNIT_MIN:
								result *= MS_PER_MIN;
								break;
						}

						if (result % MS_PER_D == 0)
						{
							result /= MS_PER_D;
							unit = "d";
						}
						else if (result % MS_PER_H == 0)
						{
							result /= MS_PER_H;
							unit = "h";
						}
						else if (result % MS_PER_MIN == 0)
						{
							result /= MS_PER_MIN;
							unit = "min";
						}
						else if (result % MS_PER_S == 0)
						{
							result /= MS_PER_S;
							unit = "s";
						}
						else
						{
							unit = "ms";
						}
					}
					else
						unit = "";

					snprintf(buffer, sizeof(buffer), INT64_FORMAT "%s",
							 result, unit);
					val = buffer;
				}
			}
			break;

		case PGC_REAL:
			{
				struct config_real *conf = (struct config_real *) record;

				if (conf->show_hook)
					val = (*conf->show_hook) ();
				else
				{
					snprintf(buffer, sizeof(buffer), "%g",
							 *conf->variable);
					val = buffer;
				}
			}
			break;

		case PGC_STRING:
			{
				struct config_string *conf = (struct config_string *) record;

				if (conf->show_hook)
					val = (*conf->show_hook) ();
				else if (*conf->variable && **conf->variable)
					val = *conf->variable;
				else
					val = "";
			}
			break;

		case PGC_ENUM:
			{
				struct config_enum *conf = (struct config_enum *) record;

				if(conf->show_hook)
					val = (*conf->show_hook) ();
				else
					val = config_enum_lookup_by_value(conf, *conf->variable);
			}
			break;

		default:
			/* just to keep compiler quiet */
			val = "???";
			break;
	}

	return pstrdup(val);
}


/*
 * Attempt (badly) to detect if a proposed new GUC setting is the same
 * as the current value.
 *
 * XXX this does not really work because it doesn't account for the
 * effects of canonicalization of string values by assign_hooks.
 */
static bool
is_newvalue_equal(struct config_generic * record, const char *newvalue)
{
	/* newvalue == NULL isn't supported */
	Assert(newvalue != NULL);

	switch (record->vartype)
	{
		case PGC_BOOL:
			{
				struct config_bool *conf = (struct config_bool *) record;
				bool		newval;

				return parse_bool(newvalue, &newval)
					&& *conf->variable == newval;
			}
		case PGC_INT:
			{
				struct config_int *conf = (struct config_int *) record;
				int			newval;

				return parse_int(newvalue, &newval, record->flags, NULL)
					&& *conf->variable == newval;
			}
		case PGC_REAL:
			{
				struct config_real *conf = (struct config_real *) record;
				double		newval;

				return parse_real(newvalue, &newval)
					&& *conf->variable == newval;
			}
		case PGC_STRING:
			{
				struct config_string *conf = (struct config_string *) record;

				return *conf->variable != NULL &&
					strcmp(*conf->variable, newvalue) == 0;
			}

		case PGC_ENUM:
			{
				struct config_enum *conf = (struct config_enum *) record;
				int			newval;

				return config_enum_lookup_by_name(conf, newvalue, &newval) &&
					*conf->variable == newval;
			}
	}

	return false;
}


#ifdef EXEC_BACKEND

/*
 *	These routines dump out all non-default GUC options into a binary
 *	file that is read by all exec'ed backends.  The format is:
 *
 *		variable name, string, null terminated
 *		variable value, string, null terminated
 *		variable source, integer
 */
static void
write_one_nondefault_variable(FILE *fp, struct config_generic *gconf)
{
	if (gconf->source == PGC_S_DEFAULT)
		return;

	fprintf(fp, "%s", gconf->name);
	fputc(0, fp);

	switch (gconf->vartype)
	{
		case PGC_BOOL:
		{
			struct config_bool *conf = (struct config_bool *) gconf;

			if (*conf->variable)
				fprintf(fp, "true");
			else
				fprintf(fp, "false");
		}
		break;

		case PGC_INT:
		{
			struct config_int *conf = (struct config_int *) gconf;

			fprintf(fp, "%d", *conf->variable);
		}
		break;

		case PGC_REAL:
		{
			struct config_real *conf = (struct config_real *) gconf;

			/* Could lose precision here? */
			fprintf(fp, "%f", *conf->variable);
		}
		break;

		case PGC_STRING:
		{
			struct config_string *conf = (struct config_string *) gconf;

			fprintf(fp, "%s", *conf->variable);
		}
		break;

		case PGC_ENUM:
		{
			struct config_enum *conf = (struct config_enum *) gconf;
						
			fprintf(fp, "%s",
					config_enum_lookup_by_value(conf, *conf->variable));
		}
		break;
	}

	fputc(0, fp);

	fwrite(&gconf->source, sizeof(gconf->source), 1, fp);
}

void
write_nondefault_variables(GucContext context)
{
	int			elevel;
	FILE	   *fp;
	struct config_generic *cvc_conf;
	int			i;

	Assert(context == PGC_POSTMASTER || context == PGC_SIGHUP);

	elevel = (context == PGC_SIGHUP) ? LOG : ERROR;

	/*
	 * Open file
	 */
	fp = AllocateFile(CONFIG_EXEC_PARAMS_NEW, "w");
	if (!fp)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m",
						CONFIG_EXEC_PARAMS_NEW)));
		return;
	}

	/*
	 * custom_variable_classes must be written out first; otherwise we might
	 * reject custom variable values while reading the file.
	 */
	cvc_conf = find_option("custom_variable_classes", false, ERROR);
	if (cvc_conf)
		write_one_nondefault_variable(fp, cvc_conf);

	for (i = 0; i < num_guc_variables; i++)
	{
		struct config_generic *gconf = guc_variables[i];

		if (gconf != cvc_conf)
			write_one_nondefault_variable(fp, gconf);
	}

	if (FreeFile(fp))
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m",
						CONFIG_EXEC_PARAMS_NEW)));
		return;
	}

	/*
	 * Put new file in place.  This could delay on Win32, but we don't hold
	 * any exclusive locks.
	 */
	rename(CONFIG_EXEC_PARAMS_NEW, CONFIG_EXEC_PARAMS);
}


/*
 *	Read string, including null byte from file
 *
 *	Return NULL on EOF and nothing read
 */
static char *
read_string_with_null(FILE *fp)
{
	int			i = 0,
				ch,
				maxlen = 256;
	char	   *str = NULL;

	do
	{
		if ((ch = fgetc(fp)) == EOF)
		{
			if (i == 0)
				return NULL;
			else
				elog(FATAL, "invalid format of exec config params file");
		}
		if (i == 0)
			str = guc_malloc(FATAL, maxlen);
		else if (i == maxlen)
			str = guc_realloc(FATAL, str, maxlen *= 2);
		str[i++] = ch;
	} while (ch != 0);

	return str;
}


/*
 *	This routine loads a previous postmaster dump of its non-default
 *	settings.
 */
void
read_nondefault_variables(void)
{
	FILE	   *fp;
	char	   *varname,
			   *varvalue;
	int			varsource;

	/*
	 * Open file
	 */
	fp = AllocateFile(CONFIG_EXEC_PARAMS, "r");
	if (!fp)
	{
		/* File not found is fine */
		if (errno != ENOENT)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not read from file \"%s\": %m",
							CONFIG_EXEC_PARAMS)));
		return;
	}

	for (;;)
	{
		struct config_generic *record;

		if ((varname = read_string_with_null(fp)) == NULL)
			break;

		if ((record = find_option(varname, true, FATAL)) == NULL)
			elog(FATAL, "failed to locate variable %s in exec config params file", varname);
		if ((varvalue = read_string_with_null(fp)) == NULL)
			elog(FATAL, "invalid format of exec config params file");
		if (fread(&varsource, sizeof(varsource), 1, fp) == 0)
			elog(FATAL, "invalid format of exec config params file");

		(void) set_config_option(varname, varvalue, record->context,
								 varsource, GUC_ACTION_SET, true);
		free(varname);
		free(varvalue);
	}

	FreeFile(fp);
}
#endif   /* EXEC_BACKEND */


/*
 * A little "long argument" simulation, although not quite GNU
 * compliant. Takes a string of the form "some-option=some value" and
 * returns name = "some_option" and value = "some value" in malloc'ed
 * storage. Note that '-' is converted to '_' in the option name. If
 * there is no '=' in the input string then value will be NULL.
 */
void
ParseLongOption(const char *string, char **name, char **value)
{
	size_t		equal_pos;
	char	   *cp;

	AssertArg(string);
	AssertArg(name);
	AssertArg(value);

	equal_pos = strcspn(string, "=");

	if (string[equal_pos] == '=')
	{
		*name = guc_malloc(FATAL, equal_pos + 1);
		strlcpy(*name, string, equal_pos + 1);

		*value = guc_strdup(FATAL, &string[equal_pos + 1]);
	}
	else
	{
		/* no equal sign in string */
		*name = guc_strdup(FATAL, string);
		*value = NULL;
	}

	for (cp = *name; *cp; cp++)
		if (*cp == '-')
			*cp = '_';
}


/*
 * Handle options fetched from pg_database.datconfig, pg_authid.rolconfig,
 * pg_proc.proconfig, etc.	Caller must specify proper context/source/action.
 *
 * The array parameter must be an array of TEXT (it must not be NULL).
 */
void
ProcessGUCArray(ArrayType *array,
				GucContext context, GucSource source, GucAction action)
{
	int			i;

	Assert(array != NULL);
	Assert(ARR_ELEMTYPE(array) == TEXTOID);
	Assert(ARR_NDIM(array) == 1);
	Assert(ARR_LBOUND(array)[0] == 1);

	for (i = 1; i <= ARR_DIMS(array)[0]; i++)
	{
		Datum		d;
		bool		isnull;
		char	   *s;
		char	   *name;
		char	   *value;

		d = array_ref(array, 1, &i,
					  -1 /* varlenarray */ ,
					  -1 /* TEXT's typlen */ ,
					  false /* TEXT's typbyval */ ,
					  'i' /* TEXT's typalign */ ,
					  &isnull);

		if (isnull)
			continue;

		s = TextDatumGetCString(d);

		ParseLongOption(s, &name, &value);
		if (!value)
		{
			ereport(WARNING,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("could not parse setting for parameter \"%s\"",
							name)));
			free(name);
			continue;
		}

		(void) set_config_option(name, value, context, source, action, true);

		free(name);
		if (value)
			free(value);
	}
}


/*
 * Add an entry to an option array.  The array parameter may be NULL
 * to indicate the current table entry is NULL.
 */
ArrayType *
GUCArrayAdd(ArrayType *array, const char *name, const char *value)
{
	const char *varname;
	Datum		datum;
	char	   *newval;
	ArrayType  *a;

	Assert(name);
	Assert(value);

	/* test if the option is valid */
	set_config_option(name, value,
					  superuser() ? PGC_SUSET : PGC_USERSET,
					  PGC_S_TEST, GUC_ACTION_SET, false);

	/* convert name to canonical spelling, so we can use plain strcmp */
	(void) GetConfigOptionByName(name, &varname);
	name = varname;

	newval = palloc(strlen(name) + 1 + strlen(value) + 1);
	sprintf(newval, "%s=%s", name, value);
	datum = CStringGetTextDatum(newval);

	if (array)
	{
		int			index;
		bool		isnull;
		int			i;

		Assert(ARR_ELEMTYPE(array) == TEXTOID);
		Assert(ARR_NDIM(array) == 1);
		Assert(ARR_LBOUND(array)[0] == 1);

		index = ARR_DIMS(array)[0] + 1; /* add after end */

		for (i = 1; i <= ARR_DIMS(array)[0]; i++)
		{
			Datum		d;
			char	   *current;

			d = array_ref(array, 1, &i,
						  -1 /* varlenarray */ ,
						  -1 /* TEXT's typlen */ ,
						  false /* TEXT's typbyval */ ,
						  'i' /* TEXT's typalign */ ,
						  &isnull);
			if (isnull)
				continue;
			current = TextDatumGetCString(d);
			if (strncmp(current, newval, strlen(name) + 1) == 0)
			{
				index = i;
				break;
			}
		}

		a = array_set(array, 1, &index,
					  datum,
					  false,
					  -1 /* varlena array */ ,
					  -1 /* TEXT's typlen */ ,
					  false /* TEXT's typbyval */ ,
					  'i' /* TEXT's typalign */ );
	}
	else
		a = construct_array(&datum, 1,
							TEXTOID,
							-1, false, 'i');

	return a;
}


/*
 * Delete an entry from an option array.  The array parameter may be NULL
 * to indicate the current table entry is NULL.  Also, if the return value
 * is NULL then a null should be stored.
 */
ArrayType *
GUCArrayDelete(ArrayType *array, const char *name)
{
	const char *varname;
	ArrayType  *newarray;
	int			i;
	int			index;

	Assert(name);

	/* test if the option is valid */
	set_config_option(name, NULL,
					  superuser() ? PGC_SUSET : PGC_USERSET,
					  PGC_S_TEST, GUC_ACTION_SET, false);

	/* convert name to canonical spelling, so we can use plain strcmp */
	(void) GetConfigOptionByName(name, &varname);
	name = varname;

	/* if array is currently null, then surely nothing to delete */
	if (!array)
		return NULL;

	newarray = NULL;
	index = 1;

	for (i = 1; i <= ARR_DIMS(array)[0]; i++)
	{
		Datum		d;
		char	   *val;
		bool		isnull;

		d = array_ref(array, 1, &i,
					  -1 /* varlenarray */ ,
					  -1 /* TEXT's typlen */ ,
					  false /* TEXT's typbyval */ ,
					  'i' /* TEXT's typalign */ ,
					  &isnull);
		if (isnull)
			continue;
		val = TextDatumGetCString(d);

		/* ignore entry if it's what we want to delete */
		if (strncmp(val, name, strlen(name)) == 0
			&& val[strlen(name)] == '=')
			continue;

		/* else add it to the output array */
		if (newarray)
		{
			newarray = array_set(newarray, 1, &index,
								 d,
								 false,
								 -1 /* varlenarray */ ,
								 -1 /* TEXT's typlen */ ,
								 false /* TEXT's typbyval */ ,
								 'i' /* TEXT's typalign */ );
		}
		else
			newarray = construct_array(&d, 1,
									   TEXTOID,
									   -1, false, 'i');

		index++;
	}

	return newarray;
}


/*
 * assign_hook and show_hook subroutines
 */

static const char *
assign_log_destination(const char *value, bool doit, GucSource source)
{
	char	   *rawstring;
	List	   *elemlist;
	ListCell   *l;
	int			newlogdest = 0;

	/* Need a modifiable copy of string */
	rawstring = pstrdup(value);

	/* Parse string into list of identifiers */
	if (!SplitIdentifierString(rawstring, ',', &elemlist))
	{
		/* syntax error in list */
		pfree(rawstring);
		list_free(elemlist);
		ereport(GUC_complaint_elevel(source),
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			errmsg("invalid list syntax for parameter \"log_destination\"")));
		return NULL;
	}

	foreach(l, elemlist)
	{
		char	   *tok = (char *) lfirst(l);

		if (pg_strcasecmp(tok, "stderr") == 0)
			newlogdest |= LOG_DESTINATION_STDERR;
		else if (pg_strcasecmp(tok, "csvlog") == 0)
			newlogdest |= LOG_DESTINATION_CSVLOG;
#ifdef HAVE_SYSLOG
		else if (pg_strcasecmp(tok, "syslog") == 0)
			newlogdest |= LOG_DESTINATION_SYSLOG;
#endif
#ifdef WIN32
		else if (pg_strcasecmp(tok, "eventlog") == 0)
			newlogdest |= LOG_DESTINATION_EVENTLOG;
#endif
		else
		{
			ereport(GUC_complaint_elevel(source),
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				  errmsg("unrecognized \"log_destination\" key word: \"%s\"",
						 tok)));
			pfree(rawstring);
			list_free(elemlist);
			return NULL;
		}
	}

	if (doit)
		Log_destination = newlogdest;

	pfree(rawstring);
	list_free(elemlist);

	return value;
}

#ifdef HAVE_SYSLOG

static bool
assign_syslog_facility(int newval, bool doit, GucSource source)
{
	if (doit)
		set_syslog_parameters(syslog_ident_str ? syslog_ident_str : "postgres",
							  newval);

	return true;
}

static const char *
assign_syslog_ident(const char *ident, bool doit, GucSource source)
{
	if (doit)
		set_syslog_parameters(ident, syslog_facility);

	return ident;
}
#endif   /* HAVE_SYSLOG */


static bool
assign_session_replication_role(int newval, bool doit, GucSource source)
{
	/*
	 * Must flush the plan cache when changing replication role; but don't
	 * flush unnecessarily.
	 */
	if (doit && SessionReplicationRole != newval)
	{
		ResetPlanCache();
	}

	return true;
}

static const char *
show_num_temp_buffers(void)
{
	/*
	 * We show the GUC var until local buffers have been initialized, and
	 * NLocBuffer afterwards.
	 */
	static char nbuf[32];

	sprintf(nbuf, "%d", NLocBuffer ? NLocBuffer : num_temp_buffers);
	return nbuf;
}

static bool
assign_phony_autocommit(bool newval, bool doit, GucSource source)
{
	if (!newval)
	{
		ereport(GUC_complaint_elevel(source),
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("SET AUTOCOMMIT TO OFF is no longer supported")));
		return false;
	}
	return true;
}

static const char *
assign_custom_variable_classes(const char *newval, bool doit, GucSource source)
{
	/*
	 * Check syntax. newval must be a comma separated list of identifiers.
	 * Whitespace is allowed but removed from the result.
	 */
	bool		hasSpaceAfterToken = false;
	const char *cp = newval;
	int			symLen = 0;
	char		c;
	StringInfoData buf;

	initStringInfo(&buf);
	while ((c = *cp++) != '\0')
	{
		if (isspace((unsigned char) c))
		{
			if (symLen > 0)
				hasSpaceAfterToken = true;
			continue;
		}

		if (c == ',')
		{
			if (symLen > 0)		/* terminate identifier */
			{
				appendStringInfoChar(&buf, ',');
				symLen = 0;
			}
			hasSpaceAfterToken = false;
			continue;
		}

		if (hasSpaceAfterToken || !(isalnum((unsigned char) c) || c == '_'))
		{
			/*
			 * Syntax error due to token following space after token or
			 * non-identifier character
			 */
			pfree(buf.data);
			return NULL;
		}
		appendStringInfoChar(&buf, c);
		symLen++;
	}

	/* Remove stray ',' at end */
	if (symLen == 0 && buf.len > 0)
		buf.data[--buf.len] = '\0';

	/* GUC wants the result malloc'd */
	newval = guc_strdup(LOG, buf.data);

	pfree(buf.data);
	return newval;
}

static bool
assign_debug_assertions(bool newval, bool doit, GucSource source)
{
#ifndef USE_ASSERT_CHECKING
	if (newval)
	{
		ereport(GUC_complaint_elevel(source),
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			   errmsg("assertion checking is not supported by this build")));
		return false;
	}
#endif
	return true;
}

static bool
assign_ssl(bool newval, bool doit, GucSource source)
{
#ifndef USE_SSL
	if (newval)
	{
		ereport(GUC_complaint_elevel(source),
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("SSL is not supported by this build")));
		return false;
	}
#endif
	return true;
}

static bool
assign_stage_log_stats(bool newval, bool doit, GucSource source)
{
	if (newval && log_statement_stats)
	{
		ereport(GUC_complaint_elevel(source),
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot enable parameter when \"log_statement_stats\" is true")));
		/* source == PGC_S_OVERRIDE means do it anyway, eg at xact abort */
		if (source != PGC_S_OVERRIDE)
			return false;
	}
	return true;
}

static bool
assign_log_stats(bool newval, bool doit, GucSource source)
{
	if (newval &&
		(log_parser_stats || log_planner_stats || log_executor_stats))
	{
		ereport(GUC_complaint_elevel(source),
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot enable \"log_statement_stats\" when "
						"\"log_parser_stats\", \"log_planner_stats\", "
						"or \"log_executor_stats\" is true")));
		/* source == PGC_S_OVERRIDE means do it anyway, eg at xact abort */
		if (source != PGC_S_OVERRIDE)
			return false;
	}
	return true;
}

static bool
assign_transaction_read_only(bool newval, bool doit, GucSource source)
{
	/* Can't go to r/w mode inside a r/o transaction */
	if (newval == false && XactReadOnly && IsSubTransaction())
	{
		ereport(GUC_complaint_elevel(source),
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot set transaction read-write mode inside a read-only transaction")));
		/* source == PGC_S_OVERRIDE means do it anyway, eg at xact abort */
		if (source != PGC_S_OVERRIDE)
			return false;
	}
	return true;
}

static const char *
assign_canonical_path(const char *newval, bool doit, GucSource source)
{
	if (doit)
	{
		char	   *canon_val = guc_strdup(ERROR, newval);

		canonicalize_path(canon_val);
		return canon_val;
	}
	else
		return newval;
}

static const char *
assign_timezone_abbreviations(const char *newval, bool doit, GucSource source)
{
	/*
	 * The powerup value shown above for timezone_abbreviations is "UNKNOWN".
	 * When we see this we just do nothing.  If this value isn't overridden
	 * from the config file then pg_timezone_abbrev_initialize() will
	 * eventually replace it with "Default".  This hack has two purposes: to
	 * avoid wasting cycles loading values that might soon be overridden from
	 * the config file, and to avoid trying to read the timezone abbrev files
	 * during InitializeGUCOptions().  The latter doesn't work in an
	 * EXEC_BACKEND subprocess because my_exec_path hasn't been set yet and so
	 * we can't locate PGSHAREDIR.  (Essentially the same hack is used to
	 * delay initializing TimeZone ... if we have any more, we should try to
	 * clean up and centralize this mechanism ...)
	 */
	if (strcmp(newval, "UNKNOWN") == 0)
	{
		return newval;
	}

	/* Loading abbrev file is expensive, so only do it when value changes */
	if (timezone_abbreviations_string == NULL ||
		strcmp(timezone_abbreviations_string, newval) != 0)
	{
		int			elevel;

		/*
		 * If reading config file, only the postmaster should bleat loudly
		 * about problems.	Otherwise, it's just this one process doing it,
		 * and we use WARNING message level.
		 */
		if (source == PGC_S_FILE)
			elevel = IsUnderPostmaster ? DEBUG3 : LOG;
		else
			elevel = WARNING;
		if (!load_tzoffsets(newval, doit, elevel))
			return NULL;
	}
	return newval;
}

/*
 * pg_timezone_abbrev_initialize --- set default value if not done already
 *
 * This is called after initial loading of postgresql.conf.  If no
 * timezone_abbreviations setting was found therein, select default.
 */
void
pg_timezone_abbrev_initialize(void)
{
	if (strcmp(timezone_abbreviations_string, "UNKNOWN") == 0)
	{
		SetConfigOption("timezone_abbreviations", "Default",
						PGC_POSTMASTER, PGC_S_ARGV);
	}
}

static const char *
show_archive_command(void)
{
	if (XLogArchiveMode)
		return XLogArchiveCommand;
	else
		return "(disabled)";
}

static bool
assign_tcp_keepalives_idle(int newval, bool doit, GucSource source)
{
	if (doit)
		return (pq_setkeepalivesidle(newval, MyProcPort) == STATUS_OK);

	return true;
}

static const char *
show_tcp_keepalives_idle(void)
{
	static char nbuf[16];

	snprintf(nbuf, sizeof(nbuf), "%d", pq_getkeepalivesidle(MyProcPort));
	return nbuf;
}

static bool
assign_tcp_keepalives_interval(int newval, bool doit, GucSource source)
{
	if (doit)
		return (pq_setkeepalivesinterval(newval, MyProcPort) == STATUS_OK);

	return true;
}

static const char *
show_tcp_keepalives_interval(void)
{
	static char nbuf[16];

	snprintf(nbuf, sizeof(nbuf), "%d", pq_getkeepalivesinterval(MyProcPort));
	return nbuf;
}

static bool
assign_tcp_keepalives_count(int newval, bool doit, GucSource source)
{
	if (doit)
		return (pq_setkeepalivescount(newval, MyProcPort) == STATUS_OK);

	return true;
}

static const char *
show_tcp_keepalives_count(void)
{
	static char nbuf[16];

	snprintf(nbuf, sizeof(nbuf), "%d", pq_getkeepalivescount(MyProcPort));
	return nbuf;
}

static bool
assign_maxconnections(int newval, bool doit, GucSource source)
{
	if (newval + autovacuum_max_workers > INT_MAX / 4)
		return false;

	if (doit)
		MaxBackends = newval + autovacuum_max_workers;

	return true;
}

static bool
assign_autovacuum_max_workers(int newval, bool doit, GucSource source)
{
	if (newval + MaxConnections > INT_MAX / 4)
		return false;

	if (doit)
		MaxBackends = newval + MaxConnections;

	return true;
}

static bool
assign_effective_io_concurrency(int newval, bool doit, GucSource source)
{
#ifdef USE_PREFETCH
	double		new_prefetch_pages = 0.0;
	int			i;

	/*----------
	 * The user-visible GUC parameter is the number of drives (spindles),
	 * which we need to translate to a number-of-pages-to-prefetch target.
	 *
	 * The expected number of prefetch pages needed to keep N drives busy is:
	 *
	 * drives |   I/O requests
	 * -------+----------------
	 *      1 |   1
	 *      2 |   2/1 + 2/2 = 3
	 *      3 |   3/1 + 3/2 + 3/3 = 5 1/2
	 *      4 |   4/1 + 4/2 + 4/3 + 4/4 = 8 1/3
	 *      n |   n * H(n)
	 *
	 * This is called the "coupon collector problem" and H(n) is called the
	 * harmonic series.  This could be approximated by n * ln(n), but for
	 * reasonable numbers of drives we might as well just compute the series.
	 *
	 * Alternatively we could set the target to the number of pages necessary
	 * so that the expected number of active spindles is some arbitrary
	 * percentage of the total.  This sounds the same but is actually slightly
	 * different.  The result ends up being ln(1-P)/ln((n-1)/n) where P is
	 * that desired fraction.
	 *
	 * Experimental results show that both of these formulas aren't aggressive
	 * enough, but we don't really have any better proposals.
	 *
	 * Note that if newval = 0 (disabled), we must set target = 0.
	 *----------
	 */

	for (i = 1; i <= newval; i++)
		new_prefetch_pages += (double) newval / (double) i;

	/* This range check shouldn't fail, but let's be paranoid */
	if (new_prefetch_pages >= 0.0 && new_prefetch_pages < (double) INT_MAX)
	{
		if (doit)
			target_prefetch_pages = (int) rint(new_prefetch_pages);
		return true;
	}
	else
		return false;
#else
	return true;
#endif /* USE_PREFETCH */
}

static const char *
assign_pgstat_temp_directory(const char *newval, bool doit, GucSource source)
{
	if (doit)
	{
		if (pgstat_stat_tmpname)
			free(pgstat_stat_tmpname);
		if (pgstat_stat_filename)
			free(pgstat_stat_filename);

		pgstat_stat_tmpname = guc_malloc(FATAL, strlen(newval) + 12);  /* /pgstat.tmp */
		pgstat_stat_filename = guc_malloc(FATAL, strlen(newval) + 13); /* /pgstat.stat */

		sprintf(pgstat_stat_tmpname, "%s/pgstat.tmp", newval);
		sprintf(pgstat_stat_filename, "%s/pgstat.stat", newval);
	}

	return newval;
}

#include "guc-file.c"
