/*-------------------------------------------------------------------------
 *
 * slru.h
 *		Simple LRU buffering for transaction status logfiles
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
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
 *
 * Note: slru.c currently assumes that segment file names will be four hex
 * digits.  This sets a lower bound on the segment size (64K transactions
 * for 32-bit TransactionIds).
 */
#define SLRU_PAGES_PER_SEGMENT	32

/* Maximum length of an SLRU name */
#define SLRU_MAX_NAME_LENGTH	32

/*
 * Page status codes.  Note that these do not include the "dirty" bit.
 * page_dirty can be TRUE only in the VALID or WRITE_IN_PROGRESS states;
 * in the latter case it implies that the page has been re-dirtied since
 * the write started.
 */
typedef enum
{
	SLRU_PAGE_EMPTY,			/* buffer is not in use */
	SLRU_PAGE_READ_IN_PROGRESS, /* page is being read in */
	SLRU_PAGE_VALID,			/* page is valid and not being written */
	SLRU_PAGE_WRITE_IN_PROGRESS /* page is being written out */
} SlruPageStatus;

/*
 * Shared-memory state
 */
typedef struct SlruSharedData
{
	LWLock	   *ControlLock;

	/* Number of buffers managed by this SLRU structure */
	int			num_slots;

	/*
	 * Arrays holding info for each buffer slot.  Page number is undefined
	 * when status is EMPTY, as is page_lru_count.
	 */
	char	  **page_buffer;
	SlruPageStatus *page_status;
	bool	   *page_dirty;
	int		   *page_number;
	int		   *page_lru_count;

	/*
	 * Optional array of WAL flush LSNs associated with entries in the SLRU
	 * pages.  If not zero/NULL, we must flush WAL before writing pages (true
	 * for pg_clog, false for multixact, pg_subtrans, pg_notify).  group_lsn[]
	 * has lsn_groups_per_page entries per buffer slot, each containing the
	 * highest LSN known for a contiguous group of SLRU entries on that slot's
	 * page.
	 */
	XLogRecPtr *group_lsn;
	int			lsn_groups_per_page;

	/*----------
	 * We mark a page "most recently used" by setting
	 *		page_lru_count[slotno] = ++cur_lru_count;
	 * The oldest page is therefore the one with the highest value of
	 *		cur_lru_count - page_lru_count[slotno]
	 * The counts will eventually wrap around, but this calculation still
	 * works as long as no page's age exceeds INT_MAX counts.
	 *----------
	 */
	int			cur_lru_count;

	/*
	 * latest_page_number is the page number of the current end of the log;
	 * this is not critical data, since we use it only to avoid swapping out
	 * the latest page.
	 */
	int			latest_page_number;

	/* LWLocks */
	int			lwlock_tranche_id;
	LWLockTranche lwlock_tranche;
	char		lwlock_tranche_name[SLRU_MAX_NAME_LENGTH];
	LWLockPadded *buffer_locks;
} SlruSharedData;

typedef SlruSharedData *SlruShared;

/*
 * SlruCtlData is an unshared structure that points to the active information
 * in shared memory.
 */
typedef struct SlruCtlData
{
	SlruShared	shared;

	/*
	 * This flag tells whether to fsync writes (true for pg_clog and multixact
	 * stuff, false for pg_subtrans and pg_notify).
	 */
	bool		do_fsync;

	/*
	 * Decide which of two page numbers is "older" for truncation purposes. We
	 * need to use comparison of TransactionIds here in order to do the right
	 * thing with wraparound XID arithmetic.
	 */
	bool		(*PagePrecedes) (int, int);

	/*
	 * Dir is set during SimpleLruInit and does not change thereafter. Since
	 * it's always the same, it doesn't need to be in shared memory.
	 */
	char		Dir[64];
} SlruCtlData;

typedef SlruCtlData *SlruCtl;


extern Size SimpleLruShmemSize(int nslots, int nlsns);
extern void SimpleLruInit(SlruCtl ctl, const char *name, int nslots, int nlsns,
			  LWLock *ctllock, const char *subdir);
extern int	SimpleLruZeroPage(SlruCtl ctl, int pageno);
extern int SimpleLruReadPage(SlruCtl ctl, int pageno, bool write_ok,
				  TransactionId xid);
extern int SimpleLruReadPage_ReadOnly(SlruCtl ctl, int pageno,
						   TransactionId xid);
extern void SimpleLruWritePage(SlruCtl ctl, int slotno);
extern void SimpleLruFlush(SlruCtl ctl, bool allow_redirtied);
extern void SimpleLruTruncate(SlruCtl ctl, int cutoffPage);
extern bool SimpleLruDoesPhysicalPageExist(SlruCtl ctl, int pageno);

typedef bool (*SlruScanCallback) (SlruCtl ctl, char *filename, int segpage,
											  void *data);
extern bool SlruScanDirectory(SlruCtl ctl, SlruScanCallback callback, void *data);
extern void SlruDeleteSegment(SlruCtl ctl, int segno);

/* SlruScanDirectory public callbacks */
extern bool SlruScanDirCbReportPresence(SlruCtl ctl, char *filename,
							int segpage, void *data);
extern bool SlruScanDirCbDeleteAll(SlruCtl ctl, char *filename, int segpage,
					   void *data);

#endif   /* SLRU_H */
