/*-------------------------------------------------------------------------
 *
 * char.c
 *	  Functions for the built-in type "char".
 *	  Functions for the built-in type "cid".
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/char.c,v 1.21 1999/02/13 23:19:05 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>				/* for sprintf() */
#include <string.h>
#include "postgres.h"
#include "utils/palloc.h"
#include "utils/builtins.h"		/* where the declarations go */

/*****************************************************************************
 *	 USER I/O ROUTINES														 *
 *****************************************************************************/

/*
 *		charin			- converts "x" to 'x'
 */
int32
charin(char *ch)
{
	if (ch == NULL)
		return (int32) '\0';
	return (int32) *ch;
}

/*
 *		charout			- converts 'x' to "x"
 */
char *
charout(int32 ch)
{
	char	   *result = (char *) palloc(2);

	result[0] = (char) ch;
	result[1] = '\0';
	return result;
}

/*
 *		cidin	- converts "..." to internal representation.
 *
 *		NOTE: we must not use 'charin' because cid might be a non
 *		printable character...
 */
int32
cidin(char *s)
{
	CommandId	c;

	if (s == NULL)
		c = 0;
	else
		c = atoi(s);

	return (int32) c;
}

/*
 *		cidout	- converts a cid to "..."
 *
 *		NOTE: we must no use 'charout' because cid might be a non
 *		printable character...
 */
char *
cidout(int32 c)
{
	char	   *result;
	CommandId	c2;

	result = palloc(12);
	c2 = (CommandId) c;
	sprintf(result, "%u", (unsigned) (c2));
	return result;
}


/*****************************************************************************
 *	 PUBLIC ROUTINES														 *
 *****************************************************************************/

bool
chareq(int8 arg1, int8 arg2)
{
	return arg1 == arg2;
}

bool
charne(int8 arg1, int8 arg2)
{
	return arg1 != arg2;
}

bool
charlt(int8 arg1, int8 arg2)
{
	return (uint8) arg1 < (uint8) arg2;
}

bool
charle(int8 arg1, int8 arg2)
{
	return (uint8) arg1 <= (uint8) arg2;
}

bool
chargt(int8 arg1, int8 arg2)
{
	return (uint8) arg1 > (uint8) arg2;
}

bool
charge(int8 arg1, int8 arg2)
{
	return (uint8) arg1 >= (uint8) arg2;
}

int8
charpl(int8 arg1, int8 arg2)
{
	return arg1 + arg2;
}

int8
charmi(int8 arg1, int8 arg2)
{
	return arg1 - arg2;
}

int8
charmul(int8 arg1, int8 arg2)
{
	return arg1 * arg2;
}

int8
chardiv(int8 arg1, int8 arg2)
{
	return arg1 / arg2;
}

bool
cideq(int8 arg1, int8 arg2)
{
	return arg1 == arg2;
}

int8
text_char(text *arg1)
{
	return ((int8) *(VARDATA(arg1)));
}

text *
char_text(int8 arg1)
{
	text *result;

	result = palloc(VARHDRSZ+1);
	VARSIZE(result) = VARHDRSZ+1;
	*(VARDATA(result)) = arg1;

	return result;
}
