/*-------------------------------------------------------------------------
 * stat_utils.c
 *
 *	  PostgreSQL statistics manipulation utilities.
 *
 * Code supporting the direct manipulation of statistics.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *       src/backend/statistics/stat_utils.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_database.h"
#include "catalog/pg_statistic.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "statistics/stat_utils.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

/* Default values assigned to new pg_statistic tuples. */
#define DEFAULT_STATATT_NULL_FRAC      Float4GetDatum(0.0)	/* stanullfrac */
#define DEFAULT_STATATT_AVG_WIDTH      Int32GetDatum(0) /* stawidth, same as
														 * unknown */
#define DEFAULT_STATATT_N_DISTINCT     Float4GetDatum(0.0)	/* stadistinct, same as
															 * unknown */

static Node *statatt_get_index_expr(Relation rel, int attnum);

/*
 * Ensure that a given argument is not null.
 */
void
stats_check_required_arg(FunctionCallInfo fcinfo,
						 struct StatsArgInfo *arginfo,
						 int argnum)
{
	if (PG_ARGISNULL(argnum))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("argument \"%s\" must not be null",
						arginfo[argnum].argname)));
}

/*
 * Check that argument is either NULL or a one dimensional array with no
 * NULLs.
 *
 * If a problem is found, emit a WARNING, and return false. Otherwise return
 * true.
 */
bool
stats_check_arg_array(FunctionCallInfo fcinfo,
					  struct StatsArgInfo *arginfo,
					  int argnum)
{
	ArrayType  *arr;

	if (PG_ARGISNULL(argnum))
		return true;

	arr = DatumGetArrayTypeP(PG_GETARG_DATUM(argnum));

	if (ARR_NDIM(arr) != 1)
	{
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("argument \"%s\" must not be a multidimensional array",
						arginfo[argnum].argname)));
		return false;
	}

	if (array_contains_nulls(arr))
	{
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("argument \"%s\" array must not contain null values",
						arginfo[argnum].argname)));
		return false;
	}

	return true;
}

/*
 * Enforce parameter pairs that must be specified together (or not at all) for
 * a particular stakind, such as most_common_vals and most_common_freqs for
 * STATISTIC_KIND_MCV.
 *
 * If a problem is found, emit a WARNING, and return false. Otherwise return
 * true.
 */
bool
stats_check_arg_pair(FunctionCallInfo fcinfo,
					 struct StatsArgInfo *arginfo,
					 int argnum1, int argnum2)
{
	if (PG_ARGISNULL(argnum1) && PG_ARGISNULL(argnum2))
		return true;

	if (PG_ARGISNULL(argnum1) || PG_ARGISNULL(argnum2))
	{
		int			nullarg = PG_ARGISNULL(argnum1) ? argnum1 : argnum2;
		int			otherarg = PG_ARGISNULL(argnum1) ? argnum2 : argnum1;

		ereport(WARNING,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("argument \"%s\" must be specified when argument \"%s\" is specified",
						arginfo[nullarg].argname,
						arginfo[otherarg].argname)));

		return false;
	}

	return true;
}

/*
 * A role has privileges to set statistics on the relation if any of the
 * following are true:
 *   - the role owns the current database and the relation is not shared
 *   - the role has the MAINTAIN privilege on the relation
 */
void
RangeVarCallbackForStats(const RangeVar *relation,
						 Oid relId, Oid oldRelId, void *arg)
{
	Oid		   *locked_oid = (Oid *) arg;
	Oid			table_oid = relId;
	HeapTuple	tuple;
	Form_pg_class form;
	char		relkind;

	/*
	 * If we previously locked some other index's heap, and the name we're
	 * looking up no longer refers to that relation, release the now-useless
	 * lock.
	 */
	if (relId != oldRelId && OidIsValid(*locked_oid))
	{
		UnlockRelationOid(*locked_oid, ShareUpdateExclusiveLock);
		*locked_oid = InvalidOid;
	}

	/* If the relation does not exist, there's nothing more to do. */
	if (!OidIsValid(relId))
		return;

	/* If the relation does exist, check whether it's an index. */
	relkind = get_rel_relkind(relId);
	if (relkind == RELKIND_INDEX ||
		relkind == RELKIND_PARTITIONED_INDEX)
		table_oid = IndexGetRelation(relId, false);

	/*
	 * If retrying yields the same OID, there are a couple of extremely
	 * unlikely scenarios we need to handle.
	 */
	if (relId == oldRelId)
	{
		/*
		 * If a previous lookup found an index, but the current lookup did
		 * not, the index was dropped and the OID was reused for something
		 * else between lookups.  In theory, we could simply drop our lock on
		 * the index's parent table and proceed, but in the interest of
		 * avoiding complexity, we just error.
		 */
		if (table_oid == relId && OidIsValid(*locked_oid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("index \"%s\" was concurrently dropped",
							relation->relname)));

		/*
		 * If the current lookup found an index but a previous lookup either
		 * did not find an index or found one with a different parent
		 * relation, the relation was dropped and the OID was reused for an
		 * index between lookups.  RangeVarGetRelidExtended() will have
		 * already locked the index at this point, so we can't just lock the
		 * newly discovered parent table OID without risking deadlock.  As
		 * above, we just error in this case.
		 */
		if (table_oid != relId && table_oid != *locked_oid)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("index \"%s\" was concurrently created",
							relation->relname)));
	}

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(table_oid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for OID %u", table_oid);
	form = (Form_pg_class) GETSTRUCT(tuple);

	/* the relkinds that can be used with ANALYZE */
	switch (form->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_MATVIEW:
		case RELKIND_FOREIGN_TABLE:
		case RELKIND_PARTITIONED_TABLE:
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot modify statistics for relation \"%s\"",
							NameStr(form->relname)),
					 errdetail_relkind_not_supported(form->relkind)));
	}

	if (form->relisshared)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot modify statistics for shared relation")));

	/* Check permissions */
	if (!object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId()))
	{
		AclResult	aclresult = pg_class_aclcheck(table_oid,
												  GetUserId(),
												  ACL_MAINTAIN);

		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult,
						   get_relkind_objtype(form->relkind),
						   NameStr(form->relname));
	}

	ReleaseSysCache(tuple);

	/* Lock heap before index to avoid deadlock. */
	if (relId != oldRelId && table_oid != relId)
	{
		LockRelationOid(table_oid, ShareUpdateExclusiveLock);
		*locked_oid = table_oid;
	}
}


/*
 * Find the argument number for the given argument name, returning -1 if not
 * found.
 */
static int
get_arg_by_name(const char *argname, struct StatsArgInfo *arginfo)
{
	int			argnum;

	for (argnum = 0; arginfo[argnum].argname != NULL; argnum++)
		if (pg_strcasecmp(argname, arginfo[argnum].argname) == 0)
			return argnum;

	ereport(WARNING,
			(errmsg("unrecognized argument name: \"%s\"", argname)));

	return -1;
}

/*
 * Ensure that a given argument matched the expected type.
 */
static bool
stats_check_arg_type(const char *argname, Oid argtype, Oid expectedtype)
{
	if (argtype != expectedtype)
	{
		ereport(WARNING,
				(errmsg("argument \"%s\" has type %s, expected type %s",
						argname, format_type_be(argtype),
						format_type_be(expectedtype))));
		return false;
	}

	return true;
}

/*
 * Check if attribute of an index is an expression, then retrieve the
 * expression if is it the case.
 *
 * If the attnum specified is known to be an expression, then we must
 * walk the list attributes up to the specified attnum to get the right
 * expression.
 */
static Node *
statatt_get_index_expr(Relation rel, int attnum)
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
	 * The index's attnum points directly to a relation attnum, hence it is
	 * not an expression attribute.
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
 * Translate variadic argument pairs from 'pairs_fcinfo' into a
 * 'positional_fcinfo' appropriate for calling relation_statistics_update() or
 * attribute_statistics_update() with positional arguments.
 *
 * Caller should have already initialized positional_fcinfo with a size
 * appropriate for calling the intended positional function, and arginfo
 * should also match the intended positional function.
 */
bool
stats_fill_fcinfo_from_arg_pairs(FunctionCallInfo pairs_fcinfo,
								 FunctionCallInfo positional_fcinfo,
								 struct StatsArgInfo *arginfo)
{
	Datum	   *args;
	bool	   *argnulls;
	Oid		   *types;
	int			nargs;
	bool		result = true;

	/* clear positional args */
	for (int i = 0; arginfo[i].argname != NULL; i++)
	{
		positional_fcinfo->args[i].value = (Datum) 0;
		positional_fcinfo->args[i].isnull = true;
	}

	nargs = extract_variadic_args(pairs_fcinfo, 0, true,
								  &args, &types, &argnulls);

	if (nargs % 2 != 0)
		ereport(ERROR,
				errmsg("variadic arguments must be name/value pairs"),
				errhint("Provide an even number of variadic arguments that can be divided into pairs."));

	/*
	 * For each argument name/value pair, find corresponding positional
	 * argument for the argument name, and assign the argument value to
	 * positional_fcinfo.
	 */
	for (int i = 0; i < nargs; i += 2)
	{
		int			argnum;
		char	   *argname;

		if (argnulls[i])
			ereport(ERROR,
					(errmsg("name at variadic position %d is null", i + 1)));

		if (types[i] != TEXTOID)
			ereport(ERROR,
					(errmsg("name at variadic position %d has type %s, expected type %s",
							i + 1, format_type_be(types[i]),
							format_type_be(TEXTOID))));

		if (argnulls[i + 1])
			continue;

		argname = TextDatumGetCString(args[i]);

		/*
		 * The 'version' argument is a special case, not handled by arginfo
		 * because it's not a valid positional argument.
		 *
		 * For now, 'version' is accepted but ignored. In the future it can be
		 * used to interpret older statistics properly.
		 */
		if (pg_strcasecmp(argname, "version") == 0)
			continue;

		argnum = get_arg_by_name(argname, arginfo);

		if (argnum < 0 || !stats_check_arg_type(argname, types[i + 1],
												arginfo[argnum].argtype))
		{
			result = false;
			continue;
		}

		positional_fcinfo->args[argnum].value = args[i + 1];
		positional_fcinfo->args[argnum].isnull = false;
	}

	return result;
}

/*
 * Derive type information from a relation attribute.
 *
 * This is needed for setting most slot statistics for all data types.
 *
 * This duplicates the logic in examine_attribute() but it will not skip the
 * attribute if the attstattarget is 0.
 *
 * This information, retrieved from pg_attribute and pg_type with some
 * specific handling for index expressions, is a prerequisite to calling
 * any of the other statatt_*() functions.
 */
void
statatt_get_type(Oid reloid, AttrNumber attnum,
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
				 errmsg("column %d of relation \"%s\" does not exist",
						attnum, RelationGetRelationName(rel))));

	attr = (Form_pg_attribute) GETSTRUCT(atup);

	if (attr->attisdropped)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column %d of relation \"%s\" does not exist",
						attnum, RelationGetRelationName(rel))));

	expr = statatt_get_index_expr(rel, attr->attnum);

	/*
	 * When analyzing an expression index, believe the expression tree's type
	 * not the column datatype --- the latter might be the opckeytype storage
	 * type of the opclass, which is not interesting for our purposes.  This
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
 * Derive element type information from the attribute type.  This information
 * is needed when the given type is one that contains elements of other types.
 *
 * The atttypid and atttyptype should be derived from a previous call to
 * statatt_get_type().
 */
bool
statatt_get_elem_type(Oid atttypid, char atttyptype,
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
 * Build an array with element type elemtypid from a text datum, used as
 * value of an attribute in a tuple to-be-inserted into pg_statistic.
 *
 * The typid and typmod should be derived from a previous call to
 * statatt_get_type().
 *
 * If an error is encountered, capture it and throw a WARNING, with "ok" set
 * to false.  If the resulting array contains NULLs, raise a WARNING and
 * set "ok" to false.  When the operation succeeds, set "ok" to true.
 */
Datum
statatt_build_stavalues(const char *staname, FmgrInfo *array_in, Datum d, Oid typid,
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
				 errmsg("\"%s\" array must not contain null values", staname)));
		*ok = false;
		return (Datum) 0;
	}

	*ok = true;

	return result;
}

/*
 * Find and update the slot of a stakind, or use the first empty slot.
 *
 * Core statistics types expect the stakind value to be one of the
 * STATISTIC_KIND_* constants defined in pg_statistic.h, but types defined
 * by extensions are not restricted to those values.
 *
 * In the case of core statistics, the required staop is determined by the
 * stakind given and will either be a hardcoded oid, or the eq/lt operator
 * derived from statatt_get_type().  Likewise, types defined by extensions
 * have no such restriction.
 *
 * The stacoll value should be either the atttypcoll derived from
 * statatt_get_type(), or a hardcoded value required by that particular
 * stakind.
 *
 * The value/null pairs for stanumbers and stavalues should be calculated
 * based on the stakind, using statatt_build_stavalues() or constructed arrays.
 */
void
statatt_set_slot(Datum *values, bool *nulls, bool *replaces,
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
 * Initialize values and nulls for a new pg_statistic tuple.
 *
 * The caller is responsible for allocating the arrays where the results are
 * stored, which should be of size Natts_pg_statistic.
 *
 * When using this routine for a tuple inserted into pg_statistic, reloid,
 * attnum and inherited flags should all be set.
 *
 * When using this routine for a tuple that is an element of a stxdexpr
 * array inserted into pg_statistic_ext_data, reloid, attnum and inherited
 * should be respectively set to InvalidOid, InvalidAttrNumber and false.
 */
void
statatt_init_empty_tuple(Oid reloid, int16 attnum, bool inherited,
						 Datum *values, bool *nulls, bool *replaces)
{
	memset(nulls, true, sizeof(bool) * Natts_pg_statistic);
	memset(replaces, true, sizeof(bool) * Natts_pg_statistic);

	/* This must initialize non-NULL attributes */
	values[Anum_pg_statistic_starelid - 1] = ObjectIdGetDatum(reloid);
	nulls[Anum_pg_statistic_starelid - 1] = false;
	values[Anum_pg_statistic_staattnum - 1] = Int16GetDatum(attnum);
	nulls[Anum_pg_statistic_staattnum - 1] = false;
	values[Anum_pg_statistic_stainherit - 1] = BoolGetDatum(inherited);
	nulls[Anum_pg_statistic_stainherit - 1] = false;

	values[Anum_pg_statistic_stanullfrac - 1] = DEFAULT_STATATT_NULL_FRAC;
	nulls[Anum_pg_statistic_stanullfrac - 1] = false;
	values[Anum_pg_statistic_stawidth - 1] = DEFAULT_STATATT_AVG_WIDTH;
	nulls[Anum_pg_statistic_stawidth - 1] = false;
	values[Anum_pg_statistic_stadistinct - 1] = DEFAULT_STATATT_N_DISTINCT;
	nulls[Anum_pg_statistic_stadistinct - 1] = false;

	/* initialize stakind, staop, and stacoll slots */
	for (int slotnum = 0; slotnum < STATISTIC_NUM_SLOTS; slotnum++)
	{
		values[Anum_pg_statistic_stakind1 + slotnum - 1] = (Datum) 0;
		nulls[Anum_pg_statistic_stakind1 + slotnum - 1] = false;
		values[Anum_pg_statistic_staop1 + slotnum - 1] = ObjectIdGetDatum(InvalidOid);
		nulls[Anum_pg_statistic_staop1 + slotnum - 1] = false;
		values[Anum_pg_statistic_stacoll1 + slotnum - 1] = ObjectIdGetDatum(InvalidOid);
		nulls[Anum_pg_statistic_stacoll1 + slotnum - 1] = false;
	}
}
