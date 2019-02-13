/*-------------------------------------------------------------------------
 *
 * pg_bitutils.c
 *	  miscellaneous functions for bit-wise operations.
 *
 * Portions Copyright (c) 2019, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/port/pg_bitutils.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#ifdef HAVE__GET_CPUID
#include <cpuid.h>
#endif

#ifdef HAVE__CPUID
#include <intrin.h>
#endif

#include "port/pg_bitutils.h"

#if defined(HAVE__GET_CPUID) && (defined(HAVE__BUILTIN_POPCOUNT) || defined(HAVE__BUILTIN_POPCOUNTL))
static bool pg_popcount_available(void);
#endif

#if defined(HAVE__BUILTIN_POPCOUNT) && defined(HAVE__GET_CPUID)
static int pg_popcount32_choose(uint32 word);
static int pg_popcount32_sse42(uint32 word);
#endif
static int pg_popcount32_slow(uint32 word);

#if defined(HAVE__BUILTIN_POPCOUNTL) && defined(HAVE__GET_CPUID)
static int pg_popcount64_choose(uint64 word);
static int pg_popcount64_sse42(uint64 word);
#endif
static int pg_popcount64_slow(uint64 word);

#if defined(HAVE__GET_CPUID) && (defined(HAVE__BUILTIN_CTZ) || defined(HAVE__BUILTIN_CTZL) || defined(HAVE__BUILTIN_CLZ) || defined(HAVE__BUILTIN_CLZL))
static bool pg_lzcnt_available(void);
#endif

#if defined(HAVE__BUILTIN_CTZ) && defined(HAVE__GET_CPUID)
static int pg_rightmost_one32_choose(uint32 word);
static int pg_rightmost_one32_abm(uint32 word);
#endif
static int pg_rightmost_one32_slow(uint32 word);

#if defined(HAVE__BUILTIN_CTZL) && defined(HAVE__GET_CPUID)
static int pg_rightmost_one64_choose(uint64 word);
static int pg_rightmost_one64_abm(uint64 word);
#endif
static int pg_rightmost_one64_slow(uint64 word);

#if defined(HAVE__BUILTIN_CLZ) && defined(HAVE__GET_CPUID)
static int pg_leftmost_one32_choose(uint32 word);
static int pg_leftmost_one32_abm(uint32 word);
#endif
static int pg_leftmost_one32_slow(uint32 word);

#if defined(HAVE__BUILTIN_CLZL) && defined(HAVE__GET_CPUID)
static int pg_leftmost_one64_choose(uint64 word);
static int pg_leftmost_one64_abm(uint64 word);
#endif
static int pg_leftmost_one64_slow(uint64 word);

#if defined(HAVE__BUILTIN_POPCOUNT) && defined(HAVE__GET_CPUID)
int (*pg_popcount32) (uint32 word) = pg_popcount32_choose;
#else
int (*pg_popcount32) (uint32 word) = pg_popcount32_slow;
#endif

#if defined(HAVE__BUILTIN_POPCOUNTL) && defined(HAVE__GET_CPUID)
int (*pg_popcount64) (uint64 word) = pg_popcount64_choose;
#else
int (*pg_popcount64) (uint64 word) = pg_popcount64_slow;
#endif

#if defined(HAVE__BUILTIN_CTZ) && defined(HAVE__GET_CPUID)
int (*pg_rightmost_one32) (uint32 word) = pg_rightmost_one32_choose;
#else
int (*pg_rightmost_one32) (uint32 word) = pg_rightmost_one32_slow;
#endif

#if defined(HAVE__BUILTIN_CTZL) && defined(HAVE__GET_CPUID)
int (*pg_rightmost_one64) (uint64 word) = pg_rightmost_one64_choose;
#else
int (*pg_rightmost_one64) (uint64 word) = pg_rightmost_one64_slow;
#endif

#if defined(HAVE__BUILTIN_CLZ) && defined(HAVE__GET_CPUID)
int (*pg_leftmost_one32) (uint32 word) = pg_leftmost_one32_choose;
#else
int (*pg_leftmost_one32) (uint32 word) = pg_leftmost_one32_slow;
#endif

#if defined(HAVE__BUILTIN_CLZL) && defined(HAVE__GET_CPUID)
int (*pg_leftmost_one64) (uint64 word) = pg_leftmost_one64_choose;
#else
int (*pg_leftmost_one64) (uint64 word) = pg_leftmost_one64_slow;
#endif


/* Array marking the number of 1-bits for each value of 0-255. */
static const uint8 number_of_ones[256] = {
	0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
	1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
	1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
	2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
	1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
	2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
	2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
	3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
	1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
	2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
	2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
	3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
	2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
	3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
	3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
	4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8
};

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

#if defined(HAVE__GET_CPUID) && (defined(HAVE__BUILTIN_POPCOUNT) || defined(HAVE__BUILTIN_POPCOUNTL))

static bool
pg_popcount_available(void)
{
	unsigned int exx[4] = { 0, 0, 0, 0 };

#if defined(HAVE__GET_CPUID)
	__get_cpuid(1, &exx[0], &exx[1], &exx[2], &exx[3]);
#elif defined(HAVE__CPUID)
	__cpuid(exx, 1);
#else
#error cpuid instruction not available
#endif

	return (exx[2] & (1 << 23)) != 0;	/* POPCNT */
}
#endif

#if defined(HAVE__GET_CPUID) && defined(HAVE__BUILTIN_POPCOUNT)

/*
 * This gets called on the first call. It replaces the function pointer
 * so that subsequent calls are routed directly to the chosen implementation.
 */
static int
pg_popcount32_choose(uint32 word)
{
	if (pg_popcount_available())
		pg_popcount32 = pg_popcount32_sse42;
	else
		pg_popcount32 = pg_popcount32_slow;

	return pg_popcount32(word);
}

static int
pg_popcount32_sse42(uint32 word)
{
	return __builtin_popcount(word);
}
#endif

/*
 * pg_popcount32_slow
 *		Return the number of 1 bits set in word
 */
static int
pg_popcount32_slow(uint32 word)
{
	int result = 0;

	while (word != 0)
	{
		result += number_of_ones[word & 255];
		word >>= 8;
	}

	return result;
}

/*
 * pg_popcount
 *		Returns the number of 1-bits in buf
 */
uint64
pg_popcount(const char *buf, int bytes)
{
	uint64 popcnt = 0;

#if SIZEOF_VOID_P >= 8
	/* Process in 64-bit chunks if the buffer is aligned. */
	if (buf == (char *) TYPEALIGN(8, buf))
	{
		uint64 *words = (uint64 *) buf;

		while (bytes >= 8)
		{
			popcnt += pg_popcount64(*words++);
			bytes -= 8;
		}

		buf = (char *) words;
	}
#else
	/* Process in 32-bit chunks if the buffer is aligned. */
	if (buf == (char *) TYPEALIGN(4, buf))
	{
		uint32 *words = (uint32 *) buf;

		while (bytes >= 4)
		{
			popcnt += pg_popcount32(*words++);
			bytes -= 4;
		}

		buf = (char *) words;
	}
#endif

	/* Process any remaining bytes */
	while (bytes--)
		popcnt += number_of_ones[(unsigned char) *buf++];

	return popcnt;
}

#if defined(HAVE__GET_CPUID) && defined(HAVE__BUILTIN_POPCOUNTL)

/*
 * This gets called on the first call. It replaces the function pointer
 * so that subsequent calls are routed directly to the chosen implementation.
 */
static int
pg_popcount64_choose(uint64 word)
{
	if (pg_popcount_available())
		pg_popcount64 = pg_popcount64_sse42;
	else
		pg_popcount64 = pg_popcount64_slow;

	return pg_popcount64(word);
}

static int
pg_popcount64_sse42(uint64 word)
{
	return __builtin_popcountl(word);
}

#endif

/*
 * pg_popcount64_slow
 *		Return the number of 1 bits set in word
 */
static int
pg_popcount64_slow(uint64 word)
{
	int result = 0;

	while (word != 0)
	{
		result += number_of_ones[word & 255];
		word >>= 8;
	}

	return result;
}

#if defined(HAVE__GET_CPUID) && (defined(HAVE__BUILTIN_CTZ) || defined(HAVE__BUILTIN_CTZL) || defined(HAVE__BUILTIN_CLZ) || defined(HAVE__BUILTIN_CLZL))

static bool
pg_lzcnt_available(void)
{

	unsigned int exx[4] = { 0, 0, 0, 0 };

#if defined(HAVE__GET_CPUID)
	__get_cpuid(0x80000001, &exx[0], &exx[1], &exx[2], &exx[3]);
#elif defined(HAVE__CPUID)
	__cpuid(exx, 0x80000001);
#else
#error cpuid instruction not available
#endif

	return (exx[2] & (1 << 5)) != 0;	/* LZCNT */
}
#endif

#if defined(HAVE__GET_CPUID) && defined(HAVE__BUILTIN_CTZ)
/*
 * This gets called on the first call. It replaces the function pointer
 * so that subsequent calls are routed directly to the chosen implementation.
 */
static int
pg_rightmost_one32_choose(uint32 word)
{
	if (pg_lzcnt_available())
		pg_rightmost_one32 = pg_rightmost_one32_abm;
	else
		pg_rightmost_one32 = pg_rightmost_one32_slow;

	return pg_rightmost_one32(word);
}

static int
pg_rightmost_one32_abm(uint32 word)
{
	return __builtin_ctz(word);
}

#endif

/*
 * pg_rightmost_one32_slow
 *		Returns the number of trailing 0-bits in word, starting at the least
 *		significant bit position. word must not be 0.
 */
static int
pg_rightmost_one32_slow(uint32 word)
{
	int result = 0;

	Assert(word != 0);

	while ((word & 255) == 0)
	{
		word >>= 8;
		result += 8;
	}
	result += rightmost_one_pos[word & 255];

	return result;
}

#if defined(HAVE__GET_CPUID) && defined(HAVE__BUILTIN_CTZL)
/*
 * This gets called on the first call. It replaces the function pointer
 * so that subsequent calls are routed directly to the chosen implementation.
 */
static int
pg_rightmost_one64_choose(uint64 word)
{
	if (pg_lzcnt_available())
		pg_rightmost_one64 = pg_rightmost_one64_abm;
	else
		pg_rightmost_one64 = pg_rightmost_one64_slow;

	return pg_rightmost_one64(word);
}

static int
pg_rightmost_one64_abm(uint64 word)
{
	return __builtin_ctzl(word);
}
#endif

/*
 * pg_rightmost_one64_slow
 *		Returns the number of trailing 0-bits in word, starting at the least
 *		significant bit position. word must not be 0.
 */
static int
pg_rightmost_one64_slow(uint64 word)
{
	int result = 0;

	Assert(word != 0);

	while ((word & 255) == 0)
	{
		word >>= 8;
		result += 8;
	}
	result += rightmost_one_pos[word & 255];

	return result;
}

#if defined(HAVE__GET_CPUID) && defined(HAVE__BUILTIN_CLZ)
/*
 * This gets called on the first call. It replaces the function pointer
 * so that subsequent calls are routed directly to the chosen implementation.
 */
static int
pg_leftmost_one32_choose(uint32 word)
{
	if (pg_lzcnt_available())
		pg_leftmost_one32 = pg_leftmost_one32_abm;
	else
		pg_leftmost_one32 = pg_leftmost_one32_slow;

	return pg_leftmost_one32(word);
}

static int
pg_leftmost_one32_abm(uint32 word)
{
	return 31 - __builtin_clz(word);
}
#endif

/*
 * pg_leftmost_one32_slow
 *		Returns the 0-based position of the most significant set bit in word
 *		measured from the least significant bit.  word must not be 0.
 */
static int
pg_leftmost_one32_slow(uint32 word)
{
	int			shift = 32 - 8;

	Assert(word != 0);

	while ((word >> shift) == 0)
		shift -= 8;

	return shift + leftmost_one_pos[(word >> shift) & 255];
}

#if defined(HAVE__GET_CPUID) && defined(HAVE__BUILTIN_CLZL)
/*
 * This gets called on the first call. It replaces the function pointer
 * so that subsequent calls are routed directly to the chosen implementation.
 */
static int
pg_leftmost_one64_choose(uint64 word)
{
	if (pg_lzcnt_available())
		pg_leftmost_one64 = pg_leftmost_one64_abm;
	else
		pg_leftmost_one64 = pg_leftmost_one64_slow;

	return pg_leftmost_one64(word);
}

static int
pg_leftmost_one64_abm(uint64 word)
{
	return 63 - __builtin_clzl(word);
}
#endif

/*
 * pg_leftmost_one64_slow
 *		Returns the 0-based position of the most significant set bit in word
 *		measured from the least significant bit.  word must not be 0.
 */
static int
pg_leftmost_one64_slow(uint64 word)
{
	int			shift = 64 - 8;

	Assert(word != 0);

	while ((word >> shift) == 0)
		shift -= 8;

	return shift + leftmost_one_pos[(word >> shift) & 255];
}
