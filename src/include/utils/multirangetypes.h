/*-------------------------------------------------------------------------
 *
 * multirangetypes.h
 *	  Declarations for Postgres multirange types.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/multirangetypes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MULTIRANGETYPES_H
#define MULTIRANGETYPES_H

#include "utils/rangetypes.h"
#include "utils/typcache.h"


/*
 * Multiranges are varlena objects, so must meet the varlena convention that
 * the first int32 of the object contains the total object size in bytes.
 * Be sure to use VARSIZE() and SET_VARSIZE() to access it, though!
 */
typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	Oid			multirangetypid;	/* multirange type's own OID */
	uint32		rangeCount;		/* the number of ranges */

	/*
	 * Following the count are the range objects themselves, as ShortRangeType
	 * structs. Note that ranges are varlena too, depending on whether they
	 * have lower/upper bounds and because even their base types can be
	 * varlena. So we can't really index into this list.
	 */
} MultirangeType;

/* Use these macros in preference to accessing these fields directly */
#define MultirangeTypeGetOid(mr)	((mr)->multirangetypid)
#define MultirangeIsEmpty(mr)  ((mr)->rangeCount == 0)

/*
 * fmgr functions for multirange type objects
 */
static inline MultirangeType *
DatumGetMultirangeTypeP(Datum X)
{
	return (MultirangeType *) PG_DETOAST_DATUM(X);
}

static inline MultirangeType *
DatumGetMultirangeTypePCopy(Datum X)
{
	return (MultirangeType *) PG_DETOAST_DATUM_COPY(X);
}

static inline Datum
MultirangeTypePGetDatum(const MultirangeType *X)
{
	return PointerGetDatum(X);
}

#define PG_GETARG_MULTIRANGE_P(n)		DatumGetMultirangeTypeP(PG_GETARG_DATUM(n))
#define PG_GETARG_MULTIRANGE_P_COPY(n)	DatumGetMultirangeTypePCopy(PG_GETARG_DATUM(n))
#define PG_RETURN_MULTIRANGE_P(x)		return MultirangeTypePGetDatum(x)

/*
 * prototypes for functions defined in multirangetypes.c
 */

/* internal versions of the above */
extern bool multirange_eq_internal(TypeCacheEntry *rangetyp,
								   const MultirangeType *mr1,
								   const MultirangeType *mr2);
extern bool multirange_ne_internal(TypeCacheEntry *rangetyp,
								   const MultirangeType *mr1,
								   const MultirangeType *mr2);
extern bool multirange_contains_elem_internal(TypeCacheEntry *rangetyp,
											  const MultirangeType *mr,
											  Datum val);
extern bool multirange_contains_range_internal(TypeCacheEntry *rangetyp,
											   const MultirangeType *mr,
											   const RangeType *r);
extern bool range_contains_multirange_internal(TypeCacheEntry *rangetyp,
											   const RangeType *r,
											   const MultirangeType *mr);
extern bool multirange_contains_multirange_internal(TypeCacheEntry *rangetyp,
													const MultirangeType *mr1,
													const MultirangeType *mr2);
extern bool range_overlaps_multirange_internal(TypeCacheEntry *rangetyp,
											   const RangeType *r,
											   const MultirangeType *mr);
extern bool multirange_overlaps_multirange_internal(TypeCacheEntry *rangetyp,
													const MultirangeType *mr1,
													const MultirangeType *mr2);
extern bool range_overleft_multirange_internal(TypeCacheEntry *rangetyp,
											   const RangeType *r,
											   const MultirangeType *mr);
extern bool range_overright_multirange_internal(TypeCacheEntry *rangetyp,
												const RangeType *r,
												const MultirangeType *mr);
extern bool range_before_multirange_internal(TypeCacheEntry *rangetyp,
											 const RangeType *r,
											 const MultirangeType *mr);
extern bool range_after_multirange_internal(TypeCacheEntry *rangetyp,
											const RangeType *r,
											const MultirangeType *mr);
extern bool range_adjacent_multirange_internal(TypeCacheEntry *rangetyp,
											   const RangeType *r,
											   const MultirangeType *mr);
extern bool multirange_before_multirange_internal(TypeCacheEntry *rangetyp,
												  const MultirangeType *mr1,
												  const MultirangeType *mr2);
extern MultirangeType *multirange_minus_internal(Oid mltrngtypoid,
												 TypeCacheEntry *rangetyp,
												 int32 range_count1,
												 RangeType **ranges1,
												 int32 range_count2,
												 RangeType **ranges2);
extern MultirangeType *multirange_intersect_internal(Oid mltrngtypoid,
													 TypeCacheEntry *rangetyp,
													 int32 range_count1,
													 RangeType **ranges1,
													 int32 range_count2,
													 RangeType **ranges2);

/* assorted support functions */
extern TypeCacheEntry *multirange_get_typcache(FunctionCallInfo fcinfo,
											   Oid mltrngtypid);
extern void multirange_deserialize(TypeCacheEntry *rangetyp,
								   const MultirangeType *multirange,
								   int32 *range_count,
								   RangeType ***ranges);
extern MultirangeType *make_multirange(Oid mltrngtypoid,
									   TypeCacheEntry *rangetyp,
									   int32 range_count, RangeType **ranges);
extern MultirangeType *make_empty_multirange(Oid mltrngtypoid,
											 TypeCacheEntry *rangetyp);
extern void multirange_get_bounds(TypeCacheEntry *rangetyp,
								  const MultirangeType *multirange,
								  uint32 i,
								  RangeBound *lower, RangeBound *upper);
extern RangeType *multirange_get_range(TypeCacheEntry *rangetyp,
									   const MultirangeType *multirange, int i);
extern RangeType *multirange_get_union_range(TypeCacheEntry *rangetyp,
											 const MultirangeType *mr);

#endif							/* MULTIRANGETYPES_H */
