/*-------------------------------------------------------------------------
 *
 * tuplesort.h
 *	  Generalized tuple sorting routines.
 *
 * This module handles sorting of either heap tuples or index tuples
 * (and could fairly easily support other kinds of sortable objects,
 * if necessary).  It works efficiently for both small and large amounts
 * of data.  Small amounts are sorted in-memory using qsort().  Large
 * amounts are sorted using temporary files and a standard external sort
 * algorithm.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: tuplesort.h,v 1.1 1999/10/17 22:15:09 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TUPLESORT_H
#define TUPLESORT_H

#include "access/htup.h"
#include "access/itup.h"
#include "access/skey.h"
#include "access/tupdesc.h"
#include "utils/rel.h"

/* Tuplesortstate is an opaque type whose details are not known outside tuplesort.c. */

typedef struct Tuplesortstate Tuplesortstate;

/*
 * We provide two different interfaces to what is essentially the same
 * code: one for sorting HeapTuples and one for sorting IndexTuples.
 * They differ primarily in the way that the sort key information is
 * supplied.
 */

extern Tuplesortstate *tuplesort_begin_heap(TupleDesc tupDesc,
											int nkeys, ScanKey keys,
											bool randomAccess);
extern Tuplesortstate *tuplesort_begin_index(Relation indexRel,
											 bool enforceUnique,
											 bool randomAccess);

extern void tuplesort_puttuple(Tuplesortstate *state, void *tuple);

extern void tuplesort_performsort(Tuplesortstate *state);

extern void *tuplesort_gettuple(Tuplesortstate *state, bool forward,
								bool *should_free);
#define tuplesort_getheaptuple(state, forward, should_free) \
	((HeapTuple) tuplesort_gettuple(state, forward, should_free))
#define tuplesort_getindextuple(state, forward, should_free) \
	((IndexTuple) tuplesort_gettuple(state, forward, should_free))

extern void tuplesort_end(Tuplesortstate *state);

/*
 * These routines may only be called if randomAccess was specified 'true'.
 * Backwards scan in gettuple is likewise only allowed if randomAccess.
 */

extern void tuplesort_rescan(Tuplesortstate *state);
extern void tuplesort_markpos(Tuplesortstate *state);
extern void tuplesort_restorepos(Tuplesortstate *state);

#endif	 /* TUPLESORT_H */
