/*-------------------------------------------------------------------------
 *
 * blinsert.c
 *		Bloom index build and insert functions.
 *
 * Copyright (c) 2016, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/bloom/blinsert.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/generic_xlog.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/indexfsm.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "bloom.h"

PG_MODULE_MAGIC;

/*
 * State of bloom index build.  We accumulate one page data here before
 * flushing it to buffer manager.
 */
typedef struct
{
	BloomState	blstate;		/* bloom index state */
	MemoryContext tmpCtx;		/* temporary memory context reset after
								 * each tuple */
	char		data[BLCKSZ];	/* cached page */
	int64		count;			/* number of tuples in cached page */
}	BloomBuildState;

/*
 * Flush page cached in BloomBuildState.
 */
static void
flushCachedPage(Relation index, BloomBuildState *buildstate)
{
	Page		page;
	Buffer		buffer = BloomNewBuffer(index);
	GenericXLogState *state;

	state = GenericXLogStart(index);
	page = GenericXLogRegister(state, buffer, true);
	memcpy(page, buildstate->data, BLCKSZ);
	GenericXLogFinish(state);
	UnlockReleaseBuffer(buffer);
}

/*
 * (Re)initialize cached page in BloomBuildState.
 */
static void
initCachedPage(BloomBuildState *buildstate)
{
	memset(buildstate->data, 0, BLCKSZ);
	BloomInitPage(buildstate->data, 0);
	buildstate->count = 0;
}

/*
 * Per-tuple callback from IndexBuildHeapScan.
 */
static void
bloomBuildCallback(Relation index, HeapTuple htup, Datum *values,
				   bool *isnull, bool tupleIsAlive, void *state)
{
	BloomBuildState *buildstate = (BloomBuildState *) state;
	MemoryContext oldCtx;
	BloomTuple *itup;

	oldCtx = MemoryContextSwitchTo(buildstate->tmpCtx);

	itup = BloomFormTuple(&buildstate->blstate, &htup->t_self, values, isnull);

	/* Try to add next item to cached page */
	if (BloomPageAddItem(&buildstate->blstate, buildstate->data, itup))
	{
		/* Next item was added successfully */
		buildstate->count++;
	}
	else
	{
		/* Cached page is full, flush it out and make a new one */
		flushCachedPage(index, buildstate);

		CHECK_FOR_INTERRUPTS();

		initCachedPage(buildstate);

		if (!BloomPageAddItem(&buildstate->blstate, buildstate->data, itup))
		{
			/* We shouldn't be here since we're inserting to the empty page */
			elog(ERROR, "could not add new bloom tuple to empty page");
		}
	}

	MemoryContextSwitchTo(oldCtx);
	MemoryContextReset(buildstate->tmpCtx);
}

/*
 * Build a new bloom index.
 */
IndexBuildResult *
blbuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
	IndexBuildResult *result;
	double		reltuples;
	BloomBuildState buildstate;

	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	/* Initialize the meta page */
	BloomInitMetapage(index);

	/* Initialize the bloom build state */
	memset(&buildstate, 0, sizeof(buildstate));
	initBloomState(&buildstate.blstate, index);
	buildstate.tmpCtx = AllocSetContextCreate(CurrentMemoryContext,
											  "Bloom build temporary context",
											  ALLOCSET_DEFAULT_MINSIZE,
											  ALLOCSET_DEFAULT_INITSIZE,
											  ALLOCSET_DEFAULT_MAXSIZE);
	initCachedPage(&buildstate);

	/* Do the heap scan */
	reltuples = IndexBuildHeapScan(heap, index, indexInfo, true,
								   bloomBuildCallback, (void *) &buildstate);

	/*
	 * There are could be some items in cached page.  Flush this page
	 * if needed.
	 */
	if (buildstate.count > 0)
		flushCachedPage(index, &buildstate);

	MemoryContextDelete(buildstate.tmpCtx);

	result = (IndexBuildResult *) palloc(sizeof(IndexBuildResult));
	result->heap_tuples = result->index_tuples = reltuples;

	return result;
}

/*
 * Build an empty bloom index in the initialization fork.
 */
void
blbuildempty(Relation index)
{
	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	/* Initialize the meta page */
	BloomInitMetapage(index);
}

/*
 * Insert new tuple to the bloom index.
 */
bool
blinsert(Relation index, Datum *values, bool *isnull,
		 ItemPointer ht_ctid, Relation heapRel, IndexUniqueCheck checkUnique)
{
	BloomState	blstate;
	BloomTuple *itup;
	MemoryContext oldCtx;
	MemoryContext insertCtx;
	BloomMetaPageData *metaData;
	Buffer		buffer,
				metaBuffer;
	Page		page,
				metaPage;
	BlockNumber blkno = InvalidBlockNumber;
	OffsetNumber nStart;
	GenericXLogState *state;

	insertCtx = AllocSetContextCreate(CurrentMemoryContext,
									  "Bloom insert temporary context",
									  ALLOCSET_DEFAULT_MINSIZE,
									  ALLOCSET_DEFAULT_INITSIZE,
									  ALLOCSET_DEFAULT_MAXSIZE);

	oldCtx = MemoryContextSwitchTo(insertCtx);

	initBloomState(&blstate, index);
	itup = BloomFormTuple(&blstate, ht_ctid, values, isnull);

	/*
	 * At first, try to insert new tuple to the first page in notFullPage
	 * array.  If success we don't need to modify the meta page.
	 */
	metaBuffer = ReadBuffer(index, BLOOM_METAPAGE_BLKNO);
	LockBuffer(metaBuffer, BUFFER_LOCK_SHARE);
	metaData = BloomPageGetMeta(BufferGetPage(metaBuffer, NULL, NULL,
											  BGP_NO_SNAPSHOT_TEST));

	if (metaData->nEnd > metaData->nStart)
	{
		Page		page;

		blkno = metaData->notFullPage[metaData->nStart];

		Assert(blkno != InvalidBlockNumber);
		LockBuffer(metaBuffer, BUFFER_LOCK_UNLOCK);

		buffer = ReadBuffer(index, blkno);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		state = GenericXLogStart(index);
		page = GenericXLogRegister(state, buffer, false);

		if (BloomPageAddItem(&blstate, page, itup))
		{
			GenericXLogFinish(state);
			UnlockReleaseBuffer(buffer);
			ReleaseBuffer(metaBuffer);
			MemoryContextSwitchTo(oldCtx);
			MemoryContextDelete(insertCtx);
			return false;
		}
		else
		{
			GenericXLogAbort(state);
			UnlockReleaseBuffer(buffer);
		}
	}
	else
	{
		/* First page in notFullPage isn't suitable */
		LockBuffer(metaBuffer, BUFFER_LOCK_UNLOCK);
	}

	/*
	 * Try other pages in notFullPage array.  We will have to change nStart in
	 * metapage.  Thus, grab exclusive lock on metapage.
	 */
	LockBuffer(metaBuffer, BUFFER_LOCK_EXCLUSIVE);

	state = GenericXLogStart(index);
	metaPage = GenericXLogRegister(state, metaBuffer, false);
	metaData = BloomPageGetMeta(metaPage);

	/*
	 * Iterate over notFullPage array.  Skip page we already tried first.
	 */
	nStart = metaData->nStart;
	if (metaData->nEnd > nStart &&
		blkno == metaData->notFullPage[nStart])
		nStart++;

	while (metaData->nEnd > nStart)
	{
		blkno = metaData->notFullPage[nStart];
		Assert(blkno != InvalidBlockNumber);

		buffer = ReadBuffer(index, blkno);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		page = GenericXLogRegister(state, buffer, false);

		if (BloomPageAddItem(&blstate, page, itup))
		{
			metaData->nStart = nStart;
			GenericXLogFinish(state);
			UnlockReleaseBuffer(buffer);
			UnlockReleaseBuffer(metaBuffer);
			MemoryContextSwitchTo(oldCtx);
			MemoryContextDelete(insertCtx);
			return false;
		}
		else
		{
			GenericXLogUnregister(state, buffer);
			UnlockReleaseBuffer(buffer);
		}
		nStart++;
	}

	GenericXLogAbort(state);

	/*
	 * Didn't find place to insert in notFullPage array.  Allocate new page.
	 */
	buffer = BloomNewBuffer(index);

	state = GenericXLogStart(index);
	metaPage = GenericXLogRegister(state, metaBuffer, false);
	metaData = BloomPageGetMeta(metaPage);
	page = GenericXLogRegister(state, buffer, true);
	BloomInitPage(page, 0);

	if (!BloomPageAddItem(&blstate, page, itup))
	{
		/* We shouldn't be here since we're inserting to the empty page */
		elog(ERROR, "could not add new bloom tuple to empty page");
	}

	metaData->nStart = 0;
	metaData->nEnd = 1;
	metaData->notFullPage[0] = BufferGetBlockNumber(buffer);

	GenericXLogFinish(state);

	UnlockReleaseBuffer(buffer);
	UnlockReleaseBuffer(metaBuffer);

	return false;
}
