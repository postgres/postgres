/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2005, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/psql/common.c,v 1.110.2.1 2005/11/22 18:23:27 momjian Exp $
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


static bool command_no_begin(const char *query);

/*
 * "Safe" wrapper around strdup()
 */
char *
pg_strdup(const char *string)
{
	char	   *tmp;

	if (!string)
	{
		fprintf(stderr, _("%s: pg_strdup: cannot duplicate null pointer (internal error)\n"),
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

void *
pg_malloc(size_t size)
{
	void	   *tmp;

	tmp = malloc(size);
	if (!tmp)
	{
		psql_error("out of memory\n");
		exit(EXIT_FAILURE);
	}
	return tmp;
}

void *
pg_malloc_zero(size_t size)
{
	void	   *tmp;

	tmp = pg_malloc(size);
	memset(tmp, 0, size);
	return tmp;
}

void *
pg_calloc(size_t nmemb, size_t size)
{
	void	   *tmp;

	tmp = calloc(nmemb, size);
	if (!tmp)
	{
		psql_error("out of memory");
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
	vfprintf(stderr, _(fmt), ap);
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
 * the signal handler is safe because PQcancel() is written to make it
 * so. We use write() to print to stderr because it's better to use simple
 * facilities in a signal handler.
 *
 * On win32, the signal cancelling happens on a separate thread, because
 * that's how SetConsoleCtrlHandler works. The PQcancel function is safe
 * for this (unlike PQrequestCancel). However, a CRITICAL_SECTION is required
 * to protect the PGcancel structure against being changed while the other
 * thread is using it.
 */
static PGcancel *cancelConn = NULL;

#ifdef WIN32
static CRITICAL_SECTION cancelConnLock;
#endif

volatile bool cancel_pressed = false;

#define write_stderr(str)	write(fileno(stderr), str, strlen(str))


#ifndef WIN32

void
handle_sigint(SIGNAL_ARGS)
{
	int			save_errno = errno;
	char		errbuf[256];

	/* Don't muck around if prompting for a password. */
	if (prompt_state)
		return;

	if (cancelConn == NULL)
		siglongjmp(main_loop_jmp, 1);

	cancel_pressed = true;

	if (PQcancel(cancelConn, errbuf, sizeof(errbuf)))
		write_stderr("Cancel request sent\n");
	else
	{
		write_stderr("Could not send cancel request: ");
		write_stderr(errbuf);
	}
	errno = save_errno;			/* just in case the write changed it */
}
#else							/* WIN32 */

static BOOL WINAPI
consoleHandler(DWORD dwCtrlType)
{
	char		errbuf[256];

	if (dwCtrlType == CTRL_C_EVENT ||
		dwCtrlType == CTRL_BREAK_EVENT)
	{
		if (prompt_state)
			return TRUE;

		/* Perform query cancel */
		EnterCriticalSection(&cancelConnLock);
		if (cancelConn != NULL)
		{
			cancel_pressed = true;

			if (PQcancel(cancelConn, errbuf, sizeof(errbuf)))
				write_stderr("Cancel request sent\n");
			else
			{
				write_stderr("Could not send cancel request: ");
				write_stderr(errbuf);
			}
		}
		LeaveCriticalSection(&cancelConnLock);

		return TRUE;
	}
	else
		/* Return FALSE for any signals not being handled */
		return FALSE;
}

void
setup_win32_locks(void)
{
	InitializeCriticalSection(&cancelConnLock);
}

void
setup_cancel_handler(void)
{
	static bool done = false;

	/* only need one handler per process */
	if (!done)
	{
		SetConsoleCtrlHandler(consoleHandler, TRUE);
		done = true;
	}
}
#endif   /* WIN32 */


/* ConnectionUp
 *
 * Returns whether our backend connection is still there.
 */
static bool
ConnectionUp(void)
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

		fputs(_("The connection to the server was lost. Attempting reset: "), stderr);
		PQreset(pset.db);
		OK = ConnectionUp();
		if (!OK)
		{
			fputs(_("Failed.\n"), stderr);
			PQfinish(pset.db);
			pset.db = NULL;
			ResetCancelConn();
			UnsyncVariables();
		}
		else
			fputs(_("Succeeded.\n"), stderr);
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
#ifdef WIN32
	EnterCriticalSection(&cancelConnLock);
#endif

	/* Free the old one if we have one */
	if (cancelConn != NULL)
		PQfreeCancel(cancelConn);

	cancelConn = PQgetCancel(pset.db);

#ifdef WIN32
	LeaveCriticalSection(&cancelConnLock);
#endif
}


/*
 * ResetCancelConn
 *
 * Free the current cancel connection, if any, and set to NULL.
 */
void
ResetCancelConn(void)
{
#ifdef WIN32
	EnterCriticalSection(&cancelConnLock);
#endif

	if (cancelConn)
		PQfreeCancel(cancelConn);

	cancelConn = NULL;

#ifdef WIN32
	LeaveCriticalSection(&cancelConnLock);
#endif
}


/*
 * on errors, print syntax error position if available.
 *
 * the query is expected to be in the client encoding.
 */
static void
ReportSyntaxErrorPosition(const PGresult *result, const char *query)
{
#define DISPLAY_SIZE	60		/* screen width limit, in screen cols */
#define MIN_RIGHT_CUT	10		/* try to keep this far away from EOL */

	int			loc = 0;
	const char *sp;
	int			clen,
				slen,
				i,
			   *qidx,
			   *scridx,
				qoffset,
				scroffset,
				ibeg,
				iend,
				loc_line;
	char	   *wquery;
	bool		beg_trunc,
				end_trunc;
	PQExpBufferData msg;

	if (pset.verbosity == PQERRORS_TERSE)
		return;

	sp = PQresultErrorField(result, PG_DIAG_STATEMENT_POSITION);
	if (sp == NULL)
	{
		sp = PQresultErrorField(result, PG_DIAG_INTERNAL_POSITION);
		if (sp == NULL)
			return;				/* no syntax error */
		query = PQresultErrorField(result, PG_DIAG_INTERNAL_QUERY);
	}
	if (query == NULL)
		return;					/* nothing to reference location to */

	if (sscanf(sp, "%d", &loc) != 1)
	{
		psql_error("INTERNAL ERROR: unexpected statement position \"%s\"\n",
				   sp);
		return;
	}

	/* Make a writable copy of the query, and a buffer for messages. */
	wquery = pg_strdup(query);

	initPQExpBuffer(&msg);

	/*
	 * The returned cursor position is measured in logical characters. Each
	 * character might occupy multiple physical bytes in the string, and in
	 * some Far Eastern character sets it might take more than one screen
	 * column as well.	We compute the starting byte offset and starting
	 * screen column of each logical character, and store these in qidx[] and
	 * scridx[] respectively.
	 */

	/* we need a safe allocation size... */
	slen = strlen(query) + 1;

	qidx = (int *) pg_malloc(slen * sizeof(int));
	scridx = (int *) pg_malloc(slen * sizeof(int));

	qoffset = 0;
	scroffset = 0;
	for (i = 0; query[qoffset] != '\0'; i++)
	{
		qidx[i] = qoffset;
		scridx[i] = scroffset;
		scroffset += PQdsplen(&query[qoffset], pset.encoding);
		qoffset += PQmblen(&query[qoffset], pset.encoding);
	}
	qidx[i] = qoffset;
	scridx[i] = scroffset;
	clen = i;
	psql_assert(clen < slen);

	/* convert loc to zero-based offset in qidx/scridx arrays */
	loc--;

	/* do we have something to show? */
	if (loc >= 0 && loc <= clen)
	{
		/* input line number of our syntax error. */
		loc_line = 1;
		/* first included char of extract. */
		ibeg = 0;
		/* last-plus-1 included char of extract. */
		iend = clen;

		/*
		 * Replace tabs with spaces in the writable copy.  (Later we might
		 * want to think about coping with their variable screen width, but
		 * not today.)
		 *
		 * Extract line number and begin and end indexes of line containing
		 * error location.	There will not be any newlines or carriage returns
		 * in the selected extract.
		 */
		for (i = 0; i < clen; i++)
		{
			/* character length must be 1 or it's not ASCII */
			if ((qidx[i + 1] - qidx[i]) == 1)
			{
				if (wquery[qidx[i]] == '\t')
					wquery[qidx[i]] = ' ';
				else if (wquery[qidx[i]] == '\r' || wquery[qidx[i]] == '\n')
				{
					if (i < loc)
					{
						/*
						 * count lines before loc.	Each \r or \n counts as a
						 * line except when \r \n appear together.
						 */
						if (wquery[qidx[i]] == '\r' ||
							i == 0 ||
							(qidx[i] - qidx[i - 1]) != 1 ||
							wquery[qidx[i - 1]] != '\r')
							loc_line++;
						/* extract beginning = last line start before loc. */
						ibeg = i + 1;
					}
					else
					{
						/* set extract end. */
						iend = i;
						/* done scanning. */
						break;
					}
				}
			}
		}

		/* If the line extracted is too long, we truncate it. */
		beg_trunc = false;
		end_trunc = false;
		if (scridx[iend] - scridx[ibeg] > DISPLAY_SIZE)
		{
			/*
			 * We first truncate right if it is enough.  This code might be
			 * off a space or so on enforcing MIN_RIGHT_CUT if there's a wide
			 * character right there, but that should be okay.
			 */
			if (scridx[ibeg] + DISPLAY_SIZE >= scridx[loc] + MIN_RIGHT_CUT)
			{
				while (scridx[iend] - scridx[ibeg] > DISPLAY_SIZE)
					iend--;
				end_trunc = true;
			}
			else
			{
				/* Truncate right if not too close to loc. */
				while (scridx[loc] + MIN_RIGHT_CUT < scridx[iend])
				{
					iend--;
					end_trunc = true;
				}

				/* Truncate left if still too long. */
				while (scridx[iend] - scridx[ibeg] > DISPLAY_SIZE)
				{
					ibeg++;
					beg_trunc = true;
				}
			}
		}

		/* the extract MUST contain the target position! */
		psql_assert(ibeg <= loc && loc <= iend);

		/* truncate working copy at desired endpoint */
		wquery[qidx[iend]] = '\0';

		/* Begin building the finished message. */
		printfPQExpBuffer(&msg, _("LINE %d: "), loc_line);
		if (beg_trunc)
			appendPQExpBufferStr(&msg, "...");

		/*
		 * While we have the prefix in the msg buffer, compute its screen
		 * width.
		 */
		scroffset = 0;
		for (i = 0; i < msg.len; i += PQmblen(&msg.data[i], pset.encoding))
			scroffset += PQdsplen(&msg.data[i], pset.encoding);

		/* Finish and emit the message. */
		appendPQExpBufferStr(&msg, &wquery[qidx[ibeg]]);
		if (end_trunc)
			appendPQExpBufferStr(&msg, "...");

		psql_error("%s\n", msg.data);

		/* Now emit the cursor marker line. */
		scroffset += scridx[loc] - scridx[ibeg];
		resetPQExpBuffer(&msg);
		for (i = 0; i < scroffset; i++)
			appendPQExpBufferChar(&msg, ' ');
		appendPQExpBufferChar(&msg, '^');

		psql_error("%s\n", msg.data);
	}

	/* Clean up. */
	termPQExpBuffer(&msg);

	free(wquery);
	free(qidx);
	free(scridx);
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
AcceptResult(const PGresult *result, const char *query)
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
		const char *error = PQerrorMessage(pset.db);

		if (strlen(error))
			psql_error("%s", error);

		ReportSyntaxErrorPosition(result, query);
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
		printf(_("********* QUERY **********\n"
				 "%s\n"
				 "**************************\n\n"), query);
		fflush(stdout);
		if (pset.logfile)
		{
			fprintf(pset.logfile,
					_("********* QUERY **********\n"
					  "%s\n"
					  "**************************\n\n"), query);
			fflush(pset.logfile);
		}

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

	if (!AcceptResult(res, query) && res)
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
		fprintf(pset.queryFout, _("Asynchronous notification \"%s\" received from server process with PID %d.\n"),
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
	printQueryOpt my_popt = pset.popt;

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

		printQuery(results, &my_popt, pset.queryFout, pset.logfile);

		/* close file/pipe, restore old setting */
		setQFout(NULL);

		pset.queryFout = queryFout_copy;
		pset.queryFoutPipe = queryFoutPipe_copy;

		free(pset.gfname);
		pset.gfname = NULL;
	}
	else
		printQuery(results, &my_popt, pset.queryFout, pset.logfile);

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
			success = handleCopyIn(pset.db, pset.cur_cmd_source);
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
				snprintf(buf, sizeof(buf),
						 "%u", (unsigned int) PQoidValue(results));
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
				if (pset.logfile)
					fprintf(pset.logfile, "%s\n", PQcmdStatus(results));
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
	bool		OK,
				on_error_rollback_savepoint = false;
	PGTransactionStatusType transaction_status;
	static bool on_error_rollback_warning = false;
	const char *rollback_str;

	if (!pset.db)
	{
		psql_error("You are currently not connected to a database.\n");
		return false;
	}

	if (GetVariableBool(pset.vars, "SINGLESTEP"))
	{
		char		buf[3];

		printf(_("***(Single step mode: verify command)*******************************************\n"
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

	if (pset.logfile)
	{
		fprintf(pset.logfile,
				_("********* QUERY **********\n"
				  "%s\n"
				  "**************************\n\n"), query);
		fflush(pset.logfile);
	}

	SetCancelConn();

	transaction_status = PQtransactionStatus(pset.db);

	if (transaction_status == PQTRANS_IDLE &&
		!GetVariableBool(pset.vars, "AUTOCOMMIT") &&
		!command_no_begin(query))
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
		transaction_status = PQtransactionStatus(pset.db);
	}

	if (transaction_status == PQTRANS_INTRANS &&
	  (rollback_str = GetVariable(pset.vars, "ON_ERROR_ROLLBACK")) != NULL &&
	/* !off and !interactive is 'on' */
		pg_strcasecmp(rollback_str, "off") != 0 &&
		(pset.cur_cmd_interactive ||
		 pg_strcasecmp(rollback_str, "interactive") != 0))
	{
		if (on_error_rollback_warning == false && pset.sversion < 80000)
		{
			fprintf(stderr, _("The server version (%d) does not support savepoints for ON_ERROR_ROLLBACK.\n"),
					pset.sversion);
			on_error_rollback_warning = true;
		}
		else
		{
			results = PQexec(pset.db, "SAVEPOINT pg_psql_temporary_savepoint");
			if (PQresultStatus(results) != PGRES_COMMAND_OK)
			{
				psql_error("%s", PQerrorMessage(pset.db));
				PQclear(results);
				ResetCancelConn();
				return false;
			}
			PQclear(results);
			on_error_rollback_savepoint = true;
		}
	}

	if (pset.timing)
		GETTIMEOFDAY(&before);

	results = PQexec(pset.db, query);

	/* these operations are included in the timing result: */
	OK = (AcceptResult(results, query) && ProcessCopyResult(results));

	if (pset.timing)
		GETTIMEOFDAY(&after);

	/* but printing results isn't: */
	if (OK)
		OK = PrintQueryResults(results);

	PQclear(results);

	/* If we made a temporary savepoint, possibly release/rollback */
	if (on_error_rollback_savepoint)
	{
		transaction_status = PQtransactionStatus(pset.db);

		/* We always rollback on an error */
		if (transaction_status == PQTRANS_INERROR)
			results = PQexec(pset.db, "ROLLBACK TO pg_psql_temporary_savepoint");
		/* If they are no longer in a transaction, then do nothing */
		else if (transaction_status != PQTRANS_INTRANS)
			results = NULL;
		else
		{
			/*
			 * Do nothing if they are messing with savepoints themselves: If
			 * the user did RELEASE or ROLLBACK, our savepoint is gone. If
			 * they issued a SAVEPOINT, releasing ours would remove theirs.
			 */
			if (strcmp(PQcmdStatus(results), "SAVEPOINT") == 0 ||
				strcmp(PQcmdStatus(results), "RELEASE") == 0 ||
				strcmp(PQcmdStatus(results), "ROLLBACK") == 0)
				results = NULL;
			else
				results = PQexec(pset.db, "RELEASE pg_psql_temporary_savepoint");
		}
		if (PQresultStatus(results) != PGRES_COMMAND_OK)
		{
			psql_error("%s", PQerrorMessage(pset.db));
			PQclear(results);
			ResetCancelConn();
			return false;
		}
		PQclear(results);
	}

	/* Possible microtiming output */
	if (OK && pset.timing)
		printf(_("Time: %.3f ms\n"), DIFF_MSEC(&after, &before));

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
 * Advance the given char pointer over white space and SQL comments.
 */
static const char *
skip_white_space(const char *query)
{
	int			cnestlevel = 0; /* slash-star comment nest level */

	while (*query)
	{
		int			mblen = PQmblen(query, pset.encoding);

		/*
		 * Note: we assume the encoding is a superset of ASCII, so that for
		 * example "query[0] == '/'" is meaningful.  However, we do NOT assume
		 * that the second and subsequent bytes of a multibyte character
		 * couldn't look like ASCII characters; so it is critical to advance
		 * by mblen, not 1, whenever we haven't exactly identified the
		 * character we are skipping over.
		 */
		if (isspace((unsigned char) *query))
			query += mblen;
		else if (query[0] == '/' && query[1] == '*')
		{
			cnestlevel++;
			query += 2;
		}
		else if (cnestlevel > 0 && query[0] == '*' && query[1] == '/')
		{
			cnestlevel--;
			query += 2;
		}
		else if (cnestlevel == 0 && query[0] == '-' && query[1] == '-')
		{
			query += 2;

			/*
			 * We have to skip to end of line since any slash-star inside the
			 * -- comment does NOT start a slash-star comment.
			 */
			while (*query)
			{
				if (*query == '\n')
				{
					query++;
					break;
				}
				query += PQmblen(query, pset.encoding);
			}
		}
		else if (cnestlevel > 0)
			query += mblen;
		else
			break;				/* found first token */
	}

	return query;
}


/*
 * Check whether a command is one of those for which we should NOT start
 * a new transaction block (ie, send a preceding BEGIN).
 *
 * These include the transaction control statements themselves, plus
 * certain statements that the backend disallows inside transaction blocks.
 */
static bool
command_no_begin(const char *query)
{
	int			wordlen;

	/*
	 * First we must advance over any whitespace and comments.
	 */
	query = skip_white_space(query);

	/*
	 * Check word length (since "beginx" is not "begin").
	 */
	wordlen = 0;
	while (isalpha((unsigned char) query[wordlen]))
		wordlen += PQmblen(&query[wordlen], pset.encoding);

	/*
	 * Transaction control commands.  These should include every keyword that
	 * gives rise to a TransactionStmt in the backend grammar, except for the
	 * savepoint-related commands.
	 *
	 * (We assume that START must be START TRANSACTION, since there is
	 * presently no other "START foo" command.)
	 */
	if (wordlen == 5 && pg_strncasecmp(query, "abort", 5) == 0)
		return true;
	if (wordlen == 5 && pg_strncasecmp(query, "begin", 5) == 0)
		return true;
	if (wordlen == 5 && pg_strncasecmp(query, "start", 5) == 0)
		return true;
	if (wordlen == 6 && pg_strncasecmp(query, "commit", 6) == 0)
		return true;
	if (wordlen == 3 && pg_strncasecmp(query, "end", 3) == 0)
		return true;
	if (wordlen == 8 && pg_strncasecmp(query, "rollback", 8) == 0)
		return true;
	if (wordlen == 7 && pg_strncasecmp(query, "prepare", 7) == 0)
	{
		/* PREPARE TRANSACTION is a TC command, PREPARE foo is not */
		query += wordlen;

		query = skip_white_space(query);

		wordlen = 0;
		while (isalpha((unsigned char) query[wordlen]))
			wordlen += PQmblen(&query[wordlen], pset.encoding);

		if (wordlen == 11 && pg_strncasecmp(query, "transaction", 11) == 0)
			return true;
		return false;
	}

	/*
	 * Commands not allowed within transactions.  The statements checked for
	 * here should be exactly those that call PreventTransactionChain() in the
	 * backend.
	 *
	 * Note: we are a bit sloppy about CLUSTER, which is transactional in some
	 * variants but not others.
	 */
	if (wordlen == 6 && pg_strncasecmp(query, "vacuum", 6) == 0)
		return true;
	if (wordlen == 7 && pg_strncasecmp(query, "cluster", 7) == 0)
		return true;

	/*
	 * Note: these tests will match CREATE SYSTEM, DROP SYSTEM, and REINDEX
	 * TABLESPACE, which aren't really valid commands so we don't care much.
	 * The other six possible matches are correct.
	 */
	if ((wordlen == 6 && pg_strncasecmp(query, "create", 6) == 0) ||
		(wordlen == 4 && pg_strncasecmp(query, "drop", 4) == 0) ||
		(wordlen == 7 && pg_strncasecmp(query, "reindex", 7) == 0))
	{
		query += wordlen;

		query = skip_white_space(query);

		wordlen = 0;
		while (isalpha((unsigned char) query[wordlen]))
			wordlen += PQmblen(&query[wordlen], pset.encoding);

		if (wordlen == 8 && pg_strncasecmp(query, "database", 8) == 0)
			return true;
		if (wordlen == 6 && pg_strncasecmp(query, "system", 6) == 0)
			return true;
		if (wordlen == 10 && pg_strncasecmp(query, "tablespace", 10) == 0)
			return true;
	}

	return false;
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


/* expand_tilde
 *
 * substitute '~' with HOME or '~username' with username's home dir
 *
 */
char *
expand_tilde(char **filename)
{
	if (!filename || !(*filename))
		return NULL;

	/*
	 * WIN32 doesn't use tilde expansion for file names. Also, it uses tilde
	 * for short versions of long file names, though the tilde is usually
	 * toward the end, not at the beginning.
	 */
#ifndef WIN32

	/* try tilde expansion */
	if (**filename == '~')
	{
		char	   *fn;
		char		oldp,
				   *p;
		struct passwd *pw;
		char		home[MAXPGPATH];

		fn = *filename;
		*home = '\0';

		p = fn + 1;
		while (*p != '/' && *p != '\0')
			p++;

		oldp = *p;
		*p = '\0';

		if (*(fn + 1) == '\0')
			get_home_path(home);	/* ~ or ~/ only */
		else if ((pw = getpwnam(fn + 1)) != NULL)
			StrNCpy(home, pw->pw_dir, MAXPGPATH);		/* ~user */

		*p = oldp;
		if (strlen(home) != 0)
		{
			char	   *newfn;

			newfn = pg_malloc(strlen(home) + strlen(p) + 1);
			strcpy(newfn, home);
			strcat(newfn, p);

			free(fn);
			*filename = newfn;
		}
	}
#endif

	return *filename;
}
