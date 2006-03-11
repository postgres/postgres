/*-------------------------------------------------------------------------
 *
 * char.c
 *	  Functions for the built-in type "char" (not to be confused with
 *	  bpchar, which is the SQL CHAR(n) type).
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/char.c,v 1.45 2006/03/11 01:19:22 neilc Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "libpq/pqformat.h"
#include "utils/builtins.h"

/*****************************************************************************
 *	 USER I/O ROUTINES														 *
 *****************************************************************************/

/*
 *		charin			- converts "x" to 'x'
 *
 * Note that an empty input string will implicitly be converted to \0.
 */
Datum
charin(PG_FUNCTION_ARGS)
{
	char	   *ch = PG_GETARG_CSTRING(0);

	PG_RETURN_CHAR(ch[0]);
}

/*
 *		charout			- converts 'x' to "x"
 *
 * Note that if the char value is \0, the resulting string will appear
 * to be empty (null-terminated after zero characters).  So this is the
 * inverse of the charin() function for such data.
 */
Datum
charout(PG_FUNCTION_ARGS)
{
	char		ch = PG_GETARG_CHAR(0);
	char	   *result = (char *) palloc(2);

	result[0] = ch;
	result[1] = '\0';
	PG_RETURN_CSTRING(result);
}

/*
 *		charrecv			- converts external binary format to char
 *
 * The external representation is one byte, with no character set
 * conversion.	This is somewhat dubious, perhaps, but in many
 * cases people use char for a 1-byte binary type.
 */
Datum
charrecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	PG_RETURN_CHAR(pq_getmsgbyte(buf));
}

/*
 *		charsend			- converts char to binary format
 */
Datum
charsend(PG_FUNCTION_ARGS)
{
	char		arg1 = PG_GETARG_CHAR(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendbyte(&buf, arg1);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*****************************************************************************
 *	 PUBLIC ROUTINES														 *
 *****************************************************************************/

/*
 * NOTE: comparisons are done as though char is unsigned (uint8).
 * Conversions to and from integer are done as though char is signed (int8).
 *
 * You wanted consistency?
 */

Datum
chareq(PG_FUNCTION_ARGS)
{
	char		arg1 = PG_GETARG_CHAR(0);
	char		arg2 = PG_GETARG_CHAR(1);

	PG_RETURN_BOOL(arg1 == arg2);
}

Datum
charne(PG_FUNCTION_ARGS)
{
	char		arg1 = PG_GETARG_CHAR(0);
	char		arg2 = PG_GETARG_CHAR(1);

	PG_RETURN_BOOL(arg1 != arg2);
}

Datum
charlt(PG_FUNCTION_ARGS)
{
	char		arg1 = PG_GETARG_CHAR(0);
	char		arg2 = PG_GETARG_CHAR(1);

	PG_RETURN_BOOL((uint8) arg1 < (uint8) arg2);
}

Datum
charle(PG_FUNCTION_ARGS)
{
	char		arg1 = PG_GETARG_CHAR(0);
	char		arg2 = PG_GETARG_CHAR(1);

	PG_RETURN_BOOL((uint8) arg1 <= (uint8) arg2);
}

Datum
chargt(PG_FUNCTION_ARGS)
{
	char		arg1 = PG_GETARG_CHAR(0);
	char		arg2 = PG_GETARG_CHAR(1);

	PG_RETURN_BOOL((uint8) arg1 > (uint8) arg2);
}

Datum
charge(PG_FUNCTION_ARGS)
{
	char		arg1 = PG_GETARG_CHAR(0);
	char		arg2 = PG_GETARG_CHAR(1);

	PG_RETURN_BOOL((uint8) arg1 >= (uint8) arg2);
}


Datum
chartoi4(PG_FUNCTION_ARGS)
{
	char		arg1 = PG_GETARG_CHAR(0);

	PG_RETURN_INT32((int32) ((int8) arg1));
}

Datum
i4tochar(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);

	if (arg1 < SCHAR_MIN || arg1 > SCHAR_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("\"char\" out of range")));

	PG_RETURN_CHAR((int8) arg1);
}


Datum
text_char(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_P(0);
	char		result;

	/*
	 * An empty input string is converted to \0 (for consistency with charin).
	 * If the input is longer than one character, the excess data is silently
	 * discarded.
	 */
	if (VARSIZE(arg1) > VARHDRSZ)
		result = *(VARDATA(arg1));
	else
		result = '\0';

	PG_RETURN_CHAR(result);
}

Datum
char_text(PG_FUNCTION_ARGS)
{
	char		arg1 = PG_GETARG_CHAR(0);
	text	   *result = palloc(VARHDRSZ + 1);

	/*
	 * Convert \0 to an empty string, for consistency with charout (and
	 * because the text stuff doesn't like embedded nulls all that well).
	 */
	if (arg1 != '\0')
	{
		VARATT_SIZEP(result) = VARHDRSZ + 1;
		*(VARDATA(result)) = arg1;
	}
	else
		VARATT_SIZEP(result) = VARHDRSZ;

	PG_RETURN_TEXT_P(result);
}
