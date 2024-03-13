/*-------------------------------------------------------------------------
 *
 * lockcmds.c
 *	  LOCK command support code
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/lockcmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_inherits.h"
#include "commands/lockcmds.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "rewrite/rewriteHandler.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

static void LockTableRecurse(Oid reloid, LOCKMODE lockmode, bool nowait);
static AclResult LockTableAclCheck(Oid reloid, LOCKMODE lockmode, Oid userid);
static void RangeVarCallbackForLockTable(const RangeVar *rv, Oid relid,
										 Oid oldrelid, void *arg);
static void LockViewRecurse(Oid reloid, LOCKMODE lockmode, bool nowait,
							List *ancestor_views);

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
		RangeVar   *rv = (RangeVar *) lfirst(p);
		bool		recurse = rv->inh;
		Oid			reloid;

		reloid = RangeVarGetRelidExtended(rv, lockstmt->mode,
										  lockstmt->nowait ? RVR_NOWAIT : 0,
										  RangeVarCallbackForLockTable,
										  (void *) &lockstmt->mode);

		if (get_rel_relkind(reloid) == RELKIND_VIEW)
			LockViewRecurse(reloid, lockstmt->mode, lockstmt->nowait, NIL);
		else if (recurse)
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
	char		relpersistence;
	AclResult	aclresult;

	if (!OidIsValid(relid))
		return;					/* doesn't exist, so no permissions check */
	relkind = get_rel_relkind(relid);
	if (!relkind)
		return;					/* woops, concurrently dropped; no permissions
								 * check */

	/* Currently, we only allow plain tables or views to be locked */
	if (relkind != RELKIND_RELATION && relkind != RELKIND_PARTITIONED_TABLE &&
		relkind != RELKIND_VIEW)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot lock relation \"%s\"",
						rv->relname),
				 errdetail_relkind_not_supported(relkind)));

	/*
	 * Make note if a temporary relation has been accessed in this
	 * transaction.
	 */
	relpersistence = get_rel_persistence(relid);
	if (relpersistence == RELPERSISTENCE_TEMP)
		MyXactFlags |= XACT_FLAGS_ACCESSEDTEMPNAMESPACE;

	/* Check permissions. */
	aclresult = LockTableAclCheck(relid, lockmode, GetUserId());
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, get_relkind_objtype(get_rel_relkind(relid)), rv->relname);
}

/*
 * Apply LOCK TABLE recursively over an inheritance tree
 *
 * This doesn't check permission to perform LOCK TABLE on the child tables,
 * because getting here means that the user has permission to lock the
 * parent which is enough.
 */
static void
LockTableRecurse(Oid reloid, LOCKMODE lockmode, bool nowait)
{
	List	   *children;
	ListCell   *lc;

	children = find_all_inheritors(reloid, NoLock, NULL);

	foreach(lc, children)
	{
		Oid			childreloid = lfirst_oid(lc);

		/* Parent already locked. */
		if (childreloid == reloid)
			continue;

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
	}
}

/*
 * Apply LOCK TABLE recursively over a view
 *
 * All tables and views appearing in the view definition query are locked
 * recursively with the same lock mode.
 */

typedef struct
{
	LOCKMODE	lockmode;		/* lock mode to use */
	bool		nowait;			/* no wait mode */
	Oid			check_as_user;	/* user for checking the privilege */
	Oid			viewoid;		/* OID of the view to be locked */
	List	   *ancestor_views; /* OIDs of ancestor views */
} LockViewRecurse_context;

static bool
LockViewRecurse_walker(Node *node, LockViewRecurse_context *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, Query))
	{
		Query	   *query = (Query *) node;
		ListCell   *rtable;

		foreach(rtable, query->rtable)
		{
			RangeTblEntry *rte = lfirst(rtable);
			AclResult	aclresult;

			Oid			relid = rte->relid;
			char		relkind = rte->relkind;
			char	   *relname = get_rel_name(relid);

			/* Currently, we only allow plain tables or views to be locked. */
			if (relkind != RELKIND_RELATION && relkind != RELKIND_PARTITIONED_TABLE &&
				relkind != RELKIND_VIEW)
				continue;

			/*
			 * We might be dealing with a self-referential view.  If so, we
			 * can just stop recursing, since we already locked it.
			 */
			if (list_member_oid(context->ancestor_views, relid))
				continue;

			/*
			 * Check permissions as the specified user.  This will either be
			 * the view owner or the current user.
			 */
			aclresult = LockTableAclCheck(relid, context->lockmode,
										  context->check_as_user);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, get_relkind_objtype(relkind), relname);

			/* We have enough rights to lock the relation; do so. */
			if (!context->nowait)
				LockRelationOid(relid, context->lockmode);
			else if (!ConditionalLockRelationOid(relid, context->lockmode))
				ereport(ERROR,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
						 errmsg("could not obtain lock on relation \"%s\"",
								relname)));

			if (relkind == RELKIND_VIEW)
				LockViewRecurse(relid, context->lockmode, context->nowait,
								context->ancestor_views);
			else if (rte->inh)
				LockTableRecurse(relid, context->lockmode, context->nowait);
		}

		return query_tree_walker(query,
								 LockViewRecurse_walker,
								 context,
								 QTW_IGNORE_JOINALIASES);
	}

	return expression_tree_walker(node,
								  LockViewRecurse_walker,
								  context);
}

static void
LockViewRecurse(Oid reloid, LOCKMODE lockmode, bool nowait,
				List *ancestor_views)
{
	LockViewRecurse_context context;
	Relation	view;
	Query	   *viewquery;

	/* caller has already locked the view */
	view = table_open(reloid, NoLock);
	viewquery = get_view_query(view);

	/*
	 * If the view has the security_invoker property set, check permissions as
	 * the current user.  Otherwise, check permissions as the view owner.
	 */
	context.lockmode = lockmode;
	context.nowait = nowait;
	if (RelationHasSecurityInvoker(view))
		context.check_as_user = GetUserId();
	else
		context.check_as_user = view->rd_rel->relowner;
	context.viewoid = reloid;
	context.ancestor_views = lappend_oid(ancestor_views, reloid);

	LockViewRecurse_walker((Node *) viewquery, &context);

	context.ancestor_views = list_delete_last(context.ancestor_views);

	table_close(view, NoLock);
}

/*
 * Check whether the current user is permitted to lock this relation.
 */
static AclResult
LockTableAclCheck(Oid reloid, LOCKMODE lockmode, Oid userid)
{
	AclResult	aclresult;
	AclMode		aclmask;

	/* any of these privileges permit any lock mode */
	aclmask = ACL_MAINTAIN | ACL_UPDATE | ACL_DELETE | ACL_TRUNCATE;

	/* SELECT privileges also permit ACCESS SHARE and below */
	if (lockmode <= AccessShareLock)
		aclmask |= ACL_SELECT;

	/* INSERT privileges also permit ROW EXCLUSIVE and below */
	if (lockmode <= RowExclusiveLock)
		aclmask |= ACL_INSERT;

	aclresult = pg_class_aclcheck(reloid, userid, aclmask);

	return aclresult;
}
