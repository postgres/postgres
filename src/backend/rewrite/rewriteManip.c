/*-------------------------------------------------------------------------
 *
 * rewriteManip.c--
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/rewrite/rewriteManip.c,v 1.28 1999/02/08 01:39:45 wieck Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include "postgres.h"
#include "nodes/pg_list.h"
#include "utils/elog.h"
#include "nodes/nodes.h"
#include "nodes/relation.h"
#include "nodes/primnodes.h"
#include "parser/parsetree.h"	/* for getrelid() */
#include "utils/lsyscache.h"
#include "utils/builtins.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteManip.h"
#include "rewrite/rewriteSupport.h"
#include "rewrite/locks.h"

#include "nodes/plannodes.h"
#include "optimizer/clauses.h"

static void ResolveNew(RewriteInfo *info, List *targetlist,
		   Node **node, int sublevels_up);


/*
 * OffsetVarnodes -
 */
void
OffsetVarNodes(Node *node, int offset, int sublevels_up)
{
	if (node == NULL)
		return;

	switch(nodeTag(node)) {
		case T_TargetEntry:
			{
				TargetEntry	*tle = (TargetEntry *)node;

				OffsetVarNodes(
						(Node *)(tle->expr),
						offset,
						sublevels_up);
			}
			break;

		case T_Aggref:
			{
				Aggref	*aggref = (Aggref *)node;

				OffsetVarNodes(
						(Node *)(aggref->target),
						offset,
						sublevels_up);
			}
			break;

		case T_GroupClause:
			{
				GroupClause	*grp = (GroupClause *)node;

				OffsetVarNodes(
						(Node *)(grp->entry),
						offset,
						sublevels_up);
			}
			break;

		case T_Expr:
			{
				Expr	*exp = (Expr *)node;

				OffsetVarNodes(
						(Node *)(exp->args),
						offset,
						sublevels_up);
			}
			break;

		case T_Iter:
			{
				Iter	*iter = (Iter *)node;

				OffsetVarNodes(
						(Node *)(iter->iterexpr),
						offset,
						sublevels_up);
			}
			break;

		case T_ArrayRef:
			{
				ArrayRef	*ref = (ArrayRef *)node;

				OffsetVarNodes(
						(Node *)(ref->refupperindexpr),
						offset,
						sublevels_up);
				OffsetVarNodes(
						(Node *)(ref->reflowerindexpr),
						offset,
						sublevels_up);
				OffsetVarNodes(
						(Node *)(ref->refexpr),
						offset,
						sublevels_up);
				OffsetVarNodes(
						(Node *)(ref->refassgnexpr),
						offset,
						sublevels_up);
			}
			break;

		case T_Var:
			{
				Var	*var = (Var *)node;

				if (var->varlevelsup == sublevels_up) {
					var->varno += offset;
					var->varnoold += offset;
				}
			}
			break;

		case T_Param:
			break;

		case T_Const:
			break;

		case T_List:
			{
				List	*l;

				foreach (l, (List *)node)
					OffsetVarNodes(
							(Node *)lfirst(l),
							offset,
							sublevels_up);
			}
			break;

		case T_SubLink:
			{
				SubLink	*sub = (SubLink *)node;
				List *tmp_oper, *tmp_lefthand;

				/* We also have to adapt the variables used in sub->lefthand
				 * and sub->oper */
				OffsetVarNodes(
						(Node *)(sub->lefthand),
						offset,
						sublevels_up);

				OffsetVarNodes(
						(Node *)(sub->subselect),
						offset,
						sublevels_up + 1);

				/***S*I***/
				/* Make sure the first argument of sub->oper points to the
				 * same var as sub->lefthand does otherwise we will
				 * run into troubles using aggregates (aggno will not be
				 * set correctly) */
				tmp_lefthand = sub->lefthand;				
				foreach(tmp_oper, sub->oper)
				  {				    
				    lfirst(((Expr *) lfirst(tmp_oper))->args) = 
				      lfirst(tmp_lefthand);
				    tmp_lefthand = lnext(tmp_lefthand);
				  }	
			}
			break;

		case T_Query:
			{
				Query	*qry = (Query *)node;

				OffsetVarNodes(
						(Node *)(qry->targetList),
						offset,
						sublevels_up);

				OffsetVarNodes(
						(Node *)(qry->qual),
						offset,
						sublevels_up);

				OffsetVarNodes(
						(Node *)(qry->havingQual),
						offset,
						sublevels_up);

				OffsetVarNodes(
						(Node *)(qry->groupClause),
						offset,
						sublevels_up);
			}
			break;

		case T_CaseExpr:
			{
				CaseExpr	*exp = (CaseExpr *)node;

				OffsetVarNodes(
						(Node *)(exp->args),
						offset,
						sublevels_up);

				OffsetVarNodes(
						(Node *)(exp->defresult),
						offset,
						sublevels_up);
			}
			break;

		case T_CaseWhen:
			{
				CaseWhen	*exp = (CaseWhen *)node;

				OffsetVarNodes(
						(Node *)(exp->expr),
						offset,
						sublevels_up);

				OffsetVarNodes(
						(Node *)(exp->result),
						offset,
						sublevels_up);
			}
			break;

		default:
			elog(NOTICE, "unknown node tag %d in OffsetVarNodes()", nodeTag(node));
			elog(NOTICE, "Node is: %s", nodeToString(node));
			break;


	}
}


/*
 * ChangeVarNodes -
 */
void
ChangeVarNodes(Node *node, int rt_index, int new_index, int sublevels_up)
{
	if (node == NULL)
		return;

	switch(nodeTag(node)) {
		case T_TargetEntry:
			{
				TargetEntry	*tle = (TargetEntry *)node;

				ChangeVarNodes(
						(Node *)(tle->expr),
						rt_index,
						new_index,
						sublevels_up);
			}
			break;

		case T_Aggref:
			{
				Aggref	*aggref = (Aggref *)node;

				ChangeVarNodes(
						(Node *)(aggref->target),
						rt_index,
						new_index,
						sublevels_up);
			}
			break;

		case T_GroupClause:
			{
				GroupClause	*grp = (GroupClause *)node;

				ChangeVarNodes(
						(Node *)(grp->entry),
						rt_index,
						new_index,
						sublevels_up);
			}
			break;

		case T_Expr:
			{
				Expr	*exp = (Expr *)node;

				ChangeVarNodes(
						(Node *)(exp->args),
						rt_index,
						new_index,
						sublevels_up);
			}
			break;

		case T_Iter:
			{
				Iter	*iter = (Iter *)node;

				ChangeVarNodes(
						(Node *)(iter->iterexpr),
						rt_index,
						new_index,
						sublevels_up);
			}
			break;

		case T_ArrayRef:
			{
				ArrayRef	*ref = (ArrayRef *)node;

				ChangeVarNodes(
						(Node *)(ref->refupperindexpr),
						rt_index,
						new_index,
						sublevels_up);
				ChangeVarNodes(
						(Node *)(ref->reflowerindexpr),
						rt_index,
						new_index,
						sublevels_up);
				ChangeVarNodes(
						(Node *)(ref->refexpr),
						rt_index,
						new_index,
						sublevels_up);
				ChangeVarNodes(
						(Node *)(ref->refassgnexpr),
						rt_index,
						new_index,
						sublevels_up);
			}
			break;

		case T_Var:
			{
				Var	*var = (Var *)node;

				if (var->varlevelsup == sublevels_up &&
						var->varno == rt_index) {
					var->varno = new_index;
					var->varnoold = new_index;
				}
			}
			break;

		case T_Param:
			break;

		case T_Const:
			break;

		case T_List:
			{
				List	*l;

				foreach (l, (List *)node)
					ChangeVarNodes(
							(Node *)lfirst(l),
							rt_index,
							new_index,
							sublevels_up);
			}
			break;

		case T_SubLink:
			{
				SubLink	*sub = (SubLink *)node;
				List *tmp_oper, *tmp_lefthand;
				
				ChangeVarNodes(
						(Node *)(sub->lefthand),
						rt_index,
						new_index,
						sublevels_up);

				ChangeVarNodes(
						(Node *)(sub->subselect),
						rt_index,
						new_index,
						sublevels_up + 1);
				
				/***S*I***/
				/* Make sure the first argument of sub->oper points to the
				 * same var as sub->lefthand does otherwise we will
				 * run into troubles using aggregates (aggno will not be
				 * set correctly) */
				tmp_lefthand = sub->lefthand;
				foreach(tmp_oper, sub->oper)
				  {				    
				    lfirst(((Expr *) lfirst(tmp_oper))->args) = 
				      lfirst(tmp_lefthand);
				    tmp_lefthand = lnext(tmp_lefthand);
				  }	
			}
			break;

		case T_Query:
			{
				Query	*qry = (Query *)node;

				ChangeVarNodes(
						(Node *)(qry->targetList),
						rt_index,
						new_index,
						sublevels_up);

				ChangeVarNodes(
						(Node *)(qry->qual),
						rt_index,
						new_index,
						sublevels_up);

				ChangeVarNodes(
						(Node *)(qry->havingQual),
						rt_index,
						new_index,
						sublevels_up);

				ChangeVarNodes(
						(Node *)(qry->groupClause),
						rt_index,
						new_index,
						sublevels_up);
			}
			break;

		case T_CaseExpr:
			{
				CaseExpr	*exp = (CaseExpr *)node;

				ChangeVarNodes(
						(Node *)(exp->args),
						rt_index,
						new_index,
						sublevels_up);

				ChangeVarNodes(
						(Node *)(exp->defresult),
						rt_index,
						new_index,
						sublevels_up);
			}
			break;

		case T_CaseWhen:
			{
				CaseWhen	*exp = (CaseWhen *)node;

				ChangeVarNodes(
						(Node *)(exp->expr),
						rt_index,
						new_index,
						sublevels_up);

				ChangeVarNodes(
						(Node *)(exp->result),
						rt_index,
						new_index,
						sublevels_up);
			}
			break;

		default:
			elog(NOTICE, "unknown node tag %d in ChangeVarNodes()", nodeTag(node));
			elog(NOTICE, "Node is: %s", nodeToString(node));
			break;


	}
}



void
AddQual(Query *parsetree, Node *qual)
{
	Node	   *copy,
			   *old;

	if (qual == NULL)
		return;

	/***S*I***/
	/* INTERSECT want's the original, but we need to copy - Jan */
	/* copy = qual; */
	copy = copyObject(qual);

	old = parsetree->qual;
	if (old == NULL)
		parsetree->qual = copy;
	else
		parsetree->qual = (Node *) make_andclause(makeList(parsetree->qual, copy, -1));
}

/* Adds the given havingQual to the one already contained in the parsetree just as
 * AddQual does for the normal 'where' qual */
void
AddHavingQual(Query *parsetree, Node *havingQual)
{
	Node	   *copy,
			   *old;

	if (havingQual == NULL)
		return;

	/***S*I***/
	/* INTERSECT want's the original, but we need to copy - Jan */
	/* copy = havingQual; */
	copy = copyObject(havingQual);

	old = parsetree->havingQual;
	if (old == NULL)
		parsetree->havingQual = copy;
	else
		parsetree->havingQual = (Node *) make_andclause(makeList(parsetree->havingQual, copy, -1));
}

void
AddNotHavingQual(Query *parsetree, Node *havingQual)
{
	Node	   *copy;

	if (havingQual == NULL)
		return;

	/***S*I***/
	/* INTERSECT want's the original, but we need to copy - Jan */
	/* copy = (Node *) make_notclause((Expr *)havingQual); */
	copy = (Node *)make_notclause( (Expr *)copyObject(havingQual));

	AddHavingQual(parsetree, copy);
}

void
AddNotQual(Query *parsetree, Node *qual)
{
	Node	   *copy;

	if (qual == NULL)
		return;

	/***S*I***/
	/* INTERSECT want's the original, but we need to copy - Jan */
	/* copy = (Node *) make_notclause((Expr *)qual); */
	copy = (Node *) make_notclause((Expr *)copyObject(qual));

	AddQual(parsetree, copy);
}

static Node *
make_null(Oid type)
{
	Const	   *c = makeNode(Const);

	c->consttype = type;
	c->constlen = get_typlen(type);
	c->constvalue = PointerGetDatum(NULL);
	c->constisnull = true;
	c->constbyval = get_typbyval(type);
	return (Node *) c;
}

#ifdef NOT_USED
void
FixResdomTypes(List *tlist)
{
	List	   *i;

	foreach(i, tlist)
	{
		TargetEntry *tle = lfirst(i);

		if (nodeTag(tle->expr) == T_Var)
		{
			Var		   *var = (Var *) tle->expr;

			tle->resdom->restype = var->vartype;
			tle->resdom->restypmod = var->vartypmod;
		}
	}
}
#endif

static Node *
FindMatchingNew(List *tlist, int attno)
{
	List	   *i;

	foreach(i, tlist)
	{
		TargetEntry *tle = lfirst(i);

		if (tle->resdom->resno == attno)
			return tle->expr;
	}
	return NULL;
}

static Node *
FindMatchingTLEntry(List *tlist, char *e_attname)
{
	List	   *i;

	foreach(i, tlist)
	{
		TargetEntry *tle = lfirst(i);
		char	   *resname;

		resname = tle->resdom->resname;
		if (!strcmp(e_attname, resname))
			return tle->expr;
	}
	return NULL;
}

static void
ResolveNew(RewriteInfo *info, List *targetlist, Node **nodePtr,
		   int sublevels_up)
{
	Node	   *node = *nodePtr;

	if (node == NULL)
		return;

	switch (nodeTag(node))
	{
		case T_TargetEntry:
			ResolveNew(info, targetlist, &((TargetEntry *) node)->expr,
					   sublevels_up);
			break;
		case T_Aggref:
			ResolveNew(info, targetlist, &((Aggref *) node)->target,
					   sublevels_up);
			break;
		case T_Expr:
			ResolveNew(info, targetlist, (Node **) (&(((Expr *) node)->args)),
					   sublevels_up);
			break;
		case T_Iter:
			ResolveNew(info, targetlist, (Node **) (&(((Iter *) node)->iterexpr)),
					   sublevels_up);
			break;
		case T_ArrayRef:
			ResolveNew(info, targetlist, (Node **) (&(((ArrayRef *) node)->refupperindexpr)),
					   sublevels_up);
			ResolveNew(info, targetlist, (Node **) (&(((ArrayRef *) node)->reflowerindexpr)),
					   sublevels_up);
			ResolveNew(info, targetlist, (Node **) (&(((ArrayRef *) node)->refexpr)),
					   sublevels_up);
			ResolveNew(info, targetlist, (Node **) (&(((ArrayRef *) node)->refassgnexpr)),
					   sublevels_up);
			break;
		case T_Var:
			{
				int			this_varno = (int) ((Var *) node)->varno;
				int			this_varlevelsup = (int) ((Var *) node)->varlevelsup;
				Node	   *n;

				if (this_varno == info->new_varno &&
					this_varlevelsup == sublevels_up)
				{
					n = FindMatchingNew(targetlist,
										((Var *) node)->varattno);
					if (n == NULL)
					{
						if (info->event == CMD_UPDATE)
						{
							*nodePtr = n = copyObject(node);
							((Var *) n)->varno = info->current_varno;
							((Var *) n)->varnoold = info->current_varno;
						}
						else
							*nodePtr = make_null(((Var *) node)->vartype);
					}
					else
						*nodePtr = copyObject(n);
				}
				break;
			}
		case T_List:
			{
				List	   *l;

				foreach(l, (List *) node)
					ResolveNew(info, targetlist, (Node **) &(lfirst(l)),
							   sublevels_up);
				break;
			}
		case T_SubLink:
			{
				SubLink    *sublink = (SubLink *) node;
				Query	   *query = (Query *) sublink->subselect;

				ResolveNew(info, targetlist, (Node **) &(query->qual), sublevels_up + 1);
			}
			break;
		default:
			/* ignore the others */
			break;
	}
}

void
FixNew(RewriteInfo *info, Query *parsetree)
{
	ResolveNew(info, parsetree->targetList,
			   (Node **) &(info->rule_action->targetList), 0);
	ResolveNew(info, parsetree->targetList, &info->rule_action->qual, 0);
}

static void
nodeHandleRIRAttributeRule(Node **nodePtr,
						   List *rtable,
						   List *targetlist,
						   int rt_index,
						   int attr_num,
						   int *modified,
						   int *badsql,
						   int sublevels_up)
{
	Node	   *node = *nodePtr;

	if (node == NULL)
		return;
	switch (nodeTag(node))
	{
		case T_TargetEntry:
			{
				TargetEntry *tle = (TargetEntry *) node;

				nodeHandleRIRAttributeRule(&tle->expr, rtable, targetlist,
									rt_index, attr_num, modified, badsql,
										   sublevels_up);
			}
			break;
		case T_Aggref:
			{
				Aggref	   *aggref = (Aggref *) node;

				nodeHandleRIRAttributeRule(&aggref->target, rtable, targetlist,
									rt_index, attr_num, modified, badsql,
										   sublevels_up);
			}
			break;
		case T_Expr:
			{
				Expr	   *expr = (Expr *) node;

				nodeHandleRIRAttributeRule((Node **) (&(expr->args)), rtable,
										   targetlist, rt_index, attr_num,
										   modified, badsql,
										   sublevels_up);
			}
			break;
		case T_Iter:
			{
				Iter	   *iter = (Iter *) node;

				nodeHandleRIRAttributeRule((Node **) (&(iter->iterexpr)), rtable,
										   targetlist, rt_index, attr_num,
										   modified, badsql,
										   sublevels_up);
			}
			break;
		case T_ArrayRef:
			{
				ArrayRef	   *ref = (ArrayRef *) node;

				nodeHandleRIRAttributeRule((Node **) (&(ref->refupperindexpr)), rtable,
										   targetlist, rt_index, attr_num,
										   modified, badsql,
										   sublevels_up);
				nodeHandleRIRAttributeRule((Node **) (&(ref->reflowerindexpr)), rtable,
										   targetlist, rt_index, attr_num,
										   modified, badsql,
										   sublevels_up);
				nodeHandleRIRAttributeRule((Node **) (&(ref->refexpr)), rtable,
										   targetlist, rt_index, attr_num,
										   modified, badsql,
										   sublevels_up);
				nodeHandleRIRAttributeRule((Node **) (&(ref->refassgnexpr)), rtable,
										   targetlist, rt_index, attr_num,
										   modified, badsql,
										   sublevels_up);
			}
			break;
		case T_Var:
			{
				int			this_varno = ((Var *) node)->varno;
				int			this_varattno = ((Var *) node)->varattno;
				int			this_varlevelsup = ((Var *) node)->varlevelsup;

				if (this_varno == rt_index &&
					this_varattno == attr_num &&
					this_varlevelsup == sublevels_up)
				{
					if (((Var *) node)->vartype == 32)
					{			/* HACK */
						*nodePtr = make_null(((Var *) node)->vartype);
						*modified = TRUE;
						*badsql = TRUE;
						break;
					}
					else
					{
						NameData	name_to_look_for;

						name_to_look_for.data[0] = '\0';
						namestrcpy(&name_to_look_for,
								(char *) get_attname(getrelid(this_varno,
															  rtable),
													 attr_num));
						if (name_to_look_for.data[0])
						{
							Node	   *n;

							n = FindMatchingTLEntry(targetlist, (char *) &name_to_look_for);
							if (n == NULL)
								*nodePtr = make_null(((Var *) node)->vartype);
							else
								*nodePtr = n;
							*modified = TRUE;
						}
					}
				}
			}
			break;
		case T_List:
			{
				List	   *i;

				foreach(i, (List *) node)
				{
					nodeHandleRIRAttributeRule((Node **) (&(lfirst(i))), rtable,
										  targetlist, rt_index, attr_num,
										 modified, badsql, sublevels_up);
				}
			}
			break;
		case T_SubLink:
			{
				SubLink    *sublink = (SubLink *) node;
				Query	   *query = (Query *) sublink->subselect;

				nodeHandleRIRAttributeRule((Node **) &(query->qual), rtable, targetlist,
									rt_index, attr_num, modified, badsql,
										   sublevels_up + 1);
			}
			break;
		default:
			/* ignore the others */
			break;
	}
}

/*
 * Handles 'on retrieve to relation.attribute
 *			do instead retrieve (attribute = expression) w/qual'
 */
void
HandleRIRAttributeRule(Query *parsetree,
					   List *rtable,
					   List *targetlist,
					   int rt_index,
					   int attr_num,
					   int *modified,
					   int *badsql)
{

	nodeHandleRIRAttributeRule((Node **) (&(parsetree->targetList)), rtable,
							   targetlist, rt_index, attr_num,
							   modified, badsql, 0);
	nodeHandleRIRAttributeRule(&parsetree->qual, rtable, targetlist,
							   rt_index, attr_num, modified, badsql, 0);
}

#ifdef NOT_USED
static void
nodeHandleViewRule(Node **nodePtr,
				   List *rtable,
				   List *targetlist,
				   int rt_index,
				   int *modified,
				   int sublevels_up)
{
	Node	   *node = *nodePtr;

	if (node == NULL)
		return;

	switch (nodeTag(node))
	{
		case T_TargetEntry:
			{
				TargetEntry *tle = (TargetEntry *) node;

				nodeHandleViewRule(&(tle->expr), rtable, targetlist,
								   rt_index, modified, sublevels_up);
			}
			break;
		case T_Aggref:
			{
				Aggref	   *aggref = (Aggref *) node;

				nodeHandleViewRule(&(aggref->target), rtable, targetlist,
								   rt_index, modified, sublevels_up);
			}
			break;

			/*
			 * This has to be done to make queries using groupclauses work
			 * on views
			 */
		case T_GroupClause:
			{
				GroupClause *group = (GroupClause *) node;

				nodeHandleViewRule((Node **) (&(group->entry)), rtable, targetlist,
								   rt_index, modified, sublevels_up);
			}
			break;
		case T_Expr:
			{
				Expr	   *expr = (Expr *) node;

				nodeHandleViewRule((Node **) (&(expr->args)),
								   rtable, targetlist,
								   rt_index, modified, sublevels_up);
			}
			break;
		case T_Iter:
			{
				Iter	   *iter = (Iter *) node;

				nodeHandleViewRule((Node **) (&(iter->iterexpr)),
								   rtable, targetlist,
								   rt_index, modified, sublevels_up);
			}
			break;
		case T_ArrayRef:
			{
				ArrayRef	   *ref = (ArrayRef *) node;

				nodeHandleViewRule((Node **) (&(ref->refupperindexpr)),
								   rtable, targetlist,
								   rt_index, modified, sublevels_up);
				nodeHandleViewRule((Node **) (&(ref->reflowerindexpr)),
								   rtable, targetlist,
								   rt_index, modified, sublevels_up);
				nodeHandleViewRule((Node **) (&(ref->refexpr)),
								   rtable, targetlist,
								   rt_index, modified, sublevels_up);
				nodeHandleViewRule((Node **) (&(ref->refassgnexpr)),
								   rtable, targetlist,
								   rt_index, modified, sublevels_up);
			}
			break;
		case T_Var:
			{
				Var		   *var = (Var *) node;
				int			this_varno = var->varno;
				int			this_varlevelsup = var->varlevelsup;
				Node	   *n;

				if (this_varno == rt_index &&
					this_varlevelsup == sublevels_up)
				{
					n = FindMatchingTLEntry(targetlist,
										 get_attname(getrelid(this_varno,
															  rtable),
													 var->varattno));
					if (n == NULL)
						*nodePtr = make_null(((Var *) node)->vartype);
					else
					{
						/*
						 * This is a hack: The varlevelsup of the orignal
						 * variable and the new one should be the same.
						 * Normally we adapt the node by changing a
						 * pointer to point to a var contained in
						 * 'targetlist'. In the targetlist all
						 * varlevelsups are 0 so if we want to change it
						 * to the original value we have to copy the node
						 * before! (Maybe this will cause troubles with
						 * some sophisticated queries on views?)
						 */
						if (this_varlevelsup > 0)
							*nodePtr = copyObject(n);
						else
							*nodePtr = n;

						if (nodeTag(nodePtr) == T_Var)
							((Var *) *nodePtr)->varlevelsup = this_varlevelsup;
						else
							nodeHandleViewRule(&n, rtable, targetlist,
										   rt_index, modified, sublevels_up);
					}
					*modified = TRUE;
				}
				break;
			}
		case T_List:
			{
				List	   *l;

				foreach(l, (List *) node)
				{
					nodeHandleViewRule((Node **) (&(lfirst(l))),
									   rtable, targetlist,
									   rt_index, modified, sublevels_up);
				}
			}
			break;
		case T_SubLink:
			{
				SubLink    *sublink = (SubLink *) node;
				Query	   *query = (Query *) sublink->subselect;
				List *tmp_lefthand, *tmp_oper;
				

				nodeHandleViewRule((Node **) &(query->qual), rtable, targetlist,
								   rt_index, modified, sublevels_up + 1);

				/***S*H*D***/
				nodeHandleViewRule((Node **) &(query->havingQual), rtable, targetlist,
						   rt_index, modified, sublevels_up + 1);
				nodeHandleViewRule((Node **) &(query->targetList), rtable, targetlist,
						   rt_index, modified, sublevels_up + 1);


				/*
				 * We also have to adapt the variables used in
				 * sublink->lefthand and sublink->oper
				 */
				nodeHandleViewRule((Node **) &(sublink->lefthand), rtable,
						   targetlist, rt_index, modified, sublevels_up);

				/*
				 * Make sure the first argument of sublink->oper points to
				 * the same var as sublink->lefthand does otherwise we
				 * will run into troubles using aggregates (aggno will not
				 * be set correctly
				 */
				pfree(lfirst(((Expr *) lfirst(sublink->oper))->args));
				lfirst(((Expr *) lfirst(sublink->oper))->args) =
							lfirst(sublink->lefthand);


				/***S*I***/
				/* INTERSECT want's this - Jan */
				/*
 				tmp_lefthand = sublink->lefthand;				
 				foreach(tmp_oper, sublink->oper)
 				  {				    
 				    lfirst(((Expr *) lfirst(tmp_oper))->args) = 
 				      lfirst(tmp_lefthand);
 				    tmp_lefthand = lnext(tmp_lefthand);
 				  }								
				*/
  			}
			break;
		default:
			/* ignore the others */
			break;
	}
}

void
HandleViewRule(Query *parsetree,
			   List *rtable,
			   List *targetlist,
			   int rt_index,
			   int *modified)
{
	nodeHandleViewRule(&parsetree->qual, rtable, targetlist, rt_index,
					   modified, 0);
	nodeHandleViewRule((Node **) (&(parsetree->targetList)), rtable, targetlist,
					   rt_index, modified, 0);

	/*
	 * The variables in the havingQual and groupClause also have to be
	 * adapted
	 */
	nodeHandleViewRule(&parsetree->havingQual, rtable, targetlist, rt_index,
					   modified, 0);
	nodeHandleViewRule((Node **) (&(parsetree->groupClause)), rtable, targetlist, rt_index,
					   modified, 0);
}
#endif

