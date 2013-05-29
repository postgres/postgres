/* src/interfaces/ecpg/ecpglib/typename.c */

#define POSTGRES_ECPG_INTERNAL
#include "postgres_fe.h"

#include <stdlib.h>
#include "ecpgtype.h"
#include "ecpglib.h"
#include "extern.h"
#include "sqltypes.h"
#include "sql3types.h"
#include "pg_type.h"

/*
 * This function is used to generate the correct type names.
 */
const char *
ecpg_type_name(enum ECPGttype typ)
{
	switch (typ)
	{
		case ECPGt_char:
		case ECPGt_string:
			return "char";
		case ECPGt_unsigned_char:
			return "unsigned char";
		case ECPGt_short:
			return "short";
		case ECPGt_unsigned_short:
			return "unsigned short";
		case ECPGt_int:
			return "int";
		case ECPGt_unsigned_int:
			return "unsigned int";
		case ECPGt_long:
			return "long";
		case ECPGt_unsigned_long:
			return "unsigned long";
		case ECPGt_long_long:
			return "long long";
		case ECPGt_unsigned_long_long:
			return "unsigned long long";
		case ECPGt_float:
			return "float";
		case ECPGt_double:
			return "double";
		case ECPGt_bool:
			return "bool";
		case ECPGt_varchar:
			return "varchar";
		case ECPGt_char_variable:
			return "char";
		case ECPGt_decimal:
			return "decimal";
		case ECPGt_numeric:
			return "numeric";
		case ECPGt_date:
			return "date";
		case ECPGt_timestamp:
			return "timestamp";
		case ECPGt_interval:
			return "interval";
		case ECPGt_const:
			return "Const";
		default:
			abort();
	}
	return "";					/* keep MSC compiler happy */
}

int
ecpg_dynamic_type(Oid type)
{
	switch (type)
	{
		case BOOLOID:
			return SQL3_BOOLEAN;	/* bool */
		case INT2OID:
			return SQL3_SMALLINT;		/* int2 */
		case INT4OID:
			return SQL3_INTEGER;	/* int4 */
		case TEXTOID:
			return SQL3_CHARACTER;		/* text */
		case FLOAT4OID:
			return SQL3_REAL;	/* float4 */
		case FLOAT8OID:
			return SQL3_DOUBLE_PRECISION;		/* float8 */
		case BPCHAROID:
			return SQL3_CHARACTER;		/* bpchar */
		case VARCHAROID:
			return SQL3_CHARACTER_VARYING;		/* varchar */
		case DATEOID:
			return SQL3_DATE_TIME_TIMESTAMP;	/* date */
		case TIMEOID:
			return SQL3_DATE_TIME_TIMESTAMP;	/* time */
		case TIMESTAMPOID:
			return SQL3_DATE_TIME_TIMESTAMP;	/* datetime */
		case NUMERICOID:
			return SQL3_NUMERIC;	/* numeric */
		default:
			return 0;
	}
}

int
sqlda_dynamic_type(Oid type, enum COMPAT_MODE compat)
{
	switch (type)
	{
		case CHAROID:
		case VARCHAROID:
		case BPCHAROID:
		case TEXTOID:
			return ECPGt_char;
		case INT2OID:
			return ECPGt_short;
		case INT4OID:
			return ECPGt_int;
		case FLOAT8OID:
			return ECPGt_double;
		case FLOAT4OID:
			return ECPGt_float;
		case NUMERICOID:
			return INFORMIX_MODE(compat) ? ECPGt_decimal : ECPGt_numeric;
		case DATEOID:
			return ECPGt_date;
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			return ECPGt_timestamp;
		case INTERVALOID:
			return ECPGt_interval;
		case INT8OID:
#ifdef HAVE_LONG_LONG_INT_64
			return ECPGt_long_long;
#endif
#ifdef HAVE_LONG_INT_64
			return ECPGt_long;
#endif
			/* Unhandled types always return a string */
		default:
			return ECPGt_char;
	}
}
