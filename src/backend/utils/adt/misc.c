/*-------------------------------------------------------------------------
 *
 * misc.c
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/misc.c,v 1.19 2000/06/05 07:28:52 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>
#include <sys/file.h>
#include <time.h>

#include "postgres.h"

#include "utils/builtins.h"


/*
 * Check if data is Null
 */
Datum
nullvalue(PG_FUNCTION_ARGS)
{
	if (PG_ARGISNULL(0))
		PG_RETURN_BOOL(true);
	PG_RETURN_BOOL(false);
}

/*
 * Check if data is not Null
 */
Datum
nonnullvalue(PG_FUNCTION_ARGS)
{
	if (PG_ARGISNULL(0))
		PG_RETURN_BOOL(false);
	PG_RETURN_BOOL(true);
}

/*
 * oidrand (oid o, int4 X)-
 *	  takes in an oid and a int4 X, and will return 'true'
 *	about 1/X of the time.
 *	  Useful for doing random sampling or subsetting.
 *	if X == 0, this will always return true;
 *
 * Example use:
 *	   select * from TEMP where oidrand(TEMP.oid, 10)
 * will return about 1/10 of the tuples in TEMP
 *
 */

static bool random_initialized = false;

Datum
oidrand(PG_FUNCTION_ARGS)
{
	/* XXX seems like we ought to be using the oid for something? */
#ifdef NOT_USED
	Oid			o = PG_GETARG_OID(0);
#endif
	int32		X = PG_GETARG_INT32(1);
	bool		result;

	if (X == 0)
		PG_RETURN_BOOL(true);

	/*
	 * We do this because the cancel key is actually a random, so we don't
	 * want them to be able to request random numbers using our postmaster
	 * seeded value.
	 */
	if (!random_initialized)
	{
		srandom((unsigned int) time(NULL));
		random_initialized = true;
	}

	result = (random() % X == 0);
	PG_RETURN_BOOL(result);
}

/*
   oidsrand(int32 X) -
	  seeds the random number generator
	  always returns true
*/
Datum
oidsrand(PG_FUNCTION_ARGS)
{
	int32		X = PG_GETARG_INT32(0);

	srand(X);
	random_initialized = true;
	PG_RETURN_BOOL(true);
}


Datum
userfntest(PG_FUNCTION_ARGS)
{
	int32		i = PG_GETARG_INT32(0);

	PG_RETURN_INT32(i);
}
