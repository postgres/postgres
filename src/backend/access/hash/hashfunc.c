/*-------------------------------------------------------------------------
 *
 * hashfunc.c
 *	  Comparison functions for hash access method.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/hash/hashfunc.c,v 1.26 2000/06/05 07:28:35 tgl Exp $
 *
 * NOTES
 *	  These functions are stored in pg_amproc.	For each operator class
 *	  defined on hash tables, they compute the hash value of the argument.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/hash.h"

Datum
hashint2(PG_FUNCTION_ARGS)
{
	PG_RETURN_UINT32((uint32) ~ PG_GETARG_INT16(0));
}

Datum
hashint4(PG_FUNCTION_ARGS)
{
	PG_RETURN_UINT32(~ PG_GETARG_UINT32(0));
}

Datum
hashint8(PG_FUNCTION_ARGS)
{
	/* we just use the low 32 bits... */
	PG_RETURN_UINT32(~((uint32) PG_GETARG_INT64(0)));
}

/* Hash function from Chris Torek. */
Datum
hashfloat4(PG_FUNCTION_ARGS)
{
	float4		key = PG_GETARG_FLOAT4(0);
	char	   *kp = (char *) &key;
	int			len = sizeof(key);
	int			loop;
	uint32		h;

#define HASH4a	 h = (h << 5) - h + *kp++;
#define HASH4b	 h = (h << 5) + h + *kp++;
#define HASH4 HASH4b

	h = 0;
	/*
	 * This is a tad silly, given that we expect len = 4, but a smart
	 * compiler should be able to eliminate the redundant code...
	 */
	loop = (len + 8 - 1) >> 3;

	switch (len & (8 - 1))
	{
		case 0:
			do
			{					/* All fall throughs */
				HASH4;
		case 7:
				HASH4;
		case 6:
				HASH4;
		case 5:
				HASH4;
		case 4:
				HASH4;
		case 3:
				HASH4;
		case 2:
				HASH4;
		case 1:
				HASH4;
			} while (--loop);
	}
	PG_RETURN_UINT32(h);
}

Datum
hashfloat8(PG_FUNCTION_ARGS)
{
	float8		key = PG_GETARG_FLOAT8(0);
	char	   *kp = (char *) &key;
	int			len = sizeof(key);
	int			loop;
	uint32		h;

#define HASH4a	 h = (h << 5) - h + *kp++;
#define HASH4b	 h = (h << 5) + h + *kp++;
#define HASH4 HASH4b

	h = 0;
	/*
	 * This is a tad silly, given that we expect len = 8, but a smart
	 * compiler should be able to eliminate the redundant code...
	 */
	loop = (len + 8 - 1) >> 3;

	switch (len & (8 - 1))
	{
		case 0:
			do
			{					/* All fall throughs */
				HASH4;
		case 7:
				HASH4;
		case 6:
				HASH4;
		case 5:
				HASH4;
		case 4:
				HASH4;
		case 3:
				HASH4;
		case 2:
				HASH4;
		case 1:
				HASH4;
			} while (--loop);
	}
	PG_RETURN_UINT32(h);
}

Datum
hashoid(PG_FUNCTION_ARGS)
{
	PG_RETURN_UINT32(~(uint32) PG_GETARG_OID(0));
}

Datum
hashoidvector(PG_FUNCTION_ARGS)
{
	Oid		   *key = (Oid *) PG_GETARG_POINTER(0);
	int			i;
	uint32		result = 0;

	for (i = INDEX_MAX_KEYS; --i >= 0;)
		result = (result << 1) ^ (~(uint32) key[i]);
	PG_RETURN_UINT32(result);
}

/*
 * Note: hashint2vector currently can't be used as a user hash table
 * hash function, because it has no pg_proc entry.	We only need it
 * for catcache indexing.
 */
Datum
hashint2vector(PG_FUNCTION_ARGS)
{
	int16	   *key = (int16 *) PG_GETARG_POINTER(0);
	int			i;
	uint32		result = 0;

	for (i = INDEX_MAX_KEYS; --i >= 0;)
		result = (result << 1) ^ (~(uint32) key[i]);
	PG_RETURN_UINT32(result);
}


#define PRIME1			37
#define PRIME2			1048583

Datum
hashchar(PG_FUNCTION_ARGS)
{
	uint32		h;

	/* Convert char to integer */
	h = (PG_GETARG_CHAR(0) - ' ');
	h %= PRIME2;

	PG_RETURN_UINT32(h);
}

Datum
hashname(PG_FUNCTION_ARGS)
{
	char	   *key = NameStr(* PG_GETARG_NAME(0));
	int			len = NAMEDATALEN;
	uint32		h;

	h = 0;
	/* Convert string to integer */
	while (len--)
		h = h * PRIME1 ^ (*key++ - ' ');
	h %= PRIME2;

	PG_RETURN_UINT32(h);
}

/*
 * (Comment from the original db3 hashing code: )
 *
 * "This is INCREDIBLY ugly, but fast.  We break the string up into 8 byte
 * units.  On the first time through the loop we get the 'leftover bytes'
 * (strlen % 8).  On every other iteration, we perform 8 HASHC's so we handle
 * all 8 bytes.  Essentially, this saves us 7 cmp & branch instructions.  If
 * this routine is heavily used enough, it's worth the ugly coding.
 *
 * "OZ's original sdbm hash"
 */
Datum
hashtext(PG_FUNCTION_ARGS)
{
	text	   *key = PG_GETARG_TEXT_P(0);
	int			keylen;
	char	   *keydata;
	uint32		n;
	int			loop;

	keydata = VARDATA(key);
	keylen = VARSIZE(key) - VARHDRSZ;

#define HASHC	n = *keydata++ + 65599 * n

	n = 0;
	if (keylen > 0)
	{
		loop = (keylen + 8 - 1) >> 3;

		switch (keylen & (8 - 1))
		{
			case 0:
				do
				{				/* All fall throughs */
					HASHC;
			case 7:
					HASHC;
			case 6:
					HASHC;
			case 5:
					HASHC;
			case 4:
					HASHC;
			case 3:
					HASHC;
			case 2:
					HASHC;
			case 1:
					HASHC;
				} while (--loop);
		}
	}
	PG_RETURN_UINT32(n);
}
