/*-------------------------------------------------------------------------
 *
 * hashpage.c
 *	  Hash table page management code for the Postgres hash access method
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/hash/hashpage.c
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

#include "access/hash.h"
#include "access/hash_xlog.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "storage/predicate.h"
#include "storage/smgr.h"
#include "utils/rel.h"

static bool _hash_alloc_buckets(Relation rel, BlockNumber firstblock,
								uint32 nblocks);
static void _hash_splitbucket(Relation rel, Buffer metabuf,
							  Bucket obucket, Bucket nbucket,
							  Buffer obuf,
							  Buffer nbuf,
							  HTAB *htab,
							  uint32 maxbucket,
							  uint32 highmask, uint32 lowmask);
static void log_split_page(Relation rel, Buffer buf);


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
 * _hash_getbuf_with_condlock_cleanup() -- Try to get a buffer for cleanup.
 *
 *		We read the page and try to acquire a cleanup lock.  If we get it,
 *		we return the buffer; otherwise, we return InvalidBuffer.
 */
Buffer
_hash_getbuf_with_condlock_cleanup(Relation rel, BlockNumber blkno, int flags)
{
	Buffer		buf;

	if (blkno == P_NEW)
		elog(ERROR, "hash AM does not use P_NEW");

	buf = ReadBuffer(rel, blkno);

	if (!ConditionalLockBufferForCleanup(buf))
	{
		ReleaseBuffer(buf);
		return InvalidBuffer;
	}

	/* ref count and lock type are correct */

	_hash_checkpage(rel, buf, flags);

	return buf;
}

/*
 *	_hash_getinitbuf() -- Get and initialize a buffer by block number.
 *
 *		This must be used only to fetch pages that are known to be before
 *		the index's filesystem EOF, but are to be filled from scratch.
 *		_hash_pageinit() is applied automatically.  Otherwise it has
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

	buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_ZERO_AND_LOCK,
							 NULL);

	/* ref count and lock type are correct */

	/* initialize the page */
	_hash_pageinit(BufferGetPage(buf), BufferGetPageSize(buf));

	return buf;
}

/*
 *	_hash_initbuf() -- Get and initialize a buffer by bucket number.
 */
void
_hash_initbuf(Buffer buf, uint32 max_bucket, uint32 num_bucket, uint32 flag,
			  bool initpage)
{
	HashPageOpaque pageopaque;
	Page		page;

	page = BufferGetPage(buf);

	/* initialize the page */
	if (initpage)
		_hash_pageinit(page, BufferGetPageSize(buf));

	pageopaque = HashPageGetOpaque(page);

	/*
	 * Set hasho_prevblkno with current hashm_maxbucket. This value will be
	 * used to validate cached HashMetaPageData. See
	 * _hash_getbucketbuf_from_hashkey().
	 */
	pageopaque->hasho_prevblkno = max_bucket;
	pageopaque->hasho_nextblkno = InvalidBlockNumber;
	pageopaque->hasho_bucket = num_bucket;
	pageopaque->hasho_flag = flag;
	pageopaque->hasho_page_id = HASHO_PAGE_ID;
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
 *		extend the index at a time.  In practice, this function is called
 *		only while holding write lock on the metapage, because adding a page
 *		is always associated with an update of metapage data.
 */
Buffer
_hash_getnewbuf(Relation rel, BlockNumber blkno, ForkNumber forkNum)
{
	BlockNumber nblocks = RelationGetNumberOfBlocksInFork(rel, forkNum);
	Buffer		buf;

	if (blkno == P_NEW)
		elog(ERROR, "hash AM does not use P_NEW");
	if (blkno > nblocks)
		elog(ERROR, "access to noncontiguous page in hash index \"%s\"",
			 RelationGetRelationName(rel));

	/* smgr insists we explicitly extend the relation */
	if (blkno == nblocks)
	{
		buf = ExtendBufferedRel(BMR_REL(rel), forkNum, NULL,
								EB_LOCK_FIRST | EB_SKIP_EXTENSION_LOCK);
		if (BufferGetBlockNumber(buf) != blkno)
			elog(ERROR, "unexpected hash relation size: %u, should be %u",
				 BufferGetBlockNumber(buf), blkno);
	}
	else
	{
		buf = ReadBufferExtended(rel, forkNum, blkno, RBM_ZERO_AND_LOCK,
								 NULL);
	}

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

	buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL, bstrategy);

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
 *	_hash_dropscanbuf() -- release buffers used in scan.
 *
 * This routine unpins the buffers used during scan on which we
 * hold no lock.
 */
void
_hash_dropscanbuf(Relation rel, HashScanOpaque so)
{
	/* release pin we hold on primary bucket page */
	if (BufferIsValid(so->hashso_bucket_buf) &&
		so->hashso_bucket_buf != so->currPos.buf)
		_hash_dropbuf(rel, so->hashso_bucket_buf);
	so->hashso_bucket_buf = InvalidBuffer;

	/* release pin we hold on primary bucket page  of bucket being split */
	if (BufferIsValid(so->hashso_split_bucket_buf) &&
		so->hashso_split_bucket_buf != so->currPos.buf)
		_hash_dropbuf(rel, so->hashso_split_bucket_buf);
	so->hashso_split_bucket_buf = InvalidBuffer;

	/* release any pin we still hold */
	if (BufferIsValid(so->currPos.buf))
		_hash_dropbuf(rel, so->currPos.buf);
	so->currPos.buf = InvalidBuffer;

	/* reset split scan */
	so->hashso_buc_populated = false;
	so->hashso_buc_split = false;
}


/*
 *	_hash_init() -- Initialize the metadata page of a hash index,
 *				the initial buckets, and the initial bitmap page.
 *
 * The initial number of buckets is dependent on num_tuples, an estimate
 * of the number of tuples to be loaded into the index initially.  The
 * chosen number of buckets is returned.
 *
 * We are fairly cavalier about locking here, since we know that no one else
 * could be accessing this index.  In particular the rule about not holding
 * multiple buffer locks is ignored.
 */
uint32
_hash_init(Relation rel, double num_tuples, ForkNumber forkNum)
{
	Buffer		metabuf;
	Buffer		buf;
	Buffer		bitmapbuf;
	Page		pg;
	HashMetaPage metap;
	RegProcedure procid;
	int32		data_width;
	int32		item_width;
	int32		ffactor;
	uint32		num_buckets;
	uint32		i;
	bool		use_wal;

	/* safety check */
	if (RelationGetNumberOfBlocksInFork(rel, forkNum) != 0)
		elog(ERROR, "cannot initialize non-empty hash index \"%s\"",
			 RelationGetRelationName(rel));

	/*
	 * WAL log creation of pages if the relation is persistent, or this is the
	 * init fork.  Init forks for unlogged relations always need to be WAL
	 * logged.
	 */
	use_wal = RelationNeedsWAL(rel) || forkNum == INIT_FORKNUM;

	/*
	 * Determine the target fill factor (in tuples per bucket) for this index.
	 * The idea is to make the fill factor correspond to pages about as full
	 * as the user-settable fillfactor parameter says.  We can compute it
	 * exactly since the index datatype (i.e. uint32 hash key) is fixed-width.
	 */
	data_width = sizeof(uint32);
	item_width = MAXALIGN(sizeof(IndexTupleData)) + MAXALIGN(data_width) +
		sizeof(ItemIdData);		/* include the line pointer */
	ffactor = HashGetTargetPageUsage(rel) / item_width;
	/* keep to a sane range */
	if (ffactor < 10)
		ffactor = 10;

	procid = index_getprocid(rel, 1, HASHSTANDARD_PROC);

	/*
	 * We initialize the metapage, the first N bucket pages, and the first
	 * bitmap page in sequence, using _hash_getnewbuf to cause smgrextend()
	 * calls to occur.  This ensures that the smgr level has the right idea of
	 * the physical index length.
	 *
	 * Critical section not required, because on error the creation of the
	 * whole relation will be rolled back.
	 */
	metabuf = _hash_getnewbuf(rel, HASH_METAPAGE, forkNum);
	_hash_init_metabuffer(metabuf, num_tuples, procid, ffactor, false);
	MarkBufferDirty(metabuf);

	pg = BufferGetPage(metabuf);
	metap = HashPageGetMeta(pg);

	/* XLOG stuff */
	if (use_wal)
	{
		xl_hash_init_meta_page xlrec;
		XLogRecPtr	recptr;

		xlrec.num_tuples = num_tuples;
		xlrec.procid = metap->hashm_procid;
		xlrec.ffactor = metap->hashm_ffactor;

		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfHashInitMetaPage);
		XLogRegisterBuffer(0, metabuf, REGBUF_WILL_INIT | REGBUF_STANDARD);

		recptr = XLogInsert(RM_HASH_ID, XLOG_HASH_INIT_META_PAGE);

		PageSetLSN(BufferGetPage(metabuf), recptr);
	}

	num_buckets = metap->hashm_maxbucket + 1;

	/*
	 * Release buffer lock on the metapage while we initialize buckets.
	 * Otherwise, we'll be in interrupt holdoff and the CHECK_FOR_INTERRUPTS
	 * won't accomplish anything.  It's a bad idea to hold buffer locks for
	 * long intervals in any case, since that can block the bgwriter.
	 */
	LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);

	/*
	 * Initialize and WAL Log the first N buckets
	 */
	for (i = 0; i < num_buckets; i++)
	{
		BlockNumber blkno;

		/* Allow interrupts, in case N is huge */
		CHECK_FOR_INTERRUPTS();

		blkno = BUCKET_TO_BLKNO(metap, i);
		buf = _hash_getnewbuf(rel, blkno, forkNum);
		_hash_initbuf(buf, metap->hashm_maxbucket, i, LH_BUCKET_PAGE, false);
		MarkBufferDirty(buf);

		if (use_wal)
			log_newpage(&rel->rd_locator,
						forkNum,
						blkno,
						BufferGetPage(buf),
						true);
		_hash_relbuf(rel, buf);
	}

	/* Now reacquire buffer lock on metapage */
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

	/*
	 * Initialize bitmap page
	 */
	bitmapbuf = _hash_getnewbuf(rel, num_buckets + 1, forkNum);
	_hash_initbitmapbuffer(bitmapbuf, metap->hashm_bmsize, false);
	MarkBufferDirty(bitmapbuf);

	/* add the new bitmap page to the metapage's list of bitmaps */
	/* metapage already has a write lock */
	if (metap->hashm_nmaps >= HASH_MAX_BITMAPS)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("out of overflow pages in hash index \"%s\"",
						RelationGetRelationName(rel))));

	metap->hashm_mapp[metap->hashm_nmaps] = num_buckets + 1;

	metap->hashm_nmaps++;
	MarkBufferDirty(metabuf);

	/* XLOG stuff */
	if (use_wal)
	{
		xl_hash_init_bitmap_page xlrec;
		XLogRecPtr	recptr;

		xlrec.bmsize = metap->hashm_bmsize;

		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfHashInitBitmapPage);
		XLogRegisterBuffer(0, bitmapbuf, REGBUF_WILL_INIT);

		/*
		 * This is safe only because nobody else can be modifying the index at
		 * this stage; it's only visible to the transaction that is creating
		 * it.
		 */
		XLogRegisterBuffer(1, metabuf, REGBUF_STANDARD);

		recptr = XLogInsert(RM_HASH_ID, XLOG_HASH_INIT_BITMAP_PAGE);

		PageSetLSN(BufferGetPage(bitmapbuf), recptr);
		PageSetLSN(BufferGetPage(metabuf), recptr);
	}

	/* all done */
	_hash_relbuf(rel, bitmapbuf);
	_hash_relbuf(rel, metabuf);

	return num_buckets;
}

/*
 *	_hash_init_metabuffer() -- Initialize the metadata page of a hash index.
 */
void
_hash_init_metabuffer(Buffer buf, double num_tuples, RegProcedure procid,
					  uint16 ffactor, bool initpage)
{
	HashMetaPage metap;
	HashPageOpaque pageopaque;
	Page		page;
	double		dnumbuckets;
	uint32		num_buckets;
	uint32		spare_index;
	uint32		lshift;

	/*
	 * Choose the number of initial bucket pages to match the fill factor
	 * given the estimated number of tuples.  We round up the result to the
	 * total number of buckets which has to be allocated before using its
	 * hashm_spares element. However always force at least 2 bucket pages. The
	 * upper limit is determined by considerations explained in
	 * _hash_expandtable().
	 */
	dnumbuckets = num_tuples / ffactor;
	if (dnumbuckets <= 2.0)
		num_buckets = 2;
	else if (dnumbuckets >= (double) 0x40000000)
		num_buckets = 0x40000000;
	else
		num_buckets = _hash_get_totalbuckets(_hash_spareindex(dnumbuckets));

	spare_index = _hash_spareindex(num_buckets);
	Assert(spare_index < HASH_MAX_SPLITPOINTS);

	page = BufferGetPage(buf);
	if (initpage)
		_hash_pageinit(page, BufferGetPageSize(buf));

	pageopaque = HashPageGetOpaque(page);
	pageopaque->hasho_prevblkno = InvalidBlockNumber;
	pageopaque->hasho_nextblkno = InvalidBlockNumber;
	pageopaque->hasho_bucket = InvalidBucket;
	pageopaque->hasho_flag = LH_META_PAGE;
	pageopaque->hasho_page_id = HASHO_PAGE_ID;

	metap = HashPageGetMeta(page);

	metap->hashm_magic = HASH_MAGIC;
	metap->hashm_version = HASH_VERSION;
	metap->hashm_ntuples = 0;
	metap->hashm_nmaps = 0;
	metap->hashm_ffactor = ffactor;
	metap->hashm_bsize = HashGetMaxBitmapSize(page);

	/* find largest bitmap array size that will fit in page size */
	lshift = pg_leftmost_one_pos32(metap->hashm_bsize);
	Assert(lshift > 0);
	metap->hashm_bmsize = 1 << lshift;
	metap->hashm_bmshift = lshift + BYTE_TO_BIT;
	Assert((1 << BMPG_SHIFT(metap)) == (BMPG_MASK(metap) + 1));

	/*
	 * Label the index with its primary hash support function's OID.  This is
	 * pretty useless for normal operation (in fact, hashm_procid is not used
	 * anywhere), but it might be handy for forensic purposes so we keep it.
	 */
	metap->hashm_procid = procid;

	/*
	 * We initialize the index with N buckets, 0 .. N-1, occupying physical
	 * blocks 1 to N.  The first freespace bitmap page is in block N+1.
	 */
	metap->hashm_maxbucket = num_buckets - 1;

	/*
	 * Set highmask as next immediate ((2 ^ x) - 1), which should be
	 * sufficient to cover num_buckets.
	 */
	metap->hashm_highmask = pg_nextpower2_32(num_buckets + 1) - 1;
	metap->hashm_lowmask = (metap->hashm_highmask >> 1);

	MemSet(metap->hashm_spares, 0, sizeof(metap->hashm_spares));
	MemSet(metap->hashm_mapp, 0, sizeof(metap->hashm_mapp));

	/* Set up mapping for one spare page after the initial splitpoints */
	metap->hashm_spares[spare_index] = 1;
	metap->hashm_ovflpoint = spare_index;
	metap->hashm_firstfree = 0;

	/*
	 * Set pd_lower just past the end of the metadata.  This is essential,
	 * because without doing so, metadata will be lost if xlog.c compresses
	 * the page.
	 */
	((PageHeader) page)->pd_lower =
		((char *) metap + sizeof(HashMetaPageData)) - (char *) page;
}

/*
 *	_hash_pageinit() -- Initialize a new hash index page.
 */
void
_hash_pageinit(Page page, Size size)
{
	PageInit(page, size, sizeof(HashPageOpaqueData));
}

/*
 * Attempt to expand the hash table by creating one new bucket.
 *
 * This will silently do nothing if we don't get cleanup lock on old or
 * new bucket.
 *
 * Complete the pending splits and remove the tuples from old bucket,
 * if there are any left over from the previous split.
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
	Buffer		buf_nblkno;
	Buffer		buf_oblkno;
	Page		opage;
	Page		npage;
	HashPageOpaque oopaque;
	HashPageOpaque nopaque;
	uint32		maxbucket;
	uint32		highmask;
	uint32		lowmask;
	bool		metap_update_masks = false;
	bool		metap_update_splitpoint = false;

restart_expand:

	/*
	 * Write-lock the meta page.  It used to be necessary to acquire a
	 * heavyweight lock to begin a split, but that is no longer required.
	 */
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

	_hash_checkpage(rel, metabuf, LH_META_PAGE);
	metap = HashPageGetMeta(BufferGetPage(metabuf));

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
	 * to half that to prevent failure of pg_ceil_log2_32() and insufficient
	 * space in hashm_spares[].  It's moot anyway because an index with 2^32
	 * buckets would certainly overflow BlockNumber and hence
	 * _hash_alloc_buckets() would fail, but if we supported buckets smaller
	 * than a disk block then this would be an independent constraint.
	 *
	 * If you change this, see also the maximum initial number of buckets in
	 * _hash_init().
	 */
	if (metap->hashm_maxbucket >= (uint32) 0x7FFFFFFE)
		goto fail;

	/*
	 * Determine which bucket is to be split, and attempt to take cleanup lock
	 * on the old bucket.  If we can't get the lock, give up.
	 *
	 * The cleanup lock protects us not only against other backends, but
	 * against our own backend as well.
	 *
	 * The cleanup lock is mainly to protect the split from concurrent
	 * inserts. See src/backend/access/hash/README, Lock Definitions for
	 * further details.  Due to this locking restriction, if there is any
	 * pending scan, the split will give up which is not good, but harmless.
	 */
	new_bucket = metap->hashm_maxbucket + 1;

	old_bucket = (new_bucket & metap->hashm_lowmask);

	start_oblkno = BUCKET_TO_BLKNO(metap, old_bucket);

	buf_oblkno = _hash_getbuf_with_condlock_cleanup(rel, start_oblkno, LH_BUCKET_PAGE);
	if (!buf_oblkno)
		goto fail;

	opage = BufferGetPage(buf_oblkno);
	oopaque = HashPageGetOpaque(opage);

	/*
	 * We want to finish the split from a bucket as there is no apparent
	 * benefit by not doing so and it will make the code complicated to finish
	 * the split that involves multiple buckets considering the case where new
	 * split also fails.  We don't need to consider the new bucket for
	 * completing the split here as it is not possible that a re-split of new
	 * bucket starts when there is still a pending split from old bucket.
	 */
	if (H_BUCKET_BEING_SPLIT(oopaque))
	{
		/*
		 * Copy bucket mapping info now; refer the comment in code below where
		 * we copy this information before calling _hash_splitbucket to see
		 * why this is okay.
		 */
		maxbucket = metap->hashm_maxbucket;
		highmask = metap->hashm_highmask;
		lowmask = metap->hashm_lowmask;

		/*
		 * Release the lock on metapage and old_bucket, before completing the
		 * split.
		 */
		LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
		LockBuffer(buf_oblkno, BUFFER_LOCK_UNLOCK);

		_hash_finish_split(rel, metabuf, buf_oblkno, old_bucket, maxbucket,
						   highmask, lowmask);

		/* release the pin on old buffer and retry for expand. */
		_hash_dropbuf(rel, buf_oblkno);

		goto restart_expand;
	}

	/*
	 * Clean the tuples remained from the previous split.  This operation
	 * requires cleanup lock and we already have one on the old bucket, so
	 * let's do it. We also don't want to allow further splits from the bucket
	 * till the garbage of previous split is cleaned.  This has two
	 * advantages; first, it helps in avoiding the bloat due to garbage and
	 * second is, during cleanup of bucket, we are always sure that the
	 * garbage tuples belong to most recently split bucket.  On the contrary,
	 * if we allow cleanup of bucket after meta page is updated to indicate
	 * the new split and before the actual split, the cleanup operation won't
	 * be able to decide whether the tuple has been moved to the newly created
	 * bucket and ended up deleting such tuples.
	 */
	if (H_NEEDS_SPLIT_CLEANUP(oopaque))
	{
		/*
		 * Copy bucket mapping info now; refer to the comment in code below
		 * where we copy this information before calling _hash_splitbucket to
		 * see why this is okay.
		 */
		maxbucket = metap->hashm_maxbucket;
		highmask = metap->hashm_highmask;
		lowmask = metap->hashm_lowmask;

		/* Release the metapage lock. */
		LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);

		hashbucketcleanup(rel, old_bucket, buf_oblkno, start_oblkno, NULL,
						  maxbucket, highmask, lowmask, NULL, NULL, true,
						  NULL, NULL);

		_hash_dropbuf(rel, buf_oblkno);

		goto restart_expand;
	}

	/*
	 * There shouldn't be any active scan on new bucket.
	 *
	 * Note: it is safe to compute the new bucket's blkno here, even though we
	 * may still need to update the BUCKET_TO_BLKNO mapping.  This is because
	 * the current value of hashm_spares[hashm_ovflpoint] correctly shows
	 * where we are going to put a new splitpoint's worth of buckets.
	 */
	start_nblkno = BUCKET_TO_BLKNO(metap, new_bucket);

	/*
	 * If the split point is increasing we need to allocate a new batch of
	 * bucket pages.
	 */
	spare_ndx = _hash_spareindex(new_bucket + 1);
	if (spare_ndx > metap->hashm_ovflpoint)
	{
		uint32		buckets_to_add;

		Assert(spare_ndx == metap->hashm_ovflpoint + 1);

		/*
		 * We treat allocation of buckets as a separate WAL-logged action.
		 * Even if we fail after this operation, won't leak bucket pages;
		 * rather, the next split will consume this space. In any case, even
		 * without failure we don't use all the space in one split operation.
		 */
		buckets_to_add = _hash_get_totalbuckets(spare_ndx) - new_bucket;
		if (!_hash_alloc_buckets(rel, start_nblkno, buckets_to_add))
		{
			/* can't split due to BlockNumber overflow */
			_hash_relbuf(rel, buf_oblkno);
			goto fail;
		}
	}

	/*
	 * Physically allocate the new bucket's primary page.  We want to do this
	 * before changing the metapage's mapping info, in case we can't get the
	 * disk space.
	 *
	 * XXX It doesn't make sense to call _hash_getnewbuf first, zeroing the
	 * buffer, and then only afterwards check whether we have a cleanup lock.
	 * However, since no scan can be accessing the buffer yet, any concurrent
	 * accesses will just be from processes like the bgwriter or checkpointer
	 * which don't care about its contents, so it doesn't really matter.
	 */
	buf_nblkno = _hash_getnewbuf(rel, start_nblkno, MAIN_FORKNUM);
	if (!IsBufferCleanupOK(buf_nblkno))
	{
		_hash_relbuf(rel, buf_oblkno);
		_hash_relbuf(rel, buf_nblkno);
		goto fail;
	}

	/*
	 * Since we are scribbling on the pages in the shared buffers, establish a
	 * critical section.  Any failure in this next code leaves us with a big
	 * problem: the metapage is effectively corrupt but could get written back
	 * to disk.
	 */
	START_CRIT_SECTION();

	/*
	 * Okay to proceed with split.  Update the metapage bucket mapping info.
	 */
	metap->hashm_maxbucket = new_bucket;

	if (new_bucket > metap->hashm_highmask)
	{
		/* Starting a new doubling */
		metap->hashm_lowmask = metap->hashm_highmask;
		metap->hashm_highmask = new_bucket | metap->hashm_lowmask;
		metap_update_masks = true;
	}

	/*
	 * If the split point is increasing we need to adjust the hashm_spares[]
	 * array and hashm_ovflpoint so that future overflow pages will be created
	 * beyond this new batch of bucket pages.
	 */
	if (spare_ndx > metap->hashm_ovflpoint)
	{
		metap->hashm_spares[spare_ndx] = metap->hashm_spares[metap->hashm_ovflpoint];
		metap->hashm_ovflpoint = spare_ndx;
		metap_update_splitpoint = true;
	}

	MarkBufferDirty(metabuf);

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

	opage = BufferGetPage(buf_oblkno);
	oopaque = HashPageGetOpaque(opage);

	/*
	 * Mark the old bucket to indicate that split is in progress.  (At
	 * operation end, we will clear the split-in-progress flag.)  Also, for a
	 * primary bucket page, hasho_prevblkno stores the number of buckets that
	 * existed as of the last split, so we must update that value here.
	 */
	oopaque->hasho_flag |= LH_BUCKET_BEING_SPLIT;
	oopaque->hasho_prevblkno = maxbucket;

	MarkBufferDirty(buf_oblkno);

	npage = BufferGetPage(buf_nblkno);

	/*
	 * initialize the new bucket's primary page and mark it to indicate that
	 * split is in progress.
	 */
	nopaque = HashPageGetOpaque(npage);
	nopaque->hasho_prevblkno = maxbucket;
	nopaque->hasho_nextblkno = InvalidBlockNumber;
	nopaque->hasho_bucket = new_bucket;
	nopaque->hasho_flag = LH_BUCKET_PAGE | LH_BUCKET_BEING_POPULATED;
	nopaque->hasho_page_id = HASHO_PAGE_ID;

	MarkBufferDirty(buf_nblkno);

	/* XLOG stuff */
	if (RelationNeedsWAL(rel))
	{
		xl_hash_split_allocate_page xlrec;
		XLogRecPtr	recptr;

		xlrec.new_bucket = maxbucket;
		xlrec.old_bucket_flag = oopaque->hasho_flag;
		xlrec.new_bucket_flag = nopaque->hasho_flag;
		xlrec.flags = 0;

		XLogBeginInsert();

		XLogRegisterBuffer(0, buf_oblkno, REGBUF_STANDARD);
		XLogRegisterBuffer(1, buf_nblkno, REGBUF_WILL_INIT);
		XLogRegisterBuffer(2, metabuf, REGBUF_STANDARD);

		if (metap_update_masks)
		{
			xlrec.flags |= XLH_SPLIT_META_UPDATE_MASKS;
			XLogRegisterBufData(2, &metap->hashm_lowmask, sizeof(uint32));
			XLogRegisterBufData(2, &metap->hashm_highmask, sizeof(uint32));
		}

		if (metap_update_splitpoint)
		{
			xlrec.flags |= XLH_SPLIT_META_UPDATE_SPLITPOINT;
			XLogRegisterBufData(2, &metap->hashm_ovflpoint,
								sizeof(uint32));
			XLogRegisterBufData(2,
								&metap->hashm_spares[metap->hashm_ovflpoint],
								sizeof(uint32));
		}

		XLogRegisterData(&xlrec, SizeOfHashSplitAllocPage);

		recptr = XLogInsert(RM_HASH_ID, XLOG_HASH_SPLIT_ALLOCATE_PAGE);

		PageSetLSN(BufferGetPage(buf_oblkno), recptr);
		PageSetLSN(BufferGetPage(buf_nblkno), recptr);
		PageSetLSN(BufferGetPage(metabuf), recptr);
	}

	END_CRIT_SECTION();

	/* drop lock, but keep pin */
	LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);

	/* Relocate records to the new bucket */
	_hash_splitbucket(rel, metabuf,
					  old_bucket, new_bucket,
					  buf_oblkno, buf_nblkno, NULL,
					  maxbucket, highmask, lowmask);

	/* all done, now release the pins on primary buckets. */
	_hash_dropbuf(rel, buf_oblkno);
	_hash_dropbuf(rel, buf_nblkno);

	return;

	/* Here if decide not to split or fail to acquire old bucket lock */
fail:

	/* We didn't write the metapage, so just drop lock */
	LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
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
 * We need to interlock against _hash_addovflpage() adding a new overflow page
 * concurrently, but it'd likely be better to use LockRelationForExtension
 * for the purpose.  OTOH, adding a splitpoint is a very infrequent operation,
 * so it may not be worth worrying about.
 *
 * Returns true if successful, or false if allocation failed due to
 * BlockNumber overflow.
 */
static bool
_hash_alloc_buckets(Relation rel, BlockNumber firstblock, uint32 nblocks)
{
	BlockNumber lastblock;
	PGIOAlignedBlock zerobuf;
	Page		page;
	HashPageOpaque ovflopaque;

	lastblock = firstblock + nblocks - 1;

	/*
	 * Check for overflow in block number calculation; if so, we cannot extend
	 * the index anymore.
	 */
	if (lastblock < firstblock || lastblock == InvalidBlockNumber)
		return false;

	page = (Page) zerobuf.data;

	/*
	 * Initialize the page.  Just zeroing the page won't work; see
	 * _hash_freeovflpage for similar usage.  We take care to make the special
	 * space valid for the benefit of tools such as pageinspect.
	 */
	_hash_pageinit(page, BLCKSZ);

	ovflopaque = HashPageGetOpaque(page);

	ovflopaque->hasho_prevblkno = InvalidBlockNumber;
	ovflopaque->hasho_nextblkno = InvalidBlockNumber;
	ovflopaque->hasho_bucket = InvalidBucket;
	ovflopaque->hasho_flag = LH_UNUSED_PAGE;
	ovflopaque->hasho_page_id = HASHO_PAGE_ID;

	if (RelationNeedsWAL(rel))
		log_newpage(&rel->rd_locator,
					MAIN_FORKNUM,
					lastblock,
					zerobuf.data,
					true);

	PageSetChecksumInplace(page, lastblock);
	smgrextend(RelationGetSmgr(rel), MAIN_FORKNUM, lastblock, zerobuf.data,
			   false);

	return true;
}


/*
 * _hash_splitbucket -- split 'obucket' into 'obucket' and 'nbucket'
 *
 * This routine is used to partition the tuples between old and new bucket and
 * is used to finish the incomplete split operations.  To finish the previously
 * interrupted split operation, the caller needs to fill htab.  If htab is set,
 * then we skip the movement of tuples that exists in htab, otherwise NULL
 * value of htab indicates movement of all the tuples that belong to the new
 * bucket.
 *
 * We are splitting a bucket that consists of a base bucket page and zero
 * or more overflow (bucket chain) pages.  We must relocate tuples that
 * belong in the new bucket.
 *
 * The caller must hold cleanup locks on both buckets to ensure that
 * no one else is trying to access them (see README).
 *
 * The caller must hold a pin, but no lock, on the metapage buffer.
 * The buffer is returned in the same state.  (The metapage is only
 * touched if it becomes necessary to add or remove overflow pages.)
 *
 * Split needs to retain pin on primary bucket pages of both old and new
 * buckets till end of operation.  This is to prevent vacuum from starting
 * while a split is in progress.
 *
 * In addition, the caller must have created the new bucket's base page,
 * which is passed in buffer nbuf, pinned and write-locked.  The lock will be
 * released here and pin must be released by the caller.  (The API is set up
 * this way because we must do _hash_getnewbuf() before releasing the metapage
 * write lock.  So instead of passing the new bucket's start block number, we
 * pass an actual buffer.)
 */
static void
_hash_splitbucket(Relation rel,
				  Buffer metabuf,
				  Bucket obucket,
				  Bucket nbucket,
				  Buffer obuf,
				  Buffer nbuf,
				  HTAB *htab,
				  uint32 maxbucket,
				  uint32 highmask,
				  uint32 lowmask)
{
	Buffer		bucket_obuf;
	Buffer		bucket_nbuf;
	Page		opage;
	Page		npage;
	HashPageOpaque oopaque;
	HashPageOpaque nopaque;
	OffsetNumber itup_offsets[MaxIndexTuplesPerPage];
	IndexTuple	itups[MaxIndexTuplesPerPage];
	Size		all_tups_size = 0;
	int			i;
	uint16		nitups = 0;

	bucket_obuf = obuf;
	opage = BufferGetPage(obuf);
	oopaque = HashPageGetOpaque(opage);

	bucket_nbuf = nbuf;
	npage = BufferGetPage(nbuf);
	nopaque = HashPageGetOpaque(npage);

	/* Copy the predicate locks from old bucket to new bucket. */
	PredicateLockPageSplit(rel,
						   BufferGetBlockNumber(bucket_obuf),
						   BufferGetBlockNumber(bucket_nbuf));

	/*
	 * Partition the tuples in the old bucket between the old bucket and the
	 * new bucket, advancing along the old bucket's overflow bucket chain and
	 * adding overflow pages to the new bucket as needed.  Outer loop iterates
	 * once per page in old bucket.
	 */
	for (;;)
	{
		BlockNumber oblkno;
		OffsetNumber ooffnum;
		OffsetNumber omaxoffnum;

		/* Scan each tuple in old page */
		omaxoffnum = PageGetMaxOffsetNumber(opage);
		for (ooffnum = FirstOffsetNumber;
			 ooffnum <= omaxoffnum;
			 ooffnum = OffsetNumberNext(ooffnum))
		{
			IndexTuple	itup;
			Size		itemsz;
			Bucket		bucket;
			bool		found = false;

			/* skip dead tuples */
			if (ItemIdIsDead(PageGetItemId(opage, ooffnum)))
				continue;

			/*
			 * Before inserting a tuple, probe the hash table containing TIDs
			 * of tuples belonging to new bucket, if we find a match, then
			 * skip that tuple, else fetch the item's hash key (conveniently
			 * stored in the item) and determine which bucket it now belongs
			 * in.
			 */
			itup = (IndexTuple) PageGetItem(opage,
											PageGetItemId(opage, ooffnum));

			if (htab)
				(void) hash_search(htab, &itup->t_tid, HASH_FIND, &found);

			if (found)
				continue;

			bucket = _hash_hashkey2bucket(_hash_get_indextuple_hashkey(itup),
										  maxbucket, highmask, lowmask);

			if (bucket == nbucket)
			{
				IndexTuple	new_itup;

				/*
				 * make a copy of index tuple as we have to scribble on it.
				 */
				new_itup = CopyIndexTuple(itup);

				/*
				 * mark the index tuple as moved by split, such tuples are
				 * skipped by scan if there is split in progress for a bucket.
				 */
				new_itup->t_info |= INDEX_MOVED_BY_SPLIT_MASK;

				/*
				 * insert the tuple into the new bucket.  if it doesn't fit on
				 * the current page in the new bucket, we must allocate a new
				 * overflow page and place the tuple on that page instead.
				 */
				itemsz = IndexTupleSize(new_itup);
				itemsz = MAXALIGN(itemsz);

				if (PageGetFreeSpaceForMultipleTuples(npage, nitups + 1) < (all_tups_size + itemsz))
				{
					/*
					 * Change the shared buffer state in critical section,
					 * otherwise any error could make it unrecoverable.
					 */
					START_CRIT_SECTION();

					_hash_pgaddmultitup(rel, nbuf, itups, itup_offsets, nitups);
					MarkBufferDirty(nbuf);
					/* log the split operation before releasing the lock */
					log_split_page(rel, nbuf);

					END_CRIT_SECTION();

					/* drop lock, but keep pin */
					LockBuffer(nbuf, BUFFER_LOCK_UNLOCK);

					/* be tidy */
					for (i = 0; i < nitups; i++)
						pfree(itups[i]);
					nitups = 0;
					all_tups_size = 0;

					/* chain to a new overflow page */
					nbuf = _hash_addovflpage(rel, metabuf, nbuf, (nbuf == bucket_nbuf));
					npage = BufferGetPage(nbuf);
					nopaque = HashPageGetOpaque(npage);
				}

				itups[nitups++] = new_itup;
				all_tups_size += itemsz;
			}
			else
			{
				/*
				 * the tuple stays on this page, so nothing to do.
				 */
				Assert(bucket == obucket);
			}
		}

		oblkno = oopaque->hasho_nextblkno;

		/* retain the pin on the old primary bucket */
		if (obuf == bucket_obuf)
			LockBuffer(obuf, BUFFER_LOCK_UNLOCK);
		else
			_hash_relbuf(rel, obuf);

		/* Exit loop if no more overflow pages in old bucket */
		if (!BlockNumberIsValid(oblkno))
		{
			/*
			 * Change the shared buffer state in critical section, otherwise
			 * any error could make it unrecoverable.
			 */
			START_CRIT_SECTION();

			_hash_pgaddmultitup(rel, nbuf, itups, itup_offsets, nitups);
			MarkBufferDirty(nbuf);
			/* log the split operation before releasing the lock */
			log_split_page(rel, nbuf);

			END_CRIT_SECTION();

			if (nbuf == bucket_nbuf)
				LockBuffer(nbuf, BUFFER_LOCK_UNLOCK);
			else
				_hash_relbuf(rel, nbuf);

			/* be tidy */
			for (i = 0; i < nitups; i++)
				pfree(itups[i]);
			break;
		}

		/* Else, advance to next old page */
		obuf = _hash_getbuf(rel, oblkno, HASH_READ, LH_OVERFLOW_PAGE);
		opage = BufferGetPage(obuf);
		oopaque = HashPageGetOpaque(opage);
	}

	/*
	 * We're at the end of the old bucket chain, so we're done partitioning
	 * the tuples.  Mark the old and new buckets to indicate split is
	 * finished.
	 *
	 * To avoid deadlocks due to locking order of buckets, first lock the old
	 * bucket and then the new bucket.
	 */
	LockBuffer(bucket_obuf, BUFFER_LOCK_EXCLUSIVE);
	opage = BufferGetPage(bucket_obuf);
	oopaque = HashPageGetOpaque(opage);

	LockBuffer(bucket_nbuf, BUFFER_LOCK_EXCLUSIVE);
	npage = BufferGetPage(bucket_nbuf);
	nopaque = HashPageGetOpaque(npage);

	START_CRIT_SECTION();

	oopaque->hasho_flag &= ~LH_BUCKET_BEING_SPLIT;
	nopaque->hasho_flag &= ~LH_BUCKET_BEING_POPULATED;

	/*
	 * After the split is finished, mark the old bucket to indicate that it
	 * contains deletable tuples.  We will clear split-cleanup flag after
	 * deleting such tuples either at the end of split or at the next split
	 * from old bucket or at the time of vacuum.
	 */
	oopaque->hasho_flag |= LH_BUCKET_NEEDS_SPLIT_CLEANUP;

	/*
	 * now write the buffers, here we don't release the locks as caller is
	 * responsible to release locks.
	 */
	MarkBufferDirty(bucket_obuf);
	MarkBufferDirty(bucket_nbuf);

	if (RelationNeedsWAL(rel))
	{
		XLogRecPtr	recptr;
		xl_hash_split_complete xlrec;

		xlrec.old_bucket_flag = oopaque->hasho_flag;
		xlrec.new_bucket_flag = nopaque->hasho_flag;

		XLogBeginInsert();

		XLogRegisterData(&xlrec, SizeOfHashSplitComplete);

		XLogRegisterBuffer(0, bucket_obuf, REGBUF_STANDARD);
		XLogRegisterBuffer(1, bucket_nbuf, REGBUF_STANDARD);

		recptr = XLogInsert(RM_HASH_ID, XLOG_HASH_SPLIT_COMPLETE);

		PageSetLSN(BufferGetPage(bucket_obuf), recptr);
		PageSetLSN(BufferGetPage(bucket_nbuf), recptr);
	}

	END_CRIT_SECTION();

	/*
	 * If possible, clean up the old bucket.  We might not be able to do this
	 * if someone else has a pin on it, but if not then we can go ahead.  This
	 * isn't absolutely necessary, but it reduces bloat; if we don't do it
	 * now, VACUUM will do it eventually, but maybe not until new overflow
	 * pages have been allocated.  Note that there's no need to clean up the
	 * new bucket.
	 */
	if (IsBufferCleanupOK(bucket_obuf))
	{
		LockBuffer(bucket_nbuf, BUFFER_LOCK_UNLOCK);
		hashbucketcleanup(rel, obucket, bucket_obuf,
						  BufferGetBlockNumber(bucket_obuf), NULL,
						  maxbucket, highmask, lowmask, NULL, NULL, true,
						  NULL, NULL);
	}
	else
	{
		LockBuffer(bucket_nbuf, BUFFER_LOCK_UNLOCK);
		LockBuffer(bucket_obuf, BUFFER_LOCK_UNLOCK);
	}
}

/*
 *	_hash_finish_split() -- Finish the previously interrupted split operation
 *
 * To complete the split operation, we form the hash table of TIDs in new
 * bucket which is then used by split operation to skip tuples that are
 * already moved before the split operation was previously interrupted.
 *
 * The caller must hold a pin, but no lock, on the metapage and old bucket's
 * primary page buffer.  The buffers are returned in the same state.  (The
 * metapage is only touched if it becomes necessary to add or remove overflow
 * pages.)
 */
void
_hash_finish_split(Relation rel, Buffer metabuf, Buffer obuf, Bucket obucket,
				   uint32 maxbucket, uint32 highmask, uint32 lowmask)
{
	HASHCTL		hash_ctl;
	HTAB	   *tidhtab;
	Buffer		bucket_nbuf = InvalidBuffer;
	Buffer		nbuf;
	Page		npage;
	BlockNumber nblkno;
	BlockNumber bucket_nblkno;
	HashPageOpaque npageopaque;
	Bucket		nbucket;
	bool		found;

	/* Initialize hash tables used to track TIDs */
	hash_ctl.keysize = sizeof(ItemPointerData);
	hash_ctl.entrysize = sizeof(ItemPointerData);
	hash_ctl.hcxt = CurrentMemoryContext;

	tidhtab =
		hash_create("bucket ctids",
					256,		/* arbitrary initial size */
					&hash_ctl,
					HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	bucket_nblkno = nblkno = _hash_get_newblock_from_oldbucket(rel, obucket);

	/*
	 * Scan the new bucket and build hash table of TIDs
	 */
	for (;;)
	{
		OffsetNumber noffnum;
		OffsetNumber nmaxoffnum;

		nbuf = _hash_getbuf(rel, nblkno, HASH_READ,
							LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);

		/* remember the primary bucket buffer to acquire cleanup lock on it. */
		if (nblkno == bucket_nblkno)
			bucket_nbuf = nbuf;

		npage = BufferGetPage(nbuf);
		npageopaque = HashPageGetOpaque(npage);

		/* Scan each tuple in new page */
		nmaxoffnum = PageGetMaxOffsetNumber(npage);
		for (noffnum = FirstOffsetNumber;
			 noffnum <= nmaxoffnum;
			 noffnum = OffsetNumberNext(noffnum))
		{
			IndexTuple	itup;

			/* Fetch the item's TID and insert it in hash table. */
			itup = (IndexTuple) PageGetItem(npage,
											PageGetItemId(npage, noffnum));

			(void) hash_search(tidhtab, &itup->t_tid, HASH_ENTER, &found);

			Assert(!found);
		}

		nblkno = npageopaque->hasho_nextblkno;

		/*
		 * release our write lock without modifying buffer and ensure to
		 * retain the pin on primary bucket.
		 */
		if (nbuf == bucket_nbuf)
			LockBuffer(nbuf, BUFFER_LOCK_UNLOCK);
		else
			_hash_relbuf(rel, nbuf);

		/* Exit loop if no more overflow pages in new bucket */
		if (!BlockNumberIsValid(nblkno))
			break;
	}

	/*
	 * Conditionally get the cleanup lock on old and new buckets to perform
	 * the split operation.  If we don't get the cleanup locks, silently give
	 * up and next insertion on old bucket will try again to complete the
	 * split.
	 */
	if (!ConditionalLockBufferForCleanup(obuf))
	{
		hash_destroy(tidhtab);
		return;
	}
	if (!ConditionalLockBufferForCleanup(bucket_nbuf))
	{
		LockBuffer(obuf, BUFFER_LOCK_UNLOCK);
		hash_destroy(tidhtab);
		return;
	}

	npage = BufferGetPage(bucket_nbuf);
	npageopaque = HashPageGetOpaque(npage);
	nbucket = npageopaque->hasho_bucket;

	_hash_splitbucket(rel, metabuf, obucket,
					  nbucket, obuf, bucket_nbuf, tidhtab,
					  maxbucket, highmask, lowmask);

	_hash_dropbuf(rel, bucket_nbuf);
	hash_destroy(tidhtab);
}

/*
 *	log_split_page() -- Log the split operation
 *
 *	We log the split operation when the new page in new bucket gets full,
 *	so we log the entire page.
 *
 *	'buf' must be locked by the caller which is also responsible for unlocking
 *	it.
 */
static void
log_split_page(Relation rel, Buffer buf)
{
	if (RelationNeedsWAL(rel))
	{
		XLogRecPtr	recptr;

		XLogBeginInsert();

		XLogRegisterBuffer(0, buf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);

		recptr = XLogInsert(RM_HASH_ID, XLOG_HASH_SPLIT_PAGE);

		PageSetLSN(BufferGetPage(buf), recptr);
	}
}

/*
 *	_hash_getcachedmetap() -- Returns cached metapage data.
 *
 *	If metabuf is not InvalidBuffer, caller must hold a pin, but no lock, on
 *	the metapage.  If not set, we'll set it before returning if we have to
 *	refresh the cache, and return with a pin but no lock on it; caller is
 *	responsible for releasing the pin.
 *
 *	We refresh the cache if it's not initialized yet or force_refresh is true.
 */
HashMetaPage
_hash_getcachedmetap(Relation rel, Buffer *metabuf, bool force_refresh)
{
	Page		page;

	Assert(metabuf);
	if (force_refresh || rel->rd_amcache == NULL)
	{
		char	   *cache = NULL;

		/*
		 * It's important that we don't set rd_amcache to an invalid value.
		 * Either MemoryContextAlloc or _hash_getbuf could fail, so don't
		 * install a pointer to the newly-allocated storage in the actual
		 * relcache entry until both have succeeded.
		 */
		if (rel->rd_amcache == NULL)
			cache = MemoryContextAlloc(rel->rd_indexcxt,
									   sizeof(HashMetaPageData));

		/* Read the metapage. */
		if (BufferIsValid(*metabuf))
			LockBuffer(*metabuf, BUFFER_LOCK_SHARE);
		else
			*metabuf = _hash_getbuf(rel, HASH_METAPAGE, HASH_READ,
									LH_META_PAGE);
		page = BufferGetPage(*metabuf);

		/* Populate the cache. */
		if (rel->rd_amcache == NULL)
			rel->rd_amcache = cache;
		memcpy(rel->rd_amcache, HashPageGetMeta(page),
			   sizeof(HashMetaPageData));

		/* Release metapage lock, but keep the pin. */
		LockBuffer(*metabuf, BUFFER_LOCK_UNLOCK);
	}

	return (HashMetaPage) rel->rd_amcache;
}

/*
 *	_hash_getbucketbuf_from_hashkey() -- Get the bucket's buffer for the given
 *										 hashkey.
 *
 *	Bucket pages do not move or get removed once they are allocated. This give
 *	us an opportunity to use the previously saved metapage contents to reach
 *	the target bucket buffer, instead of reading from the metapage every time.
 *	This saves one buffer access every time we want to reach the target bucket
 *	buffer, which is very helpful savings in bufmgr traffic and contention.
 *
 *	The access type parameter (HASH_READ or HASH_WRITE) indicates whether the
 *	bucket buffer has to be locked for reading or writing.
 *
 *	The out parameter cachedmetap is set with metapage contents used for
 *	hashkey to bucket buffer mapping. Some callers need this info to reach the
 *	old bucket in case of bucket split, see _hash_doinsert().
 */
Buffer
_hash_getbucketbuf_from_hashkey(Relation rel, uint32 hashkey, int access,
								HashMetaPage *cachedmetap)
{
	HashMetaPage metap;
	Buffer		buf;
	Buffer		metabuf = InvalidBuffer;
	Page		page;
	Bucket		bucket;
	BlockNumber blkno;
	HashPageOpaque opaque;

	/* We read from target bucket buffer, hence locking is must. */
	Assert(access == HASH_READ || access == HASH_WRITE);

	metap = _hash_getcachedmetap(rel, &metabuf, false);
	Assert(metap != NULL);

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

		/* Fetch the primary bucket page for the bucket */
		buf = _hash_getbuf(rel, blkno, access, LH_BUCKET_PAGE);
		page = BufferGetPage(buf);
		opaque = HashPageGetOpaque(page);
		Assert(opaque->hasho_bucket == bucket);
		Assert(opaque->hasho_prevblkno != InvalidBlockNumber);

		/*
		 * If this bucket hasn't been split, we're done.
		 */
		if (opaque->hasho_prevblkno <= metap->hashm_maxbucket)
			break;

		/* Drop lock on this buffer, update cached metapage, and retry. */
		_hash_relbuf(rel, buf);
		metap = _hash_getcachedmetap(rel, &metabuf, true);
		Assert(metap != NULL);
	}

	if (BufferIsValid(metabuf))
		_hash_dropbuf(rel, metabuf);

	if (cachedmetap)
		*cachedmetap = metap;

	return buf;
}
