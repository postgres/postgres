/*-------------------------------------------------------------------------
 *
 * rtproc.c--
 *    pg_amproc entries for rtrees.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/access/rtree/Attic/rtproc.c,v 1.4 1996/11/05 10:54:17 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <utils/geo-decls.h>
#ifndef HAVE_MEMMOVE
# include <regex/utils.h>
#else
# include <string.h>
#endif

BOX
*rt_box_union(BOX *a, BOX *b)
{
    BOX *n;
    
    if ((n = (BOX *) palloc(sizeof (*n))) == (BOX *) NULL)
	elog(WARN, "Cannot allocate box for union");
    
    n->xh = Max(a->xh, b->xh);
    n->yh = Max(a->yh, b->yh);
    n->xl = Min(a->xl, b->xl);
    n->yl = Min(a->yl, b->yl);
    
    return (n);
}

BOX *
rt_box_inter(BOX *a, BOX *b)
{
    BOX *n;
    
    if ((n = (BOX *) palloc(sizeof (*n))) == (BOX *) NULL)
	elog(WARN, "Cannot allocate box for union");
    
    n->xh = Min(a->xh, b->xh);
    n->yh = Min(a->yh, b->yh);
    n->xl = Max(a->xl, b->xl);
    n->yl = Max(a->yl, b->yl);
    
    if (n->xh < n->xl || n->yh < n->yl) {
	pfree(n);
	return ((BOX *) NULL);
    }
    
    return (n);
}

void
rt_box_size(BOX *a, float *size)
{
    if (a == (BOX *) NULL || a->xh <= a->xl || a->yh <= a->yl)
	*size = 0.0;
    else
	*size = (float) ((a->xh - a->xl) * (a->yh - a->yl));
    
    return;
}

/*
 *  rt_bigbox_size() -- Compute a size for big boxes.
 *
 *	In an earlier release of the system, this routine did something
 *	different from rt_box_size.  We now use floats, rather than ints,
 *	as the return type for the size routine, so we no longer need to
 *	have a special return type for big boxes.
 */
void
rt_bigbox_size(BOX *a, float *size)
{
    rt_box_size(a, size);
}

POLYGON *
rt_poly_union(POLYGON *a, POLYGON *b)
{
    POLYGON *p;
    
    p = (POLYGON *)PALLOCTYPE(POLYGON);
    
    if (!PointerIsValid(p))
	elog(WARN, "Cannot allocate polygon for union");
    
    memset((char *) p, 0, sizeof(POLYGON));	/* zero any holes */
    p->size = sizeof(POLYGON);
    p->npts = 0;
    p->boundbox.xh = Max(a->boundbox.xh, b->boundbox.xh);
    p->boundbox.yh = Max(a->boundbox.yh, b->boundbox.yh);
    p->boundbox.xl = Min(a->boundbox.xl, b->boundbox.xl);
    p->boundbox.yl = Min(a->boundbox.yl, b->boundbox.yl);
    return p;
}

void
rt_poly_size(POLYGON *a, float *size)
{
    double xdim, ydim;
    
    size = (float *) palloc(sizeof(float));
    if (a == (POLYGON *) NULL || 
	a->boundbox.xh <= a->boundbox.xl || 
	a->boundbox.yh <= a->boundbox.yl)
	*size = 0.0;
    else {
	xdim = (a->boundbox.xh - a->boundbox.xl);
	ydim = (a->boundbox.yh - a->boundbox.yl);
	
	*size = (float) (xdim * ydim);
    }
    
    return;
}

POLYGON *
rt_poly_inter(POLYGON *a, POLYGON *b)
{
    POLYGON *p;
    
    p = (POLYGON *) PALLOCTYPE(POLYGON);
    
    if (!PointerIsValid(p))
	elog(WARN, "Cannot allocate polygon for intersection");
    
    memset((char *) p, 0, sizeof(POLYGON));	/* zero any holes */
    p->size = sizeof(POLYGON);
    p->npts = 0;
    p->boundbox.xh = Min(a->boundbox.xh, b->boundbox.xh);
    p->boundbox.yh = Min(a->boundbox.yh, b->boundbox.yh);
    p->boundbox.xl = Max(a->boundbox.xl, b->boundbox.xl);
    p->boundbox.yl = Max(a->boundbox.yl, b->boundbox.yl);
    
    if (p->boundbox.xh < p->boundbox.xl || p->boundbox.yh < p->boundbox.yl)
	{
	    pfree(p);
	    return ((POLYGON *) NULL);
	}
    
    return (p);
}
