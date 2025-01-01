/*-------------------------------------------------------------------------
 *
 * rbtree.h
 *	  interface for PostgreSQL generic Red-Black binary tree package
 *
 * Copyright (c) 2009-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/include/lib/rbtree.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RBTREE_H
#define RBTREE_H

/*
 * RBTNode is intended to be used as the first field of a larger struct,
 * whose additional fields carry whatever payload data the caller needs
 * for a tree entry.  (The total size of that larger struct is passed to
 * rbt_create.)	RBTNode is declared here to support this usage, but
 * callers must treat it as an opaque struct.
 */
typedef struct RBTNode
{
	char color;					/* node's current color, red or black */
	struct RBTNode *left;		/* left child, or RBTNIL if none */
	struct RBTNode *right;		/* right child, or RBTNIL if none */
	struct RBTNode *parent;		/* parent, or NULL (not RBTNIL!) if none */
} RBTNode;

/* Opaque struct representing a whole tree */
typedef struct RBTree RBTree;

/* Available tree iteration orderings */
typedef enum RBTOrderControl
{
	LeftRightWalk,				/* inorder: left child, node, right child */
	RightLeftWalk				/* reverse inorder: right, node, left */
} RBTOrderControl;

/*
 * RBTreeIterator holds state while traversing a tree.  This is declared
 * here so that callers can stack-allocate this, but must otherwise be
 * treated as an opaque struct.
 */
typedef struct RBTreeIterator RBTreeIterator;

struct RBTreeIterator
{
	RBTree	   *rbt;
	RBTNode    *(*iterate) (RBTreeIterator *iter);
	RBTNode    *last_visited;
	bool		is_over;
};

/* Support functions to be provided by caller */
typedef int (*rbt_comparator) (const RBTNode *a, const RBTNode *b, void *arg);
typedef void (*rbt_combiner) (RBTNode *existing, const RBTNode *newdata, void *arg);
typedef RBTNode *(*rbt_allocfunc) (void *arg);
typedef void (*rbt_freefunc) (RBTNode *x, void *arg);

extern RBTree *rbt_create(Size node_size,
						  rbt_comparator comparator,
						  rbt_combiner combiner,
						  rbt_allocfunc allocfunc,
						  rbt_freefunc freefunc,
						  void *arg);

extern RBTNode *rbt_find(RBTree *rbt, const RBTNode *data);
extern RBTNode *rbt_find_great(RBTree *rbt, const RBTNode *data, bool equal_match);
extern RBTNode *rbt_find_less(RBTree *rbt, const RBTNode *data, bool equal_match);
extern RBTNode *rbt_leftmost(RBTree *rbt);

extern RBTNode *rbt_insert(RBTree *rbt, const RBTNode *data, bool *isNew);
extern void rbt_delete(RBTree *rbt, RBTNode *node);

extern void rbt_begin_iterate(RBTree *rbt, RBTOrderControl ctrl,
							  RBTreeIterator *iter);
extern RBTNode *rbt_iterate(RBTreeIterator *iter);

#endif							/* RBTREE_H */
