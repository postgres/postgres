/*-------------------------------------------------------------------------
 *
 * spgkdtreeproc.c
 *	  implementation of k-d tree over points for SP-GiST
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/spgist/spgkdtreeproc.c
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
spg_kd_config(PG_FUNCTION_ARGS)
{
	/* spgConfigIn *cfgin = (spgConfigIn *) PG_GETARG_POINTER(0); */
	spgConfigOut *cfg = (spgConfigOut *) PG_GETARG_POINTER(1);

	cfg->prefixType = FLOAT8OID;
	cfg->labelType = VOIDOID;	/* we don't need node labels */
	cfg->canReturnData = true;
	cfg->longValuesOK = false;
	PG_RETURN_VOID();
}

static int
getSide(double coord, bool isX, Point *tst)
{
	double		tstcoord = (isX) ? tst->x : tst->y;

	if (coord == tstcoord)
		return 0;
	else if (coord > tstcoord)
		return 1;
	else
		return -1;
}

Datum
spg_kd_choose(PG_FUNCTION_ARGS)
{
	spgChooseIn *in = (spgChooseIn *) PG_GETARG_POINTER(0);
	spgChooseOut *out = (spgChooseOut *) PG_GETARG_POINTER(1);
	Point	   *inPoint = DatumGetPointP(in->datum);
	double		coord;

	if (in->allTheSame)
		elog(ERROR, "allTheSame should not occur for k-d trees");

	Assert(in->hasPrefix);
	coord = DatumGetFloat8(in->prefixDatum);

	Assert(in->nNodes == 2);

	out->resultType = spgMatchNode;
	out->result.matchNode.nodeN =
		(getSide(coord, in->level % 2, inPoint) > 0) ? 0 : 1;
	out->result.matchNode.levelAdd = 1;
	out->result.matchNode.restDatum = PointPGetDatum(inPoint);

	PG_RETURN_VOID();
}

typedef struct SortedPoint
{
	Point	   *p;
	int			i;
} SortedPoint;

static int
x_cmp(const void *a, const void *b)
{
	SortedPoint *pa = (SortedPoint *) a;
	SortedPoint *pb = (SortedPoint *) b;

	if (pa->p->x == pb->p->x)
		return 0;
	return (pa->p->x > pb->p->x) ? 1 : -1;
}

static int
y_cmp(const void *a, const void *b)
{
	SortedPoint *pa = (SortedPoint *) a;
	SortedPoint *pb = (SortedPoint *) b;

	if (pa->p->y == pb->p->y)
		return 0;
	return (pa->p->y > pb->p->y) ? 1 : -1;
}


Datum
spg_kd_picksplit(PG_FUNCTION_ARGS)
{
	spgPickSplitIn *in = (spgPickSplitIn *) PG_GETARG_POINTER(0);
	spgPickSplitOut *out = (spgPickSplitOut *) PG_GETARG_POINTER(1);
	int			i;
	int			middle;
	SortedPoint *sorted;
	double		coord;

	sorted = palloc(sizeof(*sorted) * in->nTuples);
	for (i = 0; i < in->nTuples; i++)
	{
		sorted[i].p = DatumGetPointP(in->datums[i]);
		sorted[i].i = i;
	}

	qsort(sorted, in->nTuples, sizeof(*sorted),
		  (in->level % 2) ? x_cmp : y_cmp);
	middle = in->nTuples >> 1;
	coord = (in->level % 2) ? sorted[middle].p->x : sorted[middle].p->y;

	out->hasPrefix = true;
	out->prefixDatum = Float8GetDatum(coord);

	out->nNodes = 2;
	out->nodeLabels = NULL;		/* we don't need node labels */

	out->mapTuplesToNodes = palloc(sizeof(int) * in->nTuples);
	out->leafTupleDatums = palloc(sizeof(Datum) * in->nTuples);

	/*
	 * Note: points that have coordinates exactly equal to coord may get
	 * classified into either node, depending on where they happen to fall
	 * in the sorted list.  This is okay as long as the inner_consistent
	 * function descends into both sides for such cases.  This is better
	 * than the alternative of trying to have an exact boundary, because
	 * it keeps the tree balanced even when we have many instances of the
	 * same point value.  So we should never trigger the allTheSame logic.
	 */
	for (i = 0; i < in->nTuples; i++)
	{
		Point	   *p = sorted[i].p;
		int			n = sorted[i].i;

		out->mapTuplesToNodes[n] = (i < middle) ? 0 : 1;
		out->leafTupleDatums[n] = PointPGetDatum(p);
	}

	PG_RETURN_VOID();
}

Datum
spg_kd_inner_consistent(PG_FUNCTION_ARGS)
{
	spgInnerConsistentIn *in = (spgInnerConsistentIn *) PG_GETARG_POINTER(0);
	spgInnerConsistentOut *out = (spgInnerConsistentOut *) PG_GETARG_POINTER(1);
	Point	   *query;
	BOX		   *boxQuery;
	double		coord;

	query = DatumGetPointP(in->query);
	Assert(in->hasPrefix);
	coord = DatumGetFloat8(in->prefixDatum);

	if (in->allTheSame)
		elog(ERROR, "allTheSame should not occur for k-d trees");

	Assert(in->nNodes == 2);
	out->nodeNumbers = (int *) palloc(sizeof(int) * 2);
	out->levelAdds = (int *) palloc(sizeof(int) * 2);
	out->levelAdds[0] = 1;
	out->levelAdds[1] = 1;
	out->nNodes = 0;

	switch (in->strategy)
	{
		case RTLeftStrategyNumber:
			out->nNodes = 1;
			out->nodeNumbers[0] = 0;

			if ((in->level % 2) == 0 || FPge(query->x, coord))
			{
				out->nodeNumbers[1] = 1;
				out->nNodes++;
			}
			break;
		case RTRightStrategyNumber:
			out->nNodes = 1;
			out->nodeNumbers[0] = 1;

			if ((in->level % 2) == 0 || FPle(query->x, coord))
			{
				out->nodeNumbers[1] = 0;
				out->nNodes++;
			}
			break;
		case RTSameStrategyNumber:
			if (in->level % 2)
			{
				if (FPle(query->x, coord))
				{
					out->nodeNumbers[out->nNodes] = 0;
					out->nNodes++;
				}
				if (FPge(query->x, coord))
				{
					out->nodeNumbers[out->nNodes] = 1;
					out->nNodes++;
				}
			}
			else
			{
				if (FPle(query->y, coord))
				{
					out->nodeNumbers[out->nNodes] = 0;
					out->nNodes++;
				}
				if (FPge(query->y, coord))
				{
					out->nodeNumbers[out->nNodes] = 1;
					out->nNodes++;
				}
			}
			break;
		case RTBelowStrategyNumber:
			out->nNodes = 1;
			out->nodeNumbers[0] = 0;

			if ((in->level % 2) == 1 || FPge(query->y, coord))
			{
				out->nodeNumbers[1] = 1;
				out->nNodes++;
			}
			break;
		case RTAboveStrategyNumber:
			out->nNodes = 1;
			out->nodeNumbers[0] = 1;

			if ((in->level % 2) == 1 || FPle(query->y, coord))
			{
				out->nodeNumbers[1] = 0;
				out->nNodes++;
			}
			break;
		case RTContainedByStrategyNumber:

			/*
			 * For this operator, the query is a box not a point.  We cheat to
			 * the extent of assuming that DatumGetPointP won't do anything
			 * that would be bad for a pointer-to-box.
			 */
			boxQuery = DatumGetBoxP(in->query);

			out->nNodes = 1;
			if (in->level % 2)
			{
				if (FPlt(boxQuery->high.x, coord))
					out->nodeNumbers[0] = 0;
				else if (FPgt(boxQuery->low.x, coord))
					out->nodeNumbers[0] = 1;
				else
				{
					out->nodeNumbers[0] = 0;
					out->nodeNumbers[1] = 1;
					out->nNodes = 2;
				}
			}
			else
			{
				if (FPlt(boxQuery->high.y, coord))
					out->nodeNumbers[0] = 0;
				else if (FPgt(boxQuery->low.y, coord))
					out->nodeNumbers[0] = 1;
				else
				{
					out->nodeNumbers[0] = 0;
					out->nodeNumbers[1] = 1;
					out->nNodes = 2;
				}
			}
			break;
		default:
			elog(ERROR, "unrecognized strategy number: %d", in->strategy);
			break;
	}

	PG_RETURN_VOID();
}

/*
 * spg_kd_leaf_consistent() is the same as spg_quad_leaf_consistent(),
 * since we support the same operators and the same leaf data type.
 * So we just borrow that function.
 */
