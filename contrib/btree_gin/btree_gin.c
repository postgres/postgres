/*
 * contrib/btree_gin/btree_gin.c
 */
#include "postgres.h"

#include <limits.h>

#include "access/skey.h"
#include "utils/builtins.h"
#include "utils/bytea.h"
#include "utils/cash.h"
#include "utils/date.h"
#include "utils/inet.h"
#include "utils/numeric.h"
#include "utils/timestamp.h"
#include "utils/varbit.h"

PG_MODULE_MAGIC;

typedef struct TypeInfo
{
	bool		is_varlena;
	Datum		(*leftmostvalue) (void);
	Datum		(*typecmp) (FunctionCallInfo);
} TypeInfo;

typedef struct QueryInfo
{
	StrategyNumber strategy;
	Datum		datum;
} QueryInfo;

#define  GIN_EXTRACT_VALUE(type)											\
PG_FUNCTION_INFO_V1(gin_extract_value_##type);								\
Datum																		\
gin_extract_value_##type(PG_FUNCTION_ARGS)									\
{																			\
	Datum		datum = PG_GETARG_DATUM(0);									\
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);					\
	Datum	   *entries = (Datum *) palloc(sizeof(Datum));					\
																			\
	if ( TypeInfo_##type.is_varlena )										\
		datum = PointerGetDatum(PG_DETOAST_DATUM(datum));					\
	entries[0] = datum;														\
	*nentries = 1;															\
																			\
	PG_RETURN_POINTER(entries);												\
}

/*
 * For BTGreaterEqualStrategyNumber, BTGreaterStrategyNumber, and
 * BTEqualStrategyNumber we want to start the index scan at the
 * supplied query datum, and work forward. For BTLessStrategyNumber
 * and BTLessEqualStrategyNumber, we need to start at the leftmost
 * key, and work forward until the supplied query datum (which must be
 * sent along inside the QueryInfo structure).
 */

#define GIN_EXTRACT_QUERY(type)												\
PG_FUNCTION_INFO_V1(gin_extract_query_##type);								\
Datum																		\
gin_extract_query_##type(PG_FUNCTION_ARGS)									\
{																			\
	Datum		datum = PG_GETARG_DATUM(0);									\
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);					\
	StrategyNumber strategy = PG_GETARG_UINT16(2);							\
	bool	  **partialmatch = (bool **) PG_GETARG_POINTER(3);				\
	Pointer   **extra_data = (Pointer **) PG_GETARG_POINTER(4);				\
	Datum	   *entries = (Datum *) palloc(sizeof(Datum));					\
	QueryInfo  *data = (QueryInfo *) palloc(sizeof(QueryInfo));				\
	bool	   *ptr_partialmatch;											\
																			\
	*nentries = 1;															\
	ptr_partialmatch = *partialmatch = (bool *) palloc(sizeof(bool));		\
	*ptr_partialmatch = false;												\
	if ( TypeInfo_##type.is_varlena )										\
		datum = PointerGetDatum(PG_DETOAST_DATUM(datum));					\
	data->strategy = strategy;												\
	data->datum = datum;													\
	*extra_data = (Pointer *) palloc(sizeof(Pointer));						\
	**extra_data = (Pointer) data;											\
																			\
	switch (strategy)														\
	{																		\
		case BTLessStrategyNumber:											\
		case BTLessEqualStrategyNumber:										\
			entries[0] = TypeInfo_##type.leftmostvalue();					\
			*ptr_partialmatch = true;										\
			break;															\
		case BTGreaterEqualStrategyNumber:									\
		case BTGreaterStrategyNumber:										\
			*ptr_partialmatch = true;										\
		case BTEqualStrategyNumber:											\
			entries[0] = datum;												\
			break;															\
		default:															\
			elog(ERROR, "unrecognized strategy number: %d", strategy);		\
	}																		\
																			\
	PG_RETURN_POINTER(entries);												\
}

/*
 * Datum a is a value from extract_query method and for BTLess*
 * strategy it is a left-most value.  So, use original datum from QueryInfo
 * to decide to stop scanning or not.  Datum b is always from index.
 */
#define GIN_COMPARE_PREFIX(type)											\
PG_FUNCTION_INFO_V1(gin_compare_prefix_##type);								\
Datum																		\
gin_compare_prefix_##type(PG_FUNCTION_ARGS)									\
{																			\
	Datum		a = PG_GETARG_DATUM(0);										\
	Datum		b = PG_GETARG_DATUM(1);										\
	QueryInfo  *data = (QueryInfo *) PG_GETARG_POINTER(3);					\
	int32		res,														\
				cmp;														\
																			\
	cmp = DatumGetInt32(DirectFunctionCall2Coll(							\
				TypeInfo_##type.typecmp,									\
				PG_GET_COLLATION(),											\
				(data->strategy == BTLessStrategyNumber ||					\
				 data->strategy == BTLessEqualStrategyNumber)				\
				 ? data->datum : a,											\
				b));														\
																			\
	switch (data->strategy)													\
	{																		\
		case BTLessStrategyNumber:											\
			/* If original datum > indexed one then return match */			\
			if (cmp > 0)													\
				res = 0;													\
			else															\
				res = 1;													\
			break;															\
		case BTLessEqualStrategyNumber:										\
			/* The same except equality */									\
			if (cmp >= 0)													\
				res = 0;													\
			else															\
				res = 1;													\
			break;															\
		case BTEqualStrategyNumber:											\
			if (cmp != 0)													\
				res = 1;													\
			else															\
				res = 0;													\
			break;															\
		case BTGreaterEqualStrategyNumber:									\
			/* If original datum <= indexed one then return match */		\
			if (cmp <= 0)													\
				res = 0;													\
			else															\
				res = 1;													\
			break;															\
		case BTGreaterStrategyNumber:										\
			/* If original datum <= indexed one then return match */		\
			/* If original datum == indexed one then continue scan */		\
			if (cmp < 0)													\
				res = 0;													\
			else if (cmp == 0)												\
				res = -1;													\
			else															\
				res = 1;													\
			break;															\
		default:															\
			elog(ERROR, "unrecognized strategy number: %d",					\
				 data->strategy);											\
			res = 0;														\
	}																		\
																			\
	PG_RETURN_INT32(res);													\
}

#define GIN_SUPPORT(type)			\
	GIN_EXTRACT_VALUE(type)			\
	GIN_EXTRACT_QUERY(type)			\
	GIN_COMPARE_PREFIX(type)


PG_FUNCTION_INFO_V1(gin_btree_consistent);
Datum
gin_btree_consistent(PG_FUNCTION_ARGS)
{
	bool	   *recheck = (bool *) PG_GETARG_POINTER(5);

	*recheck = false;
	PG_RETURN_BOOL(true);
}

static Datum
leftmostvalue_int2(void)
{
	return Int16GetDatum(SHRT_MIN);
}
static TypeInfo TypeInfo_int2 = {false, leftmostvalue_int2, btint2cmp};

GIN_SUPPORT(int2)

static Datum
leftmostvalue_int4(void)
{
	return Int32GetDatum(INT_MIN);
}
static TypeInfo TypeInfo_int4 = {false, leftmostvalue_int4, btint4cmp};

GIN_SUPPORT(int4)

static Datum
leftmostvalue_int8(void)
{
	/*
	 * Use sequence's definition to keep compatibility.
	 */
	return Int64GetDatum(SEQ_MINVALUE);
}
static TypeInfo TypeInfo_int8 = {false, leftmostvalue_int8, btint8cmp};

GIN_SUPPORT(int8)

static Datum
leftmostvalue_float4(void)
{
	return Float4GetDatum(-get_float4_infinity());
}
static TypeInfo TypeInfo_float4 = {false, leftmostvalue_float4, btfloat4cmp};

GIN_SUPPORT(float4)

static Datum
leftmostvalue_float8(void)
{
	return Float8GetDatum(-get_float8_infinity());
}
static TypeInfo TypeInfo_float8 = {false, leftmostvalue_float8, btfloat8cmp};

GIN_SUPPORT(float8)

static Datum
leftmostvalue_money(void)
{
	/*
	 * Use sequence's definition to keep compatibility.
	 */
	return Int64GetDatum(SEQ_MINVALUE);
}
static TypeInfo TypeInfo_money = {false, leftmostvalue_money, cash_cmp};

GIN_SUPPORT(money)

static Datum
leftmostvalue_oid(void)
{
	return ObjectIdGetDatum(0);
}
static TypeInfo TypeInfo_oid = {false, leftmostvalue_oid, btoidcmp};

GIN_SUPPORT(oid)

static Datum
leftmostvalue_timestamp(void)
{
	return TimestampGetDatum(DT_NOBEGIN);
}
static TypeInfo TypeInfo_timestamp = {false, leftmostvalue_timestamp, timestamp_cmp};

GIN_SUPPORT(timestamp)

static TypeInfo TypeInfo_timestamptz = {false, leftmostvalue_timestamp, timestamp_cmp};

GIN_SUPPORT(timestamptz)

static Datum
leftmostvalue_time(void)
{
	return TimeADTGetDatum(0);
}
static TypeInfo TypeInfo_time = {false, leftmostvalue_time, time_cmp};

GIN_SUPPORT(time)

static Datum
leftmostvalue_timetz(void)
{
	TimeTzADT  *v = palloc(sizeof(TimeTzADT));

	v->time = 0;
	v->zone = -24 * 3600;		/* XXX is that true? */

	return TimeTzADTPGetDatum(v);
}
static TypeInfo TypeInfo_timetz = {false, leftmostvalue_timetz, timetz_cmp};

GIN_SUPPORT(timetz)

static Datum
leftmostvalue_date(void)
{
	return DateADTGetDatum(DATEVAL_NOBEGIN);
}
static TypeInfo TypeInfo_date = {false, leftmostvalue_date, date_cmp};

GIN_SUPPORT(date)

static Datum
leftmostvalue_interval(void)
{
	Interval   *v = palloc(sizeof(Interval));

	v->time = DT_NOBEGIN;
	v->day = 0;
	v->month = 0;
	return IntervalPGetDatum(v);
}
static TypeInfo TypeInfo_interval = {false, leftmostvalue_interval, interval_cmp};

GIN_SUPPORT(interval)

static Datum
leftmostvalue_macaddr(void)
{
	macaddr    *v = palloc0(sizeof(macaddr));

	return MacaddrPGetDatum(v);
}
static TypeInfo TypeInfo_macaddr = {false, leftmostvalue_macaddr, macaddr_cmp};

GIN_SUPPORT(macaddr)

static Datum
leftmostvalue_inet(void)
{
	return DirectFunctionCall3(inet_in,
							   CStringGetDatum("0.0.0.0/0"),
							   ObjectIdGetDatum(0),
							   Int32GetDatum(-1));
}
static TypeInfo TypeInfo_inet = {true, leftmostvalue_inet, network_cmp};

GIN_SUPPORT(inet)

static TypeInfo TypeInfo_cidr = {true, leftmostvalue_inet, network_cmp};

GIN_SUPPORT(cidr)

static Datum
leftmostvalue_text(void)
{
	return PointerGetDatum(cstring_to_text_with_len("", 0));
}
static TypeInfo TypeInfo_text = {true, leftmostvalue_text, bttextcmp};

GIN_SUPPORT(text)

static Datum
leftmostvalue_char(void)
{
	return CharGetDatum(SCHAR_MIN);
}
static TypeInfo TypeInfo_char = {false, leftmostvalue_char, btcharcmp};

GIN_SUPPORT(char)

static TypeInfo TypeInfo_bytea = {true, leftmostvalue_text, byteacmp};

GIN_SUPPORT(bytea)

static Datum
leftmostvalue_bit(void)
{
	return DirectFunctionCall3(bit_in,
							   CStringGetDatum(""),
							   ObjectIdGetDatum(0),
							   Int32GetDatum(-1));
}
static TypeInfo TypeInfo_bit = {true, leftmostvalue_bit, bitcmp};

GIN_SUPPORT(bit)

static Datum
leftmostvalue_varbit(void)
{
	return DirectFunctionCall3(varbit_in,
							   CStringGetDatum(""),
							   ObjectIdGetDatum(0),
							   Int32GetDatum(-1));
}
static TypeInfo TypeInfo_varbit = {true, leftmostvalue_varbit, bitcmp};

GIN_SUPPORT(varbit)

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

static TypeInfo TypeInfo_numeric = {true, leftmostvalue_numeric, gin_numeric_cmp};

GIN_SUPPORT(numeric)
