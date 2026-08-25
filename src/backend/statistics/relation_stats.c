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

static bool relation_statistics_update(FunctionCallInfo fcinfo);
static bool relation_statistics_update_internal(Oid reloid,
												FunctionCallInfo fcinfo);

/*
 * Internal function for modifying statistics for a relation.
 */
static bool
relation_statistics_update(FunctionCallInfo fcinfo)
{
	char	   *nspname;
	char	   *relname;
	Oid			reloid;
	Oid			locked_table = InvalidOid;

	stats_check_required_arg(fcinfo, relarginfo, RELSCHEMA_ARG);
	stats_check_required_arg(fcinfo, relarginfo, RELNAME_ARG);

	nspname = TextDatumGetCString(PG_GETARG_DATUM(RELSCHEMA_ARG));
	relname = TextDatumGetCString(PG_GETARG_DATUM(RELNAME_ARG));

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("Statistics cannot be modified during recovery.")));

	reloid = RangeVarGetRelidExtended(makeRangeVar(nspname, relname, -1),
									  ShareUpdateExclusiveLock, 0,
									  RangeVarCallbackForStats, &locked_table);

	return relation_statistics_update_internal(reloid, fcinfo);
}

/*
 * Workhorse function for relation_statistics_update.
 */
static bool
relation_statistics_update_internal(Oid reloid, FunctionCallInfo fcinfo)
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

	if (!PG_ARGISNULL(RELPAGES_ARG))
	{
		relpages = PG_GETARG_INT32(RELPAGES_ARG);
		update_relpages = true;
	}

	if (!PG_ARGISNULL(RELTUPLES_ARG))
	{
		reltuples = PG_GETARG_FLOAT4(RELTUPLES_ARG);
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

	if (!PG_ARGISNULL(RELALLVISIBLE_ARG))
	{
		relallvisible = PG_GETARG_INT32(RELALLVISIBLE_ARG);
		update_relallvisible = true;
	}

	if (!PG_ARGISNULL(RELALLFROZEN_ARG))
	{
		relallfrozen = PG_GETARG_INT32(RELALLFROZEN_ARG);
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
	LOCAL_FCINFO(newfcinfo, 6);

	InitFunctionCallInfoData(*newfcinfo, NULL, 6, InvalidOid, NULL, NULL);

	newfcinfo->args[0].value = PG_GETARG_DATUM(0);
	newfcinfo->args[0].isnull = PG_ARGISNULL(0);
	newfcinfo->args[1].value = PG_GETARG_DATUM(1);
	newfcinfo->args[1].isnull = PG_ARGISNULL(1);
	newfcinfo->args[2].value = Int32GetDatum(0);
	newfcinfo->args[2].isnull = false;
	newfcinfo->args[3].value = Float4GetDatum(-1.0);
	newfcinfo->args[3].isnull = false;
	newfcinfo->args[4].value = Int32GetDatum(0);
	newfcinfo->args[4].isnull = false;
	newfcinfo->args[5].value = Int32GetDatum(0);
	newfcinfo->args[5].isnull = false;

	relation_statistics_update(newfcinfo);
	PG_RETURN_VOID();
}

Datum
pg_restore_relation_stats(PG_FUNCTION_ARGS)
{
	LOCAL_FCINFO(positional_fcinfo, NUM_RELATION_STATS_ARGS);
	bool		result = true;

	InitFunctionCallInfoData(*positional_fcinfo, NULL,
							 NUM_RELATION_STATS_ARGS,
							 InvalidOid, NULL, NULL);

	if (!stats_fill_fcinfo_from_arg_pairs(fcinfo, positional_fcinfo,
										  relarginfo))
		result = false;

	if (!relation_statistics_update(positional_fcinfo))
		result = false;

	PG_RETURN_BOOL(result);
}

/*
 * Import relation statistics from NullableDatum inputs for all statistical
 * values.
 *
 * For now, the 'version' argument is ignored. In the future it can be used
 * to interpret older statistics properly.
 */
bool
import_relation_statistics(Relation rel,
						   const NullableDatum *version,
						   const NullableDatum *relpages,
						   const NullableDatum *reltuples,
						   const NullableDatum *relallvisible,
						   const NullableDatum *relallfrozen)
{
	LOCAL_FCINFO(newfcinfo, NUM_RELATION_STATS_ARGS);

	Assert(relpages);
	Assert(reltuples);
	Assert(relallvisible);
	Assert(relallfrozen);

	InitFunctionCallInfoData(*newfcinfo, NULL, NUM_RELATION_STATS_ARGS,
							 InvalidOid, NULL, NULL);

	newfcinfo->args[RELSCHEMA_ARG].value =
		CStringGetTextDatum(get_namespace_name(RelationGetNamespace(rel)));
	newfcinfo->args[RELSCHEMA_ARG].isnull = false;
	newfcinfo->args[RELNAME_ARG].value =
		CStringGetTextDatum(RelationGetRelationName(rel));
	newfcinfo->args[RELNAME_ARG].isnull = false;

	newfcinfo->args[RELPAGES_ARG] = *relpages;
	newfcinfo->args[RELTUPLES_ARG] = *reltuples;
	newfcinfo->args[RELALLVISIBLE_ARG] = *relallvisible;
	newfcinfo->args[RELALLFROZEN_ARG] = *relallfrozen;

	return relation_statistics_update_internal(RelationGetRelid(rel),
											   newfcinfo);
}
