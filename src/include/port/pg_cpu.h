/*-------------------------------------------------------------------------
 *
 * pg_cpu.h
 *	  Runtime CPU feature detection
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/port/pg_cpu.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_CPU_H
#define PG_CPU_H

#if defined(USE_SSE2) || defined(__i386__)

typedef enum X86FeatureId
{
	/* Have we run feature detection? */
	INIT_PG_X86,

	/* scalar registers and 128-bit XMM registers */
	PG_SSE4_2,
	PG_POPCNT,

	/* 256-bit YMM registers */
	PG_AVX2,

	/* 512-bit ZMM registers */
	PG_AVX512_BW,
	PG_AVX512_VL,
	PG_AVX512_VPCLMULQDQ,
	PG_AVX512_VPOPCNTDQ,

	/* identification */
	PG_HYPERVISOR,

	/* Time-Stamp Counter (TSC) flags */
	PG_RDTSCP,
	PG_TSC_INVARIANT,
	PG_TSC_ADJUST,
} X86FeatureId;
#define X86FeaturesSize (PG_TSC_ADJUST + 1)

extern PGDLLIMPORT bool X86Features[];

extern void set_x86_features(void);

static inline bool
x86_feature_available(X86FeatureId feature)
{
	if (X86Features[INIT_PG_X86] == false)
		set_x86_features();

	return X86Features[feature];
}

extern uint32 x86_tsc_frequency_khz(void);

#endif							/* defined(USE_SSE2) || defined(__i386__) */

#endif							/* PG_CPU_H */
