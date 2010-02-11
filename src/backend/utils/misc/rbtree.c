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
 * Copyright (c) 1996-2009, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/misc/rbtree.c,v 1.2 2010/02/11 22:17:27 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/rbtree.h"

/**********************************************************************
 *						 Declarations								  *
 **********************************************************************/

/*
 * Values for RBNode->iteratorState
 */
#define InitialState 	(0)
#define FirstStepDone	(1)
#define SecondStepDone	(2)
#define ThirdStepDone	(3)

/*
 * Colors of node
 */
#define RBBLACK		(0)
#define RBRED		(1)

typedef struct RBNode
{
	uint32		iteratorState:2,
				color:	1 ,
				unused: 29;
	struct RBNode *left;
	struct RBNode *right;
	struct RBNode *parent;
	void	   *data;
}	RBNode;

struct RBTree
{
	RBNode	   *root;
	rb_comparator comparator;
	rb_appendator appendator;
	rb_freefunc freefunc;
	void	   *arg;
};

struct RBTreeIterator
{
	RBNode	   *node;
	void	   *(*iterate) (RBTreeIterator *iterator);
};

/*
 * all leafs are sentinels, use customized NIL name to prevent
 * collision with sytem-wide NIL which is actually NULL
 */
#define RBNIL &sentinel

RBNode		sentinel = {InitialState, RBBLACK, 0, RBNIL, RBNIL, NULL, NULL};

/**********************************************************************
 *						  Create									  *
 **********************************************************************/

RBTree *
rb_create(rb_comparator comparator, rb_appendator appendator,
				  rb_freefunc freefunc, void *arg)
{
	RBTree	   *tree = palloc(sizeof(RBTree));

	tree->root = RBNIL;
	tree->comparator = comparator;
	tree->appendator = appendator;
	tree->freefunc = freefunc;
	tree->arg = arg;

	return tree;
}

/**********************************************************************
 *						  Search									  *
 **********************************************************************/

void *
rb_find(RBTree *rb, void *data)
{
	RBNode	   *node = rb->root;
	int			cmp;

	while (node != RBNIL)
	{
		cmp = rb->comparator(data, node->data, rb->arg);

		if (cmp == 0)
			return node->data;
		else if (cmp < 0)
			node = node->left;
		else
			node = node->right;
	}

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
rb_rotate_left(RBTree *rb, RBNode *x)
{
	RBNode	   *y = x->right;

	/* establish x->right link */
	x->right = y->left;
	if (y->left != RBNIL)
		y->left->parent = x;

	/* establish y->parent link */
	if (y != RBNIL)
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
		rb->root = y;
	}

	/* link x and y */
	y->left = x;
	if (x != RBNIL)
		x->parent = y;
}

/*
 * Rotate node x to right.
 *
 * x's left right child takes its place in the tree, and x becomes the right
 * child of that node.
 */
static void
rb_rotate_right(RBTree *rb, RBNode *x)
{
	RBNode	   *y = x->left;

	/* establish x->left link */
	x->left = y->right;
	if (y->right != RBNIL)
		y->right->parent = x;

	/* establish y->parent link */
	if (y != RBNIL)
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
		rb->root = y;
	}

	/* link x and y */
	y->right = x;
	if (x != RBNIL)
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
rb_insert_fixup(RBTree *rb, RBNode *x)
{
	/*
	 * x is always a red node.  Initially, it is the newly inserted node.
	 * Each iteration of this loop moves it higher up in the tree.
	 */
	while (x != rb->root && x->parent->color == RBRED)
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
		 * balance the tree.  Either x or x->parent will take the grandparent's
		 * position in the tree and recolored black, and the original
		 * grandparent will be recolored red and become a child of that node.
		 * This always leaves us with a valid red-black tree, so the loop
		 * will terminate.
		 */
		if (x->parent == x->parent->parent->left)
		{
			RBNode	   *y = x->parent->parent->right;

			if (y->color == RBRED)
			{
				/* uncle is RBRED */
				x->parent->color = RBBLACK;
				y->color = RBBLACK;
				x->parent->parent->color = RBRED;
				x = x->parent->parent;
			}
			else
			{
				/* uncle is RBBLACK */
				if (x == x->parent->right)
				{
					/* make x a left child */
					x = x->parent;
					rb_rotate_left(rb, x);
				}

				/* recolor and rotate */
				x->parent->color = RBBLACK;
				x->parent->parent->color = RBRED;
				rb_rotate_right(rb, x->parent->parent);
			}
		}
		else
		{
			/* mirror image of above code */
			RBNode	   *y = x->parent->parent->left;

			if (y->color == RBRED)
			{
				/* uncle is RBRED */
				x->parent->color = RBBLACK;
				y->color = RBBLACK;
				x->parent->parent->color = RBRED;
				x = x->parent->parent;
			}
			else
			{
				/* uncle is RBBLACK */
				if (x == x->parent->left)
				{
					x = x->parent;
					rb_rotate_right(rb, x);
				}
				x->parent->color = RBBLACK;
				x->parent->parent->color = RBRED;
				rb_rotate_left(rb, x->parent->parent);
			}
		}
	}

	/*
	 * The root may already have been black; if not, the black-height of every
	 * node in the tree increases by one.
	 */
	rb->root->color = RBBLACK;
}

/*
 * Allocate node for data and insert in tree.
 *
 * Return old data (or result of appendator method) if it exists and NULL
 * otherwise.
 */
void *
rb_insert(RBTree *rb, void *data)
{
	RBNode	   *current,
			   *parent,
			   *x;
	int			cmp;

	/* find where node belongs */
	current = rb->root;
	parent = NULL;
	cmp = 0;
	while (current != RBNIL)
	{
		cmp = rb->comparator(data, current->data, rb->arg);
		if (cmp == 0)
		{
			/*
			 * Found node with given key.  If appendator method is provided,
			 * call it to join old and new data; else, new data replaces old
			 * data.
			 */
			if (rb->appendator)
			{
				current->data = rb->appendator(current->data, data, rb->arg);
				return current->data;
			}
			else
			{
				void	   *old = current->data;

				current->data = data;
				return old;
			}
		}
		parent = current;
		current = (cmp < 0) ? current->left : current->right;
	}

	/* setup new node in tree */
	x = palloc(sizeof(RBNode));
	x->data = data;
	x->parent = parent;
	x->left = RBNIL;
	x->right = RBNIL;
	x->color = RBRED;
	x->iteratorState = InitialState;

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
		rb->root = x;
	}

	rb_insert_fixup(rb, x);
	return NULL;
}

/**********************************************************************
 *							Deletion								  *
 **********************************************************************/

/*
 * Maintain Red-Black tree balance after deleting a black node.
 */
static void
rb_delete_fixup(RBTree *rb, RBNode *x)
{
	/*
	 * x is always a black node.  Initially, it is the former child of the
	 * deleted node.  Each iteration of this loop moves it higher up in the
	 * tree.
	 */
	while (x != rb->root && x->color == RBBLACK)
	{
		/*
		 * Left and right cases are symmetric.  Any nodes that are children
		 * of x have a black-height one less than the remainder of the nodes
		 * in the tree.  We rotate and recolor nodes to move the problem up
		 * the tree: at some stage we'll either fix the problem, or reach the
		 * root (where the black-height is allowed to decrease).
		 */
		if (x == x->parent->left)
		{
			RBNode	   *w = x->parent->right;

			if (w->color == RBRED)
			{
				w->color = RBBLACK;
				x->parent->color = RBRED;
				rb_rotate_left(rb, x->parent);
				w = x->parent->right;
			}

			if (w->left->color == RBBLACK && w->right->color == RBBLACK)
			{
				w->color = RBRED;
				x = x->parent;
			}
			else
			{
				if (w->right->color == RBBLACK)
				{
					w->left->color = RBBLACK;
					w->color = RBRED;
					rb_rotate_right(rb, w);
					w = x->parent->right;
				}
				w->color = x->parent->color;
				x->parent->color = RBBLACK;
				w->right->color = RBBLACK;
				rb_rotate_left(rb, x->parent);
				x = rb->root;		/* Arrange for loop to terminate. */
			}
		}
		else
		{
			RBNode	   *w = x->parent->left;

			if (w->color == RBRED)
			{
				w->color = RBBLACK;
				x->parent->color = RBRED;
				rb_rotate_right(rb, x->parent);
				w = x->parent->left;
			}

			if (w->right->color == RBBLACK && w->left->color == RBBLACK)
			{
				w->color = RBRED;
				x = x->parent;
			}
			else
			{
				if (w->left->color == RBBLACK)
				{
					w->right->color = RBBLACK;
					w->color = RBRED;
					rb_rotate_left(rb, w);
					w = x->parent->left;
				}
				w->color = x->parent->color;
				x->parent->color = RBBLACK;
				w->left->color = RBBLACK;
				rb_rotate_right(rb, x->parent);
				x = rb->root;		/* Arrange for loop to terminate. */
			}
		}
	}
	x->color = RBBLACK;
}

/*
 * Delete node z from tree.
 */
static void
rb_delete_node(RBTree *rb, RBNode *z)
{
	RBNode	   *x,
			   *y;

	if (!z || z == RBNIL)
		return;

	/*
	 * y is the node that will actually be removed from the tree.  This will
	 * be z if z has fewer than two children, or the tree successor of z
	 * otherwise.
	 */
	if (z->left == RBNIL || z->right == RBNIL)
	{
		/* y has a RBNIL node as a child */
		y = z;
	}
	else
	{
		/* find tree successor */
		y = z->right;
		while (y->left != RBNIL)
			y = y->left;
	}

	/* x is y's only child */
	if (y->left != RBNIL)
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
		rb->root = x;
	}

	/*
	 * If we removed the tree successor of z rather than z itself, then
	 * attach the data for the removed node to the one we were supposed to
	 * remove.
	 */
	if (y != z)
		z->data = y->data;

	/*
	 * Removing a black node might make some paths from root to leaf contain
	 * fewer black nodes than others, or it might make two red nodes adjacent.
	 */
	if (y->color == RBBLACK)
		rb_delete_fixup(rb, x);

	pfree(y);
}

extern void
rb_delete(RBTree *rb, void *data)
{
	RBNode	   *node = rb->root;
	int			cmp;

	while (node != RBNIL)
	{
		cmp = rb->comparator(data, node->data, rb->arg);

		if (cmp == 0)
		{
			/* found node to delete */
			if (rb->freefunc)
				rb->freefunc(node->data);
			node->data = NULL;
			rb_delete_node(rb, node);
			return;
		}
		else if (cmp < 0)
			node = node->left;
		else
			node = node->right;
	}
}

/*
 * Return data on left most node and delete
 * that node
 */
extern void *
rb_leftmost(RBTree *rb)
{
	RBNode	   *node = rb->root;
	RBNode	   *leftmost = rb->root;
	void	   *res = NULL;

	while (node != RBNIL)
	{
		leftmost = node;
		node = node->left;
	}

	if (leftmost != RBNIL)
	{
		res = leftmost->data;
		leftmost->data = NULL;
		rb_delete_node(rb, leftmost);
	}

	return res;
}

/**********************************************************************
 *						  Traverse									  *
 **********************************************************************/

static void *
rb_next_node(RBTreeIterator *iterator, RBNode *node)
{
	node->iteratorState = InitialState;
	iterator->node = node;
	return iterator->iterate(iterator);
}

static void *
rb_left_right_iterator(RBTreeIterator *iterator)
{
	RBNode	   *node = iterator->node;

	switch (node->iteratorState)
	{
		case InitialState:
			if (node->left != RBNIL)
			{
				node->iteratorState = FirstStepDone;
				return rb_next_node(iterator, node->left);
			}
		case FirstStepDone:
			node->iteratorState = SecondStepDone;
			return node->data;
		case SecondStepDone:
			if (node->right != RBNIL)
			{
				node->iteratorState = ThirdStepDone;
				return rb_next_node(iterator, node->right);
			}
		case ThirdStepDone:
			if (node->parent)
			{
				iterator->node = node->parent;
				return iterator->iterate(iterator);
			}
			break;
		default:
			elog(ERROR, "Unknow node state: %d", node->iteratorState);
	}

	return NULL;
}

static void *
rb_right_left_iterator(RBTreeIterator *iterator)
{
	RBNode	   *node = iterator->node;

	switch (node->iteratorState)
	{
		case InitialState:
			if (node->right != RBNIL)
			{
				node->iteratorState = FirstStepDone;
				return rb_next_node(iterator, node->right);
			}
		case FirstStepDone:
			node->iteratorState = SecondStepDone;
			return node->data;
		case SecondStepDone:
			if (node->left != RBNIL)
			{
				node->iteratorState = ThirdStepDone;
				return rb_next_node(iterator, node->left);
			}
		case ThirdStepDone:
			if (node->parent)
			{
				iterator->node = node->parent;
				return iterator->iterate(iterator);
			}
			break;
		default:
			elog(ERROR, "Unknow node state: %d", node->iteratorState);
	}

	return NULL;
}

static void *
rb_direct_iterator(RBTreeIterator *iterator)
{
	RBNode	   *node = iterator->node;

	switch (node->iteratorState)
	{
		case InitialState:
			node->iteratorState = FirstStepDone;
			return node->data;
		case FirstStepDone:
			if (node->left != RBNIL)
			{
				node->iteratorState = SecondStepDone;
				return rb_next_node(iterator, node->left);
			}
		case SecondStepDone:
			if (node->right != RBNIL)
			{
				node->iteratorState = ThirdStepDone;
				return rb_next_node(iterator, node->right);
			}
		case ThirdStepDone:
			if (node->parent)
			{
				iterator->node = node->parent;
				return iterator->iterate(iterator);
			}
			break;
		default:
			elog(ERROR, "Unknow node state: %d", node->iteratorState);
	}

	return NULL;
}

static void *
rb_inverted_iterator(RBTreeIterator *iterator)
{
	RBNode	   *node = iterator->node;

	switch (node->iteratorState)
	{
		case InitialState:
			if (node->left != RBNIL)
			{
				node->iteratorState = FirstStepDone;
				return rb_next_node(iterator, node->left);
			}
		case FirstStepDone:
			if (node->right != RBNIL)
			{
				node->iteratorState = SecondStepDone;
				return rb_next_node(iterator, node->right);
			}
		case SecondStepDone:
			node->iteratorState = ThirdStepDone;
			return node->data;
		case ThirdStepDone:
			if (node->parent)
			{
				iterator->node = node->parent;
				return iterator->iterate(iterator);
			}
			break;
		default:
			elog(ERROR, "Unknow node state: %d", node->iteratorState);
	}

	return NULL;
}

RBTreeIterator *
rb_begin_iterate(RBTree *rb, RBOrderControl ctrl)
{
	RBTreeIterator *iterator = palloc(sizeof(RBTreeIterator));

	iterator->node = rb->root;
	if (iterator->node != RBNIL)
		iterator->node->iteratorState = InitialState;

	switch (ctrl)
	{
		case LeftRightWalk:			/* visit left, then self, then right */
			iterator->iterate = rb_left_right_iterator;
			break;
		case RightLeftWalk:			/* visit right, then self, then left */
			iterator->iterate = rb_right_left_iterator;
			break;
		case DirectWalk:			/* visit self, then left, then right */
			iterator->iterate = rb_direct_iterator;
			break;
		case InvertedWalk:			/* visit left, then right, then self */
			iterator->iterate = rb_inverted_iterator;
			break;
		default:
			elog(ERROR, "Unknown iterator order: %d", ctrl);
	}

	return iterator;
}

void *
rb_iterate(RBTreeIterator *iterator)
{
	if (iterator->node == RBNIL)
		return NULL;

	return iterator->iterate(iterator);
}

void
rb_free_iterator(RBTreeIterator *iterator)
{
	pfree(iterator);
}
