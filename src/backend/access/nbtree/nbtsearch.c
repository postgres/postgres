/*-------------------------------------------------------------------------
 *
 * nbtsearch.c
 *	  Search code for postgres btrees.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/nbtree/nbtsearch.c,v 1.80 2003/08/08 21:41:27 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/nbtree.h"


static Buffer _bt_walk_left(Relation rel, Buffer buf);
static bool _bt_endpoint(IndexScanDesc scan, ScanDirection dir);


/*
 *	_bt_search() -- Search the tree for a particular scankey,
 *		or more precisely for the first leaf page it could be on.
 *
 * Return value is a stack of parent-page pointers.  *bufP is set to the
 * address of the leaf-page buffer, which is read-locked and pinned.
 * No locks are held on the parent pages, however!
 *
 * NOTE that the returned buffer is read-locked regardless of the access
 * parameter.  However, access = BT_WRITE will allow an empty root page
 * to be created and returned.	When access = BT_READ, an empty index
 * will result in *bufP being set to InvalidBuffer.
 */
BTStack
_bt_search(Relation rel, int keysz, ScanKey scankey,
		   Buffer *bufP, int access)
{
	BTStack		stack_in = NULL;

	/* Get the root page to start with */
	*bufP = _bt_getroot(rel, access);

	/* If index is empty and access = BT_READ, no root page is created. */
	if (!BufferIsValid(*bufP))
		return (BTStack) NULL;

	/* Loop iterates once per level descended in the tree */
	for (;;)
	{
		Page		page;
		BTPageOpaque opaque;
		OffsetNumber offnum;
		ItemId		itemid;
		BTItem		btitem;
		IndexTuple	itup;
		BlockNumber blkno;
		BlockNumber par_blkno;
		BTStack		new_stack;

		/*
		 * Race -- the page we just grabbed may have split since we read
		 * its pointer in the parent (or metapage).  If it has, we may
		 * need to move right to its new sibling.  Do that.
		 */
		*bufP = _bt_moveright(rel, *bufP, keysz, scankey, BT_READ);

		/* if this is a leaf page, we're done */
		page = BufferGetPage(*bufP);
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);
		if (P_ISLEAF(opaque))
			break;

		/*
		 * Find the appropriate item on the internal page, and get the
		 * child page that it points to.
		 */
		offnum = _bt_binsrch(rel, *bufP, keysz, scankey);
		itemid = PageGetItemId(page, offnum);
		btitem = (BTItem) PageGetItem(page, itemid);
		itup = &(btitem->bti_itup);
		blkno = ItemPointerGetBlockNumber(&(itup->t_tid));
		par_blkno = BufferGetBlockNumber(*bufP);

		/*
		 * We need to save the location of the index entry we chose in the
		 * parent page on a stack. In case we split the tree, we'll use
		 * the stack to work back up to the parent page.  We also save the
		 * actual downlink (TID) to uniquely identify the index entry, in
		 * case it moves right while we're working lower in the tree.  See
		 * the paper by Lehman and Yao for how this is detected and
		 * handled. (We use the child link to disambiguate duplicate keys
		 * in the index -- Lehman and Yao disallow duplicate keys.)
		 */
		new_stack = (BTStack) palloc(sizeof(BTStackData));
		new_stack->bts_blkno = par_blkno;
		new_stack->bts_offset = offnum;
		memcpy(&new_stack->bts_btitem, btitem, sizeof(BTItemData));
		new_stack->bts_parent = stack_in;

		/* drop the read lock on the parent page, acquire one on the child */
		_bt_relbuf(rel, *bufP);
		*bufP = _bt_getbuf(rel, blkno, BT_READ);

		/* okay, all set to move down a level */
		stack_in = new_stack;
	}

	return stack_in;
}

/*
 *	_bt_moveright() -- move right in the btree if necessary.
 *
 *		When we follow a pointer to reach a page, it is possible that
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
 *		same on the right sibling.	Return value is the buffer we stop at.
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

	page = BufferGetPage(buf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	/*
	 * If the scan key that brought us to this page is > the high key
	 * stored on the page, then the page has split and we need to move
	 * right.  (If the scan key is equal to the high key, we might or
	 * might not need to move right; have to scan the page first anyway.)
	 * It could even have split more than once, so scan as far as needed.
	 *
	 * We also have to move right if we followed a link that brought us to a
	 * dead page.
	 */
	while (!P_RIGHTMOST(opaque) &&
		   (P_IGNORE(opaque) ||
			_bt_compare(rel, keysz, scankey, page, P_HIKEY) > 0))
	{
		/* step right one page */
		BlockNumber rblkno = opaque->btpo_next;

		_bt_relbuf(rel, buf);
		buf = _bt_getbuf(rel, rblkno, access);
		page = BufferGetPage(buf);
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	}

	if (P_IGNORE(opaque))
		elog(ERROR, "fell off the end of \"%s\"",
			 RelationGetRelationName(rel));

	return buf;
}

/*
 *	_bt_binsrch() -- Do a binary search for a key on a particular page.
 *
 * The scankey we get has the compare function stored in the procedure
 * entry of each data struct.  We invoke this regproc to do the
 * comparison for every key in the scankey.
 *
 * On a leaf page, _bt_binsrch() returns the OffsetNumber of the first
 * key >= given scankey.  (NOTE: in particular, this means it is possible
 * to return a value 1 greater than the number of keys on the page,
 * if the scankey is > all keys on the page.)
 *
 * On an internal (non-leaf) page, _bt_binsrch() returns the OffsetNumber
 * of the last key < given scankey.  (Since _bt_compare treats the first
 * data key of such a page as minus infinity, there will be at least one
 * key < scankey, so the result always points at one of the keys on the
 * page.)  This key indicates the right place to descend to be sure we
 * find all leaf keys >= given scankey.
 *
 * This procedure is not responsible for walking right, it just examines
 * the given page.	_bt_binsrch() has no lock or refcount side effects
 * on the buffer.
 */
OffsetNumber
_bt_binsrch(Relation rel,
			Buffer buf,
			int keysz,
			ScanKey scankey)
{
	TupleDesc	itupdesc;
	Page		page;
	BTPageOpaque opaque;
	OffsetNumber low,
				high;
	int32		result;

	itupdesc = RelationGetDescr(rel);
	page = BufferGetPage(buf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	low = P_FIRSTDATAKEY(opaque);
	high = PageGetMaxOffsetNumber(page);

	/*
	 * If there are no keys on the page, return the first available slot.
	 * Note this covers two cases: the page is really empty (no keys), or
	 * it contains only a high key.  The latter case is possible after
	 * vacuuming.  This can never happen on an internal page, however,
	 * since they are never empty (an internal page must have children).
	 */
	if (high < low)
		return low;

	/*
	 * Binary search to find the first key on the page >= scan key. Loop
	 * invariant: all slots before 'low' are < scan key, all slots at or
	 * after 'high' are >= scan key.  We can fall out when high == low.
	 */
	high++;						/* establish the loop invariant for high */

	while (high > low)
	{
		OffsetNumber mid = low + ((high - low) / 2);

		/* We have low <= mid < high, so mid points at a real slot */

		result = _bt_compare(rel, keysz, scankey, page, mid);

		if (result > 0)
			low = mid + 1;
		else
			high = mid;
	}

	/*
	 * At this point we have high == low, but be careful: they could point
	 * past the last slot on the page.
	 *
	 * On a leaf page, we always return the first key >= scan key (which
	 * could be the last slot + 1).
	 */
	if (P_ISLEAF(opaque))
		return low;

	/*
	 * On a non-leaf page, return the last key < scan key. There must be
	 * one if _bt_compare() is playing by the rules.
	 */
	Assert(low > P_FIRSTDATAKEY(opaque));

	return OffsetNumberPrev(low);
}

/*----------
 *	_bt_compare() -- Compare scankey to a particular tuple on the page.
 *
 *	keysz: number of key conditions to be checked (might be less than the
 *	total length of the scan key!)
 *	page/offnum: location of btree item to be compared to.
 *
 *		This routine returns:
 *			<0 if scankey < tuple at offnum;
 *			 0 if scankey == tuple at offnum;
 *			>0 if scankey > tuple at offnum.
 *		NULLs in the keys are treated as sortable values.  Therefore
 *		"equality" does not necessarily mean that the item should be
 *		returned to the caller as a matching key!
 *
 * CRUCIAL NOTE: on a non-leaf page, the first data key is assumed to be
 * "minus infinity": this routine will always claim it is less than the
 * scankey.  The actual key value stored (if any, which there probably isn't)
 * does not matter.  This convention allows us to implement the Lehman and
 * Yao convention that the first down-link pointer is before the first key.
 * See backend/access/nbtree/README for details.
 *----------
 */
int32
_bt_compare(Relation rel,
			int keysz,
			ScanKey scankey,
			Page page,
			OffsetNumber offnum)
{
	TupleDesc	itupdesc = RelationGetDescr(rel);
	BTPageOpaque opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	BTItem		btitem;
	IndexTuple	itup;
	int			i;

	/*
	 * Force result ">" if target item is first data item on an internal
	 * page --- see NOTE above.
	 */
	if (!P_ISLEAF(opaque) && offnum == P_FIRSTDATAKEY(opaque))
		return 1;

	btitem = (BTItem) PageGetItem(page, PageGetItemId(page, offnum));
	itup = &(btitem->bti_itup);

	/*
	 * The scan key is set up with the attribute number associated with
	 * each term in the key.  It is important that, if the index is
	 * multi-key, the scan contain the first k key attributes, and that
	 * they be in order.  If you think about how multi-key ordering works,
	 * you'll understand why this is.
	 *
	 * We don't test for violation of this condition here, however.  The
	 * initial setup for the index scan had better have gotten it right
	 * (see _bt_first).
	 */

	for (i = 0; i < keysz; i++)
	{
		ScanKey		entry = &scankey[i];
		Datum		datum;
		bool		isNull;
		int32		result;

		datum = index_getattr(itup, entry->sk_attno, itupdesc, &isNull);

		/* see comments about NULLs handling in btbuild */
		if (entry->sk_flags & SK_ISNULL)		/* key is NULL */
		{
			if (isNull)
				result = 0;		/* NULL "=" NULL */
			else
				result = 1;		/* NULL ">" NOT_NULL */
		}
		else if (isNull)		/* key is NOT_NULL and item is NULL */
		{
			result = -1;		/* NOT_NULL "<" NULL */
		}
		else
		{
			result = DatumGetInt32(FunctionCall2(&entry->sk_func,
												 entry->sk_argument,
												 datum));
		}

		/* if the keys are unequal, return the difference */
		if (result != 0)
			return result;
	}

	/* if we get here, the keys are equal */
	return 0;
}

/*
 *	_bt_next() -- Get the next item in a scan.
 *
 *		On entry, we have a valid currentItemData in the scan, and a
 *		read lock and pin count on the page that contains that item.
 *		We return the next item in the scan, or false if no more.
 *		On successful exit, the page containing the new item is locked
 *		and pinned; on failure exit, no lock or pin is held.
 */
bool
_bt_next(IndexScanDesc scan, ScanDirection dir)
{
	Relation	rel;
	Buffer		buf;
	Page		page;
	OffsetNumber offnum;
	ItemPointer current;
	BTItem		btitem;
	IndexTuple	itup;
	BTScanOpaque so;
	bool		continuescan;

	rel = scan->indexRelation;
	so = (BTScanOpaque) scan->opaque;
	current = &(scan->currentItemData);

	/* we still have the buffer pinned and locked */
	buf = so->btso_curbuf;
	Assert(BufferIsValid(buf));

	do
	{
		/* step one tuple in the appropriate direction */
		if (!_bt_step(scan, &buf, dir))
			return false;

		/* current is the next candidate tuple to return */
		offnum = ItemPointerGetOffsetNumber(current);
		page = BufferGetPage(buf);
		btitem = (BTItem) PageGetItem(page, PageGetItemId(page, offnum));
		itup = &btitem->bti_itup;

		if (_bt_checkkeys(scan, itup, dir, &continuescan))
		{
			/* tuple passes all scan key conditions, so return it */
			scan->xs_ctup.t_self = itup->t_tid;
			return true;
		}

		/* This tuple doesn't pass, but there might be more that do */
	} while (continuescan);

	/* No more items, so close down the current-item info */
	ItemPointerSetInvalid(current);
	so->btso_curbuf = InvalidBuffer;
	_bt_relbuf(rel, buf);

	return false;
}

/*
 *	_bt_first() -- Find the first item in a scan.
 *
 *		We need to be clever about the type of scan, the operation it's
 *		performing, and the tree ordering.	We find the
 *		first item in the tree that satisfies the qualification
 *		associated with the scan descriptor.  On exit, the page containing
 *		the current index tuple is read locked and pinned, and the scan's
 *		opaque data entry is updated to include the buffer.
 */
bool
_bt_first(IndexScanDesc scan, ScanDirection dir)
{
	Relation	rel = scan->indexRelation;
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	Buffer		buf;
	Page		page;
	BTStack		stack;
	OffsetNumber offnum;
	BTItem		btitem;
	IndexTuple	itup;
	ItemPointer current;
	BlockNumber blkno;
	StrategyNumber strat;
	bool		res;
	int32		result;
	bool		scanFromEnd;
	bool		continuescan;
	ScanKey		scankeys = NULL;
	int			keysCount = 0;
	int		   *nKeyIs = NULL;
	int			i,
				j;
	StrategyNumber strat_total;

	/*
	 * Order the scan keys in our canonical fashion and eliminate any
	 * redundant keys.
	 */
	_bt_orderkeys(scan);

	/*
	 * Quit now if _bt_orderkeys() discovered that the scan keys can never
	 * be satisfied (eg, x == 1 AND x > 2).
	 */
	if (!so->qual_ok)
		return false;

	/*
	 * Examine the scan keys to discover where we need to start the scan.
	 */
	scanFromEnd = false;
	strat_total = BTEqualStrategyNumber;
	if (so->numberOfKeys > 0)
	{
		nKeyIs = (int *) palloc(so->numberOfKeys * sizeof(int));
		for (i = 0; i < so->numberOfKeys; i++)
		{
			AttrNumber	attno = so->keyData[i].sk_attno;

			/* ignore keys for already-determined attrs */
			if (attno <= keysCount)
				continue;
			/* if we didn't find a boundary for the preceding attr, quit */
			if (attno > keysCount + 1)
				break;
			strat = _bt_getstrat(rel, attno,
								 so->keyData[i].sk_procedure);

			/*
			 * Can we use this key as a starting boundary for this attr?
			 *
			 * We can use multiple keys if they look like, say, = >= = but we
			 * have to stop after accepting a > or < boundary.
			 */
			if (strat == strat_total ||
				strat == BTEqualStrategyNumber)
				nKeyIs[keysCount++] = i;
			else if (ScanDirectionIsBackward(dir) &&
					 (strat == BTLessStrategyNumber ||
					  strat == BTLessEqualStrategyNumber))
			{
				nKeyIs[keysCount++] = i;
				strat_total = strat;
				if (strat == BTLessStrategyNumber)
					break;
			}
			else if (ScanDirectionIsForward(dir) &&
					 (strat == BTGreaterStrategyNumber ||
					  strat == BTGreaterEqualStrategyNumber))
			{
				nKeyIs[keysCount++] = i;
				strat_total = strat;
				if (strat == BTGreaterStrategyNumber)
					break;
			}
		}
		if (keysCount == 0)
			scanFromEnd = true;
	}
	else
		scanFromEnd = true;

	/* if we just need to walk down one edge of the tree, do that */
	if (scanFromEnd)
	{
		if (nKeyIs)
			pfree(nKeyIs);
		return _bt_endpoint(scan, dir);
	}

	/*
	 * We want to start the scan somewhere within the index.  Set up a
	 * scankey we can use to search for the correct starting point.
	 */
	scankeys = (ScanKey) palloc(keysCount * sizeof(ScanKeyData));
	for (i = 0; i < keysCount; i++)
	{
		FmgrInfo   *procinfo;

		j = nKeyIs[i];

		/*
		 * _bt_orderkeys disallows it, but it's place to add some code
		 * later
		 */
		if (so->keyData[j].sk_flags & SK_ISNULL)
		{
			pfree(nKeyIs);
			pfree(scankeys);
			elog(ERROR, "btree doesn't support is(not)null, yet");
			return false;
		}
		procinfo = index_getprocinfo(rel, i + 1, BTORDER_PROC);
		ScanKeyEntryInitializeWithInfo(scankeys + i,
									   so->keyData[j].sk_flags,
									   i + 1,
									   procinfo,
									   CurrentMemoryContext,
									   so->keyData[j].sk_argument);
	}
	if (nKeyIs)
		pfree(nKeyIs);

	current = &(scan->currentItemData);

	/*
	 * Use the manufactured scan key to descend the tree and position
	 * ourselves on the target leaf page.
	 */
	stack = _bt_search(rel, keysCount, scankeys, &buf, BT_READ);

	/* don't need to keep the stack around... */
	_bt_freestack(stack);

	if (!BufferIsValid(buf))
	{
		/* Only get here if index is completely empty */
		ItemPointerSetInvalid(current);
		so->btso_curbuf = InvalidBuffer;
		pfree(scankeys);
		return false;
	}

	/* remember which buffer we have pinned */
	so->btso_curbuf = buf;
	blkno = BufferGetBlockNumber(buf);
	page = BufferGetPage(buf);

	/* position to the precise item on the page */
	offnum = _bt_binsrch(rel, buf, keysCount, scankeys);

	ItemPointerSet(current, blkno, offnum);

	/*
	 * At this point we are positioned at the first item >= scan key, or
	 * possibly at the end of a page on which all the existing items are
	 * less than the scan key and we know that everything on later pages
	 * is greater than or equal to scan key.
	 *
	 * We could step forward in the latter case, but that'd be a waste of
	 * time if we want to scan backwards.  So, it's now time to examine
	 * the scan strategy to find the exact place to start the scan.
	 *
	 * Note: if _bt_step fails (meaning we fell off the end of the index in
	 * one direction or the other), we either return false (no matches) or
	 * call _bt_endpoint() to set up a scan starting at that index
	 * endpoint, as appropriate for the desired scan type.
	 *
	 * it's yet other place to add some code later for is(not)null ...
	 */

	switch (strat_total)
	{
		case BTLessStrategyNumber:

			/*
			 * Back up one to arrive at last item < scankey
			 */
			if (!_bt_step(scan, &buf, BackwardScanDirection))
			{
				pfree(scankeys);
				return false;
			}
			break;

		case BTLessEqualStrategyNumber:

			/*
			 * We need to find the last item <= scankey, so step forward
			 * till we find one > scankey, then step back one.
			 */
			if (offnum > PageGetMaxOffsetNumber(page))
			{
				if (!_bt_step(scan, &buf, ForwardScanDirection))
				{
					pfree(scankeys);
					return _bt_endpoint(scan, dir);
				}
			}
			for (;;)
			{
				offnum = ItemPointerGetOffsetNumber(current);
				page = BufferGetPage(buf);
				result = _bt_compare(rel, keysCount, scankeys, page, offnum);
				if (result < 0)
					break;
				if (!_bt_step(scan, &buf, ForwardScanDirection))
				{
					pfree(scankeys);
					return _bt_endpoint(scan, dir);
				}
			}
			if (!_bt_step(scan, &buf, BackwardScanDirection))
			{
				pfree(scankeys);
				return false;
			}
			break;

		case BTEqualStrategyNumber:

			/*
			 * Make sure we are on the first equal item; might have to
			 * step forward if currently at end of page.
			 */
			if (offnum > PageGetMaxOffsetNumber(page))
			{
				if (!_bt_step(scan, &buf, ForwardScanDirection))
				{
					pfree(scankeys);
					return false;
				}
				offnum = ItemPointerGetOffsetNumber(current);
				page = BufferGetPage(buf);
			}
			result = _bt_compare(rel, keysCount, scankeys, page, offnum);
			if (result != 0)
				goto nomatches; /* no equal items! */

			/*
			 * If a backward scan was specified, need to start with last
			 * equal item not first one.
			 */
			if (ScanDirectionIsBackward(dir))
			{
				do
				{
					if (!_bt_step(scan, &buf, ForwardScanDirection))
					{
						pfree(scankeys);
						return _bt_endpoint(scan, dir);
					}
					offnum = ItemPointerGetOffsetNumber(current);
					page = BufferGetPage(buf);
					result = _bt_compare(rel, keysCount, scankeys, page, offnum);
				} while (result == 0);
				if (!_bt_step(scan, &buf, BackwardScanDirection))
					elog(ERROR, "equal items disappeared?");
			}
			break;

		case BTGreaterEqualStrategyNumber:

			/*
			 * We want the first item >= scankey, which is where we are...
			 * unless we're not anywhere at all...
			 */
			if (offnum > PageGetMaxOffsetNumber(page))
			{
				if (!_bt_step(scan, &buf, ForwardScanDirection))
				{
					pfree(scankeys);
					return false;
				}
			}
			break;

		case BTGreaterStrategyNumber:

			/*
			 * We want the first item > scankey, so make sure we are on an
			 * item and then step over any equal items.
			 */
			if (offnum > PageGetMaxOffsetNumber(page))
			{
				if (!_bt_step(scan, &buf, ForwardScanDirection))
				{
					pfree(scankeys);
					return false;
				}
				offnum = ItemPointerGetOffsetNumber(current);
				page = BufferGetPage(buf);
			}
			result = _bt_compare(rel, keysCount, scankeys, page, offnum);
			while (result == 0)
			{
				if (!_bt_step(scan, &buf, ForwardScanDirection))
				{
					pfree(scankeys);
					return false;
				}
				offnum = ItemPointerGetOffsetNumber(current);
				page = BufferGetPage(buf);
				result = _bt_compare(rel, keysCount, scankeys, page, offnum);
			}
			break;
	}

	/* okay, current item pointer for the scan is right */
	offnum = ItemPointerGetOffsetNumber(current);
	page = BufferGetPage(buf);
	btitem = (BTItem) PageGetItem(page, PageGetItemId(page, offnum));
	itup = &btitem->bti_itup;

	/* is the first item actually acceptable? */
	if (_bt_checkkeys(scan, itup, dir, &continuescan))
	{
		/* yes, return it */
		scan->xs_ctup.t_self = itup->t_tid;
		res = true;
	}
	else if (continuescan)
	{
		/* no, but there might be another one that is */
		res = _bt_next(scan, dir);
	}
	else
	{
		/* no tuples in the index match this scan key */
nomatches:
		ItemPointerSetInvalid(current);
		so->btso_curbuf = InvalidBuffer;
		_bt_relbuf(rel, buf);
		res = false;
	}

	pfree(scankeys);

	return res;
}

/*
 *	_bt_step() -- Step one item in the requested direction in a scan on
 *				  the tree.
 *
 *		*bufP is the current buffer (read-locked and pinned).  If we change
 *		pages, it's updated appropriately.
 *
 *		If successful, update scan's currentItemData and return true.
 *		If no adjacent record exists in the requested direction,
 *		release buffer pin/locks and return false.
 */
bool
_bt_step(IndexScanDesc scan, Buffer *bufP, ScanDirection dir)
{
	Relation	rel = scan->indexRelation;
	ItemPointer current = &(scan->currentItemData);
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	Page		page;
	BTPageOpaque opaque;
	OffsetNumber offnum,
				maxoff;
	BlockNumber blkno;

	/*
	 * Don't use ItemPointerGetOffsetNumber or you risk to get assertion
	 * due to ability of ip_posid to be equal 0.
	 */
	offnum = current->ip_posid;

	page = BufferGetPage(*bufP);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	maxoff = PageGetMaxOffsetNumber(page);

	if (ScanDirectionIsForward(dir))
	{
		if (!PageIsEmpty(page) && offnum < maxoff)
			offnum = OffsetNumberNext(offnum);
		else
		{
			/* Walk right to the next page with data */
			for (;;)
			{
				/* if we're at end of scan, release the buffer and return */
				if (P_RIGHTMOST(opaque))
				{
					_bt_relbuf(rel, *bufP);
					ItemPointerSetInvalid(current);
					*bufP = so->btso_curbuf = InvalidBuffer;
					return false;
				}
				/* step right one page */
				blkno = opaque->btpo_next;
				_bt_relbuf(rel, *bufP);
				*bufP = _bt_getbuf(rel, blkno, BT_READ);
				page = BufferGetPage(*bufP);
				opaque = (BTPageOpaque) PageGetSpecialPointer(page);
				if (!P_IGNORE(opaque))
				{
					maxoff = PageGetMaxOffsetNumber(page);
					/* done if it's not empty */
					offnum = P_FIRSTDATAKEY(opaque);
					if (!PageIsEmpty(page) && offnum <= maxoff)
						break;
				}
			}
		}
	}
	else
/* backwards scan */
	{
		if (offnum > P_FIRSTDATAKEY(opaque))
			offnum = OffsetNumberPrev(offnum);
		else
		{
			/*
			 * Walk left to the next page with data.  This is much more
			 * complex than the walk-right case because of the possibility
			 * that the page to our left splits while we are in flight to
			 * it, plus the possibility that the page we were on gets
			 * deleted after we leave it.  See nbtree/README for details.
			 */
			for (;;)
			{
				*bufP = _bt_walk_left(rel, *bufP);

				/* if we're at end of scan, return failure */
				if (*bufP == InvalidBuffer)
				{
					ItemPointerSetInvalid(current);
					so->btso_curbuf = InvalidBuffer;
					return false;
				}
				page = BufferGetPage(*bufP);
				opaque = (BTPageOpaque) PageGetSpecialPointer(page);

				/*
				 * Okay, we managed to move left to a non-deleted page.
				 * Done if it's not half-dead and not empty.  Else loop
				 * back and do it all again.
				 */
				if (!P_IGNORE(opaque))
				{
					maxoff = PageGetMaxOffsetNumber(page);
					offnum = maxoff;
					if (!PageIsEmpty(page) &&
						maxoff >= P_FIRSTDATAKEY(opaque))
						break;
				}
			}
		}
	}

	/* Update scan state */
	so->btso_curbuf = *bufP;
	blkno = BufferGetBlockNumber(*bufP);
	ItemPointerSet(current, blkno, offnum);

	return true;
}

/*
 * _bt_walk_left() -- step left one page, if possible
 *
 * The given buffer must be pinned and read-locked.  This will be dropped
 * before stepping left.  On return, we have pin and read lock on the
 * returned page, instead.
 *
 * Returns InvalidBuffer if there is no page to the left (no lock is held
 * in that case).
 *
 * When working on a non-leaf level, it is possible for the returned page
 * to be half-dead; the caller should check that condition and step left
 * again if it's important.
 */
static Buffer
_bt_walk_left(Relation rel, Buffer buf)
{
	Page		page;
	BTPageOpaque opaque;

	page = BufferGetPage(buf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	for (;;)
	{
		BlockNumber obknum;
		BlockNumber lblkno;
		BlockNumber blkno;
		int			tries;

		/* if we're at end of tree, release buf and return failure */
		if (P_LEFTMOST(opaque))
		{
			_bt_relbuf(rel, buf);
			break;
		}
		/* remember original page we are stepping left from */
		obknum = BufferGetBlockNumber(buf);
		/* step left */
		blkno = lblkno = opaque->btpo_prev;
		_bt_relbuf(rel, buf);
		buf = _bt_getbuf(rel, blkno, BT_READ);
		page = BufferGetPage(buf);
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);

		/*
		 * If this isn't the page we want, walk right till we find what we
		 * want --- but go no more than four hops (an arbitrary limit). If
		 * we don't find the correct page by then, the most likely bet is
		 * that the original page got deleted and isn't in the sibling
		 * chain at all anymore, not that its left sibling got split more
		 * than four times.
		 *
		 * Note that it is correct to test P_ISDELETED not P_IGNORE here,
		 * because half-dead pages are still in the sibling chain.	Caller
		 * must reject half-dead pages if wanted.
		 */
		tries = 0;
		for (;;)
		{
			if (!P_ISDELETED(opaque) && opaque->btpo_next == obknum)
			{
				/* Found desired page, return it */
				return buf;
			}
			if (P_RIGHTMOST(opaque) || ++tries > 4)
				break;
			blkno = opaque->btpo_next;
			_bt_relbuf(rel, buf);
			buf = _bt_getbuf(rel, blkno, BT_READ);
			page = BufferGetPage(buf);
			opaque = (BTPageOpaque) PageGetSpecialPointer(page);
		}

		/* Return to the original page to see what's up */
		_bt_relbuf(rel, buf);
		buf = _bt_getbuf(rel, obknum, BT_READ);
		page = BufferGetPage(buf);
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);
		if (P_ISDELETED(opaque))
		{
			/*
			 * It was deleted.	Move right to first nondeleted page (there
			 * must be one); that is the page that has acquired the
			 * deleted one's keyspace, so stepping left from it will take
			 * us where we want to be.
			 */
			for (;;)
			{
				if (P_RIGHTMOST(opaque))
					elog(ERROR, "fell off the end of \"%s\"",
						 RelationGetRelationName(rel));
				blkno = opaque->btpo_next;
				_bt_relbuf(rel, buf);
				buf = _bt_getbuf(rel, blkno, BT_READ);
				page = BufferGetPage(buf);
				opaque = (BTPageOpaque) PageGetSpecialPointer(page);
				if (!P_ISDELETED(opaque))
					break;
			}

			/*
			 * Now return to top of loop, resetting obknum to point to
			 * this nondeleted page, and try again.
			 */
		}
		else
		{
			/*
			 * It wasn't deleted; the explanation had better be that the
			 * page to the left got split or deleted. Without this check,
			 * we'd go into an infinite loop if there's anything wrong.
			 */
			if (opaque->btpo_prev == lblkno)
				elog(ERROR, "could not find left sibling in \"%s\"",
					 RelationGetRelationName(rel));
			/* Okay to try again with new lblkno value */
		}
	}

	return InvalidBuffer;
}

/*
 * _bt_get_endpoint() -- Find the first or last page on a given tree level
 *
 * If the index is empty, we will return InvalidBuffer; any other failure
 * condition causes ereport().	We will not return a dead page.
 *
 * The returned buffer is pinned and read-locked.
 */
Buffer
_bt_get_endpoint(Relation rel, uint32 level, bool rightmost)
{
	Buffer		buf;
	Page		page;
	BTPageOpaque opaque;
	OffsetNumber offnum;
	BlockNumber blkno;
	BTItem		btitem;
	IndexTuple	itup;

	/*
	 * If we are looking for a leaf page, okay to descend from fast root;
	 * otherwise better descend from true root.  (There is no point in
	 * being smarter about intermediate levels.)
	 */
	if (level == 0)
		buf = _bt_getroot(rel, BT_READ);
	else
		buf = _bt_gettrueroot(rel);

	if (!BufferIsValid(buf))
	{
		/* empty index... */
		return InvalidBuffer;
	}

	page = BufferGetPage(buf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	for (;;)
	{
		/*
		 * If we landed on a deleted page, step right to find a live page
		 * (there must be one).  Also, if we want the rightmost page, step
		 * right if needed to get to it (this could happen if the page
		 * split since we obtained a pointer to it).
		 */
		while (P_IGNORE(opaque) ||
			   (rightmost && !P_RIGHTMOST(opaque)))
		{
			blkno = opaque->btpo_next;
			if (blkno == P_NONE)
				elog(ERROR, "fell off the end of \"%s\"",
					 RelationGetRelationName(rel));
			_bt_relbuf(rel, buf);
			buf = _bt_getbuf(rel, blkno, BT_READ);
			page = BufferGetPage(buf);
			opaque = (BTPageOpaque) PageGetSpecialPointer(page);
		}

		/* Done? */
		if (opaque->btpo.level == level)
			break;
		if (opaque->btpo.level < level)
			elog(ERROR, "btree level %u not found", level);

		/* Descend to leftmost or rightmost child page */
		if (rightmost)
			offnum = PageGetMaxOffsetNumber(page);
		else
			offnum = P_FIRSTDATAKEY(opaque);

		btitem = (BTItem) PageGetItem(page, PageGetItemId(page, offnum));
		itup = &(btitem->bti_itup);
		blkno = ItemPointerGetBlockNumber(&(itup->t_tid));

		_bt_relbuf(rel, buf);
		buf = _bt_getbuf(rel, blkno, BT_READ);
		page = BufferGetPage(buf);
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	}

	return buf;
}

/*
 *	_bt_endpoint() -- Find the first or last key in the index.
 *
 * This is used by _bt_first() to set up a scan when we've determined
 * that the scan must start at the beginning or end of the index (for
 * a forward or backward scan respectively).
 */
static bool
_bt_endpoint(IndexScanDesc scan, ScanDirection dir)
{
	Relation	rel;
	Buffer		buf;
	Page		page;
	BTPageOpaque opaque;
	ItemPointer current;
	OffsetNumber maxoff;
	OffsetNumber start;
	BlockNumber blkno;
	BTItem		btitem;
	IndexTuple	itup;
	BTScanOpaque so;
	bool		res;
	bool		continuescan;

	rel = scan->indexRelation;
	current = &(scan->currentItemData);
	so = (BTScanOpaque) scan->opaque;

	/*
	 * Scan down to the leftmost or rightmost leaf page.  This is a
	 * simplified version of _bt_search().	We don't maintain a stack
	 * since we know we won't need it.
	 */
	buf = _bt_get_endpoint(rel, 0, ScanDirectionIsBackward(dir));

	if (!BufferIsValid(buf))
	{
		/* empty index... */
		ItemPointerSetInvalid(current);
		so->btso_curbuf = InvalidBuffer;
		return false;
	}

	blkno = BufferGetBlockNumber(buf);
	page = BufferGetPage(buf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	Assert(P_ISLEAF(opaque));

	maxoff = PageGetMaxOffsetNumber(page);

	if (ScanDirectionIsForward(dir))
	{
		/* There could be dead pages to the left, so not this: */
		/* Assert(P_LEFTMOST(opaque)); */

		start = P_FIRSTDATAKEY(opaque);
	}
	else if (ScanDirectionIsBackward(dir))
	{
		Assert(P_RIGHTMOST(opaque));

		start = PageGetMaxOffsetNumber(page);
		if (start < P_FIRSTDATAKEY(opaque))		/* watch out for empty
												 * page */
			start = P_FIRSTDATAKEY(opaque);
	}
	else
	{
		elog(ERROR, "invalid scan direction: %d", (int) dir);
		start = 0;				/* keep compiler quiet */
	}

	ItemPointerSet(current, blkno, start);
	/* remember which buffer we have pinned */
	so->btso_curbuf = buf;

	/*
	 * Left/rightmost page could be empty due to deletions, if so step
	 * till we find a nonempty page.
	 */
	if (start > maxoff)
	{
		if (!_bt_step(scan, &buf, dir))
			return false;
		start = ItemPointerGetOffsetNumber(current);
		page = BufferGetPage(buf);
	}

	btitem = (BTItem) PageGetItem(page, PageGetItemId(page, start));
	itup = &(btitem->bti_itup);

	/* see if we picked a winner */
	if (_bt_checkkeys(scan, itup, dir, &continuescan))
	{
		/* yes, return it */
		scan->xs_ctup.t_self = itup->t_tid;
		res = true;
	}
	else if (continuescan)
	{
		/* no, but there might be another one that is */
		res = _bt_next(scan, dir);
	}
	else
	{
		/* no tuples in the index match this scan key */
		ItemPointerSetInvalid(current);
		so->btso_curbuf = InvalidBuffer;
		_bt_relbuf(rel, buf);
		res = false;
	}

	return res;
}
