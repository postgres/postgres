/*-------------------------------------------------------------------------
 * attribute_stats.c
 *
 *	  PostgreSQL relation attribute statistics manipulation.
 *
 * Code supporting the direct import of relation attribute statistics, similar
 * to what is done by the ANALYZE command.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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
#include "catalog/pg_collation.h"
#include "catalog/pg_operator.h"
#include "nodes/nodeFuncs.h"
#include "statistics/statistics.h"
#include "statistics/stat_utils.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#define DEFAULT_NULL_FRAC      Float4GetDatum(0.0)
#define DEFAULT_AVG_WIDTH      Int32GetDatum(0) /* unknown */
#define DEFAULT_N_DISTINCT     Float4GetDatum(0.0)	/* unknown */

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
static Node *get_attr_expr(Relation rel, int attnum);
static void get_attr_stat_type(Oid reloid, AttrNumber attnum,
							   Oid *atttypid, int32 *atttypmod,
							   char *atttyptype, Oid *atttypcoll,
							   Oid *eq_opr, Oid *lt_opr);
static bool get_elem_stat_type(Oid atttypid, char atttyptype,
							   Oid *elemtypid, Oid *elem_eq_opr);
static Datum text_to_stavalues(const char *staname, FmgrInfo *array_in, Datum d,
							   Oid typid, int32 typmod, bool *ok);
static void set_stats_slot(Datum *values, bool *nulls, bool *replaces,
						   int16 stakind, Oid staop, Oid stacoll,
						   Datum stanumbers, bool stanumbers_isnull,
						   Datum stavalues, bool stavalues_isnull);
static void upsert_pg_statistic(Relation starel, HeapTuple oldtup,
								Datum *values, bool *nulls, bool *replaces);
static bool delete_pg_statistic(Oid reloid, AttrNumber attnum, bool stainherit);
static void init_empty_stats_tuple(Oid reloid, int16 attnum, bool inherited,
								   Datum *values, bool *nulls, bool *replaces);

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

	reloid = stats_lookup_relid(nspname, relname);

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("Statistics cannot be modified during recovery.")));

	/* lock before looking up attribute */
	stats_lock_check_privileges(reloid);

	/* user can specify either attname or attnum, but not both */
	if (!PG_ARGISNULL(ATTNAME_ARG))
	{
		if (!PG_ARGISNULL(ATTNUM_ARG))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("cannot specify both attname and attnum")));
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
				 errmsg("must specify either attname or attnum")));
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
	get_attr_stat_type(reloid, attnum,
					   &atttypid, &atttypmod,
					   &atttyptype, &atttypcoll,
					   &eq_opr, &lt_opr);

	/* if needed, derive element type */
	if (do_mcelem || do_dechist)
	{
		if (!get_elem_stat_type(atttypid, atttyptype,
								&elemtypid, &elem_eq_opr))
		{
			ereport(WARNING,
					(errmsg("unable to determine element type of attribute \"%s\"", attname),
					 errdetail("Cannot set STATISTIC_KIND_MCELEM or STATISTIC_KIND_DECHIST.")));
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
				 errmsg("could not determine less-than operator for attribute \"%s\"", attname),
				 errdetail("Cannot set STATISTIC_KIND_HISTOGRAM or STATISTIC_KIND_CORRELATION.")));

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
				 errmsg("attribute \"%s\" is not a range type", attname),
				 errdetail("Cannot set STATISTIC_KIND_RANGE_LENGTH_HISTOGRAM or STATISTIC_KIND_BOUNDS_HISTOGRAM.")));

		do_bounds_histogram = false;
		do_range_length_histogram = false;
		result = false;
	}

	fmgr_info(F_ARRAY_IN, &array_in_fn);

	starel = table_open(StatisticRelationId, RowExclusiveLock);

	statup = SearchSysCache3(STATRELATTINH, reloid, attnum, inherited);

	/* initialize from existing tuple if exists */
	if (HeapTupleIsValid(statup))
		heap_deform_tuple(statup, RelationGetDescr(starel), values, nulls);
	else
		init_empty_stats_tuple(reloid, attnum, inherited, values, nulls,
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
		Datum		stavalues = text_to_stavalues("most_common_vals",
												  &array_in_fn,
												  PG_GETARG_DATUM(MOST_COMMON_VALS_ARG),
												  atttypid, atttypmod,
												  &converted);

		if (converted)
		{
			set_stats_slot(values, nulls, replaces,
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

		stavalues = text_to_stavalues("histogram_bounds",
									  &array_in_fn,
									  PG_GETARG_DATUM(HISTOGRAM_BOUNDS_ARG),
									  atttypid, atttypmod,
									  &converted);

		if (converted)
		{
			set_stats_slot(values, nulls, replaces,
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

		set_stats_slot(values, nulls, replaces,
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

		stavalues = text_to_stavalues("most_common_elems",
									  &array_in_fn,
									  PG_GETARG_DATUM(MOST_COMMON_ELEMS_ARG),
									  elemtypid, atttypmod,
									  &converted);

		if (converted)
		{
			set_stats_slot(values, nulls, replaces,
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

		set_stats_slot(values, nulls, replaces,
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

		stavalues = text_to_stavalues("range_bounds_histogram",
									  &array_in_fn,
									  PG_GETARG_DATUM(RANGE_BOUNDS_HISTOGRAM_ARG),
									  atttypid, atttypmod,
									  &converted);

		if (converted)
		{
			set_stats_slot(values, nulls, replaces,
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

		stavalues = text_to_stavalues("range_length_histogram",
									  &array_in_fn,
									  PG_GETARG_DATUM(RANGE_LENGTH_HISTOGRAM_ARG),
									  FLOAT8OID, 0, &converted);

		if (converted)
		{
			set_stats_slot(values, nulls, replaces,
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
 * If this relation is an index and that index has expressions in it, and
 * the attnum specified is known to be an expression, then we must walk
 * the list attributes up to the specified attnum to get the right
 * expression.
 */
static Node *
get_attr_expr(Relation rel, int attnum)
{
	List	   *index_exprs;
	ListCell   *indexpr_item;

	/* relation is not an index */
	if (rel->rd_rel->relkind != RELKIND_INDEX &&
		rel->rd_rel->relkind != RELKIND_PARTITIONED_INDEX)
		return NULL;

	index_exprs = RelationGetIndexExpressions(rel);

	/* index has no expressions to give */
	if (index_exprs == NIL)
		return NULL;

	/*
	 * The index attnum points directly to a relation attnum, then it's not an
	 * expression attribute.
	 */
	if (rel->rd_index->indkey.values[attnum - 1] != 0)
		return NULL;

	indexpr_item = list_head(rel->rd_indexprs);

	for (int i = 0; i < attnum - 1; i++)
		if (rel->rd_index->indkey.values[i] == 0)
			indexpr_item = lnext(rel->rd_indexprs, indexpr_item);

	if (indexpr_item == NULL)	/* shouldn't happen */
		elog(ERROR, "too few entries in indexprs list");

	return (Node *) lfirst(indexpr_item);
}

/*
 * Derive type information from the attribute.
 */
static void
get_attr_stat_type(Oid reloid, AttrNumber attnum,
				   Oid *atttypid, int32 *atttypmod,
				   char *atttyptype, Oid *atttypcoll,
				   Oid *eq_opr, Oid *lt_opr)
{
	Relation	rel = relation_open(reloid, AccessShareLock);
	Form_pg_attribute attr;
	HeapTuple	atup;
	Node	   *expr;
	TypeCacheEntry *typcache;

	atup = SearchSysCache2(ATTNUM, ObjectIdGetDatum(reloid),
						   Int16GetDatum(attnum));

	/* Attribute not found */
	if (!HeapTupleIsValid(atup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("attribute %d of relation \"%s\" does not exist",
						attnum, RelationGetRelationName(rel))));

	attr = (Form_pg_attribute) GETSTRUCT(atup);

	if (attr->attisdropped)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("attribute %d of relation \"%s\" does not exist",
						attnum, RelationGetRelationName(rel))));

	expr = get_attr_expr(rel, attr->attnum);

	/*
	 * When analyzing an expression index, believe the expression tree's type
	 * not the column datatype --- the latter might be the opckeytype storage
	 * type of the opclass, which is not interesting for our purposes. This
	 * mimics the behavior of examine_attribute().
	 */
	if (expr == NULL)
	{
		*atttypid = attr->atttypid;
		*atttypmod = attr->atttypmod;
		*atttypcoll = attr->attcollation;
	}
	else
	{
		*atttypid = exprType(expr);
		*atttypmod = exprTypmod(expr);

		if (OidIsValid(attr->attcollation))
			*atttypcoll = attr->attcollation;
		else
			*atttypcoll = exprCollation(expr);
	}
	ReleaseSysCache(atup);

	/*
	 * If it's a multirange, step down to the range type, as is done by
	 * multirange_typanalyze().
	 */
	if (type_is_multirange(*atttypid))
		*atttypid = get_multirange_range(*atttypid);

	/* finds the right operators even if atttypid is a domain */
	typcache = lookup_type_cache(*atttypid, TYPECACHE_LT_OPR | TYPECACHE_EQ_OPR);
	*atttyptype = typcache->typtype;
	*eq_opr = typcache->eq_opr;
	*lt_opr = typcache->lt_opr;

	/*
	 * Special case: collation for tsvector is DEFAULT_COLLATION_OID. See
	 * compute_tsvector_stats().
	 */
	if (*atttypid == TSVECTOROID)
		*atttypcoll = DEFAULT_COLLATION_OID;

	relation_close(rel, NoLock);
}

/*
 * Derive element type information from the attribute type.
 */
static bool
get_elem_stat_type(Oid atttypid, char atttyptype,
				   Oid *elemtypid, Oid *elem_eq_opr)
{
	TypeCacheEntry *elemtypcache;

	if (atttypid == TSVECTOROID)
	{
		/*
		 * Special case: element type for tsvector is text. See
		 * compute_tsvector_stats().
		 */
		*elemtypid = TEXTOID;
	}
	else
	{
		/* find underlying element type through any domain */
		*elemtypid = get_base_element_type(atttypid);
	}

	if (!OidIsValid(*elemtypid))
		return false;

	/* finds the right operator even if elemtypid is a domain */
	elemtypcache = lookup_type_cache(*elemtypid, TYPECACHE_EQ_OPR);
	if (!OidIsValid(elemtypcache->eq_opr))
		return false;

	*elem_eq_opr = elemtypcache->eq_opr;

	return true;
}

/*
 * Cast a text datum into an array with element type elemtypid.
 *
 * If an error is encountered, capture it and re-throw a WARNING, and set ok
 * to false. If the resulting array contains NULLs, raise a WARNING and set ok
 * to false. Otherwise, set ok to true.
 */
static Datum
text_to_stavalues(const char *staname, FmgrInfo *array_in, Datum d, Oid typid,
				  int32 typmod, bool *ok)
{
	LOCAL_FCINFO(fcinfo, 8);
	char	   *s;
	Datum		result;
	ErrorSaveContext escontext = {T_ErrorSaveContext};

	escontext.details_wanted = true;

	s = TextDatumGetCString(d);

	InitFunctionCallInfoData(*fcinfo, array_in, 3, InvalidOid,
							 (Node *) &escontext, NULL);

	fcinfo->args[0].value = CStringGetDatum(s);
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = ObjectIdGetDatum(typid);
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = Int32GetDatum(typmod);
	fcinfo->args[2].isnull = false;

	result = FunctionCallInvoke(fcinfo);

	pfree(s);

	if (escontext.error_occurred)
	{
		escontext.error_data->elevel = WARNING;
		ThrowErrorData(escontext.error_data);
		*ok = false;
		return (Datum) 0;
	}

	if (array_contains_nulls(DatumGetArrayTypeP(result)))
	{
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" array cannot contain NULL values", staname)));
		*ok = false;
		return (Datum) 0;
	}

	*ok = true;

	return result;
}

/*
 * Find and update the slot with the given stakind, or use the first empty
 * slot.
 */
static void
set_stats_slot(Datum *values, bool *nulls, bool *replaces,
			   int16 stakind, Oid staop, Oid stacoll,
			   Datum stanumbers, bool stanumbers_isnull,
			   Datum stavalues, bool stavalues_isnull)
{
	int			slotidx;
	int			first_empty = -1;
	AttrNumber	stakind_attnum;
	AttrNumber	staop_attnum;
	AttrNumber	stacoll_attnum;

	/* find existing slot with given stakind */
	for (slotidx = 0; slotidx < STATISTIC_NUM_SLOTS; slotidx++)
	{
		stakind_attnum = Anum_pg_statistic_stakind1 - 1 + slotidx;

		if (first_empty < 0 &&
			DatumGetInt16(values[stakind_attnum]) == 0)
			first_empty = slotidx;
		if (DatumGetInt16(values[stakind_attnum]) == stakind)
			break;
	}

	if (slotidx >= STATISTIC_NUM_SLOTS && first_empty >= 0)
		slotidx = first_empty;

	if (slotidx >= STATISTIC_NUM_SLOTS)
		ereport(ERROR,
				(errmsg("maximum number of statistics slots exceeded: %d",
						slotidx + 1)));

	stakind_attnum = Anum_pg_statistic_stakind1 - 1 + slotidx;
	staop_attnum = Anum_pg_statistic_staop1 - 1 + slotidx;
	stacoll_attnum = Anum_pg_statistic_stacoll1 - 1 + slotidx;

	if (DatumGetInt16(values[stakind_attnum]) != stakind)
	{
		values[stakind_attnum] = Int16GetDatum(stakind);
		replaces[stakind_attnum] = true;
	}
	if (DatumGetObjectId(values[staop_attnum]) != staop)
	{
		values[staop_attnum] = ObjectIdGetDatum(staop);
		replaces[staop_attnum] = true;
	}
	if (DatumGetObjectId(values[stacoll_attnum]) != stacoll)
	{
		values[stacoll_attnum] = ObjectIdGetDatum(stacoll);
		replaces[stacoll_attnum] = true;
	}
	if (!stanumbers_isnull)
	{
		values[Anum_pg_statistic_stanumbers1 - 1 + slotidx] = stanumbers;
		nulls[Anum_pg_statistic_stanumbers1 - 1 + slotidx] = false;
		replaces[Anum_pg_statistic_stanumbers1 - 1 + slotidx] = true;
	}
	if (!stavalues_isnull)
	{
		values[Anum_pg_statistic_stavalues1 - 1 + slotidx] = stavalues;
		nulls[Anum_pg_statistic_stavalues1 - 1 + slotidx] = false;
		replaces[Anum_pg_statistic_stavalues1 - 1 + slotidx] = true;
	}
}

/*
 * Upsert the pg_statistic record.
 */
static void
upsert_pg_statistic(Relation starel, HeapTuple oldtup,
					Datum *values, bool *nulls, bool *replaces)
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
 * Initialize values and nulls for a new stats tuple.
 */
static void
init_empty_stats_tuple(Oid reloid, int16 attnum, bool inherited,
					   Datum *values, bool *nulls, bool *replaces)
{
	memset(nulls, true, sizeof(bool) * Natts_pg_statistic);
	memset(replaces, true, sizeof(bool) * Natts_pg_statistic);

	/* must initialize non-NULL attributes */

	values[Anum_pg_statistic_starelid - 1] = ObjectIdGetDatum(reloid);
	nulls[Anum_pg_statistic_starelid - 1] = false;
	values[Anum_pg_statistic_staattnum - 1] = Int16GetDatum(attnum);
	nulls[Anum_pg_statistic_staattnum - 1] = false;
	values[Anum_pg_statistic_stainherit - 1] = BoolGetDatum(inherited);
	nulls[Anum_pg_statistic_stainherit - 1] = false;

	values[Anum_pg_statistic_stanullfrac - 1] = DEFAULT_NULL_FRAC;
	nulls[Anum_pg_statistic_stanullfrac - 1] = false;
	values[Anum_pg_statistic_stawidth - 1] = DEFAULT_AVG_WIDTH;
	nulls[Anum_pg_statistic_stawidth - 1] = false;
	values[Anum_pg_statistic_stadistinct - 1] = DEFAULT_N_DISTINCT;
	nulls[Anum_pg_statistic_stadistinct - 1] = false;

	/* initialize stakind, staop, and stacoll slots */
	for (int slotnum = 0; slotnum < STATISTIC_NUM_SLOTS; slotnum++)
	{
		values[Anum_pg_statistic_stakind1 + slotnum - 1] = (Datum) 0;
		nulls[Anum_pg_statistic_stakind1 + slotnum - 1] = false;
		values[Anum_pg_statistic_staop1 + slotnum - 1] = InvalidOid;
		nulls[Anum_pg_statistic_staop1 + slotnum - 1] = false;
		values[Anum_pg_statistic_stacoll1 + slotnum - 1] = InvalidOid;
		nulls[Anum_pg_statistic_stacoll1 + slotnum - 1] = false;
	}
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

	stats_check_required_arg(fcinfo, cleararginfo, C_ATTRELSCHEMA_ARG);
	stats_check_required_arg(fcinfo, cleararginfo, C_ATTRELNAME_ARG);
	stats_check_required_arg(fcinfo, cleararginfo, C_ATTNAME_ARG);
	stats_check_required_arg(fcinfo, cleararginfo, C_INHERITED_ARG);

	nspname = TextDatumGetCString(PG_GETARG_DATUM(C_ATTRELSCHEMA_ARG));
	relname = TextDatumGetCString(PG_GETARG_DATUM(C_ATTRELNAME_ARG));

	reloid = stats_lookup_relid(nspname, relname);

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("Statistics cannot be modified during recovery.")));

	stats_lock_check_privileges(reloid);

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
