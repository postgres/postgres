/*-------------------------------------------------------------------------
 * nbtsort.c
 *		Build a btree from sorted input by loading leaf pages sequentially.
 *
 * NOTES
 *
 * We use tuplesort.c to sort the given index tuples into order.
 * Then we scan the index tuples in order and build the btree pages
 * for each level.	When we have only one page on a level, it must be the
 * root -- it can be attached to the btree metapage and we are done.
 *
 * this code is moderately slow (~10% slower) compared to the regular
 * btree (insertion) build code on sorted or well-clustered data.  on
 * random data, however, the insertion build code is unusable -- the
 * difference on a 60MB heap is a factor of 15 because the random
 * probes into the btree thrash the buffer pool.  (NOTE: the above
 * "10%" estimate is probably obsolete, since it refers to an old and
 * not very good external sort implementation that used to exist in
 * this module.  tuplesort.c is almost certainly faster.)
 *
 * this code currently packs the pages to 100% of capacity.  this is
 * not wise, since *any* insertion will cause splitting.  filling to
 * something like the standard 70% steady-state load factor for btrees
 * would probably be better.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/nbtree/nbtsort.c,v 1.52.2.1 2001/01/04 21:51:49 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/nbtree.h"
#include "utils/tuplesort.h"


#ifdef BTREE_BUILD_STATS
#define ShowExecutorStats pg_options[TRACE_EXECUTORSTATS]
#endif

/*
 * turn on debugging output.
 *
 * XXX this code just does a numeric printf of the index key, so it's
 * only really useful for integer keys.
 */
/*#define FASTBUILD_DEBUG*/

/*
 * Status record for spooling.
 */
struct BTSpool
{
	Tuplesortstate *sortstate;	/* state data for tuplesort.c */
	Relation	index;
	bool		isunique;
};

#define BTITEMSZ(btitem) \
	((btitem) ? \
	 (IndexTupleDSize((btitem)->bti_itup) + \
	  (sizeof(BTItemData) - sizeof(IndexTupleData))) : \
	 0)


static void _bt_load(Relation index, BTSpool *btspool);
static BTItem _bt_buildadd(Relation index, Size keysz, ScanKey scankey,
			 BTPageState *state, BTItem bti, int flags);
static BTItem _bt_minitem(Page opage, BlockNumber oblkno, int atend);
static BTPageState *_bt_pagestate(Relation index, int flags,
			  int level, bool doupper);
static void _bt_uppershutdown(Relation index, Size keysz, ScanKey scankey,
				  BTPageState *state);


/*
 * Interface routines
 */


/*
 * create and initialize a spool structure
 */
BTSpool    *
_bt_spoolinit(Relation index, bool isunique)
{
	BTSpool    *btspool = (BTSpool *) palloc(sizeof(BTSpool));

	MemSet((char *) btspool, 0, sizeof(BTSpool));

	btspool->index = index;
	btspool->isunique = isunique;

	btspool->sortstate = tuplesort_begin_index(index, isunique, false);

	/*
	 * Currently, tuplesort provides sort functions on IndexTuples. If we
	 * kept anything in a BTItem other than a regular IndexTuple, we'd
	 * need to modify tuplesort to understand BTItems as such.
	 */
	Assert(sizeof(BTItemData) == sizeof(IndexTupleData));

	return btspool;
}

/*
 * clean up a spool structure and its substructures.
 */
void
_bt_spooldestroy(BTSpool *btspool)
{
	tuplesort_end(btspool->sortstate);
	pfree((void *) btspool);
}

/*
 * spool a btitem into the sort file.
 */
void
_bt_spool(BTItem btitem, BTSpool *btspool)
{
	/* A BTItem is really just an IndexTuple */
	tuplesort_puttuple(btspool->sortstate, (void *) btitem);
}

/*
 * given a spool loaded by successive calls to _bt_spool,
 * create an entire btree.
 */
void
_bt_leafbuild(BTSpool *btspool)
{
#ifdef BTREE_BUILD_STATS
	if (ShowExecutorStats)
	{
		fprintf(stderr, "! BtreeBuild (Spool) Stats:\n");
		ShowUsage();
		ResetUsage();
	}
#endif
	tuplesort_performsort(btspool->sortstate);

	_bt_load(btspool->index, btspool);
}


/*
 * Internal routines.
 */


/*
 * allocate a new, clean btree page, not linked to any siblings.
 */
static void
_bt_blnewpage(Relation index, Buffer *buf, Page *page, int flags)
{
	BTPageOpaque opaque;

	*buf = _bt_getbuf(index, P_NEW, BT_WRITE);
#ifdef NOT_USED
	printf("\tblk=%d\n", BufferGetBlockNumber(*buf));
#endif
	*page = BufferGetPage(*buf);
	_bt_pageinit(*page, BufferGetPageSize(*buf));
	opaque = (BTPageOpaque) PageGetSpecialPointer(*page);
	opaque->btpo_prev = opaque->btpo_next = P_NONE;
	opaque->btpo_flags = flags;
}

/*
 * slide an array of ItemIds back one slot (from P_FIRSTKEY to
 * P_HIKEY, overwriting P_HIKEY).  we need to do this when we discover
 * that we have built an ItemId array in what has turned out to be a
 * P_RIGHTMOST page.
 */
static void
_bt_slideleft(Relation index, Buffer buf, Page page)
{
	OffsetNumber off;
	OffsetNumber maxoff;
	ItemId		previi;
	ItemId		thisii;

	if (!PageIsEmpty(page))
	{
		maxoff = PageGetMaxOffsetNumber(page);
		previi = PageGetItemId(page, P_HIKEY);
		for (off = P_FIRSTKEY; off <= maxoff; off = OffsetNumberNext(off))
		{
			thisii = PageGetItemId(page, off);
			*previi = *thisii;
			previi = thisii;
		}
		((PageHeader) page)->pd_lower -= sizeof(ItemIdData);
	}
}

/*
 * allocate and initialize a new BTPageState.  the returned structure
 * is suitable for immediate use by _bt_buildadd.
 */
static BTPageState *
_bt_pagestate(Relation index, int flags, int level, bool doupper)
{
	BTPageState *state = (BTPageState *) palloc(sizeof(BTPageState));

	MemSet((char *) state, 0, sizeof(BTPageState));
	_bt_blnewpage(index, &(state->btps_buf), &(state->btps_page), flags);
	state->btps_firstoff = InvalidOffsetNumber;
	state->btps_lastoff = P_HIKEY;
	state->btps_lastbti = (BTItem) NULL;
	state->btps_next = (BTPageState *) NULL;
	state->btps_level = level;
	state->btps_doupper = doupper;

	return state;
}

/*
 * return a copy of the minimum (P_HIKEY or P_FIRSTKEY) item on
 * 'opage'.  the copy is modified to point to 'opage' (as opposed to
 * the page to which the item used to point, e.g., a heap page if
 * 'opage' is a leaf page).
 */
static BTItem
_bt_minitem(Page opage, BlockNumber oblkno, int atend)
{
	OffsetNumber off;
	BTItem		obti;
	BTItem		nbti;

	off = atend ? P_HIKEY : P_FIRSTKEY;
	obti = (BTItem) PageGetItem(opage, PageGetItemId(opage, off));
	nbti = _bt_formitem(&(obti->bti_itup));
	ItemPointerSet(&(nbti->bti_itup.t_tid), oblkno, P_HIKEY);

	return nbti;
}

/*
 * add an item to a disk page from a merge tape block.
 *
 * we must be careful to observe the following restrictions, placed
 * upon us by the conventions in nbtsearch.c:
 * - rightmost pages start data items at P_HIKEY instead of at
 *	 P_FIRSTKEY.
 * - duplicates cannot be split among pages unless the chain of
 *	 duplicates starts at the first data item.
 *
 * a leaf page being built looks like:
 *
 * +----------------+---------------------------------+
 * | PageHeaderData | linp0 linp1 linp2 ...			  |
 * +-----------+----+---------------------------------+
 * | ... linpN |				  ^ first			  |
 * +-----------+--------------------------------------+
 * |	 ^ last										  |
 * |												  |
 * |			   v last							  |
 * +-------------+------------------------------------+
 * |			 | itemN ...						  |
 * +-------------+------------------+-----------------+
 * |		  ... item3 item2 item1 | "special space" |
 * +--------------------------------+-----------------+
 *						^ first
 *
 * contrast this with the diagram in bufpage.h; note the mismatch
 * between linps and items.  this is because we reserve linp0 as a
 * placeholder for the pointer to the "high key" item; when we have
 * filled up the page, we will set linp0 to point to itemN and clear
 * linpN.
 *
 * 'last' pointers indicate the last offset/item added to the page.
 * 'first' pointers indicate the first offset/item that is part of a
 * chain of duplicates extending from 'first' to 'last'.
 *
 * if all keys are unique, 'first' will always be the same as 'last'.
 */
static BTItem
_bt_buildadd(Relation index, Size keysz, ScanKey scankey,
			 BTPageState *state, BTItem bti, int flags)
{
	Buffer		nbuf;
	Page		npage;
	BTItem		last_bti;
	OffsetNumber first_off;
	OffsetNumber last_off;
	OffsetNumber off;
	Size		pgspc;
	Size		btisz;

	nbuf = state->btps_buf;
	npage = state->btps_page;
	first_off = state->btps_firstoff;
	last_off = state->btps_lastoff;
	last_bti = state->btps_lastbti;

	pgspc = PageGetFreeSpace(npage);
	btisz = BTITEMSZ(bti);
	btisz = MAXALIGN(btisz);

	/*
	 * Check whether the item can fit on a btree page at all. (Eventually,
	 * we ought to try to apply TOAST methods if not.) We actually need to
	 * be able to fit three items on every page, so restrict any one item
	 * to 1/3 the per-page available space. Note that at this point, btisz
	 * doesn't include the ItemId.
	 *
	 * NOTE: similar code appears in _bt_insertonpg() to defend against
	 * oversize items being inserted into an already-existing index. But
	 * during creation of an index, we don't go through there.
	 */
	if (btisz > (PageGetPageSize(npage) - sizeof(PageHeaderData) - MAXALIGN(sizeof(BTPageOpaqueData))) / 3 - sizeof(ItemIdData))
		elog(ERROR, "btree: index item size %d exceeds maximum %ld",
			 btisz,
			 (PageGetPageSize(npage) - sizeof(PageHeaderData) - MAXALIGN(sizeof(BTPageOpaqueData))) /3 - sizeof(ItemIdData));

	while (pgspc < btisz)
	{
		Buffer		obuf = nbuf;
		Page		opage = npage;
		OffsetNumber o,
					n;
		ItemId		ii;
		ItemId		hii;

		_bt_blnewpage(index, &nbuf, &npage, flags);

		/*
		 * if 'last' is part of a chain of duplicates that does not start
		 * at the beginning of the old page, the entire chain is copied to
		 * the new page; we delete all of the duplicates from the old page
		 * except the first, which becomes the high key item of the old
		 * page.
		 *
		 * if the chain starts at the beginning of the page or there is no
		 * chain ('first' == 'last'), we need only copy 'last' to the new
		 * page.  again, 'first' (== 'last') becomes the high key of the
		 * old page.
		 *
		 * note that in either case, we copy at least one item to the new
		 * page, so 'last_bti' will always be valid.  'bti' will never be
		 * the first data item on the new page.
		 */
		if (first_off == P_FIRSTKEY)
		{
			Assert(last_off != P_FIRSTKEY);
			first_off = last_off;
		}
		for (o = first_off, n = P_FIRSTKEY;
			 o <= last_off;
			 o = OffsetNumberNext(o), n = OffsetNumberNext(n))
		{
			ii = PageGetItemId(opage, o);
			if (PageAddItem(npage, PageGetItem(opage, ii),
						  ii->lp_len, n, LP_USED) == InvalidOffsetNumber)
				elog(FATAL, "btree: failed to add item to the page in _bt_sort (1)");
#ifdef FASTBUILD_DEBUG
			{
				bool		isnull;
				BTItem		tmpbti =
				(BTItem) PageGetItem(npage, PageGetItemId(npage, n));
				Datum		d = index_getattr(&(tmpbti->bti_itup), 1,
											  index->rd_att, &isnull);

				printf("_bt_buildadd: moved <%x> to offset %d at level %d\n",
					   d, n, state->btps_level);
			}
#endif
		}

		/*
		 * this loop is backward because PageIndexTupleDelete shuffles the
		 * tuples to fill holes in the page -- by starting at the end and
		 * working back, we won't create holes (and thereby avoid
		 * shuffling).
		 */
		for (o = last_off; o > first_off; o = OffsetNumberPrev(o))
			PageIndexTupleDelete(opage, o);
		hii = PageGetItemId(opage, P_HIKEY);
		ii = PageGetItemId(opage, first_off);
		*hii = *ii;
		ii->lp_flags &= ~LP_USED;
		((PageHeader) opage)->pd_lower -= sizeof(ItemIdData);

		first_off = P_FIRSTKEY;
		last_off = PageGetMaxOffsetNumber(npage);
		last_bti = (BTItem) PageGetItem(npage, PageGetItemId(npage, last_off));

		/*
		 * set the page (side link) pointers.
		 */
		{
			BTPageOpaque oopaque = (BTPageOpaque) PageGetSpecialPointer(opage);
			BTPageOpaque nopaque = (BTPageOpaque) PageGetSpecialPointer(npage);

			oopaque->btpo_next = BufferGetBlockNumber(nbuf);
			nopaque->btpo_prev = BufferGetBlockNumber(obuf);
			nopaque->btpo_next = P_NONE;

			if (_bt_itemcmp(index, keysz, scankey,
			  (BTItem) PageGetItem(opage, PageGetItemId(opage, P_HIKEY)),
			(BTItem) PageGetItem(opage, PageGetItemId(opage, P_FIRSTKEY)),
							BTEqualStrategyNumber))
				oopaque->btpo_flags |= BTP_CHAIN;
		}

		/*
		 * copy the old buffer's minimum key to its parent.  if we don't
		 * have a parent, we have to create one; this adds a new btree
		 * level.
		 */
		if (state->btps_doupper)
		{
			BTItem		nbti;

			if (state->btps_next == (BTPageState *) NULL)
			{
				state->btps_next =
					_bt_pagestate(index, 0, state->btps_level + 1, true);
			}
			nbti = _bt_minitem(opage, BufferGetBlockNumber(obuf), 0);
			_bt_buildadd(index, keysz, scankey, state->btps_next, nbti, 0);
			pfree((void *) nbti);
		}

		/*
		 * write out the old stuff.  we never want to see it again, so we
		 * can give up our lock (if we had one; BuildingBtree is set, so
		 * we aren't locking).
		 */
		_bt_wrtbuf(index, obuf);

		/*
		 * Recompute pgspc and loop back to check free space again.  If
		 * we were forced to split at a bad split point, we might need
		 * to split again.
		 */
		pgspc = PageGetFreeSpace(npage);
	}

	/*
	 * if this item is different from the last item added, we start a new
	 * chain of duplicates.
	 */
	off = OffsetNumberNext(last_off);
	if (PageAddItem(npage, (Item) bti, btisz, off, LP_USED) == InvalidOffsetNumber)
		elog(FATAL, "btree: failed to add item to the page in _bt_sort (2)");
#ifdef FASTBUILD_DEBUG
	{
		bool		isnull;
		Datum		d = index_getattr(&(bti->bti_itup), 1, index->rd_att, &isnull);

		printf("_bt_buildadd: inserted <%x> at offset %d at level %d\n",
			   d, off, state->btps_level);
	}
#endif
	if (last_bti == (BTItem) NULL)
		first_off = P_FIRSTKEY;
	else if (!_bt_itemcmp(index, keysz, scankey,
						  bti, last_bti, BTEqualStrategyNumber))
		first_off = off;
	last_off = off;
	last_bti = (BTItem) PageGetItem(npage, PageGetItemId(npage, off));

	state->btps_buf = nbuf;
	state->btps_page = npage;
	state->btps_lastbti = last_bti;
	state->btps_lastoff = last_off;
	state->btps_firstoff = first_off;

	return last_bti;
}

static void
_bt_uppershutdown(Relation index, Size keysz, ScanKey scankey,
				  BTPageState *state)
{
	BTPageState *s;
	BlockNumber blkno;
	BTPageOpaque opaque;
	BTItem		bti;

	for (s = state; s != (BTPageState *) NULL; s = s->btps_next)
	{
		blkno = BufferGetBlockNumber(s->btps_buf);
		opaque = (BTPageOpaque) PageGetSpecialPointer(s->btps_page);

		/*
		 * if this is the root, attach it to the metapage.	otherwise,
		 * stick the minimum key of the last page on this level (which has
		 * not been split, or else it wouldn't be the last page) into its
		 * parent.	this may cause the last page of upper levels to split,
		 * but that's not a problem -- we haven't gotten to them yet.
		 */
		if (s->btps_doupper)
		{
			if (s->btps_next == (BTPageState *) NULL)
			{
				opaque->btpo_flags |= BTP_ROOT;
				_bt_metaproot(index, blkno, s->btps_level + 1);
			}
			else
			{
				bti = _bt_minitem(s->btps_page, blkno, 0);
				_bt_buildadd(index, keysz, scankey, s->btps_next, bti, 0);
				pfree((void *) bti);
			}
		}

		/*
		 * this is the rightmost page, so the ItemId array needs to be
		 * slid back one slot.
		 */
		_bt_slideleft(index, s->btps_buf, s->btps_page);
		_bt_wrtbuf(index, s->btps_buf);
	}
}

/*
 * Read tuples in correct sort order from tuplesort, and load them into
 * btree leaves.
 */
static void
_bt_load(Relation index, BTSpool *btspool)
{
	BTPageState *state;
	ScanKey		skey;
	int			natts;
	BTItem		bti;
	bool		should_free;

	/*
	 * initialize state needed for the merge into the btree leaf pages.
	 */
	state = _bt_pagestate(index, BTP_LEAF, 0, true);

	skey = _bt_mkscankey_nodata(index);
	natts = RelationGetNumberOfAttributes(index);

	for (;;)
	{
		bti = (BTItem) tuplesort_getindextuple(btspool->sortstate, true,
											   &should_free);
		if (bti == (BTItem) NULL)
			break;
		_bt_buildadd(index, natts, skey, state, bti, BTP_LEAF);
		if (should_free)
			pfree((void *) bti);
	}

	_bt_uppershutdown(index, natts, skey, state);

	_bt_freeskey(skey);
}
