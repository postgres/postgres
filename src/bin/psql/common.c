/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2004, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/psql/common.c,v 1.91 2004/09/20 18:51:19 tgl Exp $
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
	 * The returned cursor position is measured in logical characters.
	 * Each character might occupy multiple physical bytes in the string,
	 * and in some Far Eastern character sets it might take more than one
	 * screen column as well.  We compute the starting byte offset and
	 * starting screen column of each logical character, and store these
	 * in qidx[] and scridx[] respectively.
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
		 * want to think about coping with their variable screen width,
		 * but not today.)
		 *
		 * Extract line number and begin and end indexes of line containing
		 * error location.	There will not be any newlines or carriage
		 * returns in the selected extract.
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
						 * count lines before loc.	Each \r or \n counts
						 * as a line except when \r \n appear together.
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
			 * We first truncate right if it is enough.  This code might
			 * be off a space or so on enforcing MIN_RIGHT_CUT if there's
			 * a wide character right there, but that should be okay.
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
		printfPQExpBuffer(&msg, gettext("LINE %d: "), loc_line);
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
		psql_error("%s", PQerrorMessage(pset.db));
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
 * Advance the given char pointer over white space and SQL comments.
 */
static const char *
skip_white_space(const char *query)
{
	int			cnestlevel = 0;		/* slash-star comment nest level */

	while (*query)
	{
		int		mblen = PQmblen(query, pset.encoding);

		/*
		 * Note: we assume the encoding is a superset of ASCII, so that
		 * for example "query[0] == '/'" is meaningful.  However, we do NOT
		 * assume that the second and subsequent bytes of a multibyte
		 * character couldn't look like ASCII characters; so it is critical
		 * to advance by mblen, not 1, whenever we haven't exactly identified
		 * the character we are skipping over.
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
			 * We have to skip to end of line since any slash-star inside
			 * the -- comment does NOT start a slash-star comment.
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
	 * Transaction control commands.  These should include every keyword
	 * that gives rise to a TransactionStmt in the backend grammar, except
	 * for the savepoint-related commands.
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

	/*
	 * Commands not allowed within transactions.  The statements checked
	 * for here should be exactly those that call PreventTransactionChain()
	 * in the backend.
	 *
	 * Note: we are a bit sloppy about CLUSTER, which is transactional in
	 * some variants but not others.
	 */
	if (wordlen == 6 && pg_strncasecmp(query, "vacuum", 6) == 0)
		return true;
	if (wordlen == 7 && pg_strncasecmp(query, "cluster", 7) == 0)
		return true;

	/*
	 * Note: these tests will match REINDEX TABLESPACE, which isn't really
	 * a valid command so we don't care much.  The other five possible
	 * matches are correct.
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
		if (wordlen == 10 && pg_strncasecmp(query, "tablespace", 10) == 0)
			return true;
	}

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

	/* MSDOS uses tilde for short versions of long file names, so skip it. */
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
			get_home_path(home);
		else if ((pw = getpwnam(fn + 1)) != NULL)
			StrNCpy(home, pw->pw_dir, MAXPGPATH);

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
