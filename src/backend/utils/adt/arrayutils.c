/*-------------------------------------------------------------------------
 *
 * arrayutils.c
 *	  This file contains some support routines required for array functions.
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/arrayutils.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/memutils.h"


/*
 * Convert subscript list into linear element number (from 0)
 *
 * We assume caller has already range-checked the dimensions and subscripts,
 * so no overflow is possible.
 */
int
ArrayGetOffset(int n, const int *dim, const int *lb, const int *indx)
{
	int			i,
				scale = 1,
				offset = 0;

	for (i = n - 1; i >= 0; i--)
	{
		offset += (indx[i] - lb[i]) * scale;
		scale *= dim[i];
	}
	return offset;
}

/*
 * Same, but subscripts are assumed 0-based, and use a scale array
 * instead of raw dimension data (see mda_get_prod to create scale array)
 */
int
ArrayGetOffset0(int n, const int *tup, const int *scale)
{
	int			i,
				lin = 0;

	for (i = 0; i < n; i++)
		lin += tup[i] * scale[i];
	return lin;
}

/*
 * Convert array dimensions into number of elements
 *
 * This must do overflow checking, since it is used to validate that a user
 * dimensionality request doesn't overflow what we can handle.
 *
 * We limit array sizes to at most about a quarter billion elements,
 * so that it's not necessary to check for overflow in quite so many
 * places --- for instance when palloc'ing Datum arrays.
 *
 * The multiplication overflow check only works on machines that have int64
 * arithmetic, but that is nearly all platforms these days, and doing check
 * divides for those that don't seems way too expensive.
 */
int
ArrayGetNItems(int ndim, const int *dims)
{
	int32		ret;
	int			i;

#define MaxArraySize ((Size) (MaxAllocSize / sizeof(Datum)))

	if (ndim <= 0)
		return 0;
	ret = 1;
	for (i = 0; i < ndim; i++)
	{
		int64		prod;

		/* A negative dimension implies that UB-LB overflowed ... */
		if (dims[i] < 0)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("array size exceeds the maximum allowed (%d)",
							(int) MaxArraySize)));

		prod = (int64) ret *(int64) dims[i];

		ret = (int32) prod;
		if ((int64) ret != prod)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("array size exceeds the maximum allowed (%d)",
							(int) MaxArraySize)));
	}
	Assert(ret >= 0);
	if ((Size) ret > MaxArraySize)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("array size exceeds the maximum allowed (%d)",
						(int) MaxArraySize)));
	return (int) ret;
}

/*
 * Compute ranges (sub-array dimensions) for an array slice
 *
 * We assume caller has validated slice endpoints, so overflow is impossible
 */
void
mda_get_range(int n, int *span, const int *st, const int *endp)
{
	int			i;

	for (i = 0; i < n; i++)
		span[i] = endp[i] - st[i] + 1;
}

/*
 * Compute products of array dimensions, ie, scale factors for subscripts
 *
 * We assume caller has validated dimensions, so overflow is impossible
 */
void
mda_get_prod(int n, const int *range, int *prod)
{
	int			i;

	prod[n - 1] = 1;
	for (i = n - 2; i >= 0; i--)
		prod[i] = prod[i + 1] * range[i + 1];
}

/*
 * From products of whole-array dimensions and spans of a sub-array,
 * compute offset distances needed to step through subarray within array
 *
 * We assume caller has validated dimensions, so overflow is impossible
 */
void
mda_get_offset_values(int n, int *dist, const int *prod, const int *span)
{
	int			i,
				j;

	dist[n - 1] = 0;
	for (j = n - 2; j >= 0; j--)
	{
		dist[j] = prod[j] - 1;
		for (i = j + 1; i < n; i++)
			dist[j] -= (span[i] - 1) * prod[i];
	}
}

/*
 * Generates the tuple that is lexicographically one greater than the current
 * n-tuple in "curr", with the restriction that the i-th element of "curr" is
 * less than the i-th element of "span".
 *
 * Returns -1 if no next tuple exists, else the subscript position (0..n-1)
 * corresponding to the dimension to advance along.
 *
 * We assume caller has validated dimensions, so overflow is impossible
 */
int
mda_next_tuple(int n, int *curr, const int *span)
{
	int			i;

	if (n <= 0)
		return -1;

	curr[n - 1] = (curr[n - 1] + 1) % span[n - 1];
	for (i = n - 1; i && curr[i] == 0; i--)
		curr[i - 1] = (curr[i - 1] + 1) % span[i - 1];

	if (i)
		return i;
	if (curr[0])
		return 0;

	return -1;
}

/*
 * ArrayGetIntegerTypmods: verify that argument is a 1-D cstring array,
 * and get the contents converted to integers.  Returns a palloc'd array
 * and places the length at *n.
 */
int32 *
ArrayGetIntegerTypmods(ArrayType *arr, int *n)
{
	int32	   *result;
	Datum	   *elem_values;
	int			i;

	if (ARR_ELEMTYPE(arr) != CSTRINGOID)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_ELEMENT_ERROR),
				 errmsg("typmod array must be type cstring[]")));

	if (ARR_NDIM(arr) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("typmod array must be one-dimensional")));

	if (array_contains_nulls(arr))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("typmod array must not contain nulls")));

	/* hardwired knowledge about cstring's representation details here */
	deconstruct_array(arr, CSTRINGOID,
					  -2, false, 'c',
					  &elem_values, NULL, n);

	result = (int32 *) palloc(*n * sizeof(int32));

	for (i = 0; i < *n; i++)
		result[i] = pg_atoi(DatumGetCString(elem_values[i]),
							sizeof(int32), '\0');

	pfree(elem_values);

	return result;
}
