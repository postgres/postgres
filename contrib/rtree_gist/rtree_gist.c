/*-------------------------------------------------------------------------
 *
 * rtree_gist.c
 *	  pg_amproc entries for GiSTs over 2-D boxes.
 * This gives R-tree behavior, with Guttman's poly-time split algorithm.
 *
 *
 *
 * IDENTIFICATION
 *	$Header: /cvsroot/pgsql/contrib/rtree_gist/Attic/rtree_gist.c,v 1.7 2003/07/27 17:10:06 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gist.h"
#include "access/itup.h"
#include "access/rtree.h"
#include "utils/geo_decls.h"

typedef Datum (*RDF) (PG_FUNCTION_ARGS);
typedef Datum (*BINARY_UNION) (Datum, Datum, int *);
typedef float (*SIZE_BOX) (Datum);

/*
** box ops
*/
PG_FUNCTION_INFO_V1(gbox_compress);
PG_FUNCTION_INFO_V1(gbox_union);
PG_FUNCTION_INFO_V1(gbox_picksplit);
PG_FUNCTION_INFO_V1(gbox_consistent);
PG_FUNCTION_INFO_V1(gbox_penalty);
PG_FUNCTION_INFO_V1(gbox_same);

Datum		gbox_compress(PG_FUNCTION_ARGS);
Datum		gbox_union(PG_FUNCTION_ARGS);
Datum		gbox_picksplit(PG_FUNCTION_ARGS);
Datum		gbox_consistent(PG_FUNCTION_ARGS);
Datum		gbox_penalty(PG_FUNCTION_ARGS);
Datum		gbox_same(PG_FUNCTION_ARGS);

static bool gbox_leaf_consistent(BOX *key, BOX *query, StrategyNumber strategy);
static float size_box(Datum box);

/*
** Polygon ops
*/
PG_FUNCTION_INFO_V1(gpoly_compress);
PG_FUNCTION_INFO_V1(gpoly_consistent);

Datum		gpoly_compress(PG_FUNCTION_ARGS);
Datum		gpoly_consistent(PG_FUNCTION_ARGS);

/*
** Common rtree-function (for all ops)
*/
static bool rtree_internal_consistent(BOX *key, BOX *query, StrategyNumber strategy);

PG_FUNCTION_INFO_V1(rtree_decompress);

Datum		rtree_decompress(PG_FUNCTION_ARGS);

/**************************************************
 * Box ops
 **************************************************/

/*
** The GiST Consistent method for boxes
** Should return false if for all data items x below entry,
** the predicate x op query == FALSE, where op is the oper
** corresponding to strategy in the pg_amop table.
*/
Datum
gbox_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	BOX		   *query = (BOX *) PG_GETARG_POINTER(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/*
	 * * if entry is not leaf, use gbox_internal_consistent, * else use
	 * gbox_leaf_consistent
	 */
	if (!(DatumGetPointer(entry->key) != NULL && query))
		PG_RETURN_BOOL(FALSE);

	if (GIST_LEAF(entry))
		PG_RETURN_BOOL(gbox_leaf_consistent((BOX *) DatumGetPointer(entry->key), query, strategy));
	else
		PG_RETURN_BOOL(rtree_internal_consistent((BOX *) DatumGetPointer(entry->key), query, strategy));
}


/*
** The GiST Union method for boxes
** returns the minimal bounding box that encloses all the entries in entryvec
*/
Datum
gbox_union(PG_FUNCTION_ARGS)
{
	bytea	   *entryvec = (bytea *) PG_GETARG_POINTER(0);
	int		   *sizep = (int *) PG_GETARG_POINTER(1);
	int			numranges,
				i;
	BOX		   *cur,
			   *pageunion;

	numranges = (VARSIZE(entryvec) - VARHDRSZ) / sizeof(GISTENTRY);
	pageunion = (BOX *) palloc(sizeof(BOX));
	cur = DatumGetBoxP(((GISTENTRY *) VARDATA(entryvec))[0].key);
	memcpy((void *) pageunion, (void *) cur, sizeof(BOX));

	for (i = 1; i < numranges; i++)
	{
		cur = DatumGetBoxP(((GISTENTRY *) VARDATA(entryvec))[i].key);
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
** GiST Compress methods for boxes
** do not do anything.
*/
Datum
gbox_compress(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(PG_GETARG_POINTER(0));
}

/*
** The GiST Penalty method for boxes
** As in the R-tree paper, we use change in area as our penalty metric
*/
Datum
gbox_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *origentry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *newentry = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *result = (float *) PG_GETARG_POINTER(2);
	Datum		ud;
	float		tmp1;

	ud = DirectFunctionCall2(rt_box_union, origentry->key, newentry->key);
	tmp1 = size_box(ud);
	if (DatumGetPointer(ud) != NULL)
		pfree(DatumGetPointer(ud));

	*result = tmp1 - size_box(origentry->key);
	PG_RETURN_POINTER(result);
}

typedef struct
{
	BOX		   *key;
	int			pos;
}	KBsort;

static int
compare_KB(const void *a, const void *b)
{
	BOX		   *abox = ((KBsort *) a)->key;
	BOX		   *bbox = ((KBsort *) b)->key;
	float		sa = (abox->high.x - abox->low.x) * (abox->high.y - abox->low.y);
	float		sb = (bbox->high.x - bbox->low.x) * (bbox->high.y - bbox->low.y);

	if (sa == sb)
		return 0;
	return (sa > sb) ? 1 : -1;
}

/*
** The GiST PickSplit method
** New linear algorithm, see 'New Linear Node Splitting Algorithm for R-tree',
** C.H.Ang and T.C.Tan
*/
Datum
gbox_picksplit(PG_FUNCTION_ARGS)
{
	bytea	   *entryvec = (bytea *) PG_GETARG_POINTER(0);
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
	maxoff = ((VARSIZE(entryvec) - VARHDRSZ) / sizeof(GISTENTRY)) - 1;

	cur = DatumGetBoxP(((GISTENTRY *) VARDATA(entryvec))[FirstOffsetNumber].key);
	memcpy((void *) &pageunion, (void *) cur, sizeof(BOX));

	/* find MBR */
	for (i = OffsetNumberNext(FirstOffsetNumber); i <= maxoff; i = OffsetNumberNext(i))
	{
		cur = DatumGetBoxP(((GISTENTRY *) VARDATA(entryvec))[i].key);
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
		cur = DatumGetBoxP(((GISTENTRY *) VARDATA(entryvec))[OffsetNumberNext(FirstOffsetNumber)].key);
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
		if ( unionD->high.x < cur->high.x ) unionD->high.x	= cur->high.x; \
		if ( unionD->low.x	> cur->low.x  ) unionD->low.x	= cur->low.x; \
		if ( unionD->high.y < cur->high.y ) unionD->high.y	= cur->high.y; \
		if ( unionD->low.y	> cur->low.y  ) unionD->low.y	= cur->low.y; \
	} else { \
			memcpy( (void*)unionD, (void*) cur, sizeof( BOX ) );  \
	} \
	list[pos] = num; \
	(pos)++; \
} while(0)

	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		cur = DatumGetBoxP(((GISTENTRY *) VARDATA(entryvec))[i].key);
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
			arr[i - 1].key = DatumGetBoxP(((GISTENTRY *) VARDATA(entryvec))[i].key);
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
		pfree(arr);
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
		float		sizeLR,
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
		pfree(unionB);
		pfree(listB);
		pfree(unionT);
		pfree(listT);

		v->spl_left = listL;
		v->spl_right = listR;
		v->spl_nleft = posL;
		v->spl_nright = posR;
		v->spl_ldatum = BoxPGetDatum(unionL);
		v->spl_rdatum = BoxPGetDatum(unionR);
	}
	else
	{
		pfree(unionR);
		pfree(listR);
		pfree(unionL);
		pfree(listL);

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
** Equality method
*/
Datum
gbox_same(PG_FUNCTION_ARGS)
{
	BOX		   *b1 = (BOX *) PG_GETARG_POINTER(0);
	BOX		   *b2 = (BOX *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	if (b1 && b2)
		*result = DatumGetBool(DirectFunctionCall2(box_same, PointerGetDatum(b1), PointerGetDatum(b2)));
	else
		*result = (b1 == NULL && b2 == NULL) ? TRUE : FALSE;
	PG_RETURN_POINTER(result);
}

/*
** SUPPORT ROUTINES for boxes
*/
static bool
gbox_leaf_consistent(BOX *key,
					 BOX *query,
					 StrategyNumber strategy)
{
	bool		retval;

	switch (strategy)
	{
		case RTLeftStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_left, PointerGetDatum(key), PointerGetDatum(query)));
			break;
		case RTOverLeftStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_overleft, PointerGetDatum(key), PointerGetDatum(query)));
			break;
		case RTOverlapStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_overlap, PointerGetDatum(key), PointerGetDatum(query)));
			break;
		case RTOverRightStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_overright, PointerGetDatum(key), PointerGetDatum(query)));
			break;
		case RTRightStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_right, PointerGetDatum(key), PointerGetDatum(query)));
			break;
		case RTSameStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_same, PointerGetDatum(key), PointerGetDatum(query)));
			break;
		case RTContainsStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_contain, PointerGetDatum(key), PointerGetDatum(query)));
			break;
		case RTContainedByStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_contained, PointerGetDatum(key), PointerGetDatum(query)));
			break;
		default:
			retval = FALSE;
	}
	return (retval);
}

static float
size_box(Datum box)
{
	if (DatumGetPointer(box) != NULL)
	{
		float		size;

		DirectFunctionCall2(rt_box_size,
							box, PointerGetDatum(&size));
		return size;
	}
	else
		return 0.0;
}

/**************************************************
 * Polygon ops
 **************************************************/

Datum
gpoly_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval;

	if (entry->leafkey)
	{
		retval = palloc(sizeof(GISTENTRY));
		if (DatumGetPointer(entry->key) != NULL)
		{
			POLYGON    *in;
			BOX		   *r;

			in = (POLYGON *) PG_DETOAST_DATUM(entry->key);
			r = (BOX *) palloc(sizeof(BOX));
			memcpy((void *) r, (void *) &(in->boundbox), sizeof(BOX));
			if (in != (POLYGON *) DatumGetPointer(entry->key))
				pfree(in);

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

Datum
gpoly_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	POLYGON    *query = (POLYGON *) PG_DETOAST_DATUM(PG_GETARG_POINTER(1));
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	bool		result;

	/*
	 * * if entry is not leaf, use gbox_internal_consistent, * else use
	 * gbox_leaf_consistent
	 */
	if (!(DatumGetPointer(entry->key) != NULL && query))
		PG_RETURN_BOOL(FALSE);

	result = rtree_internal_consistent((BOX *) DatumGetPointer(entry->key),
									   &(query->boundbox), strategy);

	PG_FREE_IF_COPY(query, 1);
	PG_RETURN_BOOL(result);
}

/*****************************************
 * Common rtree-function (for all ops)
 *****************************************/

static bool
rtree_internal_consistent(BOX *key,
						  BOX *query,
						  StrategyNumber strategy)
{
	bool		retval;

	switch (strategy)
	{
		case RTLeftStrategyNumber:
		case RTOverLeftStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_overleft, PointerGetDatum(key), PointerGetDatum(query)));
			break;
		case RTOverlapStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_overlap, PointerGetDatum(key), PointerGetDatum(query)));
			break;
		case RTOverRightStrategyNumber:
		case RTRightStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_right, PointerGetDatum(key), PointerGetDatum(query)));
			break;
		case RTSameStrategyNumber:
		case RTContainsStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_contain, PointerGetDatum(key), PointerGetDatum(query)));
			break;
		case RTContainedByStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_overlap, PointerGetDatum(key), PointerGetDatum(query)));
			break;
		default:
			retval = FALSE;
	}
	return (retval);
}

/*
** GiST DeCompress methods
** do not do anything.
*/
Datum
rtree_decompress(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(PG_GETARG_POINTER(0));
}
