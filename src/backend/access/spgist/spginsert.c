/*-------------------------------------------------------------------------
 *
 * spginsert.c
 *	  Externally visible index creation/insertion routines
 *
 * All the actual insertion logic is in spgdoinsert.c.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
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
#include "access/tableam.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "storage/bufmgr.h"
#include "storage/bulk_write.h"
#include "utils/memutils.h"
#include "utils/rel.h"


typedef struct
{
	SpGistState spgstate;		/* SPGiST's working state */
	int64		indtuples;		/* total number of tuples indexed */
	MemoryContext tmpCtx;		/* per-tuple temporary context */
} SpGistBuildState;


/* Callback to process one heap tuple during table_index_build_scan */
static void
spgistBuildCallback(Relation index, ItemPointer tid, Datum *values,
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
	while (!spgdoinsert(index, &buildstate->spgstate, tid,
						values, isnull))
	{
		MemoryContextReset(buildstate->tmpCtx);
	}

	/* Update total tuple count */
	buildstate->indtuples += 1;

	MemoryContextSwitchTo(oldCtx);
	MemoryContextReset(buildstate->tmpCtx);
}

/*
 * Build an SP-GiST index.
 */
IndexBuildResult *
spgbuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
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


	END_CRIT_SECTION();

	UnlockReleaseBuffer(metabuffer);
	UnlockReleaseBuffer(rootbuffer);
	UnlockReleaseBuffer(nullbuffer);

	/*
	 * Now insert all the heap data into the index
	 */
	initSpGistState(&buildstate.spgstate, index);
	buildstate.spgstate.isBuild = true;
	buildstate.indtuples = 0;

	buildstate.tmpCtx = AllocSetContextCreate(CurrentMemoryContext,
											  "SP-GiST build temporary context",
											  ALLOCSET_DEFAULT_SIZES);

	reltuples = table_index_build_scan(heap, index, indexInfo, true, true,
									   spgistBuildCallback, &buildstate,
									   NULL);

	MemoryContextDelete(buildstate.tmpCtx);

	SpGistUpdateMetaPage(index);

	/*
	 * We didn't write WAL records as we built the index, so if WAL-logging is
	 * required, write all pages to the WAL now.
	 */
	if (RelationNeedsWAL(index))
	{
		log_newpage_range(index, MAIN_FORKNUM,
						  0, RelationGetNumberOfBlocks(index),
						  true);
	}

	result = (IndexBuildResult *) palloc0(sizeof(IndexBuildResult));
	result->heap_tuples = reltuples;
	result->index_tuples = buildstate.indtuples;

	return result;
}

/*
 * Build an empty SPGiST index in the initialization fork
 */
void
spgbuildempty(Relation index)
{
	BulkWriteState *bulkstate;
	BulkWriteBuffer buf;

	bulkstate = smgr_bulk_start_rel(index, INIT_FORKNUM);

	/* Construct metapage. */
	buf = smgr_bulk_get_buf(bulkstate);
	SpGistInitMetapage((Page) buf);
	smgr_bulk_write(bulkstate, SPGIST_METAPAGE_BLKNO, buf, true);

	/* Likewise for the root page. */
	buf = smgr_bulk_get_buf(bulkstate);
	SpGistInitPage((Page) buf, SPGIST_LEAF);
	smgr_bulk_write(bulkstate, SPGIST_ROOT_BLKNO, buf, true);

	/* Likewise for the null-tuples root page. */
	buf = smgr_bulk_get_buf(bulkstate);
	SpGistInitPage((Page) buf, SPGIST_LEAF | SPGIST_NULLS);
	smgr_bulk_write(bulkstate, SPGIST_NULL_BLKNO, buf, true);

	smgr_bulk_finish(bulkstate);
}

/*
 * Insert one new tuple into an SPGiST index.
 */
bool
spginsert(Relation index, Datum *values, bool *isnull,
		  ItemPointer ht_ctid, Relation heapRel,
		  IndexUniqueCheck checkUnique,
		  bool indexUnchanged,
		  IndexInfo *indexInfo)
{
	SpGistState spgstate;
	MemoryContext oldCtx;
	MemoryContext insertCtx;

	insertCtx = AllocSetContextCreate(CurrentMemoryContext,
									  "SP-GiST insert temporary context",
									  ALLOCSET_DEFAULT_SIZES);
	oldCtx = MemoryContextSwitchTo(insertCtx);

	initSpGistState(&spgstate, index);

	/*
	 * We might have to repeat spgdoinsert() multiple times, if conflicts
	 * occur with concurrent insertions.  If so, reset the insertCtx each time
	 * to avoid cumulative memory consumption.  That means we also have to
	 * redo initSpGistState(), but it's cheap enough not to matter.
	 */
	while (!spgdoinsert(index, &spgstate, ht_ctid, values, isnull))
	{
		MemoryContextReset(insertCtx);
		initSpGistState(&spgstate, index);
	}

	SpGistUpdateMetaPage(index);

	MemoryContextSwitchTo(oldCtx);
	MemoryContextDelete(insertCtx);

	/* return false since we've not done any unique check */
	return false;
}
