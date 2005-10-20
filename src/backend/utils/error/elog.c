/*-------------------------------------------------------------------------
 *
 * elog.c
 *	  error logging and reporting
 *
 * Some notes about recursion and errors during error processing:
 *
 * We need to be robust about recursive-error scenarios --- for example,
 * if we run out of memory, it's important to be able to report that fact.
 * There are a number of considerations that go into this.
 *
 * First, distinguish between re-entrant use and actual recursion.	It
 * is possible for an error or warning message to be emitted while the
 * parameters for an error message are being computed.	In this case
 * errstart has been called for the outer message, and some field values
 * may have already been saved, but we are not actually recursing.	We handle
 * this by providing a (small) stack of ErrorData records.	The inner message
 * can be computed and sent without disturbing the state of the outer message.
 * (If the inner message is actually an error, this isn't very interesting
 * because control won't come back to the outer message generator ... but
 * if the inner message is only debug or log data, this is critical.)
 *
 * Second, actual recursion will occur if an error is reported by one of
 * the elog.c routines or something they call.	By far the most probable
 * scenario of this sort is "out of memory"; and it's also the nastiest
 * to handle because we'd likely also run out of memory while trying to
 * report this error!  Our escape hatch for this condition is to force any
 * such messages up to ERROR level if they aren't already (so that we will
 * not need to return to the outer elog.c call), and to reset the ErrorContext
 * to empty before trying to process the inner message.  Since ErrorContext
 * is guaranteed to have at least 8K of space in it (see mcxt.c), we should
 * be able to process an "out of memory" message successfully.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/error/elog.c,v 1.125.2.2 2005/10/20 01:31:50 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#include <ctype.h>
#ifdef HAVE_SYSLOG
#include <syslog.h>
#endif

#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "tcop/tcopprot.h"
#include "utils/memutils.h"
#include "utils/guc.h"


/* Global variables */
ErrorContextCallback *error_context_stack = NULL;

/* GUC parameters */
PGErrorVerbosity Log_error_verbosity = PGERROR_VERBOSE;
bool		Log_timestamp = false;		/* show timestamps in stderr
										 * output */
bool		Log_pid = false;	/* show PIDs in stderr output */

#ifdef HAVE_SYSLOG
/*
 * 0 = only stdout/stderr
 * 1 = stdout+stderr and syslog
 * 2 = syslog only
 * ... in theory anyway
 */
int			Use_syslog = 0;
char	   *Syslog_facility;	/* openlog() parameters */
char	   *Syslog_ident;

static void write_syslog(int level, const char *line);

#else

#define Use_syslog 0
#endif   /* HAVE_SYSLOG */


/*
 * ErrorData holds the data accumulated during any one ereport() cycle.
 * Any non-NULL pointers must point to palloc'd data in ErrorContext.
 * (The const pointers are an exception; we assume they point at non-freeable
 * constant strings.)
 */

typedef struct ErrorData
{
	int			elevel;			/* error level */
	bool		output_to_server;		/* will report to server log? */
	bool		output_to_client;		/* will report to client? */
	bool		show_funcname;	/* true to force funcname inclusion */
	const char *filename;		/* __FILE__ of ereport() call */
	int			lineno;			/* __LINE__ of ereport() call */
	const char *funcname;		/* __func__ of ereport() call */
	int			sqlerrcode;		/* encoded ERRSTATE */
	char	   *message;		/* primary error message */
	char	   *detail;			/* detail error message */
	char	   *hint;			/* hint message */
	char	   *context;		/* context message */
	int			cursorpos;		/* cursor index into query string */
	int			saved_errno;	/* errno at entry */
} ErrorData;

/* We provide a small stack of ErrorData records for re-entrant cases */
#define ERRORDATA_STACK_SIZE  5

static ErrorData errordata[ERRORDATA_STACK_SIZE];

static int	errordata_stack_depth = -1; /* index of topmost active frame */

static int	recursion_depth = 0;	/* to detect actual recursion */


/* Macro for checking errordata_stack_depth is reasonable */
#define CHECK_STACK_DEPTH() \
	do { \
		if (errordata_stack_depth < 0) \
		{ \
			errordata_stack_depth = -1; \
			ereport(ERROR, (errmsg_internal("errstart was not called"))); \
		} \
	} while (0)


static void send_message_to_server_log(ErrorData *edata);
static void send_message_to_frontend(ErrorData *edata);
static char *expand_fmt_string(const char *fmt, ErrorData *edata);
static const char *useful_strerror(int errnum);
static const char *error_severity(int elevel);
static const char *print_timestamp(void);
static const char *print_pid(void);
static void append_with_tabs(StringInfo buf, const char *str);


/*
 * errstart --- begin an error-reporting cycle
 *
 * Create a stack entry and store the given parameters in it.  Subsequently,
 * errmsg() and perhaps other routines will be called to further populate
 * the stack entry.  Finally, errfinish() will be called to actually process
 * the error report.
 *
 * Returns TRUE in normal case.  Returns FALSE to short-circuit the error
 * report (if it's a warning or lower and not to be reported anywhere).
 */
bool
errstart(int elevel, const char *filename, int lineno,
		 const char *funcname)
{
	ErrorData  *edata;
	bool		output_to_server = false;
	bool		output_to_client = false;

	/*
	 * First decide whether we need to process this report at all; if it's
	 * warning or less and not enabled for logging, just return FALSE
	 * without starting up any error logging machinery.
	 */

	/*
	 * Convert initialization errors into fatal errors. This is probably
	 * redundant, because Warn_restart_ready won't be set anyway.
	 */
	if (elevel == ERROR && IsInitProcessingMode())
		elevel = FATAL;

	/*
	 * If we are inside a critical section, all errors become PANIC
	 * errors.	See miscadmin.h.
	 */
	if (elevel >= ERROR)
	{
		if (CritSectionCount > 0)
			elevel = PANIC;
	}

	/* Determine whether message is enabled for server log output */
	if (IsPostmasterEnvironment)
	{
		/* Complicated because LOG is sorted out-of-order for this purpose */
		if (elevel == LOG || elevel == COMMERROR)
		{
			if (log_min_messages == LOG)
				output_to_server = true;
			else if (log_min_messages < FATAL)
				output_to_server = true;
		}
		else
		{
			/* elevel != LOG */
			if (log_min_messages == LOG)
			{
				if (elevel >= FATAL)
					output_to_server = true;
			}
			/* Neither is LOG */
			else if (elevel >= log_min_messages)
				output_to_server = true;
		}
	}
	else
	{
		/* In bootstrap/standalone case, do not sort LOG out-of-order */
		output_to_server = (elevel >= log_min_messages);
	}

	/* Determine whether message is enabled for client output */
	if (whereToSendOutput == Remote && elevel != COMMERROR)
	{
		/*
		 * client_min_messages is honored only after we complete the
		 * authentication handshake.  This is required both for security
		 * reasons and because many clients can't handle NOTICE messages
		 * during authentication.
		 */
		if (ClientAuthInProgress)
			output_to_client = (elevel >= ERROR);
		else
			output_to_client = (elevel >= client_min_messages ||
								elevel == INFO);
	}

	/* Skip processing effort if non-error message will not be output */
	if (elevel < ERROR && !output_to_server && !output_to_client)
		return false;

	/*
	 * Okay, crank up a stack entry to store the info in.
	 */

	if (recursion_depth++ > 0 && elevel >= ERROR)
	{
		/*
		 * Ooops, error during error processing.  Clear ErrorContext as
		 * discussed at top of file.  We will not return to the original
		 * error's reporter or handler, so we don't need it.
		 */
		MemoryContextReset(ErrorContext);

		/*
		 * If we recurse more than once, the problem might be something
		 * broken in a context traceback routine.  Abandon them too.
		 */
		if (recursion_depth > 2)
			error_context_stack = NULL;
	}
	if (++errordata_stack_depth >= ERRORDATA_STACK_SIZE)
	{
		/* Wups, stack not big enough */
		int			i;

		elevel = Max(elevel, ERROR);

		/*
		 * Don't forget any FATAL/PANIC status on the stack (see comments
		 * in errfinish)
		 */
		for (i = 0; i < errordata_stack_depth; i++)
			elevel = Max(elevel, errordata[i].elevel);
		/* Clear the stack and try again */
		errordata_stack_depth = -1;
		ereport(elevel, (errmsg_internal("ERRORDATA_STACK_SIZE exceeded")));
	}

	/* Initialize data for this error frame */
	edata = &errordata[errordata_stack_depth];
	MemSet(edata, 0, sizeof(ErrorData));
	edata->elevel = elevel;
	edata->output_to_server = output_to_server;
	edata->output_to_client = output_to_client;
	edata->filename = filename;
	edata->lineno = lineno;
	edata->funcname = funcname;
	/* Select default errcode based on elevel */
	if (elevel >= ERROR)
		edata->sqlerrcode = ERRCODE_INTERNAL_ERROR;
	else if (elevel == WARNING)
		edata->sqlerrcode = ERRCODE_WARNING;
	else
		edata->sqlerrcode = ERRCODE_SUCCESSFUL_COMPLETION;
	/* errno is saved here so that error parameter eval can't change it */
	edata->saved_errno = errno;

	recursion_depth--;
	return true;
}

/*
 * errfinish --- end an error-reporting cycle
 *
 * Produce the appropriate error report(s) and pop the error stack.
 *
 * If elevel is ERROR or worse, control does not return to the caller.
 * See elog.h for the error level definitions.
 */
void
errfinish(int dummy,...)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];
	int			elevel = edata->elevel;
	MemoryContext oldcontext;
	ErrorContextCallback *econtext;

	recursion_depth++;
	CHECK_STACK_DEPTH();

	/*
	 * Do processing in ErrorContext, which we hope has enough reserved
	 * space to report an error.
	 */
	oldcontext = MemoryContextSwitchTo(ErrorContext);

	/*
	 * Call any context callback functions.  Errors occurring in callback
	 * functions will be treated as recursive errors --- this ensures we
	 * will avoid infinite recursion (see errstart).
	 */
	for (econtext = error_context_stack;
		 econtext != NULL;
		 econtext = econtext->previous)
		(*econtext->callback) (econtext->arg);

	/* Send to server log, if enabled */
	if (edata->output_to_server)
		send_message_to_server_log(edata);

	/*
	 * Abort any old-style COPY OUT in progress when an error is detected.
	 * This hack is necessary because of poor design of old-style copy
	 * protocol.  Note we must do this even if client is fool enough to
	 * have set client_min_messages above ERROR, so don't look at
	 * output_to_client.
	 */
	if (elevel >= ERROR && whereToSendOutput == Remote)
		pq_endcopyout(true);

	/* Send to client, if enabled */
	if (edata->output_to_client)
		send_message_to_frontend(edata);

	/* Now free up subsidiary data attached to stack entry, and release it */
	if (edata->message)
		pfree(edata->message);
	if (edata->detail)
		pfree(edata->detail);
	if (edata->hint)
		pfree(edata->hint);
	if (edata->context)
		pfree(edata->context);

	MemoryContextSwitchTo(oldcontext);

	errordata_stack_depth--;
	recursion_depth--;

	/*
	 * If the error level is ERROR or more, we are not going to return to
	 * caller; therefore, if there is any stacked error already in
	 * progress it will be lost.  This is more or less okay, except we do
	 * not want to have a FATAL or PANIC error downgraded because the
	 * reporting process was interrupted by a lower-grade error.  So check
	 * the stack and make sure we panic if panic is warranted.
	 */
	if (elevel >= ERROR)
	{
		int			i;

		for (i = 0; i <= errordata_stack_depth; i++)
			elevel = Max(elevel, errordata[i].elevel);

		/*
		 * Also, be sure to reset the stack to empty.  We do not clear
		 * ErrorContext here, though; PostgresMain does that later on.
		 */
		errordata_stack_depth = -1;
		recursion_depth = 0;
		error_context_stack = NULL;
	}

	/*
	 * Perform error recovery action as specified by elevel.
	 */
	if (elevel == ERROR || elevel == FATAL)
	{
		/* Prevent immediate interrupt while entering error recovery */
		ImmediateInterruptOK = false;

		/*
		 * If we just reported a startup failure, the client will
		 * disconnect on receiving it, so don't send any more to the
		 * client.
		 */
		if (!Warn_restart_ready && whereToSendOutput == Remote)
			whereToSendOutput = None;

		/*
		 * For a FATAL error, we let proc_exit clean up and exit.
		 *
		 * There are several other cases in which we treat ERROR as FATAL and
		 * go directly to proc_exit:
		 *
		 * 1. ExitOnAnyError mode switch is set (initdb uses this).
		 *
		 * 2. we have not yet entered the main backend loop (ie, we are in
		 * the postmaster or in backend startup); we have noplace to
		 * recover.
		 *
		 * 3. the error occurred after proc_exit has begun to run.	(It's
		 * proc_exit's responsibility to see that this doesn't turn into
		 * infinite recursion!)
		 *
		 * In the last case, we exit with nonzero exit code to indicate that
		 * something's pretty wrong.  We also want to exit with nonzero
		 * exit code if not running under the postmaster (for example, if
		 * we are being run from the initdb script, we'd better return an
		 * error status).
		 */
		if (elevel == FATAL ||
			ExitOnAnyError ||
			!Warn_restart_ready ||
			proc_exit_inprogress)
		{
			/*
			 * fflush here is just to improve the odds that we get to see
			 * the error message, in case things are so hosed that
			 * proc_exit crashes.  Any other code you might be tempted to
			 * add here should probably be in an on_proc_exit callback
			 * instead.
			 */
			fflush(stdout);
			fflush(stderr);
			proc_exit(proc_exit_inprogress || !IsUnderPostmaster);
		}

		/*
		 * Guard against infinite loop from errors during error recovery.
		 */
		if (InError)
			ereport(PANIC, (errmsg("error during error recovery, giving up")));
		InError = true;

		/*
		 * Otherwise we can return to the main loop in postgres.c.
		 */
		siglongjmp(Warn_restart, 1);
	}

	if (elevel >= PANIC)
	{
		/*
		 * Serious crash time. Postmaster will observe nonzero process
		 * exit status and kill the other backends too.
		 *
		 * XXX: what if we are *in* the postmaster?  abort() won't kill our
		 * children...
		 */
		ImmediateInterruptOK = false;
		fflush(stdout);
		fflush(stderr);
		abort();
	}

	/* We reach here if elevel <= WARNING. OK to return to caller. */
}


/*
 * errcode --- add SQLSTATE error code to the current error
 *
 * The code is expected to be represented as per MAKE_SQLSTATE().
 */
int
errcode(int sqlerrcode)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];

	/* we don't bother incrementing recursion_depth */
	CHECK_STACK_DEPTH();

	edata->sqlerrcode = sqlerrcode;

	return 0;					/* return value does not matter */
}


/*
 * errcode_for_file_access --- add SQLSTATE error code to the current error
 *
 * The SQLSTATE code is chosen based on the saved errno value.	We assume
 * that the failing operation was some type of disk file access.
 *
 * NOTE: the primary error message string should generally include %m
 * when this is used.
 */
int
errcode_for_file_access(void)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];

	/* we don't bother incrementing recursion_depth */
	CHECK_STACK_DEPTH();

	switch (edata->saved_errno)
	{
			/* Permission-denied failures */
		case EPERM:				/* Not super-user */
		case EACCES:			/* Permission denied */
#ifdef EROFS
		case EROFS:				/* Read only file system */
#endif
			edata->sqlerrcode = ERRCODE_INSUFFICIENT_PRIVILEGE;
			break;

			/* File not found */
		case ENOENT:			/* No such file or directory */
			edata->sqlerrcode = ERRCODE_UNDEFINED_FILE;
			break;

			/* Duplicate file */
		case EEXIST:			/* File exists */
			edata->sqlerrcode = ERRCODE_DUPLICATE_FILE;
			break;

			/* Wrong object type or state */
		case ENOTDIR:			/* Not a directory */
		case EISDIR:			/* Is a directory */
#if defined(ENOTEMPTY) && (ENOTEMPTY != EEXIST)	/* same code on AIX */
		case ENOTEMPTY:			/* Directory not empty */
#endif
			edata->sqlerrcode = ERRCODE_WRONG_OBJECT_TYPE;
			break;

			/* Insufficient resources */
		case ENOSPC:			/* No space left on device */
			edata->sqlerrcode = ERRCODE_DISK_FULL;
			break;

		case ENFILE:			/* File table overflow */
		case EMFILE:			/* Too many open files */
			edata->sqlerrcode = ERRCODE_INSUFFICIENT_RESOURCES;
			break;

			/* Hardware failure */
		case EIO:				/* I/O error */
			edata->sqlerrcode = ERRCODE_IO_ERROR;
			break;

			/* All else is classified as internal errors */
		default:
			edata->sqlerrcode = ERRCODE_INTERNAL_ERROR;
			break;
	}

	return 0;					/* return value does not matter */
}

/*
 * errcode_for_socket_access --- add SQLSTATE error code to the current error
 *
 * The SQLSTATE code is chosen based on the saved errno value.	We assume
 * that the failing operation was some type of socket access.
 *
 * NOTE: the primary error message string should generally include %m
 * when this is used.
 */
int
errcode_for_socket_access(void)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];

	/* we don't bother incrementing recursion_depth */
	CHECK_STACK_DEPTH();

	switch (edata->saved_errno)
	{
			/* Loss of connection */
		case EPIPE:
#ifdef ECONNRESET
		case ECONNRESET:
#endif
			edata->sqlerrcode = ERRCODE_CONNECTION_FAILURE;
			break;

			/* All else is classified as internal errors */
		default:
			edata->sqlerrcode = ERRCODE_INTERNAL_ERROR;
			break;
	}

	return 0;					/* return value does not matter */
}


/*
 * This macro handles expansion of a format string and associated parameters;
 * it's common code for errmsg(), errdetail(), etc.  Must be called inside
 * a routine that is declared like "const char *fmt, ..." and has an edata
 * pointer set up.	The message is assigned to edata->targetfield, or
 * appended to it if appendval is true.
 *
 * Note: we pstrdup the buffer rather than just transferring its storage
 * to the edata field because the buffer might be considerably larger than
 * really necessary.
 */
#define EVALUATE_MESSAGE(targetfield, appendval)  \
	{ \
		char		   *fmtbuf; \
		StringInfoData	buf; \
		/* Internationalize the error format string */ \
		fmt = gettext(fmt); \
		/* Expand %m in format string */ \
		fmtbuf = expand_fmt_string(fmt, edata); \
		initStringInfo(&buf); \
		if ((appendval) && edata->targetfield) \
			appendStringInfo(&buf, "%s\n", edata->targetfield); \
		/* Generate actual output --- have to use appendStringInfoVA */ \
		for (;;) \
		{ \
			va_list		args; \
			bool		success; \
			va_start(args, fmt); \
			success = appendStringInfoVA(&buf, fmtbuf, args); \
			va_end(args); \
			if (success) \
				break; \
			enlargeStringInfo(&buf, buf.maxlen); \
		} \
		/* Done with expanded fmt */ \
		pfree(fmtbuf); \
		/* Save the completed message into the stack item */ \
		if (edata->targetfield) \
			pfree(edata->targetfield); \
		edata->targetfield = pstrdup(buf.data); \
		pfree(buf.data); \
	}


/*
 * errmsg --- add a primary error message text to the current error
 *
 * In addition to the usual %-escapes recognized by printf, "%m" in
 * fmt is replaced by the error message for the caller's value of errno.
 *
 * Note: no newline is needed at the end of the fmt string, since
 * ereport will provide one for the output methods that need it.
 */
int
errmsg(const char *fmt,...)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];
	MemoryContext oldcontext;

	recursion_depth++;
	CHECK_STACK_DEPTH();
	oldcontext = MemoryContextSwitchTo(ErrorContext);

	EVALUATE_MESSAGE(message, false);

	MemoryContextSwitchTo(oldcontext);
	recursion_depth--;
	return 0;					/* return value does not matter */
}


/*
 * errmsg_internal --- add a primary error message text to the current error
 *
 * This is exactly like errmsg() except that strings passed to errmsg_internal
 * are customarily left out of the internationalization message dictionary.
 * This should be used for "can't happen" cases that are probably not worth
 * spending translation effort on.
 */
int
errmsg_internal(const char *fmt,...)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];
	MemoryContext oldcontext;

	recursion_depth++;
	CHECK_STACK_DEPTH();
	oldcontext = MemoryContextSwitchTo(ErrorContext);

	EVALUATE_MESSAGE(message, false);

	MemoryContextSwitchTo(oldcontext);
	recursion_depth--;
	return 0;					/* return value does not matter */
}


/*
 * errdetail --- add a detail error message text to the current error
 */
int
errdetail(const char *fmt,...)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];
	MemoryContext oldcontext;

	recursion_depth++;
	CHECK_STACK_DEPTH();
	oldcontext = MemoryContextSwitchTo(ErrorContext);

	EVALUATE_MESSAGE(detail, false);

	MemoryContextSwitchTo(oldcontext);
	recursion_depth--;
	return 0;					/* return value does not matter */
}


/*
 * errhint --- add a hint error message text to the current error
 */
int
errhint(const char *fmt,...)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];
	MemoryContext oldcontext;

	recursion_depth++;
	CHECK_STACK_DEPTH();
	oldcontext = MemoryContextSwitchTo(ErrorContext);

	EVALUATE_MESSAGE(hint, false);

	MemoryContextSwitchTo(oldcontext);
	recursion_depth--;
	return 0;					/* return value does not matter */
}


/*
 * errcontext --- add a context error message text to the current error
 *
 * Unlike other cases, multiple calls are allowed to build up a stack of
 * context information.  We assume earlier calls represent more-closely-nested
 * states.
 */
int
errcontext(const char *fmt,...)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];
	MemoryContext oldcontext;

	recursion_depth++;
	CHECK_STACK_DEPTH();
	oldcontext = MemoryContextSwitchTo(ErrorContext);

	EVALUATE_MESSAGE(context, true);

	MemoryContextSwitchTo(oldcontext);
	recursion_depth--;
	return 0;					/* return value does not matter */
}


/*
 * errfunction --- add reporting function name to the current error
 *
 * This is used when backwards compatibility demands that the function
 * name appear in messages sent to old-protocol clients.  Note that the
 * passed string is expected to be a non-freeable constant string.
 */
int
errfunction(const char *funcname)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];

	/* we don't bother incrementing recursion_depth */
	CHECK_STACK_DEPTH();

	edata->funcname = funcname;
	edata->show_funcname = true;

	return 0;					/* return value does not matter */
}

/*
 * errposition --- add cursor position to the current error
 */
int
errposition(int cursorpos)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];

	/* we don't bother incrementing recursion_depth */
	CHECK_STACK_DEPTH();

	edata->cursorpos = cursorpos;

	return 0;					/* return value does not matter */
}


/*
 * elog_finish --- finish up for old-style API
 *
 * The elog() macro already called errstart, but with ERROR rather than
 * the true elevel.
 */
void
elog_finish(int elevel, const char *fmt,...)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];
	MemoryContext oldcontext;

	CHECK_STACK_DEPTH();

	/*
	 * We need to redo errstart() because the elog macro had to call it
	 * with bogus elevel.
	 */
	errordata_stack_depth--;
	errno = edata->saved_errno;
	if (!errstart(elevel, edata->filename, edata->lineno, edata->funcname))
		return;					/* nothing to do */

	/*
	 * Format error message just like errmsg().
	 */
	recursion_depth++;
	oldcontext = MemoryContextSwitchTo(ErrorContext);

	EVALUATE_MESSAGE(message, false);

	MemoryContextSwitchTo(oldcontext);
	recursion_depth--;

	/*
	 * And let errfinish() finish up.
	 */
	errfinish(0);
}


/*
 * Initialization of error output file
 */
void
DebugFileOpen(void)
{
	int			fd,
				istty;

	if (OutputFileName[0])
	{
		/*
		 * A debug-output file name was given.
		 *
		 * Make sure we can write the file, and find out if it's a tty.
		 */
		if ((fd = open(OutputFileName, O_CREAT | O_APPEND | O_WRONLY,
					   0666)) < 0)
			ereport(FATAL,
					(errcode_for_file_access(),
				   errmsg("could not open file \"%s\": %m", OutputFileName)));
		istty = isatty(fd);
		close(fd);

		/*
		 * Redirect our stderr to the debug output file.
		 */
		if (!freopen(OutputFileName, "a", stderr))
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not reopen file \"%s\" as stderr: %m",
							OutputFileName)));

		/*
		 * If the file is a tty and we're running under the postmaster,
		 * try to send stdout there as well (if it isn't a tty then stderr
		 * will block out stdout, so we may as well let stdout go wherever
		 * it was going before).
		 */
		if (istty && IsUnderPostmaster)
			if (!freopen(OutputFileName, "a", stdout))
				ereport(FATAL,
						(errcode_for_file_access(),
						 errmsg("could not reopen file \"%s\" as stdout: %m",
								OutputFileName)));
	}
}



#ifdef HAVE_SYSLOG

#ifndef PG_SYSLOG_LIMIT
#define PG_SYSLOG_LIMIT 128
#endif

/*
 * Write a message line to syslog if the syslog option is set.
 *
 * Our problem here is that many syslog implementations don't handle
 * long messages in an acceptable manner. While this function doesn't
 * help that fact, it does work around by splitting up messages into
 * smaller pieces.
 */
static void
write_syslog(int level, const char *line)
{
	static bool openlog_done = false;
	static unsigned long seq = 0;

	int			len = strlen(line);

	if (Use_syslog == 0)
		return;

	if (!openlog_done)
	{
		int	syslog_fac = LOG_LOCAL0;
		char   *syslog_ident;

		if (strcasecmp(Syslog_facility, "LOCAL0") == 0)
			syslog_fac = LOG_LOCAL0;
		if (strcasecmp(Syslog_facility, "LOCAL1") == 0)
			syslog_fac = LOG_LOCAL1;
		if (strcasecmp(Syslog_facility, "LOCAL2") == 0)
			syslog_fac = LOG_LOCAL2;
		if (strcasecmp(Syslog_facility, "LOCAL3") == 0)
			syslog_fac = LOG_LOCAL3;
		if (strcasecmp(Syslog_facility, "LOCAL4") == 0)
			syslog_fac = LOG_LOCAL4;
		if (strcasecmp(Syslog_facility, "LOCAL5") == 0)
			syslog_fac = LOG_LOCAL5;
		if (strcasecmp(Syslog_facility, "LOCAL6") == 0)
			syslog_fac = LOG_LOCAL6;
		if (strcasecmp(Syslog_facility, "LOCAL7") == 0)
			syslog_fac = LOG_LOCAL7;
		syslog_ident = strdup(Syslog_ident);
		if (syslog_ident == NULL)			/* out of memory already!? */
			syslog_ident = "postgres";
		openlog(syslog_ident, LOG_PID | LOG_NDELAY, syslog_fac);
		openlog_done = true;
	}

	/*
	 * We add a sequence number to each log message to suppress "same"
	 * messages.
	 */
	seq++;

	/* divide into multiple syslog() calls if message is too long */
	/* or if the message contains embedded NewLine(s) '\n' */
	if (len > PG_SYSLOG_LIMIT || strchr(line, '\n') != NULL)
	{
		int			chunk_nr = 0;

		while (len > 0)
		{
			char		buf[PG_SYSLOG_LIMIT + 1];
			int			buflen;
			int			i;

			/* if we start at a newline, move ahead one char */
			if (line[0] == '\n')
			{
				line++;
				len--;
				continue;
			}

			strncpy(buf, line, PG_SYSLOG_LIMIT);
			buf[PG_SYSLOG_LIMIT] = '\0';
			if (strchr(buf, '\n') != NULL)
				*strchr(buf, '\n') = '\0';

			buflen = strlen(buf);

			/* trim to multibyte letter boundary */
			buflen = pg_mbcliplen(buf, buflen, buflen);
			if (buflen <= 0)
				return;
			buf[buflen] = '\0';

			/* already word boundary? */
			if (!isspace((unsigned char) line[buflen]) &&
				line[buflen] != '\0')
			{
				/* try to divide at word boundary */
				i = buflen - 1;
				while (i > 0 && !isspace((unsigned char) buf[i]))
					i--;

				if (i > 0)		/* else couldn't divide word boundary */
				{
					buflen = i;
					buf[i] = '\0';
				}
			}

			chunk_nr++;

			syslog(level, "[%lu-%d] %s", seq, chunk_nr, buf);
			line += buflen;
			len -= buflen;
		}
	}
	else
	{
		/* message short enough */
		syslog(level, "[%lu] %s", seq, line);
	}
}
#endif   /* HAVE_SYSLOG */


/*
 * Write error report to server's log
 */
static void
send_message_to_server_log(ErrorData *edata)
{
	StringInfoData buf;

	initStringInfo(&buf);

	appendStringInfo(&buf, "%s:  ", error_severity(edata->elevel));

	if (Log_error_verbosity >= PGERROR_VERBOSE)
	{
		/* unpack MAKE_SQLSTATE code */
		char		tbuf[12];
		int			ssval;
		int			i;

		ssval = edata->sqlerrcode;
		for (i = 0; i < 5; i++)
		{
			tbuf[i] = PGUNSIXBIT(ssval);
			ssval >>= 6;
		}
		tbuf[i] = '\0';
		appendStringInfo(&buf, "%s: ", tbuf);
	}

	if (edata->message)
		append_with_tabs(&buf, edata->message);
	else
		append_with_tabs(&buf, gettext("missing error text"));

	if (edata->cursorpos > 0)
		appendStringInfo(&buf, gettext(" at character %d"), edata->cursorpos);

	appendStringInfoChar(&buf, '\n');

	if (Log_error_verbosity >= PGERROR_DEFAULT)
	{
		if (edata->detail)
		{
			appendStringInfoString(&buf, gettext("DETAIL:  "));
			append_with_tabs(&buf, edata->detail);
			appendStringInfoChar(&buf, '\n');
		}
		if (edata->hint)
		{
			appendStringInfoString(&buf, gettext("HINT:  "));
			append_with_tabs(&buf, edata->hint);
			appendStringInfoChar(&buf, '\n');
		}
		if (edata->context)
		{
			appendStringInfoString(&buf, gettext("CONTEXT:  "));
			append_with_tabs(&buf, edata->context);
			appendStringInfoChar(&buf, '\n');
		}
		if (Log_error_verbosity >= PGERROR_VERBOSE)
		{
			/* assume no newlines in funcname or filename... */
			if (edata->funcname && edata->filename)
				appendStringInfo(&buf, gettext("LOCATION:  %s, %s:%d\n"),
								 edata->funcname, edata->filename,
								 edata->lineno);
			else if (edata->filename)
				appendStringInfo(&buf, gettext("LOCATION:  %s:%d\n"),
								 edata->filename, edata->lineno);
		}
	}

	/*
	 * If the user wants the query that generated this error logged, do it.
	 */
	if (edata->elevel >= log_min_error_statement && debug_query_string != NULL)
	{
		appendStringInfoString(&buf, gettext("STATEMENT:  "));
		append_with_tabs(&buf, debug_query_string);
		appendStringInfoChar(&buf, '\n');
	}


#ifdef HAVE_SYSLOG
	/* Write to syslog, if enabled */
	if (Use_syslog >= 1)
	{
		int			syslog_level;

		switch (edata->elevel)
		{
			case DEBUG5:
			case DEBUG4:
			case DEBUG3:
			case DEBUG2:
			case DEBUG1:
				syslog_level = LOG_DEBUG;
				break;
			case LOG:
			case COMMERROR:
			case INFO:
				syslog_level = LOG_INFO;
				break;
			case NOTICE:
			case WARNING:
				syslog_level = LOG_NOTICE;
				break;
			case ERROR:
				syslog_level = LOG_WARNING;
				break;
			case FATAL:
				syslog_level = LOG_ERR;
				break;
			case PANIC:
			default:
				syslog_level = LOG_CRIT;
				break;
		}

		write_syslog(syslog_level, buf.data);
	}
#endif   /* HAVE_SYSLOG */

	/* Write to stderr, if enabled */
	if (Use_syslog <= 1 || whereToSendOutput == Debug)
	{
		/*
		 * Timestamp and PID are only used for stderr output --- we assume
		 * the syslog daemon will supply them for us in the other case.
		 */
		fprintf(stderr, "%s%s%s",
				Log_timestamp ? print_timestamp() : "",
				Log_pid ? print_pid() : "",
				buf.data);
	}

	pfree(buf.data);
}


/*
 * Write error report to client
 */
static void
send_message_to_frontend(ErrorData *edata)
{
	StringInfoData msgbuf;

	/* 'N' (Notice) is for nonfatal conditions, 'E' is for errors */
	pq_beginmessage(&msgbuf, (edata->elevel < ERROR) ? 'N' : 'E');

	if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 3)
	{
		/* New style with separate fields */
		char		tbuf[12];
		int			ssval;
		int			i;

		pq_sendbyte(&msgbuf, PG_DIAG_SEVERITY);
		pq_sendstring(&msgbuf, error_severity(edata->elevel));

		/* unpack MAKE_SQLSTATE code */
		ssval = edata->sqlerrcode;
		for (i = 0; i < 5; i++)
		{
			tbuf[i] = PGUNSIXBIT(ssval);
			ssval >>= 6;
		}
		tbuf[i] = '\0';

		pq_sendbyte(&msgbuf, PG_DIAG_SQLSTATE);
		pq_sendstring(&msgbuf, tbuf);

		/* M field is required per protocol, so always send something */
		pq_sendbyte(&msgbuf, PG_DIAG_MESSAGE_PRIMARY);
		if (edata->message)
			pq_sendstring(&msgbuf, edata->message);
		else
			pq_sendstring(&msgbuf, gettext("missing error text"));

		if (edata->detail)
		{
			pq_sendbyte(&msgbuf, PG_DIAG_MESSAGE_DETAIL);
			pq_sendstring(&msgbuf, edata->detail);
		}

		if (edata->hint)
		{
			pq_sendbyte(&msgbuf, PG_DIAG_MESSAGE_HINT);
			pq_sendstring(&msgbuf, edata->hint);
		}

		if (edata->context)
		{
			pq_sendbyte(&msgbuf, PG_DIAG_CONTEXT);
			pq_sendstring(&msgbuf, edata->context);
		}

		if (edata->cursorpos > 0)
		{
			snprintf(tbuf, sizeof(tbuf), "%d", edata->cursorpos);
			pq_sendbyte(&msgbuf, PG_DIAG_STATEMENT_POSITION);
			pq_sendstring(&msgbuf, tbuf);
		}

		if (edata->filename)
		{
			pq_sendbyte(&msgbuf, PG_DIAG_SOURCE_FILE);
			pq_sendstring(&msgbuf, edata->filename);
		}

		if (edata->lineno > 0)
		{
			snprintf(tbuf, sizeof(tbuf), "%d", edata->lineno);
			pq_sendbyte(&msgbuf, PG_DIAG_SOURCE_LINE);
			pq_sendstring(&msgbuf, tbuf);
		}

		if (edata->funcname)
		{
			pq_sendbyte(&msgbuf, PG_DIAG_SOURCE_FUNCTION);
			pq_sendstring(&msgbuf, edata->funcname);
		}

		pq_sendbyte(&msgbuf, '\0');		/* terminator */
	}
	else
	{
		/* Old style --- gin up a backwards-compatible message */
		StringInfoData buf;

		initStringInfo(&buf);

		appendStringInfo(&buf, "%s:  ", error_severity(edata->elevel));

		if (edata->show_funcname && edata->funcname)
			appendStringInfo(&buf, "%s: ", edata->funcname);

		if (edata->message)
			appendStringInfoString(&buf, edata->message);
		else
			appendStringInfoString(&buf, gettext("missing error text"));

		if (edata->cursorpos > 0)
			appendStringInfo(&buf, gettext(" at character %d"),
							 edata->cursorpos);

		appendStringInfoChar(&buf, '\n');

		pq_sendstring(&msgbuf, buf.data);

		pfree(buf.data);
	}

	pq_endmessage(&msgbuf);

	/*
	 * This flush is normally not necessary, since postgres.c will flush
	 * out waiting data when control returns to the main loop. But it
	 * seems best to leave it here, so that the client has some clue what
	 * happened if the backend dies before getting back to the main loop
	 * ... error/notice messages should not be a performance-critical path
	 * anyway, so an extra flush won't hurt much ...
	 */
	pq_flush();
}


/*
 * Support routines for formatting error messages.
 */


/*
 * expand_fmt_string --- process special format codes in a format string
 *
 * We must replace %m with the appropriate strerror string, since vsnprintf
 * won't know what to do with it.
 *
 * The result is a palloc'd string.
 */
static char *
expand_fmt_string(const char *fmt, ErrorData *edata)
{
	StringInfoData buf;
	const char *cp;

	initStringInfo(&buf);

	for (cp = fmt; *cp; cp++)
	{
		if (cp[0] == '%' && cp[1] != '\0')
		{
			cp++;
			if (*cp == 'm')
			{
				/*
				 * Replace %m by system error string.  If there are any
				 * %'s in the string, we'd better double them so that
				 * vsnprintf won't misinterpret.
				 */
				const char *cp2;

				cp2 = useful_strerror(edata->saved_errno);
				for (; *cp2; cp2++)
				{
					if (*cp2 == '%')
						appendStringInfoCharMacro(&buf, '%');
					appendStringInfoCharMacro(&buf, *cp2);
				}
			}
			else
			{
				/* copy % and next char --- this avoids trouble with %%m */
				appendStringInfoCharMacro(&buf, '%');
				appendStringInfoCharMacro(&buf, *cp);
			}
		}
		else
			appendStringInfoCharMacro(&buf, *cp);
	}

	return buf.data;
}


/*
 * A slightly cleaned-up version of strerror()
 */
static const char *
useful_strerror(int errnum)
{
	/* this buffer is only used if errno has a bogus value */
	static char errorstr_buf[48];
	const char *str;

	str = strerror(errnum);

	/*
	 * Some strerror()s return an empty string for out-of-range errno.
	 * This is ANSI C spec compliant, but not exactly useful.
	 */
	if (str == NULL || *str == '\0')
	{
		/*
		 * translator: This string will be truncated at 47 characters
		 * expanded.
		 */
		snprintf(errorstr_buf, sizeof(errorstr_buf),
				 gettext("operating system error %d"), errnum);
		str = errorstr_buf;
	}

	return str;
}


/*
 * error_severity --- get localized string representing elevel
 */
static const char *
error_severity(int elevel)
{
	const char *prefix;

	switch (elevel)
	{
		case DEBUG1:
		case DEBUG2:
		case DEBUG3:
		case DEBUG4:
		case DEBUG5:
			prefix = gettext("DEBUG");
			break;
		case LOG:
		case COMMERROR:
			prefix = gettext("LOG");
			break;
		case INFO:
			prefix = gettext("INFO");
			break;
		case NOTICE:
			prefix = gettext("NOTICE");
			break;
		case WARNING:
			prefix = gettext("WARNING");
			break;
		case ERROR:
			prefix = gettext("ERROR");
			break;
		case FATAL:
			prefix = gettext("FATAL");
			break;
		case PANIC:
			prefix = gettext("PANIC");
			break;
		default:
			prefix = "???";
			break;
	}

	return prefix;
}


/*
 * Return a timestamp string like
 *
 *	 "2000-06-04 13:12:03 "
 */
static const char *
print_timestamp(void)
{
	time_t		curtime;
	static char buf[21];		/* format `YYYY-MM-DD HH:MM:SS ' */

	curtime = time(NULL);

	strftime(buf, sizeof(buf),
			 "%Y-%m-%d %H:%M:%S ",
			 localtime(&curtime));

	return buf;
}


/*
 * Return a string like
 *
 *	   "[123456] "
 *
 * with the current pid.
 */
static const char *
print_pid(void)
{
	static char buf[10];		/* allow `[123456] ' */

	snprintf(buf, sizeof(buf), "[%d] ", (int) MyProcPid);
	return buf;
}

/*
 *	append_with_tabs
 *
 *	Append the string to the StringInfo buffer, inserting a tab after any
 *	newline.
 */
static void
append_with_tabs(StringInfo buf, const char *str)
{
	char	ch;

	while ((ch = *str++) != '\0')
	{
		appendStringInfoCharMacro(buf, ch);
		if (ch == '\n')
			appendStringInfoCharMacro(buf, '\t');
	}
}
