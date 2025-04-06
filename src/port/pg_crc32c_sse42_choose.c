/*-------------------------------------------------------------------------
 *
 * pg_crc32c_sse42_choose.c
 *	  Choose between Intel SSE 4.2 and software CRC-32C implementation.
 *
 * On first call, checks if the CPU we're running on supports Intel SSE
 * 4.2. If it does, use the special SSE instructions for CRC-32C
 * computation. Otherwise, fall back to the pure software implementation
 * (slicing-by-8).
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/pg_crc32c_sse42_choose.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#if defined(HAVE__GET_CPUID) || defined(HAVE__GET_CPUID_COUNT)
#include <cpuid.h>
#endif

#if defined(HAVE__CPUID) || defined(HAVE__CPUIDEX)
#include <intrin.h>
#endif

#ifdef HAVE_XSAVE_INTRINSICS
#include <immintrin.h>
#endif

#include "port/pg_crc32c.h"

/*
 * Does XGETBV say the ZMM registers are enabled?
 *
 * NB: Caller is responsible for verifying that osxsave is available
 * before calling this.
 */
#ifdef HAVE_XSAVE_INTRINSICS
pg_attribute_target("xsave")
#endif
static bool
zmm_regs_available(void)
{
#ifdef HAVE_XSAVE_INTRINSICS
	return (_xgetbv(0) & 0xe6) == 0xe6;
#else
	return false;
#endif
}

/*
 * This gets called on the first call. It replaces the function pointer
 * so that subsequent calls are routed directly to the chosen implementation.
 */
static pg_crc32c
pg_comp_crc32c_choose(pg_crc32c crc, const void *data, size_t len)
{
	unsigned int exx[4] = {0, 0, 0, 0};

	/*
	 * Set fallback. We must guard since slicing-by-8 is not visible
	 * everywhere.
	 */
#ifdef USE_SSE42_CRC32C_WITH_RUNTIME_CHECK
	pg_comp_crc32c = pg_comp_crc32c_sb8;
#endif

#if defined(HAVE__GET_CPUID)
	__get_cpuid(1, &exx[0], &exx[1], &exx[2], &exx[3]);
#elif defined(HAVE__CPUID)
	__cpuid(exx, 1);
#else
#error cpuid instruction not available
#endif

	if ((exx[2] & (1 << 20)) != 0)	/* SSE 4.2 */
	{
		pg_comp_crc32c = pg_comp_crc32c_sse42;

		if (exx[2] & (1 << 27) &&	/* OSXSAVE */
			zmm_regs_available())
		{
			/* second cpuid call on leaf 7 to check extended AVX-512 support */

			memset(exx, 0, 4 * sizeof(exx[0]));

#if defined(HAVE__GET_CPUID_COUNT)
			__get_cpuid_count(7, 0, &exx[0], &exx[1], &exx[2], &exx[3]);
#elif defined(HAVE__CPUIDEX)
			__cpuidex(exx, 7, 0);
#endif

#ifdef USE_AVX512_CRC32C_WITH_RUNTIME_CHECK
			if (exx[2] & (1 << 10) &&	/* VPCLMULQDQ */
				exx[1] & (1 << 31)) /* AVX512-VL */
				pg_comp_crc32c = pg_comp_crc32c_avx512;
#endif
		}
	}

	return pg_comp_crc32c(crc, data, len);
}

pg_crc32c	(*pg_comp_crc32c) (pg_crc32c crc, const void *data, size_t len) = pg_comp_crc32c_choose;
