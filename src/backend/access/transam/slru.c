/*-------------------------------------------------------------------------
 *
 * slru.c
 *		Simple LRU buffering for transaction status logfiles
 *
 * We use a simple least-recently-used scheme to manage a pool of page
 * buffers.  Under ordinary circumstances we expect that write
 * traffic will occur mostly to the latest page (and to the just-prior
 * page, soon after a page transition).  Read traffic will probably touch
 * a larger span of pages, but in any case a fairly small number of page
 * buffers should be sufficient.  So, we just search the buffers using plain
 * linear search; there's no need for a hashtable or anything fancy.
 * The management algorithm is straight LRU except that we will never swap
 * out the latest page (since we know it's going to be hit again eventually).
 *
 * We use a control LWLock to protect the shared data structures, plus
 * per-buffer LWLocks that synchronize I/O for each buffer.  The control lock
 * must be held to examine or modify any shared state.  A process that is
 * reading in or writing out a page buffer does not hold the control lock,
 * only the per-buffer lock for the buffer it is working on.
 *
 * When initiating I/O on a buffer, we acquire the per-buffer lock exclusively
 * before releasing the control lock.  The per-buffer lock is released after
 * completing the I/O, re-acquiring the control lock, and updating the shared
 * state.  (Deadlock is not possible here, because we never try to initiate
 * I/O when someone else is already doing I/O on the same buffer.)
 * To wait for I/O to complete, release the control lock, acquire the
 * per-buffer lock in shared mode, immediately release the per-buffer lock,
 * reacquire the control lock, and then recheck state (since arbitrary things
 * could have happened while we didn't have the lock).
 *
 * As with the regular buffer manager, it is possible for another process
 * to re-dirty a page that is currently being written out.	This is handled
 * by re-setting the page's page_dirty flag.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/backend/access/transam/slru.c,v 1.30 2005/11/05 21:19:47 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/slru.h"
#include "access/xlog.h"
#include "storage/fd.h"
#include "storage/shmem.h"
#include "miscadmin.h"


/*
 * Define segment size.  A page is the same BLCKSZ as is used everywhere
 * else in Postgres.  The segment size can be chosen somewhat arbitrarily;
 * we make it 32 pages by default, or 256Kb, i.e. 1M transactions for CLOG
 * or 64K transactions for SUBTRANS.
 *
 * Note: because TransactionIds are 32 bits and wrap around at 0xFFFFFFFF,
 * page numbering also wraps around at 0xFFFFFFFF/xxxx_XACTS_PER_PAGE (where
 * xxxx is CLOG or SUBTRANS, respectively), and segment numbering at
 * 0xFFFFFFFF/xxxx_XACTS_PER_PAGE/SLRU_PAGES_PER_SEGMENT.  We need
 * take no explicit notice of that fact in this module, except when comparing
 * segment and page numbers in SimpleLruTruncate (see PagePrecedes()).
 *
 * Note: this file currently assumes that segment file names will be four
 * hex digits.	This sets a lower bound on the segment size (64K transactions
 * for 32-bit TransactionIds).
 */
#define SLRU_PAGES_PER_SEGMENT	32

#define SlruFileName(ctl, path, seg) \
	snprintf(path, MAXPGPATH, "%s/%04X", (ctl)->Dir, seg)

/*
 * During SimpleLruFlush(), we will usually not need to write/fsync more
 * than one or two physical files, but we may need to write several pages
 * per file.  We can consolidate the I/O requests by leaving files open
 * until control returns to SimpleLruFlush().  This data structure remembers
 * which files are open.
 */
typedef struct SlruFlushData
{
	int			num_files;		/* # files actually open */
	int			fd[NUM_SLRU_BUFFERS];	/* their FD's */
	int			segno[NUM_SLRU_BUFFERS];		/* their log seg#s */
} SlruFlushData;

/*
 * Macro to mark a buffer slot "most recently used".
 */
#define SlruRecentlyUsed(shared, slotno)	\
	do { \
		if ((shared)->page_lru_count[slotno] != 0) { \
			int		iilru; \
			for (iilru = 0; iilru < NUM_SLRU_BUFFERS; iilru++) \
				(shared)->page_lru_count[iilru]++; \
			(shared)->page_lru_count[slotno] = 0; \
		} \
	} while (0)

/* Saved info for SlruReportIOError */
typedef enum
{
	SLRU_OPEN_FAILED,
	SLRU_CREATE_FAILED,
	SLRU_SEEK_FAILED,
	SLRU_READ_FAILED,
	SLRU_WRITE_FAILED,
	SLRU_FSYNC_FAILED,
	SLRU_CLOSE_FAILED
} SlruErrorCause;

static SlruErrorCause slru_errcause;
static int	slru_errno;


static bool SlruPhysicalReadPage(SlruCtl ctl, int pageno, int slotno);
static bool SlruPhysicalWritePage(SlruCtl ctl, int pageno, int slotno,
					  SlruFlush fdata);
static void SlruReportIOError(SlruCtl ctl, int pageno, TransactionId xid);
static int	SlruSelectLRUPage(SlruCtl ctl, int pageno);


/*
 * Initialization of shared memory
 */

Size
SimpleLruShmemSize(void)
{
	/* we assume NUM_SLRU_BUFFERS isn't so large as to risk overflow */
	return BUFFERALIGN(sizeof(SlruSharedData)) + BLCKSZ * NUM_SLRU_BUFFERS;
}

void
SimpleLruInit(SlruCtl ctl, const char *name,
			  LWLockId ctllock, const char *subdir)
{
	SlruShared	shared;
	bool		found;

	shared = (SlruShared) ShmemInitStruct(name, SimpleLruShmemSize(), &found);

	if (!IsUnderPostmaster)
	{
		/* Initialize locks and shared memory area */
		char	   *bufptr;
		int			slotno;

		Assert(!found);

		memset(shared, 0, sizeof(SlruSharedData));

		shared->ControlLock = ctllock;

		bufptr = (char *) shared + BUFFERALIGN(sizeof(SlruSharedData));

		for (slotno = 0; slotno < NUM_SLRU_BUFFERS; slotno++)
		{
			shared->page_buffer[slotno] = bufptr;
			shared->page_status[slotno] = SLRU_PAGE_EMPTY;
			shared->page_dirty[slotno] = false;
			shared->page_lru_count[slotno] = 1;
			shared->buffer_locks[slotno] = LWLockAssign();
			bufptr += BLCKSZ;
		}

		/* shared->latest_page_number will be set later */
	}
	else
		Assert(found);

	/*
	 * Initialize the unshared control struct, including directory path. We
	 * assume caller set PagePrecedes.
	 */
	ctl->shared = shared;
	ctl->do_fsync = true;		/* default behavior */
	StrNCpy(ctl->Dir, subdir, sizeof(ctl->Dir));
}

/*
 * Initialize (or reinitialize) a page to zeroes.
 *
 * The page is not actually written, just set up in shared memory.
 * The slot number of the new page is returned.
 *
 * Control lock must be held at entry, and will be held at exit.
 */
int
SimpleLruZeroPage(SlruCtl ctl, int pageno)
{
	SlruShared	shared = ctl->shared;
	int			slotno;

	/* Find a suitable buffer slot for the page */
	slotno = SlruSelectLRUPage(ctl, pageno);
	Assert(shared->page_status[slotno] == SLRU_PAGE_EMPTY ||
		   (shared->page_status[slotno] == SLRU_PAGE_VALID &&
			!shared->page_dirty[slotno]) ||
		   shared->page_number[slotno] == pageno);

	/* Mark the slot as containing this page */
	shared->page_number[slotno] = pageno;
	shared->page_status[slotno] = SLRU_PAGE_VALID;
	shared->page_dirty[slotno] = true;
	SlruRecentlyUsed(shared, slotno);

	/* Set the buffer to zeroes */
	MemSet(shared->page_buffer[slotno], 0, BLCKSZ);

	/* Assume this page is now the latest active page */
	shared->latest_page_number = pageno;

	return slotno;
}

/*
 * Wait for any active I/O on a page slot to finish.  (This does not
 * guarantee that new I/O hasn't been started before we return, though.)
 *
 * Control lock must be held at entry, and will be held at exit.
 */
static void
SimpleLruWaitIO(SlruCtl ctl, int slotno)
{
	SlruShared	shared = ctl->shared;

	/* See notes at top of file */
	LWLockRelease(shared->ControlLock);
	LWLockAcquire(shared->buffer_locks[slotno], LW_SHARED);
	LWLockRelease(shared->buffer_locks[slotno]);
	LWLockAcquire(shared->ControlLock, LW_EXCLUSIVE);
	/*
	 * If the slot is still in an io-in-progress state, then either someone
	 * already started a new I/O on the slot, or a previous I/O failed and
	 * neglected to reset the page state.  That shouldn't happen, really,
	 * but it seems worth a few extra cycles to check and recover from it.
	 * We can cheaply test for failure by seeing if the buffer lock is still
	 * held (we assume that transaction abort would release the lock).
	 */
	if (shared->page_status[slotno] == SLRU_PAGE_READ_IN_PROGRESS ||
		shared->page_status[slotno] == SLRU_PAGE_WRITE_IN_PROGRESS)
	{
		if (LWLockConditionalAcquire(shared->buffer_locks[slotno], LW_SHARED))
		{
			/* indeed, the I/O must have failed */
			if (shared->page_status[slotno] == SLRU_PAGE_READ_IN_PROGRESS)
				shared->page_status[slotno] = SLRU_PAGE_EMPTY;
			else				/* write_in_progress */
			{
				shared->page_status[slotno] = SLRU_PAGE_VALID;
				shared->page_dirty[slotno] = true;
			}
			LWLockRelease(shared->buffer_locks[slotno]);
		}
	}
}

/*
 * Find a page in a shared buffer, reading it in if necessary.
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
int
SimpleLruReadPage(SlruCtl ctl, int pageno, TransactionId xid)
{
	SlruShared	shared = ctl->shared;

	/* Outer loop handles restart if we must wait for someone else's I/O */
	for (;;)
	{
		int			slotno;
		bool		ok;

		/* See if page already is in memory; if not, pick victim slot */
		slotno = SlruSelectLRUPage(ctl, pageno);

		/* Did we find the page in memory? */
		if (shared->page_number[slotno] == pageno &&
			shared->page_status[slotno] != SLRU_PAGE_EMPTY)
		{
			/* If page is still being read in, we must wait for I/O */
			if (shared->page_status[slotno] == SLRU_PAGE_READ_IN_PROGRESS)
			{
				SimpleLruWaitIO(ctl, slotno);
				/* Now we must recheck state from the top */
				continue;
			}
			/* Otherwise, it's ready to use */
			SlruRecentlyUsed(shared, slotno);
			return slotno;
		}

		/* We found no match; assert we selected a freeable slot */
		Assert(shared->page_status[slotno] == SLRU_PAGE_EMPTY ||
			   (shared->page_status[slotno] == SLRU_PAGE_VALID &&
				!shared->page_dirty[slotno]));

		/* Mark the slot read-busy */
		shared->page_number[slotno] = pageno;
		shared->page_status[slotno] = SLRU_PAGE_READ_IN_PROGRESS;
		shared->page_dirty[slotno] = false;

		/* Acquire per-buffer lock (cannot deadlock, see notes at top) */
		LWLockAcquire(shared->buffer_locks[slotno], LW_EXCLUSIVE);

		/*
		 * Temporarily mark page as recently-used to discourage
		 * SlruSelectLRUPage from selecting it again for someone else.
		 */
		SlruRecentlyUsed(shared, slotno);

		/* Release control lock while doing I/O */
		LWLockRelease(shared->ControlLock);

		/* Do the read */
		ok = SlruPhysicalReadPage(ctl, pageno, slotno);

		/* Re-acquire control lock and update page state */
		LWLockAcquire(shared->ControlLock, LW_EXCLUSIVE);

		Assert(shared->page_number[slotno] == pageno &&
			   shared->page_status[slotno] == SLRU_PAGE_READ_IN_PROGRESS &&
			   !shared->page_dirty[slotno]);

		shared->page_status[slotno] = ok ? SLRU_PAGE_VALID : SLRU_PAGE_EMPTY;

		LWLockRelease(shared->buffer_locks[slotno]);

		/* Now it's okay to ereport if we failed */
		if (!ok)
			SlruReportIOError(ctl, pageno, xid);

		SlruRecentlyUsed(shared, slotno);
		return slotno;
	}
}

/*
 * Write a page from a shared buffer, if necessary.
 * Does nothing if the specified slot is not dirty.
 *
 * NOTE: only one write attempt is made here.  Hence, it is possible that
 * the page is still dirty at exit (if someone else re-dirtied it during
 * the write).	However, we *do* attempt a fresh write even if the page
 * is already being written; this is for checkpoints.
 *
 * Control lock must be held at entry, and will be held at exit.
 */
void
SimpleLruWritePage(SlruCtl ctl, int slotno, SlruFlush fdata)
{
	SlruShared	shared = ctl->shared;
	int			pageno = shared->page_number[slotno];
	bool		ok;

	/* If a write is in progress, wait for it to finish */
	while (shared->page_status[slotno] == SLRU_PAGE_WRITE_IN_PROGRESS &&
		   shared->page_number[slotno] == pageno)
	{
		SimpleLruWaitIO(ctl, slotno);
	}

	/*
	 * Do nothing if page is not dirty, or if buffer no longer contains
	 * the same page we were called for.
	 */
	if (!shared->page_dirty[slotno] ||
		shared->page_status[slotno] != SLRU_PAGE_VALID ||
		shared->page_number[slotno] != pageno)
		return;

	/*
	 * Mark the slot write-busy, and clear the dirtybit.  After this point,
	 * a transaction status update on this page will mark it dirty again.
	 */
	shared->page_status[slotno] = SLRU_PAGE_WRITE_IN_PROGRESS;
	shared->page_dirty[slotno] = false;

	/* Acquire per-buffer lock (cannot deadlock, see notes at top) */
	LWLockAcquire(shared->buffer_locks[slotno], LW_EXCLUSIVE);

	/* Release control lock while doing I/O */
	LWLockRelease(shared->ControlLock);

	/* Do the write */
	ok = SlruPhysicalWritePage(ctl, pageno, slotno, fdata);

	/* If we failed, and we're in a flush, better close the files */
	if (!ok && fdata)
	{
		int			i;

		for (i = 0; i < fdata->num_files; i++)
			close(fdata->fd[i]);
	}

	/* Re-acquire control lock and update page state */
	LWLockAcquire(shared->ControlLock, LW_EXCLUSIVE);

	Assert(shared->page_number[slotno] == pageno &&
		   shared->page_status[slotno] == SLRU_PAGE_WRITE_IN_PROGRESS);

	/* If we failed to write, mark the page dirty again */
	if (!ok)
		shared->page_dirty[slotno] = true;

	shared->page_status[slotno] = SLRU_PAGE_VALID;

	LWLockRelease(shared->buffer_locks[slotno]);

	/* Now it's okay to ereport if we failed */
	if (!ok)
		SlruReportIOError(ctl, pageno, InvalidTransactionId);
}

/*
 * Physical read of a (previously existing) page into a buffer slot
 *
 * On failure, we cannot just ereport(ERROR) since caller has put state in
 * shared memory that must be undone.  So, we return FALSE and save enough
 * info in static variables to let SlruReportIOError make the report.
 *
 * For now, assume it's not worth keeping a file pointer open across
 * read/write operations.  We could cache one virtual file pointer ...
 */
static bool
SlruPhysicalReadPage(SlruCtl ctl, int pageno, int slotno)
{
	SlruShared	shared = ctl->shared;
	int			segno = pageno / SLRU_PAGES_PER_SEGMENT;
	int			rpageno = pageno % SLRU_PAGES_PER_SEGMENT;
	int			offset = rpageno * BLCKSZ;
	char		path[MAXPGPATH];
	int			fd;

	SlruFileName(ctl, path, segno);

	/*
	 * In a crash-and-restart situation, it's possible for us to receive
	 * commands to set the commit status of transactions whose bits are in
	 * already-truncated segments of the commit log (see notes in
	 * SlruPhysicalWritePage).	Hence, if we are InRecovery, allow the case
	 * where the file doesn't exist, and return zeroes instead.
	 */
	fd = BasicOpenFile(path, O_RDWR | PG_BINARY, S_IRUSR | S_IWUSR);
	if (fd < 0)
	{
		if (errno != ENOENT || !InRecovery)
		{
			slru_errcause = SLRU_OPEN_FAILED;
			slru_errno = errno;
			return false;
		}

		ereport(LOG,
				(errmsg("file \"%s\" doesn't exist, reading as zeroes",
						path)));
		MemSet(shared->page_buffer[slotno], 0, BLCKSZ);
		return true;
	}

	if (lseek(fd, (off_t) offset, SEEK_SET) < 0)
	{
		slru_errcause = SLRU_SEEK_FAILED;
		slru_errno = errno;
		close(fd);
		return false;
	}

	errno = 0;
	if (read(fd, shared->page_buffer[slotno], BLCKSZ) != BLCKSZ)
	{
		slru_errcause = SLRU_READ_FAILED;
		slru_errno = errno;
		close(fd);
		return false;
	}

	if (close(fd))
	{
		slru_errcause = SLRU_CLOSE_FAILED;
		slru_errno = errno;
		return false;
	}

	return true;
}

/*
 * Physical write of a page from a buffer slot
 *
 * On failure, we cannot just ereport(ERROR) since caller has put state in
 * shared memory that must be undone.  So, we return FALSE and save enough
 * info in static variables to let SlruReportIOError make the report.
 *
 * For now, assume it's not worth keeping a file pointer open across
 * independent read/write operations.  We do batch operations during
 * SimpleLruFlush, though.
 *
 * fdata is NULL for a standalone write, pointer to open-file info during
 * SimpleLruFlush.
 */
static bool
SlruPhysicalWritePage(SlruCtl ctl, int pageno, int slotno, SlruFlush fdata)
{
	SlruShared	shared = ctl->shared;
	int			segno = pageno / SLRU_PAGES_PER_SEGMENT;
	int			rpageno = pageno % SLRU_PAGES_PER_SEGMENT;
	int			offset = rpageno * BLCKSZ;
	char		path[MAXPGPATH];
	int			fd = -1;

	/*
	 * During a Flush, we may already have the desired file open.
	 */
	if (fdata)
	{
		int			i;

		for (i = 0; i < fdata->num_files; i++)
		{
			if (fdata->segno[i] == segno)
			{
				fd = fdata->fd[i];
				break;
			}
		}
	}

	if (fd < 0)
	{
		/*
		 * If the file doesn't already exist, we should create it.  It is
		 * possible for this to need to happen when writing a page that's not
		 * first in its segment; we assume the OS can cope with that. (Note:
		 * it might seem that it'd be okay to create files only when
		 * SimpleLruZeroPage is called for the first page of a segment.
		 * However, if after a crash and restart the REDO logic elects to
		 * replay the log from a checkpoint before the latest one, then it's
		 * possible that we will get commands to set transaction status of
		 * transactions that have already been truncated from the commit log.
		 * Easiest way to deal with that is to accept references to
		 * nonexistent files here and in SlruPhysicalReadPage.)
		 */
		SlruFileName(ctl, path, segno);
		fd = BasicOpenFile(path, O_RDWR | PG_BINARY, S_IRUSR | S_IWUSR);
		if (fd < 0)
		{
			if (errno != ENOENT)
			{
				slru_errcause = SLRU_OPEN_FAILED;
				slru_errno = errno;
				return false;
			}

			fd = BasicOpenFile(path, O_RDWR | O_CREAT | O_EXCL | PG_BINARY,
							   S_IRUSR | S_IWUSR);
			if (fd < 0)
			{
				slru_errcause = SLRU_CREATE_FAILED;
				slru_errno = errno;
				return false;
			}
		}

		if (fdata)
		{
			fdata->fd[fdata->num_files] = fd;
			fdata->segno[fdata->num_files] = segno;
			fdata->num_files++;
		}
	}

	if (lseek(fd, (off_t) offset, SEEK_SET) < 0)
	{
		slru_errcause = SLRU_SEEK_FAILED;
		slru_errno = errno;
		if (!fdata)
			close(fd);
		return false;
	}

	errno = 0;
	if (write(fd, shared->page_buffer[slotno], BLCKSZ) != BLCKSZ)
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		slru_errcause = SLRU_WRITE_FAILED;
		slru_errno = errno;
		if (!fdata)
			close(fd);
		return false;
	}

	/*
	 * If not part of Flush, need to fsync now.  We assume this happens
	 * infrequently enough that it's not a performance issue.
	 */
	if (!fdata)
	{
		if (ctl->do_fsync && pg_fsync(fd))
		{
			slru_errcause = SLRU_FSYNC_FAILED;
			slru_errno = errno;
			close(fd);
			return false;
		}

		if (close(fd))
		{
			slru_errcause = SLRU_CLOSE_FAILED;
			slru_errno = errno;
			return false;
		}
	}

	return true;
}

/*
 * Issue the error message after failure of SlruPhysicalReadPage or
 * SlruPhysicalWritePage.  Call this after cleaning up shared-memory state.
 */
static void
SlruReportIOError(SlruCtl ctl, int pageno, TransactionId xid)
{
	int			segno = pageno / SLRU_PAGES_PER_SEGMENT;
	int			rpageno = pageno % SLRU_PAGES_PER_SEGMENT;
	int			offset = rpageno * BLCKSZ;
	char		path[MAXPGPATH];

	SlruFileName(ctl, path, segno);
	errno = slru_errno;
	switch (slru_errcause)
	{
		case SLRU_OPEN_FAILED:
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not access status of transaction %u", xid),
					 errdetail("could not open file \"%s\": %m",
							   path)));
			break;
		case SLRU_CREATE_FAILED:
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not access status of transaction %u", xid),
					 errdetail("could not create file \"%s\": %m",
							   path)));
			break;
		case SLRU_SEEK_FAILED:
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not access status of transaction %u", xid),
				  errdetail("could not seek in file \"%s\" to offset %u: %m",
							path, offset)));
			break;
		case SLRU_READ_FAILED:
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not access status of transaction %u", xid),
				errdetail("could not read from file \"%s\" at offset %u: %m",
						  path, offset)));
			break;
		case SLRU_WRITE_FAILED:
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not access status of transaction %u", xid),
				 errdetail("could not write to file \"%s\" at offset %u: %m",
						   path, offset)));
			break;
		case SLRU_FSYNC_FAILED:
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not access status of transaction %u", xid),
					 errdetail("could not fsync file \"%s\": %m",
							   path)));
			break;
		case SLRU_CLOSE_FAILED:
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not access status of transaction %u", xid),
					 errdetail("could not close file \"%s\": %m",
							   path)));
			break;
		default:
			/* can't get here, we trust */
			elog(ERROR, "unrecognized SimpleLru error cause: %d",
				 (int) slru_errcause);
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
SlruSelectLRUPage(SlruCtl ctl, int pageno)
{
	SlruShared	shared = ctl->shared;

	/* Outer loop handles restart after I/O */
	for (;;)
	{
		int			slotno;
		int			bestslot = 0;
		unsigned int bestcount = 0;

		/* See if page already has a buffer assigned */
		for (slotno = 0; slotno < NUM_SLRU_BUFFERS; slotno++)
		{
			if (shared->page_number[slotno] == pageno &&
				shared->page_status[slotno] != SLRU_PAGE_EMPTY)
				return slotno;
		}

		/*
		 * If we find any EMPTY slot, just select that one. Else locate the
		 * least-recently-used slot that isn't the latest page.
		 */
		for (slotno = 0; slotno < NUM_SLRU_BUFFERS; slotno++)
		{
			if (shared->page_status[slotno] == SLRU_PAGE_EMPTY)
				return slotno;
			if (shared->page_lru_count[slotno] > bestcount &&
				shared->page_number[slotno] != shared->latest_page_number)
			{
				bestslot = slotno;
				bestcount = shared->page_lru_count[slotno];
			}
		}

		/*
		 * If the selected page is clean, we're set.
		 */
		if (shared->page_status[bestslot] == SLRU_PAGE_VALID &&
			!shared->page_dirty[bestslot])
			return bestslot;

		/*
		 * We need to wait for I/O.  Normal case is that it's dirty and we
		 * must initiate a write, but it's possible that the page is already
		 * write-busy, or in the worst case still read-busy.  In those cases
		 * we wait for the existing I/O to complete.
		 */
		if (shared->page_status[bestslot] == SLRU_PAGE_VALID)
			SimpleLruWritePage(ctl, bestslot, NULL);
		else
			SimpleLruWaitIO(ctl, bestslot);

		/*
		 * Now loop back and try again.  This is the easiest way of dealing
		 * with corner cases such as the victim page being re-dirtied while we
		 * wrote it.
		 */
	}
}

/*
 * Flush dirty pages to disk during checkpoint or database shutdown
 */
void
SimpleLruFlush(SlruCtl ctl, bool checkpoint)
{
	SlruShared	shared = ctl->shared;
	SlruFlushData fdata;
	int			slotno;
	int			pageno = 0;
	int			i;
	bool		ok;

	/*
	 * Find and write dirty pages
	 */
	fdata.num_files = 0;

	LWLockAcquire(shared->ControlLock, LW_EXCLUSIVE);

	for (slotno = 0; slotno < NUM_SLRU_BUFFERS; slotno++)
	{
		SimpleLruWritePage(ctl, slotno, &fdata);

		/*
		 * When called during a checkpoint, we cannot assert that the slot is
		 * clean now, since another process might have re-dirtied it already.
		 * That's okay.
		 */
		Assert(checkpoint ||
			   shared->page_status[slotno] == SLRU_PAGE_EMPTY ||
			   (shared->page_status[slotno] == SLRU_PAGE_VALID &&
				!shared->page_dirty[slotno]));
	}

	LWLockRelease(shared->ControlLock);

	/*
	 * Now fsync and close any files that were open
	 */
	ok = true;
	for (i = 0; i < fdata.num_files; i++)
	{
		if (ctl->do_fsync && pg_fsync(fdata.fd[i]))
		{
			slru_errcause = SLRU_FSYNC_FAILED;
			slru_errno = errno;
			pageno = fdata.segno[i] * SLRU_PAGES_PER_SEGMENT;
			ok = false;
		}

		if (close(fdata.fd[i]))
		{
			slru_errcause = SLRU_CLOSE_FAILED;
			slru_errno = errno;
			pageno = fdata.segno[i] * SLRU_PAGES_PER_SEGMENT;
			ok = false;
		}
	}
	if (!ok)
		SlruReportIOError(ctl, pageno, InvalidTransactionId);
}

/*
 * Remove all segments before the one holding the passed page number
 */
void
SimpleLruTruncate(SlruCtl ctl, int cutoffPage)
{
	SlruShared	shared = ctl->shared;
	int			slotno;

	/*
	 * The cutoff point is the start of the segment containing cutoffPage.
	 */
	cutoffPage -= cutoffPage % SLRU_PAGES_PER_SEGMENT;

	/*
	 * Scan shared memory and remove any pages preceding the cutoff page, to
	 * ensure we won't rewrite them later.  (Since this is normally called in
	 * or just after a checkpoint, any dirty pages should have been flushed
	 * already ... we're just being extra careful here.)
	 */
	LWLockAcquire(shared->ControlLock, LW_EXCLUSIVE);

restart:;

	/*
	 * While we are holding the lock, make an important safety check: the
	 * planned cutoff point must be <= the current endpoint page. Otherwise we
	 * have already wrapped around, and proceeding with the truncation would
	 * risk removing the current segment.
	 */
	if (ctl->PagePrecedes(shared->latest_page_number, cutoffPage))
	{
		LWLockRelease(shared->ControlLock);
		ereport(LOG,
		  (errmsg("could not truncate directory \"%s\": apparent wraparound",
				  ctl->Dir)));
		return;
	}

	for (slotno = 0; slotno < NUM_SLRU_BUFFERS; slotno++)
	{
		if (shared->page_status[slotno] == SLRU_PAGE_EMPTY)
			continue;
		if (!ctl->PagePrecedes(shared->page_number[slotno], cutoffPage))
			continue;

		/*
		 * If page is clean, just change state to EMPTY (expected case).
		 */
		if (shared->page_status[slotno] == SLRU_PAGE_VALID &&
			!shared->page_dirty[slotno])
		{
			shared->page_status[slotno] = SLRU_PAGE_EMPTY;
			continue;
		}

		/*
		 * Hmm, we have (or may have) I/O operations acting on the page, so
		 * we've got to wait for them to finish and then start again. This is
		 * the same logic as in SlruSelectLRUPage.  (XXX if page is dirty,
		 * wouldn't it be OK to just discard it without writing it?  For now,
		 * keep the logic the same as it was.)
		 */
		if (shared->page_status[slotno] == SLRU_PAGE_VALID)
			SimpleLruWritePage(ctl, slotno, NULL);
		else
			SimpleLruWaitIO(ctl, slotno);
		goto restart;
	}

	LWLockRelease(shared->ControlLock);

	/* Now we can remove the old segment(s) */
	(void) SlruScanDirectory(ctl, cutoffPage, true);
}

/*
 * SimpleLruTruncate subroutine: scan directory for removable segments.
 * Actually remove them iff doDeletions is true.  Return TRUE iff any
 * removable segments were found.  Note: no locking is needed.
 *
 * This can be called directly from clog.c, for reasons explained there.
 */
bool
SlruScanDirectory(SlruCtl ctl, int cutoffPage, bool doDeletions)
{
	bool		found = false;
	DIR		   *cldir;
	struct dirent *clde;
	int			segno;
	int			segpage;
	char		path[MAXPGPATH];

	/*
	 * The cutoff point is the start of the segment containing cutoffPage.
	 * (This is redundant when called from SimpleLruTruncate, but not when
	 * called directly from clog.c.)
	 */
	cutoffPage -= cutoffPage % SLRU_PAGES_PER_SEGMENT;

	cldir = AllocateDir(ctl->Dir);
	while ((clde = ReadDir(cldir, ctl->Dir)) != NULL)
	{
		if (strlen(clde->d_name) == 4 &&
			strspn(clde->d_name, "0123456789ABCDEF") == 4)
		{
			segno = (int) strtol(clde->d_name, NULL, 16);
			segpage = segno * SLRU_PAGES_PER_SEGMENT;
			if (ctl->PagePrecedes(segpage, cutoffPage))
			{
				found = true;
				if (doDeletions)
				{
					snprintf(path, MAXPGPATH, "%s/%s", ctl->Dir, clde->d_name);
					ereport(DEBUG2,
							(errmsg("removing file \"%s\"", path)));
					unlink(path);
				}
			}
		}
	}
	FreeDir(cldir);

	return found;
}
