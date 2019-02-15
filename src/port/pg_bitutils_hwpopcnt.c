/*-------------------------------------------------------------------------
 *
 * pg_bitutils_hwpopcnt.c
 *	  CPU-optimized implementation of pg_popcount variants
 *
 * This file must be compiled with a compiler-specific flag to enable the
 * POPCNT instruction.
 *
 * Portions Copyright (c) 2019, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/port/pg_bitutils_hwpopcnt.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "port/pg_bitutils.h"

int
pg_popcount32_hw(uint32 word)
{
	return __builtin_popcount(word);
}

int
pg_popcount64_hw(uint64 word)
{
#if defined(HAVE_LONG_INT_64)
	return __builtin_popcountl(word);
#elif defined(HAVE_LONG_LONG_INT_64)
	return __builtin_popcountll(word);
#else
#error must have a working 64-bit integer datatype
#endif
}
