/*-------------------------------------------------------------------------
 *
 * hashpage.c
 *	  Hash table page management code for the Postgres hash access method
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/hash/hashpage.c,v 1.72 2008/01/01 19:45:46 momjian Exp $
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
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "utils/lsyscache.h"


static bool _hash_alloc_buckets(Relation rel, BlockNumber firstblock,
					uint32 nblocks);
static void _hash_splitbucket(Relation rel, Buffer metabuf,
				  Bucket obucket, Bucket nbucket,
				  BlockNumber start_oblkno,
				  BlockNumber start_nblkno,
				  uint32 maxbucket,
				  uint32 highmask, uint32 lowmask);


/*
 * We use high-concurrency locking on hash indexes (see README for an overview
 * of the locking rules).  However, we can skip taking lmgr locks when the
 * index is local to the current backend (ie, either temp or new in the
 * current transaction).  No one else can see it, so there's no reason to
 * take locks.	We still take buffer-level locks, but not lmgr locks.
 */
#define USELOCKING(rel)		(!RELATION_IS_LOCAL(rel))


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
 *		'flags' is a bitwise OR of the allowed page types.
 *
 *		This must be used only to fetch pages that are expected to be valid
 *		already.  _hash_checkpage() is applied using the given flags.
 *
 *		When this routine returns, the appropriate lock is set on the
 *		requested buffer and its reference count has been incremented
 *		(ie, the buffer is "locked and pinned").
 *
 *		P_NEW is disallowed because this routine can only be used
 *		to access pages that are known to be before the filesystem EOF.
 *		Extending the index should be done with _hash_getnewbuf.
 */
Buffer
_hash_getbuf(Relation rel, BlockNumber blkno, int access, int flags)
{
	Buffer		buf;

	if (blkno == P_NEW)
		elog(ERROR, "hash AM does not use P_NEW");

	buf = ReadBuffer(rel, blkno);

	if (access != HASH_NOLOCK)
		LockBuffer(buf, access);

	/* ref count and lock type are correct */

	_hash_checkpage(rel, buf, flags);

	return buf;
}

/*
 *	_hash_getinitbuf() -- Get and initialize a buffer by block number.
 *
 *		This must be used only to fetch pages that are known to be before
 *		the index's filesystem EOF, but are to be filled from scratch.
 *		_hash_pageinit() is applied automatically.	Otherwise it has
 *		effects similar to _hash_getbuf() with access = HASH_WRITE.
 *
 *		When this routine returns, a write lock is set on the
 *		requested buffer and its reference count has been incremented
 *		(ie, the buffer is "locked and pinned").
 *
 *		P_NEW is disallowed because this routine can only be used
 *		to access pages that are known to be before the filesystem EOF.
 *		Extending the index should be done with _hash_getnewbuf.
 */
Buffer
_hash_getinitbuf(Relation rel, BlockNumber blkno)
{
	Buffer		buf;

	if (blkno == P_NEW)
		elog(ERROR, "hash AM does not use P_NEW");

	buf = ReadOrZeroBuffer(rel, blkno);

	LockBuffer(buf, HASH_WRITE);

	/* ref count and lock type are correct */

	/* initialize the page */
	_hash_pageinit(BufferGetPage(buf), BufferGetPageSize(buf));

	return buf;
}

/*
 *	_hash_getnewbuf() -- Get a new page at the end of the index.
 *
 *		This has the same API as _hash_getinitbuf, except that we are adding
 *		a page to the index, and hence expect the page to be past the
 *		logical EOF.  (However, we have to support the case where it isn't,
 *		since a prior try might have crashed after extending the filesystem
 *		EOF but before updating the metapage to reflect the added page.)
 *
 *		It is caller's responsibility to ensure that only one process can
 *		extend the index at a time.
 */
Buffer
_hash_getnewbuf(Relation rel, BlockNumber blkno)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);
	Buffer		buf;

	if (blkno == P_NEW)
		elog(ERROR, "hash AM does not use P_NEW");
	if (blkno > nblocks)
		elog(ERROR, "access to noncontiguous page in hash index \"%s\"",
			 RelationGetRelationName(rel));

	/* smgr insists we use P_NEW to extend the relation */
	if (blkno == nblocks)
	{
		buf = ReadBuffer(rel, P_NEW);
		if (BufferGetBlockNumber(buf) != blkno)
			elog(ERROR, "unexpected hash relation size: %u, should be %u",
				 BufferGetBlockNumber(buf), blkno);
	}
	else
		buf = ReadOrZeroBuffer(rel, blkno);

	LockBuffer(buf, HASH_WRITE);

	/* ref count and lock type are correct */

	/* initialize the page */
	_hash_pageinit(BufferGetPage(buf), BufferGetPageSize(buf));

	return buf;
}

/*
 *	_hash_getbuf_with_strategy() -- Get a buffer with nondefault strategy.
 *
 *		This is identical to _hash_getbuf() but also allows a buffer access
 *		strategy to be specified.  We use this for VACUUM operations.
 */
Buffer
_hash_getbuf_with_strategy(Relation rel, BlockNumber blkno,
						   int access, int flags,
						   BufferAccessStrategy bstrategy)
{
	Buffer		buf;

	if (blkno == P_NEW)
		elog(ERROR, "hash AM does not use P_NEW");

	buf = ReadBufferWithStrategy(rel, blkno, bstrategy);

	if (access != HASH_NOLOCK)
		LockBuffer(buf, access);

	/* ref count and lock type are correct */

	_hash_checkpage(rel, buf, flags);

	return buf;
}

/*
 *	_hash_relbuf() -- release a locked buffer.
 *
 * Lock and pin (refcount) are both dropped.
 */
void
_hash_relbuf(Relation rel, Buffer buf)
{
	UnlockReleaseBuffer(buf);
}

/*
 *	_hash_dropbuf() -- release an unlocked buffer.
 *
 * This is used to unpin a buffer on which we hold no lock.
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
 * NOTE: this routine should go away when/if hash indexes are WAL-ified.
 * The correct sequence of operations is to mark the buffer dirty, then
 * write the WAL record, then release the lock and pin; so marking dirty
 * can't be combined with releasing.
 */
void
_hash_wrtbuf(Relation rel, Buffer buf)
{
	MarkBufferDirty(buf);
	UnlockReleaseBuffer(buf);
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
	if (from_access == HASH_WRITE)
		MarkBufferDirty(buf);
	if (from_access != HASH_NOLOCK)
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
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
	 * Determine the target fill factor (in tuples per bucket) for this index.
	 * The idea is to make the fill factor correspond to pages about as full
	 * as the user-settable fillfactor parameter says.	We can compute it
	 * exactly if the index datatype is fixed-width, but for var-width there's
	 * some guessing involved.
	 */
	data_width = get_typavgwidth(RelationGetDescr(rel)->attrs[0]->atttypid,
								 RelationGetDescr(rel)->attrs[0]->atttypmod);
	item_width = MAXALIGN(sizeof(IndexTupleData)) + MAXALIGN(data_width) +
		sizeof(ItemIdData);		/* include the line pointer */
	ffactor = RelationGetTargetPageUsage(rel, HASH_DEFAULT_FILLFACTOR) / item_width;
	/* keep to a sane range */
	if (ffactor < 10)
		ffactor = 10;

	/*
	 * We initialize the metapage, the first two bucket pages, and the first
	 * bitmap page in sequence, using _hash_getnewbuf to cause smgrextend()
	 * calls to occur.	This ensures that the smgr level has the right idea of
	 * the physical index length.
	 */
	metabuf = _hash_getnewbuf(rel, HASH_METAPAGE);
	pg = BufferGetPage(metabuf);

	pageopaque = (HashPageOpaque) PageGetSpecialPointer(pg);
	pageopaque->hasho_prevblkno = InvalidBlockNumber;
	pageopaque->hasho_nextblkno = InvalidBlockNumber;
	pageopaque->hasho_bucket = -1;
	pageopaque->hasho_flag = LH_META_PAGE;
	pageopaque->hasho_page_id = HASHO_PAGE_ID;

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

	/*
	 * Label the index with its primary hash support function's OID.  This is
	 * pretty useless for normal operation (in fact, hashm_procid is not used
	 * anywhere), but it might be handy for forensic purposes so we keep it.
	 */
	metap->hashm_procid = index_getprocid(rel, 1, HASHPROC);

	/*
	 * We initialize the index with two buckets, 0 and 1, occupying physical
	 * blocks 1 and 2.	The first freespace bitmap page is in block 3.
	 */
	metap->hashm_maxbucket = metap->hashm_lowmask = 1;	/* nbuckets - 1 */
	metap->hashm_highmask = 3;	/* (nbuckets << 1) - 1 */

	MemSet(metap->hashm_spares, 0, sizeof(metap->hashm_spares));
	MemSet(metap->hashm_mapp, 0, sizeof(metap->hashm_mapp));

	metap->hashm_spares[1] = 1; /* the first bitmap page is only spare */
	metap->hashm_ovflpoint = 1;
	metap->hashm_firstfree = 0;

	/*
	 * Initialize the first two buckets
	 */
	for (i = 0; i <= 1; i++)
	{
		buf = _hash_getnewbuf(rel, BUCKET_TO_BLKNO(metap, i));
		pg = BufferGetPage(buf);
		pageopaque = (HashPageOpaque) PageGetSpecialPointer(pg);
		pageopaque->hasho_prevblkno = InvalidBlockNumber;
		pageopaque->hasho_nextblkno = InvalidBlockNumber;
		pageopaque->hasho_bucket = i;
		pageopaque->hasho_flag = LH_BUCKET_PAGE;
		pageopaque->hasho_page_id = HASHO_PAGE_ID;
		_hash_wrtbuf(rel, buf);
	}

	/*
	 * Initialize first bitmap page
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
	 * Obtain the page-zero lock to assert the right to begin a split (see
	 * README).
	 *
	 * Note: deadlock should be impossible here. Our own backend could only be
	 * holding bucket sharelocks due to stopped indexscans; those will not
	 * block other holders of the page-zero lock, who are only interested in
	 * acquiring bucket sharelocks themselves.	Exclusive bucket locks are
	 * only taken here and in hashbulkdelete, and neither of these operations
	 * needs any additional locks to complete.	(If, due to some flaw in this
	 * reasoning, we manage to deadlock anyway, it's okay to error out; the
	 * index will be left in a consistent state.)
	 */
	_hash_getlock(rel, 0, HASH_EXCLUSIVE);

	/* Write-lock the meta page */
	_hash_chgbufaccess(rel, metabuf, HASH_NOLOCK, HASH_WRITE);

	_hash_checkpage(rel, metabuf, LH_META_PAGE);
	metap = (HashMetaPage) BufferGetPage(metabuf);

	/*
	 * Check to see if split is still needed; someone else might have already
	 * done one while we waited for the lock.
	 *
	 * Make sure this stays in sync with _hash_doinsert()
	 */
	if (metap->hashm_ntuples <=
		(double) metap->hashm_ffactor * (metap->hashm_maxbucket + 1))
		goto fail;

	/*
	 * Can't split anymore if maxbucket has reached its maximum possible
	 * value.
	 *
	 * Ideally we'd allow bucket numbers up to UINT_MAX-1 (no higher because
	 * the calculation maxbucket+1 mustn't overflow).  Currently we restrict
	 * to half that because of overflow looping in _hash_log2() and
	 * insufficient space in hashm_spares[].  It's moot anyway because an
	 * index with 2^32 buckets would certainly overflow BlockNumber and hence
	 * _hash_alloc_buckets() would fail, but if we supported buckets smaller
	 * than a disk block then this would be an independent constraint.
	 */
	if (metap->hashm_maxbucket >= (uint32) 0x7FFFFFFE)
		goto fail;

	/*
	 * Determine which bucket is to be split, and attempt to lock the old
	 * bucket.	If we can't get the lock, give up.
	 *
	 * The lock protects us against other backends, but not against our own
	 * backend.  Must check for active scans separately.
	 */
	new_bucket = metap->hashm_maxbucket + 1;

	old_bucket = (new_bucket & metap->hashm_lowmask);

	start_oblkno = BUCKET_TO_BLKNO(metap, old_bucket);

	if (_hash_has_active_scan(rel, old_bucket))
		goto fail;

	if (!_hash_try_getlock(rel, start_oblkno, HASH_EXCLUSIVE))
		goto fail;

	/*
	 * Likewise lock the new bucket (should never fail).
	 *
	 * Note: it is safe to compute the new bucket's blkno here, even though we
	 * may still need to update the BUCKET_TO_BLKNO mapping.  This is because
	 * the current value of hashm_spares[hashm_ovflpoint] correctly shows
	 * where we are going to put a new splitpoint's worth of buckets.
	 */
	start_nblkno = BUCKET_TO_BLKNO(metap, new_bucket);

	if (_hash_has_active_scan(rel, new_bucket))
		elog(ERROR, "scan in progress on supposedly new bucket");

	if (!_hash_try_getlock(rel, start_nblkno, HASH_EXCLUSIVE))
		elog(ERROR, "could not get lock on supposedly new bucket");

	/*
	 * If the split point is increasing (hashm_maxbucket's log base 2
	 * increases), we need to allocate a new batch of bucket pages.
	 */
	spare_ndx = _hash_log2(new_bucket + 1);
	if (spare_ndx > metap->hashm_ovflpoint)
	{
		Assert(spare_ndx == metap->hashm_ovflpoint + 1);

		/*
		 * The number of buckets in the new splitpoint is equal to the total
		 * number already in existence, i.e. new_bucket.  Currently this maps
		 * one-to-one to blocks required, but someday we may need a more
		 * complicated calculation here.
		 */
		if (!_hash_alloc_buckets(rel, start_nblkno, new_bucket))
		{
			/* can't split due to BlockNumber overflow */
			_hash_droplock(rel, start_oblkno, HASH_EXCLUSIVE);
			_hash_droplock(rel, start_nblkno, HASH_EXCLUSIVE);
			goto fail;
		}
	}

	/*
	 * Okay to proceed with split.	Update the metapage bucket mapping info.
	 *
	 * Since we are scribbling on the metapage data right in the shared
	 * buffer, any failure in this next little bit leaves us with a big
	 * problem: the metapage is effectively corrupt but could get written back
	 * to disk.  We don't really expect any failure, but just to be sure,
	 * establish a critical section.
	 */
	START_CRIT_SECTION();

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
	 */
	if (spare_ndx > metap->hashm_ovflpoint)
	{
		metap->hashm_spares[spare_ndx] = metap->hashm_spares[metap->hashm_ovflpoint];
		metap->hashm_ovflpoint = spare_ndx;
	}

	/* Done mucking with metapage */
	END_CRIT_SECTION();

	/*
	 * Copy bucket mapping info now; this saves re-accessing the meta page
	 * inside _hash_splitbucket's inner loop.  Note that once we drop the
	 * split lock, other splits could begin, so these values might be out of
	 * date before _hash_splitbucket finishes.	That's okay, since all it
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
 * _hash_alloc_buckets -- allocate a new splitpoint's worth of bucket pages
 *
 * This does not need to initialize the new bucket pages; we'll do that as
 * each one is used by _hash_expandtable().  But we have to extend the logical
 * EOF to the end of the splitpoint; this keeps smgr's idea of the EOF in
 * sync with ours, so that we don't get complaints from smgr.
 *
 * We do this by writing a page of zeroes at the end of the splitpoint range.
 * We expect that the filesystem will ensure that the intervening pages read
 * as zeroes too.  On many filesystems this "hole" will not be allocated
 * immediately, which means that the index file may end up more fragmented
 * than if we forced it all to be allocated now; but since we don't scan
 * hash indexes sequentially anyway, that probably doesn't matter.
 *
 * XXX It's annoying that this code is executed with the metapage lock held.
 * We need to interlock against _hash_getovflpage() adding a new overflow page
 * concurrently, but it'd likely be better to use LockRelationForExtension
 * for the purpose.  OTOH, adding a splitpoint is a very infrequent operation,
 * so it may not be worth worrying about.
 *
 * Returns TRUE if successful, or FALSE if allocation failed due to
 * BlockNumber overflow.
 */
static bool
_hash_alloc_buckets(Relation rel, BlockNumber firstblock, uint32 nblocks)
{
	BlockNumber lastblock;
	char		zerobuf[BLCKSZ];

	lastblock = firstblock + nblocks - 1;

	/*
	 * Check for overflow in block number calculation; if so, we cannot extend
	 * the index anymore.
	 */
	if (lastblock < firstblock || lastblock == InvalidBlockNumber)
		return false;

	MemSet(zerobuf, 0, sizeof(zerobuf));

	RelationOpenSmgr(rel);
	smgrextend(rel->rd_smgr, lastblock, zerobuf, rel->rd_istemp);

	return true;
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
	 * It should be okay to simultaneously write-lock pages from each bucket,
	 * since no one else can be trying to acquire buffer lock on pages of
	 * either bucket.
	 */
	oblkno = start_oblkno;
	obuf = _hash_getbuf(rel, oblkno, HASH_WRITE, LH_BUCKET_PAGE);
	opage = BufferGetPage(obuf);
	oopaque = (HashPageOpaque) PageGetSpecialPointer(opage);

	nblkno = start_nblkno;
	nbuf = _hash_getnewbuf(rel, nblkno);
	npage = BufferGetPage(nbuf);

	/* initialize the new bucket's primary page */
	nopaque = (HashPageOpaque) PageGetSpecialPointer(npage);
	nopaque->hasho_prevblkno = InvalidBlockNumber;
	nopaque->hasho_nextblkno = InvalidBlockNumber;
	nopaque->hasho_bucket = nbucket;
	nopaque->hasho_flag = LH_BUCKET_PAGE;
	nopaque->hasho_page_id = HASHO_PAGE_ID;

	/*
	 * Partition the tuples in the old bucket between the old bucket and the
	 * new bucket, advancing along the old bucket's overflow bucket chain and
	 * adding overflow pages to the new bucket as needed.
	 */
	ooffnum = FirstOffsetNumber;
	omaxoffnum = PageGetMaxOffsetNumber(opage);
	for (;;)
	{
		/*
		 * at each iteration through this loop, each of these variables should
		 * be up-to-date: obuf opage oopaque ooffnum omaxoffnum
		 */

		/* check if we're at the end of the page */
		if (ooffnum > omaxoffnum)
		{
			/* at end of page, but check for an(other) overflow page */
			oblkno = oopaque->hasho_nextblkno;
			if (!BlockNumberIsValid(oblkno))
				break;

			/*
			 * we ran out of tuples on this particular page, but we have more
			 * overflow pages; advance to next page.
			 */
			_hash_wrtbuf(rel, obuf);

			obuf = _hash_getbuf(rel, oblkno, HASH_WRITE, LH_OVERFLOW_PAGE);
			opage = BufferGetPage(obuf);
			oopaque = (HashPageOpaque) PageGetSpecialPointer(opage);
			ooffnum = FirstOffsetNumber;
			omaxoffnum = PageGetMaxOffsetNumber(opage);
			continue;
		}

		/*
		 * Re-hash the tuple to determine which bucket it now belongs in.
		 *
		 * It is annoying to call the hash function while holding locks, but
		 * releasing and relocking the page for each tuple is unappealing too.
		 */
		itup = (IndexTuple) PageGetItem(opage, PageGetItemId(opage, ooffnum));
		datum = index_getattr(itup, 1, itupdesc, &null);
		Assert(!null);

		bucket = _hash_hashkey2bucket(_hash_datum2hashkey(rel, datum),
									  maxbucket, highmask, lowmask);

		if (bucket == nbucket)
		{
			/*
			 * insert the tuple into the new bucket.  if it doesn't fit on the
			 * current page in the new bucket, we must allocate a new overflow
			 * page and place the tuple on that page instead.
			 */
			itemsz = IndexTupleDSize(*itup);
			itemsz = MAXALIGN(itemsz);

			if (PageGetFreeSpace(npage) < itemsz)
			{
				/* write out nbuf and drop lock, but keep pin */
				_hash_chgbufaccess(rel, nbuf, HASH_WRITE, HASH_NOLOCK);
				/* chain to a new overflow page */
				nbuf = _hash_addovflpage(rel, metabuf, nbuf);
				npage = BufferGetPage(nbuf);
				/* we don't need nopaque within the loop */
			}

			noffnum = OffsetNumberNext(PageGetMaxOffsetNumber(npage));
			if (PageAddItem(npage, (Item) itup, itemsz, noffnum, false, false)
				== InvalidOffsetNumber)
				elog(ERROR, "failed to add index item to \"%s\"",
					 RelationGetRelationName(rel));

			/*
			 * now delete the tuple from the old bucket.  after this section
			 * of code, 'ooffnum' will actually point to the ItemId to which
			 * we would point if we had advanced it before the deletion
			 * (PageIndexTupleDelete repacks the ItemId array).  this also
			 * means that 'omaxoffnum' is exactly one less than it used to be,
			 * so we really can just decrement it instead of calling
			 * PageGetMaxOffsetNumber.
			 */
			PageIndexTupleDelete(opage, ooffnum);
			omaxoffnum = OffsetNumberPrev(omaxoffnum);
		}
		else
		{
			/*
			 * the tuple stays on this page.  we didn't move anything, so we
			 * didn't delete anything and therefore we don't have to change
			 * 'omaxoffnum'.
			 */
			Assert(bucket == obucket);
			ooffnum = OffsetNumberNext(ooffnum);
		}
	}

	/*
	 * We're at the end of the old bucket chain, so we're done partitioning
	 * the tuples.	Before quitting, call _hash_squeezebucket to ensure the
	 * tuples remaining in the old bucket (including the overflow pages) are
	 * packed as tightly as possible.  The new bucket is already tight.
	 */
	_hash_wrtbuf(rel, obuf);
	_hash_wrtbuf(rel, nbuf);

	_hash_squeezebucket(rel, obucket, start_oblkno, NULL);
}
