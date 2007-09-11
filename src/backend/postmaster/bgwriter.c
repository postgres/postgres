/*-------------------------------------------------------------------------
 *
 * bgwriter.c
 *
 * The background writer (bgwriter) is new as of Postgres 8.0.  It attempts
 * to keep regular backends from having to write out dirty shared buffers
 * (which they would only do when needing to free a shared buffer to read in
 * another page).  In the best scenario all writes from shared buffers will
 * be issued by the background writer process.	However, regular backends are
 * still empowered to issue writes if the bgwriter fails to maintain enough
 * clean shared buffers.
 *
 * The bgwriter is also charged with handling all checkpoints.	It will
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
 * a shutdown checkpoint and then exit(0).	(All backends must be stopped
 * before SIGUSR2 is issued!)  Emergency termination is by SIGQUIT; like any
 * backend, the bgwriter will simply abort and exit on SIGQUIT.
 *
 * If the bgwriter exits unexpectedly, the postmaster treats that the same
 * as a backend crash: shared memory may be corrupted, so remaining backends
 * should be killed by SIGQUIT and then a recovery cycle started.  (Even if
 * shared memory isn't corrupted, we have lost information about which
 * files need to be fsync'd for the next checkpoint, and so a system
 * restart needs to be forced.)
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/postmaster/bgwriter.c,v 1.33.2.2 2007/09/11 17:15:40 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <time.h>
#include <unistd.h>

#include "access/xlog_internal.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/bgwriter.h"
#include "storage/fd.h"
#include "storage/freespace.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/pmsignal.h"
#include "storage/shmem.h"
#include "storage/smgr.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/resowner.h"


/*----------
 * Shared memory area for communication between bgwriter and backends
 *
 * The ckpt counters allow backends to watch for completion of a checkpoint
 * request they send.  Here's how it works:
 *	* At start of a checkpoint, bgwriter increments ckpt_started.
 *	* On completion of a checkpoint, bgwriter sets ckpt_done to
 *	  equal ckpt_started.
 *	* On failure of a checkpoint, bgwrite first increments ckpt_failed,
 *	  then sets ckpt_done to equal ckpt_started.
 * All three fields are declared sig_atomic_t to ensure they can be read
 * and written without explicit locking.  The algorithm for backends is:
 *	1. Record current values of ckpt_failed and ckpt_started (in that
 *	   order!).
 *	2. Send signal to request checkpoint.
 *	3. Sleep until ckpt_started changes.  Now you know a checkpoint has
 *	   begun since you started this algorithm (although *not* that it was
 *	   specifically initiated by your signal).
 *	4. Record new value of ckpt_started.
 *	5. Sleep until ckpt_done >= saved value of ckpt_started.  (Use modulo
 *	   arithmetic here in case counters wrap around.)  Now you know a
 *	   checkpoint has started and completed, but not whether it was
 *	   successful.
 *	6. If ckpt_failed is different from the originally saved value,
 *	   assume request failed; otherwise it was definitely successful.
 *
 * An additional field is ckpt_time_warn; this is also sig_atomic_t for
 * simplicity, but is only used as a boolean.  If a backend is requesting
 * a checkpoint for which a checkpoints-too-close-together warning is
 * reasonable, it should set this field TRUE just before sending the signal.
 *
 * The requests array holds fsync requests sent by backends and not yet
 * absorbed by the bgwriter.  Unlike the checkpoint fields, the requests
 * fields are protected by BgWriterCommLock.
 *----------
 */
typedef struct
{
	RelFileNode rnode;
	BlockNumber segno;			/* see md.c for special values */
	/* might add a real request-type field later; not needed yet */
} BgWriterRequest;

typedef struct
{
	pid_t		bgwriter_pid;	/* PID of bgwriter (0 if not started) */

	sig_atomic_t ckpt_started;	/* advances when checkpoint starts */
	sig_atomic_t ckpt_done;		/* advances when checkpoint done */
	sig_atomic_t ckpt_failed;	/* advances when checkpoint fails */

	sig_atomic_t ckpt_time_warn;	/* warn if too soon since last ckpt? */

	int			num_requests;	/* current # of requests */
	int			max_requests;	/* allocated array size */
	BgWriterRequest requests[1];	/* VARIABLE LENGTH ARRAY */
} BgWriterShmemStruct;

static BgWriterShmemStruct *BgWriterShmem;

/*
 * GUC parameters
 */
int			BgWriterDelay = 200;
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
static bool am_bg_writer = false;

static bool ckpt_active = false;

static time_t last_checkpoint_time;
static time_t last_xlog_switch_time;


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
	sigjmp_buf	local_sigjmp_buf;
	MemoryContext bgwriter_context;

	Assert(BgWriterShmem != NULL);
	BgWriterShmem->bgwriter_pid = MyProcPid;
	am_bg_writer = true;

	/*
	 * If possible, make this process a group leader, so that the postmaster
	 * can signal any child processes too.  (bgwriter probably never has
	 * any child processes, but for consistency we make all postmaster
	 * child processes do this.)
	 */
#ifdef HAVE_SETSID
	if (setsid() < 0)
		elog(FATAL, "setsid() failed: %m");
#endif

	/*
	 * Properly accept or ignore signals the postmaster might send us
	 *
	 * Note: we deliberately ignore SIGTERM, because during a standard Unix
	 * system shutdown cycle, init will SIGTERM all processes at once.	We
	 * want to wait for the backends to exit, whereupon the postmaster will
	 * tell us it's okay to shut down (via SIGUSR2).
	 *
	 * SIGUSR1 is presently unused; keep it spare in case someday we want this
	 * process to participate in sinval messaging.
	 */
	pqsignal(SIGHUP, BgSigHupHandler);	/* set flag to read config file */
	pqsignal(SIGINT, ReqCheckpointHandler);		/* request checkpoint */
	pqsignal(SIGTERM, SIG_IGN); /* ignore SIGTERM */
	pqsignal(SIGQUIT, bg_quickdie);		/* hard crash time */
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, SIG_IGN); /* reserve for sinval */
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
	 * Initialize so that first time-driven event happens at the correct time.
	 */
	last_checkpoint_time = last_xlog_switch_time = time(NULL);

	/*
	 * Create a resource owner to keep track of our resources (currently only
	 * buffer pins).
	 */
	CurrentResourceOwner = ResourceOwnerCreate(NULL, "Background Writer");

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
		 * AbortTransaction().	We don't have very many resources to worry
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
		AtEOXact_Files();
		AtEOXact_HashTables(false);

		/* Warn any waiting backends that the checkpoint failed. */
		if (ckpt_active)
		{
			/* use volatile pointer to prevent code rearrangement */
			volatile BgWriterShmemStruct *bgs = BgWriterShmem;

			bgs->ckpt_failed++;
			bgs->ckpt_done = bgs->ckpt_started;
			ckpt_active = false;
		}

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
	 * Loop forever
	 */
	for (;;)
	{
		bool		do_checkpoint = false;
		bool		force_checkpoint = false;
		time_t		now;
		int			elapsed_secs;
		long		udelay;

		/*
		 * Emergency bailout if postmaster has died.  This is to avoid the
		 * necessity for manual cleanup of all postmaster children.
		 */
		if (!PostmasterIsAlive(true))
			exit(1);

		/*
		 * Process any requests or signals received recently.
		 */
		AbsorbFsyncRequests();

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
			/*
			 * From here on, elog(ERROR) should end with exit(1), not send
			 * control back to the sigsetjmp block above
			 */
			ExitOnAnyError = true;
			/* Close down the database */
			ShutdownXLOG(0, 0);
			DumpFreeSpaceMap(0, 0);
			/* Normal exit from the bgwriter is here */
			proc_exit(0);		/* done */
		}

		/*
		 * Do an unforced checkpoint if too much time has elapsed since the
		 * last one.
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
			/*
			 * We will warn if (a) too soon since last checkpoint (whatever
			 * caused it) and (b) somebody has set the ckpt_time_warn flag
			 * since the last checkpoint start.  Note in particular that this
			 * implementation will not generate warnings caused by
			 * CheckPointTimeout < CheckPointWarning.
			 */
			if (BgWriterShmem->ckpt_time_warn &&
				elapsed_secs < CheckPointWarning)
				ereport(LOG,
						(errmsg("checkpoints are occurring too frequently (%d seconds apart)",
								elapsed_secs),
						 errhint("Consider increasing the configuration parameter \"checkpoint_segments\".")));
			BgWriterShmem->ckpt_time_warn = false;

			/*
			 * Indicate checkpoint start to any waiting backends.
			 */
			ckpt_active = true;
			BgWriterShmem->ckpt_started++;

			CreateCheckPoint(false, force_checkpoint);

			/*
			 * After any checkpoint, close all smgr files.	This is so we
			 * won't hang onto smgr references to deleted files indefinitely.
			 */
			smgrcloseall();

			/*
			 * Indicate checkpoint completion to any waiting backends.
			 */
			BgWriterShmem->ckpt_done = BgWriterShmem->ckpt_started;
			ckpt_active = false;

			/*
			 * Note we record the checkpoint start time not end time as
			 * last_checkpoint_time.  This is so that time-driven checkpoints
			 * happen at a predictable spacing.
			 */
			last_checkpoint_time = now;
		}
		else
			BgBufferSync();

		/*
		 * Check for archive_timeout, if so, switch xlog files.  First we do a
		 * quick check using possibly-stale local state.
		 */
		if (XLogArchiveTimeout > 0 &&
			(int) (now - last_xlog_switch_time) >= XLogArchiveTimeout)
		{
			/*
			 * Update local state ... note that last_xlog_switch_time is the
			 * last time a switch was performed *or requested*.
			 */
			time_t		last_time = GetLastSegSwitchTime();

			last_xlog_switch_time = Max(last_xlog_switch_time, last_time);

			/* if we did a checkpoint, 'now' might be stale too */
			if (do_checkpoint)
				now = time(NULL);

			/* Now we can do the real check */
			if ((int) (now - last_xlog_switch_time) >= XLogArchiveTimeout)
			{
				XLogRecPtr	switchpoint;

				/* OK, it's time to switch */
				switchpoint = RequestXLogSwitch();

				/*
				 * If the returned pointer points exactly to a segment
				 * boundary, assume nothing happened.
				 */
				if ((switchpoint.xrecoff % XLogSegSize) != 0)
					ereport(DEBUG1,
							(errmsg("transaction log switch forced (archive_timeout=%d)",
									XLogArchiveTimeout)));

				/*
				 * Update state in any case, so we don't retry constantly when
				 * the system is idle.
				 */
				last_xlog_switch_time = now;
			}
		}

		/*
		 * Nap for the configured time, or sleep for 10 seconds if there is no
		 * bgwriter activity configured.
		 *
		 * On some platforms, signals won't interrupt the sleep.  To ensure we
		 * respond reasonably promptly when someone signals us, break down the
		 * sleep into 1-second increments, and check for interrupts after each
		 * nap.
		 *
		 * We absorb pending requests after each short sleep.
		 */
		if ((bgwriter_all_percent > 0.0 && bgwriter_all_maxpages > 0) ||
			(bgwriter_lru_percent > 0.0 && bgwriter_lru_maxpages > 0))
			udelay = BgWriterDelay * 1000L;
		else if (XLogArchiveTimeout > 0)
			udelay = 1000000L;	/* One second */
		else
			udelay = 10000000L; /* Ten seconds */

		while (udelay > 999999L)
		{
			if (got_SIGHUP || checkpoint_requested || shutdown_requested)
				break;
			pg_usleep(1000000L);
			AbsorbFsyncRequests();
			udelay -= 1000000L;
		}

		if (!(got_SIGHUP || checkpoint_requested || shutdown_requested))
			pg_usleep(udelay);
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
	 * corrupted, so we don't want to try to clean up our transaction. Just
	 * nail the windows shut and get out of town.
	 *
	 * Note we do exit(2) not exit(0).	This is to force the postmaster into a
	 * system reset cycle if some idiot DBA sends a manual SIGQUIT to a random
	 * backend.  This is necessary precisely because we don't clean up our
	 * shared memory state.
	 */
	exit(2);
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
Size
BgWriterShmemSize(void)
{
	Size		size;

	/*
	 * Currently, the size of the requests[] array is arbitrarily set equal to
	 * NBuffers.  This may prove too large or small ...
	 */
	size = offsetof(BgWriterShmemStruct, requests);
	size = add_size(size, mul_size(NBuffers, sizeof(BgWriterRequest)));

	return size;
}

/*
 * BgWriterShmemInit
 *		Allocate and initialize bgwriter-related shared memory
 */
void
BgWriterShmemInit(void)
{
	bool		found;

	BgWriterShmem = (BgWriterShmemStruct *)
		ShmemInitStruct("Background Writer Data",
						BgWriterShmemSize(),
						&found);
	if (BgWriterShmem == NULL)
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("not enough shared memory for background writer")));
	if (found)
		return;					/* already initialized */

	MemSet(BgWriterShmem, 0, sizeof(BgWriterShmemStruct));
	BgWriterShmem->max_requests = NBuffers;
}

/*
 * RequestCheckpoint
 *		Called in backend processes to request an immediate checkpoint
 *
 * If waitforit is true, wait until the checkpoint is completed
 * before returning; otherwise, just signal the request and return
 * immediately.
 *
 * If warnontime is true, and it's "too soon" since the last checkpoint,
 * the bgwriter will log a warning.  This should be true only for checkpoints
 * caused due to xlog filling, else the warning will be misleading.
 */
void
RequestCheckpoint(bool waitforit, bool warnontime)
{
	/* use volatile pointer to prevent code rearrangement */
	volatile BgWriterShmemStruct *bgs = BgWriterShmem;
	sig_atomic_t old_failed = bgs->ckpt_failed;
	sig_atomic_t old_started = bgs->ckpt_started;

	/*
	 * If in a standalone backend, just do it ourselves.
	 */
	if (!IsPostmasterEnvironment)
	{
		CreateCheckPoint(false, true);

		/*
		 * After any checkpoint, close all smgr files.	This is so we won't
		 * hang onto smgr references to deleted files indefinitely.
		 */
		smgrcloseall();

		return;
	}

	/* Set warning request flag if appropriate */
	if (warnontime)
		bgs->ckpt_time_warn = true;

	/*
	 * Send signal to request checkpoint.  When waitforit is false, we
	 * consider failure to send the signal to be nonfatal.
	 */
	if (BgWriterShmem->bgwriter_pid == 0)
		elog(waitforit ? ERROR : LOG,
			 "could not request checkpoint because bgwriter not running");
	if (kill(BgWriterShmem->bgwriter_pid, SIGINT) != 0)
		elog(waitforit ? ERROR : LOG,
			 "could not signal for checkpoint: %m");

	/*
	 * If requested, wait for completion.  We detect completion according to
	 * the algorithm given above.
	 */
	if (waitforit)
	{
		while (bgs->ckpt_started == old_started)
		{
			CHECK_FOR_INTERRUPTS();
			pg_usleep(100000L);
		}
		old_started = bgs->ckpt_started;

		/*
		 * We are waiting for ckpt_done >= old_started, in a modulo sense.
		 * This is a little tricky since we don't know the width or signedness
		 * of sig_atomic_t.  We make the lowest common denominator assumption
		 * that it is only as wide as "char".  This means that this algorithm
		 * will cope correctly as long as we don't sleep for more than 127
		 * completed checkpoints.  (If we do, we will get another chance to
		 * exit after 128 more checkpoints...)
		 */
		while (((signed char) (bgs->ckpt_done - old_started)) < 0)
		{
			CHECK_FOR_INTERRUPTS();
			pg_usleep(100000L);
		}
		if (bgs->ckpt_failed != old_failed)
			ereport(ERROR,
					(errmsg("checkpoint request failed"),
					 errhint("Consult recent messages in the server log for details.")));
	}
}

/*
 * ForwardFsyncRequest
 *		Forward a file-fsync request from a backend to the bgwriter
 *
 * Whenever a backend is compelled to write directly to a relation
 * (which should be seldom, if the bgwriter is getting its job done),
 * the backend calls this routine to pass over knowledge that the relation
 * is dirty and must be fsync'd before next checkpoint.
 *
 * segno specifies which segment (not block!) of the relation needs to be
 * fsync'd.  (Since the valid range is much less than BlockNumber, we can
 * use high values for special flags; that's all internal to md.c, which
 * see for details.)
 *
 * If we are unable to pass over the request (at present, this can happen
 * if the shared memory queue is full), we return false.  That forces
 * the backend to do its own fsync.  We hope that will be even more seldom.
 *
 * Note: we presently make no attempt to eliminate duplicate requests
 * in the requests[] queue.  The bgwriter will have to eliminate dups
 * internally anyway, so we may as well avoid holding the lock longer
 * than we have to here.
 */
bool
ForwardFsyncRequest(RelFileNode rnode, BlockNumber segno)
{
	BgWriterRequest *request;

	if (!IsUnderPostmaster)
		return false;			/* probably shouldn't even get here */
	Assert(BgWriterShmem != NULL);

	LWLockAcquire(BgWriterCommLock, LW_EXCLUSIVE);
	if (BgWriterShmem->bgwriter_pid == 0 ||
		BgWriterShmem->num_requests >= BgWriterShmem->max_requests)
	{
		LWLockRelease(BgWriterCommLock);
		return false;
	}
	request = &BgWriterShmem->requests[BgWriterShmem->num_requests++];
	request->rnode = rnode;
	request->segno = segno;
	LWLockRelease(BgWriterCommLock);
	return true;
}

/*
 * AbsorbFsyncRequests
 *		Retrieve queued fsync requests and pass them to local smgr.
 *
 * This is exported because it must be called during CreateCheckPoint;
 * we have to be sure we have accepted all pending requests *after* we
 * establish the checkpoint REDO pointer.  Since CreateCheckPoint
 * sometimes runs in non-bgwriter processes, do nothing if not bgwriter.
 */
void
AbsorbFsyncRequests(void)
{
	BgWriterRequest *requests = NULL;
	BgWriterRequest *request;
	int			n;

	if (!am_bg_writer)
		return;

	/*
	 * We have to PANIC if we fail to absorb all the pending requests (eg,
	 * because our hashtable runs out of memory).  This is because the system
	 * cannot run safely if we are unable to fsync what we have been told to
	 * fsync.  Fortunately, the hashtable is so small that the problem is
	 * quite unlikely to arise in practice.
	 */
	START_CRIT_SECTION();

	/*
	 * We try to avoid holding the lock for a long time by copying the request
	 * array.
	 */
	LWLockAcquire(BgWriterCommLock, LW_EXCLUSIVE);

	n = BgWriterShmem->num_requests;
	if (n > 0)
	{
		requests = (BgWriterRequest *) palloc(n * sizeof(BgWriterRequest));
		memcpy(requests, BgWriterShmem->requests, n * sizeof(BgWriterRequest));
	}
	BgWriterShmem->num_requests = 0;

	LWLockRelease(BgWriterCommLock);

	for (request = requests; n > 0; request++, n--)
		RememberFsyncRequest(request->rnode, request->segno);

	if (requests)
		pfree(requests);

	END_CRIT_SECTION();
}
