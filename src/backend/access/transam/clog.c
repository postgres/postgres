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
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/backend/access/transam/clog.c,v 1.22 2004/07/03 02:55:56 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/clog.h"
#include "access/slru.h"
#include "miscadmin.h"
#include "storage/lwlock.h"


/*
 * Defines for CLOG page and segment sizes.  A page is the same BLCKSZ
 * as is used everywhere else in Postgres.
 *
 * Note: because TransactionIds are 32 bits and wrap around at 0xFFFFFFFF,
 * CLOG page numbering also wraps around at 0xFFFFFFFF/CLOG_XACTS_PER_PAGE,
 * and CLOG segment numbering at 0xFFFFFFFF/CLOG_XACTS_PER_SEGMENT.  We need
 * take no explicit notice of that fact in this module, except when comparing
 * segment and page numbers in TruncateCLOG (see CLOGPagePrecedes).
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


/*----------
 * Shared-memory data structures for CLOG control
 *
 * XLOG interactions: this module generates an XLOG record whenever a new
 * CLOG page is initialized to zeroes.	Other writes of CLOG come from
 * recording of transaction commit or abort in xact.c, which generates its
 * own XLOG records for these events and will re-perform the status update
 * on redo; so we need make no additional XLOG entry here.	Also, the XLOG
 * is guaranteed flushed through the XLOG commit record before we are called
 * to log a commit, so the WAL rule "write xlog before data" is satisfied
 * automatically for commits, and we don't really care for aborts.  Therefore,
 * we don't need to mark CLOG pages with LSN information; we have enough
 * synchronization already.
 *----------
 */


static SlruCtlData ClogCtlData;
static SlruCtl ClogCtl = &ClogCtlData;


static int	ZeroCLOGPage(int pageno, bool writeXlog);
static bool CLOGPagePrecedes(int page1, int page2);
static void WriteZeroPageXlogRec(int pageno);


/*
 * Record the final state of a transaction in the commit log.
 *
 * NB: this is a low-level routine and is NOT the preferred entry point
 * for most uses; TransactionLogUpdate() in transam.c is the intended caller.
 */
void
TransactionIdSetStatus(TransactionId xid, XidStatus status)
{
	int			pageno = TransactionIdToPage(xid);
	int			byteno = TransactionIdToByte(xid);
	int			bshift = TransactionIdToBIndex(xid) * CLOG_BITS_PER_XACT;
	char	   *byteptr;
	char		byteval;

	Assert(status == TRANSACTION_STATUS_COMMITTED ||
		   status == TRANSACTION_STATUS_ABORTED ||
		   status == TRANSACTION_STATUS_SUB_COMMITTED);

	LWLockAcquire(ClogCtl->ControlLock, LW_EXCLUSIVE);

	byteptr = SimpleLruReadPage(ClogCtl, pageno, xid, true);
	byteptr += byteno;

	/* Current state should be 0, subcommitted or target state */
	Assert(((*byteptr >> bshift) & CLOG_XACT_BITMASK) == 0 ||
		   ((*byteptr >> bshift) & CLOG_XACT_BITMASK) == TRANSACTION_STATUS_SUB_COMMITTED ||
		   ((*byteptr >> bshift) & CLOG_XACT_BITMASK) == status);

	/* note this assumes exclusive access to the clog page */
	byteval = *byteptr;
	byteval &= ~(((1 << CLOG_BITS_PER_XACT) - 1) << bshift);
	byteval |= (status << bshift);
	*byteptr = byteval;

	/* ...->page_status[slotno] = SLRU_PAGE_DIRTY; already done */

	LWLockRelease(ClogCtl->ControlLock);
}

/*
 * Interrogate the state of a transaction in the commit log.
 *
 * NB: this is a low-level routine and is NOT the preferred entry point
 * for most uses; TransactionLogFetch() in transam.c is the intended caller.
 */
XidStatus
TransactionIdGetStatus(TransactionId xid)
{
	int			pageno = TransactionIdToPage(xid);
	int			byteno = TransactionIdToByte(xid);
	int			bshift = TransactionIdToBIndex(xid) * CLOG_BITS_PER_XACT;
	char	   *byteptr;
	XidStatus	status;

	LWLockAcquire(ClogCtl->ControlLock, LW_EXCLUSIVE);

	byteptr = SimpleLruReadPage(ClogCtl, pageno, xid, false);
	byteptr += byteno;

	status = (*byteptr >> bshift) & CLOG_XACT_BITMASK;

	LWLockRelease(ClogCtl->ControlLock);

	return status;
}


/*
 * Initialization of shared memory for CLOG
 */

int
CLOGShmemSize(void)
{
	return SimpleLruShmemSize();
}

void
CLOGShmemInit(void)
{
	SimpleLruInit(ClogCtl, "CLOG Ctl", "pg_clog");
	ClogCtl->PagePrecedes = CLOGPagePrecedes;
}

/*
 * This func must be called ONCE on system install.  It creates
 * the initial CLOG segment.  (The CLOG directory is assumed to
 * have been created by the initdb shell script, and CLOGShmemInit
 * must have been called already.)
 */
void
BootStrapCLOG(void)
{
	int			slotno;

	LWLockAcquire(ClogCtl->ControlLock, LW_EXCLUSIVE);

	/* Create and zero the first page of the commit log */
	slotno = ZeroCLOGPage(0, false);

	/* Make sure it's written out */
	SimpleLruWritePage(ClogCtl, slotno, NULL);
	/* Assert(ClogCtl->page_status[slotno] == SLRU_PAGE_CLEAN); */

	LWLockRelease(ClogCtl->ControlLock);
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
	int			slotno = SimpleLruZeroPage(ClogCtl, pageno);

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
	/*
	 * Initialize our idea of the latest page number.
	 */
	SimpleLruSetLatestPage(ClogCtl,
						   TransactionIdToPage(ShmemVariableCache->nextXid));
}

/*
 * This must be called ONCE during postmaster or standalone-backend shutdown
 */
void
ShutdownCLOG(void)
{
	SimpleLruFlush(ClogCtl, false);
}

/*
 * Perform a checkpoint --- either during shutdown, or on-the-fly
 */
void
CheckPointCLOG(void)
{
	SimpleLruFlush(ClogCtl, true);
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

	LWLockAcquire(ClogCtl->ControlLock, LW_EXCLUSIVE);

	/* Zero the page and make an XLOG entry about it */
	ZeroCLOGPage(pageno, true);

	LWLockRelease(ClogCtl->ControlLock);
}


/*
 * Remove all CLOG segments before the one holding the passed transaction ID
 *
 * When this is called, we know that the database logically contains no
 * reference to transaction IDs older than oldestXact.	However, we must
 * not truncate the CLOG until we have performed a checkpoint, to ensure
 * that no such references remain on disk either; else a crash just after
 * the truncation might leave us with a problem.  Since CLOG segments hold
 * a large number of transactions, the opportunity to actually remove a
 * segment is fairly rare, and so it seems best not to do the checkpoint
 * unless we have confirmed that there is a removable segment.	Therefore
 * we issue the checkpoint command here, not in higher-level code as might
 * seem cleaner.
 */
void
TruncateCLOG(TransactionId oldestXact)
{
	int			cutoffPage;

	/*
	 * The cutoff point is the start of the segment containing oldestXact.
	 * We pass the *page* containing oldestXact to SimpleLruTruncate.
	 */
	cutoffPage = TransactionIdToPage(oldestXact);
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
	(void) XLogInsert(RM_SLRU_ID, CLOG_ZEROPAGE | XLOG_NO_TRAN, &rdata);
}

/* Redo a ZEROPAGE action during WAL replay */
void
clog_zeropage_redo(int pageno)
{
	int			slotno;

	LWLockAcquire(ClogCtl->ControlLock, LW_EXCLUSIVE);

	slotno = ZeroCLOGPage(pageno, false);
	SimpleLruWritePage(ClogCtl, slotno, NULL);
	/* Assert(ClogCtl->page_status[slotno] == SLRU_PAGE_CLEAN); */

	LWLockRelease(ClogCtl->ControlLock);
}
