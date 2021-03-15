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
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/pgarch.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/interrupt.h"
#include "postmaster/pgarch.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/pmsignal.h"
#include "storage/procsignal.h"
#include "utils/guc.h"
#include "utils/ps_status.h"


/* ----------
 * Timer definitions.
 * ----------
 */
#define PGARCH_AUTOWAKE_INTERVAL 60 /* How often to force a poll of the
									 * archive status directory; in seconds. */
#define PGARCH_RESTART_INTERVAL 10	/* How often to attempt to restart a
									 * failed archiver; in seconds. */

/*
 * Maximum number of retries allowed when attempting to archive a WAL
 * file.
 */
#define NUM_ARCHIVE_RETRIES 3

/*
 * Maximum number of retries allowed when attempting to remove an
 * orphan archive status file.
 */
#define NUM_ORPHAN_CLEANUP_RETRIES 3

/* Shared memory area for archiver process */
typedef struct PgArchData
{
	int			pgprocno;		/* pgprocno of archiver process */
} PgArchData;


/* ----------
 * Local data
 * ----------
 */
static time_t last_sigterm_time = 0;
static PgArchData *PgArch = NULL;

/*
 * Flags set by interrupt handlers for later service in the main loop.
 */
static volatile sig_atomic_t ready_to_stop = false;

/* ----------
 * Local function forward declarations
 * ----------
 */
static void pgarch_waken_stop(SIGNAL_ARGS);
static void pgarch_MainLoop(void);
static void pgarch_ArchiverCopyLoop(void);
static bool pgarch_archiveXlog(char *xlog);
static bool pgarch_readyXlog(char *xlog);
static void pgarch_archiveDone(char *xlog);
static void pgarch_die(int code, Datum arg);

/* Report shared memory space needed by PgArchShmemInit */
Size
PgArchShmemSize(void)
{
	Size		size = 0;

	size = add_size(size, sizeof(PgArchData));

	return size;
}

/* Allocate and initialize archiver-related shared memory */
void
PgArchShmemInit(void)
{
	bool		found;

	PgArch = (PgArchData *)
		ShmemInitStruct("Archiver Data", PgArchShmemSize(), &found);

	if (!found)
	{
		/* First time through, so initialize */
		MemSet(PgArch, 0, PgArchShmemSize());
		PgArch->pgprocno = INVALID_PGPROCNO;
	}
}

/*
 * PgArchCanRestart
 *
 * Return true and archiver is allowed to restart if enough time has
 * passed since it was launched last to reach PGARCH_RESTART_INTERVAL.
 * Otherwise return false.
 *
 * This is a safety valve to protect against continuous respawn attempts if the
 * archiver is dying immediately at launch. Note that since we will retry to
 * launch the archiver from the postmaster main loop, we will get another
 * chance later.
 */
bool
PgArchCanRestart(void)
{
	static time_t last_pgarch_start_time = 0;
	time_t		curtime = time(NULL);

	/*
	 * Return false and don't restart archiver if too soon since last archiver
	 * start.
	 */
	if ((unsigned int) (curtime - last_pgarch_start_time) <
		(unsigned int) PGARCH_RESTART_INTERVAL)
		return false;

	last_pgarch_start_time = curtime;
	return true;
}


/* Main entry point for archiver process */
void
PgArchiverMain(void)
{
	/*
	 * Ignore all signals usually bound to some action in the postmaster,
	 * except for SIGHUP, SIGTERM, SIGUSR1, SIGUSR2, and SIGQUIT.
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGINT, SIG_IGN);
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
	/* SIGQUIT handler was already set up by InitPostmasterChild */
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, pgarch_waken_stop);

	/* Reset some signals that are accepted by postmaster but not here */
	pqsignal(SIGCHLD, SIG_DFL);

	/* Unblock signals (they were blocked when the postmaster forked us) */
	PG_SETMASK(&UnBlockSig);

	/* We shouldn't be launched unnecessarily. */
	Assert(XLogArchivingActive());

	/* Arrange to clean up at archiver exit */
	on_shmem_exit(pgarch_die, 0);

	/*
	 * Advertise our pgprocno so that backends can use our latch to wake us up
	 * while we're sleeping.
	 */
	PgArch->pgprocno = MyProc->pgprocno;

	pgarch_MainLoop();

	proc_exit(0);
}

/*
 * Wake up the archiver
 */
void
PgArchWakeup(void)
{
	int			arch_pgprocno = PgArch->pgprocno;

	/*
	 * We don't acquire ProcArrayLock here.  It's actually fine because
	 * procLatch isn't ever freed, so we just can potentially set the wrong
	 * process' (or no process') latch.  Even in that case the archiver will
	 * be relaunched shortly and will start archiving.
	 */
	if (arch_pgprocno != INVALID_PGPROCNO)
		SetLatch(&ProcGlobal->allProcs[arch_pgprocno].procLatch);
}


/* SIGUSR2 signal handler for archiver process */
static void
pgarch_waken_stop(SIGNAL_ARGS)
{
	int			save_errno = errno;

	/* set flag to do a final cycle and shut down afterwards */
	ready_to_stop = true;
	SetLatch(MyLatch);

	errno = save_errno;
}

/*
 * pgarch_MainLoop
 *
 * Main loop for archiver
 */
static void
pgarch_MainLoop(void)
{
	pg_time_t	last_copy_time = 0;
	bool		time_to_stop;

	/*
	 * There shouldn't be anything for the archiver to do except to wait for a
	 * signal ... however, the archiver exists to protect our data, so she
	 * wakes up occasionally to allow herself to be proactive.
	 */
	do
	{
		ResetLatch(MyLatch);

		/* When we get SIGUSR2, we do one more archive cycle, then exit */
		time_to_stop = ready_to_stop;

		/* Check for config update */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/*
		 * If we've gotten SIGTERM, we normally just sit and do nothing until
		 * SIGUSR2 arrives.  However, that means a random SIGTERM would
		 * disable archiving indefinitely, which doesn't seem like a good
		 * idea.  If more than 60 seconds pass since SIGTERM, exit anyway, so
		 * that the postmaster can start a new archiver if needed.
		 */
		if (ShutdownRequestPending)
		{
			time_t		curtime = time(NULL);

			if (last_sigterm_time == 0)
				last_sigterm_time = curtime;
			else if ((unsigned int) (curtime - last_sigterm_time) >=
					 (unsigned int) 60)
				break;
		}

		/* Do what we're here for */
		pgarch_ArchiverCopyLoop();
		last_copy_time = time(NULL);

		/*
		 * Sleep until a signal is received, or until a poll is forced by
		 * PGARCH_AUTOWAKE_INTERVAL having passed since last_copy_time, or
		 * until postmaster dies.
		 */
		if (!time_to_stop)		/* Don't wait during last iteration */
		{
			pg_time_t	curtime = (pg_time_t) time(NULL);
			int			timeout;

			timeout = PGARCH_AUTOWAKE_INTERVAL - (curtime - last_copy_time);
			if (timeout > 0)
			{
				int			rc;

				rc = WaitLatch(MyLatch,
							   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
							   timeout * 1000L,
							   WAIT_EVENT_ARCHIVER_MAIN);
				if (rc & WL_POSTMASTER_DEATH)
					time_to_stop = true;
			}
		}

		/*
		 * The archiver quits either when the postmaster dies (not expected)
		 * or after completing one more archiving cycle after receiving
		 * SIGUSR2.
		 */
	} while (!time_to_stop);
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
		int			failures_orphan = 0;

		for (;;)
		{
			struct stat stat_buf;
			char		pathname[MAXPGPATH];

			/*
			 * Do not initiate any more archive commands after receiving
			 * SIGTERM, nor after the postmaster has died unexpectedly. The
			 * first condition is to try to keep from having init SIGKILL the
			 * command, and the second is to avoid conflicts with another
			 * archiver spawned by a newer postmaster.
			 */
			if (ShutdownRequestPending || !PostmasterIsAlive())
				return;

			/*
			 * Check for config update.  This is so that we'll adopt a new
			 * setting for archive_command as soon as possible, even if there
			 * is a backlog of files to be archived.
			 */
			if (ConfigReloadPending)
			{
				ConfigReloadPending = false;
				ProcessConfigFile(PGC_SIGHUP);
			}

			/* can't do anything if no command ... */
			if (!XLogArchiveCommandSet())
			{
				ereport(WARNING,
						(errmsg("archive_mode enabled, yet archive_command is not set")));
				return;
			}

			/*
			 * Since archive status files are not removed in a durable manner,
			 * a system crash could leave behind .ready files for WAL segments
			 * that have already been recycled or removed.  In this case,
			 * simply remove the orphan status file and move on.  unlink() is
			 * used here as even on subsequent crashes the same orphan files
			 * would get removed, so there is no need to worry about
			 * durability.
			 */
			snprintf(pathname, MAXPGPATH, XLOGDIR "/%s", xlog);
			if (stat(pathname, &stat_buf) != 0 && errno == ENOENT)
			{
				char		xlogready[MAXPGPATH];

				StatusFilePath(xlogready, xlog, ".ready");
				if (unlink(xlogready) == 0)
				{
					ereport(WARNING,
							(errmsg("removed orphan archive status file \"%s\"",
									xlogready)));

					/* leave loop and move to the next status file */
					break;
				}

				if (++failures_orphan >= NUM_ORPHAN_CLEANUP_RETRIES)
				{
					ereport(WARNING,
							(errmsg("removal of orphan archive status file \"%s\" failed too many times, will try again later",
									xlogready)));

					/* give up cleanup of orphan status files */
					return;
				}

				/* wait a bit before retrying */
				pg_usleep(1000000L);
				continue;
			}

			if (pgarch_archiveXlog(xlog))
			{
				/* successful */
				pgarch_archiveDone(xlog);

				/*
				 * Tell the collector about the WAL file that we successfully
				 * archived
				 */
				pgstat_send_archiver(xlog, false);

				break;			/* out of inner retry loop */
			}
			else
			{
				/*
				 * Tell the collector about the WAL file that we failed to
				 * archive
				 */
				pgstat_send_archiver(xlog, true);

				if (++failures >= NUM_ARCHIVE_RETRIES)
				{
					ereport(WARNING,
							(errmsg("archiving write-ahead log file \"%s\" failed too many times, will try again later",
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
	char		activitymsg[MAXFNAMELEN + 16];
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
					strlcpy(dp, pathname, endp - dp);
					make_native_path(dp);
					dp += strlen(dp);
					break;
				case 'f':
					/* %f: filename of source file */
					sp++;
					strlcpy(dp, xlog, endp - dp);
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

	/* Report archive activity in PS display */
	snprintf(activitymsg, sizeof(activitymsg), "archiving %s", xlog);
	set_ps_display(activitymsg);

	rc = system(xlogarchcmd);
	if (rc != 0)
	{
		/*
		 * If either the shell itself, or a called command, died on a signal,
		 * abort the archiver.  We do this because system() ignores SIGINT and
		 * SIGQUIT while waiting; so a signal is very likely something that
		 * should have interrupted us too.  Also die if the shell got a hard
		 * "command not found" type of error.  If we overreact it's no big
		 * deal, the postmaster will just start the archiver again.
		 */
		int			lev = wait_result_is_any_signal(rc, true) ? FATAL : LOG;

		if (WIFEXITED(rc))
		{
			ereport(lev,
					(errmsg("archive command failed with exit code %d",
							WEXITSTATUS(rc)),
					 errdetail("The failed archive command was: %s",
							   xlogarchcmd)));
		}
		else if (WIFSIGNALED(rc))
		{
#if defined(WIN32)
			ereport(lev,
					(errmsg("archive command was terminated by exception 0x%X",
							WTERMSIG(rc)),
					 errhint("See C include file \"ntstatus.h\" for a description of the hexadecimal value."),
					 errdetail("The failed archive command was: %s",
							   xlogarchcmd)));
#else
			ereport(lev,
					(errmsg("archive command was terminated by signal %d: %s",
							WTERMSIG(rc), pg_strsignal(WTERMSIG(rc))),
					 errdetail("The failed archive command was: %s",
							   xlogarchcmd)));
#endif
		}
		else
		{
			ereport(lev,
					(errmsg("archive command exited with unrecognized status %d",
							rc),
					 errdetail("The failed archive command was: %s",
							   xlogarchcmd)));
		}

		snprintf(activitymsg, sizeof(activitymsg), "failed on %s", xlog);
		set_ps_display(activitymsg);

		return false;
	}
	elog(DEBUG1, "archived write-ahead log file \"%s\"", xlog);

	snprintf(activitymsg, sizeof(activitymsg), "last was %s", xlog);
	set_ps_display(activitymsg);

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
 * NOTE: the "oldest" comparison will consider any .history file to be older
 * than any other file except another .history file.  Segments on a timeline
 * with a smaller ID will be older than all segments on a timeline with a
 * larger ID; the net result being that past timelines are given higher
 * priority for archiving.  This seems okay, or at least not obviously worth
 * changing.
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
	DIR		   *rldir;
	struct dirent *rlde;
	bool		found = false;
	bool		historyFound = false;

	snprintf(XLogArchiveStatusDir, MAXPGPATH, XLOGDIR "/archive_status");
	rldir = AllocateDir(XLogArchiveStatusDir);

	while ((rlde = ReadDir(rldir, XLogArchiveStatusDir)) != NULL)
	{
		int			basenamelen = (int) strlen(rlde->d_name) - 6;
		char		basename[MAX_XFN_CHARS + 1];
		bool		ishistory;

		/* Ignore entries with unexpected number of characters */
		if (basenamelen < MIN_XFN_CHARS ||
			basenamelen > MAX_XFN_CHARS)
			continue;

		/* Ignore entries with unexpected characters */
		if (strspn(rlde->d_name, VALID_XFN_CHARS) < basenamelen)
			continue;

		/* Ignore anything not suffixed with .ready */
		if (strcmp(rlde->d_name + basenamelen, ".ready") != 0)
			continue;

		/* Truncate off the .ready */
		memcpy(basename, rlde->d_name, basenamelen);
		basename[basenamelen] = '\0';

		/* Is this a history file? */
		ishistory = IsTLHistoryFileName(basename);

		/*
		 * Consume the file to archive.  History files have the highest
		 * priority.  If this is the first file or the first history file
		 * ever, copy it.  In the presence of a history file already chosen as
		 * target, ignore all other files except history files which have been
		 * generated for an older timeline than what is already chosen as
		 * target to archive.
		 */
		if (!found || (ishistory && !historyFound))
		{
			strcpy(xlog, basename);
			found = true;
			historyFound = ishistory;
		}
		else if (ishistory || !historyFound)
		{
			if (strcmp(basename, xlog) < 0)
				strcpy(xlog, basename);
		}
	}
	FreeDir(rldir);

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
	(void) durable_rename(rlogready, rlogdone, WARNING);
}


/*
 * pgarch_die
 *
 * Exit-time cleanup handler
 */
static void
pgarch_die(int code, Datum arg)
{
	PgArch->pgprocno = INVALID_PGPROCNO;
}
