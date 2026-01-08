/*-------------------------------------------------------------------------
 *
 * nbtreadpage.c
 *	  Leaf page reading for btree index scans.
 *
 * NOTES
 *	  This file contains code to return items that satisfy the scan's
 *	  search-type scan keys within caller-supplied btree leaf page.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/nbtree/nbtreadpage.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/nbtree.h"
#include "access/relscan.h"
#include "storage/predicate.h"
#include "utils/datum.h"
#include "utils/rel.h"


/*
 * _bt_readpage state used across _bt_checkkeys calls for a page
 */
typedef struct BTReadPageState
{
	/* Input parameters, set by _bt_readpage for _bt_checkkeys */
	ScanDirection dir;			/* current scan direction */
	OffsetNumber minoff;		/* Lowest non-pivot tuple's offset */
	OffsetNumber maxoff;		/* Highest non-pivot tuple's offset */
	IndexTuple	finaltup;		/* Needed by scans with array keys */
	Page		page;			/* Page being read */
	bool		firstpage;		/* page is first for primitive scan? */
	bool		forcenonrequired;	/* treat all keys as nonrequired? */
	int			startikey;		/* start comparisons from this scan key */

	/* Per-tuple input parameters, set by _bt_readpage for _bt_checkkeys */
	OffsetNumber offnum;		/* current tuple's page offset number */

	/* Output parameters, set by _bt_checkkeys for _bt_readpage */
	OffsetNumber skip;			/* Array keys "look ahead" skip offnum */
	bool		continuescan;	/* Terminate ongoing (primitive) index scan? */

	/*
	 * Private _bt_checkkeys state used to manage "look ahead" optimization
	 * and primscan scheduling (only used during scans with array keys)
	 */
	int16		rechecks;
	int16		targetdistance;
	int16		nskipadvances;

} BTReadPageState;


static void _bt_set_startikey(IndexScanDesc scan, BTReadPageState *pstate);
static bool _bt_scanbehind_checkkeys(IndexScanDesc scan, ScanDirection dir,
									 IndexTuple finaltup);
static bool _bt_oppodir_checkkeys(IndexScanDesc scan, ScanDirection dir,
								  IndexTuple finaltup);
static void _bt_saveitem(BTScanOpaque so, int itemIndex,
						 OffsetNumber offnum, IndexTuple itup);
static int	_bt_setuppostingitems(BTScanOpaque so, int itemIndex,
								  OffsetNumber offnum, const ItemPointerData *heapTid,
								  IndexTuple itup);
static inline void _bt_savepostingitem(BTScanOpaque so, int itemIndex,
									   OffsetNumber offnum,
									   ItemPointer heapTid, int tupleOffset);
static bool _bt_checkkeys(IndexScanDesc scan, BTReadPageState *pstate, bool arrayKeys,
						  IndexTuple tuple, int tupnatts);
static bool _bt_check_compare(IndexScanDesc scan, ScanDirection dir,
							  IndexTuple tuple, int tupnatts, TupleDesc tupdesc,
							  bool advancenonrequired, bool forcenonrequired,
							  bool *continuescan, int *ikey);
static bool _bt_check_rowcompare(ScanKey header,
								 IndexTuple tuple, int tupnatts, TupleDesc tupdesc,
								 ScanDirection dir, bool forcenonrequired, bool *continuescan);
static bool _bt_rowcompare_cmpresult(ScanKey subkey, int cmpresult);
static bool _bt_tuple_before_array_skeys(IndexScanDesc scan, ScanDirection dir,
										 IndexTuple tuple, TupleDesc tupdesc, int tupnatts,
										 bool readpagetup, int sktrig, bool *scanBehind);
static void _bt_checkkeys_look_ahead(IndexScanDesc scan, BTReadPageState *pstate,
									 int tupnatts, TupleDesc tupdesc);
static bool _bt_advance_array_keys(IndexScanDesc scan, BTReadPageState *pstate,
								   IndexTuple tuple, int tupnatts, TupleDesc tupdesc,
								   int sktrig, bool sktrig_required);
static bool _bt_advance_array_keys_increment(IndexScanDesc scan, ScanDirection dir,
											 bool *skip_array_set);
static bool _bt_array_increment(Relation rel, ScanKey skey, BTArrayKeyInfo *array);
static bool _bt_array_decrement(Relation rel, ScanKey skey, BTArrayKeyInfo *array);
static void _bt_array_set_low_or_high(Relation rel, ScanKey skey,
									  BTArrayKeyInfo *array, bool low_not_high);
static void _bt_skiparray_set_element(Relation rel, ScanKey skey, BTArrayKeyInfo *array,
									  int32 set_elem_result, Datum tupdatum, bool tupnull);
static void _bt_skiparray_set_isnull(Relation rel, ScanKey skey, BTArrayKeyInfo *array);
static inline int32 _bt_compare_array_skey(FmgrInfo *orderproc,
										   Datum tupdatum, bool tupnull,
										   Datum arrdatum, ScanKey cur);
static void _bt_binsrch_skiparray_skey(bool cur_elem_trig, ScanDirection dir,
									   Datum tupdatum, bool tupnull,
									   BTArrayKeyInfo *array, ScanKey cur,
									   int32 *set_elem_result);
#ifdef USE_ASSERT_CHECKING
static bool _bt_verify_keys_with_arraykeys(IndexScanDesc scan);
#endif


/*
 *	_bt_readpage() -- Load data from current index page into so->currPos
 *
 * Caller must have pinned and read-locked so->currPos.buf; the buffer's state
 * is not changed here.  Also, currPos.moreLeft and moreRight must be valid;
 * they are updated as appropriate.  All other fields of so->currPos are
 * initialized from scratch here.
 *
 * We scan the current page starting at offnum and moving in the indicated
 * direction.  All items matching the scan keys are loaded into currPos.items.
 * moreLeft or moreRight (as appropriate) is cleared if _bt_checkkeys reports
 * that there can be no more matching tuples in the current scan direction
 * (could just be for the current primitive index scan when scan has arrays).
 *
 * In the case of a parallel scan, caller must have called _bt_parallel_seize
 * prior to calling this function; this function will invoke
 * _bt_parallel_release before returning.
 *
 * Returns true if any matching items found on the page, false if none.
 */
bool
_bt_readpage(IndexScanDesc scan, ScanDirection dir, OffsetNumber offnum,
			 bool firstpage)
{
	Relation	rel = scan->indexRelation;
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	Page		page;
	BTPageOpaque opaque;
	OffsetNumber minoff;
	OffsetNumber maxoff;
	BTReadPageState pstate;
	bool		arrayKeys,
				ignore_killed_tuples = scan->ignore_killed_tuples;
	int			itemIndex,
				indnatts;

	/* save the page/buffer block number, along with its sibling links */
	page = BufferGetPage(so->currPos.buf);
	opaque = BTPageGetOpaque(page);
	so->currPos.currPage = BufferGetBlockNumber(so->currPos.buf);
	so->currPos.prevPage = opaque->btpo_prev;
	so->currPos.nextPage = opaque->btpo_next;
	/* delay setting so->currPos.lsn until _bt_drop_lock_and_maybe_pin */
	pstate.dir = so->currPos.dir = dir;
	so->currPos.nextTupleOffset = 0;

	/* either moreRight or moreLeft should be set now (may be unset later) */
	Assert(ScanDirectionIsForward(dir) ? so->currPos.moreRight :
		   so->currPos.moreLeft);
	Assert(!P_IGNORE(opaque));
	Assert(BTScanPosIsPinned(so->currPos));
	Assert(!so->needPrimScan);

	/* initialize local variables */
	indnatts = IndexRelationGetNumberOfAttributes(rel);
	arrayKeys = so->numArrayKeys != 0;
	minoff = P_FIRSTDATAKEY(opaque);
	maxoff = PageGetMaxOffsetNumber(page);

	/* initialize page-level state that we'll pass to _bt_checkkeys */
	pstate.minoff = minoff;
	pstate.maxoff = maxoff;
	pstate.finaltup = NULL;
	pstate.page = page;
	pstate.firstpage = firstpage;
	pstate.forcenonrequired = false;
	pstate.startikey = 0;
	pstate.offnum = InvalidOffsetNumber;
	pstate.skip = InvalidOffsetNumber;
	pstate.continuescan = true; /* default assumption */
	pstate.rechecks = 0;
	pstate.targetdistance = 0;
	pstate.nskipadvances = 0;

	if (scan->parallel_scan)
	{
		/* allow next/prev page to be read by other worker without delay */
		if (ScanDirectionIsForward(dir))
			_bt_parallel_release(scan, so->currPos.nextPage,
								 so->currPos.currPage);
		else
			_bt_parallel_release(scan, so->currPos.prevPage,
								 so->currPos.currPage);
	}

	PredicateLockPage(rel, so->currPos.currPage, scan->xs_snapshot);

	if (ScanDirectionIsForward(dir))
	{
		/* SK_SEARCHARRAY forward scans must provide high key up front */
		if (arrayKeys)
		{
			if (!P_RIGHTMOST(opaque))
			{
				ItemId		iid = PageGetItemId(page, P_HIKEY);

				pstate.finaltup = (IndexTuple) PageGetItem(page, iid);

				if (unlikely(so->scanBehind) &&
					!_bt_scanbehind_checkkeys(scan, dir, pstate.finaltup))
				{
					/* Schedule another primitive index scan after all */
					so->currPos.moreRight = false;
					so->needPrimScan = true;
					if (scan->parallel_scan)
						_bt_parallel_primscan_schedule(scan,
													   so->currPos.currPage);
					return false;
				}
			}

			so->scanBehind = so->oppositeDirCheck = false;	/* reset */
		}

		/*
		 * Consider pstate.startikey optimization once the ongoing primitive
		 * index scan has already read at least one page
		 */
		if (!pstate.firstpage && minoff < maxoff)
			_bt_set_startikey(scan, &pstate);

		/* load items[] in ascending order */
		itemIndex = 0;

		offnum = Max(offnum, minoff);

		while (offnum <= maxoff)
		{
			ItemId		iid = PageGetItemId(page, offnum);
			IndexTuple	itup;
			bool		passes_quals;

			/*
			 * If the scan specifies not to return killed tuples, then we
			 * treat a killed tuple as not passing the qual
			 */
			if (ignore_killed_tuples && ItemIdIsDead(iid))
			{
				offnum = OffsetNumberNext(offnum);
				continue;
			}

			itup = (IndexTuple) PageGetItem(page, iid);
			Assert(!BTreeTupleIsPivot(itup));

			pstate.offnum = offnum;
			passes_quals = _bt_checkkeys(scan, &pstate, arrayKeys,
										 itup, indnatts);

			/*
			 * Check if we need to skip ahead to a later tuple (only possible
			 * when the scan uses array keys)
			 */
			if (arrayKeys && OffsetNumberIsValid(pstate.skip))
			{
				Assert(!passes_quals && pstate.continuescan);
				Assert(offnum < pstate.skip);
				Assert(!pstate.forcenonrequired);

				offnum = pstate.skip;
				pstate.skip = InvalidOffsetNumber;
				continue;
			}

			if (passes_quals)
			{
				/* tuple passes all scan key conditions */
				if (!BTreeTupleIsPosting(itup))
				{
					/* Remember it */
					_bt_saveitem(so, itemIndex, offnum, itup);
					itemIndex++;
				}
				else
				{
					int			tupleOffset;

					/* Set up posting list state (and remember first TID) */
					tupleOffset =
						_bt_setuppostingitems(so, itemIndex, offnum,
											  BTreeTupleGetPostingN(itup, 0),
											  itup);
					itemIndex++;

					/* Remember all later TIDs (must be at least one) */
					for (int i = 1; i < BTreeTupleGetNPosting(itup); i++)
					{
						_bt_savepostingitem(so, itemIndex, offnum,
											BTreeTupleGetPostingN(itup, i),
											tupleOffset);
						itemIndex++;
					}
				}
			}
			/* When !continuescan, there can't be any more matches, so stop */
			if (!pstate.continuescan)
				break;

			offnum = OffsetNumberNext(offnum);
		}

		/*
		 * We don't need to visit page to the right when the high key
		 * indicates that no more matches will be found there.
		 *
		 * Checking the high key like this works out more often than you might
		 * think.  Leaf page splits pick a split point between the two most
		 * dissimilar tuples (this is weighed against the need to evenly share
		 * free space).  Leaf pages with high key attribute values that can
		 * only appear on non-pivot tuples on the right sibling page are
		 * common.
		 */
		if (pstate.continuescan && !so->scanBehind && !P_RIGHTMOST(opaque))
		{
			ItemId		iid = PageGetItemId(page, P_HIKEY);
			IndexTuple	itup = (IndexTuple) PageGetItem(page, iid);
			int			truncatt;

			/* Reset arrays, per _bt_set_startikey contract */
			if (pstate.forcenonrequired)
				_bt_start_array_keys(scan, dir);
			pstate.forcenonrequired = false;
			pstate.startikey = 0;	/* _bt_set_startikey ignores P_HIKEY */

			truncatt = BTreeTupleGetNAtts(itup, rel);
			_bt_checkkeys(scan, &pstate, arrayKeys, itup, truncatt);
		}

		if (!pstate.continuescan)
			so->currPos.moreRight = false;

		Assert(itemIndex <= MaxTIDsPerBTreePage);
		so->currPos.firstItem = 0;
		so->currPos.lastItem = itemIndex - 1;
		so->currPos.itemIndex = 0;
	}
	else
	{
		/* SK_SEARCHARRAY backward scans must provide final tuple up front */
		if (arrayKeys)
		{
			if (minoff <= maxoff && !P_LEFTMOST(opaque))
			{
				ItemId		iid = PageGetItemId(page, minoff);

				pstate.finaltup = (IndexTuple) PageGetItem(page, iid);

				if (unlikely(so->scanBehind) &&
					!_bt_scanbehind_checkkeys(scan, dir, pstate.finaltup))
				{
					/* Schedule another primitive index scan after all */
					so->currPos.moreLeft = false;
					so->needPrimScan = true;
					if (scan->parallel_scan)
						_bt_parallel_primscan_schedule(scan,
													   so->currPos.currPage);
					return false;
				}
			}

			so->scanBehind = so->oppositeDirCheck = false;	/* reset */
		}

		/*
		 * Consider pstate.startikey optimization once the ongoing primitive
		 * index scan has already read at least one page
		 */
		if (!pstate.firstpage && minoff < maxoff)
			_bt_set_startikey(scan, &pstate);

		/* load items[] in descending order */
		itemIndex = MaxTIDsPerBTreePage;

		offnum = Min(offnum, maxoff);

		while (offnum >= minoff)
		{
			ItemId		iid = PageGetItemId(page, offnum);
			IndexTuple	itup;
			bool		tuple_alive;
			bool		passes_quals;

			/*
			 * If the scan specifies not to return killed tuples, then we
			 * treat a killed tuple as not passing the qual.  Most of the
			 * time, it's a win to not bother examining the tuple's index
			 * keys, but just skip to the next tuple (previous, actually,
			 * since we're scanning backwards).  However, if this is the first
			 * tuple on the page, we do check the index keys, to prevent
			 * uselessly advancing to the page to the left.  This is similar
			 * to the high key optimization used by forward scans.
			 */
			if (ignore_killed_tuples && ItemIdIsDead(iid))
			{
				if (offnum > minoff)
				{
					offnum = OffsetNumberPrev(offnum);
					continue;
				}

				tuple_alive = false;
			}
			else
				tuple_alive = true;

			itup = (IndexTuple) PageGetItem(page, iid);
			Assert(!BTreeTupleIsPivot(itup));

			pstate.offnum = offnum;
			if (arrayKeys && offnum == minoff && pstate.forcenonrequired)
			{
				/* Reset arrays, per _bt_set_startikey contract */
				pstate.forcenonrequired = false;
				pstate.startikey = 0;
				_bt_start_array_keys(scan, dir);
			}
			passes_quals = _bt_checkkeys(scan, &pstate, arrayKeys,
										 itup, indnatts);

			if (arrayKeys && so->scanBehind)
			{
				/*
				 * Done scanning this page, but not done with the current
				 * primscan.
				 *
				 * Note: Forward scans don't check this explicitly, since they
				 * prefer to reuse pstate.skip for this instead.
				 */
				Assert(!passes_quals && pstate.continuescan);
				Assert(!pstate.forcenonrequired);

				break;
			}

			/*
			 * Check if we need to skip ahead to a later tuple (only possible
			 * when the scan uses array keys)
			 */
			if (arrayKeys && OffsetNumberIsValid(pstate.skip))
			{
				Assert(!passes_quals && pstate.continuescan);
				Assert(offnum > pstate.skip);
				Assert(!pstate.forcenonrequired);

				offnum = pstate.skip;
				pstate.skip = InvalidOffsetNumber;
				continue;
			}

			if (passes_quals && tuple_alive)
			{
				/* tuple passes all scan key conditions */
				if (!BTreeTupleIsPosting(itup))
				{
					/* Remember it */
					itemIndex--;
					_bt_saveitem(so, itemIndex, offnum, itup);
				}
				else
				{
					uint16		nitems = BTreeTupleGetNPosting(itup);
					int			tupleOffset;

					/* Set up posting list state (and remember last TID) */
					itemIndex--;
					tupleOffset =
						_bt_setuppostingitems(so, itemIndex, offnum,
											  BTreeTupleGetPostingN(itup, nitems - 1),
											  itup);

					/* Remember all prior TIDs (must be at least one) */
					for (int i = nitems - 2; i >= 0; i--)
					{
						itemIndex--;
						_bt_savepostingitem(so, itemIndex, offnum,
											BTreeTupleGetPostingN(itup, i),
											tupleOffset);
					}
				}
			}
			/* When !continuescan, there can't be any more matches, so stop */
			if (!pstate.continuescan)
				break;

			offnum = OffsetNumberPrev(offnum);
		}

		/*
		 * We don't need to visit page to the left when no more matches will
		 * be found there
		 */
		if (!pstate.continuescan)
			so->currPos.moreLeft = false;

		Assert(itemIndex >= 0);
		so->currPos.firstItem = itemIndex;
		so->currPos.lastItem = MaxTIDsPerBTreePage - 1;
		so->currPos.itemIndex = MaxTIDsPerBTreePage - 1;
	}

	/*
	 * If _bt_set_startikey told us to temporarily treat the scan's keys as
	 * nonrequired (possible only during scans with array keys), there must be
	 * no lasting consequences for the scan's array keys.  The scan's arrays
	 * should now have exactly the same elements as they would have had if the
	 * nonrequired behavior had never been used.  (In general, a scan's arrays
	 * are expected to track its progress through the index's key space.)
	 *
	 * We are required (by _bt_set_startikey) to call _bt_checkkeys against
	 * pstate.finaltup with pstate.forcenonrequired=false to allow the scan's
	 * arrays to recover.  Assert that that step hasn't been missed.
	 */
	Assert(!pstate.forcenonrequired);

	return (so->currPos.firstItem <= so->currPos.lastItem);
}

/*
 * _bt_start_array_keys() -- Initialize array keys at start of a scan
 *
 * Set up the cur_elem counters and fill in the first sk_argument value for
 * each array scankey.
 */
void
_bt_start_array_keys(IndexScanDesc scan, ScanDirection dir)
{
	Relation	rel = scan->indexRelation;
	BTScanOpaque so = (BTScanOpaque) scan->opaque;

	Assert(so->numArrayKeys);
	Assert(so->qual_ok);

	for (int i = 0; i < so->numArrayKeys; i++)
	{
		BTArrayKeyInfo *array = &so->arrayKeys[i];
		ScanKey		skey = &so->keyData[array->scan_key];

		Assert(skey->sk_flags & SK_SEARCHARRAY);

		_bt_array_set_low_or_high(rel, skey, array,
								  ScanDirectionIsForward(dir));
	}
	so->scanBehind = so->oppositeDirCheck = false;	/* reset */
}

/*
 * Determines an offset to the first scan key (an so->keyData[]-wise offset)
 * that is _not_ guaranteed to be satisfied by every tuple from pstate.page,
 * which is set in pstate.startikey for _bt_checkkeys calls for the page.
 * This allows caller to save cycles on comparisons of a prefix of keys while
 * reading pstate.page.
 *
 * Also determines if later calls to _bt_checkkeys (for pstate.page) should be
 * forced to treat all required scan keys >= pstate.startikey as nonrequired
 * (that is, if they're to be treated as if any SK_BT_REQFWD/SK_BT_REQBKWD
 * markings that were set by preprocessing were not set at all, for the
 * duration of _bt_checkkeys calls prior to the call for pstate.finaltup).
 * This is indicated to caller by setting pstate.forcenonrequired.
 *
 * Call here at the start of reading a leaf page beyond the first one for the
 * primitive index scan.  We consider all non-pivot tuples, so it doesn't make
 * sense to call here when only a subset of those tuples can ever be read.
 * This is also a good idea on performance grounds; not calling here when on
 * the first page (first for the current primitive scan) avoids wasting cycles
 * during selective point queries.  They typically don't stand to gain as much
 * when we can set pstate.startikey, and are likely to notice the overhead of
 * calling here.  (Also, allowing pstate.forcenonrequired to be set on a
 * primscan's first page would mislead _bt_advance_array_keys, which expects
 * pstate.nskipadvances to be representative of every first page's key space.)
 *
 * Caller must call _bt_start_array_keys and reset startikey/forcenonrequired
 * ahead of the finaltup _bt_checkkeys call when we set forcenonrequired=true.
 * This will give _bt_checkkeys the opportunity to call _bt_advance_array_keys
 * with sktrig_required=true, restoring the invariant that the scan's required
 * arrays always track the scan's progress through the index's key space.
 * Caller won't need to do this on the rightmost/leftmost page in the index
 * (where pstate.finaltup isn't ever set), since forcenonrequired will never
 * be set here in the first place.
 */
static void
_bt_set_startikey(IndexScanDesc scan, BTReadPageState *pstate)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	Relation	rel = scan->indexRelation;
	TupleDesc	tupdesc = RelationGetDescr(rel);
	ItemId		iid;
	IndexTuple	firsttup,
				lasttup;
	int			startikey = 0,
				arrayidx = 0,
				firstchangingattnum;
	bool		start_past_saop_eq = false;

	Assert(!so->scanBehind);
	Assert(pstate->minoff < pstate->maxoff);
	Assert(!pstate->firstpage);
	Assert(pstate->startikey == 0);
	Assert(!so->numArrayKeys || pstate->finaltup ||
		   P_RIGHTMOST(BTPageGetOpaque(pstate->page)) ||
		   P_LEFTMOST(BTPageGetOpaque(pstate->page)));

	if (so->numberOfKeys == 0)
		return;

	/* minoff is an offset to the lowest non-pivot tuple on the page */
	iid = PageGetItemId(pstate->page, pstate->minoff);
	firsttup = (IndexTuple) PageGetItem(pstate->page, iid);

	/* maxoff is an offset to the highest non-pivot tuple on the page */
	iid = PageGetItemId(pstate->page, pstate->maxoff);
	lasttup = (IndexTuple) PageGetItem(pstate->page, iid);

	/* Determine the first attribute whose values change on caller's page */
	firstchangingattnum = _bt_keep_natts_fast(rel, firsttup, lasttup);

	for (; startikey < so->numberOfKeys; startikey++)
	{
		ScanKey		key = so->keyData + startikey;
		BTArrayKeyInfo *array;
		Datum		firstdatum,
					lastdatum;
		bool		firstnull,
					lastnull;
		int32		result;

		/*
		 * Determine if it's safe to set pstate.startikey to an offset to a
		 * key that comes after this key, by examining this key
		 */
		if (key->sk_flags & SK_ROW_HEADER)
		{
			/* RowCompare inequality (header key) */
			ScanKey		subkey = (ScanKey) DatumGetPointer(key->sk_argument);
			bool		satisfied = false;

			for (;;)
			{
				int			cmpresult;
				bool		firstsatisfies = false;

				if (subkey->sk_attno > firstchangingattnum) /* >, not >= */
					break;		/* unsafe, preceding attr has multiple
								 * distinct values */

				if (subkey->sk_flags & SK_ISNULL)
					break;		/* unsafe, unsatisfiable NULL subkey arg */

				firstdatum = index_getattr(firsttup, subkey->sk_attno,
										   tupdesc, &firstnull);
				lastdatum = index_getattr(lasttup, subkey->sk_attno,
										  tupdesc, &lastnull);

				if (firstnull || lastnull)
					break;		/* unsafe, NULL value won't satisfy subkey */

				/*
				 * Compare the first tuple's datum for this row compare member
				 */
				cmpresult = DatumGetInt32(FunctionCall2Coll(&subkey->sk_func,
															subkey->sk_collation,
															firstdatum,
															subkey->sk_argument));
				if (subkey->sk_flags & SK_BT_DESC)
					INVERT_COMPARE_RESULT(cmpresult);

				if (cmpresult != 0 || (subkey->sk_flags & SK_ROW_END))
				{
					firstsatisfies = _bt_rowcompare_cmpresult(subkey,
															  cmpresult);
					if (!firstsatisfies)
					{
						/* Unsafe, firstdatum does not satisfy subkey */
						break;
					}
				}

				/*
				 * Compare the last tuple's datum for this row compare member
				 */
				cmpresult = DatumGetInt32(FunctionCall2Coll(&subkey->sk_func,
															subkey->sk_collation,
															lastdatum,
															subkey->sk_argument));
				if (subkey->sk_flags & SK_BT_DESC)
					INVERT_COMPARE_RESULT(cmpresult);

				if (cmpresult != 0 || (subkey->sk_flags & SK_ROW_END))
				{
					if (!firstsatisfies)
					{
						/*
						 * It's only safe to set startikey beyond the row
						 * compare header key when both firsttup and lasttup
						 * satisfy the key as a whole based on the same
						 * deciding subkey/attribute.  That can't happen now.
						 */
						break;	/* unsafe */
					}

					satisfied = _bt_rowcompare_cmpresult(subkey, cmpresult);
					break;		/* safe iff 'satisfied' is true */
				}

				/* Move on to next row member/subkey */
				if (subkey->sk_flags & SK_ROW_END)
					break;		/* defensive */
				subkey++;

				/*
				 * We deliberately don't check if the next subkey has the same
				 * strategy as this iteration's subkey (which happens when
				 * subkeys for both ASC and DESC columns are used together),
				 * nor if any subkey is marked required.  This is safe because
				 * in general all prior index attributes must have only one
				 * distinct value (across all of the tuples on the page) in
				 * order for us to even consider any subkey's attribute.
				 */
			}

			if (satisfied)
			{
				/* Safe, row compare satisfied by every tuple on page */
				continue;
			}

			break;				/* unsafe */
		}
		if (key->sk_strategy != BTEqualStrategyNumber)
		{
			/*
			 * Scalar inequality key.
			 *
			 * It's definitely safe for _bt_checkkeys to avoid assessing this
			 * inequality when the page's first and last non-pivot tuples both
			 * satisfy the inequality (since the same must also be true of all
			 * the tuples in between these two).
			 *
			 * Unlike the "=" case, it doesn't matter if this attribute has
			 * more than one distinct value (though it _is_ necessary for any
			 * and all _prior_ attributes to contain no more than one distinct
			 * value amongst all of the tuples from pstate.page).
			 */
			if (key->sk_attno > firstchangingattnum)	/* >, not >= */
				break;			/* unsafe, preceding attr has multiple
								 * distinct values */

			firstdatum = index_getattr(firsttup, key->sk_attno, tupdesc, &firstnull);
			lastdatum = index_getattr(lasttup, key->sk_attno, tupdesc, &lastnull);

			if (key->sk_flags & SK_ISNULL)
			{
				/* IS NOT NULL key */
				Assert(key->sk_flags & SK_SEARCHNOTNULL);

				if (firstnull || lastnull)
					break;		/* unsafe */

				/* Safe, IS NOT NULL key satisfied by every tuple */
				continue;
			}

			/* Test firsttup */
			if (firstnull ||
				!DatumGetBool(FunctionCall2Coll(&key->sk_func,
												key->sk_collation, firstdatum,
												key->sk_argument)))
				break;			/* unsafe */

			/* Test lasttup */
			if (lastnull ||
				!DatumGetBool(FunctionCall2Coll(&key->sk_func,
												key->sk_collation, lastdatum,
												key->sk_argument)))
				break;			/* unsafe */

			/* Safe, scalar inequality satisfied by every tuple */
			continue;
		}

		/* Some = key (could be a scalar = key, could be an array = key) */
		Assert(key->sk_strategy == BTEqualStrategyNumber);

		if (!(key->sk_flags & SK_SEARCHARRAY))
		{
			/*
			 * Scalar = key (possibly an IS NULL key).
			 *
			 * It is unsafe to set pstate.startikey to an ikey beyond this
			 * key, unless the = key is satisfied by every possible tuple on
			 * the page (possible only when attribute has just one distinct
			 * value among all tuples on the page).
			 */
			if (key->sk_attno >= firstchangingattnum)
				break;			/* unsafe, multiple distinct attr values */

			firstdatum = index_getattr(firsttup, key->sk_attno, tupdesc,
									   &firstnull);
			if (key->sk_flags & SK_ISNULL)
			{
				/* IS NULL key */
				Assert(key->sk_flags & SK_SEARCHNULL);

				if (!firstnull)
					break;		/* unsafe */

				/* Safe, IS NULL key satisfied by every tuple */
				continue;
			}
			if (firstnull ||
				!DatumGetBool(FunctionCall2Coll(&key->sk_func,
												key->sk_collation, firstdatum,
												key->sk_argument)))
				break;			/* unsafe */

			/* Safe, scalar = key satisfied by every tuple */
			continue;
		}

		/* = array key (could be a SAOP array, could be a skip array) */
		array = &so->arrayKeys[arrayidx++];
		Assert(array->scan_key == startikey);
		if (array->num_elems != -1)
		{
			/*
			 * SAOP array = key.
			 *
			 * Handle this like we handle scalar = keys (though binary search
			 * for a matching element, to avoid relying on key's sk_argument).
			 */
			if (key->sk_attno >= firstchangingattnum)
				break;			/* unsafe, multiple distinct attr values */

			firstdatum = index_getattr(firsttup, key->sk_attno, tupdesc,
									   &firstnull);
			_bt_binsrch_array_skey(&so->orderProcs[startikey],
								   false, NoMovementScanDirection,
								   firstdatum, firstnull, array, key,
								   &result);
			if (result != 0)
				break;			/* unsafe */

			/* Safe, SAOP = key satisfied by every tuple */
			start_past_saop_eq = true;
			continue;
		}

		/*
		 * Skip array = key
		 */
		Assert(key->sk_flags & SK_BT_SKIP);
		if (array->null_elem)
		{
			/*
			 * Non-range skip array = key.
			 *
			 * Safe, non-range skip array "satisfied" by every tuple on page
			 * (safe even when "key->sk_attno > firstchangingattnum").
			 */
			continue;
		}

		/*
		 * Range skip array = key.
		 *
		 * Handle this like we handle scalar inequality keys (but avoid using
		 * key's sk_argument directly, as in the SAOP array case).
		 */
		if (key->sk_attno > firstchangingattnum)	/* >, not >= */
			break;				/* unsafe, preceding attr has multiple
								 * distinct values */

		firstdatum = index_getattr(firsttup, key->sk_attno, tupdesc, &firstnull);
		lastdatum = index_getattr(lasttup, key->sk_attno, tupdesc, &lastnull);

		/* Test firsttup */
		_bt_binsrch_skiparray_skey(false, ForwardScanDirection,
								   firstdatum, firstnull, array, key,
								   &result);
		if (result != 0)
			break;				/* unsafe */

		/* Test lasttup */
		_bt_binsrch_skiparray_skey(false, ForwardScanDirection,
								   lastdatum, lastnull, array, key,
								   &result);
		if (result != 0)
			break;				/* unsafe */

		/* Safe, range skip array satisfied by every tuple on page */
	}

	/*
	 * Use of forcenonrequired is typically undesirable, since it'll force
	 * _bt_readpage caller to read every tuple on the page -- even though, in
	 * general, it might well be possible to end the scan on an earlier tuple.
	 * However, caller must use forcenonrequired when start_past_saop_eq=true,
	 * since the usual required array behavior might fail to roll over to the
	 * SAOP array.
	 *
	 * We always prefer forcenonrequired=true during scans with skip arrays
	 * (except on the first page of each primitive index scan), though -- even
	 * when "startikey == 0".  That way, _bt_advance_array_keys's low-order
	 * key precheck optimization can always be used (unless on the first page
	 * of the scan).  It seems slightly preferable to check more tuples when
	 * that allows us to do significantly less skip array maintenance.
	 */
	pstate->forcenonrequired = (start_past_saop_eq || so->skipScan);
	pstate->startikey = startikey;

	/*
	 * _bt_readpage caller is required to call _bt_checkkeys against page's
	 * finaltup with forcenonrequired=false whenever we initially set
	 * forcenonrequired=true.  That way the scan's arrays will reliably track
	 * its progress through the index's key space.
	 *
	 * We don't expect this when _bt_readpage caller has no finaltup due to
	 * its page being the rightmost (or the leftmost, during backwards scans).
	 * When we see that _bt_readpage has no finaltup, back out of everything.
	 */
	Assert(!pstate->forcenonrequired || so->numArrayKeys);
	if (pstate->forcenonrequired && !pstate->finaltup)
	{
		pstate->forcenonrequired = false;
		pstate->startikey = 0;
	}
}

/*
 * Test whether caller's finaltup tuple is still before the start of matches
 * for the current array keys.
 *
 * Called at the start of reading a page during a scan with array keys, though
 * only when the so->scanBehind flag was set on the scan's prior page.
 *
 * Returns false if the tuple is still before the start of matches.  When that
 * happens, caller should cut its losses and start a new primitive index scan.
 * Otherwise returns true.
 */
static bool
_bt_scanbehind_checkkeys(IndexScanDesc scan, ScanDirection dir,
						 IndexTuple finaltup)
{
	Relation	rel = scan->indexRelation;
	TupleDesc	tupdesc = RelationGetDescr(rel);
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	int			nfinaltupatts = BTreeTupleGetNAtts(finaltup, rel);
	bool		scanBehind;

	Assert(so->numArrayKeys);

	if (_bt_tuple_before_array_skeys(scan, dir, finaltup, tupdesc,
									 nfinaltupatts, false, 0, &scanBehind))
		return false;

	/*
	 * If scanBehind was set, all of the untruncated attribute values from
	 * finaltup that correspond to an array match the array's current element,
	 * but there are other keys associated with truncated suffix attributes.
	 * Array advancement must have incremented the scan's arrays on the
	 * previous page, resulting in a set of array keys that happen to be an
	 * exact match for the current page high key's untruncated prefix values.
	 *
	 * This page definitely doesn't contain tuples that the scan will need to
	 * return.  The next page may or may not contain relevant tuples.  Handle
	 * this by cutting our losses and starting a new primscan.
	 */
	if (scanBehind)
		return false;

	if (!so->oppositeDirCheck)
		return true;

	return _bt_oppodir_checkkeys(scan, dir, finaltup);
}

/*
 * Test whether an indextuple fails to satisfy an inequality required in the
 * opposite direction only.
 *
 * Caller's finaltup tuple is the page high key (for forwards scans), or the
 * first non-pivot tuple (for backwards scans).  Called during scans with
 * required array keys and required opposite-direction inequalities.
 *
 * Returns false if an inequality scan key required in the opposite direction
 * only isn't satisfied (and any earlier required scan keys are satisfied).
 * Otherwise returns true.
 *
 * An unsatisfied inequality required in the opposite direction only might
 * well enable skipping over many leaf pages, provided another _bt_first call
 * takes place.  This type of unsatisfied inequality won't usually cause
 * _bt_checkkeys to stop the scan to consider array advancement/starting a new
 * primitive index scan.
 */
static bool
_bt_oppodir_checkkeys(IndexScanDesc scan, ScanDirection dir,
					  IndexTuple finaltup)
{
	Relation	rel = scan->indexRelation;
	TupleDesc	tupdesc = RelationGetDescr(rel);
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	int			nfinaltupatts = BTreeTupleGetNAtts(finaltup, rel);
	bool		continuescan;
	ScanDirection flipped = -dir;
	int			ikey = 0;

	Assert(so->numArrayKeys);

	_bt_check_compare(scan, flipped, finaltup, nfinaltupatts, tupdesc, false,
					  false, &continuescan,
					  &ikey);

	if (!continuescan && so->keyData[ikey].sk_strategy != BTEqualStrategyNumber)
		return false;

	return true;
}

/* Save an index item into so->currPos.items[itemIndex] */
static void
_bt_saveitem(BTScanOpaque so, int itemIndex,
			 OffsetNumber offnum, IndexTuple itup)
{
	BTScanPosItem *currItem = &so->currPos.items[itemIndex];

	Assert(!BTreeTupleIsPivot(itup) && !BTreeTupleIsPosting(itup));

	currItem->heapTid = itup->t_tid;
	currItem->indexOffset = offnum;
	if (so->currTuples)
	{
		Size		itupsz = IndexTupleSize(itup);

		currItem->tupleOffset = so->currPos.nextTupleOffset;
		memcpy(so->currTuples + so->currPos.nextTupleOffset, itup, itupsz);
		so->currPos.nextTupleOffset += MAXALIGN(itupsz);
	}
}

/*
 * Setup state to save TIDs/items from a single posting list tuple.
 *
 * Saves an index item into so->currPos.items[itemIndex] for TID that is
 * returned to scan first.  Second or subsequent TIDs for posting list should
 * be saved by calling _bt_savepostingitem().
 *
 * Returns an offset into tuple storage space that main tuple is stored at if
 * needed.
 */
static int
_bt_setuppostingitems(BTScanOpaque so, int itemIndex, OffsetNumber offnum,
					  const ItemPointerData *heapTid, IndexTuple itup)
{
	BTScanPosItem *currItem = &so->currPos.items[itemIndex];

	Assert(BTreeTupleIsPosting(itup));

	currItem->heapTid = *heapTid;
	currItem->indexOffset = offnum;
	if (so->currTuples)
	{
		/* Save base IndexTuple (truncate posting list) */
		IndexTuple	base;
		Size		itupsz = BTreeTupleGetPostingOffset(itup);

		itupsz = MAXALIGN(itupsz);
		currItem->tupleOffset = so->currPos.nextTupleOffset;
		base = (IndexTuple) (so->currTuples + so->currPos.nextTupleOffset);
		memcpy(base, itup, itupsz);
		/* Defensively reduce work area index tuple header size */
		base->t_info &= ~INDEX_SIZE_MASK;
		base->t_info |= itupsz;
		so->currPos.nextTupleOffset += itupsz;

		return currItem->tupleOffset;
	}

	return 0;
}

/*
 * Save an index item into so->currPos.items[itemIndex] for current posting
 * tuple.
 *
 * Assumes that _bt_setuppostingitems() has already been called for current
 * posting list tuple.  Caller passes its return value as tupleOffset.
 */
static inline void
_bt_savepostingitem(BTScanOpaque so, int itemIndex, OffsetNumber offnum,
					ItemPointer heapTid, int tupleOffset)
{
	BTScanPosItem *currItem = &so->currPos.items[itemIndex];

	currItem->heapTid = *heapTid;
	currItem->indexOffset = offnum;

	/*
	 * Have index-only scans return the same base IndexTuple for every TID
	 * that originates from the same posting list
	 */
	if (so->currTuples)
		currItem->tupleOffset = tupleOffset;
}

#define LOOK_AHEAD_REQUIRED_RECHECKS 	3
#define LOOK_AHEAD_DEFAULT_DISTANCE 	5
#define NSKIPADVANCES_THRESHOLD			3

/*
 * Test whether an indextuple satisfies all the scankey conditions.
 *
 * Returns true if so, false if not.  If not,
 * we also determine whether there's any need to continue the scan beyond
 * this tuple, and set pstate.continuescan accordingly.  See comments for
 * _bt_preprocess_keys() about how this is done.
 *
 * Forward scan callers can pass a high key tuple in the hopes of having
 * us set pstate.continuescan to false, avoiding an unnecessary visit to
 * the page to the right.
 *
 * Advances the scan's array keys when necessary for arrayKeys=true callers.
 * Scans without any array keys must always pass arrayKeys=false.
 *
 * Also stops and starts primitive index scans for arrayKeys=true callers.
 * Scans with array keys are required to set up page state that helps us with
 * this.  The page's finaltup tuple (the page high key for a forward scan, or
 * the page's first non-pivot tuple for a backward scan) must be set in
 * pstate.finaltup ahead of the first call here for the page.  Set it to
 * NULL for rightmost page (or the leftmost page for backwards scans).
 *
 * scan: index scan descriptor (containing a search-type scankey)
 * pstate: page level input and output parameters
 * arrayKeys: should we advance the scan's array keys if necessary?
 * tuple: index tuple to test
 * tupnatts: number of attributes in tupnatts (high key may be truncated)
 */
static bool
_bt_checkkeys(IndexScanDesc scan, BTReadPageState *pstate, bool arrayKeys,
			  IndexTuple tuple, int tupnatts)
{
	TupleDesc	tupdesc = RelationGetDescr(scan->indexRelation);
	BTScanOpaque so PG_USED_FOR_ASSERTS_ONLY = (BTScanOpaque) scan->opaque;
	ScanDirection dir = pstate->dir;
	int			ikey = pstate->startikey;
	bool		res;

	Assert(BTreeTupleGetNAtts(tuple, scan->indexRelation) == tupnatts);
	Assert(!so->needPrimScan && !so->scanBehind && !so->oppositeDirCheck);
	Assert(arrayKeys || so->numArrayKeys == 0);

	res = _bt_check_compare(scan, dir, tuple, tupnatts, tupdesc, arrayKeys,
							pstate->forcenonrequired, &pstate->continuescan,
							&ikey);

	/*
	 * If _bt_check_compare relied on the pstate.startikey optimization, call
	 * again (in assert-enabled builds) to verify it didn't affect our answer.
	 *
	 * Note: we can't do this when !pstate.forcenonrequired, since any arrays
	 * before pstate.startikey won't have advanced on this page at all.
	 */
	Assert(!pstate->forcenonrequired || arrayKeys);
#ifdef USE_ASSERT_CHECKING
	if (pstate->startikey > 0 && !pstate->forcenonrequired)
	{
		bool		dres,
					dcontinuescan;
		int			dikey = 0;

		/* Pass advancenonrequired=false to avoid array side-effects */
		dres = _bt_check_compare(scan, dir, tuple, tupnatts, tupdesc, false,
								 pstate->forcenonrequired, &dcontinuescan,
								 &dikey);
		Assert(res == dres);
		Assert(pstate->continuescan == dcontinuescan);

		/*
		 * Should also get the same ikey result.  We need a slightly weaker
		 * assertion during arrayKeys calls, since they might be using an
		 * array that couldn't be marked required during preprocessing.
		 */
		Assert(arrayKeys || ikey == dikey);
		Assert(ikey <= dikey);
	}
#endif

	/*
	 * Only one _bt_check_compare call is required in the common case where
	 * there are no equality strategy array scan keys.  With array keys, we
	 * can only accept _bt_check_compare's answer unreservedly when it set
	 * pstate.continuescan=true.
	 */
	if (!arrayKeys || pstate->continuescan)
		return res;

	/*
	 * _bt_check_compare call set continuescan=false in the presence of
	 * equality type array keys.  This could mean that the tuple is just past
	 * the end of matches for the current array keys.
	 *
	 * It's also possible that the scan is still _before_ the _start_ of
	 * tuples matching the current set of array keys.  Check for that first.
	 */
	Assert(!pstate->forcenonrequired);
	if (_bt_tuple_before_array_skeys(scan, dir, tuple, tupdesc, tupnatts, true,
									 ikey, NULL))
	{
		/* Override _bt_check_compare, continue primitive scan */
		pstate->continuescan = true;

		/*
		 * We will end up here repeatedly given a group of tuples > the
		 * previous array keys and < the now-current keys (for a backwards
		 * scan it's just the same, though the operators swap positions).
		 *
		 * We must avoid allowing this linear search process to scan very many
		 * tuples from well before the start of tuples matching the current
		 * array keys (or from well before the point where we'll once again
		 * have to advance the scan's array keys).
		 *
		 * We keep the overhead under control by speculatively "looking ahead"
		 * to later still-unscanned items from this same leaf page.  We'll
		 * only attempt this once the number of tuples that the linear search
		 * process has examined starts to get out of hand.
		 */
		pstate->rechecks++;
		if (pstate->rechecks >= LOOK_AHEAD_REQUIRED_RECHECKS)
		{
			/* See if we should skip ahead within the current leaf page */
			_bt_checkkeys_look_ahead(scan, pstate, tupnatts, tupdesc);

			/*
			 * Might have set pstate.skip to a later page offset.  When that
			 * happens then _bt_readpage caller will inexpensively skip ahead
			 * to a later tuple from the same page (the one just after the
			 * tuple we successfully "looked ahead" to).
			 */
		}

		/* This indextuple doesn't match the current qual, in any case */
		return false;
	}

	/*
	 * Caller's tuple is >= the current set of array keys and other equality
	 * constraint scan keys (or <= if this is a backwards scan).  It's now
	 * clear that we _must_ advance any required array keys in lockstep with
	 * the scan.
	 */
	return _bt_advance_array_keys(scan, pstate, tuple, tupnatts, tupdesc,
								  ikey, true);
}

/*
 * Test whether an indextuple satisfies current scan condition.
 *
 * Return true if so, false if not.  If not, also sets *continuescan to false
 * when it's also not possible for any later tuples to pass the current qual
 * (with the scan's current set of array keys, in the current scan direction),
 * in addition to setting *ikey to the so->keyData[] subscript/offset for the
 * unsatisfied scan key (needed when caller must consider advancing the scan's
 * array keys).
 *
 * This is a subroutine for _bt_checkkeys.  We provisionally assume that
 * reaching the end of the current set of required keys (in particular the
 * current required array keys) ends the ongoing (primitive) index scan.
 * Callers without array keys should just end the scan right away when they
 * find that continuescan has been set to false here by us.  Things are more
 * complicated for callers with array keys.
 *
 * Callers with array keys must first consider advancing the arrays when
 * continuescan has been set to false here by us.  They must then consider if
 * it really does make sense to end the current (primitive) index scan, in
 * light of everything that is known at that point.  (In general when we set
 * continuescan=false for these callers it must be treated as provisional.)
 *
 * We deal with advancing unsatisfied non-required arrays directly, though.
 * This is safe, since by definition non-required keys can't end the scan.
 * This is just how we determine if non-required arrays are just unsatisfied
 * by the current array key, or if they're truly unsatisfied (that is, if
 * they're unsatisfied by every possible array key).
 *
 * Pass advancenonrequired=false to avoid all array related side effects.
 * This allows _bt_advance_array_keys caller to avoid infinite recursion.
 *
 * Pass forcenonrequired=true to instruct us to treat all keys as nonrequired.
 * This is used to make it safe to temporarily stop properly maintaining the
 * scan's required arrays.  _bt_checkkeys caller (_bt_readpage, actually)
 * determines a prefix of keys that must satisfy every possible corresponding
 * index attribute value from its page, which is passed to us via *ikey arg
 * (this is the first key that might be unsatisfied by tuples on the page).
 * Obviously, we won't maintain any array keys from before *ikey, so it's
 * quite possible for such arrays to "fall behind" the index's keyspace.
 * Caller will need to "catch up" by passing forcenonrequired=true (alongside
 * an *ikey=0) once the page's finaltup is reached.
 *
 * Note: it's safe to pass an *ikey > 0 with forcenonrequired=false, but only
 * when caller determines that it won't affect array maintenance.
 */
static bool
_bt_check_compare(IndexScanDesc scan, ScanDirection dir,
				  IndexTuple tuple, int tupnatts, TupleDesc tupdesc,
				  bool advancenonrequired, bool forcenonrequired,
				  bool *continuescan, int *ikey)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;

	*continuescan = true;		/* default assumption */

	for (; *ikey < so->numberOfKeys; (*ikey)++)
	{
		ScanKey		key = so->keyData + *ikey;
		Datum		datum;
		bool		isNull;
		bool		requiredSameDir = false,
					requiredOppositeDirOnly = false;

		/*
		 * Check if the key is required in the current scan direction, in the
		 * opposite scan direction _only_, or in neither direction (except
		 * when we're forced to treat all scan keys as nonrequired)
		 */
		if (forcenonrequired)
		{
			/* treating scan's keys as non-required */
		}
		else if (((key->sk_flags & SK_BT_REQFWD) && ScanDirectionIsForward(dir)) ||
				 ((key->sk_flags & SK_BT_REQBKWD) && ScanDirectionIsBackward(dir)))
			requiredSameDir = true;
		else if (((key->sk_flags & SK_BT_REQFWD) && ScanDirectionIsBackward(dir)) ||
				 ((key->sk_flags & SK_BT_REQBKWD) && ScanDirectionIsForward(dir)))
			requiredOppositeDirOnly = true;

		if (key->sk_attno > tupnatts)
		{
			/*
			 * This attribute is truncated (must be high key).  The value for
			 * this attribute in the first non-pivot tuple on the page to the
			 * right could be any possible value.  Assume that truncated
			 * attribute passes the qual.
			 */
			Assert(BTreeTupleIsPivot(tuple));
			continue;
		}

		/*
		 * A skip array scan key uses one of several sentinel values.  We just
		 * fall back on _bt_tuple_before_array_skeys when we see such a value.
		 */
		if (key->sk_flags & (SK_BT_MINVAL | SK_BT_MAXVAL |
							 SK_BT_NEXT | SK_BT_PRIOR))
		{
			Assert(key->sk_flags & SK_SEARCHARRAY);
			Assert(key->sk_flags & SK_BT_SKIP);
			Assert(requiredSameDir || forcenonrequired);

			/*
			 * Cannot fall back on _bt_tuple_before_array_skeys when we're
			 * treating the scan's keys as nonrequired, though.  Just handle
			 * this like any other non-required equality-type array key.
			 */
			if (forcenonrequired)
				return _bt_advance_array_keys(scan, NULL, tuple, tupnatts,
											  tupdesc, *ikey, false);

			*continuescan = false;
			return false;
		}

		/* row-comparison keys need special processing */
		if (key->sk_flags & SK_ROW_HEADER)
		{
			if (_bt_check_rowcompare(key, tuple, tupnatts, tupdesc, dir,
									 forcenonrequired, continuescan))
				continue;
			return false;
		}

		datum = index_getattr(tuple,
							  key->sk_attno,
							  tupdesc,
							  &isNull);

		if (key->sk_flags & SK_ISNULL)
		{
			/* Handle IS NULL/NOT NULL tests */
			if (key->sk_flags & SK_SEARCHNULL)
			{
				if (isNull)
					continue;	/* tuple satisfies this qual */
			}
			else
			{
				Assert(key->sk_flags & SK_SEARCHNOTNULL);
				Assert(!(key->sk_flags & SK_BT_SKIP));
				if (!isNull)
					continue;	/* tuple satisfies this qual */
			}

			/*
			 * Tuple fails this qual.  If it's a required qual for the current
			 * scan direction, then we can conclude no further tuples will
			 * pass, either.
			 */
			if (requiredSameDir)
				*continuescan = false;
			else if (unlikely(key->sk_flags & SK_BT_SKIP))
			{
				/*
				 * If we're treating scan keys as nonrequired, and encounter a
				 * skip array scan key whose current element is NULL, then it
				 * must be a non-range skip array.  It must be satisfied, so
				 * there's no need to call _bt_advance_array_keys to check.
				 */
				Assert(forcenonrequired && *ikey > 0);
				continue;
			}

			/*
			 * This indextuple doesn't match the qual.
			 */
			return false;
		}

		if (isNull)
		{
			/*
			 * Scalar scan key isn't satisfied by NULL tuple value.
			 *
			 * If we're treating scan keys as nonrequired, and key is for a
			 * skip array, then we must attempt to advance the array to NULL
			 * (if we're successful then the tuple might match the qual).
			 */
			if (unlikely(forcenonrequired && key->sk_flags & SK_BT_SKIP))
				return _bt_advance_array_keys(scan, NULL, tuple, tupnatts,
											  tupdesc, *ikey, false);

			if (key->sk_flags & SK_BT_NULLS_FIRST)
			{
				/*
				 * Since NULLs are sorted before non-NULLs, we know we have
				 * reached the lower limit of the range of values for this
				 * index attr.  On a backward scan, we can stop if this qual
				 * is one of the "must match" subset.  We can stop regardless
				 * of whether the qual is > or <, so long as it's required,
				 * because it's not possible for any future tuples to pass. On
				 * a forward scan, however, we must keep going, because we may
				 * have initially positioned to the start of the index.
				 * (_bt_advance_array_keys also relies on this behavior during
				 * forward scans.)
				 */
				if ((requiredSameDir || requiredOppositeDirOnly) &&
					ScanDirectionIsBackward(dir))
					*continuescan = false;
			}
			else
			{
				/*
				 * Since NULLs are sorted after non-NULLs, we know we have
				 * reached the upper limit of the range of values for this
				 * index attr.  On a forward scan, we can stop if this qual is
				 * one of the "must match" subset.  We can stop regardless of
				 * whether the qual is > or <, so long as it's required,
				 * because it's not possible for any future tuples to pass. On
				 * a backward scan, however, we must keep going, because we
				 * may have initially positioned to the end of the index.
				 * (_bt_advance_array_keys also relies on this behavior during
				 * backward scans.)
				 */
				if ((requiredSameDir || requiredOppositeDirOnly) &&
					ScanDirectionIsForward(dir))
					*continuescan = false;
			}

			/*
			 * This indextuple doesn't match the qual.
			 */
			return false;
		}

		if (!DatumGetBool(FunctionCall2Coll(&key->sk_func, key->sk_collation,
											datum, key->sk_argument)))
		{
			/*
			 * Tuple fails this qual.  If it's a required qual for the current
			 * scan direction, then we can conclude no further tuples will
			 * pass, either.
			 */
			if (requiredSameDir)
				*continuescan = false;

			/*
			 * If this is a non-required equality-type array key, the tuple
			 * needs to be checked against every possible array key.  Handle
			 * this by "advancing" the scan key's array to a matching value
			 * (if we're successful then the tuple might match the qual).
			 */
			else if (advancenonrequired &&
					 key->sk_strategy == BTEqualStrategyNumber &&
					 (key->sk_flags & SK_SEARCHARRAY))
				return _bt_advance_array_keys(scan, NULL, tuple, tupnatts,
											  tupdesc, *ikey, false);

			/*
			 * This indextuple doesn't match the qual.
			 */
			return false;
		}
	}

	/* If we get here, the tuple passes all index quals. */
	return true;
}

/*
 * Test whether an indextuple satisfies a row-comparison scan condition.
 *
 * Return true if so, false if not.  If not, also clear *continuescan if
 * it's not possible for any future tuples in the current scan direction
 * to pass the qual.
 *
 * This is a subroutine for _bt_checkkeys/_bt_check_compare.  Caller passes us
 * a row compare header key taken from so->keyData[].
 *
 * Row value comparisons can be described in terms of logical expansions that
 * use only scalar operators.  Consider the following example row comparison:
 *
 * "(a, b, c) > (7, 'bar', 62)"
 *
 * This can be evaluated as:
 *
 * "(a = 7 AND b = 'bar' AND c > 62) OR (a = 7 AND b > 'bar') OR (a > 7)".
 *
 * Notice that this condition is satisfied by _all_ rows that satisfy "a > 7",
 * and by a subset of all rows that satisfy "a >= 7" (possibly all such rows).
 * It _can't_ be satisfied by other rows (where "a < 7" or where "a IS NULL").
 * A row comparison header key can therefore often be treated as if it was a
 * simple scalar inequality on the row compare's most significant column.
 * (For example, _bt_advance_array_keys and most preprocessing routines treat
 * row compares like any other same-strategy inequality on the same column.)
 *
 * Things get more complicated for our row compare given a row where "a = 7".
 * Note that a row compare isn't necessarily satisfied by _every_ tuple that
 * appears between the first and last satisfied tuple returned by the scan,
 * due to the way that its lower-order subkeys are only conditionally applied.
 * A forwards scan that uses our example qual might initially return a tuple
 * "(a, b, c) = (7, 'zebra', 54)".  But it won't subsequently return a tuple
 * "(a, b, c) = (7, NULL, 1)" located to the right of the first matching tuple
 * (assume that "b" was declared NULLS LAST here).  The scan will only return
 * additional matches upon reaching tuples where "a > 7".  If you rereview our
 * example row comparison's logical expansion, you'll understand why this is.
 * (Here we assume that all subkeys could be marked required, guaranteeing
 * that row comparison order matches index order.  This is the common case.)
 *
 * Note that a row comparison header key behaves _exactly_ the same as a
 * similar scalar inequality key on the row's most significant column once the
 * scan reaches the point where it no longer needs to evaluate lower-order
 * subkeys (or before the point where it starts needing to evaluate them).
 * For example, once a forwards scan that uses our example qual reaches the
 * first tuple "a > 7", we'll behave in just the same way as our caller would
 * behave with a similar scalar inequality "a > 7" for the remainder of the
 * scan (assuming that the scan never changes direction/never goes backwards).
 * We'll even set continuescan=false according to exactly the same rules as
 * the ones our caller applies with simple scalar inequalities, including the
 * rules it applies when NULL tuple values don't satisfy an inequality qual.
 */
static bool
_bt_check_rowcompare(ScanKey header, IndexTuple tuple, int tupnatts,
					 TupleDesc tupdesc, ScanDirection dir,
					 bool forcenonrequired, bool *continuescan)
{
	ScanKey		subkey = (ScanKey) DatumGetPointer(header->sk_argument);
	int32		cmpresult = 0;
	bool		result;

	/* First subkey should be same as the header says */
	Assert(header->sk_flags & SK_ROW_HEADER);
	Assert(subkey->sk_attno == header->sk_attno);
	Assert(subkey->sk_strategy == header->sk_strategy);

	/* Loop over columns of the row condition */
	for (;;)
	{
		Datum		datum;
		bool		isNull;

		Assert(subkey->sk_flags & SK_ROW_MEMBER);

		/* When a NULL row member is compared, the row never matches */
		if (subkey->sk_flags & SK_ISNULL)
		{
			/*
			 * Unlike the simple-scankey case, this isn't a disallowed case
			 * (except when it's the first row element that has the NULL arg).
			 * But it can never match.  If all the earlier row comparison
			 * columns are required for the scan direction, we can stop the
			 * scan, because there can't be another tuple that will succeed.
			 */
			Assert(subkey != (ScanKey) DatumGetPointer(header->sk_argument));
			subkey--;
			if (forcenonrequired)
			{
				/* treating scan's keys as non-required */
			}
			else if ((subkey->sk_flags & SK_BT_REQFWD) &&
					 ScanDirectionIsForward(dir))
				*continuescan = false;
			else if ((subkey->sk_flags & SK_BT_REQBKWD) &&
					 ScanDirectionIsBackward(dir))
				*continuescan = false;
			return false;
		}

		if (subkey->sk_attno > tupnatts)
		{
			/*
			 * This attribute is truncated (must be high key).  The value for
			 * this attribute in the first non-pivot tuple on the page to the
			 * right could be any possible value.  Assume that truncated
			 * attribute passes the qual.
			 */
			Assert(BTreeTupleIsPivot(tuple));
			return true;
		}

		datum = index_getattr(tuple,
							  subkey->sk_attno,
							  tupdesc,
							  &isNull);

		if (isNull)
		{
			int			reqflags;

			if (forcenonrequired)
			{
				/* treating scan's keys as non-required */
			}
			else if (subkey->sk_flags & SK_BT_NULLS_FIRST)
			{
				/*
				 * Since NULLs are sorted before non-NULLs, we know we have
				 * reached the lower limit of the range of values for this
				 * index attr.  On a backward scan, we can stop if this qual
				 * is one of the "must match" subset.  However, on a forwards
				 * scan, we must keep going, because we may have initially
				 * positioned to the start of the index.
				 *
				 * All required NULLS FIRST > row members can use NULL tuple
				 * values to end backwards scans, just like with other values.
				 * A qual "WHERE (a, b, c) > (9, 42, 'foo')" can terminate a
				 * backwards scan upon reaching the index's rightmost "a = 9"
				 * tuple whose "b" column contains a NULL (if not sooner).
				 * Since "b" is NULLS FIRST, we can treat its NULLs as "<" 42.
				 */
				reqflags = SK_BT_REQBKWD;

				/*
				 * When a most significant required NULLS FIRST < row compare
				 * member sees NULL tuple values during a backwards scan, it
				 * signals the end of matches for the whole row compare/scan.
				 * A qual "WHERE (a, b, c) < (9, 42, 'foo')" will terminate a
				 * backwards scan upon reaching the rightmost tuple whose "a"
				 * column has a NULL.  The "a" NULL value is "<" 9, and yet
				 * our < row compare will still end the scan.  (This isn't
				 * safe with later/lower-order row members.  Notice that it
				 * can only happen with an "a" NULL some time after the scan
				 * completely stops needing to use its "b" and "c" members.)
				 */
				if (subkey == (ScanKey) DatumGetPointer(header->sk_argument))
					reqflags |= SK_BT_REQFWD;	/* safe, first row member */

				if ((subkey->sk_flags & reqflags) &&
					ScanDirectionIsBackward(dir))
					*continuescan = false;
			}
			else
			{
				/*
				 * Since NULLs are sorted after non-NULLs, we know we have
				 * reached the upper limit of the range of values for this
				 * index attr.  On a forward scan, we can stop if this qual is
				 * one of the "must match" subset.  However, on a backward
				 * scan, we must keep going, because we may have initially
				 * positioned to the end of the index.
				 *
				 * All required NULLS LAST < row members can use NULL tuple
				 * values to end forwards scans, just like with other values.
				 * A qual "WHERE (a, b, c) < (9, 42, 'foo')" can terminate a
				 * forwards scan upon reaching the index's leftmost "a = 9"
				 * tuple whose "b" column contains a NULL (if not sooner).
				 * Since "b" is NULLS LAST, we can treat its NULLs as ">" 42.
				 */
				reqflags = SK_BT_REQFWD;

				/*
				 * When a most significant required NULLS LAST > row compare
				 * member sees NULL tuple values during a forwards scan, it
				 * signals the end of matches for the whole row compare/scan.
				 * A qual "WHERE (a, b, c) > (9, 42, 'foo')" will terminate a
				 * forwards scan upon reaching the leftmost tuple whose "a"
				 * column has a NULL.  The "a" NULL value is ">" 9, and yet
				 * our > row compare will end the scan.  (This isn't safe with
				 * later/lower-order row members.  Notice that it can only
				 * happen with an "a" NULL some time after the scan completely
				 * stops needing to use its "b" and "c" members.)
				 */
				if (subkey == (ScanKey) DatumGetPointer(header->sk_argument))
					reqflags |= SK_BT_REQBKWD;	/* safe, first row member */

				if ((subkey->sk_flags & reqflags) &&
					ScanDirectionIsForward(dir))
					*continuescan = false;
			}

			/*
			 * In any case, this indextuple doesn't match the qual.
			 */
			return false;
		}

		/* Perform the test --- three-way comparison not bool operator */
		cmpresult = DatumGetInt32(FunctionCall2Coll(&subkey->sk_func,
													subkey->sk_collation,
													datum,
													subkey->sk_argument));

		if (subkey->sk_flags & SK_BT_DESC)
			INVERT_COMPARE_RESULT(cmpresult);

		/* Done comparing if unequal, else advance to next column */
		if (cmpresult != 0)
			break;

		if (subkey->sk_flags & SK_ROW_END)
			break;
		subkey++;
	}

	/* Final subkey/column determines if row compare is satisfied */
	result = _bt_rowcompare_cmpresult(subkey, cmpresult);

	if (!result && !forcenonrequired)
	{
		/*
		 * Tuple fails this qual.  If it's a required qual for the current
		 * scan direction, then we can conclude no further tuples will pass,
		 * either.  Note we have to look at the deciding column, not
		 * necessarily the first or last column of the row condition.
		 */
		if ((subkey->sk_flags & SK_BT_REQFWD) &&
			ScanDirectionIsForward(dir))
			*continuescan = false;
		else if ((subkey->sk_flags & SK_BT_REQBKWD) &&
				 ScanDirectionIsBackward(dir))
			*continuescan = false;
	}

	return result;
}

/*
 * Call here when a row compare member returns a non-zero result, or with the
 * result for the final ROW_END row compare member (no matter the cmpresult).
 *
 * cmpresult indicates the overall result of the row comparison (must already
 * be commuted for DESC subkeys), and subkey is the deciding row member.
 */
static bool
_bt_rowcompare_cmpresult(ScanKey subkey, int cmpresult)
{
	bool		satisfied;

	Assert(subkey->sk_flags & SK_ROW_MEMBER);

	switch (subkey->sk_strategy)
	{
		case BTLessStrategyNumber:
			satisfied = (cmpresult < 0);
			break;
		case BTLessEqualStrategyNumber:
			satisfied = (cmpresult <= 0);
			break;
		case BTGreaterEqualStrategyNumber:
			satisfied = (cmpresult >= 0);
			break;
		case BTGreaterStrategyNumber:
			satisfied = (cmpresult > 0);
			break;
		default:
			/* EQ and NE cases aren't allowed here */
			elog(ERROR, "unexpected strategy number %d", subkey->sk_strategy);
			satisfied = false;	/* keep compiler quiet */
			break;
	}

	return satisfied;
}

/*
 * _bt_tuple_before_array_skeys() -- too early to advance required arrays?
 *
 * We always compare the tuple using the current array keys (which we assume
 * are already set in so->keyData[]).  readpagetup indicates if tuple is the
 * scan's current _bt_readpage-wise tuple.
 *
 * readpagetup callers must only call here when _bt_check_compare already set
 * continuescan=false.  We help these callers deal with _bt_check_compare's
 * inability to distinguish between the < and > cases (it uses equality
 * operator scan keys, whereas we use 3-way ORDER procs).  These callers pass
 * a _bt_check_compare-set sktrig value that indicates which scan key
 * triggered the call (!readpagetup callers just pass us sktrig=0 instead).
 * This information allows us to avoid wastefully checking earlier scan keys
 * that were already deemed to have been satisfied inside _bt_check_compare.
 *
 * Returns false when caller's tuple is >= the current required equality scan
 * keys (or <=, in the case of backwards scans).  This happens to readpagetup
 * callers when the scan has reached the point of needing its array keys
 * advanced; caller will need to advance required and non-required arrays at
 * scan key offsets >= sktrig, plus scan keys < sktrig iff sktrig rolls over.
 * (When we return false to readpagetup callers, tuple can only be == current
 * required equality scan keys when caller's sktrig indicates that the arrays
 * need to be advanced due to an unsatisfied required inequality key trigger.)
 *
 * Returns true when caller passes a tuple that is < the current set of
 * equality keys for the most significant non-equal required scan key/column
 * (or > the keys, during backwards scans).  This happens to readpagetup
 * callers when tuple is still before the start of matches for the scan's
 * required equality strategy scan keys.  (sktrig can't have indicated that an
 * inequality strategy scan key wasn't satisfied in _bt_check_compare when we
 * return true.  In fact, we automatically return false when passed such an
 * inequality sktrig by readpagetup callers -- _bt_check_compare's initial
 * continuescan=false doesn't really need to be confirmed here by us.)
 *
 * !readpagetup callers optionally pass us *scanBehind, which tracks whether
 * any missing truncated attributes might have affected array advancement
 * (compared to what would happen if it was shown the first non-pivot tuple on
 * the page to the right of caller's finaltup/high key tuple instead).  It's
 * only possible that we'll set *scanBehind to true when caller passes us a
 * pivot tuple (with truncated -inf attributes) that we return false for.
 */
static bool
_bt_tuple_before_array_skeys(IndexScanDesc scan, ScanDirection dir,
							 IndexTuple tuple, TupleDesc tupdesc, int tupnatts,
							 bool readpagetup, int sktrig, bool *scanBehind)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;

	Assert(so->numArrayKeys);
	Assert(so->numberOfKeys);
	Assert(sktrig == 0 || readpagetup);
	Assert(!readpagetup || scanBehind == NULL);

	if (scanBehind)
		*scanBehind = false;

	for (int ikey = sktrig; ikey < so->numberOfKeys; ikey++)
	{
		ScanKey		cur = so->keyData + ikey;
		Datum		tupdatum;
		bool		tupnull;
		int32		result;

		/* readpagetup calls require one ORDER proc comparison (at most) */
		Assert(!readpagetup || ikey == sktrig);

		/*
		 * Once we reach a non-required scan key, we're completely done.
		 *
		 * Note: we deliberately don't consider the scan direction here.
		 * _bt_advance_array_keys caller requires that we track *scanBehind
		 * without concern for scan direction.
		 */
		if ((cur->sk_flags & (SK_BT_REQFWD | SK_BT_REQBKWD)) == 0)
		{
			Assert(!readpagetup);
			Assert(ikey > sktrig || ikey == 0);
			return false;
		}

		if (cur->sk_attno > tupnatts)
		{
			Assert(!readpagetup);

			/*
			 * When we reach a high key's truncated attribute, assume that the
			 * tuple attribute's value is >= the scan's equality constraint
			 * scan keys (but set *scanBehind to let interested callers know
			 * that a truncated attribute might have affected our answer).
			 */
			if (scanBehind)
				*scanBehind = true;

			return false;
		}

		/*
		 * Deal with inequality strategy scan keys that _bt_check_compare set
		 * continuescan=false for
		 */
		if (cur->sk_strategy != BTEqualStrategyNumber)
		{
			/*
			 * When _bt_check_compare indicated that a required inequality
			 * scan key wasn't satisfied, there's no need to verify anything;
			 * caller always calls _bt_advance_array_keys with this sktrig.
			 */
			if (readpagetup)
				return false;

			/*
			 * Otherwise we can't give up, since we must check all required
			 * scan keys (required in either direction) in order to correctly
			 * track *scanBehind for caller
			 */
			continue;
		}

		tupdatum = index_getattr(tuple, cur->sk_attno, tupdesc, &tupnull);

		if (likely(!(cur->sk_flags & (SK_BT_MINVAL | SK_BT_MAXVAL))))
		{
			/* Scankey has a valid/comparable sk_argument value */
			result = _bt_compare_array_skey(&so->orderProcs[ikey],
											tupdatum, tupnull,
											cur->sk_argument, cur);

			if (result == 0)
			{
				/*
				 * Interpret result in a way that takes NEXT/PRIOR into
				 * account
				 */
				if (cur->sk_flags & SK_BT_NEXT)
					result = -1;
				else if (cur->sk_flags & SK_BT_PRIOR)
					result = 1;

				Assert(result == 0 || (cur->sk_flags & SK_BT_SKIP));
			}
		}
		else
		{
			BTArrayKeyInfo *array = NULL;

			/*
			 * Current array element/array = scan key value is a sentinel
			 * value that represents the lowest (or highest) possible value
			 * that's still within the range of the array.
			 *
			 * Like _bt_first, we only see MINVAL keys during forwards scans
			 * (and similarly only see MAXVAL keys during backwards scans).
			 * Even if the scan's direction changes, we'll stop at some higher
			 * order key before we can ever reach any MAXVAL (or MINVAL) keys.
			 * (However, unlike _bt_first we _can_ get to keys marked either
			 * NEXT or PRIOR, regardless of the scan's current direction.)
			 */
			Assert(ScanDirectionIsForward(dir) ?
				   !(cur->sk_flags & SK_BT_MAXVAL) :
				   !(cur->sk_flags & SK_BT_MINVAL));

			/*
			 * There are no valid sk_argument values in MINVAL/MAXVAL keys.
			 * Check if tupdatum is within the range of skip array instead.
			 */
			for (int arrayidx = 0; arrayidx < so->numArrayKeys; arrayidx++)
			{
				array = &so->arrayKeys[arrayidx];
				if (array->scan_key == ikey)
					break;
			}

			_bt_binsrch_skiparray_skey(false, dir, tupdatum, tupnull,
									   array, cur, &result);

			if (result == 0)
			{
				/*
				 * tupdatum satisfies both low_compare and high_compare, so
				 * it's time to advance the array keys.
				 *
				 * Note: It's possible that the skip array will "advance" from
				 * its MINVAL (or MAXVAL) representation to an alternative,
				 * logically equivalent representation of the same value: a
				 * representation where the = key gets a valid datum in its
				 * sk_argument.  This is only possible when low_compare uses
				 * the >= strategy (or high_compare uses the <= strategy).
				 */
				return false;
			}
		}

		/*
		 * Does this comparison indicate that caller must _not_ advance the
		 * scan's arrays just yet?
		 */
		if ((ScanDirectionIsForward(dir) && result < 0) ||
			(ScanDirectionIsBackward(dir) && result > 0))
			return true;

		/*
		 * Does this comparison indicate that caller should now advance the
		 * scan's arrays?  (Must be if we get here during a readpagetup call.)
		 */
		if (readpagetup || result != 0)
		{
			Assert(result != 0);
			return false;
		}

		/*
		 * Inconclusive -- need to check later scan keys, too.
		 *
		 * This must be a finaltup precheck, or a call made from an assertion.
		 */
		Assert(result == 0);
	}

	Assert(!readpagetup);

	return false;
}

/*
 * Determine if a scan with array keys should skip over uninteresting tuples.
 *
 * This is a subroutine for _bt_checkkeys, called when _bt_readpage's linear
 * search process has scanned an excessive number of tuples whose key space is
 * "between arrays".  (The linear search process is started after _bt_readpage
 * finishes reading an initial group of matching tuples.  It locates the start
 * of the first group of tuples matching the next set of required array keys.)
 *
 * When look ahead is successful, we set pstate.skip which
 * instructs _bt_readpage to skip ahead to that tuple next (could be past the
 * end of the scan's leaf page).  Pages where the optimization is effective
 * will generally still need to skip several times.  Each call here performs
 * only a single "look ahead" comparison of a later tuple, whose distance from
 * the current tuple is determined by heuristics.
 */
static void
_bt_checkkeys_look_ahead(IndexScanDesc scan, BTReadPageState *pstate,
						 int tupnatts, TupleDesc tupdesc)
{
	ScanDirection dir = pstate->dir;
	OffsetNumber aheadoffnum;
	IndexTuple	ahead;

	Assert(!pstate->forcenonrequired);

	/* Avoid looking ahead when comparing the page high key */
	if (pstate->offnum < pstate->minoff)
		return;

	/*
	 * Don't look ahead when there aren't enough tuples remaining on the page
	 * (in the current scan direction) for it to be worth our while
	 */
	if (ScanDirectionIsForward(dir) &&
		pstate->offnum >= pstate->maxoff - LOOK_AHEAD_DEFAULT_DISTANCE)
		return;
	else if (ScanDirectionIsBackward(dir) &&
			 pstate->offnum <= pstate->minoff + LOOK_AHEAD_DEFAULT_DISTANCE)
		return;

	/*
	 * The look ahead distance starts small, and ramps up as each call here
	 * allows _bt_readpage to skip over more tuples
	 */
	if (!pstate->targetdistance)
		pstate->targetdistance = LOOK_AHEAD_DEFAULT_DISTANCE;
	else if (pstate->targetdistance < MaxIndexTuplesPerPage / 2)
		pstate->targetdistance *= 2;

	/* Don't read past the end (or before the start) of the page, though */
	if (ScanDirectionIsForward(dir))
		aheadoffnum = Min((int) pstate->maxoff,
						  (int) pstate->offnum + pstate->targetdistance);
	else
		aheadoffnum = Max((int) pstate->minoff,
						  (int) pstate->offnum - pstate->targetdistance);

	ahead = (IndexTuple) PageGetItem(pstate->page,
									 PageGetItemId(pstate->page, aheadoffnum));
	if (_bt_tuple_before_array_skeys(scan, dir, ahead, tupdesc, tupnatts,
									 false, 0, NULL))
	{
		/*
		 * Success -- instruct _bt_readpage to skip ahead to very next tuple
		 * after the one we determined was still before the current array keys
		 */
		if (ScanDirectionIsForward(dir))
			pstate->skip = aheadoffnum + 1;
		else
			pstate->skip = aheadoffnum - 1;
	}
	else
	{
		/*
		 * Failure -- "ahead" tuple is too far ahead (we were too aggressive).
		 *
		 * Reset the number of rechecks, and aggressively reduce the target
		 * distance (we're much more aggressive here than we were when the
		 * distance was initially ramped up).
		 */
		pstate->rechecks = 0;
		pstate->targetdistance = Max(pstate->targetdistance / 8, 1);
	}
}

/*
 * _bt_advance_array_keys() -- Advance array elements using a tuple
 *
 * The scan always gets a new qual as a consequence of calling here (except
 * when we determine that the top-level scan has run out of matching tuples).
 * All later _bt_check_compare calls also use the same new qual that was first
 * used here (at least until the next call here advances the keys once again).
 * It's convenient to structure _bt_check_compare rechecks of caller's tuple
 * (using the new qual) as one the steps of advancing the scan's array keys,
 * so this function works as a wrapper around _bt_check_compare.
 *
 * Like _bt_check_compare, we'll set pstate.continuescan on behalf of the
 * caller, and return a boolean indicating if caller's tuple satisfies the
 * scan's new qual.  But unlike _bt_check_compare, we set so->needPrimScan
 * when we set continuescan=false, indicating if a new primitive index scan
 * has been scheduled (otherwise, the top-level scan has run out of tuples in
 * the current scan direction).
 *
 * Caller must use _bt_tuple_before_array_skeys to determine if the current
 * place in the scan is >= the current array keys _before_ calling here.
 * We're responsible for ensuring that caller's tuple is <= the newly advanced
 * required array keys once we return.  We try to find an exact match, but
 * failing that we'll advance the array keys to whatever set of array elements
 * comes next in the key space for the current scan direction.  Required array
 * keys "ratchet forwards" (or backwards).  They can only advance as the scan
 * itself advances through the index/key space.
 *
 * (The rules are the same for backwards scans, except that the operators are
 * flipped: just replace the precondition's >= operator with a <=, and the
 * postcondition's <= operator with a >=.  In other words, just swap the
 * precondition with the postcondition.)
 *
 * We also deal with "advancing" non-required arrays here (or arrays that are
 * treated as non-required for the duration of a _bt_readpage call).  Callers
 * whose sktrig scan key is non-required specify sktrig_required=false.  These
 * calls are the only exception to the general rule about always advancing the
 * required array keys (the scan may not even have a required array).  These
 * callers should just pass a NULL pstate (since there is never any question
 * of stopping the scan).  No call to _bt_tuple_before_array_skeys is required
 * ahead of these calls (it's already clear that any required scan keys must
 * be satisfied by caller's tuple).
 *
 * Note that we deal with non-array required equality strategy scan keys as
 * degenerate single element arrays here.  Obviously, they can never really
 * advance in the way that real arrays can, but they must still affect how we
 * advance real array scan keys (exactly like true array equality scan keys).
 * We have to keep around a 3-way ORDER proc for these (using the "=" operator
 * won't do), since in general whether the tuple is < or > _any_ unsatisfied
 * required equality key influences how the scan's real arrays must advance.
 *
 * Note also that we may sometimes need to advance the array keys when the
 * existing required array keys (and other required equality keys) are already
 * an exact match for every corresponding value from caller's tuple.  We must
 * do this for inequalities that _bt_check_compare set continuescan=false for.
 * They'll advance the array keys here, just like any other scan key that
 * _bt_check_compare stops on.  (This can even happen _after_ we advance the
 * array keys, in which case we'll advance the array keys a second time.  That
 * way _bt_checkkeys caller always has its required arrays advance to the
 * maximum possible extent that its tuple will allow.)
 */
static bool
_bt_advance_array_keys(IndexScanDesc scan, BTReadPageState *pstate,
					   IndexTuple tuple, int tupnatts, TupleDesc tupdesc,
					   int sktrig, bool sktrig_required)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	Relation	rel = scan->indexRelation;
	ScanDirection dir = pstate ? pstate->dir : ForwardScanDirection;
	int			arrayidx = 0;
	bool		beyond_end_advance = false,
				skip_array_advanced = false,
				has_required_opposite_direction_only = false,
				all_required_satisfied = true,
				all_satisfied = true;

	Assert(!so->needPrimScan && !so->scanBehind && !so->oppositeDirCheck);
	Assert(_bt_verify_keys_with_arraykeys(scan));

	if (sktrig_required)
	{
		/*
		 * Precondition array state assertion
		 */
		Assert(!_bt_tuple_before_array_skeys(scan, dir, tuple, tupdesc,
											 tupnatts, false, 0, NULL));

		/*
		 * Once we return we'll have a new set of required array keys, so
		 * reset state used by "look ahead" optimization
		 */
		pstate->rechecks = 0;
		pstate->targetdistance = 0;
	}
	else if (sktrig < so->numberOfKeys - 1 &&
			 !(so->keyData[so->numberOfKeys - 1].sk_flags & SK_SEARCHARRAY))
	{
		int			least_sign_ikey = so->numberOfKeys - 1;
		bool		continuescan;

		/*
		 * Optimization: perform a precheck of the least significant key
		 * during !sktrig_required calls when it isn't already our sktrig
		 * (provided the precheck key is not itself an array).
		 *
		 * When the precheck works out we'll avoid an expensive binary search
		 * of sktrig's array (plus any other arrays before least_sign_ikey).
		 */
		Assert(so->keyData[sktrig].sk_flags & SK_SEARCHARRAY);
		if (!_bt_check_compare(scan, dir, tuple, tupnatts, tupdesc, false,
							   false, &continuescan,
							   &least_sign_ikey))
			return false;
	}

	for (int ikey = 0; ikey < so->numberOfKeys; ikey++)
	{
		ScanKey		cur = so->keyData + ikey;
		BTArrayKeyInfo *array = NULL;
		Datum		tupdatum;
		bool		required = false,
					tupnull;
		int32		result;
		int			set_elem = 0;

		if (cur->sk_strategy == BTEqualStrategyNumber)
		{
			/* Manage array state */
			if (cur->sk_flags & SK_SEARCHARRAY)
			{
				array = &so->arrayKeys[arrayidx++];
				Assert(array->scan_key == ikey);
			}
		}
		else
		{
			/*
			 * Are any inequalities required in the opposite direction only
			 * present here?
			 */
			if (((ScanDirectionIsForward(dir) &&
				  (cur->sk_flags & (SK_BT_REQBKWD))) ||
				 (ScanDirectionIsBackward(dir) &&
				  (cur->sk_flags & (SK_BT_REQFWD)))))
				has_required_opposite_direction_only = true;
		}

		/* Optimization: skip over known-satisfied scan keys */
		if (ikey < sktrig)
			continue;

		if (cur->sk_flags & (SK_BT_REQFWD | SK_BT_REQBKWD))
		{
			required = true;

			if (cur->sk_attno > tupnatts)
			{
				/* Set this just like _bt_tuple_before_array_skeys */
				Assert(sktrig < ikey);
				so->scanBehind = true;
			}
		}

		/*
		 * Handle a required non-array scan key that the initial call to
		 * _bt_check_compare indicated triggered array advancement, if any.
		 *
		 * The non-array scan key's strategy will be <, <=, or = during a
		 * forwards scan (or any one of =, >=, or > during a backwards scan).
		 * It follows that the corresponding tuple attribute's value must now
		 * be either > or >= the scan key value (for backwards scans it must
		 * be either < or <= that value).
		 *
		 * If this is a required equality strategy scan key, this is just an
		 * optimization; _bt_tuple_before_array_skeys already confirmed that
		 * this scan key places us ahead of caller's tuple.  There's no need
		 * to repeat that work now.  (The same underlying principle also gets
		 * applied by the cur_elem_trig optimization used to speed up searches
		 * for the next array element.)
		 *
		 * If this is a required inequality strategy scan key, we _must_ rely
		 * on _bt_check_compare like this; we aren't capable of directly
		 * evaluating required inequality strategy scan keys here, on our own.
		 */
		if (ikey == sktrig && !array)
		{
			Assert(sktrig_required && required && all_required_satisfied);

			/* Use "beyond end" advancement.  See below for an explanation. */
			beyond_end_advance = true;
			all_satisfied = all_required_satisfied = false;

			continue;
		}

		/*
		 * Nothing more for us to do with an inequality strategy scan key that
		 * wasn't the one that _bt_check_compare stopped on, though.
		 *
		 * Note: if our later call to _bt_check_compare (to recheck caller's
		 * tuple) sets continuescan=false due to finding this same inequality
		 * unsatisfied (possible when it's required in the scan direction),
		 * we'll deal with it via a recursive "second pass" call.
		 */
		else if (cur->sk_strategy != BTEqualStrategyNumber)
			continue;

		/*
		 * Nothing for us to do with an equality strategy scan key that isn't
		 * marked required, either -- unless it's a non-required array
		 */
		else if (!required && !array)
			continue;

		/*
		 * Here we perform steps for all array scan keys after a required
		 * array scan key whose binary search triggered "beyond end of array
		 * element" array advancement due to encountering a tuple attribute
		 * value > the closest matching array key (or < for backwards scans).
		 */
		if (beyond_end_advance)
		{
			if (array)
				_bt_array_set_low_or_high(rel, cur, array,
										  ScanDirectionIsBackward(dir));

			continue;
		}

		/*
		 * Here we perform steps for all array scan keys after a required
		 * array scan key whose tuple attribute was < the closest matching
		 * array key when we dealt with it (or > for backwards scans).
		 *
		 * This earlier required array key already puts us ahead of caller's
		 * tuple in the key space (for the current scan direction).  We must
		 * make sure that subsequent lower-order array keys do not put us too
		 * far ahead (ahead of tuples that have yet to be seen by our caller).
		 * For example, when a tuple "(a, b) = (42, 5)" advances the array
		 * keys on "a" from 40 to 45, we must also set "b" to whatever the
		 * first array element for "b" is.  It would be wrong to allow "b" to
		 * be set based on the tuple value.
		 *
		 * Perform the same steps with truncated high key attributes.  You can
		 * think of this as a "binary search" for the element closest to the
		 * value -inf.  Again, the arrays must never get ahead of the scan.
		 */
		if (!all_required_satisfied || cur->sk_attno > tupnatts)
		{
			if (array)
				_bt_array_set_low_or_high(rel, cur, array,
										  ScanDirectionIsForward(dir));

			continue;
		}

		/*
		 * Search in scankey's array for the corresponding tuple attribute
		 * value from caller's tuple
		 */
		tupdatum = index_getattr(tuple, cur->sk_attno, tupdesc, &tupnull);

		if (array)
		{
			bool		cur_elem_trig = (sktrig_required && ikey == sktrig);

			/*
			 * "Binary search" by checking if tupdatum/tupnull are within the
			 * range of the skip array
			 */
			if (array->num_elems == -1)
				_bt_binsrch_skiparray_skey(cur_elem_trig, dir,
										   tupdatum, tupnull, array, cur,
										   &result);

			/*
			 * Binary search for the closest match from the SAOP array
			 */
			else
				set_elem = _bt_binsrch_array_skey(&so->orderProcs[ikey],
												  cur_elem_trig, dir,
												  tupdatum, tupnull, array, cur,
												  &result);
		}
		else
		{
			Assert(required);

			/*
			 * This is a required non-array equality strategy scan key, which
			 * we'll treat as a degenerate single element array.
			 *
			 * This scan key's imaginary "array" can't really advance, but it
			 * can still roll over like any other array.  (Actually, this is
			 * no different to real single value arrays, which never advance
			 * without rolling over -- they can never truly advance, either.)
			 */
			result = _bt_compare_array_skey(&so->orderProcs[ikey],
											tupdatum, tupnull,
											cur->sk_argument, cur);
		}

		/*
		 * Consider "beyond end of array element" array advancement.
		 *
		 * When the tuple attribute value is > the closest matching array key
		 * (or < in the backwards scan case), we need to ratchet this array
		 * forward (backward) by one increment, so that caller's tuple ends up
		 * being < final array value instead (or > final array value instead).
		 * This process has to work for all of the arrays, not just this one:
		 * it must "carry" to higher-order arrays when the set_elem that we
		 * just found happens to be the final one for the scan's direction.
		 * Incrementing (decrementing) set_elem itself isn't good enough.
		 *
		 * Our approach is to provisionally use set_elem as if it was an exact
		 * match now, then set each later/less significant array to whatever
		 * its final element is.  Once outside the loop we'll then "increment
		 * this array's set_elem" by calling _bt_advance_array_keys_increment.
		 * That way the process rolls over to higher order arrays as needed.
		 *
		 * Under this scheme any required arrays only ever ratchet forwards
		 * (or backwards), and always do so to the maximum possible extent
		 * that we can know will be safe without seeing the scan's next tuple.
		 * We don't need any special handling for required scan keys that lack
		 * a real array to advance, nor for redundant scan keys that couldn't
		 * be eliminated by _bt_preprocess_keys.  It won't matter if some of
		 * our "true" array scan keys (or even all of them) are non-required.
		 */
		if (sktrig_required && required &&
			((ScanDirectionIsForward(dir) && result > 0) ||
			 (ScanDirectionIsBackward(dir) && result < 0)))
			beyond_end_advance = true;

		Assert(all_required_satisfied && all_satisfied);
		if (result != 0)
		{
			/*
			 * Track whether caller's tuple satisfies our new post-advancement
			 * qual, for required scan keys, as well as for the entire set of
			 * interesting scan keys (all required scan keys plus non-required
			 * array scan keys are considered interesting.)
			 */
			all_satisfied = false;
			if (sktrig_required && required)
				all_required_satisfied = false;
			else
			{
				/*
				 * There's no need to advance the arrays using the best
				 * available match for a non-required array.  Give up now.
				 * (Though note that sktrig_required calls still have to do
				 * all the usual post-advancement steps, including the recheck
				 * call to _bt_check_compare.)
				 */
				break;
			}
		}

		/* Advance array keys, even when we don't have an exact match */
		if (array)
		{
			if (array->num_elems == -1)
			{
				/* Skip array's new element is tupdatum (or MINVAL/MAXVAL) */
				_bt_skiparray_set_element(rel, cur, array, result,
										  tupdatum, tupnull);
				skip_array_advanced = true;
			}
			else if (array->cur_elem != set_elem)
			{
				/* SAOP array's new element is set_elem datum */
				array->cur_elem = set_elem;
				cur->sk_argument = array->elem_values[set_elem];
			}
		}
	}

	/*
	 * Advance the array keys incrementally whenever "beyond end of array
	 * element" array advancement happens, so that advancement will carry to
	 * higher-order arrays (might exhaust all the scan's arrays instead, which
	 * ends the top-level scan).
	 */
	if (beyond_end_advance &&
		!_bt_advance_array_keys_increment(scan, dir, &skip_array_advanced))
		goto end_toplevel_scan;

	Assert(_bt_verify_keys_with_arraykeys(scan));

	/*
	 * Maintain a page-level count of the number of times the scan's array
	 * keys advanced in a way that affected at least one skip array
	 */
	if (sktrig_required && skip_array_advanced)
		pstate->nskipadvances++;

	/*
	 * Does tuple now satisfy our new qual?  Recheck with _bt_check_compare.
	 *
	 * Calls triggered by an unsatisfied required scan key, whose tuple now
	 * satisfies all required scan keys, but not all nonrequired array keys,
	 * will still require a recheck call to _bt_check_compare.  They'll still
	 * need its "second pass" handling of required inequality scan keys.
	 * (Might have missed a still-unsatisfied required inequality scan key
	 * that caller didn't detect as the sktrig scan key during its initial
	 * _bt_check_compare call that used the old/original qual.)
	 *
	 * Calls triggered by an unsatisfied nonrequired array scan key never need
	 * "second pass" handling of required inequalities (nor any other handling
	 * of any required scan key).  All that matters is whether caller's tuple
	 * satisfies the new qual, so it's safe to just skip the _bt_check_compare
	 * recheck when we've already determined that it can only return 'false'.
	 *
	 * Note: In practice most scan keys are marked required by preprocessing,
	 * if necessary by generating a preceding skip array.  We nevertheless
	 * often handle array keys marked required as if they were nonrequired.
	 * This behavior is requested by our _bt_check_compare caller, though only
	 * when it is passed "forcenonrequired=true" by _bt_checkkeys.
	 */
	if ((sktrig_required && all_required_satisfied) ||
		(!sktrig_required && all_satisfied))
	{
		int			nsktrig = sktrig + 1;
		bool		continuescan;

		Assert(all_required_satisfied);

		/* Recheck _bt_check_compare on behalf of caller */
		if (_bt_check_compare(scan, dir, tuple, tupnatts, tupdesc, false,
							  !sktrig_required, &continuescan,
							  &nsktrig) &&
			!so->scanBehind)
		{
			/* This tuple satisfies the new qual */
			Assert(all_satisfied && continuescan);

			if (pstate)
				pstate->continuescan = true;

			return true;
		}

		/*
		 * Consider "second pass" handling of required inequalities.
		 *
		 * It's possible that our _bt_check_compare call indicated that the
		 * scan should end due to some unsatisfied inequality that wasn't
		 * initially recognized as such by us.  Handle this by calling
		 * ourselves recursively, this time indicating that the trigger is the
		 * inequality that we missed first time around (and using a set of
		 * required array/equality keys that are now exact matches for tuple).
		 *
		 * We make a strong, general guarantee that every _bt_checkkeys call
		 * here will advance the array keys to the maximum possible extent
		 * that we can know to be safe based on caller's tuple alone.  If we
		 * didn't perform this step, then that guarantee wouldn't quite hold.
		 */
		if (unlikely(!continuescan))
		{
			bool		satisfied PG_USED_FOR_ASSERTS_ONLY;

			Assert(sktrig_required);
			Assert(so->keyData[nsktrig].sk_strategy != BTEqualStrategyNumber);

			/*
			 * The tuple must use "beyond end" advancement during the
			 * recursive call, so we cannot possibly end up back here when
			 * recursing.  We'll consume a small, fixed amount of stack space.
			 */
			Assert(!beyond_end_advance);

			/* Advance the array keys a second time using same tuple */
			satisfied = _bt_advance_array_keys(scan, pstate, tuple, tupnatts,
											   tupdesc, nsktrig, true);

			/* This tuple doesn't satisfy the inequality */
			Assert(!satisfied);
			return false;
		}

		/*
		 * Some non-required scan key (from new qual) still not satisfied.
		 *
		 * All scan keys required in the current scan direction must still be
		 * satisfied, though, so we can trust all_required_satisfied below.
		 */
	}

	/*
	 * When we were called just to deal with "advancing" non-required arrays,
	 * this is as far as we can go (cannot stop the scan for these callers)
	 */
	if (!sktrig_required)
	{
		/* Caller's tuple doesn't match any qual */
		return false;
	}

	/*
	 * Postcondition array state assertion (for still-unsatisfied tuples).
	 *
	 * By here we have established that the scan's required arrays (scan must
	 * have at least one required array) advanced, without becoming exhausted.
	 *
	 * Caller's tuple is now < the newly advanced array keys (or > when this
	 * is a backwards scan), except in the case where we only got this far due
	 * to an unsatisfied non-required scan key.  Verify that with an assert.
	 *
	 * Note: we don't just quit at this point when all required scan keys were
	 * found to be satisfied because we need to consider edge-cases involving
	 * scan keys required in the opposite direction only; those aren't tracked
	 * by all_required_satisfied.
	 */
	Assert(_bt_tuple_before_array_skeys(scan, dir, tuple, tupdesc, tupnatts,
										false, 0, NULL) ==
		   !all_required_satisfied);

	/*
	 * We generally permit primitive index scans to continue onto the next
	 * sibling page when the page's finaltup satisfies all required scan keys
	 * at the point where we're between pages.
	 *
	 * If caller's tuple is also the page's finaltup, and we see that required
	 * scan keys still aren't satisfied, start a new primitive index scan.
	 */
	if (!all_required_satisfied && pstate->finaltup == tuple)
		goto new_prim_scan;

	/*
	 * Proactively check finaltup (don't wait until finaltup is reached by the
	 * scan) when it might well turn out to not be satisfied later on.
	 *
	 * Note: if so->scanBehind hasn't already been set for finaltup by us,
	 * it'll be set during this call to _bt_tuple_before_array_skeys.  Either
	 * way, it'll be set correctly (for the whole page) after this point.
	 */
	if (!all_required_satisfied && pstate->finaltup &&
		_bt_tuple_before_array_skeys(scan, dir, pstate->finaltup, tupdesc,
									 BTreeTupleGetNAtts(pstate->finaltup, rel),
									 false, 0, &so->scanBehind))
		goto new_prim_scan;

	/*
	 * When we encounter a truncated finaltup high key attribute, we're
	 * optimistic about the chances of its corresponding required scan key
	 * being satisfied when we go on to recheck it against tuples from this
	 * page's right sibling leaf page.  We consider truncated attributes to be
	 * satisfied by required scan keys, which allows the primitive index scan
	 * to continue to the next leaf page.  We must set so->scanBehind to true
	 * to remember that the last page's finaltup had "satisfied" required scan
	 * keys for one or more truncated attribute values (scan keys required in
	 * _either_ scan direction).
	 *
	 * There is a chance that _bt_readpage (which checks so->scanBehind) will
	 * find that even the sibling leaf page's finaltup is < the new array
	 * keys.  When that happens, our optimistic policy will have incurred a
	 * single extra leaf page access that could have been avoided.
	 *
	 * A pessimistic policy would give backward scans a gratuitous advantage
	 * over forward scans.  We'd punish forward scans for applying more
	 * accurate information from the high key, rather than just using the
	 * final non-pivot tuple as finaltup, in the style of backward scans.
	 * Being pessimistic would also give some scans with non-required arrays a
	 * perverse advantage over similar scans that use required arrays instead.
	 *
	 * This is similar to our scan-level heuristics, below.  They also set
	 * scanBehind to speculatively continue the primscan onto the next page.
	 */
	if (so->scanBehind)
	{
		/* Truncated high key -- _bt_scanbehind_checkkeys recheck scheduled */
	}

	/*
	 * Handle inequalities marked required in the opposite scan direction.
	 * They can also signal that we should start a new primitive index scan.
	 *
	 * It's possible that the scan is now positioned where "matching" tuples
	 * begin, and that caller's tuple satisfies all scan keys required in the
	 * current scan direction.  But if caller's tuple still doesn't satisfy
	 * other scan keys that are required in the opposite scan direction only
	 * (e.g., a required >= strategy scan key when scan direction is forward),
	 * it's still possible that there are many leaf pages before the page that
	 * _bt_first could skip straight to.  Groveling through all those pages
	 * will always give correct answers, but it can be very inefficient.  We
	 * must avoid needlessly scanning extra pages.
	 *
	 * Separately, it's possible that _bt_check_compare set continuescan=false
	 * for a scan key that's required in the opposite direction only.  This is
	 * a special case, that happens only when _bt_check_compare sees that the
	 * inequality encountered a NULL value.  This signals the end of non-NULL
	 * values in the current scan direction, which is reason enough to end the
	 * (primitive) scan.  If this happens at the start of a large group of
	 * NULL values, then we shouldn't expect to be called again until after
	 * the scan has already read indefinitely-many leaf pages full of tuples
	 * with NULL suffix values.  (_bt_first is expected to skip over the group
	 * of NULLs by applying a similar "deduce NOT NULL" rule of its own, which
	 * involves consing up an explicit SK_SEARCHNOTNULL key.)
	 *
	 * Apply a test against finaltup to detect and recover from the problem:
	 * if even finaltup doesn't satisfy such an inequality, we just skip by
	 * starting a new primitive index scan.  When we skip, we know for sure
	 * that all of the tuples on the current page following caller's tuple are
	 * also before the _bt_first-wise start of tuples for our new qual.  That
	 * at least suggests many more skippable pages beyond the current page.
	 * (when so->scanBehind and so->oppositeDirCheck are set, this'll happen
	 * when we test the next page's finaltup/high key instead.)
	 */
	else if (has_required_opposite_direction_only && pstate->finaltup &&
			 unlikely(!_bt_oppodir_checkkeys(scan, dir, pstate->finaltup)))
		goto new_prim_scan;

continue_scan:

	/*
	 * Stick with the ongoing primitive index scan for now.
	 *
	 * It's possible that later tuples will also turn out to have values that
	 * are still < the now-current array keys (or > the current array keys).
	 * Our caller will handle this by performing what amounts to a linear
	 * search of the page, implemented by calling _bt_check_compare and then
	 * _bt_tuple_before_array_skeys for each tuple.
	 *
	 * This approach has various advantages over a binary search of the page.
	 * Repeated binary searches of the page (one binary search for every array
	 * advancement) won't outperform a continuous linear search.  While there
	 * are workloads that a naive linear search won't handle well, our caller
	 * has a "look ahead" fallback mechanism to deal with that problem.
	 */
	pstate->continuescan = true;	/* Override _bt_check_compare */
	so->needPrimScan = false;	/* _bt_readpage has more tuples to check */

	if (so->scanBehind)
	{
		/*
		 * Remember if recheck needs to call _bt_oppodir_checkkeys for next
		 * page's finaltup (see above comments about "Handle inequalities
		 * marked required in the opposite scan direction" for why).
		 */
		so->oppositeDirCheck = has_required_opposite_direction_only;

		/*
		 * skip by setting "look ahead" mechanism's offnum for forwards scans
		 * (backwards scans check scanBehind flag directly instead)
		 */
		if (ScanDirectionIsForward(dir))
			pstate->skip = pstate->maxoff + 1;
	}

	/* Caller's tuple doesn't match the new qual */
	return false;

new_prim_scan:

	Assert(pstate->finaltup);	/* not on rightmost/leftmost page */

	/*
	 * Looks like another primitive index scan is required.  But consider
	 * continuing the current primscan based on scan-level heuristics.
	 *
	 * Continue the ongoing primitive scan (and schedule a recheck for when
	 * the scan arrives on the next sibling leaf page) when it has already
	 * read at least one leaf page before the one we're reading now.  This
	 * makes primscan scheduling more efficient when scanning subsets of an
	 * index with many distinct attribute values matching many array elements.
	 * It encourages fewer, larger primitive scans where that makes sense.
	 * This will in turn encourage _bt_readpage to apply the pstate.startikey
	 * optimization more often.
	 *
	 * Also continue the ongoing primitive index scan when it is still on the
	 * first page if there have been more than NSKIPADVANCES_THRESHOLD calls
	 * here that each advanced at least one of the scan's skip arrays
	 * (deliberately ignore advancements that only affected SAOP arrays here).
	 * A page that cycles through this many skip array elements is quite
	 * likely to neighbor similar pages, that we'll also need to read.
	 *
	 * Note: These heuristics aren't as aggressive as you might think.  We're
	 * conservative about allowing a primitive scan to step from the first
	 * leaf page it reads to the page's sibling page (we only allow it on
	 * first pages whose finaltup strongly suggests that it'll work out, as
	 * well as first pages that have a large number of skip array advances).
	 * Clearing this first page finaltup hurdle is a strong signal in itself.
	 *
	 * Note: The NSKIPADVANCES_THRESHOLD heuristic exists only to avoid
	 * pathological cases.  Specifically, cases where a skip scan should just
	 * behave like a traditional full index scan, but ends up "skipping" again
	 * and again, descending to the prior leaf page's direct sibling leaf page
	 * each time.  This misbehavior would otherwise be possible during scans
	 * that never quite manage to "clear the first page finaltup hurdle".
	 */
	if (!pstate->firstpage || pstate->nskipadvances > NSKIPADVANCES_THRESHOLD)
	{
		/* Schedule a recheck once on the next (or previous) page */
		so->scanBehind = true;

		/* Continue the current primitive scan after all */
		goto continue_scan;
	}

	/*
	 * End this primitive index scan, but schedule another.
	 *
	 * Note: We make a soft assumption that the current scan direction will
	 * also be used within _bt_next, when it is asked to step off this page.
	 * It is up to _bt_next to cancel this scheduled primitive index scan
	 * whenever it steps to a page in the direction opposite currPos.dir.
	 */
	pstate->continuescan = false;	/* Tell _bt_readpage we're done... */
	so->needPrimScan = true;	/* ...but call _bt_first again */

	if (scan->parallel_scan)
		_bt_parallel_primscan_schedule(scan, so->currPos.currPage);

	/* Caller's tuple doesn't match the new qual */
	return false;

end_toplevel_scan:

	/*
	 * End the current primitive index scan, but don't schedule another.
	 *
	 * This ends the entire top-level scan in the current scan direction.
	 *
	 * Note: The scan's arrays (including any non-required arrays) are now in
	 * their final positions for the current scan direction.  If the scan
	 * direction happens to change, then the arrays will already be in their
	 * first positions for what will then be the current scan direction.
	 */
	pstate->continuescan = false;	/* Tell _bt_readpage we're done... */
	so->needPrimScan = false;	/* ...and don't call _bt_first again */

	/* Caller's tuple doesn't match any qual */
	return false;
}

/*
 * _bt_advance_array_keys_increment() -- Advance to next set of array elements
 *
 * Advances the array keys by a single increment in the current scan
 * direction.  When there are multiple array keys this can roll over from the
 * lowest order array to higher order arrays.
 *
 * Returns true if there is another set of values to consider, false if not.
 * On true result, the scankeys are initialized with the next set of values.
 * On false result, the scankeys stay the same, and the array keys are not
 * advanced (every array remains at its final element for scan direction).
 */
static bool
_bt_advance_array_keys_increment(IndexScanDesc scan, ScanDirection dir,
								 bool *skip_array_set)
{
	Relation	rel = scan->indexRelation;
	BTScanOpaque so = (BTScanOpaque) scan->opaque;

	/*
	 * We must advance the last array key most quickly, since it will
	 * correspond to the lowest-order index column among the available
	 * qualifications
	 */
	for (int i = so->numArrayKeys - 1; i >= 0; i--)
	{
		BTArrayKeyInfo *array = &so->arrayKeys[i];
		ScanKey		skey = &so->keyData[array->scan_key];

		if (array->num_elems == -1)
			*skip_array_set = true;

		if (ScanDirectionIsForward(dir))
		{
			if (_bt_array_increment(rel, skey, array))
				return true;
		}
		else
		{
			if (_bt_array_decrement(rel, skey, array))
				return true;
		}

		/*
		 * Couldn't increment (or decrement) array.  Handle array roll over.
		 *
		 * Start over at the array's lowest sorting value (or its highest
		 * value, for backward scans)...
		 */
		_bt_array_set_low_or_high(rel, skey, array,
								  ScanDirectionIsForward(dir));

		/* ...then increment (or decrement) next most significant array */
	}

	/*
	 * The array keys are now exhausted.
	 *
	 * Restore the array keys to the state they were in immediately before we
	 * were called.  This ensures that the arrays only ever ratchet in the
	 * current scan direction.
	 *
	 * Without this, scans could overlook matching tuples when the scan
	 * direction gets reversed just before btgettuple runs out of items to
	 * return, but just after _bt_readpage prepares all the items from the
	 * scan's final page in so->currPos.  When we're on the final page it is
	 * typical for so->currPos to get invalidated once btgettuple finally
	 * returns false, which'll effectively invalidate the scan's array keys.
	 * That hasn't happened yet, though -- and in general it may never happen.
	 */
	_bt_start_array_keys(scan, -dir);

	return false;
}

/*
 * _bt_array_increment() -- increment array scan key's sk_argument
 *
 * Return value indicates whether caller's array was successfully incremented.
 * Cannot increment an array whose current element is already the final one.
 */
static bool
_bt_array_increment(Relation rel, ScanKey skey, BTArrayKeyInfo *array)
{
	bool		oflow = false;
	Datum		inc_sk_argument;

	Assert(skey->sk_flags & SK_SEARCHARRAY);
	Assert(!(skey->sk_flags & (SK_BT_MINVAL | SK_BT_NEXT | SK_BT_PRIOR)));

	/* SAOP array? */
	if (array->num_elems != -1)
	{
		Assert(!(skey->sk_flags & (SK_BT_SKIP | SK_BT_MINVAL | SK_BT_MAXVAL)));
		if (array->cur_elem < array->num_elems - 1)
		{
			/*
			 * Just increment current element, and assign its datum to skey
			 * (only skip arrays need us to free existing sk_argument memory)
			 */
			array->cur_elem++;
			skey->sk_argument = array->elem_values[array->cur_elem];

			/* Successfully incremented array */
			return true;
		}

		/* Cannot increment past final array element */
		return false;
	}

	/* Nope, this is a skip array */
	Assert(skey->sk_flags & SK_BT_SKIP);

	/*
	 * The sentinel value that represents the maximum value within the range
	 * of a skip array (often just +inf) is never incrementable
	 */
	if (skey->sk_flags & SK_BT_MAXVAL)
		return false;

	/*
	 * When the current array element is NULL, and the highest sorting value
	 * in the index is also NULL, we cannot increment past the final element
	 */
	if ((skey->sk_flags & SK_ISNULL) && !(skey->sk_flags & SK_BT_NULLS_FIRST))
		return false;

	/*
	 * Opclasses without skip support "increment" the scan key's current
	 * element by setting the NEXT flag.  The true next value is determined by
	 * repositioning to the first index tuple > existing sk_argument/current
	 * array element.  Note that this works in the usual way when the scan key
	 * is already marked ISNULL (i.e. when the current element is NULL).
	 */
	if (!array->sksup)
	{
		/* Successfully "incremented" array */
		skey->sk_flags |= SK_BT_NEXT;
		return true;
	}

	/*
	 * Opclasses with skip support directly increment sk_argument
	 */
	if (skey->sk_flags & SK_ISNULL)
	{
		Assert(skey->sk_flags & SK_BT_NULLS_FIRST);

		/*
		 * Existing sk_argument/array element is NULL (for an IS NULL qual).
		 *
		 * "Increment" from NULL to the low_elem value provided by opclass
		 * skip support routine.
		 */
		skey->sk_flags &= ~(SK_SEARCHNULL | SK_ISNULL);
		skey->sk_argument = datumCopy(array->sksup->low_elem,
									  array->attbyval, array->attlen);
		return true;
	}

	/*
	 * Ask opclass support routine to provide incremented copy of existing
	 * non-NULL sk_argument
	 */
	inc_sk_argument = array->sksup->increment(rel, skey->sk_argument, &oflow);
	if (unlikely(oflow))
	{
		/* inc_sk_argument has undefined value (so no pfree) */
		if (array->null_elem && !(skey->sk_flags & SK_BT_NULLS_FIRST))
		{
			_bt_skiparray_set_isnull(rel, skey, array);

			/* Successfully "incremented" array to NULL */
			return true;
		}

		/* Cannot increment past final array element */
		return false;
	}

	/*
	 * Successfully incremented sk_argument to a non-NULL value.  Make sure
	 * that the incremented value is still within the range of the array.
	 */
	if (array->high_compare &&
		!DatumGetBool(FunctionCall2Coll(&array->high_compare->sk_func,
										array->high_compare->sk_collation,
										inc_sk_argument,
										array->high_compare->sk_argument)))
	{
		/* Keep existing sk_argument after all */
		if (!array->attbyval)
			pfree(DatumGetPointer(inc_sk_argument));

		/* Cannot increment past final array element */
		return false;
	}

	/* Accept value returned by opclass increment callback */
	if (!array->attbyval && skey->sk_argument)
		pfree(DatumGetPointer(skey->sk_argument));
	skey->sk_argument = inc_sk_argument;

	/* Successfully incremented array */
	return true;
}

/*
 * _bt_array_decrement() -- decrement array scan key's sk_argument
 *
 * Return value indicates whether caller's array was successfully decremented.
 * Cannot decrement an array whose current element is already the first one.
 */
static bool
_bt_array_decrement(Relation rel, ScanKey skey, BTArrayKeyInfo *array)
{
	bool		uflow = false;
	Datum		dec_sk_argument;

	Assert(skey->sk_flags & SK_SEARCHARRAY);
	Assert(!(skey->sk_flags & (SK_BT_MAXVAL | SK_BT_NEXT | SK_BT_PRIOR)));

	/* SAOP array? */
	if (array->num_elems != -1)
	{
		Assert(!(skey->sk_flags & (SK_BT_SKIP | SK_BT_MINVAL | SK_BT_MAXVAL)));
		if (array->cur_elem > 0)
		{
			/*
			 * Just decrement current element, and assign its datum to skey
			 * (only skip arrays need us to free existing sk_argument memory)
			 */
			array->cur_elem--;
			skey->sk_argument = array->elem_values[array->cur_elem];

			/* Successfully decremented array */
			return true;
		}

		/* Cannot decrement to before first array element */
		return false;
	}

	/* Nope, this is a skip array */
	Assert(skey->sk_flags & SK_BT_SKIP);

	/*
	 * The sentinel value that represents the minimum value within the range
	 * of a skip array (often just -inf) is never decrementable
	 */
	if (skey->sk_flags & SK_BT_MINVAL)
		return false;

	/*
	 * When the current array element is NULL, and the lowest sorting value in
	 * the index is also NULL, we cannot decrement before first array element
	 */
	if ((skey->sk_flags & SK_ISNULL) && (skey->sk_flags & SK_BT_NULLS_FIRST))
		return false;

	/*
	 * Opclasses without skip support "decrement" the scan key's current
	 * element by setting the PRIOR flag.  The true prior value is determined
	 * by repositioning to the last index tuple < existing sk_argument/current
	 * array element.  Note that this works in the usual way when the scan key
	 * is already marked ISNULL (i.e. when the current element is NULL).
	 */
	if (!array->sksup)
	{
		/* Successfully "decremented" array */
		skey->sk_flags |= SK_BT_PRIOR;
		return true;
	}

	/*
	 * Opclasses with skip support directly decrement sk_argument
	 */
	if (skey->sk_flags & SK_ISNULL)
	{
		Assert(!(skey->sk_flags & SK_BT_NULLS_FIRST));

		/*
		 * Existing sk_argument/array element is NULL (for an IS NULL qual).
		 *
		 * "Decrement" from NULL to the high_elem value provided by opclass
		 * skip support routine.
		 */
		skey->sk_flags &= ~(SK_SEARCHNULL | SK_ISNULL);
		skey->sk_argument = datumCopy(array->sksup->high_elem,
									  array->attbyval, array->attlen);
		return true;
	}

	/*
	 * Ask opclass support routine to provide decremented copy of existing
	 * non-NULL sk_argument
	 */
	dec_sk_argument = array->sksup->decrement(rel, skey->sk_argument, &uflow);
	if (unlikely(uflow))
	{
		/* dec_sk_argument has undefined value (so no pfree) */
		if (array->null_elem && (skey->sk_flags & SK_BT_NULLS_FIRST))
		{
			_bt_skiparray_set_isnull(rel, skey, array);

			/* Successfully "decremented" array to NULL */
			return true;
		}

		/* Cannot decrement to before first array element */
		return false;
	}

	/*
	 * Successfully decremented sk_argument to a non-NULL value.  Make sure
	 * that the decremented value is still within the range of the array.
	 */
	if (array->low_compare &&
		!DatumGetBool(FunctionCall2Coll(&array->low_compare->sk_func,
										array->low_compare->sk_collation,
										dec_sk_argument,
										array->low_compare->sk_argument)))
	{
		/* Keep existing sk_argument after all */
		if (!array->attbyval)
			pfree(DatumGetPointer(dec_sk_argument));

		/* Cannot decrement to before first array element */
		return false;
	}

	/* Accept value returned by opclass decrement callback */
	if (!array->attbyval && skey->sk_argument)
		pfree(DatumGetPointer(skey->sk_argument));
	skey->sk_argument = dec_sk_argument;

	/* Successfully decremented array */
	return true;
}

/*
 * _bt_array_set_low_or_high() -- Set array scan key to lowest/highest element
 *
 * Caller also passes associated scan key, which will have its argument set to
 * the lowest/highest array value in passing.
 */
static void
_bt_array_set_low_or_high(Relation rel, ScanKey skey, BTArrayKeyInfo *array,
						  bool low_not_high)
{
	Assert(skey->sk_flags & SK_SEARCHARRAY);

	if (array->num_elems != -1)
	{
		/* set low or high element for SAOP array */
		int			set_elem = 0;

		Assert(!(skey->sk_flags & SK_BT_SKIP));

		if (!low_not_high)
			set_elem = array->num_elems - 1;

		/*
		 * Just copy over array datum (only skip arrays require freeing and
		 * allocating memory for sk_argument)
		 */
		array->cur_elem = set_elem;
		skey->sk_argument = array->elem_values[set_elem];

		return;
	}

	/* set low or high element for skip array */
	Assert(skey->sk_flags & SK_BT_SKIP);
	Assert(array->num_elems == -1);

	/* Free memory previously allocated for sk_argument if needed */
	if (!array->attbyval && skey->sk_argument)
		pfree(DatumGetPointer(skey->sk_argument));

	/* Reset flags */
	skey->sk_argument = (Datum) 0;
	skey->sk_flags &= ~(SK_SEARCHNULL | SK_ISNULL |
						SK_BT_MINVAL | SK_BT_MAXVAL |
						SK_BT_NEXT | SK_BT_PRIOR);

	if (array->null_elem &&
		(low_not_high == ((skey->sk_flags & SK_BT_NULLS_FIRST) != 0)))
	{
		/* Requested element (either lowest or highest) has the value NULL */
		skey->sk_flags |= (SK_SEARCHNULL | SK_ISNULL);
	}
	else if (low_not_high)
	{
		/* Setting array to lowest element (according to low_compare) */
		skey->sk_flags |= SK_BT_MINVAL;
	}
	else
	{
		/* Setting array to highest element (according to high_compare) */
		skey->sk_flags |= SK_BT_MAXVAL;
	}
}

/*
 * _bt_skiparray_set_element() -- Set skip array scan key's sk_argument
 *
 * Caller passes set_elem_result returned by _bt_binsrch_skiparray_skey for
 * caller's tupdatum/tupnull.
 *
 * We copy tupdatum/tupnull into skey's sk_argument iff set_elem_result == 0.
 * Otherwise, we set skey to either the lowest or highest value that's within
 * the range of caller's skip array (whichever is the best available match to
 * tupdatum/tupnull that is still within the range of the skip array according
 * to _bt_binsrch_skiparray_skey/set_elem_result).
 */
static void
_bt_skiparray_set_element(Relation rel, ScanKey skey, BTArrayKeyInfo *array,
						  int32 set_elem_result, Datum tupdatum, bool tupnull)
{
	Assert(skey->sk_flags & SK_BT_SKIP);
	Assert(skey->sk_flags & SK_SEARCHARRAY);

	if (set_elem_result)
	{
		/* tupdatum/tupnull is out of the range of the skip array */
		Assert(!array->null_elem);

		_bt_array_set_low_or_high(rel, skey, array, set_elem_result < 0);
		return;
	}

	/* Advance skip array to tupdatum (or tupnull) value */
	if (unlikely(tupnull))
	{
		_bt_skiparray_set_isnull(rel, skey, array);
		return;
	}

	/* Free memory previously allocated for sk_argument if needed */
	if (!array->attbyval && skey->sk_argument)
		pfree(DatumGetPointer(skey->sk_argument));

	/* tupdatum becomes new sk_argument/new current element */
	skey->sk_flags &= ~(SK_SEARCHNULL | SK_ISNULL |
						SK_BT_MINVAL | SK_BT_MAXVAL |
						SK_BT_NEXT | SK_BT_PRIOR);
	skey->sk_argument = datumCopy(tupdatum, array->attbyval, array->attlen);
}

/*
 * _bt_skiparray_set_isnull() -- set skip array scan key to NULL
 */
static void
_bt_skiparray_set_isnull(Relation rel, ScanKey skey, BTArrayKeyInfo *array)
{
	Assert(skey->sk_flags & SK_BT_SKIP);
	Assert(skey->sk_flags & SK_SEARCHARRAY);
	Assert(array->null_elem && !array->low_compare && !array->high_compare);

	/* Free memory previously allocated for sk_argument if needed */
	if (!array->attbyval && skey->sk_argument)
		pfree(DatumGetPointer(skey->sk_argument));

	/* NULL becomes new sk_argument/new current element */
	skey->sk_argument = (Datum) 0;
	skey->sk_flags &= ~(SK_BT_MINVAL | SK_BT_MAXVAL |
						SK_BT_NEXT | SK_BT_PRIOR);
	skey->sk_flags |= (SK_SEARCHNULL | SK_ISNULL);
}

/*
 * _bt_compare_array_skey() -- apply array comparison function
 *
 * Compares caller's tuple attribute value to a scan key/array element.
 * Helper function used during binary searches of SK_SEARCHARRAY arrays.
 *
 *		This routine returns:
 *			<0 if tupdatum < arrdatum;
 *			 0 if tupdatum == arrdatum;
 *			>0 if tupdatum > arrdatum.
 *
 * This is essentially the same interface as _bt_compare: both functions
 * compare the value that they're searching for to a binary search pivot.
 * However, unlike _bt_compare, this function's "tuple argument" comes first,
 * while its "array/scankey argument" comes second.
*/
static inline int32
_bt_compare_array_skey(FmgrInfo *orderproc,
					   Datum tupdatum, bool tupnull,
					   Datum arrdatum, ScanKey cur)
{
	int32		result = 0;

	Assert(cur->sk_strategy == BTEqualStrategyNumber);
	Assert(!(cur->sk_flags & (SK_BT_MINVAL | SK_BT_MAXVAL)));

	if (tupnull)				/* NULL tupdatum */
	{
		if (cur->sk_flags & SK_ISNULL)
			result = 0;			/* NULL "=" NULL */
		else if (cur->sk_flags & SK_BT_NULLS_FIRST)
			result = -1;		/* NULL "<" NOT_NULL */
		else
			result = 1;			/* NULL ">" NOT_NULL */
	}
	else if (cur->sk_flags & SK_ISNULL) /* NOT_NULL tupdatum, NULL arrdatum */
	{
		if (cur->sk_flags & SK_BT_NULLS_FIRST)
			result = 1;			/* NOT_NULL ">" NULL */
		else
			result = -1;		/* NOT_NULL "<" NULL */
	}
	else
	{
		/*
		 * Like _bt_compare, we need to be careful of cross-type comparisons,
		 * so the left value has to be the value that came from an index tuple
		 */
		result = DatumGetInt32(FunctionCall2Coll(orderproc, cur->sk_collation,
												 tupdatum, arrdatum));

		/*
		 * We flip the sign by following the obvious rule: flip whenever the
		 * column is a DESC column.
		 *
		 * _bt_compare does it the wrong way around (flip when *ASC*) in order
		 * to compensate for passing its orderproc arguments backwards.  We
		 * don't need to play these games because we find it natural to pass
		 * tupdatum as the left value (and arrdatum as the right value).
		 */
		if (cur->sk_flags & SK_BT_DESC)
			INVERT_COMPARE_RESULT(result);
	}

	return result;
}

/*
 * _bt_binsrch_array_skey() -- Binary search for next matching array key
 *
 * Returns an index to the first array element >= caller's tupdatum argument.
 * This convention is more natural for forwards scan callers, but that can't
 * really matter to backwards scan callers.  Both callers require handling for
 * the case where the match we return is < tupdatum, and symmetric handling
 * for the case where our best match is > tupdatum.
 *
 * Also sets *set_elem_result to the result _bt_compare_array_skey returned
 * when we used it to compare the matching array element to tupdatum/tupnull.
 *
 * cur_elem_trig indicates if array advancement was triggered by this array's
 * scan key, and that the array is for a required scan key.  We can apply this
 * information to find the next matching array element in the current scan
 * direction using far fewer comparisons (fewer on average, compared to naive
 * binary search).  This scheme takes advantage of an important property of
 * required arrays: required arrays always advance in lockstep with the index
 * scan's progress through the index's key space.
 */
int
_bt_binsrch_array_skey(FmgrInfo *orderproc,
					   bool cur_elem_trig, ScanDirection dir,
					   Datum tupdatum, bool tupnull,
					   BTArrayKeyInfo *array, ScanKey cur,
					   int32 *set_elem_result)
{
	int			low_elem = 0,
				mid_elem = -1,
				high_elem = array->num_elems - 1,
				result = 0;
	Datum		arrdatum;

	Assert(cur->sk_flags & SK_SEARCHARRAY);
	Assert(!(cur->sk_flags & SK_BT_SKIP));
	Assert(!(cur->sk_flags & SK_ISNULL));	/* SAOP arrays never have NULLs */
	Assert(cur->sk_strategy == BTEqualStrategyNumber);

	if (cur_elem_trig)
	{
		Assert(!ScanDirectionIsNoMovement(dir));
		Assert(cur->sk_flags & SK_BT_REQFWD);

		/*
		 * When the scan key that triggered array advancement is a required
		 * array scan key, it is now certain that the current array element
		 * (plus all prior elements relative to the current scan direction)
		 * cannot possibly be at or ahead of the corresponding tuple value.
		 * (_bt_checkkeys must have called _bt_tuple_before_array_skeys, which
		 * makes sure this is true as a condition of advancing the arrays.)
		 *
		 * This makes it safe to exclude array elements up to and including
		 * the former-current array element from our search.
		 *
		 * Separately, when array advancement was triggered by a required scan
		 * key, the array element immediately after the former-current element
		 * is often either an exact tupdatum match, or a "close by" near-match
		 * (a near-match tupdatum is one whose key space falls _between_ the
		 * former-current and new-current array elements).  We'll detect both
		 * cases via an optimistic comparison of the new search lower bound
		 * (or new search upper bound in the case of backwards scans).
		 */
		if (ScanDirectionIsForward(dir))
		{
			low_elem = array->cur_elem + 1; /* old cur_elem exhausted */

			/* Compare prospective new cur_elem (also the new lower bound) */
			if (high_elem >= low_elem)
			{
				arrdatum = array->elem_values[low_elem];
				result = _bt_compare_array_skey(orderproc, tupdatum, tupnull,
												arrdatum, cur);

				if (result <= 0)
				{
					/* Optimistic comparison optimization worked out */
					*set_elem_result = result;
					return low_elem;
				}
				mid_elem = low_elem;
				low_elem++;		/* this cur_elem exhausted, too */
			}

			if (high_elem < low_elem)
			{
				/* Caller needs to perform "beyond end" array advancement */
				*set_elem_result = 1;
				return high_elem;
			}
		}
		else
		{
			high_elem = array->cur_elem - 1;	/* old cur_elem exhausted */

			/* Compare prospective new cur_elem (also the new upper bound) */
			if (high_elem >= low_elem)
			{
				arrdatum = array->elem_values[high_elem];
				result = _bt_compare_array_skey(orderproc, tupdatum, tupnull,
												arrdatum, cur);

				if (result >= 0)
				{
					/* Optimistic comparison optimization worked out */
					*set_elem_result = result;
					return high_elem;
				}
				mid_elem = high_elem;
				high_elem--;	/* this cur_elem exhausted, too */
			}

			if (high_elem < low_elem)
			{
				/* Caller needs to perform "beyond end" array advancement */
				*set_elem_result = -1;
				return low_elem;
			}
		}
	}

	while (high_elem > low_elem)
	{
		mid_elem = low_elem + ((high_elem - low_elem) / 2);
		arrdatum = array->elem_values[mid_elem];

		result = _bt_compare_array_skey(orderproc, tupdatum, tupnull,
										arrdatum, cur);

		if (result == 0)
		{
			/*
			 * It's safe to quit as soon as we see an equal array element.
			 * This often saves an extra comparison or two...
			 */
			low_elem = mid_elem;
			break;
		}

		if (result > 0)
			low_elem = mid_elem + 1;
		else
			high_elem = mid_elem;
	}

	/*
	 * ...but our caller also cares about how its searched-for tuple datum
	 * compares to the low_elem datum.  Must always set *set_elem_result with
	 * the result of that comparison specifically.
	 */
	if (low_elem != mid_elem)
		result = _bt_compare_array_skey(orderproc, tupdatum, tupnull,
										array->elem_values[low_elem], cur);

	*set_elem_result = result;

	return low_elem;
}

/*
 * _bt_binsrch_skiparray_skey() -- "Binary search" within a skip array
 *
 * Does not return an index into the array, since skip arrays don't really
 * contain elements (they generate their array elements procedurally instead).
 * Our interface matches that of _bt_binsrch_array_skey in every other way.
 *
 * Sets *set_elem_result just like _bt_binsrch_array_skey would with a true
 * array.  The value 0 indicates that tupdatum/tupnull is within the range of
 * the skip array.  We return -1 when tupdatum/tupnull is lower that any value
 * within the range of the array, and 1 when it is higher than every value.
 * Caller should pass *set_elem_result to _bt_skiparray_set_element to advance
 * the array.
 *
 * cur_elem_trig indicates if array advancement was triggered by this array's
 * scan key.  We use this to optimize-away comparisons that are known by our
 * caller to be unnecessary from context, just like _bt_binsrch_array_skey.
 */
static void
_bt_binsrch_skiparray_skey(bool cur_elem_trig, ScanDirection dir,
						   Datum tupdatum, bool tupnull,
						   BTArrayKeyInfo *array, ScanKey cur,
						   int32 *set_elem_result)
{
	Assert(cur->sk_flags & SK_BT_SKIP);
	Assert(cur->sk_flags & SK_SEARCHARRAY);
	Assert(cur->sk_flags & SK_BT_REQFWD);
	Assert(array->num_elems == -1);
	Assert(!ScanDirectionIsNoMovement(dir));

	if (array->null_elem)
	{
		Assert(!array->low_compare && !array->high_compare);

		*set_elem_result = 0;
		return;
	}

	if (tupnull)				/* NULL tupdatum */
	{
		if (cur->sk_flags & SK_BT_NULLS_FIRST)
			*set_elem_result = -1;	/* NULL "<" NOT_NULL */
		else
			*set_elem_result = 1;	/* NULL ">" NOT_NULL */
		return;
	}

	/*
	 * Array inequalities determine whether tupdatum is within the range of
	 * caller's skip array
	 */
	*set_elem_result = 0;
	if (ScanDirectionIsForward(dir))
	{
		/*
		 * Evaluate low_compare first (unless cur_elem_trig tells us that it
		 * cannot possibly fail to be satisfied), then evaluate high_compare
		 */
		if (!cur_elem_trig && array->low_compare &&
			!DatumGetBool(FunctionCall2Coll(&array->low_compare->sk_func,
											array->low_compare->sk_collation,
											tupdatum,
											array->low_compare->sk_argument)))
			*set_elem_result = -1;
		else if (array->high_compare &&
				 !DatumGetBool(FunctionCall2Coll(&array->high_compare->sk_func,
												 array->high_compare->sk_collation,
												 tupdatum,
												 array->high_compare->sk_argument)))
			*set_elem_result = 1;
	}
	else
	{
		/*
		 * Evaluate high_compare first (unless cur_elem_trig tells us that it
		 * cannot possibly fail to be satisfied), then evaluate low_compare
		 */
		if (!cur_elem_trig && array->high_compare &&
			!DatumGetBool(FunctionCall2Coll(&array->high_compare->sk_func,
											array->high_compare->sk_collation,
											tupdatum,
											array->high_compare->sk_argument)))
			*set_elem_result = 1;
		else if (array->low_compare &&
				 !DatumGetBool(FunctionCall2Coll(&array->low_compare->sk_func,
												 array->low_compare->sk_collation,
												 tupdatum,
												 array->low_compare->sk_argument)))
			*set_elem_result = -1;
	}

	/*
	 * Assert that any keys that were assumed to be satisfied already (due to
	 * caller passing cur_elem_trig=true) really are satisfied as expected
	 */
#ifdef USE_ASSERT_CHECKING
	if (cur_elem_trig)
	{
		if (ScanDirectionIsForward(dir) && array->low_compare)
			Assert(DatumGetBool(FunctionCall2Coll(&array->low_compare->sk_func,
												  array->low_compare->sk_collation,
												  tupdatum,
												  array->low_compare->sk_argument)));

		if (ScanDirectionIsBackward(dir) && array->high_compare)
			Assert(DatumGetBool(FunctionCall2Coll(&array->high_compare->sk_func,
												  array->high_compare->sk_collation,
												  tupdatum,
												  array->high_compare->sk_argument)));
	}
#endif
}

#ifdef USE_ASSERT_CHECKING
/*
 * Verify that the scan's "so->keyData[]" scan keys are in agreement with
 * its array key state
 */
static bool
_bt_verify_keys_with_arraykeys(IndexScanDesc scan)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	int			last_sk_attno = InvalidAttrNumber,
				arrayidx = 0;
	bool		nonrequiredseen = false;

	if (!so->qual_ok)
		return false;

	for (int ikey = 0; ikey < so->numberOfKeys; ikey++)
	{
		ScanKey		cur = so->keyData + ikey;
		BTArrayKeyInfo *array;

		if (cur->sk_strategy != BTEqualStrategyNumber ||
			!(cur->sk_flags & SK_SEARCHARRAY))
			continue;

		array = &so->arrayKeys[arrayidx++];
		if (array->scan_key != ikey)
			return false;

		if (array->num_elems == 0 || array->num_elems < -1)
			return false;

		if (array->num_elems != -1 &&
			cur->sk_argument != array->elem_values[array->cur_elem])
			return false;
		if (cur->sk_flags & (SK_BT_REQFWD | SK_BT_REQBKWD))
		{
			if (last_sk_attno > cur->sk_attno)
				return false;
			if (nonrequiredseen)
				return false;
		}
		else
			nonrequiredseen = true;

		last_sk_attno = cur->sk_attno;
	}

	if (arrayidx != so->numArrayKeys)
		return false;

	return true;
}
#endif
