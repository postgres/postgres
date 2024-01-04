/*-------------------------------------------------------------------------
 *
 * rangetypes.h
 *	  Declarations for Postgres range types.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/rangetypes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RANGETYPES_H
#define RANGETYPES_H

#include "utils/typcache.h"


/*
 * Ranges are varlena objects, so must meet the varlena convention that
 * the first int32 of the object contains the total object size in bytes.
 * Be sure to use VARSIZE() and SET_VARSIZE() to access it, though!
 */
typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	Oid			rangetypid;		/* range type's own OID */
	/* Following the OID are zero to two bound values, then a flags byte */
} RangeType;

#define RANGE_EMPTY_LITERAL "empty"

/* Use this macro in preference to fetching rangetypid field directly */
#define RangeTypeGetOid(r)	((r)->rangetypid)

/* A range's flags byte contains these bits: */
#define RANGE_EMPTY			0x01	/* range is empty */
#define RANGE_LB_INC		0x02	/* lower bound is inclusive */
#define RANGE_UB_INC		0x04	/* upper bound is inclusive */
#define RANGE_LB_INF		0x08	/* lower bound is -infinity */
#define RANGE_UB_INF		0x10	/* upper bound is +infinity */
#define RANGE_LB_NULL		0x20	/* lower bound is null (NOT USED) */
#define RANGE_UB_NULL		0x40	/* upper bound is null (NOT USED) */
#define RANGE_CONTAIN_EMPTY 0x80	/* marks a GiST internal-page entry whose
									 * subtree contains some empty ranges */

#define RANGE_HAS_LBOUND(flags) (!((flags) & (RANGE_EMPTY | \
											  RANGE_LB_NULL | \
											  RANGE_LB_INF)))

#define RANGE_HAS_UBOUND(flags) (!((flags) & (RANGE_EMPTY | \
											  RANGE_UB_NULL | \
											  RANGE_UB_INF)))

#define RangeIsEmpty(r)  ((range_get_flags(r) & RANGE_EMPTY) != 0)
#define RangeIsOrContainsEmpty(r)  \
	((range_get_flags(r) & (RANGE_EMPTY | RANGE_CONTAIN_EMPTY)) != 0)


/* Internal representation of either bound of a range (not what's on disk) */
typedef struct
{
	Datum		val;			/* the bound value, if any */
	bool		infinite;		/* bound is +/- infinity */
	bool		inclusive;		/* bound is inclusive (vs exclusive) */
	bool		lower;			/* this is the lower (vs upper) bound */
} RangeBound;

/*
 * fmgr functions for range type objects
 */
static inline RangeType *
DatumGetRangeTypeP(Datum X)
{
	return (RangeType *) PG_DETOAST_DATUM(X);
}

static inline RangeType *
DatumGetRangeTypePCopy(Datum X)
{
	return (RangeType *) PG_DETOAST_DATUM_COPY(X);
}

static inline Datum
RangeTypePGetDatum(const RangeType *X)
{
	return PointerGetDatum(X);
}

#define PG_GETARG_RANGE_P(n)		DatumGetRangeTypeP(PG_GETARG_DATUM(n))
#define PG_GETARG_RANGE_P_COPY(n)	DatumGetRangeTypePCopy(PG_GETARG_DATUM(n))
#define PG_RETURN_RANGE_P(x)		return RangeTypePGetDatum(x)

/* Operator strategy numbers used in the GiST and SP-GiST range opclasses */
/* Numbers are chosen to match up operator names with existing usages */
#define RANGESTRAT_BEFORE				RTLeftStrategyNumber
#define RANGESTRAT_OVERLEFT				RTOverLeftStrategyNumber
#define RANGESTRAT_OVERLAPS				RTOverlapStrategyNumber
#define RANGESTRAT_OVERRIGHT			RTOverRightStrategyNumber
#define RANGESTRAT_AFTER				RTRightStrategyNumber
#define RANGESTRAT_ADJACENT				RTSameStrategyNumber
#define RANGESTRAT_CONTAINS				RTContainsStrategyNumber
#define RANGESTRAT_CONTAINED_BY			RTContainedByStrategyNumber
#define RANGESTRAT_CONTAINS_ELEM		RTContainsElemStrategyNumber
#define RANGESTRAT_EQ					RTEqualStrategyNumber

/*
 * prototypes for functions defined in rangetypes.c
 */

extern bool range_contains_elem_internal(TypeCacheEntry *typcache, const RangeType *r, Datum val);

/* internal versions of the above */
extern bool range_eq_internal(TypeCacheEntry *typcache, const RangeType *r1,
							  const RangeType *r2);
extern bool range_ne_internal(TypeCacheEntry *typcache, const RangeType *r1,
							  const RangeType *r2);
extern bool range_contains_internal(TypeCacheEntry *typcache, const RangeType *r1,
									const RangeType *r2);
extern bool range_contained_by_internal(TypeCacheEntry *typcache, const RangeType *r1,
										const RangeType *r2);
extern bool range_before_internal(TypeCacheEntry *typcache, const RangeType *r1,
								  const RangeType *r2);
extern bool range_after_internal(TypeCacheEntry *typcache, const RangeType *r1,
								 const RangeType *r2);
extern bool range_adjacent_internal(TypeCacheEntry *typcache, const RangeType *r1,
									const RangeType *r2);
extern bool range_overlaps_internal(TypeCacheEntry *typcache, const RangeType *r1,
									const RangeType *r2);
extern bool range_overleft_internal(TypeCacheEntry *typcache, const RangeType *r1,
									const RangeType *r2);
extern bool range_overright_internal(TypeCacheEntry *typcache, const RangeType *r1,
									 const RangeType *r2);
extern RangeType *range_union_internal(TypeCacheEntry *typcache, RangeType *r1,
									   RangeType *r2, bool strict);
extern RangeType *range_minus_internal(TypeCacheEntry *typcache, RangeType *r1,
									   RangeType *r2);
extern RangeType *range_intersect_internal(TypeCacheEntry *typcache, const RangeType *r1,
										   const RangeType *r2);

/* assorted support functions */
extern TypeCacheEntry *range_get_typcache(FunctionCallInfo fcinfo,
										  Oid rngtypid);
extern RangeType *range_serialize(TypeCacheEntry *typcache, RangeBound *lower,
								  RangeBound *upper, bool empty,
								  struct Node *escontext);
extern void range_deserialize(TypeCacheEntry *typcache, const RangeType *range,
							  RangeBound *lower, RangeBound *upper,
							  bool *empty);
extern char range_get_flags(const RangeType *range);
extern void range_set_contain_empty(RangeType *range);
extern RangeType *make_range(TypeCacheEntry *typcache, RangeBound *lower,
							 RangeBound *upper, bool empty,
							 struct Node *escontext);
extern int	range_cmp_bounds(TypeCacheEntry *typcache, const RangeBound *b1,
							 const RangeBound *b2);
extern int	range_cmp_bound_values(TypeCacheEntry *typcache, const RangeBound *b1,
								   const RangeBound *b2);
extern int	range_compare(const void *key1, const void *key2, void *arg);
extern bool bounds_adjacent(TypeCacheEntry *typcache, RangeBound boundA,
							RangeBound boundB);
extern RangeType *make_empty_range(TypeCacheEntry *typcache);
extern bool range_split_internal(TypeCacheEntry *typcache, const RangeType *r1,
								 const RangeType *r2, RangeType **output1,
								 RangeType **output2);

#endif							/* RANGETYPES_H */
