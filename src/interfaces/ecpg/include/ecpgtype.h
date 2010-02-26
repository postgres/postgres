/*
 * This file implements a data structure that is built and maintained by the
 * preprocessor.
 *
 * All types that can be handled for host variable declarations has to
 * be handled eventually.
 *
 * $PostgreSQL: pgsql/src/interfaces/ecpg/include/ecpgtype.h,v 1.40 2010/02/26 02:01:31 momjian Exp $
 */

/*
 * Here are all the types that we are to handle. Note that it is the type
 * that is registered and that has nothing whatsoever to do with the storage
 * class.
 *
 * Simple types
 * integers: char, short, int, long (signed and unsigned)
 * floats: float, double
 *
 * Complex types:
 * VARCHAR, VARCHAR2 - Strings with length (maxlen is given in the declaration)
 * Arrays of simple types and of VARCHAR, VARCHAR2 (size given in declaration)
 * Records build of simple types, arrays and other structs.
 *
 * Complicating things:
 * typedefs and struct names!
 *
 * Conclusion:
 * This is a typically recursive definition. A structure of typed list elements
 * would probably work fine:
 */

#ifndef _ECPGTYPE_H
#define _ECPGTYPE_H

#ifdef __cplusplus
extern		"C"
{
#endif

enum ECPGttype
{
	ECPGt_char = 1, ECPGt_unsigned_char, ECPGt_short, ECPGt_unsigned_short,
	ECPGt_int, ECPGt_unsigned_int, ECPGt_long, ECPGt_unsigned_long,
	ECPGt_long_long, ECPGt_unsigned_long_long,
	ECPGt_bool,
	ECPGt_float, ECPGt_double,
	ECPGt_varchar, ECPGt_varchar2,
	ECPGt_numeric,				/* this is a decimal that stores its digits in
								 * a malloced array */
	ECPGt_decimal,				/* this is a decimal that stores its digits in
								 * a fixed array */
	ECPGt_date,
	ECPGt_timestamp,
	ECPGt_interval,
	ECPGt_array,
	ECPGt_struct,
	ECPGt_union,
	ECPGt_descriptor,			/* sql descriptor, no C variable */
	ECPGt_char_variable,
	ECPGt_const,				/* a constant is needed sometimes */
	ECPGt_EOIT,					/* End of insert types. */
	ECPGt_EORT,					/* End of result types. */
	ECPGt_NO_INDICATOR,			/* no indicator */
	ECPGt_string,				/* trimmed (char *) type */
	ECPGt_sqlda					/* C struct descriptor */
};

 /* descriptor items */
enum ECPGdtype
{
	ECPGd_count = 1,
	ECPGd_data,
	ECPGd_di_code,
	ECPGd_di_precision,
	ECPGd_indicator,
	ECPGd_key_member,
	ECPGd_length,
	ECPGd_name,
	ECPGd_nullable,
	ECPGd_octet,
	ECPGd_precision,
	ECPGd_ret_length,
	ECPGd_ret_octet,
	ECPGd_scale,
	ECPGd_type,
	ECPGd_EODT,					/* End of descriptor types. */
	ECPGd_cardinality
};

#define IS_SIMPLE_TYPE(type) (((type) >= ECPGt_char && (type) <= ECPGt_interval) || ((type) == ECPGt_string))

/* we also have to handle different statement types */
enum ECPG_statement_type
{
	ECPGst_normal,
	ECPGst_execute,
	ECPGst_exec_immediate,
	ECPGst_prepnormal
};

#ifdef __cplusplus
}
#endif

#endif   /* _ECPGTYPE_H */
