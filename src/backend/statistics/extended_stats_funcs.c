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
#include "catalog/pg_collation_d.h"
#include "catalog/pg_database.h"
#include "catalog/pg_operator.h"
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
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


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
	EXPRESSIONS_ARG,
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
	[EXPRESSIONS_ARG] = {"exprs", JSONBOID},
	[NUM_EXTENDED_STATS_ARGS] = {0},
};

/*
 * An index of the elements of a stxdexpr Datum, which repeat for each
 * expression in the extended statistics object.
 */
enum extended_stats_exprs_element
{
	NULL_FRAC_ELEM = 0,
	AVG_WIDTH_ELEM,
	N_DISTINCT_ELEM,
	MOST_COMMON_VALS_ELEM,
	MOST_COMMON_FREQS_ELEM,
	HISTOGRAM_BOUNDS_ELEM,
	CORRELATION_ELEM,
	MOST_COMMON_ELEMS_ELEM,
	MOST_COMMON_ELEM_FREQS_ELEM,
	ELEM_COUNT_HISTOGRAM_ELEM,
	RANGE_LENGTH_HISTOGRAM_ELEM,
	RANGE_EMPTY_FRAC_ELEM,
	RANGE_BOUNDS_HISTOGRAM_ELEM,
	NUM_ATTRIBUTE_STATS_ELEMS
};

/*
 * The argument names of the repeating arguments for stxdexpr.
 */
static const char *extexprargname[NUM_ATTRIBUTE_STATS_ELEMS] =
{
	"null_frac",
	"avg_width",
	"n_distinct",
	"most_common_vals",
	"most_common_freqs",
	"histogram_bounds",
	"correlation",
	"most_common_elems",
	"most_common_elem_freqs",
	"elem_count_histogram",
	"range_length_histogram",
	"range_empty_frac",
	"range_bounds_histogram"
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
static Datum import_expressions(Relation pgsd, int numexprs,
								Oid *atttypids, int32 *atttypmods,
								Oid *atttypcolls, Jsonb *exprs_jsonb,
								bool *exprs_is_perfect);
static Datum import_mcv(const ArrayType *mcv_arr,
						const ArrayType *freqs_arr,
						const ArrayType *base_freqs_arr,
						Oid *atttypids, int32 *atttypmods,
						Oid *atttypcolls, int numattrs,
						bool *ok);

static char *jbv_string_get_cstr(JsonbValue *jval);
static bool jbv_to_infunc_datum(JsonbValue *jval, PGFunction func,
								AttrNumber exprnum, const char *argname,
								Datum *datum);
static bool key_in_expr_argnames(JsonbValue *key);
static bool check_all_expr_argnames_valid(JsonbContainer *cont, AttrNumber exprnum);
static Datum array_in_safe(FmgrInfo *array_in, const char *s, Oid typid,
						   int32 typmod, AttrNumber exprnum,
						   const char *element_name, bool *ok);
static Datum import_pg_statistic(Relation pgsd, JsonbContainer *cont,
								 AttrNumber exprnum, FmgrInfo *array_in_fn,
								 Oid typid, int32 typmod, Oid typcoll,
								 bool *pg_statistic_ok);

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
	has.expressions = !PG_ARGISNULL(EXPRESSIONS_ARG);

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
	 * If the object cannot support expressions, we should not have data for
	 * them.
	 */
	if (has.expressions && !enabled.expressions)
	{
		ereport(WARNING,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("cannot specify parameter \"%s\"",
					   extarginfo[EXPRESSIONS_ARG].argname),
				errhint("Extended statistics object \"%s.%s\" does not support statistics of this type.",
						nspname, stxname));

		has.expressions = false;
		success = false;
	}

	/*
	 * Either of these statistic types requires that we supply a semi-filled
	 * VacAttrStatsP array.
	 *
	 * It is not possible to use the existing lookup_var_attr_stats() and
	 * examine_attribute() because these functions will skip attributes where
	 * attstattarget is 0, and we may have statistics data to import for those
	 * attributes.
	 */
	if (has.mcv || has.expressions)
	{
		atttypids = palloc0_array(Oid, numattrs);
		atttypmods = palloc0_array(int32, numattrs);
		atttypcolls = palloc0_array(Oid, numattrs);

		/*
		 * The leading stxkeys are attribute numbers up through numattnums.
		 * These keys must be in ascending AttrNumber order, but we do not
		 * rely on that.
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

	if (has.expressions)
	{
		Datum		datum;
		Relation	pgsd;
		bool		ok = false;

		pgsd = table_open(StatisticRelationId, RowExclusiveLock);

		/*
		 * Generate the expressions array.
		 *
		 * The atttypids, atttypmods, and atttypcolls arrays have all the
		 * regular attributes listed first, so we can pass those arrays with a
		 * start point after the last regular attribute.  There are numexprs
		 * elements remaining.
		 */
		datum = import_expressions(pgsd, numexprs,
								   &atttypids[numattnums],
								   &atttypmods[numattnums],
								   &atttypcolls[numattnums],
								   PG_GETARG_JSONB_P(EXPRESSIONS_ARG),
								   &ok);

		table_close(pgsd, RowExclusiveLock);

		if (ok)
		{
			Assert(datum != (Datum) 0);
			values[Anum_pg_statistic_ext_data_stxdexpr - 1] = datum;
			replaces[Anum_pg_statistic_ext_data_stxdexpr - 1] = true;
			nulls[Anum_pg_statistic_ext_data_stxdexpr - 1] = false;
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
 * Check if key is found in the list of expression argnames.
 */
static bool
key_in_expr_argnames(JsonbValue *key)
{
	Assert(key->type == jbvString);
	for (int i = 0; i < NUM_ATTRIBUTE_STATS_ELEMS; i++)
	{
		if (strncmp(extexprargname[i], key->val.string.val, key->val.string.len) == 0)
			return true;
	}
	return false;
}

/*
 * Verify that all of the keys in the object are valid argnames.
 */
static bool
check_all_expr_argnames_valid(JsonbContainer *cont, AttrNumber exprnum)
{
	bool		all_keys_valid = true;

	JsonbIterator *jbit;
	JsonbIteratorToken jitok;
	JsonbValue	jkey;

	Assert(JsonContainerIsObject(cont));

	jbit = JsonbIteratorInit(cont);

	/* We always start off with a BEGIN OBJECT */
	jitok = JsonbIteratorNext(&jbit, &jkey, false);
	Assert(jitok == WJB_BEGIN_OBJECT);

	while (true)
	{
		JsonbValue	jval;

		jitok = JsonbIteratorNext(&jbit, &jkey, false);

		/*
		 * We have run of keys. This is the only condition where it is
		 * memory-safe to break out of the loop.
		 */
		if (jitok == WJB_END_OBJECT)
			break;

		/* We can only find keys inside an object */
		Assert(jitok == WJB_KEY);
		Assert(jkey.type == jbvString);

		/* A value must follow the key */
		jitok = JsonbIteratorNext(&jbit, &jval, false);
		Assert(jitok == WJB_VALUE);

		/*
		 * If we have already found an invalid key, there is no point in
		 * looking for more, because additional WARNINGs are just clutter. We
		 * must continue iterating over the json to ensure that we clean up
		 * all allocated memory.
		 */
		if (!all_keys_valid)
			continue;

		if (!key_in_expr_argnames(&jkey))
		{
			char	   *bad_element_name = jbv_string_get_cstr(&jkey);

			ereport(WARNING,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("could not import element in expression %d: invalid key name",
						   exprnum));

			pfree(bad_element_name);
			all_keys_valid = false;
		}
	}
	return all_keys_valid;
}

/*
 * Simple conversion of jbvString to cstring
 */
static char *
jbv_string_get_cstr(JsonbValue *jval)
{
	char	   *s;

	Assert(jval->type == jbvString);

	s = palloc0(jval->val.string.len + 1);
	memcpy(s, jval->val.string.val, jval->val.string.len);

	return s;
}

/*
 * Apply a jbvString value to a safe scalar input function.
 */
static bool
jbv_to_infunc_datum(JsonbValue *jval, PGFunction func, AttrNumber exprnum,
					const char *argname, Datum *datum)
{
	ErrorSaveContext escontext = {
		.type = T_ErrorSaveContext,
		.details_wanted = true
	};

	char	   *s = jbv_string_get_cstr(jval);
	bool		ok;

	ok = DirectInputFunctionCallSafe(func, s, InvalidOid, -1,
									 (Node *) &escontext, datum);

	/*
	 * If we got a type import error, use the report generated and add an
	 * error hint before throwing a warning.
	 */
	if (!ok)
	{
		StringInfoData hint_str;

		initStringInfo(&hint_str);
		appendStringInfo(&hint_str,
						 "Element \"%s\" in expression %d could not be parsed.",
						 argname, exprnum);

		escontext.error_data->elevel = WARNING;
		escontext.error_data->hint = hint_str.data;

		ThrowErrorData(escontext.error_data);
		pfree(hint_str.data);
	}

	pfree(s);
	return ok;
}

/*
 * Build an array datum with element type elemtypid from a text datum, used as
 * value of an attribute in a pg_statistic tuple.
 *
 * If an error is encountered, capture it, and reduce the elevel to WARNING.
 *
 * This is an adaptation of statatt_build_stavalues().
 */
static Datum
array_in_safe(FmgrInfo *array_in, const char *s, Oid typid, int32 typmod,
			  AttrNumber exprnum, const char *element_name, bool *ok)
{
	LOCAL_FCINFO(fcinfo, 3);
	Datum		result;

	ErrorSaveContext escontext = {
		.type = T_ErrorSaveContext,
		.details_wanted = true
	};

	*ok = false;
	InitFunctionCallInfoData(*fcinfo, array_in, 3, InvalidOid,
							 (Node *) &escontext, NULL);

	fcinfo->args[0].value = CStringGetDatum(s);
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = ObjectIdGetDatum(typid);
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = Int32GetDatum(typmod);
	fcinfo->args[2].isnull = false;

	result = FunctionCallInvoke(fcinfo);

	/*
	 * If the array_in function returned an error, we will want to report that
	 * ERROR as a WARNING, and add some location context to the error message.
	 * Overwriting the existing hint (if any) is not ideal, and an error
	 * context would only work for level >= ERROR.
	 */
	if (escontext.error_occurred)
	{
		StringInfoData hint_str;

		initStringInfo(&hint_str);
		appendStringInfo(&hint_str,
						 "Element \"%s\" in expression %d could not be parsed.",
						 element_name, exprnum);
		escontext.error_data->elevel = WARNING;
		escontext.error_data->hint = hint_str.data;
		ThrowErrorData(escontext.error_data);
		pfree(hint_str.data);
		return (Datum) 0;
	}

	if (array_contains_nulls(DatumGetArrayTypeP(result)))
	{
		ereport(WARNING,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("could not import element \"%s\" in expression %d: null value found",
					   element_name, exprnum));
		return (Datum) 0;
	}

	*ok = true;
	return result;
}

/*
 * Create a pg_statistic tuple from an expression JSONB container.
 *
 * The pg_statistic tuple is pre-populated with acceptable defaults, therefore
 * even if there is an issue with all of the keys in the container, we can
 * still return a legit tuple datum.
 *
 * Set pg_statistic_ok to true if all of the values found in the container
 * were imported without issue.  pg_statistic_ok is switched to "true" once
 * the full pg_statistic tuple has been built and validated.
 */
static Datum
import_pg_statistic(Relation pgsd, JsonbContainer *cont,
					AttrNumber exprnum, FmgrInfo *array_in_fn,
					Oid typid, int32 typmod, Oid typcoll,
					bool *pg_statistic_ok)
{
	const char *argname = extarginfo[EXPRESSIONS_ARG].argname;
	TypeCacheEntry *typcache;
	Datum		values[Natts_pg_statistic];
	bool		nulls[Natts_pg_statistic];
	bool		replaces[Natts_pg_statistic];
	HeapTuple	pgstup = NULL;
	Datum		pgstdat = (Datum) 0;
	Oid			elemtypid = InvalidOid;
	Oid			elemeqopr = InvalidOid;
	bool		found[NUM_ATTRIBUTE_STATS_ELEMS] = {0};
	JsonbValue	val[NUM_ATTRIBUTE_STATS_ELEMS] = {0};

	/* Assume the worst by default. */
	*pg_statistic_ok = false;

	if (!JsonContainerIsObject(cont))
	{
		ereport(WARNING,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("could not parse \"%s\": invalid element in expression %d",
					   argname, exprnum));
		goto pg_statistic_error;
	}

	/*
	 * Loop through all keys that we need to look up.  If any value found is
	 * neither a string nor a NULL, there is not much we can do, so just give
	 * on the entire tuple for this expression.
	 */
	for (int i = 0; i < NUM_ATTRIBUTE_STATS_ELEMS; i++)
	{
		const char *s = extexprargname[i];
		int			len = strlen(s);

		if (getKeyJsonValueFromContainer(cont, s, len, &val[i]) == NULL)
			continue;

		switch (val[i].type)
		{
			case jbvString:
				found[i] = true;
				break;

			case jbvNull:
				break;

			default:
				ereport(WARNING,
						errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("could not parse \"%s\": invalid element in expression %d", argname, exprnum),
						errhint("Value of element \"%s\" must be type a null or a string.", s));
				goto pg_statistic_error;
		}
	}

	/* Look for invalid keys */
	if (!check_all_expr_argnames_valid(cont, exprnum))
		goto pg_statistic_error;

	/*
	 * There are two arg pairs, MCV+MCF and MCEV+MCEF.  Both values must
	 * either be found or not be found.  Any disagreement is a warning.  Once
	 * we have ruled out disagreeing pairs, we can use either found flag as a
	 * proxy for the other.
	 */
	if (found[MOST_COMMON_VALS_ELEM] != found[MOST_COMMON_FREQS_ELEM])
	{
		ereport(WARNING,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("could not parse \"%s\": invalid element in expression %d",
					   argname, exprnum),
				errhint("\"%s\" and \"%s\" must be both either strings or nulls.",
						extexprargname[MOST_COMMON_VALS_ELEM],
						extexprargname[MOST_COMMON_FREQS_ELEM]));
		goto pg_statistic_error;
	}
	if (found[MOST_COMMON_ELEMS_ELEM] != found[MOST_COMMON_ELEM_FREQS_ELEM])
	{
		ereport(WARNING,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("could not parse \"%s\": invalid element in expression %d",
					   argname, exprnum),
				errhint("\"%s\" and \"%s\" must be both either strings or nulls.",
						extexprargname[MOST_COMMON_ELEMS_ELEM],
						extexprargname[MOST_COMMON_ELEM_FREQS_ELEM]));
		goto pg_statistic_error;
	}

	/*
	 * Range types may expect three values to be set.  All three of them must
	 * either be found or not be found.  Any disagreement is a warning.
	 */
	if (found[RANGE_LENGTH_HISTOGRAM_ELEM] != found[RANGE_EMPTY_FRAC_ELEM] ||
		found[RANGE_LENGTH_HISTOGRAM_ELEM] != found[RANGE_BOUNDS_HISTOGRAM_ELEM])
	{
		ereport(WARNING,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("could not parse \"%s\": invalid element in expression %d",
					   argname, exprnum),
				errhint("\"%s\", \"%s\", and \"%s\" must be all either strings or all nulls.",
						extexprargname[RANGE_LENGTH_HISTOGRAM_ELEM],
						extexprargname[RANGE_EMPTY_FRAC_ELEM],
						extexprargname[RANGE_BOUNDS_HISTOGRAM_ELEM]));
		goto pg_statistic_error;
	}

	/* This finds the right operators even if atttypid is a domain */
	typcache = lookup_type_cache(typid, TYPECACHE_LT_OPR | TYPECACHE_EQ_OPR);

	statatt_init_empty_tuple(InvalidOid, InvalidAttrNumber, false,
							 values, nulls, replaces);

	/*
	 * Special case: collation for tsvector is DEFAULT_COLLATION_OID. See
	 * compute_tsvector_stats().
	 */
	if (typid == TSVECTOROID)
		typcoll = DEFAULT_COLLATION_OID;

	/*
	 * We only need to fetch element type and eq operator if we have a stat of
	 * type MCELEM or DECHIST, otherwise the values are unnecessary and not
	 * meaningful.
	 */
	if (found[MOST_COMMON_ELEMS_ELEM] || found[ELEM_COUNT_HISTOGRAM_ELEM])
	{
		if (!statatt_get_elem_type(typid, typcache->typtype,
								   &elemtypid, &elemeqopr))
		{
			ereport(WARNING,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("could not parse \"%s\": invalid element type in expression %d",
						   argname, exprnum));
			goto pg_statistic_error;
		}
	}

	/*
	 * These three fields can only be set if dealing with a range or
	 * multi-range type.
	 */
	if (found[RANGE_LENGTH_HISTOGRAM_ELEM] ||
		found[RANGE_EMPTY_FRAC_ELEM] ||
		found[RANGE_BOUNDS_HISTOGRAM_ELEM])
	{
		if (typcache->typtype != TYPTYPE_RANGE &&
			typcache->typtype != TYPTYPE_MULTIRANGE)
		{
			ereport(WARNING,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("could not parse \"%s\": invalid data in expression %d",
						   argname, exprnum),
					errhint("\"%s\", \"%s\", and \"%s\" can only be set for a range type.",
							extexprargname[RANGE_LENGTH_HISTOGRAM_ELEM],
							extexprargname[RANGE_EMPTY_FRAC_ELEM],
							extexprargname[RANGE_BOUNDS_HISTOGRAM_ELEM]));
			goto pg_statistic_error;
		}
	}

	/* null_frac */
	if (found[NULL_FRAC_ELEM])
	{
		Datum		datum;

		if (jbv_to_infunc_datum(&val[NULL_FRAC_ELEM], float4in, exprnum,
								extexprargname[NULL_FRAC_ELEM], &datum))
			values[Anum_pg_statistic_stanullfrac - 1] = datum;
		else
			goto pg_statistic_error;
	}

	/* avg_width */
	if (found[AVG_WIDTH_ELEM])
	{
		Datum		datum;

		if (jbv_to_infunc_datum(&val[AVG_WIDTH_ELEM], int4in, exprnum,
								extexprargname[AVG_WIDTH_ELEM], &datum))
			values[Anum_pg_statistic_stawidth - 1] = datum;
		else
			goto pg_statistic_error;
	}

	/* n_distinct */
	if (found[N_DISTINCT_ELEM])
	{
		Datum		datum;

		if (jbv_to_infunc_datum(&val[N_DISTINCT_ELEM], float4in, exprnum,
								extexprargname[N_DISTINCT_ELEM], &datum))
			values[Anum_pg_statistic_stadistinct - 1] = datum;
		else
			goto pg_statistic_error;
	}

	/*
	 * The STAKIND statistics are the same as the ones found in attribute
	 * stats.  However, these are all derived from json strings, whereas the
	 * ones derived for attribute stats are a mix of datatypes.  This limits
	 * the opportunities for code sharing between the two.
	 *
	 * Some statistic kinds have both a stanumbers and a stavalues components.
	 * In those cases, both values must either be NOT NULL or both NULL, and
	 * if they aren't then we need to reject that stakind completely.
	 * Currently we go a step further and reject the expression array
	 * completely.
	 */

	if (found[MOST_COMMON_VALS_ELEM])
	{
		Datum		stavalues;
		Datum		stanumbers;
		bool		val_ok = false;
		bool		num_ok = false;
		char	   *s;

		s = jbv_string_get_cstr(&val[MOST_COMMON_VALS_ELEM]);
		stavalues = array_in_safe(array_in_fn, s, typid, typmod, exprnum,
								  extexprargname[MOST_COMMON_VALS_ELEM],
								  &val_ok);

		pfree(s);

		s = jbv_string_get_cstr(&val[MOST_COMMON_FREQS_ELEM]);
		stanumbers = array_in_safe(array_in_fn, s, FLOAT4OID, -1, exprnum,
								   extexprargname[MOST_COMMON_FREQS_ELEM],
								   &num_ok);
		pfree(s);

		/* Only set the slot if both datums have been built */
		if (val_ok && num_ok)
			statatt_set_slot(values, nulls, replaces,
							 STATISTIC_KIND_MCV,
							 typcache->eq_opr, typcoll,
							 stanumbers, false, stavalues, false);
		else
			goto pg_statistic_error;
	}

	/* STATISTIC_KIND_HISTOGRAM */
	if (found[HISTOGRAM_BOUNDS_ELEM])
	{
		Datum		stavalues;
		bool		val_ok = false;
		char	   *s = jbv_string_get_cstr(&val[HISTOGRAM_BOUNDS_ELEM]);

		stavalues = array_in_safe(array_in_fn, s, typid, typmod, exprnum,
								  extexprargname[HISTOGRAM_BOUNDS_ELEM],
								  &val_ok);
		pfree(s);

		if (val_ok)
			statatt_set_slot(values, nulls, replaces,
							 STATISTIC_KIND_HISTOGRAM,
							 typcache->lt_opr, typcoll,
							 0, true, stavalues, false);
		else
			goto pg_statistic_error;
	}

	/* STATISTIC_KIND_CORRELATION */
	if (found[CORRELATION_ELEM])
	{
		Datum		corr[] = {(Datum) 0};

		if (jbv_to_infunc_datum(&val[CORRELATION_ELEM], float4in, exprnum,
								extexprargname[CORRELATION_ELEM], &corr[0]))
		{
			ArrayType  *arry = construct_array_builtin(corr, 1, FLOAT4OID);
			Datum		stanumbers = PointerGetDatum(arry);

			statatt_set_slot(values, nulls, replaces,
							 STATISTIC_KIND_CORRELATION,
							 typcache->lt_opr, typcoll,
							 stanumbers, false, 0, true);
		}
		else
			goto pg_statistic_error;
	}

	/* STATISTIC_KIND_MCELEM */
	if (found[MOST_COMMON_ELEMS_ELEM])
	{
		Datum		stavalues;
		Datum		stanumbers;
		bool		val_ok = false;
		bool		num_ok = false;
		char	   *s;

		s = jbv_string_get_cstr(&val[MOST_COMMON_ELEMS_ELEM]);
		stavalues = array_in_safe(array_in_fn, s, elemtypid, typmod, exprnum,
								  extexprargname[MOST_COMMON_ELEMS_ELEM],
								  &val_ok);
		pfree(s);


		s = jbv_string_get_cstr(&val[MOST_COMMON_ELEM_FREQS_ELEM]);
		stanumbers = array_in_safe(array_in_fn, s, FLOAT4OID, -1, exprnum,
								   extexprargname[MOST_COMMON_ELEM_FREQS_ELEM],
								   &num_ok);
		pfree(s);

		/* Only set the slot if both datums have been built */
		if (val_ok && num_ok)
			statatt_set_slot(values, nulls, replaces,
							 STATISTIC_KIND_MCELEM,
							 elemeqopr, typcoll,
							 stanumbers, false, stavalues, false);
		else
			goto pg_statistic_error;
	}

	/* STATISTIC_KIND_DECHIST */
	if (found[ELEM_COUNT_HISTOGRAM_ELEM])
	{
		Datum		stanumbers;
		bool		num_ok = false;
		char	   *s;

		s = jbv_string_get_cstr(&val[ELEM_COUNT_HISTOGRAM_ELEM]);
		stanumbers = array_in_safe(array_in_fn, s, FLOAT4OID, -1, exprnum,
								   extexprargname[ELEM_COUNT_HISTOGRAM_ELEM],
								   &num_ok);
		pfree(s);

		if (num_ok)
			statatt_set_slot(values, nulls, replaces, STATISTIC_KIND_DECHIST,
							 elemeqopr, typcoll, stanumbers, false, 0, true);
		else
			goto pg_statistic_error;
	}

	/*
	 * STATISTIC_KIND_BOUNDS_HISTOGRAM
	 *
	 * This stakind appears before STATISTIC_KIND_RANGE_LENGTH_HISTOGRAM even
	 * though it is numerically greater, and all other stakinds appear in
	 * numerical order.
	 */
	if (found[RANGE_BOUNDS_HISTOGRAM_ELEM])
	{
		Datum		stavalues;
		bool		val_ok = false;
		char	   *s;
		Oid			rtypid = typid;

		/*
		 * If it's a multirange, step down to the range type, as is done by
		 * multirange_typanalyze().
		 */
		if (type_is_multirange(typid))
			rtypid = get_multirange_range(typid);

		s = jbv_string_get_cstr(&val[RANGE_BOUNDS_HISTOGRAM_ELEM]);

		stavalues = array_in_safe(array_in_fn, s, rtypid, typmod, exprnum,
								  extexprargname[RANGE_BOUNDS_HISTOGRAM_ELEM],
								  &val_ok);

		if (val_ok)
			statatt_set_slot(values, nulls, replaces,
							 STATISTIC_KIND_BOUNDS_HISTOGRAM,
							 InvalidOid, InvalidOid,
							 0, true, stavalues, false);
		else
			goto pg_statistic_error;
	}

	/* STATISTIC_KIND_RANGE_LENGTH_HISTOGRAM */
	if (found[RANGE_LENGTH_HISTOGRAM_ELEM])
	{
		Datum		empty_frac[] = {(Datum) 0};
		Datum		stavalues;
		Datum		stanumbers;
		bool		val_ok = false;
		char	   *s;

		if (jbv_to_infunc_datum(&val[RANGE_EMPTY_FRAC_ELEM], float4in, exprnum,
								extexprargname[RANGE_EMPTY_FRAC_ELEM], &empty_frac[0]))
		{
			ArrayType  *arry = construct_array_builtin(empty_frac, 1, FLOAT4OID);

			stanumbers = PointerGetDatum(arry);
		}
		else
			goto pg_statistic_error;

		s = jbv_string_get_cstr(&val[RANGE_LENGTH_HISTOGRAM_ELEM]);
		stavalues = array_in_safe(array_in_fn, s, FLOAT8OID, -1, exprnum,
								  extexprargname[RANGE_LENGTH_HISTOGRAM_ELEM],
								  &val_ok);

		if (val_ok)
			statatt_set_slot(values, nulls, replaces,
							 STATISTIC_KIND_RANGE_LENGTH_HISTOGRAM,
							 Float8LessOperator, InvalidOid,
							 stanumbers, false, stavalues, false);
		else
			goto pg_statistic_error;
	}

	pgstup = heap_form_tuple(RelationGetDescr(pgsd), values, nulls);
	pgstdat = heap_copy_tuple_as_datum(pgstup, RelationGetDescr(pgsd));

	heap_freetuple(pgstup);

	*pg_statistic_ok = true;

	return pgstdat;

pg_statistic_error:
	return (Datum) 0;
}

/*
 * Create the stxdexpr datum, which is an array of pg_statistic rows with all
 * of the object identification fields left at defaults, using the json array
 * of objects/nulls referenced against the datatypes for the expressions.
 *
 * The exprs_is_perfect will be set to true if all pg_statistic rows were
 * imported cleanly.  If any of them experienced a problem (and thus were
 * set as if they were null), then the expression is kept but exprs_is_perfect
 * will be marked as false.
 *
 * This datum is needed to fill out a complete pg_statistic_ext_data tuple.
 */
static Datum
import_expressions(Relation pgsd, int numexprs,
				   Oid *atttypids, int32 *atttypmods,
				   Oid *atttypcolls, Jsonb *exprs_jsonb,
				   bool *exprs_is_perfect)
{
	const char *argname = extarginfo[EXPRESSIONS_ARG].argname;
	Oid			pgstypoid = get_rel_type_id(StatisticRelationId);
	ArrayBuildState *astate = NULL;
	Datum		result = (Datum) 0;
	int			num_import_ok = 0;
	JsonbContainer *root;
	int			num_root_elements;

	FmgrInfo	array_in_fn;

	*exprs_is_perfect = false;

	/* Json schema must be [{expr},...] */
	if (!JB_ROOT_IS_ARRAY(exprs_jsonb))
	{
		ereport(WARNING,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("could not parse \"%s\": root-level array required", argname));
		goto exprs_error;
	}

	root = &exprs_jsonb->root;

	/*
	 * The number of elements in the array must match the number of
	 * expressions in the stats object definition.
	 */
	num_root_elements = JsonContainerSize(root);
	if (numexprs != num_root_elements)
	{
		ereport(WARNING,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("could not parse \"%s\": incorrect number of elements (%d required)",
					   argname, num_root_elements));
		goto exprs_error;
	}

	fmgr_info(F_ARRAY_IN, &array_in_fn);

	/*
	 * Iterate over each expected expression object in the array.  Some of
	 * them could be null.  If the element is a completely wrong data type,
	 * give a WARNING and then treat the element like a NULL element in the
	 * result array.
	 *
	 * Each expression *MUST* have a value appended in the result pg_statistic
	 * array.
	 */
	for (int i = 0; i < numexprs; i++)
	{
		Datum		pgstdat = (Datum) 0;
		bool		isnull = false;
		AttrNumber	exprattnum = -1 - i;

		JsonbValue *elem = getIthJsonbValueFromContainer(root, i);

		switch (elem->type)
		{
			case jbvBinary:
				{
					bool		sta_ok = false;

					/* a real stats object */
					pgstdat = import_pg_statistic(pgsd, elem->val.binary.data,
												  exprattnum, &array_in_fn,
												  atttypids[i], atttypmods[i],
												  atttypcolls[i], &sta_ok);

					/*
					 * If some incorrect data has been found, assign NULL for
					 * this expression as a mean to give up.
					 */
					if (sta_ok)
						num_import_ok++;
					else
					{
						isnull = true;
						pgstdat = (Datum) 0;
					}
				}
				break;

			case jbvNull:
				/* NULL placeholder for invalid data, still fine */
				isnull = true;
				num_import_ok++;
				break;

			default:
				/* cannot possibly be valid */
				ereport(WARNING,
						errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("could not parse \"%s\": invalid element in expression %d",
							   argname, exprattnum));
				goto exprs_error;
		}

		astate = accumArrayResult(astate, pgstdat, isnull, pgstypoid,
								  CurrentMemoryContext);
	}

	/*
	 * The expressions datum is perfect *if and only if* all of the
	 * pg_statistic elements were also ok, for a number of elements equal to
	 * the number of expressions.  Anything else means a failure in restoring
	 * the data of this statistics object.
	 */
	*exprs_is_perfect = (num_import_ok == numexprs);

	if (astate != NULL)
		result = makeArrayResult(astate, CurrentMemoryContext);

	return result;

exprs_error:
	if (astate != NULL)
		pfree(astate);
	return (Datum) 0;
};

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

	/* Is there already a pg_statistic_ext_data tuple for this attribute? */
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
