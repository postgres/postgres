/*-------------------------------------------------------------------------
 *
 * pgarch.c
 *
 *	PostgreSQL WAL archiver
 *
 *	All functions relating to archiver are included here
 *
 *	- All functions executed by archiver process
 *
 *	- archiver is forked from postmaster, and the two
 *	processes then communicate using signals. All functions
 *	executed by postmaster are included in this file.
 *
 *	Initial author: Simon Riggs		simon@2ndquadrant.com
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/postmaster/pgarch.c,v 1.27 2006/11/21 20:59:52 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "access/xlog_internal.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/fork_process.h"
#include "postmaster/pgarch.h"
#include "postmaster/postmaster.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/pg_shmem.h"
#include "storage/pmsignal.h"
#include "utils/guc.h"
#include "utils/ps_status.h"


/* ----------
 * Timer definitions.
 * ----------
 */
#define PGARCH_AUTOWAKE_INTERVAL 60		/* How often to force a poll of the
										 * archive status directory; in
										 * seconds. */
#define PGARCH_RESTART_INTERVAL 10		/* How often to attempt to restart a
										 * failed archiver; in seconds. */

/* ----------
 * Archiver control info.
 *
 * We expect that archivable files within pg_xlog will have names between
 * MIN_XFN_CHARS and MAX_XFN_CHARS in length, consisting only of characters
 * appearing in VALID_XFN_CHARS.  The status files in archive_status have
 * corresponding names with ".ready" or ".done" appended.
 * ----------
 */
#define MIN_XFN_CHARS	16
#define MAX_XFN_CHARS	40
#define VALID_XFN_CHARS "0123456789ABCDEF.history.backup"

#define NUM_ARCHIVE_RETRIES 3


/* ----------
 * Local data
 * ----------
 */
static time_t last_pgarch_start_time;

/*
 * Flags set by interrupt handlers for later service in the main loop.
 */
static volatile sig_atomic_t got_SIGHUP = false;
static volatile sig_atomic_t wakened = false;

/* ----------
 * Local function forward declarations
 * ----------
 */
#ifdef EXEC_BACKEND
static pid_t pgarch_forkexec(void);
#endif

NON_EXEC_STATIC void PgArchiverMain(int argc, char *argv[]);
static void pgarch_exit(SIGNAL_ARGS);
static void ArchSigHupHandler(SIGNAL_ARGS);
static void pgarch_waken(SIGNAL_ARGS);
static void pgarch_MainLoop(void);
static void pgarch_ArchiverCopyLoop(void);
static bool pgarch_archiveXlog(char *xlog);
static bool pgarch_readyXlog(char *xlog);
static void pgarch_archiveDone(char *xlog);


/* ------------------------------------------------------------
 * Public functions called from postmaster follow
 * ------------------------------------------------------------
 */

/*
 * pgarch_start
 *
 *	Called from postmaster at startup or after an existing archiver
 *	died.  Attempt to fire up a fresh archiver process.
 *
 *	Returns PID of child process, or 0 if fail.
 *
 *	Note: if fail, we will be called again from the postmaster main loop.
 */
int
pgarch_start(void)
{
	time_t		curtime;
	pid_t		pgArchPid;

	/*
	 * Do nothing if no archiver needed
	 */
	if (!XLogArchivingActive())
		return 0;

	/*
	 * Do nothing if too soon since last archiver start.  This is a safety
	 * valve to protect against continuous respawn attempts if the archiver is
	 * dying immediately at launch. Note that since we will be re-called from
	 * the postmaster main loop, we will get another chance later.
	 */
	curtime = time(NULL);
	if ((unsigned int) (curtime - last_pgarch_start_time) <
		(unsigned int) PGARCH_RESTART_INTERVAL)
		return 0;
	last_pgarch_start_time = curtime;

#ifdef EXEC_BACKEND
	switch ((pgArchPid = pgarch_forkexec()))
#else
	switch ((pgArchPid = fork_process()))
#endif
	{
		case -1:
			ereport(LOG,
					(errmsg("could not fork archiver: %m")));
			return 0;

#ifndef EXEC_BACKEND
		case 0:
			/* in postmaster child ... */
			/* Close the postmaster's sockets */
			ClosePostmasterPorts(false);

			/* Lose the postmaster's on-exit routines */
			on_exit_reset();

			/* Drop our connection to postmaster's shared memory, as well */
			PGSharedMemoryDetach();

			PgArchiverMain(0, NULL);
			break;
#endif

		default:
			return (int) pgArchPid;
	}

	/* shouldn't get here */
	return 0;
}

/* ------------------------------------------------------------
 * Local functions called by archiver follow
 * ------------------------------------------------------------
 */


#ifdef EXEC_BACKEND

/*
 * pgarch_forkexec() -
 *
 * Format up the arglist for, then fork and exec, archive process
 */
static pid_t
pgarch_forkexec(void)
{
	char	   *av[10];
	int			ac = 0;

	av[ac++] = "postgres";

	av[ac++] = "--forkarch";

	av[ac++] = NULL;			/* filled in by postmaster_forkexec */

	av[ac] = NULL;
	Assert(ac < lengthof(av));

	return postmaster_forkexec(ac, av);
}
#endif   /* EXEC_BACKEND */


/*
 * PgArchiverMain
 *
 *	The argc/argv parameters are valid only in EXEC_BACKEND case.  However,
 *	since we don't use 'em, it hardly matters...
 */
NON_EXEC_STATIC void
PgArchiverMain(int argc, char *argv[])
{
	IsUnderPostmaster = true;	/* we are a postmaster subprocess now */

	MyProcPid = getpid();		/* reset MyProcPid */

	/*
	 * If possible, make this process a group leader, so that the postmaster
	 * can signal any child processes too.
	 */
#ifdef HAVE_SETSID
	if (setsid() < 0)
		elog(FATAL, "setsid() failed: %m");
#endif

	/*
	 * Ignore all signals usually bound to some action in the postmaster,
	 * except for SIGHUP, SIGUSR1 and SIGQUIT.
	 */
	pqsignal(SIGHUP, ArchSigHupHandler);
	pqsignal(SIGINT, SIG_IGN);
	pqsignal(SIGTERM, SIG_IGN);
	pqsignal(SIGQUIT, pgarch_exit);
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, pgarch_waken);
	pqsignal(SIGUSR2, SIG_IGN);
	pqsignal(SIGCHLD, SIG_DFL);
	pqsignal(SIGTTIN, SIG_DFL);
	pqsignal(SIGTTOU, SIG_DFL);
	pqsignal(SIGCONT, SIG_DFL);
	pqsignal(SIGWINCH, SIG_DFL);
	PG_SETMASK(&UnBlockSig);

	/*
	 * Identify myself via ps
	 */
	init_ps_display("archiver process", "", "", "");

	pgarch_MainLoop();

	exit(0);
}

/* SIGQUIT signal handler for archiver process */
static void
pgarch_exit(SIGNAL_ARGS)
{
	/*
	 * For now, we just nail the doors shut and get out of town.  It might
	 * seem cleaner to finish up any pending archive copies, but there's a
	 * nontrivial risk that init will kill us partway through.
	 */
	exit(0);
}

/* SIGHUP: set flag to re-read config file at next convenient time */
static void
ArchSigHupHandler(SIGNAL_ARGS)
{
	got_SIGHUP = true;
}

/* SIGUSR1 signal handler for archiver process */
static void
pgarch_waken(SIGNAL_ARGS)
{
	wakened = true;
}

/*
 * pgarch_MainLoop
 *
 * Main loop for archiver
 */
static void
pgarch_MainLoop(void)
{
	time_t		last_copy_time = 0;

	/*
	 * We run the copy loop immediately upon entry, in case there are
	 * unarchived files left over from a previous database run (or maybe the
	 * archiver died unexpectedly).  After that we wait for a signal or
	 * timeout before doing more.
	 */
	wakened = true;

	do
	{
		/* Check for config update */
		if (got_SIGHUP)
		{
			got_SIGHUP = false;
			ProcessConfigFile(PGC_SIGHUP);
			if (!XLogArchivingActive())
				break;			/* user wants us to shut down */
		}

		/* Do what we're here for */
		if (wakened)
		{
			wakened = false;
			pgarch_ArchiverCopyLoop();
			last_copy_time = time(NULL);
		}

		/*
		 * There shouldn't be anything for the archiver to do except to wait
		 * for a signal ... however, the archiver exists to protect our data,
		 * so she wakes up occasionally to allow herself to be proactive.
		 *
		 * On some platforms, signals won't interrupt the sleep.  To ensure we
		 * respond reasonably promptly when someone signals us, break down the
		 * sleep into 1-second increments, and check for interrupts after each
		 * nap.
		 */
		while (!(wakened || got_SIGHUP))
		{
			time_t		curtime;

			pg_usleep(1000000L);
			curtime = time(NULL);
			if ((unsigned int) (curtime - last_copy_time) >=
				(unsigned int) PGARCH_AUTOWAKE_INTERVAL)
				wakened = true;
		}
	} while (PostmasterIsAlive(true));
}

/*
 * pgarch_ArchiverCopyLoop
 *
 * Archives all outstanding xlogs then returns
 */
static void
pgarch_ArchiverCopyLoop(void)
{
	char		xlog[MAX_XFN_CHARS + 1];

	/*
	 * loop through all xlogs with archive_status of .ready and archive
	 * them...mostly we expect this to be a single file, though it is possible
	 * some backend will add files onto the list of those that need archiving
	 * while we are still copying earlier archives
	 */
	while (pgarch_readyXlog(xlog))
	{
		int			failures = 0;

		for (;;)
		{
			/* Abandon processing if we notice our postmaster has died */
			if (!PostmasterIsAlive(true))
				return;

			if (pgarch_archiveXlog(xlog))
			{
				/* successful */
				pgarch_archiveDone(xlog);
				break;			/* out of inner retry loop */
			}
			else
			{
				if (++failures >= NUM_ARCHIVE_RETRIES)
				{
					ereport(WARNING,
							(errmsg("transaction log file \"%s\" could not be archived: too many failures",
									xlog)));
					return;		/* give up archiving for now */
				}
				pg_usleep(1000000L);	/* wait a bit before retrying */
			}
		}
	}
}

/*
 * pgarch_archiveXlog
 *
 * Invokes system(3) to copy one archive file to wherever it should go
 *
 * Returns true if successful
 */
static bool
pgarch_archiveXlog(char *xlog)
{
	char		xlogarchcmd[MAXPGPATH];
	char		pathname[MAXPGPATH];
	char	   *dp;
	char	   *endp;
	const char *sp;
	int			rc;

	snprintf(pathname, MAXPGPATH, XLOGDIR "/%s", xlog);

	/*
	 * construct the command to be executed
	 */
	dp = xlogarchcmd;
	endp = xlogarchcmd + MAXPGPATH - 1;
	*endp = '\0';

	for (sp = XLogArchiveCommand; *sp; sp++)
	{
		if (*sp == '%')
		{
			switch (sp[1])
			{
				case 'p':
					/* %p: relative path of source file */
					sp++;
					StrNCpy(dp, pathname, endp - dp);
					make_native_path(dp);
					dp += strlen(dp);
					break;
				case 'f':
					/* %f: filename of source file */
					sp++;
					StrNCpy(dp, xlog, endp - dp);
					dp += strlen(dp);
					break;
				case '%':
					/* convert %% to a single % */
					sp++;
					if (dp < endp)
						*dp++ = *sp;
					break;
				default:
					/* otherwise treat the % as not special */
					if (dp < endp)
						*dp++ = *sp;
					break;
			}
		}
		else
		{
			if (dp < endp)
				*dp++ = *sp;
		}
	}
	*dp = '\0';

	ereport(DEBUG3,
			(errmsg_internal("executing archive command \"%s\"",
							 xlogarchcmd)));
	rc = system(xlogarchcmd);
	if (rc != 0)
	{
		/*
		 * If either the shell itself, or a called command, died on a signal,
		 * abort the archiver.  We do this because system() ignores SIGINT and
		 * SIGQUIT while waiting; so a signal is very likely something that
		 * should have interrupted us too.  If we overreact it's no big deal,
		 * the postmaster will just start the archiver again.
		 *
		 * Per the Single Unix Spec, shells report exit status > 128 when
		 * a called command died on a signal.
		 */
		bool	signaled = WIFSIGNALED(rc) || WEXITSTATUS(rc) > 128;

		ereport(signaled ? FATAL : LOG,
				(errmsg("archive command \"%s\" failed: return code %d",
						xlogarchcmd, rc)));

		return false;
	}
	ereport(LOG,
			(errmsg("archived transaction log file \"%s\"", xlog)));

	return true;
}

/*
 * pgarch_readyXlog
 *
 * Return name of the oldest xlog file that has not yet been archived.
 * No notification is set that file archiving is now in progress, so
 * this would need to be extended if multiple concurrent archival
 * tasks were created. If a failure occurs, we will completely
 * re-copy the file at the next available opportunity.
 *
 * It is important that we return the oldest, so that we archive xlogs
 * in order that they were written, for two reasons:
 * 1) to maintain the sequential chain of xlogs required for recovery
 * 2) because the oldest ones will sooner become candidates for
 * recycling at time of checkpoint
 *
 * NOTE: the "oldest" comparison will presently consider all segments of
 * a timeline with a smaller ID to be older than all segments of a timeline
 * with a larger ID; the net result being that past timelines are given
 * higher priority for archiving.  This seems okay, or at least not
 * obviously worth changing.
 */
static bool
pgarch_readyXlog(char *xlog)
{
	/*
	 * open xlog status directory and read through list of xlogs that have the
	 * .ready suffix, looking for earliest file. It is possible to optimise
	 * this code, though only a single file is expected on the vast majority
	 * of calls, so....
	 */
	char		XLogArchiveStatusDir[MAXPGPATH];
	char		newxlog[MAX_XFN_CHARS + 6 + 1];
	DIR		   *rldir;
	struct dirent *rlde;
	bool		found = false;

	snprintf(XLogArchiveStatusDir, MAXPGPATH, XLOGDIR "/archive_status");
	rldir = AllocateDir(XLogArchiveStatusDir);
	if (rldir == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open archive status directory \"%s\": %m",
						XLogArchiveStatusDir)));

	while ((rlde = ReadDir(rldir, XLogArchiveStatusDir)) != NULL)
	{
		int			basenamelen = (int) strlen(rlde->d_name) - 6;

		if (basenamelen >= MIN_XFN_CHARS &&
			basenamelen <= MAX_XFN_CHARS &&
			strspn(rlde->d_name, VALID_XFN_CHARS) >= basenamelen &&
			strcmp(rlde->d_name + basenamelen, ".ready") == 0)
		{
			if (!found)
			{
				strcpy(newxlog, rlde->d_name);
				found = true;
			}
			else
			{
				if (strcmp(rlde->d_name, newxlog) < 0)
					strcpy(newxlog, rlde->d_name);
			}
		}
	}
	FreeDir(rldir);

	if (found)
	{
		/* truncate off the .ready */
		newxlog[strlen(newxlog) - 6] = '\0';
		strcpy(xlog, newxlog);
	}
	return found;
}

/*
 * pgarch_archiveDone
 *
 * Emit notification that an xlog file has been successfully archived.
 * We do this by renaming the status file from NNN.ready to NNN.done.
 * Eventually, a checkpoint process will notice this and delete both the
 * NNN.done file and the xlog file itself.
 */
static void
pgarch_archiveDone(char *xlog)
{
	char		rlogready[MAXPGPATH];
	char		rlogdone[MAXPGPATH];

	StatusFilePath(rlogready, xlog, ".ready");
	StatusFilePath(rlogdone, xlog, ".done");
	if (rename(rlogready, rlogdone) < 0)
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						rlogready, rlogdone)));
}
