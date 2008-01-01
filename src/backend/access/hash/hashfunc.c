/*-------------------------------------------------------------------------
 *
 * hashfunc.c
 *	  Support functions for hash access method.
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/hash/hashfunc.c,v 1.55 2008/01/01 19:45:46 momjian Exp $
 *
 * NOTES
 *	  These functions are stored in pg_amproc.	For each operator class
 *	  defined for hash indexes, they compute the hash value of the argument.
 *
 *	  Additional hash functions appear in /utils/adt/ files for various
 *	  specialized datatypes.
 *
 *	  It is expected that every bit of a hash function's 32-bit result is
 *	  as random as every other; failure to ensure this is likely to lead
 *	  to poor performance of hash joins, for example.  In most cases a hash
 *	  function should use hash_any() or its variant hash_uint32().
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/hash.h"


/* Note: this is used for both "char" and boolean datatypes */
Datum
hashchar(PG_FUNCTION_ARGS)
{
	return hash_uint32((int32) PG_GETARG_CHAR(0));
}

Datum
hashint2(PG_FUNCTION_ARGS)
{
	return hash_uint32((int32) PG_GETARG_INT16(0));
}

Datum
hashint4(PG_FUNCTION_ARGS)
{
	return hash_uint32(PG_GETARG_INT32(0));
}

Datum
hashint8(PG_FUNCTION_ARGS)
{
	/*
	 * The idea here is to produce a hash value compatible with the values
	 * produced by hashint4 and hashint2 for logically equal inputs; this is
	 * necessary to support cross-type hash joins across these input types.
	 * Since all three types are signed, we can xor the high half of the int8
	 * value if the sign is positive, or the complement of the high half when
	 * the sign is negative.
	 */
#ifndef INT64_IS_BUSTED
	int64		val = PG_GETARG_INT64(0);
	uint32		lohalf = (uint32) val;
	uint32		hihalf = (uint32) (val >> 32);

	lohalf ^= (val >= 0) ? hihalf : ~hihalf;

	return hash_uint32(lohalf);
#else
	/* here if we can't count on "x >> 32" to work sanely */
	return hash_uint32((int32) PG_GETARG_INT64(0));
#endif
}

Datum
hashoid(PG_FUNCTION_ARGS)
{
	return hash_uint32((uint32) PG_GETARG_OID(0));
}

Datum
hashenum(PG_FUNCTION_ARGS)
{
	return hash_uint32((uint32) PG_GETARG_OID(0));
}

Datum
hashfloat4(PG_FUNCTION_ARGS)
{
	float4		key = PG_GETARG_FLOAT4(0);
	float8		key8;

	/*
	 * On IEEE-float machines, minus zero and zero have different bit patterns
	 * but should compare as equal.  We must ensure that they have the same
	 * hash value, which is most reliably done this way:
	 */
	if (key == (float4) 0)
		PG_RETURN_UINT32(0);

	/*
	 * To support cross-type hashing of float8 and float4, we want to return
	 * the same hash value hashfloat8 would produce for an equal float8 value.
	 * So, widen the value to float8 and hash that.  (We must do this rather
	 * than have hashfloat8 try to narrow its value to float4; that could fail
	 * on overflow.)
	 */
	key8 = key;

	return hash_any((unsigned char *) &key8, sizeof(key8));
}

Datum
hashfloat8(PG_FUNCTION_ARGS)
{
	float8		key = PG_GETARG_FLOAT8(0);

	/*
	 * On IEEE-float machines, minus zero and zero have different bit patterns
	 * but should compare as equal.  We must ensure that they have the same
	 * hash value, which is most reliably done this way:
	 */
	if (key == (float8) 0)
		PG_RETURN_UINT32(0);

	return hash_any((unsigned char *) &key, sizeof(key));
}

Datum
hashoidvector(PG_FUNCTION_ARGS)
{
	oidvector  *key = (oidvector *) PG_GETARG_POINTER(0);

	return hash_any((unsigned char *) key->values, key->dim1 * sizeof(Oid));
}

Datum
hashint2vector(PG_FUNCTION_ARGS)
{
	int2vector *key = (int2vector *) PG_GETARG_POINTER(0);

	return hash_any((unsigned char *) key->values, key->dim1 * sizeof(int2));
}

Datum
hashname(PG_FUNCTION_ARGS)
{
	char	   *key = NameStr(*PG_GETARG_NAME(0));
	int			keylen = strlen(key);

	Assert(keylen < NAMEDATALEN);		/* else it's not truncated correctly */

	return hash_any((unsigned char *) key, keylen);
}

Datum
hashtext(PG_FUNCTION_ARGS)
{
	text	   *key = PG_GETARG_TEXT_PP(0);
	Datum		result;

	/*
	 * Note: this is currently identical in behavior to hashvarlena, but keep
	 * it as a separate function in case we someday want to do something
	 * different in non-C locales.	(See also hashbpchar, if so.)
	 */
	result = hash_any((unsigned char *) VARDATA_ANY(key),
					  VARSIZE_ANY_EXHDR(key));

	/* Avoid leaking memory for toasted inputs */
	PG_FREE_IF_COPY(key, 0);

	return result;
}

/*
 * hashvarlena() can be used for any varlena datatype in which there are
 * no non-significant bits, ie, distinct bitpatterns never compare as equal.
 */
Datum
hashvarlena(PG_FUNCTION_ARGS)
{
	struct varlena *key = PG_GETARG_VARLENA_PP(0);
	Datum		result;

	result = hash_any((unsigned char *) VARDATA_ANY(key),
					  VARSIZE_ANY_EXHDR(key));

	/* Avoid leaking memory for toasted inputs */
	PG_FREE_IF_COPY(key, 0);

	return result;
}

/*
 * This hash function was written by Bob Jenkins
 * (bob_jenkins@burtleburtle.net), and superficially adapted
 * for PostgreSQL by Neil Conway. For more information on this
 * hash function, see http://burtleburtle.net/bob/hash/doobs.html,
 * or Bob's article in Dr. Dobb's Journal, Sept. 1997.
 */

/*----------
 * mix -- mix 3 32-bit values reversibly.
 * For every delta with one or two bits set, and the deltas of all three
 * high bits or all three low bits, whether the original value of a,b,c
 * is almost all zero or is uniformly distributed,
 * - If mix() is run forward or backward, at least 32 bits in a,b,c
 *	 have at least 1/4 probability of changing.
 * - If mix() is run forward, every bit of c will change between 1/3 and
 *	 2/3 of the time.  (Well, 22/100 and 78/100 for some 2-bit deltas.)
 *----------
 */
#define mix(a,b,c) \
{ \
  a -= b; a -= c; a ^= ((c)>>13); \
  b -= c; b -= a; b ^= ((a)<<8); \
  c -= a; c -= b; c ^= ((b)>>13); \
  a -= b; a -= c; a ^= ((c)>>12);  \
  b -= c; b -= a; b ^= ((a)<<16); \
  c -= a; c -= b; c ^= ((b)>>5); \
  a -= b; a -= c; a ^= ((c)>>3);	\
  b -= c; b -= a; b ^= ((a)<<10); \
  c -= a; c -= b; c ^= ((b)>>15); \
}

/*
 * hash_any() -- hash a variable-length key into a 32-bit value
 *		k		: the key (the unaligned variable-length array of bytes)
 *		len		: the length of the key, counting by bytes
 *
 * Returns a uint32 value.	Every bit of the key affects every bit of
 * the return value.  Every 1-bit and 2-bit delta achieves avalanche.
 * About 6*len+35 instructions. The best hash table sizes are powers
 * of 2.  There is no need to do mod a prime (mod is sooo slow!).
 * If you need less than 32 bits, use a bitmask.
 */
Datum
hash_any(register const unsigned char *k, register int keylen)
{
	register uint32 a,
				b,
				c,
				len;

	/* Set up the internal state */
	len = keylen;
	a = b = 0x9e3779b9;			/* the golden ratio; an arbitrary value */
	c = 3923095;				/* initialize with an arbitrary value */

	/* handle most of the key */
	while (len >= 12)
	{
		a += (k[0] + ((uint32) k[1] << 8) + ((uint32) k[2] << 16) + ((uint32) k[3] << 24));
		b += (k[4] + ((uint32) k[5] << 8) + ((uint32) k[6] << 16) + ((uint32) k[7] << 24));
		c += (k[8] + ((uint32) k[9] << 8) + ((uint32) k[10] << 16) + ((uint32) k[11] << 24));
		mix(a, b, c);
		k += 12;
		len -= 12;
	}

	/* handle the last 11 bytes */
	c += keylen;
	switch (len)				/* all the case statements fall through */
	{
		case 11:
			c += ((uint32) k[10] << 24);
		case 10:
			c += ((uint32) k[9] << 16);
		case 9:
			c += ((uint32) k[8] << 8);
			/* the first byte of c is reserved for the length */
		case 8:
			b += ((uint32) k[7] << 24);
		case 7:
			b += ((uint32) k[6] << 16);
		case 6:
			b += ((uint32) k[5] << 8);
		case 5:
			b += k[4];
		case 4:
			a += ((uint32) k[3] << 24);
		case 3:
			a += ((uint32) k[2] << 16);
		case 2:
			a += ((uint32) k[1] << 8);
		case 1:
			a += k[0];
			/* case 0: nothing left to add */
	}
	mix(a, b, c);

	/* report the result */
	return UInt32GetDatum(c);
}

/*
 * hash_uint32() -- hash a 32-bit value
 *
 * This has the same result (at least on little-endian machines) as
 *		hash_any(&k, sizeof(uint32))
 * but is faster and doesn't force the caller to store k into memory.
 */
Datum
hash_uint32(uint32 k)
{
	register uint32 a,
				b,
				c;

	a = 0x9e3779b9 + k;
	b = 0x9e3779b9;
	c = 3923095 + (uint32) sizeof(uint32);

	mix(a, b, c);

	/* report the result */
	return UInt32GetDatum(c);
}
