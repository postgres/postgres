/*-------------------------------------------------------------------------
 *
 * gindatapage.c
 *	  page utilities routines for the postgres inverted index access method.
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gin/gindatapage.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin_private.h"
#include "miscadmin.h"
#include "utils/rel.h"

/*
 * Checks, should we move to right link...
 * Compares inserting itemp pointer with right bound of current page
 */
static bool
dataIsMoveRight(GinBtree btree, Page page)
{
	ItemPointer iptr = GinDataPageGetRightBound(page);

	if (GinPageRightMost(page))
		return FALSE;

	return (ginCompareItemPointers(&btree->itemptr, iptr) > 0) ? TRUE : FALSE;
}

/*
 * Find correct PostingItem in non-leaf page. It supposed that page
 * correctly chosen and searching value SHOULD be on page
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
 * Searches correct position for value on leaf page.
 * Page should be correctly chosen.
 * Returns true if value found on page.
 */
static bool
dataLocateLeafItem(GinBtree btree, GinBtreeStack *stack)
{
	Page		page = BufferGetPage(stack->buffer);
	OffsetNumber low,
				high;
	int			result;

	Assert(GinPageIsLeaf(page));
	Assert(GinPageIsData(page));

	if (btree->fullScan)
	{
		stack->off = FirstOffsetNumber;
		return TRUE;
	}

	low = FirstOffsetNumber;
	high = GinPageGetOpaque(page)->maxoff;

	if (high < low)
	{
		stack->off = FirstOffsetNumber;
		return false;
	}

	high++;

	while (high > low)
	{
		OffsetNumber mid = low + ((high - low) / 2);

		result = ginCompareItemPointers(&btree->itemptr,
										GinDataPageGetItemPointer(page, mid));

		if (result == 0)
		{
			stack->off = mid;
			return true;
		}
		else if (result > 0)
			low = mid + 1;
		else
			high = mid;
	}

	stack->off = high;
	return false;
}

/*
 * Finds links to blkno on non-leaf page, returns
 * offset of PostingItem
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
 * returns blkno of leftmost child
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
 * add ItemPointer to a leaf page.
 */
void
GinDataPageAddItemPointer(Page page, ItemPointer data, OffsetNumber offset)
{
	OffsetNumber maxoff = GinPageGetOpaque(page)->maxoff;
	char	   *ptr;

	Assert(ItemPointerIsValid(data));
	Assert(GinPageIsLeaf(page));

	if (offset == InvalidOffsetNumber)
	{
		ptr = (char *) GinDataPageGetItemPointer(page, maxoff + 1);
	}
	else
	{
		ptr = (char *) GinDataPageGetItemPointer(page, offset);
		if (maxoff + 1 - offset != 0)
			memmove(ptr + sizeof(ItemPointerData),
					ptr,
					(maxoff - offset + 1) * sizeof(ItemPointerData));
	}
	memcpy(ptr, data, sizeof(ItemPointerData));

	GinPageGetOpaque(page)->maxoff++;
}

/*
 * add PostingItem to a non-leaf page.
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
		if (maxoff + 1 - offset != 0)
			memmove(ptr + sizeof(PostingItem),
					ptr,
					(maxoff - offset + 1) * sizeof(PostingItem));
	}
	memcpy(ptr, data, sizeof(PostingItem));

	GinPageGetOpaque(page)->maxoff++;
}

/*
 * Deletes posting item from non-leaf page
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

	GinPageGetOpaque(page)->maxoff--;
}

/*
 * checks space to install new value,
 * item pointer never deletes!
 */
static bool
dataIsEnoughSpace(GinBtree btree, Buffer buf, OffsetNumber off, void *insertdata)
{
	Page		page = BufferGetPage(buf);

	Assert(GinPageIsData(page));

	if (GinPageIsLeaf(page))
	{
		GinBtreeDataLeafInsertData *items = insertdata;

		if (GinPageRightMost(page) && off > GinPageGetOpaque(page)->maxoff)
		{
			if ((items->nitem - items->curitem) * sizeof(ItemPointerData) <= GinDataPageGetFreeSpace(page))
				return true;
		}
		else if (sizeof(ItemPointerData) <= GinDataPageGetFreeSpace(page))
			return true;
	}
	else if (sizeof(PostingItem) <= GinDataPageGetFreeSpace(page))
		return true;

	return false;
}

/*
 * Places keys to page and fills WAL record. In case leaf page and
 * build mode puts all ItemPointers to page.
 *
 * If none of the keys fit, returns false without modifying the page.
 *
 * On insertion to an internal node, in addition to inserting the given item,
 * the downlink of the existing item at 'off' is updated to point to
 * 'updateblkno'.
 */
static bool
dataPlaceToPage(GinBtree btree, Buffer buf, OffsetNumber off,
				void *insertdata, BlockNumber updateblkno,
				XLogRecData **prdata)
{
	Page		page = BufferGetPage(buf);
	/* these must be static so they can be returned to caller */
	static XLogRecData rdata[2];

	/* quick exit if it doesn't fit */
	if (!dataIsEnoughSpace(btree, buf, off, insertdata))
		return false;

	*prdata = rdata;
	Assert(GinPageIsData(page));

	/* Update existing downlink to point to next page (on internal page) */
	if (!GinPageIsLeaf(page))
	{
		PostingItem *pitem = GinDataPageGetPostingItem(page, off);

		PostingItemSetBlockNumber(pitem, updateblkno);
	}

	if (GinPageIsLeaf(page))
	{
		GinBtreeDataLeafInsertData *items = insertdata;
		static ginxlogInsertDataLeaf data;
		uint32		savedPos = items->curitem;

		if (GinPageRightMost(page) && off > GinPageGetOpaque(page)->maxoff)
		{
			/* usually, create index... */
			while (items->curitem < items->nitem)
			{
				GinDataPageAddItemPointer(page, items->items + items->curitem, off);
				off++;
				items->curitem++;
			}
			data.nitem = items->curitem - savedPos;
		}
		else
		{
			GinDataPageAddItemPointer(page, items->items + items->curitem, off);
			items->curitem++;
			data.nitem = 1;
		}

		rdata[0].buffer = buf;
		rdata[0].buffer_std = false;
		rdata[0].data = (char *) &data;
		rdata[0].len = offsetof(ginxlogInsertDataLeaf, items);
		rdata[0].next = &rdata[1];

		rdata[1].buffer = buf;
		rdata[1].buffer_std = false;
		rdata[1].data = (char *) &items->items[savedPos];
		rdata[1].len = sizeof(ItemPointerData) * data.nitem;
		rdata[1].next = NULL;
	}
	else
	{
		PostingItem *pitem = insertdata;

		GinDataPageAddPostingItem(page, pitem, off);

		rdata[0].buffer = buf;
		rdata[0].buffer_std = false;
		rdata[0].data = (char *) pitem;
		rdata[0].len = sizeof(PostingItem);
		rdata[0].next = NULL;
	}

	return true;
}

/*
 * split page and fills WAL record. original buffer(lbuf) leaves untouched,
 * returns shadow page of lbuf filled new data. In leaf page and build mode puts all
 * ItemPointers to pages. Also, in build mode splits data by way to full fulled
 * left page
 */
static Page
dataSplitPage(GinBtree btree, Buffer lbuf, Buffer rbuf, OffsetNumber off,
			  void *insertdata, BlockNumber updateblkno, XLogRecData **prdata)
{
	char	   *ptr;
	OffsetNumber separator;
	ItemPointer bound;
	Page		lpage = PageGetTempPageCopy(BufferGetPage(lbuf));
	bool		isleaf = GinPageIsLeaf(lpage);
	ItemPointerData oldbound = *GinDataPageGetRightBound(lpage);
	int			sizeofitem = GinSizeOfDataPageItem(lpage);
	OffsetNumber maxoff = GinPageGetOpaque(lpage)->maxoff;
	Page		rpage = BufferGetPage(rbuf);
	Size		pageSize = PageGetPageSize(lpage);
	Size		freeSpace;

	/* these must be static so they can be returned to caller */
	static ginxlogSplitData data;
	static XLogRecData rdata[2];
	static char vector[2 * BLCKSZ];

	GinInitPage(rpage, GinPageGetOpaque(lpage)->flags, pageSize);
	freeSpace = GinDataPageGetFreeSpace(rpage);

	*prdata = rdata;

	/* Update existing downlink to point to next page (on internal page) */
	if (!isleaf)
	{
		PostingItem *pitem = GinDataPageGetPostingItem(lpage, off);

		PostingItemSetBlockNumber(pitem, updateblkno);
	}

	if (isleaf)
	{
		memcpy(vector,
			   GinDataPageGetItemPointer(lpage, FirstOffsetNumber),
			   maxoff * sizeof(ItemPointerData));
	}
	else
	{
		memcpy(vector,
			   GinDataPageGetPostingItem(lpage, FirstOffsetNumber),
			   maxoff * sizeof(PostingItem));
	}

	if (isleaf && GinPageRightMost(lpage) && off > GinPageGetOpaque(lpage)->maxoff)
	{
		/* append new items to the end */
		GinBtreeDataLeafInsertData *items = insertdata;

		while (items->curitem < items->nitem &&
			   maxoff * sizeof(ItemPointerData) < 2 * (freeSpace - sizeof(ItemPointerData)))
		{
			memcpy(vector + maxoff * sizeof(ItemPointerData),
				   items->items + items->curitem,
				   sizeof(ItemPointerData));
			maxoff++;
			items->curitem++;
		}
	}
	else
	{
		ptr = vector + (off - 1) * sizeofitem;
		if (maxoff + 1 - off != 0)
			memmove(ptr + sizeofitem, ptr, (maxoff - off + 1) * sizeofitem);
		if (isleaf)
		{
			GinBtreeDataLeafInsertData *items = insertdata;

			memcpy(ptr, items->items + items->curitem, sizeofitem);
			items->curitem++;
		}
		else
		{
			PostingItem *pitem = insertdata;

			memcpy(ptr, pitem, sizeofitem);
		}

		maxoff++;
	}

	/*
	 * we assume that during index creation the table scanned from beginning
	 * to end, so ItemPointers are in monotonically increasing order.
	 */
	if (btree->isBuild && GinPageRightMost(lpage))
		separator = freeSpace / sizeofitem;
	else
		separator = maxoff / 2;

	GinInitPage(rpage, GinPageGetOpaque(lpage)->flags, pageSize);
	GinInitPage(lpage, GinPageGetOpaque(rpage)->flags, pageSize);

	if (isleaf)
		memcpy(GinDataPageGetItemPointer(lpage, FirstOffsetNumber),
			   vector, separator * sizeof(ItemPointerData));
	else
		memcpy(GinDataPageGetPostingItem(lpage, FirstOffsetNumber),
			   vector, separator * sizeof(PostingItem));

	GinPageGetOpaque(lpage)->maxoff = separator;
	if (isleaf)
		memcpy(GinDataPageGetItemPointer(rpage, FirstOffsetNumber),
			   vector + separator * sizeof(ItemPointerData),
			   (maxoff - separator) * sizeof(ItemPointerData));
	else
		memcpy(GinDataPageGetPostingItem(rpage, FirstOffsetNumber),
			   vector + separator * sizeof(PostingItem),
			   (maxoff - separator) * sizeof(PostingItem));

	GinPageGetOpaque(rpage)->maxoff = maxoff - separator;

	/* set up right bound for left page */
	bound = GinDataPageGetRightBound(lpage);
	if (GinPageIsLeaf(lpage))
		*bound = *GinDataPageGetItemPointer(lpage,
											GinPageGetOpaque(lpage)->maxoff);
	else
		*bound = GinDataPageGetPostingItem(lpage,
									   GinPageGetOpaque(lpage)->maxoff)->key;

	/* set up right bound for right page */
	bound = GinDataPageGetRightBound(rpage);
	*bound = oldbound;

	data.separator = separator;
	data.nitem = maxoff;
	data.rightbound = oldbound;

	rdata[0].buffer = InvalidBuffer;
	rdata[0].data = (char *) &data;
	rdata[0].len = sizeof(ginxlogSplitData);
	rdata[0].next = &rdata[1];

	rdata[1].buffer = InvalidBuffer;
	rdata[1].data = vector;
	rdata[1].len = maxoff * sizeofitem;
	rdata[1].next = NULL;

	return lpage;
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

/*
 * Creates new posting tree containing the given TIDs. Returns the page
 * number of the root of the new posting tree.
 *
 * items[] must be in sorted order with no duplicates.
 */
BlockNumber
createPostingTree(Relation index, ItemPointerData *items, uint32 nitems,
				  GinStatsData *buildStats)
{
	BlockNumber blkno;
	Buffer		buffer;
	Page		page;
	int			nrootitems;

	/* Calculate how many TIDs will fit on first page. */
	nrootitems = Min(nitems, GinMaxLeafDataItems);

	/*
	 * Create the root page.
	 */
	buffer = GinNewBuffer(index);
	page = BufferGetPage(buffer);
	blkno = BufferGetBlockNumber(buffer);

	START_CRIT_SECTION();

	GinInitBuffer(buffer, GIN_DATA | GIN_LEAF);
	memcpy(GinDataPageGetData(page), items, sizeof(ItemPointerData) * nrootitems);
	GinPageGetOpaque(page)->maxoff = nrootitems;

	MarkBufferDirty(buffer);

	if (RelationNeedsWAL(index))
	{
		XLogRecPtr	recptr;
		XLogRecData rdata[2];
		ginxlogCreatePostingTree data;

		data.node = index->rd_node;
		data.blkno = blkno;
		data.nitem = nrootitems;

		rdata[0].buffer = InvalidBuffer;
		rdata[0].data = (char *) &data;
		rdata[0].len = sizeof(ginxlogCreatePostingTree);
		rdata[0].next = &rdata[1];

		rdata[1].buffer = InvalidBuffer;
		rdata[1].data = (char *) items;
		rdata[1].len = sizeof(ItemPointerData) * nrootitems;
		rdata[1].next = NULL;

		recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_CREATE_PTREE, rdata);
		PageSetLSN(page, recptr);
	}

	UnlockReleaseBuffer(buffer);

	END_CRIT_SECTION();

	/* During index build, count the newly-added data page */
	if (buildStats)
		buildStats->nDataPages++;

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

void
ginPrepareDataScan(GinBtree btree, Relation index, BlockNumber rootBlkno)
{
	memset(btree, 0, sizeof(GinBtreeData));

	btree->index = index;
	btree->rootBlkno = rootBlkno;

	btree->findChildPage = dataLocateItem;
	btree->getLeftMostChild = dataGetLeftMostPage;
	btree->isMoveRight = dataIsMoveRight;
	btree->findItem = dataLocateLeafItem;
	btree->findChildPtr = dataFindChildPtr;
	btree->placeToPage = dataPlaceToPage;
	btree->splitPage = dataSplitPage;
	btree->fillRoot = ginDataFillRoot;
	btree->prepareDownlink = dataPrepareDownlink;

	btree->isData = TRUE;
	btree->fullScan = FALSE;
	btree->isBuild = FALSE;
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
		stack = ginFindLeafPage(&btree, false);

		if (btree.findItem(&btree, stack))
		{
			/*
			 * Current item already exists in index.
			 */
			insertdata.curitem++;
			LockBuffer(stack->buffer, GIN_UNLOCK);
			freeGinBtreeStack(stack);
		}
		else
			ginInsertValue(&btree, stack, &insertdata, buildStats);
	}
}

/*
 * Starts a new scan on a posting tree.
 */
GinBtreeStack *
ginScanBeginPostingTree(Relation index, BlockNumber rootBlkno)
{
	GinBtreeData btree;
	GinBtreeStack *stack;

	ginPrepareDataScan(&btree, index, rootBlkno);

	btree.fullScan = TRUE;

	stack = ginFindLeafPage(&btree, TRUE);

	return stack;
}
