/*-------------------------------------------------------------------------
 *
 * oid.c--
 *    Functions for the built-in type Oid.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/utils/adt/oid.c,v 1.1.1.1 1996/07/09 06:22:05 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "utils/palloc.h"
#include "utils/builtins.h"	/* where function declarations go */
#include "utils/elog.h"

/***************************************************************************** 
 *   USER I/O ROUTINES                                                       *
 *****************************************************************************/

/*
 *	oid8in		- converts "num num ..." to internal form
 *
 *	Note:
 *		Fills any nonexistent digits with NULL oids.
 */
Oid *oid8in(char *oidString)
{
    register Oid	(*result)[];
    int			nums;
    
    if (oidString == NULL)
	return(NULL);
    result = (Oid (*)[]) palloc(sizeof(Oid [8]));
    if ((nums = sscanf(oidString, "%d%d%d%d%d%d%d%d",
		       *result,
		       *result + 1,
		       *result + 2,
		       *result + 3,
		       *result + 4,
		       *result + 5,
		       *result + 6,
		       *result + 7)) != 8) {
	do
	    (*result)[nums++] = 0;
	while (nums < 8);
    }
    return((Oid *) result);
}

/*
 *	oid8out	- converts internal form to "num num ..."
 */
char *oid8out(Oid	(*oidArray)[])
{
    register int		num;
    register Oid	*sp;
    register char		*rp;
    char			*result;
    
    if (oidArray == NULL) {
	result = (char *) palloc(2);
	result[0] = '-';
	result[1] = '\0';
	return(result);
    }
    
    /* assumes sign, 10 digits, ' ' */
    rp = result = (char *) palloc(8 * 12);
    sp = *oidArray;
    for (num = 8; num != 0; num--) {
	ltoa(*sp++, rp);
	while (*++rp != '\0')
	    ;
	*rp++ = ' ';
    }
    *--rp = '\0';
    return(result);
}

Oid oidin(char *s)
{
    extern int32 int4in();
    
    return(int4in(s));
}

char *oidout(Oid o)
{
    extern char *int4out();
    
    return(int4out(o));
}

/***************************************************************************** 
 *   PUBLIC ROUTINES                                                         *
 *****************************************************************************/

int32 oideq(Oid arg1, Oid arg2)
{
    return(arg1 == arg2);
}

int32 oidne(Oid arg1, Oid arg2)
{
    return(arg1 != arg2);
}

int32 oid8eq(Oid arg1[], Oid arg2[])
{
    return (int32)(memcmp(arg1, arg2, 8 * sizeof(Oid)) == 0);
}

bool oideqint4(Oid arg1, int32 arg2)
{
/* oid is unsigned, but int4 is signed */
    return (arg2 >= 0 && arg1 == arg2);
}

bool int4eqoid(int32 arg1, Oid arg2)
{
/* oid is unsigned, but int4 is signed */
    return (arg1 >= 0 && arg1 == arg2);
}

