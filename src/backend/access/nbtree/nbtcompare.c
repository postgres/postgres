/*-------------------------------------------------------------------------
 *
 * nbtcompare.c--
 *	  Comparison functions for btree access method.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/nbtree/nbtcompare.c,v 1.12 1997/09/08 02:20:44 momjian Exp $
 *
 *	NOTES
 *		These functions are stored in pg_amproc.  For each operator class
 *		defined on btrees, they compute
 *
 *				compare(a, b):
 *						< 0 if a < b,
 *						= 0 if a == b,
 *						> 0 if a > b.
 *-------------------------------------------------------------------------
 */

#include <string.h>

#include <postgres.h>

#include <utils/builtins.h>
#include <utils/nabstime.h>

int32
btint2cmp(int16 a, int16 b)
{
	return ((int32) (a - b));
}

int32
btint4cmp(int32 a, int32 b)
{
	return (a - b);
}

int32
btint24cmp(int16 a, int32 b)
{
	return (((int32) a) - b);
}

int32
btint42cmp(int32 a, int16 b)
{
	return (a - ((int32) b));
}

int32
btfloat4cmp(float32 a, float32 b)
{
	if (*a > *b)
		return (1);
	else if (*a == *b)
		return (0);
	else
		return (-1);
}

int32
btfloat8cmp(float64 a, float64 b)
{
	if (*a > *b)
		return (1);
	else if (*a == *b)
		return (0);
	else
		return (-1);
}

int32
btoidcmp(Oid a, Oid b)
{
	if (a > b)
		return (1);
	else if (a == b)
		return (0);
	else
		return (-1);
}

int32
btabstimecmp(AbsoluteTime a, AbsoluteTime b)
{
	if (AbsoluteTimeIsBefore(a, b))
		return (-1);
	else if (AbsoluteTimeIsBefore(b, a))
		return (1);
	else
		return (0);
}

int32
btcharcmp(char a, char b)
{
	return ((int32) ((uint8) a - (uint8) b));
}

int32
btchar2cmp(uint16 a, uint16 b)
{
	return (strncmp((char *) &a, (char *) &b, 2));
}

int32
btchar4cmp(uint32 a, uint32 b)
{
	return (strncmp((char *) &a, (char *) &b, 4));
}

int32
btchar8cmp(char *a, char *b)
{
	return (strncmp(a, b, 8));
}

int32
btchar16cmp(char *a, char *b)
{
	return (strncmp(a, b, 16));
}

int32
btnamecmp(NameData * a, NameData * b)
{
	return (strncmp(a->data, b->data, NAMEDATALEN));
}

int32
bttextcmp(struct varlena * a, struct varlena * b)
{
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
			res = (int) (*ap++ - *bp++);
			len--;
		} while (res == 0 && len != 0);
	}

#endif

	if (res != 0 || VARSIZE(a) == VARSIZE(b))
		return (res);

	/*
	 * The two strings are the same in the first len bytes, and they are
	 * of different lengths.
	 */

	if (VARSIZE(a) < VARSIZE(b))
		return (-1);
	else
		return (1);
}
