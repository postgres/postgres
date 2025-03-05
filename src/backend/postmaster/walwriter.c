/*-------------------------------------------------------------------------
 *
 * walwriter.c
 *
 * The WAL writer background process is new as of Postgres 8.3.  It attempts
 * to keep regular backends from having to write out (and fsync) WAL pages.
 * Also, it guarantees that transaction commit records that weren't synced
 * to disk immediately upon commit (ie, were "asynchronously committed")
 * will reach disk within a knowable time --- which, as it happens, is at
 * most three times the wal_writer_delay cycle time.
 *
 * Note that as with the bgwriter for shared buffers, regular backends are
 * still empowered to issue WAL writes and fsyncs when the walwriter doesn't
 * keep up. This means that the WALWriter is not an essential process and
 * can shutdown quickly when requested.
 *
 * Because the walwriter's cycle is directly linked to the maximum delay
 * before async-commit transactions are guaranteed committed, it's probably
 * unwise to load additional functionality onto it.  For instance, if you've
 * got a yen to create xlog segments further in advance, that'd be better done
 * in bgwriter than in walwriter.
 *
 * The walwriter is started by the postmaster as soon as the startup subprocess
 * finishes.  It remains alive until the postmaster commands it to terminate.
 * Normal termination is by SIGTERM, which instructs the walwriter to exit(0).
 * Emergency termination is by SIGQUIT; like any backend, the walwriter will
 * simply abort and exit on SIGQUIT.
 *
 * If the walwriter exits unexpectedly, the postmaster treats that the same
 * as a backend crash: shared memory may be corrupted, so remaining backends
 * should be killed by SIGQUIT and then a recovery cycle started.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/walwriter.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>

#include "access/xlog.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/auxprocess.h"
#include "postmaster/interrupt.h"
#include "postmaster/walwriter.h"
#include "storage/bufmgr.h"
#include "storage/condition_variable.h"
#include "storage/fd.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/smgr.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/resowner.h"


/*
 * GUC parameters
 */
int			WalWriterDelay = 200;
int			WalWriterFlushAfter = DEFAULT_WAL_WRITER_FLUSH_AFTER;

/*
 * Number of do-nothing loops before lengthening the delay time, and the
 * multiplier to apply to WalWriterDelay when we do decide to hibernate.
 * (Perhaps these need to be configurable?)
 */
#define LOOPS_UNTIL_HIBERNATE		50
#define HIBERNATE_FACTOR			25

/*
 * Main entry point for walwriter process
 *
 * This is invoked from AuxiliaryProcessMain, which has already created the
 * basic execution environment, but not enabled signals yet.
 */
void
WalWriterMain(const void *startup_data, size_t startup_data_len)
{
	sigjmp_buf	local_sigjmp_buf;
	MemoryContext walwriter_context;
	int			left_till_hibernate;
	bool		hibernating;

	Assert(startup_data_len == 0);

	MyBackendType = B_WAL_WRITER;
	AuxiliaryProcessMainCommon();

	/*
	 * Properly accept or ignore signals the postmaster might send us
	 *
	 * We have no particular use for SIGINT at the moment, but seems
	 * reasonable to treat like SIGTERM.
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGINT, SignalHandlerForShutdownRequest);
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
	/* SIGQUIT handler was already set up by InitPostmasterChild */
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, SIG_IGN); /* not used */

	/*
	 * Reset some signals that are accepted by postmaster but not here
	 */
	pqsignal(SIGCHLD, SIG_DFL);

	/*
	 * Create a memory context that we will do all our work in.  We do this so
	 * that we can reset the context during error recovery and thereby avoid
	 * possible memory leaks.  Formerly this code just ran in
	 * TopMemoryContext, but resetting that would be a really bad idea.
	 */
	walwriter_context = AllocSetContextCreate(TopMemoryContext,
											  "Wal Writer",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(walwriter_context);

	/*
	 * If an exception is encountered, processing resumes here.
	 *
	 * You might wonder why this isn't coded as an infinite loop around a
	 * PG_TRY construct.  The reason is that this is the bottom of the
	 * exception stack, and so with PG_TRY there would be no exception handler
	 * in force at all during the CATCH part.  By leaving the outermost setjmp
	 * always active, we have at least some chance of recovering from an error
	 * during error recovery.  (If we get into an infinite loop thereby, it
	 * will soon be stopped by overflow of elog.c's internal state stack.)
	 *
	 * Note that we use sigsetjmp(..., 1), so that the prevailing signal mask
	 * (to wit, BlockSig) will be restored when longjmp'ing to here.  Thus,
	 * signals other than SIGQUIT will be blocked until we complete error
	 * recovery.  It might seem that this policy makes the HOLD_INTERRUPTS()
	 * call redundant, but it is not since InterruptPending might be set
	 * already.
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
		 * about in walwriter, but we do have LWLocks, and perhaps buffers?
		 */
		LWLockReleaseAll();
		ConditionVariableCancelSleep();
		pgstat_report_wait_end();
		UnlockBuffers();
		ReleaseAuxProcessResources(false);
		AtEOXact_Buffers(false);
		AtEOXact_SMgr();
		AtEOXact_Files(false);
		AtEOXact_HashTables(false);

		/*
		 * Now return to normal top-level context and clear ErrorContext for
		 * next time.
		 */
		MemoryContextSwitchTo(walwriter_context);
		FlushErrorState();

		/* Flush any leaked data in the top-level context */
		MemoryContextReset(walwriter_context);

		/* Now we can allow interrupts again */
		RESUME_INTERRUPTS();

		/*
		 * Sleep at least 1 second after any error.  A write error is likely
		 * to be repeated, and we don't want to be filling the error logs as
		 * fast as we can.
		 */
		pg_usleep(1000000L);
	}

	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	/*
	 * Unblock signals (they were blocked when the postmaster forked us)
	 */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/*
	 * Reset hibernation state after any error.
	 */
	left_till_hibernate = LOOPS_UNTIL_HIBERNATE;
	hibernating = false;
	SetWalWriterSleeping(false);

	/*
	 * Advertise our proc number that backends can use to wake us up while
	 * we're sleeping.
	 */
	ProcGlobal->walwriterProc = MyProcNumber;

	/*
	 * Loop forever
	 */
	for (;;)
	{
		long		cur_timeout;

		/*
		 * Advertise whether we might hibernate in this cycle.  We do this
		 * before resetting the latch to ensure that any async commits will
		 * see the flag set if they might possibly need to wake us up, and
		 * that we won't miss any signal they send us.  (If we discover work
		 * to do in the last cycle before we would hibernate, the global flag
		 * will be set unnecessarily, but little harm is done.)  But avoid
		 * touching the global flag if it doesn't need to change.
		 */
		if (hibernating != (left_till_hibernate <= 1))
		{
			hibernating = (left_till_hibernate <= 1);
			SetWalWriterSleeping(hibernating);
		}

		/* Clear any already-pending wakeups */
		ResetLatch(MyLatch);

		/* Process any signals received recently */
		ProcessMainLoopInterrupts();

		/*
		 * Do what we're here for; then, if XLogBackgroundFlush() found useful
		 * work to do, reset hibernation counter.
		 */
		if (XLogBackgroundFlush())
			left_till_hibernate = LOOPS_UNTIL_HIBERNATE;
		else if (left_till_hibernate > 0)
			left_till_hibernate--;

		/* report pending statistics to the cumulative stats system */
		pgstat_report_wal(false);

		/*
		 * Sleep until we are signaled or WalWriterDelay has elapsed.  If we
		 * haven't done anything useful for quite some time, lengthen the
		 * sleep time so as to reduce the server's idle power consumption.
		 */
		if (left_till_hibernate > 0)
			cur_timeout = WalWriterDelay;	/* in ms */
		else
			cur_timeout = WalWriterDelay * HIBERNATE_FACTOR;

		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 cur_timeout,
						 WAIT_EVENT_WAL_WRITER_MAIN);
	}
}
