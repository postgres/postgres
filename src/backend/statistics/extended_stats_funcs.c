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
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
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
	DEPENDENCIES_ARG,
	MOST_COMMON_VALS_ARG,
	MOST_COMMON_FREQS_ARG,
	MOST_COMMON_BASE_FREQS_ARG,
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
	[DEPENDENCIES_ARG] = {"dependencies", PG_DEPENDENCIESOID},
	[MOST_COMMON_VALS_ARG] = {"most_common_vals", TEXTARRAYOID},
	[MOST_COMMON_FREQS_ARG] = {"most_common_freqs", FLOAT8ARRAYOID},
	[MOST_COMMON_BASE_FREQS_ARG] = {"most_common_base_freqs", FLOAT8ARRAYOID},
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

static bool check_mcvlist_array(const ArrayType *arr, int argindex,
								int required_ndims, int mcv_length);
static Datum import_mcv(const ArrayType *mcv_arr,
						const ArrayType *freqs_arr,
						const ArrayType *base_freqs_arr,
						Oid *atttypids, int32 *atttypmods,
						Oid *atttypcolls, int numattrs,
						bool *ok);


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
	Datum		exprdatum;
	bool		isnull;
	List	   *exprs = NIL;
	int			numattnums = 0;
	int			numexprs = 0;
	int			numattrs = 0;

	/* arrays of type info, if we need them */
	Oid		   *atttypids = NULL;
	int32	   *atttypmods = NULL;
	Oid		   *atttypcolls = NULL;
	Oid			relid;
	Oid			locked_table = InvalidOid;

	/*
	 * Fill out the StakindFlags "has" structure based on which parameters
	 * were provided to the function.
	 *
	 * The MCV stats composite value is an array of record type, but this is
	 * externally represented as three arrays that must be interleaved into
	 * the array of records (pg_stats_ext stores four arrays,
	 * most_common_val_nulls is built from the contents of most_common_vals).
	 * Therefore, none of the three array values is meaningful unless the
	 * other two are also present and in sync in terms of array length.
	 */
	has.mcv = (!PG_ARGISNULL(MOST_COMMON_VALS_ARG) &&
			   !PG_ARGISNULL(MOST_COMMON_FREQS_ARG) &&
			   !PG_ARGISNULL(MOST_COMMON_BASE_FREQS_ARG));
	has.ndistinct = !PG_ARGISNULL(NDISTINCT_ARG);
	has.dependencies = !PG_ARGISNULL(DEPENDENCIES_ARG);

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
				errmsg("could not find extended statistics object \"%s.%s\"",
					   nspname, stxname));
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
				errmsg("could not restore extended statistics object \"%s.%s\": incorrect relation \"%s.%s\" specified",
					   nspname, stxname,
					   relnspname, relname));

		success = false;
		goto cleanup;
	}

	/* Find out what extended statistics kinds we should expect. */
	expand_stxkind(tup, &enabled);
	numattnums = stxform->stxkeys.dim1;

	/* decode expression (if any) */
	exprdatum = SysCacheGetAttr(STATEXTOID,
								tup,
								Anum_pg_statistic_ext_stxexprs,
								&isnull);
	if (!isnull)
	{
		char	   *s;

		s = TextDatumGetCString(exprdatum);
		exprs = (List *) stringToNode(s);
		pfree(s);

		/*
		 * Run the expressions through eval_const_expressions().  This is not
		 * just an optimization, but is necessary, because the planner will be
		 * comparing them to similarly-processed qual clauses, and may fail to
		 * detect valid matches without this.
		 *
		 * We must not use canonicalize_qual(), however, since these are not
		 * qual expressions.
		 */
		exprs = (List *) eval_const_expressions(NULL, (Node *) exprs);

		/* May as well fix opfuncids too */
		fix_opfuncids((Node *) exprs);

		/* Compute the number of expression, for input validation. */
		numexprs = list_length(exprs);
	}

	numattrs = numattnums + numexprs;

	/*
	 * If the object cannot support ndistinct, we should not have data for it.
	 */
	if (has.ndistinct && !enabled.ndistinct)
	{
		ereport(WARNING,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("cannot specify parameter \"%s\"",
					   extarginfo[NDISTINCT_ARG].argname),
				errhint("Extended statistics object \"%s.%s\" does not support statistics of this type.",
						nspname, stxname));

		has.ndistinct = false;
		success = false;
	}

	/*
	 * If the object cannot support dependencies, we should not have data for
	 * it.
	 */
	if (has.dependencies && !enabled.dependencies)
	{
		ereport(WARNING,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("cannot specify parameter \"%s\"",
					   extarginfo[DEPENDENCIES_ARG].argname),
				errhint("Extended statistics object \"%s.%s\" does not support statistics of this type.",
						nspname, stxname));
		has.dependencies = false;
		success = false;
	}

	/*
	 * If the object cannot hold an MCV value, but any of the MCV parameters
	 * are set, then issue a WARNING and ensure that we do not try to load MCV
	 * stats later.  In pg_stats_ext, most_common_val_nulls, most_common_freqs
	 * and most_common_base_freqs are NULL if most_common_vals is NULL.
	 */
	if (!enabled.mcv)
	{
		if (!PG_ARGISNULL(MOST_COMMON_VALS_ARG) ||
			!PG_ARGISNULL(MOST_COMMON_FREQS_ARG) ||
			!PG_ARGISNULL(MOST_COMMON_BASE_FREQS_ARG))
		{
			ereport(WARNING,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("cannot specify parameters \"%s\", \"%s\" or \"%s\"",
						   extarginfo[MOST_COMMON_VALS_ARG].argname,
						   extarginfo[MOST_COMMON_FREQS_ARG].argname,
						   extarginfo[MOST_COMMON_BASE_FREQS_ARG].argname),
					errhint("Extended statistics object \"%s.%s\" does not support statistics of this type.",
							nspname, stxname));

			has.mcv = false;
			success = false;
		}
	}
	else if (!has.mcv)
	{
		/*
		 * If we do not have all of the MCV arrays set while the extended
		 * statistics object expects something, something is wrong.  This
		 * issues a WARNING if a partial input has been provided.
		 */
		if (!PG_ARGISNULL(MOST_COMMON_VALS_ARG) ||
			!PG_ARGISNULL(MOST_COMMON_FREQS_ARG) ||
			!PG_ARGISNULL(MOST_COMMON_BASE_FREQS_ARG))
		{
			ereport(WARNING,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("could not use \"%s\", \"%s\" and \"%s\": missing one or more parameters",
						   extarginfo[MOST_COMMON_VALS_ARG].argname,
						   extarginfo[MOST_COMMON_FREQS_ARG].argname,
						   extarginfo[MOST_COMMON_BASE_FREQS_ARG].argname));
			success = false;
		}
	}

	/*
	 * Either of these statistic types requires that we supply a semi-filled
	 * VacAttrStatP array.
	 *
	 * It is not possible to use the existing lookup_var_attr_stats() and
	 * examine_attribute() because these functions will skip attributes where
	 * attstattarget is 0, and we may have statistics data to import for those
	 * attributes.
	 */
	if (has.mcv)
	{
		atttypids = palloc0_array(Oid, numattrs);
		atttypmods = palloc0_array(int32, numattrs);
		atttypcolls = palloc0_array(Oid, numattrs);

		/*
		 * The leading stxkeys are attribute numbers up through numattnums.
		 * These keys must be in ascending AttNumber order, but we do not rely
		 * on that.
		 */
		for (int i = 0; i < numattnums; i++)
		{
			AttrNumber	attnum = stxform->stxkeys.values[i];
			HeapTuple	atup = SearchSysCache2(ATTNUM,
											   ObjectIdGetDatum(relid),
											   Int16GetDatum(attnum));

			Form_pg_attribute attr;

			/* Attribute not found */
			if (!HeapTupleIsValid(atup))
				elog(ERROR, "stxkeys references nonexistent attnum %d", attnum);

			attr = (Form_pg_attribute) GETSTRUCT(atup);

			if (attr->attisdropped)
				elog(ERROR, "stxkeys references dropped attnum %d", attnum);

			atttypids[i] = attr->atttypid;
			atttypmods[i] = attr->atttypmod;
			atttypcolls[i] = attr->attcollation;
			ReleaseSysCache(atup);
		}

		/*
		 * After all the positive number attnums in stxkeys come the negative
		 * numbers (if any) which represent expressions in the order that they
		 * appear in stxdexpr.  Because the expressions are always
		 * monotonically decreasing from -1, there is no point in looking at
		 * the values in stxkeys, it's enough to know how many of them there
		 * are.
		 */
		for (int i = numattnums; i < numattrs; i++)
		{
			Node	   *expr = list_nth(exprs, i - numattnums);

			atttypids[i] = exprType(expr);
			atttypmods[i] = exprTypmod(expr);
			atttypcolls[i] = exprCollation(expr);
		}
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

	if (has.dependencies)
	{
		Datum		dependencies_datum = PG_GETARG_DATUM(DEPENDENCIES_ARG);
		bytea	   *data = DatumGetByteaPP(dependencies_datum);
		MVDependencies *dependencies = statext_dependencies_deserialize(data);

		if (statext_dependencies_validate(dependencies, &stxform->stxkeys,
										  numexprs, WARNING))
		{
			values[Anum_pg_statistic_ext_data_stxddependencies - 1] = dependencies_datum;
			nulls[Anum_pg_statistic_ext_data_stxddependencies - 1] = false;
			replaces[Anum_pg_statistic_ext_data_stxddependencies - 1] = true;
		}
		else
			success = false;

		statext_dependencies_free(dependencies);
	}

	if (has.mcv)
	{
		Datum		datum;
		bool		val_ok = false;

		datum = import_mcv(PG_GETARG_ARRAYTYPE_P(MOST_COMMON_VALS_ARG),
						   PG_GETARG_ARRAYTYPE_P(MOST_COMMON_FREQS_ARG),
						   PG_GETARG_ARRAYTYPE_P(MOST_COMMON_BASE_FREQS_ARG),
						   atttypids, atttypmods, atttypcolls, numattrs,
						   &val_ok);

		if (val_ok)
		{
			Assert(datum != (Datum) 0);
			values[Anum_pg_statistic_ext_data_stxdmcv - 1] = datum;
			nulls[Anum_pg_statistic_ext_data_stxdmcv - 1] = false;
			replaces[Anum_pg_statistic_ext_data_stxdmcv - 1] = true;
		}
		else
			success = false;
	}

	upsert_pg_statistic_ext_data(values, nulls, replaces);

cleanup:
	if (HeapTupleIsValid(tup))
		heap_freetuple(tup);
	if (pg_stext != NULL)
		table_close(pg_stext, RowExclusiveLock);
	if (atttypids != NULL)
		pfree(atttypids);
	if (atttypmods != NULL)
		pfree(atttypmods);
	if (atttypcolls != NULL)
		pfree(atttypcolls);
	return success;
}

/*
 * Consistency checks to ensure that other mcvlist arrays are in alignment
 * with the mcv array.
 */
static bool
check_mcvlist_array(const ArrayType *arr, int argindex, int required_ndims,
					int mcv_length)
{
	if (ARR_NDIM(arr) != required_ndims)
	{
		ereport(WARNING,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("could not parse array \"%s\": incorrect number of dimensions (%d required)",
					   extarginfo[argindex].argname, required_ndims));
		return false;
	}

	if (array_contains_nulls(arr))
	{
		ereport(WARNING,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("could not parse array \"%s\": NULL value found",
					   extarginfo[argindex].argname));
		return false;
	}

	if (ARR_DIMS(arr)[0] != mcv_length)
	{
		ereport(WARNING,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("could not parse array \"%s\": incorrect number of elements (same as \"%s\" required)",
					   extarginfo[argindex].argname,
					   extarginfo[MOST_COMMON_VALS_ARG].argname));
		return false;
	}

	return true;
}

/*
 * Create the stxdmcv datum from the equal-sized arrays of most common values,
 * their null flags, and the frequency and base frequency associated with
 * each value.
 */
static Datum
import_mcv(const ArrayType *mcv_arr, const ArrayType *freqs_arr,
		   const ArrayType *base_freqs_arr, Oid *atttypids, int32 *atttypmods,
		   Oid *atttypcolls, int numattrs, bool *ok)
{
	int			nitems;
	Datum	   *mcv_elems;
	bool	   *mcv_nulls;
	int			check_nummcv;
	Datum		mcv = (Datum) 0;

	*ok = false;

	/*
	 * mcv_arr is an array of arrays.  Each inner array must have the same
	 * number of elements "numattrs".
	 */
	if (ARR_NDIM(mcv_arr) != 2)
	{
		ereport(WARNING,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("could not parse array \"%s\": incorrect number of dimensions (%d required)",
					   extarginfo[MOST_COMMON_VALS_ARG].argname, 2));
		goto mcv_error;
	}

	if (ARR_DIMS(mcv_arr)[1] != numattrs)
	{
		ereport(WARNING,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("could not parse array \"%s\": found %d attributes but expected %d",
					   extarginfo[MOST_COMMON_VALS_ARG].argname,
					   ARR_DIMS(mcv_arr)[1], numattrs));
		goto mcv_error;
	}

	/*
	 * "most_common_freqs" and "most_common_base_freqs" arrays must be of the
	 * same length, one-dimension and cannot contain NULLs.  We use mcv_arr as
	 * the reference array for determining their length.
	 */
	nitems = ARR_DIMS(mcv_arr)[0];
	if (!check_mcvlist_array(freqs_arr, MOST_COMMON_FREQS_ARG, 1, nitems) ||
		!check_mcvlist_array(base_freqs_arr, MOST_COMMON_BASE_FREQS_ARG, 1, nitems))
	{
		/* inconsistent input arrays found */
		goto mcv_error;
	}

	/*
	 * This part builds the contents for "most_common_val_nulls", based on the
	 * values from "most_common_vals".
	 */
	deconstruct_array_builtin(mcv_arr, TEXTOID, &mcv_elems,
							  &mcv_nulls, &check_nummcv);

	mcv = statext_mcv_import(WARNING, numattrs,
							 atttypids, atttypmods, atttypcolls,
							 nitems, mcv_elems, mcv_nulls,
							 (float8 *) ARR_DATA_PTR(freqs_arr),
							 (float8 *) ARR_DATA_PTR(base_freqs_arr));

	*ok = (mcv != (Datum) 0);

mcv_error:
	return mcv;
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
				errmsg("could not find extended statistics object \"%s.%s\"",
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
				errmsg("could not clear extended statistics object \"%s.%s\": incorrect relation \"%s.%s\" specified",
					   get_namespace_name(nspoid), stxname,
					   relnspname, relname));
		PG_RETURN_VOID();
	}

	delete_pg_statistic_ext_data(stxform->oid, inherited);
	heap_freetuple(tup);

	table_close(pg_stext, RowExclusiveLock);

	PG_RETURN_VOID();
}
