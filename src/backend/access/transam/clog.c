/*-------------------------------------------------------------------------
 *
 * clog.c
 *		PostgreSQL transaction-commit-log manager
 *
 * This module stores two bits per transaction regarding its commit/abort
 * status; the status for four transactions fit in a byte.
 *
 * This would be a pretty simple abstraction on top of slru.c, except that
 * for performance reasons we allow multiple transactions that are
 * committing concurrently to form a queue, so that a single process can
 * update the status for all of them within a single lock acquisition run.
 *
 * XLOG interactions: this module generates an XLOG record whenever a new
 * CLOG page is initialized to zeroes.  Other writes of CLOG come from
 * recording of transaction commit or abort in xact.c, which generates its
 * own XLOG records for these events and will re-perform the status update
 * on redo; so we need make no additional XLOG entry here.  For synchronous
 * transaction commits, the XLOG is guaranteed flushed through the XLOG commit
 * record before we are called to log a commit, so the WAL rule "write xlog
 * before data" is satisfied automatically.  However, for async commits we
 * must track the latest LSN affecting each CLOG page, so that we can flush
 * XLOG that far and satisfy the WAL rule.  We don't have to worry about this
 * for aborts (whether sync or async), since the post-crash assumption would
 * be that such transactions failed anyway.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/clog.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/clog.h"
#include "access/slru.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "pgstat.h"
#include "storage/proc.h"
#include "storage/sync.h"
#include "utils/guc_hooks.h"

/*
 * Defines for CLOG page sizes.  A page is the same BLCKSZ as is used
 * everywhere else in Postgres.
 *
 * Note: because TransactionIds are 32 bits and wrap around at 0xFFFFFFFF,
 * CLOG page numbering also wraps around at 0xFFFFFFFF/CLOG_XACTS_PER_PAGE,
 * and CLOG segment numbering at
 * 0xFFFFFFFF/CLOG_XACTS_PER_PAGE/SLRU_PAGES_PER_SEGMENT.  We need take no
 * explicit notice of that fact in this module, except when comparing segment
 * and page numbers in TruncateCLOG (see CLOGPagePrecedes).
 */

/* We need two bits per xact, so four xacts fit in a byte */
#define CLOG_BITS_PER_XACT	2
#define CLOG_XACTS_PER_BYTE 4
#define CLOG_XACTS_PER_PAGE (BLCKSZ * CLOG_XACTS_PER_BYTE)
#define CLOG_XACT_BITMASK	((1 << CLOG_BITS_PER_XACT) - 1)

/*
 * Because space used in CLOG by each transaction is so small, we place a
 * smaller limit on the number of CLOG buffers than SLRU allows.  No other
 * SLRU needs this.
 */
#define CLOG_MAX_ALLOWED_BUFFERS \
	Min(SLRU_MAX_ALLOWED_BUFFERS, \
		(((MaxTransactionId / 2) + (CLOG_XACTS_PER_PAGE - 1)) / CLOG_XACTS_PER_PAGE))


/*
 * Although we return an int64 the actual value can't currently exceed
 * 0xFFFFFFFF/CLOG_XACTS_PER_PAGE.
 */
static inline int64
TransactionIdToPage(TransactionId xid)
{
	return xid / (int64) CLOG_XACTS_PER_PAGE;
}

#define TransactionIdToPgIndex(xid) ((xid) % (TransactionId) CLOG_XACTS_PER_PAGE)
#define TransactionIdToByte(xid)	(TransactionIdToPgIndex(xid) / CLOG_XACTS_PER_BYTE)
#define TransactionIdToBIndex(xid)	((xid) % (TransactionId) CLOG_XACTS_PER_BYTE)

/* We store the latest async LSN for each group of transactions */
#define CLOG_XACTS_PER_LSN_GROUP	32	/* keep this a power of 2 */
#define CLOG_LSNS_PER_PAGE	(CLOG_XACTS_PER_PAGE / CLOG_XACTS_PER_LSN_GROUP)

#define GetLSNIndex(slotno, xid)	((slotno) * CLOG_LSNS_PER_PAGE + \
	((xid) % (TransactionId) CLOG_XACTS_PER_PAGE) / CLOG_XACTS_PER_LSN_GROUP)

/*
 * The number of subtransactions below which we consider to apply clog group
 * update optimization.  Testing reveals that the number higher than this can
 * hurt performance.
 */
#define THRESHOLD_SUBTRANS_CLOG_OPT	5

/*
 * Link to shared-memory data structures for CLOG control
 */
static SlruCtlData XactCtlData;

#define XactCtl (&XactCtlData)


static int	ZeroCLOGPage(int64 pageno, bool writeXlog);
static bool CLOGPagePrecedes(int64 page1, int64 page2);
static void WriteZeroPageXlogRec(int64 pageno);
static void WriteTruncateXlogRec(int64 pageno, TransactionId oldestXact,
								 Oid oldestXactDb);
static void TransactionIdSetPageStatus(TransactionId xid, int nsubxids,
									   TransactionId *subxids, XidStatus status,
									   XLogRecPtr lsn, int64 pageno,
									   bool all_xact_same_page);
static void TransactionIdSetStatusBit(TransactionId xid, XidStatus status,
									  XLogRecPtr lsn, int slotno);
static void set_status_by_pages(int nsubxids, TransactionId *subxids,
								XidStatus status, XLogRecPtr lsn);
static bool TransactionGroupUpdateXidStatus(TransactionId xid,
											XidStatus status, XLogRecPtr lsn, int64 pageno);
static void TransactionIdSetPageStatusInternal(TransactionId xid, int nsubxids,
											   TransactionId *subxids, XidStatus status,
											   XLogRecPtr lsn, int64 pageno);


/*
 * TransactionIdSetTreeStatus
 *
 * Record the final state of transaction entries in the commit log for
 * a transaction and its subtransaction tree. Take care to ensure this is
 * efficient, and as atomic as possible.
 *
 * xid is a single xid to set status for. This will typically be
 * the top level transactionid for a top level commit or abort. It can
 * also be a subtransaction when we record transaction aborts.
 *
 * subxids is an array of xids of length nsubxids, representing subtransactions
 * in the tree of xid. In various cases nsubxids may be zero.
 *
 * lsn must be the WAL location of the commit record when recording an async
 * commit.  For a synchronous commit it can be InvalidXLogRecPtr, since the
 * caller guarantees the commit record is already flushed in that case.  It
 * should be InvalidXLogRecPtr for abort cases, too.
 *
 * In the commit case, atomicity is limited by whether all the subxids are in
 * the same CLOG page as xid.  If they all are, then the lock will be grabbed
 * only once, and the status will be set to committed directly.  Otherwise
 * we must
 *	 1. set sub-committed all subxids that are not on the same page as the
 *		main xid
 *	 2. atomically set committed the main xid and the subxids on the same page
 *	 3. go over the first bunch again and set them committed
 * Note that as far as concurrent checkers are concerned, main transaction
 * commit as a whole is still atomic.
 *
 * Example:
 *		TransactionId t commits and has subxids t1, t2, t3, t4
 *		t is on page p1, t1 is also on p1, t2 and t3 are on p2, t4 is on p3
 *		1. update pages2-3:
 *					page2: set t2,t3 as sub-committed
 *					page3: set t4 as sub-committed
 *		2. update page1:
 *					page1: set t,t1 as committed
 *		3. update pages2-3:
 *					page2: set t2,t3 as committed
 *					page3: set t4 as committed
 *
 * NB: this is a low-level routine and is NOT the preferred entry point
 * for most uses; functions in transam.c are the intended callers.
 *
 * XXX Think about issuing POSIX_FADV_WILLNEED on pages that we will need,
 * but aren't yet in cache, as well as hinting pages not to fall out of
 * cache yet.
 */
void
TransactionIdSetTreeStatus(TransactionId xid, int nsubxids,
						   TransactionId *subxids, XidStatus status, XLogRecPtr lsn)
{
	int64		pageno = TransactionIdToPage(xid);	/* get page of parent */
	int			i;

	Assert(status == TRANSACTION_STATUS_COMMITTED ||
		   status == TRANSACTION_STATUS_ABORTED);

	/*
	 * See how many subxids, if any, are on the same page as the parent, if
	 * any.
	 */
	for (i = 0; i < nsubxids; i++)
	{
		if (TransactionIdToPage(subxids[i]) != pageno)
			break;
	}

	/*
	 * Do all items fit on a single page?
	 */
	if (i == nsubxids)
	{
		/*
		 * Set the parent and all subtransactions in a single call
		 */
		TransactionIdSetPageStatus(xid, nsubxids, subxids, status, lsn,
								   pageno, true);
	}
	else
	{
		int			nsubxids_on_first_page = i;

		/*
		 * If this is a commit then we care about doing this correctly (i.e.
		 * using the subcommitted intermediate status).  By here, we know
		 * we're updating more than one page of clog, so we must mark entries
		 * that are *not* on the first page so that they show as subcommitted
		 * before we then return to update the status to fully committed.
		 *
		 * To avoid touching the first page twice, skip marking subcommitted
		 * for the subxids on that first page.
		 */
		if (status == TRANSACTION_STATUS_COMMITTED)
			set_status_by_pages(nsubxids - nsubxids_on_first_page,
								subxids + nsubxids_on_first_page,
								TRANSACTION_STATUS_SUB_COMMITTED, lsn);

		/*
		 * Now set the parent and subtransactions on same page as the parent,
		 * if any
		 */
		pageno = TransactionIdToPage(xid);
		TransactionIdSetPageStatus(xid, nsubxids_on_first_page, subxids, status,
								   lsn, pageno, false);

		/*
		 * Now work through the rest of the subxids one clog page at a time,
		 * starting from the second page onwards, like we did above.
		 */
		set_status_by_pages(nsubxids - nsubxids_on_first_page,
							subxids + nsubxids_on_first_page,
							status, lsn);
	}
}

/*
 * Helper for TransactionIdSetTreeStatus: set the status for a bunch of
 * transactions, chunking in the separate CLOG pages involved. We never
 * pass the whole transaction tree to this function, only subtransactions
 * that are on different pages to the top level transaction id.
 */
static void
set_status_by_pages(int nsubxids, TransactionId *subxids,
					XidStatus status, XLogRecPtr lsn)
{
	int64		pageno = TransactionIdToPage(subxids[0]);
	int			offset = 0;
	int			i = 0;

	Assert(nsubxids > 0);		/* else the pageno fetch above is unsafe */

	while (i < nsubxids)
	{
		int			num_on_page = 0;
		int64		nextpageno;

		do
		{
			nextpageno = TransactionIdToPage(subxids[i]);
			if (nextpageno != pageno)
				break;
			num_on_page++;
			i++;
		} while (i < nsubxids);

		TransactionIdSetPageStatus(InvalidTransactionId,
								   num_on_page, subxids + offset,
								   status, lsn, pageno, false);
		offset = i;
		pageno = nextpageno;
	}
}

/*
 * Record the final state of transaction entries in the commit log for all
 * entries on a single page.  Atomic only on this page.
 */
static void
TransactionIdSetPageStatus(TransactionId xid, int nsubxids,
						   TransactionId *subxids, XidStatus status,
						   XLogRecPtr lsn, int64 pageno,
						   bool all_xact_same_page)
{
	LWLock	   *lock;

	/* Can't use group update when PGPROC overflows. */
	StaticAssertDecl(THRESHOLD_SUBTRANS_CLOG_OPT <= PGPROC_MAX_CACHED_SUBXIDS,
					 "group clog threshold less than PGPROC cached subxids");

	/* Get the SLRU bank lock for the page we are going to access. */
	lock = SimpleLruGetBankLock(XactCtl, pageno);

	/*
	 * When there is contention on the SLRU bank lock we need, we try to group
	 * multiple updates; a single leader process will perform transaction
	 * status updates for multiple backends so that the number of times the
	 * bank lock needs to be acquired is reduced.
	 *
	 * For this optimization to be safe, the XID and subxids in MyProc must be
	 * the same as the ones for which we're setting the status.  Check that
	 * this is the case.
	 *
	 * For this optimization to be efficient, we shouldn't have too many
	 * sub-XIDs and all of the XIDs for which we're adjusting clog should be
	 * on the same page.  Check those conditions, too.
	 */
	if (all_xact_same_page && xid == MyProc->xid &&
		nsubxids <= THRESHOLD_SUBTRANS_CLOG_OPT &&
		nsubxids == MyProc->subxidStatus.count &&
		(nsubxids == 0 ||
		 memcmp(subxids, MyProc->subxids.xids,
				nsubxids * sizeof(TransactionId)) == 0))
	{
		/*
		 * If we can immediately acquire the lock, we update the status of our
		 * own XID and release the lock.  If not, try use group XID update. If
		 * that doesn't work out, fall back to waiting for the lock to perform
		 * an update for this transaction only.
		 */
		if (LWLockConditionalAcquire(lock, LW_EXCLUSIVE))
		{
			/* Got the lock without waiting!  Do the update. */
			TransactionIdSetPageStatusInternal(xid, nsubxids, subxids, status,
											   lsn, pageno);
			LWLockRelease(lock);
			return;
		}
		else if (TransactionGroupUpdateXidStatus(xid, status, lsn, pageno))
		{
			/* Group update mechanism has done the work. */
			return;
		}

		/* Fall through only if update isn't done yet. */
	}

	/* Group update not applicable, or couldn't accept this page number. */
	LWLockAcquire(lock, LW_EXCLUSIVE);
	TransactionIdSetPageStatusInternal(xid, nsubxids, subxids, status,
									   lsn, pageno);
	LWLockRelease(lock);
}

/*
 * Record the final state of transaction entry in the commit log
 *
 * We don't do any locking here; caller must handle that.
 */
static void
TransactionIdSetPageStatusInternal(TransactionId xid, int nsubxids,
								   TransactionId *subxids, XidStatus status,
								   XLogRecPtr lsn, int64 pageno)
{
	int			slotno;
	int			i;

	Assert(status == TRANSACTION_STATUS_COMMITTED ||
		   status == TRANSACTION_STATUS_ABORTED ||
		   (status == TRANSACTION_STATUS_SUB_COMMITTED && !TransactionIdIsValid(xid)));
	Assert(LWLockHeldByMeInMode(SimpleLruGetBankLock(XactCtl, pageno),
								LW_EXCLUSIVE));

	/*
	 * If we're doing an async commit (ie, lsn is valid), then we must wait
	 * for any active write on the page slot to complete.  Otherwise our
	 * update could reach disk in that write, which will not do since we
	 * mustn't let it reach disk until we've done the appropriate WAL flush.
	 * But when lsn is invalid, it's OK to scribble on a page while it is
	 * write-busy, since we don't care if the update reaches disk sooner than
	 * we think.
	 */
	slotno = SimpleLruReadPage(XactCtl, pageno, XLogRecPtrIsInvalid(lsn), xid);

	/*
	 * Set the main transaction id, if any.
	 *
	 * If we update more than one xid on this page while it is being written
	 * out, we might find that some of the bits go to disk and others don't.
	 * If we are updating commits on the page with the top-level xid that
	 * could break atomicity, so we subcommit the subxids first before we mark
	 * the top-level commit.
	 */
	if (TransactionIdIsValid(xid))
	{
		/* Subtransactions first, if needed ... */
		if (status == TRANSACTION_STATUS_COMMITTED)
		{
			for (i = 0; i < nsubxids; i++)
			{
				Assert(XactCtl->shared->page_number[slotno] == TransactionIdToPage(subxids[i]));
				TransactionIdSetStatusBit(subxids[i],
										  TRANSACTION_STATUS_SUB_COMMITTED,
										  lsn, slotno);
			}
		}

		/* ... then the main transaction */
		TransactionIdSetStatusBit(xid, status, lsn, slotno);
	}

	/* Set the subtransactions */
	for (i = 0; i < nsubxids; i++)
	{
		Assert(XactCtl->shared->page_number[slotno] == TransactionIdToPage(subxids[i]));
		TransactionIdSetStatusBit(subxids[i], status, lsn, slotno);
	}

	XactCtl->shared->page_dirty[slotno] = true;
}

/*
 * Subroutine for TransactionIdSetPageStatus, q.v.
 *
 * When we cannot immediately acquire the SLRU bank lock in exclusive mode at
 * commit time, add ourselves to a list of processes that need their XIDs
 * status update.  The first process to add itself to the list will acquire
 * the lock in exclusive mode and set transaction status as required on behalf
 * of all group members.  This avoids a great deal of contention when many
 * processes are trying to commit at once, since the lock need not be
 * repeatedly handed off from one committing process to the next.
 *
 * Returns true when transaction status has been updated in clog; returns
 * false if we decided against applying the optimization because the page
 * number we need to update differs from those processes already waiting.
 */
static bool
TransactionGroupUpdateXidStatus(TransactionId xid, XidStatus status,
								XLogRecPtr lsn, int64 pageno)
{
	volatile PROC_HDR *procglobal = ProcGlobal;
	PGPROC	   *proc = MyProc;
	uint32		nextidx;
	uint32		wakeidx;
	int64		prevpageno;
	LWLock	   *prevlock = NULL;

	/* We should definitely have an XID whose status needs to be updated. */
	Assert(TransactionIdIsValid(xid));

	/*
	 * Prepare to add ourselves to the list of processes needing a group XID
	 * status update.
	 */
	proc->clogGroupMember = true;
	proc->clogGroupMemberXid = xid;
	proc->clogGroupMemberXidStatus = status;
	proc->clogGroupMemberPage = pageno;
	proc->clogGroupMemberLsn = lsn;

	/*
	 * We put ourselves in the queue by writing MyProcNumber to
	 * ProcGlobal->clogGroupFirst.  However, if there's already a process
	 * listed there, we compare our pageno with that of that process; if it
	 * differs, we cannot participate in the group, so we return for caller to
	 * update pg_xact in the normal way.
	 *
	 * If we're not the first process in the list, we must follow the leader.
	 * We do this by storing the data we want updated in our PGPROC entry
	 * where the leader can find it, then going to sleep.
	 *
	 * If no process is already in the list, we're the leader; our first step
	 * is to lock the SLRU bank to which our page belongs, then we close out
	 * the group by resetting the list pointer from ProcGlobal->clogGroupFirst
	 * (this lets other processes set up other groups later); finally we do
	 * the SLRU updates, release the SLRU bank lock, and wake up the sleeping
	 * processes.
	 *
	 * If another group starts to update a page in a different SLRU bank, they
	 * can proceed concurrently, since the bank lock they're going to use is
	 * different from ours.  If another group starts to update a page in the
	 * same bank as ours, they wait until we release the lock.
	 */
	nextidx = pg_atomic_read_u32(&procglobal->clogGroupFirst);

	while (true)
	{
		/*
		 * Add the proc to list, if the clog page where we need to update the
		 * current transaction status is same as group leader's clog page.
		 *
		 * There is a race condition here, which is that after doing the below
		 * check and before adding this proc's clog update to a group, the
		 * group leader might have already finished the group update for this
		 * page and becomes group leader of another group, updating a
		 * different page.  This will lead to a situation where a single group
		 * can have different clog page updates.  This isn't likely and will
		 * still work, just less efficiently -- we handle this case by
		 * switching to a different bank lock in the loop below.
		 */
		if (nextidx != INVALID_PROC_NUMBER &&
			GetPGProcByNumber(nextidx)->clogGroupMemberPage != proc->clogGroupMemberPage)
		{
			/*
			 * Ensure that this proc is not a member of any clog group that
			 * needs an XID status update.
			 */
			proc->clogGroupMember = false;
			pg_atomic_write_u32(&proc->clogGroupNext, INVALID_PROC_NUMBER);
			return false;
		}

		pg_atomic_write_u32(&proc->clogGroupNext, nextidx);

		if (pg_atomic_compare_exchange_u32(&procglobal->clogGroupFirst,
										   &nextidx,
										   (uint32) MyProcNumber))
			break;
	}

	/*
	 * If the list was not empty, the leader will update the status of our
	 * XID. It is impossible to have followers without a leader because the
	 * first process that has added itself to the list will always have
	 * nextidx as INVALID_PROC_NUMBER.
	 */
	if (nextidx != INVALID_PROC_NUMBER)
	{
		int			extraWaits = 0;

		/* Sleep until the leader updates our XID status. */
		pgstat_report_wait_start(WAIT_EVENT_XACT_GROUP_UPDATE);
		for (;;)
		{
			/* acts as a read barrier */
			PGSemaphoreLock(proc->sem);
			if (!proc->clogGroupMember)
				break;
			extraWaits++;
		}
		pgstat_report_wait_end();

		Assert(pg_atomic_read_u32(&proc->clogGroupNext) == INVALID_PROC_NUMBER);

		/* Fix semaphore count for any absorbed wakeups */
		while (extraWaits-- > 0)
			PGSemaphoreUnlock(proc->sem);
		return true;
	}

	/*
	 * By here, we know we're the leader process.  Acquire the SLRU bank lock
	 * that corresponds to the page we originally wanted to modify.
	 */
	prevpageno = proc->clogGroupMemberPage;
	prevlock = SimpleLruGetBankLock(XactCtl, prevpageno);
	LWLockAcquire(prevlock, LW_EXCLUSIVE);

	/*
	 * Now that we've got the lock, clear the list of processes waiting for
	 * group XID status update, saving a pointer to the head of the list.
	 * (Trying to pop elements one at a time could lead to an ABA problem.)
	 *
	 * At this point, any processes trying to do this would create a separate
	 * group.
	 */
	nextidx = pg_atomic_exchange_u32(&procglobal->clogGroupFirst,
									 INVALID_PROC_NUMBER);

	/* Remember head of list so we can perform wakeups after dropping lock. */
	wakeidx = nextidx;

	/* Walk the list and update the status of all XIDs. */
	while (nextidx != INVALID_PROC_NUMBER)
	{
		PGPROC	   *nextproc = &ProcGlobal->allProcs[nextidx];
		int64		thispageno = nextproc->clogGroupMemberPage;

		/*
		 * If the page to update belongs to a different bank than the previous
		 * one, exchange bank lock to the new one.  This should be quite rare,
		 * as described above.
		 *
		 * (We could try to optimize this by waking up the processes for which
		 * we have already updated the status while we exchange the lock, but
		 * the code doesn't do that at present.  I think it'd require
		 * additional bookkeeping, making the common path slower in order to
		 * improve an infrequent case.)
		 */
		if (thispageno != prevpageno)
		{
			LWLock	   *lock = SimpleLruGetBankLock(XactCtl, thispageno);

			if (prevlock != lock)
			{
				LWLockRelease(prevlock);
				LWLockAcquire(lock, LW_EXCLUSIVE);
			}
			prevlock = lock;
			prevpageno = thispageno;
		}

		/*
		 * Transactions with more than THRESHOLD_SUBTRANS_CLOG_OPT sub-XIDs
		 * should not use group XID status update mechanism.
		 */
		Assert(nextproc->subxidStatus.count <= THRESHOLD_SUBTRANS_CLOG_OPT);

		TransactionIdSetPageStatusInternal(nextproc->clogGroupMemberXid,
										   nextproc->subxidStatus.count,
										   nextproc->subxids.xids,
										   nextproc->clogGroupMemberXidStatus,
										   nextproc->clogGroupMemberLsn,
										   nextproc->clogGroupMemberPage);

		/* Move to next proc in list. */
		nextidx = pg_atomic_read_u32(&nextproc->clogGroupNext);
	}

	/* We're done with the lock now. */
	if (prevlock != NULL)
		LWLockRelease(prevlock);

	/*
	 * Now that we've released the lock, go back and wake everybody up.  We
	 * don't do this under the lock so as to keep lock hold times to a
	 * minimum.
	 *
	 * (Perhaps we could do this in two passes, the first setting
	 * clogGroupNext to invalid while saving the semaphores to an array, then
	 * a single write barrier, then another pass unlocking the semaphores.)
	 */
	while (wakeidx != INVALID_PROC_NUMBER)
	{
		PGPROC	   *wakeproc = &ProcGlobal->allProcs[wakeidx];

		wakeidx = pg_atomic_read_u32(&wakeproc->clogGroupNext);
		pg_atomic_write_u32(&wakeproc->clogGroupNext, INVALID_PROC_NUMBER);

		/* ensure all previous writes are visible before follower continues. */
		pg_write_barrier();

		wakeproc->clogGroupMember = false;

		if (wakeproc != MyProc)
			PGSemaphoreUnlock(wakeproc->sem);
	}

	return true;
}

/*
 * Sets the commit status of a single transaction.
 *
 * Caller must hold the corresponding SLRU bank lock, will be held at exit.
 */
static void
TransactionIdSetStatusBit(TransactionId xid, XidStatus status, XLogRecPtr lsn, int slotno)
{
	int			byteno = TransactionIdToByte(xid);
	int			bshift = TransactionIdToBIndex(xid) * CLOG_BITS_PER_XACT;
	char	   *byteptr;
	char		byteval;
	char		curval;

	Assert(XactCtl->shared->page_number[slotno] == TransactionIdToPage(xid));
	Assert(LWLockHeldByMeInMode(SimpleLruGetBankLock(XactCtl,
													 XactCtl->shared->page_number[slotno]),
								LW_EXCLUSIVE));

	byteptr = XactCtl->shared->page_buffer[slotno] + byteno;
	curval = (*byteptr >> bshift) & CLOG_XACT_BITMASK;

	/*
	 * When replaying transactions during recovery we still need to perform
	 * the two phases of subcommit and then commit. However, some transactions
	 * are already correctly marked, so we just treat those as a no-op which
	 * allows us to keep the following Assert as restrictive as possible.
	 */
	if (InRecovery && status == TRANSACTION_STATUS_SUB_COMMITTED &&
		curval == TRANSACTION_STATUS_COMMITTED)
		return;

	/*
	 * Current state change should be from 0 or subcommitted to target state
	 * or we should already be there when replaying changes during recovery.
	 */
	Assert(curval == 0 ||
		   (curval == TRANSACTION_STATUS_SUB_COMMITTED &&
			status != TRANSACTION_STATUS_IN_PROGRESS) ||
		   curval == status);

	/* note this assumes exclusive access to the clog page */
	byteval = *byteptr;
	byteval &= ~(((1 << CLOG_BITS_PER_XACT) - 1) << bshift);
	byteval |= (status << bshift);
	*byteptr = byteval;

	/*
	 * Update the group LSN if the transaction completion LSN is higher.
	 *
	 * Note: lsn will be invalid when supplied during InRecovery processing,
	 * so we don't need to do anything special to avoid LSN updates during
	 * recovery. After recovery completes the next clog change will set the
	 * LSN correctly.
	 */
	if (!XLogRecPtrIsInvalid(lsn))
	{
		int			lsnindex = GetLSNIndex(slotno, xid);

		if (XactCtl->shared->group_lsn[lsnindex] < lsn)
			XactCtl->shared->group_lsn[lsnindex] = lsn;
	}
}

/*
 * Interrogate the state of a transaction in the commit log.
 *
 * Aside from the actual commit status, this function returns (into *lsn)
 * an LSN that is late enough to be able to guarantee that if we flush up to
 * that LSN then we will have flushed the transaction's commit record to disk.
 * The result is not necessarily the exact LSN of the transaction's commit
 * record!	For example, for long-past transactions (those whose clog pages
 * already migrated to disk), we'll return InvalidXLogRecPtr.  Also, because
 * we group transactions on the same clog page to conserve storage, we might
 * return the LSN of a later transaction that falls into the same group.
 *
 * NB: this is a low-level routine and is NOT the preferred entry point
 * for most uses; TransactionLogFetch() in transam.c is the intended caller.
 */
XidStatus
TransactionIdGetStatus(TransactionId xid, XLogRecPtr *lsn)
{
	int64		pageno = TransactionIdToPage(xid);
	int			byteno = TransactionIdToByte(xid);
	int			bshift = TransactionIdToBIndex(xid) * CLOG_BITS_PER_XACT;
	int			slotno;
	int			lsnindex;
	char	   *byteptr;
	XidStatus	status;

	/* lock is acquired by SimpleLruReadPage_ReadOnly */

	slotno = SimpleLruReadPage_ReadOnly(XactCtl, pageno, xid);
	byteptr = XactCtl->shared->page_buffer[slotno] + byteno;

	status = (*byteptr >> bshift) & CLOG_XACT_BITMASK;

	lsnindex = GetLSNIndex(slotno, xid);
	*lsn = XactCtl->shared->group_lsn[lsnindex];

	LWLockRelease(SimpleLruGetBankLock(XactCtl, pageno));

	return status;
}

/*
 * Number of shared CLOG buffers.
 *
 * If asked to autotune, use 2MB for every 1GB of shared buffers, up to 8MB.
 * Otherwise just cap the configured amount to be between 16 and the maximum
 * allowed.
 */
static int
CLOGShmemBuffers(void)
{
	/* auto-tune based on shared buffers */
	if (transaction_buffers == 0)
		return SimpleLruAutotuneBuffers(512, 1024);

	return Min(Max(16, transaction_buffers), CLOG_MAX_ALLOWED_BUFFERS);
}

/*
 * Initialization of shared memory for CLOG
 */
Size
CLOGShmemSize(void)
{
	return SimpleLruShmemSize(CLOGShmemBuffers(), CLOG_LSNS_PER_PAGE);
}

void
CLOGShmemInit(void)
{
	/* If auto-tuning is requested, now is the time to do it */
	if (transaction_buffers == 0)
	{
		char		buf[32];

		snprintf(buf, sizeof(buf), "%d", CLOGShmemBuffers());
		SetConfigOption("transaction_buffers", buf, PGC_POSTMASTER,
						PGC_S_DYNAMIC_DEFAULT);

		/*
		 * We prefer to report this value's source as PGC_S_DYNAMIC_DEFAULT.
		 * However, if the DBA explicitly set transaction_buffers = 0 in the
		 * config file, then PGC_S_DYNAMIC_DEFAULT will fail to override that
		 * and we must force the matter with PGC_S_OVERRIDE.
		 */
		if (transaction_buffers == 0)	/* failed to apply it? */
			SetConfigOption("transaction_buffers", buf, PGC_POSTMASTER,
							PGC_S_OVERRIDE);
	}
	Assert(transaction_buffers != 0);

	XactCtl->PagePrecedes = CLOGPagePrecedes;
	SimpleLruInit(XactCtl, "transaction", CLOGShmemBuffers(), CLOG_LSNS_PER_PAGE,
				  "pg_xact", LWTRANCHE_XACT_BUFFER,
				  LWTRANCHE_XACT_SLRU, SYNC_HANDLER_CLOG, false);
	SlruPagePrecedesUnitTests(XactCtl, CLOG_XACTS_PER_PAGE);
}

/*
 * GUC check_hook for transaction_buffers
 */
bool
check_transaction_buffers(int *newval, void **extra, GucSource source)
{
	return check_slru_buffers("transaction_buffers", newval);
}

/*
 * This func must be called ONCE on system install.  It creates
 * the initial CLOG segment.  (The CLOG directory is assumed to
 * have been created by initdb, and CLOGShmemInit must have been
 * called already.)
 */
void
BootStrapCLOG(void)
{
	int			slotno;
	LWLock	   *lock = SimpleLruGetBankLock(XactCtl, 0);

	LWLockAcquire(lock, LW_EXCLUSIVE);

	/* Create and zero the first page of the commit log */
	slotno = ZeroCLOGPage(0, false);

	/* Make sure it's written out */
	SimpleLruWritePage(XactCtl, slotno);
	Assert(!XactCtl->shared->page_dirty[slotno]);

	LWLockRelease(lock);
}

/*
 * Initialize (or reinitialize) a page of CLOG to zeroes.
 * If writeXlog is true, also emit an XLOG record saying we did this.
 *
 * The page is not actually written, just set up in shared memory.
 * The slot number of the new page is returned.
 *
 * Control lock must be held at entry, and will be held at exit.
 */
static int
ZeroCLOGPage(int64 pageno, bool writeXlog)
{
	int			slotno;

	slotno = SimpleLruZeroPage(XactCtl, pageno);

	if (writeXlog)
		WriteZeroPageXlogRec(pageno);

	return slotno;
}

/*
 * This must be called ONCE during postmaster or standalone-backend startup,
 * after StartupXLOG has initialized TransamVariables->nextXid.
 */
void
StartupCLOG(void)
{
	TransactionId xid = XidFromFullTransactionId(TransamVariables->nextXid);
	int64		pageno = TransactionIdToPage(xid);

	/*
	 * Initialize our idea of the latest page number.
	 */
	pg_atomic_write_u64(&XactCtl->shared->latest_page_number, pageno);
}

/*
 * This must be called ONCE at the end of startup/recovery.
 */
void
TrimCLOG(void)
{
	TransactionId xid = XidFromFullTransactionId(TransamVariables->nextXid);
	int64		pageno = TransactionIdToPage(xid);
	LWLock	   *lock = SimpleLruGetBankLock(XactCtl, pageno);

	LWLockAcquire(lock, LW_EXCLUSIVE);

	/*
	 * Zero out the remainder of the current clog page.  Under normal
	 * circumstances it should be zeroes already, but it seems at least
	 * theoretically possible that XLOG replay will have settled on a nextXID
	 * value that is less than the last XID actually used and marked by the
	 * previous database lifecycle (since subtransaction commit writes clog
	 * but makes no WAL entry).  Let's just be safe. (We need not worry about
	 * pages beyond the current one, since those will be zeroed when first
	 * used.  For the same reason, there is no need to do anything when
	 * nextXid is exactly at a page boundary; and it's likely that the
	 * "current" page doesn't exist yet in that case.)
	 */
	if (TransactionIdToPgIndex(xid) != 0)
	{
		int			byteno = TransactionIdToByte(xid);
		int			bshift = TransactionIdToBIndex(xid) * CLOG_BITS_PER_XACT;
		int			slotno;
		char	   *byteptr;

		slotno = SimpleLruReadPage(XactCtl, pageno, false, xid);
		byteptr = XactCtl->shared->page_buffer[slotno] + byteno;

		/* Zero so-far-unused positions in the current byte */
		*byteptr &= (1 << bshift) - 1;
		/* Zero the rest of the page */
		MemSet(byteptr + 1, 0, BLCKSZ - byteno - 1);

		XactCtl->shared->page_dirty[slotno] = true;
	}

	LWLockRelease(lock);
}

/*
 * Perform a checkpoint --- either during shutdown, or on-the-fly
 */
void
CheckPointCLOG(void)
{
	/*
	 * Write dirty CLOG pages to disk.  This may result in sync requests
	 * queued for later handling by ProcessSyncRequests(), as part of the
	 * checkpoint.
	 */
	TRACE_POSTGRESQL_CLOG_CHECKPOINT_START(true);
	SimpleLruWriteAll(XactCtl, true);
	TRACE_POSTGRESQL_CLOG_CHECKPOINT_DONE(true);
}


/*
 * Make sure that CLOG has room for a newly-allocated XID.
 *
 * NB: this is called while holding XidGenLock.  We want it to be very fast
 * most of the time; even when it's not so fast, no actual I/O need happen
 * unless we're forced to write out a dirty clog or xlog page to make room
 * in shared memory.
 */
void
ExtendCLOG(TransactionId newestXact)
{
	int64		pageno;
	LWLock	   *lock;

	/*
	 * No work except at first XID of a page.  But beware: just after
	 * wraparound, the first XID of page zero is FirstNormalTransactionId.
	 */
	if (TransactionIdToPgIndex(newestXact) != 0 &&
		!TransactionIdEquals(newestXact, FirstNormalTransactionId))
		return;

	pageno = TransactionIdToPage(newestXact);
	lock = SimpleLruGetBankLock(XactCtl, pageno);

	LWLockAcquire(lock, LW_EXCLUSIVE);

	/* Zero the page and make an XLOG entry about it */
	ZeroCLOGPage(pageno, true);

	LWLockRelease(lock);
}


/*
 * Remove all CLOG segments before the one holding the passed transaction ID
 *
 * Before removing any CLOG data, we must flush XLOG to disk, to ensure that
 * any recently-emitted records with freeze plans have reached disk; otherwise
 * a crash and restart might leave us with some unfrozen tuples referencing
 * removed CLOG data.  We choose to emit a special TRUNCATE XLOG record too.
 * Replaying the deletion from XLOG is not critical, since the files could
 * just as well be removed later, but doing so prevents a long-running hot
 * standby server from acquiring an unreasonably bloated CLOG directory.
 *
 * Since CLOG segments hold a large number of transactions, the opportunity to
 * actually remove a segment is fairly rare, and so it seems best not to do
 * the XLOG flush unless we have confirmed that there is a removable segment.
 */
void
TruncateCLOG(TransactionId oldestXact, Oid oldestxid_datoid)
{
	int64		cutoffPage;

	/*
	 * The cutoff point is the start of the segment containing oldestXact. We
	 * pass the *page* containing oldestXact to SimpleLruTruncate.
	 */
	cutoffPage = TransactionIdToPage(oldestXact);

	/* Check to see if there's any files that could be removed */
	if (!SlruScanDirectory(XactCtl, SlruScanDirCbReportPresence, &cutoffPage))
		return;					/* nothing to remove */

	/*
	 * Advance oldestClogXid before truncating clog, so concurrent xact status
	 * lookups can ensure they don't attempt to access truncated-away clog.
	 *
	 * It's only necessary to do this if we will actually truncate away clog
	 * pages.
	 */
	AdvanceOldestClogXid(oldestXact);

	/*
	 * Write XLOG record and flush XLOG to disk. We record the oldest xid
	 * we're keeping information about here so we can ensure that it's always
	 * ahead of clog truncation in case we crash, and so a standby finds out
	 * the new valid xid before the next checkpoint.
	 */
	WriteTruncateXlogRec(cutoffPage, oldestXact, oldestxid_datoid);

	/* Now we can remove the old CLOG segment(s) */
	SimpleLruTruncate(XactCtl, cutoffPage);
}


/*
 * Decide whether a CLOG page number is "older" for truncation purposes.
 *
 * We need to use comparison of TransactionIds here in order to do the right
 * thing with wraparound XID arithmetic.  However, TransactionIdPrecedes()
 * would get weird about permanent xact IDs.  So, offset both such that xid1,
 * xid2, and xid2 + CLOG_XACTS_PER_PAGE - 1 are all normal XIDs; this offset
 * is relevant to page 0 and to the page preceding page 0.
 *
 * The page containing oldestXact-2^31 is the important edge case.  The
 * portion of that page equaling or following oldestXact-2^31 is expendable,
 * but the portion preceding oldestXact-2^31 is not.  When oldestXact-2^31 is
 * the first XID of a page and segment, the entire page and segment is
 * expendable, and we could truncate the segment.  Recognizing that case would
 * require making oldestXact, not just the page containing oldestXact,
 * available to this callback.  The benefit would be rare and small, so we
 * don't optimize that edge case.
 */
static bool
CLOGPagePrecedes(int64 page1, int64 page2)
{
	TransactionId xid1;
	TransactionId xid2;

	xid1 = ((TransactionId) page1) * CLOG_XACTS_PER_PAGE;
	xid1 += FirstNormalTransactionId + 1;
	xid2 = ((TransactionId) page2) * CLOG_XACTS_PER_PAGE;
	xid2 += FirstNormalTransactionId + 1;

	return (TransactionIdPrecedes(xid1, xid2) &&
			TransactionIdPrecedes(xid1, xid2 + CLOG_XACTS_PER_PAGE - 1));
}


/*
 * Write a ZEROPAGE xlog record
 */
static void
WriteZeroPageXlogRec(int64 pageno)
{
	XLogBeginInsert();
	XLogRegisterData(&pageno, sizeof(pageno));
	(void) XLogInsert(RM_CLOG_ID, CLOG_ZEROPAGE);
}

/*
 * Write a TRUNCATE xlog record
 *
 * We must flush the xlog record to disk before returning --- see notes
 * in TruncateCLOG().
 */
static void
WriteTruncateXlogRec(int64 pageno, TransactionId oldestXact, Oid oldestXactDb)
{
	XLogRecPtr	recptr;
	xl_clog_truncate xlrec;

	xlrec.pageno = pageno;
	xlrec.oldestXact = oldestXact;
	xlrec.oldestXactDb = oldestXactDb;

	XLogBeginInsert();
	XLogRegisterData(&xlrec, sizeof(xl_clog_truncate));
	recptr = XLogInsert(RM_CLOG_ID, CLOG_TRUNCATE);
	XLogFlush(recptr);
}

/*
 * CLOG resource manager's routines
 */
void
clog_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	/* Backup blocks are not used in clog records */
	Assert(!XLogRecHasAnyBlockRefs(record));

	if (info == CLOG_ZEROPAGE)
	{
		int64		pageno;
		int			slotno;
		LWLock	   *lock;

		memcpy(&pageno, XLogRecGetData(record), sizeof(pageno));

		lock = SimpleLruGetBankLock(XactCtl, pageno);
		LWLockAcquire(lock, LW_EXCLUSIVE);

		slotno = ZeroCLOGPage(pageno, false);
		SimpleLruWritePage(XactCtl, slotno);
		Assert(!XactCtl->shared->page_dirty[slotno]);

		LWLockRelease(lock);
	}
	else if (info == CLOG_TRUNCATE)
	{
		xl_clog_truncate xlrec;

		memcpy(&xlrec, XLogRecGetData(record), sizeof(xl_clog_truncate));

		AdvanceOldestClogXid(xlrec.oldestXact);

		SimpleLruTruncate(XactCtl, xlrec.pageno);
	}
	else
		elog(PANIC, "clog_redo: unknown op code %u", info);
}

/*
 * Entrypoint for sync.c to sync clog files.
 */
int
clogsyncfiletag(const FileTag *ftag, char *path)
{
	return SlruSyncFileTag(XactCtl, ftag, path);
}
