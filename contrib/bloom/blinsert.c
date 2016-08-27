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
#include "storage/smgr.h"
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
	MemoryContext tmpCtx;		/* temporary memory context reset after each
								 * tuple */
	char		data[BLCKSZ];	/* cached page */
	int64		count;			/* number of tuples in cached page */
} BloomBuildState;

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
	page = GenericXLogRegisterBuffer(state, buffer, GENERIC_XLOG_FULL_IMAGE);
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
											  ALLOCSET_DEFAULT_SIZES);
	initCachedPage(&buildstate);

	/* Do the heap scan */
	reltuples = IndexBuildHeapScan(heap, index, indexInfo, true,
								   bloomBuildCallback, (void *) &buildstate);

	/*
	 * There are could be some items in cached page.  Flush this page if
	 * needed.
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
	Page		metapage;

	/* Construct metapage. */
	metapage = (Page) palloc(BLCKSZ);
	BloomFillMetapage(index, metapage);

	/* Write the page.  If archiving/streaming, XLOG it. */
	PageSetChecksumInplace(metapage, BLOOM_METAPAGE_BLKNO);
	smgrwrite(index->rd_smgr, INIT_FORKNUM, BLOOM_METAPAGE_BLKNO,
			  (char *) metapage, true);
	if (XLogIsNeeded())
		log_newpage(&index->rd_smgr->smgr_rnode.node, INIT_FORKNUM,
					BLOOM_METAPAGE_BLKNO, metapage, false);

	/*
	 * An immediate sync is required even if we xlog'd the page, because the
	 * write did not go through shared_buffers and therefore a concurrent
	 * checkpoint may have moved the redo pointer past our xlog record.
	 */
	smgrimmedsync(index->rd_smgr, INIT_FORKNUM);
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
									  ALLOCSET_DEFAULT_SIZES);

	oldCtx = MemoryContextSwitchTo(insertCtx);

	initBloomState(&blstate, index);
	itup = BloomFormTuple(&blstate, ht_ctid, values, isnull);

	/*
	 * At first, try to insert new tuple to the first page in notFullPage
	 * array.  If successful, we don't need to modify the meta page.
	 */
	metaBuffer = ReadBuffer(index, BLOOM_METAPAGE_BLKNO);
	LockBuffer(metaBuffer, BUFFER_LOCK_SHARE);
	metaData = BloomPageGetMeta(BufferGetPage(metaBuffer));

	if (metaData->nEnd > metaData->nStart)
	{
		Page		page;

		blkno = metaData->notFullPage[metaData->nStart];
		Assert(blkno != InvalidBlockNumber);

		/* Don't hold metabuffer lock while doing insert */
		LockBuffer(metaBuffer, BUFFER_LOCK_UNLOCK);

		buffer = ReadBuffer(index, blkno);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

		state = GenericXLogStart(index);
		page = GenericXLogRegisterBuffer(state, buffer, 0);

		/*
		 * We might have found a page that was recently deleted by VACUUM.  If
		 * so, we can reuse it, but we must reinitialize it.
		 */
		if (PageIsNew(page) || BloomPageIsDeleted(page))
			BloomInitPage(page, 0);

		if (BloomPageAddItem(&blstate, page, itup))
		{
			/* Success!  Apply the change, clean up, and exit */
			GenericXLogFinish(state);
			UnlockReleaseBuffer(buffer);
			ReleaseBuffer(metaBuffer);
			MemoryContextSwitchTo(oldCtx);
			MemoryContextDelete(insertCtx);
			return false;
		}

		/* Didn't fit, must try other pages */
		GenericXLogAbort(state);
		UnlockReleaseBuffer(buffer);
	}
	else
	{
		/* No entries in notFullPage */
		LockBuffer(metaBuffer, BUFFER_LOCK_UNLOCK);
	}

	/*
	 * Try other pages in notFullPage array.  We will have to change nStart in
	 * metapage.  Thus, grab exclusive lock on metapage.
	 */
	LockBuffer(metaBuffer, BUFFER_LOCK_EXCLUSIVE);

	/* nStart might have changed while we didn't have lock */
	nStart = metaData->nStart;

	/* Skip first page if we already tried it above */
	if (nStart < metaData->nEnd &&
		blkno == metaData->notFullPage[nStart])
		nStart++;

	/*
	 * This loop iterates for each page we try from the notFullPage array, and
	 * will also initialize a GenericXLogState for the fallback case of having
	 * to allocate a new page.
	 */
	for (;;)
	{
		state = GenericXLogStart(index);

		/* get modifiable copy of metapage */
		metaPage = GenericXLogRegisterBuffer(state, metaBuffer, 0);
		metaData = BloomPageGetMeta(metaPage);

		if (nStart >= metaData->nEnd)
			break;				/* no more entries in notFullPage array */

		blkno = metaData->notFullPage[nStart];
		Assert(blkno != InvalidBlockNumber);

		buffer = ReadBuffer(index, blkno);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		page = GenericXLogRegisterBuffer(state, buffer, 0);

		/* Basically same logic as above */
		if (PageIsNew(page) || BloomPageIsDeleted(page))
			BloomInitPage(page, 0);

		if (BloomPageAddItem(&blstate, page, itup))
		{
			/* Success!  Apply the changes, clean up, and exit */
			metaData->nStart = nStart;
			GenericXLogFinish(state);
			UnlockReleaseBuffer(buffer);
			UnlockReleaseBuffer(metaBuffer);
			MemoryContextSwitchTo(oldCtx);
			MemoryContextDelete(insertCtx);
			return false;
		}

		/* Didn't fit, must try other pages */
		GenericXLogAbort(state);
		UnlockReleaseBuffer(buffer);
		nStart++;
	}

	/*
	 * Didn't find place to insert in notFullPage array.  Allocate new page.
	 * (XXX is it good to do this while holding ex-lock on the metapage??)
	 */
	buffer = BloomNewBuffer(index);

	page = GenericXLogRegisterBuffer(state, buffer, GENERIC_XLOG_FULL_IMAGE);
	BloomInitPage(page, 0);

	if (!BloomPageAddItem(&blstate, page, itup))
	{
		/* We shouldn't be here since we're inserting to an empty page */
		elog(ERROR, "could not add new bloom tuple to empty page");
	}

	/* Reset notFullPage array to contain just this new page */
	metaData->nStart = 0;
	metaData->nEnd = 1;
	metaData->notFullPage[0] = BufferGetBlockNumber(buffer);

	/* Apply the changes, clean up, and exit */
	GenericXLogFinish(state);

	UnlockReleaseBuffer(buffer);
	UnlockReleaseBuffer(metaBuffer);

	MemoryContextSwitchTo(oldCtx);
	MemoryContextDelete(insertCtx);

	return false;
}
