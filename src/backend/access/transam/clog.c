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
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Header: /cvsroot/pgsql/src/backend/access/transam/clog.c,v 1.13 2003/05/02 21:52:42 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/clog.h"
#include "storage/lwlock.h"
#include "miscadmin.h"


/*
 * Defines for CLOG page and segment sizes.  A page is the same BLCKSZ
 * as is used everywhere else in Postgres.	The CLOG segment size can be
 * chosen somewhat arbitrarily; we make it 1 million transactions by default,
 * or 256Kb.
 *
 * Note: because TransactionIds are 32 bits and wrap around at 0xFFFFFFFF,
 * CLOG page numbering also wraps around at 0xFFFFFFFF/CLOG_XACTS_PER_PAGE,
 * and CLOG segment numbering at 0xFFFFFFFF/CLOG_XACTS_PER_SEGMENT.  We need
 * take no explicit notice of that fact in this module, except when comparing
 * segment and page numbers in TruncateCLOG (see CLOGPagePrecedes).
 */

#define CLOG_BLCKSZ			BLCKSZ

/* We need two bits per xact, so four xacts fit in a byte */
#define CLOG_BITS_PER_XACT	2
#define CLOG_XACTS_PER_BYTE 4
#define CLOG_XACTS_PER_PAGE (CLOG_BLCKSZ * CLOG_XACTS_PER_BYTE)
#define CLOG_XACT_BITMASK	((1 << CLOG_BITS_PER_XACT) - 1)

#define CLOG_XACTS_PER_SEGMENT	0x100000
#define CLOG_PAGES_PER_SEGMENT	(CLOG_XACTS_PER_SEGMENT / CLOG_XACTS_PER_PAGE)

#define TransactionIdToPage(xid)	((xid) / (TransactionId) CLOG_XACTS_PER_PAGE)
#define TransactionIdToPgIndex(xid) ((xid) % (TransactionId) CLOG_XACTS_PER_PAGE)
#define TransactionIdToByte(xid)	(TransactionIdToPgIndex(xid) / CLOG_XACTS_PER_BYTE)
#define TransactionIdToBIndex(xid)	((xid) % (TransactionId) CLOG_XACTS_PER_BYTE)


/*----------
 * Shared-memory data structures for CLOG control
 *
 * We use a simple least-recently-used scheme to manage a pool of page
 * buffers for the CLOG.  Under ordinary circumstances we expect that write
 * traffic will occur mostly to the latest CLOG page (and to the just-prior
 * page, soon after a page transition).  Read traffic will probably touch
 * a larger span of pages, but in any case a fairly small number of page
 * buffers should be sufficient.  So, we just search the buffers using plain
 * linear search; there's no need for a hashtable or anything fancy.
 * The management algorithm is straight LRU except that we will never swap
 * out the latest page (since we know it's going to be hit again eventually).
 *
 * We use an overall LWLock to protect the shared data structures, plus
 * per-buffer LWLocks that synchronize I/O for each buffer.  A process
 * that is reading in or writing out a page buffer does not hold the control
 * lock, only the per-buffer lock for the buffer it is working on.
 *
 * To change the page number or state of a buffer, one must normally hold
 * the control lock.  (The sole exception to this rule is that a writer
 * process changes the state from DIRTY to WRITE_IN_PROGRESS while holding
 * only the per-buffer lock.)  If the buffer's state is neither EMPTY nor
 * CLEAN, then there may be processes doing (or waiting to do) I/O on the
 * buffer, so the page number may not be changed, and the only allowed state
 * transition is to change WRITE_IN_PROGRESS to DIRTY after dirtying the page.
 * To do any other state transition involving a buffer with potential I/O
 * processes, one must hold both the per-buffer lock and the control lock.
 * (Note the control lock must be acquired second; do not wait on a buffer
 * lock while holding the control lock.)  A process wishing to read a page
 * marks the buffer state as READ_IN_PROGRESS, then drops the control lock,
 * acquires the per-buffer lock, and rechecks the state before proceeding.
 * This recheck takes care of the possibility that someone else already did
 * the read, while the early marking prevents someone else from trying to
 * read the same page into a different buffer.
 *
 * Note we are assuming that read and write of the state value is atomic,
 * since I/O processes may examine and change the state while not holding
 * the control lock.
 *
 * As with the regular buffer manager, it is possible for another process
 * to re-dirty a page that is currently being written out.	This is handled
 * by setting the page's state from WRITE_IN_PROGRESS to DIRTY.  The writing
 * process must notice this and not mark the page CLEAN when it's done.
 *
 * XLOG interactions: this module generates an XLOG record whenever a new
 * CLOG page is initialized to zeroes.	Other writes of CLOG come from
 * recording of transaction commit or abort in xact.c, which generates its
 * own XLOG records for these events and will re-perform the status update
 * on redo; so we need make no additional XLOG entry here.	Also, the XLOG
 * is guaranteed flushed through the XLOG commit record before we are called
 * to log a commit, so the WAL rule "write xlog before data" is satisfied
 * automatically for commits, and we don't really care for aborts.  Therefore,
 * we don't need to mark XLOG pages with LSN information; we have enough
 * synchronization already.
 *----------
 */

typedef enum
{
	CLOG_PAGE_EMPTY,			/* CLOG buffer is not in use */
	CLOG_PAGE_READ_IN_PROGRESS, /* CLOG page is being read in */
	CLOG_PAGE_CLEAN,			/* CLOG page is valid and not dirty */
	CLOG_PAGE_DIRTY,			/* CLOG page is valid but needs write */
	CLOG_PAGE_WRITE_IN_PROGRESS /* CLOG page is being written out */
} ClogPageStatus;

/*
 * Shared-memory state for CLOG.
 */
typedef struct ClogCtlData
{
	/*
	 * Info for each buffer slot.  Page number is undefined when status is
	 * EMPTY.  lru_count is essentially the number of operations since
	 * last use of this page; the page with highest lru_count is the best
	 * candidate to replace.
	 */
	char	   *page_buffer[NUM_CLOG_BUFFERS];
	ClogPageStatus page_status[NUM_CLOG_BUFFERS];
	int			page_number[NUM_CLOG_BUFFERS];
	unsigned int page_lru_count[NUM_CLOG_BUFFERS];

	/*
	 * latest_page_number is the page number of the current end of the
	 * CLOG; this is not critical data, since we use it only to avoid
	 * swapping out the latest page.
	 */
	int			latest_page_number;
} ClogCtlData;

static ClogCtlData *ClogCtl = NULL;

/*
 * ClogBufferLocks is set during CLOGShmemInit and does not change thereafter.
 * The value is automatically inherited by backends via fork, and
 * doesn't need to be in shared memory.
 */
static LWLockId *ClogBufferLocks;		/* Per-buffer I/O locks */

/*
 * ClogDir is set during CLOGShmemInit and does not change thereafter.
 * The value is automatically inherited by backends via fork, and
 * doesn't need to be in shared memory.
 */
static char ClogDir[MAXPGPATH];

#define ClogFileName(path, seg) \
	snprintf(path, MAXPGPATH, "%s/%04X", ClogDir, seg)

/*
 * Macro to mark a buffer slot "most recently used".
 */
#define ClogRecentlyUsed(slotno)	\
	do { \
		int		iilru; \
		for (iilru = 0; iilru < NUM_CLOG_BUFFERS; iilru++) \
			ClogCtl->page_lru_count[iilru]++; \
		ClogCtl->page_lru_count[slotno] = 0; \
	} while (0)

/* Saved info for CLOGReportIOError */
typedef enum
{
	CLOG_OPEN_FAILED,
	CLOG_CREATE_FAILED,
	CLOG_SEEK_FAILED,
	CLOG_READ_FAILED,
	CLOG_WRITE_FAILED
} ClogErrorCause;
static ClogErrorCause clog_errcause;
static int	clog_errno;


static int	ZeroCLOGPage(int pageno, bool writeXlog);
static int	ReadCLOGPage(int pageno, TransactionId xid);
static void WriteCLOGPage(int slotno);
static bool CLOGPhysicalReadPage(int pageno, int slotno);
static bool CLOGPhysicalWritePage(int pageno, int slotno);
static void CLOGReportIOError(int pageno, TransactionId xid);
static int	SelectLRUCLOGPage(int pageno);
static bool ScanCLOGDirectory(int cutoffPage, bool doDeletions);
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
	int			slotno;
	char	   *byteptr;

	Assert(status == TRANSACTION_STATUS_COMMITTED ||
		   status == TRANSACTION_STATUS_ABORTED);

	LWLockAcquire(CLogControlLock, LW_EXCLUSIVE);

	slotno = ReadCLOGPage(pageno, xid);
	byteptr = ClogCtl->page_buffer[slotno] + byteno;

	/* Current state should be 0 or target state */
	Assert(((*byteptr >> bshift) & CLOG_XACT_BITMASK) == 0 ||
		   ((*byteptr >> bshift) & CLOG_XACT_BITMASK) == status);

	*byteptr |= (status << bshift);

	ClogCtl->page_status[slotno] = CLOG_PAGE_DIRTY;

	LWLockRelease(CLogControlLock);
}

/*
 * Interrogate the state of a transaction in the commit log.
 *
 * NB: this is a low-level routine and is NOT the preferred entry point
 * for most uses; TransactionLogTest() in transam.c is the intended caller.
 */
XidStatus
TransactionIdGetStatus(TransactionId xid)
{
	int			pageno = TransactionIdToPage(xid);
	int			byteno = TransactionIdToByte(xid);
	int			bshift = TransactionIdToBIndex(xid) * CLOG_BITS_PER_XACT;
	int			slotno;
	char	   *byteptr;
	XidStatus	status;

	LWLockAcquire(CLogControlLock, LW_EXCLUSIVE);

	slotno = ReadCLOGPage(pageno, xid);
	byteptr = ClogCtl->page_buffer[slotno] + byteno;

	status = (*byteptr >> bshift) & CLOG_XACT_BITMASK;

	LWLockRelease(CLogControlLock);

	return status;
}


/*
 * Initialization of shared memory for CLOG
 */
int
CLOGShmemSize(void)
{
	return MAXALIGN(sizeof(ClogCtlData) + CLOG_BLCKSZ * NUM_CLOG_BUFFERS)
#ifdef EXEC_BACKEND
			+ MAXALIGN(NUM_CLOG_BUFFERS * sizeof(LWLockId))
#endif
	;
}


void
CLOGShmemInit(void)
{
	bool		found;
	int			slotno;

	/* Handle ClogCtl */
	
	/* this must agree with space requested by CLOGShmemSize() */
	ClogCtl = (ClogCtlData *) ShmemInitStruct("CLOG Ctl",
				MAXALIGN(sizeof(ClogCtlData) +
				CLOG_BLCKSZ * NUM_CLOG_BUFFERS), &found);

	if (!IsUnderPostmaster)
	/* Initialize ClogCtl shared memory area */
	{
		char	   *bufptr;

		Assert(!found);

		memset(ClogCtl, 0, sizeof(ClogCtlData));

		bufptr = (char *)ClogCtl + sizeof(ClogCtlData);
	
		for (slotno = 0; slotno < NUM_CLOG_BUFFERS; slotno++)
		{
			ClogCtl->page_buffer[slotno] = bufptr;
			ClogCtl->page_status[slotno] = CLOG_PAGE_EMPTY;
			bufptr += CLOG_BLCKSZ;
		}

		/* ClogCtl->latest_page_number will be set later */
	}
	else
		Assert(found);

	/* Handle ClogBufferLocks */
	
#ifdef EXEC_BACKEND
	ClogBufferLocks = (LWLockId *) ShmemInitStruct("CLOG Buffer Locks",
						NUM_CLOG_BUFFERS * sizeof(LWLockId), &found);
	Assert((!found && !IsUnderPostmaster) || (found && IsUnderPostmaster));
#else
	ClogBufferLocks = malloc(NUM_CLOG_BUFFERS * sizeof(LWLockId));
	Assert(ClogBufferLocks);
#endif

	if (!IsUnderPostmaster)
		for (slotno = 0; slotno < NUM_CLOG_BUFFERS; slotno++)
			ClogBufferLocks[slotno] = LWLockAssign();

	/* Init CLOG directory path */
	snprintf(ClogDir, MAXPGPATH, "%s/pg_clog", DataDir);
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

	LWLockAcquire(CLogControlLock, LW_EXCLUSIVE);

	/* Create and zero the first page of the commit log */
	slotno = ZeroCLOGPage(0, false);

	/* Make sure it's written out */
	WriteCLOGPage(slotno);
	Assert(ClogCtl->page_status[slotno] == CLOG_PAGE_CLEAN);

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

	/* Find a suitable buffer slot for the page */
	slotno = SelectLRUCLOGPage(pageno);
	Assert(ClogCtl->page_status[slotno] == CLOG_PAGE_EMPTY ||
		   ClogCtl->page_status[slotno] == CLOG_PAGE_CLEAN ||
		   ClogCtl->page_number[slotno] == pageno);

	/* Mark the slot as containing this page */
	ClogCtl->page_number[slotno] = pageno;
	ClogCtl->page_status[slotno] = CLOG_PAGE_DIRTY;
	ClogRecentlyUsed(slotno);

	/* Set the buffer to zeroes */
	MemSet(ClogCtl->page_buffer[slotno], 0, CLOG_BLCKSZ);

	/* Assume this page is now the latest active page */
	ClogCtl->latest_page_number = pageno;

	if (writeXlog)
		WriteZeroPageXlogRec(pageno);

	return slotno;
}

/*
 * Find a CLOG page in a shared buffer, reading it in if necessary.
 * The page number must correspond to an already-initialized page.
 *
 * The passed-in xid is used only for error reporting, and may be
 * InvalidTransactionId if no specific xid is associated with the action.
 *
 * Return value is the shared-buffer slot number now holding the page.
 * The buffer's LRU access info is updated.
 *
 * Control lock must be held at entry, and will be held at exit.
 */
static int
ReadCLOGPage(int pageno, TransactionId xid)
{
	/* Outer loop handles restart if we lose the buffer to someone else */
	for (;;)
	{
		int			slotno;
		bool		ok;

		/* See if page already is in memory; if not, pick victim slot */
		slotno = SelectLRUCLOGPage(pageno);

		/* Did we find the page in memory? */
		if (ClogCtl->page_number[slotno] == pageno &&
			ClogCtl->page_status[slotno] != CLOG_PAGE_EMPTY)
		{
			/* If page is still being read in, we cannot use it yet */
			if (ClogCtl->page_status[slotno] != CLOG_PAGE_READ_IN_PROGRESS)
			{
				/* otherwise, it's ready to use */
				ClogRecentlyUsed(slotno);
				return slotno;
			}
		}
		else
		{
			/* We found no match; assert we selected a freeable slot */
			Assert(ClogCtl->page_status[slotno] == CLOG_PAGE_EMPTY ||
				   ClogCtl->page_status[slotno] == CLOG_PAGE_CLEAN);
		}

		/* Mark the slot read-busy (no-op if it already was) */
		ClogCtl->page_number[slotno] = pageno;
		ClogCtl->page_status[slotno] = CLOG_PAGE_READ_IN_PROGRESS;

		/*
		 * Temporarily mark page as recently-used to discourage
		 * SelectLRUCLOGPage from selecting it again for someone else.
		 */
		ClogCtl->page_lru_count[slotno] = 0;

		/* Release shared lock, grab per-buffer lock instead */
		LWLockRelease(CLogControlLock);
		LWLockAcquire(ClogBufferLocks[slotno], LW_EXCLUSIVE);

		/*
		 * Check to see if someone else already did the read, or took the
		 * buffer away from us.  If so, restart from the top.
		 */
		if (ClogCtl->page_number[slotno] != pageno ||
			ClogCtl->page_status[slotno] != CLOG_PAGE_READ_IN_PROGRESS)
		{
			LWLockRelease(ClogBufferLocks[slotno]);
			LWLockAcquire(CLogControlLock, LW_EXCLUSIVE);
			continue;
		}

		/* Okay, do the read */
		ok = CLOGPhysicalReadPage(pageno, slotno);

		/* Re-acquire shared control lock and update page state */
		LWLockAcquire(CLogControlLock, LW_EXCLUSIVE);

		Assert(ClogCtl->page_number[slotno] == pageno &&
			 ClogCtl->page_status[slotno] == CLOG_PAGE_READ_IN_PROGRESS);

		ClogCtl->page_status[slotno] = ok ? CLOG_PAGE_CLEAN : CLOG_PAGE_EMPTY;

		LWLockRelease(ClogBufferLocks[slotno]);

		/* Now it's okay to elog if we failed */
		if (!ok)
			CLOGReportIOError(pageno, xid);

		ClogRecentlyUsed(slotno);
		return slotno;
	}
}

/*
 * Write a CLOG page from a shared buffer, if necessary.
 * Does nothing if the specified slot is not dirty.
 *
 * NOTE: only one write attempt is made here.  Hence, it is possible that
 * the page is still dirty at exit (if someone else re-dirtied it during
 * the write).	However, we *do* attempt a fresh write even if the page
 * is already being written; this is for checkpoints.
 *
 * Control lock must be held at entry, and will be held at exit.
 */
static void
WriteCLOGPage(int slotno)
{
	int			pageno;
	bool		ok;

	/* Do nothing if page does not need writing */
	if (ClogCtl->page_status[slotno] != CLOG_PAGE_DIRTY &&
		ClogCtl->page_status[slotno] != CLOG_PAGE_WRITE_IN_PROGRESS)
		return;

	pageno = ClogCtl->page_number[slotno];

	/* Release shared lock, grab per-buffer lock instead */
	LWLockRelease(CLogControlLock);
	LWLockAcquire(ClogBufferLocks[slotno], LW_EXCLUSIVE);

	/*
	 * Check to see if someone else already did the write, or took the
	 * buffer away from us.  If so, do nothing.  NOTE: we really should
	 * never see WRITE_IN_PROGRESS here, since that state should only
	 * occur while the writer is holding the buffer lock.  But accept it
	 * so that we have a recovery path if a writer aborts.
	 */
	if (ClogCtl->page_number[slotno] != pageno ||
		(ClogCtl->page_status[slotno] != CLOG_PAGE_DIRTY &&
		 ClogCtl->page_status[slotno] != CLOG_PAGE_WRITE_IN_PROGRESS))
	{
		LWLockRelease(ClogBufferLocks[slotno]);
		LWLockAcquire(CLogControlLock, LW_EXCLUSIVE);
		return;
	}

	/*
	 * Mark the slot write-busy.  After this point, a transaction status
	 * update on this page will mark it dirty again.  NB: we are assuming
	 * that read/write of the page status field is atomic, since we change
	 * the state while not holding control lock.  However, we cannot set
	 * this state any sooner, or we'd possibly fool a previous writer into
	 * thinking he's successfully dumped the page when he hasn't.
	 * (Scenario: other writer starts, page is redirtied, we come along
	 * and set WRITE_IN_PROGRESS again, other writer completes and sets
	 * CLEAN because redirty info has been lost, then we think it's clean
	 * too.)
	 */
	ClogCtl->page_status[slotno] = CLOG_PAGE_WRITE_IN_PROGRESS;

	/* Okay, do the write */
	ok = CLOGPhysicalWritePage(pageno, slotno);

	/* Re-acquire shared control lock and update page state */
	LWLockAcquire(CLogControlLock, LW_EXCLUSIVE);

	Assert(ClogCtl->page_number[slotno] == pageno &&
		   (ClogCtl->page_status[slotno] == CLOG_PAGE_WRITE_IN_PROGRESS ||
			ClogCtl->page_status[slotno] == CLOG_PAGE_DIRTY));

	/* Cannot set CLEAN if someone re-dirtied page since write started */
	if (ClogCtl->page_status[slotno] == CLOG_PAGE_WRITE_IN_PROGRESS)
		ClogCtl->page_status[slotno] = ok ? CLOG_PAGE_CLEAN : CLOG_PAGE_DIRTY;

	LWLockRelease(ClogBufferLocks[slotno]);

	/* Now it's okay to elog if we failed */
	if (!ok)
		CLOGReportIOError(pageno, InvalidTransactionId);
}

/*
 * Physical read of a (previously existing) page into a buffer slot
 *
 * On failure, we cannot just elog(ERROR) since caller has put state in
 * shared memory that must be undone.  So, we return FALSE and save enough
 * info in static variables to let CLOGReportIOError make the report.
 *
 * For now, assume it's not worth keeping a file pointer open across
 * read/write operations.  We could cache one virtual file pointer ...
 */
static bool
CLOGPhysicalReadPage(int pageno, int slotno)
{
	int			segno = pageno / CLOG_PAGES_PER_SEGMENT;
	int			rpageno = pageno % CLOG_PAGES_PER_SEGMENT;
	int			offset = rpageno * CLOG_BLCKSZ;
	char		path[MAXPGPATH];
	int			fd;

	ClogFileName(path, segno);

	/*
	 * In a crash-and-restart situation, it's possible for us to receive
	 * commands to set the commit status of transactions whose bits are in
	 * already-truncated segments of the commit log (see notes in
	 * CLOGPhysicalWritePage).	Hence, if we are InRecovery, allow the
	 * case where the file doesn't exist, and return zeroes instead.
	 */
	fd = BasicOpenFile(path, O_RDWR | PG_BINARY, S_IRUSR | S_IWUSR);
	if (fd < 0)
	{
		if (errno != ENOENT || !InRecovery)
		{
			clog_errcause = CLOG_OPEN_FAILED;
			clog_errno = errno;
			return false;
		}

		elog(LOG, "clog file %s doesn't exist, reading as zeroes", path);
		MemSet(ClogCtl->page_buffer[slotno], 0, CLOG_BLCKSZ);
		return true;
	}

	if (lseek(fd, (off_t) offset, SEEK_SET) < 0)
	{
		clog_errcause = CLOG_SEEK_FAILED;
		clog_errno = errno;
		return false;
	}

	errno = 0;
	if (read(fd, ClogCtl->page_buffer[slotno], CLOG_BLCKSZ) != CLOG_BLCKSZ)
	{
		clog_errcause = CLOG_READ_FAILED;
		clog_errno = errno;
		return false;
	}

	close(fd);
	return true;
}

/*
 * Physical write of a page from a buffer slot
 *
 * On failure, we cannot just elog(ERROR) since caller has put state in
 * shared memory that must be undone.  So, we return FALSE and save enough
 * info in static variables to let CLOGReportIOError make the report.
 *
 * For now, assume it's not worth keeping a file pointer open across
 * read/write operations.  We could cache one virtual file pointer ...
 */
static bool
CLOGPhysicalWritePage(int pageno, int slotno)
{
	int			segno = pageno / CLOG_PAGES_PER_SEGMENT;
	int			rpageno = pageno % CLOG_PAGES_PER_SEGMENT;
	int			offset = rpageno * CLOG_BLCKSZ;
	char		path[MAXPGPATH];
	int			fd;

	ClogFileName(path, segno);

	/*
	 * If the file doesn't already exist, we should create it.  It is
	 * possible for this to need to happen when writing a page that's not
	 * first in its segment; we assume the OS can cope with that.  (Note:
	 * it might seem that it'd be okay to create files only when
	 * ZeroCLOGPage is called for the first page of a segment.	However,
	 * if after a crash and restart the REDO logic elects to replay the
	 * log from a checkpoint before the latest one, then it's possible
	 * that we will get commands to set transaction status of transactions
	 * that have already been truncated from the commit log.  Easiest way
	 * to deal with that is to accept references to nonexistent files here
	 * and in CLOGPhysicalReadPage.)
	 */
	fd = BasicOpenFile(path, O_RDWR | PG_BINARY, S_IRUSR | S_IWUSR);
	if (fd < 0)
	{
		if (errno != ENOENT)
		{
			clog_errcause = CLOG_OPEN_FAILED;
			clog_errno = errno;
			return false;
		}

		fd = BasicOpenFile(path, O_RDWR | O_CREAT | O_EXCL | PG_BINARY,
						   S_IRUSR | S_IWUSR);
		if (fd < 0)
		{
			clog_errcause = CLOG_CREATE_FAILED;
			clog_errno = errno;
			return false;
		}
	}

	if (lseek(fd, (off_t) offset, SEEK_SET) < 0)
	{
		clog_errcause = CLOG_SEEK_FAILED;
		clog_errno = errno;
		return false;
	}

	errno = 0;
	if (write(fd, ClogCtl->page_buffer[slotno], CLOG_BLCKSZ) != CLOG_BLCKSZ)
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		clog_errcause = CLOG_WRITE_FAILED;
		clog_errno = errno;
		return false;
	}

	close(fd);
	return true;
}

/*
 * Issue the error message after failure of CLOGPhysicalReadPage or
 * CLOGPhysicalWritePage.  Call this after cleaning up shared-memory state.
 */
static void
CLOGReportIOError(int pageno, TransactionId xid)
{
	int			segno = pageno / CLOG_PAGES_PER_SEGMENT;
	int			rpageno = pageno % CLOG_PAGES_PER_SEGMENT;
	int			offset = rpageno * CLOG_BLCKSZ;
	char		path[MAXPGPATH];

	/* XXX TODO: provide xid as context in error messages */

	ClogFileName(path, segno);
	errno = clog_errno;
	switch (clog_errcause)
	{
		case CLOG_OPEN_FAILED:
			elog(ERROR, "open of %s failed: %m", path);
			break;
		case CLOG_CREATE_FAILED:
			elog(ERROR, "creation of file %s failed: %m", path);
			break;
		case CLOG_SEEK_FAILED:
			elog(ERROR, "lseek of clog file %u, offset %u failed: %m",
				 segno, offset);
			break;
		case CLOG_READ_FAILED:
			elog(ERROR, "read of clog file %u, offset %u failed: %m",
				 segno, offset);
			break;
		case CLOG_WRITE_FAILED:
			elog(ERROR, "write of clog file %u, offset %u failed: %m",
				 segno, offset);
			break;
		default:
			/* can't get here, we trust */
			elog(ERROR, "unknown CLOG I/O error");
			break;
	}
}

/*
 * Select the slot to re-use when we need a free slot.
 *
 * The target page number is passed because we need to consider the
 * possibility that some other process reads in the target page while
 * we are doing I/O to free a slot.  Hence, check or recheck to see if
 * any slot already holds the target page, and return that slot if so.
 * Thus, the returned slot is *either* a slot already holding the pageno
 * (could be any state except EMPTY), *or* a freeable slot (state EMPTY
 * or CLEAN).
 *
 * Control lock must be held at entry, and will be held at exit.
 */
static int
SelectLRUCLOGPage(int pageno)
{
	/* Outer loop handles restart after I/O */
	for (;;)
	{
		int			slotno;
		int			bestslot = 0;
		unsigned int bestcount = 0;

		/* See if page already has a buffer assigned */
		for (slotno = 0; slotno < NUM_CLOG_BUFFERS; slotno++)
		{
			if (ClogCtl->page_number[slotno] == pageno &&
				ClogCtl->page_status[slotno] != CLOG_PAGE_EMPTY)
				return slotno;
		}

		/*
		 * If we find any EMPTY slot, just select that one. Else locate
		 * the least-recently-used slot that isn't the latest CLOG page.
		 */
		for (slotno = 0; slotno < NUM_CLOG_BUFFERS; slotno++)
		{
			if (ClogCtl->page_status[slotno] == CLOG_PAGE_EMPTY)
				return slotno;
			if (ClogCtl->page_lru_count[slotno] > bestcount &&
			 ClogCtl->page_number[slotno] != ClogCtl->latest_page_number)
			{
				bestslot = slotno;
				bestcount = ClogCtl->page_lru_count[slotno];
			}
		}

		/*
		 * If the selected page is clean, we're set.
		 */
		if (ClogCtl->page_status[bestslot] == CLOG_PAGE_CLEAN)
			return bestslot;

		/*
		 * We need to do I/O.  Normal case is that we have to write it
		 * out, but it's possible in the worst case to have selected a
		 * read-busy page.	In that case we use ReadCLOGPage to wait for
		 * the read to complete.
		 */
		if (ClogCtl->page_status[bestslot] == CLOG_PAGE_READ_IN_PROGRESS)
			(void) ReadCLOGPage(ClogCtl->page_number[bestslot],
								InvalidTransactionId);
		else
			WriteCLOGPage(bestslot);

		/*
		 * Now loop back and try again.  This is the easiest way of
		 * dealing with corner cases such as the victim page being
		 * re-dirtied while we wrote it.
		 */
	}
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
	ClogCtl->latest_page_number = TransactionIdToPage(ShmemVariableCache->nextXid);
}

/*
 * This must be called ONCE during postmaster or standalone-backend shutdown
 */
void
ShutdownCLOG(void)
{
	int			slotno;

	LWLockAcquire(CLogControlLock, LW_EXCLUSIVE);

	for (slotno = 0; slotno < NUM_CLOG_BUFFERS; slotno++)
	{
		WriteCLOGPage(slotno);
		Assert(ClogCtl->page_status[slotno] == CLOG_PAGE_EMPTY ||
			   ClogCtl->page_status[slotno] == CLOG_PAGE_CLEAN);
	}

	LWLockRelease(CLogControlLock);
}

/*
 * Perform a checkpoint --- either during shutdown, or on-the-fly
 */
void
CheckPointCLOG(void)
{
	int			slotno;

	LWLockAcquire(CLogControlLock, LW_EXCLUSIVE);

	for (slotno = 0; slotno < NUM_CLOG_BUFFERS; slotno++)
	{
		WriteCLOGPage(slotno);

		/*
		 * We cannot assert that the slot is clean now, since another
		 * process might have re-dirtied it already.  That's okay.
		 */
	}

	LWLockRelease(CLogControlLock);
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
	int			slotno;

	/*
	 * The cutoff point is the start of the segment containing oldestXact.
	 */
	oldestXact -= oldestXact % CLOG_XACTS_PER_SEGMENT;
	cutoffPage = TransactionIdToPage(oldestXact);

	if (!ScanCLOGDirectory(cutoffPage, false))
		return;					/* nothing to remove */

	/* Perform a forced CHECKPOINT */
	CreateCheckPoint(false, true);

	/*
	 * Scan CLOG shared memory and remove any pages preceding the cutoff
	 * page, to ensure we won't rewrite them later.  (Any dirty pages
	 * should have been flushed already during the checkpoint, we're just
	 * being extra careful here.)
	 */
	LWLockAcquire(CLogControlLock, LW_EXCLUSIVE);

restart:;

	/*
	 * While we are holding the lock, make an important safety check: the
	 * planned cutoff point must be <= the current CLOG endpoint page.
	 * Otherwise we have already wrapped around, and proceeding with the
	 * truncation would risk removing the current CLOG segment.
	 */
	if (CLOGPagePrecedes(ClogCtl->latest_page_number, cutoffPage))
	{
		LWLockRelease(CLogControlLock);
		elog(LOG, "unable to truncate commit log: apparent wraparound");
		return;
	}

	for (slotno = 0; slotno < NUM_CLOG_BUFFERS; slotno++)
	{
		if (ClogCtl->page_status[slotno] == CLOG_PAGE_EMPTY)
			continue;
		if (!CLOGPagePrecedes(ClogCtl->page_number[slotno], cutoffPage))
			continue;

		/*
		 * If page is CLEAN, just change state to EMPTY (expected case).
		 */
		if (ClogCtl->page_status[slotno] == CLOG_PAGE_CLEAN)
		{
			ClogCtl->page_status[slotno] = CLOG_PAGE_EMPTY;
			continue;
		}

		/*
		 * Hmm, we have (or may have) I/O operations acting on the page,
		 * so we've got to wait for them to finish and then start again.
		 * This is the same logic as in SelectLRUCLOGPage.
		 */
		if (ClogCtl->page_status[slotno] == CLOG_PAGE_READ_IN_PROGRESS)
			(void) ReadCLOGPage(ClogCtl->page_number[slotno],
								InvalidTransactionId);
		else
			WriteCLOGPage(slotno);
		goto restart;
	}

	LWLockRelease(CLogControlLock);

	/* Now we can remove the old CLOG segment(s) */
	(void) ScanCLOGDirectory(cutoffPage, true);
}

/*
 * TruncateCLOG subroutine: scan CLOG directory for removable segments.
 * Actually remove them iff doDeletions is true.  Return TRUE iff any
 * removable segments were found.  Note: no locking is needed.
 */
static bool
ScanCLOGDirectory(int cutoffPage, bool doDeletions)
{
	bool		found = false;
	DIR		   *cldir;
	struct dirent *clde;
	int			segno;
	int			segpage;
	char		path[MAXPGPATH];

	cldir = opendir(ClogDir);
	if (cldir == NULL)
		elog(ERROR, "could not open transaction-commit log directory (%s): %m",
			 ClogDir);

	errno = 0;
	while ((clde = readdir(cldir)) != NULL)
	{
		if (strlen(clde->d_name) == 4 &&
			strspn(clde->d_name, "0123456789ABCDEF") == 4)
		{
			segno = (int) strtol(clde->d_name, NULL, 16);
			segpage = segno * CLOG_PAGES_PER_SEGMENT;
			if (CLOGPagePrecedes(segpage, cutoffPage))
			{
				found = true;
				if (doDeletions)
				{
					elog(LOG, "removing commit log file %s", clde->d_name);
					snprintf(path, MAXPGPATH, "%s/%s", ClogDir, clde->d_name);
					unlink(path);
				}
			}
		}
		errno = 0;
	}
	if (errno)
		elog(ERROR, "could not read transaction-commit log directory (%s): %m",
			 ClogDir);
	closedir(cldir);

	return found;
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
	(void) XLogInsert(RM_CLOG_ID, CLOG_ZEROPAGE | XLOG_NO_TRAN, &rdata);
}

/*
 * CLOG resource manager's routines
 */
void
clog_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	if (info == CLOG_ZEROPAGE)
	{
		int			pageno;
		int			slotno;

		memcpy(&pageno, XLogRecGetData(record), sizeof(int));

		LWLockAcquire(CLogControlLock, LW_EXCLUSIVE);

		slotno = ZeroCLOGPage(pageno, false);
		WriteCLOGPage(slotno);
		Assert(ClogCtl->page_status[slotno] == CLOG_PAGE_CLEAN);

		LWLockRelease(CLogControlLock);
	}
}

void
clog_undo(XLogRecPtr lsn, XLogRecord *record)
{
}

void
clog_desc(char *buf, uint8 xl_info, char *rec)
{
	uint8		info = xl_info & ~XLR_INFO_MASK;

	if (info == CLOG_ZEROPAGE)
	{
		int			pageno;

		memcpy(&pageno, rec, sizeof(int));
		sprintf(buf + strlen(buf), "zeropage: %d", pageno);
	}
	else
		strcat(buf, "UNKNOWN");
}
