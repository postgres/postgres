/*-------------------------------------------------------------------------
 *
 * pg_popcount_avx512.c
 *	  Holds the AVX-512 pg_popcount() implementation.
 *
 * Copyright (c) 2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/port/pg_popcount_avx512.c
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include <immintrin.h>

#include "port/pg_bitutils.h"

/*
 * It's probably unlikely that TRY_POPCNT_FAST won't be set if we are able to
 * use AVX-512 intrinsics, but we check it anyway to be sure.  We piggy-back on
 * the function pointers that are only used when TRY_POPCNT_FAST is set.
 */
#ifdef TRY_POPCNT_FAST

/*
 * pg_popcount_avx512
 *		Returns the number of 1-bits in buf
 */
uint64
pg_popcount_avx512(const char *buf, int bytes)
{
	__m512i		val,
				cnt;
	__m512i		accum = _mm512_setzero_si512();
	const char *final;
	int			tail_idx;
	__mmask64	mask = ~UINT64CONST(0);

	/*
	 * Align buffer down to avoid double load overhead from unaligned access.
	 * Calculate a mask to ignore preceding bytes.  Find start offset of final
	 * iteration and ensure it is not empty.
	 */
	mask <<= ((uintptr_t) buf) % sizeof(__m512i);
	tail_idx = (((uintptr_t) buf + bytes - 1) % sizeof(__m512i)) + 1;
	final = (const char *) TYPEALIGN_DOWN(sizeof(__m512i), buf + bytes - 1);
	buf = (const char *) TYPEALIGN_DOWN(sizeof(__m512i), buf);

	/*
	 * Iterate through all but the final iteration.  Starting from the second
	 * iteration, the mask is ignored.
	 */
	if (buf < final)
	{
		val = _mm512_maskz_loadu_epi8(mask, (const __m512i *) buf);
		cnt = _mm512_popcnt_epi64(val);
		accum = _mm512_add_epi64(accum, cnt);

		buf += sizeof(__m512i);
		mask = ~UINT64CONST(0);

		for (; buf < final; buf += sizeof(__m512i))
		{
			val = _mm512_load_si512((const __m512i *) buf);
			cnt = _mm512_popcnt_epi64(val);
			accum = _mm512_add_epi64(accum, cnt);
		}
	}

	/* Final iteration needs to ignore bytes that are not within the length */
	mask &= (~UINT64CONST(0) >> (sizeof(__m512i) - tail_idx));

	val = _mm512_maskz_loadu_epi8(mask, (const __m512i *) buf);
	cnt = _mm512_popcnt_epi64(val);
	accum = _mm512_add_epi64(accum, cnt);

	return _mm512_reduce_add_epi64(accum);
}

/*
 * pg_popcount_masked_avx512
 *		Returns the number of 1-bits in buf after applying the mask to each byte
 */
uint64
pg_popcount_masked_avx512(const char *buf, int bytes, bits8 mask)
{
	__m512i		val,
				vmasked,
				cnt;
	__m512i		accum = _mm512_setzero_si512();
	const char *final;
	int			tail_idx;
	__mmask64	bmask = ~UINT64CONST(0);
	const __m512i maskv = _mm512_set1_epi8(mask);

	/*
	 * Align buffer down to avoid double load overhead from unaligned access.
	 * Calculate a mask to ignore preceding bytes.  Find start offset of final
	 * iteration and ensure it is not empty.
	 */
	bmask <<= ((uintptr_t) buf) % sizeof(__m512i);
	tail_idx = (((uintptr_t) buf + bytes - 1) % sizeof(__m512i)) + 1;
	final = (const char *) TYPEALIGN_DOWN(sizeof(__m512i), buf + bytes - 1);
	buf = (const char *) TYPEALIGN_DOWN(sizeof(__m512i), buf);

	/*
	 * Iterate through all but the final iteration.  Starting from the second
	 * iteration, the mask is ignored.
	 */
	if (buf < final)
	{
		val = _mm512_maskz_loadu_epi8(bmask, (const __m512i *) buf);
		vmasked = _mm512_and_si512(val, maskv);
		cnt = _mm512_popcnt_epi64(vmasked);
		accum = _mm512_add_epi64(accum, cnt);

		buf += sizeof(__m512i);
		bmask = ~UINT64CONST(0);

		for (; buf < final; buf += sizeof(__m512i))
		{
			val = _mm512_load_si512((const __m512i *) buf);
			vmasked = _mm512_and_si512(val, maskv);
			cnt = _mm512_popcnt_epi64(vmasked);
			accum = _mm512_add_epi64(accum, cnt);
		}
	}

	/* Final iteration needs to ignore bytes that are not within the length */
	bmask &= (~UINT64CONST(0) >> (sizeof(__m512i) - tail_idx));

	val = _mm512_maskz_loadu_epi8(bmask, (const __m512i *) buf);
	vmasked = _mm512_and_si512(val, maskv);
	cnt = _mm512_popcnt_epi64(vmasked);
	accum = _mm512_add_epi64(accum, cnt);

	return _mm512_reduce_add_epi64(accum);
}

#endif							/* TRY_POPCNT_FAST */
