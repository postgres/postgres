/*-------------------------------------------------------------------------
 *
 * pg_crc32c.h
 *	  Routines for computing CRC-32C checksums.
 *
 * The speed of CRC-32C calculation has a big impact on performance, so we
 * jump through some hoops to get the best implementation for each
 * platform. Some CPU architectures have special instructions for speeding
 * up CRC calculations (e.g. Intel SSE 4.2), on other platforms we use the
 * Slicing-by-8 algorithm which uses lookup tables.
 *
 * The public interface consists of four macros:
 *
 * INIT_CRC32C(crc)
 *		Initialize a CRC accumulator
 *
 * COMP_CRC32C(crc, data, len)
 *		Accumulate some (more) bytes into a CRC
 *
 * FIN_CRC32C(crc)
 *		Finish a CRC calculation
 *
 * EQ_CRC32C(c1, c2)
 *		Check for equality of two CRCs.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/port/pg_crc32c.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_CRC32C_H
#define PG_CRC32C_H

#include "port/pg_bswap.h"

typedef uint32 pg_crc32c;

/* The INIT and EQ macros are the same for all implementations. */
#define INIT_CRC32C(crc) ((crc) = 0xFFFFFFFF)
#define EQ_CRC32C(c1, c2) ((c1) == (c2))

#if defined(USE_SSE42_CRC32C)
/*
 * Use either Intel SSE 4.2 or AVX-512 instructions. We don't need a runtime check
 * for SSE 4.2, so we can inline those in some cases.
 */

#include <nmmintrin.h>

#define COMP_CRC32C(crc, data, len) \
	((crc) = pg_comp_crc32c_dispatch((crc), (data), (len)))
#define FIN_CRC32C(crc) ((crc) ^= 0xFFFFFFFF)

extern pg_crc32c (*pg_comp_crc32c) (pg_crc32c crc, const void *data, size_t len);
extern pg_crc32c pg_comp_crc32c_sse42(pg_crc32c crc, const void *data, size_t len);
#ifdef USE_AVX512_CRC32C_WITH_RUNTIME_CHECK
extern pg_crc32c pg_comp_crc32c_avx512(pg_crc32c crc, const void *data, size_t len);
#endif

/*
 * We can only get here if the host compiler targets SSE 4.2, but on some
 * systems gcc and clang don't have the same built-in targets, so we still
 * must use a function attribute here to accommodate "--with-llvm" builds.
 */
pg_attribute_no_sanitize_alignment()
pg_attribute_target("sse4.2")
static inline
pg_crc32c
pg_comp_crc32c_dispatch(pg_crc32c crc, const void *data, size_t len)
{
	if (__builtin_constant_p(len) && len < 32)
	{
		const unsigned char *p = data;

		/*
		 * For small constant inputs, inline the computation to avoid a
		 * function call and allow the compiler to unroll loops.
		 */
#if SIZEOF_VOID_P >= 8
		for (; len >= 8; p += 8, len -= 8)
			crc = _mm_crc32_u64(crc, *(const uint64 *) p);
#endif
		for (; len >= 4; p += 4, len -= 4)
			crc = _mm_crc32_u32(crc, *(const uint32 *) p);
		for (; len > 0; --len)
			crc = _mm_crc32_u8(crc, *p++);
		return crc;
	}
	else
		/* Otherwise, use a runtime check for AVX-512 instructions. */
		return pg_comp_crc32c(crc, data, len);
}

#elif defined(USE_SSE42_CRC32C_WITH_RUNTIME_CHECK)

/*
 * Use Intel SSE 4.2 or AVX-512 instructions, but perform a runtime check first
 * to check that they are available.
 */
#define COMP_CRC32C(crc, data, len) \
	((crc) = pg_comp_crc32c((crc), (data), (len)))
#define FIN_CRC32C(crc) ((crc) ^= 0xFFFFFFFF)

extern pg_crc32c pg_comp_crc32c_sb8(pg_crc32c crc, const void *data, size_t len);
extern PGDLLIMPORT pg_crc32c (*pg_comp_crc32c) (pg_crc32c crc, const void *data, size_t len);
extern pg_crc32c pg_comp_crc32c_sse42(pg_crc32c crc, const void *data, size_t len);
#ifdef USE_AVX512_CRC32C_WITH_RUNTIME_CHECK
extern pg_crc32c pg_comp_crc32c_avx512(pg_crc32c crc, const void *data, size_t len);
#endif

#elif defined(USE_ARMV8_CRC32C)
/* Use ARMv8 CRC Extension instructions. */

#define COMP_CRC32C(crc, data, len)							\
	((crc) = pg_comp_crc32c_armv8((crc), (data), (len)))
#define FIN_CRC32C(crc) ((crc) ^= 0xFFFFFFFF)

extern pg_crc32c pg_comp_crc32c_armv8(pg_crc32c crc, const void *data, size_t len);

#elif defined(USE_LOONGARCH_CRC32C)
/* Use LoongArch CRCC instructions. */

#define COMP_CRC32C(crc, data, len)							\
	((crc) = pg_comp_crc32c_loongarch((crc), (data), (len)))
#define FIN_CRC32C(crc) ((crc) ^= 0xFFFFFFFF)

extern pg_crc32c pg_comp_crc32c_loongarch(pg_crc32c crc, const void *data, size_t len);

#elif defined(USE_ARMV8_CRC32C_WITH_RUNTIME_CHECK)

/*
 * Use ARMv8 instructions, but perform a runtime check first
 * to check that they are available.
 */
#define COMP_CRC32C(crc, data, len) \
	((crc) = pg_comp_crc32c((crc), (data), (len)))
#define FIN_CRC32C(crc) ((crc) ^= 0xFFFFFFFF)

extern pg_crc32c pg_comp_crc32c_sb8(pg_crc32c crc, const void *data, size_t len);
extern pg_crc32c (*pg_comp_crc32c) (pg_crc32c crc, const void *data, size_t len);
extern pg_crc32c pg_comp_crc32c_armv8(pg_crc32c crc, const void *data, size_t len);

#else
/*
 * Use slicing-by-8 algorithm.
 *
 * On big-endian systems, the intermediate value is kept in reverse byte
 * order, to avoid byte-swapping during the calculation. FIN_CRC32C reverses
 * the bytes to the final order.
 */
#define COMP_CRC32C(crc, data, len) \
	((crc) = pg_comp_crc32c_sb8((crc), (data), (len)))
#ifdef WORDS_BIGENDIAN
#define FIN_CRC32C(crc) ((crc) = pg_bswap32(crc) ^ 0xFFFFFFFF)
#else
#define FIN_CRC32C(crc) ((crc) ^= 0xFFFFFFFF)
#endif

extern pg_crc32c pg_comp_crc32c_sb8(pg_crc32c crc, const void *data, size_t len);

#endif

#endif							/* PG_CRC32C_H */
