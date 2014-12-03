/*-------------------------------------------------------------------------
 *
 * clog.c
 *		PostgreSQL transaction-commit-log manager
 *
 * This module replaces the old "pg_log" access code, which treated pg_log
 * essentially like a relation, in that it went through the regular buffer
 * manager.  The problem with that was that there wasn't any good way to
 * recycle storage space for transactions so old that they'll never be
 * looked up again.  Now we use specialized access code so that the commit
 * log can be broken into relatively small, independent segments.
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
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
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
#include "miscadmin.h"
#include "pg_trace.h"

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

#define TransactionIdToPage(xid)	((xid) / (TransactionId) CLOG_XACTS_PER_PAGE)
#define TransactionIdToPgIndex(xid) ((xid) % (TransactionId) CLOG_XACTS_PER_PAGE)
#define TransactionIdToByte(xid)	(TransactionIdToPgIndex(xid) / CLOG_XACTS_PER_BYTE)
#define TransactionIdToBIndex(xid)	((xid) % (TransactionId) CLOG_XACTS_PER_BYTE)

/* We store the latest async LSN for each group of transactions */
#define CLOG_XACTS_PER_LSN_GROUP	32	/* keep this a power of 2 */
#define CLOG_LSNS_PER_PAGE	(CLOG_XACTS_PER_PAGE / CLOG_XACTS_PER_LSN_GROUP)

#define GetLSNIndex(slotno, xid)	((slotno) * CLOG_LSNS_PER_PAGE + \
	((xid) % (TransactionId) CLOG_XACTS_PER_PAGE) / CLOG_XACTS_PER_LSN_GROUP)


/*
 * Link to shared-memory data structures for CLOG control
 */
static SlruCtlData ClogCtlData;

#define ClogCtl (&ClogCtlData)


static int	ZeroCLOGPage(int pageno, bool writeXlog);
static bool CLOGPagePrecedes(int page1, int page2);
static void WriteZeroPageXlogRec(int pageno);
static void WriteTruncateXlogRec(int pageno);
static void TransactionIdSetPageStatus(TransactionId xid, int nsubxids,
						   TransactionId *subxids, XidStatus status,
						   XLogRecPtr lsn, int pageno);
static void TransactionIdSetStatusBit(TransactionId xid, XidStatus status,
						  XLogRecPtr lsn, int slotno);
static void set_status_by_pages(int nsubxids, TransactionId *subxids,
					XidStatus status, XLogRecPtr lsn);


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
 *					set t1 as sub-committed,
 *					then set t as committed,
					then set t1 as committed
 *		3. update pages2-3:
 *					page2: set t2,t3 as committed
 *					page3: set t4 as committed
 *
 * NB: this is a low-level routine and is NOT the preferred entry point
 * for most uses; functions in transam.c are the intended callers.
 *
 * XXX Think about issuing FADVISE_WILLNEED on pages that we will need,
 * but aren't yet in cache, as well as hinting pages not to fall out of
 * cache yet.
 */
void
TransactionIdSetTreeStatus(TransactionId xid, int nsubxids,
					TransactionId *subxids, XidStatus status, XLogRecPtr lsn)
{
	int			pageno = TransactionIdToPage(xid);		/* get page of parent */
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
								   pageno);
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
								   lsn, pageno);

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
	int			pageno = TransactionIdToPage(subxids[0]);
	int			offset = 0;
	int			i = 0;

	while (i < nsubxids)
	{
		int			num_on_page = 0;

		while (TransactionIdToPage(subxids[i]) == pageno && i < nsubxids)
		{
			num_on_page++;
			i++;
		}

		TransactionIdSetPageStatus(InvalidTransactionId,
								   num_on_page, subxids + offset,
								   status, lsn, pageno);
		offset = i;
		pageno = TransactionIdToPage(subxids[offset]);
	}
}

/*
 * Record the final state of transaction entries in the commit log for
 * all entries on a single page.  Atomic only on this page.
 *
 * Otherwise API is same as TransactionIdSetTreeStatus()
 */
static void
TransactionIdSetPageStatus(TransactionId xid, int nsubxids,
						   TransactionId *subxids, XidStatus status,
						   XLogRecPtr lsn, int pageno)
{
	int			slotno;
	int			i;

	Assert(status == TRANSACTION_STATUS_COMMITTED ||
		   status == TRANSACTION_STATUS_ABORTED ||
		   (status == TRANSACTION_STATUS_SUB_COMMITTED && !TransactionIdIsValid(xid)));

	LWLockAcquire(CLogControlLock, LW_EXCLUSIVE);

	/*
	 * If we're doing an async commit (ie, lsn is valid), then we must wait
	 * for any active write on the page slot to complete.  Otherwise our
	 * update could reach disk in that write, which will not do since we
	 * mustn't let it reach disk until we've done the appropriate WAL flush.
	 * But when lsn is invalid, it's OK to scribble on a page while it is
	 * write-busy, since we don't care if the update reaches disk sooner than
	 * we think.
	 */
	slotno = SimpleLruReadPage(ClogCtl, pageno, XLogRecPtrIsInvalid(lsn), xid);

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
				Assert(ClogCtl->shared->page_number[slotno] == TransactionIdToPage(subxids[i]));
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
		Assert(ClogCtl->shared->page_number[slotno] == TransactionIdToPage(subxids[i]));
		TransactionIdSetStatusBit(subxids[i], status, lsn, slotno);
	}

	ClogCtl->shared->page_dirty[slotno] = true;

	LWLockRelease(CLogControlLock);
}

/*
 * Sets the commit status of a single transaction.
 *
 * Must be called with CLogControlLock held
 */
static void
TransactionIdSetStatusBit(TransactionId xid, XidStatus status, XLogRecPtr lsn, int slotno)
{
	int			byteno = TransactionIdToByte(xid);
	int			bshift = TransactionIdToBIndex(xid) * CLOG_BITS_PER_XACT;
	char	   *byteptr;
	char		byteval;
	char		curval;

	byteptr = ClogCtl->shared->page_buffer[slotno] + byteno;
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

		if (ClogCtl->shared->group_lsn[lsnindex] < lsn)
			ClogCtl->shared->group_lsn[lsnindex] = lsn;
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
	int			pageno = TransactionIdToPage(xid);
	int			byteno = TransactionIdToByte(xid);
	int			bshift = TransactionIdToBIndex(xid) * CLOG_BITS_PER_XACT;
	int			slotno;
	int			lsnindex;
	char	   *byteptr;
	XidStatus	status;

	/* lock is acquired by SimpleLruReadPage_ReadOnly */

	slotno = SimpleLruReadPage_ReadOnly(ClogCtl, pageno, xid);
	byteptr = ClogCtl->shared->page_buffer[slotno] + byteno;

	status = (*byteptr >> bshift) & CLOG_XACT_BITMASK;

	lsnindex = GetLSNIndex(slotno, xid);
	*lsn = ClogCtl->shared->group_lsn[lsnindex];

	LWLockRelease(CLogControlLock);

	return status;
}

/*
 * Number of shared CLOG buffers.
 *
 * Testing during the PostgreSQL 9.2 development cycle revealed that on a
 * large multi-processor system, it was possible to have more CLOG page
 * requests in flight at one time than the number of CLOG buffers which existed
 * at that time, which was hardcoded to 8.  Further testing revealed that
 * performance dropped off with more than 32 CLOG buffers, possibly because
 * the linear buffer search algorithm doesn't scale well.
 *
 * Unconditionally increasing the number of CLOG buffers to 32 did not seem
 * like a good idea, because it would increase the minimum amount of shared
 * memory required to start, which could be a problem for people running very
 * small configurations.  The following formula seems to represent a reasonable
 * compromise: people with very low values for shared_buffers will get fewer
 * CLOG buffers as well, and everyone else will get 32.
 *
 * It is likely that some further work will be needed here in future releases;
 * for example, on a 64-core server, the maximum number of CLOG requests that
 * can be simultaneously in flight will be even larger.  But that will
 * apparently require more than just changing the formula, so for now we take
 * the easy way out.
 */
Size
CLOGShmemBuffers(void)
{
	return Min(32, Max(4, NBuffers / 512));
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
	ClogCtl->PagePrecedes = CLOGPagePrecedes;
	SimpleLruInit(ClogCtl, "CLOG Ctl", CLOGShmemBuffers(), CLOG_LSNS_PER_PAGE,
				  CLogControlLock, "pg_clog");
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

	LWLockAcquire(CLogControlLock, LW_EXCLUSIVE);

	/* Create and zero the first page of the commit log */
	slotno = ZeroCLOGPage(0, false);

	/* Make sure it's written out */
	SimpleLruWritePage(ClogCtl, slotno);
	Assert(!ClogCtl->shared->page_dirty[slotno]);

	LWLockRelease(CLogControlLock);
}

/*
 * Initialize (or reinitialize) a page of CLOG to zeroes.
 * If writeXlog is TRUE, also emit an XLOG record saying we did this.
 *
 * The page is not actually written, just set up in shared memory.
 * The slot number of the new page is returned.
 *
 * Control lock must be held at entry, and will be held at exit.
 */
static int
ZeroCLOGPage(int pageno, bool writeXlog)
{
	int			slotno;

	slotno = SimpleLruZeroPage(ClogCtl, pageno);

	if (writeXlog)
		WriteZeroPageXlogRec(pageno);

	return slotno;
}

/*
 * This must be called ONCE during postmaster or standalone-backend startup,
 * after StartupXLOG has initialized ShmemVariableCache->nextXid.
 */
void
StartupCLOG(void)
{
	TransactionId xid = ShmemVariableCache->nextXid;
	int			pageno = TransactionIdToPage(xid);

	LWLockAcquire(CLogControlLock, LW_EXCLUSIVE);

	/*
	 * Initialize our idea of the latest page number.
	 */
	ClogCtl->shared->latest_page_number = pageno;

	LWLockRelease(CLogControlLock);
}

/*
 * This must be called ONCE at the end of startup/recovery.
 */
void
TrimCLOG(void)
{
	TransactionId xid = ShmemVariableCache->nextXid;
	int			pageno = TransactionIdToPage(xid);

	LWLockAcquire(CLogControlLock, LW_EXCLUSIVE);

	/*
	 * Re-Initialize our idea of the latest page number.
	 */
	ClogCtl->shared->latest_page_number = pageno;

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

		slotno = SimpleLruReadPage(ClogCtl, pageno, false, xid);
		byteptr = ClogCtl->shared->page_buffer[slotno] + byteno;

		/* Zero so-far-unused positions in the current byte */
		*byteptr &= (1 << bshift) - 1;
		/* Zero the rest of the page */
		MemSet(byteptr + 1, 0, BLCKSZ - byteno - 1);

		ClogCtl->shared->page_dirty[slotno] = true;
	}

	LWLockRelease(CLogControlLock);
}

/*
 * This must be called ONCE during postmaster or standalone-backend shutdown
 */
void
ShutdownCLOG(void)
{
	/* Flush dirty CLOG pages to disk */
	TRACE_POSTGRESQL_CLOG_CHECKPOINT_START(false);
	SimpleLruFlush(ClogCtl, false);
	TRACE_POSTGRESQL_CLOG_CHECKPOINT_DONE(false);
}

/*
 * Perform a checkpoint --- either during shutdown, or on-the-fly
 */
void
CheckPointCLOG(void)
{
	/* Flush dirty CLOG pages to disk */
	TRACE_POSTGRESQL_CLOG_CHECKPOINT_START(true);
	SimpleLruFlush(ClogCtl, true);
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
	int			pageno;

	/*
	 * No work except at first XID of a page.  But beware: just after
	 * wraparound, the first XID of page zero is FirstNormalTransactionId.
	 */
	if (TransactionIdToPgIndex(newestXact) != 0 &&
		!TransactionIdEquals(newestXact, FirstNormalTransactionId))
		return;

	pageno = TransactionIdToPage(newestXact);

	LWLockAcquire(CLogControlLock, LW_EXCLUSIVE);

	/* Zero the page and make an XLOG entry about it */
	ZeroCLOGPage(pageno, true);

	LWLockRelease(CLogControlLock);
}


/*
 * Remove all CLOG segments before the one holding the passed transaction ID
 *
 * Before removing any CLOG data, we must flush XLOG to disk, to ensure
 * that any recently-emitted HEAP_FREEZE records have reached disk; otherwise
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
TruncateCLOG(TransactionId oldestXact)
{
	int			cutoffPage;

	/*
	 * The cutoff point is the start of the segment containing oldestXact. We
	 * pass the *page* containing oldestXact to SimpleLruTruncate.
	 */
	cutoffPage = TransactionIdToPage(oldestXact);

	/* Check to see if there's any files that could be removed */
	if (!SlruScanDirectory(ClogCtl, SlruScanDirCbReportPresence, &cutoffPage))
		return;					/* nothing to remove */

	/* Write XLOG record and flush XLOG to disk */
	WriteTruncateXlogRec(cutoffPage);

	/* Now we can remove the old CLOG segment(s) */
	SimpleLruTruncate(ClogCtl, cutoffPage);
}


/*
 * Decide which of two CLOG page numbers is "older" for truncation purposes.
 *
 * We need to use comparison of TransactionIds here in order to do the right
 * thing with wraparound XID arithmetic.  However, if we are asked about
 * page number zero, we don't want to hand InvalidTransactionId to
 * TransactionIdPrecedes: it'll get weird about permanent xact IDs.  So,
 * offset both xids by FirstNormalTransactionId to avoid that.
 */
static bool
CLOGPagePrecedes(int page1, int page2)
{
	TransactionId xid1;
	TransactionId xid2;

	xid1 = ((TransactionId) page1) * CLOG_XACTS_PER_PAGE;
	xid1 += FirstNormalTransactionId;
	xid2 = ((TransactionId) page2) * CLOG_XACTS_PER_PAGE;
	xid2 += FirstNormalTransactionId;

	return TransactionIdPrecedes(xid1, xid2);
}


/*
 * Write a ZEROPAGE xlog record
 */
static void
WriteZeroPageXlogRec(int pageno)
{
	XLogRecData rdata;

	rdata.data = (char *) (&pageno);
	rdata.len = sizeof(int);
	rdata.buffer = InvalidBuffer;
	rdata.next = NULL;
	(void) XLogInsert(RM_CLOG_ID, CLOG_ZEROPAGE, &rdata);
}

/*
 * Write a TRUNCATE xlog record
 *
 * We must flush the xlog record to disk before returning --- see notes
 * in TruncateCLOG().
 */
static void
WriteTruncateXlogRec(int pageno)
{
	XLogRecData rdata;
	XLogRecPtr	recptr;

	rdata.data = (char *) (&pageno);
	rdata.len = sizeof(int);
	rdata.buffer = InvalidBuffer;
	rdata.next = NULL;
	recptr = XLogInsert(RM_CLOG_ID, CLOG_TRUNCATE, &rdata);
	XLogFlush(recptr);
}

/*
 * CLOG resource manager's routines
 */
void
clog_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	/* Backup blocks are not used in clog records */
	Assert(!(record->xl_info & XLR_BKP_BLOCK_MASK));

	if (info == CLOG_ZEROPAGE)
	{
		int			pageno;
		int			slotno;

		memcpy(&pageno, XLogRecGetData(record), sizeof(int));

		LWLockAcquire(CLogControlLock, LW_EXCLUSIVE);

		slotno = ZeroCLOGPage(pageno, false);
		SimpleLruWritePage(ClogCtl, slotno);
		Assert(!ClogCtl->shared->page_dirty[slotno]);

		LWLockRelease(CLogControlLock);
	}
	else if (info == CLOG_TRUNCATE)
	{
		int			pageno;

		memcpy(&pageno, XLogRecGetData(record), sizeof(int));

		/*
		 * During XLOG replay, latest_page_number isn't set up yet; insert a
		 * suitable value to bypass the sanity test in SimpleLruTruncate.
		 */
		ClogCtl->shared->latest_page_number = pageno;

		SimpleLruTruncate(ClogCtl, pageno);
	}
	else
		elog(PANIC, "clog_redo: unknown op code %u", info);
}
