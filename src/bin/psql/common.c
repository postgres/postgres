#include <c.h>
#include "common.h"

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif
#ifndef HAVE_STRDUP
#include <strdup.h>
#endif
#include <signal.h>
#ifndef WIN32
#include <unistd.h>				/* for write() */
#else
#include <io.h>                 /* for _write() */
#endif

#include <libpq-fe.h>
#include <postgres_ext.h>
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
#define write(a,b,c) _write(a,b,c)
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
setQFout(const char *fname)
{
	bool		status = true;

	/* Close old file/pipe */
	if (pset.queryFout && pset.queryFout != stdout && pset.queryFout != stderr)
	{
		if (pset.queryFoutPipe)
			pclose(pset.queryFout);
		else
			fclose(pset.queryFout);
	}

	/* If no filename, set stdout */
	if (!fname || fname[0] == '\0')
	{
		pset.queryFout = stdout;
		pset.queryFoutPipe = false;
	}
	else if (*fname == '|')
	{
		pset.queryFout = popen(fname + 1, "w");
		pset.queryFoutPipe = true;
	}
	else
	{
		pset.queryFout = fopen(fname, "w");
		pset.queryFoutPipe = false;
	}

	if (!(pset.queryFout))
	{
		fprintf(stderr, "%s: %s: %s\n", pset.progname, fname, strerror(errno));
		pset.queryFout = stdout;
		pset.queryFoutPipe = false;
		status = false;
	}

	/* Direct signals */
#ifndef WIN32
	if (pset.queryFoutPipe)
		pqsignal(SIGPIPE, SIG_IGN);
	else
		pqsignal(SIGPIPE, SIG_DFL);
#endif

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
 * Code to support query cancellation
 *
 * Before we start a query, we enable a SIGINT signal catcher that sends a
 * cancel request to the backend. Note that sending the cancel directly from
 * the signal handler is safe because PQrequestCancel() is written to make it
 * so. We have to be very careful what else we do in the signal handler. This
 * includes using write() for output.
 */

static PGconn *cancelConn;

#define write_stderr(String) write(fileno(stderr), String, strlen(String))

static void
handle_sigint(SIGNAL_ARGS)
{
	if (cancelConn == NULL)
		return;
	/* Try to send cancel request */
	if (PQrequestCancel(cancelConn))
		write_stderr("\nCancel request sent\n");
	else
	{
		write_stderr("\nCould not send cancel request: ");
		write_stderr(PQerrorMessage(cancelConn));
	}
}



/*
 * PSQLexec
 *
 * This is the way to send "backdoor" queries (those not directly entered
 * by the user). It is subject to -E but not -e.
 */
PGresult   *
PSQLexec(const char *query)
{
	PGresult   *res;
	const char *var;

	if (!pset.db)
	{
		fputs("You are currently not connected to a database.\n", stderr);
		return NULL;
	}

	var = GetVariable(pset.vars, "ECHO_HIDDEN");
	if (var)
	{
		printf("********* QUERY *********\n%s\n*************************\n\n", query);
		fflush(stdout);
	}

	if (var && strcmp(var, "noexec") == 0)
		return NULL;

	cancelConn = pset.db;
#ifndef WIN32
	pqsignal(SIGINT, handle_sigint);	/* control-C => cancel */
#endif

	res = PQexec(pset.db, query);

#ifndef WIN32
	pqsignal(SIGINT, SIG_DFL);	/* now control-C is back to normal */
#endif

	if (PQstatus(pset.db) == CONNECTION_BAD)
	{
        if (!pset.cur_cmd_interactive)
        {
            fprintf(stderr, "%s: connection to server was lost", pset.progname);
            exit(EXIT_BADCONN);
        }
		fputs("The connection to the server was lost. Attempting reset: ", stderr);
		PQreset(pset.db);
		if (PQstatus(pset.db) == CONNECTION_BAD)
		{
			fputs("Failed.\n", stderr);
			PQfinish(pset.db);
			PQclear(res);
			pset.db = NULL;
            SetVariable(pset.vars, "DBNAME", NULL);
            SetVariable(pset.vars, "HOST", NULL);
            SetVariable(pset.vars, "PORT", NULL);
            SetVariable(pset.vars, "USER", NULL);
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
		fputs(PQerrorMessage(pset.db), stderr);
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
SendQuery(const char *query)
{
	bool		success = false;
	PGresult   *results;
	PGnotify   *notify;

	if (!pset.db)
	{
		fputs("You are currently not connected to a database.\n", stderr);
		return false;
	}

	if (GetVariableBool(pset.vars, "SINGLESTEP"))
	{
		char		buf[3];

		printf("***(Single step mode: Verify query)*********************************************\n"
               "%s\n"
               "***(press return to proceed or enter x and return to cancel)********************\n",
               query);
		fflush(stdout);
		fgets(buf, 3, stdin);
		if (buf[0] == 'x')
			return false;
	}
    else
    {
        const char * var = GetVariable(pset.vars, "ECHO");
        if (var && strcmp(var, "brief")==0)
            puts(query);
    }

	cancelConn = pset.db;
#ifndef WIN32
	pqsignal(SIGINT, handle_sigint);
#endif

	results = PQexec(pset.db, query);

#ifndef WIN32
	pqsignal(SIGINT, SIG_DFL);
#endif

	if (results == NULL)
	{
		fputs(PQerrorMessage(pset.db), pset.queryFout);
		success = false;
	}
	else
	{
		switch (PQresultStatus(results))
		{
			case PGRES_TUPLES_OK:
                /* write output to \g argument, if any */
				if (pset.gfname)
				{
					FILE * queryFout_copy = pset.queryFout;
                    bool queryFoutPipe_copy = pset.queryFoutPipe;
                    pset.queryFout = NULL; /* so it doesn't get closed */

                    /* open file/pipe */
					if (!setQFout(pset.gfname))
					{
						success = false;
						break;
					}

					printQuery(results, &pset.popt, pset.queryFout);

					/* close file/pipe */
					setQFout(NULL);

					free(pset.gfname);
					pset.gfname = NULL;

					pset.queryFout = queryFout_copy;
                    pset.queryFoutPipe = queryFoutPipe_copy;

					success = true;
					break;
				}
				else
				{
					success = true;
					printQuery(results, &pset.popt, pset.queryFout);
				}
				break;
			case PGRES_EMPTY_QUERY:
				success = true;
				break;
			case PGRES_COMMAND_OK:
            {
                char buf[10];

				success = true;
                sprintf(buf, "%u", (unsigned int)PQoidValue(results));
                if (!QUIET())
                    fprintf(pset.queryFout, "%s\n", PQcmdStatus(results));
                SetVariable(pset.vars, "LASTOID", buf);
				break;
            }
			case PGRES_COPY_OUT:
				success = handleCopyOut(pset.db, pset.queryFout);
				break;

			case PGRES_COPY_IN:
				if (pset.cur_cmd_interactive && !QUIET())
					puts("Enter data to be copied followed by a newline.\n"
						 "End with a backslash and a period on a line by itself.");

				success = handleCopyIn(pset.db, pset.cur_cmd_source,
									   pset.cur_cmd_interactive ? get_prompt(PROMPT_COPY) : NULL);
				break;

			case PGRES_NONFATAL_ERROR:
			case PGRES_FATAL_ERROR:
			case PGRES_BAD_RESPONSE:
				success = false;
				fputs(PQerrorMessage(pset.db), stderr);
				break;
		}

        fflush(pset.queryFout);

		if (PQstatus(pset.db) == CONNECTION_BAD)
		{
            if (!pset.cur_cmd_interactive)
            {
                fprintf(stderr, "%s: connection to server was lost", pset.progname);
                exit(EXIT_BADCONN);
            }
			fputs("The connection to the server was lost. Attempting reset: ", stderr);
			PQreset(pset.db);
			if (PQstatus(pset.db) == CONNECTION_BAD)
			{
				fputs("Failed.\n", stderr);
				PQfinish(pset.db);
				PQclear(results);
				pset.db = NULL;
                SetVariable(pset.vars, "DBNAME", NULL);
                SetVariable(pset.vars, "HOST", NULL);
                SetVariable(pset.vars, "PORT", NULL);
                SetVariable(pset.vars, "USER", NULL);
				return false;
			}
			else
				fputs("Succeeded.\n", stderr);
		}
        
		/* check for asynchronous notification returns */
		while ((notify = PQnotifies(pset.db)) != NULL)
		{
			fprintf(pset.queryFout, "Asynchronous NOTIFY '%s' from backend with pid '%d' received.\n",
					notify->relname, notify->be_pid);
			free(notify);
            fflush(pset.queryFout);
		}

		if (results)
			PQclear(results);
    }

	return success;
}
