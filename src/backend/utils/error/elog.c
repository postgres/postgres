/*-------------------------------------------------------------------------
 *
 * elog.c
 *	  error logger
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/error/elog.c,v 1.107 2003/03/20 03:34:56 momjian Exp $
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

#include "commands/copy.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/memutils.h"
#include "utils/guc.h"

#include "mb/pg_wchar.h"

#ifdef HAVE_SYSLOG
/*
 * 0 = only stdout/stderr
 * 1 = stdout+stderr and syslog
 * 2 = syslog only
 * ... in theory anyway
 */
int			Use_syslog = 0;
char	   *Syslog_facility;
char	   *Syslog_ident;

static void write_syslog(int level, const char *line);

#else
#define Use_syslog 0
#endif

bool		Log_timestamp;
bool		Log_pid;

#define TIMESTAMP_SIZE 20		/* format `YYYY-MM-DD HH:MM:SS ' */
#define PID_SIZE 9				/* format `[123456] ' */

static const char *print_timestamp(void);
static const char *print_pid(void);
static void send_message_to_frontend(int type, const char *msg);
static const char *useful_strerror(int errnum);
static const char *elog_message_prefix(int lev);

static int	Debugfile = -1;


/*--------------------
 * elog
 *		Primary error logging function.
 *
 * 'lev': error level; indicates recovery action to take, if any.
 * 'fmt': a printf-style string.
 * Additional arguments, if any, are formatted per %-escapes in 'fmt'.
 *
 * In addition to the usual %-escapes recognized by printf, "%m" in
 * fmt is replaced by the error message for the current value of errno.
 *
 * Note: no newline is needed at the end of the fmt string, since
 * elog will provide one for the output methods that need it.
 *
 * If 'lev' is ERROR or worse, control does not return to the caller.
 * See elog.h for the error level definitions.
 *--------------------
 */
void
elog(int lev, const char *fmt,...)
{
	va_list		ap;

	/*
	 * The expanded format and final output message are dynamically
	 * allocated if necessary, but not if they fit in the "reasonable
	 * size" buffers shown here.  In extremis, we'd rather depend on
	 * having a few hundred bytes of stack space than on malloc() still
	 * working (since memory-clobber errors often take out malloc first).
	 * Don't make these buffers unreasonably large though, on pain of
	 * having to chase a bug with no error message.
	 *
	 * Note that we use malloc() not palloc() because we want to retain
	 * control if we run out of memory.  palloc() would recursively call
	 * elog(ERROR), which would be all right except if we are working on a
	 * FATAL or PANIC error.	We'd lose track of the fatal condition and
	 * report a mere ERROR to outer loop, which would be a Bad Thing. So,
	 * we substitute an appropriate message in-place, without downgrading
	 * the level if it's above ERROR.
	 */
	char		fmt_fixedbuf[128];
	char		msg_fixedbuf[256];
	char	   *fmt_buf = fmt_fixedbuf;
	char	   *msg_buf = msg_fixedbuf;
	char		copylineno_buf[32];		/* for COPY line numbers */
	const char *errorstr;
	const char *prefix;
	const char *cp;
	char	   *bp;
	size_t		space_needed;
	size_t		timestamp_size; /* prefix len for timestamp+pid */
	bool		output_to_server = false;
	bool		output_to_client = false;

	/* Check for old elog calls.  Codes were renumbered in 7.3. 2002-02-24 */
	if (lev < DEBUG5)
		elog(FATAL, "Pre-7.3 object file made an elog() call.  Recompile.");

	/*
	 * Convert initialization errors into fatal errors. This is probably
	 * redundant, because Warn_restart_ready won't be set anyway.
	 */
	if (lev == ERROR && IsInitProcessingMode())
		lev = FATAL;

	/*
	 * If we are inside a critical section, all errors become PANIC
	 * errors.	See miscadmin.h.
	 */
	if (lev >= ERROR)
	{
		if (CritSectionCount > 0)
			lev = PANIC;
	}

	/* Determine whether message is enabled for server log output */
	/* Complicated because LOG is sorted out-of-order for this purpose */
	if (lev == LOG || lev == COMMERROR)
	{
		if (log_min_messages == LOG)
			output_to_server = true;
		else if (log_min_messages < FATAL)
			output_to_server = true;
	}
	else
	{
		/* lev != LOG */
		if (log_min_messages == LOG)
		{
			if (lev >= FATAL)
				output_to_server = true;
		}
		/* Neither is LOG */
		else if (lev >= log_min_messages)
			output_to_server = true;
	}

	/* Determine whether message is enabled for client output */
	if (whereToSendOutput == Remote && lev != COMMERROR)
	{
		/*
		 * client_min_messages is honored only after we complete the
		 * authentication handshake.  This is required both for security
		 * reasons and because many clients can't handle NOTICE messages
		 * during authentication.
		 */
		if (ClientAuthInProgress)
			output_to_client = (lev >= ERROR);
		else
			output_to_client = (lev >= client_min_messages || lev == INFO);
	}

	/* Skip formatting effort if non-error message will not be output */
	if (lev < ERROR && !output_to_server && !output_to_client)
		return;

	/* Save error str before calling any function that might change errno */
	errorstr = useful_strerror(errno);

	/* Internationalize the error format string */
	fmt = gettext(fmt);

	/* Begin formatting by determining prefix information */
	prefix = elog_message_prefix(lev);

	timestamp_size = 0;
	if (Log_timestamp)
		timestamp_size += TIMESTAMP_SIZE;
	if (Log_pid)
		timestamp_size += PID_SIZE;

	/*
	 * Set up the expanded format, consisting of the prefix string plus
	 * input format, with any %m replaced by strerror() string (since
	 * vsnprintf won't know what to do with %m).  To keep space
	 * calculation simple, we only allow one %m.
	 */
	space_needed = timestamp_size + strlen(prefix) +
		strlen(fmt) + strlen(errorstr) + 1;

	if (copy_lineno)
	{
		/*
		 * Prints the failure line of the COPY.  Wow, what a hack!	bjm
		 * Translator:	Error message will be truncated at 31 characters.
		 */
		snprintf(copylineno_buf, sizeof(copylineno_buf),
				 gettext("copy: line %d, "), copy_lineno);
		space_needed += strlen(copylineno_buf);
	}

	if (space_needed > sizeof(fmt_fixedbuf))
	{
		fmt_buf = malloc(space_needed);
		if (fmt_buf == NULL)
		{
			/* We're up against it, convert to out-of-memory error */
			fmt_buf = fmt_fixedbuf;
			if (lev < ERROR)
			{
				lev = ERROR;
				prefix = elog_message_prefix(lev);
			}

			/*
			 * gettext doesn't allocate memory, except in the very first
			 * call (which this isn't), so it's safe to translate here.
			 * Worst case we get the untranslated string back.
			 */
			/* translator: This must fit in fmt_fixedbuf. */
			fmt = gettext("elog: out of memory");
		}
	}

	fmt_buf[0] = '\0';

	if (Log_timestamp)
		strcat(fmt_buf, print_timestamp());
	if (Log_pid)
		strcat(fmt_buf, print_pid());

	strcat(fmt_buf, prefix);

	/* If error was in CopyFrom() print the offending line number -- dz */
	if (copy_lineno)
	{
		strcat(fmt_buf, copylineno_buf);
		if (lev >= ERROR)
			copy_lineno = 0;
	}

	bp = fmt_buf + strlen(fmt_buf);

	for (cp = fmt; *cp; cp++)
	{
		if (cp[0] == '%' && cp[1] != '\0')
		{
			if (cp[1] == 'm')
			{
				/*
				 * XXX If there are any %'s in errorstr then vsnprintf
				 * will do the Wrong Thing; do we need to cope? Seems
				 * unlikely that % would appear in system errors.
				 */
				strcpy(bp, errorstr);

				/*
				 * copy the rest of fmt literally, since we can't afford
				 * to insert another %m.
				 */
				strcat(bp, cp + 2);
				bp += strlen(bp);
				break;
			}
			else
			{
				/* copy % and next char --- this avoids trouble with %%m */
				*bp++ = *cp++;
				*bp++ = *cp;
			}
		}
		else
			*bp++ = *cp;
	}
	*bp = '\0';

	/*
	 * Now generate the actual output text using vsnprintf(). Be sure to
	 * leave space for \n added later as well as trailing null.
	 */
	space_needed = sizeof(msg_fixedbuf);
	for (;;)
	{
		int			nprinted;

		va_start(ap, fmt);
		nprinted = vsnprintf(msg_buf, space_needed - 2, fmt_buf, ap);
		va_end(ap);

		/*
		 * Note: some versions of vsnprintf return the number of chars
		 * actually stored, but at least one returns -1 on failure. Be
		 * conservative about believing whether the print worked.
		 */
		if (nprinted >= 0 && nprinted < space_needed - 3)
			break;
		/* It didn't work, try to get a bigger buffer */
		if (msg_buf != msg_fixedbuf)
			free(msg_buf);
		space_needed *= 2;
		msg_buf = malloc(space_needed);
		if (msg_buf == NULL)
		{
			/* We're up against it, convert to out-of-memory error */
			msg_buf = msg_fixedbuf;
			if (lev < ERROR)
			{
				lev = ERROR;
				prefix = elog_message_prefix(lev);
			}
			msg_buf[0] = '\0';
			if (Log_timestamp)
				strcat(msg_buf, print_timestamp());
			if (Log_pid)
				strcat(msg_buf, print_pid());
			strcat(msg_buf, prefix);
			strcat(msg_buf, gettext("elog: out of memory"));
			break;
		}
	}


	/*
	 * Message prepared; send it where it should go
	 */

#ifdef HAVE_SYSLOG
	/* Write to syslog, if enabled */
	if (output_to_server && Use_syslog >= 1)
	{
		int			syslog_level;

		switch (lev)
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

		write_syslog(syslog_level, msg_buf + timestamp_size);
	}
#endif   /* HAVE_SYSLOG */

	/* syslog doesn't want a trailing newline, but other destinations do */
	strcat(msg_buf, "\n");

	/* Write to stderr, if enabled */
	if (output_to_server && (Use_syslog <= 1 || whereToSendOutput == Debug))
		write(2, msg_buf, strlen(msg_buf));

	/* Send to client, if enabled */
	if (output_to_client)
	{
		/* Send IPC message to the front-end program */
		MemoryContext oldcxt;

		/*
		 * Since backend libpq may call palloc(), switch to a context
		 * where there's fairly likely to be some free space.  After all
		 * the pushups above, we don't want to drop the ball by running
		 * out of space now...
		 */
		oldcxt = MemoryContextSwitchTo(ErrorContext);

		if (lev <= WARNING)
			/* exclude the timestamp from msg sent to frontend */
			send_message_to_frontend(lev, msg_buf + timestamp_size);
		else
		{
			/*
			 * Abort any COPY OUT in progress when an error is detected.
			 * This hack is necessary because of poor design of copy
			 * protocol.
			 */
			pq_endcopyout(true);
			send_message_to_frontend(ERROR, msg_buf + timestamp_size);
		}

		MemoryContextSwitchTo(oldcxt);
	}

	/* done with the message, release space */
	if (fmt_buf != fmt_fixedbuf)
		free(fmt_buf);
	if (msg_buf != msg_fixedbuf)
		free(msg_buf);

	/*
	 * If the user wants this elog() generating query logged, do so. We
	 * only want to log if the query has been written to
	 * debug_query_string. Also, avoid infinite loops.
	 */

	if (lev != LOG && lev >= log_min_error_statement && debug_query_string)
		elog(LOG, "statement: %s", debug_query_string);

	/*
	 * Perform error recovery action as specified by lev.
	 */
	if (lev == ERROR || lev == FATAL)
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
		 * If we have not yet entered the main backend loop (ie, we are in
		 * the postmaster or in backend startup), we also go directly to
		 * proc_exit.  The same is true if anyone tries to report an error
		 * after proc_exit has begun to run.  (It's proc_exit's
		 * responsibility to see that this doesn't turn into infinite
		 * recursion!)	But in the latter case, we exit with nonzero exit
		 * code to indicate that something's pretty wrong.  We also want
		 * to exit with nonzero exit code if not running under the
		 * postmaster (for example, if we are being run from the initdb
		 * script, we'd better return an error status).
		 */
		if (lev == FATAL || !Warn_restart_ready || proc_exit_inprogress)
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
		 * Guard against infinite loop from elog() during error recovery.
		 */
		if (InError)
			elog(PANIC, "elog: error during error recovery, giving up!");
		InError = true;

		/*
		 * Otherwise we can return to the main loop in postgres.c.
		 */
		siglongjmp(Warn_restart, 1);
	}

	if (lev == PANIC)
	{
		/*
		 * Serious crash time. Postmaster will observe nonzero process
		 * exit status and kill the other backends too.
		 *
		 * XXX: what if we are *in* the postmaster?  abort() won't kill
		 * our children...
		 */
		ImmediateInterruptOK = false;
		fflush(stdout);
		fflush(stderr);
		abort();
	}

	/* We reach here if lev <= WARNING. OK to return to caller. */
}


int
DebugFileOpen(void)
{
	int			fd,
				istty;

	Debugfile = -1;

	if (OutputFileName[0])
	{
		/*
		 * A debug-output file name was given.
		 *
		 * Make sure we can write the file, and find out if it's a tty.
		 */
		if ((fd = open(OutputFileName, O_CREAT | O_APPEND | O_WRONLY,
					   0666)) < 0)
			elog(FATAL, "DebugFileOpen: open of %s: %m",
				 OutputFileName);
		istty = isatty(fd);
		close(fd);

		/*
		 * Redirect our stderr to the debug output file.
		 */
		if (!freopen(OutputFileName, "a", stderr))
			elog(FATAL, "DebugFileOpen: %s reopen as stderr: %m",
				 OutputFileName);
		Debugfile = fileno(stderr);

		/*
		 * If the file is a tty and we're running under the postmaster,
		 * try to send stdout there as well (if it isn't a tty then stderr
		 * will block out stdout, so we may as well let stdout go wherever
		 * it was going before).
		 */
		if (istty && IsUnderPostmaster)
			if (!freopen(OutputFileName, "a", stdout))
				elog(FATAL, "DebugFileOpen: %s reopen as stdout: %m",
					 OutputFileName);
		return Debugfile;
	}

	/*
	 * If no filename was specified, send debugging output to stderr. If
	 * stderr has been hosed, try to open a file.
	 */
	fd = fileno(stderr);
	if (fcntl(fd, F_GETFD, 0) < 0)
	{
		snprintf(OutputFileName, MAXPGPATH, "%s/pg.errors.%d",
				 DataDir, (int) MyProcPid);
		fd = open(OutputFileName, O_CREAT | O_APPEND | O_WRONLY, 0666);
	}
	if (fd < 0)
		elog(FATAL, "DebugFileOpen: could not open debugging file");

	Debugfile = fd;
	return Debugfile;
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
	static char buf[TIMESTAMP_SIZE + 1];

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
	static char buf[PID_SIZE + 1];

	snprintf(buf, PID_SIZE + 1, "[%d]      ", (int) MyProcPid);
	return buf;
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
	static int	syslog_fac = LOG_LOCAL0;

	int			len = strlen(line);

	if (Use_syslog == 0)
		return;

	if (!openlog_done)
	{
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
		openlog(Syslog_ident, LOG_PID | LOG_NDELAY, syslog_fac);
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
			int			l;
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

			l = strlen(buf);

			/* trim to multibyte letter boundary */
			buflen = pg_mbcliplen(buf, l, l);
			if (buflen <= 0)
				return;
			buf[buflen] = '\0';
			l = strlen(buf);

			/* already word boundary? */
			if (isspace((unsigned char) line[l]) || line[l] == '\0')
				buflen = l;
			else
			{
				/* try to divide at word boundary */
				i = l - 1;
				while (i > 0 && !isspace((unsigned char) buf[i]))
					i--;

				if (i <= 0)		/* couldn't divide word boundary */
					buflen = l;
				else
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


static void
send_message_to_frontend(int type, const char *msg)
{
	StringInfoData buf;

	AssertArg(type <= ERROR);

	pq_beginmessage(&buf);
	/* 'N' (Notice) is for nonfatal conditions, 'E' is for errors */
	pq_sendbyte(&buf, type < ERROR ? 'N' : 'E');
	pq_sendstring(&buf, msg);
	pq_endmessage(&buf);

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


static const char *
useful_strerror(int errnum)
{
	/* this buffer is only used if errno has a bogus value */
	static char errorstr_buf[48];
	char	   *str;

	if (errnum == ERANGE)
		/* small trick to save creating many regression test result files */
		str = gettext("Numerical result out of range");
	else
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



static const char *
elog_message_prefix(int lev)
{
	const char *prefix = NULL;

	switch (lev)
	{
		case DEBUG1:
		case DEBUG2:
		case DEBUG3:
		case DEBUG4:
		case DEBUG5:
			prefix = gettext("DEBUG:  ");
			break;
		case LOG:
		case COMMERROR:
			prefix = gettext("LOG:  ");
			break;
		case INFO:
			prefix = gettext("INFO:  ");
			break;
		case NOTICE:
			prefix = gettext("NOTICE:  ");
			break;
		case WARNING:
			prefix = gettext("WARNING:  ");
			break;
		case ERROR:
			prefix = gettext("ERROR:  ");
			break;
		case FATAL:
			prefix = gettext("FATAL:  ");
			break;
		case PANIC:
			prefix = gettext("PANIC:  ");
			break;
	}

	Assert(prefix != NULL);
	return prefix;
}
