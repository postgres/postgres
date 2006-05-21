/*--------------------------------------------------------------------
 * guc.c
 *
 * Support for grand unified configuration scheme, including SET
 * command, configuration file, and command line options.
 * See src/backend/utils/misc/README for more information.
 *
 *
 * Copyright (c) 2000-2003, PostgreSQL Global Development Group
 * Written by Peter Eisentraut <peter_e@gmx.net>.
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/misc/guc.c,v 1.164.2.5 2006/05/21 20:11:58 tgl Exp $
 *
 *--------------------------------------------------------------------
 */
#include "postgres.h"

#include <errno.h>
#include <float.h>
#include <limits.h>
#include <unistd.h>

#include "utils/guc.h"
#include "utils/guc_tables.h"

#include "access/xlog.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/async.h"
#include "commands/variable.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "funcapi.h"
#include "libpq/auth.h"
#include "libpq/pqcomm.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "optimizer/cost.h"
#include "optimizer/geqo.h"
#include "optimizer/paths.h"
#include "optimizer/prep.h"
#include "parser/gramparse.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/scansup.h"
#include "storage/fd.h"
#include "storage/freespace.h"
#include "storage/lock.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/pg_locale.h"
#include "pgstat.h"


#ifndef PG_KRB_SRVTAB
#define PG_KRB_SRVTAB ""
#endif

#ifdef EXEC_BACKEND
#define CONFIG_EXEC_PARAMS "global/config_exec_params"
#endif

/* XXX these should appear in other modules' header files */
extern bool Log_connections;
extern bool check_function_bodies;
extern int	PreAuthDelay;
extern int	AuthenticationTimeout;
extern int	CheckPointTimeout;
extern int	CommitDelay;
extern int	CommitSiblings;
extern char *preload_libraries_string;

#ifdef HAVE_SYSLOG
extern char *Syslog_facility;
extern char *Syslog_ident;

static const char *assign_facility(const char *facility,
				bool doit, bool interactive);
#endif

static const char *assign_defaultxactisolevel(const char *newval,
						   bool doit, bool interactive);
static const char *assign_log_min_messages(const char *newval,
						bool doit, bool interactive);
static const char *assign_client_min_messages(const char *newval,
						   bool doit, bool interactive);
static const char *assign_min_error_statement(const char *newval, bool doit,
						   bool interactive);
static const char *assign_msglvl(int *var, const char *newval,
			  bool doit, bool interactive);
static const char *assign_log_error_verbosity(const char *newval, bool doit,
						   bool interactive);
static const char *assign_backslash_quote(const char *newval, bool doit,
										  bool interactive);
static bool assign_phony_autocommit(bool newval, bool doit, bool interactive);


/*
 * Debugging options
 */
#ifdef USE_ASSERT_CHECKING
bool		assert_enabled = true;
#endif
bool		log_statement = false;
bool		log_duration = false;
bool		Debug_print_plan = false;
bool		Debug_print_parse = false;
bool		Debug_print_rewritten = false;
bool		Debug_pretty_print = false;
bool		Explain_pretty_print = true;

bool		log_parser_stats = false;
bool		log_planner_stats = false;
bool		log_executor_stats = false;
bool		log_statement_stats = false;		/* this is sort of all
												 * three above together */
bool		log_btree_build_stats = false;

bool		SQL_inheritance = true;

bool		Australian_timezones = false;

bool		Password_encryption = true;

int			log_min_error_statement = PANIC;
int			log_min_messages = NOTICE;
int			client_min_messages = NOTICE;

int			log_min_duration_statement = -1;


/*
 * These variables are all dummies that don't do anything, except in some
 * cases provide the value for SHOW to display.  The real state is elsewhere
 * and is kept in sync by assign_hooks.
 */
static char *client_min_messages_str;
static char *log_min_messages_str;
static char *log_error_verbosity_str;
static char *log_min_error_statement_str;
static bool phony_autocommit;
static bool session_auth_is_superuser;
static double phony_random_seed;
static char *backslash_quote_string;
static char *client_encoding_string;
static char *datestyle_string;
static char *default_iso_level_string;
static char *locale_collate;
static char *locale_ctype;
static char *regex_flavor_string;
static char *server_encoding_string;
static char *server_version_string;
static char *timezone_string;
static char *XactIsoLevel_string;
/* should be static, but commands/variable.c needs to get at it */
char *session_authorization_string;


/* Macros for freeing malloc'd pointers only if appropriate to do so */
/* Some of these tests are probably redundant, but be safe ... */
#define SET_STRING_VARIABLE(rec, newval) \
	do { \
		if (*(rec)->variable && \
			*(rec)->variable != (rec)->reset_val && \
			*(rec)->variable != (rec)->session_val && \
			*(rec)->variable != (rec)->tentative_val) \
			free(*(rec)->variable); \
		*(rec)->variable = (newval); \
	} while (0)
#define SET_STRING_RESET_VAL(rec, newval) \
	do { \
		if ((rec)->reset_val && \
			(rec)->reset_val != *(rec)->variable && \
			(rec)->reset_val != (rec)->session_val && \
			(rec)->reset_val != (rec)->tentative_val) \
			free((rec)->reset_val); \
		(rec)->reset_val = (newval); \
	} while (0)
#define SET_STRING_SESSION_VAL(rec, newval) \
	do { \
		if ((rec)->session_val && \
			(rec)->session_val != *(rec)->variable && \
			(rec)->session_val != (rec)->reset_val && \
			(rec)->session_val != (rec)->tentative_val) \
			free((rec)->session_val); \
		(rec)->session_val = (newval); \
	} while (0)
#define SET_STRING_TENTATIVE_VAL(rec, newval) \
	do { \
		if ((rec)->tentative_val && \
			(rec)->tentative_val != *(rec)->variable && \
			(rec)->tentative_val != (rec)->reset_val && \
			(rec)->tentative_val != (rec)->session_val) \
			free((rec)->tentative_val); \
		(rec)->tentative_val = (newval); \
	} while (0)


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
	 /* PGC_USERLIMIT */ "userlimit",
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
	 /* PGC_S_UNPRIVILEGED */ "unprivileged",
	 /* PGC_S_DATABASE */ "database",
	 /* PGC_S_USER */ "user",
	 /* PGC_S_CLIENT */ "client",
	 /* PGC_S_OVERRIDE */ "override",
	 /* PGC_S_SESSION */ "session"
};

/*
 * Displayable names for the groupings defined in enum config_group
 */
const char *const config_group_names[] =
{
	/* UNGROUPED */
	gettext_noop("Ungrouped"),
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
	/* RESOURCES_FSM */
	gettext_noop("Resource Usage / Free Space Map"),
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
	gettext_noop("Query Tuning / Planner Method Enabling"),
	/* QUERY_TUNING_COST */
	gettext_noop("Query Tuning / Planner Cost Constants"),
	/* QUERY_TUNING_GEQO */
	gettext_noop("Query Tuning / Genetic Query Optimizer"),
	/* QUERY_TUNING_OTHER */
	gettext_noop("Query Tuning / Other Planner Options"),
	/* LOGGING */
	gettext_noop("Reporting and Logging"),
	/* LOGGING_SYSLOG */
	gettext_noop("Reporting and Logging / Syslog"),
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
	 /* PGC_STRING */ "string"
};


/*
 * Contents of GUC tables
 *
 * See src/backend/utils/misc/README for design notes.
 *
 * TO ADD AN OPTION:
 *
 * 1. Declare a global variable of type bool, int, double, or char*
 * and make use of it.
 *
 * 2. Decide at what times it's safe to set the option. See guc.h for
 * details.
 *
 * 3. Decide on a name, a default value, upper and lower bounds (if
 * applicable), etc.
 *
 * 4. Add a record below.
 *
 * 5. Add it to src/backend/utils/misc/postgresql.conf.sample.
 *
 * 6. Add it to src/bin/psql/tab-complete.c, if it's a USERSET option.
 *
 * 7. Don't forget to document the option.
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
		{"tcpip_socket", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
			gettext_noop("Makes the server accept TCP/IP connections."),
			NULL
		},
		&NetServer,
		false, NULL, NULL
	},
	{
		{"ssl", PGC_POSTMASTER, CONN_AUTH_SECURITY,
			gettext_noop("Enables SSL connections."),
			NULL
		},
		&EnableSSL,
		false, NULL, NULL
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
		{"silent_mode", PGC_POSTMASTER, LOGGING_WHEN,
			gettext_noop("Runs the server silently."),
			gettext_noop("If this parameter is set, the server will automatically run in the "
			"background and any controlling terminals are dissociated.")
		},
		&SilentMode,
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
		{"log_timestamp", PGC_SIGHUP, LOGGING_WHAT,
			gettext_noop("Prefixes server log messages with a time stamp."),
			NULL
		},
		&Log_timestamp,
		false, NULL, NULL
	},
	{
		{"log_pid", PGC_SIGHUP, LOGGING_WHAT,
			gettext_noop("Prefixes server log messages with the server PID."),
			NULL
		},
		&Log_pid,
		false, NULL, NULL
	},

#ifdef USE_ASSERT_CHECKING
	{
		{"debug_assertions", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Turns on various assertion checks."),
			gettext_noop("This is a debugging aid."),
			GUC_NOT_IN_SAMPLE
		},
		&assert_enabled,
		true, NULL, NULL
	},
#endif

	{
		/* currently undocumented, so don't show in SHOW ALL */
		{"exit_on_error", PGC_USERSET, UNGROUPED,
			gettext_noop("no description available"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE
		},
		&ExitOnAnyError,
		false, NULL, NULL
	},
	{
		{"log_statement", PGC_USERLIMIT, LOGGING_WHAT,
			gettext_noop("Logs each SQL statement."),
			NULL
		},
		&log_statement,
		false, NULL, NULL
	},
	{
		{"log_duration", PGC_USERLIMIT, LOGGING_WHAT,
			gettext_noop("Logs the duration each completed SQL statement."),
			NULL
		},
		&log_duration,
		false, NULL, NULL
	},
	{
		{"debug_print_parse", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Prints the parse tree to the server log."),
			NULL
		},
		&Debug_print_parse,
		false, NULL, NULL
	},
	{
		{"debug_print_rewritten", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Prints the parse tree after rewriting to server log."),
			NULL
		},
		&Debug_print_rewritten,
		false, NULL, NULL
	},
	{
		{"debug_print_plan", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Prints the execution plan to server log."),
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
		false, NULL, NULL
	},
	{
		{"log_parser_stats", PGC_USERLIMIT, STATS_MONITORING,
			gettext_noop("Writes parser performance statistics to the server log."),
			NULL
		},
		&log_parser_stats,
		false, NULL, NULL
	},
	{
		{"log_planner_stats", PGC_USERLIMIT, STATS_MONITORING,
			gettext_noop("Writes planner performance statistics to the server log."),
			NULL
		},
		&log_planner_stats,
		false, NULL, NULL
	},
	{
		{"log_executor_stats", PGC_USERLIMIT, STATS_MONITORING,
			gettext_noop("Writes executor performance statistics to the server log."),
			NULL
		},
		&log_executor_stats,
		false, NULL, NULL
	},
	{
		{"log_statement_stats", PGC_USERLIMIT, STATS_MONITORING,
			gettext_noop("Writes cumulative performance statistics to the server log."),
			NULL
		},
		&log_statement_stats,
		false, NULL, NULL
	},
#ifdef BTREE_BUILD_STATS
	{
		{"log_btree_build_stats", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("no description available"),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&log_btree_build_stats,
		false, NULL, NULL
	},
#endif

	{
		{"explain_pretty_print", PGC_USERSET, CLIENT_CONN_OTHER,
			gettext_noop("Uses the indented output format for EXPLAIN VERBOSE."),
			NULL
		},
		&Explain_pretty_print,
		true, NULL, NULL
	},
	{
		{"stats_start_collector", PGC_POSTMASTER, STATS_COLLECTOR,
			gettext_noop("Starts the server statistics-collection subprocess."),
			NULL
		},
		&pgstat_collect_startcollector,
		true, NULL, NULL
	},
	{
		{"stats_reset_on_server_start", PGC_POSTMASTER, STATS_COLLECTOR,
			gettext_noop("Zeroes collected statistics on server restart."),
			NULL
		},
		&pgstat_collect_resetonpmstart,
		true, NULL, NULL
	},
	{
		{"stats_command_string", PGC_SUSET, STATS_COLLECTOR,
			gettext_noop("Collects statistics about executing commands."),
			gettext_noop("Enables the collection of statistics on the currently "
				"executing command of each session, along with the time "
						 "at which that command began execution.")
		},
		&pgstat_collect_querystring,
		false, NULL, NULL
	},
	{
		{"stats_row_level", PGC_SUSET, STATS_COLLECTOR,
			gettext_noop("Collects row-level statistics on database activity."),
			NULL
		},
		&pgstat_collect_tuplelevel,
		false, NULL, NULL
	},
	{
		{"stats_block_level", PGC_SUSET, STATS_COLLECTOR,
			gettext_noop("Collects block-level statistics on database activity."),
			NULL
		},
		&pgstat_collect_blocklevel,
		false, NULL, NULL
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
			gettext_noop("no description available"),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&Trace_locks,
		false, NULL, NULL
	},
	{
		{"trace_userlocks", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("no description available"),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&Trace_userlocks,
		false, NULL, NULL
	},
	{
		{"trace_lwlocks", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("no description available"),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&Trace_lwlocks,
		false, NULL, NULL
	},
	{
		{"debug_deadlocks", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("no description available"),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&Debug_deadlocks,
		false, NULL, NULL
	},
#endif

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
		{"log_source_port", PGC_SIGHUP, LOGGING_WHAT,
			gettext_noop("Logs the outgoing port number of the connecting host."),
			NULL
		},
		&LogSourcePort,
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
		{"australian_timezones", PGC_USERSET, CLIENT_CONN_LOCALE,
			gettext_noop("Interprets ACST, CST, EST, and SAT as Australian time zones."),
			gettext_noop("Otherwise they are interpreted as North/South American "
						 "time zones and Saturday.")
		},
		&Australian_timezones,
		false, ClearDateCache, NULL
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
			gettext_noop("Shows the current transaction's read-only status."),
			NULL,
			GUC_NO_RESET_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&XactReadOnly,
		false, NULL, NULL
	},
	{
		{"add_missing_from", PGC_USERSET, COMPAT_OPTIONS_PREVIOUS,
			gettext_noop("Automatically adds missing table references to FROM clauses."),
			NULL
		},
		&add_missing_from,
		true, NULL, NULL
	},
	{
		{"check_function_bodies", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Check function bodies during CREATE FUNCTION."),
			NULL
		},
		&check_function_bodies,
		true, NULL, NULL
	},

	/* End-of-list marker */
	{
		{NULL, 0, 0, NULL, NULL}, NULL, false, NULL, NULL
	}
};


static struct config_int ConfigureNamesInt[] =
{
	{
		{"default_statistics_target", PGC_USERSET, QUERY_TUNING_OTHER,
			gettext_noop("Sets the default statistics target."),
			gettext_noop("This applies to table columns that have not had a "
			 "column-specific target set via ALTER TABLE SET STATISTICS.")
		},
		&default_statistics_target,
		10, 1, 1000, NULL, NULL
	},
	{
		{"from_collapse_limit", PGC_USERSET, QUERY_TUNING_OTHER,
			gettext_noop("Sets the FROM-list size beyond which subqueries are not "
						 "collapsed."),
			gettext_noop("The planner will merge subqueries into upper "
			"queries if the resulting FROM list would have no more than "
						 "this many items.")
		},
		&from_collapse_limit,
		8, 1, INT_MAX, NULL, NULL
	},
	{
		{"join_collapse_limit", PGC_USERSET, QUERY_TUNING_OTHER,
			gettext_noop("Sets the FROM-list size beyond which JOIN constructs are not "
						 "flattened."),
			gettext_noop("The planner will flatten explicit inner JOIN "
						 "constructs into lists of FROM items whenever a list of no more "
						 "than this many items would result.")
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
		11, 2, INT_MAX, NULL, NULL
	},
	{
		{"geqo_pool_size", PGC_USERSET, QUERY_TUNING_GEQO,
			gettext_noop("GEQO: number of individuals in one population."),
			NULL
		},
		&Geqo_pool_size,
		DEFAULT_GEQO_POOL_SIZE, 0, MAX_GEQO_POOL_SIZE, NULL, NULL
	},
	{
		{"geqo_effort", PGC_USERSET, QUERY_TUNING_GEQO,
			gettext_noop("GEQO: effort is used to calculate a default for generations."),
			NULL
		},
		&Geqo_effort,
		1, 1, INT_MAX, NULL, NULL
	},
	{
		{"geqo_generations", PGC_USERSET, QUERY_TUNING_GEQO,
			gettext_noop("GEQO: number of iterations in the algorithm."),
			gettext_noop("The number must be a positive integer. If 0 is "
						 "specified then effort * log2(poolsize) is used.")
		},
		&Geqo_generations,
		0, 0, INT_MAX, NULL, NULL
	},

	{
		{"deadlock_timeout", PGC_SIGHUP, LOCK_MANAGEMENT,
			gettext_noop("The time in milliseconds to wait on lock before checking for deadlock."),
			NULL
		},
		&DeadlockTimeout,
		1000, 0, INT_MAX, NULL, NULL
	},

#ifdef HAVE_SYSLOG
	{
		{"syslog", PGC_SIGHUP, LOGGING_SYSLOG,
			gettext_noop("Uses syslog for logging."),
			gettext_noop("If this parameter is 1, messages go both to syslog "
						 "and the standard output. A value of 2 sends output only to syslog. "
						 "(Some messages will still go to the standard output/error.) The "
						 "default is 0, which means syslog is off.")
		},
		&Use_syslog,
		0, 0, 2, NULL, NULL
	},
#endif

	/*
	 * Note: There is some postprocessing done in PostmasterMain() to make
	 * sure the buffers are at least twice the number of backends, so the
	 * constraints here are partially unused. Similarly, the superuser
	 * reserved number is checked to ensure it is less than the max
	 * backends number.
	 */
	{
		{"max_connections", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
			gettext_noop("Sets the maximum number of concurrent connections."),
			NULL
		},
		&MaxBackends,
		100, 1, INT_MAX, NULL, NULL
	},

	{
		{"superuser_reserved_connections", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
			gettext_noop("Sets the number of connection slots reserved for superusers."),
			NULL
		},
		&ReservedBackends,
		2, 0, INT_MAX, NULL, NULL
	},

	{
		{"shared_buffers", PGC_POSTMASTER, RESOURCES_MEM,
			gettext_noop("Sets the number of shared memory buffers used by the server."),
			NULL
		},
		&NBuffers,
		1000, 16, INT_MAX, NULL, NULL
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
						 "permission set. The parameter value is expected to be an numeric mode "
						 "specification in the form accepted by the chmod and umask system "
						 "calls. (To use the customary octal format the number must start with "
						 "a 0 (zero).)")
		},
		&Unix_socket_permissions,
		0777, 0000, 0777, NULL, NULL
	},

	{
		{"sort_mem", PGC_USERSET, RESOURCES_MEM,
			gettext_noop("Sets the maximum memory to be used for sorts and hash tables."),
			gettext_noop("Specifies the amount of memory to be used by internal "
						 "sort operations and hash tables before switching to temporary disk "
						 "files")
		},
		&SortMem,
		1024, 8 * BLCKSZ / 1024, INT_MAX, NULL, NULL
	},

	{
		{"vacuum_mem", PGC_USERSET, RESOURCES_MEM,
			gettext_noop("Sets the maximum memory used to keep track of to-be-reclaimed rows."),
			NULL
		},
		&VacuumMem,
		8192, 1024, INT_MAX, NULL, NULL
	},

	{
		{"max_files_per_process", PGC_POSTMASTER, RESOURCES_KERNEL,
			gettext_noop("Sets the maximum number of simultaneously open files for each server process."),
			NULL
		},
		&max_files_per_process,
		1000, 25, INT_MAX, NULL, NULL
	},

#ifdef LOCK_DEBUG
	{
		{"trace_lock_oidmin", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("no description available"),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&Trace_lock_oidmin,
		BootstrapObjectIdData, 1, INT_MAX, NULL, NULL
	},
	{
		{"trace_lock_table", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("no description available"),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&Trace_lock_table,
		0, 0, INT_MAX, NULL, NULL
	},
#endif
	{
		{"max_expr_depth", PGC_USERSET, CLIENT_CONN_OTHER,
			gettext_noop("Sets the maximum expression nesting depth."),
			NULL
		},
		&max_expr_depth,
		DEFAULT_MAX_EXPR_DEPTH, 10, INT_MAX, NULL, NULL
	},

	{
		{"statement_timeout", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets the maximum allowed duration (in milliseconds) of any statement."),
			gettext_noop("A value of 0 turns off the timeout.")
		},
		&StatementTimeout,
		0, 0, INT_MAX, NULL, NULL
	},

	{
		{"max_fsm_relations", PGC_POSTMASTER, RESOURCES_FSM,
			gettext_noop("Sets the maximum number of tables and indexes for which free space is "
						 "tracked."),
			NULL
		},
		&MaxFSMRelations,
		1000, 100, INT_MAX, NULL, NULL
	},
	{
		{"max_fsm_pages", PGC_POSTMASTER, RESOURCES_FSM,
			gettext_noop("Sets the maximum number of disk pages for which free space is "
						 "tracked."),
			NULL
		},
		&MaxFSMPages,
		20000, 1000, INT_MAX, NULL, NULL
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
			gettext_noop("Sets the maximum time in seconds to complete client authentication."),
			NULL
		},
		&AuthenticationTimeout,
		60, 1, 600, NULL, NULL
	},

	{
		/* Not for general use */
		{"pre_auth_delay", PGC_SIGHUP, DEVELOPER_OPTIONS,
			gettext_noop("no description available"),
			NULL,
			GUC_NOT_IN_SAMPLE
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
			gettext_noop("Sets the maximum time in seconds between automatic WAL checkpoints."),
			NULL
		},
		&CheckPointTimeout,
		300, 30, 3600, NULL, NULL
	},

	{
		{"checkpoint_warning", PGC_SIGHUP, WAL_CHECKPOINTS,
			gettext_noop("Logs if filling of checkpoint segments happens more "
						 "frequently than this (in seconds)."),
			gettext_noop("Write a message to the server log if checkpoints "
						 "caused by the filling of checkpoint segment files happens more "
						 "frequently than this number of seconds. Zero turns off the warning.")
		},
		&CheckPointWarning,
		30, 0, INT_MAX, NULL, NULL
	},

	{
		{"wal_buffers", PGC_POSTMASTER, WAL_SETTINGS,
			gettext_noop("Sets the number of disk-page buffers in shared memory for WAL."),
			NULL
		},
		&XLOGbuffers,
		8, 4, INT_MAX, NULL, NULL
	},

	{
		{"wal_debug", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("If nonzero, WAL-related debugging output is logged."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&XLOG_DEBUG,
		0, 0, 16, NULL, NULL
	},

	{
		{"commit_delay", PGC_USERSET, WAL_CHECKPOINTS,
			gettext_noop("Sets the delay in microseconds between transaction commit and "
						 "flushing WAL to disk."),
			NULL
		},
		&CommitDelay,
		0, 0, 100000, NULL, NULL
	},

	{
		{"commit_siblings", PGC_USERSET, WAL_CHECKPOINTS,
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
		{"log_min_duration_statement", PGC_USERLIMIT, LOGGING_WHEN,
			gettext_noop("Sets the minimum execution time in milliseconds above which statements will "
						 "be logged."),
			gettext_noop("Zero prints all queries. The default is -1 (turning this feature off).")
		},
		&log_min_duration_statement,
		-1, -1, INT_MAX / 1000, NULL, NULL
	},

	/* End-of-list marker */
	{
		{NULL, 0, 0, NULL, NULL}, NULL, 0, 0, 0, NULL, NULL
	}
};


static struct config_real ConfigureNamesReal[] =
{
	{
		{"effective_cache_size", PGC_USERSET, QUERY_TUNING_COST,
			gettext_noop("Sets the planner's assumption about size of the disk cache."),
			gettext_noop("That is, the portion of the kernel's disk cache that "
						 "will be used for PostgreSQL data files. This is measured in disk "
						 "pages, which are normally 8 kB each.")
		},
		&effective_cache_size,
		DEFAULT_EFFECTIVE_CACHE_SIZE, 0, DBL_MAX, NULL, NULL
	},
	{
		{"random_page_cost", PGC_USERSET, QUERY_TUNING_COST,
			gettext_noop("Sets the planner's estimate of the cost of a nonsequentially "
						 "fetched disk page."),
			gettext_noop("This is measured as a multiple of the cost of a "
						 "sequential page fetch. A higher value makes it more likely a "
						 "sequential scan will be used, a lower value makes it more likely an "
						 "index scan will be used.")
		},
		&random_page_cost,
		DEFAULT_RANDOM_PAGE_COST, 0, DBL_MAX, NULL, NULL
	},
	{
		{"cpu_tuple_cost", PGC_USERSET, QUERY_TUNING_COST,
			gettext_noop("Sets the planner's estimate of the cost of processing each tuple (row)."),
			gettext_noop("This is measured as a fraction of the cost of a "
						 "sequential page fetch.")
		},
		&cpu_tuple_cost,
		DEFAULT_CPU_TUPLE_COST, 0, DBL_MAX, NULL, NULL
	},
	{
		{"cpu_index_tuple_cost", PGC_USERSET, QUERY_TUNING_COST,
			gettext_noop("Sets the planner's estimate of processing cost for each "
						 "index tuple (row) during index scan."),
			gettext_noop("This is measured as a fraction of the cost of a "
						 "sequential page fetch.")
		},
		&cpu_index_tuple_cost,
		DEFAULT_CPU_INDEX_TUPLE_COST, 0, DBL_MAX, NULL, NULL
	},
	{
		{"cpu_operator_cost", PGC_USERSET, QUERY_TUNING_COST,
			gettext_noop("Sets the planner's estimate of processing cost of each operator in WHERE."),
			gettext_noop("This is measured as a fraction of the cost of a sequential "
						 "page fetch.")
		},
		&cpu_operator_cost,
		DEFAULT_CPU_OPERATOR_COST, 0, DBL_MAX, NULL, NULL
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
		{"seed", PGC_USERSET, UNGROUPED,
			gettext_noop("Sets the seed for random-number generation."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NO_RESET_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&phony_random_seed,
		0.5, 0.0, 1.0, assign_random_seed, show_random_seed
	},

	/* End-of-list marker */
	{
		{NULL, 0, 0, NULL, NULL}, NULL, 0.0, 0.0, 0.0, NULL, NULL
	}
};


static struct config_string ConfigureNamesString[] =
{
	{
		{"backslash_quote", PGC_USERSET, COMPAT_OPTIONS_PREVIOUS,
			gettext_noop("Sets whether \"\\'\" is allowed in string literals."),
			gettext_noop("Valid values are ON, OFF, and SAFE_ENCODING.")
		},
		&backslash_quote_string,
		"safe_encoding", assign_backslash_quote, NULL
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
		{"client_min_messages", PGC_USERSET, LOGGING_WHEN,
			gettext_noop("Sets the message levels that are sent to the client."),
			gettext_noop("Valid values are DEBUG5, DEBUG4, DEBUG3, DEBUG2, "
						 "DEBUG1, LOG, NOTICE, WARNING, and ERROR. Each level includes all the "
						 "levels that follow it. The later the level, the fewer messages are "
						 "sent.")
		},
		&client_min_messages_str,
		"notice", assign_client_min_messages, NULL
	},

	{
		{"log_min_messages", PGC_USERLIMIT, LOGGING_WHEN,
			gettext_noop("Sets the message levels that are logged."),
			gettext_noop("Valid values are DEBUG5, DEBUG4, DEBUG3, DEBUG2, DEBUG1, "
						 "INFO, NOTICE, WARNING, ERROR, LOG, FATAL, and PANIC. Each level "
						 "includes all the levels that follow it.")
		},
		&log_min_messages_str,
		"notice", assign_log_min_messages, NULL
	},

	{
		{"log_error_verbosity", PGC_SUSET, LOGGING_WHEN,
			gettext_noop("Sets the verbosity of logged messages."),
			gettext_noop("Valid values are \"terse\", \"default\", and \"verbose\".")
		},
		&log_error_verbosity_str,
		"default", assign_log_error_verbosity, NULL
	},

	{
		{"log_min_error_statement", PGC_USERLIMIT, LOGGING_WHEN,
			gettext_noop("Causes all statements generating error at or above this level to be logged."),
			gettext_noop("All SQL statements that cause an error of the "
						 "specified level or a higher level are logged.")
		},
		&log_min_error_statement_str,
		"panic", assign_min_error_statement, NULL
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
		{"default_transaction_isolation", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets the transaction isolation level of each new transaction."),
			gettext_noop("Each SQL transaction has an isolation level, which "
				 "can be either \"read committed\" or \"serializable\".")
		},
		&default_iso_level_string,
		"read committed", assign_defaultxactisolevel, NULL
	},

	{
		{"dynamic_library_path", PGC_SUSET, CLIENT_CONN_OTHER,
			gettext_noop("Sets the path for dynamically loadable modules."),
			gettext_noop("If a dynamically loadable module needs to be opened and "
						 "the specified name does not have a directory component (i.e., the "
						 "name does not contain a slash), the system will search this path for "
						 "the specified file.")
		},
		&Dynamic_library_path,
		"$libdir", NULL, NULL
	},

	{
		{"krb_server_keyfile", PGC_POSTMASTER, CONN_AUTH_SECURITY,
			gettext_noop("Sets the location of the Kerberos server key file."),
			NULL
		},
		&pg_krb_server_keyfile,
		PG_KRB_SRVTAB, NULL, NULL
	},

	{
		{"rendezvous_name", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
			gettext_noop("Sets the Rendezvous broadcast service name."),
			NULL
		},
		&rendezvous_name,
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
		{"preload_libraries", PGC_POSTMASTER, RESOURCES_KERNEL,
			gettext_noop("Lists shared libraries to preload into server."),
			NULL,
			GUC_LIST_INPUT | GUC_LIST_QUOTE
		},
		&preload_libraries_string,
		"", NULL, NULL
	},

	{
		{"regex_flavor", PGC_USERSET, COMPAT_OPTIONS_PREVIOUS,
			gettext_noop("Sets the regular expression \"flavor\"."),
			gettext_noop("This can be set to advanced, extended, or basic.")
		},
		&regex_flavor_string,
		"advanced", assign_regex_flavor, NULL
	},

	{
		{"search_path", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets the schema search order for names that are not schema-qualified."),
			NULL,
			GUC_LIST_INPUT | GUC_LIST_QUOTE
		},
		&namespace_search_path,
		"$user,public", assign_search_path, NULL
	},

	{
		/* Can't be set in postgresql.conf */
		{"server_encoding", PGC_INTERNAL, CLIENT_CONN_LOCALE,
			gettext_noop("Sets the server (database) character set encoding."),
			NULL,
			GUC_IS_NAME | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&server_encoding_string,
		"SQL_ASCII", NULL, NULL
	},

	{
		/* Can't be set in postgresql.conf */
		{"server_version", PGC_INTERNAL, UNGROUPED,
			gettext_noop("Shows the server version."),
			NULL,
			GUC_REPORT | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&server_version_string,
		PG_VERSION, NULL, NULL
	},

	{
		/* Not for general use --- used by SET SESSION AUTHORIZATION */
		{"session_authorization", PGC_USERSET, UNGROUPED,
			gettext_noop("Shows the session user name."),
			NULL,
			GUC_IS_NAME | GUC_REPORT | GUC_NO_SHOW_ALL | GUC_NO_RESET_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&session_authorization_string,
		NULL, assign_session_authorization, show_session_authorization
	},

#ifdef HAVE_SYSLOG
	{
		{"syslog_facility", PGC_POSTMASTER, LOGGING_SYSLOG,
			gettext_noop("Sets the syslog \"facility\" to be used when syslog enabled."),
			gettext_noop("Valid values are LOCAL0, LOCAL1, LOCAL2, LOCAL3, "
						 "LOCAL4, LOCAL5, LOCAL6, LOCAL7.")
		},
		&Syslog_facility,
		"LOCAL0", assign_facility, NULL
	},
	{
		{"syslog_ident", PGC_POSTMASTER, LOGGING_SYSLOG,
			gettext_noop("Sets the program name used to identify PostgreSQL messages "
						 "in syslog."),
			NULL
		},
		&Syslog_ident,
		"postgres", NULL, NULL
	},
#endif

	{
		{"TimeZone", PGC_USERSET, CLIENT_CONN_LOCALE,
			gettext_noop("Sets the time zone for displaying and interpreting time stamps."),
			NULL
		},
		&timezone_string,
		"UNKNOWN", assign_timezone, show_timezone
	},

	{
		{"transaction_isolation", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Shows the current transaction's isolation level."),
			NULL,
			GUC_NO_RESET_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&XactIsoLevel_string,
		NULL, assign_XactIsoLevel, show_XactIsoLevel
	},

	{
		{"unix_socket_group", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
			gettext_noop("Sets the owning group of the Unix-domain socket."),
			gettext_noop("(The owning user of the socket is always the user "
						 "that starts the server.)")
		},
		&Unix_socket_group,
		"", NULL, NULL
	},

	{
		{"unix_socket_directory", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
			gettext_noop("Sets the directory where the Unix-domain socket will be created."),
			NULL
		},
		&UnixSocketDir,
		"", NULL, NULL
	},

	{
		{"virtual_host", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
			gettext_noop("Sets the host name or IP address to listen to."),
			NULL
		},
		&VirtualHost,
		"", NULL, NULL
	},

	{
		{"wal_sync_method", PGC_SIGHUP, WAL_SETTINGS,
			gettext_noop("Selects the method used for forcing WAL updates out to disk."),
			NULL
		},
		&XLOG_sync_method,
		XLOG_sync_method_default, assign_xlog_sync_method, NULL
	},

	/* End-of-list marker */
	{
		{NULL, 0, 0, NULL, NULL}, NULL, NULL, NULL, NULL
	}
};

/******** end of options list ********/


/*
 * Actual lookup of variables is done through this single, sorted array.
 */
struct config_generic **guc_variables;
int			num_guc_variables;

static bool guc_dirty;			/* TRUE if need to do commit/abort work */

static bool reporting_enabled;	/* TRUE to enable GUC_REPORT */

static char *guc_string_workspace;		/* for avoiding memory leaks */


static int	guc_var_compare(const void *a, const void *b);
static void ReportGUCOption(struct config_generic * record);
static char *_ShowOption(struct config_generic * record);


/*
 * Build the sorted array.	This is split out so that it could be
 * re-executed after startup (eg, we could allow loadable modules to
 * add vars, and then we'd need to re-sort).
 */
void
build_guc_variables(void)
{
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

	guc_vars = (struct config_generic **)
		malloc(num_vars * sizeof(struct config_generic *));
	if (!guc_vars)
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	num_vars = 0;

	for (i = 0; ConfigureNamesBool[i].gen.name; i++)
		guc_vars[num_vars++] = &ConfigureNamesBool[i].gen;

	for (i = 0; ConfigureNamesInt[i].gen.name; i++)
		guc_vars[num_vars++] = &ConfigureNamesInt[i].gen;

	for (i = 0; ConfigureNamesReal[i].gen.name; i++)
		guc_vars[num_vars++] = &ConfigureNamesReal[i].gen;

	for (i = 0; ConfigureNamesString[i].gen.name; i++)
		guc_vars[num_vars++] = &ConfigureNamesString[i].gen;

	qsort((void *) guc_vars, num_vars, sizeof(struct config_generic *),
		  guc_var_compare);

	if (guc_variables)
		free(guc_variables);
	guc_variables = guc_vars;
	num_guc_variables = num_vars;
}


/*
 * Look up option NAME. If it exists, return a pointer to its record,
 * else return NULL.
 */
static struct config_generic *
find_option(const char *name)
{
	const char **key = &name;
	struct config_generic **res;

	Assert(name);

	/*
	 * by equating const char ** with struct config_generic *, we are
	 * assuming the name field is first in config_generic.
	 */
	res = (struct config_generic **) bsearch((void *) &key,
											 (void *) guc_variables,
											 num_guc_variables,
										 sizeof(struct config_generic *),
											 guc_var_compare);
	if (res)
		return *res;
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
	const char *namea;
	const char *nameb;

	/*
	 * The temptation to use strcasecmp() here must be resisted, because
	 * the array ordering has to remain stable across setlocale() calls.
	 * So, build our own with a simple ASCII-only downcasing.
	 */
	namea = confa->name;
	nameb = confb->name;
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
 */
void
InitializeGUCOptions(void)
{
	int			i;
	char	   *env;

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
		struct config_generic *gconf = guc_variables[i];

		gconf->status = 0;
		gconf->reset_source = PGC_S_DEFAULT;
		gconf->session_source = PGC_S_DEFAULT;
		gconf->tentative_source = PGC_S_DEFAULT;
		gconf->source = PGC_S_DEFAULT;

		switch (gconf->vartype)
		{
			case PGC_BOOL:
				{
					struct config_bool *conf = (struct config_bool *) gconf;

					if (conf->assign_hook)
						if (!(*conf->assign_hook) (conf->reset_val, true, false))
							elog(FATAL, "failed to initialize %s to %d",
								 conf->gen.name, (int) conf->reset_val);
					*conf->variable = conf->reset_val;
					conf->session_val = conf->reset_val;
					break;
				}
			case PGC_INT:
				{
					struct config_int *conf = (struct config_int *) gconf;

					Assert(conf->reset_val >= conf->min);
					Assert(conf->reset_val <= conf->max);

					/*
					 * Check to make sure we only have valid
					 * PGC_USERLIMITs
					 */
					Assert(conf->gen.context != PGC_USERLIMIT ||
						   strcmp(conf->gen.name, "log_min_duration_statement") == 0);
					if (conf->assign_hook)
						if (!(*conf->assign_hook) (conf->reset_val, true, false))
							elog(FATAL, "failed to initialize %s to %d",
								 conf->gen.name, conf->reset_val);
					*conf->variable = conf->reset_val;
					conf->session_val = conf->reset_val;
					break;
				}
			case PGC_REAL:
				{
					struct config_real *conf = (struct config_real *) gconf;

					Assert(conf->reset_val >= conf->min);
					Assert(conf->reset_val <= conf->max);
					Assert(conf->gen.context != PGC_USERLIMIT);
					if (conf->assign_hook)
						if (!(*conf->assign_hook) (conf->reset_val, true, false))
							elog(FATAL, "failed to initialize %s to %g",
								 conf->gen.name, conf->reset_val);
					*conf->variable = conf->reset_val;
					conf->session_val = conf->reset_val;
					break;
				}
			case PGC_STRING:
				{
					struct config_string *conf = (struct config_string *) gconf;
					char	   *str;

					/*
					 * Check to make sure we only have valid
					 * PGC_USERLIMITs
					 */
					Assert(conf->gen.context != PGC_USERLIMIT ||
						   conf->assign_hook == assign_log_min_messages ||
					   conf->assign_hook == assign_client_min_messages ||
						conf->assign_hook == assign_min_error_statement);
					*conf->variable = NULL;
					conf->reset_val = NULL;
					conf->session_val = NULL;
					conf->tentative_val = NULL;

					if (conf->boot_val == NULL)
					{
						/* Cannot set value yet */
						break;
					}

					str = strdup(conf->boot_val);
					if (str == NULL)
						ereport(FATAL,
								(errcode(ERRCODE_OUT_OF_MEMORY),
								 errmsg("out of memory")));
					conf->reset_val = str;

					if (conf->assign_hook)
					{
						const char *newstr;

						newstr = (*conf->assign_hook) (str, true, false);
						if (newstr == NULL)
						{
							elog(FATAL, "failed to initialize %s to \"%s\"",
								 conf->gen.name, str);
						}
						else if (newstr != str)
						{
							free(str);

							/*
							 * See notes in set_config_option about
							 * casting
							 */
							str = (char *) newstr;
							conf->reset_val = str;
						}
					}
					*conf->variable = str;
					conf->session_val = str;
					break;
				}
		}
	}

	guc_dirty = false;

	reporting_enabled = false;

	guc_string_workspace = NULL;

	/*
	 * Prevent any attempt to override the transaction modes from
	 * non-interactive sources.
	 */
	SetConfigOption("transaction_isolation", "default",
					PGC_POSTMASTER, PGC_S_OVERRIDE);
	SetConfigOption("transaction_read_only", "no",
					PGC_POSTMASTER, PGC_S_OVERRIDE);

	/*
	 * For historical reasons, some GUC parameters can receive defaults
	 * from environment variables.	Process those settings.
	 */

	env = getenv("PGPORT");
	if (env != NULL)
		SetConfigOption("port", env, PGC_POSTMASTER, PGC_S_ENV_VAR);

	env = getenv("PGDATESTYLE");
	if (env != NULL)
		SetConfigOption("datestyle", env, PGC_POSTMASTER, PGC_S_ENV_VAR);

	env = getenv("TZ");
	if (env != NULL)
		SetConfigOption("timezone", env, PGC_POSTMASTER, PGC_S_ENV_VAR);

	env = getenv("PGCLIENTENCODING");
	if (env != NULL)
		SetConfigOption("client_encoding", env, PGC_POSTMASTER, PGC_S_ENV_VAR);
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
			gconf->context != PGC_USERLIMIT &&
			gconf->context != PGC_USERSET)
			continue;
		/* Don't reset if special exclusion from RESET ALL */
		if (gconf->flags & GUC_NO_RESET_ALL)
			continue;
		/* No need to reset if wasn't SET */
		if (gconf->source <= PGC_S_OVERRIDE)
			continue;

		switch (gconf->vartype)
		{
			case PGC_BOOL:
				{
					struct config_bool *conf = (struct config_bool *) gconf;

					if (conf->assign_hook)
						if (!(*conf->assign_hook) (conf->reset_val, true, true))
							elog(ERROR, "failed to reset %s", conf->gen.name);
					*conf->variable = conf->reset_val;
					conf->tentative_val = conf->reset_val;
					conf->gen.source = conf->gen.reset_source;
					conf->gen.tentative_source = conf->gen.reset_source;
					conf->gen.status |= GUC_HAVE_TENTATIVE;
					guc_dirty = true;
					break;
				}
			case PGC_INT:
				{
					struct config_int *conf = (struct config_int *) gconf;

					if (conf->assign_hook)
						if (!(*conf->assign_hook) (conf->reset_val, true, true))
							elog(ERROR, "failed to reset %s", conf->gen.name);
					*conf->variable = conf->reset_val;
					conf->tentative_val = conf->reset_val;
					conf->gen.source = conf->gen.reset_source;
					conf->gen.tentative_source = conf->gen.reset_source;
					conf->gen.status |= GUC_HAVE_TENTATIVE;
					guc_dirty = true;
					break;
				}
			case PGC_REAL:
				{
					struct config_real *conf = (struct config_real *) gconf;

					if (conf->assign_hook)
						if (!(*conf->assign_hook) (conf->reset_val, true, true))
							elog(ERROR, "failed to reset %s", conf->gen.name);
					*conf->variable = conf->reset_val;
					conf->tentative_val = conf->reset_val;
					conf->gen.source = conf->gen.reset_source;
					conf->gen.tentative_source = conf->gen.reset_source;
					conf->gen.status |= GUC_HAVE_TENTATIVE;
					guc_dirty = true;
					break;
				}
			case PGC_STRING:
				{
					struct config_string *conf = (struct config_string *) gconf;
					char	   *str;

					if (conf->reset_val == NULL)
					{
						/* Nothing to reset to, as yet; so do nothing */
						break;
					}

					/* We need not strdup here */
					str = conf->reset_val;

					if (conf->assign_hook)
					{
						const char *newstr;

						newstr = (*conf->assign_hook) (str, true, true);
						if (newstr == NULL)
							elog(ERROR, "failed to reset %s", conf->gen.name);
						else if (newstr != str)
						{
							/*
							 * See notes in set_config_option about
							 * casting
							 */
							str = (char *) newstr;
						}
					}

					SET_STRING_VARIABLE(conf, str);
					SET_STRING_TENTATIVE_VAL(conf, str);
					conf->gen.source = conf->gen.reset_source;
					conf->gen.tentative_source = conf->gen.reset_source;
					conf->gen.status |= GUC_HAVE_TENTATIVE;
					guc_dirty = true;
					break;
				}
		}

		if (gconf->flags & GUC_REPORT)
			ReportGUCOption(gconf);
	}
}


/*
 * Do GUC processing at transaction commit or abort.
 */
void
AtEOXact_GUC(bool isCommit)
{
	int			i;

	/* Quick exit if nothing's changed in this transaction */
	if (!guc_dirty)
		return;

	/* Prevent memory leak if ereport during an assign_hook */
	if (guc_string_workspace)
	{
		free(guc_string_workspace);
		guc_string_workspace = NULL;
	}

	for (i = 0; i < num_guc_variables; i++)
	{
		struct config_generic *gconf = guc_variables[i];
		bool		changed;

		/* Skip if nothing's happened to this var in this transaction */
		if (gconf->status == 0)
			continue;

		changed = false;

		switch (gconf->vartype)
		{
			case PGC_BOOL:
				{
					struct config_bool *conf = (struct config_bool *) gconf;

					if (isCommit && (conf->gen.status & GUC_HAVE_TENTATIVE))
					{
						conf->session_val = conf->tentative_val;
						conf->gen.session_source = conf->gen.tentative_source;
					}

					if (*conf->variable != conf->session_val)
					{
						if (conf->assign_hook)
							if (!(*conf->assign_hook) (conf->session_val,
													   true, false))
								elog(LOG, "failed to commit %s",
									 conf->gen.name);
						*conf->variable = conf->session_val;
						changed = true;
					}
					conf->gen.source = conf->gen.session_source;
					conf->gen.status = 0;
					break;
				}
			case PGC_INT:
				{
					struct config_int *conf = (struct config_int *) gconf;

					if (isCommit && (conf->gen.status & GUC_HAVE_TENTATIVE))
					{
						conf->session_val = conf->tentative_val;
						conf->gen.session_source = conf->gen.tentative_source;
					}

					if (*conf->variable != conf->session_val)
					{
						if (conf->assign_hook)
							if (!(*conf->assign_hook) (conf->session_val,
													   true, false))
								elog(LOG, "failed to commit %s",
									 conf->gen.name);
						*conf->variable = conf->session_val;
						changed = true;
					}
					conf->gen.source = conf->gen.session_source;
					conf->gen.status = 0;
					break;
				}
			case PGC_REAL:
				{
					struct config_real *conf = (struct config_real *) gconf;

					if (isCommit && (conf->gen.status & GUC_HAVE_TENTATIVE))
					{
						conf->session_val = conf->tentative_val;
						conf->gen.session_source = conf->gen.tentative_source;
					}

					if (*conf->variable != conf->session_val)
					{
						if (conf->assign_hook)
							if (!(*conf->assign_hook) (conf->session_val,
													   true, false))
								elog(LOG, "failed to commit %s",
									 conf->gen.name);
						*conf->variable = conf->session_val;
						changed = true;
					}
					conf->gen.source = conf->gen.session_source;
					conf->gen.status = 0;
					break;
				}
			case PGC_STRING:
				{
					struct config_string *conf = (struct config_string *) gconf;

					if (isCommit && (conf->gen.status & GUC_HAVE_TENTATIVE))
					{
						SET_STRING_SESSION_VAL(conf, conf->tentative_val);
						conf->gen.session_source = conf->gen.tentative_source;
						conf->tentative_val = NULL;		/* transfer ownership */
					}
					else
						SET_STRING_TENTATIVE_VAL(conf, NULL);

					if (*conf->variable != conf->session_val)
					{
						char	   *str = conf->session_val;

						if (conf->assign_hook)
						{
							const char *newstr;

							newstr = (*conf->assign_hook) (str, true, false);
							if (newstr == NULL)
								elog(LOG, "failed to commit %s",
									 conf->gen.name);
							else if (newstr != str)
							{
								/*
								 * See notes in set_config_option about
								 * casting
								 */
								str = (char *) newstr;
								SET_STRING_SESSION_VAL(conf, str);
							}
						}

						SET_STRING_VARIABLE(conf, str);
						changed = true;
					}
					conf->gen.source = conf->gen.session_source;
					conf->gen.status = 0;
					break;
				}
		}

		if (changed && (gconf->flags & GUC_REPORT))
			ReportGUCOption(gconf);
	}

	guc_dirty = false;
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
	 * Don't do anything unless talking to an interactive frontend of
	 * protocol 3.0 or later.
	 */
	if (whereToSendOutput != Remote ||
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
		char	   *val = _ShowOption(record);
		StringInfoData msgbuf;

		pq_beginmessage(&msgbuf, 'S');
		pq_sendstring(&msgbuf, record->name);
		pq_sendstring(&msgbuf, val);
		pq_endmessage(&msgbuf);

		pfree(val);
	}
}


/*
 * Try to interpret value as boolean value.  Valid values are: true,
 * false, yes, no, on, off, 1, 0.  If the string parses okay, return
 * true, else false.  If result is not NULL, return the parsing result
 * there.
 */
static bool
parse_bool(const char *value, bool *result)
{
	size_t		len = strlen(value);

	if (strncasecmp(value, "true", len) == 0)
	{
		if (result)
			*result = true;
	}
	else if (strncasecmp(value, "false", len) == 0)
	{
		if (result)
			*result = false;
	}

	else if (strncasecmp(value, "yes", len) == 0)
	{
		if (result)
			*result = true;
	}
	else if (strncasecmp(value, "no", len) == 0)
	{
		if (result)
			*result = false;
	}

	else if (strcasecmp(value, "on") == 0)
	{
		if (result)
			*result = true;
	}
	else if (strcasecmp(value, "off") == 0)
	{
		if (result)
			*result = false;
	}

	else if (strcasecmp(value, "1") == 0)
	{
		if (result)
			*result = true;
	}
	else if (strcasecmp(value, "0") == 0)
	{
		if (result)
			*result = false;
	}

	else
		return false;
	return true;
}



/*
 * Try to parse value as an integer.  The accepted formats are the
 * usual decimal, octal, or hexadecimal formats.  If the string parses
 * okay, return true, else false.  If result is not NULL, return the
 * value there.
 */
static bool
parse_int(const char *value, int *result)
{
	long		val;
	char	   *endptr;

	errno = 0;
	val = strtol(value, &endptr, 0);
	if (endptr == value || *endptr != '\0' || errno == ERANGE
#ifdef HAVE_LONG_INT_64
	/* if long > 32 bits, check for overflow of int4 */
		|| val != (long) ((int32) val)
#endif
		)
		return false;
	if (result)
		*result = (int) val;
	return true;
}



/*
 * Try to parse value as a floating point constant in the usual
 * format.	If the value parsed okay return true, else false.  If
 * result is not NULL, return the semantic value there.
 */
static bool
parse_real(const char *value, double *result)
{
	double		val;
	char	   *endptr;

	errno = 0;
	val = strtod(value, &endptr);
	if (endptr == value || *endptr != '\0' || errno == ERANGE)
		return false;
	if (result)
		*result = val;
	return true;
}



/*
 * Sets option `name' to given value. The value should be a string
 * which is going to be parsed and converted to the appropriate data
 * type.  The context and source parameters indicate in which context this
 * function is being called so it can apply the access restrictions
 * properly.
 *
 * If value is NULL, set the option to its default value. If the
 * parameter changeVal is false then don't really set the option but do all
 * the checks to see if it would work.
 *
 * If there is an error (non-existing option, invalid value) then an
 * ereport(ERROR) is thrown *unless* this is called in a context where we
 * don't want to ereport (currently, startup or SIGHUP config file reread).
 * In that case we write a suitable error message via ereport(DEBUG) and
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
				  bool isLocal, bool changeVal)
{
	struct config_generic *record;
	int			elevel;
	bool		interactive;
	bool		makeDefault;
	bool		changeVal_orig;

	if (context == PGC_SIGHUP || source == PGC_S_DEFAULT)
		elevel = DEBUG2;
	else if (source == PGC_S_DATABASE || source == PGC_S_USER)
		elevel = INFO;
	else
		elevel = ERROR;

	record = find_option(name);
	if (record == NULL)
	{
		ereport(elevel,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("unrecognized configuration parameter \"%s\"", name)));
		return false;
	}

	/*
	 * Check if the option can be set at this time. See guc.h for the
	 * precise rules. Note that we don't want to throw errors if we're in
	 * the SIGHUP context. In that case we just ignore the attempt and
	 * return true.
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
				return true;
			if (context != PGC_POSTMASTER)
			{
				ereport(elevel,
						(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
					errmsg("parameter \"%s\" cannot be changed after server start",
						   name)));
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
			 * Hmm, the idea of the SIGHUP context is "ought to be global,
			 * but can be changed after postmaster start". But there's
			 * nothing that prevents a crafty administrator from sending
			 * SIGHUP signals to individual backends only.
			 */
			break;
		case PGC_BACKEND:
			if (context == PGC_SIGHUP)
			{
				/*
				 * If a PGC_BACKEND parameter is changed in the config
				 * file, we want to accept the new value in the postmaster
				 * (whence it will propagate to subsequently-started
				 * backends), but ignore it in existing backends.  This is
				 * a tad klugy, but necessary because we don't re-read the
				 * config file during backend start.
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
		case PGC_USERLIMIT:
			/* USERLIMIT permissions checked below */
			break;
		case PGC_USERSET:
			/* always okay */
			break;
	}

	/* Should we report errors interactively? */
	interactive = (source >= PGC_S_SESSION);

	/*
	 * Should we set reset/session values?	(If so, the behavior is not
	 * transactional.)
	 */
	makeDefault = changeVal && (source <= PGC_S_OVERRIDE) && (value != NULL);

	/*
	 * Ignore attempted set if overridden by previously processed setting.
	 * However, if changeVal is false then plow ahead anyway since we are
	 * trying to find out if the value is potentially good, not actually
	 * use it. Also keep going if makeDefault is true, since we may want
	 * to set the reset/session values even if we can't set the variable
	 * itself.
	 */
	changeVal_orig = changeVal;			/* we might have to reverse this later */
	if (record->source > source)
	{
		if (changeVal && !makeDefault)
		{
			elog(DEBUG3, "\"%s\": setting ignored because previous source is higher priority",
				 name);
			return true;
		}
		changeVal = false;			/* we won't change the variable itself */
	}

	/*
	 * Evaluate value and set variable
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
					/* Limit non-superuser changes */
					if (record->context == PGC_USERLIMIT &&
						source > PGC_S_UNPRIVILEGED &&
						newval < conf->reset_val &&
						!superuser())
					{
						ereport(elevel,
								(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to set parameter \"%s\"",
								name),
								 errhint("Must be superuser to change this value to false.")));
						return false;
					}
					/* Allow admin to override non-superuser setting */
					if (record->context == PGC_USERLIMIT &&
						source < PGC_S_UNPRIVILEGED &&
						record->reset_source > PGC_S_UNPRIVILEGED &&
						newval > conf->reset_val &&
						!superuser())
						changeVal = changeVal_orig;
				}
				else
				{
					newval = conf->reset_val;
					source = conf->gen.reset_source;
				}

				if (conf->assign_hook)
					if (!(*conf->assign_hook) (newval, changeVal, interactive))
					{
						ereport(elevel,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("invalid value for parameter \"%s\": %d",
										name, (int) newval)));
						return false;
					}

				if (changeVal || makeDefault)
				{
					if (changeVal)
					{
						*conf->variable = newval;
						conf->gen.source = source;
					}
					if (makeDefault)
					{
						if (conf->gen.reset_source <= source)
						{
							conf->reset_val = newval;
							conf->gen.reset_source = source;
						}
						if (conf->gen.session_source <= source)
						{
							conf->session_val = newval;
							conf->gen.session_source = source;
						}
					}
					else if (isLocal)
					{
						conf->gen.status |= GUC_HAVE_LOCAL;
						guc_dirty = true;
					}
					else
					{
						conf->tentative_val = newval;
						conf->gen.tentative_source = source;
						conf->gen.status |= GUC_HAVE_TENTATIVE;
						guc_dirty = true;
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
					if (!parse_int(value, &newval))
					{
						ereport(elevel,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							   errmsg("parameter \"%s\" requires an integer value",
									  name)));
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
					/* Limit non-superuser changes */
					if (record->context == PGC_USERLIMIT &&
						source > PGC_S_UNPRIVILEGED &&
						conf->reset_val != 0 &&
						(newval > conf->reset_val || newval == 0) &&
						!superuser())
					{
						ereport(elevel,
								(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to set parameter \"%s\"",
								name),
								 errhint("Must be superuser to increase this value or set it to zero.")));
						return false;
					}
					/* Allow admin to override non-superuser setting */
					if (record->context == PGC_USERLIMIT &&
						source < PGC_S_UNPRIVILEGED &&
						record->reset_source > PGC_S_UNPRIVILEGED &&
						newval < conf->reset_val &&
						!superuser())
						changeVal = changeVal_orig;
				}
				else
				{
					newval = conf->reset_val;
					source = conf->gen.reset_source;
				}

				if (conf->assign_hook)
					if (!(*conf->assign_hook) (newval, changeVal, interactive))
					{
						ereport(elevel,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("invalid value for parameter \"%s\": %d",
										name, newval)));
						return false;
					}

				if (changeVal || makeDefault)
				{
					if (changeVal)
					{
						*conf->variable = newval;
						conf->gen.source = source;
					}
					if (makeDefault)
					{
						if (conf->gen.reset_source <= source)
						{
							conf->reset_val = newval;
							conf->gen.reset_source = source;
						}
						if (conf->gen.session_source <= source)
						{
							conf->session_val = newval;
							conf->gen.session_source = source;
						}
					}
					else if (isLocal)
					{
						conf->gen.status |= GUC_HAVE_LOCAL;
						guc_dirty = true;
					}
					else
					{
						conf->tentative_val = newval;
						conf->gen.tentative_source = source;
						conf->gen.status |= GUC_HAVE_TENTATIVE;
						guc_dirty = true;
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
					/* Limit non-superuser changes */
					if (record->context == PGC_USERLIMIT &&
						source > PGC_S_UNPRIVILEGED &&
						newval > conf->reset_val &&
						!superuser())
					{
						ereport(elevel,
								(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to set parameter \"%s\"",
								name),
								 errhint("Must be superuser to increase this value.")));
						return false;
					}
					/* Allow admin to override non-superuser setting */
					if (record->context == PGC_USERLIMIT &&
						source < PGC_S_UNPRIVILEGED &&
						record->reset_source > PGC_S_UNPRIVILEGED &&
						newval < conf->reset_val &&
						!superuser())
						changeVal = changeVal_orig;
				}
				else
				{
					newval = conf->reset_val;
					source = conf->gen.reset_source;
				}

				if (conf->assign_hook)
					if (!(*conf->assign_hook) (newval, changeVal, interactive))
					{
						ereport(elevel,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("invalid value for parameter \"%s\": %g",
										name, newval)));
						return false;
					}

				if (changeVal || makeDefault)
				{
					if (changeVal)
					{
						*conf->variable = newval;
						conf->gen.source = source;
					}
					if (makeDefault)
					{
						if (conf->gen.reset_source <= source)
						{
							conf->reset_val = newval;
							conf->gen.reset_source = source;
						}
						if (conf->gen.session_source <= source)
						{
							conf->session_val = newval;
							conf->gen.session_source = source;
						}
					}
					else if (isLocal)
					{
						conf->gen.status |= GUC_HAVE_LOCAL;
						guc_dirty = true;
					}
					else
					{
						conf->tentative_val = newval;
						conf->gen.tentative_source = source;
						conf->gen.status |= GUC_HAVE_TENTATIVE;
						guc_dirty = true;
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
					newval = strdup(value);
					if (newval == NULL)
					{
						ereport(elevel,
								(errcode(ERRCODE_OUT_OF_MEMORY),
								 errmsg("out of memory")));
						return false;
					}

					/*
					 * The only sort of "parsing" check we need to do is
					 * apply truncation if GUC_IS_NAME.
					 */
					if (conf->gen.flags & GUC_IS_NAME)
						truncate_identifier(newval, strlen(newval), true);

					if (record->context == PGC_USERLIMIT &&
						*conf->variable)
					{
						int			old_int_value,
									new_int_value;

						/* all USERLIMIT strings are message levels */
						assign_msglvl(&old_int_value, conf->reset_val,
									  true, interactive);
						assign_msglvl(&new_int_value, newval,
									  true, interactive);
						/* Limit non-superuser changes */
						if (source > PGC_S_UNPRIVILEGED &&
							new_int_value > old_int_value &&
							!superuser())
						{
							ereport(elevel,
								(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
								 errmsg("permission denied to set parameter \"%s\"",
										name),
								 errhint("Must be superuser to increase this value.")));
							return false;
						}
						/* Allow admin to override non-superuser setting */
						if (source < PGC_S_UNPRIVILEGED &&
							record->reset_source > PGC_S_UNPRIVILEGED &&
							newval < conf->reset_val &&
							!superuser())
							changeVal = changeVal_orig;
					}
				}
				else if (conf->reset_val)
				{
					/*
					 * We could possibly avoid strdup here, but easier to
					 * make this case work the same as the normal
					 * assignment case.
					 */
					newval = strdup(conf->reset_val);
					if (newval == NULL)
					{
						ereport(elevel,
								(errcode(ERRCODE_OUT_OF_MEMORY),
								 errmsg("out of memory")));
						return false;
					}
					source = conf->gen.reset_source;
				}
				else
				{
					/* Nothing to reset to, as yet; so do nothing */
					break;
				}

				/*
				 * Remember string in workspace, so that we can free it
				 * and avoid a permanent memory leak if hook ereports.
				 */
				if (guc_string_workspace)
					free(guc_string_workspace);
				guc_string_workspace = newval;

				if (conf->assign_hook)
				{
					const char *hookresult;

					hookresult = (*conf->assign_hook) (newval,
													   changeVal, interactive);
					guc_string_workspace = NULL;
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
						 * Having to cast away const here is annoying, but
						 * the alternative is to declare assign_hooks as
						 * returning char*, which would mean they'd have
						 * to cast away const, or as both taking and
						 * returning char*, which doesn't seem attractive
						 * either --- we don't want them to scribble on
						 * the passed str.
						 */
						newval = (char *) hookresult;
					}
				}

				guc_string_workspace = NULL;

				if (changeVal || makeDefault)
				{
					if (changeVal)
					{
						SET_STRING_VARIABLE(conf, newval);
						conf->gen.source = source;
					}
					if (makeDefault)
					{
						if (conf->gen.reset_source <= source)
						{
							SET_STRING_RESET_VAL(conf, newval);
							conf->gen.reset_source = source;
						}
						if (conf->gen.session_source <= source)
						{
							SET_STRING_SESSION_VAL(conf, newval);
							conf->gen.session_source = source;
						}
						/* Perhaps we didn't install newval anywhere */
						if (newval != *conf->variable &&
							newval != conf->session_val &&
							newval != conf->reset_val)
							free(newval);
					}
					else if (isLocal)
					{
						conf->gen.status |= GUC_HAVE_LOCAL;
						guc_dirty = true;
					}
					else
					{
						SET_STRING_TENTATIVE_VAL(conf, newval);
						conf->gen.tentative_source = source;
						conf->gen.status |= GUC_HAVE_TENTATIVE;
						guc_dirty = true;
					}
				}
				else
					free(newval);
				break;
			}
	}

	if (changeVal && (record->flags & GUC_REPORT))
		ReportGUCOption(record);

	return true;
}



/*
 * Set a config option to the given value. See also set_config_option,
 * this is just the wrapper to be called from outside GUC.	NB: this
 * is used only for non-transactional operations.
 */
void
SetConfigOption(const char *name, const char *value,
				GucContext context, GucSource source)
{
	(void) set_config_option(name, value, context, source, false, true);
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

	record = find_option(name);
	if (record == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("unrecognized configuration parameter \"%s\"", name)));

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
	}
	return NULL;
}

/*
 * Get the RESET value associated with the given option.
 */
const char *
GetConfigOptionResetString(const char *name)
{
	struct config_generic *record;
	static char buffer[256];

	record = find_option(name);
	if (record == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("unrecognized configuration parameter \"%s\"", name)));

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
	}
	return NULL;
}



/*
 * flatten_set_variable_args
 *		Given a parsenode List as emitted by the grammar for SET,
 *		convert to the flat string representation used by GUC.
 *
 * We need to be told the name of the variable the args are for, because
 * the flattening rules vary (ugh).
 *
 * The result is NULL if input is NIL (ie, SET ... TO DEFAULT), otherwise
 * a palloc'd string.
 */
char *
flatten_set_variable_args(const char *name, List *args)
{
	struct config_generic *record;
	int			flags;
	StringInfoData buf;
	List	   *l;

	/*
	 * Fast path if just DEFAULT.  We do not check the variable name in
	 * this case --- necessary for RESET ALL to work correctly.
	 */
	if (args == NIL)
		return NULL;

	/* Else get flags for the variable */
	record = find_option(name);
	if (record == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("unrecognized configuration parameter \"%s\"", name)));

	flags = record->flags;

	/* Complain if list input and non-list variable */
	if ((flags & GUC_LIST_INPUT) == 0 &&
		lnext(args) != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("SET %s takes only one argument", name)));

	initStringInfo(&buf);

	foreach(l, args)
	{
		A_Const    *arg = (A_Const *) lfirst(l);
		char	   *val;

		if (l != args)
			appendStringInfo(&buf, ", ");

		if (!IsA(arg, A_Const))
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(arg));

		switch (nodeTag(&arg->val))
		{
			case T_Integer:
				appendStringInfo(&buf, "%ld", intVal(&arg->val));
				break;
			case T_Float:
				/* represented as a string, so just copy it */
				appendStringInfo(&buf, "%s", strVal(&arg->val));
				break;
			case T_String:
				val = strVal(&arg->val);
				if (arg->typename != NULL)
				{
					/*
					 * Must be a ConstInterval argument for TIME ZONE.
					 * Coerce to interval and back to normalize the value
					 * and account for any typmod.
					 */
					Datum		interval;
					char	   *intervalout;

					interval =
						DirectFunctionCall3(interval_in,
											CStringGetDatum(val),
											ObjectIdGetDatum(InvalidOid),
								   Int32GetDatum(arg->typename->typmod));

					intervalout =
						DatumGetCString(DirectFunctionCall3(interval_out,
															interval,
											ObjectIdGetDatum(InvalidOid),
													 Int32GetDatum(-1)));
					appendStringInfo(&buf, "INTERVAL '%s'", intervalout);
				}
				else
				{
					/*
					 * Plain string literal or identifier.	For quote
					 * mode, quote it if it's not a vanilla identifier.
					 */
					if (flags & GUC_LIST_QUOTE)
						appendStringInfo(&buf, "%s", quote_identifier(val));
					else
						appendStringInfo(&buf, "%s", val);
				}
				break;
			default:
				elog(ERROR, "unrecognized node type: %d",
					 (int) nodeTag(&arg->val));
				break;
		}
	}

	return buf.data;
}


/*
 * SET command
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
					  is_local,
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
	text	   *result_text;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("SET requires parameter name")));

	/* Get the GUC variable name */
	name = DatumGetCString(DirectFunctionCall1(textout, PG_GETARG_DATUM(0)));

	/* Get the desired value or set to NULL for a reset request */
	if (PG_ARGISNULL(1))
		value = NULL;
	else
		value = DatumGetCString(DirectFunctionCall1(textout, PG_GETARG_DATUM(1)));

	/*
	 * Get the desired state of is_local. Default to false if provided
	 * value is NULL
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
					  is_local,
					  true);

	/* get the new current value */
	new_value = GetConfigOptionByName(name, NULL);

	/* Convert return string to text */
	result_text = DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum(new_value)));

	/* return it */
	PG_RETURN_TEXT_P(result_text);
}

/*
 * SHOW command
 */
void
GetPGVariable(const char *name, DestReceiver *dest)
{
	if (strcasecmp(name, "all") == 0)
		ShowAllGUCConfig(dest);
	else
		ShowGUCConfigOption(name, dest);
}

TupleDesc
GetPGVariableResultDesc(const char *name)
{
	TupleDesc	tupdesc;

	if (strcasecmp(name, "all") == 0)
	{
		/* need a tuple descriptor representing two TEXT columns */
		tupdesc = CreateTemplateTupleDesc(2, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "name",
						   TEXTOID, -1, 0, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "setting",
						   TEXTOID, -1, 0, false);
	}
	else
	{
		const char *varname;

		/* Get the canonical spelling of name */
		(void) GetConfigOptionByName(name, &varname);

		/* need a tuple descriptor representing a single TEXT column */
		tupdesc = CreateTemplateTupleDesc(1, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, varname,
						   TEXTOID, -1, 0, false);
	}
	return tupdesc;
}

/*
 * RESET command
 */
void
ResetPGVariable(const char *name)
{
	if (strcasecmp(name, "all") == 0)
		ResetAllOptions();
	else
		set_config_option(name,
						  NULL,
						  (superuser() ? PGC_SUSET : PGC_USERSET),
						  PGC_S_SESSION,
						  false,
						  true);
}


/*
 * SHOW command
 */
void
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
					   TEXTOID, -1, 0, false);

	/* prepare for projection of tuples */
	tstate = begin_tup_output_tupdesc(dest, tupdesc);

	/* Send it */
	do_text_output_oneline(tstate, value);

	end_tup_output(tstate);
}

/*
 * SHOW ALL command
 */
void
ShowAllGUCConfig(DestReceiver *dest)
{
	int			i;
	TupOutputState *tstate;
	TupleDesc	tupdesc;
	char	   *values[2];

	/* need a tuple descriptor representing two TEXT columns */
	tupdesc = CreateTemplateTupleDesc(2, false);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "name",
					   TEXTOID, -1, 0, false);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "setting",
					   TEXTOID, -1, 0, false);

	/* prepare for projection of tuples */
	tstate = begin_tup_output_tupdesc(dest, tupdesc);

	for (i = 0; i < num_guc_variables; i++)
	{
		struct config_generic *conf = guc_variables[i];

		if (conf->flags & GUC_NO_SHOW_ALL)
			continue;

		/* assign to the values array */
		values[0] = (char *) conf->name;
		values[1] = _ShowOption(conf);

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

	record = find_option(name);
	if (record == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("unrecognized configuration parameter \"%s\"", name)));

	if (varname)
		*varname = record->name;

	return _ShowOption(record);
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
		*noshow = (conf->flags & GUC_NO_SHOW_ALL) ? true : false;

	/* first get the generic attributes */

	/* name */
	values[0] = conf->name;

	/* setting : use _ShowOption in order to avoid duplicating the logic */
	values[1] = _ShowOption(conf);

	/* context */
	values[2] = GucContext_Names[conf->context];

	/* vartype */
	values[3] = config_type_names[conf->vartype];

	/* source */
	values[4] = GucSource_Names[conf->source];

	/* now get the type specifc attributes */
	switch (conf->vartype)
	{
		case PGC_BOOL:
			{
				/* min_val */
				values[5] = NULL;

				/* max_val */
				values[6] = NULL;
			}
			break;

		case PGC_INT:
			{
				struct config_int *lconf = (struct config_int *) conf;

				/* min_val */
				snprintf(buffer, sizeof(buffer), "%d", lconf->min);
				values[5] = pstrdup(buffer);

				/* max_val */
				snprintf(buffer, sizeof(buffer), "%d", lconf->max);
				values[6] = pstrdup(buffer);
			}
			break;

		case PGC_REAL:
			{
				struct config_real *lconf = (struct config_real *) conf;

				/* min_val */
				snprintf(buffer, sizeof(buffer), "%g", lconf->min);
				values[5] = pstrdup(buffer);

				/* max_val */
				snprintf(buffer, sizeof(buffer), "%g", lconf->max);
				values[6] = pstrdup(buffer);
			}
			break;

		case PGC_STRING:
			{
				/* min_val */
				values[5] = NULL;

				/* max_val */
				values[6] = NULL;
			}
			break;

		default:
			{
				/*
				 * should never get here, but in case we do, set 'em to
				 * NULL
				 */

				/* min_val */
				values[5] = NULL;

				/* max_val */
				values[6] = NULL;
			}
			break;
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
	text	   *result_text;

	/* Get the GUC variable name */
	varname = DatumGetCString(DirectFunctionCall1(textout, PG_GETARG_DATUM(0)));

	/* Get the value */
	varval = GetConfigOptionByName(varname, NULL);

	/* Convert to text */
	result_text = DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum(varval)));

	/* return it */
	PG_RETURN_TEXT_P(result_text);
}

/*
 * show_all_settings - equiv to SHOW ALL command but implemented as
 * a Table Function.
 */
#define NUM_PG_SETTINGS_ATTS	7

Datum
show_all_settings(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	TupleDesc	tupdesc;
	int			call_cntr;
	int			max_calls;
	TupleTableSlot *slot;
	AttInMetadata *attinmeta;
	MemoryContext oldcontext;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * switch to memory context appropriate for multiple function
		 * calls
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/*
		 * need a tuple descriptor representing NUM_PG_SETTINGS_ATTS
		 * columns of the appropriate types
		 */
		tupdesc = CreateTemplateTupleDesc(NUM_PG_SETTINGS_ATTS, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "name",
						   TEXTOID, -1, 0, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "setting",
						   TEXTOID, -1, 0, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "context",
						   TEXTOID, -1, 0, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 4, "vartype",
						   TEXTOID, -1, 0, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 5, "source",
						   TEXTOID, -1, 0, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 6, "min_val",
						   TEXTOID, -1, 0, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 7, "max_val",
						   TEXTOID, -1, 0, false);

		/* allocate a slot for a tuple with this tupdesc */
		slot = TupleDescGetSlot(tupdesc);

		/* assign slot to function context */
		funcctx->slot = slot;

		/*
		 * Generate attribute metadata needed later to produce tuples from
		 * raw C strings
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
	slot = funcctx->slot;
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
		result = TupleGetDatum(slot, tuple);

		SRF_RETURN_NEXT(funcctx, result);
	}
	else
	{
		/* do when there is no more left */
		SRF_RETURN_DONE(funcctx);
	}
}

static char *
_ShowOption(struct config_generic * record)
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
					snprintf(buffer, sizeof(buffer), "%d",
							 *conf->variable);
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
					val = "unset";
			}
			break;

		default:
			/* just to keep compiler quiet */
			val = "???";
			break;
	}

	return pstrdup(val);
}


#ifdef EXEC_BACKEND
/*
 *	This routine dumps out all non-default GUC options into a binary
 *	file that is read by all exec'ed backends.  The format is:
 *
 *		variable name, string, null terminated
 *		variable value, string, null terminated
 *		variable source, integer
 */
void
write_nondefault_variables(GucContext context)
{
	int			i;
	char	   *new_filename,
			   *filename;
	int			elevel;
	FILE	   *fp;

	Assert(context == PGC_POSTMASTER || context == PGC_SIGHUP);
	Assert(DataDir);
	elevel = (context == PGC_SIGHUP) ? DEBUG4 : ERROR;

	/*
	 * Open file
	 */
	new_filename = malloc(strlen(DataDir) + strlen(CONFIG_EXEC_PARAMS) +
						  strlen(".new") + 2);
	filename = malloc(strlen(DataDir) + strlen(CONFIG_EXEC_PARAMS) + 2);
	if (new_filename == NULL || filename == NULL)
	{
		ereport(elevel,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
		return;
	}
	sprintf(new_filename, "%s/" CONFIG_EXEC_PARAMS ".new", DataDir);
	sprintf(filename, "%s/" CONFIG_EXEC_PARAMS, DataDir);

	fp = AllocateFile(new_filename, "w");
	if (!fp)
	{
		free(new_filename);
		free(filename);
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", CONFIG_EXEC_PARAMS)));
		return;
	}

	for (i = 0; i < num_guc_variables; i++)
	{
		struct config_generic *gconf = guc_variables[i];

		if (gconf->source != PGC_S_DEFAULT)
		{
			fprintf(fp, "%s", gconf->name);
			fputc(0, fp);

			switch (gconf->vartype)
			{
				case PGC_BOOL:
					{
						struct config_bool *conf = (struct config_bool *) gconf;

						if (*conf->variable == 0)
							fprintf(fp, "false");
						else
							fprintf(fp, "true");
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
			}

			fputc(0, fp);

			fwrite(&gconf->source, sizeof(gconf->source), 1, fp);
		}
	}

	FreeFile(fp);
	/* Put new file in place, this could delay on Win32 */
	rename(new_filename, filename);
	free(new_filename);
	free(filename);
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
			str = malloc(maxlen);
		else if (i == maxlen)
			str = realloc(str, maxlen *= 2);
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
	char	   *filename;
	FILE	   *fp;
	char	   *varname,
			   *varvalue;
	int			varsource;

	Assert(DataDir);

	/*
	 * Open file
	 */
	filename = malloc(strlen(DataDir) + strlen(CONFIG_EXEC_PARAMS) + 2);
	if (filename == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
		return;
	}
	sprintf(filename, "%s/" CONFIG_EXEC_PARAMS, DataDir);

	fp = AllocateFile(filename, "r");
	if (!fp)
	{
		free(filename);
		/* File not found is fine */
		if (errno != ENOENT)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not read from file \"%s\": %m", CONFIG_EXEC_PARAMS)));
		return;
	}

	for (;;)
	{
		if ((varname = read_string_with_null(fp)) == NULL)
			break;

		if ((varvalue = read_string_with_null(fp)) == NULL)
			elog(FATAL, "invalid format of exec config params file");
		if (fread(&varsource, sizeof(varsource), 1, fp) == 0)
			elog(FATAL, "invalid format of exec config params file");

		(void) set_config_option(varname, varvalue, PGC_POSTMASTER,
								 varsource, false, true);
		free(varname);
		free(varvalue);
	}

	FreeFile(fp);
	free(filename);
	return;
}
#endif


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
		*name = malloc(equal_pos + 1);
		if (!*name)
			ereport(FATAL,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		strncpy(*name, string, equal_pos);
		(*name)[equal_pos] = '\0';

		*value = strdup(&string[equal_pos + 1]);
		if (!*value)
			ereport(FATAL,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
	}
	else
	{
		/* no equal sign in string */
		*name = strdup(string);
		if (!*name)
			ereport(FATAL,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		*value = NULL;
	}

	for (cp = *name; *cp; cp++)
		if (*cp == '-')
			*cp = '_';
}


/*
 * Handle options fetched from pg_database.datconfig or pg_shadow.useconfig.
 * The array parameter must be an array of TEXT (it must not be NULL).
 */
void
ProcessGUCArray(ArrayType *array, GucSource source)
{
	int			i;

	Assert(array != NULL);
	Assert(ARR_ELEMTYPE(array) == TEXTOID);
	Assert(ARR_NDIM(array) == 1);
	Assert(ARR_LBOUND(array)[0] == 1);
	Assert(source == PGC_S_DATABASE || source == PGC_S_USER);

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

		s = DatumGetCString(DirectFunctionCall1(textout, d));

		ParseLongOption(s, &name, &value);
		if (!value)
		{
			ereport(WARNING,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("could not parse setting for parameter \"%s\"", name)));
			free(name);
			continue;
		}

		/*
		 * We process all these options at SUSET level.  We assume that
		 * the right to insert an option into pg_database or pg_shadow was
		 * checked when it was inserted.
		 */
		SetConfigOption(name, value, PGC_SUSET, source);

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
					  PGC_S_SESSION, false, false);

	/* convert name to canonical spelling, so we can use plain strcmp */
	(void) GetConfigOptionByName(name, &varname);
	name = varname;

	newval = palloc(strlen(name) + 1 + strlen(value) + 1);
	sprintf(newval, "%s=%s", name, value);
	datum = DirectFunctionCall1(textin, CStringGetDatum(newval));

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
			current = DatumGetCString(DirectFunctionCall1(textout, d));
			if (strncmp(current, newval, strlen(name) + 1) == 0)
			{
				index = i;
				break;
			}
		}

		isnull = false;
		a = array_set(array, 1, &index,
					  datum,
					  -1 /* varlenarray */ ,
					  -1 /* TEXT's typlen */ ,
					  false /* TEXT's typbyval */ ,
					  'i' /* TEXT's typalign */ ,
					  &isnull);
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
					  PGC_S_SESSION, false, false);

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
		val = DatumGetCString(DirectFunctionCall1(textout, d));

		/* ignore entry if it's what we want to delete */
		if (strncmp(val, name, strlen(name)) == 0
			&& val[strlen(name)] == '=')
			continue;

		/* else add it to the output array */
		if (newarray)
		{
			isnull = false;
			newarray = array_set(newarray, 1, &index,
								 d,
								 -1 /* varlenarray */ ,
								 -1 /* TEXT's typlen */ ,
								 false /* TEXT's typbyval */ ,
								 'i' /* TEXT's typalign */ ,
								 &isnull);
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
 * assign_hook subroutines
 */

#ifdef HAVE_SYSLOG

static const char *
assign_facility(const char *facility, bool doit, bool interactive)
{
	if (strcasecmp(facility, "LOCAL0") == 0)
		return facility;
	if (strcasecmp(facility, "LOCAL1") == 0)
		return facility;
	if (strcasecmp(facility, "LOCAL2") == 0)
		return facility;
	if (strcasecmp(facility, "LOCAL3") == 0)
		return facility;
	if (strcasecmp(facility, "LOCAL4") == 0)
		return facility;
	if (strcasecmp(facility, "LOCAL5") == 0)
		return facility;
	if (strcasecmp(facility, "LOCAL6") == 0)
		return facility;
	if (strcasecmp(facility, "LOCAL7") == 0)
		return facility;
	return NULL;
}
#endif


static const char *
assign_defaultxactisolevel(const char *newval, bool doit, bool interactive)
{
	if (strcasecmp(newval, "serializable") == 0)
	{
		if (doit)
			DefaultXactIsoLevel = XACT_SERIALIZABLE;
	}
	else if (strcasecmp(newval, "read committed") == 0)
	{
		if (doit)
			DefaultXactIsoLevel = XACT_READ_COMMITTED;
	}
	else
		return NULL;
	return newval;
}

static const char *
assign_log_min_messages(const char *newval,
						bool doit, bool interactive)
{
	return (assign_msglvl(&log_min_messages, newval, doit, interactive));
}

static const char *
assign_client_min_messages(const char *newval,
						   bool doit, bool interactive)
{
	return (assign_msglvl(&client_min_messages, newval, doit, interactive));
}

static const char *
assign_min_error_statement(const char *newval, bool doit, bool interactive)
{
	return (assign_msglvl(&log_min_error_statement, newval, doit, interactive));
}

static const char *
assign_msglvl(int *var, const char *newval, bool doit, bool interactive)
{
	if (strcasecmp(newval, "debug") == 0)
	{
		if (doit)
			(*var) = DEBUG2;
	}
	else if (strcasecmp(newval, "debug5") == 0)
	{
		if (doit)
			(*var) = DEBUG5;
	}
	else if (strcasecmp(newval, "debug4") == 0)
	{
		if (doit)
			(*var) = DEBUG4;
	}
	else if (strcasecmp(newval, "debug3") == 0)
	{
		if (doit)
			(*var) = DEBUG3;
	}
	else if (strcasecmp(newval, "debug2") == 0)
	{
		if (doit)
			(*var) = DEBUG2;
	}
	else if (strcasecmp(newval, "debug1") == 0)
	{
		if (doit)
			(*var) = DEBUG1;
	}
	else if (strcasecmp(newval, "log") == 0)
	{
		if (doit)
			(*var) = LOG;
	}
	else if (strcasecmp(newval, "info") == 0)
	{
		if (doit)
			(*var) = INFO;
	}
	else if (strcasecmp(newval, "notice") == 0)
	{
		if (doit)
			(*var) = NOTICE;
	}
	else if (strcasecmp(newval, "warning") == 0)
	{
		if (doit)
			(*var) = WARNING;
	}
	else if (strcasecmp(newval, "error") == 0)
	{
		if (doit)
			(*var) = ERROR;
	}
	/* We allow FATAL/PANIC for client-side messages too. */
	else if (strcasecmp(newval, "fatal") == 0)
	{
		if (doit)
			(*var) = FATAL;
	}
	else if (strcasecmp(newval, "panic") == 0)
	{
		if (doit)
			(*var) = PANIC;
	}
	else
		return NULL;			/* fail */
	return newval;				/* OK */
}

static const char *
assign_log_error_verbosity(const char *newval, bool doit, bool interactive)
{
	if (strcasecmp(newval, "terse") == 0)
	{
		if (doit)
			Log_error_verbosity = PGERROR_TERSE;
	}
	else if (strcasecmp(newval, "default") == 0)
	{
		if (doit)
			Log_error_verbosity = PGERROR_DEFAULT;
	}
	else if (strcasecmp(newval, "verbose") == 0)
	{
		if (doit)
			Log_error_verbosity = PGERROR_VERBOSE;
	}
	else
		return NULL;			/* fail */
	return newval;				/* OK */
}

static const char *
assign_backslash_quote(const char *newval, bool doit, bool interactive)
{
	BackslashQuoteType bq;
	bool	bqbool;

	/*
	 * Although only "on", "off", and "safe_encoding" are documented,
	 * we use parse_bool so we can accept all the likely variants of
	 * "on" and "off".
	 */
	if (strcasecmp(newval, "safe_encoding") == 0)
		bq = BACKSLASH_QUOTE_SAFE_ENCODING;
	else if (parse_bool(newval, &bqbool))
	{
		bq = bqbool ? BACKSLASH_QUOTE_ON : BACKSLASH_QUOTE_OFF;
	}
	else
		return NULL;			/* reject */

	if (doit)
		backslash_quote = bq;

	return newval;
}

static bool
assign_phony_autocommit(bool newval, bool doit, bool interactive)
{
	if (!newval)
	{
		if (doit && interactive)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("SET AUTOCOMMIT TO OFF is no longer supported")));
		return false;
	}
	return true;
}


#include "guc-file.c"
