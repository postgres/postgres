/*-------------------------------------------------------------------------
 *
 * bufpage.h--
 *	  Standard POSTGRES buffer page definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: bufpage.h,v 1.19 1998/06/15 18:40:02 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUFPAGE_H
#define BUFPAGE_H

#include <storage/off.h>
#include <storage/itemid.h>
#include <storage/item.h>
#include <storage/buf.h>
#include <storage/page.h>

/*
 * a postgres disk page is an abstraction layered on top of a postgres
 * disk block (which is simply a unit of i/o, see block.h).
 *
 * specifically, while a disk block can be unformatted, a postgres
 * disk page is always a slotted page of the form:
 *
 * +----------------+---------------------------------+
 * | PageHeaderData | linp0 linp1 linp2 ...			  |
 * +-----------+----+---------------------------------+
 * | ... linpN |									  |
 * +-----------+--------------------------------------+
 * |		   ^ pd_lower							  |
 * |												  |
 * |			 v pd_upper							  |
 * +-------------+------------------------------------+
 * |			 | tupleN ...						  |
 * +-------------+------------------+-----------------+
 * |	   ... tuple2 tuple1 tuple0 | "special space" |
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
 * the contents of the special pg_variable/pg_time/pg_log tables are
 * raw disk blocks with special formats.  these are the only "access
 * methods" that need not write disk pages.
 *
 * NOTES:
 *
 * linp0..N form an ItemId array.  ItemPointers point into this array
 * rather than pointing directly to a tuple.
 *
 * tuple0..N are added "backwards" on the page.  because a tuple's
 * ItemPointer points to its ItemId entry rather than its actual
 * byte-offset position, tuples can be physically shuffled on a page
 * whenever the need arises.
 *
 * AM-generic per-page information is kept in the pd_opaque field of
 * the PageHeaderData.	(this is currently only the page size.)
 * AM-specific per-page data is kept in the area marked "special
 * space"; each AM has an "opaque" structure defined somewhere that is
 * stored as the page trailer.	an access method should always
 * initialize its pages with PageInit and then set its own opaque
 * fields.
 */

/*
 * PageIsValid --
 *		True iff page is valid.
 */
#define PageIsValid(page) PointerIsValid(page)


/*
 * location (byte offset) within a page.
 *
 * note that this is actually limited to 2^13 because we have limited
 * ItemIdData.lp_off and ItemIdData.lp_len to 13 bits (see itemid.h).
 *
 * uint16 is still valid, but the limit has been raised to 15 bits.
 * 06 Jan 98 - darrenk
 */
typedef uint16 LocationIndex;


/*
 * space management information generic to any page
 *
 *		od_pagesize		- size in bytes.
 *						  in reality, we need at least 64B to fit the
 *						  page header, opaque space and a minimal tuple;
 *						  on the high end, we can only support pages up
 *						  to 8KB because lp_off/lp_len are 13 bits.
 *
 *						  see above comment.  Now use 15 bits and pages
 *						  up to 32KB at your own risk.
 */
typedef struct OpaqueData
{
	uint16		od_pagesize;
} OpaqueData;

typedef OpaqueData *Opaque;


/*
 * disk page organization
 */
typedef struct PageHeaderData
{
	LocationIndex pd_lower;		/* offset to start of free space */
	LocationIndex pd_upper;		/* offset to end of free space */
	LocationIndex pd_special;	/* offset to start of special space */
	OpaqueData	pd_opaque;		/* AM-generic information */
	ItemIdData	pd_linp[1];		/* line pointers */
} PageHeaderData;

typedef PageHeaderData *PageHeader;

typedef enum
{
	ShufflePageManagerMode,
	OverwritePageManagerMode
} PageManagerMode;

/* ----------------
 *		misc support macros
 * ----------------
 */

/*
 * XXX this is wrong -- ignores padding/alignment, variable page size,
 * AM-specific opaque space at the end of the page (as in btrees), ...
 * however, it at least serves as an upper bound for heap pages.
 */
#define MAXTUPLEN		(BLCKSZ - sizeof (PageHeaderData))

/* ----------------------------------------------------------------
 *						page support macros
 * ----------------------------------------------------------------
 */
/*
 * PageIsValid -- This is defined in page.h.
 */

/*
 * PageIsUsed --
 *		True iff the page size is used.
 *
 * Note:
 *		Assumes page is valid.
 */
#define PageIsUsed(page) \
( \
	AssertMacro(PageIsValid(page)), \
	((bool) (((PageHeader) (page))->pd_lower != 0)) \
)

/*
 * PageIsEmpty --
 *		returns true iff no itemid has been allocated on the page
 */
#define PageIsEmpty(page) \
	(((PageHeader) (page))->pd_lower == \
	 (sizeof(PageHeaderData) - sizeof(ItemIdData)) ? true : false)

/*
 * PageIsNew --
 *		returns true iff page is not initialized (by PageInit)
 */

#define PageIsNew(page) (((PageHeader) (page))->pd_upper == 0)

/*
 * PageGetItemId --
 *		Returns an item identifier of a page.
 */
#define PageGetItemId(page, offsetNumber) \
	((ItemId) (&((PageHeader) (page))->pd_linp[(-1) + (offsetNumber)]))

/* ----------------
 *		macros to access opaque space
 * ----------------
 */

/*
 * PageSizeIsValid --
 *		True iff the page size is valid.
 *
 * XXX currently all page sizes are "valid" but we only actually
 *	   use BLCKSZ.
 *
 * 01/06/98 Now does something useful.	darrenk
 *
 */
#define PageSizeIsValid(pageSize) ((pageSize) == BLCKSZ)

/*
 * PageGetPageSize --
 *		Returns the page size of a page.
 *
 * this can only be called on a formatted page (unlike
 * BufferGetPageSize, which can be called on an unformatted page).
 * however, it can be called on a page for which there is no buffer.
 */
#define PageGetPageSize(page) \
	((Size) ((PageHeader) (page))->pd_opaque.od_pagesize)

/*
 * PageSetPageSize --
 *		Sets the page size of a page.
 */
#define PageSetPageSize(page, size) \
	((PageHeader) (page))->pd_opaque.od_pagesize = (size)

/* ----------------
 *		page special data macros
 * ----------------
 */
/*
 * PageGetSpecialSize --
 *		Returns size of special space on a page.
 *
 * Note:
 *		Assumes page is locked.
 */
#define PageGetSpecialSize(page) \
	((uint16) (PageGetPageSize(page) - ((PageHeader)(page))->pd_special))

/*
 * PageGetSpecialPointer --
 *		Returns pointer to special space on a page.
 *
 * Note:
 *		Assumes page is locked.
 */
#define PageGetSpecialPointer(page) \
( \
	AssertMacro(PageIsValid(page)), \
	(char *) ((char *) (page) + ((PageHeader) (page))->pd_special) \
)

/*
 * PageGetItem --
 *		Retrieves an item on the given page.
 *
 * Note:
 *		This does change the status of any of the resources passed.
 *		The semantics may change in the future.
 */
#define PageGetItem(page, itemId) \
( \
	AssertMacro(PageIsValid(page)), \
	AssertMacro((itemId)->lp_flags & LP_USED), \
	(Item)(((char *)(page)) + (itemId)->lp_off) \
)

/*
 * BufferGetPageSize --
 *		Returns the page size within a buffer.
 *
 * Notes:
 *		Assumes buffer is valid.
 *
 *		The buffer can be a raw disk block and need not contain a valid
 *		(formatted) disk page.
 */
/* XXX dig out of buffer descriptor */
#define BufferGetPageSize(buffer) \
( \
	AssertMacro(BufferIsValid(buffer)), \
	(Size)BLCKSZ \
)

/*
 * BufferGetPage --
 *		Returns the page associated with a buffer.
 */
#define BufferGetPage(buffer) ((Page)BufferGetBlock(buffer))

/*
 * PageGetMaxOffsetNumber --
 *		Returns the maximum offset number used by the given page.
 *
 *		NOTE: The offset is invalid if the page is non-empty.
 *		Test whether PageIsEmpty before calling this routine
 *		and/or using its return value.
 */
#define PageGetMaxOffsetNumber(page) \
( \
	(((PageHeader) (page))->pd_lower - \
		(sizeof(PageHeaderData) - sizeof(ItemIdData))) \
	/ sizeof(ItemIdData) \
)


/* ----------------------------------------------------------------
 *		extern declarations
 * ----------------------------------------------------------------
 */

extern void PageInit(Page page, Size pageSize, Size specialSize);
extern OffsetNumber
PageAddItem(Page page, Item item, Size size,
			OffsetNumber offsetNumber, ItemIdFlags flags);
extern Page PageGetTempPage(Page page, Size specialSize);
extern void PageRestoreTempPage(Page tempPage, Page oldPage);
extern void PageRepairFragmentation(Page page);
extern Size PageGetFreeSpace(Page page);
extern void PageManagerModeSet(PageManagerMode mode);
extern void PageIndexTupleDelete(Page page, OffsetNumber offset);


#endif							/* BUFPAGE_H */
