/*--------------------------------------------------------------------
 * guc.h
 *
 * External declarations pertaining to backend/utils/misc/guc.c and
 * backend/utils/misc/guc-file.l
 *
 * Copyright 2000-2003 by PostgreSQL Global Development Group
 * Written by Peter Eisentraut <peter_e@gmx.net>.
 *
 * $Id: guc.h,v 1.35 2003/07/21 21:02:12 momjian Exp $
 *--------------------------------------------------------------------
 */
#ifndef GUC_H
#define GUC_H

#include "nodes/pg_list.h"
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
 * mechanism, or from SQL if you're a superuser. These options cannot
 * be set in the connection startup packet, because when it is processed
 * we don't yet know if the user is a superuser.
 *
 * USERLIMIT options can only be manipulated in certain ways by
 * non-super users.
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
	PGC_USERLIMIT,
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
 */
typedef enum
{
	PGC_S_DEFAULT = 0,			/* wired-in default */
	PGC_S_ENV_VAR = 1,			/* postmaster environment variable */
	PGC_S_FILE = 2,				/* postgresql.conf */
	PGC_S_ARGV = 3,				/* postmaster command line */
	PGC_S_USERSTART=4,			/*
								 *	Settings below are controlled by users.
								 *	This is used by PGC_USERLIMT to prevent
								 *	non-super users from changing certain settings.
								 */
	PGC_S_DATABASE = 5,			/* per-database setting */
	PGC_S_USER = 6,				/* per-user setting */
	PGC_S_CLIENT = 7,			/* from client connection request */
	PGC_S_OVERRIDE = 8,			/* special case to forcibly set default */
	PGC_S_SESSION = 9			/* SET command */
} GucSource;


/* GUC vars that are actually declared in guc.c, rather than elsewhere */
extern bool log_statement;
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

extern bool SQL_inheritance;
extern bool Australian_timezones;

extern int	log_min_error_statement;
extern int	log_min_messages;
extern int	client_min_messages;


extern void SetConfigOption(const char *name, const char *value,
				GucContext context, GucSource source);
extern const char *GetConfigOption(const char *name);
extern const char *GetConfigOptionResetString(const char *name);
extern void ProcessConfigFile(GucContext context);
extern void InitializeGUCOptions(void);
extern void ResetAllOptions(void);
extern void AtEOXact_GUC(bool isCommit);
extern void BeginReportingGUCOptions(void);
extern void ParseLongOption(const char *string, char **name, char **value);
extern bool set_config_option(const char *name, const char *value,
				  GucContext context, GucSource source,
				  bool isLocal, bool DoIt);
extern void ShowGUCConfigOption(const char *name, DestReceiver *dest);
extern void ShowAllGUCConfig(DestReceiver *dest);
extern char *GetConfigOptionByName(const char *name, const char **varname);
extern char *GetConfigOptionByNum(int varnum, const char **varname, bool *noshow);
extern int	GetNumConfigOptions(void);

extern void SetPGVariable(const char *name, List *args, bool is_local);
extern void GetPGVariable(const char *name, DestReceiver *dest);
extern TupleDesc GetPGVariableResultDesc(const char *name);
extern void ResetPGVariable(const char *name);

extern char *flatten_set_variable_args(const char *name, List *args);

extern void ProcessGUCArray(ArrayType *array, GucSource source);
extern ArrayType *GUCArrayAdd(ArrayType *array, const char *name, const char *value);
extern ArrayType *GUCArrayDelete(ArrayType *array, const char *name);

#ifdef EXEC_BACKEND
void write_nondefault_variables(GucContext context);
void read_nondefault_variables(void);
#endif

extern int	log_min_duration_statement;

#endif   /* GUC_H */
