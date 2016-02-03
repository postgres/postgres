/*-------------------------------------------------------------------------
 *
 * gininsert.c
 *	  insert routines for the postgres inverted index access method.
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gin/gininsert.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin_private.h"
#include "access/xloginsert.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/smgr.h"
#include "storage/indexfsm.h"
#include "utils/memutils.h"
#include "utils/rel.h"


typedef struct
{
	GinState	ginstate;
	double		indtuples;
	GinStatsData buildStats;
	MemoryContext tmpCtx;
	MemoryContext funcCtx;
	BuildAccumulator accum;
} GinBuildState;


/*
 * Adds array of item pointers to tuple's posting list, or
 * creates posting tree and tuple pointing to tree in case
 * of not enough space.  Max size of tuple is defined in
 * GinFormTuple().  Returns a new, modified index tuple.
 * items[] must be in sorted order with no duplicates.
 */
static IndexTuple
addItemPointersToLeafTuple(GinState *ginstate,
						   IndexTuple old,
						   ItemPointerData *items, uint32 nitem,
						   GinStatsData *buildStats)
{
	OffsetNumber attnum;
	Datum		key;
	GinNullCategory category;
	IndexTuple	res;
	ItemPointerData *newItems,
			   *oldItems;
	int			oldNPosting,
				newNPosting;
	GinPostingList *compressedList;

	Assert(!GinIsPostingTree(old));

	attnum = gintuple_get_attrnum(ginstate, old);
	key = gintuple_get_key(ginstate, old, &category);

	/* merge the old and new posting lists */
	oldItems = ginReadTuple(ginstate, attnum, old, &oldNPosting);

	newItems = ginMergeItemPointers(items, nitem,
									oldItems, oldNPosting,
									&newNPosting);

	/* Compress the posting list, and try to a build tuple with room for it */
	res = NULL;
	compressedList = ginCompressPostingList(newItems, newNPosting, GinMaxItemSize,
											NULL);
	pfree(newItems);
	if (compressedList)
	{
		res = GinFormTuple(ginstate, attnum, key, category,
						   (char *) compressedList,
						   SizeOfGinPostingList(compressedList),
						   newNPosting,
						   false);
		pfree(compressedList);
	}
	if (!res)
	{
		/* posting list would be too big, convert to posting tree */
		BlockNumber postingRoot;

		/*
		 * Initialize posting tree with the old tuple's posting list.  It's
		 * surely small enough to fit on one posting-tree page, and should
		 * already be in order with no duplicates.
		 */
		postingRoot = createPostingTree(ginstate->index,
										oldItems,
										oldNPosting,
										buildStats);

		/* Now insert the TIDs-to-be-added into the posting tree */
		ginInsertItemPointers(ginstate->index, postingRoot,
							  items, nitem,
							  buildStats);

		/* And build a new posting-tree-only result tuple */
		res = GinFormTuple(ginstate, attnum, key, category, NULL, 0, 0, true);
		GinSetPostingTree(res, postingRoot);
	}
	pfree(oldItems);

	return res;
}

/*
 * Build a fresh leaf tuple, either posting-list or posting-tree format
 * depending on whether the given items list will fit.
 * items[] must be in sorted order with no duplicates.
 *
 * This is basically the same logic as in addItemPointersToLeafTuple,
 * but working from slightly different input.
 */
static IndexTuple
buildFreshLeafTuple(GinState *ginstate,
					OffsetNumber attnum, Datum key, GinNullCategory category,
					ItemPointerData *items, uint32 nitem,
					GinStatsData *buildStats)
{
	IndexTuple	res = NULL;
	GinPostingList *compressedList;

	/* try to build a posting list tuple with all the items */
	compressedList = ginCompressPostingList(items, nitem, GinMaxItemSize, NULL);
	if (compressedList)
	{
		res = GinFormTuple(ginstate, attnum, key, category,
						   (char *) compressedList,
						   SizeOfGinPostingList(compressedList),
						   nitem, false);
		pfree(compressedList);
	}
	if (!res)
	{
		/* posting list would be too big, build posting tree */
		BlockNumber postingRoot;

		/*
		 * Build posting-tree-only result tuple.  We do this first so as to
		 * fail quickly if the key is too big.
		 */
		res = GinFormTuple(ginstate, attnum, key, category, NULL, 0, 0, true);

		/*
		 * Initialize a new posting tree with the TIDs.
		 */
		postingRoot = createPostingTree(ginstate->index, items, nitem,
										buildStats);

		/* And save the root link in the result tuple */
		GinSetPostingTree(res, postingRoot);
	}

	return res;
}

/*
 * Insert one or more heap TIDs associated with the given key value.
 * This will either add a single key entry, or enlarge a pre-existing entry.
 *
 * During an index build, buildStats is non-null and the counters
 * it contains should be incremented as needed.
 */
void
ginEntryInsert(GinState *ginstate,
			   OffsetNumber attnum, Datum key, GinNullCategory category,
			   ItemPointerData *items, uint32 nitem,
			   GinStatsData *buildStats)
{
	GinBtreeData btree;
	GinBtreeEntryInsertData insertdata;
	GinBtreeStack *stack;
	IndexTuple	itup;
	Page		page;

	insertdata.isDelete = FALSE;

	/* During index build, count the to-be-inserted entry */
	if (buildStats)
		buildStats->nEntries++;

	ginPrepareEntryScan(&btree, attnum, key, category, ginstate);

	stack = ginFindLeafPage(&btree, false);
	page = BufferGetPage(stack->buffer);

	if (btree.findItem(&btree, stack))
	{
		/* found pre-existing entry */
		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, stack->off));

		if (GinIsPostingTree(itup))
		{
			/* add entries to existing posting tree */
			BlockNumber rootPostingTree = GinGetPostingTree(itup);

			/* release all stack */
			LockBuffer(stack->buffer, GIN_UNLOCK);
			freeGinBtreeStack(stack);

			/* insert into posting tree */
			ginInsertItemPointers(ginstate->index, rootPostingTree,
								  items, nitem,
								  buildStats);
			return;
		}

		/* modify an existing leaf entry */
		itup = addItemPointersToLeafTuple(ginstate, itup,
										  items, nitem, buildStats);

		insertdata.isDelete = TRUE;
	}
	else
	{
		/* no match, so construct a new leaf entry */
		itup = buildFreshLeafTuple(ginstate, attnum, key, category,
								   items, nitem, buildStats);
	}

	/* Insert the new or modified leaf tuple */
	insertdata.entry = itup;
	ginInsertValue(&btree, stack, &insertdata, buildStats);
	pfree(itup);
}

/*
 * Extract index entries for a single indexable item, and add them to the
 * BuildAccumulator's state.
 *
 * This function is used only during initial index creation.
 */
static void
ginHeapTupleBulkInsert(GinBuildState *buildstate, OffsetNumber attnum,
					   Datum value, bool isNull,
					   ItemPointer heapptr)
{
	Datum	   *entries;
	GinNullCategory *categories;
	int32		nentries;
	MemoryContext oldCtx;

	oldCtx = MemoryContextSwitchTo(buildstate->funcCtx);
	entries = ginExtractEntries(buildstate->accum.ginstate, attnum,
								value, isNull,
								&nentries, &categories);
	MemoryContextSwitchTo(oldCtx);

	ginInsertBAEntries(&buildstate->accum, heapptr, attnum,
					   entries, categories, nentries);

	buildstate->indtuples += nentries;

	MemoryContextReset(buildstate->funcCtx);
}

static void
ginBuildCallback(Relation index, HeapTuple htup, Datum *values,
				 bool *isnull, bool tupleIsAlive, void *state)
{
	GinBuildState *buildstate = (GinBuildState *) state;
	MemoryContext oldCtx;
	int			i;

	oldCtx = MemoryContextSwitchTo(buildstate->tmpCtx);

	for (i = 0; i < buildstate->ginstate.origTupdesc->natts; i++)
		ginHeapTupleBulkInsert(buildstate, (OffsetNumber) (i + 1),
							   values[i], isnull[i],
							   &htup->t_self);

	/* If we've maxed out our available memory, dump everything to the index */
	if (buildstate->accum.allocatedMemory >= (Size)maintenance_work_mem * 1024L)
	{
		ItemPointerData *list;
		Datum		key;
		GinNullCategory category;
		uint32		nlist;
		OffsetNumber attnum;

		ginBeginBAScan(&buildstate->accum);
		while ((list = ginGetBAEntry(&buildstate->accum,
								  &attnum, &key, &category, &nlist)) != NULL)
		{
			/* there could be many entries, so be willing to abort here */
			CHECK_FOR_INTERRUPTS();
			ginEntryInsert(&buildstate->ginstate, attnum, key, category,
						   list, nlist, &buildstate->buildStats);
		}

		MemoryContextReset(buildstate->tmpCtx);
		ginInitBA(&buildstate->accum);
	}

	MemoryContextSwitchTo(oldCtx);
}

IndexBuildResult *
ginbuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
	IndexBuildResult *result;
	double		reltuples;
	GinBuildState buildstate;
	Buffer		RootBuffer,
				MetaBuffer;
	ItemPointerData *list;
	Datum		key;
	GinNullCategory category;
	uint32		nlist;
	MemoryContext oldCtx;
	OffsetNumber attnum;

	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	initGinState(&buildstate.ginstate, index);
	buildstate.indtuples = 0;
	memset(&buildstate.buildStats, 0, sizeof(GinStatsData));

	/* initialize the meta page */
	MetaBuffer = GinNewBuffer(index);

	/* initialize the root page */
	RootBuffer = GinNewBuffer(index);

	START_CRIT_SECTION();
	GinInitMetabuffer(MetaBuffer);
	MarkBufferDirty(MetaBuffer);
	GinInitBuffer(RootBuffer, GIN_LEAF);
	MarkBufferDirty(RootBuffer);

	if (RelationNeedsWAL(index))
	{
		XLogRecPtr	recptr;
		Page		page;

		XLogBeginInsert();
		XLogRegisterBuffer(0, MetaBuffer, REGBUF_WILL_INIT);
		XLogRegisterBuffer(1, RootBuffer, REGBUF_WILL_INIT);

		recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_CREATE_INDEX);

		page = BufferGetPage(RootBuffer);
		PageSetLSN(page, recptr);

		page = BufferGetPage(MetaBuffer);
		PageSetLSN(page, recptr);
	}

	UnlockReleaseBuffer(MetaBuffer);
	UnlockReleaseBuffer(RootBuffer);
	END_CRIT_SECTION();

	/* count the root as first entry page */
	buildstate.buildStats.nEntryPages++;

	/*
	 * create a temporary memory context that is used to hold data not yet
	 * dumped out to the index
	 */
	buildstate.tmpCtx = AllocSetContextCreate(CurrentMemoryContext,
											  "Gin build temporary context",
											  ALLOCSET_DEFAULT_MINSIZE,
											  ALLOCSET_DEFAULT_INITSIZE,
											  ALLOCSET_DEFAULT_MAXSIZE);

	/*
	 * create a temporary memory context that is used for calling
	 * ginExtractEntries(), and can be reset after each tuple
	 */
	buildstate.funcCtx = AllocSetContextCreate(CurrentMemoryContext,
					 "Gin build temporary context for user-defined function",
											   ALLOCSET_DEFAULT_MINSIZE,
											   ALLOCSET_DEFAULT_INITSIZE,
											   ALLOCSET_DEFAULT_MAXSIZE);

	buildstate.accum.ginstate = &buildstate.ginstate;
	ginInitBA(&buildstate.accum);

	/*
	 * Do the heap scan.  We disallow sync scan here because dataPlaceToPage
	 * prefers to receive tuples in TID order.
	 */
	reltuples = IndexBuildHeapScan(heap, index, indexInfo, false,
								   ginBuildCallback, (void *) &buildstate);

	/* dump remaining entries to the index */
	oldCtx = MemoryContextSwitchTo(buildstate.tmpCtx);
	ginBeginBAScan(&buildstate.accum);
	while ((list = ginGetBAEntry(&buildstate.accum,
								 &attnum, &key, &category, &nlist)) != NULL)
	{
		/* there could be many entries, so be willing to abort here */
		CHECK_FOR_INTERRUPTS();
		ginEntryInsert(&buildstate.ginstate, attnum, key, category,
					   list, nlist, &buildstate.buildStats);
	}
	MemoryContextSwitchTo(oldCtx);

	MemoryContextDelete(buildstate.funcCtx);
	MemoryContextDelete(buildstate.tmpCtx);

	/*
	 * Update metapage stats
	 */
	buildstate.buildStats.nTotalPages = RelationGetNumberOfBlocks(index);
	ginUpdateStats(index, &buildstate.buildStats);

	/*
	 * Return statistics
	 */
	result = (IndexBuildResult *) palloc(sizeof(IndexBuildResult));

	result->heap_tuples = reltuples;
	result->index_tuples = buildstate.indtuples;

	return result;
}

/*
 *	ginbuildempty() -- build an empty gin index in the initialization fork
 */
void
ginbuildempty(Relation index)
{
	Buffer		RootBuffer,
				MetaBuffer;

	/* An empty GIN index has two pages. */
	MetaBuffer =
		ReadBufferExtended(index, INIT_FORKNUM, P_NEW, RBM_NORMAL, NULL);
	LockBuffer(MetaBuffer, BUFFER_LOCK_EXCLUSIVE);
	RootBuffer =
		ReadBufferExtended(index, INIT_FORKNUM, P_NEW, RBM_NORMAL, NULL);
	LockBuffer(RootBuffer, BUFFER_LOCK_EXCLUSIVE);

	/* Initialize and xlog metabuffer and root buffer. */
	START_CRIT_SECTION();
	GinInitMetabuffer(MetaBuffer);
	MarkBufferDirty(MetaBuffer);
	log_newpage_buffer(MetaBuffer, false);
	GinInitBuffer(RootBuffer, GIN_LEAF);
	MarkBufferDirty(RootBuffer);
	log_newpage_buffer(RootBuffer, false);
	END_CRIT_SECTION();

	/* Unlock and release the buffers. */
	UnlockReleaseBuffer(MetaBuffer);
	UnlockReleaseBuffer(RootBuffer);
}

/*
 * Insert index entries for a single indexable item during "normal"
 * (non-fast-update) insertion
 */
static void
ginHeapTupleInsert(GinState *ginstate, OffsetNumber attnum,
				   Datum value, bool isNull,
				   ItemPointer item)
{
	Datum	   *entries;
	GinNullCategory *categories;
	int32		i,
				nentries;

	entries = ginExtractEntries(ginstate, attnum, value, isNull,
								&nentries, &categories);

	for (i = 0; i < nentries; i++)
		ginEntryInsert(ginstate, attnum, entries[i], categories[i],
					   item, 1, NULL);
}

bool
gininsert(Relation index, Datum *values, bool *isnull,
		  ItemPointer ht_ctid, Relation heapRel,
		  IndexUniqueCheck checkUnique)
{
	GinState	ginstate;
	MemoryContext oldCtx;
	MemoryContext insertCtx;
	int			i;

	insertCtx = AllocSetContextCreate(CurrentMemoryContext,
									  "Gin insert temporary context",
									  ALLOCSET_DEFAULT_MINSIZE,
									  ALLOCSET_DEFAULT_INITSIZE,
									  ALLOCSET_DEFAULT_MAXSIZE);

	oldCtx = MemoryContextSwitchTo(insertCtx);

	initGinState(&ginstate, index);

	if (GinGetUseFastUpdate(index))
	{
		GinTupleCollector collector;

		memset(&collector, 0, sizeof(GinTupleCollector));

		for (i = 0; i < ginstate.origTupdesc->natts; i++)
			ginHeapTupleFastCollect(&ginstate, &collector,
									(OffsetNumber) (i + 1),
									values[i], isnull[i],
									ht_ctid);

		ginHeapTupleFastInsert(&ginstate, &collector);
	}
	else
	{
		for (i = 0; i < ginstate.origTupdesc->natts; i++)
			ginHeapTupleInsert(&ginstate, (OffsetNumber) (i + 1),
							   values[i], isnull[i],
							   ht_ctid);
	}

	MemoryContextSwitchTo(oldCtx);
	MemoryContextDelete(insertCtx);

	return false;
}
