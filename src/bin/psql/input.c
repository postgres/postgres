/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright 2000 by PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/input.c,v 1.16 2001/02/27 08:13:27 ishii Exp $
 */
#include "postgres_fe.h"
#include "input.h"

#include <errno.h>

#include "pqexpbuffer.h"
#include "settings.h"
#include "tab-complete.h"
#include "common.h"

/* Runtime options for turning off readline and history */
/* (of course there is no runtime command for doing that :) */
#ifdef USE_READLINE
static bool useReadline;

#endif
#ifdef USE_HISTORY
static bool useHistory;

#endif


/*
 * gets_interactive()
 *
 * Gets a line of interactive input, using readline of desired.
 * The result is malloced.
 */
char *
gets_interactive(char *prompt)
{
	char	   *s;

#ifdef USE_HISTORY
	const char *var;
	static char *prev_hist = NULL;

#endif

#ifdef USE_READLINE
	if (useReadline)
		s = readline(prompt);
	else
	{
#endif
		fputs(prompt, stdout);
		fflush(stdout);
		s = gets_fromFile(stdin);
#ifdef USE_READLINE
	}
#endif

#ifdef USE_HISTORY
	if (useHistory && s && s[0] != '\0')
	{
		var = GetVariable(pset.vars, "HISTCONTROL");
		if (!var || (var
					 && !((strcmp(var, "ignorespace") == 0 || strcmp(var, "ignoreboth") == 0) && s[0] == ' ')
					 && !((strcmp(var, "ignoredups") == 0 || strcmp(var, "ignoreboth") == 0) && prev_hist && strcmp(s, prev_hist) == 0)
					 ))
		{
			free(prev_hist);
			prev_hist = strdup(s);
			add_history(s);
		}
	}
#endif

	return s;
}



/*
 * gets_fromFile
 *
 * Gets a line of noninteractive input from a file (which could be stdin).
 */
char *
gets_fromFile(FILE *source)
{
	PQExpBufferData buffer;
	char		line[1024];

	initPQExpBuffer(&buffer);

	while (fgets(line, sizeof(line), source) != NULL)
	{
		appendPQExpBufferStr(&buffer, line);
		if (buffer.data[buffer.len - 1] == '\n')
		{
			buffer.data[buffer.len - 1] = '\0';
			return buffer.data;
		}
	}

	if (buffer.len > 0)
		return buffer.data;		/* EOF after reading some bufferload(s) */

	/* EOF, so return null */
	termPQExpBuffer(&buffer);
	return NULL;
}



/*
 * Put any startup stuff related to input in here. It's good to maintain
 * abstraction this way.
 *
 * The only "flag" right now is 1 for use readline & history.
 */
void
initializeInput(int flags)
{
#ifdef USE_READLINE
	if (flags == 1)
	{
		useReadline = true;
		initialize_readline();
	}
#endif

#ifdef USE_HISTORY
	if (flags == 1)
	{
		const char *home;

		useHistory = true;
		SetVariable(pset.vars, "HISTSIZE", "500");
		using_history();
		home = getenv("HOME");
		if (home)
		{
			char	   *psql_history = (char *) malloc(strlen(home) + 20);

			if (psql_history)
			{
				sprintf(psql_history, "%s/.psql_history", home);
				read_history(psql_history);
				free(psql_history);
			}
		}
	}
#endif

#ifdef HAVE_ATEXIT
	atexit(finishInput);
#else
	on_exit(finishInput);
#endif
}



bool
saveHistory(char *fname)
{
#ifdef USE_HISTORY
	if (useHistory && fname)
	{
		if (write_history(fname) != 0)
		{
			psql_error("could not save history to %s: %s\n", fname, strerror(errno));
			return false;
		}
		return true;
	}
	else
		return false;
#else
	return false;
#endif
}



void
finishInput(void)
{
#ifdef USE_HISTORY
	if (useHistory)
	{
		char	   *home;
		char	   *psql_history;

		home = getenv("HOME");
		if (home)
		{
			psql_history = (char *) malloc(strlen(home) + 20);
			if (psql_history)
			{
				const char *var = GetVariable(pset.vars, "HISTSIZE");

				if (var)
					stifle_history(atoi(var));
				sprintf(psql_history, "%s/.psql_history", home);
				write_history(psql_history);
				free(psql_history);
			}
		}
	}
#endif
}
