/*-------------------------------------------------------------------------
 *
 * checkpointer.c
 *
 * The checkpointer is new as of Postgres 9.2.  It handles all checkpoints.
 * Checkpoints are automatically dispatched after a certain amount of time has
 * elapsed since the last one, and it can be signaled to perform requested
 * checkpoints as well.  (The GUC parameter that mandates a checkpoint every
 * so many WAL segments is implemented by having backends signal when they
 * fill WAL segments; the checkpointer itself doesn't watch for the
 * condition.)
 *
 * The normal termination sequence is that checkpointer is instructed to
 * execute the shutdown checkpoint by SIGINT.  After that checkpointer waits
 * to be terminated via SIGUSR2, which instructs the checkpointer to exit(0).
 * All backends must be stopped before SIGINT or SIGUSR2 is issued!
 *
 * Emergency termination is by SIGQUIT; like any backend, the checkpointer
 * will simply abort and exit on SIGQUIT.
 *
 * If the checkpointer exits unexpectedly, the postmaster treats that the same
 * as a backend crash: shared memory may be corrupted, so remaining backends
 * should be killed by SIGQUIT and then a recovery cycle started.  (Even if
 * shared memory isn't corrupted, we have lost information about which
 * files need to be fsync'd for the next checkpoint, and so a system
 * restart needs to be forced.)
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/checkpointer.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/time.h>
#include <time.h>

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xlogrecovery.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/auxprocess.h"
#include "postmaster/bgwriter.h"
#include "postmaster/interrupt.h"
#include "replication/syncrep.h"
#include "storage/aio_subsys.h"
#include "storage/bufmgr.h"
#include "storage/condition_variable.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/shmem.h"
#include "storage/smgr.h"
#include "storage/spin.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/resowner.h"


/*----------
 * Shared memory area for communication between checkpointer and backends
 *
 * The ckpt counters allow backends to watch for completion of a checkpoint
 * request they send.  Here's how it works:
 *	* At start of a checkpoint, checkpointer reads (and clears) the request
 *	  flags and increments ckpt_started, while holding ckpt_lck.
 *	* On completion of a checkpoint, checkpointer sets ckpt_done to
 *	  equal ckpt_started.
 *	* On failure of a checkpoint, checkpointer increments ckpt_failed
 *	  and sets ckpt_done to equal ckpt_started.
 *
 * The algorithm for backends is:
 *	1. Record current values of ckpt_failed and ckpt_started, and
 *	   set request flags, while holding ckpt_lck.
 *	2. Send signal to request checkpoint.
 *	3. Sleep until ckpt_started changes.  Now you know a checkpoint has
 *	   begun since you started this algorithm (although *not* that it was
 *	   specifically initiated by your signal), and that it is using your flags.
 *	4. Record new value of ckpt_started.
 *	5. Sleep until ckpt_done >= saved value of ckpt_started.  (Use modulo
 *	   arithmetic here in case counters wrap around.)  Now you know a
 *	   checkpoint has started and completed, but not whether it was
 *	   successful.
 *	6. If ckpt_failed is different from the originally saved value,
 *	   assume request failed; otherwise it was definitely successful.
 *
 * ckpt_flags holds the OR of the checkpoint request flags sent by all
 * requesting backends since the last checkpoint start.  The flags are
 * chosen so that OR'ing is the correct way to combine multiple requests.
 *
 * The requests array holds fsync requests sent by backends and not yet
 * absorbed by the checkpointer.
 *
 * Unlike the checkpoint fields, requests related fields are protected by
 * CheckpointerCommLock.
 *----------
 */
typedef struct
{
	SyncRequestType type;		/* request type */
	FileTag		ftag;			/* file identifier */
} CheckpointerRequest;

typedef struct
{
	pid_t		checkpointer_pid;	/* PID (0 if not started) */

	slock_t		ckpt_lck;		/* protects all the ckpt_* fields */

	int			ckpt_started;	/* advances when checkpoint starts */
	int			ckpt_done;		/* advances when checkpoint done */
	int			ckpt_failed;	/* advances when checkpoint fails */

	int			ckpt_flags;		/* checkpoint flags, as defined in xlog.h */

	ConditionVariable start_cv; /* signaled when ckpt_started advances */
	ConditionVariable done_cv;	/* signaled when ckpt_done advances */

	int			num_requests;	/* current # of requests */
	int			max_requests;	/* allocated array size */
	CheckpointerRequest requests[FLEXIBLE_ARRAY_MEMBER];
} CheckpointerShmemStruct;

static CheckpointerShmemStruct *CheckpointerShmem;

/* interval for calling AbsorbSyncRequests in CheckpointWriteDelay */
#define WRITES_PER_ABSORB		1000

/*
 * GUC parameters
 */
int			CheckPointTimeout = 300;
int			CheckPointWarning = 30;
double		CheckPointCompletionTarget = 0.9;

/*
 * Private state
 */
static bool ckpt_active = false;
static volatile sig_atomic_t ShutdownXLOGPending = false;

/* these values are valid when ckpt_active is true: */
static pg_time_t ckpt_start_time;
static XLogRecPtr ckpt_start_recptr;
static double ckpt_cached_elapsed;

static pg_time_t last_checkpoint_time;
static pg_time_t last_xlog_switch_time;

/* Prototypes for private functions */

static void ProcessCheckpointerInterrupts(void);
static void CheckArchiveTimeout(void);
static bool IsCheckpointOnSchedule(double progress);
static bool ImmediateCheckpointRequested(void);
static bool CompactCheckpointerRequestQueue(void);
static void UpdateSharedMemoryConfig(void);

/* Signal handlers */
static void ReqShutdownXLOG(SIGNAL_ARGS);


/*
 * Main entry point for checkpointer process
 *
 * This is invoked from AuxiliaryProcessMain, which has already created the
 * basic execution environment, but not enabled signals yet.
 */
void
CheckpointerMain(const void *startup_data, size_t startup_data_len)
{
	sigjmp_buf	local_sigjmp_buf;
	MemoryContext checkpointer_context;

	Assert(startup_data_len == 0);

	MyBackendType = B_CHECKPOINTER;
	AuxiliaryProcessMainCommon();

	CheckpointerShmem->checkpointer_pid = MyProcPid;

	/*
	 * Properly accept or ignore signals the postmaster might send us
	 *
	 * Note: we deliberately ignore SIGTERM, because during a standard Unix
	 * system shutdown cycle, init will SIGTERM all processes at once.  We
	 * want to wait for the backends to exit, whereupon the postmaster will
	 * tell us it's okay to shut down (via SIGUSR2).
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGINT, ReqShutdownXLOG);
	pqsignal(SIGTERM, SIG_IGN); /* ignore SIGTERM */
	/* SIGQUIT handler was already set up by InitPostmasterChild */
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, SignalHandlerForShutdownRequest);

	/*
	 * Reset some signals that are accepted by postmaster but not here
	 */
	pqsignal(SIGCHLD, SIG_DFL);

	/*
	 * Initialize so that first time-driven event happens at the correct time.
	 */
	last_checkpoint_time = last_xlog_switch_time = (pg_time_t) time(NULL);

	/*
	 * Write out stats after shutdown. This needs to be called by exactly one
	 * process during a normal shutdown, and since checkpointer is shut down
	 * very late...
	 *
	 * While e.g. walsenders are active after the shutdown checkpoint has been
	 * written (and thus could produce more stats), checkpointer stays around
	 * after the shutdown checkpoint has been written. postmaster will only
	 * signal checkpointer to exit after all processes that could emit stats
	 * have been shut down.
	 */
	before_shmem_exit(pgstat_before_server_shutdown, 0);

	/*
	 * Create a memory context that we will do all our work in.  We do this so
	 * that we can reset the context during error recovery and thereby avoid
	 * possible memory leaks.  Formerly this code just ran in
	 * TopMemoryContext, but resetting that would be a really bad idea.
	 */
	checkpointer_context = AllocSetContextCreate(TopMemoryContext,
												 "Checkpointer",
												 ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(checkpointer_context);

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
		 * about in checkpointer, but we do have LWLocks, buffers, and temp
		 * files.
		 */
		LWLockReleaseAll();
		ConditionVariableCancelSleep();
		pgstat_report_wait_end();
		pgaio_error_cleanup();
		UnlockBuffers();
		ReleaseAuxProcessResources(false);
		AtEOXact_Buffers(false);
		AtEOXact_SMgr();
		AtEOXact_Files(false);
		AtEOXact_HashTables(false);

		/* Warn any waiting backends that the checkpoint failed. */
		if (ckpt_active)
		{
			SpinLockAcquire(&CheckpointerShmem->ckpt_lck);
			CheckpointerShmem->ckpt_failed++;
			CheckpointerShmem->ckpt_done = CheckpointerShmem->ckpt_started;
			SpinLockRelease(&CheckpointerShmem->ckpt_lck);

			ConditionVariableBroadcast(&CheckpointerShmem->done_cv);

			ckpt_active = false;
		}

		/*
		 * Now return to normal top-level context and clear ErrorContext for
		 * next time.
		 */
		MemoryContextSwitchTo(checkpointer_context);
		FlushErrorState();

		/* Flush any leaked data in the top-level context */
		MemoryContextReset(checkpointer_context);

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
	 * Ensure all shared memory values are set correctly for the config. Doing
	 * this here ensures no race conditions from other concurrent updaters.
	 */
	UpdateSharedMemoryConfig();

	/*
	 * Advertise our proc number that backends can use to wake us up while
	 * we're sleeping.
	 */
	ProcGlobal->checkpointerProc = MyProcNumber;

	/*
	 * Loop until we've been asked to write the shutdown checkpoint or
	 * terminate.
	 */
	for (;;)
	{
		bool		do_checkpoint = false;
		int			flags = 0;
		pg_time_t	now;
		int			elapsed_secs;
		int			cur_timeout;
		bool		chkpt_or_rstpt_requested = false;
		bool		chkpt_or_rstpt_timed = false;

		/* Clear any already-pending wakeups */
		ResetLatch(MyLatch);

		/*
		 * Process any requests or signals received recently.
		 */
		AbsorbSyncRequests();

		ProcessCheckpointerInterrupts();
		if (ShutdownXLOGPending || ShutdownRequestPending)
			break;

		/*
		 * Detect a pending checkpoint request by checking whether the flags
		 * word in shared memory is nonzero.  We shouldn't need to acquire the
		 * ckpt_lck for this.
		 */
		if (((volatile CheckpointerShmemStruct *) CheckpointerShmem)->ckpt_flags)
		{
			do_checkpoint = true;
			chkpt_or_rstpt_requested = true;
		}

		/*
		 * Force a checkpoint if too much time has elapsed since the last one.
		 * Note that we count a timed checkpoint in stats only when this
		 * occurs without an external request, but we set the CAUSE_TIME flag
		 * bit even if there is also an external request.
		 */
		now = (pg_time_t) time(NULL);
		elapsed_secs = now - last_checkpoint_time;
		if (elapsed_secs >= CheckPointTimeout)
		{
			if (!do_checkpoint)
				chkpt_or_rstpt_timed = true;
			do_checkpoint = true;
			flags |= CHECKPOINT_CAUSE_TIME;
		}

		/*
		 * Do a checkpoint if requested.
		 */
		if (do_checkpoint)
		{
			bool		ckpt_performed = false;
			bool		do_restartpoint;

			/* Check if we should perform a checkpoint or a restartpoint. */
			do_restartpoint = RecoveryInProgress();

			/*
			 * Atomically fetch the request flags to figure out what kind of a
			 * checkpoint we should perform, and increase the started-counter
			 * to acknowledge that we've started a new checkpoint.
			 */
			SpinLockAcquire(&CheckpointerShmem->ckpt_lck);
			flags |= CheckpointerShmem->ckpt_flags;
			CheckpointerShmem->ckpt_flags = 0;
			CheckpointerShmem->ckpt_started++;
			SpinLockRelease(&CheckpointerShmem->ckpt_lck);

			ConditionVariableBroadcast(&CheckpointerShmem->start_cv);

			/*
			 * The end-of-recovery checkpoint is a real checkpoint that's
			 * performed while we're still in recovery.
			 */
			if (flags & CHECKPOINT_END_OF_RECOVERY)
				do_restartpoint = false;

			if (chkpt_or_rstpt_timed)
			{
				chkpt_or_rstpt_timed = false;
				if (do_restartpoint)
					PendingCheckpointerStats.restartpoints_timed++;
				else
					PendingCheckpointerStats.num_timed++;
			}

			if (chkpt_or_rstpt_requested)
			{
				chkpt_or_rstpt_requested = false;
				if (do_restartpoint)
					PendingCheckpointerStats.restartpoints_requested++;
				else
					PendingCheckpointerStats.num_requested++;
			}

			/*
			 * We will warn if (a) too soon since last checkpoint (whatever
			 * caused it) and (b) somebody set the CHECKPOINT_CAUSE_XLOG flag
			 * since the last checkpoint start.  Note in particular that this
			 * implementation will not generate warnings caused by
			 * CheckPointTimeout < CheckPointWarning.
			 */
			if (!do_restartpoint &&
				(flags & CHECKPOINT_CAUSE_XLOG) &&
				elapsed_secs < CheckPointWarning)
				ereport(LOG,
						(errmsg_plural("checkpoints are occurring too frequently (%d second apart)",
									   "checkpoints are occurring too frequently (%d seconds apart)",
									   elapsed_secs,
									   elapsed_secs),
						 errhint("Consider increasing the configuration parameter \"%s\".", "max_wal_size")));

			/*
			 * Initialize checkpointer-private variables used during
			 * checkpoint.
			 */
			ckpt_active = true;
			if (do_restartpoint)
				ckpt_start_recptr = GetXLogReplayRecPtr(NULL);
			else
				ckpt_start_recptr = GetInsertRecPtr();
			ckpt_start_time = now;
			ckpt_cached_elapsed = 0;

			/*
			 * Do the checkpoint.
			 */
			if (!do_restartpoint)
				ckpt_performed = CreateCheckPoint(flags);
			else
				ckpt_performed = CreateRestartPoint(flags);

			/*
			 * After any checkpoint, free all smgr objects.  Otherwise we
			 * would never do so for dropped relations, as the checkpointer
			 * does not process shared invalidation messages or call
			 * AtEOXact_SMgr().
			 */
			smgrdestroyall();

			/*
			 * Indicate checkpoint completion to any waiting backends.
			 */
			SpinLockAcquire(&CheckpointerShmem->ckpt_lck);
			CheckpointerShmem->ckpt_done = CheckpointerShmem->ckpt_started;
			SpinLockRelease(&CheckpointerShmem->ckpt_lck);

			ConditionVariableBroadcast(&CheckpointerShmem->done_cv);

			if (!do_restartpoint)
			{
				/*
				 * Note we record the checkpoint start time not end time as
				 * last_checkpoint_time.  This is so that time-driven
				 * checkpoints happen at a predictable spacing.
				 */
				last_checkpoint_time = now;

				if (ckpt_performed)
					PendingCheckpointerStats.num_performed++;
			}
			else
			{
				if (ckpt_performed)
				{
					/*
					 * The same as for checkpoint. Please see the
					 * corresponding comment.
					 */
					last_checkpoint_time = now;

					PendingCheckpointerStats.restartpoints_performed++;
				}
				else
				{
					/*
					 * We were not able to perform the restartpoint
					 * (checkpoints throw an ERROR in case of error).  Most
					 * likely because we have not received any new checkpoint
					 * WAL records since the last restartpoint. Try again in
					 * 15 s.
					 */
					last_checkpoint_time = now - CheckPointTimeout + 15;
				}
			}

			ckpt_active = false;

			/*
			 * We may have received an interrupt during the checkpoint and the
			 * latch might have been reset (e.g. in CheckpointWriteDelay).
			 */
			ProcessCheckpointerInterrupts();
			if (ShutdownXLOGPending || ShutdownRequestPending)
				break;
		}

		/* Check for archive_timeout and switch xlog files if necessary. */
		CheckArchiveTimeout();

		/* Report pending statistics to the cumulative stats system */
		pgstat_report_checkpointer();
		pgstat_report_wal(true);

		/*
		 * If any checkpoint flags have been set, redo the loop to handle the
		 * checkpoint without sleeping.
		 */
		if (((volatile CheckpointerShmemStruct *) CheckpointerShmem)->ckpt_flags)
			continue;

		/*
		 * Sleep until we are signaled or it's time for another checkpoint or
		 * xlog file switch.
		 */
		now = (pg_time_t) time(NULL);
		elapsed_secs = now - last_checkpoint_time;
		if (elapsed_secs >= CheckPointTimeout)
			continue;			/* no sleep for us ... */
		cur_timeout = CheckPointTimeout - elapsed_secs;
		if (XLogArchiveTimeout > 0 && !RecoveryInProgress())
		{
			elapsed_secs = now - last_xlog_switch_time;
			if (elapsed_secs >= XLogArchiveTimeout)
				continue;		/* no sleep for us ... */
			cur_timeout = Min(cur_timeout, XLogArchiveTimeout - elapsed_secs);
		}

		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 cur_timeout * 1000L /* convert to ms */ ,
						 WAIT_EVENT_CHECKPOINTER_MAIN);
	}

	/*
	 * From here on, elog(ERROR) should end with exit(1), not send control
	 * back to the sigsetjmp block above.
	 */
	ExitOnAnyError = true;

	if (ShutdownXLOGPending)
	{
		/*
		 * Close down the database.
		 *
		 * Since ShutdownXLOG() creates restartpoint or checkpoint, and
		 * updates the statistics, increment the checkpoint request and flush
		 * out pending statistic.
		 */
		PendingCheckpointerStats.num_requested++;
		ShutdownXLOG(0, 0);
		pgstat_report_checkpointer();
		pgstat_report_wal(true);

		/*
		 * Tell postmaster that we're done.
		 */
		SendPostmasterSignal(PMSIGNAL_XLOG_IS_SHUTDOWN);
		ShutdownXLOGPending = false;
	}

	/*
	 * Wait until we're asked to shut down. By separating the writing of the
	 * shutdown checkpoint from checkpointer exiting, checkpointer can perform
	 * some should-be-as-late-as-possible work like writing out stats.
	 */
	for (;;)
	{
		/* Clear any already-pending wakeups */
		ResetLatch(MyLatch);

		ProcessCheckpointerInterrupts();

		if (ShutdownRequestPending)
			break;

		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_EXIT_ON_PM_DEATH,
						 0,
						 WAIT_EVENT_CHECKPOINTER_SHUTDOWN);
	}

	/* Normal exit from the checkpointer is here */
	proc_exit(0);				/* done */
}

/*
 * Process any new interrupts.
 */
static void
ProcessCheckpointerInterrupts(void)
{
	if (ProcSignalBarrierPending)
		ProcessProcSignalBarrier();

	if (ConfigReloadPending)
	{
		ConfigReloadPending = false;
		ProcessConfigFile(PGC_SIGHUP);

		/*
		 * Checkpointer is the last process to shut down, so we ask it to hold
		 * the keys for a range of other tasks required most of which have
		 * nothing to do with checkpointing at all.
		 *
		 * For various reasons, some config values can change dynamically so
		 * the primary copy of them is held in shared memory to make sure all
		 * backends see the same value.  We make Checkpointer responsible for
		 * updating the shared memory copy if the parameter setting changes
		 * because of SIGHUP.
		 */
		UpdateSharedMemoryConfig();
	}

	/* Perform logging of memory contexts of this process */
	if (LogMemoryContextPending)
		ProcessLogMemoryContextInterrupt();
}

/*
 * CheckArchiveTimeout -- check for archive_timeout and switch xlog files
 *
 * This will switch to a new WAL file and force an archive file write if
 * meaningful activity is recorded in the current WAL file. This includes most
 * writes, including just a single checkpoint record, but excludes WAL records
 * that were inserted with the XLOG_MARK_UNIMPORTANT flag being set (like
 * snapshots of running transactions).  Such records, depending on
 * configuration, occur on regular intervals and don't contain important
 * information.  This avoids generating archives with a few unimportant
 * records.
 */
static void
CheckArchiveTimeout(void)
{
	pg_time_t	now;
	pg_time_t	last_time;
	XLogRecPtr	last_switch_lsn;

	if (XLogArchiveTimeout <= 0 || RecoveryInProgress())
		return;

	now = (pg_time_t) time(NULL);

	/* First we do a quick check using possibly-stale local state. */
	if ((int) (now - last_xlog_switch_time) < XLogArchiveTimeout)
		return;

	/*
	 * Update local state ... note that last_xlog_switch_time is the last time
	 * a switch was performed *or requested*.
	 */
	last_time = GetLastSegSwitchData(&last_switch_lsn);

	last_xlog_switch_time = Max(last_xlog_switch_time, last_time);

	/* Now we can do the real checks */
	if ((int) (now - last_xlog_switch_time) >= XLogArchiveTimeout)
	{
		/*
		 * Switch segment only when "important" WAL has been logged since the
		 * last segment switch (last_switch_lsn points to end of segment
		 * switch occurred in).
		 */
		if (GetLastImportantRecPtr() > last_switch_lsn)
		{
			XLogRecPtr	switchpoint;

			/* mark switch as unimportant, avoids triggering checkpoints */
			switchpoint = RequestXLogSwitch(true);

			/*
			 * If the returned pointer points exactly to a segment boundary,
			 * assume nothing happened.
			 */
			if (XLogSegmentOffset(switchpoint, wal_segment_size) != 0)
				elog(DEBUG1, "write-ahead log switch forced (\"archive_timeout\"=%d)",
					 XLogArchiveTimeout);
		}

		/*
		 * Update state in any case, so we don't retry constantly when the
		 * system is idle.
		 */
		last_xlog_switch_time = now;
	}
}

/*
 * Returns true if an immediate checkpoint request is pending.  (Note that
 * this does not check the *current* checkpoint's IMMEDIATE flag, but whether
 * there is one pending behind it.)
 */
static bool
ImmediateCheckpointRequested(void)
{
	volatile CheckpointerShmemStruct *cps = CheckpointerShmem;

	/*
	 * We don't need to acquire the ckpt_lck in this case because we're only
	 * looking at a single flag bit.
	 */
	if (cps->ckpt_flags & CHECKPOINT_IMMEDIATE)
		return true;
	return false;
}

/*
 * CheckpointWriteDelay -- control rate of checkpoint
 *
 * This function is called after each page write performed by BufferSync().
 * It is responsible for throttling BufferSync()'s write rate to hit
 * checkpoint_completion_target.
 *
 * The checkpoint request flags should be passed in; currently the only one
 * examined is CHECKPOINT_IMMEDIATE, which disables delays between writes.
 *
 * 'progress' is an estimate of how much of the work has been done, as a
 * fraction between 0.0 meaning none, and 1.0 meaning all done.
 */
void
CheckpointWriteDelay(int flags, double progress)
{
	static int	absorb_counter = WRITES_PER_ABSORB;

	/* Do nothing if checkpoint is being executed by non-checkpointer process */
	if (!AmCheckpointerProcess())
		return;

	/*
	 * Perform the usual duties and take a nap, unless we're behind schedule,
	 * in which case we just try to catch up as quickly as possible.
	 */
	if (!(flags & CHECKPOINT_IMMEDIATE) &&
		!ShutdownXLOGPending &&
		!ShutdownRequestPending &&
		!ImmediateCheckpointRequested() &&
		IsCheckpointOnSchedule(progress))
	{
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
			/* update shmem copies of config variables */
			UpdateSharedMemoryConfig();
		}

		AbsorbSyncRequests();
		absorb_counter = WRITES_PER_ABSORB;

		CheckArchiveTimeout();

		/* Report interim statistics to the cumulative stats system */
		pgstat_report_checkpointer();

		/*
		 * This sleep used to be connected to bgwriter_delay, typically 200ms.
		 * That resulted in more frequent wakeups if not much work to do.
		 * Checkpointer and bgwriter are no longer related so take the Big
		 * Sleep.
		 */
		WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH | WL_TIMEOUT,
				  100,
				  WAIT_EVENT_CHECKPOINT_WRITE_DELAY);
		ResetLatch(MyLatch);
	}
	else if (--absorb_counter <= 0)
	{
		/*
		 * Absorb pending fsync requests after each WRITES_PER_ABSORB write
		 * operations even when we don't sleep, to prevent overflow of the
		 * fsync request queue.
		 */
		AbsorbSyncRequests();
		absorb_counter = WRITES_PER_ABSORB;
	}

	/* Check for barrier events. */
	if (ProcSignalBarrierPending)
		ProcessProcSignalBarrier();
}

/*
 * IsCheckpointOnSchedule -- are we on schedule to finish this checkpoint
 *		 (or restartpoint) in time?
 *
 * Compares the current progress against the time/segments elapsed since last
 * checkpoint, and returns true if the progress we've made this far is greater
 * than the elapsed time/segments.
 */
static bool
IsCheckpointOnSchedule(double progress)
{
	XLogRecPtr	recptr;
	struct timeval now;
	double		elapsed_xlogs,
				elapsed_time;

	Assert(ckpt_active);

	/* Scale progress according to checkpoint_completion_target. */
	progress *= CheckPointCompletionTarget;

	/*
	 * Check against the cached value first. Only do the more expensive
	 * calculations once we reach the target previously calculated. Since
	 * neither time or WAL insert pointer moves backwards, a freshly
	 * calculated value can only be greater than or equal to the cached value.
	 */
	if (progress < ckpt_cached_elapsed)
		return false;

	/*
	 * Check progress against WAL segments written and CheckPointSegments.
	 *
	 * We compare the current WAL insert location against the location
	 * computed before calling CreateCheckPoint. The code in XLogInsert that
	 * actually triggers a checkpoint when CheckPointSegments is exceeded
	 * compares against RedoRecPtr, so this is not completely accurate.
	 * However, it's good enough for our purposes, we're only calculating an
	 * estimate anyway.
	 *
	 * During recovery, we compare last replayed WAL record's location with
	 * the location computed before calling CreateRestartPoint. That maintains
	 * the same pacing as we have during checkpoints in normal operation, but
	 * we might exceed max_wal_size by a fair amount. That's because there can
	 * be a large gap between a checkpoint's redo-pointer and the checkpoint
	 * record itself, and we only start the restartpoint after we've seen the
	 * checkpoint record. (The gap is typically up to CheckPointSegments *
	 * checkpoint_completion_target where checkpoint_completion_target is the
	 * value that was in effect when the WAL was generated).
	 */
	if (RecoveryInProgress())
		recptr = GetXLogReplayRecPtr(NULL);
	else
		recptr = GetInsertRecPtr();
	elapsed_xlogs = (((double) (recptr - ckpt_start_recptr)) /
					 wal_segment_size) / CheckPointSegments;

	if (progress < elapsed_xlogs)
	{
		ckpt_cached_elapsed = elapsed_xlogs;
		return false;
	}

	/*
	 * Check progress against time elapsed and checkpoint_timeout.
	 */
	gettimeofday(&now, NULL);
	elapsed_time = ((double) ((pg_time_t) now.tv_sec - ckpt_start_time) +
					now.tv_usec / 1000000.0) / CheckPointTimeout;

	if (progress < elapsed_time)
	{
		ckpt_cached_elapsed = elapsed_time;
		return false;
	}

	/* It looks like we're on schedule. */
	return true;
}


/* --------------------------------
 *		signal handler routines
 * --------------------------------
 */

/* SIGINT: set flag to trigger writing of shutdown checkpoint */
static void
ReqShutdownXLOG(SIGNAL_ARGS)
{
	ShutdownXLOGPending = true;
	SetLatch(MyLatch);
}


/* --------------------------------
 *		communication with backends
 * --------------------------------
 */

/*
 * CheckpointerShmemSize
 *		Compute space needed for checkpointer-related shared memory
 */
Size
CheckpointerShmemSize(void)
{
	Size		size;

	/*
	 * Currently, the size of the requests[] array is arbitrarily set equal to
	 * NBuffers.  This may prove too large or small ...
	 */
	size = offsetof(CheckpointerShmemStruct, requests);
	size = add_size(size, mul_size(NBuffers, sizeof(CheckpointerRequest)));

	return size;
}

/*
 * CheckpointerShmemInit
 *		Allocate and initialize checkpointer-related shared memory
 */
void
CheckpointerShmemInit(void)
{
	Size		size = CheckpointerShmemSize();
	bool		found;

	CheckpointerShmem = (CheckpointerShmemStruct *)
		ShmemInitStruct("Checkpointer Data",
						size,
						&found);

	if (!found)
	{
		/*
		 * First time through, so initialize.  Note that we zero the whole
		 * requests array; this is so that CompactCheckpointerRequestQueue can
		 * assume that any pad bytes in the request structs are zeroes.
		 */
		MemSet(CheckpointerShmem, 0, size);
		SpinLockInit(&CheckpointerShmem->ckpt_lck);
		CheckpointerShmem->max_requests = NBuffers;
		ConditionVariableInit(&CheckpointerShmem->start_cv);
		ConditionVariableInit(&CheckpointerShmem->done_cv);
	}
}

/*
 * RequestCheckpoint
 *		Called in backend processes to request a checkpoint
 *
 * flags is a bitwise OR of the following:
 *	CHECKPOINT_IS_SHUTDOWN: checkpoint is for database shutdown.
 *	CHECKPOINT_END_OF_RECOVERY: checkpoint is for end of WAL recovery.
 *	CHECKPOINT_IMMEDIATE: finish the checkpoint ASAP,
 *		ignoring checkpoint_completion_target parameter.
 *	CHECKPOINT_FORCE: force a checkpoint even if no XLOG activity has occurred
 *		since the last one (implied by CHECKPOINT_IS_SHUTDOWN or
 *		CHECKPOINT_END_OF_RECOVERY).
 *	CHECKPOINT_WAIT: wait for completion before returning (otherwise,
 *		just signal checkpointer to do it, and return).
 *	CHECKPOINT_CAUSE_XLOG: checkpoint is requested due to xlog filling.
 *		(This affects logging, and in particular enables CheckPointWarning.)
 */
void
RequestCheckpoint(int flags)
{
	int			ntries;
	int			old_failed,
				old_started;

	/*
	 * If in a standalone backend, just do it ourselves.
	 */
	if (!IsPostmasterEnvironment)
	{
		/*
		 * There's no point in doing slow checkpoints in a standalone backend,
		 * because there's no other backends the checkpoint could disrupt.
		 */
		CreateCheckPoint(flags | CHECKPOINT_IMMEDIATE);

		/* Free all smgr objects, as CheckpointerMain() normally would. */
		smgrdestroyall();

		return;
	}

	/*
	 * Atomically set the request flags, and take a snapshot of the counters.
	 * When we see ckpt_started > old_started, we know the flags we set here
	 * have been seen by checkpointer.
	 *
	 * Note that we OR the flags with any existing flags, to avoid overriding
	 * a "stronger" request by another backend.  The flag senses must be
	 * chosen to make this work!
	 */
	SpinLockAcquire(&CheckpointerShmem->ckpt_lck);

	old_failed = CheckpointerShmem->ckpt_failed;
	old_started = CheckpointerShmem->ckpt_started;
	CheckpointerShmem->ckpt_flags |= (flags | CHECKPOINT_REQUESTED);

	SpinLockRelease(&CheckpointerShmem->ckpt_lck);

	/*
	 * Set checkpointer's latch to request checkpoint.  It's possible that the
	 * checkpointer hasn't started yet, so we will retry a few times if
	 * needed.  (Actually, more than a few times, since on slow or overloaded
	 * buildfarm machines, it's been observed that the checkpointer can take
	 * several seconds to start.)  However, if not told to wait for the
	 * checkpoint to occur, we consider failure to set the latch to be
	 * nonfatal and merely LOG it.  The checkpointer should see the request
	 * when it does start, with or without the SetLatch().
	 */
#define MAX_SIGNAL_TRIES 600	/* max wait 60.0 sec */
	for (ntries = 0;; ntries++)
	{
		volatile PROC_HDR *procglobal = ProcGlobal;
		ProcNumber	checkpointerProc = procglobal->checkpointerProc;

		if (checkpointerProc == INVALID_PROC_NUMBER)
		{
			if (ntries >= MAX_SIGNAL_TRIES || !(flags & CHECKPOINT_WAIT))
			{
				elog((flags & CHECKPOINT_WAIT) ? ERROR : LOG,
					 "could not notify checkpoint: checkpointer is not running");
				break;
			}
		}
		else
		{
			SetLatch(&GetPGProcByNumber(checkpointerProc)->procLatch);
			/* notified successfully */
			break;
		}

		CHECK_FOR_INTERRUPTS();
		pg_usleep(100000L);		/* wait 0.1 sec, then retry */
	}

	/*
	 * If requested, wait for completion.  We detect completion according to
	 * the algorithm given above.
	 */
	if (flags & CHECKPOINT_WAIT)
	{
		int			new_started,
					new_failed;

		/* Wait for a new checkpoint to start. */
		ConditionVariablePrepareToSleep(&CheckpointerShmem->start_cv);
		for (;;)
		{
			SpinLockAcquire(&CheckpointerShmem->ckpt_lck);
			new_started = CheckpointerShmem->ckpt_started;
			SpinLockRelease(&CheckpointerShmem->ckpt_lck);

			if (new_started != old_started)
				break;

			ConditionVariableSleep(&CheckpointerShmem->start_cv,
								   WAIT_EVENT_CHECKPOINT_START);
		}
		ConditionVariableCancelSleep();

		/*
		 * We are waiting for ckpt_done >= new_started, in a modulo sense.
		 */
		ConditionVariablePrepareToSleep(&CheckpointerShmem->done_cv);
		for (;;)
		{
			int			new_done;

			SpinLockAcquire(&CheckpointerShmem->ckpt_lck);
			new_done = CheckpointerShmem->ckpt_done;
			new_failed = CheckpointerShmem->ckpt_failed;
			SpinLockRelease(&CheckpointerShmem->ckpt_lck);

			if (new_done - new_started >= 0)
				break;

			ConditionVariableSleep(&CheckpointerShmem->done_cv,
								   WAIT_EVENT_CHECKPOINT_DONE);
		}
		ConditionVariableCancelSleep();

		if (new_failed != old_failed)
			ereport(ERROR,
					(errmsg("checkpoint request failed"),
					 errhint("Consult recent messages in the server log for details.")));
	}
}

/*
 * ForwardSyncRequest
 *		Forward a file-fsync request from a backend to the checkpointer
 *
 * Whenever a backend is compelled to write directly to a relation
 * (which should be seldom, if the background writer is getting its job done),
 * the backend calls this routine to pass over knowledge that the relation
 * is dirty and must be fsync'd before next checkpoint.  We also use this
 * opportunity to count such writes for statistical purposes.
 *
 * To avoid holding the lock for longer than necessary, we normally write
 * to the requests[] queue without checking for duplicates.  The checkpointer
 * will have to eliminate dups internally anyway.  However, if we discover
 * that the queue is full, we make a pass over the entire queue to compact
 * it.  This is somewhat expensive, but the alternative is for the backend
 * to perform its own fsync, which is far more expensive in practice.  It
 * is theoretically possible a backend fsync might still be necessary, if
 * the queue is full and contains no duplicate entries.  In that case, we
 * let the backend know by returning false.
 */
bool
ForwardSyncRequest(const FileTag *ftag, SyncRequestType type)
{
	CheckpointerRequest *request;
	bool		too_full;

	if (!IsUnderPostmaster)
		return false;			/* probably shouldn't even get here */

	if (AmCheckpointerProcess())
		elog(ERROR, "ForwardSyncRequest must not be called in checkpointer");

	LWLockAcquire(CheckpointerCommLock, LW_EXCLUSIVE);

	/*
	 * If the checkpointer isn't running or the request queue is full, the
	 * backend will have to perform its own fsync request.  But before forcing
	 * that to happen, we can try to compact the request queue.
	 */
	if (CheckpointerShmem->checkpointer_pid == 0 ||
		(CheckpointerShmem->num_requests >= CheckpointerShmem->max_requests &&
		 !CompactCheckpointerRequestQueue()))
	{
		LWLockRelease(CheckpointerCommLock);
		return false;
	}

	/* OK, insert request */
	request = &CheckpointerShmem->requests[CheckpointerShmem->num_requests++];
	request->ftag = *ftag;
	request->type = type;

	/* If queue is more than half full, nudge the checkpointer to empty it */
	too_full = (CheckpointerShmem->num_requests >=
				CheckpointerShmem->max_requests / 2);

	LWLockRelease(CheckpointerCommLock);

	/* ... but not till after we release the lock */
	if (too_full)
	{
		volatile PROC_HDR *procglobal = ProcGlobal;
		ProcNumber	checkpointerProc = procglobal->checkpointerProc;

		if (checkpointerProc != INVALID_PROC_NUMBER)
			SetLatch(&GetPGProcByNumber(checkpointerProc)->procLatch);
	}

	return true;
}

/*
 * CompactCheckpointerRequestQueue
 *		Remove duplicates from the request queue to avoid backend fsyncs.
 *		Returns "true" if any entries were removed.
 *
 * Although a full fsync request queue is not common, it can lead to severe
 * performance problems when it does happen.  So far, this situation has
 * only been observed to occur when the system is under heavy write load,
 * and especially during the "sync" phase of a checkpoint.  Without this
 * logic, each backend begins doing an fsync for every block written, which
 * gets very expensive and can slow down the whole system.
 *
 * Trying to do this every time the queue is full could lose if there
 * aren't any removable entries.  But that should be vanishingly rare in
 * practice: there's one queue entry per shared buffer.
 */
static bool
CompactCheckpointerRequestQueue(void)
{
	struct CheckpointerSlotMapping
	{
		CheckpointerRequest request;
		int			slot;
	};

	int			n,
				preserve_count;
	int			num_skipped = 0;
	HASHCTL		ctl;
	HTAB	   *htab;
	bool	   *skip_slot;

	/* must hold CheckpointerCommLock in exclusive mode */
	Assert(LWLockHeldByMe(CheckpointerCommLock));

	/* Avoid memory allocations in a critical section. */
	if (CritSectionCount > 0)
		return false;

	/* Initialize skip_slot array */
	skip_slot = palloc0(sizeof(bool) * CheckpointerShmem->num_requests);

	/* Initialize temporary hash table */
	ctl.keysize = sizeof(CheckpointerRequest);
	ctl.entrysize = sizeof(struct CheckpointerSlotMapping);
	ctl.hcxt = CurrentMemoryContext;

	htab = hash_create("CompactCheckpointerRequestQueue",
					   CheckpointerShmem->num_requests,
					   &ctl,
					   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/*
	 * The basic idea here is that a request can be skipped if it's followed
	 * by a later, identical request.  It might seem more sensible to work
	 * backwards from the end of the queue and check whether a request is
	 * *preceded* by an earlier, identical request, in the hopes of doing less
	 * copying.  But that might change the semantics, if there's an
	 * intervening SYNC_FORGET_REQUEST or SYNC_FILTER_REQUEST, so we do it
	 * this way.  It would be possible to be even smarter if we made the code
	 * below understand the specific semantics of such requests (it could blow
	 * away preceding entries that would end up being canceled anyhow), but
	 * it's not clear that the extra complexity would buy us anything.
	 */
	for (n = 0; n < CheckpointerShmem->num_requests; n++)
	{
		CheckpointerRequest *request;
		struct CheckpointerSlotMapping *slotmap;
		bool		found;

		/*
		 * We use the request struct directly as a hashtable key.  This
		 * assumes that any padding bytes in the structs are consistently the
		 * same, which should be okay because we zeroed them in
		 * CheckpointerShmemInit.  Note also that RelFileLocator had better
		 * contain no pad bytes.
		 */
		request = &CheckpointerShmem->requests[n];
		slotmap = hash_search(htab, request, HASH_ENTER, &found);
		if (found)
		{
			/* Duplicate, so mark the previous occurrence as skippable */
			skip_slot[slotmap->slot] = true;
			num_skipped++;
		}
		/* Remember slot containing latest occurrence of this request value */
		slotmap->slot = n;
	}

	/* Done with the hash table. */
	hash_destroy(htab);

	/* If no duplicates, we're out of luck. */
	if (!num_skipped)
	{
		pfree(skip_slot);
		return false;
	}

	/* We found some duplicates; remove them. */
	preserve_count = 0;
	for (n = 0; n < CheckpointerShmem->num_requests; n++)
	{
		if (skip_slot[n])
			continue;
		CheckpointerShmem->requests[preserve_count++] = CheckpointerShmem->requests[n];
	}
	ereport(DEBUG1,
			(errmsg_internal("compacted fsync request queue from %d entries to %d entries",
							 CheckpointerShmem->num_requests, preserve_count)));
	CheckpointerShmem->num_requests = preserve_count;

	/* Cleanup. */
	pfree(skip_slot);
	return true;
}

/*
 * AbsorbSyncRequests
 *		Retrieve queued sync requests and pass them to sync mechanism.
 *
 * This is exported because it must be called during CreateCheckPoint;
 * we have to be sure we have accepted all pending requests just before
 * we start fsync'ing.  Since CreateCheckPoint sometimes runs in
 * non-checkpointer processes, do nothing if not checkpointer.
 */
void
AbsorbSyncRequests(void)
{
	CheckpointerRequest *requests = NULL;
	CheckpointerRequest *request;
	int			n;

	if (!AmCheckpointerProcess())
		return;

	LWLockAcquire(CheckpointerCommLock, LW_EXCLUSIVE);

	/*
	 * We try to avoid holding the lock for a long time by copying the request
	 * array, and processing the requests after releasing the lock.
	 *
	 * Once we have cleared the requests from shared memory, we have to PANIC
	 * if we then fail to absorb them (eg, because our hashtable runs out of
	 * memory).  This is because the system cannot run safely if we are unable
	 * to fsync what we have been told to fsync.  Fortunately, the hashtable
	 * is so small that the problem is quite unlikely to arise in practice.
	 */
	n = CheckpointerShmem->num_requests;
	if (n > 0)
	{
		requests = (CheckpointerRequest *) palloc(n * sizeof(CheckpointerRequest));
		memcpy(requests, CheckpointerShmem->requests, n * sizeof(CheckpointerRequest));
	}

	START_CRIT_SECTION();

	CheckpointerShmem->num_requests = 0;

	LWLockRelease(CheckpointerCommLock);

	for (request = requests; n > 0; request++, n--)
		RememberSyncRequest(&request->ftag, request->type);

	END_CRIT_SECTION();

	if (requests)
		pfree(requests);
}

/*
 * Update any shared memory configurations based on config parameters
 */
static void
UpdateSharedMemoryConfig(void)
{
	/* update global shmem state for sync rep */
	SyncRepUpdateSyncStandbysDefined();

	/*
	 * If full_page_writes has been changed by SIGHUP, we update it in shared
	 * memory and write an XLOG_FPW_CHANGE record.
	 */
	UpdateFullPageWrites();

	elog(DEBUG2, "checkpointer updated shared memory configuration values");
}

/*
 * FirstCallSinceLastCheckpoint allows a process to take an action once
 * per checkpoint cycle by asynchronously checking for checkpoint completion.
 */
bool
FirstCallSinceLastCheckpoint(void)
{
	static int	ckpt_done = 0;
	int			new_done;
	bool		FirstCall = false;

	SpinLockAcquire(&CheckpointerShmem->ckpt_lck);
	new_done = CheckpointerShmem->ckpt_done;
	SpinLockRelease(&CheckpointerShmem->ckpt_lck);

	if (new_done != ckpt_done)
		FirstCall = true;

	ckpt_done = new_done;

	return FirstCall;
}
