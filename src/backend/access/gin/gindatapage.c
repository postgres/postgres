/*-------------------------------------------------------------------------
 *
 * gindatapage.c
 *	  page utilities routines for the postgres inverted index access method.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			$PostgreSQL: pgsql/src/backend/access/gin/gindatapage.c,v 1.5.2.1 2007/06/04 15:59:19 teodor Exp $
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "access/gin.h"

int
compareItemPointers(ItemPointer a, ItemPointer b)
{
	if (GinItemPointerGetBlockNumber(a) == GinItemPointerGetBlockNumber(b))
	{
		if (GinItemPointerGetOffsetNumber(a) == GinItemPointerGetOffsetNumber(b))
			return 0;
		return (GinItemPointerGetOffsetNumber(a) > GinItemPointerGetOffsetNumber(b)) ? 1 : -1;
	}

	return (GinItemPointerGetBlockNumber(a) > GinItemPointerGetBlockNumber(b)) ? 1 : -1;
}

/*
 * Merge two ordered array of itempointer
 */
void
MergeItemPointers(ItemPointerData *dst, ItemPointerData *a, uint32 na, ItemPointerData *b, uint32 nb)
{
	ItemPointerData *dptr = dst;
	ItemPointerData *aptr = a,
			   *bptr = b;

	while (aptr - a < na && bptr - b < nb)
	{
		if (compareItemPointers(aptr, bptr) > 0)
			*dptr++ = *bptr++;
		else
			*dptr++ = *aptr++;
	}

	while (aptr - a < na)
		*dptr++ = *aptr++;

	while (bptr - b < nb)
		*dptr++ = *bptr++;
}

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

	return (compareItemPointers(btree->items + btree->curitem, iptr) > 0) ? TRUE : FALSE;
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
		return btree->getLeftMostPage(btree, page);
	}

	low = FirstOffsetNumber;
	maxoff = high = GinPageGetOpaque(page)->maxoff;
	Assert(high >= low);

	high++;

	while (high > low)
	{
		OffsetNumber mid = low + ((high - low) / 2);

		pitem = (PostingItem *) GinDataPageGetItem(page, mid);

		if (mid == maxoff)

			/*
			 * Right infinity, page already correctly chosen with a help of
			 * dataIsMoveRight
			 */
			result = -1;
		else
		{
			pitem = (PostingItem *) GinDataPageGetItem(page, mid);
			result = compareItemPointers(btree->items + btree->curitem, &(pitem->key));
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
	pitem = (PostingItem *) GinDataPageGetItem(page, high);
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

		result = compareItemPointers(btree->items + btree->curitem, (ItemPointer) GinDataPageGetItem(page, mid));

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

	/* if page isn't changed, we returns storedOff */
	if (storedOff >= FirstOffsetNumber && storedOff <= maxoff)
	{
		pitem = (PostingItem *) GinDataPageGetItem(page, storedOff);
		if (PostingItemGetBlockNumber(pitem) == blkno)
			return storedOff;

		/*
		 * we hope, that needed pointer goes to right. It's true if there
		 * wasn't a deletion
		 */
		for (i = storedOff + 1; i <= maxoff; i++)
		{
			pitem = (PostingItem *) GinDataPageGetItem(page, i);
			if (PostingItemGetBlockNumber(pitem) == blkno)
				return i;
		}

		maxoff = storedOff - 1;
	}

	/* last chance */
	for (i = FirstOffsetNumber; i <= maxoff; i++)
	{
		pitem = (PostingItem *) GinDataPageGetItem(page, i);
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

	pitem = (PostingItem *) GinDataPageGetItem(page, FirstOffsetNumber);
	return PostingItemGetBlockNumber(pitem);
}

/*
 * add ItemPointer or PostingItem to page. data should point to
 * correct value! depending on leaf or non-leaf page
 */
void
GinDataPageAddItem(Page page, void *data, OffsetNumber offset)
{
	OffsetNumber maxoff = GinPageGetOpaque(page)->maxoff;
	char	   *ptr;

	if (offset == InvalidOffsetNumber)
	{
		ptr = GinDataPageGetItem(page, maxoff + 1);
	}
	else
	{
		ptr = GinDataPageGetItem(page, offset);
		if (maxoff + 1 - offset != 0)
			memmove(ptr + GinSizeOfItem(page), ptr, (maxoff - offset + 1) * GinSizeOfItem(page));
	}
	memcpy(ptr, data, GinSizeOfItem(page));

	GinPageGetOpaque(page)->maxoff++;
}

/*
 * Deletes posting item from non-leaf page
 */
void
PageDeletePostingItem(Page page, OffsetNumber offset)
{
	OffsetNumber maxoff = GinPageGetOpaque(page)->maxoff;

	Assert(!GinPageIsLeaf(page));
	Assert(offset >= FirstOffsetNumber && offset <= maxoff);

	if (offset != maxoff)
		memmove(GinDataPageGetItem(page, offset), GinDataPageGetItem(page, offset + 1),
				sizeof(PostingItem) * (maxoff - offset));

	GinPageGetOpaque(page)->maxoff--;
}

/*
 * checks space to install new value,
 * item pointer never deletes!
 */
static bool
dataIsEnoughSpace(GinBtree btree, Buffer buf, OffsetNumber off)
{
	Page		page = BufferGetPage(buf);

	Assert(GinPageIsData(page));
	Assert(!btree->isDelete);

	if (GinPageIsLeaf(page))
	{
		if (GinPageRightMost(page) && off > GinPageGetOpaque(page)->maxoff)
		{
			if ((btree->nitem - btree->curitem) * sizeof(ItemPointerData) <= GinDataPageGetFreeSpace(page))
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
 * In case of previous split update old child blkno to
 * new right page
 * item pointer never deletes!
 */
static BlockNumber
dataPrepareData(GinBtree btree, Page page, OffsetNumber off)
{
	BlockNumber ret = InvalidBlockNumber;

	Assert(GinPageIsData(page));

	if (!GinPageIsLeaf(page) && btree->rightblkno != InvalidBlockNumber)
	{
		PostingItem *pitem = (PostingItem *) GinDataPageGetItem(page, off);

		PostingItemSetBlockNumber(pitem, btree->rightblkno);
		ret = btree->rightblkno;
	}

	btree->rightblkno = InvalidBlockNumber;

	return ret;
}

/*
 * Places keys to page and fills WAL record. In case leaf page and
 * build mode puts all ItemPointers to page.
 */
static void
dataPlaceToPage(GinBtree btree, Buffer buf, OffsetNumber off, XLogRecData **prdata)
{
	Page		page = BufferGetPage(buf);
	static XLogRecData rdata[3];
	int			sizeofitem = GinSizeOfItem(page);
	static ginxlogInsert data;
	int 		cnt=0;

	*prdata = rdata;
	Assert(GinPageIsData(page));

	data.updateBlkno = dataPrepareData(btree, page, off);

	data.node = btree->index->rd_node;
	data.blkno = BufferGetBlockNumber(buf);
	data.offset = off;
	data.nitem = 1;
	data.isDelete = FALSE;
	data.isData = TRUE;
	data.isLeaf = GinPageIsLeaf(page) ? TRUE : FALSE;

	/* 
	 * Prevent full page write if child's split occurs. That is needed
	 * to remove incomplete splits while replaying WAL
	 * 
	 * data.updateBlkno contains new block number (of newly created right page)
	 * for recently splited page.
	 */
	if ( data.updateBlkno == InvalidBlockNumber ) 
	{
		rdata[0].buffer = buf;
		rdata[0].buffer_std = FALSE;
		rdata[0].data = NULL;
		rdata[0].len = 0;
		rdata[0].next = &rdata[1];
		cnt++;
	}

	rdata[cnt].buffer = InvalidBuffer;
	rdata[cnt].data = (char *) &data;
	rdata[cnt].len = sizeof(ginxlogInsert);
	rdata[cnt].next = &rdata[cnt+1];
	cnt++;

	rdata[cnt].buffer = InvalidBuffer;
	rdata[cnt].data = (GinPageIsLeaf(page)) ? ((char *) (btree->items + btree->curitem)) : ((char *) &(btree->pitem));
	rdata[cnt].len = sizeofitem;
	rdata[cnt].next = NULL;

	if (GinPageIsLeaf(page))
	{
		if (GinPageRightMost(page) && off > GinPageGetOpaque(page)->maxoff)
		{
			/* usually, create index... */
			uint32		savedPos = btree->curitem;

			while (btree->curitem < btree->nitem)
			{
				GinDataPageAddItem(page, btree->items + btree->curitem, off);
				off++;
				btree->curitem++;
			}
			data.nitem = btree->curitem - savedPos;
			rdata[cnt].len = sizeofitem * data.nitem;
		}
		else
		{
			GinDataPageAddItem(page, btree->items + btree->curitem, off);
			btree->curitem++;
		}
	}
	else
		GinDataPageAddItem(page, &(btree->pitem), off);
}

/*
 * split page and fills WAL record. original buffer(lbuf) leaves untouched,
 * returns shadow page of lbuf filled new data. In leaf page and build mode puts all
 * ItemPointers to pages. Also, in build mode splits data by way to full fulled
 * left page
 */
static Page
dataSplitPage(GinBtree btree, Buffer lbuf, Buffer rbuf, OffsetNumber off, XLogRecData **prdata)
{
	static ginxlogSplit data;
	static XLogRecData rdata[4];
	static char vector[2 * BLCKSZ];
	char	   *ptr;
	OffsetNumber separator;
	ItemPointer bound;
	Page		lpage = GinPageGetCopyPage(BufferGetPage(lbuf));
	ItemPointerData oldbound = *GinDataPageGetRightBound(lpage);
	int			sizeofitem = GinSizeOfItem(lpage);
	OffsetNumber maxoff = GinPageGetOpaque(lpage)->maxoff;
	Page		rpage = BufferGetPage(rbuf);
	Size		pageSize = PageGetPageSize(lpage);
	Size		freeSpace;
	uint32		nCopied = 1;

	GinInitPage(rpage, GinPageGetOpaque(lpage)->flags, pageSize);
	freeSpace = GinDataPageGetFreeSpace(rpage);

	*prdata = rdata;
	data.leftChildBlkno = (GinPageIsLeaf(lpage)) ?
		InvalidOffsetNumber : PostingItemGetBlockNumber(&(btree->pitem));
	data.updateBlkno = dataPrepareData(btree, lpage, off);

	memcpy(vector, GinDataPageGetItem(lpage, FirstOffsetNumber),
		   maxoff * sizeofitem);

	if (GinPageIsLeaf(lpage) && GinPageRightMost(lpage) && off > GinPageGetOpaque(lpage)->maxoff)
	{
		nCopied = 0;
		while (btree->curitem < btree->nitem && maxoff * sizeof(ItemPointerData) < 2 * (freeSpace - sizeof(ItemPointerData)))
		{
			memcpy(vector + maxoff * sizeof(ItemPointerData), btree->items + btree->curitem,
				   sizeof(ItemPointerData));
			maxoff++;
			nCopied++;
			btree->curitem++;
		}
	}
	else
	{
		ptr = vector + (off - 1) * sizeofitem;
		if (maxoff + 1 - off != 0)
			memmove(ptr + sizeofitem, ptr, (maxoff - off + 1) * sizeofitem);
		if (GinPageIsLeaf(lpage))
		{
			memcpy(ptr, btree->items + btree->curitem, sizeofitem);
			btree->curitem++;
		}
		else
			memcpy(ptr, &(btree->pitem), sizeofitem);

		maxoff++;
	}

	/*
	 * we suppose that during index creation table scaned from begin to end,
	 * so ItemPointers are monotonically increased..
	 */
	if (btree->isBuild && GinPageRightMost(lpage))
		separator = freeSpace / sizeofitem;
	else
		separator = maxoff / 2;

	GinInitPage(rpage, GinPageGetOpaque(lpage)->flags, pageSize);
	GinInitPage(lpage, GinPageGetOpaque(rpage)->flags, pageSize);

	memcpy(GinDataPageGetItem(lpage, FirstOffsetNumber), vector, separator * sizeofitem);
	GinPageGetOpaque(lpage)->maxoff = separator;
	memcpy(GinDataPageGetItem(rpage, FirstOffsetNumber),
		 vector + separator * sizeofitem, (maxoff - separator) * sizeofitem);
	GinPageGetOpaque(rpage)->maxoff = maxoff - separator;

	PostingItemSetBlockNumber(&(btree->pitem), BufferGetBlockNumber(lbuf));
	if (GinPageIsLeaf(lpage))
		btree->pitem.key = *(ItemPointerData *) GinDataPageGetItem(lpage,
											GinPageGetOpaque(lpage)->maxoff);
	else
		btree->pitem.key = ((PostingItem *) GinDataPageGetItem(lpage,
									  GinPageGetOpaque(lpage)->maxoff))->key;
	btree->rightblkno = BufferGetBlockNumber(rbuf);

	/* set up right bound for left page */
	bound = GinDataPageGetRightBound(lpage);
	*bound = btree->pitem.key;

	/* set up right bound for right page */
	bound = GinDataPageGetRightBound(rpage);
	*bound = oldbound;

	data.node = btree->index->rd_node;
	data.rootBlkno = InvalidBlockNumber;
	data.lblkno = BufferGetBlockNumber(lbuf);
	data.rblkno = BufferGetBlockNumber(rbuf);
	data.separator = separator;
	data.nitem = maxoff;
	data.isData = TRUE;
	data.isLeaf = GinPageIsLeaf(lpage) ? TRUE : FALSE;
	data.isRootSplit = FALSE;
	data.rightbound = oldbound;

	rdata[0].buffer = InvalidBuffer;
	rdata[0].data = (char *) &data;
	rdata[0].len = sizeof(ginxlogSplit);
	rdata[0].next = &rdata[1];

	rdata[1].buffer = InvalidBuffer;
	rdata[1].data = vector;
	rdata[1].len = MAXALIGN(maxoff * sizeofitem);
	rdata[1].next = NULL;

	return lpage;
}

/*
 * Fills new root by right bound values from child.
 * Also called from ginxlog, should not use btree
 */
void
dataFillRoot(GinBtree btree, Buffer root, Buffer lbuf, Buffer rbuf)
{
	Page		page = BufferGetPage(root),
				lpage = BufferGetPage(lbuf),
				rpage = BufferGetPage(rbuf);
	PostingItem li,
				ri;

	li.key = *GinDataPageGetRightBound(lpage);
	PostingItemSetBlockNumber(&li, BufferGetBlockNumber(lbuf));
	GinDataPageAddItem(page, &li, InvalidOffsetNumber);

	ri.key = *GinDataPageGetRightBound(rpage);
	PostingItemSetBlockNumber(&ri, BufferGetBlockNumber(rbuf));
	GinDataPageAddItem(page, &ri, InvalidOffsetNumber);
}

void
prepareDataScan(GinBtree btree, Relation index)
{
	memset(btree, 0, sizeof(GinBtreeData));
	btree->index = index;
	btree->isMoveRight = dataIsMoveRight;
	btree->findChildPage = dataLocateItem;
	btree->findItem = dataLocateLeafItem;
	btree->findChildPtr = dataFindChildPtr;
	btree->getLeftMostPage = dataGetLeftMostPage;
	btree->isEnoughSpace = dataIsEnoughSpace;
	btree->placeToPage = dataPlaceToPage;
	btree->splitPage = dataSplitPage;
	btree->fillRoot = dataFillRoot;

	btree->searchMode = FALSE;
	btree->isDelete = FALSE;
	btree->fullScan = FALSE;
	btree->isBuild = FALSE;
}

GinPostingTreeScan *
prepareScanPostingTree(Relation index, BlockNumber rootBlkno, bool searchMode)
{
	GinPostingTreeScan *gdi = (GinPostingTreeScan *) palloc0(sizeof(GinPostingTreeScan));

	prepareDataScan(&gdi->btree, index);

	gdi->btree.searchMode = searchMode;
	gdi->btree.fullScan = searchMode;

	gdi->stack = ginPrepareFindLeafPage(&gdi->btree, rootBlkno);

	return gdi;
}

/*
 * Inserts array of item pointers, may execute several tree scan (very rare)
 */
void
insertItemPointer(GinPostingTreeScan *gdi, ItemPointerData *items, uint32 nitem)
{
	BlockNumber rootBlkno = gdi->stack->blkno;

	gdi->btree.items = items;
	gdi->btree.nitem = nitem;
	gdi->btree.curitem = 0;

	while (gdi->btree.curitem < gdi->btree.nitem)
	{
		if (!gdi->stack)
			gdi->stack = ginPrepareFindLeafPage(&gdi->btree, rootBlkno);

		gdi->stack = ginFindLeafPage(&gdi->btree, gdi->stack);

		if (gdi->btree.findItem(&(gdi->btree), gdi->stack))
			elog(ERROR, "item pointer (%u,%d) already exists",
			ItemPointerGetBlockNumber(gdi->btree.items + gdi->btree.curitem),
				 ItemPointerGetOffsetNumber(gdi->btree.items + gdi->btree.curitem));

		ginInsertValue(&(gdi->btree), gdi->stack);

		gdi->stack = NULL;
	}
}

Buffer
scanBeginPostingTree(GinPostingTreeScan *gdi)
{
	gdi->stack = ginFindLeafPage(&gdi->btree, gdi->stack);
	return gdi->stack->buffer;
}
