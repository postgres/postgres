/*-------------------------------------------------------------------------
 *
 * oidint4.c--
 *    Functions for the built-in type "oidint4".
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/utils/adt/Attic/oidint4.c,v 1.1.1.1 1996/07/09 06:22:05 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>		/* for sprintf() */
#include "postgres.h"
#include "utils/palloc.h"
#include "utils/builtins.h"
#include "utils/oidcompos.h"	/* where function declarations go */

OidInt4 oidint4in(char *o)
{
    OidInt4 oi;
    char *p;
    
    oi = (OidInt4) palloc(sizeof(OidInt4Data));
    
    for (p = o; *p != '\0' && *p != '/'; p++)
	continue;
    
    oi->oi_oid = (Oid) pg_atoi(o, sizeof(Oid), '/');
    if (*p == '\0') {
	oi->oi_int4 = 0;
    } else {
	oi->oi_int4 = pg_atoi(++p, sizeof(int4), '\0');
    }
    
    return (oi);
}

char *oidint4out(OidInt4 o)
{
    char *r;
    
    /*
     * -2147483647/-2147483647 
     * 0        1         2
     * 123456789012345678901234
     */
    r = (char *) palloc(24);
    sprintf(r, "%d/%d", o->oi_oid, o->oi_int4);
    
    return (r);
}

bool oidint4lt(OidInt4 o1, OidInt4 o2)
{
    return
	((bool) (o1->oi_oid < o2->oi_oid ||
		 (o1->oi_oid == o2->oi_oid && o1->oi_int4 < o2->oi_int4)));
}

bool oidint4le(OidInt4 o1, OidInt4 o2)
{
    return ((bool) (o1->oi_oid < o2->oi_oid ||
		    (o1->oi_oid == o2->oi_oid && o1->oi_int4 <= o2->oi_int4)));
}

bool oidint4eq(OidInt4 o1, OidInt4 o2)
{
    return ((bool) (o1->oi_oid == o2->oi_oid && o1->oi_int4 == o2->oi_int4));
}

bool oidint4ge(OidInt4 o1, OidInt4 o2)
{
    return ((bool) (o1->oi_oid > o2->oi_oid ||
		    (o1->oi_oid == o2->oi_oid && o1->oi_int4 >= o2->oi_int4)));
}

bool oidint4gt(OidInt4 o1, OidInt4 o2)
{
    return ((bool) (o1->oi_oid > o2->oi_oid ||
		    (o1->oi_oid == o2->oi_oid && o1->oi_int4 > o2->oi_int4)));
}

bool oidint4ne(OidInt4 o1, OidInt4 o2)
{
    return ((bool) (o1->oi_oid != o2->oi_oid || o1->oi_int4 != o2->oi_int4));
}

int oidint4cmp(OidInt4 o1, OidInt4 o2)
{
    if (oidint4lt(o1, o2))
	return (-1);
    else if (oidint4eq(o1, o2))
	return (0);
    else
	return (1);
}

OidInt4 mkoidint4(Oid v_oid, uint32 v_int4)
{
    OidInt4 o;
    
    o = (OidInt4) palloc(sizeof(OidInt4Data));
    o->oi_oid = v_oid;
    o->oi_int4 = v_int4;
    return (o);
}



