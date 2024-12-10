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

#ifdef USE_AVX512_POPCNT_WITH_RUNTIME_CHECK

#if defined(HAVE__GET_CPUID) || defined(HAVE__GET_CPUID_COUNT)
#include <cpuid.h>
#endif

#include <immintrin.h>

#if defined(HAVE__CPUID) || defined(HAVE__CPUIDEX)
#include <intrin.h>
#endif

#include "port/pg_bitutils.h"

/*
 * It's probably unlikely that TRY_POPCNT_FAST won't be set if we are able to
 * use AVX-512 intrinsics, but we check it anyway to be sure.  We piggy-back on
 * the function pointers that are only used when TRY_POPCNT_FAST is set.
 */
#ifdef TRY_POPCNT_FAST

/*
 * Does CPUID say there's support for XSAVE instructions?
 */
static inline bool
xsave_available(void)
{
	unsigned int exx[4] = {0, 0, 0, 0};

#if defined(HAVE__GET_CPUID)
	__get_cpuid(1, &exx[0], &exx[1], &exx[2], &exx[3]);
#elif defined(HAVE__CPUID)
	__cpuid(exx, 1);
#else
#error cpuid instruction not available
#endif
	return (exx[2] & (1 << 27)) != 0;	/* osxsave */
}

/*
 * Does XGETBV say the ZMM registers are enabled?
 *
 * NB: Caller is responsible for verifying that xsave_available() returns true
 * before calling this.
 */
#ifdef HAVE_XSAVE_INTRINSICS
pg_attribute_target("xsave")
#endif
static inline bool
zmm_regs_available(void)
{
#ifdef HAVE_XSAVE_INTRINSICS
	return (_xgetbv(0) & 0xe6) == 0xe6;
#else
	return false;
#endif
}

/*
 * Does CPUID say there's support for AVX-512 popcount and byte-and-word
 * instructions?
 */
static inline bool
avx512_popcnt_available(void)
{
	unsigned int exx[4] = {0, 0, 0, 0};

#if defined(HAVE__GET_CPUID_COUNT)
	__get_cpuid_count(7, 0, &exx[0], &exx[1], &exx[2], &exx[3]);
#elif defined(HAVE__CPUIDEX)
	__cpuidex(exx, 7, 0);
#else
#error cpuid instruction not available
#endif
	return (exx[2] & (1 << 14)) != 0 && /* avx512-vpopcntdq */
		(exx[1] & (1 << 30)) != 0;	/* avx512-bw */
}

/*
 * Returns true if the CPU supports the instructions required for the AVX-512
 * pg_popcount() implementation.
 */
bool
pg_popcount_avx512_available(void)
{
	return xsave_available() &&
		zmm_regs_available() &&
		avx512_popcnt_available();
}

/*
 * pg_popcount_avx512
 *		Returns the number of 1-bits in buf
 */
pg_attribute_target("avx512vpopcntdq,avx512bw")
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
pg_attribute_target("avx512vpopcntdq,avx512bw")
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
#endif							/* USE_AVX512_POPCNT_WITH_RUNTIME_CHECK */
