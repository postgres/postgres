/*-------------------------------------------------------------------------
 *
 * btinsert.c
 *	  Item insertion in Lehman and Yao btrees for Postgres.
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/nbtree/nbtinsert.c,v 1.96.2.1 2003/02/21 18:24:54 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/nbtree.h"
#include "miscadmin.h"


typedef struct
{
	/* context data for _bt_checksplitloc */
	Size		newitemsz;		/* size of new item to be inserted */
	bool		is_leaf;		/* T if splitting a leaf page */
	bool		is_rightmost;	/* T if splitting a rightmost page */

	bool		have_split;		/* found a valid split? */

	/* these fields valid only if have_split is true */
	bool		newitemonleft;	/* new item on left or right of best split */
	OffsetNumber firstright;	/* best split point */
	int			best_delta;		/* best size delta so far */
} FindSplitData;

extern bool FixBTree;

Buffer		_bt_fixroot(Relation rel, Buffer oldrootbuf, bool release);
static void _bt_fixtree(Relation rel, BlockNumber blkno);
static void _bt_fixbranch(Relation rel, BlockNumber lblkno,
			  BlockNumber rblkno, BTStack true_stack);
static void _bt_fixlevel(Relation rel, Buffer buf, BlockNumber limit);
static void _bt_fixup(Relation rel, Buffer buf);
static OffsetNumber _bt_getoff(Page page, BlockNumber blkno);

static Buffer _bt_newroot(Relation rel, Buffer lbuf, Buffer rbuf);

static TransactionId _bt_check_unique(Relation rel, BTItem btitem,
				 Relation heapRel, Buffer buf,
				 ScanKey itup_scankey);
static InsertIndexResult _bt_insertonpg(Relation rel, Buffer buf,
			   BTStack stack,
			   int keysz, ScanKey scankey,
			   BTItem btitem,
			   OffsetNumber afteritem);
static void _bt_insertuple(Relation rel, Buffer buf,
			   Size itemsz, BTItem btitem, OffsetNumber newitemoff);
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
static Buffer _bt_getstackbuf(Relation rel, BTStack stack, int access);
static void _bt_pgaddtup(Relation rel, Page page,
			 Size itemsize, BTItem btitem,
			 OffsetNumber itup_off, const char *where);
static bool _bt_isequal(TupleDesc itupdesc, Page page, OffsetNumber offnum,
			int keysz, ScanKey scankey);


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
	 * If we're not allowing duplicates, make sure the key isn't already
	 * in the index.
	 *
	 * NOTE: obviously, _bt_check_unique can only detect keys that are
	 * already in the index; so it cannot defend against concurrent
	 * insertions of the same key.	We protect against that by means of
	 * holding a write lock on the target page.  Any other would-be
	 * inserter of the same key must acquire a write lock on the same
	 * target page, so only one would-be inserter can be making the check
	 * at one time.  Furthermore, once we are past the check we hold write
	 * locks continuously until we have performed our insertion, so no
	 * later inserter can fail to see our insertion.  (This requires some
	 * care in _bt_insertonpg.)
	 *
	 * If we must wait for another xact, we release the lock while waiting,
	 * and then must start over completely.
	 */
	if (index_is_unique)
	{
		TransactionId xwait;

		xwait = _bt_check_unique(rel, btitem, heapRel, buf, itup_scankey);

		if (TransactionIdIsValid(xwait))
		{
			/* Have to wait for the other guy ... */
			_bt_relbuf(rel, buf);
			XactLockTableWait(xwait);
			/* start over... */
			_bt_freestack(stack);
			goto top;
		}
	}

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
 * Returns InvalidTransactionId if there is no conflict, else an xact ID
 * we must wait for to see if it commits a conflicting tuple.	If an actual
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

	page = BufferGetPage(buf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	maxoff = PageGetMaxOffsetNumber(page);

	/*
	 * Find first item >= proposed new item.  Note we could also get a
	 * pointer to end-of-page here.
	 */
	offset = _bt_binsrch(rel, buf, natts, itup_scankey);

	/*
	 * Scan over all equal tuples, looking for live conflicts.
	 */
	for (;;)
	{
		HeapTupleData htup;
		Buffer		hbuffer;
		ItemId		curitemid;
		BTItem		cbti;
		BlockNumber nblkno;

		/*
		 * make sure the offset points to an actual key before trying to
		 * compare it...
		 */
		if (offset <= maxoff)
		{
			/*
			 * _bt_compare returns 0 for (1,NULL) and (1,NULL) - this's
			 * how we handling NULLs - and so we must not use _bt_compare
			 * in real comparison, but only for ordering/finding items on
			 * pages. - vadim 03/24/97
			 */
			if (!_bt_isequal(itupdesc, page, offset, natts, itup_scankey))
				break;			/* we're past all the equal tuples */

			curitemid = PageGetItemId(page, offset);

			/*
			 * We can skip the heap fetch if the item is marked killed.
			 */
			if (!ItemIdDeleted(curitemid))
			{
				cbti = (BTItem) PageGetItem(page, curitemid);
				htup.t_self = cbti->bti_itup.t_tid;
				if (heap_fetch(heapRel, SnapshotDirty, &htup, &hbuffer,
							   true, NULL))
				{
					/* it is a duplicate */
					TransactionId xwait =
					(TransactionIdIsValid(SnapshotDirty->xmin)) ?
					SnapshotDirty->xmin : SnapshotDirty->xmax;

					ReleaseBuffer(hbuffer);

					/*
					 * If this tuple is being updated by other transaction
					 * then we have to wait for its commit/abort.
					 */
					if (TransactionIdIsValid(xwait))
					{
						if (nbuf != InvalidBuffer)
							_bt_relbuf(rel, nbuf);
						/* Tell _bt_doinsert to wait... */
						return xwait;
					}

					/*
					 * Otherwise we have a definite conflict.
					 */
					elog(ERROR, "Cannot insert a duplicate key into unique index %s",
						 RelationGetRelationName(rel));
				}
				else if (htup.t_data != NULL)
				{
					/*
					 * Hmm, if we can't see the tuple, maybe it can be
					 * marked killed.  This logic should match
					 * index_getnext and btgettuple.
					 */
					uint16		sv_infomask;

					LockBuffer(hbuffer, BUFFER_LOCK_SHARE);
					sv_infomask = htup.t_data->t_infomask;
					if (HeapTupleSatisfiesVacuum(htup.t_data,
												 RecentGlobalXmin) ==
						HEAPTUPLE_DEAD)
					{
						curitemid->lp_flags |= LP_DELETE;
						SetBufferCommitInfoNeedsSave(buf);
					}
					if (sv_infomask != htup.t_data->t_infomask)
						SetBufferCommitInfoNeedsSave(hbuffer);
					LockBuffer(hbuffer, BUFFER_LOCK_UNLOCK);
					ReleaseBuffer(hbuffer);
				}
			}
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
				_bt_relbuf(rel, nbuf);
			nbuf = _bt_getbuf(rel, nblkno, BT_READ);
			page = BufferGetPage(nbuf);
			opaque = (BTPageOpaque) PageGetSpecialPointer(page);
			maxoff = PageGetMaxOffsetNumber(page);
			offset = P_FIRSTDATAKEY(opaque);
		}
	}

	if (nbuf != InvalidBuffer)
		_bt_relbuf(rel, nbuf);

	return InvalidTransactionId;
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
 *		the next page.	If it is equal to the high key, and there's not room
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
	if (itemsz > BTMaxItemSize(page))
		elog(ERROR, "btree: index item size %lu exceeds maximum %lu",
			 (unsigned long) itemsz, BTMaxItemSize(page));

	/*
	 * Determine exactly where new item will go.
	 */
	if (afteritem > 0)
		newitemoff = afteritem + 1;
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
		 * pages).	Currently the probability of moving right is set at 0.99,
		 * which may seem too high to change the behavior much, but it does an
		 * excellent job of preventing O(N^2) behavior with many equal keys.
		 *----------
		 */
		bool		movedright = false;

		while (PageGetFreeSpace(page) < itemsz &&
			   !P_RIGHTMOST(lpageop) &&
			   _bt_compare(rel, keysz, scankey, page, P_HIKEY) == 0 &&
			   random() > (MAX_RANDOM_VALUE / 100))
		{
			/* step right one page */
			BlockNumber rblkno = lpageop->btpo_next;
			Buffer		rbuf;

			/*
			 * must write-lock next page before releasing write lock on
			 * current page; else someone else's _bt_check_unique scan
			 * could fail to see our insertion.
			 */
			rbuf = _bt_getbuf(rel, rblkno, BT_WRITE);
			_bt_relbuf(rel, buf);
			buf = rbuf;
			page = BufferGetPage(buf);
			lpageop = (BTPageOpaque) PageGetSpecialPointer(page);
			movedright = true;
		}

		/*
		 * Now we are on the right page, so find the insert position. If
		 * we moved right at all, we know we should insert at the start of
		 * the page, else must find the position by searching.
		 */
		if (movedright)
			newitemoff = P_FIRSTDATAKEY(lpageop);
		else
			newitemoff = _bt_binsrch(rel, buf, keysz, scankey);
	}

	/*
	 * Do we need to split the page to fit the item on it?
	 *
	 * Note: PageGetFreeSpace() subtracts sizeof(ItemIdData) from its result,
	 * so this comparison is correct even though we appear to be
	 * accounting only for the item and not for its line pointer.
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
			Buffer		rootbuf;

			Assert(stack == (BTStack) NULL);
			/* create a new root node and release the split buffers */
			rootbuf = _bt_newroot(rel, buf, rbuf);
			_bt_wrtbuf(rel, rootbuf);
			_bt_wrtbuf(rel, rbuf);
			_bt_wrtbuf(rel, buf);
		}
		else
		{
			InsertIndexResult newres;
			BTItem		new_item;
			BTStackData fakestack;
			BTItem		ritem;
			Buffer		pbuf;

			/* If root page was splitted */
			if (stack == (BTStack) NULL)
			{
				elog(LOG, "btree: concurrent ROOT page split");

				/*
				 * If root page splitter failed to create new root page
				 * then old root' btpo_parent still points to metapage. We
				 * have to fix root page in this case.
				 */
				if (BTreeInvalidParent(lpageop))
				{
					if (!FixBTree)
						elog(ERROR, "bt_insertonpg[%s]: no root page found", RelationGetRelationName(rel));
					_bt_wrtbuf(rel, rbuf);
					_bt_wrtnorelbuf(rel, buf);
					elog(WARNING, "bt_insertonpg[%s]: root page unfound - fixing upper levels", RelationGetRelationName(rel));
					_bt_fixup(rel, buf);
					goto formres;
				}

				/*
				 * Set up a phony stack entry if we haven't got a real one
				 */
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
			 * Interestingly, this means we didn't *really* need to stack the
			 * parent key at all; all we really care about is the saved
			 * block and offset as a starting point for our search...
			 */
			ItemPointerSet(&(stack->bts_btitem.bti_itup.t_tid),
						   bknum, P_HIKEY);

			pbuf = _bt_getstackbuf(rel, stack, BT_WRITE);

			/* Now we can write and unlock the children */
			_bt_wrtbuf(rel, rbuf);
			_bt_wrtbuf(rel, buf);

			if (pbuf == InvalidBuffer)
			{
				if (!FixBTree)
					elog(ERROR, "_bt_getstackbuf: my bits moved right off the end of the world!"
						 "\n\tRecreate index %s.", RelationGetRelationName(rel));
				pfree(new_item);
				elog(WARNING, "bt_insertonpg[%s]: parent page unfound - fixing branch", RelationGetRelationName(rel));
				_bt_fixbranch(rel, bknum, rbknum, stack);
				goto formres;
			}
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
		itup_off = newitemoff;
		itup_blkno = BufferGetBlockNumber(buf);

		_bt_insertuple(rel, buf, itemsz, btitem, newitemoff);

		/* Write out the updated page and release pin/lock */
		_bt_wrtbuf(rel, buf);
	}

formres:;
	/* by here, the new tuple is inserted at itup_blkno/itup_off */
	res = (InsertIndexResult) palloc(sizeof(InsertIndexResultData));
	ItemPointerSet(&(res->pointerData), itup_blkno, itup_off);

	return res;
}

static void
_bt_insertuple(Relation rel, Buffer buf,
			   Size itemsz, BTItem btitem, OffsetNumber newitemoff)
{
	Page		page = BufferGetPage(buf);
	BTPageOpaque pageop = (BTPageOpaque) PageGetSpecialPointer(page);

	START_CRIT_SECTION();

	_bt_pgaddtup(rel, page, itemsz, btitem, newitemoff, "page");

	/* XLOG stuff */
	if (!rel->rd_istemp)
	{
		xl_btree_insert xlrec;
		uint8		flag = XLOG_BTREE_INSERT;
		XLogRecPtr	recptr;
		XLogRecData rdata[2];
		BTItemData	truncitem;

		xlrec.target.node = rel->rd_node;
		ItemPointerSet(&(xlrec.target.tid), BufferGetBlockNumber(buf), newitemoff);
		rdata[0].buffer = InvalidBuffer;
		rdata[0].data = (char *) &xlrec;
		rdata[0].len = SizeOfBtreeInsert;
		rdata[0].next = &(rdata[1]);

		/* Read comments in _bt_pgaddtup */
		if (!(P_ISLEAF(pageop)) && newitemoff == P_FIRSTDATAKEY(pageop))
		{
			truncitem = *btitem;
			truncitem.bti_itup.t_info = sizeof(BTItemData);
			rdata[1].data = (char *) &truncitem;
			rdata[1].len = sizeof(BTItemData);
		}
		else
		{
			rdata[1].data = (char *) btitem;
			rdata[1].len = IndexTupleDSize(btitem->bti_itup) +
				(sizeof(BTItemData) - sizeof(IndexTupleData));
		}
		rdata[1].buffer = buf;
		rdata[1].next = NULL;
		if (P_ISLEAF(pageop))
			flag |= XLOG_BTREE_LEAF;

		recptr = XLogInsert(RM_BTREE_ID, flag, rdata);

		PageSetLSN(page, recptr);
		PageSetSUI(page, ThisStartUpID);
	}

	END_CRIT_SECTION();
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
	BTPageOpaque sopaque = 0;
	Size		itemsz;
	ItemId		itemid;
	BTItem		item;
	OffsetNumber leftoff,
				rightoff;
	OffsetNumber maxoff;
	OffsetNumber i;
	BTItem		lhikey;

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
	 * in the tree, then the first entry on the page is the high key for
	 * the page.  We need to copy that to the right half.  Otherwise
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
			elog(PANIC, "btree: failed to add hikey to the right sibling");
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
	lhikey = item;
	if (PageAddItem(leftpage, (Item) item, itemsz, leftoff,
					LP_USED) == InvalidOffsetNumber)
		elog(PANIC, "btree: failed to add hikey to the left sibling");
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
	 * We have to grab the right sibling (if any) and fix the prev pointer
	 * there. We are guaranteed that this is deadlock-free since no other
	 * writer will be holding a lock on that page and trying to move left,
	 * and all readers release locks on a page before trying to fetch its
	 * neighbors.
	 */

	if (!P_RIGHTMOST(ropaque))
	{
		sbuf = _bt_getbuf(rel, ropaque->btpo_next, BT_WRITE);
		spage = BufferGetPage(sbuf);
		sopaque = (BTPageOpaque) PageGetSpecialPointer(spage);
	}

	/*
	 * Right sibling is locked, new siblings are prepared, but original
	 * page is not updated yet. Log changes before continuing.
	 *
	 * NO ELOG(ERROR) till right sibling is updated.
	 */
	START_CRIT_SECTION();

	if (!P_RIGHTMOST(ropaque))
		sopaque->btpo_prev = BufferGetBlockNumber(rbuf);

	/* XLOG stuff */
	if (!rel->rd_istemp)
	{
		xl_btree_split xlrec;
		int			flag = (newitemonleft) ?
		XLOG_BTREE_SPLEFT : XLOG_BTREE_SPLIT;
		BlockNumber blkno;
		XLogRecPtr	recptr;
		XLogRecData rdata[4];

		xlrec.target.node = rel->rd_node;
		ItemPointerSet(&(xlrec.target.tid), *itup_blkno, *itup_off);
		if (newitemonleft)
		{
			blkno = BufferGetBlockNumber(rbuf);
			BlockIdSet(&(xlrec.otherblk), blkno);
		}
		else
		{
			blkno = BufferGetBlockNumber(buf);
			BlockIdSet(&(xlrec.otherblk), blkno);
		}
		BlockIdSet(&(xlrec.parentblk), lopaque->btpo_parent);
		BlockIdSet(&(xlrec.leftblk), lopaque->btpo_prev);
		BlockIdSet(&(xlrec.rightblk), ropaque->btpo_next);

		/*
		 * Direct access to page is not good but faster - we should
		 * implement some new func in page API.
		 */
		xlrec.leftlen = ((PageHeader) leftpage)->pd_special -
			((PageHeader) leftpage)->pd_upper;
		rdata[0].buffer = InvalidBuffer;
		rdata[0].data = (char *) &xlrec;
		rdata[0].len = SizeOfBtreeSplit;
		rdata[0].next = &(rdata[1]);

		rdata[1].buffer = InvalidBuffer;
		rdata[1].data = (char *) leftpage + ((PageHeader) leftpage)->pd_upper;
		rdata[1].len = xlrec.leftlen;
		rdata[1].next = &(rdata[2]);

		rdata[2].buffer = InvalidBuffer;
		rdata[2].data = (char *) rightpage + ((PageHeader) rightpage)->pd_upper;
		rdata[2].len = ((PageHeader) rightpage)->pd_special -
			((PageHeader) rightpage)->pd_upper;
		rdata[2].next = NULL;

		if (!P_RIGHTMOST(ropaque))
		{
			rdata[2].next = &(rdata[3]);
			rdata[3].buffer = sbuf;
			rdata[3].data = NULL;
			rdata[3].len = 0;
			rdata[3].next = NULL;
		}

		if (P_ISLEAF(lopaque))
			flag |= XLOG_BTREE_LEAF;

		recptr = XLogInsert(RM_BTREE_ID, flag, rdata);

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

	END_CRIT_SECTION();

	/* write and release the old right sibling */
	if (!P_RIGHTMOST(ropaque))
		_bt_wrtbuf(rel, sbuf);

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
 * If the page is the rightmost page on its level, we instead try to arrange
 * for twice as much free space on the right as on the left.  In this way,
 * when we are inserting successively increasing keys (consider sequences,
 * timestamps, etc) we will end up with a tree whose pages are about 67% full,
 * instead of the 50% full result that we'd get without this special case.
 * (We could bias it even further to make the initially-loaded tree more full.
 * But since the steady-state load for a btree is about 70%, we'd likely just
 * be making more page-splitting work for ourselves later on, when we start
 * seeing updates to existing tuples.)
 *
 * We are passed the intended insert position of the new tuple, expressed as
 * the offsetnumber of the tuple it must go in front of.  (This could be
 * maxoff+1 if the tuple is to go at the end.)
 *
 * We return the index of the first existing tuple that should go on the
 * righthand page, plus a boolean indicating whether the new tuple goes on
 * the left or right page.	The bool is necessary to disambiguate the case
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
	state.is_leaf = P_ISLEAF(opaque);
	state.is_rightmost = P_RIGHTMOST(opaque);
	state.have_split = false;

	/* Total free space available on a btree page, after fixed overhead */
	leftspace = rightspace =
		PageGetPageSize(page) - SizeOfPageHeaderData -
		MAXALIGN(sizeof(BTPageOpaqueData));

	/*
	 * Finding the best possible split would require checking all the
	 * possible split points, because of the high-key and left-key special
	 * cases. That's probably more work than it's worth; instead, stop as
	 * soon as we find a "good-enough" split, where good-enough is defined
	 * as an imbalance in free space of no more than pagesize/16
	 * (arbitrary...) This should let us stop near the middle on most
	 * pages, instead of plowing to the end.
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
	 * I believe it is not possible to fail to find a feasible split, but
	 * just in case ...
	 */
	if (!state.have_split)
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
	 * If we are not on the leaf level, we will be able to discard the key
	 * data from the first item that winds up on the right page.
	 */
	if (!state->is_leaf)
		rightfree += (int) firstrightitemsz -
			(int) (MAXALIGN(sizeof(BTItemData)) + sizeof(ItemIdData));

	/*
	 * If feasible split point, remember best delta.
	 */
	if (leftfree >= 0 && rightfree >= 0)
	{
		int			delta;

		if (state->is_rightmost)
		{
			/*
			 * On a rightmost page, try to equalize right free space with
			 * twice the left free space.  See comments for
			 * _bt_findsplitloc.
			 */
			delta = (2 * leftfree) - rightfree;
		}
		else
		{
			/* Otherwise, aim for equal free space on both sides */
			delta = leftfree - rightfree;
		}

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
_bt_getstackbuf(Relation rel, BTStack stack, int access)
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
	buf = _bt_getbuf(rel, blkno, access);
	page = BufferGetPage(buf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	maxoff = PageGetMaxOffsetNumber(page);

	start = stack->bts_offset;

	/*
	 * _bt_insertonpg set bts_offset to InvalidOffsetNumber in the case of
	 * concurrent ROOT page split.	Also, watch out for possibility that
	 * page has a high key now when it didn't before.
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

		/*
		 * by here, the item we're looking for moved right at least one
		 * page
		 */
		if (P_RIGHTMOST(opaque))
		{
			_bt_relbuf(rel, buf);
			return (InvalidBuffer);
		}

		blkno = opaque->btpo_next;
		_bt_relbuf(rel, buf);
		buf = _bt_getbuf(rel, blkno, access);
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
 *		locked. On exit, a new root page exists with entries for the
 *		two new children, metapage is updated and unlocked/unpinned.
 *		The new root buffer is returned to caller which has to unlock/unpin
 *		lbuf, rbuf & rootbuf.
 */
static Buffer
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
	Buffer		metabuf;
	Page		metapg;
	BTMetaPageData *metad;

	/* get a new root page */
	rootbuf = _bt_getbuf(rel, P_NEW, BT_WRITE);
	rootpage = BufferGetPage(rootbuf);
	rootblknum = BufferGetBlockNumber(rootbuf);
	metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_WRITE);
	metapg = BufferGetPage(metabuf);
	metad = BTPageGetMeta(metapg);

	/* NO ELOG(ERROR) from here till newroot op is logged */
	START_CRIT_SECTION();

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
	 * Make sure pages in old root level have valid parent links --- we
	 * will need this in _bt_insertonpg() if a concurrent root split
	 * happens (see README).
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
		elog(PANIC, "btree: failed to add leftkey to new root page");
	pfree(new_item);

	/*
	 * Create downlink item for right page.  The key for it is obtained
	 * from the "high key" position in the left page.
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
		elog(PANIC, "btree: failed to add rightkey to new root page");
	pfree(new_item);

	metad->btm_root = rootblknum;
	(metad->btm_level)++;

	/* XLOG stuff */
	if (!rel->rd_istemp)
	{
		xl_btree_newroot xlrec;
		XLogRecPtr	recptr;
		XLogRecData rdata[2];

		xlrec.node = rel->rd_node;
		xlrec.level = metad->btm_level;
		BlockIdSet(&(xlrec.rootblk), rootblknum);
		rdata[0].buffer = InvalidBuffer;
		rdata[0].data = (char *) &xlrec;
		rdata[0].len = SizeOfBtreeNewroot;
		rdata[0].next = &(rdata[1]);

		/*
		 * Direct access to page is not good but faster - we should
		 * implement some new func in page API.
		 */
		rdata[1].buffer = InvalidBuffer;
		rdata[1].data = (char *) rootpage + ((PageHeader) rootpage)->pd_upper;
		rdata[1].len = ((PageHeader) rootpage)->pd_special -
			((PageHeader) rootpage)->pd_upper;
		rdata[1].next = NULL;

		recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_NEWROOT, rdata);

		PageSetLSN(rootpage, recptr);
		PageSetSUI(rootpage, ThisStartUpID);
		PageSetLSN(metapg, recptr);
		PageSetSUI(metapg, ThisStartUpID);

		/* we changed their btpo_parent */
		PageSetLSN(lpage, recptr);
		PageSetSUI(lpage, ThisStartUpID);
		PageSetLSN(rpage, recptr);
		PageSetSUI(rpage, ThisStartUpID);
	}

	END_CRIT_SECTION();

	/* write and let go of metapage buffer */
	_bt_wrtbuf(rel, metabuf);

	return (rootbuf);
}

/*
 * In the event old root page was splitted but no new one was created we
 * build required parent levels keeping write lock on old root page.
 * Note: it's assumed that old root page' btpo_parent points to meta page,
 * ie not to parent page. On exit, new root page buffer is write locked.
 * If "release" is TRUE then oldrootbuf will be released immediately
 * after upper level is builded.
 */
Buffer
_bt_fixroot(Relation rel, Buffer oldrootbuf, bool release)
{
	Buffer		rootbuf;
	BlockNumber rootblk;
	Page		rootpage;
	XLogRecPtr	rootLSN;
	Page		oldrootpage = BufferGetPage(oldrootbuf);
	BTPageOpaque oldrootopaque = (BTPageOpaque)
	PageGetSpecialPointer(oldrootpage);
	Buffer		buf,
				leftbuf,
				rightbuf;
	Page		page,
				leftpage,
				rightpage;
	BTPageOpaque opaque,
				leftopaque,
				rightopaque;
	OffsetNumber newitemoff;
	BTItem		btitem,
				ritem;
	Size		itemsz;

	if (!P_LEFTMOST(oldrootopaque) || P_RIGHTMOST(oldrootopaque))
		elog(ERROR, "bt_fixroot: not valid old root page");

	/* Read right neighbor and create new root page */
	leftbuf = _bt_getbuf(rel, oldrootopaque->btpo_next, BT_WRITE);
	leftpage = BufferGetPage(leftbuf);
	leftopaque = (BTPageOpaque) PageGetSpecialPointer(leftpage);
	rootbuf = _bt_newroot(rel, oldrootbuf, leftbuf);
	rootpage = BufferGetPage(rootbuf);
	rootLSN = PageGetLSN(rootpage);
	rootblk = BufferGetBlockNumber(rootbuf);

	/* parent page where to insert pointers */
	buf = rootbuf;
	page = BufferGetPage(buf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	/*
	 * Now read other pages (if any) on level and add them to new root.
	 * Here we break one of our locking rules - never hold lock on parent
	 * page when acquiring lock on its child, - but we free from deadlock:
	 *
	 * If concurrent process will split one of pages on this level then it
	 * will see either btpo_parent == metablock or btpo_parent == rootblk.
	 * In first case it will give up its locks and walk to the leftmost
	 * page (oldrootbuf) in _bt_fixup() - ie it will wait for us and let
	 * us continue. In second case it will try to lock rootbuf keeping its
	 * locks on buffers we already passed, also waiting for us. If we'll
	 * have to unlock rootbuf (split it) and that process will have to
	 * split page of new level we created (level of rootbuf) then it will
	 * wait while we create upper level. Etc.
	 */
	while (!P_RIGHTMOST(leftopaque))
	{
		rightbuf = _bt_getbuf(rel, leftopaque->btpo_next, BT_WRITE);
		rightpage = BufferGetPage(rightbuf);
		rightopaque = (BTPageOpaque) PageGetSpecialPointer(rightpage);

		/*
		 * Update LSN & StartUpID of child page buffer to ensure that it
		 * will be written on disk after flushing log record for new root
		 * creation. Unfortunately, for the moment (?) we do not log this
		 * operation and so possibly break our rule to log entire page
		 * content on first after checkpoint modification.
		 */
		HOLD_INTERRUPTS();
		rightopaque->btpo_parent = rootblk;
		if (XLByteLT(PageGetLSN(rightpage), rootLSN))
			PageSetLSN(rightpage, rootLSN);
		PageSetSUI(rightpage, ThisStartUpID);
		RESUME_INTERRUPTS();

		ritem = (BTItem) PageGetItem(leftpage, PageGetItemId(leftpage, P_HIKEY));
		btitem = _bt_formitem(&(ritem->bti_itup));
		ItemPointerSet(&(btitem->bti_itup.t_tid), leftopaque->btpo_next, P_HIKEY);
		itemsz = IndexTupleDSize(btitem->bti_itup)
			+ (sizeof(BTItemData) - sizeof(IndexTupleData));
		itemsz = MAXALIGN(itemsz);

		newitemoff = OffsetNumberNext(PageGetMaxOffsetNumber(page));

		if (PageGetFreeSpace(page) < itemsz)
		{
			Buffer		newbuf;
			OffsetNumber firstright;
			OffsetNumber itup_off;
			BlockNumber itup_blkno;
			bool		newitemonleft;

			firstright = _bt_findsplitloc(rel, page,
									 newitemoff, itemsz, &newitemonleft);
			newbuf = _bt_split(rel, buf, firstright,
							   newitemoff, itemsz, btitem, newitemonleft,
							   &itup_off, &itup_blkno);
			/* Keep lock on new "root" buffer ! */
			if (buf != rootbuf)
				_bt_relbuf(rel, buf);
			buf = newbuf;
			page = BufferGetPage(buf);
			opaque = (BTPageOpaque) PageGetSpecialPointer(page);
		}
		else
			_bt_insertuple(rel, buf, itemsz, btitem, newitemoff);

		/* give up left buffer */
		_bt_wrtbuf(rel, leftbuf);
		pfree(btitem);
		leftbuf = rightbuf;
		leftpage = rightpage;
		leftopaque = rightopaque;
	}

	/* give up rightmost page buffer */
	_bt_wrtbuf(rel, leftbuf);

	/*
	 * Here we hold locks on old root buffer, new root buffer we've
	 * created with _bt_newroot() - rootbuf, - and buf we've used for last
	 * insert ops - buf. If rootbuf != buf then we have to create at least
	 * one more level. And if "release" is TRUE then we give up
	 * oldrootbuf.
	 */
	if (release)
		_bt_wrtbuf(rel, oldrootbuf);

	if (rootbuf != buf)
	{
		_bt_wrtbuf(rel, buf);
		return (_bt_fixroot(rel, rootbuf, true));
	}

	return (rootbuf);
}

/*
 * Using blkno of leftmost page on a level inside tree this func
 * checks/fixes tree from this level up to the root page.
 */
static void
_bt_fixtree(Relation rel, BlockNumber blkno)
{
	Buffer		buf;
	Page		page;
	BTPageOpaque opaque;
	BlockNumber pblkno;

	for (;;)
	{
		buf = _bt_getbuf(rel, blkno, BT_READ);
		page = BufferGetPage(buf);
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);
		if (!P_LEFTMOST(opaque) || P_ISLEAF(opaque))
			elog(ERROR, "bt_fixtree[%s]: invalid start page (need to recreate index)", RelationGetRelationName(rel));
		pblkno = opaque->btpo_parent;

		/* check/fix entire level */
		_bt_fixlevel(rel, buf, InvalidBlockNumber);

		/*
		 * No pins/locks are held here. Re-read start page if its
		 * btpo_parent pointed to meta page else go up one level.
		 *
		 * XXX have to catch InvalidBlockNumber at the moment -:(
		 */
		if (pblkno == BTREE_METAPAGE || pblkno == InvalidBlockNumber)
		{
			buf = _bt_getbuf(rel, blkno, BT_WRITE);
			page = BufferGetPage(buf);
			opaque = (BTPageOpaque) PageGetSpecialPointer(page);
			if (P_ISROOT(opaque))
			{
				/* Tree is Ok now */
				_bt_relbuf(rel, buf);
				return;
			}
			/* Call _bt_fixroot() if there is no upper level */
			if (BTreeInvalidParent(opaque))
			{
				elog(WARNING, "bt_fixtree[%s]: fixing root page", RelationGetRelationName(rel));
				buf = _bt_fixroot(rel, buf, true);
				_bt_relbuf(rel, buf);
				return;
			}
			/* Have to go up one level */
			pblkno = opaque->btpo_parent;
			_bt_relbuf(rel, buf);
		}
		blkno = pblkno;
	}

}

/*
 * Check/fix level starting from page in buffer buf up to block
 * limit on *child* level (or till rightmost child page if limit
 * is InvalidBlockNumber). Start buffer must be read locked.
 * No pins/locks are held on exit.
 */
static void
_bt_fixlevel(Relation rel, Buffer buf, BlockNumber limit)
{
	BlockNumber blkno = BufferGetBlockNumber(buf);
	Page		page;
	BTPageOpaque opaque;
	BlockNumber cblkno[3];
	OffsetNumber coff[3];
	Buffer		cbuf[3];
	Page		cpage[3];
	BTPageOpaque copaque[3];
	BTItem		btitem;
	int			cidx,
				i;
	bool		goodbye = false;
	char		tbuf[BLCKSZ];

	page = BufferGetPage(buf);
	/* copy page to temp storage */
	memmove(tbuf, page, PageGetPageSize(page));
	_bt_relbuf(rel, buf);

	page = (Page) tbuf;
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	/* Initialize first child data */
	coff[0] = P_FIRSTDATAKEY(opaque);
	if (coff[0] > PageGetMaxOffsetNumber(page))
		elog(ERROR, "bt_fixlevel[%s]: invalid maxoff on start page (need to recreate index)", RelationGetRelationName(rel));
	btitem = (BTItem) PageGetItem(page, PageGetItemId(page, coff[0]));
	cblkno[0] = ItemPointerGetBlockNumber(&(btitem->bti_itup.t_tid));
	cbuf[0] = _bt_getbuf(rel, cblkno[0], BT_READ);
	cpage[0] = BufferGetPage(cbuf[0]);
	copaque[0] = (BTPageOpaque) PageGetSpecialPointer(cpage[0]);
	if (P_LEFTMOST(opaque) && !P_LEFTMOST(copaque[0]))
		elog(ERROR, "bt_fixtlevel[%s]: non-leftmost child page of leftmost parent (need to recreate index)", RelationGetRelationName(rel));
	/* caller should take care and avoid this */
	if (P_RIGHTMOST(copaque[0]))
		elog(ERROR, "bt_fixtlevel[%s]: invalid start child (need to recreate index)", RelationGetRelationName(rel));

	for (;;)
	{
		/*
		 * Read up to 2 more child pages and look for pointers to them in
		 * *saved* parent page
		 */
		coff[1] = coff[2] = InvalidOffsetNumber;
		for (cidx = 0; cidx < 2;)
		{
			cidx++;
			cblkno[cidx] = (copaque[cidx - 1])->btpo_next;
			cbuf[cidx] = _bt_getbuf(rel, cblkno[cidx], BT_READ);
			cpage[cidx] = BufferGetPage(cbuf[cidx]);
			copaque[cidx] = (BTPageOpaque) PageGetSpecialPointer(cpage[cidx]);
			coff[cidx] = _bt_getoff(page, cblkno[cidx]);

			/* sanity check */
			if (coff[cidx] != InvalidOffsetNumber)
			{
				for (i = cidx - 1; i >= 0; i--)
				{
					if (coff[i] == InvalidOffsetNumber)
						continue;
					if (coff[cidx] != coff[i] + 1)
						elog(ERROR, "bt_fixlevel[%s]: invalid item order(1) (need to recreate index)", RelationGetRelationName(rel));
					break;
				}
			}

			if (P_RIGHTMOST(copaque[cidx]))
				break;
		}

		/*
		 * Read parent page and insert missed pointers.
		 */
		if (coff[1] == InvalidOffsetNumber ||
			(cidx == 2 && coff[2] == InvalidOffsetNumber))
		{
			Buffer		newbuf;
			Page		newpage;
			BTPageOpaque newopaque;
			BTItem		ritem;
			Size		itemsz;
			OffsetNumber newitemoff;
			BlockNumber parblk[3];
			BTStackData stack;

			stack.bts_parent = NULL;
			stack.bts_blkno = blkno;
			stack.bts_offset = InvalidOffsetNumber;
			ItemPointerSet(&(stack.bts_btitem.bti_itup.t_tid),
						   cblkno[0], P_HIKEY);

			buf = _bt_getstackbuf(rel, &stack, BT_WRITE);
			if (buf == InvalidBuffer)
				elog(ERROR, "bt_fixlevel[%s]: pointer disappeared (need to recreate index)", RelationGetRelationName(rel));

			page = BufferGetPage(buf);
			opaque = (BTPageOpaque) PageGetSpecialPointer(page);
			coff[0] = stack.bts_offset;
			blkno = BufferGetBlockNumber(buf);
			parblk[0] = blkno;

			/* Check/insert missed pointers */
			for (i = 1; i <= cidx; i++)
			{
				coff[i] = _bt_getoff(page, cblkno[i]);

				/* sanity check */
				parblk[i] = BufferGetBlockNumber(buf);
				if (coff[i] != InvalidOffsetNumber)
				{
					if (parblk[i] == parblk[i - 1] &&
						coff[i] != coff[i - 1] + 1)
						elog(ERROR, "bt_fixlevel[%s]: invalid item order(2) (need to recreate index)", RelationGetRelationName(rel));
					continue;
				}
				/* Have to check next page ? */
				if ((!P_RIGHTMOST(opaque)) &&
					coff[i - 1] == PageGetMaxOffsetNumber(page))		/* yes */
				{
					newbuf = _bt_getbuf(rel, opaque->btpo_next, BT_WRITE);
					newpage = BufferGetPage(newbuf);
					newopaque = (BTPageOpaque) PageGetSpecialPointer(newpage);
					coff[i] = _bt_getoff(newpage, cblkno[i]);
					if (coff[i] != InvalidOffsetNumber) /* found ! */
					{
						if (coff[i] != P_FIRSTDATAKEY(newopaque))
							elog(ERROR, "bt_fixlevel[%s]: invalid item order(3) (need to recreate index)", RelationGetRelationName(rel));
						_bt_relbuf(rel, buf);
						buf = newbuf;
						page = newpage;
						opaque = newopaque;
						blkno = BufferGetBlockNumber(buf);
						parblk[i] = blkno;
						continue;
					}
					/* unfound - need to insert on current page */
					_bt_relbuf(rel, newbuf);
				}
				/* insert pointer */
				ritem = (BTItem) PageGetItem(cpage[i - 1],
								   PageGetItemId(cpage[i - 1], P_HIKEY));
				btitem = _bt_formitem(&(ritem->bti_itup));
				ItemPointerSet(&(btitem->bti_itup.t_tid), cblkno[i], P_HIKEY);
				itemsz = IndexTupleDSize(btitem->bti_itup)
					+ (sizeof(BTItemData) - sizeof(IndexTupleData));
				itemsz = MAXALIGN(itemsz);

				newitemoff = coff[i - 1] + 1;

				if (PageGetFreeSpace(page) < itemsz)
				{
					OffsetNumber firstright;
					OffsetNumber itup_off;
					BlockNumber itup_blkno;
					bool		newitemonleft;

					firstright = _bt_findsplitloc(rel, page,
									 newitemoff, itemsz, &newitemonleft);
					newbuf = _bt_split(rel, buf, firstright,
							   newitemoff, itemsz, btitem, newitemonleft,
									   &itup_off, &itup_blkno);
					/* what buffer we need in ? */
					if (newitemonleft)
						_bt_relbuf(rel, newbuf);
					else
					{
						_bt_relbuf(rel, buf);
						buf = newbuf;
						page = BufferGetPage(buf);
						opaque = (BTPageOpaque) PageGetSpecialPointer(page);
					}
					blkno = BufferGetBlockNumber(buf);
					coff[i] = itup_off;
				}
				else
				{
					_bt_insertuple(rel, buf, itemsz, btitem, newitemoff);
					coff[i] = newitemoff;
				}

				pfree(btitem);
				parblk[i] = blkno;
			}

			/* copy page with pointer to cblkno[cidx] to temp storage */
			memmove(tbuf, page, PageGetPageSize(page));
			_bt_relbuf(rel, buf);
			page = (Page) tbuf;
			opaque = (BTPageOpaque) PageGetSpecialPointer(page);
		}

		/* Continue if current check/fix level page is rightmost */
		if (P_RIGHTMOST(opaque))
			goodbye = false;

		/* Pointers to child pages are Ok - right end of child level ? */
		_bt_relbuf(rel, cbuf[0]);
		_bt_relbuf(rel, cbuf[1]);
		if (cidx == 1 ||
			(cidx == 2 && (P_RIGHTMOST(copaque[2]) || goodbye)))
		{
			if (cidx == 2)
				_bt_relbuf(rel, cbuf[2]);
			return;
		}
		if (cblkno[0] == limit || cblkno[1] == limit)
			goodbye = true;
		cblkno[0] = cblkno[2];
		cbuf[0] = cbuf[2];
		cpage[0] = cpage[2];
		copaque[0] = copaque[2];
		coff[0] = coff[2];
	}
}

/*
 * Check/fix part of tree - branch - up from parent of level with blocks
 * lblkno and rblknum. We first ensure that parent level has pointers
 * to both lblkno & rblknum and if those pointers are on different
 * parent pages then do the same for parent level, etc. No locks must
 * be held on target level and upper on entry. No locks will be held
 * on exit. Stack created when traversing tree down should be provided and
 * it must points to parent level. rblkno must be on the right from lblkno.
 * (This function is special edition of more expensive _bt_fixtree(),
 * but it doesn't guarantee full consistency of tree.)
 */
static void
_bt_fixbranch(Relation rel, BlockNumber lblkno,
			  BlockNumber rblkno, BTStack true_stack)
{
	BlockNumber blkno = true_stack->bts_blkno;
	BTStackData stack;
	BTPageOpaque opaque;
	Buffer		buf,
				rbuf;
	Page		page;
	OffsetNumber offnum;

	true_stack = true_stack->bts_parent;
	for (;;)
	{
		buf = _bt_getbuf(rel, blkno, BT_READ);

		/* Check/fix parent level pointed by blkno */
		_bt_fixlevel(rel, buf, rblkno);

		/*
		 * Here parent level should have pointers for both lblkno and
		 * rblkno and we have to find them.
		 */
		stack.bts_parent = NULL;
		stack.bts_blkno = blkno;
		stack.bts_offset = InvalidOffsetNumber;
		ItemPointerSet(&(stack.bts_btitem.bti_itup.t_tid), lblkno, P_HIKEY);
		buf = _bt_getstackbuf(rel, &stack, BT_READ);
		if (buf == InvalidBuffer)
			elog(ERROR, "bt_fixbranch[%s]: left pointer unfound (need to recreate index)", RelationGetRelationName(rel));
		page = BufferGetPage(buf);
		offnum = _bt_getoff(page, rblkno);

		if (offnum != InvalidOffsetNumber)		/* right pointer found */
		{
			if (offnum <= stack.bts_offset)
				elog(ERROR, "bt_fixbranch[%s]: invalid item order (need to recreate index)", RelationGetRelationName(rel));
			_bt_relbuf(rel, buf);
			return;
		}

		/* Pointers are on different parent pages - find right one */
		lblkno = BufferGetBlockNumber(buf);
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);
		if (P_RIGHTMOST(opaque))
			elog(ERROR, "bt_fixbranch[%s]: right pointer unfound(1) (need to recreate index)", RelationGetRelationName(rel));

		stack.bts_parent = NULL;
		stack.bts_blkno = opaque->btpo_next;
		stack.bts_offset = InvalidOffsetNumber;
		ItemPointerSet(&(stack.bts_btitem.bti_itup.t_tid), rblkno, P_HIKEY);
		rbuf = _bt_getstackbuf(rel, &stack, BT_READ);
		if (rbuf == InvalidBuffer)
			elog(ERROR, "bt_fixbranch[%s]: right pointer unfound(2) (need to recreate index)", RelationGetRelationName(rel));
		rblkno = BufferGetBlockNumber(rbuf);
		_bt_relbuf(rel, rbuf);

		/*
		 * If we have parent item in true_stack then go up one level and
		 * ensure that it has pointers to new lblkno & rblkno.
		 */
		if (true_stack)
		{
			_bt_relbuf(rel, buf);
			blkno = true_stack->bts_blkno;
			true_stack = true_stack->bts_parent;
			continue;
		}

		/*
		 * Well, we are on the level that was root or unexistent when we
		 * started traversing tree down. If btpo_parent is updated then
		 * we'll use it to continue, else we'll fix/restore upper levels
		 * entirely.
		 */
		if (!BTreeInvalidParent(opaque))
		{
			blkno = opaque->btpo_parent;
			_bt_relbuf(rel, buf);
			continue;
		}

		/* Have to switch to excl buf lock and re-check btpo_parent */
		_bt_relbuf(rel, buf);
		buf = _bt_getbuf(rel, blkno, BT_WRITE);
		page = BufferGetPage(buf);
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);
		if (!BTreeInvalidParent(opaque))
		{
			blkno = opaque->btpo_parent;
			_bt_relbuf(rel, buf);
			continue;
		}

		/*
		 * We hold excl lock on some internal page with unupdated
		 * btpo_parent - time for _bt_fixup.
		 */
		break;
	}

	elog(WARNING, "bt_fixbranch[%s]: fixing upper levels", RelationGetRelationName(rel));
	_bt_fixup(rel, buf);

	return;
}

/*
 * Having buf excl locked this routine walks to the left on level and
 * uses either _bt_fixtree() or _bt_fixroot() to create/check&fix upper
 * levels. No buffer pins/locks will be held on exit.
 */
static void
_bt_fixup(Relation rel, Buffer buf)
{
	Page		page;
	BTPageOpaque opaque;
	BlockNumber blkno;

	for (;;)
	{
		page = BufferGetPage(buf);
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);

		/*
		 * If someone else already created parent pages then it's time for
		 * _bt_fixtree() to check upper levels and fix them, if required.
		 */
		if (!BTreeInvalidParent(opaque))
		{
			blkno = opaque->btpo_parent;
			_bt_relbuf(rel, buf);
			elog(WARNING, "bt_fixup[%s]: checking/fixing upper levels", RelationGetRelationName(rel));
			_bt_fixtree(rel, blkno);
			return;
		}
		if (P_LEFTMOST(opaque))
			break;
		blkno = opaque->btpo_prev;
		_bt_relbuf(rel, buf);
		buf = _bt_getbuf(rel, blkno, BT_WRITE);
	}

	/*
	 * Ok, we are on the leftmost page, it's write locked by us and its
	 * btpo_parent points to meta page - time for _bt_fixroot().
	 */
	elog(WARNING, "bt_fixup[%s]: fixing root page", RelationGetRelationName(rel));
	buf = _bt_fixroot(rel, buf, true);
	_bt_relbuf(rel, buf);
}

static OffsetNumber
_bt_getoff(Page page, BlockNumber blkno)
{
	BTPageOpaque opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	OffsetNumber offnum = P_FIRSTDATAKEY(opaque);
	BlockNumber curblkno;
	ItemId		itemid;
	BTItem		item;

	for (; offnum <= maxoff; offnum++)
	{
		itemid = PageGetItemId(page, offnum);
		item = (BTItem) PageGetItem(page, itemid);
		curblkno = ItemPointerGetBlockNumber(&(item->bti_itup.t_tid));
		if (curblkno == blkno)
			return (offnum);
	}

	return (InvalidOffsetNumber);
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
	BTItemData	truncitem;

	if (!P_ISLEAF(opaque) && itup_off == P_FIRSTDATAKEY(opaque))
	{
		memcpy(&truncitem, btitem, sizeof(BTItemData));
		truncitem.bti_itup.t_info = sizeof(BTItemData);
		btitem = &truncitem;
		itemsize = sizeof(BTItemData);
	}

	if (PageAddItem(page, (Item) btitem, itemsize, itup_off,
					LP_USED) == InvalidOffsetNumber)
		elog(PANIC, "btree: failed to add item to the %s for %s",
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
