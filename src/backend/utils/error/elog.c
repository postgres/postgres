/*-------------------------------------------------------------------------
 *
 * elog.c--
 *	  error logger
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/error/elog.c,v 1.20 1997/11/09 04:43:35 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#ifndef O_RDONLY
#include <sys/file.h>
#endif							/* O_RDONLY */
#include <sys/types.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>

#include "postgres.h"
#include "miscadmin.h"
#include "libpq/libpq.h"
#include "storage/proc.h"

static int	Debugfile = -1;
static int	Err_file = -1;
static int	ElogDebugIndentLevel = 0;

extern char OutputFileName[];

/*
 * elog --
 *		Old error logging function.
 */
void
elog(int lev, const char *fmt,...)
{
	va_list		ap;
	char		buf[ELOG_MAXLEN],
				line[ELOG_MAXLEN];
	register char *bp;
	register const char *cp;
	extern int	errno,
				sys_nerr;

#ifndef PG_STANDALONE
	extern FILE *Pfout;

#endif							/* !PG_STANDALONE */
#ifdef ELOG_TIMESTAMPS
	time_t		tim;

#endif
	int			len;
	int			i = 0;

	va_start(ap, fmt);
	if (lev == DEBUG && Debugfile < 0)
	{
		return;
	}
	switch (lev)
	{
		case NOIND:
			i = ElogDebugIndentLevel - 1;
			if (i < 0)
				i = 0;
			if (i > 30)
				i = i % 30;
			cp = "DEBUG:";
			break;
		case DEBUG:
			i = ElogDebugIndentLevel;
			if (i < 0)
				i = 0;
			if (i > 30)
				i = i % 30;
			cp = "DEBUG:";
			break;
		case NOTICE:
			cp = "NOTICE:";
			break;
		case WARN:
			cp = "WARN:";
			break;
		default:
			sprintf(line, "FATAL %d:", lev);
			cp = line;
	}
#ifdef ELOG_TIMESTAMPS
	time(&tim);
	strcat(strcpy(buf, cp), ctime(&tim) + 4);
	bp = buf + strlen(buf) - 6;
	*bp++ = ':';
#else
	strcpy(buf, cp);
	bp = buf + strlen(buf);
#endif
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
	vsprintf(line, buf, ap);
	va_end(ap);
	len = strlen(strcat(line, "\n"));
	if (Debugfile > -1)
		write(Debugfile, line, len);
	if (lev == DEBUG || lev == NOIND)
		return;

	/*
	 * If there's an error log file other than our channel to the
	 * front-end program, write to it first.  This is important because
	 * there's a bug in the socket code on ultrix.  If the front end has
	 * gone away (so the channel to it has been closed at the other end),
	 * then writing here can cause this backend to exit without warning --
	 * that is, write() does an exit(). In this case, our only hope of
	 * finding out what's going on is if Err_file was set to some disk
	 * log.  This is a major pain.
	 */

	if (Err_file > -1 && Debugfile != Err_file)
	{
		if (write(Err_file, line, len) < 0)
		{
			write(open("/dev/console", O_WRONLY, 0666), line, len);
			fflush(stdout);
			fflush(stderr);
			exitpg(lev);
		}
		fsync(Err_file);
	}

#ifndef PG_STANDALONE
	/* Send IPC message to the front-end program */
	if (Pfout != NULL && lev > DEBUG)
	{
		/* notices are not exactly errors, handle it differently */
		if (lev == NOTICE)
			pq_putnchar("N", 1);
		else
			pq_putnchar("E", 1);
		/* pq_putint(-101, 4); *//* should be query id */
		pq_putstr(line);
		pq_flush();
	}
	if (Pfout == NULL) {
	/* There is no socket.  One explanation for this is we are running
	   as the Postmaster.  So we'll write the message to stderr.
	 */
		fputs(line, stderr);
	}
#endif							/* !PG_STANDALONE */

	if (lev == WARN)
	{
		extern int	InWarn;

		ProcReleaseSpins(NULL); /* get rid of spinlocks we hold */
		if (!InWarn)
		{
			kill(getpid(), 1);	/* abort to traffic cop */
			pause();
		}

		/*
		 * The pause(3) is just to avoid race conditions where the thread
		 * of control on an MP system gets past here (i.e., the signal is
		 * not received instantaneously).
		 */
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
		exitpg(0);
	}

	if (lev > FATAL)
	{
		fflush(stdout);
		fflush(stderr);
		exitpg(lev);
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
		return (Debugfile);
	}

	/*
	 * If no filename was specified, send debugging output to stderr. If
	 * stderr has been hosed, try to open a file.
	 */
	fd = fileno(stderr);
	if (fcntl(fd, F_GETFD, 0) < 0)
	{
		sprintf(OutputFileName, "%s/pg.errors.%d",
				DataDir, (int) getpid());
		fd = open(OutputFileName, O_CREAT | O_APPEND | O_WRONLY, 0666);
	}
	if (fd < 0)
		elog(FATAL, "DebugFileOpen: could not open debugging file");

	Err_file = Debugfile = fd;
	return (Debugfile);
}

#endif
