/*-------------------------------------------------------------------------
 *
 * bgwriter.c
 *
 * The background writer (bgwriter) is new in Postgres 7.5.  It attempts
 * to keep regular backends from having to write out dirty shared buffers
 * (which they would only do when needing to free a shared buffer to read in
 * another page).  In the best scenario all writes from shared buffers will
 * be issued by the background writer process.  However, regular backends are
 * still empowered to issue writes if the bgwriter fails to maintain enough
 * clean shared buffers.
 *
 * The bgwriter is also charged with handling all checkpoints.  It will
 * automatically dispatch a checkpoint after a certain amount of time has
 * elapsed since the last one, and it can be signaled to perform requested
 * checkpoints as well.  (The GUC parameter that mandates a checkpoint every
 * so many WAL segments is implemented by having backends signal the bgwriter
 * when they fill WAL segments; the bgwriter itself doesn't watch for the
 * condition.)
 *
 * The bgwriter is started by the postmaster as soon as the startup subprocess
 * finishes.  It remains alive until the postmaster commands it to terminate.
 * Normal termination is by SIGUSR2, which instructs the bgwriter to execute
 * a shutdown checkpoint and then exit(0).  (All backends must be stopped
 * before SIGUSR2 is issued!)  Emergency termination is by SIGQUIT; like any
 * backend, the bgwriter will simply abort and exit on SIGQUIT.
 *
 * If the bgwriter exits unexpectedly, the postmaster treats that the same
 * as a backend crash: shared memory may be corrupted, so remaining backends
 * should be killed by SIGQUIT and then a recovery cycle started.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/postmaster/bgwriter.c,v 1.1 2004/05/29 22:48:19 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>

#include "access/xlog.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/bgwriter.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/smgr.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"


/*
 * Shared memory area for communication between bgwriter and backends
 */
typedef struct
{
	pid_t	bgwriter_pid;		/* PID of bgwriter (0 if not started) */
	sig_atomic_t	checkpoint_count; /* advances when checkpoint done */
} BgWriterShmemStruct;

static BgWriterShmemStruct *BgWriterShmem;

/*
 * GUC parameters
 */
int			BgWriterDelay = 200;
int			BgWriterPercent = 1;
int			BgWriterMaxPages = 100;

int			CheckPointTimeout = 300;
int			CheckPointWarning = 30;

/*
 * Flags set by interrupt handlers for later service in the main loop.
 */
static volatile sig_atomic_t got_SIGHUP = false;
static volatile sig_atomic_t checkpoint_requested = false;
static volatile sig_atomic_t shutdown_requested = false;

/*
 * Private state
 */
static time_t	last_checkpoint_time;


static void bg_quickdie(SIGNAL_ARGS);
static void BgSigHupHandler(SIGNAL_ARGS);
static void ReqCheckpointHandler(SIGNAL_ARGS);
static void ReqShutdownHandler(SIGNAL_ARGS);


/*
 * Main entry point for bgwriter process
 *
 * This is invoked from BootstrapMain, which has already created the basic
 * execution environment, but not enabled signals yet.
 */
void
BackgroundWriterMain(void)
{
	Assert(BgWriterShmem != NULL);
	BgWriterShmem->bgwriter_pid = MyProcPid;

	/*
	 * Properly accept or ignore signals the postmaster might send us
	 *
	 * Note: we deliberately ignore SIGTERM, because during a standard Unix
	 * system shutdown cycle, init will SIGTERM all processes at once.  We
	 * want to wait for the backends to exit, whereupon the postmaster will
	 * tell us it's okay to shut down (via SIGUSR2).
	 *
	 * SIGUSR1 is presently unused; keep it spare in case someday we want
	 * this process to participate in sinval messaging.
	 */
	pqsignal(SIGHUP, BgSigHupHandler);	/* set flag to read config file */
	pqsignal(SIGINT, ReqCheckpointHandler);		/* request checkpoint */
	pqsignal(SIGTERM, SIG_IGN);			/* ignore SIGTERM */
	pqsignal(SIGQUIT, bg_quickdie);		/* hard crash time */
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, SIG_IGN);			/* reserve for sinval */
	pqsignal(SIGUSR2, ReqShutdownHandler);		/* request shutdown */

	/*
	 * Reset some signals that are accepted by postmaster but not here
	 */
	pqsignal(SIGCHLD, SIG_DFL);
	pqsignal(SIGTTIN, SIG_DFL);
	pqsignal(SIGTTOU, SIG_DFL);
	pqsignal(SIGCONT, SIG_DFL);
	pqsignal(SIGWINCH, SIG_DFL);

	/* We allow SIGQUIT (quickdie) at all times */
#ifdef HAVE_SIGPROCMASK
	sigdelset(&BlockSig, SIGQUIT);
#else
	BlockSig &= ~(sigmask(SIGQUIT));
#endif

	/*
	 * Initialize so that first time-driven checkpoint happens
	 * at the correct time.
	 */
	last_checkpoint_time = time(NULL);

	/*
	 * If an exception is encountered, processing resumes here.
	 */
	if (sigsetjmp(Warn_restart, 1) != 0)
	{
		/*
		 * Make sure we're not interrupted while cleaning up.  Also forget
		 * any pending QueryCancel request, since we're aborting anyway.
		 * Force InterruptHoldoffCount to a known state in case we
		 * ereport'd from inside a holdoff section.
		 */
		ImmediateInterruptOK = false;
		QueryCancelPending = false;
		InterruptHoldoffCount = 1;
		CritSectionCount = 0;	/* should be unnecessary, but... */

		/*
		 * These operations are really just a minimal subset of
		 * AbortTransaction().  We don't have very many resources
		 * to worry about in bgwriter, but we do have LWLocks and buffers.
		 */
		LWLockReleaseAll();
		AbortBufferIO();
		UnlockBuffers();

		/*
		 * Clear flag to indicate that we got out of error recovery mode
		 * successfully.  (Flag was set in elog.c before longjmp().)
		 */
		InError = false;

		/*
		 * Exit interrupt holdoff section we implicitly established above.
		 */
		RESUME_INTERRUPTS();

		/*
		 * Sleep at least 1 second after any error.  A write error is
		 * likely to be repeated, and we don't want to be filling the
		 * error logs as fast as we can.  (XXX think about ways to make
		 * progress when the LRU dirty buffer cannot be written...)
		 */
		pg_usleep(1000000L);
	}

	Warn_restart_ready = true;	/* we can now handle ereport(ERROR) */

	/*
	 * Unblock signals (they were blocked when the postmaster forked us)
	 */
	PG_SETMASK(&UnBlockSig);

	/*
	 * Loop forever 
	 */
	for (;;)
	{
		bool		do_checkpoint = false;
		bool		force_checkpoint = false;
		time_t		now;
		int			elapsed_secs;
		int			n;
		long		udelay;

		/*
		 * Process any signals received recently.
		 */
		if (got_SIGHUP)
		{
			got_SIGHUP = false;
			ProcessConfigFile(PGC_SIGHUP);
		}
		if (checkpoint_requested)
		{
			checkpoint_requested = false;
			do_checkpoint = true;
			force_checkpoint = true;
		}
		if (shutdown_requested)
		{
			ShutdownXLOG(0, 0);
			DumpFreeSpaceMap(0, 0);
			/* Normal exit from the bgwriter is here */
			proc_exit(0);		/* done */
		}

		/*
		 * Do an unforced checkpoint if too much time has elapsed
		 * since the last one.
		 */
		now = time(NULL);
		elapsed_secs = now - last_checkpoint_time;
		if (elapsed_secs >= CheckPointTimeout)
			do_checkpoint = true;

		/*
		 * Do a checkpoint if requested, otherwise do one cycle of
		 * dirty-buffer writing.
		 */
		if (do_checkpoint)
		{
			if (CheckPointWarning != 0)
			{
				/*
				 * Ideally we should only warn if this checkpoint was
				 * requested due to running out of segment files, and not
				 * if it was manually requested.  However we can't tell the
				 * difference with the current signalling mechanism.
				 */
				if (elapsed_secs < CheckPointWarning)
					ereport(LOG,
							(errmsg("checkpoints are occurring too frequently (%d seconds apart)",
									elapsed_secs),
							 errhint("Consider increasing the configuration parameter \"checkpoint_segments\".")));
			}

			CreateCheckPoint(false, force_checkpoint);

			/*
			 * Note we record the checkpoint start time not end time as
			 * last_checkpoint_time.  This is so that time-driven checkpoints
			 * happen at a predictable spacing.
			 */
			last_checkpoint_time = now;

			/*
			 * Indicate checkpoint completion to any waiting backends.
			 */
			BgWriterShmem->checkpoint_count++;

			/*
			 * After any checkpoint, close all smgr files.  This is so we
			 * won't hang onto smgr references to deleted files indefinitely.
			 */
			smgrcloseall();

			/* Nap for configured time before rechecking */
			n = 1;
		}
		else
		{
			n = BufferSync(BgWriterPercent, BgWriterMaxPages);
		}

		/*
		 * Nap for the configured time or sleep for 10 seconds if
		 * there was nothing to do at all.
		 *
		 * On some platforms, signals won't interrupt the sleep.  To ensure
		 * we respond reasonably promptly when someone signals us,
		 * break down the sleep into 1-second increments, and check for
		 * interrupts after each nap.
		 */
		udelay = ((n > 0) ? BgWriterDelay : 10000) * 1000L;
		while (udelay > 1000000L)
		{
			if (got_SIGHUP || checkpoint_requested || shutdown_requested)
				break;
			pg_usleep(1000000L);
			udelay -= 1000000L;
		}
		if (!(got_SIGHUP || checkpoint_requested || shutdown_requested))
			pg_usleep(udelay);

		/*
		 * Emergency bailout if postmaster has died.  This is to avoid the
		 * necessity for manual cleanup of all postmaster children.
		 */
		if (!PostmasterIsAlive(true))
			exit(1);
	}
}


/* --------------------------------
 *		signal handler routines
 * --------------------------------
 */

/*
 * bg_quickdie() occurs when signalled SIGQUIT by the postmaster.
 *
 * Some backend has bought the farm,
 * so we need to stop what we're doing and exit.
 */
static void
bg_quickdie(SIGNAL_ARGS)
{
	PG_SETMASK(&BlockSig);

	/*
	 * DO NOT proc_exit() -- we're here because shared memory may be
	 * corrupted, so we don't want to try to clean up our transaction.
	 * Just nail the windows shut and get out of town.
	 *
	 * Note we do exit(1) not exit(0).	This is to force the postmaster into
	 * a system reset cycle if some idiot DBA sends a manual SIGQUIT to a
	 * random backend.	This is necessary precisely because we don't clean
	 * up our shared memory state.
	 */
	exit(1);
}

/* SIGHUP: set flag to re-read config file at next convenient time */
static void
BgSigHupHandler(SIGNAL_ARGS)
{
	got_SIGHUP = true;
}

/* SIGINT: set flag to run a normal checkpoint right away */
static void
ReqCheckpointHandler(SIGNAL_ARGS)
{
	checkpoint_requested = true;
}

/* SIGUSR2: set flag to run a shutdown checkpoint and exit */
static void
ReqShutdownHandler(SIGNAL_ARGS)
{
	shutdown_requested = true;
}


/* --------------------------------
 *		communication with backends
 * --------------------------------
 */

/*
 * BgWriterShmemSize
 *		Compute space needed for bgwriter-related shared memory
 */
int
BgWriterShmemSize(void)
{
	/*
	 * This is not worth measuring right now, but may become so after we
	 * add fsync signaling ...
	 */
	return MAXALIGN(sizeof(BgWriterShmemStruct));
}

/*
 * BgWriterShmemInit
 *		Allocate and initialize bgwriter-related shared memory
 */
void
BgWriterShmemInit(void)
{
	bool found;

	BgWriterShmem = (BgWriterShmemStruct *)
		ShmemInitStruct("Background Writer Data",
						sizeof(BgWriterShmemStruct),
						&found);
	if (BgWriterShmem == NULL)
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("insufficient shared memory for bgwriter")));
	if (found)
		return;					/* already initialized */

	MemSet(BgWriterShmem, 0, sizeof(BgWriterShmemStruct));
}

/*
 * RequestCheckpoint
 *		Called in backend processes to request an immediate checkpoint
 *
 * If waitforit is true, wait until the checkpoint is completed
 * before returning; otherwise, just signal the request and return
 * immediately.
 */
void
RequestCheckpoint(bool waitforit)
{
	volatile sig_atomic_t *count_ptr = &BgWriterShmem->checkpoint_count;
	sig_atomic_t	old_count = *count_ptr;

	/*
	 * Send signal to request checkpoint.  When waitforit is false,
	 * we consider failure to send the signal to be nonfatal.
	 */
	if (BgWriterShmem->bgwriter_pid == 0)
		elog(waitforit ? ERROR : LOG,
			 "could not request checkpoint because bgwriter not running");
	if (kill(BgWriterShmem->bgwriter_pid, SIGINT) != 0)
		elog(waitforit ? ERROR : LOG,
			 "could not signal for checkpoint: %m");

	/*
	 * If requested, wait for completion.  We detect completion by
	 * observing a change in checkpoint_count in shared memory.
	 */
	if (waitforit)
	{
		while (*count_ptr == old_count)
		{
			CHECK_FOR_INTERRUPTS();
			pg_usleep(1000000L);
		}
	}
}
