/*-------------------------------------------------------------------------
 *
 * gindatapage.c
 *	  routines for handling GIN posting tree pages.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gin/gindatapage.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin_private.h"
#include "access/ginxlog.h"
#include "access/xloginsert.h"
#include "lib/ilist.h"
#include "miscadmin.h"
#include "storage/predicate.h"
#include "utils/rel.h"

/*
 * Min, Max and Target size of posting lists stored on leaf pages, in bytes.
 *
 * The code can deal with any size, but random access is more efficient when
 * a number of smaller lists are stored, rather than one big list. If a
 * posting list would become larger than Max size as a result of insertions,
 * it is split into two. If a posting list would be smaller than minimum
 * size, it is merged with the next posting list.
 */
#define GinPostingListSegmentMaxSize 384
#define GinPostingListSegmentTargetSize 256
#define GinPostingListSegmentMinSize 128

/*
 * At least this many items fit in a GinPostingListSegmentMaxSize-bytes
 * long segment. This is used when estimating how much space is required
 * for N items, at minimum.
 */
#define MinTuplesPerSegment ((GinPostingListSegmentMaxSize - 2) / 6)

/*
 * A working struct for manipulating a posting tree leaf page.
 */
typedef struct
{
	dlist_head	segments;		/* a list of leafSegmentInfos */

	/*
	 * The following fields represent how the segments are split across pages,
	 * if a page split is required. Filled in by leafRepackItems.
	 */
	dlist_node *lastleft;		/* last segment on left page */
	int			lsize;			/* total size on left page */
	int			rsize;			/* total size on right page */

	bool		oldformat;		/* page is in pre-9.4 format on disk */

	/*
	 * If we need WAL data representing the reconstructed leaf page, it's
	 * stored here by computeLeafRecompressWALData.
	 */
	void	   *walinfo;		/* buffer start */
	int			walinfolen;		/* and length */
} disassembledLeaf;

typedef struct
{
	dlist_node	node;			/* linked list pointers */

	/*-------------
	 * 'action' indicates the status of this in-memory segment, compared to
	 * what's on disk. It is one of the GIN_SEGMENT_* action codes:
	 *
	 * UNMODIFIED	no changes
	 * DELETE		the segment is to be removed. 'seg' and 'items' are
	 *				ignored
	 * INSERT		this is a completely new segment
	 * REPLACE		this replaces an existing segment with new content
	 * ADDITEMS		like REPLACE, but no items have been removed, and we track
	 *				in detail what items have been added to this segment, in
	 *				'modifieditems'
	 *-------------
	 */
	char		action;

	ItemPointerData *modifieditems;
	uint16		nmodifieditems;

	/*
	 * The following fields represent the items in this segment. If 'items' is
	 * not NULL, it contains a palloc'd array of the items in this segment. If
	 * 'seg' is not NULL, it contains the items in an already-compressed
	 * format. It can point to an on-disk page (!modified), or a palloc'd
	 * segment in memory. If both are set, they must represent the same items.
	 */
	GinPostingList *seg;
	ItemPointer items;
	int			nitems;			/* # of items in 'items', if items != NULL */
} leafSegmentInfo;

static ItemPointer dataLeafPageGetUncompressed(Page page, int *nitems);
static void dataSplitPageInternal(GinBtree btree, Buffer origbuf,
								  GinBtreeStack *stack,
								  void *insertdata, BlockNumber updateblkno,
								  Page *newlpage, Page *newrpage);

static disassembledLeaf *disassembleLeaf(Page page);
static bool leafRepackItems(disassembledLeaf *leaf, ItemPointer remaining);
static bool addItemsToLeaf(disassembledLeaf *leaf, ItemPointer newItems,
						   int nNewItems);

static void computeLeafRecompressWALData(disassembledLeaf *leaf);
static void dataPlaceToPageLeafRecompress(Buffer buf, disassembledLeaf *leaf);
static void dataPlaceToPageLeafSplit(disassembledLeaf *leaf,
									 ItemPointerData lbound, ItemPointerData rbound,
									 Page lpage, Page rpage);

/*
 * Read TIDs from leaf data page to single uncompressed array. The TIDs are
 * returned in ascending order.
 *
 * advancePast is a hint, indicating that the caller is only interested in
 * TIDs > advancePast. To return all items, use ItemPointerSetMin.
 *
 * Note: This function can still return items smaller than advancePast that
 * are in the same posting list as the items of interest, so the caller must
 * still check all the returned items. But passing it allows this function to
 * skip whole posting lists.
 */
ItemPointer
GinDataLeafPageGetItems(Page page, int *nitems, ItemPointerData advancePast)
{
	ItemPointer result;

	if (GinPageIsCompressed(page))
	{
		GinPostingList *seg = GinDataLeafPageGetPostingList(page);
		Size		len = GinDataLeafPageGetPostingListSize(page);
		Pointer		endptr = ((Pointer) seg) + len;
		GinPostingList *next;

		/* Skip to the segment containing advancePast+1 */
		if (ItemPointerIsValid(&advancePast))
		{
			next = GinNextPostingListSegment(seg);
			while ((Pointer) next < endptr &&
				   ginCompareItemPointers(&next->first, &advancePast) <= 0)
			{
				seg = next;
				next = GinNextPostingListSegment(seg);
			}
			len = endptr - (Pointer) seg;
		}

		if (len > 0)
			result = ginPostingListDecodeAllSegments(seg, len, nitems);
		else
		{
			result = NULL;
			*nitems = 0;
		}
	}
	else
	{
		ItemPointer tmp = dataLeafPageGetUncompressed(page, nitems);

		result = palloc((*nitems) * sizeof(ItemPointerData));
		memcpy(result, tmp, (*nitems) * sizeof(ItemPointerData));
	}

	return result;
}

/*
 * Places all TIDs from leaf data page to bitmap.
 */
int
GinDataLeafPageGetItemsToTbm(Page page, TIDBitmap *tbm)
{
	ItemPointer uncompressed;
	int			nitems;

	if (GinPageIsCompressed(page))
	{
		GinPostingList *segment = GinDataLeafPageGetPostingList(page);
		Size		len = GinDataLeafPageGetPostingListSize(page);

		nitems = ginPostingListDecodeAllSegmentsToTbm(segment, len, tbm);
	}
	else
	{
		uncompressed = dataLeafPageGetUncompressed(page, &nitems);

		if (nitems > 0)
			tbm_add_tuples(tbm, uncompressed, nitems, false);
	}

	return nitems;
}

/*
 * Get pointer to the uncompressed array of items on a pre-9.4 format
 * uncompressed leaf page. The number of items in the array is returned in
 * *nitems.
 */
static ItemPointer
dataLeafPageGetUncompressed(Page page, int *nitems)
{
	ItemPointer items;

	Assert(!GinPageIsCompressed(page));

	/*
	 * In the old pre-9.4 page format, the whole page content is used for
	 * uncompressed items, and the number of items is stored in 'maxoff'
	 */
	items = (ItemPointer) GinDataPageGetData(page);
	*nitems = GinPageGetOpaque(page)->maxoff;

	return items;
}

/*
 * Check if we should follow the right link to find the item we're searching
 * for.
 *
 * Compares inserting item pointer with the right bound of the current page.
 */
static bool
dataIsMoveRight(GinBtree btree, Page page)
{
	ItemPointer iptr = GinDataPageGetRightBound(page);

	if (GinPageRightMost(page))
		return false;

	if (GinPageIsDeleted(page))
		return true;

	return (ginCompareItemPointers(&btree->itemptr, iptr) > 0);
}

/*
 * Find correct PostingItem in non-leaf page. It is assumed that this is
 * the correct page, and the searched value SHOULD be on the page.
 */
static BlockNumber
dataLocateItem(GinBtree btree, GinBtreeStack *stack)
{
	OffsetNumber low,
				high,
				maxoff;
	PostingItem *pitem = NULL;
	int			result;
	Page		page = BufferGetPage(stack->buffer);

	Assert(!GinPageIsLeaf(page));
	Assert(GinPageIsData(page));

	if (btree->fullScan)
	{
		stack->off = FirstOffsetNumber;
		stack->predictNumber *= GinPageGetOpaque(page)->maxoff;
		return btree->getLeftMostChild(btree, page);
	}

	low = FirstOffsetNumber;
	maxoff = high = GinPageGetOpaque(page)->maxoff;
	Assert(high >= low);

	high++;

	while (high > low)
	{
		OffsetNumber mid = low + ((high - low) / 2);

		pitem = GinDataPageGetPostingItem(page, mid);

		if (mid == maxoff)
		{
			/*
			 * Right infinity, page already correctly chosen with a help of
			 * dataIsMoveRight
			 */
			result = -1;
		}
		else
		{
			pitem = GinDataPageGetPostingItem(page, mid);
			result = ginCompareItemPointers(&btree->itemptr, &(pitem->key));
		}

		if (result == 0)
		{
			stack->off = mid;
			return PostingItemGetBlockNumber(pitem);
		}
		else if (result > 0)
			low = mid + 1;
		else
			high = mid;
	}

	Assert(high >= FirstOffsetNumber && high <= maxoff);

	stack->off = high;
	pitem = GinDataPageGetPostingItem(page, high);
	return PostingItemGetBlockNumber(pitem);
}

/*
 * Find link to blkno on non-leaf page, returns offset of PostingItem
 */
static OffsetNumber
dataFindChildPtr(GinBtree btree, Page page, BlockNumber blkno, OffsetNumber storedOff)
{
	OffsetNumber i,
				maxoff = GinPageGetOpaque(page)->maxoff;
	PostingItem *pitem;

	Assert(!GinPageIsLeaf(page));
	Assert(GinPageIsData(page));

	/* if page isn't changed, we return storedOff */
	if (storedOff >= FirstOffsetNumber && storedOff <= maxoff)
	{
		pitem = GinDataPageGetPostingItem(page, storedOff);
		if (PostingItemGetBlockNumber(pitem) == blkno)
			return storedOff;

		/*
		 * we hope, that needed pointer goes to right. It's true if there
		 * wasn't a deletion
		 */
		for (i = storedOff + 1; i <= maxoff; i++)
		{
			pitem = GinDataPageGetPostingItem(page, i);
			if (PostingItemGetBlockNumber(pitem) == blkno)
				return i;
		}

		maxoff = storedOff - 1;
	}

	/* last chance */
	for (i = FirstOffsetNumber; i <= maxoff; i++)
	{
		pitem = GinDataPageGetPostingItem(page, i);
		if (PostingItemGetBlockNumber(pitem) == blkno)
			return i;
	}

	return InvalidOffsetNumber;
}

/*
 * Return blkno of leftmost child
 */
static BlockNumber
dataGetLeftMostPage(GinBtree btree, Page page)
{
	PostingItem *pitem;

	Assert(!GinPageIsLeaf(page));
	Assert(GinPageIsData(page));
	Assert(GinPageGetOpaque(page)->maxoff >= FirstOffsetNumber);

	pitem = GinDataPageGetPostingItem(page, FirstOffsetNumber);
	return PostingItemGetBlockNumber(pitem);
}

/*
 * Add PostingItem to a non-leaf page.
 */
void
GinDataPageAddPostingItem(Page page, PostingItem *data, OffsetNumber offset)
{
	OffsetNumber maxoff = GinPageGetOpaque(page)->maxoff;
	char	   *ptr;

	Assert(PostingItemGetBlockNumber(data) != InvalidBlockNumber);
	Assert(!GinPageIsLeaf(page));

	if (offset == InvalidOffsetNumber)
	{
		ptr = (char *) GinDataPageGetPostingItem(page, maxoff + 1);
	}
	else
	{
		ptr = (char *) GinDataPageGetPostingItem(page, offset);
		if (offset != maxoff + 1)
			memmove(ptr + sizeof(PostingItem),
					ptr,
					(maxoff - offset + 1) * sizeof(PostingItem));
	}
	memcpy(ptr, data, sizeof(PostingItem));

	maxoff++;
	GinPageGetOpaque(page)->maxoff = maxoff;

	/*
	 * Also set pd_lower to the end of the posting items, to follow the
	 * "standard" page layout, so that we can squeeze out the unused space
	 * from full-page images.
	 */
	GinDataPageSetDataSize(page, maxoff * sizeof(PostingItem));
}

/*
 * Delete posting item from non-leaf page
 */
void
GinPageDeletePostingItem(Page page, OffsetNumber offset)
{
	OffsetNumber maxoff = GinPageGetOpaque(page)->maxoff;

	Assert(!GinPageIsLeaf(page));
	Assert(offset >= FirstOffsetNumber && offset <= maxoff);

	if (offset != maxoff)
		memmove(GinDataPageGetPostingItem(page, offset),
				GinDataPageGetPostingItem(page, offset + 1),
				sizeof(PostingItem) * (maxoff - offset));

	maxoff--;
	GinPageGetOpaque(page)->maxoff = maxoff;

	GinDataPageSetDataSize(page, maxoff * sizeof(PostingItem));
}

/*
 * Prepare to insert data on a leaf data page.
 *
 * If it will fit, return GPTP_INSERT after doing whatever setup is needed
 * before we enter the insertion critical section.  *ptp_workspace can be
 * set to pass information along to the execPlaceToPage function.
 *
 * If it won't fit, perform a page split and return two temporary page
 * images into *newlpage and *newrpage, with result GPTP_SPLIT.
 *
 * In neither case should the given page buffer be modified here.
 */
static GinPlaceToPageRC
dataBeginPlaceToPageLeaf(GinBtree btree, Buffer buf, GinBtreeStack *stack,
						 void *insertdata,
						 void **ptp_workspace,
						 Page *newlpage, Page *newrpage)
{
	GinBtreeDataLeafInsertData *items = insertdata;
	ItemPointer newItems = &items->items[items->curitem];
	int			maxitems = items->nitem - items->curitem;
	Page		page = BufferGetPage(buf);
	int			i;
	ItemPointerData rbound;
	ItemPointerData lbound;
	bool		needsplit;
	bool		append;
	int			segsize;
	Size		freespace;
	disassembledLeaf *leaf;
	leafSegmentInfo *lastleftinfo;
	ItemPointerData maxOldItem;
	ItemPointerData remaining;

	rbound = *GinDataPageGetRightBound(page);

	/*
	 * Count how many of the new items belong to this page.
	 */
	if (!GinPageRightMost(page))
	{
		for (i = 0; i < maxitems; i++)
		{
			if (ginCompareItemPointers(&newItems[i], &rbound) > 0)
			{
				/*
				 * This needs to go to some other location in the tree. (The
				 * caller should've chosen the insert location so that at
				 * least the first item goes here.)
				 */
				Assert(i > 0);
				break;
			}
		}
		maxitems = i;
	}

	/* Disassemble the data on the page */
	leaf = disassembleLeaf(page);

	/*
	 * Are we appending to the end of the page? IOW, are all the new items
	 * larger than any of the existing items.
	 */
	if (!dlist_is_empty(&leaf->segments))
	{
		lastleftinfo = dlist_container(leafSegmentInfo, node,
									   dlist_tail_node(&leaf->segments));
		if (!lastleftinfo->items)
			lastleftinfo->items = ginPostingListDecode(lastleftinfo->seg,
													   &lastleftinfo->nitems);
		maxOldItem = lastleftinfo->items[lastleftinfo->nitems - 1];
		if (ginCompareItemPointers(&newItems[0], &maxOldItem) >= 0)
			append = true;
		else
			append = false;
	}
	else
	{
		ItemPointerSetMin(&maxOldItem);
		append = true;
	}

	/*
	 * If we're appending to the end of the page, we will append as many items
	 * as we can fit (after splitting), and stop when the pages becomes full.
	 * Otherwise we have to limit the number of new items to insert, because
	 * once we start packing we can't just stop when we run out of space,
	 * because we must make sure that all the old items still fit.
	 */
	if (GinPageIsCompressed(page))
		freespace = GinDataLeafPageGetFreeSpace(page);
	else
		freespace = 0;
	if (append)
	{
		/*
		 * Even when appending, trying to append more items than will fit is
		 * not completely free, because we will merge the new items and old
		 * items into an array below. In the best case, every new item fits in
		 * a single byte, and we can use all the free space on the old page as
		 * well as the new page. For simplicity, ignore segment overhead etc.
		 */
		maxitems = Min(maxitems, freespace + GinDataPageMaxDataSize);
	}
	else
	{
		/*
		 * Calculate a conservative estimate of how many new items we can fit
		 * on the two pages after splitting.
		 *
		 * We can use any remaining free space on the old page to store full
		 * segments, as well as the new page. Each full-sized segment can hold
		 * at least MinTuplesPerSegment items
		 */
		int			nnewsegments;

		nnewsegments = freespace / GinPostingListSegmentMaxSize;
		nnewsegments += GinDataPageMaxDataSize / GinPostingListSegmentMaxSize;
		maxitems = Min(maxitems, nnewsegments * MinTuplesPerSegment);
	}

	/* Add the new items to the segment list */
	if (!addItemsToLeaf(leaf, newItems, maxitems))
	{
		/* all items were duplicates, we have nothing to do */
		items->curitem += maxitems;

		return GPTP_NO_WORK;
	}

	/*
	 * Pack the items back to compressed segments, ready for writing to disk.
	 */
	needsplit = leafRepackItems(leaf, &remaining);

	/*
	 * Did all the new items fit?
	 *
	 * If we're appending, it's OK if they didn't. But as a sanity check,
	 * verify that all the old items fit.
	 */
	if (ItemPointerIsValid(&remaining))
	{
		if (!append || ItemPointerCompare(&maxOldItem, &remaining) >= 0)
			elog(ERROR, "could not split GIN page; all old items didn't fit");

		/* Count how many of the new items did fit. */
		for (i = 0; i < maxitems; i++)
		{
			if (ginCompareItemPointers(&newItems[i], &remaining) >= 0)
				break;
		}
		if (i == 0)
			elog(ERROR, "could not split GIN page; no new items fit");
		maxitems = i;
	}

	if (!needsplit)
	{
		/*
		 * Great, all the items fit on a single page.  If needed, prepare data
		 * for a WAL record describing the changes we'll make.
		 */
		if (RelationNeedsWAL(btree->index) && !btree->isBuild)
			computeLeafRecompressWALData(leaf);

		/*
		 * We're ready to enter the critical section, but
		 * dataExecPlaceToPageLeaf will need access to the "leaf" data.
		 */
		*ptp_workspace = leaf;

		if (append)
			elog(DEBUG2, "appended %d new items to block %u; %d bytes (%d to go)",
				 maxitems, BufferGetBlockNumber(buf), (int) leaf->lsize,
				 items->nitem - items->curitem - maxitems);
		else
			elog(DEBUG2, "inserted %d new items to block %u; %d bytes (%d to go)",
				 maxitems, BufferGetBlockNumber(buf), (int) leaf->lsize,
				 items->nitem - items->curitem - maxitems);
	}
	else
	{
		/*
		 * Have to split.
		 *
		 * leafRepackItems already divided the segments between the left and
		 * the right page. It filled the left page as full as possible, and
		 * put the rest to the right page. When building a new index, that's
		 * good, because the table is scanned from beginning to end and there
		 * won't be any more insertions to the left page during the build.
		 * This packs the index as tight as possible. But otherwise, split
		 * 50/50, by moving segments from the left page to the right page
		 * until they're balanced.
		 *
		 * As a further heuristic, when appending items to the end of the
		 * page, try to make the left page 75% full, on the assumption that
		 * subsequent insertions will probably also go to the end. This packs
		 * the index somewhat tighter when appending to a table, which is very
		 * common.
		 */
		if (!btree->isBuild)
		{
			while (dlist_has_prev(&leaf->segments, leaf->lastleft))
			{
				lastleftinfo = dlist_container(leafSegmentInfo, node, leaf->lastleft);

				/* ignore deleted segments */
				if (lastleftinfo->action != GIN_SEGMENT_DELETE)
				{
					segsize = SizeOfGinPostingList(lastleftinfo->seg);

					/*
					 * Note that we check that the right page doesn't become
					 * more full than the left page even when appending. It's
					 * possible that we added enough items to make both pages
					 * more than 75% full.
					 */
					if ((leaf->lsize - segsize) - (leaf->rsize + segsize) < 0)
						break;
					if (append)
					{
						if ((leaf->lsize - segsize) < (BLCKSZ * 3) / 4)
							break;
					}

					leaf->lsize -= segsize;
					leaf->rsize += segsize;
				}
				leaf->lastleft = dlist_prev_node(&leaf->segments, leaf->lastleft);
			}
		}
		Assert(leaf->lsize <= GinDataPageMaxDataSize);
		Assert(leaf->rsize <= GinDataPageMaxDataSize);

		/*
		 * Fetch the max item in the left page's last segment; it becomes the
		 * right bound of the page.
		 */
		lastleftinfo = dlist_container(leafSegmentInfo, node, leaf->lastleft);
		if (!lastleftinfo->items)
			lastleftinfo->items = ginPostingListDecode(lastleftinfo->seg,
													   &lastleftinfo->nitems);
		lbound = lastleftinfo->items[lastleftinfo->nitems - 1];

		/*
		 * Now allocate a couple of temporary page images, and fill them.
		 */
		*newlpage = palloc(BLCKSZ);
		*newrpage = palloc(BLCKSZ);

		dataPlaceToPageLeafSplit(leaf, lbound, rbound,
								 *newlpage, *newrpage);

		Assert(GinPageRightMost(page) ||
			   ginCompareItemPointers(GinDataPageGetRightBound(*newlpage),
									  GinDataPageGetRightBound(*newrpage)) < 0);

		if (append)
			elog(DEBUG2, "appended %d items to block %u; split %d/%d (%d to go)",
				 maxitems, BufferGetBlockNumber(buf), (int) leaf->lsize, (int) leaf->rsize,
				 items->nitem - items->curitem - maxitems);
		else
			elog(DEBUG2, "inserted %d items to block %u; split %d/%d (%d to go)",
				 maxitems, BufferGetBlockNumber(buf), (int) leaf->lsize, (int) leaf->rsize,
				 items->nitem - items->curitem - maxitems);
	}

	items->curitem += maxitems;

	return needsplit ? GPTP_SPLIT : GPTP_INSERT;
}

/*
 * Perform data insertion after beginPlaceToPage has decided it will fit.
 *
 * This is invoked within a critical section, and XLOG record creation (if
 * needed) is already started.  The target buffer is registered in slot 0.
 */
static void
dataExecPlaceToPageLeaf(GinBtree btree, Buffer buf, GinBtreeStack *stack,
						void *insertdata, void *ptp_workspace)
{
	disassembledLeaf *leaf = (disassembledLeaf *) ptp_workspace;

	/* Apply changes to page */
	dataPlaceToPageLeafRecompress(buf, leaf);

	MarkBufferDirty(buf);

	/* If needed, register WAL data built by computeLeafRecompressWALData */
	if (RelationNeedsWAL(btree->index) && !btree->isBuild)
	{
		XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
		XLogRegisterBufData(0, leaf->walinfo, leaf->walinfolen);
	}
}

/*
 * Vacuum a posting tree leaf page.
 */
void
ginVacuumPostingTreeLeaf(Relation indexrel, Buffer buffer, GinVacuumState *gvs)
{
	Page		page = BufferGetPage(buffer);
	disassembledLeaf *leaf;
	bool		removedsomething = false;
	dlist_iter	iter;

	leaf = disassembleLeaf(page);

	/* Vacuum each segment. */
	dlist_foreach(iter, &leaf->segments)
	{
		leafSegmentInfo *seginfo = dlist_container(leafSegmentInfo, node, iter.cur);
		int			oldsegsize;
		ItemPointer cleaned;
		int			ncleaned;

		if (!seginfo->items)
			seginfo->items = ginPostingListDecode(seginfo->seg,
												  &seginfo->nitems);
		if (seginfo->seg)
			oldsegsize = SizeOfGinPostingList(seginfo->seg);
		else
			oldsegsize = GinDataPageMaxDataSize;

		cleaned = ginVacuumItemPointers(gvs,
										seginfo->items,
										seginfo->nitems,
										&ncleaned);
		pfree(seginfo->items);
		seginfo->items = NULL;
		seginfo->nitems = 0;
		if (cleaned)
		{
			if (ncleaned > 0)
			{
				int			npacked;

				seginfo->seg = ginCompressPostingList(cleaned,
													  ncleaned,
													  oldsegsize,
													  &npacked);
				/* Removing an item never increases the size of the segment */
				if (npacked != ncleaned)
					elog(ERROR, "could not fit vacuumed posting list");
				seginfo->action = GIN_SEGMENT_REPLACE;
			}
			else
			{
				seginfo->seg = NULL;
				seginfo->items = NULL;
				seginfo->action = GIN_SEGMENT_DELETE;
			}
			seginfo->nitems = ncleaned;

			removedsomething = true;
		}
	}

	/*
	 * If we removed any items, reconstruct the page from the pieces.
	 *
	 * We don't try to re-encode the segments here, even though some of them
	 * might be really small now that we've removed some items from them. It
	 * seems like a waste of effort, as there isn't really any benefit from
	 * larger segments per se; larger segments only help to pack more items in
	 * the same space. We might as well delay doing that until the next
	 * insertion, which will need to re-encode at least part of the page
	 * anyway.
	 *
	 * Also note if the page was in uncompressed, pre-9.4 format before, it is
	 * now represented as one huge segment that contains all the items. It
	 * might make sense to split that, to speed up random access, but we don't
	 * bother. You'll have to REINDEX anyway if you want the full gain of the
	 * new tighter index format.
	 */
	if (removedsomething)
	{
		bool		modified;

		/*
		 * Make sure we have a palloc'd copy of all segments, after the first
		 * segment that is modified. (dataPlaceToPageLeafRecompress requires
		 * this).
		 */
		modified = false;
		dlist_foreach(iter, &leaf->segments)
		{
			leafSegmentInfo *seginfo = dlist_container(leafSegmentInfo, node,
													   iter.cur);

			if (seginfo->action != GIN_SEGMENT_UNMODIFIED)
				modified = true;
			if (modified && seginfo->action != GIN_SEGMENT_DELETE)
			{
				int			segsize = SizeOfGinPostingList(seginfo->seg);
				GinPostingList *tmp = (GinPostingList *) palloc(segsize);

				memcpy(tmp, seginfo->seg, segsize);
				seginfo->seg = tmp;
			}
		}

		if (RelationNeedsWAL(indexrel))
			computeLeafRecompressWALData(leaf);

		/* Apply changes to page */
		START_CRIT_SECTION();

		dataPlaceToPageLeafRecompress(buffer, leaf);

		MarkBufferDirty(buffer);

		if (RelationNeedsWAL(indexrel))
		{
			XLogRecPtr	recptr;

			XLogBeginInsert();
			XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);
			XLogRegisterBufData(0, leaf->walinfo, leaf->walinfolen);
			recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_VACUUM_DATA_LEAF_PAGE);
			PageSetLSN(page, recptr);
		}

		END_CRIT_SECTION();
	}
}

/*
 * Construct a ginxlogRecompressDataLeaf record representing the changes
 * in *leaf.  (Because this requires a palloc, we have to do it before
 * we enter the critical section that actually updates the page.)
 */
static void
computeLeafRecompressWALData(disassembledLeaf *leaf)
{
	int			nmodified = 0;
	char	   *walbufbegin;
	char	   *walbufend;
	dlist_iter	iter;
	int			segno;
	ginxlogRecompressDataLeaf *recompress_xlog;

	/* Count the modified segments */
	dlist_foreach(iter, &leaf->segments)
	{
		leafSegmentInfo *seginfo = dlist_container(leafSegmentInfo, node,
												   iter.cur);

		if (seginfo->action != GIN_SEGMENT_UNMODIFIED)
			nmodified++;
	}

	walbufbegin =
		palloc(sizeof(ginxlogRecompressDataLeaf) +
			   BLCKSZ +			/* max size needed to hold the segment data */
			   nmodified * 2	/* (segno + action) per action */
		);
	walbufend = walbufbegin;

	recompress_xlog = (ginxlogRecompressDataLeaf *) walbufend;
	walbufend += sizeof(ginxlogRecompressDataLeaf);

	recompress_xlog->nactions = nmodified;

	segno = 0;
	dlist_foreach(iter, &leaf->segments)
	{
		leafSegmentInfo *seginfo = dlist_container(leafSegmentInfo, node,
												   iter.cur);
		int			segsize = 0;
		int			datalen;
		uint8		action = seginfo->action;

		if (action == GIN_SEGMENT_UNMODIFIED)
		{
			segno++;
			continue;
		}

		if (action != GIN_SEGMENT_DELETE)
			segsize = SizeOfGinPostingList(seginfo->seg);

		/*
		 * If storing the uncompressed list of added item pointers would take
		 * more space than storing the compressed segment as is, do that
		 * instead.
		 */
		if (action == GIN_SEGMENT_ADDITEMS &&
			seginfo->nmodifieditems * sizeof(ItemPointerData) > segsize)
		{
			action = GIN_SEGMENT_REPLACE;
		}

		*((uint8 *) (walbufend++)) = segno;
		*(walbufend++) = action;

		switch (action)
		{
			case GIN_SEGMENT_DELETE:
				datalen = 0;
				break;

			case GIN_SEGMENT_ADDITEMS:
				datalen = seginfo->nmodifieditems * sizeof(ItemPointerData);
				memcpy(walbufend, &seginfo->nmodifieditems, sizeof(uint16));
				memcpy(walbufend + sizeof(uint16), seginfo->modifieditems, datalen);
				datalen += sizeof(uint16);
				break;

			case GIN_SEGMENT_INSERT:
			case GIN_SEGMENT_REPLACE:
				datalen = SHORTALIGN(segsize);
				memcpy(walbufend, seginfo->seg, segsize);
				break;

			default:
				elog(ERROR, "unexpected GIN leaf action %d", action);
		}
		walbufend += datalen;

		if (action != GIN_SEGMENT_INSERT)
			segno++;
	}

	/* Pass back the constructed info via *leaf */
	leaf->walinfo = walbufbegin;
	leaf->walinfolen = walbufend - walbufbegin;
}

/*
 * Assemble a disassembled posting tree leaf page back to a buffer.
 *
 * This just updates the target buffer; WAL stuff is caller's responsibility.
 *
 * NOTE: The segment pointers must not point directly to the same buffer,
 * except for segments that have not been modified and whose preceding
 * segments have not been modified either.
 */
static void
dataPlaceToPageLeafRecompress(Buffer buf, disassembledLeaf *leaf)
{
	Page		page = BufferGetPage(buf);
	char	   *ptr;
	int			newsize;
	bool		modified = false;
	dlist_iter	iter;
	int			segsize;

	/*
	 * If the page was in pre-9.4 format before, convert the header, and force
	 * all segments to be copied to the page whether they were modified or
	 * not.
	 */
	if (!GinPageIsCompressed(page))
	{
		Assert(leaf->oldformat);
		GinPageSetCompressed(page);
		GinPageGetOpaque(page)->maxoff = InvalidOffsetNumber;
		modified = true;
	}

	ptr = (char *) GinDataLeafPageGetPostingList(page);
	newsize = 0;
	dlist_foreach(iter, &leaf->segments)
	{
		leafSegmentInfo *seginfo = dlist_container(leafSegmentInfo, node, iter.cur);

		if (seginfo->action != GIN_SEGMENT_UNMODIFIED)
			modified = true;

		if (seginfo->action != GIN_SEGMENT_DELETE)
		{
			segsize = SizeOfGinPostingList(seginfo->seg);

			if (modified)
				memcpy(ptr, seginfo->seg, segsize);

			ptr += segsize;
			newsize += segsize;
		}
	}

	Assert(newsize <= GinDataPageMaxDataSize);
	GinDataPageSetDataSize(page, newsize);
}

/*
 * Like dataPlaceToPageLeafRecompress, but writes the disassembled leaf
 * segments to two pages instead of one.
 *
 * This is different from the non-split cases in that this does not modify
 * the original page directly, but writes to temporary in-memory copies of
 * the new left and right pages.
 */
static void
dataPlaceToPageLeafSplit(disassembledLeaf *leaf,
						 ItemPointerData lbound, ItemPointerData rbound,
						 Page lpage, Page rpage)
{
	char	   *ptr;
	int			segsize;
	int			lsize;
	int			rsize;
	dlist_node *node;
	dlist_node *firstright;
	leafSegmentInfo *seginfo;

	/* Initialize temporary pages to hold the new left and right pages */
	GinInitPage(lpage, GIN_DATA | GIN_LEAF | GIN_COMPRESSED, BLCKSZ);
	GinInitPage(rpage, GIN_DATA | GIN_LEAF | GIN_COMPRESSED, BLCKSZ);

	/*
	 * Copy the segments that go to the left page.
	 *
	 * XXX: We should skip copying the unmodified part of the left page, like
	 * we do when recompressing.
	 */
	lsize = 0;
	ptr = (char *) GinDataLeafPageGetPostingList(lpage);
	firstright = dlist_next_node(&leaf->segments, leaf->lastleft);
	for (node = dlist_head_node(&leaf->segments);
		 node != firstright;
		 node = dlist_next_node(&leaf->segments, node))
	{
		seginfo = dlist_container(leafSegmentInfo, node, node);

		if (seginfo->action != GIN_SEGMENT_DELETE)
		{
			segsize = SizeOfGinPostingList(seginfo->seg);
			memcpy(ptr, seginfo->seg, segsize);
			ptr += segsize;
			lsize += segsize;
		}
	}
	Assert(lsize == leaf->lsize);
	GinDataPageSetDataSize(lpage, lsize);
	*GinDataPageGetRightBound(lpage) = lbound;

	/* Copy the segments that go to the right page */
	ptr = (char *) GinDataLeafPageGetPostingList(rpage);
	rsize = 0;
	for (node = firstright;
		 ;
		 node = dlist_next_node(&leaf->segments, node))
	{
		seginfo = dlist_container(leafSegmentInfo, node, node);

		if (seginfo->action != GIN_SEGMENT_DELETE)
		{
			segsize = SizeOfGinPostingList(seginfo->seg);
			memcpy(ptr, seginfo->seg, segsize);
			ptr += segsize;
			rsize += segsize;
		}

		if (!dlist_has_next(&leaf->segments, node))
			break;
	}
	Assert(rsize == leaf->rsize);
	GinDataPageSetDataSize(rpage, rsize);
	*GinDataPageGetRightBound(rpage) = rbound;
}

/*
 * Prepare to insert data on an internal data page.
 *
 * If it will fit, return GPTP_INSERT after doing whatever setup is needed
 * before we enter the insertion critical section.  *ptp_workspace can be
 * set to pass information along to the execPlaceToPage function.
 *
 * If it won't fit, perform a page split and return two temporary page
 * images into *newlpage and *newrpage, with result GPTP_SPLIT.
 *
 * In neither case should the given page buffer be modified here.
 *
 * Note: on insertion to an internal node, in addition to inserting the given
 * item, the downlink of the existing item at stack->off will be updated to
 * point to updateblkno.
 */
static GinPlaceToPageRC
dataBeginPlaceToPageInternal(GinBtree btree, Buffer buf, GinBtreeStack *stack,
							 void *insertdata, BlockNumber updateblkno,
							 void **ptp_workspace,
							 Page *newlpage, Page *newrpage)
{
	Page		page = BufferGetPage(buf);

	/* If it doesn't fit, deal with split case */
	if (GinNonLeafDataPageGetFreeSpace(page) < sizeof(PostingItem))
	{
		dataSplitPageInternal(btree, buf, stack, insertdata, updateblkno,
							  newlpage, newrpage);
		return GPTP_SPLIT;
	}

	/* Else, we're ready to proceed with insertion */
	return GPTP_INSERT;
}

/*
 * Perform data insertion after beginPlaceToPage has decided it will fit.
 *
 * This is invoked within a critical section, and XLOG record creation (if
 * needed) is already started.  The target buffer is registered in slot 0.
 */
static void
dataExecPlaceToPageInternal(GinBtree btree, Buffer buf, GinBtreeStack *stack,
							void *insertdata, BlockNumber updateblkno,
							void *ptp_workspace)
{
	Page		page = BufferGetPage(buf);
	OffsetNumber off = stack->off;
	PostingItem *pitem;

	/* Update existing downlink to point to next page (on internal page) */
	pitem = GinDataPageGetPostingItem(page, off);
	PostingItemSetBlockNumber(pitem, updateblkno);

	/* Add new item */
	pitem = (PostingItem *) insertdata;
	GinDataPageAddPostingItem(page, pitem, off);

	MarkBufferDirty(buf);

	if (RelationNeedsWAL(btree->index) && !btree->isBuild)
	{
		/*
		 * This must be static, because it has to survive until XLogInsert,
		 * and we can't palloc here.  Ugly, but the XLogInsert infrastructure
		 * isn't reentrant anyway.
		 */
		static ginxlogInsertDataInternal data;

		data.offset = off;
		data.newitem = *pitem;

		XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
		XLogRegisterBufData(0, &data,
							sizeof(ginxlogInsertDataInternal));
	}
}

/*
 * Prepare to insert data on a posting-tree data page.
 *
 * If it will fit, return GPTP_INSERT after doing whatever setup is needed
 * before we enter the insertion critical section.  *ptp_workspace can be
 * set to pass information along to the execPlaceToPage function.
 *
 * If it won't fit, perform a page split and return two temporary page
 * images into *newlpage and *newrpage, with result GPTP_SPLIT.
 *
 * In neither case should the given page buffer be modified here.
 *
 * Note: on insertion to an internal node, in addition to inserting the given
 * item, the downlink of the existing item at stack->off will be updated to
 * point to updateblkno.
 *
 * Calls relevant function for internal or leaf page because they are handled
 * very differently.
 */
static GinPlaceToPageRC
dataBeginPlaceToPage(GinBtree btree, Buffer buf, GinBtreeStack *stack,
					 void *insertdata, BlockNumber updateblkno,
					 void **ptp_workspace,
					 Page *newlpage, Page *newrpage)
{
	Page		page = BufferGetPage(buf);

	Assert(GinPageIsData(page));

	if (GinPageIsLeaf(page))
		return dataBeginPlaceToPageLeaf(btree, buf, stack, insertdata,
										ptp_workspace,
										newlpage, newrpage);
	else
		return dataBeginPlaceToPageInternal(btree, buf, stack,
											insertdata, updateblkno,
											ptp_workspace,
											newlpage, newrpage);
}

/*
 * Perform data insertion after beginPlaceToPage has decided it will fit.
 *
 * This is invoked within a critical section, and XLOG record creation (if
 * needed) is already started.  The target buffer is registered in slot 0.
 *
 * Calls relevant function for internal or leaf page because they are handled
 * very differently.
 */
static void
dataExecPlaceToPage(GinBtree btree, Buffer buf, GinBtreeStack *stack,
					void *insertdata, BlockNumber updateblkno,
					void *ptp_workspace)
{
	Page		page = BufferGetPage(buf);

	if (GinPageIsLeaf(page))
		dataExecPlaceToPageLeaf(btree, buf, stack, insertdata,
								ptp_workspace);
	else
		dataExecPlaceToPageInternal(btree, buf, stack, insertdata,
									updateblkno, ptp_workspace);
}

/*
 * Split internal page and insert new data.
 *
 * Returns new temp pages to *newlpage and *newrpage.
 * The original buffer is left untouched.
 */
static void
dataSplitPageInternal(GinBtree btree, Buffer origbuf,
					  GinBtreeStack *stack,
					  void *insertdata, BlockNumber updateblkno,
					  Page *newlpage, Page *newrpage)
{
	Page		oldpage = BufferGetPage(origbuf);
	OffsetNumber off = stack->off;
	int			nitems = GinPageGetOpaque(oldpage)->maxoff;
	int			nleftitems;
	int			nrightitems;
	Size		pageSize = PageGetPageSize(oldpage);
	ItemPointerData oldbound = *GinDataPageGetRightBound(oldpage);
	ItemPointer bound;
	Page		lpage;
	Page		rpage;
	OffsetNumber separator;
	PostingItem allitems[(BLCKSZ / sizeof(PostingItem)) + 1];

	lpage = PageGetTempPage(oldpage);
	rpage = PageGetTempPage(oldpage);
	GinInitPage(lpage, GinPageGetOpaque(oldpage)->flags, pageSize);
	GinInitPage(rpage, GinPageGetOpaque(oldpage)->flags, pageSize);

	/*
	 * First construct a new list of PostingItems, which includes all the old
	 * items, and the new item.
	 */
	memcpy(allitems, GinDataPageGetPostingItem(oldpage, FirstOffsetNumber),
		   (off - 1) * sizeof(PostingItem));

	allitems[off - 1] = *((PostingItem *) insertdata);
	memcpy(&allitems[off], GinDataPageGetPostingItem(oldpage, off),
		   (nitems - (off - 1)) * sizeof(PostingItem));
	nitems++;

	/* Update existing downlink to point to next page */
	PostingItemSetBlockNumber(&allitems[off], updateblkno);

	/*
	 * When creating a new index, fit as many tuples as possible on the left
	 * page, on the assumption that the table is scanned from beginning to
	 * end. This packs the index as tight as possible.
	 */
	if (btree->isBuild && GinPageRightMost(oldpage))
		separator = GinNonLeafDataPageGetFreeSpace(rpage) / sizeof(PostingItem);
	else
		separator = nitems / 2;
	nleftitems = separator;
	nrightitems = nitems - separator;

	memcpy(GinDataPageGetPostingItem(lpage, FirstOffsetNumber),
		   allitems,
		   nleftitems * sizeof(PostingItem));
	GinPageGetOpaque(lpage)->maxoff = nleftitems;
	memcpy(GinDataPageGetPostingItem(rpage, FirstOffsetNumber),
		   &allitems[separator],
		   nrightitems * sizeof(PostingItem));
	GinPageGetOpaque(rpage)->maxoff = nrightitems;

	/*
	 * Also set pd_lower for both pages, like GinDataPageAddPostingItem does.
	 */
	GinDataPageSetDataSize(lpage, nleftitems * sizeof(PostingItem));
	GinDataPageSetDataSize(rpage, nrightitems * sizeof(PostingItem));

	/* set up right bound for left page */
	bound = GinDataPageGetRightBound(lpage);
	*bound = GinDataPageGetPostingItem(lpage, nleftitems)->key;

	/* set up right bound for right page */
	*GinDataPageGetRightBound(rpage) = oldbound;

	/* return temp pages to caller */
	*newlpage = lpage;
	*newrpage = rpage;
}

/*
 * Construct insertion payload for inserting the downlink for given buffer.
 */
static void *
dataPrepareDownlink(GinBtree btree, Buffer lbuf)
{
	PostingItem *pitem = palloc(sizeof(PostingItem));
	Page		lpage = BufferGetPage(lbuf);

	PostingItemSetBlockNumber(pitem, BufferGetBlockNumber(lbuf));
	pitem->key = *GinDataPageGetRightBound(lpage);

	return pitem;
}

/*
 * Fills new root by right bound values from child.
 * Also called from ginxlog, should not use btree
 */
void
ginDataFillRoot(GinBtree btree, Page root, BlockNumber lblkno, Page lpage, BlockNumber rblkno, Page rpage)
{
	PostingItem li,
				ri;

	li.key = *GinDataPageGetRightBound(lpage);
	PostingItemSetBlockNumber(&li, lblkno);
	GinDataPageAddPostingItem(root, &li, InvalidOffsetNumber);

	ri.key = *GinDataPageGetRightBound(rpage);
	PostingItemSetBlockNumber(&ri, rblkno);
	GinDataPageAddPostingItem(root, &ri, InvalidOffsetNumber);
}


/*** Functions to work with disassembled leaf pages ***/

/*
 * Disassemble page into a disassembledLeaf struct.
 */
static disassembledLeaf *
disassembleLeaf(Page page)
{
	disassembledLeaf *leaf;
	GinPostingList *seg;
	Pointer		segbegin;
	Pointer		segend;

	leaf = palloc0(sizeof(disassembledLeaf));
	dlist_init(&leaf->segments);

	if (GinPageIsCompressed(page))
	{
		/*
		 * Create a leafSegmentInfo entry for each segment.
		 */
		seg = GinDataLeafPageGetPostingList(page);
		segbegin = (Pointer) seg;
		segend = segbegin + GinDataLeafPageGetPostingListSize(page);
		while ((Pointer) seg < segend)
		{
			leafSegmentInfo *seginfo = palloc(sizeof(leafSegmentInfo));

			seginfo->action = GIN_SEGMENT_UNMODIFIED;
			seginfo->seg = seg;
			seginfo->items = NULL;
			seginfo->nitems = 0;
			dlist_push_tail(&leaf->segments, &seginfo->node);

			seg = GinNextPostingListSegment(seg);
		}
		leaf->oldformat = false;
	}
	else
	{
		/*
		 * A pre-9.4 format uncompressed page is represented by a single
		 * segment, with an array of items.  The corner case is uncompressed
		 * page containing no items, which is represented as no segments.
		 */
		ItemPointer uncompressed;
		int			nuncompressed;
		leafSegmentInfo *seginfo;

		uncompressed = dataLeafPageGetUncompressed(page, &nuncompressed);

		if (nuncompressed > 0)
		{
			seginfo = palloc(sizeof(leafSegmentInfo));

			seginfo->action = GIN_SEGMENT_REPLACE;
			seginfo->seg = NULL;
			seginfo->items = palloc(nuncompressed * sizeof(ItemPointerData));
			memcpy(seginfo->items, uncompressed, nuncompressed * sizeof(ItemPointerData));
			seginfo->nitems = nuncompressed;

			dlist_push_tail(&leaf->segments, &seginfo->node);
		}

		leaf->oldformat = true;
	}

	return leaf;
}

/*
 * Distribute newItems to the segments.
 *
 * Any segments that acquire new items are decoded, and the new items are
 * merged with the old items.
 *
 * Returns true if any new items were added. False means they were all
 * duplicates of existing items on the page.
 */
static bool
addItemsToLeaf(disassembledLeaf *leaf, ItemPointer newItems, int nNewItems)
{
	dlist_iter	iter;
	ItemPointer nextnew = newItems;
	int			newleft = nNewItems;
	bool		modified = false;
	leafSegmentInfo *newseg;

	/*
	 * If the page is completely empty, just construct one new segment to hold
	 * all the new items.
	 */
	if (dlist_is_empty(&leaf->segments))
	{
		newseg = palloc(sizeof(leafSegmentInfo));
		newseg->seg = NULL;
		newseg->items = newItems;
		newseg->nitems = nNewItems;
		newseg->action = GIN_SEGMENT_INSERT;
		dlist_push_tail(&leaf->segments, &newseg->node);
		return true;
	}

	dlist_foreach(iter, &leaf->segments)
	{
		leafSegmentInfo *cur = (leafSegmentInfo *) dlist_container(leafSegmentInfo, node, iter.cur);
		int			nthis;
		ItemPointer tmpitems;
		int			ntmpitems;

		/*
		 * How many of the new items fall into this segment?
		 */
		if (!dlist_has_next(&leaf->segments, iter.cur))
			nthis = newleft;
		else
		{
			leafSegmentInfo *next;
			ItemPointerData next_first;

			next = (leafSegmentInfo *) dlist_container(leafSegmentInfo, node,
													   dlist_next_node(&leaf->segments, iter.cur));
			if (next->items)
				next_first = next->items[0];
			else
			{
				Assert(next->seg != NULL);
				next_first = next->seg->first;
			}

			nthis = 0;
			while (nthis < newleft && ginCompareItemPointers(&nextnew[nthis], &next_first) < 0)
				nthis++;
		}
		if (nthis == 0)
			continue;

		/* Merge the new items with the existing items. */
		if (!cur->items)
			cur->items = ginPostingListDecode(cur->seg, &cur->nitems);

		/*
		 * Fast path for the important special case that we're appending to
		 * the end of the page: don't let the last segment on the page grow
		 * larger than the target, create a new segment before that happens.
		 */
		if (!dlist_has_next(&leaf->segments, iter.cur) &&
			ginCompareItemPointers(&cur->items[cur->nitems - 1], &nextnew[0]) < 0 &&
			cur->seg != NULL &&
			SizeOfGinPostingList(cur->seg) >= GinPostingListSegmentTargetSize)
		{
			newseg = palloc(sizeof(leafSegmentInfo));
			newseg->seg = NULL;
			newseg->items = nextnew;
			newseg->nitems = nthis;
			newseg->action = GIN_SEGMENT_INSERT;
			dlist_push_tail(&leaf->segments, &newseg->node);
			modified = true;
			break;
		}

		tmpitems = ginMergeItemPointers(cur->items, cur->nitems,
										nextnew, nthis,
										&ntmpitems);
		if (ntmpitems != cur->nitems)
		{
			/*
			 * If there are no duplicates, track the added items so that we
			 * can emit a compact ADDITEMS WAL record later on. (it doesn't
			 * seem worth re-checking which items were duplicates, if there
			 * were any)
			 */
			if (ntmpitems == nthis + cur->nitems &&
				cur->action == GIN_SEGMENT_UNMODIFIED)
			{
				cur->action = GIN_SEGMENT_ADDITEMS;
				cur->modifieditems = nextnew;
				cur->nmodifieditems = nthis;
			}
			else
				cur->action = GIN_SEGMENT_REPLACE;

			cur->items = tmpitems;
			cur->nitems = ntmpitems;
			cur->seg = NULL;
			modified = true;
		}

		nextnew += nthis;
		newleft -= nthis;
		if (newleft == 0)
			break;
	}

	return modified;
}

/*
 * Recompresses all segments that have been modified.
 *
 * If not all the items fit on two pages (ie. after split), we store as
 * many items as fit, and set *remaining to the first item that didn't fit.
 * If all items fit, *remaining is set to invalid.
 *
 * Returns true if the page has to be split.
 */
static bool
leafRepackItems(disassembledLeaf *leaf, ItemPointer remaining)
{
	int			pgused = 0;
	bool		needsplit = false;
	dlist_iter	iter;
	int			segsize;
	leafSegmentInfo *nextseg;
	int			npacked;
	bool		modified;
	dlist_node *cur_node;
	dlist_node *next_node;

	ItemPointerSetInvalid(remaining);

	/*
	 * cannot use dlist_foreach_modify here because we insert adjacent items
	 * while iterating.
	 */
	for (cur_node = dlist_head_node(&leaf->segments);
		 cur_node != NULL;
		 cur_node = next_node)
	{
		leafSegmentInfo *seginfo = dlist_container(leafSegmentInfo, node,
												   cur_node);

		if (dlist_has_next(&leaf->segments, cur_node))
			next_node = dlist_next_node(&leaf->segments, cur_node);
		else
			next_node = NULL;

		/* Compress the posting list, if necessary */
		if (seginfo->action != GIN_SEGMENT_DELETE)
		{
			if (seginfo->seg == NULL)
			{
				if (seginfo->nitems > GinPostingListSegmentMaxSize)
					npacked = 0;	/* no chance that it would fit. */
				else
				{
					seginfo->seg = ginCompressPostingList(seginfo->items,
														  seginfo->nitems,
														  GinPostingListSegmentMaxSize,
														  &npacked);
				}
				if (npacked != seginfo->nitems)
				{
					/*
					 * Too large. Compress again to the target size, and
					 * create a new segment to represent the remaining items.
					 * The new segment is inserted after this one, so it will
					 * be processed in the next iteration of this loop.
					 */
					if (seginfo->seg)
						pfree(seginfo->seg);
					seginfo->seg = ginCompressPostingList(seginfo->items,
														  seginfo->nitems,
														  GinPostingListSegmentTargetSize,
														  &npacked);
					if (seginfo->action != GIN_SEGMENT_INSERT)
						seginfo->action = GIN_SEGMENT_REPLACE;

					nextseg = palloc(sizeof(leafSegmentInfo));
					nextseg->action = GIN_SEGMENT_INSERT;
					nextseg->seg = NULL;
					nextseg->items = &seginfo->items[npacked];
					nextseg->nitems = seginfo->nitems - npacked;
					next_node = &nextseg->node;
					dlist_insert_after(cur_node, next_node);
				}
			}

			/*
			 * If the segment is very small, merge it with the next segment.
			 */
			if (SizeOfGinPostingList(seginfo->seg) < GinPostingListSegmentMinSize && next_node)
			{
				int			nmerged;

				nextseg = dlist_container(leafSegmentInfo, node, next_node);

				if (seginfo->items == NULL)
					seginfo->items = ginPostingListDecode(seginfo->seg,
														  &seginfo->nitems);
				if (nextseg->items == NULL)
					nextseg->items = ginPostingListDecode(nextseg->seg,
														  &nextseg->nitems);
				nextseg->items =
					ginMergeItemPointers(seginfo->items, seginfo->nitems,
										 nextseg->items, nextseg->nitems,
										 &nmerged);
				Assert(nmerged == seginfo->nitems + nextseg->nitems);
				nextseg->nitems = nmerged;
				nextseg->seg = NULL;

				nextseg->action = GIN_SEGMENT_REPLACE;
				nextseg->modifieditems = NULL;
				nextseg->nmodifieditems = 0;

				if (seginfo->action == GIN_SEGMENT_INSERT)
				{
					dlist_delete(cur_node);
					continue;
				}
				else
				{
					seginfo->action = GIN_SEGMENT_DELETE;
					seginfo->seg = NULL;
				}
			}

			seginfo->items = NULL;
			seginfo->nitems = 0;
		}

		if (seginfo->action == GIN_SEGMENT_DELETE)
			continue;

		/*
		 * OK, we now have a compressed version of this segment ready for
		 * copying to the page. Did we exceed the size that fits on one page?
		 */
		segsize = SizeOfGinPostingList(seginfo->seg);
		if (pgused + segsize > GinDataPageMaxDataSize)
		{
			if (!needsplit)
			{
				/* switch to right page */
				Assert(pgused > 0);
				leaf->lastleft = dlist_prev_node(&leaf->segments, cur_node);
				needsplit = true;
				leaf->lsize = pgused;
				pgused = 0;
			}
			else
			{
				/*
				 * Filled both pages. The last segment we constructed did not
				 * fit.
				 */
				*remaining = seginfo->seg->first;

				/*
				 * remove all segments that did not fit from the list.
				 */
				while (dlist_has_next(&leaf->segments, cur_node))
					dlist_delete(dlist_next_node(&leaf->segments, cur_node));
				dlist_delete(cur_node);
				break;
			}
		}

		pgused += segsize;
	}

	if (!needsplit)
	{
		leaf->lsize = pgused;
		leaf->rsize = 0;
	}
	else
		leaf->rsize = pgused;

	Assert(leaf->lsize <= GinDataPageMaxDataSize);
	Assert(leaf->rsize <= GinDataPageMaxDataSize);

	/*
	 * Make a palloc'd copy of every segment after the first modified one,
	 * because as we start copying items to the original page, we might
	 * overwrite an existing segment.
	 */
	modified = false;
	dlist_foreach(iter, &leaf->segments)
	{
		leafSegmentInfo *seginfo = dlist_container(leafSegmentInfo, node,
												   iter.cur);

		if (!modified && seginfo->action != GIN_SEGMENT_UNMODIFIED)
		{
			modified = true;
		}
		else if (modified && seginfo->action == GIN_SEGMENT_UNMODIFIED)
		{
			GinPostingList *tmp;

			segsize = SizeOfGinPostingList(seginfo->seg);
			tmp = palloc(segsize);
			memcpy(tmp, seginfo->seg, segsize);
			seginfo->seg = tmp;
		}
	}

	return needsplit;
}


/*** Functions that are exported to the rest of the GIN code ***/

/*
 * Creates new posting tree containing the given TIDs. Returns the page
 * number of the root of the new posting tree.
 *
 * items[] must be in sorted order with no duplicates.
 */
BlockNumber
createPostingTree(Relation index, ItemPointerData *items, uint32 nitems,
				  GinStatsData *buildStats, Buffer entrybuffer)
{
	BlockNumber blkno;
	Buffer		buffer;
	Page		tmppage;
	Page		page;
	Pointer		ptr;
	int			nrootitems;
	int			rootsize;
	bool		is_build = (buildStats != NULL);

	/* Construct the new root page in memory first. */
	tmppage = (Page) palloc(BLCKSZ);
	GinInitPage(tmppage, GIN_DATA | GIN_LEAF | GIN_COMPRESSED, BLCKSZ);
	GinPageGetOpaque(tmppage)->rightlink = InvalidBlockNumber;

	/*
	 * Write as many of the items to the root page as fit. In segments of max
	 * GinPostingListSegmentMaxSize bytes each.
	 */
	nrootitems = 0;
	rootsize = 0;
	ptr = (Pointer) GinDataLeafPageGetPostingList(tmppage);
	while (nrootitems < nitems)
	{
		GinPostingList *segment;
		int			npacked;
		int			segsize;

		segment = ginCompressPostingList(&items[nrootitems],
										 nitems - nrootitems,
										 GinPostingListSegmentMaxSize,
										 &npacked);
		segsize = SizeOfGinPostingList(segment);
		if (rootsize + segsize > GinDataPageMaxDataSize)
			break;

		memcpy(ptr, segment, segsize);
		ptr += segsize;
		rootsize += segsize;
		nrootitems += npacked;
		pfree(segment);
	}
	GinDataPageSetDataSize(tmppage, rootsize);

	/*
	 * All set. Get a new physical page, and copy the in-memory page to it.
	 */
	buffer = GinNewBuffer(index);
	page = BufferGetPage(buffer);
	blkno = BufferGetBlockNumber(buffer);

	/*
	 * Copy any predicate locks from the entry tree leaf (containing posting
	 * list) to the posting tree.
	 */
	PredicateLockPageSplit(index, BufferGetBlockNumber(entrybuffer), blkno);

	START_CRIT_SECTION();

	PageRestoreTempPage(tmppage, page);
	MarkBufferDirty(buffer);

	if (RelationNeedsWAL(index) && !is_build)
	{
		XLogRecPtr	recptr;
		ginxlogCreatePostingTree data;

		data.size = rootsize;

		XLogBeginInsert();
		XLogRegisterData(&data, sizeof(ginxlogCreatePostingTree));

		XLogRegisterData(GinDataLeafPageGetPostingList(page),
						 rootsize);
		XLogRegisterBuffer(0, buffer, REGBUF_WILL_INIT);

		recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_CREATE_PTREE);
		PageSetLSN(page, recptr);
	}

	UnlockReleaseBuffer(buffer);

	END_CRIT_SECTION();

	/* During index build, count the newly-added data page */
	if (buildStats)
		buildStats->nDataPages++;

	elog(DEBUG2, "created GIN posting tree with %d items", nrootitems);

	/*
	 * Add any remaining TIDs to the newly-created posting tree.
	 */
	if (nitems > nrootitems)
	{
		ginInsertItemPointers(index, blkno,
							  items + nrootitems,
							  nitems - nrootitems,
							  buildStats);
	}

	return blkno;
}

static void
ginPrepareDataScan(GinBtree btree, Relation index, BlockNumber rootBlkno)
{
	memset(btree, 0, sizeof(GinBtreeData));

	btree->index = index;
	btree->rootBlkno = rootBlkno;

	btree->findChildPage = dataLocateItem;
	btree->getLeftMostChild = dataGetLeftMostPage;
	btree->isMoveRight = dataIsMoveRight;
	btree->findItem = NULL;
	btree->findChildPtr = dataFindChildPtr;
	btree->beginPlaceToPage = dataBeginPlaceToPage;
	btree->execPlaceToPage = dataExecPlaceToPage;
	btree->fillRoot = ginDataFillRoot;
	btree->prepareDownlink = dataPrepareDownlink;

	btree->isData = true;
	btree->fullScan = false;
	btree->isBuild = false;
}

/*
 * Inserts array of item pointers, may execute several tree scan (very rare)
 */
void
ginInsertItemPointers(Relation index, BlockNumber rootBlkno,
					  ItemPointerData *items, uint32 nitem,
					  GinStatsData *buildStats)
{
	GinBtreeData btree;
	GinBtreeDataLeafInsertData insertdata;
	GinBtreeStack *stack;

	ginPrepareDataScan(&btree, index, rootBlkno);
	btree.isBuild = (buildStats != NULL);
	insertdata.items = items;
	insertdata.nitem = nitem;
	insertdata.curitem = 0;

	while (insertdata.curitem < insertdata.nitem)
	{
		/* search for the leaf page where the first item should go to */
		btree.itemptr = insertdata.items[insertdata.curitem];
		stack = ginFindLeafPage(&btree, false, true);

		ginInsertValue(&btree, stack, &insertdata, buildStats);
	}
}

/*
 * Starts a new scan on a posting tree.
 */
GinBtreeStack *
ginScanBeginPostingTree(GinBtree btree, Relation index, BlockNumber rootBlkno)
{
	GinBtreeStack *stack;

	ginPrepareDataScan(btree, index, rootBlkno);

	btree->fullScan = true;

	stack = ginFindLeafPage(btree, true, false);

	return stack;
}
