/*-------------------------------------------------------------------------
 *
 * pg_lfind.h
 *	  Optimized linear search routines.
 *
 * Copyright (c) 2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/include/port/pg_lfind.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_LFIND_H
#define PG_LFIND_H

#include "port/simd.h"

/*
 * pg_lfind32
 *
 * Return true if there is an element in 'base' that equals 'key', otherwise
 * return false.
 */
static inline bool
pg_lfind32(uint32 key, uint32 *base, uint32 nelem)
{
	uint32		i = 0;

	/* Use SIMD intrinsics where available. */
#ifdef USE_SSE2

	/*
	 * A 16-byte register only has four 4-byte lanes. For better
	 * instruction-level parallelism, each loop iteration operates on a block
	 * of four registers. Testing has showed this is ~40% faster than using a
	 * block of two registers.
	 */
	const		__m128i keys = _mm_set1_epi32(key); /* load 4 copies of key */
	uint32		iterations = nelem & ~0xF;	/* round down to multiple of 16 */

#if defined(USE_ASSERT_CHECKING)
	bool		assert_result = false;

	/* pre-compute the result for assert checking */
	for (i = 0; i < nelem; i++)
	{
		if (key == base[i])
		{
			assert_result = true;
			break;
		}
	}
#endif

	for (i = 0; i < iterations; i += 16)
	{
		/* load the next block into 4 registers holding 4 values each */
		const		__m128i vals1 = _mm_loadu_si128((__m128i *) & base[i]);
		const		__m128i vals2 = _mm_loadu_si128((__m128i *) & base[i + 4]);
		const		__m128i vals3 = _mm_loadu_si128((__m128i *) & base[i + 8]);
		const		__m128i vals4 = _mm_loadu_si128((__m128i *) & base[i + 12]);

		/* compare each value to the key */
		const		__m128i result1 = _mm_cmpeq_epi32(keys, vals1);
		const		__m128i result2 = _mm_cmpeq_epi32(keys, vals2);
		const		__m128i result3 = _mm_cmpeq_epi32(keys, vals3);
		const		__m128i result4 = _mm_cmpeq_epi32(keys, vals4);

		/* combine the results into a single variable */
		const		__m128i tmp1 = _mm_or_si128(result1, result2);
		const		__m128i tmp2 = _mm_or_si128(result3, result4);
		const		__m128i result = _mm_or_si128(tmp1, tmp2);

		/* see if there was a match */
		if (_mm_movemask_epi8(result) != 0)
		{
#if defined(USE_ASSERT_CHECKING)
			Assert(assert_result == true);
#endif
			return true;
		}
	}
#endif							/* USE_SSE2 */

	/* Process the remaining elements one at a time. */
	for (; i < nelem; i++)
	{
		if (key == base[i])
		{
#if defined(USE_SSE2) && defined(USE_ASSERT_CHECKING)
			Assert(assert_result == true);
#endif
			return true;
		}
	}

#if defined(USE_SSE2) && defined(USE_ASSERT_CHECKING)
	Assert(assert_result == false);
#endif
	return false;
}

#endif							/* PG_LFIND_H */
