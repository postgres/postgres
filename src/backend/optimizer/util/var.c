/*-------------------------------------------------------------------------
 *
 * var.c
 *	  Var node manipulation routines
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/var.c,v 1.31 2001/04/18 20:42:55 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/plannodes.h"
#include "optimizer/clauses.h"
#include "optimizer/var.h"


typedef struct
{
	List	   *varlist;
	int			sublevels_up;
} pull_varnos_context;

typedef struct
{
	int			varno;
	int			sublevels_up;
} contain_whole_tuple_var_context;

typedef struct
{
	List	   *varlist;
	bool		includeUpperVars;
} pull_var_clause_context;

static bool pull_varnos_walker(Node *node,
				   pull_varnos_context *context);
static bool contain_whole_tuple_var_walker(Node *node,
				   contain_whole_tuple_var_context *context);
static bool contain_var_clause_walker(Node *node, void *context);
static bool pull_var_clause_walker(Node *node,
					   pull_var_clause_context *context);


/*
 *		pull_varnos
 *
 *		Create a list of all the distinct varnos present in a parsetree.
 *		Only varnos that reference level-zero rtable entries are considered.
 *
 * NOTE: this is used on not-yet-planned expressions.  It may therefore find
 * bare SubLinks, and if so it needs to recurse into them to look for uplevel
 * references to the desired rtable level!  But when we find a completed
 * SubPlan, we only need to look at the parameters passed to the subplan.
 */
List *
pull_varnos(Node *node)
{
	pull_varnos_context context;

	context.varlist = NIL;
	context.sublevels_up = 0;

	/*
	 * Must be prepared to start with a Query or a bare expression tree;
	 * if it's a Query, go straight to query_tree_walker to make sure that
	 * sublevels_up doesn't get incremented prematurely.
	 */
	if (node && IsA(node, Query))
		query_tree_walker((Query *) node, pull_varnos_walker,
						  (void *) &context, true);
	else
		pull_varnos_walker(node, &context);

	return context.varlist;
}

static bool
pull_varnos_walker(Node *node, pull_varnos_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup == context->sublevels_up &&
			!intMember(var->varno, context->varlist))
			context->varlist = lconsi(var->varno, context->varlist);
		return false;
	}
	if (is_subplan(node))
	{

		/*
		 * Already-planned subquery.  Examine the args list (parameters to
		 * be passed to subquery), as well as the "oper" list which is
		 * executed by the outer query.  But short-circuit recursion into
		 * the subquery itself, which would be a waste of effort.
		 */
		Expr	   *expr = (Expr *) node;

		if (pull_varnos_walker((Node *) ((SubPlan *) expr->oper)->sublink->oper,
							   context))
			return true;
		if (pull_varnos_walker((Node *) expr->args,
							   context))
			return true;
		return false;
	}
	if (IsA(node, Query))
	{
		/* Recurse into RTE subquery or not-yet-planned sublink subquery */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node, pull_varnos_walker,
								   (void *) context, true);
		context->sublevels_up--;
		return result;
	}
	return expression_tree_walker(node, pull_varnos_walker,
								  (void *) context);
}


/*
 *		contain_whole_tuple_var
 *
 *		Detect whether a parsetree contains any references to the whole
 *		tuple of a given rtable entry (ie, a Var with varattno = 0).
 *
 * NOTE: this is used on not-yet-planned expressions.  It may therefore find
 * bare SubLinks, and if so it needs to recurse into them to look for uplevel
 * references to the desired rtable entry!  But when we find a completed
 * SubPlan, we only need to look at the parameters passed to the subplan.
 */
bool
contain_whole_tuple_var(Node *node, int varno, int levelsup)
{
	contain_whole_tuple_var_context context;

	context.varno = varno;
	context.sublevels_up = levelsup;

	/*
	 * Must be prepared to start with a Query or a bare expression tree;
	 * if it's a Query, go straight to query_tree_walker to make sure that
	 * sublevels_up doesn't get incremented prematurely.
	 */
	if (node && IsA(node, Query))
		return query_tree_walker((Query *) node,
								 contain_whole_tuple_var_walker,
								 (void *) &context, true);
	else
		return contain_whole_tuple_var_walker(node, &context);
}

static bool
contain_whole_tuple_var_walker(Node *node,
							   contain_whole_tuple_var_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varno == context->varno &&
			var->varlevelsup == context->sublevels_up &&
			var->varattno == InvalidAttrNumber)
			return true;
		return false;
	}
	if (is_subplan(node))
	{

		/*
		 * Already-planned subquery.  Examine the args list (parameters to
		 * be passed to subquery), as well as the "oper" list which is
		 * executed by the outer query.  But short-circuit recursion into
		 * the subquery itself, which would be a waste of effort.
		 */
		Expr	   *expr = (Expr *) node;

		if (contain_whole_tuple_var_walker((Node *) ((SubPlan *) expr->oper)->sublink->oper,
										   context))
			return true;
		if (contain_whole_tuple_var_walker((Node *) expr->args,
										   context))
			return true;
		return false;
	}
	if (IsA(node, Query))
	{
		/* Recurse into RTE subquery or not-yet-planned sublink subquery */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node,
								   contain_whole_tuple_var_walker,
								   (void *) context, true);
		context->sublevels_up--;
		return result;
	}
	return expression_tree_walker(node, contain_whole_tuple_var_walker,
								  (void *) context);
}


/*
 * contain_var_clause
 *	  Recursively scan a clause to discover whether it contains any Var nodes
 *	  (of the current query level).
 *
 *	  Returns true if any varnode found.
 *
 * Does not examine subqueries, therefore must only be used after reduction
 * of sublinks to subplans!
 */
bool
contain_var_clause(Node *node)
{
	return contain_var_clause_walker(node, NULL);
}

static bool
contain_var_clause_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		if (((Var *) node)->varlevelsup == 0)
			return true;		/* abort the tree traversal and return
								 * true */
		return false;
	}
	return expression_tree_walker(node, contain_var_clause_walker, context);
}


/*
 * pull_var_clause
 *	  Recursively pulls all var nodes from an expression clause.
 *
 *	  Upper-level vars (with varlevelsup > 0) are included only
 *	  if includeUpperVars is true.	Most callers probably want
 *	  to ignore upper-level vars.
 *
 *	  Returns list of varnodes found.  Note the varnodes themselves are not
 *	  copied, only referenced.
 *
 * Does not examine subqueries, therefore must only be used after reduction
 * of sublinks to subplans!
 */
List *
pull_var_clause(Node *node, bool includeUpperVars)
{
	pull_var_clause_context context;

	context.varlist = NIL;
	context.includeUpperVars = includeUpperVars;

	pull_var_clause_walker(node, &context);
	return context.varlist;
}

static bool
pull_var_clause_walker(Node *node, pull_var_clause_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		if (((Var *) node)->varlevelsup == 0 || context->includeUpperVars)
			context->varlist = lappend(context->varlist, node);
		return false;
	}
	return expression_tree_walker(node, pull_var_clause_walker,
								  (void *) context);
}
