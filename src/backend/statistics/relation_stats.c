/*-------------------------------------------------------------------------
 * relation_stats.c
 *
 *	  PostgreSQL relation statistics manipulation
 *
 * Code supporting the direct import of relation statistics, similar to
 * what is done by the ANALYZE command.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *       src/backend/statistics/relation_stats.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <math.h>

#include "access/heapam.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "nodes/makefuncs.h"
#include "statistics/statistics.h"
#include "statistics/stat_utils.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/*
 * Positional argument numbers, names, and types for
 * relation_statistics_update().
 */

enum relation_stats_argnum
{
	RELSCHEMA_ARG = 0,
	RELNAME_ARG,
	RELPAGES_ARG,
	RELTUPLES_ARG,
	RELALLVISIBLE_ARG,
	RELALLFROZEN_ARG,
	NUM_RELATION_STATS_ARGS
};

static struct StatsArgInfo relarginfo[] =
{
	[RELSCHEMA_ARG] = {"schemaname", TEXTOID},
	[RELNAME_ARG] = {"relname", TEXTOID},
	[RELPAGES_ARG] = {"relpages", INT4OID},
	[RELTUPLES_ARG] = {"reltuples", FLOAT4OID},
	[RELALLVISIBLE_ARG] = {"relallvisible", INT4OID},
	[RELALLFROZEN_ARG] = {"relallfrozen", INT4OID},
	[NUM_RELATION_STATS_ARGS] = {0}
};

static bool relation_statistics_update(const NullableDatum *args);
static bool relation_statistics_update_internal(Oid reloid,
												const RelationStatsValues *statvalues);

/*
 * Internal function for modifying statistics for a relation.
 */
static bool
relation_statistics_update(const NullableDatum *args)
{
	char	   *nspname;
	char	   *relname;
	Oid			reloid;
	Oid			locked_table = InvalidOid;
	RelationStatsValues values;

	stats_check_required_arg(args, relarginfo, RELSCHEMA_ARG);
	stats_check_required_arg(args, relarginfo, RELNAME_ARG);

	nspname = TextDatumGetCString(args[RELSCHEMA_ARG].value);
	relname = TextDatumGetCString(args[RELNAME_ARG].value);

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("Statistics cannot be modified during recovery.")));

	reloid = RangeVarGetRelidExtended(makeRangeVar(nspname, relname, -1),
									  ShareUpdateExclusiveLock, 0,
									  RangeVarCallbackForStats, &locked_table);

	/* Collect the values to apply. */
	values.version.value = (Datum) 0;
	values.version.isnull = true;
	values.relpages = args[RELPAGES_ARG];
	values.reltuples = args[RELTUPLES_ARG];
	values.relallvisible = args[RELALLVISIBLE_ARG];
	values.relallfrozen = args[RELALLFROZEN_ARG];

	return relation_statistics_update_internal(reloid, &values);
}

/*
 * Workhorse function for relation_statistics_update.
 */
static bool
relation_statistics_update_internal(Oid reloid,
									const RelationStatsValues *statvalues)
{
	int32		relpages = 0;
	bool		update_relpages = false;
	float4		reltuples = 0;
	bool		update_reltuples = false;
	int32		relallvisible = 0;
	bool		update_relallvisible = false;
	int32		relallfrozen = 0;
	bool		update_relallfrozen = false;
	Relation	crel;
	HeapTuple	ctup;
	Form_pg_class pgcform;
	int			replaces[4] = {0};
	Datum		values[4] = {0};
	bool		nulls[4] = {0};
	int			nreplaces = 0;
	bool		result = true;

	if (!statvalues->relpages.isnull)
	{
		relpages = DatumGetInt32(statvalues->relpages.value);
		update_relpages = true;
	}

	if (!statvalues->reltuples.isnull)
	{
		reltuples = DatumGetFloat4(statvalues->reltuples.value);
		if (isnan(reltuples) || isinf(reltuples))
		{
			ereport(WARNING,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("argument \"%s\" must be a finite value", "reltuples")));
			result = false;
		}
		else if (reltuples < -1.0)
		{
			ereport(WARNING,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("argument \"%s\" must not be less than -1.0", "reltuples")));
			result = false;
		}
		else
			update_reltuples = true;
	}

	if (!statvalues->relallvisible.isnull)
	{
		relallvisible = DatumGetInt32(statvalues->relallvisible.value);
		update_relallvisible = true;
	}

	if (!statvalues->relallfrozen.isnull)
	{
		relallfrozen = DatumGetInt32(statvalues->relallfrozen.value);
		update_relallfrozen = true;
	}

	/*
	 * Take RowExclusiveLock on pg_class, consistent with
	 * vac_update_relstats().
	 */
	crel = table_open(RelationRelationId, RowExclusiveLock);

	ctup = SearchSysCache1(RELOID, ObjectIdGetDatum(reloid));
	if (!HeapTupleIsValid(ctup))
		elog(ERROR, "pg_class entry for relid %u not found", reloid);

	pgcform = (Form_pg_class) GETSTRUCT(ctup);

	if (update_relpages && relpages != pgcform->relpages)
	{
		replaces[nreplaces] = Anum_pg_class_relpages;
		values[nreplaces] = Int32GetDatum(relpages);
		nreplaces++;
	}

	if (update_reltuples && reltuples != pgcform->reltuples)
	{
		replaces[nreplaces] = Anum_pg_class_reltuples;
		values[nreplaces] = Float4GetDatum(reltuples);
		nreplaces++;
	}

	if (update_relallvisible && relallvisible != pgcform->relallvisible)
	{
		replaces[nreplaces] = Anum_pg_class_relallvisible;
		values[nreplaces] = Int32GetDatum(relallvisible);
		nreplaces++;
	}

	if (update_relallfrozen && relallfrozen != pgcform->relallfrozen)
	{
		replaces[nreplaces] = Anum_pg_class_relallfrozen;
		values[nreplaces] = Int32GetDatum(relallfrozen);
		nreplaces++;
	}

	if (nreplaces > 0)
	{
		TupleDesc	tupdesc = RelationGetDescr(crel);
		HeapTuple	newtup;

		newtup = heap_modify_tuple_by_cols(ctup, tupdesc, nreplaces,
										   replaces, values, nulls);
		CatalogTupleUpdate(crel, &newtup->t_self, newtup);
		heap_freetuple(newtup);
	}

	ReleaseSysCache(ctup);

	/* release the lock, consistent with vac_update_relstats() */
	table_close(crel, RowExclusiveLock);

	CommandCounterIncrement();

	return result;
}

/*
 * Clear statistics for a given pg_class entry; that is, set back to initial
 * stats for a newly-created table.
 */
Datum
pg_clear_relation_stats(PG_FUNCTION_ARGS)
{
	NullableDatum args[NUM_RELATION_STATS_ARGS];

	args[RELSCHEMA_ARG].value = PG_GETARG_DATUM(0);
	args[RELSCHEMA_ARG].isnull = PG_ARGISNULL(0);
	args[RELNAME_ARG].value = PG_GETARG_DATUM(1);
	args[RELNAME_ARG].isnull = PG_ARGISNULL(1);
	args[RELPAGES_ARG].value = Int32GetDatum(0);
	args[RELPAGES_ARG].isnull = false;
	args[RELTUPLES_ARG].value = Float4GetDatum(-1.0);
	args[RELTUPLES_ARG].isnull = false;
	args[RELALLVISIBLE_ARG].value = Int32GetDatum(0);
	args[RELALLVISIBLE_ARG].isnull = false;
	args[RELALLFROZEN_ARG].value = Int32GetDatum(0);
	args[RELALLFROZEN_ARG].isnull = false;

	relation_statistics_update(args);
	PG_RETURN_VOID();
}

Datum
pg_restore_relation_stats(PG_FUNCTION_ARGS)
{
	NullableDatum positional_args[NUM_RELATION_STATS_ARGS];
	bool		result = true;

	if (!stats_fill_args_from_arg_pairs(fcinfo, positional_args,
										relarginfo))
		result = false;

	if (!relation_statistics_update(positional_args))
		result = false;

	PG_RETURN_BOOL(result);
}

/*
 * Import relation statistics.
 *
 * See RelationStatsValues for the values to provide.
 */
bool
import_relation_statistics(Relation rel, const RelationStatsValues *statvalues)
{
	Assert(statvalues);

	return relation_statistics_update_internal(RelationGetRelid(rel),
											   statvalues);
}
