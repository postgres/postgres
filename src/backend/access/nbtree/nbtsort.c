/*-------------------------------------------------------------------------
 * nbtsort.c
 *		Build a btree from sorted input by loading leaf pages sequentially.
 *
 * NOTES
 *
 * We use tuplesort.c to sort the given index tuples into order.
 * Then we scan the index tuples in order and build the btree pages
 * for each level.  We load source tuples into leaf-level pages.
 * Whenever we fill a page at one level, we add a link to it to its
 * parent level (starting a new parent level if necessary).  When
 * done, we write out each final page on each level, adding it to
 * its parent level.  When we have only one page on a level, it must be
 * the root -- it can be attached to the btree metapage and we are done.
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
 * Another limitation is that we currently load full copies of all keys
 * into upper tree levels.  The leftmost data key in each non-leaf node
 * could be omitted as far as normal btree operations are concerned
 * (see README for more info).  However, because we build the tree from
 * the bottom up, we need that data key to insert into the node's parent.
 * This could be fixed by keeping a spare copy of the minimum key in the
 * state stack, but I haven't time for that right now.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/nbtree/nbtsort.c,v 1.55 2000/07/21 06:42:33 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/nbtree.h"
#include "utils/tuplesort.h"


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

/*
 * Status record for a btree page being built.  We have one of these
 * for each active tree level.
 */
typedef struct BTPageState
{
	Buffer		btps_buf;		/* current buffer & page */
	Page		btps_page;
	OffsetNumber btps_lastoff;	/* last item offset loaded */
	int			btps_level;
	struct BTPageState *btps_next; /* link to parent level, if any */
} BTPageState;


#define BTITEMSZ(btitem) \
	((btitem) ? \
	 (IndexTupleDSize((btitem)->bti_itup) + \
	  (sizeof(BTItemData) - sizeof(IndexTupleData))) : \
	 0)


static void _bt_load(Relation index, BTSpool *btspool);
static void _bt_buildadd(Relation index, BTPageState *state,
						 BTItem bti, int flags);
static BTItem _bt_minitem(Page opage, BlockNumber oblkno, int atend);
static BTPageState *_bt_pagestate(Relation index, int flags, int level);
static void _bt_uppershutdown(Relation index, BTPageState *state);


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
	if (Show_btree_build_stats)
	{
		fprintf(StatFp, "BTREE BUILD (Spool) STATISTICS\n");
		ShowUsage();
		ResetUsage();
	}
#endif /* BTREE_BUILD_STATS */
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
_bt_pagestate(Relation index, int flags, int level)
{
	BTPageState *state = (BTPageState *) palloc(sizeof(BTPageState));

	MemSet((char *) state, 0, sizeof(BTPageState));
	_bt_blnewpage(index, &(state->btps_buf), &(state->btps_page), flags);
	state->btps_lastoff = P_HIKEY;
	state->btps_next = (BTPageState *) NULL;
	state->btps_level = level;

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
 * add an item to a disk page from the sort output.
 *
 * we must be careful to observe the following restrictions, placed
 * upon us by the conventions in nbtsearch.c:
 * - rightmost pages start data items at P_HIKEY instead of at
 *	 P_FIRSTKEY.
 *
 * a leaf page being built looks like:
 *
 * +----------------+---------------------------------+
 * | PageHeaderData | linp0 linp1 linp2 ...			  |
 * +-----------+----+---------------------------------+
 * | ... linpN |									  |
 * +-----------+--------------------------------------+
 * |	 ^ last										  |
 * |												  |
 * +-------------+------------------------------------+
 * |			 | itemN ...						  |
 * +-------------+------------------+-----------------+
 * |		  ... item3 item2 item1 | "special space" |
 * +--------------------------------+-----------------+
 *
 * contrast this with the diagram in bufpage.h; note the mismatch
 * between linps and items.  this is because we reserve linp0 as a
 * placeholder for the pointer to the "high key" item; when we have
 * filled up the page, we will set linp0 to point to itemN and clear
 * linpN.
 *
 * 'last' pointer indicates the last offset added to the page.
 */
static void
_bt_buildadd(Relation index, BTPageState *state, BTItem bti, int flags)
{
	Buffer		nbuf;
	Page		npage;
	OffsetNumber last_off;
	Size		pgspc;
	Size		btisz;

	nbuf = state->btps_buf;
	npage = state->btps_page;
	last_off = state->btps_lastoff;

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

	if (pgspc < btisz)
	{
		/*
		 * Item won't fit on this page, so finish off the page and
		 * write it out.
		 */
		Buffer		obuf = nbuf;
		Page		opage = npage;
		ItemId		ii;
		ItemId		hii;
		BTItem		nbti;

		_bt_blnewpage(index, &nbuf, &npage, flags);

		/*
		 * We copy the last item on the page into the new page, and then
		 * rearrange the old page so that the 'last item' becomes its high
		 * key rather than a true data item.
		 *
		 * note that since we always copy an item to the new page,
		 * 'bti' will never be the first data item on the new page.
		 */
		ii = PageGetItemId(opage, last_off);
		if (PageAddItem(npage, PageGetItem(opage, ii), ii->lp_len,
						P_FIRSTKEY, LP_USED) == InvalidOffsetNumber)
			elog(FATAL, "btree: failed to add item to the page in _bt_sort (1)");
#ifdef FASTBUILD_DEBUG
		{
			bool		isnull;
			BTItem		tmpbti =
				(BTItem) PageGetItem(npage, PageGetItemId(npage, P_FIRSTKEY));
			Datum		d = index_getattr(&(tmpbti->bti_itup), 1,
										  index->rd_att, &isnull);

			printf("_bt_buildadd: moved <%x> to offset %d at level %d\n",
				   d, P_FIRSTKEY, state->btps_level);
		}
#endif

		/*
		 * Move 'last' into the high key position on opage
		 */
		hii = PageGetItemId(opage, P_HIKEY);
		*hii = *ii;
		ii->lp_flags &= ~LP_USED;
		((PageHeader) opage)->pd_lower -= sizeof(ItemIdData);

		/*
		 * Reset last_off to point to new page
		 */
		last_off = PageGetMaxOffsetNumber(npage);

		/*
		 * set the page (side link) pointers.
		 */
		{
			BTPageOpaque oopaque = (BTPageOpaque) PageGetSpecialPointer(opage);
			BTPageOpaque nopaque = (BTPageOpaque) PageGetSpecialPointer(npage);

			oopaque->btpo_next = BufferGetBlockNumber(nbuf);
			nopaque->btpo_prev = BufferGetBlockNumber(obuf);
			nopaque->btpo_next = P_NONE;
		}

		/*
		 * Link the old buffer into its parent, using its minimum key.
		 * If we don't have a parent, we have to create one;
		 * this adds a new btree level.
		 */
		if (state->btps_next == (BTPageState *) NULL)
		{
			state->btps_next =
				_bt_pagestate(index, 0, state->btps_level + 1);
		}
		nbti = _bt_minitem(opage, BufferGetBlockNumber(obuf), 0);
		_bt_buildadd(index, state->btps_next, nbti, 0);
		pfree((void *) nbti);

		/*
		 * write out the old stuff.  we never want to see it again, so we
		 * can give up our lock (if we had one; BuildingBtree is set, so
		 * we aren't locking).
		 */
		_bt_wrtbuf(index, obuf);
	}

	/*
	 * Add the new item into the current page.
	 */
	last_off = OffsetNumberNext(last_off);
	if (PageAddItem(npage, (Item) bti, btisz,
					last_off, LP_USED) == InvalidOffsetNumber)
		elog(FATAL, "btree: failed to add item to the page in _bt_sort (2)");
#ifdef FASTBUILD_DEBUG
	{
		bool		isnull;
		Datum		d = index_getattr(&(bti->bti_itup), 1, index->rd_att, &isnull);

		printf("_bt_buildadd: inserted <%x> at offset %d at level %d\n",
			   d, last_off, state->btps_level);
	}
#endif

	state->btps_buf = nbuf;
	state->btps_page = npage;
	state->btps_lastoff = last_off;
}

/*
 * Finish writing out the completed btree.
 */
static void
_bt_uppershutdown(Relation index, BTPageState *state)
{
	BTPageState *s;
	BlockNumber blkno;
	BTPageOpaque opaque;
	BTItem		bti;

	/*
	 * Each iteration of this loop completes one more level of the tree.
	 */
	for (s = state; s != (BTPageState *) NULL; s = s->btps_next)
	{
		blkno = BufferGetBlockNumber(s->btps_buf);
		opaque = (BTPageOpaque) PageGetSpecialPointer(s->btps_page);

		/*
		 * We have to link the last page on this level to somewhere.
		 *
		 * If we're at the top, it's the root, so attach it to the metapage.
		 * Otherwise, add an entry for it to its parent using its minimum
		 * key.  This may cause the last page of the parent level to split,
		 * but that's not a problem -- we haven't gotten to it yet.
		 */
		if (s->btps_next == (BTPageState *) NULL)
		{
			opaque->btpo_flags |= BTP_ROOT;
			_bt_metaproot(index, blkno, s->btps_level + 1);
		}
		else
		{
			bti = _bt_minitem(s->btps_page, blkno, 0);
			_bt_buildadd(index, s->btps_next, bti, 0);
			pfree((void *) bti);
		}

		/*
		 * This is the rightmost page, so the ItemId array needs to be
		 * slid back one slot.  Then we can dump out the page.
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
	BTPageState *state = NULL;

	for (;;)
	{
		BTItem		bti;
		bool		should_free;

		bti = (BTItem) tuplesort_getindextuple(btspool->sortstate, true,
											   &should_free);
		if (bti == (BTItem) NULL)
			break;

		/* When we see first tuple, create first index page */
		if (state == NULL)
			state = _bt_pagestate(index, BTP_LEAF, 0);

		_bt_buildadd(index, state, bti, BTP_LEAF);
		if (should_free)
			pfree((void *) bti);
	}

	if (state != NULL)
		_bt_uppershutdown(index, state);
}
