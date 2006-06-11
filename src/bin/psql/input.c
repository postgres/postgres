/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2006, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/psql/input.c,v 1.54 2006/06/11 23:06:00 tgl Exp $
 */
#include "postgres_fe.h"

#include "input.h"
#include "pqexpbuffer.h"
#include "settings.h"
#include "tab-complete.h"
#include "common.h"

#ifndef WIN32
#define PSQLHISTORY ".psql_history"
#else
#define PSQLHISTORY "psql_history"
#endif

/* Runtime options for turning off readline and history */
/* (of course there is no runtime command for doing that :) */
#ifdef USE_READLINE
static bool useReadline;
static bool useHistory;
char	   *psql_history;

/*
 *	Preserve newlines in saved queries by mapping '\n' to NL_IN_HISTORY
 *
 *	It is assumed NL_IN_HISTORY will never be entered by the user
 *	nor appear inside a multi-byte string.  0x00 is not properly
 *	handled by the readline routines so it can not be used
 *	for this purpose.
 */
#define NL_IN_HISTORY	0x01

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


/*
 * gets_interactive()
 *
 * Gets a line of interactive input, using readline if desired.
 * The result is malloc'ed.
 */
char *
gets_interactive(const char *prompt)
{
#ifdef USE_READLINE
	if (useReadline)
	{
		/* On some platforms, readline is declared as readline(char *) */
		return readline((char *) prompt);
	}
#endif

	fputs(prompt, stdout);
	fflush(stdout);
	return gets_fromFile(stdin);
}


/*
 * Append the line to the history buffer, making sure there is a trailing '\n'
 */
void
pg_append_history(const char *s, PQExpBuffer history_buf)
{
#ifdef USE_READLINE
	if (useHistory && s && s[0])
	{
		appendPQExpBufferStr(history_buf, s);
		if (s[strlen(s) - 1] != '\n')
			appendPQExpBufferChar(history_buf, '\n');
	}	
#endif	
}


/*
 * Emit accumulated history entry to readline's history mechanism,
 * then reset the buffer to empty.
 *
 * Note: we write nothing if history_buf is empty, so extra calls to this
 * function don't hurt.  There must have been at least one line added by
 * pg_append_history before we'll do anything.
 */
void
pg_send_history(PQExpBuffer history_buf)
{
#ifdef USE_READLINE
	static char *prev_hist = NULL;

	char   *s = history_buf->data;

	if (useHistory && s[0])
	{
		enum histcontrol HC = GetHistControlConfig();

		if (((HC & hctl_ignorespace) && s[0] == ' ') ||
			((HC & hctl_ignoredups) && prev_hist && strcmp(s, prev_hist) == 0))
		{
			/* Ignore this line as far as history is concerned */
		}
		else
		{
			int		i;

			/* Trim any trailing \n's (OK to scribble on history_buf) */
			for (i = strlen(s)-1; i >= 0 && s[i] == '\n'; i--)
				;
			s[i + 1] = '\0';
			/* Save each previous line for ignoredups processing */
			if (prev_hist)
				free(prev_hist);
			prev_hist = pg_strdup(s);
			/* And send it to readline */
			add_history(s);
		}
	}

	resetPQExpBuffer(history_buf);
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


#ifdef USE_READLINE
/*
 * Convert newlines to NL_IN_HISTORY for safe saving in readline history file
 */
static void
encode_history(void)
{
	HIST_ENTRY *cur_hist;
	char *cur_ptr;

	for (history_set_pos(0), cur_hist = current_history();
		 cur_hist; cur_hist = next_history())
		for (cur_ptr = cur_hist->line; *cur_ptr; cur_ptr++)
			if (*cur_ptr == '\n')
				*cur_ptr = NL_IN_HISTORY;
}

/*
 * Reverse the above encoding
 */
static void
decode_history(void)
{
	HIST_ENTRY *cur_hist;
	char *cur_ptr;

	for (history_set_pos(0), cur_hist = current_history();
		 cur_hist; cur_hist = next_history())
		for (cur_ptr = cur_hist->line; *cur_ptr; cur_ptr++)
			if (*cur_ptr == NL_IN_HISTORY)
				*cur_ptr = '\n';
}
#endif /* USE_READLINE */


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
		char		home[MAXPGPATH];

		useReadline = true;
		initialize_readline();

		useHistory = true;
		if (GetVariable(pset.vars, "HISTSIZE") == NULL)
			SetVariable(pset.vars, "HISTSIZE", "500");
		using_history();

		if (GetVariable(pset.vars, "HISTFILE") == NULL)
		{
			if (get_home_path(home))
			{
				psql_history = pg_malloc(strlen(home) + 1 +
										 strlen(PSQLHISTORY) + 1);
				snprintf(psql_history, MAXPGPATH, "%s/%s", home, PSQLHISTORY);
			}
		}
		else
		{
			psql_history = pg_strdup(GetVariable(pset.vars, "HISTFILE"));
			expand_tilde(&psql_history);
		}

		if (psql_history)
		{
			read_history(psql_history);
			decode_history();
		}
	}
#endif

#ifdef HAVE_ATEXIT
	atexit(finishInput);
#else
	on_exit(finishInput, NULL);
#endif
}


/*
 * This function is for saving the readline history when user 
 * runs \s command or when psql finishes. 
 *
 * We have an argument named encodeFlag to handle the cases differently.
 * In case of call via \s we don't really need to encode \n as \x01,
 * but when we save history for Readline we must do that conversion.
 */
bool
saveHistory(char *fname, bool encodeFlag)
{
#ifdef USE_READLINE
	if (useHistory && fname)
	{
		if (encodeFlag)
			encode_history();
		if (write_history(fname) == 0)
			return true;

		psql_error("could not save history to file \"%s\": %s\n",
				   fname, strerror(errno));
	}
#else
	psql_error("history is not supported by this installation\n");
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
	if (useHistory && psql_history)
	{
		int			hist_size;

		hist_size = GetVariableNum(pset.vars, "HISTSIZE", -1, -1, true);
		if (hist_size >= 0)
			stifle_history(hist_size);

		saveHistory(psql_history, true);
		free(psql_history);
		psql_history = NULL;
	}
#endif
}
