/*-------------------------------------------------------------------------
 *
 * rbtree.c
 *	  implementation for PostgreSQL generic Red-Black binary tree package
 *	  Adopted from http://algolist.manual.ru/ds/rbtree.php
 *
 * This code comes from Thomas Niemann's "Sorting and Searching Algorithms:
 * a Cookbook".
 *
 * See http://www.cs.auckland.ac.nz/software/AlgAnim/niemann/s_man.htm for
 * license terms: "Source code, when part of a software project, may be used
 * freely without reference to the author."
 *
 * Red-black trees are a type of balanced binary tree wherein (1) any child of
 * a red node is always black, and (2) every path from root to leaf traverses
 * an equal number of black nodes.  From these properties, it follows that the
 * longest path from root to leaf is only about twice as long as the shortest,
 * so lookups are guaranteed to run in O(lg n) time.
 *
 * Copyright (c) 2009-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/lib/rbtree.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lib/rbtree.h"


/*
 * Colors of nodes (values of RBTNode.color)
 */
#define RBTBLACK	(0)
#define RBTRED		(1)

/*
 * RBTree control structure
 */
struct RBTree
{
	RBTNode    *root;			/* root node, or RBTNIL if tree is empty */

	/* Remaining fields are constant after rbt_create */

	Size		node_size;		/* actual size of tree nodes */
	/* The caller-supplied manipulation functions */
	rbt_comparator comparator;
	rbt_combiner combiner;
	rbt_allocfunc allocfunc;
	rbt_freefunc freefunc;
	/* Passthrough arg passed to all manipulation functions */
	void	   *arg;
};

/*
 * all leafs are sentinels, use customized NIL name to prevent
 * collision with system-wide constant NIL which is actually NULL
 */
#define RBTNIL (&sentinel)

static RBTNode sentinel =
{
	.color = RBTBLACK,.left = RBTNIL,.right = RBTNIL,.parent = NULL
};


/*
 * rbt_create: create an empty RBTree
 *
 * Arguments are:
 *	node_size: actual size of tree nodes (> sizeof(RBTNode))
 *	The manipulation functions:
 *	comparator: compare two RBTNodes for less/equal/greater
 *	combiner: merge an existing tree entry with a new one
 *	allocfunc: allocate a new RBTNode
 *	freefunc: free an old RBTNode
 *	arg: passthrough pointer that will be passed to the manipulation functions
 *
 * Note that the combiner's righthand argument will be a "proposed" tree node,
 * ie the input to rbt_insert, in which the RBTNode fields themselves aren't
 * valid.  Similarly, either input to the comparator may be a "proposed" node.
 * This shouldn't matter since the functions aren't supposed to look at the
 * RBTNode fields, only the extra fields of the struct the RBTNode is embedded
 * in.
 *
 * The freefunc should just be pfree or equivalent; it should NOT attempt
 * to free any subsidiary data, because the node passed to it may not contain
 * valid data!	freefunc can be NULL if caller doesn't require retail
 * space reclamation.
 *
 * The RBTree node is palloc'd in the caller's memory context.  Note that
 * all contents of the tree are actually allocated by the caller, not here.
 *
 * Since tree contents are managed by the caller, there is currently not
 * an explicit "destroy" operation; typically a tree would be freed by
 * resetting or deleting the memory context it's stored in.  You can pfree
 * the RBTree node if you feel the urge.
 */
RBTree *
rbt_create(Size node_size,
		   rbt_comparator comparator,
		   rbt_combiner combiner,
		   rbt_allocfunc allocfunc,
		   rbt_freefunc freefunc,
		   void *arg)
{
	RBTree	   *tree = (RBTree *) palloc(sizeof(RBTree));

	Assert(node_size > sizeof(RBTNode));

	tree->root = RBTNIL;
	tree->node_size = node_size;
	tree->comparator = comparator;
	tree->combiner = combiner;
	tree->allocfunc = allocfunc;
	tree->freefunc = freefunc;

	tree->arg = arg;

	return tree;
}

/* Copy the additional data fields from one RBTNode to another */
static inline void
rbt_copy_data(RBTree *rbt, RBTNode *dest, const RBTNode *src)
{
	memcpy(dest + 1, src + 1, rbt->node_size - sizeof(RBTNode));
}

/**********************************************************************
 *						  Search									  *
 **********************************************************************/

/*
 * rbt_find: search for a value in an RBTree
 *
 * data represents the value to try to find.  Its RBTNode fields need not
 * be valid, it's the extra data in the larger struct that is of interest.
 *
 * Returns the matching tree entry, or NULL if no match is found.
 */
RBTNode *
rbt_find(RBTree *rbt, const RBTNode *data)
{
	RBTNode    *node = rbt->root;

	while (node != RBTNIL)
	{
		int			cmp = rbt->comparator(data, node, rbt->arg);

		if (cmp == 0)
			return node;
		else if (cmp < 0)
			node = node->left;
		else
			node = node->right;
	}

	return NULL;
}

/*
 * rbt_find_great: search for a greater value in an RBTree
 *
 * If equal_match is true, this will be a great or equal search.
 *
 * Returns the matching tree entry, or NULL if no match is found.
 */
RBTNode *
rbt_find_great(RBTree *rbt, const RBTNode *data, bool equal_match)
{
	RBTNode    *node = rbt->root;
	RBTNode    *greater = NULL;

	while (node != RBTNIL)
	{
		int			cmp = rbt->comparator(data, node, rbt->arg);

		if (equal_match && cmp == 0)
			return node;
		else if (cmp < 0)
		{
			greater = node;
			node = node->left;
		}
		else
			node = node->right;
	}

	return greater;
}

/*
 * rbt_find_less: search for a lesser value in an RBTree
 *
 * If equal_match is true, this will be a less or equal search.
 *
 * Returns the matching tree entry, or NULL if no match is found.
 */
RBTNode *
rbt_find_less(RBTree *rbt, const RBTNode *data, bool equal_match)
{
	RBTNode    *node = rbt->root;
	RBTNode    *lesser = NULL;

	while (node != RBTNIL)
	{
		int			cmp = rbt->comparator(data, node, rbt->arg);

		if (equal_match && cmp == 0)
			return node;
		else if (cmp > 0)
		{
			lesser = node;
			node = node->right;
		}
		else
			node = node->left;
	}

	return lesser;
}

/*
 * rbt_leftmost: fetch the leftmost (smallest-valued) tree node.
 * Returns NULL if tree is empty.
 *
 * Note: in the original implementation this included an unlink step, but
 * that's a bit awkward.  Just call rbt_delete on the result if that's what
 * you want.
 */
RBTNode *
rbt_leftmost(RBTree *rbt)
{
	RBTNode    *node = rbt->root;
	RBTNode    *leftmost = rbt->root;

	while (node != RBTNIL)
	{
		leftmost = node;
		node = node->left;
	}

	if (leftmost != RBTNIL)
		return leftmost;

	return NULL;
}

/**********************************************************************
 *							  Insertion								  *
 **********************************************************************/

/*
 * Rotate node x to left.
 *
 * x's right child takes its place in the tree, and x becomes the left
 * child of that node.
 */
static void
rbt_rotate_left(RBTree *rbt, RBTNode *x)
{
	RBTNode    *y = x->right;

	/* establish x->right link */
	x->right = y->left;
	if (y->left != RBTNIL)
		y->left->parent = x;

	/* establish y->parent link */
	if (y != RBTNIL)
		y->parent = x->parent;
	if (x->parent)
	{
		if (x == x->parent->left)
			x->parent->left = y;
		else
			x->parent->right = y;
	}
	else
	{
		rbt->root = y;
	}

	/* link x and y */
	y->left = x;
	if (x != RBTNIL)
		x->parent = y;
}

/*
 * Rotate node x to right.
 *
 * x's left right child takes its place in the tree, and x becomes the right
 * child of that node.
 */
static void
rbt_rotate_right(RBTree *rbt, RBTNode *x)
{
	RBTNode    *y = x->left;

	/* establish x->left link */
	x->left = y->right;
	if (y->right != RBTNIL)
		y->right->parent = x;

	/* establish y->parent link */
	if (y != RBTNIL)
		y->parent = x->parent;
	if (x->parent)
	{
		if (x == x->parent->right)
			x->parent->right = y;
		else
			x->parent->left = y;
	}
	else
	{
		rbt->root = y;
	}

	/* link x and y */
	y->right = x;
	if (x != RBTNIL)
		x->parent = y;
}

/*
 * Maintain Red-Black tree balance after inserting node x.
 *
 * The newly inserted node is always initially marked red.  That may lead to
 * a situation where a red node has a red child, which is prohibited.  We can
 * always fix the problem by a series of color changes and/or "rotations",
 * which move the problem progressively higher up in the tree.  If one of the
 * two red nodes is the root, we can always fix the problem by changing the
 * root from red to black.
 *
 * (This does not work lower down in the tree because we must also maintain
 * the invariant that every leaf has equal black-height.)
 */
static void
rbt_insert_fixup(RBTree *rbt, RBTNode *x)
{
	/*
	 * x is always a red node.  Initially, it is the newly inserted node. Each
	 * iteration of this loop moves it higher up in the tree.
	 */
	while (x != rbt->root && x->parent->color == RBTRED)
	{
		/*
		 * x and x->parent are both red.  Fix depends on whether x->parent is
		 * a left or right child.  In either case, we define y to be the
		 * "uncle" of x, that is, the other child of x's grandparent.
		 *
		 * If the uncle is red, we flip the grandparent to red and its two
		 * children to black.  Then we loop around again to check whether the
		 * grandparent still has a problem.
		 *
		 * If the uncle is black, we will perform one or two "rotations" to
		 * balance the tree.  Either x or x->parent will take the
		 * grandparent's position in the tree and recolored black, and the
		 * original grandparent will be recolored red and become a child of
		 * that node. This always leaves us with a valid red-black tree, so
		 * the loop will terminate.
		 */
		if (x->parent == x->parent->parent->left)
		{
			RBTNode    *y = x->parent->parent->right;

			if (y->color == RBTRED)
			{
				/* uncle is RBTRED */
				x->parent->color = RBTBLACK;
				y->color = RBTBLACK;
				x->parent->parent->color = RBTRED;

				x = x->parent->parent;
			}
			else
			{
				/* uncle is RBTBLACK */
				if (x == x->parent->right)
				{
					/* make x a left child */
					x = x->parent;
					rbt_rotate_left(rbt, x);
				}

				/* recolor and rotate */
				x->parent->color = RBTBLACK;
				x->parent->parent->color = RBTRED;

				rbt_rotate_right(rbt, x->parent->parent);
			}
		}
		else
		{
			/* mirror image of above code */
			RBTNode    *y = x->parent->parent->left;

			if (y->color == RBTRED)
			{
				/* uncle is RBTRED */
				x->parent->color = RBTBLACK;
				y->color = RBTBLACK;
				x->parent->parent->color = RBTRED;

				x = x->parent->parent;
			}
			else
			{
				/* uncle is RBTBLACK */
				if (x == x->parent->left)
				{
					x = x->parent;
					rbt_rotate_right(rbt, x);
				}
				x->parent->color = RBTBLACK;
				x->parent->parent->color = RBTRED;

				rbt_rotate_left(rbt, x->parent->parent);
			}
		}
	}

	/*
	 * The root may already have been black; if not, the black-height of every
	 * node in the tree increases by one.
	 */
	rbt->root->color = RBTBLACK;
}

/*
 * rbt_insert: insert a new value into the tree.
 *
 * data represents the value to insert.  Its RBTNode fields need not
 * be valid, it's the extra data in the larger struct that is of interest.
 *
 * If the value represented by "data" is not present in the tree, then
 * we copy "data" into a new tree entry and return that node, setting *isNew
 * to true.
 *
 * If the value represented by "data" is already present, then we call the
 * combiner function to merge data into the existing node, and return the
 * existing node, setting *isNew to false.
 *
 * "data" is unmodified in either case; it's typically just a local
 * variable in the caller.
 */
RBTNode *
rbt_insert(RBTree *rbt, const RBTNode *data, bool *isNew)
{
	RBTNode    *current,
			   *parent,
			   *x;
	int			cmp;

	/* find where node belongs */
	current = rbt->root;
	parent = NULL;
	cmp = 0;					/* just to prevent compiler warning */

	while (current != RBTNIL)
	{
		cmp = rbt->comparator(data, current, rbt->arg);
		if (cmp == 0)
		{
			/*
			 * Found node with given key.  Apply combiner.
			 */
			rbt->combiner(current, data, rbt->arg);
			*isNew = false;
			return current;
		}
		parent = current;
		current = (cmp < 0) ? current->left : current->right;
	}

	/*
	 * Value is not present, so create a new node containing data.
	 */
	*isNew = true;

	x = rbt->allocfunc(rbt->arg);

	x->color = RBTRED;

	x->left = RBTNIL;
	x->right = RBTNIL;
	x->parent = parent;
	rbt_copy_data(rbt, x, data);

	/* insert node in tree */
	if (parent)
	{
		if (cmp < 0)
			parent->left = x;
		else
			parent->right = x;
	}
	else
	{
		rbt->root = x;
	}

	rbt_insert_fixup(rbt, x);

	return x;
}

/**********************************************************************
 *							Deletion								  *
 **********************************************************************/

/*
 * Maintain Red-Black tree balance after deleting a black node.
 */
static void
rbt_delete_fixup(RBTree *rbt, RBTNode *x)
{
	/*
	 * x is always a black node.  Initially, it is the former child of the
	 * deleted node.  Each iteration of this loop moves it higher up in the
	 * tree.
	 */
	while (x != rbt->root && x->color == RBTBLACK)
	{
		/*
		 * Left and right cases are symmetric.  Any nodes that are children of
		 * x have a black-height one less than the remainder of the nodes in
		 * the tree.  We rotate and recolor nodes to move the problem up the
		 * tree: at some stage we'll either fix the problem, or reach the root
		 * (where the black-height is allowed to decrease).
		 */
		if (x == x->parent->left)
		{
			RBTNode    *w = x->parent->right;

			if (w->color == RBTRED)
			{
				w->color = RBTBLACK;
				x->parent->color = RBTRED;

				rbt_rotate_left(rbt, x->parent);
				w = x->parent->right;
			}

			if (w->left->color == RBTBLACK && w->right->color == RBTBLACK)
			{
				w->color = RBTRED;

				x = x->parent;
			}
			else
			{
				if (w->right->color == RBTBLACK)
				{
					w->left->color = RBTBLACK;
					w->color = RBTRED;

					rbt_rotate_right(rbt, w);
					w = x->parent->right;
				}
				w->color = x->parent->color;
				x->parent->color = RBTBLACK;
				w->right->color = RBTBLACK;

				rbt_rotate_left(rbt, x->parent);
				x = rbt->root;	/* Arrange for loop to terminate. */
			}
		}
		else
		{
			RBTNode    *w = x->parent->left;

			if (w->color == RBTRED)
			{
				w->color = RBTBLACK;
				x->parent->color = RBTRED;

				rbt_rotate_right(rbt, x->parent);
				w = x->parent->left;
			}

			if (w->right->color == RBTBLACK && w->left->color == RBTBLACK)
			{
				w->color = RBTRED;

				x = x->parent;
			}
			else
			{
				if (w->left->color == RBTBLACK)
				{
					w->right->color = RBTBLACK;
					w->color = RBTRED;

					rbt_rotate_left(rbt, w);
					w = x->parent->left;
				}
				w->color = x->parent->color;
				x->parent->color = RBTBLACK;
				w->left->color = RBTBLACK;

				rbt_rotate_right(rbt, x->parent);
				x = rbt->root;	/* Arrange for loop to terminate. */
			}
		}
	}
	x->color = RBTBLACK;
}

/*
 * Delete node z from tree.
 */
static void
rbt_delete_node(RBTree *rbt, RBTNode *z)
{
	RBTNode    *x,
			   *y;

	/* This is just paranoia: we should only get called on a valid node */
	if (!z || z == RBTNIL)
		return;

	/*
	 * y is the node that will actually be removed from the tree.  This will
	 * be z if z has fewer than two children, or the tree successor of z
	 * otherwise.
	 */
	if (z->left == RBTNIL || z->right == RBTNIL)
	{
		/* y has a RBTNIL node as a child */
		y = z;
	}
	else
	{
		/* find tree successor */
		y = z->right;
		while (y->left != RBTNIL)
			y = y->left;
	}

	/* x is y's only child */
	if (y->left != RBTNIL)
		x = y->left;
	else
		x = y->right;

	/* Remove y from the tree. */
	x->parent = y->parent;
	if (y->parent)
	{
		if (y == y->parent->left)
			y->parent->left = x;
		else
			y->parent->right = x;
	}
	else
	{
		rbt->root = x;
	}

	/*
	 * If we removed the tree successor of z rather than z itself, then move
	 * the data for the removed node to the one we were supposed to remove.
	 */
	if (y != z)
		rbt_copy_data(rbt, z, y);

	/*
	 * Removing a black node might make some paths from root to leaf contain
	 * fewer black nodes than others, or it might make two red nodes adjacent.
	 */
	if (y->color == RBTBLACK)
		rbt_delete_fixup(rbt, x);

	/* Now we can recycle the y node */
	if (rbt->freefunc)
		rbt->freefunc(y, rbt->arg);
}

/*
 * rbt_delete: remove the given tree entry
 *
 * "node" must have previously been found via rbt_find or rbt_leftmost.
 * It is caller's responsibility to free any subsidiary data attached
 * to the node before calling rbt_delete.  (Do *not* try to push that
 * responsibility off to the freefunc, as some other physical node
 * may be the one actually freed!)
 */
void
rbt_delete(RBTree *rbt, RBTNode *node)
{
	rbt_delete_node(rbt, node);
}

/**********************************************************************
 *						  Traverse									  *
 **********************************************************************/

static RBTNode *
rbt_left_right_iterator(RBTreeIterator *iter)
{
	if (iter->last_visited == NULL)
	{
		iter->last_visited = iter->rbt->root;
		while (iter->last_visited->left != RBTNIL)
			iter->last_visited = iter->last_visited->left;

		return iter->last_visited;
	}

	if (iter->last_visited->right != RBTNIL)
	{
		iter->last_visited = iter->last_visited->right;
		while (iter->last_visited->left != RBTNIL)
			iter->last_visited = iter->last_visited->left;

		return iter->last_visited;
	}

	for (;;)
	{
		RBTNode    *came_from = iter->last_visited;

		iter->last_visited = iter->last_visited->parent;
		if (iter->last_visited == NULL)
		{
			iter->is_over = true;
			break;
		}

		if (iter->last_visited->left == came_from)
			break;				/* came from left sub-tree, return current
								 * node */

		/* else - came from right sub-tree, continue to move up */
	}

	return iter->last_visited;
}

static RBTNode *
rbt_right_left_iterator(RBTreeIterator *iter)
{
	if (iter->last_visited == NULL)
	{
		iter->last_visited = iter->rbt->root;
		while (iter->last_visited->right != RBTNIL)
			iter->last_visited = iter->last_visited->right;

		return iter->last_visited;
	}

	if (iter->last_visited->left != RBTNIL)
	{
		iter->last_visited = iter->last_visited->left;
		while (iter->last_visited->right != RBTNIL)
			iter->last_visited = iter->last_visited->right;

		return iter->last_visited;
	}

	for (;;)
	{
		RBTNode    *came_from = iter->last_visited;

		iter->last_visited = iter->last_visited->parent;
		if (iter->last_visited == NULL)
		{
			iter->is_over = true;
			break;
		}

		if (iter->last_visited->right == came_from)
			break;				/* came from right sub-tree, return current
								 * node */

		/* else - came from left sub-tree, continue to move up */
	}

	return iter->last_visited;
}

/*
 * rbt_begin_iterate: prepare to traverse the tree in any of several orders
 *
 * After calling rbt_begin_iterate, call rbt_iterate repeatedly until it
 * returns NULL or the traversal stops being of interest.
 *
 * If the tree is changed during traversal, results of further calls to
 * rbt_iterate are unspecified.  Multiple concurrent iterators on the same
 * tree are allowed.
 *
 * The iterator state is stored in the 'iter' struct.  The caller should
 * treat it as an opaque struct.
 */
void
rbt_begin_iterate(RBTree *rbt, RBTOrderControl ctrl, RBTreeIterator *iter)
{
	/* Common initialization for all traversal orders */
	iter->rbt = rbt;
	iter->last_visited = NULL;
	iter->is_over = (rbt->root == RBTNIL);

	switch (ctrl)
	{
		case LeftRightWalk:		/* visit left, then self, then right */
			iter->iterate = rbt_left_right_iterator;
			break;
		case RightLeftWalk:		/* visit right, then self, then left */
			iter->iterate = rbt_right_left_iterator;
			break;
		default:
			elog(ERROR, "unrecognized rbtree iteration order: %d", ctrl);
	}
}

/*
 * rbt_iterate: return the next node in traversal order, or NULL if no more
 */
RBTNode *
rbt_iterate(RBTreeIterator *iter)
{
	if (iter->is_over)
		return NULL;

	return iter->iterate(iter);
}
