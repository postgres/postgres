/*-------------------------------------------------------------------------
 *
 * bool.c--
 *	  Functions for the built-in type "bool".
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/bool.c,v 1.8 1997/10/17 05:38:32 thomas Exp $
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
 * Check explicitly for "true/false" and TRUE/FALSE, 1/0, YES/NO.
 * Reject other values. - thomas 1997-10-05
 * For now, allow old behavior of everything FALSE if not TRUE.
 * After v6.2.1 release, then enable code to reject goofy values.
 * Also, start checking the entire string rather than just the first character.
 * - thomas 1997-10-16
 *
 * In the switch statement, check the most-used possibilities first.
 */
bool
boolin(char *b)
{
	switch(*b) {
		case 't':
		case 'T':
			return (TRUE);
			break;

		case 'f':
		case 'F':
			return (FALSE);
			break;

		case 'y':
		case 'Y':
		case '1':
			return (TRUE);
			break;

		case 'n':
		case 'N':
		case '0':
			return (FALSE);
			break;

		default:
#if FALSE
			elog(WARN,"Invalid input string '%s'\n", b);
#endif
			break;
	}

	return (FALSE);
} /* boolin() */

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
} /* boolout() */

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
