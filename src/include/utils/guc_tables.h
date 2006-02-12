/*-------------------------------------------------------------------------
 *
 * guc_tables.h
 *		Declarations of tables used by GUC.
 *
 * See src/backend/utils/misc/README for design notes.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 *
 *	  $Id: guc_tables.h,v 1.6.4.1 2006/02/12 22:33:29 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef GUC_TABLES
#define GUC_TABLES 1

/*
 * Groupings to help organize all the run-time options for display
 */
enum config_group
{
	UNGROUPED,
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
	LOGGING_SYSLOG,
	LOGGING_WHEN,
	LOGGING_WHAT,
	STATS,
	STATS_MONITORING,
	STATS_COLLECTOR,
	CLIENT_CONN,
	CLIENT_CONN_STATEMENT,
	CLIENT_CONN_LOCALE,
	CLIENT_CONN_OTHER,
	LOCK_MANAGEMENT,
	COMPAT_OPTIONS,
	COMPAT_OPTIONS_PREVIOUS,
	COMPAT_OPTIONS_CLIENT,
	DEVELOPER_OPTIONS
};

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
	GucSource	session_source; /* source of the session_value */
	GucSource	tentative_source;		/* source of the tentative_value */
	GucSource	source;			/* source of the current actual value */
};

/* bit values in flags field */
#define GUC_LIST_INPUT			0x0001	/* input can be list format */
#define GUC_LIST_QUOTE			0x0002	/* double-quote list elements */
#define GUC_NO_SHOW_ALL			0x0004	/* exclude from SHOW ALL */
#define GUC_NO_RESET_ALL		0x0008	/* exclude from RESET ALL */
#define GUC_REPORT				0x0010	/* auto-report changes to client */
#define GUC_NOT_IN_SAMPLE		0x0020	/* not in postgresql.conf.sample */
#define GUC_DISALLOW_IN_FILE	0x0040	/* can't set in postgresql.conf */
#define GUC_IS_NAME				0x0080	/* limit string to NAMEDATALEN-1 */

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

/* constant tables corresponding to enums above and in guc.h */
extern const char *const config_group_names[];
extern const char *const config_type_names[];
extern const char *const GucContext_Names[];
extern const char *const GucSource_Names[];

/* the current set of variables */
extern struct config_generic **guc_variables;
extern int	num_guc_variables;

extern void build_guc_variables(void);

#endif
