/*-------------------------------------------------------------------------
 *
 * nbtcompare.c
 *	  Comparison functions for btree access method.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/nbtree/nbtcompare.c,v 1.36 2000/06/09 01:11:01 tgl Exp $
 *
 * NOTES
 *
 *	These functions are stored in pg_amproc.  For each operator class
 *	defined on btrees, they compute
 *
 *				compare(a, b):
 *						< 0 if a < b,
 *						= 0 if a == b,
 *						> 0 if a > b.
 *
 *	The result is always an int32 regardless of the input datatype.
 *
 *	NOTE: although any negative int32 is acceptable for reporting "<",
 *	and any positive int32 is acceptable for reporting ">", routines
 *	that work on 32-bit or wider datatypes can't just return "a - b".
 *	That could overflow and give the wrong answer.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/builtins.h"

Datum
btboolcmp(PG_FUNCTION_ARGS)
{
	bool		a = PG_GETARG_BOOL(0);
	bool		b = PG_GETARG_BOOL(1);

	PG_RETURN_INT32((int32) a - (int32) b);
}

Datum
btint2cmp(PG_FUNCTION_ARGS)
{
	int16		a = PG_GETARG_INT16(0);
	int16		b = PG_GETARG_INT16(1);

	PG_RETURN_INT32((int32) a - (int32) b);
}

Datum
btint4cmp(PG_FUNCTION_ARGS)
{
	int32		a = PG_GETARG_INT32(0);
	int32		b = PG_GETARG_INT32(1);

	if (a > b)
		PG_RETURN_INT32(1);
	else if (a == b)
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(-1);
}

Datum
btint8cmp(PG_FUNCTION_ARGS)
{
	int64		a = PG_GETARG_INT64(0);
	int64		b = PG_GETARG_INT64(1);

	if (a > b)
		PG_RETURN_INT32(1);
	else if (a == b)
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(-1);
}

Datum
btint24cmp(PG_FUNCTION_ARGS)
{
	int16		a = PG_GETARG_INT16(0);
	int32		b = PG_GETARG_INT32(1);

	if (a > b)
		PG_RETURN_INT32(1);
	else if (a == b)
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(-1);
}

Datum
btint42cmp(PG_FUNCTION_ARGS)
{
	int32		a = PG_GETARG_INT32(0);
	int16		b = PG_GETARG_INT16(1);

	if (a > b)
		PG_RETURN_INT32(1);
	else if (a == b)
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(-1);
}

Datum
btfloat4cmp(PG_FUNCTION_ARGS)
{
	float4		a = PG_GETARG_FLOAT4(0);
	float4		b = PG_GETARG_FLOAT4(1);

	if (a > b)
		PG_RETURN_INT32(1);
	else if (a == b)
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(-1);
}

Datum
btfloat8cmp(PG_FUNCTION_ARGS)
{
	float8		a = PG_GETARG_FLOAT8(0);
	float8		b = PG_GETARG_FLOAT8(1);

	if (a > b)
		PG_RETURN_INT32(1);
	else if (a == b)
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(-1);
}

Datum
btoidcmp(PG_FUNCTION_ARGS)
{
	Oid			a = PG_GETARG_OID(0);
	Oid			b = PG_GETARG_OID(1);

	if (a > b)
		PG_RETURN_INT32(1);
	else if (a == b)
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(-1);
}

Datum
btoidvectorcmp(PG_FUNCTION_ARGS)
{
	Oid		   *a = (Oid *) PG_GETARG_POINTER(0);
	Oid		   *b = (Oid *) PG_GETARG_POINTER(1);
	int			i;

	for (i = 0; i < INDEX_MAX_KEYS; i++)
	{
		if (a[i] != b[i])
		{
			if (a[i] > b[i])
				PG_RETURN_INT32(1);
			else
				PG_RETURN_INT32(-1);
		}
	}
	PG_RETURN_INT32(0);
}

Datum
btabstimecmp(PG_FUNCTION_ARGS)
{
	AbsoluteTime	a = PG_GETARG_ABSOLUTETIME(0);
	AbsoluteTime	b = PG_GETARG_ABSOLUTETIME(1);

	if (AbsoluteTimeIsBefore(a, b))
		PG_RETURN_INT32(-1);
	else if (AbsoluteTimeIsBefore(b, a))
		PG_RETURN_INT32(1);
	else
		PG_RETURN_INT32(0);
}

Datum
btcharcmp(PG_FUNCTION_ARGS)
{
	char		a = PG_GETARG_CHAR(0);
	char		b = PG_GETARG_CHAR(1);

	/* Be careful to compare chars as unsigned */
	PG_RETURN_INT32((int32) ((uint8) a) - (int32) ((uint8) b));
}

Datum
btnamecmp(PG_FUNCTION_ARGS)
{
	Name		a = PG_GETARG_NAME(0);
	Name		b = PG_GETARG_NAME(1);

	PG_RETURN_INT32(strncmp(NameStr(*a), NameStr(*b), NAMEDATALEN));
}

Datum
bttextcmp(PG_FUNCTION_ARGS)
{
	text	   *a = PG_GETARG_TEXT_P(0);
	text	   *b = PG_GETARG_TEXT_P(1);
	int			res;
	unsigned char *ap,
			   *bp;

#ifdef USE_LOCALE
	int			la = VARSIZE(a) - VARHDRSZ;
	int			lb = VARSIZE(b) - VARHDRSZ;

	ap = (unsigned char *) palloc(la + 1);
	bp = (unsigned char *) palloc(lb + 1);

	memcpy(ap, VARDATA(a), la);
	*(ap + la) = '\0';
	memcpy(bp, VARDATA(b), lb);
	*(bp + lb) = '\0';

	res = strcoll(ap, bp);

	pfree(ap);
	pfree(bp);

#else
	int			len = VARSIZE(a);

	/* len is the length of the shorter of the two strings */
	if (len > VARSIZE(b))
		len = VARSIZE(b);

	len -= VARHDRSZ;

	ap = (unsigned char *) VARDATA(a);
	bp = (unsigned char *) VARDATA(b);

	/*
	 * If the two strings differ in the first len bytes, or if they're the
	 * same in the first len bytes and they're both len bytes long, we're
	 * done.
	 */

	res = 0;
	if (len > 0)
	{
		do
		{
			res = (int) *ap++ - (int) *bp++;
			len--;
		} while (res == 0 && len != 0);
	}

#endif

	if (res != 0 || VARSIZE(a) == VARSIZE(b))
		PG_RETURN_INT32(res);

	/*
	 * The two strings are the same in the first len bytes, and they are
	 * of different lengths.
	 */

	if (VARSIZE(a) < VARSIZE(b))
		PG_RETURN_INT32(-1);
	else
		PG_RETURN_INT32(1);
}
