/*-------------------------------------------------------------------------
 *
 * geo_selfuncs.c
 *	  Selectivity routines registered in the operator catalog in the
 *	  "oprrest" and "oprjoin" attributes.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/geo_selfuncs.c,v 1.14 2000/04/12 17:15:50 momjian Exp $
 *
 *	XXX These are totally bogus.  Perhaps someone will make them do
 *	something reasonable, someday.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/builtins.h"


/*
 *	Selectivity functions for rtrees.  These are bogus -- unless we know
 *	the actual key distribution in the index, we can't make a good prediction
 *	of the selectivity of these operators.
 *
 *	Note: the values used here may look unreasonably small.  Perhaps they
 *	are.  For now, we want to make sure that the optimizer will make use
 *	of an r-tree index if one is available, so the selectivity had better
 *	be fairly small.
 *
 *	In general, rtrees need to search multiple subtrees in order to guarantee
 *	that all occurrences of the same key have been found.  Because of this,
 *	the estimated cost for scanning the index ought to be higher than the
 *	output selectivity would indicate.	rtcostestimate(), over in selfuncs.c,
 *	ought to be adjusted accordingly --- but until we can generate somewhat
 *	realistic numbers here, it hardly matters...
 */


/*
 * Selectivity for operators that depend on area, such as "overlap".
 */

float64
areasel(Oid opid,
		Oid relid,
		AttrNumber attno,
		Datum value,
		int32 flag)
{
	float64		result;

	result = (float64) palloc(sizeof(float64data));
	*result = 0.05;
	return result;
}

float64
areajoinsel(Oid opid,
			Oid relid1,
			AttrNumber attno1,
			Oid relid2,
			AttrNumber attno2)
{
	float64		result;

	result = (float64) palloc(sizeof(float64data));
	*result = 0.05;
	return result;
}

/*
 *	positionsel
 *
 * How likely is a box to be strictly left of (right of, above, below)
 * a given box?
 */

float64
positionsel(Oid opid,
			Oid relid,
			AttrNumber attno,
			Datum value,
			int32 flag)
{
	float64		result;

	result = (float64) palloc(sizeof(float64data));
	*result = 0.1;
	return result;
}

float64
positionjoinsel(Oid opid,
				Oid relid1,
				AttrNumber attno1,
				Oid relid2,
				AttrNumber attno2)
{
	float64		result;

	result = (float64) palloc(sizeof(float64data));
	*result = 0.1;
	return result;
}

/*
 *	contsel -- How likely is a box to contain (be contained by) a given box?
 *
 * This is a tighter constraint than "overlap", so produce a smaller
 * estimate than areasel does.
 */

float64
contsel(Oid opid,
		Oid relid,
		AttrNumber attno,
		Datum value,
		int32 flag)
{
	float64		result;

	result = (float64) palloc(sizeof(float64data));
	*result = 0.01;
	return result;
}

float64
contjoinsel(Oid opid,
			Oid relid1,
			AttrNumber attno1,
			Oid relid2,
			AttrNumber attno2)
{
	float64		result;

	result = (float64) palloc(sizeof(float64data));
	*result = 0.01;
	return result;
}
