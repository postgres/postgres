/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2003, PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/common.c,v 1.76.2.1 2003/11/12 22:55:42 tgl Exp $
 */
#include "postgres_fe.h"
#include "common.h"

#include <ctype.h>
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
#include "command.h"
#include "copy.h"
#include "prompt.h"
#include "print.h"
#include "mainloop.h"
#include "mb/pg_wchar.h"


/* Workarounds for Windows */
/* Probably to be moved up the source tree in the future, perhaps to be replaced by
 * more specific checks like configure-style HAVE_GETTIMEOFDAY macros.
 */
#ifndef WIN32

typedef struct timeval TimevalStruct;

#define GETTIMEOFDAY(T) gettimeofday(T, NULL)
#define DIFF_MSEC(T, U) \
	((((int) ((T)->tv_sec - (U)->tv_sec)) * 1000000.0 + \
	  ((int) ((T)->tv_usec - (U)->tv_usec))) / 1000.0)

#else

typedef struct _timeb TimevalStruct;

#define GETTIMEOFDAY(T) _ftime(T)
#define DIFF_MSEC(T, U) \
	(((T)->time - (U)->time) * 1000.0 + \
	 ((T)->millitm - (U)->millitm))

#endif

extern bool prompt_state;


static bool is_transact_command(const char *query);


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
	pqsignal(SIGPIPE, pset.queryFoutPipe ? SIG_IGN : SIG_DFL);
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
static PGconn *volatile cancelConn = NULL;

volatile bool cancel_pressed = false;


#ifndef WIN32

#define write_stderr(String) write(fileno(stderr), String, strlen(String))

void
handle_sigint(SIGNAL_ARGS)
{
	int			save_errno = errno;

	/* Don't muck around if prompting for a password. */
	if (prompt_state)
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



/* ConnectionUp
 *
 * Returns whether our backend connection is still there.
 */
static bool
ConnectionUp()
{
	return PQstatus(pset.db) != CONNECTION_BAD;
}



/* CheckConnection
 *
 * Verify that we still have a good connection to the backend, and if not,
 * see if it can be restored.
 *
 * Returns true if either the connection was still there, or it could be
 * restored successfully; false otherwise.	If, however, there was no
 * connection and the session is non-interactive, this will exit the program
 * with a code of EXIT_BADCONN.
 */
static bool
CheckConnection(void)
{
	bool		OK;

	OK = ConnectionUp();
	if (!OK)
	{
		if (!pset.cur_cmd_interactive)
		{
			psql_error("connection to server was lost\n");
			exit(EXIT_BADCONN);
		}

		fputs(gettext("The connection to the server was lost. Attempting reset: "), stderr);
		PQreset(pset.db);
		OK = ConnectionUp();
		if (!OK)
		{
			fputs(gettext("Failed.\n"), stderr);
			PQfinish(pset.db);
			pset.db = NULL;
			ResetCancelConn();
			UnsyncVariables();
		}
		else
			fputs(gettext("Succeeded.\n"), stderr);
	}

	return OK;
}



/*
 * SetCancelConn
 *
 * Set cancelConn to point to the current database connection.
 */
static void
SetCancelConn(void)
{
	cancelConn = pset.db;
}


/*
 * ResetCancelConn
 *
 * Set cancelConn to NULL.	I don't know what this means exactly, but it saves
 * having to export the variable.
 */
void
ResetCancelConn(void)
{
	cancelConn = NULL;
}


/*
 * AcceptResult
 *
 * Checks whether a result is valid, giving an error message if necessary;
 * resets cancelConn as needed, and ensures that the connection to the backend
 * is still up.
 *
 * Returns true for valid result, false for error state.
 */
static bool
AcceptResult(const PGresult *result)
{
	bool		OK = true;

	ResetCancelConn();

	if (!result)
		OK = false;
	else
		switch (PQresultStatus(result))
		{
			case PGRES_COMMAND_OK:
			case PGRES_TUPLES_OK:
			case PGRES_EMPTY_QUERY:
			case PGRES_COPY_IN:
				/* Fine, do nothing */
				break;

			case PGRES_COPY_OUT:
				/* keep cancel connection for copy out state */
				SetCancelConn();
				break;

			default:
				OK = false;
				break;
		}

	if (!OK)
	{
		psql_error("%s", PQerrorMessage(pset.db));
		CheckConnection();
	}

	return OK;
}



/*
 * PSQLexec
 *
 * This is the way to send "backdoor" queries (those not directly entered
 * by the user). It is subject to -E but not -e.
 *
 * In autocommit-off mode, a new transaction block is started if start_xact
 * is true; nothing special is done when start_xact is false.  Typically,
 * start_xact = false is used for SELECTs and explicit BEGIN/COMMIT commands.
 *
 * Note: we don't bother to check PQclientEncoding; it is assumed that no
 * caller uses this path to issue "SET CLIENT_ENCODING".
 */
PGresult *
PSQLexec(const char *query, bool start_xact)
{
	PGresult   *res;
	int			echo_hidden;

	if (!pset.db)
	{
		psql_error("You are currently not connected to a database.\n");
		return NULL;
	}

	echo_hidden = SwitchVariable(pset.vars, "ECHO_HIDDEN", "noexec", NULL);
	if (echo_hidden != VAR_NOTSET)
	{
		printf("********* QUERY **********\n"
			   "%s\n"
			   "**************************\n\n", query);
		fflush(stdout);

		if (echo_hidden == 1)	/* noexec? */
			return NULL;
	}

	SetCancelConn();

	if (start_xact && PQtransactionStatus(pset.db) == PQTRANS_IDLE &&
		!GetVariableBool(pset.vars, "AUTOCOMMIT"))
	{
		res = PQexec(pset.db, "BEGIN");
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			psql_error("%s", PQerrorMessage(pset.db));
			PQclear(res);
			ResetCancelConn();
			return NULL;
		}
		PQclear(res);
	}

	res = PQexec(pset.db, query);

	if (!AcceptResult(res) && res)
	{
		PQclear(res);
		res = NULL;
	}

	return res;
}



/*
 * PrintNotifications: check for asynchronous notifications, and print them out
 */
static void
PrintNotifications(void)
{
	PGnotify   *notify;

	while ((notify = PQnotifies(pset.db)))
	{
		fprintf(pset.queryFout, gettext("Asynchronous notification \"%s\" received from server process with PID %d.\n"),
				notify->relname, notify->be_pid);
		fflush(pset.queryFout);
		PQfreemem(notify);
	}
}


/*
 * PrintQueryTuples: assuming query result is OK, print its tuples
 *
 * Returns true if successful, false otherwise.
 */
static bool
PrintQueryTuples(const PGresult *results)
{
	/* write output to \g argument, if any */
	if (pset.gfname)
	{
		FILE	   *queryFout_copy = pset.queryFout;
		bool		queryFoutPipe_copy = pset.queryFoutPipe;

		pset.queryFout = stdout;	/* so it doesn't get closed */

		/* open file/pipe */
		if (!setQFout(pset.gfname))
		{
			pset.queryFout = queryFout_copy;
			pset.queryFoutPipe = queryFoutPipe_copy;
			return false;
		}

		printQuery(results, &pset.popt, pset.queryFout);

		/* close file/pipe, restore old setting */
		setQFout(NULL);

		pset.queryFout = queryFout_copy;
		pset.queryFoutPipe = queryFoutPipe_copy;

		free(pset.gfname);
		pset.gfname = NULL;
	}
	else
		printQuery(results, &pset.popt, pset.queryFout);

	return true;
}


/*
 * ProcessCopyResult: if command was a COPY FROM STDIN/TO STDOUT, handle it
 *
 * Note: Utility function for use by SendQuery() only.
 *
 * Returns true if the query executed successfully, false otherwise.
 */
static bool
ProcessCopyResult(PGresult *results)
{
	bool		success = false;

	if (!results)
		return false;

	switch (PQresultStatus(results))
	{
		case PGRES_TUPLES_OK:
		case PGRES_COMMAND_OK:
		case PGRES_EMPTY_QUERY:
			/* nothing to do here */
			success = true;
			break;

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

		default:
			break;
	}

	/* may need this to recover from conn loss during COPY */
	if (!CheckConnection())
		return false;

	return success;
}


/*
 * PrintQueryResults: print out query results as required
 *
 * Note: Utility function for use by SendQuery() only.
 *
 * Returns true if the query executed successfully, false otherwise.
 */
static bool
PrintQueryResults(PGresult *results)
{
	bool		success = false;

	if (!results)
		return false;

	switch (PQresultStatus(results))
	{
		case PGRES_TUPLES_OK:
			success = PrintQueryTuples(results);
			break;

		case PGRES_COMMAND_OK:
			{
				char		buf[10];

				success = true;
				sprintf(buf, "%u", (unsigned int) PQoidValue(results));
				if (!QUIET())
				{
					if (pset.popt.topt.format == PRINT_HTML)
					{
						fputs("<p>", pset.queryFout);
						html_escaped_print(PQcmdStatus(results),
										   pset.queryFout);
						fputs("</p>\n", pset.queryFout);
					}
					else
						fprintf(pset.queryFout, "%s\n", PQcmdStatus(results));
				}
				SetVariable(pset.vars, "LASTOID", buf);
				break;
			}

		case PGRES_EMPTY_QUERY:
			success = true;
			break;

		case PGRES_COPY_OUT:
		case PGRES_COPY_IN:
			/* nothing to do here */
			success = true;
			break;

		default:
			break;
	}

	fflush(pset.queryFout);

	return success;
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
	PGresult   *results;
	TimevalStruct before,
				after;
	bool		OK;

	if (!pset.db)
	{
		psql_error("You are currently not connected to a database.\n");
		return false;
	}

	if (GetVariableBool(pset.vars, "SINGLESTEP"))
	{
		char		buf[3];

		printf(gettext("***(Single step mode: verify command)*******************************************\n"
					   "%s\n"
					   "***(press return to proceed or enter x and return to cancel)********************\n"),
			   query);
		fflush(stdout);
		if (fgets(buf, sizeof(buf), stdin) != NULL)
			if (buf[0] == 'x')
				return false;
	}
	else if (VariableEquals(pset.vars, "ECHO", "queries"))
	{
		puts(query);
		fflush(stdout);
	}

	SetCancelConn();

	if (PQtransactionStatus(pset.db) == PQTRANS_IDLE &&
		!GetVariableBool(pset.vars, "AUTOCOMMIT") &&
		!is_transact_command(query))
	{
		results = PQexec(pset.db, "BEGIN");
		if (PQresultStatus(results) != PGRES_COMMAND_OK)
		{
			psql_error("%s", PQerrorMessage(pset.db));
			PQclear(results);
			ResetCancelConn();
			return false;
		}
		PQclear(results);
	}

	if (pset.timing)
		GETTIMEOFDAY(&before);

	results = PQexec(pset.db, query);

	/* these operations are included in the timing result: */
	OK = (AcceptResult(results) && ProcessCopyResult(results));

	if (pset.timing)
		GETTIMEOFDAY(&after);

	/* but printing results isn't: */
	if (OK)
		OK = PrintQueryResults(results);

	PQclear(results);

	/* Possible microtiming output */
	if (OK && pset.timing)
		printf(gettext("Time: %.3f ms\n"), DIFF_MSEC(&after, &before));

	/* check for events that may occur during query execution */

	if (pset.encoding != PQclientEncoding(pset.db) &&
		PQclientEncoding(pset.db) >= 0)
	{
		/* track effects of SET CLIENT_ENCODING */
		pset.encoding = PQclientEncoding(pset.db);
		pset.popt.topt.encoding = pset.encoding;
		SetVariable(pset.vars, "ENCODING",
					pg_encoding_to_char(pset.encoding));
	}

	PrintNotifications();

	return OK;
}

/*
 * check whether a query string begins with BEGIN/COMMIT/ROLLBACK/START XACT
 */
static bool
is_transact_command(const char *query)
{
	int			wordlen;

	/*
	 * First we must advance over any whitespace and comments.
	 */
	while (*query)
	{
		if (isspace((unsigned char) *query))
			query++;
		else if (query[0] == '-' && query[1] == '-')
		{
			query += 2;
			while (*query && *query != '\n')
				query++;
		}
		else if (query[0] == '/' && query[1] == '*')
		{
			query += 2;
			while (*query)
			{
				if (query[0] == '*' && query[1] == '/')
				{
					query += 2;
					break;
				}
				else
					query++;
			}
		}
		else
			break;				/* found first token */
	}

	/*
	 * Check word length ("beginx" is not "begin").
	 */
	wordlen = 0;
	while (isalpha((unsigned char) query[wordlen]))
		wordlen++;

	if (wordlen == 5 && strncasecmp(query, "begin", 5) == 0)
		return true;
	if (wordlen == 6 && strncasecmp(query, "commit", 6) == 0)
		return true;
	if (wordlen == 8 && strncasecmp(query, "rollback", 8) == 0)
		return true;
	if (wordlen == 5 && strncasecmp(query, "abort", 5) == 0)
		return true;
	if (wordlen == 3 && strncasecmp(query, "end", 3) == 0)
		return true;
	if (wordlen == 5 && strncasecmp(query, "start", 5) == 0)
		return true;

	return false;
}


char
parse_char(char **buf)
{
	long		l;

	l = strtol(*buf, buf, 0);
	--*buf;
	return (char) l;
}


/*
 * Test if the current user is a database superuser.
 *
 * Note: this will correctly detect superuserness only with a protocol-3.0
 * or newer backend; otherwise it will always say "false".
 */
bool
is_superuser(void)
{
	const char *val;

	if (!pset.db)
		return false;

	val = PQparameterStatus(pset.db, "is_superuser");

	if (val && strcmp(val, "on") == 0)
		return true;

	return false;
}


/*
 * Return the session user of the current connection.
 *
 * Note: this will correctly detect the session user only with a
 * protocol-3.0 or newer backend; otherwise it will return the
 * connection user.
 */
const char *
session_username(void)
{
	const char *val;

	if (!pset.db)
		return NULL;

	val = PQparameterStatus(pset.db, "session_authorization");
	if (val)
		return val;
	else
		return PQuser(pset.db);
}
