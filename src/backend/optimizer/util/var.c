/*-------------------------------------------------------------------------
 *
 * var.c
 *	  Var node manipulation routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/var.c,v 1.24 1999/08/26 05:09:06 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>

#include "postgres.h"

#include "optimizer/clauses.h"
#include "optimizer/var.h"


typedef struct {
	List	   *varlist;
	bool		includeUpperVars;
} pull_var_clause_context;

static bool pull_varnos_walker(Node *node, List **listptr);
static bool contain_var_clause_walker(Node *node, void *context);
static bool pull_var_clause_walker(Node *node,
								   pull_var_clause_context *context);


/*
 *		pull_varnos
 *
 *		Create a list of all the distinct varnos present in a parsetree
 *		(tlist or qual).  Note that only varnos attached to level-zero
 *		Vars are considered --- upper Vars refer to some other rtable!
 */
List *
pull_varnos(Node *node)
{
	List	   *result = NIL;

	pull_varnos_walker(node, &result);
	return result;
}

static bool
pull_varnos_walker(Node *node, List **listptr)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var	   *var = (Var *) node;
		if (var->varlevelsup == 0 && !intMember(var->varno, *listptr))
			*listptr = lconsi(var->varno, *listptr);
		return false;
	}
	return expression_tree_walker(node, pull_varnos_walker, (void *) listptr);
}

/*
 * contain_var_clause
 *	  Recursively scan a clause to discover whether it contains any Var nodes
 *	  (of the current query level).
 *
 *	  Returns true if any varnode found.
 */
bool
contain_var_clause(Node *clause)
{
	return contain_var_clause_walker(clause, NULL);
}

static bool
contain_var_clause_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		if (((Var *) node)->varlevelsup == 0)
			return true;		/* abort the tree traversal and return true */
		return false;
	}
	return expression_tree_walker(node, contain_var_clause_walker, context);
}

/*
 * pull_var_clause
 *	  Recursively pulls all var nodes from an expression clause.
 *
 *	  Upper-level vars (with varlevelsup > 0) are included only
 *	  if includeUpperVars is true.  Most callers probably want
 *	  to ignore upper-level vars.
 *
 *	  Returns list of varnodes found.  Note the varnodes themselves are not
 *	  copied, only referenced.
 */
List *
pull_var_clause(Node *clause, bool includeUpperVars)
{
	pull_var_clause_context		context;

	context.varlist = NIL;
	context.includeUpperVars = includeUpperVars;

	pull_var_clause_walker(clause, &context);
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
