/*-------------------------------------------------------------------------
 *
 * var.c--
 *	  Var node manipulation routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/var.c,v 1.9 1998/02/10 04:01:27 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>

#include "postgres.h"

#include <nodes/relation.h>

#include "nodes/primnodes.h"
#include "nodes/nodeFuncs.h"

#include "optimizer/internal.h"
#include "optimizer/clauses.h"
#include "optimizer/var.h"

#include "parser/parsetree.h"

/*
 *		find_varnos
 *
 *		Descends down part of a parsetree (qual or tlist),
 *
 *		XXX assumes varno's are always integers, which shouldn't be true...
 *		(though it currently is, see primnodes.h)
 */
List	   *
pull_varnos(Node *me)
{
	List	   *i,
			   *result = NIL;

	if (me == NULL)
		return (NIL);

	switch (nodeTag(me))
	{
		case T_List:
			foreach(i, (List *) me)
			{
				result = nconc(result, pull_varnos(lfirst(i)));
			}
			break;
		case T_ArrayRef:
			foreach(i, ((ArrayRef *) me)->refupperindexpr)
				result = nconc(result, pull_varnos(lfirst(i)));
			foreach(i, ((ArrayRef *) me)->reflowerindexpr)
				result = nconc(result, pull_varnos(lfirst(i)));
			result = nconc(result, pull_varnos(((ArrayRef *) me)->refassgnexpr));
			break;
		case T_Var:
			result = lconsi(((Var *) me)->varno, NIL);
			break;
		default:
			break;
	}
	return (result);
}

/*
 * contain_var_clause--
 *	  Recursively find var nodes from a clause by pulling vars from the
 *	  left and right operands of the clause.
 *
 *	  Returns true if any varnode found.
 */
bool
contain_var_clause(Node *clause)
{
	if (clause == NULL)
		return FALSE;
	else if (IsA(clause, Var))
		return TRUE;
	else if (IsA(clause, Iter))
		return contain_var_clause(((Iter *) clause)->iterexpr);
	else if (single_node(clause))
		return FALSE;
	else if (or_clause(clause) || and_clause(clause))
	{
		List	   *temp;

		foreach(temp, ((Expr *) clause)->args)
		{
			if (contain_var_clause(lfirst(temp)))
				return TRUE;
		}
		return FALSE;
	}
	else if (is_funcclause(clause))
	{
		List	   *temp;

		foreach(temp, ((Expr *) clause)->args)
		{
			if (contain_var_clause(lfirst(temp)))
				return TRUE;
		}
		return FALSE;
	}
	else if (IsA(clause, ArrayRef))
	{
		List	   *temp;

		foreach(temp, ((ArrayRef *) clause)->refupperindexpr)
		{
			if (contain_var_clause(lfirst(temp)))
				return TRUE;
		}
		foreach(temp, ((ArrayRef *) clause)->reflowerindexpr)
		{
			if (contain_var_clause(lfirst(temp)))
				return TRUE;
		}
		if (contain_var_clause(((ArrayRef *) clause)->refexpr))
			return TRUE;
		if (contain_var_clause(((ArrayRef *) clause)->refassgnexpr))
			return TRUE;
		return FALSE;
	}
	else if (not_clause(clause))
		return contain_var_clause((Node *) get_notclausearg((Expr *) clause));
	else if (is_opclause(clause))
		return (contain_var_clause((Node *) get_leftop((Expr *) clause)) ||
			  contain_var_clause((Node *) get_rightop((Expr *) clause)));

	return FALSE;
}

/*
 * pull_var_clause--
 *	  Recursively pulls all var nodes from a clause by pulling vars from the
 *	  left and right operands of the clause.
 *
 *	  Returns list of varnodes found.
 */
List	   *
pull_var_clause(Node *clause)
{
	List	   *retval = NIL;

	if (clause == NULL)
		return (NIL);
	else if (IsA(clause, Var))
		retval = lcons(clause, NIL);
	else if (IsA(clause, Iter))
		retval = pull_var_clause(((Iter *) clause)->iterexpr);
	else if (single_node(clause))
		retval = NIL;
	else if (or_clause(clause) || and_clause(clause))
	{
		List	   *temp;

		foreach(temp, ((Expr *) clause)->args)
			retval = nconc(retval, pull_var_clause(lfirst(temp)));
	}
	else if (is_funcclause(clause))
	{
		List	   *temp;

		foreach(temp, ((Expr *) clause)->args)
			retval = nconc(retval, pull_var_clause(lfirst(temp)));
	}
	else if (IsA(clause, Aggreg))
	{
		retval = pull_var_clause(((Aggreg *) clause)->target);
	}
	else if (IsA(clause, ArrayRef))
	{
		List	   *temp;

		foreach(temp, ((ArrayRef *) clause)->refupperindexpr)
			retval = nconc(retval, pull_var_clause(lfirst(temp)));
		foreach(temp, ((ArrayRef *) clause)->reflowerindexpr)
			retval = nconc(retval, pull_var_clause(lfirst(temp)));
		retval = nconc(retval,
					   pull_var_clause(((ArrayRef *) clause)->refexpr));
		retval = nconc(retval,
				   pull_var_clause(((ArrayRef *) clause)->refassgnexpr));
	}
	else if (not_clause(clause))
		retval = pull_var_clause((Node *) get_notclausearg((Expr *) clause));
	else if (is_opclause(clause))
		retval = nconc(pull_var_clause((Node *) get_leftop((Expr *) clause)),
				 pull_var_clause((Node *) get_rightop((Expr *) clause)));
	else
		retval = NIL;

	return (retval);
}

/*
 *		var_equal
 *
 *		Returns t iff two var nodes correspond to the same attribute.
 */
bool
var_equal(Var *var1, Var *var2)
{
	if (IsA(var1, Var) &&IsA(var2, Var) &&
		(((Var *) var1)->varno == ((Var *) var2)->varno) &&
		(((Var *) var1)->vartype == ((Var *) var2)->vartype) &&
		(((Var *) var1)->vartypmod == ((Var *) var2)->vartypmod) &&
		(((Var *) var1)->varlevelsup == ((Var *) var2)->varlevelsup) &&
		(((Var *) var1)->varattno == ((Var *) var2)->varattno))
	{

		return (true);
	}
	else
		return (false);
}
