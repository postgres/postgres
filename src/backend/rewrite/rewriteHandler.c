/*-------------------------------------------------------------------------
 *
 * rewriteHandler.c--
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/rewrite/rewriteHandler.c,v 1.25 1998/10/21 16:21:24 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include "postgres.h"
#include "miscadmin.h"
#include "utils/palloc.h"
#include "utils/elog.h"
#include "utils/rel.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "nodes/relation.h"

#include "parser/parsetree.h"	/* for parsetree manipulation */
#include "parser/parse_relation.h"
#include "nodes/parsenodes.h"

#include "rewrite/rewriteSupport.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteManip.h"
#include "rewrite/locks.h"

#include "commands/creatinh.h"
#include "access/heapam.h"

#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/acl.h"
#include "catalog/pg_shadow.h"
#include "catalog/pg_type.h"



static RewriteInfo *gatherRewriteMeta(Query *parsetree,
				  Query *rule_action,
				  Node *rule_qual,
				  int rt_index,
				  CmdType event,
				  bool *instead_flag);
static bool rangeTableEntry_used(Node *node, int rt_index, int sublevels_up);
static bool attribute_used(Node *node, int rt_index, int attno, int sublevels_up);
static void modifyAggregUplevel(Node *node);
static void modifyAggregChangeVarnodes(Node **nodePtr, int rt_index, int new_index, int sublevels_up);
static void modifyAggregDropQual(Node **nodePtr, Node *orignode, Expr *expr);
static SubLink *modifyAggregMakeSublink(Expr *origexp, Query *parsetree);
static void modifyAggregQual(Node **nodePtr, Query *parsetree);













static Query *fireRIRrules(Query *parsetree);


/*
 * gatherRewriteMeta -
 *	  Gather meta information about parsetree, and rule. Fix rule body
 *	  and qualifier so that they can be mixed with the parsetree and
 *	  maintain semantic validity
 */
static RewriteInfo *
gatherRewriteMeta(Query *parsetree,
				  Query *rule_action,
				  Node *rule_qual,
				  int rt_index,
				  CmdType event,
				  bool *instead_flag)
{
	RewriteInfo *info;
	int			rt_length;
	int			result_reln;

	info = (RewriteInfo *) palloc(sizeof(RewriteInfo));
	info->rt_index = rt_index;
	info->event = event;
	info->instead_flag = *instead_flag;
	info->rule_action = (Query *) copyObject(rule_action);
	info->rule_qual = (Node *) copyObject(rule_qual);
	if (info->rule_action == NULL)
		info->nothing = TRUE;
	else
	{
		info->nothing = FALSE;
		info->action = info->rule_action->commandType;
		info->current_varno = rt_index;
		info->rt = parsetree->rtable;
		rt_length = length(info->rt);
		info->rt = append(info->rt, info->rule_action->rtable);

		info->new_varno = PRS2_NEW_VARNO + rt_length;
		OffsetVarNodes(info->rule_action->qual, rt_length, 0);
		OffsetVarNodes((Node *) info->rule_action->targetList, rt_length, 0);
		OffsetVarNodes(info->rule_qual, rt_length, 0);
		ChangeVarNodes((Node *) info->rule_action->qual,
					   PRS2_CURRENT_VARNO + rt_length, rt_index, 0);
		ChangeVarNodes((Node *) info->rule_action->targetList,
					   PRS2_CURRENT_VARNO + rt_length, rt_index, 0);
		ChangeVarNodes(info->rule_qual,
					   PRS2_CURRENT_VARNO + rt_length, rt_index, 0);

		/*
		 * bug here about replace CURRENT  -- sort of replace current is
		 * deprecated now so this code shouldn't really need to be so
		 * clutzy but.....
		 */
		if (info->action != CMD_SELECT)
		{						/* i.e update XXXXX */
			int			new_result_reln = 0;

			result_reln = info->rule_action->resultRelation;
			switch (result_reln)
			{
				case PRS2_CURRENT_VARNO:
					new_result_reln = rt_index;
					break;
				case PRS2_NEW_VARNO:	/* XXX */
				default:
					new_result_reln = result_reln + rt_length;
					break;
			}
			info->rule_action->resultRelation = new_result_reln;
		}
	}
	return info;
}


/*
 * rangeTableEntry_used -
 *	we need to process a RTE for RIR rules only if it is
 *	referenced somewhere in var nodes of the query.
 */
static bool
rangeTableEntry_used(Node *node, int rt_index, int sublevels_up)
{
	if (node == NULL)
		return FALSE;

	switch(nodeTag(node)) {
		case T_TargetEntry:
			{
				TargetEntry	*tle = (TargetEntry *)node;

				return rangeTableEntry_used(
						(Node *)(tle->expr),
						rt_index,
						sublevels_up);
			}
			break;

		case T_Aggreg:
			{
				Aggreg	*agg = (Aggreg *)node;

				return rangeTableEntry_used(
						(Node *)(agg->target),
						rt_index,
						sublevels_up);
			}
			break;

		case T_GroupClause:
			{
				GroupClause	*grp = (GroupClause *)node;

				return rangeTableEntry_used(
						(Node *)(grp->entry),
						rt_index,
						sublevels_up);
			}
			break;

		case T_Expr:
			{
				Expr	*exp = (Expr *)node;

				return rangeTableEntry_used(
						(Node *)(exp->args),
						rt_index,
						sublevels_up);
			}
			break;

		case T_Iter:
			{
				Iter	*iter = (Iter *)node;

				return rangeTableEntry_used(
						(Node *)(iter->iterexpr),
						rt_index,
						sublevels_up);
			}
			break;

		case T_ArrayRef:
			{
				ArrayRef	*ref = (ArrayRef *)node;

				if (rangeTableEntry_used(
						(Node *)(ref->refupperindexpr),
						rt_index,
						sublevels_up))
					return TRUE;
				
				if (rangeTableEntry_used(
						(Node *)(ref->reflowerindexpr),
						rt_index,
						sublevels_up))
					return TRUE;
				
				if (rangeTableEntry_used(
						(Node *)(ref->refexpr),
						rt_index,
						sublevels_up))
					return TRUE;
				
				if (rangeTableEntry_used(
						(Node *)(ref->refassgnexpr),
						rt_index,
						sublevels_up))
					return TRUE;
				
				return FALSE;
			}
			break;

		case T_Var:
			{
				Var	*var = (Var *)node;

				if (var->varlevelsup == sublevels_up)
					return var->varno == rt_index;
				else
					return FALSE;
			}
			break;

		case T_Param:
			return FALSE;

		case T_Const:
			return FALSE;

		case T_List:
			{
				List	*l;

				foreach (l, (List *)node) {
					if (rangeTableEntry_used(
							(Node *)lfirst(l),
							rt_index,
							sublevels_up))
						return TRUE;
				}
				return FALSE;
			}
			break;

		case T_SubLink:
			{
				SubLink	*sub = (SubLink *)node;

				if (rangeTableEntry_used(
						(Node *)(sub->lefthand),
						rt_index,
						sublevels_up))
					return TRUE;

				if (rangeTableEntry_used(
						(Node *)(sub->subselect),
						rt_index,
						sublevels_up + 1))
					return TRUE;

				return FALSE;
			}
			break;

		case T_Query:
			{
				Query	*qry = (Query *)node;

				if (rangeTableEntry_used(
						(Node *)(qry->targetList),
						rt_index,
						sublevels_up))
					return TRUE;

				if (rangeTableEntry_used(
						(Node *)(qry->qual),
						rt_index,
						sublevels_up))
					return TRUE;

				if (rangeTableEntry_used(
						(Node *)(qry->havingQual),
						rt_index,
						sublevels_up))
					return TRUE;

				if (rangeTableEntry_used(
						(Node *)(qry->groupClause),
						rt_index,
						sublevels_up))
					return TRUE;

				return FALSE;
			}
			break;

		default:
			elog(NOTICE, "unknown node tag %d in rangeTableEntry_used()", nodeTag(node));
			elog(NOTICE, "Node is: %s", nodeToString(node));
			break;


	}

	return FALSE;
}


/*
 * attribute_used -
 *	Check if a specific attribute number of a RTE is used
 *	somewhere in the query
 */
static bool
attribute_used(Node *node, int rt_index, int attno, int sublevels_up)
{
	if (node == NULL)
		return FALSE;

	switch(nodeTag(node)) {
		case T_TargetEntry:
			{
				TargetEntry	*tle = (TargetEntry *)node;

				return attribute_used(
						(Node *)(tle->expr),
						rt_index,
						attno,
						sublevels_up);
			}
			break;

		case T_Aggreg:
			{
				Aggreg	*agg = (Aggreg *)node;

				return attribute_used(
						(Node *)(agg->target),
						rt_index,
						attno,
						sublevels_up);
			}
			break;

		case T_GroupClause:
			{
				GroupClause	*grp = (GroupClause *)node;

				return attribute_used(
						(Node *)(grp->entry),
						rt_index,
						attno,
						sublevels_up);
			}
			break;

		case T_Expr:
			{
				Expr	*exp = (Expr *)node;

				return attribute_used(
						(Node *)(exp->args),
						rt_index,
						attno,
						sublevels_up);
			}
			break;

		case T_Iter:
			{
				Iter	*iter = (Iter *)node;

				return attribute_used(
						(Node *)(iter->iterexpr),
						rt_index,
						attno,
						sublevels_up);
			}
			break;

		case T_ArrayRef:
			{
				ArrayRef	*ref = (ArrayRef *)node;

				if (attribute_used(
						(Node *)(ref->refupperindexpr),
						rt_index,
						attno,
						sublevels_up))
					return TRUE;

				if (attribute_used(
						(Node *)(ref->reflowerindexpr),
						rt_index,
						attno,
						sublevels_up))
					return TRUE;

				if (attribute_used(
						(Node *)(ref->refexpr),
						rt_index,
						attno,
						sublevels_up))
					return TRUE;

				if (attribute_used(
						(Node *)(ref->refassgnexpr),
						rt_index,
						attno,
						sublevels_up))
					return TRUE;

				return FALSE;
			}
			break;

		case T_Var:
			{
				Var	*var = (Var *)node;

				if (var->varlevelsup == sublevels_up)
					return var->varno == rt_index;
				else
					return FALSE;
			}
			break;

		case T_Param:
			return FALSE;

		case T_Const:
			return FALSE;

		case T_List:
			{
				List	*l;

				foreach (l, (List *)node) {
					if (attribute_used(
							(Node *)lfirst(l),
							rt_index,
							attno,
							sublevels_up))
						return TRUE;
				}
				return FALSE;
			}
			break;

		case T_SubLink:
			{
				SubLink	*sub = (SubLink *)node;

				if (attribute_used(
						(Node *)(sub->lefthand),
						rt_index,
						attno,
						sublevels_up))
					return TRUE;

				if (attribute_used(
						(Node *)(sub->subselect),
						rt_index,
						attno,
						sublevels_up + 1))
					return TRUE;

				return FALSE;
			}
			break;

		case T_Query:
			{
				Query	*qry = (Query *)node;

				if (attribute_used(
						(Node *)(qry->targetList),
						rt_index,
						attno,
						sublevels_up))
					return TRUE;

				if (attribute_used(
						(Node *)(qry->qual),
						rt_index,
						attno,
						sublevels_up))
					return TRUE;

				if (attribute_used(
						(Node *)(qry->havingQual),
						rt_index,
						attno,
						sublevels_up))
					return TRUE;

				if (attribute_used(
						(Node *)(qry->groupClause),
						rt_index,
						attno,
						sublevels_up))
					return TRUE;

				return FALSE;
			}
			break;

		default:
			elog(NOTICE, "unknown node tag %d in attribute_used()", nodeTag(node));
			elog(NOTICE, "Node is: %s", nodeToString(node));
			break;


	}

	return FALSE;
}


/*
 * modifyAggregUplevel -
 *	In the newly created sublink for an aggregate column used in
 *	the qualification, we must adjust the varlevelsup in all the
 *	var nodes.
 */
static void
modifyAggregUplevel(Node *node)
{
	if (node == NULL)
		return;

	switch(nodeTag(node)) {
		case T_TargetEntry:
			{
				TargetEntry	*tle = (TargetEntry *)node;

				modifyAggregUplevel(
						(Node *)(tle->expr));
			}
			break;

		case T_Aggreg:
			{
				Aggreg	*agg = (Aggreg *)node;

				modifyAggregUplevel(
						(Node *)(agg->target));
			}
			break;

		case T_Expr:
			{
				Expr	*exp = (Expr *)node;

				modifyAggregUplevel(
						(Node *)(exp->args));
			}
			break;

		case T_Iter:
			{
				Iter	*iter = (Iter *)node;

				modifyAggregUplevel(
						(Node *)(iter->iterexpr));
			}
			break;

		case T_ArrayRef:
			{
				ArrayRef	*ref = (ArrayRef *)node;

				modifyAggregUplevel(
						(Node *)(ref->refupperindexpr));
				modifyAggregUplevel(
						(Node *)(ref->reflowerindexpr));
				modifyAggregUplevel(
						(Node *)(ref->refexpr));
				modifyAggregUplevel(
						(Node *)(ref->refassgnexpr));
			}
			break;

		case T_Var:
			{
				Var	*var = (Var *)node;

				var->varlevelsup++;
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
					modifyAggregUplevel(
							(Node *)lfirst(l));
			}
			break;

		case T_SubLink:
			{
				SubLink	*sub = (SubLink *)node;

				modifyAggregUplevel(
						(Node *)(sub->lefthand));

				modifyAggregUplevel(
						(Node *)(sub->oper));

				modifyAggregUplevel(
						(Node *)(sub->subselect));
			}
			break;

		case T_Query:
			{
				Query	*qry = (Query *)node;

				modifyAggregUplevel(
						(Node *)(qry->targetList));

				modifyAggregUplevel(
						(Node *)(qry->qual));

				modifyAggregUplevel(
						(Node *)(qry->havingQual));

				modifyAggregUplevel(
						(Node *)(qry->groupClause));
			}
			break;

		default:
			elog(NOTICE, "unknown node tag %d in modifyAggregUplevel()", nodeTag(node));
			elog(NOTICE, "Node is: %s", nodeToString(node));
			break;


	}
}


/*
 * modifyAggregChangeVarnodes -
 *	Change the var nodes in a sublink created for an aggregate column
 *	used in the qualification that is subject of the aggregate
 *	function to point to the correct local RTE.
 */
static void
modifyAggregChangeVarnodes(Node **nodePtr, int rt_index, int new_index, int sublevels_up)
{
	Node	*node = *nodePtr;

	if (node == NULL)
		return;

	switch(nodeTag(node)) {
		case T_TargetEntry:
			{
				TargetEntry	*tle = (TargetEntry *)node;

				modifyAggregChangeVarnodes(
						(Node **)(&(tle->expr)),
						rt_index,
						new_index,
						sublevels_up);
			}
			break;

		case T_Aggreg:
			{
				Aggreg	*agg = (Aggreg *)node;

				modifyAggregChangeVarnodes(
						(Node **)(&(agg->target)),
						rt_index,
						new_index,
						sublevels_up);
			}
			break;

		case T_GroupClause:
			{
				GroupClause	*grp = (GroupClause *)node;

				modifyAggregChangeVarnodes(
						(Node **)(&(grp->entry)),
						rt_index,
						new_index,
						sublevels_up);
			}
			break;

		case T_Expr:
			{
				Expr	*exp = (Expr *)node;

				modifyAggregChangeVarnodes(
						(Node **)(&(exp->args)),
						rt_index,
						new_index,
						sublevels_up);
			}
			break;

		case T_Iter:
			{
				Iter	*iter = (Iter *)node;

				modifyAggregChangeVarnodes(
						(Node **)(&(iter->iterexpr)),
						rt_index,
						new_index,
						sublevels_up);
			}
			break;

		case T_ArrayRef:
			{
				ArrayRef	*ref = (ArrayRef *)node;

				modifyAggregChangeVarnodes(
						(Node **)(&(ref->refupperindexpr)),
						rt_index,
						new_index,
						sublevels_up);
				modifyAggregChangeVarnodes(
						(Node **)(&(ref->reflowerindexpr)),
						rt_index,
						new_index,
						sublevels_up);
				modifyAggregChangeVarnodes(
						(Node **)(&(ref->refexpr)),
						rt_index,
						new_index,
						sublevels_up);
				modifyAggregChangeVarnodes(
						(Node **)(&(ref->refassgnexpr)),
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
					var = copyObject(var);
					var->varno = new_index;
					var->varnoold = new_index;
					var->varlevelsup = 0;

					*nodePtr = (Node *)var;
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
					modifyAggregChangeVarnodes(
							(Node **)(&lfirst(l)),
							rt_index,
							new_index,
							sublevels_up);
			}
			break;

		case T_SubLink:
			{
				SubLink	*sub = (SubLink *)node;

				modifyAggregChangeVarnodes(
						(Node **)(&(sub->lefthand)),
						rt_index,
						new_index,
						sublevels_up);

				modifyAggregChangeVarnodes(
						(Node **)(&(sub->oper)),
						rt_index,
						new_index,
						sublevels_up);

				modifyAggregChangeVarnodes(
						(Node **)(&(sub->subselect)),
						rt_index,
						new_index,
						sublevels_up + 1);
			}
			break;

		case T_Query:
			{
				Query	*qry = (Query *)node;

				modifyAggregChangeVarnodes(
						(Node **)(&(qry->targetList)),
						rt_index,
						new_index,
						sublevels_up);

				modifyAggregChangeVarnodes(
						(Node **)(&(qry->qual)),
						rt_index,
						new_index,
						sublevels_up);

				modifyAggregChangeVarnodes(
						(Node **)(&(qry->havingQual)),
						rt_index,
						new_index,
						sublevels_up);

				modifyAggregChangeVarnodes(
						(Node **)(&(qry->groupClause)),
						rt_index,
						new_index,
						sublevels_up);
			}
			break;

		default:
			elog(NOTICE, "unknown node tag %d in modifyAggregChangeVarnodes()", nodeTag(node));
			elog(NOTICE, "Node is: %s", nodeToString(node));
			break;


	}
}


/*
 * modifyAggregDropQual -
 *	remove the pure aggreg clase from a qualification
 */
static void
modifyAggregDropQual(Node **nodePtr, Node *orignode, Expr *expr)
{
	Node	*node = *nodePtr;

	if (node == NULL)
		return;

	switch(nodeTag(node)) {
		case T_Var:
			break;

		case T_Aggreg:
			{
				Aggreg	*agg = (Aggreg *)node;
				Aggreg	*oagg = (Aggreg *)orignode;

				modifyAggregDropQual(
						(Node **)(&(agg->target)),
						(Node *)(oagg->target),
						expr);
			}
			break;

		case T_Param:
			break;

		case T_Const:
			break;

		case T_GroupClause:
			break;

		case T_Expr:
			{
				Expr	*this_expr = (Expr *)node;
				Expr	*orig_expr = (Expr *)orignode;

				if (orig_expr == expr) {
					Const	*ctrue;

					if (expr->typeOid != BOOLOID)
						elog(ERROR,
							"aggregate expression in qualification isn't of type bool");
					ctrue = makeNode(Const);
					ctrue->consttype = BOOLOID;
					ctrue->constlen = 1;
					ctrue->constisnull = FALSE;
					ctrue->constvalue = (Datum)TRUE;
					ctrue->constbyval = TRUE;

					*nodePtr = (Node *)ctrue;
				}
				else
					modifyAggregDropQual(
						(Node **)(&(this_expr->args)),
						(Node *)(orig_expr->args),
						expr);
			}
			break;

		case T_Iter:
			{
				Iter	*iter = (Iter *)node;
				Iter	*oiter = (Iter *)orignode;

				modifyAggregDropQual(
						(Node **)(&(iter->iterexpr)),
						(Node *)(oiter->iterexpr),
						expr);
			}
			break;

		case T_ArrayRef:
			{
				ArrayRef	*ref = (ArrayRef *)node;
				ArrayRef	*oref = (ArrayRef *)orignode;

				modifyAggregDropQual(
						(Node **)(&(ref->refupperindexpr)),
						(Node *)(oref->refupperindexpr),
						expr);
				modifyAggregDropQual(
						(Node **)(&(ref->reflowerindexpr)),
						(Node *)(oref->reflowerindexpr),
						expr);
				modifyAggregDropQual(
						(Node **)(&(ref->refexpr)),
						(Node *)(oref->refexpr),
						expr);
				modifyAggregDropQual(
						(Node **)(&(ref->refassgnexpr)),
						(Node *)(oref->refassgnexpr),
						expr);
			}
			break;

		case T_List:
			{
				List	*l;
				List	*ol = (List *)orignode;
				int	li = 0;

				foreach (l, (List *)node) {
					modifyAggregDropQual(
							(Node **)(&(lfirst(l))),
							(Node *)nth(li, ol),
							expr);
					li++;
				}
			}
			break;

		case T_SubLink:
			{
				SubLink	*sub = (SubLink *)node;
				SubLink	*osub = (SubLink *)orignode;

				modifyAggregDropQual(
						(Node **)(&(sub->subselect)),
						(Node *)(osub->subselect),
						expr);
			}
			break;

		case T_Query:
			{
				Query	*qry = (Query *)node;
				Query	*oqry = (Query *)orignode;

				modifyAggregDropQual(
						(Node **)(&(qry->qual)),
						(Node *)(oqry->qual),
						expr);

				modifyAggregDropQual(
						(Node **)(&(qry->havingQual)),
						(Node *)(oqry->havingQual),
						expr);
			}
			break;

		default:
			elog(NOTICE, "unknown node tag %d in modifyAggregDropQual()", nodeTag(node));
			elog(NOTICE, "Node is: %s", nodeToString(node));
			break;


	}
}


/*
 * modifyAggregMakeSublink -
 *	Create a sublink node for a qualification expression that
 *	uses an aggregate column of a view
 */
static SubLink *
modifyAggregMakeSublink(Expr *origexp, Query *parsetree)
{
	SubLink		*sublink;
	Query		*subquery;
	Node		*subqual;
	RangeTblEntry	*rte;
	Aggreg		*aggreg;
	Var		*target;
	TargetEntry	*tle;
	Resdom		*resdom;
	Expr		*exp = copyObject(origexp);

	if (nodeTag(nth(0, exp->args)) == T_Aggreg)
		if (nodeTag(nth(1, exp->args)) == T_Aggreg)
			elog(ERROR, "rewrite: comparision of 2 aggregate columns not supported");
		else
			elog(ERROR, "rewrite: aggregate column of view must be at rigth side in qual");

	aggreg = (Aggreg *)nth(1, exp->args);
	target	= (Var *)(aggreg->target);
	rte	= (RangeTblEntry *)nth(target->varno - 1, parsetree->rtable);
	tle	= makeNode(TargetEntry);
	resdom	= makeNode(Resdom);

	aggreg->usenulls = TRUE;

	resdom->resno	= 1;
	resdom->restype	= ((Oper *)(exp->oper))->opresulttype;
	resdom->restypmod = -1;
	resdom->resname = pstrdup("<noname>");
	resdom->reskey	= 0;
	resdom->reskeyop = 0;
	resdom->resjunk	= 0;

	tle->resdom	= resdom;
	tle->expr	= (Node *)aggreg;

	subqual = copyObject(parsetree->qual);
	modifyAggregDropQual((Node **)&subqual, (Node *)parsetree->qual, origexp);

	sublink = makeNode(SubLink);
	sublink->subLinkType	= EXPR_SUBLINK;
	sublink->useor		= FALSE;
	sublink->lefthand	= lappend(NIL, copyObject(lfirst(exp->args)));
	sublink->oper		= lappend(NIL, copyObject(exp));
	sublink->subselect	= NULL;

	subquery		= makeNode(Query);
	sublink->subselect	= (Node *)subquery;

	subquery->commandType		= CMD_SELECT;
	subquery->utilityStmt		= NULL;
	subquery->resultRelation	= 0;
	subquery->into			= NULL;
	subquery->isPortal		= FALSE;
	subquery->isBinary		= FALSE;
	subquery->unionall		= FALSE;
	subquery->uniqueFlag		= NULL;
	subquery->sortClause		= NULL;
	subquery->rtable		= lappend(NIL, rte);
	subquery->targetList		= lappend(NIL, tle);
	subquery->qual			= subqual;
	subquery->groupClause		= NIL;
	subquery->havingQual		= NULL;
	subquery->hasAggs		= TRUE;
	subquery->hasSubLinks		= FALSE;
	subquery->unionClause		= NULL;


	modifyAggregUplevel((Node *)sublink);

	modifyAggregChangeVarnodes((Node **)&(sublink->lefthand), target->varno,
			1, target->varlevelsup);
	modifyAggregChangeVarnodes((Node **)&(sublink->oper), target->varno,
			1, target->varlevelsup);
	modifyAggregChangeVarnodes((Node **)&(sublink->subselect), target->varno,
			1, target->varlevelsup);

	return sublink;
}


/*
 * modifyAggregQual -
 *	Search for qualification expressions that contain aggregate
 *	functions and substiture them by sublinks. These expressions
 *	originally come from qualifications that use aggregate columns
 *	of a view.
 */
static void
modifyAggregQual(Node **nodePtr, Query *parsetree)
{
	Node	*node = *nodePtr;

	if (node == NULL)
		return;

	switch(nodeTag(node)) {
		case T_Var:
			break;

		case T_Param:
			break;

		case T_Const:
			break;

		case T_GroupClause:
			{
				GroupClause	*grp = (GroupClause *)node;

				modifyAggregQual(
						(Node **)(&(grp->entry)),
						parsetree);
			}
			break;

		case T_Expr:
			{
				Expr	*exp = (Expr *)node;
				SubLink	*sub;


				if (length(exp->args) != 2) {
					modifyAggregQual(
						(Node **)(&(exp->args)),
						parsetree);
					break;
				}

				if (nodeTag(nth(0, exp->args)) != T_Aggreg &&
					nodeTag(nth(1, exp->args)) != T_Aggreg) {

					modifyAggregQual(
						(Node **)(&(exp->args)),
						parsetree);
					break;
				}

				sub = modifyAggregMakeSublink(exp,
						parsetree);

				*nodePtr = (Node *)sub;
				parsetree->hasSubLinks = TRUE;
			}
			break;

		case T_Iter:
			{
				Iter	*iter = (Iter *)node;

				modifyAggregQual(
						(Node **)(&(iter->iterexpr)),
						parsetree);
			}
			break;

		case T_ArrayRef:
			{
				ArrayRef	*ref = (ArrayRef *)node;

				modifyAggregQual(
						(Node **)(&(ref->refupperindexpr)),
						parsetree);
				modifyAggregQual(
						(Node **)(&(ref->reflowerindexpr)),
						parsetree);
				modifyAggregQual(
						(Node **)(&(ref->refexpr)),
						parsetree);
				modifyAggregQual(
						(Node **)(&(ref->refassgnexpr)),
						parsetree);
			}
			break;

		case T_List:
			{
				List	*l;

				foreach (l, (List *)node)
					modifyAggregQual(
							(Node **)(&(lfirst(l))),
							parsetree);
			}
			break;

		case T_SubLink:
			{
				SubLink	*sub = (SubLink *)node;

				modifyAggregQual(
						(Node **)(&(sub->subselect)),
						(Query *)(sub->subselect));
			}
			break;

		case T_Query:
			{
				Query	*qry = (Query *)node;

				modifyAggregQual(
						(Node **)(&(qry->qual)),
						parsetree);

				modifyAggregQual(
						(Node **)(&(qry->havingQual)),
						parsetree);
			}
			break;

		default:
			elog(NOTICE, "unknown node tag %d in modifyAggregQual()", nodeTag(node));
			elog(NOTICE, "Node is: %s", nodeToString(node));
			break;


	}
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
			return (tle->expr);
	}
	return NULL;
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


static void 
apply_RIR_adjust_sublevel(Node *node, int sublevels_up)
{
	if (node == NULL)
		return;

	switch(nodeTag(node)) {
		case T_TargetEntry:
			{
				TargetEntry	*tle = (TargetEntry *)node;

				apply_RIR_adjust_sublevel(
						(Node *)(tle->expr),
						sublevels_up);
			}
			break;

		case T_Aggreg:
			{
				Aggreg	*agg = (Aggreg *)node;

				apply_RIR_adjust_sublevel(
						(Node *)(agg->target),
						sublevels_up);
			}
			break;

		case T_GroupClause:
			{
				GroupClause	*grp = (GroupClause *)node;

				apply_RIR_adjust_sublevel(
						(Node *)(grp->entry),
						sublevels_up);
			}
			break;

		case T_Expr:
			{
				Expr	*exp = (Expr *)node;

				apply_RIR_adjust_sublevel(
						(Node *)(exp->args),
						sublevels_up);
			}
			break;

		case T_Iter:
			{
				Iter	*iter = (Iter *)node;

				apply_RIR_adjust_sublevel(
						(Node *)(iter->iterexpr),
						sublevels_up);
			}
			break;

		case T_ArrayRef:
			{
				ArrayRef	*ref = (ArrayRef *)node;

				apply_RIR_adjust_sublevel(
						(Node *)(ref->refupperindexpr),
						sublevels_up);

				apply_RIR_adjust_sublevel(
						(Node *)(ref->reflowerindexpr),
						sublevels_up);

				apply_RIR_adjust_sublevel(
						(Node *)(ref->refexpr),
						sublevels_up);

				apply_RIR_adjust_sublevel(
						(Node *)(ref->refassgnexpr),
						sublevels_up);
			}
			break;

		case T_Var:
			{
				Var	*var = (Var *)node;

				var->varlevelsup = sublevels_up;
			}
			break;

		case T_Param:
			break;

		case T_Const:
			break;

		case T_List:
			{
				List	*l;

				foreach (l, (List *)node) {
					apply_RIR_adjust_sublevel(
							(Node *)lfirst(l),
							sublevels_up);
				}
			}
			break;

		default:
			elog(NOTICE, "unknown node tag %d in attribute_used()", nodeTag(node));
			elog(NOTICE, "Node is: %s", nodeToString(node));
			break;


	}
}


static void
apply_RIR_view(Node **nodePtr, int rt_index, RangeTblEntry *rte, List *tlist, int *modified, int sublevels_up)
{
	Node	*node = *nodePtr;

	if (node == NULL)
		return;

	switch(nodeTag(node)) {
		case T_TargetEntry:
			{
				TargetEntry	*tle = (TargetEntry *)node;

				apply_RIR_view(
						(Node **)(&(tle->expr)),
						rt_index,
						rte,
						tlist,
						modified,
						sublevels_up);
			}
			break;

		case T_Aggreg:
			{
				Aggreg	*agg = (Aggreg *)node;

				apply_RIR_view(
						(Node **)(&(agg->target)),
						rt_index,
						rte,
						tlist,
						modified,
						sublevels_up);
			}
			break;

		case T_GroupClause:
			{
				GroupClause	*grp = (GroupClause *)node;

				apply_RIR_view(
						(Node **)(&(grp->entry)),
						rt_index,
						rte,
						tlist,
						modified,
						sublevels_up);
			}
			break;

		case T_Expr:
			{
				Expr	*exp = (Expr *)node;

				apply_RIR_view(
						(Node **)(&(exp->args)),
						rt_index,
						rte,
						tlist,
						modified,
						sublevels_up);
			}
			break;

		case T_Iter:
			{
				Iter	*iter = (Iter *)node;

				apply_RIR_view(
						(Node **)(&(iter->iterexpr)),
						rt_index,
						rte,
						tlist,
						modified,
						sublevels_up);
			}
			break;

		case T_ArrayRef:
			{
				ArrayRef	*ref = (ArrayRef *)node;

				apply_RIR_view(
						(Node **)(&(ref->refupperindexpr)),
						rt_index,
						rte,
						tlist,
						modified,
						sublevels_up);
				apply_RIR_view(
						(Node **)(&(ref->reflowerindexpr)),
						rt_index,
						rte,
						tlist,
						modified,
						sublevels_up);
				apply_RIR_view(
						(Node **)(&(ref->refexpr)),
						rt_index,
						rte,
						tlist,
						modified,
						sublevels_up);
				apply_RIR_view(
						(Node **)(&(ref->refassgnexpr)),
						rt_index,
						rte,
						tlist,
						modified,
						sublevels_up);
			}
			break;

		case T_Var:
			{
				Var	*var = (Var *)node;

				if (var->varlevelsup == sublevels_up &&
						var->varno == rt_index) {
					Node		*exp;

					if (var->varattno < 0)
						elog(ERROR, "system column %s not available - %s is a view", get_attname(rte->relid, var->varattno), rte->relname);
					exp = FindMatchingTLEntry(
							tlist,
							get_attname(rte->relid,
								var->varattno));

					if (exp == NULL) {
						*nodePtr = make_null(var->vartype);
						return;
					}

					exp = copyObject(exp);
					if (var->varlevelsup > 0)
						apply_RIR_adjust_sublevel(exp, var->varlevelsup);
					*nodePtr = exp;
					*modified = TRUE;
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
					apply_RIR_view(
							(Node **)(&(lfirst(l))),
							rt_index,
							rte,
							tlist,
							modified,
							sublevels_up);
			}
			break;

		case T_SubLink:
			{
				SubLink	*sub = (SubLink *)node;

				apply_RIR_view(
						(Node **)(&(sub->lefthand)),
						rt_index,
						rte,
						tlist,
						modified,
						sublevels_up);

				apply_RIR_view(
						(Node **)(&(sub->subselect)),
						rt_index,
						rte,
						tlist,
						modified,
						sublevels_up + 1);
			}
			break;

		case T_Query:
			{
				Query	*qry = (Query *)node;

				apply_RIR_view(
						(Node **)(&(qry->targetList)),
						rt_index,
						rte,
						tlist,
						modified,
						sublevels_up);

				apply_RIR_view(
						(Node **)(&(qry->qual)),
						rt_index,
						rte,
						tlist,
						modified,
						sublevels_up);

				apply_RIR_view(
						(Node **)(&(qry->havingQual)),
						rt_index,
						rte,
						tlist,
						modified,
						sublevels_up);

				apply_RIR_view(
						(Node **)(&(qry->groupClause)),
						rt_index,
						rte,
						tlist,
						modified,
						sublevels_up);
			}
			break;

		default:
			elog(NOTICE, "unknown node tag %d in apply_RIR_view()", nodeTag(node));
			elog(NOTICE, "Node is: %s", nodeToString(node));
			break;
	}
}


static void
ApplyRetrieveRule(Query *parsetree,
				  RewriteRule *rule,
				  int rt_index,
				  int relation_level,
				  Relation relation,
				  int *modified)
{
	Query	   *rule_action = NULL;
	Node	   *rule_qual;
	List	   *rtable,
			   *rt;
	int			nothing,
				rt_length;
	int			badsql = FALSE;

	rule_qual = rule->qual;
	if (rule->actions)
	{
		if (length(rule->actions) > 1)	/* ??? because we don't handle
										 * rules with more than one
										 * action? -ay */

			return;
		rule_action = copyObject(lfirst(rule->actions));
		nothing = FALSE;
	}
	else
		nothing = TRUE;

	rtable = copyObject(parsetree->rtable);
	foreach(rt, rtable)
	{
		RangeTblEntry *rte = lfirst(rt);

		/*
		 * this is to prevent add_missing_vars_to_base_rels() from adding
		 * a bogus entry to the new target list.
		 */
		rte->inFromCl = false;
	}
	rt_length = length(rtable);

	rtable = nconc(rtable, copyObject(rule_action->rtable));
	parsetree->rtable = rtable;

	rule_action->rtable = rtable;
	OffsetVarNodes((Node *) rule_qual,   rt_length, 0);
	OffsetVarNodes((Node *) rule_action, rt_length, 0);

	ChangeVarNodes((Node *) rule_qual, 
				   PRS2_CURRENT_VARNO + rt_length, rt_index, 0);
	ChangeVarNodes((Node *) rule_action,
				   PRS2_CURRENT_VARNO + rt_length, rt_index, 0);

	if (relation_level)
	{
	  apply_RIR_view((Node **) &parsetree, rt_index, 
	  		(RangeTblEntry *)nth(rt_index - 1, rtable),
			rule_action->targetList, modified, 0);
	  apply_RIR_view((Node **) &rule_action, rt_index, 
	  		(RangeTblEntry *)nth(rt_index - 1, rtable),
			rule_action->targetList, modified, 0);
	}
	else
	{
	  HandleRIRAttributeRule(parsetree, rtable, rule_action->targetList,
				 rt_index, rule->attrno, modified, &badsql);
	}
	if (*modified && !badsql) {
	  AddQual(parsetree, rule_action->qual);
	  /* This will only work if the query made to the view defined by the following
	   * groupClause groups by the same attributes or does not use group at all! */
	  if (parsetree->groupClause == NULL)
	    parsetree->groupClause=rule_action->groupClause;
	  AddHavingQual(parsetree, rule_action->havingQual);
	  parsetree->hasAggs = (rule_action->hasAggs || parsetree->hasAggs);
	  parsetree->hasSubLinks = (rule_action->hasSubLinks ||  parsetree->hasSubLinks);
	}	
}


static void
fireRIRonSubselect(Node *node)
{
	if (node == NULL)
		return;

	switch(nodeTag(node)) {
		case T_TargetEntry:
			{
				TargetEntry	*tle = (TargetEntry *)node;

				fireRIRonSubselect(
						(Node *)(tle->expr));
			}
			break;

		case T_Aggreg:
			{
				Aggreg	*agg = (Aggreg *)node;

				fireRIRonSubselect(
						(Node *)(agg->target));
			}
			break;

		case T_GroupClause:
			{
				GroupClause	*grp = (GroupClause *)node;

				fireRIRonSubselect(
						(Node *)(grp->entry));
			}
			break;

		case T_Expr:
			{
				Expr	*exp = (Expr *)node;

				fireRIRonSubselect(
						(Node *)(exp->args));
			}
			break;

		case T_Iter:
			{
				Iter	*iter = (Iter *)node;

				fireRIRonSubselect(
						(Node *)(iter->iterexpr));
			}
			break;

		case T_ArrayRef:
			{
				ArrayRef	*ref = (ArrayRef *)node;

				fireRIRonSubselect(
						(Node *)(ref->refupperindexpr));
				fireRIRonSubselect(
						(Node *)(ref->reflowerindexpr));
				fireRIRonSubselect(
						(Node *)(ref->refexpr));
				fireRIRonSubselect(
						(Node *)(ref->refassgnexpr));
			}
			break;

		case T_Var:
			break;

		case T_Param:
			break;

		case T_Const:
			break;

		case T_List:
			{
				List	*l;

				foreach (l, (List *)node)
					fireRIRonSubselect(
							(Node *)(lfirst(l)));
			}
			break;

		case T_SubLink:
			{
				SubLink	*sub = (SubLink *)node;
				Query	*qry;

				fireRIRonSubselect(
						(Node *)(sub->lefthand));

				qry = fireRIRrules((Query *)(sub->subselect));

				fireRIRonSubselect(
						(Node *)qry);

				sub->subselect = (Node *) qry;
			}
			break;

		case T_Query:
			{
				Query	*qry = (Query *)node;

				fireRIRonSubselect(
						(Node *)(qry->targetList));

				fireRIRonSubselect(
						(Node *)(qry->qual));

				fireRIRonSubselect(
						(Node *)(qry->havingQual));

				fireRIRonSubselect(
						(Node *)(qry->groupClause));
			}
			break;

		default:
			elog(NOTICE, "unknown node tag %d in fireRIRonSubselect()", nodeTag(node));
			elog(NOTICE, "Node is: %s", nodeToString(node));
			break;


	}
}


/*
 * fireRIRrules -
 *	Apply all RIR rules on each rangetable entry in a query
 */
static Query *
fireRIRrules(Query *parsetree)
{
	int		rt_index;
	RangeTblEntry	*rte;
	Relation	rel;
	List		*locks;
	RuleLock	*rules;
	RewriteRule	*rule;
	RewriteRule	RIRonly;
	int		modified;
	int		i;
	List		*l;

	rt_index = 0;
	while(rt_index < length(parsetree->rtable)) {
		++rt_index;

		if (!rangeTableEntry_used((Node *)parsetree, rt_index, 0))
			continue;
		
		rte = nth(rt_index - 1, parsetree->rtable);
		rel = heap_openr(rte->relname);
		if (rel->rd_rules == NULL) {
			heap_close(rel);
			continue;
		}

		rules = rel->rd_rules;
		locks = NIL;

		/*
		 * Collect the RIR rules that we must apply
		 */
		for (i = 0; i < rules->numLocks; i++) {
			rule = rules->rules[i];
			if (rule->event != CMD_SELECT)
				continue;
			
			if (rule->attrno > 0 &&
					!attribute_used((Node *)parsetree,
							rt_index,
							rule->attrno, 0))
				continue;

			locks = lappend(locks, rule);
		}

		/*
		 * Check permissions
		 */
		checkLockPerms(locks, parsetree, rt_index);

		/*
		 * Now apply them
		 */
		foreach (l, locks) {
			rule = lfirst(l);

			RIRonly.event	= rule->event;
			RIRonly.attrno	= rule->attrno;
			RIRonly.qual	= rule->qual;
			RIRonly.actions	= rule->actions;

			ApplyRetrieveRule(parsetree,
					&RIRonly,
					rt_index,
					RIRonly.attrno == -1,
					rel,
					&modified);
		}

		heap_close(rel);
	}

	fireRIRonSubselect((Node *) parsetree);
	modifyAggregQual((Node **) &(parsetree->qual), parsetree);

	return parsetree;
}


/*
 * idea is to fire regular rules first, then qualified instead
 * rules and unqualified instead rules last. Any lemming is counted for.
 */
static List *
orderRules(List *locks)
{
	List	   *regular = NIL;
	List	   *instead_rules = NIL;
	List	   *instead_qualified = NIL;
	List	   *i;

	foreach(i, locks)
	{
		RewriteRule *rule_lock = (RewriteRule *) lfirst(i);

		if (rule_lock->isInstead)
		{
			if (rule_lock->qual == NULL)
				instead_rules = lappend(instead_rules, rule_lock);
			else
				instead_qualified = lappend(instead_qualified, rule_lock);
		}
		else
			regular = lappend(regular, rule_lock);
	}
	regular = nconc(regular, instead_qualified);
	return nconc(regular, instead_rules);
}



static Query *
CopyAndAddQual(Query *parsetree,
			   List *actions,
			   Node *rule_qual,
			   int rt_index,
			   CmdType event)
{
	Query	   *new_tree = (Query *) copyObject(parsetree);
	Node	   *new_qual = NULL;
	Query	   *rule_action = NULL;

	if (actions)
		rule_action = lfirst(actions);
	if (rule_qual != NULL)
		new_qual = (Node *) copyObject(rule_qual);
	if (rule_action != NULL)
	{
		List	   *rtable;
		int			rt_length;

		rtable = new_tree->rtable;
		rt_length = length(rtable);
		rtable = append(rtable, listCopy(rule_action->rtable));
		new_tree->rtable = rtable;
		OffsetVarNodes(new_qual, rt_length, 0);
		ChangeVarNodes(new_qual, PRS2_CURRENT_VARNO + rt_length, rt_index, 0);
	}
	/* XXX -- where current doesn't work for instead nothing.... yet */
	AddNotQual(new_tree, new_qual);

	return new_tree;
}



/*
 *	fireRules -
 *	   Iterate through rule locks applying rules.
 *	   All rules create their own parsetrees. Instead rules
 *	   with rule qualification save the original parsetree
 *	   and add their negated qualification to it. Real instead
 *	   rules finally throw away the original parsetree.
 *
 *	   remember: reality is for dead birds -- glass
 *
 */
static List *
fireRules(Query *parsetree,
		  int rt_index,
		  CmdType event,
		  bool *instead_flag,
		  List *locks,
		  List **qual_products)
{
	RewriteInfo *info;
	List	   *results = NIL;
	List	   *i;

	/* choose rule to fire from list of rules */
	if (locks == NIL)
	{
		return NIL;
	}

	locks = orderRules(locks);	/* real instead rules last */
	foreach(i, locks)
	{
		RewriteRule *rule_lock = (RewriteRule *) lfirst(i);
		Node	   *qual,
				   *event_qual;
		List	   *actions;
		List	   *r;

		/*
		 * Instead rules change the resultRelation of the query. So the
		 * permission checks on the initial resultRelation would never be
		 * done (this is normally done in the executor deep down). So we
		 * must do it here. The result relations resulting from earlier
		 * rewrites are already checked against the rules eventrelation
		 * owner (during matchLocks) and have the skipAcl flag set.
		 */
		if (rule_lock->isInstead &&
			parsetree->commandType != CMD_SELECT)
		{
			RangeTblEntry *rte;
			int32		acl_rc;
			int32		reqperm;

			switch (parsetree->commandType)
			{
				case CMD_INSERT:
					reqperm = ACL_AP;
					break;
				default:
					reqperm = ACL_WR;
					break;
			}

			rte = (RangeTblEntry *) nth(parsetree->resultRelation - 1,
										parsetree->rtable);
			if (!rte->skipAcl)
			{
				acl_rc = pg_aclcheck(rte->relname,
									 GetPgUserName(), reqperm);
				if (acl_rc != ACLCHECK_OK)
				{
					elog(ERROR, "%s: %s",
						 rte->relname,
						 aclcheck_error_strings[acl_rc]);
				}
			}
		}

		/* multiple rule action time */
		*instead_flag = rule_lock->isInstead;
		event_qual = rule_lock->qual;
		actions = rule_lock->actions;
		if (event_qual != NULL && *instead_flag)
		{
			Query	   *qual_product;
			RewriteInfo qual_info;

			/* ----------
			 * If there are instead rules with qualifications,
			 * the original query is still performed. But all
			 * the negated rule qualifications of the instead
			 * rules are added so it does it's actions only
			 * in cases where the rule quals of all instead
			 * rules are false. Think of it as the default
			 * action in a case. We save this in *qual_products
			 * so deepRewriteQuery() can add it to the query
			 * list after we mangled it up enough.
			 * ----------
			 */
			if (*qual_products == NIL)
				qual_product = parsetree;
			else
				qual_product = (Query *) nth(0, *qual_products);

			qual_info.event = qual_product->commandType;
			qual_info.new_varno = length(qual_product->rtable) + 2;
			qual_product = CopyAndAddQual(qual_product,
										  actions,
										  event_qual,
										  rt_index,
										  event);

			qual_info.rule_action = qual_product;

			if (event == CMD_INSERT || event == CMD_UPDATE)
				FixNew(&qual_info, qual_product);

			*qual_products = lappend(NIL, qual_product);
		}

		foreach(r, actions)
		{
			Query	   *rule_action = lfirst(r);
			Node	   *rule_qual = copyObject(event_qual);

			if (rule_action->commandType == CMD_NOTHING)
				continue;

			/*--------------------------------------------------
			 * We copy the qualifications of the parsetree
			 * to the action and vice versa. So force
			 * hasSubLinks if one of them has it.
			 *
			 * As of 6.4 only parsetree qualifications can
			 * have sublinks. If this changes, we must make
			 * this a node lookup at the end of rewriting.
			 *
			 * Jan
			 *--------------------------------------------------
			 */
			if (parsetree->hasSubLinks && !rule_action->hasSubLinks)
			{
				rule_action = copyObject(rule_action);
				rule_action->hasSubLinks = TRUE;
			}
			if (!parsetree->hasSubLinks && rule_action->hasSubLinks)
			{
				parsetree->hasSubLinks = TRUE;
			}

			/*--------------------------------------------------
			 * Step 1:
			 *	  Rewrite current.attribute or current to tuple variable
			 *	  this appears to be done in parser?
			 *--------------------------------------------------
			 */
			info = gatherRewriteMeta(parsetree, rule_action, rule_qual,
									 rt_index, event, instead_flag);

			/* handle escapable cases, or those handled by other code */
			if (info->nothing)
			{
				if (*instead_flag)
					return NIL;
				else
					continue;
			}

			if (info->action == info->event &&
				info->event == CMD_SELECT)
				continue;

			/*
			 * Event Qualification forces copying of parsetree and
			 * splitting into two queries one w/rule_qual, one w/NOT
			 * rule_qual. Also add user query qual onto rule action
			 */
			qual = parsetree->qual;
			AddQual(info->rule_action, qual);

			if (info->rule_qual != NULL)
				AddQual(info->rule_action, info->rule_qual);

			/*--------------------------------------------------
			 * Step 2:
			 *	  Rewrite new.attribute w/ right hand side of target-list
			 *	  entry for appropriate field name in insert/update
			 *--------------------------------------------------
			 */
			if ((info->event == CMD_INSERT) || (info->event == CMD_UPDATE))
				FixNew(info, parsetree);

			/*--------------------------------------------------
			 * Step 3:
			 *	  rewriting due to retrieve rules
			 *--------------------------------------------------
			 */
			info->rule_action->rtable = info->rt;
			/*
			ProcessRetrieveQuery(info->rule_action, info->rt,
								 &orig_instead_flag, TRUE);
			*/

			/*--------------------------------------------------
			 * Step 4
			 *	  Simplify? hey, no algorithm for simplification... let
			 *	  the planner do it.
			 *--------------------------------------------------
			 */
			results = lappend(results, info->rule_action);

			pfree(info);
		}

		/* ----------
		 * If this was an unqualified instead rule,
		 * throw away an eventually saved 'default' parsetree
		 * ----------
		 */
		if (event_qual == NULL && *instead_flag)
			*qual_products = NIL;
	}
	return results;
}



static List *
RewriteQuery(Query *parsetree, bool *instead_flag, List **qual_products)
{
	CmdType		event;
	List	   	*product_queries = NIL;
	int		result_relation = 0;
	RangeTblEntry	*rt_entry;
	Relation	rt_entry_relation = NULL;
	RuleLock	*rt_entry_locks = NULL;

	Assert(parsetree != NULL);

	event = parsetree->commandType;

	/*
	 * SELECT rules are handled later when we have all the
	 * queries that should get executed
	 */
	if (event == CMD_SELECT)
		return NIL;

	/*
	 * Utilities aren't rewritten at all - why is this here?
	 */
	if (event == CMD_UTILITY)
		return NIL;

	/*
	 * only for a delete may the targetlist be NULL
	 */
	if (event != CMD_DELETE)
		Assert(parsetree->targetList != NULL);

	result_relation = parsetree->resultRelation;

	/*
	 * the statement is an update, insert or delete - fire rules
	 * on it.
	 */
	rt_entry = rt_fetch(result_relation, parsetree->rtable);
	rt_entry_relation = heap_openr(rt_entry->relname);
	rt_entry_locks = rt_entry_relation->rd_rules;
	heap_close(rt_entry_relation);

	if (rt_entry_locks != NULL)
	{
		List	   *locks =
		matchLocks(event, rt_entry_locks, result_relation, parsetree);

		product_queries =
			fireRules(parsetree,
					  result_relation,
					  event,
					  instead_flag,
					  locks,
					  qual_products);
	}

	return product_queries;

}


/*
 * to avoid infinite recursion, we restrict the number of times a query
 * can be rewritten. Detecting cycles is left for the reader as an excercise.
 */
#ifndef REWRITE_INVOKE_MAX
#define REWRITE_INVOKE_MAX		10
#endif

static int	numQueryRewriteInvoked = 0;

/*
 * deepRewriteQuery -
 *	  rewrites the query and apply the rules again on the queries rewritten
 */
static List *
deepRewriteQuery(Query *parsetree)
{
	List	   *n;
	List	   *rewritten = NIL;
	List	   *result = NIL;
	bool		instead;
	List	   *qual_products = NIL;



	if (++numQueryRewriteInvoked > REWRITE_INVOKE_MAX)
	{
		elog(ERROR, "query rewritten %d times, may contain cycles",
			 numQueryRewriteInvoked - 1);
	}

	instead = FALSE;
	result = RewriteQuery(parsetree, &instead, &qual_products);

	foreach(n, result)
	{
		Query	   *pt = lfirst(n);
		List	   *newstuff = NIL;

		newstuff = deepRewriteQuery(pt);
		if (newstuff != NIL)
			rewritten = nconc(rewritten, newstuff);
	}

	/* ----------
	 * qual_products are the original query with the negated
	 * rule qualification of an instead rule
	 * ----------
	 */
	if (qual_products != NIL)
		rewritten = nconc(rewritten, qual_products);

	/* ----------
	 * The original query is appended last if not instead
	 * because update and delete rule actions might not do
	 * anything if they are invoked after the update or
	 * delete is performed. The command counter increment
	 * between the query execution makes the deleted (and
	 * maybe the updated) tuples disappear so the scans
	 * for them in the rule actions cannot find them.
	 * ----------
	 */
	if (!instead)
		rewritten = lappend(rewritten, parsetree);

	return rewritten;
}


/*
 * QueryOneRewrite -
 *	  rewrite one query
 */
static List *
QueryRewriteOne(Query *parsetree)
{
	numQueryRewriteInvoked = 0;

	/*
	 * take a deep breath and apply all the rewrite rules - ay
	 */
	return deepRewriteQuery(parsetree);
}


/* ----------
 * RewritePreprocessQuery -
 *	adjust details in the parsetree, the rule system
 *	depends on
 * ----------
 */
static void
RewritePreprocessQuery(Query *parsetree)
{
	/* ----------
	 * if the query has a resultRelation, reassign the
	 * result domain numbers to the attribute numbers in the
	 * target relation. FixNew() depends on it when replacing
	 * *new* references in a rule action by the expressions
	 * from the rewritten query.
	 * ----------
	 */
	if (parsetree->resultRelation > 0)
	{
		RangeTblEntry *rte;
		Relation	rd;
		List	   *tl;
		TargetEntry *tle;
		int			resdomno;

		rte = (RangeTblEntry *) nth(parsetree->resultRelation - 1,
									parsetree->rtable);
		rd = heap_openr(rte->relname);

		foreach(tl, parsetree->targetList)
		{
			tle = (TargetEntry *) lfirst(tl);
			resdomno = attnameAttNum(rd, tle->resdom->resname);
			tle->resdom->resno = resdomno;
		}

		heap_close(rd);
	}
}


/*
 * QueryRewrite -
 *	  rewrite one query via query rewrite system, possibly returning 0
 *	  or many queries
 */
List *
QueryRewrite(Query *parsetree)
{
	List		*querylist;
	List		*results = NIL;
	List		*l;
	Query		*query;

	/*
	 * Step 1
	 *
	 * There still seems something broken with the resdom numbers
	 * so we reassign them first.
	 */
	RewritePreprocessQuery(parsetree);

	/*
	 * Step 2
	 *
	 * Apply all non-SELECT rules possibly getting 0 or many queries
	 */
	querylist = QueryRewriteOne(parsetree);

	/*
	 * Step 3
	 *
	 * Apply all the RIR rules on each query
	 */
	foreach (l, querylist) {
		query = (Query *)lfirst(l);
		results = lappend(results, fireRIRrules(query));
	}

	return results;
}


