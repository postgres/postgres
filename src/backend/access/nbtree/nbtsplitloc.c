/*-------------------------------------------------------------------------
 *
 * nbtsplitloc.c
 *	  Choose split point code for Postgres btree implementation.
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/nbtree/nbtsplitloc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/nbtree.h"
#include "storage/lmgr.h"

typedef enum
{
	/* strategy for searching through materialized list of split points */
	SPLIT_DEFAULT,				/* give some weight to truncation */
	SPLIT_MANY_DUPLICATES,		/* find minimally distinguishing point */
	SPLIT_SINGLE_VALUE			/* leave left page almost full */
} FindSplitStrat;

typedef struct
{
	/* details of free space left by split */
	int16		curdelta;		/* current leftfree/rightfree delta */
	int16		leftfree;		/* space left on left page post-split */
	int16		rightfree;		/* space left on right page post-split */

	/* split point identifying fields (returned by _bt_findsplitloc) */
	OffsetNumber firstrightoff; /* first origpage item on rightpage */
	bool		newitemonleft;	/* new item goes on left, or right? */

} SplitPoint;

typedef struct
{
	/* context data for _bt_recsplitloc */
	Relation	rel;			/* index relation */
	Page		origpage;		/* page undergoing split */
	IndexTuple	newitem;		/* new item (cause of page split) */
	Size		newitemsz;		/* size of newitem (includes line pointer) */
	bool		is_leaf;		/* T if splitting a leaf page */
	bool		is_rightmost;	/* T if splitting rightmost page on level */
	OffsetNumber newitemoff;	/* where the new item is to be inserted */
	int			leftspace;		/* space available for items on left page */
	int			rightspace;		/* space available for items on right page */
	int			olddataitemstotal;	/* space taken by old items */
	Size		minfirstrightsz;	/* smallest firstright size */

	/* candidate split point data */
	int			maxsplits;		/* maximum number of splits */
	int			nsplits;		/* current number of splits */
	SplitPoint *splits;			/* all candidate split points for page */
	int			interval;		/* current range of acceptable split points */
} FindSplitData;

static void _bt_recsplitloc(FindSplitData *state,
							OffsetNumber firstrightoff, bool newitemonleft,
							int olddataitemstoleft,
							Size firstrightofforigpagetuplesz);
static void _bt_deltasortsplits(FindSplitData *state, double fillfactormult,
								bool usemult);
static int	_bt_splitcmp(const void *arg1, const void *arg2);
static bool _bt_afternewitemoff(FindSplitData *state, OffsetNumber maxoff,
								int leaffillfactor, bool *usemult);
static bool _bt_adjacenthtid(ItemPointer lowhtid, ItemPointer highhtid);
static OffsetNumber _bt_bestsplitloc(FindSplitData *state, int perfectpenalty,
									 bool *newitemonleft, FindSplitStrat strategy);
static int	_bt_defaultinterval(FindSplitData *state);
static int	_bt_strategy(FindSplitData *state, SplitPoint *leftpage,
						 SplitPoint *rightpage, FindSplitStrat *strategy);
static void _bt_interval_edges(FindSplitData *state,
							   SplitPoint **leftinterval, SplitPoint **rightinterval);
static inline int _bt_split_penalty(FindSplitData *state, SplitPoint *split);
static inline IndexTuple _bt_split_lastleft(FindSplitData *state,
											SplitPoint *split);
static inline IndexTuple _bt_split_firstright(FindSplitData *state,
											  SplitPoint *split);


/*
 *	_bt_findsplitloc() -- find an appropriate place to split a page.
 *
 * The main goal here is to equalize the free space that will be on each
 * split page, *after accounting for the inserted tuple*.  (If we fail to
 * account for it, we might find ourselves with too little room on the page
 * that it needs to go into!)
 *
 * If the page is the rightmost page on its level, we instead try to arrange
 * to leave the left split page fillfactor% full.  In this way, when we are
 * inserting successively increasing keys (consider sequences, timestamps,
 * etc) we will end up with a tree whose pages are about fillfactor% full,
 * instead of the 50% full result that we'd get without this special case.
 * This is the same as nbtsort.c produces for a newly-created tree.  Note
 * that leaf and nonleaf pages use different fillfactors.  Note also that
 * there are a number of further special cases where fillfactor is not
 * applied in the standard way.
 *
 * We are passed the intended insert position of the new tuple, expressed as
 * the offsetnumber of the tuple it must go in front of (this could be
 * maxoff+1 if the tuple is to go at the end).  The new tuple itself is also
 * passed, since it's needed to give some weight to how effective suffix
 * truncation will be.  The implementation picks the split point that
 * maximizes the effectiveness of suffix truncation from a small list of
 * alternative candidate split points that leave each side of the split with
 * about the same share of free space.  Suffix truncation is secondary to
 * equalizing free space, except in cases with large numbers of duplicates.
 * Note that it is always assumed that caller goes on to perform truncation,
 * even with pg_upgrade'd indexes where that isn't actually the case
 * (!heapkeyspace indexes).  See nbtree/README for more information about
 * suffix truncation.
 *
 * We return the index of the first existing tuple that should go on the
 * righthand page (which is called firstrightoff), plus a boolean
 * indicating whether the new tuple goes on the left or right page.  You
 * can think of the returned state as a point _between_ two adjacent data
 * items (laftleft and firstright data items) on an imaginary version of
 * origpage that already includes newitem.  The bool is necessary to
 * disambiguate the case where firstrightoff == newitemoff (i.e. it is
 * sometimes needed to determine if the firstright tuple for the split is
 * newitem rather than the tuple from origpage at offset firstrightoff).
 */
OffsetNumber
_bt_findsplitloc(Relation rel,
				 Page origpage,
				 OffsetNumber newitemoff,
				 Size newitemsz,
				 IndexTuple newitem,
				 bool *newitemonleft)
{
	BTPageOpaque opaque;
	int			leftspace,
				rightspace,
				olddataitemstotal,
				olddataitemstoleft,
				perfectpenalty,
				leaffillfactor;
	FindSplitData state;
	FindSplitStrat strategy;
	ItemId		itemid;
	OffsetNumber offnum,
				maxoff,
				firstrightoff;
	double		fillfactormult;
	bool		usemult;
	SplitPoint	leftpage,
				rightpage;

	opaque = (BTPageOpaque) PageGetSpecialPointer(origpage);
	maxoff = PageGetMaxOffsetNumber(origpage);

	/* Total free space available on a btree page, after fixed overhead */
	leftspace = rightspace =
		PageGetPageSize(origpage) - SizeOfPageHeaderData -
		MAXALIGN(sizeof(BTPageOpaqueData));

	/* The right page will have the same high key as the old page */
	if (!P_RIGHTMOST(opaque))
	{
		itemid = PageGetItemId(origpage, P_HIKEY);
		rightspace -= (int) (MAXALIGN(ItemIdGetLength(itemid)) +
							 sizeof(ItemIdData));
	}

	/* Count up total space in data items before actually scanning 'em */
	olddataitemstotal = rightspace - (int) PageGetExactFreeSpace(origpage);
	leaffillfactor = BTGetFillFactor(rel);

	/* Passed-in newitemsz is MAXALIGNED but does not include line pointer */
	newitemsz += sizeof(ItemIdData);
	state.rel = rel;
	state.origpage = origpage;
	state.newitem = newitem;
	state.newitemsz = newitemsz;
	state.is_leaf = P_ISLEAF(opaque);
	state.is_rightmost = P_RIGHTMOST(opaque);
	state.leftspace = leftspace;
	state.rightspace = rightspace;
	state.olddataitemstotal = olddataitemstotal;
	state.minfirstrightsz = SIZE_MAX;
	state.newitemoff = newitemoff;

	/* newitem cannot be a posting list item */
	Assert(!BTreeTupleIsPosting(newitem));

	/*
	 * maxsplits should never exceed maxoff because there will be at most as
	 * many candidate split points as there are points _between_ tuples, once
	 * you imagine that the new item is already on the original page (the
	 * final number of splits may be slightly lower because not all points
	 * between tuples will be legal).
	 */
	state.maxsplits = maxoff;
	state.splits = palloc(sizeof(SplitPoint) * state.maxsplits);
	state.nsplits = 0;

	/*
	 * Scan through the data items and calculate space usage for a split at
	 * each possible position
	 */
	olddataitemstoleft = 0;

	for (offnum = P_FIRSTDATAKEY(opaque);
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		Size		itemsz;

		itemid = PageGetItemId(origpage, offnum);
		itemsz = MAXALIGN(ItemIdGetLength(itemid)) + sizeof(ItemIdData);

		/*
		 * When item offset number is not newitemoff, neither side of the
		 * split can be newitem.  Record a split after the previous data item
		 * from original page, but before the current data item from original
		 * page. (_bt_recsplitloc() will reject the split when there are no
		 * previous items, which we rely on.)
		 */
		if (offnum < newitemoff)
			_bt_recsplitloc(&state, offnum, false, olddataitemstoleft, itemsz);
		else if (offnum > newitemoff)
			_bt_recsplitloc(&state, offnum, true, olddataitemstoleft, itemsz);
		else
		{
			/*
			 * Record a split after all "offnum < newitemoff" original page
			 * data items, but before newitem
			 */
			_bt_recsplitloc(&state, offnum, false, olddataitemstoleft, itemsz);

			/*
			 * Record a split after newitem, but before data item from
			 * original page at offset newitemoff/current offset
			 */
			_bt_recsplitloc(&state, offnum, true, olddataitemstoleft, itemsz);
		}

		olddataitemstoleft += itemsz;
	}

	/*
	 * Record a split after all original page data items, but before newitem.
	 * (Though only when it's possible that newitem will end up alone on new
	 * right page.)
	 */
	Assert(olddataitemstoleft == olddataitemstotal);
	if (newitemoff > maxoff)
		_bt_recsplitloc(&state, newitemoff, false, olddataitemstotal, 0);

	/*
	 * I believe it is not possible to fail to find a feasible split, but just
	 * in case ...
	 */
	if (state.nsplits == 0)
		elog(ERROR, "could not find a feasible split point for index \"%s\"",
			 RelationGetRelationName(rel));

	/*
	 * Start search for a split point among list of legal split points.  Give
	 * primary consideration to equalizing available free space in each half
	 * of the split initially (start with default strategy), while applying
	 * rightmost and split-after-new-item optimizations where appropriate.
	 * Either of the two other fallback strategies may be required for cases
	 * with a large number of duplicates around the original/space-optimal
	 * split point.
	 *
	 * Default strategy gives some weight to suffix truncation in deciding a
	 * split point on leaf pages.  It attempts to select a split point where a
	 * distinguishing attribute appears earlier in the new high key for the
	 * left side of the split, in order to maximize the number of trailing
	 * attributes that can be truncated away.  Only candidate split points
	 * that imply an acceptable balance of free space on each side are
	 * considered.  See _bt_defaultinterval().
	 */
	if (!state.is_leaf)
	{
		/* fillfactormult only used on rightmost page */
		usemult = state.is_rightmost;
		fillfactormult = BTREE_NONLEAF_FILLFACTOR / 100.0;
	}
	else if (state.is_rightmost)
	{
		/* Rightmost leaf page --  fillfactormult always used */
		usemult = true;
		fillfactormult = leaffillfactor / 100.0;
	}
	else if (_bt_afternewitemoff(&state, maxoff, leaffillfactor, &usemult))
	{
		/*
		 * New item inserted at rightmost point among a localized grouping on
		 * a leaf page -- apply "split after new item" optimization, either by
		 * applying leaf fillfactor multiplier, or by choosing the exact split
		 * point that leaves newitem as lastleft. (usemult is set for us.)
		 */
		if (usemult)
		{
			/* fillfactormult should be set based on leaf fillfactor */
			fillfactormult = leaffillfactor / 100.0;
		}
		else
		{
			/* find precise split point after newitemoff */
			for (int i = 0; i < state.nsplits; i++)
			{
				SplitPoint *split = state.splits + i;

				if (split->newitemonleft &&
					newitemoff == split->firstrightoff)
				{
					pfree(state.splits);
					*newitemonleft = true;
					return newitemoff;
				}
			}

			/*
			 * Cannot legally split after newitemoff; proceed with split
			 * without using fillfactor multiplier.  This is defensive, and
			 * should never be needed in practice.
			 */
			fillfactormult = 0.50;
		}
	}
	else
	{
		/* Other leaf page.  50:50 page split. */
		usemult = false;
		/* fillfactormult not used, but be tidy */
		fillfactormult = 0.50;
	}

	/*
	 * Save leftmost and rightmost splits for page before original ordinal
	 * sort order is lost by delta/fillfactormult sort
	 */
	leftpage = state.splits[0];
	rightpage = state.splits[state.nsplits - 1];

	/* Give split points a fillfactormult-wise delta, and sort on deltas */
	_bt_deltasortsplits(&state, fillfactormult, usemult);

	/* Determine split interval for default strategy */
	state.interval = _bt_defaultinterval(&state);

	/*
	 * Determine if default strategy/split interval will produce a
	 * sufficiently distinguishing split, or if we should change strategies.
	 * Alternative strategies change the range of split points that are
	 * considered acceptable (split interval), and possibly change
	 * fillfactormult, in order to deal with pages with a large number of
	 * duplicates gracefully.
	 *
	 * Pass low and high splits for the entire page (actually, they're for an
	 * imaginary version of the page that includes newitem).  These are used
	 * when the initial split interval encloses split points that are full of
	 * duplicates, and we need to consider if it's even possible to avoid
	 * appending a heap TID.
	 */
	perfectpenalty = _bt_strategy(&state, &leftpage, &rightpage, &strategy);

	if (strategy == SPLIT_DEFAULT)
	{
		/*
		 * Default strategy worked out (always works out with internal page).
		 * Original split interval still stands.
		 */
	}

	/*
	 * Many duplicates strategy is used when a heap TID would otherwise be
	 * appended, but the page isn't completely full of logical duplicates.
	 *
	 * The split interval is widened to include all legal candidate split
	 * points.  There might be a few as two distinct values in the whole-page
	 * split interval, though it's also possible that most of the values on
	 * the page are unique.  The final split point will either be to the
	 * immediate left or to the immediate right of the group of duplicate
	 * tuples that enclose the first/delta-optimal split point (perfect
	 * penalty was set so that the lowest delta split point that avoids
	 * appending a heap TID will be chosen).  Maximizing the number of
	 * attributes that can be truncated away is not a goal of the many
	 * duplicates strategy.
	 *
	 * Single value strategy is used when it is impossible to avoid appending
	 * a heap TID.  It arranges to leave the left page very full.  This
	 * maximizes space utilization in cases where tuples with the same
	 * attribute values span many pages.  Newly inserted duplicates will tend
	 * to have higher heap TID values, so we'll end up splitting to the right
	 * consistently.  (Single value strategy is harmless though not
	 * particularly useful with !heapkeyspace indexes.)
	 */
	else if (strategy == SPLIT_MANY_DUPLICATES)
	{
		Assert(state.is_leaf);
		/* Shouldn't try to truncate away extra user attributes */
		Assert(perfectpenalty ==
			   IndexRelationGetNumberOfKeyAttributes(state.rel));
		/* No need to resort splits -- no change in fillfactormult/deltas */
		state.interval = state.nsplits;
	}
	else if (strategy == SPLIT_SINGLE_VALUE)
	{
		Assert(state.is_leaf);
		/* Split near the end of the page */
		usemult = true;
		fillfactormult = BTREE_SINGLEVAL_FILLFACTOR / 100.0;
		/* Resort split points with new delta */
		_bt_deltasortsplits(&state, fillfactormult, usemult);
		/* Appending a heap TID is unavoidable, so interval of 1 is fine */
		state.interval = 1;
	}

	/*
	 * Search among acceptable split points (using final split interval) for
	 * the entry that has the lowest penalty, and is therefore expected to
	 * maximize fan-out.  Sets *newitemonleft for us.
	 */
	firstrightoff = _bt_bestsplitloc(&state, perfectpenalty, newitemonleft,
									 strategy);
	pfree(state.splits);

	return firstrightoff;
}

/*
 * Subroutine to record a particular point between two tuples (possibly the
 * new item) on page (ie, combination of firstrightoff and newitemonleft
 * settings) in *state for later analysis.  This is also a convenient point to
 * check if the split is legal (if it isn't, it won't be recorded).
 *
 * firstrightoff is the offset of the first item on the original page that
 * goes to the right page, and firstrightofforigpagetuplesz is the size of
 * that tuple.  firstrightoff can be > max offset, which means that all the
 * old items go to the left page and only the new item goes to the right page.
 * We don't actually use firstrightofforigpagetuplesz in that case (actually,
 * we don't use it for _any_ split where the firstright tuple happens to be
 * newitem).
 *
 * olddataitemstoleft is the total size of all old items to the left of the
 * split point that is recorded here when legal.  Should not include
 * newitemsz, since that is handled here.
 */
static void
_bt_recsplitloc(FindSplitData *state,
				OffsetNumber firstrightoff,
				bool newitemonleft,
				int olddataitemstoleft,
				Size firstrightofforigpagetuplesz)
{
	int16		leftfree,
				rightfree;
	Size		firstrightsz;
	Size		postingsz = 0;
	bool		newitemisfirstright;

	/* Is the new item going to be split point's firstright tuple? */
	newitemisfirstright = (firstrightoff == state->newitemoff &&
						   !newitemonleft);

	if (newitemisfirstright)
		firstrightsz = state->newitemsz;
	else
	{
		firstrightsz = firstrightofforigpagetuplesz;

		/*
		 * Calculate suffix truncation space saving when firstright tuple is a
		 * posting list tuple, though only when the tuple is over 64 bytes
		 * including line pointer overhead (arbitrary).  This avoids accessing
		 * the tuple in cases where its posting list must be very small (if
		 * tuple has one at all).
		 *
		 * Note: We don't do this in the case where firstright tuple is
		 * newitem, since newitem cannot have a posting list.
		 */
		if (state->is_leaf && firstrightsz > 64)
		{
			ItemId		itemid;
			IndexTuple	newhighkey;

			itemid = PageGetItemId(state->origpage, firstrightoff);
			newhighkey = (IndexTuple) PageGetItem(state->origpage, itemid);

			if (BTreeTupleIsPosting(newhighkey))
				postingsz = IndexTupleSize(newhighkey) -
					BTreeTupleGetPostingOffset(newhighkey);
		}
	}

	/* Account for all the old tuples */
	leftfree = state->leftspace - olddataitemstoleft;
	rightfree = state->rightspace -
		(state->olddataitemstotal - olddataitemstoleft);

	/*
	 * The first item on the right page becomes the high key of the left page;
	 * therefore it counts against left space as well as right space (we
	 * cannot assume that suffix truncation will make it any smaller).  When
	 * index has included attributes, then those attributes of left page high
	 * key will be truncated leaving that page with slightly more free space.
	 * However, that shouldn't affect our ability to find valid split
	 * location, since we err in the direction of being pessimistic about free
	 * space on the left half.  Besides, even when suffix truncation of
	 * non-TID attributes occurs, the new high key often won't even be a
	 * single MAXALIGN() quantum smaller than the firstright tuple it's based
	 * on.
	 *
	 * If we are on the leaf level, assume that suffix truncation cannot avoid
	 * adding a heap TID to the left half's new high key when splitting at the
	 * leaf level.  In practice the new high key will often be smaller and
	 * will rarely be larger, but conservatively assume the worst case.  We do
	 * go to the trouble of subtracting away posting list overhead, though
	 * only when it looks like it will make an appreciable difference.
	 * (Posting lists are the only case where truncation will typically make
	 * the final high key far smaller than firstright, so being a bit more
	 * precise there noticeably improves the balance of free space.)
	 */
	if (state->is_leaf)
		leftfree -= (int16) (firstrightsz +
							 MAXALIGN(sizeof(ItemPointerData)) -
							 postingsz);
	else
		leftfree -= (int16) firstrightsz;

	/* account for the new item */
	if (newitemonleft)
		leftfree -= (int16) state->newitemsz;
	else
		rightfree -= (int16) state->newitemsz;

	/*
	 * If we are not on the leaf level, we will be able to discard the key
	 * data from the first item that winds up on the right page.
	 */
	if (!state->is_leaf)
		rightfree += (int16) firstrightsz -
			(int16) (MAXALIGN(sizeof(IndexTupleData)) + sizeof(ItemIdData));

	/* Record split if legal */
	if (leftfree >= 0 && rightfree >= 0)
	{
		Assert(state->nsplits < state->maxsplits);

		/* Determine smallest firstright tuple size among legal splits */
		state->minfirstrightsz = Min(state->minfirstrightsz, firstrightsz);

		state->splits[state->nsplits].curdelta = 0;
		state->splits[state->nsplits].leftfree = leftfree;
		state->splits[state->nsplits].rightfree = rightfree;
		state->splits[state->nsplits].firstrightoff = firstrightoff;
		state->splits[state->nsplits].newitemonleft = newitemonleft;
		state->nsplits++;
	}
}

/*
 * Subroutine to assign space deltas to materialized array of candidate split
 * points based on current fillfactor, and to sort array using that fillfactor
 */
static void
_bt_deltasortsplits(FindSplitData *state, double fillfactormult,
					bool usemult)
{
	for (int i = 0; i < state->nsplits; i++)
	{
		SplitPoint *split = state->splits + i;
		int16		delta;

		if (usemult)
			delta = fillfactormult * split->leftfree -
				(1.0 - fillfactormult) * split->rightfree;
		else
			delta = split->leftfree - split->rightfree;

		if (delta < 0)
			delta = -delta;

		/* Save delta */
		split->curdelta = delta;
	}

	qsort(state->splits, state->nsplits, sizeof(SplitPoint), _bt_splitcmp);
}

/*
 * qsort-style comparator used by _bt_deltasortsplits()
 */
static int
_bt_splitcmp(const void *arg1, const void *arg2)
{
	SplitPoint *split1 = (SplitPoint *) arg1;
	SplitPoint *split2 = (SplitPoint *) arg2;

	if (split1->curdelta > split2->curdelta)
		return 1;
	if (split1->curdelta < split2->curdelta)
		return -1;

	return 0;
}

/*
 * Subroutine to determine whether or not a non-rightmost leaf page should be
 * split immediately after the would-be original page offset for the
 * new/incoming tuple (or should have leaf fillfactor applied when new item is
 * to the right on original page).  This is appropriate when there is a
 * pattern of localized monotonically increasing insertions into a composite
 * index, where leading attribute values form local groupings, and we
 * anticipate further insertions of the same/current grouping (new item's
 * grouping) in the near future.  This can be thought of as a variation on
 * applying leaf fillfactor during rightmost leaf page splits, since cases
 * that benefit will converge on packing leaf pages leaffillfactor% full over
 * time.
 *
 * We may leave extra free space remaining on the rightmost page of a "most
 * significant column" grouping of tuples if that grouping never ends up
 * having future insertions that use the free space.  That effect is
 * self-limiting; a future grouping that becomes the "nearest on the right"
 * grouping of the affected grouping usually puts the extra free space to good
 * use.
 *
 * Caller uses optimization when routine returns true, though the exact action
 * taken by caller varies.  Caller uses original leaf page fillfactor in
 * standard way rather than using the new item offset directly when *usemult
 * was also set to true here.  Otherwise, caller applies optimization by
 * locating the legal split point that makes the new tuple the lastleft tuple
 * for the split.
 */
static bool
_bt_afternewitemoff(FindSplitData *state, OffsetNumber maxoff,
					int leaffillfactor, bool *usemult)
{
	int16		nkeyatts;
	ItemId		itemid;
	IndexTuple	tup;
	int			keepnatts;

	Assert(state->is_leaf && !state->is_rightmost);

	nkeyatts = IndexRelationGetNumberOfKeyAttributes(state->rel);

	/* Single key indexes not considered here */
	if (nkeyatts == 1)
		return false;

	/* Ascending insertion pattern never inferred when new item is first */
	if (state->newitemoff == P_FIRSTKEY)
		return false;

	/*
	 * Only apply optimization on pages with equisized tuples, since ordinal
	 * keys are likely to be fixed-width.  Testing if the new tuple is
	 * variable width directly might also work, but that fails to apply the
	 * optimization to indexes with a numeric_ops attribute.
	 *
	 * Conclude that page has equisized tuples when the new item is the same
	 * width as the smallest item observed during pass over page, and other
	 * non-pivot tuples must be the same width as well.  (Note that the
	 * possibly-truncated existing high key isn't counted in
	 * olddataitemstotal, and must be subtracted from maxoff.)
	 */
	if (state->newitemsz != state->minfirstrightsz)
		return false;
	if (state->newitemsz * (maxoff - 1) != state->olddataitemstotal)
		return false;

	/*
	 * Avoid applying optimization when tuples are wider than a tuple
	 * consisting of two non-NULL int8/int64 attributes (or four non-NULL
	 * int4/int32 attributes)
	 */
	if (state->newitemsz >
		MAXALIGN(sizeof(IndexTupleData) + sizeof(int64) * 2) +
		sizeof(ItemIdData))
		return false;

	/*
	 * At least the first attribute's value must be equal to the corresponding
	 * value in previous tuple to apply optimization.  New item cannot be a
	 * duplicate, either.
	 *
	 * Handle case where new item is to the right of all items on the existing
	 * page.  This is suggestive of monotonically increasing insertions in
	 * itself, so the "heap TID adjacency" test is not applied here.
	 */
	if (state->newitemoff > maxoff)
	{
		itemid = PageGetItemId(state->origpage, maxoff);
		tup = (IndexTuple) PageGetItem(state->origpage, itemid);
		keepnatts = _bt_keep_natts_fast(state->rel, tup, state->newitem);

		if (keepnatts > 1 && keepnatts <= nkeyatts)
		{
			*usemult = true;
			return true;
		}

		return false;
	}

	/*
	 * "Low cardinality leading column, high cardinality suffix column"
	 * indexes with a random insertion pattern (e.g., an index with a boolean
	 * column, such as an index on '(book_is_in_print, book_isbn)') present us
	 * with a risk of consistently misapplying the optimization.  We're
	 * willing to accept very occasional misapplication of the optimization,
	 * provided the cases where we get it wrong are rare and self-limiting.
	 *
	 * Heap TID adjacency strongly suggests that the item just to the left was
	 * inserted very recently, which limits overapplication of the
	 * optimization.  Besides, all inappropriate cases triggered here will
	 * still split in the middle of the page on average.
	 */
	itemid = PageGetItemId(state->origpage, OffsetNumberPrev(state->newitemoff));
	tup = (IndexTuple) PageGetItem(state->origpage, itemid);
	/* Do cheaper test first */
	if (BTreeTupleIsPosting(tup) ||
		!_bt_adjacenthtid(&tup->t_tid, &state->newitem->t_tid))
		return false;
	/* Check same conditions as rightmost item case, too */
	keepnatts = _bt_keep_natts_fast(state->rel, tup, state->newitem);

	if (keepnatts > 1 && keepnatts <= nkeyatts)
	{
		double		interp = (double) state->newitemoff / ((double) maxoff + 1);
		double		leaffillfactormult = (double) leaffillfactor / 100.0;

		/*
		 * Don't allow caller to split after a new item when it will result in
		 * a split point to the right of the point that a leaf fillfactor
		 * split would use -- have caller apply leaf fillfactor instead
		 */
		*usemult = interp > leaffillfactormult;

		return true;
	}

	return false;
}

/*
 * Subroutine for determining if two heap TIDS are "adjacent".
 *
 * Adjacent means that the high TID is very likely to have been inserted into
 * heap relation immediately after the low TID, probably during the current
 * transaction.
 */
static bool
_bt_adjacenthtid(ItemPointer lowhtid, ItemPointer highhtid)
{
	BlockNumber lowblk,
				highblk;

	lowblk = ItemPointerGetBlockNumber(lowhtid);
	highblk = ItemPointerGetBlockNumber(highhtid);

	/* Make optimistic assumption of adjacency when heap blocks match */
	if (lowblk == highblk)
		return true;

	/* When heap block one up, second offset should be FirstOffsetNumber */
	if (lowblk + 1 == highblk &&
		ItemPointerGetOffsetNumber(highhtid) == FirstOffsetNumber)
		return true;

	return false;
}

/*
 * Subroutine to find the "best" split point among candidate split points.
 * The best split point is the split point with the lowest penalty among split
 * points that fall within current/final split interval.  Penalty is an
 * abstract score, with a definition that varies depending on whether we're
 * splitting a leaf page or an internal page.  See _bt_split_penalty() for
 * details.
 *
 * "perfectpenalty" is assumed to be the lowest possible penalty among
 * candidate split points.  This allows us to return early without wasting
 * cycles on calculating the first differing attribute for all candidate
 * splits when that clearly cannot improve our choice (or when we only want a
 * minimally distinguishing split point, and don't want to make the split any
 * more unbalanced than is necessary).
 *
 * We return the index of the first existing tuple that should go on the right
 * page, plus a boolean indicating if new item is on left of split point.
 */
static OffsetNumber
_bt_bestsplitloc(FindSplitData *state, int perfectpenalty,
				 bool *newitemonleft, FindSplitStrat strategy)
{
	int			bestpenalty,
				lowsplit;
	int			highsplit = Min(state->interval, state->nsplits);
	SplitPoint *final;

	bestpenalty = INT_MAX;
	lowsplit = 0;
	for (int i = lowsplit; i < highsplit; i++)
	{
		int			penalty;

		penalty = _bt_split_penalty(state, state->splits + i);

		if (penalty < bestpenalty)
		{
			bestpenalty = penalty;
			lowsplit = i;
		}

		if (penalty <= perfectpenalty)
			break;
	}

	final = &state->splits[lowsplit];

	/*
	 * There is a risk that the "many duplicates" strategy will repeatedly do
	 * the wrong thing when there are monotonically decreasing insertions to
	 * the right of a large group of duplicates.   Repeated splits could leave
	 * a succession of right half pages with free space that can never be
	 * used.  This must be avoided.
	 *
	 * Consider the example of the leftmost page in a single integer attribute
	 * NULLS FIRST index which is almost filled with NULLs.  Monotonically
	 * decreasing integer insertions might cause the same leftmost page to
	 * split repeatedly at the same point.  Each split derives its new high
	 * key from the lowest current value to the immediate right of the large
	 * group of NULLs, which will always be higher than all future integer
	 * insertions, directing all future integer insertions to the same
	 * leftmost page.
	 */
	if (strategy == SPLIT_MANY_DUPLICATES && !state->is_rightmost &&
		!final->newitemonleft && final->firstrightoff >= state->newitemoff &&
		final->firstrightoff < state->newitemoff + 9)
	{
		/*
		 * Avoid the problem by performing a 50:50 split when the new item is
		 * just to the right of the would-be "many duplicates" split point.
		 * (Note that the test used for an insert that is "just to the right"
		 * of the split point is conservative.)
		 */
		final = &state->splits[0];
	}

	*newitemonleft = final->newitemonleft;
	return final->firstrightoff;
}

#define LEAF_SPLIT_DISTANCE			0.050
#define INTERNAL_SPLIT_DISTANCE		0.075

/*
 * Return a split interval to use for the default strategy.  This is a limit
 * on the number of candidate split points to give further consideration to.
 * Only a fraction of all candidate splits points (those located at the start
 * of the now-sorted splits array) fall within the split interval.  Split
 * interval is applied within _bt_bestsplitloc().
 *
 * Split interval represents an acceptable range of split points -- those that
 * have leftfree and rightfree values that are acceptably balanced.  The final
 * split point chosen is the split point with the lowest "penalty" among split
 * points in this split interval (unless we change our entire strategy, in
 * which case the interval also changes -- see _bt_strategy()).
 *
 * The "Prefix B-Trees" paper calls split interval sigma l for leaf splits,
 * and sigma b for internal ("branch") splits.  It's hard to provide a
 * theoretical justification for the size of the split interval, though it's
 * clear that a small split interval can make tuples on level L+1 much smaller
 * on average, without noticeably affecting space utilization on level L.
 * (Note that the way that we calculate split interval might need to change if
 * suffix truncation is taught to truncate tuples "within" the last
 * attribute/datum for data types like text, which is more or less how it is
 * assumed to work in the paper.)
 */
static int
_bt_defaultinterval(FindSplitData *state)
{
	SplitPoint *spaceoptimal;
	int16		tolerance,
				lowleftfree,
				lowrightfree,
				highleftfree,
				highrightfree;

	/*
	 * Determine leftfree and rightfree values that are higher and lower than
	 * we're willing to tolerate.  Note that the final split interval will be
	 * about 10% of nsplits in the common case where all non-pivot tuples
	 * (data items) from a leaf page are uniformly sized.  We're a bit more
	 * aggressive when splitting internal pages.
	 */
	if (state->is_leaf)
		tolerance = state->olddataitemstotal * LEAF_SPLIT_DISTANCE;
	else
		tolerance = state->olddataitemstotal * INTERNAL_SPLIT_DISTANCE;

	/* First candidate split point is the most evenly balanced */
	spaceoptimal = state->splits;
	lowleftfree = spaceoptimal->leftfree - tolerance;
	lowrightfree = spaceoptimal->rightfree - tolerance;
	highleftfree = spaceoptimal->leftfree + tolerance;
	highrightfree = spaceoptimal->rightfree + tolerance;

	/*
	 * Iterate through split points, starting from the split immediately after
	 * 'spaceoptimal'.  Find the first split point that divides free space so
	 * unevenly that including it in the split interval would be unacceptable.
	 */
	for (int i = 1; i < state->nsplits; i++)
	{
		SplitPoint *split = state->splits + i;

		/* Cannot use curdelta here, since its value is often weighted */
		if (split->leftfree < lowleftfree || split->rightfree < lowrightfree ||
			split->leftfree > highleftfree || split->rightfree > highrightfree)
			return i;
	}

	return state->nsplits;
}

/*
 * Subroutine to decide whether split should use default strategy/initial
 * split interval, or whether it should finish splitting the page using
 * alternative strategies (this is only possible with leaf pages).
 *
 * Caller uses alternative strategy (or sticks with default strategy) based
 * on how *strategy is set here.  Return value is "perfect penalty", which is
 * passed to _bt_bestsplitloc() as a final constraint on how far caller is
 * willing to go to avoid appending a heap TID when using the many duplicates
 * strategy (it also saves _bt_bestsplitloc() useless cycles).
 */
static int
_bt_strategy(FindSplitData *state, SplitPoint *leftpage,
			 SplitPoint *rightpage, FindSplitStrat *strategy)
{
	IndexTuple	leftmost,
				rightmost;
	SplitPoint *leftinterval,
			   *rightinterval;
	int			perfectpenalty;
	int			indnkeyatts = IndexRelationGetNumberOfKeyAttributes(state->rel);

	/* Assume that alternative strategy won't be used for now */
	*strategy = SPLIT_DEFAULT;

	/*
	 * Use smallest observed firstright item size for entire page (actually,
	 * entire imaginary version of page that includes newitem) as perfect
	 * penalty on internal pages.  This can save cycles in the common case
	 * where most or all splits (not just splits within interval) have
	 * firstright tuples that are the same size.
	 */
	if (!state->is_leaf)
		return state->minfirstrightsz;

	/*
	 * Use leftmost and rightmost tuples from leftmost and rightmost splits in
	 * current split interval
	 */
	_bt_interval_edges(state, &leftinterval, &rightinterval);
	leftmost = _bt_split_lastleft(state, leftinterval);
	rightmost = _bt_split_firstright(state, rightinterval);

	/*
	 * If initial split interval can produce a split point that will at least
	 * avoid appending a heap TID in new high key, we're done.  Finish split
	 * with default strategy and initial split interval.
	 */
	perfectpenalty = _bt_keep_natts_fast(state->rel, leftmost, rightmost);
	if (perfectpenalty <= indnkeyatts)
		return perfectpenalty;

	/*
	 * Work out how caller should finish split when even their "perfect"
	 * penalty for initial/default split interval indicates that the interval
	 * does not contain even a single split that avoids appending a heap TID.
	 *
	 * Use the leftmost split's lastleft tuple and the rightmost split's
	 * firstright tuple to assess every possible split.
	 */
	leftmost = _bt_split_lastleft(state, leftpage);
	rightmost = _bt_split_firstright(state, rightpage);

	/*
	 * If page (including new item) has many duplicates but is not entirely
	 * full of duplicates, a many duplicates strategy split will be performed.
	 * If page is entirely full of duplicates, a single value strategy split
	 * will be performed.
	 */
	perfectpenalty = _bt_keep_natts_fast(state->rel, leftmost, rightmost);
	if (perfectpenalty <= indnkeyatts)
	{
		*strategy = SPLIT_MANY_DUPLICATES;

		/*
		 * Many duplicates strategy should split at either side the group of
		 * duplicates that enclose the delta-optimal split point.  Return
		 * indnkeyatts rather than the true perfect penalty to make that
		 * happen.  (If perfectpenalty was returned here then low cardinality
		 * composite indexes could have continual unbalanced splits.)
		 *
		 * Note that caller won't go through with a many duplicates split in
		 * rare cases where it looks like there are ever-decreasing insertions
		 * to the immediate right of the split point.  This must happen just
		 * before a final decision is made, within _bt_bestsplitloc().
		 */
		return indnkeyatts;
	}

	/*
	 * Single value strategy is only appropriate with ever-increasing heap
	 * TIDs; otherwise, original default strategy split should proceed to
	 * avoid pathological performance.  Use page high key to infer if this is
	 * the rightmost page among pages that store the same duplicate value.
	 * This should not prevent insertions of heap TIDs that are slightly out
	 * of order from using single value strategy, since that's expected with
	 * concurrent inserters of the same duplicate value.
	 */
	else if (state->is_rightmost)
		*strategy = SPLIT_SINGLE_VALUE;
	else
	{
		ItemId		itemid;
		IndexTuple	hikey;

		itemid = PageGetItemId(state->origpage, P_HIKEY);
		hikey = (IndexTuple) PageGetItem(state->origpage, itemid);
		perfectpenalty = _bt_keep_natts_fast(state->rel, hikey,
											 state->newitem);
		if (perfectpenalty <= indnkeyatts)
			*strategy = SPLIT_SINGLE_VALUE;
		else
		{
			/*
			 * Have caller finish split using default strategy, since page
			 * does not appear to be the rightmost page for duplicates of the
			 * value the page is filled with
			 */
		}
	}

	return perfectpenalty;
}

/*
 * Subroutine to locate leftmost and rightmost splits for current/default
 * split interval.  Note that it will be the same split iff there is only one
 * split in interval.
 */
static void
_bt_interval_edges(FindSplitData *state, SplitPoint **leftinterval,
				   SplitPoint **rightinterval)
{
	int			highsplit = Min(state->interval, state->nsplits);
	SplitPoint *deltaoptimal;

	deltaoptimal = state->splits;
	*leftinterval = NULL;
	*rightinterval = NULL;

	/*
	 * Delta is an absolute distance to optimal split point, so both the
	 * leftmost and rightmost split point will usually be at the end of the
	 * array
	 */
	for (int i = highsplit - 1; i >= 0; i--)
	{
		SplitPoint *distant = state->splits + i;

		if (distant->firstrightoff < deltaoptimal->firstrightoff)
		{
			if (*leftinterval == NULL)
				*leftinterval = distant;
		}
		else if (distant->firstrightoff > deltaoptimal->firstrightoff)
		{
			if (*rightinterval == NULL)
				*rightinterval = distant;
		}
		else if (!distant->newitemonleft && deltaoptimal->newitemonleft)
		{
			/*
			 * "incoming tuple will become firstright" (distant) is to the
			 * left of "incoming tuple will become lastleft" (delta-optimal)
			 */
			Assert(distant->firstrightoff == state->newitemoff);
			if (*leftinterval == NULL)
				*leftinterval = distant;
		}
		else if (distant->newitemonleft && !deltaoptimal->newitemonleft)
		{
			/*
			 * "incoming tuple will become lastleft" (distant) is to the right
			 * of "incoming tuple will become firstright" (delta-optimal)
			 */
			Assert(distant->firstrightoff == state->newitemoff);
			if (*rightinterval == NULL)
				*rightinterval = distant;
		}
		else
		{
			/* There was only one or two splits in initial split interval */
			Assert(distant == deltaoptimal);
			if (*leftinterval == NULL)
				*leftinterval = distant;
			if (*rightinterval == NULL)
				*rightinterval = distant;
		}

		if (*leftinterval && *rightinterval)
			return;
	}

	Assert(false);
}

/*
 * Subroutine to find penalty for caller's candidate split point.
 *
 * On leaf pages, penalty is the attribute number that distinguishes each side
 * of a split.  It's the last attribute that needs to be included in new high
 * key for left page.  It can be greater than the number of key attributes in
 * cases where a heap TID will need to be appended during truncation.
 *
 * On internal pages, penalty is simply the size of the firstright tuple for
 * the split (including line pointer overhead).  This tuple will become the
 * new high key for the left page.
 */
static inline int
_bt_split_penalty(FindSplitData *state, SplitPoint *split)
{
	IndexTuple	lastleft;
	IndexTuple	firstright;

	if (!state->is_leaf)
	{
		ItemId		itemid;

		if (!split->newitemonleft &&
			split->firstrightoff == state->newitemoff)
			return state->newitemsz;

		itemid = PageGetItemId(state->origpage, split->firstrightoff);

		return MAXALIGN(ItemIdGetLength(itemid)) + sizeof(ItemIdData);
	}

	lastleft = _bt_split_lastleft(state, split);
	firstright = _bt_split_firstright(state, split);

	return _bt_keep_natts_fast(state->rel, lastleft, firstright);
}

/*
 * Subroutine to get a lastleft IndexTuple for a split point
 */
static inline IndexTuple
_bt_split_lastleft(FindSplitData *state, SplitPoint *split)
{
	ItemId		itemid;

	if (split->newitemonleft && split->firstrightoff == state->newitemoff)
		return state->newitem;

	itemid = PageGetItemId(state->origpage,
						   OffsetNumberPrev(split->firstrightoff));
	return (IndexTuple) PageGetItem(state->origpage, itemid);
}

/*
 * Subroutine to get a firstright IndexTuple for a split point
 */
static inline IndexTuple
_bt_split_firstright(FindSplitData *state, SplitPoint *split)
{
	ItemId		itemid;

	if (!split->newitemonleft && split->firstrightoff == state->newitemoff)
		return state->newitem;

	itemid = PageGetItemId(state->origpage, split->firstrightoff);
	return (IndexTuple) PageGetItem(state->origpage, itemid);
}
