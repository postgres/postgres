/*-------------------------------------------------------------------------
 *
 * btinsert.c--
 *    Item insertion in Lehman and Yao btrees for Postgres.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/access/nbtree/nbtinsert.c,v 1.7 1996/11/13 20:47:11 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <utils/memutils.h>
#include <storage/bufpage.h>
#include <access/nbtree.h>
#include <storage/bufmgr.h>

#ifndef HAVE_MEMMOVE
# include <regex/utils.h>
#else
# include <string.h>
#endif

static InsertIndexResult _bt_insertonpg(Relation rel, Buffer buf, BTStack stack, int keysz, ScanKey scankey, BTItem btitem, BTItem afteritem);
static Buffer _bt_split(Relation rel, Buffer buf);
static OffsetNumber _bt_findsplitloc(Relation rel, Page page, OffsetNumber start, OffsetNumber maxoff, Size llimit);
static void _bt_newroot(Relation rel, Buffer lbuf, Buffer rbuf);
static OffsetNumber _bt_pgaddtup(Relation rel, Buffer buf, int keysz, ScanKey itup_scankey, Size itemsize, BTItem btitem, BTItem afteritem);
static bool _bt_goesonpg(Relation rel, Buffer buf, Size keysz, ScanKey scankey, BTItem afteritem);

#if 0
static void _bt_updateitem(Relation rel, Size keysz, Buffer buf, Oid bti_oid, BTItem newItem);
#endif

/*
 *  _bt_doinsert() -- Handle insertion of a single btitem in the tree.
 *
 *	This routine is called by the public interface routines, btbuild
 *	and btinsert.  By here, btitem is filled in, and has a unique
 *	(xid, seqno) pair.
 */
InsertIndexResult
_bt_doinsert(Relation rel, BTItem btitem, bool index_is_unique, bool is_update)
{
    ScanKey itup_scankey;
    IndexTuple itup;
    BTStack stack;
    Buffer buf;
    BlockNumber blkno;
    int natts;
    InsertIndexResult res;
    
    itup = &(btitem->bti_itup);
    
    /* we need a scan key to do our search, so build one */
    itup_scankey = _bt_mkscankey(rel, itup);
    natts = rel->rd_rel->relnatts;
    
    /* find the page containing this key */
    stack = _bt_search(rel, natts, itup_scankey, &buf);

    /* if we're not allowing duplicates, make sure the key isn't */
    /* already in the node */
    if(index_is_unique && !is_update) {
	OffsetNumber offset;
	TupleDesc itupdesc;
	Page page;

	itupdesc = RelationGetTupleDescriptor(rel);
	page = BufferGetPage(buf);

	offset = _bt_binsrch(rel, buf, natts, itup_scankey, BT_DESCENT);

	/* make sure the offset we're given points to an actual */
	/* key on the page before trying to compare it */
	if(!PageIsEmpty(page) &&
	   offset <= PageGetMaxOffsetNumber(page)) {
	    if(!_bt_compare(rel, itupdesc, page, 
			    natts, itup_scankey, offset)) {
		/* it is a duplicate */
		elog(WARN, "Cannot insert a duplicate key into a unique index.");
	    }
	}
    }

    blkno = BufferGetBlockNumber(buf);
    
    /* trade in our read lock for a write lock */
    _bt_relbuf(rel, buf, BT_READ);
    buf = _bt_getbuf(rel, blkno, BT_WRITE);
    
    /*
     *  If the page was split between the time that we surrendered our
     *  read lock and acquired our write lock, then this page may no
     *  longer be the right place for the key we want to insert.  In this
     *  case, we need to move right in the tree.  See Lehman and Yao for
     *  an excruciatingly precise description.
     */
    
    buf = _bt_moveright(rel, buf, natts, itup_scankey, BT_WRITE);
    
    /* do the insertion */
    res = _bt_insertonpg(rel, buf, stack, natts, itup_scankey,
			 btitem, (BTItem) NULL);
    
    /* be tidy */
    _bt_freestack(stack);
    _bt_freeskey(itup_scankey);
    
    return (res);
}

/*
 *  _bt_insertonpg() -- Insert a tuple on a particular page in the index.
 *
 *	This recursive procedure does the following things:
 *
 *	    +  if necessary, splits the target page.
 *	    +  finds the right place to insert the tuple (taking into
 *	       account any changes induced by a split).
 *	    +  inserts the tuple.
 *	    +  if the page was split, pops the parent stack, and finds the
 *	       right place to insert the new child pointer (by walking
 *	       right using information stored in the parent stack).
 *	    +  invoking itself with the appropriate tuple for the right
 *	       child page on the parent.
 *
 *	On entry, we must have the right buffer on which to do the
 *	insertion, and the buffer must be pinned and locked.  On return,
 *	we will have dropped both the pin and the write lock on the buffer.
 *
 *	The locking interactions in this code are critical.  You should
 *	grok Lehman and Yao's paper before making any changes.  In addition,
 *	you need to understand how we disambiguate duplicate keys in this
 *	implementation, in order to be able to find our location using
 *	L&Y "move right" operations.  Since we may insert duplicate user
 *	keys, and since these dups may propogate up the tree, we use the
 *	'afteritem' parameter to position ourselves correctly for the
 *	insertion on internal pages.
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
    Page page;
    Buffer rbuf;
    Buffer pbuf;
    Page rpage;
    ScanKey newskey;
    BTItem ritem;
    BTPageOpaque rpageop;
    BlockNumber rbknum, itup_blkno;
    OffsetNumber itup_off;
    int itemsz;
    InsertIndexResult newres;
    BTItem new_item = (BTItem) NULL;
    BTItem lowLeftItem;
    OffsetNumber leftmost_offset;
    Page ppage;
    BTPageOpaque ppageop;
    BlockNumber bknum;
    
    page = BufferGetPage(buf);
    itemsz = IndexTupleDSize(btitem->bti_itup)
	+ (sizeof(BTItemData) - sizeof(IndexTupleData));

    itemsz = DOUBLEALIGN(itemsz);	/* be safe, PageAddItem will do this
					   but we need to be consistent */
    
    if (PageGetFreeSpace(page) < itemsz) {
	
	/* split the buffer into left and right halves */
	rbuf = _bt_split(rel, buf);
	
	/* which new page (left half or right half) gets the tuple? */
	if (_bt_goesonpg(rel, buf, keysz, scankey, afteritem)) {
	    /* left page */
	    itup_off = _bt_pgaddtup(rel, buf, keysz, scankey,
				    itemsz, btitem, afteritem);
	    itup_blkno = BufferGetBlockNumber(buf);
	} else {
	    /* right page */
	    itup_off = _bt_pgaddtup(rel, rbuf, keysz, scankey,
				    itemsz, btitem, afteritem);
	    itup_blkno = BufferGetBlockNumber(rbuf);
	}
	
	/*
	 *  By here,
	 *
	 *	+  our target page has been split;
	 *	+  the original tuple has been inserted;
	 *	+  we have write locks on both the old (left half) and new
	 *	   (right half) buffers, after the split; and
	 *	+  we have the key we want to insert into the parent.
	 *
	 *  Do the parent insertion.  We need to hold onto the locks for
	 *  the child pages until we locate the parent, but we can release
	 *  them before doing the actual insertion (see Lehman and Yao for
	 *  the reasoning).
	 */
	
	if (stack == (BTStack) NULL) {
	    
	    /* create a new root node and release the split buffers */
	    _bt_newroot(rel, buf, rbuf);
	    _bt_relbuf(rel, buf, BT_WRITE);
	    _bt_relbuf(rel, rbuf, BT_WRITE);
	    
	} else {

	    /* form a index tuple that points at the new right page */
	    rbknum = BufferGetBlockNumber(rbuf);
	    rpage = BufferGetPage(rbuf);
	    rpageop = (BTPageOpaque) PageGetSpecialPointer(rpage);
	    
	    /*
	     *  By convention, the first entry (0) on every
	     *  non-rightmost page is the high key for that page.  In
	     *  order to get the lowest key on the new right page, we
	     *  actually look at its second (1) entry.
	     */
	    
	    if (! P_RIGHTMOST(rpageop)) {
		ritem = (BTItem) PageGetItem(rpage,
					     PageGetItemId(rpage, P_FIRSTKEY));
	    } else {
		ritem = (BTItem) PageGetItem(rpage,
					     PageGetItemId(rpage, P_HIKEY));
	    }
	    
	    /* get a unique btitem for this key */
	    new_item = _bt_formitem(&(ritem->bti_itup));
	    
	    ItemPointerSet(&(new_item->bti_itup.t_tid), rbknum, P_HIKEY);
	    
	    /* find the parent buffer */
	    pbuf = _bt_getstackbuf(rel, stack, BT_WRITE);
	    
	    /*
	     *  If the key of new_item is < than the key of the item
	     *  in the parent page pointing to the left page
	     *  (stack->bts_btitem), we have to update the latter key;
	     *  otherwise the keys on the parent page wouldn't be
	     *  monotonically increasing after we inserted the new
	     *  pointer to the right page (new_item). This only
	     *  happens if our left page is the leftmost page and a
	     *  new minimum key had been inserted before, which is not
	     *  reflected in the parent page but didn't matter so
	     *  far. If there are duplicate keys and this new minimum
	     *  key spills over to our new right page, we get an
	     *  inconsistency if we don't update the left key in the
	     *  parent page.
	     */
	    
	    if (_bt_itemcmp(rel, keysz, stack->bts_btitem, new_item,
	                    BTGreaterStrategyNumber)) {
		lowLeftItem =
		    (BTItem) PageGetItem(page,
					 PageGetItemId(page, P_FIRSTKEY));
		
		/* this method does not work--_bt_updateitem tries to     */
		/* overwrite an entry with another entry that might be    */
		/* bigger.  if lowLeftItem is bigger, it corrupts the     */
		/* parent page.  instead, we have to delete the original  */
		/* leftmost item from the parent, and insert the new one  */
		/* with a regular _bt_insertonpg (it could cause a split  */
		/* because it's bigger than what was there before).       */
                /*                                  --djm 8/21/96         */

		/* _bt_updateitem(rel, keysz, pbuf, stack->bts_btitem->bti_oid,
		               lowLeftItem); */
		
		/* get the parent page */
		ppage = BufferGetPage(pbuf);
		ppageop = (BTPageOpaque) PageGetSpecialPointer(ppage);

		/* figure out which key is leftmost (if the parent page   */
		/* is rightmost, too, it must be the root)                */
		if(P_RIGHTMOST(ppageop)) {
		    leftmost_offset = P_HIKEY;
		} else {
		    leftmost_offset = P_FIRSTKEY;
	    }
       		PageIndexTupleDelete(ppage, leftmost_offset);
		
		/* don't write anything out yet--we still have the write  */
		/* lock, and now we call another _bt_insertonpg to        */
		/* insert the correct leftmost key                        */

		/* make a new leftmost item, using the tuple data from    */
		/* lowLeftItem.  point it to the left child.              */
		/* update it on the stack at the same time.               */
		bknum = BufferGetBlockNumber(buf);
		pfree(stack->bts_btitem);
		stack->bts_btitem = _bt_formitem(&(lowLeftItem->bti_itup));
		ItemPointerSet(&(stack->bts_btitem->bti_itup.t_tid), 
			       bknum, P_HIKEY);
		
		/* unlock the children before doing this */
		_bt_relbuf(rel, buf, BT_WRITE);
		_bt_relbuf(rel, rbuf, BT_WRITE);
		
		/* a regular _bt_binsrch should find the right place to   */
		/* put the new entry, since it should be lower than any   */
		/* other key on the page, therefore set afteritem to NULL */
		newskey = _bt_mkscankey(rel, &(stack->bts_btitem->bti_itup));
		newres = _bt_insertonpg(rel, pbuf, stack->bts_parent,
					keysz, newskey, stack->bts_btitem,
					NULL);

		pfree(newres);
		pfree(newskey);
	    
		/* we have now lost our lock on the parent buffer, and    */
		/* need to get it back.                                   */
		pbuf = _bt_getstackbuf(rel, stack, BT_WRITE);
	    } else {
	    _bt_relbuf(rel, buf, BT_WRITE);
	    _bt_relbuf(rel, rbuf, BT_WRITE);
	    }
	    
	    newskey = _bt_mkscankey(rel, &(new_item->bti_itup));
	    newres = _bt_insertonpg(rel, pbuf, stack->bts_parent,
				    keysz, newskey, new_item,
				    stack->bts_btitem);
	    
	    /* be tidy */
	    pfree(newres);
	    pfree(newskey);
	    pfree(new_item);
	}
    } else {
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
 *  _bt_split() -- split a page in the btree.
 *
 *	On entry, buf is the page to split, and is write-locked and pinned.
 *	Returns the new right sibling of buf, pinned and write-locked.  The
 *	pin and lock on buf are maintained.
 */
static Buffer
_bt_split(Relation rel, Buffer buf)
{
    Buffer rbuf;
    Page origpage;
    Page leftpage, rightpage;
    BTPageOpaque ropaque, lopaque, oopaque;
    Buffer sbuf;
    Page spage;
    BTPageOpaque sopaque;
    Size itemsz;
    ItemId itemid;
    BTItem item;
    OffsetNumber leftoff, rightoff;
    OffsetNumber start;
    OffsetNumber maxoff;
    OffsetNumber firstright;
    OffsetNumber i;
    Size llimit;
    
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
    lopaque->btpo_flags = ropaque->btpo_flags = oopaque->btpo_flags;
    lopaque->btpo_prev = oopaque->btpo_prev;
    ropaque->btpo_prev = BufferGetBlockNumber(buf);
    lopaque->btpo_next = BufferGetBlockNumber(rbuf);
    ropaque->btpo_next = oopaque->btpo_next;
    
    /*
     *  If the page we're splitting is not the rightmost page at its
     *  level in the tree, then the first (0) entry on the page is the
     *  high key for the page.  We need to copy that to the right
     *  half.  Otherwise (meaning the rightmost page case), we should
     *  treat the line pointers beginning at zero as user data.
     *
     *  We leave a blank space at the start of the line table for the
     *  left page.  We'll come back later and fill it in with the high
     *  key item we get from the right key.
     */
    
    leftoff = P_FIRSTKEY;
    ropaque->btpo_next = oopaque->btpo_next;
    if (! P_RIGHTMOST(oopaque)) {
	/* splitting a non-rightmost page, start at the first data item */
	start = P_FIRSTKEY;

	/* copy the original high key to the new page */
	itemid = PageGetItemId(origpage, P_HIKEY);
	itemsz = ItemIdGetLength(itemid);
	item = (BTItem) PageGetItem(origpage, itemid);
	(void) PageAddItem(rightpage, (Item) item, itemsz, P_HIKEY, LP_USED);
	rightoff = P_FIRSTKEY;
    } else {
	/* splitting a rightmost page, "high key" is the first data item */
	start = P_HIKEY;

	/* the new rightmost page will not have a high key */
	rightoff = P_HIKEY;
    }
    maxoff = PageGetMaxOffsetNumber(origpage);
    llimit = PageGetFreeSpace(leftpage) / 2;
    firstright = _bt_findsplitloc(rel, origpage, start, maxoff, llimit);
    
    for (i = start; i <= maxoff; i = OffsetNumberNext(i)) {
	itemid = PageGetItemId(origpage, i);
	itemsz = ItemIdGetLength(itemid);
	item = (BTItem) PageGetItem(origpage, itemid);
	
	/* decide which page to put it on */
	if (i < firstright) {
	    (void) PageAddItem(leftpage, (Item) item, itemsz, leftoff,
			       LP_USED);
	    leftoff = OffsetNumberNext(leftoff);
	} else {
	    (void) PageAddItem(rightpage, (Item) item, itemsz, rightoff,
			       LP_USED);
	    rightoff = OffsetNumberNext(rightoff);
	}
    }
    
    /*
     *  Okay, page has been split, high key on right page is correct.  Now
     *  set the high key on the left page to be the min key on the right
     *  page.
     */
    
    if (P_RIGHTMOST(ropaque)) {
	itemid = PageGetItemId(rightpage, P_HIKEY);
    } else {
	itemid = PageGetItemId(rightpage, P_FIRSTKEY);
    }
    itemsz = ItemIdGetLength(itemid);
    item = (BTItem) PageGetItem(rightpage, itemid);
    
    /*
     *  We left a hole for the high key on the left page; fill it.  The
     *  modal crap is to tell the page manager to put the new item on the
     *  page and not screw around with anything else.  Whoever designed
     *  this interface has presumably crawled back into the dung heap they
     *  came from.  No one here will admit to it.
     */
    
    PageManagerModeSet(OverwritePageManagerMode);
    (void) PageAddItem(leftpage, (Item) item, itemsz, P_HIKEY, LP_USED);
    PageManagerModeSet(ShufflePageManagerMode);
    
    /*
     *  By here, the original data page has been split into two new halves,
     *  and these are correct.  The algorithm requires that the left page
     *  never move during a split, so we copy the new left page back on top
     *  of the original.  Note that this is not a waste of time, since we
     *  also require (in the page management code) that the center of a
     *  page always be clean, and the most efficient way to guarantee this
     *  is just to compact the data by reinserting it into a new left page.
     */
    
    PageRestoreTempPage(leftpage, origpage);
    
    /* write these guys out */
    _bt_wrtnorelbuf(rel, rbuf);
    _bt_wrtnorelbuf(rel, buf);
    
    /*
     *  Finally, we need to grab the right sibling (if any) and fix the
     *  prev pointer there.  We are guaranteed that this is deadlock-free
     *  since no other writer will be moving holding a lock on that page
     *  and trying to move left, and all readers release locks on a page
     *  before trying to fetch its neighbors.
     */
    
    if (! P_RIGHTMOST(ropaque)) {
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
 *  _bt_findsplitloc() -- find a safe place to split a page.
 *
 *	In order to guarantee the proper handling of searches for duplicate
 *	keys, the first duplicate in the chain must either be the first
 *	item on the page after the split, or the entire chain must be on
 *	one of the two pages.  That is,
 *		[1 2 2 2 3 4 5]
 *	must become
 *		[1] [2 2 2 3 4 5]
 *	or
 *		[1 2 2 2] [3 4 5]
 *	but not
 *		[1 2 2] [2 3 4 5].
 *	However,
 *		[2 2 2 2 2 3 4]
 *	may be split as
 *		[2 2 2 2] [2 3 4].
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
    ItemId nxtitemid, safeitemid;
    BTItem safeitem, nxtitem;
    IndexTuple safetup, nxttup;
    Size nbytes;
    TupleDesc itupdesc;
    int natts;
    int attno;
    Datum attsafe;
    Datum attnext;
    bool null;
    
    itupdesc = RelationGetTupleDescriptor(rel);
    natts = rel->rd_rel->relnatts;
    
    saferight = start;
    safeitemid = PageGetItemId(page, saferight);
    nbytes = ItemIdGetLength(safeitemid) + sizeof(ItemIdData);
    safeitem = (BTItem) PageGetItem(page, safeitemid);
    safetup = &(safeitem->bti_itup);
    
    i = OffsetNumberNext(start);
    
    while (nbytes < llimit) {
	
	/* check the next item on the page */
	nxtitemid = PageGetItemId(page, i);
	nbytes += (ItemIdGetLength(nxtitemid) + sizeof(ItemIdData));
	nxtitem = (BTItem) PageGetItem(page, nxtitemid);
	nxttup = &(nxtitem->bti_itup);
	
	/* test against last known safe item */
	for (attno = 1; attno <= natts; attno++) {
	    attsafe = index_getattr(safetup, attno, itupdesc, &null);
	    attnext = index_getattr(nxttup, attno, itupdesc, &null);

	    /*
	     *  If the tuple we're looking at isn't equal to the last safe one
	     *  we saw, then it's our new safe tuple.
	     */
	    
	    if (!_bt_invokestrat(rel, attno, BTEqualStrategyNumber,
				 attsafe, attnext)) {
		safetup = nxttup;
		saferight = i;
		
		/* break is for the attno for loop */
		break;
	    }
	}
	i = OffsetNumberNext(i);
    }
    
    /*
     *  If the chain of dups starts at the beginning of the page and extends
     *  past the halfway mark, we can split it in the middle.
     */
    
    if (saferight == start)
	saferight = i;
    
    return (saferight);
}

/*
 *  _bt_newroot() -- Create a new root page for the index.
 *
 *	We've just split the old root page and need to create a new one.
 *	In order to do this, we add a new root page to the file, then lock
 *	the metadata page and update it.  This is guaranteed to be deadlock-
 *	free, because all readers release their locks on the metadata page
 *	before trying to lock the root, and all writers lock the root before
 *	trying to lock the metadata page.  We have a write lock on the old
 *	root page, so we have not introduced any cycles into the waits-for
 *	graph.
 *
 *	On entry, lbuf (the old root) and rbuf (its new peer) are write-
 *	locked.  We don't drop the locks in this routine; that's done by
 *	the caller.  On exit, a new root page exists with entries for the
 *	two new children.  The new root page is neither pinned nor locked.
 */
static void
_bt_newroot(Relation rel, Buffer lbuf, Buffer rbuf)
{
    Buffer rootbuf;
    Page lpage, rpage, rootpage;
    BlockNumber lbkno, rbkno;
    BlockNumber rootbknum;
    BTPageOpaque rootopaque;
    ItemId itemid;
    BTItem item;
    Size itemsz;
    BTItem new_item;
    
    /* get a new root page */
    rootbuf = _bt_getbuf(rel, P_NEW, BT_WRITE);
    rootpage = BufferGetPage(rootbuf);
    _bt_pageinit(rootpage, BufferGetPageSize(rootbuf));
    
    /* set btree special data */
    rootopaque = (BTPageOpaque) PageGetSpecialPointer(rootpage);
    rootopaque->btpo_prev = rootopaque->btpo_next = P_NONE;
    rootopaque->btpo_flags |= BTP_ROOT;
    
    /*
     *  Insert the internal tuple pointers.
     */
    
    lbkno = BufferGetBlockNumber(lbuf);
    rbkno = BufferGetBlockNumber(rbuf);
    lpage = BufferGetPage(lbuf);
    rpage = BufferGetPage(rbuf);
    
    /*
     * step over the high key on the left page while building the 
     * left page pointer.
     */
    itemid = PageGetItemId(lpage, P_FIRSTKEY);
    itemsz = ItemIdGetLength(itemid);
    item = (BTItem) PageGetItem(lpage, itemid);
    new_item = _bt_formitem(&(item->bti_itup));
    ItemPointerSet(&(new_item->bti_itup.t_tid), lbkno, P_FIRSTKEY);
    
    /*
     * insert the left page pointer into the new root page.  the root
     * page is the rightmost page on its level so the "high key" item
     * is the first data item.
     */
    (void) PageAddItem(rootpage, (Item) new_item, itemsz, P_HIKEY, LP_USED);
    pfree(new_item);
    
    /*
     * the right page is the rightmost page on the second level, so 
     * the "high key" item is the first data item on that page as well.
     */
    itemid = PageGetItemId(rpage, P_HIKEY);
    itemsz = ItemIdGetLength(itemid);
    item = (BTItem) PageGetItem(rpage, itemid);
    new_item = _bt_formitem(&(item->bti_itup));
    ItemPointerSet(&(new_item->bti_itup.t_tid), rbkno, P_HIKEY);
    
    /*
     * insert the right page pointer into the new root page.
     */
    (void) PageAddItem(rootpage, (Item) new_item, itemsz, P_FIRSTKEY, LP_USED);
    pfree(new_item);
    
    /* write and let go of the root buffer */
    rootbknum = BufferGetBlockNumber(rootbuf);
    _bt_wrtbuf(rel, rootbuf);
    
    /* update metadata page with new root block number */
    _bt_metaproot(rel, rootbknum);
}

/*
 *  _bt_pgaddtup() -- add a tuple to a particular page in the index.
 *
 *	This routine adds the tuple to the page as requested, and keeps the
 *	write lock and reference associated with the page's buffer.  It is
 *	an error to call pgaddtup() without a write lock and reference.  If
 *	afteritem is non-null, it's the item that we expect our new item
 *	to follow.  Otherwise, we do a binary search for the correct place
 *	and insert the new item there.
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
    Page page;
    BTPageOpaque opaque;
    BTItem chkitem;
    Oid afteroid;
    
    page = BufferGetPage(buf);
    opaque = (BTPageOpaque) PageGetSpecialPointer(page);
    first = P_RIGHTMOST(opaque) ? P_HIKEY : P_FIRSTKEY;
    
    if (afteritem == (BTItem) NULL) {
	itup_off = _bt_binsrch(rel, buf, keysz, itup_scankey, BT_INSERTION);
    } else {
	afteroid = afteritem->bti_oid;
	itup_off = first;
	
	do {
	    chkitem =
		(BTItem) PageGetItem(page, PageGetItemId(page, itup_off));
	    itup_off = OffsetNumberNext(itup_off);
	} while (chkitem->bti_oid != afteroid);
    }

    (void) PageAddItem(page, (Item) btitem, itemsize, itup_off, LP_USED);
    
    /* write the buffer, but hold our lock */
    _bt_wrtnorelbuf(rel, buf);
    
    return (itup_off);
}

/*
 *  _bt_goesonpg() -- Does a new tuple belong on this page?
 *
 *	This is part of the complexity introduced by allowing duplicate
 *	keys into the index.  The tuple belongs on this page if:
 *
 *		+ there is no page to the right of this one; or
 *		+ it is less than the high key on the page; or
 *		+ the item it is to follow ("afteritem") appears on this
 *		  page.
 */
static bool
_bt_goesonpg(Relation rel,
	     Buffer buf,
	     Size keysz,
	     ScanKey scankey,
	     BTItem afteritem)
{
    Page page;
    ItemId hikey;
    BTPageOpaque opaque;
    BTItem chkitem;
    OffsetNumber offnum, maxoff;
    Oid afteroid;
    bool found;
    
    page = BufferGetPage(buf);
    
    /* no right neighbor? */
    opaque = (BTPageOpaque) PageGetSpecialPointer(page);
    if (P_RIGHTMOST(opaque))
	return (true);
    
    /*
     *  this is a non-rightmost page, so it must have a high key item.
     *
     *  If the scan key is < the high key (the min key on the next page),
     *  then it for sure belongs here.
     */
    hikey = PageGetItemId(page, P_HIKEY);
    if (_bt_skeycmp(rel, keysz, scankey, page, hikey, BTLessStrategyNumber))
	return (true);
    
    /*
     *  If the scan key is > the high key, then it for sure doesn't belong
     *  here.
     */
    
    if (_bt_skeycmp(rel, keysz, scankey, page, hikey, BTGreaterStrategyNumber))
	return (false);
    
    /*
     *  If we have no adjacency information, and the item is equal to the
     *  high key on the page (by here it is), then the item does not belong
     *  on this page.
     */
    
    if (afteritem == (BTItem) NULL)
	return (false);
    
    /* damn, have to work for it.  i hate that. */
    afteroid = afteritem->bti_oid;
    maxoff = PageGetMaxOffsetNumber(page);
    
    /*
     *  Search the entire page for the afteroid.  We need to do this, rather
     *  than doing a binary search and starting from there, because if the
     *  key we're searching for is the leftmost key in the tree at this
     *  level, then a binary search will do the wrong thing.  Splits are
     *  pretty infrequent, so the cost isn't as bad as it could be.
     */
    
    found = false;
    for (offnum = P_FIRSTKEY;
	 offnum <= maxoff;
	 offnum = OffsetNumberNext(offnum)) {
	chkitem = (BTItem) PageGetItem(page, PageGetItemId(page, offnum));
	if (chkitem->bti_oid == afteroid) {
	    found = true;
	    break;
	}
    }
    
    return (found);
}

/*
 *	_bt_itemcmp() -- compare item1 to item2 using a requested
 *		         strategy (<, <=, =, >=, >)
 *
 */
bool
_bt_itemcmp(Relation rel,
	    Size keysz,
	    BTItem item1,
	    BTItem item2,
	    StrategyNumber strat)
{
    TupleDesc tupDes;
    IndexTuple indexTuple1, indexTuple2;
    Datum attrDatum1, attrDatum2;
    int i;
    bool isNull;
    bool compare;
    
    tupDes = RelationGetTupleDescriptor(rel);
    indexTuple1 = &(item1->bti_itup);
    indexTuple2 = &(item2->bti_itup);
    
    for (i = 1; i <= keysz; i++) {
	attrDatum1 = index_getattr(indexTuple1, i, tupDes, &isNull);
	attrDatum2 = index_getattr(indexTuple2, i, tupDes, &isNull);
	compare = _bt_invokestrat(rel, i, strat, attrDatum1, attrDatum2);
	if (!compare) {
	    return (false);
	}
    }
    return (true);
}

#if 0
/* gone since updating in place doesn't work in general --djm 11/13/96 */
/*
 *	_bt_updateitem() -- updates the key of the item identified by the
 *			    oid with the key of newItem (done in place if
 *			    possible)
 *
 */
static void
_bt_updateitem(Relation rel,
	       Size keysz,
	       Buffer buf,
	       Oid bti_oid,
	       BTItem newItem)
{
    Page page;
    OffsetNumber maxoff;
    OffsetNumber i;
    ItemPointerData itemPtrData;
    BTItem item;
    IndexTuple oldIndexTuple, newIndexTuple;
    int first;
    
    page = BufferGetPage(buf);
    maxoff = PageGetMaxOffsetNumber(page);
    
    /* locate item on the page */
    first = P_RIGHTMOST((BTPageOpaque) PageGetSpecialPointer(page)) \
        ? P_HIKEY : P_FIRSTKEY;
    i = first;
    do {
	item = (BTItem) PageGetItem(page, PageGetItemId(page, i));
	i = OffsetNumberNext(i);
    } while (i <= maxoff && item->bti_oid != bti_oid);
    
    /* this should never happen (in theory) */
    if (item->bti_oid != bti_oid) {
	elog(FATAL, "_bt_getstackbuf was lying!!");
    }
    
    if(IndexTupleDSize(newItem->bti_itup) >
       IndexTupleDSize(item->bti_itup)) {
	elog(NOTICE, "trying to overwrite a smaller value with a bigger one in _bt_updateitem");
	elog(WARN, "this is not good.");
    }

    oldIndexTuple = &(item->bti_itup);
    newIndexTuple = &(newItem->bti_itup);

	/* keep the original item pointer */
	ItemPointerCopy(&(oldIndexTuple->t_tid), &itemPtrData);
	CopyIndexTuple(newIndexTuple, &oldIndexTuple);
	ItemPointerCopy(&itemPtrData, &(oldIndexTuple->t_tid));
}
#endif
