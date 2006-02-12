/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2005, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/psql/input.c,v 1.48 2006/02/12 05:24:38 momjian Exp $
 */
#include "postgres_fe.h"

#include "pqexpbuffer.h"
#include "input.h"
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
 * The result is malloc'ed.
 */
char *
gets_interactive(const char *prompt)
{
#ifdef USE_READLINE
	char	   *s;

	if (useReadline)
		/* On some platforms, readline is declared as readline(char *) */
		s = readline((char *) prompt);
	else
		s = gets_basic(prompt);

	return s;
#else
	return gets_basic(prompt);
#endif
}


/* Put the line in the history buffer and also add the trailing \n */
void pgadd_history(char *s, PQExpBuffer history_buf)
{
#ifdef USE_READLINE

	int slen;
	if (useReadline && useHistory && s && s[0])
	{
		slen = strlen(s);
		if (s[slen-1] == '\n')
			appendPQExpBufferStr(history_buf, s);
		else
		{
			appendPQExpBufferStr(history_buf, s);
			appendPQExpBufferChar(history_buf, '\n');
		}
	}	
#endif	
}


/* Feed the contents of the history buffer to readline */
void pgflush_history(PQExpBuffer history_buf)
{
#ifdef USE_READLINE	
	char *s;
	static char *prev_hist;
	int slen, i;
	
	if (useReadline && useHistory )
	{
		enum histcontrol HC;
		
		s = history_buf->data;
		prev_hist = NULL;
			
		HC = GetHistControlConfig();

		if (((HC & hctl_ignorespace) && s[0] == ' ') ||
		  ((HC & hctl_ignoredups) && prev_hist && strcmp(s, prev_hist) == 0))
		{
			/* Ignore this line as far as history is concerned */
		}
		else
		{
			free(prev_hist);
			slen = strlen(s);
			/* Trim the trailing \n's */
			for (i = slen-1; i >= 0 && s[i] == '\n'; i--)
				;
			s[i + 1] = '\0';
			prev_hist = pg_strdup(s);
			add_history(s);
		}
		
		resetPQExpBuffer(history_buf);
	}
#endif
}

void pgclear_history(PQExpBuffer history_buf)
{
#ifdef USE_READLINE	
	if (useReadline && useHistory)
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


static void encode_history()
{
	HIST_ENTRY *cur_hist;
	char *cur_ptr;

	for (history_set_pos(0), cur_hist = current_history();
		 cur_hist; cur_hist = next_history())
		for (cur_ptr = cur_hist->line; *cur_ptr; cur_ptr++)
			if (*cur_ptr == '\n')
				*cur_ptr = NL_IN_HISTORY;
}

static void decode_history()
{
	HIST_ENTRY *cur_hist;
	char *cur_ptr;

	for (history_set_pos(0), cur_hist = current_history();
		 cur_hist; cur_hist = next_history())
		for (cur_ptr = cur_hist->line; *cur_ptr; cur_ptr++)
			if (*cur_ptr == NL_IN_HISTORY)
				*cur_ptr = '\n';
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
			read_history(psql_history);
			
		decode_history();
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
		encode_history();		
		if (write_history(fname) == 0)
			return true;

		psql_error("could not save history to file \"%s\": %s\n", fname, strerror(errno));
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

		saveHistory(psql_history);
		free(psql_history);
		psql_history = NULL;
	}
#endif
}
