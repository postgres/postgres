/*-------------------------------------------------------------------------
 *
 * Pseudo-Random Number Generator
 *
 * We use Blackman and Vigna's xoroshiro128** 1.0 algorithm
 * to have a small, fast PRNG suitable for generating reasonably
 * good-quality 64-bit data.  This should not be considered
 * cryptographically strong, however.
 *
 * About these generators: https://prng.di.unimi.it/
 * See also https://en.wikipedia.org/wiki/List_of_random_number_generators
 *
 * Copyright (c) 2021-2025, PostgreSQL Global Development Group
 *
 * src/common/pg_prng.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#include <math.h>

#include "common/pg_prng.h"
#include "port/pg_bitutils.h"

/* X/Open (XSI) requires <math.h> to provide M_PI, but core POSIX does not */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


/* process-wide state vector */
pg_prng_state pg_global_prng_state;


/*
 * 64-bit rotate left
 */
static inline uint64
rotl(uint64 x, int bits)
{
	return (x << bits) | (x >> (64 - bits));
}

/*
 * The basic xoroshiro128** algorithm.
 * Generates and returns a 64-bit uniformly distributed number,
 * updating the state vector for next time.
 *
 * Note: the state vector must not be all-zeroes, as that is a fixed point.
 */
static uint64
xoroshiro128ss(pg_prng_state *state)
{
	uint64		s0 = state->s0,
				sx = state->s1 ^ s0,
				val = rotl(s0 * 5, 7) * 9;

	/* update state */
	state->s0 = rotl(s0, 24) ^ sx ^ (sx << 16);
	state->s1 = rotl(sx, 37);

	return val;
}

/*
 * We use this generator just to fill the xoroshiro128** state vector
 * from a 64-bit seed.
 */
static uint64
splitmix64(uint64 *state)
{
	/* state update */
	uint64		val = (*state += UINT64CONST(0x9E3779B97f4A7C15));

	/* value extraction */
	val = (val ^ (val >> 30)) * UINT64CONST(0xBF58476D1CE4E5B9);
	val = (val ^ (val >> 27)) * UINT64CONST(0x94D049BB133111EB);

	return val ^ (val >> 31);
}

/*
 * Initialize the PRNG state from a 64-bit integer,
 * taking care that we don't produce all-zeroes.
 */
void
pg_prng_seed(pg_prng_state *state, uint64 seed)
{
	state->s0 = splitmix64(&seed);
	state->s1 = splitmix64(&seed);
	/* Let's just make sure we didn't get all-zeroes */
	(void) pg_prng_seed_check(state);
}

/*
 * Initialize the PRNG state from a double in the range [-1.0, 1.0],
 * taking care that we don't produce all-zeroes.
 */
void
pg_prng_fseed(pg_prng_state *state, double fseed)
{
	/* Assume there's about 52 mantissa bits; the sign contributes too. */
	int64		seed = ((double) ((UINT64CONST(1) << 52) - 1)) * fseed;

	pg_prng_seed(state, (uint64) seed);
}

/*
 * Validate a PRNG seed value.
 */
bool
pg_prng_seed_check(pg_prng_state *state)
{
	/*
	 * If the seeding mechanism chanced to produce all-zeroes, insert
	 * something nonzero.  Anything would do; use Knuth's LCG parameters.
	 */
	if (unlikely(state->s0 == 0 && state->s1 == 0))
	{
		state->s0 = UINT64CONST(0x5851F42D4C957F2D);
		state->s1 = UINT64CONST(0x14057B7EF767814F);
	}

	/* As a convenience for the pg_prng_strong_seed macro, return true */
	return true;
}

/*
 * Select a random uint64 uniformly from the range [0, PG_UINT64_MAX].
 */
uint64
pg_prng_uint64(pg_prng_state *state)
{
	return xoroshiro128ss(state);
}

/*
 * Select a random uint64 uniformly from the range [rmin, rmax].
 * If the range is empty, rmin is always produced.
 */
uint64
pg_prng_uint64_range(pg_prng_state *state, uint64 rmin, uint64 rmax)
{
	uint64		val;

	if (likely(rmax > rmin))
	{
		/*
		 * Use bitmask rejection method to generate an offset in 0..range.
		 * Each generated val is less than twice "range", so on average we
		 * should not have to iterate more than twice.
		 */
		uint64		range = rmax - rmin;
		uint32		rshift = 63 - pg_leftmost_one_pos64(range);

		do
		{
			val = xoroshiro128ss(state) >> rshift;
		} while (val > range);
	}
	else
		val = 0;

	return rmin + val;
}

/*
 * Select a random int64 uniformly from the range [PG_INT64_MIN, PG_INT64_MAX].
 */
int64
pg_prng_int64(pg_prng_state *state)
{
	return (int64) xoroshiro128ss(state);
}

/*
 * Select a random int64 uniformly from the range [0, PG_INT64_MAX].
 */
int64
pg_prng_int64p(pg_prng_state *state)
{
	return (int64) (xoroshiro128ss(state) & UINT64CONST(0x7FFFFFFFFFFFFFFF));
}

/*
 * Select a random int64 uniformly from the range [rmin, rmax].
 * If the range is empty, rmin is always produced.
 */
int64
pg_prng_int64_range(pg_prng_state *state, int64 rmin, int64 rmax)
{
	int64		val;

	if (likely(rmax > rmin))
	{
		uint64		uval;

		/*
		 * Use pg_prng_uint64_range().  Can't simply pass it rmin and rmax,
		 * since (uint64) rmin will be larger than (uint64) rmax if rmin < 0.
		 */
		uval = (uint64) rmin +
			pg_prng_uint64_range(state, 0, (uint64) rmax - (uint64) rmin);

		/*
		 * Safely convert back to int64, avoiding implementation-defined
		 * behavior for values larger than PG_INT64_MAX.  Modern compilers
		 * will reduce this to a simple assignment.
		 */
		if (uval > PG_INT64_MAX)
			val = (int64) (uval - PG_INT64_MIN) + PG_INT64_MIN;
		else
			val = (int64) uval;
	}
	else
		val = rmin;

	return val;
}

/*
 * Select a random uint32 uniformly from the range [0, PG_UINT32_MAX].
 */
uint32
pg_prng_uint32(pg_prng_state *state)
{
	/*
	 * Although xoroshiro128** is not known to have any weaknesses in
	 * randomness of low-order bits, we prefer to use the upper bits of its
	 * result here and below.
	 */
	uint64		v = xoroshiro128ss(state);

	return (uint32) (v >> 32);
}

/*
 * Select a random int32 uniformly from the range [PG_INT32_MIN, PG_INT32_MAX].
 */
int32
pg_prng_int32(pg_prng_state *state)
{
	uint64		v = xoroshiro128ss(state);

	return (int32) (v >> 32);
}

/*
 * Select a random int32 uniformly from the range [0, PG_INT32_MAX].
 */
int32
pg_prng_int32p(pg_prng_state *state)
{
	uint64		v = xoroshiro128ss(state);

	return (int32) (v >> 33);
}

/*
 * Select a random double uniformly from the range [0.0, 1.0).
 *
 * Note: if you want a result in the range (0.0, 1.0], the standard way
 * to get that is "1.0 - pg_prng_double(state)".
 */
double
pg_prng_double(pg_prng_state *state)
{
	uint64		v = xoroshiro128ss(state);

	/*
	 * As above, assume there's 52 mantissa bits in a double.  This result
	 * could round to 1.0 if double's precision is less than that; but we
	 * assume IEEE float arithmetic elsewhere in Postgres, so this seems OK.
	 */
	return ldexp((double) (v >> (64 - 52)), -52);
}

/*
 * Select a random double from the normal distribution with
 * mean = 0.0 and stddev = 1.0.
 *
 * To get a result from a different normal distribution use
 *   STDDEV * pg_prng_double_normal() + MEAN
 *
 * Uses https://en.wikipedia.org/wiki/Box%E2%80%93Muller_transform
 */
double
pg_prng_double_normal(pg_prng_state *state)
{
	double		u1,
				u2,
				z0;

	/*
	 * pg_prng_double generates [0, 1), but for the basic version of the
	 * Box-Muller transform the two uniformly distributed random numbers are
	 * expected to be in (0, 1]; in particular we'd better not compute log(0).
	 */
	u1 = 1.0 - pg_prng_double(state);
	u2 = 1.0 - pg_prng_double(state);

	/* Apply Box-Muller transform to get one normal-valued output */
	z0 = sqrt(-2.0 * log(u1)) * sin(2.0 * M_PI * u2);
	return z0;
}

/*
 * Select a random boolean value.
 */
bool
pg_prng_bool(pg_prng_state *state)
{
	uint64		v = xoroshiro128ss(state);

	return (bool) (v >> 63);
}
