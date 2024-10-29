/*-------------------------------------------------------------------------
 * relation_stats.c
 *
 *	  PostgreSQL relation statistics manipulation
 *
 * Code supporting the direct import of relation statistics, similar to
 * what is done by the ANALYZE command.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *       src/backend/statistics/relation_stats.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/indexing.h"
#include "statistics/stat_utils.h"
#include "utils/fmgrprotos.h"
#include "utils/syscache.h"

#define DEFAULT_RELPAGES Int32GetDatum(0)
#define DEFAULT_RELTUPLES Float4GetDatum(-1.0)
#define DEFAULT_RELALLVISIBLE Int32GetDatum(0)

/*
 * Positional argument numbers, names, and types for
 * relation_statistics_update().
 */

enum relation_stats_argnum
{
	RELATION_ARG = 0,
	RELPAGES_ARG,
	RELTUPLES_ARG,
	RELALLVISIBLE_ARG,
	NUM_RELATION_STATS_ARGS
};

static struct StatsArgInfo relarginfo[] =
{
	[RELATION_ARG] = {"relation", REGCLASSOID},
	[RELPAGES_ARG] = {"relpages", INT4OID},
	[RELTUPLES_ARG] = {"reltuples", FLOAT4OID},
	[RELALLVISIBLE_ARG] = {"relallvisible", INT4OID},
	[NUM_RELATION_STATS_ARGS] = {0}
};

static bool relation_statistics_update(FunctionCallInfo fcinfo, int elevel);

/*
 * Internal function for modifying statistics for a relation.
 */
static bool
relation_statistics_update(FunctionCallInfo fcinfo, int elevel)
{
	Oid			reloid;
	Relation	crel;
	HeapTuple	ctup;
	Form_pg_class pgcform;
	int			replaces[3] = {0};
	Datum		values[3] = {0};
	bool		nulls[3] = {0};
	int			ncols = 0;
	TupleDesc	tupdesc;
	bool		result = true;

	stats_check_required_arg(fcinfo, relarginfo, RELATION_ARG);
	reloid = PG_GETARG_OID(RELATION_ARG);

	stats_lock_check_privileges(reloid);

	/*
	 * Take RowExclusiveLock on pg_class, consistent with
	 * vac_update_relstats().
	 */
	crel = table_open(RelationRelationId, RowExclusiveLock);

	tupdesc = RelationGetDescr(crel);
	ctup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(reloid));
	if (!HeapTupleIsValid(ctup))
	{
		ereport(elevel,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("pg_class entry for relid %u not found", reloid)));
		table_close(crel, RowExclusiveLock);
		return false;
	}

	pgcform = (Form_pg_class) GETSTRUCT(ctup);

	/* relpages */
	if (!PG_ARGISNULL(RELPAGES_ARG))
	{
		int32		relpages = PG_GETARG_INT32(RELPAGES_ARG);

		/*
		 * Partitioned tables may have relpages=-1. Note: for relations with
		 * no storage, relpages=-1 is not used consistently, but must be
		 * supported here.
		 */
		if (relpages < -1)
		{
			ereport(elevel,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("relpages cannot be < -1")));
			result = false;
		}
		else if (relpages != pgcform->relpages)
		{
			replaces[ncols] = Anum_pg_class_relpages;
			values[ncols] = Int32GetDatum(relpages);
			ncols++;
		}
	}

	if (!PG_ARGISNULL(RELTUPLES_ARG))
	{
		float		reltuples = PG_GETARG_FLOAT4(RELTUPLES_ARG);

		if (reltuples < -1.0)
		{
			ereport(elevel,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("reltuples cannot be < -1.0")));
			result = false;
		}
		else if (reltuples != pgcform->reltuples)
		{
			replaces[ncols] = Anum_pg_class_reltuples;
			values[ncols] = Float4GetDatum(reltuples);
			ncols++;
		}

	}

	if (!PG_ARGISNULL(RELALLVISIBLE_ARG))
	{
		int32		relallvisible = PG_GETARG_INT32(RELALLVISIBLE_ARG);

		if (relallvisible < 0)
		{
			ereport(elevel,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("relallvisible cannot be < 0")));
			result = false;
		}
		else if (relallvisible != pgcform->relallvisible)
		{
			replaces[ncols] = Anum_pg_class_relallvisible;
			values[ncols] = Int32GetDatum(relallvisible);
			ncols++;
		}
	}

	/* only update pg_class if there is a meaningful change */
	if (ncols > 0)
	{
		HeapTuple	newtup;

		newtup = heap_modify_tuple_by_cols(ctup, tupdesc, ncols, replaces, values,
										   nulls);
		CatalogTupleUpdate(crel, &newtup->t_self, newtup);
		heap_freetuple(newtup);
	}

	/* release the lock, consistent with vac_update_relstats() */
	table_close(crel, RowExclusiveLock);

	CommandCounterIncrement();

	return result;
}

/*
 * Set statistics for a given pg_class entry.
 */
Datum
pg_set_relation_stats(PG_FUNCTION_ARGS)
{
	relation_statistics_update(fcinfo, ERROR);
	PG_RETURN_VOID();
}

/*
 * Clear statistics for a given pg_class entry; that is, set back to initial
 * stats for a newly-created table.
 */
Datum
pg_clear_relation_stats(PG_FUNCTION_ARGS)
{
	LOCAL_FCINFO(newfcinfo, 4);

	InitFunctionCallInfoData(*newfcinfo, NULL, 4, InvalidOid, NULL, NULL);

	newfcinfo->args[0].value = PG_GETARG_OID(0);
	newfcinfo->args[0].isnull = PG_ARGISNULL(0);
	newfcinfo->args[1].value = DEFAULT_RELPAGES;
	newfcinfo->args[1].isnull = false;
	newfcinfo->args[2].value = DEFAULT_RELTUPLES;
	newfcinfo->args[2].isnull = false;
	newfcinfo->args[3].value = DEFAULT_RELALLVISIBLE;
	newfcinfo->args[3].isnull = false;

	relation_statistics_update(newfcinfo, ERROR);
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
										  relarginfo, WARNING))
		result = false;

	if (!relation_statistics_update(positional_fcinfo, WARNING))
		result = false;

	PG_RETURN_BOOL(result);
}
