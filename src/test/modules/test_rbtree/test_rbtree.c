/*--------------------------------------------------------------------------
 *
 * test_rbtree.c
 *		Test correctness of red-black tree operations.
 *
 * Copyright (c) 2009-2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_rbtree/test_rbtree.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "lib/rbtree.h"
#include "utils/memutils.h"

PG_MODULE_MAGIC;


/*
 * Our test trees store an integer key, and nothing else.
 */
typedef struct IntRBTreeNode
{
	RBTNode		rbtnode;
	int			key;
} IntRBTreeNode;


/*
 * Node comparator.  We don't worry about overflow in the subtraction,
 * since none of our test keys are negative.
 */
static int
irbt_cmp(const RBTNode *a, const RBTNode *b, void *arg)
{
	const IntRBTreeNode *ea = (const IntRBTreeNode *) a;
	const IntRBTreeNode *eb = (const IntRBTreeNode *) b;

	return ea->key - eb->key;
}

/*
 * Node combiner.  For testing purposes, just check that library doesn't
 * try to combine unequal keys.
 */
static void
irbt_combine(RBTNode *existing, const RBTNode *newdata, void *arg)
{
	const IntRBTreeNode *eexist = (const IntRBTreeNode *) existing;
	const IntRBTreeNode *enew = (const IntRBTreeNode *) newdata;

	if (eexist->key != enew->key)
		elog(ERROR, "red-black tree combines %d into %d",
			 enew->key, eexist->key);
}

/* Node allocator */
static RBTNode *
irbt_alloc(void *arg)
{
	return (RBTNode *) palloc(sizeof(IntRBTreeNode));
}

/* Node freer */
static void
irbt_free(RBTNode *node, void *arg)
{
	pfree(node);
}

/*
 * Create a red-black tree using our support functions
 */
static RBTree *
create_int_rbtree(void)
{
	return rbt_create(sizeof(IntRBTreeNode),
					  irbt_cmp,
					  irbt_combine,
					  irbt_alloc,
					  irbt_free,
					  NULL);
}

/*
 * Generate a random permutation of the integers 0..size-1
 */
static int *
GetPermutation(int size)
{
	int		   *permutation;
	int			i;

	permutation = (int *) palloc(size * sizeof(int));

	permutation[0] = 0;

	/*
	 * This is the "inside-out" variant of the Fisher-Yates shuffle algorithm.
	 * Notionally, we append each new value to the array and then swap it with
	 * a randomly-chosen array element (possibly including itself, else we
	 * fail to generate permutations with the last integer last).  The swap
	 * step can be optimized by combining it with the insertion.
	 */
	for (i = 1; i < size; i++)
	{
		int			j = random() % (i + 1);

		if (j < i)				/* avoid fetching undefined data if j=i */
			permutation[i] = permutation[j];
		permutation[j] = i;
	}

	return permutation;
}

/*
 * Populate an empty RBTree with "size" integers having the values
 * 0, step, 2*step, 3*step, ..., inserting them in random order
 */
static void
rbt_populate(RBTree *tree, int size, int step)
{
	int		   *permutation = GetPermutation(size);
	IntRBTreeNode node;
	bool		isNew;
	int			i;

	/* Insert values.  We don't expect any collisions. */
	for (i = 0; i < size; i++)
	{
		node.key = step * permutation[i];
		rbt_insert(tree, (RBTNode *) &node, &isNew);
		if (!isNew)
			elog(ERROR, "unexpected !isNew result from rbt_insert");
	}

	/*
	 * Re-insert the first value to make sure collisions work right.  It's
	 * probably not useful to test that case over again for all the values.
	 */
	if (size > 0)
	{
		node.key = step * permutation[0];
		rbt_insert(tree, (RBTNode *) &node, &isNew);
		if (isNew)
			elog(ERROR, "unexpected isNew result from rbt_insert");
	}

	pfree(permutation);
}

/*
 * Check the correctness of left-right traversal.
 * Left-right traversal is correct if all elements are
 * visited in increasing order.
 */
static void
testleftright(int size)
{
	RBTree	   *tree = create_int_rbtree();
	IntRBTreeNode *node;
	RBTreeIterator iter;
	int			lastKey = -1;
	int			count = 0;

	/* check iteration over empty tree */
	rbt_begin_iterate(tree, LeftRightWalk, &iter);
	if (rbt_iterate(&iter) != NULL)
		elog(ERROR, "left-right walk over empty tree produced an element");

	/* fill tree with consecutive natural numbers */
	rbt_populate(tree, size, 1);

	/* iterate over the tree */
	rbt_begin_iterate(tree, LeftRightWalk, &iter);

	while ((node = (IntRBTreeNode *) rbt_iterate(&iter)) != NULL)
	{
		/* check that order is increasing */
		if (node->key <= lastKey)
			elog(ERROR, "left-right walk gives elements not in sorted order");
		lastKey = node->key;
		count++;
	}

	if (lastKey != size - 1)
		elog(ERROR, "left-right walk did not reach end");
	if (count != size)
		elog(ERROR, "left-right walk missed some elements");
}

/*
 * Check the correctness of right-left traversal.
 * Right-left traversal is correct if all elements are
 * visited in decreasing order.
 */
static void
testrightleft(int size)
{
	RBTree	   *tree = create_int_rbtree();
	IntRBTreeNode *node;
	RBTreeIterator iter;
	int			lastKey = size;
	int			count = 0;

	/* check iteration over empty tree */
	rbt_begin_iterate(tree, RightLeftWalk, &iter);
	if (rbt_iterate(&iter) != NULL)
		elog(ERROR, "right-left walk over empty tree produced an element");

	/* fill tree with consecutive natural numbers */
	rbt_populate(tree, size, 1);

	/* iterate over the tree */
	rbt_begin_iterate(tree, RightLeftWalk, &iter);

	while ((node = (IntRBTreeNode *) rbt_iterate(&iter)) != NULL)
	{
		/* check that order is decreasing */
		if (node->key >= lastKey)
			elog(ERROR, "right-left walk gives elements not in sorted order");
		lastKey = node->key;
		count++;
	}

	if (lastKey != 0)
		elog(ERROR, "right-left walk did not reach end");
	if (count != size)
		elog(ERROR, "right-left walk missed some elements");
}

/*
 * Check the correctness of the rbt_find operation by searching for
 * both elements we inserted and elements we didn't.
 */
static void
testfind(int size)
{
	RBTree	   *tree = create_int_rbtree();
	int			i;

	/* Insert even integers from 0 to 2 * (size-1) */
	rbt_populate(tree, size, 2);

	/* Check that all inserted elements can be found */
	for (i = 0; i < size; i++)
	{
		IntRBTreeNode node;
		IntRBTreeNode *resultNode;

		node.key = 2 * i;
		resultNode = (IntRBTreeNode *) rbt_find(tree, (RBTNode *) &node);
		if (resultNode == NULL)
			elog(ERROR, "inserted element was not found");
		if (node.key != resultNode->key)
			elog(ERROR, "find operation in rbtree gave wrong result");
	}

	/*
	 * Check that not-inserted elements can not be found, being sure to try
	 * values before the first and after the last element.
	 */
	for (i = -1; i <= 2 * size; i += 2)
	{
		IntRBTreeNode node;
		IntRBTreeNode *resultNode;

		node.key = i;
		resultNode = (IntRBTreeNode *) rbt_find(tree, (RBTNode *) &node);
		if (resultNode != NULL)
			elog(ERROR, "not-inserted element was found");
	}
}

/*
 * Check the correctness of the rbt_leftmost operation.
 * This operation should always return the smallest element of the tree.
 */
static void
testleftmost(int size)
{
	RBTree	   *tree = create_int_rbtree();
	IntRBTreeNode *result;

	/* Check that empty tree has no leftmost element */
	if (rbt_leftmost(tree) != NULL)
		elog(ERROR, "leftmost node of empty tree is not NULL");

	/* fill tree with consecutive natural numbers */
	rbt_populate(tree, size, 1);

	/* Check that leftmost element is the smallest one */
	result = (IntRBTreeNode *) rbt_leftmost(tree);
	if (result == NULL || result->key != 0)
		elog(ERROR, "rbt_leftmost gave wrong result");
}

/*
 * Check the correctness of the rbt_delete operation.
 */
static void
testdelete(int size, int delsize)
{
	RBTree	   *tree = create_int_rbtree();
	int		   *deleteIds;
	bool	   *chosen;
	int			i;

	/* fill tree with consecutive natural numbers */
	rbt_populate(tree, size, 1);

	/* Choose unique ids to delete */
	deleteIds = (int *) palloc(delsize * sizeof(int));
	chosen = (bool *) palloc0(size * sizeof(bool));

	for (i = 0; i < delsize; i++)
	{
		int			k = random() % size;

		while (chosen[k])
			k = (k + 1) % size;
		deleteIds[i] = k;
		chosen[k] = true;
	}

	/* Delete elements */
	for (i = 0; i < delsize; i++)
	{
		IntRBTreeNode find;
		IntRBTreeNode *node;

		find.key = deleteIds[i];
		/* Locate the node to be deleted */
		node = (IntRBTreeNode *) rbt_find(tree, (RBTNode *) &find);
		if (node == NULL || node->key != deleteIds[i])
			elog(ERROR, "expected element was not found during deleting");
		/* Delete it */
		rbt_delete(tree, (RBTNode *) node);
	}

	/* Check that deleted elements are deleted */
	for (i = 0; i < size; i++)
	{
		IntRBTreeNode node;
		IntRBTreeNode *result;

		node.key = i;
		result = (IntRBTreeNode *) rbt_find(tree, (RBTNode *) &node);
		if (chosen[i])
		{
			/* Deleted element should be absent */
			if (result != NULL)
				elog(ERROR, "deleted element still present in the rbtree");
		}
		else
		{
			/* Else it should be present */
			if (result == NULL || result->key != i)
				elog(ERROR, "delete operation removed wrong rbtree value");
		}
	}

	/* Delete remaining elements, so as to exercise reducing tree to empty */
	for (i = 0; i < size; i++)
	{
		IntRBTreeNode find;
		IntRBTreeNode *node;

		if (chosen[i])
			continue;
		find.key = i;
		/* Locate the node to be deleted */
		node = (IntRBTreeNode *) rbt_find(tree, (RBTNode *) &find);
		if (node == NULL || node->key != i)
			elog(ERROR, "expected element was not found during deleting");
		/* Delete it */
		rbt_delete(tree, (RBTNode *) node);
	}

	/* Tree should now be empty */
	if (rbt_leftmost(tree) != NULL)
		elog(ERROR, "deleting all elements failed");

	pfree(deleteIds);
	pfree(chosen);
}

/*
 * SQL-callable entry point to perform all tests
 *
 * Argument is the number of entries to put in the trees
 */
PG_FUNCTION_INFO_V1(test_rb_tree);

Datum
test_rb_tree(PG_FUNCTION_ARGS)
{
	int			size = PG_GETARG_INT32(0);

	if (size <= 0 || size > MaxAllocSize / sizeof(int))
		elog(ERROR, "invalid size for test_rb_tree: %d", size);
	testleftright(size);
	testrightleft(size);
	testfind(size);
	testleftmost(size);
	testdelete(size, Max(size / 10, 1));
	PG_RETURN_VOID();
}
