/*-------------------------------------------------------------------------
 *
 * rtproc.c
 *	  pg_amproc entries for rtrees.
 *
 * NOTE: for largely-historical reasons, the intersection functions should
 * return a NULL pointer (*not* an SQL null value) to indicate "no
 * intersection".  The size functions must be prepared to accept such
 * a pointer and return 0.	This convention means that only pass-by-reference
 * data types can be used as the output of the union and intersection
 * routines, but that's not a big problem.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/rtree/Attic/rtproc.c,v 1.37 2003/08/04 02:39:57 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/geo_decls.h"


Datum
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

Datum
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

Datum
rt_box_size(PG_FUNCTION_ARGS)
{
	BOX		   *a = PG_GETARG_BOX_P(0);

	/* NB: size is an output argument */
	float	   *size = (float *) PG_GETARG_POINTER(1);

	if (a == (BOX *) NULL || a->high.x <= a->low.x || a->high.y <= a->low.y)
		*size = 0.0;
	else
		*size = (float) ((a->high.x - a->low.x) * (a->high.y - a->low.y));

	PG_RETURN_VOID();
}

/*
 *	rt_bigbox_size() -- Compute a size for big boxes.
 *
 *		In an earlier release of the system, this routine did something
 *		different from rt_box_size.  We now use floats, rather than ints,
 *		as the return type for the size routine, so we no longer need to
 *		have a special return type for big boxes.
 */
Datum
rt_bigbox_size(PG_FUNCTION_ARGS)
{
	return rt_box_size(fcinfo);
}

Datum
rt_poly_union(PG_FUNCTION_ARGS)
{
	POLYGON    *a = PG_GETARG_POLYGON_P(0);
	POLYGON    *b = PG_GETARG_POLYGON_P(1);
	POLYGON    *p;

	p = (POLYGON *) palloc0(sizeof(POLYGON));	/* zero any holes */
	p->size = sizeof(POLYGON);
	p->npts = 0;
	p->boundbox.high.x = Max(a->boundbox.high.x, b->boundbox.high.x);
	p->boundbox.high.y = Max(a->boundbox.high.y, b->boundbox.high.y);
	p->boundbox.low.x = Min(a->boundbox.low.x, b->boundbox.low.x);
	p->boundbox.low.y = Min(a->boundbox.low.y, b->boundbox.low.y);

	/* Avoid leaking memory when handed toasted input. */
	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);

	PG_RETURN_POLYGON_P(p);
}

Datum
rt_poly_inter(PG_FUNCTION_ARGS)
{
	POLYGON    *a = PG_GETARG_POLYGON_P(0);
	POLYGON    *b = PG_GETARG_POLYGON_P(1);
	POLYGON    *p;

	p = (POLYGON *) palloc0(sizeof(POLYGON));	/* zero any holes */
	p->size = sizeof(POLYGON);
	p->npts = 0;
	p->boundbox.high.x = Min(a->boundbox.high.x, b->boundbox.high.x);
	p->boundbox.high.y = Min(a->boundbox.high.y, b->boundbox.high.y);
	p->boundbox.low.x = Max(a->boundbox.low.x, b->boundbox.low.x);
	p->boundbox.low.y = Max(a->boundbox.low.y, b->boundbox.low.y);

	if (p->boundbox.high.x < p->boundbox.low.x ||
		p->boundbox.high.y < p->boundbox.low.y)
	{
		pfree(p);
		/* Indicate "no intersection" by returning NULL pointer */
		p = NULL;
	}

	/* Avoid leaking memory when handed toasted input. */
	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);

	PG_RETURN_POLYGON_P(p);
}

Datum
rt_poly_size(PG_FUNCTION_ARGS)
{
	Pointer		aptr = PG_GETARG_POINTER(0);

	/* NB: size is an output argument */
	float	   *size = (float *) PG_GETARG_POINTER(1);
	POLYGON    *a;
	double		xdim,
				ydim;

	/*
	 * Can't just use GETARG because of possibility that input is NULL;
	 * since POLYGON is toastable, GETARG will try to inspect its value
	 */
	if (aptr == NULL)
	{
		*size = 0.0;
		PG_RETURN_VOID();
	}
	/* Now safe to apply GETARG */
	a = PG_GETARG_POLYGON_P(0);

	if (a->boundbox.high.x <= a->boundbox.low.x ||
		a->boundbox.high.y <= a->boundbox.low.y)
		*size = 0.0;
	else
	{
		xdim = (a->boundbox.high.x - a->boundbox.low.x);
		ydim = (a->boundbox.high.y - a->boundbox.low.y);

		*size = (float) (xdim * ydim);
	}

	/* Avoid leaking memory when handed toasted input. */
	PG_FREE_IF_COPY(a, 0);

	PG_RETURN_VOID();
}
