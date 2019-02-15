/*------------------------------------------------------------------------ -
 *
 * pg_bitutils.h
 *	  miscellaneous functions for bit-wise operations.
  *
 *
 * Portions Copyright(c) 2019, PostgreSQL Global Development Group
 *
 * src/include/port/pg_bitutils.h
 *
 *------------------------------------------------------------------------ -
 */
#ifndef PG_BITUTILS_H
#define PG_BITUTILS_H

extern int	(*pg_popcount32) (uint32 word);
extern int	(*pg_popcount64) (uint64 word);
extern uint64 pg_popcount(const char *buf, int bytes);

/* in pg_bitutils_hwpopcnt.c */
extern int	pg_popcount32_hw(uint32 word);
extern int	pg_popcount64_hw(uint64 word);


#ifndef HAVE__BUILTIN_CTZ
/*
 * Array marking the position of the right-most set bit for each value of
 * 1-255.  We count the right-most position as the 0th bit, and the
 * left-most the 7th bit.  The 0th index of the array must not be used.
 */
static const uint8 rightmost_one_pos[256] = {
	0, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	6, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	7, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	6, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0
};
#endif							/* !HAVE__BUILTIN_CTZ */

/*
 * pg_rightmost_one32
 *		Returns the number of trailing 0-bits in word, starting at the least
 *		significant bit position. word must not be 0.
 */
static inline int
pg_rightmost_one32(uint32 word)
{
	int			result = 0;

	Assert(word != 0);

#ifdef HAVE__BUILTIN_CTZ
	result = __builtin_ctz(word);
#else
	while ((word & 255) == 0)
	{
		word >>= 8;
		result += 8;
	}
	result += rightmost_one_pos[word & 255];
#endif							/* HAVE__BUILTIN_CTZ */

	return result;
}

/*
 * pg_rightmost_one64
 *		Returns the number of trailing 0-bits in word, starting at the least
 *		significant bit position. word must not be 0.
 */
static inline int
pg_rightmost_one64(uint64 word)
{
	int			result = 0;

	Assert(word != 0);
#ifdef HAVE__BUILTIN_CTZ
#if defined(HAVE_LONG_INT_64)
	return __builtin_ctzl(word);
#elif defined(HAVE_LONG_LONG_INT_64)
	return __builtin_ctzll(word);
#else
#error must have a working 64-bit integer datatype
#endif
#else							/* HAVE__BUILTIN_CTZ */
	while ((word & 255) == 0)
	{
		word >>= 8;
		result += 8;
	}
	result += rightmost_one_pos[word & 255];
#endif

	return result;
}

#ifndef HAVE__BUILTIN_CLZ
/*
 * Array marking the position of the left-most set bit for each value of
 * 1-255.  We count the right-most position as the 0th bit, and the
 * left-most the 7th bit.  The 0th index of the array must not be used.
 */
static const uint8 leftmost_one_pos[256] = {
	0, 0, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3,
	4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
	5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
	5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
	6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
	6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
	6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
	6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
	7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
	7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
	7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
	7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
	7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
	7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
	7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
	7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7
};
#endif							/* !HAVE_BUILTIN_CLZ */

/*
 * pg_leftmost_one32
 *		Returns the 0-based position of the most significant set bit in word
 *		measured from the least significant bit.  word must not be 0.
 */
static inline int
pg_leftmost_one32(uint32 word)
{
#ifdef HAVE__BUILTIN_CLZ
	Assert(word != 0);

	return 31 - __builtin_clz(word);
#else
	int			shift = 32 - 8;

	Assert(word != 0);

	while ((word >> shift) == 0)
		shift -= 8;

	return shift + leftmost_one_pos[(word >> shift) & 255];
#endif							/* HAVE__BUILTIN_CLZ */
}

/*
 * pg_leftmost_one64
 *		Returns the 0-based position of the most significant set bit in word
 *		measured from the least significant bit.  word must not be 0.
 */
static inline int
pg_leftmost_one64(uint64 word)
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
#else							/* HAVE__BUILTIN_CLZ */
	int			shift = 64 - 8;

	Assert(word != 0);
	while ((word >> shift) == 0)
		shift -= 8;

	return shift + leftmost_one_pos[(word >> shift) & 255];
#endif							/* !HAVE__BUIILTIN_CLZ */
}

#endif							/* PG_BITUTILS_H */
