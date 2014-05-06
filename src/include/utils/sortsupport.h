/*-------------------------------------------------------------------------
 *
 * sortsupport.h
 *	  Framework for accelerated sorting.
 *
 * Traditionally, PostgreSQL has implemented sorting by repeatedly invoking
 * an SQL-callable comparison function "cmp(x, y) returns int" on pairs of
 * values to be compared, where the comparison function is the BTORDER_PROC
 * pg_amproc support function of the appropriate btree index opclass.
 *
 * This file defines alternative APIs that allow sorting to be performed with
 * reduced overhead.  To support lower-overhead sorting, a btree opclass may
 * provide a BTSORTSUPPORT_PROC pg_amproc entry, which must take a single
 * argument of type internal and return void.  The argument is actually a
 * pointer to a SortSupportData struct, which is defined below.
 *
 * If provided, the BTSORTSUPPORT function will be called during sort setup,
 * and it must initialize the provided struct with pointers to function(s)
 * that can be called to perform sorting.  This API is defined to allow
 * multiple acceleration mechanisms to be supported, but no opclass is
 * required to provide all of them.  The BTSORTSUPPORT function should
 * simply not set any function pointers for mechanisms it doesn't support.
 * (However, all opclasses that provide BTSORTSUPPORT are required to provide
 * the comparator function.)
 *
 * All sort support functions will be passed the address of the
 * SortSupportData struct when called, so they can use it to store
 * additional private data as needed.  In particular, for collation-aware
 * datatypes, the ssup_collation field is set before calling BTSORTSUPPORT
 * and is available to all support functions.  Additional opclass-dependent
 * data can be stored using the ssup_extra field.  Any such data
 * should be allocated in the ssup_cxt memory context.
 *
 * Note: since pg_amproc functions are indexed by (lefttype, righttype)
 * it is possible to associate a BTSORTSUPPORT function with a cross-type
 * comparison.  This could sensibly be used to provide a fast comparator
 * function for such cases, but probably not any other acceleration method.
 *
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/sortsupport.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SORTSUPPORT_H
#define SORTSUPPORT_H

#include "access/attnum.h"

typedef struct SortSupportData *SortSupport;

typedef struct SortSupportData
{
	/*
	 * These fields are initialized before calling the BTSORTSUPPORT function
	 * and should not be changed later.
	 */
	MemoryContext ssup_cxt;		/* Context containing sort info */
	Oid			ssup_collation; /* Collation to use, or InvalidOid */

	/*
	 * Additional sorting parameters; but unlike ssup_collation, these can be
	 * changed after BTSORTSUPPORT is called, so don't use them in selecting
	 * sort support functions.
	 */
	bool		ssup_reverse;	/* descending-order sort? */
	bool		ssup_nulls_first;		/* sort nulls first? */

	/*
	 * These fields are workspace for callers, and should not be touched by
	 * opclass-specific functions.
	 */
	AttrNumber	ssup_attno;		/* column number to sort */

	/*
	 * ssup_extra is zeroed before calling the BTSORTSUPPORT function, and is
	 * not touched subsequently by callers.
	 */
	void	   *ssup_extra;		/* Workspace for opclass functions */

	/*
	 * Function pointers are zeroed before calling the BTSORTSUPPORT function,
	 * and must be set by it for any acceleration methods it wants to supply.
	 * The comparator pointer must be set, others are optional.
	 */

	/*
	 * Comparator function has the same API as the traditional btree
	 * comparison function, ie, return <0, 0, or >0 according as x is less
	 * than, equal to, or greater than y.  Note that x and y are guaranteed
	 * not null, and there is no way to return null either.  Do not return
	 * INT_MIN, as callers are allowed to negate the result before using it.
	 */
	int			(*comparator) (Datum x, Datum y, SortSupport ssup);

	/*
	 * Additional sort-acceleration functions might be added here later.
	 */
} SortSupportData;


/*
 * ApplySortComparator should be inlined if possible.  See STATIC_IF_INLINE
 * in c.h.
 */
#ifndef PG_USE_INLINE
extern int ApplySortComparator(Datum datum1, bool isNull1,
					Datum datum2, bool isNull2,
					SortSupport ssup);
#endif   /* !PG_USE_INLINE */
#if defined(PG_USE_INLINE) || defined(SORTSUPPORT_INCLUDE_DEFINITIONS)
/*
 * Apply a sort comparator function and return a 3-way comparison result.
 * This takes care of handling reverse-sort and NULLs-ordering properly.
 */
STATIC_IF_INLINE int
ApplySortComparator(Datum datum1, bool isNull1,
					Datum datum2, bool isNull2,
					SortSupport ssup)
{
	int			compare;

	if (isNull1)
	{
		if (isNull2)
			compare = 0;		/* NULL "=" NULL */
		else if (ssup->ssup_nulls_first)
			compare = -1;		/* NULL "<" NOT_NULL */
		else
			compare = 1;		/* NULL ">" NOT_NULL */
	}
	else if (isNull2)
	{
		if (ssup->ssup_nulls_first)
			compare = 1;		/* NOT_NULL ">" NULL */
		else
			compare = -1;		/* NOT_NULL "<" NULL */
	}
	else
	{
		compare = (*ssup->comparator) (datum1, datum2, ssup);
		if (ssup->ssup_reverse)
			compare = -compare;
	}

	return compare;
}
#endif   /*-- PG_USE_INLINE || SORTSUPPORT_INCLUDE_DEFINITIONS */

/* Other functions in utils/sort/sortsupport.c */
extern void PrepareSortSupportComparisonShim(Oid cmpFunc, SortSupport ssup);
extern void PrepareSortSupportFromOrderingOp(Oid orderingOp, SortSupport ssup);

#endif   /* SORTSUPPORT_H */
