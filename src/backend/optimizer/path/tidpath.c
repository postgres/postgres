/*-------------------------------------------------------------------------
 *
 * tidpath.c
 *	  Routines to determine which tids are usable for scanning a
 *	  given relation, and create TidPaths accordingly.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/path/tidpath.c,v 1.6 2000/04/12 17:15:20 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <math.h>

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_operator.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/restrictinfo.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"

static void create_tidscan_joinpaths(RelOptInfo *rel);
static List *TidqualFromRestrictinfo(List *relids, List *restrictinfo);
static bool isEvaluable(int varno, Node *node);
static Node *TidequalClause(int varno, Expr *node);
static List *TidqualFromExpr(int varno, Expr *expr);

static
bool
isEvaluable(int varno, Node *node)
{
	List	   *lst;
	Expr	   *expr;

	if (IsA(node, Const))
		return true;
	if (IsA(node, Param))
		return true;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varno == varno)
			return false;
		return true;
	}
	if (!is_funcclause(node))
		return false;
	expr = (Expr *) node;
	foreach(lst, expr->args)
	{
		if (!isEvaluable(varno, lfirst(lst)))
			return false;
	}

	return true;
}

/*
 *	The 2nd parameter should be an opclause
 *	Extract the right node if the opclause is CTID= ....
 *	  or	the left  node if the opclause is ....=CTID
 */
static
Node *
TidequalClause(int varno, Expr *node)
{
	Node	   *rnode = 0,
			   *arg1,
			   *arg2,
			   *arg;
	Oper	   *oper;
	Var		   *var;
	Const	   *aconst;
	Param	   *param;
	Expr	   *expr;

	if (!node->oper)
		return rnode;
	if (!node->args)
		return rnode;
	if (length(node->args) != 2)
		return rnode;
	oper = (Oper *) node->oper;
	if (oper->opno != TIDEqualOperator)
		return rnode;
	arg1 = lfirst(node->args);
	arg2 = lsecond(node->args);

	arg = (Node *) 0;
	if (IsA(arg1, Var))
	{
		var = (Var *) arg1;
		if (var->varno == varno &&
			var->varattno == SelfItemPointerAttributeNumber &&
			var->vartype == TIDOID)
			arg = arg2;
		else if (var->varnoold == varno &&
				 var->varoattno == SelfItemPointerAttributeNumber &&
				 var->vartype == TIDOID)
			arg = arg2;
	}
	if ((!arg) && IsA(arg2, Var))
	{
		var = (Var *) arg2;
		if (var->varno == varno &&
			var->varattno == SelfItemPointerAttributeNumber &&
			var->vartype == TIDOID)
			arg = arg1;
	}
	if (!arg)
		return rnode;
	switch (nodeTag(arg))
	{
		case T_Const:
			aconst = (Const *) arg;
			if (aconst->consttype != TIDOID)
				return rnode;
			if (aconst->constbyval)
				return rnode;
			rnode = arg;
			break;
		case T_Param:
			param = (Param *) arg;
			if (param->paramtype != TIDOID)
				return rnode;
			rnode = arg;
			break;
		case T_Var:
			var = (Var *) arg;
			if (var->varno == varno ||
				var->vartype != TIDOID)
				return rnode;
			rnode = arg;
			break;
		case T_Expr:
			expr = (Expr *) arg;
			if (expr->typeOid != TIDOID)
				return rnode;
			if (expr->opType != FUNC_EXPR)
				return rnode;
			if (isEvaluable(varno, (Node *) expr))
				rnode = arg;
			break;
		default:
			break;
	}
	return rnode;
}

/*
 *	Extract the list of CTID values from a specified expr node.
 *	When the expr node is an or_clause,we try to extract CTID
 *	values from all member nodes. However we would discard them
 *	all if we couldn't extract CTID values from a member node.
 *	When the expr node is an and_clause,we return the list of
 *	CTID values if we could extract the CTID values from a member
 *	node.
 */
static
List *
TidqualFromExpr(int varno, Expr *expr)
{
	List	   *rlst = NIL,
			   *lst,
			   *frtn;
	Node	   *node = (Node *) expr,
			   *rnode;

	if (is_opclause(node))
	{
		rnode = TidequalClause(varno, expr);
		if (rnode)
			rlst = lcons(rnode, rlst);
	}
	else if (and_clause(node))
	{
		foreach(lst, expr->args)
		{
			node = lfirst(lst);
			if (!IsA(node, Expr))
				continue;
			rlst = TidqualFromExpr(varno, (Expr *) node);
			if (rlst)
				break;
		}
	}
	else if (or_clause(node))
	{
		foreach(lst, expr->args)
		{
			node = lfirst(lst);
			if (IsA(node, Expr) &&
				(frtn = TidqualFromExpr(varno, (Expr *) node)))
				rlst = nconc(rlst, frtn);
			else
			{
				if (rlst)
					freeList(rlst);
				rlst = NIL;
				break;
			}
		}
	}
	return rlst;
}

static List *
TidqualFromRestrictinfo(List *relids, List *restrictinfo)
{
	List	   *lst,
			   *rlst = NIL;
	int			varno;
	Node	   *node;
	Expr	   *expr;

	if (length(relids) != 1)
		return NIL;
	varno = lfirsti(relids);
	foreach(lst, restrictinfo)
	{
		node = lfirst(lst);
		if (!IsA(node, RestrictInfo))
			continue;
		expr = ((RestrictInfo *) node)->clause;
		rlst = TidqualFromExpr(varno, expr);
		if (rlst)
			break;
	}
	return rlst;
}

/*
 * create_tidscan_joinpaths
 *	  Create innerjoin paths if there are suitable joinclauses.
 *
 * XXX does this actually work?
 */
static void
create_tidscan_joinpaths(RelOptInfo *rel)
{
	List	   *rlst = NIL,
			   *lst;

	foreach(lst, rel->joininfo)
	{
		JoinInfo   *joininfo = (JoinInfo *) lfirst(lst);
		List	   *restinfo,
				   *tideval;

		restinfo = joininfo->jinfo_restrictinfo;
		tideval = TidqualFromRestrictinfo(rel->relids, restinfo);
		if (length(tideval) == 1)
		{
			TidPath    *pathnode = makeNode(TidPath);

			pathnode->path.pathtype = T_TidScan;
			pathnode->path.parent = rel;
			pathnode->path.pathkeys = NIL;
			pathnode->tideval = tideval;
			pathnode->unjoined_relids = joininfo->unjoined_relids;

			cost_tidscan(&pathnode->path, rel, tideval);

			rlst = lappend(rlst, pathnode);
		}
	}
	rel->innerjoin = nconc(rel->innerjoin, rlst);
}

/*
 * create_tidscan_paths
 *	  Creates paths corresponding to tid direct scans of the given rel.
 *	  Candidate paths are added to the rel's pathlist (using add_path).
 */
void
create_tidscan_paths(Query *root, RelOptInfo *rel)
{
	List	   *tideval = TidqualFromRestrictinfo(rel->relids,
												  rel->baserestrictinfo);

	if (tideval)
		add_path(rel, (Path *) create_tidscan_path(rel, tideval));
	create_tidscan_joinpaths(rel);
}
