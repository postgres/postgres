/*-------------------------------------------------------------------------
 *
 * ginget.c
 *	  fetch tuples from a GIN scan.
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			$PostgreSQL: pgsql/src/backend/access/gin/ginget.c,v 1.30.4.2 2010/08/01 19:16:47 tgl Exp $
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin.h"
#include "access/relscan.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
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
 * Tries to refind previously taken ItemPointer on page.
 */
static bool
findItemInPage(Page page, ItemPointer item, OffsetNumber *off)
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
		res = compareItemPointers(item, (ItemPointer) GinDataPageGetItem(page, *off));

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
		if (GinPageRightMost(page))
			return false;		/* no more pages */

		stack->buffer = ginStepRight(stack->buffer, btree->index, GIN_SHARE);
		stack->blkno = BufferGetBlockNumber(stack->buffer);
		stack->off = FirstOffsetNumber;
	}

	return true;
}

/*
 * Does fullscan of posting tree and saves ItemPointers
 * in scanEntry->partialMatch TIDBitmap
 */
static void
scanForItems(Relation index, GinScanEntry scanEntry, BlockNumber rootPostingTree)
{
	GinPostingTreeScan *gdi;
	Buffer		buffer;
	Page		page;

	gdi = prepareScanPostingTree(index, rootPostingTree, TRUE);

	buffer = scanBeginPostingTree(gdi);
	IncrBufferRefCount(buffer); /* prevent unpin in freeGinBtreeStack */

	freeGinBtreeStack(gdi->stack);
	pfree(gdi);

	/*
	 * Goes through all leaves
	 */
	for (;;)
	{
		page = BufferGetPage(buffer);

		if ((GinPageGetOpaque(page)->flags & GIN_DELETED) == 0 && GinPageGetOpaque(page)->maxoff >= FirstOffsetNumber)
		{
			tbm_add_tuples(scanEntry->partialMatch,
				   (ItemPointer) GinDataPageGetItem(page, FirstOffsetNumber),
						   GinPageGetOpaque(page)->maxoff, false);
			scanEntry->predictNumberResult += GinPageGetOpaque(page)->maxoff;
		}

		if (GinPageRightMost(page))
		{
			UnlockReleaseBuffer(buffer);
			return;				/* no more pages */
		}

		buffer = ginStepRight(buffer, index, GIN_SHARE);
	}
}

/*
 * Collects all ItemPointer into the TIDBitmap struct
 * for entries partially matched to search entry.
 *
 * Returns true if done, false if it's needed to restart scan from scratch
 */
static bool
computePartialMatchList(GinBtreeData *btree, GinBtreeStack *stack, GinScanEntry scanEntry)
{
	Page		page;
	IndexTuple	itup;
	Datum		idatum;
	int32		cmp;

	scanEntry->partialMatch = tbm_create(work_mem * 1024L);

	for (;;)
	{
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
		if (gintuple_get_attrnum(btree->ginstate, itup) != scanEntry->attnum)
			return true;

		idatum = gin_index_getattr(btree->ginstate, itup);


		/*----------
		 * Check of partial match.
		 * case cmp == 0 => match
		 * case cmp > 0 => not match and finish scan
		 * case cmp < 0 => not match and continue scan
		 *----------
		 */
		cmp = DatumGetInt32(FunctionCall4(&btree->ginstate->comparePartialFn[scanEntry->attnum - 1],
										  scanEntry->entry,
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

		if (GinIsPostingTree(itup))
		{
			BlockNumber rootPostingTree = GinGetPostingTree(itup);
			Datum		newDatum,
						savedDatum = datumCopy(
											   idatum,
											   btree->ginstate->origTupdesc->attrs[scanEntry->attnum - 1]->attbyval,
			btree->ginstate->origTupdesc->attrs[scanEntry->attnum - 1]->attlen
			);

			/*
			 * We should unlock current page (but not unpin) during tree scan
			 * to prevent deadlock with vacuum processes.
			 *
			 * We save current entry value (savedDatum) to be able to refind
			 * our tuple after re-locking
			 */
			LockBuffer(stack->buffer, GIN_UNLOCK);
			scanForItems(btree->index, scanEntry, rootPostingTree);

			/*
			 * We lock again the entry page and while it was unlocked insert
			 * might occured, so we need to refind our position
			 */
			LockBuffer(stack->buffer, GIN_SHARE);
			page = BufferGetPage(stack->buffer);
			if (!GinPageIsLeaf(page))
			{
				/*
				 * Root page becomes non-leaf while we unlock it. We will
				 * start again, this situation doesn't cause often - root can
				 * became a non-leaf only one per life of index.
				 */

				return false;
			}

			for (;;)
			{
				if (moveRightIfItNeeded(btree, stack) == false)
					elog(ERROR, "lost saved point in index");	/* must not happen !!! */

				page = BufferGetPage(stack->buffer);
				itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, stack->off));
				newDatum = gin_index_getattr(btree->ginstate, itup);

				if (gintuple_get_attrnum(btree->ginstate, itup) != scanEntry->attnum)
					elog(ERROR, "lost saved point in index");	/* must not happen !!! */

				if (compareEntries(btree->ginstate, scanEntry->attnum, newDatum, savedDatum) == 0)
				{
					/* Found!  */
					if (btree->ginstate->origTupdesc->attrs[scanEntry->attnum - 1]->attbyval == false)
						pfree(DatumGetPointer(savedDatum));
					break;
				}

				stack->off++;
			}
		}
		else
		{
			tbm_add_tuples(scanEntry->partialMatch, GinGetPosting(itup), GinGetNPosting(itup), false);
			scanEntry->predictNumberResult += GinGetNPosting(itup);
		}

		/*
		 * Ok, we save ItemPointers, go to the next entry
		 */
		stack->off++;
	}

	return true;
}

/*
 * Start* functions setup beginning state of searches: finds correct buffer and pins it.
 */
static void
startScanEntry(Relation index, GinState *ginstate, GinScanEntry entry)
{
	GinBtreeData btreeEntry;
	GinBtreeStack *stackEntry;
	Page		page;
	bool		needUnlock = TRUE;

	entry->buffer = InvalidBuffer;
	entry->offset = InvalidOffsetNumber;
	entry->list = NULL;
	entry->nlist = 0;
	entry->partialMatch = NULL;
	entry->partialMatchResult = NULL;
	entry->reduceResult = FALSE;
	entry->predictNumberResult = 0;

	if (entry->master != NULL)
	{
		entry->isFinished = entry->master->isFinished;
		return;
	}

	/*
	 * we should find entry, and begin scan of posting tree or just store
	 * posting list in memory
	 */

	prepareEntryScan(&btreeEntry, index, entry->attnum, entry->entry, ginstate);
	btreeEntry.searchMode = TRUE;
	stackEntry = ginFindLeafPage(&btreeEntry, NULL);
	page = BufferGetPage(stackEntry->buffer);

	entry->isFinished = TRUE;

	if (entry->isPartialMatch)
	{
		/*
		 * btreeEntry.findItem points to the first equal or greater value than
		 * needed. So we will scan further and collect all ItemPointers
		 */
		btreeEntry.findItem(&btreeEntry, stackEntry);
		if (computePartialMatchList(&btreeEntry, stackEntry, entry) == false)
		{
			/*
			 * GIN tree was seriously restructured, so we will cleanup all
			 * found data and rescan. See comments near 'return false' in
			 * computePartialMatchList()
			 */
			if (entry->partialMatch)
			{
				if (entry->partialMatchIterator)
					tbm_end_iterate(entry->partialMatchIterator);
				entry->partialMatchIterator = NULL;
				tbm_free(entry->partialMatch);
				entry->partialMatch = NULL;
			}
			LockBuffer(stackEntry->buffer, GIN_UNLOCK);
			freeGinBtreeStack(stackEntry);

			startScanEntry(index, ginstate, entry);
			return;
		}

		if (entry->partialMatch && !tbm_is_empty(entry->partialMatch))
		{
			entry->partialMatchIterator = tbm_begin_iterate(entry->partialMatch);
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
			 * We should unlock entry page before make deal with posting tree
			 * to prevent deadlocks with vacuum processes. Because entry is
			 * never deleted from page and posting tree is never reduced to
			 * the posting list, we can unlock page after getting BlockNumber
			 * of root of posting tree.
			 */
			LockBuffer(stackEntry->buffer, GIN_UNLOCK);
			needUnlock = FALSE;
			gdi = prepareScanPostingTree(index, rootPostingTree, TRUE);

			entry->buffer = scanBeginPostingTree(gdi);

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
startScanKey(Relation index, GinState *ginstate, GinScanKey key)
{
	uint32		i;

	if (!key->firstCall)
		return;

	for (i = 0; i < key->nentries; i++)
		startScanEntry(index, ginstate, key->scanEntry + i);

	key->isFinished = FALSE;
	key->firstCall = FALSE;

	if (GinFuzzySearchLimit > 0)
	{
		/*
		 * If all of keys more than threshold we will try to reduce result, we
		 * hope (and only hope, for intersection operation of array our
		 * supposition isn't true), that total result will not more than
		 * minimal predictNumberResult.
		 */

		for (i = 0; i < key->nentries; i++)
			if (key->scanEntry[i].predictNumberResult <= key->nentries * GinFuzzySearchLimit)
				return;

		for (i = 0; i < key->nentries; i++)
			if (key->scanEntry[i].predictNumberResult > key->nentries * GinFuzzySearchLimit)
			{
				key->scanEntry[i].predictNumberResult /= key->nentries;
				key->scanEntry[i].reduceResult = TRUE;
			}
	}
}

static void
startScan(IndexScanDesc scan)
{
	uint32		i;
	GinScanOpaque so = (GinScanOpaque) scan->opaque;

	for (i = 0; i < so->nkeys; i++)
		startScanKey(scan->indexRelation, &so->ginstate, so->keys + i);
}

/*
 * Gets next ItemPointer from PostingTree. Note, that we copy
 * page into GinScanEntry->list array and unlock page, but keep it pinned
 * to prevent interference with vacuum
 */
static void
entryGetNextItem(Relation index, GinScanEntry entry)
{
	Page		page;

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
			if (GinPageRightMost(page))
			{
				UnlockReleaseBuffer(entry->buffer);
				ItemPointerSetInvalid(&entry->curItem);
				entry->buffer = InvalidBuffer;
				entry->isFinished = TRUE;
				return;
			}

			entry->buffer = ginStepRight(entry->buffer, index, GIN_SHARE);
			page = BufferGetPage(entry->buffer);

			entry->offset = InvalidOffsetNumber;
			if (!ItemPointerIsValid(&entry->curItem) ||
				findItemInPage(page, &entry->curItem, &entry->offset))
			{
				/*
				 * Found position equal to or greater than stored
				 */
				entry->nlist = GinPageGetOpaque(page)->maxoff;
				memcpy(entry->list, GinDataPageGetItem(page, FirstOffsetNumber),
				   GinPageGetOpaque(page)->maxoff * sizeof(ItemPointerData));

				LockBuffer(entry->buffer, GIN_UNLOCK);

				if (!ItemPointerIsValid(&entry->curItem) ||
					compareItemPointers(&entry->curItem,
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

/* convenience function for invoking a key's consistentFn */
static inline bool
callConsistentFn(GinState *ginstate, GinScanKey key)
{
	/*
	 * Initialize recheckCurItem in case the consistentFn doesn't know it
	 * should set it.  The safe assumption in that case is to force recheck.
	 */
	key->recheckCurItem = true;

	return DatumGetBool(FunctionCall6(&ginstate->consistentFn[key->attnum - 1],
									  PointerGetDatum(key->entryRes),
									  UInt16GetDatum(key->strategy),
									  key->query,
									  UInt32GetDatum(key->nentries),
									  PointerGetDatum(key->extra_data),
									  PointerGetDatum(&key->recheckCurItem)));
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
entryGetItem(Relation index, GinScanEntry entry)
{
	Assert(!entry->isFinished);

	if (entry->master)
	{
		entry->isFinished = entry->master->isFinished;
		entry->curItem = entry->master->curItem;
	}
	else if (entry->partialMatch)
	{
		do
		{
			if (entry->partialMatchResult == NULL ||
				entry->offset >= entry->partialMatchResult->ntuples)
			{
				entry->partialMatchResult = tbm_iterate(entry->partialMatchIterator);

				if (entry->partialMatchResult == NULL)
				{
					ItemPointerSetInvalid(&entry->curItem);
					tbm_end_iterate(entry->partialMatchIterator);
					entry->partialMatchIterator = NULL;
					entry->isFinished = TRUE;
					break;
				}

				/*
				 * reset counter to the beginning of
				 * entry->partialMatchResult. Note: entry->offset is still
				 * greater than partialMatchResult->ntuples if
				 * partialMatchResult is lossy. So, on next call we will get
				 * next result from TIDBitmap.
				 */
				entry->offset = 0;
			}

			if (entry->partialMatchResult->ntuples < 0)
			{
				/*
				 * lossy result, so we need to check the whole page
				 */
				ItemPointerSetLossyPage(&entry->curItem,
										entry->partialMatchResult->blockno);

				/*
				 * We might as well fall out of the loop; we could not
				 * estimate number of results on this page to support correct
				 * reducing of result even if it's enabled
				 */
				break;
			}

			ItemPointerSet(&entry->curItem,
						   entry->partialMatchResult->blockno,
						   entry->partialMatchResult->offsets[entry->offset]);
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
			entryGetNextItem(index, entry);
		} while (entry->isFinished == FALSE &&
				 entry->reduceResult == TRUE &&
				 dropItem(entry));
	}
}

/*
 * Sets key->curItem to next heap item pointer for one scan key, advancing
 * past any item pointers <= advancePast.
 * Sets key->isFinished to TRUE if there are no more.
 *
 * On success, key->recheckCurItem is set true iff recheck is needed for this
 * item pointer (including the case where the item pointer is a lossy page
 * pointer).
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
keyGetItem(Relation index, GinState *ginstate, MemoryContext tempCtx,
		   GinScanKey key, ItemPointer advancePast)
{
	ItemPointerData myAdvancePast = *advancePast;
	ItemPointerData curPageLossy;
	uint32		i;
	uint32		lossyEntry;
	bool		haveLossyEntry;
	GinScanEntry entry;
	bool		res;
	MemoryContext oldCtx;

	Assert(!key->isFinished);

	do
	{
		/*
		 * Advance any entries that are <= myAdvancePast.  In particular,
		 * since entry->curItem was initialized with ItemPointerSetMin, this
		 * ensures we fetch the first item for each entry on the first call.
		 * Then set key->curItem to the minimum of the valid entry curItems.
		 *
		 * Note: a lossy-page entry is encoded by a ItemPointer with max value
		 * for offset (0xffff), so that it will sort after any exact entries
		 * for the same page.  So we'll prefer to return exact pointers not
		 * lossy pointers, which is good.  Also, when we advance past an exact
		 * entry after processing it, we will not advance past lossy entries
		 * for the same page in other keys, which is NECESSARY for correct
		 * results (since we might have additional entries for the same page
		 * in the first key).
		 */
		ItemPointerSetMax(&key->curItem);

		for (i = 0; i < key->nentries; i++)
		{
			entry = key->scanEntry + i;

			while (entry->isFinished == FALSE &&
				   compareItemPointers(&entry->curItem, &myAdvancePast) <= 0)
				entryGetItem(index, entry);

			if (entry->isFinished == FALSE &&
				compareItemPointers(&entry->curItem, &key->curItem) < 0)
				key->curItem = entry->curItem;
		}

		if (ItemPointerIsMax(&key->curItem))
		{
			/* all entries are finished */
			key->isFinished = TRUE;
			return;
		}

		/*
		 * Now key->curItem contains first ItemPointer after previous result.
		 * Advance myAdvancePast to this value, so that if the consistentFn
		 * rejects the entry and we loop around again, we will advance to the
		 * next available item pointer.
		 */
		myAdvancePast = key->curItem;

		/*
		 * Lossy-page entries pose a problem, since we don't know the correct
		 * entryRes state to pass to the consistentFn, and we also don't know
		 * what its combining logic will be (could be AND, OR, or even NOT).
		 * If the logic is OR then the consistentFn might succeed for all
		 * items in the lossy page even when none of the other entries match.
		 *
		 * If we have a single lossy-page entry then we check to see if the
		 * consistentFn will succeed with only that entry TRUE.  If so,
		 * we return a lossy-page pointer to indicate that the whole heap
		 * page must be checked.  (On the next call, we'll advance past all
		 * regular and lossy entries for this page before resuming search,
		 * thus ensuring that we never return both regular and lossy pointers
		 * for the same page.)
		 *
		 * This idea could be generalized to more than one lossy-page entry,
		 * but ideally lossy-page entries should be infrequent so it would
		 * seldom be the case that we have more than one at once.  So it
		 * doesn't seem worth the extra complexity to optimize that case.
		 * If we do find more than one, we just punt and return a lossy-page
		 * pointer always.
		 *
		 * Note that only lossy-page entries pointing to the current item's
		 * page should trigger this processing; we might have future lossy
		 * pages in the entry array, but they aren't relevant yet.
		 */
		ItemPointerSetLossyPage(&curPageLossy,
								GinItemPointerGetBlockNumber(&key->curItem));

		lossyEntry = 0;
		haveLossyEntry = false;
		for (i = 0; i < key->nentries; i++)
		{
			entry = key->scanEntry + i;
			if (entry->isFinished == FALSE &&
				compareItemPointers(&entry->curItem, &curPageLossy) == 0)
			{
				if (haveLossyEntry)
				{
					/* Multiple lossy entries, punt */
					key->curItem = curPageLossy;
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
				key->recheckCurItem = true;
				return;
			}
		}

		/*
		 * At this point we know that we don't need to return a lossy
		 * whole-page pointer, but we might have matches for individual exact
		 * item pointers, possibly in combination with a lossy pointer.  Our
		 * strategy if there's a lossy pointer is to try the consistentFn both
		 * ways and return a hit if it accepts either one (forcing the hit to
		 * be marked lossy so it will be rechecked).
		 *
		 * Prepare entryRes array to be passed to consistentFn.
		 *
		 * (If key->nentries == 1 then the consistentFn should always succeed,
		 * but we must call it anyway to find out the recheck status.)
		 */
		for (i = 0; i < key->nentries; i++)
		{
			entry = key->scanEntry + i;
			if (entry->isFinished == FALSE &&
				compareItemPointers(&entry->curItem, &key->curItem) == 0)
				key->entryRes[i] = TRUE;
			else
				key->entryRes[i] = FALSE;
		}
		if (haveLossyEntry)
			key->entryRes[lossyEntry] = TRUE;

		res = callConsistentFn(ginstate, key);

		if (!res && haveLossyEntry)
		{
			/* try the other way for the lossy item */
			key->entryRes[lossyEntry] = FALSE;

			res = callConsistentFn(ginstate, key);
		}

		/* clean up after consistentFn calls */
		MemoryContextSwitchTo(oldCtx);
		MemoryContextReset(tempCtx);

		/* If we matched a lossy entry, force recheckCurItem = true */
		if (haveLossyEntry)
			key->recheckCurItem = true;
	} while (!res);
}


/*
 * Get ItemPointer of next heap row to be checked from pending list.
 * Returns false if there are no more. On pages with several rows
 * it returns each row separately, on page with part of heap row returns
 * per page data.  pos->firstOffset and pos->lastOffset points
 * fraction of tuples for current heap row.
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
			 * row, pos->lastOffset points to the first tuple of second heap
			 * row (or to the end of page)
			 */

			break;
		}
	}

	return true;
}

/*
 * Scan page from current tuple (off) up till the first of:
 * - match is found (then returns true)
 * - no later match is possible
 * - tuple's attribute number is not equal to entry's attrnum
 * - reach end of page
 */
static bool
matchPartialInPendingList(GinState *ginstate, Page page,
						  OffsetNumber off, OffsetNumber maxoff,
						  Datum value, OffsetNumber attrnum,
						  Datum *datum, bool *datumExtracted,
						  StrategyNumber strategy,
						  Pointer extra_data)
{
	IndexTuple	itup;
	int32		cmp;

	while (off < maxoff)
	{
		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, off));
		if (attrnum != gintuple_get_attrnum(ginstate, itup))
			return false;

		if (datumExtracted[off - 1] == false)
		{
			datum[off - 1] = gin_index_getattr(ginstate, itup);
			datumExtracted[off - 1] = true;
		}

		/*----------
		 * Check partial match.
		 * case cmp == 0 => match
		 * case cmp > 0 => not match and end scan (no later match possible)
		 * case cmp < 0 => not match and continue scan
		 *----------
		 */
		cmp = DatumGetInt32(FunctionCall4(&ginstate->comparePartialFn[attrnum - 1],
										  value,
										  datum[off - 1],
										  UInt16GetDatum(strategy),
										  PointerGetDatum(extra_data)));
		if (cmp == 0)
			return true;
		else if (cmp > 0)
			return false;

		off++;
	}

	return false;
}

static bool
hasAllMatchingKeys(GinScanOpaque so, pendingPosition *pos)
{
	int			i;

	for (i = 0; i < so->nkeys; i++)
		if (pos->hasMatchKey[i] == false)
			return false;

	return true;
}

/*
 * Sets entryRes array for each key by looking at
 * every entry per indexed value (heap's row) in pending list.
 * returns true if at least one of datum was matched by key's entry
 *
 * The pendingBuffer is presumed pinned and share-locked on entry.
 */
static bool
collectDatumForItem(IndexScanDesc scan, pendingPosition *pos)
{
	GinScanOpaque so = (GinScanOpaque) scan->opaque;
	OffsetNumber attrnum;
	Page		page;
	IndexTuple	itup;
	int			i,
				j;

	/*
	 * Reset entryRes
	 */
	for (i = 0; i < so->nkeys; i++)
	{
		GinScanKey	key = so->keys + i;

		memset(key->entryRes, FALSE, key->nentries);
	}
	memset(pos->hasMatchKey, FALSE, so->nkeys);

	for (;;)
	{
		Datum		datum[BLCKSZ / sizeof(IndexTupleData)];
		bool		datumExtracted[BLCKSZ / sizeof(IndexTupleData)];

		Assert(pos->lastOffset > pos->firstOffset);
		memset(datumExtracted + pos->firstOffset - 1, 0, sizeof(bool) * (pos->lastOffset - pos->firstOffset));

		page = BufferGetPage(pos->pendingBuffer);

		for (i = 0; i < so->nkeys; i++)
		{
			GinScanKey	key = so->keys + i;

			for (j = 0; j < key->nentries; j++)
			{
				OffsetNumber StopLow = pos->firstOffset,
							StopHigh = pos->lastOffset,
							StopMiddle;
				GinScanEntry entry = key->scanEntry + j;

				/* already true - do not extra work */
				if (key->entryRes[j])
					continue;

				/*
				 * Interested tuples are from pos->firstOffset to
				 * pos->lastOffset and they are ordered by (attnum, Datum) as
				 * it's done in entry tree So we could use binary search to
				 * prevent linear scanning
				 */
				while (StopLow < StopHigh)
				{
					StopMiddle = StopLow + ((StopHigh - StopLow) >> 1);

					itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, StopMiddle));
					attrnum = gintuple_get_attrnum(&so->ginstate, itup);

					if (key->attnum < attrnum)
						StopHigh = StopMiddle;
					else if (key->attnum > attrnum)
						StopLow = StopMiddle + 1;
					else
					{
						int			res;

						if (datumExtracted[StopMiddle - 1] == false)
						{
							datum[StopMiddle - 1] = gin_index_getattr(&so->ginstate, itup);
							datumExtracted[StopMiddle - 1] = true;
						}
						res = compareEntries(&so->ginstate,
											 entry->attnum,
											 entry->entry,
											 datum[StopMiddle - 1]);

						if (res == 0)
						{
							/*
							 * The exact match causes, so we just scan from
							 * current position to find a partial match. See
							 * comment above about tuple's ordering.
							 */
							if (entry->isPartialMatch)
								key->entryRes[j] =
									matchPartialInPendingList(&so->ginstate,
															page, StopMiddle,
															  pos->lastOffset,
															  entry->entry,
															  entry->attnum,
															  datum,
															  datumExtracted,
															  entry->strategy,
														  entry->extra_data);
							else
								key->entryRes[j] = true;
							break;
						}
						else if (res < 0)
							StopHigh = StopMiddle;
						else
							StopLow = StopMiddle + 1;
					}
				}

				if (StopLow >= StopHigh && entry->isPartialMatch)
				{
					/*
					 * The exact match wasn't found, so we need to start scan
					 * from first tuple greater then current entry See comment
					 * above about tuple's ordering.
					 */
					key->entryRes[j] =
						matchPartialInPendingList(&so->ginstate,
												  page, StopHigh,
												  pos->lastOffset,
												  entry->entry,
												  entry->attnum,
												  datum,
												  datumExtracted,
												  entry->strategy,
												  entry->extra_data);
				}

				pos->hasMatchKey[i] |= key->entryRes[j];
			}
		}

		pos->firstOffset = pos->lastOffset;

		if (GinPageHasFullRow(page))
		{
			/*
			 * We scan all values from one tuple, go to next one
			 */

			return hasAllMatchingKeys(so, pos);
		}
		else
		{
			ItemPointerData item = pos->item;

			/*
			 * need to get next portion of tuples of row containing on several
			 * pages
			 */

			if (scanGetCandidate(scan, pos) == false || !ItemPointerEquals(&pos->item, &item))
				elog(ERROR, "Could not process tuple"); /* XXX should not be
														 * here ! */
		}
	}

	return hasAllMatchingKeys(so, pos);
}

/*
 * Collect all matched rows from pending list in bitmap
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
		 * Check entries in tuple and setup entryRes array If tuples of heap's
		 * row are placed on several pages collectDatumForItem will read all
		 * of that pages.
		 */
		if (!collectDatumForItem(scan, &pos))
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
	ItemPointerData myAdvancePast = *advancePast;
	uint32		i;
	bool		match;

	for (;;)
	{
		/*
		 * Advance any keys that are <= myAdvancePast.  In particular,
		 * since key->curItem was initialized with ItemPointerSetMin, this
		 * ensures we fetch the first item for each key on the first call.
		 * Then set *item to the minimum of the key curItems.
		 *
		 * Note: a lossy-page entry is encoded by a ItemPointer with max value
		 * for offset (0xffff), so that it will sort after any exact entries
		 * for the same page.  So we'll prefer to return exact pointers not
		 * lossy pointers, which is good.  Also, when we advance past an exact
		 * entry after processing it, we will not advance past lossy entries
		 * for the same page in other keys, which is NECESSARY for correct
		 * results (since we might have additional entries for the same page
		 * in the first key).
		 */
		ItemPointerSetMax(item);

		for (i = 0; i < so->nkeys; i++)
		{
			GinScanKey	key = so->keys + i;

			while (key->isFinished == FALSE &&
				   compareItemPointers(&key->curItem, &myAdvancePast) <= 0)
				keyGetItem(scan->indexRelation, &so->ginstate, so->tempCtx,
						   key, &myAdvancePast);

			if (key->isFinished)
					return FALSE;		/* finished one of keys */

			if (compareItemPointers(&key->curItem, item) < 0)
				*item = key->curItem;
		}

		Assert(!ItemPointerIsMax(item));

		/*----------
		 * Now *item contains first ItemPointer after previous result.
		 *
		 * The item is a valid hit only if all the keys returned either
		 * that exact TID, or a lossy reference to the same page.
		 *
		 * This logic works only if a keyGetItem stream can never contain both
		 * exact and lossy pointers for the same page.  Else we could have a
		 * case like
		 *
		 *		stream 1		stream 2
		 *		...             ...
		 *		42/6			42/7
		 *		50/1			42/0xffff
		 *		...             ...
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

			if (compareItemPointers(item, &key->curItem) == 0)
				continue;
			if (ItemPointerIsLossyPage(&key->curItem) &&
				GinItemPointerGetBlockNumber(&key->curItem) ==
				GinItemPointerGetBlockNumber(item))
				continue;
			match = false;
			break;
		}

		if (match)
			break;

		/*
		 * No hit.  Update myAdvancePast to this TID, so that on the next
		 * pass we'll move to the next possible entry.
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

#define GinIsNewKey(s)		( ((GinScanOpaque) scan->opaque)->keys == NULL )
#define GinIsVoidRes(s)		( ((GinScanOpaque) scan->opaque)->isVoidRes == true )

Datum
gingetbitmap(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	TIDBitmap  *tbm = (TIDBitmap *) PG_GETARG_POINTER(1);
	int64		ntids;
	ItemPointerData iptr;
	bool		recheck;

	if (GinIsNewKey(scan))
		newScanKey(scan);

	if (GinIsVoidRes(scan))
		PG_RETURN_INT64(0);

	ntids = 0;

	/*
	 * First, scan the pending list and collect any matching entries into the
	 * bitmap.  After we scan a pending item, some other backend could post it
	 * into the main index, and so we might visit it a second time during the
	 * main scan.  This is okay because we'll just re-set the same bit in the
	 * bitmap.  (The possibility of duplicate visits is a major reason why GIN
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
