/*-------------------------------------------------------------------------
 *
 * gistproc.c
 *	  Support procedures for GiSTs over 2-D objects (boxes, polygons, circles,
 *	  points).
 *
 * This gives R-tree behavior, with Guttman's poly-time split algorithm.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	src/backend/access/gist/gistproc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/gist.h"
#include "access/stratnum.h"
#include "utils/float.h"
#include "utils/fmgrprotos.h"
#include "utils/geo_decls.h"
#include "utils/sortsupport.h"


static bool gist_box_leaf_consistent(BOX *key, BOX *query,
									 StrategyNumber strategy);
static bool rtree_internal_consistent(BOX *key, BOX *query,
									  StrategyNumber strategy);

static uint64 point_zorder_internal(float4 x, float4 y);
static uint64 part_bits32_by2(uint32 x);
static uint32 ieee_float32_to_uint32(float f);
static int	gist_bbox_zorder_cmp(Datum a, Datum b, SortSupport ssup);
static Datum gist_bbox_zorder_abbrev_convert(Datum original, SortSupport ssup);
static bool gist_bbox_zorder_abbrev_abort(int memtupcount, SortSupport ssup);


/* Minimum accepted ratio of split */
#define LIMIT_RATIO 0.3


/**************************************************
 * Box ops
 **************************************************/

/*
 * Calculates union of two boxes, a and b. The result is stored in *n.
 */
static void
rt_box_union(BOX *n, const BOX *a, const BOX *b)
{
	n->high.x = float8_max(a->high.x, b->high.x);
	n->high.y = float8_max(a->high.y, b->high.y);
	n->low.x = float8_min(a->low.x, b->low.x);
	n->low.y = float8_min(a->low.y, b->low.y);
}

/*
 * Size of a BOX for penalty-calculation purposes.
 * The result can be +Infinity, but not NaN.
 */
static float8
size_box(const BOX *box)
{
	/*
	 * Check for zero-width cases.  Note that we define the size of a zero-
	 * by-infinity box as zero.  It's important to special-case this somehow,
	 * as naively multiplying infinity by zero will produce NaN.
	 *
	 * The less-than cases should not happen, but if they do, say "zero".
	 */
	if (float8_le(box->high.x, box->low.x) ||
		float8_le(box->high.y, box->low.y))
		return 0.0;

	/*
	 * We treat NaN as larger than +Infinity, so any distance involving a NaN
	 * and a non-NaN is infinite.  Note the previous check eliminated the
	 * possibility that the low fields are NaNs.
	 */
	if (isnan(box->high.x) || isnan(box->high.y))
		return get_float8_infinity();
	return float8_mul(float8_mi(box->high.x, box->low.x),
					  float8_mi(box->high.y, box->low.y));
}

/*
 * Return amount by which the union of the two boxes is larger than
 * the original BOX's area.  The result can be +Infinity, but not NaN.
 */
static float8
box_penalty(const BOX *original, const BOX *new)
{
	BOX			unionbox;

	rt_box_union(&unionbox, original, new);
	return float8_mi(size_box(&unionbox), size_box(original));
}

/*
 * The GiST Consistent method for boxes
 *
 * Should return false if for all data items x below entry,
 * the predicate x op query must be false, where op is the oper
 * corresponding to strategy in the pg_amop table.
 */
Datum
gist_box_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	BOX		   *query = PG_GETARG_BOX_P(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);

	/* All cases served by this function are exact */
	*recheck = false;

	if (DatumGetBoxP(entry->key) == NULL || query == NULL)
		PG_RETURN_BOOL(false);

	/*
	 * if entry is not leaf, use rtree_internal_consistent, else use
	 * gist_box_leaf_consistent
	 */
	if (GIST_LEAF(entry))
		PG_RETURN_BOOL(gist_box_leaf_consistent(DatumGetBoxP(entry->key),
												query,
												strategy));
	else
		PG_RETURN_BOOL(rtree_internal_consistent(DatumGetBoxP(entry->key),
												 query,
												 strategy));
}

/*
 * Increase BOX b to include addon.
 */
static void
adjustBox(BOX *b, const BOX *addon)
{
	if (float8_lt(b->high.x, addon->high.x))
		b->high.x = addon->high.x;
	if (float8_gt(b->low.x, addon->low.x))
		b->low.x = addon->low.x;
	if (float8_lt(b->high.y, addon->high.y))
		b->high.y = addon->high.y;
	if (float8_gt(b->low.y, addon->low.y))
		b->low.y = addon->low.y;
}

/*
 * The GiST Union method for boxes
 *
 * returns the minimal bounding box that encloses all the entries in entryvec
 */
Datum
gist_box_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int		   *sizep = (int *) PG_GETARG_POINTER(1);
	int			numranges,
				i;
	BOX		   *cur,
			   *pageunion;

	numranges = entryvec->n;
	pageunion = (BOX *) palloc(sizeof(BOX));
	cur = DatumGetBoxP(entryvec->vector[0].key);
	memcpy(pageunion, cur, sizeof(BOX));

	for (i = 1; i < numranges; i++)
	{
		cur = DatumGetBoxP(entryvec->vector[i].key);
		adjustBox(pageunion, cur);
	}
	*sizep = sizeof(BOX);

	PG_RETURN_POINTER(pageunion);
}

/*
 * We store boxes as boxes in GiST indexes, so we do not need
 * compress, decompress, or fetch functions.
 */

/*
 * The GiST Penalty method for boxes (also used for points)
 *
 * As in the R-tree paper, we use change in area as our penalty metric
 */
Datum
gist_box_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *origentry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *newentry = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *result = (float *) PG_GETARG_POINTER(2);
	BOX		   *origbox = DatumGetBoxP(origentry->key);
	BOX		   *newbox = DatumGetBoxP(newentry->key);

	*result = (float) box_penalty(origbox, newbox);
	PG_RETURN_POINTER(result);
}

/*
 * Trivial split: half of entries will be placed on one page
 * and another half - to another
 */
static void
fallbackSplit(GistEntryVector *entryvec, GIST_SPLITVEC *v)
{
	OffsetNumber i,
				maxoff;
	BOX		   *unionL = NULL,
			   *unionR = NULL;
	int			nbytes;

	maxoff = entryvec->n - 1;

	nbytes = (maxoff + 2) * sizeof(OffsetNumber);
	v->spl_left = (OffsetNumber *) palloc(nbytes);
	v->spl_right = (OffsetNumber *) palloc(nbytes);
	v->spl_nleft = v->spl_nright = 0;

	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		BOX		   *cur = DatumGetBoxP(entryvec->vector[i].key);

		if (i <= (maxoff - FirstOffsetNumber + 1) / 2)
		{
			v->spl_left[v->spl_nleft] = i;
			if (unionL == NULL)
			{
				unionL = (BOX *) palloc(sizeof(BOX));
				*unionL = *cur;
			}
			else
				adjustBox(unionL, cur);

			v->spl_nleft++;
		}
		else
		{
			v->spl_right[v->spl_nright] = i;
			if (unionR == NULL)
			{
				unionR = (BOX *) palloc(sizeof(BOX));
				*unionR = *cur;
			}
			else
				adjustBox(unionR, cur);

			v->spl_nright++;
		}
	}

	v->spl_ldatum = BoxPGetDatum(unionL);
	v->spl_rdatum = BoxPGetDatum(unionR);
}

/*
 * Represents information about an entry that can be placed to either group
 * without affecting overlap over selected axis ("common entry").
 */
typedef struct
{
	/* Index of entry in the initial array */
	int			index;
	/* Delta between penalties of entry insertion into different groups */
	float8		delta;
} CommonEntry;

/*
 * Context for g_box_consider_split. Contains information about currently
 * selected split and some general information.
 */
typedef struct
{
	int			entriesCount;	/* total number of entries being split */
	BOX			boundingBox;	/* minimum bounding box across all entries */

	/* Information about currently selected split follows */

	bool		first;			/* true if no split was selected yet */

	float8		leftUpper;		/* upper bound of left interval */
	float8		rightLower;		/* lower bound of right interval */

	float4		ratio;
	float4		overlap;
	int			dim;			/* axis of this split */
	float8		range;			/* width of general MBR projection to the
								 * selected axis */
} ConsiderSplitContext;

/*
 * Interval represents projection of box to axis.
 */
typedef struct
{
	float8		lower,
				upper;
} SplitInterval;

/*
 * Interval comparison function by lower bound of the interval;
 */
static int
interval_cmp_lower(const void *i1, const void *i2)
{
	float8		lower1 = ((const SplitInterval *) i1)->lower,
				lower2 = ((const SplitInterval *) i2)->lower;

	return float8_cmp_internal(lower1, lower2);
}

/*
 * Interval comparison function by upper bound of the interval;
 */
static int
interval_cmp_upper(const void *i1, const void *i2)
{
	float8		upper1 = ((const SplitInterval *) i1)->upper,
				upper2 = ((const SplitInterval *) i2)->upper;

	return float8_cmp_internal(upper1, upper2);
}

/*
 * Replace negative (or NaN) value with zero.
 */
static inline float
non_negative(float val)
{
	if (val >= 0.0f)
		return val;
	else
		return 0.0f;
}

/*
 * Consider replacement of currently selected split with the better one.
 */
static inline void
g_box_consider_split(ConsiderSplitContext *context, int dimNum,
					 float8 rightLower, int minLeftCount,
					 float8 leftUpper, int maxLeftCount)
{
	int			leftCount,
				rightCount;
	float4		ratio,
				overlap;
	float8		range;

	/*
	 * Calculate entries distribution ratio assuming most uniform distribution
	 * of common entries.
	 */
	if (minLeftCount >= (context->entriesCount + 1) / 2)
	{
		leftCount = minLeftCount;
	}
	else
	{
		if (maxLeftCount <= context->entriesCount / 2)
			leftCount = maxLeftCount;
		else
			leftCount = context->entriesCount / 2;
	}
	rightCount = context->entriesCount - leftCount;

	/*
	 * Ratio of split - quotient between size of lesser group and total
	 * entries count.
	 */
	ratio = float4_div(Min(leftCount, rightCount), context->entriesCount);

	if (ratio > LIMIT_RATIO)
	{
		bool		selectthis = false;

		/*
		 * The ratio is acceptable, so compare current split with previously
		 * selected one. Between splits of one dimension we search for minimal
		 * overlap (allowing negative values) and minimal ration (between same
		 * overlaps. We switch dimension if find less overlap (non-negative)
		 * or less range with same overlap.
		 */
		if (dimNum == 0)
			range = float8_mi(context->boundingBox.high.x,
							  context->boundingBox.low.x);
		else
			range = float8_mi(context->boundingBox.high.y,
							  context->boundingBox.low.y);

		overlap = float8_div(float8_mi(leftUpper, rightLower), range);

		/* If there is no previous selection, select this */
		if (context->first)
			selectthis = true;
		else if (context->dim == dimNum)
		{
			/*
			 * Within the same dimension, choose the new split if it has a
			 * smaller overlap, or same overlap but better ratio.
			 */
			if (overlap < context->overlap ||
				(overlap == context->overlap && ratio > context->ratio))
				selectthis = true;
		}
		else
		{
			/*
			 * Across dimensions, choose the new split if it has a smaller
			 * *non-negative* overlap, or same *non-negative* overlap but
			 * bigger range. This condition differs from the one described in
			 * the article. On the datasets where leaf MBRs don't overlap
			 * themselves, non-overlapping splits (i.e. splits which have zero
			 * *non-negative* overlap) are frequently possible. In this case
			 * splits tends to be along one dimension, because most distant
			 * non-overlapping splits (i.e. having lowest negative overlap)
			 * appears to be in the same dimension as in the previous split.
			 * Therefore MBRs appear to be very prolonged along another
			 * dimension, which leads to bad search performance. Using range
			 * as the second split criteria makes MBRs more quadratic. Using
			 * *non-negative* overlap instead of overlap as the first split
			 * criteria gives to range criteria a chance to matter, because
			 * non-overlapping splits are equivalent in this criteria.
			 */
			if (non_negative(overlap) < non_negative(context->overlap) ||
				(range > context->range &&
				 non_negative(overlap) <= non_negative(context->overlap)))
				selectthis = true;
		}

		if (selectthis)
		{
			/* save information about selected split */
			context->first = false;
			context->ratio = ratio;
			context->range = range;
			context->overlap = overlap;
			context->rightLower = rightLower;
			context->leftUpper = leftUpper;
			context->dim = dimNum;
		}
	}
}

/*
 * Compare common entries by their deltas.
 */
static int
common_entry_cmp(const void *i1, const void *i2)
{
	float8		delta1 = ((const CommonEntry *) i1)->delta,
				delta2 = ((const CommonEntry *) i2)->delta;

	return float8_cmp_internal(delta1, delta2);
}

/*
 * --------------------------------------------------------------------------
 * Double sorting split algorithm. This is used for both boxes and points.
 *
 * The algorithm finds split of boxes by considering splits along each axis.
 * Each entry is first projected as an interval on the X-axis, and different
 * ways to split the intervals into two groups are considered, trying to
 * minimize the overlap of the groups. Then the same is repeated for the
 * Y-axis, and the overall best split is chosen. The quality of a split is
 * determined by overlap along that axis and some other criteria (see
 * g_box_consider_split).
 *
 * After that, all the entries are divided into three groups:
 *
 * 1) Entries which should be placed to the left group
 * 2) Entries which should be placed to the right group
 * 3) "Common entries" which can be placed to any of groups without affecting
 *	  of overlap along selected axis.
 *
 * The common entries are distributed by minimizing penalty.
 *
 * For details see:
 * "A new double sorting-based node splitting algorithm for R-tree", A. Korotkov
 * http://syrcose.ispras.ru/2011/files/SYRCoSE2011_Proceedings.pdf#page=36
 * --------------------------------------------------------------------------
 */
Datum
gist_box_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	OffsetNumber i,
				maxoff;
	ConsiderSplitContext context;
	BOX		   *box,
			   *leftBox,
			   *rightBox;
	int			dim,
				commonEntriesCount;
	SplitInterval *intervalsLower,
			   *intervalsUpper;
	CommonEntry *commonEntries;
	int			nentries;

	memset(&context, 0, sizeof(ConsiderSplitContext));

	maxoff = entryvec->n - 1;
	nentries = context.entriesCount = maxoff - FirstOffsetNumber + 1;

	/* Allocate arrays for intervals along axes */
	intervalsLower = (SplitInterval *) palloc(nentries * sizeof(SplitInterval));
	intervalsUpper = (SplitInterval *) palloc(nentries * sizeof(SplitInterval));

	/*
	 * Calculate the overall minimum bounding box over all the entries.
	 */
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		box = DatumGetBoxP(entryvec->vector[i].key);
		if (i == FirstOffsetNumber)
			context.boundingBox = *box;
		else
			adjustBox(&context.boundingBox, box);
	}

	/*
	 * Iterate over axes for optimal split searching.
	 */
	context.first = true;		/* nothing selected yet */
	for (dim = 0; dim < 2; dim++)
	{
		float8		leftUpper,
					rightLower;
		int			i1,
					i2;

		/* Project each entry as an interval on the selected axis. */
		for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
		{
			box = DatumGetBoxP(entryvec->vector[i].key);
			if (dim == 0)
			{
				intervalsLower[i - FirstOffsetNumber].lower = box->low.x;
				intervalsLower[i - FirstOffsetNumber].upper = box->high.x;
			}
			else
			{
				intervalsLower[i - FirstOffsetNumber].lower = box->low.y;
				intervalsLower[i - FirstOffsetNumber].upper = box->high.y;
			}
		}

		/*
		 * Make two arrays of intervals: one sorted by lower bound and another
		 * sorted by upper bound.
		 */
		memcpy(intervalsUpper, intervalsLower,
			   sizeof(SplitInterval) * nentries);
		qsort(intervalsLower, nentries, sizeof(SplitInterval),
			  interval_cmp_lower);
		qsort(intervalsUpper, nentries, sizeof(SplitInterval),
			  interval_cmp_upper);

		/*----
		 * The goal is to form a left and right interval, so that every entry
		 * interval is contained by either left or right interval (or both).
		 *
		 * For example, with the intervals (0,1), (1,3), (2,3), (2,4):
		 *
		 * 0 1 2 3 4
		 * +-+
		 *	 +---+
		 *	   +-+
		 *	   +---+
		 *
		 * The left and right intervals are of the form (0,a) and (b,4).
		 * We first consider splits where b is the lower bound of an entry.
		 * We iterate through all entries, and for each b, calculate the
		 * smallest possible a. Then we consider splits where a is the
		 * upper bound of an entry, and for each a, calculate the greatest
		 * possible b.
		 *
		 * In the above example, the first loop would consider splits:
		 * b=0: (0,1)-(0,4)
		 * b=1: (0,1)-(1,4)
		 * b=2: (0,3)-(2,4)
		 *
		 * And the second loop:
		 * a=1: (0,1)-(1,4)
		 * a=3: (0,3)-(2,4)
		 * a=4: (0,4)-(2,4)
		 */

		/*
		 * Iterate over lower bound of right group, finding smallest possible
		 * upper bound of left group.
		 */
		i1 = 0;
		i2 = 0;
		rightLower = intervalsLower[i1].lower;
		leftUpper = intervalsUpper[i2].lower;
		while (true)
		{
			/*
			 * Find next lower bound of right group.
			 */
			while (i1 < nentries &&
				   float8_eq(rightLower, intervalsLower[i1].lower))
			{
				if (float8_lt(leftUpper, intervalsLower[i1].upper))
					leftUpper = intervalsLower[i1].upper;
				i1++;
			}
			if (i1 >= nentries)
				break;
			rightLower = intervalsLower[i1].lower;

			/*
			 * Find count of intervals which anyway should be placed to the
			 * left group.
			 */
			while (i2 < nentries &&
				   float8_le(intervalsUpper[i2].upper, leftUpper))
				i2++;

			/*
			 * Consider found split.
			 */
			g_box_consider_split(&context, dim, rightLower, i1, leftUpper, i2);
		}

		/*
		 * Iterate over upper bound of left group finding greatest possible
		 * lower bound of right group.
		 */
		i1 = nentries - 1;
		i2 = nentries - 1;
		rightLower = intervalsLower[i1].upper;
		leftUpper = intervalsUpper[i2].upper;
		while (true)
		{
			/*
			 * Find next upper bound of left group.
			 */
			while (i2 >= 0 && float8_eq(leftUpper, intervalsUpper[i2].upper))
			{
				if (float8_gt(rightLower, intervalsUpper[i2].lower))
					rightLower = intervalsUpper[i2].lower;
				i2--;
			}
			if (i2 < 0)
				break;
			leftUpper = intervalsUpper[i2].upper;

			/*
			 * Find count of intervals which anyway should be placed to the
			 * right group.
			 */
			while (i1 >= 0 && float8_ge(intervalsLower[i1].lower, rightLower))
				i1--;

			/*
			 * Consider found split.
			 */
			g_box_consider_split(&context, dim,
								 rightLower, i1 + 1, leftUpper, i2 + 1);
		}
	}

	/*
	 * If we failed to find any acceptable splits, use trivial split.
	 */
	if (context.first)
	{
		fallbackSplit(entryvec, v);
		PG_RETURN_POINTER(v);
	}

	/*
	 * Ok, we have now selected the split across one axis.
	 *
	 * While considering the splits, we already determined that there will be
	 * enough entries in both groups to reach the desired ratio, but we did
	 * not memorize which entries go to which group. So determine that now.
	 */

	/* Allocate vectors for results */
	v->spl_left = (OffsetNumber *) palloc(nentries * sizeof(OffsetNumber));
	v->spl_right = (OffsetNumber *) palloc(nentries * sizeof(OffsetNumber));
	v->spl_nleft = 0;
	v->spl_nright = 0;

	/* Allocate bounding boxes of left and right groups */
	leftBox = palloc0(sizeof(BOX));
	rightBox = palloc0(sizeof(BOX));

	/*
	 * Allocate an array for "common entries" - entries which can be placed to
	 * either group without affecting overlap along selected axis.
	 */
	commonEntriesCount = 0;
	commonEntries = (CommonEntry *) palloc(nentries * sizeof(CommonEntry));

	/* Helper macros to place an entry in the left or right group */
#define PLACE_LEFT(box, off)					\
	do {										\
		if (v->spl_nleft > 0)					\
			adjustBox(leftBox, box);			\
		else									\
			*leftBox = *(box);					\
		v->spl_left[v->spl_nleft++] = off;		\
	} while(0)

#define PLACE_RIGHT(box, off)					\
	do {										\
		if (v->spl_nright > 0)					\
			adjustBox(rightBox, box);			\
		else									\
			*rightBox = *(box);					\
		v->spl_right[v->spl_nright++] = off;	\
	} while(0)

	/*
	 * Distribute entries which can be distributed unambiguously, and collect
	 * common entries.
	 */
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		float8		lower,
					upper;

		/*
		 * Get upper and lower bounds along selected axis.
		 */
		box = DatumGetBoxP(entryvec->vector[i].key);
		if (context.dim == 0)
		{
			lower = box->low.x;
			upper = box->high.x;
		}
		else
		{
			lower = box->low.y;
			upper = box->high.y;
		}

		if (float8_le(upper, context.leftUpper))
		{
			/* Fits to the left group */
			if (float8_ge(lower, context.rightLower))
			{
				/* Fits also to the right group, so "common entry" */
				commonEntries[commonEntriesCount++].index = i;
			}
			else
			{
				/* Doesn't fit to the right group, so join to the left group */
				PLACE_LEFT(box, i);
			}
		}
		else
		{
			/*
			 * Each entry should fit on either left or right group. Since this
			 * entry didn't fit on the left group, it better fit in the right
			 * group.
			 */
			Assert(float8_ge(lower, context.rightLower));

			/* Doesn't fit to the left group, so join to the right group */
			PLACE_RIGHT(box, i);
		}
	}

	/*
	 * Distribute "common entries", if any.
	 */
	if (commonEntriesCount > 0)
	{
		/*
		 * Calculate minimum number of entries that must be placed in both
		 * groups, to reach LIMIT_RATIO.
		 */
		int			m = ceil(LIMIT_RATIO * nentries);

		/*
		 * Calculate delta between penalties of join "common entries" to
		 * different groups.
		 */
		for (i = 0; i < commonEntriesCount; i++)
		{
			box = DatumGetBoxP(entryvec->vector[commonEntries[i].index].key);
			commonEntries[i].delta = fabs(float8_mi(box_penalty(leftBox, box),
													box_penalty(rightBox, box)));
		}

		/*
		 * Sort "common entries" by calculated deltas in order to distribute
		 * the most ambiguous entries first.
		 */
		qsort(commonEntries, commonEntriesCount, sizeof(CommonEntry), common_entry_cmp);

		/*
		 * Distribute "common entries" between groups.
		 */
		for (i = 0; i < commonEntriesCount; i++)
		{
			box = DatumGetBoxP(entryvec->vector[commonEntries[i].index].key);

			/*
			 * Check if we have to place this entry in either group to achieve
			 * LIMIT_RATIO.
			 */
			if (v->spl_nleft + (commonEntriesCount - i) <= m)
				PLACE_LEFT(box, commonEntries[i].index);
			else if (v->spl_nright + (commonEntriesCount - i) <= m)
				PLACE_RIGHT(box, commonEntries[i].index);
			else
			{
				/* Otherwise select the group by minimal penalty */
				if (box_penalty(leftBox, box) < box_penalty(rightBox, box))
					PLACE_LEFT(box, commonEntries[i].index);
				else
					PLACE_RIGHT(box, commonEntries[i].index);
			}
		}
	}

	v->spl_ldatum = PointerGetDatum(leftBox);
	v->spl_rdatum = PointerGetDatum(rightBox);
	PG_RETURN_POINTER(v);
}

/*
 * Equality method
 *
 * This is used for boxes, points, circles, and polygons, all of which store
 * boxes as GiST index entries.
 *
 * Returns true only when boxes are exactly the same.  We can't use fuzzy
 * comparisons here without breaking index consistency; therefore, this isn't
 * equivalent to box_same().
 */
Datum
gist_box_same(PG_FUNCTION_ARGS)
{
	BOX		   *b1 = PG_GETARG_BOX_P(0);
	BOX		   *b2 = PG_GETARG_BOX_P(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	if (b1 && b2)
		*result = (float8_eq(b1->low.x, b2->low.x) &&
				   float8_eq(b1->low.y, b2->low.y) &&
				   float8_eq(b1->high.x, b2->high.x) &&
				   float8_eq(b1->high.y, b2->high.y));
	else
		*result = (b1 == NULL && b2 == NULL);
	PG_RETURN_POINTER(result);
}

/*
 * Leaf-level consistency for boxes: just apply the query operator
 */
static bool
gist_box_leaf_consistent(BOX *key, BOX *query, StrategyNumber strategy)
{
	bool		retval;

	switch (strategy)
	{
		case RTLeftStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_left,
													  PointerGetDatum(key),
													  PointerGetDatum(query)));
			break;
		case RTOverLeftStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_overleft,
													  PointerGetDatum(key),
													  PointerGetDatum(query)));
			break;
		case RTOverlapStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_overlap,
													  PointerGetDatum(key),
													  PointerGetDatum(query)));
			break;
		case RTOverRightStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_overright,
													  PointerGetDatum(key),
													  PointerGetDatum(query)));
			break;
		case RTRightStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_right,
													  PointerGetDatum(key),
													  PointerGetDatum(query)));
			break;
		case RTSameStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_same,
													  PointerGetDatum(key),
													  PointerGetDatum(query)));
			break;
		case RTContainsStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_contain,
													  PointerGetDatum(key),
													  PointerGetDatum(query)));
			break;
		case RTContainedByStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_contained,
													  PointerGetDatum(key),
													  PointerGetDatum(query)));
			break;
		case RTOverBelowStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_overbelow,
													  PointerGetDatum(key),
													  PointerGetDatum(query)));
			break;
		case RTBelowStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_below,
													  PointerGetDatum(key),
													  PointerGetDatum(query)));
			break;
		case RTAboveStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_above,
													  PointerGetDatum(key),
													  PointerGetDatum(query)));
			break;
		case RTOverAboveStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_overabove,
													  PointerGetDatum(key),
													  PointerGetDatum(query)));
			break;
		default:
			elog(ERROR, "unrecognized strategy number: %d", strategy);
			retval = false;		/* keep compiler quiet */
			break;
	}
	return retval;
}

/*****************************************
 * Common rtree functions (for boxes, polygons, and circles)
 *****************************************/

/*
 * Internal-page consistency for all these types
 *
 * We can use the same function since all types use bounding boxes as the
 * internal-page representation.
 */
static bool
rtree_internal_consistent(BOX *key, BOX *query, StrategyNumber strategy)
{
	bool		retval;

	switch (strategy)
	{
		case RTLeftStrategyNumber:
			retval = !DatumGetBool(DirectFunctionCall2(box_overright,
													   PointerGetDatum(key),
													   PointerGetDatum(query)));
			break;
		case RTOverLeftStrategyNumber:
			retval = !DatumGetBool(DirectFunctionCall2(box_right,
													   PointerGetDatum(key),
													   PointerGetDatum(query)));
			break;
		case RTOverlapStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_overlap,
													  PointerGetDatum(key),
													  PointerGetDatum(query)));
			break;
		case RTOverRightStrategyNumber:
			retval = !DatumGetBool(DirectFunctionCall2(box_left,
													   PointerGetDatum(key),
													   PointerGetDatum(query)));
			break;
		case RTRightStrategyNumber:
			retval = !DatumGetBool(DirectFunctionCall2(box_overleft,
													   PointerGetDatum(key),
													   PointerGetDatum(query)));
			break;
		case RTSameStrategyNumber:
		case RTContainsStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_contain,
													  PointerGetDatum(key),
													  PointerGetDatum(query)));
			break;
		case RTContainedByStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_overlap,
													  PointerGetDatum(key),
													  PointerGetDatum(query)));
			break;
		case RTOverBelowStrategyNumber:
			retval = !DatumGetBool(DirectFunctionCall2(box_above,
													   PointerGetDatum(key),
													   PointerGetDatum(query)));
			break;
		case RTBelowStrategyNumber:
			retval = !DatumGetBool(DirectFunctionCall2(box_overabove,
													   PointerGetDatum(key),
													   PointerGetDatum(query)));
			break;
		case RTAboveStrategyNumber:
			retval = !DatumGetBool(DirectFunctionCall2(box_overbelow,
													   PointerGetDatum(key),
													   PointerGetDatum(query)));
			break;
		case RTOverAboveStrategyNumber:
			retval = !DatumGetBool(DirectFunctionCall2(box_below,
													   PointerGetDatum(key),
													   PointerGetDatum(query)));
			break;
		default:
			elog(ERROR, "unrecognized strategy number: %d", strategy);
			retval = false;		/* keep compiler quiet */
			break;
	}
	return retval;
}

/**************************************************
 * Polygon ops
 **************************************************/

/*
 * GiST compress for polygons: represent a polygon by its bounding box
 */
Datum
gist_poly_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval;

	if (entry->leafkey)
	{
		POLYGON    *in = DatumGetPolygonP(entry->key);
		BOX		   *r;

		r = (BOX *) palloc(sizeof(BOX));
		memcpy(r, &(in->boundbox), sizeof(BOX));

		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(r),
					  entry->rel, entry->page,
					  entry->offset, false);
	}
	else
		retval = entry;
	PG_RETURN_POINTER(retval);
}

/*
 * The GiST Consistent method for polygons
 */
Datum
gist_poly_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	POLYGON    *query = PG_GETARG_POLYGON_P(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	bool		result;

	/* All cases served by this function are inexact */
	*recheck = true;

	if (DatumGetBoxP(entry->key) == NULL || query == NULL)
		PG_RETURN_BOOL(false);

	/*
	 * Since the operators require recheck anyway, we can just use
	 * rtree_internal_consistent even at leaf nodes.  (This works in part
	 * because the index entries are bounding boxes not polygons.)
	 */
	result = rtree_internal_consistent(DatumGetBoxP(entry->key),
									   &(query->boundbox), strategy);

	/* Avoid memory leak if supplied poly is toasted */
	PG_FREE_IF_COPY(query, 1);

	PG_RETURN_BOOL(result);
}

/**************************************************
 * Circle ops
 **************************************************/

/*
 * GiST compress for circles: represent a circle by its bounding box
 */
Datum
gist_circle_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval;

	if (entry->leafkey)
	{
		CIRCLE	   *in = DatumGetCircleP(entry->key);
		BOX		   *r;

		r = (BOX *) palloc(sizeof(BOX));
		r->high.x = float8_pl(in->center.x, in->radius);
		r->low.x = float8_mi(in->center.x, in->radius);
		r->high.y = float8_pl(in->center.y, in->radius);
		r->low.y = float8_mi(in->center.y, in->radius);

		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(r),
					  entry->rel, entry->page,
					  entry->offset, false);
	}
	else
		retval = entry;
	PG_RETURN_POINTER(retval);
}

/*
 * The GiST Consistent method for circles
 */
Datum
gist_circle_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	CIRCLE	   *query = PG_GETARG_CIRCLE_P(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	BOX			bbox;
	bool		result;

	/* All cases served by this function are inexact */
	*recheck = true;

	if (DatumGetBoxP(entry->key) == NULL || query == NULL)
		PG_RETURN_BOOL(false);

	/*
	 * Since the operators require recheck anyway, we can just use
	 * rtree_internal_consistent even at leaf nodes.  (This works in part
	 * because the index entries are bounding boxes not circles.)
	 */
	bbox.high.x = float8_pl(query->center.x, query->radius);
	bbox.low.x = float8_mi(query->center.x, query->radius);
	bbox.high.y = float8_pl(query->center.y, query->radius);
	bbox.low.y = float8_mi(query->center.y, query->radius);

	result = rtree_internal_consistent(DatumGetBoxP(entry->key),
									   &bbox, strategy);

	PG_RETURN_BOOL(result);
}

/**************************************************
 * Point ops
 **************************************************/

Datum
gist_point_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);

	if (entry->leafkey)			/* Point, actually */
	{
		BOX		   *box = palloc(sizeof(BOX));
		Point	   *point = DatumGetPointP(entry->key);
		GISTENTRY  *retval = palloc(sizeof(GISTENTRY));

		box->high = box->low = *point;

		gistentryinit(*retval, BoxPGetDatum(box),
					  entry->rel, entry->page, entry->offset, false);

		PG_RETURN_POINTER(retval);
	}

	PG_RETURN_POINTER(entry);
}

/*
 * GiST Fetch method for point
 *
 * Get point coordinates from its bounding box coordinates and form new
 * gistentry.
 */
Datum
gist_point_fetch(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	BOX		   *in = DatumGetBoxP(entry->key);
	Point	   *r;
	GISTENTRY  *retval;

	retval = palloc(sizeof(GISTENTRY));

	r = (Point *) palloc(sizeof(Point));
	r->x = in->high.x;
	r->y = in->high.y;
	gistentryinit(*retval, PointerGetDatum(r),
				  entry->rel, entry->page,
				  entry->offset, false);

	PG_RETURN_POINTER(retval);
}


#define point_point_distance(p1,p2) \
	DatumGetFloat8(DirectFunctionCall2(point_distance, \
									   PointPGetDatum(p1), PointPGetDatum(p2)))

static float8
computeDistance(bool isLeaf, BOX *box, Point *point)
{
	float8		result = 0.0;

	if (isLeaf)
	{
		/* simple point to point distance */
		result = point_point_distance(point, &box->low);
	}
	else if (point->x <= box->high.x && point->x >= box->low.x &&
			 point->y <= box->high.y && point->y >= box->low.y)
	{
		/* point inside the box */
		result = 0.0;
	}
	else if (point->x <= box->high.x && point->x >= box->low.x)
	{
		/* point is over or below box */
		Assert(box->low.y <= box->high.y);
		if (point->y > box->high.y)
			result = float8_mi(point->y, box->high.y);
		else if (point->y < box->low.y)
			result = float8_mi(box->low.y, point->y);
		else
			elog(ERROR, "inconsistent point values");
	}
	else if (point->y <= box->high.y && point->y >= box->low.y)
	{
		/* point is to left or right of box */
		Assert(box->low.x <= box->high.x);
		if (point->x > box->high.x)
			result = float8_mi(point->x, box->high.x);
		else if (point->x < box->low.x)
			result = float8_mi(box->low.x, point->x);
		else
			elog(ERROR, "inconsistent point values");
	}
	else
	{
		/* closest point will be a vertex */
		Point		p;
		float8		subresult;

		result = point_point_distance(point, &box->low);

		subresult = point_point_distance(point, &box->high);
		if (result > subresult)
			result = subresult;

		p.x = box->low.x;
		p.y = box->high.y;
		subresult = point_point_distance(point, &p);
		if (result > subresult)
			result = subresult;

		p.x = box->high.x;
		p.y = box->low.y;
		subresult = point_point_distance(point, &p);
		if (result > subresult)
			result = subresult;
	}

	return result;
}

static bool
gist_point_consistent_internal(StrategyNumber strategy,
							   bool isLeaf, BOX *key, Point *query)
{
	bool		result = false;

	switch (strategy)
	{
		case RTLeftStrategyNumber:
			result = FPlt(key->low.x, query->x);
			break;
		case RTRightStrategyNumber:
			result = FPgt(key->high.x, query->x);
			break;
		case RTAboveStrategyNumber:
			result = FPgt(key->high.y, query->y);
			break;
		case RTBelowStrategyNumber:
			result = FPlt(key->low.y, query->y);
			break;
		case RTSameStrategyNumber:
			if (isLeaf)
			{
				/* key.high must equal key.low, so we can disregard it */
				result = (FPeq(key->low.x, query->x) &&
						  FPeq(key->low.y, query->y));
			}
			else
			{
				result = (FPle(query->x, key->high.x) &&
						  FPge(query->x, key->low.x) &&
						  FPle(query->y, key->high.y) &&
						  FPge(query->y, key->low.y));
			}
			break;
		default:
			elog(ERROR, "unrecognized strategy number: %d", strategy);
			result = false;		/* keep compiler quiet */
			break;
	}

	return result;
}

#define GeoStrategyNumberOffset		20
#define PointStrategyNumberGroup	0
#define BoxStrategyNumberGroup		1
#define PolygonStrategyNumberGroup	2
#define CircleStrategyNumberGroup	3

Datum
gist_point_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	bool		result;
	StrategyNumber strategyGroup;

	/*
	 * We have to remap these strategy numbers to get this klugy
	 * classification logic to work.
	 */
	if (strategy == RTOldBelowStrategyNumber)
		strategy = RTBelowStrategyNumber;
	else if (strategy == RTOldAboveStrategyNumber)
		strategy = RTAboveStrategyNumber;

	strategyGroup = strategy / GeoStrategyNumberOffset;
	switch (strategyGroup)
	{
		case PointStrategyNumberGroup:
			result = gist_point_consistent_internal(strategy % GeoStrategyNumberOffset,
													GIST_LEAF(entry),
													DatumGetBoxP(entry->key),
													PG_GETARG_POINT_P(1));
			*recheck = false;
			break;
		case BoxStrategyNumberGroup:
			{
				/*
				 * The only operator in this group is point <@ box (on_pb), so
				 * we needn't examine strategy again.
				 *
				 * For historical reasons, on_pb uses exact rather than fuzzy
				 * comparisons.  We could use box_overlap when at an internal
				 * page, but that would lead to possibly visiting child pages
				 * uselessly, because box_overlap uses fuzzy comparisons.
				 * Instead we write a non-fuzzy overlap test.  The same code
				 * will also serve for leaf-page tests, since leaf keys have
				 * high == low.
				 */
				BOX		   *query,
						   *key;

				query = PG_GETARG_BOX_P(1);
				key = DatumGetBoxP(entry->key);

				result = (key->high.x >= query->low.x &&
						  key->low.x <= query->high.x &&
						  key->high.y >= query->low.y &&
						  key->low.y <= query->high.y);
				*recheck = false;
			}
			break;
		case PolygonStrategyNumberGroup:
			{
				POLYGON    *query = PG_GETARG_POLYGON_P(1);

				result = DatumGetBool(DirectFunctionCall5(gist_poly_consistent,
														  PointerGetDatum(entry),
														  PolygonPGetDatum(query),
														  Int16GetDatum(RTOverlapStrategyNumber),
														  0, PointerGetDatum(recheck)));

				if (GIST_LEAF(entry) && result)
				{
					/*
					 * We are on leaf page and quick check shows overlapping
					 * of polygon's bounding box and point
					 */
					BOX		   *box = DatumGetBoxP(entry->key);

					Assert(box->high.x == box->low.x
						   && box->high.y == box->low.y);
					result = DatumGetBool(DirectFunctionCall2(poly_contain_pt,
															  PolygonPGetDatum(query),
															  PointPGetDatum(&box->high)));
					*recheck = false;
				}
			}
			break;
		case CircleStrategyNumberGroup:
			{
				CIRCLE	   *query = PG_GETARG_CIRCLE_P(1);

				result = DatumGetBool(DirectFunctionCall5(gist_circle_consistent,
														  PointerGetDatum(entry),
														  CirclePGetDatum(query),
														  Int16GetDatum(RTOverlapStrategyNumber),
														  0, PointerGetDatum(recheck)));

				if (GIST_LEAF(entry) && result)
				{
					/*
					 * We are on leaf page and quick check shows overlapping
					 * of polygon's bounding box and point
					 */
					BOX		   *box = DatumGetBoxP(entry->key);

					Assert(box->high.x == box->low.x
						   && box->high.y == box->low.y);
					result = DatumGetBool(DirectFunctionCall2(circle_contain_pt,
															  CirclePGetDatum(query),
															  PointPGetDatum(&box->high)));
					*recheck = false;
				}
			}
			break;
		default:
			elog(ERROR, "unrecognized strategy number: %d", strategy);
			result = false;		/* keep compiler quiet */
			break;
	}

	PG_RETURN_BOOL(result);
}

Datum
gist_point_distance(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	float8		distance;
	StrategyNumber strategyGroup = strategy / GeoStrategyNumberOffset;

	switch (strategyGroup)
	{
		case PointStrategyNumberGroup:
			distance = computeDistance(GIST_LEAF(entry),
									   DatumGetBoxP(entry->key),
									   PG_GETARG_POINT_P(1));
			break;
		default:
			elog(ERROR, "unrecognized strategy number: %d", strategy);
			distance = 0.0;		/* keep compiler quiet */
			break;
	}

	PG_RETURN_FLOAT8(distance);
}

static float8
gist_bbox_distance(GISTENTRY *entry, Datum query, StrategyNumber strategy)
{
	float8		distance;
	StrategyNumber strategyGroup = strategy / GeoStrategyNumberOffset;

	switch (strategyGroup)
	{
		case PointStrategyNumberGroup:
			distance = computeDistance(false,
									   DatumGetBoxP(entry->key),
									   DatumGetPointP(query));
			break;
		default:
			elog(ERROR, "unrecognized strategy number: %d", strategy);
			distance = 0.0;		/* keep compiler quiet */
	}

	return distance;
}

Datum
gist_box_distance(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	Datum		query = PG_GETARG_DATUM(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid subtype = PG_GETARG_OID(3); */
	/* bool	   *recheck = (bool *) PG_GETARG_POINTER(4); */
	float8		distance;

	distance = gist_bbox_distance(entry, query, strategy);

	PG_RETURN_FLOAT8(distance);
}

/*
 * The inexact GiST distance methods for geometric types that store bounding
 * boxes.
 *
 * Compute lossy distance from point to index entries.  The result is inexact
 * because index entries are bounding boxes, not the exact shapes of the
 * indexed geometric types.  We use distance from point to MBR of index entry.
 * This is a lower bound estimate of distance from point to indexed geometric
 * type.
 */
Datum
gist_circle_distance(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	Datum		query = PG_GETARG_DATUM(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	float8		distance;

	distance = gist_bbox_distance(entry, query, strategy);
	*recheck = true;

	PG_RETURN_FLOAT8(distance);
}

Datum
gist_poly_distance(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	Datum		query = PG_GETARG_DATUM(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	float8		distance;

	distance = gist_bbox_distance(entry, query, strategy);
	*recheck = true;

	PG_RETURN_FLOAT8(distance);
}

/*
 * Z-order routines for fast index build
 */

/*
 * Compute Z-value of a point
 *
 * Z-order (also known as Morton Code) maps a two-dimensional point to a
 * single integer, in a way that preserves locality. Points that are close in
 * the two-dimensional space are mapped to integer that are not far from each
 * other. We do that by interleaving the bits in the X and Y components.
 *
 * Morton Code is normally defined only for integers, but the X and Y values
 * of a point are floating point. We expect floats to be in IEEE format.
 */
static uint64
point_zorder_internal(float4 x, float4 y)
{
	uint32		ix = ieee_float32_to_uint32(x);
	uint32		iy = ieee_float32_to_uint32(y);

	/* Interleave the bits */
	return part_bits32_by2(ix) | (part_bits32_by2(iy) << 1);
}

/* Interleave 32 bits with zeroes */
static uint64
part_bits32_by2(uint32 x)
{
	uint64		n = x;

	n = (n | (n << 16)) & UINT64CONST(0x0000FFFF0000FFFF);
	n = (n | (n << 8)) & UINT64CONST(0x00FF00FF00FF00FF);
	n = (n | (n << 4)) & UINT64CONST(0x0F0F0F0F0F0F0F0F);
	n = (n | (n << 2)) & UINT64CONST(0x3333333333333333);
	n = (n | (n << 1)) & UINT64CONST(0x5555555555555555);

	return n;
}

/*
 * Convert a 32-bit IEEE float to uint32 in a way that preserves the ordering
 */
static uint32
ieee_float32_to_uint32(float f)
{
	/*----
	 *
	 * IEEE 754 floating point format
	 * ------------------------------
	 *
	 * IEEE 754 floating point numbers have this format:
	 *
	 *   exponent (8 bits)
	 *   |
	 * s eeeeeeee mmmmmmmmmmmmmmmmmmmmmmm
	 * |          |
	 * sign       mantissa (23 bits)
	 *
	 * Infinity has all bits in the exponent set and the mantissa is all
	 * zeros. Negative infinity is the same but with the sign bit set.
	 *
	 * NaNs are represented with all bits in the exponent set, and the least
	 * significant bit in the mantissa also set. The rest of the mantissa bits
	 * can be used to distinguish different kinds of NaNs.
	 *
	 * The IEEE format has the nice property that when you take the bit
	 * representation and interpret it as an integer, the order is preserved,
	 * except for the sign. That holds for the +-Infinity values too.
	 *
	 * Mapping to uint32
	 * -----------------
	 *
	 * In order to have a smooth transition from negative to positive numbers,
	 * we map floats to unsigned integers like this:
	 *
	 * x < 0 to range 0-7FFFFFFF
	 * x = 0 to value 8000000 (both positive and negative zero)
	 * x > 0 to range 8000001-FFFFFFFF
	 *
	 * We don't care to distinguish different kind of NaNs, so they are all
	 * mapped to the same arbitrary value, FFFFFFFF. Because of the IEEE bit
	 * representation of NaNs, there aren't any non-NaN values that would be
	 * mapped to FFFFFFFF. In fact, there is a range of unused values on both
	 * ends of the uint32 space.
	 */
	if (isnan(f))
		return 0xFFFFFFFF;
	else
	{
		union
		{
			float		f;
			uint32		i;
		}			u;

		u.f = f;

		/* Check the sign bit */
		if ((u.i & 0x80000000) != 0)
		{
			/*
			 * Map the negative value to range 0-7FFFFFFF. This flips the sign
			 * bit to 0 in the same instruction.
			 */
			Assert(f <= 0);		/* can be -0 */
			u.i ^= 0xFFFFFFFF;
		}
		else
		{
			/* Map the positive value (or 0) to range 80000000-FFFFFFFF */
			u.i |= 0x80000000;
		}

		return u.i;
	}
}

/*
 * Compare the Z-order of points
 */
static int
gist_bbox_zorder_cmp(Datum a, Datum b, SortSupport ssup)
{
	Point	   *p1 = &(DatumGetBoxP(a)->low);
	Point	   *p2 = &(DatumGetBoxP(b)->low);
	uint64		z1;
	uint64		z2;

	/*
	 * Do a quick check for equality first. It's not clear if this is worth it
	 * in general, but certainly is when used as tie-breaker with abbreviated
	 * keys,
	 */
	if (p1->x == p2->x && p1->y == p2->y)
		return 0;

	z1 = point_zorder_internal(p1->x, p1->y);
	z2 = point_zorder_internal(p2->x, p2->y);
	if (z1 > z2)
		return 1;
	else if (z1 < z2)
		return -1;
	else
		return 0;
}

/*
 * Abbreviated version of Z-order comparison
 *
 * The abbreviated format is a Z-order value computed from the two 32-bit
 * floats. If SIZEOF_DATUM == 8, the 64-bit Z-order value fits fully in the
 * abbreviated Datum, otherwise use its most significant bits.
 */
static Datum
gist_bbox_zorder_abbrev_convert(Datum original, SortSupport ssup)
{
	Point	   *p = &(DatumGetBoxP(original)->low);
	uint64		z;

	z = point_zorder_internal(p->x, p->y);

#if SIZEOF_DATUM == 8
	return (Datum) z;
#else
	return (Datum) (z >> 32);
#endif
}

/*
 * We never consider aborting the abbreviation.
 *
 * On 64-bit systems, the abbreviation is not lossy so it is always
 * worthwhile. (Perhaps it's not on 32-bit systems, but we don't bother
 * with logic to decide.)
 */
static bool
gist_bbox_zorder_abbrev_abort(int memtupcount, SortSupport ssup)
{
	return false;
}

/*
 * Sort support routine for fast GiST index build by sorting.
 */
Datum
gist_point_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	if (ssup->abbreviate)
	{
		ssup->comparator = ssup_datum_unsigned_cmp;
		ssup->abbrev_converter = gist_bbox_zorder_abbrev_convert;
		ssup->abbrev_abort = gist_bbox_zorder_abbrev_abort;
		ssup->abbrev_full_comparator = gist_bbox_zorder_cmp;
	}
	else
	{
		ssup->comparator = gist_bbox_zorder_cmp;
	}
	PG_RETURN_VOID();
}
