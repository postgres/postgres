/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2003, PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/settings.h,v 1.15 2003/08/04 23:59:40 tgl Exp $
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include "libpq-fe.h"

#include "variables.h"
#include "print.h"

#define DEFAULT_FIELD_SEP "|"
#define DEFAULT_RECORD_SEP "\n"
#define DEFAULT_EDITOR	"vi"

#define DEFAULT_PROMPT1 "%/%R%# "
#define DEFAULT_PROMPT2 "%/%R%# "
#define DEFAULT_PROMPT3 ">> "


typedef struct _psqlSettings
{
	PGconn	   *db;				/* connection to backend */
	int			encoding;
	FILE	   *queryFout;		/* where to send the query results */
	bool		queryFoutPipe;	/* queryFout is from a popen() */

	printQueryOpt popt;
	VariableSpace vars;			/* "shell variable" repository */

	char	   *gfname;			/* one-shot file output argument for \g */

	bool		notty;			/* stdin or stdout is not a tty (as
								 * determined on startup) */
	bool		useReadline;	/* use libreadline routines */
	bool		useHistory;
	bool		getPassword;	/* prompt the user for a username and
								 * password */
	FILE	   *cur_cmd_source; /* describe the status of the current main
								 * loop */
	bool		cur_cmd_interactive;

	char	   *progname;		/* in case you renamed psql */
	char	   *inputfile;		/* for error reporting */
	unsigned	lineno;			/* also for error reporting */

	bool		timing;			/* enable timing of all queries */
} PsqlSettings;

extern PsqlSettings pset;


#define QUIET() (GetVariableBool(pset.vars, "QUIET"))


#ifndef EXIT_SUCCESS
#define EXIT_SUCCESS 0
#endif

#ifndef EXIT_FAILURE
#define EXIT_FAILURE 1
#endif

#define EXIT_BADCONN 2

#define EXIT_USER 3

#endif
