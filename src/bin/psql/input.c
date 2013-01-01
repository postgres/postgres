/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2013, PostgreSQL Global Development Group
 *
 * src/bin/psql/input.c
 */
#include "postgres_fe.h"

#ifndef WIN32
#include <unistd.h>
#endif
#include <fcntl.h>

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

static char *psql_history;

static int	history_lines_added;


/*
 *	Preserve newlines in saved queries by mapping '\n' to NL_IN_HISTORY
 *
 *	It is assumed NL_IN_HISTORY will never be entered by the user
 *	nor appear inside a multi-byte string.	0x00 is not properly
 *	handled by the readline routines so it can not be used
 *	for this purpose.
 */
#define NL_IN_HISTORY	0x01
#endif

static void finishInput(void);


/*
 * gets_interactive()
 *
 * Gets a line of interactive input, using readline if desired.
 * The result is a malloc'd string.
 *
 * Caller *must* have set up sigint_interrupt_jmp before calling.
 */
char *
gets_interactive(const char *prompt)
{
#ifdef USE_READLINE
	if (useReadline)
	{
		char	   *result;

		/* Enable SIGINT to longjmp to sigint_interrupt_jmp */
		sigint_interrupt_enabled = true;

		/* On some platforms, readline is declared as readline(char *) */
		result = readline((char *) prompt);

		/* Disable SIGINT again */
		sigint_interrupt_enabled = false;

		return result;
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
	if (useHistory && s)
	{
		appendPQExpBufferStr(history_buf, s);
		if (!s[0] || s[strlen(s) - 1] != '\n')
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

	char	   *s = history_buf->data;
	int			i;

	/* Trim any trailing \n's (OK to scribble on history_buf) */
	for (i = strlen(s) - 1; i >= 0 && s[i] == '\n'; i--)
		;
	s[i + 1] = '\0';

	if (useHistory && s[0])
	{
		if (((pset.histcontrol & hctl_ignorespace) &&
			 s[0] == ' ') ||
			((pset.histcontrol & hctl_ignoredups) &&
			 prev_hist && strcmp(s, prev_hist) == 0))
		{
			/* Ignore this line as far as history is concerned */
		}
		else
		{
			/* Save each previous line for ignoredups processing */
			if (prev_hist)
				free(prev_hist);
			prev_hist = pg_strdup(s);
			/* And send it to readline */
			add_history(s);
			/* Count lines added to history for use later */
			history_lines_added++;
		}
	}

	resetPQExpBuffer(history_buf);
#endif
}


/*
 * gets_fromFile
 *
 * Gets a line of noninteractive input from a file (which could be stdin).
 * The result is a malloc'd string, or NULL on EOF or input error.
 *
 * Caller *must* have set up sigint_interrupt_jmp before calling.
 *
 * Note: we re-use a static PQExpBuffer for each call.	This is to avoid
 * leaking memory if interrupted by SIGINT.
 */
char *
gets_fromFile(FILE *source)
{
	static PQExpBuffer buffer = NULL;

	char		line[1024];

	if (buffer == NULL)			/* first time through? */
		buffer = createPQExpBuffer();
	else
		resetPQExpBuffer(buffer);

	for (;;)
	{
		char	   *result;

		/* Enable SIGINT to longjmp to sigint_interrupt_jmp */
		sigint_interrupt_enabled = true;

		/* Get some data */
		result = fgets(line, sizeof(line), source);

		/* Disable SIGINT again */
		sigint_interrupt_enabled = false;

		/* EOF or error? */
		if (result == NULL)
		{
			if (ferror(source))
			{
				psql_error("could not read from input file: %s\n",
						   strerror(errno));
				return NULL;
			}
			break;
		}

		appendPQExpBufferStr(buffer, line);

		if (PQExpBufferBroken(buffer))
		{
			psql_error("out of memory\n");
			return NULL;
		}

		/* EOL? */
		if (buffer->data[buffer->len - 1] == '\n')
		{
			buffer->data[buffer->len - 1] = '\0';
			return pg_strdup(buffer->data);
		}
	}

	if (buffer->len > 0)		/* EOF after reading some bufferload(s) */
		return pg_strdup(buffer->data);

	/* EOF, so return null */
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
	char	   *cur_ptr;

	history_set_pos(0);
	for (cur_hist = current_history(); cur_hist; cur_hist = next_history())
	{
		/* some platforms declare HIST_ENTRY.line as const char * */
		for (cur_ptr = (char *) cur_hist->line; *cur_ptr; cur_ptr++)
			if (*cur_ptr == '\n')
				*cur_ptr = NL_IN_HISTORY;
	}
}

/*
 * Reverse the above encoding
 */
static void
decode_history(void)
{
	HIST_ENTRY *cur_hist;
	char	   *cur_ptr;

	history_set_pos(0);
	for (cur_hist = current_history(); cur_hist; cur_hist = next_history())
	{
		/* some platforms declare HIST_ENTRY.line as const char * */
		for (cur_ptr = (char *) cur_hist->line; *cur_ptr; cur_ptr++)
			if (*cur_ptr == NL_IN_HISTORY)
				*cur_ptr = '\n';
	}
}
#endif   /* USE_READLINE */


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
		const char *histfile;
		char		home[MAXPGPATH];

		useReadline = true;
		initialize_readline();

		useHistory = true;
		using_history();
		history_lines_added = 0;

		histfile = GetVariable(pset.vars, "HISTFILE");

		if (histfile == NULL)
		{
			char	   *envhist;

			envhist = getenv("PSQL_HISTORY");
			if (envhist != NULL && strlen(envhist) > 0)
				histfile = envhist;
		}

		if (histfile == NULL)
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
			psql_history = pg_strdup(histfile);
			expand_tilde(&psql_history);
		}

		if (psql_history)
		{
			read_history(psql_history);
			decode_history();
		}
	}
#endif

	atexit(finishInput);
}


/*
 * This function saves the readline history when user
 * runs \s command or when psql exits.
 *
 * fname: pathname of history file.  (Should really be "const char *",
 * but some ancient versions of readline omit the const-decoration.)
 *
 * max_lines: if >= 0, limit history file to that many entries.
 *
 * appendFlag: if true, try to append just our new lines to the file.
 * If false, write the whole available history.
 *
 * encodeFlag: whether to encode \n as \x01.  For \s calls we don't wish
 * to do that, but must do so when saving the final history file.
 */
bool
saveHistory(char *fname, int max_lines, bool appendFlag, bool encodeFlag)
{
#ifdef USE_READLINE

	/*
	 * Suppressing the write attempt when HISTFILE is set to /dev/null may
	 * look like a negligible optimization, but it's necessary on e.g. Darwin,
	 * where write_history will fail because it tries to chmod the target
	 * file.
	 */
	if (useHistory && fname &&
		strcmp(fname, DEVNULL) != 0)
	{
		if (encodeFlag)
			encode_history();

		/*
		 * On newer versions of libreadline, truncate the history file as
		 * needed and then append what we've added.  This avoids overwriting
		 * history from other concurrent sessions (although there are still
		 * race conditions when two sessions exit at about the same time). If
		 * we don't have those functions, fall back to write_history().
		 *
		 * Note: return value of write_history is not standardized across GNU
		 * readline and libedit.  Therefore, check for errno becoming set to
		 * see if the write failed.  Similarly for append_history.
		 */
#if defined(HAVE_HISTORY_TRUNCATE_FILE) && defined(HAVE_APPEND_HISTORY)
		if (appendFlag)
		{
			int			nlines;
			int			fd;

			/* truncate previous entries if needed */
			if (max_lines >= 0)
			{
				nlines = Max(max_lines - history_lines_added, 0);
				(void) history_truncate_file(fname, nlines);
			}
			/* append_history fails if file doesn't already exist :-( */
			fd = open(fname, O_CREAT | O_WRONLY | PG_BINARY, 0600);
			if (fd >= 0)
				close(fd);
			/* append the appropriate number of lines */
			if (max_lines >= 0)
				nlines = Min(max_lines, history_lines_added);
			else
				nlines = history_lines_added;
			errno = 0;
			(void) append_history(nlines, fname);
			if (errno == 0)
				return true;
		}
		else
#endif
		{
			/* truncate what we have ... */
			if (max_lines >= 0)
				stifle_history(max_lines);
			/* ... and overwrite file.	Tough luck for concurrent sessions. */
			errno = 0;
			(void) write_history(fname);
			if (errno == 0)
				return true;
		}

		psql_error("could not save history to file \"%s\": %s\n",
				   fname, strerror(errno));
	}
#else
	/* only get here in \s case, so complain */
	psql_error("history is not supported by this installation\n");
#endif

	return false;
}


static void
finishInput(void)
{
#ifdef USE_READLINE
	if (useHistory && psql_history)
	{
		int			hist_size;

		hist_size = GetVariableNum(pset.vars, "HISTSIZE", 500, -1, true);
		saveHistory(psql_history, hist_size, true, true);
		free(psql_history);
		psql_history = NULL;
	}
#endif
}
