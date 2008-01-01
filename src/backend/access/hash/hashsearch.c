/*-------------------------------------------------------------------------
 *
 * hashsearch.c
 *	  search code for postgres hash tables
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/hash/hashsearch.c,v 1.51 2008/01/01 19:45:46 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "pgstat.h"


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
	scan->xs_ctup.t_self = itup->t_tid;

	return true;
}

/*
 * Advance to next page in a bucket, if any.
 */
static void
_hash_readnext(Relation rel,
			   Buffer *bufp, Page *pagep, HashPageOpaque *opaquep)
{
	BlockNumber blkno;

	blkno = (*opaquep)->hasho_nextblkno;
	_hash_relbuf(rel, *bufp);
	*bufp = InvalidBuffer;
	if (BlockNumberIsValid(blkno))
	{
		*bufp = _hash_getbuf(rel, blkno, HASH_READ, LH_OVERFLOW_PAGE);
		*pagep = BufferGetPage(*bufp);
		*opaquep = (HashPageOpaque) PageGetSpecialPointer(*pagep);
	}
}

/*
 * Advance to previous page in a bucket, if any.
 */
static void
_hash_readprev(Relation rel,
			   Buffer *bufp, Page *pagep, HashPageOpaque *opaquep)
{
	BlockNumber blkno;

	blkno = (*opaquep)->hasho_prevblkno;
	_hash_relbuf(rel, *bufp);
	*bufp = InvalidBuffer;
	if (BlockNumberIsValid(blkno))
	{
		*bufp = _hash_getbuf(rel, blkno, HASH_READ,
							 LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
		*pagep = BufferGetPage(*bufp);
		*opaquep = (HashPageOpaque) PageGetSpecialPointer(*pagep);
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

	/*
	 * Acquire shared split lock so we can compute the target bucket safely
	 * (see README).
	 */
	_hash_getlock(rel, 0, HASH_SHARE);

	/* Read the metapage */
	metabuf = _hash_getbuf(rel, HASH_METAPAGE, HASH_READ, LH_META_PAGE);
	metap = (HashMetaPage) BufferGetPage(metabuf);

	/*
	 * Compute the target bucket number, and convert to block number.
	 */
	bucket = _hash_hashkey2bucket(hashkey,
								  metap->hashm_maxbucket,
								  metap->hashm_highmask,
								  metap->hashm_lowmask);

	blkno = BUCKET_TO_BLKNO(metap, bucket);

	/* done with the metapage */
	_hash_relbuf(rel, metabuf);

	/*
	 * Acquire share lock on target bucket; then we can release split lock.
	 */
	_hash_getlock(rel, blkno, HASH_SHARE);

	_hash_droplock(rel, 0, HASH_SHARE);

	/* Update scan opaque state to show we have lock on the bucket */
	so->hashso_bucket = bucket;
	so->hashso_bucket_valid = true;
	so->hashso_bucket_blkno = blkno;

	/* Fetch the primary bucket page for the bucket */
	buf = _hash_getbuf(rel, blkno, HASH_READ, LH_BUCKET_PAGE);
	page = BufferGetPage(buf);
	opaque = (HashPageOpaque) PageGetSpecialPointer(page);
	Assert(opaque->hasho_bucket == bucket);

	/* If a backwards scan is requested, move to the end of the chain */
	if (ScanDirectionIsBackward(dir))
	{
		while (BlockNumberIsValid(opaque->hasho_nextblkno))
			_hash_readnext(rel, &buf, &page, &opaque);
	}

	/* Now find the first tuple satisfying the qualification */
	if (!_hash_step(scan, &buf, dir))
		return false;

	/* if we're here, _hash_step found a valid tuple */
	offnum = ItemPointerGetOffsetNumber(current);
	_hash_checkpage(rel, buf, LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
	page = BufferGetPage(buf);
	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offnum));
	scan->xs_ctup.t_self = itup->t_tid;

	return true;
}

/*
 *	_hash_step() -- step to the next valid item in a scan in the bucket.
 *
 *		If no valid record exists in the requested direction, return
 *		false.	Else, return true and set the hashso_curpos for the
 *		scan to the right thing.
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
	 * 'offnum' now points to the last tuple we have seen (if any).
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
					offnum = FirstOffsetNumber; /* new page */

				while (offnum > maxoff)
				{
					/*
					 * either this page is empty (maxoff ==
					 * InvalidOffsetNumber) or we ran off the end.
					 */
					_hash_readnext(rel, &buf, &page, &opaque);
					if (BufferIsValid(buf))
					{
						maxoff = PageGetMaxOffsetNumber(page);
						offnum = FirstOffsetNumber;
					}
					else
					{
						/* end of bucket */
						maxoff = offnum = InvalidOffsetNumber;
						break;	/* exit while */
					}
				}
				break;

			case BackwardScanDirection:
				if (offnum != InvalidOffsetNumber)
					offnum = OffsetNumberPrev(offnum);	/* move back */
				else
					offnum = maxoff;	/* new page */

				while (offnum < FirstOffsetNumber)
				{
					/*
					 * either this page is empty (offnum ==
					 * InvalidOffsetNumber) or we ran off the end.
					 */
					_hash_readprev(rel, &buf, &page, &opaque);
					if (BufferIsValid(buf))
						maxoff = offnum = PageGetMaxOffsetNumber(page);
					else
					{
						/* end of bucket */
						maxoff = offnum = InvalidOffsetNumber;
						break;	/* exit while */
					}
				}
				break;

			default:
				/* NoMovementScanDirection */
				/* this should not be reached */
				break;
		}

		/* we ran off the end of the world without finding a match */
		if (offnum == InvalidOffsetNumber)
		{
			*bufP = so->hashso_curbuf = InvalidBuffer;
			ItemPointerSetInvalid(current);
			return false;
		}

		/* get ready to check this tuple */
		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offnum));
	} while (!_hash_checkqual(scan, itup));

	/* if we made it to here, we've found a valid tuple */
	blkno = BufferGetBlockNumber(buf);
	*bufP = so->hashso_curbuf = buf;
	ItemPointerSet(current, blkno, offnum);
	return true;
}
