/*--------------------------------------------------------------------------
 * ginblock.h
 *	  details of structures stored in GIN index blocks
 *
 *	Copyright (c) 2006-2019, PostgreSQL Global Development Group
 *
 *	src/include/access/ginblock.h
 *--------------------------------------------------------------------------
 */
#ifndef GINBLOCK_H
#define GINBLOCK_H

#include "access/transam.h"
#include "storage/block.h"
#include "storage/itemptr.h"
#include "storage/off.h"

/*
 * Page opaque data in an inverted index page.
 *
 * Note: GIN does not include a page ID word as do the other index types.
 * This is OK because the opaque data is only 8 bytes and so can be reliably
 * distinguished by size.  Revisit this if the size ever increases.
 * Further note: as of 9.2, SP-GiST also uses 8-byte special space, as does
 * BRIN as of 9.5.  This is still OK, as long as GIN isn't using all of the
 * high-order bits in its flags word, because that way the flags word cannot
 * match the page IDs used by SP-GiST and BRIN.
 */
typedef struct GinPageOpaqueData
{
	BlockNumber rightlink;		/* next page if any */
	OffsetNumber maxoff;		/* number of PostingItems on GIN_DATA &
								 * ~GIN_LEAF page. On GIN_LIST page, number of
								 * heap tuples. */
	uint16		flags;			/* see bit definitions below */
} GinPageOpaqueData;

typedef GinPageOpaqueData *GinPageOpaque;

#define GIN_DATA		  (1 << 0)
#define GIN_LEAF		  (1 << 1)
#define GIN_DELETED		  (1 << 2)
#define GIN_META		  (1 << 3)
#define GIN_LIST		  (1 << 4)
#define GIN_LIST_FULLROW  (1 << 5)	/* makes sense only on GIN_LIST page */
#define GIN_INCOMPLETE_SPLIT (1 << 6)	/* page was split, but parent not
										 * updated */
#define GIN_COMPRESSED	  (1 << 7)

/* Page numbers of fixed-location pages */
#define GIN_METAPAGE_BLKNO	(0)
#define GIN_ROOT_BLKNO		(1)

typedef struct GinMetaPageData
{
	/*
	 * Pointers to head and tail of pending list, which consists of GIN_LIST
	 * pages.  These store fast-inserted entries that haven't yet been moved
	 * into the regular GIN structure.
	 */
	BlockNumber head;
	BlockNumber tail;

	/*
	 * Free space in bytes in the pending list's tail page.
	 */
	uint32		tailFreeSize;

	/*
	 * We store both number of pages and number of heap tuples that are in the
	 * pending list.
	 */
	BlockNumber nPendingPages;
	int64		nPendingHeapTuples;

	/*
	 * Statistics for planner use (accurate as of last VACUUM)
	 */
	BlockNumber nTotalPages;
	BlockNumber nEntryPages;
	BlockNumber nDataPages;
	int64		nEntries;

	/*
	 * GIN version number (ideally this should have been at the front, but too
	 * late now.  Don't move it!)
	 *
	 * Currently 2 (for indexes initialized in 9.4 or later)
	 *
	 * Version 1 (indexes initialized in version 9.1, 9.2 or 9.3), is
	 * compatible, but may contain uncompressed posting tree (leaf) pages and
	 * posting lists. They will be converted to compressed format when
	 * modified.
	 *
	 * Version 0 (indexes initialized in 9.0 or before) is compatible but may
	 * be missing null entries, including both null keys and placeholders.
	 * Reject full-index-scan attempts on such indexes.
	 */
	int32		ginVersion;
} GinMetaPageData;

#define GIN_CURRENT_VERSION		2

#define GinPageGetMeta(p) \
	((GinMetaPageData *) PageGetContents(p))

/*
 * Macros for accessing a GIN index page's opaque data
 */
#define GinPageGetOpaque(page) ( (GinPageOpaque) PageGetSpecialPointer(page) )

#define GinPageIsLeaf(page)    ( (GinPageGetOpaque(page)->flags & GIN_LEAF) != 0 )
#define GinPageSetLeaf(page)   ( GinPageGetOpaque(page)->flags |= GIN_LEAF )
#define GinPageSetNonLeaf(page)    ( GinPageGetOpaque(page)->flags &= ~GIN_LEAF )
#define GinPageIsData(page)    ( (GinPageGetOpaque(page)->flags & GIN_DATA) != 0 )
#define GinPageSetData(page)   ( GinPageGetOpaque(page)->flags |= GIN_DATA )
#define GinPageIsList(page)    ( (GinPageGetOpaque(page)->flags & GIN_LIST) != 0 )
#define GinPageSetList(page)   ( GinPageGetOpaque(page)->flags |= GIN_LIST )
#define GinPageHasFullRow(page)    ( (GinPageGetOpaque(page)->flags & GIN_LIST_FULLROW) != 0 )
#define GinPageSetFullRow(page)   ( GinPageGetOpaque(page)->flags |= GIN_LIST_FULLROW )
#define GinPageIsCompressed(page)	 ( (GinPageGetOpaque(page)->flags & GIN_COMPRESSED) != 0 )
#define GinPageSetCompressed(page)	 ( GinPageGetOpaque(page)->flags |= GIN_COMPRESSED )

#define GinPageIsDeleted(page) ( (GinPageGetOpaque(page)->flags & GIN_DELETED) != 0 )
#define GinPageSetDeleted(page)    ( GinPageGetOpaque(page)->flags |= GIN_DELETED)
#define GinPageSetNonDeleted(page) ( GinPageGetOpaque(page)->flags &= ~GIN_DELETED)
#define GinPageIsIncompleteSplit(page) ( (GinPageGetOpaque(page)->flags & GIN_INCOMPLETE_SPLIT) != 0 )

#define GinPageRightMost(page) ( GinPageGetOpaque(page)->rightlink == InvalidBlockNumber)

/*
 * We should reclaim deleted page only once every transaction started before
 * its deletion is over.
 */
#define GinPageGetDeleteXid(page) ( ((PageHeader) (page))->pd_prune_xid )
#define GinPageSetDeleteXid(page, xid) ( ((PageHeader) (page))->pd_prune_xid = xid)
#define GinPageIsRecyclable(page) ( PageIsNew(page) || (GinPageIsDeleted(page) \
	&& TransactionIdPrecedes(GinPageGetDeleteXid(page), RecentGlobalXmin)))

/*
 * We use our own ItemPointerGet(BlockNumber|OffsetNumber)
 * to avoid Asserts, since sometimes the ip_posid isn't "valid"
 */
#define GinItemPointerGetBlockNumber(pointer) \
	(ItemPointerGetBlockNumberNoCheck(pointer))

#define GinItemPointerGetOffsetNumber(pointer) \
	(ItemPointerGetOffsetNumberNoCheck(pointer))

#define GinItemPointerSetBlockNumber(pointer, blkno) \
	(ItemPointerSetBlockNumber((pointer), (blkno)))

#define GinItemPointerSetOffsetNumber(pointer, offnum) \
	(ItemPointerSetOffsetNumber((pointer), (offnum)))


/*
 * Special-case item pointer values needed by the GIN search logic.
 *	MIN: sorts less than any valid item pointer
 *	MAX: sorts greater than any valid item pointer
 *	LOSSY PAGE: indicates a whole heap page, sorts after normal item
 *				pointers for that page
 * Note that these are all distinguishable from an "invalid" item pointer
 * (which is InvalidBlockNumber/0) as well as from all normal item
 * pointers (which have item numbers in the range 1..MaxHeapTuplesPerPage).
 */
#define ItemPointerSetMin(p)  \
	ItemPointerSet((p), (BlockNumber)0, (OffsetNumber)0)
#define ItemPointerIsMin(p)  \
	(GinItemPointerGetOffsetNumber(p) == (OffsetNumber)0 && \
	 GinItemPointerGetBlockNumber(p) == (BlockNumber)0)
#define ItemPointerSetMax(p)  \
	ItemPointerSet((p), InvalidBlockNumber, (OffsetNumber)0xffff)
#define ItemPointerIsMax(p)  \
	(GinItemPointerGetOffsetNumber(p) == (OffsetNumber)0xffff && \
	 GinItemPointerGetBlockNumber(p) == InvalidBlockNumber)
#define ItemPointerSetLossyPage(p, b)  \
	ItemPointerSet((p), (b), (OffsetNumber)0xffff)
#define ItemPointerIsLossyPage(p)  \
	(GinItemPointerGetOffsetNumber(p) == (OffsetNumber)0xffff && \
	 GinItemPointerGetBlockNumber(p) != InvalidBlockNumber)

/*
 * Posting item in a non-leaf posting-tree page
 */
typedef struct
{
	/* We use BlockIdData not BlockNumber to avoid padding space wastage */
	BlockIdData child_blkno;
	ItemPointerData key;
} PostingItem;

#define PostingItemGetBlockNumber(pointer) \
	BlockIdGetBlockNumber(&(pointer)->child_blkno)

#define PostingItemSetBlockNumber(pointer, blockNumber) \
	BlockIdSet(&((pointer)->child_blkno), (blockNumber))

/*
 * Category codes to distinguish placeholder nulls from ordinary NULL keys.
 *
 * The first two code values were chosen to be compatible with the usual usage
 * of bool isNull flags.  However, casting between bool and GinNullCategory is
 * risky because of the possibility of different bit patterns and type sizes,
 * so it is no longer done.
 *
 * GIN_CAT_EMPTY_QUERY is never stored in the index; and notice that it is
 * chosen to sort before not after regular key values.
 */
typedef signed char GinNullCategory;

#define GIN_CAT_NORM_KEY		0	/* normal, non-null key value */
#define GIN_CAT_NULL_KEY		1	/* null key value */
#define GIN_CAT_EMPTY_ITEM		2	/* placeholder for zero-key item */
#define GIN_CAT_NULL_ITEM		3	/* placeholder for null item */
#define GIN_CAT_EMPTY_QUERY		(-1)	/* placeholder for full-scan query */

/*
 * Access macros for null category byte in entry tuples
 */
#define GinCategoryOffset(itup,ginstate) \
	(IndexInfoFindDataOffset((itup)->t_info) + \
	 ((ginstate)->oneCol ? 0 : sizeof(int16)))
#define GinGetNullCategory(itup,ginstate) \
	(*((GinNullCategory *) ((char*)(itup) + GinCategoryOffset(itup,ginstate))))
#define GinSetNullCategory(itup,ginstate,c) \
	(*((GinNullCategory *) ((char*)(itup) + GinCategoryOffset(itup,ginstate))) = (c))

/*
 * Access macros for leaf-page entry tuples (see discussion in README)
 */
#define GinGetNPosting(itup)	GinItemPointerGetOffsetNumber(&(itup)->t_tid)
#define GinSetNPosting(itup,n)	ItemPointerSetOffsetNumber(&(itup)->t_tid,n)
#define GIN_TREE_POSTING		((OffsetNumber)0xffff)
#define GinIsPostingTree(itup)	(GinGetNPosting(itup) == GIN_TREE_POSTING)
#define GinSetPostingTree(itup, blkno)	( GinSetNPosting((itup),GIN_TREE_POSTING), ItemPointerSetBlockNumber(&(itup)->t_tid, blkno) )
#define GinGetPostingTree(itup) GinItemPointerGetBlockNumber(&(itup)->t_tid)

#define GIN_ITUP_COMPRESSED		(1U << 31)
#define GinGetPostingOffset(itup)	(GinItemPointerGetBlockNumber(&(itup)->t_tid) & (~GIN_ITUP_COMPRESSED))
#define GinSetPostingOffset(itup,n) ItemPointerSetBlockNumber(&(itup)->t_tid,(n)|GIN_ITUP_COMPRESSED)
#define GinGetPosting(itup)			((Pointer) ((char*)(itup) + GinGetPostingOffset(itup)))
#define GinItupIsCompressed(itup)	((GinItemPointerGetBlockNumber(&(itup)->t_tid) & GIN_ITUP_COMPRESSED) != 0)

/*
 * Maximum size of an item on entry tree page. Make sure that we fit at least
 * three items on each page. (On regular B-tree indexes, we must fit at least
 * three items: two data items and the "high key". In GIN entry tree, we don't
 * currently store the high key explicitly, we just use the rightmost item on
 * the page, so it would actually be enough to fit two items.)
 */
#define GinMaxItemSize \
	Min(INDEX_SIZE_MASK, \
		MAXALIGN_DOWN(((BLCKSZ - \
						MAXALIGN(SizeOfPageHeaderData + 3 * sizeof(ItemIdData)) - \
						MAXALIGN(sizeof(GinPageOpaqueData))) / 3)))

/*
 * Access macros for non-leaf entry tuples
 */
#define GinGetDownlink(itup)	GinItemPointerGetBlockNumber(&(itup)->t_tid)
#define GinSetDownlink(itup,blkno)	ItemPointerSet(&(itup)->t_tid, blkno, InvalidOffsetNumber)


/*
 * Data (posting tree) pages
 *
 * Posting tree pages don't store regular tuples. Non-leaf pages contain
 * PostingItems, which are pairs of ItemPointers and child block numbers.
 * Leaf pages contain GinPostingLists and an uncompressed array of item
 * pointers.
 *
 * In a leaf page, the compressed posting lists are stored after the regular
 * page header, one after each other. Although we don't store regular tuples,
 * pd_lower is used to indicate the end of the posting lists. After that, free
 * space follows.  This layout is compatible with the "standard" heap and
 * index page layout described in bufpage.h, so that we can e.g set buffer_std
 * when writing WAL records.
 *
 * In the special space is the GinPageOpaque struct.
 */
#define GinDataLeafPageGetPostingList(page) \
	(GinPostingList *) ((PageGetContents(page) + MAXALIGN(sizeof(ItemPointerData))))
#define GinDataLeafPageGetPostingListSize(page) \
	(((PageHeader) page)->pd_lower - MAXALIGN(SizeOfPageHeaderData) - MAXALIGN(sizeof(ItemPointerData)))

#define GinDataLeafPageIsEmpty(page) \
	(GinPageIsCompressed(page) ? (GinDataLeafPageGetPostingListSize(page) == 0) : (GinPageGetOpaque(page)->maxoff < FirstOffsetNumber))

#define GinDataLeafPageGetFreeSpace(page) PageGetExactFreeSpace(page)

#define GinDataPageGetRightBound(page)	((ItemPointer) PageGetContents(page))
/*
 * Pointer to the data portion of a posting tree page. For internal pages,
 * that's the beginning of the array of PostingItems. For compressed leaf
 * pages, the first compressed posting list. For uncompressed (pre-9.4) leaf
 * pages, it's the beginning of the ItemPointer array.
 */
#define GinDataPageGetData(page)	\
	(PageGetContents(page) + MAXALIGN(sizeof(ItemPointerData)))
/* non-leaf pages contain PostingItems */
#define GinDataPageGetPostingItem(page, i)	\
	((PostingItem *) (GinDataPageGetData(page) + ((i)-1) * sizeof(PostingItem)))

/*
 * Note: there is no GinDataPageGetDataSize macro, because before version
 * 9.4, we didn't set pd_lower on data pages. There can be pages in the index
 * that were binary-upgraded from earlier versions and still have an invalid
 * pd_lower, so we cannot trust it in general. Compressed posting tree leaf
 * pages are new in 9.4, however, so we can trust them; see
 * GinDataLeafPageGetPostingListSize.
 */
#define GinDataPageSetDataSize(page, size) \
	{ \
		Assert(size <= GinDataPageMaxDataSize); \
		((PageHeader) page)->pd_lower = (size) + MAXALIGN(SizeOfPageHeaderData) + MAXALIGN(sizeof(ItemPointerData)); \
	}

#define GinNonLeafDataPageGetFreeSpace(page)	\
	(GinDataPageMaxDataSize - \
	 GinPageGetOpaque(page)->maxoff * sizeof(PostingItem))

#define GinDataPageMaxDataSize	\
	(BLCKSZ - MAXALIGN(SizeOfPageHeaderData) \
	 - MAXALIGN(sizeof(ItemPointerData)) \
	 - MAXALIGN(sizeof(GinPageOpaqueData)))

/*
 * List pages
 */
#define GinListPageSize  \
	( BLCKSZ - SizeOfPageHeaderData - MAXALIGN(sizeof(GinPageOpaqueData)) )

/*
 * A compressed posting list.
 *
 * Note: This requires 2-byte alignment.
 */
typedef struct
{
	ItemPointerData first;		/* first item in this posting list (unpacked) */
	uint16		nbytes;			/* number of bytes that follow */
	unsigned char bytes[FLEXIBLE_ARRAY_MEMBER]; /* varbyte encoded items */
} GinPostingList;

#define SizeOfGinPostingList(plist) (offsetof(GinPostingList, bytes) + SHORTALIGN((plist)->nbytes) )
#define GinNextPostingListSegment(cur) ((GinPostingList *) (((char *) (cur)) + SizeOfGinPostingList((cur))))

#endif							/* GINBLOCK_H */
