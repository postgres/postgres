/*-------------------------------------------------------------------------
 *
 * blvacuum.c
 *		Bloom VACUUM functions.
 *
 * Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/bloom/blvacuum.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "bloom.h"
#include "commands/vacuum.h"
#include "storage/bufmgr.h"
#include "storage/indexfsm.h"
#include "storage/read_stream.h"


/*
 * Bulk deletion of all index entries pointing to a set of heap tuples.
 * The set of target tuples is specified via a callback routine that tells
 * whether any given heap tuple (identified by ItemPointer) is being deleted.
 *
 * Result: a palloc'd struct containing statistical info for VACUUM displays.
 */
IndexBulkDeleteResult *
blbulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			 IndexBulkDeleteCallback callback, void *callback_state)
{
	Relation	index = info->index;
	BlockNumber blkno,
				npages;
	FreeBlockNumberArray notFullPage;
	int			countPage = 0;
	BloomState	state;
	Buffer		buffer;
	Page		page;
	BloomMetaPageData *metaData;
	GenericXLogState *gxlogState;
	BlockRangeReadStreamPrivate p;
	ReadStream *stream;

	if (stats == NULL)
		stats = palloc0_object(IndexBulkDeleteResult);

	initBloomState(&state, index);

	/*
	 * Iterate over the pages. We don't care about concurrently added pages,
	 * they can't contain tuples to delete.
	 */
	npages = RelationGetNumberOfBlocks(index);

	/* Scan all blocks except the metapage using streaming reads */
	p.current_blocknum = BLOOM_HEAD_BLKNO;
	p.last_exclusive = npages;

	/*
	 * It is safe to use batchmode as block_range_read_stream_cb takes no
	 * locks.
	 */
	stream = read_stream_begin_relation(READ_STREAM_MAINTENANCE |
										READ_STREAM_FULL |
										READ_STREAM_USE_BATCHING,
										info->strategy,
										index,
										MAIN_FORKNUM,
										block_range_read_stream_cb,
										&p,
										0);

	for (blkno = BLOOM_HEAD_BLKNO; blkno < npages; blkno++)
	{
		BloomTuple *itup,
				   *itupPtr,
				   *itupEnd;

		vacuum_delay_point(false);

		buffer = read_stream_next_buffer(stream, NULL);

		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		gxlogState = GenericXLogStart(index);
		page = GenericXLogRegisterBuffer(gxlogState, buffer, 0);

		/* Ignore empty/deleted pages until blvacuumcleanup() */
		if (PageIsNew(page) || BloomPageIsDeleted(page))
		{
			UnlockReleaseBuffer(buffer);
			GenericXLogAbort(gxlogState);
			continue;
		}

		/*
		 * Iterate over the tuples.  itup points to current tuple being
		 * scanned, itupPtr points to where to save next non-deleted tuple.
		 */
		itup = itupPtr = BloomPageGetTuple(&state, page, FirstOffsetNumber);
		itupEnd = BloomPageGetTuple(&state, page,
									OffsetNumberNext(BloomPageGetMaxOffset(page)));
		while (itup < itupEnd)
		{
			/* Do we have to delete this tuple? */
			if (callback(&itup->heapPtr, callback_state))
			{
				/* Yes; adjust count of tuples that will be left on page */
				BloomPageGetOpaque(page)->maxoff--;
				stats->tuples_removed += 1;
			}
			else
			{
				/* No; copy it to itupPtr++, but skip copy if not needed */
				if (itupPtr != itup)
					memmove(itupPtr, itup, state.sizeOfBloomTuple);
				itupPtr = BloomPageGetNextTuple(&state, itupPtr);
			}

			itup = BloomPageGetNextTuple(&state, itup);
		}

		/* Assert that we counted correctly */
		Assert(itupPtr == BloomPageGetTuple(&state, page,
											OffsetNumberNext(BloomPageGetMaxOffset(page))));

		/*
		 * Add page to new notFullPage list if we will not mark page as
		 * deleted and there is free space on it
		 */
		if (BloomPageGetMaxOffset(page) != 0 &&
			BloomPageGetFreeSpace(&state, page) >= state.sizeOfBloomTuple &&
			countPage < BloomMetaBlockN)
			notFullPage[countPage++] = blkno;

		/* Did we delete something? */
		if (itupPtr != itup)
		{
			/* Is it empty page now? */
			if (BloomPageGetMaxOffset(page) == 0)
				BloomPageSetDeleted(page);
			/* Adjust pd_lower */
			((PageHeader) page)->pd_lower = (char *) itupPtr - page;
			/* Finish WAL-logging */
			GenericXLogFinish(gxlogState);
		}
		else
		{
			/* Didn't change anything: abort WAL-logging */
			GenericXLogAbort(gxlogState);
		}
		UnlockReleaseBuffer(buffer);
	}

	Assert(read_stream_next_buffer(stream, NULL) == InvalidBuffer);
	read_stream_end(stream);

	/*
	 * Update the metapage's notFullPage list with whatever we found.  Our
	 * info could already be out of date at this point, but blinsert() will
	 * cope if so.
	 */
	buffer = ReadBuffer(index, BLOOM_METAPAGE_BLKNO);
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	gxlogState = GenericXLogStart(index);
	page = GenericXLogRegisterBuffer(gxlogState, buffer, 0);

	metaData = BloomPageGetMeta(page);
	memcpy(metaData->notFullPage, notFullPage, sizeof(BlockNumber) * countPage);
	metaData->nStart = 0;
	metaData->nEnd = countPage;

	GenericXLogFinish(gxlogState);
	UnlockReleaseBuffer(buffer);

	return stats;
}

/*
 * Post-VACUUM cleanup.
 *
 * Result: a palloc'd struct containing statistical info for VACUUM displays.
 */
IndexBulkDeleteResult *
blvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	Relation	index = info->index;
	BlockNumber npages,
				blkno;
	BlockRangeReadStreamPrivate p;
	ReadStream *stream;

	if (info->analyze_only)
		return stats;

	if (stats == NULL)
		stats = palloc0_object(IndexBulkDeleteResult);

	/*
	 * Iterate over the pages: insert deleted pages into FSM and collect
	 * statistics.
	 */
	npages = RelationGetNumberOfBlocks(index);
	stats->num_pages = npages;
	stats->pages_free = 0;
	stats->num_index_tuples = 0;

	/* Scan all blocks except the metapage using streaming reads */
	p.current_blocknum = BLOOM_HEAD_BLKNO;
	p.last_exclusive = npages;

	/*
	 * It is safe to use batchmode as block_range_read_stream_cb takes no
	 * locks.
	 */
	stream = read_stream_begin_relation(READ_STREAM_MAINTENANCE |
										READ_STREAM_FULL |
										READ_STREAM_USE_BATCHING,
										info->strategy,
										index,
										MAIN_FORKNUM,
										block_range_read_stream_cb,
										&p,
										0);

	for (blkno = BLOOM_HEAD_BLKNO; blkno < npages; blkno++)
	{
		Buffer		buffer;
		Page		page;

		vacuum_delay_point(false);

		buffer = read_stream_next_buffer(stream, NULL);
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buffer);

		if (PageIsNew(page) || BloomPageIsDeleted(page))
		{
			RecordFreeIndexPage(index, blkno);
			stats->pages_free++;
		}
		else
		{
			stats->num_index_tuples += BloomPageGetMaxOffset(page);
		}

		UnlockReleaseBuffer(buffer);
	}

	Assert(read_stream_next_buffer(stream, NULL) == InvalidBuffer);
	read_stream_end(stream);

	IndexFreeSpaceMapVacuum(info->index);

	return stats;
}
