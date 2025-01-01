/*-------------------------------------------------------------------------
 *
 * char.c
 *	  Functions for the built-in type "char" (not to be confused with
 *	  bpchar, which is the SQL CHAR(n) type).
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/char.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "libpq/pqformat.h"
#include "utils/fmgrprotos.h"
#include "varatt.h"

#define ISOCTAL(c)   (((c) >= '0') && ((c) <= '7'))
#define TOOCTAL(c)   ((c) + '0')
#define FROMOCTAL(c) ((unsigned char) (c) - '0')


/*****************************************************************************
 *	 USER I/O ROUTINES														 *
 *****************************************************************************/

/*
 *		charin			- converts "x" to 'x'
 *
 * This accepts the formats charout produces.  If we have multibyte input
 * that is not in the form '\ooo', then we take its first byte as the value
 * and silently discard the rest; this is a backwards-compatibility provision.
 */
Datum
charin(PG_FUNCTION_ARGS)
{
	char	   *ch = PG_GETARG_CSTRING(0);

	if (strlen(ch) == 4 && ch[0] == '\\' &&
		ISOCTAL(ch[1]) && ISOCTAL(ch[2]) && ISOCTAL(ch[3]))
		PG_RETURN_CHAR((FROMOCTAL(ch[1]) << 6) +
					   (FROMOCTAL(ch[2]) << 3) +
					   FROMOCTAL(ch[3]));
	/* This will do the right thing for a zero-length input string */
	PG_RETURN_CHAR(ch[0]);
}

/*
 *		charout			- converts 'x' to "x"
 *
 * The possible output formats are:
 * 1. 0x00 is represented as an empty string.
 * 2. 0x01..0x7F are represented as a single ASCII byte.
 * 3. 0x80..0xFF are represented as \ooo (backslash and 3 octal digits).
 * Case 3 is meant to match the traditional "escape" format of bytea.
 */
Datum
charout(PG_FUNCTION_ARGS)
{
	char		ch = PG_GETARG_CHAR(0);
	char	   *result = (char *) palloc(5);

	if (IS_HIGHBIT_SET(ch))
	{
		result[0] = '\\';
		result[1] = TOOCTAL(((unsigned char) ch) >> 6);
		result[2] = TOOCTAL((((unsigned char) ch) >> 3) & 07);
		result[3] = TOOCTAL(((unsigned char) ch) & 07);
		result[4] = '\0';
	}
	else
	{
		/* This produces acceptable results for 0x00 as well */
		result[0] = ch;
		result[1] = '\0';
	}
	PG_RETURN_CSTRING(result);
}

/*
 *		charrecv			- converts external binary format to char
 *
 * The external representation is one byte, with no character set
 * conversion.  This is somewhat dubious, perhaps, but in many
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
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	char	   *ch = VARDATA_ANY(arg1);
	char		result;

	/*
	 * Conversion rules are the same as in charin(), but here we need to
	 * handle the empty-string case honestly.
	 */
	if (VARSIZE_ANY_EXHDR(arg1) == 4 && ch[0] == '\\' &&
		ISOCTAL(ch[1]) && ISOCTAL(ch[2]) && ISOCTAL(ch[3]))
		result = (FROMOCTAL(ch[1]) << 6) +
			(FROMOCTAL(ch[2]) << 3) +
			FROMOCTAL(ch[3]);
	else if (VARSIZE_ANY_EXHDR(arg1) > 0)
		result = ch[0];
	else
		result = '\0';

	PG_RETURN_CHAR(result);
}

Datum
char_text(PG_FUNCTION_ARGS)
{
	char		arg1 = PG_GETARG_CHAR(0);
	text	   *result = palloc(VARHDRSZ + 4);

	/*
	 * Conversion rules are the same as in charout(), but here we need to be
	 * honest about converting 0x00 to an empty string.
	 */
	if (IS_HIGHBIT_SET(arg1))
	{
		SET_VARSIZE(result, VARHDRSZ + 4);
		(VARDATA(result))[0] = '\\';
		(VARDATA(result))[1] = TOOCTAL(((unsigned char) arg1) >> 6);
		(VARDATA(result))[2] = TOOCTAL((((unsigned char) arg1) >> 3) & 07);
		(VARDATA(result))[3] = TOOCTAL(((unsigned char) arg1) & 07);
	}
	else if (arg1 != '\0')
	{
		SET_VARSIZE(result, VARHDRSZ + 1);
		*(VARDATA(result)) = arg1;
	}
	else
		SET_VARSIZE(result, VARHDRSZ);

	PG_RETURN_TEXT_P(result);
}
