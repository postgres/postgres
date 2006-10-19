/*--------------------------------------------------------------------
 * guc.h
 *
 * External declarations pertaining to backend/utils/misc/guc.c and
 * backend/utils/misc/guc-file.l
 *
 * Copyright (c) 2000-2006, PostgreSQL Global Development Group
 * Written by Peter Eisentraut <peter_e@gmx.net>.
 *
 * $PostgreSQL: pgsql/src/include/utils/guc.h,v 1.76 2006/10/19 18:32:47 tgl Exp $
 *--------------------------------------------------------------------
 */
#ifndef GUC_H
#define GUC_H

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
 * configuration file.	The idea is that these options are fixed for a
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
 * as the actual source of any value).	This is an interactive case, but
 * it needs its own source value because some assign hooks need to make
 * different validity checks in this case.
 */
typedef enum
{
	PGC_S_DEFAULT,				/* wired-in default */
	PGC_S_ENV_VAR,				/* postmaster environment variable */
	PGC_S_FILE,					/* postgresql.conf */
	PGC_S_ARGV,					/* postmaster command line */
	PGC_S_DATABASE,				/* per-database setting */
	PGC_S_USER,					/* per-user setting */
	PGC_S_CLIENT,				/* from client connection request */
	PGC_S_OVERRIDE,				/* special case to forcibly set default */
	PGC_S_INTERACTIVE,			/* dividing line for error reporting */
	PGC_S_TEST,					/* test per-database or per-user setting */
	PGC_S_SESSION				/* SET command */
} GucSource;

typedef const char *(*GucStringAssignHook) (const char *newval, bool doit, GucSource source);
typedef bool (*GucBoolAssignHook) (bool newval, bool doit, GucSource source);
typedef bool (*GucIntAssignHook) (int newval, bool doit, GucSource source);
typedef bool (*GucRealAssignHook) (double newval, bool doit, GucSource source);

typedef const char *(*GucShowHook) (void);

#define GUC_QUALIFIER_SEPARATOR '.'

/* GUC vars that are actually declared in guc.c, rather than elsewhere */
extern bool log_duration;
extern bool Debug_print_plan;
extern bool Debug_print_parse;
extern bool Debug_print_rewritten;
extern bool Debug_pretty_print;
extern bool Explain_pretty_print;

extern bool log_parser_stats;
extern bool log_planner_stats;
extern bool log_executor_stats;
extern bool log_statement_stats;
extern bool log_btree_build_stats;

extern DLLIMPORT bool check_function_bodies;
extern bool default_with_oids;
extern bool SQL_inheritance;

extern int	log_min_error_statement;
extern int	log_min_messages;
extern int	client_min_messages;
extern int	log_min_duration_statement;

extern int	num_temp_buffers;

extern char *ConfigFileName;
extern char *HbaFileName;
extern char *IdentFileName;
extern char *external_pid_file;

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
						 GucContext context,
						 GucBoolAssignHook assign_hook,
						 GucShowHook show_hook);

extern void DefineCustomIntVariable(
						const char *name,
						const char *short_desc,
						const char *long_desc,
						int *valueAddr,
						int minValue,
						int maxValue,
						GucContext context,
						GucIntAssignHook assign_hook,
						GucShowHook show_hook);

extern void DefineCustomRealVariable(
						 const char *name,
						 const char *short_desc,
						 const char *long_desc,
						 double *valueAddr,
						 double minValue,
						 double maxValue,
						 GucContext context,
						 GucRealAssignHook assign_hook,
						 GucShowHook show_hook);

extern void DefineCustomStringVariable(
						   const char *name,
						   const char *short_desc,
						   const char *long_desc,
						   char **valueAddr,
						   GucContext context,
						   GucStringAssignHook assign_hook,
						   GucShowHook show_hook);

extern void EmitWarningsOnPlaceholders(const char *className);

extern const char *GetConfigOption(const char *name);
extern const char *GetConfigOptionResetString(const char *name);
extern bool IsSuperuserConfigOption(const char *name);
extern void ProcessConfigFile(GucContext context);
extern void InitializeGUCOptions(void);
extern bool SelectConfigFiles(const char *userDoption, const char *progname);
extern void ResetAllOptions(void);
extern void AtEOXact_GUC(bool isCommit, bool isSubXact);
extern void BeginReportingGUCOptions(void);
extern void ParseLongOption(const char *string, char **name, char **value);
extern bool set_config_option(const char *name, const char *value,
				  GucContext context, GucSource source,
				  bool isLocal, bool changeVal);
extern char *GetConfigOptionByName(const char *name, const char **varname);
extern void GetConfigOptionByNum(int varnum, const char **values, bool *noshow);
extern int	GetNumConfigOptions(void);

extern void SetPGVariable(const char *name, List *args, bool is_local);
extern void GetPGVariable(const char *name, DestReceiver *dest);
extern TupleDesc GetPGVariableResultDesc(const char *name);
extern void ResetPGVariable(const char *name);

extern char *flatten_set_variable_args(const char *name, List *args);

extern void ProcessGUCArray(ArrayType *array, GucSource source);
extern ArrayType *GUCArrayAdd(ArrayType *array, const char *name, const char *value);
extern ArrayType *GUCArrayDelete(ArrayType *array, const char *name);

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

/* in utils/adt/regexp.c */
extern const char *assign_regex_flavor(const char *value,
					bool doit, GucSource source);

/* in catalog/namespace.c */
extern const char *assign_search_path(const char *newval,
				   bool doit, GucSource source);

/* in access/transam/xlog.c */
extern const char *assign_xlog_sync_method(const char *method,
						bool doit, GucSource source);

#endif   /* GUC_H */
