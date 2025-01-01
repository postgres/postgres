/*-------------------------------------------------------------------------
 *
 * int.h
 *	  Overflow-aware integer math and integer comparison routines.
 *
 * The routines in this file are intended to be well defined C, without
 * relying on compiler flags like -fwrapv.
 *
 * To reduce the overhead of these routines try to use compiler intrinsics
 * where available. That's not that important for the 16, 32 bit cases, but
 * the 64 bit cases can be considerably faster with intrinsics. In case no
 * intrinsics are available 128 bit math is used where available.
 *
 * Copyright (c) 2017-2025, PostgreSQL Global Development Group
 *
 * src/include/common/int.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef COMMON_INT_H
#define COMMON_INT_H


/*---------
 * The following guidelines apply to all the overflow routines:
 *
 * If the result overflows, return true, otherwise store the result into
 * *result.  The content of *result is implementation defined in case of
 * overflow.
 *
 *  bool pg_add_*_overflow(a, b, *result)
 *
 *    Calculate a + b
 *
 *  bool pg_sub_*_overflow(a, b, *result)
 *
 *    Calculate a - b
 *
 *  bool pg_mul_*_overflow(a, b, *result)
 *
 *    Calculate a * b
 *
 *  bool pg_neg_*_overflow(a, *result)
 *
 *    Calculate -a
 *
 *
 * In addition, this file contains:
 *
 *  <unsigned int type> pg_abs_*(<signed int type> a)
 *
 *    Calculate absolute value of a.  Unlike the standard library abs()
 *    and labs() functions, the return type is unsigned, so the operation
 *    cannot overflow.
 *---------
 */

/*------------------------------------------------------------------------
 * Overflow routines for signed integers
 *------------------------------------------------------------------------
 */

/*
 * INT16
 */
static inline bool
pg_add_s16_overflow(int16 a, int16 b, int16 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_add_overflow(a, b, result);
#else
	int32		res = (int32) a + (int32) b;

	if (res > PG_INT16_MAX || res < PG_INT16_MIN)
	{
		*result = 0x5EED;		/* to avoid spurious warnings */
		return true;
	}
	*result = (int16) res;
	return false;
#endif
}

static inline bool
pg_sub_s16_overflow(int16 a, int16 b, int16 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_sub_overflow(a, b, result);
#else
	int32		res = (int32) a - (int32) b;

	if (res > PG_INT16_MAX || res < PG_INT16_MIN)
	{
		*result = 0x5EED;		/* to avoid spurious warnings */
		return true;
	}
	*result = (int16) res;
	return false;
#endif
}

static inline bool
pg_mul_s16_overflow(int16 a, int16 b, int16 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_mul_overflow(a, b, result);
#else
	int32		res = (int32) a * (int32) b;

	if (res > PG_INT16_MAX || res < PG_INT16_MIN)
	{
		*result = 0x5EED;		/* to avoid spurious warnings */
		return true;
	}
	*result = (int16) res;
	return false;
#endif
}

static inline bool
pg_neg_s16_overflow(int16 a, int16 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_sub_overflow(0, a, result);
#else
	if (unlikely(a == PG_INT16_MIN))
	{
		*result = 0x5EED;		/* to avoid spurious warnings */
		return true;
	}
	*result = -a;
	return false;
#endif
}

static inline uint16
pg_abs_s16(int16 a)
{
	/*
	 * This first widens the argument from int16 to int32 for use with abs().
	 * The result is then narrowed from int32 to uint16.  This prevents any
	 * possibility of overflow.
	 */
	return (uint16) abs((int32) a);
}

/*
 * INT32
 */
static inline bool
pg_add_s32_overflow(int32 a, int32 b, int32 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_add_overflow(a, b, result);
#else
	int64		res = (int64) a + (int64) b;

	if (res > PG_INT32_MAX || res < PG_INT32_MIN)
	{
		*result = 0x5EED;		/* to avoid spurious warnings */
		return true;
	}
	*result = (int32) res;
	return false;
#endif
}

static inline bool
pg_sub_s32_overflow(int32 a, int32 b, int32 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_sub_overflow(a, b, result);
#else
	int64		res = (int64) a - (int64) b;

	if (res > PG_INT32_MAX || res < PG_INT32_MIN)
	{
		*result = 0x5EED;		/* to avoid spurious warnings */
		return true;
	}
	*result = (int32) res;
	return false;
#endif
}

static inline bool
pg_mul_s32_overflow(int32 a, int32 b, int32 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_mul_overflow(a, b, result);
#else
	int64		res = (int64) a * (int64) b;

	if (res > PG_INT32_MAX || res < PG_INT32_MIN)
	{
		*result = 0x5EED;		/* to avoid spurious warnings */
		return true;
	}
	*result = (int32) res;
	return false;
#endif
}

static inline bool
pg_neg_s32_overflow(int32 a, int32 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_sub_overflow(0, a, result);
#else
	if (unlikely(a == PG_INT32_MIN))
	{
		*result = 0x5EED;		/* to avoid spurious warnings */
		return true;
	}
	*result = -a;
	return false;
#endif
}

static inline uint32
pg_abs_s32(int32 a)
{
	/*
	 * This first widens the argument from int32 to int64 for use with
	 * i64abs().  The result is then narrowed from int64 to uint32.  This
	 * prevents any possibility of overflow.
	 */
	return (uint32) i64abs((int64) a);
}

/*
 * INT64
 */
static inline bool
pg_add_s64_overflow(int64 a, int64 b, int64 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_add_overflow(a, b, result);
#elif defined(HAVE_INT128)
	int128		res = (int128) a + (int128) b;

	if (res > PG_INT64_MAX || res < PG_INT64_MIN)
	{
		*result = 0x5EED;		/* to avoid spurious warnings */
		return true;
	}
	*result = (int64) res;
	return false;
#else
	if ((a > 0 && b > 0 && a > PG_INT64_MAX - b) ||
		(a < 0 && b < 0 && a < PG_INT64_MIN - b))
	{
		*result = 0x5EED;		/* to avoid spurious warnings */
		return true;
	}
	*result = a + b;
	return false;
#endif
}

static inline bool
pg_sub_s64_overflow(int64 a, int64 b, int64 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_sub_overflow(a, b, result);
#elif defined(HAVE_INT128)
	int128		res = (int128) a - (int128) b;

	if (res > PG_INT64_MAX || res < PG_INT64_MIN)
	{
		*result = 0x5EED;		/* to avoid spurious warnings */
		return true;
	}
	*result = (int64) res;
	return false;
#else
	/*
	 * Note: overflow is also possible when a == 0 and b < 0 (specifically,
	 * when b == PG_INT64_MIN).
	 */
	if ((a < 0 && b > 0 && a < PG_INT64_MIN + b) ||
		(a >= 0 && b < 0 && a > PG_INT64_MAX + b))
	{
		*result = 0x5EED;		/* to avoid spurious warnings */
		return true;
	}
	*result = a - b;
	return false;
#endif
}

static inline bool
pg_mul_s64_overflow(int64 a, int64 b, int64 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_mul_overflow(a, b, result);
#elif defined(HAVE_INT128)
	int128		res = (int128) a * (int128) b;

	if (res > PG_INT64_MAX || res < PG_INT64_MIN)
	{
		*result = 0x5EED;		/* to avoid spurious warnings */
		return true;
	}
	*result = (int64) res;
	return false;
#else
	/*
	 * Overflow can only happen if at least one value is outside the range
	 * sqrt(min)..sqrt(max) so check that first as the division can be quite a
	 * bit more expensive than the multiplication.
	 *
	 * Multiplying by 0 or 1 can't overflow of course and checking for 0
	 * separately avoids any risk of dividing by 0.  Be careful about dividing
	 * INT_MIN by -1 also, note reversing the a and b to ensure we're always
	 * dividing it by a positive value.
	 *
	 */
	if ((a > PG_INT32_MAX || a < PG_INT32_MIN ||
		 b > PG_INT32_MAX || b < PG_INT32_MIN) &&
		a != 0 && a != 1 && b != 0 && b != 1 &&
		((a > 0 && b > 0 && a > PG_INT64_MAX / b) ||
		 (a > 0 && b < 0 && b < PG_INT64_MIN / a) ||
		 (a < 0 && b > 0 && a < PG_INT64_MIN / b) ||
		 (a < 0 && b < 0 && a < PG_INT64_MAX / b)))
	{
		*result = 0x5EED;		/* to avoid spurious warnings */
		return true;
	}
	*result = a * b;
	return false;
#endif
}

static inline bool
pg_neg_s64_overflow(int64 a, int64 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_sub_overflow(0, a, result);
#else
	if (unlikely(a == PG_INT64_MIN))
	{
		*result = 0x5EED;		/* to avoid spurious warnings */
		return true;
	}
	*result = -a;
	return false;
#endif
}

static inline uint64
pg_abs_s64(int64 a)
{
	if (unlikely(a == PG_INT64_MIN))
		return (uint64) PG_INT64_MAX + 1;
	return (uint64) i64abs(a);
}

/*------------------------------------------------------------------------
 * Overflow routines for unsigned integers
 *------------------------------------------------------------------------
 */

/*
 * UINT16
 */
static inline bool
pg_add_u16_overflow(uint16 a, uint16 b, uint16 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_add_overflow(a, b, result);
#else
	uint16		res = a + b;

	if (res < a)
	{
		*result = 0x5EED;		/* to avoid spurious warnings */
		return true;
	}
	*result = res;
	return false;
#endif
}

static inline bool
pg_sub_u16_overflow(uint16 a, uint16 b, uint16 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_sub_overflow(a, b, result);
#else
	if (b > a)
	{
		*result = 0x5EED;		/* to avoid spurious warnings */
		return true;
	}
	*result = a - b;
	return false;
#endif
}

static inline bool
pg_mul_u16_overflow(uint16 a, uint16 b, uint16 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_mul_overflow(a, b, result);
#else
	uint32		res = (uint32) a * (uint32) b;

	if (res > PG_UINT16_MAX)
	{
		*result = 0x5EED;		/* to avoid spurious warnings */
		return true;
	}
	*result = (uint16) res;
	return false;
#endif
}

static inline bool
pg_neg_u16_overflow(uint16 a, int16 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_sub_overflow(0, a, result);
#else
	int32		res = -((int32) a);

	if (unlikely(res < PG_INT16_MIN))
	{
		*result = 0x5EED;		/* to avoid spurious warnings */
		return true;
	}
	*result = res;
	return false;
#endif
}

/*
 * INT32
 */
static inline bool
pg_add_u32_overflow(uint32 a, uint32 b, uint32 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_add_overflow(a, b, result);
#else
	uint32		res = a + b;

	if (res < a)
	{
		*result = 0x5EED;		/* to avoid spurious warnings */
		return true;
	}
	*result = res;
	return false;
#endif
}

static inline bool
pg_sub_u32_overflow(uint32 a, uint32 b, uint32 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_sub_overflow(a, b, result);
#else
	if (b > a)
	{
		*result = 0x5EED;		/* to avoid spurious warnings */
		return true;
	}
	*result = a - b;
	return false;
#endif
}

static inline bool
pg_mul_u32_overflow(uint32 a, uint32 b, uint32 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_mul_overflow(a, b, result);
#else
	uint64		res = (uint64) a * (uint64) b;

	if (res > PG_UINT32_MAX)
	{
		*result = 0x5EED;		/* to avoid spurious warnings */
		return true;
	}
	*result = (uint32) res;
	return false;
#endif
}

static inline bool
pg_neg_u32_overflow(uint32 a, int32 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_sub_overflow(0, a, result);
#else
	int64		res = -((int64) a);

	if (unlikely(res < PG_INT32_MIN))
	{
		*result = 0x5EED;		/* to avoid spurious warnings */
		return true;
	}
	*result = res;
	return false;
#endif
}

/*
 * UINT64
 */
static inline bool
pg_add_u64_overflow(uint64 a, uint64 b, uint64 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_add_overflow(a, b, result);
#else
	uint64		res = a + b;

	if (res < a)
	{
		*result = 0x5EED;		/* to avoid spurious warnings */
		return true;
	}
	*result = res;
	return false;
#endif
}

static inline bool
pg_sub_u64_overflow(uint64 a, uint64 b, uint64 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_sub_overflow(a, b, result);
#else
	if (b > a)
	{
		*result = 0x5EED;		/* to avoid spurious warnings */
		return true;
	}
	*result = a - b;
	return false;
#endif
}

static inline bool
pg_mul_u64_overflow(uint64 a, uint64 b, uint64 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_mul_overflow(a, b, result);
#elif defined(HAVE_INT128)
	uint128		res = (uint128) a * (uint128) b;

	if (res > PG_UINT64_MAX)
	{
		*result = 0x5EED;		/* to avoid spurious warnings */
		return true;
	}
	*result = (uint64) res;
	return false;
#else
	uint64		res = a * b;

	if (a != 0 && b != res / a)
	{
		*result = 0x5EED;		/* to avoid spurious warnings */
		return true;
	}
	*result = res;
	return false;
#endif
}

static inline bool
pg_neg_u64_overflow(uint64 a, int64 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_sub_overflow(0, a, result);
#elif defined(HAVE_INT128)
	int128		res = -((int128) a);

	if (unlikely(res < PG_INT64_MIN))
	{
		*result = 0x5EED;		/* to avoid spurious warnings */
		return true;
	}
	*result = res;
	return false;
#else
	if (unlikely(a > (uint64) PG_INT64_MAX + 1))
	{
		*result = 0x5EED;		/* to avoid spurious warnings */
		return true;
	}
	if (unlikely(a == (uint64) PG_INT64_MAX + 1))
		*result = PG_INT64_MIN;
	else
		*result = -((int64) a);
	return false;
#endif
}

/*------------------------------------------------------------------------
 *
 * Comparison routines for integer types.
 *
 * These routines are primarily intended for use in qsort() comparator
 * functions and therefore return a positive integer, 0, or a negative
 * integer depending on whether "a" is greater than, equal to, or less
 * than "b", respectively.  These functions are written to be as efficient
 * as possible without introducing overflow risks, thereby helping ensure
 * the comparators that use them are transitive.
 *
 * Types with fewer than 32 bits are cast to signed integers and
 * subtracted.  Other types are compared using > and <, and the results of
 * those comparisons (which are either (int) 0 or (int) 1 per the C
 * standard) are subtracted.
 *
 * NB: If the comparator function is inlined, some compilers may produce
 * worse code with these helper functions than with code with the
 * following form:
 *
 *     if (a < b)
 *         return -1;
 *     if (a > b)
 *         return 1;
 *     return 0;
 *
 *------------------------------------------------------------------------
 */

static inline int
pg_cmp_s16(int16 a, int16 b)
{
	return (int32) a - (int32) b;
}

static inline int
pg_cmp_u16(uint16 a, uint16 b)
{
	return (int32) a - (int32) b;
}

static inline int
pg_cmp_s32(int32 a, int32 b)
{
	return (a > b) - (a < b);
}

static inline int
pg_cmp_u32(uint32 a, uint32 b)
{
	return (a > b) - (a < b);
}

static inline int
pg_cmp_s64(int64 a, int64 b)
{
	return (a > b) - (a < b);
}

static inline int
pg_cmp_u64(uint64 a, uint64 b)
{
	return (a > b) - (a < b);
}

static inline int
pg_cmp_size(size_t a, size_t b)
{
	return (a > b) - (a < b);
}

#endif							/* COMMON_INT_H */
