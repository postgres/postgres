/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright 2000 by PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/port/sprompt.c,v 1.2 2003/10/26 04:29:15 momjian Exp $
 */


/*
 * simple_prompt
 *
 * Generalized function especially intended for reading in usernames and
 * password interactively. Reads from /dev/tty or stdin/stderr.
 *
 * prompt:		The prompt to print
 * maxlen:		How many characters to accept
 * echo:		Set to false if you want to hide what is entered (for passwords)
 *
 * Returns a malloc()'ed string with the input (w/o trailing newline).
 */
#include "postgres.h"

#ifdef HAVE_TERMIOS_H
#include <termios.h>
#else
#ifdef WIN32
#include <windows.h>
#undef ERROR
#endif
#endif

bool		prompt_state = false;
extern char *simple_prompt(const char *prompt, int maxlen, bool echo);

char *
simple_prompt(const char *prompt, int maxlen, bool echo)
{
	int			length;
	char	   *destination;
	FILE	   *termin,
			   *termout;

#ifdef HAVE_TERMIOS_H
	struct termios t_orig,
				t;

#else
#ifdef WIN32
	HANDLE		t;
	LPDWORD		t_orig;
#endif
#endif

	destination = (char *) malloc(maxlen + 1);
	if (!destination)
		return NULL;

	prompt_state = true;		/* disable SIGINT */

	/*
	 * Do not try to collapse these into one "w+" mode file. Doesn't work
	 * on some platforms (eg, HPUX 10.20).
	 */
	termin = fopen("/dev/tty", "r");
	termout = fopen("/dev/tty", "w");
	if (!termin || !termout)
	{
		if (termin)
			fclose(termin);
		if (termout)
			fclose(termout);
		termin = stdin;
		termout = stderr;
	}

#ifdef HAVE_TERMIOS_H
	if (!echo)
	{
		tcgetattr(fileno(termin), &t);
		t_orig = t;
		t.c_lflag &= ~ECHO;
		tcsetattr(fileno(termin), TCSAFLUSH, &t);
	}
#else
#ifdef WIN32
	if (!echo)
	{
		/* get a new handle to turn echo off */
		t_orig = (LPDWORD) malloc(sizeof(DWORD));
		t = GetStdHandle(STD_INPUT_HANDLE);

		/* save the old configuration first */
		GetConsoleMode(t, t_orig);

		/* set to the new mode */
		SetConsoleMode(t, ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
	}
#endif
#endif

	if (prompt)
	{
		fputs(gettext(prompt), termout);
		fflush(termout);
	}

	if (fgets(destination, maxlen + 1, termin) == NULL)
		destination[0] = '\0';

	length = strlen(destination);
	if (length > 0 && destination[length - 1] != '\n')
	{
		/* eat rest of the line */
		char		buf[128];
		int			buflen;

		do
		{
			if (fgets(buf, sizeof(buf), termin) == NULL)
				break;
			buflen = strlen(buf);
		} while (buflen > 0 && buf[buflen - 1] != '\n');
	}

	if (length > 0 && destination[length - 1] == '\n')
		/* remove trailing newline */
		destination[length - 1] = '\0';

#ifdef HAVE_TERMIOS_H
	if (!echo)
	{
		tcsetattr(fileno(termin), TCSAFLUSH, &t_orig);
		fputs("\n", termout);
		fflush(termout);
	}
#else
#ifdef WIN32
	if (!echo)
	{
		/* reset to the original console mode */
		SetConsoleMode(t, *t_orig);
		fputs("\n", termout);
		fflush(termout);
		free(t_orig);
	}
#endif
#endif

	if (termin != stdin)
	{
		fclose(termin);
		fclose(termout);
	}

	prompt_state = false;		/* SIGINT okay again */

	return destination;
}
