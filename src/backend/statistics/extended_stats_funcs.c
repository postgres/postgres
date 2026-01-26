/*-------------------------------------------------------------------------
 *
 * extended_stats_funcs.c
 *	  Functions for manipulating extended statistics.
 *
 * This file includes the set of facilities required to support the direct
 * manipulations of extended statistics objects.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/statistics/extended_stats_funcs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_database.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_statistic_ext_data.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "statistics/extended_stats_internal.h"
#include "statistics/stat_utils.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/*
 * Index of the arguments for the SQL functions.
 */
enum extended_stats_argnum
{
	RELSCHEMA_ARG = 0,
	RELNAME_ARG,
	STATSCHEMA_ARG,
	STATNAME_ARG,
	INHERITED_ARG,
	NDISTINCT_ARG,
	NUM_EXTENDED_STATS_ARGS,
};

/*
 * The argument names and type OIDs of the arguments for the SQL
 * functions.
 */
static struct StatsArgInfo extarginfo[] =
{
	[RELSCHEMA_ARG] = {"schemaname", TEXTOID},
	[RELNAME_ARG] = {"relname", TEXTOID},
	[STATSCHEMA_ARG] = {"statistics_schemaname", TEXTOID},
	[STATNAME_ARG] = {"statistics_name", TEXTOID},
	[INHERITED_ARG] = {"inherited", BOOLOID},
	[NDISTINCT_ARG] = {"n_distinct", PG_NDISTINCTOID},
	[NUM_EXTENDED_STATS_ARGS] = {0},
};

static bool extended_statistics_update(FunctionCallInfo fcinfo);

static HeapTuple get_pg_statistic_ext(Relation pg_stext, Oid nspoid,
									  const char *stxname);
static bool delete_pg_statistic_ext_data(Oid stxoid, bool inherited);

/*
 * Track the extended statistics kinds expected for a pg_statistic_ext
 * tuple.
 */
typedef struct
{
	bool		ndistinct;
	bool		dependencies;
	bool		mcv;
	bool		expressions;
} StakindFlags;

static void expand_stxkind(HeapTuple tup, StakindFlags *enabled);
static void upsert_pg_statistic_ext_data(const Datum *values,
										 const bool *nulls,
										 const bool *replaces);

/*
 * Fetch a pg_statistic_ext row by name and namespace OID.
 */
static HeapTuple
get_pg_statistic_ext(Relation pg_stext, Oid nspoid, const char *stxname)
{
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	Oid			stxoid = InvalidOid;

	ScanKeyInit(&key[0],
				Anum_pg_statistic_ext_stxname,
				BTEqualStrategyNumber,
				F_NAMEEQ,
				CStringGetDatum(stxname));
	ScanKeyInit(&key[1],
				Anum_pg_statistic_ext_stxnamespace,
				BTEqualStrategyNumber,
				F_OIDEQ,
				ObjectIdGetDatum(nspoid));

	/*
	 * Try to find matching pg_statistic_ext row.
	 */
	scan = systable_beginscan(pg_stext,
							  StatisticExtNameIndexId,
							  true,
							  NULL,
							  2,
							  key);

	/* Lookup is based on a unique index, so we get either 0 or 1 tuple. */
	tup = systable_getnext(scan);

	if (HeapTupleIsValid(tup))
		stxoid = ((Form_pg_statistic_ext) GETSTRUCT(tup))->oid;

	systable_endscan(scan);

	if (!OidIsValid(stxoid))
		return NULL;

	return SearchSysCacheCopy1(STATEXTOID, ObjectIdGetDatum(stxoid));
}

/*
 * Decode the stxkind column so that we know which stats types to expect,
 * returning a StakindFlags set depending on the stats kinds expected by
 * a pg_statistic_ext tuple.
 */
static void
expand_stxkind(HeapTuple tup, StakindFlags *enabled)
{
	Datum		datum;
	ArrayType  *arr;
	char	   *kinds;

	datum = SysCacheGetAttrNotNull(STATEXTOID,
								   tup,
								   Anum_pg_statistic_ext_stxkind);
	arr = DatumGetArrayTypeP(datum);
	if (ARR_NDIM(arr) != 1 || ARR_HASNULL(arr) || ARR_ELEMTYPE(arr) != CHAROID)
		elog(ERROR, "stxkind is not a one-dimension char array");

	kinds = (char *) ARR_DATA_PTR(arr);

	for (int i = 0; i < ARR_DIMS(arr)[0]; i++)
	{
		switch (kinds[i])
		{
			case STATS_EXT_NDISTINCT:
				enabled->ndistinct = true;
				break;
			case STATS_EXT_DEPENDENCIES:
				enabled->dependencies = true;
				break;
			case STATS_EXT_MCV:
				enabled->mcv = true;
				break;
			case STATS_EXT_EXPRESSIONS:
				enabled->expressions = true;
				break;
			default:
				elog(ERROR, "incorrect stxkind %c found", kinds[i]);
				break;
		}
	}
}

/*
 * Perform the actual storage of a pg_statistic_ext_data tuple.
 */
static void
upsert_pg_statistic_ext_data(const Datum *values, const bool *nulls,
							 const bool *replaces)
{
	Relation	pg_stextdata;
	HeapTuple	stxdtup;
	HeapTuple	newtup;

	pg_stextdata = table_open(StatisticExtDataRelationId, RowExclusiveLock);

	stxdtup = SearchSysCache2(STATEXTDATASTXOID,
							  values[Anum_pg_statistic_ext_data_stxoid - 1],
							  values[Anum_pg_statistic_ext_data_stxdinherit - 1]);

	if (HeapTupleIsValid(stxdtup))
	{
		newtup = heap_modify_tuple(stxdtup,
								   RelationGetDescr(pg_stextdata),
								   values,
								   nulls,
								   replaces);
		CatalogTupleUpdate(pg_stextdata, &newtup->t_self, newtup);
		ReleaseSysCache(stxdtup);
	}
	else
	{
		newtup = heap_form_tuple(RelationGetDescr(pg_stextdata), values, nulls);
		CatalogTupleInsert(pg_stextdata, newtup);
	}

	heap_freetuple(newtup);

	CommandCounterIncrement();

	table_close(pg_stextdata, RowExclusiveLock);
}

/*
 * Insert or update an extended statistics object.
 *
 * Major errors, such as the table not existing or permission errors, are
 * reported as ERRORs.  There are a couple of paths that generate a WARNING,
 * like when the statistics object or its schema do not exist, a conversion
 * failure on one statistic kind, or when other statistic kinds may still
 * be updated.
 */
static bool
extended_statistics_update(FunctionCallInfo fcinfo)
{
	char	   *relnspname;
	char	   *relname;
	Oid			nspoid;
	char	   *nspname;
	char	   *stxname;
	bool		inherited;
	Relation	pg_stext = NULL;
	HeapTuple	tup = NULL;

	StakindFlags enabled = {false, false, false, false};
	StakindFlags has = {false, false, false, false};

	Form_pg_statistic_ext stxform;

	Datum		values[Natts_pg_statistic_ext_data] = {0};
	bool		nulls[Natts_pg_statistic_ext_data] = {0};
	bool		replaces[Natts_pg_statistic_ext_data] = {0};
	bool		success = true;
	int			numexprs = 0;

	/* arrays of type info, if we need them */
	Oid			relid;
	Oid			locked_table = InvalidOid;

	/*
	 * Fill out the StakindFlags "has" structure based on which parameters
	 * were provided to the function.
	 */
	has.ndistinct = !PG_ARGISNULL(NDISTINCT_ARG);

	if (RecoveryInProgress())
	{
		ereport(WARNING,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("recovery is in progress"),
				errhint("Statistics cannot be modified during recovery."));
		return false;
	}

	/* relation arguments */
	stats_check_required_arg(fcinfo, extarginfo, RELSCHEMA_ARG);
	relnspname = TextDatumGetCString(PG_GETARG_DATUM(RELSCHEMA_ARG));
	stats_check_required_arg(fcinfo, extarginfo, RELNAME_ARG);
	relname = TextDatumGetCString(PG_GETARG_DATUM(RELNAME_ARG));

	/* extended statistics arguments */
	stats_check_required_arg(fcinfo, extarginfo, STATSCHEMA_ARG);
	nspname = TextDatumGetCString(PG_GETARG_DATUM(STATSCHEMA_ARG));
	stats_check_required_arg(fcinfo, extarginfo, STATNAME_ARG);
	stxname = TextDatumGetCString(PG_GETARG_DATUM(STATNAME_ARG));
	stats_check_required_arg(fcinfo, extarginfo, INHERITED_ARG);
	inherited = PG_GETARG_BOOL(INHERITED_ARG);

	/*
	 * First open the relation where we expect to find the statistics.  This
	 * is similar to relation and attribute statistics, so as ACL checks are
	 * done before any locks are taken, even before any attempts related to
	 * the extended stats object.
	 */
	relid = RangeVarGetRelidExtended(makeRangeVar(relnspname, relname, -1),
									 ShareUpdateExclusiveLock, 0,
									 RangeVarCallbackForStats, &locked_table);

	nspoid = get_namespace_oid(nspname, true);
	if (nspoid == InvalidOid)
	{
		ereport(WARNING,
				errcode(ERRCODE_UNDEFINED_OBJECT),
				errmsg("could not find schema \"%s\"", nspname));
		success = false;
		goto cleanup;
	}

	pg_stext = table_open(StatisticExtRelationId, RowExclusiveLock);
	tup = get_pg_statistic_ext(pg_stext, nspoid, stxname);

	if (!HeapTupleIsValid(tup))
	{
		ereport(WARNING,
				errcode(ERRCODE_UNDEFINED_OBJECT),
				errmsg("could not find extended statistics object \"%s\".\"%s\"",
					   quote_identifier(nspname),
					   quote_identifier(stxname)));
		success = false;
		goto cleanup;
	}

	stxform = (Form_pg_statistic_ext) GETSTRUCT(tup);

	/*
	 * The relation tracked by the stats object has to match with the relation
	 * we have already locked.
	 */
	if (stxform->stxrelid != relid)
	{
		ereport(WARNING,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("could not restore extended statistics object \"%s\".\"%s\": incorrect relation \"%s\".\"%s\" specified",
					   quote_identifier(nspname),
					   quote_identifier(stxname),
					   quote_identifier(relnspname),
					   quote_identifier(relname)));

		success = false;
		goto cleanup;
	}

	/* Find out what extended statistics kinds we should expect. */
	expand_stxkind(tup, &enabled);

	/*
	 * If the object cannot support ndistinct, we should not have data for it.
	 */
	if (has.ndistinct && !enabled.ndistinct)
	{
		ereport(WARNING,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("cannot not specify parameter \"%s\"",
					   extarginfo[NDISTINCT_ARG].argname),
				errhint("Extended statistics object \"%s\".\"%s\" does not support statistics of this type.",
						quote_identifier(nspname),
						quote_identifier(stxname)));

		has.ndistinct = false;
		success = false;
	}

	/*
	 * Populate the pg_statistic_ext_data result tuple.
	 */

	/* Primary Key: cannot be NULL or replaced. */
	values[Anum_pg_statistic_ext_data_stxoid - 1] = ObjectIdGetDatum(stxform->oid);
	nulls[Anum_pg_statistic_ext_data_stxoid - 1] = false;
	values[Anum_pg_statistic_ext_data_stxdinherit - 1] = BoolGetDatum(inherited);
	nulls[Anum_pg_statistic_ext_data_stxdinherit - 1] = false;

	/* All unspecified parameters will be left unmodified */
	nulls[Anum_pg_statistic_ext_data_stxdndistinct - 1] = true;
	nulls[Anum_pg_statistic_ext_data_stxddependencies - 1] = true;
	nulls[Anum_pg_statistic_ext_data_stxdmcv - 1] = true;
	nulls[Anum_pg_statistic_ext_data_stxdexpr - 1] = true;

	/*
	 * For each stats kind, deserialize the data at hand and perform a round
	 * of validation.  The resulting tuple is filled with a set of updated
	 * values.
	 */

	if (has.ndistinct)
	{
		Datum		ndistinct_datum = PG_GETARG_DATUM(NDISTINCT_ARG);
		bytea	   *data = DatumGetByteaPP(ndistinct_datum);
		MVNDistinct *ndistinct = statext_ndistinct_deserialize(data);

		if (statext_ndistinct_validate(ndistinct, &stxform->stxkeys,
									   numexprs, WARNING))
		{
			values[Anum_pg_statistic_ext_data_stxdndistinct - 1] = ndistinct_datum;
			nulls[Anum_pg_statistic_ext_data_stxdndistinct - 1] = false;
			replaces[Anum_pg_statistic_ext_data_stxdndistinct - 1] = true;
		}
		else
			success = false;

		statext_ndistinct_free(ndistinct);
	}

	upsert_pg_statistic_ext_data(values, nulls, replaces);

cleanup:
	if (HeapTupleIsValid(tup))
		heap_freetuple(tup);
	if (pg_stext != NULL)
		table_close(pg_stext, RowExclusiveLock);
	return success;
}

/*
 * Remove an existing pg_statistic_ext_data row for a given pg_statistic_ext
 * row and "inherited" pair.
 */
static bool
delete_pg_statistic_ext_data(Oid stxoid, bool inherited)
{
	Relation	sed = table_open(StatisticExtDataRelationId, RowExclusiveLock);
	HeapTuple	oldtup;
	bool		result = false;

	/* Is there already a pg_statistic tuple for this attribute? */
	oldtup = SearchSysCache2(STATEXTDATASTXOID,
							 ObjectIdGetDatum(stxoid),
							 BoolGetDatum(inherited));

	if (HeapTupleIsValid(oldtup))
	{
		CatalogTupleDelete(sed, &oldtup->t_self);
		ReleaseSysCache(oldtup);
		result = true;
	}

	table_close(sed, RowExclusiveLock);

	CommandCounterIncrement();

	return result;
}

/*
 * Restore (insert or replace) statistics for the given statistics object.
 *
 * This function accepts variadic arguments in key-value pairs, which are
 * given to stats_fill_fcinfo_from_arg_pairs to be mapped into positional
 * arguments.
 */
Datum
pg_restore_extended_stats(PG_FUNCTION_ARGS)
{
	LOCAL_FCINFO(positional_fcinfo, NUM_EXTENDED_STATS_ARGS);
	bool		result = true;

	InitFunctionCallInfoData(*positional_fcinfo, NULL, NUM_EXTENDED_STATS_ARGS,
							 InvalidOid, NULL, NULL);

	if (!stats_fill_fcinfo_from_arg_pairs(fcinfo, positional_fcinfo, extarginfo))
		result = false;

	if (!extended_statistics_update(positional_fcinfo))
		result = false;

	PG_RETURN_BOOL(result);
}

/*
 * Delete statistics for the given statistics object.
 */
Datum
pg_clear_extended_stats(PG_FUNCTION_ARGS)
{
	char	   *relnspname;
	char	   *relname;
	char	   *nspname;
	Oid			nspoid;
	Oid			relid;
	char	   *stxname;
	bool		inherited;
	Relation	pg_stext;
	HeapTuple	tup;
	Form_pg_statistic_ext stxform;
	Oid			locked_table = InvalidOid;

	/* relation arguments */
	stats_check_required_arg(fcinfo, extarginfo, RELSCHEMA_ARG);
	relnspname = TextDatumGetCString(PG_GETARG_DATUM(RELSCHEMA_ARG));
	stats_check_required_arg(fcinfo, extarginfo, RELNAME_ARG);
	relname = TextDatumGetCString(PG_GETARG_DATUM(RELNAME_ARG));

	/* extended statistics arguments */
	stats_check_required_arg(fcinfo, extarginfo, STATSCHEMA_ARG);
	nspname = TextDatumGetCString(PG_GETARG_DATUM(STATSCHEMA_ARG));
	stats_check_required_arg(fcinfo, extarginfo, STATNAME_ARG);
	stxname = TextDatumGetCString(PG_GETARG_DATUM(STATNAME_ARG));
	stats_check_required_arg(fcinfo, extarginfo, INHERITED_ARG);
	inherited = PG_GETARG_BOOL(INHERITED_ARG);

	if (RecoveryInProgress())
	{
		ereport(WARNING,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("recovery is in progress"),
				errhint("Statistics cannot be modified during recovery."));
		PG_RETURN_VOID();
	}

	/*
	 * First open the relation where we expect to find the statistics.  This
	 * is similar to relation and attribute statistics, so as ACL checks are
	 * done before any locks are taken, even before any attempts related to
	 * the extended stats object.
	 */
	relid = RangeVarGetRelidExtended(makeRangeVar(relnspname, relname, -1),
									 ShareUpdateExclusiveLock, 0,
									 RangeVarCallbackForStats, &locked_table);

	/* Now check if the namespace of the stats object exists. */
	nspoid = get_namespace_oid(nspname, true);
	if (nspoid == InvalidOid)
	{
		ereport(WARNING,
				errcode(ERRCODE_UNDEFINED_OBJECT),
				errmsg("could not find schema \"%s\"", nspname));
		PG_RETURN_VOID();
	}

	pg_stext = table_open(StatisticExtRelationId, RowExclusiveLock);
	tup = get_pg_statistic_ext(pg_stext, nspoid, stxname);

	if (!HeapTupleIsValid(tup))
	{
		table_close(pg_stext, RowExclusiveLock);
		ereport(WARNING,
				errcode(ERRCODE_UNDEFINED_OBJECT),
				errmsg("could not find extended statistics object \"%s\".\"%s\"",
					   nspname, stxname));
		PG_RETURN_VOID();
	}

	stxform = (Form_pg_statistic_ext) GETSTRUCT(tup);

	/*
	 * This should be consistent, based on the lock taken on the table when we
	 * started.
	 */
	if (stxform->stxrelid != relid)
	{
		table_close(pg_stext, RowExclusiveLock);
		ereport(WARNING,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("could not clear extended statistics object \"%s\".\"%s\": incorrect relation \"%s\".\"%s\" specified",
					   get_namespace_name(nspoid), stxname,
					   relnspname, relname));
		PG_RETURN_VOID();
	}

	delete_pg_statistic_ext_data(stxform->oid, inherited);
	heap_freetuple(tup);

	table_close(pg_stext, RowExclusiveLock);

	PG_RETURN_VOID();
}
