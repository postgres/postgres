/*-------------------------------------------------------------------------
 *
 * testint128.c
 *	  Testbed for roll-our-own 128-bit integer arithmetic.
 *
 * This is a standalone test program that compares the behavior of an
 * implementation in int128.h to an (assumed correct) int128 native type.
 *
 * Copyright (c) 2017-2024, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/tools/testint128.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

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
	union
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

	pg_prng_seed(&pg_global_prng_state, 0);

	if (argc >= 2)
		count = strtol(argv[1], NULL, 0);
	else
		count = 1000000000;

	while (count-- > 0)
	{
		int64		x = pg_prng_uint64(&pg_global_prng_state);
		int64		y = pg_prng_uint64(&pg_global_prng_state);
		int64		z = pg_prng_uint64(&pg_global_prng_state);
		test128		t1;
		test128		t2;

		/* check unsigned addition */
		t1.hl.hi = x;
		t1.hl.lo = y;
		t2 = t1;
		t1.i128 += (int128) (uint64) z;
		int128_add_uint64(&t2.I128, (uint64) z);

		if (t1.hl.hi != t2.hl.hi || t1.hl.lo != t2.hl.lo)
		{
			printf("%016lX%016lX + unsigned %lX\n", x, y, z);
			printf("native = %016lX%016lX\n", t1.hl.hi, t1.hl.lo);
			printf("result = %016lX%016lX\n", t2.hl.hi, t2.hl.lo);
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
			printf("%016lX%016lX + signed %lX\n", x, y, z);
			printf("native = %016lX%016lX\n", t1.hl.hi, t1.hl.lo);
			printf("result = %016lX%016lX\n", t2.hl.hi, t2.hl.lo);
			return 1;
		}

		/* check multiplication */
		t1.i128 = (int128) x * (int128) y;

		t2.hl.hi = t2.hl.lo = 0;
		int128_add_int64_mul_int64(&t2.I128, x, y);

		if (t1.hl.hi != t2.hl.hi || t1.hl.lo != t2.hl.lo)
		{
			printf("%lX * %lX\n", x, y);
			printf("native = %016lX%016lX\n", t1.hl.hi, t1.hl.lo);
			printf("result = %016lX%016lX\n", t2.hl.hi, t2.hl.lo);
			return 1;
		}

		/* check comparison */
		t1.hl.hi = x;
		t1.hl.lo = y;
		t2.hl.hi = z;
		t2.hl.lo = pg_prng_uint64(&pg_global_prng_state);

		if (my_int128_compare(t1.i128, t2.i128) !=
			int128_compare(t1.I128, t2.I128))
		{
			printf("comparison failure: %d vs %d\n",
				   my_int128_compare(t1.i128, t2.i128),
				   int128_compare(t1.I128, t2.I128));
			printf("arg1 = %016lX%016lX\n", t1.hl.hi, t1.hl.lo);
			printf("arg2 = %016lX%016lX\n", t2.hl.hi, t2.hl.lo);
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
			printf("arg1 = %016lX%016lX\n", t1.hl.hi, t1.hl.lo);
			printf("arg2 = %016lX%016lX\n", t2.hl.hi, t2.hl.lo);
			return 1;
		}
	}

	return 0;
}
