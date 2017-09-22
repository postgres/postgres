/*
 * Utilities for working with hash values.
 *
 * Portions Copyright (c) 2017, PostgreSQL Global Development Group
 */

#ifndef HASHUTILS_H
#define HASHUTILS_H

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
