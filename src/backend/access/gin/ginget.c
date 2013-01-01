/*-------------------------------------------------------------------------
 *
 * ginget.c
 *	  fetch tuples from a GIN scan.
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gin/ginget.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin_private.h"
#include "access/relscan.h"
#include "miscadmin.h"
#include "utils/datum.h"
#include "utils/memutils.h"


typedef struct pendingPosition
{
	Buffer		pendingBuffer;
	OffsetNumber firstOffset;
	OffsetNumber lastOffset;
	ItemPointerData item;
	bool	   *hasMatchKey;
} pendingPosition;


/*
 * Convenience function for invoking a key's consistentFn
 */
static bool
callConsistentFn(GinState *ginstate, GinScanKey key)
{
	/*
	 * If we're dealing with a dummy EVERYTHING key, we don't want to call the
	 * consistentFn; just claim it matches.
	 */
	if (key->searchMode == GIN_SEARCH_MODE_EVERYTHING)
	{
		key->recheckCurItem = false;
		return true;
	}

	/*
	 * Initialize recheckCurItem in case the consistentFn doesn't know it
	 * should set it.  The safe assumption in that case is to force recheck.
	 */
	key->recheckCurItem = true;

	return DatumGetBool(FunctionCall8Coll(&ginstate->consistentFn[key->attnum - 1],
								 ginstate->supportCollation[key->attnum - 1],
										  PointerGetDatum(key->entryRes),
										  UInt16GetDatum(key->strategy),
										  key->query,
										  UInt32GetDatum(key->nuserentries),
										  PointerGetDatum(key->extra_data),
									   PointerGetDatum(&key->recheckCurItem),
										  PointerGetDatum(key->queryValues),
									 PointerGetDatum(key->queryCategories)));
}

/*
 * Tries to refind previously taken ItemPointer on a posting page.
 */
static bool
findItemInPostingPage(Page page, ItemPointer item, OffsetNumber *off)
{
	OffsetNumber maxoff = GinPageGetOpaque(page)->maxoff;
	int			res;

	if (GinPageGetOpaque(page)->flags & GIN_DELETED)
		/* page was deleted by concurrent vacuum */
		return false;

	/*
	 * scan page to find equal or first greater value
	 */
	for (*off = FirstOffsetNumber; *off <= maxoff; (*off)++)
	{
		res = ginCompareItemPointers(item, (ItemPointer) GinDataPageGetItem(page, *off));

		if (res <= 0)
			return true;
	}

	return false;
}

/*
 * Goes to the next page if current offset is outside of bounds
 */
static bool
moveRightIfItNeeded(GinBtreeData *btree, GinBtreeStack *stack)
{
	Page		page = BufferGetPage(stack->buffer);

	if (stack->off > PageGetMaxOffsetNumber(page))
	{
		/*
		 * We scanned the whole page, so we should take right page
		 */
		stack->blkno = GinPageGetOpaque(page)->rightlink;

		if (GinPageRightMost(page))
			return false;		/* no more pages */

		LockBuffer(stack->buffer, GIN_UNLOCK);
		stack->buffer = ReleaseAndReadBuffer(stack->buffer,
											 btree->index,
											 stack->blkno);
		LockBuffer(stack->buffer, GIN_SHARE);
		stack->off = FirstOffsetNumber;
	}

	return true;
}

/*
 * Scan all pages of a posting tree and save all its heap ItemPointers
 * in scanEntry->matchBitmap
 */
static void
scanPostingTree(Relation index, GinScanEntry scanEntry,
				BlockNumber rootPostingTree)
{
	GinPostingTreeScan *gdi;
	Buffer		buffer;
	Page		page;
	BlockNumber blkno;

	/* Descend to the leftmost leaf page */
	gdi = ginPrepareScanPostingTree(index, rootPostingTree, TRUE);

	buffer = ginScanBeginPostingTree(gdi);
	IncrBufferRefCount(buffer); /* prevent unpin in freeGinBtreeStack */

	freeGinBtreeStack(gdi->stack);
	pfree(gdi);

	/*
	 * Loop iterates through all leaf pages of posting tree
	 */
	for (;;)
	{
		page = BufferGetPage(buffer);

		if ((GinPageGetOpaque(page)->flags & GIN_DELETED) == 0 &&
			GinPageGetOpaque(page)->maxoff >= FirstOffsetNumber)
		{
			tbm_add_tuples(scanEntry->matchBitmap,
				   (ItemPointer) GinDataPageGetItem(page, FirstOffsetNumber),
						   GinPageGetOpaque(page)->maxoff, false);
			scanEntry->predictNumberResult += GinPageGetOpaque(page)->maxoff;
		}

		if (GinPageRightMost(page))
			break;				/* no more pages */

		blkno = GinPageGetOpaque(page)->rightlink;
		LockBuffer(buffer, GIN_UNLOCK);
		buffer = ReleaseAndReadBuffer(buffer, index, blkno);
		LockBuffer(buffer, GIN_SHARE);
	}

	UnlockReleaseBuffer(buffer);
}

/*
 * Collects TIDs into scanEntry->matchBitmap for all heap tuples that
 * match the search entry.	This supports three different match modes:
 *
 * 1. Partial-match support: scan from current point until the
 *	  comparePartialFn says we're done.
 * 2. SEARCH_MODE_ALL: scan from current point (which should be first
 *	  key for the current attnum) until we hit null items or end of attnum
 * 3. SEARCH_MODE_EVERYTHING: scan from current point (which should be first
 *	  key for the current attnum) until we hit end of attnum
 *
 * Returns true if done, false if it's necessary to restart scan from scratch
 */
static bool
collectMatchBitmap(GinBtreeData *btree, GinBtreeStack *stack,
				   GinScanEntry scanEntry)
{
	OffsetNumber attnum;
	Form_pg_attribute attr;

	/* Initialize empty bitmap result */
	scanEntry->matchBitmap = tbm_create(work_mem * 1024L);

	/* Null query cannot partial-match anything */
	if (scanEntry->isPartialMatch &&
		scanEntry->queryCategory != GIN_CAT_NORM_KEY)
		return true;

	/* Locate tupdesc entry for key column (for attbyval/attlen data) */
	attnum = scanEntry->attnum;
	attr = btree->ginstate->origTupdesc->attrs[attnum - 1];

	for (;;)
	{
		Page		page;
		IndexTuple	itup;
		Datum		idatum;
		GinNullCategory icategory;

		/*
		 * stack->off points to the interested entry, buffer is already locked
		 */
		if (moveRightIfItNeeded(btree, stack) == false)
			return true;

		page = BufferGetPage(stack->buffer);
		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, stack->off));

		/*
		 * If tuple stores another attribute then stop scan
		 */
		if (gintuple_get_attrnum(btree->ginstate, itup) != attnum)
			return true;

		/* Safe to fetch attribute value */
		idatum = gintuple_get_key(btree->ginstate, itup, &icategory);

		/*
		 * Check for appropriate scan stop conditions
		 */
		if (scanEntry->isPartialMatch)
		{
			int32		cmp;

			/*
			 * In partial match, stop scan at any null (including
			 * placeholders); partial matches never match nulls
			 */
			if (icategory != GIN_CAT_NORM_KEY)
				return true;

			/*----------
			 * Check of partial match.
			 * case cmp == 0 => match
			 * case cmp > 0 => not match and finish scan
			 * case cmp < 0 => not match and continue scan
			 *----------
			 */
			cmp = DatumGetInt32(FunctionCall4Coll(&btree->ginstate->comparePartialFn[attnum - 1],
							   btree->ginstate->supportCollation[attnum - 1],
												  scanEntry->queryKey,
												  idatum,
										 UInt16GetDatum(scanEntry->strategy),
									PointerGetDatum(scanEntry->extra_data)));

			if (cmp > 0)
				return true;
			else if (cmp < 0)
			{
				stack->off++;
				continue;
			}
		}
		else if (scanEntry->searchMode == GIN_SEARCH_MODE_ALL)
		{
			/*
			 * In ALL mode, we are not interested in null items, so we can
			 * stop if we get to a null-item placeholder (which will be the
			 * last entry for a given attnum).	We do want to include NULL_KEY
			 * and EMPTY_ITEM entries, though.
			 */
			if (icategory == GIN_CAT_NULL_ITEM)
				return true;
		}

		/*
		 * OK, we want to return the TIDs listed in this entry.
		 */
		if (GinIsPostingTree(itup))
		{
			BlockNumber rootPostingTree = GinGetPostingTree(itup);

			/*
			 * We should unlock current page (but not unpin) during tree scan
			 * to prevent deadlock with vacuum processes.
			 *
			 * We save current entry value (idatum) to be able to re-find our
			 * tuple after re-locking
			 */
			if (icategory == GIN_CAT_NORM_KEY)
				idatum = datumCopy(idatum, attr->attbyval, attr->attlen);

			LockBuffer(stack->buffer, GIN_UNLOCK);

			/* Collect all the TIDs in this entry's posting tree */
			scanPostingTree(btree->index, scanEntry, rootPostingTree);

			/*
			 * We lock again the entry page and while it was unlocked insert
			 * might have occurred, so we need to re-find our position.
			 */
			LockBuffer(stack->buffer, GIN_SHARE);
			page = BufferGetPage(stack->buffer);
			if (!GinPageIsLeaf(page))
			{
				/*
				 * Root page becomes non-leaf while we unlock it. We will
				 * start again, this situation doesn't occur often - root can
				 * became a non-leaf only once per life of index.
				 */
				return false;
			}

			/* Search forward to re-find idatum */
			for (;;)
			{
				Datum		newDatum;
				GinNullCategory newCategory;

				if (moveRightIfItNeeded(btree, stack) == false)
					elog(ERROR, "lost saved point in index");	/* must not happen !!! */

				page = BufferGetPage(stack->buffer);
				itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, stack->off));

				if (gintuple_get_attrnum(btree->ginstate, itup) != attnum)
					elog(ERROR, "lost saved point in index");	/* must not happen !!! */
				newDatum = gintuple_get_key(btree->ginstate, itup,
											&newCategory);

				if (ginCompareEntries(btree->ginstate, attnum,
									  newDatum, newCategory,
									  idatum, icategory) == 0)
					break;		/* Found! */

				stack->off++;
			}

			if (icategory == GIN_CAT_NORM_KEY && !attr->attbyval)
				pfree(DatumGetPointer(idatum));
		}
		else
		{
			tbm_add_tuples(scanEntry->matchBitmap,
						   GinGetPosting(itup), GinGetNPosting(itup), false);
			scanEntry->predictNumberResult += GinGetNPosting(itup);
		}

		/*
		 * Done with this entry, go to the next
		 */
		stack->off++;
	}
}

/*
 * Start* functions setup beginning state of searches: finds correct buffer and pins it.
 */
static void
startScanEntry(GinState *ginstate, GinScanEntry entry)
{
	GinBtreeData btreeEntry;
	GinBtreeStack *stackEntry;
	Page		page;
	bool		needUnlock;

restartScanEntry:
	entry->buffer = InvalidBuffer;
	ItemPointerSetMin(&entry->curItem);
	entry->offset = InvalidOffsetNumber;
	entry->list = NULL;
	entry->nlist = 0;
	entry->matchBitmap = NULL;
	entry->matchResult = NULL;
	entry->reduceResult = FALSE;
	entry->predictNumberResult = 0;

	/*
	 * we should find entry, and begin scan of posting tree or just store
	 * posting list in memory
	 */
	ginPrepareEntryScan(&btreeEntry, entry->attnum,
						entry->queryKey, entry->queryCategory,
						ginstate);
	btreeEntry.searchMode = TRUE;
	stackEntry = ginFindLeafPage(&btreeEntry, NULL);
	page = BufferGetPage(stackEntry->buffer);
	needUnlock = TRUE;

	entry->isFinished = TRUE;

	if (entry->isPartialMatch ||
		entry->queryCategory == GIN_CAT_EMPTY_QUERY)
	{
		/*
		 * btreeEntry.findItem locates the first item >= given search key.
		 * (For GIN_CAT_EMPTY_QUERY, it will find the leftmost index item
		 * because of the way the GIN_CAT_EMPTY_QUERY category code is
		 * assigned.)  We scan forward from there and collect all TIDs needed
		 * for the entry type.
		 */
		btreeEntry.findItem(&btreeEntry, stackEntry);
		if (collectMatchBitmap(&btreeEntry, stackEntry, entry) == false)
		{
			/*
			 * GIN tree was seriously restructured, so we will cleanup all
			 * found data and rescan. See comments near 'return false' in
			 * collectMatchBitmap()
			 */
			if (entry->matchBitmap)
			{
				if (entry->matchIterator)
					tbm_end_iterate(entry->matchIterator);
				entry->matchIterator = NULL;
				tbm_free(entry->matchBitmap);
				entry->matchBitmap = NULL;
			}
			LockBuffer(stackEntry->buffer, GIN_UNLOCK);
			freeGinBtreeStack(stackEntry);
			goto restartScanEntry;
		}

		if (entry->matchBitmap && !tbm_is_empty(entry->matchBitmap))
		{
			entry->matchIterator = tbm_begin_iterate(entry->matchBitmap);
			entry->isFinished = FALSE;
		}
	}
	else if (btreeEntry.findItem(&btreeEntry, stackEntry))
	{
		IndexTuple	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, stackEntry->off));

		if (GinIsPostingTree(itup))
		{
			BlockNumber rootPostingTree = GinGetPostingTree(itup);
			GinPostingTreeScan *gdi;
			Page		page;

			/*
			 * We should unlock entry page before touching posting tree to
			 * prevent deadlocks with vacuum processes. Because entry is never
			 * deleted from page and posting tree is never reduced to the
			 * posting list, we can unlock page after getting BlockNumber of
			 * root of posting tree.
			 */
			LockBuffer(stackEntry->buffer, GIN_UNLOCK);
			needUnlock = FALSE;
			gdi = ginPrepareScanPostingTree(ginstate->index, rootPostingTree, TRUE);

			entry->buffer = ginScanBeginPostingTree(gdi);

			/*
			 * We keep buffer pinned because we need to prevent deletion of
			 * page during scan. See GIN's vacuum implementation. RefCount is
			 * increased to keep buffer pinned after freeGinBtreeStack() call.
			 */
			IncrBufferRefCount(entry->buffer);

			page = BufferGetPage(entry->buffer);
			entry->predictNumberResult = gdi->stack->predictNumber * GinPageGetOpaque(page)->maxoff;

			/*
			 * Keep page content in memory to prevent durable page locking
			 */
			entry->list = (ItemPointerData *) palloc(BLCKSZ);
			entry->nlist = GinPageGetOpaque(page)->maxoff;
			memcpy(entry->list, GinDataPageGetItem(page, FirstOffsetNumber),
				   GinPageGetOpaque(page)->maxoff * sizeof(ItemPointerData));

			LockBuffer(entry->buffer, GIN_UNLOCK);
			freeGinBtreeStack(gdi->stack);
			pfree(gdi);
			entry->isFinished = FALSE;
		}
		else if (GinGetNPosting(itup) > 0)
		{
			entry->nlist = GinGetNPosting(itup);
			entry->list = (ItemPointerData *) palloc(sizeof(ItemPointerData) * entry->nlist);
			memcpy(entry->list, GinGetPosting(itup), sizeof(ItemPointerData) * entry->nlist);
			entry->isFinished = FALSE;
		}
	}

	if (needUnlock)
		LockBuffer(stackEntry->buffer, GIN_UNLOCK);
	freeGinBtreeStack(stackEntry);
}

static void
startScanKey(GinState *ginstate, GinScanKey key)
{
	ItemPointerSetMin(&key->curItem);
	key->curItemMatches = false;
	key->recheckCurItem = false;
	key->isFinished = false;
}

static void
startScan(IndexScanDesc scan)
{
	GinScanOpaque so = (GinScanOpaque) scan->opaque;
	GinState   *ginstate = &so->ginstate;
	uint32		i;

	for (i = 0; i < so->totalentries; i++)
		startScanEntry(ginstate, so->entries[i]);

	if (GinFuzzySearchLimit > 0)
	{
		/*
		 * If all of keys more than threshold we will try to reduce result, we
		 * hope (and only hope, for intersection operation of array our
		 * supposition isn't true), that total result will not more than
		 * minimal predictNumberResult.
		 */

		for (i = 0; i < so->totalentries; i++)
			if (so->entries[i]->predictNumberResult <= so->totalentries * GinFuzzySearchLimit)
				return;

		for (i = 0; i < so->totalentries; i++)
			if (so->entries[i]->predictNumberResult > so->totalentries * GinFuzzySearchLimit)
			{
				so->entries[i]->predictNumberResult /= so->totalentries;
				so->entries[i]->reduceResult = TRUE;
			}
	}

	for (i = 0; i < so->nkeys; i++)
		startScanKey(ginstate, so->keys + i);
}

/*
 * Gets next ItemPointer from PostingTree. Note, that we copy
 * page into GinScanEntry->list array and unlock page, but keep it pinned
 * to prevent interference with vacuum
 */
static void
entryGetNextItem(GinState *ginstate, GinScanEntry entry)
{
	Page		page;
	BlockNumber blkno;

	for (;;)
	{
		if (entry->offset < entry->nlist)
		{
			entry->curItem = entry->list[entry->offset++];
			return;
		}

		LockBuffer(entry->buffer, GIN_SHARE);
		page = BufferGetPage(entry->buffer);
		for (;;)
		{
			/*
			 * It's needed to go by right link. During that we should refind
			 * first ItemPointer greater that stored
			 */

			blkno = GinPageGetOpaque(page)->rightlink;

			LockBuffer(entry->buffer, GIN_UNLOCK);
			if (blkno == InvalidBlockNumber)
			{
				ReleaseBuffer(entry->buffer);
				ItemPointerSetInvalid(&entry->curItem);
				entry->buffer = InvalidBuffer;
				entry->isFinished = TRUE;
				return;
			}

			entry->buffer = ReleaseAndReadBuffer(entry->buffer,
												 ginstate->index,
												 blkno);
			LockBuffer(entry->buffer, GIN_SHARE);
			page = BufferGetPage(entry->buffer);

			entry->offset = InvalidOffsetNumber;
			if (!ItemPointerIsValid(&entry->curItem) ||
				findItemInPostingPage(page, &entry->curItem, &entry->offset))
			{
				/*
				 * Found position equal to or greater than stored
				 */
				entry->nlist = GinPageGetOpaque(page)->maxoff;
				memcpy(entry->list, GinDataPageGetItem(page, FirstOffsetNumber),
				   GinPageGetOpaque(page)->maxoff * sizeof(ItemPointerData));

				LockBuffer(entry->buffer, GIN_UNLOCK);

				if (!ItemPointerIsValid(&entry->curItem) ||
					ginCompareItemPointers(&entry->curItem,
									   entry->list + entry->offset - 1) == 0)
				{
					/*
					 * First pages are deleted or empty, or we found exact
					 * position, so break inner loop and continue outer one.
					 */
					break;
				}

				/*
				 * Find greater than entry->curItem position, store it.
				 */
				entry->curItem = entry->list[entry->offset - 1];

				return;
			}
		}
	}
}

#define gin_rand() (((double) random()) / ((double) MAX_RANDOM_VALUE))
#define dropItem(e) ( gin_rand() > ((double)GinFuzzySearchLimit)/((double)((e)->predictNumberResult)) )

/*
 * Sets entry->curItem to next heap item pointer for one entry of one scan key,
 * or sets entry->isFinished to TRUE if there are no more.
 *
 * Item pointers must be returned in ascending order.
 *
 * Note: this can return a "lossy page" item pointer, indicating that the
 * entry potentially matches all items on that heap page.  However, it is
 * not allowed to return both a lossy page pointer and exact (regular)
 * item pointers for the same page.  (Doing so would break the key-combination
 * logic in keyGetItem and scanGetItem; see comment in scanGetItem.)  In the
 * current implementation this is guaranteed by the behavior of tidbitmaps.
 */
static void
entryGetItem(GinState *ginstate, GinScanEntry entry)
{
	Assert(!entry->isFinished);

	if (entry->matchBitmap)
	{
		do
		{
			if (entry->matchResult == NULL ||
				entry->offset >= entry->matchResult->ntuples)
			{
				entry->matchResult = tbm_iterate(entry->matchIterator);

				if (entry->matchResult == NULL)
				{
					ItemPointerSetInvalid(&entry->curItem);
					tbm_end_iterate(entry->matchIterator);
					entry->matchIterator = NULL;
					entry->isFinished = TRUE;
					break;
				}

				/*
				 * Reset counter to the beginning of entry->matchResult. Note:
				 * entry->offset is still greater than matchResult->ntuples if
				 * matchResult is lossy.  So, on next call we will get next
				 * result from TIDBitmap.
				 */
				entry->offset = 0;
			}

			if (entry->matchResult->ntuples < 0)
			{
				/*
				 * lossy result, so we need to check the whole page
				 */
				ItemPointerSetLossyPage(&entry->curItem,
										entry->matchResult->blockno);

				/*
				 * We might as well fall out of the loop; we could not
				 * estimate number of results on this page to support correct
				 * reducing of result even if it's enabled
				 */
				break;
			}

			ItemPointerSet(&entry->curItem,
						   entry->matchResult->blockno,
						   entry->matchResult->offsets[entry->offset]);
			entry->offset++;
		} while (entry->reduceResult == TRUE && dropItem(entry));
	}
	else if (!BufferIsValid(entry->buffer))
	{
		entry->offset++;
		if (entry->offset <= entry->nlist)
			entry->curItem = entry->list[entry->offset - 1];
		else
		{
			ItemPointerSetInvalid(&entry->curItem);
			entry->isFinished = TRUE;
		}
	}
	else
	{
		do
		{
			entryGetNextItem(ginstate, entry);
		} while (entry->isFinished == FALSE &&
				 entry->reduceResult == TRUE &&
				 dropItem(entry));
	}
}

/*
 * Identify the "current" item among the input entry streams for this scan key,
 * and test whether it passes the scan key qual condition.
 *
 * The current item is the smallest curItem among the inputs.  key->curItem
 * is set to that value.  key->curItemMatches is set to indicate whether that
 * TID passes the consistentFn test.  If so, key->recheckCurItem is set true
 * iff recheck is needed for this item pointer (including the case where the
 * item pointer is a lossy page pointer).
 *
 * If all entry streams are exhausted, sets key->isFinished to TRUE.
 *
 * Item pointers must be returned in ascending order.
 *
 * Note: this can return a "lossy page" item pointer, indicating that the
 * key potentially matches all items on that heap page.  However, it is
 * not allowed to return both a lossy page pointer and exact (regular)
 * item pointers for the same page.  (Doing so would break the key-combination
 * logic in scanGetItem.)
 */
static void
keyGetItem(GinState *ginstate, MemoryContext tempCtx, GinScanKey key)
{
	ItemPointerData minItem;
	ItemPointerData curPageLossy;
	uint32		i;
	uint32		lossyEntry;
	bool		haveLossyEntry;
	GinScanEntry entry;
	bool		res;
	MemoryContext oldCtx;

	Assert(!key->isFinished);

	/*
	 * Find the minimum of the active entry curItems.
	 *
	 * Note: a lossy-page entry is encoded by a ItemPointer with max value for
	 * offset (0xffff), so that it will sort after any exact entries for the
	 * same page.  So we'll prefer to return exact pointers not lossy
	 * pointers, which is good.
	 */
	ItemPointerSetMax(&minItem);

	for (i = 0; i < key->nentries; i++)
	{
		entry = key->scanEntry[i];
		if (entry->isFinished == FALSE &&
			ginCompareItemPointers(&entry->curItem, &minItem) < 0)
			minItem = entry->curItem;
	}

	if (ItemPointerIsMax(&minItem))
	{
		/* all entries are finished */
		key->isFinished = TRUE;
		return;
	}

	/*
	 * We might have already tested this item; if so, no need to repeat work.
	 * (Note: the ">" case can happen, if minItem is exact but we previously
	 * had to set curItem to a lossy-page pointer.)
	 */
	if (ginCompareItemPointers(&key->curItem, &minItem) >= 0)
		return;

	/*
	 * OK, advance key->curItem and perform consistentFn test.
	 */
	key->curItem = minItem;

	/*
	 * Lossy-page entries pose a problem, since we don't know the correct
	 * entryRes state to pass to the consistentFn, and we also don't know what
	 * its combining logic will be (could be AND, OR, or even NOT). If the
	 * logic is OR then the consistentFn might succeed for all items in the
	 * lossy page even when none of the other entries match.
	 *
	 * If we have a single lossy-page entry then we check to see if the
	 * consistentFn will succeed with only that entry TRUE.  If so, we return
	 * a lossy-page pointer to indicate that the whole heap page must be
	 * checked.  (On subsequent calls, we'll do nothing until minItem is past
	 * the page altogether, thus ensuring that we never return both regular
	 * and lossy pointers for the same page.)
	 *
	 * This idea could be generalized to more than one lossy-page entry, but
	 * ideally lossy-page entries should be infrequent so it would seldom be
	 * the case that we have more than one at once.  So it doesn't seem worth
	 * the extra complexity to optimize that case. If we do find more than
	 * one, we just punt and return a lossy-page pointer always.
	 *
	 * Note that only lossy-page entries pointing to the current item's page
	 * should trigger this processing; we might have future lossy pages in the
	 * entry array, but they aren't relevant yet.
	 */
	ItemPointerSetLossyPage(&curPageLossy,
							GinItemPointerGetBlockNumber(&key->curItem));

	lossyEntry = 0;
	haveLossyEntry = false;
	for (i = 0; i < key->nentries; i++)
	{
		entry = key->scanEntry[i];
		if (entry->isFinished == FALSE &&
			ginCompareItemPointers(&entry->curItem, &curPageLossy) == 0)
		{
			if (haveLossyEntry)
			{
				/* Multiple lossy entries, punt */
				key->curItem = curPageLossy;
				key->curItemMatches = true;
				key->recheckCurItem = true;
				return;
			}
			lossyEntry = i;
			haveLossyEntry = true;
		}
	}

	/* prepare for calling consistentFn in temp context */
	oldCtx = MemoryContextSwitchTo(tempCtx);

	if (haveLossyEntry)
	{
		/* Single lossy-page entry, so see if whole page matches */
		memset(key->entryRes, FALSE, key->nentries);
		key->entryRes[lossyEntry] = TRUE;

		if (callConsistentFn(ginstate, key))
		{
			/* Yes, so clean up ... */
			MemoryContextSwitchTo(oldCtx);
			MemoryContextReset(tempCtx);

			/* and return lossy pointer for whole page */
			key->curItem = curPageLossy;
			key->curItemMatches = true;
			key->recheckCurItem = true;
			return;
		}
	}

	/*
	 * At this point we know that we don't need to return a lossy whole-page
	 * pointer, but we might have matches for individual exact item pointers,
	 * possibly in combination with a lossy pointer.  Our strategy if there's
	 * a lossy pointer is to try the consistentFn both ways and return a hit
	 * if it accepts either one (forcing the hit to be marked lossy so it will
	 * be rechecked).  An exception is that we don't need to try it both ways
	 * if the lossy pointer is in a "hidden" entry, because the consistentFn's
	 * result can't depend on that.
	 *
	 * Prepare entryRes array to be passed to consistentFn.
	 */
	for (i = 0; i < key->nentries; i++)
	{
		entry = key->scanEntry[i];
		if (entry->isFinished == FALSE &&
			ginCompareItemPointers(&entry->curItem, &key->curItem) == 0)
			key->entryRes[i] = TRUE;
		else
			key->entryRes[i] = FALSE;
	}
	if (haveLossyEntry)
		key->entryRes[lossyEntry] = TRUE;

	res = callConsistentFn(ginstate, key);

	if (!res && haveLossyEntry && lossyEntry < key->nuserentries)
	{
		/* try the other way for the lossy item */
		key->entryRes[lossyEntry] = FALSE;

		res = callConsistentFn(ginstate, key);
	}

	key->curItemMatches = res;
	/* If we matched a lossy entry, force recheckCurItem = true */
	if (haveLossyEntry)
		key->recheckCurItem = true;

	/* clean up after consistentFn calls */
	MemoryContextSwitchTo(oldCtx);
	MemoryContextReset(tempCtx);
}

/*
 * Get next heap item pointer (after advancePast) from scan.
 * Returns true if anything found.
 * On success, *item and *recheck are set.
 *
 * Note: this is very nearly the same logic as in keyGetItem(), except
 * that we know the keys are to be combined with AND logic, whereas in
 * keyGetItem() the combination logic is known only to the consistentFn.
 */
static bool
scanGetItem(IndexScanDesc scan, ItemPointer advancePast,
			ItemPointerData *item, bool *recheck)
{
	GinScanOpaque so = (GinScanOpaque) scan->opaque;
	GinState   *ginstate = &so->ginstate;
	ItemPointerData myAdvancePast = *advancePast;
	uint32		i;
	bool		allFinished;
	bool		match;

	for (;;)
	{
		/*
		 * Advance any entries that are <= myAdvancePast.  In particular,
		 * since entry->curItem was initialized with ItemPointerSetMin, this
		 * ensures we fetch the first item for each entry on the first call.
		 */
		allFinished = TRUE;

		for (i = 0; i < so->totalentries; i++)
		{
			GinScanEntry entry = so->entries[i];

			while (entry->isFinished == FALSE &&
				   ginCompareItemPointers(&entry->curItem,
										  &myAdvancePast) <= 0)
				entryGetItem(ginstate, entry);

			if (entry->isFinished == FALSE)
				allFinished = FALSE;
		}

		if (allFinished)
		{
			/* all entries exhausted, so we're done */
			return false;
		}

		/*
		 * Perform the consistentFn test for each scan key.  If any key
		 * reports isFinished, meaning its subset of the entries is exhausted,
		 * we can stop.  Otherwise, set *item to the minimum of the key
		 * curItems.
		 */
		ItemPointerSetMax(item);

		for (i = 0; i < so->nkeys; i++)
		{
			GinScanKey	key = so->keys + i;

			keyGetItem(&so->ginstate, so->tempCtx, key);

			if (key->isFinished)
				return false;	/* finished one of keys */

			if (ginCompareItemPointers(&key->curItem, item) < 0)
				*item = key->curItem;
		}

		Assert(!ItemPointerIsMax(item));

		/*----------
		 * Now *item contains first ItemPointer after previous result.
		 *
		 * The item is a valid hit only if all the keys succeeded for either
		 * that exact TID, or a lossy reference to the same page.
		 *
		 * This logic works only if a keyGetItem stream can never contain both
		 * exact and lossy pointers for the same page.	Else we could have a
		 * case like
		 *
		 *		stream 1		stream 2
		 *		...				...
		 *		42/6			42/7
		 *		50/1			42/0xffff
		 *		...				...
		 *
		 * We would conclude that 42/6 is not a match and advance stream 1,
		 * thus never detecting the match to the lossy pointer in stream 2.
		 * (keyGetItem has a similar problem versus entryGetItem.)
		 *----------
		 */
		match = true;
		for (i = 0; i < so->nkeys; i++)
		{
			GinScanKey	key = so->keys + i;

			if (key->curItemMatches)
			{
				if (ginCompareItemPointers(item, &key->curItem) == 0)
					continue;
				if (ItemPointerIsLossyPage(&key->curItem) &&
					GinItemPointerGetBlockNumber(&key->curItem) ==
					GinItemPointerGetBlockNumber(item))
					continue;
			}
			match = false;
			break;
		}

		if (match)
			break;

		/*
		 * No hit.	Update myAdvancePast to this TID, so that on the next pass
		 * we'll move to the next possible entry.
		 */
		myAdvancePast = *item;
	}

	/*
	 * We must return recheck = true if any of the keys are marked recheck.
	 */
	*recheck = false;
	for (i = 0; i < so->nkeys; i++)
	{
		GinScanKey	key = so->keys + i;

		if (key->recheckCurItem)
		{
			*recheck = true;
			break;
		}
	}

	return TRUE;
}


/*
 * Functions for scanning the pending list
 */


/*
 * Get ItemPointer of next heap row to be checked from pending list.
 * Returns false if there are no more. On pages with several heap rows
 * it returns each row separately, on page with part of heap row returns
 * per page data.  pos->firstOffset and pos->lastOffset are set to identify
 * the range of pending-list tuples belonging to this heap row.
 *
 * The pendingBuffer is presumed pinned and share-locked on entry, and is
 * pinned and share-locked on success exit.  On failure exit it's released.
 */
static bool
scanGetCandidate(IndexScanDesc scan, pendingPosition *pos)
{
	OffsetNumber maxoff;
	Page		page;
	IndexTuple	itup;

	ItemPointerSetInvalid(&pos->item);
	for (;;)
	{
		page = BufferGetPage(pos->pendingBuffer);

		maxoff = PageGetMaxOffsetNumber(page);
		if (pos->firstOffset > maxoff)
		{
			BlockNumber blkno = GinPageGetOpaque(page)->rightlink;

			if (blkno == InvalidBlockNumber)
			{
				UnlockReleaseBuffer(pos->pendingBuffer);
				pos->pendingBuffer = InvalidBuffer;

				return false;
			}
			else
			{
				/*
				 * Here we must prevent deletion of next page by insertcleanup
				 * process, which may be trying to obtain exclusive lock on
				 * current page.  So, we lock next page before releasing the
				 * current one
				 */
				Buffer		tmpbuf = ReadBuffer(scan->indexRelation, blkno);

				LockBuffer(tmpbuf, GIN_SHARE);
				UnlockReleaseBuffer(pos->pendingBuffer);

				pos->pendingBuffer = tmpbuf;
				pos->firstOffset = FirstOffsetNumber;
			}
		}
		else
		{
			itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, pos->firstOffset));
			pos->item = itup->t_tid;
			if (GinPageHasFullRow(page))
			{
				/*
				 * find itempointer to the next row
				 */
				for (pos->lastOffset = pos->firstOffset + 1; pos->lastOffset <= maxoff; pos->lastOffset++)
				{
					itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, pos->lastOffset));
					if (!ItemPointerEquals(&pos->item, &itup->t_tid))
						break;
				}
			}
			else
			{
				/*
				 * All itempointers are the same on this page
				 */
				pos->lastOffset = maxoff + 1;
			}

			/*
			 * Now pos->firstOffset points to the first tuple of current heap
			 * row, pos->lastOffset points to the first tuple of next heap row
			 * (or to the end of page)
			 */
			break;
		}
	}

	return true;
}

/*
 * Scan pending-list page from current tuple (off) up till the first of:
 * - match is found (then returns true)
 * - no later match is possible
 * - tuple's attribute number is not equal to entry's attrnum
 * - reach end of page
 *
 * datum[]/category[]/datumExtracted[] arrays are used to cache the results
 * of gintuple_get_key() on the current page.
 */
static bool
matchPartialInPendingList(GinState *ginstate, Page page,
						  OffsetNumber off, OffsetNumber maxoff,
						  GinScanEntry entry,
						  Datum *datum, GinNullCategory *category,
						  bool *datumExtracted)
{
	IndexTuple	itup;
	int32		cmp;

	/* Partial match to a null is not possible */
	if (entry->queryCategory != GIN_CAT_NORM_KEY)
		return false;

	while (off < maxoff)
	{
		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, off));

		if (gintuple_get_attrnum(ginstate, itup) != entry->attnum)
			return false;

		if (datumExtracted[off - 1] == false)
		{
			datum[off - 1] = gintuple_get_key(ginstate, itup,
											  &category[off - 1]);
			datumExtracted[off - 1] = true;
		}

		/* Once we hit nulls, no further match is possible */
		if (category[off - 1] != GIN_CAT_NORM_KEY)
			return false;

		/*----------
		 * Check partial match.
		 * case cmp == 0 => match
		 * case cmp > 0 => not match and end scan (no later match possible)
		 * case cmp < 0 => not match and continue scan
		 *----------
		 */
		cmp = DatumGetInt32(FunctionCall4Coll(&ginstate->comparePartialFn[entry->attnum - 1],
							   ginstate->supportCollation[entry->attnum - 1],
											  entry->queryKey,
											  datum[off - 1],
											  UInt16GetDatum(entry->strategy),
										PointerGetDatum(entry->extra_data)));
		if (cmp == 0)
			return true;
		else if (cmp > 0)
			return false;

		off++;
	}

	return false;
}

/*
 * Set up the entryRes array for each key by looking at
 * every entry for current heap row in pending list.
 *
 * Returns true if each scan key has at least one entryRes match.
 * This corresponds to the situations where the normal index search will
 * try to apply the key's consistentFn.  (A tuple not meeting that requirement
 * cannot be returned by the normal search since no entry stream will
 * source its TID.)
 *
 * The pendingBuffer is presumed pinned and share-locked on entry.
 */
static bool
collectMatchesForHeapRow(IndexScanDesc scan, pendingPosition *pos)
{
	GinScanOpaque so = (GinScanOpaque) scan->opaque;
	OffsetNumber attrnum;
	Page		page;
	IndexTuple	itup;
	int			i,
				j;

	/*
	 * Reset all entryRes and hasMatchKey flags
	 */
	for (i = 0; i < so->nkeys; i++)
	{
		GinScanKey	key = so->keys + i;

		memset(key->entryRes, FALSE, key->nentries);
	}
	memset(pos->hasMatchKey, FALSE, so->nkeys);

	/*
	 * Outer loop iterates over multiple pending-list pages when a single heap
	 * row has entries spanning those pages.
	 */
	for (;;)
	{
		Datum		datum[BLCKSZ / sizeof(IndexTupleData)];
		GinNullCategory category[BLCKSZ / sizeof(IndexTupleData)];
		bool		datumExtracted[BLCKSZ / sizeof(IndexTupleData)];

		Assert(pos->lastOffset > pos->firstOffset);
		memset(datumExtracted + pos->firstOffset - 1, 0,
			   sizeof(bool) * (pos->lastOffset - pos->firstOffset));

		page = BufferGetPage(pos->pendingBuffer);

		for (i = 0; i < so->nkeys; i++)
		{
			GinScanKey	key = so->keys + i;

			for (j = 0; j < key->nentries; j++)
			{
				GinScanEntry entry = key->scanEntry[j];
				OffsetNumber StopLow = pos->firstOffset,
							StopHigh = pos->lastOffset,
							StopMiddle;

				/* If already matched on earlier page, do no extra work */
				if (key->entryRes[j])
					continue;

				/*
				 * Interesting tuples are from pos->firstOffset to
				 * pos->lastOffset and they are ordered by (attnum, Datum) as
				 * it's done in entry tree.  So we can use binary search to
				 * avoid linear scanning.
				 */
				while (StopLow < StopHigh)
				{
					int			res;

					StopMiddle = StopLow + ((StopHigh - StopLow) >> 1);

					itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, StopMiddle));

					attrnum = gintuple_get_attrnum(&so->ginstate, itup);

					if (key->attnum < attrnum)
					{
						StopHigh = StopMiddle;
						continue;
					}
					if (key->attnum > attrnum)
					{
						StopLow = StopMiddle + 1;
						continue;
					}

					if (datumExtracted[StopMiddle - 1] == false)
					{
						datum[StopMiddle - 1] =
							gintuple_get_key(&so->ginstate, itup,
											 &category[StopMiddle - 1]);
						datumExtracted[StopMiddle - 1] = true;
					}

					if (entry->queryCategory == GIN_CAT_EMPTY_QUERY)
					{
						/* special behavior depending on searchMode */
						if (entry->searchMode == GIN_SEARCH_MODE_ALL)
						{
							/* match anything except NULL_ITEM */
							if (category[StopMiddle - 1] == GIN_CAT_NULL_ITEM)
								res = -1;
							else
								res = 0;
						}
						else
						{
							/* match everything */
							res = 0;
						}
					}
					else
					{
						res = ginCompareEntries(&so->ginstate,
												entry->attnum,
												entry->queryKey,
												entry->queryCategory,
												datum[StopMiddle - 1],
												category[StopMiddle - 1]);
					}

					if (res == 0)
					{
						/*
						 * Found exact match (there can be only one, except in
						 * EMPTY_QUERY mode).
						 *
						 * If doing partial match, scan forward from here to
						 * end of page to check for matches.
						 *
						 * See comment above about tuple's ordering.
						 */
						if (entry->isPartialMatch)
							key->entryRes[j] =
								matchPartialInPendingList(&so->ginstate,
														  page,
														  StopMiddle,
														  pos->lastOffset,
														  entry,
														  datum,
														  category,
														  datumExtracted);
						else
							key->entryRes[j] = true;

						/* done with binary search */
						break;
					}
					else if (res < 0)
						StopHigh = StopMiddle;
					else
						StopLow = StopMiddle + 1;
				}

				if (StopLow >= StopHigh && entry->isPartialMatch)
				{
					/*
					 * No exact match on this page.  If doing partial match,
					 * scan from the first tuple greater than target value to
					 * end of page.  Note that since we don't remember whether
					 * the comparePartialFn told us to stop early on a
					 * previous page, we will uselessly apply comparePartialFn
					 * to the first tuple on each subsequent page.
					 */
					key->entryRes[j] =
						matchPartialInPendingList(&so->ginstate,
												  page,
												  StopHigh,
												  pos->lastOffset,
												  entry,
												  datum,
												  category,
												  datumExtracted);
				}

				pos->hasMatchKey[i] |= key->entryRes[j];
			}
		}

		/* Advance firstOffset over the scanned tuples */
		pos->firstOffset = pos->lastOffset;

		if (GinPageHasFullRow(page))
		{
			/*
			 * We have examined all pending entries for the current heap row.
			 * Break out of loop over pages.
			 */
			break;
		}
		else
		{
			/*
			 * Advance to next page of pending entries for the current heap
			 * row.  Complain if there isn't one.
			 */
			ItemPointerData item = pos->item;

			if (scanGetCandidate(scan, pos) == false ||
				!ItemPointerEquals(&pos->item, &item))
				elog(ERROR, "could not find additional pending pages for same heap tuple");
		}
	}

	/*
	 * Now return "true" if all scan keys have at least one matching datum
	 */
	for (i = 0; i < so->nkeys; i++)
	{
		if (pos->hasMatchKey[i] == false)
			return false;
	}

	return true;
}

/*
 * Collect all matched rows from pending list into bitmap
 */
static void
scanPendingInsert(IndexScanDesc scan, TIDBitmap *tbm, int64 *ntids)
{
	GinScanOpaque so = (GinScanOpaque) scan->opaque;
	MemoryContext oldCtx;
	bool		recheck,
				match;
	int			i;
	pendingPosition pos;
	Buffer		metabuffer = ReadBuffer(scan->indexRelation, GIN_METAPAGE_BLKNO);
	BlockNumber blkno;

	*ntids = 0;

	LockBuffer(metabuffer, GIN_SHARE);
	blkno = GinPageGetMeta(BufferGetPage(metabuffer))->head;

	/*
	 * fetch head of list before unlocking metapage. head page must be pinned
	 * to prevent deletion by vacuum process
	 */
	if (blkno == InvalidBlockNumber)
	{
		/* No pending list, so proceed with normal scan */
		UnlockReleaseBuffer(metabuffer);
		return;
	}

	pos.pendingBuffer = ReadBuffer(scan->indexRelation, blkno);
	LockBuffer(pos.pendingBuffer, GIN_SHARE);
	pos.firstOffset = FirstOffsetNumber;
	UnlockReleaseBuffer(metabuffer);
	pos.hasMatchKey = palloc(sizeof(bool) * so->nkeys);

	/*
	 * loop for each heap row. scanGetCandidate returns full row or row's
	 * tuples from first page.
	 */
	while (scanGetCandidate(scan, &pos))
	{
		/*
		 * Check entries in tuple and set up entryRes array.
		 *
		 * If pending tuples belonging to the current heap row are spread
		 * across several pages, collectMatchesForHeapRow will read all of
		 * those pages.
		 */
		if (!collectMatchesForHeapRow(scan, &pos))
			continue;

		/*
		 * Matching of entries of one row is finished, so check row using
		 * consistent functions.
		 */
		oldCtx = MemoryContextSwitchTo(so->tempCtx);
		recheck = false;
		match = true;

		for (i = 0; i < so->nkeys; i++)
		{
			GinScanKey	key = so->keys + i;

			if (!callConsistentFn(&so->ginstate, key))
			{
				match = false;
				break;
			}
			recheck |= key->recheckCurItem;
		}

		MemoryContextSwitchTo(oldCtx);
		MemoryContextReset(so->tempCtx);

		if (match)
		{
			tbm_add_tuples(tbm, &pos.item, 1, recheck);
			(*ntids)++;
		}
	}

	pfree(pos.hasMatchKey);
}


#define GinIsNewKey(s)		( ((GinScanOpaque) scan->opaque)->keys == NULL )
#define GinIsVoidRes(s)		( ((GinScanOpaque) scan->opaque)->isVoidRes )

Datum
gingetbitmap(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	TIDBitmap  *tbm = (TIDBitmap *) PG_GETARG_POINTER(1);
	int64		ntids;
	ItemPointerData iptr;
	bool		recheck;

	/*
	 * Set up the scan keys, and check for unsatisfiable query.
	 */
	if (GinIsNewKey(scan))
		ginNewScanKey(scan);

	if (GinIsVoidRes(scan))
		PG_RETURN_INT64(0);

	ntids = 0;

	/*
	 * First, scan the pending list and collect any matching entries into the
	 * bitmap.	After we scan a pending item, some other backend could post it
	 * into the main index, and so we might visit it a second time during the
	 * main scan.  This is okay because we'll just re-set the same bit in the
	 * bitmap.	(The possibility of duplicate visits is a major reason why GIN
	 * can't support the amgettuple API, however.) Note that it would not do
	 * to scan the main index before the pending list, since concurrent
	 * cleanup could then make us miss entries entirely.
	 */
	scanPendingInsert(scan, tbm, &ntids);

	/*
	 * Now scan the main index.
	 */
	startScan(scan);

	ItemPointerSetMin(&iptr);

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		if (!scanGetItem(scan, &iptr, &iptr, &recheck))
			break;

		if (ItemPointerIsLossyPage(&iptr))
			tbm_add_page(tbm, ItemPointerGetBlockNumber(&iptr));
		else
			tbm_add_tuples(tbm, &iptr, 1, recheck);
		ntids++;
	}

	PG_RETURN_INT64(ntids);
}
