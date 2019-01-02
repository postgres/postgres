/*-------------------------------------------------------------------------
 *
 * int128.h
 *	  Roll-our-own 128-bit integer arithmetic.
 *
 * We make use of the native int128 type if there is one, otherwise
 * implement things the hard way based on two int64 halves.
 *
 * See src/tools/testint128.c for a simple test harness for this file.
 *
 * Copyright (c) 2017-2019, PostgreSQL Global Development Group
 *
 * src/include/common/int128.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef INT128_H
#define INT128_H

/*
 * For testing purposes, use of native int128 can be switched on/off by
 * predefining USE_NATIVE_INT128.
 */
#ifndef USE_NATIVE_INT128
#ifdef HAVE_INT128
#define USE_NATIVE_INT128 1
#else
#define USE_NATIVE_INT128 0
#endif
#endif


#if USE_NATIVE_INT128

typedef int128 INT128;

/*
 * Add an unsigned int64 value into an INT128 variable.
 */
static inline void
int128_add_uint64(INT128 *i128, uint64 v)
{
	*i128 += v;
}

/*
 * Add a signed int64 value into an INT128 variable.
 */
static inline void
int128_add_int64(INT128 *i128, int64 v)
{
	*i128 += v;
}

/*
 * Add the 128-bit product of two int64 values into an INT128 variable.
 *
 * XXX with a stupid compiler, this could actually be less efficient than
 * the other implementation; maybe we should do it by hand always?
 */
static inline void
int128_add_int64_mul_int64(INT128 *i128, int64 x, int64 y)
{
	*i128 += (int128) x * (int128) y;
}

/*
 * Compare two INT128 values, return -1, 0, or +1.
 */
static inline int
int128_compare(INT128 x, INT128 y)
{
	if (x < y)
		return -1;
	if (x > y)
		return 1;
	return 0;
}

/*
 * Widen int64 to INT128.
 */
static inline INT128
int64_to_int128(int64 v)
{
	return (INT128) v;
}

/*
 * Convert INT128 to int64 (losing any high-order bits).
 * This also works fine for casting down to uint64.
 */
static inline int64
int128_to_int64(INT128 val)
{
	return (int64) val;
}

#else							/* !USE_NATIVE_INT128 */

/*
 * We lay out the INT128 structure with the same content and byte ordering
 * that a native int128 type would (probably) have.  This makes no difference
 * for ordinary use of INT128, but allows union'ing INT128 with int128 for
 * testing purposes.
 */
typedef struct
{
#ifdef WORDS_BIGENDIAN
	int64		hi;				/* most significant 64 bits, including sign */
	uint64		lo;				/* least significant 64 bits, without sign */
#else
	uint64		lo;				/* least significant 64 bits, without sign */
	int64		hi;				/* most significant 64 bits, including sign */
#endif
} INT128;

/*
 * Add an unsigned int64 value into an INT128 variable.
 */
static inline void
int128_add_uint64(INT128 *i128, uint64 v)
{
	/*
	 * First add the value to the .lo part, then check to see if a carry needs
	 * to be propagated into the .hi part.  A carry is needed if both inputs
	 * have high bits set, or if just one input has high bit set while the new
	 * .lo part doesn't.  Remember that .lo part is unsigned; we cast to
	 * signed here just as a cheap way to check the high bit.
	 */
	uint64		oldlo = i128->lo;

	i128->lo += v;
	if (((int64) v < 0 && (int64) oldlo < 0) ||
		(((int64) v < 0 || (int64) oldlo < 0) && (int64) i128->lo >= 0))
		i128->hi++;
}

/*
 * Add a signed int64 value into an INT128 variable.
 */
static inline void
int128_add_int64(INT128 *i128, int64 v)
{
	/*
	 * This is much like the above except that the carry logic differs for
	 * negative v.  Ordinarily we'd need to subtract 1 from the .hi part
	 * (corresponding to adding the sign-extended bits of v to it); but if
	 * there is a carry out of the .lo part, that cancels and we do nothing.
	 */
	uint64		oldlo = i128->lo;

	i128->lo += v;
	if (v >= 0)
	{
		if ((int64) oldlo < 0 && (int64) i128->lo >= 0)
			i128->hi++;
	}
	else
	{
		if (!((int64) oldlo < 0 || (int64) i128->lo >= 0))
			i128->hi--;
	}
}

/*
 * INT64_AU32 extracts the most significant 32 bits of int64 as int64, while
 * INT64_AL32 extracts the least significant 32 bits as uint64.
 */
#define INT64_AU32(i64) ((i64) >> 32)
#define INT64_AL32(i64) ((i64) & UINT64CONST(0xFFFFFFFF))

/*
 * Add the 128-bit product of two int64 values into an INT128 variable.
 */
static inline void
int128_add_int64_mul_int64(INT128 *i128, int64 x, int64 y)
{
	/* INT64_AU32 must use arithmetic right shift */
	StaticAssertStmt(((int64) -1 >> 1) == (int64) -1,
					 "arithmetic right shift is needed");

	/*----------
	 * Form the 128-bit product x * y using 64-bit arithmetic.
	 * Considering each 64-bit input as having 32-bit high and low parts,
	 * we can compute
	 *
	 *	 x * y = ((x.hi << 32) + x.lo) * (((y.hi << 32) + y.lo)
	 *		   = (x.hi * y.hi) << 64 +
	 *			 (x.hi * y.lo) << 32 +
	 *			 (x.lo * y.hi) << 32 +
	 *			 x.lo * y.lo
	 *
	 * Each individual product is of 32-bit terms so it won't overflow when
	 * computed in 64-bit arithmetic.  Then we just have to shift it to the
	 * correct position while adding into the 128-bit result.  We must also
	 * keep in mind that the "lo" parts must be treated as unsigned.
	 *----------
	 */

	/* No need to work hard if product must be zero */
	if (x != 0 && y != 0)
	{
		int64		x_u32 = INT64_AU32(x);
		uint64		x_l32 = INT64_AL32(x);
		int64		y_u32 = INT64_AU32(y);
		uint64		y_l32 = INT64_AL32(y);
		int64		tmp;

		/* the first term */
		i128->hi += x_u32 * y_u32;

		/* the second term: sign-extend it only if x is negative */
		tmp = x_u32 * y_l32;
		if (x < 0)
			i128->hi += INT64_AU32(tmp);
		else
			i128->hi += ((uint64) tmp) >> 32;
		int128_add_uint64(i128, ((uint64) INT64_AL32(tmp)) << 32);

		/* the third term: sign-extend it only if y is negative */
		tmp = x_l32 * y_u32;
		if (y < 0)
			i128->hi += INT64_AU32(tmp);
		else
			i128->hi += ((uint64) tmp) >> 32;
		int128_add_uint64(i128, ((uint64) INT64_AL32(tmp)) << 32);

		/* the fourth term: always unsigned */
		int128_add_uint64(i128, x_l32 * y_l32);
	}
}

/*
 * Compare two INT128 values, return -1, 0, or +1.
 */
static inline int
int128_compare(INT128 x, INT128 y)
{
	if (x.hi < y.hi)
		return -1;
	if (x.hi > y.hi)
		return 1;
	if (x.lo < y.lo)
		return -1;
	if (x.lo > y.lo)
		return 1;
	return 0;
}

/*
 * Widen int64 to INT128.
 */
static inline INT128
int64_to_int128(int64 v)
{
	INT128		val;

	val.lo = (uint64) v;
	val.hi = (v < 0) ? -INT64CONST(1) : INT64CONST(0);
	return val;
}

/*
 * Convert INT128 to int64 (losing any high-order bits).
 * This also works fine for casting down to uint64.
 */
static inline int64
int128_to_int64(INT128 val)
{
	return (int64) val.lo;
}

#endif							/* USE_NATIVE_INT128 */

#endif							/* INT128_H */
