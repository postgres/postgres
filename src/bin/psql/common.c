/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright 2000 by PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/common.c,v 1.57 2003/03/18 22:15:44 petere Exp $
 */
#include "postgres_fe.h"
#include "common.h"

#include <errno.h>
#include <stdarg.h>
#ifndef HAVE_STRDUP
#include <strdup.h>
#endif
#include <signal.h>
#ifndef WIN32
#include <sys/time.h>
#include <unistd.h>				/* for write() */
#include <setjmp.h>
#else
#include <io.h>					/* for _write() */
#include <win32.h>
#include <sys/timeb.h>			/* for _ftime() */
#endif

#include "libpq-fe.h"
#include "pqsignal.h"

#include "settings.h"
#include "variables.h"
#include "copy.h"
#include "prompt.h"
#include "print.h"
#include "mainloop.h"

extern bool prompt_state;

/*
 * "Safe" wrapper around strdup()
 */
char *
xstrdup(const char *string)
{
	char	   *tmp;

	if (!string)
	{
		fprintf(stderr, gettext("%s: xstrdup: cannot duplicate null pointer (internal error)\n"),
				pset.progname);
		exit(EXIT_FAILURE);
	}
	tmp = strdup(string);
	if (!tmp)
	{
		psql_error("out of memory\n");
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
		psql_error("%s: %s\n", fname, strerror(errno));
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
 * Error reporting for scripts. Errors should look like
 *	 psql:filename:lineno: message
 *
 */
void
psql_error(const char *fmt,...)
{
	va_list		ap;

	fflush(stdout);
	if (pset.queryFout != stdout)
		fflush(pset.queryFout);

	if (pset.inputfile)
		fprintf(stderr, "%s:%s:%u: ", pset.progname, pset.inputfile, pset.lineno);
	va_start(ap, fmt);
	vfprintf(stderr, gettext(fmt), ap);
	va_end(ap);
}



/*
 * for backend Notice messages (INFO, WARNING, etc)
 */
void
NoticeProcessor(void *arg, const char *message)
{
	(void) arg;					/* not used */
	psql_error("%s", message);
}



/*
 * Code to support query cancellation
 *
 * Before we start a query, we enable a SIGINT signal catcher that sends a
 * cancel request to the backend. Note that sending the cancel directly from
 * the signal handler is safe because PQrequestCancel() is written to make it
 * so. We use write() to print to stdout because it's better to use simple
 * facilities in a signal handler.
 */
PGconn	   *cancelConn;
volatile bool cancel_pressed;

#ifndef WIN32

#define write_stderr(String) write(fileno(stderr), String, strlen(String))

void
handle_sigint(SIGNAL_ARGS)
{
	int			save_errno = errno;

	/* Don't muck around if copying in or prompting for a password. */
	if ((copy_in_state && pset.cur_cmd_interactive) || prompt_state)
		return;

	if (cancelConn == NULL)
		siglongjmp(main_loop_jmp, 1);

	cancel_pressed = true;

	if (PQrequestCancel(cancelConn))
		write_stderr("Cancel request sent\n");
	else
	{
		write_stderr("Could not send cancel request: ");
		write_stderr(PQerrorMessage(cancelConn));
	}
	errno = save_errno;			/* just in case the write changed it */
}
#endif   /* not WIN32 */


/*
 * PSQLexec
 *
 * This is the way to send "backdoor" queries (those not directly entered
 * by the user). It is subject to -E but not -e.
 *
 * If the given querystring generates multiple PGresults, normally the last
 * one is returned to the caller.  However, if ignore_command_ok is TRUE,
 * then PGresults with status PGRES_COMMAND_OK are ignored.  This is intended
 * mainly to allow locutions such as "begin; select ...; commit".
 */
PGresult *
PSQLexec(const char *query, bool ignore_command_ok)
{
	PGresult   *res = NULL;
	PGresult   *newres;
	const char *var;
	ExecStatusType rstatus;

	if (!pset.db)
	{
		psql_error("You are currently not connected to a database.\n");
		return NULL;
	}

	var = GetVariable(pset.vars, "ECHO_HIDDEN");
	if (var)
	{
		printf("********* QUERY **********\n"
			   "%s\n"
			   "**************************\n\n", query);
		fflush(stdout);
	}

	if (var && strcmp(var, "noexec") == 0)
		return NULL;

	/* discard any uneaten results of past queries */
	while ((newres = PQgetResult(pset.db)) != NULL)
		PQclear(newres);

	cancelConn = pset.db;
	if (PQsendQuery(pset.db, query))
	{
		while ((newres = PQgetResult(pset.db)) != NULL)
		{
			rstatus = PQresultStatus(newres);
			if (ignore_command_ok && rstatus == PGRES_COMMAND_OK)
			{
				PQclear(newres);
				continue;
			}
			PQclear(res);
			res = newres;
			if (rstatus == PGRES_COPY_IN ||
				rstatus == PGRES_COPY_OUT)
				break;
		}
	}
	rstatus = PQresultStatus(res);
	/* keep cancel connection for copy out state */
	if (rstatus != PGRES_COPY_OUT)
		cancelConn = NULL;
	if (rstatus == PGRES_COPY_IN)
		copy_in_state = true;

	if (res && (rstatus == PGRES_COMMAND_OK ||
				rstatus == PGRES_TUPLES_OK ||
				rstatus == PGRES_COPY_IN ||
				rstatus == PGRES_COPY_OUT))
		return res;
	else
	{
		psql_error("%s", PQerrorMessage(pset.db));
		PQclear(res);

		if (PQstatus(pset.db) == CONNECTION_BAD)
		{
			if (!pset.cur_cmd_interactive)
			{
				psql_error("connection to server was lost\n");
				exit(EXIT_BADCONN);
			}
			fputs(gettext("The connection to the server was lost. Attempting reset: "), stderr);
			PQreset(pset.db);
			if (PQstatus(pset.db) == CONNECTION_BAD)
			{
				fputs(gettext("Failed.\n"), stderr);
				PQfinish(pset.db);
				pset.db = NULL;
				SetVariable(pset.vars, "DBNAME", NULL);
				SetVariable(pset.vars, "HOST", NULL);
				SetVariable(pset.vars, "PORT", NULL);
				SetVariable(pset.vars, "USER", NULL);
				SetVariable(pset.vars, "ENCODING", NULL);
			}
			else
				fputs(gettext("Succeeded.\n"), stderr);
		}

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
#ifndef WIN32
	struct timeval before,
				after;
#else
	struct _timeb before,
				after;
#endif

	if (!pset.db)
	{
		psql_error("You are currently not connected to a database.\n");
		return false;
	}

	if (GetVariableBool(pset.vars, "SINGLESTEP"))
	{
		char		buf[3];

		printf(gettext("***(Single step mode: Verify query)*********************************************\n"
					   "%s\n"
					   "***(press return to proceed or enter x and return to cancel)********************\n"),
			   query);
		fflush(stdout);
		if (fgets(buf, sizeof(buf), stdin) != NULL)
			if (buf[0] == 'x')
				return false;
	}
	else
	{
		const char *var = GetVariable(pset.vars, "ECHO");

		if (var && strncmp(var, "queries", strlen(var)) == 0)
			puts(query);
	}

	cancelConn = pset.db;

#ifndef WIN32
	if (pset.timing)
		gettimeofday(&before, NULL);
	results = PQexec(pset.db, query);
	if (pset.timing)
		gettimeofday(&after, NULL);
#else
	if (pset.timing)
		_ftime(&before);
	results = PQexec(pset.db, query);
	if (pset.timing)
		_ftime(&after);
#endif

	if (PQresultStatus(results) == PGRES_COPY_IN)
		copy_in_state = true;
	/* keep cancel connection for copy out state */
	if (PQresultStatus(results) != PGRES_COPY_OUT)
		cancelConn = NULL;

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
					FILE	   *queryFout_copy = pset.queryFout;
					bool		queryFoutPipe_copy = pset.queryFoutPipe;

					pset.queryFout = stdout;	/* so it doesn't get
												 * closed */

					/* open file/pipe */
					if (!setQFout(pset.gfname))
					{
						pset.queryFout = queryFout_copy;
						pset.queryFoutPipe = queryFoutPipe_copy;
						success = false;
						break;
					}

					printQuery(results, &pset.popt, pset.queryFout);

					/* close file/pipe, restore old setting */
					setQFout(NULL);

					pset.queryFout = queryFout_copy;
					pset.queryFoutPipe = queryFoutPipe_copy;

					free(pset.gfname);
					pset.gfname = NULL;

					success = true;
				}
				else
				{
					printQuery(results, &pset.popt, pset.queryFout);
					success = true;
				}
				break;
			case PGRES_EMPTY_QUERY:
				success = true;
				break;
			case PGRES_COMMAND_OK:
				{
					char		buf[10];

					success = true;
					sprintf(buf, "%u", (unsigned int) PQoidValue(results));
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
					puts(gettext("Enter data to be copied followed by a newline.\n"
								 "End with a backslash and a period on a line by itself."));

				success = handleCopyIn(pset.db, pset.cur_cmd_source,
									   pset.cur_cmd_interactive ? get_prompt(PROMPT_COPY) : NULL);
				break;

			case PGRES_NONFATAL_ERROR:
			case PGRES_FATAL_ERROR:
			case PGRES_BAD_RESPONSE:
				success = false;
				psql_error("%s", PQerrorMessage(pset.db));
				break;
		}

		fflush(pset.queryFout);

		if (PQstatus(pset.db) == CONNECTION_BAD)
		{
			if (!pset.cur_cmd_interactive)
			{
				psql_error("connection to server was lost\n");
				exit(EXIT_BADCONN);
			}
			fputs(gettext("The connection to the server was lost. Attempting reset: "), stderr);
			PQreset(pset.db);
			if (PQstatus(pset.db) == CONNECTION_BAD)
			{
				fputs(gettext("Failed.\n"), stderr);
				PQfinish(pset.db);
				PQclear(results);
				pset.db = NULL;
				SetVariable(pset.vars, "DBNAME", NULL);
				SetVariable(pset.vars, "HOST", NULL);
				SetVariable(pset.vars, "PORT", NULL);
				SetVariable(pset.vars, "USER", NULL);
				SetVariable(pset.vars, "ENCODING", NULL);
				return false;
			}
			else
				fputs(gettext("Succeeded.\n"), stderr);
		}

		/* check for asynchronous notification returns */
		while ((notify = PQnotifies(pset.db)) != NULL)
		{
			fprintf(pset.queryFout, gettext("Asynchronous NOTIFY '%s' from backend with pid %d received.\n"),
					notify->relname, notify->be_pid);
			free(notify);
			fflush(pset.queryFout);
		}

		if (results)
			PQclear(results);
	}

	/* Possible microtiming output */
	if (pset.timing && success)
#ifndef WIN32
		printf(gettext("Time: %.2f ms\n"),
			   ((after.tv_sec - before.tv_sec) * 1000000.0 + after.tv_usec - before.tv_usec) / 1000.0);
#else
		printf(gettext("Time: %.2f ms\n"),
			   ((after.time - before.time) * 1000.0 + after.millitm - before.millitm));
#endif

	return success;
}
