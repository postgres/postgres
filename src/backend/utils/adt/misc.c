/*-------------------------------------------------------------------------
 *
 * misc.c--
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/misc.c,v 1.12 1998/02/24 03:47:26 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>
#include <sys/file.h>
#include "postgres.h"
#include "utils/datum.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"

/*-------------------------------------------------------------------------
 * Check if data is Null
 */
bool
nullvalue(Datum value, bool *isNull)
{
	if (*isNull)
	{
		*isNull = false;
		return (true);
	}
	return (false);

}

/*----------------------------------------------------------------------*
 *	   check if data is not Null										*
 *--------------------------------------------------------------------- */
bool
nonnullvalue(Datum value, bool *isNull)
{
	if (*isNull)
	{
		*isNull = false;
		return (false);
	}
	return (true);

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
bool
oidrand(Oid o, int32 X)
{
	bool		result;

	if (X == 0)
		return true;

	result = (random() % X == 0);
	return result;
}

/*
   oidsrand(int32 X) -
	  seeds the random number generator
	  always return true
*/
bool
oidsrand(int32 X)
{
	srand(X);
	return true;
}



int32
userfntest(int i)
{
	return (i);
}
