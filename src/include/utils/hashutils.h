/*
 * Utilities for working with hash values.
 *
 * Portions Copyright (c) 2017, PostgreSQL Global Development Group
 */

#ifndef HASHUTILS_H
#define HASHUTILS_H

/*
 * Combine two hash values, resulting in another hash value, with decent bit
 * mixing.
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
 * Simple inline murmur hash implementation hashing a 32 bit ingeger, for
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
