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
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/utils/tuplesort.h,v 1.23 2006/10/04 00:30:11 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TUPLESORT_H
#define TUPLESORT_H

#include "access/itup.h"
#include "executor/tuptable.h"


/* Tuplesortstate is an opaque type whose details are not known outside
 * tuplesort.c.
 */
typedef struct Tuplesortstate Tuplesortstate;

/*
 * We provide two different interfaces to what is essentially the same
 * code: one for sorting HeapTuples and one for sorting IndexTuples.
 * They differ primarily in the way that the sort key information is
 * supplied.  Also, the HeapTuple case actually stores MinimalTuples,
 * which means it doesn't preserve the "system columns" (tuple identity and
 * transaction visibility info).  The IndexTuple case does preserve all
 * the header fields of an index entry.  In the HeapTuple case we can
 * save some cycles by passing and returning the tuples in TupleTableSlots,
 * rather than forming actual HeapTuples (which'd have to be converted to
 * MinimalTuples).
 *
 * Yet a third slightly different interface supports sorting bare Datums.
 */

extern Tuplesortstate *tuplesort_begin_heap(TupleDesc tupDesc,
					 int nkeys,
					 Oid *sortOperators, AttrNumber *attNums,
					 int workMem, bool randomAccess);
extern Tuplesortstate *tuplesort_begin_index(Relation indexRel,
					  bool enforceUnique,
					  int workMem, bool randomAccess);
extern Tuplesortstate *tuplesort_begin_datum(Oid datumType,
					  Oid sortOperator,
					  int workMem, bool randomAccess);

extern void tuplesort_puttupleslot(Tuplesortstate *state,
					   TupleTableSlot *slot);
extern void tuplesort_putindextuple(Tuplesortstate *state, IndexTuple tuple);
extern void tuplesort_putdatum(Tuplesortstate *state, Datum val,
				   bool isNull);

extern void tuplesort_performsort(Tuplesortstate *state);

extern bool tuplesort_gettupleslot(Tuplesortstate *state, bool forward,
					   TupleTableSlot *slot);
extern IndexTuple tuplesort_getindextuple(Tuplesortstate *state, bool forward,
						bool *should_free);
extern bool tuplesort_getdatum(Tuplesortstate *state, bool forward,
				   Datum *val, bool *isNull);

extern void tuplesort_end(Tuplesortstate *state);

extern int	tuplesort_merge_order(long allowedMem);

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
