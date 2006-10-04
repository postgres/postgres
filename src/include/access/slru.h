/*-------------------------------------------------------------------------
 *
 * slru.h
 *		Simple LRU buffering for transaction status logfiles
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/slru.h,v 1.19 2006/10/04 00:30:07 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SLRU_H
#define SLRU_H

#include "storage/lwlock.h"


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
	LWLockId	ControlLock;

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
	LWLockId   *buffer_locks;

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
	 * This flag tells whether to fsync writes (true for pg_clog, false for
	 * pg_subtrans).
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

/* Opaque struct known only in slru.c */
typedef struct SlruFlushData *SlruFlush;


extern Size SimpleLruShmemSize(int nslots);
extern void SimpleLruInit(SlruCtl ctl, const char *name, int nslots,
			  LWLockId ctllock, const char *subdir);
extern int	SimpleLruZeroPage(SlruCtl ctl, int pageno);
extern int	SimpleLruReadPage(SlruCtl ctl, int pageno, TransactionId xid);
extern int SimpleLruReadPage_ReadOnly(SlruCtl ctl, int pageno,
						   TransactionId xid);
extern void SimpleLruWritePage(SlruCtl ctl, int slotno, SlruFlush fdata);
extern void SimpleLruFlush(SlruCtl ctl, bool checkpoint);
extern void SimpleLruTruncate(SlruCtl ctl, int cutoffPage);
extern bool SlruScanDirectory(SlruCtl ctl, int cutoffPage, bool doDeletions);

#endif   /* SLRU_H */
