/*-------------------------------------------------------------------------
 *
 * rtproc.c--
 *	  pg_amproc entries for rtrees.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/rtree/Attic/rtproc.c,v 1.15 1998/01/07 21:02:05 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <utils/builtins.h>
#include <utils/geo_decls.h>
#ifndef HAVE_MEMMOVE
#include <regex/utils.h>
#else
#include <string.h>
#endif

BOX
		   *
rt_box_union(BOX *a, BOX *b)
{
	BOX		   *n;

	if ((n = (BOX *) palloc(sizeof(*n))) == (BOX *) NULL)
		elog(ERROR, "Cannot allocate box for union");

	n->high.x = Max(a->high.x, b->high.x);
	n->high.y = Max(a->high.y, b->high.y);
	n->low.x = Min(a->low.x, b->low.x);
	n->low.y = Min(a->low.y, b->low.y);

	return (n);
}

BOX		   *
rt_box_inter(BOX *a, BOX *b)
{
	BOX		   *n;

	if ((n = (BOX *) palloc(sizeof(*n))) == (BOX *) NULL)
		elog(ERROR, "Cannot allocate box for union");

	n->high.x = Min(a->high.x, b->high.x);
	n->high.y = Min(a->high.y, b->high.y);
	n->low.x = Max(a->low.x, b->low.x);
	n->low.y = Max(a->low.y, b->low.y);

	if (n->high.x < n->low.x || n->high.y < n->low.y)
	{
		pfree(n);
		return ((BOX *) NULL);
	}

	return (n);
}

void
rt_box_size(BOX *a, float *size)
{
	if (a == (BOX *) NULL || a->high.x <= a->low.x || a->high.y <= a->low.y)
		*size = 0.0;
	else
		*size = (float) ((a->high.x - a->low.x) * (a->high.y - a->low.y));

	return;
}

/*
 *	rt_bigbox_size() -- Compute a size for big boxes.
 *
 *		In an earlier release of the system, this routine did something
 *		different from rt_box_size.  We now use floats, rather than ints,
 *		as the return type for the size routine, so we no longer need to
 *		have a special return type for big boxes.
 */
void
rt_bigbox_size(BOX *a, float *size)
{
	rt_box_size(a, size);
}

POLYGON    *
rt_poly_union(POLYGON *a, POLYGON *b)
{
	POLYGON    *p;

	p = (POLYGON *) palloc(sizeof(POLYGON));

	if (!PointerIsValid(p))
		elog(ERROR, "Cannot allocate polygon for union");

	MemSet((char *) p, 0, sizeof(POLYGON));		/* zero any holes */
	p->size = sizeof(POLYGON);
	p->npts = 0;
	p->boundbox.high.x = Max(a->boundbox.high.x, b->boundbox.high.x);
	p->boundbox.high.y = Max(a->boundbox.high.y, b->boundbox.high.y);
	p->boundbox.low.x = Min(a->boundbox.low.x, b->boundbox.low.x);
	p->boundbox.low.y = Min(a->boundbox.low.y, b->boundbox.low.y);
	return p;
}

void
rt_poly_size(POLYGON *a, float *size)
{
	double		xdim,
				ydim;

	size = (float *) palloc(sizeof(float));
	if (a == (POLYGON *) NULL ||
		a->boundbox.high.x <= a->boundbox.low.x ||
		a->boundbox.high.y <= a->boundbox.low.y)
		*size = 0.0;
	else
	{
		xdim = (a->boundbox.high.x - a->boundbox.low.x);
		ydim = (a->boundbox.high.y - a->boundbox.low.y);

		*size = (float) (xdim * ydim);
	}

	return;
}

POLYGON    *
rt_poly_inter(POLYGON *a, POLYGON *b)
{
	POLYGON    *p;

	p = (POLYGON *) palloc(sizeof(POLYGON));

	if (!PointerIsValid(p))
		elog(ERROR, "Cannot allocate polygon for intersection");

	MemSet((char *) p, 0, sizeof(POLYGON));		/* zero any holes */
	p->size = sizeof(POLYGON);
	p->npts = 0;
	p->boundbox.high.x = Min(a->boundbox.high.x, b->boundbox.high.x);
	p->boundbox.high.y = Min(a->boundbox.high.y, b->boundbox.high.y);
	p->boundbox.low.x = Max(a->boundbox.low.x, b->boundbox.low.x);
	p->boundbox.low.y = Max(a->boundbox.low.y, b->boundbox.low.y);

	if (p->boundbox.high.x < p->boundbox.low.x || p->boundbox.high.y < p->boundbox.low.y)
	{
		pfree(p);
		return ((POLYGON *) NULL);
	}

	return (p);
}
