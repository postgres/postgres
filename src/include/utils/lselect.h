/*-------------------------------------------------------------------------
 *
 * lselect.h--
 *	  definitions for the replacement selection algorithm.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: lselect.h,v 1.11 1998/09/01 04:39:16 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef LSELECT_H
#define LSELECT_H

#include <stdio.h>

#include "access/skey.h"
#include "access/tupdesc.h"
#include "access/htup.h"
#include "utils/syscache.h"

struct leftist
{
	short		lt_dist;		/* distance to leaf/empty node */
	short		lt_devnum;		/* device number of tuple */
	HeapTuple	lt_tuple;
	struct leftist *lt_left;
	struct leftist *lt_right;
};

/* replaces global variables in lselect.c to make it reentrant */
typedef struct
{
	TupleDesc	tupDesc;
	int			nKeys;
	ScanKey		scanKeys;
	int			sortMem;		/* needed for psort */
} LeftistContextData;
typedef LeftistContextData *LeftistContext;

extern struct leftist *lmerge(struct leftist * pt, struct leftist * qt,
	   LeftistContext context);
extern HeapTuple gettuple(struct leftist ** treep, short *devnum,
		 LeftistContext context);
extern void puttuple(struct leftist ** treep, HeapTuple newtuple, short devnum,
		 LeftistContext context);
extern int	tuplecmp(HeapTuple ltup, HeapTuple rtup, LeftistContext context);

#ifdef EBUG
extern void checktree(struct leftist * tree, LeftistContext context);
extern int	checktreer(struct leftist * tree, int level, LeftistContext context);

#endif	 /* EBUG */

#endif	 /* LSELECT_H */
