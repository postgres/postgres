/*-------------------------------------------------------------------------
 *
 * char.c--
 *	  Functions for the built-in type "char".
 *	  Functions for the built-in type "cid".
 *	  Functions for the built-in type "char2".
 *	  Functions for the built-in type "char4".
 *	  Functions for the built-in type "char8".
 *	  Functions for the built-in type "char16".
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/char.c,v 1.14 1997/11/02 15:25:57 vadim Exp $
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
		return ((int32) NULL);
	return ((int32) *ch);
}

/*
 *		charout			- converts 'x' to "x"
 */
char	   *
charout(int32 ch)
{
	char	   *result = (char *) palloc(2);

	result[0] = (char) ch;
	result[1] = '\0';
	return (result);
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

	return ((int32) c);
}

/*
 *		cidout	- converts a cid to "..."
 *
 *		NOTE: we must no use 'charout' because cid might be a non
 *		printable character...
 */
char	   *
cidout(int32 c)
{
	char	   *result;
	CommandId	c2;

	result = palloc(12);
	c2 = (CommandId) c;
	sprintf(result, "%u", (unsigned) (c2));
	return (result);
}

/*
 *		char16in		- converts "..." to internal reprsentation
 *
 *		Note:
 *				Currently if strlen(s) < 14, the extra chars are nulls
 */
char	   *
char16in(char *s)
{
	char	   *result;

	if (s == NULL)
		return (NULL);
	result = (char *) palloc(16);
	strncpy(result, s, 16);
	return (result);
}

/*
 *		char16out		- converts internal reprsentation to "..."
 */
char	   *
char16out(char *s)
{
	char	   *result = (char *) palloc(17);

	if (s == NULL)
	{
		result[0] = '-';
		result[1] = '\0';
	}
	else
		StrNCpy(result, s, 17);
	return (result);
}


/*****************************************************************************
 *	 PUBLIC ROUTINES														 *
 *****************************************************************************/

bool
chareq(int8 arg1, int8 arg2)
{
	return (arg1 == arg2);
}

bool
charne(int8 arg1, int8 arg2)
{
	return (arg1 != arg2);
}

bool
charlt(int8 arg1, int8 arg2)
{
	return ((uint8) arg1 < (uint8) arg2);
}

bool
charle(int8 arg1, int8 arg2)
{
	return ((uint8) arg1 <= (uint8) arg2);
}

bool
chargt(int8 arg1, int8 arg2)
{
	return ((uint8) arg1 > (uint8) arg2);
}

bool
charge(int8 arg1, int8 arg2)
{
	return ((uint8) arg1 >= (uint8) arg2);
}

int8
charpl(int8 arg1, int8 arg2)
{
	return (arg1 + arg2);
}

int8
charmi(int8 arg1, int8 arg2)
{
	return (arg1 - arg2);
}

int8
charmul(int8 arg1, int8 arg2)
{
	return (arg1 * arg2);
}

int8
chardiv(int8 arg1, int8 arg2)
{
	return (arg1 / arg2);
}

bool
cideq(int8 arg1, int8 arg2)
{
	return (arg1 == arg2);
}

/*
 *		char16eq		- returns 1 iff arguments are equal
 *		char16ne		- returns 1 iff arguments are not equal
 *
 *		BUGS:
 *				Assumes that "xy\0\0a" should be equal to "xy\0b".
 *				If not, can do the comparison backwards for efficiency.
 *
 *		char16lt		- returns 1 iff a < b
 *		char16le		- returns 1 iff a <= b
 *		char16gt		- returns 1 iff a < b
 *		char16ge		- returns 1 iff a <= b
 *
 */
bool
char16eq(char *arg1, char *arg2)
{
	if (arg1 == NULL || arg2 == NULL)
		return ((bool) 0);
	return (strncmp(arg1, arg2, 16) == 0);
}

bool
char16ne(char *arg1, char *arg2)
{
	if (arg1 == NULL || arg2 == NULL)
		return ((bool) 0);
	return (strncmp(arg1, arg2, 16) != 0);
}

bool
char16lt(char *arg1, char *arg2)
{
	if (arg1 == NULL || arg2 == NULL)
		return ((bool) 0);
	return (strncmp(arg1, arg2, 16) < 0);
}

bool
char16le(char *arg1, char *arg2)
{
	if (arg1 == NULL || arg2 == NULL)
		return ((bool) 0);
	return (strncmp(arg1, arg2, 16) <= 0);
}

bool
char16gt(char *arg1, char *arg2)
{
	if (arg1 == NULL || arg2 == NULL)
		return ((bool) 0);

	return (strncmp(arg1, arg2, 16) > 0);
}

bool
char16ge(char *arg1, char *arg2)
{
	if (arg1 == NULL || arg2 == NULL)
		return ((bool) 0);

	return (strncmp(arg1, arg2, 16) >= 0);
}


/* ============================== char2 ============================== */
uint16
char2in(char *s)
{
	uint16		res;

	if (s == NULL)
		return (0);

	strncpy((char *) &res, s, 2);
	return (res);
}

char	   *
char2out(uint16 s)
{
	char	   *result = (char *) palloc(3);

	StrNCpy(result, (char *) &s, 3);

	return (result);
}

bool
char2eq(uint16 a, uint16 b)
{
	return (strncmp((char *) &a, (char *) &b, 2) == 0);
}

bool
char2ne(uint16 a, uint16 b)
{
	return (strncmp((char *) &a, (char *) &b, 2) != 0);
}

bool
char2lt(uint16 a, uint16 b)
{
	return (strncmp((char *) &a, (char *) &b, 2) < 0);
}

bool
char2le(uint16 a, uint16 b)
{
	return (strncmp((char *) &a, (char *) &b, 2) <= 0);
}

bool
char2gt(uint16 a, uint16 b)
{
	return (strncmp((char *) &a, (char *) &b, 2) > 0);
}

bool
char2ge(uint16 a, uint16 b)
{
	return (strncmp((char *) &a, (char *) &b, 2) >= 0);
}

int32
char2cmp(uint16 a, uint16 b)
{
	return (strncmp((char *) &a, (char *) &b, 2));
}

/* ============================== char4 ============================== */
uint32
char4in(char *s)
{
	uint32		res;

	if (s == NULL)
		return (0);

	strncpy((char *) &res, s, 4);

	return (res);
}

char	   *
char4out(s)
uint32		s;
{
	char	   *result = (char *) palloc(5);

	StrNCpy(result, (char *) &s, 5);

	return (result);
}

bool
char4eq(uint32 a, uint32 b)
{
	return (strncmp((char *) &a, (char *) &b, 4) == 0);
}

bool
char4ne(uint32 a, uint32 b)
{
	return (strncmp((char *) &a, (char *) &b, 4) != 0);
}

bool
char4lt(uint32 a, uint32 b)
{
	return (strncmp((char *) &a, (char *) &b, 4) < 0);
}

bool
char4le(uint32 a, uint32 b)
{
	return (strncmp((char *) &a, (char *) &b, 4) <= 0);
}

bool
char4gt(uint32 a, uint32 b)
{
	return (strncmp((char *) &a, (char *) &b, 4) > 0);
}

bool
char4ge(uint32 a, uint32 b)
{
	return (strncmp((char *) &a, (char *) &b, 4) >= 0);
}

int32
char4cmp(uint32 a, uint32 b)
{
	return (strncmp((char *) &a, (char *) &b, 4));
}

/* ============================== char8 ============================== */
char	   *
char8in(char *s)
{
	char	   *result;

	if (s == NULL)
		return ((char *) NULL);

	result = (char *) palloc(8);
	strncpy(result, s, 8);
	return (result);
}

char	   *
char8out(char *s)
{
	char	   *result = (char *) palloc(9);

	if (s == NULL)
	{
		result[0] = '-';
		result[1] = '\0';
	}
	else
		StrNCpy(result, s, 9);
	return (result);
}

bool
char8eq(char *arg1, char *arg2)
{
	if (arg1 == NULL || arg2 == NULL)
		return ((bool) 0);
	return (strncmp(arg1, arg2, 8) == 0);
}

bool
char8ne(char *arg1, char *arg2)
{
	if (arg1 == NULL || arg2 == NULL)
		return ((bool) 0);
	return (strncmp(arg1, arg2, 8) != 0);
}

bool
char8lt(char *arg1, char *arg2)
{
	if (arg1 == NULL || arg2 == NULL)
		return ((bool) 0);
	return (strncmp(arg1, arg2, 8) < 0);
}

bool
char8le(char *arg1, char *arg2)
{
	if (arg1 == NULL || arg2 == NULL)
		return ((bool) 0);
	return (strncmp(arg1, arg2, 8) <= 0);
}

bool
char8gt(char *arg1, char *arg2)
{
	if (arg1 == NULL || arg2 == NULL)
		return ((bool) 0);
	return (strncmp(arg1, arg2, 8) > 0);
}

bool
char8ge(char *arg1, char *arg2)
{
	if (arg1 == NULL || arg2 == NULL)
		return ((bool) 0);
	return (strncmp(arg1, arg2, 8) >= 0);
}

int32
char8cmp(char *arg1, char *arg2)
{
	return (strncmp(arg1, arg2, 8));
}
