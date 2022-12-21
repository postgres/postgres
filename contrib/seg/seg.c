/*
 * contrib/seg/seg.c
 *
 ******************************************************************************
  This file contains routines that can be bound to a Postgres backend and
  called by the backend in the process of processing queries.  The calling
  format for these routines is dictated by Postgres architecture.
******************************************************************************/

#include "postgres.h"

#include <float.h>

#include "access/gist.h"
#include "access/stratnum.h"
#include "fmgr.h"

#include "segdata.h"


#define DatumGetSegP(X) ((SEG *) DatumGetPointer(X))
#define PG_GETARG_SEG_P(n) DatumGetSegP(PG_GETARG_POINTER(n))


/*
#define GIST_DEBUG
#define GIST_QUERY_DEBUG
*/

PG_MODULE_MAGIC;

/*
 * Auxiliary data structure for picksplit method.
 */
typedef struct
{
	float		center;
	OffsetNumber index;
	SEG		   *data;
} gseg_picksplit_item;

/*
** Input/Output routines
*/
PG_FUNCTION_INFO_V1(seg_in);
PG_FUNCTION_INFO_V1(seg_out);
PG_FUNCTION_INFO_V1(seg_size);
PG_FUNCTION_INFO_V1(seg_lower);
PG_FUNCTION_INFO_V1(seg_upper);
PG_FUNCTION_INFO_V1(seg_center);

/*
** GiST support methods
*/
PG_FUNCTION_INFO_V1(gseg_consistent);
PG_FUNCTION_INFO_V1(gseg_compress);
PG_FUNCTION_INFO_V1(gseg_decompress);
PG_FUNCTION_INFO_V1(gseg_picksplit);
PG_FUNCTION_INFO_V1(gseg_penalty);
PG_FUNCTION_INFO_V1(gseg_union);
PG_FUNCTION_INFO_V1(gseg_same);
static Datum gseg_leaf_consistent(Datum key, Datum query, StrategyNumber strategy);
static Datum gseg_internal_consistent(Datum key, Datum query, StrategyNumber strategy);
static Datum gseg_binary_union(Datum r1, Datum r2, int *sizep);


/*
** R-tree support functions
*/
PG_FUNCTION_INFO_V1(seg_same);
PG_FUNCTION_INFO_V1(seg_contains);
PG_FUNCTION_INFO_V1(seg_contained);
PG_FUNCTION_INFO_V1(seg_overlap);
PG_FUNCTION_INFO_V1(seg_left);
PG_FUNCTION_INFO_V1(seg_over_left);
PG_FUNCTION_INFO_V1(seg_right);
PG_FUNCTION_INFO_V1(seg_over_right);
PG_FUNCTION_INFO_V1(seg_union);
PG_FUNCTION_INFO_V1(seg_inter);
static void rt_seg_size(SEG *a, float *size);

/*
** Various operators
*/
PG_FUNCTION_INFO_V1(seg_cmp);
PG_FUNCTION_INFO_V1(seg_lt);
PG_FUNCTION_INFO_V1(seg_le);
PG_FUNCTION_INFO_V1(seg_gt);
PG_FUNCTION_INFO_V1(seg_ge);
PG_FUNCTION_INFO_V1(seg_different);

/*
** Auxiliary functions
*/
static int	restore(char *s, float val, int n);


/*****************************************************************************
 * Input/Output functions
 *****************************************************************************/

Datum
seg_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	SEG		   *result = palloc(sizeof(SEG));

	seg_scanner_init(str);

	if (seg_yyparse(result) != 0)
		seg_yyerror(result, "bogus input");

	seg_scanner_finish();

	PG_RETURN_POINTER(result);
}

Datum
seg_out(PG_FUNCTION_ARGS)
{
	SEG		   *seg = PG_GETARG_SEG_P(0);
	char	   *result;
	char	   *p;

	p = result = (char *) palloc(40);

	if (seg->l_ext == '>' || seg->l_ext == '<' || seg->l_ext == '~')
		p += sprintf(p, "%c", seg->l_ext);

	if (seg->lower == seg->upper && seg->l_ext == seg->u_ext)
	{
		/*
		 * indicates that this interval was built by seg_in off a single point
		 */
		p += restore(p, seg->lower, seg->l_sigd);
	}
	else
	{
		if (seg->l_ext != '-')
		{
			/* print the lower boundary if exists */
			p += restore(p, seg->lower, seg->l_sigd);
			p += sprintf(p, " ");
		}
		p += sprintf(p, "..");
		if (seg->u_ext != '-')
		{
			/* print the upper boundary if exists */
			p += sprintf(p, " ");
			if (seg->u_ext == '>' || seg->u_ext == '<' || seg->l_ext == '~')
				p += sprintf(p, "%c", seg->u_ext);
			p += restore(p, seg->upper, seg->u_sigd);
		}
	}

	PG_RETURN_CSTRING(result);
}

Datum
seg_center(PG_FUNCTION_ARGS)
{
	SEG		   *seg = PG_GETARG_SEG_P(0);

	PG_RETURN_FLOAT4(((float) seg->lower + (float) seg->upper) / 2.0);
}

Datum
seg_lower(PG_FUNCTION_ARGS)
{
	SEG		   *seg = PG_GETARG_SEG_P(0);

	PG_RETURN_FLOAT4(seg->lower);
}

Datum
seg_upper(PG_FUNCTION_ARGS)
{
	SEG		   *seg = PG_GETARG_SEG_P(0);

	PG_RETURN_FLOAT4(seg->upper);
}


/*****************************************************************************
 *						   GiST functions
 *****************************************************************************/

/*
** The GiST Consistent method for segments
** Should return false if for all data items x below entry,
** the predicate x op query == false, where op is the oper
** corresponding to strategy in the pg_amop table.
*/
Datum
gseg_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	Datum		query = PG_GETARG_DATUM(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);

	/* All cases served by this function are exact */
	*recheck = false;

	/*
	 * if entry is not leaf, use gseg_internal_consistent, else use
	 * gseg_leaf_consistent
	 */
	if (GIST_LEAF(entry))
		return gseg_leaf_consistent(entry->key, query, strategy);
	else
		return gseg_internal_consistent(entry->key, query, strategy);
}

/*
** The GiST Union method for segments
** returns the minimal bounding seg that encloses all the entries in entryvec
*/
Datum
gseg_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int		   *sizep = (int *) PG_GETARG_POINTER(1);
	int			numranges,
				i;
	Datum		out = 0;
	Datum		tmp;

#ifdef GIST_DEBUG
	fprintf(stderr, "union\n");
#endif

	numranges = entryvec->n;
	tmp = entryvec->vector[0].key;
	*sizep = sizeof(SEG);

	for (i = 1; i < numranges; i++)
	{
		out = gseg_binary_union(tmp, entryvec->vector[i].key, sizep);
		tmp = out;
	}

	PG_RETURN_DATUM(out);
}

/*
** GiST Compress and Decompress methods for segments
** do not do anything.
*/
Datum
gseg_compress(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(PG_GETARG_POINTER(0));
}

Datum
gseg_decompress(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(PG_GETARG_POINTER(0));
}

/*
** The GiST Penalty method for segments
** As in the R-tree paper, we use change in area as our penalty metric
*/
Datum
gseg_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *origentry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *newentry = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *result = (float *) PG_GETARG_POINTER(2);
	SEG		   *ud;
	float		tmp1,
				tmp2;

	ud = DatumGetSegP(DirectFunctionCall2(seg_union,
										  origentry->key,
										  newentry->key));
	rt_seg_size(ud, &tmp1);
	rt_seg_size(DatumGetSegP(origentry->key), &tmp2);
	*result = tmp1 - tmp2;

#ifdef GIST_DEBUG
	fprintf(stderr, "penalty\n");
	fprintf(stderr, "\t%g\n", *result);
#endif

	PG_RETURN_POINTER(result);
}

/*
 * Compare function for gseg_picksplit_item: sort by center.
 */
static int
gseg_picksplit_item_cmp(const void *a, const void *b)
{
	const gseg_picksplit_item *i1 = (const gseg_picksplit_item *) a;
	const gseg_picksplit_item *i2 = (const gseg_picksplit_item *) b;

	if (i1->center < i2->center)
		return -1;
	else if (i1->center == i2->center)
		return 0;
	else
		return 1;
}

/*
 * The GiST PickSplit method for segments
 *
 * We used to use Guttman's split algorithm here, but since the data is 1-D
 * it's easier and more robust to just sort the segments by center-point and
 * split at the middle.
 */
Datum
gseg_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	int			i;
	SEG		   *seg,
			   *seg_l,
			   *seg_r;
	gseg_picksplit_item *sort_items;
	OffsetNumber *left,
			   *right;
	OffsetNumber maxoff;
	OffsetNumber firstright;

#ifdef GIST_DEBUG
	fprintf(stderr, "picksplit\n");
#endif

	/* Valid items in entryvec->vector[] are indexed 1..maxoff */
	maxoff = entryvec->n - 1;

	/*
	 * Prepare the auxiliary array and sort it.
	 */
	sort_items = (gseg_picksplit_item *)
		palloc(maxoff * sizeof(gseg_picksplit_item));
	for (i = 1; i <= maxoff; i++)
	{
		seg = DatumGetSegP(entryvec->vector[i].key);
		/* center calculation is done this way to avoid possible overflow */
		sort_items[i - 1].center = seg->lower * 0.5f + seg->upper * 0.5f;
		sort_items[i - 1].index = i;
		sort_items[i - 1].data = seg;
	}
	qsort(sort_items, maxoff, sizeof(gseg_picksplit_item),
		  gseg_picksplit_item_cmp);

	/* sort items below "firstright" will go into the left side */
	firstright = maxoff / 2;

	v->spl_left = (OffsetNumber *) palloc(maxoff * sizeof(OffsetNumber));
	v->spl_right = (OffsetNumber *) palloc(maxoff * sizeof(OffsetNumber));
	left = v->spl_left;
	v->spl_nleft = 0;
	right = v->spl_right;
	v->spl_nright = 0;

	/*
	 * Emit segments to the left output page, and compute its bounding box.
	 */
	seg_l = (SEG *) palloc(sizeof(SEG));
	memcpy(seg_l, sort_items[0].data, sizeof(SEG));
	*left++ = sort_items[0].index;
	v->spl_nleft++;
	for (i = 1; i < firstright; i++)
	{
		Datum		sortitem = PointerGetDatum(sort_items[i].data);

		seg_l = DatumGetSegP(DirectFunctionCall2(seg_union,
												 PointerGetDatum(seg_l),
												 sortitem));
		*left++ = sort_items[i].index;
		v->spl_nleft++;
	}

	/*
	 * Likewise for the right page.
	 */
	seg_r = (SEG *) palloc(sizeof(SEG));
	memcpy(seg_r, sort_items[firstright].data, sizeof(SEG));
	*right++ = sort_items[firstright].index;
	v->spl_nright++;
	for (i = firstright + 1; i < maxoff; i++)
	{
		Datum		sortitem = PointerGetDatum(sort_items[i].data);

		seg_r = DatumGetSegP(DirectFunctionCall2(seg_union,
												 PointerGetDatum(seg_r),
												 sortitem));
		*right++ = sort_items[i].index;
		v->spl_nright++;
	}

	v->spl_ldatum = PointerGetDatum(seg_l);
	v->spl_rdatum = PointerGetDatum(seg_r);

	PG_RETURN_POINTER(v);
}

/*
** Equality methods
*/
Datum
gseg_same(PG_FUNCTION_ARGS)
{
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	if (DirectFunctionCall2(seg_same, PG_GETARG_DATUM(0), PG_GETARG_DATUM(1)))
		*result = true;
	else
		*result = false;

#ifdef GIST_DEBUG
	fprintf(stderr, "same: %s\n", (*result ? "TRUE" : "FALSE"));
#endif

	PG_RETURN_POINTER(result);
}

/*
** SUPPORT ROUTINES
*/
static Datum
gseg_leaf_consistent(Datum key, Datum query, StrategyNumber strategy)
{
	Datum		retval;

#ifdef GIST_QUERY_DEBUG
	fprintf(stderr, "leaf_consistent, %d\n", strategy);
#endif

	switch (strategy)
	{
		case RTLeftStrategyNumber:
			retval = DirectFunctionCall2(seg_left, key, query);
			break;
		case RTOverLeftStrategyNumber:
			retval = DirectFunctionCall2(seg_over_left, key, query);
			break;
		case RTOverlapStrategyNumber:
			retval = DirectFunctionCall2(seg_overlap, key, query);
			break;
		case RTOverRightStrategyNumber:
			retval = DirectFunctionCall2(seg_over_right, key, query);
			break;
		case RTRightStrategyNumber:
			retval = DirectFunctionCall2(seg_right, key, query);
			break;
		case RTSameStrategyNumber:
			retval = DirectFunctionCall2(seg_same, key, query);
			break;
		case RTContainsStrategyNumber:
		case RTOldContainsStrategyNumber:
			retval = DirectFunctionCall2(seg_contains, key, query);
			break;
		case RTContainedByStrategyNumber:
		case RTOldContainedByStrategyNumber:
			retval = DirectFunctionCall2(seg_contained, key, query);
			break;
		default:
			retval = false;
	}

	PG_RETURN_DATUM(retval);
}

static Datum
gseg_internal_consistent(Datum key, Datum query, StrategyNumber strategy)
{
	bool		retval;

#ifdef GIST_QUERY_DEBUG
	fprintf(stderr, "internal_consistent, %d\n", strategy);
#endif

	switch (strategy)
	{
		case RTLeftStrategyNumber:
			retval =
				!DatumGetBool(DirectFunctionCall2(seg_over_right, key, query));
			break;
		case RTOverLeftStrategyNumber:
			retval =
				!DatumGetBool(DirectFunctionCall2(seg_right, key, query));
			break;
		case RTOverlapStrategyNumber:
			retval =
				DatumGetBool(DirectFunctionCall2(seg_overlap, key, query));
			break;
		case RTOverRightStrategyNumber:
			retval =
				!DatumGetBool(DirectFunctionCall2(seg_left, key, query));
			break;
		case RTRightStrategyNumber:
			retval =
				!DatumGetBool(DirectFunctionCall2(seg_over_left, key, query));
			break;
		case RTSameStrategyNumber:
		case RTContainsStrategyNumber:
		case RTOldContainsStrategyNumber:
			retval =
				DatumGetBool(DirectFunctionCall2(seg_contains, key, query));
			break;
		case RTContainedByStrategyNumber:
		case RTOldContainedByStrategyNumber:
			retval =
				DatumGetBool(DirectFunctionCall2(seg_overlap, key, query));
			break;
		default:
			retval = false;
	}

	PG_RETURN_BOOL(retval);
}

static Datum
gseg_binary_union(Datum r1, Datum r2, int *sizep)
{
	Datum		retval;

	retval = DirectFunctionCall2(seg_union, r1, r2);
	*sizep = sizeof(SEG);

	return retval;
}


Datum
seg_contains(PG_FUNCTION_ARGS)
{
	SEG		   *a = PG_GETARG_SEG_P(0);
	SEG		   *b = PG_GETARG_SEG_P(1);

	PG_RETURN_BOOL((a->lower <= b->lower) && (a->upper >= b->upper));
}

Datum
seg_contained(PG_FUNCTION_ARGS)
{
	Datum		a = PG_GETARG_DATUM(0);
	Datum		b = PG_GETARG_DATUM(1);

	PG_RETURN_DATUM(DirectFunctionCall2(seg_contains, b, a));
}

/*****************************************************************************
 * Operator class for R-tree indexing
 *****************************************************************************/

Datum
seg_same(PG_FUNCTION_ARGS)
{
	int			cmp = DatumGetInt32(
									DirectFunctionCall2(seg_cmp, PG_GETARG_DATUM(0), PG_GETARG_DATUM(1)));

	PG_RETURN_BOOL(cmp == 0);
}

/*	seg_overlap -- does a overlap b?
 */
Datum
seg_overlap(PG_FUNCTION_ARGS)
{
	SEG		   *a = PG_GETARG_SEG_P(0);
	SEG		   *b = PG_GETARG_SEG_P(1);

	PG_RETURN_BOOL(((a->upper >= b->upper) && (a->lower <= b->upper)) ||
				   ((b->upper >= a->upper) && (b->lower <= a->upper)));
}

/*	seg_over_left -- is the right edge of (a) located at or left of the right edge of (b)?
 */
Datum
seg_over_left(PG_FUNCTION_ARGS)
{
	SEG		   *a = PG_GETARG_SEG_P(0);
	SEG		   *b = PG_GETARG_SEG_P(1);

	PG_RETURN_BOOL(a->upper <= b->upper);
}

/*	seg_left -- is (a) entirely on the left of (b)?
 */
Datum
seg_left(PG_FUNCTION_ARGS)
{
	SEG		   *a = PG_GETARG_SEG_P(0);
	SEG		   *b = PG_GETARG_SEG_P(1);

	PG_RETURN_BOOL(a->upper < b->lower);
}

/*	seg_right -- is (a) entirely on the right of (b)?
 */
Datum
seg_right(PG_FUNCTION_ARGS)
{
	SEG		   *a = PG_GETARG_SEG_P(0);
	SEG		   *b = PG_GETARG_SEG_P(1);

	PG_RETURN_BOOL(a->lower > b->upper);
}

/*	seg_over_right -- is the left edge of (a) located at or right of the left edge of (b)?
 */
Datum
seg_over_right(PG_FUNCTION_ARGS)
{
	SEG		   *a = PG_GETARG_SEG_P(0);
	SEG		   *b = PG_GETARG_SEG_P(1);

	PG_RETURN_BOOL(a->lower >= b->lower);
}

Datum
seg_union(PG_FUNCTION_ARGS)
{
	SEG		   *a = PG_GETARG_SEG_P(0);
	SEG		   *b = PG_GETARG_SEG_P(1);
	SEG		   *n;

	n = (SEG *) palloc(sizeof(*n));

	/* take max of upper endpoints */
	if (a->upper > b->upper)
	{
		n->upper = a->upper;
		n->u_sigd = a->u_sigd;
		n->u_ext = a->u_ext;
	}
	else
	{
		n->upper = b->upper;
		n->u_sigd = b->u_sigd;
		n->u_ext = b->u_ext;
	}

	/* take min of lower endpoints */
	if (a->lower < b->lower)
	{
		n->lower = a->lower;
		n->l_sigd = a->l_sigd;
		n->l_ext = a->l_ext;
	}
	else
	{
		n->lower = b->lower;
		n->l_sigd = b->l_sigd;
		n->l_ext = b->l_ext;
	}

	PG_RETURN_POINTER(n);
}

Datum
seg_inter(PG_FUNCTION_ARGS)
{
	SEG		   *a = PG_GETARG_SEG_P(0);
	SEG		   *b = PG_GETARG_SEG_P(1);
	SEG		   *n;

	n = (SEG *) palloc(sizeof(*n));

	/* take min of upper endpoints */
	if (a->upper < b->upper)
	{
		n->upper = a->upper;
		n->u_sigd = a->u_sigd;
		n->u_ext = a->u_ext;
	}
	else
	{
		n->upper = b->upper;
		n->u_sigd = b->u_sigd;
		n->u_ext = b->u_ext;
	}

	/* take max of lower endpoints */
	if (a->lower > b->lower)
	{
		n->lower = a->lower;
		n->l_sigd = a->l_sigd;
		n->l_ext = a->l_ext;
	}
	else
	{
		n->lower = b->lower;
		n->l_sigd = b->l_sigd;
		n->l_ext = b->l_ext;
	}

	PG_RETURN_POINTER(n);
}

static void
rt_seg_size(SEG *a, float *size)
{
	if (a == (SEG *) NULL || a->upper <= a->lower)
		*size = 0.0;
	else
		*size = (float) Abs(a->upper - a->lower);

	return;
}

Datum
seg_size(PG_FUNCTION_ARGS)
{
	SEG		   *seg = PG_GETARG_SEG_P(0);

	PG_RETURN_FLOAT4((float) Abs(seg->upper - seg->lower));
}


/*****************************************************************************
 *				   Miscellaneous operators
 *****************************************************************************/
Datum
seg_cmp(PG_FUNCTION_ARGS)
{
	SEG		   *a = PG_GETARG_SEG_P(0);
	SEG		   *b = PG_GETARG_SEG_P(1);

	/*
	 * First compare on lower boundary position
	 */
	if (a->lower < b->lower)
		PG_RETURN_INT32(-1);
	if (a->lower > b->lower)
		PG_RETURN_INT32(1);

	/*
	 * a->lower == b->lower, so consider type of boundary.
	 *
	 * A '-' lower bound is < any other kind (this could only be relevant if
	 * -HUGE_VAL is used as a regular data value). A '<' lower bound is < any
	 * other kind except '-'. A '>' lower bound is > any other kind.
	 */
	if (a->l_ext != b->l_ext)
	{
		if (a->l_ext == '-')
			PG_RETURN_INT32(-1);
		if (b->l_ext == '-')
			PG_RETURN_INT32(1);
		if (a->l_ext == '<')
			PG_RETURN_INT32(-1);
		if (b->l_ext == '<')
			PG_RETURN_INT32(1);
		if (a->l_ext == '>')
			PG_RETURN_INT32(1);
		if (b->l_ext == '>')
			PG_RETURN_INT32(-1);
	}

	/*
	 * For other boundary types, consider # of significant digits first.
	 */
	if (a->l_sigd < b->l_sigd)	/* (a) is blurred and is likely to include (b) */
		PG_RETURN_INT32(-1);
	if (a->l_sigd > b->l_sigd)	/* (a) is less blurred and is likely to be
								 * included in (b) */
		PG_RETURN_INT32(1);

	/*
	 * For same # of digits, an approximate boundary is more blurred than
	 * exact.
	 */
	if (a->l_ext != b->l_ext)
	{
		if (a->l_ext == '~')	/* (a) is approximate, while (b) is exact */
			PG_RETURN_INT32(-1);
		if (b->l_ext == '~')
			PG_RETURN_INT32(1);
		/* can't get here unless data is corrupt */
		elog(ERROR, "bogus lower boundary types %d %d",
			 (int) a->l_ext, (int) b->l_ext);
	}

	/* at this point, the lower boundaries are identical */

	/*
	 * First compare on upper boundary position
	 */
	if (a->upper < b->upper)
		PG_RETURN_INT32(-1);
	if (a->upper > b->upper)
		PG_RETURN_INT32(1);

	/*
	 * a->upper == b->upper, so consider type of boundary.
	 *
	 * A '-' upper bound is > any other kind (this could only be relevant if
	 * HUGE_VAL is used as a regular data value). A '<' upper bound is < any
	 * other kind. A '>' upper bound is > any other kind except '-'.
	 */
	if (a->u_ext != b->u_ext)
	{
		if (a->u_ext == '-')
			PG_RETURN_INT32(1);
		if (b->u_ext == '-')
			PG_RETURN_INT32(-1);
		if (a->u_ext == '<')
			PG_RETURN_INT32(-1);
		if (b->u_ext == '<')
			PG_RETURN_INT32(1);
		if (a->u_ext == '>')
			PG_RETURN_INT32(1);
		if (b->u_ext == '>')
			PG_RETURN_INT32(-1);
	}

	/*
	 * For other boundary types, consider # of significant digits first. Note
	 * result here is converse of the lower-boundary case.
	 */
	if (a->u_sigd < b->u_sigd)	/* (a) is blurred and is likely to include (b) */
		PG_RETURN_INT32(1);
	if (a->u_sigd > b->u_sigd)	/* (a) is less blurred and is likely to be
								 * included in (b) */
		PG_RETURN_INT32(-1);

	/*
	 * For same # of digits, an approximate boundary is more blurred than
	 * exact.  Again, result is converse of lower-boundary case.
	 */
	if (a->u_ext != b->u_ext)
	{
		if (a->u_ext == '~')	/* (a) is approximate, while (b) is exact */
			PG_RETURN_INT32(1);
		if (b->u_ext == '~')
			PG_RETURN_INT32(-1);
		/* can't get here unless data is corrupt */
		elog(ERROR, "bogus upper boundary types %d %d",
			 (int) a->u_ext, (int) b->u_ext);
	}

	PG_RETURN_INT32(0);
}

Datum
seg_lt(PG_FUNCTION_ARGS)
{
	int			cmp = DatumGetInt32(
									DirectFunctionCall2(seg_cmp, PG_GETARG_DATUM(0), PG_GETARG_DATUM(1)));

	PG_RETURN_BOOL(cmp < 0);
}

Datum
seg_le(PG_FUNCTION_ARGS)
{
	int			cmp = DatumGetInt32(
									DirectFunctionCall2(seg_cmp, PG_GETARG_DATUM(0), PG_GETARG_DATUM(1)));

	PG_RETURN_BOOL(cmp <= 0);
}

Datum
seg_gt(PG_FUNCTION_ARGS)
{
	int			cmp = DatumGetInt32(
									DirectFunctionCall2(seg_cmp, PG_GETARG_DATUM(0), PG_GETARG_DATUM(1)));

	PG_RETURN_BOOL(cmp > 0);
}

Datum
seg_ge(PG_FUNCTION_ARGS)
{
	int			cmp = DatumGetInt32(
									DirectFunctionCall2(seg_cmp, PG_GETARG_DATUM(0), PG_GETARG_DATUM(1)));

	PG_RETURN_BOOL(cmp >= 0);
}


Datum
seg_different(PG_FUNCTION_ARGS)
{
	int			cmp = DatumGetInt32(
									DirectFunctionCall2(seg_cmp, PG_GETARG_DATUM(0), PG_GETARG_DATUM(1)));

	PG_RETURN_BOOL(cmp != 0);
}



/*****************************************************************************
 *				   Auxiliary functions
 *****************************************************************************/

/*
 * The purpose of this routine is to print the given floating point
 * value with exactly n significant digits.  Its behaviour
 * is similar to %.ng except it prints 8.00 where %.ng would
 * print 8.  Returns the length of the string written at "result".
 *
 * Caller must provide a sufficiently large result buffer; 16 bytes
 * should be enough for all known float implementations.
 */
static int
restore(char *result, float val, int n)
{
	char		buf[25] = {
		'0', '0', '0', '0', '0',
		'0', '0', '0', '0', '0',
		'0', '0', '0', '0', '0',
		'0', '0', '0', '0', '0',
		'0', '0', '0', '0', '\0'
	};
	char	   *p;
	int			exp;
	int			i,
				dp,
				sign;

	/*
	 * Put a cap on the number of significant digits to avoid garbage in the
	 * output and ensure we don't overrun the result buffer.  (n should not be
	 * negative, but check to protect ourselves against corrupted data.)
	 */
	if (n <= 0)
		n = FLT_DIG;
	else
		n = Min(n, FLT_DIG);

	/* remember the sign */
	sign = (val < 0 ? 1 : 0);

	/* print, in %e style to start with */
	sprintf(result, "%.*e", n - 1, val);

	/* find the exponent */
	p = strchr(result, 'e');

	/* punt if we have 'inf' or similar */
	if (p == NULL)
		return strlen(result);

	exp = atoi(p + 1);
	if (exp == 0)
	{
		/* just truncate off the 'e+00' */
		*p = '\0';
	}
	else
	{
		if (Abs(exp) <= 4)
		{
			/*
			 * remove the decimal point from the mantissa and write the digits
			 * to the buf array
			 */
			for (p = result + sign, i = 10, dp = 0; *p != 'e'; p++, i++)
			{
				buf[i] = *p;
				if (*p == '.')
				{
					dp = i--;	/* skip the decimal point */
				}
			}
			if (dp == 0)
				dp = i--;		/* no decimal point was found in the above
								 * for() loop */

			if (exp > 0)
			{
				if (dp - 10 + exp >= n)
				{
					/*
					 * the decimal point is behind the last significant digit;
					 * the digits in between must be converted to the exponent
					 * and the decimal point placed after the first digit
					 */
					exp = dp - 10 + exp - n;
					buf[10 + n] = '\0';

					/* insert the decimal point */
					if (n > 1)
					{
						dp = 11;
						for (i = 23; i > dp; i--)
							buf[i] = buf[i - 1];
						buf[dp] = '.';
					}

					/*
					 * adjust the exponent by the number of digits after the
					 * decimal point
					 */
					if (n > 1)
						sprintf(&buf[11 + n], "e%d", exp + n - 1);
					else
						sprintf(&buf[11], "e%d", exp + n - 1);

					if (sign)
					{
						buf[9] = '-';
						strcpy(result, &buf[9]);
					}
					else
						strcpy(result, &buf[10]);
				}
				else
				{				/* insert the decimal point */
					dp += exp;
					for (i = 23; i > dp; i--)
						buf[i] = buf[i - 1];
					buf[11 + n] = '\0';
					buf[dp] = '.';
					if (sign)
					{
						buf[9] = '-';
						strcpy(result, &buf[9]);
					}
					else
						strcpy(result, &buf[10]);
				}
			}
			else
			{					/* exp <= 0 */
				dp += exp - 1;
				buf[10 + n] = '\0';
				buf[dp] = '.';
				if (sign)
				{
					buf[dp - 2] = '-';
					strcpy(result, &buf[dp - 2]);
				}
				else
					strcpy(result, &buf[dp - 1]);
			}
		}

		/* do nothing for Abs(exp) > 4; %e must be OK */
		/* just get rid of zeroes after [eE]- and +zeroes after [Ee]. */

		/* ... this is not done yet. */
	}
	return strlen(result);
}


/*
** Miscellany
*/

/* find out the number of significant digits in a string representing
 * a floating point number
 */
int
significant_digits(const char *s)
{
	const char *p = s;
	int			n,
				c,
				zeroes;

	zeroes = 1;
	/* skip leading zeroes and sign */
	for (c = *p; (c == '0' || c == '+' || c == '-') && c != 0; c = *(++p));

	/* skip decimal point and following zeroes */
	for (c = *p; (c == '0' || c == '.') && c != 0; c = *(++p))
	{
		if (c != '.')
			zeroes++;
	}

	/* count significant digits (n) */
	for (c = *p, n = 0; c != 0; c = *(++p))
	{
		if (!((c >= '0' && c <= '9') || (c == '.')))
			break;
		if (c != '.')
			n++;
	}

	if (!n)
		return zeroes;

	return n;
}
