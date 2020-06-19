/*-------------------------------------------------------------------------
 *
 * nbtdedup.c
 *	  Deduplicate items in Postgres btrees.
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/nbtree/nbtdedup.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/nbtree.h"
#include "access/nbtxlog.h"
#include "miscadmin.h"
#include "utils/rel.h"

static bool _bt_do_singleval(Relation rel, Page page, BTDedupState state,
							 OffsetNumber minoff, IndexTuple newitem);
static void _bt_singleval_fillfactor(Page page, BTDedupState state,
									 Size newitemsz);
#ifdef USE_ASSERT_CHECKING
static bool _bt_posting_valid(IndexTuple posting);
#endif

/*
 * Deduplicate items on a leaf page.  The page will have to be split by caller
 * if we cannot successfully free at least newitemsz (we also need space for
 * newitem's line pointer, which isn't included in caller's newitemsz).
 *
 * The general approach taken here is to perform as much deduplication as
 * possible to free as much space as possible.  Note, however, that "single
 * value" strategy is sometimes used for !checkingunique callers, in which
 * case deduplication will leave a few tuples untouched at the end of the
 * page.  The general idea is to prepare the page for an anticipated page
 * split that uses nbtsplitloc.c's "single value" strategy to determine a
 * split point.  (There is no reason to deduplicate items that will end up on
 * the right half of the page after the anticipated page split; better to
 * handle those if and when the anticipated right half page gets its own
 * deduplication pass, following further inserts of duplicates.)
 *
 * This function should be called during insertion, when the page doesn't have
 * enough space to fit an incoming newitem.  If the BTP_HAS_GARBAGE page flag
 * was set, caller should have removed any LP_DEAD items by calling
 * _bt_vacuum_one_page() before calling here.  We may still have to kill
 * LP_DEAD items here when the page's BTP_HAS_GARBAGE hint is falsely unset,
 * but that should be rare.  Also, _bt_vacuum_one_page() won't unset the
 * BTP_HAS_GARBAGE flag when it finds no LP_DEAD items, so a successful
 * deduplication pass will always clear it, just to keep things tidy.
 */
void
_bt_dedup_one_page(Relation rel, Buffer buf, Relation heapRel,
				   IndexTuple newitem, Size newitemsz, bool checkingunique)
{
	OffsetNumber offnum,
				minoff,
				maxoff;
	Page		page = BufferGetPage(buf);
	BTPageOpaque opaque;
	Page		newpage;
	OffsetNumber deletable[MaxIndexTuplesPerPage];
	BTDedupState state;
	int			ndeletable = 0;
	Size		pagesaving = 0;
	bool		singlevalstrat = false;
	int			nkeyatts = IndexRelationGetNumberOfKeyAttributes(rel);

	/*
	 * We can't assume that there are no LP_DEAD items.  For one thing, VACUUM
	 * will clear the BTP_HAS_GARBAGE hint without reliably removing items
	 * that are marked LP_DEAD.  We don't want to unnecessarily unset LP_DEAD
	 * bits when deduplicating items.  Allowing it would be correct, though
	 * wasteful.
	 */
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	minoff = P_FIRSTDATAKEY(opaque);
	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = minoff;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemid = PageGetItemId(page, offnum);

		if (ItemIdIsDead(itemid))
			deletable[ndeletable++] = offnum;
	}

	if (ndeletable > 0)
	{
		_bt_delitems_delete(rel, buf, deletable, ndeletable, heapRel);

		/*
		 * Return when a split will be avoided.  This is equivalent to
		 * avoiding a split using the usual _bt_vacuum_one_page() path.
		 */
		if (PageGetFreeSpace(page) >= newitemsz)
			return;

		/*
		 * Reconsider number of items on page, in case _bt_delitems_delete()
		 * managed to delete an item or two
		 */
		minoff = P_FIRSTDATAKEY(opaque);
		maxoff = PageGetMaxOffsetNumber(page);
	}

	/* Passed-in newitemsz is MAXALIGNED but does not include line pointer */
	newitemsz += sizeof(ItemIdData);

	/*
	 * By here, it's clear that deduplication will definitely be attempted.
	 * Initialize deduplication state.
	 *
	 * It would be possible for maxpostingsize (limit on posting list tuple
	 * size) to be set to one third of the page.  However, it seems like a
	 * good idea to limit the size of posting lists to one sixth of a page.
	 * That ought to leave us with a good split point when pages full of
	 * duplicates can be split several times.
	 */
	state = (BTDedupState) palloc(sizeof(BTDedupStateData));
	state->deduplicate = true;
	state->nmaxitems = 0;
	state->maxpostingsize = Min(BTMaxItemSize(page) / 2, INDEX_SIZE_MASK);
	/* Metadata about base tuple of current pending posting list */
	state->base = NULL;
	state->baseoff = InvalidOffsetNumber;
	state->basetupsize = 0;
	/* Metadata about current pending posting list TIDs */
	state->htids = palloc(state->maxpostingsize);
	state->nhtids = 0;
	state->nitems = 0;
	/* Size of all physical tuples to be replaced by pending posting list */
	state->phystupsize = 0;
	/* nintervals should be initialized to zero */
	state->nintervals = 0;

	/* Determine if "single value" strategy should be used */
	if (!checkingunique)
		singlevalstrat = _bt_do_singleval(rel, page, state, minoff, newitem);

	/*
	 * Deduplicate items from page, and write them to newpage.
	 *
	 * Copy the original page's LSN into newpage copy.  This will become the
	 * updated version of the page.  We need this because XLogInsert will
	 * examine the LSN and possibly dump it in a page image.
	 */
	newpage = PageGetTempPageCopySpecial(page);
	PageSetLSN(newpage, PageGetLSN(page));

	/* Copy high key, if any */
	if (!P_RIGHTMOST(opaque))
	{
		ItemId		hitemid = PageGetItemId(page, P_HIKEY);
		Size		hitemsz = ItemIdGetLength(hitemid);
		IndexTuple	hitem = (IndexTuple) PageGetItem(page, hitemid);

		if (PageAddItem(newpage, (Item) hitem, hitemsz, P_HIKEY,
						false, false) == InvalidOffsetNumber)
			elog(ERROR, "deduplication failed to add highkey");
	}

	for (offnum = minoff;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemid = PageGetItemId(page, offnum);
		IndexTuple	itup = (IndexTuple) PageGetItem(page, itemid);

		Assert(!ItemIdIsDead(itemid));

		if (offnum == minoff)
		{
			/*
			 * No previous/base tuple for the data item -- use the data item
			 * as base tuple of pending posting list
			 */
			_bt_dedup_start_pending(state, itup, offnum);
		}
		else if (state->deduplicate &&
				 _bt_keep_natts_fast(rel, state->base, itup) > nkeyatts &&
				 _bt_dedup_save_htid(state, itup))
		{
			/*
			 * Tuple is equal to base tuple of pending posting list.  Heap
			 * TID(s) for itup have been saved in state.
			 */
		}
		else
		{
			/*
			 * Tuple is not equal to pending posting list tuple, or
			 * _bt_dedup_save_htid() opted to not merge current item into
			 * pending posting list for some other reason (e.g., adding more
			 * TIDs would have caused posting list to exceed current
			 * maxpostingsize).
			 *
			 * If state contains pending posting list with more than one item,
			 * form new posting tuple, and actually update the page.  Else
			 * reset the state and move on without modifying the page.
			 */
			pagesaving += _bt_dedup_finish_pending(newpage, state);

			if (singlevalstrat)
			{
				/*
				 * Single value strategy's extra steps.
				 *
				 * Lower maxpostingsize for sixth and final large posting list
				 * tuple at the point where 5 maxpostingsize-capped tuples
				 * have either been formed or observed.
				 *
				 * When a sixth maxpostingsize-capped item is formed/observed,
				 * stop merging together tuples altogether.  The few tuples
				 * that remain at the end of the page won't be merged together
				 * at all (at least not until after a future page split takes
				 * place).
				 */
				if (state->nmaxitems == 5)
					_bt_singleval_fillfactor(page, state, newitemsz);
				else if (state->nmaxitems == 6)
				{
					state->deduplicate = false;
					singlevalstrat = false; /* won't be back here */
				}
			}

			/* itup starts new pending posting list */
			_bt_dedup_start_pending(state, itup, offnum);
		}
	}

	/* Handle the last item */
	pagesaving += _bt_dedup_finish_pending(newpage, state);

	/*
	 * If no items suitable for deduplication were found, newpage must be
	 * exactly the same as the original page, so just return from function.
	 *
	 * We could determine whether or not to proceed on the basis the space
	 * savings being sufficient to avoid an immediate page split instead.  We
	 * don't do that because there is some small value in nbtsplitloc.c always
	 * operating against a page that is fully deduplicated (apart from
	 * newitem).  Besides, most of the cost has already been paid.
	 */
	if (state->nintervals == 0)
	{
		/* cannot leak memory here */
		pfree(newpage);
		pfree(state->htids);
		pfree(state);
		return;
	}

	/*
	 * By here, it's clear that deduplication will definitely go ahead.
	 *
	 * Clear the BTP_HAS_GARBAGE page flag in the unlikely event that it is
	 * still falsely set, just to keep things tidy.  (We can't rely on
	 * _bt_vacuum_one_page() having done this already, and we can't rely on a
	 * page split or VACUUM getting to it in the near future.)
	 */
	if (P_HAS_GARBAGE(opaque))
	{
		BTPageOpaque nopaque = (BTPageOpaque) PageGetSpecialPointer(newpage);

		nopaque->btpo_flags &= ~BTP_HAS_GARBAGE;
	}

	START_CRIT_SECTION();

	PageRestoreTempPage(newpage, page);
	MarkBufferDirty(buf);

	/* XLOG stuff */
	if (RelationNeedsWAL(rel))
	{
		XLogRecPtr	recptr;
		xl_btree_dedup xlrec_dedup;

		xlrec_dedup.nintervals = state->nintervals;

		XLogBeginInsert();
		XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
		XLogRegisterData((char *) &xlrec_dedup, SizeOfBtreeDedup);

		/*
		 * The intervals array is not in the buffer, but pretend that it is.
		 * When XLogInsert stores the whole buffer, the array need not be
		 * stored too.
		 */
		XLogRegisterBufData(0, (char *) state->intervals,
							state->nintervals * sizeof(BTDedupInterval));

		recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_DEDUP);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	/* Local space accounting should agree with page accounting */
	Assert(pagesaving < newitemsz || PageGetExactFreeSpace(page) >= newitemsz);

	/* cannot leak memory here */
	pfree(state->htids);
	pfree(state);
}

/*
 * Create a new pending posting list tuple based on caller's base tuple.
 *
 * Every tuple processed by deduplication either becomes the base tuple for a
 * posting list, or gets its heap TID(s) accepted into a pending posting list.
 * A tuple that starts out as the base tuple for a posting list will only
 * actually be rewritten within _bt_dedup_finish_pending() when it turns out
 * that there are duplicates that can be merged into the base tuple.
 */
void
_bt_dedup_start_pending(BTDedupState state, IndexTuple base,
						OffsetNumber baseoff)
{
	Assert(state->nhtids == 0);
	Assert(state->nitems == 0);
	Assert(!BTreeTupleIsPivot(base));

	/*
	 * Copy heap TID(s) from new base tuple for new candidate posting list
	 * into working state's array
	 */
	if (!BTreeTupleIsPosting(base))
	{
		memcpy(state->htids, &base->t_tid, sizeof(ItemPointerData));
		state->nhtids = 1;
		state->basetupsize = IndexTupleSize(base);
	}
	else
	{
		int			nposting;

		nposting = BTreeTupleGetNPosting(base);
		memcpy(state->htids, BTreeTupleGetPosting(base),
			   sizeof(ItemPointerData) * nposting);
		state->nhtids = nposting;
		/* basetupsize should not include existing posting list */
		state->basetupsize = BTreeTupleGetPostingOffset(base);
	}

	/*
	 * Save new base tuple itself -- it'll be needed if we actually create a
	 * new posting list from new pending posting list.
	 *
	 * Must maintain physical size of all existing tuples (including line
	 * pointer overhead) so that we can calculate space savings on page.
	 */
	state->nitems = 1;
	state->base = base;
	state->baseoff = baseoff;
	state->phystupsize = MAXALIGN(IndexTupleSize(base)) + sizeof(ItemIdData);
	/* Also save baseoff in pending state for interval */
	state->intervals[state->nintervals].baseoff = state->baseoff;
}

/*
 * Save itup heap TID(s) into pending posting list where possible.
 *
 * Returns bool indicating if the pending posting list managed by state now
 * includes itup's heap TID(s).
 */
bool
_bt_dedup_save_htid(BTDedupState state, IndexTuple itup)
{
	int			nhtids;
	ItemPointer htids;
	Size		mergedtupsz;

	Assert(!BTreeTupleIsPivot(itup));

	if (!BTreeTupleIsPosting(itup))
	{
		nhtids = 1;
		htids = &itup->t_tid;
	}
	else
	{
		nhtids = BTreeTupleGetNPosting(itup);
		htids = BTreeTupleGetPosting(itup);
	}

	/*
	 * Don't append (have caller finish pending posting list as-is) if
	 * appending heap TID(s) from itup would put us over maxpostingsize limit.
	 *
	 * This calculation needs to match the code used within _bt_form_posting()
	 * for new posting list tuples.
	 */
	mergedtupsz = MAXALIGN(state->basetupsize +
						   (state->nhtids + nhtids) * sizeof(ItemPointerData));

	if (mergedtupsz > state->maxpostingsize)
	{
		/*
		 * Count this as an oversized item for single value strategy, though
		 * only when there are 50 TIDs in the final posting list tuple.  This
		 * limit (which is fairly arbitrary) avoids confusion about how many
		 * 1/6 of a page tuples have been encountered/created by the current
		 * deduplication pass.
		 *
		 * Note: We deliberately don't consider which deduplication pass
		 * merged together tuples to create this item (could be a previous
		 * deduplication pass, or current pass).  See _bt_do_singleval()
		 * comments.
		 */
		if (state->nhtids > 50)
			state->nmaxitems++;

		return false;
	}

	/*
	 * Save heap TIDs to pending posting list tuple -- itup can be merged into
	 * pending posting list
	 */
	state->nitems++;
	memcpy(state->htids + state->nhtids, htids,
		   sizeof(ItemPointerData) * nhtids);
	state->nhtids += nhtids;
	state->phystupsize += MAXALIGN(IndexTupleSize(itup)) + sizeof(ItemIdData);

	return true;
}

/*
 * Finalize pending posting list tuple, and add it to the page.  Final tuple
 * is based on saved base tuple, and saved list of heap TIDs.
 *
 * Returns space saving from deduplicating to make a new posting list tuple.
 * Note that this includes line pointer overhead.  This is zero in the case
 * where no deduplication was possible.
 */
Size
_bt_dedup_finish_pending(Page newpage, BTDedupState state)
{
	OffsetNumber tupoff;
	Size		tuplesz;
	Size		spacesaving;

	Assert(state->nitems > 0);
	Assert(state->nitems <= state->nhtids);
	Assert(state->intervals[state->nintervals].baseoff == state->baseoff);

	tupoff = OffsetNumberNext(PageGetMaxOffsetNumber(newpage));
	if (state->nitems == 1)
	{
		/* Use original, unchanged base tuple */
		tuplesz = IndexTupleSize(state->base);
		if (PageAddItem(newpage, (Item) state->base, tuplesz, tupoff,
						false, false) == InvalidOffsetNumber)
			elog(ERROR, "deduplication failed to add tuple to page");

		spacesaving = 0;
	}
	else
	{
		IndexTuple	final;

		/* Form a tuple with a posting list */
		final = _bt_form_posting(state->base, state->htids, state->nhtids);
		tuplesz = IndexTupleSize(final);
		Assert(tuplesz <= state->maxpostingsize);

		/* Save final number of items for posting list */
		state->intervals[state->nintervals].nitems = state->nitems;

		Assert(tuplesz == MAXALIGN(IndexTupleSize(final)));
		if (PageAddItem(newpage, (Item) final, tuplesz, tupoff, false,
						false) == InvalidOffsetNumber)
			elog(ERROR, "deduplication failed to add tuple to page");

		pfree(final);
		spacesaving = state->phystupsize - (tuplesz + sizeof(ItemIdData));
		/* Increment nintervals, since we wrote a new posting list tuple */
		state->nintervals++;
		Assert(spacesaving > 0 && spacesaving < BLCKSZ);
	}

	/* Reset state for next pending posting list */
	state->nhtids = 0;
	state->nitems = 0;
	state->phystupsize = 0;

	return spacesaving;
}

/*
 * Determine if page non-pivot tuples (data items) are all duplicates of the
 * same value -- if they are, deduplication's "single value" strategy should
 * be applied.  The general goal of this strategy is to ensure that
 * nbtsplitloc.c (which uses its own single value strategy) will find a useful
 * split point as further duplicates are inserted, and successive rightmost
 * page splits occur among pages that store the same duplicate value.  When
 * the page finally splits, it should end up BTREE_SINGLEVAL_FILLFACTOR% full,
 * just like it would if deduplication were disabled.
 *
 * We expect that affected workloads will require _several_ single value
 * strategy deduplication passes (over a page that only stores duplicates)
 * before the page is finally split.  The first deduplication pass should only
 * find regular non-pivot tuples.  Later deduplication passes will find
 * existing maxpostingsize-capped posting list tuples, which must be skipped
 * over.  The penultimate pass is generally the first pass that actually
 * reaches _bt_singleval_fillfactor(), and so will deliberately leave behind a
 * few untouched non-pivot tuples.  The final deduplication pass won't free
 * any space -- it will skip over everything without merging anything (it
 * retraces the steps of the penultimate pass).
 *
 * Fortunately, having several passes isn't too expensive.  Each pass (after
 * the first pass) won't spend many cycles on the large posting list tuples
 * left by previous passes.  Each pass will find a large contiguous group of
 * smaller duplicate tuples to merge together at the end of the page.
 *
 * Note: We deliberately don't bother checking if the high key is a distinct
 * value (prior to the TID tiebreaker column) before proceeding, unlike
 * nbtsplitloc.c.  Its single value strategy only gets applied on the
 * rightmost page of duplicates of the same value (other leaf pages full of
 * duplicates will get a simple 50:50 page split instead of splitting towards
 * the end of the page).  There is little point in making the same distinction
 * here.
 */
static bool
_bt_do_singleval(Relation rel, Page page, BTDedupState state,
				 OffsetNumber minoff, IndexTuple newitem)
{
	int			nkeyatts = IndexRelationGetNumberOfKeyAttributes(rel);
	ItemId		itemid;
	IndexTuple	itup;

	itemid = PageGetItemId(page, minoff);
	itup = (IndexTuple) PageGetItem(page, itemid);

	if (_bt_keep_natts_fast(rel, newitem, itup) > nkeyatts)
	{
		itemid = PageGetItemId(page, PageGetMaxOffsetNumber(page));
		itup = (IndexTuple) PageGetItem(page, itemid);

		if (_bt_keep_natts_fast(rel, newitem, itup) > nkeyatts)
			return true;
	}

	return false;
}

/*
 * Lower maxpostingsize when using "single value" strategy, to avoid a sixth
 * and final maxpostingsize-capped tuple.  The sixth and final posting list
 * tuple will end up somewhat smaller than the first five.  (Note: The first
 * five tuples could actually just be very large duplicate tuples that
 * couldn't be merged together at all.  Deduplication will simply not modify
 * the page when that happens.)
 *
 * When there are six posting lists on the page (after current deduplication
 * pass goes on to create/observe a sixth very large tuple), caller should end
 * its deduplication pass.  It isn't useful to try to deduplicate items that
 * are supposed to end up on the new right sibling page following the
 * anticipated page split.  A future deduplication pass of future right
 * sibling page might take care of it.  (This is why the first single value
 * strategy deduplication pass for a given leaf page will generally find only
 * plain non-pivot tuples -- see _bt_do_singleval() comments.)
 */
static void
_bt_singleval_fillfactor(Page page, BTDedupState state, Size newitemsz)
{
	Size		leftfree;
	int			reduction;

	/* This calculation needs to match nbtsplitloc.c */
	leftfree = PageGetPageSize(page) - SizeOfPageHeaderData -
		MAXALIGN(sizeof(BTPageOpaqueData));
	/* Subtract size of new high key (includes pivot heap TID space) */
	leftfree -= newitemsz + MAXALIGN(sizeof(ItemPointerData));

	/*
	 * Reduce maxpostingsize by an amount equal to target free space on left
	 * half of page
	 */
	reduction = leftfree * ((100 - BTREE_SINGLEVAL_FILLFACTOR) / 100.0);
	if (state->maxpostingsize > reduction)
		state->maxpostingsize -= reduction;
	else
		state->maxpostingsize = 0;
}

/*
 * Build a posting list tuple based on caller's "base" index tuple and list of
 * heap TIDs.  When nhtids == 1, builds a standard non-pivot tuple without a
 * posting list. (Posting list tuples can never have a single heap TID, partly
 * because that ensures that deduplication always reduces final MAXALIGN()'d
 * size of entire tuple.)
 *
 * Convention is that posting list starts at a MAXALIGN()'d offset (rather
 * than a SHORTALIGN()'d offset), in line with the approach taken when
 * appending a heap TID to new pivot tuple/high key during suffix truncation.
 * This sometimes wastes a little space that was only needed as alignment
 * padding in the original tuple.  Following this convention simplifies the
 * space accounting used when deduplicating a page (the same convention
 * simplifies the accounting for choosing a point to split a page at).
 *
 * Note: Caller's "htids" array must be unique and already in ascending TID
 * order.  Any existing heap TIDs from "base" won't automatically appear in
 * returned posting list tuple (they must be included in htids array.)
 */
IndexTuple
_bt_form_posting(IndexTuple base, ItemPointer htids, int nhtids)
{
	uint32		keysize,
				newsize;
	IndexTuple	itup;

	if (BTreeTupleIsPosting(base))
		keysize = BTreeTupleGetPostingOffset(base);
	else
		keysize = IndexTupleSize(base);

	Assert(!BTreeTupleIsPivot(base));
	Assert(nhtids > 0 && nhtids <= PG_UINT16_MAX);
	Assert(keysize == MAXALIGN(keysize));

	/* Determine final size of new tuple */
	if (nhtids > 1)
		newsize = MAXALIGN(keysize +
						   nhtids * sizeof(ItemPointerData));
	else
		newsize = keysize;

	Assert(newsize <= INDEX_SIZE_MASK);
	Assert(newsize == MAXALIGN(newsize));

	/* Allocate memory using palloc0() (matches index_form_tuple()) */
	itup = palloc0(newsize);
	memcpy(itup, base, keysize);
	itup->t_info &= ~INDEX_SIZE_MASK;
	itup->t_info |= newsize;
	if (nhtids > 1)
	{
		/* Form posting list tuple */
		BTreeTupleSetPosting(itup, nhtids, keysize);
		memcpy(BTreeTupleGetPosting(itup), htids,
			   sizeof(ItemPointerData) * nhtids);
		Assert(_bt_posting_valid(itup));
	}
	else
	{
		/* Form standard non-pivot tuple */
		itup->t_info &= ~INDEX_ALT_TID_MASK;
		ItemPointerCopy(htids, &itup->t_tid);
		Assert(ItemPointerIsValid(&itup->t_tid));
	}

	return itup;
}

/*
 * Generate a replacement tuple by "updating" a posting list tuple so that it
 * no longer has TIDs that need to be deleted.
 *
 * Used by VACUUM.  Caller's vacposting argument points to the existing
 * posting list tuple to be updated.
 *
 * On return, caller's vacposting argument will point to final "updated"
 * tuple, which will be palloc()'d in caller's memory context.
 */
void
_bt_update_posting(BTVacuumPosting vacposting)
{
	IndexTuple	origtuple = vacposting->itup;
	uint32		keysize,
				newsize;
	IndexTuple	itup;
	int			nhtids;
	int			ui,
				d;
	ItemPointer htids;

	nhtids = BTreeTupleGetNPosting(origtuple) - vacposting->ndeletedtids;

	Assert(_bt_posting_valid(origtuple));
	Assert(nhtids > 0 && nhtids < BTreeTupleGetNPosting(origtuple));

	/*
	 * Determine final size of new tuple.
	 *
	 * This calculation needs to match the code used within _bt_form_posting()
	 * for new posting list tuples.  We avoid calling _bt_form_posting() here
	 * to save ourselves a second memory allocation for a htids workspace.
	 */
	keysize = BTreeTupleGetPostingOffset(origtuple);
	if (nhtids > 1)
		newsize = MAXALIGN(keysize +
						   nhtids * sizeof(ItemPointerData));
	else
		newsize = keysize;

	Assert(newsize <= INDEX_SIZE_MASK);
	Assert(newsize == MAXALIGN(newsize));

	/* Allocate memory using palloc0() (matches index_form_tuple()) */
	itup = palloc0(newsize);
	memcpy(itup, origtuple, keysize);
	itup->t_info &= ~INDEX_SIZE_MASK;
	itup->t_info |= newsize;

	if (nhtids > 1)
	{
		/* Form posting list tuple */
		BTreeTupleSetPosting(itup, nhtids, keysize);
		htids = BTreeTupleGetPosting(itup);
	}
	else
	{
		/* Form standard non-pivot tuple */
		itup->t_info &= ~INDEX_ALT_TID_MASK;
		htids = &itup->t_tid;
	}

	ui = 0;
	d = 0;
	for (int i = 0; i < BTreeTupleGetNPosting(origtuple); i++)
	{
		if (d < vacposting->ndeletedtids && vacposting->deletetids[d] == i)
		{
			d++;
			continue;
		}
		htids[ui++] = *BTreeTupleGetPostingN(origtuple, i);
	}
	Assert(ui == nhtids);
	Assert(d == vacposting->ndeletedtids);
	Assert(nhtids == 1 || _bt_posting_valid(itup));
	Assert(nhtids > 1 || ItemPointerIsValid(&itup->t_tid));

	/* vacposting arg's itup will now point to updated version */
	vacposting->itup = itup;
}

/*
 * Prepare for a posting list split by swapping heap TID in newitem with heap
 * TID from original posting list (the 'oposting' heap TID located at offset
 * 'postingoff').  Modifies newitem, so caller should pass their own private
 * copy that can safely be modified.
 *
 * Returns new posting list tuple, which is palloc()'d in caller's context.
 * This is guaranteed to be the same size as 'oposting'.  Modified newitem is
 * what caller actually inserts. (This happens inside the same critical
 * section that performs an in-place update of old posting list using new
 * posting list returned here.)
 *
 * While the keys from newitem and oposting must be opclass equal, and must
 * generate identical output when run through the underlying type's output
 * function, it doesn't follow that their representations match exactly.
 * Caller must avoid assuming that there can't be representational differences
 * that make datums from oposting bigger or smaller than the corresponding
 * datums from newitem.  For example, differences in TOAST input state might
 * break a faulty assumption about tuple size (the executor is entitled to
 * apply TOAST compression based on its own criteria).  It also seems possible
 * that further representational variation will be introduced in the future,
 * in order to support nbtree features like page-level prefix compression.
 *
 * See nbtree/README for details on the design of posting list splits.
 */
IndexTuple
_bt_swap_posting(IndexTuple newitem, IndexTuple oposting, int postingoff)
{
	int			nhtids;
	char	   *replacepos;
	char	   *replaceposright;
	Size		nmovebytes;
	IndexTuple	nposting;

	nhtids = BTreeTupleGetNPosting(oposting);
	Assert(_bt_posting_valid(oposting));
	Assert(postingoff > 0 && postingoff < nhtids);

	/*
	 * Move item pointers in posting list to make a gap for the new item's
	 * heap TID.  We shift TIDs one place to the right, losing original
	 * rightmost TID. (nmovebytes must not include TIDs to the left of
	 * postingoff, nor the existing rightmost/max TID that gets overwritten.)
	 */
	nposting = CopyIndexTuple(oposting);
	replacepos = (char *) BTreeTupleGetPostingN(nposting, postingoff);
	replaceposright = (char *) BTreeTupleGetPostingN(nposting, postingoff + 1);
	nmovebytes = (nhtids - postingoff - 1) * sizeof(ItemPointerData);
	memmove(replaceposright, replacepos, nmovebytes);

	/* Fill the gap at postingoff with TID of new item (original new TID) */
	Assert(!BTreeTupleIsPivot(newitem) && !BTreeTupleIsPosting(newitem));
	ItemPointerCopy(&newitem->t_tid, (ItemPointer) replacepos);

	/* Now copy oposting's rightmost/max TID into new item (final new TID) */
	ItemPointerCopy(BTreeTupleGetMaxHeapTID(oposting), &newitem->t_tid);

	Assert(ItemPointerCompare(BTreeTupleGetMaxHeapTID(nposting),
							  BTreeTupleGetHeapTID(newitem)) < 0);
	Assert(_bt_posting_valid(nposting));

	return nposting;
}

/*
 * Verify posting list invariants for "posting", which must be a posting list
 * tuple.  Used within assertions.
 */
#ifdef USE_ASSERT_CHECKING
static bool
_bt_posting_valid(IndexTuple posting)
{
	ItemPointerData last;
	ItemPointer htid;

	if (!BTreeTupleIsPosting(posting) || BTreeTupleGetNPosting(posting) < 2)
		return false;

	/* Remember first heap TID for loop */
	ItemPointerCopy(BTreeTupleGetHeapTID(posting), &last);
	if (!ItemPointerIsValid(&last))
		return false;

	/* Iterate, starting from second TID */
	for (int i = 1; i < BTreeTupleGetNPosting(posting); i++)
	{
		htid = BTreeTupleGetPostingN(posting, i);

		if (!ItemPointerIsValid(htid))
			return false;
		if (ItemPointerCompare(htid, &last) <= 0)
			return false;
		ItemPointerCopy(htid, &last);
	}

	return true;
}
#endif
