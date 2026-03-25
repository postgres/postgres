/*-------------------------------------------------------------------------
 *
 * pg_cpu_x86.c
 *	  Runtime CPU feature detection for x86
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/pg_cpu_x86.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#if defined(USE_SSE2) || defined(__i386__)

#if defined(HAVE__GET_CPUID) || defined(HAVE__GET_CPUID_COUNT)
#include <cpuid.h>
#endif

#if defined(HAVE__CPUID) || defined(HAVE__CPUIDEX)
#include <intrin.h>
#endif

#ifdef HAVE_XSAVE_INTRINSICS
#include <immintrin.h>
#endif

#include "port/pg_cpu.h"

/*
 * XSAVE state component bits that we need
 *
 * https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-1-manual.pdf
 * Chapter "MANAGING STATE USING THE XSAVE FEATURE SET"
 */
#define XMM			(1<<1)
#define YMM			(1<<2)
#define OPMASK		(1<<5)
#define ZMM0_15		(1<<6)
#define ZMM16_31	(1<<7)


/* array indexed by enum X86FeatureId */
bool		X86Features[X86FeaturesSize] = {0};

static bool
mask_available(uint32 value, uint32 mask)
{
	return (value & mask) == mask;
}

/* Named indexes for CPUID register array */
#define EAX 0
#define EBX 1
#define ECX 2
#define EDX 3

/*
 * Request CPUID information for the specified leaf.
 */
static inline void
pg_cpuid(int leaf, unsigned int *reg)
{
#if defined(HAVE__GET_CPUID)
	__get_cpuid(leaf, &reg[EAX], &reg[EBX], &reg[ECX], &reg[EDX]);
#elif defined(HAVE__CPUID)
	__cpuid((int *) reg, leaf);
#else
#error cpuid instruction not available
#endif
}

/*
 * Request CPUID information for the specified leaf and subleaf.
 *
 * Returns true if the CPUID leaf/subleaf is supported, false otherwise.
 */
static inline bool
pg_cpuid_subleaf(int leaf, int subleaf, unsigned int *reg)
{
#if defined(HAVE__GET_CPUID_COUNT)
	return __get_cpuid_count(leaf, subleaf, &reg[EAX], &reg[EBX], &reg[ECX], &reg[EDX]) == 1;
#elif defined(HAVE__CPUIDEX)
	__cpuidex((int *) reg, leaf, subleaf);
	return true;
#else
	memset(reg, 0, 4 * sizeof(unsigned int));
	return false;
#endif
}

/*
 * Parse the CPU ID info for runtime checks.
 */
#ifdef HAVE_XSAVE_INTRINSICS
pg_attribute_target("xsave")
#endif
void
set_x86_features(void)
{
	unsigned int reg[4] = {0};

	pg_cpuid(0x01, reg);

	X86Features[PG_SSE4_2] = reg[ECX] >> 20 & 1;
	X86Features[PG_POPCNT] = reg[ECX] >> 23 & 1;

	/* leaf 7 features that depend on OSXSAVE */
	if (reg[ECX] & (1 << 27))
	{
		uint32		xcr0_val = 0;

		pg_cpuid_subleaf(0x07, 0, reg);

#ifdef HAVE_XSAVE_INTRINSICS
		/* get value of Extended Control Register */
		xcr0_val = _xgetbv(0);
#endif

		/* Are ZMM registers enabled? */
		if (mask_available(xcr0_val, XMM | YMM |
						   OPMASK | ZMM0_15 | ZMM16_31))
		{
			X86Features[PG_AVX512_BW] = reg[EBX] >> 30 & 1;
			X86Features[PG_AVX512_VL] = reg[EBX] >> 31 & 1;

			X86Features[PG_AVX512_VPCLMULQDQ] = reg[ECX] >> 10 & 1;
			X86Features[PG_AVX512_VPOPCNTDQ] = reg[ECX] >> 14 & 1;
		}
	}

	X86Features[INIT_PG_X86] = true;
}

#endif							/* defined(USE_SSE2) || defined(__i386__) */
