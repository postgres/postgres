/*-------------------------------------------------------------------------
 *
 * hashinsert.c
 *	  Item insertion in hash tables for Postgres.
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/hash/hashinsert.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/hash.h"
#include "utils/rel.h"


/*
 *	_hash_doinsert() -- Handle insertion of a single index tuple.
 *
 *		This routine is called by the public interface routines, hashbuild
 *		and hashinsert.  By here, itup is completely filled in.
 */
void
_hash_doinsert(Relation rel, IndexTuple itup)
{
	Buffer		buf;
	Buffer		metabuf;
	HashMetaPage metap;
	BlockNumber blkno;
	BlockNumber oldblkno = InvalidBlockNumber;
	bool		retry = false;
	Page		page;
	HashPageOpaque pageopaque;
	Size		itemsz;
	bool		do_expand;
	uint32		hashkey;
	Bucket		bucket;

	/*
	 * Get the hash key for the item (it's stored in the index tuple itself).
	 */
	hashkey = _hash_get_indextuple_hashkey(itup);

	/* compute item size too */
	itemsz = IndexTupleDSize(*itup);
	itemsz = MAXALIGN(itemsz);	/* be safe, PageAddItem will do this but we
								 * need to be consistent */

	/* Read the metapage */
	metabuf = _hash_getbuf(rel, HASH_METAPAGE, HASH_READ, LH_META_PAGE);
	metap = HashPageGetMeta(BufferGetPage(metabuf));

	/*
	 * Check whether the item can fit on a hash page at all. (Eventually, we
	 * ought to try to apply TOAST methods if not.)  Note that at this point,
	 * itemsz doesn't include the ItemId.
	 *
	 * XXX this is useless code if we are only storing hash keys.
	 */
	if (itemsz > HashMaxItemSize((Page) metap))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("index row size %zu exceeds hash maximum %zu",
						itemsz, HashMaxItemSize((Page) metap)),
			errhint("Values larger than a buffer page cannot be indexed.")));

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
		_hash_chgbufaccess(rel, metabuf, HASH_READ, HASH_NOLOCK);

		/*
		 * If the previous iteration of this loop locked what is still the
		 * correct target bucket, we are done.  Otherwise, drop any old lock
		 * and lock what now appears to be the correct bucket.
		 */
		if (retry)
		{
			if (oldblkno == blkno)
				break;
			_hash_droplock(rel, oldblkno, HASH_SHARE);
		}
		_hash_getlock(rel, blkno, HASH_SHARE);

		/*
		 * Reacquire metapage lock and check that no bucket split has taken
		 * place while we were awaiting the bucket lock.
		 */
		_hash_chgbufaccess(rel, metabuf, HASH_NOLOCK, HASH_READ);
		oldblkno = blkno;
		retry = true;
	}

	/* Fetch the primary bucket page for the bucket */
	buf = _hash_getbuf(rel, blkno, HASH_WRITE, LH_BUCKET_PAGE);
	page = BufferGetPage(buf);
	pageopaque = (HashPageOpaque) PageGetSpecialPointer(page);
	Assert(pageopaque->hasho_bucket == bucket);

	/* Do the insertion */
	while (PageGetFreeSpace(page) < itemsz)
	{
		/*
		 * no space on this page; check for an overflow page
		 */
		BlockNumber nextblkno = pageopaque->hasho_nextblkno;

		if (BlockNumberIsValid(nextblkno))
		{
			/*
			 * ovfl page exists; go get it.  if it doesn't have room, we'll
			 * find out next pass through the loop test above.
			 */
			_hash_relbuf(rel, buf);
			buf = _hash_getbuf(rel, nextblkno, HASH_WRITE, LH_OVERFLOW_PAGE);
			page = BufferGetPage(buf);
		}
		else
		{
			/*
			 * we're at the end of the bucket chain and we haven't found a
			 * page with enough room.  allocate a new overflow page.
			 */

			/* release our write lock without modifying buffer */
			_hash_chgbufaccess(rel, buf, HASH_READ, HASH_NOLOCK);

			/* chain to a new overflow page */
			buf = _hash_addovflpage(rel, metabuf, buf);
			page = BufferGetPage(buf);

			/* should fit now, given test above */
			Assert(PageGetFreeSpace(page) >= itemsz);
		}
		pageopaque = (HashPageOpaque) PageGetSpecialPointer(page);
		Assert(pageopaque->hasho_flag == LH_OVERFLOW_PAGE);
		Assert(pageopaque->hasho_bucket == bucket);
	}

	/* found page with enough space, so add the item here */
	(void) _hash_pgaddtup(rel, buf, itemsz, itup);

	/* write and release the modified page */
	_hash_wrtbuf(rel, buf);

	/* We can drop the bucket lock now */
	_hash_droplock(rel, blkno, HASH_SHARE);

	/*
	 * Write-lock the metapage so we can increment the tuple count. After
	 * incrementing it, check to see if it's time for a split.
	 */
	_hash_chgbufaccess(rel, metabuf, HASH_NOLOCK, HASH_WRITE);

	metap->hashm_ntuples += 1;

	/* Make sure this stays in sync with _hash_expandtable() */
	do_expand = metap->hashm_ntuples >
		(double) metap->hashm_ffactor * (metap->hashm_maxbucket + 1);

	/* Write out the metapage and drop lock, but keep pin */
	_hash_chgbufaccess(rel, metabuf, HASH_WRITE, HASH_NOLOCK);

	/* Attempt to split if a split is needed */
	if (do_expand)
		_hash_expandtable(rel, metabuf);

	/* Finally drop our pin on the metapage */
	_hash_dropbuf(rel, metabuf);
}

/*
 *	_hash_pgaddtup() -- add a tuple to a particular page in the index.
 *
 * This routine adds the tuple to the page as requested; it does not write out
 * the page.  It is an error to call pgaddtup() without pin and write lock on
 * the target buffer.
 *
 * Returns the offset number at which the tuple was inserted.  This function
 * is responsible for preserving the condition that tuples in a hash index
 * page are sorted by hashkey value.
 */
OffsetNumber
_hash_pgaddtup(Relation rel, Buffer buf, Size itemsize, IndexTuple itup)
{
	OffsetNumber itup_off;
	Page		page;
	uint32		hashkey;

	_hash_checkpage(rel, buf, LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
	page = BufferGetPage(buf);

	/* Find where to insert the tuple (preserving page's hashkey ordering) */
	hashkey = _hash_get_indextuple_hashkey(itup);
	itup_off = _hash_binsearch(page, hashkey);

	if (PageAddItem(page, (Item) itup, itemsize, itup_off, false, false)
		== InvalidOffsetNumber)
		elog(ERROR, "failed to add index item to \"%s\"",
			 RelationGetRelationName(rel));

	return itup_off;
}
