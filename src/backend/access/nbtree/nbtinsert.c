/*-------------------------------------------------------------------------
 *
 * btinsert.c
 *	  Item insertion in Lehman and Yao btrees for Postgres.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/nbtree/nbtinsert.c,v 1.68 2000/11/16 05:50:58 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/nbtree.h"


typedef struct
{
	/* context data for _bt_checksplitloc */
	Size	newitemsz;			/* size of new item to be inserted */
	bool	non_leaf;			/* T if splitting an internal node */

	bool	have_split;			/* found a valid split? */

	/* these fields valid only if have_split is true */
	bool	newitemonleft;		/* new item on left or right of best split */
	OffsetNumber firstright;	/* best split point */
	int		best_delta;			/* best size delta so far */
} FindSplitData;

void _bt_newroot(Relation rel, Buffer lbuf, Buffer rbuf);

static TransactionId _bt_check_unique(Relation rel, BTItem btitem,
									  Relation heapRel, Buffer buf,
									  ScanKey itup_scankey);
static InsertIndexResult _bt_insertonpg(Relation rel, Buffer buf,
										BTStack stack,
										int keysz, ScanKey scankey,
										BTItem btitem,
										OffsetNumber afteritem);
static Buffer _bt_split(Relation rel, Buffer buf, OffsetNumber firstright,
						OffsetNumber newitemoff, Size newitemsz,
						BTItem newitem, bool newitemonleft,
						OffsetNumber *itup_off, BlockNumber *itup_blkno);
static OffsetNumber _bt_findsplitloc(Relation rel, Page page,
									 OffsetNumber newitemoff,
									 Size newitemsz,
									 bool *newitemonleft);
static void _bt_checksplitloc(FindSplitData *state, OffsetNumber firstright,
							  int leftfree, int rightfree,
							  bool newitemonleft, Size firstrightitemsz);
static Buffer _bt_getstackbuf(Relation rel, BTStack stack);
static void _bt_pgaddtup(Relation rel, Page page,
						 Size itemsize, BTItem btitem,
						 OffsetNumber itup_off, const char *where);
static bool _bt_isequal(TupleDesc itupdesc, Page page, OffsetNumber offnum,
						int keysz, ScanKey scankey);

#ifdef XLOG
static Relation		_xlheapRel;	/* temporary hack */
#endif

/*
 *	_bt_doinsert() -- Handle insertion of a single btitem in the tree.
 *
 *		This routine is called by the public interface routines, btbuild
 *		and btinsert.  By here, btitem is filled in, including the TID.
 */
InsertIndexResult
_bt_doinsert(Relation rel, BTItem btitem,
			 bool index_is_unique, Relation heapRel)
{
	IndexTuple	itup = &(btitem->bti_itup);
	int			natts = rel->rd_rel->relnatts;
	ScanKey		itup_scankey;
	BTStack		stack;
	Buffer		buf;
	InsertIndexResult res;

	/* we need a scan key to do our search, so build one */
	itup_scankey = _bt_mkscankey(rel, itup);

top:
	/* find the page containing this key */
	stack = _bt_search(rel, natts, itup_scankey, &buf, BT_WRITE);

	/* trade in our read lock for a write lock */
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	LockBuffer(buf, BT_WRITE);

	/*
	 * If the page was split between the time that we surrendered our read
	 * lock and acquired our write lock, then this page may no longer be
	 * the right place for the key we want to insert.  In this case, we
	 * need to move right in the tree.	See Lehman and Yao for an
	 * excruciatingly precise description.
	 */
	buf = _bt_moveright(rel, buf, natts, itup_scankey, BT_WRITE);

	/*
	 * If we're not allowing duplicates, make sure the key isn't
	 * already in the index.  XXX this belongs somewhere else, likely
	 */
	if (index_is_unique)
	{
		TransactionId xwait;

		xwait = _bt_check_unique(rel, btitem, heapRel, buf, itup_scankey);

		if (TransactionIdIsValid(xwait))
		{
			/* Have to wait for the other guy ... */
			_bt_relbuf(rel, buf, BT_WRITE);
			XactLockTableWait(xwait);
			/* start over... */
			_bt_freestack(stack);
			goto top;
		}
	}

#ifdef XLOG
	_xlheapRel = heapRel;	/* temporary hack */
#endif

	/* do the insertion */
	res = _bt_insertonpg(rel, buf, stack, natts, itup_scankey, btitem, 0);

	/* be tidy */
	_bt_freestack(stack);
	_bt_freeskey(itup_scankey);

	return res;
}

/*
 *	_bt_check_unique() -- Check for violation of unique index constraint
 *
 * Returns NullTransactionId if there is no conflict, else an xact ID we
 * must wait for to see if it commits a conflicting tuple.  If an actual
 * conflict is detected, no return --- just elog().
 */
static TransactionId
_bt_check_unique(Relation rel, BTItem btitem, Relation heapRel,
				 Buffer buf, ScanKey itup_scankey)
{
	TupleDesc	itupdesc = RelationGetDescr(rel);
	int			natts = rel->rd_rel->relnatts;
	OffsetNumber offset,
				maxoff;
	Page		page;
	BTPageOpaque opaque;
	Buffer		nbuf = InvalidBuffer;
	bool		chtup = true;

	page = BufferGetPage(buf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	maxoff = PageGetMaxOffsetNumber(page);

	/*
	 * Find first item >= proposed new item.  Note we could also get
	 * a pointer to end-of-page here.
	 */
	offset = _bt_binsrch(rel, buf, natts, itup_scankey);

	/*
	 * Scan over all equal tuples, looking for live conflicts.
	 */
	for (;;)
	{
		HeapTupleData htup;
		Buffer		buffer;
		BTItem		cbti;
		BlockNumber nblkno;

		/*
		 * _bt_compare returns 0 for (1,NULL) and (1,NULL) - this's
		 * how we handling NULLs - and so we must not use _bt_compare
		 * in real comparison, but only for ordering/finding items on
		 * pages. - vadim 03/24/97
		 *
		 * make sure the offset points to an actual key
		 * before trying to compare it...
		 */
		if (offset <= maxoff)
		{
			if (! _bt_isequal(itupdesc, page, offset, natts, itup_scankey))
				break;			/* we're past all the equal tuples */

			/*
			 * Have to check is inserted heap tuple deleted one (i.e.
			 * just moved to another place by vacuum)!  We only need to
			 * do this once, but don't want to do it at all unless
			 * we see equal tuples, so as not to slow down unequal case.
			 */
			if (chtup)
			{
				htup.t_self = btitem->bti_itup.t_tid;
				heap_fetch(heapRel, SnapshotDirty, &htup, &buffer);
				if (htup.t_data == NULL)		/* YES! */
					break;
				/* Live tuple is being inserted, so continue checking */
				ReleaseBuffer(buffer);
				chtup = false;
			}

			cbti = (BTItem) PageGetItem(page, PageGetItemId(page, offset));
			htup.t_self = cbti->bti_itup.t_tid;
			heap_fetch(heapRel, SnapshotDirty, &htup, &buffer);
			if (htup.t_data != NULL)		/* it is a duplicate */
			{
				TransactionId xwait =
					(TransactionIdIsValid(SnapshotDirty->xmin)) ?
					SnapshotDirty->xmin : SnapshotDirty->xmax;

				/*
				 * If this tuple is being updated by other transaction
				 * then we have to wait for its commit/abort.
				 */
				ReleaseBuffer(buffer);
				if (TransactionIdIsValid(xwait))
				{
					if (nbuf != InvalidBuffer)
						_bt_relbuf(rel, nbuf, BT_READ);
					/* Tell _bt_doinsert to wait... */
					return xwait;
				}
				/*
				 * Otherwise we have a definite conflict.
				 */
				elog(ERROR, "Cannot insert a duplicate key into unique index %s",
					 RelationGetRelationName(rel));
			}
			/* htup null so no buffer to release */
		}

		/*
		 * Advance to next tuple to continue checking.
		 */
		if (offset < maxoff)
			offset = OffsetNumberNext(offset);
		else
		{
			/* If scankey == hikey we gotta check the next page too */
			if (P_RIGHTMOST(opaque))
				break;
			if (!_bt_isequal(itupdesc, page, P_HIKEY,
							 natts, itup_scankey))
				break;
			nblkno = opaque->btpo_next;
			if (nbuf != InvalidBuffer)
				_bt_relbuf(rel, nbuf, BT_READ);
			nbuf = _bt_getbuf(rel, nblkno, BT_READ);
			page = BufferGetPage(nbuf);
			opaque = (BTPageOpaque) PageGetSpecialPointer(page);
			maxoff = PageGetMaxOffsetNumber(page);
			offset = P_FIRSTDATAKEY(opaque);
		}
	}

	if (nbuf != InvalidBuffer)
		_bt_relbuf(rel, nbuf, BT_READ);

	return NullTransactionId;
}

/*----------
 *	_bt_insertonpg() -- Insert a tuple on a particular page in the index.
 *
 *		This recursive procedure does the following things:
 *
 *			+  finds the right place to insert the tuple.
 *			+  if necessary, splits the target page (making sure that the
 *			   split is equitable as far as post-insert free space goes).
 *			+  inserts the tuple.
 *			+  if the page was split, pops the parent stack, and finds the
 *			   right place to insert the new child pointer (by walking
 *			   right using information stored in the parent stack).
 *			+  invokes itself with the appropriate tuple for the right
 *			   child page on the parent.
 *
 *		On entry, we must have the right buffer on which to do the
 *		insertion, and the buffer must be pinned and locked.  On return,
 *		we will have dropped both the pin and the write lock on the buffer.
 *
 *		If 'afteritem' is >0 then the new tuple must be inserted after the
 *		existing item of that number, noplace else.  If 'afteritem' is 0
 *		then the procedure finds the exact spot to insert it by searching.
 *		(keysz and scankey parameters are used ONLY if afteritem == 0.)
 *
 *		NOTE: if the new key is equal to one or more existing keys, we can
 *		legitimately place it anywhere in the series of equal keys --- in fact,
 *		if the new key is equal to the page's "high key" we can place it on
 *		the next page.  If it is equal to the high key, and there's not room
 *		to insert the new tuple on the current page without splitting, then
 *		we can move right hoping to find more free space and avoid a split.
 *		(We should not move right indefinitely, however, since that leads to
 *		O(N^2) insertion behavior in the presence of many equal keys.)
 *		Once we have chosen the page to put the key on, we'll insert it before
 *		any existing equal keys because of the way _bt_binsrch() works.
 *
 *		The locking interactions in this code are critical.  You should
 *		grok Lehman and Yao's paper before making any changes.  In addition,
 *		you need to understand how we disambiguate duplicate keys in this
 *		implementation, in order to be able to find our location using
 *		L&Y "move right" operations.  Since we may insert duplicate user
 *		keys, and since these dups may propagate up the tree, we use the
 *		'afteritem' parameter to position ourselves correctly for the
 *		insertion on internal pages.
 *----------
 */
static InsertIndexResult
_bt_insertonpg(Relation rel,
			   Buffer buf,
			   BTStack stack,
			   int keysz,
			   ScanKey scankey,
			   BTItem btitem,
			   OffsetNumber afteritem)
{
	InsertIndexResult res;
	Page		page;
	BTPageOpaque lpageop;
	OffsetNumber itup_off;
	BlockNumber itup_blkno;
	OffsetNumber newitemoff;
	OffsetNumber firstright = InvalidOffsetNumber;
	Size		itemsz;

	page = BufferGetPage(buf);
	lpageop = (BTPageOpaque) PageGetSpecialPointer(page);

	itemsz = IndexTupleDSize(btitem->bti_itup)
		+ (sizeof(BTItemData) - sizeof(IndexTupleData));

	itemsz = MAXALIGN(itemsz);	/* be safe, PageAddItem will do this but
								 * we need to be consistent */

	/*
	 * Check whether the item can fit on a btree page at all. (Eventually,
	 * we ought to try to apply TOAST methods if not.) We actually need to
	 * be able to fit three items on every page, so restrict any one item
	 * to 1/3 the per-page available space. Note that at this point,
	 * itemsz doesn't include the ItemId.
	 */
	if (itemsz > (PageGetPageSize(page) - sizeof(PageHeaderData) - MAXALIGN(sizeof(BTPageOpaqueData))) / 3 - sizeof(ItemIdData))
		elog(ERROR, "btree: index item size %lu exceeds maximum %lu",
			 (unsigned long)itemsz,
			 (PageGetPageSize(page) - sizeof(PageHeaderData) - MAXALIGN(sizeof(BTPageOpaqueData))) /3 - sizeof(ItemIdData));

	/*
	 * Determine exactly where new item will go.
	 */
	if (afteritem > 0)
	{
		newitemoff = afteritem + 1;
	}
	else
	{
		/*----------
		 * If we will need to split the page to put the item here,
		 * check whether we can put the tuple somewhere to the right,
		 * instead.  Keep scanning right until we
		 *		(a) find a page with enough free space,
		 *		(b) reach the last page where the tuple can legally go, or
		 *		(c) get tired of searching.
		 * (c) is not flippant; it is important because if there are many
		 * pages' worth of equal keys, it's better to split one of the early
		 * pages than to scan all the way to the end of the run of equal keys
		 * on every insert.  We implement "get tired" as a random choice,
		 * since stopping after scanning a fixed number of pages wouldn't work
		 * well (we'd never reach the right-hand side of previously split
		 * pages).  Currently the probability of moving right is set at 0.99,
		 * which may seem too high to change the behavior much, but it does an
		 * excellent job of preventing O(N^2) behavior with many equal keys.
		 *----------
		 */
		bool	movedright = false;

		while (PageGetFreeSpace(page) < itemsz &&
			   !P_RIGHTMOST(lpageop) &&
			   _bt_compare(rel, keysz, scankey, page, P_HIKEY) == 0 &&
			   random() > (MAX_RANDOM_VALUE / 100))
		{
			/* step right one page */
			BlockNumber		rblkno = lpageop->btpo_next;

			_bt_relbuf(rel, buf, BT_WRITE);
			buf = _bt_getbuf(rel, rblkno, BT_WRITE);
			page = BufferGetPage(buf);
			lpageop = (BTPageOpaque) PageGetSpecialPointer(page);
			movedright = true;
		}
		/*
		 * Now we are on the right page, so find the insert position.
		 * If we moved right at all, we know we should insert at the
		 * start of the page, else must find the position by searching.
		 */
		if (movedright)
			newitemoff = P_FIRSTDATAKEY(lpageop);
		else
			newitemoff = _bt_binsrch(rel, buf, keysz, scankey);
	}

	/*
	 * Do we need to split the page to fit the item on it?
	 *
	 * Note: PageGetFreeSpace() subtracts sizeof(ItemIdData) from its
	 * result, so this comparison is correct even though we appear to
	 * be accounting only for the item and not for its line pointer.
	 */
	if (PageGetFreeSpace(page) < itemsz)
	{
		Buffer		rbuf;
		BlockNumber bknum = BufferGetBlockNumber(buf);
		BlockNumber rbknum;
		bool		is_root = P_ISROOT(lpageop);
		bool		newitemonleft;

		/* Choose the split point */
		firstright = _bt_findsplitloc(rel, page,
									  newitemoff, itemsz,
									  &newitemonleft);

		/* split the buffer into left and right halves */
		rbuf = _bt_split(rel, buf, firstright,
						 newitemoff, itemsz, btitem, newitemonleft,
						 &itup_off, &itup_blkno);

		/*----------
		 * By here,
		 *
		 *		+  our target page has been split;
		 *		+  the original tuple has been inserted;
		 *		+  we have write locks on both the old (left half)
		 *		   and new (right half) buffers, after the split; and
		 *		+  we know the key we want to insert into the parent
		 *		   (it's the "high key" on the left child page).
		 *
		 * We're ready to do the parent insertion.  We need to hold onto the
		 * locks for the child pages until we locate the parent, but we can
		 * release them before doing the actual insertion (see Lehman and Yao
		 * for the reasoning).
		 *
		 * Here we have to do something Lehman and Yao don't talk about:
		 * deal with a root split and construction of a new root.  If our
		 * stack is empty then we have just split a node on what had been
		 * the root level when we descended the tree.  If it is still the
		 * root then we perform a new-root construction.  If it *wasn't*
		 * the root anymore, use the parent pointer to get up to the root
		 * level that someone constructed meanwhile, and find the right
		 * place to insert as for the normal case.
		 *----------
		 */

		if (is_root)
		{
			Assert(stack == (BTStack) NULL);
			/* create a new root node and release the split buffers */
			_bt_newroot(rel, buf, rbuf);
		}
		else
		{
			InsertIndexResult newres;
			BTItem		new_item;
			BTStackData	fakestack;
			BTItem		ritem;
			Buffer		pbuf;

			/* Set up a phony stack entry if we haven't got a real one */
			if (stack == (BTStack) NULL)
			{
				elog(DEBUG, "btree: concurrent ROOT page split");
				stack = &fakestack;
				stack->bts_blkno = lpageop->btpo_parent;
				stack->bts_offset = InvalidOffsetNumber;
				/* bts_btitem will be initialized below */
				stack->bts_parent = NULL;
			}

			/* get high key from left page == lowest key on new right page */
			ritem = (BTItem) PageGetItem(page,
										 PageGetItemId(page, P_HIKEY));

			/* form an index tuple that points at the new right page */
			new_item = _bt_formitem(&(ritem->bti_itup));
			rbknum = BufferGetBlockNumber(rbuf);
			ItemPointerSet(&(new_item->bti_itup.t_tid), rbknum, P_HIKEY);

			/*
			 * Find the parent buffer and get the parent page.
			 *
			 * Oops - if we were moved right then we need to change stack
			 * item! We want to find parent pointing to where we are,
			 * right ?	  - vadim 05/27/97
			 *
			 * Interestingly, this means we didn't *really* need to stack
			 * the parent key at all; all we really care about is the
			 * saved block and offset as a starting point for our search...
			 */
			ItemPointerSet(&(stack->bts_btitem.bti_itup.t_tid),
						   bknum, P_HIKEY);

			pbuf = _bt_getstackbuf(rel, stack);

			/* Now we can write and unlock the children */
			_bt_wrtbuf(rel, rbuf);
			_bt_wrtbuf(rel, buf);

			/* Recursively update the parent */
			newres = _bt_insertonpg(rel, pbuf, stack->bts_parent,
									0, NULL, new_item, stack->bts_offset);

			/* be tidy */
			pfree(newres);
			pfree(new_item);
		}
	}
	else
	{
#ifdef XLOG
		/* XLOG stuff */
		{
			char				xlbuf[sizeof(xl_btree_insert) + 
					sizeof(CommandId) + sizeof(RelFileNode)];
			xl_btree_insert	   *xlrec = (xl_btree_insert*)xlbuf;
			int					hsize = SizeOfBtreeInsert;
			BTItemData			truncitem;
			BTItem				xlitem = btitem;
			Size				xlsize = IndexTupleDSize(btitem->bti_itup) + 
							(sizeof(BTItemData) - sizeof(IndexTupleData));
			XLogRecPtr			recptr;

			xlrec->target.node = rel->rd_node;
			ItemPointerSet(&(xlrec->target.tid), BufferGetBlockNumber(buf), newitemoff);
			if (P_ISLEAF(lpageop))
 			{
				CommandId	cid = GetCurrentCommandId();
				memcpy(xlbuf + hsize, &cid, sizeof(CommandId));
				hsize += sizeof(CommandId);
				memcpy(xlbuf + hsize, &(_xlheapRel->rd_node), sizeof(RelFileNode));
				hsize += sizeof(RelFileNode);
			}
			/*
			 * Read comments in _bt_pgaddtup
			 */
			else if (newitemoff == P_FIRSTDATAKEY(lpageop))
			{
				truncitem = *btitem;
				truncitem.bti_itup.t_info = sizeof(BTItemData);
				xlitem = &truncitem;
				xlsize = sizeof(BTItemData);
			}

			recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_INSERT,
				xlbuf, hsize, (char*) xlitem, xlsize);

			PageSetLSN(page, recptr);
			PageSetSUI(page, ThisStartUpID);
		}
#endif
		_bt_pgaddtup(rel, page, itemsz, btitem, newitemoff, "page");
		itup_off = newitemoff;
		itup_blkno = BufferGetBlockNumber(buf);
		/* Write out the updated page and release pin/lock */
		_bt_wrtbuf(rel, buf);
	}

	/* by here, the new tuple is inserted at itup_blkno/itup_off */
	res = (InsertIndexResult) palloc(sizeof(InsertIndexResultData));
	ItemPointerSet(&(res->pointerData), itup_blkno, itup_off);

	return res;
}

/*
 *	_bt_split() -- split a page in the btree.
 *
 *		On entry, buf is the page to split, and is write-locked and pinned.
 *		firstright is the item index of the first item to be moved to the
 *		new right page.  newitemoff etc. tell us about the new item that
 *		must be inserted along with the data from the old page.
 *
 *		Returns the new right sibling of buf, pinned and write-locked.
 *		The pin and lock on buf are maintained.  *itup_off and *itup_blkno
 *		are set to the exact location where newitem was inserted.
 */
static Buffer
_bt_split(Relation rel, Buffer buf, OffsetNumber firstright,
		  OffsetNumber newitemoff, Size newitemsz, BTItem newitem,
		  bool newitemonleft,
		  OffsetNumber *itup_off, BlockNumber *itup_blkno)
{
	Buffer		rbuf;
	Page		origpage;
	Page		leftpage,
				rightpage;
	BTPageOpaque ropaque,
				lopaque,
				oopaque;
	Buffer		sbuf = 0;
	Page		spage = 0;
	BTPageOpaque sopaque;
	Size		itemsz;
	ItemId		itemid;
	BTItem		item;
	OffsetNumber leftoff,
				rightoff;
	OffsetNumber maxoff;
	OffsetNumber i;

#ifdef XLOG
	BTItem		lhikey;
#endif

	rbuf = _bt_getbuf(rel, P_NEW, BT_WRITE);
	origpage = BufferGetPage(buf);
	leftpage = PageGetTempPage(origpage, sizeof(BTPageOpaqueData));
	rightpage = BufferGetPage(rbuf);

	_bt_pageinit(leftpage, BufferGetPageSize(buf));
	_bt_pageinit(rightpage, BufferGetPageSize(rbuf));

	/* init btree private data */
	oopaque = (BTPageOpaque) PageGetSpecialPointer(origpage);
	lopaque = (BTPageOpaque) PageGetSpecialPointer(leftpage);
	ropaque = (BTPageOpaque) PageGetSpecialPointer(rightpage);

	/* if we're splitting this page, it won't be the root when we're done */
	lopaque->btpo_flags = oopaque->btpo_flags;
	lopaque->btpo_flags &= ~BTP_ROOT;
	ropaque->btpo_flags = lopaque->btpo_flags;
	lopaque->btpo_prev = oopaque->btpo_prev;
	lopaque->btpo_next = BufferGetBlockNumber(rbuf);
	ropaque->btpo_prev = BufferGetBlockNumber(buf);
	ropaque->btpo_next = oopaque->btpo_next;

	/*
	 * Must copy the original parent link into both new pages, even though
	 * it might be quite obsolete by now.  We might need it if this level
	 * is or recently was the root (see README).
	 */
	lopaque->btpo_parent = ropaque->btpo_parent = oopaque->btpo_parent;

	/*
	 * If the page we're splitting is not the rightmost page at its level
	 * in the tree, then the first entry on the page is the high key
	 * for the page.  We need to copy that to the right half.  Otherwise
	 * (meaning the rightmost page case), all the items on the right half
	 * will be user data.
	 */
	rightoff = P_HIKEY;

	if (!P_RIGHTMOST(oopaque))
	{
		itemid = PageGetItemId(origpage, P_HIKEY);
		itemsz = ItemIdGetLength(itemid);
		item = (BTItem) PageGetItem(origpage, itemid);
		if (PageAddItem(rightpage, (Item) item, itemsz, rightoff,
						LP_USED) == InvalidOffsetNumber)
			elog(STOP, "btree: failed to add hikey to the right sibling");
		rightoff = OffsetNumberNext(rightoff);
	}

	/*
	 * The "high key" for the new left page will be the first key that's
	 * going to go into the new right page.  This might be either the
	 * existing data item at position firstright, or the incoming tuple.
	 */
	leftoff = P_HIKEY;
	if (!newitemonleft && newitemoff == firstright)
	{
		/* incoming tuple will become first on right page */
		itemsz = newitemsz;
		item = newitem;
	}
	else
	{
		/* existing item at firstright will become first on right page */
		itemid = PageGetItemId(origpage, firstright);
		itemsz = ItemIdGetLength(itemid);
		item = (BTItem) PageGetItem(origpage, itemid);
	}
#ifdef XLOG
	lhikey = item;
#endif
	if (PageAddItem(leftpage, (Item) item, itemsz, leftoff,
					LP_USED) == InvalidOffsetNumber)
		elog(STOP, "btree: failed to add hikey to the left sibling");
	leftoff = OffsetNumberNext(leftoff);

	/*
	 * Now transfer all the data items to the appropriate page
	 */
	maxoff = PageGetMaxOffsetNumber(origpage);

	for (i = P_FIRSTDATAKEY(oopaque); i <= maxoff; i = OffsetNumberNext(i))
	{
		itemid = PageGetItemId(origpage, i);
		itemsz = ItemIdGetLength(itemid);
		item = (BTItem) PageGetItem(origpage, itemid);

		/* does new item belong before this one? */
		if (i == newitemoff)
		{
			if (newitemonleft)
			{
				_bt_pgaddtup(rel, leftpage, newitemsz, newitem, leftoff,
							 "left sibling");
				*itup_off = leftoff;
				*itup_blkno = BufferGetBlockNumber(buf);
				leftoff = OffsetNumberNext(leftoff);
			}
			else
			{
				_bt_pgaddtup(rel, rightpage, newitemsz, newitem, rightoff,
							 "right sibling");
				*itup_off = rightoff;
				*itup_blkno = BufferGetBlockNumber(rbuf);
				rightoff = OffsetNumberNext(rightoff);
			}
		}

		/* decide which page to put it on */
		if (i < firstright)
		{
			_bt_pgaddtup(rel, leftpage, itemsz, item, leftoff,
							 "left sibling");
			leftoff = OffsetNumberNext(leftoff);
		}
		else
		{
			_bt_pgaddtup(rel, rightpage, itemsz, item, rightoff,
							 "right sibling");
			rightoff = OffsetNumberNext(rightoff);
		}
	}

	/* cope with possibility that newitem goes at the end */
	if (i <= newitemoff)
	{
		if (newitemonleft)
		{
			_bt_pgaddtup(rel, leftpage, newitemsz, newitem, leftoff,
						 "left sibling");
			*itup_off = leftoff;
			*itup_blkno = BufferGetBlockNumber(buf);
			leftoff = OffsetNumberNext(leftoff);
		}
		else
		{
			_bt_pgaddtup(rel, rightpage, newitemsz, newitem, rightoff,
						 "right sibling");
			*itup_off = rightoff;
			*itup_blkno = BufferGetBlockNumber(rbuf);
			rightoff = OffsetNumberNext(rightoff);
		}
	}

	/*
	 * We have to grab the right sibling (if any) and fix the prev
	 * pointer there. We are guaranteed that this is deadlock-free
	 * since no other writer will be holding a lock on that page
	 * and trying to move left, and all readers release locks on a page
	 * before trying to fetch its neighbors.
	 */

	if (!P_RIGHTMOST(ropaque))
	{
		sbuf = _bt_getbuf(rel, ropaque->btpo_next, BT_WRITE);
		spage = BufferGetPage(sbuf);
	}

#ifdef XLOG
	/*
	 * Right sibling is locked, new siblings are prepared, but original
	 * page is not updated yet. Log changes before continuing.
	 *
	 * NO ELOG(ERROR) till right sibling is updated.
	 *
	 */
	{
		char				xlbuf[sizeof(xl_btree_split) + 
			sizeof(CommandId) + sizeof(RelFileNode) + BLCKSZ];
		xl_btree_split	   *xlrec = (xl_btree_split*) xlbuf;
		int					hsize = SizeOfBtreeSplit;
		int					flag = (newitemonleft) ? 
				XLOG_BTREE_SPLEFT : XLOG_BTREE_SPLIT;
		BlockNumber			blkno;
		XLogRecPtr			recptr;

		xlrec->target.node = rel->rd_node;
		ItemPointerSet(&(xlrec->target.tid), *itup_blkno, *itup_off);
		if (P_ISLEAF(lopaque))
		{
			CommandId	cid = GetCurrentCommandId();
			memcpy(xlbuf + hsize, &cid, sizeof(CommandId));
			hsize += sizeof(CommandId);
			memcpy(xlbuf + hsize, &(_xlheapRel->rd_node), sizeof(RelFileNode));
			hsize += sizeof(RelFileNode);
		}
		else
		{
			Size	itemsz = IndexTupleDSize(lhikey->bti_itup) + 
						(sizeof(BTItemData) - sizeof(IndexTupleData));
			memcpy(xlbuf + hsize, (char*) lhikey, itemsz);
			hsize += itemsz;
		}
		if (newitemonleft)
		{
			/*
			 * Read comments in _bt_pgaddtup.
			 * Actually, seems that in non-leaf splits newitem shouldn't
			 * go to first data key position on left page.
			 */
			if (! P_ISLEAF(lopaque) && *itup_off == P_FIRSTDATAKEY(lopaque))
			{
				BTItemData	truncitem = *newitem;
				truncitem.bti_itup.t_info = sizeof(BTItemData);
				memcpy(xlbuf + hsize, &truncitem, sizeof(BTItemData));
				hsize += sizeof(BTItemData);
			}
			else
			{
				Size	itemsz = IndexTupleDSize(newitem->bti_itup) + 
							(sizeof(BTItemData) - sizeof(IndexTupleData));
				memcpy(xlbuf + hsize, (char*) newitem, itemsz);
				hsize += itemsz;
			}
			blkno = BufferGetBlockNumber(rbuf);
			BlockIdSet(&(xlrec->otherblk), blkno);
		}
		else
		{
			blkno = BufferGetBlockNumber(buf);
			BlockIdSet(&(xlrec->otherblk), blkno);
		}

		BlockIdSet(&(xlrec->rightblk), ropaque->btpo_next);

		/* 
		 * Dirrect access to page is not good but faster - we should 
		 * implement some new func in page API.
		 */
		recptr = XLogInsert(RM_BTREE_ID, flag, xlbuf, 
			hsize, (char*)rightpage + ((PageHeader) rightpage)->pd_upper,
			((PageHeader) rightpage)->pd_special - ((PageHeader) rightpage)->pd_upper);

		PageSetLSN(leftpage, recptr);
		PageSetSUI(leftpage, ThisStartUpID);
		PageSetLSN(rightpage, recptr);
		PageSetSUI(rightpage, ThisStartUpID);
		if (!P_RIGHTMOST(ropaque))
		{
			PageSetLSN(spage, recptr);
			PageSetSUI(spage, ThisStartUpID);
		}
	}
#endif

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

	if (!P_RIGHTMOST(ropaque))
	{
		sopaque = (BTPageOpaque) PageGetSpecialPointer(spage);
		sopaque->btpo_prev = BufferGetBlockNumber(rbuf);

		/* write and release the old right sibling */
		_bt_wrtbuf(rel, sbuf);
	}

	/* split's done */
	return rbuf;
}

/*
 *	_bt_findsplitloc() -- find an appropriate place to split a page.
 *
 * The idea here is to equalize the free space that will be on each split
 * page, *after accounting for the inserted tuple*.  (If we fail to account
 * for it, we might find ourselves with too little room on the page that
 * it needs to go into!)
 *
 * We are passed the intended insert position of the new tuple, expressed as
 * the offsetnumber of the tuple it must go in front of.  (This could be
 * maxoff+1 if the tuple is to go at the end.)
 *
 * We return the index of the first existing tuple that should go on the
 * righthand page, plus a boolean indicating whether the new tuple goes on
 * the left or right page.  The bool is necessary to disambiguate the case
 * where firstright == newitemoff.
 */
static OffsetNumber
_bt_findsplitloc(Relation rel,
				 Page page,
				 OffsetNumber newitemoff,
				 Size newitemsz,
				 bool *newitemonleft)
{
	BTPageOpaque opaque;
	OffsetNumber offnum;
	OffsetNumber maxoff;
	ItemId		itemid;
	FindSplitData state;
	int			leftspace,
				rightspace,
				goodenough,
				dataitemtotal,
				dataitemstoleft;

	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	/* Passed-in newitemsz is MAXALIGNED but does not include line pointer */
	newitemsz += sizeof(ItemIdData);
	state.newitemsz = newitemsz;
	state.non_leaf = ! P_ISLEAF(opaque);
	state.have_split = false;

	/* Total free space available on a btree page, after fixed overhead */
	leftspace = rightspace =
		PageGetPageSize(page) - sizeof(PageHeaderData) -
		MAXALIGN(sizeof(BTPageOpaqueData))
		+ sizeof(ItemIdData);

	/*
	 * Finding the best possible split would require checking all the possible
	 * split points, because of the high-key and left-key special cases.
	 * That's probably more work than it's worth; instead, stop as soon as
	 * we find a "good-enough" split, where good-enough is defined as an
	 * imbalance in free space of no more than pagesize/16 (arbitrary...)
	 * This should let us stop near the middle on most pages, instead of
	 * plowing to the end.
	 */
	goodenough = leftspace / 16;

	/* The right page will have the same high key as the old page */
	if (!P_RIGHTMOST(opaque))
	{
		itemid = PageGetItemId(page, P_HIKEY);
		rightspace -= (int) (MAXALIGN(ItemIdGetLength(itemid)) +
							 sizeof(ItemIdData));
	}

	/* Count up total space in data items without actually scanning 'em */
	dataitemtotal = rightspace - (int) PageGetFreeSpace(page);

	/*
	 * Scan through the data items and calculate space usage for a split
	 * at each possible position.
	 */
	dataitemstoleft = 0;
	maxoff = PageGetMaxOffsetNumber(page);

	for (offnum = P_FIRSTDATAKEY(opaque);
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		Size		itemsz;
		int			leftfree,
					rightfree;

		itemid = PageGetItemId(page, offnum);
		itemsz = MAXALIGN(ItemIdGetLength(itemid)) + sizeof(ItemIdData);

		/*
		 * We have to allow for the current item becoming the high key of
		 * the left page; therefore it counts against left space as well
		 * as right space.
		 */
		leftfree = leftspace - dataitemstoleft - (int) itemsz;
		rightfree = rightspace - (dataitemtotal - dataitemstoleft);
		/*
		 * Will the new item go to left or right of split?
		 */
		if (offnum > newitemoff)
			_bt_checksplitloc(&state, offnum, leftfree, rightfree,
							  true, itemsz);
		else if (offnum < newitemoff)
			_bt_checksplitloc(&state, offnum, leftfree, rightfree,
							  false, itemsz);
		else
		{
			/* need to try it both ways! */
			_bt_checksplitloc(&state, offnum, leftfree, rightfree,
							  true, itemsz);
			/* here we are contemplating newitem as first on right */
			_bt_checksplitloc(&state, offnum, leftfree, rightfree,
							  false, newitemsz);
		}

		/* Abort scan once we find a good-enough choice */
		if (state.have_split && state.best_delta <= goodenough)
			break;

		dataitemstoleft += itemsz;
	}

	/*
	 * I believe it is not possible to fail to find a feasible split,
	 * but just in case ...
	 */
	if (! state.have_split)
		elog(FATAL, "_bt_findsplitloc: can't find a feasible split point for %s",
			 RelationGetRelationName(rel));

	*newitemonleft = state.newitemonleft;
	return state.firstright;
}

/*
 * Subroutine to analyze a particular possible split choice (ie, firstright
 * and newitemonleft settings), and record the best split so far in *state.
 */
static void
_bt_checksplitloc(FindSplitData *state, OffsetNumber firstright,
				  int leftfree, int rightfree,
				  bool newitemonleft, Size firstrightitemsz)
{
	/*
	 * Account for the new item on whichever side it is to be put.
	 */
	if (newitemonleft)
		leftfree -= (int) state->newitemsz;
	else
		rightfree -= (int) state->newitemsz;
	/*
	 * If we are not on the leaf level, we will be able to discard the
	 * key data from the first item that winds up on the right page.
	 */
	if (state->non_leaf)
		rightfree += (int) firstrightitemsz -
			(int) (MAXALIGN(sizeof(BTItemData)) + sizeof(ItemIdData));
	/*
	 * If feasible split point, remember best delta.
	 */
	if (leftfree >= 0 && rightfree >= 0)
	{
		int		delta = leftfree - rightfree;

		if (delta < 0)
			delta = -delta;
		if (!state->have_split || delta < state->best_delta)
		{
			state->have_split = true;
			state->newitemonleft = newitemonleft;
			state->firstright = firstright;
			state->best_delta = delta;
		}
	}
}

/*
 *	_bt_getstackbuf() -- Walk back up the tree one step, and find the item
 *						 we last looked at in the parent.
 *
 *		This is possible because we save a bit image of the last item
 *		we looked at in the parent, and the update algorithm guarantees
 *		that if items above us in the tree move, they only move right.
 *
 *		Also, re-set bts_blkno & bts_offset if changed.
 */
static Buffer
_bt_getstackbuf(Relation rel, BTStack stack)
{
	BlockNumber blkno;
	Buffer		buf;
	OffsetNumber start,
				offnum,
				maxoff;
	Page		page;
	ItemId		itemid;
	BTItem		item;
	BTPageOpaque opaque;

	blkno = stack->bts_blkno;
	buf = _bt_getbuf(rel, blkno, BT_WRITE);
	page = BufferGetPage(buf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	maxoff = PageGetMaxOffsetNumber(page);

	start = stack->bts_offset;
	/*
	 * _bt_insertonpg set bts_offset to InvalidOffsetNumber in the
	 * case of concurrent ROOT page split.  Also, watch out for
	 * possibility that page has a high key now when it didn't before.
	 */
	if (start < P_FIRSTDATAKEY(opaque))
		start = P_FIRSTDATAKEY(opaque);

	for (;;)
	{
		/* see if it's on this page */
		for (offnum = start;
			 offnum <= maxoff;
			 offnum = OffsetNumberNext(offnum))
		{
			itemid = PageGetItemId(page, offnum);
			item = (BTItem) PageGetItem(page, itemid);
			if (BTItemSame(item, &stack->bts_btitem))
			{
				/* Return accurate pointer to where link is now */
				stack->bts_blkno = blkno;
				stack->bts_offset = offnum;
				return buf;
			}
		}
		/* by here, the item we're looking for moved right at least one page */
		if (P_RIGHTMOST(opaque))
			elog(FATAL, "_bt_getstackbuf: my bits moved right off the end of the world!"
				 "\n\tRecreate index %s.", RelationGetRelationName(rel));

		blkno = opaque->btpo_next;
		_bt_relbuf(rel, buf, BT_WRITE);
		buf = _bt_getbuf(rel, blkno, BT_WRITE);
		page = BufferGetPage(buf);
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);
		maxoff = PageGetMaxOffsetNumber(page);
		start = P_FIRSTDATAKEY(opaque);
	}
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
 *		locked.  On exit, a new root page exists with entries for the
 *		two new children.  The new root page is neither pinned nor locked, and
 *		we have also written out lbuf and rbuf and dropped their pins/locks.
 */
void
_bt_newroot(Relation rel, Buffer lbuf, Buffer rbuf)
{
	Buffer		rootbuf;
	Page		lpage,
				rpage,
				rootpage;
	BlockNumber lbkno,
				rbkno;
	BlockNumber rootblknum;
	BTPageOpaque rootopaque;
	ItemId		itemid;
	BTItem		item;
	Size		itemsz;
	BTItem		new_item;

#ifdef XLOG
	Buffer		metabuf;
#endif

	/* get a new root page */
	rootbuf = _bt_getbuf(rel, P_NEW, BT_WRITE);
	rootpage = BufferGetPage(rootbuf);
	rootblknum = BufferGetBlockNumber(rootbuf);

#ifdef XLOG
	metabuf = _bt_getbuf(rel, BTREE_METAPAGE,BT_WRITE);
#endif

	/* NO ELOG(ERROR) from here till newroot op is logged */

	/* set btree special data */
	rootopaque = (BTPageOpaque) PageGetSpecialPointer(rootpage);
	rootopaque->btpo_prev = rootopaque->btpo_next = P_NONE;
	rootopaque->btpo_flags |= BTP_ROOT;
	rootopaque->btpo_parent = BTREE_METAPAGE;

	lbkno = BufferGetBlockNumber(lbuf);
	rbkno = BufferGetBlockNumber(rbuf);
	lpage = BufferGetPage(lbuf);
	rpage = BufferGetPage(rbuf);

	/*
	 * Make sure pages in old root level have valid parent links --- we will
	 * need this in _bt_insertonpg() if a concurrent root split happens (see
	 * README).
	 */
	((BTPageOpaque) PageGetSpecialPointer(lpage))->btpo_parent =
		((BTPageOpaque) PageGetSpecialPointer(rpage))->btpo_parent =
		rootblknum;

	/*
	 * Create downlink item for left page (old root).  Since this will be
	 * the first item in a non-leaf page, it implicitly has minus-infinity
	 * key value, so we need not store any actual key in it.
	 */
	itemsz = sizeof(BTItemData);
	new_item = (BTItem) palloc(itemsz);
	new_item->bti_itup.t_info = itemsz;
	ItemPointerSet(&(new_item->bti_itup.t_tid), lbkno, P_HIKEY);

	/*
	 * Insert the left page pointer into the new root page.  The root page
	 * is the rightmost page on its level so there is no "high key" in it;
	 * the two items will go into positions P_HIKEY and P_FIRSTKEY.
	 */
	if (PageAddItem(rootpage, (Item) new_item, itemsz, P_HIKEY, LP_USED) == InvalidOffsetNumber)
		elog(STOP, "btree: failed to add leftkey to new root page");
	pfree(new_item);

	/*
	 * Create downlink item for right page.  The key for it is obtained from
	 * the "high key" position in the left page.
	 */
	itemid = PageGetItemId(lpage, P_HIKEY);
	itemsz = ItemIdGetLength(itemid);
	item = (BTItem) PageGetItem(lpage, itemid);
	new_item = _bt_formitem(&(item->bti_itup));
	ItemPointerSet(&(new_item->bti_itup.t_tid), rbkno, P_HIKEY);

	/*
	 * insert the right page pointer into the new root page.
	 */
	if (PageAddItem(rootpage, (Item) new_item, itemsz, P_FIRSTKEY, LP_USED) == InvalidOffsetNumber)
		elog(STOP, "btree: failed to add rightkey to new root page");
	pfree(new_item);

#ifdef XLOG
	/* XLOG stuff */
	{
		xl_btree_newroot	xlrec;
		Page				metapg = BufferGetPage(metabuf);
		BTMetaPageData	   *metad = BTPageGetMeta(metapg);
		XLogRecPtr			recptr;

		xlrec.node = rel->rd_node;
		BlockIdSet(&(xlrec.rootblk), rootblknum);

		/* 
		 * Dirrect access to page is not good but faster - we should 
		 * implement some new func in page API.
		 */
		recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_NEWROOT,
			(char*)&xlrec, SizeOfBtreeNewroot, 
			(char*)rootpage + ((PageHeader) rootpage)->pd_upper,
			((PageHeader) rootpage)->pd_special - ((PageHeader) rootpage)->pd_upper);

		metad->btm_root = rootblknum;
		(metad->btm_level)++;

		PageSetLSN(rootpage, recptr);
		PageSetSUI(rootpage, ThisStartUpID);
		PageSetLSN(metapg, recptr);
		PageSetSUI(metapg, ThisStartUpID);

		_bt_wrtbuf(rel, metabuf);
	}
#endif

	/* write and let go of the new root buffer */
	_bt_wrtbuf(rel, rootbuf);

#ifndef XLOG
	/* update metadata page with new root block number */
	_bt_metaproot(rel, rootblknum, 0);
#endif

	/* update and release new sibling, and finally the old root */
	_bt_wrtbuf(rel, rbuf);
	_bt_wrtbuf(rel, lbuf);
}

/*
 *	_bt_pgaddtup() -- add a tuple to a particular page in the index.
 *
 *		This routine adds the tuple to the page as requested.  It does
 *		not affect pin/lock status, but you'd better have a write lock
 *		and pin on the target buffer!  Don't forget to write and release
 *		the buffer afterwards, either.
 *
 *		The main difference between this routine and a bare PageAddItem call
 *		is that this code knows that the leftmost data item on a non-leaf
 *		btree page doesn't need to have a key.  Therefore, it strips such
 *		items down to just the item header.  CAUTION: this works ONLY if
 *		we insert the items in order, so that the given itup_off does
 *		represent the final position of the item!
 */
static void
_bt_pgaddtup(Relation rel,
			 Page page,
			 Size itemsize,
			 BTItem btitem,
			 OffsetNumber itup_off,
			 const char *where)
{
	BTPageOpaque opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	BTItemData truncitem;

	if (! P_ISLEAF(opaque) && itup_off == P_FIRSTDATAKEY(opaque))
	{
		memcpy(&truncitem, btitem, sizeof(BTItemData));
		truncitem.bti_itup.t_info = sizeof(BTItemData);
		btitem = &truncitem;
		itemsize = sizeof(BTItemData);
	}

	if (PageAddItem(page, (Item) btitem, itemsize, itup_off,
					LP_USED) == InvalidOffsetNumber)
		elog(STOP, "btree: failed to add item to the %s for %s",
			 where, RelationGetRelationName(rel));
}

/*
 * _bt_isequal - used in _bt_doinsert in check for duplicates.
 *
 * This is very similar to _bt_compare, except for NULL handling.
 * Rule is simple: NOT_NULL not equal NULL, NULL not_equal NULL too.
 */
static bool
_bt_isequal(TupleDesc itupdesc, Page page, OffsetNumber offnum,
			int keysz, ScanKey scankey)
{
	BTItem		btitem;
	IndexTuple	itup;
	int			i;

	/* Better be comparing to a leaf item */
	Assert(P_ISLEAF((BTPageOpaque) PageGetSpecialPointer(page)));

	btitem = (BTItem) PageGetItem(page, PageGetItemId(page, offnum));
	itup = &(btitem->bti_itup);

	for (i = 1; i <= keysz; i++)
	{
		ScanKey		entry = &scankey[i - 1];
		AttrNumber	attno;
		Datum		datum;
		bool		isNull;
		int32		result;

		attno = entry->sk_attno;
		Assert(attno == i);
		datum = index_getattr(itup, attno, itupdesc, &isNull);

		/* NULLs are never equal to anything */
		if (entry->sk_flags & SK_ISNULL || isNull)
			return false;

		result = DatumGetInt32(FunctionCall2(&entry->sk_func,
											 entry->sk_argument,
											 datum));

		if (result != 0)
			return false;
	}

	/* if we get here, the keys are equal */
	return true;
}
