/*-------------------------------------------------------------------------
 *
 * lockcmds.c
 *	  LOCK command support code
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
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
#include "utils/syscache.h"

static void LockTableRecurse(Oid reloid, LOCKMODE lockmode, bool nowait);
static AclResult LockTableAclCheck(Oid relid, LOCKMODE lockmode);
static void RangeVarCallbackForLockTable(const RangeVar *rv, Oid relid,
							 Oid oldrelid, void *arg);

/*
 * LOCK TABLE
 */
void
LockTableCommand(LockStmt *lockstmt)
{
	ListCell   *p;

	/*---------
	 * During recovery we only accept these variations:
	 * LOCK TABLE foo IN ACCESS SHARE MODE
	 * LOCK TABLE foo IN ROW SHARE MODE
	 * LOCK TABLE foo IN ROW EXCLUSIVE MODE
	 * This test must match the restrictions defined in LockAcquireExtended()
	 *---------
	 */
	if (lockstmt->mode > RowExclusiveLock)
		PreventCommandDuringRecovery("LOCK TABLE");

	/*
	 * Iterate over the list and process the named relations one at a time
	 */
	foreach(p, lockstmt->relations)
	{
		RangeVar   *rv = (RangeVar *) lfirst(p);
		bool		recurse = interpretInhOption(rv->inhOpt);
		Oid			reloid;

		reloid = RangeVarGetRelidExtended(rv, lockstmt->mode, false,
										  lockstmt->nowait,
										  RangeVarCallbackForLockTable,
										  (void *) &lockstmt->mode);

		if (recurse)
			LockTableRecurse(reloid, lockstmt->mode, lockstmt->nowait);
	}
}

/*
 * Before acquiring a table lock on the named table, check whether we have
 * permission to do so.
 */
static void
RangeVarCallbackForLockTable(const RangeVar *rv, Oid relid, Oid oldrelid,
							 void *arg)
{
	LOCKMODE	lockmode = *(LOCKMODE *) arg;
	char		relkind;
	AclResult	aclresult;

	if (!OidIsValid(relid))
		return;					/* doesn't exist, so no permissions check */
	relkind = get_rel_relkind(relid);
	if (!relkind)
		return;					/* woops, concurrently dropped; no permissions
								 * check */

	/* Currently, we only allow plain tables to be locked */
	if (relkind != RELKIND_RELATION)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table",
						rv->relname)));

	/* Check permissions. */
	aclresult = LockTableAclCheck(relid, lockmode);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_CLASS, rv->relname);
}

/*
 * Apply LOCK TABLE recursively over an inheritance tree
 *
 * We use find_inheritance_children not find_all_inheritors to avoid taking
 * locks far in advance of checking privileges.  This means we'll visit
 * multiply-inheriting children more than once, but that's no problem.
 */
static void
LockTableRecurse(Oid reloid, LOCKMODE lockmode, bool nowait)
{
	List	   *children;
	ListCell   *lc;

	children = find_inheritance_children(reloid, NoLock);

	foreach(lc, children)
	{
		Oid			childreloid = lfirst_oid(lc);
		AclResult	aclresult;

		/* Check permissions before acquiring the lock. */
		aclresult = LockTableAclCheck(childreloid, lockmode);
		if (aclresult != ACLCHECK_OK)
		{
			char	   *relname = get_rel_name(childreloid);

			if (!relname)
				continue;		/* child concurrently dropped, just skip it */
			aclcheck_error(aclresult, ACL_KIND_CLASS, relname);
		}

		/* We have enough rights to lock the relation; do so. */
		if (!nowait)
			LockRelationOid(childreloid, lockmode);
		else if (!ConditionalLockRelationOid(childreloid, lockmode))
		{
			/* try to throw error by name; relation could be deleted... */
			char	   *relname = get_rel_name(childreloid);

			if (!relname)
				continue;		/* child concurrently dropped, just skip it */
			ereport(ERROR,
					(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					 errmsg("could not obtain lock on relation \"%s\"",
							relname)));
		}

		/*
		 * Even if we got the lock, child might have been concurrently
		 * dropped. If so, we can skip it.
		 */
		if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(childreloid)))
		{
			/* Release useless lock */
			UnlockRelationOid(childreloid, lockmode);
			continue;
		}

		LockTableRecurse(childreloid, lockmode, nowait);
	}
}

/*
 * Check whether the current user is permitted to lock this relation.
 */
static AclResult
LockTableAclCheck(Oid reloid, LOCKMODE lockmode)
{
	AclResult	aclresult;

	/* Verify adequate privilege */
	if (lockmode == AccessShareLock)
		aclresult = pg_class_aclcheck(reloid, GetUserId(),
									  ACL_SELECT);
	else
		aclresult = pg_class_aclcheck(reloid, GetUserId(),
									  ACL_UPDATE | ACL_DELETE | ACL_TRUNCATE);
	return aclresult;
}
