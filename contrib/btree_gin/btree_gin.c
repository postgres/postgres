/*
 * contrib/btree_gin/btree_gin.c
 */
#include "postgres.h"

#include <limits.h>

#include "access/stratnum.h"
#include "utils/builtins.h"
#include "utils/bytea.h"
#include "utils/cash.h"
#include "utils/date.h"
#include "utils/inet.h"
#include "utils/numeric.h"
#include "utils/timestamp.h"
#include "utils/varbit.h"

PG_MODULE_MAGIC;

typedef struct QueryInfo
{
	StrategyNumber strategy;
	Datum		datum;
	bool		is_varlena;
	Datum		(*typecmp) (FunctionCallInfo);
} QueryInfo;


/*** GIN support functions shared by all datatypes ***/

static Datum
gin_btree_extract_value(FunctionCallInfo fcinfo, bool is_varlena)
{
	Datum		datum = PG_GETARG_DATUM(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	Datum	   *entries = (Datum *) palloc(sizeof(Datum));

	if (is_varlena)
		datum = PointerGetDatum(PG_DETOAST_DATUM(datum));
	entries[0] = datum;
	*nentries = 1;

	PG_RETURN_POINTER(entries);
}

/*
 * For BTGreaterEqualStrategyNumber, BTGreaterStrategyNumber, and
 * BTEqualStrategyNumber we want to start the index scan at the
 * supplied query datum, and work forward. For BTLessStrategyNumber
 * and BTLessEqualStrategyNumber, we need to start at the leftmost
 * key, and work forward until the supplied query datum (which must be
 * sent along inside the QueryInfo structure).
 */
static Datum
gin_btree_extract_query(FunctionCallInfo fcinfo,
						bool is_varlena,
						Datum (*leftmostvalue) (void),
						Datum (*typecmp) (FunctionCallInfo))
{
	Datum		datum = PG_GETARG_DATUM(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	StrategyNumber strategy = PG_GETARG_UINT16(2);
	bool	  **partialmatch = (bool **) PG_GETARG_POINTER(3);
	Pointer   **extra_data = (Pointer **) PG_GETARG_POINTER(4);
	Datum	   *entries = (Datum *) palloc(sizeof(Datum));
	QueryInfo  *data = (QueryInfo *) palloc(sizeof(QueryInfo));
	bool	   *ptr_partialmatch;

	*nentries = 1;
	ptr_partialmatch = *partialmatch = (bool *) palloc(sizeof(bool));
	*ptr_partialmatch = false;
	if (is_varlena)
		datum = PointerGetDatum(PG_DETOAST_DATUM(datum));
	data->strategy = strategy;
	data->datum = datum;
	data->is_varlena = is_varlena;
	data->typecmp = typecmp;
	*extra_data = (Pointer *) palloc(sizeof(Pointer));
	**extra_data = (Pointer) data;

	switch (strategy)
	{
		case BTLessStrategyNumber:
		case BTLessEqualStrategyNumber:
			entries[0] = leftmostvalue();
			*ptr_partialmatch = true;
			break;
		case BTGreaterEqualStrategyNumber:
		case BTGreaterStrategyNumber:
			*ptr_partialmatch = true;
		case BTEqualStrategyNumber:
			entries[0] = datum;
			break;
		default:
			elog(ERROR, "unrecognized strategy number: %d", strategy);
	}

	PG_RETURN_POINTER(entries);
}

/*
 * Datum a is a value from extract_query method and for BTLess*
 * strategy it is a left-most value.  So, use original datum from QueryInfo
 * to decide to stop scanning or not.  Datum b is always from index.
 */
static Datum
gin_btree_compare_prefix(FunctionCallInfo fcinfo)
{
	Datum		a = PG_GETARG_DATUM(0);
	Datum		b = PG_GETARG_DATUM(1);
	QueryInfo  *data = (QueryInfo *) PG_GETARG_POINTER(3);
	int32		res,
				cmp;

	cmp = DatumGetInt32(DirectFunctionCall2Coll(
												data->typecmp,
												PG_GET_COLLATION(),
								   (data->strategy == BTLessStrategyNumber ||
								 data->strategy == BTLessEqualStrategyNumber)
												? data->datum : a,
												b));

	switch (data->strategy)
	{
		case BTLessStrategyNumber:
			/* If original datum > indexed one then return match */
			if (cmp > 0)
				res = 0;
			else
				res = 1;
			break;
		case BTLessEqualStrategyNumber:
			/* The same except equality */
			if (cmp >= 0)
				res = 0;
			else
				res = 1;
			break;
		case BTEqualStrategyNumber:
			if (cmp != 0)
				res = 1;
			else
				res = 0;
			break;
		case BTGreaterEqualStrategyNumber:
			/* If original datum <= indexed one then return match */
			if (cmp <= 0)
				res = 0;
			else
				res = 1;
			break;
		case BTGreaterStrategyNumber:
			/* If original datum <= indexed one then return match */
			/* If original datum == indexed one then continue scan */
			if (cmp < 0)
				res = 0;
			else if (cmp == 0)
				res = -1;
			else
				res = 1;
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

#define GIN_SUPPORT(type, is_varlena, leftmostvalue, typecmp)				\
PG_FUNCTION_INFO_V1(gin_extract_value_##type);								\
Datum																		\
gin_extract_value_##type(PG_FUNCTION_ARGS)									\
{																			\
	return gin_btree_extract_value(fcinfo, is_varlena);						\
}	\
PG_FUNCTION_INFO_V1(gin_extract_query_##type);								\
Datum																		\
gin_extract_query_##type(PG_FUNCTION_ARGS)									\
{																			\
	return gin_btree_extract_query(fcinfo,									\
								   is_varlena, leftmostvalue, typecmp);		\
}	\
PG_FUNCTION_INFO_V1(gin_compare_prefix_##type);								\
Datum																		\
gin_compare_prefix_##type(PG_FUNCTION_ARGS)									\
{																			\
	return gin_btree_compare_prefix(fcinfo);								\
}


/*** Datatype specifications ***/

static Datum
leftmostvalue_int2(void)
{
	return Int16GetDatum(SHRT_MIN);
}

GIN_SUPPORT(int2, false, leftmostvalue_int2, btint2cmp)

static Datum
leftmostvalue_int4(void)
{
	return Int32GetDatum(INT_MIN);
}

GIN_SUPPORT(int4, false, leftmostvalue_int4, btint4cmp)

static Datum
leftmostvalue_int8(void)
{
	return Int64GetDatum(PG_INT64_MIN);
}

GIN_SUPPORT(int8, false, leftmostvalue_int8, btint8cmp)

static Datum
leftmostvalue_float4(void)
{
	return Float4GetDatum(-get_float4_infinity());
}

GIN_SUPPORT(float4, false, leftmostvalue_float4, btfloat4cmp)

static Datum
leftmostvalue_float8(void)
{
	return Float8GetDatum(-get_float8_infinity());
}

GIN_SUPPORT(float8, false, leftmostvalue_float8, btfloat8cmp)

static Datum
leftmostvalue_money(void)
{
	return Int64GetDatum(PG_INT64_MIN);
}

GIN_SUPPORT(money, false, leftmostvalue_money, cash_cmp)

static Datum
leftmostvalue_oid(void)
{
	return ObjectIdGetDatum(0);
}

GIN_SUPPORT(oid, false, leftmostvalue_oid, btoidcmp)

static Datum
leftmostvalue_timestamp(void)
{
	return TimestampGetDatum(DT_NOBEGIN);
}

GIN_SUPPORT(timestamp, false, leftmostvalue_timestamp, timestamp_cmp)

GIN_SUPPORT(timestamptz, false, leftmostvalue_timestamp, timestamp_cmp)

static Datum
leftmostvalue_time(void)
{
	return TimeADTGetDatum(0);
}

GIN_SUPPORT(time, false, leftmostvalue_time, time_cmp)

static Datum
leftmostvalue_timetz(void)
{
	TimeTzADT  *v = palloc(sizeof(TimeTzADT));

	v->time = 0;
	v->zone = -24 * 3600;		/* XXX is that true? */

	return TimeTzADTPGetDatum(v);
}

GIN_SUPPORT(timetz, false, leftmostvalue_timetz, timetz_cmp)

static Datum
leftmostvalue_date(void)
{
	return DateADTGetDatum(DATEVAL_NOBEGIN);
}

GIN_SUPPORT(date, false, leftmostvalue_date, date_cmp)

static Datum
leftmostvalue_interval(void)
{
	Interval   *v = palloc(sizeof(Interval));

	v->time = DT_NOBEGIN;
	v->day = 0;
	v->month = 0;
	return IntervalPGetDatum(v);
}

GIN_SUPPORT(interval, false, leftmostvalue_interval, interval_cmp)

static Datum
leftmostvalue_macaddr(void)
{
	macaddr    *v = palloc0(sizeof(macaddr));

	return MacaddrPGetDatum(v);
}

GIN_SUPPORT(macaddr, false, leftmostvalue_macaddr, macaddr_cmp)

static Datum
leftmostvalue_inet(void)
{
	return DirectFunctionCall1(inet_in, CStringGetDatum("0.0.0.0/0"));
}

GIN_SUPPORT(inet, true, leftmostvalue_inet, network_cmp)

GIN_SUPPORT(cidr, true, leftmostvalue_inet, network_cmp)

static Datum
leftmostvalue_text(void)
{
	return PointerGetDatum(cstring_to_text_with_len("", 0));
}

GIN_SUPPORT(text, true, leftmostvalue_text, bttextcmp)

static Datum
leftmostvalue_char(void)
{
	return CharGetDatum(SCHAR_MIN);
}

GIN_SUPPORT(char, false, leftmostvalue_char, btcharcmp)

GIN_SUPPORT(bytea, true, leftmostvalue_text, byteacmp)

static Datum
leftmostvalue_bit(void)
{
	return DirectFunctionCall3(bit_in,
							   CStringGetDatum(""),
							   ObjectIdGetDatum(0),
							   Int32GetDatum(-1));
}

GIN_SUPPORT(bit, true, leftmostvalue_bit, bitcmp)

static Datum
leftmostvalue_varbit(void)
{
	return DirectFunctionCall3(varbit_in,
							   CStringGetDatum(""),
							   ObjectIdGetDatum(0),
							   Int32GetDatum(-1));
}

GIN_SUPPORT(varbit, true, leftmostvalue_varbit, bitcmp)

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

GIN_SUPPORT(numeric, true, leftmostvalue_numeric, gin_numeric_cmp)
