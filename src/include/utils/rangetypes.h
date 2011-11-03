/*-------------------------------------------------------------------------
 *
 * rangetypes.h
 *	  Declarations for Postgres range types.
 *
 */

#ifndef RANGETYPES_H
#define RANGETYPES_H

#include "fmgr.h"

typedef struct varlena RangeType;

typedef struct
{
	Datum		val;
	Oid			rngtypid;
	bool		infinite;
	bool		lower;
	bool		inclusive;
} RangeBound;

typedef struct
{
	FmgrInfo	canonicalFn;
	FmgrInfo	cmpFn;
	FmgrInfo	subdiffFn;
	Oid			rngtypid;
	Oid			subtype;
	Oid			collation;
	int16		subtyplen;
	char		subtypalign;
	char		subtypstorage;
	bool		subtypbyval;
} RangeTypeInfo;

/*
 * fmgr macros for range type objects
 */
#define DatumGetRangeType(X)			((RangeType *) PG_DETOAST_DATUM(X))
#define DatumGetRangeTypeCopy(X)		((RangeType *) PG_DETOAST_DATUM_COPY(X))
#define RangeTypeGetDatum(X)			PointerGetDatum(X)
#define PG_GETARG_RANGE(n)				DatumGetRangeType(PG_GETARG_DATUM(n))
#define PG_GETARG_RANGE_COPY(n)			DatumGetRangeTypeCopy(PG_GETARG_DATUM(n))
#define PG_RETURN_RANGE(x)				return RangeTypeGetDatum(x)

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
extern Datum range_make1(PG_FUNCTION_ARGS);
extern Datum range_linf_(PG_FUNCTION_ARGS);
extern Datum range_uinf_(PG_FUNCTION_ARGS);
extern Datum range_linfi(PG_FUNCTION_ARGS);
extern Datum range_uinfi(PG_FUNCTION_ARGS);
extern Datum range(PG_FUNCTION_ARGS);
extern Datum range__(PG_FUNCTION_ARGS);
extern Datum range_i(PG_FUNCTION_ARGS);
extern Datum rangei_(PG_FUNCTION_ARGS);
extern Datum rangeii(PG_FUNCTION_ARGS);

/* range -> subtype */
extern Datum range_lower(PG_FUNCTION_ARGS);
extern Datum range_upper(PG_FUNCTION_ARGS);

/* range -> bool */
extern Datum range_empty(PG_FUNCTION_ARGS);
extern Datum range_lower_inc(PG_FUNCTION_ARGS);
extern Datum range_upper_inc(PG_FUNCTION_ARGS);
extern Datum range_lower_inf(PG_FUNCTION_ARGS);
extern Datum range_upper_inf(PG_FUNCTION_ARGS);

/* range, point -> bool */
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

/* GiST support (rangetypes_gist.c) */
extern Datum range_gist_consistent(PG_FUNCTION_ARGS);
extern Datum range_gist_compress(PG_FUNCTION_ARGS);
extern Datum range_gist_decompress(PG_FUNCTION_ARGS);
extern Datum range_gist_union(PG_FUNCTION_ARGS);
extern Datum range_gist_penalty(PG_FUNCTION_ARGS);
extern Datum range_gist_picksplit(PG_FUNCTION_ARGS);
extern Datum range_gist_same(PG_FUNCTION_ARGS);

/* Canonical functions */
Datum int4range_canonical(PG_FUNCTION_ARGS);
Datum int8range_canonical(PG_FUNCTION_ARGS);
Datum daterange_canonical(PG_FUNCTION_ARGS);

/* Subtype Difference functions */
Datum int4range_subdiff(PG_FUNCTION_ARGS);
Datum int8range_subdiff(PG_FUNCTION_ARGS);
Datum numrange_subdiff(PG_FUNCTION_ARGS);
Datum daterange_subdiff(PG_FUNCTION_ARGS);
Datum tsrange_subdiff(PG_FUNCTION_ARGS);
Datum tstzrange_subdiff(PG_FUNCTION_ARGS);

/* for defining more generic functions */
extern Datum make_range(FunctionCallInfo fcinfo, RangeBound *lower,
						RangeBound *upper, bool empty);
extern void range_deserialize(FunctionCallInfo fcinfo, RangeType *range,
							  RangeBound *lower, RangeBound *upper,
							  bool *empty);
extern int range_cmp_bounds(FunctionCallInfo fcinfo, RangeBound *b1,
							RangeBound *b2);
extern RangeType *make_empty_range(FunctionCallInfo fcinfo, Oid rngtypid);
extern void range_gettypinfo(FunctionCallInfo fcinfo, Oid rngtypid,
							 RangeTypeInfo *rngtypinfo);

/* for defining a range "canonicalize" function */
extern Datum range_serialize(FunctionCallInfo fcinfo, RangeBound *lower,
							 RangeBound *upper, bool empty);

/* for use in DefineRange */
extern char range_parse_flags(char *flags_str);

#endif   /* RANGETYPES_H */
