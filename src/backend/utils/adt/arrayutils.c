/*-------------------------------------------------------------------------
 *
 * arrayutils.c
 *	  This file contains some support routines required for array functions.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/arrayutils.c,v 1.14 2003/08/04 02:40:04 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/array.h"


/* Convert subscript list into linear element number (from 0) */
int
ArrayGetOffset(int n, int *dim, int *lb, int *indx)
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

/* Same, but subscripts are assumed 0-based, and use a scale array
 * instead of raw dimension data (see mda_get_prod to create scale array)
 */
int
ArrayGetOffset0(int n, int *tup, int *scale)
{
	int			i,
				lin = 0;

	for (i = 0; i < n; i++)
		lin += tup[i] * scale[i];
	return lin;
}

/* Convert array dimensions into number of elements */
int
ArrayGetNItems(int n, int *a)
{
	int			i,
				ret;

	if (n <= 0)
		return 0;
	ret = 1;
	for (i = 0; i < n; i++)
		ret *= a[i];
	return ret;
}

/* Compute ranges (sub-array dimensions) for an array slice */
void
mda_get_range(int n, int *span, int *st, int *endp)
{
	int			i;

	for (i = 0; i < n; i++)
		span[i] = endp[i] - st[i] + 1;
}

/* Compute products of array dimensions, ie, scale factors for subscripts */
void
mda_get_prod(int n, int *range, int *prod)
{
	int			i;

	prod[n - 1] = 1;
	for (i = n - 2; i >= 0; i--)
		prod[i] = prod[i + 1] * range[i + 1];
}

/* From products of whole-array dimensions and spans of a sub-array,
 * compute offset distances needed to step through subarray within array
 */
void
mda_get_offset_values(int n, int *dist, int *prod, int *span)
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

/*-----------------------------------------------------------------------------
  generates the tuple that is lexicographically one greater than the current
  n-tuple in "curr", with the restriction that the i-th element of "curr" is
  less than the i-th element of "span".
  Returns -1 if no next tuple exists, else the subscript position (0..n-1)
  corresponding to the dimension to advance along.
  -----------------------------------------------------------------------------
*/
int
mda_next_tuple(int n, int *curr, int *span)
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
