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
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/misc.c,v 1.20 2000/08/07 00:51:14 tgl Exp $
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
 *	  Takes in an oid and a int4 X, and will return 'true' about 1/X of
 *	  the time.  If X == 0, this will always return true.
 *	  Useful for doing random sampling or subsetting.
 *
 * Example use:
 *	   select * from TEMP where oidrand(TEMP.oid, 10)
 * will return about 1/10 of the tuples in TEMP
 *
 * NOTE: the OID input is not used at all.  It is there just because of
 * an old optimizer bug: a qual expression containing no variables was
 * mistakenly assumed to be a constant.  Pretending to access the row's OID
 * prevented the optimizer from treating the oidrand() result as constant.
 */

static bool random_initialized = false;

Datum
oidrand(PG_FUNCTION_ARGS)
{
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

	srandom((unsigned int) X);
	random_initialized = true;
	PG_RETURN_BOOL(true);
}


Datum
userfntest(PG_FUNCTION_ARGS)
{
	int32		i = PG_GETARG_INT32(0);

	PG_RETURN_INT32(i);
}
