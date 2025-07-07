/*-------------------------------------------------------------------------
 *
 * slru.h
 *		Simple LRU buffering for transaction status logfiles
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/slru.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SLRU_H
#define SLRU_H

#include "access/xlogdefs.h"
#include "storage/lwlock.h"
#include "storage/sync.h"

/*
 * To avoid overflowing internal arithmetic and the size_t data type, the
 * number of buffers must not exceed this number.
 */
#define SLRU_MAX_ALLOWED_BUFFERS ((1024 * 1024 * 1024) / BLCKSZ)

/*
 * Define SLRU segment size.  A page is the same BLCKSZ as is used everywhere
 * else in Postgres.  The segment size can be chosen somewhat arbitrarily;
 * we make it 32 pages by default, or 256Kb, i.e. 1M transactions for CLOG
 * or 64K transactions for SUBTRANS.
 *
 * Note: because TransactionIds are 32 bits and wrap around at 0xFFFFFFFF,
 * page numbering also wraps around at 0xFFFFFFFF/xxxx_XACTS_PER_PAGE (where
 * xxxx is CLOG or SUBTRANS, respectively), and segment numbering at
 * 0xFFFFFFFF/xxxx_XACTS_PER_PAGE/SLRU_PAGES_PER_SEGMENT.  We need
 * take no explicit notice of that fact in slru.c, except when comparing
 * segment and page numbers in SimpleLruTruncate (see PagePrecedes()).
 */
#define SLRU_PAGES_PER_SEGMENT	32

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
 * ControlLock is used to protect access to the other fields, except
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

/*
 * SlruCtlData is an unshared structure that points to the active information
 * in shared memory.
 */
typedef struct SlruCtlData
{
	SlruShared	shared;

	/* Number of banks in this SLRU. */
	uint16		nbanks;

	/*
	 * If true, use long segment file names.  Otherwise, use short file names.
	 *
	 * For details about the file name format, see SlruFileName().
	 */
	bool		long_segment_names;

	/*
	 * Which sync handler function to use when handing sync requests over to
	 * the checkpointer.  SYNC_HANDLER_NONE to disable fsync (eg pg_notify).
	 */
	SyncRequestHandler sync_handler;

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
	 * Dir is set during SimpleLruInit and does not change thereafter. Since
	 * it's always the same, it doesn't need to be in shared memory.
	 */
	char		Dir[64];
} SlruCtlData;

typedef SlruCtlData *SlruCtl;

/*
 * Get the SLRU bank lock for given SlruCtl and the pageno.
 *
 * This lock needs to be acquired to access the slru buffer slots in the
 * respective bank.
 */
static inline LWLock *
SimpleLruGetBankLock(SlruCtl ctl, int64 pageno)
{
	int			bankno;

	bankno = pageno % ctl->nbanks;
	return &(ctl->shared->bank_locks[bankno].lock);
}

extern Size SimpleLruShmemSize(int nslots, int nlsns);
extern int	SimpleLruAutotuneBuffers(int divisor, int max);
extern void SimpleLruInit(SlruCtl ctl, const char *name, int nslots, int nlsns,
						  const char *subdir, int buffer_tranche_id,
						  int bank_tranche_id, SyncRequestHandler sync_handler,
						  bool long_segment_names);
extern int	SimpleLruZeroPage(SlruCtl ctl, int64 pageno);
extern void SimpleLruZeroAndWritePage(SlruCtl ctl, int64 pageno);
extern int	SimpleLruReadPage(SlruCtl ctl, int64 pageno, bool write_ok,
							  TransactionId xid);
extern int	SimpleLruReadPage_ReadOnly(SlruCtl ctl, int64 pageno,
									   TransactionId xid);
extern void SimpleLruWritePage(SlruCtl ctl, int slotno);
extern void SimpleLruWriteAll(SlruCtl ctl, bool allow_redirtied);
#ifdef USE_ASSERT_CHECKING
extern void SlruPagePrecedesUnitTests(SlruCtl ctl, int per_page);
#else
#define SlruPagePrecedesUnitTests(ctl, per_page) do {} while (0)
#endif
extern void SimpleLruTruncate(SlruCtl ctl, int64 cutoffPage);
extern bool SimpleLruDoesPhysicalPageExist(SlruCtl ctl, int64 pageno);

typedef bool (*SlruScanCallback) (SlruCtl ctl, char *filename, int64 segpage,
								  void *data);
extern bool SlruScanDirectory(SlruCtl ctl, SlruScanCallback callback, void *data);
extern void SlruDeleteSegment(SlruCtl ctl, int64 segno);

extern int	SlruSyncFileTag(SlruCtl ctl, const FileTag *ftag, char *path);

/* SlruScanDirectory public callbacks */
extern bool SlruScanDirCbReportPresence(SlruCtl ctl, char *filename,
										int64 segpage, void *data);
extern bool SlruScanDirCbDeleteAll(SlruCtl ctl, char *filename, int64 segpage,
								   void *data);
extern bool check_slru_buffers(const char *name, int *newval);

#endif							/* SLRU_H */
