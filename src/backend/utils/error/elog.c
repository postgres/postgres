/*-------------------------------------------------------------------------
 *
 * elog.c
 *	  error logger
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/error/elog.c,v 1.42 1999/04/25 03:19:11 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#ifndef O_RDONLY
#include <sys/file.h>
#endif	 /* O_RDONLY */
#include <sys/types.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>

#ifdef USE_SYSLOG
#include <syslog.h>
#endif

#include "postgres.h"
#include "miscadmin.h"
#include "libpq/libpq.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/trace.h"

#ifdef USE_SYSLOG
/*
 * Global option to control the use of syslog(3) for logging:
 *
 *		0	stdout/stderr only
 *		1	stdout/stderr + syslog
 *		2	syslog only
 */
#define UseSyslog pg_options[OPT_SYSLOG]
#define PG_LOG_FACILITY LOG_LOCAL0
#else
#define UseSyslog 0
#endif

static int	Debugfile = -1;
static int	Err_file = -1;
static int	ElogDebugIndentLevel = 0;

/*
 * elog 
 *		Old error logging function.
 */
void
elog(int lev, const char *fmt,...)
{
	va_list		ap;
	char		buf[ELOG_MAXLEN],
				line[ELOG_MAXLEN];
	char	   *bp;
	const char *cp;
	extern int	errno,
				sys_nerr;

#ifdef USE_SYSLOG
	int			log_level;

#endif

	int			len;
	int			i = 0;

	va_start(ap, fmt);
	if (lev == DEBUG && Debugfile < 0)
		return;
	switch (lev)
	{
		case NOIND:
			i = ElogDebugIndentLevel - 1;
			if (i < 0)
				i = 0;
			if (i > 30)
				i = i % 30;
			cp = "DEBUG:  ";
			break;
		case DEBUG:
			i = ElogDebugIndentLevel;
			if (i < 0)
				i = 0;
			if (i > 30)
				i = i % 30;
			cp = "DEBUG:  ";
			break;
		case NOTICE:
			cp = "NOTICE:  ";
			break;
		case ERROR:
			cp = "ERROR:  ";
			break;
		default:
			sprintf(line, "FATAL %d:  ", lev);
			cp = line;
	}
#ifdef ELOG_TIMESTAMPS
	strcpy(buf, tprintf_timestamp());
	strcat(buf, cp);
#else
	strcpy(buf, cp);
#endif
	bp = buf + strlen(buf);
	while (i-- > 0)
		*bp++ = ' ';
	for (cp = fmt; *cp; cp++)
		if (*cp == '%' && *(cp + 1) == 'm')
		{
			if (errno < sys_nerr && errno >= 0)
				strcpy(bp, strerror(errno));
			else
				sprintf(bp, "error %d", errno);
			bp += strlen(bp);
			cp++;
		}
		else
			*bp++ = *cp;
	*bp = '\0';
	vsnprintf(line, ELOG_MAXLEN - 1, buf, ap);
	va_end(ap);

#ifdef USE_SYSLOG
	switch (lev)
	{
		case NOIND:
			log_level = LOG_DEBUG;
			break;
		case DEBUG:
			log_level = LOG_DEBUG;
			break;
		case NOTICE:
			log_level = LOG_NOTICE;
			break;
		case ERROR:
			log_level = LOG_WARNING;
			break;
		case FATAL:
		default:
			log_level = LOG_ERR;
			break;
	}
	write_syslog(log_level, line + TIMESTAMP_SIZE);
#endif

	len = strlen(strcat(line, "\n"));
	if ((Debugfile > -1) && (UseSyslog <= 1))
		write(Debugfile, line, len);
	if (lev == DEBUG || lev == NOIND)
		return;

	/*
	 * If there's an error log file other than our channel to the
	 * front-end program, write to it first.  This is important because
	 * there's a bug in the socket code on ultrix.  If the front end has
	 * gone away (so the channel to it has been closed at the other end),
	 * then writing here can cause this backend to exit without warning 
	 * that is, write() does an exit(). In this case, our only hope of
	 * finding out what's going on is if Err_file was set to some disk
	 * log.  This is a major pain.
	 */

	if (Err_file > -1 && Debugfile != Err_file && (UseSyslog <= 1))
	{
		if (write(Err_file, line, len) < 0)
		{
			write(open("/dev/console", O_WRONLY, 0666), line, len);
			fflush(stdout);
			fflush(stderr);
			proc_exit(lev);
		}
		fsync(Err_file);
	}

#ifndef PG_STANDALONE
	/* Send IPC message to the front-end program */
	if (IsUnderPostmaster && lev > DEBUG)
	{
		/* notices are not errors, handle 'em differently */
		char msgtype;
		if (lev == NOTICE)
			msgtype = 'N';
		else
		{
			/* Abort any COPY OUT in progress when an error is detected.
			 * This hack is necessary because of poor design of copy protocol.
			 */
			pq_endcopyout(true);
			msgtype = 'E';
		}
		/* exclude the timestamp from msg sent to frontend */
		pq_putmessage(msgtype, line + TIMESTAMP_SIZE,
					  strlen(line + TIMESTAMP_SIZE) + 1);
		/*
		 * This flush is normally not necessary, since postgres.c will
		 * flush out waiting data when control returns to the main loop.
		 * But it seems best to leave it here, so that the client has some
		 * clue what happened if the backend dies before getting back to the
		 * main loop ... error/notice messages should not be a performance-
		 * critical path anyway, so an extra flush won't hurt much ...
		 */
		pq_flush();
	}
	if (!IsUnderPostmaster)
	{

		/*
		 * There is no socket.	One explanation for this is we are running
		 * as the Postmaster.  So we'll write the message to stderr.
		 */
		fputs(line, stderr);
	}
#endif	 /* !PG_STANDALONE */

	if (lev == ERROR)
	{
		ProcReleaseSpins(NULL); /* get rid of spinlocks we hold */
		if (!InError)
		{
			/* exit to main loop */
			siglongjmp(Warn_restart, 1);
		}
	}

	if (lev == FATAL)
	{

		/*
		 * Assume that if we have detected the failure we can exit with a
		 * normal exit status.	This will prevent the postmaster from
		 * cleaning up when it's not needed.
		 */
		fflush(stdout);
		fflush(stderr);
		ProcReleaseSpins(NULL); /* get rid of spinlocks we hold */
		ProcReleaseLocks();		/* get rid of real locks we hold */
		proc_exit(0);
	}

	if (lev > FATAL)
	{
		fflush(stdout);
		fflush(stderr);
		proc_exit(lev);
	}
}

#ifndef PG_STANDALONE
int
DebugFileOpen(void)
{
	int			fd,
				istty;

	Err_file = Debugfile = -1;
	ElogDebugIndentLevel = 0;

	if (OutputFileName[0])
	{
		OutputFileName[MAXPGPATH - 1] = '\0';
		if ((fd = open(OutputFileName, O_CREAT | O_APPEND | O_WRONLY,
					   0666)) < 0)
			elog(FATAL, "DebugFileOpen: open of %s: %m",
				 OutputFileName);
		istty = isatty(fd);
		close(fd);

		/*
		 * If the file is a tty and we're running under the postmaster,
		 * try to send stdout there as well (if it isn't a tty then stderr
		 * will block out stdout, so we may as well let stdout go wherever
		 * it was going before).
		 */
		if (istty &&
			IsUnderPostmaster &&
			!freopen(OutputFileName, "a", stdout))
			elog(FATAL, "DebugFileOpen: %s reopen as stdout: %m",
				 OutputFileName);
		if (!freopen(OutputFileName, "a", stderr))
			elog(FATAL, "DebugFileOpen: %s reopen as stderr: %m",
				 OutputFileName);
		Err_file = Debugfile = fileno(stderr);
		return Debugfile;
	}

	/*
	 * If no filename was specified, send debugging output to stderr. If
	 * stderr has been hosed, try to open a file.
	 */
	fd = fileno(stderr);
	if (fcntl(fd, F_GETFD, 0) < 0)
	{
		sprintf(OutputFileName, "%s/pg.errors.%d",
				DataDir, (int) MyProcPid);
		fd = open(OutputFileName, O_CREAT | O_APPEND | O_WRONLY, 0666);
	}
	if (fd < 0)
		elog(FATAL, "DebugFileOpen: could not open debugging file");

	Err_file = Debugfile = fd;
	return Debugfile;
}

#endif
