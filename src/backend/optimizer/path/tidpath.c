/*-------------------------------------------------------------------------
 *
 * tidpath.c
 *	  Routines to determine which tids are usable for scanning a
 *	  given relation, and create TidPaths accordingly.
 *
 * Portions Copyright (c) 1996-2004, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/path/tidpath.c,v 1.21 2004/08/29 04:12:33 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "catalog/pg_operator.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "parser/parse_coerce.h"
#include "utils/lsyscache.h"

static List *TidqualFromRestrictinfo(Relids relids, List *restrictinfo);
static bool isEvaluable(int varno, Node *node);
static Node *TidequalClause(int varno, OpExpr *node);
static List *TidqualFromExpr(int varno, Expr *expr);

static bool
isEvaluable(int varno, Node *node)
{
	ListCell   *l;
	FuncExpr   *expr;

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
	expr = (FuncExpr *) node;
	foreach(l, expr->args)
	{
		if (!isEvaluable(varno, lfirst(l)))
			return false;
	}

	return true;
}

/*
 *	The 2nd parameter should be an opclause
 *	Extract the right node if the opclause is CTID= ....
 *	  or	the left  node if the opclause is ....=CTID
 */
static Node *
TidequalClause(int varno, OpExpr *node)
{
	Node	   *rnode = NULL,
			   *arg1,
			   *arg2,
			   *arg;
	Var		   *var;
	Const	   *aconst;
	Param	   *param;
	FuncExpr   *expr;

	if (node->opno != TIDEqualOperator)
		return rnode;
	if (list_length(node->args) != 2)
		return rnode;
	arg1 = linitial(node->args);
	arg2 = lsecond(node->args);

	arg = NULL;
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
		case T_FuncExpr:
			expr = (FuncExpr *) arg;
			if (expr->funcresulttype != TIDOID)
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
static List *
TidqualFromExpr(int varno, Expr *expr)
{
	List	   *rlst = NIL,
			   *frtn;
	ListCell   *l;
	Node	   *node = (Node *) expr,
			   *rnode;

	if (is_opclause(node))
	{
		rnode = TidequalClause(varno, (OpExpr *) expr);
		if (rnode)
			rlst = lcons(rnode, rlst);
	}
	else if (and_clause(node))
	{
		foreach(l, ((BoolExpr *) expr)->args)
		{
			node = (Node *) lfirst(l);
			rlst = TidqualFromExpr(varno, (Expr *) node);
			if (rlst)
				break;
		}
	}
	else if (or_clause(node))
	{
		foreach(l, ((BoolExpr *) expr)->args)
		{
			node = (Node *) lfirst(l);
			frtn = TidqualFromExpr(varno, (Expr *) node);
			if (frtn)
				rlst = list_concat(rlst, frtn);
			else
			{
				if (rlst)
					list_free(rlst);
				rlst = NIL;
				break;
			}
		}
	}
	return rlst;
}

static List *
TidqualFromRestrictinfo(Relids relids, List *restrictinfo)
{
	ListCell   *l;
	List	   *rlst = NIL;
	int			varno;
	Node	   *node;
	Expr	   *expr;

	if (bms_membership(relids) != BMS_SINGLETON)
		return NIL;
	varno = bms_singleton_member(relids);
	foreach(l, restrictinfo)
	{
		node = (Node *) lfirst(l);
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
		add_path(rel, (Path *) create_tidscan_path(root, rel, tideval));
}
