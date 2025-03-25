/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2025, PostgreSQL Global Development Group
 *
 * src/bin/psql/settings.h
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include "fe_utils/print.h"
#include "variables.h"

#define DEFAULT_CSV_FIELD_SEP ','
#define DEFAULT_FIELD_SEP "|"
#define DEFAULT_RECORD_SEP "\n"

#if defined(WIN32) || defined(__CYGWIN__)
#define DEFAULT_EDITOR	"notepad.exe"
/* no DEFAULT_EDITOR_LINENUMBER_ARG for Notepad */
#else
#define DEFAULT_EDITOR	"vi"
#define DEFAULT_EDITOR_LINENUMBER_ARG "+"
#endif

#define DEFAULT_PROMPT1 "%/%R%x%# "
#define DEFAULT_PROMPT2 "%/%R%x%# "
#define DEFAULT_PROMPT3 ">> "

#define DEFAULT_WATCH_INTERVAL "2"
/*
 * Limit the max default setting to a value which should be safe for the
 * itimer call, yet large enough to cover all realistic usecases.
 */
#define DEFAULT_WATCH_INTERVAL_MAX (1000*1000)
/*
 * Note: these enums should generally be chosen so that zero corresponds
 * to the default behavior.
 */

typedef enum
{
	PSQL_ECHO_NONE,
	PSQL_ECHO_QUERIES,
	PSQL_ECHO_ERRORS,
	PSQL_ECHO_ALL,
} PSQL_ECHO;

typedef enum
{
	PSQL_ECHO_HIDDEN_OFF,
	PSQL_ECHO_HIDDEN_ON,
	PSQL_ECHO_HIDDEN_NOEXEC,
} PSQL_ECHO_HIDDEN;

typedef enum
{
	PSQL_ERROR_ROLLBACK_OFF,
	PSQL_ERROR_ROLLBACK_INTERACTIVE,
	PSQL_ERROR_ROLLBACK_ON,
} PSQL_ERROR_ROLLBACK;

typedef enum
{
	PSQL_COMP_CASE_PRESERVE_UPPER,
	PSQL_COMP_CASE_PRESERVE_LOWER,
	PSQL_COMP_CASE_UPPER,
	PSQL_COMP_CASE_LOWER,
} PSQL_COMP_CASE;

typedef enum
{
	PSQL_SEND_QUERY,
	PSQL_SEND_EXTENDED_CLOSE,
	PSQL_SEND_EXTENDED_PARSE,
	PSQL_SEND_EXTENDED_QUERY_PARAMS,
	PSQL_SEND_EXTENDED_QUERY_PREPARED,
	PSQL_SEND_PIPELINE_SYNC,
	PSQL_SEND_START_PIPELINE_MODE,
	PSQL_SEND_END_PIPELINE_MODE,
	PSQL_SEND_FLUSH,
	PSQL_SEND_FLUSH_REQUEST,
	PSQL_SEND_GET_RESULTS,
} PSQL_SEND_MODE;

typedef enum
{
	hctl_none = 0,
	hctl_ignorespace = 1,
	hctl_ignoredups = 2,
	hctl_ignoreboth = hctl_ignorespace | hctl_ignoredups,
} HistControl;

enum trivalue
{
	TRI_DEFAULT,
	TRI_NO,
	TRI_YES,
};

typedef struct _psqlSettings
{
	PGconn	   *db;				/* connection to backend */
	int			encoding;		/* client_encoding */
	FILE	   *queryFout;		/* where to send the query results */
	bool		queryFoutPipe;	/* queryFout is from a popen() */

	FILE	   *copyStream;		/* Stream to read/write for \copy command */

	PGresult   *last_error_result;	/* most recent error result, if any */

	printQueryOpt popt;			/* The active print format settings */

	char	   *gfname;			/* one-shot file output argument for \g */
	printQueryOpt *gsavepopt;	/* if not null, saved print format settings */

	char	   *gset_prefix;	/* one-shot prefix argument for \gset */
	bool		gdesc_flag;		/* one-shot request to describe query result */
	bool		gexec_flag;		/* one-shot request to execute query result */
	PSQL_SEND_MODE send_mode;	/* one-shot request to send query with normal
								 * or extended query protocol */
	int			bind_nparams;	/* number of parameters */
	char	  **bind_params;	/* parameters for extended query protocol call */
	char	   *stmtName;		/* prepared statement name used for extended
								 * query protocol commands */
	int			piped_commands; /* number of piped commands */
	int			piped_syncs;	/* number of piped syncs */
	int			available_results;	/* number of results available to get */
	int			requested_results;	/* number of requested results, including
									 * sync messages.  Used to read a limited
									 * subset of the available_results. */
	bool		crosstab_flag;	/* one-shot request to crosstab result */
	char	   *ctv_args[4];	/* \crosstabview arguments */

	bool		notty;			/* stdin or stdout is not a tty (as determined
								 * on startup) */
	enum trivalue getPassword;	/* prompt the user for a username and password */
	FILE	   *cur_cmd_source; /* describe the status of the current main
								 * loop */
	bool		cur_cmd_interactive;
	int			sversion;		/* backend server version */
	const char *progname;		/* in case you renamed psql */
	char	   *inputfile;		/* file being currently processed, if any */
	uint64		lineno;			/* also for error reporting */
	uint64		stmt_lineno;	/* line number inside the current statement */

	bool		timing;			/* enable timing of all queries */

	FILE	   *logfile;		/* session log file handle */

	VariableSpace vars;			/* "shell variable" repository */

	/*
	 * If we get a connection failure, the now-unusable PGconn is stashed here
	 * until we can successfully reconnect.  Never attempt to do anything with
	 * this PGconn except extract parameters for a \connect attempt.
	 */
	PGconn	   *dead_conn;		/* previous connection to backend */

	/*
	 * The remaining fields are set by assign hooks associated with entries in
	 * "vars".  They should not be set directly except by those hook
	 * functions.
	 */
	bool		autocommit;
	bool		on_error_stop;
	bool		quiet;
	bool		singleline;
	bool		singlestep;
	bool		hide_compression;
	bool		hide_tableam;
	int			fetch_count;
	int			histsize;
	int			ignoreeof;
	double		watch_interval;
	PSQL_ECHO	echo;
	PSQL_ECHO_HIDDEN echo_hidden;
	PSQL_ERROR_ROLLBACK on_error_rollback;
	PSQL_COMP_CASE comp_case;
	HistControl histcontrol;
	const char *prompt1;
	const char *prompt2;
	const char *prompt3;
	PGVerbosity verbosity;		/* current error verbosity level */
	bool		show_all_results;
	PGContextVisibility show_context;	/* current context display level */
} PsqlSettings;

extern PsqlSettings pset;


#ifndef EXIT_SUCCESS
#define EXIT_SUCCESS 0
#endif

#ifndef EXIT_FAILURE
#define EXIT_FAILURE 1
#endif

#define EXIT_BADCONN 2

#define EXIT_USER 3

#endif
