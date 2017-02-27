/*-------------------------------------------------------------------------
 *
 * hashinsert.c
 *	  Item insertion in hash tables for Postgres.
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
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
	Buffer		buf = InvalidBuffer;
	Buffer		bucket_buf;
	Buffer		metabuf;
	HashMetaPage metap;
	HashMetaPage usedmetap = NULL;
	Page		metapage;
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

restart_insert:

	/*
	 * Read the metapage.  We don't lock it yet; HashMaxItemSize() will
	 * examine pd_pagesize_version, but that can't change so we can examine
	 * it without a lock.
	 */
	metabuf = _hash_getbuf(rel, HASH_METAPAGE, HASH_NOLOCK, LH_META_PAGE);
	metapage = BufferGetPage(metabuf);

	/*
	 * Check whether the item can fit on a hash page at all. (Eventually, we
	 * ought to try to apply TOAST methods if not.)  Note that at this point,
	 * itemsz doesn't include the ItemId.
	 *
	 * XXX this is useless code if we are only storing hash keys.
	 */
	if (itemsz > HashMaxItemSize(metapage))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("index row size %zu exceeds hash maximum %zu",
						itemsz, HashMaxItemSize(metapage)),
			errhint("Values larger than a buffer page cannot be indexed.")));

	/* Lock the primary bucket page for the target bucket. */
	buf = _hash_getbucketbuf_from_hashkey(rel, hashkey, HASH_WRITE,
										  &usedmetap);
	Assert(usedmetap != NULL);

	/* remember the primary bucket buffer to release the pin on it at end. */
	bucket_buf = buf;

	page = BufferGetPage(buf);
	pageopaque = (HashPageOpaque) PageGetSpecialPointer(page);
	bucket = pageopaque->hasho_bucket;

	/*
	 * If this bucket is in the process of being split, try to finish the
	 * split before inserting, because that might create room for the
	 * insertion to proceed without allocating an additional overflow page.
	 * It's only interesting to finish the split if we're trying to insert
	 * into the bucket from which we're removing tuples (the "old" bucket),
	 * not if we're trying to insert into the bucket into which tuples are
	 * being moved (the "new" bucket).
	 */
	if (H_BUCKET_BEING_SPLIT(pageopaque) && IsBufferCleanupOK(buf))
	{
		/* release the lock on bucket buffer, before completing the split. */
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);

		_hash_finish_split(rel, metabuf, buf, bucket,
						   usedmetap->hashm_maxbucket,
						   usedmetap->hashm_highmask,
						   usedmetap->hashm_lowmask);

		/* release the pin on old and meta buffer.  retry for insert. */
		_hash_dropbuf(rel, buf);
		_hash_dropbuf(rel, metabuf);
		goto restart_insert;
	}

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
			 * find out next pass through the loop test above.  we always
			 * release both the lock and pin if this is an overflow page, but
			 * only the lock if this is the primary bucket page, since the pin
			 * on the primary bucket must be retained throughout the scan.
			 */
			if (buf != bucket_buf)
				_hash_relbuf(rel, buf);
			else
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);
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
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);

			/* chain to a new overflow page */
			buf = _hash_addovflpage(rel, metabuf, buf, (buf == bucket_buf) ? true : false);
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

	/*
	 * dirty and release the modified page.  if the page we modified was an
	 * overflow page, we also need to separately drop the pin we retained on
	 * the primary bucket page.
	 */
	MarkBufferDirty(buf);
	_hash_relbuf(rel, buf);
	if (buf != bucket_buf)
		_hash_dropbuf(rel, bucket_buf);

	/*
	 * Write-lock the metapage so we can increment the tuple count. After
	 * incrementing it, check to see if it's time for a split.
	 */
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

	metap = HashPageGetMeta(metapage);
	metap->hashm_ntuples += 1;

	/* Make sure this stays in sync with _hash_expandtable() */
	do_expand = metap->hashm_ntuples >
		(double) metap->hashm_ffactor * (metap->hashm_maxbucket + 1);

	/* Write out the metapage and drop lock, but keep pin */
	MarkBufferDirty(metabuf);
	LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);

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

/*
 *	_hash_pgaddmultitup() -- add a tuple vector to a particular page in the
 *							 index.
 *
 * This routine has same requirements for locking and tuple ordering as
 * _hash_pgaddtup().
 *
 * Returns the offset number array at which the tuples were inserted.
 */
void
_hash_pgaddmultitup(Relation rel, Buffer buf, IndexTuple *itups,
					OffsetNumber *itup_offsets, uint16 nitups)
{
	OffsetNumber itup_off;
	Page		page;
	uint32		hashkey;
	int			i;

	_hash_checkpage(rel, buf, LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
	page = BufferGetPage(buf);

	for (i = 0; i < nitups; i++)
	{
		Size		itemsize;

		itemsize = IndexTupleDSize(*itups[i]);
		itemsize = MAXALIGN(itemsize);

		/* Find where to insert the tuple (preserving page's hashkey ordering) */
		hashkey = _hash_get_indextuple_hashkey(itups[i]);
		itup_off = _hash_binsearch(page, hashkey);

		itup_offsets[i] = itup_off;

		if (PageAddItem(page, (Item) itups[i], itemsize, itup_off, false, false)
			== InvalidOffsetNumber)
			elog(ERROR, "failed to add index item to \"%s\"",
				 RelationGetRelationName(rel));
	}
}
