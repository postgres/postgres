#include <stdlib.h>
#include <ecpgtype.h>
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
