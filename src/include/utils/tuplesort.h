/*-------------------------------------------------------------------------
 *
 * tuplesort.h
 *	  Generalized tuple sorting routines.
 *
 * This module handles sorting of heap tuples, index tuples, or single
 * Datums (and could easily support other kinds of sortable objects,
 * if necessary).  It works efficiently for both small and large amounts
 * of data.  Small amounts are sorted in-memory using qsort().	Large
 * amounts are sorted using temporary files and a standard external sort
 * algorithm.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: tuplesort.h,v 1.13 2003/08/04 02:40:15 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TUPLESORT_H
#define TUPLESORT_H

#include "access/htup.h"
#include "access/itup.h"
#include "fmgr.h"

/* Tuplesortstate is an opaque type whose details are not known outside tuplesort.c. */

typedef struct Tuplesortstate Tuplesortstate;

/*
 * We provide two different interfaces to what is essentially the same
 * code: one for sorting HeapTuples and one for sorting IndexTuples.
 * They differ primarily in the way that the sort key information is
 * supplied.
 * Yet a third slightly different interface supports sorting bare Datums.
 */

extern Tuplesortstate *tuplesort_begin_heap(TupleDesc tupDesc,
					 int nkeys,
					 Oid *sortOperators, AttrNumber *attNums,
					 bool randomAccess);
extern Tuplesortstate *tuplesort_begin_index(Relation indexRel,
					  bool enforceUnique,
					  bool randomAccess);
extern Tuplesortstate *tuplesort_begin_datum(Oid datumType,
					  Oid sortOperator,
					  bool randomAccess);

extern void tuplesort_puttuple(Tuplesortstate *state, void *tuple);

extern void tuplesort_putdatum(Tuplesortstate *state, Datum val,
				   bool isNull);

extern void tuplesort_performsort(Tuplesortstate *state);

extern void *tuplesort_gettuple(Tuplesortstate *state, bool forward,
				   bool *should_free);

#define tuplesort_getheaptuple(state, forward, should_free) \
	((HeapTuple) tuplesort_gettuple(state, forward, should_free))
#define tuplesort_getindextuple(state, forward, should_free) \
	((IndexTuple) tuplesort_gettuple(state, forward, should_free))

extern bool tuplesort_getdatum(Tuplesortstate *state, bool forward,
				   Datum *val, bool *isNull);

extern void tuplesort_end(Tuplesortstate *state);

/*
 * These routines may only be called if randomAccess was specified 'true'.
 * Likewise, backwards scan in gettuple/getdatum is only allowed if
 * randomAccess was specified.
 */

extern void tuplesort_rescan(Tuplesortstate *state);
extern void tuplesort_markpos(Tuplesortstate *state);
extern void tuplesort_restorepos(Tuplesortstate *state);

/*
 * This routine selects an appropriate sorting function to implement
 * a sort operator as efficiently as possible.
 */
typedef enum
{
	SORTFUNC_LT,				/* raw "<" operator */
	SORTFUNC_REVLT,				/* raw "<" operator, but reverse NULLs */
	SORTFUNC_CMP,				/* -1 / 0 / 1 three-way comparator */
	SORTFUNC_REVCMP				/* 1 / 0 / -1 (reversed) 3-way comparator */
} SortFunctionKind;

extern void SelectSortFunction(Oid sortOperator,
				   RegProcedure *sortFunction,
				   SortFunctionKind *kind);

/*
 * Apply a sort function (by now converted to fmgr lookup form)
 * and return a 3-way comparison result.  This takes care of handling
 * NULLs and sort ordering direction properly.
 */
extern int32 ApplySortFunction(FmgrInfo *sortFunction, SortFunctionKind kind,
				  Datum datum1, bool isNull1,
				  Datum datum2, bool isNull2);

#endif   /* TUPLESORT_H */
