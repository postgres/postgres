/*-------------------------------------------------------------------------
 *
 * pseudorandomfuncs.c
 *	  Functions giving SQL access to a pseudorandom number generator.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/pseudorandomfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "common/pg_prng.h"
#include "miscadmin.h"
#include "utils/fmgrprotos.h"
#include "utils/numeric.h"
#include "utils/timestamp.h"

/* Shared PRNG state used by all the random functions */
static pg_prng_state prng_state;
static bool prng_seed_set = false;

/*
 * initialize_prng() -
 *
 *	Initialize (seed) the PRNG, if not done yet in this process.
 */
static void
initialize_prng(void)
{
	if (unlikely(!prng_seed_set))
	{
		/*
		 * If possible, seed the PRNG using high-quality random bits. Should
		 * that fail for some reason, we fall back on a lower-quality seed
		 * based on current time and PID.
		 */
		if (unlikely(!pg_prng_strong_seed(&prng_state)))
		{
			TimestampTz now = GetCurrentTimestamp();
			uint64		iseed;

			/* Mix the PID with the most predictable bits of the timestamp */
			iseed = (uint64) now ^ ((uint64) MyProcPid << 32);
			pg_prng_seed(&prng_state, iseed);
		}
		prng_seed_set = true;
	}
}

/*
 * setseed() -
 *
 *	Seed the PRNG from a specified value in the range [-1.0, 1.0].
 */
Datum
setseed(PG_FUNCTION_ARGS)
{
	float8		seed = PG_GETARG_FLOAT8(0);

	if (seed < -1 || seed > 1 || isnan(seed))
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("setseed parameter %g is out of allowed range [-1,1]",
					   seed));

	pg_prng_fseed(&prng_state, seed);
	prng_seed_set = true;

	PG_RETURN_VOID();
}

/*
 * drandom() -
 *
 *	Returns a random number chosen uniformly in the range [0.0, 1.0).
 */
Datum
drandom(PG_FUNCTION_ARGS)
{
	float8		result;

	initialize_prng();

	/* pg_prng_double produces desired result range [0.0, 1.0) */
	result = pg_prng_double(&prng_state);

	PG_RETURN_FLOAT8(result);
}

/*
 * drandom_normal() -
 *
 *	Returns a random number from a normal distribution.
 */
Datum
drandom_normal(PG_FUNCTION_ARGS)
{
	float8		mean = PG_GETARG_FLOAT8(0);
	float8		stddev = PG_GETARG_FLOAT8(1);
	float8		result,
				z;

	initialize_prng();

	/* Get random value from standard normal(mean = 0.0, stddev = 1.0) */
	z = pg_prng_double_normal(&prng_state);
	/* Transform the normal standard variable (z) */
	/* using the target normal distribution parameters */
	result = (stddev * z) + mean;

	PG_RETURN_FLOAT8(result);
}

/*
 * int4random() -
 *
 *	Returns a random 32-bit integer chosen uniformly in the specified range.
 */
Datum
int4random(PG_FUNCTION_ARGS)
{
	int32		rmin = PG_GETARG_INT32(0);
	int32		rmax = PG_GETARG_INT32(1);
	int32		result;

	if (rmin > rmax)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("lower bound must be less than or equal to upper bound"));

	initialize_prng();

	result = (int32) pg_prng_int64_range(&prng_state, rmin, rmax);

	PG_RETURN_INT32(result);
}

/*
 * int8random() -
 *
 *	Returns a random 64-bit integer chosen uniformly in the specified range.
 */
Datum
int8random(PG_FUNCTION_ARGS)
{
	int64		rmin = PG_GETARG_INT64(0);
	int64		rmax = PG_GETARG_INT64(1);
	int64		result;

	if (rmin > rmax)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("lower bound must be less than or equal to upper bound"));

	initialize_prng();

	result = pg_prng_int64_range(&prng_state, rmin, rmax);

	PG_RETURN_INT64(result);
}

/*
 * numeric_random() -
 *
 *	Returns a random numeric value chosen uniformly in the specified range.
 */
Datum
numeric_random(PG_FUNCTION_ARGS)
{
	Numeric		rmin = PG_GETARG_NUMERIC(0);
	Numeric		rmax = PG_GETARG_NUMERIC(1);
	Numeric		result;

	initialize_prng();

	result = random_numeric(&prng_state, rmin, rmax);

	PG_RETURN_NUMERIC(result);
}
