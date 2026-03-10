/*-------------------------------------------------------------------------
 *
 * blscan.c
 *		Bloom index scan functions.
 *
 * Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/bloom/blscan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relscan.h"
#include "bloom.h"
#include "executor/instrument_node.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/read_stream.h"

/*
 * Begin scan of bloom index.
 */
IndexScanDesc
blbeginscan(Relation r, int nkeys, int norderbys)
{
	IndexScanDesc scan;
	BloomScanOpaque so;

	scan = RelationGetIndexScan(r, nkeys, norderbys);

	so = (BloomScanOpaque) palloc_object(BloomScanOpaqueData);
	initBloomState(&so->state, scan->indexRelation);
	so->sign = NULL;

	scan->opaque = so;

	return scan;
}

/*
 * Rescan a bloom index.
 */
void
blrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
		 ScanKey orderbys, int norderbys)
{
	BloomScanOpaque so = (BloomScanOpaque) scan->opaque;

	if (so->sign)
		pfree(so->sign);
	so->sign = NULL;

	if (scankey && scan->numberOfKeys > 0)
		memcpy(scan->keyData, scankey, scan->numberOfKeys * sizeof(ScanKeyData));
}

/*
 * End scan of bloom index.
 */
void
blendscan(IndexScanDesc scan)
{
	BloomScanOpaque so = (BloomScanOpaque) scan->opaque;

	if (so->sign)
		pfree(so->sign);
	so->sign = NULL;
}

/*
 * Insert all matching tuples into a bitmap.
 */
int64
blgetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	int64		ntids = 0;
	BlockNumber blkno,
				npages;
	int			i;
	BufferAccessStrategy bas;
	BloomScanOpaque so = (BloomScanOpaque) scan->opaque;
	BlockRangeReadStreamPrivate p;
	ReadStream *stream;

	if (so->sign == NULL)
	{
		/* New search: have to calculate search signature */
		ScanKey		skey = scan->keyData;

		so->sign = palloc0_array(BloomSignatureWord, so->state.opts.bloomLength);

		for (i = 0; i < scan->numberOfKeys; i++)
		{
			/*
			 * Assume bloom-indexable operators to be strict, so nothing could
			 * be found for NULL key.
			 */
			if (skey->sk_flags & SK_ISNULL)
			{
				pfree(so->sign);
				so->sign = NULL;
				return 0;
			}

			/* Add next value to the signature */
			signValue(&so->state, so->sign, skey->sk_argument,
					  skey->sk_attno - 1);

			skey++;
		}
	}

	/*
	 * We're going to read the whole index. This is why we use appropriate
	 * buffer access strategy.
	 */
	bas = GetAccessStrategy(BAS_BULKREAD);
	npages = RelationGetNumberOfBlocks(scan->indexRelation);
	pgstat_count_index_scan(scan->indexRelation);
	if (scan->instrument)
		scan->instrument->nsearches++;

	/* Scan all blocks except the metapage using streaming reads */
	p.current_blocknum = BLOOM_HEAD_BLKNO;
	p.last_exclusive = npages;

	/*
	 * It is safe to use batchmode as block_range_read_stream_cb takes no
	 * locks.
	 */
	stream = read_stream_begin_relation(READ_STREAM_FULL |
										READ_STREAM_USE_BATCHING,
										bas,
										scan->indexRelation,
										MAIN_FORKNUM,
										block_range_read_stream_cb,
										&p,
										0);

	for (blkno = BLOOM_HEAD_BLKNO; blkno < npages; blkno++)
	{
		Buffer		buffer;
		Page		page;

		buffer = read_stream_next_buffer(stream, NULL);
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buffer);

		if (!PageIsNew(page) && !BloomPageIsDeleted(page))
		{
			OffsetNumber offset,
						maxOffset = BloomPageGetMaxOffset(page);

			for (offset = 1; offset <= maxOffset; offset++)
			{
				BloomTuple *itup = BloomPageGetTuple(&so->state, page, offset);
				bool		res = true;

				/* Check index signature with scan signature */
				for (i = 0; i < so->state.opts.bloomLength; i++)
				{
					if ((itup->sign[i] & so->sign[i]) != so->sign[i])
					{
						res = false;
						break;
					}
				}

				/* Add matching tuples to bitmap */
				if (res)
				{
					tbm_add_tuples(tbm, &itup->heapPtr, 1, true);
					ntids++;
				}
			}
		}

		UnlockReleaseBuffer(buffer);
		CHECK_FOR_INTERRUPTS();
	}

	Assert(read_stream_next_buffer(stream, NULL) == InvalidBuffer);
	read_stream_end(stream);
	FreeAccessStrategy(bas);

	return ntids;
}
