/*-------------------------------------------------------------------------
 *
 * pg_dependencies.c
 *		pg_dependencies data type support.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/pg_dependencies.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "lib/stringinfo.h"
#include "statistics/extended_stats_internal.h"
#include "statistics/statistics_format.h"
#include "utils/fmgrprotos.h"

/*
 * pg_dependencies_in		- input routine for type pg_dependencies.
 *
 * pg_dependencies is real enough to be a table column, but it has no operations
 * of its own, and disallows input too
 */
Datum
pg_dependencies_in(PG_FUNCTION_ARGS)
{
	/*
	 * pg_node_list stores the data in binary form and parsing text input is
	 * not needed, so disallow this.
	 */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type %s", "pg_dependencies")));

	PG_RETURN_VOID();			/* keep compiler quiet */
}

/*
 * pg_dependencies_out		- output routine for type pg_dependencies.
 */
Datum
pg_dependencies_out(PG_FUNCTION_ARGS)
{
	bytea	   *data = PG_GETARG_BYTEA_PP(0);
	MVDependencies *dependencies = statext_dependencies_deserialize(data);
	StringInfoData str;

	initStringInfo(&str);
	appendStringInfoChar(&str, '[');

	for (int i = 0; i < dependencies->ndeps; i++)
	{
		MVDependency *dependency = dependencies->deps[i];

		if (i > 0)
			appendStringInfoString(&str, ", ");

		if (dependency->nattributes <= 1)
			elog(ERROR, "invalid zero-length nattributes array in MVDependencies");

		appendStringInfo(&str, "{\"" PG_DEPENDENCIES_KEY_ATTRIBUTES "\": [%d",
						 dependency->attributes[0]);

		for (int j = 1; j < dependency->nattributes - 1; j++)
			appendStringInfo(&str, ", %d", dependency->attributes[j]);

		appendStringInfo(&str, "], \"" PG_DEPENDENCIES_KEY_DEPENDENCY "\": %d, "
						 "\"" PG_DEPENDENCIES_KEY_DEGREE "\": %f}",
						 dependency->attributes[dependency->nattributes - 1],
						 dependency->degree);
	}

	appendStringInfoChar(&str, ']');

	PG_RETURN_CSTRING(str.data);
}

/*
 * pg_dependencies_recv		- binary input routine for type pg_dependencies.
 */
Datum
pg_dependencies_recv(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type %s", "pg_dependencies")));

	PG_RETURN_VOID();			/* keep compiler quiet */
}

/*
 * pg_dependencies_send		- binary output routine for type pg_dependencies.
 *
 * Functional dependencies are serialized in a bytea value (although the type
 * is named differently), so let's just send that.
 */
Datum
pg_dependencies_send(PG_FUNCTION_ARGS)
{
	return byteasend(fcinfo);
}
