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
 *	  $Header: /cvsroot/pgsql/src/backend/access/hash/hashpage.c,v 1.40 2003/09/02 02:18:38 tgl Exp $
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


/*
 *	We use high-concurrency locking on hash indices.  There are two cases in
 *	which we don't do locking.  One is when we're building the index.
 *	Since the creating transaction has not committed, no one can see
 *	the index, and there's no reason to share locks.  The second case
 *	is when we're just starting up the database system.  We use some
 *	special-purpose initialization code in the relation cache manager
 *	(see utils/cache/relcache.c) to allow us to do indexed scans on
 *	the system catalogs before we'd normally be able to.  This happens
 *	before the lock table is fully initialized, so we can't use it.
 *	Strictly speaking, this violates 2pl, but we don't do 2pl on the
 *	system catalogs anyway.
 *
 *	Note that our page locks are actual lockmanager locks, not buffer
 *	locks (as are used by btree, for example).	This is a good idea because
 *	the algorithms are not deadlock-free, and we'd better be able to detect
 *	and recover from deadlocks.
 *
 *	Another important difference from btree is that a hash indexscan
 *	retains both a lock and a buffer pin on the current index page
 *	between hashgettuple() calls (btree keeps only a buffer pin).
 *	Because of this, it's safe to do item deletions with only a regular
 *	write lock on a hash page --- there cannot be an indexscan stopped on
 *	the page being deleted, other than an indexscan of our own backend,
 *	which will be taken care of by _hash_adjscans.
 */
#define USELOCKING		(!BuildingHash && !IsInitProcessingMode())


static void _hash_setpagelock(Relation rel, BlockNumber blkno, int access);
static void _hash_unsetpagelock(Relation rel, BlockNumber blkno, int access);
static void _hash_splitbucket(Relation rel, Buffer metabuf,
							  Bucket obucket, Bucket nbucket);


/*
 *	_hash_metapinit() -- Initialize the metadata page of a hash index,
 *				the two buckets that we begin with and the initial
 *				bitmap page.
 */
void
_hash_metapinit(Relation rel)
{
	HashMetaPage metap;
	HashPageOpaque pageopaque;
	Buffer		metabuf;
	Buffer		buf;
	Page		pg;
	uint16		i;

	/* can't be sharing this with anyone, now... */
	if (USELOCKING)
		LockRelation(rel, AccessExclusiveLock);

	if (RelationGetNumberOfBlocks(rel) != 0)
		elog(ERROR, "cannot initialize non-empty hash index \"%s\"",
			 RelationGetRelationName(rel));

	metabuf = _hash_getbuf(rel, HASH_METAPAGE, HASH_WRITE);
	pg = BufferGetPage(metabuf);
	_hash_pageinit(pg, BufferGetPageSize(metabuf));

	pageopaque = (HashPageOpaque) PageGetSpecialPointer(pg);
	pageopaque->hasho_oaddr = 0;
	pageopaque->hasho_prevblkno = InvalidBlockNumber;
	pageopaque->hasho_nextblkno = InvalidBlockNumber;
	pageopaque->hasho_flag = LH_META_PAGE;
	pageopaque->hasho_bucket = -1;

	metap = (HashMetaPage) pg;

	metap->hashm_magic = HASH_MAGIC;
	metap->hashm_version = HASH_VERSION;
	metap->hashm_ntuples = 0;
	metap->hashm_nmaps = 0;
	metap->hashm_ffactor = DEFAULT_FFACTOR;
	metap->hashm_bsize = BufferGetPageSize(metabuf);
	metap->hashm_bshift = _hash_log2(metap->hashm_bsize);
	/* page size must be power of 2 */
	Assert(metap->hashm_bsize == (1 << metap->hashm_bshift));
	/* bitmap size is half of page size, to keep it also power of 2 */
	metap->hashm_bmsize = (metap->hashm_bsize >> 1);
	Assert(metap->hashm_bsize >= metap->hashm_bmsize +
		   MAXALIGN(sizeof(PageHeaderData)) +
		   MAXALIGN(sizeof(HashPageOpaqueData)));
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
	 * initialize the first two buckets
	 */
	for (i = 0; i <= 1; i++)
	{
		buf = _hash_getbuf(rel, BUCKET_TO_BLKNO(metap, i), HASH_WRITE);
		pg = BufferGetPage(buf);
		_hash_pageinit(pg, BufferGetPageSize(buf));
		pageopaque = (HashPageOpaque) PageGetSpecialPointer(pg);
		pageopaque->hasho_oaddr = 0;
		pageopaque->hasho_prevblkno = InvalidBlockNumber;
		pageopaque->hasho_nextblkno = InvalidBlockNumber;
		pageopaque->hasho_flag = LH_BUCKET_PAGE;
		pageopaque->hasho_bucket = i;
		_hash_wrtbuf(rel, buf);
	}

	/*
	 * Initialize bitmap page.  Can't do this until we
	 * create the first two buckets, else smgr will complain.
	 */
	_hash_initbitmap(rel, metap, 3);

	/* all done */
	_hash_wrtbuf(rel, metabuf);

	if (USELOCKING)
		UnlockRelation(rel, AccessExclusiveLock);
}

/*
 *	_hash_getbuf() -- Get a buffer by block number for read or write.
 *
 *		When this routine returns, the appropriate lock is set on the
 *		requested buffer its reference count is correct.
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
	switch (access)
	{
		case HASH_WRITE:
		case HASH_READ:
			_hash_setpagelock(rel, blkno, access);
			break;
		default:
			elog(ERROR, "unrecognized hash access code: %d", access);
			break;
	}
	buf = ReadBuffer(rel, blkno);

	/* ref count and lock type are correct */
	return buf;
}

/*
 *	_hash_relbuf() -- release a locked buffer.
 */
void
_hash_relbuf(Relation rel, Buffer buf, int access)
{
	BlockNumber blkno;

	blkno = BufferGetBlockNumber(buf);

	switch (access)
	{
		case HASH_WRITE:
		case HASH_READ:
			_hash_unsetpagelock(rel, blkno, access);
			break;
		default:
			elog(ERROR, "unrecognized hash access code: %d", access);
			break;
	}

	ReleaseBuffer(buf);
}

/*
 *	_hash_wrtbuf() -- write a hash page to disk.
 *
 *		This routine releases the lock held on the buffer and our reference
 *		to it.	It is an error to call _hash_wrtbuf() without a write lock
 *		or a reference to the buffer.
 */
void
_hash_wrtbuf(Relation rel, Buffer buf)
{
	BlockNumber blkno;

	blkno = BufferGetBlockNumber(buf);
	WriteBuffer(buf);
	_hash_unsetpagelock(rel, blkno, HASH_WRITE);
}

/*
 *	_hash_wrtnorelbuf() -- write a hash page to disk, but do not release
 *						 our reference or lock.
 *
 *		It is an error to call _hash_wrtnorelbuf() without a write lock
 *		or a reference to the buffer.
 */
void
_hash_wrtnorelbuf(Buffer buf)
{
	BlockNumber blkno;

	blkno = BufferGetBlockNumber(buf);
	WriteNoReleaseBuffer(buf);
}

/*
 * _hash_chgbufaccess() -- Change from read to write access or vice versa.
 *
 * When changing from write to read, we assume the buffer is dirty and tell
 * bufmgr it must be written out.
 */
void
_hash_chgbufaccess(Relation rel,
				   Buffer buf,
				   int from_access,
				   int to_access)
{
	BlockNumber blkno;

	blkno = BufferGetBlockNumber(buf);

	if (from_access == HASH_WRITE)
		_hash_wrtnorelbuf(buf);

	_hash_unsetpagelock(rel, blkno, from_access);

	_hash_setpagelock(rel, blkno, to_access);
}

/*
 *	_hash_pageinit() -- Initialize a new page.
 */
void
_hash_pageinit(Page page, Size size)
{
	Assert(PageIsNew(page));
	PageInit(page, size, sizeof(HashPageOpaqueData));
}

/*
 *  _hash_setpagelock() -- Acquire the requested type of lock on a page.
 */
static void
_hash_setpagelock(Relation rel,
				  BlockNumber blkno,
				  int access)
{
	if (USELOCKING)
	{
		switch (access)
		{
			case HASH_WRITE:
				LockPage(rel, blkno, ExclusiveLock);
				break;
			case HASH_READ:
				LockPage(rel, blkno, ShareLock);
				break;
			default:
				elog(ERROR, "unrecognized hash access code: %d", access);
				break;
		}
	}
}

/*
 *  _hash_unsetpagelock() -- Release the specified type of lock on a page.
 */
static void
_hash_unsetpagelock(Relation rel,
					BlockNumber blkno,
					int access)
{
	if (USELOCKING)
	{
		switch (access)
		{
			case HASH_WRITE:
				UnlockPage(rel, blkno, ExclusiveLock);
				break;
			case HASH_READ:
				UnlockPage(rel, blkno, ShareLock);
				break;
			default:
				elog(ERROR, "unrecognized hash access code: %d", access);
				break;
		}
	}
}

/*
 * Delete a hash index item.
 *
 * It is safe to delete an item after acquiring a regular WRITE lock on
 * the page, because no other backend can hold a READ lock on the page,
 * and that means no other backend currently has an indexscan stopped on
 * any item of the item being deleted.	Our own backend might have such
 * an indexscan (in fact *will*, since that's how VACUUM found the item
 * in the first place), but _hash_adjscans will fix the scan position.
 */
void
_hash_pagedel(Relation rel, ItemPointer tid)
{
	Buffer		buf;
	Buffer		metabuf;
	Page		page;
	BlockNumber blkno;
	OffsetNumber offno;
	HashMetaPage metap;
	HashPageOpaque opaque;

	blkno = ItemPointerGetBlockNumber(tid);
	offno = ItemPointerGetOffsetNumber(tid);

	buf = _hash_getbuf(rel, blkno, HASH_WRITE);
	page = BufferGetPage(buf);
	_hash_checkpage(page, LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
	opaque = (HashPageOpaque) PageGetSpecialPointer(page);

	PageIndexTupleDelete(page, offno);

	if (PageIsEmpty(page) && (opaque->hasho_flag & LH_OVERFLOW_PAGE))
		_hash_freeovflpage(rel, buf);
	else
		_hash_wrtbuf(rel, buf);

	metabuf = _hash_getbuf(rel, HASH_METAPAGE, HASH_WRITE);
	metap = (HashMetaPage) BufferGetPage(metabuf);
	_hash_checkpage((Page) metap, LH_META_PAGE);
	metap->hashm_ntuples--;
	_hash_wrtbuf(rel, metabuf);
}

/*
 * Expand the hash table by creating one new bucket.
 */
void
_hash_expandtable(Relation rel, Buffer metabuf)
{
	HashMetaPage metap;
	Bucket		old_bucket;
	Bucket		new_bucket;
	uint32		spare_ndx;

	metap = (HashMetaPage) BufferGetPage(metabuf);
	_hash_checkpage((Page) metap, LH_META_PAGE);

	_hash_chgbufaccess(rel, metabuf, HASH_READ, HASH_WRITE);

	new_bucket = ++metap->hashm_maxbucket;
	old_bucket = (new_bucket & metap->hashm_lowmask);

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
	 * page creation.
	 */
	spare_ndx = _hash_log2(metap->hashm_maxbucket + 1);
	if (spare_ndx > metap->hashm_ovflpoint)
	{
		Assert(spare_ndx == metap->hashm_ovflpoint + 1);
		metap->hashm_spares[spare_ndx] = metap->hashm_spares[metap->hashm_ovflpoint];
		metap->hashm_ovflpoint = spare_ndx;
	}

	_hash_chgbufaccess(rel, metabuf, HASH_WRITE, HASH_READ);

	/* Relocate records to the new bucket */
	_hash_splitbucket(rel, metabuf, old_bucket, new_bucket);
}


/*
 * _hash_splitbucket -- split 'obucket' into 'obucket' and 'nbucket'
 *
 * We are splitting a bucket that consists of a base bucket page and zero
 * or more overflow (bucket chain) pages.  We must relocate tuples that
 * belong in the new bucket, and compress out any free space in the old
 * bucket.
 */
static void
_hash_splitbucket(Relation rel,
				  Buffer metabuf,
				  Bucket obucket,
				  Bucket nbucket)
{
	Bucket		bucket;
	Buffer		obuf;
	Buffer		nbuf;
	Buffer		ovflbuf;
	BlockNumber oblkno;
	BlockNumber nblkno;
	BlockNumber start_oblkno;
	BlockNumber start_nblkno;
	bool		null;
	Datum		datum;
	HashItem	hitem;
	HashPageOpaque oopaque;
	HashPageOpaque nopaque;
	HashMetaPage metap;
	IndexTuple	itup;
	Size		itemsz;
	OffsetNumber ooffnum;
	OffsetNumber noffnum;
	OffsetNumber omaxoffnum;
	Page		opage;
	Page		npage;
	TupleDesc	itupdesc = RelationGetDescr(rel);

	metap = (HashMetaPage) BufferGetPage(metabuf);
	_hash_checkpage((Page) metap, LH_META_PAGE);

	/* get the buffers & pages */
	start_oblkno = BUCKET_TO_BLKNO(metap, obucket);
	start_nblkno = BUCKET_TO_BLKNO(metap, nbucket);
	oblkno = start_oblkno;
	nblkno = start_nblkno;
	obuf = _hash_getbuf(rel, oblkno, HASH_WRITE);
	nbuf = _hash_getbuf(rel, nblkno, HASH_WRITE);
	opage = BufferGetPage(obuf);
	npage = BufferGetPage(nbuf);

	/* initialize the new bucket page */
	_hash_pageinit(npage, BufferGetPageSize(nbuf));
	nopaque = (HashPageOpaque) PageGetSpecialPointer(npage);
	nopaque->hasho_prevblkno = InvalidBlockNumber;
	nopaque->hasho_nextblkno = InvalidBlockNumber;
	nopaque->hasho_flag = LH_BUCKET_PAGE;
	nopaque->hasho_oaddr = 0;
	nopaque->hasho_bucket = nbucket;
	_hash_wrtnorelbuf(nbuf);

	/*
	 * make sure the old bucket isn't empty.  advance 'opage' and friends
	 * through the overflow bucket chain until we find a non-empty page.
	 *
	 * XXX we should only need this once, if we are careful to preserve the
	 * invariant that overflow pages are never empty.
	 */
	_hash_checkpage(opage, LH_BUCKET_PAGE);
	oopaque = (HashPageOpaque) PageGetSpecialPointer(opage);
	if (PageIsEmpty(opage))
	{
		oblkno = oopaque->hasho_nextblkno;
		_hash_relbuf(rel, obuf, HASH_WRITE);
		if (!BlockNumberIsValid(oblkno))
		{
			/*
			 * the old bucket is completely empty; of course, the new
			 * bucket will be as well, but since it's a base bucket page
			 * we don't care.
			 */
			_hash_relbuf(rel, nbuf, HASH_WRITE);
			return;
		}
		obuf = _hash_getbuf(rel, oblkno, HASH_WRITE);
		opage = BufferGetPage(obuf);
		_hash_checkpage(opage, LH_OVERFLOW_PAGE);
		if (PageIsEmpty(opage))
			elog(ERROR, "empty hash overflow page %u", oblkno);
		oopaque = (HashPageOpaque) PageGetSpecialPointer(opage);
	}

	/*
	 * we are now guaranteed that 'opage' is not empty.  partition the
	 * tuples in the old bucket between the old bucket and the new bucket,
	 * advancing along their respective overflow bucket chains and adding
	 * overflow pages as needed.
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
			/* at end of page, but check for overflow page */
			oblkno = oopaque->hasho_nextblkno;
			if (BlockNumberIsValid(oblkno))
			{
				/*
				 * we ran out of tuples on this particular page, but we
				 * have more overflow pages; re-init values.
				 */
				_hash_wrtbuf(rel, obuf);
				obuf = _hash_getbuf(rel, oblkno, HASH_WRITE);
				opage = BufferGetPage(obuf);
				_hash_checkpage(opage, LH_OVERFLOW_PAGE);
				oopaque = (HashPageOpaque) PageGetSpecialPointer(opage);
				/* we're guaranteed that an ovfl page has at least 1 tuple */
				if (PageIsEmpty(opage))
					elog(ERROR, "empty hash overflow page %u", oblkno);
				ooffnum = FirstOffsetNumber;
				omaxoffnum = PageGetMaxOffsetNumber(opage);
			}
			else
			{
				/*
				 * We're at the end of the bucket chain, so now we're
				 * really done with everything.  Before quitting, call
				 * _hash_squeezebucket to ensure the tuples remaining in the
				 * old bucket (including the overflow pages) are packed as
				 * tightly as possible.  The new bucket is already tight.
				 */
				_hash_wrtbuf(rel, obuf);
				_hash_wrtbuf(rel, nbuf);
				_hash_squeezebucket(rel, obucket, start_oblkno);
				return;
			}
		}

		/* hash on the tuple */
		hitem = (HashItem) PageGetItem(opage, PageGetItemId(opage, ooffnum));
		itup = &(hitem->hash_itup);
		datum = index_getattr(itup, 1, itupdesc, &null);
		Assert(!null);

		bucket = _hash_call(rel, metap, datum);

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
				ovflbuf = _hash_addovflpage(rel, metabuf, nbuf);
				_hash_wrtbuf(rel, nbuf);
				nbuf = ovflbuf;
				npage = BufferGetPage(nbuf);
				_hash_checkpage(npage, LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
			}

			noffnum = OffsetNumberNext(PageGetMaxOffsetNumber(npage));
			if (PageAddItem(npage, (Item) hitem, itemsz, noffnum, LP_USED)
				== InvalidOffsetNumber)
				elog(ERROR, "failed to add index item to \"%s\"",
					 RelationGetRelationName(rel));
			_hash_wrtnorelbuf(nbuf);

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
			_hash_wrtnorelbuf(obuf);
			omaxoffnum = OffsetNumberPrev(omaxoffnum);

			/*
			 * tidy up.  if the old page was an overflow page and it is
			 * now empty, we must free it (we want to preserve the
			 * invariant that overflow pages cannot be empty).
			 */
			if (PageIsEmpty(opage) &&
				(oopaque->hasho_flag & LH_OVERFLOW_PAGE))
			{
				oblkno = _hash_freeovflpage(rel, obuf);

				/* check that we're not through the bucket chain */
				if (!BlockNumberIsValid(oblkno))
				{
					_hash_wrtbuf(rel, nbuf);
					_hash_squeezebucket(rel, obucket, start_oblkno);
					return;
				}

				/*
				 * re-init. again, we're guaranteed that an ovfl page has
				 * at least one tuple.
				 */
				obuf = _hash_getbuf(rel, oblkno, HASH_WRITE);
				opage = BufferGetPage(obuf);
				_hash_checkpage(opage, LH_OVERFLOW_PAGE);
				oopaque = (HashPageOpaque) PageGetSpecialPointer(opage);
				if (PageIsEmpty(opage))
					elog(ERROR, "empty hash overflow page %u", oblkno);
				ooffnum = FirstOffsetNumber;
				omaxoffnum = PageGetMaxOffsetNumber(opage);
			}
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
	/* NOTREACHED */
}
