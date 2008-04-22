/*-------------------------------------------------------------------------
 *
 * ginget.c
 *	  fetch tuples from a GIN scan.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			$PostgreSQL: pgsql/src/backend/access/gin/ginget.c,v 1.4.2.2 2008/04/22 17:53:41 teodor Exp $
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "access/gin.h"
#include "catalog/index.h"
#include "utils/memutils.h"

static bool
findItemInPage(Page page, ItemPointer item, OffsetNumber *off)
{
	OffsetNumber maxoff = GinPageGetOpaque(page)->maxoff;
	int			res;

	if ( GinPageGetOpaque(page)->flags & GIN_DELETED )
		/* page was deleted by concurrent  vacuum */
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
 * Start* functions setup state of searches: find correct buffer and locks it,
 * Stop* functions unlock buffer (but don't release!)
 */
static void
startScanEntry(Relation index, GinState *ginstate, GinScanEntry entry)
{
	GinBtreeData    btreeEntry;
	GinBtreeStack  *stackEntry;
	Page            page;
	bool            needUnlock = TRUE;

	if (entry->master != NULL)
	{
		entry->isFinished = entry->master->isFinished;
		return;
	}

	/*
	 * We should find entry, and begin scan of posting tree
	 * or just store posting list in memory
	 */

	prepareEntryScan(&btreeEntry, index, entry->entry, ginstate);
	btreeEntry.searchMode = TRUE;
	stackEntry = ginFindLeafPage(&btreeEntry, NULL);
	page = BufferGetPage(stackEntry->buffer);

	entry->isFinished = TRUE;
	entry->buffer = InvalidBuffer;
	entry->offset = InvalidOffsetNumber;
	entry->list = NULL;
	entry->nlist = 0;
	entry->reduceResult = FALSE;
	entry->predictNumberResult = 0;

	if (btreeEntry.findItem(&btreeEntry, stackEntry))
	{
		IndexTuple	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, stackEntry->off));

		if (GinIsPostingTree(itup))
		{
			BlockNumber rootPostingTree = GinGetPostingTree(itup);
			GinPostingTreeScan *gdi;
			Page		page;

			LockBuffer(stackEntry->buffer, GIN_UNLOCK);
			needUnlock = FALSE;
			gdi = prepareScanPostingTree(index, rootPostingTree, TRUE);

			entry->buffer = scanBeginPostingTree(gdi);
			/*
			 * We keep buffer pinned because we need to prevent deletition
			 * page during scan. See GIN's vacuum implementation. RefCount
			 * is increased to keep buffer pinned after freeGinBtreeStack() call.
			 */
			IncrBufferRefCount(entry->buffer);

			page = BufferGetPage(entry->buffer);
			entry->predictNumberResult = gdi->stack->predictNumber * GinPageGetOpaque(page)->maxoff;

			/*
			 * Keep page content in memory to prevent durable page locking
			 */
			entry->list = (ItemPointerData *) palloc( BLCKSZ );
			entry->nlist = GinPageGetOpaque(page)->maxoff;
			memcpy( entry->list, GinDataPageGetItem(page, FirstOffsetNumber),
						GinPageGetOpaque(page)->maxoff * sizeof(ItemPointerData) );

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

	memset(key->entryRes, TRUE, sizeof(bool) * key->nentries);
	key->isFinished = FALSE;
	key->firstCall = FALSE;

	if (GinFuzzySearchLimit > 0)
	{
		/*
		 * If all of keys more than treshold we will try to reduce result,
		 * we hope (and only hope, for intersection operation of array our
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
	BlockNumber blkno;

	for(;;)
	{
		entry->offset++;

		if (entry->offset <= entry->nlist)
		{
			entry->curItem = entry->list[entry->offset - 1];
			return;
		}

		LockBuffer(entry->buffer, GIN_SHARE);
		page = BufferGetPage(entry->buffer);
		for(;;)
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
				ItemPointerSet(&entry->curItem, InvalidBlockNumber, InvalidOffsetNumber);
				entry->buffer = InvalidBuffer;
				entry->isFinished = TRUE;
				return;
			}

			entry->buffer = ReleaseAndReadBuffer(entry->buffer, index, blkno);
			LockBuffer(entry->buffer, GIN_SHARE);
			page = BufferGetPage(entry->buffer);

			entry->offset = InvalidOffsetNumber;
			if (!ItemPointerIsValid(&entry->curItem) || findItemInPage(page, &entry->curItem, &entry->offset))
			{
				/*
				 * Found position equal to or greater than stored
				 */
				entry->nlist = GinPageGetOpaque(page)->maxoff;
				memcpy( entry->list, GinDataPageGetItem(page, FirstOffsetNumber), 
							GinPageGetOpaque(page)->maxoff * sizeof(ItemPointerData) );

				LockBuffer(entry->buffer, GIN_UNLOCK);

				if ( !ItemPointerIsValid(&entry->curItem) || 
					 compareItemPointers( &entry->curItem, entry->list + entry->offset - 1 ) == 0 )
				{
					/*
					 * First pages are deleted or empty, or we found exact position,
					 * so break inner loop and continue outer one.
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
 * Sets entry->curItem to new found heap item pointer for one
 * entry of one scan key
 */
static bool
entryGetItem(Relation index, GinScanEntry entry)
{
	if (entry->master)
	{
		entry->isFinished = entry->master->isFinished;
		entry->curItem = entry->master->curItem;
	}
	else if (!BufferIsValid(entry->buffer))
	{
		entry->offset++;
		if (entry->offset <= entry->nlist)
			entry->curItem = entry->list[entry->offset - 1];
		else
		{
			ItemPointerSet(&entry->curItem, InvalidBlockNumber, InvalidOffsetNumber);
			entry->isFinished = TRUE;
		}
	}
	else
	{
		do
		{
			entryGetNextItem(index, entry);
		} while (entry->isFinished == FALSE && entry->reduceResult == TRUE && dropItem(entry));
	}

	return entry->isFinished;
}

/*
 * Sets key->curItem to new found heap item pointer for one scan key
 * returns isFinished!
 */
static bool
keyGetItem(Relation index, GinState *ginstate, MemoryContext tempCtx, GinScanKey key)
{
	uint32		i;
	GinScanEntry entry;
	bool		res;
	MemoryContext oldCtx;

	if (key->isFinished)
		return TRUE;

	do
	{
		/*
		 * move forward from previously value and set new curItem, which is
		 * minimal from entries->curItems
		 */
		ItemPointerSetMax(&key->curItem);
		for (i = 0; i < key->nentries; i++)
		{
			entry = key->scanEntry + i;

			if (key->entryRes[i])
			{
				if (entry->isFinished == FALSE && entryGetItem(index, entry) == FALSE)
				{
					if (compareItemPointers(&entry->curItem, &key->curItem) < 0)
						key->curItem = entry->curItem;
				}
				else
					key->entryRes[i] = FALSE;
			}
			else if (entry->isFinished == FALSE)
			{
				if (compareItemPointers(&entry->curItem, &key->curItem) < 0)
					key->curItem = entry->curItem;
			}
		}

		if (ItemPointerIsMax(&key->curItem))
		{
			/* all entries are finished */
			key->isFinished = TRUE;
			return TRUE;
		}

		if (key->nentries == 1)
		{
			/* we can do not call consistentFn !! */
			key->entryRes[0] = TRUE;
			return FALSE;
		}

		/* setting up array for consistentFn */
		for (i = 0; i < key->nentries; i++)
		{
			entry = key->scanEntry + i;

			if (entry->isFinished == FALSE && compareItemPointers(&entry->curItem, &key->curItem) == 0)
				key->entryRes[i] = TRUE;
			else
				key->entryRes[i] = FALSE;
		}

		oldCtx = MemoryContextSwitchTo(tempCtx);
		res = DatumGetBool(FunctionCall3(
										 &ginstate->consistentFn,
										 PointerGetDatum(key->entryRes),
										 UInt16GetDatum(key->strategy),
										 key->query
										 ));
		MemoryContextSwitchTo(oldCtx);
		MemoryContextReset(tempCtx);
	} while (!res);

	return FALSE;
}

/*
 * Get heap item pointer from scan
 * returns true if found
 */
static bool
scanGetItem(IndexScanDesc scan, ItemPointerData *item)
{
	uint32		i;
	GinScanOpaque so = (GinScanOpaque) scan->opaque;

	ItemPointerSetMin(item);
	for (i = 0; i < so->nkeys; i++)
	{
		GinScanKey	key = so->keys + i;

		if (keyGetItem(scan->indexRelation, &so->ginstate, so->tempCtx, key) == FALSE)
		{
			if (compareItemPointers(item, &key->curItem) < 0)
				*item = key->curItem;
		}
		else
			return FALSE;		/* finished one of keys */
	}

	for (i = 1; i <= so->nkeys; i++)
	{
		GinScanKey	key = so->keys + i - 1;

		for (;;)
		{
			int			cmp = compareItemPointers(item, &key->curItem);

			if (cmp == 0)
				break;
			else if (cmp > 0)
			{
				if (keyGetItem(scan->indexRelation, &so->ginstate, so->tempCtx, key) == TRUE)
					return FALSE;		/* finished one of keys */
			}
			else
			{					/* returns to begin */
				*item = key->curItem;
				i = 0;
				break;
			}
		}
	}

	return TRUE;
}

#define GinIsNewKey(s)		( ((GinScanOpaque) scan->opaque)->keys == NULL )

Datum
gingetmulti(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ItemPointer tids = (ItemPointer) PG_GETARG_POINTER(1);
	int32		max_tids = PG_GETARG_INT32(2);
	int32	   *returned_tids = (int32 *) PG_GETARG_POINTER(3);

	if (GinIsNewKey(scan))
		newScanKey(scan);

	startScan(scan);

	*returned_tids = 0;

	do
	{
		if (scanGetItem(scan, tids + *returned_tids))
			(*returned_tids)++;
		else
			break;
	} while (*returned_tids < max_tids);

	PG_RETURN_BOOL(*returned_tids == max_tids);
}

Datum
gingettuple(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ScanDirection dir = (ScanDirection) PG_GETARG_INT32(1);
	bool		res;

	if (dir != ForwardScanDirection)
		elog(ERROR, "Gin doesn't support other scan directions than forward");

	if (GinIsNewKey(scan))
		newScanKey(scan);

	startScan(scan);
	res = scanGetItem(scan, &scan->xs_ctup.t_self);

	PG_RETURN_BOOL(res);
}
