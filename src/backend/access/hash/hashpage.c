/*-------------------------------------------------------------------------
 *
 * hashpage.c
 *	  Hash table page management code for the Postgres hash access method
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/hash/hashpage.c,v 1.42 2003/09/04 22:06:27 tgl Exp $
 *
 * NOTES
 *	  Postgres hash pages look like ordinary relation pages.  The opaque
 *	  data at high addresses includes information about the page including
 *	  whether a page is an overflow page or a true bucket, the bucket
 *	  number, and the block numbers of the preceding and following pages
 *	  in the same bucket.
 *
 *	  The first page in a hash relation, page zero, is special -- it stores
 *	  information describing the hash table; it is referred to as the
 *	  "meta page." Pages one and higher store the actual data.
 *
 *	  There are also bitmap pages, which are not manipulated here;
 *	  see hashovfl.c.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/hash.h"
#include "storage/lmgr.h"
#include "utils/lsyscache.h"


static void _hash_splitbucket(Relation rel, Buffer metabuf,
							  Bucket obucket, Bucket nbucket,
							  BlockNumber start_oblkno,
							  BlockNumber start_nblkno,
							  uint32 maxbucket,
							  uint32 highmask, uint32 lowmask);


/*
 * We use high-concurrency locking on hash indexes (see README for an overview
 * of the locking rules).  There are two cases in which we don't do locking.
 * One is when the index is newly created in the current transaction.  Since
 * the creating transaction has not committed, no one else can see the index,
 * and there's no reason to take locks.  The second case is for temp
 * relations, which no one else can see either.  (We still take buffer-level
 * locks, but not lmgr locks.)
 */
#define USELOCKING(rel)		(!((rel)->rd_isnew || (rel)->rd_istemp))


/*
 * _hash_getlock() -- Acquire an lmgr lock.
 *
 * 'whichlock' should be zero to acquire the split-control lock, or the
 * block number of a bucket's primary bucket page to acquire the per-bucket
 * lock.  (See README for details of the use of these locks.)
 *
 * 'access' must be HASH_SHARE or HASH_EXCLUSIVE.
 */
void
_hash_getlock(Relation rel, BlockNumber whichlock, int access)
{
	if (USELOCKING(rel))
		LockPage(rel, whichlock, access);
}

/*
 * _hash_try_getlock() -- Acquire an lmgr lock, but only if it's free.
 *
 * Same as above except we return FALSE without blocking if lock isn't free.
 */
bool
_hash_try_getlock(Relation rel, BlockNumber whichlock, int access)
{
	if (USELOCKING(rel))
		return ConditionalLockPage(rel, whichlock, access);
	else
		return true;
}

/*
 * _hash_droplock() -- Release an lmgr lock.
 */
void
_hash_droplock(Relation rel, BlockNumber whichlock, int access)
{
	if (USELOCKING(rel))
		UnlockPage(rel, whichlock, access);
}

/*
 *	_hash_getbuf() -- Get a buffer by block number for read or write.
 *
 *		'access' must be HASH_READ, HASH_WRITE, or HASH_NOLOCK.
 *
 *		When this routine returns, the appropriate lock is set on the
 *		requested buffer and its reference count has been incremented
 *		(ie, the buffer is "locked and pinned").
 *
 *		XXX P_NEW is not used because, unlike the tree structures, we
 *		need the bucket blocks to be at certain block numbers.	we must
 *		depend on the caller to call _hash_pageinit on the block if it
 *		knows that this is a new block.
 */
Buffer
_hash_getbuf(Relation rel, BlockNumber blkno, int access)
{
	Buffer		buf;

	if (blkno == P_NEW)
		elog(ERROR, "hash AM does not use P_NEW");

	buf = ReadBuffer(rel, blkno);

	if (access != HASH_NOLOCK)
		LockBuffer(buf, access);

	/* ref count and lock type are correct */
	return buf;
}

/*
 *	_hash_relbuf() -- release a locked buffer.
 *
 * Lock and pin (refcount) are both dropped.  Note that either read or
 * write lock can be dropped this way, but if we modified the buffer,
 * this is NOT the right way to release a write lock.
 */
void
_hash_relbuf(Relation rel, Buffer buf)
{
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buf);
}

/*
 *	_hash_dropbuf() -- release an unlocked buffer.
 *
 * This is used to unpin a buffer on which we hold no lock.  It is assumed
 * that the buffer is not dirty.
 */
void
_hash_dropbuf(Relation rel, Buffer buf)
{
	ReleaseBuffer(buf);
}

/*
 *	_hash_wrtbuf() -- write a hash page to disk.
 *
 *		This routine releases the lock held on the buffer and our refcount
 *		for it.  It is an error to call _hash_wrtbuf() without a write lock
 *		and a pin on the buffer.
 *
 * NOTE: actually, the buffer manager just marks the shared buffer page
 * dirty here; the real I/O happens later.	This is okay since we are not
 * relying on write ordering anyway.  The WAL mechanism is responsible for
 * guaranteeing correctness after a crash.
 */
void
_hash_wrtbuf(Relation rel, Buffer buf)
{
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	WriteBuffer(buf);
}

/*
 *	_hash_wrtnorelbuf() -- write a hash page to disk, but do not release
 *						 our reference or lock.
 *
 *		It is an error to call _hash_wrtnorelbuf() without a write lock
 *		and a pin on the buffer.
 *
 * See above NOTE.
 */
void
_hash_wrtnorelbuf(Relation rel, Buffer buf)
{
	WriteNoReleaseBuffer(buf);
}

/*
 * _hash_chgbufaccess() -- Change the lock type on a buffer, without
 *			dropping our pin on it.
 *
 * from_access and to_access may be HASH_READ, HASH_WRITE, or HASH_NOLOCK,
 * the last indicating that no buffer-level lock is held or wanted.
 *
 * When from_access == HASH_WRITE, we assume the buffer is dirty and tell
 * bufmgr it must be written out.  If the caller wants to release a write
 * lock on a page that's not been modified, it's okay to pass from_access
 * as HASH_READ (a bit ugly, but handy in some places).
 */
void
_hash_chgbufaccess(Relation rel,
				   Buffer buf,
				   int from_access,
				   int to_access)
{
	if (from_access != HASH_NOLOCK)
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	if (from_access == HASH_WRITE)
		WriteNoReleaseBuffer(buf);

	if (to_access != HASH_NOLOCK)
		LockBuffer(buf, to_access);
}


/*
 *	_hash_metapinit() -- Initialize the metadata page of a hash index,
 *				the two buckets that we begin with and the initial
 *				bitmap page.
 *
 * We are fairly cavalier about locking here, since we know that no one else
 * could be accessing this index.  In particular the rule about not holding
 * multiple buffer locks is ignored.
 */
void
_hash_metapinit(Relation rel)
{
	HashMetaPage metap;
	HashPageOpaque pageopaque;
	Buffer		metabuf;
	Buffer		buf;
	Page		pg;
	int32		data_width;
	int32		item_width;
	int32		ffactor;
	uint16		i;

	/* safety check */
	if (RelationGetNumberOfBlocks(rel) != 0)
		elog(ERROR, "cannot initialize non-empty hash index \"%s\"",
			 RelationGetRelationName(rel));

	/*
	 * Determine the target fill factor (tuples per bucket) for this index.
	 * The idea is to make the fill factor correspond to pages about 3/4ths
	 * full.  We can compute it exactly if the index datatype is fixed-width,
	 * but for var-width there's some guessing involved.
	 */
	data_width = get_typavgwidth(RelationGetDescr(rel)->attrs[0]->atttypid,
								 RelationGetDescr(rel)->attrs[0]->atttypmod);
	item_width = MAXALIGN(sizeof(HashItemData)) + MAXALIGN(data_width) +
		sizeof(ItemIdData);		/* include the line pointer */
	ffactor = (BLCKSZ * 3 / 4) / item_width;
	/* keep to a sane range */
	if (ffactor < 10)
		ffactor = 10;

	metabuf = _hash_getbuf(rel, HASH_METAPAGE, HASH_WRITE);
	pg = BufferGetPage(metabuf);
	_hash_pageinit(pg, BufferGetPageSize(metabuf));

	pageopaque = (HashPageOpaque) PageGetSpecialPointer(pg);
	pageopaque->hasho_prevblkno = InvalidBlockNumber;
	pageopaque->hasho_nextblkno = InvalidBlockNumber;
	pageopaque->hasho_bucket = -1;
	pageopaque->hasho_flag = LH_META_PAGE;
	pageopaque->hasho_filler = HASHO_FILL;

	metap = (HashMetaPage) pg;

	metap->hashm_magic = HASH_MAGIC;
	metap->hashm_version = HASH_VERSION;
	metap->hashm_ntuples = 0;
	metap->hashm_nmaps = 0;
	metap->hashm_ffactor = ffactor;
	metap->hashm_bsize = BufferGetPageSize(metabuf);
	/* find largest bitmap array size that will fit in page size */
	for (i = _hash_log2(metap->hashm_bsize); i > 0; --i)
	{
		if ((1 << i) <= (metap->hashm_bsize -
						 (MAXALIGN(sizeof(PageHeaderData)) +
						  MAXALIGN(sizeof(HashPageOpaqueData)))))
			break;
	}
	Assert(i > 0);
	metap->hashm_bmsize = 1 << i;
	metap->hashm_bmshift = i + BYTE_TO_BIT;
	Assert((1 << BMPG_SHIFT(metap)) == (BMPG_MASK(metap) + 1));

	metap->hashm_procid = index_getprocid(rel, 1, HASHPROC);

	/*
	 * We initialize the index with two buckets, 0 and 1, occupying physical
	 * blocks 1 and 2.  The first freespace bitmap page is in block 3.
	 */
	metap->hashm_maxbucket = metap->hashm_lowmask = 1;	/* nbuckets - 1 */
	metap->hashm_highmask = 3;	/* (nbuckets << 1) - 1 */

	MemSet((char *) metap->hashm_spares, 0, sizeof(metap->hashm_spares));
	MemSet((char *) metap->hashm_mapp, 0, sizeof(metap->hashm_mapp));

	metap->hashm_spares[1] = 1;	/* the first bitmap page is only spare */
	metap->hashm_ovflpoint = 1;
	metap->hashm_firstfree = 0;

	/*
	 * Initialize the first two buckets
	 */
	for (i = 0; i <= 1; i++)
	{
		buf = _hash_getbuf(rel, BUCKET_TO_BLKNO(metap, i), HASH_WRITE);
		pg = BufferGetPage(buf);
		_hash_pageinit(pg, BufferGetPageSize(buf));
		pageopaque = (HashPageOpaque) PageGetSpecialPointer(pg);
		pageopaque->hasho_prevblkno = InvalidBlockNumber;
		pageopaque->hasho_nextblkno = InvalidBlockNumber;
		pageopaque->hasho_bucket = i;
		pageopaque->hasho_flag = LH_BUCKET_PAGE;
		pageopaque->hasho_filler = HASHO_FILL;
		_hash_wrtbuf(rel, buf);
	}

	/*
	 * Initialize first bitmap page.  Can't do this until we
	 * create the first two buckets, else smgr will complain.
	 */
	_hash_initbitmap(rel, metap, 3);

	/* all done */
	_hash_wrtbuf(rel, metabuf);
}

/*
 *	_hash_pageinit() -- Initialize a new hash index page.
 */
void
_hash_pageinit(Page page, Size size)
{
	Assert(PageIsNew(page));
	PageInit(page, size, sizeof(HashPageOpaqueData));
}

/*
 * Attempt to expand the hash table by creating one new bucket.
 *
 * This will silently do nothing if it cannot get the needed locks.
 *
 * The caller should hold no locks on the hash index.
 *
 * The caller must hold a pin, but no lock, on the metapage buffer.
 * The buffer is returned in the same state.
 */
void
_hash_expandtable(Relation rel, Buffer metabuf)
{
	HashMetaPage metap;
	Bucket		old_bucket;
	Bucket		new_bucket;
	uint32		spare_ndx;
	BlockNumber start_oblkno;
	BlockNumber start_nblkno;
	uint32		maxbucket;
	uint32		highmask;
	uint32		lowmask;

	/*
	 * Obtain the page-zero lock to assert the right to begin a split
	 * (see README).
	 *
	 * Note: deadlock should be impossible here. Our own backend could only
	 * be holding bucket sharelocks due to stopped indexscans; those will not
	 * block other holders of the page-zero lock, who are only interested in
	 * acquiring bucket sharelocks themselves.  Exclusive bucket locks are
	 * only taken here and in hashbulkdelete, and neither of these operations
	 * needs any additional locks to complete.  (If, due to some flaw in this
	 * reasoning, we manage to deadlock anyway, it's okay to error out; the
	 * index will be left in a consistent state.)
	 */
	_hash_getlock(rel, 0, HASH_EXCLUSIVE);

	/* Write-lock the meta page */
	_hash_chgbufaccess(rel, metabuf, HASH_NOLOCK, HASH_WRITE);

	metap = (HashMetaPage) BufferGetPage(metabuf);
	_hash_checkpage(rel, (Page) metap, LH_META_PAGE);

	/*
	 * Check to see if split is still needed; someone else might have already
	 * done one while we waited for the lock.
	 *
	 * Make sure this stays in sync with_hash_doinsert()
	 */
	if (metap->hashm_ntuples <=
		(double) metap->hashm_ffactor * (metap->hashm_maxbucket + 1))
		goto fail;

	/*
	 * Determine which bucket is to be split, and attempt to lock the old
	 * bucket.  If we can't get the lock, give up.
	 *
	 * The lock protects us against other backends, but not against our own
	 * backend.  Must check for active scans separately.
	 *
	 * Ideally we would lock the new bucket too before proceeding, but if
	 * we are about to cross a splitpoint then the BUCKET_TO_BLKNO mapping
	 * isn't correct yet.  For simplicity we update the metapage first and
	 * then lock.  This should be okay because no one else should be trying
	 * to lock the new bucket yet...
	 */
	new_bucket = metap->hashm_maxbucket + 1;
	old_bucket = (new_bucket & metap->hashm_lowmask);

	start_oblkno = BUCKET_TO_BLKNO(metap, old_bucket);

	if (_hash_has_active_scan(rel, old_bucket))
		goto fail;

	if (!_hash_try_getlock(rel, start_oblkno, HASH_EXCLUSIVE))
		goto fail;

	/*
	 * Okay to proceed with split.  Update the metapage bucket mapping info.
	 */
	metap->hashm_maxbucket = new_bucket;

	if (new_bucket > metap->hashm_highmask)
	{
		/* Starting a new doubling */
		metap->hashm_lowmask = metap->hashm_highmask;
		metap->hashm_highmask = new_bucket | metap->hashm_lowmask;
	}

	/*
	 * If the split point is increasing (hashm_maxbucket's log base 2
	 * increases), we need to adjust the hashm_spares[] array and
	 * hashm_ovflpoint so that future overflow pages will be created beyond
	 * this new batch of bucket pages.
	 *
	 * XXX should initialize new bucket pages to prevent out-of-order
	 * page creation?  Don't wanna do it right here though.
	 */
	spare_ndx = _hash_log2(metap->hashm_maxbucket + 1);
	if (spare_ndx > metap->hashm_ovflpoint)
	{
		Assert(spare_ndx == metap->hashm_ovflpoint + 1);
		metap->hashm_spares[spare_ndx] = metap->hashm_spares[metap->hashm_ovflpoint];
		metap->hashm_ovflpoint = spare_ndx;
	}

	/* now we can compute the new bucket's primary block number */
	start_nblkno = BUCKET_TO_BLKNO(metap, new_bucket);

	Assert(!_hash_has_active_scan(rel, new_bucket));

	if (!_hash_try_getlock(rel, start_nblkno, HASH_EXCLUSIVE))
		elog(PANIC, "could not get lock on supposedly new bucket");

	/*
	 * Copy bucket mapping info now; this saves re-accessing the meta page
	 * inside _hash_splitbucket's inner loop.  Note that once we drop the
	 * split lock, other splits could begin, so these values might be out of
	 * date before _hash_splitbucket finishes.  That's okay, since all it
	 * needs is to tell which of these two buckets to map hashkeys into.
	 */
	maxbucket = metap->hashm_maxbucket;
	highmask = metap->hashm_highmask;
	lowmask = metap->hashm_lowmask;

	/* Write out the metapage and drop lock, but keep pin */
	_hash_chgbufaccess(rel, metabuf, HASH_WRITE, HASH_NOLOCK);

	/* Release split lock; okay for other splits to occur now */
	_hash_droplock(rel, 0, HASH_EXCLUSIVE);

	/* Relocate records to the new bucket */
	_hash_splitbucket(rel, metabuf, old_bucket, new_bucket,
					  start_oblkno, start_nblkno,
					  maxbucket, highmask, lowmask);

	/* Release bucket locks, allowing others to access them */
	_hash_droplock(rel, start_oblkno, HASH_EXCLUSIVE);
	_hash_droplock(rel, start_nblkno, HASH_EXCLUSIVE);

	return;

	/* Here if decide not to split or fail to acquire old bucket lock */
fail:

	/* We didn't write the metapage, so just drop lock */
	_hash_chgbufaccess(rel, metabuf, HASH_READ, HASH_NOLOCK);

	/* Release split lock */
	_hash_droplock(rel, 0, HASH_EXCLUSIVE);
}


/*
 * _hash_splitbucket -- split 'obucket' into 'obucket' and 'nbucket'
 *
 * We are splitting a bucket that consists of a base bucket page and zero
 * or more overflow (bucket chain) pages.  We must relocate tuples that
 * belong in the new bucket, and compress out any free space in the old
 * bucket.
 *
 * The caller must hold exclusive locks on both buckets to ensure that
 * no one else is trying to access them (see README).
 *
 * The caller must hold a pin, but no lock, on the metapage buffer.
 * The buffer is returned in the same state.  (The metapage is only
 * touched if it becomes necessary to add or remove overflow pages.)
 */
static void
_hash_splitbucket(Relation rel,
				  Buffer metabuf,
				  Bucket obucket,
				  Bucket nbucket,
				  BlockNumber start_oblkno,
				  BlockNumber start_nblkno,
				  uint32 maxbucket,
				  uint32 highmask,
				  uint32 lowmask)
{
	Bucket		bucket;
	Buffer		obuf;
	Buffer		nbuf;
	BlockNumber oblkno;
	BlockNumber nblkno;
	bool		null;
	Datum		datum;
	HashItem	hitem;
	HashPageOpaque oopaque;
	HashPageOpaque nopaque;
	IndexTuple	itup;
	Size		itemsz;
	OffsetNumber ooffnum;
	OffsetNumber noffnum;
	OffsetNumber omaxoffnum;
	Page		opage;
	Page		npage;
	TupleDesc	itupdesc = RelationGetDescr(rel);

	/*
	 * It should be okay to simultaneously write-lock pages from each
	 * bucket, since no one else can be trying to acquire buffer lock
	 * on pages of either bucket.
	 */
	oblkno = start_oblkno;
	nblkno = start_nblkno;
	obuf = _hash_getbuf(rel, oblkno, HASH_WRITE);
	nbuf = _hash_getbuf(rel, nblkno, HASH_WRITE);
	opage = BufferGetPage(obuf);
	npage = BufferGetPage(nbuf);

	_hash_checkpage(rel, opage, LH_BUCKET_PAGE);
	oopaque = (HashPageOpaque) PageGetSpecialPointer(opage);

	/* initialize the new bucket's primary page */
	_hash_pageinit(npage, BufferGetPageSize(nbuf));
	nopaque = (HashPageOpaque) PageGetSpecialPointer(npage);
	nopaque->hasho_prevblkno = InvalidBlockNumber;
	nopaque->hasho_nextblkno = InvalidBlockNumber;
	nopaque->hasho_bucket = nbucket;
	nopaque->hasho_flag = LH_BUCKET_PAGE;
	nopaque->hasho_filler = HASHO_FILL;

	/*
	 * Partition the tuples in the old bucket between the old bucket and the
	 * new bucket, advancing along the old bucket's overflow bucket chain
	 * and adding overflow pages to the new bucket as needed.
	 */
	ooffnum = FirstOffsetNumber;
	omaxoffnum = PageGetMaxOffsetNumber(opage);
	for (;;)
	{
		/*
		 * at each iteration through this loop, each of these variables
		 * should be up-to-date: obuf opage oopaque ooffnum omaxoffnum
		 */

		/* check if we're at the end of the page */
		if (ooffnum > omaxoffnum)
		{
			/* at end of page, but check for an(other) overflow page */
			oblkno = oopaque->hasho_nextblkno;
			if (!BlockNumberIsValid(oblkno))
				break;
			/*
			 * we ran out of tuples on this particular page, but we
			 * have more overflow pages; advance to next page.
			 */
			_hash_wrtbuf(rel, obuf);

			obuf = _hash_getbuf(rel, oblkno, HASH_WRITE);
			opage = BufferGetPage(obuf);
			_hash_checkpage(rel, opage, LH_OVERFLOW_PAGE);
			oopaque = (HashPageOpaque) PageGetSpecialPointer(opage);
			ooffnum = FirstOffsetNumber;
			omaxoffnum = PageGetMaxOffsetNumber(opage);
			continue;
		}

		/*
		 * Re-hash the tuple to determine which bucket it now belongs in.
		 *
		 * It is annoying to call the hash function while holding locks,
		 * but releasing and relocking the page for each tuple is unappealing
		 * too.
		 */
		hitem = (HashItem) PageGetItem(opage, PageGetItemId(opage, ooffnum));
		itup = &(hitem->hash_itup);
		datum = index_getattr(itup, 1, itupdesc, &null);
		Assert(!null);

		bucket = _hash_hashkey2bucket(_hash_datum2hashkey(rel, datum),
									  maxbucket, highmask, lowmask);

		if (bucket == nbucket)
		{
			/*
			 * insert the tuple into the new bucket.  if it doesn't fit on
			 * the current page in the new bucket, we must allocate a new
			 * overflow page and place the tuple on that page instead.
			 */
			itemsz = IndexTupleDSize(hitem->hash_itup)
				+ (sizeof(HashItemData) - sizeof(IndexTupleData));

			itemsz = MAXALIGN(itemsz);

			if (PageGetFreeSpace(npage) < itemsz)
			{
				/* write out nbuf and drop lock, but keep pin */
				_hash_chgbufaccess(rel, nbuf, HASH_WRITE, HASH_NOLOCK);
				/* chain to a new overflow page */
				nbuf = _hash_addovflpage(rel, metabuf, nbuf);
				npage = BufferGetPage(nbuf);
				_hash_checkpage(rel, npage, LH_OVERFLOW_PAGE);
				/* we don't need nopaque within the loop */
			}

			noffnum = OffsetNumberNext(PageGetMaxOffsetNumber(npage));
			if (PageAddItem(npage, (Item) hitem, itemsz, noffnum, LP_USED)
				== InvalidOffsetNumber)
				elog(ERROR, "failed to add index item to \"%s\"",
					 RelationGetRelationName(rel));

			/*
			 * now delete the tuple from the old bucket.  after this
			 * section of code, 'ooffnum' will actually point to the
			 * ItemId to which we would point if we had advanced it before
			 * the deletion (PageIndexTupleDelete repacks the ItemId
			 * array).	this also means that 'omaxoffnum' is exactly one
			 * less than it used to be, so we really can just decrement it
			 * instead of calling PageGetMaxOffsetNumber.
			 */
			PageIndexTupleDelete(opage, ooffnum);
			omaxoffnum = OffsetNumberPrev(omaxoffnum);
		}
		else
		{
			/*
			 * the tuple stays on this page.  we didn't move anything, so
			 * we didn't delete anything and therefore we don't have to
			 * change 'omaxoffnum'.
			 */
			Assert(bucket == obucket);
			ooffnum = OffsetNumberNext(ooffnum);
		}
	}

	/*
	 * We're at the end of the old bucket chain, so we're done partitioning
	 * the tuples.  Before quitting, call _hash_squeezebucket to ensure the
	 * tuples remaining in the old bucket (including the overflow pages) are
	 * packed as tightly as possible.  The new bucket is already tight.
	 */
	_hash_wrtbuf(rel, obuf);
	_hash_wrtbuf(rel, nbuf);

	_hash_squeezebucket(rel, obucket, start_oblkno);
}
