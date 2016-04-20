/*-------------------------------------------------------------------------
 *
 * ginentrypage.c
 *	  routines for handling GIN entry tree pages.
 *
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gin/ginentrypage.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin_private.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "utils/rel.h"

static void entrySplitPage(GinBtree btree, Buffer origbuf,
			   GinBtreeStack *stack,
			   GinBtreeEntryInsertData *insertData,
			   BlockNumber updateblkno,
			   Page *newlpage, Page *newrpage);

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
 * entry, or pending-list entry, it should pass dataSize = 0 and then overwrite
 * the t_tid fields as necessary.  In any case, 'data' can be NULL to skip
 * filling in the posting list; the caller is responsible for filling it
 * afterwards if data = NULL and nipd > 0.
 */
IndexTuple
GinFormTuple(GinState *ginstate,
			 OffsetNumber attnum, Datum key, GinNullCategory category,
			 Pointer data, Size dataSize, int nipd,
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
	newsize += dataSize;

	newsize = MAXALIGN(newsize);

	if (newsize > GinMaxItemSize)
	{
		if (errorTooBig)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
			errmsg("index row size %zu exceeds maximum %zu for index \"%s\"",
				   (Size) newsize, (Size) GinMaxItemSize,
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

		/*
		 * PostgreSQL 9.3 and earlier did not clear this new space, so we
		 * might find uninitialized padding when reading tuples from disk.
		 */
		memset((char *) itup + IndexTupleSize(itup),
			   0, newsize - IndexTupleSize(itup));
		/* set new size in tuple header */
		itup->t_info &= ~INDEX_SIZE_MASK;
		itup->t_info |= newsize;
	}

	/*
	 * Copy in the posting list, if provided
	 */
	if (data)
	{
		char	   *ptr = GinGetPosting(itup);

		memcpy(ptr, data, dataSize);
	}

	/*
	 * Insert category byte, if needed
	 */
	if (category != GIN_CAT_NORM_KEY)
	{
		Assert(IndexTupleHasNulls(itup));
		GinSetNullCategory(itup, ginstate, category);
	}
	return itup;
}

/*
 * Read item pointers from leaf entry tuple.
 *
 * Returns a palloc'd array of ItemPointers. The number of items is returned
 * in *nitems.
 */
ItemPointer
ginReadTuple(GinState *ginstate, OffsetNumber attnum, IndexTuple itup,
			 int *nitems)
{
	Pointer		ptr = GinGetPosting(itup);
	int			nipd = GinGetNPosting(itup);
	ItemPointer ipd;
	int			ndecoded;

	if (GinItupIsCompressed(itup))
	{
		if (nipd > 0)
		{
			ipd = ginPostingListDecode((GinPostingList *) ptr, &ndecoded);
			if (nipd != ndecoded)
				elog(ERROR, "number of items mismatch in GIN entry tuple, %d in tuple header, %d decoded",
					 nipd, ndecoded);
		}
		else
		{
			ipd = palloc(0);
		}
	}
	else
	{
		ipd = (ItemPointer) palloc(sizeof(ItemPointerData) * nipd);
		memcpy(ipd, ptr, sizeof(ItemPointerData) * nipd);
	}
	*nitems = nipd;
	return ipd;
}

/*
 * Form a non-leaf entry tuple by copying the key data from the given tuple,
 * which can be either a leaf or non-leaf entry tuple.
 *
 * Any posting list in the source tuple is not copied.  The specified child
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
		return btree->getLeftMostChild(btree, page);
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
entryIsEnoughSpace(GinBtree btree, Buffer buf, OffsetNumber off,
				   GinBtreeEntryInsertData *insertData)
{
	Size		releasedsz = 0;
	Size		addedsz;
	Page		page = BufferGetPage(buf);

	Assert(insertData->entry);
	Assert(!GinPageIsData(page));

	if (insertData->isDelete)
	{
		IndexTuple	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, off));

		releasedsz = MAXALIGN(IndexTupleSize(itup)) + sizeof(ItemIdData);
	}

	addedsz = MAXALIGN(IndexTupleSize(insertData->entry)) + sizeof(ItemIdData);

	if (PageGetFreeSpace(page) + releasedsz >= addedsz)
		return true;

	return false;
}

/*
 * Delete tuple on leaf page if tuples existed and we
 * should update it, update old child blkno to new right page
 * if child split occurred
 */
static void
entryPreparePage(GinBtree btree, Page page, OffsetNumber off,
				 GinBtreeEntryInsertData *insertData, BlockNumber updateblkno)
{
	Assert(insertData->entry);
	Assert(!GinPageIsData(page));

	if (insertData->isDelete)
	{
		Assert(GinPageIsLeaf(page));
		PageIndexTupleDelete(page, off);
	}

	if (!GinPageIsLeaf(page) && updateblkno != InvalidBlockNumber)
	{
		IndexTuple	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, off));

		GinSetDownlink(itup, updateblkno);
	}
}

/*
 * Prepare to insert data on an entry page.
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
entryBeginPlaceToPage(GinBtree btree, Buffer buf, GinBtreeStack *stack,
					  void *insertPayload, BlockNumber updateblkno,
					  void **ptp_workspace,
					  Page *newlpage, Page *newrpage)
{
	GinBtreeEntryInsertData *insertData = insertPayload;
	OffsetNumber off = stack->off;

	/* If it doesn't fit, deal with split case */
	if (!entryIsEnoughSpace(btree, buf, off, insertData))
	{
		entrySplitPage(btree, buf, stack, insertData, updateblkno,
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
entryExecPlaceToPage(GinBtree btree, Buffer buf, GinBtreeStack *stack,
					 void *insertPayload, BlockNumber updateblkno,
					 void *ptp_workspace)
{
	GinBtreeEntryInsertData *insertData = insertPayload;
	Page		page = BufferGetPage(buf);
	OffsetNumber off = stack->off;
	OffsetNumber placed;

	entryPreparePage(btree, page, off, insertData, updateblkno);

	placed = PageAddItem(page,
						 (Item) insertData->entry,
						 IndexTupleSize(insertData->entry),
						 off, false, false);
	if (placed != off)
		elog(ERROR, "failed to add item to index page in \"%s\"",
			 RelationGetRelationName(btree->index));

	if (RelationNeedsWAL(btree->index))
	{
		/*
		 * This must be static, because it has to survive until XLogInsert,
		 * and we can't palloc here.  Ugly, but the XLogInsert infrastructure
		 * isn't reentrant anyway.
		 */
		static ginxlogInsertEntry data;

		data.isDelete = insertData->isDelete;
		data.offset = off;

		XLogRegisterBufData(0, (char *) &data,
							offsetof(ginxlogInsertEntry, tuple));
		XLogRegisterBufData(0, (char *) insertData->entry,
							IndexTupleSize(insertData->entry));
	}
}

/*
 * Split entry page and insert new data.
 *
 * Returns new temp pages to *newlpage and *newrpage.
 * The original buffer is left untouched.
 */
static void
entrySplitPage(GinBtree btree, Buffer origbuf,
			   GinBtreeStack *stack,
			   GinBtreeEntryInsertData *insertData,
			   BlockNumber updateblkno,
			   Page *newlpage, Page *newrpage)
{
	OffsetNumber off = stack->off;
	OffsetNumber i,
				maxoff,
				separator = InvalidOffsetNumber;
	Size		totalsize = 0;
	Size		lsize = 0,
				size;
	char	   *ptr;
	IndexTuple	itup;
	Page		page;
	Page		lpage = PageGetTempPageCopy(BufferGetPage(origbuf));
	Page		rpage = PageGetTempPageCopy(BufferGetPage(origbuf));
	Size		pageSize = PageGetPageSize(lpage);
	char		tupstore[2 * BLCKSZ];

	entryPreparePage(btree, lpage, off, insertData, updateblkno);

	/*
	 * First, append all the existing tuples and the new tuple we're inserting
	 * one after another in a temporary workspace.
	 */
	maxoff = PageGetMaxOffsetNumber(lpage);
	ptr = tupstore;
	for (i = FirstOffsetNumber; i <= maxoff; i++)
	{
		if (i == off)
		{
			size = MAXALIGN(IndexTupleSize(insertData->entry));
			memcpy(ptr, insertData->entry, size);
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
		size = MAXALIGN(IndexTupleSize(insertData->entry));
		memcpy(ptr, insertData->entry, size);
		ptr += size;
		totalsize += size + sizeof(ItemIdData);
	}

	/*
	 * Initialize the left and right pages, and copy all the tuples back to
	 * them.
	 */
	GinInitPage(rpage, GinPageGetOpaque(lpage)->flags, pageSize);
	GinInitPage(lpage, GinPageGetOpaque(rpage)->flags, pageSize);

	ptr = tupstore;
	maxoff++;
	lsize = 0;

	page = lpage;
	for (i = FirstOffsetNumber; i <= maxoff; i++)
	{
		itup = (IndexTuple) ptr;

		/*
		 * Decide where to split.  We try to equalize the pages' total data
		 * size, not number of tuples.
		 */
		if (lsize > totalsize / 2)
		{
			if (separator == InvalidOffsetNumber)
				separator = i - 1;
			page = rpage;
		}
		else
		{
			lsize += MAXALIGN(IndexTupleSize(itup)) + sizeof(ItemIdData);
		}

		if (PageAddItem(page, (Item) itup, IndexTupleSize(itup), InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
			elog(ERROR, "failed to add item to index page in \"%s\"",
				 RelationGetRelationName(btree->index));
		ptr += MAXALIGN(IndexTupleSize(itup));
	}

	/* return temp pages to caller */
	*newlpage = lpage;
	*newrpage = rpage;
}

/*
 * Construct insertion payload for inserting the downlink for given buffer.
 */
static void *
entryPrepareDownlink(GinBtree btree, Buffer lbuf)
{
	GinBtreeEntryInsertData *insertData;
	Page		lpage = BufferGetPage(lbuf);
	BlockNumber lblkno = BufferGetBlockNumber(lbuf);
	IndexTuple	itup;

	itup = getRightMostTuple(lpage);

	insertData = palloc(sizeof(GinBtreeEntryInsertData));
	insertData->entry = GinFormInteriorTuple(itup, lpage, lblkno);
	insertData->isDelete = false;

	return insertData;
}

/*
 * Fills new root by rightest values from child.
 * Also called from ginxlog, should not use btree
 */
void
ginEntryFillRoot(GinBtree btree, Page root,
				 BlockNumber lblkno, Page lpage,
				 BlockNumber rblkno, Page rpage)
{
	IndexTuple	itup;

	itup = GinFormInteriorTuple(getRightMostTuple(lpage), lpage, lblkno);
	if (PageAddItem(root, (Item) itup, IndexTupleSize(itup), InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
		elog(ERROR, "failed to add item to index root page");
	pfree(itup);

	itup = GinFormInteriorTuple(getRightMostTuple(rpage), rpage, rblkno);
	if (PageAddItem(root, (Item) itup, IndexTupleSize(itup), InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
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
	btree->rootBlkno = GIN_ROOT_BLKNO;
	btree->ginstate = ginstate;

	btree->findChildPage = entryLocateEntry;
	btree->getLeftMostChild = entryGetLeftMostPage;
	btree->isMoveRight = entryIsMoveRight;
	btree->findItem = entryLocateLeafEntry;
	btree->findChildPtr = entryFindChildPtr;
	btree->beginPlaceToPage = entryBeginPlaceToPage;
	btree->execPlaceToPage = entryExecPlaceToPage;
	btree->fillRoot = ginEntryFillRoot;
	btree->prepareDownlink = entryPrepareDownlink;

	btree->isData = FALSE;
	btree->fullScan = FALSE;
	btree->isBuild = FALSE;

	btree->entryAttnum = attnum;
	btree->entryKey = key;
	btree->entryCategory = category;
}
