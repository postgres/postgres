/*-------------------------------------------------------------------------
 * stat_utils.c
 *
 *	  PostgreSQL statistics manipulation utilities.
 *
 * Code supporting the direct manipulation of statistics.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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
#include "catalog/pg_database.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "statistics/stat_utils.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

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
