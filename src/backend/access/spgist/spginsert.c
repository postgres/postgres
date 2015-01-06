/*-------------------------------------------------------------------------
 *
 * spginsert.c
 *	  Externally visible index creation/insertion routines
 *
 * All the actual insertion logic is in spgdoinsert.c.
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/spgist/spginsert.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/spgist_private.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/smgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"


typedef struct
{
	SpGistState spgstate;		/* SPGiST's working state */
	MemoryContext tmpCtx;		/* per-tuple temporary context */
} SpGistBuildState;


/* Callback to process one heap tuple during IndexBuildHeapScan */
static void
spgistBuildCallback(Relation index, HeapTuple htup, Datum *values,
					bool *isnull, bool tupleIsAlive, void *state)
{
	SpGistBuildState *buildstate = (SpGistBuildState *) state;
	MemoryContext oldCtx;

	/* Work in temp context, and reset it after each tuple */
	oldCtx = MemoryContextSwitchTo(buildstate->tmpCtx);

	/*
	 * Even though no concurrent insertions can be happening, we still might
	 * get a buffer-locking failure due to bgwriter or checkpointer taking a
	 * lock on some buffer.  So we need to be willing to retry.  We can flush
	 * any temp data when retrying.
	 */
	while (!spgdoinsert(index, &buildstate->spgstate, &htup->t_self,
						*values, *isnull))
	{
		MemoryContextReset(buildstate->tmpCtx);
	}

	MemoryContextSwitchTo(oldCtx);
	MemoryContextReset(buildstate->tmpCtx);
}

/*
 * Build an SP-GiST index.
 */
Datum
spgbuild(PG_FUNCTION_ARGS)
{
	Relation	heap = (Relation) PG_GETARG_POINTER(0);
	Relation	index = (Relation) PG_GETARG_POINTER(1);
	IndexInfo  *indexInfo = (IndexInfo *) PG_GETARG_POINTER(2);
	IndexBuildResult *result;
	double		reltuples;
	SpGistBuildState buildstate;
	Buffer		metabuffer,
				rootbuffer,
				nullbuffer;

	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	/*
	 * Initialize the meta page and root pages
	 */
	metabuffer = SpGistNewBuffer(index);
	rootbuffer = SpGistNewBuffer(index);
	nullbuffer = SpGistNewBuffer(index);

	Assert(BufferGetBlockNumber(metabuffer) == SPGIST_METAPAGE_BLKNO);
	Assert(BufferGetBlockNumber(rootbuffer) == SPGIST_ROOT_BLKNO);
	Assert(BufferGetBlockNumber(nullbuffer) == SPGIST_NULL_BLKNO);

	START_CRIT_SECTION();

	SpGistInitMetapage(BufferGetPage(metabuffer));
	MarkBufferDirty(metabuffer);
	SpGistInitBuffer(rootbuffer, SPGIST_LEAF);
	MarkBufferDirty(rootbuffer);
	SpGistInitBuffer(nullbuffer, SPGIST_LEAF | SPGIST_NULLS);
	MarkBufferDirty(nullbuffer);

	if (RelationNeedsWAL(index))
	{
		XLogRecPtr	recptr;

		XLogBeginInsert();

		/*
		 * Replay will re-initialize the pages, so don't take full pages
		 * images.  No other data to log.
		 */
		XLogRegisterBuffer(0, metabuffer, REGBUF_WILL_INIT);
		XLogRegisterBuffer(1, rootbuffer, REGBUF_WILL_INIT | REGBUF_STANDARD);
		XLogRegisterBuffer(2, nullbuffer, REGBUF_WILL_INIT | REGBUF_STANDARD);

		recptr = XLogInsert(RM_SPGIST_ID, XLOG_SPGIST_CREATE_INDEX);

		PageSetLSN(BufferGetPage(metabuffer), recptr);
		PageSetLSN(BufferGetPage(rootbuffer), recptr);
		PageSetLSN(BufferGetPage(nullbuffer), recptr);
	}

	END_CRIT_SECTION();

	UnlockReleaseBuffer(metabuffer);
	UnlockReleaseBuffer(rootbuffer);
	UnlockReleaseBuffer(nullbuffer);

	/*
	 * Now insert all the heap data into the index
	 */
	initSpGistState(&buildstate.spgstate, index);
	buildstate.spgstate.isBuild = true;

	buildstate.tmpCtx = AllocSetContextCreate(CurrentMemoryContext,
										   "SP-GiST build temporary context",
											  ALLOCSET_DEFAULT_MINSIZE,
											  ALLOCSET_DEFAULT_INITSIZE,
											  ALLOCSET_DEFAULT_MAXSIZE);

	reltuples = IndexBuildHeapScan(heap, index, indexInfo, true,
								   spgistBuildCallback, (void *) &buildstate);

	MemoryContextDelete(buildstate.tmpCtx);

	SpGistUpdateMetaPage(index);

	result = (IndexBuildResult *) palloc0(sizeof(IndexBuildResult));
	result->heap_tuples = result->index_tuples = reltuples;

	PG_RETURN_POINTER(result);
}

/*
 * Build an empty SPGiST index in the initialization fork
 */
Datum
spgbuildempty(PG_FUNCTION_ARGS)
{
	Relation	index = (Relation) PG_GETARG_POINTER(0);
	Page		page;

	/* Construct metapage. */
	page = (Page) palloc(BLCKSZ);
	SpGistInitMetapage(page);

	/* Write the page.  If archiving/streaming, XLOG it. */
	PageSetChecksumInplace(page, SPGIST_METAPAGE_BLKNO);
	smgrwrite(index->rd_smgr, INIT_FORKNUM, SPGIST_METAPAGE_BLKNO,
			  (char *) page, true);
	if (XLogIsNeeded())
		log_newpage(&index->rd_smgr->smgr_rnode.node, INIT_FORKNUM,
					SPGIST_METAPAGE_BLKNO, page, false);

	/* Likewise for the root page. */
	SpGistInitPage(page, SPGIST_LEAF);

	PageSetChecksumInplace(page, SPGIST_ROOT_BLKNO);
	smgrwrite(index->rd_smgr, INIT_FORKNUM, SPGIST_ROOT_BLKNO,
			  (char *) page, true);
	if (XLogIsNeeded())
		log_newpage(&index->rd_smgr->smgr_rnode.node, INIT_FORKNUM,
					SPGIST_ROOT_BLKNO, page, true);

	/* Likewise for the null-tuples root page. */
	SpGistInitPage(page, SPGIST_LEAF | SPGIST_NULLS);

	PageSetChecksumInplace(page, SPGIST_NULL_BLKNO);
	smgrwrite(index->rd_smgr, INIT_FORKNUM, SPGIST_NULL_BLKNO,
			  (char *) page, true);
	if (XLogIsNeeded())
		log_newpage(&index->rd_smgr->smgr_rnode.node, INIT_FORKNUM,
					SPGIST_NULL_BLKNO, page, true);

	/*
	 * An immediate sync is required even if we xlog'd the pages, because the
	 * writes did not go through shared buffers and therefore a concurrent
	 * checkpoint may have moved the redo pointer past our xlog record.
	 */
	smgrimmedsync(index->rd_smgr, INIT_FORKNUM);

	PG_RETURN_VOID();
}

/*
 * Insert one new tuple into an SPGiST index.
 */
Datum
spginsert(PG_FUNCTION_ARGS)
{
	Relation	index = (Relation) PG_GETARG_POINTER(0);
	Datum	   *values = (Datum *) PG_GETARG_POINTER(1);
	bool	   *isnull = (bool *) PG_GETARG_POINTER(2);
	ItemPointer ht_ctid = (ItemPointer) PG_GETARG_POINTER(3);

#ifdef NOT_USED
	Relation	heapRel = (Relation) PG_GETARG_POINTER(4);
	IndexUniqueCheck checkUnique = (IndexUniqueCheck) PG_GETARG_INT32(5);
#endif
	SpGistState spgstate;
	MemoryContext oldCtx;
	MemoryContext insertCtx;

	insertCtx = AllocSetContextCreate(CurrentMemoryContext,
									  "SP-GiST insert temporary context",
									  ALLOCSET_DEFAULT_MINSIZE,
									  ALLOCSET_DEFAULT_INITSIZE,
									  ALLOCSET_DEFAULT_MAXSIZE);
	oldCtx = MemoryContextSwitchTo(insertCtx);

	initSpGistState(&spgstate, index);

	/*
	 * We might have to repeat spgdoinsert() multiple times, if conflicts
	 * occur with concurrent insertions.  If so, reset the insertCtx each time
	 * to avoid cumulative memory consumption.  That means we also have to
	 * redo initSpGistState(), but it's cheap enough not to matter.
	 */
	while (!spgdoinsert(index, &spgstate, ht_ctid, *values, *isnull))
	{
		MemoryContextReset(insertCtx);
		initSpGistState(&spgstate, index);
	}

	SpGistUpdateMetaPage(index);

	MemoryContextSwitchTo(oldCtx);
	MemoryContextDelete(insertCtx);

	/* return false since we've not done any unique check */
	PG_RETURN_BOOL(false);
}
