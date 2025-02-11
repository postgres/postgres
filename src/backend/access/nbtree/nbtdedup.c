/*-------------------------------------------------------------------------
 *
 * nbtdedup.c
 *	  Deduplicate or bottom-up delete items in Postgres btrees.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "utils/rel.h"

static void _bt_bottomupdel_finish_pending(Page page, BTDedupState state,
										   TM_IndexDeleteOp *delstate);
static bool _bt_do_singleval(Relation rel, Page page, BTDedupState state,
							 OffsetNumber minoff, IndexTuple newitem);
static void _bt_singleval_fillfactor(Page page, BTDedupState state,
									 Size newitemsz);
#ifdef USE_ASSERT_CHECKING
static bool _bt_posting_valid(IndexTuple posting);
#endif

/*
 * Perform a deduplication pass.
 *
 * The general approach taken here is to perform as much deduplication as
 * possible to free as much space as possible.  Note, however, that "single
 * value" strategy is used for !bottomupdedup callers when the page is full of
 * tuples of a single value.  Deduplication passes that apply the strategy
 * will leave behind a few untouched tuples at the end of the page, preparing
 * the page for an anticipated page split that uses nbtsplitloc.c's own single
 * value strategy.  Our high level goal is to delay merging the untouched
 * tuples until after the page splits.
 *
 * When a call to _bt_bottomupdel_pass() just took place (and failed), our
 * high level goal is to prevent a page split entirely by buying more time.
 * We still hope that a page split can be avoided altogether.  That's why
 * single value strategy is not even considered for bottomupdedup callers.
 *
 * The page will have to be split if we cannot successfully free at least
 * newitemsz (we also need space for newitem's line pointer, which isn't
 * included in caller's newitemsz).
 *
 * Note: Caller should have already deleted all existing items with their
 * LP_DEAD bits set.
 */
void
_bt_dedup_pass(Relation rel, Buffer buf, IndexTuple newitem, Size newitemsz,
			   bool bottomupdedup)
{
	OffsetNumber offnum,
				minoff,
				maxoff;
	Page		page = BufferGetPage(buf);
	BTPageOpaque opaque = BTPageGetOpaque(page);
	Page		newpage;
	BTDedupState state;
	Size		pagesaving PG_USED_FOR_ASSERTS_ONLY = 0;
	bool		singlevalstrat = false;
	int			nkeyatts = IndexRelationGetNumberOfKeyAttributes(rel);

	/* Passed-in newitemsz is MAXALIGNED but does not include line pointer */
	newitemsz += sizeof(ItemIdData);

	/*
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

	minoff = P_FIRSTDATAKEY(opaque);
	maxoff = PageGetMaxOffsetNumber(page);

	/*
	 * Consider applying "single value" strategy, though only if the page
	 * seems likely to be split in the near future
	 */
	if (!bottomupdedup)
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
			 * form new posting tuple and add it to our temp page (newpage).
			 * Else add pending interval's base tuple to the temp page as-is.
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
				 * place, when this page's newly allocated right sibling page
				 * gets its first deduplication pass).
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
	 * Clear the BTP_HAS_GARBAGE page flag.  The index must be a heapkeyspace
	 * index, and as such we'll never pay attention to BTP_HAS_GARBAGE anyway.
	 * But keep things tidy.
	 */
	if (P_HAS_GARBAGE(opaque))
	{
		BTPageOpaque nopaque = BTPageGetOpaque(newpage);

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
		XLogRegisterData(&xlrec_dedup, SizeOfBtreeDedup);

		/*
		 * The intervals array is not in the buffer, but pretend that it is.
		 * When XLogInsert stores the whole buffer, the array need not be
		 * stored too.
		 */
		XLogRegisterBufData(0, state->intervals,
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
 * Perform bottom-up index deletion pass.
 *
 * See if duplicate index tuples (plus certain nearby tuples) are eligible to
 * be deleted via bottom-up index deletion.  The high level goal here is to
 * entirely prevent "unnecessary" page splits caused by MVCC version churn
 * from UPDATEs (when the UPDATEs don't logically modify any of the columns
 * covered by the 'rel' index).  This is qualitative, not quantitative -- we
 * do not particularly care about once-off opportunities to delete many index
 * tuples together.
 *
 * See nbtree/README for details on the design of nbtree bottom-up deletion.
 * See access/tableam.h for a description of how we're expected to cooperate
 * with the tableam.
 *
 * Returns true on success, in which case caller can assume page split will be
 * avoided for a reasonable amount of time.  Returns false when caller should
 * deduplicate the page (if possible at all).
 *
 * Note: Occasionally we return true despite failing to delete enough items to
 * avoid a split.  This makes caller skip deduplication and go split the page
 * right away.  Our return value is always just advisory information.
 *
 * Note: Caller should have already deleted all existing items with their
 * LP_DEAD bits set.
 */
bool
_bt_bottomupdel_pass(Relation rel, Buffer buf, Relation heapRel,
					 Size newitemsz)
{
	OffsetNumber offnum,
				minoff,
				maxoff;
	Page		page = BufferGetPage(buf);
	BTPageOpaque opaque = BTPageGetOpaque(page);
	BTDedupState state;
	TM_IndexDeleteOp delstate;
	bool		neverdedup;
	int			nkeyatts = IndexRelationGetNumberOfKeyAttributes(rel);

	/* Passed-in newitemsz is MAXALIGNED but does not include line pointer */
	newitemsz += sizeof(ItemIdData);

	/* Initialize deduplication state */
	state = (BTDedupState) palloc(sizeof(BTDedupStateData));
	state->deduplicate = true;
	state->nmaxitems = 0;
	state->maxpostingsize = BLCKSZ; /* We're not really deduplicating */
	state->base = NULL;
	state->baseoff = InvalidOffsetNumber;
	state->basetupsize = 0;
	state->htids = palloc(state->maxpostingsize);
	state->nhtids = 0;
	state->nitems = 0;
	state->phystupsize = 0;
	state->nintervals = 0;

	/*
	 * Initialize tableam state that describes bottom-up index deletion
	 * operation.
	 *
	 * We'll go on to ask the tableam to search for TIDs whose index tuples we
	 * can safely delete.  The tableam will search until our leaf page space
	 * target is satisfied, or until the cost of continuing with the tableam
	 * operation seems too high.  It focuses its efforts on TIDs associated
	 * with duplicate index tuples that we mark "promising".
	 *
	 * This space target is a little arbitrary.  The tableam must be able to
	 * keep the costs and benefits in balance.  We provide the tableam with
	 * exhaustive information about what might work, without directly
	 * concerning ourselves with avoiding work during the tableam call.  Our
	 * role in costing the bottom-up deletion process is strictly advisory.
	 */
	delstate.irel = rel;
	delstate.iblknum = BufferGetBlockNumber(buf);
	delstate.bottomup = true;
	delstate.bottomupfreespace = Max(BLCKSZ / 16, newitemsz);
	delstate.ndeltids = 0;
	delstate.deltids = palloc(MaxTIDsPerBTreePage * sizeof(TM_IndexDelete));
	delstate.status = palloc(MaxTIDsPerBTreePage * sizeof(TM_IndexStatus));

	minoff = P_FIRSTDATAKEY(opaque);
	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = minoff;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemid = PageGetItemId(page, offnum);
		IndexTuple	itup = (IndexTuple) PageGetItem(page, itemid);

		Assert(!ItemIdIsDead(itemid));

		if (offnum == minoff)
		{
			/* itup starts first pending interval */
			_bt_dedup_start_pending(state, itup, offnum);
		}
		else if (_bt_keep_natts_fast(rel, state->base, itup) > nkeyatts &&
				 _bt_dedup_save_htid(state, itup))
		{
			/* Tuple is equal; just added its TIDs to pending interval */
		}
		else
		{
			/* Finalize interval -- move its TIDs to delete state */
			_bt_bottomupdel_finish_pending(page, state, &delstate);

			/* itup starts new pending interval */
			_bt_dedup_start_pending(state, itup, offnum);
		}
	}
	/* Finalize final interval -- move its TIDs to delete state */
	_bt_bottomupdel_finish_pending(page, state, &delstate);

	/*
	 * We don't give up now in the event of having few (or even zero)
	 * promising tuples for the tableam because it's not up to us as the index
	 * AM to manage costs (note that the tableam might have heuristics of its
	 * own that work out what to do).  We should at least avoid having our
	 * caller do a useless deduplication pass after we return in the event of
	 * zero promising tuples, though.
	 */
	neverdedup = false;
	if (state->nintervals == 0)
		neverdedup = true;

	pfree(state->htids);
	pfree(state);

	/* Ask tableam which TIDs are deletable, then physically delete them */
	_bt_delitems_delete_check(rel, buf, heapRel, &delstate);

	pfree(delstate.deltids);
	pfree(delstate.status);

	/* Report "success" to caller unconditionally to avoid deduplication */
	if (neverdedup)
		return true;

	/* Don't dedup when we won't end up back here any time soon anyway */
	return PageGetExactFreeSpace(page) >= Max(BLCKSZ / 24, newitemsz);
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
		Assert(tuplesz == MAXALIGN(IndexTupleSize(state->base)));
		Assert(tuplesz <= BTMaxItemSize(newpage));
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
		Assert(tuplesz <= BTMaxItemSize(newpage));
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
 * Finalize interval during bottom-up index deletion.
 *
 * During a bottom-up pass we expect that TIDs will be recorded in dedup state
 * first, and then get moved over to delstate (in variable-sized batches) by
 * calling here.  Call here happens when the number of TIDs in a dedup
 * interval is known, and interval gets finalized (i.e. when caller sees next
 * tuple on the page is not a duplicate, or when caller runs out of tuples to
 * process from leaf page).
 *
 * This is where bottom-up deletion determines and remembers which entries are
 * duplicates.  This will be important information to the tableam delete
 * infrastructure later on.  Plain index tuple duplicates are marked
 * "promising" here, per tableam contract.
 *
 * Our approach to marking entries whose TIDs come from posting lists is more
 * complicated.  Posting lists can only be formed by a deduplication pass (or
 * during an index build), so recent version churn affecting the pointed-to
 * logical rows is not particularly likely.  We may still give a weak signal
 * about posting list tuples' entries (by marking just one of its TIDs/entries
 * promising), though this is only a possibility in the event of further
 * duplicate index tuples in final interval that covers posting list tuple (as
 * in the plain tuple case).  A weak signal/hint will be useful to the tableam
 * when it has no stronger signal to go with for the deletion operation as a
 * whole.
 *
 * The heuristics we use work well in practice because we only need to give
 * the tableam the right _general_ idea about where to look.  Garbage tends to
 * naturally get concentrated in relatively few table blocks with workloads
 * that bottom-up deletion targets.  The tableam cannot possibly rank all
 * available table blocks sensibly based on the hints we provide, but that's
 * okay -- only the extremes matter.  The tableam just needs to be able to
 * predict which few table blocks will have the most tuples that are safe to
 * delete for each deletion operation, with low variance across related
 * deletion operations.
 */
static void
_bt_bottomupdel_finish_pending(Page page, BTDedupState state,
							   TM_IndexDeleteOp *delstate)
{
	bool		dupinterval = (state->nitems > 1);

	Assert(state->nitems > 0);
	Assert(state->nitems <= state->nhtids);
	Assert(state->intervals[state->nintervals].baseoff == state->baseoff);

	for (int i = 0; i < state->nitems; i++)
	{
		OffsetNumber offnum = state->baseoff + i;
		ItemId		itemid = PageGetItemId(page, offnum);
		IndexTuple	itup = (IndexTuple) PageGetItem(page, itemid);
		TM_IndexDelete *ideltid = &delstate->deltids[delstate->ndeltids];
		TM_IndexStatus *istatus = &delstate->status[delstate->ndeltids];

		if (!BTreeTupleIsPosting(itup))
		{
			/* Simple case: A plain non-pivot tuple */
			ideltid->tid = itup->t_tid;
			ideltid->id = delstate->ndeltids;
			istatus->idxoffnum = offnum;
			istatus->knowndeletable = false;	/* for now */
			istatus->promising = dupinterval;	/* simple rule */
			istatus->freespace = ItemIdGetLength(itemid) + sizeof(ItemIdData);

			delstate->ndeltids++;
		}
		else
		{
			/*
			 * Complicated case: A posting list tuple.
			 *
			 * We make the conservative assumption that there can only be at
			 * most one affected logical row per posting list tuple.  There
			 * will be at most one promising entry in deltids to represent
			 * this presumed lone logical row.  Note that this isn't even
			 * considered unless the posting list tuple is also in an interval
			 * of duplicates -- this complicated rule is just a variant of the
			 * simple rule used to decide if plain index tuples are promising.
			 */
			int			nitem = BTreeTupleGetNPosting(itup);
			bool		firstpromising = false;
			bool		lastpromising = false;

			Assert(_bt_posting_valid(itup));

			if (dupinterval)
			{
				/*
				 * Complicated rule: either the first or last TID in the
				 * posting list gets marked promising (if any at all)
				 */
				BlockNumber minblocklist,
							midblocklist,
							maxblocklist;
				ItemPointer mintid,
							midtid,
							maxtid;

				mintid = BTreeTupleGetHeapTID(itup);
				midtid = BTreeTupleGetPostingN(itup, nitem / 2);
				maxtid = BTreeTupleGetMaxHeapTID(itup);
				minblocklist = ItemPointerGetBlockNumber(mintid);
				midblocklist = ItemPointerGetBlockNumber(midtid);
				maxblocklist = ItemPointerGetBlockNumber(maxtid);

				/* Only entry with predominant table block can be promising */
				firstpromising = (minblocklist == midblocklist);
				lastpromising = (!firstpromising &&
								 midblocklist == maxblocklist);
			}

			for (int p = 0; p < nitem; p++)
			{
				ItemPointer htid = BTreeTupleGetPostingN(itup, p);

				ideltid->tid = *htid;
				ideltid->id = delstate->ndeltids;
				istatus->idxoffnum = offnum;
				istatus->knowndeletable = false;	/* for now */
				istatus->promising = false;
				if ((firstpromising && p == 0) ||
					(lastpromising && p == nitem - 1))
					istatus->promising = true;
				istatus->freespace = sizeof(ItemPointerData);	/* at worst */

				ideltid++;
				istatus++;
				delstate->ndeltids++;
			}
		}
	}

	if (dupinterval)
	{
		state->intervals[state->nintervals].nitems = state->nitems;
		state->nintervals++;
	}

	/* Reset state for next interval */
	state->nhtids = 0;
	state->nitems = 0;
	state->phystupsize = 0;
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
 * Used by both VACUUM and index deletion.  Caller's vacposting argument
 * points to the existing posting list tuple to be updated.
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

	/*
	 * The postingoff argument originated as a _bt_binsrch_posting() return
	 * value.  It will be 0 in the event of corruption that makes a leaf page
	 * contain a non-pivot tuple that's somehow identical to newitem (no two
	 * non-pivot tuples should ever have the same TID).  This has been known
	 * to happen in the field from time to time.
	 *
	 * Perform a basic sanity check to catch this case now.
	 */
	if (!(postingoff > 0 && postingoff < nhtids))
		elog(ERROR, "posting list tuple with %d items cannot be split at offset %d",
			 nhtids, postingoff);

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
