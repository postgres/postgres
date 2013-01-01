/*-------------------------------------------------------------------------
 *
 * ginentrypage.c
 *	  page utilities routines for the postgres inverted index access method.
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gin/ginentrypage.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin_private.h"
#include "utils/rel.h"

/*
 * Form a tuple for entry tree.
 *
 * If the tuple would be too big to be stored, function throws a suitable
 * error if errorTooBig is TRUE, or returns NULL if errorTooBig is FALSE.
 *
 * See src/backend/access/gin/README for a description of the index tuple
 * format that is being built here.  We build on the assumption that we
 * are making a leaf-level key entry containing a posting list of nipd items.
 * If the caller is actually trying to make a posting-tree entry, non-leaf
 * entry, or pending-list entry, it should pass nipd = 0 and then overwrite
 * the t_tid fields as necessary.  In any case, ipd can be NULL to skip
 * copying any itempointers into the posting list; the caller is responsible
 * for filling the posting list afterwards, if ipd = NULL and nipd > 0.
 */
IndexTuple
GinFormTuple(GinState *ginstate,
			 OffsetNumber attnum, Datum key, GinNullCategory category,
			 ItemPointerData *ipd, uint32 nipd,
			 bool errorTooBig)
{
	Datum		datums[2];
	bool		isnull[2];
	IndexTuple	itup;
	uint32		newsize;

	/* Build the basic tuple: optional column number, plus key datum */
	if (ginstate->oneCol)
	{
		datums[0] = key;
		isnull[0] = (category != GIN_CAT_NORM_KEY);
	}
	else
	{
		datums[0] = UInt16GetDatum(attnum);
		isnull[0] = false;
		datums[1] = key;
		isnull[1] = (category != GIN_CAT_NORM_KEY);
	}

	itup = index_form_tuple(ginstate->tupdesc[attnum - 1], datums, isnull);

	/*
	 * Determine and store offset to the posting list, making sure there is
	 * room for the category byte if needed.
	 *
	 * Note: because index_form_tuple MAXALIGNs the tuple size, there may well
	 * be some wasted pad space.  Is it worth recomputing the data length to
	 * prevent that?  That would also allow us to Assert that the real data
	 * doesn't overlap the GinNullCategory byte, which this code currently
	 * takes on faith.
	 */
	newsize = IndexTupleSize(itup);

	if (IndexTupleHasNulls(itup))
	{
		uint32		minsize;

		Assert(category != GIN_CAT_NORM_KEY);
		minsize = GinCategoryOffset(itup, ginstate) + sizeof(GinNullCategory);
		newsize = Max(newsize, minsize);
	}

	newsize = SHORTALIGN(newsize);

	GinSetPostingOffset(itup, newsize);

	GinSetNPosting(itup, nipd);

	/*
	 * Add space needed for posting list, if any.  Then check that the tuple
	 * won't be too big to store.
	 */
	newsize += sizeof(ItemPointerData) * nipd;
	newsize = MAXALIGN(newsize);
	if (newsize > Min(INDEX_SIZE_MASK, GinMaxItemSize))
	{
		if (errorTooBig)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
			errmsg("index row size %lu exceeds maximum %lu for index \"%s\"",
				   (unsigned long) newsize,
				   (unsigned long) Min(INDEX_SIZE_MASK,
									   GinMaxItemSize),
				   RelationGetRelationName(ginstate->index))));
		pfree(itup);
		return NULL;
	}

	/*
	 * Resize tuple if needed
	 */
	if (newsize != IndexTupleSize(itup))
	{
		itup = repalloc(itup, newsize);

		/* set new size in tuple header */
		itup->t_info &= ~INDEX_SIZE_MASK;
		itup->t_info |= newsize;
	}

	/*
	 * Insert category byte, if needed
	 */
	if (category != GIN_CAT_NORM_KEY)
	{
		Assert(IndexTupleHasNulls(itup));
		GinSetNullCategory(itup, ginstate, category);
	}

	/*
	 * Copy in the posting list, if provided
	 */
	if (ipd)
		memcpy(GinGetPosting(itup), ipd, sizeof(ItemPointerData) * nipd);

	return itup;
}

/*
 * Sometimes we reduce the number of posting list items in a tuple after
 * having built it with GinFormTuple.  This function adjusts the size
 * fields to match.
 */
void
GinShortenTuple(IndexTuple itup, uint32 nipd)
{
	uint32		newsize;

	Assert(nipd <= GinGetNPosting(itup));

	newsize = GinGetPostingOffset(itup) + sizeof(ItemPointerData) * nipd;
	newsize = MAXALIGN(newsize);

	Assert(newsize <= (itup->t_info & INDEX_SIZE_MASK));

	itup->t_info &= ~INDEX_SIZE_MASK;
	itup->t_info |= newsize;

	GinSetNPosting(itup, nipd);
}

/*
 * Form a non-leaf entry tuple by copying the key data from the given tuple,
 * which can be either a leaf or non-leaf entry tuple.
 *
 * Any posting list in the source tuple is not copied.	The specified child
 * block number is inserted into t_tid.
 */
static IndexTuple
GinFormInteriorTuple(IndexTuple itup, Page page, BlockNumber childblk)
{
	IndexTuple	nitup;

	if (GinPageIsLeaf(page) && !GinIsPostingTree(itup))
	{
		/* Tuple contains a posting list, just copy stuff before that */
		uint32		origsize = GinGetPostingOffset(itup);

		origsize = MAXALIGN(origsize);
		nitup = (IndexTuple) palloc(origsize);
		memcpy(nitup, itup, origsize);
		/* ... be sure to fix the size header field ... */
		nitup->t_info &= ~INDEX_SIZE_MASK;
		nitup->t_info |= origsize;
	}
	else
	{
		/* Copy the tuple as-is */
		nitup = (IndexTuple) palloc(IndexTupleSize(itup));
		memcpy(nitup, itup, IndexTupleSize(itup));
	}

	/* Now insert the correct downlink */
	GinSetDownlink(nitup, childblk);

	return nitup;
}

/*
 * Entry tree is a "static", ie tuple never deletes from it,
 * so we don't use right bound, we use rightmost key instead.
 */
static IndexTuple
getRightMostTuple(Page page)
{
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

	return (IndexTuple) PageGetItem(page, PageGetItemId(page, maxoff));
}

static bool
entryIsMoveRight(GinBtree btree, Page page)
{
	IndexTuple	itup;
	OffsetNumber attnum;
	Datum		key;
	GinNullCategory category;

	if (GinPageRightMost(page))
		return FALSE;

	itup = getRightMostTuple(page);
	attnum = gintuple_get_attrnum(btree->ginstate, itup);
	key = gintuple_get_key(btree->ginstate, itup, &category);

	if (ginCompareAttEntries(btree->ginstate,
				   btree->entryAttnum, btree->entryKey, btree->entryCategory,
							 attnum, key, category) > 0)
		return TRUE;

	return FALSE;
}

/*
 * Find correct tuple in non-leaf page. It supposed that
 * page correctly chosen and searching value SHOULD be on page
 */
static BlockNumber
entryLocateEntry(GinBtree btree, GinBtreeStack *stack)
{
	OffsetNumber low,
				high,
				maxoff;
	IndexTuple	itup = NULL;
	int			result;
	Page		page = BufferGetPage(stack->buffer);

	Assert(!GinPageIsLeaf(page));
	Assert(!GinPageIsData(page));

	if (btree->fullScan)
	{
		stack->off = FirstOffsetNumber;
		stack->predictNumber *= PageGetMaxOffsetNumber(page);
		return btree->getLeftMostPage(btree, page);
	}

	low = FirstOffsetNumber;
	maxoff = high = PageGetMaxOffsetNumber(page);
	Assert(high >= low);

	high++;

	while (high > low)
	{
		OffsetNumber mid = low + ((high - low) / 2);

		if (mid == maxoff && GinPageRightMost(page))
		{
			/* Right infinity */
			result = -1;
		}
		else
		{
			OffsetNumber attnum;
			Datum		key;
			GinNullCategory category;

			itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, mid));
			attnum = gintuple_get_attrnum(btree->ginstate, itup);
			key = gintuple_get_key(btree->ginstate, itup, &category);
			result = ginCompareAttEntries(btree->ginstate,
										  btree->entryAttnum,
										  btree->entryKey,
										  btree->entryCategory,
										  attnum, key, category);
		}

		if (result == 0)
		{
			stack->off = mid;
			Assert(GinGetDownlink(itup) != GIN_ROOT_BLKNO);
			return GinGetDownlink(itup);
		}
		else if (result > 0)
			low = mid + 1;
		else
			high = mid;
	}

	Assert(high >= FirstOffsetNumber && high <= maxoff);

	stack->off = high;
	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, high));
	Assert(GinGetDownlink(itup) != GIN_ROOT_BLKNO);
	return GinGetDownlink(itup);
}

/*
 * Searches correct position for value on leaf page.
 * Page should be correctly chosen.
 * Returns true if value found on page.
 */
static bool
entryLocateLeafEntry(GinBtree btree, GinBtreeStack *stack)
{
	Page		page = BufferGetPage(stack->buffer);
	OffsetNumber low,
				high;

	Assert(GinPageIsLeaf(page));
	Assert(!GinPageIsData(page));

	if (btree->fullScan)
	{
		stack->off = FirstOffsetNumber;
		return TRUE;
	}

	low = FirstOffsetNumber;
	high = PageGetMaxOffsetNumber(page);

	if (high < low)
	{
		stack->off = FirstOffsetNumber;
		return false;
	}

	high++;

	while (high > low)
	{
		OffsetNumber mid = low + ((high - low) / 2);
		IndexTuple	itup;
		OffsetNumber attnum;
		Datum		key;
		GinNullCategory category;
		int			result;

		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, mid));
		attnum = gintuple_get_attrnum(btree->ginstate, itup);
		key = gintuple_get_key(btree->ginstate, itup, &category);
		result = ginCompareAttEntries(btree->ginstate,
									  btree->entryAttnum,
									  btree->entryKey,
									  btree->entryCategory,
									  attnum, key, category);
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

static OffsetNumber
entryFindChildPtr(GinBtree btree, Page page, BlockNumber blkno, OffsetNumber storedOff)
{
	OffsetNumber i,
				maxoff = PageGetMaxOffsetNumber(page);
	IndexTuple	itup;

	Assert(!GinPageIsLeaf(page));
	Assert(!GinPageIsData(page));

	/* if page isn't changed, we returns storedOff */
	if (storedOff >= FirstOffsetNumber && storedOff <= maxoff)
	{
		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, storedOff));
		if (GinGetDownlink(itup) == blkno)
			return storedOff;

		/*
		 * we hope, that needed pointer goes to right. It's true if there
		 * wasn't a deletion
		 */
		for (i = storedOff + 1; i <= maxoff; i++)
		{
			itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, i));
			if (GinGetDownlink(itup) == blkno)
				return i;
		}
		maxoff = storedOff - 1;
	}

	/* last chance */
	for (i = FirstOffsetNumber; i <= maxoff; i++)
	{
		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, i));
		if (GinGetDownlink(itup) == blkno)
			return i;
	}

	return InvalidOffsetNumber;
}

static BlockNumber
entryGetLeftMostPage(GinBtree btree, Page page)
{
	IndexTuple	itup;

	Assert(!GinPageIsLeaf(page));
	Assert(!GinPageIsData(page));
	Assert(PageGetMaxOffsetNumber(page) >= FirstOffsetNumber);

	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, FirstOffsetNumber));
	return GinGetDownlink(itup);
}

static bool
entryIsEnoughSpace(GinBtree btree, Buffer buf, OffsetNumber off)
{
	Size		itupsz = 0;
	Page		page = BufferGetPage(buf);

	Assert(btree->entry);
	Assert(!GinPageIsData(page));

	if (btree->isDelete)
	{
		IndexTuple	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, off));

		itupsz = MAXALIGN(IndexTupleSize(itup)) + sizeof(ItemIdData);
	}

	if (PageGetFreeSpace(page) + itupsz >= MAXALIGN(IndexTupleSize(btree->entry)) + sizeof(ItemIdData))
		return true;

	return false;
}

/*
 * Delete tuple on leaf page if tuples existed and we
 * should update it, update old child blkno to new right page
 * if child split occurred
 */
static BlockNumber
entryPreparePage(GinBtree btree, Page page, OffsetNumber off)
{
	BlockNumber ret = InvalidBlockNumber;

	Assert(btree->entry);
	Assert(!GinPageIsData(page));

	if (btree->isDelete)
	{
		Assert(GinPageIsLeaf(page));
		PageIndexTupleDelete(page, off);
	}

	if (!GinPageIsLeaf(page) && btree->rightblkno != InvalidBlockNumber)
	{
		IndexTuple	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, off));

		GinSetDownlink(itup, btree->rightblkno);
		ret = btree->rightblkno;
	}

	btree->rightblkno = InvalidBlockNumber;

	return ret;
}

/*
 * Place tuple on page and fills WAL record
 */
static void
entryPlaceToPage(GinBtree btree, Buffer buf, OffsetNumber off, XLogRecData **prdata)
{
	Page		page = BufferGetPage(buf);
	OffsetNumber placed;
	int			cnt = 0;

	/* these must be static so they can be returned to caller */
	static XLogRecData rdata[3];
	static ginxlogInsert data;

	*prdata = rdata;
	data.updateBlkno = entryPreparePage(btree, page, off);

	placed = PageAddItem(page, (Item) btree->entry, IndexTupleSize(btree->entry), off, false, false);
	if (placed != off)
		elog(ERROR, "failed to add item to index page in \"%s\"",
			 RelationGetRelationName(btree->index));

	data.node = btree->index->rd_node;
	data.blkno = BufferGetBlockNumber(buf);
	data.offset = off;
	data.nitem = 1;
	data.isDelete = btree->isDelete;
	data.isData = false;
	data.isLeaf = GinPageIsLeaf(page) ? TRUE : FALSE;

	/*
	 * Prevent full page write if child's split occurs. That is needed to
	 * remove incomplete splits while replaying WAL
	 *
	 * data.updateBlkno contains new block number (of newly created right
	 * page) for recently splited page.
	 */
	if (data.updateBlkno == InvalidBlockNumber)
	{
		rdata[0].buffer = buf;
		rdata[0].buffer_std = TRUE;
		rdata[0].data = NULL;
		rdata[0].len = 0;
		rdata[0].next = &rdata[1];
		cnt++;
	}

	rdata[cnt].buffer = InvalidBuffer;
	rdata[cnt].data = (char *) &data;
	rdata[cnt].len = sizeof(ginxlogInsert);
	rdata[cnt].next = &rdata[cnt + 1];
	cnt++;

	rdata[cnt].buffer = InvalidBuffer;
	rdata[cnt].data = (char *) btree->entry;
	rdata[cnt].len = IndexTupleSize(btree->entry);
	rdata[cnt].next = NULL;

	btree->entry = NULL;
}

/*
 * Place tuple and split page, original buffer(lbuf) leaves untouched,
 * returns shadow page of lbuf filled new data.
 * Tuples are distributed between pages by equal size on its, not
 * an equal number!
 */
static Page
entrySplitPage(GinBtree btree, Buffer lbuf, Buffer rbuf, OffsetNumber off, XLogRecData **prdata)
{
	OffsetNumber i,
				maxoff,
				separator = InvalidOffsetNumber;
	Size		totalsize = 0;
	Size		lsize = 0,
				size;
	char	   *ptr;
	IndexTuple	itup,
				leftrightmost = NULL;
	Page		page;
	Page		lpage = PageGetTempPageCopy(BufferGetPage(lbuf));
	Page		rpage = BufferGetPage(rbuf);
	Size		pageSize = PageGetPageSize(lpage);

	/* these must be static so they can be returned to caller */
	static XLogRecData rdata[2];
	static ginxlogSplit data;
	static char tupstore[2 * BLCKSZ];

	*prdata = rdata;
	data.leftChildBlkno = (GinPageIsLeaf(lpage)) ?
		InvalidOffsetNumber : GinGetDownlink(btree->entry);
	data.updateBlkno = entryPreparePage(btree, lpage, off);

	maxoff = PageGetMaxOffsetNumber(lpage);
	ptr = tupstore;

	for (i = FirstOffsetNumber; i <= maxoff; i++)
	{
		if (i == off)
		{
			size = MAXALIGN(IndexTupleSize(btree->entry));
			memcpy(ptr, btree->entry, size);
			ptr += size;
			totalsize += size + sizeof(ItemIdData);
		}

		itup = (IndexTuple) PageGetItem(lpage, PageGetItemId(lpage, i));
		size = MAXALIGN(IndexTupleSize(itup));
		memcpy(ptr, itup, size);
		ptr += size;
		totalsize += size + sizeof(ItemIdData);
	}

	if (off == maxoff + 1)
	{
		size = MAXALIGN(IndexTupleSize(btree->entry));
		memcpy(ptr, btree->entry, size);
		ptr += size;
		totalsize += size + sizeof(ItemIdData);
	}

	GinInitPage(rpage, GinPageGetOpaque(lpage)->flags, pageSize);
	GinInitPage(lpage, GinPageGetOpaque(rpage)->flags, pageSize);

	ptr = tupstore;
	maxoff++;
	lsize = 0;

	page = lpage;
	for (i = FirstOffsetNumber; i <= maxoff; i++)
	{
		itup = (IndexTuple) ptr;

		if (lsize > totalsize / 2)
		{
			if (separator == InvalidOffsetNumber)
				separator = i - 1;
			page = rpage;
		}
		else
		{
			leftrightmost = itup;
			lsize += MAXALIGN(IndexTupleSize(itup)) + sizeof(ItemIdData);
		}

		if (PageAddItem(page, (Item) itup, IndexTupleSize(itup), InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
			elog(ERROR, "failed to add item to index page in \"%s\"",
				 RelationGetRelationName(btree->index));
		ptr += MAXALIGN(IndexTupleSize(itup));
	}

	btree->entry = GinFormInteriorTuple(leftrightmost, lpage,
										BufferGetBlockNumber(lbuf));

	btree->rightblkno = BufferGetBlockNumber(rbuf);

	data.node = btree->index->rd_node;
	data.rootBlkno = InvalidBlockNumber;
	data.lblkno = BufferGetBlockNumber(lbuf);
	data.rblkno = BufferGetBlockNumber(rbuf);
	data.separator = separator;
	data.nitem = maxoff;
	data.isData = FALSE;
	data.isLeaf = GinPageIsLeaf(lpage) ? TRUE : FALSE;
	data.isRootSplit = FALSE;

	rdata[0].buffer = InvalidBuffer;
	rdata[0].data = (char *) &data;
	rdata[0].len = sizeof(ginxlogSplit);
	rdata[0].next = &rdata[1];

	rdata[1].buffer = InvalidBuffer;
	rdata[1].data = tupstore;
	rdata[1].len = MAXALIGN(totalsize);
	rdata[1].next = NULL;

	return lpage;
}

/*
 * return newly allocated rightmost tuple
 */
IndexTuple
ginPageGetLinkItup(Buffer buf)
{
	IndexTuple	itup,
				nitup;
	Page		page = BufferGetPage(buf);

	itup = getRightMostTuple(page);
	nitup = GinFormInteriorTuple(itup, page, BufferGetBlockNumber(buf));

	return nitup;
}

/*
 * Fills new root by rightest values from child.
 * Also called from ginxlog, should not use btree
 */
void
ginEntryFillRoot(GinBtree btree, Buffer root, Buffer lbuf, Buffer rbuf)
{
	Page		page;
	IndexTuple	itup;

	page = BufferGetPage(root);

	itup = ginPageGetLinkItup(lbuf);
	if (PageAddItem(page, (Item) itup, IndexTupleSize(itup), InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
		elog(ERROR, "failed to add item to index root page");
	pfree(itup);

	itup = ginPageGetLinkItup(rbuf);
	if (PageAddItem(page, (Item) itup, IndexTupleSize(itup), InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
		elog(ERROR, "failed to add item to index root page");
	pfree(itup);
}

/*
 * Set up GinBtree for entry page access
 *
 * Note: during WAL recovery, there may be no valid data in ginstate
 * other than a faked-up Relation pointer; the key datum is bogus too.
 */
void
ginPrepareEntryScan(GinBtree btree, OffsetNumber attnum,
					Datum key, GinNullCategory category,
					GinState *ginstate)
{
	memset(btree, 0, sizeof(GinBtreeData));

	btree->index = ginstate->index;
	btree->ginstate = ginstate;

	btree->findChildPage = entryLocateEntry;
	btree->isMoveRight = entryIsMoveRight;
	btree->findItem = entryLocateLeafEntry;
	btree->findChildPtr = entryFindChildPtr;
	btree->getLeftMostPage = entryGetLeftMostPage;
	btree->isEnoughSpace = entryIsEnoughSpace;
	btree->placeToPage = entryPlaceToPage;
	btree->splitPage = entrySplitPage;
	btree->fillRoot = ginEntryFillRoot;

	btree->isData = FALSE;
	btree->searchMode = FALSE;
	btree->fullScan = FALSE;
	btree->isBuild = FALSE;

	btree->entryAttnum = attnum;
	btree->entryKey = key;
	btree->entryCategory = category;
	btree->isDelete = FALSE;
}
