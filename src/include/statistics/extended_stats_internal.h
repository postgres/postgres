/*-------------------------------------------------------------------------
 *
 * extended_stats_internal.h
 *	  POSTGRES extended statistics internal declarations
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/include/statistics/extended_stats_internal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXTENDED_STATS_INTERNAL_H
#define EXTENDED_STATS_INTERNAL_H

#include "statistics/statistics.h"
#include "utils/sortsupport.h"

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

/* (de)serialization info */
typedef struct DimensionInfo
{
	int			nvalues;		/* number of deduplicated values */
	int			nbytes;			/* number of bytes (serialized) */
	int			nbytes_aligned; /* size of deserialized data with alignment */
	int			typlen;			/* pg_type.typlen */
	bool		typbyval;		/* pg_type.typbyval */
} DimensionInfo;

/* multi-sort */
typedef struct MultiSortSupportData
{
	int			ndims;			/* number of dimensions */
	/* sort support data for each dimension: */
	SortSupportData ssup[FLEXIBLE_ARRAY_MEMBER];
} MultiSortSupportData;

typedef MultiSortSupportData *MultiSortSupport;

typedef struct SortItem
{
	Datum	   *values;
	bool	   *isnull;
	int			count;
} SortItem;

/* a unified representation of the data the statistics is built on */
typedef struct StatsBuildData
{
	int			numrows;
	int			nattnums;
	AttrNumber *attnums;
	VacAttrStats **stats;
	Datum	  **values;
	bool	  **nulls;
} StatsBuildData;


extern MVNDistinct *statext_ndistinct_build(double totalrows, StatsBuildData *data);
extern bytea *statext_ndistinct_serialize(MVNDistinct *ndistinct);
extern MVNDistinct *statext_ndistinct_deserialize(bytea *data);

extern MVDependencies *statext_dependencies_build(StatsBuildData *data);
extern bytea *statext_dependencies_serialize(MVDependencies *dependencies);
extern MVDependencies *statext_dependencies_deserialize(bytea *data);

extern MCVList *statext_mcv_build(StatsBuildData *data,
								  double totalrows, int stattarget);
extern bytea *statext_mcv_serialize(MCVList *mcv, VacAttrStats **stats);
extern MCVList *statext_mcv_deserialize(bytea *data);

extern MultiSortSupport multi_sort_init(int ndims);
extern void multi_sort_add_dimension(MultiSortSupport mss, int sortdim,
									 Oid oper, Oid collation);
extern int	multi_sort_compare(const void *a, const void *b, void *arg);
extern int	multi_sort_compare_dim(int dim, const SortItem *a,
								   const SortItem *b, MultiSortSupport mss);
extern int	multi_sort_compare_dims(int start, int end, const SortItem *a,
									const SortItem *b, MultiSortSupport mss);
extern int	compare_scalars_simple(const void *a, const void *b, void *arg);
extern int	compare_datums_simple(Datum a, Datum b, SortSupport ssup);

extern AttrNumber *build_attnums_array(Bitmapset *attrs, int nexprs, int *numattrs);

extern SortItem *build_sorted_items(StatsBuildData *data, int *nitems,
									MultiSortSupport mss,
									int numattrs, AttrNumber *attnums);

extern bool examine_opclause_args(List *args, Node **exprp,
								  Const **cstp, bool *expronleftp);

extern Selectivity mcv_combine_selectivities(Selectivity simple_sel,
											 Selectivity mcv_sel,
											 Selectivity mcv_basesel,
											 Selectivity mcv_totalsel);

extern Selectivity mcv_clauselist_selectivity(PlannerInfo *root,
											  StatisticExtInfo *stat,
											  List *clauses,
											  int varRelid,
											  JoinType jointype,
											  SpecialJoinInfo *sjinfo,
											  RelOptInfo *rel,
											  Selectivity *basesel,
											  Selectivity *totalsel);

extern Selectivity mcv_clause_selectivity_or(PlannerInfo *root,
											 StatisticExtInfo *stat,
											 MCVList *mcv,
											 Node *clause,
											 bool **or_matches,
											 Selectivity *basesel,
											 Selectivity *overlap_mcvsel,
											 Selectivity *overlap_basesel,
											 Selectivity *totalsel);

#endif							/* EXTENDED_STATS_INTERNAL_H */
