/*---------------------------------------------------------------------------
 *
 * Ryu floating-point output for double precision.
 *
 * Portions Copyright (c) 2018-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/common/d2s.c
 *
 * This is a modification of code taken from github.com/ulfjack/ryu under the
 * terms of the Boost license (not the Apache license). The original copyright
 * notice follows:
 *
 * Copyright 2018 Ulf Adams
 *
 * The contents of this file may be used under the terms of the Apache
 * License, Version 2.0.
 *
 *     (See accompanying file LICENSE-Apache or copy at
 *      http://www.apache.org/licenses/LICENSE-2.0)
 *
 * Alternatively, the contents of this file may be used under the terms of the
 * Boost Software License, Version 1.0.
 *
 *     (See accompanying file LICENSE-Boost or copy at
 *      https://www.boost.org/LICENSE_1_0.txt)
 *
 * Unless required by applicable law or agreed to in writing, this software is
 * distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.
 *
 *---------------------------------------------------------------------------
 */

/*
 *  Runtime compiler options:
 *
 *  -DRYU_ONLY_64_BIT_OPS Avoid using uint128 or 64-bit intrinsics. Slower,
 *      depending on your compiler.
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "common/shortest_dec.h"

/*
 * For consistency, we use 128-bit types if and only if the rest of PG also
 * does, even though we could use them here without worrying about the
 * alignment concerns that apply elsewhere.
 */
#if !defined(HAVE_INT128) && defined(_MSC_VER) \
	&& !defined(RYU_ONLY_64_BIT_OPS) && defined(_M_X64)
#define HAS_64_BIT_INTRINSICS
#endif

#include "ryu_common.h"
#include "digit_table.h"
#include "d2s_full_table.h"
#include "d2s_intrinsics.h"

#define DOUBLE_MANTISSA_BITS 52
#define DOUBLE_EXPONENT_BITS 11
#define DOUBLE_BIAS 1023

#define DOUBLE_POW5_INV_BITCOUNT 122
#define DOUBLE_POW5_BITCOUNT 121


static inline uint32
pow5Factor(uint64 value)
{
	uint32		count = 0;

	for (;;)
	{
		Assert(value != 0);
		const uint64 q = div5(value);
		const uint32 r = (uint32) (value - 5 * q);

		if (r != 0)
			break;

		value = q;
		++count;
	}
	return count;
}

/*  Returns true if value is divisible by 5^p. */
static inline bool
multipleOfPowerOf5(const uint64 value, const uint32 p)
{
	/*
	 * I tried a case distinction on p, but there was no performance
	 * difference.
	 */
	return pow5Factor(value) >= p;
}

/*  Returns true if value is divisible by 2^p. */
static inline bool
multipleOfPowerOf2(const uint64 value, const uint32 p)
{
	/* return __builtin_ctzll(value) >= p; */
	return (value & ((UINT64CONST(1) << p) - 1)) == 0;
}

/*
 * We need a 64x128-bit multiplication and a subsequent 128-bit shift.
 *
 * Multiplication:
 *
 *    The 64-bit factor is variable and passed in, the 128-bit factor comes
 *    from a lookup table. We know that the 64-bit factor only has 55
 *    significant bits (i.e., the 9 topmost bits are zeros). The 128-bit
 *    factor only has 124 significant bits (i.e., the 4 topmost bits are
 *    zeros).
 *
 * Shift:
 *
 *    In principle, the multiplication result requires 55 + 124 = 179 bits to
 *    represent. However, we then shift this value to the right by j, which is
 *    at least j >= 115, so the result is guaranteed to fit into 179 - 115 =
 *    64 bits. This means that we only need the topmost 64 significant bits of
 *    the 64x128-bit multiplication.
 *
 * There are several ways to do this:
 *
 *  1. Best case: the compiler exposes a 128-bit type.
 *     We perform two 64x64-bit multiplications, add the higher 64 bits of the
 *     lower result to the higher result, and shift by j - 64 bits.
 *
 *     We explicitly cast from 64-bit to 128-bit, so the compiler can tell
 *     that these are only 64-bit inputs, and can map these to the best
 *     possible sequence of assembly instructions. x86-64 machines happen to
 *     have matching assembly instructions for 64x64-bit multiplications and
 *     128-bit shifts.
 *
 *  2. Second best case: the compiler exposes intrinsics for the x86-64
 *     assembly instructions mentioned in 1.
 *
 *  3. We only have 64x64 bit instructions that return the lower 64 bits of
 *     the result, i.e., we have to use plain C.
 *
 *     Our inputs are less than the full width, so we have three options:
 *     a. Ignore this fact and just implement the intrinsics manually.
 *     b. Split both into 31-bit pieces, which guarantees no internal
 *        overflow, but requires extra work upfront (unless we change the
 *        lookup table).
 *     c. Split only the first factor into 31-bit pieces, which also
 *        guarantees no internal overflow, but requires extra work since the
 *        intermediate results are not perfectly aligned.
 */
#if defined(HAVE_INT128)

/*  Best case: use 128-bit type. */
static inline uint64
mulShift(const uint64 m, const uint64 *const mul, const int32 j)
{
	const uint128 b0 = ((uint128) m) * mul[0];
	const uint128 b2 = ((uint128) m) * mul[1];

	return (uint64) (((b0 >> 64) + b2) >> (j - 64));
}

static inline uint64
mulShiftAll(const uint64 m, const uint64 *const mul, const int32 j,
			uint64 *const vp, uint64 *const vm, const uint32 mmShift)
{
	*vp = mulShift(4 * m + 2, mul, j);
	*vm = mulShift(4 * m - 1 - mmShift, mul, j);
	return mulShift(4 * m, mul, j);
}

#elif defined(HAS_64_BIT_INTRINSICS)

static inline uint64
mulShift(const uint64 m, const uint64 *const mul, const int32 j)
{
	/* m is maximum 55 bits */
	uint64		high1;

	/* 128 */
	const uint64 low1 = umul128(m, mul[1], &high1);

	/* 64 */
	uint64		high0;
	uint64		sum;

	/* 64 */
	umul128(m, mul[0], &high0);
	/* 0 */
	sum = high0 + low1;

	if (sum < high0)
	{
		++high1;
		/* overflow into high1 */
	}
	return shiftright128(sum, high1, j - 64);
}

static inline uint64
mulShiftAll(const uint64 m, const uint64 *const mul, const int32 j,
			uint64 *const vp, uint64 *const vm, const uint32 mmShift)
{
	*vp = mulShift(4 * m + 2, mul, j);
	*vm = mulShift(4 * m - 1 - mmShift, mul, j);
	return mulShift(4 * m, mul, j);
}

#else							/* // !defined(HAVE_INT128) &&
								 * !defined(HAS_64_BIT_INTRINSICS) */

static inline uint64
mulShiftAll(uint64 m, const uint64 *const mul, const int32 j,
			uint64 *const vp, uint64 *const vm, const uint32 mmShift)
{
	m <<= 1;					/* m is maximum 55 bits */

	uint64		tmp;
	const uint64 lo = umul128(m, mul[0], &tmp);
	uint64		hi;
	const uint64 mid = tmp + umul128(m, mul[1], &hi);

	hi += mid < tmp;			/* overflow into hi */

	const uint64 lo2 = lo + mul[0];
	const uint64 mid2 = mid + mul[1] + (lo2 < lo);
	const uint64 hi2 = hi + (mid2 < mid);

	*vp = shiftright128(mid2, hi2, j - 64 - 1);

	if (mmShift == 1)
	{
		const uint64 lo3 = lo - mul[0];
		const uint64 mid3 = mid - mul[1] - (lo3 > lo);
		const uint64 hi3 = hi - (mid3 > mid);

		*vm = shiftright128(mid3, hi3, j - 64 - 1);
	}
	else
	{
		const uint64 lo3 = lo + lo;
		const uint64 mid3 = mid + mid + (lo3 < lo);
		const uint64 hi3 = hi + hi + (mid3 < mid);
		const uint64 lo4 = lo3 - mul[0];
		const uint64 mid4 = mid3 - mul[1] - (lo4 > lo3);
		const uint64 hi4 = hi3 - (mid4 > mid3);

		*vm = shiftright128(mid4, hi4, j - 64);
	}

	return shiftright128(mid, hi, j - 64 - 1);
}

#endif							/* // HAS_64_BIT_INTRINSICS */

static inline uint32
decimalLength(const uint64 v)
{
	/* This is slightly faster than a loop. */
	/* The average output length is 16.38 digits, so we check high-to-low. */
	/* Function precondition: v is not an 18, 19, or 20-digit number. */
	/* (17 digits are sufficient for round-tripping.) */
	Assert(v < 100000000000000000L);
	if (v >= 10000000000000000L)
	{
		return 17;
	}
	if (v >= 1000000000000000L)
	{
		return 16;
	}
	if (v >= 100000000000000L)
	{
		return 15;
	}
	if (v >= 10000000000000L)
	{
		return 14;
	}
	if (v >= 1000000000000L)
	{
		return 13;
	}
	if (v >= 100000000000L)
	{
		return 12;
	}
	if (v >= 10000000000L)
	{
		return 11;
	}
	if (v >= 1000000000L)
	{
		return 10;
	}
	if (v >= 100000000L)
	{
		return 9;
	}
	if (v >= 10000000L)
	{
		return 8;
	}
	if (v >= 1000000L)
	{
		return 7;
	}
	if (v >= 100000L)
	{
		return 6;
	}
	if (v >= 10000L)
	{
		return 5;
	}
	if (v >= 1000L)
	{
		return 4;
	}
	if (v >= 100L)
	{
		return 3;
	}
	if (v >= 10L)
	{
		return 2;
	}
	return 1;
}

/*  A floating decimal representing m * 10^e. */
typedef struct floating_decimal_64
{
	uint64		mantissa;
	int32		exponent;
} floating_decimal_64;

static inline floating_decimal_64
d2d(const uint64 ieeeMantissa, const uint32 ieeeExponent)
{
	int32		e2;
	uint64		m2;

	if (ieeeExponent == 0)
	{
		/* We subtract 2 so that the bounds computation has 2 additional bits. */
		e2 = 1 - DOUBLE_BIAS - DOUBLE_MANTISSA_BITS - 2;
		m2 = ieeeMantissa;
	}
	else
	{
		e2 = ieeeExponent - DOUBLE_BIAS - DOUBLE_MANTISSA_BITS - 2;
		m2 = (UINT64CONST(1) << DOUBLE_MANTISSA_BITS) | ieeeMantissa;
	}

#if STRICTLY_SHORTEST
	const bool	even = (m2 & 1) == 0;
	const bool	acceptBounds = even;
#else
	const bool	acceptBounds = false;
#endif

	/* Step 2: Determine the interval of legal decimal representations. */
	const uint64 mv = 4 * m2;

	/* Implicit bool -> int conversion. True is 1, false is 0. */
	const uint32 mmShift = ieeeMantissa != 0 || ieeeExponent <= 1;

	/* We would compute mp and mm like this: */
	/* uint64 mp = 4 * m2 + 2; */
	/* uint64 mm = mv - 1 - mmShift; */

	/* Step 3: Convert to a decimal power base using 128-bit arithmetic. */
	uint64		vr,
				vp,
				vm;
	int32		e10;
	bool		vmIsTrailingZeros = false;
	bool		vrIsTrailingZeros = false;

	if (e2 >= 0)
	{
		/*
		 * I tried special-casing q == 0, but there was no effect on
		 * performance.
		 *
		 * This expr is slightly faster than max(0, log10Pow2(e2) - 1).
		 */
		const uint32 q = log10Pow2(e2) - (e2 > 3);
		const int32 k = DOUBLE_POW5_INV_BITCOUNT + pow5bits(q) - 1;
		const int32 i = -e2 + q + k;

		e10 = q;

		vr = mulShiftAll(m2, DOUBLE_POW5_INV_SPLIT[q], i, &vp, &vm, mmShift);

		if (q <= 21)
		{
			/*
			 * This should use q <= 22, but I think 21 is also safe. Smaller
			 * values may still be safe, but it's more difficult to reason
			 * about them.
			 *
			 * Only one of mp, mv, and mm can be a multiple of 5, if any.
			 */
			const uint32 mvMod5 = (uint32) (mv - 5 * div5(mv));

			if (mvMod5 == 0)
			{
				vrIsTrailingZeros = multipleOfPowerOf5(mv, q);
			}
			else if (acceptBounds)
			{
				/*----
				 * Same as min(e2 + (~mm & 1), pow5Factor(mm)) >= q
				 * <=> e2 + (~mm & 1) >= q && pow5Factor(mm) >= q
				 * <=> true && pow5Factor(mm) >= q, since e2 >= q.
				 *----
				 */
				vmIsTrailingZeros = multipleOfPowerOf5(mv - 1 - mmShift, q);
			}
			else
			{
				/* Same as min(e2 + 1, pow5Factor(mp)) >= q. */
				vp -= multipleOfPowerOf5(mv + 2, q);
			}
		}
	}
	else
	{
		/*
		 * This expression is slightly faster than max(0, log10Pow5(-e2) - 1).
		 */
		const uint32 q = log10Pow5(-e2) - (-e2 > 1);
		const int32 i = -e2 - q;
		const int32 k = pow5bits(i) - DOUBLE_POW5_BITCOUNT;
		const int32 j = q - k;

		e10 = q + e2;

		vr = mulShiftAll(m2, DOUBLE_POW5_SPLIT[i], j, &vp, &vm, mmShift);

		if (q <= 1)
		{
			/*
			 * {vr,vp,vm} is trailing zeros if {mv,mp,mm} has at least q
			 * trailing 0 bits.
			 */
			/* mv = 4 * m2, so it always has at least two trailing 0 bits. */
			vrIsTrailingZeros = true;
			if (acceptBounds)
			{
				/*
				 * mm = mv - 1 - mmShift, so it has 1 trailing 0 bit iff
				 * mmShift == 1.
				 */
				vmIsTrailingZeros = mmShift == 1;
			}
			else
			{
				/*
				 * mp = mv + 2, so it always has at least one trailing 0 bit.
				 */
				--vp;
			}
		}
		else if (q < 63)
		{
			/* TODO(ulfjack):Use a tighter bound here. */
			/*
			 * We need to compute min(ntz(mv), pow5Factor(mv) - e2) >= q - 1
			 */
			/* <=> ntz(mv) >= q - 1 && pow5Factor(mv) - e2 >= q - 1 */
			/* <=> ntz(mv) >= q - 1 (e2 is negative and -e2 >= q) */
			/* <=> (mv & ((1 << (q - 1)) - 1)) == 0 */

			/*
			 * We also need to make sure that the left shift does not
			 * overflow.
			 */
			vrIsTrailingZeros = multipleOfPowerOf2(mv, q - 1);
		}
	}

	/*
	 * Step 4: Find the shortest decimal representation in the interval of
	 * legal representations.
	 */
	uint32		removed = 0;
	uint8		lastRemovedDigit = 0;
	uint64		output;

	/* On average, we remove ~2 digits. */
	if (vmIsTrailingZeros || vrIsTrailingZeros)
	{
		/* General case, which happens rarely (~0.7%). */
		for (;;)
		{
			const uint64 vpDiv10 = div10(vp);
			const uint64 vmDiv10 = div10(vm);

			if (vpDiv10 <= vmDiv10)
				break;

			const uint32 vmMod10 = (uint32) (vm - 10 * vmDiv10);
			const uint64 vrDiv10 = div10(vr);
			const uint32 vrMod10 = (uint32) (vr - 10 * vrDiv10);

			vmIsTrailingZeros &= vmMod10 == 0;
			vrIsTrailingZeros &= lastRemovedDigit == 0;
			lastRemovedDigit = (uint8) vrMod10;
			vr = vrDiv10;
			vp = vpDiv10;
			vm = vmDiv10;
			++removed;
		}

		if (vmIsTrailingZeros)
		{
			for (;;)
			{
				const uint64 vmDiv10 = div10(vm);
				const uint32 vmMod10 = (uint32) (vm - 10 * vmDiv10);

				if (vmMod10 != 0)
					break;

				const uint64 vpDiv10 = div10(vp);
				const uint64 vrDiv10 = div10(vr);
				const uint32 vrMod10 = (uint32) (vr - 10 * vrDiv10);

				vrIsTrailingZeros &= lastRemovedDigit == 0;
				lastRemovedDigit = (uint8) vrMod10;
				vr = vrDiv10;
				vp = vpDiv10;
				vm = vmDiv10;
				++removed;
			}
		}

		if (vrIsTrailingZeros && lastRemovedDigit == 5 && vr % 2 == 0)
		{
			/* Round even if the exact number is .....50..0. */
			lastRemovedDigit = 4;
		}

		/*
		 * We need to take vr + 1 if vr is outside bounds or we need to round
		 * up.
		 */
		output = vr + ((vr == vm && (!acceptBounds || !vmIsTrailingZeros)) || lastRemovedDigit >= 5);
	}
	else
	{
		/*
		 * Specialized for the common case (~99.3%). Percentages below are
		 * relative to this.
		 */
		bool		roundUp = false;
		const uint64 vpDiv100 = div100(vp);
		const uint64 vmDiv100 = div100(vm);

		if (vpDiv100 > vmDiv100)
		{
			/* Optimization:remove two digits at a time(~86.2 %). */
			const uint64 vrDiv100 = div100(vr);
			const uint32 vrMod100 = (uint32) (vr - 100 * vrDiv100);

			roundUp = vrMod100 >= 50;
			vr = vrDiv100;
			vp = vpDiv100;
			vm = vmDiv100;
			removed += 2;
		}

		/*----
		 * Loop iterations below (approximately), without optimization
		 * above:
		 *
		 * 0: 0.03%, 1: 13.8%, 2: 70.6%, 3: 14.0%, 4: 1.40%, 5: 0.14%,
		 * 6+: 0.02%
		 *
		 * Loop iterations below (approximately), with optimization
		 * above:
		 *
		 * 0: 70.6%, 1: 27.8%, 2: 1.40%, 3: 0.14%, 4+: 0.02%
		 *----
		 */
		for (;;)
		{
			const uint64 vpDiv10 = div10(vp);
			const uint64 vmDiv10 = div10(vm);

			if (vpDiv10 <= vmDiv10)
				break;

			const uint64 vrDiv10 = div10(vr);
			const uint32 vrMod10 = (uint32) (vr - 10 * vrDiv10);

			roundUp = vrMod10 >= 5;
			vr = vrDiv10;
			vp = vpDiv10;
			vm = vmDiv10;
			++removed;
		}

		/*
		 * We need to take vr + 1 if vr is outside bounds or we need to round
		 * up.
		 */
		output = vr + (vr == vm || roundUp);
	}

	const int32 exp = e10 + removed;

	floating_decimal_64 fd;

	fd.exponent = exp;
	fd.mantissa = output;
	return fd;
}

static inline int
to_chars_df(const floating_decimal_64 v, const uint32 olength, char *const result)
{
	/* Step 5: Print the decimal representation. */
	int			index = 0;

	uint64		output = v.mantissa;
	int32		exp = v.exponent;

	/*----
	 * On entry, mantissa * 10^exp is the result to be output.
	 * Caller has already done the - sign if needed.
	 *
	 * We want to insert the point somewhere depending on the output length
	 * and exponent, which might mean adding zeros:
	 *
	 *            exp  | format
	 *            1+   |  ddddddddd000000
	 *            0    |  ddddddddd
	 *  -1 .. -len+1   |  dddddddd.d to d.ddddddddd
	 *  -len ...       |  0.ddddddddd to 0.000dddddd
	 */
	uint32		i = 0;
	int32		nexp = exp + olength;

	if (nexp <= 0)
	{
		/* -nexp is number of 0s to add after '.' */
		Assert(nexp >= -3);
		/* 0.000ddddd */
		index = 2 - nexp;
		/* won't need more than this many 0s */
		memcpy(result, "0.000000", 8);
	}
	else if (exp < 0)
	{
		/*
		 * dddd.dddd; leave space at the start and move the '.' in after
		 */
		index = 1;
	}
	else
	{
		/*
		 * We can save some code later by pre-filling with zeros. We know that
		 * there can be no more than 16 output digits in this form, otherwise
		 * we would not choose fixed-point output.
		 */
		Assert(exp < 16 && exp + olength <= 16);
		memset(result, '0', 16);
	}

	/*
	 * We prefer 32-bit operations, even on 64-bit platforms. We have at most
	 * 17 digits, and uint32 can store 9 digits. If output doesn't fit into
	 * uint32, we cut off 8 digits, so the rest will fit into uint32.
	 */
	if ((output >> 32) != 0)
	{
		/* Expensive 64-bit division. */
		const uint64 q = div1e8(output);
		uint32		output2 = (uint32) (output - 100000000 * q);
		const uint32 c = output2 % 10000;

		output = q;
		output2 /= 10000;

		const uint32 d = output2 % 10000;
		const uint32 c0 = (c % 100) << 1;
		const uint32 c1 = (c / 100) << 1;
		const uint32 d0 = (d % 100) << 1;
		const uint32 d1 = (d / 100) << 1;

		memcpy(result + index + olength - i - 2, DIGIT_TABLE + c0, 2);
		memcpy(result + index + olength - i - 4, DIGIT_TABLE + c1, 2);
		memcpy(result + index + olength - i - 6, DIGIT_TABLE + d0, 2);
		memcpy(result + index + olength - i - 8, DIGIT_TABLE + d1, 2);
		i += 8;
	}

	uint32		output2 = (uint32) output;

	while (output2 >= 10000)
	{
		const uint32 c = output2 - 10000 * (output2 / 10000);
		const uint32 c0 = (c % 100) << 1;
		const uint32 c1 = (c / 100) << 1;

		output2 /= 10000;
		memcpy(result + index + olength - i - 2, DIGIT_TABLE + c0, 2);
		memcpy(result + index + olength - i - 4, DIGIT_TABLE + c1, 2);
		i += 4;
	}
	if (output2 >= 100)
	{
		const uint32 c = (output2 % 100) << 1;

		output2 /= 100;
		memcpy(result + index + olength - i - 2, DIGIT_TABLE + c, 2);
		i += 2;
	}
	if (output2 >= 10)
	{
		const uint32 c = output2 << 1;

		memcpy(result + index + olength - i - 2, DIGIT_TABLE + c, 2);
	}
	else
	{
		result[index] = (char) ('0' + output2);
	}

	if (index == 1)
	{
		/*
		 * nexp is 1..15 here, representing the number of digits before the
		 * point. A value of 16 is not possible because we switch to
		 * scientific notation when the display exponent reaches 15.
		 */
		Assert(nexp < 16);
		/* gcc only seems to want to optimize memmove for small 2^n */
		if (nexp & 8)
		{
			memmove(result + index - 1, result + index, 8);
			index += 8;
		}
		if (nexp & 4)
		{
			memmove(result + index - 1, result + index, 4);
			index += 4;
		}
		if (nexp & 2)
		{
			memmove(result + index - 1, result + index, 2);
			index += 2;
		}
		if (nexp & 1)
		{
			result[index - 1] = result[index];
		}
		result[nexp] = '.';
		index = olength + 1;
	}
	else if (exp >= 0)
	{
		/* we supplied the trailing zeros earlier, now just set the length. */
		index = olength + exp;
	}
	else
	{
		index = olength + (2 - nexp);
	}

	return index;
}

static inline int
to_chars(floating_decimal_64 v, const bool sign, char *const result)
{
	/* Step 5: Print the decimal representation. */
	int			index = 0;

	uint64		output = v.mantissa;
	uint32		olength = decimalLength(output);
	int32		exp = v.exponent + olength - 1;

	if (sign)
	{
		result[index++] = '-';
	}

	/*
	 * The thresholds for fixed-point output are chosen to match printf
	 * defaults. Beware that both the code of to_chars_df and the value of
	 * DOUBLE_SHORTEST_DECIMAL_LEN are sensitive to these thresholds.
	 */
	if (exp >= -4 && exp < 15)
		return to_chars_df(v, olength, result + index) + sign;

	/*
	 * If v.exponent is exactly 0, we might have reached here via the small
	 * integer fast path, in which case v.mantissa might contain trailing
	 * (decimal) zeros. For scientific notation we need to move these zeros
	 * into the exponent. (For fixed point this doesn't matter, which is why
	 * we do this here rather than above.)
	 *
	 * Since we already calculated the display exponent (exp) above based on
	 * the old decimal length, that value does not change here. Instead, we
	 * just reduce the display length for each digit removed.
	 *
	 * If we didn't get here via the fast path, the raw exponent will not
	 * usually be 0, and there will be no trailing zeros, so we pay no more
	 * than one div10/multiply extra cost. We claw back half of that by
	 * checking for divisibility by 2 before dividing by 10.
	 */
	if (v.exponent == 0)
	{
		while ((output & 1) == 0)
		{
			const uint64 q = div10(output);
			const uint32 r = (uint32) (output - 10 * q);

			if (r != 0)
				break;
			output = q;
			--olength;
		}
	}

	/*----
	 * Print the decimal digits.
	 *
	 * The following code is equivalent to:
	 *
	 * for (uint32 i = 0; i < olength - 1; ++i) {
	 *   const uint32 c = output % 10; output /= 10;
	 *   result[index + olength - i] = (char) ('0' + c);
	 * }
	 * result[index] = '0' + output % 10;
	 *----
	 */

	uint32		i = 0;

	/*
	 * We prefer 32-bit operations, even on 64-bit platforms. We have at most
	 * 17 digits, and uint32 can store 9 digits. If output doesn't fit into
	 * uint32, we cut off 8 digits, so the rest will fit into uint32.
	 */
	if ((output >> 32) != 0)
	{
		/* Expensive 64-bit division. */
		const uint64 q = div1e8(output);
		uint32		output2 = (uint32) (output - 100000000 * q);

		output = q;

		const uint32 c = output2 % 10000;

		output2 /= 10000;

		const uint32 d = output2 % 10000;
		const uint32 c0 = (c % 100) << 1;
		const uint32 c1 = (c / 100) << 1;
		const uint32 d0 = (d % 100) << 1;
		const uint32 d1 = (d / 100) << 1;

		memcpy(result + index + olength - i - 1, DIGIT_TABLE + c0, 2);
		memcpy(result + index + olength - i - 3, DIGIT_TABLE + c1, 2);
		memcpy(result + index + olength - i - 5, DIGIT_TABLE + d0, 2);
		memcpy(result + index + olength - i - 7, DIGIT_TABLE + d1, 2);
		i += 8;
	}

	uint32		output2 = (uint32) output;

	while (output2 >= 10000)
	{
		const uint32 c = output2 - 10000 * (output2 / 10000);

		output2 /= 10000;

		const uint32 c0 = (c % 100) << 1;
		const uint32 c1 = (c / 100) << 1;

		memcpy(result + index + olength - i - 1, DIGIT_TABLE + c0, 2);
		memcpy(result + index + olength - i - 3, DIGIT_TABLE + c1, 2);
		i += 4;
	}
	if (output2 >= 100)
	{
		const uint32 c = (output2 % 100) << 1;

		output2 /= 100;
		memcpy(result + index + olength - i - 1, DIGIT_TABLE + c, 2);
		i += 2;
	}
	if (output2 >= 10)
	{
		const uint32 c = output2 << 1;

		/*
		 * We can't use memcpy here: the decimal dot goes between these two
		 * digits.
		 */
		result[index + olength - i] = DIGIT_TABLE[c + 1];
		result[index] = DIGIT_TABLE[c];
	}
	else
	{
		result[index] = (char) ('0' + output2);
	}

	/* Print decimal point if needed. */
	if (olength > 1)
	{
		result[index + 1] = '.';
		index += olength + 1;
	}
	else
	{
		++index;
	}

	/* Print the exponent. */
	result[index++] = 'e';
	if (exp < 0)
	{
		result[index++] = '-';
		exp = -exp;
	}
	else
		result[index++] = '+';

	if (exp >= 100)
	{
		const int32 c = exp % 10;

		memcpy(result + index, DIGIT_TABLE + 2 * (exp / 10), 2);
		result[index + 2] = (char) ('0' + c);
		index += 3;
	}
	else
	{
		memcpy(result + index, DIGIT_TABLE + 2 * exp, 2);
		index += 2;
	}

	return index;
}

static inline bool
d2d_small_int(const uint64 ieeeMantissa,
			  const uint32 ieeeExponent,
			  floating_decimal_64 *v)
{
	const int32 e2 = (int32) ieeeExponent - DOUBLE_BIAS - DOUBLE_MANTISSA_BITS;

	/*
	 * Avoid using multiple "return false;" here since it tends to provoke the
	 * compiler into inlining multiple copies of d2d, which is undesirable.
	 */

	if (e2 >= -DOUBLE_MANTISSA_BITS && e2 <= 0)
	{
		/*----
		 * Since 2^52 <= m2 < 2^53 and 0 <= -e2 <= 52:
		 *   1 <= f = m2 / 2^-e2 < 2^53.
		 *
		 * Test if the lower -e2 bits of the significand are 0, i.e. whether
		 * the fraction is 0. We can use ieeeMantissa here, since the implied
		 * 1 bit can never be tested by this; the implied 1 can only be part
		 * of a fraction if e2 < -DOUBLE_MANTISSA_BITS which we already
		 * checked. (e.g. 0.5 gives ieeeMantissa == 0 and e2 == -53)
		 */
		const uint64 mask = (UINT64CONST(1) << -e2) - 1;
		const uint64 fraction = ieeeMantissa & mask;

		if (fraction == 0)
		{
			/*----
			 * f is an integer in the range [1, 2^53).
			 * Note: mantissa might contain trailing (decimal) 0's.
			 * Note: since 2^53 < 10^16, there is no need to adjust
			 * decimalLength().
			 */
			const uint64 m2 = (UINT64CONST(1) << DOUBLE_MANTISSA_BITS) | ieeeMantissa;

			v->mantissa = m2 >> -e2;
			v->exponent = 0;
			return true;
		}
	}

	return false;
}

/*
 * Store the shortest decimal representation of the given double as an
 * UNTERMINATED string in the caller's supplied buffer (which must be at least
 * DOUBLE_SHORTEST_DECIMAL_LEN-1 bytes long).
 *
 * Returns the number of bytes stored.
 */
int
double_to_shortest_decimal_bufn(double f, char *result)
{
	/*
	 * Step 1: Decode the floating-point number, and unify normalized and
	 * subnormal cases.
	 */
	const uint64 bits = double_to_bits(f);

	/* Decode bits into sign, mantissa, and exponent. */
	const bool	ieeeSign = ((bits >> (DOUBLE_MANTISSA_BITS + DOUBLE_EXPONENT_BITS)) & 1) != 0;
	const uint64 ieeeMantissa = bits & ((UINT64CONST(1) << DOUBLE_MANTISSA_BITS) - 1);
	const uint32 ieeeExponent = (uint32) ((bits >> DOUBLE_MANTISSA_BITS) & ((1u << DOUBLE_EXPONENT_BITS) - 1));

	/* Case distinction; exit early for the easy cases. */
	if (ieeeExponent == ((1u << DOUBLE_EXPONENT_BITS) - 1u) || (ieeeExponent == 0 && ieeeMantissa == 0))
	{
		return copy_special_str(result, ieeeSign, (ieeeExponent != 0), (ieeeMantissa != 0));
	}

	floating_decimal_64 v;
	const bool	isSmallInt = d2d_small_int(ieeeMantissa, ieeeExponent, &v);

	if (!isSmallInt)
	{
		v = d2d(ieeeMantissa, ieeeExponent);
	}

	return to_chars(v, ieeeSign, result);
}

/*
 * Store the shortest decimal representation of the given double as a
 * null-terminated string in the caller's supplied buffer (which must be at
 * least DOUBLE_SHORTEST_DECIMAL_LEN bytes long).
 *
 * Returns the string length.
 */
int
double_to_shortest_decimal_buf(double f, char *result)
{
	const int	index = double_to_shortest_decimal_bufn(f, result);

	/* Terminate the string. */
	Assert(index < DOUBLE_SHORTEST_DECIMAL_LEN);
	result[index] = '\0';
	return index;
}

/*
 * Return the shortest decimal representation as a null-terminated palloc'd
 * string (outside the backend, uses malloc() instead).
 *
 * Caller is responsible for freeing the result.
 */
char *
double_to_shortest_decimal(double f)
{
	char	   *const result = (char *) palloc(DOUBLE_SHORTEST_DECIMAL_LEN);

	double_to_shortest_decimal_buf(f, result);
	return result;
}
