/*-------------------------------------------------------------------------
 *
 * version.c
 *	 Returns the PostgreSQL version string
 *
 * IDENTIFICATION
 *
 * $Header: /cvsroot/pgsql/src/backend/utils/adt/version.c,v 1.12 2000/07/06 05:48:11 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/builtins.h"


Datum
pgsql_version(PG_FUNCTION_ARGS)
{
	int			n = strlen(PG_VERSION_STR);
	text	   *ret = (text *) palloc(n + VARHDRSZ);

	VARATT_SIZEP(ret) = n + VARHDRSZ;
	memcpy(VARDATA(ret), PG_VERSION_STR, n);

	PG_RETURN_TEXT_P(ret);
}
