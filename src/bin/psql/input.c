/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2003, PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/input.c,v 1.31 2003/09/12 02:40:09 momjian Exp $
 */
#include "postgres_fe.h"
#include "input.h"

#include <errno.h>

#ifdef WIN32
#include <windows.h>
#endif

#include "pqexpbuffer.h"
#include "settings.h"
#include "tab-complete.h"
#include "common.h"

/* Runtime options for turning off readline and history */
/* (of course there is no runtime command for doing that :) */
#ifdef USE_READLINE
static bool useReadline;
static bool useHistory;

enum histcontrol
{
	hctl_none = 0,
	hctl_ignorespace = 1,
	hctl_ignoredups = 2,
	hctl_ignoreboth = hctl_ignorespace | hctl_ignoredups
};
#endif

#ifdef HAVE_ATEXIT
static void finishInput(void);

#else
/* designed for use with on_exit() */
static void finishInput(int, void *);
#endif

#define PSQLHISTORY ".psql_history"


#ifdef USE_READLINE
static enum histcontrol
GetHistControlConfig(void)
{
	enum histcontrol HC;
	const char *var;

	var = GetVariable(pset.vars, "HISTCONTROL");

	if (!var)
		HC = hctl_none;
	else if (strcmp(var, "ignorespace") == 0)
		HC = hctl_ignorespace;
	else if (strcmp(var, "ignoredups") == 0)
		HC = hctl_ignoredups;
	else if (strcmp(var, "ignoreboth") == 0)
		HC = hctl_ignoreboth;
	else
		HC = hctl_none;

	return HC;
}
#endif


static char *
gets_basic(const char prompt[])
{
	fputs(prompt, stdout);
	fflush(stdout);
	return gets_fromFile(stdin);
}


/*
 * gets_interactive()
 *
 * Gets a line of interactive input, using readline of desired.
 * The result is malloced.
 */
char *
gets_interactive(const char *prompt)
{
#ifdef USE_READLINE
	char	   *s;

	static char *prev_hist = NULL;

	if (useReadline)
		/* On some platforms, readline is declared as readline(char *) */
		s = readline((char *) prompt);
	else
		s = gets_basic(prompt);

	if (useHistory && s && s[0])
	{
		enum histcontrol HC;

		HC = GetHistControlConfig();

		if (((HC & hctl_ignorespace) && s[0] == ' ') ||
			((HC & hctl_ignoredups) && prev_hist && strcmp(s, prev_hist) == 0))
		{
			/* Ignore this line as far as history is concerned */
		}
		else
		{
			free(prev_hist);
			prev_hist = strdup(s);
			add_history(s);
		}
	}

	return s;
#else
	return gets_basic(prompt);
#endif
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
	if (flags & 1)
	{
		const char *home;

		useReadline = true;
		initialize_readline();

		useHistory = true;
		if (GetVariable(pset.vars, "HISTSIZE") == NULL)
			SetVariable(pset.vars, "HISTSIZE", "500");
		using_history();
		home = getenv("HOME");
		if (home)
		{
			char	   *psql_history = (char *) malloc(strlen(home) + 1 +
												strlen(PSQLHISTORY) + 1);

			if (psql_history)
			{
				sprintf(psql_history, "%s/%s", home, PSQLHISTORY);
				read_history(psql_history);
				free(psql_history);
			}
		}
	}
#endif

#ifdef HAVE_ATEXIT
	atexit(finishInput);
#else
	on_exit(finishInput, NULL);
#endif
}



bool
saveHistory(char *fname)
{
#ifdef USE_READLINE
	if (useHistory && fname)
	{
		if (write_history(fname) == 0)
			return true;

		psql_error("could not save history to file \"%s\": %s\n", fname, strerror(errno));
	}
#endif

	return false;
}



static void
#ifdef HAVE_ATEXIT
finishInput(void)
#else
finishInput(int exitstatus, void *arg)
#endif
{
#ifdef USE_READLINE
	if (useHistory)
	{
		char	   *home;
		char	   *psql_history;

		home = getenv("HOME");
		if (home)
		{
			psql_history = (char *) malloc(strlen(home) + 1 +
										   strlen(PSQLHISTORY) + 1);
			if (psql_history)
			{
				int			hist_size;

				hist_size = GetVariableNum(pset.vars, "HISTSIZE", -1, -1, true);

				if (hist_size >= 0)
					stifle_history(hist_size);

				sprintf(psql_history, "%s/%s", home, PSQLHISTORY);
				write_history(psql_history);
				free(psql_history);
			}
		}
	}
#endif
}
