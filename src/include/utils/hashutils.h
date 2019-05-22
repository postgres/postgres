/*
 * Utilities for working with hash values.
 *
 * Portions Copyright (c) 2017-2019, PostgreSQL Global Development Group
 */

#ifndef HASHUTILS_H
#define HASHUTILS_H


/*
 * Rotate the high 32 bits and the low 32 bits separately.  The standard
 * hash function sometimes rotates the low 32 bits by one bit when
 * combining elements.  We want extended hash functions to be compatible with
 * that algorithm when the seed is 0, so we can't just do a normal rotation.
 * This works, though.
 */
#define ROTATE_HIGH_AND_LOW_32BITS(v) \
	((((v) << 1) & UINT64CONST(0xfffffffefffffffe)) | \
	(((v) >> 31) & UINT64CONST(0x100000001)))


extern Datum hash_any(register const unsigned char *k, register int keylen);
extern Datum hash_any_extended(register const unsigned char *k,
							   register int keylen, uint64 seed);
extern Datum hash_uint32(uint32 k);
extern Datum hash_uint32_extended(uint32 k, uint64 seed);

/*
 * Combine two 32-bit hash values, resulting in another hash value, with
 * decent bit mixing.
 *
 * Similar to boost's hash_combine().
 */
static inline uint32
hash_combine(uint32 a, uint32 b)
{
	a ^= b + 0x9e3779b9 + (a << 6) + (a >> 2);
	return a;
}

/*
 * Combine two 64-bit hash values, resulting in another hash value, using the
 * same kind of technique as hash_combine().  Testing shows that this also
 * produces good bit mixing.
 */
static inline uint64
hash_combine64(uint64 a, uint64 b)
{
	/* 0x49a0f4dd15e5a8e3 is 64bit random data */
	a ^= b + UINT64CONST(0x49a0f4dd15e5a8e3) + (a << 54) + (a >> 7);
	return a;
}

/*
 * Simple inline murmur hash implementation hashing a 32 bit integer, for
 * performance.
 */
static inline uint32
murmurhash32(uint32 data)
{
	uint32		h = data;

	h ^= h >> 16;
	h *= 0x85ebca6b;
	h ^= h >> 13;
	h *= 0xc2b2ae35;
	h ^= h >> 16;
	return h;
}

#endif							/* HASHUTILS_H */
