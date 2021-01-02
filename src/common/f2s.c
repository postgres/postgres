/*---------------------------------------------------------------------------
 *
 * Ryu floating-point output for single precision.
 *
 * Portions Copyright (c) 2018-2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/common/f2s.c
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

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "common/shortest_dec.h"
#include "digit_table.h"
#include "ryu_common.h"

#define FLOAT_MANTISSA_BITS 23
#define FLOAT_EXPONENT_BITS 8
#define FLOAT_BIAS 127

/*
 * This table is generated (by the upstream) by PrintFloatLookupTable,
 * and modified (by us) to add UINT64CONST.
 */
#define FLOAT_POW5_INV_BITCOUNT 59
static const uint64 FLOAT_POW5_INV_SPLIT[31] = {
	UINT64CONST(576460752303423489), UINT64CONST(461168601842738791), UINT64CONST(368934881474191033), UINT64CONST(295147905179352826),
	UINT64CONST(472236648286964522), UINT64CONST(377789318629571618), UINT64CONST(302231454903657294), UINT64CONST(483570327845851670),
	UINT64CONST(386856262276681336), UINT64CONST(309485009821345069), UINT64CONST(495176015714152110), UINT64CONST(396140812571321688),
	UINT64CONST(316912650057057351), UINT64CONST(507060240091291761), UINT64CONST(405648192073033409), UINT64CONST(324518553658426727),
	UINT64CONST(519229685853482763), UINT64CONST(415383748682786211), UINT64CONST(332306998946228969), UINT64CONST(531691198313966350),
	UINT64CONST(425352958651173080), UINT64CONST(340282366920938464), UINT64CONST(544451787073501542), UINT64CONST(435561429658801234),
	UINT64CONST(348449143727040987), UINT64CONST(557518629963265579), UINT64CONST(446014903970612463), UINT64CONST(356811923176489971),
	UINT64CONST(570899077082383953), UINT64CONST(456719261665907162), UINT64CONST(365375409332725730)
};
#define FLOAT_POW5_BITCOUNT 61
static const uint64 FLOAT_POW5_SPLIT[47] = {
	UINT64CONST(1152921504606846976), UINT64CONST(1441151880758558720), UINT64CONST(1801439850948198400), UINT64CONST(2251799813685248000),
	UINT64CONST(1407374883553280000), UINT64CONST(1759218604441600000), UINT64CONST(2199023255552000000), UINT64CONST(1374389534720000000),
	UINT64CONST(1717986918400000000), UINT64CONST(2147483648000000000), UINT64CONST(1342177280000000000), UINT64CONST(1677721600000000000),
	UINT64CONST(2097152000000000000), UINT64CONST(1310720000000000000), UINT64CONST(1638400000000000000), UINT64CONST(2048000000000000000),
	UINT64CONST(1280000000000000000), UINT64CONST(1600000000000000000), UINT64CONST(2000000000000000000), UINT64CONST(1250000000000000000),
	UINT64CONST(1562500000000000000), UINT64CONST(1953125000000000000), UINT64CONST(1220703125000000000), UINT64CONST(1525878906250000000),
	UINT64CONST(1907348632812500000), UINT64CONST(1192092895507812500), UINT64CONST(1490116119384765625), UINT64CONST(1862645149230957031),
	UINT64CONST(1164153218269348144), UINT64CONST(1455191522836685180), UINT64CONST(1818989403545856475), UINT64CONST(2273736754432320594),
	UINT64CONST(1421085471520200371), UINT64CONST(1776356839400250464), UINT64CONST(2220446049250313080), UINT64CONST(1387778780781445675),
	UINT64CONST(1734723475976807094), UINT64CONST(2168404344971008868), UINT64CONST(1355252715606880542), UINT64CONST(1694065894508600678),
	UINT64CONST(2117582368135750847), UINT64CONST(1323488980084844279), UINT64CONST(1654361225106055349), UINT64CONST(2067951531382569187),
	UINT64CONST(1292469707114105741), UINT64CONST(1615587133892632177), UINT64CONST(2019483917365790221)
};

static inline uint32
pow5Factor(uint32 value)
{
	uint32		count = 0;

	for (;;)
	{
		Assert(value != 0);
		const uint32 q = value / 5;
		const uint32 r = value % 5;

		if (r != 0)
			break;

		value = q;
		++count;
	}
	return count;
}

/*  Returns true if value is divisible by 5^p. */
static inline bool
multipleOfPowerOf5(const uint32 value, const uint32 p)
{
	return pow5Factor(value) >= p;
}

/*  Returns true if value is divisible by 2^p. */
static inline bool
multipleOfPowerOf2(const uint32 value, const uint32 p)
{
	/* return __builtin_ctz(value) >= p; */
	return (value & ((1u << p) - 1)) == 0;
}

/*
 * It seems to be slightly faster to avoid uint128_t here, although the
 * generated code for uint128_t looks slightly nicer.
 */
static inline uint32
mulShift(const uint32 m, const uint64 factor, const int32 shift)
{
	/*
	 * The casts here help MSVC to avoid calls to the __allmul library
	 * function.
	 */
	const uint32 factorLo = (uint32) (factor);
	const uint32 factorHi = (uint32) (factor >> 32);
	const uint64 bits0 = (uint64) m * factorLo;
	const uint64 bits1 = (uint64) m * factorHi;

	Assert(shift > 32);

#ifdef RYU_32_BIT_PLATFORM

	/*
	 * On 32-bit platforms we can avoid a 64-bit shift-right since we only
	 * need the upper 32 bits of the result and the shift value is > 32.
	 */
	const uint32 bits0Hi = (uint32) (bits0 >> 32);
	uint32		bits1Lo = (uint32) (bits1);
	uint32		bits1Hi = (uint32) (bits1 >> 32);

	bits1Lo += bits0Hi;
	bits1Hi += (bits1Lo < bits0Hi);

	const int32 s = shift - 32;

	return (bits1Hi << (32 - s)) | (bits1Lo >> s);

#else							/* RYU_32_BIT_PLATFORM */

	const uint64 sum = (bits0 >> 32) + bits1;
	const uint64 shiftedSum = sum >> (shift - 32);

	Assert(shiftedSum <= PG_UINT32_MAX);
	return (uint32) shiftedSum;

#endif							/* RYU_32_BIT_PLATFORM */
}

static inline uint32
mulPow5InvDivPow2(const uint32 m, const uint32 q, const int32 j)
{
	return mulShift(m, FLOAT_POW5_INV_SPLIT[q], j);
}

static inline uint32
mulPow5divPow2(const uint32 m, const uint32 i, const int32 j)
{
	return mulShift(m, FLOAT_POW5_SPLIT[i], j);
}

static inline uint32
decimalLength(const uint32 v)
{
	/* Function precondition: v is not a 10-digit number. */
	/* (9 digits are sufficient for round-tripping.) */
	Assert(v < 1000000000);
	if (v >= 100000000)
	{
		return 9;
	}
	if (v >= 10000000)
	{
		return 8;
	}
	if (v >= 1000000)
	{
		return 7;
	}
	if (v >= 100000)
	{
		return 6;
	}
	if (v >= 10000)
	{
		return 5;
	}
	if (v >= 1000)
	{
		return 4;
	}
	if (v >= 100)
	{
		return 3;
	}
	if (v >= 10)
	{
		return 2;
	}
	return 1;
}

/*  A floating decimal representing m * 10^e. */
typedef struct floating_decimal_32
{
	uint32		mantissa;
	int32		exponent;
} floating_decimal_32;

static inline floating_decimal_32
f2d(const uint32 ieeeMantissa, const uint32 ieeeExponent)
{
	int32		e2;
	uint32		m2;

	if (ieeeExponent == 0)
	{
		/* We subtract 2 so that the bounds computation has 2 additional bits. */
		e2 = 1 - FLOAT_BIAS - FLOAT_MANTISSA_BITS - 2;
		m2 = ieeeMantissa;
	}
	else
	{
		e2 = ieeeExponent - FLOAT_BIAS - FLOAT_MANTISSA_BITS - 2;
		m2 = (1u << FLOAT_MANTISSA_BITS) | ieeeMantissa;
	}

#if STRICTLY_SHORTEST
	const bool	even = (m2 & 1) == 0;
	const bool	acceptBounds = even;
#else
	const bool	acceptBounds = false;
#endif

	/* Step 2: Determine the interval of legal decimal representations. */
	const uint32 mv = 4 * m2;
	const uint32 mp = 4 * m2 + 2;

	/* Implicit bool -> int conversion. True is 1, false is 0. */
	const uint32 mmShift = ieeeMantissa != 0 || ieeeExponent <= 1;
	const uint32 mm = 4 * m2 - 1 - mmShift;

	/* Step 3: Convert to a decimal power base using 64-bit arithmetic. */
	uint32		vr,
				vp,
				vm;
	int32		e10;
	bool		vmIsTrailingZeros = false;
	bool		vrIsTrailingZeros = false;
	uint8		lastRemovedDigit = 0;

	if (e2 >= 0)
	{
		const uint32 q = log10Pow2(e2);

		e10 = q;

		const int32 k = FLOAT_POW5_INV_BITCOUNT + pow5bits(q) - 1;
		const int32 i = -e2 + q + k;

		vr = mulPow5InvDivPow2(mv, q, i);
		vp = mulPow5InvDivPow2(mp, q, i);
		vm = mulPow5InvDivPow2(mm, q, i);

		if (q != 0 && (vp - 1) / 10 <= vm / 10)
		{
			/*
			 * We need to know one removed digit even if we are not going to
			 * loop below. We could use q = X - 1 above, except that would
			 * require 33 bits for the result, and we've found that 32-bit
			 * arithmetic is faster even on 64-bit machines.
			 */
			const int32 l = FLOAT_POW5_INV_BITCOUNT + pow5bits(q - 1) - 1;

			lastRemovedDigit = (uint8) (mulPow5InvDivPow2(mv, q - 1, -e2 + q - 1 + l) % 10);
		}
		if (q <= 9)
		{
			/*
			 * The largest power of 5 that fits in 24 bits is 5^10, but q <= 9
			 * seems to be safe as well.
			 *
			 * Only one of mp, mv, and mm can be a multiple of 5, if any.
			 */
			if (mv % 5 == 0)
			{
				vrIsTrailingZeros = multipleOfPowerOf5(mv, q);
			}
			else if (acceptBounds)
			{
				vmIsTrailingZeros = multipleOfPowerOf5(mm, q);
			}
			else
			{
				vp -= multipleOfPowerOf5(mp, q);
			}
		}
	}
	else
	{
		const uint32 q = log10Pow5(-e2);

		e10 = q + e2;

		const int32 i = -e2 - q;
		const int32 k = pow5bits(i) - FLOAT_POW5_BITCOUNT;
		int32		j = q - k;

		vr = mulPow5divPow2(mv, i, j);
		vp = mulPow5divPow2(mp, i, j);
		vm = mulPow5divPow2(mm, i, j);

		if (q != 0 && (vp - 1) / 10 <= vm / 10)
		{
			j = q - 1 - (pow5bits(i + 1) - FLOAT_POW5_BITCOUNT);
			lastRemovedDigit = (uint8) (mulPow5divPow2(mv, i + 1, j) % 10);
		}
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
		else if (q < 31)
		{
			/* TODO(ulfjack):Use a tighter bound here. */
			vrIsTrailingZeros = multipleOfPowerOf2(mv, q - 1);
		}
	}

	/*
	 * Step 4: Find the shortest decimal representation in the interval of
	 * legal representations.
	 */
	uint32		removed = 0;
	uint32		output;

	if (vmIsTrailingZeros || vrIsTrailingZeros)
	{
		/* General case, which happens rarely (~4.0%). */
		while (vp / 10 > vm / 10)
		{
			vmIsTrailingZeros &= vm - (vm / 10) * 10 == 0;
			vrIsTrailingZeros &= lastRemovedDigit == 0;
			lastRemovedDigit = (uint8) (vr % 10);
			vr /= 10;
			vp /= 10;
			vm /= 10;
			++removed;
		}
		if (vmIsTrailingZeros)
		{
			while (vm % 10 == 0)
			{
				vrIsTrailingZeros &= lastRemovedDigit == 0;
				lastRemovedDigit = (uint8) (vr % 10);
				vr /= 10;
				vp /= 10;
				vm /= 10;
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
		 * Specialized for the common case (~96.0%). Percentages below are
		 * relative to this.
		 *
		 * Loop iterations below (approximately): 0: 13.6%, 1: 70.7%, 2:
		 * 14.1%, 3: 1.39%, 4: 0.14%, 5+: 0.01%
		 */
		while (vp / 10 > vm / 10)
		{
			lastRemovedDigit = (uint8) (vr % 10);
			vr /= 10;
			vp /= 10;
			vm /= 10;
			++removed;
		}

		/*
		 * We need to take vr + 1 if vr is outside bounds or we need to round
		 * up.
		 */
		output = vr + (vr == vm || lastRemovedDigit >= 5);
	}

	const int32 exp = e10 + removed;

	floating_decimal_32 fd;

	fd.exponent = exp;
	fd.mantissa = output;
	return fd;
}

static inline int
to_chars_f(const floating_decimal_32 v, const uint32 olength, char *const result)
{
	/* Step 5: Print the decimal representation. */
	int			index = 0;

	uint32		output = v.mantissa;
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
		/* copy 8 bytes rather than 5 to let compiler optimize */
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
		 * there can be no more than 6 output digits in this form, otherwise
		 * we would not choose fixed-point output. memset 8 rather than 6
		 * bytes to let the compiler optimize it.
		 */
		Assert(exp < 6 && exp + olength <= 6);
		memset(result, '0', 8);
	}

	while (output >= 10000)
	{
		const uint32 c = output - 10000 * (output / 10000);
		const uint32 c0 = (c % 100) << 1;
		const uint32 c1 = (c / 100) << 1;

		output /= 10000;

		memcpy(result + index + olength - i - 2, DIGIT_TABLE + c0, 2);
		memcpy(result + index + olength - i - 4, DIGIT_TABLE + c1, 2);
		i += 4;
	}
	if (output >= 100)
	{
		const uint32 c = (output % 100) << 1;

		output /= 100;
		memcpy(result + index + olength - i - 2, DIGIT_TABLE + c, 2);
		i += 2;
	}
	if (output >= 10)
	{
		const uint32 c = output << 1;

		memcpy(result + index + olength - i - 2, DIGIT_TABLE + c, 2);
	}
	else
	{
		result[index] = (char) ('0' + output);
	}

	if (index == 1)
	{
		/*
		 * nexp is 1..6 here, representing the number of digits before the
		 * point. A value of 7+ is not possible because we switch to
		 * scientific notation when the display exponent reaches 6.
		 */
		Assert(nexp < 7);
		/* gcc only seems to want to optimize memmove for small 2^n */
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
to_chars(const floating_decimal_32 v, const bool sign, char *const result)
{
	/* Step 5: Print the decimal representation. */
	int			index = 0;

	uint32		output = v.mantissa;
	uint32		olength = decimalLength(output);
	int32		exp = v.exponent + olength - 1;

	if (sign)
		result[index++] = '-';

	/*
	 * The thresholds for fixed-point output are chosen to match printf
	 * defaults. Beware that both the code of to_chars_f and the value of
	 * FLOAT_SHORTEST_DECIMAL_LEN are sensitive to these thresholds.
	 */
	if (exp >= -4 && exp < 6)
		return to_chars_f(v, olength, result + index) + sign;

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
			const uint32 q = output / 10;
			const uint32 r = output - 10 * q;

			if (r != 0)
				break;
			output = q;
			--olength;
		}
	}

	/*----
	 * Print the decimal digits.
	 * The following code is equivalent to:
	 *
	 * for (uint32 i = 0; i < olength - 1; ++i) {
	 *   const uint32 c = output % 10; output /= 10;
	 *   result[index + olength - i] = (char) ('0' + c);
	 * }
	 * result[index] = '0' + output % 10;
	 */
	uint32		i = 0;

	while (output >= 10000)
	{
		const uint32 c = output - 10000 * (output / 10000);
		const uint32 c0 = (c % 100) << 1;
		const uint32 c1 = (c / 100) << 1;

		output /= 10000;

		memcpy(result + index + olength - i - 1, DIGIT_TABLE + c0, 2);
		memcpy(result + index + olength - i - 3, DIGIT_TABLE + c1, 2);
		i += 4;
	}
	if (output >= 100)
	{
		const uint32 c = (output % 100) << 1;

		output /= 100;
		memcpy(result + index + olength - i - 1, DIGIT_TABLE + c, 2);
		i += 2;
	}
	if (output >= 10)
	{
		const uint32 c = output << 1;

		/*
		 * We can't use memcpy here: the decimal dot goes between these two
		 * digits.
		 */
		result[index + olength - i] = DIGIT_TABLE[c + 1];
		result[index] = DIGIT_TABLE[c];
	}
	else
	{
		result[index] = (char) ('0' + output);
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

	memcpy(result + index, DIGIT_TABLE + 2 * exp, 2);
	index += 2;

	return index;
}

static inline bool
f2d_small_int(const uint32 ieeeMantissa,
			  const uint32 ieeeExponent,
			  floating_decimal_32 *v)
{
	const int32 e2 = (int32) ieeeExponent - FLOAT_BIAS - FLOAT_MANTISSA_BITS;

	/*
	 * Avoid using multiple "return false;" here since it tends to provoke the
	 * compiler into inlining multiple copies of f2d, which is undesirable.
	 */

	if (e2 >= -FLOAT_MANTISSA_BITS && e2 <= 0)
	{
		/*----
		 * Since 2^23 <= m2 < 2^24 and 0 <= -e2 <= 23:
		 *   1 <= f = m2 / 2^-e2 < 2^24.
		 *
		 * Test if the lower -e2 bits of the significand are 0, i.e. whether
		 * the fraction is 0. We can use ieeeMantissa here, since the implied
		 * 1 bit can never be tested by this; the implied 1 can only be part
		 * of a fraction if e2 < -FLOAT_MANTISSA_BITS which we already
		 * checked. (e.g. 0.5 gives ieeeMantissa == 0 and e2 == -24)
		 */
		const uint32 mask = (1U << -e2) - 1;
		const uint32 fraction = ieeeMantissa & mask;

		if (fraction == 0)
		{
			/*----
			 * f is an integer in the range [1, 2^24).
			 * Note: mantissa might contain trailing (decimal) 0's.
			 * Note: since 2^24 < 10^9, there is no need to adjust
			 * decimalLength().
			 */
			const uint32 m2 = (1U << FLOAT_MANTISSA_BITS) | ieeeMantissa;

			v->mantissa = m2 >> -e2;
			v->exponent = 0;
			return true;
		}
	}

	return false;
}

/*
 * Store the shortest decimal representation of the given float as an
 * UNTERMINATED string in the caller's supplied buffer (which must be at least
 * FLOAT_SHORTEST_DECIMAL_LEN-1 bytes long).
 *
 * Returns the number of bytes stored.
 */
int
float_to_shortest_decimal_bufn(float f, char *result)
{
	/*
	 * Step 1: Decode the floating-point number, and unify normalized and
	 * subnormal cases.
	 */
	const uint32 bits = float_to_bits(f);

	/* Decode bits into sign, mantissa, and exponent. */
	const bool	ieeeSign = ((bits >> (FLOAT_MANTISSA_BITS + FLOAT_EXPONENT_BITS)) & 1) != 0;
	const uint32 ieeeMantissa = bits & ((1u << FLOAT_MANTISSA_BITS) - 1);
	const uint32 ieeeExponent = (bits >> FLOAT_MANTISSA_BITS) & ((1u << FLOAT_EXPONENT_BITS) - 1);

	/* Case distinction; exit early for the easy cases. */
	if (ieeeExponent == ((1u << FLOAT_EXPONENT_BITS) - 1u) || (ieeeExponent == 0 && ieeeMantissa == 0))
	{
		return copy_special_str(result, ieeeSign, (ieeeExponent != 0), (ieeeMantissa != 0));
	}

	floating_decimal_32 v;
	const bool	isSmallInt = f2d_small_int(ieeeMantissa, ieeeExponent, &v);

	if (!isSmallInt)
	{
		v = f2d(ieeeMantissa, ieeeExponent);
	}

	return to_chars(v, ieeeSign, result);
}

/*
 * Store the shortest decimal representation of the given float as a
 * null-terminated string in the caller's supplied buffer (which must be at
 * least FLOAT_SHORTEST_DECIMAL_LEN bytes long).
 *
 * Returns the string length.
 */
int
float_to_shortest_decimal_buf(float f, char *result)
{
	const int	index = float_to_shortest_decimal_bufn(f, result);

	/* Terminate the string. */
	Assert(index < FLOAT_SHORTEST_DECIMAL_LEN);
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
float_to_shortest_decimal(float f)
{
	char	   *const result = (char *) palloc(FLOAT_SHORTEST_DECIMAL_LEN);

	float_to_shortest_decimal_buf(f, result);
	return result;
}
