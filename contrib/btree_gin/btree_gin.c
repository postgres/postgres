/*
 * contrib/btree_gin/btree_gin.c
 */
#include "postgres.h"

#include <limits.h>

#include "access/stratnum.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/float.h"
#include "utils/inet.h"
#include "utils/numeric.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"
#include "varatt.h"

PG_MODULE_MAGIC_EXT(
					.name = "btree_gin",
					.version = PG_VERSION
);

/*
 * Our opclasses use the same strategy numbers as btree (1-5) for same-type
 * comparison operators.  For cross-type comparison operators, the
 * low 4 bits of our strategy numbers are the btree strategy number,
 * and the upper bits are a code for the right-hand-side data type.
 */
#define BTGIN_GET_BTREE_STRATEGY(strat)		((strat) & 0x0F)
#define BTGIN_GET_RHS_TYPE_CODE(strat)		((strat) >> 4)

/* extra data passed from gin_btree_extract_query to gin_btree_compare_prefix */
typedef struct QueryInfo
{
	StrategyNumber strategy;	/* operator strategy number */
	Datum		orig_datum;		/* original query (comparison) datum */
	Datum		entry_datum;	/* datum we reported as the entry value */
	PGFunction	typecmp;		/* appropriate btree comparison function */
} QueryInfo;

typedef Datum (*btree_gin_convert_function) (Datum input);

typedef Datum (*btree_gin_leftmost_function) (void);


/*** GIN support functions shared by all datatypes ***/

static Datum
gin_btree_extract_value(FunctionCallInfo fcinfo, bool is_varlena)
{
	Datum		datum = PG_GETARG_DATUM(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	Datum	   *entries = (Datum *) palloc(sizeof(Datum));

	/* Ensure that values stored in the index are not toasted */
	if (is_varlena)
		datum = PointerGetDatum(PG_DETOAST_DATUM(datum));
	entries[0] = datum;
	*nentries = 1;

	PG_RETURN_POINTER(entries);
}

static Datum
gin_btree_extract_query(FunctionCallInfo fcinfo,
						btree_gin_leftmost_function leftmostvalue,
						const bool *rhs_is_varlena,
						const btree_gin_convert_function *cvt_fns,
						const PGFunction *cmp_fns)
{
	Datum		datum = PG_GETARG_DATUM(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	StrategyNumber strategy = PG_GETARG_UINT16(2);
	bool	  **partialmatch = (bool **) PG_GETARG_POINTER(3);
	Pointer   **extra_data = (Pointer **) PG_GETARG_POINTER(4);
	Datum	   *entries = (Datum *) palloc(sizeof(Datum));
	QueryInfo  *data = (QueryInfo *) palloc(sizeof(QueryInfo));
	bool	   *ptr_partialmatch = (bool *) palloc(sizeof(bool));
	int			btree_strat,
				rhs_code;

	/*
	 * Extract the btree strategy code and the RHS data type code from the
	 * given strategy number.
	 */
	btree_strat = BTGIN_GET_BTREE_STRATEGY(strategy);
	rhs_code = BTGIN_GET_RHS_TYPE_CODE(strategy);

	/*
	 * Detoast the comparison datum.  This isn't necessary for correctness,
	 * but it can save repeat detoastings within the comparison function.
	 */
	if (rhs_is_varlena[rhs_code])
		datum = PointerGetDatum(PG_DETOAST_DATUM(datum));

	/* Prep single comparison key with possible partial-match flag */
	*nentries = 1;
	*partialmatch = ptr_partialmatch;
	*ptr_partialmatch = false;

	/*
	 * For BTGreaterEqualStrategyNumber, BTGreaterStrategyNumber, and
	 * BTEqualStrategyNumber we want to start the index scan at the supplied
	 * query datum, and work forward.  For BTLessStrategyNumber and
	 * BTLessEqualStrategyNumber, we need to start at the leftmost key, and
	 * work forward until the supplied query datum (which we'll send along
	 * inside the QueryInfo structure).  Use partial match rules except for
	 * BTEqualStrategyNumber without a conversion function.  (If there is a
	 * conversion function, comparison to the entry value is not trustworthy.)
	 */
	switch (btree_strat)
	{
		case BTLessStrategyNumber:
		case BTLessEqualStrategyNumber:
			entries[0] = leftmostvalue();
			*ptr_partialmatch = true;
			break;
		case BTGreaterEqualStrategyNumber:
		case BTGreaterStrategyNumber:
			*ptr_partialmatch = true;
			/* FALLTHROUGH */
		case BTEqualStrategyNumber:
			/* If we have a conversion function, apply it */
			if (cvt_fns && cvt_fns[rhs_code])
			{
				entries[0] = (*cvt_fns[rhs_code]) (datum);
				*ptr_partialmatch = true;
			}
			else
				entries[0] = datum;
			break;
		default:
			elog(ERROR, "unrecognized strategy number: %d", strategy);
	}

	/* Fill "extra" data */
	data->strategy = strategy;
	data->orig_datum = datum;
	data->entry_datum = entries[0];
	data->typecmp = cmp_fns[rhs_code];
	*extra_data = (Pointer *) palloc(sizeof(Pointer));
	**extra_data = (Pointer) data;

	PG_RETURN_POINTER(entries);
}

static Datum
gin_btree_compare_prefix(FunctionCallInfo fcinfo)
{
	Datum		partial_key PG_USED_FOR_ASSERTS_ONLY = PG_GETARG_DATUM(0);
	Datum		key = PG_GETARG_DATUM(1);
	QueryInfo  *data = (QueryInfo *) PG_GETARG_POINTER(3);
	int32		res,
				cmp;

	/*
	 * partial_key is only an approximation to the real comparison value,
	 * especially if it's a leftmost value.  We can get an accurate answer by
	 * doing a possibly-cross-type comparison to the real comparison value.
	 * (Note that partial_key and key are of the indexed datatype while
	 * orig_datum is of the query operator's RHS datatype.)
	 *
	 * But just to be sure that things are what we expect, let's assert that
	 * partial_key is indeed what gin_btree_extract_query reported, so that
	 * we'll notice if anyone ever changes the core code in a way that breaks
	 * our assumptions.
	 */
	Assert(partial_key == data->entry_datum);

	cmp = DatumGetInt32(CallerFInfoFunctionCall2(data->typecmp,
												 fcinfo->flinfo,
												 PG_GET_COLLATION(),
												 data->orig_datum,
												 key));

	/*
	 * Convert the comparison result to the correct thing for the search
	 * operator strategy.  When dealing with cross-type comparisons, an
	 * imprecise entry datum could lead GIN to start the scan just before the
	 * first possible match, so we must continue the scan if the current index
	 * entry doesn't satisfy the search condition for >= and > cases.  But if
	 * that happens in an = search we can stop, because an imprecise entry
	 * datum means that the search value is unrepresentable in the indexed
	 * data type, so that there will be no exact matches.
	 */
	switch (BTGIN_GET_BTREE_STRATEGY(data->strategy))
	{
		case BTLessStrategyNumber:
			/* If original datum > indexed one then return match */
			if (cmp > 0)
				res = 0;
			else
				res = 1;		/* end scan */
			break;
		case BTLessEqualStrategyNumber:
			/* If original datum >= indexed one then return match */
			if (cmp >= 0)
				res = 0;
			else
				res = 1;		/* end scan */
			break;
		case BTEqualStrategyNumber:
			/* If original datum = indexed one then return match */
			/* See above about why we can end scan when cmp < 0 */
			if (cmp == 0)
				res = 0;
			else
				res = 1;		/* end scan */
			break;
		case BTGreaterEqualStrategyNumber:
			/* If original datum <= indexed one then return match */
			if (cmp <= 0)
				res = 0;
			else
				res = -1;		/* keep scanning */
			break;
		case BTGreaterStrategyNumber:
			/* If original datum < indexed one then return match */
			if (cmp < 0)
				res = 0;
			else
				res = -1;		/* keep scanning */
			break;
		default:
			elog(ERROR, "unrecognized strategy number: %d",
				 data->strategy);
			res = 0;
	}

	PG_RETURN_INT32(res);
}

PG_FUNCTION_INFO_V1(gin_btree_consistent);
Datum
gin_btree_consistent(PG_FUNCTION_ARGS)
{
	bool	   *recheck = (bool *) PG_GETARG_POINTER(5);

	*recheck = false;
	PG_RETURN_BOOL(true);
}

/*** GIN_SUPPORT macro defines the datatype specific functions ***/

#define GIN_SUPPORT(type, leftmostvalue, is_varlena, cvtfns, cmpfns)		\
PG_FUNCTION_INFO_V1(gin_extract_value_##type);								\
Datum																		\
gin_extract_value_##type(PG_FUNCTION_ARGS)									\
{																			\
	return gin_btree_extract_value(fcinfo, is_varlena[0]);					\
}	\
PG_FUNCTION_INFO_V1(gin_extract_query_##type);								\
Datum																		\
gin_extract_query_##type(PG_FUNCTION_ARGS)									\
{																			\
	return gin_btree_extract_query(fcinfo,									\
								   leftmostvalue, is_varlena,				\
								   cvtfns, cmpfns);							\
}	\
PG_FUNCTION_INFO_V1(gin_compare_prefix_##type);								\
Datum																		\
gin_compare_prefix_##type(PG_FUNCTION_ARGS)									\
{																			\
	return gin_btree_compare_prefix(fcinfo);								\
}


/*** Datatype specifications ***/

/* Function to produce the least possible value of the indexed datatype */
static Datum
leftmostvalue_int2(void)
{
	return Int16GetDatum(SHRT_MIN);
}

/*
 * For cross-type support, we must provide conversion functions that produce
 * a Datum of the indexed datatype, since GIN requires the "entry" datums to
 * be of that type.  If an exact conversion is not possible, produce a value
 * that will lead GIN to find the first index entry that is greater than
 * or equal to the actual comparison value.  (But rounding down is OK, so
 * sometimes we might find an index entry that's just less than the
 * comparison value.)
 *
 * For integer values, it's sufficient to clamp the input to be in-range.
 *
 * Note: for out-of-range input values, we could in theory detect that the
 * search condition matches all or none of the index, and avoid a useless
 * index descent in the latter case.  Such searches are probably rare though,
 * so we don't contort this code enough to do that.
 */
static Datum
cvt_int4_int2(Datum input)
{
	int32		val = DatumGetInt32(input);

	val = Max(val, SHRT_MIN);
	val = Min(val, SHRT_MAX);
	return Int16GetDatum((int16) val);
}

static Datum
cvt_int8_int2(Datum input)
{
	int64		val = DatumGetInt64(input);

	val = Max(val, SHRT_MIN);
	val = Min(val, SHRT_MAX);
	return Int16GetDatum((int16) val);
}

/*
 * RHS-type-is-varlena flags, conversion and comparison function arrays,
 * indexed by high bits of the operator strategy number.  A NULL in the
 * conversion function array indicates that no conversion is needed, which
 * will always be the case for the zero'th entry.  Note that the cross-type
 * comparison functions should be the ones with the indexed datatype second.
 */
static const bool int2_rhs_is_varlena[] =
{false, false, false};

static const btree_gin_convert_function int2_cvt_fns[] =
{NULL, cvt_int4_int2, cvt_int8_int2};

static const PGFunction int2_cmp_fns[] =
{btint2cmp, btint42cmp, btint82cmp};

GIN_SUPPORT(int2, leftmostvalue_int2, int2_rhs_is_varlena, int2_cvt_fns, int2_cmp_fns)

static Datum
leftmostvalue_int4(void)
{
	return Int32GetDatum(INT_MIN);
}

static Datum
cvt_int2_int4(Datum input)
{
	int16		val = DatumGetInt16(input);

	return Int32GetDatum((int32) val);
}

static Datum
cvt_int8_int4(Datum input)
{
	int64		val = DatumGetInt64(input);

	val = Max(val, INT_MIN);
	val = Min(val, INT_MAX);
	return Int32GetDatum((int32) val);
}

static const bool int4_rhs_is_varlena[] =
{false, false, false};

static const btree_gin_convert_function int4_cvt_fns[] =
{NULL, cvt_int2_int4, cvt_int8_int4};

static const PGFunction int4_cmp_fns[] =
{btint4cmp, btint24cmp, btint84cmp};

GIN_SUPPORT(int4, leftmostvalue_int4, int4_rhs_is_varlena, int4_cvt_fns, int4_cmp_fns)

static Datum
leftmostvalue_int8(void)
{
	return Int64GetDatum(PG_INT64_MIN);
}

static Datum
cvt_int2_int8(Datum input)
{
	int16		val = DatumGetInt16(input);

	return Int64GetDatum((int64) val);
}

static Datum
cvt_int4_int8(Datum input)
{
	int32		val = DatumGetInt32(input);

	return Int64GetDatum((int64) val);
}

static const bool int8_rhs_is_varlena[] =
{false, false, false};

static const btree_gin_convert_function int8_cvt_fns[] =
{NULL, cvt_int2_int8, cvt_int4_int8};

static const PGFunction int8_cmp_fns[] =
{btint8cmp, btint28cmp, btint48cmp};

GIN_SUPPORT(int8, leftmostvalue_int8, int8_rhs_is_varlena, int8_cvt_fns, int8_cmp_fns)

static Datum
leftmostvalue_float4(void)
{
	return Float4GetDatum(-get_float4_infinity());
}

static Datum
cvt_float8_float4(Datum input)
{
	float8		val = DatumGetFloat8(input);
	float4		result;

	/*
	 * Assume that ordinary C conversion will produce a usable result.
	 * (Compare dtof(), which raises error conditions that we don't need.)
	 * Note that for inputs that aren't exactly representable as float4, it
	 * doesn't matter whether the conversion rounds up or down.  That might
	 * cause us to scan a few index entries that we'll reject as not matching,
	 * but we won't miss any that should match.
	 */
	result = (float4) val;
	return Float4GetDatum(result);
}

static const bool float4_rhs_is_varlena[] =
{false, false};

static const btree_gin_convert_function float4_cvt_fns[] =
{NULL, cvt_float8_float4};

static const PGFunction float4_cmp_fns[] =
{btfloat4cmp, btfloat84cmp};

GIN_SUPPORT(float4, leftmostvalue_float4, float4_rhs_is_varlena, float4_cvt_fns, float4_cmp_fns)

static Datum
leftmostvalue_float8(void)
{
	return Float8GetDatum(-get_float8_infinity());
}

static Datum
cvt_float4_float8(Datum input)
{
	float4		val = DatumGetFloat4(input);

	return Float8GetDatum((float8) val);
}

static const bool float8_rhs_is_varlena[] =
{false, false};

static const btree_gin_convert_function float8_cvt_fns[] =
{NULL, cvt_float4_float8};

static const PGFunction float8_cmp_fns[] =
{btfloat8cmp, btfloat48cmp};

GIN_SUPPORT(float8, leftmostvalue_float8, float8_rhs_is_varlena, float8_cvt_fns, float8_cmp_fns)

static Datum
leftmostvalue_money(void)
{
	return Int64GetDatum(PG_INT64_MIN);
}

static const bool money_rhs_is_varlena[] =
{false};

static const PGFunction money_cmp_fns[] =
{cash_cmp};

GIN_SUPPORT(money, leftmostvalue_money, money_rhs_is_varlena, NULL, money_cmp_fns)

static Datum
leftmostvalue_oid(void)
{
	return ObjectIdGetDatum(0);
}

static const bool oid_rhs_is_varlena[] =
{false};

static const PGFunction oid_cmp_fns[] =
{btoidcmp};

GIN_SUPPORT(oid, leftmostvalue_oid, oid_rhs_is_varlena, NULL, oid_cmp_fns)

static Datum
leftmostvalue_timestamp(void)
{
	return TimestampGetDatum(DT_NOBEGIN);
}

static Datum
cvt_date_timestamp(Datum input)
{
	DateADT		val = DatumGetDateADT(input);
	Timestamp	result;
	int			overflow;

	result = date2timestamp_opt_overflow(val, &overflow);
	/* We can ignore the overflow result, since result is useful as-is */
	return TimestampGetDatum(result);
}

static Datum
cvt_timestamptz_timestamp(Datum input)
{
	TimestampTz val = DatumGetTimestampTz(input);
	Timestamp	result;
	int			overflow;

	result = timestamptz2timestamp_opt_overflow(val, &overflow);
	/* We can ignore the overflow result, since result is useful as-is */
	return TimestampGetDatum(result);
}

static const bool timestamp_rhs_is_varlena[] =
{false, false, false};

static const btree_gin_convert_function timestamp_cvt_fns[] =
{NULL, cvt_date_timestamp, cvt_timestamptz_timestamp};

static const PGFunction timestamp_cmp_fns[] =
{timestamp_cmp, date_cmp_timestamp, timestamptz_cmp_timestamp};

GIN_SUPPORT(timestamp, leftmostvalue_timestamp, timestamp_rhs_is_varlena, timestamp_cvt_fns, timestamp_cmp_fns)

static Datum
cvt_date_timestamptz(Datum input)
{
	DateADT		val = DatumGetDateADT(input);
	TimestampTz result;
	int			overflow;

	result = date2timestamptz_opt_overflow(val, &overflow);
	/* We can ignore the overflow result, since result is useful as-is */
	return TimestampTzGetDatum(result);
}

static Datum
cvt_timestamp_timestamptz(Datum input)
{
	Timestamp	val = DatumGetTimestamp(input);
	TimestampTz result;
	int			overflow;

	result = timestamp2timestamptz_opt_overflow(val, &overflow);
	/* We can ignore the overflow result, since result is useful as-is */
	return TimestampTzGetDatum(result);
}

static const bool timestamptz_rhs_is_varlena[] =
{false, false, false};

static const btree_gin_convert_function timestamptz_cvt_fns[] =
{NULL, cvt_date_timestamptz, cvt_timestamp_timestamptz};

static const PGFunction timestamptz_cmp_fns[] =
{timestamp_cmp, date_cmp_timestamptz, timestamp_cmp_timestamptz};

GIN_SUPPORT(timestamptz, leftmostvalue_timestamp, timestamptz_rhs_is_varlena, timestamptz_cvt_fns, timestamptz_cmp_fns)

static Datum
leftmostvalue_time(void)
{
	return TimeADTGetDatum(0);
}

static const bool time_rhs_is_varlena[] =
{false};

static const PGFunction time_cmp_fns[] =
{time_cmp};

GIN_SUPPORT(time, leftmostvalue_time, time_rhs_is_varlena, NULL, time_cmp_fns)

static Datum
leftmostvalue_timetz(void)
{
	TimeTzADT  *v = palloc(sizeof(TimeTzADT));

	v->time = 0;
	v->zone = -24 * 3600;		/* XXX is that true? */

	return TimeTzADTPGetDatum(v);
}

static const bool timetz_rhs_is_varlena[] =
{false};

static const PGFunction timetz_cmp_fns[] =
{timetz_cmp};

GIN_SUPPORT(timetz, leftmostvalue_timetz, timetz_rhs_is_varlena, NULL, timetz_cmp_fns)

static Datum
leftmostvalue_date(void)
{
	return DateADTGetDatum(DATEVAL_NOBEGIN);
}

static Datum
cvt_timestamp_date(Datum input)
{
	Timestamp	val = DatumGetTimestamp(input);
	DateADT		result;
	int			overflow;

	result = timestamp2date_opt_overflow(val, &overflow);
	/* We can ignore the overflow result, since result is useful as-is */
	return DateADTGetDatum(result);
}

static Datum
cvt_timestamptz_date(Datum input)
{
	TimestampTz val = DatumGetTimestampTz(input);
	DateADT		result;
	int			overflow;

	result = timestamptz2date_opt_overflow(val, &overflow);
	/* We can ignore the overflow result, since result is useful as-is */
	return DateADTGetDatum(result);
}

static const bool date_rhs_is_varlena[] =
{false, false, false};

static const btree_gin_convert_function date_cvt_fns[] =
{NULL, cvt_timestamp_date, cvt_timestamptz_date};

static const PGFunction date_cmp_fns[] =
{date_cmp, timestamp_cmp_date, timestamptz_cmp_date};

GIN_SUPPORT(date, leftmostvalue_date, date_rhs_is_varlena, date_cvt_fns, date_cmp_fns)

static Datum
leftmostvalue_interval(void)
{
	Interval   *v = palloc(sizeof(Interval));

	INTERVAL_NOBEGIN(v);

	return IntervalPGetDatum(v);
}

static const bool interval_rhs_is_varlena[] =
{false};

static const PGFunction interval_cmp_fns[] =
{interval_cmp};

GIN_SUPPORT(interval, leftmostvalue_interval, interval_rhs_is_varlena, NULL, interval_cmp_fns)

static Datum
leftmostvalue_macaddr(void)
{
	macaddr    *v = palloc0(sizeof(macaddr));

	return MacaddrPGetDatum(v);
}

static const bool macaddr_rhs_is_varlena[] =
{false};

static const PGFunction macaddr_cmp_fns[] =
{macaddr_cmp};

GIN_SUPPORT(macaddr, leftmostvalue_macaddr, macaddr_rhs_is_varlena, NULL, macaddr_cmp_fns)

static Datum
leftmostvalue_macaddr8(void)
{
	macaddr8   *v = palloc0(sizeof(macaddr8));

	return Macaddr8PGetDatum(v);
}

static const bool macaddr8_rhs_is_varlena[] =
{false};

static const PGFunction macaddr8_cmp_fns[] =
{macaddr8_cmp};

GIN_SUPPORT(macaddr8, leftmostvalue_macaddr8, macaddr8_rhs_is_varlena, NULL, macaddr8_cmp_fns)

static Datum
leftmostvalue_inet(void)
{
	return DirectFunctionCall1(inet_in, CStringGetDatum("0.0.0.0/0"));
}

static const bool inet_rhs_is_varlena[] =
{true};

static const PGFunction inet_cmp_fns[] =
{network_cmp};

GIN_SUPPORT(inet, leftmostvalue_inet, inet_rhs_is_varlena, NULL, inet_cmp_fns)

static const bool cidr_rhs_is_varlena[] =
{true};

static const PGFunction cidr_cmp_fns[] =
{network_cmp};

GIN_SUPPORT(cidr, leftmostvalue_inet, cidr_rhs_is_varlena, NULL, cidr_cmp_fns)

static Datum
leftmostvalue_text(void)
{
	return PointerGetDatum(cstring_to_text_with_len("", 0));
}

static Datum
cvt_name_text(Datum input)
{
	Name		val = DatumGetName(input);

	return PointerGetDatum(cstring_to_text(NameStr(*val)));
}

static const bool text_rhs_is_varlena[] =
{true, false};

static const btree_gin_convert_function text_cvt_fns[] =
{NULL, cvt_name_text};

static const PGFunction text_cmp_fns[] =
{bttextcmp, btnametextcmp};

GIN_SUPPORT(text, leftmostvalue_text, text_rhs_is_varlena, text_cvt_fns, text_cmp_fns)

static const bool bpchar_rhs_is_varlena[] =
{true};

static const PGFunction bpchar_cmp_fns[] =
{bpcharcmp};

GIN_SUPPORT(bpchar, leftmostvalue_text, bpchar_rhs_is_varlena, NULL, bpchar_cmp_fns)

static Datum
leftmostvalue_char(void)
{
	return CharGetDatum(0);
}

static const bool char_rhs_is_varlena[] =
{false};

static const PGFunction char_cmp_fns[] =
{btcharcmp};

GIN_SUPPORT(char, leftmostvalue_char, char_rhs_is_varlena, NULL, char_cmp_fns)

static const bool bytea_rhs_is_varlena[] =
{true};

static const PGFunction bytea_cmp_fns[] =
{byteacmp};

GIN_SUPPORT(bytea, leftmostvalue_text, bytea_rhs_is_varlena, NULL, bytea_cmp_fns)

static Datum
leftmostvalue_bit(void)
{
	return DirectFunctionCall3(bit_in,
							   CStringGetDatum(""),
							   ObjectIdGetDatum(0),
							   Int32GetDatum(-1));
}

static const bool bit_rhs_is_varlena[] =
{true};

static const PGFunction bit_cmp_fns[] =
{bitcmp};

GIN_SUPPORT(bit, leftmostvalue_bit, bit_rhs_is_varlena, NULL, bit_cmp_fns)

static Datum
leftmostvalue_varbit(void)
{
	return DirectFunctionCall3(varbit_in,
							   CStringGetDatum(""),
							   ObjectIdGetDatum(0),
							   Int32GetDatum(-1));
}

static const bool varbit_rhs_is_varlena[] =
{true};

static const PGFunction varbit_cmp_fns[] =
{bitcmp};

GIN_SUPPORT(varbit, leftmostvalue_varbit, varbit_rhs_is_varlena, NULL, varbit_cmp_fns)

/*
 * Numeric type hasn't a real left-most value, so we use PointerGetDatum(NULL)
 * (*not* a SQL NULL) to represent that.  We can get away with that because
 * the value returned by our leftmostvalue function will never be stored in
 * the index nor passed to anything except our compare and prefix-comparison
 * functions.  The same trick could be used for other pass-by-reference types.
 */

#define NUMERIC_IS_LEFTMOST(x)	((x) == NULL)

PG_FUNCTION_INFO_V1(gin_numeric_cmp);

Datum
gin_numeric_cmp(PG_FUNCTION_ARGS)
{
	Numeric		a = (Numeric) PG_GETARG_POINTER(0);
	Numeric		b = (Numeric) PG_GETARG_POINTER(1);
	int			res = 0;

	if (NUMERIC_IS_LEFTMOST(a))
	{
		res = (NUMERIC_IS_LEFTMOST(b)) ? 0 : -1;
	}
	else if (NUMERIC_IS_LEFTMOST(b))
	{
		res = 1;
	}
	else
	{
		res = DatumGetInt32(DirectFunctionCall2(numeric_cmp,
												NumericGetDatum(a),
												NumericGetDatum(b)));
	}

	PG_RETURN_INT32(res);
}

static Datum
leftmostvalue_numeric(void)
{
	return PointerGetDatum(NULL);
}

static const bool numeric_rhs_is_varlena[] =
{true};

static const PGFunction numeric_cmp_fns[] =
{gin_numeric_cmp};

GIN_SUPPORT(numeric, leftmostvalue_numeric, numeric_rhs_is_varlena, NULL, numeric_cmp_fns)

/*
 * Use a similar trick to that used for numeric for enums, since we don't
 * actually know the leftmost value of any enum without knowing the concrete
 * type, so we use a dummy leftmost value of InvalidOid.
 *
 * Note that we use CallerFInfoFunctionCall2 here so that enum_cmp
 * gets a valid fn_extra to work with. Unlike most other type comparison
 * routines it needs it, so we can't use DirectFunctionCall2.
 */

#define ENUM_IS_LEFTMOST(x) ((x) == InvalidOid)

PG_FUNCTION_INFO_V1(gin_enum_cmp);

Datum
gin_enum_cmp(PG_FUNCTION_ARGS)
{
	Oid			a = PG_GETARG_OID(0);
	Oid			b = PG_GETARG_OID(1);
	int			res = 0;

	if (ENUM_IS_LEFTMOST(a))
	{
		res = (ENUM_IS_LEFTMOST(b)) ? 0 : -1;
	}
	else if (ENUM_IS_LEFTMOST(b))
	{
		res = 1;
	}
	else
	{
		res = DatumGetInt32(CallerFInfoFunctionCall2(enum_cmp,
													 fcinfo->flinfo,
													 PG_GET_COLLATION(),
													 ObjectIdGetDatum(a),
													 ObjectIdGetDatum(b)));
	}

	PG_RETURN_INT32(res);
}

static Datum
leftmostvalue_enum(void)
{
	return ObjectIdGetDatum(InvalidOid);
}

static const bool enum_rhs_is_varlena[] =
{false};

static const PGFunction enum_cmp_fns[] =
{gin_enum_cmp};

GIN_SUPPORT(anyenum, leftmostvalue_enum, enum_rhs_is_varlena, NULL, enum_cmp_fns)

static Datum
leftmostvalue_uuid(void)
{
	/*
	 * palloc0 will create the UUID with all zeroes:
	 * "00000000-0000-0000-0000-000000000000"
	 */
	pg_uuid_t  *retval = (pg_uuid_t *) palloc0(sizeof(pg_uuid_t));

	return UUIDPGetDatum(retval);
}

static const bool uuid_rhs_is_varlena[] =
{false};

static const PGFunction uuid_cmp_fns[] =
{uuid_cmp};

GIN_SUPPORT(uuid, leftmostvalue_uuid, uuid_rhs_is_varlena, NULL, uuid_cmp_fns)

static Datum
leftmostvalue_name(void)
{
	NameData   *result = (NameData *) palloc0(NAMEDATALEN);

	return NameGetDatum(result);
}

static Datum
cvt_text_name(Datum input)
{
	text	   *val = DatumGetTextPP(input);
	NameData   *result = (NameData *) palloc0(NAMEDATALEN);
	int			len = VARSIZE_ANY_EXHDR(val);

	/*
	 * Truncate oversize input.  We're assuming this will produce a result
	 * considered less than the original.  That could be a bad assumption in
	 * some collations, but fortunately an index on "name" is generally going
	 * to use C collation.
	 */
	if (len >= NAMEDATALEN)
		len = pg_mbcliplen(VARDATA_ANY(val), len, NAMEDATALEN - 1);

	memcpy(NameStr(*result), VARDATA_ANY(val), len);

	return NameGetDatum(result);
}

static const bool name_rhs_is_varlena[] =
{false, true};

static const btree_gin_convert_function name_cvt_fns[] =
{NULL, cvt_text_name};

static const PGFunction name_cmp_fns[] =
{btnamecmp, bttextnamecmp};

GIN_SUPPORT(name, leftmostvalue_name, name_rhs_is_varlena, name_cvt_fns, name_cmp_fns)

static Datum
leftmostvalue_bool(void)
{
	return BoolGetDatum(false);
}

static const bool bool_rhs_is_varlena[] =
{false};

static const PGFunction bool_cmp_fns[] =
{btboolcmp};

GIN_SUPPORT(bool, leftmostvalue_bool, bool_rhs_is_varlena, NULL, bool_cmp_fns)
