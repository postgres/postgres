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
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/var.c,v 1.35 2002/04/11 20:00:00 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/plannodes.h"
#include "optimizer/clauses.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "parser/parse_coerce.h"


typedef struct
{
	List	   *varlist;
	int			sublevels_up;
} pull_varnos_context;

typedef struct
{
	int			varno;
	int			varattno;
	int			sublevels_up;
} contain_var_reference_context;

typedef struct
{
	List	   *varlist;
	bool		includeUpperVars;
} pull_var_clause_context;

typedef struct
{
	Query	   *root;
	int			expandRTI;
} flatten_join_alias_vars_context;

static bool pull_varnos_walker(Node *node,
				   pull_varnos_context *context);
static bool contain_var_reference_walker(Node *node,
							 contain_var_reference_context *context);
static bool contain_var_clause_walker(Node *node, void *context);
static bool pull_var_clause_walker(Node *node,
					   pull_var_clause_context *context);
static Node *flatten_join_alias_vars_mutator(Node *node,
						flatten_join_alias_vars_context *context);
static Node *flatten_join_alias_var(Var *var, Query *root, int expandRTI);
static Node *find_jointree_item(Node *jtnode, int rtindex);


/*
 *		pull_varnos
 *
 *		Create a list of all the distinct varnos present in a parsetree.
 *		Only varnos that reference level-zero rtable entries are considered.
 *
 * NOTE: this is used on not-yet-planned expressions.  It may therefore find
 * bare SubLinks, and if so it needs to recurse into them to look for uplevel
 * references to the desired rtable level!	But when we find a completed
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
 *		contain_var_reference
 *
 *		Detect whether a parsetree contains any references to a specified
 *		attribute of a specified rtable entry.
 *
 * NOTE: this is used on not-yet-planned expressions.  It may therefore find
 * bare SubLinks, and if so it needs to recurse into them to look for uplevel
 * references to the desired rtable entry!	But when we find a completed
 * SubPlan, we only need to look at the parameters passed to the subplan.
 */
bool
contain_var_reference(Node *node, int varno, int varattno, int levelsup)
{
	contain_var_reference_context context;

	context.varno = varno;
	context.varattno = varattno;
	context.sublevels_up = levelsup;

	/*
	 * Must be prepared to start with a Query or a bare expression tree;
	 * if it's a Query, go straight to query_tree_walker to make sure that
	 * sublevels_up doesn't get incremented prematurely.
	 */
	if (node && IsA(node, Query))
		return query_tree_walker((Query *) node,
								 contain_var_reference_walker,
								 (void *) &context, true);
	else
		return contain_var_reference_walker(node, &context);
}

static bool
contain_var_reference_walker(Node *node,
							 contain_var_reference_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varno == context->varno &&
			var->varattno == context->varattno &&
			var->varlevelsup == context->sublevels_up)
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

		if (contain_var_reference_walker((Node *) ((SubPlan *) expr->oper)->sublink->oper,
										 context))
			return true;
		if (contain_var_reference_walker((Node *) expr->args,
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
								   contain_var_reference_walker,
								   (void *) context, true);
		context->sublevels_up--;
		return result;
	}
	return expression_tree_walker(node, contain_var_reference_walker,
								  (void *) context);
}


/*
 *		contain_whole_tuple_var
 *
 *		Detect whether a parsetree contains any references to the whole
 *		tuple of a given rtable entry (ie, a Var with varattno = 0).
 */
bool
contain_whole_tuple_var(Node *node, int varno, int levelsup)
{
	return contain_var_reference(node, varno, InvalidAttrNumber, levelsup);
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


/*
 * flatten_join_alias_vars
 *	  Whereever possible, replace Vars that reference JOIN outputs with
 *	  references to the original relation variables instead.  This allows
 *	  quals involving such vars to be pushed down.  Vars that cannot be
 *	  simplified to non-join Vars are replaced by COALESCE expressions
 *	  if they have varno = expandRTI, and are left as JOIN RTE references
 *	  otherwise.  (Pass expandRTI = 0 to prevent all COALESCE expansion.)
 *
 *	  Upper-level vars (with varlevelsup > 0) are ignored; normally there
 *	  should not be any by the time this routine is called.
 *
 * Does not examine subqueries, therefore must only be used after reduction
 * of sublinks to subplans!
 */
Node *
flatten_join_alias_vars(Node *node, Query *root, int expandRTI)
{
	flatten_join_alias_vars_context context;

	context.root = root;
	context.expandRTI = expandRTI;

	return flatten_join_alias_vars_mutator(node, &context);
}

static Node *
flatten_join_alias_vars_mutator(Node *node,
								flatten_join_alias_vars_context *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var	   *var = (Var *) node;

		if (var->varlevelsup != 0)
			return node;		/* no need to copy, really */
		return flatten_join_alias_var(var, context->root, context->expandRTI);
	}
	return expression_tree_mutator(node, flatten_join_alias_vars_mutator,
								   (void *) context);
}

static Node *
flatten_join_alias_var(Var *var, Query *root, int expandRTI)
{
	Index		varno = var->varno;
	AttrNumber	varattno = var->varattno;
	Oid			vartype = var->vartype;
	int32		vartypmod = var->vartypmod;
	JoinExpr   *jexpr = NULL;

	/*
	 * Loop to cope with joins of joins
	 */
	for (;;)
	{
		RangeTblEntry *rte = rt_fetch(varno, root->rtable);
		Index		leftrti,
					rightrti;
		AttrNumber	leftattno,
					rightattno;
		RangeTblEntry *subrte;
		Oid			subtype;
		int32		subtypmod;

		if (rte->rtekind != RTE_JOIN)
			break;				/* reached a non-join RTE */
		/*
		 * Find the RT indexes of the left and right children of the
		 * join node.  We have to search the join tree to do this,
		 * which is a major pain in the neck --- but keeping RT indexes
		 * in other RT entries is worse, because it makes modifying
		 * querytrees difficult.  (Perhaps we can improve on the
		 * rangetable/jointree datastructure someday.)  One thing we
		 * can do is avoid repeated searches while tracing a single
		 * variable down to its baserel.
		 */
		if (jexpr == NULL)
			jexpr = (JoinExpr *)
				find_jointree_item((Node *) root->jointree, varno);
		if (jexpr == NULL ||
			!IsA(jexpr, JoinExpr) ||
			jexpr->rtindex != varno)
			elog(ERROR, "flatten_join_alias_var: failed to find JoinExpr");
		if (IsA(jexpr->larg, RangeTblRef))
			leftrti = ((RangeTblRef *) jexpr->larg)->rtindex;
		else if (IsA(jexpr->larg, JoinExpr))
			leftrti = ((JoinExpr *) jexpr->larg)->rtindex;
		else
		{
			elog(ERROR, "flatten_join_alias_var: unexpected subtree type");
			leftrti = 0;		/* keep compiler quiet */
		}
		if (IsA(jexpr->rarg, RangeTblRef))
			rightrti = ((RangeTblRef *) jexpr->rarg)->rtindex;
		else if (IsA(jexpr->rarg, JoinExpr))
			rightrti = ((JoinExpr *) jexpr->rarg)->rtindex;
		else
		{
			elog(ERROR, "flatten_join_alias_var: unexpected subtree type");
			rightrti = 0;		/* keep compiler quiet */
		}
		/*
		 * See if the join var is from the left side, the right side,
		 * or both (ie, it is a USING/NATURAL JOIN merger column).
		 */
		Assert(varattno > 0);
		leftattno = (AttrNumber) nthi(varattno-1, rte->joinleftcols);
		rightattno = (AttrNumber) nthi(varattno-1, rte->joinrightcols);
		if (leftattno && rightattno)
		{
			/*
			 * Var is a merge var.  If a left or right join, we can replace
			 * it by the left or right input var respectively; we only need
			 * a COALESCE for a full join.  However, beware of the possibility
			 * that there's been a type promotion to make the input vars
			 * compatible; do not replace a var by one of a different type!
			 */
			if (rte->jointype == JOIN_INNER ||
				rte->jointype == JOIN_LEFT)
			{
				subrte = rt_fetch(leftrti, root->rtable);
				get_rte_attribute_type(subrte, leftattno,
									   &subtype, &subtypmod);
				if (vartype == subtype && vartypmod == subtypmod)
				{
					varno = leftrti;
					varattno = leftattno;
					jexpr = (JoinExpr *) jexpr->larg;
					continue;
				}
			}
			if (rte->jointype == JOIN_INNER ||
				rte->jointype == JOIN_RIGHT)
			{
				subrte = rt_fetch(rightrti, root->rtable);
				get_rte_attribute_type(subrte, rightattno,
									   &subtype, &subtypmod);
				if (vartype == subtype && vartypmod == subtypmod)
				{
					varno = rightrti;
					varattno = rightattno;
					jexpr = (JoinExpr *) jexpr->rarg;
					continue;
				}
			}
			/*
			 * This var cannot be substituted directly, only with a COALESCE.
			 * Do so only if it belongs to the particular join indicated by
			 * the caller.
			 */
			if (varno != expandRTI)
				break;
			{
				Node   *l_var,
					   *r_var;
				CaseExpr   *c = makeNode(CaseExpr);
				CaseWhen   *w = makeNode(CaseWhen);
				NullTest   *n = makeNode(NullTest);

				subrte = rt_fetch(leftrti, root->rtable);
				get_rte_attribute_type(subrte, leftattno,
									   &subtype, &subtypmod);
				l_var = (Node *) makeVar(leftrti,
										 leftattno,
										 subtype,
										 subtypmod,
										 0);
				if (subtype != vartype)
				{
					l_var = coerce_type(NULL, l_var, subtype,
										vartype, vartypmod, false);
					l_var = coerce_type_typmod(NULL, l_var,
											   vartype, vartypmod);
				}
				else if (subtypmod != vartypmod)
					l_var = coerce_type_typmod(NULL, l_var,
											   vartype, vartypmod);

				subrte = rt_fetch(rightrti, root->rtable);
				get_rte_attribute_type(subrte, rightattno,
									   &subtype, &subtypmod);
				r_var = (Node *) makeVar(rightrti,
										 rightattno,
										 subtype,
										 subtypmod,
										 0);
				if (subtype != vartype)
				{
					r_var = coerce_type(NULL, r_var, subtype,
										vartype, vartypmod, false);
					r_var = coerce_type_typmod(NULL, r_var,
											   vartype, vartypmod);
				}
				else if (subtypmod != vartypmod)
					r_var = coerce_type_typmod(NULL, r_var,
											   vartype, vartypmod);

				n->arg = l_var;
				n->nulltesttype = IS_NOT_NULL;
				w->expr = (Node *) n;
				w->result = l_var;
				c->casetype = vartype;
				c->args = makeList1(w);
				c->defresult = r_var;
				return (Node *) c;
			}
		}
		else if (leftattno)
		{
			/* Here we do not need to check the type */
			varno = leftrti;
			varattno = leftattno;
			jexpr = (JoinExpr *) jexpr->larg;
		}
		else
		{
			Assert(rightattno);
			/* Here we do not need to check the type */
			varno = rightrti;
			varattno = rightattno;
			jexpr = (JoinExpr *) jexpr->rarg;
		}
	}

	/*
	 * When we fall out of the loop, we've reached the base Var.
	 */
	return (Node *) makeVar(varno,
							varattno,
							vartype,
							vartypmod,
							0);
}

/*
 * Given a join alias Var, construct Vars for the two input vars it directly
 * depends on.  Note that this should *only* be called for merger alias Vars.
 * In practice it is only used for Vars that got past flatten_join_alias_vars.
 */
void
build_join_alias_subvars(Query *root, Var *aliasvar,
						 Var **leftsubvar, Var **rightsubvar)
{
	Index		varno = aliasvar->varno;
	AttrNumber	varattno = aliasvar->varattno;
	RangeTblEntry *rte;
	JoinExpr   *jexpr;
	Index		leftrti,
				rightrti;
	AttrNumber	leftattno,
				rightattno;
	RangeTblEntry *subrte;
	Oid			subtype;
	int32		subtypmod;

	Assert(aliasvar->varlevelsup == 0);
	rte = rt_fetch(varno, root->rtable);
	Assert(rte->rtekind == RTE_JOIN);

	/*
	 * Find the RT indexes of the left and right children of the
	 * join node.
	 */
	jexpr = (JoinExpr *) find_jointree_item((Node *) root->jointree, varno);
	if (jexpr == NULL ||
		!IsA(jexpr, JoinExpr) ||
		jexpr->rtindex != varno)
		elog(ERROR, "build_join_alias_subvars: failed to find JoinExpr");
	if (IsA(jexpr->larg, RangeTblRef))
		leftrti = ((RangeTblRef *) jexpr->larg)->rtindex;
	else if (IsA(jexpr->larg, JoinExpr))
		leftrti = ((JoinExpr *) jexpr->larg)->rtindex;
	else
	{
		elog(ERROR, "build_join_alias_subvars: unexpected subtree type");
		leftrti = 0;			/* keep compiler quiet */
	}
	if (IsA(jexpr->rarg, RangeTblRef))
		rightrti = ((RangeTblRef *) jexpr->rarg)->rtindex;
	else if (IsA(jexpr->rarg, JoinExpr))
		rightrti = ((JoinExpr *) jexpr->rarg)->rtindex;
	else
	{
		elog(ERROR, "build_join_alias_subvars: unexpected subtree type");
		rightrti = 0;			/* keep compiler quiet */
	}

	Assert(varattno > 0);
	leftattno = (AttrNumber) nthi(varattno-1, rte->joinleftcols);
	rightattno = (AttrNumber) nthi(varattno-1, rte->joinrightcols);
	if (!(leftattno && rightattno))
		elog(ERROR, "build_join_alias_subvars: non-merger variable");

	subrte = rt_fetch(leftrti, root->rtable);
	get_rte_attribute_type(subrte, leftattno,
						   &subtype, &subtypmod);
	*leftsubvar = makeVar(leftrti,
						  leftattno,
						  subtype,
						  subtypmod,
						  0);

	subrte = rt_fetch(rightrti, root->rtable);
	get_rte_attribute_type(subrte, rightattno,
						   &subtype, &subtypmod);
	*rightsubvar = makeVar(rightrti,
						   rightattno,
						   subtype,
						   subtypmod,
						   0);
}

/*
 * Find jointree item matching the specified RT index
 */
static Node *
find_jointree_item(Node *jtnode, int rtindex)
{
	if (jtnode == NULL)
		return NULL;
	if (IsA(jtnode, RangeTblRef))
	{
		if (((RangeTblRef *) jtnode)->rtindex == rtindex)
			return jtnode;
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		List	   *l;

		foreach(l, f->fromlist)
		{
			jtnode = find_jointree_item(lfirst(l), rtindex);
			if (jtnode)
				return jtnode;
		}
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		if (j->rtindex == rtindex)
			return jtnode;
		jtnode = find_jointree_item(j->larg, rtindex);
		if (jtnode)
			return jtnode;
		jtnode = find_jointree_item(j->rarg, rtindex);
		if (jtnode)
			return jtnode;
	}
	else
		elog(ERROR, "find_jointree_item: unexpected node type %d",
			 nodeTag(jtnode));
	return NULL;
}
