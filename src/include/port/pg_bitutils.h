/*-------------------------------------------------------------------------
 *
 * pg_bitutils.h
 *	  Miscellaneous functions for bit-wise operations.
 *
 *
 * Copyright (c) 2019-2020, PostgreSQL Global Development Group
 *
 * src/include/port/pg_bitutils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_BITUTILS_H
#define PG_BITUTILS_H

#ifndef FRONTEND
extern PGDLLIMPORT const uint8 pg_leftmost_one_pos[256];
extern PGDLLIMPORT const uint8 pg_rightmost_one_pos[256];
extern PGDLLIMPORT const uint8 pg_number_of_ones[256];
#else
extern const uint8 pg_leftmost_one_pos[256];
extern const uint8 pg_rightmost_one_pos[256];
extern const uint8 pg_number_of_ones[256];
#endif

/*
 * pg_leftmost_one_pos32
 *		Returns the position of the most significant set bit in "word",
 *		measured from the least significant bit.  word must not be 0.
 */
static inline int
pg_leftmost_one_pos32(uint32 word)
{
#ifdef HAVE__BUILTIN_CLZ
	Assert(word != 0);

	return 31 - __builtin_clz(word);
#else
	int			shift = 32 - 8;

	Assert(word != 0);

	while ((word >> shift) == 0)
		shift -= 8;

	return shift + pg_leftmost_one_pos[(word >> shift) & 255];
#endif							/* HAVE__BUILTIN_CLZ */
}

/*
 * pg_leftmost_one_pos64
 *		As above, but for a 64-bit word.
 */
static inline int
pg_leftmost_one_pos64(uint64 word)
{
#ifdef HAVE__BUILTIN_CLZ
	Assert(word != 0);

#if defined(HAVE_LONG_INT_64)
	return 63 - __builtin_clzl(word);
#elif defined(HAVE_LONG_LONG_INT_64)
	return 63 - __builtin_clzll(word);
#else
#error must have a working 64-bit integer datatype
#endif
#else							/* !HAVE__BUILTIN_CLZ */
	int			shift = 64 - 8;

	Assert(word != 0);

	while ((word >> shift) == 0)
		shift -= 8;

	return shift + pg_leftmost_one_pos[(word >> shift) & 255];
#endif							/* HAVE__BUILTIN_CLZ */
}

/*
 * pg_rightmost_one_pos32
 *		Returns the position of the least significant set bit in "word",
 *		measured from the least significant bit.  word must not be 0.
 */
static inline int
pg_rightmost_one_pos32(uint32 word)
{
#ifdef HAVE__BUILTIN_CTZ
	Assert(word != 0);

	return __builtin_ctz(word);
#else
	int			result = 0;

	Assert(word != 0);

	while ((word & 255) == 0)
	{
		word >>= 8;
		result += 8;
	}
	result += pg_rightmost_one_pos[word & 255];
	return result;
#endif							/* HAVE__BUILTIN_CTZ */
}

/*
 * pg_rightmost_one_pos64
 *		As above, but for a 64-bit word.
 */
static inline int
pg_rightmost_one_pos64(uint64 word)
{
#ifdef HAVE__BUILTIN_CTZ
	Assert(word != 0);

#if defined(HAVE_LONG_INT_64)
	return __builtin_ctzl(word);
#elif defined(HAVE_LONG_LONG_INT_64)
	return __builtin_ctzll(word);
#else
#error must have a working 64-bit integer datatype
#endif
#else							/* !HAVE__BUILTIN_CTZ */
	int			result = 0;

	Assert(word != 0);

	while ((word & 255) == 0)
	{
		word >>= 8;
		result += 8;
	}
	result += pg_rightmost_one_pos[word & 255];
	return result;
#endif							/* HAVE__BUILTIN_CTZ */
}

/*
 * pg_nextpower2_32
 *		Returns the next higher power of 2 above 'num', or 'num' if it's
 *		already a power of 2.
 *
 * 'num' mustn't be 0 or be above PG_UINT32_MAX / 2 + 1.
 */
static inline uint32
pg_nextpower2_32(uint32 num)
{
	Assert(num > 0 && num <= PG_UINT32_MAX / 2 + 1);

	/*
	 * A power 2 number has only 1 bit set.  Subtracting 1 from such a number
	 * will turn on all previous bits resulting in no common bits being set
	 * between num and num-1.
	 */
	if ((num & (num - 1)) == 0)
		return num;				/* already power 2 */

	return ((uint32) 1) << (pg_leftmost_one_pos32(num) + 1);
}

/*
 * pg_nextpower2_64
 *		Returns the next higher power of 2 above 'num', or 'num' if it's
 *		already a power of 2.
 *
 * 'num' mustn't be 0 or be above PG_UINT64_MAX / 2  + 1.
 */
static inline uint64
pg_nextpower2_64(uint64 num)
{
	Assert(num > 0 && num <= PG_UINT64_MAX / 2 + 1);

	/*
	 * A power 2 number has only 1 bit set.  Subtracting 1 from such a number
	 * will turn on all previous bits resulting in no common bits being set
	 * between num and num-1.
	 */
	if ((num & (num - 1)) == 0)
		return num;				/* already power 2 */

	return ((uint64) 1) << (pg_leftmost_one_pos64(num) + 1);
}

/*
 * pg_nextpower2_size_t
 *		Returns the next higher power of 2 above 'num', for a size_t input.
 */
#if SIZEOF_SIZE_T == 4
#define pg_nextpower2_size_t(num) pg_nextpower2_32(num)
#else
#define pg_nextpower2_size_t(num) pg_nextpower2_64(num)
#endif

/*
 * pg_prevpower2_32
 *		Returns the next lower power of 2 below 'num', or 'num' if it's
 *		already a power of 2.
 *
 * 'num' mustn't be 0.
 */
static inline uint32
pg_prevpower2_32(uint32 num)
{
	return ((uint32) 1) << pg_leftmost_one_pos32(num);
}

/*
 * pg_prevpower2_64
 *		Returns the next lower power of 2 below 'num', or 'num' if it's
 *		already a power of 2.
 *
 * 'num' mustn't be 0.
 */
static inline uint64
pg_prevpower2_64(uint64 num)
{
	return ((uint64) 1) << pg_leftmost_one_pos64(num);
}

/*
 * pg_prevpower2_size_t
 *		Returns the next lower power of 2 below 'num', for a size_t input.
 */
#if SIZEOF_SIZE_T == 4
#define pg_prevpower2_size_t(num) pg_prevpower2_32(num)
#else
#define pg_prevpower2_size_t(num) pg_prevpower2_64(num)
#endif

/*
 * pg_ceil_log2_32
 *		Returns equivalent of ceil(log2(num))
 */
static inline uint32
pg_ceil_log2_32(uint32 num)
{
	if (num < 2)
		return 0;
	else
		return pg_leftmost_one_pos32(num - 1) + 1;
}

/*
 * pg_ceil_log2_64
 *		Returns equivalent of ceil(log2(num))
 */
static inline uint64
pg_ceil_log2_64(uint64 num)
{
	if (num < 2)
		return 0;
	else
		return pg_leftmost_one_pos64(num - 1) + 1;
}

/* Count the number of one-bits in a uint32 or uint64 */
extern int	(*pg_popcount32) (uint32 word);
extern int	(*pg_popcount64) (uint64 word);

/* Count the number of one-bits in a byte array */
extern uint64 pg_popcount(const char *buf, int bytes);

/*
 * Rotate the bits of "word" to the right by n bits.
 */
static inline uint32
pg_rotate_right32(uint32 word, int n)
{
	return (word >> n) | (word << (sizeof(word) * BITS_PER_BYTE - n));
}

#endif							/* PG_BITUTILS_H */
