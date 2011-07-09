/*-------------------------------------------------------------------------
 *
 * lockcmds.c
 *	  LOCK command support code
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/lockcmds.c
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

static void LockTableRecurse(Relation rel, LOCKMODE lockmode, bool nowait,
				 bool recurse);


/*
 * LOCK TABLE
 */
void
LockTableCommand(LockStmt *lockstmt)
{
	ListCell   *p;

	/*
	 * During recovery we only accept these variations: LOCK TABLE foo IN
	 * ACCESS SHARE MODE LOCK TABLE foo IN ROW SHARE MODE LOCK TABLE foo
	 * IN ROW EXCLUSIVE MODE This test must match the restrictions defined
	 * in LockAcquire()
	 */
	if (lockstmt->mode > RowExclusiveLock)
		PreventCommandDuringRecovery("LOCK TABLE");

	/*
	 * Iterate over the list and process the named relations one at a time
	 */
	foreach(p, lockstmt->relations)
	{
		RangeVar   *rv = (RangeVar *) lfirst(p);
		Relation	rel;
		bool		recurse = interpretInhOption(rv->inhOpt);
		Oid			reloid;

		reloid = RangeVarGetRelid(rv, lockstmt->mode, false, lockstmt->nowait);
		rel = relation_open(reloid, NoLock);

		LockTableRecurse(rel, lockstmt->mode, lockstmt->nowait, recurse);
	}
}

/*
 * Apply LOCK TABLE recursively over an inheritance tree
 */
static void
LockTableRecurse(Relation rel, LOCKMODE lockmode, bool nowait, bool recurse)
{
	AclResult	aclresult;
	Oid			reloid = RelationGetRelid(rel);

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
		Relation	childrel;

		foreach(lc, children)
		{
			Oid			childreloid = lfirst_oid(lc);

			/*
			 * Acquire the lock, to protect against concurrent drops.  Note
			 * that a lock against an already-dropped relation's OID won't
			 * fail.
			 */
			if (!nowait)
				LockRelationOid(childreloid, lockmode);
			else if (!ConditionalLockRelationOid(childreloid, lockmode))
			{
				/* try to throw error by name; relation could be deleted... */
				char	   *relname = get_rel_name(childreloid);

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

			/*
			 * Now that we have the lock, check to see if the relation really
			 * exists or not.
			 */
			childrel = try_relation_open(childreloid, NoLock);
			if (!childrel)
			{
				/* Release useless lock */
				UnlockRelationOid(childreloid, lockmode);
			}

			LockTableRecurse(childrel, lockmode, nowait, recurse);
		}
	}

	relation_close(rel, NoLock);	/* close rel, keep lock */
}
