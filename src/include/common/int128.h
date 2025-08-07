/*-------------------------------------------------------------------------
 *
 * int128.h
 *	  Roll-our-own 128-bit integer arithmetic.
 *
 * We make use of the native int128 type if there is one, otherwise
 * implement things the hard way based on two int64 halves.
 *
 * See src/test/modules/test_int128 for a simple test harness for this file.
 *
 * Copyright (c) 2017-2025, PostgreSQL Global Development Group
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

/*
 * If native int128 support is enabled, INT128 is just int128. Otherwise, it
 * is a structure with separate 64-bit high and low parts.
 *
 * We lay out the INT128 structure with the same content and byte ordering
 * that a native int128 type would (probably) have.  This makes no difference
 * for ordinary use of INT128, but allows union'ing INT128 with int128 for
 * testing purposes.
 *
 * PG_INT128_HI_INT64 and PG_INT128_LO_UINT64 allow the (signed) high and
 * (unsigned) low 64-bit integer parts to be extracted portably on all
 * platforms.
 */
#if USE_NATIVE_INT128

typedef int128 INT128;

#define PG_INT128_HI_INT64(i128)	((int64) ((i128) >> 64))
#define PG_INT128_LO_UINT64(i128)	((uint64) (i128))

#else

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

#define PG_INT128_HI_INT64(i128)	((i128).hi)
#define PG_INT128_LO_UINT64(i128)	((i128).lo)

#endif

/*
 * Construct an INT128 from (signed) high and (unsigned) low 64-bit integer
 * parts.
 */
static inline INT128
make_int128(int64 hi, uint64 lo)
{
#if USE_NATIVE_INT128
	return (((int128) hi) << 64) + lo;
#else
	INT128		val;

	val.hi = hi;
	val.lo = lo;
	return val;
#endif
}

/*
 * Add an unsigned int64 value into an INT128 variable.
 */
static inline void
int128_add_uint64(INT128 *i128, uint64 v)
{
#if USE_NATIVE_INT128
	*i128 += v;
#else
	/*
	 * First add the value to the .lo part, then check to see if a carry needs
	 * to be propagated into the .hi part.  Since this is unsigned integer
	 * arithmetic, which is just modular arithmetic, a carry is needed if the
	 * new .lo part is less than the old .lo part (i.e., if modular
	 * wrap-around occurred).  Writing this in the form below, rather than
	 * using an "if" statement causes modern compilers to produce branchless
	 * machine code identical to the native code.
	 */
	uint64		oldlo = i128->lo;

	i128->lo += v;
	i128->hi += (i128->lo < oldlo);
#endif
}

/*
 * Add a signed int64 value into an INT128 variable.
 */
static inline void
int128_add_int64(INT128 *i128, int64 v)
{
#if USE_NATIVE_INT128
	*i128 += v;
#else
	/*
	 * This is much like the above except that the carry logic differs for
	 * negative v -- we need to subtract 1 from the .hi part if the new .lo
	 * value is greater than the old .lo value.  That can be achieved without
	 * any branching by adding the sign bit from v (v >> 63 = 0 or -1) to the
	 * previous result (for negative v, if the new .lo value is less than the
	 * old .lo value, the two terms cancel and we leave the .hi part
	 * unchanged, otherwise we subtract 1 from the .hi part).  With modern
	 * compilers this often produces machine code identical to the native
	 * code.
	 */
	uint64		oldlo = i128->lo;

	i128->lo += v;
	i128->hi += (i128->lo < oldlo) + (v >> 63);
#endif
}

/*
 * Add an INT128 value into an INT128 variable.
 */
static inline void
int128_add_int128(INT128 *i128, INT128 v)
{
#if USE_NATIVE_INT128
	*i128 += v;
#else
	int128_add_uint64(i128, v.lo);
	i128->hi += v.hi;
#endif
}

/*
 * Subtract an unsigned int64 value from an INT128 variable.
 */
static inline void
int128_sub_uint64(INT128 *i128, uint64 v)
{
#if USE_NATIVE_INT128
	*i128 -= v;
#else
	/*
	 * This is like int128_add_uint64(), except we must propagate a borrow to
	 * (subtract 1 from) the .hi part if the new .lo part is greater than the
	 * old .lo part.
	 */
	uint64		oldlo = i128->lo;

	i128->lo -= v;
	i128->hi -= (i128->lo > oldlo);
#endif
}

/*
 * Subtract a signed int64 value from an INT128 variable.
 */
static inline void
int128_sub_int64(INT128 *i128, int64 v)
{
#if USE_NATIVE_INT128
	*i128 -= v;
#else
	/* Like int128_add_int64() with the sign of v inverted */
	uint64		oldlo = i128->lo;

	i128->lo -= v;
	i128->hi -= (i128->lo > oldlo) + (v >> 63);
#endif
}

/*
 * INT64_HI_INT32 extracts the most significant 32 bits of int64 as int32.
 * INT64_LO_UINT32 extracts the least significant 32 bits as uint32.
 */
#define INT64_HI_INT32(i64)		((int32) ((i64) >> 32))
#define INT64_LO_UINT32(i64)	((uint32) (i64))

/*
 * Add the 128-bit product of two int64 values into an INT128 variable.
 */
static inline void
int128_add_int64_mul_int64(INT128 *i128, int64 x, int64 y)
{
#if USE_NATIVE_INT128
	/*
	 * XXX with a stupid compiler, this could actually be less efficient than
	 * the non-native implementation; maybe we should do it by hand always?
	 */
	*i128 += (int128) x * (int128) y;
#else
	/* INT64_HI_INT32 must use arithmetic right shift */
	StaticAssertDecl(((int64) -1 >> 1) == (int64) -1,
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
		int32		x_hi = INT64_HI_INT32(x);
		uint32		x_lo = INT64_LO_UINT32(x);
		int32		y_hi = INT64_HI_INT32(y);
		uint32		y_lo = INT64_LO_UINT32(y);
		int64		tmp;

		/* the first term */
		i128->hi += (int64) x_hi * (int64) y_hi;

		/* the second term: sign-extended with the sign of x */
		tmp = (int64) x_hi * (int64) y_lo;
		i128->hi += INT64_HI_INT32(tmp);
		int128_add_uint64(i128, ((uint64) INT64_LO_UINT32(tmp)) << 32);

		/* the third term: sign-extended with the sign of y */
		tmp = (int64) x_lo * (int64) y_hi;
		i128->hi += INT64_HI_INT32(tmp);
		int128_add_uint64(i128, ((uint64) INT64_LO_UINT32(tmp)) << 32);

		/* the fourth term: always unsigned */
		int128_add_uint64(i128, (uint64) x_lo * (uint64) y_lo);
	}
#endif
}

/*
 * Subtract the 128-bit product of two int64 values from an INT128 variable.
 */
static inline void
int128_sub_int64_mul_int64(INT128 *i128, int64 x, int64 y)
{
#if USE_NATIVE_INT128
	*i128 -= (int128) x * (int128) y;
#else
	/* As above, except subtract the 128-bit product */
	if (x != 0 && y != 0)
	{
		int32		x_hi = INT64_HI_INT32(x);
		uint32		x_lo = INT64_LO_UINT32(x);
		int32		y_hi = INT64_HI_INT32(y);
		uint32		y_lo = INT64_LO_UINT32(y);
		int64		tmp;

		/* the first term */
		i128->hi -= (int64) x_hi * (int64) y_hi;

		/* the second term: sign-extended with the sign of x */
		tmp = (int64) x_hi * (int64) y_lo;
		i128->hi -= INT64_HI_INT32(tmp);
		int128_sub_uint64(i128, ((uint64) INT64_LO_UINT32(tmp)) << 32);

		/* the third term: sign-extended with the sign of y */
		tmp = (int64) x_lo * (int64) y_hi;
		i128->hi -= INT64_HI_INT32(tmp);
		int128_sub_uint64(i128, ((uint64) INT64_LO_UINT32(tmp)) << 32);

		/* the fourth term: always unsigned */
		int128_sub_uint64(i128, (uint64) x_lo * (uint64) y_lo);
	}
#endif
}

/*
 * Divide an INT128 variable by a signed int32 value, returning the quotient
 * and remainder.  The remainder will have the same sign as *i128.
 *
 * Note: This provides no protection against dividing by 0, or dividing
 * INT128_MIN by -1, which overflows.  It is the caller's responsibility to
 * guard against those.
 */
static inline void
int128_div_mod_int32(INT128 *i128, int32 v, int32 *remainder)
{
#if USE_NATIVE_INT128
	int128		old_i128 = *i128;

	*i128 /= v;
	*remainder = (int32) (old_i128 - *i128 * v);
#else
	/*
	 * To avoid any intermediate values overflowing (as happens if INT64_MIN
	 * is divided by -1), we first compute the quotient abs(*i128) / abs(v)
	 * using unsigned 64-bit arithmetic, and then fix the signs up at the end.
	 *
	 * The quotient is computed using the short division algorithm described
	 * in Knuth volume 2, section 4.3.1 exercise 16 (cf. div_var_int() in
	 * numeric.c).  Since the absolute value of the divisor is known to be at
	 * most 2^31, the remainder carried from one digit to the next is at most
	 * 2^31 - 1, and so there is no danger of overflow when this is combined
	 * with the next digit (a 32-bit unsigned integer).
	 */
	uint64		n_hi;
	uint64		n_lo;
	uint32		d;
	uint64		q;
	uint64		r;
	uint64		tmp;

	/* numerator: absolute value of *i128 */
	if (i128->hi < 0)
	{
		n_hi = 0 - ((uint64) i128->hi);
		n_lo = 0 - i128->lo;
		if (n_lo != 0)
			n_hi--;
	}
	else
	{
		n_hi = i128->hi;
		n_lo = i128->lo;
	}

	/* denomimator: absolute value of v */
	d = abs(v);

	/* quotient and remainder of high 64 bits */
	q = n_hi / d;
	r = n_hi % d;
	n_hi = q;

	/* quotient and remainder of next 32 bits (upper half of n_lo) */
	tmp = (r << 32) + (n_lo >> 32);
	q = tmp / d;
	r = tmp % d;

	/* quotient and remainder of last 32 bits (lower half of n_lo) */
	tmp = (r << 32) + (uint32) n_lo;
	n_lo = q << 32;
	q = tmp / d;
	r = tmp % d;
	n_lo += q;

	/* final remainder should have the same sign as *i128 */
	*remainder = i128->hi < 0 ? (int32) (0 - r) : (int32) r;

	/* store the quotient in *i128, negating it if necessary */
	if ((i128->hi < 0) != (v < 0))
	{
		n_hi = 0 - n_hi;
		n_lo = 0 - n_lo;
		if (n_lo != 0)
			n_hi--;
	}
	i128->hi = (int64) n_hi;
	i128->lo = n_lo;
#endif
}

/*
 * Test if an INT128 value is zero.
 */
static inline bool
int128_is_zero(INT128 x)
{
#if USE_NATIVE_INT128
	return x == 0;
#else
	return x.hi == 0 && x.lo == 0;
#endif
}

/*
 * Return the sign of an INT128 value (returns -1, 0, or +1).
 */
static inline int
int128_sign(INT128 x)
{
#if USE_NATIVE_INT128
	if (x < 0)
		return -1;
	if (x > 0)
		return 1;
	return 0;
#else
	if (x.hi < 0)
		return -1;
	if (x.hi > 0)
		return 1;
	if (x.lo > 0)
		return 1;
	return 0;
#endif
}

/*
 * Compare two INT128 values, return -1, 0, or +1.
 */
static inline int
int128_compare(INT128 x, INT128 y)
{
#if USE_NATIVE_INT128
	if (x < y)
		return -1;
	if (x > y)
		return 1;
	return 0;
#else
	if (x.hi < y.hi)
		return -1;
	if (x.hi > y.hi)
		return 1;
	if (x.lo < y.lo)
		return -1;
	if (x.lo > y.lo)
		return 1;
	return 0;
#endif
}

/*
 * Widen int64 to INT128.
 */
static inline INT128
int64_to_int128(int64 v)
{
#if USE_NATIVE_INT128
	return (INT128) v;
#else
	INT128		val;

	val.lo = (uint64) v;
	val.hi = (v < 0) ? -INT64CONST(1) : INT64CONST(0);
	return val;
#endif
}

/*
 * Convert INT128 to int64 (losing any high-order bits).
 * This also works fine for casting down to uint64.
 */
static inline int64
int128_to_int64(INT128 val)
{
#if USE_NATIVE_INT128
	return (int64) val;
#else
	return (int64) val.lo;
#endif
}

#endif							/* INT128_H */
