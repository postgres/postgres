/*-------------------------------------------------------------------------
 *
 * rewriteManip.c--
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/rewrite/rewriteManip.c,v 1.6 1997/09/08 02:28:18 momjian Exp $
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

static void ResolveNew(RewriteInfo * info, List * targetlist, Node ** node);



void
OffsetVarNodes(Node * node, int offset)
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
		case T_Expr:
			{
				Expr	   *expr = (Expr *) node;

				OffsetVarNodes((Node *) expr->args, offset);
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
				{
					OffsetVarNodes(lfirst(l), offset);
				}
			}
			break;
		default:
			/* ignore the others */
			break;
	}
}

void
ChangeVarNodes(Node * node, int old_varno, int new_varno)
{
	if (node == NULL)
		return;
	switch (nodeTag(node))
	{
		case T_TargetEntry:
			{
				TargetEntry *tle = (TargetEntry *) node;

				ChangeVarNodes(tle->expr, old_varno, new_varno);
			}
			break;
		case T_Expr:
			{
				Expr	   *expr = (Expr *) node;

				ChangeVarNodes((Node *) expr->args, old_varno, new_varno);
			}
			break;
		case T_Var:
			{
				Var		   *var = (Var *) node;

				if (var->varno == old_varno)
				{
					var->varno = new_varno;
					var->varnoold = new_varno;
				}
			}
			break;
		case T_List:
			{
				List	   *l;

				foreach(l, (List *) node)
				{
					ChangeVarNodes(lfirst(l), old_varno, new_varno);
				}
			}
			break;
		default:
			/* ignore the others */
			break;
	}
}

void
AddQual(Query * parsetree, Node * qual)
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

void
AddNotQual(Query * parsetree, Node * qual)
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

void
FixResdomTypes(List * tlist)
{
	List	   *i;

	foreach(i, tlist)
	{
		TargetEntry *tle = lfirst(i);

		if (nodeTag(tle->expr) == T_Var)
		{
			Var		   *var = (Var *) tle->expr;

			tle->resdom->restype = var->vartype;
			tle->resdom->reslen = get_typlen(var->vartype);
		}
	}
}

static Node *
FindMatchingNew(List * tlist, int attno)
{
	List	   *i;

	foreach(i, tlist)
	{
		TargetEntry *tle = lfirst(i);

		if (tle->resdom->resno == attno)
		{
			return (tle->expr);
		}
	}
	return NULL;
}

static Node *
FindMatchingTLEntry(List * tlist, char *e_attname)
{
	List	   *i;

	foreach(i, tlist)
	{
		TargetEntry *tle = lfirst(i);
		char	   *resname;

		resname = tle->resdom->resname;
		if (!strcmp(e_attname, resname))
			return (tle->expr);
	}
	return NULL;
}

static void
ResolveNew(RewriteInfo * info, List * targetlist, Node ** nodePtr)
{
	Node	   *node = *nodePtr;

	if (node == NULL)
		return;

	switch (nodeTag(node))
	{
		case T_TargetEntry:
			ResolveNew(info, targetlist, &((TargetEntry *) node)->expr);
			break;
		case T_Expr:
			ResolveNew(info, targetlist, (Node **) (&(((Expr *) node)->args)));
			break;
		case T_Var:
			{
				int			this_varno = (int) ((Var *) node)->varno;
				Node	   *n;

				if (this_varno == info->new_varno)
				{
					n = FindMatchingNew(targetlist,
										((Var *) node)->varattno);
					if (n == NULL)
					{
						if (info->event == CMD_UPDATE)
						{
							((Var *) node)->varno = info->current_varno;
							((Var *) node)->varnoold = info->current_varno;
						}
						else
						{
							*nodePtr = make_null(((Var *) node)->vartype);
						}
					}
					else
					{
						*nodePtr = n;
					}
				}
				break;
			}
		case T_List:
			{
				List	   *l;

				foreach(l, (List *) node)
				{
					ResolveNew(info, targetlist, (Node **) & (lfirst(l)));
				}
				break;
			}
		default:
			/* ignore the others */
			break;
	}
}

void
FixNew(RewriteInfo * info, Query * parsetree)
{
	ResolveNew(info, parsetree->targetList,
			   (Node **) & (info->rule_action->targetList));
	ResolveNew(info, parsetree->targetList, &info->rule_action->qual);
}

static void
nodeHandleRIRAttributeRule(Node ** nodePtr,
						   List * rtable,
						   List * targetlist,
						   int rt_index,
						   int attr_num,
						   int *modified,
						   int *badsql)
{
	Node	   *node = *nodePtr;

	if (node == NULL)
		return;
	switch (nodeTag(node))
	{
		case T_List:
			{
				List	   *i;

				foreach(i, (List *) node)
				{
					nodeHandleRIRAttributeRule((Node **) (&(lfirst(i))), rtable,
										  targetlist, rt_index, attr_num,
											   modified, badsql);
				}
			}
			break;
		case T_TargetEntry:
			{
				TargetEntry *tle = (TargetEntry *) node;

				nodeHandleRIRAttributeRule(&tle->expr, rtable, targetlist,
								   rt_index, attr_num, modified, badsql);
			}
			break;
		case T_Expr:
			{
				Expr	   *expr = (Expr *) node;

				nodeHandleRIRAttributeRule((Node **) (&(expr->args)), rtable,
										   targetlist, rt_index, attr_num,
										   modified, badsql);
			}
			break;
		case T_Var:
			{
				int			this_varno = (int) ((Var *) node)->varno;
				NameData	name_to_look_for;

				memset(name_to_look_for.data, 0, NAMEDATALEN);

				if (this_varno == rt_index &&
					((Var *) node)->varattno == attr_num)
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
						namestrcpy(&name_to_look_for,
								(char *) get_attname(getrelid(this_varno,
															  rtable),
													 attr_num));
					}
				}
				if (name_to_look_for.data[0])
				{
					Node	   *n;

					n = FindMatchingTLEntry(targetlist, (char *) &name_to_look_for);
					if (n == NULL)
					{
						*nodePtr = make_null(((Var *) node)->vartype);
					}
					else
					{
						*nodePtr = n;
					}
					*modified = TRUE;
				}
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
HandleRIRAttributeRule(Query * parsetree,
					   List * rtable,
					   List * targetlist,
					   int rt_index,
					   int attr_num,
					   int *modified,
					   int *badsql)
{
	nodeHandleRIRAttributeRule((Node **) (&(parsetree->targetList)), rtable,
							   targetlist, rt_index, attr_num,
							   modified, badsql);
	nodeHandleRIRAttributeRule(&parsetree->qual, rtable, targetlist,
							   rt_index, attr_num, modified, badsql);
}


static void
nodeHandleViewRule(Node ** nodePtr,
				   List * rtable,
				   List * targetlist,
				   int rt_index,
				   int *modified)
{
	Node	   *node = *nodePtr;

	if (node == NULL)
		return;

	switch (nodeTag(node))
	{
		case T_List:
			{
				List	   *l;

				foreach(l, (List *) node)
				{
					nodeHandleViewRule((Node **) (&(lfirst(l))),
									   rtable, targetlist,
									   rt_index, modified);
				}
			}
			break;
		case T_TargetEntry:
			{
				TargetEntry *tle = (TargetEntry *) node;

				nodeHandleViewRule(&(tle->expr), rtable, targetlist,
								   rt_index, modified);
			}
			break;
		case T_Expr:
			{
				Expr	   *expr = (Expr *) node;

				nodeHandleViewRule((Node **) (&(expr->args)),
								   rtable, targetlist,
								   rt_index, modified);
			}
			break;
		case T_Var:
			{
				Var		   *var = (Var *) node;
				int			this_varno = var->varno;
				Node	   *n;

				if (this_varno == rt_index)
				{
					n = FindMatchingTLEntry(targetlist,
										 get_attname(getrelid(this_varno,
															  rtable),
													 var->varattno));
					if (n == NULL)
					{
						*nodePtr = make_null(((Var *) node)->vartype);
					}
					else
					{
						*nodePtr = n;
					}
					*modified = TRUE;
				}
				break;
			}
		default:
			/* ignore the others */
			break;
	}
}

void
HandleViewRule(Query * parsetree,
			   List * rtable,
			   List * targetlist,
			   int rt_index,
			   int *modified)
{
	nodeHandleViewRule(&parsetree->qual, rtable, targetlist, rt_index,
					   modified);
	nodeHandleViewRule((Node **) (&(parsetree->targetList)), rtable, targetlist,
					   rt_index, modified);
}
