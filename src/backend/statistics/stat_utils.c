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

#include "access/relation.h"
#include "catalog/index.h"
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
				 errmsg("\"%s\" cannot be NULL",
						arginfo[argnum].argname)));
}

/*
 * Check that argument is either NULL or a one dimensional array with no
 * NULLs.
 *
 * If a problem is found, emit at elevel, and return false. Otherwise return
 * true.
 */
bool
stats_check_arg_array(FunctionCallInfo fcinfo,
					  struct StatsArgInfo *arginfo,
					  int argnum, int elevel)
{
	ArrayType  *arr;

	if (PG_ARGISNULL(argnum))
		return true;

	arr = DatumGetArrayTypeP(PG_GETARG_DATUM(argnum));

	if (ARR_NDIM(arr) != 1)
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" cannot be a multidimensional array",
						arginfo[argnum].argname)));
		return false;
	}

	if (array_contains_nulls(arr))
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" array cannot contain NULL values",
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
 * If a problem is found, emit at elevel, and return false. Otherwise return
 * true.
 */
bool
stats_check_arg_pair(FunctionCallInfo fcinfo,
					 struct StatsArgInfo *arginfo,
					 int argnum1, int argnum2, int elevel)
{
	if (PG_ARGISNULL(argnum1) && PG_ARGISNULL(argnum2))
		return true;

	if (PG_ARGISNULL(argnum1) || PG_ARGISNULL(argnum2))
	{
		int			nullarg = PG_ARGISNULL(argnum1) ? argnum1 : argnum2;
		int			otherarg = PG_ARGISNULL(argnum1) ? argnum2 : argnum1;

		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" must be specified when \"%s\" is specified",
						arginfo[nullarg].argname,
						arginfo[otherarg].argname)));

		return false;
	}

	return true;
}

/*
 * Lock relation in ShareUpdateExclusive mode, check privileges, and close the
 * relation (but retain the lock).
 *
 * A role has privileges to set statistics on the relation if any of the
 * following are true:
 *   - the role owns the current database and the relation is not shared
 *   - the role has the MAINTAIN privilege on the relation
 */
void
stats_lock_check_privileges(Oid reloid)
{
	Relation	table;
	Oid			table_oid = reloid;
	Oid			index_oid = InvalidOid;
	LOCKMODE	index_lockmode = NoLock;

	/*
	 * For indexes, we follow the locking behavior in do_analyze_rel() and
	 * check_inplace_rel_lock(), which is to lock the table first in
	 * ShareUpdateExclusive mode and then the index in AccessShare mode.
	 *
	 * Partitioned indexes are treated differently than normal indexes in
	 * check_inplace_rel_lock(), so we take a ShareUpdateExclusive lock on
	 * both the partitioned table and the partitioned index.
	 */
	switch (get_rel_relkind(reloid))
	{
		case RELKIND_INDEX:
			index_oid = reloid;
			table_oid = IndexGetRelation(index_oid, false);
			index_lockmode = AccessShareLock;
			break;
		case RELKIND_PARTITIONED_INDEX:
			index_oid = reloid;
			table_oid = IndexGetRelation(index_oid, false);
			index_lockmode = ShareUpdateExclusiveLock;
			break;
		default:
			break;
	}

	table = relation_open(table_oid, ShareUpdateExclusiveLock);

	/* the relkinds that can be used with ANALYZE */
	switch (table->rd_rel->relkind)
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
							RelationGetRelationName(table)),
					 errdetail_relkind_not_supported(table->rd_rel->relkind)));
	}

	if (OidIsValid(index_oid))
	{
		Relation	index;

		Assert(index_lockmode != NoLock);
		index = relation_open(index_oid, index_lockmode);

		Assert(index->rd_index && index->rd_index->indrelid == table_oid);

		/* retain lock on index */
		relation_close(index, NoLock);
	}

	if (table->rd_rel->relisshared)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot modify statistics for shared relation")));

	if (!object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId()))
	{
		AclResult	aclresult = pg_class_aclcheck(RelationGetRelid(table),
												  GetUserId(),
												  ACL_MAINTAIN);

		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult,
						   get_relkind_objtype(table->rd_rel->relkind),
						   NameStr(table->rd_rel->relname));
	}

	/* retain lock on table */
	relation_close(table, NoLock);
}

/*
 * Find the argument number for the given argument name, returning -1 if not
 * found.
 */
static int
get_arg_by_name(const char *argname, struct StatsArgInfo *arginfo, int elevel)
{
	int			argnum;

	for (argnum = 0; arginfo[argnum].argname != NULL; argnum++)
		if (pg_strcasecmp(argname, arginfo[argnum].argname) == 0)
			return argnum;

	ereport(elevel,
			(errmsg("unrecognized argument name: \"%s\"", argname)));

	return -1;
}

/*
 * Ensure that a given argument matched the expected type.
 */
static bool
stats_check_arg_type(const char *argname, Oid argtype, Oid expectedtype, int elevel)
{
	if (argtype != expectedtype)
	{
		ereport(elevel,
				(errmsg("argument \"%s\" has type \"%s\", expected type \"%s\"",
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
								 struct StatsArgInfo *arginfo,
								 int elevel)
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
					(errmsg("name at variadic position %d is NULL", i + 1)));

		if (types[i] != TEXTOID)
			ereport(ERROR,
					(errmsg("name at variadic position %d has type \"%s\", expected type \"%s\"",
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

		argnum = get_arg_by_name(argname, arginfo, elevel);

		if (argnum < 0 || !stats_check_arg_type(argname, types[i + 1],
												arginfo[argnum].argtype,
												elevel))
		{
			result = false;
			continue;
		}

		positional_fcinfo->args[argnum].value = args[i + 1];
		positional_fcinfo->args[argnum].isnull = false;
	}

	return result;
}
