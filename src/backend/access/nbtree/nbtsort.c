/*-------------------------------------------------------------------------
 *
 * nbtsort.c
 *		Build a btree from sorted input by loading leaf pages sequentially.
 *
 * NOTES
 *
 * We use tuplesort.c to sort the given index tuples into order.
 * Then we scan the index tuples in order and build the btree pages
 * for each level.	We load source tuples into leaf-level pages.
 * Whenever we fill a page at one level, we add a link to it to its
 * parent level (starting a new parent level if necessary).  When
 * done, we write out each final page on each level, adding it to
 * its parent level.  When we have only one page on a level, it must be
 * the root -- it can be attached to the btree metapage and we are done.
 *
 * This code is moderately slow (~10% slower) compared to the regular
 * btree (insertion) build code on sorted or well-clustered data.  On
 * random data, however, the insertion build code is unusable -- the
 * difference on a 60MB heap is a factor of 15 because the random
 * probes into the btree thrash the buffer pool.  (NOTE: the above
 * "10%" estimate is probably obsolete, since it refers to an old and
 * not very good external sort implementation that used to exist in
 * this module.  tuplesort.c is almost certainly faster.)
 *
 * It is not wise to pack the pages entirely full, since then *any*
 * insertion would cause a split (and not only of the leaf page; the need
 * for a split would cascade right up the tree).  The steady-state load
 * factor for btrees is usually estimated at 70%.  We choose to pack leaf
 * pages to 90% and upper pages to 70%.  This gives us reasonable density
 * (there aren't many upper pages if the keys are reasonable-size) without
 * incurring a lot of cascading splits during early insertions.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/nbtree/nbtsort.c,v 1.77 2003/09/29 23:40:26 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/nbtree.h"
#include "miscadmin.h"
#include "utils/tuplesort.h"


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
 * Status record for a btree page being built.	We have one of these
 * for each active tree level.
 *
 * The reason we need to store a copy of the minimum key is that we'll
 * need to propagate it to the parent node when this page is linked
 * into its parent.  However, if the page is not a leaf page, the first
 * entry on the page doesn't need to contain a key, so we will not have
 * stored the key itself on the page.  (You might think we could skip
 * copying the minimum key on leaf pages, but actually we must have a
 * writable copy anyway because we'll poke the page's address into it
 * before passing it up to the parent...)
 */
typedef struct BTPageState
{
	Buffer		btps_buf;		/* current buffer & page */
	Page		btps_page;
	BTItem		btps_minkey;	/* copy of minimum key (first item) on
								 * page */
	OffsetNumber btps_lastoff;	/* last item offset loaded */
	uint32		btps_level;		/* tree level (0 = leaf) */
	Size		btps_full;		/* "full" if less than this much free
								 * space */
	struct BTPageState *btps_next;		/* link to parent level, if any */
} BTPageState;


#define BTITEMSZ(btitem) \
	((btitem) ? \
	 (IndexTupleDSize((btitem)->bti_itup) + \
	  (sizeof(BTItemData) - sizeof(IndexTupleData))) : \
	 0)


static void _bt_blnewpage(Relation index, Buffer *buf, Page *page,
			  uint32 level);
static BTPageState *_bt_pagestate(Relation index, uint32 level);
static void _bt_slideleft(Relation index, Buffer buf, Page page);
static void _bt_sortaddtup(Page page, Size itemsize,
			   BTItem btitem, OffsetNumber itup_off);
static void _bt_buildadd(Relation index, BTPageState *state, BTItem bti);
static void _bt_uppershutdown(Relation index, BTPageState *state);
static void _bt_load(Relation index, BTSpool *btspool, BTSpool *btspool2);


/*
 * Interface routines
 */


/*
 * create and initialize a spool structure
 */
BTSpool *
_bt_spoolinit(Relation index, bool isunique)
{
	BTSpool    *btspool = (BTSpool *) palloc0(sizeof(BTSpool));

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
_bt_leafbuild(BTSpool *btspool, BTSpool *btspool2)
{
#ifdef BTREE_BUILD_STATS
	if (log_btree_build_stats)
	{
		ShowUsage("BTREE BUILD (Spool) STATISTICS");
		ResetUsage();
	}
#endif   /* BTREE_BUILD_STATS */

	tuplesort_performsort(btspool->sortstate);
	if (btspool2)
		tuplesort_performsort(btspool2->sortstate);
	_bt_load(btspool->index, btspool, btspool2);
}


/*
 * Internal routines.
 */


/*
 * allocate a new, clean btree page, not linked to any siblings.
 */
static void
_bt_blnewpage(Relation index, Buffer *buf, Page *page, uint32 level)
{
	BTPageOpaque opaque;

	*buf = _bt_getbuf(index, P_NEW, BT_WRITE);
	*page = BufferGetPage(*buf);

	/* Zero the page and set up standard page header info */
	_bt_pageinit(*page, BufferGetPageSize(*buf));

	/* Initialize BT opaque state */
	opaque = (BTPageOpaque) PageGetSpecialPointer(*page);
	opaque->btpo_prev = opaque->btpo_next = P_NONE;
	opaque->btpo.level = level;
	opaque->btpo_flags = (level > 0) ? 0 : BTP_LEAF;

	/* Make the P_HIKEY line pointer appear allocated */
	((PageHeader) *page)->pd_lower += sizeof(ItemIdData);
}

/*
 * emit a completed btree page, and release the lock and pin on it.
 * This is essentially _bt_wrtbuf except we also emit a WAL record.
 */
static void
_bt_blwritepage(Relation index, Buffer buf)
{
	Page		pg = BufferGetPage(buf);

	/* NO ELOG(ERROR) from here till newpage op is logged */
	START_CRIT_SECTION();

	/* XLOG stuff */
	if (!index->rd_istemp)
	{
		xl_btree_newpage xlrec;
		XLogRecPtr	recptr;
		XLogRecData rdata[2];

		xlrec.node = index->rd_node;
		xlrec.blkno = BufferGetBlockNumber(buf);

		rdata[0].buffer = InvalidBuffer;
		rdata[0].data = (char *) &xlrec;
		rdata[0].len = SizeOfBtreeNewpage;
		rdata[0].next = &(rdata[1]);

		rdata[1].buffer = buf;
		rdata[1].data = (char *) pg;
		rdata[1].len = BLCKSZ;
		rdata[1].next = NULL;

		recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_NEWPAGE, rdata);

		PageSetLSN(pg, recptr);
		PageSetSUI(pg, ThisStartUpID);
	}

	END_CRIT_SECTION();

	_bt_wrtbuf(index, buf);
}

/*
 * allocate and initialize a new BTPageState.  the returned structure
 * is suitable for immediate use by _bt_buildadd.
 */
static BTPageState *
_bt_pagestate(Relation index, uint32 level)
{
	BTPageState *state = (BTPageState *) palloc0(sizeof(BTPageState));

	/* create initial page */
	_bt_blnewpage(index, &(state->btps_buf), &(state->btps_page), level);

	state->btps_minkey = (BTItem) NULL;
	/* initialize lastoff so first item goes into P_FIRSTKEY */
	state->btps_lastoff = P_HIKEY;
	state->btps_level = level;
	/* set "full" threshold based on level.  See notes at head of file. */
	if (level > 0)
		state->btps_full = (PageGetPageSize(state->btps_page) * 3) / 10;
	else
		state->btps_full = PageGetPageSize(state->btps_page) / 10;
	/* no parent level, yet */
	state->btps_next = (BTPageState *) NULL;

	return state;
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
 * Add an item to a page being built.
 *
 * The main difference between this routine and a bare PageAddItem call
 * is that this code knows that the leftmost data item on a non-leaf
 * btree page doesn't need to have a key.  Therefore, it strips such
 * items down to just the item header.
 *
 * This is almost like nbtinsert.c's _bt_pgaddtup(), but we can't use
 * that because it assumes that P_RIGHTMOST() will return the correct
 * answer for the page.  Here, we don't know yet if the page will be
 * rightmost.  Offset P_FIRSTKEY is always the first data key.
 */
static void
_bt_sortaddtup(Page page,
			   Size itemsize,
			   BTItem btitem,
			   OffsetNumber itup_off)
{
	BTPageOpaque opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	BTItemData	truncitem;

	if (!P_ISLEAF(opaque) && itup_off == P_FIRSTKEY)
	{
		memcpy(&truncitem, btitem, sizeof(BTItemData));
		truncitem.bti_itup.t_info = sizeof(BTItemData);
		btitem = &truncitem;
		itemsize = sizeof(BTItemData);
	}

	if (PageAddItem(page, (Item) btitem, itemsize, itup_off,
					LP_USED) == InvalidOffsetNumber)
		elog(ERROR, "failed to add item to the index page");
}

/*----------
 * Add an item to a disk page from the sort output.
 *
 * We must be careful to observe the page layout conventions of nbtsearch.c:
 * - rightmost pages start data items at P_HIKEY instead of at P_FIRSTKEY.
 * - on non-leaf pages, the key portion of the first item need not be
 *	 stored, we should store only the link.
 *
 * A leaf page being built looks like:
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
 * Contrast this with the diagram in bufpage.h; note the mismatch
 * between linps and items.  This is because we reserve linp0 as a
 * placeholder for the pointer to the "high key" item; when we have
 * filled up the page, we will set linp0 to point to itemN and clear
 * linpN.  On the other hand, if we find this is the last (rightmost)
 * page, we leave the items alone and slide the linp array over.
 *
 * 'last' pointer indicates the last offset added to the page.
 *----------
 */
static void
_bt_buildadd(Relation index, BTPageState *state, BTItem bti)
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
	if (btisz > BTMaxItemSize(npage))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("index row size %lu exceeds btree maximum, %lu",
						(unsigned long) btisz,
						(unsigned long) BTMaxItemSize(npage))));

	if (pgspc < btisz || pgspc < state->btps_full)
	{
		/*
		 * Item won't fit on this page, or we feel the page is full enough
		 * already.  Finish off the page and write it out.
		 */
		Buffer		obuf = nbuf;
		Page		opage = npage;
		ItemId		ii;
		ItemId		hii;
		BTItem		obti;

		/* Create new page on same level */
		_bt_blnewpage(index, &nbuf, &npage, state->btps_level);

		/*
		 * We copy the last item on the page into the new page, and then
		 * rearrange the old page so that the 'last item' becomes its high
		 * key rather than a true data item.  There had better be at least
		 * two items on the page already, else the page would be empty of
		 * useful data.  (Hence, we must allow pages to be packed at least
		 * 2/3rds full; the 70% figure used above is close to minimum.)
		 */
		Assert(last_off > P_FIRSTKEY);
		ii = PageGetItemId(opage, last_off);
		obti = (BTItem) PageGetItem(opage, ii);
		_bt_sortaddtup(npage, ItemIdGetLength(ii), obti, P_FIRSTKEY);

		/*
		 * Move 'last' into the high key position on opage
		 */
		hii = PageGetItemId(opage, P_HIKEY);
		*hii = *ii;
		ii->lp_flags &= ~LP_USED;
		((PageHeader) opage)->pd_lower -= sizeof(ItemIdData);

		/*
		 * Link the old buffer into its parent, using its minimum key. If
		 * we don't have a parent, we have to create one; this adds a new
		 * btree level.
		 */
		if (state->btps_next == (BTPageState *) NULL)
			state->btps_next = _bt_pagestate(index, state->btps_level + 1);

		Assert(state->btps_minkey != NULL);
		ItemPointerSet(&(state->btps_minkey->bti_itup.t_tid),
					   BufferGetBlockNumber(obuf), P_HIKEY);
		_bt_buildadd(index, state->btps_next, state->btps_minkey);
		pfree((void *) state->btps_minkey);

		/*
		 * Save a copy of the minimum key for the new page.  We have to
		 * copy it off the old page, not the new one, in case we are not
		 * at leaf level.
		 */
		state->btps_minkey = _bt_formitem(&(obti->bti_itup));

		/*
		 * Set the sibling links for both pages.
		 */
		{
			BTPageOpaque oopaque = (BTPageOpaque) PageGetSpecialPointer(opage);
			BTPageOpaque nopaque = (BTPageOpaque) PageGetSpecialPointer(npage);

			oopaque->btpo_next = BufferGetBlockNumber(nbuf);
			nopaque->btpo_prev = BufferGetBlockNumber(obuf);
			nopaque->btpo_next = P_NONE;		/* redundant */
		}

		/*
		 * Write out the old page.	We never want to see it again, so we
		 * can give up our lock.
		 */
		_bt_blwritepage(index, obuf);

		/*
		 * Reset last_off to point to new page
		 */
		last_off = P_FIRSTKEY;
	}

	/*
	 * If the new item is the first for its page, stash a copy for later.
	 * Note this will only happen for the first item on a level; on later
	 * pages, the first item for a page is copied from the prior page in
	 * the code above.
	 */
	if (last_off == P_HIKEY)
	{
		Assert(state->btps_minkey == NULL);
		state->btps_minkey = _bt_formitem(&(bti->bti_itup));
	}

	/*
	 * Add the new item into the current page.
	 */
	last_off = OffsetNumberNext(last_off);
	_bt_sortaddtup(npage, btisz, bti, last_off);

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
	BlockNumber	rootblkno = P_NONE;
	uint32		rootlevel = 0;

	/*
	 * Each iteration of this loop completes one more level of the tree.
	 */
	for (s = state; s != (BTPageState *) NULL; s = s->btps_next)
	{
		BlockNumber blkno;
		BTPageOpaque opaque;

		blkno = BufferGetBlockNumber(s->btps_buf);
		opaque = (BTPageOpaque) PageGetSpecialPointer(s->btps_page);

		/*
		 * We have to link the last page on this level to somewhere.
		 *
		 * If we're at the top, it's the root, so attach it to the metapage.
		 * Otherwise, add an entry for it to its parent using its minimum
		 * key.  This may cause the last page of the parent level to
		 * split, but that's not a problem -- we haven't gotten to it yet.
		 */
		if (s->btps_next == (BTPageState *) NULL)
		{
			opaque->btpo_flags |= BTP_ROOT;
			rootblkno = blkno;
			rootlevel = s->btps_level;
		}
		else
		{
			Assert(s->btps_minkey != NULL);
			ItemPointerSet(&(s->btps_minkey->bti_itup.t_tid),
						   blkno, P_HIKEY);
			_bt_buildadd(index, s->btps_next, s->btps_minkey);
			pfree((void *) s->btps_minkey);
			s->btps_minkey = NULL;
		}

		/*
		 * This is the rightmost page, so the ItemId array needs to be
		 * slid back one slot.	Then we can dump out the page.
		 */
		_bt_slideleft(index, s->btps_buf, s->btps_page);
		_bt_blwritepage(index, s->btps_buf);
	}

	/*
	 * As the last step in the process, update the metapage to point to
	 * the new root (unless we had no data at all, in which case it's
	 * left pointing to "P_NONE").  This changes the index to the "valid"
	 * state by updating its magic number.
	 */
	_bt_metaproot(index, rootblkno, rootlevel);
}

/*
 * Read tuples in correct sort order from tuplesort, and load them into
 * btree leaves.
 */
static void
_bt_load(Relation index, BTSpool *btspool, BTSpool *btspool2)
{
	BTPageState *state = NULL;
	bool		merge = (btspool2 != NULL);
	BTItem		bti,
				bti2 = NULL;
	bool		should_free,
				should_free2,
				load1;
	TupleDesc	tupdes = RelationGetDescr(index);
	int			i,
				keysz = RelationGetNumberOfAttributes(index);
	ScanKey		indexScanKey = NULL;

	if (merge)
	{
		/*
		 * Another BTSpool for dead tuples exists. Now we have to merge
		 * btspool and btspool2.
		 */
		ScanKey		entry;
		Datum		attrDatum1,
					attrDatum2;
		bool		isFirstNull,
					isSecondNull;
		int32		compare;

		/* the preparation of merge */
		bti = (BTItem) tuplesort_getindextuple(btspool->sortstate, true, &should_free);
		bti2 = (BTItem) tuplesort_getindextuple(btspool2->sortstate, true, &should_free2);
		indexScanKey = _bt_mkscankey_nodata(index);
		for (;;)
		{
			load1 = true;		/* load BTSpool next ? */
			if (NULL == bti2)
			{
				if (NULL == bti)
					break;
			}
			else if (NULL != bti)
			{

				for (i = 1; i <= keysz; i++)
				{
					entry = indexScanKey + i - 1;
					attrDatum1 = index_getattr((IndexTuple) bti, i, tupdes, &isFirstNull);
					attrDatum2 = index_getattr((IndexTuple) bti2, i, tupdes, &isSecondNull);
					if (isFirstNull)
					{
						if (!isSecondNull)
						{
							load1 = false;
							break;
						}
					}
					else if (isSecondNull)
						break;
					else
					{
						compare = DatumGetInt32(FunctionCall2(&entry->sk_func, attrDatum1, attrDatum2));
						if (compare > 0)
						{
							load1 = false;
							break;
						}
						else if (compare < 0)
							break;
					}
				}
			}
			else
				load1 = false;

			/* When we see first tuple, create first index page */
			if (state == NULL)
				state = _bt_pagestate(index, 0);

			if (load1)
			{
				_bt_buildadd(index, state, bti);
				if (should_free)
					pfree((void *) bti);
				bti = (BTItem) tuplesort_getindextuple(btspool->sortstate, true, &should_free);
			}
			else
			{
				_bt_buildadd(index, state, bti2);
				if (should_free2)
					pfree((void *) bti2);
				bti2 = (BTItem) tuplesort_getindextuple(btspool2->sortstate, true, &should_free2);
			}
		}
		_bt_freeskey(indexScanKey);
	}
	else
	{
		/* merge is unnecessary */
		while (bti = (BTItem) tuplesort_getindextuple(btspool->sortstate, true, &should_free), bti != (BTItem) NULL)
		{
			/* When we see first tuple, create first index page */
			if (state == NULL)
				state = _bt_pagestate(index, 0);

			_bt_buildadd(index, state, bti);
			if (should_free)
				pfree((void *) bti);
		}
	}

	/* Close down final pages and rewrite the metapage */
	_bt_uppershutdown(index, state);
}
