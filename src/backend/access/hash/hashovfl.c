/*-------------------------------------------------------------------------
 *
 * hashovfl.c
 *	  Overflow page management code for the Postgres hash access method
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/hash/hashovfl.c,v 1.40 2003/09/02 18:13:30 tgl Exp $
 *
 * NOTES
 *	  Overflow pages look like ordinary relation pages.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"


static BlockNumber _hash_getovflpage(Relation rel, Buffer metabuf);
static uint32 _hash_firstfreebit(uint32 map);


/*
 * Convert overflow page bit number (its index in the free-page bitmaps)
 * to block number within the index.
 */
static BlockNumber
bitno_to_blkno(HashMetaPage metap, uint32 ovflbitnum)
{
	uint32		splitnum = metap->hashm_ovflpoint;
	uint32		i;

	/* Convert zero-based bitnumber to 1-based page number */
	ovflbitnum += 1;

	/* Determine the split number for this page (must be >= 1) */
	for (i = 1;
		 i < splitnum && ovflbitnum > metap->hashm_spares[i];
		 i++)
		/* loop */ ;

	/*
	 * Convert to absolute page number by adding the number of bucket pages
	 * that exist before this split point.
	 */
	return (BlockNumber) ((1 << i) + ovflbitnum);
}

/*
 * Convert overflow page block number to bit number for free-page bitmap.
 */
static uint32
blkno_to_bitno(HashMetaPage metap, BlockNumber ovflblkno)
{
	uint32		splitnum = metap->hashm_ovflpoint;
	uint32		i;
	uint32		bitnum;

	/* Determine the split number containing this page */
	for (i = 1; i <= splitnum; i++)
	{
		if (ovflblkno <= (BlockNumber) (1 << i))
			break;				/* oops */
		bitnum = ovflblkno - (1 << i);
		if (bitnum <= metap->hashm_spares[i])
			return bitnum - 1;	/* -1 to convert 1-based to 0-based */
	}

	elog(ERROR, "invalid overflow block number %u", ovflblkno);
	return 0;					/* keep compiler quiet */
}

/*
 *	_hash_addovflpage
 *
 *	Add an overflow page to the page currently pointed to by the buffer
 *	argument 'buf'.
 *
 *	metabuf has a read lock upon entering the function; buf has a
 *	write lock.  The same is true on exit.  The returned overflow page
 *	is write-locked.
 */
Buffer
_hash_addovflpage(Relation rel, Buffer metabuf, Buffer buf)
{
	BlockNumber ovflblkno;
	Buffer		ovflbuf;
	HashMetaPage metap;
	HashPageOpaque ovflopaque;
	HashPageOpaque pageopaque;
	Page		page;
	Page		ovflpage;

	/* this had better be the last page in a bucket chain */
	page = BufferGetPage(buf);
	_hash_checkpage(rel, page, LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
	pageopaque = (HashPageOpaque) PageGetSpecialPointer(page);
	Assert(!BlockNumberIsValid(pageopaque->hasho_nextblkno));

	metap = (HashMetaPage) BufferGetPage(metabuf);
	_hash_checkpage(rel, (Page) metap, LH_META_PAGE);

	/* allocate an empty overflow page */
	ovflblkno = _hash_getovflpage(rel, metabuf);
	ovflbuf = _hash_getbuf(rel, ovflblkno, HASH_WRITE);
	ovflpage = BufferGetPage(ovflbuf);

	/* initialize the new overflow page */
	_hash_pageinit(ovflpage, BufferGetPageSize(ovflbuf));
	ovflopaque = (HashPageOpaque) PageGetSpecialPointer(ovflpage);
	ovflopaque->hasho_prevblkno = BufferGetBlockNumber(buf);
	ovflopaque->hasho_nextblkno = InvalidBlockNumber;
	ovflopaque->hasho_bucket = pageopaque->hasho_bucket;
	ovflopaque->hasho_flag = LH_OVERFLOW_PAGE;
	ovflopaque->hasho_filler = HASHO_FILL;
	_hash_wrtnorelbuf(ovflbuf);

	/* logically chain overflow page to previous page */
	pageopaque->hasho_nextblkno = ovflblkno;
	_hash_wrtnorelbuf(buf);
	return ovflbuf;
}

/*
 *	_hash_getovflpage()
 *
 *	Find an available overflow page and return its block number.
 *
 *	When we enter this function, we have a read lock on metabuf which
 *	we change to a write lock immediately. Before exiting, the write lock
 *	is exchanged for a read lock.
 */
static BlockNumber
_hash_getovflpage(Relation rel, Buffer metabuf)
{
	HashMetaPage metap;
	Buffer		mapbuf = 0;
	BlockNumber blkno;
	uint32		splitnum;
	uint32	   *freep = NULL;
	uint32		max_ovflpg;
	uint32		bit;
	uint32		first_page;
	uint32		last_bit;
	uint32		last_page;
	uint32		i,
				j;

	_hash_chgbufaccess(rel, metabuf, HASH_READ, HASH_WRITE);
	metap = (HashMetaPage) BufferGetPage(metabuf);
	splitnum = metap->hashm_ovflpoint;

	/* end search with the last existing overflow page */
	max_ovflpg = metap->hashm_spares[splitnum] - 1;
	last_page = max_ovflpg >> BMPG_SHIFT(metap);
	last_bit = max_ovflpg & BMPG_MASK(metap);

	/* start search at hashm_firstfree */
	first_page = metap->hashm_firstfree >> BMPG_SHIFT(metap);
	bit = metap->hashm_firstfree & BMPG_MASK(metap);
	j = bit / BITS_PER_MAP;
	bit &= ~(BITS_PER_MAP - 1);

	for (i = first_page; i <= last_page; i++)
	{
		BlockNumber	mapblkno;
		Page		mappage;
		uint32		last_inpage;

		mapblkno = metap->hashm_mapp[i];
		mapbuf = _hash_getbuf(rel, mapblkno, HASH_WRITE);
		mappage = BufferGetPage(mapbuf);
		_hash_checkpage(rel, mappage, LH_BITMAP_PAGE);
		freep = HashPageGetBitmap(mappage);

		if (i != first_page)
		{
			bit = 0;
			j = 0;
		}

		if (i == last_page)
			last_inpage = last_bit;
		else
			last_inpage = BMPGSZ_BIT(metap) - 1;

		for (; bit <= last_inpage; j++, bit += BITS_PER_MAP)
		{
			if (freep[j] != ALL_SET)
				goto found;
		}

		_hash_relbuf(rel, mapbuf, HASH_WRITE);
	}

	/* No Free Page Found - have to allocate a new page */
	bit = metap->hashm_spares[splitnum];
	metap->hashm_spares[splitnum]++;

	/* Check if we need to allocate a new bitmap page */
	if (last_bit == (uint32) (BMPGSZ_BIT(metap) - 1))
	{
		/*
		 * We create the new bitmap page with all pages marked "in use".
		 * Actually two pages in the new bitmap's range will exist
		 * immediately: the bitmap page itself, and the following page
		 * which is the one we return to the caller.  Both of these are
		 * correctly marked "in use".  Subsequent pages do not exist yet,
		 * but it is convenient to pre-mark them as "in use" too.
		 */
		_hash_initbitmap(rel, metap, bitno_to_blkno(metap, bit));

		bit = metap->hashm_spares[splitnum];
		metap->hashm_spares[splitnum]++;
	}
	else
	{
		/*
		 * Nothing to do here; since the page was past the last used page,
		 * we know its bitmap bit was preinitialized to "in use".
		 */
	}

	/* mark new page as first free so we don't search much next time */
	metap->hashm_firstfree = bit;

	/* Calculate address of the new overflow page */
	blkno = bitno_to_blkno(metap, bit);

	_hash_chgbufaccess(rel, metabuf, HASH_WRITE, HASH_READ);

	return blkno;

found:
	/* convert bit to bit number within page */
	bit += _hash_firstfreebit(freep[j]);

	/* mark page "in use" */
	SETBIT(freep, bit);
	_hash_wrtbuf(rel, mapbuf);

	/* convert bit to absolute bit number */
	bit += (i << BMPG_SHIFT(metap));

	/* adjust hashm_firstfree to avoid redundant searches */
	if (bit > metap->hashm_firstfree)
		metap->hashm_firstfree = bit;

	blkno = bitno_to_blkno(metap, bit);

	_hash_chgbufaccess(rel, metabuf, HASH_WRITE, HASH_READ);

	return blkno;
}

/*
 *	_hash_firstfreebit()
 *
 *	Return the number of the first bit that is not set in the word 'map'.
 */
static uint32
_hash_firstfreebit(uint32 map)
{
	uint32		i,
				mask;

	mask = 0x1;
	for (i = 0; i < BITS_PER_MAP; i++)
	{
		if (!(mask & map))
			return i;
		mask <<= 1;
	}
	return i;
}

/*
 *	_hash_freeovflpage() -
 *
 *	Remove this overflow page from its bucket's chain, and mark the page as
 *	free.  On entry, ovflbuf is write-locked; it is released before exiting.
 *
 *	Returns the block number of the page that followed the given page
 *	in the bucket, or InvalidBlockNumber if no following page.
 *
 *	NB: caller must not hold lock on metapage.
 */
BlockNumber
_hash_freeovflpage(Relation rel, Buffer ovflbuf)
{
	HashMetaPage metap;
	Buffer		metabuf;
	Buffer		mapbuf;
	BlockNumber ovflblkno;
	BlockNumber prevblkno;
	BlockNumber blkno;
	BlockNumber nextblkno;
	HashPageOpaque ovflopaque;
	Page		ovflpage;
	Page		mappage;
	uint32	   *freep;
	uint32		ovflbitno;
	int32		bitmappage,
				bitmapbit;
	Bucket		bucket;

	metabuf = _hash_getbuf(rel, HASH_METAPAGE, HASH_WRITE);
	metap = (HashMetaPage) BufferGetPage(metabuf);
	_hash_checkpage(rel, (Page) metap, LH_META_PAGE);

	ovflblkno = BufferGetBlockNumber(ovflbuf);	
	ovflpage = BufferGetPage(ovflbuf);
	_hash_checkpage(rel, ovflpage, LH_OVERFLOW_PAGE);
	ovflopaque = (HashPageOpaque) PageGetSpecialPointer(ovflpage);
	nextblkno = ovflopaque->hasho_nextblkno;
	prevblkno = ovflopaque->hasho_prevblkno;
	bucket = ovflopaque->hasho_bucket;
	MemSet(ovflpage, 0, BufferGetPageSize(ovflbuf));
	_hash_wrtbuf(rel, ovflbuf);

	/*
	 * fix up the bucket chain.  this is a doubly-linked list, so we must
	 * fix up the bucket chain members behind and ahead of the overflow
	 * page being deleted.
	 *
	 * XXX this should look like: - lock prev/next - modify/write prev/next
	 * (how to do write ordering with a doubly-linked list?) - unlock
	 * prev/next
	 */
	if (BlockNumberIsValid(prevblkno))
	{
		Buffer		prevbuf = _hash_getbuf(rel, prevblkno, HASH_WRITE);
		Page		prevpage = BufferGetPage(prevbuf);
		HashPageOpaque prevopaque = (HashPageOpaque) PageGetSpecialPointer(prevpage);

		_hash_checkpage(rel, prevpage, LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
		Assert(prevopaque->hasho_bucket == bucket);
		prevopaque->hasho_nextblkno = nextblkno;
		_hash_wrtbuf(rel, prevbuf);
	}
	if (BlockNumberIsValid(nextblkno))
	{
		Buffer		nextbuf = _hash_getbuf(rel, nextblkno, HASH_WRITE);
		Page		nextpage = BufferGetPage(nextbuf);
		HashPageOpaque nextopaque = (HashPageOpaque) PageGetSpecialPointer(nextpage);

		_hash_checkpage(rel, nextpage, LH_OVERFLOW_PAGE);
		Assert(nextopaque->hasho_bucket == bucket);
		nextopaque->hasho_prevblkno = prevblkno;
		_hash_wrtbuf(rel, nextbuf);
	}

	/*
	 * Clear the bitmap bit to indicate that this overflow page is free.
	 */
	ovflbitno = blkno_to_bitno(metap, ovflblkno);

	bitmappage = ovflbitno >> BMPG_SHIFT(metap);
	bitmapbit = ovflbitno & BMPG_MASK(metap);

	if (bitmappage >= metap->hashm_nmaps)
		elog(ERROR, "invalid overflow bit number %u", ovflbitno);
	blkno = metap->hashm_mapp[bitmappage];

	mapbuf = _hash_getbuf(rel, blkno, HASH_WRITE);
	mappage = BufferGetPage(mapbuf);
	_hash_checkpage(rel, mappage, LH_BITMAP_PAGE);
	freep = HashPageGetBitmap(mappage);
	CLRBIT(freep, bitmapbit);
	_hash_wrtbuf(rel, mapbuf);

	/* if this is now the first free page, update hashm_firstfree */
	if (ovflbitno < metap->hashm_firstfree)
		metap->hashm_firstfree = ovflbitno;

	_hash_wrtbuf(rel, metabuf);

	return nextblkno;
}


/*
 *	_hash_initbitmap()
 *
 *	 Initialize a new bitmap page.	The metapage has a write-lock upon
 *	 entering the function, and must be written by caller after return.
 *
 * 'blkno' is the block number of the new bitmap page.
 *
 * All bits in the new bitmap page are set to "1", indicating "in use".
 */
void
_hash_initbitmap(Relation rel, HashMetaPage metap, BlockNumber blkno)
{
	Buffer		buf;
	Page		pg;
	HashPageOpaque op;
	uint32	   *freep;

	/* initialize the page */
	buf = _hash_getbuf(rel, blkno, HASH_WRITE);
	pg = BufferGetPage(buf);
	_hash_pageinit(pg, BufferGetPageSize(buf));
	op = (HashPageOpaque) PageGetSpecialPointer(pg);
	op->hasho_prevblkno = InvalidBlockNumber;
	op->hasho_nextblkno = InvalidBlockNumber;
	op->hasho_bucket = -1;
	op->hasho_flag = LH_BITMAP_PAGE;
	op->hasho_filler = HASHO_FILL;

	/* set all of the bits to 1 */
	freep = HashPageGetBitmap(pg);
	MemSet((char *) freep, 0xFF, BMPGSZ_BYTE(metap));

	/* write out the new bitmap page (releasing write lock) */
	_hash_wrtbuf(rel, buf);

	/* add the new bitmap page to the metapage's list of bitmaps */
	/* metapage already has a write lock */
	if (metap->hashm_nmaps >= HASH_MAX_BITMAPS)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("out of overflow pages in hash index \"%s\"",
						RelationGetRelationName(rel))));

	metap->hashm_mapp[metap->hashm_nmaps] = blkno;

	metap->hashm_nmaps++;
}


/*
 *	_hash_squeezebucket(rel, bucket)
 *
 *	Try to squeeze the tuples onto pages occurring earlier in the
 *	bucket chain in an attempt to free overflow pages. When we start
 *	the "squeezing", the page from which we start taking tuples (the
 *	"read" page) is the last bucket in the bucket chain and the page
 *	onto which we start squeezing tuples (the "write" page) is the
 *	first page in the bucket chain.  The read page works backward and
 *	the write page works forward; the procedure terminates when the
 *	read page and write page are the same page.
 *
 *	Caller must hold exclusive lock on the target bucket.
 */
void
_hash_squeezebucket(Relation rel,
					Bucket bucket,
					BlockNumber bucket_blkno)
{
	Buffer		wbuf;
	Buffer		rbuf = 0;
	BlockNumber wblkno;
	BlockNumber rblkno;
	Page		wpage;
	Page		rpage;
	HashPageOpaque wopaque;
	HashPageOpaque ropaque;
	OffsetNumber woffnum;
	OffsetNumber roffnum;
	HashItem	hitem;
	Size		itemsz;

	/*
	 * start squeezing into the base bucket page.
	 */
	wblkno = bucket_blkno;
	wbuf = _hash_getbuf(rel, wblkno, HASH_WRITE);
	wpage = BufferGetPage(wbuf);
	_hash_checkpage(rel, wpage, LH_BUCKET_PAGE);
	wopaque = (HashPageOpaque) PageGetSpecialPointer(wpage);

	/*
	 * if there aren't any overflow pages, there's nothing to squeeze.
	 */
	if (!BlockNumberIsValid(wopaque->hasho_nextblkno))
	{
		_hash_relbuf(rel, wbuf, HASH_WRITE);
		return;
	}

	/*
	 * find the last page in the bucket chain by starting at the base
	 * bucket page and working forward.
	 */
	ropaque = wopaque;
	do
	{
		rblkno = ropaque->hasho_nextblkno;
		if (ropaque != wopaque)
			_hash_relbuf(rel, rbuf, HASH_WRITE);
		rbuf = _hash_getbuf(rel, rblkno, HASH_WRITE);
		rpage = BufferGetPage(rbuf);
		_hash_checkpage(rel, rpage, LH_OVERFLOW_PAGE);
		Assert(!PageIsEmpty(rpage));
		ropaque = (HashPageOpaque) PageGetSpecialPointer(rpage);
		Assert(ropaque->hasho_bucket == bucket);
	} while (BlockNumberIsValid(ropaque->hasho_nextblkno));

	/*
	 * squeeze the tuples.
	 */
	roffnum = FirstOffsetNumber;
	for (;;)
	{
		hitem = (HashItem) PageGetItem(rpage, PageGetItemId(rpage, roffnum));
		itemsz = IndexTupleDSize(hitem->hash_itup)
			+ (sizeof(HashItemData) - sizeof(IndexTupleData));
		itemsz = MAXALIGN(itemsz);

		/*
		 * walk up the bucket chain, looking for a page big enough for
		 * this item.
		 */
		while (PageGetFreeSpace(wpage) < itemsz)
		{
			wblkno = wopaque->hasho_nextblkno;

			_hash_wrtbuf(rel, wbuf);

			if (!BlockNumberIsValid(wblkno) || (rblkno == wblkno))
			{
				_hash_wrtbuf(rel, rbuf);
				/* wbuf is already released */
				return;
			}

			wbuf = _hash_getbuf(rel, wblkno, HASH_WRITE);
			wpage = BufferGetPage(wbuf);
			_hash_checkpage(rel, wpage, LH_OVERFLOW_PAGE);
			Assert(!PageIsEmpty(wpage));
			wopaque = (HashPageOpaque) PageGetSpecialPointer(wpage);
			Assert(wopaque->hasho_bucket == bucket);
		}

		/*
		 * if we're here, we have found room so insert on the "write"
		 * page.
		 */
		woffnum = OffsetNumberNext(PageGetMaxOffsetNumber(wpage));
		if (PageAddItem(wpage, (Item) hitem, itemsz, woffnum, LP_USED)
			== InvalidOffsetNumber)
			elog(ERROR, "failed to add index item to \"%s\"",
				 RelationGetRelationName(rel));

		/*
		 * delete the tuple from the "read" page. PageIndexTupleDelete
		 * repacks the ItemId array, so 'roffnum' will be "advanced" to
		 * the "next" ItemId.
		 */
		PageIndexTupleDelete(rpage, roffnum);
		_hash_wrtnorelbuf(rbuf);

		/*
		 * if the "read" page is now empty because of the deletion, free
		 * it.
		 */
		if (PageIsEmpty(rpage) && (ropaque->hasho_flag & LH_OVERFLOW_PAGE))
		{
			rblkno = ropaque->hasho_prevblkno;
			Assert(BlockNumberIsValid(rblkno));

			/* free this overflow page */
			_hash_freeovflpage(rel, rbuf);

			if (rblkno == wblkno)
			{
				/* rbuf is already released */
				_hash_wrtbuf(rel, wbuf);
				return;
			}

			rbuf = _hash_getbuf(rel, rblkno, HASH_WRITE);
			rpage = BufferGetPage(rbuf);
			_hash_checkpage(rel, rpage, LH_OVERFLOW_PAGE);
			Assert(!PageIsEmpty(rpage));
			ropaque = (HashPageOpaque) PageGetSpecialPointer(rpage);
			Assert(ropaque->hasho_bucket == bucket);

			roffnum = FirstOffsetNumber;
		}
	}
}
