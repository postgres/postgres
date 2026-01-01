/*-------------------------------------------------------------------------
 * attribute_stats.c
 *
 *	  PostgreSQL relation attribute statistics manipulation.
 *
 * Code supporting the direct import of relation attribute statistics, similar
 * to what is done by the ANALYZE command.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *       src/backend/statistics/attribute_stats.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_operator.h"
#include "nodes/makefuncs.h"
#include "statistics/statistics.h"
#include "statistics/stat_utils.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

/*
 * Positional argument numbers, names, and types for
 * attribute_statistics_update() and pg_restore_attribute_stats().
 */

enum attribute_stats_argnum
{
	ATTRELSCHEMA_ARG = 0,
	ATTRELNAME_ARG,
	ATTNAME_ARG,
	ATTNUM_ARG,
	INHERITED_ARG,
	NULL_FRAC_ARG,
	AVG_WIDTH_ARG,
	N_DISTINCT_ARG,
	MOST_COMMON_VALS_ARG,
	MOST_COMMON_FREQS_ARG,
	HISTOGRAM_BOUNDS_ARG,
	CORRELATION_ARG,
	MOST_COMMON_ELEMS_ARG,
	MOST_COMMON_ELEM_FREQS_ARG,
	ELEM_COUNT_HISTOGRAM_ARG,
	RANGE_LENGTH_HISTOGRAM_ARG,
	RANGE_EMPTY_FRAC_ARG,
	RANGE_BOUNDS_HISTOGRAM_ARG,
	NUM_ATTRIBUTE_STATS_ARGS
};

static struct StatsArgInfo attarginfo[] =
{
	[ATTRELSCHEMA_ARG] = {"schemaname", TEXTOID},
	[ATTRELNAME_ARG] = {"relname", TEXTOID},
	[ATTNAME_ARG] = {"attname", TEXTOID},
	[ATTNUM_ARG] = {"attnum", INT2OID},
	[INHERITED_ARG] = {"inherited", BOOLOID},
	[NULL_FRAC_ARG] = {"null_frac", FLOAT4OID},
	[AVG_WIDTH_ARG] = {"avg_width", INT4OID},
	[N_DISTINCT_ARG] = {"n_distinct", FLOAT4OID},
	[MOST_COMMON_VALS_ARG] = {"most_common_vals", TEXTOID},
	[MOST_COMMON_FREQS_ARG] = {"most_common_freqs", FLOAT4ARRAYOID},
	[HISTOGRAM_BOUNDS_ARG] = {"histogram_bounds", TEXTOID},
	[CORRELATION_ARG] = {"correlation", FLOAT4OID},
	[MOST_COMMON_ELEMS_ARG] = {"most_common_elems", TEXTOID},
	[MOST_COMMON_ELEM_FREQS_ARG] = {"most_common_elem_freqs", FLOAT4ARRAYOID},
	[ELEM_COUNT_HISTOGRAM_ARG] = {"elem_count_histogram", FLOAT4ARRAYOID},
	[RANGE_LENGTH_HISTOGRAM_ARG] = {"range_length_histogram", TEXTOID},
	[RANGE_EMPTY_FRAC_ARG] = {"range_empty_frac", FLOAT4OID},
	[RANGE_BOUNDS_HISTOGRAM_ARG] = {"range_bounds_histogram", TEXTOID},
	[NUM_ATTRIBUTE_STATS_ARGS] = {0}
};

/*
 * Positional argument numbers, names, and types for
 * pg_clear_attribute_stats().
 */

enum clear_attribute_stats_argnum
{
	C_ATTRELSCHEMA_ARG = 0,
	C_ATTRELNAME_ARG,
	C_ATTNAME_ARG,
	C_INHERITED_ARG,
	C_NUM_ATTRIBUTE_STATS_ARGS
};

static struct StatsArgInfo cleararginfo[] =
{
	[C_ATTRELSCHEMA_ARG] = {"relation", TEXTOID},
	[C_ATTRELNAME_ARG] = {"relation", TEXTOID},
	[C_ATTNAME_ARG] = {"attname", TEXTOID},
	[C_INHERITED_ARG] = {"inherited", BOOLOID},
	[C_NUM_ATTRIBUTE_STATS_ARGS] = {0}
};

static bool attribute_statistics_update(FunctionCallInfo fcinfo);
static void upsert_pg_statistic(Relation starel, HeapTuple oldtup,
								const Datum *values, const bool *nulls, const bool *replaces);
static bool delete_pg_statistic(Oid reloid, AttrNumber attnum, bool stainherit);

/*
 * Insert or Update Attribute Statistics
 *
 * See pg_statistic.h for an explanation of how each statistic kind is
 * stored. Custom statistics kinds are not supported.
 *
 * Depending on the statistics kind, we need to derive information from the
 * attribute for which we're storing the stats. For instance, the MCVs are
 * stored as an anyarray, and the representation of the array needs to store
 * the correct element type, which must be derived from the attribute.
 *
 * Major errors, such as the table not existing, the attribute not existing,
 * or a permissions failure are always reported at ERROR. Other errors, such
 * as a conversion failure on one statistic kind, are reported as a WARNING
 * and other statistic kinds may still be updated.
 */
static bool
attribute_statistics_update(FunctionCallInfo fcinfo)
{
	char	   *nspname;
	char	   *relname;
	Oid			reloid;
	char	   *attname;
	AttrNumber	attnum;
	bool		inherited;
	Oid			locked_table = InvalidOid;

	Relation	starel;
	HeapTuple	statup;

	Oid			atttypid = InvalidOid;
	int32		atttypmod;
	char		atttyptype;
	Oid			atttypcoll = InvalidOid;
	Oid			eq_opr = InvalidOid;
	Oid			lt_opr = InvalidOid;

	Oid			elemtypid = InvalidOid;
	Oid			elem_eq_opr = InvalidOid;

	FmgrInfo	array_in_fn;

	bool		do_mcv = !PG_ARGISNULL(MOST_COMMON_FREQS_ARG) &&
		!PG_ARGISNULL(MOST_COMMON_VALS_ARG);
	bool		do_histogram = !PG_ARGISNULL(HISTOGRAM_BOUNDS_ARG);
	bool		do_correlation = !PG_ARGISNULL(CORRELATION_ARG);
	bool		do_mcelem = !PG_ARGISNULL(MOST_COMMON_ELEMS_ARG) &&
		!PG_ARGISNULL(MOST_COMMON_ELEM_FREQS_ARG);
	bool		do_dechist = !PG_ARGISNULL(ELEM_COUNT_HISTOGRAM_ARG);
	bool		do_bounds_histogram = !PG_ARGISNULL(RANGE_BOUNDS_HISTOGRAM_ARG);
	bool		do_range_length_histogram = !PG_ARGISNULL(RANGE_LENGTH_HISTOGRAM_ARG) &&
		!PG_ARGISNULL(RANGE_EMPTY_FRAC_ARG);

	Datum		values[Natts_pg_statistic] = {0};
	bool		nulls[Natts_pg_statistic] = {0};
	bool		replaces[Natts_pg_statistic] = {0};

	bool		result = true;

	stats_check_required_arg(fcinfo, attarginfo, ATTRELSCHEMA_ARG);
	stats_check_required_arg(fcinfo, attarginfo, ATTRELNAME_ARG);

	nspname = TextDatumGetCString(PG_GETARG_DATUM(ATTRELSCHEMA_ARG));
	relname = TextDatumGetCString(PG_GETARG_DATUM(ATTRELNAME_ARG));

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("Statistics cannot be modified during recovery.")));

	/* lock before looking up attribute */
	reloid = RangeVarGetRelidExtended(makeRangeVar(nspname, relname, -1),
									  ShareUpdateExclusiveLock, 0,
									  RangeVarCallbackForStats, &locked_table);

	/* user can specify either attname or attnum, but not both */
	if (!PG_ARGISNULL(ATTNAME_ARG))
	{
		if (!PG_ARGISNULL(ATTNUM_ARG))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("cannot specify both \"%s\" and \"%s\"", "attname", "attnum")));
		attname = TextDatumGetCString(PG_GETARG_DATUM(ATTNAME_ARG));
		attnum = get_attnum(reloid, attname);
		/* note that this test covers attisdropped cases too: */
		if (attnum == InvalidAttrNumber)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" does not exist",
							attname, relname)));
	}
	else if (!PG_ARGISNULL(ATTNUM_ARG))
	{
		attnum = PG_GETARG_INT16(ATTNUM_ARG);
		attname = get_attname(reloid, attnum, true);
		/* annoyingly, get_attname doesn't check attisdropped */
		if (attname == NULL ||
			!SearchSysCacheExistsAttName(reloid, attname))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column %d of relation \"%s\" does not exist",
							attnum, relname)));
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("must specify either \"%s\" or \"%s\"", "attname", "attnum")));
		attname = NULL;			/* keep compiler quiet */
		attnum = 0;
	}

	if (attnum < 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot modify statistics on system column \"%s\"",
						attname)));

	stats_check_required_arg(fcinfo, attarginfo, INHERITED_ARG);
	inherited = PG_GETARG_BOOL(INHERITED_ARG);

	/*
	 * Check argument sanity. If some arguments are unusable, emit a WARNING
	 * and set the corresponding argument to NULL in fcinfo.
	 */

	if (!stats_check_arg_array(fcinfo, attarginfo, MOST_COMMON_FREQS_ARG))
	{
		do_mcv = false;
		result = false;
	}

	if (!stats_check_arg_array(fcinfo, attarginfo, MOST_COMMON_ELEM_FREQS_ARG))
	{
		do_mcelem = false;
		result = false;
	}
	if (!stats_check_arg_array(fcinfo, attarginfo, ELEM_COUNT_HISTOGRAM_ARG))
	{
		do_dechist = false;
		result = false;
	}

	if (!stats_check_arg_pair(fcinfo, attarginfo,
							  MOST_COMMON_VALS_ARG, MOST_COMMON_FREQS_ARG))
	{
		do_mcv = false;
		result = false;
	}

	if (!stats_check_arg_pair(fcinfo, attarginfo,
							  MOST_COMMON_ELEMS_ARG,
							  MOST_COMMON_ELEM_FREQS_ARG))
	{
		do_mcelem = false;
		result = false;
	}

	if (!stats_check_arg_pair(fcinfo, attarginfo,
							  RANGE_LENGTH_HISTOGRAM_ARG,
							  RANGE_EMPTY_FRAC_ARG))
	{
		do_range_length_histogram = false;
		result = false;
	}

	/* derive information from attribute */
	statatt_get_type(reloid, attnum,
					 &atttypid, &atttypmod,
					 &atttyptype, &atttypcoll,
					 &eq_opr, &lt_opr);

	/* if needed, derive element type */
	if (do_mcelem || do_dechist)
	{
		if (!statatt_get_elem_type(atttypid, atttyptype,
								   &elemtypid, &elem_eq_opr))
		{
			ereport(WARNING,
					(errmsg("could not determine element type of column \"%s\"", attname),
					 errdetail("Cannot set %s or %s.",
							   "STATISTIC_KIND_MCELEM", "STATISTIC_KIND_DECHIST")));
			elemtypid = InvalidOid;
			elem_eq_opr = InvalidOid;

			do_mcelem = false;
			do_dechist = false;
			result = false;
		}
	}

	/* histogram and correlation require less-than operator */
	if ((do_histogram || do_correlation) && !OidIsValid(lt_opr))
	{
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("could not determine less-than operator for column \"%s\"", attname),
				 errdetail("Cannot set %s or %s.",
						   "STATISTIC_KIND_HISTOGRAM", "STATISTIC_KIND_CORRELATION")));

		do_histogram = false;
		do_correlation = false;
		result = false;
	}

	/* only range types can have range stats */
	if ((do_range_length_histogram || do_bounds_histogram) &&
		!(atttyptype == TYPTYPE_RANGE || atttyptype == TYPTYPE_MULTIRANGE))
	{
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("column \"%s\" is not a range type", attname),
				 errdetail("Cannot set %s or %s.",
						   "STATISTIC_KIND_RANGE_LENGTH_HISTOGRAM", "STATISTIC_KIND_BOUNDS_HISTOGRAM")));

		do_bounds_histogram = false;
		do_range_length_histogram = false;
		result = false;
	}

	fmgr_info(F_ARRAY_IN, &array_in_fn);

	starel = table_open(StatisticRelationId, RowExclusiveLock);

	statup = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(reloid), Int16GetDatum(attnum), BoolGetDatum(inherited));

	/* initialize from existing tuple if exists */
	if (HeapTupleIsValid(statup))
		heap_deform_tuple(statup, RelationGetDescr(starel), values, nulls);
	else
		statatt_init_empty_tuple(reloid, attnum, inherited, values, nulls,
								 replaces);

	/* if specified, set to argument values */
	if (!PG_ARGISNULL(NULL_FRAC_ARG))
	{
		values[Anum_pg_statistic_stanullfrac - 1] = PG_GETARG_DATUM(NULL_FRAC_ARG);
		replaces[Anum_pg_statistic_stanullfrac - 1] = true;
	}
	if (!PG_ARGISNULL(AVG_WIDTH_ARG))
	{
		values[Anum_pg_statistic_stawidth - 1] = PG_GETARG_DATUM(AVG_WIDTH_ARG);
		replaces[Anum_pg_statistic_stawidth - 1] = true;
	}
	if (!PG_ARGISNULL(N_DISTINCT_ARG))
	{
		values[Anum_pg_statistic_stadistinct - 1] = PG_GETARG_DATUM(N_DISTINCT_ARG);
		replaces[Anum_pg_statistic_stadistinct - 1] = true;
	}

	/* STATISTIC_KIND_MCV */
	if (do_mcv)
	{
		bool		converted;
		Datum		stanumbers = PG_GETARG_DATUM(MOST_COMMON_FREQS_ARG);
		Datum		stavalues = statatt_build_stavalues("most_common_vals",
														&array_in_fn,
														PG_GETARG_DATUM(MOST_COMMON_VALS_ARG),
														atttypid, atttypmod,
														&converted);

		if (converted)
		{
			statatt_set_slot(values, nulls, replaces,
							 STATISTIC_KIND_MCV,
							 eq_opr, atttypcoll,
							 stanumbers, false, stavalues, false);
		}
		else
			result = false;
	}

	/* STATISTIC_KIND_HISTOGRAM */
	if (do_histogram)
	{
		Datum		stavalues;
		bool		converted = false;

		stavalues = statatt_build_stavalues("histogram_bounds",
											&array_in_fn,
											PG_GETARG_DATUM(HISTOGRAM_BOUNDS_ARG),
											atttypid, atttypmod,
											&converted);

		if (converted)
		{
			statatt_set_slot(values, nulls, replaces,
							 STATISTIC_KIND_HISTOGRAM,
							 lt_opr, atttypcoll,
							 0, true, stavalues, false);
		}
		else
			result = false;
	}

	/* STATISTIC_KIND_CORRELATION */
	if (do_correlation)
	{
		Datum		elems[] = {PG_GETARG_DATUM(CORRELATION_ARG)};
		ArrayType  *arry = construct_array_builtin(elems, 1, FLOAT4OID);
		Datum		stanumbers = PointerGetDatum(arry);

		statatt_set_slot(values, nulls, replaces,
						 STATISTIC_KIND_CORRELATION,
						 lt_opr, atttypcoll,
						 stanumbers, false, 0, true);
	}

	/* STATISTIC_KIND_MCELEM */
	if (do_mcelem)
	{
		Datum		stanumbers = PG_GETARG_DATUM(MOST_COMMON_ELEM_FREQS_ARG);
		bool		converted = false;
		Datum		stavalues;

		stavalues = statatt_build_stavalues("most_common_elems",
											&array_in_fn,
											PG_GETARG_DATUM(MOST_COMMON_ELEMS_ARG),
											elemtypid, atttypmod,
											&converted);

		if (converted)
		{
			statatt_set_slot(values, nulls, replaces,
							 STATISTIC_KIND_MCELEM,
							 elem_eq_opr, atttypcoll,
							 stanumbers, false, stavalues, false);
		}
		else
			result = false;
	}

	/* STATISTIC_KIND_DECHIST */
	if (do_dechist)
	{
		Datum		stanumbers = PG_GETARG_DATUM(ELEM_COUNT_HISTOGRAM_ARG);

		statatt_set_slot(values, nulls, replaces,
						 STATISTIC_KIND_DECHIST,
						 elem_eq_opr, atttypcoll,
						 stanumbers, false, 0, true);
	}

	/*
	 * STATISTIC_KIND_BOUNDS_HISTOGRAM
	 *
	 * This stakind appears before STATISTIC_KIND_RANGE_LENGTH_HISTOGRAM even
	 * though it is numerically greater, and all other stakinds appear in
	 * numerical order. We duplicate this quirk for consistency.
	 */
	if (do_bounds_histogram)
	{
		bool		converted = false;
		Datum		stavalues;

		stavalues = statatt_build_stavalues("range_bounds_histogram",
											&array_in_fn,
											PG_GETARG_DATUM(RANGE_BOUNDS_HISTOGRAM_ARG),
											atttypid, atttypmod,
											&converted);

		if (converted)
		{
			statatt_set_slot(values, nulls, replaces,
							 STATISTIC_KIND_BOUNDS_HISTOGRAM,
							 InvalidOid, InvalidOid,
							 0, true, stavalues, false);
		}
		else
			result = false;
	}

	/* STATISTIC_KIND_RANGE_LENGTH_HISTOGRAM */
	if (do_range_length_histogram)
	{
		/* The anyarray is always a float8[] for this stakind */
		Datum		elems[] = {PG_GETARG_DATUM(RANGE_EMPTY_FRAC_ARG)};
		ArrayType  *arry = construct_array_builtin(elems, 1, FLOAT4OID);
		Datum		stanumbers = PointerGetDatum(arry);

		bool		converted = false;
		Datum		stavalues;

		stavalues = statatt_build_stavalues("range_length_histogram",
											&array_in_fn,
											PG_GETARG_DATUM(RANGE_LENGTH_HISTOGRAM_ARG),
											FLOAT8OID, 0, &converted);

		if (converted)
		{
			statatt_set_slot(values, nulls, replaces,
							 STATISTIC_KIND_RANGE_LENGTH_HISTOGRAM,
							 Float8LessOperator, InvalidOid,
							 stanumbers, false, stavalues, false);
		}
		else
			result = false;
	}

	upsert_pg_statistic(starel, statup, values, nulls, replaces);

	if (HeapTupleIsValid(statup))
		ReleaseSysCache(statup);
	table_close(starel, RowExclusiveLock);

	return result;
}

/*
 * Upsert the pg_statistic record.
 */
static void
upsert_pg_statistic(Relation starel, HeapTuple oldtup,
					const Datum *values, const bool *nulls, const bool *replaces)
{
	HeapTuple	newtup;

	if (HeapTupleIsValid(oldtup))
	{
		newtup = heap_modify_tuple(oldtup, RelationGetDescr(starel),
								   values, nulls, replaces);
		CatalogTupleUpdate(starel, &newtup->t_self, newtup);
	}
	else
	{
		newtup = heap_form_tuple(RelationGetDescr(starel), values, nulls);
		CatalogTupleInsert(starel, newtup);
	}

	heap_freetuple(newtup);

	CommandCounterIncrement();
}

/*
 * Delete pg_statistic record.
 */
static bool
delete_pg_statistic(Oid reloid, AttrNumber attnum, bool stainherit)
{
	Relation	sd = table_open(StatisticRelationId, RowExclusiveLock);
	HeapTuple	oldtup;
	bool		result = false;

	/* Is there already a pg_statistic tuple for this attribute? */
	oldtup = SearchSysCache3(STATRELATTINH,
							 ObjectIdGetDatum(reloid),
							 Int16GetDatum(attnum),
							 BoolGetDatum(stainherit));

	if (HeapTupleIsValid(oldtup))
	{
		CatalogTupleDelete(sd, &oldtup->t_self);
		ReleaseSysCache(oldtup);
		result = true;
	}

	table_close(sd, RowExclusiveLock);

	CommandCounterIncrement();

	return result;
}

/*
 * Delete statistics for the given attribute.
 */
Datum
pg_clear_attribute_stats(PG_FUNCTION_ARGS)
{
	char	   *nspname;
	char	   *relname;
	Oid			reloid;
	char	   *attname;
	AttrNumber	attnum;
	bool		inherited;
	Oid			locked_table = InvalidOid;

	stats_check_required_arg(fcinfo, cleararginfo, C_ATTRELSCHEMA_ARG);
	stats_check_required_arg(fcinfo, cleararginfo, C_ATTRELNAME_ARG);
	stats_check_required_arg(fcinfo, cleararginfo, C_ATTNAME_ARG);
	stats_check_required_arg(fcinfo, cleararginfo, C_INHERITED_ARG);

	nspname = TextDatumGetCString(PG_GETARG_DATUM(C_ATTRELSCHEMA_ARG));
	relname = TextDatumGetCString(PG_GETARG_DATUM(C_ATTRELNAME_ARG));

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("Statistics cannot be modified during recovery.")));

	reloid = RangeVarGetRelidExtended(makeRangeVar(nspname, relname, -1),
									  ShareUpdateExclusiveLock, 0,
									  RangeVarCallbackForStats, &locked_table);

	attname = TextDatumGetCString(PG_GETARG_DATUM(C_ATTNAME_ARG));
	attnum = get_attnum(reloid, attname);

	if (attnum < 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot clear statistics on system column \"%s\"",
						attname)));

	if (attnum == InvalidAttrNumber)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						attname, get_rel_name(reloid))));

	inherited = PG_GETARG_BOOL(C_INHERITED_ARG);

	delete_pg_statistic(reloid, attnum, inherited);
	PG_RETURN_VOID();
}

/*
 * Import statistics for a given relation attribute.
 *
 * Inserts or replaces a row in pg_statistic for the given relation and
 * attribute name or number. It takes input parameters that correspond to
 * columns in the view pg_stats.
 *
 * Parameters are given in a pseudo named-attribute style: they must be
 * pairs of parameter names (as text) and values (of appropriate types).
 * We do that, rather than using regular named-parameter notation, so
 * that we can add or change parameters without fear of breaking
 * carelessly-written calls.
 *
 * Parameters null_frac, avg_width, and n_distinct all correspond to NOT NULL
 * columns in pg_statistic. The remaining parameters all belong to a specific
 * stakind. Some stakinds require multiple parameters, which must be specified
 * together (or neither specified).
 *
 * Parameters are only superficially validated. Omitting a parameter or
 * passing NULL leaves the statistic unchanged.
 *
 * Parameters corresponding to ANYARRAY columns are instead passed in as text
 * values, which is a valid input string for an array of the type or element
 * type of the attribute. Any error generated by the array_in() function will
 * in turn fail the function.
 */
Datum
pg_restore_attribute_stats(PG_FUNCTION_ARGS)
{
	LOCAL_FCINFO(positional_fcinfo, NUM_ATTRIBUTE_STATS_ARGS);
	bool		result = true;

	InitFunctionCallInfoData(*positional_fcinfo, NULL, NUM_ATTRIBUTE_STATS_ARGS,
							 InvalidOid, NULL, NULL);

	if (!stats_fill_fcinfo_from_arg_pairs(fcinfo, positional_fcinfo,
										  attarginfo))
		result = false;

	if (!attribute_statistics_update(positional_fcinfo))
		result = false;

	PG_RETURN_BOOL(result);
}
