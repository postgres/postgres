/*--------------------------------------------------------------------
 * guc.c
 *
 * Support for grand unified configuration scheme, including SET
 * command, configuration file, and command line options.
 * See src/backend/utils/misc/README for more information.
 *
 *
 * Copyright 2000-2003 by PostgreSQL Global Development Group
 * Written by Peter Eisentraut <peter_e@gmx.net>.
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/misc/guc.c,v 1.129 2003/06/11 18:01:14 momjian Exp $
 *
 *--------------------------------------------------------------------
 */

#include "postgres.h"

#include <errno.h>
#include <float.h>
#include <limits.h>
#include <unistd.h>

#include "utils/guc.h"

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
#include "parser/parse_expr.h"
#include "storage/fd.h"
#include "storage/freespace.h"
#include "storage/lock.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/elog.h"
#include "utils/pg_locale.h"
#include "pgstat.h"

int			log_min_duration_statement = 0;


#ifndef PG_KRB_SRVTAB
#define PG_KRB_SRVTAB ""
#endif

#ifdef EXEC_BACKEND
#define CONFIG_EXEC_PARAMS "global/config_exec_params"
#endif

/* XXX these should appear in other modules' header files */
extern bool Log_connections;
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


/*
 * These variables are all dummies that don't do anything, except in some
 * cases provide the value for SHOW to display.  The real state is elsewhere
 * and is kept in sync by assign_hooks.
 */
static char *log_min_error_statement_str;
static char *log_min_messages_str;
static char *client_min_messages_str;
static bool phony_autocommit;
static double phony_random_seed;
static char *client_encoding_string;
static char *datestyle_string;
static char *default_iso_level_string;
static char *locale_collate;
static char *locale_ctype;
static char *regex_flavor_string;
static char *server_encoding_string;
static char *server_version_string;
static char *session_authorization_string;
static char *timezone_string;
static char *XactIsoLevel_string;


/*
 * Declarations for GUC tables
 *
 * See src/backend/utils/misc/README for design notes.
 */
enum config_type
{
	PGC_BOOL,
	PGC_INT,
	PGC_REAL,
	PGC_STRING
};

/* Generic fields applicable to all types of variables */
struct config_generic
{
	/* constant fields, must be set correctly in initial value: */
	const char *name;			/* name of variable - MUST BE FIRST */
	GucContext	context;		/* context required to set the variable */
	int			flags;			/* flag bits, see below */
	/* variable fields, initialized at runtime: */
	enum config_type vartype;	/* type of variable (set only at startup) */
	int			status;			/* status bits, see below */
	GucSource	reset_source;	/* source of the reset_value */
	GucSource	session_source; /* source of the session_value */
	GucSource	tentative_source;		/* source of the tentative_value */
	GucSource	source;			/* source of the current actual value */
};

/* bit values in flags field */
#define GUC_LIST_INPUT		0x0001		/* input can be list format */
#define GUC_LIST_QUOTE		0x0002		/* double-quote list elements */
#define GUC_NO_SHOW_ALL		0x0004		/* exclude from SHOW ALL */
#define GUC_NO_RESET_ALL	0x0008		/* exclude from RESET ALL */
#define GUC_REPORT			0x0010		/* auto-report changes to client */

/* bit values in status field */
#define GUC_HAVE_TENTATIVE	0x0001		/* tentative value is defined */
#define GUC_HAVE_LOCAL		0x0002		/* a SET LOCAL has been executed */


/* GUC records for specific variable types */

struct config_bool
{
	struct config_generic gen;
	/* these fields must be set correctly in initial value: */
	/* (all but reset_val are constants) */
	bool	   *variable;
	bool		reset_val;
	bool		(*assign_hook) (bool newval, bool doit, bool interactive);
	const char *(*show_hook) (void);
	/* variable fields, initialized at runtime: */
	bool		session_val;
	bool		tentative_val;
};

struct config_int
{
	struct config_generic gen;
	/* these fields must be set correctly in initial value: */
	/* (all but reset_val are constants) */
	int		   *variable;
	int			reset_val;
	int			min;
	int			max;
	bool		(*assign_hook) (int newval, bool doit, bool interactive);
	const char *(*show_hook) (void);
	/* variable fields, initialized at runtime: */
	int			session_val;
	int			tentative_val;
};

struct config_real
{
	struct config_generic gen;
	/* these fields must be set correctly in initial value: */
	/* (all but reset_val are constants) */
	double	   *variable;
	double		reset_val;
	double		min;
	double		max;
	bool		(*assign_hook) (double newval, bool doit, bool interactive);
	const char *(*show_hook) (void);
	/* variable fields, initialized at runtime: */
	double		session_val;
	double		tentative_val;
};

struct config_string
{
	struct config_generic gen;
	/* these fields must be set correctly in initial value: */
	/* (all are constants) */
	char	  **variable;
	const char *boot_val;
	const char *(*assign_hook) (const char *newval, bool doit, bool interactive);
	const char *(*show_hook) (void);
	/* variable fields, initialized at runtime: */
	char	   *reset_val;
	char	   *session_val;
	char	   *tentative_val;
};

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

static struct config_bool
			ConfigureNamesBool[] =
{
	{
		{"enable_seqscan", PGC_USERSET}, &enable_seqscan,
		true, NULL, NULL
	},
	{
		{"enable_indexscan", PGC_USERSET}, &enable_indexscan,
		true, NULL, NULL
	},
	{
		{"enable_tidscan", PGC_USERSET}, &enable_tidscan,
		true, NULL, NULL
	},
	{
		{"enable_sort", PGC_USERSET}, &enable_sort,
		true, NULL, NULL
	},
	{
		{"enable_hashagg", PGC_USERSET}, &enable_hashagg,
		true, NULL, NULL
	},
	{
		{"enable_nestloop", PGC_USERSET}, &enable_nestloop,
		true, NULL, NULL
	},
	{
		{"enable_mergejoin", PGC_USERSET}, &enable_mergejoin,
		true, NULL, NULL
	},
	{
		{"enable_hashjoin", PGC_USERSET}, &enable_hashjoin,
		true, NULL, NULL
	},
	{
		{"geqo", PGC_USERSET}, &enable_geqo,
		true, NULL, NULL
	},

	{
		{"tcpip_socket", PGC_POSTMASTER}, &NetServer,
		false, NULL, NULL
	},
	{
		{"ssl", PGC_POSTMASTER}, &EnableSSL,
		false, NULL, NULL
	},
	{
		{"fsync", PGC_SIGHUP}, &enableFsync,
		true, NULL, NULL
	},
	{
		{"zero_damaged_pages", PGC_SUSET}, &zero_damaged_pages,
		false, NULL, NULL
	},
	{
		{"silent_mode", PGC_POSTMASTER}, &SilentMode,
		false, NULL, NULL
	},

	{
		{"log_connections", PGC_BACKEND}, &Log_connections,
		false, NULL, NULL
	},
	{
		{"log_timestamp", PGC_SIGHUP}, &Log_timestamp,
		false, NULL, NULL
	},
	{
		{"log_pid", PGC_SIGHUP}, &Log_pid,
		false, NULL, NULL
	},

#ifdef USE_ASSERT_CHECKING
	{
		{"debug_assertions", PGC_USERSET}, &assert_enabled,
		true, NULL, NULL
	},
#endif

	{
		/* currently undocumented, so don't show in SHOW ALL */
		{"exit_on_error", PGC_USERSET, GUC_NO_SHOW_ALL}, &ExitOnAnyError,
		false, NULL, NULL
	},

	{
		{"log_statement", PGC_SUSET}, &log_statement,
		false, NULL, NULL
	},
	{
		{"log_duration", PGC_SUSET}, &log_duration,
		false, NULL, NULL
	},
	{
		{"debug_print_parse", PGC_USERSET}, &Debug_print_parse,
		false, NULL, NULL
	},
	{
		{"debug_print_rewritten", PGC_USERSET}, &Debug_print_rewritten,
		false, NULL, NULL
	},
	{
		{"debug_print_plan", PGC_USERSET}, &Debug_print_plan,
		false, NULL, NULL
	},
	{
		{"debug_pretty_print", PGC_USERSET}, &Debug_pretty_print,
		false, NULL, NULL
	},

	{
		{"log_parser_stats", PGC_SUSET}, &log_parser_stats,
		false, NULL, NULL
	},
	{
		{"log_planner_stats", PGC_SUSET}, &log_planner_stats,
		false, NULL, NULL
	},
	{
		{"log_executor_stats", PGC_SUSET}, &log_executor_stats,
		false, NULL, NULL
	},
	{
		{"log_statement_stats", PGC_SUSET}, &log_statement_stats,
		false, NULL, NULL
	},
#ifdef BTREE_BUILD_STATS
	{
		{"log_btree_build_stats", PGC_SUSET}, &log_btree_build_stats,
		false, NULL, NULL
	},
#endif

	{
		{"explain_pretty_print", PGC_USERSET}, &Explain_pretty_print,
		true, NULL, NULL
	},

	{
		{"stats_start_collector", PGC_POSTMASTER}, &pgstat_collect_startcollector,
		true, NULL, NULL
	},
	{
		{"stats_reset_on_server_start", PGC_POSTMASTER}, &pgstat_collect_resetonpmstart,
		true, NULL, NULL
	},
	{
		{"stats_command_string", PGC_SUSET}, &pgstat_collect_querystring,
		false, NULL, NULL
	},
	{
		{"stats_row_level", PGC_SUSET}, &pgstat_collect_tuplelevel,
		false, NULL, NULL
	},
	{
		{"stats_block_level", PGC_SUSET}, &pgstat_collect_blocklevel,
		false, NULL, NULL
	},

	{
		{"trace_notify", PGC_USERSET}, &Trace_notify,
		false, NULL, NULL
	},

#ifdef LOCK_DEBUG
	{
		{"trace_locks", PGC_SUSET}, &Trace_locks,
		false, NULL, NULL
	},
	{
		{"trace_userlocks", PGC_SUSET}, &Trace_userlocks,
		false, NULL, NULL
	},
	{
		{"trace_lwlocks", PGC_SUSET}, &Trace_lwlocks,
		false, NULL, NULL
	},
	{
		{"debug_deadlocks", PGC_SUSET}, &Debug_deadlocks,
		false, NULL, NULL
	},
#endif

	{
		{"log_hostname", PGC_SIGHUP}, &log_hostname,
		false, NULL, NULL
	},
	{
		{"log_source_port", PGC_SIGHUP}, &LogSourcePort,
		false, NULL, NULL
	},

	{
		{"sql_inheritance", PGC_USERSET}, &SQL_inheritance,
		true, NULL, NULL
	},
	{
		{"australian_timezones", PGC_USERSET}, &Australian_timezones,
		false, ClearDateCache, NULL
	},
	{
		{"password_encryption", PGC_USERSET}, &Password_encryption,
		true, NULL, NULL
	},
	{
		{"transform_null_equals", PGC_USERSET}, &Transform_null_equals,
		false, NULL, NULL
	},
	{
		{"db_user_namespace", PGC_SIGHUP}, &Db_user_namespace,
		false, NULL, NULL
	},
	{
		/*
		 * This var doesn't do anything; it's just here so that we won't
		 * choke on SET AUTOCOMMIT TO ON from 7.3-vintage clients.
		 */
		{"autocommit", PGC_USERSET, GUC_NO_SHOW_ALL}, &phony_autocommit,
		true, assign_phony_autocommit, NULL
	},
	{
		{"default_transaction_read_only", PGC_USERSET}, &DefaultXactReadOnly,
		false, NULL, NULL
	},
	{
		{"transaction_read_only", PGC_USERSET, GUC_NO_RESET_ALL}, &XactReadOnly,
		false, NULL, NULL
	},

	{
		{NULL, 0}, NULL, false, NULL, NULL
	}
};


static struct config_int
			ConfigureNamesInt[] =
{
	{
		{"default_statistics_target", PGC_USERSET}, &default_statistics_target,
		10, 1, 1000, NULL, NULL
	},
	{
		{"from_collapse_limit", PGC_USERSET}, &from_collapse_limit,
		8, 1, INT_MAX, NULL, NULL
	},
	{
		{"join_collapse_limit", PGC_USERSET}, &join_collapse_limit,
		8, 1, INT_MAX, NULL, NULL
	},
	{
		{"geqo_threshold", PGC_USERSET}, &geqo_threshold,
		11, 2, INT_MAX, NULL, NULL
	},
	{
		{"geqo_pool_size", PGC_USERSET}, &Geqo_pool_size,
		DEFAULT_GEQO_POOL_SIZE, 0, MAX_GEQO_POOL_SIZE, NULL, NULL
	},
	{
		{"geqo_effort", PGC_USERSET}, &Geqo_effort,
		1, 1, INT_MAX, NULL, NULL
	},
	{
		{"geqo_generations", PGC_USERSET}, &Geqo_generations,
		0, 0, INT_MAX, NULL, NULL
	},
	{
		{"geqo_random_seed", PGC_USERSET}, &Geqo_random_seed,
		-1, INT_MIN, INT_MAX, NULL, NULL
	},

	{
		{"deadlock_timeout", PGC_SIGHUP}, &DeadlockTimeout,
		1000, 0, INT_MAX, NULL, NULL
	},

#ifdef HAVE_SYSLOG
	{
		{"syslog", PGC_SIGHUP}, &Use_syslog,
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
		{"max_connections", PGC_POSTMASTER}, &MaxBackends,
		DEF_MAXBACKENDS, 1, INT_MAX, NULL, NULL
	},

	{
		{"superuser_reserved_connections", PGC_POSTMASTER}, &ReservedBackends,
		2, 0, INT_MAX, NULL, NULL
	},

	{
		{"shared_buffers", PGC_POSTMASTER}, &NBuffers,
		DEF_NBUFFERS, 16, INT_MAX, NULL, NULL
	},

	{
		{"port", PGC_POSTMASTER}, &PostPortNumber,
		DEF_PGPORT, 1, 65535, NULL, NULL
	},

	{
		{"unix_socket_permissions", PGC_POSTMASTER}, &Unix_socket_permissions,
		0777, 0000, 0777, NULL, NULL
	},

	{
		{"sort_mem", PGC_USERSET}, &SortMem,
		1024, 8 * BLCKSZ / 1024, INT_MAX, NULL, NULL
	},

	{
		{"vacuum_mem", PGC_USERSET}, &VacuumMem,
		8192, 1024, INT_MAX, NULL, NULL
	},

	{
		{"max_files_per_process", PGC_BACKEND}, &max_files_per_process,
		1000, 25, INT_MAX, NULL, NULL
	},

#ifdef LOCK_DEBUG
	{
		{"trace_lock_oidmin", PGC_SUSET}, &Trace_lock_oidmin,
		BootstrapObjectIdData, 1, INT_MAX, NULL, NULL
	},
	{
		{"trace_lock_table", PGC_SUSET}, &Trace_lock_table,
		0, 0, INT_MAX, NULL, NULL
	},
#endif
	{
		{"max_expr_depth", PGC_USERSET}, &max_expr_depth,
		DEFAULT_MAX_EXPR_DEPTH, 10, INT_MAX, NULL, NULL
	},

	{
		{"statement_timeout", PGC_USERSET}, &StatementTimeout,
		0, 0, INT_MAX, NULL, NULL
	},

	{
		{"max_fsm_relations", PGC_POSTMASTER}, &MaxFSMRelations,
		1000, 100, INT_MAX, NULL, NULL
	},
	{
		{"max_fsm_pages", PGC_POSTMASTER}, &MaxFSMPages,
		20000, 1000, INT_MAX, NULL, NULL
	},

	{
		{"max_locks_per_transaction", PGC_POSTMASTER}, &max_locks_per_xact,
		64, 10, INT_MAX, NULL, NULL
	},

	{
		{"authentication_timeout", PGC_SIGHUP}, &AuthenticationTimeout,
		60, 1, 600, NULL, NULL
	},

	/* Not for general use */
	{
		{"pre_auth_delay", PGC_SIGHUP}, &PreAuthDelay,
		0, 0, 60, NULL, NULL
	},

	{
		{"checkpoint_segments", PGC_SIGHUP}, &CheckPointSegments,
		3, 1, INT_MAX, NULL, NULL
	},

	{
		{"checkpoint_timeout", PGC_SIGHUP}, &CheckPointTimeout,
		300, 30, 3600, NULL, NULL
	},

	{
		{"checkpoint_warning", PGC_SIGHUP}, &CheckPointWarning,
		30, 0, INT_MAX, NULL, NULL
	},

	{
		{"wal_buffers", PGC_POSTMASTER}, &XLOGbuffers,
		8, 4, INT_MAX, NULL, NULL
	},

	{
		{"wal_debug", PGC_SUSET}, &XLOG_DEBUG,
		0, 0, 16, NULL, NULL
	},

	{
		{"commit_delay", PGC_USERSET}, &CommitDelay,
		0, 0, 100000, NULL, NULL
	},

	{
		{"commit_siblings", PGC_USERSET}, &CommitSiblings,
		5, 1, 1000, NULL, NULL
	},

	{
		{"extra_float_digits", PGC_USERSET}, &extra_float_digits,
		0, -15, 2, NULL, NULL
	},

	{
		{"log_min_duration_statement", PGC_USERSET}, &log_min_duration_statement,
		0, 0, INT_MAX / 1000, NULL, NULL
	},

	{
		{NULL, 0}, NULL, 0, 0, 0, NULL, NULL
	}
};


static struct config_real
			ConfigureNamesReal[] =
{
	{
		{"effective_cache_size", PGC_USERSET}, &effective_cache_size,
		DEFAULT_EFFECTIVE_CACHE_SIZE, 0, DBL_MAX, NULL, NULL
	},
	{
		{"random_page_cost", PGC_USERSET}, &random_page_cost,
		DEFAULT_RANDOM_PAGE_COST, 0, DBL_MAX, NULL, NULL
	},
	{
		{"cpu_tuple_cost", PGC_USERSET}, &cpu_tuple_cost,
		DEFAULT_CPU_TUPLE_COST, 0, DBL_MAX, NULL, NULL
	},
	{
		{"cpu_index_tuple_cost", PGC_USERSET}, &cpu_index_tuple_cost,
		DEFAULT_CPU_INDEX_TUPLE_COST, 0, DBL_MAX, NULL, NULL
	},
	{
		{"cpu_operator_cost", PGC_USERSET}, &cpu_operator_cost,
		DEFAULT_CPU_OPERATOR_COST, 0, DBL_MAX, NULL, NULL
	},

	{
		{"geqo_selection_bias", PGC_USERSET}, &Geqo_selection_bias,
		DEFAULT_GEQO_SELECTION_BIAS, MIN_GEQO_SELECTION_BIAS,
		MAX_GEQO_SELECTION_BIAS, NULL, NULL
	},

	{
		{"seed", PGC_USERSET, GUC_NO_SHOW_ALL | GUC_NO_RESET_ALL},
		&phony_random_seed,
		0.5, 0.0, 1.0, assign_random_seed, show_random_seed
	},

	{
		{NULL, 0}, NULL, 0.0, 0.0, 0.0, NULL, NULL
	}
};


static struct config_string
			ConfigureNamesString[] =
{
	{
		{"client_encoding", PGC_USERSET, GUC_REPORT},
		&client_encoding_string,
		"SQL_ASCII", assign_client_encoding, NULL
	},

	{
		{"client_min_messages", PGC_USERSET}, &client_min_messages_str,
		"notice", assign_client_min_messages, NULL
	},

	{
		{"log_min_error_statement", PGC_SUSET}, &log_min_error_statement_str,
		"panic", assign_min_error_statement, NULL
	},

	{
		{"DateStyle", PGC_USERSET, GUC_LIST_INPUT | GUC_REPORT},
		&datestyle_string,
		"ISO, US", assign_datestyle, show_datestyle
	},

	{
		{"default_transaction_isolation", PGC_USERSET}, &default_iso_level_string,
		"read committed", assign_defaultxactisolevel, NULL
	},

	{
		{"dynamic_library_path", PGC_SUSET}, &Dynamic_library_path,
		"$libdir", NULL, NULL
	},

	{
		{"krb_server_keyfile", PGC_POSTMASTER}, &pg_krb_server_keyfile,
		PG_KRB_SRVTAB, NULL, NULL
	},

	/* See main.c about why defaults for LC_foo are not all alike */

	{
		{"lc_collate", PGC_INTERNAL}, &locale_collate,
		"C", NULL, NULL
	},

	{
		{"lc_ctype", PGC_INTERNAL}, &locale_ctype,
		"C", NULL, NULL
	},

	{
		{"lc_messages", PGC_SUSET}, &locale_messages,
		"", locale_messages_assign, NULL
	},

	{
		{"lc_monetary", PGC_USERSET}, &locale_monetary,
		"C", locale_monetary_assign, NULL
	},

	{
		{"lc_numeric", PGC_USERSET}, &locale_numeric,
		"C", locale_numeric_assign, NULL
	},

	{
		{"lc_time", PGC_USERSET}, &locale_time,
		"C", locale_time_assign, NULL
	},

	{
		{"preload_libraries", PGC_POSTMASTER, GUC_LIST_INPUT | GUC_LIST_QUOTE},
		&preload_libraries_string,
		"", NULL, NULL
	},

	{
		{"regex_flavor", PGC_USERSET}, &regex_flavor_string,
		"advanced", assign_regex_flavor, NULL
	},

	{
		{"search_path", PGC_USERSET, GUC_LIST_INPUT | GUC_LIST_QUOTE},
		&namespace_search_path,
		"$user,public", assign_search_path, NULL
	},

	/* Can't be set in postgresql.conf */
	{
		{"server_encoding", PGC_INTERNAL, GUC_REPORT},
		&server_encoding_string,
		"SQL_ASCII", NULL, NULL
	},

	{
		{"server_version", PGC_INTERNAL, GUC_REPORT},
		&server_version_string,
		PG_VERSION, NULL, NULL
	},

	{
		{"log_min_messages", PGC_SUSET}, &log_min_messages_str,
		"notice", assign_log_min_messages, NULL
	},

	/* Not for general use --- used by SET SESSION AUTHORIZATION */
	{
		{"session_authorization", PGC_USERSET, GUC_NO_SHOW_ALL | GUC_NO_RESET_ALL},
		&session_authorization_string,
		NULL, assign_session_authorization, show_session_authorization
	},

#ifdef HAVE_SYSLOG
	{
		{"syslog_facility", PGC_POSTMASTER}, &Syslog_facility,
		"LOCAL0", assign_facility, NULL
	},
	{
		{"syslog_ident", PGC_POSTMASTER}, &Syslog_ident,
		"postgres", NULL, NULL
	},
#endif

	{
		{"TimeZone", PGC_USERSET}, &timezone_string,
		"UNKNOWN", assign_timezone, show_timezone
	},

	{
		{"transaction_isolation", PGC_USERSET, GUC_NO_RESET_ALL},
		&XactIsoLevel_string,
		NULL, assign_XactIsoLevel, show_XactIsoLevel
	},

	{
		{"unix_socket_group", PGC_POSTMASTER}, &Unix_socket_group,
		"", NULL, NULL
	},

	{
		{"unix_socket_directory", PGC_POSTMASTER}, &UnixSocketDir,
		"", NULL, NULL
	},

	{
		{"virtual_host", PGC_POSTMASTER}, &VirtualHost,
		"", NULL, NULL
	},

	{
		{"wal_sync_method", PGC_SIGHUP}, &XLOG_sync_method,
		XLOG_sync_method_default, assign_xlog_sync_method, NULL
	},

	{
		{NULL, 0}, NULL, NULL, NULL, NULL
	}
};

/******** end of options list ********/


/*
 * Actual lookup of variables is done through this single, sorted array.
 */
static struct config_generic **guc_variables;
static int	num_guc_variables;

static bool guc_dirty;			/* TRUE if need to do commit/abort work */

static bool reporting_enabled;	/* TRUE to enable GUC_REPORT */

static char *guc_string_workspace;		/* for avoiding memory leaks */


static int	guc_var_compare(const void *a, const void *b);
static void ReportGUCOption(struct config_generic *record);
static char *_ShowOption(struct config_generic * record);


/*
 * Build the sorted array.	This is split out so that it could be
 * re-executed after startup (eg, we could allow loadable modules to
 * add vars, and then we'd need to re-sort).
 */
static void
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
		elog(PANIC, "out of memory");

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
	 *
	 * Note: any errors here are reported with plain ol' printf, since we
	 * shouldn't assume that elog will work before we've initialized its
	 * config variables.  An error here would be unexpected anyway...
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
							fprintf(stderr, "Failed to initialize %s to %d\n",
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
					if (conf->assign_hook)
						if (!(*conf->assign_hook) (conf->reset_val, true, false))
							fprintf(stderr, "Failed to initialize %s to %d\n",
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
					if (conf->assign_hook)
						if (!(*conf->assign_hook) (conf->reset_val, true, false))
							fprintf(stderr, "Failed to initialize %s to %g\n",
									conf->gen.name, conf->reset_val);
					*conf->variable = conf->reset_val;
					conf->session_val = conf->reset_val;
					break;
				}
			case PGC_STRING:
				{
					struct config_string *conf = (struct config_string *) gconf;
					char	   *str;

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
						elog(PANIC, "out of memory");
					conf->reset_val = str;

					if (conf->assign_hook)
					{
						const char *newstr;

						newstr = (*conf->assign_hook) (str, true, false);
						if (newstr == NULL)
						{
							fprintf(stderr, "Failed to initialize %s to '%s'\n",
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
		if (gconf->context != PGC_SUSET && gconf->context != PGC_USERSET)
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
							elog(ERROR, "Failed to reset %s", conf->gen.name);
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
							elog(ERROR, "Failed to reset %s", conf->gen.name);
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
							elog(ERROR, "Failed to reset %s", conf->gen.name);
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
							elog(ERROR, "Failed to reset %s", conf->gen.name);
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

	/* Prevent memory leak if elog during an assign_hook */
	if (guc_string_workspace)
	{
		free(guc_string_workspace);
		guc_string_workspace = NULL;
	}

	for (i = 0; i < num_guc_variables; i++)
	{
		struct config_generic *gconf = guc_variables[i];
		bool	changed;

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
								elog(LOG, "Failed to commit %s", conf->gen.name);
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
								elog(LOG, "Failed to commit %s", conf->gen.name);
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
								elog(LOG, "Failed to commit %s", conf->gen.name);
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
								elog(LOG, "Failed to commit %s", conf->gen.name);
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
ReportGUCOption(struct config_generic *record)
{
	if (reporting_enabled && (record->flags & GUC_REPORT))
	{
		char	*val = _ShowOption(record);
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
 * parameter DoIt is false then don't really set the option but do all
 * the checks to see if it would work.
 *
 * If there is an error (non-existing option, invalid value) then an
 * elog(ERROR) is thrown *unless* this is called in a context where we
 * don't want to elog (currently, startup or SIGHUP config file reread).
 * In that case we write a suitable error message via elog(DEBUG) and
 * return false. This is working around the deficiencies in the elog
 * mechanism, so don't blame me.  In all other cases, the function
 * returns true, including cases where the input is valid but we chose
 * not to apply it because of context or source-priority considerations.
 *
 * See also SetConfigOption for an external interface.
 */
bool
set_config_option(const char *name, const char *value,
				  GucContext context, GucSource source,
				  bool isLocal, bool DoIt)
{
	struct config_generic *record;
	int			elevel;
	bool		interactive;
	bool		makeDefault;

	if (context == PGC_SIGHUP || source == PGC_S_DEFAULT)
		elevel = DEBUG2;
	else if (source == PGC_S_DATABASE || source == PGC_S_USER)
		elevel = INFO;
	else
		elevel = ERROR;

	record = find_option(name);
	if (record == NULL)
	{
		elog(elevel, "'%s' is not a valid option name", name);
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
				elog(elevel, "'%s' cannot be changed",
					 name);
				return false;
			}
			break;
		case PGC_POSTMASTER:
			if (context == PGC_SIGHUP)
				return true;
			if (context != PGC_POSTMASTER)
			{
				elog(elevel, "'%s' cannot be changed after server start",
					 name);
				return false;
			}
			break;
		case PGC_SIGHUP:
			if (context != PGC_SIGHUP && context != PGC_POSTMASTER)
			{
				elog(elevel, "'%s' cannot be changed now", name);
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
				elog(elevel, "'%s' cannot be set after connection start",
					 name);
				return false;
			}
			break;
		case PGC_SUSET:
			if (context == PGC_USERSET || context == PGC_BACKEND)
			{
				elog(elevel, "'%s': permission denied", name);
				return false;
			}
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
	makeDefault = DoIt && (source <= PGC_S_OVERRIDE) && (value != NULL);

	/*
	 * Ignore attempted set if overridden by previously processed setting.
	 * However, if DoIt is false then plow ahead anyway since we are
	 * trying to find out if the value is potentially good, not actually
	 * use it. Also keep going if makeDefault is true, since we may want
	 * to set the reset/session values even if we can't set the variable
	 * itself.
	 */
	if (record->source > source)
	{
		if (DoIt && !makeDefault)
		{
			elog(DEBUG3, "%s: setting ignored because previous source is higher priority",
				 name);
			return true;
		}
		DoIt = false;			/* we won't change the variable itself */
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
						elog(elevel, "option '%s' requires a boolean value",
							 name);
						return false;
					}
				}
				else
				{
					newval = conf->reset_val;
					source = conf->gen.reset_source;
				}

				if (conf->assign_hook)
					if (!(*conf->assign_hook) (newval, DoIt, interactive))
					{
						elog(elevel, "invalid value for option '%s': %d",
							 name, (int) newval);
						return false;
					}

				if (DoIt || makeDefault)
				{
					if (DoIt)
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
						elog(elevel, "option '%s' expects an integer value",
							 name);
						return false;
					}
					if (newval < conf->min || newval > conf->max)
					{
						elog(elevel, "option '%s' value %d is outside"
							 " of permissible range [%d .. %d]",
							 name, newval, conf->min, conf->max);
						return false;
					}
				}
				else
				{
					newval = conf->reset_val;
					source = conf->gen.reset_source;
				}

				if (conf->assign_hook)
					if (!(*conf->assign_hook) (newval, DoIt, interactive))
					{
						elog(elevel, "invalid value for option '%s': %d",
							 name, newval);
						return false;
					}

				if (DoIt || makeDefault)
				{
					if (DoIt)
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
						elog(elevel, "option '%s' expects a real number",
							 name);
						return false;
					}
					if (newval < conf->min || newval > conf->max)
					{
						elog(elevel, "option '%s' value %g is outside"
							 " of permissible range [%g .. %g]",
							 name, newval, conf->min, conf->max);
						return false;
					}
				}
				else
				{
					newval = conf->reset_val;
					source = conf->gen.reset_source;
				}

				if (conf->assign_hook)
					if (!(*conf->assign_hook) (newval, DoIt, interactive))
					{
						elog(elevel, "invalid value for option '%s': %g",
							 name, newval);
						return false;
					}

				if (DoIt || makeDefault)
				{
					if (DoIt)
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
						elog(elevel, "out of memory");
						return false;
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
						elog(elevel, "out of memory");
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
				 * and avoid a permanent memory leak if hook elogs.
				 */
				if (guc_string_workspace)
					free(guc_string_workspace);
				guc_string_workspace = newval;

				if (conf->assign_hook)
				{
					const char *hookresult;

					hookresult = (*conf->assign_hook) (newval,
													   DoIt, interactive);
					guc_string_workspace = NULL;
					if (hookresult == NULL)
					{
						free(newval);
						elog(elevel, "invalid value for option '%s': '%s'",
							 name, value ? value : "");
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

				if (DoIt || makeDefault)
				{
					if (DoIt)
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

	if (DoIt && (record->flags & GUC_REPORT))
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
 * throw an elog and don't return.
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
		elog(ERROR, "Option '%s' is not recognized", name);

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
		elog(ERROR, "Option '%s' is not recognized", name);

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
		elog(ERROR, "'%s' is not a valid option name", name);

	flags = record->flags;

	/* Complain if list input and non-list variable */
	if ((flags & GUC_LIST_INPUT) == 0 &&
		lnext(args) != NIL)
		elog(ERROR, "SET %s takes only one argument", name);

	initStringInfo(&buf);

	foreach(l, args)
	{
		A_Const    *arg = (A_Const *) lfirst(l);
		char	   *val;

		if (l != args)
			appendStringInfo(&buf, ", ");

		if (!IsA(arg, A_Const))
			elog(ERROR, "flatten_set_variable_args: unexpected input");

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
				elog(ERROR, "flatten_set_variable_args: unexpected input");
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
		elog(ERROR, "SET variable name is required");

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
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, (char *) varname,
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
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, (char *) varname,
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
		elog(ERROR, "Option '%s' is not recognized", name);

	if (varname)
		*varname = record->name;

	return _ShowOption(record);
}

/*
 * Return GUC variable value by variable number; optionally return canonical
 * form of name.  Return value is palloc'd.
 */
char *
GetConfigOptionByNum(int varnum, const char **varname, bool *noshow)
{
	struct config_generic *conf;

	/* check requested variable number valid */
	Assert((varnum >= 0) && (varnum < num_guc_variables));

	conf = guc_variables[varnum];

	if (varname)
		*varname = conf->name;

	if (noshow)
		*noshow = (conf->flags & GUC_NO_SHOW_ALL) ? true : false;

	return _ShowOption(conf);
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

		/* need a tuple descriptor representing two TEXT columns */
		tupdesc = CreateTemplateTupleDesc(2, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "name",
						   TEXTOID, -1, 0, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "setting",
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
		char	   *values[2];
		char	   *varname;
		char	   *varval;
		bool		noshow;
		HeapTuple	tuple;
		Datum		result;

		/*
		 * Get the next visible GUC variable name and value
		 */
		do
		{
			varval = GetConfigOptionByNum(call_cntr,
										  (const char **) &varname,
										  &noshow);
			if (noshow)
			{
				/* varval is a palloc'd copy, so free it */
				if (varval != NULL)
					pfree(varval);

				/* bump the counter and get the next config setting */
				call_cntr = ++funcctx->call_cntr;

				/* make sure we haven't gone too far now */
				if (call_cntr >= max_calls)
					SRF_RETURN_DONE(funcctx);
			}
		} while (noshow);

		/*
		 * Prepare a values array for storage in our slot. This should be
		 * an array of C strings which will be processed later by the
		 * appropriate "in" functions.
		 */
		values[0] = varname;
		values[1] = varval;

		/* build a tuple */
		tuple = BuildTupleFromCStrings(attinmeta, values);

		/* make the tuple into a datum */
		result = TupleGetDatum(slot, tuple);

		/* Clean up */
		if (varval != NULL)
			pfree(varval);

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
	int i;
	char *new_filename, *filename;
	int elevel;
	FILE *fp;

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
		elog(elevel, "out of memory");
		return;
	}
	sprintf(new_filename, "%s/" CONFIG_EXEC_PARAMS ".new", DataDir);
	sprintf(filename, "%s/" CONFIG_EXEC_PARAMS, DataDir);

    fp = AllocateFile(new_filename, "w");
    if (!fp)
    {
 		free(new_filename);
		free(filename);
		elog(elevel, "could not write exec config params file `"
					CONFIG_EXEC_PARAMS "': %s", strerror(errno));
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
	return;
}


/*
 *	Read string, including null byte from file
 *
 *	Return NULL on EOF and nothing read
 */
static char *
read_string_with_null(FILE *fp)
{
	int i = 0, ch, maxlen = 256;
	char *str = NULL;

	do
	{
		if ((ch = fgetc(fp)) == EOF)
		{
			if (i == 0)
				return NULL;
			else
				elog(FATAL, "Invalid format of exec config params file");
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
	char *filename;
	FILE *fp;
	char *varname, *varvalue;
	int varsource;

	Assert(DataDir);

	/*
	 * Open file
	 */
	filename = malloc(strlen(DataDir) + strlen(CONFIG_EXEC_PARAMS) + 2);
	if (filename == NULL)
	{
		elog(ERROR, "out of memory");
		return;
	}
	sprintf(filename, "%s/" CONFIG_EXEC_PARAMS, DataDir);

    fp = AllocateFile(filename, "r");
    if (!fp)
    {
		free(filename);
        /* File not found is fine */
        if (errno != ENOENT)
            elog(FATAL, "could not read exec config params file `"
					CONFIG_EXEC_PARAMS "': %s", strerror(errno));
		return;
    }

    while (1)
	{
		if ((varname = read_string_with_null(fp)) == NULL)
			break;

		if ((varvalue = read_string_with_null(fp)) == NULL)
			elog(FATAL, "Invalid format of exec config params file");
 		if (fread(&varsource, sizeof(varsource), 1, fp) == 0)
			elog(FATAL, "Invalid format of exec config params file");

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
			elog(FATAL, "out of memory");
		strncpy(*name, string, equal_pos);
		(*name)[equal_pos] = '\0';

		*value = strdup(&string[equal_pos + 1]);
		if (!*value)
			elog(FATAL, "out of memory");
	}
	else
	{
		/* no equal sign in string */
		*name = strdup(string);
		if (!*name)
			elog(FATAL, "out of memory");
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
			elog(WARNING, "cannot parse setting \"%s\"", name);
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

static bool
assign_phony_autocommit(bool newval, bool doit, bool interactive)
{
	if (!newval)
	{
		if (doit && interactive)
			elog(ERROR, "SET AUTOCOMMIT TO OFF is no longer supported");
		return false;
	}
	return true;
}


#include "guc-file.c"

