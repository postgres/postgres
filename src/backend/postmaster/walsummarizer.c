/*-------------------------------------------------------------------------
 *
 * walsummarizer.c
 *
 * Background process to perform WAL summarization, if it is enabled.
 * It continuously scans the write-ahead log and periodically emits a
 * summary file which indicates which blocks in which relation forks
 * were modified by WAL records in the LSN range covered by the summary
 * file. See walsummary.c and blkreftable.c for more details on the
 * naming and contents of WAL summary files.
 *
 * If configured to do, this background process will also remove WAL
 * summary files when the file timestamp is older than a configurable
 * threshold (but only if the WAL has been removed first).
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/walsummarizer.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/timeline.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "backup/walsummary.h"
#include "catalog/storage_xlog.h"
#include "commands/dbcommands_xlog.h"
#include "common/blkreftable.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/auxprocess.h"
#include "postmaster/interrupt.h"
#include "postmaster/walsummarizer.h"
#include "replication/walreceiver.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/shmem.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

/*
 * Data in shared memory related to WAL summarization.
 */
typedef struct
{
	/*
	 * These fields are protected by WALSummarizerLock.
	 *
	 * Until we've discovered what summary files already exist on disk and
	 * stored that information in shared memory, initialized is false and the
	 * other fields here contain no meaningful information. After that has
	 * been done, initialized is true.
	 *
	 * summarized_tli and summarized_lsn indicate the last LSN and TLI at
	 * which the next summary file will start. Normally, these are the LSN and
	 * TLI at which the last file ended; in such case, lsn_is_exact is true.
	 * If, however, the LSN is just an approximation, then lsn_is_exact is
	 * false. This can happen if, for example, there are no existing WAL
	 * summary files at startup. In that case, we have to derive the position
	 * at which to start summarizing from the WAL files that exist on disk,
	 * and so the LSN might point to the start of the next file even though
	 * that might happen to be in the middle of a WAL record.
	 *
	 * summarizer_pgprocno is the proc number of the summarizer process, if
	 * one is running, or else INVALID_PROC_NUMBER.
	 *
	 * pending_lsn is used by the summarizer to advertise the ending LSN of a
	 * record it has recently read. It shouldn't ever be less than
	 * summarized_lsn, but might be greater, because the summarizer buffers
	 * data for a range of LSNs in memory before writing out a new file.
	 */
	bool		initialized;
	TimeLineID	summarized_tli;
	XLogRecPtr	summarized_lsn;
	bool		lsn_is_exact;
	ProcNumber	summarizer_pgprocno;
	XLogRecPtr	pending_lsn;

	/*
	 * This field handles its own synchronization.
	 */
	ConditionVariable summary_file_cv;
} WalSummarizerData;

/*
 * Private data for our xlogreader's page read callback.
 */
typedef struct
{
	TimeLineID	tli;
	bool		historic;
	XLogRecPtr	read_upto;
	bool		end_of_wal;
} SummarizerReadLocalXLogPrivate;

/* Pointer to shared memory state. */
static WalSummarizerData *WalSummarizerCtl;

/*
 * When we reach end of WAL and need to read more, we sleep for a number of
 * milliseconds that is an integer multiple of MS_PER_SLEEP_QUANTUM. This is
 * the multiplier. It should vary between 1 and MAX_SLEEP_QUANTA, depending
 * on system activity. See summarizer_wait_for_wal() for how we adjust this.
 */
static long sleep_quanta = 1;

/*
 * The sleep time will always be a multiple of 200ms and will not exceed
 * thirty seconds (150 * 200 = 30 * 1000). Note that the timeout here needs
 * to be substantially less than the maximum amount of time for which an
 * incremental backup will wait for this process to catch up. Otherwise, an
 * incremental backup might time out on an idle system just because we sleep
 * for too long.
 */
#define MAX_SLEEP_QUANTA		150
#define MS_PER_SLEEP_QUANTUM	200

/*
 * This is a count of the number of pages of WAL that we've read since the
 * last time we waited for more WAL to appear.
 */
static long pages_read_since_last_sleep = 0;

/*
 * Most recent RedoRecPtr value observed by MaybeRemoveOldWalSummaries.
 */
static XLogRecPtr redo_pointer_at_last_summary_removal = InvalidXLogRecPtr;

/*
 * GUC parameters
 */
bool		summarize_wal = false;
int			wal_summary_keep_time = 10 * HOURS_PER_DAY * MINS_PER_HOUR;

static void WalSummarizerShutdown(int code, Datum arg);
static XLogRecPtr GetLatestLSN(TimeLineID *tli);
static void HandleWalSummarizerInterrupts(void);
static XLogRecPtr SummarizeWAL(TimeLineID tli, XLogRecPtr start_lsn,
							   bool exact, XLogRecPtr switch_lsn,
							   XLogRecPtr maximum_lsn);
static void SummarizeDbaseRecord(XLogReaderState *xlogreader,
								 BlockRefTable *brtab);
static void SummarizeSmgrRecord(XLogReaderState *xlogreader,
								BlockRefTable *brtab);
static void SummarizeXactRecord(XLogReaderState *xlogreader,
								BlockRefTable *brtab);
static bool SummarizeXlogRecord(XLogReaderState *xlogreader,
								bool *new_fast_forward);
static int	summarizer_read_local_xlog_page(XLogReaderState *state,
											XLogRecPtr targetPagePtr,
											int reqLen,
											XLogRecPtr targetRecPtr,
											char *cur_page);
static void summarizer_wait_for_wal(void);
static void MaybeRemoveOldWalSummaries(void);

/*
 * Amount of shared memory required for this module.
 */
Size
WalSummarizerShmemSize(void)
{
	return sizeof(WalSummarizerData);
}

/*
 * Create or attach to shared memory segment for this module.
 */
void
WalSummarizerShmemInit(void)
{
	bool		found;

	WalSummarizerCtl = (WalSummarizerData *)
		ShmemInitStruct("Wal Summarizer Ctl", WalSummarizerShmemSize(),
						&found);

	if (!found)
	{
		/*
		 * First time through, so initialize.
		 *
		 * We're just filling in dummy values here -- the real initialization
		 * will happen when GetOldestUnsummarizedLSN() is called for the first
		 * time.
		 */
		WalSummarizerCtl->initialized = false;
		WalSummarizerCtl->summarized_tli = 0;
		WalSummarizerCtl->summarized_lsn = InvalidXLogRecPtr;
		WalSummarizerCtl->lsn_is_exact = false;
		WalSummarizerCtl->summarizer_pgprocno = INVALID_PROC_NUMBER;
		WalSummarizerCtl->pending_lsn = InvalidXLogRecPtr;
		ConditionVariableInit(&WalSummarizerCtl->summary_file_cv);
	}
}

/*
 * Entry point for walsummarizer process.
 */
void
WalSummarizerMain(char *startup_data, size_t startup_data_len)
{
	sigjmp_buf	local_sigjmp_buf;
	MemoryContext context;

	/*
	 * Within this function, 'current_lsn' and 'current_tli' refer to the
	 * point from which the next WAL summary file should start. 'exact' is
	 * true if 'current_lsn' is known to be the start of a WAL record or WAL
	 * segment, and false if it might be in the middle of a record someplace.
	 *
	 * 'switch_lsn' and 'switch_tli', if set, are the LSN at which we need to
	 * switch to a new timeline and the timeline to which we need to switch.
	 * If not set, we either haven't figured out the answers yet or we're
	 * already on the latest timeline.
	 */
	XLogRecPtr	current_lsn;
	TimeLineID	current_tli;
	bool		exact;
	XLogRecPtr	switch_lsn = InvalidXLogRecPtr;
	TimeLineID	switch_tli = 0;

	Assert(startup_data_len == 0);

	MyBackendType = B_WAL_SUMMARIZER;
	AuxiliaryProcessMainCommon();

	ereport(DEBUG1,
			(errmsg_internal("WAL summarizer started")));

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

	/* Advertise ourselves. */
	on_shmem_exit(WalSummarizerShutdown, (Datum) 0);
	LWLockAcquire(WALSummarizerLock, LW_EXCLUSIVE);
	WalSummarizerCtl->summarizer_pgprocno = MyProcNumber;
	LWLockRelease(WALSummarizerLock);

	/* Create and switch to a memory context that we can reset on error. */
	context = AllocSetContextCreate(TopMemoryContext,
									"Wal Summarizer",
									ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(context);

	/*
	 * Reset some signals that are accepted by postmaster but not here
	 */
	pqsignal(SIGCHLD, SIG_DFL);

	/*
	 * If an exception is encountered, processing resumes here.
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* Since not using PG_TRY, must reset error stack by hand */
		error_context_stack = NULL;

		/* Prevent interrupts while cleaning up */
		HOLD_INTERRUPTS();

		/* Report the error to the server log */
		EmitErrorReport();

		/* Release resources we might have acquired. */
		LWLockReleaseAll();
		ConditionVariableCancelSleep();
		pgstat_report_wait_end();
		ReleaseAuxProcessResources(false);
		AtEOXact_Files(false);
		AtEOXact_HashTables(false);

		/*
		 * Now return to normal top-level context and clear ErrorContext for
		 * next time.
		 */
		MemoryContextSwitchTo(context);
		FlushErrorState();

		/* Flush any leaked data in the top-level context */
		MemoryContextReset(context);

		/* Now we can allow interrupts again */
		RESUME_INTERRUPTS();

		/*
		 * Sleep for 10 seconds before attempting to resume operations in
		 * order to avoid excessive logging.
		 *
		 * Many of the likely error conditions are things that will repeat
		 * every time. For example, if the WAL can't be read or the summary
		 * can't be written, only administrator action will cure the problem.
		 * So a really fast retry time doesn't seem to be especially
		 * beneficial, and it will clutter the logs.
		 */
		(void) WaitLatch(NULL,
						 WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 10000,
						 WAIT_EVENT_WAL_SUMMARIZER_ERROR);
	}

	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	/*
	 * Unblock signals (they were blocked when the postmaster forked us)
	 */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/*
	 * Fetch information about previous progress from shared memory, and ask
	 * GetOldestUnsummarizedLSN to reset pending_lsn to summarized_lsn. We
	 * might be recovering from an error, and if so, pending_lsn might have
	 * advanced past summarized_lsn, but any WAL we read previously has been
	 * lost and will need to be reread.
	 *
	 * If we discover that WAL summarization is not enabled, just exit.
	 */
	current_lsn = GetOldestUnsummarizedLSN(&current_tli, &exact);
	if (XLogRecPtrIsInvalid(current_lsn))
		proc_exit(0);

	/*
	 * Loop forever
	 */
	for (;;)
	{
		XLogRecPtr	latest_lsn;
		TimeLineID	latest_tli;
		XLogRecPtr	end_of_summary_lsn;

		/* Flush any leaked data in the top-level context */
		MemoryContextReset(context);

		/* Process any signals received recently. */
		HandleWalSummarizerInterrupts();

		/* If it's time to remove any old WAL summaries, do that now. */
		MaybeRemoveOldWalSummaries();

		/* Find the LSN and TLI up to which we can safely summarize. */
		latest_lsn = GetLatestLSN(&latest_tli);

		/*
		 * If we're summarizing a historic timeline and we haven't yet
		 * computed the point at which to switch to the next timeline, do that
		 * now.
		 *
		 * Note that if this is a standby, what was previously the current
		 * timeline could become historic at any time.
		 *
		 * We could try to make this more efficient by caching the results of
		 * readTimeLineHistory when latest_tli has not changed, but since we
		 * only have to do this once per timeline switch, we probably wouldn't
		 * save any significant amount of work in practice.
		 */
		if (current_tli != latest_tli && XLogRecPtrIsInvalid(switch_lsn))
		{
			List	   *tles = readTimeLineHistory(latest_tli);

			switch_lsn = tliSwitchPoint(current_tli, tles, &switch_tli);
			ereport(DEBUG1,
					errmsg_internal("switch point from TLI %u to TLI %u is at %X/%X",
									current_tli, switch_tli, LSN_FORMAT_ARGS(switch_lsn)));
		}

		/*
		 * If we've reached the switch LSN, we can't summarize anything else
		 * on this timeline. Switch to the next timeline and go around again,
		 * backing up to the exact switch point if we passed it.
		 */
		if (!XLogRecPtrIsInvalid(switch_lsn) && current_lsn >= switch_lsn)
		{
			/* Restart summarization from switch point. */
			current_tli = switch_tli;
			current_lsn = switch_lsn;

			/* Next timeline and switch point, if any, not yet known. */
			switch_lsn = InvalidXLogRecPtr;
			switch_tli = 0;

			/* Update (really, rewind, if needed) state in shared memory. */
			LWLockAcquire(WALSummarizerLock, LW_EXCLUSIVE);
			WalSummarizerCtl->summarized_lsn = current_lsn;
			WalSummarizerCtl->summarized_tli = current_tli;
			WalSummarizerCtl->lsn_is_exact = true;
			WalSummarizerCtl->pending_lsn = current_lsn;
			LWLockRelease(WALSummarizerLock);

			continue;
		}

		/* Summarize WAL. */
		end_of_summary_lsn = SummarizeWAL(current_tli,
										  current_lsn, exact,
										  switch_lsn, latest_lsn);
		Assert(!XLogRecPtrIsInvalid(end_of_summary_lsn));
		Assert(end_of_summary_lsn >= current_lsn);

		/*
		 * Update state for next loop iteration.
		 *
		 * Next summary file should start from exactly where this one ended.
		 */
		current_lsn = end_of_summary_lsn;
		exact = true;

		/* Update state in shared memory. */
		LWLockAcquire(WALSummarizerLock, LW_EXCLUSIVE);
		WalSummarizerCtl->summarized_lsn = end_of_summary_lsn;
		WalSummarizerCtl->summarized_tli = current_tli;
		WalSummarizerCtl->lsn_is_exact = true;
		WalSummarizerCtl->pending_lsn = end_of_summary_lsn;
		LWLockRelease(WALSummarizerLock);

		/* Wake up anyone waiting for more summary files to be written. */
		ConditionVariableBroadcast(&WalSummarizerCtl->summary_file_cv);
	}
}

/*
 * Get information about the state of the WAL summarizer.
 */
void
GetWalSummarizerState(TimeLineID *summarized_tli, XLogRecPtr *summarized_lsn,
					  XLogRecPtr *pending_lsn, int *summarizer_pid)
{
	LWLockAcquire(WALSummarizerLock, LW_SHARED);
	if (!WalSummarizerCtl->initialized)
	{
		/*
		 * If initialized is false, the rest of the structure contents are
		 * undefined.
		 */
		*summarized_tli = 0;
		*summarized_lsn = InvalidXLogRecPtr;
		*pending_lsn = InvalidXLogRecPtr;
		*summarizer_pid = -1;
	}
	else
	{
		int			summarizer_pgprocno = WalSummarizerCtl->summarizer_pgprocno;

		*summarized_tli = WalSummarizerCtl->summarized_tli;
		*summarized_lsn = WalSummarizerCtl->summarized_lsn;
		if (summarizer_pgprocno == INVALID_PROC_NUMBER)
		{
			/*
			 * If the summarizer has exited, the fact that it had processed
			 * beyond summarized_lsn is irrelevant now.
			 */
			*pending_lsn = WalSummarizerCtl->summarized_lsn;
			*summarizer_pid = -1;
		}
		else
		{
			*pending_lsn = WalSummarizerCtl->pending_lsn;

			/*
			 * We're not fussed about inexact answers here, since they could
			 * become stale instantly, so we don't bother taking the lock, but
			 * make sure that invalid PID values are normalized to -1.
			 */
			*summarizer_pid = GetPGProcByNumber(summarizer_pgprocno)->pid;
			if (*summarizer_pid <= 0)
				*summarizer_pid = -1;
		}
	}
	LWLockRelease(WALSummarizerLock);
}

/*
 * Get the oldest LSN in this server's timeline history that has not yet been
 * summarized, and update shared memory state as appropriate.
 *
 * If *tli != NULL, it will be set to the TLI for the LSN that is returned.
 *
 * If *lsn_is_exact != NULL, it will be set to true if the returned LSN is
 * necessarily the start of a WAL record and false if it's just the beginning
 * of a WAL segment.
 */
XLogRecPtr
GetOldestUnsummarizedLSN(TimeLineID *tli, bool *lsn_is_exact)
{
	TimeLineID	latest_tli;
	int			n;
	List	   *tles;
	XLogRecPtr	unsummarized_lsn = InvalidXLogRecPtr;
	TimeLineID	unsummarized_tli = 0;
	bool		should_make_exact = false;
	List	   *existing_summaries;
	ListCell   *lc;
	bool		am_wal_summarizer = AmWalSummarizerProcess();

	/* If not summarizing WAL, do nothing. */
	if (!summarize_wal)
		return InvalidXLogRecPtr;

	/*
	 * If we are not the WAL summarizer process, then we normally just want to
	 * read the values from shared memory. However, as an exception, if shared
	 * memory hasn't been initialized yet, then we need to do that so that we
	 * can read legal values and not remove any WAL too early.
	 */
	if (!am_wal_summarizer)
	{
		LWLockAcquire(WALSummarizerLock, LW_SHARED);

		if (WalSummarizerCtl->initialized)
		{
			unsummarized_lsn = WalSummarizerCtl->summarized_lsn;
			if (tli != NULL)
				*tli = WalSummarizerCtl->summarized_tli;
			if (lsn_is_exact != NULL)
				*lsn_is_exact = WalSummarizerCtl->lsn_is_exact;
			LWLockRelease(WALSummarizerLock);
			return unsummarized_lsn;
		}

		LWLockRelease(WALSummarizerLock);
	}

	/*
	 * Find the oldest timeline on which WAL still exists, and the earliest
	 * segment for which it exists.
	 *
	 * Note that we do this every time the WAL summarizer process restarts or
	 * recovers from an error, in case the contents of pg_wal have changed
	 * under us e.g. if some files were removed, either manually - which
	 * shouldn't really happen, but might - or by postgres itself, if
	 * summarize_wal was turned off and then back on again.
	 */
	(void) GetLatestLSN(&latest_tli);
	tles = readTimeLineHistory(latest_tli);
	for (n = list_length(tles) - 1; n >= 0; --n)
	{
		TimeLineHistoryEntry *tle = list_nth(tles, n);
		XLogSegNo	oldest_segno;

		oldest_segno = XLogGetOldestSegno(tle->tli);
		if (oldest_segno != 0)
		{
			/* Compute oldest LSN that still exists on disk. */
			XLogSegNoOffsetToRecPtr(oldest_segno, 0, wal_segment_size,
									unsummarized_lsn);

			unsummarized_tli = tle->tli;
			break;
		}
	}

	/*
	 * Don't try to summarize anything older than the end LSN of the newest
	 * summary file that exists for this timeline.
	 */
	existing_summaries =
		GetWalSummaries(unsummarized_tli,
						InvalidXLogRecPtr, InvalidXLogRecPtr);
	foreach(lc, existing_summaries)
	{
		WalSummaryFile *ws = lfirst(lc);

		if (ws->end_lsn > unsummarized_lsn)
		{
			unsummarized_lsn = ws->end_lsn;
			should_make_exact = true;
		}
	}

	/* It really should not be possible for us to find no WAL. */
	if (unsummarized_tli == 0)
		ereport(ERROR,
				errcode(ERRCODE_INTERNAL_ERROR),
				errmsg_internal("no WAL found on timeline %u", latest_tli));

	/*
	 * If we're the WAL summarizer, we always want to store the values we just
	 * computed into shared memory, because those are the values we're going
	 * to use to drive our operation, and so they are the authoritative
	 * values. Otherwise, we only store values into shared memory if shared
	 * memory is uninitialized. Our values are not canonical in such a case,
	 * but it's better to have something than nothing, to guide WAL retention.
	 */
	LWLockAcquire(WALSummarizerLock, LW_EXCLUSIVE);
	if (am_wal_summarizer || !WalSummarizerCtl->initialized)
	{
		WalSummarizerCtl->initialized = true;
		WalSummarizerCtl->summarized_lsn = unsummarized_lsn;
		WalSummarizerCtl->summarized_tli = unsummarized_tli;
		WalSummarizerCtl->lsn_is_exact = should_make_exact;
		WalSummarizerCtl->pending_lsn = unsummarized_lsn;
	}
	else
		unsummarized_lsn = WalSummarizerCtl->summarized_lsn;

	/* Also return the to the caller as required. */
	if (tli != NULL)
		*tli = WalSummarizerCtl->summarized_tli;
	if (lsn_is_exact != NULL)
		*lsn_is_exact = WalSummarizerCtl->lsn_is_exact;
	LWLockRelease(WALSummarizerLock);

	return unsummarized_lsn;
}

/*
 * Attempt to set the WAL summarizer's latch.
 *
 * This might not work, because there's no guarantee that the WAL summarizer
 * process was successfully started, and it also might have started but
 * subsequently terminated. So, under normal circumstances, this will get the
 * latch set, but there's no guarantee.
 */
void
SetWalSummarizerLatch(void)
{
	ProcNumber	pgprocno;

	if (WalSummarizerCtl == NULL)
		return;

	LWLockAcquire(WALSummarizerLock, LW_EXCLUSIVE);
	pgprocno = WalSummarizerCtl->summarizer_pgprocno;
	LWLockRelease(WALSummarizerLock);

	if (pgprocno != INVALID_PROC_NUMBER)
		SetLatch(&ProcGlobal->allProcs[pgprocno].procLatch);
}

/*
 * Wait until WAL summarization reaches the given LSN, but time out with an
 * error if the summarizer seems to be stick.
 *
 * Returns immediately if summarize_wal is turned off while we wait. Caller
 * is expected to handle this case, if necessary.
 */
void
WaitForWalSummarization(XLogRecPtr lsn)
{
	TimestampTz initial_time,
				cycle_time,
				current_time;
	XLogRecPtr	prior_pending_lsn = InvalidXLogRecPtr;
	int			deadcycles = 0;

	initial_time = cycle_time = GetCurrentTimestamp();

	while (1)
	{
		long		timeout_in_ms = 10000;
		XLogRecPtr	summarized_lsn;
		XLogRecPtr	pending_lsn;

		CHECK_FOR_INTERRUPTS();

		/* If WAL summarization is disabled while we're waiting, give up. */
		if (!summarize_wal)
			return;

		/*
		 * If the LSN summarized on disk has reached the target value, stop.
		 */
		LWLockAcquire(WALSummarizerLock, LW_EXCLUSIVE);
		summarized_lsn = WalSummarizerCtl->summarized_lsn;
		pending_lsn = WalSummarizerCtl->pending_lsn;
		LWLockRelease(WALSummarizerLock);

		/* If WAL summarization has progressed sufficiently, stop waiting. */
		if (summarized_lsn >= lsn)
			break;

		/* Recheck current time. */
		current_time = GetCurrentTimestamp();

		/* Have we finished the current cycle of waiting? */
		if (TimestampDifferenceMilliseconds(cycle_time,
											current_time) >= timeout_in_ms)
		{
			long		elapsed_seconds;

			/* Begin new wait cycle. */
			cycle_time = TimestampTzPlusMilliseconds(cycle_time,
													 timeout_in_ms);

			/*
			 * Keep track of the number of cycles during which there has been
			 * no progression of pending_lsn. If pending_lsn is not advancing,
			 * that means that not only are no new files appearing on disk,
			 * but we're not even incorporating new records into the in-memory
			 * state.
			 */
			if (pending_lsn > prior_pending_lsn)
			{
				prior_pending_lsn = pending_lsn;
				deadcycles = 0;
			}
			else
				++deadcycles;

			/*
			 * If we've managed to wait for an entire minute without the WAL
			 * summarizer absorbing a single WAL record, error out; probably
			 * something is wrong.
			 *
			 * We could consider also erroring out if the summarizer is taking
			 * too long to catch up, but it's not clear what rate of progress
			 * would be acceptable and what would be too slow. So instead, we
			 * just try to error out in the case where there's no progress at
			 * all. That seems likely to catch a reasonable number of the
			 * things that can go wrong in practice (e.g. the summarizer
			 * process is completely hung, say because somebody hooked up a
			 * debugger to it or something) without giving up too quickly when
			 * the system is just slow.
			 */
			if (deadcycles >= 6)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("WAL summarization is not progressing"),
						 errdetail("Summarization is needed through %X/%X, but is stuck at %X/%X on disk and %X/%X in memory.",
								   LSN_FORMAT_ARGS(lsn),
								   LSN_FORMAT_ARGS(summarized_lsn),
								   LSN_FORMAT_ARGS(pending_lsn))));


			/*
			 * Otherwise, just let the user know what's happening.
			 */
			elapsed_seconds =
				TimestampDifferenceMilliseconds(initial_time,
												current_time) / 1000;
			ereport(WARNING,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg_plural("still waiting for WAL summarization through %X/%X after %ld second",
								   "still waiting for WAL summarization through %X/%X after %ld seconds",
								   elapsed_seconds,
								   LSN_FORMAT_ARGS(lsn),
								   elapsed_seconds),
					 errdetail("Summarization has reached %X/%X on disk and %X/%X in memory.",
							   LSN_FORMAT_ARGS(summarized_lsn),
							   LSN_FORMAT_ARGS(pending_lsn))));
		}

		/*
		 * Align the wait time to prevent drift. This doesn't really matter,
		 * but we'd like the warnings about how long we've been waiting to say
		 * 10 seconds, 20 seconds, 30 seconds, 40 seconds ... without ever
		 * drifting to something that is not a multiple of ten.
		 */
		timeout_in_ms -=
			TimestampDifferenceMilliseconds(cycle_time, current_time);

		/* Wait and see. */
		ConditionVariableTimedSleep(&WalSummarizerCtl->summary_file_cv,
									timeout_in_ms,
									WAIT_EVENT_WAL_SUMMARY_READY);
	}

	ConditionVariableCancelSleep();
}

/*
 * On exit, update shared memory to make it clear that we're no longer
 * running.
 */
static void
WalSummarizerShutdown(int code, Datum arg)
{
	LWLockAcquire(WALSummarizerLock, LW_EXCLUSIVE);
	WalSummarizerCtl->summarizer_pgprocno = INVALID_PROC_NUMBER;
	LWLockRelease(WALSummarizerLock);
}

/*
 * Get the latest LSN that is eligible to be summarized, and set *tli to the
 * corresponding timeline.
 */
static XLogRecPtr
GetLatestLSN(TimeLineID *tli)
{
	if (!RecoveryInProgress())
	{
		/* Don't summarize WAL before it's flushed. */
		return GetFlushRecPtr(tli);
	}
	else
	{
		XLogRecPtr	flush_lsn;
		TimeLineID	flush_tli;
		XLogRecPtr	replay_lsn;
		TimeLineID	replay_tli;
		TimeLineID	insert_tli;

		/*
		 * After the insert TLI has been set and before the control file has
		 * been updated to show the DB in production, RecoveryInProgress()
		 * will return true, because it's not yet safe for all backends to
		 * begin writing WAL. However, replay has already ceased, so from our
		 * point of view, recovery is already over. We should summarize up to
		 * where replay stopped and then prepare to resume at the start of the
		 * insert timeline.
		 */
		if ((insert_tli = GetWALInsertionTimeLineIfSet()) != 0)
		{
			*tli = insert_tli;
			return GetXLogReplayRecPtr(NULL);
		}

		/*
		 * What we really want to know is how much WAL has been flushed to
		 * disk, but the only flush position available is the one provided by
		 * the walreceiver, which may not be running, because this could be
		 * crash recovery or recovery via restore_command. So use either the
		 * WAL receiver's flush position or the replay position, whichever is
		 * further ahead, on the theory that if the WAL has been replayed then
		 * it must also have been flushed to disk.
		 */
		flush_lsn = GetWalRcvFlushRecPtr(NULL, &flush_tli);
		replay_lsn = GetXLogReplayRecPtr(&replay_tli);
		if (flush_lsn > replay_lsn)
		{
			*tli = flush_tli;
			return flush_lsn;
		}
		else
		{
			*tli = replay_tli;
			return replay_lsn;
		}
	}
}

/*
 * Interrupt handler for main loop of WAL summarizer process.
 */
static void
HandleWalSummarizerInterrupts(void)
{
	if (ProcSignalBarrierPending)
		ProcessProcSignalBarrier();

	if (ConfigReloadPending)
	{
		ConfigReloadPending = false;
		ProcessConfigFile(PGC_SIGHUP);
	}

	if (ShutdownRequestPending || !summarize_wal)
	{
		ereport(DEBUG1,
				errmsg_internal("WAL summarizer shutting down"));
		proc_exit(0);
	}

	/* Perform logging of memory contexts of this process */
	if (LogMemoryContextPending)
		ProcessLogMemoryContextInterrupt();
}

/*
 * Summarize a range of WAL records on a single timeline.
 *
 * 'tli' is the timeline to be summarized.
 *
 * 'start_lsn' is the point at which we should start summarizing. If this
 * value comes from the end LSN of the previous record as returned by the
 * xlogreader machinery, 'exact' should be true; otherwise, 'exact' should
 * be false, and this function will search forward for the start of a valid
 * WAL record.
 *
 * 'switch_lsn' is the point at which we should switch to a later timeline,
 * if we're summarizing a historic timeline.
 *
 * 'maximum_lsn' identifies the point beyond which we can't count on being
 * able to read any more WAL. It should be the switch point when reading a
 * historic timeline, or the most-recently-measured end of WAL when reading
 * the current timeline.
 *
 * The return value is the LSN at which the WAL summary actually ends. Most
 * often, a summary file ends because we notice that a checkpoint has
 * occurred and reach the redo pointer of that checkpoint, but sometimes
 * we stop for other reasons, such as a timeline switch.
 */
static XLogRecPtr
SummarizeWAL(TimeLineID tli, XLogRecPtr start_lsn, bool exact,
			 XLogRecPtr switch_lsn, XLogRecPtr maximum_lsn)
{
	SummarizerReadLocalXLogPrivate *private_data;
	XLogReaderState *xlogreader;
	XLogRecPtr	summary_start_lsn;
	XLogRecPtr	summary_end_lsn = switch_lsn;
	char		temp_path[MAXPGPATH];
	char		final_path[MAXPGPATH];
	WalSummaryIO io;
	BlockRefTable *brtab = CreateEmptyBlockRefTable();
	bool		fast_forward = true;

	/* Initialize private data for xlogreader. */
	private_data = (SummarizerReadLocalXLogPrivate *)
		palloc0(sizeof(SummarizerReadLocalXLogPrivate));
	private_data->tli = tli;
	private_data->historic = !XLogRecPtrIsInvalid(switch_lsn);
	private_data->read_upto = maximum_lsn;

	/* Create xlogreader. */
	xlogreader = XLogReaderAllocate(wal_segment_size, NULL,
									XL_ROUTINE(.page_read = &summarizer_read_local_xlog_page,
											   .segment_open = &wal_segment_open,
											   .segment_close = &wal_segment_close),
									private_data);
	if (xlogreader == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while allocating a WAL reading processor.")));

	/*
	 * When exact = false, we're starting from an arbitrary point in the WAL
	 * and must search forward for the start of the next record.
	 *
	 * When exact = true, start_lsn should be either the LSN where a record
	 * begins, or the LSN of a page where the page header is immediately
	 * followed by the start of a new record. XLogBeginRead should tolerate
	 * either case.
	 *
	 * We need to allow for both cases because the behavior of xlogreader
	 * varies. When a record spans two or more xlog pages, the ending LSN
	 * reported by xlogreader will be the starting LSN of the following
	 * record, but when an xlog page boundary falls between two records, the
	 * end LSN for the first will be reported as the first byte of the
	 * following page. We can't know until we read that page how large the
	 * header will be, but we'll have to skip over it to find the next record.
	 */
	if (exact)
	{
		/*
		 * Even if start_lsn is the beginning of a page rather than the
		 * beginning of the first record on that page, we should still use it
		 * as the start LSN for the summary file. That's because we detect
		 * missing summary files by looking for cases where the end LSN of one
		 * file is less than the start LSN of the next file. When only a page
		 * header is skipped, nothing has been missed.
		 */
		XLogBeginRead(xlogreader, start_lsn);
		summary_start_lsn = start_lsn;
	}
	else
	{
		summary_start_lsn = XLogFindNextRecord(xlogreader, start_lsn);
		if (XLogRecPtrIsInvalid(summary_start_lsn))
		{
			/*
			 * If we hit end-of-WAL while trying to find the next valid
			 * record, we must be on a historic timeline that has no valid
			 * records that begin after start_lsn and before end of WAL.
			 */
			if (private_data->end_of_wal)
			{
				ereport(DEBUG1,
						errmsg_internal("could not read WAL from timeline %u at %X/%X: end of WAL at %X/%X",
										tli,
										LSN_FORMAT_ARGS(start_lsn),
										LSN_FORMAT_ARGS(private_data->read_upto)));

				/*
				 * The timeline ends at or after start_lsn, without containing
				 * any records. Thus, we must make sure the main loop does not
				 * iterate. If start_lsn is the end of the timeline, then we
				 * won't actually emit an empty summary file, but otherwise,
				 * we must, to capture the fact that the LSN range in question
				 * contains no interesting WAL records.
				 */
				summary_start_lsn = start_lsn;
				summary_end_lsn = private_data->read_upto;
				switch_lsn = xlogreader->EndRecPtr;
			}
			else
				ereport(ERROR,
						(errmsg("could not find a valid record after %X/%X",
								LSN_FORMAT_ARGS(start_lsn))));
		}

		/* We shouldn't go backward. */
		Assert(summary_start_lsn >= start_lsn);
	}

	/*
	 * Main loop: read xlog records one by one.
	 */
	while (1)
	{
		int			block_id;
		char	   *errormsg;
		XLogRecord *record;
		uint8		rmid;

		HandleWalSummarizerInterrupts();

		/* We shouldn't go backward. */
		Assert(summary_start_lsn <= xlogreader->EndRecPtr);

		/* Now read the next record. */
		record = XLogReadRecord(xlogreader, &errormsg);
		if (record == NULL)
		{
			if (private_data->end_of_wal)
			{
				/*
				 * This timeline must be historic and must end before we were
				 * able to read a complete record.
				 */
				ereport(DEBUG1,
						errmsg_internal("could not read WAL from timeline %u at %X/%X: end of WAL at %X/%X",
										tli,
										LSN_FORMAT_ARGS(xlogreader->EndRecPtr),
										LSN_FORMAT_ARGS(private_data->read_upto)));
				/* Summary ends at end of WAL. */
				summary_end_lsn = private_data->read_upto;
				break;
			}
			if (errormsg)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not read WAL from timeline %u at %X/%X: %s",
								tli, LSN_FORMAT_ARGS(xlogreader->EndRecPtr),
								errormsg)));
			else
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not read WAL from timeline %u at %X/%X",
								tli, LSN_FORMAT_ARGS(xlogreader->EndRecPtr))));
		}

		/* We shouldn't go backward. */
		Assert(summary_start_lsn <= xlogreader->EndRecPtr);

		if (!XLogRecPtrIsInvalid(switch_lsn) &&
			xlogreader->ReadRecPtr >= switch_lsn)
		{
			/*
			 * Whoops! We've read a record that *starts* after the switch LSN,
			 * contrary to our goal of reading only until we hit the first
			 * record that ends at or after the switch LSN. Pretend we didn't
			 * read it after all by bailing out of this loop right here,
			 * before we do anything with this record.
			 *
			 * This can happen because the last record before the switch LSN
			 * might be continued across multiple pages, and then we might
			 * come to a page with XLP_FIRST_IS_OVERWRITE_CONTRECORD set. In
			 * that case, the record that was continued across multiple pages
			 * is incomplete and will be disregarded, and the read will
			 * restart from the beginning of the page that is flagged
			 * XLP_FIRST_IS_OVERWRITE_CONTRECORD.
			 *
			 * If this case occurs, we can fairly say that the current summary
			 * file ends at the switch LSN exactly. The first record on the
			 * page marked XLP_FIRST_IS_OVERWRITE_CONTRECORD will be
			 * discovered when generating the next summary file.
			 */
			summary_end_lsn = switch_lsn;
			break;
		}

		/*
		 * Certain types of records require special handling. Redo points and
		 * shutdown checkpoints trigger creation of new summary files and can
		 * also cause us to enter or exit "fast forward" mode. Other types of
		 * records can require special updates to the block reference table.
		 */
		rmid = XLogRecGetRmid(xlogreader);
		if (rmid == RM_XLOG_ID)
		{
			bool		new_fast_forward;

			/*
			 * If we've already processed some WAL records when we hit a redo
			 * point or shutdown checkpoint, then we stop summarization before
			 * including this record in the current file, so that it will be
			 * the first record in the next file.
			 *
			 * When we hit one of those record types as the first record in a
			 * file, we adjust our notion of whether we're fast-forwarding.
			 * Any WAL generated with wal_level=minimal must be skipped
			 * without actually generating any summary file, because an
			 * incremental backup that crosses such WAL would be unsafe.
			 */
			if (SummarizeXlogRecord(xlogreader, &new_fast_forward))
			{
				if (xlogreader->ReadRecPtr > summary_start_lsn)
				{
					summary_end_lsn = xlogreader->ReadRecPtr;
					break;
				}
				else
					fast_forward = new_fast_forward;
			}
		}
		else if (!fast_forward)
		{
			/*
			 * This switch handles record types that require extra updates to
			 * the contents of the block reference table.
			 */
			switch (rmid)
			{
				case RM_DBASE_ID:
					SummarizeDbaseRecord(xlogreader, brtab);
					break;
				case RM_SMGR_ID:
					SummarizeSmgrRecord(xlogreader, brtab);
					break;
				case RM_XACT_ID:
					SummarizeXactRecord(xlogreader, brtab);
					break;
			}
		}

		/*
		 * If we're in fast-forward mode, we don't really need to do anything.
		 * Otherwise, feed block references from xlog record to block
		 * reference table.
		 */
		if (!fast_forward)
		{
			for (block_id = 0; block_id <= XLogRecMaxBlockId(xlogreader);
				 block_id++)
			{
				RelFileLocator rlocator;
				ForkNumber	forknum;
				BlockNumber blocknum;

				if (!XLogRecGetBlockTagExtended(xlogreader, block_id, &rlocator,
												&forknum, &blocknum, NULL))
					continue;

				/*
				 * As we do elsewhere, ignore the FSM fork, because it's not
				 * fully WAL-logged.
				 */
				if (forknum != FSM_FORKNUM)
					BlockRefTableMarkBlockModified(brtab, &rlocator, forknum,
												   blocknum);
			}
		}

		/* Update our notion of where this summary file ends. */
		summary_end_lsn = xlogreader->EndRecPtr;

		/* Also update shared memory. */
		LWLockAcquire(WALSummarizerLock, LW_EXCLUSIVE);
		Assert(summary_end_lsn >= WalSummarizerCtl->summarized_lsn);
		WalSummarizerCtl->pending_lsn = summary_end_lsn;
		LWLockRelease(WALSummarizerLock);

		/*
		 * If we have a switch LSN and have reached it, stop before reading
		 * the next record.
		 */
		if (!XLogRecPtrIsInvalid(switch_lsn) &&
			xlogreader->EndRecPtr >= switch_lsn)
			break;
	}

	/* Destroy xlogreader. */
	pfree(xlogreader->private_data);
	XLogReaderFree(xlogreader);

	/*
	 * If a timeline switch occurs, we may fail to make any progress at all
	 * before exiting the loop above. If that happens, we don't write a WAL
	 * summary file at all. We can also skip writing a file if we're in
	 * fast-forward mode.
	 */
	if (summary_end_lsn > summary_start_lsn && !fast_forward)
	{
		/* Generate temporary and final path name. */
		snprintf(temp_path, MAXPGPATH,
				 XLOGDIR "/summaries/temp.summary");
		snprintf(final_path, MAXPGPATH,
				 XLOGDIR "/summaries/%08X%08X%08X%08X%08X.summary",
				 tli,
				 LSN_FORMAT_ARGS(summary_start_lsn),
				 LSN_FORMAT_ARGS(summary_end_lsn));

		/* Open the temporary file for writing. */
		io.filepos = 0;
		io.file = PathNameOpenFile(temp_path, O_WRONLY | O_CREAT | O_TRUNC);
		if (io.file < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create file \"%s\": %m", temp_path)));

		/* Write the data. */
		WriteBlockRefTable(brtab, WriteWalSummary, &io);

		/* Close temporary file and shut down xlogreader. */
		FileClose(io.file);

		/* Tell the user what we did. */
		ereport(DEBUG1,
				errmsg_internal("summarized WAL on TLI %u from %X/%X to %X/%X",
								tli,
								LSN_FORMAT_ARGS(summary_start_lsn),
								LSN_FORMAT_ARGS(summary_end_lsn)));

		/* Durably rename the new summary into place. */
		durable_rename(temp_path, final_path, ERROR);
	}

	/* If we skipped a non-zero amount of WAL, log a debug message. */
	if (summary_end_lsn > summary_start_lsn && fast_forward)
		ereport(DEBUG1,
				errmsg_internal("skipped summarizing WAL on TLI %u from %X/%X to %X/%X",
								tli,
								LSN_FORMAT_ARGS(summary_start_lsn),
								LSN_FORMAT_ARGS(summary_end_lsn)));

	return summary_end_lsn;
}

/*
 * Special handling for WAL records with RM_DBASE_ID.
 */
static void
SummarizeDbaseRecord(XLogReaderState *xlogreader, BlockRefTable *brtab)
{
	uint8		info = XLogRecGetInfo(xlogreader) & ~XLR_INFO_MASK;

	/*
	 * We use relfilenode zero for a given database OID and tablespace OID to
	 * indicate that all relations with that pair of IDs have been recreated
	 * if they exist at all. Effectively, we're setting a limit block of 0 for
	 * all such relfilenodes.
	 *
	 * Technically, this special handling is only needed in the case of
	 * XLOG_DBASE_CREATE_FILE_COPY, because that can create a whole bunch of
	 * relation files in a directory without logging anything specific to each
	 * one. If we didn't mark the whole DB OID/TS OID combination in some way,
	 * then a tablespace that was dropped after the reference backup and
	 * recreated using the FILE_COPY method prior to the incremental backup
	 * would look just like one that was never touched at all, which would be
	 * catastrophic.
	 *
	 * But it seems best to adopt this treatment for all records that drop or
	 * create a DB OID/TS OID combination. That's similar to how we treat the
	 * limit block for individual relations, and it's an extra layer of safety
	 * here. We can never lose data by marking more stuff as needing to be
	 * backed up in full.
	 */
	if (info == XLOG_DBASE_CREATE_FILE_COPY)
	{
		xl_dbase_create_file_copy_rec *xlrec;
		RelFileLocator rlocator;

		xlrec =
			(xl_dbase_create_file_copy_rec *) XLogRecGetData(xlogreader);
		rlocator.spcOid = xlrec->tablespace_id;
		rlocator.dbOid = xlrec->db_id;
		rlocator.relNumber = 0;
		BlockRefTableSetLimitBlock(brtab, &rlocator, MAIN_FORKNUM, 0);
	}
	else if (info == XLOG_DBASE_CREATE_WAL_LOG)
	{
		xl_dbase_create_wal_log_rec *xlrec;
		RelFileLocator rlocator;

		xlrec = (xl_dbase_create_wal_log_rec *) XLogRecGetData(xlogreader);
		rlocator.spcOid = xlrec->tablespace_id;
		rlocator.dbOid = xlrec->db_id;
		rlocator.relNumber = 0;
		BlockRefTableSetLimitBlock(brtab, &rlocator, MAIN_FORKNUM, 0);
	}
	else if (info == XLOG_DBASE_DROP)
	{
		xl_dbase_drop_rec *xlrec;
		RelFileLocator rlocator;
		int			i;

		xlrec = (xl_dbase_drop_rec *) XLogRecGetData(xlogreader);
		rlocator.dbOid = xlrec->db_id;
		rlocator.relNumber = 0;
		for (i = 0; i < xlrec->ntablespaces; ++i)
		{
			rlocator.spcOid = xlrec->tablespace_ids[i];
			BlockRefTableSetLimitBlock(brtab, &rlocator, MAIN_FORKNUM, 0);
		}
	}
}

/*
 * Special handling for WAL records with RM_SMGR_ID.
 */
static void
SummarizeSmgrRecord(XLogReaderState *xlogreader, BlockRefTable *brtab)
{
	uint8		info = XLogRecGetInfo(xlogreader) & ~XLR_INFO_MASK;

	if (info == XLOG_SMGR_CREATE)
	{
		xl_smgr_create *xlrec;

		/*
		 * If a new relation fork is created on disk, there is no point
		 * tracking anything about which blocks have been modified, because
		 * the whole thing will be new. Hence, set the limit block for this
		 * fork to 0.
		 *
		 * Ignore the FSM fork, which is not fully WAL-logged.
		 */
		xlrec = (xl_smgr_create *) XLogRecGetData(xlogreader);

		if (xlrec->forkNum != FSM_FORKNUM)
			BlockRefTableSetLimitBlock(brtab, &xlrec->rlocator,
									   xlrec->forkNum, 0);
	}
	else if (info == XLOG_SMGR_TRUNCATE)
	{
		xl_smgr_truncate *xlrec;

		xlrec = (xl_smgr_truncate *) XLogRecGetData(xlogreader);

		/*
		 * If a relation fork is truncated on disk, there is no point in
		 * tracking anything about block modifications beyond the truncation
		 * point.
		 *
		 * We ignore SMGR_TRUNCATE_FSM here because the FSM isn't fully
		 * WAL-logged and thus we can't track modified blocks for it anyway.
		 */
		if ((xlrec->flags & SMGR_TRUNCATE_HEAP) != 0)
			BlockRefTableSetLimitBlock(brtab, &xlrec->rlocator,
									   MAIN_FORKNUM, xlrec->blkno);
		if ((xlrec->flags & SMGR_TRUNCATE_VM) != 0)
			BlockRefTableSetLimitBlock(brtab, &xlrec->rlocator,
									   VISIBILITYMAP_FORKNUM, xlrec->blkno);
	}
}

/*
 * Special handling for WAL records with RM_XACT_ID.
 */
static void
SummarizeXactRecord(XLogReaderState *xlogreader, BlockRefTable *brtab)
{
	uint8		info = XLogRecGetInfo(xlogreader) & ~XLR_INFO_MASK;
	uint8		xact_info = info & XLOG_XACT_OPMASK;

	if (xact_info == XLOG_XACT_COMMIT ||
		xact_info == XLOG_XACT_COMMIT_PREPARED)
	{
		xl_xact_commit *xlrec = (xl_xact_commit *) XLogRecGetData(xlogreader);
		xl_xact_parsed_commit parsed;
		int			i;

		/*
		 * Don't track modified blocks for any relations that were removed on
		 * commit.
		 */
		ParseCommitRecord(XLogRecGetInfo(xlogreader), xlrec, &parsed);
		for (i = 0; i < parsed.nrels; ++i)
		{
			ForkNumber	forknum;

			for (forknum = 0; forknum <= MAX_FORKNUM; ++forknum)
				if (forknum != FSM_FORKNUM)
					BlockRefTableSetLimitBlock(brtab, &parsed.xlocators[i],
											   forknum, 0);
		}
	}
	else if (xact_info == XLOG_XACT_ABORT ||
			 xact_info == XLOG_XACT_ABORT_PREPARED)
	{
		xl_xact_abort *xlrec = (xl_xact_abort *) XLogRecGetData(xlogreader);
		xl_xact_parsed_abort parsed;
		int			i;

		/*
		 * Don't track modified blocks for any relations that were removed on
		 * abort.
		 */
		ParseAbortRecord(XLogRecGetInfo(xlogreader), xlrec, &parsed);
		for (i = 0; i < parsed.nrels; ++i)
		{
			ForkNumber	forknum;

			for (forknum = 0; forknum <= MAX_FORKNUM; ++forknum)
				if (forknum != FSM_FORKNUM)
					BlockRefTableSetLimitBlock(brtab, &parsed.xlocators[i],
											   forknum, 0);
		}
	}
}

/*
 * Special handling for WAL records with RM_XLOG_ID.
 *
 * The return value is true if WAL summarization should stop before this
 * record and false otherwise. When the return value is true,
 * *new_fast_forward indicates whether future processing should be done
 * in fast forward mode (i.e. read WAL without emitting summaries) or not.
 */
static bool
SummarizeXlogRecord(XLogReaderState *xlogreader, bool *new_fast_forward)
{
	uint8		info = XLogRecGetInfo(xlogreader) & ~XLR_INFO_MASK;
	int			record_wal_level;

	if (info == XLOG_CHECKPOINT_REDO)
	{
		/* Payload is wal_level at the time record was written. */
		memcpy(&record_wal_level, XLogRecGetData(xlogreader), sizeof(int));
	}
	else if (info == XLOG_CHECKPOINT_SHUTDOWN)
	{
		CheckPoint	rec_ckpt;

		/* Extract wal_level at time record was written from payload. */
		memcpy(&rec_ckpt, XLogRecGetData(xlogreader), sizeof(CheckPoint));
		record_wal_level = rec_ckpt.wal_level;
	}
	else if (info == XLOG_PARAMETER_CHANGE)
	{
		xl_parameter_change xlrec;

		/* Extract wal_level at time record was written from payload. */
		memcpy(&xlrec, XLogRecGetData(xlogreader),
			   sizeof(xl_parameter_change));
		record_wal_level = xlrec.wal_level;
	}
	else if (info == XLOG_END_OF_RECOVERY)
	{
		xl_end_of_recovery xlrec;

		/* Extract wal_level at time record was written from payload. */
		memcpy(&xlrec, XLogRecGetData(xlogreader), sizeof(xl_end_of_recovery));
		record_wal_level = xlrec.wal_level;
	}
	else
	{
		/* No special handling required. Return false. */
		return false;
	}

	/*
	 * Redo can only begin at an XLOG_CHECKPOINT_REDO or
	 * XLOG_CHECKPOINT_SHUTDOWN record, so we want WAL summarization to begin
	 * at those points. Hence, when those records are encountered, return
	 * true, so that we stop just before summarizing either of those records.
	 *
	 * We also reach here if we just saw XLOG_END_OF_RECOVERY or
	 * XLOG_PARAMETER_CHANGE. These are not places where recovery can start,
	 * but they're still relevant here. A new timeline can begin with
	 * XLOG_END_OF_RECOVERY, so we need to confirm the WAL level at that
	 * point; and a restart can provoke XLOG_PARAMETER_CHANGE after an
	 * intervening change to postgresql.conf, which might force us to stop
	 * summarizing.
	 */
	*new_fast_forward = (record_wal_level == WAL_LEVEL_MINIMAL);
	return true;
}

/*
 * Similar to read_local_xlog_page, but limited to read from one particular
 * timeline. If the end of WAL is reached, it will wait for more if reading
 * from the current timeline, or give up if reading from a historic timeline.
 * In the latter case, it will also set private_data->end_of_wal = true.
 *
 * Caller must set private_data->tli to the TLI of interest,
 * private_data->read_upto to the lowest LSN that is not known to be safe
 * to read on that timeline, and private_data->historic to true if and only
 * if the timeline is not the current timeline. This function will update
 * private_data->read_upto and private_data->historic if more WAL appears
 * on the current timeline or if the current timeline becomes historic.
 */
static int
summarizer_read_local_xlog_page(XLogReaderState *state,
								XLogRecPtr targetPagePtr, int reqLen,
								XLogRecPtr targetRecPtr, char *cur_page)
{
	int			count;
	WALReadError errinfo;
	SummarizerReadLocalXLogPrivate *private_data;

	HandleWalSummarizerInterrupts();

	private_data = (SummarizerReadLocalXLogPrivate *)
		state->private_data;

	while (1)
	{
		if (targetPagePtr + XLOG_BLCKSZ <= private_data->read_upto)
		{
			/*
			 * more than one block available; read only that block, have
			 * caller come back if they need more.
			 */
			count = XLOG_BLCKSZ;
			break;
		}
		else if (targetPagePtr + reqLen > private_data->read_upto)
		{
			/* We don't seem to have enough data. */
			if (private_data->historic)
			{
				/*
				 * This is a historic timeline, so there will never be any
				 * more data than we have currently.
				 */
				private_data->end_of_wal = true;
				return -1;
			}
			else
			{
				XLogRecPtr	latest_lsn;
				TimeLineID	latest_tli;

				/*
				 * This is - or at least was up until very recently - the
				 * current timeline, so more data might show up.  Delay here
				 * so we don't tight-loop.
				 */
				HandleWalSummarizerInterrupts();
				summarizer_wait_for_wal();

				/* Recheck end-of-WAL. */
				latest_lsn = GetLatestLSN(&latest_tli);
				if (private_data->tli == latest_tli)
				{
					/* Still the current timeline, update max LSN. */
					Assert(latest_lsn >= private_data->read_upto);
					private_data->read_upto = latest_lsn;
				}
				else
				{
					List	   *tles = readTimeLineHistory(latest_tli);
					XLogRecPtr	switchpoint;

					/*
					 * The timeline we're scanning is no longer the latest
					 * one. Figure out when it ended.
					 */
					private_data->historic = true;
					switchpoint = tliSwitchPoint(private_data->tli, tles,
												 NULL);

					/*
					 * Allow reads up to exactly the switch point.
					 *
					 * It's possible that this will cause read_upto to move
					 * backwards, because we might have been promoted before
					 * reaching the end of the previous timeline. In that
					 * case, the next loop iteration will likely conclude that
					 * we've reached end of WAL.
					 */
					private_data->read_upto = switchpoint;

					/* Debugging output. */
					ereport(DEBUG1,
							errmsg_internal("timeline %u became historic, can read up to %X/%X",
											private_data->tli, LSN_FORMAT_ARGS(private_data->read_upto)));
				}

				/* Go around and try again. */
			}
		}
		else
		{
			/* enough bytes available to satisfy the request */
			count = private_data->read_upto - targetPagePtr;
			break;
		}
	}

	if (!WALRead(state, cur_page, targetPagePtr, count,
				 private_data->tli, &errinfo))
		WALReadRaiseError(&errinfo);

	/* Track that we read a page, for sleep time calculation. */
	++pages_read_since_last_sleep;

	/* number of valid bytes in the buffer */
	return count;
}

/*
 * Sleep for long enough that we believe it's likely that more WAL will
 * be available afterwards.
 */
static void
summarizer_wait_for_wal(void)
{
	if (pages_read_since_last_sleep == 0)
	{
		/*
		 * No pages were read since the last sleep, so double the sleep time,
		 * but not beyond the maximum allowable value.
		 */
		sleep_quanta = Min(sleep_quanta * 2, MAX_SLEEP_QUANTA);
	}
	else if (pages_read_since_last_sleep > 1)
	{
		/*
		 * Multiple pages were read since the last sleep, so reduce the sleep
		 * time.
		 *
		 * A large burst of activity should be able to quickly reduce the
		 * sleep time to the minimum, but we don't want a handful of extra WAL
		 * records to provoke a strong reaction. We choose to reduce the sleep
		 * time by 1 quantum for each page read beyond the first, which is a
		 * fairly arbitrary way of trying to be reactive without overreacting.
		 */
		if (pages_read_since_last_sleep > sleep_quanta - 1)
			sleep_quanta = 1;
		else
			sleep_quanta -= pages_read_since_last_sleep;
	}

	/* OK, now sleep. */
	(void) WaitLatch(MyLatch,
					 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					 sleep_quanta * MS_PER_SLEEP_QUANTUM,
					 WAIT_EVENT_WAL_SUMMARIZER_WAL);
	ResetLatch(MyLatch);

	/* Reset count of pages read. */
	pages_read_since_last_sleep = 0;
}

/*
 * Remove WAL summaries whose mtimes are older than wal_summary_keep_time.
 */
static void
MaybeRemoveOldWalSummaries(void)
{
	XLogRecPtr	redo_pointer = GetRedoRecPtr();
	List	   *wslist;
	time_t		cutoff_time;

	/* If WAL summary removal is disabled, don't do anything. */
	if (wal_summary_keep_time == 0)
		return;

	/*
	 * If the redo pointer has not advanced, don't do anything.
	 *
	 * This has the effect that we only try to remove old WAL summary files
	 * once per checkpoint cycle.
	 */
	if (redo_pointer == redo_pointer_at_last_summary_removal)
		return;
	redo_pointer_at_last_summary_removal = redo_pointer;

	/*
	 * Files should only be removed if the last modification time precedes the
	 * cutoff time we compute here.
	 */
	cutoff_time = time(NULL) - wal_summary_keep_time * SECS_PER_MINUTE;

	/* Get all the summaries that currently exist. */
	wslist = GetWalSummaries(0, InvalidXLogRecPtr, InvalidXLogRecPtr);

	/* Loop until all summaries have been considered for removal. */
	while (wslist != NIL)
	{
		ListCell   *lc;
		XLogSegNo	oldest_segno;
		XLogRecPtr	oldest_lsn = InvalidXLogRecPtr;
		TimeLineID	selected_tli;

		HandleWalSummarizerInterrupts();

		/*
		 * Pick a timeline for which some summary files still exist on disk,
		 * and find the oldest LSN that still exists on disk for that
		 * timeline.
		 */
		selected_tli = ((WalSummaryFile *) linitial(wslist))->tli;
		oldest_segno = XLogGetOldestSegno(selected_tli);
		if (oldest_segno != 0)
			XLogSegNoOffsetToRecPtr(oldest_segno, 0, wal_segment_size,
									oldest_lsn);


		/* Consider each WAL file on the selected timeline in turn. */
		foreach(lc, wslist)
		{
			WalSummaryFile *ws = lfirst(lc);

			HandleWalSummarizerInterrupts();

			/* If it's not on this timeline, it's not time to consider it. */
			if (selected_tli != ws->tli)
				continue;

			/*
			 * If the WAL doesn't exist any more, we can remove it if the file
			 * modification time is old enough.
			 */
			if (XLogRecPtrIsInvalid(oldest_lsn) || ws->end_lsn <= oldest_lsn)
				RemoveWalSummaryIfOlderThan(ws, cutoff_time);

			/*
			 * Whether we removed the file or not, we need not consider it
			 * again.
			 */
			wslist = foreach_delete_current(wslist, lc);
			pfree(ws);
		}
	}
}
