/*-------------------------------------------------------------------------
 *
 * subtrans.c
 *		PostgreSQL subtransaction-log manager
 *
 * The pg_subtrans manager is a pg_xact-like manager that stores the parent
 * transaction Id for each transaction.  It is a fundamental part of the
 * nested transactions implementation.  A main transaction has a parent
 * of InvalidTransactionId, and each subtransaction has its immediate parent.
 * The tree can easily be walked from child to parent, but not in the
 * opposite direction.
 *
 * This code is based on xact.c, but the robustness requirements
 * are completely different from pg_xact, because we only need to remember
 * pg_subtrans information for currently-open transactions.  Thus, there is
 * no need to preserve data over a crash and restart.
 *
 * There are no XLOG interactions since we do not care about preserving
 * data across crashes.  During database startup, we simply force the
 * currently-active page of SUBTRANS to zeroes.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/subtrans.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/slru.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "utils/guc_hooks.h"
#include "utils/snapmgr.h"


/*
 * Defines for SubTrans page sizes.  A page is the same BLCKSZ as is used
 * everywhere else in Postgres.
 *
 * Note: because TransactionIds are 32 bits and wrap around at 0xFFFFFFFF,
 * SubTrans page numbering also wraps around at
 * 0xFFFFFFFF/SUBTRANS_XACTS_PER_PAGE, and segment numbering at
 * 0xFFFFFFFF/SUBTRANS_XACTS_PER_PAGE/SLRU_PAGES_PER_SEGMENT.  We need take no
 * explicit notice of that fact in this module, except when comparing segment
 * and page numbers in TruncateSUBTRANS (see SubTransPagePrecedes) and zeroing
 * them in StartupSUBTRANS.
 */

/* We need four bytes per xact */
#define SUBTRANS_XACTS_PER_PAGE (BLCKSZ / sizeof(TransactionId))

/*
 * Although we return an int64 the actual value can't currently exceed
 * 0xFFFFFFFF/SUBTRANS_XACTS_PER_PAGE.
 */
static inline int64
TransactionIdToPage(TransactionId xid)
{
	return xid / (int64) SUBTRANS_XACTS_PER_PAGE;
}

#define TransactionIdToEntry(xid) ((xid) % (TransactionId) SUBTRANS_XACTS_PER_PAGE)


/*
 * Link to shared-memory data structures for SUBTRANS control
 */
static SlruCtlData SubTransCtlData;

#define SubTransCtl  (&SubTransCtlData)


static int	ZeroSUBTRANSPage(int64 pageno);
static bool SubTransPagePrecedes(int64 page1, int64 page2);


/*
 * Record the parent of a subtransaction in the subtrans log.
 */
void
SubTransSetParent(TransactionId xid, TransactionId parent)
{
	int64		pageno = TransactionIdToPage(xid);
	int			entryno = TransactionIdToEntry(xid);
	int			slotno;
	LWLock	   *lock;
	TransactionId *ptr;

	Assert(TransactionIdIsValid(parent));
	Assert(TransactionIdFollows(xid, parent));

	lock = SimpleLruGetBankLock(SubTransCtl, pageno);
	LWLockAcquire(lock, LW_EXCLUSIVE);

	slotno = SimpleLruReadPage(SubTransCtl, pageno, true, xid);
	ptr = (TransactionId *) SubTransCtl->shared->page_buffer[slotno];
	ptr += entryno;

	/*
	 * It's possible we'll try to set the parent xid multiple times but we
	 * shouldn't ever be changing the xid from one valid xid to another valid
	 * xid, which would corrupt the data structure.
	 */
	if (*ptr != parent)
	{
		Assert(*ptr == InvalidTransactionId);
		*ptr = parent;
		SubTransCtl->shared->page_dirty[slotno] = true;
	}

	LWLockRelease(lock);
}

/*
 * Interrogate the parent of a transaction in the subtrans log.
 */
TransactionId
SubTransGetParent(TransactionId xid)
{
	int64		pageno = TransactionIdToPage(xid);
	int			entryno = TransactionIdToEntry(xid);
	int			slotno;
	TransactionId *ptr;
	TransactionId parent;

	/* Can't ask about stuff that might not be around anymore */
	Assert(TransactionIdFollowsOrEquals(xid, TransactionXmin));

	/* Bootstrap and frozen XIDs have no parent */
	if (!TransactionIdIsNormal(xid))
		return InvalidTransactionId;

	/* lock is acquired by SimpleLruReadPage_ReadOnly */

	slotno = SimpleLruReadPage_ReadOnly(SubTransCtl, pageno, xid);
	ptr = (TransactionId *) SubTransCtl->shared->page_buffer[slotno];
	ptr += entryno;

	parent = *ptr;

	LWLockRelease(SimpleLruGetBankLock(SubTransCtl, pageno));

	return parent;
}

/*
 * SubTransGetTopmostTransaction
 *
 * Returns the topmost transaction of the given transaction id.
 *
 * Because we cannot look back further than TransactionXmin, it is possible
 * that this function will lie and return an intermediate subtransaction ID
 * instead of the true topmost parent ID.  This is OK, because in practice
 * we only care about detecting whether the topmost parent is still running
 * or is part of a current snapshot's list of still-running transactions.
 * Therefore, any XID before TransactionXmin is as good as any other.
 */
TransactionId
SubTransGetTopmostTransaction(TransactionId xid)
{
	TransactionId parentXid = xid,
				previousXid = xid;

	/* Can't ask about stuff that might not be around anymore */
	Assert(TransactionIdFollowsOrEquals(xid, TransactionXmin));

	while (TransactionIdIsValid(parentXid))
	{
		previousXid = parentXid;
		if (TransactionIdPrecedes(parentXid, TransactionXmin))
			break;
		parentXid = SubTransGetParent(parentXid);

		/*
		 * By convention the parent xid gets allocated first, so should always
		 * precede the child xid. Anything else points to a corrupted data
		 * structure that could lead to an infinite loop, so exit.
		 */
		if (!TransactionIdPrecedes(parentXid, previousXid))
			elog(ERROR, "pg_subtrans contains invalid entry: xid %u points to parent xid %u",
				 previousXid, parentXid);
	}

	Assert(TransactionIdIsValid(previousXid));

	return previousXid;
}

/*
 * Number of shared SUBTRANS buffers.
 *
 * If asked to autotune, use 2MB for every 1GB of shared buffers, up to 8MB.
 * Otherwise just cap the configured amount to be between 16 and the maximum
 * allowed.
 */
static int
SUBTRANSShmemBuffers(void)
{
	/* auto-tune based on shared buffers */
	if (subtransaction_buffers == 0)
		return SimpleLruAutotuneBuffers(512, 1024);

	return Min(Max(16, subtransaction_buffers), SLRU_MAX_ALLOWED_BUFFERS);
}

/*
 * Initialization of shared memory for SUBTRANS
 */
Size
SUBTRANSShmemSize(void)
{
	return SimpleLruShmemSize(SUBTRANSShmemBuffers(), 0);
}

void
SUBTRANSShmemInit(void)
{
	/* If auto-tuning is requested, now is the time to do it */
	if (subtransaction_buffers == 0)
	{
		char		buf[32];

		snprintf(buf, sizeof(buf), "%d", SUBTRANSShmemBuffers());
		SetConfigOption("subtransaction_buffers", buf, PGC_POSTMASTER,
						PGC_S_DYNAMIC_DEFAULT);

		/*
		 * We prefer to report this value's source as PGC_S_DYNAMIC_DEFAULT.
		 * However, if the DBA explicitly set subtransaction_buffers = 0 in
		 * the config file, then PGC_S_DYNAMIC_DEFAULT will fail to override
		 * that and we must force the matter with PGC_S_OVERRIDE.
		 */
		if (subtransaction_buffers == 0)	/* failed to apply it? */
			SetConfigOption("subtransaction_buffers", buf, PGC_POSTMASTER,
							PGC_S_OVERRIDE);
	}
	Assert(subtransaction_buffers != 0);

	SubTransCtl->PagePrecedes = SubTransPagePrecedes;
	SimpleLruInit(SubTransCtl, "subtransaction", SUBTRANSShmemBuffers(), 0,
				  "pg_subtrans", LWTRANCHE_SUBTRANS_BUFFER,
				  LWTRANCHE_SUBTRANS_SLRU, SYNC_HANDLER_NONE, false);
	SlruPagePrecedesUnitTests(SubTransCtl, SUBTRANS_XACTS_PER_PAGE);
}

/*
 * GUC check_hook for subtransaction_buffers
 */
bool
check_subtrans_buffers(int *newval, void **extra, GucSource source)
{
	return check_slru_buffers("subtransaction_buffers", newval);
}

/*
 * This func must be called ONCE on system install.  It creates
 * the initial SUBTRANS segment.  (The SUBTRANS directory is assumed to
 * have been created by the initdb shell script, and SUBTRANSShmemInit
 * must have been called already.)
 *
 * Note: it's not really necessary to create the initial segment now,
 * since slru.c would create it on first write anyway.  But we may as well
 * do it to be sure the directory is set up correctly.
 */
void
BootStrapSUBTRANS(void)
{
	int			slotno;
	LWLock	   *lock = SimpleLruGetBankLock(SubTransCtl, 0);

	LWLockAcquire(lock, LW_EXCLUSIVE);

	/* Create and zero the first page of the subtrans log */
	slotno = ZeroSUBTRANSPage(0);

	/* Make sure it's written out */
	SimpleLruWritePage(SubTransCtl, slotno);
	Assert(!SubTransCtl->shared->page_dirty[slotno]);

	LWLockRelease(lock);
}

/*
 * Initialize (or reinitialize) a page of SUBTRANS to zeroes.
 *
 * The page is not actually written, just set up in shared memory.
 * The slot number of the new page is returned.
 *
 * Control lock must be held at entry, and will be held at exit.
 */
static int
ZeroSUBTRANSPage(int64 pageno)
{
	return SimpleLruZeroPage(SubTransCtl, pageno);
}

/*
 * This must be called ONCE during postmaster or standalone-backend startup,
 * after StartupXLOG has initialized TransamVariables->nextXid.
 *
 * oldestActiveXID is the oldest XID of any prepared transaction, or nextXid
 * if there are none.
 */
void
StartupSUBTRANS(TransactionId oldestActiveXID)
{
	FullTransactionId nextXid;
	int64		startPage;
	int64		endPage;
	LWLock	   *prevlock = NULL;
	LWLock	   *lock;

	/*
	 * Since we don't expect pg_subtrans to be valid across crashes, we
	 * initialize the currently-active page(s) to zeroes during startup.
	 * Whenever we advance into a new page, ExtendSUBTRANS will likewise zero
	 * the new page without regard to whatever was previously on disk.
	 */
	startPage = TransactionIdToPage(oldestActiveXID);
	nextXid = TransamVariables->nextXid;
	endPage = TransactionIdToPage(XidFromFullTransactionId(nextXid));

	for (;;)
	{
		lock = SimpleLruGetBankLock(SubTransCtl, startPage);
		if (prevlock != lock)
		{
			if (prevlock)
				LWLockRelease(prevlock);
			LWLockAcquire(lock, LW_EXCLUSIVE);
			prevlock = lock;
		}

		(void) ZeroSUBTRANSPage(startPage);
		if (startPage == endPage)
			break;

		startPage++;
		/* must account for wraparound */
		if (startPage > TransactionIdToPage(MaxTransactionId))
			startPage = 0;
	}

	LWLockRelease(lock);
}

/*
 * Perform a checkpoint --- either during shutdown, or on-the-fly
 */
void
CheckPointSUBTRANS(void)
{
	/*
	 * Write dirty SUBTRANS pages to disk
	 *
	 * This is not actually necessary from a correctness point of view. We do
	 * it merely to improve the odds that writing of dirty pages is done by
	 * the checkpoint process and not by backends.
	 */
	TRACE_POSTGRESQL_SUBTRANS_CHECKPOINT_START(true);
	SimpleLruWriteAll(SubTransCtl, true);
	TRACE_POSTGRESQL_SUBTRANS_CHECKPOINT_DONE(true);
}


/*
 * Make sure that SUBTRANS has room for a newly-allocated XID.
 *
 * NB: this is called while holding XidGenLock.  We want it to be very fast
 * most of the time; even when it's not so fast, no actual I/O need happen
 * unless we're forced to write out a dirty subtrans page to make room
 * in shared memory.
 */
void
ExtendSUBTRANS(TransactionId newestXact)
{
	int64		pageno;
	LWLock	   *lock;

	/*
	 * No work except at first XID of a page.  But beware: just after
	 * wraparound, the first XID of page zero is FirstNormalTransactionId.
	 */
	if (TransactionIdToEntry(newestXact) != 0 &&
		!TransactionIdEquals(newestXact, FirstNormalTransactionId))
		return;

	pageno = TransactionIdToPage(newestXact);

	lock = SimpleLruGetBankLock(SubTransCtl, pageno);
	LWLockAcquire(lock, LW_EXCLUSIVE);

	/* Zero the page */
	ZeroSUBTRANSPage(pageno);

	LWLockRelease(lock);
}


/*
 * Remove all SUBTRANS segments before the one holding the passed transaction ID
 *
 * oldestXact is the oldest TransactionXmin of any running transaction.  This
 * is called only during checkpoint.
 */
void
TruncateSUBTRANS(TransactionId oldestXact)
{
	int64		cutoffPage;

	/*
	 * The cutoff point is the start of the segment containing oldestXact. We
	 * pass the *page* containing oldestXact to SimpleLruTruncate.  We step
	 * back one transaction to avoid passing a cutoff page that hasn't been
	 * created yet in the rare case that oldestXact would be the first item on
	 * a page and oldestXact == next XID.  In that case, if we didn't subtract
	 * one, we'd trigger SimpleLruTruncate's wraparound detection.
	 */
	TransactionIdRetreat(oldestXact);
	cutoffPage = TransactionIdToPage(oldestXact);

	SimpleLruTruncate(SubTransCtl, cutoffPage);
}


/*
 * Decide whether a SUBTRANS page number is "older" for truncation purposes.
 * Analogous to CLOGPagePrecedes().
 */
static bool
SubTransPagePrecedes(int64 page1, int64 page2)
{
	TransactionId xid1;
	TransactionId xid2;

	xid1 = ((TransactionId) page1) * SUBTRANS_XACTS_PER_PAGE;
	xid1 += FirstNormalTransactionId + 1;
	xid2 = ((TransactionId) page2) * SUBTRANS_XACTS_PER_PAGE;
	xid2 += FirstNormalTransactionId + 1;

	return (TransactionIdPrecedes(xid1, xid2) &&
			TransactionIdPrecedes(xid1, xid2 + SUBTRANS_XACTS_PER_PAGE - 1));
}
