#include <config.h>
#include <c.h>
#include "common.h"

#include <stdlib.h>
#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif
#include <stdio.h>
#include <string.h>
#ifndef HAVE_STRDUP
#include <strdup.h>
#endif
#include <signal.h>
#include <assert.h>
#ifndef WIN32
#include <unistd.h>				/* for write() */
#endif

#include <libpq-fe.h>
#include <pqsignal.h>
#include <version.h>

#include "settings.h"
#include "variables.h"
#include "copy.h"
#include "prompt.h"
#include "print.h"

#ifdef WIN32
#define popen(x,y) _popen(x,y)
#define pclose(x) _pclose(x)
#endif



/* xstrdup()
 *
 * "Safe" wrapper around strdup()
 * (Using this also avoids writing #ifdef HAVE_STRDUP in every file :)
 */
char *
xstrdup(const char *string)
{
	char	   *tmp;

	if (!string)
	{
		fprintf(stderr, "xstrdup: Cannot duplicate null pointer.\n");
		exit(EXIT_FAILURE);
	}
	tmp = strdup(string);
	if (!tmp)
	{
		perror("strdup");
		exit(EXIT_FAILURE);
	}
	return tmp;
}



/*
 * setQFout
 * -- handler for -o command line option and \o command
 *
 * Tries to open file fname (or pipe if fname starts with '|')
 * and stores the file handle in pset)
 * Upon failure, sets stdout and returns false.
 */
bool
setQFout(const char *fname, PsqlSettings *pset)
{
	bool		status = true;

#ifdef USE_ASSERT_CHECKING
	assert(pset);
#else
	if (!pset)
		return false;
#endif

	/* Close old file/pipe */
	if (pset->queryFout && pset->queryFout != stdout && pset->queryFout != stderr)
	{
		if (pset->queryFoutPipe)
			pclose(pset->queryFout);
		else
			fclose(pset->queryFout);
	}

	/* If no filename, set stdout */
	if (!fname || fname[0] == '\0')
	{
		pset->queryFout = stdout;
		pset->queryFoutPipe = false;
	}
	else if (*fname == '|')
	{
		const char *pipename = fname + 1;


#ifndef __CYGWIN32__
		pset->queryFout = popen(pipename, "w");
#else
		pset->queryFout = popen(pipename, "wb");
#endif
		pset->queryFoutPipe = true;
	}
	else
	{
#ifndef __CYGWIN32__
		pset->queryFout = fopen(fname, "w");
#else
		pset->queryFout = fopen(fname, "wb");
#endif
		pset->queryFoutPipe = false;
	}

	if (!pset->queryFout)
	{
		perror(fname);
		pset->queryFout = stdout;
		pset->queryFoutPipe = false;
		status = false;
	}

	/* Direct signals */
	if (pset->queryFoutPipe)
		pqsignal(SIGPIPE, SIG_IGN);
	else
		pqsignal(SIGPIPE, SIG_DFL);

	return status;
}



/*
 * simple_prompt
 *
 * Generalized function especially intended for reading in usernames and
 * password interactively. Reads from stdin.
 *
 * prompt:		The prompt to print
 * maxlen:		How many characters to accept
 * echo:		Set to false if you want to hide what is entered (for passwords)
 *
 * Returns a malloc()'ed string with the input (w/o trailing newline).
 */
char *
simple_prompt(const char *prompt, int maxlen, bool echo)
{
	int			length;
	char	   *destination;

#ifdef HAVE_TERMIOS_H
	struct termios t_orig,
				t;

#endif

	destination = (char *) malloc(maxlen + 2);
	if (!destination)
		return NULL;
	if (prompt)
		fputs(prompt, stdout);

#ifdef HAVE_TERMIOS_H
	if (!echo)
	{
		tcgetattr(0, &t);
		t_orig = t;
		t.c_lflag &= ~ECHO;
		tcsetattr(0, TCSADRAIN, &t);
	}
#endif

	fgets(destination, maxlen, stdin);

#ifdef HAVE_TERMIOS_H
	if (!echo)
	{
		tcsetattr(0, TCSADRAIN, &t_orig);
		puts("");
	}
#endif

	length = strlen(destination);
	if (length > 0 && destination[length - 1] != '\n')
	{
		/* eat rest of the line */
		char		buf[512];

		do
		{
			fgets(buf, 512, stdin);
		} while (buf[strlen(buf) - 1] != '\n');
	}

	if (length > 0 && destination[length - 1] == '\n')
		/* remove trailing newline */
		destination[length - 1] = '\0';

	return destination;
}



/*
 * interpolate_var()
 *
 * If the variable is a regular psql variable, just return its value.
 * If it's a magic variable, return that value.
 *
 * This function only returns NULL if you feed in NULL. Otherwise it's ready for
 * immediate consumption.
 */
const char *
interpolate_var(const char *name, PsqlSettings *pset)
{
	const char *var;

#ifdef USE_ASSERT_CHECKING
	assert(name);
	assert(pset);
#else
	if (!name || !pset)
		return NULL;
#endif

	if (strspn(name, VALID_VARIABLE_CHARS) == strlen(name))
	{
		var = GetVariable(pset->vars, name);
		if (var)
			return var;
		else
			return "";
	}

	/* otherwise return magic variable */

	/*
	 * (by convention these should be capitalized (but not all caps), to
	 * not be shadowed by regular vars or to shadow env vars)
	 */
	if (strcmp(name, "Version") == 0)
		return PG_VERSION_STR;

	if (strcmp(name, "Database") == 0)
	{
		if (PQdb(pset->db))
			return PQdb(pset->db);
		else
			return "";
	}

	if (strcmp(name, "User") == 0)
	{
		if (PQuser(pset->db))
			return PQuser(pset->db);
		else
			return "";
	}

	if (strcmp(name, "Host") == 0)
	{
		if (PQhost(pset->db))
			return PQhost(pset->db);
		else
			return "";
	}

	if (strcmp(name, "Port") == 0)
	{
		if (PQport(pset->db))
			return PQport(pset->db);
		else
			return "";
	}

	/*
	 * env vars (if env vars are all caps there should be no prob,
	 * otherwise you're on your own
	 */

	if ((var = getenv(name)))
		return var;

	return "";
}



/*
 * Code to support command cancellation.
 *
 * If interactive, we enable a SIGINT signal catcher before we start a
 * query that sends a cancel request to the backend.
 * Note that sending the cancel directly from the signal handler is safe
 * only because PQrequestCancel is carefully written to make it so. We
 * have to be very careful what else we do in the signal handler.
 *
 * Writing on stderr is potentially dangerous, if the signal interrupted
 * some stdio operation on stderr. On Unix we can avoid trouble by using
 * write() instead; on Windows that's probably not workable, but we can
 * at least avoid trusting printf by using the more primitive fputs().
 */

PGconn	   *cancelConn;

#ifdef WIN32
#define safe_write_stderr(String) fputs(s, stderr)
#else
#define safe_write_stderr(String) write(fileno(stderr), String, strlen(String))
#endif


static void
handle_sigint(SIGNAL_ARGS)
{
	/* accept signal if no connection */
	if (cancelConn == NULL)
		exit(1);
	/* Try to send cancel request */
	if (PQrequestCancel(cancelConn))
		safe_write_stderr("\nCANCEL request sent\n");
	else
	{
		safe_write_stderr("\nCould not send cancel request: ");
		safe_write_stderr(PQerrorMessage(cancelConn));
	}
}



/*
 * PSQLexec
 *
 * This is the way to send "backdoor" queries (those not directly entered
 * by the user). It is subject to -E (echo_secret) but not -e (echo).
 */
PGresult   *
PSQLexec(PsqlSettings *pset, const char *query)
{
	PGresult   *res;
	const char *var;

	if (!pset->db)
	{
		fputs("You are not currently connected to a database.\n", stderr);
		return NULL;
	}

	var = GetVariable(pset->vars, "echo_secret");
	if (var)
	{
		printf("********* QUERY *********\n%s\n*************************\n\n", query);
		fflush(stdout);
	}

	if (var && strcmp(var, "noexec") == 0)
		return NULL;

	cancelConn = pset->db;
	pqsignal(SIGINT, handle_sigint);	/* control-C => cancel */

	res = PQexec(pset->db, query);

	pqsignal(SIGINT, SIG_DFL);	/* no control-C is back to normal */

	if (PQstatus(pset->db) == CONNECTION_BAD)
	{
		fputs("The connection to the server was lost. Attempting reset: ", stderr);
		PQreset(pset->db);
		if (PQstatus(pset->db) == CONNECTION_BAD)
		{
			fputs("Failed.\n", stderr);
			PQfinish(pset->db);
			PQclear(res);
			pset->db = NULL;
			return NULL;
		}
		else
			fputs("Succeeded.\n", stderr);
	}

	if (res && (PQresultStatus(res) == PGRES_COMMAND_OK ||
				PQresultStatus(res) == PGRES_TUPLES_OK ||
				PQresultStatus(res) == PGRES_COPY_IN ||
				PQresultStatus(res) == PGRES_COPY_OUT)
		)
		return res;
	else
	{
		fprintf(stderr, "%s", PQerrorMessage(pset->db));
		PQclear(res);
		return NULL;
	}
}



/*
 * SendQuery: send the query string to the backend
 * (and print out results)
 *
 * Note: This is the "front door" way to send a query. That is, use it to
 * send queries actually entered by the user. These queries will be subject to
 * single step mode.
 * To send "back door" queries (generated by slash commands, etc.) in a
 * controlled way, use PSQLexec().
 *
 * Returns true if the query executed successfully, false otherwise.
 */
bool
SendQuery(PsqlSettings *pset, const char *query)
{
	bool		success = false;
	PGresult   *results;
	PGnotify   *notify;

	if (!pset->db)
	{
		fputs("You are not currently connected to a database.\n", stderr);
		return false;
	}

	if (GetVariableBool(pset->vars, "singlestep"))
	{
		char		buf[3];

		fprintf(stdout, "***(Single step mode: Verify query)*********************************************\n"
				"QUERY: %s\n"
				"***(press return to proceed or enter x and return to cancel)********************\n",
				query);
		fflush(stdout);
		fgets(buf, 3, stdin);
		if (buf[0] == 'x')
			return false;
		fflush(stdin);
	}

	cancelConn = pset->db;
	pqsignal(SIGINT, handle_sigint);

	results = PQexec(pset->db, query);

	pqsignal(SIGINT, SIG_DFL);

	if (results == NULL)
	{
		fputs(PQerrorMessage(pset->db), pset->queryFout);
		success = false;
	}
	else
	{
		switch (PQresultStatus(results))
		{
			case PGRES_TUPLES_OK:
				if (pset->gfname)
				{
					PsqlSettings settings_copy = *pset;

					settings_copy.queryFout = stdout;
					if (!setQFout(pset->gfname, &settings_copy))
					{
						success = false;
						break;
					}

					printQuery(results, &settings_copy.popt, settings_copy.queryFout);

					/* close file/pipe */
					setQFout(NULL, &settings_copy);

					free(pset->gfname);
					pset->gfname = NULL;

					success = true;
					break;
				}
				else
				{
					success = true;
					printQuery(results, &pset->popt, pset->queryFout);
					fflush(pset->queryFout);
				}
				break;
			case PGRES_EMPTY_QUERY:
				success = true;
				break;
			case PGRES_COMMAND_OK:
				success = true;
				fprintf(pset->queryFout, "%s\n", PQcmdStatus(results));
				break;

			case PGRES_COPY_OUT:
				if (pset->cur_cmd_interactive && !GetVariable(pset->vars, "quiet"))
					puts("Copy command returns:");

				success = handleCopyOut(pset->db, pset->queryFout);
				break;

			case PGRES_COPY_IN:
				if (pset->cur_cmd_interactive && !GetVariable(pset->vars, "quiet"))
					puts("Enter data to be copied followed by a newline.\n"
						 "End with a backslash and a period on a line by itself.");

				success = handleCopyIn(pset->db, pset->cur_cmd_source,
									   pset->cur_cmd_interactive ? get_prompt(pset, PROMPT_COPY) : NULL);
				break;

			case PGRES_NONFATAL_ERROR:
			case PGRES_FATAL_ERROR:
			case PGRES_BAD_RESPONSE:
				success = false;
				fputs(PQerrorMessage(pset->db), pset->queryFout);
				break;
		}

		if (PQstatus(pset->db) == CONNECTION_BAD)
		{
			fputs("The connection to the server was lost. Attempting reset: ", stderr);
			PQreset(pset->db);
			if (PQstatus(pset->db) == CONNECTION_BAD)
			{
				fputs("Failed.\n", stderr);
				PQfinish(pset->db);
				PQclear(results);
				pset->db = NULL;
				return false;
			}
			else
				fputs("Succeeded.\n", stderr);
		}

		/* check for asynchronous notification returns */
		while ((notify = PQnotifies(pset->db)) != NULL)
		{
			fprintf(pset->queryFout, "Asynchronous NOTIFY '%s' from backend with pid '%d' received.\n",
					notify->relname, notify->be_pid);
			free(notify);
		}

		if (results)
			PQclear(results);
	}

	return success;
}
