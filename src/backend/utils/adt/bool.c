/*-------------------------------------------------------------------------
 *
 * bool.c--
 *	  Functions for the built-in type "bool".
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/bool.c,v 1.7 1997/10/09 05:06:12 thomas Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/builtins.h"		/* where the declarations go */
#include "utils/palloc.h"

/*****************************************************************************
 *	 USER I/O ROUTINES														 *
 *****************************************************************************/

/*
 *		boolin			- converts "t" or "f" to 1 or 0
 *
 * Check explicitly for "true/TRUE" and allow any odd ASCII value to be "true".
 * This handles "true/false", "yes/no", "1/0". - thomas 1997-10-05
 */
bool
boolin(char *b)
{
	if (b == NULL)
		elog(WARN, "Bad input string for type bool");
	return ((bool) (((*b) == 't') || ((*b) == 'T') || ((*b) & 1)));
}

/*
 *		boolout			- converts 1 or 0 to "t" or "f"
 */
char *
boolout(bool b)
{
	char	   *result = (char *) palloc(2);

	*result = (b) ? 't' : 'f';
	result[1] = '\0';
	return (result);
}

/*****************************************************************************
 *	 PUBLIC ROUTINES														 *
 *****************************************************************************/

bool
booleq(bool arg1, bool arg2)
{
	return (arg1 == arg2);
}

bool
boolne(bool arg1, bool arg2)
{
	return (arg1 != arg2);
}

bool
boollt(bool arg1, bool arg2)
{
	return (arg1 < arg2);
}

bool
boolgt(bool arg1, bool arg2)
{
	return (arg1 > arg2);
}
