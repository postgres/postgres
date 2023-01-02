/*-------------------------------------------------------------------------
 *
 * hashsearch.c
 *	  search code for postgres hash tables
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
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
#include "storage/predicate.h"
#include "utils/rel.h"

static bool _hash_readpage(IndexScanDesc scan, Buffer *bufP,
						   ScanDirection dir);
static int	_hash_load_qualified_items(IndexScanDesc scan, Page page,
									   OffsetNumber offnum, ScanDirection dir);
static inline void _hash_saveitem(HashScanOpaque so, int itemIndex,
								  OffsetNumber offnum, IndexTuple itup);
static void _hash_readnext(IndexScanDesc scan, Buffer *bufp,
						   Page *pagep, HashPageOpaque *opaquep);

/*
 *	_hash_next() -- Get the next item in a scan.
 *
 *		On entry, so->currPos describes the current page, which may
 *		be pinned but not locked, and so->currPos.itemIndex identifies
 *		which item was previously returned.
 *
 *		On successful exit, scan->xs_ctup.t_self is set to the TID
 *		of the next heap tuple. so->currPos is updated as needed.
 *
 *		On failure exit (no more tuples), we return false with pin
 *		held on bucket page but no pins or locks held on overflow
 *		page.
 */
bool
_hash_next(IndexScanDesc scan, ScanDirection dir)
{
	Relation	rel = scan->indexRelation;
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	HashScanPosItem *currItem;
	BlockNumber blkno;
	Buffer		buf;
	bool		end_of_scan = false;

	/*
	 * Advance to the next tuple on the current page; or if done, try to read
	 * data from the next or previous page based on the scan direction. Before
	 * moving to the next or previous page make sure that we deal with all the
	 * killed items.
	 */
	if (ScanDirectionIsForward(dir))
	{
		if (++so->currPos.itemIndex > so->currPos.lastItem)
		{
			if (so->numKilled > 0)
				_hash_kill_items(scan);

			blkno = so->currPos.nextPage;
			if (BlockNumberIsValid(blkno))
			{
				buf = _hash_getbuf(rel, blkno, HASH_READ, LH_OVERFLOW_PAGE);
				TestForOldSnapshot(scan->xs_snapshot, rel, BufferGetPage(buf));
				if (!_hash_readpage(scan, &buf, dir))
					end_of_scan = true;
			}
			else
				end_of_scan = true;
		}
	}
	else
	{
		if (--so->currPos.itemIndex < so->currPos.firstItem)
		{
			if (so->numKilled > 0)
				_hash_kill_items(scan);

			blkno = so->currPos.prevPage;
			if (BlockNumberIsValid(blkno))
			{
				buf = _hash_getbuf(rel, blkno, HASH_READ,
								   LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
				TestForOldSnapshot(scan->xs_snapshot, rel, BufferGetPage(buf));

				/*
				 * We always maintain the pin on bucket page for whole scan
				 * operation, so releasing the additional pin we have acquired
				 * here.
				 */
				if (buf == so->hashso_bucket_buf ||
					buf == so->hashso_split_bucket_buf)
					_hash_dropbuf(rel, buf);

				if (!_hash_readpage(scan, &buf, dir))
					end_of_scan = true;
			}
			else
				end_of_scan = true;
		}
	}

	if (end_of_scan)
	{
		_hash_dropscanbuf(rel, so);
		HashScanPosInvalidate(so->currPos);
		return false;
	}

	/* OK, itemIndex says what to return */
	currItem = &so->currPos.items[so->currPos.itemIndex];
	scan->xs_heaptid = currItem->heapTid;

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
		PredicateLockPage(rel, BufferGetBlockNumber(*bufp), scan->xs_snapshot);

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
		TestForOldSnapshot(scan->xs_snapshot, rel, *pagep);
		*opaquep = HashPageGetOpaque(*pagep);
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
	bool		haveprevblk;

	blkno = (*opaquep)->hasho_prevblkno;

	/*
	 * Retain the pin on primary bucket page till the end of scan.  Refer the
	 * comments in _hash_first to know the reason of retaining pin.
	 */
	if (*bufp == so->hashso_bucket_buf || *bufp == so->hashso_split_bucket_buf)
	{
		LockBuffer(*bufp, BUFFER_LOCK_UNLOCK);
		haveprevblk = false;
	}
	else
	{
		_hash_relbuf(rel, *bufp);
		haveprevblk = true;
	}

	*bufp = InvalidBuffer;
	/* check for interrupts while we're not holding any buffer lock */
	CHECK_FOR_INTERRUPTS();

	if (haveprevblk)
	{
		Assert(BlockNumberIsValid(blkno));
		*bufp = _hash_getbuf(rel, blkno, HASH_READ,
							 LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
		*pagep = BufferGetPage(*bufp);
		TestForOldSnapshot(scan->xs_snapshot, rel, *pagep);
		*opaquep = HashPageGetOpaque(*pagep);

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
		*opaquep = HashPageGetOpaque(*pagep);

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
 *		We find the first item (or, if backward scan, the last item) in the
 *		index that satisfies the qualification associated with the scan
 *		descriptor.
 *
 *		On successful exit, if the page containing current index tuple is an
 *		overflow page, both pin and lock are released whereas if it is a bucket
 *		page then it is pinned but not locked and data about the matching
 *		tuple(s) on the page has been loaded into so->currPos,
 *		scan->xs_ctup.t_self is set to the heap TID of the current tuple.
 *
 *		On failure exit (no more tuples), we return false, with pin held on
 *		bucket page but no pins or locks held on overflow page.
 */
bool
_hash_first(IndexScanDesc scan, ScanDirection dir)
{
	Relation	rel = scan->indexRelation;
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	ScanKey		cur;
	uint32		hashkey;
	Bucket		bucket;
	Buffer		buf;
	Page		page;
	HashPageOpaque opaque;
	HashScanPosItem *currItem;

	pgstat_count_index_scan(rel);

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

	buf = _hash_getbucketbuf_from_hashkey(rel, hashkey, HASH_READ, NULL);
	PredicateLockPage(rel, BufferGetBlockNumber(buf), scan->xs_snapshot);
	page = BufferGetPage(buf);
	TestForOldSnapshot(scan->xs_snapshot, rel, page);
	opaque = HashPageGetOpaque(page);
	bucket = opaque->hasho_bucket;

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
		TestForOldSnapshot(scan->xs_snapshot, rel, BufferGetPage(old_buf));

		/*
		 * remember the split bucket buffer so as to use it later for
		 * scanning.
		 */
		so->hashso_split_bucket_buf = old_buf;
		LockBuffer(old_buf, BUFFER_LOCK_UNLOCK);

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		opaque = HashPageGetOpaque(page);
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

	/* remember which buffer we have pinned, if any */
	Assert(BufferIsInvalid(so->currPos.buf));
	so->currPos.buf = buf;

	/* Now find all the tuples satisfying the qualification from a page */
	if (!_hash_readpage(scan, &buf, dir))
		return false;

	/* OK, itemIndex says what to return */
	currItem = &so->currPos.items[so->currPos.itemIndex];
	scan->xs_heaptid = currItem->heapTid;

	/* if we're here, _hash_readpage found a valid tuples */
	return true;
}

/*
 *	_hash_readpage() -- Load data from current index page into so->currPos
 *
 *	We scan all the items in the current index page and save them into
 *	so->currPos if it satisfies the qualification. If no matching items
 *	are found in the current page, we move to the next or previous page
 *	in a bucket chain as indicated by the direction.
 *
 *	Return true if any matching items are found else return false.
 */
static bool
_hash_readpage(IndexScanDesc scan, Buffer *bufP, ScanDirection dir)
{
	Relation	rel = scan->indexRelation;
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	Buffer		buf;
	Page		page;
	HashPageOpaque opaque;
	OffsetNumber offnum;
	uint16		itemIndex;

	buf = *bufP;
	Assert(BufferIsValid(buf));
	_hash_checkpage(rel, buf, LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
	page = BufferGetPage(buf);
	opaque = HashPageGetOpaque(page);

	so->currPos.buf = buf;
	so->currPos.currPage = BufferGetBlockNumber(buf);

	if (ScanDirectionIsForward(dir))
	{
		BlockNumber prev_blkno = InvalidBlockNumber;

		for (;;)
		{
			/* new page, locate starting position by binary search */
			offnum = _hash_binsearch(page, so->hashso_sk_hash);

			itemIndex = _hash_load_qualified_items(scan, page, offnum, dir);

			if (itemIndex != 0)
				break;

			/*
			 * Could not find any matching tuples in the current page, move to
			 * the next page. Before leaving the current page, deal with any
			 * killed items.
			 */
			if (so->numKilled > 0)
				_hash_kill_items(scan);

			/*
			 * If this is a primary bucket page, hasho_prevblkno is not a real
			 * block number.
			 */
			if (so->currPos.buf == so->hashso_bucket_buf ||
				so->currPos.buf == so->hashso_split_bucket_buf)
				prev_blkno = InvalidBlockNumber;
			else
				prev_blkno = opaque->hasho_prevblkno;

			_hash_readnext(scan, &buf, &page, &opaque);
			if (BufferIsValid(buf))
			{
				so->currPos.buf = buf;
				so->currPos.currPage = BufferGetBlockNumber(buf);
			}
			else
			{
				/*
				 * Remember next and previous block numbers for scrollable
				 * cursors to know the start position and return false
				 * indicating that no more matching tuples were found. Also,
				 * don't reset currPage or lsn, because we expect
				 * _hash_kill_items to be called for the old page after this
				 * function returns.
				 */
				so->currPos.prevPage = prev_blkno;
				so->currPos.nextPage = InvalidBlockNumber;
				so->currPos.buf = buf;
				return false;
			}
		}

		so->currPos.firstItem = 0;
		so->currPos.lastItem = itemIndex - 1;
		so->currPos.itemIndex = 0;
	}
	else
	{
		BlockNumber next_blkno = InvalidBlockNumber;

		for (;;)
		{
			/* new page, locate starting position by binary search */
			offnum = _hash_binsearch_last(page, so->hashso_sk_hash);

			itemIndex = _hash_load_qualified_items(scan, page, offnum, dir);

			if (itemIndex != MaxIndexTuplesPerPage)
				break;

			/*
			 * Could not find any matching tuples in the current page, move to
			 * the previous page. Before leaving the current page, deal with
			 * any killed items.
			 */
			if (so->numKilled > 0)
				_hash_kill_items(scan);

			if (so->currPos.buf == so->hashso_bucket_buf ||
				so->currPos.buf == so->hashso_split_bucket_buf)
				next_blkno = opaque->hasho_nextblkno;

			_hash_readprev(scan, &buf, &page, &opaque);
			if (BufferIsValid(buf))
			{
				so->currPos.buf = buf;
				so->currPos.currPage = BufferGetBlockNumber(buf);
			}
			else
			{
				/*
				 * Remember next and previous block numbers for scrollable
				 * cursors to know the start position and return false
				 * indicating that no more matching tuples were found. Also,
				 * don't reset currPage or lsn, because we expect
				 * _hash_kill_items to be called for the old page after this
				 * function returns.
				 */
				so->currPos.prevPage = InvalidBlockNumber;
				so->currPos.nextPage = next_blkno;
				so->currPos.buf = buf;
				return false;
			}
		}

		so->currPos.firstItem = itemIndex;
		so->currPos.lastItem = MaxIndexTuplesPerPage - 1;
		so->currPos.itemIndex = MaxIndexTuplesPerPage - 1;
	}

	if (so->currPos.buf == so->hashso_bucket_buf ||
		so->currPos.buf == so->hashso_split_bucket_buf)
	{
		so->currPos.prevPage = InvalidBlockNumber;
		so->currPos.nextPage = opaque->hasho_nextblkno;
		LockBuffer(so->currPos.buf, BUFFER_LOCK_UNLOCK);
	}
	else
	{
		so->currPos.prevPage = opaque->hasho_prevblkno;
		so->currPos.nextPage = opaque->hasho_nextblkno;
		_hash_relbuf(rel, so->currPos.buf);
		so->currPos.buf = InvalidBuffer;
	}

	Assert(so->currPos.firstItem <= so->currPos.lastItem);
	return true;
}

/*
 * Load all the qualified items from a current index page
 * into so->currPos. Helper function for _hash_readpage.
 */
static int
_hash_load_qualified_items(IndexScanDesc scan, Page page,
						   OffsetNumber offnum, ScanDirection dir)
{
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	IndexTuple	itup;
	int			itemIndex;
	OffsetNumber maxoff;

	maxoff = PageGetMaxOffsetNumber(page);

	if (ScanDirectionIsForward(dir))
	{
		/* load items[] in ascending order */
		itemIndex = 0;

		while (offnum <= maxoff)
		{
			Assert(offnum >= FirstOffsetNumber);
			itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offnum));

			/*
			 * skip the tuples that are moved by split operation for the scan
			 * that has started when split was in progress. Also, skip the
			 * tuples that are marked as dead.
			 */
			if ((so->hashso_buc_populated && !so->hashso_buc_split &&
				 (itup->t_info & INDEX_MOVED_BY_SPLIT_MASK)) ||
				(scan->ignore_killed_tuples &&
				 (ItemIdIsDead(PageGetItemId(page, offnum)))))
			{
				offnum = OffsetNumberNext(offnum);	/* move forward */
				continue;
			}

			if (so->hashso_sk_hash == _hash_get_indextuple_hashkey(itup) &&
				_hash_checkqual(scan, itup))
			{
				/* tuple is qualified, so remember it */
				_hash_saveitem(so, itemIndex, offnum, itup);
				itemIndex++;
			}
			else
			{
				/*
				 * No more matching tuples exist in this page. so, exit while
				 * loop.
				 */
				break;
			}

			offnum = OffsetNumberNext(offnum);
		}

		Assert(itemIndex <= MaxIndexTuplesPerPage);
		return itemIndex;
	}
	else
	{
		/* load items[] in descending order */
		itemIndex = MaxIndexTuplesPerPage;

		while (offnum >= FirstOffsetNumber)
		{
			Assert(offnum <= maxoff);
			itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offnum));

			/*
			 * skip the tuples that are moved by split operation for the scan
			 * that has started when split was in progress. Also, skip the
			 * tuples that are marked as dead.
			 */
			if ((so->hashso_buc_populated && !so->hashso_buc_split &&
				 (itup->t_info & INDEX_MOVED_BY_SPLIT_MASK)) ||
				(scan->ignore_killed_tuples &&
				 (ItemIdIsDead(PageGetItemId(page, offnum)))))
			{
				offnum = OffsetNumberPrev(offnum);	/* move back */
				continue;
			}

			if (so->hashso_sk_hash == _hash_get_indextuple_hashkey(itup) &&
				_hash_checkqual(scan, itup))
			{
				itemIndex--;
				/* tuple is qualified, so remember it */
				_hash_saveitem(so, itemIndex, offnum, itup);
			}
			else
			{
				/*
				 * No more matching tuples exist in this page. so, exit while
				 * loop.
				 */
				break;
			}

			offnum = OffsetNumberPrev(offnum);
		}

		Assert(itemIndex >= 0);
		return itemIndex;
	}
}

/* Save an index item into so->currPos.items[itemIndex] */
static inline void
_hash_saveitem(HashScanOpaque so, int itemIndex,
			   OffsetNumber offnum, IndexTuple itup)
{
	HashScanPosItem *currItem = &so->currPos.items[itemIndex];

	currItem->heapTid = itup->t_tid;
	currItem->indexOffset = offnum;
}
