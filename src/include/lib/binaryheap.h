/*
 * binaryheap.h
 *
 * A simple binary heap implementation
 *
 * Portions Copyright (c) 2012-2024, PostgreSQL Global Development Group
 *
 * src/include/lib/binaryheap.h
 */

#ifndef BINARYHEAP_H
#define BINARYHEAP_H

/*
 * We provide a Datum-based API for backend code and a void *-based API for
 * frontend code (since the Datum definitions are not available to frontend
 * code).  You should typically avoid using bh_node_type directly and instead
 * use Datum or void * as appropriate.
 */
#ifdef FRONTEND
typedef void *bh_node_type;
#else
typedef Datum bh_node_type;
#endif

/*
 * For a max-heap, the comparator must return <0 iff a < b, 0 iff a == b,
 * and >0 iff a > b.  For a min-heap, the conditions are reversed.
 */
typedef int (*binaryheap_comparator) (bh_node_type a, bh_node_type b, void *arg);

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
	bh_node_type bh_nodes[FLEXIBLE_ARRAY_MEMBER];
} binaryheap;

extern binaryheap *binaryheap_allocate(int capacity,
									   binaryheap_comparator compare,
									   void *arg);
extern void binaryheap_reset(binaryheap *heap);
extern void binaryheap_free(binaryheap *heap);
extern void binaryheap_add_unordered(binaryheap *heap, bh_node_type d);
extern void binaryheap_build(binaryheap *heap);
extern void binaryheap_add(binaryheap *heap, bh_node_type d);
extern bh_node_type binaryheap_first(binaryheap *heap);
extern bh_node_type binaryheap_remove_first(binaryheap *heap);
extern void binaryheap_remove_node(binaryheap *heap, int n);
extern void binaryheap_replace_first(binaryheap *heap, bh_node_type d);

#define binaryheap_empty(h)			((h)->bh_size == 0)
#define binaryheap_size(h)			((h)->bh_size)
#define binaryheap_get_node(h, n)	((h)->bh_nodes[n])

#endif							/* BINARYHEAP_H */
