/*
 * binaryheap.h
 *
 * A simple binary heap implementation
 *
 * Portions Copyright (c) 2012-2016, PostgreSQL Global Development Group
 *
 * src/include/lib/binaryheap.h
 */

#ifndef BINARYHEAP_H
#define BINARYHEAP_H

/*
 * For a max-heap, the comparator must return <0 iff a < b, 0 iff a == b,
 * and >0 iff a > b.  For a min-heap, the conditions are reversed.
 */
typedef int (*binaryheap_comparator) (Datum a, Datum b, void *arg);

/*
 * binaryheap
 *
 *		bh_size			how many nodes are currently in "nodes"
 *		bh_space		how many nodes can be stored in "nodes"
 *		bh_has_heap_property	no unordered operations since last heap build
 *		bh_compare		comparison function to define the heap property
 *		bh_arg			user data for comparison function
 *		bh_nodes		variable-length array of "space" nodes
 */
typedef struct binaryheap
{
	int			bh_size;
	int			bh_space;
	bool		bh_has_heap_property;	/* debugging cross-check */
	binaryheap_comparator bh_compare;
	void	   *bh_arg;
	Datum		bh_nodes[FLEXIBLE_ARRAY_MEMBER];
} binaryheap;

extern binaryheap *binaryheap_allocate(int capacity,
					binaryheap_comparator compare,
					void *arg);
extern void binaryheap_reset(binaryheap *heap);
extern void binaryheap_free(binaryheap *heap);
extern void binaryheap_add_unordered(binaryheap *heap, Datum d);
extern void binaryheap_build(binaryheap *heap);
extern void binaryheap_add(binaryheap *heap, Datum d);
extern Datum binaryheap_first(binaryheap *heap);
extern Datum binaryheap_remove_first(binaryheap *heap);
extern void binaryheap_replace_first(binaryheap *heap, Datum d);

#define binaryheap_empty(h)			((h)->bh_size == 0)

#endif   /* BINARYHEAP_H */
