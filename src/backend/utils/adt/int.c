/*-------------------------------------------------------------------------
 *
 * int.c--
 *	  Functions for the built-in integer types.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/int.c,v 1.17 1998/09/01 04:32:35 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * OLD COMMENTS
 *		I/O routines:
 *		 int2in, int2out, int28in, int28out, int4in, int4out
 *		Conversion routines:
 *		 itoi, int2_text, int4_text
 *		Boolean operators:
 *		 inteq, intne, intlt, intle, intgt, intge
 *		Arithmetic operators:
 *		 intpl, intmi, int4mul, intdiv
 *
 *		Arithmetic operators:
 *		 intmod, int4fac
 *
 * XXX makes massive and possibly unwarranted type promotion assumptions.
 * fix me when we figure out what we want to do about ANSIfication...
 */
#include <stdio.h>
#include <string.h>
#ifdef HAVE_LIMITS
#include <limits.h>
#endif

#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"		/* where the declarations go */

#ifndef SHRT_MAX
#define SHRT_MAX (0x7FFF)
#endif
#ifndef SHRT_MIN
#define SHRT_MIN (-0x8000)
#endif

/*****************************************************************************
 *	 USER I/O ROUTINES														 *
 *****************************************************************************/

/*
 *		int2in			- converts "num" to short
 */
int32
int2in(char *num)
{
	return (int32) pg_atoi(num, sizeof(int16), '\0');
}

/*
 *		int2out			- converts short to "num"
 */
char *
int2out(int16 sh)
{
	char	   *result;

	result = (char *) palloc(7);/* assumes sign, 5 digits, '\0' */
	itoa((int) sh, result);
	return result;
}

/*
 *		int28in			- converts "num num ..." to internal form
 *
 *		Note:
 *				Fills any nonexistent digits with NULLs.
 */
int16 *
int28in(char *shs)
{
	int16	  **result;
	int			nums;

	if (shs == NULL)
		return NULL;
	result = (int16 **) palloc(sizeof(int16[8]));
	if ((nums = sscanf(shs, "%hd%hd%hd%hd%hd%hd%hd%hd",
					   *result,
					   *result + 1,
					   *result + 2,
					   *result + 3,
					   *result + 4,
					   *result + 5,
					   *result + 6,
					   *result + 7)) != 8)
	{
		do
			(*result)[nums++] = 0;
		while (nums < 8);
	}
	return (int16 *) result;
}

/*
 *		int28out		- converts internal form to "num num ..."
 */
char *
int28out(int16 **shs)
{
	int			num;
	int16	   *sp;
	char	   *rp;
	char	   *result;

	if (shs == NULL)
	{
		result = (char *) palloc(2);
		result[0] = '-';
		result[1] = '\0';
		return result;
	}
	rp = result = (char *) palloc(8 * 7);		/* assumes sign, 5 digits,
												 * ' ' */
	sp = *shs;
	for (num = 8; num != 0; num--)
	{
		itoa(*sp++, rp);
		while (*++rp != '\0')
			;
		*rp++ = ' ';
	}
	*--rp = '\0';
	return result;
}

/*
 *		int28in			- converts "num num ..." to internal form
 *
 *		Note:
 *				Fills any nonexistent digits with NULLs.
 */
int32 *
int44in(char *input_string)
{
	int32	   *foo = (int32 *) palloc(4 * sizeof(int32));
	int			i = 0;

	i = sscanf(input_string,
			   "%d, %d, %d, %d",
			   &foo[0],
			   &foo[1],
			   &foo[2],
			   &foo[3]);
	while (i < 4)
		foo[i++] = 0;

	return foo;
}

/*
 *		int28out		- converts internal form to "num num ..."
 */
char *
int44out(int32 *an_array)
{
	int			temp = 4;
	char	   *output_string = NULL;
	int			i;

	if (temp > 0)
	{
		char	   *walk;

		output_string = (char *) palloc(16 * temp);		/* assume 15 digits +
														 * sign */
		walk = output_string;
		for (i = 0; i < temp; i++)
		{
			itoa(an_array[i], walk);
			while (*++walk != '\0')
				;
			*walk++ = ' ';
		}
		*--walk = '\0';
	}
	return output_string;
}


/*****************************************************************************
 *	 PUBLIC ROUTINES														 *
 *****************************************************************************/

/*
 *		int4in			- converts "num" to int4
 */
int32
int4in(char *num)
{
	return pg_atoi(num, sizeof(int32), '\0');
}

/*
 *		int4out			- converts int4 to "num"
 */
char *
int4out(int32 l)
{
	char	   *result;

	result = (char *) palloc(12);		/* assumes sign, 10 digits, '\0' */
	ltoa(l, result);
	return result;
}


/*
 *		===================
 *		CONVERSION ROUTINES
 *		===================
 */

int32
i2toi4(int16 arg1)
{
	return (int32) arg1;
}

int16
i4toi2(int32 arg1)
{
	if (arg1 < SHRT_MIN)
		elog(ERROR, "i4toi2: '%d' causes int2 underflow", arg1);
	if (arg1 > SHRT_MAX)
		elog(ERROR, "i4toi2: '%d' causes int2 overflow", arg1);

	return (int16) arg1;
}

text *
int2_text(int16 arg1)
{
	text	   *result;

	int			len;
	char	   *str;

	str = int2out(arg1);
	len = (strlen(str) + VARHDRSZ);

	result = palloc(len);

	VARSIZE(result) = len;
	memmove(VARDATA(result), str, (len - VARHDRSZ));

	pfree(str);

	return result;
}	/* int2_text() */

int16
text_int2(text *string)
{
	int16		result;

	int			len;
	char	   *str;

	len = (VARSIZE(string) - VARHDRSZ);

	str = palloc(len + 1);
	memmove(str, VARDATA(string), len);
	*(str + len) = '\0';

	result = int2in(str);
	pfree(str);

	return result;
}	/* text_int2() */

text *
int4_text(int32 arg1)
{
	text	   *result;

	int			len;
	char	   *str;

	str = int4out(arg1);
	len = (strlen(str) + VARHDRSZ);

	result = palloc(len);

	VARSIZE(result) = len;
	memmove(VARDATA(result), str, (len - VARHDRSZ));

	pfree(str);

	return result;
}	/* int4_text() */

int32
text_int4(text *string)
{
	int32		result;

	int			len;
	char	   *str;

	len = (VARSIZE(string) - VARHDRSZ);

	str = palloc(len + 1);
	memmove(str, VARDATA(string), len);
	*(str + len) = '\0';

	result = int4in(str);
	pfree(str);

	return result;
}	/* text_int4() */


/*
 *		=========================
 *		BOOLEAN OPERATOR ROUTINES
 *		=========================
 */

/*
 *		inteq			- returns 1 iff arg1 == arg2
 *		intne			- returns 1 iff arg1 != arg2
 *		intlt			- returns 1 iff arg1 < arg2
 *		intle			- returns 1 iff arg1 <= arg2
 *		intgt			- returns 1 iff arg1 > arg2
 *		intge			- returns 1 iff arg1 >= arg2
 */
bool
int4eq(int32 arg1, int32 arg2)
{
	return arg1 == arg2;
}

bool
int4ne(int32 arg1, int32 arg2)
{
	return arg1 != arg2;
}

bool
int4lt(int32 arg1, int32 arg2)
{
	return arg1 < arg2;
}

bool
int4le(int32 arg1, int32 arg2)
{
	return arg1 <= arg2;
}

bool
int4gt(int32 arg1, int32 arg2)
{
	return arg1 > arg2;
}

bool
int4ge(int32 arg1, int32 arg2)
{
	return arg1 >= arg2;
}

bool
int2eq(int16 arg1, int16 arg2)
{
	return arg1 == arg2;
}

bool
int2ne(int16 arg1, int16 arg2)
{
	return arg1 != arg2;
}

bool
int2lt(int16 arg1, int16 arg2)
{
	return arg1 < arg2;
}

bool
int2le(int16 arg1, int16 arg2)
{
	return arg1 <= arg2;
}

bool
int2gt(int16 arg1, int16 arg2)
{
	return arg1 > arg2;
}

bool
int2ge(int16 arg1, int16 arg2)
{
	return arg1 >= arg2;
}

bool
int24eq(int32 arg1, int32 arg2)
{
	return arg1 == arg2;
}

bool
int24ne(int32 arg1, int32 arg2)
{
	return arg1 != arg2;
}

bool
int24lt(int32 arg1, int32 arg2)
{
	return arg1 < arg2;
}

bool
int24le(int32 arg1, int32 arg2)
{
	return arg1 <= arg2;
}

bool
int24gt(int32 arg1, int32 arg2)
{
	return arg1 > arg2;
}

bool
int24ge(int32 arg1, int32 arg2)
{
	return arg1 >= arg2;
}

bool
int42eq(int32 arg1, int32 arg2)
{
	return arg1 == arg2;
}

bool
int42ne(int32 arg1, int32 arg2)
{
	return arg1 != arg2;
}

bool
int42lt(int32 arg1, int32 arg2)
{
	return arg1 < arg2;
}

bool
int42le(int32 arg1, int32 arg2)
{
	return arg1 <= arg2;
}

bool
int42gt(int32 arg1, int32 arg2)
{
	return arg1 > arg2;
}

bool
int42ge(int32 arg1, int32 arg2)
{
	return arg1 >= arg2;
}


bool
keyfirsteq(int16 *arg1, int16 arg2)
{
	return *arg1 == arg2;
}

/*
 *		int[24]pl		- returns arg1 + arg2
 *		int[24]mi		- returns arg1 - arg2
 *		int[24]mul		- returns arg1 * arg2
 *		int[24]div		- returns arg1 / arg2
 */
int32
int4um(int32 arg)
{
	return -arg;
}

int32
int4pl(int32 arg1, int32 arg2)
{
	return arg1 + arg2;
}

int32
int4mi(int32 arg1, int32 arg2)
{
	return arg1 - arg2;
}

int32
int4mul(int32 arg1, int32 arg2)
{
	return arg1 * arg2;
}

int32
int4div(int32 arg1, int32 arg2)
{
	return arg1 / arg2;
}

int32
int4inc(int32 arg)
{
	return arg + (int32) 1;
}

int16
int2um(int16 arg)
{
	return -arg;
}

int16
int2pl(int16 arg1, int16 arg2)
{
	return arg1 + arg2;
}

int16
int2mi(int16 arg1, int16 arg2)
{
	return arg1 - arg2;
}

int16
int2mul(int16 arg1, int16 arg2)
{
	return arg1 * arg2;
}

int16
int2div(int16 arg1, int16 arg2)
{
	return arg1 / arg2;
}

int16
int2inc(int16 arg)
{
	return arg + (int16) 1;
}

int32
int24pl(int32 arg1, int32 arg2)
{
	return arg1 + arg2;
}

int32
int24mi(int32 arg1, int32 arg2)
{
	return arg1 - arg2;
}

int32
int24mul(int32 arg1, int32 arg2)
{
	return arg1 * arg2;
}

int32
int24div(int32 arg1, int32 arg2)
{
	return arg1 / arg2;
}

int32
int42pl(int32 arg1, int32 arg2)
{
	return arg1 + arg2;
}

int32
int42mi(int32 arg1, int32 arg2)
{
	return arg1 - arg2;
}

int32
int42mul(int32 arg1, int32 arg2)
{
	return arg1 * arg2;
}

int32
int42div(int32 arg1, int32 arg2)
{
	return arg1 / arg2;
}

/*
 *		int[24]mod		- returns arg1 mod arg2
 */
int32
int4mod(int32 arg1, int32 arg2)
{
	return arg1 % arg2;
}

int32
int2mod(int16 arg1, int16 arg2)
{
	return arg1 % arg2;
}

int32
int24mod(int32 arg1, int32 arg2)
{
	return arg1 % arg2;
}

int32
int42mod(int32 arg1, int32 arg2)
{
	return arg1 % arg2;
}

/*
 *		int[24]fac		- returns arg1!
 */
int32
int4fac(int32 arg1)
{
	int32		result;

	if (arg1 < 1)
		result = 0;
	else
		for (result = 1; arg1 > 0; --arg1)
			result *= arg1;
	return result;
}

int32
int2fac(int16 arg1)
{
	int16		result;

	if (arg1 < 1)
		result = 0;
	else
		for (result = 1; arg1 > 0; --arg1)
			result *= arg1;
	return result;
}

int16
int2larger(int16 arg1, int16 arg2)
{
	return (arg1 > arg2) ? arg1 : arg2;
}

int16
int2smaller(int16 arg1, int16 arg2)
{
	return (arg1 < arg2) ? arg1 : arg2;
}

int32
int4larger(int32 arg1, int32 arg2)
{
	return (arg1 > arg2) ? arg1 : arg2;
}

int32
int4smaller(int32 arg1, int32 arg2)
{
	return (arg1 < arg2) ? arg1 : arg2;
}
