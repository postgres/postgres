/*-------------------------------------------------------------------------
 *
 * var.c
 *	  Var node manipulation routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/var.c,v 1.20.2.1 1999/08/02 06:27:09 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>

#include "postgres.h"


#include "optimizer/clauses.h"
#include "optimizer/var.h"



static bool pull_varnos_walker(Node *node, List **listptr);
static bool contain_var_clause_walker(Node *node, void *context);
static bool pull_var_clause_walker(Node *node, List **listptr);


/*
 *		pull_varnos
 *
 *		Create a list of all the distinct varnos present in a parsetree
 *		(tlist or qual).
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
		if (!intMember(var->varno, *listptr))
			*listptr = lconsi(var->varno, *listptr);
		return false;
	}
	return expression_tree_walker(node, pull_varnos_walker, (void *) listptr);
}

/*
 * contain_var_clause
 *	  Recursively scan a clause to discover whether it contains any Var nodes.
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
		return true;			/* abort the tree traversal and return true */
	return expression_tree_walker(node, contain_var_clause_walker, context);
}

/*
 * pull_var_clause
 *	  Recursively pulls all var nodes from an expression clause.
 *
 *	  Returns list of varnodes found.  Note the varnodes themselves are not
 *	  copied, only referenced.
 */
List *
pull_var_clause(Node *clause)
{
	List	   *result = NIL;

	pull_var_clause_walker(clause, &result);
	return result;
}

static bool
pull_var_clause_walker(Node *node, List **listptr)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		*listptr = lappend(*listptr, node);
		return false;
	}
	return expression_tree_walker(node, pull_var_clause_walker,
								  (void *) listptr);
}

/*
 *		var_equal
 *
 *		The only difference between this an equal() is that this does not
 *		test varnoold and varoattno.
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
		Assert(((Var *) var1)->varlevelsup == 0);
		return true;
	}
	else
		return false;
}
