/*-------------------------------------------------------------------------
 *
 * locks.c--
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/rewrite/Attic/locks.c,v 1.12 1998/09/01 03:24:53 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"			/* for oid defs */
#include "utils/elog.h"			/* for elog */
#include "nodes/pg_list.h"		/* lisp support package */
#include "nodes/parsenodes.h"
#include "nodes/primnodes.h"	/* Var node def */
#include "utils/syscache.h"		/* for SearchSysCache */
#include "rewrite/locks.h"		/* for rewrite specific lock defns */

#include "access/heapam.h"		/* for ACL checking */
#include "utils/syscache.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "catalog/pg_shadow.h"

static void checkLockPerms(List *locks, Query *parsetree, int rt_index);

/*
 * ThisLockWasTriggered
 *
 * walk the tree, if there we find a varnode,
 * we check the varattno against the attnum
 * if we find at least one such match, we return true
 * otherwise, we return false
 */
static bool
nodeThisLockWasTriggered(Node *node, int varno, AttrNumber attnum,
						 int sublevels_up)
{
	if (node == NULL)
		return FALSE;
	switch (nodeTag(node))
	{
		case T_Var:
			{
				Var		   *var = (Var *) node;

				if (varno == var->varno &&
					(attnum == var->varattno || attnum == -1))
					return TRUE;
			}
			break;
		case T_Expr:
			{
				Expr	   *expr = (Expr *) node;

				return nodeThisLockWasTriggered((Node *) expr->args, varno,
												attnum, sublevels_up);
			}
			break;
		case T_TargetEntry:
			{
				TargetEntry *tle = (TargetEntry *) node;

				return nodeThisLockWasTriggered(tle->expr, varno, attnum,
												sublevels_up);
			}
			break;
		case T_Aggreg:
			{
				Aggreg	   *agg = (Aggreg *) node;

				return nodeThisLockWasTriggered(agg->target, varno, attnum,
												sublevels_up);
			}
			break;
		case T_List:
			{
				List	   *l;

				foreach(l, (List *) node)
				{
					if (nodeThisLockWasTriggered(lfirst(l), varno, attnum,
												 sublevels_up))
						return TRUE;
				}
				return FALSE;
			}
			break;
		case T_SubLink:
			{
				SubLink    *sublink = (SubLink *) node;
				Query	   *query = (Query *) sublink->subselect;

				return nodeThisLockWasTriggered(query->qual, varno, attnum,
												sublevels_up + 1);
			}
			break;
		default:
			break;
	}
	return FALSE;
}

/*
 * thisLockWasTriggered -
 *	   walk the tree, if there we find a varnode, we check the varattno
 *	   against the attnum if we find at least one such match, we return true
 *	   otherwise, we return false
 */
static bool
thisLockWasTriggered(int varno,
					 AttrNumber attnum,
					 Query *parsetree)
{

	if (nodeThisLockWasTriggered(parsetree->qual, varno, attnum, 0))
		return true;

	if (nodeThisLockWasTriggered((Node *) parsetree->targetList, varno, attnum, 0))
		return true;

	return false;

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


static void
checkLockPerms(List *locks, Query *parsetree, int rt_index)
{
	Relation	ev_rel;
	HeapTuple	usertup;
	char		*evowner;
	RangeTblEntry	*rte;
	int32		reqperm;
	int32		aclcheck_res;
	int		i;
	List		*l;

	if (locks == NIL)
		return;

	/*
	 * Get the usename of the rules event relation owner
	 */
	rte = (RangeTblEntry *)nth(rt_index - 1, parsetree->rtable);
	ev_rel = heap_openr(rte->relname);
	usertup = SearchSysCacheTuple(USESYSID,
			ObjectIdGetDatum(ev_rel->rd_rel->relowner),
			0, 0, 0);
	if (!HeapTupleIsValid(usertup))
	{
		elog(ERROR, "cache lookup for userid %d failed",
			 ev_rel->rd_rel->relowner);
	}
	heap_close(ev_rel);
	evowner = nameout(&(((Form_pg_shadow) GETSTRUCT(usertup))->usename));
	
	/*
	 * Check all the locks, that should get fired on this query
	 */
	foreach (l, locks) {
		RewriteRule	*onelock = (RewriteRule *)lfirst(l);
		List		*action;

		/*
		 * In each lock check every action
		 */
		foreach (action, onelock->actions) {
			Query 	*query = (Query *)lfirst(action);

			/*
			 * In each action check every rangetable entry
			 * for read/write permission of the event relations
			 * owner depending on if it's the result relation
			 * (write) or not (read)
			 */
			for (i = 2; i < length(query->rtable); i++) {
				if (i + 1 == query->resultRelation)
					switch (query->resultRelation) {
						case CMD_INSERT:
							reqperm = ACL_AP;
							break;
						default:
							reqperm = ACL_WR;
							break;
					}
				else
					reqperm = ACL_RD;

				rte = (RangeTblEntry *)nth(i, query->rtable);
				aclcheck_res = pg_aclcheck(rte->relname, 
							evowner, reqperm);
				if (aclcheck_res != ACLCHECK_OK) {
					elog(ERROR, "%s: %s", 
						rte->relname,
						aclcheck_error_strings[aclcheck_res]);
				}

				/*
				 * So this is allowed due to the permissions
				 * of the rules event relation owner. But
				 * let's see if the next one too
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


