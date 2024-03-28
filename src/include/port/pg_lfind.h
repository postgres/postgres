/*-------------------------------------------------------------------------
 *
 * pg_lfind.h
 *	  Optimized linear search routines using SIMD intrinsics where
 *	  available.
 *
 * Copyright (c) 2022-2024, PostgreSQL Global Development Group
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
 * pg_lfind8
 *
 * Return true if there is an element in 'base' that equals 'key', otherwise
 * return false.
 */
static inline bool
pg_lfind8(uint8 key, uint8 *base, uint32 nelem)
{
	uint32		i;

	/* round down to multiple of vector length */
	uint32		tail_idx = nelem & ~(sizeof(Vector8) - 1);
	Vector8		chunk;

	for (i = 0; i < tail_idx; i += sizeof(Vector8))
	{
		vector8_load(&chunk, &base[i]);
		if (vector8_has(chunk, key))
			return true;
	}

	/* Process the remaining elements one at a time. */
	for (; i < nelem; i++)
	{
		if (key == base[i])
			return true;
	}

	return false;
}

/*
 * pg_lfind8_le
 *
 * Return true if there is an element in 'base' that is less than or equal to
 * 'key', otherwise return false.
 */
static inline bool
pg_lfind8_le(uint8 key, uint8 *base, uint32 nelem)
{
	uint32		i;

	/* round down to multiple of vector length */
	uint32		tail_idx = nelem & ~(sizeof(Vector8) - 1);
	Vector8		chunk;

	for (i = 0; i < tail_idx; i += sizeof(Vector8))
	{
		vector8_load(&chunk, &base[i]);
		if (vector8_has_le(chunk, key))
			return true;
	}

	/* Process the remaining elements one at a time. */
	for (; i < nelem; i++)
	{
		if (base[i] <= key)
			return true;
	}

	return false;
}

/*
 * pg_lfind32_one_by_one_helper
 *
 * Searches the array of integers one-by-one.  The caller is responsible for
 * ensuring that there are at least "nelem" integers in the array.
 */
static inline bool
pg_lfind32_one_by_one_helper(uint32 key, const uint32 *base, uint32 nelem)
{
	for (uint32 i = 0; i < nelem; i++)
	{
		if (key == base[i])
			return true;
	}

	return false;
}

#ifndef USE_NO_SIMD
/*
 * pg_lfind32_simd_helper
 *
 * Searches one 4-register-block of integers.  The caller is responsible for
 * ensuring that there are at least 4-registers-worth of integers remaining.
 */
static inline bool
pg_lfind32_simd_helper(const Vector32 keys, const uint32 *base)
{
	const uint32 nelem_per_vector = sizeof(Vector32) / sizeof(uint32);
	Vector32	vals1,
				vals2,
				vals3,
				vals4,
				result1,
				result2,
				result3,
				result4,
				tmp1,
				tmp2,
				result;

	/* load the next block into 4 registers */
	vector32_load(&vals1, base);
	vector32_load(&vals2, &base[nelem_per_vector]);
	vector32_load(&vals3, &base[nelem_per_vector * 2]);
	vector32_load(&vals4, &base[nelem_per_vector * 3]);

	/* compare each value to the key */
	result1 = vector32_eq(keys, vals1);
	result2 = vector32_eq(keys, vals2);
	result3 = vector32_eq(keys, vals3);
	result4 = vector32_eq(keys, vals4);

	/* combine the results into a single variable */
	tmp1 = vector32_or(result1, result2);
	tmp2 = vector32_or(result3, result4);
	result = vector32_or(tmp1, tmp2);

	/* return whether there was a match */
	return vector32_is_highbit_set(result);
}
#endif							/* ! USE_NO_SIMD */

/*
 * pg_lfind32
 *
 * Return true if there is an element in 'base' that equals 'key', otherwise
 * return false.
 */
static inline bool
pg_lfind32(uint32 key, const uint32 *base, uint32 nelem)
{
#ifndef USE_NO_SIMD
	uint32		i = 0;

	/*
	 * For better instruction-level parallelism, each loop iteration operates
	 * on a block of four registers.
	 */
	const Vector32 keys = vector32_broadcast(key);	/* load copies of key */
	const uint32 nelem_per_vector = sizeof(Vector32) / sizeof(uint32);
	const uint32 nelem_per_iteration = 4 * nelem_per_vector;

	/* round down to multiple of elements per iteration */
	const uint32 tail_idx = nelem & ~(nelem_per_iteration - 1);

#if defined(USE_ASSERT_CHECKING)
	bool		assert_result = pg_lfind32_one_by_one_helper(key, base, nelem);
#endif

	/*
	 * If there aren't enough elements for the SIMD code, use the standard
	 * one-by-one linear search code.
	 */
	if (nelem < nelem_per_iteration)
		return pg_lfind32_one_by_one_helper(key, base, nelem);

	/*
	 * Process as many elements as possible with a block of 4 registers.
	 */
	do
	{
		if (pg_lfind32_simd_helper(keys, &base[i]))
		{
			Assert(assert_result == true);
			return true;
		}

		i += nelem_per_iteration;

	} while (i < tail_idx);

	/*
	 * Process the last 'nelem_per_iteration' elements in the array with a
	 * 4-register block.  This will cause us to check a subset of the elements
	 * more than once, but that won't affect correctness, and testing has
	 * demonstrated that this helps more cases than it harms.
	 */
	Assert(assert_result == pg_lfind32_simd_helper(keys, &base[nelem - nelem_per_iteration]));
	return pg_lfind32_simd_helper(keys, &base[nelem - nelem_per_iteration]);
#else
	/* Process the elements one at a time. */
	return pg_lfind32_one_by_one_helper(key, base, nelem);
#endif
}

#endif							/* PG_LFIND_H */
