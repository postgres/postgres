/*-------------------------------------------------------------------------
 *
 * version.c
 *	 Returns the PostgreSQL version string
 *
 * Copyright (c) 1998-2008, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *
 * $PostgreSQL: pgsql/src/backend/utils/adt/version.c,v 1.16 2008/01/01 19:45:53 momjian Exp $
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

	SET_VARSIZE(ret, n + VARHDRSZ);
	memcpy(VARDATA(ret), PG_VERSION_STR, n);

	PG_RETURN_TEXT_P(ret);
}
