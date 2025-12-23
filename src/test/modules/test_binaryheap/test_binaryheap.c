/*--------------------------------------------------------------------------
 *
 * test_binaryheap.c
 *		Test correctness of binary heap implementation.
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_binaryheap/test_binaryheap.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "common/int.h"
#include "common/pg_prng.h"
#include "fmgr.h"
#include "lib/binaryheap.h"

PG_MODULE_MAGIC;

/*
 * Test binaryheap_comparator for max-heap of integers.
 */
static int
int_cmp(Datum a, Datum b, void *arg)
{
	return pg_cmp_s32(DatumGetInt32(a), DatumGetInt32(b));
}

/*
 * Loops through all nodes and returns the maximum value.
 */
static int
get_max_from_heap(binaryheap *heap)
{
	int			max = -1;

	for (int i = 0; i < binaryheap_size(heap); i++)
		max = Max(max, DatumGetInt32(binaryheap_get_node(heap, i)));

	return max;
}

/*
 * Generate a random permutation of the integers 0..size-1.
 */
static int *
get_permutation(int size)
{
	int		   *permutation = (int *) palloc(size * sizeof(int));

	permutation[0] = 0;

	/*
	 * This is the "inside-out" variant of the Fisher-Yates shuffle algorithm.
	 * Notionally, we append each new value to the array and then swap it with
	 * a randomly-chosen array element (possibly including itself, else we
	 * fail to generate permutations with the last integer last).  The swap
	 * step can be optimized by combining it with the insertion.
	 */
	for (int i = 1; i < size; i++)
	{
		int			j = pg_prng_uint64_range(&pg_global_prng_state, 0, i);

		if (j < i)				/* avoid fetching undefined data if j=i */
			permutation[i] = permutation[j];
		permutation[j] = i;
	}

	return permutation;
}

/*
 * Ensure that the heap property holds for the given heap, i.e., each parent is
 * greater than or equal to its children.
 */
static void
verify_heap_property(binaryheap *heap)
{
	for (int i = 0; i < binaryheap_size(heap); i++)
	{
		int			left = 2 * i + 1;
		int			right = 2 * i + 2;
		int			parent_val = DatumGetInt32(binaryheap_get_node(heap, i));

		if (left < binaryheap_size(heap) &&
			parent_val < DatumGetInt32(binaryheap_get_node(heap, left)))
			elog(ERROR, "parent node less than left child");

		if (right < binaryheap_size(heap) &&
			parent_val < DatumGetInt32(binaryheap_get_node(heap, right)))
			elog(ERROR, "parent node less than right child");
	}
}

/*
 * Check correctness of basic operations.
 */
static void
test_basic(int size)
{
	binaryheap *heap = binaryheap_allocate(size, int_cmp, NULL);
	int		   *permutation = get_permutation(size);

	if (!binaryheap_empty(heap))
		elog(ERROR, "new heap not empty");
	if (binaryheap_size(heap) != 0)
		elog(ERROR, "wrong size for new heap");

	for (int i = 0; i < size; i++)
	{
		binaryheap_add(heap, Int32GetDatum(permutation[i]));
		verify_heap_property(heap);
	}

	if (binaryheap_empty(heap))
		elog(ERROR, "heap empty after adding values");
	if (binaryheap_size(heap) != size)
		elog(ERROR, "wrong size for heap after adding values");

	if (DatumGetInt32(binaryheap_first(heap)) != get_max_from_heap(heap))
		elog(ERROR, "incorrect root node after adding values");

	for (int i = 0; i < size; i++)
	{
		int			expected = get_max_from_heap(heap);
		int			actual = DatumGetInt32(binaryheap_remove_first(heap));

		if (actual != expected)
			elog(ERROR, "incorrect root node after removing root");
		verify_heap_property(heap);
	}

	if (!binaryheap_empty(heap))
		elog(ERROR, "heap not empty after removing all nodes");
}

/*
 * Test building heap after unordered additions.
 */
static void
test_build(int size)
{
	binaryheap *heap = binaryheap_allocate(size, int_cmp, NULL);
	int		   *permutation = get_permutation(size);

	for (int i = 0; i < size; i++)
		binaryheap_add_unordered(heap, Int32GetDatum(permutation[i]));

	if (binaryheap_size(heap) != size)
		elog(ERROR, "wrong size for heap after unordered additions");

	binaryheap_build(heap);
	verify_heap_property(heap);
}

/*
 * Test removing nodes.
 */
static void
test_remove_node(int size)
{
	binaryheap *heap = binaryheap_allocate(size, int_cmp, NULL);
	int		   *permutation = get_permutation(size);
	int			remove_count = pg_prng_uint64_range(&pg_global_prng_state,
													0, size - 1);

	for (int i = 0; i < size; i++)
		binaryheap_add(heap, Int32GetDatum(permutation[i]));

	for (int i = 0; i < remove_count; i++)
	{
		int			idx = pg_prng_uint64_range(&pg_global_prng_state,
											   0, binaryheap_size(heap) - 1);

		binaryheap_remove_node(heap, idx);
		verify_heap_property(heap);
	}

	if (binaryheap_size(heap) != size - remove_count)
		elog(ERROR, "wrong size after removing nodes");
}

/*
 * Test replacing the root node.
 */
static void
test_replace_first(int size)
{
	binaryheap *heap = binaryheap_allocate(size, int_cmp, NULL);

	for (int i = 0; i < size; i++)
		binaryheap_add(heap, Int32GetDatum(i));

	/*
	 * Replace root with a value smaller than everything in the heap.
	 */
	binaryheap_replace_first(heap, Int32GetDatum(-1));
	verify_heap_property(heap);

	/*
	 * Replace root with a value in the middle of the heap.
	 */
	binaryheap_replace_first(heap, Int32GetDatum(size / 2));
	verify_heap_property(heap);

	/*
	 * Replace root with a larger value than everything in the heap.
	 */
	binaryheap_replace_first(heap, Int32GetDatum(size + 1));
	verify_heap_property(heap);
}

/*
 * Test duplicate values.
 */
static void
test_duplicates(int size)
{
	binaryheap *heap = binaryheap_allocate(size, int_cmp, NULL);
	int			dup = pg_prng_uint64_range(&pg_global_prng_state, 0, size - 1);

	for (int i = 0; i < size; i++)
		binaryheap_add(heap, Int32GetDatum(dup));

	for (int i = 0; i < size; i++)
	{
		if (DatumGetInt32(binaryheap_remove_first(heap)) != dup)
			elog(ERROR, "unexpected value in heap with duplicates");
	}
}

/*
 * Test resetting.
 */
static void
test_reset(int size)
{
	binaryheap *heap = binaryheap_allocate(size, int_cmp, NULL);

	for (int i = 0; i < size; i++)
		binaryheap_add(heap, Int32GetDatum(i));

	binaryheap_reset(heap);

	if (!binaryheap_empty(heap))
		elog(ERROR, "heap not empty after resetting");
}

/*
 * SQL-callable entry point to perform all tests.
 */
PG_FUNCTION_INFO_V1(test_binaryheap);

Datum
test_binaryheap(PG_FUNCTION_ARGS)
{
	static const int test_sizes[] = {1, 2, 3, 10, 100, 1000};

	for (int i = 0; i < sizeof(test_sizes) / sizeof(int); i++)
	{
		int			size = test_sizes[i];

		test_basic(size);
		test_build(size);
		test_remove_node(size);
		test_replace_first(size);
		test_duplicates(size);
		test_reset(size);
	}

	PG_RETURN_VOID();
}
