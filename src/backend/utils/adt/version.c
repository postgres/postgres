/*-------------------------------------------------------------------------
 *
 * version.c
 *	 Returns the PostgreSQL version string
 *
 * Copyright (c) 1998-2009, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *
 * $PostgreSQL: pgsql/src/backend/utils/adt/version.c,v 1.18 2009/01/01 17:23:50 momjian Exp $
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
