/*-------------------------------------------------------------------------
 *
 * spgquadtreeproc.c
 *	  implementation of quad tree over points for SP-GiST
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/spgist/spgquadtreeproc.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gist.h"		/* for RTree strategy numbers */
#include "access/spgist.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/geo_decls.h"


Datum
spg_quad_config(PG_FUNCTION_ARGS)
{
	/* spgConfigIn *cfgin = (spgConfigIn *) PG_GETARG_POINTER(0); */
	spgConfigOut *cfg = (spgConfigOut *) PG_GETARG_POINTER(1);

	cfg->prefixType = POINTOID;
	cfg->labelType = VOIDOID;	/* we don't need node labels */
	cfg->canReturnData = true;
	cfg->longValuesOK = false;
	PG_RETURN_VOID();
}

#define SPTEST(f, x, y) \
	DatumGetBool(DirectFunctionCall2(f, PointPGetDatum(x), PointPGetDatum(y)))

/*
 * Determine which quadrant a point falls into, relative to the centroid.
 *
 * Quadrants are identified like this:
 *
 *	 4	|  1
 *	----+-----
 *	 3	|  2
 *
 * Points on one of the axes are taken to lie in the lowest-numbered
 * adjacent quadrant.
 */
static int2
getQuadrant(Point *centroid, Point *tst)
{
	if ((SPTEST(point_above, tst, centroid) ||
		 SPTEST(point_horiz, tst, centroid)) &&
		(SPTEST(point_right, tst, centroid) ||
		 SPTEST(point_vert, tst, centroid)))
		return 1;

	if (SPTEST(point_below, tst, centroid) &&
		(SPTEST(point_right, tst, centroid) ||
		 SPTEST(point_vert, tst, centroid)))
		return 2;

	if ((SPTEST(point_below, tst, centroid) ||
		 SPTEST(point_horiz, tst, centroid)) &&
		SPTEST(point_left, tst, centroid))
		return 3;

	if (SPTEST(point_above, tst, centroid) &&
		SPTEST(point_left, tst, centroid))
		return 4;

	elog(ERROR, "getQuadrant: impossible case");
	return 0;
}


Datum
spg_quad_choose(PG_FUNCTION_ARGS)
{
	spgChooseIn *in = (spgChooseIn *) PG_GETARG_POINTER(0);
	spgChooseOut *out = (spgChooseOut *) PG_GETARG_POINTER(1);
	Point	   *inPoint = DatumGetPointP(in->datum),
			   *centroid;

	if (in->allTheSame)
	{
		out->resultType = spgMatchNode;
		/* nodeN will be set by core */
		out->result.matchNode.levelAdd = 0;
		out->result.matchNode.restDatum = PointPGetDatum(inPoint);
		PG_RETURN_VOID();
	}

	Assert(in->hasPrefix);
	centroid = DatumGetPointP(in->prefixDatum);

	Assert(in->nNodes == 4);

	out->resultType = spgMatchNode;
	out->result.matchNode.nodeN = getQuadrant(centroid, inPoint) - 1;
	out->result.matchNode.levelAdd = 0;
	out->result.matchNode.restDatum = PointPGetDatum(inPoint);

	PG_RETURN_VOID();
}

#ifdef USE_MEDIAN
static int
x_cmp(const void *a, const void *b, void *arg)
{
	Point	   *pa = *(Point **) a;
	Point	   *pb = *(Point **) b;

	if (pa->x == pb->x)
		return 0;
	return (pa->x > pb->x) ? 1 : -1;
}

static int
y_cmp(const void *a, const void *b, void *arg)
{
	Point	   *pa = *(Point **) a;
	Point	   *pb = *(Point **) b;

	if (pa->y == pb->y)
		return 0;
	return (pa->y > pb->y) ? 1 : -1;
}
#endif

Datum
spg_quad_picksplit(PG_FUNCTION_ARGS)
{
	spgPickSplitIn *in = (spgPickSplitIn *) PG_GETARG_POINTER(0);
	spgPickSplitOut *out = (spgPickSplitOut *) PG_GETARG_POINTER(1);
	int			i;
	Point	   *centroid;

#ifdef USE_MEDIAN
	/* Use the median values of x and y as the centroid point */
	Point	  **sorted;

	sorted = palloc(sizeof(*sorted) * in->nTuples);
	for (i = 0; i < in->nTuples; i++)
		sorted[i] = DatumGetPointP(in->datums[i]);

	centroid = palloc(sizeof(*centroid));

	qsort(sorted, in->nTuples, sizeof(*sorted), x_cmp);
	centroid->x = sorted[in->nTuples >> 1]->x;
	qsort(sorted, in->nTuples, sizeof(*sorted), y_cmp);
	centroid->y = sorted[in->nTuples >> 1]->y;
#else
	/* Use the average values of x and y as the centroid point */
	centroid = palloc0(sizeof(*centroid));

	for (i = 0; i < in->nTuples; i++)
	{
		centroid->x += DatumGetPointP(in->datums[i])->x;
		centroid->y += DatumGetPointP(in->datums[i])->y;
	}

	centroid->x /= in->nTuples;
	centroid->y /= in->nTuples;
#endif

	out->hasPrefix = true;
	out->prefixDatum = PointPGetDatum(centroid);

	out->nNodes = 4;
	out->nodeLabels = NULL;		/* we don't need node labels */

	out->mapTuplesToNodes = palloc(sizeof(int) * in->nTuples);
	out->leafTupleDatums = palloc(sizeof(Datum) * in->nTuples);

	for (i = 0; i < in->nTuples; i++)
	{
		Point	   *p = DatumGetPointP(in->datums[i]);
		int			quadrant = getQuadrant(centroid, p) - 1;

		out->leafTupleDatums[i] = PointPGetDatum(p);
		out->mapTuplesToNodes[i] = quadrant;
	}

	PG_RETURN_VOID();
}


/* Subroutine to fill out->nodeNumbers[] for spg_quad_inner_consistent */
static void
setNodes(spgInnerConsistentOut *out, bool isAll, int first, int second)
{
	if (isAll)
	{
		out->nNodes = 4;
		out->nodeNumbers[0] = 0;
		out->nodeNumbers[1] = 1;
		out->nodeNumbers[2] = 2;
		out->nodeNumbers[3] = 3;
	}
	else
	{
		out->nNodes = 2;
		out->nodeNumbers[0] = first - 1;
		out->nodeNumbers[1] = second - 1;
	}
}


Datum
spg_quad_inner_consistent(PG_FUNCTION_ARGS)
{
	spgInnerConsistentIn *in = (spgInnerConsistentIn *) PG_GETARG_POINTER(0);
	spgInnerConsistentOut *out = (spgInnerConsistentOut *) PG_GETARG_POINTER(1);
	Point	   *query,
			   *centroid;
	BOX		   *boxQuery;

	query = DatumGetPointP(in->query);
	Assert(in->hasPrefix);
	centroid = DatumGetPointP(in->prefixDatum);

	if (in->allTheSame)
	{
		/* Report that all nodes should be visited */
		int		i;

		out->nNodes = in->nNodes;
		out->nodeNumbers = (int *) palloc(sizeof(int) * in->nNodes);
		for (i = 0; i < in->nNodes; i++)
			out->nodeNumbers[i] = i;
		PG_RETURN_VOID();
	}

	Assert(in->nNodes == 4);
	out->nodeNumbers = (int *) palloc(sizeof(int) * 4);

	switch (in->strategy)
	{
		case RTLeftStrategyNumber:
			setNodes(out, SPTEST(point_left, centroid, query), 3, 4);
			break;
		case RTRightStrategyNumber:
			setNodes(out, SPTEST(point_right, centroid, query), 1, 2);
			break;
		case RTSameStrategyNumber:
			out->nNodes = 1;
			out->nodeNumbers[0] = getQuadrant(centroid, query) - 1;
			break;
		case RTBelowStrategyNumber:
			setNodes(out, SPTEST(point_below, centroid, query), 2, 3);
			break;
		case RTAboveStrategyNumber:
			setNodes(out, SPTEST(point_above, centroid, query), 1, 4);
			break;
		case RTContainedByStrategyNumber:

			/*
			 * For this operator, the query is a box not a point.  We cheat to
			 * the extent of assuming that DatumGetPointP won't do anything
			 * that would be bad for a pointer-to-box.
			 */
			boxQuery = DatumGetBoxP(in->query);

			if (DatumGetBool(DirectFunctionCall2(box_contain_pt,
												 PointerGetDatum(boxQuery),
												 PointerGetDatum(centroid))))
			{
				/* centroid is in box, so descend to all quadrants */
				setNodes(out, true, 0, 0);
			}
			else
			{
				/* identify quadrant(s) containing all corners of box */
				Point		p;
				int			i,
							r = 0;

				p = boxQuery->low;
				r |= 1 << (getQuadrant(centroid, &p) - 1);

				p.y = boxQuery->high.y;
				r |= 1 << (getQuadrant(centroid, &p) - 1);

				p = boxQuery->high;
				r |= 1 << (getQuadrant(centroid, &p) - 1);

				p.x = boxQuery->low.x;
				r |= 1 << (getQuadrant(centroid, &p) - 1);

				/* we must descend into those quadrant(s) */
				out->nNodes = 0;
				for (i = 0; i < 4; i++)
				{
					if (r & (1 << i))
					{
						out->nodeNumbers[out->nNodes] = i;
						out->nNodes++;
					}
				}
			}
			break;
		default:
			elog(ERROR, "unrecognized strategy number: %d", in->strategy);
			break;
	}

	PG_RETURN_VOID();
}


Datum
spg_quad_leaf_consistent(PG_FUNCTION_ARGS)
{
	spgLeafConsistentIn *in = (spgLeafConsistentIn *) PG_GETARG_POINTER(0);
	spgLeafConsistentOut *out = (spgLeafConsistentOut *) PG_GETARG_POINTER(1);
	Point	   *query = DatumGetPointP(in->query);
	Point	   *datum = DatumGetPointP(in->leafDatum);
	bool		res;

	/* all tests are exact */
	out->recheck = false;

	/* leafDatum is what it is... */
	out->leafValue = in->leafDatum;

	switch (in->strategy)
	{
		case RTLeftStrategyNumber:
			res = SPTEST(point_left, datum, query);
			break;
		case RTRightStrategyNumber:
			res = SPTEST(point_right, datum, query);
			break;
		case RTSameStrategyNumber:
			res = SPTEST(point_eq, datum, query);
			break;
		case RTBelowStrategyNumber:
			res = SPTEST(point_below, datum, query);
			break;
		case RTAboveStrategyNumber:
			res = SPTEST(point_above, datum, query);
			break;
		case RTContainedByStrategyNumber:

			/*
			 * For this operator, the query is a box not a point.  We cheat to
			 * the extent of assuming that DatumGetPointP won't do anything
			 * that would be bad for a pointer-to-box.
			 */
			res = SPTEST(box_contain_pt, query, datum);
			break;
		default:
			elog(ERROR, "unrecognized strategy number: %d", in->strategy);
			res = false;
			break;
	}

	PG_RETURN_BOOL(res);
}
