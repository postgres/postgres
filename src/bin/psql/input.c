#include <config.h>
#include <c.h>
#include "input.h"

#include <pqexpbuffer.h>

#include "settings.h"
#include "tab-complete.h"

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
gets_interactive(const char *prompt)
{
	char	   *s;

#ifdef USE_READLINE
	if (useReadline)
	{
		s = readline(prompt);
		fputc('\r', stdout);
		fflush(stdout);
	}
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
		add_history(s);
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

	while (fgets(line, 1024, source) != NULL)
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
initializeInput(int flags, PsqlSettings *pset)
{
#ifdef USE_READLINE
	if (flags == 1)
	{
		useReadline = true;
		rl_readline_name = "psql";
        initialize_readline(&(pset->db));
	}
#endif

#ifdef USE_HISTORY
	if (flags == 1)
	{
		const char *home;

		useHistory = true;
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
}



bool
saveHistory(const char *fname)
{
#ifdef USE_HISTORY
	if (useHistory)
	{
		if (write_history(fname) != 0)
		{
			perror(fname);
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
				sprintf(psql_history, "%s/.psql_history", home);
				write_history(psql_history);
				free(psql_history);
			}
		}
	}
#endif
}
