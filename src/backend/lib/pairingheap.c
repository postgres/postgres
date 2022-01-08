/*-------------------------------------------------------------------------
 *
 * pairingheap.c
 *	  A Pairing Heap implementation
 *
 * A pairing heap is a data structure that's useful for implementing
 * priority queues. It is simple to implement, and provides amortized O(1)
 * insert and find-min operations, and amortized O(log n) delete-min.
 *
 * The pairing heap was first described in this paper:
 *
 *	Michael L. Fredman, Robert Sedgewick, Daniel D. Sleator, and Robert E.
 *	 Tarjan. 1986.
 *	The pairing heap: a new form of self-adjusting heap.
 *	Algorithmica 1, 1 (January 1986), pages 111-129. DOI: 10.1007/BF01840439
 *
 * Portions Copyright (c) 2012-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/lib/pairingheap.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "lib/pairingheap.h"

static pairingheap_node *merge(pairingheap *heap, pairingheap_node *a,
							   pairingheap_node *b);
static pairingheap_node *merge_children(pairingheap *heap,
										pairingheap_node *children);

/*
 * pairingheap_allocate
 *
 * Returns a pointer to a newly-allocated heap, with the heap property defined
 * by the given comparator function, which will be invoked with the additional
 * argument specified by 'arg'.
 */
pairingheap *
pairingheap_allocate(pairingheap_comparator compare, void *arg)
{
	pairingheap *heap;

	heap = (pairingheap *) palloc(sizeof(pairingheap));
	heap->ph_compare = compare;
	heap->ph_arg = arg;

	heap->ph_root = NULL;

	return heap;
}

/*
 * pairingheap_free
 *
 * Releases memory used by the given pairingheap.
 *
 * Note: The nodes in the heap are not freed!
 */
void
pairingheap_free(pairingheap *heap)
{
	pfree(heap);
}

/*
 * A helper function to merge two subheaps into one.
 *
 * The subheap with smaller value is put as a child of the other one (assuming
 * a max-heap).
 *
 * The next_sibling and prev_or_parent pointers of the input nodes are
 * ignored. On return, the returned node's next_sibling and prev_or_parent
 * pointers are garbage.
 */
static pairingheap_node *
merge(pairingheap *heap, pairingheap_node *a, pairingheap_node *b)
{
	if (a == NULL)
		return b;
	if (b == NULL)
		return a;

	/* swap 'a' and 'b' so that 'a' is the one with larger value */
	if (heap->ph_compare(a, b, heap->ph_arg) < 0)
	{
		pairingheap_node *tmp;

		tmp = a;
		a = b;
		b = tmp;
	}

	/* and put 'b' as a child of 'a' */
	if (a->first_child)
		a->first_child->prev_or_parent = b;
	b->prev_or_parent = a;
	b->next_sibling = a->first_child;
	a->first_child = b;

	return a;
}

/*
 * pairingheap_add
 *
 * Adds the given node to the heap in O(1) time.
 */
void
pairingheap_add(pairingheap *heap, pairingheap_node *node)
{
	node->first_child = NULL;

	/* Link the new node as a new tree */
	heap->ph_root = merge(heap, heap->ph_root, node);
	heap->ph_root->prev_or_parent = NULL;
	heap->ph_root->next_sibling = NULL;
}

/*
 * pairingheap_first
 *
 * Returns a pointer to the first (root, topmost) node in the heap without
 * modifying the heap. The caller must ensure that this routine is not used on
 * an empty heap. Always O(1).
 */
pairingheap_node *
pairingheap_first(pairingheap *heap)
{
	Assert(!pairingheap_is_empty(heap));

	return heap->ph_root;
}

/*
 * pairingheap_remove_first
 *
 * Removes the first (root, topmost) node in the heap and returns a pointer to
 * it after rebalancing the heap. The caller must ensure that this routine is
 * not used on an empty heap. O(log n) amortized.
 */
pairingheap_node *
pairingheap_remove_first(pairingheap *heap)
{
	pairingheap_node *result;
	pairingheap_node *children;

	Assert(!pairingheap_is_empty(heap));

	/* Remove the root, and form a new heap of its children. */
	result = heap->ph_root;
	children = result->first_child;

	heap->ph_root = merge_children(heap, children);
	if (heap->ph_root)
	{
		heap->ph_root->prev_or_parent = NULL;
		heap->ph_root->next_sibling = NULL;
	}

	return result;
}

/*
 * Remove 'node' from the heap. O(log n) amortized.
 */
void
pairingheap_remove(pairingheap *heap, pairingheap_node *node)
{
	pairingheap_node *children;
	pairingheap_node *replacement;
	pairingheap_node *next_sibling;
	pairingheap_node **prev_ptr;

	/*
	 * If the removed node happens to be the root node, do it with
	 * pairingheap_remove_first().
	 */
	if (node == heap->ph_root)
	{
		(void) pairingheap_remove_first(heap);
		return;
	}

	/*
	 * Before we modify anything, remember the removed node's first_child and
	 * next_sibling pointers.
	 */
	children = node->first_child;
	next_sibling = node->next_sibling;

	/*
	 * Also find the pointer to the removed node in its previous sibling, or
	 * if this is the first child of its parent, in its parent.
	 */
	if (node->prev_or_parent->first_child == node)
		prev_ptr = &node->prev_or_parent->first_child;
	else
		prev_ptr = &node->prev_or_parent->next_sibling;
	Assert(*prev_ptr == node);

	/*
	 * If this node has children, make a new subheap of the children and link
	 * the subheap in place of the removed node. Otherwise just unlink this
	 * node.
	 */
	if (children)
	{
		replacement = merge_children(heap, children);

		replacement->prev_or_parent = node->prev_or_parent;
		replacement->next_sibling = node->next_sibling;
		*prev_ptr = replacement;
		if (next_sibling)
			next_sibling->prev_or_parent = replacement;
	}
	else
	{
		*prev_ptr = next_sibling;
		if (next_sibling)
			next_sibling->prev_or_parent = node->prev_or_parent;
	}
}

/*
 * Merge a list of subheaps into a single heap.
 *
 * This implements the basic two-pass merging strategy, first forming pairs
 * from left to right, and then merging the pairs.
 */
static pairingheap_node *
merge_children(pairingheap *heap, pairingheap_node *children)
{
	pairingheap_node *curr,
			   *next;
	pairingheap_node *pairs;
	pairingheap_node *newroot;

	if (children == NULL || children->next_sibling == NULL)
		return children;

	/* Walk the subheaps from left to right, merging in pairs */
	next = children;
	pairs = NULL;
	for (;;)
	{
		curr = next;

		if (curr == NULL)
			break;

		if (curr->next_sibling == NULL)
		{
			/* last odd node at the end of list */
			curr->next_sibling = pairs;
			pairs = curr;
			break;
		}

		next = curr->next_sibling->next_sibling;

		/* merge this and the next subheap, and add to 'pairs' list. */

		curr = merge(heap, curr, curr->next_sibling);
		curr->next_sibling = pairs;
		pairs = curr;
	}

	/*
	 * Merge all the pairs together to form a single heap.
	 */
	newroot = pairs;
	next = pairs->next_sibling;
	while (next)
	{
		curr = next;
		next = curr->next_sibling;

		newroot = merge(heap, newroot, curr);
	}

	return newroot;
}

/*
 * A debug function to dump the contents of the heap as a string.
 *
 * The 'dumpfunc' callback appends a string representation of a single node
 * to the StringInfo. 'opaque' can be used to pass more information to the
 * callback.
 */
#ifdef PAIRINGHEAP_DEBUG
static void
pairingheap_dump_recurse(StringInfo buf,
						 pairingheap_node *node,
						 void (*dumpfunc) (pairingheap_node *node, StringInfo buf, void *opaque),
						 void *opaque,
						 int depth,
						 pairingheap_node *prev_or_parent)
{
	while (node)
	{
		Assert(node->prev_or_parent == prev_or_parent);

		appendStringInfoSpaces(buf, depth * 4);
		dumpfunc(node, buf, opaque);
		appendStringInfoChar(buf, '\n');
		if (node->first_child)
			pairingheap_dump_recurse(buf, node->first_child, dumpfunc, opaque, depth + 1, node);
		prev_or_parent = node;
		node = node->next_sibling;
	}
}

char *
pairingheap_dump(pairingheap *heap,
				 void (*dumpfunc) (pairingheap_node *node, StringInfo buf, void *opaque),
				 void *opaque)
{
	StringInfoData buf;

	if (!heap->ph_root)
		return pstrdup("(empty)");

	initStringInfo(&buf);

	pairingheap_dump_recurse(&buf, heap->ph_root, dumpfunc, opaque, 0, NULL);

	return buf.data;
}
#endif
