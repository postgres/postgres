/*-------------------------------------------------------------------------
 *
 * test_int128.c
 *	  Testbed for roll-our-own 128-bit integer arithmetic.
 *
 * This is a standalone test program that compares the behavior of an
 * implementation in int128.h to an (assumed correct) int128 native type.
 *
 * Copyright (c) 2017-2025, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/test/modules/test_int128/test_int128.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <time.h>

/* Require a native int128 type */
#ifdef HAVE_INT128

/*
 * By default, we test the non-native implementation in int128.h; but
 * by predefining USE_NATIVE_INT128 to 1, you can test the native
 * implementation, just to be sure.
 */
#ifndef USE_NATIVE_INT128
#define USE_NATIVE_INT128 0
#endif

#include "common/int128.h"
#include "common/pg_prng.h"

/*
 * We assume the parts of this union are laid out compatibly.
 */
typedef union
{
	int128		i128;
	INT128		I128;
	struct
	{
#ifdef WORDS_BIGENDIAN
		int64		hi;
		uint64		lo;
#else
		uint64		lo;
		int64		hi;
#endif
	}			hl;
}			test128;

#define INT128_HEX_FORMAT	"%016" PRIx64 "%016" PRIx64

/*
 * Control version of comparator.
 */
static inline int
my_int128_compare(int128 x, int128 y)
{
	if (x < y)
		return -1;
	if (x > y)
		return 1;
	return 0;
}

/*
 * Main program.
 *
 * Generates a lot of random numbers and tests the implementation for each.
 * The results should be reproducible, since we use a fixed PRNG seed.
 *
 * You can give a loop count if you don't like the default 1B iterations.
 */
int
main(int argc, char **argv)
{
	long		count;

	pg_prng_seed(&pg_global_prng_state, (uint64) time(NULL));

	if (argc >= 2)
		count = strtol(argv[1], NULL, 0);
	else
		count = 1000000000;

	while (count-- > 0)
	{
		int64		x = pg_prng_uint64(&pg_global_prng_state);
		int64		y = pg_prng_uint64(&pg_global_prng_state);
		int64		z = pg_prng_uint64(&pg_global_prng_state);
		int64		w = pg_prng_uint64(&pg_global_prng_state);
		int32		z32 = (int32) z;
		test128		t1;
		test128		t2;
		test128		t3;
		int32		r1;
		int32		r2;

		/* check unsigned addition */
		t1.hl.hi = x;
		t1.hl.lo = y;
		t2 = t1;
		t1.i128 += (int128) (uint64) z;
		int128_add_uint64(&t2.I128, (uint64) z);

		if (t1.hl.hi != t2.hl.hi || t1.hl.lo != t2.hl.lo)
		{
			printf(INT128_HEX_FORMAT " + unsigned %016" PRIx64 "\n", x, y, z);
			printf("native = " INT128_HEX_FORMAT "\n", t1.hl.hi, t1.hl.lo);
			printf("result = " INT128_HEX_FORMAT "\n", t2.hl.hi, t2.hl.lo);
			return 1;
		}

		/* check signed addition */
		t1.hl.hi = x;
		t1.hl.lo = y;
		t2 = t1;
		t1.i128 += (int128) z;
		int128_add_int64(&t2.I128, z);

		if (t1.hl.hi != t2.hl.hi || t1.hl.lo != t2.hl.lo)
		{
			printf(INT128_HEX_FORMAT " + signed %016" PRIx64 "\n", x, y, z);
			printf("native = " INT128_HEX_FORMAT "\n", t1.hl.hi, t1.hl.lo);
			printf("result = " INT128_HEX_FORMAT "\n", t2.hl.hi, t2.hl.lo);
			return 1;
		}

		/* check 128-bit signed addition */
		t1.hl.hi = x;
		t1.hl.lo = y;
		t2 = t1;
		t3.hl.hi = z;
		t3.hl.lo = w;
		t1.i128 += t3.i128;
		int128_add_int128(&t2.I128, t3.I128);

		if (t1.hl.hi != t2.hl.hi || t1.hl.lo != t2.hl.lo)
		{
			printf(INT128_HEX_FORMAT " + " INT128_HEX_FORMAT "\n", x, y, z, w);
			printf("native = " INT128_HEX_FORMAT "\n", t1.hl.hi, t1.hl.lo);
			printf("result = " INT128_HEX_FORMAT "\n", t2.hl.hi, t2.hl.lo);
			return 1;
		}

		/* check unsigned subtraction */
		t1.hl.hi = x;
		t1.hl.lo = y;
		t2 = t1;
		t1.i128 -= (int128) (uint64) z;
		int128_sub_uint64(&t2.I128, (uint64) z);

		if (t1.hl.hi != t2.hl.hi || t1.hl.lo != t2.hl.lo)
		{
			printf(INT128_HEX_FORMAT " - unsigned %016" PRIx64 "\n", x, y, z);
			printf("native = " INT128_HEX_FORMAT "\n", t1.hl.hi, t1.hl.lo);
			printf("result = " INT128_HEX_FORMAT "\n", t2.hl.hi, t2.hl.lo);
			return 1;
		}

		/* check signed subtraction */
		t1.hl.hi = x;
		t1.hl.lo = y;
		t2 = t1;
		t1.i128 -= (int128) z;
		int128_sub_int64(&t2.I128, z);

		if (t1.hl.hi != t2.hl.hi || t1.hl.lo != t2.hl.lo)
		{
			printf(INT128_HEX_FORMAT " - signed %016" PRIx64 "\n", x, y, z);
			printf("native = " INT128_HEX_FORMAT "\n", t1.hl.hi, t1.hl.lo);
			printf("result = " INT128_HEX_FORMAT "\n", t2.hl.hi, t2.hl.lo);
			return 1;
		}

		/* check 64x64-bit multiply-add */
		t1.hl.hi = x;
		t1.hl.lo = y;
		t2 = t1;
		t1.i128 += (int128) z * (int128) w;
		int128_add_int64_mul_int64(&t2.I128, z, w);

		if (t1.hl.hi != t2.hl.hi || t1.hl.lo != t2.hl.lo)
		{
			printf(INT128_HEX_FORMAT " + %016" PRIx64 " * %016" PRIx64 "\n", x, y, z, w);
			printf("native = " INT128_HEX_FORMAT "\n", t1.hl.hi, t1.hl.lo);
			printf("result = " INT128_HEX_FORMAT "\n", t2.hl.hi, t2.hl.lo);
			return 1;
		}

		/* check 64x64-bit multiply-subtract */
		t1.hl.hi = x;
		t1.hl.lo = y;
		t2 = t1;
		t1.i128 -= (int128) z * (int128) w;
		int128_sub_int64_mul_int64(&t2.I128, z, w);

		if (t1.hl.hi != t2.hl.hi || t1.hl.lo != t2.hl.lo)
		{
			printf(INT128_HEX_FORMAT " - %016" PRIx64 " * %016" PRIx64 "\n", x, y, z, w);
			printf("native = " INT128_HEX_FORMAT "\n", t1.hl.hi, t1.hl.lo);
			printf("result = " INT128_HEX_FORMAT "\n", t2.hl.hi, t2.hl.lo);
			return 1;
		}

		/* check 128/32-bit division */
		t3.hl.hi = x;
		t3.hl.lo = y;
		t1.i128 = t3.i128 / z32;
		r1 = (int32) (t3.i128 % z32);
		t2 = t3;
		int128_div_mod_int32(&t2.I128, z32, &r2);

		if (t1.hl.hi != t2.hl.hi || t1.hl.lo != t2.hl.lo)
		{
			printf(INT128_HEX_FORMAT " / signed %08X\n", t3.hl.hi, t3.hl.lo, z32);
			printf("native = " INT128_HEX_FORMAT "\n", t1.hl.hi, t1.hl.lo);
			printf("result = " INT128_HEX_FORMAT "\n", t2.hl.hi, t2.hl.lo);
			return 1;
		}
		if (r1 != r2)
		{
			printf(INT128_HEX_FORMAT " %% signed %08X\n", t3.hl.hi, t3.hl.lo, z32);
			printf("native = %08X\n", r1);
			printf("result = %08X\n", r2);
			return 1;
		}

		/* check comparison */
		t1.hl.hi = x;
		t1.hl.lo = y;
		t2.hl.hi = z;
		t2.hl.lo = w;

		if (my_int128_compare(t1.i128, t2.i128) !=
			int128_compare(t1.I128, t2.I128))
		{
			printf("comparison failure: %d vs %d\n",
				   my_int128_compare(t1.i128, t2.i128),
				   int128_compare(t1.I128, t2.I128));
			printf("arg1 = " INT128_HEX_FORMAT "\n", t1.hl.hi, t1.hl.lo);
			printf("arg2 = " INT128_HEX_FORMAT "\n", t2.hl.hi, t2.hl.lo);
			return 1;
		}

		/* check case with identical hi parts; above will hardly ever hit it */
		t2.hl.hi = x;

		if (my_int128_compare(t1.i128, t2.i128) !=
			int128_compare(t1.I128, t2.I128))
		{
			printf("comparison failure: %d vs %d\n",
				   my_int128_compare(t1.i128, t2.i128),
				   int128_compare(t1.I128, t2.I128));
			printf("arg1 = " INT128_HEX_FORMAT "\n", t1.hl.hi, t1.hl.lo);
			printf("arg2 = " INT128_HEX_FORMAT "\n", t2.hl.hi, t2.hl.lo);
			return 1;
		}
	}

	return 0;
}

#else							/* ! HAVE_INT128 */

/*
 * For now, do nothing if we don't have a native int128 type.
 */
int
main(int argc, char **argv)
{
	printf("skipping tests: no native int128 type\n");
	return 0;
}

#endif
