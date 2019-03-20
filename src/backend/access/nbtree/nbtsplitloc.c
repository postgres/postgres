/*-------------------------------------------------------------------------
 *
 * nbtsplitloc.c
 *	  Choose split point code for Postgres btree implementation.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
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

/* limits on split interval (default strategy only) */
#define MAX_LEAF_INTERVAL			9
#define MAX_INTERNAL_INTERVAL		18

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
	OffsetNumber firstoldonright;	/* first item on new right page */
	bool		newitemonleft;	/* new item goes on left, or right? */

} SplitPoint;

typedef struct
{
	/* context data for _bt_recsplitloc */
	Relation	rel;			/* index relation */
	Page		page;			/* page undergoing split */
	IndexTuple	newitem;		/* new item (cause of page split) */
	Size		newitemsz;		/* size of newitem (includes line pointer) */
	bool		is_leaf;		/* T if splitting a leaf page */
	bool		is_rightmost;	/* T if splitting rightmost page on level */
	OffsetNumber newitemoff;	/* where the new item is to be inserted */
	int			leftspace;		/* space available for items on left page */
	int			rightspace;		/* space available for items on right page */
	int			olddataitemstotal;	/* space taken by old items */
	Size		minfirstrightsz;	/* smallest firstoldonright tuple size */

	/* candidate split point data */
	int			maxsplits;		/* maximum number of splits */
	int			nsplits;		/* current number of splits */
	SplitPoint *splits;			/* all candidate split points for page */
	int			interval;		/* current range of acceptable split points */
} FindSplitData;

static void _bt_recsplitloc(FindSplitData *state,
				OffsetNumber firstoldonright, bool newitemonleft,
				int olddataitemstoleft, Size firstoldonrightsz);
static void _bt_deltasortsplits(FindSplitData *state, double fillfactormult,
					bool usemult);
static int	_bt_splitcmp(const void *arg1, const void *arg2);
static OffsetNumber _bt_bestsplitloc(FindSplitData *state, int perfectpenalty,
				 bool *newitemonleft);
static int _bt_strategy(FindSplitData *state, SplitPoint *leftpage,
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
 * righthand page, plus a boolean indicating whether the new tuple goes on
 * the left or right page.  The bool is necessary to disambiguate the case
 * where firstright == newitemoff.
 */
OffsetNumber
_bt_findsplitloc(Relation rel,
				 Page page,
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
				foundfirstright;
	double		fillfactormult;
	bool		usemult;
	SplitPoint	leftpage,
				rightpage;

	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	maxoff = PageGetMaxOffsetNumber(page);

	/* Total free space available on a btree page, after fixed overhead */
	leftspace = rightspace =
		PageGetPageSize(page) - SizeOfPageHeaderData -
		MAXALIGN(sizeof(BTPageOpaqueData));

	/* The right page will have the same high key as the old page */
	if (!P_RIGHTMOST(opaque))
	{
		itemid = PageGetItemId(page, P_HIKEY);
		rightspace -= (int) (MAXALIGN(ItemIdGetLength(itemid)) +
							 sizeof(ItemIdData));
	}

	/* Count up total space in data items before actually scanning 'em */
	olddataitemstotal = rightspace - (int) PageGetExactFreeSpace(page);
	leaffillfactor = RelationGetFillFactor(rel, BTREE_DEFAULT_FILLFACTOR);

	/* Passed-in newitemsz is MAXALIGNED but does not include line pointer */
	newitemsz += sizeof(ItemIdData);
	state.rel = rel;
	state.page = page;
	state.newitem = newitem;
	state.newitemsz = newitemsz;
	state.is_leaf = P_ISLEAF(opaque);
	state.is_rightmost = P_RIGHTMOST(opaque);
	state.leftspace = leftspace;
	state.rightspace = rightspace;
	state.olddataitemstotal = olddataitemstotal;
	state.minfirstrightsz = SIZE_MAX;
	state.newitemoff = newitemoff;

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
	 * each possible position.  We start at the first data offset rather than
	 * the second data offset to handle the "newitemoff == first data offset"
	 * case (any other split whose firstoldonright is the first data offset
	 * can't be legal, though, and so won't actually end up being recorded in
	 * first loop iteration).
	 */
	olddataitemstoleft = 0;

	for (offnum = P_FIRSTDATAKEY(opaque);
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		Size		itemsz;

		itemid = PageGetItemId(page, offnum);
		itemsz = MAXALIGN(ItemIdGetLength(itemid)) + sizeof(ItemIdData);

		/*
		 * Will the new item go to left or right of split?
		 */
		if (offnum > newitemoff)
			_bt_recsplitloc(&state, offnum, true, olddataitemstoleft, itemsz);
		else if (offnum < newitemoff)
			_bt_recsplitloc(&state, offnum, false, olddataitemstoleft, itemsz);
		else
		{
			/* may need to record a split on one or both sides of new item */
			_bt_recsplitloc(&state, offnum, true, olddataitemstoleft, itemsz);
			_bt_recsplitloc(&state, offnum, false, olddataitemstoleft, itemsz);
		}

		olddataitemstoleft += itemsz;
	}

	/*
	 * If the new item goes as the last item, record the split point that
	 * leaves all the old items on the left page, and the new item on the
	 * right page.  This is required because a split that leaves the new item
	 * as the firstoldonright won't have been reached within the loop.
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
	 * rightmost optimization where appropriate.  Either of the two other
	 * fallback strategies may be required for cases with a large number of
	 * duplicates around the original/space-optimal split point.
	 *
	 * Default strategy gives some weight to suffix truncation in deciding a
	 * split point on leaf pages.  It attempts to select a split point where a
	 * distinguishing attribute appears earlier in the new high key for the
	 * left side of the split, in order to maximize the number of trailing
	 * attributes that can be truncated away.  Only candidate split points
	 * that imply an acceptable balance of free space on each side are
	 * considered.
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
	else
	{
		/* Other leaf page.  50:50 page split. */
		usemult = false;
		/* fillfactormult not used, but be tidy */
		fillfactormult = 0.50;
	}

	/*
	 * Set an initial limit on the split interval/number of candidate split
	 * points as appropriate.  The "Prefix B-Trees" paper refers to this as
	 * sigma l for leaf splits and sigma b for internal ("branch") splits.
	 * It's hard to provide a theoretical justification for the initial size
	 * of the split interval, though it's clear that a small split interval
	 * makes suffix truncation much more effective without noticeably
	 * affecting space utilization over time.
	 */
	state.interval = Min(Max(1, state.nsplits * 0.05),
						 state.is_leaf ? MAX_LEAF_INTERVAL :
						 MAX_INTERNAL_INTERVAL);

	/*
	 * Save leftmost and rightmost splits for page before original ordinal
	 * sort order is lost by delta/fillfactormult sort
	 */
	leftpage = state.splits[0];
	rightpage = state.splits[state.nsplits - 1];

	/* Give split points a fillfactormult-wise delta, and sort on deltas */
	_bt_deltasortsplits(&state, fillfactormult, usemult);

	/*
	 * Determine if default strategy/split interval will produce a
	 * sufficiently distinguishing split, or if we should change strategies.
	 * Alternative strategies change the range of split points that are
	 * considered acceptable (split interval), and possibly change
	 * fillfactormult, in order to deal with pages with a large number of
	 * duplicates gracefully.
	 *
	 * Pass low and high splits for the entire page (including even newitem).
	 * These are used when the initial split interval encloses split points
	 * that are full of duplicates, and we need to consider if it's even
	 * possible to avoid appending a heap TID.
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
	 * points.  There may be a few as two distinct values in the whole-page
	 * split interval.  Many duplicates strategy has no hard requirements for
	 * space utilization, though it still keeps the use of space balanced as a
	 * non-binding secondary goal (perfect penalty is set so that the
	 * first/lowest delta split points that avoids appending a heap TID is
	 * used).
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
	foundfirstright = _bt_bestsplitloc(&state, perfectpenalty, newitemonleft);
	pfree(state.splits);

	return foundfirstright;
}

/*
 * Subroutine to record a particular point between two tuples (possibly the
 * new item) on page (ie, combination of firstright and newitemonleft
 * settings) in *state for later analysis.  This is also a convenient point
 * to check if the split is legal (if it isn't, it won't be recorded).
 *
 * firstoldonright is the offset of the first item on the original page that
 * goes to the right page, and firstoldonrightsz is the size of that tuple.
 * firstoldonright can be > max offset, which means that all the old items go
 * to the left page and only the new item goes to the right page.  In that
 * case, firstoldonrightsz is not used.
 *
 * olddataitemstoleft is the total size of all old items to the left of the
 * split point that is recorded here when legal.  Should not include
 * newitemsz, since that is handled here.
 */
static void
_bt_recsplitloc(FindSplitData *state,
				OffsetNumber firstoldonright,
				bool newitemonleft,
				int olddataitemstoleft,
				Size firstoldonrightsz)
{
	int16		leftfree,
				rightfree;
	Size		firstrightitemsz;
	bool		newitemisfirstonright;

	/* Is the new item going to be the first item on the right page? */
	newitemisfirstonright = (firstoldonright == state->newitemoff
							 && !newitemonleft);

	if (newitemisfirstonright)
		firstrightitemsz = state->newitemsz;
	else
		firstrightitemsz = firstoldonrightsz;

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
	 * will rarely be larger, but conservatively assume the worst case.
	 */
	if (state->is_leaf)
		leftfree -= (int16) (firstrightitemsz +
							 MAXALIGN(sizeof(ItemPointerData)));
	else
		leftfree -= (int16) firstrightitemsz;

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
		rightfree += (int16) firstrightitemsz -
			(int16) (MAXALIGN(sizeof(IndexTupleData)) + sizeof(ItemIdData));

	/* Record split if legal */
	if (leftfree >= 0 && rightfree >= 0)
	{
		Assert(state->nsplits < state->maxsplits);

		/* Determine smallest firstright item size on page */
		state->minfirstrightsz = Min(state->minfirstrightsz, firstrightitemsz);

		state->splits[state->nsplits].curdelta = 0;
		state->splits[state->nsplits].leftfree = leftfree;
		state->splits[state->nsplits].rightfree = rightfree;
		state->splits[state->nsplits].firstoldonright = firstoldonright;
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
 * Subroutine to find the "best" split point among an array of acceptable
 * candidate split points that split without there being an excessively high
 * delta between the space left free on the left and right halves.  The "best"
 * split point is the split point with the lowest penalty among split points
 * that fall within current/final split interval.  Penalty is an abstract
 * score, with a definition that varies depending on whether we're splitting a
 * leaf page or an internal page.  See _bt_split_penalty() for details.
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
_bt_bestsplitloc(FindSplitData *state, int perfectpenalty, bool *newitemonleft)
{
	int			bestpenalty,
				lowsplit;
	int			highsplit = Min(state->interval, state->nsplits);

	/* No point in calculating penalty when there's only one choice */
	if (state->nsplits == 1)
	{
		*newitemonleft = state->splits[0].newitemonleft;
		return state->splits[0].firstoldonright;
	}

	bestpenalty = INT_MAX;
	lowsplit = 0;
	for (int i = lowsplit; i < highsplit; i++)
	{
		int			penalty;

		penalty = _bt_split_penalty(state, state->splits + i);

		if (penalty <= perfectpenalty)
		{
			bestpenalty = penalty;
			lowsplit = i;
			break;
		}

		if (penalty < bestpenalty)
		{
			bestpenalty = penalty;
			lowsplit = i;
		}
	}

	*newitemonleft = state->splits[lowsplit].newitemonleft;
	return state->splits[lowsplit].firstoldonright;
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
	 * Use smallest observed first right item size for entire page as perfect
	 * penalty on internal pages.  This can save cycles in the common case
	 * where most or all splits (not just splits within interval) have first
	 * right tuples that are the same size.
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
		 * Caller should choose the lowest delta split that avoids appending a
		 * heap TID.  Maximizing the number of attributes that can be
		 * truncated away (returning perfectpenalty when it happens to be less
		 * than the number of key attributes in index) can result in continual
		 * unbalanced page splits.
		 *
		 * Just avoiding appending a heap TID can still make splits very
		 * unbalanced, but this is self-limiting.  When final split has a very
		 * high delta, one side of the split will likely consist of a single
		 * value.  If that page is split once again, then that split will
		 * likely use the single value strategy.
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

		itemid = PageGetItemId(state->page, P_HIKEY);
		hikey = (IndexTuple) PageGetItem(state->page, itemid);
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

		if (distant->firstoldonright < deltaoptimal->firstoldonright)
		{
			if (*leftinterval == NULL)
				*leftinterval = distant;
		}
		else if (distant->firstoldonright > deltaoptimal->firstoldonright)
		{
			if (*rightinterval == NULL)
				*rightinterval = distant;
		}
		else if (!distant->newitemonleft && deltaoptimal->newitemonleft)
		{
			/*
			 * "incoming tuple will become first on right page" (distant) is
			 * to the left of "incoming tuple will become last on left page"
			 * (delta-optimal)
			 */
			Assert(distant->firstoldonright == state->newitemoff);
			if (*leftinterval == NULL)
				*leftinterval = distant;
		}
		else if (distant->newitemonleft && !deltaoptimal->newitemonleft)
		{
			/*
			 * "incoming tuple will become last on left page" (distant) is to
			 * the right of "incoming tuple will become first on right page"
			 * (delta-optimal)
			 */
			Assert(distant->firstoldonright == state->newitemoff);
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
 * On internal pages, penalty is simply the size of the first item on the
 * right half of the split (including line pointer overhead).  This tuple will
 * become the new high key for the left page.
 */
static inline int
_bt_split_penalty(FindSplitData *state, SplitPoint *split)
{
	IndexTuple	lastleftuple;
	IndexTuple	firstrighttuple;

	if (!state->is_leaf)
	{
		ItemId		itemid;

		if (!split->newitemonleft &&
			split->firstoldonright == state->newitemoff)
			return state->newitemsz;

		itemid = PageGetItemId(state->page, split->firstoldonright);

		return MAXALIGN(ItemIdGetLength(itemid)) + sizeof(ItemIdData);
	}

	lastleftuple = _bt_split_lastleft(state, split);
	firstrighttuple = _bt_split_firstright(state, split);

	Assert(lastleftuple != firstrighttuple);
	return _bt_keep_natts_fast(state->rel, lastleftuple, firstrighttuple);
}

/*
 * Subroutine to get a lastleft IndexTuple for a spit point from page
 */
static inline IndexTuple
_bt_split_lastleft(FindSplitData *state, SplitPoint *split)
{
	ItemId		itemid;

	if (split->newitemonleft && split->firstoldonright == state->newitemoff)
		return state->newitem;

	itemid = PageGetItemId(state->page,
						   OffsetNumberPrev(split->firstoldonright));
	return (IndexTuple) PageGetItem(state->page, itemid);
}

/*
 * Subroutine to get a firstright IndexTuple for a spit point from page
 */
static inline IndexTuple
_bt_split_firstright(FindSplitData *state, SplitPoint *split)
{
	ItemId		itemid;

	if (!split->newitemonleft && split->firstoldonright == state->newitemoff)
		return state->newitem;

	itemid = PageGetItemId(state->page, split->firstoldonright);
	return (IndexTuple) PageGetItem(state->page, itemid);
}
