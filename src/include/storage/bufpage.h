/*-------------------------------------------------------------------------
 *
 * bufpage.h
 *	  Standard POSTGRES buffer page definitions.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: bufpage.h,v 1.55.4.1 2004/06/05 17:42:50 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUFPAGE_H
#define BUFPAGE_H

#include "storage/buf.h"
#include "storage/bufmgr.h"
#include "storage/item.h"
#include "storage/itemid.h"
#include "storage/off.h"
#include "access/xlog.h"

/*
 * A postgres disk page is an abstraction layered on top of a postgres
 * disk block (which is simply a unit of i/o, see block.h).
 *
 * specifically, while a disk block can be unformatted, a postgres
 * disk page is always a slotted page of the form:
 *
 * +----------------+---------------------------------+
 * | PageHeaderData | linp1 linp2 linp3 ...			  |
 * +-----------+----+---------------------------------+
 * | ... linpN |									  |
 * +-----------+--------------------------------------+
 * |		   ^ pd_lower							  |
 * |												  |
 * |			 v pd_upper							  |
 * +-------------+------------------------------------+
 * |			 | tupleN ...						  |
 * +-------------+------------------+-----------------+
 * |	   ... tuple3 tuple2 tuple1 | "special space" |
 * +--------------------------------+-----------------+
 *									^ pd_special
 *
 * a page is full when nothing can be added between pd_lower and
 * pd_upper.
 *
 * all blocks written out by an access method must be disk pages.
 *
 * EXCEPTIONS:
 *
 * obviously, a page is not formatted before it is initialized with by
 * a call to PageInit.
 *
 * NOTES:
 *
 * linp1..N form an ItemId array.  ItemPointers point into this array
 * rather than pointing directly to a tuple.  Note that OffsetNumbers
 * conventionally start at 1, not 0.
 *
 * tuple1..N are added "backwards" on the page.  because a tuple's
 * ItemPointer points to its ItemId entry rather than its actual
 * byte-offset position, tuples can be physically shuffled on a page
 * whenever the need arises.
 *
 * AM-generic per-page information is kept in PageHeaderData.
 *
 * AM-specific per-page data (if any) is kept in the area marked "special
 * space"; each AM has an "opaque" structure defined somewhere that is
 * stored as the page trailer.	an access method should always
 * initialize its pages with PageInit and then set its own opaque
 * fields.
 */

typedef Pointer Page;


/*
 * location (byte offset) within a page.
 *
 * note that this is actually limited to 2^15 because we have limited
 * ItemIdData.lp_off and ItemIdData.lp_len to 15 bits (see itemid.h).
 */
typedef uint16 LocationIndex;


/*
 * disk page organization
 * space management information generic to any page
 *
 *		pd_lower	- offset to start of free space.
 *		pd_upper	- offset to end of free space.
 *		pd_special	- offset to start of special space.
 *		pd_pagesize_version - size in bytes and page layout version number.
 *
 * The page version number and page size are packed together into a single
 * uint16 field.  This is for historical reasons: before PostgreSQL 7.3,
 * there was no concept of a page version number, and doing it this way
 * lets us pretend that pre-7.3 databases have page version number zero.
 * We constrain page sizes to be multiples of 256, leaving the low eight
 * bytes available for a version number.
 *
 * Minimum possible page size is perhaps 64B to fit page header, opaque space
 * and a minimal tuple; of course, in reality you want it much bigger, so
 * the constraint on pagesize mod 256 is not an important restriction.
 * On the high end, we can only support pages up to 32KB because lp_off/lp_len
 * are 15 bits.
 */
typedef struct PageHeaderData
{
	/* XXX LSN is member of *any* block, not */
	/* only page-organized - 'll change later */
	XLogRecPtr	pd_lsn;			/* LSN: next byte after last byte of xlog */
	/* record for last change of this page */
	StartUpID	pd_sui;			/* SUI of last changes (currently it's */
	/* used by heap AM only) */

	LocationIndex pd_lower;		/* offset to start of free space */
	LocationIndex pd_upper;		/* offset to end of free space */
	LocationIndex pd_special;	/* offset to start of special space */
	uint16		pd_pagesize_version;
	ItemIdData	pd_linp[1];		/* beginning of line pointer array */
} PageHeaderData;

typedef PageHeaderData *PageHeader;

/*
 * Page layout version number 0 is for pre-7.3 Postgres releases.  The
 * current version number is 1, denoting a new HeapTupleHeader layout.
 */
#define PG_PAGE_LAYOUT_VERSION		1


/* ----------------------------------------------------------------
 *						page support macros
 * ----------------------------------------------------------------
 */

/*
 * PageIsValid
 *		True iff page is valid.
 */
#define PageIsValid(page) PointerIsValid(page)

/*
 * line pointer does not count as part of header
 */
#define SizeOfPageHeaderData (offsetof(PageHeaderData, pd_linp[0]))

/*
 * PageIsEmpty
 *		returns true iff no itemid has been allocated on the page
 */
#define PageIsEmpty(page) \
	(((PageHeader) (page))->pd_lower <= SizeOfPageHeaderData)

/*
 * PageIsNew
 *		returns true iff page has not been initialized (by PageInit)
 */
#define PageIsNew(page) (((PageHeader) (page))->pd_upper == 0)

/*
 * PageGetItemId
 *		Returns an item identifier of a page.
 */
#define PageGetItemId(page, offsetNumber) \
	((ItemId) (&((PageHeader) (page))->pd_linp[(offsetNumber) - 1]))

/*
 * PageGetContents
 *		To be used in case the page does not contain item pointers.
 */
#define PageGetContents(page) \
	((char *) (&((PageHeader) (page))->pd_linp[0]))

/* ----------------
 *		macros to access page size info
 * ----------------
 */

/*
 * PageSizeIsValid
 *		True iff the page size is valid.
 */
#define PageSizeIsValid(pageSize) ((pageSize) == BLCKSZ)

/*
 * PageGetPageSize
 *		Returns the page size of a page.
 *
 * this can only be called on a formatted page (unlike
 * BufferGetPageSize, which can be called on an unformatted page).
 * however, it can be called on a page that is not stored in a buffer.
 */
#define PageGetPageSize(page) \
	((Size) (((PageHeader) (page))->pd_pagesize_version & (uint16) 0xFF00))

/*
 * PageGetPageLayoutVersion
 *		Returns the page layout version of a page.
 */
#define PageGetPageLayoutVersion(page) \
	(((PageHeader) (page))->pd_pagesize_version & 0x00FF)

/*
 * PageSetPageSizeAndVersion
 *		Sets the page size and page layout version number of a page.
 *
 * We could support setting these two values separately, but there's
 * no real need for it at the moment.
 */
#define PageSetPageSizeAndVersion(page, size, version) \
( \
	AssertMacro(((size) & 0xFF00) == (size)), \
	AssertMacro(((version) & 0x00FF) == (version)), \
	((PageHeader) (page))->pd_pagesize_version = (size) | (version) \
)

/* ----------------
 *		page special data macros
 * ----------------
 */
/*
 * PageGetSpecialSize
 *		Returns size of special space on a page.
 */
#define PageGetSpecialSize(page) \
	((uint16) (PageGetPageSize(page) - ((PageHeader)(page))->pd_special))

/*
 * PageGetSpecialPointer
 *		Returns pointer to special space on a page.
 */
#define PageGetSpecialPointer(page) \
( \
	AssertMacro(PageIsValid(page)), \
	(char *) ((char *) (page) + ((PageHeader) (page))->pd_special) \
)

/*
 * PageGetItem
 *		Retrieves an item on the given page.
 *
 * Note:
 *		This does not change the status of any of the resources passed.
 *		The semantics may change in the future.
 */
#define PageGetItem(page, itemId) \
( \
	AssertMacro(PageIsValid(page)), \
	AssertMacro((itemId)->lp_flags & LP_USED), \
	(Item)(((char *)(page)) + (itemId)->lp_off) \
)

/*
 * BufferGetPageSize
 *		Returns the page size within a buffer.
 *
 * Notes:
 *		Assumes buffer is valid.
 *
 *		The buffer can be a raw disk block and need not contain a valid
 *		(formatted) disk page.
 */
/* XXX should dig out of buffer descriptor */
#define BufferGetPageSize(buffer) \
( \
	AssertMacro(BufferIsValid(buffer)), \
	(Size)BLCKSZ \
)

/*
 * BufferGetPage
 *		Returns the page associated with a buffer.
 */
#define BufferGetPage(buffer) ((Page)BufferGetBlock(buffer))

/*
 * PageGetMaxOffsetNumber
 *		Returns the maximum offset number used by the given page.
 *		Since offset numbers are 1-based, this is also the number
 *		of items on the page.
 *
 *		NOTE: if the page is not initialized (pd_lower == 0), we must
 *		return zero to ensure sane behavior.  Accept double evaluation
 *		of the argument so that we can ensure this.
 */
#define PageGetMaxOffsetNumber(page) \
	(((PageHeader) (page))->pd_lower <= SizeOfPageHeaderData ? 0 : \
	 ((((PageHeader) (page))->pd_lower - SizeOfPageHeaderData) \
	  / sizeof(ItemIdData)))

#define PageGetLSN(page) \
	(((PageHeader) (page))->pd_lsn)
#define PageSetLSN(page, lsn) \
	(((PageHeader) (page))->pd_lsn = (lsn))

#define PageGetSUI(page) \
	(((PageHeader) (page))->pd_sui)
#define PageSetSUI(page, sui) \
	(((PageHeader) (page))->pd_sui = (StartUpID) (sui))

/* ----------------------------------------------------------------
 *		extern declarations
 * ----------------------------------------------------------------
 */

extern void PageInit(Page page, Size pageSize, Size specialSize);
extern bool PageHeaderIsValid(PageHeader page);
extern OffsetNumber PageAddItem(Page page, Item item, Size size,
			OffsetNumber offsetNumber, ItemIdFlags flags);
extern Page PageGetTempPage(Page page, Size specialSize);
extern void PageRestoreTempPage(Page tempPage, Page oldPage);
extern int	PageRepairFragmentation(Page page, OffsetNumber *unused);
extern Size PageGetFreeSpace(Page page);
extern void PageIndexTupleDelete(Page page, OffsetNumber offset);

#endif   /* BUFPAGE_H */
