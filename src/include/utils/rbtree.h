/*-------------------------------------------------------------------------
 *
 * rbtree.h
 *    interface for PostgreSQL generic Red-Black binary tree package
 *
 * Copyright (c) 1996-2009, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 * 		$PostgreSQL: pgsql/src/include/utils/rbtree.h,v 1.1 2010/02/11 14:29:50 teodor Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef RBTREE_H
#define RBTREE_H

typedef struct RBTree RBTree;
typedef struct RBTreeIterator RBTreeIterator;

typedef int (*rb_comparator) (const void *a, const void *b, void *arg);
typedef void* (*rb_appendator) (void *current, void *new, void *arg);
typedef void (*rb_freefunc) (void *a);

extern RBTree *rb_create(rb_comparator comparator,
							rb_appendator appendator,
							rb_freefunc freefunc,
							void *arg);

extern void *rb_find(RBTree *rb, void *data);
extern void *rb_insert(RBTree *rb, void *data);
extern void rb_delete(RBTree *rb, void *data);
extern void *rb_leftmost(RBTree *rb);

typedef enum RBOrderControl
{
	LeftRightWalk,
	RightLeftWalk,
	DirectWalk,
	InvertedWalk
} RBOrderControl;

extern RBTreeIterator* rb_begin_iterate(RBTree *rb, RBOrderControl ctrl);
extern void *rb_iterate(RBTreeIterator *iterator);
extern void rb_free_iterator(RBTreeIterator *iterator);

#endif
