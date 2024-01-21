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
 *
 * The incremental interface is especially useful if any of the inputs
 * are NUL-terminated C strings, since the length is not needed ahead
 * of time. This avoids needing to call strlen(). This case is optimized
 * in fasthash_accum_cstring() :
 *
 * fasthash_state hs;
 * fasthash_init(&hs, 0);
 * len = fasthash_accum_cstring(&hs, *str);
 * ...
 * return fasthash_final32(&hs, len);
 *
 * The length is computed on-the-fly. Experimentation has found that
 * SMHasher fails unless we incorporate the length, so it is passed to
 * the finalizer as a tweak.
 */


typedef struct fasthash_state
{
	/* staging area for chunks of input */
	uint64		accum;

	uint64		hash;
} fasthash_state;

#define FH_SIZEOF_ACCUM sizeof(uint64)


/*
 * Initialize the hash state.
 *
 * 'seed' can be zero.
 */
static inline void
fasthash_init(fasthash_state *hs, uint64 seed)
{
	memset(hs, 0, sizeof(fasthash_state));
	hs->hash = seed ^ 0x880355f21e6d1965;
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
 * Set high bit in lowest byte where the input is zero, from:
 * https://graphics.stanford.edu/~seander/bithacks.html#ZeroInWord
 */
#define haszero64(v) \
	(((v) - 0x0101010101010101) & ~(v) & 0x8080808080808080)

/*
 * all-purpose workhorse for fasthash_accum_cstring
 */
static inline int
fasthash_accum_cstring_unaligned(fasthash_state *hs, const char *str)
{
	const char *const start = str;

	while (*str)
	{
		int			chunk_len = 0;

		while (chunk_len < FH_SIZEOF_ACCUM && str[chunk_len] != '\0')
			chunk_len++;

		fasthash_accum(hs, str, chunk_len);
		str += chunk_len;
	}

	return str - start;
}

/*
 * specialized workhorse for fasthash_accum_cstring
 *
 * With an aligned pointer, we consume the string a word at a time.
 * Loading the word containing the NUL terminator cannot segfault since
 * allocation boundaries are suitably aligned.
 */
static inline int
fasthash_accum_cstring_aligned(fasthash_state *hs, const char *str)
{
	const char *const start = str;
	int			remainder;
	uint64		zero_bytes_le;

	Assert(PointerIsAligned(start, uint64));
	for (;;)
	{
		uint64		chunk = *(uint64 *) str;

		/*
		 * With little-endian representation, we can use this calculation,
		 * which sets bits in the first byte in the result word that
		 * corresponds to a zero byte in the original word. The rest of the
		 * bytes are indeterminate, so cannot be used on big-endian machines
		 * without either swapping or a bytewise check.
		 */
#ifdef WORDS_BIGENDIAN
		zero_bytes_le = haszero64(pg_bswap64(chunk));
#else
		zero_bytes_le = haszero64(chunk);
#endif
		if (zero_bytes_le)
			break;

		hs->accum = chunk;
		fasthash_combine(hs);
		str += FH_SIZEOF_ACCUM;
	}

	/*
	 * For the last word, only use bytes up to the NUL for the hash. Bytes
	 * with set bits will be 0x80, so calculate the first occurrence of a zero
	 * byte within the input word by counting the number of trailing (because
	 * little-endian) zeros and dividing the result by 8.
	 */
	remainder = pg_rightmost_one_pos64(zero_bytes_le) / BITS_PER_BYTE;
	fasthash_accum(hs, str, remainder);
	str += remainder;

	return str - start;
}

/*
 * Mix 'str' into the hash state and return the length of the string.
 */
static inline int
fasthash_accum_cstring(fasthash_state *hs, const char *str)
{
#if SIZEOF_VOID_P >= 8

	int			len;
#ifdef USE_ASSERT_CHECKING
	int			len_check;
	fasthash_state hs_check;

	memcpy(&hs_check, hs, sizeof(fasthash_state));
	len_check = fasthash_accum_cstring_unaligned(&hs_check, str);
#endif
	if (PointerIsAligned(str, uint64))
	{
		len = fasthash_accum_cstring_aligned(hs, str);
		Assert(hs_check.hash == hs->hash && len_check == len);
		return len;
	}
#endif							/* SIZEOF_VOID_P */

	/*
	 * It's not worth it to try to make the word-at-a-time optimization work
	 * on 32-bit platforms.
	 */
	return fasthash_accum_cstring_unaligned(hs, str);
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

	fasthash_init(&hs, 0);

	/* re-initialize the seed according to input length */
	hs.hash = seed ^ (len * 0x880355f21e6d1965);

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
