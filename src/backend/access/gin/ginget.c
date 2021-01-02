/*-------------------------------------------------------------------------
 *
 * ginget.c
 *	  fetch tuples from a GIN scan.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
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
#include "storage/predicate.h"
#include "utils/datum.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/* GUC parameter */
int			GinFuzzySearchLimit = 0;

typedef struct pendingPosition
{
	Buffer		pendingBuffer;
	OffsetNumber firstOffset;
	OffsetNumber lastOffset;
	ItemPointerData item;
	bool	   *hasMatchKey;
} pendingPosition;


/*
 * Goes to the next page if current offset is outside of bounds
 */
static bool
moveRightIfItNeeded(GinBtreeData *btree, GinBtreeStack *stack, Snapshot snapshot)
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
		PredicateLockPage(btree->index, stack->blkno, snapshot);
	}

	return true;
}

/*
 * Scan all pages of a posting tree and save all its heap ItemPointers
 * in scanEntry->matchBitmap
 */
static void
scanPostingTree(Relation index, GinScanEntry scanEntry,
				BlockNumber rootPostingTree, Snapshot snapshot)
{
	GinBtreeData btree;
	GinBtreeStack *stack;
	Buffer		buffer;
	Page		page;

	/* Descend to the leftmost leaf page */
	stack = ginScanBeginPostingTree(&btree, index, rootPostingTree, snapshot);
	buffer = stack->buffer;

	IncrBufferRefCount(buffer); /* prevent unpin in freeGinBtreeStack */

	freeGinBtreeStack(stack);

	/*
	 * Loop iterates through all leaf pages of posting tree
	 */
	for (;;)
	{
		page = BufferGetPage(buffer);
		if ((GinPageGetOpaque(page)->flags & GIN_DELETED) == 0)
		{
			int			n = GinDataLeafPageGetItemsToTbm(page, scanEntry->matchBitmap);

			scanEntry->predictNumberResult += n;
		}

		if (GinPageRightMost(page))
			break;				/* no more pages */

		buffer = ginStepRight(buffer, index, GIN_SHARE);
	}

	UnlockReleaseBuffer(buffer);
}

/*
 * Collects TIDs into scanEntry->matchBitmap for all heap tuples that
 * match the search entry.  This supports three different match modes:
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
				   GinScanEntry scanEntry, Snapshot snapshot)
{
	OffsetNumber attnum;
	Form_pg_attribute attr;

	/* Initialize empty bitmap result */
	scanEntry->matchBitmap = tbm_create(work_mem * 1024L, NULL);

	/* Null query cannot partial-match anything */
	if (scanEntry->isPartialMatch &&
		scanEntry->queryCategory != GIN_CAT_NORM_KEY)
		return true;

	/* Locate tupdesc entry for key column (for attbyval/attlen data) */
	attnum = scanEntry->attnum;
	attr = TupleDescAttr(btree->ginstate->origTupdesc, attnum - 1);

	/*
	 * Predicate lock entry leaf page, following pages will be locked by
	 * moveRightIfItNeeded()
	 */
	PredicateLockPage(btree->index, stack->buffer, snapshot);

	for (;;)
	{
		Page		page;
		IndexTuple	itup;
		Datum		idatum;
		GinNullCategory icategory;

		/*
		 * stack->off points to the interested entry, buffer is already locked
		 */
		if (moveRightIfItNeeded(btree, stack, snapshot) == false)
			return true;

		page = BufferGetPage(stack->buffer);
		TestForOldSnapshot(snapshot, btree->index, page);
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
			 * last entry for a given attnum).  We do want to include NULL_KEY
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

			/*
			 * Acquire predicate lock on the posting tree.  We already hold a
			 * lock on the entry page, but insertions to the posting tree
			 * don't check for conflicts on that level.
			 */
			PredicateLockPage(btree->index, rootPostingTree, snapshot);

			/* Collect all the TIDs in this entry's posting tree */
			scanPostingTree(btree->index, scanEntry, rootPostingTree,
							snapshot);

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
				if (moveRightIfItNeeded(btree, stack, snapshot) == false)
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("failed to re-find tuple within index \"%s\"",
									RelationGetRelationName(btree->index))));

				page = BufferGetPage(stack->buffer);
				itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, stack->off));

				if (gintuple_get_attrnum(btree->ginstate, itup) == attnum)
				{
					Datum		newDatum;
					GinNullCategory newCategory;

					newDatum = gintuple_get_key(btree->ginstate, itup,
												&newCategory);

					if (ginCompareEntries(btree->ginstate, attnum,
										  newDatum, newCategory,
										  idatum, icategory) == 0)
						break;	/* Found! */
				}

				stack->off++;
			}

			if (icategory == GIN_CAT_NORM_KEY && !attr->attbyval)
				pfree(DatumGetPointer(idatum));
		}
		else
		{
			ItemPointer ipd;
			int			nipd;

			ipd = ginReadTuple(btree->ginstate, scanEntry->attnum, itup, &nipd);
			tbm_add_tuples(scanEntry->matchBitmap, ipd, nipd, false);
			scanEntry->predictNumberResult += GinGetNPosting(itup);
			pfree(ipd);
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
startScanEntry(GinState *ginstate, GinScanEntry entry, Snapshot snapshot)
{
	GinBtreeData btreeEntry;
	GinBtreeStack *stackEntry;
	Page		page;
	bool		needUnlock;

restartScanEntry:
	entry->buffer = InvalidBuffer;
	ItemPointerSetMin(&entry->curItem);
	entry->offset = InvalidOffsetNumber;
	if (entry->list)
		pfree(entry->list);
	entry->list = NULL;
	entry->nlist = 0;
	entry->matchBitmap = NULL;
	entry->matchResult = NULL;
	entry->reduceResult = false;
	entry->predictNumberResult = 0;

	/*
	 * we should find entry, and begin scan of posting tree or just store
	 * posting list in memory
	 */
	ginPrepareEntryScan(&btreeEntry, entry->attnum,
						entry->queryKey, entry->queryCategory,
						ginstate);
	stackEntry = ginFindLeafPage(&btreeEntry, true, false, snapshot);
	page = BufferGetPage(stackEntry->buffer);

	/* ginFindLeafPage() will have already checked snapshot age. */
	needUnlock = true;

	entry->isFinished = true;

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
		if (collectMatchBitmap(&btreeEntry, stackEntry, entry, snapshot)
			== false)
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
			entry->isFinished = false;
		}
	}
	else if (btreeEntry.findItem(&btreeEntry, stackEntry))
	{
		IndexTuple	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, stackEntry->off));

		if (GinIsPostingTree(itup))
		{
			BlockNumber rootPostingTree = GinGetPostingTree(itup);
			GinBtreeStack *stack;
			Page		page;
			ItemPointerData minItem;

			/*
			 * This is an equality scan, so lock the root of the posting tree.
			 * It represents a lock on the exact key value, and covers all the
			 * items in the posting tree.
			 */
			PredicateLockPage(ginstate->index, rootPostingTree, snapshot);

			/*
			 * We should unlock entry page before touching posting tree to
			 * prevent deadlocks with vacuum processes. Because entry is never
			 * deleted from page and posting tree is never reduced to the
			 * posting list, we can unlock page after getting BlockNumber of
			 * root of posting tree.
			 */
			LockBuffer(stackEntry->buffer, GIN_UNLOCK);
			needUnlock = false;

			stack = ginScanBeginPostingTree(&entry->btree, ginstate->index,
											rootPostingTree, snapshot);
			entry->buffer = stack->buffer;

			/*
			 * We keep buffer pinned because we need to prevent deletion of
			 * page during scan. See GIN's vacuum implementation. RefCount is
			 * increased to keep buffer pinned after freeGinBtreeStack() call.
			 */
			IncrBufferRefCount(entry->buffer);

			page = BufferGetPage(entry->buffer);

			/*
			 * Load the first page into memory.
			 */
			ItemPointerSetMin(&minItem);
			entry->list = GinDataLeafPageGetItems(page, &entry->nlist, minItem);

			entry->predictNumberResult = stack->predictNumber * entry->nlist;

			LockBuffer(entry->buffer, GIN_UNLOCK);
			freeGinBtreeStack(stack);
			entry->isFinished = false;
		}
		else
		{
			/*
			 * Lock the entry leaf page.  This is more coarse-grained than
			 * necessary, because it will conflict with any insertions that
			 * land on the same leaf page, not only the exact key we searched
			 * for.  But locking an individual tuple would require updating
			 * that lock whenever it moves because of insertions or vacuums,
			 * which seems too complicated.
			 */
			PredicateLockPage(ginstate->index,
							  BufferGetBlockNumber(stackEntry->buffer),
							  snapshot);
			if (GinGetNPosting(itup) > 0)
			{
				entry->list = ginReadTuple(ginstate, entry->attnum, itup,
										   &entry->nlist);
				entry->predictNumberResult = entry->nlist;

				entry->isFinished = false;
			}
		}
	}
	else
	{
		/*
		 * No entry found.  Predicate lock the leaf page, to lock the place
		 * where the entry would've been, had there been one.
		 */
		PredicateLockPage(ginstate->index,
						  BufferGetBlockNumber(stackEntry->buffer), snapshot);
	}

	if (needUnlock)
		LockBuffer(stackEntry->buffer, GIN_UNLOCK);
	freeGinBtreeStack(stackEntry);
}

/*
 * Comparison function for scan entry indexes. Sorts by predictNumberResult,
 * least frequent items first.
 */
static int
entryIndexByFrequencyCmp(const void *a1, const void *a2, void *arg)
{
	const GinScanKey key = (const GinScanKey) arg;
	int			i1 = *(const int *) a1;
	int			i2 = *(const int *) a2;
	uint32		n1 = key->scanEntry[i1]->predictNumberResult;
	uint32		n2 = key->scanEntry[i2]->predictNumberResult;

	if (n1 < n2)
		return -1;
	else if (n1 == n2)
		return 0;
	else
		return 1;
}

static void
startScanKey(GinState *ginstate, GinScanOpaque so, GinScanKey key)
{
	MemoryContext oldCtx = CurrentMemoryContext;
	int			i;
	int			j;
	int		   *entryIndexes;

	ItemPointerSetMin(&key->curItem);
	key->curItemMatches = false;
	key->recheckCurItem = false;
	key->isFinished = false;

	/*
	 * Divide the entries into two distinct sets: required and additional.
	 * Additional entries are not enough for a match alone, without any items
	 * from the required set, but are needed by the consistent function to
	 * decide if an item matches. When scanning, we can skip over items from
	 * additional entries that have no corresponding matches in any of the
	 * required entries. That speeds up queries like "frequent & rare"
	 * considerably, if the frequent term can be put in the additional set.
	 *
	 * There can be many legal ways to divide them entries into these two
	 * sets. A conservative division is to just put everything in the required
	 * set, but the more you can put in the additional set, the more you can
	 * skip during the scan. To maximize skipping, we try to put as many
	 * frequent items as possible into additional, and less frequent ones into
	 * required. To do that, sort the entries by frequency
	 * (predictNumberResult), and put entries into the required set in that
	 * order, until the consistent function says that none of the remaining
	 * entries can form a match, without any items from the required set. The
	 * rest go to the additional set.
	 *
	 * Exclude-only scan keys are known to have no required entries.
	 */
	if (key->excludeOnly)
	{
		MemoryContextSwitchTo(so->keyCtx);

		key->nrequired = 0;
		key->nadditional = key->nentries;
		key->additionalEntries = palloc(key->nadditional * sizeof(GinScanEntry));
		for (i = 0; i < key->nadditional; i++)
			key->additionalEntries[i] = key->scanEntry[i];
	}
	else if (key->nentries > 1)
	{
		MemoryContextSwitchTo(so->tempCtx);

		entryIndexes = (int *) palloc(sizeof(int) * key->nentries);
		for (i = 0; i < key->nentries; i++)
			entryIndexes[i] = i;
		qsort_arg(entryIndexes, key->nentries, sizeof(int),
				  entryIndexByFrequencyCmp, key);

		for (i = 0; i < key->nentries - 1; i++)
		{
			/* Pass all entries <= i as FALSE, and the rest as MAYBE */
			for (j = 0; j <= i; j++)
				key->entryRes[entryIndexes[j]] = GIN_FALSE;
			for (j = i + 1; j < key->nentries; j++)
				key->entryRes[entryIndexes[j]] = GIN_MAYBE;

			if (key->triConsistentFn(key) == GIN_FALSE)
				break;
		}
		/* i is now the last required entry. */

		MemoryContextSwitchTo(so->keyCtx);

		key->nrequired = i + 1;
		key->nadditional = key->nentries - key->nrequired;
		key->requiredEntries = palloc(key->nrequired * sizeof(GinScanEntry));
		key->additionalEntries = palloc(key->nadditional * sizeof(GinScanEntry));

		j = 0;
		for (i = 0; i < key->nrequired; i++)
			key->requiredEntries[i] = key->scanEntry[entryIndexes[j++]];
		for (i = 0; i < key->nadditional; i++)
			key->additionalEntries[i] = key->scanEntry[entryIndexes[j++]];

		/* clean up after consistentFn calls (also frees entryIndexes) */
		MemoryContextReset(so->tempCtx);
	}
	else
	{
		MemoryContextSwitchTo(so->keyCtx);

		key->nrequired = 1;
		key->nadditional = 0;
		key->requiredEntries = palloc(1 * sizeof(GinScanEntry));
		key->requiredEntries[0] = key->scanEntry[0];
	}
	MemoryContextSwitchTo(oldCtx);
}

static void
startScan(IndexScanDesc scan)
{
	GinScanOpaque so = (GinScanOpaque) scan->opaque;
	GinState   *ginstate = &so->ginstate;
	uint32		i;

	for (i = 0; i < so->totalentries; i++)
		startScanEntry(ginstate, so->entries[i], scan->xs_snapshot);

	if (GinFuzzySearchLimit > 0)
	{
		/*
		 * If all of keys more than threshold we will try to reduce result, we
		 * hope (and only hope, for intersection operation of array our
		 * supposition isn't true), that total result will not more than
		 * minimal predictNumberResult.
		 */
		bool		reduce = true;

		for (i = 0; i < so->totalentries; i++)
		{
			if (so->entries[i]->predictNumberResult <= so->totalentries * GinFuzzySearchLimit)
			{
				reduce = false;
				break;
			}
		}
		if (reduce)
		{
			for (i = 0; i < so->totalentries; i++)
			{
				so->entries[i]->predictNumberResult /= so->totalentries;
				so->entries[i]->reduceResult = true;
			}
		}
	}

	/*
	 * Now that we have the estimates for the entry frequencies, finish
	 * initializing the scan keys.
	 */
	for (i = 0; i < so->nkeys; i++)
		startScanKey(ginstate, so, so->keys + i);
}

/*
 * Load the next batch of item pointers from a posting tree.
 *
 * Note that we copy the page into GinScanEntry->list array and unlock it, but
 * keep it pinned to prevent interference with vacuum.
 */
static void
entryLoadMoreItems(GinState *ginstate, GinScanEntry entry,
				   ItemPointerData advancePast, Snapshot snapshot)
{
	Page		page;
	int			i;
	bool		stepright;

	if (!BufferIsValid(entry->buffer))
	{
		entry->isFinished = true;
		return;
	}

	/*
	 * We have two strategies for finding the correct page: step right from
	 * the current page, or descend the tree again from the root. If
	 * advancePast equals the current item, the next matching item should be
	 * on the next page, so we step right. Otherwise, descend from root.
	 */
	if (ginCompareItemPointers(&entry->curItem, &advancePast) == 0)
	{
		stepright = true;
		LockBuffer(entry->buffer, GIN_SHARE);
	}
	else
	{
		GinBtreeStack *stack;

		ReleaseBuffer(entry->buffer);

		/*
		 * Set the search key, and find the correct leaf page.
		 */
		if (ItemPointerIsLossyPage(&advancePast))
		{
			ItemPointerSet(&entry->btree.itemptr,
						   GinItemPointerGetBlockNumber(&advancePast) + 1,
						   FirstOffsetNumber);
		}
		else
		{
			ItemPointerSet(&entry->btree.itemptr,
						   GinItemPointerGetBlockNumber(&advancePast),
						   OffsetNumberNext(GinItemPointerGetOffsetNumber(&advancePast)));
		}
		entry->btree.fullScan = false;
		stack = ginFindLeafPage(&entry->btree, true, false, snapshot);

		/* we don't need the stack, just the buffer. */
		entry->buffer = stack->buffer;
		IncrBufferRefCount(entry->buffer);
		freeGinBtreeStack(stack);
		stepright = false;
	}

	elog(DEBUG2, "entryLoadMoreItems, %u/%u, skip: %d",
		 GinItemPointerGetBlockNumber(&advancePast),
		 GinItemPointerGetOffsetNumber(&advancePast),
		 !stepright);

	page = BufferGetPage(entry->buffer);
	for (;;)
	{
		entry->offset = InvalidOffsetNumber;
		if (entry->list)
		{
			pfree(entry->list);
			entry->list = NULL;
			entry->nlist = 0;
		}

		if (stepright)
		{
			/*
			 * We've processed all the entries on this page. If it was the
			 * last page in the tree, we're done.
			 */
			if (GinPageRightMost(page))
			{
				UnlockReleaseBuffer(entry->buffer);
				entry->buffer = InvalidBuffer;
				entry->isFinished = true;
				return;
			}

			/*
			 * Step to next page, following the right link. then find the
			 * first ItemPointer greater than advancePast.
			 */
			entry->buffer = ginStepRight(entry->buffer,
										 ginstate->index,
										 GIN_SHARE);
			page = BufferGetPage(entry->buffer);
		}
		stepright = true;

		if (GinPageGetOpaque(page)->flags & GIN_DELETED)
			continue;			/* page was deleted by concurrent vacuum */

		/*
		 * The first item > advancePast might not be on this page, but
		 * somewhere to the right, if the page was split, or a non-match from
		 * another key in the query allowed us to skip some items from this
		 * entry. Keep following the right-links until we re-find the correct
		 * page.
		 */
		if (!GinPageRightMost(page) &&
			ginCompareItemPointers(&advancePast, GinDataPageGetRightBound(page)) >= 0)
		{
			/*
			 * the item we're looking is > the right bound of the page, so it
			 * can't be on this page.
			 */
			continue;
		}

		entry->list = GinDataLeafPageGetItems(page, &entry->nlist, advancePast);

		for (i = 0; i < entry->nlist; i++)
		{
			if (ginCompareItemPointers(&advancePast, &entry->list[i]) < 0)
			{
				entry->offset = i;

				if (GinPageRightMost(page))
				{
					/* after processing the copied items, we're done. */
					UnlockReleaseBuffer(entry->buffer);
					entry->buffer = InvalidBuffer;
				}
				else
					LockBuffer(entry->buffer, GIN_UNLOCK);
				return;
			}
		}
	}
}

#define gin_rand() (((double) random()) / ((double) MAX_RANDOM_VALUE))
#define dropItem(e) ( gin_rand() > ((double)GinFuzzySearchLimit)/((double)((e)->predictNumberResult)) )

/*
 * Sets entry->curItem to next heap item pointer > advancePast, for one entry
 * of one scan key, or sets entry->isFinished to true if there are no more.
 *
 * Item pointers are returned in ascending order.
 *
 * Note: this can return a "lossy page" item pointer, indicating that the
 * entry potentially matches all items on that heap page.  However, it is
 * not allowed to return both a lossy page pointer and exact (regular)
 * item pointers for the same page.  (Doing so would break the key-combination
 * logic in keyGetItem and scanGetItem; see comment in scanGetItem.)  In the
 * current implementation this is guaranteed by the behavior of tidbitmaps.
 */
static void
entryGetItem(GinState *ginstate, GinScanEntry entry,
			 ItemPointerData advancePast, Snapshot snapshot)
{
	Assert(!entry->isFinished);

	Assert(!ItemPointerIsValid(&entry->curItem) ||
		   ginCompareItemPointers(&entry->curItem, &advancePast) <= 0);

	if (entry->matchBitmap)
	{
		/* A bitmap result */
		BlockNumber advancePastBlk = GinItemPointerGetBlockNumber(&advancePast);
		OffsetNumber advancePastOff = GinItemPointerGetOffsetNumber(&advancePast);

		for (;;)
		{
			/*
			 * If we've exhausted all items on this block, move to next block
			 * in the bitmap.
			 */
			while (entry->matchResult == NULL ||
				   (entry->matchResult->ntuples >= 0 &&
					entry->offset >= entry->matchResult->ntuples) ||
				   entry->matchResult->blockno < advancePastBlk ||
				   (ItemPointerIsLossyPage(&advancePast) &&
					entry->matchResult->blockno == advancePastBlk))
			{
				entry->matchResult = tbm_iterate(entry->matchIterator);

				if (entry->matchResult == NULL)
				{
					ItemPointerSetInvalid(&entry->curItem);
					tbm_end_iterate(entry->matchIterator);
					entry->matchIterator = NULL;
					entry->isFinished = true;
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
			if (entry->isFinished)
				break;

			/*
			 * We're now on the first page after advancePast which has any
			 * items on it. If it's a lossy result, return that.
			 */
			if (entry->matchResult->ntuples < 0)
			{
				ItemPointerSetLossyPage(&entry->curItem,
										entry->matchResult->blockno);

				/*
				 * We might as well fall out of the loop; we could not
				 * estimate number of results on this page to support correct
				 * reducing of result even if it's enabled.
				 */
				break;
			}

			/*
			 * Not a lossy page. Skip over any offsets <= advancePast, and
			 * return that.
			 */
			if (entry->matchResult->blockno == advancePastBlk)
			{
				/*
				 * First, do a quick check against the last offset on the
				 * page. If that's > advancePast, so are all the other
				 * offsets, so just go back to the top to get the next page.
				 */
				if (entry->matchResult->offsets[entry->matchResult->ntuples - 1] <= advancePastOff)
				{
					entry->offset = entry->matchResult->ntuples;
					continue;
				}

				/* Otherwise scan to find the first item > advancePast */
				while (entry->matchResult->offsets[entry->offset] <= advancePastOff)
					entry->offset++;
			}

			ItemPointerSet(&entry->curItem,
						   entry->matchResult->blockno,
						   entry->matchResult->offsets[entry->offset]);
			entry->offset++;

			/* Done unless we need to reduce the result */
			if (!entry->reduceResult || !dropItem(entry))
				break;
		}
	}
	else if (!BufferIsValid(entry->buffer))
	{
		/*
		 * A posting list from an entry tuple, or the last page of a posting
		 * tree.
		 */
		for (;;)
		{
			if (entry->offset >= entry->nlist)
			{
				ItemPointerSetInvalid(&entry->curItem);
				entry->isFinished = true;
				break;
			}

			entry->curItem = entry->list[entry->offset++];

			/* If we're not past advancePast, keep scanning */
			if (ginCompareItemPointers(&entry->curItem, &advancePast) <= 0)
				continue;

			/* Done unless we need to reduce the result */
			if (!entry->reduceResult || !dropItem(entry))
				break;
		}
	}
	else
	{
		/* A posting tree */
		for (;;)
		{
			/* If we've processed the current batch, load more items */
			while (entry->offset >= entry->nlist)
			{
				entryLoadMoreItems(ginstate, entry, advancePast, snapshot);

				if (entry->isFinished)
				{
					ItemPointerSetInvalid(&entry->curItem);
					return;
				}
			}

			entry->curItem = entry->list[entry->offset++];

			/* If we're not past advancePast, keep scanning */
			if (ginCompareItemPointers(&entry->curItem, &advancePast) <= 0)
				continue;

			/* Done unless we need to reduce the result */
			if (!entry->reduceResult || !dropItem(entry))
				break;

			/*
			 * Advance advancePast (so that entryLoadMoreItems will load the
			 * right data), and keep scanning
			 */
			advancePast = entry->curItem;
		}
	}
}

/*
 * Identify the "current" item among the input entry streams for this scan key
 * that is greater than advancePast, and test whether it passes the scan key
 * qual condition.
 *
 * The current item is the smallest curItem among the inputs.  key->curItem
 * is set to that value.  key->curItemMatches is set to indicate whether that
 * TID passes the consistentFn test.  If so, key->recheckCurItem is set true
 * iff recheck is needed for this item pointer (including the case where the
 * item pointer is a lossy page pointer).
 *
 * If all entry streams are exhausted, sets key->isFinished to true.
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
keyGetItem(GinState *ginstate, MemoryContext tempCtx, GinScanKey key,
		   ItemPointerData advancePast, Snapshot snapshot)
{
	ItemPointerData minItem;
	ItemPointerData curPageLossy;
	uint32		i;
	bool		haveLossyEntry;
	GinScanEntry entry;
	GinTernaryValue res;
	MemoryContext oldCtx;
	bool		allFinished;

	Assert(!key->isFinished);

	/*
	 * We might have already tested this item; if so, no need to repeat work.
	 * (Note: the ">" case can happen, if advancePast is exact but we
	 * previously had to set curItem to a lossy-page pointer.)
	 */
	if (ginCompareItemPointers(&key->curItem, &advancePast) > 0)
		return;

	/*
	 * Find the minimum item > advancePast among the active entry streams.
	 *
	 * Note: a lossy-page entry is encoded by a ItemPointer with max value for
	 * offset (0xffff), so that it will sort after any exact entries for the
	 * same page.  So we'll prefer to return exact pointers not lossy
	 * pointers, which is good.
	 */
	ItemPointerSetMax(&minItem);
	allFinished = true;
	for (i = 0; i < key->nrequired; i++)
	{
		entry = key->requiredEntries[i];

		if (entry->isFinished)
			continue;

		/*
		 * Advance this stream if necessary.
		 *
		 * In particular, since entry->curItem was initialized with
		 * ItemPointerSetMin, this ensures we fetch the first item for each
		 * entry on the first call.
		 */
		if (ginCompareItemPointers(&entry->curItem, &advancePast) <= 0)
		{
			entryGetItem(ginstate, entry, advancePast, snapshot);
			if (entry->isFinished)
				continue;
		}

		allFinished = false;
		if (ginCompareItemPointers(&entry->curItem, &minItem) < 0)
			minItem = entry->curItem;
	}

	if (allFinished && !key->excludeOnly)
	{
		/* all entries are finished */
		key->isFinished = true;
		return;
	}

	if (!key->excludeOnly)
	{
		/*
		 * For a normal scan key, we now know there are no matches < minItem.
		 *
		 * If minItem is lossy, it means that there were no exact items on the
		 * page among requiredEntries, because lossy pointers sort after exact
		 * items. However, there might be exact items for the same page among
		 * additionalEntries, so we mustn't advance past them.
		 */
		if (ItemPointerIsLossyPage(&minItem))
		{
			if (GinItemPointerGetBlockNumber(&advancePast) <
				GinItemPointerGetBlockNumber(&minItem))
			{
				ItemPointerSet(&advancePast,
							   GinItemPointerGetBlockNumber(&minItem),
							   InvalidOffsetNumber);
			}
		}
		else
		{
			Assert(GinItemPointerGetOffsetNumber(&minItem) > 0);
			ItemPointerSet(&advancePast,
						   GinItemPointerGetBlockNumber(&minItem),
						   OffsetNumberPrev(GinItemPointerGetOffsetNumber(&minItem)));
		}
	}
	else
	{
		/*
		 * excludeOnly scan keys don't have any entries that are necessarily
		 * present in matching items.  So, we consider the item just after
		 * advancePast.
		 */
		Assert(key->nrequired == 0);
		ItemPointerSet(&minItem,
					   GinItemPointerGetBlockNumber(&advancePast),
					   OffsetNumberNext(GinItemPointerGetOffsetNumber(&advancePast)));
	}

	/*
	 * We might not have loaded all the entry streams for this TID yet. We
	 * could call the consistent function, passing MAYBE for those entries, to
	 * see if it can decide if this TID matches based on the information we
	 * have. But if the consistent-function is expensive, and cannot in fact
	 * decide with partial information, that could be a big loss. So, load all
	 * the additional entries, before calling the consistent function.
	 */
	for (i = 0; i < key->nadditional; i++)
	{
		entry = key->additionalEntries[i];

		if (entry->isFinished)
			continue;

		if (ginCompareItemPointers(&entry->curItem, &advancePast) <= 0)
		{
			entryGetItem(ginstate, entry, advancePast, snapshot);
			if (entry->isFinished)
				continue;
		}

		/*
		 * Normally, none of the items in additionalEntries can have a curItem
		 * larger than minItem. But if minItem is a lossy page, then there
		 * might be exact items on the same page among additionalEntries.
		 */
		if (ginCompareItemPointers(&entry->curItem, &minItem) < 0)
		{
			Assert(ItemPointerIsLossyPage(&minItem));
			minItem = entry->curItem;
		}
	}

	/*
	 * Ok, we've advanced all the entries up to minItem now. Set key->curItem,
	 * and perform consistentFn test.
	 *
	 * Lossy-page entries pose a problem, since we don't know the correct
	 * entryRes state to pass to the consistentFn, and we also don't know what
	 * its combining logic will be (could be AND, OR, or even NOT). If the
	 * logic is OR then the consistentFn might succeed for all items in the
	 * lossy page even when none of the other entries match.
	 *
	 * Our strategy is to call the tri-state consistent function, with the
	 * lossy-page entries set to MAYBE, and all the other entries FALSE. If it
	 * returns FALSE, none of the lossy items alone are enough for a match, so
	 * we don't need to return a lossy-page pointer. Otherwise, return a
	 * lossy-page pointer to indicate that the whole heap page must be
	 * checked.  (On subsequent calls, we'll do nothing until minItem is past
	 * the page altogether, thus ensuring that we never return both regular
	 * and lossy pointers for the same page.)
	 *
	 * An exception is that it doesn't matter what we pass for lossy pointers
	 * in "hidden" entries, because the consistentFn's result can't depend on
	 * them. We could pass them as MAYBE as well, but if we're using the
	 * "shim" implementation of a tri-state consistent function (see
	 * ginlogic.c), it's better to pass as few MAYBEs as possible. So pass
	 * them as true.
	 *
	 * Note that only lossy-page entries pointing to the current item's page
	 * should trigger this processing; we might have future lossy pages in the
	 * entry array, but they aren't relevant yet.
	 */
	key->curItem = minItem;
	ItemPointerSetLossyPage(&curPageLossy,
							GinItemPointerGetBlockNumber(&key->curItem));
	haveLossyEntry = false;
	for (i = 0; i < key->nentries; i++)
	{
		entry = key->scanEntry[i];
		if (entry->isFinished == false &&
			ginCompareItemPointers(&entry->curItem, &curPageLossy) == 0)
		{
			if (i < key->nuserentries)
				key->entryRes[i] = GIN_MAYBE;
			else
				key->entryRes[i] = GIN_TRUE;
			haveLossyEntry = true;
		}
		else
			key->entryRes[i] = GIN_FALSE;
	}

	/* prepare for calling consistentFn in temp context */
	oldCtx = MemoryContextSwitchTo(tempCtx);

	if (haveLossyEntry)
	{
		/* Have lossy-page entries, so see if whole page matches */
		res = key->triConsistentFn(key);

		if (res == GIN_TRUE || res == GIN_MAYBE)
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
	 * possibly in combination with a lossy pointer. Pass lossy pointers as
	 * MAYBE to the ternary consistent function, to let it decide if this
	 * tuple satisfies the overall key, even though we don't know if the lossy
	 * entries match.
	 *
	 * Prepare entryRes array to be passed to consistentFn.
	 */
	for (i = 0; i < key->nentries; i++)
	{
		entry = key->scanEntry[i];
		if (entry->isFinished)
			key->entryRes[i] = GIN_FALSE;
#if 0

		/*
		 * This case can't currently happen, because we loaded all the entries
		 * for this item earlier.
		 */
		else if (ginCompareItemPointers(&entry->curItem, &advancePast) <= 0)
			key->entryRes[i] = GIN_MAYBE;
#endif
		else if (ginCompareItemPointers(&entry->curItem, &curPageLossy) == 0)
			key->entryRes[i] = GIN_MAYBE;
		else if (ginCompareItemPointers(&entry->curItem, &minItem) == 0)
			key->entryRes[i] = GIN_TRUE;
		else
			key->entryRes[i] = GIN_FALSE;
	}

	res = key->triConsistentFn(key);

	switch (res)
	{
		case GIN_TRUE:
			key->curItemMatches = true;
			/* triConsistentFn set recheckCurItem */
			break;

		case GIN_FALSE:
			key->curItemMatches = false;
			break;

		case GIN_MAYBE:
			key->curItemMatches = true;
			key->recheckCurItem = true;
			break;

		default:

			/*
			 * the 'default' case shouldn't happen, but if the consistent
			 * function returns something bogus, this is the safe result
			 */
			key->curItemMatches = true;
			key->recheckCurItem = true;
			break;
	}

	/*
	 * We have a tuple, and we know if it matches or not. If it's a non-match,
	 * we could continue to find the next matching tuple, but let's break out
	 * and give scanGetItem a chance to advance the other keys. They might be
	 * able to skip past to a much higher TID, allowing us to save work.
	 */

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
scanGetItem(IndexScanDesc scan, ItemPointerData advancePast,
			ItemPointerData *item, bool *recheck)
{
	GinScanOpaque so = (GinScanOpaque) scan->opaque;
	uint32		i;
	bool		match;

	/*----------
	 * Advance the scan keys in lock-step, until we find an item that matches
	 * all the keys. If any key reports isFinished, meaning its subset of the
	 * entries is exhausted, we can stop.  Otherwise, set *item to the next
	 * matching item.
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
	do
	{
		ItemPointerSetMin(item);
		match = true;
		for (i = 0; i < so->nkeys && match; i++)
		{
			GinScanKey	key = so->keys + i;

			/*
			 * If we're considering a lossy page, skip excludeOnly keys,  They
			 * can't exclude the whole page anyway.
			 */
			if (ItemPointerIsLossyPage(item) && key->excludeOnly)
			{
				/*
				 * ginNewScanKey() should never mark the first key as
				 * excludeOnly.
				 */
				Assert(i > 0);
				continue;
			}

			/* Fetch the next item for this key that is > advancePast. */
			keyGetItem(&so->ginstate, so->tempCtx, key, advancePast,
					   scan->xs_snapshot);

			if (key->isFinished)
				return false;

			/*
			 * If it's not a match, we can immediately conclude that nothing
			 * <= this item matches, without checking the rest of the keys.
			 */
			if (!key->curItemMatches)
			{
				advancePast = key->curItem;
				match = false;
				break;
			}

			/*
			 * It's a match. We can conclude that nothing < matches, so the
			 * other key streams can skip to this item.
			 *
			 * Beware of lossy pointers, though; from a lossy pointer, we can
			 * only conclude that nothing smaller than this *block* matches.
			 */
			if (ItemPointerIsLossyPage(&key->curItem))
			{
				if (GinItemPointerGetBlockNumber(&advancePast) <
					GinItemPointerGetBlockNumber(&key->curItem))
				{
					ItemPointerSet(&advancePast,
								   GinItemPointerGetBlockNumber(&key->curItem),
								   InvalidOffsetNumber);
				}
			}
			else
			{
				Assert(GinItemPointerGetOffsetNumber(&key->curItem) > 0);
				ItemPointerSet(&advancePast,
							   GinItemPointerGetBlockNumber(&key->curItem),
							   OffsetNumberPrev(GinItemPointerGetOffsetNumber(&key->curItem)));
			}

			/*
			 * If this is the first key, remember this location as a potential
			 * match, and proceed to check the rest of the keys.
			 *
			 * Otherwise, check if this is the same item that we checked the
			 * previous keys for (or a lossy pointer for the same page). If
			 * not, loop back to check the previous keys for this item (we
			 * will check this key again too, but keyGetItem returns quickly
			 * for that)
			 */
			if (i == 0)
			{
				*item = key->curItem;
			}
			else
			{
				if (ItemPointerIsLossyPage(&key->curItem) ||
					ItemPointerIsLossyPage(item))
				{
					Assert(GinItemPointerGetBlockNumber(&key->curItem) >= GinItemPointerGetBlockNumber(item));
					match = (GinItemPointerGetBlockNumber(&key->curItem) ==
							 GinItemPointerGetBlockNumber(item));
				}
				else
				{
					Assert(ginCompareItemPointers(&key->curItem, item) >= 0);
					match = (ginCompareItemPointers(&key->curItem, item) == 0);
				}
			}
		}
	} while (!match);

	Assert(!ItemPointerIsMin(item));

	/*
	 * Now *item contains the first ItemPointer after previous result that
	 * satisfied all the keys for that exact TID, or a lossy reference to the
	 * same page.
	 *
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

	return true;
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
		TestForOldSnapshot(scan->xs_snapshot, scan->indexRelation, page);

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

		memset(key->entryRes, GIN_FALSE, key->nentries);
	}
	memset(pos->hasMatchKey, false, so->nkeys);

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
		TestForOldSnapshot(scan->xs_snapshot, scan->indexRelation, page);

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
	 * All scan keys except excludeOnly require at least one entry to match.
	 * excludeOnly keys are an exception, because their implied
	 * GIN_CAT_EMPTY_QUERY scanEntry always matches.  So return "true" if all
	 * non-excludeOnly scan keys have at least one match.
	 */
	for (i = 0; i < so->nkeys; i++)
	{
		if (pos->hasMatchKey[i] == false && !so->keys[i].excludeOnly)
			return false;
	}

	return true;
}

/*
 * Collect all matched rows from pending list into bitmap.
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
	Page		page;
	BlockNumber blkno;

	*ntids = 0;

	/*
	 * Acquire predicate lock on the metapage, to conflict with any fastupdate
	 * insertions.
	 */
	PredicateLockPage(scan->indexRelation, GIN_METAPAGE_BLKNO, scan->xs_snapshot);

	LockBuffer(metabuffer, GIN_SHARE);
	page = BufferGetPage(metabuffer);
	TestForOldSnapshot(scan->xs_snapshot, scan->indexRelation, page);
	blkno = GinPageGetMeta(page)->head;

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

			if (!key->boolConsistentFn(key))
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


#define GinIsVoidRes(s)		( ((GinScanOpaque) scan->opaque)->isVoidRes )

int64
gingetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	GinScanOpaque so = (GinScanOpaque) scan->opaque;
	int64		ntids;
	ItemPointerData iptr;
	bool		recheck;

	/*
	 * Set up the scan keys, and check for unsatisfiable query.
	 */
	ginFreeScanKeys(so);		/* there should be no keys yet, but just to be
								 * sure */
	ginNewScanKey(scan);

	if (GinIsVoidRes(scan))
		return 0;

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

		if (!scanGetItem(scan, iptr, &iptr, &recheck))
			break;

		if (ItemPointerIsLossyPage(&iptr))
			tbm_add_page(tbm, ItemPointerGetBlockNumber(&iptr));
		else
			tbm_add_tuples(tbm, &iptr, 1, recheck);
		ntids++;
	}

	return ntids;
}
