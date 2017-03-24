/*-------------------------------------------------------------------------
 *
 * extended_stats_internal.h
 *	  POSTGRES extended statistics internal declarations
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/include/statistics/extended_stats_internal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXTENDED_STATS_INTERNAL_H
#define EXTENDED_STATS_INTERNAL_H

#include "utils/sortsupport.h"
#include "statistics/statistics.h"


typedef struct
{
	Oid			eqopr;			/* '=' operator for datatype, if any */
	Oid			eqfunc;			/* and associated function */
	Oid			ltopr;			/* '<' operator for datatype, if any */
} StdAnalyzeData;

typedef struct
{
	Datum		value;			/* a data value */
	int			tupno;			/* position index for tuple it came from */
} ScalarItem;

/* multi-sort */
typedef struct MultiSortSupportData
{
	int			ndims;			/* number of dimensions supported by the */
	SortSupportData ssup[1];	/* sort support data for each dimension */
} MultiSortSupportData;

typedef MultiSortSupportData *MultiSortSupport;

typedef struct SortItem
{
	Datum	   *values;
	bool	   *isnull;
} SortItem;

extern MVNDistinct *statext_ndistinct_build(double totalrows,
						int numrows, HeapTuple *rows,
						Bitmapset *attrs, VacAttrStats **stats);
extern bytea *statext_ndistinct_serialize(MVNDistinct *ndistinct);
extern MVNDistinct *statext_ndistinct_deserialize(bytea *data);

extern MultiSortSupport multi_sort_init(int ndims);
extern void multi_sort_add_dimension(MultiSortSupport mss, int sortdim,
						 Oid oper);
extern int	multi_sort_compare(const void *a, const void *b, void *arg);
extern int multi_sort_compare_dim(int dim, const SortItem * a,
					   const SortItem * b, MultiSortSupport mss);
extern int multi_sort_compare_dims(int start, int end, const SortItem * a,
						const SortItem * b, MultiSortSupport mss);

#endif   /* EXTENDED_STATS_INTERNAL_H */
