/*-------------------------------------------------------------------------
 *
 * pg_popcount_avx512_choose.c
 *    Test whether we can use the AVX-512 pg_popcount() implementation.
 *
 * Copyright (c) 2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    src/port/pg_popcount_avx512_choose.c
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"

#if defined(HAVE__GET_CPUID) || defined(HAVE__GET_CPUID_COUNT)
#include <cpuid.h>
#endif

#ifdef HAVE_XSAVE_INTRINSICS
#include <immintrin.h>
#endif

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

#endif							/* TRY_POPCNT_FAST */
