/*-------------------------------------------------------------------------
 *
 * knapsack.c
 *	  Knapsack problem solver
 *
 * Given input vectors of integral item weights (must be >= 0) and values
 * (double >= 0), compute the set of items which produces the greatest total
 * value without exceeding a specified total weight; each item is included at
 * most once (this is the 0/1 knapsack problem).  Weight 0 items will always be
 * included.
 *
 * The performance of this algorithm is pseudo-polynomial, O(nW) where W is the
 * weight limit.  To use with non-integral weights or approximate solutions,
 * the caller should pre-scale the input weights to a suitable range.  This
 * allows approximate solutions in polynomial time (the general case of the
 * exact problem is NP-hard).
 *
 * Copyright (c) 2017-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/lib/knapsack.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>
#include <limits.h>

#include "lib/knapsack.h"
#include "nodes/bitmapset.h"
#include "utils/memutils.h"

/*
 * DiscreteKnapsack
 *
 * The item_values input is optional; if omitted, all the items are assumed to
 * have value 1.
 *
 * Returns a Bitmapset of the 0..(n-1) indexes of the items chosen for
 * inclusion in the solution.
 *
 * This uses the usual dynamic-programming algorithm, adapted to reuse the
 * memory on each pass (by working from larger weights to smaller).  At the
 * start of pass number i, the values[w] array contains the largest value
 * computed with total weight <= w, using only items with indices < i; and
 * sets[w] contains the bitmap of items actually used for that value.  (The
 * bitmapsets are all pre-initialized with an unused high bit so that memory
 * allocation is done only once.)
 */
Bitmapset *
DiscreteKnapsack(int max_weight, int num_items,
				 int *item_weights, double *item_values)
{
	MemoryContext local_ctx = AllocSetContextCreate(CurrentMemoryContext,
													"Knapsack",
													ALLOCSET_SMALL_SIZES);
	MemoryContext oldctx = MemoryContextSwitchTo(local_ctx);
	double	   *values;
	Bitmapset **sets;
	Bitmapset  *result;
	int			i,
				j;

	Assert(max_weight >= 0);
	Assert(num_items > 0 && item_weights);

	values = palloc((1 + max_weight) * sizeof(double));
	sets = palloc((1 + max_weight) * sizeof(Bitmapset *));

	for (i = 0; i <= max_weight; ++i)
	{
		values[i] = 0;
		sets[i] = bms_make_singleton(num_items);
	}

	for (i = 0; i < num_items; ++i)
	{
		int			iw = item_weights[i];
		double		iv = item_values ? item_values[i] : 1;

		for (j = max_weight; j >= iw; --j)
		{
			int			ow = j - iw;

			if (values[j] <= values[ow] + iv)
			{
				/* copy sets[ow] to sets[j] without realloc */
				if (j != ow)
					sets[j] = bms_replace_members(sets[j], sets[ow]);

				sets[j] = bms_add_member(sets[j], i);

				values[j] = values[ow] + iv;
			}
		}
	}

	MemoryContextSwitchTo(oldctx);

	result = bms_del_member(bms_copy(sets[max_weight]), num_items);

	MemoryContextDelete(local_ctx);

	return result;
}
