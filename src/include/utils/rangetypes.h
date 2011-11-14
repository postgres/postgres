/*-------------------------------------------------------------------------
 *
 * rangetypes.h
 *	  Declarations for Postgres range types.
 *
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/rangetypes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RANGETYPES_H
#define RANGETYPES_H

#include "fmgr.h"


/* All ranges are represented as varlena objects */
typedef struct varlena RangeType;

/* Internal representation of either bound of a range (not what's on disk) */
typedef struct
{
	Datum		val;			/* the bound value, if any */
	Oid			rngtypid;		/* OID of the range type itself */
	bool		infinite;		/* bound is +/- infinity */
	bool		lower;			/* this is the lower (vs upper) bound */
	bool		inclusive;		/* bound is inclusive (vs exclusive) */
} RangeBound;

/* Standard runtime-cached data for a range type */
typedef struct
{
	FmgrInfo	canonicalFn;	/* canonicalization function, if any */
	FmgrInfo	cmpFn;			/* element type's btree comparison function */
	FmgrInfo	subdiffFn;		/* element type difference function, if any */
	Oid			rngtypid;		/* OID of the range type itself */
	Oid			subtype;		/* OID of the element type */
	Oid			collation;		/* collation for comparisons, if any */
	int16		subtyplen;		/* typlen of element type */
	char		subtypalign;	/* typalign of element type */
	char		subtypstorage;	/* typstorage of element type */
	bool		subtypbyval;	/* typbyval of element type */
} RangeTypeInfo;

/*
 * fmgr macros for range type objects
 */
#define DatumGetRangeType(X)		((RangeType *) PG_DETOAST_DATUM(X))
#define DatumGetRangeTypeCopy(X)	((RangeType *) PG_DETOAST_DATUM_COPY(X))
#define RangeTypeGetDatum(X)		PointerGetDatum(X)
#define PG_GETARG_RANGE(n)			DatumGetRangeType(PG_GETARG_DATUM(n))
#define PG_GETARG_RANGE_COPY(n)		DatumGetRangeTypeCopy(PG_GETARG_DATUM(n))
#define PG_RETURN_RANGE(x)			return RangeTypeGetDatum(x)

/*
 * prototypes for functions defined in rangetypes.c
 */

/* IO */
extern Datum anyrange_in(PG_FUNCTION_ARGS);
extern Datum anyrange_out(PG_FUNCTION_ARGS);
extern Datum range_in(PG_FUNCTION_ARGS);
extern Datum range_out(PG_FUNCTION_ARGS);
extern Datum range_recv(PG_FUNCTION_ARGS);
extern Datum range_send(PG_FUNCTION_ARGS);

/* constructors */
extern Datum range_constructor0(PG_FUNCTION_ARGS);
extern Datum range_constructor1(PG_FUNCTION_ARGS);
extern Datum range_constructor2(PG_FUNCTION_ARGS);
extern Datum range_constructor3(PG_FUNCTION_ARGS);

/* range -> subtype */
extern Datum range_lower(PG_FUNCTION_ARGS);
extern Datum range_upper(PG_FUNCTION_ARGS);

/* range -> bool */
extern Datum range_empty(PG_FUNCTION_ARGS);
extern Datum range_lower_inc(PG_FUNCTION_ARGS);
extern Datum range_upper_inc(PG_FUNCTION_ARGS);
extern Datum range_lower_inf(PG_FUNCTION_ARGS);
extern Datum range_upper_inf(PG_FUNCTION_ARGS);

/* range, element -> bool */
extern Datum range_contains_elem(PG_FUNCTION_ARGS);
extern Datum elem_contained_by_range(PG_FUNCTION_ARGS);

/* range, range -> bool */
extern Datum range_eq(PG_FUNCTION_ARGS);
extern Datum range_ne(PG_FUNCTION_ARGS);
extern Datum range_contains(PG_FUNCTION_ARGS);
extern Datum range_contained_by(PG_FUNCTION_ARGS);
extern Datum range_before(PG_FUNCTION_ARGS);
extern Datum range_after(PG_FUNCTION_ARGS);
extern Datum range_adjacent(PG_FUNCTION_ARGS);
extern Datum range_overlaps(PG_FUNCTION_ARGS);
extern Datum range_overleft(PG_FUNCTION_ARGS);
extern Datum range_overright(PG_FUNCTION_ARGS);

/* range, range -> range */
extern Datum range_minus(PG_FUNCTION_ARGS);
extern Datum range_union(PG_FUNCTION_ARGS);
extern Datum range_intersect(PG_FUNCTION_ARGS);

/* BTree support */
extern Datum range_cmp(PG_FUNCTION_ARGS);
extern Datum range_lt(PG_FUNCTION_ARGS);
extern Datum range_le(PG_FUNCTION_ARGS);
extern Datum range_ge(PG_FUNCTION_ARGS);
extern Datum range_gt(PG_FUNCTION_ARGS);

/* Hash support */
extern Datum hash_range(PG_FUNCTION_ARGS);

/* Canonical functions */
extern Datum int4range_canonical(PG_FUNCTION_ARGS);
extern Datum int8range_canonical(PG_FUNCTION_ARGS);
extern Datum daterange_canonical(PG_FUNCTION_ARGS);

/* Subtype Difference functions */
extern Datum int4range_subdiff(PG_FUNCTION_ARGS);
extern Datum int8range_subdiff(PG_FUNCTION_ARGS);
extern Datum numrange_subdiff(PG_FUNCTION_ARGS);
extern Datum daterange_subdiff(PG_FUNCTION_ARGS);
extern Datum tsrange_subdiff(PG_FUNCTION_ARGS);
extern Datum tstzrange_subdiff(PG_FUNCTION_ARGS);

/* assorted support functions */
extern Datum range_serialize(FunctionCallInfo fcinfo, RangeBound *lower,
							 RangeBound *upper, bool empty);
extern void range_deserialize(FunctionCallInfo fcinfo, RangeType *range,
							  RangeBound *lower, RangeBound *upper,
							  bool *empty);
extern Datum make_range(FunctionCallInfo fcinfo, RangeBound *lower,
						RangeBound *upper, bool empty);
extern int range_cmp_bounds(FunctionCallInfo fcinfo, RangeBound *b1,
							RangeBound *b2);
extern RangeType *make_empty_range(FunctionCallInfo fcinfo, Oid rngtypid);
extern void range_gettypinfo(FunctionCallInfo fcinfo, Oid rngtypid,
							 RangeTypeInfo *rngtypinfo);

/* GiST support (in rangetypes_gist.c) */
extern Datum range_gist_consistent(PG_FUNCTION_ARGS);
extern Datum range_gist_compress(PG_FUNCTION_ARGS);
extern Datum range_gist_decompress(PG_FUNCTION_ARGS);
extern Datum range_gist_union(PG_FUNCTION_ARGS);
extern Datum range_gist_penalty(PG_FUNCTION_ARGS);
extern Datum range_gist_picksplit(PG_FUNCTION_ARGS);
extern Datum range_gist_same(PG_FUNCTION_ARGS);

#endif   /* RANGETYPES_H */
