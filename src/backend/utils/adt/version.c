/*-------------------------------------------------------------------------
 *
 * version.c
 *	 Returns the PostgreSQL version string
 *
 * Copyright (c) 1998-2008, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *
 * $PostgreSQL: pgsql/src/backend/utils/adt/version.c,v 1.17 2008/03/25 22:42:44 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/builtins.h"


Datum
pgsql_version(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(PG_VERSION_STR));
}
