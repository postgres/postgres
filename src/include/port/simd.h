/*-------------------------------------------------------------------------
 *
 * simd.h
 *	  Support for platform-specific vector operations.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/port/simd.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SIMD_H
#define SIMD_H

/*
 * SSE2 instructions are part of the spec for the 64-bit x86 ISA. We assume
 * that compilers targeting this architecture understand SSE2 intrinsics.
 *
 * We use emmintrin.h rather than the comprehensive header immintrin.h in
 * order to exclude extensions beyond SSE2. This is because MSVC, at least,
 * will allow the use of intrinsics that haven't been enabled at compile
 * time.
 */
#if (defined(__x86_64__) || defined(_M_AMD64))
#include <emmintrin.h>
#define USE_SSE2
#endif

#endif							/* SIMD_H */
