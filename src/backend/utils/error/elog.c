/*-------------------------------------------------------------------------
 *
 * elog.c
 *	  error logger
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/error/elog.c,v 1.78 2001/01/21 00:59:26 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <time.h>
#include <fcntl.h>
#ifndef O_RDONLY
#include <sys/file.h>
#endif	 /* O_RDONLY */
#include <sys/types.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#include <ctype.h>
#ifdef ENABLE_SYSLOG
#include <syslog.h>
#endif

#include "commands/copy.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/memutils.h"

#ifdef MULTIBYTE
#include "mb/pg_wchar.h"
#endif

extern int	errno;

#ifdef HAVE_SYS_NERR
extern int sys_nerr;
#endif

extern CommandDest whereToSendOutput;

#ifdef ENABLE_SYSLOG
/*
 * 0 = only stdout/stderr
 * 1 = stdout+stderr and syslog
 * 2 = syslog only
 * ... in theory anyway
 */
int Use_syslog = 0;
char *Syslog_facility;
char *Syslog_ident;

static void write_syslog(int level, const char *line);

#else
# define Use_syslog 0
#endif

bool Log_timestamp;
bool Log_pid;

#define TIMESTAMP_SIZE 20		/* format `YYYY-MM-DD HH:MM:SS ' */
#define PID_SIZE 9				/* format `[123456] ' */

static const char * print_timestamp(void);
static const char * print_pid(void);

static int	Debugfile = -1;
static int	ElogDebugIndentLevel = 0;

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
elog(int lev, const char *fmt, ...)
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
	 * FATAL or REALLYFATAL error.  We'd lose track of the fatal condition
	 * and report a mere ERROR to outer loop, which would be a Bad Thing.
	 * So, we substitute an appropriate message in-place, without downgrading
	 * the level if it's above ERROR.
	 */
	char		fmt_fixedbuf[128];
	char		msg_fixedbuf[256];
	char	   *fmt_buf = fmt_fixedbuf;
	char	   *msg_buf = msg_fixedbuf;
	/* this buffer is only used for strange values of lev: */
	char		prefix_buf[32];
	/* this buffer is only used if errno has a bogus value: */
	char		errorstr_buf[32];
	const char *errorstr;
	const char *prefix;
	const char *cp;
	char	   *bp;
	int			indent = 0;
	int			space_needed;
	int			len;
	/* size of the prefix needed for timestamp and pid, if enabled */
	size_t		timestamp_size;

	if (lev <= DEBUG && Debugfile < 0)
		return;					/* ignore debug msgs if noplace to send */

	/* Save error str before calling any function that might change errno */
	if (errno >= 0
#ifdef HAVE_SYS_NERR
		&& errno <= sys_nerr
#endif
		)
		errorstr = strerror(errno);
	else
		errorstr = NULL;
	/*
	 * Some strerror()s return an empty string for out-of-range errno.
	 * This is ANSI C spec compliant, but not exactly useful.
	 */
	if (errorstr == NULL || *errorstr == '\0')
	{
		sprintf(errorstr_buf, "error %d", errno);
		errorstr = errorstr_buf;
	}

	if (lev == ERROR || lev == FATAL)
	{
		/*
		 * Convert initialization errors into fatal errors.
		 * This is probably redundant, because Warn_restart_ready won't
		 * be set anyway...
		 */
		if (IsInitProcessingMode())
			lev = FATAL;
		/*
		 * If we are inside a critical section, all errors become STOP errors.
		 * See miscadmin.h.
		 */
		if (CritSectionCount > 0)
			lev = STOP;
	}

	/* choose message prefix and indent level */
	switch (lev)
	{
		case NOIND:
			indent = ElogDebugIndentLevel - 1;
			if (indent < 0)
				indent = 0;
			if (indent > 30)
				indent = indent % 30;
			prefix = "DEBUG:  ";
			break;
		case DEBUG:
			indent = ElogDebugIndentLevel;
			if (indent < 0)
				indent = 0;
			if (indent > 30)
				indent = indent % 30;
			prefix = "DEBUG:  ";
			break;
		case NOTICE:
			prefix = "NOTICE:  ";
			break;
		case ERROR:
			prefix = "ERROR:  ";
			break;
		default:
			sprintf(prefix_buf, "FATAL %d:  ", lev);
			prefix = prefix_buf;
			break;
	}

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
	space_needed = timestamp_size + strlen(prefix) + indent + (lineno ? 24 : 0)
		+ strlen(fmt) + strlen(errorstr) + 1;
	if (space_needed > (int) sizeof(fmt_fixedbuf))
	{
		fmt_buf = (char *) malloc(space_needed);
		if (fmt_buf == NULL)
		{
			/* We're up against it, convert to out-of-memory error */
			fmt_buf = fmt_fixedbuf;
			if (lev < FATAL)
			{
				lev = ERROR;
				prefix = "ERROR:  ";
			}
			fmt = "elog: out of memory";		/* this must fit in
												 * fmt_fixedbuf! */
		}
	}

	fmt_buf[0] = '\0';

	if (Log_timestamp)
		strcat(fmt_buf, print_timestamp());
	if (Log_pid)
		strcat(fmt_buf, print_pid());

	strcat(fmt_buf, prefix);

	bp = fmt_buf + strlen(fmt_buf);
	while (indent-- > 0)
		*bp++ = ' ';

	/* If error was in CopyFrom() print the offending line number -- dz */
	if (lineno)
	{
		sprintf(bp, "copy: line %d, ", lineno);
		bp += strlen(bp);
		if (lev == ERROR || lev >= FATAL)
			lineno = 0;
	}

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
		msg_buf = (char *) malloc(space_needed);
		if (msg_buf == NULL)
		{
			/* We're up against it, convert to out-of-memory error */
			msg_buf = msg_fixedbuf;
			if (lev < FATAL)
			{
				lev = ERROR;
				prefix = "ERROR:  ";
			}
			msg_buf[0] = '\0';
			if (Log_timestamp)
				strcat(msg_buf, print_timestamp());
			if (Log_pid)
				strcat(msg_buf, print_pid());
			strcat(msg_buf, prefix);
			strcat(msg_buf, "elog: out of memory");
			break;
		}
	}

	/*
	 * Message prepared; send it where it should go
	 */

#ifdef ENABLE_SYSLOG
	/* Write to syslog, if enabled */
	if (Use_syslog >= 1)
	{
		int syslog_level;

		switch (lev)
		{
			case NOIND:
				syslog_level = LOG_DEBUG;
				break;
			case DEBUG:
				syslog_level = LOG_DEBUG;
				break;
			case NOTICE:
				syslog_level = LOG_NOTICE;
				break;
			case ERROR:
				syslog_level = LOG_WARNING;
				break;
			case FATAL:
				syslog_level = LOG_ERR;
				break;
			case REALLYFATAL:
			default:
				syslog_level = LOG_CRIT;
		}

		write_syslog(syslog_level, msg_buf + timestamp_size);
	}
#endif /* ENABLE_SYSLOG */

	/* syslog doesn't want a trailing newline, but other destinations do */
	strcat(msg_buf, "\n");

	len = strlen(msg_buf);

	/* Write to debug file, if open and enabled */
	/* NOTE: debug file is typically pointed at stderr */
	if (Debugfile >= 0 && Use_syslog <= 1)
		write(Debugfile, msg_buf, len);

	if (lev > DEBUG && whereToSendOutput == Remote)
	{
		/* Send IPC message to the front-end program */
		MemoryContext oldcxt;
		char		msgtype;

		/*
		 * Since backend libpq may call palloc(), switch to a context where
		 * there's fairly likely to be some free space.  After all the
		 * pushups above, we don't want to drop the ball by running out of
		 * space now...
		 */
		oldcxt = MemoryContextSwitchTo(ErrorContext);

		if (lev == NOTICE)
			msgtype = 'N';
		else
		{

			/*
			 * Abort any COPY OUT in progress when an error is detected.
			 * This hack is necessary because of poor design of copy
			 * protocol.
			 */
			pq_endcopyout(true);
			msgtype = 'E';
		}
		/* exclude the timestamp from msg sent to frontend */
		pq_puttextmessage(msgtype, msg_buf + timestamp_size);

		/*
		 * This flush is normally not necessary, since postgres.c will
		 * flush out waiting data when control returns to the main loop.
		 * But it seems best to leave it here, so that the client has some
		 * clue what happened if the backend dies before getting back to
		 * the main loop ... error/notice messages should not be a
		 * performance-critical path anyway, so an extra flush won't hurt
		 * much ...
		 */
		pq_flush();

		MemoryContextSwitchTo(oldcxt);
	}

	if (lev > DEBUG && whereToSendOutput != Remote)
	{

		/*
		 * We are running as an interactive backend, so just send the
		 * message to stderr.  But don't send a duplicate if Debugfile
		 * write, above, already sent to stderr.
		 */
		if (Debugfile != fileno(stderr))
			fputs(msg_buf, stderr);
	}

	/* done with the message, release space */
	if (fmt_buf != fmt_fixedbuf)
		free(fmt_buf);
	if (msg_buf != msg_fixedbuf)
		free(msg_buf);

	/*
	 * Perform error recovery action as specified by lev.
	 */
	if (lev == ERROR || lev == FATAL)
	{
		/* Prevent immediate interrupt while entering error recovery */
		ImmediateInterruptOK = false;

		/*
		 * For a FATAL error, we let proc_exit clean up and exit.
		 *
		 * If we have not yet entered the main backend loop (ie, we are in
		 * the postmaster or in backend startup), we also go directly to
		 * proc_exit.  The same is true if anyone tries to report an error
		 * after proc_exit has begun to run.  (It's proc_exit's
		 * responsibility to see that this doesn't turn into infinite
		 * recursion!)	But in the latter case, we exit with nonzero exit
		 * code to indicate that something's pretty wrong.
		 */
		if (lev == FATAL || !Warn_restart_ready || proc_exit_inprogress)
		{
			/*
			 * fflush here is just to improve the odds that we get to see
			 * the error message, in case things are so hosed that proc_exit
			 * crashes.  Any other code you might be tempted to add here
			 * should probably be in an on_proc_exit callback instead.
			 */
			fflush(stdout);
			fflush(stderr);
			proc_exit((int) proc_exit_inprogress);
		}

		/*
		 * Guard against infinite loop from elog() during error recovery.
		 */
		if (InError)
			elog(REALLYFATAL, "elog: error during error recovery, giving up!");
		InError = true;

		/*
		 * Otherwise we can return to the main loop in postgres.c.
		 */
		siglongjmp(Warn_restart, 1);
	}

	if (lev > FATAL)
	{
		/*
		 * Serious crash time. Postmaster will observe nonzero process
		 * exit status and kill the other backends too.
		 *
		 * XXX: what if we are *in* the postmaster?  proc_exit() won't kill
		 * our children...
		 */
		ImmediateInterruptOK = false;
		fflush(stdout);
		fflush(stderr);
		proc_exit(lev);
	}

	/* We reach here if lev <= NOTICE.	OK to return to caller. */
}

int
DebugFileOpen(void)
{
	int			fd,
				istty;

	Debugfile = -1;
	ElogDebugIndentLevel = 0;

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
		snprintf(OutputFileName, MAXPGPATH, "%s%cpg.errors.%d",
				 DataDir, SEP_CHAR, (int) MyProcPid);
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
 *   "2000-06-04 13:12:03 "
 */
static const char *
print_timestamp(void)
{
	time_t curtime;
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
 *     "[123456] "
 *
 * with the current pid.
 */
static const char *
print_pid(void)
{
	static char buf[PID_SIZE + 1];

	snprintf(buf, PID_SIZE + 1, "[%d]      ", (int)MyProcPid);
	return buf;
}



#ifdef ENABLE_SYSLOG

#ifndef PG_SYSLOG_LIMIT
# define PG_SYSLOG_LIMIT 128
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
	static bool	openlog_done = false;
	static unsigned long seq = 0;
	static int	syslog_fac = LOG_LOCAL0;

	int len = strlen(line);

	if (Use_syslog == 0)
		return;

	if (!openlog_done)
	{
		if (strcasecmp(Syslog_facility,"LOCAL0") == 0) 
			syslog_fac = LOG_LOCAL0;
		if (strcasecmp(Syslog_facility,"LOCAL1") == 0)
			syslog_fac = LOG_LOCAL1;
		if (strcasecmp(Syslog_facility,"LOCAL2") == 0)
			syslog_fac = LOG_LOCAL2;
		if (strcasecmp(Syslog_facility,"LOCAL3") == 0)
			syslog_fac = LOG_LOCAL3;
		if (strcasecmp(Syslog_facility,"LOCAL4") == 0)
			syslog_fac = LOG_LOCAL4;
		if (strcasecmp(Syslog_facility,"LOCAL5") == 0)
			syslog_fac = LOG_LOCAL5;
		if (strcasecmp(Syslog_facility,"LOCAL6") == 0)
			syslog_fac = LOG_LOCAL6;
		if (strcasecmp(Syslog_facility,"LOCAL7") == 0)
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
	if (len > PG_SYSLOG_LIMIT || strchr(line,'\n') != NULL )
	{
		int		chunk_nr = 0;

		while (len > 0)
		{
			char	buf[PG_SYSLOG_LIMIT+1];
			int		buflen;
			int		l;
			int		i;

			/* if we start at a newline, move ahead one char */
			if (line[0] == '\n')
			{
				line++;
				len--;
				continue;
			}

			strncpy(buf, line, PG_SYSLOG_LIMIT);
			buf[PG_SYSLOG_LIMIT] = '\0';
			if (strchr(buf,'\n') != NULL) 
				*strchr(buf,'\n') = '\0';

			l = strlen(buf);
#ifdef MULTIBYTE
			/* trim to multibyte letter boundary */ 
			buflen = pg_mbcliplen(buf, l, l);
			buf[buflen] = '\0';
			l = strlen(buf);
#endif
			/* already word boundary? */
			if (isspace((unsigned char) line[l]) || line[l] == '\0')
				buflen = l;
			else
			{
				/* try to divide at word boundary */
				i = l - 1;
				while (i > 0 && !isspace((unsigned char) buf[i]))
					i--;

				if (i <= 0)	/* couldn't divide word boundary */
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

#endif /* ENABLE_SYSLOG */
