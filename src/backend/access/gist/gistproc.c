/*-------------------------------------------------------------------------
 *
 * gistproc.c
 *	  Support procedures for GiSTs over 2-D objects (boxes, polygons, circles).
 *
 * This gives R-tree behavior, with Guttman's poly-time split algorithm.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	$PostgreSQL: pgsql/src/backend/access/gist/gistproc.c,v 1.3 2005/10/15 02:49:08 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gist.h"
#include "access/itup.h"
#include "access/rtree.h"
#include "utils/geo_decls.h"


typedef struct
{
	BOX		   *key;
	int			pos;
} KBsort;

static int	compare_KB(const void *a, const void *b);
static bool gist_box_leaf_consistent(BOX *key, BOX *query,
						 StrategyNumber strategy);
static double size_box(Datum dbox);
static bool rtree_internal_consistent(BOX *key, BOX *query,
						  StrategyNumber strategy);


/**************************************************
 * Box ops
 **************************************************/

/*
 * The GiST Consistent method for boxes
 *
 * Should return false if for all data items x below entry,
 * the predicate x op query must be FALSE, where op is the oper
 * corresponding to strategy in the pg_amop table.
 */
Datum
gist_box_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	BOX		   *query = PG_GETARG_BOX_P(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	if (DatumGetBoxP(entry->key) == NULL || query == NULL)
		PG_RETURN_BOOL(FALSE);

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
	memcpy((void *) pageunion, (void *) cur, sizeof(BOX));

	for (i = 1; i < numranges; i++)
	{
		cur = DatumGetBoxP(entryvec->vector[i].key);
		if (pageunion->high.x < cur->high.x)
			pageunion->high.x = cur->high.x;
		if (pageunion->low.x > cur->low.x)
			pageunion->low.x = cur->low.x;
		if (pageunion->high.y < cur->high.y)
			pageunion->high.y = cur->high.y;
		if (pageunion->low.y > cur->low.y)
			pageunion->low.y = cur->low.y;
	}
	*sizep = sizeof(BOX);

	PG_RETURN_POINTER(pageunion);
}

/*
 * GiST Compress methods for boxes
 *
 * do not do anything.
 */
Datum
gist_box_compress(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(PG_GETARG_POINTER(0));
}

/*
 * GiST DeCompress method for boxes (also used for polygons and circles)
 *
 * do not do anything --- we just use the stored box as is.
 */
Datum
gist_box_decompress(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(PG_GETARG_POINTER(0));
}

/*
 * The GiST Penalty method for boxes
 *
 * As in the R-tree paper, we use change in area as our penalty metric
 */
Datum
gist_box_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *origentry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *newentry = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *result = (float *) PG_GETARG_POINTER(2);
	Datum		ud;

	ud = DirectFunctionCall2(rt_box_union, origentry->key, newentry->key);
	*result = (float) (size_box(ud) - size_box(origentry->key));
	PG_RETURN_POINTER(result);
}

/*
 * qsort comparator for box areas
 */
static int
compare_KB(const void *a, const void *b)
{
	BOX		   *abox = ((const KBsort *) a)->key;
	BOX		   *bbox = ((const KBsort *) b)->key;
	double		sa = (abox->high.x - abox->low.x) * (abox->high.y - abox->low.y);
	double		sb = (bbox->high.x - bbox->low.x) * (bbox->high.y - bbox->low.y);

	if (sa == sb)
		return 0;
	return (sa > sb) ? 1 : -1;
}

/*
 * The GiST PickSplit method
 *
 * New linear algorithm, see 'New Linear Node Splitting Algorithm for R-tree',
 * C.H.Ang and T.C.Tan
 */
Datum
gist_box_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	OffsetNumber i;
	OffsetNumber *listL,
			   *listR,
			   *listB,
			   *listT;
	BOX		   *unionL,
			   *unionR,
			   *unionB,
			   *unionT;
	int			posL,
				posR,
				posB,
				posT;
	BOX			pageunion;
	BOX		   *cur;
	char		direction = ' ';
	bool		allisequal = true;
	OffsetNumber maxoff;
	int			nbytes;

	posL = posR = posB = posT = 0;
	maxoff = entryvec->n - 1;

	cur = DatumGetBoxP(entryvec->vector[FirstOffsetNumber].key);
	memcpy((void *) &pageunion, (void *) cur, sizeof(BOX));

	/* find MBR */
	for (i = OffsetNumberNext(FirstOffsetNumber); i <= maxoff; i = OffsetNumberNext(i))
	{
		cur = DatumGetBoxP(entryvec->vector[i].key);
		if (allisequal == true && (
								   pageunion.high.x != cur->high.x ||
								   pageunion.high.y != cur->high.y ||
								   pageunion.low.x != cur->low.x ||
								   pageunion.low.y != cur->low.y
								   ))
			allisequal = false;

		if (pageunion.high.x < cur->high.x)
			pageunion.high.x = cur->high.x;
		if (pageunion.low.x > cur->low.x)
			pageunion.low.x = cur->low.x;
		if (pageunion.high.y < cur->high.y)
			pageunion.high.y = cur->high.y;
		if (pageunion.low.y > cur->low.y)
			pageunion.low.y = cur->low.y;
	}

	nbytes = (maxoff + 2) * sizeof(OffsetNumber);
	listL = (OffsetNumber *) palloc(nbytes);
	listR = (OffsetNumber *) palloc(nbytes);
	unionL = (BOX *) palloc(sizeof(BOX));
	unionR = (BOX *) palloc(sizeof(BOX));
	if (allisequal)
	{
		cur = DatumGetBoxP(entryvec->vector[OffsetNumberNext(FirstOffsetNumber)].key);
		if (memcmp((void *) cur, (void *) &pageunion, sizeof(BOX)) == 0)
		{
			v->spl_left = listL;
			v->spl_right = listR;
			v->spl_nleft = v->spl_nright = 0;
			memcpy((void *) unionL, (void *) &pageunion, sizeof(BOX));
			memcpy((void *) unionR, (void *) &pageunion, sizeof(BOX));

			for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
			{
				if (i <= (maxoff - FirstOffsetNumber + 1) / 2)
				{
					v->spl_left[v->spl_nleft] = i;
					v->spl_nleft++;
				}
				else
				{
					v->spl_right[v->spl_nright] = i;
					v->spl_nright++;
				}
			}
			v->spl_ldatum = BoxPGetDatum(unionL);
			v->spl_rdatum = BoxPGetDatum(unionR);

			PG_RETURN_POINTER(v);
		}
	}

	listB = (OffsetNumber *) palloc(nbytes);
	listT = (OffsetNumber *) palloc(nbytes);
	unionB = (BOX *) palloc(sizeof(BOX));
	unionT = (BOX *) palloc(sizeof(BOX));

#define ADDLIST( list, unionD, pos, num ) do { \
	if ( pos ) { \
		if ( (unionD)->high.x < cur->high.x ) (unionD)->high.x	= cur->high.x; \
		if ( (unionD)->low.x  > cur->low.x	) (unionD)->low.x	= cur->low.x; \
		if ( (unionD)->high.y < cur->high.y ) (unionD)->high.y	= cur->high.y; \
		if ( (unionD)->low.y  > cur->low.y	) (unionD)->low.y	= cur->low.y; \
	} else { \
			memcpy( (void*)(unionD), (void*) cur, sizeof( BOX ) );	\
	} \
	(list)[pos] = num; \
	(pos)++; \
} while(0)

	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		cur = DatumGetBoxP(entryvec->vector[i].key);
		if (cur->low.x - pageunion.low.x < pageunion.high.x - cur->high.x)
			ADDLIST(listL, unionL, posL, i);
		else
			ADDLIST(listR, unionR, posR, i);
		if (cur->low.y - pageunion.low.y < pageunion.high.y - cur->high.y)
			ADDLIST(listB, unionB, posB, i);
		else
			ADDLIST(listT, unionT, posT, i);
	}

	/* bad disposition, sort by ascending and resplit */
	if ((posR == 0 || posL == 0) && (posT == 0 || posB == 0))
	{
		KBsort	   *arr = (KBsort *) palloc(sizeof(KBsort) * maxoff);

		posL = posR = posB = posT = 0;
		for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
		{
			arr[i - 1].key = DatumGetBoxP(entryvec->vector[i].key);
			arr[i - 1].pos = i;
		}
		qsort(arr, maxoff, sizeof(KBsort), compare_KB);
		for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
		{
			cur = arr[i - 1].key;
			if (cur->low.x - pageunion.low.x < pageunion.high.x - cur->high.x)
				ADDLIST(listL, unionL, posL, arr[i - 1].pos);
			else if (cur->low.x - pageunion.low.x == pageunion.high.x - cur->high.x)
			{
				if (posL > posR)
					ADDLIST(listR, unionR, posR, arr[i - 1].pos);
				else
					ADDLIST(listL, unionL, posL, arr[i - 1].pos);
			}
			else
				ADDLIST(listR, unionR, posR, arr[i - 1].pos);

			if (cur->low.y - pageunion.low.y < pageunion.high.y - cur->high.y)
				ADDLIST(listB, unionB, posB, arr[i - 1].pos);
			else if (cur->low.y - pageunion.low.y == pageunion.high.y - cur->high.y)
			{
				if (posB > posT)
					ADDLIST(listT, unionT, posT, arr[i - 1].pos);
				else
					ADDLIST(listB, unionB, posB, arr[i - 1].pos);
			}
			else
				ADDLIST(listT, unionT, posT, arr[i - 1].pos);
		}
	}

	/* which split more optimal? */
	if (Max(posL, posR) < Max(posB, posT))
		direction = 'x';
	else if (Max(posL, posR) > Max(posB, posT))
		direction = 'y';
	else
	{
		Datum		interLR = DirectFunctionCall2(rt_box_inter,
												  BoxPGetDatum(unionL),
												  BoxPGetDatum(unionR));
		Datum		interBT = DirectFunctionCall2(rt_box_inter,
												  BoxPGetDatum(unionB),
												  BoxPGetDatum(unionT));
		double		sizeLR,
					sizeBT;

		sizeLR = size_box(interLR);
		sizeBT = size_box(interBT);

		if (sizeLR < sizeBT)
			direction = 'x';
		else
			direction = 'y';
	}

	if (direction == 'x')
	{
		v->spl_left = listL;
		v->spl_right = listR;
		v->spl_nleft = posL;
		v->spl_nright = posR;
		v->spl_ldatum = BoxPGetDatum(unionL);
		v->spl_rdatum = BoxPGetDatum(unionR);
	}
	else
	{
		v->spl_left = listB;
		v->spl_right = listT;
		v->spl_nleft = posB;
		v->spl_nright = posT;
		v->spl_ldatum = BoxPGetDatum(unionB);
		v->spl_rdatum = BoxPGetDatum(unionT);
	}

	PG_RETURN_POINTER(v);
}

/*
 * Equality method
 */
Datum
gist_box_same(PG_FUNCTION_ARGS)
{
	BOX		   *b1 = PG_GETARG_BOX_P(0);
	BOX		   *b2 = PG_GETARG_BOX_P(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	if (b1 && b2)
		*result = DatumGetBool(DirectFunctionCall2(box_same,
												   PointerGetDatum(b1),
												   PointerGetDatum(b2)));
	else
		*result = (b1 == NULL && b2 == NULL) ? TRUE : FALSE;
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
			retval = FALSE;
	}
	return retval;
}

static double
size_box(Datum dbox)
{
	BOX		   *box = DatumGetBoxP(dbox);

	if (box == NULL || box->high.x <= box->low.x || box->high.y <= box->low.y)
		return 0.0;
	return (box->high.x - box->low.x) * (box->high.y - box->low.y);
}

/*****************************************
 * Common rtree functions (for boxes, polygons, and circles)
 *****************************************/

/*
 * Internal-page consistency for all these types
 *
 * We can use the same function since all types use bounding boxes as the
 * internal-page representation.
 *
 * This implements the same logic as the rtree internal-page strategy map.
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
			retval = FALSE;
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
		retval = palloc(sizeof(GISTENTRY));
		if (DatumGetPointer(entry->key) != NULL)
		{
			POLYGON    *in = DatumGetPolygonP(entry->key);
			BOX		   *r;

			r = (BOX *) palloc(sizeof(BOX));
			memcpy((void *) r, (void *) &(in->boundbox), sizeof(BOX));
			gistentryinit(*retval, PointerGetDatum(r),
						  entry->rel, entry->page,
						  entry->offset, sizeof(BOX), FALSE);

		}
		else
		{
			gistentryinit(*retval, (Datum) 0,
						  entry->rel, entry->page,
						  entry->offset, 0, FALSE);
		}
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
	bool		result;

	if (DatumGetBoxP(entry->key) == NULL || query == NULL)
		PG_RETURN_BOOL(FALSE);

	/*
	 * Since the operators are marked lossy anyway, we can just use
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
		retval = palloc(sizeof(GISTENTRY));
		if (DatumGetCircleP(entry->key) != NULL)
		{
			CIRCLE	   *in = DatumGetCircleP(entry->key);
			BOX		   *r;

			r = (BOX *) palloc(sizeof(BOX));
			r->high.x = in->center.x + in->radius;
			r->low.x = in->center.x - in->radius;
			r->high.y = in->center.y + in->radius;
			r->low.y = in->center.y - in->radius;
			gistentryinit(*retval, PointerGetDatum(r),
						  entry->rel, entry->page,
						  entry->offset, sizeof(BOX), FALSE);

		}
		else
		{
			gistentryinit(*retval, (Datum) 0,
						  entry->rel, entry->page,
						  entry->offset, 0, FALSE);
		}
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
	BOX			bbox;
	bool		result;

	if (DatumGetBoxP(entry->key) == NULL || query == NULL)
		PG_RETURN_BOOL(FALSE);

	/*
	 * Since the operators are marked lossy anyway, we can just use
	 * rtree_internal_consistent even at leaf nodes.  (This works in part
	 * because the index entries are bounding boxes not circles.)
	 */
	bbox.high.x = query->center.x + query->radius;
	bbox.low.x = query->center.x - query->radius;
	bbox.high.y = query->center.y + query->radius;
	bbox.low.y = query->center.y - query->radius;

	result = rtree_internal_consistent(DatumGetBoxP(entry->key),
									   &bbox, strategy);

	PG_RETURN_BOOL(result);
}
