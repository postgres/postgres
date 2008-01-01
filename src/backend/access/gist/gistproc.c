/*-------------------------------------------------------------------------
 *
 * gistproc.c
 *	  Support procedures for GiSTs over 2-D objects (boxes, polygons, circles).
 *
 * This gives R-tree behavior, with Guttman's poly-time split algorithm.
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	$PostgreSQL: pgsql/src/backend/access/gist/gistproc.c,v 1.13 2008/01/01 19:45:46 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gist.h"
#include "access/skey.h"
#include "utils/geo_decls.h"


static bool gist_box_leaf_consistent(BOX *key, BOX *query,
						 StrategyNumber strategy);
static double size_box(Datum dbox);
static bool rtree_internal_consistent(BOX *key, BOX *query,
						  StrategyNumber strategy);


/**************************************************
 * Box ops
 **************************************************/

static Datum
rt_box_union(PG_FUNCTION_ARGS)
{
	BOX		   *a = PG_GETARG_BOX_P(0);
	BOX		   *b = PG_GETARG_BOX_P(1);
	BOX		   *n;

	n = (BOX *) palloc(sizeof(BOX));

	n->high.x = Max(a->high.x, b->high.x);
	n->high.y = Max(a->high.y, b->high.y);
	n->low.x = Min(a->low.x, b->low.x);
	n->low.y = Min(a->low.y, b->low.y);

	PG_RETURN_BOX_P(n);
}

static Datum
rt_box_inter(PG_FUNCTION_ARGS)
{
	BOX		   *a = PG_GETARG_BOX_P(0);
	BOX		   *b = PG_GETARG_BOX_P(1);
	BOX		   *n;

	n = (BOX *) palloc(sizeof(BOX));

	n->high.x = Min(a->high.x, b->high.x);
	n->high.y = Min(a->high.y, b->high.y);
	n->low.x = Max(a->low.x, b->low.x);
	n->low.y = Max(a->low.y, b->low.y);

	if (n->high.x < n->low.x || n->high.y < n->low.y)
	{
		pfree(n);
		/* Indicate "no intersection" by returning NULL pointer */
		n = NULL;
	}

	PG_RETURN_BOX_P(n);
}

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

static void
adjustBox(BOX *b, BOX *addon)
{
	if (b->high.x < addon->high.x)
		b->high.x = addon->high.x;
	if (b->low.x > addon->low.x)
		b->low.x = addon->low.x;
	if (b->high.y < addon->high.y)
		b->high.y = addon->high.y;
	if (b->low.y > addon->low.y)
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
	memcpy((void *) pageunion, (void *) cur, sizeof(BOX));

	for (i = 1; i < numranges; i++)
	{
		cur = DatumGetBoxP(entryvec->vector[i].key);
		adjustBox(pageunion, cur);
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

static void
chooseLR(GIST_SPLITVEC *v,
		 OffsetNumber *list1, int nlist1, BOX *union1,
		 OffsetNumber *list2, int nlist2, BOX *union2)
{
	bool		firstToLeft = true;

	if (v->spl_ldatum_exists || v->spl_rdatum_exists)
	{
		if (v->spl_ldatum_exists && v->spl_rdatum_exists)
		{
			BOX			LRl = *union1,
						LRr = *union2;
			BOX			RLl = *union2,
						RLr = *union1;
			double		sizeLR,
						sizeRL;

			adjustBox(&LRl, DatumGetBoxP(v->spl_ldatum));
			adjustBox(&LRr, DatumGetBoxP(v->spl_rdatum));
			adjustBox(&RLl, DatumGetBoxP(v->spl_ldatum));
			adjustBox(&RLr, DatumGetBoxP(v->spl_rdatum));

			sizeLR = size_box(DirectFunctionCall2(rt_box_inter, BoxPGetDatum(&LRl), BoxPGetDatum(&LRr)));
			sizeRL = size_box(DirectFunctionCall2(rt_box_inter, BoxPGetDatum(&RLl), BoxPGetDatum(&RLr)));

			if (sizeLR > sizeRL)
				firstToLeft = false;

		}
		else
		{
			float		p1,
						p2;
			GISTENTRY	oldUnion,
						addon;

			gistentryinit(oldUnion, (v->spl_ldatum_exists) ? v->spl_ldatum : v->spl_rdatum,
						  NULL, NULL, InvalidOffsetNumber, FALSE);

			gistentryinit(addon, BoxPGetDatum(union1), NULL, NULL, InvalidOffsetNumber, FALSE);
			DirectFunctionCall3(gist_box_penalty, PointerGetDatum(&oldUnion), PointerGetDatum(&union1), PointerGetDatum(&p1));
			gistentryinit(addon, BoxPGetDatum(union2), NULL, NULL, InvalidOffsetNumber, FALSE);
			DirectFunctionCall3(gist_box_penalty, PointerGetDatum(&oldUnion), PointerGetDatum(&union2), PointerGetDatum(&p2));

			if ((v->spl_ldatum_exists && p1 > p2) || (v->spl_rdatum_exists && p1 < p2))
				firstToLeft = false;
		}
	}

	if (firstToLeft)
	{
		v->spl_left = list1;
		v->spl_right = list2;
		v->spl_nleft = nlist1;
		v->spl_nright = nlist2;
		if (v->spl_ldatum_exists)
			adjustBox(union1, DatumGetBoxP(v->spl_ldatum));
		v->spl_ldatum = BoxPGetDatum(union1);
		if (v->spl_rdatum_exists)
			adjustBox(union2, DatumGetBoxP(v->spl_rdatum));
		v->spl_rdatum = BoxPGetDatum(union2);
	}
	else
	{
		v->spl_left = list2;
		v->spl_right = list1;
		v->spl_nleft = nlist2;
		v->spl_nright = nlist1;
		if (v->spl_ldatum_exists)
			adjustBox(union2, DatumGetBoxP(v->spl_ldatum));
		v->spl_ldatum = BoxPGetDatum(union2);
		if (v->spl_rdatum_exists)
			adjustBox(union1, DatumGetBoxP(v->spl_rdatum));
		v->spl_rdatum = BoxPGetDatum(union1);
	}

	v->spl_ldatum_exists = v->spl_rdatum_exists = false;
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

		adjustBox(&pageunion, cur);
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

			if (v->spl_ldatum_exists)
				adjustBox(unionL, DatumGetBoxP(v->spl_ldatum));
			v->spl_ldatum = BoxPGetDatum(unionL);

			if (v->spl_rdatum_exists)
				adjustBox(unionR, DatumGetBoxP(v->spl_rdatum));
			v->spl_rdatum = BoxPGetDatum(unionR);

			v->spl_ldatum_exists = v->spl_rdatum_exists = false;

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

#define LIMIT_RATIO 0.1
#define _IS_BADRATIO(x,y)	( (y) == 0 || (float)(x)/(float)(y) < LIMIT_RATIO )
#define IS_BADRATIO(x,y) ( _IS_BADRATIO((x),(y)) || _IS_BADRATIO((y),(x)) )
	/* bad disposition, try to split by centers of boxes  */
	if (IS_BADRATIO(posR, posL) && IS_BADRATIO(posT, posB))
	{
		double		avgCenterX = 0.0,
					avgCenterY = 0.0;
		double		CenterX,
					CenterY;

		for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
		{
			cur = DatumGetBoxP(entryvec->vector[i].key);
			avgCenterX += ((double) cur->high.x + (double) cur->low.x) / 2.0;
			avgCenterY += ((double) cur->high.y + (double) cur->low.y) / 2.0;
		}

		avgCenterX /= maxoff;
		avgCenterY /= maxoff;

		posL = posR = posB = posT = 0;
		for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
		{
			cur = DatumGetBoxP(entryvec->vector[i].key);

			CenterX = ((double) cur->high.x + (double) cur->low.x) / 2.0;
			CenterY = ((double) cur->high.y + (double) cur->low.y) / 2.0;

			if (CenterX < avgCenterX)
				ADDLIST(listL, unionL, posL, i);
			else if (CenterX == avgCenterX)
			{
				if (posL > posR)
					ADDLIST(listR, unionR, posR, i);
				else
					ADDLIST(listL, unionL, posL, i);
			}
			else
				ADDLIST(listR, unionR, posR, i);

			if (CenterY < avgCenterY)
				ADDLIST(listB, unionB, posB, i);
			else if (CenterY == avgCenterY)
			{
				if (posB > posT)
					ADDLIST(listT, unionT, posT, i);
				else
					ADDLIST(listB, unionB, posB, i);
			}
			else
				ADDLIST(listT, unionT, posT, i);
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
		chooseLR(v,
				 listL, posL, unionL,
				 listR, posR, unionR);
	else
		chooseLR(v,
				 listB, posB, unionB,
				 listT, posT, unionT);

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
		case RTOldContainsStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_contain,
													  PointerGetDatum(key),
													PointerGetDatum(query)));
			break;
		case RTContainedByStrategyNumber:
		case RTOldContainedByStrategyNumber:
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
		case RTOldContainsStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_contain,
													  PointerGetDatum(key),
													PointerGetDatum(query)));
			break;
		case RTContainedByStrategyNumber:
		case RTOldContainedByStrategyNumber:
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
						  entry->offset, FALSE);

		}
		else
		{
			gistentryinit(*retval, (Datum) 0,
						  entry->rel, entry->page,
						  entry->offset, FALSE);
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
						  entry->offset, FALSE);

		}
		else
		{
			gistentryinit(*retval, (Datum) 0,
						  entry->rel, entry->page,
						  entry->offset, FALSE);
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
