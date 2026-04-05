/*-------------------------------------------------------------------------
 *
 * slru.h
 *		Simple LRU buffering for transaction status logfiles
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/slru.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SLRU_H
#define SLRU_H

#include "access/transam.h"
#include "access/xlogdefs.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/sync.h"

/*
 * To avoid overflowing internal arithmetic and the size_t data type, the
 * number of buffers must not exceed this number.
 */
#define SLRU_MAX_ALLOWED_BUFFERS ((1024 * 1024 * 1024) / BLCKSZ)

/*
 * Page status codes.  Note that these do not include the "dirty" bit.
 * page_dirty can be true only in the VALID or WRITE_IN_PROGRESS states;
 * in the latter case it implies that the page has been re-dirtied since
 * the write started.
 */
typedef enum
{
	SLRU_PAGE_EMPTY,			/* buffer is not in use */
	SLRU_PAGE_READ_IN_PROGRESS, /* page is being read in */
	SLRU_PAGE_VALID,			/* page is valid and not being written */
	SLRU_PAGE_WRITE_IN_PROGRESS,	/* page is being written out */
} SlruPageStatus;

/*
 * Shared-memory state
 *
 * SLRU bank locks are used to protect access to the other fields, except
 * latest_page_number, which uses atomics; see comment in slru.c.
 */
typedef struct SlruSharedData
{
	/* Number of buffers managed by this SLRU structure */
	int			num_slots;

	/*
	 * Arrays holding info for each buffer slot.  Page number is undefined
	 * when status is EMPTY, as is page_lru_count.
	 */
	char	  **page_buffer;
	SlruPageStatus *page_status;
	bool	   *page_dirty;
	int64	   *page_number;
	int		   *page_lru_count;

	/* The buffer_locks protects the I/O on each buffer slots */
	LWLockPadded *buffer_locks;

	/* Locks to protect the in memory buffer slot access in SLRU bank. */
	LWLockPadded *bank_locks;

	/*----------
	 * A bank-wise LRU counter is maintained because we do a victim buffer
	 * search within a bank. Furthermore, manipulating an individual bank
	 * counter avoids frequent cache invalidation since we update it every time
	 * we access the page.
	 *
	 * We mark a page "most recently used" by setting
	 *		page_lru_count[slotno] = ++bank_cur_lru_count[bankno];
	 * The oldest page in the bank is therefore the one with the highest value
	 * of
	 * 		bank_cur_lru_count[bankno] - page_lru_count[slotno]
	 * The counts will eventually wrap around, but this calculation still
	 * works as long as no page's age exceeds INT_MAX counts.
	 *----------
	 */
	int		   *bank_cur_lru_count;

	/*
	 * Optional array of WAL flush LSNs associated with entries in the SLRU
	 * pages.  If not zero/NULL, we must flush WAL before writing pages (true
	 * for pg_xact, false for everything else).  group_lsn[] has
	 * lsn_groups_per_page entries per buffer slot, each containing the
	 * highest LSN known for a contiguous group of SLRU entries on that slot's
	 * page.
	 */
	XLogRecPtr *group_lsn;
	int			lsn_groups_per_page;

	/*
	 * latest_page_number is the page number of the current end of the log;
	 * this is not critical data, since we use it only to avoid swapping out
	 * the latest page.
	 */
	pg_atomic_uint64 latest_page_number;

	/* SLRU's index for statistics purposes (might not be unique) */
	int			slru_stats_idx;
} SlruSharedData;

typedef SlruSharedData *SlruShared;

typedef struct SlruDesc SlruDesc;

/*
 * Options for SimpleLruRequest()
 */
typedef struct SlruOpts
{
	/* Options for allocating the underlying shmem area; do not touch directly */
	ShmemStructOpts base;

	/*
	 * name of SLRU.  (This is user-visible, pick with care!)
	 */
	const char *name;

	/*
	 * Pointer to a backend-private handle for the SLRU.  It is initialized
	 * when the SLRU is initialized or attached to.
	 */
	SlruDesc   *desc;

	/* number of page slots to use. */
	int			nslots;

	/* number of LSN groups per page (set to zero if not relevant). */
	int			nlsns;

	/*
	 * Which sync handler function to use when handing sync requests over to
	 * the checkpointer.  SYNC_HANDLER_NONE to disable fsync (eg pg_notify).
	 */
	SyncRequestHandler sync_handler;

	/*
	 * PGDATA-relative subdirectory that will contain the files.
	 */
	const char *Dir;

	/*
	 * If true, use long segment file names.  Otherwise, use short file names.
	 *
	 * For details about the file name format, see SlruFileName().
	 */
	bool		long_segment_names;


	/*
	 * Decide whether a page is "older" for truncation and as a hint for
	 * evicting pages in LRU order.  Return true if every entry of the first
	 * argument is older than every entry of the second argument.  Note that
	 * !PagePrecedes(a,b) && !PagePrecedes(b,a) need not imply a==b; it also
	 * arises when some entries are older and some are not.  For SLRUs using
	 * SimpleLruTruncate(), this must use modular arithmetic.  (For others,
	 * the behavior of this callback has no functional implications.)  Use
	 * SlruPagePrecedesUnitTests() in SLRUs meeting its criteria.
	 */
	bool		(*PagePrecedes) (int64, int64);

	/*
	 * Callback to provide more details on an I/O error.  This is called as
	 * part of ereport(), and the callback function is expected to call
	 * errdetail() to provide more context on the SLRU access.
	 *
	 * The opaque_data argument here is the argument that was passed to the
	 * SimpleLruReadPage() function.
	 */
	int			(*errdetail_for_io_error) (const void *opaque_data);

	/*
	 * Tranche IDs to use for the SLRU's per-buffer and per-bank LWLocks.  If
	 * these are left as zeros, new tranches will be assigned dynamically.
	 */
	int			buffer_tranche_id;
	int			bank_tranche_id;
} SlruOpts;

/*
 * SlruDesc is an unshared structure that points to the active information
 * in shared memory.
 */
typedef struct SlruDesc
{
	SlruOpts	options;

	SlruShared	shared;

	/* Number of banks in this SLRU. */
	uint16		nbanks;
} SlruDesc;

/*
 * Get the SLRU bank lock for the given pageno.
 *
 * This lock needs to be acquired to access the slru buffer slots in the
 * respective bank.
 */
static inline LWLock *
SimpleLruGetBankLock(SlruDesc *ctl, int64 pageno)
{
	int			bankno;

	Assert(ctl->nbanks != 0);
	bankno = pageno % ctl->nbanks;
	return &(ctl->shared->bank_locks[bankno].lock);
}

extern void SimpleLruRequestWithOpts(const SlruOpts *options);

#define SimpleLruRequest(...)  \
	SimpleLruRequestWithOpts(&(SlruOpts){__VA_ARGS__})

extern int	SimpleLruAutotuneBuffers(int divisor, int max);
extern int	SimpleLruZeroPage(SlruDesc *ctl, int64 pageno);
extern void SimpleLruZeroAndWritePage(SlruDesc *ctl, int64 pageno);
extern int	SimpleLruReadPage(SlruDesc *ctl, int64 pageno, bool write_ok,
							  const void *opaque_data);
extern int	SimpleLruReadPage_ReadOnly(SlruDesc *ctl, int64 pageno,
									   const void *opaque_data);
extern void SimpleLruWritePage(SlruDesc *ctl, int slotno);
extern void SimpleLruWriteAll(SlruDesc *ctl, bool allow_redirtied);
#ifdef USE_ASSERT_CHECKING
extern void SlruPagePrecedesUnitTests(SlruDesc *ctl, int per_page);
#else
#define SlruPagePrecedesUnitTests(ctl, per_page) do {} while (0)
#endif
extern void SimpleLruTruncate(SlruDesc *ctl, int64 cutoffPage);
extern bool SimpleLruDoesPhysicalPageExist(SlruDesc *ctl, int64 pageno);

typedef bool (*SlruScanCallback) (SlruDesc *ctl, char *filename, int64 segpage,
								  void *data);
extern bool SlruScanDirectory(SlruDesc *ctl, SlruScanCallback callback, void *data);
extern void SlruDeleteSegment(SlruDesc *ctl, int64 segno);

extern int	SlruSyncFileTag(SlruDesc *ctl, const FileTag *ftag, char *path);

/* SlruScanDirectory public callbacks */
extern bool SlruScanDirCbReportPresence(SlruDesc *ctl, char *filename,
										int64 segpage, void *data);
extern bool SlruScanDirCbDeleteAll(SlruDesc *ctl, char *filename, int64 segpage,
								   void *data);
extern bool check_slru_buffers(const char *name, int *newval);

extern void shmem_slru_init(void *location, ShmemStructOpts *base_options);
extern void shmem_slru_attach(void *location, ShmemStructOpts *base_options);

#endif							/* SLRU_H */
