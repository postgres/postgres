/*-------------------------------------------------------------------------
 *
 * rewriteManip.c--
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/rewrite/rewriteManip.c,v 1.21 1998/10/20 17:21:44 momjian Exp $
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



void
OffsetVarNodes(Node *node, int offset)
{
	if (node == NULL)
		return;
	switch (nodeTag(node))
	{
		case T_TargetEntry:
			{
				TargetEntry *tle = (TargetEntry *) node;

				OffsetVarNodes(tle->expr, offset);
			}
			break;
		case T_Aggreg:
			{
				Aggreg	   *agg = (Aggreg *) node;

				OffsetVarNodes(agg->target, offset);
			}
			break;

			/*
			 * This has to be done to make queries using groupclauses work
			 * on views
			 */
		case T_GroupClause:
			{
				GroupClause *group = (GroupClause *) node;

				OffsetVarNodes((Node *) (group->entry), offset);
			}
			break;
		case T_Expr:
			{
				Expr	   *expr = (Expr *) node;

				OffsetVarNodes((Node *) expr->args, offset);
			}
			break;
		case T_Iter:
			{
				Iter	   *iter = (Iter *) node;

				OffsetVarNodes((Node *) iter->iterexpr, offset);
			}
			break;
		case T_ArrayRef:
			{
				ArrayRef	   *ref = (ArrayRef *) node;

				OffsetVarNodes((Node *) ref->refupperindexpr, offset);
				OffsetVarNodes((Node *) ref->reflowerindexpr, offset);
				OffsetVarNodes((Node *) ref->refexpr, offset);
				OffsetVarNodes((Node *) ref->refassgnexpr, offset);
			}
			break;
		case T_Var:
			{
				Var		   *var = (Var *) node;

				var->varno += offset;
				var->varnoold += offset;
			}
			break;
		case T_List:
			{
				List	   *l;

				foreach(l, (List *) node)
					OffsetVarNodes(lfirst(l), offset);
			}
			break;
		case T_SubLink:
			{
				SubLink    *sublink = (SubLink *) node;

				/*
				 * We also have to adapt the variables used in
				 * sublink->lefthand and sublink->oper
				 */
				OffsetVarNodes((Node *) (sublink->lefthand), offset);

				/*
				 * Make sure the first argument of sublink->oper points to
				 * the same var as sublink->lefthand does otherwise we
				 * will run into troubles using aggregates (aggno will not
				 * be set correctly)
				 */
				lfirst(((Expr *) lfirst(sublink->oper))->args) =
					lfirst(sublink->lefthand);
			}
			break;
		default:
			/* ignore the others */
			break;
	}
}

void
ChangeVarNodes(Node *node, int old_varno, int new_varno, int sublevels_up)
{
	if (node == NULL)
		return;
	switch (nodeTag(node))
	{
		case T_TargetEntry:
			{
				TargetEntry *tle = (TargetEntry *) node;

				ChangeVarNodes(tle->expr, old_varno, new_varno, sublevels_up);
			}
			break;
		case T_Aggreg:
			{
				Aggreg	   *agg = (Aggreg *) node;

				ChangeVarNodes(agg->target, old_varno, new_varno, sublevels_up);
			}
			break;

			/*
			 * This has to be done to make queries using groupclauses work
			 * on views
			 */
		case T_GroupClause:
			{
				GroupClause *group = (GroupClause *) node;

				ChangeVarNodes((Node *) (group->entry), old_varno, new_varno,
							   sublevels_up);
			}
			break;

		case T_Expr:
			{
				Expr	   *expr = (Expr *) node;

				ChangeVarNodes((Node *) expr->args, old_varno, new_varno, sublevels_up);
			}
			break;
		case T_Iter:
			{
				Iter	   *iter = (Iter *) node;

				ChangeVarNodes((Node *) iter->iterexpr, old_varno, new_varno, sublevels_up);
			}
			break;
		case T_ArrayRef:
			{
				ArrayRef	   *ref = (ArrayRef *) node;

				ChangeVarNodes((Node *) ref->refupperindexpr, old_varno, new_varno, sublevels_up);
				ChangeVarNodes((Node *) ref->reflowerindexpr, old_varno, new_varno, sublevels_up);
				ChangeVarNodes((Node *) ref->refexpr, old_varno, new_varno, sublevels_up);
				ChangeVarNodes((Node *) ref->refassgnexpr, old_varno, new_varno, sublevels_up);
			}
			break;
		case T_Var:
			{
				Var		   *var = (Var *) node;

				if (var->varno == old_varno &&
					var->varlevelsup == sublevels_up)
				{
					var->varno = new_varno;
					var->varnoold = new_varno;
				}
				if (var->varlevelsup > 0)
					OffsetVarNodes((Node *) var, 3);

			}
			break;
		case T_List:
			{
				List	   *l;

				foreach(l, (List *) node)
					ChangeVarNodes(lfirst(l), old_varno, new_varno, sublevels_up);
			}
			break;
		case T_SubLink:
			{
				SubLink    *sublink = (SubLink *) node;
				Query	   *query = (Query *) sublink->subselect;

				ChangeVarNodes((Node *) query->qual, old_varno, new_varno,
							   sublevels_up + 1);

				/*
				 * We also have to adapt the variables used in
				 * sublink->lefthand and sublink->oper
				 */
				ChangeVarNodes((Node *) (sublink->lefthand), old_varno, new_varno,
							   sublevels_up);

				/*
				 * Make sure the first argument of sublink->oper points to
				 * the same var as sublink->lefthand does otherwise we
				 * will run into troubles using aggregates (aggno will not
				 * be set correctly
				 */

				/*
				 * lfirst(((Expr *) lfirst(sublink->oper))->args) =
				 * lfirst(sublink->lefthand);
				 */
			}
			break;
		default:
			/* ignore the others */
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

	copy = copyObject(qual);
	old = parsetree->qual;
	if (old == NULL)
		parsetree->qual = copy;
	else
		parsetree->qual =
			(Node *) make_andclause(makeList(parsetree->qual, copy, -1));
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

	copy = copyObject(havingQual);
	old = parsetree->havingQual;
	if (old == NULL)
		parsetree->havingQual = copy;
	else
		parsetree->havingQual =
			(Node *) make_andclause(makeList(parsetree->havingQual, copy, -1));
}


void
AddNotQual(Query *parsetree, Node *qual)
{
	Node	   *copy;

	if (qual == NULL)
		return;

	copy = (Node *) make_notclause(copyObject(qual));

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
		case T_Aggreg:
			ResolveNew(info, targetlist, &((Aggreg *) node)->target,
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
		case T_Aggreg:
			{
				Aggreg	   *agg = (Aggreg *) node;

				nodeHandleRIRAttributeRule(&agg->target, rtable, targetlist,
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
		case T_Aggreg:
			{
				Aggreg	   *agg = (Aggreg *) node;

				nodeHandleViewRule(&(agg->target), rtable, targetlist,
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

				nodeHandleViewRule((Node **) &(query->qual), rtable, targetlist,
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
			}
			break;
		default:
			/* ignore the others */
			break;
	}
}

#ifdef NOT_USED
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

