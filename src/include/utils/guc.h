/*--------------------------------------------------------------------
 * guc.h
 *
 * External declarations pertaining to backend/utils/misc/guc.c and
 * backend/utils/misc/guc-file.l
 *
 * Copyright (c) 2000-2010, PostgreSQL Global Development Group
 * Written by Peter Eisentraut <peter_e@gmx.net>.
 *
 * $PostgreSQL: pgsql/src/include/utils/guc.h,v 1.113 2010/03/25 14:44:34 alvherre Exp $
 *--------------------------------------------------------------------
 */
#ifndef GUC_H
#define GUC_H

#include "nodes/parsenodes.h"
#include "tcop/dest.h"
#include "utils/array.h"


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
 * BACKEND options can only be set at postmaster startup, from the
 * configuration file, or by client request in the connection startup
 * packet (e.g., from libpq's PGOPTIONS variable).  Furthermore, an
 * already-started backend will ignore changes to such an option in the
 * configuration file.  The idea is that these options are fixed for a
 * given backend once it's started, but they can vary across backends.
 *
 * SUSET options can be set at postmaster startup, with the SIGHUP
 * mechanism, or from SQL if you're a superuser.
 *
 * USERSET options can be set by anyone any time.
 */
typedef enum
{
	PGC_INTERNAL,
	PGC_POSTMASTER,
	PGC_SIGHUP,
	PGC_BACKEND,
	PGC_SUSET,
	PGC_USERSET
} GucContext;

/*
 * The following type records the source of the current setting.  A
 * new setting can only take effect if the previous setting had the
 * same or lower level.  (E.g, changing the config file doesn't
 * override the postmaster command line.)  Tracking the source allows us
 * to process sources in any convenient order without affecting results.
 * Sources <= PGC_S_OVERRIDE will set the default used by RESET, as well
 * as the current value.  Note that source == PGC_S_OVERRIDE should be
 * used when setting a PGC_INTERNAL option.
 *
 * PGC_S_INTERACTIVE isn't actually a source value, but is the
 * dividing line between "interactive" and "non-interactive" sources for
 * error reporting purposes.
 *
 * PGC_S_TEST is used when testing values to be stored as per-database or
 * per-user defaults ("doit" will always be false, so this never gets stored
 * as the actual source of any value).  This is an interactive case, but
 * it needs its own source value because some assign hooks need to make
 * different validity checks in this case.
 *
 * NB: see GucSource_Names in guc.c if you change this.
 */
typedef enum
{
	PGC_S_DEFAULT,				/* wired-in default */
	PGC_S_ENV_VAR,				/* postmaster environment variable */
	PGC_S_FILE,					/* postgresql.conf */
	PGC_S_ARGV,					/* postmaster command line */
	PGC_S_DATABASE,				/* per-database setting */
	PGC_S_USER,					/* per-user setting */
	PGC_S_DATABASE_USER,		/* per-user-and-database setting */
	PGC_S_CLIENT,				/* from client connection request */
	PGC_S_OVERRIDE,				/* special case to forcibly set default */
	PGC_S_INTERACTIVE,			/* dividing line for error reporting */
	PGC_S_TEST,					/* test per-database or per-user setting */
	PGC_S_SESSION				/* SET command */
} GucSource;

/*
 * Enum values are made up of an array of name-value pairs
 */
struct config_enum_entry
{
	const char *name;
	int			val;
	bool		hidden;
};


typedef const char *(*GucStringAssignHook) (const char *newval, bool doit, GucSource source);
typedef bool (*GucBoolAssignHook) (bool newval, bool doit, GucSource source);
typedef bool (*GucIntAssignHook) (int newval, bool doit, GucSource source);
typedef bool (*GucRealAssignHook) (double newval, bool doit, GucSource source);
typedef bool (*GucEnumAssignHook) (int newval, bool doit, GucSource source);

typedef const char *(*GucShowHook) (void);

typedef enum
{
	/* Types of set_config_option actions */
	GUC_ACTION_SET,				/* regular SET command */
	GUC_ACTION_LOCAL,			/* SET LOCAL command */
	GUC_ACTION_SAVE				/* function SET option */
} GucAction;

#define GUC_QUALIFIER_SEPARATOR '.'

/*
 * bit values in "flags" of a GUC variable
 */
#define GUC_LIST_INPUT			0x0001	/* input can be list format */
#define GUC_LIST_QUOTE			0x0002	/* double-quote list elements */
#define GUC_NO_SHOW_ALL			0x0004	/* exclude from SHOW ALL */
#define GUC_NO_RESET_ALL		0x0008	/* exclude from RESET ALL */
#define GUC_REPORT				0x0010	/* auto-report changes to client */
#define GUC_NOT_IN_SAMPLE		0x0020	/* not in postgresql.conf.sample */
#define GUC_DISALLOW_IN_FILE	0x0040	/* can't set in postgresql.conf */
#define GUC_CUSTOM_PLACEHOLDER	0x0080	/* placeholder for custom variable */
#define GUC_SUPERUSER_ONLY		0x0100	/* show only to superusers */
#define GUC_IS_NAME				0x0200	/* limit string to NAMEDATALEN-1 */

#define GUC_UNIT_KB				0x0400	/* value is in kilobytes */
#define GUC_UNIT_BLOCKS			0x0800	/* value is in blocks */
#define GUC_UNIT_XBLOCKS		0x0C00	/* value is in xlog blocks */
#define GUC_UNIT_MEMORY			0x0C00	/* mask for KB, BLOCKS, XBLOCKS */

#define GUC_UNIT_MS				0x1000	/* value is in milliseconds */
#define GUC_UNIT_S				0x2000	/* value is in seconds */
#define GUC_UNIT_MIN			0x4000	/* value is in minutes */
#define GUC_UNIT_TIME			0x7000	/* mask for MS, S, MIN */

#define GUC_NOT_WHILE_SEC_REST	0x8000	/* can't set if security restricted */

/* GUC vars that are actually declared in guc.c, rather than elsewhere */
extern bool log_duration;
extern bool Debug_print_plan;
extern bool Debug_print_parse;
extern bool Debug_print_rewritten;
extern bool Debug_pretty_print;

extern bool log_parser_stats;
extern bool log_planner_stats;
extern bool log_executor_stats;
extern bool log_statement_stats;
extern bool log_btree_build_stats;

extern PGDLLIMPORT bool check_function_bodies;
extern bool default_with_oids;
extern bool SQL_inheritance;

extern int	log_min_error_statement;
extern int	log_min_messages;
extern int	client_min_messages;
extern int	log_min_duration_statement;
extern int	log_temp_files;

extern int	num_temp_buffers;

extern char *data_directory;
extern char *ConfigFileName;
extern char *HbaFileName;
extern char *IdentFileName;
extern char *external_pid_file;

extern char *application_name;

extern int	tcp_keepalives_idle;
extern int	tcp_keepalives_interval;
extern int	tcp_keepalives_count;

extern void SetConfigOption(const char *name, const char *value,
				GucContext context, GucSource source);

extern void DefineCustomBoolVariable(
						 const char *name,
						 const char *short_desc,
						 const char *long_desc,
						 bool *valueAddr,
						 bool bootValue,
						 GucContext context,
						 int flags,
						 GucBoolAssignHook assign_hook,
						 GucShowHook show_hook);

extern void DefineCustomIntVariable(
						const char *name,
						const char *short_desc,
						const char *long_desc,
						int *valueAddr,
						int bootValue,
						int minValue,
						int maxValue,
						GucContext context,
						int flags,
						GucIntAssignHook assign_hook,
						GucShowHook show_hook);

extern void DefineCustomRealVariable(
						 const char *name,
						 const char *short_desc,
						 const char *long_desc,
						 double *valueAddr,
						 double bootValue,
						 double minValue,
						 double maxValue,
						 GucContext context,
						 int flags,
						 GucRealAssignHook assign_hook,
						 GucShowHook show_hook);

extern void DefineCustomStringVariable(
						   const char *name,
						   const char *short_desc,
						   const char *long_desc,
						   char **valueAddr,
						   const char *bootValue,
						   GucContext context,
						   int flags,
						   GucStringAssignHook assign_hook,
						   GucShowHook show_hook);

extern void DefineCustomEnumVariable(
						 const char *name,
						 const char *short_desc,
						 const char *long_desc,
						 int *valueAddr,
						 int bootValue,
						 const struct config_enum_entry * options,
						 GucContext context,
						 int flags,
						 GucEnumAssignHook assign_hook,
						 GucShowHook show_hook);

extern void EmitWarningsOnPlaceholders(const char *className);

extern const char *GetConfigOption(const char *name, bool restrict_superuser);
extern const char *GetConfigOptionNoError(const char *name);
extern const char *GetConfigOptionResetString(const char *name);
extern void ProcessConfigFile(GucContext context);
extern void InitializeGUCOptions(void);
extern bool SelectConfigFiles(const char *userDoption, const char *progname);
extern void ResetAllOptions(void);
extern void AtStart_GUC(void);
extern int	NewGUCNestLevel(void);
extern void AtEOXact_GUC(bool isCommit, int nestLevel);
extern void BeginReportingGUCOptions(void);
extern void ParseLongOption(const char *string, char **name, char **value);
extern bool parse_int(const char *value, int *result, int flags,
		  const char **hintmsg);
extern bool parse_real(const char *value, double *result);
extern bool set_config_option(const char *name, const char *value,
				  GucContext context, GucSource source,
				  GucAction action, bool changeVal);
extern char *GetConfigOptionByName(const char *name, const char **varname);
extern void GetConfigOptionByNum(int varnum, const char **values, bool *noshow);
extern int	GetNumConfigOptions(void);

extern void SetPGVariable(const char *name, List *args, bool is_local);
extern void GetPGVariable(const char *name, DestReceiver *dest);
extern TupleDesc GetPGVariableResultDesc(const char *name);

extern void ExecSetVariableStmt(VariableSetStmt *stmt);
extern char *ExtractSetVariableArgs(VariableSetStmt *stmt);

extern void ProcessGUCArray(ArrayType *array,
				GucContext context, GucSource source, GucAction action);
extern ArrayType *GUCArrayAdd(ArrayType *array, const char *name, const char *value);
extern ArrayType *GUCArrayDelete(ArrayType *array, const char *name);
extern ArrayType *GUCArrayReset(ArrayType *array);

extern int	GUC_complaint_elevel(GucSource source);

extern void pg_timezone_abbrev_initialize(void);

#ifdef EXEC_BACKEND
extern void write_nondefault_variables(GucContext context);
extern void read_nondefault_variables(void);
#endif

/*
 * The following functions are not in guc.c, but are declared here to avoid
 * having to include guc.h in some widely used headers that it really doesn't
 * belong in.
 */

/* in commands/tablespace.c */
extern const char *assign_default_tablespace(const char *newval,
						  bool doit, GucSource source);
extern const char *assign_temp_tablespaces(const char *newval,
						bool doit, GucSource source);

/* in catalog/namespace.c */
extern const char *assign_search_path(const char *newval,
				   bool doit, GucSource source);

/* in access/transam/xlog.c */
extern bool assign_xlog_sync_method(int newval,
						bool doit, GucSource source);

#endif   /* GUC_H */
