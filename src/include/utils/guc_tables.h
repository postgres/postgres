/*-------------------------------------------------------------------------
 *
 * guc_tables.h
 *		Declarations of tables used by GUC.
 *
 * See src/backend/utils/misc/README for design notes.
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 *
 *	  $PostgreSQL: pgsql/src/include/utils/guc_tables.h,v 1.49 2010/06/15 07:52:11 itagaki Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef GUC_TABLES_H
#define GUC_TABLES_H 1

#include "utils/guc.h"

/*
 * GUC supports these types of variables:
 */
enum config_type
{
	PGC_BOOL,
	PGC_INT,
	PGC_REAL,
	PGC_STRING,
	PGC_ENUM
};

union config_var_value
{
	bool		boolval;
	int			intval;
	double		realval;
	char	   *stringval;
	int			enumval;
};

/*
 * Groupings to help organize all the run-time options for display
 */
enum config_group
{
	UNGROUPED,
	FILE_LOCATIONS,
	CONN_AUTH,
	CONN_AUTH_SETTINGS,
	CONN_AUTH_SECURITY,
	RESOURCES,
	RESOURCES_MEM,
	RESOURCES_KERNEL,
	RESOURCES_VACUUM_DELAY,
	RESOURCES_BGWRITER,
	RESOURCES_ASYNCHRONOUS,
	WAL,
	WAL_SETTINGS,
	WAL_CHECKPOINTS,
	WAL_ARCHIVING,
	WAL_REPLICATION,
	WAL_STANDBY_SERVERS,
	QUERY_TUNING,
	QUERY_TUNING_METHOD,
	QUERY_TUNING_COST,
	QUERY_TUNING_GEQO,
	QUERY_TUNING_OTHER,
	LOGGING,
	LOGGING_WHERE,
	LOGGING_WHEN,
	LOGGING_WHAT,
	STATS,
	STATS_MONITORING,
	STATS_COLLECTOR,
	AUTOVACUUM,
	CLIENT_CONN,
	CLIENT_CONN_STATEMENT,
	CLIENT_CONN_LOCALE,
	CLIENT_CONN_OTHER,
	LOCK_MANAGEMENT,
	COMPAT_OPTIONS,
	COMPAT_OPTIONS_PREVIOUS,
	COMPAT_OPTIONS_CLIENT,
	PRESET_OPTIONS,
	CUSTOM_OPTIONS,
	DEVELOPER_OPTIONS
};

/*
 * Stack entry for saving the state a variable had prior to an uncommitted
 * transactional change
 */
typedef enum
{
	/* This is almost GucAction, but we need a fourth state for SET+LOCAL */
	GUC_SAVE,					/* entry caused by function SET option */
	GUC_SET,					/* entry caused by plain SET command */
	GUC_LOCAL,					/* entry caused by SET LOCAL command */
	GUC_SET_LOCAL				/* entry caused by SET then SET LOCAL */
} GucStackState;

typedef struct guc_stack
{
	struct guc_stack *prev;		/* previous stack item, if any */
	int			nest_level;		/* nesting depth at which we made entry */
	GucStackState state;		/* see enum above */
	GucSource	source;			/* source of the prior value */
	union config_var_value prior;		/* previous value of variable */
	union config_var_value masked;		/* SET value in a GUC_SET_LOCAL entry */
	/* masked value's source must be PGC_S_SESSION, so no need to store it */
} GucStack;

/*
 * Generic fields applicable to all types of variables
 *
 * The short description should be less than 80 chars in length. Some
 * applications may use the long description as well, and will append
 * it to the short description. (separated by a newline or '. ')
 */
struct config_generic
{
	/* constant fields, must be set correctly in initial value: */
	const char *name;			/* name of variable - MUST BE FIRST */
	GucContext	context;		/* context required to set the variable */
	enum config_group group;	/* to help organize variables by function */
	const char *short_desc;		/* short desc. of this variable's purpose */
	const char *long_desc;		/* long desc. of this variable's purpose */
	int			flags;			/* flag bits, see below */
	/* variable fields, initialized at runtime: */
	enum config_type vartype;	/* type of variable (set only at startup) */
	int			status;			/* status bits, see below */
	GucSource	reset_source;	/* source of the reset_value */
	GucSource	source;			/* source of the current actual value */
	GucStack   *stack;			/* stacked prior values */
	char	   *sourcefile;		/* file this settings is from (NULL if not
								 * file) */
	int			sourceline;		/* line in source file */
};

/* bit values in flags field are defined in guc.h */

/* bit values in status field */
#define GUC_IS_IN_FILE		0x0001		/* found it in config file */
/*
 * Caution: the GUC_IS_IN_FILE bit is transient state for ProcessConfigFile.
 * Do not assume that its value represents useful information elsewhere.
 */


/* GUC records for specific variable types */

struct config_bool
{
	struct config_generic gen;
	/* constant fields, must be set correctly in initial value: */
	bool	   *variable;
	bool		boot_val;
	GucBoolAssignHook assign_hook;
	GucShowHook show_hook;
	/* variable fields, initialized at runtime: */
	bool		reset_val;
};

struct config_int
{
	struct config_generic gen;
	/* constant fields, must be set correctly in initial value: */
	int		   *variable;
	int			boot_val;
	int			min;
	int			max;
	GucIntAssignHook assign_hook;
	GucShowHook show_hook;
	/* variable fields, initialized at runtime: */
	int			reset_val;
};

struct config_real
{
	struct config_generic gen;
	/* constant fields, must be set correctly in initial value: */
	double	   *variable;
	double		boot_val;
	double		min;
	double		max;
	GucRealAssignHook assign_hook;
	GucShowHook show_hook;
	/* variable fields, initialized at runtime: */
	double		reset_val;
};

struct config_string
{
	struct config_generic gen;
	/* constant fields, must be set correctly in initial value: */
	char	  **variable;
	const char *boot_val;
	GucStringAssignHook assign_hook;
	GucShowHook show_hook;
	/* variable fields, initialized at runtime: */
	char	   *reset_val;
};

struct config_enum
{
	struct config_generic gen;
	/* constant fields, must be set correctly in initial value: */
	int		   *variable;
	int			boot_val;
	const struct config_enum_entry *options;
	GucEnumAssignHook assign_hook;
	GucShowHook show_hook;
	/* variable fields, initialized at runtime: */
	int			reset_val;
};

/* constant tables corresponding to enums above and in guc.h */
extern const char *const config_group_names[];
extern const char *const config_type_names[];
extern const char *const GucContext_Names[];
extern const char *const GucSource_Names[];

/* get the current set of variables */
extern struct config_generic **get_guc_variables(void);

extern void build_guc_variables(void);

/* search in enum options */
extern const char *config_enum_lookup_by_value(struct config_enum * record, int val);
extern bool config_enum_lookup_by_name(struct config_enum * record,
						   const char *value, int *retval);


#endif   /* GUC_TABLES_H */
