/*-------------------------------------------------------------------------
 *
 * hashfunc.c
 *	  Comparison functions for hash access method.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/hash/hashfunc.c,v 1.30 2001/03/22 03:59:13 momjian Exp $
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
hashchar(PG_FUNCTION_ARGS)
{
	PG_RETURN_UINT32(~((uint32) PG_GETARG_CHAR(0)));
}

Datum
hashint2(PG_FUNCTION_ARGS)
{
	PG_RETURN_UINT32(~((uint32) PG_GETARG_INT16(0)));
}

Datum
hashint4(PG_FUNCTION_ARGS)
{
	PG_RETURN_UINT32(~PG_GETARG_UINT32(0));
}

Datum
hashint8(PG_FUNCTION_ARGS)
{
	/* we just use the low 32 bits... */
	PG_RETURN_UINT32(~((uint32) PG_GETARG_INT64(0)));
}

Datum
hashoid(PG_FUNCTION_ARGS)
{
	PG_RETURN_UINT32(~((uint32) PG_GETARG_OID(0)));
}

Datum
hashfloat4(PG_FUNCTION_ARGS)
{
	float4		key = PG_GETARG_FLOAT4(0);

	return hash_any((char *) &key, sizeof(key));
}

Datum
hashfloat8(PG_FUNCTION_ARGS)
{
	float8		key = PG_GETARG_FLOAT8(0);

	return hash_any((char *) &key, sizeof(key));
}

Datum
hashoidvector(PG_FUNCTION_ARGS)
{
	Oid		   *key = (Oid *) PG_GETARG_POINTER(0);

	return hash_any((char *) key, INDEX_MAX_KEYS * sizeof(Oid));
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

	return hash_any((char *) key, INDEX_MAX_KEYS * sizeof(int16));
}

Datum
hashname(PG_FUNCTION_ARGS)
{
	char	   *key = NameStr(*PG_GETARG_NAME(0));

	return hash_any((char *) key, NAMEDATALEN);
}

/*
 * hashvarlena() can be used for any varlena datatype in which there are
 * no non-significant bits, ie, distinct bitpatterns never compare as equal.
 */
Datum
hashvarlena(PG_FUNCTION_ARGS)
{
	struct varlena *key = PG_GETARG_VARLENA_P(0);
	Datum		result;

	result = hash_any(VARDATA(key), VARSIZE(key) - VARHDRSZ);

	/* Avoid leaking memory for toasted inputs */
	PG_FREE_IF_COPY(key, 0);

	return result;
}


/*
 * hash_any --- compute a hash function for any specified chunk of memory
 *
 * This can be used as the underlying hash function for any pass-by-reference
 * data type in which there are no non-significant bits.
 *
 * (Comment from the original db3 hashing code: )
 *
 * "This is INCREDIBLY ugly, but fast.  We break the string up into 8 byte
 * units.  On the first time through the loop we get the 'leftover bytes'
 * (strlen % 8).  On every later iteration, we perform 8 HASHC's so we handle
 * all 8 bytes.  Essentially, this saves us 7 cmp & branch instructions.  If
 * this routine is heavily used enough, it's worth the ugly coding.
 *
 * "OZ's original sdbm hash"
 */
Datum
hash_any(char *keydata, int keylen)
{
	uint32		n;
	int			loop;

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

#undef HASHC

	PG_RETURN_UINT32(n);
}
