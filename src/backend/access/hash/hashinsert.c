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
 *	  $Header: /cvsroot/pgsql/src/backend/access/hash/hashinsert.c,v 1.28 2003/09/01 20:26:34 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/hash.h"

static InsertIndexResult _hash_insertonpg(Relation rel, Buffer buf, int keysz, ScanKey scankey, HashItem hitem, Buffer metabuf);
static OffsetNumber _hash_pgaddtup(Relation rel, Buffer buf, int keysz, ScanKey itup_scankey, Size itemsize, HashItem hitem);

/*
 *	_hash_doinsert() -- Handle insertion of a single HashItem in the table.
 *
 *		This routine is called by the public interface routines, hashbuild
 *		and hashinsert.  By here, hashitem is filled in, and has a unique
 *		(xid, seqno) pair. The datum to be used as a "key" is in the
 *		hashitem.
 */
InsertIndexResult
_hash_doinsert(Relation rel, HashItem hitem)
{
	Buffer		buf;
	Buffer		metabuf;
	BlockNumber blkno;
	HashMetaPage metap;
	IndexTuple	itup;
	InsertIndexResult res;
	ScanKey		itup_scankey;
	int			natts;
	Page		page;

	metabuf = _hash_getbuf(rel, HASH_METAPAGE, HASH_READ);
	metap = (HashMetaPage) BufferGetPage(metabuf);
	_hash_checkpage((Page) metap, LH_META_PAGE);

	/* we need a scan key to do our search, so build one */
	itup = &(hitem->hash_itup);
	if ((natts = rel->rd_rel->relnatts) != 1)
		elog(ERROR, "Hash indexes support only one index key");
	itup_scankey = _hash_mkscankey(rel, itup);

	/*
	 * find the first page in the bucket chain containing this key and
	 * place it in buf.  _hash_search obtains a read lock for us.
	 */
	_hash_search(rel, natts, itup_scankey, &buf, metap);
	page = BufferGetPage(buf);
	_hash_checkpage(page, LH_BUCKET_PAGE);

	/*
	 * trade in our read lock for a write lock so that we can do the
	 * insertion.
	 */
	blkno = BufferGetBlockNumber(buf);
	_hash_relbuf(rel, buf, HASH_READ);
	buf = _hash_getbuf(rel, blkno, HASH_WRITE);


	/*
	 * XXX btree comment (haven't decided what to do in hash): don't think
	 * the bucket can be split while we're reading the metapage.
	 *
	 * If the page was split between the time that we surrendered our read
	 * lock and acquired our write lock, then this page may no longer be
	 * the right place for the key we want to insert.
	 */

	/* do the insertion */
	res = _hash_insertonpg(rel, buf, natts, itup_scankey,
						   hitem, metabuf);

	/* be tidy */
	_hash_freeskey(itup_scankey);

	return res;
}

/*
 *	_hash_insertonpg() -- Insert a tuple on a particular page in the table.
 *
 *		This recursive procedure does the following things:
 *
 *			+  if necessary, splits the target page.
 *			+  inserts the tuple.
 *
 *		On entry, we must have the right buffer on which to do the
 *		insertion, and the buffer must be pinned and locked.  On return,
 *		we will have dropped both the pin and the write lock on the buffer.
 *
 */
static InsertIndexResult
_hash_insertonpg(Relation rel,
				 Buffer buf,
				 int keysz,
				 ScanKey scankey,
				 HashItem hitem,
				 Buffer metabuf)
{
	InsertIndexResult res;
	Page		page;
	BlockNumber itup_blkno;
	OffsetNumber itup_off;
	Size		itemsz;
	HashPageOpaque pageopaque;
	bool		do_expand = false;
	Buffer		ovflbuf;
	HashMetaPage metap;
	Bucket		bucket;

	metap = (HashMetaPage) BufferGetPage(metabuf);
	_hash_checkpage((Page) metap, LH_META_PAGE);

	page = BufferGetPage(buf);
	_hash_checkpage(page, LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
	pageopaque = (HashPageOpaque) PageGetSpecialPointer(page);
	bucket = pageopaque->hasho_bucket;

	itemsz = IndexTupleDSize(hitem->hash_itup)
		+ (sizeof(HashItemData) - sizeof(IndexTupleData));
	itemsz = MAXALIGN(itemsz);

	while (PageGetFreeSpace(page) < itemsz)
	{
		/*
		 * no space on this page; check for an overflow page
		 */
		if (BlockNumberIsValid(pageopaque->hasho_nextblkno))
		{
			/*
			 * ovfl page exists; go get it.  if it doesn't have room,
			 * we'll find out next pass through the loop test above.
			 */
			ovflbuf = _hash_getbuf(rel, pageopaque->hasho_nextblkno,
								   HASH_WRITE);
			_hash_relbuf(rel, buf, HASH_WRITE);
			buf = ovflbuf;
			page = BufferGetPage(buf);
		}
		else
		{
			/*
			 * we're at the end of the bucket chain and we haven't found a
			 * page with enough room.  allocate a new overflow page.
			 */
			do_expand = true;
			ovflbuf = _hash_addovflpage(rel, metabuf, buf);
			_hash_relbuf(rel, buf, HASH_WRITE);
			buf = ovflbuf;
			page = BufferGetPage(buf);

			if (PageGetFreeSpace(page) < itemsz)
			{
				/* it doesn't fit on an empty page -- give up */
				elog(ERROR, "hash item too large");
			}
		}
		_hash_checkpage(page, LH_OVERFLOW_PAGE);
		pageopaque = (HashPageOpaque) PageGetSpecialPointer(page);
		Assert(pageopaque->hasho_bucket == bucket);
	}

	itup_off = _hash_pgaddtup(rel, buf, keysz, scankey, itemsz, hitem);
	itup_blkno = BufferGetBlockNumber(buf);

	/* by here, the new tuple is inserted */
	res = (InsertIndexResult) palloc(sizeof(InsertIndexResultData));

	ItemPointerSet(&(res->pointerData), itup_blkno, itup_off);

	if (res != NULL)
	{
		/*
		 * Increment the number of keys in the table. We switch lock
		 * access type just for a moment to allow greater accessibility to
		 * the metapage.
		 */
		_hash_chgbufaccess(rel, metabuf, HASH_READ, HASH_WRITE);
		metap->hashm_ntuples += 1;
		_hash_chgbufaccess(rel, metabuf, HASH_WRITE, HASH_READ);
	}

	_hash_wrtbuf(rel, buf);

	if (do_expand ||
		(metap->hashm_ntuples / (metap->hashm_maxbucket + 1))
		> metap->hashm_ffactor)
		_hash_expandtable(rel, metabuf);
	_hash_relbuf(rel, metabuf, HASH_READ);
	return res;
}

/*
 *	_hash_pgaddtup() -- add a tuple to a particular page in the index.
 *
 *		This routine adds the tuple to the page as requested, and keeps the
 *		write lock and reference associated with the page's buffer.  It is
 *		an error to call pgaddtup() without a write lock and reference.
 */
static OffsetNumber
_hash_pgaddtup(Relation rel,
			   Buffer buf,
			   int keysz,
			   ScanKey itup_scankey,
			   Size itemsize,
			   HashItem hitem)
{
	OffsetNumber itup_off;
	Page		page;

	page = BufferGetPage(buf);
	_hash_checkpage(page, LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);

	itup_off = OffsetNumberNext(PageGetMaxOffsetNumber(page));
	if (PageAddItem(page, (Item) hitem, itemsize, itup_off, LP_USED)
		== InvalidOffsetNumber)
		elog(ERROR, "failed to add index item to \"%s\"",
			 RelationGetRelationName(rel));

	/* write the buffer, but hold our lock */
	_hash_wrtnorelbuf(buf);

	return itup_off;
}
