/*-------------------------------------------------------------------------
 *
 * hashovfl.c
 *	  Overflow page management code for the Postgres hash access method
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/hash/hashovfl.c,v 1.36 2003/08/04 00:43:12 momjian Exp $
 *
 * NOTES
 *	  Overflow pages look like ordinary relation pages.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"


static OverflowPageAddress _hash_getovfladdr(Relation rel, Buffer *metabufp);
static uint32 _hash_firstfreebit(uint32 map);

/*
 *	_hash_addovflpage
 *
 *	Add an overflow page to the page currently pointed to by the buffer
 *	argument 'buf'.
 *
 *	*Metabufp has a read lock upon entering the function; buf has a
 *	write lock.
 *
 */
Buffer
_hash_addovflpage(Relation rel, Buffer *metabufp, Buffer buf)
{

	OverflowPageAddress oaddr;
	BlockNumber ovflblkno;
	Buffer		ovflbuf;
	HashMetaPage metap;
	HashPageOpaque ovflopaque;
	HashPageOpaque pageopaque;
	Page		page;
	Page		ovflpage;

	/* this had better be the last page in a bucket chain */
	page = BufferGetPage(buf);
	_hash_checkpage(page, LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
	pageopaque = (HashPageOpaque) PageGetSpecialPointer(page);
	Assert(!BlockNumberIsValid(pageopaque->hasho_nextblkno));

	metap = (HashMetaPage) BufferGetPage(*metabufp);
	_hash_checkpage((Page) metap, LH_META_PAGE);

	/* allocate an empty overflow page */
	oaddr = _hash_getovfladdr(rel, metabufp);
	if (oaddr == InvalidOvflAddress)
		elog(ERROR, "_hash_getovfladdr failed");
	ovflblkno = OADDR_TO_BLKNO(OADDR_OF(SPLITNUM(oaddr), OPAGENUM(oaddr)));
	Assert(BlockNumberIsValid(ovflblkno));
	ovflbuf = _hash_getbuf(rel, ovflblkno, HASH_WRITE);
	Assert(BufferIsValid(ovflbuf));
	ovflpage = BufferGetPage(ovflbuf);

	/* initialize the new overflow page */
	_hash_pageinit(ovflpage, BufferGetPageSize(ovflbuf));
	ovflopaque = (HashPageOpaque) PageGetSpecialPointer(ovflpage);
	ovflopaque->hasho_prevblkno = BufferGetBlockNumber(buf);
	ovflopaque->hasho_nextblkno = InvalidBlockNumber;
	ovflopaque->hasho_flag = LH_OVERFLOW_PAGE;
	ovflopaque->hasho_oaddr = oaddr;
	ovflopaque->hasho_bucket = pageopaque->hasho_bucket;
	_hash_wrtnorelbuf(ovflbuf);

	/* logically chain overflow page to previous page */
	pageopaque->hasho_nextblkno = ovflblkno;
	_hash_wrtnorelbuf(buf);
	return ovflbuf;
}

/*
 *	_hash_getovfladdr()
 *
 *	Find an available overflow page and return its address.
 *
 *	When we enter this function, we have a read lock on *metabufp which
 *	we change to a write lock immediately. Before exiting, the write lock
 *	is exchanged for a read lock.
 *
 */
static OverflowPageAddress
_hash_getovfladdr(Relation rel, Buffer *metabufp)
{
	HashMetaPage metap;
	Buffer		mapbuf = 0;
	BlockNumber blkno;
	PageOffset	offset;
	OverflowPageAddress oaddr;
	SplitNumber splitnum;
	uint32	   *freep = NULL;
	uint32		max_free;
	uint32		bit;
	uint32		first_page;
	uint32		free_bit;
	uint32		free_page;
	uint32		in_use_bits;
	uint32		i,
				j;

	metap = (HashMetaPage) _hash_chgbufaccess(rel, metabufp, HASH_READ, HASH_WRITE);

	splitnum = metap->hashm_ovflpoint;
	max_free = metap->hashm_spares[splitnum];

	free_page = (max_free - 1) >> (metap->hashm_bshift + BYTE_TO_BIT);
	free_bit = (max_free - 1) & (BMPGSZ_BIT(metap) - 1);

	/* Look through all the free maps to find the first free block */
	first_page = metap->hashm_lastfreed >> (metap->hashm_bshift + BYTE_TO_BIT);
	for (i = first_page; i <= free_page; i++)
	{
		Page		mappage;

		blkno = metap->hashm_mapp[i];
		mapbuf = _hash_getbuf(rel, blkno, HASH_WRITE);
		mappage = BufferGetPage(mapbuf);
		_hash_checkpage(mappage, LH_BITMAP_PAGE);
		freep = HashPageGetBitmap(mappage);
		Assert(freep);

		if (i == free_page)
			in_use_bits = free_bit;
		else
			in_use_bits = BMPGSZ_BIT(metap) - 1;

		if (i == first_page)
		{
			bit = metap->hashm_lastfreed & (BMPGSZ_BIT(metap) - 1);
			j = bit / BITS_PER_MAP;
			bit = bit & ~(BITS_PER_MAP - 1);
		}
		else
		{
			bit = 0;
			j = 0;
		}
		for (; bit <= in_use_bits; j++, bit += BITS_PER_MAP)
			if (freep[j] != ALL_SET)
				goto found;
	}

	/* No Free Page Found - have to allocate a new page */
	metap->hashm_lastfreed = metap->hashm_spares[splitnum];
	metap->hashm_spares[splitnum]++;
	offset = metap->hashm_spares[splitnum] -
		(splitnum ? metap->hashm_spares[splitnum - 1] : 0);

	if (offset > SPLITMASK)
	{
		if (++splitnum >= NCACHED)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("out of overflow pages in hash index \"%s\"",
							RelationGetRelationName(rel))));
		metap->hashm_ovflpoint = splitnum;
		metap->hashm_spares[splitnum] = metap->hashm_spares[splitnum - 1];
		metap->hashm_spares[splitnum - 1]--;
		offset = 0;
	}

	/* Check if we need to allocate a new bitmap page */
	if (free_bit == (uint32) (BMPGSZ_BIT(metap) - 1))
	{
		/* won't be needing old map page */

		_hash_relbuf(rel, mapbuf, HASH_WRITE);

		free_page++;
		if (free_page >= NCACHED)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("out of overflow pages in hash index \"%s\"",
							RelationGetRelationName(rel))));

		/*
		 * This is tricky.	The 1 indicates that you want the new page
		 * allocated with 1 clear bit.	Actually, you are going to
		 * allocate 2 pages from this map.	The first is going to be the
		 * map page, the second is the overflow page we were looking for.
		 * The init_bitmap routine automatically, sets the first bit of
		 * itself to indicate that the bitmap itself is in use.  We would
		 * explicitly set the second bit, but don't have to if we tell
		 * init_bitmap not to leave it clear in the first place.
		 */
		if (_hash_initbitmap(rel, metap, OADDR_OF(splitnum, offset),
							 1, free_page))
			elog(ERROR, "_hash_initbitmap failed");
		metap->hashm_spares[splitnum]++;
		offset++;
		if (offset > SPLITMASK)
		{
			if (++splitnum >= NCACHED)
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("out of overflow pages in hash index \"%s\"",
							RelationGetRelationName(rel))));
			metap->hashm_ovflpoint = splitnum;
			metap->hashm_spares[splitnum] = metap->hashm_spares[splitnum - 1];
			metap->hashm_spares[splitnum - 1]--;
			offset = 0;
		}
	}
	else
	{
		/*
		 * Free_bit addresses the last used bit.  Bump it to address the
		 * first available bit.
		 */
		free_bit++;
		SETBIT(freep, free_bit);
		_hash_wrtbuf(rel, mapbuf);
	}

	/* Calculate address of the new overflow page */
	oaddr = OADDR_OF(splitnum, offset);
	_hash_chgbufaccess(rel, metabufp, HASH_WRITE, HASH_READ);
	return oaddr;

found:
	bit = bit + _hash_firstfreebit(freep[j]);
	SETBIT(freep, bit);
	_hash_wrtbuf(rel, mapbuf);

	/*
	 * Bits are addressed starting with 0, but overflow pages are
	 * addressed beginning at 1. Bit is a bit addressnumber, so we need to
	 * increment it to convert it to a page number.
	 */

	bit = 1 + bit + (i * BMPGSZ_BIT(metap));
	if (bit >= metap->hashm_lastfreed)
		metap->hashm_lastfreed = bit - 1;

	/* Calculate the split number for this page */
	for (i = 0; (i < splitnum) && (bit > metap->hashm_spares[i]); i++)
		;
	offset = (i ? bit - metap->hashm_spares[i - 1] : bit);
	if (offset >= SPLITMASK)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("out of overflow pages in hash index \"%s\"",
						RelationGetRelationName(rel))));

	/* initialize this page */
	oaddr = OADDR_OF(i, offset);
	_hash_chgbufaccess(rel, metabufp, HASH_WRITE, HASH_READ);
	return oaddr;
}

/*
 *	_hash_firstfreebit()
 *
 *	Return the first bit that is not set in the argument 'map'. This
 *	function is used to find an available overflow page within a
 *	splitnumber.
 *
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
		mask = mask << 1;
	}
	return i;
}

/*
 *	_hash_freeovflpage() -
 *
 *	Mark this overflow page as free and return a buffer with
 *	the page that follows it (which may be defined as
 *	InvalidBuffer).
 *
 */
Buffer
_hash_freeovflpage(Relation rel, Buffer ovflbuf)
{
	HashMetaPage metap;
	Buffer		metabuf;
	Buffer		mapbuf;
	BlockNumber prevblkno;
	BlockNumber blkno;
	BlockNumber nextblkno;
	HashPageOpaque ovflopaque;
	Page		ovflpage;
	Page		mappage;
	OverflowPageAddress addr;
	SplitNumber splitnum;
	uint32	   *freep;
	uint32		ovflpgno;
	int32		bitmappage,
				bitmapbit;
	Bucket		bucket;

	metabuf = _hash_getbuf(rel, HASH_METAPAGE, HASH_WRITE);
	metap = (HashMetaPage) BufferGetPage(metabuf);
	_hash_checkpage((Page) metap, LH_META_PAGE);

	ovflpage = BufferGetPage(ovflbuf);
	_hash_checkpage(ovflpage, LH_OVERFLOW_PAGE);
	ovflopaque = (HashPageOpaque) PageGetSpecialPointer(ovflpage);
	addr = ovflopaque->hasho_oaddr;
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

		_hash_checkpage(prevpage, LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
		Assert(prevopaque->hasho_bucket == bucket);
		prevopaque->hasho_nextblkno = nextblkno;
		_hash_wrtbuf(rel, prevbuf);
	}
	if (BlockNumberIsValid(nextblkno))
	{
		Buffer		nextbuf = _hash_getbuf(rel, nextblkno, HASH_WRITE);
		Page		nextpage = BufferGetPage(nextbuf);
		HashPageOpaque nextopaque = (HashPageOpaque) PageGetSpecialPointer(nextpage);

		_hash_checkpage(nextpage, LH_OVERFLOW_PAGE);
		Assert(nextopaque->hasho_bucket == bucket);
		nextopaque->hasho_prevblkno = prevblkno;
		_hash_wrtbuf(rel, nextbuf);
	}

	/*
	 * Fix up the overflow page bitmap that tracks this particular
	 * overflow page. The bitmap can be found in the MetaPageData array
	 * element hashm_mapp[bitmappage].
	 */
	splitnum = (addr >> SPLITSHIFT);
	ovflpgno = (splitnum ? metap->hashm_spares[splitnum - 1] : 0) + (addr & SPLITMASK) - 1;

	if (ovflpgno < metap->hashm_lastfreed)
		metap->hashm_lastfreed = ovflpgno;

	bitmappage = (ovflpgno >> (metap->hashm_bshift + BYTE_TO_BIT));
	bitmapbit = ovflpgno & (BMPGSZ_BIT(metap) - 1);

	blkno = metap->hashm_mapp[bitmappage];
	mapbuf = _hash_getbuf(rel, blkno, HASH_WRITE);
	mappage = BufferGetPage(mapbuf);
	_hash_checkpage(mappage, LH_BITMAP_PAGE);
	freep = HashPageGetBitmap(mappage);
	CLRBIT(freep, bitmapbit);
	_hash_wrtbuf(rel, mapbuf);

	_hash_relbuf(rel, metabuf, HASH_WRITE);

	/*
	 * now instantiate the page that replaced this one, if it exists, and
	 * return that buffer with a write lock.
	 */
	if (BlockNumberIsValid(nextblkno))
		return _hash_getbuf(rel, nextblkno, HASH_WRITE);
	else
		return InvalidBuffer;
}


/*
 *	_hash_initbitmap()
 *
 *	 Initialize a new bitmap page.	The metapage has a write-lock upon
 *	 entering the function.
 *
 * 'pnum' is the OverflowPageAddress of the new bitmap page.
 * 'nbits' is how many bits to clear (i.e., make available) in the new
 * bitmap page.  the remainder of the bits (as well as the first bit,
 * representing the bitmap page itself) will be set.
 * 'ndx' is the 0-based offset of the new bitmap page within the
 * metapage's array of bitmap page OverflowPageAddresses.
 */

#define INT_MASK		((1 << INT_TO_BIT) -1)

int32
_hash_initbitmap(Relation rel,
				 HashMetaPage metap,
				 int32 pnum,
				 int32 nbits,
				 int32 ndx)
{
	Buffer		buf;
	BlockNumber blkno;
	Page		pg;
	HashPageOpaque op;
	uint32	   *freep;
	int			clearbytes,
				clearints;

	blkno = OADDR_TO_BLKNO(pnum);
	buf = _hash_getbuf(rel, blkno, HASH_WRITE);
	pg = BufferGetPage(buf);
	_hash_pageinit(pg, BufferGetPageSize(buf));
	op = (HashPageOpaque) PageGetSpecialPointer(pg);
	op->hasho_oaddr = InvalidOvflAddress;
	op->hasho_prevblkno = InvalidBlockNumber;
	op->hasho_nextblkno = InvalidBlockNumber;
	op->hasho_flag = LH_BITMAP_PAGE;
	op->hasho_bucket = -1;

	freep = HashPageGetBitmap(pg);

	/* set all of the bits above 'nbits' to 1 */
	clearints = ((nbits - 1) >> INT_TO_BIT) + 1;
	clearbytes = clearints << INT_TO_BYTE;
	MemSet((char *) freep, 0, clearbytes);
	MemSet(((char *) freep) + clearbytes, 0xFF,
		   BMPGSZ_BYTE(metap) - clearbytes);
	freep[clearints - 1] = ALL_SET << (nbits & INT_MASK);

	/* bit 0 represents the new bitmap page */
	SETBIT(freep, 0);

	/* metapage already has a write lock */
	metap->hashm_nmaps++;
	metap->hashm_mapp[ndx] = blkno;

	/* write out the new bitmap page (releasing its locks) */
	_hash_wrtbuf(rel, buf);

	return 0;
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
 */
void
_hash_squeezebucket(Relation rel,
					HashMetaPage metap,
					Bucket bucket)
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
	wblkno = BUCKET_TO_BLKNO(bucket);
	wbuf = _hash_getbuf(rel, wblkno, HASH_WRITE);
	wpage = BufferGetPage(wbuf);
	_hash_checkpage(wpage, LH_BUCKET_PAGE);
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
	 *
	 * XXX if chains tend to be long, we should probably move forward using
	 * HASH_READ and then _hash_chgbufaccess to HASH_WRITE when we reach
	 * the end.  if they are short we probably don't care very much.  if
	 * the hash function is working at all, they had better be short..
	 */
	ropaque = wopaque;
	do
	{
		rblkno = ropaque->hasho_nextblkno;
		if (ropaque != wopaque)
			_hash_relbuf(rel, rbuf, HASH_WRITE);
		rbuf = _hash_getbuf(rel, rblkno, HASH_WRITE);
		rpage = BufferGetPage(rbuf);
		_hash_checkpage(rpage, LH_OVERFLOW_PAGE);
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
			_hash_checkpage(wpage, LH_OVERFLOW_PAGE);
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

			/*
			 * free this overflow page.  the extra _hash_relbuf is because
			 * _hash_freeovflpage gratuitously returns the next page (we
			 * want the previous page and will get it ourselves later).
			 */
			rbuf = _hash_freeovflpage(rel, rbuf);
			if (BufferIsValid(rbuf))
				_hash_relbuf(rel, rbuf, HASH_WRITE);

			if (rblkno == wblkno)
			{
				/* rbuf is already released */
				_hash_wrtbuf(rel, wbuf);
				return;
			}

			rbuf = _hash_getbuf(rel, rblkno, HASH_WRITE);
			rpage = BufferGetPage(rbuf);
			_hash_checkpage(rpage, LH_OVERFLOW_PAGE);
			Assert(!PageIsEmpty(rpage));
			ropaque = (HashPageOpaque) PageGetSpecialPointer(rpage);
			Assert(ropaque->hasho_bucket == bucket);

			roffnum = FirstOffsetNumber;
		}
	}
}
