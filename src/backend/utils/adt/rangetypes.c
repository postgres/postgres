/*-------------------------------------------------------------------------
 *
 * rangetypes.c
 *	  I/O functions, operators, and support functions for range types.
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/rangetypes.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "access/nbtree.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_range.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/fmgroids.h"
#include "utils/int8.h"
#include "utils/lsyscache.h"
#include "utils/rangetypes.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/typcache.h"


#define TYPE_IS_PACKABLE(typlen, typstorage) \
	(typlen == -1 && typstorage != 'p')

/* flags */
#define RANGE_EMPTY		0x01
#define RANGE_LB_INC	0x02
#define RANGE_LB_NULL	0x04	/* NOT USED */
#define RANGE_LB_INF	0x08
#define RANGE_UB_INC	0x10
#define RANGE_UB_NULL	0x20	/* NOT USED */
#define RANGE_UB_INF	0x40

#define RANGE_HAS_LBOUND(flags) (!(flags & (RANGE_EMPTY |	\
											RANGE_LB_NULL | \
											RANGE_LB_INF)))

#define RANGE_HAS_UBOUND(flags) (!(flags & (RANGE_EMPTY |	\
											RANGE_UB_NULL | \
											RANGE_UB_INF)))

#define RANGE_EMPTY_LITERAL "empty"

#define RANGE_DEFAULT_FLAGS		"[)"


static char range_parse_flags(const char *flags_str);
static void range_parse(char *input_str, char *flags, char **lbound_str,
			char **ubound_str);
static char *range_parse_bound(char *string, char *ptr, char **bound_str,
				  bool *infinite);
static char *range_deparse(char flags, char *lbound_str, char *ubound_str);
static char *range_bound_escape(char *in_str);
static bool range_contains_internal(FunctionCallInfo fcinfo, RangeType *r1,
						RangeType *r2);
static Size datum_compute_size(Size sz, Datum datum, bool typbyval,
				   char typalign, int16 typlen, char typstorage);
static Pointer datum_write(Pointer ptr, Datum datum, bool typbyval,
			char typalign, int16 typlen, char typstorage);


/*
 *----------------------------------------------------------
 * I/O FUNCTIONS
 *----------------------------------------------------------
 */

Datum
range_in(PG_FUNCTION_ARGS)
{
	char	   *input_str = PG_GETARG_CSTRING(0);
	Oid			rngtypoid = PG_GETARG_OID(1);
	Oid			typmod = PG_GETARG_INT32(2);
	Datum		range;
	char		flags;
	char	   *lbound_str;
	char	   *ubound_str;
	regproc		subInput;
	FmgrInfo	subInputFn;
	Oid			ioParam;
	RangeTypeInfo rngtypinfo;
	RangeBound	lower;
	RangeBound	upper;

	if (rngtypoid == ANYRANGEOID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot accept a value of type anyrange")));

	range_gettypinfo(fcinfo, rngtypoid, &rngtypinfo);

	/* parse */
	range_parse(input_str, &flags, &lbound_str, &ubound_str);

	/* input */
	getTypeInputInfo(rngtypinfo.subtype, &subInput, &ioParam);
	fmgr_info(subInput, &subInputFn);

	lower.rngtypid = rngtypoid;
	lower.infinite = (flags & RANGE_LB_INF) != 0;
	lower.inclusive = (flags & RANGE_LB_INC) != 0;
	lower.lower = true;
	upper.rngtypid = rngtypoid;
	upper.infinite = (flags & RANGE_UB_INF) != 0;
	upper.inclusive = (flags & RANGE_UB_INC) != 0;
	upper.lower = false;

	if (RANGE_HAS_LBOUND(flags))
		lower.val = InputFunctionCall(&subInputFn, lbound_str,
									  ioParam, typmod);
	if (RANGE_HAS_UBOUND(flags))
		upper.val = InputFunctionCall(&subInputFn, ubound_str,
									  ioParam, typmod);

	/* serialize and canonicalize */
	range = make_range(fcinfo, &lower, &upper, flags & RANGE_EMPTY);

	PG_RETURN_RANGE(range);
}

Datum
range_out(PG_FUNCTION_ARGS)
{
	RangeType  *range = PG_GETARG_RANGE(0);
	char	   *output_str;
	regproc		subOutput;
	FmgrInfo	subOutputFn;
	bool		isVarlena;
	char		flags = 0;
	char	   *lbound_str = NULL;
	char	   *ubound_str = NULL;
	bool		empty;
	RangeTypeInfo rngtypinfo;
	RangeBound	lower;
	RangeBound	upper;

	/* deserialize */
	range_deserialize(fcinfo, range, &lower, &upper, &empty);

	if (lower.rngtypid != upper.rngtypid)
		elog(ERROR, "range types do not match");

	range_gettypinfo(fcinfo, lower.rngtypid, &rngtypinfo);

	if (empty)
		flags |= RANGE_EMPTY;

	flags |= (lower.inclusive) ? RANGE_LB_INC : 0;
	flags |= (lower.infinite) ? RANGE_LB_INF : 0;
	flags |= (upper.inclusive) ? RANGE_UB_INC : 0;
	flags |= (upper.infinite) ? RANGE_UB_INF : 0;

	/* output */
	getTypeOutputInfo(rngtypinfo.subtype, &subOutput, &isVarlena);
	fmgr_info(subOutput, &subOutputFn);

	if (RANGE_HAS_LBOUND(flags))
		lbound_str = OutputFunctionCall(&subOutputFn, lower.val);
	if (RANGE_HAS_UBOUND(flags))
		ubound_str = OutputFunctionCall(&subOutputFn, upper.val);

	/* deparse */
	output_str = range_deparse(flags, lbound_str, ubound_str);

	PG_RETURN_CSTRING(output_str);
}

/*
 * Binary representation: The first byte is the flags, then 4 bytes are the
 * range type Oid, then the lower bound (if present) then the upper bound (if
 * present). Each bound is represented by a 4-byte length header and the binary
 * representation of that bound (as returned by a call to the send function for
 * the subtype).
 */

Datum
range_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	Oid			rngtypid = PG_GETARG_OID(1);
	int32		typmod = PG_GETARG_INT32(2);
	Datum		range;
	Oid			subrecv;
	Oid			ioparam;
	char		flags;
	RangeBound	lower;
	RangeBound	upper;
	RangeTypeInfo rngtypinfo;

	flags = (unsigned char) pq_getmsgbyte(buf);

	range_gettypinfo(fcinfo, rngtypid, &rngtypinfo);

	getTypeBinaryInputInfo(rngtypinfo.subtype, &subrecv, &ioparam);

	if (RANGE_HAS_LBOUND(flags))
	{
		uint32		bound_len = pq_getmsgint(buf, 4);
		const char *bound_data = pq_getmsgbytes(buf, bound_len);
		StringInfoData bound_buf;

		initStringInfo(&bound_buf);
		appendBinaryStringInfo(&bound_buf, bound_data, bound_len);

		lower.val = OidReceiveFunctionCall(subrecv,
										   &bound_buf,
										   ioparam,
										   typmod);
		pfree(bound_buf.data);
	}
	else
		lower.val = (Datum) 0;

	if (RANGE_HAS_UBOUND(flags))
	{
		uint32		bound_len = pq_getmsgint(buf, 4);
		const char *bound_data = pq_getmsgbytes(buf, bound_len);
		StringInfoData bound_buf;

		initStringInfo(&bound_buf);
		appendBinaryStringInfo(&bound_buf, bound_data, bound_len);

		upper.val = OidReceiveFunctionCall(subrecv,
										   &bound_buf,
										   ioparam,
										   typmod);
		pfree(bound_buf.data);
	}
	else
		upper.val = (Datum) 0;

	pq_getmsgend(buf);

	lower.rngtypid = rngtypid;
	lower.infinite = (flags & RANGE_LB_INF) != 0;
	lower.inclusive = (flags & RANGE_LB_INC) != 0;
	lower.lower = true;
	upper.rngtypid = rngtypid;
	upper.infinite = (flags & RANGE_UB_INF) != 0;
	upper.inclusive = (flags & RANGE_UB_INC) != 0;
	upper.lower = false;

	/* serialize and canonicalize */
	range = make_range(fcinfo, &lower, &upper, flags & RANGE_EMPTY);

	PG_RETURN_RANGE(range);
}

Datum
range_send(PG_FUNCTION_ARGS)
{
	RangeType  *range = PG_GETARG_RANGE(0);
	StringInfo	buf = makeStringInfo();
	char		flags = 0;
	RangeBound	lower;
	RangeBound	upper;
	bool		empty;
	Oid			subsend;
	bool		typIsVarlena;
	RangeTypeInfo rngtypinfo;

	pq_begintypsend(buf);

	range_deserialize(fcinfo, range, &lower, &upper, &empty);

	if (empty)
		flags |= RANGE_EMPTY;

	flags |= (lower.inclusive) ? RANGE_LB_INC : 0;
	flags |= (lower.infinite) ? RANGE_LB_INF : 0;
	flags |= (upper.inclusive) ? RANGE_UB_INC : 0;
	flags |= (upper.infinite) ? RANGE_UB_INF : 0;

	range_gettypinfo(fcinfo, lower.rngtypid, &rngtypinfo);

	getTypeBinaryOutputInfo(rngtypinfo.subtype,
							&subsend, &typIsVarlena);

	pq_sendbyte(buf, flags);

	if (RANGE_HAS_LBOUND(flags))
	{
		Datum		bound = PointerGetDatum(OidSendFunctionCall(subsend,
																lower.val));
		uint32		bound_len = VARSIZE(bound) - VARHDRSZ;
		char	   *bound_data = VARDATA(bound);

		pq_sendint(buf, bound_len, 4);
		pq_sendbytes(buf, bound_data, bound_len);
	}

	if (RANGE_HAS_UBOUND(flags))
	{
		Datum		bound = PointerGetDatum(OidSendFunctionCall(subsend,
																upper.val));
		uint32		bound_len = VARSIZE(bound) - VARHDRSZ;
		char	   *bound_data = VARDATA(bound);

		pq_sendint(buf, bound_len, 4);
		pq_sendbytes(buf, bound_data, bound_len);
	}

	PG_RETURN_BYTEA_P(pq_endtypsend(buf));
}


/*
 *----------------------------------------------------------
 * GENERIC FUNCTIONS
 *----------------------------------------------------------
 */

Datum
range_constructor0(PG_FUNCTION_ARGS)
{
	Oid			rngtypid = get_fn_expr_rettype(fcinfo->flinfo);
	RangeType  *range;
	RangeBound	lower;
	RangeBound	upper;

	lower.rngtypid = rngtypid;
	lower.val = (Datum) 0;
	lower.inclusive = false;
	lower.infinite = false;
	lower.lower = true;

	upper.rngtypid = rngtypid;
	upper.val = (Datum) 0;
	upper.inclusive = false;
	upper.infinite = false;
	upper.lower = false;

	range = DatumGetRangeType(make_range(fcinfo, &lower, &upper, true));

	PG_RETURN_RANGE(range);
}

Datum
range_constructor1(PG_FUNCTION_ARGS)
{
	Datum		arg1 = PG_GETARG_DATUM(0);
	Oid			rngtypid = get_fn_expr_rettype(fcinfo->flinfo);
	RangeType  *range;
	RangeBound	lower;
	RangeBound	upper;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("argument must not be NULL")));

	lower.rngtypid = rngtypid;
	lower.val = arg1;
	lower.inclusive = true;
	lower.infinite = false;
	lower.lower = true;

	upper.rngtypid = rngtypid;
	upper.val = arg1;
	upper.inclusive = true;
	upper.infinite = false;
	upper.lower = false;

	range = DatumGetRangeType(make_range(fcinfo, &lower, &upper, false));

	PG_RETURN_RANGE(range);
}

Datum
range_constructor2(PG_FUNCTION_ARGS)
{
	Datum		arg1 = PG_GETARG_DATUM(0);
	Datum		arg2 = PG_GETARG_DATUM(1);
	Oid			rngtypid = get_fn_expr_rettype(fcinfo->flinfo);
	RangeType  *range;
	RangeBound	lower;
	RangeBound	upper;
	char		flags;

	flags = range_parse_flags(RANGE_DEFAULT_FLAGS);

	lower.rngtypid = rngtypid;
	lower.val = PG_ARGISNULL(0) ? (Datum) 0 : arg1;
	lower.inclusive = flags & RANGE_LB_INC;
	lower.infinite = PG_ARGISNULL(0);
	lower.lower = true;

	upper.rngtypid = rngtypid;
	upper.val = PG_ARGISNULL(1) ? (Datum) 0 : arg2;
	upper.inclusive = flags & RANGE_UB_INC;
	upper.infinite = PG_ARGISNULL(1);
	upper.lower = false;

	range = DatumGetRangeType(make_range(fcinfo, &lower, &upper, false));

	PG_RETURN_RANGE(range);
}

Datum
range_constructor3(PG_FUNCTION_ARGS)
{
	Datum		arg1 = PG_GETARG_DATUM(0);
	Datum		arg2 = PG_GETARG_DATUM(1);
	Oid			rngtypid = get_fn_expr_rettype(fcinfo->flinfo);
	RangeType  *range;
	RangeBound	lower;
	RangeBound	upper;
	char		flags;

	if (PG_ARGISNULL(2))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("flags argument must not be NULL")));

	flags = range_parse_flags(text_to_cstring(PG_GETARG_TEXT_P(2)));

	lower.rngtypid = rngtypid;
	lower.val = PG_ARGISNULL(0) ? (Datum) 0 : arg1;
	lower.inclusive = flags & RANGE_LB_INC;
	lower.infinite = PG_ARGISNULL(0);
	lower.lower = true;

	upper.rngtypid = rngtypid;
	upper.val = PG_ARGISNULL(1) ? (Datum) 0 : arg2;
	upper.inclusive = flags & RANGE_UB_INC;
	upper.infinite = PG_ARGISNULL(1);
	upper.lower = false;

	range = DatumGetRangeType(make_range(fcinfo, &lower, &upper, false));

	PG_RETURN_RANGE(range);
}

/* range -> subtype */
Datum
range_lower(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE(0);
	RangeBound	lower;
	RangeBound	upper;
	bool		empty;

	range_deserialize(fcinfo, r1, &lower, &upper, &empty);

	if (empty)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("empty range has no lower bound")));
	if (lower.infinite)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("range lower bound is infinite")));

	PG_RETURN_DATUM(lower.val);
}

Datum
range_upper(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE(0);
	RangeBound	lower;
	RangeBound	upper;
	bool		empty;

	range_deserialize(fcinfo, r1, &lower, &upper, &empty);

	if (empty)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("empty range has no upper bound")));
	if (upper.infinite)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("range upper bound is infinite")));

	PG_RETURN_DATUM(upper.val);
}


/* range -> bool */
Datum
range_empty(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE(0);
	RangeBound	lower;
	RangeBound	upper;
	bool		empty;

	range_deserialize(fcinfo, r1, &lower, &upper, &empty);

	PG_RETURN_BOOL(empty);
}

Datum
range_lower_inc(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE(0);
	RangeBound	lower;
	RangeBound	upper;
	bool		empty;

	range_deserialize(fcinfo, r1, &lower, &upper, &empty);

	PG_RETURN_BOOL(lower.inclusive);
}

Datum
range_upper_inc(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE(0);
	RangeBound	lower;
	RangeBound	upper;
	bool		empty;

	range_deserialize(fcinfo, r1, &lower, &upper, &empty);

	PG_RETURN_BOOL(upper.inclusive);
}

Datum
range_lower_inf(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE(0);
	RangeBound	lower;
	RangeBound	upper;
	bool		empty;

	range_deserialize(fcinfo, r1, &lower, &upper, &empty);

	PG_RETURN_BOOL(lower.infinite);
}

Datum
range_upper_inf(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE(0);
	RangeBound	lower;
	RangeBound	upper;
	bool		empty;

	range_deserialize(fcinfo, r1, &lower, &upper, &empty);

	PG_RETURN_BOOL(upper.infinite);
}


/* range, range -> bool */
Datum
range_eq(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE(0);
	RangeType  *r2 = PG_GETARG_RANGE(1);
	RangeBound	lower1,
				lower2;
	RangeBound	upper1,
				upper2;
	bool		empty1,
				empty2;

	range_deserialize(fcinfo, r1, &lower1, &upper1, &empty1);
	range_deserialize(fcinfo, r2, &lower2, &upper2, &empty2);

	if (lower1.rngtypid != upper1.rngtypid ||
		lower1.rngtypid != lower2.rngtypid ||
		lower1.rngtypid != upper2.rngtypid)
		elog(ERROR, "range types do not match");

	if (empty1 && empty2)
		PG_RETURN_BOOL(true);
	if (empty1 != empty2)
		PG_RETURN_BOOL(false);

	if (range_cmp_bounds(fcinfo, &lower1, &lower2) != 0)
		PG_RETURN_BOOL(false);

	if (range_cmp_bounds(fcinfo, &upper1, &upper2) != 0)
		PG_RETURN_BOOL(false);

	PG_RETURN_BOOL(true);
}

Datum
range_ne(PG_FUNCTION_ARGS)
{
	bool		eq = DatumGetBool(range_eq(fcinfo));

	PG_RETURN_BOOL(!eq);
}

Datum
range_contains_elem(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE(0);
	Datum		val = PG_GETARG_DATUM(1);
	RangeType  *r2;
	RangeBound	lower1,
				lower2;
	RangeBound	upper1,
				upper2;
	bool		empty1;

	range_deserialize(fcinfo, r1, &lower1, &upper1, &empty1);

	lower2.rngtypid = lower1.rngtypid;
	lower2.inclusive = true;
	lower2.infinite = false;
	lower2.lower = true;
	lower2.val = val;

	upper2.rngtypid = lower1.rngtypid;
	upper2.inclusive = true;
	upper2.infinite = false;
	upper2.lower = false;
	upper2.val = val;

	r2 = DatumGetRangeType(make_range(fcinfo, &lower2, &upper2, false));

	PG_RETURN_BOOL(range_contains_internal(fcinfo, r1, r2));
}

Datum
range_contains(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE(0);
	RangeType  *r2 = PG_GETARG_RANGE(1);

	PG_RETURN_BOOL(range_contains_internal(fcinfo, r1, r2));
}

Datum
elem_contained_by_range(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE(1);
	Datum		val = PG_GETARG_DATUM(0);
	RangeType  *r2;
	RangeBound	lower1,
				lower2;
	RangeBound	upper1,
				upper2;
	bool		empty1;

	range_deserialize(fcinfo, r1, &lower1, &upper1, &empty1);

	lower2.rngtypid = lower1.rngtypid;
	lower2.inclusive = true;
	lower2.infinite = false;
	lower2.lower = true;
	lower2.val = val;

	upper2.rngtypid = lower1.rngtypid;
	upper2.inclusive = true;
	upper2.infinite = false;
	upper2.lower = false;
	upper2.val = val;

	r2 = DatumGetRangeType(make_range(fcinfo, &lower2, &upper2, false));

	PG_RETURN_BOOL(range_contains_internal(fcinfo, r1, r2));
}

Datum
range_contained_by(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE(0);
	RangeType  *r2 = PG_GETARG_RANGE(1);

	PG_RETURN_BOOL(range_contains_internal(fcinfo, r2, r1));
}

Datum
range_before(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE(0);
	RangeType  *r2 = PG_GETARG_RANGE(1);
	RangeBound	lower1,
				lower2;
	RangeBound	upper1,
				upper2;
	bool		empty1,
				empty2;

	range_deserialize(fcinfo, r1, &lower1, &upper1, &empty1);
	range_deserialize(fcinfo, r2, &lower2, &upper2, &empty2);

	if (lower1.rngtypid != upper1.rngtypid ||
		lower1.rngtypid != lower2.rngtypid ||
		lower1.rngtypid != upper2.rngtypid)
		elog(ERROR, "range types do not match");

	/* An empty range is neither before nor after any other range */
	if (empty1 || empty2)
		PG_RETURN_BOOL(false);

	PG_RETURN_BOOL(range_cmp_bounds(fcinfo, &upper1, &lower2) < 0);
}

Datum
range_after(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE(0);
	RangeType  *r2 = PG_GETARG_RANGE(1);
	RangeBound	lower1,
				lower2;
	RangeBound	upper1,
				upper2;
	bool		empty1,
				empty2;

	range_deserialize(fcinfo, r1, &lower1, &upper1, &empty1);
	range_deserialize(fcinfo, r2, &lower2, &upper2, &empty2);

	if (lower1.rngtypid != upper1.rngtypid ||
		lower1.rngtypid != lower2.rngtypid ||
		lower1.rngtypid != upper2.rngtypid)
		elog(ERROR, "range types do not match");

	/* An empty range is neither before nor after any other range */
	if (empty1 || empty2)
		PG_RETURN_BOOL(false);

	PG_RETURN_BOOL(range_cmp_bounds(fcinfo, &lower1, &upper2) > 0);
}

Datum
range_adjacent(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE(0);
	RangeType  *r2 = PG_GETARG_RANGE(1);
	RangeTypeInfo rngtypinfo;
	RangeBound	lower1,
				lower2;
	RangeBound	upper1,
				upper2;
	bool		empty1,
				empty2;

	range_deserialize(fcinfo, r1, &lower1, &upper1, &empty1);
	range_deserialize(fcinfo, r2, &lower2, &upper2, &empty2);

	if (lower1.rngtypid != upper1.rngtypid ||
		lower1.rngtypid != lower2.rngtypid ||
		lower1.rngtypid != upper2.rngtypid)
		elog(ERROR, "range types do not match");

	/* An empty range is not adjacent to any other range */
	if (empty1 || empty2)
		PG_RETURN_BOOL(false);

	/*
	 * For two ranges to be adjacent, the lower boundary of one range has to
	 * match the upper boundary of the other. However, the inclusivity of
	 * those two boundaries must also be different.
	 *
	 * The semantics for range_cmp_bounds aren't quite what we need here, so
	 * we do the comparison more directly.
	 */

	range_gettypinfo(fcinfo, lower1.rngtypid, &rngtypinfo);

	if (lower1.inclusive != upper2.inclusive)
	{
		if (DatumGetInt32(FunctionCall2Coll(&rngtypinfo.cmpFn,
											rngtypinfo.collation,
											lower1.val, upper2.val)) == 0)
			PG_RETURN_BOOL(true);
	}

	if (upper1.inclusive != lower2.inclusive)
	{
		if (DatumGetInt32(FunctionCall2Coll(&rngtypinfo.cmpFn,
											rngtypinfo.collation,
											upper1.val, lower2.val)) == 0)
			PG_RETURN_BOOL(true);
	}

	PG_RETURN_BOOL(false);
}

Datum
range_overlaps(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE(0);
	RangeType  *r2 = PG_GETARG_RANGE(1);
	RangeBound	lower1,
				lower2;
	RangeBound	upper1,
				upper2;
	bool		empty1,
				empty2;

	range_deserialize(fcinfo, r1, &lower1, &upper1, &empty1);
	range_deserialize(fcinfo, r2, &lower2, &upper2, &empty2);

	if (lower1.rngtypid != upper1.rngtypid ||
		lower1.rngtypid != lower2.rngtypid ||
		lower1.rngtypid != upper2.rngtypid)
		elog(ERROR, "range types do not match");

	/* An empty range does not overlap any other range */
	if (empty1 || empty2)
		PG_RETURN_BOOL(false);

	if (range_cmp_bounds(fcinfo, &lower1, &lower2) >= 0 &&
		range_cmp_bounds(fcinfo, &lower1, &upper2) <= 0)
		PG_RETURN_BOOL(true);

	if (range_cmp_bounds(fcinfo, &lower2, &lower1) >= 0 &&
		range_cmp_bounds(fcinfo, &lower2, &upper1) <= 0)
		PG_RETURN_BOOL(true);

	PG_RETURN_BOOL(false);
}

Datum
range_overleft(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE(0);
	RangeType  *r2 = PG_GETARG_RANGE(1);
	RangeBound	lower1,
				lower2;
	RangeBound	upper1,
				upper2;
	bool		empty1,
				empty2;

	range_deserialize(fcinfo, r1, &lower1, &upper1, &empty1);
	range_deserialize(fcinfo, r2, &lower2, &upper2, &empty2);

	if (lower1.rngtypid != upper1.rngtypid ||
		lower1.rngtypid != lower2.rngtypid ||
		lower1.rngtypid != upper2.rngtypid)
		elog(ERROR, "range types do not match");

	/* An empty range is neither before nor after any other range */
	if (empty1 || empty2)
		PG_RETURN_BOOL(false);

	if (range_cmp_bounds(fcinfo, &upper1, &upper2) <= 0)
		PG_RETURN_BOOL(true);

	PG_RETURN_BOOL(false);
}

Datum
range_overright(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE(0);
	RangeType  *r2 = PG_GETARG_RANGE(1);
	RangeBound	lower1,
				lower2;
	RangeBound	upper1,
				upper2;
	bool		empty1,
				empty2;

	range_deserialize(fcinfo, r1, &lower1, &upper1, &empty1);
	range_deserialize(fcinfo, r2, &lower2, &upper2, &empty2);

	if (lower1.rngtypid != upper1.rngtypid ||
		lower1.rngtypid != lower2.rngtypid ||
		lower1.rngtypid != upper2.rngtypid)
		elog(ERROR, "range types do not match");

	/* An empty range is neither before nor after any other range */
	if (empty1 || empty2)
		PG_RETURN_BOOL(false);

	if (range_cmp_bounds(fcinfo, &lower1, &lower2) >= 0)
		PG_RETURN_BOOL(true);

	PG_RETURN_BOOL(false);
}


/* range, range -> range */
Datum
range_minus(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE(0);
	RangeType  *r2 = PG_GETARG_RANGE(1);
	RangeBound	lower1,
				lower2;
	RangeBound	upper1,
				upper2;
	bool		empty1,
				empty2;
	int			cmp_l1l2,
				cmp_l1u2,
				cmp_u1l2,
				cmp_u1u2;

	range_deserialize(fcinfo, r1, &lower1, &upper1, &empty1);
	range_deserialize(fcinfo, r2, &lower2, &upper2, &empty2);

	if (lower1.rngtypid != upper1.rngtypid ||
		lower1.rngtypid != lower2.rngtypid ||
		lower1.rngtypid != upper2.rngtypid)
		elog(ERROR, "range types do not match");

	/* if either is empty, r1 is the correct answer */
	if (empty1 || empty2)
		PG_RETURN_RANGE(r1);

	cmp_l1l2 = range_cmp_bounds(fcinfo, &lower1, &lower2);
	cmp_l1u2 = range_cmp_bounds(fcinfo, &lower1, &upper2);
	cmp_u1l2 = range_cmp_bounds(fcinfo, &upper1, &lower2);
	cmp_u1u2 = range_cmp_bounds(fcinfo, &upper1, &upper2);

	if (cmp_l1l2 < 0 && cmp_u1u2 > 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("result of range difference would not be contiguous")));

	if (cmp_l1u2 > 0 || cmp_u1l2 < 0)
		PG_RETURN_RANGE(r1);

	if (cmp_l1l2 >= 0 && cmp_u1u2 <= 0)
		PG_RETURN_RANGE(make_empty_range(fcinfo, lower1.rngtypid));

	if (cmp_l1l2 <= 0 && cmp_u1l2 >= 0 && cmp_u1u2 <= 0)
	{
		lower2.inclusive = !lower2.inclusive;
		lower2.lower = false;	/* it will become the upper bound */
		PG_RETURN_RANGE(make_range(fcinfo, &lower1, &lower2, false));
	}

	if (cmp_l1l2 >= 0 && cmp_u1u2 >= 0 && cmp_l1u2 <= 0)
	{
		upper2.inclusive = !upper2.inclusive;
		upper2.lower = true;	/* it will become the lower bound */
		PG_RETURN_RANGE(make_range(fcinfo, &upper2, &upper1, false));
	}

	elog(ERROR, "unexpected case in range_minus");
	PG_RETURN_NULL();
}

Datum
range_union(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE(0);
	RangeType  *r2 = PG_GETARG_RANGE(1);
	RangeBound	lower1,
				lower2;
	RangeBound	upper1,
				upper2;
	bool		empty1,
				empty2;
	RangeBound *result_lower;
	RangeBound *result_upper;

	range_deserialize(fcinfo, r1, &lower1, &upper1, &empty1);
	range_deserialize(fcinfo, r2, &lower2, &upper2, &empty2);

	/* if either is empty, the other is the correct answer */
	if (empty1)
		PG_RETURN_RANGE(r2);
	if (empty2)
		PG_RETURN_RANGE(r1);

	if (!DatumGetBool(range_overlaps(fcinfo)) &&
		!DatumGetBool(range_adjacent(fcinfo)))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("result of range union would not be contiguous")));

	if (range_cmp_bounds(fcinfo, &lower1, &lower2) < 0)
		result_lower = &lower1;
	else
		result_lower = &lower2;

	if (range_cmp_bounds(fcinfo, &upper1, &upper2) > 0)
		result_upper = &upper1;
	else
		result_upper = &upper2;

	PG_RETURN_RANGE(make_range(fcinfo, result_lower, result_upper, false));
}

Datum
range_intersect(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE(0);
	RangeType  *r2 = PG_GETARG_RANGE(1);
	RangeBound	lower1,
				lower2;
	RangeBound	upper1,
				upper2;
	bool		empty1,
				empty2;
	RangeBound *result_lower;
	RangeBound *result_upper;

	range_deserialize(fcinfo, r1, &lower1, &upper1, &empty1);
	range_deserialize(fcinfo, r2, &lower2, &upper2, &empty2);

	if (empty1 || empty2 || !DatumGetBool(range_overlaps(fcinfo)))
		PG_RETURN_RANGE(make_empty_range(fcinfo, lower1.rngtypid));

	if (range_cmp_bounds(fcinfo, &lower1, &lower2) >= 0)
		result_lower = &lower1;
	else
		result_lower = &lower2;

	if (range_cmp_bounds(fcinfo, &upper1, &upper2) <= 0)
		result_upper = &upper1;
	else
		result_upper = &upper2;

	PG_RETURN_RANGE(make_range(fcinfo, result_lower, result_upper, false));
}

/* Btree support */

Datum
range_cmp(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE(0);
	RangeType  *r2 = PG_GETARG_RANGE(1);
	RangeBound	lower1,
				lower2;
	RangeBound	upper1,
				upper2;
	bool		empty1,
				empty2;
	int			cmp;

	range_deserialize(fcinfo, r1, &lower1, &upper1, &empty1);
	range_deserialize(fcinfo, r2, &lower2, &upper2, &empty2);

	if (lower1.rngtypid != upper1.rngtypid ||
		lower1.rngtypid != lower2.rngtypid ||
		lower1.rngtypid != upper2.rngtypid)
		elog(ERROR, "range types do not match");

	/* For b-tree use, empty ranges sort before all else */
	if (empty1 && empty2)
		PG_RETURN_INT32(0);
	else if (empty1)
		PG_RETURN_INT32(-1);
	else if (empty2)
		PG_RETURN_INT32(1);

	if ((cmp = range_cmp_bounds(fcinfo, &lower1, &lower2)) != 0)
		PG_RETURN_INT32(cmp);

	PG_RETURN_INT32(range_cmp_bounds(fcinfo, &upper1, &upper2));
}

Datum
range_lt(PG_FUNCTION_ARGS)
{
	int			cmp = range_cmp(fcinfo);

	PG_RETURN_BOOL(cmp < 0);
}

Datum
range_le(PG_FUNCTION_ARGS)
{
	int			cmp = range_cmp(fcinfo);

	PG_RETURN_BOOL(cmp <= 0);
}

Datum
range_ge(PG_FUNCTION_ARGS)
{
	int			cmp = range_cmp(fcinfo);

	PG_RETURN_BOOL(cmp >= 0);
}

Datum
range_gt(PG_FUNCTION_ARGS)
{
	int			cmp = range_cmp(fcinfo);

	PG_RETURN_BOOL(cmp > 0);
}

/* Hash support */

Datum
hash_range(PG_FUNCTION_ARGS)
{
	RangeType  *r = PG_GETARG_RANGE(0);
	RangeBound	lower;
	RangeBound	upper;
	bool		empty;
	char		flags = 0;
	uint32		lower_hash = 0;
	uint32		upper_hash = 0;
	uint32		result = 0;
	RangeTypeInfo rngtypinfo;
	TypeCacheEntry *typentry;
	Oid			subtype;
	FunctionCallInfoData locfcinfo;

	range_deserialize(fcinfo, r, &lower, &upper, &empty);

	if (lower.rngtypid != upper.rngtypid)
		elog(ERROR, "range types do not match");

	if (empty)
		flags |= RANGE_EMPTY;

	flags |= (lower.inclusive) ? RANGE_LB_INC : 0;
	flags |= (lower.infinite) ? RANGE_LB_INF : 0;
	flags |= (upper.inclusive) ? RANGE_UB_INC : 0;
	flags |= (upper.infinite) ? RANGE_UB_INF : 0;

	range_gettypinfo(fcinfo, lower.rngtypid, &rngtypinfo);
	subtype = rngtypinfo.subtype;

	/*
	 * We arrange to look up the hash function only once per series of calls,
	 * assuming the subtype doesn't change underneath us.  The typcache is
	 * used so that we have no memory leakage when being used as an index
	 * support function.
	 */
	typentry = (TypeCacheEntry *) fcinfo->flinfo->fn_extra;
	if (typentry == NULL || typentry->type_id != subtype)
	{
		typentry = lookup_type_cache(subtype, TYPECACHE_HASH_PROC_FINFO);
		if (!OidIsValid(typentry->hash_proc_finfo.fn_oid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("could not identify a hash function for type %s",
							format_type_be(subtype))));
		fcinfo->flinfo->fn_extra = (void *) typentry;
	}

	/*
	 * Apply the hash function to each bound (the hash function shouldn't care
	 * about the collation).
	 */
	InitFunctionCallInfoData(locfcinfo, &typentry->hash_proc_finfo, 1,
							 InvalidOid, NULL, NULL);

	if (RANGE_HAS_LBOUND(flags))
	{
		locfcinfo.arg[0] = lower.val;
		locfcinfo.argnull[0] = false;
		locfcinfo.isnull = false;
		lower_hash = DatumGetUInt32(FunctionCallInvoke(&locfcinfo));
	}
	if (RANGE_HAS_UBOUND(flags))
	{
		locfcinfo.arg[0] = upper.val;
		locfcinfo.argnull[0] = false;
		locfcinfo.isnull = false;
		upper_hash = DatumGetUInt32(FunctionCallInvoke(&locfcinfo));
	}

	result = hash_uint32((uint32) flags);
	result ^= lower_hash;
	result = (result << 1) | (result >> 31);
	result ^= upper_hash;

	PG_RETURN_INT32(result);
}

/*
 *----------------------------------------------------------
 * CANONICAL FUNCTIONS
 *
 *	 Functions for specific built-in range types.
 *----------------------------------------------------------
 */

Datum
int4range_canonical(PG_FUNCTION_ARGS)
{
	RangeType  *r = PG_GETARG_RANGE(0);
	RangeBound	lower;
	RangeBound	upper;
	bool		empty;

	range_deserialize(fcinfo, r, &lower, &upper, &empty);

	if (empty)
		PG_RETURN_RANGE(r);

	if (!lower.infinite && !lower.inclusive)
	{
		lower.val = DirectFunctionCall2(int4pl, lower.val, Int32GetDatum(1));
		lower.inclusive = true;
	}

	if (!upper.infinite && upper.inclusive)
	{
		upper.val = DirectFunctionCall2(int4pl, upper.val, Int32GetDatum(1));
		upper.inclusive = false;
	}

	PG_RETURN_RANGE(range_serialize(fcinfo, &lower, &upper, false));
}

Datum
int8range_canonical(PG_FUNCTION_ARGS)
{
	RangeType  *r = PG_GETARG_RANGE(0);
	RangeBound	lower;
	RangeBound	upper;
	bool		empty;

	range_deserialize(fcinfo, r, &lower, &upper, &empty);

	if (empty)
		PG_RETURN_RANGE(r);

	if (!lower.infinite && !lower.inclusive)
	{
		lower.val = DirectFunctionCall2(int8pl, lower.val, Int64GetDatum(1));
		lower.inclusive = true;
	}

	if (!upper.infinite && upper.inclusive)
	{
		upper.val = DirectFunctionCall2(int8pl, upper.val, Int64GetDatum(1));
		upper.inclusive = false;
	}

	PG_RETURN_RANGE(range_serialize(fcinfo, &lower, &upper, false));
}

Datum
daterange_canonical(PG_FUNCTION_ARGS)
{
	RangeType  *r = PG_GETARG_RANGE(0);
	RangeBound	lower;
	RangeBound	upper;
	bool		empty;

	range_deserialize(fcinfo, r, &lower, &upper, &empty);

	if (empty)
		PG_RETURN_RANGE(r);

	if (!lower.infinite && !lower.inclusive)
	{
		lower.val = DirectFunctionCall2(date_pli, lower.val, Int32GetDatum(1));
		lower.inclusive = true;
	}

	if (!upper.infinite && upper.inclusive)
	{
		upper.val = DirectFunctionCall2(date_pli, upper.val, Int32GetDatum(1));
		upper.inclusive = false;
	}

	PG_RETURN_RANGE(range_serialize(fcinfo, &lower, &upper, false));
}

/*
 *----------------------------------------------------------
 * SUBTYPE_DIFF FUNCTIONS
 *
 *	 Functions for specific built-in range types.
 *----------------------------------------------------------
 */

Datum
int4range_subdiff(PG_FUNCTION_ARGS)
{
	int32		v1 = PG_GETARG_INT32(0);
	int32		v2 = PG_GETARG_INT32(1);

	PG_RETURN_FLOAT8((float8) v1 - (float8) v2);
}

Datum
int8range_subdiff(PG_FUNCTION_ARGS)
{
	int64		v1 = PG_GETARG_INT64(0);
	int64		v2 = PG_GETARG_INT64(1);

	PG_RETURN_FLOAT8((float8) v1 - (float8) v2);
}

Datum
numrange_subdiff(PG_FUNCTION_ARGS)
{
	Datum		v1 = PG_GETARG_DATUM(0);
	Datum		v2 = PG_GETARG_DATUM(1);
	Datum		numresult;
	float8		floatresult;

	numresult = DirectFunctionCall2(numeric_sub, v1, v2);

	floatresult = DatumGetFloat8(DirectFunctionCall1(numeric_float8,
													 numresult));

	PG_RETURN_FLOAT8(floatresult);
}

Datum
daterange_subdiff(PG_FUNCTION_ARGS)
{
	int32		v1 = PG_GETARG_INT32(0);
	int32		v2 = PG_GETARG_INT32(1);

	PG_RETURN_FLOAT8((float8) v1 - (float8) v2);
}

Datum
tsrange_subdiff(PG_FUNCTION_ARGS)
{
	Timestamp	v1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	v2 = PG_GETARG_TIMESTAMP(1);
	float8		result;

#ifdef HAVE_INT64_TIMESTAMP
	result = ((float8) v1 - (float8) v2) / USECS_PER_SEC;
#else
	result = v1 - v2;
#endif

	PG_RETURN_FLOAT8(result);
}

Datum
tstzrange_subdiff(PG_FUNCTION_ARGS)
{
	Timestamp	v1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	v2 = PG_GETARG_TIMESTAMP(1);
	float8		result;

#ifdef HAVE_INT64_TIMESTAMP
	result = ((float8) v1 - (float8) v2) / USECS_PER_SEC;
#else
	result = v1 - v2;
#endif

	PG_RETURN_FLOAT8(result);
}

/*
 *----------------------------------------------------------
 * SUPPORT FUNCTIONS
 *
 *	 These functions aren't in pg_proc, but are useful for
 *	 defining new generic range functions in C.
 *----------------------------------------------------------
 */

/*
 * Serialized format is:
 *
 *	4 bytes: Range type Oid
 *	Lower boundary, if any, aligned according to subtype's typalign
 *	Upper boundary, if any, aligned according to subtype's typalign
 *	1 byte for flags
 *
 * This representation is chosen to be compact when the boundary
 * values need to be MAXALIGNed. A palloc chunk always starts out
 * MAXALIGNed, and the first 4 bytes will be the length header (range
 * types are always variable-length), then the next 4 bytes will be
 * the range type Oid. That leaves the first boundary item MAXALIGNed
 * without the need for padding.
 *
 * However, it requires a slightly odd deserialization strategy,
 * because we have to read the flags byte before we know whether to
 * read a boundary value.
 */

/*
 * This serializes a range, but does not canonicalize it. This should
 * only be called by a canonicalization function.
 */
Datum
range_serialize(FunctionCallInfo fcinfo, RangeBound *lower, RangeBound *upper,
				bool empty)
{
	Datum		range;
	size_t		msize;
	Pointer		ptr;
	int16		typlen;
	char		typalign;
	bool		typbyval;
	char		typstorage;
	char		flags = 0;
	RangeTypeInfo rngtypinfo;

	if (lower->rngtypid != upper->rngtypid)
		elog(ERROR, "range types do not match");

	range_gettypinfo(fcinfo, lower->rngtypid, &rngtypinfo);

	typlen = rngtypinfo.subtyplen;
	typalign = rngtypinfo.subtypalign;
	typbyval = rngtypinfo.subtypbyval;
	typstorage = rngtypinfo.subtypstorage;

	if (empty)
		flags |= RANGE_EMPTY;
	else if (range_cmp_bounds(fcinfo, lower, upper) > 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("range lower bound must be less than or equal to range upper bound")));

	flags |= (lower->inclusive) ? RANGE_LB_INC : 0;
	flags |= (lower->infinite) ? RANGE_LB_INF : 0;
	flags |= (upper->inclusive) ? RANGE_UB_INC : 0;
	flags |= (upper->infinite) ? RANGE_UB_INF : 0;

	msize = VARHDRSZ;
	msize += sizeof(Oid);

	if (RANGE_HAS_LBOUND(flags))
	{
		msize = datum_compute_size(msize, lower->val, typbyval, typalign,
								   typlen, typstorage);
	}

	if (RANGE_HAS_UBOUND(flags))
	{
		msize = datum_compute_size(msize, upper->val, typbyval, typalign,
								   typlen, typstorage);
	}

	msize += sizeof(char);

	ptr = palloc0(msize);
	range = (Datum) ptr;

	ptr += VARHDRSZ;

	memcpy(ptr, &lower->rngtypid, sizeof(Oid));
	ptr += sizeof(Oid);

	if (RANGE_HAS_LBOUND(flags))
	{
		Assert(lower->lower);
		ptr = datum_write(ptr, lower->val, typbyval, typalign, typlen,
						  typstorage);
	}

	if (RANGE_HAS_UBOUND(flags))
	{
		Assert(!upper->lower);
		ptr = datum_write(ptr, upper->val, typbyval, typalign, typlen,
						  typstorage);
	}

	memcpy(ptr, &flags, sizeof(char));
	ptr += sizeof(char);

	SET_VARSIZE(range, msize);
	PG_RETURN_RANGE(range);
}

void
range_deserialize(FunctionCallInfo fcinfo, RangeType *range, RangeBound *lower,
				  RangeBound *upper, bool *empty)
{
	Pointer		ptr = VARDATA(range);
	char		typalign;
	int16		typlen;
	int16		typbyval;
	char		flags;
	Oid			rngtypid;
	Datum		lbound;
	Datum		ubound;
	Pointer		flags_ptr;
	RangeTypeInfo rngtypinfo;

	memset(lower, 0, sizeof(RangeBound));
	memset(upper, 0, sizeof(RangeBound));

	/* peek at last byte to read the flag byte */
	flags_ptr = ptr + VARSIZE(range) - VARHDRSZ - 1;
	memcpy(&flags, flags_ptr, sizeof(char));

	memcpy(&rngtypid, ptr, sizeof(Oid));
	ptr += sizeof(Oid);

	if (rngtypid == ANYRANGEOID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot output a value of type anyrange")));

	range_gettypinfo(fcinfo, rngtypid, &rngtypinfo);

	typalign = rngtypinfo.subtypalign;
	typlen = rngtypinfo.subtyplen;
	typbyval = rngtypinfo.subtypbyval;

	if (RANGE_HAS_LBOUND(flags))
	{
		ptr = (Pointer) att_align_pointer(ptr, typalign, typlen, ptr);
		lbound = fetch_att(ptr, typbyval, typlen);
		ptr = (Pointer) att_addlength_datum(ptr, typlen, PointerGetDatum(ptr));
		if (typlen == -1)
			lbound = PointerGetDatum(PG_DETOAST_DATUM(lbound));
	}
	else
		lbound = (Datum) 0;

	if (RANGE_HAS_UBOUND(flags))
	{
		ptr = (Pointer) att_align_pointer(ptr, typalign, typlen, ptr);
		ubound = fetch_att(ptr, typbyval, typlen);
		ptr = (Pointer) att_addlength_datum(ptr, typlen, PointerGetDatum(ptr));
		if (typlen == -1)
			ubound = PointerGetDatum(PG_DETOAST_DATUM(ubound));
	}
	else
		ubound = (Datum) 0;

	*empty = flags & RANGE_EMPTY;

	lower->rngtypid = rngtypid;
	lower->val = lbound;
	lower->inclusive = flags & RANGE_LB_INC;
	lower->infinite = flags & RANGE_LB_INF;
	lower->lower = true;

	upper->rngtypid = rngtypid;
	upper->val = ubound;
	upper->inclusive = flags & RANGE_UB_INC;
	upper->infinite = flags & RANGE_UB_INF;
	upper->lower = false;
}

/*
 * This both serializes and canonicalizes (if applicable) the range.
 * This should be used by most callers.
 */
Datum
make_range(FunctionCallInfo fcinfo, RangeBound *lower, RangeBound *upper,
		   bool empty)
{
	Datum		range;
	RangeTypeInfo rngtypinfo;

	range_gettypinfo(fcinfo, lower->rngtypid, &rngtypinfo);

	if (lower->rngtypid != upper->rngtypid)
		elog(ERROR, "range types do not match");

	range = range_serialize(fcinfo, lower, upper, empty);

	if (rngtypinfo.canonicalFn.fn_addr != NULL)
		range = FunctionCall1(&rngtypinfo.canonicalFn, range);

	PG_RETURN_RANGE(range);
}

int
range_cmp_bounds(FunctionCallInfo fcinfo, RangeBound *b1, RangeBound *b2)
{
	int			result;
	RangeTypeInfo rngtypinfo;

	if (b1->infinite && b2->infinite)
	{
		if (b1->lower == b2->lower)
			return 0;
		else
			return (b1->lower) ? -1 : 1;
	}
	else if (b1->infinite && !b2->infinite)
		return (b1->lower) ? -1 : 1;
	else if (!b1->infinite && b2->infinite)
		return (b2->lower) ? 1 : -1;

	range_gettypinfo(fcinfo, b1->rngtypid, &rngtypinfo);

	result = DatumGetInt32(FunctionCall2Coll(&rngtypinfo.cmpFn,
											 rngtypinfo.collation,
											 b1->val, b2->val));

	if (result == 0)
	{
		if (b1->inclusive && !b2->inclusive)
			return (b2->lower) ? -1 : 1;
		else if (!b1->inclusive && b2->inclusive)
			return (b1->lower) ? 1 : -1;
	}

	return result;
}

RangeType *
make_empty_range(FunctionCallInfo fcinfo, Oid rngtypid)
{
	RangeBound	lower;
	RangeBound	upper;

	memset(&lower, 0, sizeof(RangeBound));
	memset(&upper, 0, sizeof(RangeBound));

	lower.rngtypid = rngtypid;
	lower.lower = true;
	upper.rngtypid = rngtypid;
	upper.lower = false;

	return DatumGetRangeType(make_range(fcinfo, &lower, &upper, true));
}

/*
 * Fills in rngtypinfo, from a cached copy if available.
 */
void
range_gettypinfo(FunctionCallInfo fcinfo, Oid rngtypid,
				 RangeTypeInfo *rngtypinfo)
{
	RangeTypeInfo *cached = (RangeTypeInfo *) fcinfo->flinfo->fn_extra;

	if (cached == NULL)
	{
		fcinfo->flinfo->fn_extra = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
													  sizeof(RangeTypeInfo));
		cached = (RangeTypeInfo *) fcinfo->flinfo->fn_extra;
		cached->rngtypid = ~rngtypid;
	}

	if (cached->rngtypid != rngtypid)
	{
		Form_pg_range pg_range;
		Form_pg_opclass pg_opclass;
		Form_pg_type pg_type;
		HeapTuple	tup;
		Oid			subtypeOid;
		Oid			collationOid;
		Oid			canonicalOid;
		Oid			subdiffOid;
		Oid			opclassOid;
		Oid			cmpFnOid;
		Oid			opfamilyOid;
		Oid			opcintype;
		int16		subtyplen;
		char		subtypalign;
		char		subtypstorage;
		bool		subtypbyval;

		/* get information from pg_range */
		tup = SearchSysCache1(RANGETYPE, ObjectIdGetDatum(rngtypid));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for range type %u", rngtypid);

		pg_range = (Form_pg_range) GETSTRUCT(tup);

		subtypeOid = pg_range->rngsubtype;
		collationOid = pg_range->rngcollation;
		canonicalOid = pg_range->rngcanonical;
		opclassOid = pg_range->rngsubopc;
		subdiffOid = pg_range->rngsubdiff;

		ReleaseSysCache(tup);

		/* get information from pg_opclass */
		tup = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclassOid));
		if (!HeapTupleIsValid(tup))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("operator class with OID %u does not exist",
							opclassOid)));

		pg_opclass = (Form_pg_opclass) GETSTRUCT(tup);

		opfamilyOid = pg_opclass->opcfamily;
		opcintype = pg_opclass->opcintype;

		ReleaseSysCache(tup);

		cmpFnOid = get_opfamily_proc(opfamilyOid, opcintype, opcintype,
									 BTORDER_PROC);
		if (!RegProcedureIsValid(cmpFnOid))
			elog(ERROR, "missing support function %d(%u,%u) in opfamily %u",
				 BTORDER_PROC, opcintype, opcintype, opfamilyOid);

		/* get information from pg_type */
		tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(subtypeOid));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for type %u", subtypeOid);

		pg_type = (Form_pg_type) GETSTRUCT(tup);

		subtyplen = pg_type->typlen;
		subtypalign = pg_type->typalign;
		subtypstorage = pg_type->typstorage;
		subtypbyval = pg_type->typbyval;

		ReleaseSysCache(tup);

		/* set up the cache */

		if (OidIsValid(canonicalOid))
			fmgr_info(canonicalOid, &cached->canonicalFn);
		else
			cached->canonicalFn.fn_addr = NULL;

		if (OidIsValid(subdiffOid))
			fmgr_info(subdiffOid, &cached->subdiffFn);
		else
			cached->subdiffFn.fn_addr = NULL;

		fmgr_info(cmpFnOid, &cached->cmpFn);
		cached->subtype = subtypeOid;
		cached->collation = collationOid;
		cached->subtyplen = subtyplen;
		cached->subtypalign = subtypalign;
		cached->subtypstorage = subtypstorage;
		cached->subtypbyval = subtypbyval;
		cached->rngtypid = rngtypid;
	}

	memcpy(rngtypinfo, cached, sizeof(RangeTypeInfo));
}

/*
 *----------------------------------------------------------
 * STATIC FUNCTIONS
 *----------------------------------------------------------
 */

/*
 * Given a string representing the flags for the range type, return the flags
 * represented as a char.
 */
static char
range_parse_flags(const char *flags_str)
{
	char		flags = 0;

	if (flags_str[0] == '\0' ||
		flags_str[1] == '\0' ||
		flags_str[2] != '\0')
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("invalid range bound flags"),
				 errhint("Valid values are '[]', '[)', '(]', and '()'.")));

	switch (flags_str[0])
	{
		case '[':
			flags |= RANGE_LB_INC;
			break;
		case '(':
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("invalid range bound flags"),
				   errhint("Valid values are '[]', '[)', '(]', and '()'.")));
	}

	switch (flags_str[1])
	{
		case ']':
			flags |= RANGE_UB_INC;
			break;
		case ')':
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("invalid range bound flags"),
				   errhint("Valid values are '[]', '[)', '(]', and '()'.")));
	}

	return flags;
}

/*
 * Parse range input, modeled after record_in in rowtypes.c.
 *
 *	<range>   := EMPTY
 *			   | <lb-inc> <string>, <string> <ub-inc>
 *	<lb-inc>  := '[' | '('
 *	<ub-inc>  := ']' | ')'
 *
 * Whitespace before or after <range> is ignored. Whitespace within a <string>
 * is taken literally and becomes the input string for that bound.
 *
 * A <string> of length zero is taken as "infinite" (i.e. no bound); unless it
 * is surrounded by double-quotes, in which case it is the literal empty
 * string.
 *
 * Within a <string>, special characters (such as comma, parenthesis, or
 * brackets) can be enclosed in double-quotes or escaped with backslash. Within
 * double-quotes, a double-quote can be escaped with double-quote or backslash.
 */
static void
range_parse(char *string, char *flags, char **lbound_str,
			char **ubound_str)
{
	char	   *ptr = string;
	bool		infinite;

	*flags = 0;

	/* consume whitespace */
	while (*ptr != '\0' && isspace(*ptr))
		ptr++;

	/* check for empty range */
	if (pg_strncasecmp(ptr, RANGE_EMPTY_LITERAL,
					   strlen(RANGE_EMPTY_LITERAL)) == 0)
	{
		*flags = RANGE_EMPTY;

		ptr += strlen(RANGE_EMPTY_LITERAL);

		/* the rest should be whitespace */
		while (*ptr != '\0' && isspace(*ptr))
			ptr++;

		/* should have consumed everything */
		if (*ptr != '\0')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("malformed range literal: \"%s\"",
							string),
					 errdetail("Unexpected end of input.")));

		return;
	}

	if (*ptr == '[' || *ptr == '(')
	{
		if (*ptr == '[')
			*flags |= RANGE_LB_INC;
		ptr++;
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("malformed range literal: \"%s\"",
						string),
				 errdetail("Missing left parenthesis or bracket.")));
	}

	ptr = range_parse_bound(string, ptr, lbound_str, &infinite);
	if (infinite)
	{
		*flags |= RANGE_LB_INF;
		*flags &= ~RANGE_LB_INC;
	}

	if (*ptr != ',')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("malformed range literal: \"%s\"",
						string),
				 errdetail("Missing upper bound.")));
	ptr++;

	ptr = range_parse_bound(string, ptr, ubound_str, &infinite);

	if (*ptr == ')' || *ptr == ']')
	{
		if (*ptr == ']')
			*flags |= RANGE_UB_INC;
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("malformed range literal: \"%s\"",
						string),
				 errdetail("Too many boundaries.")));
	}

	ptr++;

	if (infinite)
	{
		*flags |= RANGE_UB_INF;
		*flags &= ~RANGE_UB_INC;
	}

	/* consume whitespace */
	while (*ptr != '\0' && isspace(*ptr))
		ptr++;

	if (*ptr != '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("malformed range literal: \"%s\"",
						string),
				 errdetail("Junk after right parenthesis or bracket.")));

	return;
}

static char *
range_parse_bound(char *string, char *ptr, char **bound_str, bool *infinite)
{
	StringInfoData buf;

	/* Check for null: completely empty input means null */
	if (*ptr == ',' || *ptr == ')' || *ptr == ']')
	{
		*bound_str = NULL;
		*infinite = true;
	}
	else
	{
		/* Extract string for this column */
		bool		inquote = false;

		initStringInfo(&buf);
		while (inquote || !(*ptr == ',' || *ptr == ')' || *ptr == ']'))
		{
			char		ch = *ptr++;

			if (ch == '\0')
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("malformed range literal: \"%s\"",
								string),
						 errdetail("Unexpected end of input.")));
			if (ch == '\\')
			{
				if (*ptr == '\0')
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							 errmsg("malformed range literal: \"%s\"",
									string),
							 errdetail("Unexpected end of input.")));
				appendStringInfoChar(&buf, *ptr++);
			}
			else if (ch == '\"')
			{
				if (!inquote)
					inquote = true;
				else if (*ptr == '\"')
				{
					/* doubled quote within quote sequence */
					appendStringInfoChar(&buf, *ptr++);
				}
				else
					inquote = false;
			}
			else
				appendStringInfoChar(&buf, ch);
		}

		*bound_str = buf.data;
		*infinite = false;
	}

	return ptr;
}

static char *
range_deparse(char flags, char *lbound_str, char *ubound_str)
{
	StringInfoData buf;

	initStringInfo(&buf);

	if (flags & RANGE_EMPTY)
		return pstrdup(RANGE_EMPTY_LITERAL);

	appendStringInfoString(&buf, (flags & RANGE_LB_INC) ? "[" : "(");

	if (RANGE_HAS_LBOUND(flags))
		appendStringInfoString(&buf, range_bound_escape(lbound_str));

	appendStringInfoString(&buf, ",");

	if (RANGE_HAS_UBOUND(flags))
		appendStringInfoString(&buf, range_bound_escape(ubound_str));

	appendStringInfoString(&buf, (flags & RANGE_UB_INC) ? "]" : ")");

	return buf.data;
}

static char *
range_bound_escape(char *value)
{
	bool		nq;
	char	   *tmp;
	StringInfoData buf;

	initStringInfo(&buf);

	/* Detect whether we need double quotes for this value */
	nq = (value[0] == '\0');	/* force quotes for empty string */
	for (tmp = value; *tmp; tmp++)
	{
		char		ch = *tmp;

		if (ch == '"' || ch == '\\' ||
			ch == '(' || ch == ')' ||
			ch == '[' || ch == ']' ||
			ch == ',' ||
			isspace((unsigned char) ch))
		{
			nq = true;
			break;
		}
	}

	/* And emit the string */
	if (nq)
		appendStringInfoChar(&buf, '"');
	for (tmp = value; *tmp; tmp++)
	{
		char		ch = *tmp;

		if (ch == '"' || ch == '\\')
			appendStringInfoChar(&buf, ch);
		appendStringInfoChar(&buf, ch);
	}
	if (nq)
		appendStringInfoChar(&buf, '"');

	return buf.data;
}

static bool
range_contains_internal(FunctionCallInfo fcinfo, RangeType *r1, RangeType *r2)
{
	RangeBound	lower1;
	RangeBound	upper1;
	bool		empty1;
	RangeBound	lower2;
	RangeBound	upper2;
	bool		empty2;

	range_deserialize(fcinfo, r1, &lower1, &upper1, &empty1);
	range_deserialize(fcinfo, r2, &lower2, &upper2, &empty2);

	if (lower1.rngtypid != upper1.rngtypid ||
		lower1.rngtypid != lower2.rngtypid ||
		lower1.rngtypid != upper2.rngtypid)
		elog(ERROR, "range types do not match");

	if (empty2)
		return true;
	else if (empty1)
		return false;

	if (range_cmp_bounds(fcinfo, &lower1, &lower2) > 0)
		return false;
	if (range_cmp_bounds(fcinfo, &upper1, &upper2) < 0)
		return false;

	return true;
}

/*
 * datum_compute_size() and datum_write() are modeled after
 * heap_compute_data_size() and heap_fill_tuple().
 */

static Size
datum_compute_size(Size data_length, Datum val, bool typbyval, char typalign,
				   int16 typlen, char typstorage)
{
	if (TYPE_IS_PACKABLE(typlen, typstorage) &&
		VARATT_CAN_MAKE_SHORT(DatumGetPointer(val)))
	{
		/*
		 * we're anticipating converting to a short varlena header, so adjust
		 * length and don't count any alignment
		 */
		data_length += VARATT_CONVERTED_SHORT_SIZE(DatumGetPointer(val));
	}
	else
	{
		data_length = att_align_datum(data_length, typalign, typlen, val);
		data_length = att_addlength_datum(data_length, typlen, val);
	}

	return data_length;
}

/*
 * Modified version of the code in heap_fill_tuple(). Writes the datum to ptr
 * using the correct alignment, and also uses short varlena header if
 * applicable.
 */
static Pointer
datum_write(Pointer ptr, Datum datum, bool typbyval, char typalign,
			int16 typlen, char typstorage)
{
	Size		data_length;

	if (typbyval)
	{
		/* pass-by-value */
		ptr = (char *) att_align_nominal(ptr, typalign);
		store_att_byval(ptr, datum, typlen);
		data_length = typlen;
	}
	else if (typlen == -1)
	{
		/* varlena */
		Pointer		val = DatumGetPointer(datum);

		if (VARATT_IS_EXTERNAL(val))
		{
			/* no alignment, since it's short by definition */
			data_length = VARSIZE_EXTERNAL(val);
			memcpy(ptr, val, data_length);
		}
		else if (VARATT_IS_SHORT(val))
		{
			/* no alignment for short varlenas */
			data_length = VARSIZE_SHORT(val);
			memcpy(ptr, val, data_length);
		}
		else if (TYPE_IS_PACKABLE(typlen, typstorage) &&
				 VARATT_CAN_MAKE_SHORT(val))
		{
			/* convert to short varlena -- no alignment */
			data_length = VARATT_CONVERTED_SHORT_SIZE(val);
			SET_VARSIZE_SHORT(ptr, data_length);
			memcpy(ptr + 1, VARDATA(val), data_length - 1);
		}
		else
		{
			/* full 4-byte header varlena */
			ptr = (char *) att_align_nominal(ptr, typalign);
			data_length = VARSIZE(val);
			memcpy(ptr, val, data_length);
		}
	}
	else if (typlen == -2)
	{
		/* cstring ... never needs alignment */
		Assert(typalign == 'c');
		data_length = strlen(DatumGetCString(datum)) + 1;
		memcpy(ptr, DatumGetPointer(datum), data_length);
	}
	else
	{
		/* fixed-length pass-by-reference */
		ptr = (char *) att_align_nominal(ptr, typalign);
		Assert(typlen > 0);
		data_length = typlen;
		memcpy(ptr, DatumGetPointer(datum), data_length);
	}

	ptr += data_length;

	return ptr;
}
