/*-------------------------------------------------------------------------
 *
 * pg_ndistinct.c
 *		pg_ndistinct data type support.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/pg_ndistinct.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "lib/stringinfo.h"
#include "statistics/extended_stats_internal.h"
#include "statistics/statistics_format.h"
#include "utils/fmgrprotos.h"


/*
 * pg_ndistinct_in
 *		input routine for type pg_ndistinct
 *
 * pg_ndistinct is real enough to be a table column, but it has no
 * operations of its own, and disallows input (just like pg_node_tree).
 */
Datum
pg_ndistinct_in(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type %s", "pg_ndistinct")));

	PG_RETURN_VOID();			/* keep compiler quiet */
}

/*
 * pg_ndistinct_out
 *		output routine for type pg_ndistinct
 *
 * Produces a human-readable representation of the value.
 */
Datum
pg_ndistinct_out(PG_FUNCTION_ARGS)
{
	bytea	   *data = PG_GETARG_BYTEA_PP(0);
	MVNDistinct *ndist = statext_ndistinct_deserialize(data);
	int			i;
	StringInfoData str;

	initStringInfo(&str);
	appendStringInfoChar(&str, '[');

	for (i = 0; i < ndist->nitems; i++)
	{
		MVNDistinctItem item = ndist->items[i];

		if (i > 0)
			appendStringInfoString(&str, ", ");

		if (item.nattributes <= 0)
			elog(ERROR, "invalid zero-length attribute array in MVNDistinct");

		appendStringInfo(&str, "{\"" PG_NDISTINCT_KEY_ATTRIBUTES "\": [%d",
						 item.attributes[0]);

		for (int j = 1; j < item.nattributes; j++)
			appendStringInfo(&str, ", %d", item.attributes[j]);

		appendStringInfo(&str, "], \"" PG_NDISTINCT_KEY_NDISTINCT "\": %d}",
						 (int) item.ndistinct);
	}

	appendStringInfoChar(&str, ']');

	PG_RETURN_CSTRING(str.data);
}

/*
 * pg_ndistinct_recv
 *		binary input routine for type pg_ndistinct
 */
Datum
pg_ndistinct_recv(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type %s", "pg_ndistinct")));

	PG_RETURN_VOID();			/* keep compiler quiet */
}

/*
 * pg_ndistinct_send
 *		binary output routine for type pg_ndistinct
 *
 * n-distinct is serialized into a bytea value, so let's send that.
 */
Datum
pg_ndistinct_send(PG_FUNCTION_ARGS)
{
	return byteasend(fcinfo);
}
