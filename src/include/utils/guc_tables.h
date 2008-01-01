/*-------------------------------------------------------------------------
 *
 * guc_tables.h
 *		Declarations of tables used by GUC.
 *
 * See src/backend/utils/misc/README for design notes.
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 *
 *	  $PostgreSQL: pgsql/src/include/utils/guc_tables.h,v 1.38 2008/01/01 19:45:59 momjian Exp $
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
	PGC_STRING
};

union config_var_value
{
	bool		boolval;
	int			intval;
	double		realval;
	char	   *stringval;
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
	RESOURCES_FSM,
	RESOURCES_KERNEL,
	WAL,
	WAL_SETTINGS,
	WAL_CHECKPOINTS,
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
};

/* bit values in flags field */
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

/* constant tables corresponding to enums above and in guc.h */
extern const char *const config_group_names[];
extern const char *const config_type_names[];
extern const char *const GucContext_Names[];
extern const char *const GucSource_Names[];

/* get the current set of variables */
extern struct config_generic **get_guc_variables(void);

extern void build_guc_variables(void);

#endif   /* GUC_TABLES_H */
