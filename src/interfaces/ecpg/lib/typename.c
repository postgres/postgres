#include <stdlib.h>
#include "ecpgtype.h"
#include "ecpglib.h"
#include "extern.h"
#include "sql3types.h"

/*
 * This function is used to generate the correct type names.
 */
const char *
ECPGtype_name(enum ECPGttype typ)
{
	switch (typ)
	{
			case ECPGt_char:
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
		default:
			abort();
	}
	return NULL;
}

unsigned int
ECPGDynamicType(Oid type)
{
	switch (type)
	{
			case 16:return SQL3_BOOLEAN;		/* bool */
		case 21:
			return SQL3_SMALLINT;		/* int2 */
		case 23:
			return SQL3_INTEGER;/* int4 */
		case 25:
			return SQL3_CHARACTER;		/* text */
		case 700:
			return SQL3_REAL;	/* float4 */
		case 701:
			return SQL3_DOUBLE_PRECISION;		/* float8 */
		case 1042:
			return SQL3_CHARACTER;		/* bpchar */
		case 1043:
			return SQL3_CHARACTER_VARYING;		/* varchar */
		case 1082:
			return SQL3_DATE_TIME_TIMESTAMP;	/* date */
		case 1083:
			return SQL3_DATE_TIME_TIMESTAMP;	/* time */
		case 1184:
			return SQL3_DATE_TIME_TIMESTAMP;	/* datetime */
		case 1296:
			return SQL3_DATE_TIME_TIMESTAMP;	/* timestamp */
		case 1700:
			return SQL3_NUMERIC;/* numeric */
		default:
			return -type;
	}
}
