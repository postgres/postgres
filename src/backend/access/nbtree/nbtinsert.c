/*-------------------------------------------------------------------------
 *
 * btinsert.c--
 *	  Item insertion in Lehman and Yao btrees for Postgres.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/nbtree/nbtinsert.c,v 1.23 1998/01/05 03:29:45 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <utils/memutils.h>
#include <storage/bufpage.h>
#include <access/nbtree.h>
#include <access/heapam.h>
#include <storage/bufmgr.h>
#include <fmgr.h>

#ifndef HAVE_MEMMOVE
#include <regex/utils.h>
#else
#include <string.h>
#endif

static InsertIndexResult _bt_insertonpg(Relation rel, Buffer buf, BTStack stack, int keysz, ScanKey scankey, BTItem btitem, BTItem afteritem);
static Buffer _bt_split(Relation rel, Buffer buf, OffsetNumber firstright);
static OffsetNumber _bt_findsplitloc(Relation rel, Page page, OffsetNumber start, OffsetNumber maxoff, Size llimit);
static void _bt_newroot(Relation rel, Buffer lbuf, Buffer rbuf);
static OffsetNumber _bt_pgaddtup(Relation rel, Buffer buf, int keysz, ScanKey itup_scankey, Size itemsize, BTItem btitem, BTItem afteritem);
static bool _bt_goesonpg(Relation rel, Buffer buf, Size keysz, ScanKey scankey, BTItem afteritem);
static void _bt_updateitem(Relation rel, Size keysz, Buffer buf, BTItem oldItem, BTItem newItem);
static bool _bt_isequal(TupleDesc itupdesc, Page page, OffsetNumber offnum, int keysz, ScanKey scankey);

/*
 *	_bt_doinsert() -- Handle insertion of a single btitem in the tree.
 *
 *		This routine is called by the public interface routines, btbuild
 *		and btinsert.  By here, btitem is filled in, and has a unique
 *		(xid, seqno) pair.
 */
InsertIndexResult
_bt_doinsert(Relation rel, BTItem btitem, bool index_is_unique, Relation heapRel)
{
	ScanKey		itup_scankey;
	IndexTuple	itup;
	BTStack		stack;
	Buffer		buf;
	BlockNumber blkno;
	int			natts = rel->rd_rel->relnatts;
	InsertIndexResult res;

	itup = &(btitem->bti_itup);

	/* we need a scan key to do our search, so build one */
	itup_scankey = _bt_mkscankey(rel, itup);

	/* find the page containing this key */
	stack = _bt_search(rel, natts, itup_scankey, &buf);

	blkno = BufferGetBlockNumber(buf);

	/* trade in our read lock for a write lock */
	_bt_relbuf(rel, buf, BT_READ);
	buf = _bt_getbuf(rel, blkno, BT_WRITE);

	/*
	 * If the page was split between the time that we surrendered our read
	 * lock and acquired our write lock, then this page may no longer be
	 * the right place for the key we want to insert.  In this case, we
	 * need to move right in the tree.	See Lehman and Yao for an
	 * excruciatingly precise description.
	 */

	buf = _bt_moveright(rel, buf, natts, itup_scankey, BT_WRITE);

	/* if we're not allowing duplicates, make sure the key isn't */
	/* already in the node */
	if (index_is_unique)
	{
		OffsetNumber offset,
					maxoff;
		Page		page;

		page = BufferGetPage(buf);
		maxoff = PageGetMaxOffsetNumber(page);

		offset = _bt_binsrch(rel, buf, natts, itup_scankey, BT_DESCENT);

		/* make sure the offset we're given points to an actual */
		/* key on the page before trying to compare it */
		if (!PageIsEmpty(page) && offset <= maxoff)
		{
			TupleDesc	itupdesc;
			BTItem		btitem;
			IndexTuple	itup;
			HeapTuple	htup;
			BTPageOpaque opaque;
			Buffer		nbuf;
			BlockNumber blkno;

			itupdesc = RelationGetTupleDescriptor(rel);
			nbuf = InvalidBuffer;
			opaque = (BTPageOpaque) PageGetSpecialPointer(page);

			/*
			 * _bt_compare returns 0 for (1,NULL) and (1,NULL) - this's
			 * how we handling NULLs - and so we must not use _bt_compare
			 * in real comparison, but only for ordering/finding items on
			 * pages. - vadim 03/24/97
			 *
			 * while ( !_bt_compare (rel, itupdesc, page, natts,
			 * itup_scankey, offset) )
			 */
			while (_bt_isequal(itupdesc, page, offset, natts, itup_scankey))
			{					/* they're equal */
				btitem = (BTItem) PageGetItem(page, PageGetItemId(page, offset));
				itup = &(btitem->bti_itup);
				htup = heap_fetch(heapRel, true, &(itup->t_tid), NULL);
				if (htup != (HeapTuple) NULL)
				{				/* it is a duplicate */
					elog(ABORT, "Cannot insert a duplicate key into a unique index");
				}
				/* get next offnum */
				if (offset < maxoff)
				{
					offset = OffsetNumberNext(offset);
				}
				else
				{				/* move right ? */
					if (P_RIGHTMOST(opaque))
						break;
					if (!_bt_isequal(itupdesc, page, P_HIKEY,
									 natts, itup_scankey))
						break;

					/*
					 * min key of the right page is the same, ooh - so
					 * many dead duplicates...
					 */
					blkno = opaque->btpo_next;
					if (nbuf != InvalidBuffer)
						_bt_relbuf(rel, nbuf, BT_READ);
					for (nbuf = InvalidBuffer;;)
					{
						nbuf = _bt_getbuf(rel, blkno, BT_READ);
						page = BufferGetPage(nbuf);
						maxoff = PageGetMaxOffsetNumber(page);
						opaque = (BTPageOpaque) PageGetSpecialPointer(page);
						offset = P_RIGHTMOST(opaque) ? P_HIKEY : P_FIRSTKEY;
						if (!PageIsEmpty(page) && offset <= maxoff)
						{		/* Found some key */
							break;
						}
						else
						{		/* Empty or "pseudo"-empty page - get next */
							blkno = opaque->btpo_next;
							_bt_relbuf(rel, nbuf, BT_READ);
							nbuf = InvalidBuffer;
							if (blkno == P_NONE)
								break;
						}
					}
					if (nbuf == InvalidBuffer)
						break;
				}
			}
			if (nbuf != InvalidBuffer)
				_bt_relbuf(rel, nbuf, BT_READ);
		}
	}

	/* do the insertion */
	res = _bt_insertonpg(rel, buf, stack, natts, itup_scankey,
						 btitem, (BTItem) NULL);

	/* be tidy */
	_bt_freestack(stack);
	_bt_freeskey(itup_scankey);

	return (res);
}

/*
 *	_bt_insertonpg() -- Insert a tuple on a particular page in the index.
 *
 *		This recursive procedure does the following things:
 *
 *			+  if necessary, splits the target page.
 *			+  finds the right place to insert the tuple (taking into
 *			   account any changes induced by a split).
 *			+  inserts the tuple.
 *			+  if the page was split, pops the parent stack, and finds the
 *			   right place to insert the new child pointer (by walking
 *			   right using information stored in the parent stack).
 *			+  invoking itself with the appropriate tuple for the right
 *			   child page on the parent.
 *
 *		On entry, we must have the right buffer on which to do the
 *		insertion, and the buffer must be pinned and locked.  On return,
 *		we will have dropped both the pin and the write lock on the buffer.
 *
 *		The locking interactions in this code are critical.  You should
 *		grok Lehman and Yao's paper before making any changes.  In addition,
 *		you need to understand how we disambiguate duplicate keys in this
 *		implementation, in order to be able to find our location using
 *		L&Y "move right" operations.  Since we may insert duplicate user
 *		keys, and since these dups may propogate up the tree, we use the
 *		'afteritem' parameter to position ourselves correctly for the
 *		insertion on internal pages.
 */
static InsertIndexResult
_bt_insertonpg(Relation rel,
			   Buffer buf,
			   BTStack stack,
			   int keysz,
			   ScanKey scankey,
			   BTItem btitem,
			   BTItem afteritem)
{
	InsertIndexResult res;
	Page		page;
	BTPageOpaque lpageop;
	BlockNumber itup_blkno;
	OffsetNumber itup_off;
	OffsetNumber firstright = InvalidOffsetNumber;
	int			itemsz;
	bool		do_split = false;
	bool		keys_equal = false;

	page = BufferGetPage(buf);
	lpageop = (BTPageOpaque) PageGetSpecialPointer(page);

	itemsz = IndexTupleDSize(btitem->bti_itup)
		+ (sizeof(BTItemData) - sizeof(IndexTupleData));

	itemsz = DOUBLEALIGN(itemsz);		/* be safe, PageAddItem will do
										 * this but we need to be
										 * consistent */

	/*
	 * If we have to insert item on the leftmost page which is the first
	 * page in the chain of duplicates then: 1. if scankey == hikey (i.e.
	 * - new duplicate item) then insert it here; 2. if scankey < hikey
	 * then: 2.a if there is duplicate key(s) here - we force splitting;
	 * 2.b else - we may "eat" this page from duplicates chain.
	 */
	if (lpageop->btpo_flags & BTP_CHAIN)
	{
		OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
		ItemId		hitemid;
		BTItem		hitem;

		Assert(!P_RIGHTMOST(lpageop));
		hitemid = PageGetItemId(page, P_HIKEY);
		hitem = (BTItem) PageGetItem(page, hitemid);
		if (maxoff > P_HIKEY &&
			!_bt_itemcmp(rel, keysz, hitem,
			 (BTItem) PageGetItem(page, PageGetItemId(page, P_FIRSTKEY)),
						 BTEqualStrategyNumber))
			elog(FATAL, "btree: bad key on the page in the chain of duplicates");

		if (!_bt_skeycmp(rel, keysz, scankey, page, hitemid,
						 BTEqualStrategyNumber))
		{
			if (!P_LEFTMOST(lpageop))
				elog(FATAL, "btree: attempt to insert bad key on the non-leftmost page in the chain of duplicates");
			if (!_bt_skeycmp(rel, keysz, scankey, page, hitemid,
							 BTLessStrategyNumber))
				elog(FATAL, "btree: attempt to insert higher key on the leftmost page in the chain of duplicates");
			if (maxoff > P_HIKEY)		/* have duplicate(s) */
			{
				firstright = P_FIRSTKEY;
				do_split = true;
			}
			else
/* "eat" page */
			{
				Buffer		pbuf;
				Page		ppage;

				itup_blkno = BufferGetBlockNumber(buf);
				itup_off = PageAddItem(page, (Item) btitem, itemsz,
									   P_FIRSTKEY, LP_USED);
				if (itup_off == InvalidOffsetNumber)
					elog(FATAL, "btree: failed to add item");
				lpageop->btpo_flags &= ~BTP_CHAIN;
				pbuf = _bt_getstackbuf(rel, stack, BT_WRITE);
				ppage = BufferGetPage(pbuf);
				PageIndexTupleDelete(ppage, stack->bts_offset);
				pfree(stack->bts_btitem);
				stack->bts_btitem = _bt_formitem(&(btitem->bti_itup));
				ItemPointerSet(&(stack->bts_btitem->bti_itup.t_tid),
							   itup_blkno, P_HIKEY);
				_bt_wrtbuf(rel, buf);
				res = _bt_insertonpg(rel, pbuf, stack->bts_parent,
									 keysz, scankey, stack->bts_btitem,
									 NULL);
				ItemPointerSet(&(res->pointerData), itup_blkno, itup_off);
				return (res);
			}
		}
		else
		{
			keys_equal = true;
			if (PageGetFreeSpace(page) < itemsz)
				do_split = true;
		}
	}
	else if (PageGetFreeSpace(page) < itemsz)
		do_split = true;
	else if (PageGetFreeSpace(page) < 3 * itemsz + 2 * sizeof(ItemIdData))
	{
		OffsetNumber offnum = (P_RIGHTMOST(lpageop)) ? P_HIKEY : P_FIRSTKEY;
		OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
		ItemId		itid;
		BTItem		previtem,
					chkitem;
		Size		maxsize;
		Size		currsize;

		itid = PageGetItemId(page, offnum);
		previtem = (BTItem) PageGetItem(page, itid);
		maxsize = currsize = (ItemIdGetLength(itid) + sizeof(ItemIdData));
		for (offnum = OffsetNumberNext(offnum);
			 offnum <= maxoff; offnum = OffsetNumberNext(offnum))
		{
			itid = PageGetItemId(page, offnum);
			chkitem = (BTItem) PageGetItem(page, itid);
			if (!_bt_itemcmp(rel, keysz, previtem, chkitem,
							 BTEqualStrategyNumber))
			{
				if (currsize > maxsize)
					maxsize = currsize;
				currsize = 0;
				previtem = chkitem;
			}
			currsize += (ItemIdGetLength(itid) + sizeof(ItemIdData));
		}
		if (currsize > maxsize)
			maxsize = currsize;
		maxsize += sizeof(PageHeaderData) +
			DOUBLEALIGN(sizeof(BTPageOpaqueData));
		if (maxsize >= PageGetPageSize(page) / 2)
			do_split = true;
	}

	if (do_split)
	{
		Buffer		rbuf;
		Page		rpage;
		BTItem		ritem;
		BlockNumber rbknum;
		BTPageOpaque rpageop;
		Buffer		pbuf;
		Page		ppage;
		BTPageOpaque ppageop;
		BlockNumber bknum = BufferGetBlockNumber(buf);
		BTItem		lowLeftItem;
		OffsetNumber maxoff;
		bool		shifted = false;
		bool		left_chained = (lpageop->btpo_flags & BTP_CHAIN) ? true : false;

		/*
		 * If we have to split leaf page in the chain of duplicates by new
		 * duplicate then we try to look at our right sibling first.
		 */
		if ((lpageop->btpo_flags & BTP_CHAIN) &&
			(lpageop->btpo_flags & BTP_LEAF) && keys_equal)
		{
			bool		use_left = true;

			rbuf = _bt_getbuf(rel, lpageop->btpo_next, BT_WRITE);
			rpage = BufferGetPage(rbuf);
			rpageop = (BTPageOpaque) PageGetSpecialPointer(rpage);
			if (!P_RIGHTMOST(rpageop))	/* non-rightmost page */
			{					/* If we have the same hikey here then
								 * it's yet another page in chain. */
				if (_bt_skeycmp(rel, keysz, scankey, rpage,
								PageGetItemId(rpage, P_HIKEY),
								BTEqualStrategyNumber))
				{
					if (!(rpageop->btpo_flags & BTP_CHAIN))
						elog(FATAL, "btree: lost page in the chain of duplicates");
				}
				else if (_bt_skeycmp(rel, keysz, scankey, rpage,
									 PageGetItemId(rpage, P_HIKEY),
									 BTGreaterStrategyNumber))
					elog(FATAL, "btree: hikey is out of order");
				else if (rpageop->btpo_flags & BTP_CHAIN)

					/*
					 * If hikey > scankey then it's last page in chain and
					 * BTP_CHAIN must be OFF
					 */
					elog(FATAL, "btree: lost last page in the chain of duplicates");

				/* if there is room here then we use this page. */
				if (PageGetFreeSpace(rpage) > itemsz)
					use_left = false;
			}
			else
/* rightmost page */
			{
				Assert(!(rpageop->btpo_flags & BTP_CHAIN));
				/* if there is room here then we use this page. */
				if (PageGetFreeSpace(rpage) > itemsz)
					use_left = false;
			}
			if (!use_left)		/* insert on the right page */
			{
				_bt_relbuf(rel, buf, BT_WRITE);
				return (_bt_insertonpg(rel, rbuf, stack, keysz,
									   scankey, btitem, afteritem));
			}
			_bt_relbuf(rel, rbuf, BT_WRITE);
		}

		/*
		 * If after splitting un-chained page we'll got chain of pages
		 * with duplicates then we want to know 1. on which of two pages
		 * new btitem will go (current _bt_findsplitloc is quite bad); 2.
		 * what parent (if there's one) thinking about it (remember about
		 * deletions)
		 */
		else if (!(lpageop->btpo_flags & BTP_CHAIN))
		{
			OffsetNumber start = (P_RIGHTMOST(lpageop)) ? P_HIKEY : P_FIRSTKEY;
			Size		llimit;

			maxoff = PageGetMaxOffsetNumber(page);
			llimit = PageGetPageSize(page) - sizeof(PageHeaderData) -
				DOUBLEALIGN(sizeof(BTPageOpaqueData))
				+sizeof(ItemIdData);
			llimit /= 2;
			firstright = _bt_findsplitloc(rel, page, start, maxoff, llimit);

			if (_bt_itemcmp(rel, keysz,
				  (BTItem) PageGetItem(page, PageGetItemId(page, start)),
			 (BTItem) PageGetItem(page, PageGetItemId(page, firstright)),
							BTEqualStrategyNumber))
			{
				if (_bt_skeycmp(rel, keysz, scankey, page,
								PageGetItemId(page, firstright),
								BTLessStrategyNumber))

					/*
					 * force moving current items to the new page: new
					 * item will go on the current page.
					 */
					firstright = start;
				else

					/*
					 * new btitem >= firstright, start item == firstright
					 * - new chain of duplicates: if this non-leftmost
					 * leaf page and parent item < start item then force
					 * moving all items to the new page - current page
					 * will be "empty" after it.
					 */
				{
					if (!P_LEFTMOST(lpageop) &&
						(lpageop->btpo_flags & BTP_LEAF))
					{
						ItemPointerSet(&(stack->bts_btitem->bti_itup.t_tid),
									   bknum, P_HIKEY);
						pbuf = _bt_getstackbuf(rel, stack, BT_WRITE);
						if (_bt_itemcmp(rel, keysz, stack->bts_btitem,
										(BTItem) PageGetItem(page,
											 PageGetItemId(page, start)),
										BTLessStrategyNumber))
						{
							firstright = start;
							shifted = true;
						}
						_bt_relbuf(rel, pbuf, BT_WRITE);
					}
				}
			}					/* else - no new chain if start item <
								 * firstright one */
		}

		/* split the buffer into left and right halves */
		rbuf = _bt_split(rel, buf, firstright);

		/* which new page (left half or right half) gets the tuple? */
		if (_bt_goesonpg(rel, buf, keysz, scankey, afteritem))
		{
			/* left page */
			itup_off = _bt_pgaddtup(rel, buf, keysz, scankey,
									itemsz, btitem, afteritem);
			itup_blkno = BufferGetBlockNumber(buf);
		}
		else
		{
			/* right page */
			itup_off = _bt_pgaddtup(rel, rbuf, keysz, scankey,
									itemsz, btitem, afteritem);
			itup_blkno = BufferGetBlockNumber(rbuf);
		}

		maxoff = PageGetMaxOffsetNumber(page);
		if (shifted)
		{
			if (maxoff > P_FIRSTKEY)
				elog(FATAL, "btree: shifted page is not empty");
			lowLeftItem = (BTItem) NULL;
		}
		else
		{
			if (maxoff < P_FIRSTKEY)
				elog(FATAL, "btree: un-shifted page is empty");
			lowLeftItem = (BTItem) PageGetItem(page,
										PageGetItemId(page, P_FIRSTKEY));
			if (_bt_itemcmp(rel, keysz, lowLeftItem,
				(BTItem) PageGetItem(page, PageGetItemId(page, P_HIKEY)),
							BTEqualStrategyNumber))
				lpageop->btpo_flags |= BTP_CHAIN;
		}

		/*
		 * By here,
		 *
		 * +  our target page has been split; +  the original tuple has been
		 * inserted; +	we have write locks on both the old (left half)
		 * and new (right half) buffers, after the split; and +  we have
		 * the key we want to insert into the parent.
		 *
		 * Do the parent insertion.  We need to hold onto the locks for the
		 * child pages until we locate the parent, but we can release them
		 * before doing the actual insertion (see Lehman and Yao for the
		 * reasoning).
		 */

		if (stack == (BTStack) NULL)
		{

			/* create a new root node and release the split buffers */
			_bt_newroot(rel, buf, rbuf);
			_bt_relbuf(rel, buf, BT_WRITE);
			_bt_relbuf(rel, rbuf, BT_WRITE);

		}
		else
		{
			ScanKey		newskey;
			InsertIndexResult newres;
			BTItem		new_item;
			OffsetNumber upditem_offset = P_HIKEY;
			bool		do_update = false;
			bool		update_in_place = true;
			bool		parent_chained;

			/* form a index tuple that points at the new right page */
			rbknum = BufferGetBlockNumber(rbuf);
			rpage = BufferGetPage(rbuf);
			rpageop = (BTPageOpaque) PageGetSpecialPointer(rpage);

			/*
			 * By convention, the first entry (1) on every non-rightmost
			 * page is the high key for that page.	In order to get the
			 * lowest key on the new right page, we actually look at its
			 * second (2) entry.
			 */

			if (!P_RIGHTMOST(rpageop))
			{
				ritem = (BTItem) PageGetItem(rpage,
									   PageGetItemId(rpage, P_FIRSTKEY));
				if (_bt_itemcmp(rel, keysz, ritem,
								(BTItem) PageGetItem(rpage,
										  PageGetItemId(rpage, P_HIKEY)),
								BTEqualStrategyNumber))
					rpageop->btpo_flags |= BTP_CHAIN;
			}
			else
				ritem = (BTItem) PageGetItem(rpage,
										  PageGetItemId(rpage, P_HIKEY));

			/* get a unique btitem for this key */
			new_item = _bt_formitem(&(ritem->bti_itup));

			ItemPointerSet(&(new_item->bti_itup.t_tid), rbknum, P_HIKEY);

			/*
			 * Find the parent buffer and get the parent page.
			 *
			 * Oops - if we were moved right then we need to change stack
			 * item! We want to find parent pointing to where we are,
			 * right ?	  - vadim 05/27/97
			 */
			ItemPointerSet(&(stack->bts_btitem->bti_itup.t_tid),
						   bknum, P_HIKEY);
			pbuf = _bt_getstackbuf(rel, stack, BT_WRITE);
			ppage = BufferGetPage(pbuf);
			ppageop = (BTPageOpaque) PageGetSpecialPointer(ppage);
			parent_chained = ((ppageop->btpo_flags & BTP_CHAIN)) ? true : false;

			if (parent_chained && !left_chained)
				elog(FATAL, "nbtree: unexpected chained parent of unchained page");

			/*
			 * If the key of new_item is < than the key of the item in the
			 * parent page pointing to the left page (stack->bts_btitem),
			 * we have to update the latter key; otherwise the keys on the
			 * parent page wouldn't be monotonically increasing after we
			 * inserted the new pointer to the right page (new_item). This
			 * only happens if our left page is the leftmost page and a
			 * new minimum key had been inserted before, which is not
			 * reflected in the parent page but didn't matter so far. If
			 * there are duplicate keys and this new minimum key spills
			 * over to our new right page, we get an inconsistency if we
			 * don't update the left key in the parent page.
			 *
			 * Also, new duplicates handling code require us to update parent
			 * item if some smaller items left on the left page (which is
			 * possible in splitting leftmost page) and current parent
			 * item == new_item.		- vadim 05/27/97
			 */
			if (_bt_itemcmp(rel, keysz, stack->bts_btitem, new_item,
							BTGreaterStrategyNumber) ||
				(!shifted &&
				 _bt_itemcmp(rel, keysz, stack->bts_btitem,
							 new_item, BTEqualStrategyNumber) &&
				 _bt_itemcmp(rel, keysz, lowLeftItem,
							 new_item, BTLessStrategyNumber)))
			{
				do_update = true;

				/*
				 * figure out which key is leftmost (if the parent page is
				 * rightmost, too, it must be the root)
				 */
				if (P_RIGHTMOST(ppageop))
					upditem_offset = P_HIKEY;
				else
					upditem_offset = P_FIRSTKEY;
				if (!P_LEFTMOST(lpageop) ||
					stack->bts_offset != upditem_offset)
					elog(FATAL, "btree: items are out of order (leftmost %d, stack %u, update %u)",
						 P_LEFTMOST(lpageop), stack->bts_offset, upditem_offset);
			}

			if (do_update)
			{
				if (shifted)
					elog(FATAL, "btree: attempt to update parent for shifted page");

				/*
				 * Try to update in place. If out parent page is chained
				 * then we must forse insertion.
				 */
				if (!parent_chained &&
					DOUBLEALIGN(IndexTupleDSize(lowLeftItem->bti_itup)) ==
				DOUBLEALIGN(IndexTupleDSize(stack->bts_btitem->bti_itup)))
				{
					_bt_updateitem(rel, keysz, pbuf,
								   stack->bts_btitem, lowLeftItem);
					_bt_relbuf(rel, buf, BT_WRITE);
					_bt_relbuf(rel, rbuf, BT_WRITE);
				}
				else
				{
					update_in_place = false;
					PageIndexTupleDelete(ppage, upditem_offset);

					/*
					 * don't write anything out yet--we still have the
					 * write lock, and now we call another _bt_insertonpg
					 * to insert the correct key. First, make a new item,
					 * using the tuple data from lowLeftItem. Point it to
					 * the left child. Update it on the stack at the same
					 * time.
					 */
					pfree(stack->bts_btitem);
					stack->bts_btitem = _bt_formitem(&(lowLeftItem->bti_itup));
					ItemPointerSet(&(stack->bts_btitem->bti_itup.t_tid),
								   bknum, P_HIKEY);

					/*
					 * Unlock the children before doing this
					 *
					 * Mmm ... I foresee problems here. - vadim 06/10/97
					 */
					_bt_relbuf(rel, buf, BT_WRITE);
					_bt_relbuf(rel, rbuf, BT_WRITE);

					/*
					 * A regular _bt_binsrch should find the right place
					 * to put the new entry, since it should be lower than
					 * any other key on the page. Therefore set afteritem
					 * to NULL.
					 */
					newskey = _bt_mkscankey(rel, &(stack->bts_btitem->bti_itup));
					newres = _bt_insertonpg(rel, pbuf, stack->bts_parent,
									   keysz, newskey, stack->bts_btitem,
											NULL);

					pfree(newres);
					pfree(newskey);

					/*
					 * we have now lost our lock on the parent buffer, and
					 * need to get it back.
					 */
					pbuf = _bt_getstackbuf(rel, stack, BT_WRITE);
				}
			}
			else
			{
				_bt_relbuf(rel, buf, BT_WRITE);
				_bt_relbuf(rel, rbuf, BT_WRITE);
			}

			newskey = _bt_mkscankey(rel, &(new_item->bti_itup));

			afteritem = stack->bts_btitem;
			if (parent_chained && !update_in_place)
			{
				ppage = BufferGetPage(pbuf);
				ppageop = (BTPageOpaque) PageGetSpecialPointer(ppage);
				if (ppageop->btpo_flags & BTP_CHAIN)
					elog(FATAL, "btree: unexpected BTP_CHAIN flag in parent after update");
				if (P_RIGHTMOST(ppageop))
					elog(FATAL, "btree: chained parent is RIGHTMOST after update");
				maxoff = PageGetMaxOffsetNumber(ppage);
				if (maxoff != P_FIRSTKEY)
					elog(FATAL, "btree: FIRSTKEY was unexpected in parent after update");
				if (_bt_skeycmp(rel, keysz, newskey, ppage,
								PageGetItemId(ppage, P_FIRSTKEY),
								BTLessEqualStrategyNumber))
					elog(FATAL, "btree: parent FIRSTKEY is >= duplicate key after update");
				if (!_bt_skeycmp(rel, keysz, newskey, ppage,
								 PageGetItemId(ppage, P_HIKEY),
								 BTEqualStrategyNumber))
					elog(FATAL, "btree: parent HIGHKEY is not equal duplicate key after update");
				afteritem = (BTItem) NULL;
			}
			else if (left_chained && !update_in_place)
			{
				ppage = BufferGetPage(pbuf);
				ppageop = (BTPageOpaque) PageGetSpecialPointer(ppage);
				if (!P_RIGHTMOST(ppageop) &&
					_bt_skeycmp(rel, keysz, newskey, ppage,
								PageGetItemId(ppage, P_HIKEY),
								BTGreaterStrategyNumber))
					afteritem = (BTItem) NULL;
			}
			if (afteritem == (BTItem) NULL)
			{
				rbuf = _bt_getbuf(rel, ppageop->btpo_next, BT_WRITE);
				_bt_relbuf(rel, pbuf, BT_WRITE);
				pbuf = rbuf;
			}

			newres = _bt_insertonpg(rel, pbuf, stack->bts_parent,
									keysz, newskey, new_item,
									afteritem);

			/* be tidy */
			pfree(newres);
			pfree(newskey);
			pfree(new_item);
		}
	}
	else
	{
		itup_off = _bt_pgaddtup(rel, buf, keysz, scankey,
								itemsz, btitem, afteritem);
		itup_blkno = BufferGetBlockNumber(buf);

		_bt_relbuf(rel, buf, BT_WRITE);
	}

	/* by here, the new tuple is inserted */
	res = (InsertIndexResult) palloc(sizeof(InsertIndexResultData));
	ItemPointerSet(&(res->pointerData), itup_blkno, itup_off);

	return (res);
}

/*
 *	_bt_split() -- split a page in the btree.
 *
 *		On entry, buf is the page to split, and is write-locked and pinned.
 *		Returns the new right sibling of buf, pinned and write-locked.	The
 *		pin and lock on buf are maintained.
 */
static Buffer
_bt_split(Relation rel, Buffer buf, OffsetNumber firstright)
{
	Buffer		rbuf;
	Page		origpage;
	Page		leftpage,
				rightpage;
	BTPageOpaque ropaque,
				lopaque,
				oopaque;
	Buffer		sbuf;
	Page		spage;
	BTPageOpaque sopaque;
	Size		itemsz;
	ItemId		itemid;
	BTItem		item;
	OffsetNumber leftoff,
				rightoff;
	OffsetNumber start;
	OffsetNumber maxoff;
	OffsetNumber i;

	rbuf = _bt_getbuf(rel, P_NEW, BT_WRITE);
	origpage = BufferGetPage(buf);
	leftpage = PageGetTempPage(origpage, sizeof(BTPageOpaqueData));
	rightpage = BufferGetPage(rbuf);

	_bt_pageinit(rightpage, BufferGetPageSize(rbuf));
	_bt_pageinit(leftpage, BufferGetPageSize(buf));

	/* init btree private data */
	oopaque = (BTPageOpaque) PageGetSpecialPointer(origpage);
	lopaque = (BTPageOpaque) PageGetSpecialPointer(leftpage);
	ropaque = (BTPageOpaque) PageGetSpecialPointer(rightpage);

	/* if we're splitting this page, it won't be the root when we're done */
	oopaque->btpo_flags &= ~BTP_ROOT;
	oopaque->btpo_flags &= ~BTP_CHAIN;
	lopaque->btpo_flags = ropaque->btpo_flags = oopaque->btpo_flags;
	lopaque->btpo_prev = oopaque->btpo_prev;
	ropaque->btpo_prev = BufferGetBlockNumber(buf);
	lopaque->btpo_next = BufferGetBlockNumber(rbuf);
	ropaque->btpo_next = oopaque->btpo_next;

	/*
	 * If the page we're splitting is not the rightmost page at its level
	 * in the tree, then the first (0) entry on the page is the high key
	 * for the page.  We need to copy that to the right half.  Otherwise
	 * (meaning the rightmost page case), we should treat the line
	 * pointers beginning at zero as user data.
	 *
	 * We leave a blank space at the start of the line table for the left
	 * page.  We'll come back later and fill it in with the high key item
	 * we get from the right key.
	 */

	leftoff = P_FIRSTKEY;
	ropaque->btpo_next = oopaque->btpo_next;
	if (!P_RIGHTMOST(oopaque))
	{
		/* splitting a non-rightmost page, start at the first data item */
		start = P_FIRSTKEY;

		itemid = PageGetItemId(origpage, P_HIKEY);
		itemsz = ItemIdGetLength(itemid);
		item = (BTItem) PageGetItem(origpage, itemid);
		if (PageAddItem(rightpage, (Item) item, itemsz, P_HIKEY, LP_USED) == InvalidOffsetNumber)
			elog(FATAL, "btree: failed to add hikey to the right sibling");
		rightoff = P_FIRSTKEY;
	}
	else
	{
		/* splitting a rightmost page, "high key" is the first data item */
		start = P_HIKEY;

		/* the new rightmost page will not have a high key */
		rightoff = P_HIKEY;
	}
	maxoff = PageGetMaxOffsetNumber(origpage);
	if (firstright == InvalidOffsetNumber)
	{
		Size		llimit = PageGetFreeSpace(leftpage) / 2;

		firstright = _bt_findsplitloc(rel, origpage, start, maxoff, llimit);
	}

	for (i = start; i <= maxoff; i = OffsetNumberNext(i))
	{
		itemid = PageGetItemId(origpage, i);
		itemsz = ItemIdGetLength(itemid);
		item = (BTItem) PageGetItem(origpage, itemid);

		/* decide which page to put it on */
		if (i < firstright)
		{
			if (PageAddItem(leftpage, (Item) item, itemsz, leftoff,
							LP_USED) == InvalidOffsetNumber)
				elog(FATAL, "btree: failed to add item to the left sibling");
			leftoff = OffsetNumberNext(leftoff);
		}
		else
		{
			if (PageAddItem(rightpage, (Item) item, itemsz, rightoff,
							LP_USED) == InvalidOffsetNumber)
				elog(FATAL, "btree: failed to add item to the right sibling");
			rightoff = OffsetNumberNext(rightoff);
		}
	}

	/*
	 * Okay, page has been split, high key on right page is correct.  Now
	 * set the high key on the left page to be the min key on the right
	 * page.
	 */

	if (P_RIGHTMOST(ropaque))
	{
		itemid = PageGetItemId(rightpage, P_HIKEY);
	}
	else
	{
		itemid = PageGetItemId(rightpage, P_FIRSTKEY);
	}
	itemsz = ItemIdGetLength(itemid);
	item = (BTItem) PageGetItem(rightpage, itemid);

	/*
	 * We left a hole for the high key on the left page; fill it.  The
	 * modal crap is to tell the page manager to put the new item on the
	 * page and not screw around with anything else.  Whoever designed
	 * this interface has presumably crawled back into the dung heap they
	 * came from.  No one here will admit to it.
	 */

	PageManagerModeSet(OverwritePageManagerMode);
	if (PageAddItem(leftpage, (Item) item, itemsz, P_HIKEY, LP_USED) == InvalidOffsetNumber)
		elog(FATAL, "btree: failed to add hikey to the left sibling");
	PageManagerModeSet(ShufflePageManagerMode);

	/*
	 * By here, the original data page has been split into two new halves,
	 * and these are correct.  The algorithm requires that the left page
	 * never move during a split, so we copy the new left page back on top
	 * of the original.  Note that this is not a waste of time, since we
	 * also require (in the page management code) that the center of a
	 * page always be clean, and the most efficient way to guarantee this
	 * is just to compact the data by reinserting it into a new left page.
	 */

	PageRestoreTempPage(leftpage, origpage);

	/* write these guys out */
	_bt_wrtnorelbuf(rel, rbuf);
	_bt_wrtnorelbuf(rel, buf);

	/*
	 * Finally, we need to grab the right sibling (if any) and fix the
	 * prev pointer there.	We are guaranteed that this is deadlock-free
	 * since no other writer will be moving holding a lock on that page
	 * and trying to move left, and all readers release locks on a page
	 * before trying to fetch its neighbors.
	 */

	if (!P_RIGHTMOST(ropaque))
	{
		sbuf = _bt_getbuf(rel, ropaque->btpo_next, BT_WRITE);
		spage = BufferGetPage(sbuf);
		sopaque = (BTPageOpaque) PageGetSpecialPointer(spage);
		sopaque->btpo_prev = BufferGetBlockNumber(rbuf);

		/* write and release the old right sibling */
		_bt_wrtbuf(rel, sbuf);
	}

	/* split's done */
	return (rbuf);
}

/*
 *	_bt_findsplitloc() -- find a safe place to split a page.
 *
 *		In order to guarantee the proper handling of searches for duplicate
 *		keys, the first duplicate in the chain must either be the first
 *		item on the page after the split, or the entire chain must be on
 *		one of the two pages.  That is,
 *				[1 2 2 2 3 4 5]
 *		must become
 *				[1] [2 2 2 3 4 5]
 *		or
 *				[1 2 2 2] [3 4 5]
 *		but not
 *				[1 2 2] [2 3 4 5].
 *		However,
 *				[2 2 2 2 2 3 4]
 *		may be split as
 *				[2 2 2 2] [2 3 4].
 */
static OffsetNumber
_bt_findsplitloc(Relation rel,
				 Page page,
				 OffsetNumber start,
				 OffsetNumber maxoff,
				 Size llimit)
{
	OffsetNumber i;
	OffsetNumber saferight;
	ItemId		nxtitemid,
				safeitemid;
	BTItem		safeitem,
				nxtitem;
	Size		nbytes;
	int			natts;

	if (start >= maxoff)
		elog(FATAL, "btree: cannot split if start (%d) >= maxoff (%d)",
			 start, maxoff);
	natts = rel->rd_rel->relnatts;
	saferight = start;
	safeitemid = PageGetItemId(page, saferight);
	nbytes = ItemIdGetLength(safeitemid) + sizeof(ItemIdData);
	safeitem = (BTItem) PageGetItem(page, safeitemid);

	i = OffsetNumberNext(start);

	while (nbytes < llimit)
	{
		/* check the next item on the page */
		nxtitemid = PageGetItemId(page, i);
		nbytes += (ItemIdGetLength(nxtitemid) + sizeof(ItemIdData));
		nxtitem = (BTItem) PageGetItem(page, nxtitemid);

		/*
		 * Test against last known safe item: if the tuple we're looking
		 * at isn't equal to the last safe one we saw, then it's our new
		 * safe tuple.
		 */
		if (!_bt_itemcmp(rel, natts,
						 safeitem, nxtitem, BTEqualStrategyNumber))
		{
			safeitem = nxtitem;
			saferight = i;
		}
		if (i < maxoff)
			i = OffsetNumberNext(i);
		else
			break;
	}

	/*
	 * If the chain of dups starts at the beginning of the page and
	 * extends past the halfway mark, we can split it in the middle.
	 */

	if (saferight == start)
		saferight = i;

	if (saferight == maxoff && (maxoff - start) > 1)
		saferight = start + (maxoff - start) / 2;

	return (saferight);
}

/*
 *	_bt_newroot() -- Create a new root page for the index.
 *
 *		We've just split the old root page and need to create a new one.
 *		In order to do this, we add a new root page to the file, then lock
 *		the metadata page and update it.  This is guaranteed to be deadlock-
 *		free, because all readers release their locks on the metadata page
 *		before trying to lock the root, and all writers lock the root before
 *		trying to lock the metadata page.  We have a write lock on the old
 *		root page, so we have not introduced any cycles into the waits-for
 *		graph.
 *
 *		On entry, lbuf (the old root) and rbuf (its new peer) are write-
 *		locked.  We don't drop the locks in this routine; that's done by
 *		the caller.  On exit, a new root page exists with entries for the
 *		two new children.  The new root page is neither pinned nor locked.
 */
static void
_bt_newroot(Relation rel, Buffer lbuf, Buffer rbuf)
{
	Buffer		rootbuf;
	Page		lpage,
				rpage,
				rootpage;
	BlockNumber lbkno,
				rbkno;
	BlockNumber rootbknum;
	BTPageOpaque rootopaque;
	ItemId		itemid;
	BTItem		item;
	Size		itemsz;
	BTItem		new_item;

	/* get a new root page */
	rootbuf = _bt_getbuf(rel, P_NEW, BT_WRITE);
	rootpage = BufferGetPage(rootbuf);
	_bt_pageinit(rootpage, BufferGetPageSize(rootbuf));

	/* set btree special data */
	rootopaque = (BTPageOpaque) PageGetSpecialPointer(rootpage);
	rootopaque->btpo_prev = rootopaque->btpo_next = P_NONE;
	rootopaque->btpo_flags |= BTP_ROOT;

	/*
	 * Insert the internal tuple pointers.
	 */

	lbkno = BufferGetBlockNumber(lbuf);
	rbkno = BufferGetBlockNumber(rbuf);
	lpage = BufferGetPage(lbuf);
	rpage = BufferGetPage(rbuf);

	/*
	 * step over the high key on the left page while building the left
	 * page pointer.
	 */
	itemid = PageGetItemId(lpage, P_FIRSTKEY);
	itemsz = ItemIdGetLength(itemid);
	item = (BTItem) PageGetItem(lpage, itemid);
	new_item = _bt_formitem(&(item->bti_itup));
	ItemPointerSet(&(new_item->bti_itup.t_tid), lbkno, P_HIKEY);

	/*
	 * insert the left page pointer into the new root page.  the root page
	 * is the rightmost page on its level so the "high key" item is the
	 * first data item.
	 */
	if (PageAddItem(rootpage, (Item) new_item, itemsz, P_HIKEY, LP_USED) == InvalidOffsetNumber)
		elog(FATAL, "btree: failed to add leftkey to new root page");
	pfree(new_item);

	/*
	 * the right page is the rightmost page on the second level, so the
	 * "high key" item is the first data item on that page as well.
	 */
	itemid = PageGetItemId(rpage, P_HIKEY);
	itemsz = ItemIdGetLength(itemid);
	item = (BTItem) PageGetItem(rpage, itemid);
	new_item = _bt_formitem(&(item->bti_itup));
	ItemPointerSet(&(new_item->bti_itup.t_tid), rbkno, P_HIKEY);

	/*
	 * insert the right page pointer into the new root page.
	 */
	if (PageAddItem(rootpage, (Item) new_item, itemsz, P_FIRSTKEY, LP_USED) == InvalidOffsetNumber)
		elog(FATAL, "btree: failed to add rightkey to new root page");
	pfree(new_item);

	/* write and let go of the root buffer */
	rootbknum = BufferGetBlockNumber(rootbuf);
	_bt_wrtbuf(rel, rootbuf);

	/* update metadata page with new root block number */
	_bt_metaproot(rel, rootbknum, 0);
}

/*
 *	_bt_pgaddtup() -- add a tuple to a particular page in the index.
 *
 *		This routine adds the tuple to the page as requested, and keeps the
 *		write lock and reference associated with the page's buffer.  It is
 *		an error to call pgaddtup() without a write lock and reference.  If
 *		afteritem is non-null, it's the item that we expect our new item
 *		to follow.	Otherwise, we do a binary search for the correct place
 *		and insert the new item there.
 */
static OffsetNumber
_bt_pgaddtup(Relation rel,
			 Buffer buf,
			 int keysz,
			 ScanKey itup_scankey,
			 Size itemsize,
			 BTItem btitem,
			 BTItem afteritem)
{
	OffsetNumber itup_off;
	OffsetNumber first;
	Page		page;
	BTPageOpaque opaque;
	BTItem		chkitem;

	page = BufferGetPage(buf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	first = P_RIGHTMOST(opaque) ? P_HIKEY : P_FIRSTKEY;

	if (afteritem == (BTItem) NULL)
	{
		itup_off = _bt_binsrch(rel, buf, keysz, itup_scankey, BT_INSERTION);
	}
	else
	{
		itup_off = first;

		do
		{
			chkitem =
				(BTItem) PageGetItem(page, PageGetItemId(page, itup_off));
			itup_off = OffsetNumberNext(itup_off);
		} while (!BTItemSame(chkitem, afteritem));
	}

	if (PageAddItem(page, (Item) btitem, itemsize, itup_off, LP_USED) == InvalidOffsetNumber)
		elog(FATAL, "btree: failed to add item to the page");

	/* write the buffer, but hold our lock */
	_bt_wrtnorelbuf(rel, buf);

	return (itup_off);
}

/*
 *	_bt_goesonpg() -- Does a new tuple belong on this page?
 *
 *		This is part of the complexity introduced by allowing duplicate
 *		keys into the index.  The tuple belongs on this page if:
 *
 *				+ there is no page to the right of this one; or
 *				+ it is less than the high key on the page; or
 *				+ the item it is to follow ("afteritem") appears on this
 *				  page.
 */
static bool
_bt_goesonpg(Relation rel,
			 Buffer buf,
			 Size keysz,
			 ScanKey scankey,
			 BTItem afteritem)
{
	Page		page;
	ItemId		hikey;
	BTPageOpaque opaque;
	BTItem		chkitem;
	OffsetNumber offnum,
				maxoff;
	bool		found;

	page = BufferGetPage(buf);

	/* no right neighbor? */
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	if (P_RIGHTMOST(opaque))
		return (true);

	/*
	 * this is a non-rightmost page, so it must have a high key item.
	 *
	 * If the scan key is < the high key (the min key on the next page), then
	 * it for sure belongs here.
	 */
	hikey = PageGetItemId(page, P_HIKEY);
	if (_bt_skeycmp(rel, keysz, scankey, page, hikey, BTLessStrategyNumber))
		return (true);

	/*
	 * If the scan key is > the high key, then it for sure doesn't belong
	 * here.
	 */

	if (_bt_skeycmp(rel, keysz, scankey, page, hikey, BTGreaterStrategyNumber))
		return (false);

	/*
	 * If we have no adjacency information, and the item is equal to the
	 * high key on the page (by here it is), then the item does not belong
	 * on this page.
	 *
	 * Now it's not true in all cases.		- vadim 06/10/97
	 */

	if (afteritem == (BTItem) NULL)
	{
		if (opaque->btpo_flags & BTP_LEAF)
			return (false);
		if (opaque->btpo_flags & BTP_CHAIN)
			return (true);
		if (_bt_skeycmp(rel, keysz, scankey, page,
						PageGetItemId(page, P_FIRSTKEY),
						BTEqualStrategyNumber))
			return (true);
		return (false);
	}

	/* damn, have to work for it.  i hate that. */
	maxoff = PageGetMaxOffsetNumber(page);

	/*
	 * Search the entire page for the afteroid.  We need to do this,
	 * rather than doing a binary search and starting from there, because
	 * if the key we're searching for is the leftmost key in the tree at
	 * this level, then a binary search will do the wrong thing.  Splits
	 * are pretty infrequent, so the cost isn't as bad as it could be.
	 */

	found = false;
	for (offnum = P_FIRSTKEY;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		chkitem = (BTItem) PageGetItem(page, PageGetItemId(page, offnum));

		if (BTItemSame(chkitem, afteritem))
		{
			found = true;
			break;
		}
	}

	return (found);
}

/*
 *		_bt_itemcmp() -- compare item1 to item2 using a requested
 *						 strategy (<, <=, =, >=, >)
 *
 */
bool
_bt_itemcmp(Relation rel,
			Size keysz,
			BTItem item1,
			BTItem item2,
			StrategyNumber strat)
{
	TupleDesc	tupDes;
	IndexTuple	indexTuple1,
				indexTuple2;
	Datum		attrDatum1,
				attrDatum2;
	int			i;
	bool		isFirstNull,
				isSecondNull;
	bool		compare;
	bool		useEqual = false;

	if (strat == BTLessEqualStrategyNumber)
	{
		useEqual = true;
		strat = BTLessStrategyNumber;
	}
	else if (strat == BTGreaterEqualStrategyNumber)
	{
		useEqual = true;
		strat = BTGreaterStrategyNumber;
	}

	tupDes = RelationGetTupleDescriptor(rel);
	indexTuple1 = &(item1->bti_itup);
	indexTuple2 = &(item2->bti_itup);

	for (i = 1; i <= keysz; i++)
	{
		attrDatum1 = index_getattr(indexTuple1, i, tupDes, &isFirstNull);
		attrDatum2 = index_getattr(indexTuple2, i, tupDes, &isSecondNull);

		/* see comments about NULLs handling in btbuild */
		if (isFirstNull)		/* attr in item1 is NULL */
		{
			if (isSecondNull)	/* attr in item2 is NULL too */
				compare = (strat == BTEqualStrategyNumber) ? true : false;
			else
				compare = (strat == BTGreaterStrategyNumber) ? true : false;
		}
		else if (isSecondNull)	/* attr in item1 is NOT_NULL and */
		{						/* and attr in item2 is NULL */
			compare = (strat == BTLessStrategyNumber) ? true : false;
		}
		else
		{
			compare = _bt_invokestrat(rel, i, strat, attrDatum1, attrDatum2);
		}

		if (compare)			/* true for one of ">, <, =" */
		{
			if (strat != BTEqualStrategyNumber)
				return (true);
		}
		else
/* false for one of ">, <, =" */
		{
			if (strat == BTEqualStrategyNumber)
				return (false);

			/*
			 * if original strat was "<=, >=" OR "<, >" but some
			 * attribute(s) left - need to test for Equality
			 */
			if (useEqual || i < keysz)
			{
				if (isFirstNull || isSecondNull)
					compare = (isFirstNull && isSecondNull) ? true : false;
				else
					compare = _bt_invokestrat(rel, i, BTEqualStrategyNumber,
											  attrDatum1, attrDatum2);
				if (compare)	/* item1' and item2' attributes are equal */
					continue;	/* - try to compare next attributes */
			}
			return (false);
		}
	}
	return (true);
}

/*
 *		_bt_updateitem() -- updates the key of the item identified by the
 *							oid with the key of newItem (done in place if
 *							possible)
 *
 */
static void
_bt_updateitem(Relation rel,
			   Size keysz,
			   Buffer buf,
			   BTItem oldItem,
			   BTItem newItem)
{
	Page		page;
	OffsetNumber maxoff;
	OffsetNumber i;
	ItemPointerData itemPtrData;
	BTItem		item;
	IndexTuple	oldIndexTuple,
				newIndexTuple;
	int			first;

	page = BufferGetPage(buf);
	maxoff = PageGetMaxOffsetNumber(page);

	/* locate item on the page */
	first = P_RIGHTMOST((BTPageOpaque) PageGetSpecialPointer(page))
		? P_HIKEY : P_FIRSTKEY;
	i = first;
	do
	{
		item = (BTItem) PageGetItem(page, PageGetItemId(page, i));
		i = OffsetNumberNext(i);
	} while (i <= maxoff && !BTItemSame(item, oldItem));

	/* this should never happen (in theory) */
	if (!BTItemSame(item, oldItem))
	{
		elog(FATAL, "_bt_getstackbuf was lying!!");
	}

	/*
	 * It's  defined by caller (_bt_insertonpg)
	 */

	/*
	 * if(IndexTupleDSize(newItem->bti_itup) >
	 * IndexTupleDSize(item->bti_itup)) { elog(NOTICE, "trying to
	 * overwrite a smaller value with a bigger one in _bt_updateitem");
	 * elog(ABORT, "this is not good."); }
	 */

	oldIndexTuple = &(item->bti_itup);
	newIndexTuple = &(newItem->bti_itup);

	/* keep the original item pointer */
	ItemPointerCopy(&(oldIndexTuple->t_tid), &itemPtrData);
	CopyIndexTuple(newIndexTuple, &oldIndexTuple);
	ItemPointerCopy(&itemPtrData, &(oldIndexTuple->t_tid));

}

/*
 * _bt_isequal - used in _bt_doinsert in check for duplicates.
 *
 * Rule is simple: NOT_NULL not equal NULL, NULL not_equal NULL too.
 */
static bool
_bt_isequal(TupleDesc itupdesc, Page page, OffsetNumber offnum,
			int keysz, ScanKey scankey)
{
	Datum		datum;
	BTItem		btitem;
	IndexTuple	itup;
	ScanKey		entry;
	AttrNumber	attno;
	long		result;
	int			i;
	bool		null;

	btitem = (BTItem) PageGetItem(page, PageGetItemId(page, offnum));
	itup = &(btitem->bti_itup);

	for (i = 1; i <= keysz; i++)
	{
		entry = &scankey[i - 1];
		attno = entry->sk_attno;
		Assert(attno == i);
		datum = index_getattr(itup, attno, itupdesc, &null);

		/* NULLs are not equal */
		if (entry->sk_flags & SK_ISNULL || null)
			return (false);

		result = (long) FMGR_PTR2(entry->sk_func, entry->sk_procedure,
								  entry->sk_argument, datum);
		if (result != 0)
			return (false);
	}

	/* by here, the keys are equal */
	return (true);
}

#ifdef NOT_USED
/*
 * _bt_shift - insert btitem on the passed page after shifting page
 *			   to the right in the tree.
 *
 * NOTE: tested for shifting leftmost page only, having btitem < hikey.
 */
static InsertIndexResult
_bt_shift(Relation rel, Buffer buf, BTStack stack, int keysz,
		  ScanKey scankey, BTItem btitem, BTItem hikey)
{
	InsertIndexResult res;
	int			itemsz;
	Page		page;
	BlockNumber bknum;
	BTPageOpaque pageop;
	Buffer		rbuf;
	Page		rpage;
	BTPageOpaque rpageop;
	Buffer		pbuf;
	Page		ppage;
	BTPageOpaque ppageop;
	Buffer		nbuf;
	Page		npage;
	BTPageOpaque npageop;
	BlockNumber nbknum;
	BTItem		nitem;
	OffsetNumber afteroff;

	btitem = _bt_formitem(&(btitem->bti_itup));
	hikey = _bt_formitem(&(hikey->bti_itup));

	page = BufferGetPage(buf);

	/* grab new page */
	nbuf = _bt_getbuf(rel, P_NEW, BT_WRITE);
	nbknum = BufferGetBlockNumber(nbuf);
	npage = BufferGetPage(nbuf);
	_bt_pageinit(npage, BufferGetPageSize(nbuf));
	npageop = (BTPageOpaque) PageGetSpecialPointer(npage);

	/* copy content of the passed page */
	memmove((char *) npage, (char *) page, BufferGetPageSize(buf));

	/* re-init old (passed) page */
	_bt_pageinit(page, BufferGetPageSize(buf));
	pageop = (BTPageOpaque) PageGetSpecialPointer(page);

	/* init old page opaque */
	pageop->btpo_flags = npageop->btpo_flags;	/* restore flags */
	pageop->btpo_flags &= ~BTP_CHAIN;
	if (_bt_itemcmp(rel, keysz, hikey, btitem, BTEqualStrategyNumber))
		pageop->btpo_flags |= BTP_CHAIN;
	pageop->btpo_prev = npageop->btpo_prev;		/* restore prev */
	pageop->btpo_next = nbknum; /* next points to the new page */

	/* init shifted page opaque */
	npageop->btpo_prev = bknum = BufferGetBlockNumber(buf);

	/* shifted page is ok, populate old page */

	/* add passed hikey */
	itemsz = IndexTupleDSize(hikey->bti_itup)
		+ (sizeof(BTItemData) - sizeof(IndexTupleData));
	itemsz = DOUBLEALIGN(itemsz);
	if (PageAddItem(page, (Item) hikey, itemsz, P_HIKEY, LP_USED) == InvalidOffsetNumber)
		elog(FATAL, "btree: failed to add hikey in _bt_shift");
	pfree(hikey);

	/* add btitem */
	itemsz = IndexTupleDSize(btitem->bti_itup)
		+ (sizeof(BTItemData) - sizeof(IndexTupleData));
	itemsz = DOUBLEALIGN(itemsz);
	if (PageAddItem(page, (Item) btitem, itemsz, P_FIRSTKEY, LP_USED) == InvalidOffsetNumber)
		elog(FATAL, "btree: failed to add firstkey in _bt_shift");
	pfree(btitem);
	nitem = (BTItem) PageGetItem(page, PageGetItemId(page, P_FIRSTKEY));
	btitem = _bt_formitem(&(nitem->bti_itup));
	ItemPointerSet(&(btitem->bti_itup.t_tid), bknum, P_HIKEY);

	/* ok, write them out */
	_bt_wrtnorelbuf(rel, nbuf);
	_bt_wrtnorelbuf(rel, buf);

	/* fix btpo_prev on right sibling of old page */
	if (!P_RIGHTMOST(npageop))
	{
		rbuf = _bt_getbuf(rel, npageop->btpo_next, BT_WRITE);
		rpage = BufferGetPage(rbuf);
		rpageop = (BTPageOpaque) PageGetSpecialPointer(rpage);
		rpageop->btpo_prev = nbknum;
		_bt_wrtbuf(rel, rbuf);
	}

	/* get parent pointing to the old page */
	ItemPointerSet(&(stack->bts_btitem->bti_itup.t_tid),
				   bknum, P_HIKEY);
	pbuf = _bt_getstackbuf(rel, stack, BT_WRITE);
	ppage = BufferGetPage(pbuf);
	ppageop = (BTPageOpaque) PageGetSpecialPointer(ppage);

	_bt_relbuf(rel, nbuf, BT_WRITE);
	_bt_relbuf(rel, buf, BT_WRITE);

	/* re-set parent' pointer - we shifted our page to the right ! */
	nitem = (BTItem) PageGetItem(ppage,
								 PageGetItemId(ppage, stack->bts_offset));
	ItemPointerSet(&(nitem->bti_itup.t_tid), nbknum, P_HIKEY);
	ItemPointerSet(&(stack->bts_btitem->bti_itup.t_tid), nbknum, P_HIKEY);
	_bt_wrtnorelbuf(rel, pbuf);

	/*
	 * Now we want insert into the parent pointer to our old page. It has
	 * to be inserted before the pointer to new page. You may get problems
	 * here (in the _bt_goesonpg and/or _bt_pgaddtup), but may be not - I
	 * don't know. It works if old page is leftmost (nitem is NULL) and
	 * btitem < hikey and it's all what we need currently. - vadim
	 * 05/30/97
	 */
	nitem = NULL;
	afteroff = P_FIRSTKEY;
	if (!P_RIGHTMOST(ppageop))
		afteroff = OffsetNumberNext(afteroff);
	if (stack->bts_offset >= afteroff)
	{
		afteroff = OffsetNumberPrev(stack->bts_offset);
		nitem = (BTItem) PageGetItem(ppage, PageGetItemId(ppage, afteroff));
		nitem = _bt_formitem(&(nitem->bti_itup));
	}
	res = _bt_insertonpg(rel, pbuf, stack->bts_parent,
						 keysz, scankey, btitem, nitem);
	pfree(btitem);

	ItemPointerSet(&(res->pointerData), nbknum, P_HIKEY);

	return (res);
}

#endif
