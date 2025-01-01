/*--------------------------------------------------------------------
 * guc.h
 *
 * External declarations pertaining to Grand Unified Configuration.
 *
 * Copyright (c) 2000-2025, PostgreSQL Global Development Group
 * Written by Peter Eisentraut <peter_e@gmx.net>.
 *
 * src/include/utils/guc.h
 *--------------------------------------------------------------------
 */
#ifndef GUC_H
#define GUC_H

#include "nodes/parsenodes.h"
#include "tcop/dest.h"
#include "utils/array.h"


/* upper limit for GUC variables measured in kilobytes of memory */
/* note that various places assume the byte size fits in a "long" variable */
#if SIZEOF_SIZE_T > 4 && SIZEOF_LONG > 4
#define MAX_KILOBYTES	INT_MAX
#else
#define MAX_KILOBYTES	(INT_MAX / 1024)
#endif

/*
 * Automatic configuration file name for ALTER SYSTEM.
 * This file will be used to store values of configuration parameters
 * set by ALTER SYSTEM command.
 */
#define PG_AUTOCONF_FILENAME		"postgresql.auto.conf"

/*
 * Certain options can only be set at certain times. The rules are
 * like this:
 *
 * INTERNAL options cannot be set by the user at all, but only through
 * internal processes ("server_version" is an example).  These are GUC
 * variables only so they can be shown by SHOW, etc.
 *
 * POSTMASTER options can only be set when the postmaster starts,
 * either from the configuration file or the command line.
 *
 * SIGHUP options can only be set at postmaster startup or by changing
 * the configuration file and sending the HUP signal to the postmaster
 * or a backend process. (Notice that the signal receipt will not be
 * evaluated immediately. The postmaster and the backend check it at a
 * certain point in their main loop. It's safer to wait than to read a
 * file asynchronously.)
 *
 * BACKEND and SU_BACKEND options can only be set at postmaster startup,
 * from the configuration file, or by client request in the connection
 * startup packet (e.g., from libpq's PGOPTIONS variable).  SU_BACKEND
 * options can be set from the startup packet only when the user is a
 * superuser.  Furthermore, an already-started backend will ignore changes
 * to such an option in the configuration file.  The idea is that these
 * options are fixed for a given backend once it's started, but they can
 * vary across backends.
 *
 * SUSET options can be set at postmaster startup, with the SIGHUP
 * mechanism, or from the startup packet or SQL if you're a superuser.
 *
 * USERSET options can be set by anyone any time.
 */
typedef enum
{
	PGC_INTERNAL,
	PGC_POSTMASTER,
	PGC_SIGHUP,
	PGC_SU_BACKEND,
	PGC_BACKEND,
	PGC_SUSET,
	PGC_USERSET,
} GucContext;

/*
 * The following type records the source of the current setting.  A
 * new setting can only take effect if the previous setting had the
 * same or lower level.  (E.g, changing the config file doesn't
 * override the postmaster command line.)  Tracking the source allows us
 * to process sources in any convenient order without affecting results.
 * Sources <= PGC_S_OVERRIDE will set the default used by RESET, as well
 * as the current value.
 *
 * PGC_S_INTERACTIVE isn't actually a source value, but is the
 * dividing line between "interactive" and "non-interactive" sources for
 * error reporting purposes.
 *
 * PGC_S_TEST is used when testing values to be used later.  For example,
 * ALTER DATABASE/ROLE tests proposed per-database or per-user defaults this
 * way, and CREATE FUNCTION tests proposed function SET clauses this way.
 * This is an interactive case, but it needs its own source value because
 * some assign hooks need to make different validity checks in this case.
 * In particular, references to nonexistent database objects generally
 * shouldn't throw hard errors in this case, at most NOTICEs, since the
 * objects might exist by the time the setting is used for real.
 *
 * When setting the value of a non-compile-time-constant PGC_INTERNAL option,
 * source == PGC_S_DYNAMIC_DEFAULT should typically be used so that the value
 * will show as "default" in pg_settings.  If there is a specific reason not
 * to want that, use source == PGC_S_OVERRIDE.
 *
 * NB: see GucSource_Names in guc.c if you change this.
 */
typedef enum
{
	PGC_S_DEFAULT,				/* hard-wired default ("boot_val") */
	PGC_S_DYNAMIC_DEFAULT,		/* default computed during initialization */
	PGC_S_ENV_VAR,				/* postmaster environment variable */
	PGC_S_FILE,					/* postgresql.conf */
	PGC_S_ARGV,					/* postmaster command line */
	PGC_S_GLOBAL,				/* global in-database setting */
	PGC_S_DATABASE,				/* per-database setting */
	PGC_S_USER,					/* per-user setting */
	PGC_S_DATABASE_USER,		/* per-user-and-database setting */
	PGC_S_CLIENT,				/* from client connection request */
	PGC_S_OVERRIDE,				/* special case to forcibly set default */
	PGC_S_INTERACTIVE,			/* dividing line for error reporting */
	PGC_S_TEST,					/* test per-database or per-user setting */
	PGC_S_SESSION,				/* SET command */
} GucSource;

/*
 * Parsing the configuration file(s) will return a list of name-value pairs
 * with source location info.  We also abuse this data structure to carry
 * error reports about the config files.  An entry reporting an error will
 * have errmsg != NULL, and might have NULLs for name, value, and/or filename.
 *
 * If "ignore" is true, don't attempt to apply the item (it might be an error
 * report, or an item we determined to be duplicate).  "applied" is set true
 * if we successfully applied, or could have applied, the setting.
 */
typedef struct ConfigVariable
{
	char	   *name;
	char	   *value;
	char	   *errmsg;
	char	   *filename;
	int			sourceline;
	bool		ignore;
	bool		applied;
	struct ConfigVariable *next;
} ConfigVariable;

typedef struct config_generic config_handle;

extern bool ParseConfigFile(const char *config_file, bool strict,
							const char *calling_file, int calling_lineno,
							int depth, int elevel,
							ConfigVariable **head_p, ConfigVariable **tail_p);
extern bool ParseConfigFp(FILE *fp, const char *config_file,
						  int depth, int elevel,
						  ConfigVariable **head_p, ConfigVariable **tail_p);
extern bool ParseConfigDirectory(const char *includedir,
								 const char *calling_file, int calling_lineno,
								 int depth, int elevel,
								 ConfigVariable **head_p,
								 ConfigVariable **tail_p);
extern void FreeConfigVariables(ConfigVariable *list);
extern char *DeescapeQuotedString(const char *s);

/*
 * The possible values of an enum variable are specified by an array of
 * name-value pairs.  The "hidden" flag means the value is accepted but
 * won't be displayed when guc.c is asked for a list of acceptable values.
 */
struct config_enum_entry
{
	const char *name;
	int			val;
	bool		hidden;
};

/*
 * Signatures for per-variable check/assign/show hook functions
 */
typedef bool (*GucBoolCheckHook) (bool *newval, void **extra, GucSource source);
typedef bool (*GucIntCheckHook) (int *newval, void **extra, GucSource source);
typedef bool (*GucRealCheckHook) (double *newval, void **extra, GucSource source);
typedef bool (*GucStringCheckHook) (char **newval, void **extra, GucSource source);
typedef bool (*GucEnumCheckHook) (int *newval, void **extra, GucSource source);

typedef void (*GucBoolAssignHook) (bool newval, void *extra);
typedef void (*GucIntAssignHook) (int newval, void *extra);
typedef void (*GucRealAssignHook) (double newval, void *extra);
typedef void (*GucStringAssignHook) (const char *newval, void *extra);
typedef void (*GucEnumAssignHook) (int newval, void *extra);

typedef const char *(*GucShowHook) (void);

/*
 * Miscellaneous
 */
typedef enum
{
	/* Types of set_config_option actions */
	GUC_ACTION_SET,				/* regular SET command */
	GUC_ACTION_LOCAL,			/* SET LOCAL command */
	GUC_ACTION_SAVE,			/* function SET option, or temp assignment */
} GucAction;

#define GUC_QUALIFIER_SEPARATOR '.'

/*
 * Bit values in "flags" of a GUC variable.  Note that these don't appear
 * on disk, so we can reassign their values freely.
 */
#define GUC_LIST_INPUT		   0x000001 /* input can be list format */
#define GUC_LIST_QUOTE		   0x000002 /* double-quote list elements */
#define GUC_NO_SHOW_ALL		   0x000004 /* exclude from SHOW ALL */
#define GUC_NO_RESET		   0x000008 /* disallow RESET and SAVE */
#define GUC_NO_RESET_ALL	   0x000010 /* exclude from RESET ALL */
#define GUC_EXPLAIN			   0x000020 /* include in EXPLAIN */
#define GUC_REPORT			   0x000040 /* auto-report changes to client */
#define GUC_NOT_IN_SAMPLE	   0x000080 /* not in postgresql.conf.sample */
#define GUC_DISALLOW_IN_FILE   0x000100 /* can't set in postgresql.conf */
#define GUC_CUSTOM_PLACEHOLDER 0x000200 /* placeholder for custom variable */
#define GUC_SUPERUSER_ONLY	   0x000400 /* show only to superusers */
#define GUC_IS_NAME			   0x000800 /* limit string to NAMEDATALEN-1 */
#define GUC_NOT_WHILE_SEC_REST 0x001000 /* can't set if security restricted */
#define GUC_DISALLOW_IN_AUTO_FILE \
							   0x002000 /* can't set in PG_AUTOCONF_FILENAME */
#define GUC_RUNTIME_COMPUTED   0x004000 /* delay processing in 'postgres -C' */
#define GUC_ALLOW_IN_PARALLEL  0x008000 /* allow setting in parallel mode */

#define GUC_UNIT_KB			 0x01000000 /* value is in kilobytes */
#define GUC_UNIT_BLOCKS		 0x02000000 /* value is in blocks */
#define GUC_UNIT_XBLOCKS	 0x03000000 /* value is in xlog blocks */
#define GUC_UNIT_MB			 0x04000000 /* value is in megabytes */
#define GUC_UNIT_BYTE		 0x05000000 /* value is in bytes */
#define GUC_UNIT_MEMORY		 0x0F000000 /* mask for size-related units */

#define GUC_UNIT_MS			 0x10000000 /* value is in milliseconds */
#define GUC_UNIT_S			 0x20000000 /* value is in seconds */
#define GUC_UNIT_MIN		 0x30000000 /* value is in minutes */
#define GUC_UNIT_TIME		 0x70000000 /* mask for time-related units */

#define GUC_UNIT			 (GUC_UNIT_MEMORY | GUC_UNIT_TIME)


/* GUC vars that are actually defined in guc_tables.c, rather than elsewhere */
extern PGDLLIMPORT bool Debug_print_plan;
extern PGDLLIMPORT bool Debug_print_parse;
extern PGDLLIMPORT bool Debug_print_rewritten;
extern PGDLLIMPORT bool Debug_pretty_print;

#ifdef DEBUG_NODE_TESTS_ENABLED
extern PGDLLIMPORT bool Debug_copy_parse_plan_trees;
extern PGDLLIMPORT bool Debug_write_read_parse_plan_trees;
extern PGDLLIMPORT bool Debug_raw_expression_coverage_test;
#endif

extern PGDLLIMPORT bool log_parser_stats;
extern PGDLLIMPORT bool log_planner_stats;
extern PGDLLIMPORT bool log_executor_stats;
extern PGDLLIMPORT bool log_statement_stats;
extern PGDLLIMPORT bool log_btree_build_stats;
extern PGDLLIMPORT char *event_source;

extern PGDLLIMPORT bool check_function_bodies;
extern PGDLLIMPORT bool current_role_is_superuser;

extern PGDLLIMPORT bool AllowAlterSystem;
extern PGDLLIMPORT bool log_duration;
extern PGDLLIMPORT int log_parameter_max_length;
extern PGDLLIMPORT int log_parameter_max_length_on_error;
extern PGDLLIMPORT int log_min_error_statement;
extern PGDLLIMPORT int log_min_messages;
extern PGDLLIMPORT int client_min_messages;
extern PGDLLIMPORT int log_min_duration_sample;
extern PGDLLIMPORT int log_min_duration_statement;
extern PGDLLIMPORT int log_temp_files;
extern PGDLLIMPORT double log_statement_sample_rate;
extern PGDLLIMPORT double log_xact_sample_rate;
extern PGDLLIMPORT char *backtrace_functions;

extern PGDLLIMPORT int temp_file_limit;

extern PGDLLIMPORT int num_temp_buffers;

extern PGDLLIMPORT char *cluster_name;
extern PGDLLIMPORT char *ConfigFileName;
extern PGDLLIMPORT char *HbaFileName;
extern PGDLLIMPORT char *IdentFileName;
extern PGDLLIMPORT char *external_pid_file;

extern PGDLLIMPORT char *application_name;

extern PGDLLIMPORT int tcp_keepalives_idle;
extern PGDLLIMPORT int tcp_keepalives_interval;
extern PGDLLIMPORT int tcp_keepalives_count;
extern PGDLLIMPORT int tcp_user_timeout;

extern PGDLLIMPORT char *role_string;
extern PGDLLIMPORT bool in_hot_standby_guc;
extern PGDLLIMPORT bool trace_sort;

#ifdef DEBUG_BOUNDED_SORT
extern PGDLLIMPORT bool optimize_bounded_sort;
#endif

/*
 * Declarations for options for enum values
 *
 * For most parameters, these are defined statically inside guc_tables.c.  But
 * for some parameters, the definitions require symbols that are not easily
 * available inside guc_tables.c, so they are instead defined in their home
 * modules.  For those, we keep the extern declarations here.  (An alternative
 * would be to put the extern declarations in the modules' header files, but
 * that would then require including the definition of struct
 * config_enum_entry into those header files.)
 */
extern PGDLLIMPORT const struct config_enum_entry archive_mode_options[];
extern PGDLLIMPORT const struct config_enum_entry dynamic_shared_memory_options[];
extern PGDLLIMPORT const struct config_enum_entry recovery_target_action_options[];
extern PGDLLIMPORT const struct config_enum_entry wal_level_options[];
extern PGDLLIMPORT const struct config_enum_entry wal_sync_method_options[];

/*
 * Functions exported by guc.c
 */
extern void SetConfigOption(const char *name, const char *value,
							GucContext context, GucSource source);

extern void DefineCustomBoolVariable(const char *name,
									 const char *short_desc,
									 const char *long_desc,
									 bool *valueAddr,
									 bool bootValue,
									 GucContext context,
									 int flags,
									 GucBoolCheckHook check_hook,
									 GucBoolAssignHook assign_hook,
									 GucShowHook show_hook) pg_attribute_nonnull(1, 4);

extern void DefineCustomIntVariable(const char *name,
									const char *short_desc,
									const char *long_desc,
									int *valueAddr,
									int bootValue,
									int minValue,
									int maxValue,
									GucContext context,
									int flags,
									GucIntCheckHook check_hook,
									GucIntAssignHook assign_hook,
									GucShowHook show_hook) pg_attribute_nonnull(1, 4);

extern void DefineCustomRealVariable(const char *name,
									 const char *short_desc,
									 const char *long_desc,
									 double *valueAddr,
									 double bootValue,
									 double minValue,
									 double maxValue,
									 GucContext context,
									 int flags,
									 GucRealCheckHook check_hook,
									 GucRealAssignHook assign_hook,
									 GucShowHook show_hook) pg_attribute_nonnull(1, 4);

extern void DefineCustomStringVariable(const char *name,
									   const char *short_desc,
									   const char *long_desc,
									   char **valueAddr,
									   const char *bootValue,
									   GucContext context,
									   int flags,
									   GucStringCheckHook check_hook,
									   GucStringAssignHook assign_hook,
									   GucShowHook show_hook) pg_attribute_nonnull(1, 4);

extern void DefineCustomEnumVariable(const char *name,
									 const char *short_desc,
									 const char *long_desc,
									 int *valueAddr,
									 int bootValue,
									 const struct config_enum_entry *options,
									 GucContext context,
									 int flags,
									 GucEnumCheckHook check_hook,
									 GucEnumAssignHook assign_hook,
									 GucShowHook show_hook) pg_attribute_nonnull(1, 4);

extern void MarkGUCPrefixReserved(const char *className);

/* old name for MarkGUCPrefixReserved, for backwards compatibility: */
#define EmitWarningsOnPlaceholders(className) MarkGUCPrefixReserved(className)

extern const char *GetConfigOption(const char *name, bool missing_ok,
								   bool restrict_privileged);
extern const char *GetConfigOptionResetString(const char *name);
extern int	GetConfigOptionFlags(const char *name, bool missing_ok);
extern void ProcessConfigFile(GucContext context);
extern char *convert_GUC_name_for_parameter_acl(const char *name);
extern void check_GUC_name_for_parameter_acl(const char *name);
extern void InitializeGUCOptions(void);
extern bool SelectConfigFiles(const char *userDoption, const char *progname);
extern void ResetAllOptions(void);
extern void AtStart_GUC(void);
extern int	NewGUCNestLevel(void);
extern void RestrictSearchPath(void);
extern void AtEOXact_GUC(bool isCommit, int nestLevel);
extern void BeginReportingGUCOptions(void);
extern void ReportChangedGUCOptions(void);
extern void ParseLongOption(const char *string, char **name, char **value);
extern const char *get_config_unit_name(int flags);
extern bool parse_int(const char *value, int *result, int flags,
					  const char **hintmsg);
extern bool parse_real(const char *value, double *result, int flags,
					   const char **hintmsg);
extern int	set_config_option(const char *name, const char *value,
							  GucContext context, GucSource source,
							  GucAction action, bool changeVal, int elevel,
							  bool is_reload);
extern int	set_config_option_ext(const char *name, const char *value,
								  GucContext context, GucSource source,
								  Oid srole,
								  GucAction action, bool changeVal, int elevel,
								  bool is_reload);
extern int	set_config_with_handle(const char *name, config_handle *handle,
								   const char *value,
								   GucContext context, GucSource source,
								   Oid srole,
								   GucAction action, bool changeVal,
								   int elevel, bool is_reload);
extern config_handle *get_config_handle(const char *name);
extern void AlterSystemSetConfigFile(AlterSystemStmt *altersysstmt);
extern char *GetConfigOptionByName(const char *name, const char **varname,
								   bool missing_ok);

extern void TransformGUCArray(ArrayType *array, List **names,
							  List **values);
extern void ProcessGUCArray(ArrayType *array,
							GucContext context, GucSource source, GucAction action);
extern ArrayType *GUCArrayAdd(ArrayType *array, const char *name, const char *value);
extern ArrayType *GUCArrayDelete(ArrayType *array, const char *name);
extern ArrayType *GUCArrayReset(ArrayType *array);

extern void *guc_malloc(int elevel, size_t size);
extern pg_nodiscard void *guc_realloc(int elevel, void *old, size_t size);
extern char *guc_strdup(int elevel, const char *src);
extern void guc_free(void *ptr);

#ifdef EXEC_BACKEND
extern void write_nondefault_variables(GucContext context);
extern void read_nondefault_variables(void);
#endif

/* GUC serialization */
extern Size EstimateGUCStateSpace(void);
extern void SerializeGUCState(Size maxsize, char *start_address);
extern void RestoreGUCState(void *gucstate);

/* Functions exported by guc_funcs.c */
extern void ExecSetVariableStmt(VariableSetStmt *stmt, bool isTopLevel);
extern char *ExtractSetVariableArgs(VariableSetStmt *stmt);
extern void SetPGVariable(const char *name, List *args, bool is_local);
extern void GetPGVariable(const char *name, DestReceiver *dest);
extern TupleDesc GetPGVariableResultDesc(const char *name);

/* Support for messages reported from GUC check hooks */

extern PGDLLIMPORT char *GUC_check_errmsg_string;
extern PGDLLIMPORT char *GUC_check_errdetail_string;
extern PGDLLIMPORT char *GUC_check_errhint_string;

extern void GUC_check_errcode(int sqlerrcode);

#define GUC_check_errmsg \
	pre_format_elog_string(errno, TEXTDOMAIN), \
	GUC_check_errmsg_string = format_elog_string

#define GUC_check_errdetail \
	pre_format_elog_string(errno, TEXTDOMAIN), \
	GUC_check_errdetail_string = format_elog_string

#define GUC_check_errhint \
	pre_format_elog_string(errno, TEXTDOMAIN), \
	GUC_check_errhint_string = format_elog_string

#endif							/* GUC_H */
