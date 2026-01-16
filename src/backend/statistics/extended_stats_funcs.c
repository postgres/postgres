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
#include "statistics/stat_utils.h"
#include "utils/acl.h"
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
	[NUM_EXTENDED_STATS_ARGS] = {0},
};

static HeapTuple get_pg_statistic_ext(Relation pg_stext, Oid nspoid,
									  const char *stxname);
static bool delete_pg_statistic_ext_data(Oid stxoid, bool inherited);

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
