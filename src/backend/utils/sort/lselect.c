/*-------------------------------------------------------------------------
 *
 * lselect.c--
 *    leftist tree selection algorithm (linked priority queue--Knuth, Vol.3,
 *    pp.150-52)
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/utils/sort/Attic/lselect.c,v 1.3 1997/05/20 11:35:48 vadim Exp $
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

extern	Relation	SortRdesc;		/* later static */ 

/*
 *	PUTTUP		- writes the next tuple
 *	ENDRUN		- mark end of run
 *	GETLEN		- reads the length of the next tuple
 *	ALLOCTUP	- returns space for the new tuple
 *	SETTUPLEN	- stores the length into the tuple
 *	GETTUP		- reads the tuple
 *
 *	Note:
 *		LEN field must be a short; FP is a stream
 */

#define	PUTTUP(TUP, FP)	fwrite((char *)TUP, (TUP)->t_len, 1, FP)
#define	ENDRUN(FP)	fwrite((char *)&shortzero, sizeof (shortzero), 1, FP)
#define	GETLEN(LEN, FP)	fread(&(LEN), sizeof (shortzero), 1, FP)
#define	ALLOCTUP(LEN)	((HeapTuple)palloc((unsigned)LEN))
#define	GETTUP(TUP, LEN, FP)\
	fread((char *)(TUP) + sizeof (shortzero), 1, (LEN) - sizeof (shortzero), FP)
#define	SETTUPLEN(TUP, LEN)	(TUP)->t_len = LEN

/*
 *	USEMEM		- record use of memory
 *	FREEMEM		- record freeing of memory
 *	FULLMEM		- 1 iff a tuple will fit
 */

#define	USEMEM(AMT)	SortMemory -= (AMT)
#define	FREEMEM(AMT)	SortMemory += (AMT)
#define	LACKMEM()	(SortMemory <= BLCKSZ)		/* not accurate */

/*
 *	lmerge		- merges two leftist trees into one
 *
 *	Note:
 *		Enforcing the rule that pt->lt_dist >= qt->lt_dist may
 *		simplifify much of the code.  Removing recursion will not
 *		speed up code significantly.
 */
struct leftist *
lmerge(struct leftist *pt, struct leftist *qt)
{
    register struct	leftist	*root, *majorLeftist, *minorLeftist;
    int		dist;
    
    if (tuplecmp(pt->lt_tuple, qt->lt_tuple)) {
	root = pt;
	majorLeftist = qt;
    } else {
	root = qt;
	majorLeftist = pt;
    }
    if (root->lt_left == NULL)
	root->lt_left = majorLeftist;
    else {
	if ((minorLeftist = root->lt_right) != NULL)
	    majorLeftist = lmerge(majorLeftist, minorLeftist);
	if ((dist = root->lt_left->lt_dist) < majorLeftist->lt_dist) {
	    root->lt_dist = 1 + dist;
	    root->lt_right = root->lt_left;
	    root->lt_left = majorLeftist;
	} else {
	    root->lt_dist = 1 + majorLeftist->lt_dist;
	    root->lt_right = majorLeftist;
	}
    }
    return(root);
}

static struct leftist *
linsert(struct leftist *root, struct leftist *new1)
{
    register struct	leftist	*left, *right;
    
    if (! tuplecmp(root->lt_tuple, new1->lt_tuple)) {
	new1->lt_left = root;
	return(new1);
    }
    left = root->lt_left;
    right = root->lt_right;
    if (right == NULL) {
	if (left == NULL)
	    root->lt_left = new1;
	else {
	    root->lt_right = new1;
	    root->lt_dist = 2;
	}
	return(root);
    }
    right = linsert(right, new1);
    if (right->lt_dist < left->lt_dist) {
	root->lt_dist = 1 + left->lt_dist;
	root->lt_left = right;
	root->lt_right = left;
    } else {
	root->lt_dist = 1 + right->lt_dist;
	root->lt_right = right;
    }
    return(root);
}

/*
 *	gettuple	- returns tuple at top of tree (Tuples)
 *
 *	Returns:
 *		tuple at top of tree, NULL if failed ALLOC()
 *		*devnum is set to the devnum of tuple returned
 *		*treep is set to the new tree
 *
 *	Note:
 *		*treep must not be NULL
 *		NULL is currently never returned BUG
 */
HeapTuple
gettuple(struct leftist **treep,
	 short *devnum)		/* device from which tuple came */
{
    register struct	leftist	 *tp;
    HeapTuple	tup;
    
    tp = *treep;
    tup = tp->lt_tuple;
    *devnum = tp->lt_devnum;
    if (tp->lt_dist == 1)				/* lt_left == NULL */
	*treep = tp->lt_left;
    else
	*treep = lmerge(tp->lt_left, tp->lt_right);
    
    FREEMEM(sizeof (struct leftist));
    FREE(tp);
    return(tup);
}

/*
 *	puttuple	- inserts new tuple into tree
 *
 *	Returns:
 *		NULL iff failed ALLOC()
 *
 *	Note:
 *		Currently never returns NULL BUG
 */
int
puttuple(struct leftist **treep, HeapTuple newtuple, int devnum)
{
    register struct	leftist	*new1;
    register struct	leftist	*tp;
    
    new1 = (struct leftist *) palloc((unsigned) sizeof (struct leftist));
    USEMEM(sizeof (struct leftist));
    new1->lt_dist = 1;
    new1->lt_devnum = devnum;
    new1->lt_tuple = newtuple;
    new1->lt_left = NULL;
    new1->lt_right = NULL;
    if ((tp = *treep) == NULL)
	*treep = new1;
    else
	*treep = linsert(tp, new1);
    return(1);
}


/*
 *	dumptuples	- stores all the tuples in tree into file
 */
void
dumptuples(FILE *file)
{
    register struct	leftist	*tp;
    register struct	leftist	*newp;
    HeapTuple	tup;
    
    tp = Tuples;
    while (tp != NULL) {
	tup = tp->lt_tuple;
	if (tp->lt_dist == 1)			/* lt_right == NULL */
	    newp = tp->lt_left;
	else
	    newp = lmerge(tp->lt_left, tp->lt_right);
	FREEMEM(sizeof (struct leftist));
	FREE(tp);
	PUTTUP(tup, file);
	FREEMEM(tup->t_len);
	FREE(tup);
	tp = newp;
    }
    Tuples = NULL;
}

/*
 *	tuplecmp	- Compares two tuples with respect CmpList
 *
 *	Returns:
 *		1 if left < right ;0 otherwise
 *	Assumtions:
 */
int
tuplecmp(HeapTuple ltup, HeapTuple rtup)
{
    register char	*lattr, *rattr;
    int		nkey = 0;
    extern	int	Nkeys;
    extern	ScanKey	Key;
    int		result = 0;
    bool		isnull;
    
    if (ltup == (HeapTuple)NULL)
	return(0);
    if (rtup == (HeapTuple)NULL)
	return(1);
    while (nkey < Nkeys && !result) {
	lattr = heap_getattr(ltup, InvalidBuffer,
			     Key[nkey].sk_attno, 
			     RelationGetTupleDescriptor(SortRdesc),
			     &isnull);
	if (isnull)
	    return(0);
	rattr = heap_getattr(rtup, InvalidBuffer,
			     Key[nkey].sk_attno, 
			     RelationGetTupleDescriptor(SortRdesc),
			     &isnull);
	if (isnull)
	    return(1);
	if (Key[nkey].sk_flags & SK_COMMUTE) {
	    if (!(result = (long) (*Key[nkey].sk_func) (rattr, lattr)))
		result = -(long) (*Key[nkey].sk_func) (lattr, rattr);
	} else if (!(result = (long) (*Key[nkey].sk_func) (lattr, rattr)))
	    result = -(long) (*Key[nkey].sk_func) (rattr, lattr);
	nkey++;
    }
    return (result == 1);
}

#ifdef	EBUG
void
checktree(struct leftist *tree)
{
    int		lnodes;
    int		rnodes;
    
    if (tree == NULL) {
	puts("Null tree.");
	return;
    }
    lnodes = checktreer(tree->lt_left, 1);
    rnodes = checktreer(tree->lt_right, 1);
    if (lnodes < 0) {
	lnodes = -lnodes;
	puts("0:\tBad left side.");
    }
    if (rnodes < 0) {
	rnodes = -rnodes;
	puts("0:\tBad right side.");
    }
    if (lnodes == 0) {
	if (rnodes != 0)
	    puts("0:\tLeft and right reversed.");
	if (tree->lt_dist != 1)
	    puts("0:\tDistance incorrect.");
    } else if (rnodes == 0) {
	if (tree->lt_dist != 1)
	    puts("0:\tDistance incorrect.");
    } else if (tree->lt_left->lt_dist < tree->lt_right->lt_dist) {
	puts("0:\tLeft and right reversed.");
	if (tree->lt_dist != 1 + tree->lt_left->lt_dist)
	    puts("0:\tDistance incorrect.");
    } else if (tree->lt_dist != 1+ tree->lt_right->lt_dist)
	puts("0:\tDistance incorrect.");
    if (lnodes > 0)
	if (tuplecmp(tree->lt_left->lt_tuple, tree->lt_tuple))
	    printf("%d:\tLeft child < parent.\n");
    if (rnodes > 0)
	if (tuplecmp(tree->lt_right->lt_tuple, tree->lt_tuple))
	    printf("%d:\tRight child < parent.\n");
    printf("Tree has %d nodes\n", 1 + lnodes + rnodes);
}

int
checktreer(struct leftist *tree, int level)
{
    int	lnodes, rnodes;
    int	error = 0;
    
    if (tree == NULL)
	return(0);
    lnodes = checktreer(tree->lt_left, level + 1);
    rnodes = checktreer(tree->lt_right, level + 1);
    if (lnodes < 0) {
	error = 1;
	lnodes = -lnodes;
	printf("%d:\tBad left side.\n", level);
    }
    if (rnodes < 0) {
	error = 1;
	rnodes = -rnodes;
	printf("%d:\tBad right side.\n", level);
    }
    if (lnodes == 0) {
	if (rnodes != 0) {
	    error = 1;
	    printf("%d:\tLeft and right reversed.\n", level);
	}
	if (tree->lt_dist != 1) {
	    error = 1;
	    printf("%d:\tDistance incorrect.\n", level);
	}
    } else if (rnodes == 0) {
	if (tree->lt_dist != 1) {
	    error = 1;
	    printf("%d:\tDistance incorrect.\n", level);
	}			
    } else if (tree->lt_left->lt_dist < tree->lt_right->lt_dist) {
	error = 1;
	printf("%d:\tLeft and right reversed.\n", level);
	if (tree->lt_dist != 1 + tree->lt_left->lt_dist)
	    printf("%d:\tDistance incorrect.\n", level);
    } else if (tree->lt_dist != 1+ tree->lt_right->lt_dist) {
	error = 1;
	printf("%d:\tDistance incorrect.\n", level);
    }
    if (lnodes > 0)
	if (tuplecmp(tree->lt_left->lt_tuple, tree->lt_tuple)) {
	    error = 1;
	    printf("%d:\tLeft child < parent.\n");
	}
    if (rnodes > 0)
	if (tuplecmp(tree->lt_right->lt_tuple, tree->lt_tuple)) {
	    error = 1;
	    printf("%d:\tRight child < parent.\n");
	}
    if (error)
	return(-1 + -lnodes + -rnodes);
    return(1 + lnodes + rnodes);
}
#endif
