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

#endif							/* HASHUTILS_H */
