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

#ifdef HAVE__BUILTIN_POPCOUNT
static bool pg_popcount_available(void);
static int	pg_popcount32_choose(uint32 word);
static int	pg_popcount32_builtin(uint32 word);
static int	pg_popcount64_choose(uint64 word);
static int	pg_popcount64_builtin(uint64 word);
int			(*pg_popcount32) (uint32 word) = pg_popcount32_choose;
int			(*pg_popcount64) (uint64 word) = pg_popcount64_choose;
#else
static int	pg_popcount32_slow(uint32 word);
static int	pg_popcount64_slow(uint64 word);
int			(*pg_popcount32) (uint32 word) = pg_popcount32_slow;
int			(*pg_popcount64) (uint64 word) = pg_popcount64_slow;
#endif							/* !HAVE_BUILTIN_POPCOUNT */


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
 * Return true iff we have CPUID support and it indicates that the POPCNT
 * instruction is available.
 */
static bool
pg_popcount_available(void)
{
#if defined(HAVE__GET_CPUID) || defined(HAVE__CPUID)
	unsigned int exx[4] = {0, 0, 0, 0};

#if defined(HAVE__GET_CPUID)
	__get_cpuid(1, &exx[0], &exx[1], &exx[2], &exx[3]);
#elif defined(HAVE__CPUID)
	__cpuid(exx, 1);
#endif

	return (exx[2] & (1 << 23)) != 0;	/* POPCNT */
#else							/* HAVE__GET_CPUID || HAVE__CPUID */

	return false;
#endif
}

#ifdef HAVE__BUILTIN_POPCOUNT
/*
 * This gets called on the first call to pg_popcount32. It replaces the
 * function pointer so that subsequent calls are routed directly to the chosen
 * implementation.
 */
static int
pg_popcount32_choose(uint32 word)
{
	if (pg_popcount_available())
		pg_popcount32 = pg_popcount32_hw;
	else
		pg_popcount32 = pg_popcount32_builtin;

	return pg_popcount32(word);
}

static int
pg_popcount32_builtin(uint32 word)
{
	return __builtin_popcount(word);
}
#else							/* HAVE__BUILTIN_POPCOUNT */
/*
 * pg_popcount32_slow
 *		Return the number of 1 bits set in word
 */
static int
pg_popcount32_slow(uint32 word)
{
	int			result = 0;

	while (word != 0)
	{
		result += number_of_ones[word & 255];
		word >>= 8;
	}

	return result;
}
#endif

/*
 * pg_popcount
 *		Returns the number of 1-bits in buf
 */
uint64
pg_popcount(const char *buf, int bytes)
{
	uint64		popcnt = 0;

#if SIZEOF_VOID_P >= 8
	/* Process in 64-bit chunks if the buffer is aligned. */
	if (buf == (char *) TYPEALIGN(8, buf))
	{
		uint64	   *words = (uint64 *) buf;

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
		uint32	   *words = (uint32 *) buf;

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

#ifdef HAVE__BUILTIN_POPCOUNT
/*
 * This gets called on the first call to pg_popcount64. It replaces the
 * function pointer so that subsequent calls are routed directly to the chosen
 * implementation.
 */
static int
pg_popcount64_choose(uint64 word)
{
	if (pg_popcount_available())
		pg_popcount64 = pg_popcount64_hw;
	else
		pg_popcount64 = pg_popcount64_builtin;

	return pg_popcount64(word);
}

static int
pg_popcount64_builtin(uint64 word)
{
#if defined(HAVE_LONG_INT_64)
	return __builtin_popcountl(word);
#elif defined(HAVE_LONG_LONG_INT_64)
	return __builtin_popcountll(word);
#else
#error must have a working 64-bit integer datatype
#endif
}

#else							/* HAVE__BUILTIN_POPCOUNT */
/*
 * pg_popcount64_slow
 *		Return the number of 1 bits set in word
 */
static int
pg_popcount64_slow(uint64 word)
{
	int			result = 0;

	while (word != 0)
	{
		result += number_of_ones[word & 255];
		word >>= 8;
	}

	return result;
}
#endif
