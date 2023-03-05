/*-------------------------------------------------------------------------
 *
 * simd.h
 *	  Support for platform-specific vector operations.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/port/simd.h
 *
 * NOTES
 * - VectorN in this file refers to a register where the element operands
 * are N bits wide. The vector width is platform-specific, so users that care
 * about that will need to inspect "sizeof(VectorN)".
 *
 *-------------------------------------------------------------------------
 */
#ifndef SIMD_H
#define SIMD_H

#if (defined(__x86_64__) || defined(_M_AMD64))
/*
 * SSE2 instructions are part of the spec for the 64-bit x86 ISA. We assume
 * that compilers targeting this architecture understand SSE2 intrinsics.
 *
 * We use emmintrin.h rather than the comprehensive header immintrin.h in
 * order to exclude extensions beyond SSE2. This is because MSVC, at least,
 * will allow the use of intrinsics that haven't been enabled at compile
 * time.
 */
#include <emmintrin.h>
#define USE_SSE2
typedef __m128i Vector8;
typedef __m128i Vector32;

#elif defined(__aarch64__) && defined(__ARM_NEON)
/*
 * We use the Neon instructions if the compiler provides access to them (as
 * indicated by __ARM_NEON) and we are on aarch64.  While Neon support is
 * technically optional for aarch64, it appears that all available 64-bit
 * hardware does have it.  Neon exists in some 32-bit hardware too, but we
 * could not realistically use it there without a run-time check, which seems
 * not worth the trouble for now.
 */
#include <arm_neon.h>
#define USE_NEON
typedef uint8x16_t Vector8;
typedef uint32x4_t Vector32;

#else
/*
 * If no SIMD instructions are available, we can in some cases emulate vector
 * operations using bitwise operations on unsigned integers.  Note that many
 * of the functions in this file presently do not have non-SIMD
 * implementations.  In particular, none of the functions involving Vector32
 * are implemented without SIMD since it's likely not worthwhile to represent
 * two 32-bit integers using a uint64.
 */
#define USE_NO_SIMD
typedef uint64 Vector8;
#endif

/* load/store operations */
static inline void vector8_load(Vector8 *v, const uint8 *s);
#ifndef USE_NO_SIMD
static inline void vector32_load(Vector32 *v, const uint32 *s);
#endif

/* assignment operations */
static inline Vector8 vector8_broadcast(const uint8 c);
#ifndef USE_NO_SIMD
static inline Vector32 vector32_broadcast(const uint32 c);
#endif

/* element-wise comparisons to a scalar */
static inline bool vector8_has(const Vector8 v, const uint8 c);
static inline bool vector8_has_zero(const Vector8 v);
static inline bool vector8_has_le(const Vector8 v, const uint8 c);
static inline bool vector8_is_highbit_set(const Vector8 v);
#ifndef USE_NO_SIMD
static inline bool vector32_is_highbit_set(const Vector32 v);
#endif

/* arithmetic operations */
static inline Vector8 vector8_or(const Vector8 v1, const Vector8 v2);
#ifndef USE_NO_SIMD
static inline Vector32 vector32_or(const Vector32 v1, const Vector32 v2);
static inline Vector8 vector8_ssub(const Vector8 v1, const Vector8 v2);
#endif

/*
 * comparisons between vectors
 *
 * Note: These return a vector rather than boolean, which is why we don't
 * have non-SIMD implementations.
 */
#ifndef USE_NO_SIMD
static inline Vector8 vector8_eq(const Vector8 v1, const Vector8 v2);
static inline Vector32 vector32_eq(const Vector32 v1, const Vector32 v2);
#endif

/*
 * Load a chunk of memory into the given vector.
 */
static inline void
vector8_load(Vector8 *v, const uint8 *s)
{
#if defined(USE_SSE2)
	*v = _mm_loadu_si128((const __m128i *) s);
#elif defined(USE_NEON)
	*v = vld1q_u8(s);
#else
	memcpy(v, s, sizeof(Vector8));
#endif
}

#ifndef USE_NO_SIMD
static inline void
vector32_load(Vector32 *v, const uint32 *s)
{
#ifdef USE_SSE2
	*v = _mm_loadu_si128((const __m128i *) s);
#elif defined(USE_NEON)
	*v = vld1q_u32(s);
#endif
}
#endif							/* ! USE_NO_SIMD */

/*
 * Create a vector with all elements set to the same value.
 */
static inline Vector8
vector8_broadcast(const uint8 c)
{
#if defined(USE_SSE2)
	return _mm_set1_epi8(c);
#elif defined(USE_NEON)
	return vdupq_n_u8(c);
#else
	return ~UINT64CONST(0) / 0xFF * c;
#endif
}

#ifndef USE_NO_SIMD
static inline Vector32
vector32_broadcast(const uint32 c)
{
#ifdef USE_SSE2
	return _mm_set1_epi32(c);
#elif defined(USE_NEON)
	return vdupq_n_u32(c);
#endif
}
#endif							/* ! USE_NO_SIMD */

/*
 * Return true if any elements in the vector are equal to the given scalar.
 */
static inline bool
vector8_has(const Vector8 v, const uint8 c)
{
	bool		result;

	/* pre-compute the result for assert checking */
#ifdef USE_ASSERT_CHECKING
	bool		assert_result = false;

	for (Size i = 0; i < sizeof(Vector8); i++)
	{
		if (((const uint8 *) &v)[i] == c)
		{
			assert_result = true;
			break;
		}
	}
#endif							/* USE_ASSERT_CHECKING */

#if defined(USE_NO_SIMD)
	/* any bytes in v equal to c will evaluate to zero via XOR */
	result = vector8_has_zero(v ^ vector8_broadcast(c));
#else
	result = vector8_is_highbit_set(vector8_eq(v, vector8_broadcast(c)));
#endif

	Assert(assert_result == result);
	return result;
}

/*
 * Convenience function equivalent to vector8_has(v, 0)
 */
static inline bool
vector8_has_zero(const Vector8 v)
{
#if defined(USE_NO_SIMD)
	/*
	 * We cannot call vector8_has() here, because that would lead to a
	 * circular definition.
	 */
	return vector8_has_le(v, 0);
#else
	return vector8_has(v, 0);
#endif
}

/*
 * Return true if any elements in the vector are less than or equal to the
 * given scalar.
 */
static inline bool
vector8_has_le(const Vector8 v, const uint8 c)
{
	bool		result = false;

	/* pre-compute the result for assert checking */
#ifdef USE_ASSERT_CHECKING
	bool		assert_result = false;

	for (Size i = 0; i < sizeof(Vector8); i++)
	{
		if (((const uint8 *) &v)[i] <= c)
		{
			assert_result = true;
			break;
		}
	}
#endif							/* USE_ASSERT_CHECKING */

#if defined(USE_NO_SIMD)

	/*
	 * To find bytes <= c, we can use bitwise operations to find bytes < c+1,
	 * but it only works if c+1 <= 128 and if the highest bit in v is not set.
	 * Adapted from
	 * https://graphics.stanford.edu/~seander/bithacks.html#HasLessInWord
	 */
	if ((int64) v >= 0 && c < 0x80)
		result = (v - vector8_broadcast(c + 1)) & ~v & vector8_broadcast(0x80);
	else
	{
		/* one byte at a time */
		for (Size i = 0; i < sizeof(Vector8); i++)
		{
			if (((const uint8 *) &v)[i] <= c)
			{
				result = true;
				break;
			}
		}
	}
#else

	/*
	 * Use saturating subtraction to find bytes <= c, which will present as
	 * NUL bytes.  This approach is a workaround for the lack of unsigned
	 * comparison instructions on some architectures.
	 */
	result = vector8_has_zero(vector8_ssub(v, vector8_broadcast(c)));
#endif

	Assert(assert_result == result);
	return result;
}

/*
 * Return true if the high bit of any element is set
 */
static inline bool
vector8_is_highbit_set(const Vector8 v)
{
#ifdef USE_SSE2
	return _mm_movemask_epi8(v) != 0;
#elif defined(USE_NEON)
	return vmaxvq_u8(v) > 0x7F;
#else
	return v & vector8_broadcast(0x80);
#endif
}

/*
 * Exactly like vector8_is_highbit_set except for the input type, so it
 * looks at each byte separately.
 *
 * XXX x86 uses the same underlying type for 8-bit, 16-bit, and 32-bit
 * integer elements, but Arm does not, hence the need for a separate
 * function. We could instead adopt the behavior of Arm's vmaxvq_u32(), i.e.
 * check each 32-bit element, but that would require an additional mask
 * operation on x86.
 */
#ifndef USE_NO_SIMD
static inline bool
vector32_is_highbit_set(const Vector32 v)
{
#if defined(USE_NEON)
	return vector8_is_highbit_set((Vector8) v);
#else
	return vector8_is_highbit_set(v);
#endif
}
#endif							/* ! USE_NO_SIMD */

/*
 * Return the bitwise OR of the inputs
 */
static inline Vector8
vector8_or(const Vector8 v1, const Vector8 v2)
{
#ifdef USE_SSE2
	return _mm_or_si128(v1, v2);
#elif defined(USE_NEON)
	return vorrq_u8(v1, v2);
#else
	return v1 | v2;
#endif
}

#ifndef USE_NO_SIMD
static inline Vector32
vector32_or(const Vector32 v1, const Vector32 v2)
{
#ifdef USE_SSE2
	return _mm_or_si128(v1, v2);
#elif defined(USE_NEON)
	return vorrq_u32(v1, v2);
#endif
}
#endif							/* ! USE_NO_SIMD */

/*
 * Return the result of subtracting the respective elements of the input
 * vectors using saturation (i.e., if the operation would yield a value less
 * than zero, zero is returned instead).  For more information on saturation
 * arithmetic, see https://en.wikipedia.org/wiki/Saturation_arithmetic
 */
#ifndef USE_NO_SIMD
static inline Vector8
vector8_ssub(const Vector8 v1, const Vector8 v2)
{
#ifdef USE_SSE2
	return _mm_subs_epu8(v1, v2);
#elif defined(USE_NEON)
	return vqsubq_u8(v1, v2);
#endif
}
#endif							/* ! USE_NO_SIMD */

/*
 * Return a vector with all bits set in each lane where the corresponding
 * lanes in the inputs are equal.
 */
#ifndef USE_NO_SIMD
static inline Vector8
vector8_eq(const Vector8 v1, const Vector8 v2)
{
#ifdef USE_SSE2
	return _mm_cmpeq_epi8(v1, v2);
#elif defined(USE_NEON)
	return vceqq_u8(v1, v2);
#endif
}
#endif							/* ! USE_NO_SIMD */

#ifndef USE_NO_SIMD
static inline Vector32
vector32_eq(const Vector32 v1, const Vector32 v2)
{
#ifdef USE_SSE2
	return _mm_cmpeq_epi32(v1, v2);
#elif defined(USE_NEON)
	return vceqq_u32(v1, v2);
#endif
}
#endif							/* ! USE_NO_SIMD */

#endif							/* SIMD_H */
