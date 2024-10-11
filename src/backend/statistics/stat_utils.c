/*-------------------------------------------------------------------------
 * stat_utils.c
 *
 *	  PostgreSQL statistics manipulation utilities.
 *
 * Code supporting the direct manipulation of statistics.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *       src/backend/statistics/stat_utils.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/relation.h"
#include "catalog/pg_database.h"
#include "miscadmin.h"
#include "statistics/stat_utils.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/acl.h"
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
	Relation	rel = relation_open(reloid, ShareUpdateExclusiveLock);
	const char	relkind = rel->rd_rel->relkind;

	/* All of the types that can be used with ANALYZE, plus indexes */
	switch (relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_INDEX:
		case RELKIND_MATVIEW:
		case RELKIND_FOREIGN_TABLE:
		case RELKIND_PARTITIONED_TABLE:
		case RELKIND_PARTITIONED_INDEX:
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot modify statistics for relation \"%s\"",
							RelationGetRelationName(rel)),
					 errdetail_relkind_not_supported(rel->rd_rel->relkind)));
	}

	if (rel->rd_rel->relisshared)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot modify statistics for shared relation")));

	if (!object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId()))
	{
		AclResult	aclresult = pg_class_aclcheck(RelationGetRelid(rel),
												  GetUserId(),
												  ACL_MAINTAIN);

		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult,
						   get_relkind_objtype(rel->rd_rel->relkind),
						   NameStr(rel->rd_rel->relname));
	}

	relation_close(rel, NoLock);
}
