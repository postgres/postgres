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

#else
/*
 * If no SIMD instructions are available, we can in some cases emulate vector
 * operations using bitwise operations on unsigned integers.
 */
#define USE_NO_SIMD
typedef uint64 Vector8;
#endif


/* load/store operations */
static inline void vector8_load(Vector8 *v, const uint8 *s);

/* assignment operations */
static inline Vector8 vector8_broadcast(const uint8 c);

/* element-wise comparisons to a scalar */
static inline bool vector8_has(const Vector8 v, const uint8 c);
static inline bool vector8_has_zero(const Vector8 v);
static inline bool vector8_has_le(const Vector8 v, const uint8 c);


/*
 * Load a chunk of memory into the given vector.
 */
static inline void
vector8_load(Vector8 *v, const uint8 *s)
{
#if defined(USE_SSE2)
	*v = _mm_loadu_si128((const __m128i *) s);
#else
	memcpy(v, s, sizeof(Vector8));
#endif
}


/*
 * Create a vector with all elements set to the same value.
 */
static inline Vector8
vector8_broadcast(const uint8 c)
{
#if defined(USE_SSE2)
	return _mm_set1_epi8(c);
#else
	return ~UINT64CONST(0) / 0xFF * c;
#endif
}

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

	for (int i = 0; i < sizeof(Vector8); i++)
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
#elif defined(USE_SSE2)
	result = _mm_movemask_epi8(_mm_cmpeq_epi8(v, vector8_broadcast(c)));
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
	 * We cannot call vector8_has() here, because that would lead to a circular
	 * definition.
	 */
	return vector8_has_le(v, 0);
#elif defined(USE_SSE2)
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
#if defined(USE_SSE2)
	__m128i		sub;
#endif

	/* pre-compute the result for assert checking */
#ifdef USE_ASSERT_CHECKING
	bool		assert_result = false;

	for (int i = 0; i < sizeof(Vector8); i++)
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
		for (int i = 0; i < sizeof(Vector8); i++)
		{
			if (((const uint8 *) &v)[i] <= c)
			{
				result = true;
				break;
			}
		}
	}
#elif defined(USE_SSE2)

	/*
	 * Use saturating subtraction to find bytes <= c, which will present as
	 * NUL bytes in 'sub'.
	 */
	sub = _mm_subs_epu8(v, vector8_broadcast(c));
	result = vector8_has_zero(sub);
#endif

	Assert(assert_result == result);
	return result;
}

#endif							/* SIMD_H */
