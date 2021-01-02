/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2021, PostgreSQL Global Development Group
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

/*
 * Note: these enums should generally be chosen so that zero corresponds
 * to the default behavior.
 */

typedef enum
{
	PSQL_ECHO_NONE,
	PSQL_ECHO_QUERIES,
	PSQL_ECHO_ERRORS,
	PSQL_ECHO_ALL
} PSQL_ECHO;

typedef enum
{
	PSQL_ECHO_HIDDEN_OFF,
	PSQL_ECHO_HIDDEN_ON,
	PSQL_ECHO_HIDDEN_NOEXEC
} PSQL_ECHO_HIDDEN;

typedef enum
{
	PSQL_ERROR_ROLLBACK_OFF,
	PSQL_ERROR_ROLLBACK_INTERACTIVE,
	PSQL_ERROR_ROLLBACK_ON
} PSQL_ERROR_ROLLBACK;

typedef enum
{
	PSQL_COMP_CASE_PRESERVE_UPPER,
	PSQL_COMP_CASE_PRESERVE_LOWER,
	PSQL_COMP_CASE_UPPER,
	PSQL_COMP_CASE_LOWER
} PSQL_COMP_CASE;

typedef enum
{
	hctl_none = 0,
	hctl_ignorespace = 1,
	hctl_ignoredups = 2,
	hctl_ignoreboth = hctl_ignorespace | hctl_ignoredups
} HistControl;

enum trivalue
{
	TRI_DEFAULT,
	TRI_NO,
	TRI_YES
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
	bool		gdesc_flag;		/* one-shot request to describe query results */
	bool		gexec_flag;		/* one-shot request to execute query results */
	bool		crosstab_flag;	/* one-shot request to crosstab results */
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
	bool		hide_tableam;
	int			fetch_count;
	int			histsize;
	int			ignoreeof;
	PSQL_ECHO	echo;
	PSQL_ECHO_HIDDEN echo_hidden;
	PSQL_ERROR_ROLLBACK on_error_rollback;
	PSQL_COMP_CASE comp_case;
	HistControl histcontrol;
	const char *prompt1;
	const char *prompt2;
	const char *prompt3;
	PGVerbosity verbosity;		/* current error verbosity level */
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
