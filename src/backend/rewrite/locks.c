/*-------------------------------------------------------------------------
 *
 * locks.c
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/rewrite/Attic/locks.c,v 1.31 2000/09/06 14:15:20 petere Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/pg_shadow.h"
#include "optimizer/clauses.h"
#include "rewrite/locks.h"
#include "parser/parsetree.h"
#include "utils/acl.h"
#include "utils/syscache.h"


/*
 * thisLockWasTriggered
 *
 * walk the tree, if there we find a varnode,
 * we check the varattno against the attnum
 * if we find at least one such match, we return true
 * otherwise, we return false
 *
 * XXX this should be unified with attribute_used()
 */

typedef struct
{
	int			varno;
	int			attnum;
	int			sublevels_up;
} thisLockWasTriggered_context;

static bool
thisLockWasTriggered_walker(Node *node,
							thisLockWasTriggered_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup == context->sublevels_up &&
			var->varno == context->varno &&
			(var->varattno == context->attnum || context->attnum == -1))
			return true;
		return false;
	}
	if (IsA(node, SubLink))
	{

		/*
		 * Standard expression_tree_walker will not recurse into
		 * subselect, but here we must do so.
		 */
		SubLink    *sub = (SubLink *) node;

		if (thisLockWasTriggered_walker((Node *) (sub->lefthand), context))
			return true;
		context->sublevels_up++;
		if (thisLockWasTriggered_walker((Node *) (sub->subselect), context))
		{
			context->sublevels_up--;	/* not really necessary */
			return true;
		}
		context->sublevels_up--;
		return false;
	}
	if (IsA(node, Query))
	{
		/* Reach here after recursing down into subselect above... */
		Query	   *qry = (Query *) node;

		if (thisLockWasTriggered_walker((Node *) (qry->targetList), context))
			return true;
		if (thisLockWasTriggered_walker((Node *) (qry->qual), context))
			return true;
		if (thisLockWasTriggered_walker((Node *) (qry->havingQual), context))
			return true;
		return false;
	}
	return expression_tree_walker(node, thisLockWasTriggered_walker,
								  (void *) context);
}

static bool
thisLockWasTriggered(int varno,
					 int attnum,
					 Query *parsetree)
{
	thisLockWasTriggered_context context;

	context.varno = varno;
	context.attnum = attnum;
	context.sublevels_up = 0;

	return thisLockWasTriggered_walker((Node *) parsetree, &context);
}

/*
 * matchLocks -
 *	  match the list of locks and returns the matching rules
 */
List *
matchLocks(CmdType event,
		   RuleLock *rulelocks,
		   int varno,
		   Query *parsetree)
{
	List	   *real_locks = NIL;
	int			nlocks;
	int			i;

	Assert(rulelocks != NULL);	/* we get called iff there is some lock */
	Assert(parsetree != NULL);

	if (parsetree->commandType != CMD_SELECT)
	{
		if (parsetree->resultRelation != varno)
			return NULL;
	}

	nlocks = rulelocks->numLocks;

	for (i = 0; i < nlocks; i++)
	{
		RewriteRule *oneLock = rulelocks->rules[i];

		if (oneLock->event == event)
		{
			if (parsetree->commandType != CMD_SELECT ||
				thisLockWasTriggered(varno,
									 oneLock->attrno,
									 parsetree))
				real_locks = lappend(real_locks, oneLock);
		}
	}

	checkLockPerms(real_locks, parsetree, varno);

	return real_locks;
}


/*
 * Check the access permissions of tables that are referred to by a rule.
 * We want to check the access permissions using the userid of the rule's
 * owner, *not* of the current user (the one accessing the rule).  So, we
 * do the permission check here and set skipAcl = TRUE in each of the rule's
 * RTEs, to prevent the executor from running another check with the current
 * user's ID.
 *
 * XXX This routine is called before the rule's query tree has been copied
 * out of the relcache entry where it is kept.  Therefore, when we set
 * skipAcl = TRUE, we are destructively modifying the relcache entry for
 * the event relation!  This seems fairly harmless because the relcache
 * querytree is only used as a source for the rewriter, but it's a tad
 * unclean anyway.
 *
 * Note that we must check permissions every time, even if skipAcl was
 * already set TRUE by a prior call.  This ensures that we enforce the
 * current permission settings for each referenced table, even if they
 * have changed since the relcache entry was loaded.
 */

typedef struct
{
	Oid	evowner;
} checkLockPerms_context;

static bool
checkLockPerms_walker(Node *node,
					  checkLockPerms_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, SubLink))
	{
		/*
		 * Standard expression_tree_walker will not recurse into
		 * subselect, but here we must do so.
		 */
		SubLink    *sub = (SubLink *) node;

		if (checkLockPerms_walker((Node *) (sub->lefthand), context))
			return true;
		if (checkLockPerms_walker((Node *) (sub->subselect), context))
			return true;
		return false;
	}
	if (IsA(node, Query))
	{
		/* Reach here after recursing down into subselect above... */
		Query	   *qry = (Query *) node;
		int			rtablength = length(qry->rtable);
		int			i;

		/* Check all the RTEs in this query node, except OLD and NEW */
		for (i = 1; i <= rtablength; i++)
		{
			RangeTblEntry *rte = rt_fetch(i, qry->rtable);
			int32		reqperm;
			int32		aclcheck_res;

			if (rte->ref != NULL)
			{
				if (strcmp(rte->ref->relname, "*NEW*") == 0)
					continue;
				if (strcmp(rte->ref->relname, "*OLD*") == 0)
					continue;
			}

			if (i == qry->resultRelation)
				switch (qry->commandType)
				{
					case CMD_INSERT:
						reqperm = ACL_AP;
						break;
					default:
						reqperm = ACL_WR;
						break;
				}
			else
				reqperm = ACL_RD;

			aclcheck_res = pg_aclcheck(rte->relname,
									   context->evowner,
									   reqperm);
			if (aclcheck_res != ACLCHECK_OK)
				elog(ERROR, "%s: %s",
					 rte->relname,
					 aclcheck_error_strings[aclcheck_res]);

			/*
			 * Mark RTE to prevent executor from checking again with the
			 * current user's ID...
			 */
			rte->skipAcl = true;
		}

		/* If there are sublinks, search for them and check their RTEs */
		if (qry->hasSubLinks)
		{
			if (checkLockPerms_walker((Node *) (qry->targetList), context))
				return true;
			if (checkLockPerms_walker((Node *) (qry->qual), context))
				return true;
			if (checkLockPerms_walker((Node *) (qry->havingQual), context))
				return true;
		}
		return false;
	}
	return expression_tree_walker(node, checkLockPerms_walker,
								  (void *) context);
}

void
checkLockPerms(List *locks, Query *parsetree, int rt_index)
{
	RangeTblEntry *rte;
	Relation	ev_rel;
	HeapTuple	usertup;
	Form_pg_shadow userform;
	checkLockPerms_context context;
	List	   *l;

	if (locks == NIL)
		return;					/* nothing to check */

	/*
	 * Get the usename of the rule's event relation owner
	 */
	rte = rt_fetch(rt_index, parsetree->rtable);
	ev_rel = heap_openr(rte->relname, AccessShareLock);
	usertup = SearchSysCacheTuple(SHADOWSYSID,
							  ObjectIdGetDatum(ev_rel->rd_rel->relowner),
								  0, 0, 0);
	if (!HeapTupleIsValid(usertup))
		elog(ERROR, "cache lookup for userid %d failed",
			 ev_rel->rd_rel->relowner);
	userform = (Form_pg_shadow) GETSTRUCT(usertup);
	context.evowner = userform->usesysid;
	heap_close(ev_rel, AccessShareLock);

	/*
	 * Check all the locks that should get fired on this query
	 */
	foreach(l, locks)
	{
		RewriteRule *onelock = (RewriteRule *) lfirst(l);
		List	   *action;

		/*
		 * In each lock check every action.  We must scan the action
		 * recursively in case there are any sub-queries within it.
		 */
		foreach(action, onelock->actions)
		{
			Query	   *query = (Query *) lfirst(action);

			checkLockPerms_walker((Node *) query, &context);
		}
	}
}
