/*-------------------------------------------------------------------------
 *
 * subtrans.c
 *		PostgreSQL subtrans-log manager
 *
 * The pg_subtrans manager is a pg_clog-like manager which stores the parent
 * transaction Id for each transaction.  It is a fundamental part of the
 * nested transactions implementation.  A main transaction has a parent
 * of InvalidTransactionId, and each subtransaction has its immediate parent.
 * The tree can easily be walked from child to parent, but not in the
 * opposite direction.
 *
 * This code is mostly derived from clog.c.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/backend/access/transam/subtrans.c,v 1.2 2004/08/22 02:41:57 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/slru.h"
#include "access/subtrans.h"
#include "miscadmin.h"
#include "storage/lwlock.h"
#include "utils/tqual.h"


/*
 * Defines for SubTrans page and segment sizes.  A page is the same BLCKSZ
 * as is used everywhere else in Postgres.
 *
 * Note: because TransactionIds are 32 bits and wrap around at 0xFFFFFFFF,
 * SubTrans page numbering also wraps around at
 * 0xFFFFFFFF/SUBTRANS_XACTS_PER_PAGE, and segment numbering at
 * 0xFFFFFFFF/SUBTRANS_XACTS_PER_PAGE/SLRU_SEGMENTS_PER_PAGE.  We need take no
 * explicit notice of that fact in this module, except when comparing segment
 * and page numbers in TruncateSubTrans (see SubTransPagePrecedes).
 */

/* We need four bytes per xact */
#define SUBTRANS_XACTS_PER_PAGE (BLCKSZ / sizeof(TransactionId))

#define TransactionIdToPage(xid) ((xid) / (TransactionId) SUBTRANS_XACTS_PER_PAGE)
#define TransactionIdToEntry(xid) ((xid) % (TransactionId) SUBTRANS_XACTS_PER_PAGE)


/*----------
 * Shared-memory data structures for SUBTRANS control
 *
 * XLOG interactions: this module generates an XLOG record whenever a new
 * SUBTRANS page is initialized to zeroes.	Other writes of SUBTRANS come from
 * recording of transaction commit or abort in xact.c, which generates its
 * own XLOG records for these events and will re-perform the status update
 * on redo; so we need make no additional XLOG entry here.	Also, the XLOG
 * is guaranteed flushed through the XLOG commit record before we are called
 * to log a commit, so the WAL rule "write xlog before data" is satisfied
 * automatically for commits, and we don't really care for aborts.  Therefore,
 * we don't need to mark SUBTRANS pages with LSN information; we have enough
 * synchronization already.
 *----------
 */


static SlruCtlData SubTransCtlData;
static SlruCtl SubTransCtl = &SubTransCtlData;


static int	ZeroSUBTRANSPage(int pageno, bool writeXlog);
static bool SubTransPagePrecedes(int page1, int page2);
static void WriteZeroPageXlogRec(int pageno);


/*
 * Record the parent of a subtransaction in the subtrans log.
 */
void
SubTransSetParent(TransactionId xid, TransactionId parent)
{
	int			pageno = TransactionIdToPage(xid);
	int			entryno = TransactionIdToEntry(xid);
	TransactionId *ptr;

	LWLockAcquire(SubTransCtl->ControlLock, LW_EXCLUSIVE);

	ptr = (TransactionId *) SimpleLruReadPage(SubTransCtl, pageno, xid, true);
	ptr += entryno;

	/* Current state should be 0 or target state */
	Assert(*ptr == InvalidTransactionId || *ptr == parent);

	*ptr = parent;

	/* ...->page_status[slotno] = SLRU_PAGE_DIRTY; already done */

	LWLockRelease(SubTransCtl->ControlLock);
}

/*
 * Interrogate the parent of a transaction in the subtrans log.
 */
TransactionId
SubTransGetParent(TransactionId xid)
{
	int			pageno = TransactionIdToPage(xid);
	int			entryno = TransactionIdToEntry(xid);
	TransactionId *ptr;
	TransactionId	parent;

	/* Can't ask about stuff that might not be around anymore */
	Assert(TransactionIdFollowsOrEquals(xid, RecentXmin));

	/* Bootstrap and frozen XIDs have no parent */
	if (!TransactionIdIsNormal(xid))
		return InvalidTransactionId;

	LWLockAcquire(SubTransCtl->ControlLock, LW_EXCLUSIVE);

	ptr = (TransactionId *) SimpleLruReadPage(SubTransCtl, pageno, xid, false);
	ptr += entryno;

	parent = *ptr;

	LWLockRelease(SubTransCtl->ControlLock);

	return parent;
}

/*
 * SubTransGetTopmostTransaction
 *
 * Returns the topmost transaction of the given transaction id.
 *
 * Because we cannot look back further than RecentXmin, it is possible
 * that this function will lie and return an intermediate subtransaction ID
 * instead of the true topmost parent ID.  This is OK, because in practice
 * we only care about detecting whether the topmost parent is still running
 * or is part of a current snapshot's list of still-running transactions.
 * Therefore, any XID before RecentXmin is as good as any other.
 */
TransactionId
SubTransGetTopmostTransaction(TransactionId xid)
{
	TransactionId parentXid = xid,
				  previousXid = xid;

	/* Can't ask about stuff that might not be around anymore */
	Assert(TransactionIdFollowsOrEquals(xid, RecentXmin));

	while (TransactionIdIsValid(parentXid))
	{
		previousXid = parentXid;
		if (TransactionIdPrecedes(parentXid, RecentXmin))
			break;
		parentXid = SubTransGetParent(parentXid);
	}

	Assert(TransactionIdIsValid(previousXid));

	return previousXid;
}


/*
 * Initialization of shared memory for Subtrans
 */

int
SUBTRANSShmemSize(void)
{
	return SimpleLruShmemSize();
}

void
SUBTRANSShmemInit(void)
{
	SimpleLruInit(SubTransCtl, "SUBTRANS Ctl", "pg_subtrans");
	SubTransCtl->PagePrecedes = SubTransPagePrecedes;
}

/*
 * This func must be called ONCE on system install.  It creates
 * the initial SubTrans segment.  (The SubTrans directory is assumed to
 * have been created by initdb, and SubTransShmemInit must have been called
 * already.)
 */
void
BootStrapSUBTRANS(void)
{
	int			slotno;

	LWLockAcquire(SubTransCtl->ControlLock, LW_EXCLUSIVE);

	/* Create and zero the first page of the commit log */
	slotno = ZeroSUBTRANSPage(0, false);

	/* Make sure it's written out */
	SimpleLruWritePage(SubTransCtl, slotno, NULL);
	/* Assert(SubTransCtl->page_status[slotno] == SLRU_PAGE_CLEAN); */

	LWLockRelease(SubTransCtl->ControlLock);
}

/*
 * Initialize (or reinitialize) a page of SubTrans to zeroes.
 * If writeXlog is TRUE, also emit an XLOG record saying we did this.
 *
 * The page is not actually written, just set up in shared memory.
 * The slot number of the new page is returned.
 *
 * Control lock must be held at entry, and will be held at exit.
 */
static int
ZeroSUBTRANSPage(int pageno, bool writeXlog)
{
	int			slotno = SimpleLruZeroPage(SubTransCtl, pageno);

	if (writeXlog)
		WriteZeroPageXlogRec(pageno);

	return slotno;
}

/*
 * This must be called ONCE during postmaster or standalone-backend startup,
 * after StartupXLOG has initialized ShmemVariableCache->nextXid.
 */
void
StartupSUBTRANS(void)
{
	/*
	 * Initialize our idea of the latest page number.
	 */
	SimpleLruSetLatestPage(SubTransCtl,
						   TransactionIdToPage(ShmemVariableCache->nextXid));
}

/*
 * This must be called ONCE during postmaster or standalone-backend shutdown
 */
void
ShutdownSUBTRANS(void)
{
	SimpleLruFlush(SubTransCtl, false);
}

/*
 * Perform a checkpoint --- either during shutdown, or on-the-fly
 */
void
CheckPointSUBTRANS(void)
{
	SimpleLruFlush(SubTransCtl, true);
}


/*
 * Make sure that SubTrans has room for a newly-allocated XID.
 *
 * NB: this is called while holding XidGenLock.  We want it to be very fast
 * most of the time; even when it's not so fast, no actual I/O need happen
 * unless we're forced to write out a dirty subtrans or xlog page to make room
 * in shared memory.
 */
void
ExtendSUBTRANS(TransactionId newestXact)
{
	int			pageno;

	/*
	 * No work except at first XID of a page.  But beware: just after
	 * wraparound, the first XID of page zero is FirstNormalTransactionId.
	 */
	if (TransactionIdToEntry(newestXact) != 0 &&
		!TransactionIdEquals(newestXact, FirstNormalTransactionId))
		return;

	pageno = TransactionIdToPage(newestXact);

	LWLockAcquire(SubTransCtl->ControlLock, LW_EXCLUSIVE);

	/* Zero the page and make an XLOG entry about it */
	ZeroSUBTRANSPage(pageno, true);

	LWLockRelease(SubTransCtl->ControlLock);
}


/*
 * Remove all SubTrans segments before the one holding the passed transaction ID
 *
 * When this is called, we know that the database logically contains no
 * reference to transaction IDs older than oldestXact.	However, we must
 * not truncate the SubTrans until we have performed a checkpoint, to ensure
 * that no such references remain on disk either; else a crash just after
 * the truncation might leave us with a problem.  Since SubTrans segments hold
 * a large number of transactions, the opportunity to actually remove a
 * segment is fairly rare, and so it seems best not to do the checkpoint
 * unless we have confirmed that there is a removable segment.	Therefore
 * we issue the checkpoint command here, not in higher-level code as might
 * seem cleaner.
 */
void
TruncateSUBTRANS(TransactionId oldestXact)
{
	int			cutoffPage;

	/*
	 * The cutoff point is the start of the segment containing oldestXact.
	 * We pass the *page* containing oldestXact to SimpleLruTruncate.
	 */
	cutoffPage = TransactionIdToPage(oldestXact);
	SimpleLruTruncate(SubTransCtl, cutoffPage);
}


/*
 * Decide which of two SubTrans page numbers is "older" for truncation purposes.
 *
 * We need to use comparison of TransactionIds here in order to do the right
 * thing with wraparound XID arithmetic.  However, if we are asked about
 * page number zero, we don't want to hand InvalidTransactionId to
 * TransactionIdPrecedes: it'll get weird about permanent xact IDs.  So,
 * offset both xids by FirstNormalTransactionId to avoid that.
 */
static bool
SubTransPagePrecedes(int page1, int page2)
{
	TransactionId xid1;
	TransactionId xid2;

	xid1 = ((TransactionId) page1) * SUBTRANS_XACTS_PER_PAGE;
	xid1 += FirstNormalTransactionId;
	xid2 = ((TransactionId) page2) * SUBTRANS_XACTS_PER_PAGE;
	xid2 += FirstNormalTransactionId;

	return TransactionIdPrecedes(xid1, xid2);
}


/*
 * Write a ZEROPAGE xlog record
 *
 * Note: xlog record is marked as outside transaction control, since we
 * want it to be redone whether the invoking transaction commits or not.
 * (Besides which, this is normally done just before entering a transaction.)
 */
static void
WriteZeroPageXlogRec(int pageno)
{
	XLogRecData rdata;

	rdata.buffer = InvalidBuffer;
	rdata.data = (char *) (&pageno);
	rdata.len = sizeof(int);
	rdata.next = NULL;
	(void) XLogInsert(RM_SLRU_ID, SUBTRANS_ZEROPAGE | XLOG_NO_TRAN, &rdata);
}

/* Redo a ZEROPAGE action during WAL replay */
void
subtrans_zeropage_redo(int pageno)
{
	int			slotno;

	LWLockAcquire(SubTransCtl->ControlLock, LW_EXCLUSIVE);

	slotno = ZeroSUBTRANSPage(pageno, false);
	SimpleLruWritePage(SubTransCtl, slotno, NULL);
	/* Assert(SubTransCtl->page_status[slotno] == SLRU_PAGE_CLEAN); */

	LWLockRelease(SubTransCtl->ControlLock);
}
