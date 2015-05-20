/*-------------------------------------------------------------------------
 *
 * bgwriter.c
 *
 * The background writer (bgwriter) is new as of Postgres 8.0.  It attempts
 * to keep regular backends from having to write out dirty shared buffers
 * (which they would only do when needing to free a shared buffer to read in
 * another page).  In the best scenario all writes from shared buffers will
 * be issued by the background writer process.  However, regular backends are
 * still empowered to issue writes if the bgwriter fails to maintain enough
 * clean shared buffers.
 *
 * As of Postgres 9.2 the bgwriter no longer handles checkpoints.
 *
 * The bgwriter is started by the postmaster as soon as the startup subprocess
 * finishes, or as soon as recovery begins if we are doing archive recovery.
 * It remains alive until the postmaster commands it to terminate.
 * Normal termination is by SIGTERM, which instructs the bgwriter to exit(0).
 * Emergency termination is by SIGQUIT; like any backend, the bgwriter will
 * simply abort and exit on SIGQUIT.
 *
 * If the bgwriter exits unexpectedly, the postmaster treats that the same
 * as a backend crash: shared memory may be corrupted, so remaining backends
 * should be killed by SIGQUIT and then a recovery cycle started.
 *
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/bgwriter.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <sys/time.h>
#include <unistd.h>

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgwriter.h"
#include "storage/bufmgr.h"
#include "storage/buf_internals.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "storage/smgr.h"
#include "storage/spin.h"
#include "storage/standby.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/timestamp.h"


/*
 * GUC parameters
 */
int			BgWriterDelay = 200;

/*
 * Multiplier to apply to BgWriterDelay when we decide to hibernate.
 * (Perhaps this needs to be configurable?)
 */
#define HIBERNATE_FACTOR			50

/*
 * Interval in which standby snapshots are logged into the WAL stream, in
 * milliseconds.
 */
#define LOG_SNAPSHOT_INTERVAL_MS 15000

/*
 * LSN and timestamp at which we last issued a LogStandbySnapshot(), to avoid
 * doing so too often or repeatedly if there has been no other write activity
 * in the system.
 */
static TimestampTz last_snapshot_ts;
static XLogRecPtr last_snapshot_lsn = InvalidXLogRecPtr;

/*
 * Flags set by interrupt handlers for later service in the main loop.
 */
static volatile sig_atomic_t got_SIGHUP = false;
static volatile sig_atomic_t shutdown_requested = false;

/* Signal handlers */

static void bg_quickdie(SIGNAL_ARGS);
static void BgSigHupHandler(SIGNAL_ARGS);
static void ReqShutdownHandler(SIGNAL_ARGS);
static void bgwriter_sigusr1_handler(SIGNAL_ARGS);


/*
 * Main entry point for bgwriter process
 *
 * This is invoked from AuxiliaryProcessMain, which has already created the
 * basic execution environment, but not enabled signals yet.
 */
void
BackgroundWriterMain(void)
{
	sigjmp_buf	local_sigjmp_buf;
	MemoryContext bgwriter_context;
	bool		prev_hibernate;

	/*
	 * Properly accept or ignore signals the postmaster might send us.
	 *
	 * bgwriter doesn't participate in ProcSignal signalling, but a SIGUSR1
	 * handler is still needed for latch wakeups.
	 */
	pqsignal(SIGHUP, BgSigHupHandler);	/* set flag to read config file */
	pqsignal(SIGINT, SIG_IGN);
	pqsignal(SIGTERM, ReqShutdownHandler);		/* shutdown */
	pqsignal(SIGQUIT, bg_quickdie);		/* hard crash time */
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, bgwriter_sigusr1_handler);
	pqsignal(SIGUSR2, SIG_IGN);

	/*
	 * Reset some signals that are accepted by postmaster but not here
	 */
	pqsignal(SIGCHLD, SIG_DFL);
	pqsignal(SIGTTIN, SIG_DFL);
	pqsignal(SIGTTOU, SIG_DFL);
	pqsignal(SIGCONT, SIG_DFL);
	pqsignal(SIGWINCH, SIG_DFL);

	/* We allow SIGQUIT (quickdie) at all times */
	sigdelset(&BlockSig, SIGQUIT);

	/*
	 * Create a resource owner to keep track of our resources (currently only
	 * buffer pins).
	 */
	CurrentResourceOwner = ResourceOwnerCreate(NULL, "Background Writer");

	/*
	 * We just started, assume there has been either a shutdown or
	 * end-of-recovery snapshot.
	 */
	last_snapshot_ts = GetCurrentTimestamp();

	/*
	 * Create a memory context that we will do all our work in.  We do this so
	 * that we can reset the context during error recovery and thereby avoid
	 * possible memory leaks.  Formerly this code just ran in
	 * TopMemoryContext, but resetting that would be a really bad idea.
	 */
	bgwriter_context = AllocSetContextCreate(TopMemoryContext,
											 "Background Writer",
											 ALLOCSET_DEFAULT_MINSIZE,
											 ALLOCSET_DEFAULT_INITSIZE,
											 ALLOCSET_DEFAULT_MAXSIZE);
	MemoryContextSwitchTo(bgwriter_context);

	/*
	 * If an exception is encountered, processing resumes here.
	 *
	 * See notes in postgres.c about the design of this coding.
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* Since not using PG_TRY, must reset error stack by hand */
		error_context_stack = NULL;

		/* Prevent interrupts while cleaning up */
		HOLD_INTERRUPTS();

		/* Report the error to the server log */
		EmitErrorReport();

		/*
		 * These operations are really just a minimal subset of
		 * AbortTransaction().  We don't have very many resources to worry
		 * about in bgwriter, but we do have LWLocks, buffers, and temp files.
		 */
		LWLockReleaseAll();
		AbortBufferIO();
		UnlockBuffers();
		/* buffer pins are released here: */
		ResourceOwnerRelease(CurrentResourceOwner,
							 RESOURCE_RELEASE_BEFORE_LOCKS,
							 false, true);
		/* we needn't bother with the other ResourceOwnerRelease phases */
		AtEOXact_Buffers(false);
		AtEOXact_SMgr();
		AtEOXact_Files();
		AtEOXact_HashTables(false);

		/*
		 * Now return to normal top-level context and clear ErrorContext for
		 * next time.
		 */
		MemoryContextSwitchTo(bgwriter_context);
		FlushErrorState();

		/* Flush any leaked data in the top-level context */
		MemoryContextResetAndDeleteChildren(bgwriter_context);

		/* Now we can allow interrupts again */
		RESUME_INTERRUPTS();

		/*
		 * Sleep at least 1 second after any error.  A write error is likely
		 * to be repeated, and we don't want to be filling the error logs as
		 * fast as we can.
		 */
		pg_usleep(1000000L);

		/*
		 * Close all open files after any error.  This is helpful on Windows,
		 * where holding deleted files open causes various strange errors.
		 * It's not clear we need it elsewhere, but shouldn't hurt.
		 */
		smgrcloseall();
	}

	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	/*
	 * Unblock signals (they were blocked when the postmaster forked us)
	 */
	PG_SETMASK(&UnBlockSig);

	/*
	 * Reset hibernation state after any error.
	 */
	prev_hibernate = false;

	/*
	 * Loop forever
	 */
	for (;;)
	{
		bool		can_hibernate;
		int			rc;

		/* Clear any already-pending wakeups */
		ResetLatch(MyLatch);

		if (got_SIGHUP)
		{
			got_SIGHUP = false;
			ProcessConfigFile(PGC_SIGHUP);
		}
		if (shutdown_requested)
		{
			/*
			 * From here on, elog(ERROR) should end with exit(1), not send
			 * control back to the sigsetjmp block above
			 */
			ExitOnAnyError = true;
			/* Normal exit from the bgwriter is here */
			proc_exit(0);		/* done */
		}

		/*
		 * Do one cycle of dirty-buffer writing.
		 */
		can_hibernate = BgBufferSync();

		/*
		 * Send off activity statistics to the stats collector
		 */
		pgstat_send_bgwriter();

		if (FirstCallSinceLastCheckpoint())
		{
			/*
			 * After any checkpoint, close all smgr files.  This is so we
			 * won't hang onto smgr references to deleted files indefinitely.
			 */
			smgrcloseall();
		}

		/*
		 * Log a new xl_running_xacts every now and then so replication can
		 * get into a consistent state faster (think of suboverflowed
		 * snapshots) and clean up resources (locks, KnownXids*) more
		 * frequently. The costs of this are relatively low, so doing it 4
		 * times (LOG_SNAPSHOT_INTERVAL_MS) a minute seems fine.
		 *
		 * We assume the interval for writing xl_running_xacts is
		 * significantly bigger than BgWriterDelay, so we don't complicate the
		 * overall timeout handling but just assume we're going to get called
		 * often enough even if hibernation mode is active. It's not that
		 * important that log_snap_interval_ms is met strictly. To make sure
		 * we're not waking the disk up unnecessarily on an idle system we
		 * check whether there has been any WAL inserted since the last time
		 * we've logged a running xacts.
		 *
		 * We do this logging in the bgwriter as its the only process thats
		 * run regularly and returns to its mainloop all the time. E.g.
		 * Checkpointer, when active, is barely ever in its mainloop and thus
		 * makes it hard to log regularly.
		 */
		if (XLogStandbyInfoActive() && !RecoveryInProgress())
		{
			TimestampTz timeout = 0;
			TimestampTz now = GetCurrentTimestamp();

			timeout = TimestampTzPlusMilliseconds(last_snapshot_ts,
												  LOG_SNAPSHOT_INTERVAL_MS);

			/*
			 * only log if enough time has passed and some xlog record has
			 * been inserted.
			 */
			if (now >= timeout &&
				last_snapshot_lsn != GetXLogInsertRecPtr())
			{
				last_snapshot_lsn = LogStandbySnapshot();
				last_snapshot_ts = now;
			}
		}

		/*
		 * Sleep until we are signaled or BgWriterDelay has elapsed.
		 *
		 * Note: the feedback control loop in BgBufferSync() expects that we
		 * will call it every BgWriterDelay msec.  While it's not critical for
		 * correctness that that be exact, the feedback loop might misbehave
		 * if we stray too far from that.  Hence, avoid loading this process
		 * down with latch events that are likely to happen frequently during
		 * normal operation.
		 */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   BgWriterDelay /* ms */ );

		/*
		 * If no latch event and BgBufferSync says nothing's happening, extend
		 * the sleep in "hibernation" mode, where we sleep for much longer
		 * than bgwriter_delay says.  Fewer wakeups save electricity.  When a
		 * backend starts using buffers again, it will wake us up by setting
		 * our latch.  Because the extra sleep will persist only as long as no
		 * buffer allocations happen, this should not distort the behavior of
		 * BgBufferSync's control loop too badly; essentially, it will think
		 * that the system-wide idle interval didn't exist.
		 *
		 * There is a race condition here, in that a backend might allocate a
		 * buffer between the time BgBufferSync saw the alloc count as zero
		 * and the time we call StrategyNotifyBgWriter.  While it's not
		 * critical that we not hibernate anyway, we try to reduce the odds of
		 * that by only hibernating when BgBufferSync says nothing's happening
		 * for two consecutive cycles.  Also, we mitigate any possible
		 * consequences of a missed wakeup by not hibernating forever.
		 */
		if (rc == WL_TIMEOUT && can_hibernate && prev_hibernate)
		{
			/* Ask for notification at next buffer allocation */
			StrategyNotifyBgWriter(MyProc->pgprocno);
			/* Sleep ... */
			rc = WaitLatch(MyLatch,
						   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
						   BgWriterDelay * HIBERNATE_FACTOR);
			/* Reset the notification request in case we timed out */
			StrategyNotifyBgWriter(-1);
		}

		/*
		 * Emergency bailout if postmaster has died.  This is to avoid the
		 * necessity for manual cleanup of all postmaster children.
		 */
		if (rc & WL_POSTMASTER_DEATH)
			exit(1);

		prev_hibernate = can_hibernate;
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
	 * We DO NOT want to run proc_exit() callbacks -- we're here because
	 * shared memory may be corrupted, so we don't want to try to clean up our
	 * transaction.  Just nail the windows shut and get out of town.  Now that
	 * there's an atexit callback to prevent third-party code from breaking
	 * things by calling exit() directly, we have to reset the callbacks
	 * explicitly to make this work as intended.
	 */
	on_exit_reset();

	/*
	 * Note we do exit(2) not exit(0).  This is to force the postmaster into a
	 * system reset cycle if some idiot DBA sends a manual SIGQUIT to a random
	 * backend.  This is necessary precisely because we don't clean up our
	 * shared memory state.  (The "dead man switch" mechanism in pmsignal.c
	 * should ensure the postmaster sees this as a crash, too, but no harm in
	 * being doubly sure.)
	 */
	exit(2);
}

/* SIGHUP: set flag to re-read config file at next convenient time */
static void
BgSigHupHandler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_SIGHUP = true;
	SetLatch(MyLatch);

	errno = save_errno;
}

/* SIGTERM: set flag to shutdown and exit */
static void
ReqShutdownHandler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	shutdown_requested = true;
	SetLatch(MyLatch);

	errno = save_errno;
}

/* SIGUSR1: used for latch wakeups */
static void
bgwriter_sigusr1_handler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	latch_sigusr1_handler();

	errno = save_errno;
}
