/*
 * This file implements a data structure that is built and maintained by the
 * preprocessor.
 *
 * All types that can be handled for host variable declarations has to
 * be handled eventually.
 */

/*
 * Here are all the types that we are to handle. Note that it is the type
 * that is registered and that has nothing whatsoever to do with the storage
 * class.
 *
 * Simle types
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
#include <stdio.h>

#ifdef __cplusplus
extern		"C"
{
#endif

	enum ECPGttype
	{
		ECPGt_char = 1, ECPGt_unsigned_char, ECPGt_short, ECPGt_unsigned_short,
		ECPGt_int, ECPGt_unsigned_int, ECPGt_long, ECPGt_unsigned_long,
		ECPGt_bool,
		ECPGt_float, ECPGt_double,
		ECPGt_varchar, ECPGt_varchar2,
		ECPGt_array,
		ECPGt_struct,
		ECPGt_union,
		ECPGt_char_variable,
		ECPGt_EOIT,				/* End of insert types. */
		ECPGt_EORT,				/* End of result types. */
		ECPGt_NO_INDICATOR		/* no indicator */
	};

	/* descriptor items */	
	enum ECPGdtype
	{
		ECPGd_count,
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
		ECPGd_EODT				/* End of descriptor types. */
	};

#define IS_SIMPLE_TYPE(type) ((type) >= ECPGt_char && (type) <= ECPGt_varchar2)

#ifdef __cplusplus
}

#endif
