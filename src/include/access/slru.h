/*-------------------------------------------------------------------------
 *
 * slru.h
 *		Simple LRU buffering for transaction status logfiles
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/slru.h,v 1.14 2005/10/15 02:49:42 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SLRU_H
#define SLRU_H

#include "storage/lwlock.h"


/*
 * Number of page buffers.	Ideally this could be different for CLOG and
 * SUBTRANS, but the benefit doesn't seem to be worth any additional
 * notational cruft.
 */
#define NUM_SLRU_BUFFERS	8

/* Page status codes */
typedef enum
{
	SLRU_PAGE_EMPTY,			/* buffer is not in use */
	SLRU_PAGE_READ_IN_PROGRESS, /* page is being read in */
	SLRU_PAGE_CLEAN,			/* page is valid and not dirty */
	SLRU_PAGE_DIRTY,			/* page is valid but needs write */
	SLRU_PAGE_WRITE_IN_PROGRESS /* page is being written out */
} SlruPageStatus;

/*
 * Shared-memory state
 */
typedef struct SlruSharedData
{
	LWLockId	ControlLock;

	/*
	 * Info for each buffer slot.  Page number is undefined when status is
	 * EMPTY.  lru_count is essentially the number of page switches since last
	 * use of this page; the page with highest lru_count is the best candidate
	 * to replace.
	 */
	char	   *page_buffer[NUM_SLRU_BUFFERS];
	SlruPageStatus page_status[NUM_SLRU_BUFFERS];
	int			page_number[NUM_SLRU_BUFFERS];
	unsigned int page_lru_count[NUM_SLRU_BUFFERS];
	LWLockId	buffer_locks[NUM_SLRU_BUFFERS];

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


extern Size SimpleLruShmemSize(void);
extern void SimpleLruInit(SlruCtl ctl, const char *name,
			  LWLockId ctllock, const char *subdir);
extern int	SimpleLruZeroPage(SlruCtl ctl, int pageno);
extern int	SimpleLruReadPage(SlruCtl ctl, int pageno, TransactionId xid);
extern void SimpleLruWritePage(SlruCtl ctl, int slotno, SlruFlush fdata);
extern void SimpleLruFlush(SlruCtl ctl, bool checkpoint);
extern void SimpleLruTruncate(SlruCtl ctl, int cutoffPage);
extern bool SlruScanDirectory(SlruCtl ctl, int cutoffPage, bool doDeletions);

#endif   /* SLRU_H */
