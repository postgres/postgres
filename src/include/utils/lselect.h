/*-------------------------------------------------------------------------
 *
 * lselect.h--
 *    definitions for the replacement selection algorithm.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: lselect.h,v 1.3 1996/11/04 11:51:19 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	LSELECT_H
#define	LSELECT_H

#include <stdio.h>
#include <access/htup.h>

struct	leftist {
    short	lt_dist; 	/* distance to leaf/empty node */
    short	lt_devnum; 	/* device number of tuple */
    HeapTuple	lt_tuple;
    struct	leftist	*lt_left;
    struct	leftist	*lt_right;
};

extern	struct	leftist	*Tuples;

extern struct leftist *lmerge(struct leftist *pt, struct leftist *qt);
extern HeapTuple gettuple(struct leftist **treep, short *devnum);
extern int puttuple(struct leftist **treep, HeapTuple newtuple, int devnum);
extern void dumptuples(FILE *file);
extern int tuplecmp(HeapTuple ltup, HeapTuple rtup);

#ifdef EBUG
extern void checktree(struct leftist *tree);
extern int checktreer(struct leftist *tree, int level);
#endif /* EBUG */

#endif 	/* LSELECT_H */
