/*-------------------------------------------------------------------------
 *
 * locks.c--
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/rewrite/Attic/locks.c,v 1.7 1998/01/15 19:00:06 momjian Exp $
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

/*
 * ThisLockWasTriggered
 *
 * walk the tree, if there we find a varnode,
 * we check the varattno against the attnum
 * if we find at least one such match, we return true
 * otherwise, we return false
 */
static bool
nodeThisLockWasTriggered(Node *node, int varno, AttrNumber attnum)
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

				return
					nodeThisLockWasTriggered((Node *) expr->args, varno, attnum);
			}
			break;
		case T_TargetEntry:
			{
				TargetEntry *tle = (TargetEntry *) node;

				return
					nodeThisLockWasTriggered(tle->expr, varno, attnum);
			}
			break;
		case T_Aggreg:
			{
				Aggreg *agg = (Aggreg *) node;

				return
					nodeThisLockWasTriggered(agg->target, varno, attnum);
			}
			break;
		case T_List:
			{
				List	   *l;

				foreach(l, (List *) node)
				{
					if (nodeThisLockWasTriggered(lfirst(l), varno, attnum))
						return TRUE;
				}
				return FALSE;
			}
			break;
		default:
			break;
	}
	return (FALSE);
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
	
	if (nodeThisLockWasTriggered(parsetree->qual, varno, attnum))
		return true;

	if (nodeThisLockWasTriggered((Node *) parsetree->targetList, varno, attnum))
		return true;

	return false;
		
}

/*
 * matchLocks -
 *	  match the list of locks and returns the matching rules
 */
List	   *
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
		{
			return (NULL);
		}
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
			{
				real_locks = lappend(real_locks, oneLock);
			}
		}
	}

	return (real_locks);
}
