/*-------------------------------------------------------------------------
 *
 * oidint2.c--
 *	  Functions for the built-in type "oidint2".
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/Attic/oidint2.c,v 1.3 1997/09/08 02:30:54 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include "postgres.h"
#include "utils/palloc.h"
#include "utils/builtins.h"		/* for pg_atoi() */
#include "utils/oidcompos.h"	/* where function declarations go */


OidInt2
oidint2in(char *o)
{
	OidInt2		oi;
	char	   *p;

	oi = (OidInt2) palloc(sizeof(OidInt2Data));

	for (p = o; *p != '\0' && *p != '/'; p++)
		continue;

	oi->oi_oid = (Oid) pg_atoi(o, sizeof(Oid), '/');
	if (*p == '\0')
	{
		oi->oi_int2 = 0;
	}
	else
	{
		oi->oi_int2 = (int16) pg_atoi(++p, sizeof(int2), '\0');
	}

	return (oi);
}

char	   *
oidint2out(OidInt2 o)
{
	char	   *r;

	/*
	 * -2147483647/-32767 0		   1 1234567890123456789
	 */
	r = (char *) palloc(19);
	sprintf(r, "%d/%d", o->oi_oid, o->oi_int2);

	return (r);
}

bool
oidint2lt(OidInt2 o1, OidInt2 o2)
{
	return
		((bool) (o1->oi_oid < o2->oi_oid ||
			   (o1->oi_oid == o2->oi_oid && o1->oi_int2 < o2->oi_int2)));
}

bool
oidint2le(OidInt2 o1, OidInt2 o2)
{
	return ((bool) (o1->oi_oid < o2->oi_oid ||
			  (o1->oi_oid == o2->oi_oid && o1->oi_int2 <= o2->oi_int2)));
}

bool
oidint2eq(OidInt2 o1, OidInt2 o2)
{
	return ((bool) (o1->oi_oid == o2->oi_oid && o1->oi_int2 == o2->oi_int2));
}

bool
oidint2ge(OidInt2 o1, OidInt2 o2)
{
	return ((bool) (o1->oi_oid > o2->oi_oid ||
			  (o1->oi_oid == o2->oi_oid && o1->oi_int2 >= o2->oi_int2)));
}

bool
oidint2gt(OidInt2 o1, OidInt2 o2)
{
	return ((bool) (o1->oi_oid > o2->oi_oid ||
			   (o1->oi_oid == o2->oi_oid && o1->oi_int2 > o2->oi_int2)));
}

bool
oidint2ne(OidInt2 o1, OidInt2 o2)
{
	return ((bool) (o1->oi_oid != o2->oi_oid || o1->oi_int2 != o2->oi_int2));
}

int
oidint2cmp(OidInt2 o1, OidInt2 o2)
{
	if (oidint2lt(o1, o2))
		return (-1);
	else if (oidint2eq(o1, o2))
		return (0);
	else
		return (1);
}

OidInt2
mkoidint2(Oid v_oid, uint16 v_int2)
{
	OidInt2		o;

	o = (OidInt2) palloc(sizeof(OidInt2Data));
	o->oi_oid = v_oid;
	o->oi_int2 = v_int2;
	return (o);
}
