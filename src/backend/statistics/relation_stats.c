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
#include "utils/fmgroids.h"
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

static bool relation_statistics_update(FunctionCallInfo fcinfo, int elevel,
									   bool inplace);

/*
 * Internal function for modifying statistics for a relation.
 */
static bool
relation_statistics_update(FunctionCallInfo fcinfo, int elevel, bool inplace)
{
	Oid			reloid;
	Relation	crel;
	int32		relpages = DEFAULT_RELPAGES;
	bool		update_relpages = false;
	float		reltuples = DEFAULT_RELTUPLES;
	bool		update_reltuples = false;
	int32		relallvisible = DEFAULT_RELALLVISIBLE;
	bool		update_relallvisible = false;
	bool		result = true;

	if (!PG_ARGISNULL(RELPAGES_ARG))
	{
		relpages = PG_GETARG_INT32(RELPAGES_ARG);

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
		else
			update_relpages = true;
	}

	if (!PG_ARGISNULL(RELTUPLES_ARG))
	{
		reltuples = PG_GETARG_FLOAT4(RELTUPLES_ARG);

		if (reltuples < -1.0)
		{
			ereport(elevel,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("reltuples cannot be < -1.0")));
			result = false;
		}
		else
			update_reltuples = true;
	}

	if (!PG_ARGISNULL(RELALLVISIBLE_ARG))
	{
		relallvisible = PG_GETARG_INT32(RELALLVISIBLE_ARG);

		if (relallvisible < 0)
		{
			ereport(elevel,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("relallvisible cannot be < 0")));
			result = false;
		}
		else
			update_relallvisible = true;
	}

	stats_check_required_arg(fcinfo, relarginfo, RELATION_ARG);
	reloid = PG_GETARG_OID(RELATION_ARG);

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("Statistics cannot be modified during recovery.")));

	stats_lock_check_privileges(reloid);

	/*
	 * Take RowExclusiveLock on pg_class, consistent with
	 * vac_update_relstats().
	 */
	crel = table_open(RelationRelationId, RowExclusiveLock);

	if (inplace)
	{
		HeapTuple	ctup = NULL;
		ScanKeyData key[1];
		Form_pg_class pgcform;
		void	   *inplace_state = NULL;
		bool		dirty = false;

		ScanKeyInit(&key[0], Anum_pg_class_oid, BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(reloid));
		systable_inplace_update_begin(crel, ClassOidIndexId, true, NULL, 1, key,
									  &ctup, &inplace_state);
		if (!HeapTupleIsValid(ctup))
			elog(ERROR, "pg_class entry for relid %u vanished while updating statistics",
				 reloid);
		pgcform = (Form_pg_class) GETSTRUCT(ctup);

		if (update_relpages && pgcform->relpages != relpages)
		{
			pgcform->relpages = relpages;
			dirty = true;
		}
		if (update_reltuples && pgcform->reltuples != reltuples)
		{
			pgcform->reltuples = reltuples;
			dirty = true;
		}
		if (update_relallvisible && pgcform->relallvisible != relallvisible)
		{
			pgcform->relallvisible = relallvisible;
			dirty = true;
		}

		if (dirty)
			systable_inplace_update_finish(inplace_state, ctup);
		else
			systable_inplace_update_cancel(inplace_state);

		heap_freetuple(ctup);
	}
	else
	{
		TupleDesc	tupdesc = RelationGetDescr(crel);
		HeapTuple	ctup;
		Form_pg_class pgcform;
		int			replaces[3] = {0};
		Datum		values[3] = {0};
		bool		nulls[3] = {0};
		int			nreplaces = 0;

		ctup = SearchSysCache1(RELOID, ObjectIdGetDatum(reloid));
		if (!HeapTupleIsValid(ctup))
		{
			ereport(elevel,
					(errcode(ERRCODE_OBJECT_IN_USE),
					 errmsg("pg_class entry for relid %u not found", reloid)));
			table_close(crel, RowExclusiveLock);
			return false;
		}
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

		if (nreplaces > 0)
		{
			HeapTuple	newtup;

			newtup = heap_modify_tuple_by_cols(ctup, tupdesc, nreplaces,
											   replaces, values, nulls);
			CatalogTupleUpdate(crel, &newtup->t_self, newtup);
			heap_freetuple(newtup);
		}

		ReleaseSysCache(ctup);
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
	relation_statistics_update(fcinfo, ERROR, false);
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

	relation_statistics_update(newfcinfo, ERROR, false);
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

	if (!relation_statistics_update(positional_fcinfo, WARNING, true))
		result = false;

	PG_RETURN_BOOL(result);
}
