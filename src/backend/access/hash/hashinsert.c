/*-------------------------------------------------------------------------
 *
 * hashinsert.c
 *	  Item insertion in hash tables for Postgres.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/hash/hashinsert.c,v 1.31 2003/09/25 06:57:56 petere Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/hash.h"
#include "storage/lmgr.h"


static OffsetNumber _hash_pgaddtup(Relation rel, Buffer buf,
								   Size itemsize, HashItem hitem);


/*
 *	_hash_doinsert() -- Handle insertion of a single HashItem in the table.
 *
 *		This routine is called by the public interface routines, hashbuild
 *		and hashinsert.  By here, hashitem is completely filled in.
 *		The datum to be used as a "key" is in the hashitem.
 */
InsertIndexResult
_hash_doinsert(Relation rel, HashItem hitem)
{
	Buffer		buf;
	Buffer		metabuf;
	HashMetaPage metap;
	IndexTuple	itup;
	BlockNumber itup_blkno;
	OffsetNumber itup_off;
	InsertIndexResult res;
	BlockNumber blkno;
	Page		page;
	HashPageOpaque pageopaque;
	Size		itemsz;
	bool		do_expand;
	uint32		hashkey;
	Bucket		bucket;
	Datum		datum;
	bool		isnull;

	/*
	 * Compute the hash key for the item.  We do this first so as not to
	 * need to hold any locks while running the hash function.
	 */
	itup = &(hitem->hash_itup);
	if (rel->rd_rel->relnatts != 1)
		elog(ERROR, "hash indexes support only one index key");
	datum = index_getattr(itup, 1, RelationGetDescr(rel), &isnull);
	Assert(!isnull);
	hashkey = _hash_datum2hashkey(rel, datum);

	/* compute item size too */
	itemsz = IndexTupleDSize(hitem->hash_itup)
		+ (sizeof(HashItemData) - sizeof(IndexTupleData));

	itemsz = MAXALIGN(itemsz);	/* be safe, PageAddItem will do this but
								 * we need to be consistent */

	/*
	 * Acquire shared split lock so we can compute the target bucket
	 * safely (see README).
	 */
	_hash_getlock(rel, 0, HASH_SHARE);

	/* Read the metapage */
	metabuf = _hash_getbuf(rel, HASH_METAPAGE, HASH_READ);
	metap = (HashMetaPage) BufferGetPage(metabuf);
	_hash_checkpage(rel, (Page) metap, LH_META_PAGE);

	/*
	 * Check whether the item can fit on a hash page at all. (Eventually,
	 * we ought to try to apply TOAST methods if not.)  Note that at this
	 * point, itemsz doesn't include the ItemId.
	 */
	if (itemsz > HashMaxItemSize((Page) metap))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("index row size %lu exceeds hash maximum %lu",
						(unsigned long) itemsz,
						(unsigned long) HashMaxItemSize((Page) metap))));

	/*
	 * Compute the target bucket number, and convert to block number.
	 */
	bucket = _hash_hashkey2bucket(hashkey,
								  metap->hashm_maxbucket,
								  metap->hashm_highmask,
								  metap->hashm_lowmask);

	blkno = BUCKET_TO_BLKNO(metap, bucket);

	/* release lock on metapage, but keep pin since we'll need it again */
	_hash_chgbufaccess(rel, metabuf, HASH_READ, HASH_NOLOCK);

	/*
	 * Acquire share lock on target bucket; then we can release split lock.
	 */
	_hash_getlock(rel, blkno, HASH_SHARE);

	_hash_droplock(rel, 0, HASH_SHARE);

	/* Fetch the primary bucket page for the bucket */
	buf = _hash_getbuf(rel, blkno, HASH_WRITE);
	page = BufferGetPage(buf);
	_hash_checkpage(rel, page, LH_BUCKET_PAGE);
	pageopaque = (HashPageOpaque) PageGetSpecialPointer(page);
	Assert(pageopaque->hasho_bucket == bucket);

	/* Do the insertion */
	while (PageGetFreeSpace(page) < itemsz)
	{
		/*
		 * no space on this page; check for an overflow page
		 */
		BlockNumber	nextblkno = pageopaque->hasho_nextblkno;

		if (BlockNumberIsValid(nextblkno))
		{
			/*
			 * ovfl page exists; go get it.  if it doesn't have room,
			 * we'll find out next pass through the loop test above.
			 */
			_hash_relbuf(rel, buf);
			buf = _hash_getbuf(rel, nextblkno, HASH_WRITE);
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
		_hash_checkpage(rel, page, LH_OVERFLOW_PAGE);
		pageopaque = (HashPageOpaque) PageGetSpecialPointer(page);
		Assert(pageopaque->hasho_bucket == bucket);
	}

	/* found page with enough space, so add the item here */
	itup_off = _hash_pgaddtup(rel, buf, itemsz, hitem);
	itup_blkno = BufferGetBlockNumber(buf);

	/* write and release the modified page */
	_hash_wrtbuf(rel, buf);

	/* We can drop the bucket lock now */
	_hash_droplock(rel, blkno, HASH_SHARE);

	/*
	 * Write-lock the metapage so we can increment the tuple count.
	 * After incrementing it, check to see if it's time for a split.
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

	/* Create the return data structure */
	res = (InsertIndexResult) palloc(sizeof(InsertIndexResultData));

	ItemPointerSet(&(res->pointerData), itup_blkno, itup_off);

	return res;
}

/*
 *	_hash_pgaddtup() -- add a tuple to a particular page in the index.
 *
 *		This routine adds the tuple to the page as requested; it does
 *		not write out the page.  It is an error to call pgaddtup() without
 *		a write lock and pin.
 */
static OffsetNumber
_hash_pgaddtup(Relation rel,
			   Buffer buf,
			   Size itemsize,
			   HashItem hitem)
{
	OffsetNumber itup_off;
	Page		page;

	page = BufferGetPage(buf);
	_hash_checkpage(rel, page, LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);

	itup_off = OffsetNumberNext(PageGetMaxOffsetNumber(page));
	if (PageAddItem(page, (Item) hitem, itemsize, itup_off, LP_USED)
		== InvalidOffsetNumber)
		elog(ERROR, "failed to add index item to \"%s\"",
			 RelationGetRelationName(rel));

	return itup_off;
}
