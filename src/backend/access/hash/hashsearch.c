/*-------------------------------------------------------------------------
 *
 * hashsearch.c--
 *	  search code for postgres hash tables
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/hash/hashsearch.c,v 1.14 1997/09/08 21:40:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <access/hash.h>
#include <storage/bufmgr.h>

#ifndef HAVE_MEMMOVE
#include "regex/utils.h"
#else
#include <string.h>
#endif

/*
 *	_hash_search() -- Finds the page/bucket that the contains the
 *	scankey and loads it into *bufP.  the buffer has a read lock.
 */
void
_hash_search(Relation rel,
			 int keysz,
			 ScanKey scankey,
			 Buffer *bufP,
			 HashMetaPage metap)
{
	BlockNumber blkno;
	Datum		keyDatum;
	Bucket		bucket;

	if (scankey == (ScanKey) NULL ||
		(keyDatum = scankey[0].sk_argument) == (Datum) NULL)
	{

		/*
		 * If the scankey argument is NULL, all tuples will satisfy the
		 * scan so we start the scan at the first bucket (bucket 0).
		 */
		bucket = 0;
	}
	else
	{
		bucket = _hash_call(rel, metap, keyDatum);
	}

	blkno = BUCKET_TO_BLKNO(bucket);

	*bufP = _hash_getbuf(rel, blkno, HASH_READ);
}

/*
 *	_hash_next() -- Get the next item in a scan.
 *
 *		On entry, we have a valid currentItemData in the scan, and a
 *		read lock on the page that contains that item.	We do not have
 *		the page pinned.  We return the next item in the scan.	On
 *		exit, we have the page containing the next item locked but not
 *		pinned.
 */
RetrieveIndexResult
_hash_next(IndexScanDesc scan, ScanDirection dir)
{
	Relation	rel;
	Buffer		buf;
	Buffer		metabuf;
	Page		page;
	OffsetNumber offnum;
	RetrieveIndexResult res;
	ItemPointer current;
	HashItem	hitem;
	IndexTuple	itup;
	HashScanOpaque so;

	rel = scan->relation;
	so = (HashScanOpaque) scan->opaque;
	current = &(scan->currentItemData);

	metabuf = _hash_getbuf(rel, HASH_METAPAGE, HASH_READ);

	/*
	 * XXX 10 may 91:  somewhere there's a bug in our management of the
	 * cached buffer for this scan.  wei discovered it.  the following is
	 * a workaround so he can work until i figure out what's going on.
	 */

	if (!BufferIsValid(so->hashso_curbuf))
	{
		so->hashso_curbuf = _hash_getbuf(rel,
									  ItemPointerGetBlockNumber(current),
										 HASH_READ);
	}

	/* we still have the buffer pinned and locked */
	buf = so->hashso_curbuf;

	/*
	 * step to next valid tuple.  note that _hash_step releases our lock
	 * on 'metabuf'; if we switch to a new 'buf' while looking for the
	 * next tuple, we come back with a lock on that buffer.
	 */
	if (!_hash_step(scan, &buf, dir, metabuf))
	{
		return ((RetrieveIndexResult) NULL);
	}

	/* if we're here, _hash_step found a valid tuple */
	current = &(scan->currentItemData);
	offnum = ItemPointerGetOffsetNumber(current);
	page = BufferGetPage(buf);
	_hash_checkpage(page, LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
	hitem = (HashItem) PageGetItem(page, PageGetItemId(page, offnum));
	itup = &hitem->hash_itup;
	res = FormRetrieveIndexResult(current, &(itup->t_tid));

	return (res);
}

static void
_hash_readnext(Relation rel,
			   Buffer *bufp, Page *pagep, HashPageOpaque *opaquep)
{
	BlockNumber blkno;

	blkno = (*opaquep)->hasho_nextblkno;
	_hash_relbuf(rel, *bufp, HASH_READ);
	*bufp = InvalidBuffer;
	if (BlockNumberIsValid(blkno))
	{
		*bufp = _hash_getbuf(rel, blkno, HASH_READ);
		*pagep = BufferGetPage(*bufp);
		_hash_checkpage(*pagep, LH_OVERFLOW_PAGE);
		*opaquep = (HashPageOpaque) PageGetSpecialPointer(*pagep);
		Assert(!PageIsEmpty(*pagep));
	}
}

static void
_hash_readprev(Relation rel,
			   Buffer *bufp, Page *pagep, HashPageOpaque *opaquep)
{
	BlockNumber blkno;

	blkno = (*opaquep)->hasho_prevblkno;
	_hash_relbuf(rel, *bufp, HASH_READ);
	*bufp = InvalidBuffer;
	if (BlockNumberIsValid(blkno))
	{
		*bufp = _hash_getbuf(rel, blkno, HASH_READ);
		*pagep = BufferGetPage(*bufp);
		_hash_checkpage(*pagep, LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
		*opaquep = (HashPageOpaque) PageGetSpecialPointer(*pagep);
		if (PageIsEmpty(*pagep))
		{
			Assert((*opaquep)->hasho_flag & LH_BUCKET_PAGE);
			_hash_relbuf(rel, *bufp, HASH_READ);
			*bufp = InvalidBuffer;
		}
	}
}

/*
 *	_hash_first() -- Find the first item in a scan.
 *
 *		Return the RetrieveIndexResult of the first item in the tree that
 *		satisfies the qualificatin associated with the scan descriptor. On
 *		exit, the page containing the current index tuple is read locked
 *		and pinned, and the scan's opaque data entry is updated to
 *		include the buffer.
 */
RetrieveIndexResult
_hash_first(IndexScanDesc scan, ScanDirection dir)
{
	Relation	rel;
	Buffer		buf;
	Buffer		metabuf;
	Page		page;
	HashPageOpaque opaque;
	HashMetaPage metap;
	HashItem	hitem;
	IndexTuple	itup;
	ItemPointer current;
	OffsetNumber offnum;
	RetrieveIndexResult res;
	HashScanOpaque so;

	rel = scan->relation;
	so = (HashScanOpaque) scan->opaque;
	current = &(scan->currentItemData);

	metabuf = _hash_getbuf(rel, HASH_METAPAGE, HASH_READ);
	metap = (HashMetaPage) BufferGetPage(metabuf);
	_hash_checkpage((Page) metap, LH_META_PAGE);

	/*
	 * XXX -- The attribute number stored in the scan key is the attno in
	 * the heap relation.  We need to transmogrify this into the index
	 * relation attno here.  For the moment, we have hardwired attno == 1.
	 */

	/* find the correct bucket page and load it into buf */
	_hash_search(rel, 1, scan->keyData, &buf, metap);
	page = BufferGetPage(buf);
	_hash_checkpage(page, LH_BUCKET_PAGE);
	opaque = (HashPageOpaque) PageGetSpecialPointer(page);

	/*
	 * if we are scanning forward, we need to find the first non-empty
	 * page (if any) in the bucket chain.  since overflow pages are never
	 * empty, this had better be either the bucket page or the first
	 * overflow page.
	 *
	 * if we are scanning backward, we always go all the way to the end of
	 * the bucket chain.
	 */
	if (PageIsEmpty(page))
	{
		if (BlockNumberIsValid(opaque->hasho_nextblkno))
		{
			_hash_readnext(rel, &buf, &page, &opaque);
		}
		else
		{
			ItemPointerSetInvalid(current);
			so->hashso_curbuf = InvalidBuffer;

			/*
			 * If there is no scankeys, all tuples will satisfy the scan -
			 * so we continue in _hash_step to get tuples from all
			 * buckets. - vadim 04/29/97
			 */
			if (scan->numberOfKeys >= 1)
			{
				_hash_relbuf(rel, buf, HASH_READ);
				_hash_relbuf(rel, metabuf, HASH_READ);
				return ((RetrieveIndexResult) NULL);
			}
		}
	}
	if (ScanDirectionIsBackward(dir))
	{
		while (BlockNumberIsValid(opaque->hasho_nextblkno))
		{
			_hash_readnext(rel, &buf, &page, &opaque);
		}
	}

	if (!_hash_step(scan, &buf, dir, metabuf))
	{
		return ((RetrieveIndexResult) NULL);
	}

	/* if we're here, _hash_step found a valid tuple */
	current = &(scan->currentItemData);
	offnum = ItemPointerGetOffsetNumber(current);
	page = BufferGetPage(buf);
	_hash_checkpage(page, LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
	hitem = (HashItem) PageGetItem(page, PageGetItemId(page, offnum));
	itup = &hitem->hash_itup;
	res = FormRetrieveIndexResult(current, &(itup->t_tid));

	return (res);
}

/*
 *	_hash_step() -- step to the next valid item in a scan in the bucket.
 *
 *		If no valid record exists in the requested direction, return
 *		false.	Else, return true and set the CurrentItemData for the
 *		scan to the right thing.
 *
 *		'bufP' points to the buffer which contains the current page
 *		that we'll step through.
 *
 *		'metabuf' is released when this returns.
 */
bool
_hash_step(IndexScanDesc scan, Buffer *bufP, ScanDirection dir, Buffer metabuf)
{
	Relation	rel;
	ItemPointer current;
	HashScanOpaque so;
	int			allbuckets;
	HashMetaPage metap;
	Buffer		buf;
	Page		page;
	HashPageOpaque opaque;
	OffsetNumber maxoff;
	OffsetNumber offnum;
	Bucket		bucket;
	BlockNumber blkno;
	HashItem	hitem;
	IndexTuple	itup;

	rel = scan->relation;
	current = &(scan->currentItemData);
	so = (HashScanOpaque) scan->opaque;
	allbuckets = (scan->numberOfKeys < 1);

	metap = (HashMetaPage) BufferGetPage(metabuf);
	_hash_checkpage((Page) metap, LH_META_PAGE);

	buf = *bufP;
	page = BufferGetPage(buf);
	_hash_checkpage(page, LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
	opaque = (HashPageOpaque) PageGetSpecialPointer(page);

	/*
	 * If _hash_step is called from _hash_first, current will not be
	 * valid, so we can't dereference it.  However, in that case, we
	 * presumably want to start at the beginning/end of the page...
	 */
	maxoff = PageGetMaxOffsetNumber(page);
	if (ItemPointerIsValid(current))
	{
		offnum = ItemPointerGetOffsetNumber(current);
	}
	else
	{
		offnum = InvalidOffsetNumber;
	}

	/*
	 * 'offnum' now points to the last tuple we have seen (if any).
	 *
	 * continue to step through tuples until: 1) we get to the end of the
	 * bucket chain or 2) we find a valid tuple.
	 */
	do
	{
		bucket = opaque->hasho_bucket;

		switch (dir)
		{
			case ForwardScanDirection:
				if (offnum != InvalidOffsetNumber)
				{
					offnum = OffsetNumberNext(offnum);	/* move forward */
				}
				else
				{
					offnum = FirstOffsetNumber; /* new page */
				}
				while (offnum > maxoff)
				{

					/*
					 * either this page is empty (maxoff ==
					 * InvalidOffsetNumber) or we ran off the end.
					 */
					_hash_readnext(rel, &buf, &page, &opaque);
					if (BufferIsInvalid(buf))
					{			/* end of chain */
						if (allbuckets && bucket < metap->hashm_maxbucket)
						{
							++bucket;
							blkno = BUCKET_TO_BLKNO(bucket);
							buf = _hash_getbuf(rel, blkno, HASH_READ);
							page = BufferGetPage(buf);
							_hash_checkpage(page, LH_BUCKET_PAGE);
							opaque = (HashPageOpaque) PageGetSpecialPointer(page);
							Assert(opaque->hasho_bucket == bucket);
							while (PageIsEmpty(page) &&
							 BlockNumberIsValid(opaque->hasho_nextblkno))
							{
								_hash_readnext(rel, &buf, &page, &opaque);
							}
							maxoff = PageGetMaxOffsetNumber(page);
							offnum = FirstOffsetNumber;
						}
						else
						{
							maxoff = offnum = InvalidOffsetNumber;
							break;		/* while */
						}
					}
					else
					{
						/* _hash_readnext never returns an empty page */
						maxoff = PageGetMaxOffsetNumber(page);
						offnum = FirstOffsetNumber;
					}
				}
				break;
			case BackwardScanDirection:
				if (offnum != InvalidOffsetNumber)
				{
					offnum = OffsetNumberPrev(offnum);	/* move back */
				}
				else
				{
					offnum = maxoff;	/* new page */
				}
				while (offnum < FirstOffsetNumber)
				{

					/*
					 * either this page is empty (offnum ==
					 * InvalidOffsetNumber) or we ran off the end.
					 */
					_hash_readprev(rel, &buf, &page, &opaque);
					if (BufferIsInvalid(buf))
					{			/* end of chain */
						if (allbuckets && bucket > 0)
						{
							--bucket;
							blkno = BUCKET_TO_BLKNO(bucket);
							buf = _hash_getbuf(rel, blkno, HASH_READ);
							page = BufferGetPage(buf);
							_hash_checkpage(page, LH_BUCKET_PAGE);
							opaque = (HashPageOpaque) PageGetSpecialPointer(page);
							Assert(opaque->hasho_bucket == bucket);
							while (BlockNumberIsValid(opaque->hasho_nextblkno))
							{
								_hash_readnext(rel, &buf, &page, &opaque);
							}
							maxoff = offnum = PageGetMaxOffsetNumber(page);
						}
						else
						{
							maxoff = offnum = InvalidOffsetNumber;
							break;		/* while */
						}
					}
					else
					{
						/* _hash_readprev never returns an empty page */
						maxoff = offnum = PageGetMaxOffsetNumber(page);
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
			_hash_relbuf(rel, metabuf, HASH_READ);
			*bufP = so->hashso_curbuf = InvalidBuffer;
			ItemPointerSetInvalid(current);
			return (false);
		}

		/* get ready to check this tuple */
		hitem = (HashItem) PageGetItem(page, PageGetItemId(page, offnum));
		itup = &hitem->hash_itup;
	} while (!_hash_checkqual(scan, itup));

	/* if we made it to here, we've found a valid tuple */
	_hash_relbuf(rel, metabuf, HASH_READ);
	blkno = BufferGetBlockNumber(buf);
	*bufP = so->hashso_curbuf = buf;
	ItemPointerSet(current, blkno, offnum);
	return (true);
}
