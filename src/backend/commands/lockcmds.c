/*-------------------------------------------------------------------------
 *
 * lockcmds.c
 *	  LOCK command support code
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/commands/lockcmds.c,v 1.25 2009/06/11 14:48:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/namespace.h"
#include "catalog/pg_inherits_fn.h"
#include "commands/lockcmds.h"
#include "miscadmin.h"
#include "parser/parse_clause.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"

static void LockTableRecurse(Oid reloid, RangeVar *rv,
				 LOCKMODE lockmode, bool nowait, bool recurse);


/*
 * LOCK TABLE
 */
void
LockTableCommand(LockStmt *lockstmt)
{
	ListCell   *p;

	/*
	 * Iterate over the list and process the named relations one at a time
	 */
	foreach(p, lockstmt->relations)
	{
		RangeVar   *relation = (RangeVar *) lfirst(p);
		bool		recurse = interpretInhOption(relation->inhOpt);
		Oid			reloid;

		reloid = RangeVarGetRelid(relation, false);

		LockTableRecurse(reloid, relation,
						 lockstmt->mode, lockstmt->nowait, recurse);
	}
}

/*
 * Apply LOCK TABLE recursively over an inheritance tree
 *
 * At top level, "rv" is the original command argument; we use it to throw
 * an appropriate error message if the relation isn't there.  Below top level,
 * "rv" is NULL and we should just silently ignore any dropped child rel.
 */
static void
LockTableRecurse(Oid reloid, RangeVar *rv,
				 LOCKMODE lockmode, bool nowait, bool recurse)
{
	Relation	rel;
	AclResult	aclresult;

	/*
	 * Acquire the lock.  We must do this first to protect against concurrent
	 * drops.  Note that a lock against an already-dropped relation's OID
	 * won't fail.
	 */
	if (nowait)
	{
		if (!ConditionalLockRelationOid(reloid, lockmode))
		{
			/* try to throw error by name; relation could be deleted... */
			char	   *relname = rv ? rv->relname : get_rel_name(reloid);

			if (relname)
				ereport(ERROR,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
						 errmsg("could not obtain lock on relation \"%s\"",
								relname)));
			else
				ereport(ERROR,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					  errmsg("could not obtain lock on relation with OID %u",
							 reloid)));
		}
	}
	else
		LockRelationOid(reloid, lockmode);

	/*
	 * Now that we have the lock, check to see if the relation really exists
	 * or not.
	 */
	rel = try_relation_open(reloid, NoLock);

	if (!rel)
	{
		/* Release useless lock */
		UnlockRelationOid(reloid, lockmode);

		/* At top level, throw error; otherwise, ignore this child rel */
		if (rv)
		{
			if (rv->schemaname)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_TABLE),
						 errmsg("relation \"%s.%s\" does not exist",
								rv->schemaname, rv->relname)));
			else
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_TABLE),
						 errmsg("relation \"%s\" does not exist",
								rv->relname)));
		}

		return;
	}

	/* Verify adequate privilege */
	if (lockmode == AccessShareLock)
		aclresult = pg_class_aclcheck(reloid, GetUserId(),
									  ACL_SELECT);
	else
		aclresult = pg_class_aclcheck(reloid, GetUserId(),
									  ACL_UPDATE | ACL_DELETE | ACL_TRUNCATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_CLASS,
					   RelationGetRelationName(rel));

	/* Currently, we only allow plain tables to be locked */
	if (rel->rd_rel->relkind != RELKIND_RELATION)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table",
						RelationGetRelationName(rel))));

	/*
	 * If requested, recurse to children.  We use find_inheritance_children
	 * not find_all_inheritors to avoid taking locks far in advance of
	 * checking privileges.  This means we'll visit multiply-inheriting
	 * children more than once, but that's no problem.
	 */
	if (recurse)
	{
		List	   *children = find_inheritance_children(reloid, NoLock);
		ListCell   *lc;

		foreach(lc, children)
		{
			Oid			childreloid = lfirst_oid(lc);

			LockTableRecurse(childreloid, NULL, lockmode, nowait, recurse);
		}
	}

	relation_close(rel, NoLock);	/* close rel, keep lock */
}
