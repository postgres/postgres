/*-------------------------------------------------------------------------
 *
 * locks.c
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/rewrite/Attic/locks.c,v 1.28 2000/04/12 17:15:32 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/pg_shadow.h"
#include "optimizer/clauses.h"
#include "rewrite/locks.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/syscache.h"
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


void
checkLockPerms(List *locks, Query *parsetree, int rt_index)
{
	Relation	ev_rel;
	HeapTuple	usertup;
	char	   *evowner;
	RangeTblEntry *rte;
	int32		reqperm;
	int32		aclcheck_res;
	int			i;
	List	   *l;

	if (locks == NIL)
		return;

	/*
	 * Get the usename of the rules event relation owner
	 */
	rte = (RangeTblEntry *) nth(rt_index - 1, parsetree->rtable);
	ev_rel = heap_openr(rte->relname, AccessShareLock);
	usertup = SearchSysCacheTuple(SHADOWSYSID,
							  ObjectIdGetDatum(ev_rel->rd_rel->relowner),
								  0, 0, 0);
	if (!HeapTupleIsValid(usertup))
	{
		elog(ERROR, "cache lookup for userid %d failed",
			 ev_rel->rd_rel->relowner);
	}
	heap_close(ev_rel, AccessShareLock);
	evowner = pstrdup(NameStr(((Form_pg_shadow) GETSTRUCT(usertup))->usename));

	/*
	 * Check all the locks, that should get fired on this query
	 */
	foreach(l, locks)
	{
		RewriteRule *onelock = (RewriteRule *) lfirst(l);
		List	   *action;

		/*
		 * In each lock check every action
		 */
		foreach(action, onelock->actions)
		{
			Query	   *query = (Query *) lfirst(action);

			/*
			 * In each action check every rangetable entry for read/write
			 * permission of the event relations owner depending on if
			 * it's the result relation (write) or not (read)
			 */
			for (i = 2; i < length(query->rtable); i++)
			{
				if (i + 1 == query->resultRelation)
					switch (query->resultRelation)
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

				rte = (RangeTblEntry *) nth(i, query->rtable);
				aclcheck_res = pg_aclcheck(rte->relname,
										   evowner, reqperm);
				if (aclcheck_res != ACLCHECK_OK)
				{
					elog(ERROR, "%s: %s",
						 rte->relname,
						 aclcheck_error_strings[aclcheck_res]);
				}

				/*
				 * So this is allowed due to the permissions of the rules
				 * event relation owner. But let's see if the next one too
				 */
				rte->skipAcl = TRUE;
			}
		}
	}

	/*
	 * Phew, that was close
	 */
	return;
}
