/*-------------------------------------------------------------------------
 *
 * lselect.c--
 *	  leftist tree selection algorithm (linked priority queue--Knuth, Vol.3,
 *	  pp.150-52)
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/sort/Attic/lselect.c,v 1.10 1998/01/15 19:46:08 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include <stdio.h>

#include "postgres.h"

#include "storage/buf.h"
#include "access/skey.h"
#include "access/heapam.h"
#include "access/htup.h"
#include "utils/rel.h"

#include "utils/psort.h"
#include "utils/lselect.h"

/*
 *		lmerge			- merges two leftist trees into one
 *
 *		Note:
 *				Enforcing the rule that pt->lt_dist >= qt->lt_dist may
 *				simplifify much of the code.  Removing recursion will not
 *				speed up code significantly.
 */
struct leftist *
lmerge(struct leftist * pt, struct leftist * qt, LeftistContext context)
{
	register struct leftist *root,
			   *majorLeftist,
			   *minorLeftist;
	int			dist;

	if (tuplecmp(pt->lt_tuple, qt->lt_tuple, context))
	{
		root = pt;
		majorLeftist = qt;
	}
	else
	{
		root = qt;
		majorLeftist = pt;
	}
	if (root->lt_left == NULL)
		root->lt_left = majorLeftist;
	else
	{
		if ((minorLeftist = root->lt_right) != NULL)
			majorLeftist = lmerge(majorLeftist, minorLeftist, context);
		if ((dist = root->lt_left->lt_dist) < majorLeftist->lt_dist)
		{
			root->lt_dist = 1 + dist;
			root->lt_right = root->lt_left;
			root->lt_left = majorLeftist;
		}
		else
		{
			root->lt_dist = 1 + majorLeftist->lt_dist;
			root->lt_right = majorLeftist;
		}
	}
	return (root);
}

static struct leftist *
linsert(struct leftist * root, struct leftist * new1, LeftistContext context)
{
	register struct leftist *left,
			   *right;

	if (!tuplecmp(root->lt_tuple, new1->lt_tuple, context))
	{
		new1->lt_left = root;
		return (new1);
	}
	left = root->lt_left;
	right = root->lt_right;
	if (right == NULL)
	{
		if (left == NULL)
			root->lt_left = new1;
		else
		{
			root->lt_right = new1;
			root->lt_dist = 2;
		}
		return (root);
	}
	right = linsert(right, new1, context);
	if (right->lt_dist < left->lt_dist)
	{
		root->lt_dist = 1 + left->lt_dist;
		root->lt_left = right;
		root->lt_right = left;
	}
	else
	{
		root->lt_dist = 1 + right->lt_dist;
		root->lt_right = right;
	}
	return (root);
}

/*
 *		gettuple		- returns tuple at top of tree (Tuples)
 *
 *		Returns:
 *				tuple at top of tree, NULL if failed ALLOC()
 *				*devnum is set to the devnum of tuple returned
 *				*treep is set to the new tree
 *
 *		Note:
 *				*treep must not be NULL
 *				NULL is currently never returned BUG
 */
HeapTuple
gettuple(struct leftist ** treep,
		 short *devnum,			/* device from which tuple came */
		 LeftistContext context)
{
	register struct leftist *tp;
	HeapTuple	tup;

	tp = *treep;
	tup = tp->lt_tuple;
	*devnum = tp->lt_devnum;
	if (tp->lt_dist == 1)		/* lt_left == NULL */
		*treep = tp->lt_left;
	else
		*treep = lmerge(tp->lt_left, tp->lt_right, context);

	pfree (tp);
	return (tup);
}

/*
 *		puttuple		- inserts new tuple into tree
 *
 *		Returns:
 *				NULL iff failed ALLOC()
 *
 *		Note:
 *				Currently never returns NULL BUG
 */
void
puttuple(struct leftist ** treep,
		 HeapTuple newtuple,
		 short devnum,
		 LeftistContext context)
{
	register struct leftist *new1;
	register struct leftist *tp;

	new1 = (struct leftist *) palloc((unsigned) sizeof(struct leftist));
	new1->lt_dist = 1;
	new1->lt_devnum = devnum;
	new1->lt_tuple = newtuple;
	new1->lt_left = NULL;
	new1->lt_right = NULL;
	if ((tp = *treep) == NULL)
		*treep = new1;
	else
		*treep = linsert(tp, new1, context);
	return;
}


/*
 *		tuplecmp		- Compares two tuples with respect CmpList
 *
 *		Returns:
 *				1 if left < right ;0 otherwise
 *		Assumtions:
 */
int
tuplecmp(HeapTuple ltup, HeapTuple rtup, LeftistContext context)
{
	register Datum lattr,
				rattr;
	int			nkey = 0;
	int			result = 0;
	bool		isnull;

	if (ltup == (HeapTuple) NULL)
		return (0);
	if (rtup == (HeapTuple) NULL)
		return (1);
	while (nkey < context->nKeys && !result)
	{
		lattr = heap_getattr(ltup, InvalidBuffer,
							 context->scanKeys[nkey].sk_attno,
							 context->tupDesc, &isnull);
		if (isnull)
			return (0);
		rattr = heap_getattr(rtup, InvalidBuffer,
							 context->scanKeys[nkey].sk_attno,
							 context->tupDesc,
							 &isnull);
		if (isnull)
			return (1);
		if (context->scanKeys[nkey].sk_flags & SK_COMMUTE)
		{
			if (!(result =
			   (long) (*fmgr_faddr(&context->scanKeys[nkey].sk_func)) (rattr, lattr)))
				result =
					-(long) (*fmgr_faddr(&context->scanKeys[nkey].sk_func)) (lattr, rattr);
		}
		else if (!(result =
			   (long) (*fmgr_faddr(&context->scanKeys[nkey].sk_func)) (lattr, rattr)))
			result =
				-(long) (*fmgr_faddr(&context->scanKeys[nkey].sk_func)) (rattr, lattr);
		nkey++;
	}
	return (result == 1);
}

#ifdef	EBUG
void
checktree(struct leftist * tree, LeftistContext context)
{
	int			lnodes;
	int			rnodes;

	if (tree == NULL)
	{
		puts("Null tree.");
		return;
	}
	lnodes = checktreer(tree->lt_left, 1, context);
	rnodes = checktreer(tree->lt_right, 1, context);
	if (lnodes < 0)
	{
		lnodes = -lnodes;
		puts("0:\tBad left side.");
	}
	if (rnodes < 0)
	{
		rnodes = -rnodes;
		puts("0:\tBad right side.");
	}
	if (lnodes == 0)
	{
		if (rnodes != 0)
			puts("0:\tLeft and right reversed.");
		if (tree->lt_dist != 1)
			puts("0:\tDistance incorrect.");
	}
	else if (rnodes == 0)
	{
		if (tree->lt_dist != 1)
			puts("0:\tDistance incorrect.");
	}
	else if (tree->lt_left->lt_dist < tree->lt_right->lt_dist)
	{
		puts("0:\tLeft and right reversed.");
		if (tree->lt_dist != 1 + tree->lt_left->lt_dist)
			puts("0:\tDistance incorrect.");
	}
	else if (tree->lt_dist != 1 + tree->lt_right->lt_dist)
		puts("0:\tDistance incorrect.");
	if (lnodes > 0)
		if (tuplecmp(tree->lt_left->lt_tuple, tree->lt_tuple, context))
			printf("%d:\tLeft child < parent.\n");
	if (rnodes > 0)
		if (tuplecmp(tree->lt_right->lt_tuple, tree->lt_tuple, context))
			printf("%d:\tRight child < parent.\n");
	printf("Tree has %d nodes\n", 1 + lnodes + rnodes);
}

int
checktreer(struct leftist * tree, int level, LeftistContext context)
{
	int			lnodes,
				rnodes;
	int			error = 0;

	if (tree == NULL)
		return (0);
	lnodes = checktreer(tree->lt_left, level + 1, context);
	rnodes = checktreer(tree->lt_right, level + 1, context);
	if (lnodes < 0)
	{
		error = 1;
		lnodes = -lnodes;
		printf("%d:\tBad left side.\n", level);
	}
	if (rnodes < 0)
	{
		error = 1;
		rnodes = -rnodes;
		printf("%d:\tBad right side.\n", level);
	}
	if (lnodes == 0)
	{
		if (rnodes != 0)
		{
			error = 1;
			printf("%d:\tLeft and right reversed.\n", level);
		}
		if (tree->lt_dist != 1)
		{
			error = 1;
			printf("%d:\tDistance incorrect.\n", level);
		}
	}
	else if (rnodes == 0)
	{
		if (tree->lt_dist != 1)
		{
			error = 1;
			printf("%d:\tDistance incorrect.\n", level);
		}
	}
	else if (tree->lt_left->lt_dist < tree->lt_right->lt_dist)
	{
		error = 1;
		printf("%d:\tLeft and right reversed.\n", level);
		if (tree->lt_dist != 1 + tree->lt_left->lt_dist)
			printf("%d:\tDistance incorrect.\n", level);
	}
	else if (tree->lt_dist != 1 + tree->lt_right->lt_dist)
	{
		error = 1;
		printf("%d:\tDistance incorrect.\n", level);
	}
	if (lnodes > 0)
		if (tuplecmp(tree->lt_left->lt_tuple, tree->lt_tuple, context))
		{
			error = 1;
			printf("%d:\tLeft child < parent.\n");
		}
	if (rnodes > 0)
		if (tuplecmp(tree->lt_right->lt_tuple, tree->lt_tuple, context))
		{
			error = 1;
			printf("%d:\tRight child < parent.\n");
		}
	if (error)
		return (-1 + -lnodes + -rnodes);
	return (1 + lnodes + rnodes);
}

#endif
