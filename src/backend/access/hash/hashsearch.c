/*-------------------------------------------------------------------------
 *
 * hashsearch.c
 *	  search code for postgres hash tables
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/hash/hashsearch.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "access/relscan.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "utils/rel.h"


/*
 *	_hash_next() -- Get the next item in a scan.
 *
 *		On entry, we have a valid hashso_curpos in the scan, and a
 *		pin and read lock on the page that contains that item.
 *		We find the next item in the scan, if any.
 *		On success exit, we have the page containing the next item
 *		pinned and locked.
 */
bool
_hash_next(IndexScanDesc scan, ScanDirection dir)
{
	Relation	rel = scan->indexRelation;
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	Buffer		buf;
	Page		page;
	OffsetNumber offnum;
	ItemPointer current;
	IndexTuple	itup;

	/* we still have the buffer pinned and read-locked */
	buf = so->hashso_curbuf;
	Assert(BufferIsValid(buf));

	/*
	 * step to next valid tuple.
	 */
	if (!_hash_step(scan, &buf, dir))
		return false;

	/* if we're here, _hash_step found a valid tuple */
	current = &(so->hashso_curpos);
	offnum = ItemPointerGetOffsetNumber(current);
	_hash_checkpage(rel, buf, LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
	page = BufferGetPage(buf);
	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offnum));
	so->hashso_heappos = itup->t_tid;

	return true;
}

/*
 * Advance to next page in a bucket, if any.  If we are scanning the bucket
 * being populated during split operation then this function advances to the
 * bucket being split after the last bucket page of bucket being populated.
 */
static void
_hash_readnext(IndexScanDesc scan,
			   Buffer *bufp, Page *pagep, HashPageOpaque *opaquep)
{
	BlockNumber blkno;
	Relation	rel = scan->indexRelation;
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	bool		block_found = false;

	blkno = (*opaquep)->hasho_nextblkno;

	/*
	 * Retain the pin on primary bucket page till the end of scan.  Refer the
	 * comments in _hash_first to know the reason of retaining pin.
	 */
	if (*bufp == so->hashso_bucket_buf || *bufp == so->hashso_split_bucket_buf)
		LockBuffer(*bufp, BUFFER_LOCK_UNLOCK);
	else
		_hash_relbuf(rel, *bufp);

	*bufp = InvalidBuffer;
	/* check for interrupts while we're not holding any buffer lock */
	CHECK_FOR_INTERRUPTS();
	if (BlockNumberIsValid(blkno))
	{
		*bufp = _hash_getbuf(rel, blkno, HASH_READ, LH_OVERFLOW_PAGE);
		block_found = true;
	}
	else if (so->hashso_buc_populated && !so->hashso_buc_split)
	{
		/*
		 * end of bucket, scan bucket being split if there was a split in
		 * progress at the start of scan.
		 */
		*bufp = so->hashso_split_bucket_buf;

		/*
		 * buffer for bucket being split must be valid as we acquire the pin
		 * on it before the start of scan and retain it till end of scan.
		 */
		Assert(BufferIsValid(*bufp));

		LockBuffer(*bufp, BUFFER_LOCK_SHARE);

		/*
		 * setting hashso_buc_split to true indicates that we are scanning
		 * bucket being split.
		 */
		so->hashso_buc_split = true;

		block_found = true;
	}

	if (block_found)
	{
		*pagep = BufferGetPage(*bufp);
		*opaquep = (HashPageOpaque) PageGetSpecialPointer(*pagep);
	}
}

/*
 * Advance to previous page in a bucket, if any.  If the current scan has
 * started during split operation then this function advances to bucket
 * being populated after the first bucket page of bucket being split.
 */
static void
_hash_readprev(IndexScanDesc scan,
			   Buffer *bufp, Page *pagep, HashPageOpaque *opaquep)
{
	BlockNumber blkno;
	Relation	rel = scan->indexRelation;
	HashScanOpaque so = (HashScanOpaque) scan->opaque;

	blkno = (*opaquep)->hasho_prevblkno;

	/*
	 * Retain the pin on primary bucket page till the end of scan.  Refer the
	 * comments in _hash_first to know the reason of retaining pin.
	 */
	if (*bufp == so->hashso_bucket_buf || *bufp == so->hashso_split_bucket_buf)
		LockBuffer(*bufp, BUFFER_LOCK_UNLOCK);
	else
		_hash_relbuf(rel, *bufp);

	*bufp = InvalidBuffer;
	/* check for interrupts while we're not holding any buffer lock */
	CHECK_FOR_INTERRUPTS();
	if (BlockNumberIsValid(blkno))
	{
		*bufp = _hash_getbuf(rel, blkno, HASH_READ,
							 LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
		*pagep = BufferGetPage(*bufp);
		*opaquep = (HashPageOpaque) PageGetSpecialPointer(*pagep);

		/*
		 * We always maintain the pin on bucket page for whole scan operation,
		 * so releasing the additional pin we have acquired here.
		 */
		if (*bufp == so->hashso_bucket_buf || *bufp == so->hashso_split_bucket_buf)
			_hash_dropbuf(rel, *bufp);
	}
	else if (so->hashso_buc_populated && so->hashso_buc_split)
	{
		/*
		 * end of bucket, scan bucket being populated if there was a split in
		 * progress at the start of scan.
		 */
		*bufp = so->hashso_bucket_buf;

		/*
		 * buffer for bucket being populated must be valid as we acquire the
		 * pin on it before the start of scan and retain it till end of scan.
		 */
		Assert(BufferIsValid(*bufp));

		LockBuffer(*bufp, BUFFER_LOCK_SHARE);
		*pagep = BufferGetPage(*bufp);
		*opaquep = (HashPageOpaque) PageGetSpecialPointer(*pagep);

		/* move to the end of bucket chain */
		while (BlockNumberIsValid((*opaquep)->hasho_nextblkno))
			_hash_readnext(scan, bufp, pagep, opaquep);

		/*
		 * setting hashso_buc_split to false indicates that we are scanning
		 * bucket being populated.
		 */
		so->hashso_buc_split = false;
	}
}

/*
 *	_hash_first() -- Find the first item in a scan.
 *
 *		Find the first item in the index that
 *		satisfies the qualification associated with the scan descriptor. On
 *		success, the page containing the current index tuple is read locked
 *		and pinned, and the scan's opaque data entry is updated to
 *		include the buffer.
 */
bool
_hash_first(IndexScanDesc scan, ScanDirection dir)
{
	Relation	rel = scan->indexRelation;
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	ScanKey		cur;
	uint32		hashkey;
	Bucket		bucket;
	BlockNumber blkno;
	BlockNumber oldblkno = InvalidBuffer;
	bool		retry = false;
	Buffer		buf;
	Buffer		metabuf;
	Page		page;
	HashPageOpaque opaque;
	HashMetaPage metap;
	IndexTuple	itup;
	ItemPointer current;
	OffsetNumber offnum;

	pgstat_count_index_scan(rel);

	current = &(so->hashso_curpos);
	ItemPointerSetInvalid(current);

	/*
	 * We do not support hash scans with no index qualification, because we
	 * would have to read the whole index rather than just one bucket. That
	 * creates a whole raft of problems, since we haven't got a practical way
	 * to lock all the buckets against splits or compactions.
	 */
	if (scan->numberOfKeys < 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("hash indexes do not support whole-index scans")));

	/* There may be more than one index qual, but we hash only the first */
	cur = &scan->keyData[0];

	/* We support only single-column hash indexes */
	Assert(cur->sk_attno == 1);
	/* And there's only one operator strategy, too */
	Assert(cur->sk_strategy == HTEqualStrategyNumber);

	/*
	 * If the constant in the index qual is NULL, assume it cannot match any
	 * items in the index.
	 */
	if (cur->sk_flags & SK_ISNULL)
		return false;

	/*
	 * Okay to compute the hash key.  We want to do this before acquiring any
	 * locks, in case a user-defined hash function happens to be slow.
	 *
	 * If scankey operator is not a cross-type comparison, we can use the
	 * cached hash function; otherwise gotta look it up in the catalogs.
	 *
	 * We support the convention that sk_subtype == InvalidOid means the
	 * opclass input type; this is a hack to simplify life for ScanKeyInit().
	 */
	if (cur->sk_subtype == rel->rd_opcintype[0] ||
		cur->sk_subtype == InvalidOid)
		hashkey = _hash_datum2hashkey(rel, cur->sk_argument);
	else
		hashkey = _hash_datum2hashkey_type(rel, cur->sk_argument,
										   cur->sk_subtype);

	so->hashso_sk_hash = hashkey;

	/* Read the metapage */
	metabuf = _hash_getbuf(rel, HASH_METAPAGE, HASH_READ, LH_META_PAGE);
	page = BufferGetPage(metabuf);
	metap = HashPageGetMeta(page);

	/*
	 * Loop until we get a lock on the correct target bucket.
	 */
	for (;;)
	{
		/*
		 * Compute the target bucket number, and convert to block number.
		 */
		bucket = _hash_hashkey2bucket(hashkey,
									  metap->hashm_maxbucket,
									  metap->hashm_highmask,
									  metap->hashm_lowmask);

		blkno = BUCKET_TO_BLKNO(metap, bucket);

		/* Release metapage lock, but keep pin. */
		LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);

		/*
		 * If the previous iteration of this loop locked what is still the
		 * correct target bucket, we are done.  Otherwise, drop any old lock
		 * and lock what now appears to be the correct bucket.
		 */
		if (retry)
		{
			if (oldblkno == blkno)
				break;
			_hash_relbuf(rel, buf);
		}

		/* Fetch the primary bucket page for the bucket */
		buf = _hash_getbuf(rel, blkno, HASH_READ, LH_BUCKET_PAGE);

		/*
		 * Reacquire metapage lock and check that no bucket split has taken
		 * place while we were awaiting the bucket lock.
		 */
		LockBuffer(metabuf, BUFFER_LOCK_SHARE);
		oldblkno = blkno;
		retry = true;
	}

	/* done with the metapage */
	_hash_dropbuf(rel, metabuf);

	page = BufferGetPage(buf);
	opaque = (HashPageOpaque) PageGetSpecialPointer(page);
	Assert(opaque->hasho_bucket == bucket);

	so->hashso_bucket_buf = buf;

	/*
	 * If a bucket split is in progress, then while scanning the bucket being
	 * populated, we need to skip tuples that were copied from bucket being
	 * split.  We also need to maintain a pin on the bucket being split to
	 * ensure that split-cleanup work done by vacuum doesn't remove tuples
	 * from it till this scan is done.  We need to maintain a pin on the
	 * bucket being populated to ensure that vacuum doesn't squeeze that
	 * bucket till this scan is complete; otherwise, the ordering of tuples
	 * can't be maintained during forward and backward scans.  Here, we have
	 * to be cautious about locking order: first, acquire the lock on bucket
	 * being split; then, release the lock on it but not the pin; then,
	 * acquire a lock on bucket being populated and again re-verify whether
	 * the bucket split is still in progress.  Acquiring the lock on bucket
	 * being split first ensures that the vacuum waits for this scan to
	 * finish.
	 */
	if (H_BUCKET_BEING_POPULATED(opaque))
	{
		BlockNumber old_blkno;
		Buffer		old_buf;

		old_blkno = _hash_get_oldblock_from_newbucket(rel, bucket);

		/*
		 * release the lock on new bucket and re-acquire it after acquiring
		 * the lock on old bucket.
		 */
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);

		old_buf = _hash_getbuf(rel, old_blkno, HASH_READ, LH_BUCKET_PAGE);

		/*
		 * remember the split bucket buffer so as to use it later for
		 * scanning.
		 */
		so->hashso_split_bucket_buf = old_buf;
		LockBuffer(old_buf, BUFFER_LOCK_UNLOCK);

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		opaque = (HashPageOpaque) PageGetSpecialPointer(page);
		Assert(opaque->hasho_bucket == bucket);

		if (H_BUCKET_BEING_POPULATED(opaque))
			so->hashso_buc_populated = true;
		else
		{
			_hash_dropbuf(rel, so->hashso_split_bucket_buf);
			so->hashso_split_bucket_buf = InvalidBuffer;
		}
	}

	/* If a backwards scan is requested, move to the end of the chain */
	if (ScanDirectionIsBackward(dir))
	{
		/*
		 * Backward scans that start during split needs to start from end of
		 * bucket being split.
		 */
		while (BlockNumberIsValid(opaque->hasho_nextblkno) ||
			   (so->hashso_buc_populated && !so->hashso_buc_split))
			_hash_readnext(scan, &buf, &page, &opaque);
	}

	/* Now find the first tuple satisfying the qualification */
	if (!_hash_step(scan, &buf, dir))
		return false;

	/* if we're here, _hash_step found a valid tuple */
	offnum = ItemPointerGetOffsetNumber(current);
	_hash_checkpage(rel, buf, LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
	page = BufferGetPage(buf);
	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offnum));
	so->hashso_heappos = itup->t_tid;

	return true;
}

/*
 *	_hash_step() -- step to the next valid item in a scan in the bucket.
 *
 *		If no valid record exists in the requested direction, return
 *		false.  Else, return true and set the hashso_curpos for the
 *		scan to the right thing.
 *
 *		Here we need to ensure that if the scan has started during split, then
 *		skip the tuples that are moved by split while scanning bucket being
 *		populated and then scan the bucket being split to cover all such
 *		tuples.  This is done to ensure that we don't miss tuples in the scans
 *		that are started during split.
 *
 *		'bufP' points to the current buffer, which is pinned and read-locked.
 *		On success exit, we have pin and read-lock on whichever page
 *		contains the right item; on failure, we have released all buffers.
 */
bool
_hash_step(IndexScanDesc scan, Buffer *bufP, ScanDirection dir)
{
	Relation	rel = scan->indexRelation;
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	ItemPointer current;
	Buffer		buf;
	Page		page;
	HashPageOpaque opaque;
	OffsetNumber maxoff;
	OffsetNumber offnum;
	BlockNumber blkno;
	IndexTuple	itup;

	current = &(so->hashso_curpos);

	buf = *bufP;
	_hash_checkpage(rel, buf, LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
	page = BufferGetPage(buf);
	opaque = (HashPageOpaque) PageGetSpecialPointer(page);

	/*
	 * If _hash_step is called from _hash_first, current will not be valid, so
	 * we can't dereference it.  However, in that case, we presumably want to
	 * start at the beginning/end of the page...
	 */
	maxoff = PageGetMaxOffsetNumber(page);
	if (ItemPointerIsValid(current))
		offnum = ItemPointerGetOffsetNumber(current);
	else
		offnum = InvalidOffsetNumber;

	/*
	 * 'offnum' now points to the last tuple we examined (if any).
	 *
	 * continue to step through tuples until: 1) we get to the end of the
	 * bucket chain or 2) we find a valid tuple.
	 */
	do
	{
		switch (dir)
		{
			case ForwardScanDirection:
				if (offnum != InvalidOffsetNumber)
					offnum = OffsetNumberNext(offnum);	/* move forward */
				else
				{
					/* new page, locate starting position by binary search */
					offnum = _hash_binsearch(page, so->hashso_sk_hash);
				}

				for (;;)
				{
					/*
					 * check if we're still in the range of items with the
					 * target hash key
					 */
					if (offnum <= maxoff)
					{
						Assert(offnum >= FirstOffsetNumber);
						itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offnum));

						/*
						 * skip the tuples that are moved by split operation
						 * for the scan that has started when split was in
						 * progress
						 */
						if (so->hashso_buc_populated && !so->hashso_buc_split &&
							(itup->t_info & INDEX_MOVED_BY_SPLIT_MASK))
						{
							offnum = OffsetNumberNext(offnum);	/* move forward */
							continue;
						}

						if (so->hashso_sk_hash == _hash_get_indextuple_hashkey(itup))
							break;		/* yes, so exit for-loop */
					}

					/*
					 * ran off the end of this page, try the next
					 */
					_hash_readnext(scan, &buf, &page, &opaque);
					if (BufferIsValid(buf))
					{
						maxoff = PageGetMaxOffsetNumber(page);
						offnum = _hash_binsearch(page, so->hashso_sk_hash);
					}
					else
					{
						itup = NULL;
						break;	/* exit for-loop */
					}
				}
				break;

			case BackwardScanDirection:
				if (offnum != InvalidOffsetNumber)
					offnum = OffsetNumberPrev(offnum);	/* move back */
				else
				{
					/* new page, locate starting position by binary search */
					offnum = _hash_binsearch_last(page, so->hashso_sk_hash);
				}

				for (;;)
				{
					/*
					 * check if we're still in the range of items with the
					 * target hash key
					 */
					if (offnum >= FirstOffsetNumber)
					{
						Assert(offnum <= maxoff);
						itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offnum));

						/*
						 * skip the tuples that are moved by split operation
						 * for the scan that has started when split was in
						 * progress
						 */
						if (so->hashso_buc_populated && !so->hashso_buc_split &&
							(itup->t_info & INDEX_MOVED_BY_SPLIT_MASK))
						{
							offnum = OffsetNumberPrev(offnum);	/* move back */
							continue;
						}

						if (so->hashso_sk_hash == _hash_get_indextuple_hashkey(itup))
							break;		/* yes, so exit for-loop */
					}

					/*
					 * ran off the end of this page, try the next
					 */
					_hash_readprev(scan, &buf, &page, &opaque);
					if (BufferIsValid(buf))
					{
						maxoff = PageGetMaxOffsetNumber(page);
						offnum = _hash_binsearch_last(page, so->hashso_sk_hash);
					}
					else
					{
						itup = NULL;
						break;	/* exit for-loop */
					}
				}
				break;

			default:
				/* NoMovementScanDirection */
				/* this should not be reached */
				itup = NULL;
				break;
		}

		if (itup == NULL)
		{
			/*
			 * We ran off the end of the bucket without finding a match.
			 * Release the pin on bucket buffers.  Normally, such pins are
			 * released at end of scan, however scrolling cursors can
			 * reacquire the bucket lock and pin in the same scan multiple
			 * times.
			 */
			*bufP = so->hashso_curbuf = InvalidBuffer;
			ItemPointerSetInvalid(current);
			_hash_dropscanbuf(rel, so);
			return false;
		}

		/* check the tuple quals, loop around if not met */
	} while (!_hash_checkqual(scan, itup));

	/* if we made it to here, we've found a valid tuple */
	blkno = BufferGetBlockNumber(buf);
	*bufP = so->hashso_curbuf = buf;
	ItemPointerSet(current, blkno, offnum);
	return true;
}
