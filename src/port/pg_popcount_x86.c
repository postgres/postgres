/*-------------------------------------------------------------------------
 *
 * pg_popcount_x86.c
 *	  Holds the x86-64 pg_popcount() implementations.
 *
 * Copyright (c) 2024-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/port/pg_popcount_x86.c
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"

#ifdef HAVE_X86_64_POPCNTQ

#if defined(HAVE__GET_CPUID) || defined(HAVE__GET_CPUID_COUNT)
#include <cpuid.h>
#endif

#ifdef USE_AVX512_POPCNT_WITH_RUNTIME_CHECK
#include <immintrin.h>
#endif

#if defined(HAVE__CPUID) || defined(HAVE__CPUIDEX)
#include <intrin.h>
#endif

#include "port/pg_bitutils.h"

/*
 * The SSE4.2 versions are built regardless of whether we are building the
 * AVX-512 versions.
 *
 * Technically, POPCNT is not part of SSE4.2, and isn't even a vector
 * operation, but in practice this is close enough, and "sse42" seems easier to
 * follow than "popcnt" for these names.
 */
static uint64 pg_popcount_sse42(const char *buf, int bytes);
static uint64 pg_popcount_masked_sse42(const char *buf, int bytes, bits8 mask);

/*
 * These are the AVX-512 implementations of the popcount functions.
 */
#ifdef USE_AVX512_POPCNT_WITH_RUNTIME_CHECK
static uint64 pg_popcount_avx512(const char *buf, int bytes);
static uint64 pg_popcount_masked_avx512(const char *buf, int bytes, bits8 mask);
#endif							/* USE_AVX512_POPCNT_WITH_RUNTIME_CHECK */

/*
 * The function pointers are initially set to "choose" functions.  These
 * functions will first set the pointers to the right implementations (base on
 * what the current CPU supports) and then will call the pointer to fulfill the
 * caller's request.
 */
static uint64 pg_popcount_choose(const char *buf, int bytes);
static uint64 pg_popcount_masked_choose(const char *buf, int bytes, bits8 mask);
uint64		(*pg_popcount_optimized) (const char *buf, int bytes) = pg_popcount_choose;
uint64		(*pg_popcount_masked_optimized) (const char *buf, int bytes, bits8 mask) = pg_popcount_masked_choose;

/*
 * Return true if CPUID indicates that the POPCNT instruction is available.
 */
static bool
pg_popcount_sse42_available(void)
{
	unsigned int exx[4] = {0, 0, 0, 0};

#if defined(HAVE__GET_CPUID)
	__get_cpuid(1, &exx[0], &exx[1], &exx[2], &exx[3]);
#elif defined(HAVE__CPUID)
	__cpuid(exx, 1);
#else
#error cpuid instruction not available
#endif

	return (exx[2] & (1 << 23)) != 0;	/* POPCNT */
}

#ifdef USE_AVX512_POPCNT_WITH_RUNTIME_CHECK

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
static bool
pg_popcount_avx512_available(void)
{
	return xsave_available() &&
		zmm_regs_available() &&
		avx512_popcnt_available();
}

#endif							/* USE_AVX512_POPCNT_WITH_RUNTIME_CHECK */

/*
 * These functions get called on the first call to pg_popcount(), etc.
 * They detect whether we can use the asm implementations, and replace
 * the function pointers so that subsequent calls are routed directly to
 * the chosen implementation.
 */
static inline void
choose_popcount_functions(void)
{
	if (pg_popcount_sse42_available())
	{
		pg_popcount_optimized = pg_popcount_sse42;
		pg_popcount_masked_optimized = pg_popcount_masked_sse42;
	}
	else
	{
		pg_popcount_optimized = pg_popcount_portable;
		pg_popcount_masked_optimized = pg_popcount_masked_portable;
	}

#ifdef USE_AVX512_POPCNT_WITH_RUNTIME_CHECK
	if (pg_popcount_avx512_available())
	{
		pg_popcount_optimized = pg_popcount_avx512;
		pg_popcount_masked_optimized = pg_popcount_masked_avx512;
	}
#endif
}

static uint64
pg_popcount_choose(const char *buf, int bytes)
{
	choose_popcount_functions();
	return pg_popcount_optimized(buf, bytes);
}

static uint64
pg_popcount_masked_choose(const char *buf, int bytes, bits8 mask)
{
	choose_popcount_functions();
	return pg_popcount_masked(buf, bytes, mask);
}

#ifdef USE_AVX512_POPCNT_WITH_RUNTIME_CHECK

/*
 * pg_popcount_avx512
 *		Returns the number of 1-bits in buf
 */
pg_attribute_target("avx512vpopcntdq,avx512bw")
static uint64
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
static uint64
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

#endif							/* USE_AVX512_POPCNT_WITH_RUNTIME_CHECK */

/*
 * pg_popcount64_sse42
 *		Return the number of 1 bits set in word
 */
static inline int
pg_popcount64_sse42(uint64 word)
{
#ifdef _MSC_VER
	return __popcnt64(word);
#else
	uint64		res;

__asm__ __volatile__(" popcntq %1,%0\n":"=q"(res):"rm"(word):"cc");
	return (int) res;
#endif
}

/*
 * pg_popcount_sse42
 *		Returns the number of 1-bits in buf
 */
pg_attribute_no_sanitize_alignment()
static uint64
pg_popcount_sse42(const char *buf, int bytes)
{
	uint64		popcnt = 0;
	const uint64 *words = (const uint64 *) buf;

	while (bytes >= 8)
	{
		popcnt += pg_popcount64_sse42(*words++);
		bytes -= 8;
	}

	buf = (const char *) words;

	/* Process any remaining bytes */
	while (bytes--)
		popcnt += pg_number_of_ones[(unsigned char) *buf++];

	return popcnt;
}

/*
 * pg_popcount_masked_sse42
 *		Returns the number of 1-bits in buf after applying the mask to each byte
 */
pg_attribute_no_sanitize_alignment()
static uint64
pg_popcount_masked_sse42(const char *buf, int bytes, bits8 mask)
{
	uint64		popcnt = 0;
	uint64		maskv = ~UINT64CONST(0) / 0xFF * mask;
	const uint64 *words = (const uint64 *) buf;

	while (bytes >= 8)
	{
		popcnt += pg_popcount64_sse42(*words++ & maskv);
		bytes -= 8;
	}

	buf = (const char *) words;

	/* Process any remaining bytes */
	while (bytes--)
		popcnt += pg_number_of_ones[(unsigned char) *buf++ & mask];

	return popcnt;
}

#endif							/* HAVE_X86_64_POPCNTQ */
