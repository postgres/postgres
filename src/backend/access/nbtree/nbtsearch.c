/*-------------------------------------------------------------------------
 *
 * btsearch.c
 *	  search code for postgres btrees.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/nbtree/nbtsearch.c,v 1.42 1999/03/28 20:31:58 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <access/genam.h>
#include <fmgr.h>
#include <storage/bufpage.h>
#include <storage/bufmgr.h>
#include <access/nbtree.h>
#include <catalog/pg_proc.h>

#ifndef HAVE_MEMMOVE
#include <regex/utils.h>
#else
#include <string.h>
#endif


static BTStack _bt_searchr(Relation rel, int keysz, ScanKey scankey,
			Buffer *bufP, BTStack stack_in);
static OffsetNumber _bt_firsteq(Relation rel, TupleDesc itupdesc, Page page,
			Size keysz, ScanKey scankey, OffsetNumber offnum);
static int _bt_compare(Relation rel, TupleDesc itupdesc, Page page,
			int keysz, ScanKey scankey, OffsetNumber offnum);
static bool
			_bt_twostep(IndexScanDesc scan, Buffer *bufP, ScanDirection dir);
static RetrieveIndexResult
			_bt_endpoint(IndexScanDesc scan, ScanDirection dir);

/*
 *	_bt_search() -- Search for a scan key in the index.
 *
 *		This routine is actually just a helper that sets things up and
 *		calls a recursive-descent search routine on the tree.
 */
BTStack
_bt_search(Relation rel, int keysz, ScanKey scankey, Buffer *bufP)
{
	*bufP = _bt_getroot(rel, BT_READ);
	return _bt_searchr(rel, keysz, scankey, bufP, (BTStack) NULL);
}

/*
 *	_bt_searchr() -- Search the tree recursively for a particular scankey.
 */
static BTStack
_bt_searchr(Relation rel,
			int keysz,
			ScanKey scankey,
			Buffer *bufP,
			BTStack stack_in)
{
	BTStack		stack;
	OffsetNumber offnum;
	Page		page;
	BTPageOpaque opaque;
	BlockNumber par_blkno;
	BlockNumber blkno;
	ItemId		itemid;
	BTItem		btitem;
	BTItem		item_save;
	int			item_nbytes;
	IndexTuple	itup;

	/* if this is a leaf page, we're done */
	page = BufferGetPage(*bufP);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	if (opaque->btpo_flags & BTP_LEAF)
		return stack_in;

	/*
	 * Find the appropriate item on the internal page, and get the child
	 * page that it points to.
	 */

	par_blkno = BufferGetBlockNumber(*bufP);
	offnum = _bt_binsrch(rel, *bufP, keysz, scankey, BT_DESCENT);
	itemid = PageGetItemId(page, offnum);
	btitem = (BTItem) PageGetItem(page, itemid);
	itup = &(btitem->bti_itup);
	blkno = ItemPointerGetBlockNumber(&(itup->t_tid));

	/*
	 * We need to save the bit image of the index entry we chose in the
	 * parent page on a stack.	In case we split the tree, we'll use this
	 * bit image to figure out what our real parent page is, in case the
	 * parent splits while we're working lower in the tree.  See the paper
	 * by Lehman and Yao for how this is detected and handled.	(We use
	 * unique OIDs to disambiguate duplicate keys in the index -- Lehman
	 * and Yao disallow duplicate keys).
	 */

	item_nbytes = ItemIdGetLength(itemid);
	item_save = (BTItem) palloc(item_nbytes);
	memmove((char *) item_save, (char *) btitem, item_nbytes);
	stack = (BTStack) palloc(sizeof(BTStackData));
	stack->bts_blkno = par_blkno;
	stack->bts_offset = offnum;
	stack->bts_btitem = item_save;
	stack->bts_parent = stack_in;

	/* drop the read lock on the parent page and acquire one on the child */
	_bt_relbuf(rel, *bufP, BT_READ);
	*bufP = _bt_getbuf(rel, blkno, BT_READ);

	/*
	 * Race -- the page we just grabbed may have split since we read its
	 * pointer in the parent.  If it has, we may need to move right to its
	 * new sibling.  Do that.
	 */

	*bufP = _bt_moveright(rel, *bufP, keysz, scankey, BT_READ);

	/* okay, all set to move down a level */
	return _bt_searchr(rel, keysz, scankey, bufP, stack);
}

/*
 *	_bt_moveright() -- move right in the btree if necessary.
 *
 *		When we drop and reacquire a pointer to a page, it is possible that
 *		the page has changed in the meanwhile.	If this happens, we're
 *		guaranteed that the page has "split right" -- that is, that any
 *		data that appeared on the page originally is either on the page
 *		or strictly to the right of it.
 *
 *		This routine decides whether or not we need to move right in the
 *		tree by examining the high key entry on the page.  If that entry
 *		is strictly less than one we expect to be on the page, then our
 *		picture of the page is incorrect and we need to move right.
 *
 *		On entry, we have the buffer pinned and a lock of the proper type.
 *		If we move right, we release the buffer and lock and acquire the
 *		same on the right sibling.
 */
Buffer
_bt_moveright(Relation rel,
			  Buffer buf,
			  int keysz,
			  ScanKey scankey,
			  int access)
{
	Page		page;
	BTPageOpaque opaque;
	ItemId		hikey;
	BlockNumber rblkno;
	int			natts = rel->rd_rel->relnatts;

	page = BufferGetPage(buf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	/* if we're on a rightmost page, we don't need to move right */
	if (P_RIGHTMOST(opaque))
		return buf;

	/* by convention, item 0 on non-rightmost pages is the high key */
	hikey = PageGetItemId(page, P_HIKEY);

	/*
	 * If the scan key that brought us to this page is >= the high key
	 * stored on the page, then the page has split and we need to move
	 * right.
	 */

	if (_bt_skeycmp(rel, keysz, scankey, page, hikey,
					BTGreaterEqualStrategyNumber))
	{
		/* move right as long as we need to */
		do
		{
			OffsetNumber offmax = PageGetMaxOffsetNumber(page);

			/*
			 * If this page consists of all duplicate keys (hikey and
			 * first key on the page have the same value), then we don't
			 * need to step right.
			 *
			 * NOTE for multi-column indices: we may do scan using keys not
			 * for all attrs. But we handle duplicates using all attrs in
			 * _bt_insert/_bt_spool code. And so we've to compare scankey
			 * with _last_ item on this page to do not lose "good" tuples
			 * if number of attrs > keysize. Example: (2,0) - last items
			 * on this page, (2,1) - first item on next page (hikey), our
			 * scankey is x = 2. Scankey == (2,1) because of we compare
			 * first attrs only, but we shouldn't to move right of here. -
			 * vadim 04/15/97
			 *
			 * Also, if this page is not LEAF one (and # of attrs > keysize)
			 * then we can't move too. - vadim 10/22/97
			 */

			if (_bt_skeycmp(rel, keysz, scankey, page, hikey,
							BTEqualStrategyNumber))
			{
				if (opaque->btpo_flags & BTP_CHAIN)
				{
					Assert((opaque->btpo_flags & BTP_LEAF) || offmax > P_HIKEY);
					break;
				}
				if (offmax > P_HIKEY)
				{
					if (natts == keysz) /* sanity checks */
					{
						if (_bt_skeycmp(rel, keysz, scankey, page,
										PageGetItemId(page, P_FIRSTKEY),
										BTEqualStrategyNumber))
							elog(FATAL, "btree: BTP_CHAIN flag was expected in %s (access = %s)",
								 rel->rd_rel->relname.data, access ? "bt_write" : "bt_read");
						if (_bt_skeycmp(rel, keysz, scankey, page,
										PageGetItemId(page, offmax),
										BTEqualStrategyNumber))
							elog(FATAL, "btree: unexpected equal last item");
						if (_bt_skeycmp(rel, keysz, scankey, page,
										PageGetItemId(page, offmax),
										BTLessStrategyNumber))
							elog(FATAL, "btree: unexpected greater last item");
						/* move right */
					}
					else if (!(opaque->btpo_flags & BTP_LEAF))
						break;
					else if (_bt_skeycmp(rel, keysz, scankey, page,
										 PageGetItemId(page, offmax),
										 BTLessEqualStrategyNumber))
						break;
				}
			}

			/* step right one page */
			rblkno = opaque->btpo_next;
			_bt_relbuf(rel, buf, access);
			buf = _bt_getbuf(rel, rblkno, access);
			page = BufferGetPage(buf);
			opaque = (BTPageOpaque) PageGetSpecialPointer(page);
			hikey = PageGetItemId(page, P_HIKEY);

		} while (!P_RIGHTMOST(opaque)
				 && _bt_skeycmp(rel, keysz, scankey, page, hikey,
								BTGreaterEqualStrategyNumber));
	}
	return buf;
}

/*
 *	_bt_skeycmp() -- compare a scan key to a particular item on a page using
 *					 a requested strategy (<, <=, =, >=, >).
 *
 *		We ignore the unique OIDs stored in the btree item here.  Those
 *		numbers are intended for use internally only, in repositioning a
 *		scan after a page split.  They do not impose any meaningful ordering.
 *
 *		The comparison is A <op> B, where A is the scan key and B is the
 *		tuple pointed at by itemid on page.
 */
bool
_bt_skeycmp(Relation rel,
			Size keysz,
			ScanKey scankey,
			Page page,
			ItemId itemid,
			StrategyNumber strat)
{
	BTItem		item;
	IndexTuple	indexTuple;
	TupleDesc	tupDes;
	ScanKey		entry;
	int			i;
	Datum		attrDatum;
	Datum		keyDatum;
	bool		compare;
	bool		isNull;
	bool		useEqual = false;
	bool		keyNull;

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

	item = (BTItem) PageGetItem(page, itemid);
	indexTuple = &(item->bti_itup);

	tupDes = RelationGetDescr(rel);

	/* see if the comparison is true for all of the key attributes */
	for (i = 1; i <= keysz; i++)
	{

		entry = &scankey[i - 1];
		Assert(entry->sk_attno == i);
		attrDatum = index_getattr(indexTuple,
								  entry->sk_attno,
								  tupDes,
								  &isNull);
		keyDatum = entry->sk_argument;

		/* see comments about NULLs handling in btbuild */
		if (entry->sk_flags & SK_ISNULL)		/* key is NULL */
		{
			Assert(entry->sk_procedure == F_NULLVALUE);
			keyNull = true;
			if (isNull)
				compare = (strat == BTEqualStrategyNumber) ? true : false;
			else
				compare = (strat == BTGreaterStrategyNumber) ? true : false;
		}
		else if (isNull)		/* key is NOT_NULL and item is NULL */
		{
			keyNull = false;
			compare = (strat == BTLessStrategyNumber) ? true : false;
		}
		else
		{
			keyNull = false;
			compare = _bt_invokestrat(rel, i, strat, keyDatum, attrDatum);
		}

		if (compare)			/* true for one of ">, <, =" */
		{
			if (strat != BTEqualStrategyNumber)
				return true;
		}
		else
/* false for one of ">, <, =" */
		{
			if (strat == BTEqualStrategyNumber)
				return false;

			/*
			 * if original strat was "<=, >=" OR "<, >" but some
			 * attribute(s) left - need to test for Equality
			 */
			if (useEqual || i < keysz)
			{
				if (keyNull || isNull)
					compare = (keyNull && isNull) ? true : false;
				else
					compare = _bt_invokestrat(rel, i, BTEqualStrategyNumber,
											  keyDatum, attrDatum);
				if (compare)	/* key' and item' attributes are equal */
					continue;	/* - try to compare next attributes */
			}
			return false;
		}
	}

	return true;
}

/*
 *	_bt_binsrch() -- Do a binary search for a key on a particular page.
 *
 *		The scankey we get has the compare function stored in the procedure
 *		entry of each data struct.	We invoke this regproc to do the
 *		comparison for every key in the scankey.  _bt_binsrch() returns
 *		the OffsetNumber of the first matching key on the page, or the
 *		OffsetNumber at which the matching key would appear if it were
 *		on this page.
 *
 *		By the time this procedure is called, we're sure we're looking
 *		at the right page -- don't need to walk right.  _bt_binsrch() has
 *		no lock or refcount side effects on the buffer.
 */
OffsetNumber
_bt_binsrch(Relation rel,
			Buffer buf,
			int keysz,
			ScanKey scankey,
			int srchtype)
{
	TupleDesc	itupdesc;
	Page		page;
	BTPageOpaque opaque;
	OffsetNumber low,
				mid,
				high;
	int			natts = rel->rd_rel->relnatts;
	int			result;

	itupdesc = RelationGetDescr(rel);
	page = BufferGetPage(buf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	/* by convention, item 1 on any non-rightmost page is the high key */
	low = mid = P_RIGHTMOST(opaque) ? P_HIKEY : P_FIRSTKEY;

	high = PageGetMaxOffsetNumber(page);

	/*
	 * Since for non-rightmost pages, the first item on the page is the
	 * high key, there are two notions of emptiness.  One is if nothing
	 * appears on the page.  The other is if nothing but the high key
	 * does. The reason we test high <= low, rather than high == low, is
	 * that after vacuuming there may be nothing *but* the high key on a
	 * page. In that case, given the scheme above, low = 2 and high = 1.
	 */

	if (PageIsEmpty(page))
		return low;
	if ((!P_RIGHTMOST(opaque) && high <= low))
	{
		if (high < low ||
			(srchtype == BT_DESCENT && !(opaque->btpo_flags & BTP_LEAF)))
			return low;
		/* It's insertion and high == low == 2 */
		result = _bt_compare(rel, itupdesc, page, keysz, scankey, low);
		if (result > 0)
			return OffsetNumberNext(low);
		return low;
	}

	while ((high - low) > 1)
	{
		mid = low + ((high - low) / 2);
		result = _bt_compare(rel, itupdesc, page, keysz, scankey, mid);

		if (result > 0)
			low = mid;
		else if (result < 0)
			high = mid - 1;
		else
		{
			mid = _bt_firsteq(rel, itupdesc, page, keysz, scankey, mid);

			/*
			 * NOTE for multi-column indices: we may do scan using keys
			 * not for all attrs. But we handle duplicates using all attrs
			 * in _bt_insert/_bt_spool code. And so while searching on
			 * internal pages having number of attrs > keysize we want to
			 * point at the last item < the scankey, not at the first item
			 * = the scankey (!!!), and let _bt_moveright decide later
			 * whether to move right or not (see comments and example
			 * there). Note also that INSERTions are not affected by this
			 * code (natts == keysz).		   - vadim 04/15/97
			 */
			if (natts == keysz || opaque->btpo_flags & BTP_LEAF)
				return mid;
			low = P_RIGHTMOST(opaque) ? P_HIKEY : P_FIRSTKEY;
			if (mid == low)
				return mid;
			return OffsetNumberPrev(mid);
		}
	}

	/*
	 * We terminated because the endpoints got too close together.	There
	 * are two cases to take care of.
	 *
	 * For non-insertion searches on internal pages, we want to point at the
	 * last key <, or first key =, the scankey on the page.  This
	 * guarantees that we'll descend the tree correctly. (NOTE comments
	 * above for multi-column indices).
	 *
	 * For all other cases, we want to point at the first key >= the scankey
	 * on the page.  This guarantees that scans and insertions will happen
	 * correctly.
	 */

	if (!(opaque->btpo_flags & BTP_LEAF) && srchtype == BT_DESCENT)
	{							/* We want the last key <, or first key
								 * ==, the scan key. */
		result = _bt_compare(rel, itupdesc, page, keysz, scankey, high);

		if (result == 0)
		{
			mid = _bt_firsteq(rel, itupdesc, page, keysz, scankey, high);

			/*
			 * If natts > keysz we want last item < the scan key. See
			 * comments above for multi-column indices.
			 */
			if (natts == keysz)
				return mid;
			low = P_RIGHTMOST(opaque) ? P_HIKEY : P_FIRSTKEY;
			if (mid == low)
				return mid;
			return OffsetNumberPrev(mid);
		}
		else if (result > 0)
			return high;
		else
			return low;
	}
	else
/* we want the first key >= the scan key */
	{
		result = _bt_compare(rel, itupdesc, page, keysz, scankey, low);
		if (result <= 0)
			return low;
		else
		{
			if (low == high)
				return OffsetNumberNext(low);

			result = _bt_compare(rel, itupdesc, page, keysz, scankey, high);
			if (result <= 0)
				return high;
			else
				return OffsetNumberNext(high);
		}
	}
}

static OffsetNumber
_bt_firsteq(Relation rel,
			TupleDesc itupdesc,
			Page page,
			Size keysz,
			ScanKey scankey,
			OffsetNumber offnum)
{
	BTPageOpaque opaque;
	OffsetNumber limit;

	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	/* skip the high key, if any */
	limit = P_RIGHTMOST(opaque) ? P_HIKEY : P_FIRSTKEY;

	/* walk backwards looking for the first key in the chain of duplicates */
	while (offnum > limit
		   && _bt_compare(rel, itupdesc, page,
						  keysz, scankey, OffsetNumberPrev(offnum)) == 0)
		offnum = OffsetNumberPrev(offnum);

	return offnum;
}

/*
 *	_bt_compare() -- Compare scankey to a particular tuple on the page.
 *
 *		This routine returns:
 *			-1 if scankey < tuple at offnum;
 *			 0 if scankey == tuple at offnum;
 *			+1 if scankey > tuple at offnum.
 *
 *		-- Old comments:
 *		In order to avoid having to propagate changes up the tree any time
 *		a new minimal key is inserted, the leftmost entry on the leftmost
 *		page is less than all possible keys, by definition.
 *
 *		-- New ones:
 *		New insertion code (fix against updating _in_place_ if new minimal
 *		key has bigger size than old one) may delete P_HIKEY entry on the
 *		root page in order to insert new minimal key - and so this definition
 *		does not work properly in this case and breaks key' order on root
 *		page. BTW, this propagation occures only while page' splitting,
 *		but not "any time a new min key is inserted" (see _bt_insertonpg).
 *				- vadim 12/05/96
 */
static int
_bt_compare(Relation rel,
			TupleDesc itupdesc,
			Page page,
			int keysz,
			ScanKey scankey,
			OffsetNumber offnum)
{
	Datum		datum;
	BTItem		btitem;
	ItemId		itemid;
	IndexTuple	itup;
	BTPageOpaque opaque;
	ScanKey		entry;
	AttrNumber	attno;
	int			result;
	int			i;
	bool		null;

	/*
	 * If this is a leftmost internal page, and if our comparison is with
	 * the first key on the page, then the item at that position is by
	 * definition less than the scan key.
	 *
	 * - see new comments above...
	 */

	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	if (!(opaque->btpo_flags & BTP_LEAF)
		&& P_LEFTMOST(opaque)
		&& offnum == P_HIKEY)
	{
		itemid = PageGetItemId(page, offnum);

		/*
		 * we just have to believe that this will only be called with
		 * offnum == P_HIKEY when P_HIKEY is the OffsetNumber of the first
		 * actual data key (i.e., this is also a rightmost page).  there
		 * doesn't seem to be any code that implies that the leftmost page
		 * is normally missing a high key as well as the rightmost page.
		 * but that implies that this code path only applies to the root
		 * -- which seems unlikely..
		 *
		 * - see new comments above...
		 */
		if (!P_RIGHTMOST(opaque))
			elog(ERROR, "_bt_compare: invalid comparison to high key");

#ifdef NOT_USED

		/*
		 * We just have to belive that right answer will not break
		 * anything. I've checked code and all seems to be ok. See new
		 * comments above...
		 *
		 * -- Old comments If the item on the page is equal to the scankey,
		 * that's okay to admit.  We just can't claim that the first key
		 * on the page is greater than anything.
		 */

		if (_bt_skeycmp(rel, keysz, scankey, page, itemid,
						BTEqualStrategyNumber))
			return 0;
		return 1;
#endif
	}

	btitem = (BTItem) PageGetItem(page, PageGetItemId(page, offnum));
	itup = &(btitem->bti_itup);

	/*
	 * The scan key is set up with the attribute number associated with
	 * each term in the key.  It is important that, if the index is
	 * multi-key, the scan contain the first k key attributes, and that
	 * they be in order.  If you think about how multi-key ordering works,
	 * you'll understand why this is.
	 *
	 * We don't test for violation of this condition here.
	 */

	for (i = 1; i <= keysz; i++)
	{
		long		tmpres;

		entry = &scankey[i - 1];
		attno = entry->sk_attno;
		datum = index_getattr(itup, attno, itupdesc, &null);

		/* see comments about NULLs handling in btbuild */
		if (entry->sk_flags & SK_ISNULL)		/* key is NULL */
		{
			Assert(entry->sk_procedure == F_NULLVALUE);
			if (null)
				tmpres = (long) 0;		/* NULL "=" NULL */
			else
				tmpres = (long) 1;		/* NULL ">" NOT_NULL */
		}
		else if (null)			/* key is NOT_NULL and item is NULL */
		{
			tmpres = (long) -1; /* NOT_NULL "<" NULL */
		}
		else
			tmpres = (long) FMGR_PTR2(&entry->sk_func, entry->sk_argument, datum);
		result = tmpres;

		/* if the keys are unequal, return the difference */
		if (result != 0)
			return result;
	}

	/* by here, the keys are equal */
	return 0;
}

/*
 *	_bt_next() -- Get the next item in a scan.
 *
 *		On entry, we have a valid currentItemData in the scan, and a
 *		read lock on the page that contains that item.	We do not have
 *		the page pinned.  We return the next item in the scan.	On
 *		exit, we have the page containing the next item locked but not
 *		pinned.
 */
RetrieveIndexResult
_bt_next(IndexScanDesc scan, ScanDirection dir)
{
	Relation	rel;
	Buffer		buf;
	Page		page;
	OffsetNumber offnum;
	RetrieveIndexResult res;
	ItemPointer current;
	BTItem		btitem;
	IndexTuple	itup;
	BTScanOpaque so;
	Size		keysok;

	rel = scan->relation;
	so = (BTScanOpaque) scan->opaque;
	current = &(scan->currentItemData);

	Assert (BufferIsValid(so->btso_curbuf));

	/* we still have the buffer pinned and locked */
	buf = so->btso_curbuf;

	do
	{
		/* step one tuple in the appropriate direction */
		if (!_bt_step(scan, &buf, dir))
			return (RetrieveIndexResult) NULL;

		/* by here, current is the tuple we want to return */
		offnum = ItemPointerGetOffsetNumber(current);
		page = BufferGetPage(buf);
		btitem = (BTItem) PageGetItem(page, PageGetItemId(page, offnum));
		itup = &btitem->bti_itup;

		if (_bt_checkkeys(scan, itup, &keysok))
		{
			Assert(keysok == so->numberOfKeys);
			res = FormRetrieveIndexResult(current, &(itup->t_tid));

			/* remember which buffer we have pinned and locked */
			so->btso_curbuf = buf;
			return res;
		}

	} while (keysok >= so->numberOfFirstKeys);

	ItemPointerSetInvalid(current);
	so->btso_curbuf = InvalidBuffer;
	_bt_relbuf(rel, buf, BT_READ);

	return (RetrieveIndexResult) NULL;
}

/*
 *	_bt_first() -- Find the first item in a scan.
 *
 *		We need to be clever about the type of scan, the operation it's
 *		performing, and the tree ordering.	We return the RetrieveIndexResult
 *		of the first item in the tree that satisfies the qualification
 *		associated with the scan descriptor.  On exit, the page containing
 *		the current index tuple is read locked and pinned, and the scan's
 *		opaque data entry is updated to include the buffer.
 */
RetrieveIndexResult
_bt_first(IndexScanDesc scan, ScanDirection dir)
{
	Relation	rel;
	TupleDesc	itupdesc;
	Buffer		buf;
	Page		page;
	BTPageOpaque pop;
	BTStack		stack;
	OffsetNumber offnum,
				maxoff;
	bool		offGmax = false;
	BTItem		btitem;
	IndexTuple	itup;
	ItemPointer current;
	BlockNumber blkno;
	StrategyNumber strat;
	RetrieveIndexResult res;
	RegProcedure proc;
	int			result;
	BTScanOpaque so;
	ScanKeyData skdata;
	Size		keysok;

	rel = scan->relation;
	so = (BTScanOpaque) scan->opaque;

	/*
	 * Order the keys in the qualification and be sure that the scan
	 * exploits the tree order.
	 */
	so->numberOfFirstKeys = 0;	/* may be changed by _bt_orderkeys */
	so->qual_ok = 1;			/* may be changed by _bt_orderkeys */
	scan->scanFromEnd = false;
	if (so->numberOfKeys > 0)
	{
		_bt_orderkeys(rel, so);

		strat = _bt_getstrat(rel, 1, so->keyData[0].sk_procedure);

		/* NOTE: it assumes ForwardScanDirection */
		if (strat == BTLessStrategyNumber ||
			strat == BTLessEqualStrategyNumber)
			scan->scanFromEnd = true;
	}
	else
		scan->scanFromEnd = true;

	if (so->qual_ok == 0)
		return (RetrieveIndexResult) NULL;

	/* if we just need to walk down one edge of the tree, do that */
	if (scan->scanFromEnd)
		return _bt_endpoint(scan, dir);

	itupdesc = RelationGetDescr(rel);
	current = &(scan->currentItemData);

	/*
	 * Okay, we want something more complicated.  What we'll do is use the
	 * first item in the scan key passed in (which has been correctly
	 * ordered to take advantage of index ordering) to position ourselves
	 * at the right place in the scan.
	 */
	/* _bt_orderkeys disallows it, but it's place to add some code latter */
	if (so->keyData[0].sk_flags & SK_ISNULL)
	{
		elog(ERROR, "_bt_first: btree doesn't support is(not)null, yet");
		return (RetrieveIndexResult) NULL;
	}
	proc = index_getprocid(rel, 1, BTORDER_PROC);
	ScanKeyEntryInitialize(&skdata, so->keyData[0].sk_flags, 1, proc,
						   so->keyData[0].sk_argument);

	stack = _bt_search(rel, 1, &skdata, &buf);
	_bt_freestack(stack);

	blkno = BufferGetBlockNumber(buf);
	page = BufferGetPage(buf);

	/*
	 * This will happen if the tree we're searching is entirely empty, or
	 * if we're doing a search for a key that would appear on an entirely
	 * empty internal page.  In either case, there are no matching tuples
	 * in the index.
	 */

	if (PageIsEmpty(page))
	{
		ItemPointerSetInvalid(current);
		so->btso_curbuf = InvalidBuffer;
		_bt_relbuf(rel, buf, BT_READ);
		return (RetrieveIndexResult) NULL;
	}
	maxoff = PageGetMaxOffsetNumber(page);
	pop = (BTPageOpaque) PageGetSpecialPointer(page);

	/*
	 * Now _bt_moveright doesn't move from non-rightmost leaf page if
	 * scankey == hikey and there is only hikey there. It's good for
	 * insertion, but we need to do work for scan here. - vadim 05/27/97
	 */

	while (maxoff == P_HIKEY && !P_RIGHTMOST(pop) &&
		   _bt_skeycmp(rel, 1, &skdata, page,
					   PageGetItemId(page, P_HIKEY),
					   BTGreaterEqualStrategyNumber))
	{
		/* step right one page */
		blkno = pop->btpo_next;
		_bt_relbuf(rel, buf, BT_READ);
		buf = _bt_getbuf(rel, blkno, BT_READ);
		page = BufferGetPage(buf);
		if (PageIsEmpty(page))
		{
			ItemPointerSetInvalid(current);
			so->btso_curbuf = InvalidBuffer;
			_bt_relbuf(rel, buf, BT_READ);
			return (RetrieveIndexResult) NULL;
		}
		maxoff = PageGetMaxOffsetNumber(page);
		pop = (BTPageOpaque) PageGetSpecialPointer(page);
	}


	/* find the nearest match to the manufactured scan key on the page */
	offnum = _bt_binsrch(rel, buf, 1, &skdata, BT_DESCENT);

	if (offnum > maxoff)
	{
		offnum = maxoff;
		offGmax = true;
	}

	ItemPointerSet(current, blkno, offnum);

	/*
	 * Now find the right place to start the scan.	Result is the value
	 * we're looking for minus the value we're looking at in the index.
	 */

	result = _bt_compare(rel, itupdesc, page, 1, &skdata, offnum);

	/* it's yet other place to add some code latter for is(not)null */

	strat = _bt_getstrat(rel, 1, so->keyData[0].sk_procedure);

	switch (strat)
	{
		case BTLessStrategyNumber:
			if (result <= 0)
			{
				do
				{
					if (!_bt_twostep(scan, &buf, BackwardScanDirection))
						break;

					offnum = ItemPointerGetOffsetNumber(current);
					page = BufferGetPage(buf);
					result = _bt_compare(rel, itupdesc, page, 1, &skdata, offnum);
				} while (result <= 0);

				/* if this is true, the key we just looked at is gone */
				if (result > 0)
					_bt_twostep(scan, &buf, ForwardScanDirection);
			}
			break;

		case BTLessEqualStrategyNumber:
			if (result >= 0)
			{
				do
				{
					if (!_bt_twostep(scan, &buf, ForwardScanDirection))
						break;

					offnum = ItemPointerGetOffsetNumber(current);
					page = BufferGetPage(buf);
					result = _bt_compare(rel, itupdesc, page, 1, &skdata, offnum);
				} while (result >= 0);

				if (result < 0)
					_bt_twostep(scan, &buf, BackwardScanDirection);
			}
			break;

		case BTEqualStrategyNumber:
			if (result != 0)
			{
				_bt_relbuf(scan->relation, buf, BT_READ);
				so->btso_curbuf = InvalidBuffer;
				ItemPointerSetInvalid(&(scan->currentItemData));
				return (RetrieveIndexResult) NULL;
			}
			break;

		case BTGreaterEqualStrategyNumber:
			if (offGmax)
			{
				if (result < 0)
				{
					Assert(!P_RIGHTMOST(pop) && maxoff == P_HIKEY);
					if (!_bt_step(scan, &buf, ForwardScanDirection))
					{
						_bt_relbuf(scan->relation, buf, BT_READ);
						so->btso_curbuf = InvalidBuffer;
						ItemPointerSetInvalid(&(scan->currentItemData));
						return (RetrieveIndexResult) NULL;
					}
				}
				else if (result > 0)
				{				/* Just remember:  _bt_binsrch() returns
								 * the OffsetNumber of the first matching
								 * key on the page, or the OffsetNumber at
								 * which the matching key WOULD APPEAR IF
								 * IT WERE on this page. No key on this
								 * page, but offnum from _bt_binsrch()
								 * greater maxoff - have to move right. -
								 * vadim 12/06/96 */
					_bt_twostep(scan, &buf, ForwardScanDirection);
				}
			}
			else if (result < 0)
			{
				do
				{
					if (!_bt_twostep(scan, &buf, BackwardScanDirection))
						break;

					page = BufferGetPage(buf);
					offnum = ItemPointerGetOffsetNumber(current);
					result = _bt_compare(rel, itupdesc, page, 1, &skdata, offnum);
				} while (result < 0);

				if (result > 0)
					_bt_twostep(scan, &buf, ForwardScanDirection);
			}
			break;

		case BTGreaterStrategyNumber:
			/* offGmax helps as above */
			if (result >= 0 || offGmax)
			{
				do
				{
					if (!_bt_twostep(scan, &buf, ForwardScanDirection))
						break;

					offnum = ItemPointerGetOffsetNumber(current);
					page = BufferGetPage(buf);
					result = _bt_compare(rel, itupdesc, page, 1, &skdata, offnum);
				} while (result >= 0);
			}
			break;
	}

	/* okay, current item pointer for the scan is right */
	offnum = ItemPointerGetOffsetNumber(current);
	page = BufferGetPage(buf);
	btitem = (BTItem) PageGetItem(page, PageGetItemId(page, offnum));
	itup = &btitem->bti_itup;

	if (_bt_checkkeys(scan, itup, &keysok))
	{
		res = FormRetrieveIndexResult(current, &(itup->t_tid));

		/* remember which buffer we have pinned */
		so->btso_curbuf = buf;
	}
	else if (keysok >= so->numberOfFirstKeys)
	{
		so->btso_curbuf = buf;
		return _bt_next(scan, dir);
	}
	else
	{
		ItemPointerSetInvalid(current);
		so->btso_curbuf = InvalidBuffer;
		_bt_relbuf(rel, buf, BT_READ);
		res = (RetrieveIndexResult) NULL;
	}

	return res;
}

/*
 *	_bt_step() -- Step one item in the requested direction in a scan on
 *				  the tree.
 *
 *		If no adjacent record exists in the requested direction, return
 *		false.	Else, return true and set the currentItemData for the
 *		scan to the right thing.
 */
bool
_bt_step(IndexScanDesc scan, Buffer *bufP, ScanDirection dir)
{
	Page		page;
	BTPageOpaque opaque;
	OffsetNumber offnum,
				maxoff;
	OffsetNumber start;
	BlockNumber blkno;
	BlockNumber obknum;
	BTScanOpaque so;
	ItemPointer current;
	Relation	rel;

	rel = scan->relation;
	current = &(scan->currentItemData);
	/*
	 * Don't use ItemPointerGetOffsetNumber or you risk to get
	 * assertion due to ability of ip_posid to be equal 0.
	 */
	offnum = current->ip_posid;
	page = BufferGetPage(*bufP);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	so = (BTScanOpaque) scan->opaque;
	maxoff = PageGetMaxOffsetNumber(page);

	/* get the next tuple */
	if (ScanDirectionIsForward(dir))
	{
		if (!PageIsEmpty(page) && offnum < maxoff)
			offnum = OffsetNumberNext(offnum);
		else
		{

			/* if we're at end of scan, release the buffer and return */
			blkno = opaque->btpo_next;
			if (P_RIGHTMOST(opaque))
			{
				_bt_relbuf(rel, *bufP, BT_READ);
				ItemPointerSetInvalid(current);
				*bufP = so->btso_curbuf = InvalidBuffer;
				return false;
			}
			else
			{

				/* walk right to the next page with data */
				_bt_relbuf(rel, *bufP, BT_READ);
				for (;;)
				{
					*bufP = _bt_getbuf(rel, blkno, BT_READ);
					page = BufferGetPage(*bufP);
					opaque = (BTPageOpaque) PageGetSpecialPointer(page);
					maxoff = PageGetMaxOffsetNumber(page);
					start = P_RIGHTMOST(opaque) ? P_HIKEY : P_FIRSTKEY;

					if (!PageIsEmpty(page) && start <= maxoff)
						break;
					else
					{
						blkno = opaque->btpo_next;
						_bt_relbuf(rel, *bufP, BT_READ);
						if (blkno == P_NONE)
						{
							*bufP = so->btso_curbuf = InvalidBuffer;
							ItemPointerSetInvalid(current);
							return false;
						}
					}
				}
				offnum = start;
			}
		}
	}
	else if (ScanDirectionIsBackward(dir))
	{

		/* remember that high key is item zero on non-rightmost pages */
		start = P_RIGHTMOST(opaque) ? P_HIKEY : P_FIRSTKEY;

		if (offnum > start)
			offnum = OffsetNumberPrev(offnum);
		else
		{

			/* if we're at end of scan, release the buffer and return */
			blkno = opaque->btpo_prev;
			if (P_LEFTMOST(opaque))
			{
				_bt_relbuf(rel, *bufP, BT_READ);
				*bufP = so->btso_curbuf = InvalidBuffer;
				ItemPointerSetInvalid(current);
				return false;
			}
			else
			{

				obknum = BufferGetBlockNumber(*bufP);

				/* walk right to the next page with data */
				_bt_relbuf(rel, *bufP, BT_READ);
				for (;;)
				{
					*bufP = _bt_getbuf(rel, blkno, BT_READ);
					page = BufferGetPage(*bufP);
					opaque = (BTPageOpaque) PageGetSpecialPointer(page);
					maxoff = PageGetMaxOffsetNumber(page);

					/*
					 * If the adjacent page just split, then we may have
					 * the wrong block.  Handle this case.	Because pages
					 * only split right, we don't have to worry about this
					 * failing to terminate.
					 */

					while (opaque->btpo_next != obknum)
					{
						blkno = opaque->btpo_next;
						_bt_relbuf(rel, *bufP, BT_READ);
						*bufP = _bt_getbuf(rel, blkno, BT_READ);
						page = BufferGetPage(*bufP);
						opaque = (BTPageOpaque) PageGetSpecialPointer(page);
						maxoff = PageGetMaxOffsetNumber(page);
					}

					/* don't consider the high key */
					start = P_RIGHTMOST(opaque) ? P_HIKEY : P_FIRSTKEY;

					/* anything to look at here? */
					if (!PageIsEmpty(page) && maxoff >= start)
						break;
					else
					{
						blkno = opaque->btpo_prev;
						obknum = BufferGetBlockNumber(*bufP);
						_bt_relbuf(rel, *bufP, BT_READ);
						if (blkno == P_NONE)
						{
							*bufP = so->btso_curbuf = InvalidBuffer;
							ItemPointerSetInvalid(current);
							return false;
						}
					}
				}
				offnum = maxoff;/* XXX PageIsEmpty? */
			}
		}
	}
	blkno = BufferGetBlockNumber(*bufP);
	so->btso_curbuf = *bufP;
	ItemPointerSet(current, blkno, offnum);

	return true;
}

/*
 *	_bt_twostep() -- Move to an adjacent record in a scan on the tree,
 *					 if an adjacent record exists.
 *
 *		This is like _bt_step, except that if no adjacent record exists
 *		it restores us to where we were before trying the step.  This is
 *		only hairy when you cross page boundaries, since the page you cross
 *		from could have records inserted or deleted, or could even split.
 *		This is unlikely, but we try to handle it correctly here anyway.
 *
 *		This routine contains the only case in which our changes to Lehman
 *		and Yao's algorithm.
 *
 *		Like step, this routine leaves the scan's currentItemData in the
 *		proper state and acquires a lock and pin on *bufP.	If the twostep
 *		succeeded, we return true; otherwise, we return false.
 */
static bool
_bt_twostep(IndexScanDesc scan, Buffer *bufP, ScanDirection dir)
{
	Page		page;
	BTPageOpaque opaque;
	OffsetNumber offnum,
				maxoff;
	OffsetNumber start;
	ItemPointer current;
	ItemId		itemid;
	int			itemsz;
	BTItem		btitem;
	BTItem		svitem;
	BlockNumber blkno;

	blkno = BufferGetBlockNumber(*bufP);
	page = BufferGetPage(*bufP);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	maxoff = PageGetMaxOffsetNumber(page);
	current = &(scan->currentItemData);
	offnum = ItemPointerGetOffsetNumber(current);

	start = P_RIGHTMOST(opaque) ? P_HIKEY : P_FIRSTKEY;

	/* if we're safe, just do it */
	if (ScanDirectionIsForward(dir) && offnum < maxoff)
	{							/* XXX PageIsEmpty? */
		ItemPointerSet(current, blkno, OffsetNumberNext(offnum));
		return true;
	}
	else if (ScanDirectionIsBackward(dir) && offnum > start)
	{
		ItemPointerSet(current, blkno, OffsetNumberPrev(offnum));
		return true;
	}

	/* if we've hit end of scan we don't have to do any work */
	if (ScanDirectionIsForward(dir) && P_RIGHTMOST(opaque))
		return false;
	else if (ScanDirectionIsBackward(dir) && P_LEFTMOST(opaque))
		return false;

	/*
	 * Okay, it's off the page; let _bt_step() do the hard work, and we'll
	 * try to remember where we were.  This is not guaranteed to work;
	 * this is the only place in the code where concurrency can screw us
	 * up, and it's because we want to be able to move in two directions
	 * in the scan.
	 */

	itemid = PageGetItemId(page, offnum);
	itemsz = ItemIdGetLength(itemid);
	btitem = (BTItem) PageGetItem(page, itemid);
	svitem = (BTItem) palloc(itemsz);
	memmove((char *) svitem, (char *) btitem, itemsz);

	if (_bt_step(scan, bufP, dir))
	{
		pfree(svitem);
		return true;
	}

	/* try to find our place again */
	*bufP = _bt_getbuf(scan->relation, blkno, BT_READ);
	page = BufferGetPage(*bufP);
	maxoff = PageGetMaxOffsetNumber(page);

	while (offnum <= maxoff)
	{
		itemid = PageGetItemId(page, offnum);
		btitem = (BTItem) PageGetItem(page, itemid);
		if (BTItemSame(btitem, svitem))
		{
			pfree(svitem);
			ItemPointerSet(current, blkno, offnum);
			return false;
		}
	}

	/*
	 * XXX crash and burn -- can't find our place.  We can be a little
	 * smarter -- walk to the next page to the right, for example, since
	 * that's the only direction that splits happen in.  Deletions screw
	 * us up less often since they're only done by the vacuum daemon.
	 */

	elog(ERROR, "btree synchronization error:  concurrent update botched scan");

	return false;
}

/*
 *	_bt_endpoint() -- Find the first or last key in the index.
 */
static RetrieveIndexResult
_bt_endpoint(IndexScanDesc scan, ScanDirection dir)
{
	Relation	rel;
	Buffer		buf;
	Page		page;
	BTPageOpaque opaque;
	ItemPointer current;
	OffsetNumber offnum,
				maxoff;
	OffsetNumber start = 0;
	BlockNumber blkno;
	BTItem		btitem;
	IndexTuple	itup;
	BTScanOpaque so;
	RetrieveIndexResult res;
	Size		keysok;

	rel = scan->relation;
	current = &(scan->currentItemData);
	so = (BTScanOpaque) scan->opaque;

	buf = _bt_getroot(rel, BT_READ);
	blkno = BufferGetBlockNumber(buf);
	page = BufferGetPage(buf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	for (;;)
	{
		if (opaque->btpo_flags & BTP_LEAF)
			break;

		if (ScanDirectionIsForward(dir))
			offnum = P_RIGHTMOST(opaque) ? P_HIKEY : P_FIRSTKEY;
		else
			offnum = PageGetMaxOffsetNumber(page);

		btitem = (BTItem) PageGetItem(page, PageGetItemId(page, offnum));
		itup = &(btitem->bti_itup);

		blkno = ItemPointerGetBlockNumber(&(itup->t_tid));

		_bt_relbuf(rel, buf, BT_READ);
		buf = _bt_getbuf(rel, blkno, BT_READ);
		page = BufferGetPage(buf);
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);

		/*
		 * Race condition: If the child page we just stepped onto is in
		 * the process of being split, we need to make sure we're all the
		 * way at the right edge of the tree.  See the paper by Lehman and
		 * Yao.
		 */

		if (ScanDirectionIsBackward(dir) && !P_RIGHTMOST(opaque))
		{
			do
			{
				blkno = opaque->btpo_next;
				_bt_relbuf(rel, buf, BT_READ);
				buf = _bt_getbuf(rel, blkno, BT_READ);
				page = BufferGetPage(buf);
				opaque = (BTPageOpaque) PageGetSpecialPointer(page);
			} while (!P_RIGHTMOST(opaque));
		}
	}

	/* okay, we've got the {left,right}-most page in the tree */
	maxoff = PageGetMaxOffsetNumber(page);

	if (ScanDirectionIsForward(dir))
	{
		if (!P_LEFTMOST(opaque))/* non-leftmost page ? */
			elog(ERROR, "_bt_endpoint: leftmost page (%u) has not leftmost flag", blkno);
		start = P_RIGHTMOST(opaque) ? P_HIKEY : P_FIRSTKEY;

		/*
		 * I don't understand this stuff! It doesn't work for
		 * non-rightmost pages with only one element (P_HIKEY) which we
		 * have after deletion itups by vacuum (it's case of start >
		 * maxoff). Scanning in BackwardScanDirection is not
		 * understandable at all. Well - new stuff. - vadim 12/06/96
		 */
#ifdef NOT_USED
		if (PageIsEmpty(page) || start > maxoff)
		{
			ItemPointerSet(current, blkno, maxoff);
			if (!_bt_step(scan, &buf, BackwardScanDirection))
				return (RetrieveIndexResult) NULL;

			start = ItemPointerGetOffsetNumber(current);
			page = BufferGetPage(buf);
		}
#endif
		if (PageIsEmpty(page))
		{
			if (start != P_HIKEY)		/* non-rightmost page */
				elog(ERROR, "_bt_endpoint: non-rightmost page (%u) is empty", blkno);

			/*
			 * It's left- & right- most page - root page, - and it's
			 * empty...
			 */
			_bt_relbuf(rel, buf, BT_READ);
			ItemPointerSetInvalid(current);
			so->btso_curbuf = InvalidBuffer;
			return (RetrieveIndexResult) NULL;
		}
		if (start > maxoff)		/* start == 2 && maxoff == 1 */
		{
			ItemPointerSet(current, blkno, maxoff);
			if (!_bt_step(scan, &buf, ForwardScanDirection))
				return (RetrieveIndexResult) NULL;

			start = ItemPointerGetOffsetNumber(current);
			page = BufferGetPage(buf);
		}
		/* new stuff ends here */
		else
			ItemPointerSet(current, blkno, start);
	}
	else if (ScanDirectionIsBackward(dir))
	{

		/*
		 * I don't understand this stuff too! If RIGHT-most leaf page is
		 * empty why do scanning in ForwardScanDirection ??? Well - new
		 * stuff. - vadim 12/06/96
		 */
#ifdef NOT_USED
		if (PageIsEmpty(page))
		{
			ItemPointerSet(current, blkno, FirstOffsetNumber);
			if (!_bt_step(scan, &buf, ForwardScanDirection))
				return (RetrieveIndexResult) NULL;

			start = ItemPointerGetOffsetNumber(current);
			page = BufferGetPage(buf);
		}
#endif
		if (PageIsEmpty(page))
		{
			/* If it's leftmost page too - it's empty root page... */
			if (P_LEFTMOST(opaque))
			{
				_bt_relbuf(rel, buf, BT_READ);
				ItemPointerSetInvalid(current);
				so->btso_curbuf = InvalidBuffer;
				return (RetrieveIndexResult) NULL;
			}
			/* Go back ! */
			ItemPointerSet(current, blkno, FirstOffsetNumber);
			if (!_bt_step(scan, &buf, BackwardScanDirection))
				return (RetrieveIndexResult) NULL;

			start = ItemPointerGetOffsetNumber(current);
			page = BufferGetPage(buf);
		}
		/* new stuff ends here */
		else
		{
			start = PageGetMaxOffsetNumber(page);
			ItemPointerSet(current, blkno, start);
		}
	}
	else
		elog(ERROR, "Illegal scan direction %d", dir);

	btitem = (BTItem) PageGetItem(page, PageGetItemId(page, start));
	itup = &(btitem->bti_itup);

	/* see if we picked a winner */
	if (_bt_checkkeys(scan, itup, &keysok))
	{
		res = FormRetrieveIndexResult(current, &(itup->t_tid));

		/* remember which buffer we have pinned */
		so->btso_curbuf = buf;
	}
	else if (keysok >= so->numberOfFirstKeys)
	{
		so->btso_curbuf = buf;
		return _bt_next(scan, dir);
	}
	else
	{
		ItemPointerSetInvalid(current);
		so->btso_curbuf = InvalidBuffer;
		_bt_relbuf(rel, buf, BT_READ);
		res = (RetrieveIndexResult) NULL;
	}

	return res;
}
