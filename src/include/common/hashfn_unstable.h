/*
 * hashfn_unstable.h
 *
 * Building blocks for creating fast inlineable hash functions. The
 * functions in this file are not guaranteed to be stable between versions,
 * and may differ by hardware platform. Hence they must not be used in
 * indexes or other on-disk structures. See hashfn.h if you need stability.
 *
 *
 * Portions Copyright (c) 2024, PostgreSQL Global Development Group
 *
 * src/include/common/hashfn_unstable.h
 */
#ifndef HASHFN_UNSTABLE_H
#define HASHFN_UNSTABLE_H

#include "port/pg_bitutils.h"
#include "port/pg_bswap.h"

/*
 * fasthash is a modification of code taken from
 * https://code.google.com/archive/p/fast-hash/source/default/source
 * under the terms of the MIT license. The original copyright
 * notice follows:
 */

/* The MIT License

   Copyright (C) 2012 Zilong Tan (eric.zltan@gmail.com)

   Permission is hereby granted, free of charge, to any person
   obtaining a copy of this software and associated documentation
   files (the "Software"), to deal in the Software without
   restriction, including without limitation the rights to use, copy,
   modify, merge, publish, distribute, sublicense, and/or sell copies
   of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
   BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
   ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.
*/

/*
 * fasthash as implemented here has two interfaces:
 *
 * 1) Standalone functions, e.g. fasthash32() for a single value with a
 * known length.
 *
 * 2) Incremental interface. This can used for incorporating multiple
 * inputs. The standalone functions use this internally, so see fasthash64()
 * for an an example of how this works.
 */


typedef struct fasthash_state
{
	/* staging area for chunks of input */
	uint64		accum;

	uint64		hash;
} fasthash_state;

#define FH_SIZEOF_ACCUM sizeof(uint64)

#define FH_UNKNOWN_LENGTH 1

/*
 * Initialize the hash state.
 *
 * 'len' is the length of the input, if known ahead of time.
 * If that is not known, pass FH_UNKNOWN_LENGTH.
 * 'seed' can be zero.
 */
static inline void
fasthash_init(fasthash_state *hs, int len, uint64 seed)
{
	memset(hs, 0, sizeof(fasthash_state));
	hs->hash = seed ^ (len * 0x880355f21e6d1965);
}

/* both the finalizer and part of the combining step */
static inline uint64
fasthash_mix(uint64 h, uint64 tweak)
{
	h ^= (h >> 23) + tweak;
	h *= 0x2127599bf4325c37;
	h ^= h >> 47;
	return h;
}

/* combine one chunk of input into the hash */
static inline void
fasthash_combine(fasthash_state *hs)
{
	hs->hash ^= fasthash_mix(hs->accum, 0);
	hs->hash *= 0x880355f21e6d1965;

	/* reset hash state for next input */
	hs->accum = 0;
}

/* accumulate up to 8 bytes of input and combine it into the hash */
static inline void
fasthash_accum(fasthash_state *hs, const char *k, int len)
{
	uint32		lower_four;

	Assert(hs->accum == 0);
	Assert(len <= FH_SIZEOF_ACCUM);

	switch (len)
	{
		case 8:
			memcpy(&hs->accum, k, 8);
			break;
		case 7:
			hs->accum |= (uint64) k[6] << 48;
			/* FALLTHROUGH */
		case 6:
			hs->accum |= (uint64) k[5] << 40;
			/* FALLTHROUGH */
		case 5:
			hs->accum |= (uint64) k[4] << 32;
			/* FALLTHROUGH */
		case 4:
			memcpy(&lower_four, k, sizeof(lower_four));
			hs->accum |= lower_four;
			break;
		case 3:
			hs->accum |= (uint64) k[2] << 16;
			/* FALLTHROUGH */
		case 2:
			hs->accum |= (uint64) k[1] << 8;
			/* FALLTHROUGH */
		case 1:
			hs->accum |= (uint64) k[0];
			break;
		case 0:
			return;
	}

	fasthash_combine(hs);
}

/*
 * The finalizer
 *
 * 'tweak' is intended to be the input length when the caller doesn't know
 * the length ahead of time, such as for NUL-terminated strings, otherwise
 * zero.
 */
static inline uint64
fasthash_final64(fasthash_state *hs, uint64 tweak)
{
	return fasthash_mix(hs->hash, tweak);
}

/*
 * Reduce a 64-bit hash to a 32-bit hash.
 *
 * This optional step provides a bit more additional mixing compared to
 * just taking the lower 32-bits.
 */
static inline uint32
fasthash_reduce32(uint64 h)
{
	/*
	 * Convert the 64-bit hashcode to Fermat residue, which shall retain
	 * information from both the higher and lower parts of hashcode.
	 */
	return h - (h >> 32);
}

/* finalize and reduce */
static inline uint32
fasthash_final32(fasthash_state *hs, uint64 tweak)
{
	return fasthash_reduce32(fasthash_final64(hs, tweak));
}

/*
 * The original fasthash64 function, re-implemented using the incremental
 * interface. Returns a 64-bit hashcode. 'len' controls not only how
 * many bytes to hash, but also modifies the internal seed.
 * 'seed' can be zero.
 */
static inline uint64
fasthash64(const char *k, int len, uint64 seed)
{
	fasthash_state hs;

	fasthash_init(&hs, len, seed);

	while (len >= FH_SIZEOF_ACCUM)
	{
		fasthash_accum(&hs, k, FH_SIZEOF_ACCUM);
		k += FH_SIZEOF_ACCUM;
		len -= FH_SIZEOF_ACCUM;
	}

	fasthash_accum(&hs, k, len);
	return fasthash_final64(&hs, 0);
}

/* like fasthash64, but returns a 32-bit hashcode */
static inline uint64
fasthash32(const char *k, int len, uint64 seed)
{
	return fasthash_reduce32(fasthash64(k, len, seed));
}

#endif							/* HASHFN_UNSTABLE_H */
