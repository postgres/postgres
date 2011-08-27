/*
 * contrib/btree_gist/btree_gist.c
 */
#include "postgres.h"

#include "btree_gist.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(gbt_decompress);
PG_FUNCTION_INFO_V1(gbtreekey_in);
PG_FUNCTION_INFO_V1(gbtreekey_out);

Datum		gbt_decompress(PG_FUNCTION_ARGS);

/**************************************************
 * In/Out for keys
 **************************************************/


Datum
gbtreekey_in(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("<datatype>key_in() not implemented")));

	PG_RETURN_POINTER(NULL);
}

#include "btree_utils_var.h"
#include "utils/builtins.h"
Datum
gbtreekey_out(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("<datatype>key_out() not implemented")));
	PG_RETURN_POINTER(NULL);
}


/*
** GiST DeCompress methods
** do not do anything.
*/
Datum
gbt_decompress(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(PG_GETARG_POINTER(0));
}
