/*-------------------------------------------------------------------------
 *
 * parse_agg.c
 *	  handle aggregates in parser
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_agg.c,v 1.50.2.1 2003/04/03 18:04:17 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/clauses.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "parser/parse_agg.h"
#include "parser/parsetree.h"


typedef struct
{
	ParseState *pstate;
	List	   *groupClauses;
} check_ungrouped_columns_context;

static void check_ungrouped_columns(Node *node, ParseState *pstate,
						List *groupClauses);
static bool check_ungrouped_columns_walker(Node *node,
							   check_ungrouped_columns_context *context);

/*
 * check_ungrouped_columns -
 *	  Scan the given expression tree for ungrouped variables (variables
 *	  that are not listed in the groupClauses list and are not within
 *	  the arguments of aggregate functions).  Emit a suitable error message
 *	  if any are found.
 *
 * NOTE: we assume that the given clause has been transformed suitably for
 * parser output.  This means we can use the planner's expression_tree_walker.
 *
 * NOTE: in the case of a SubLink, expression_tree_walker does not descend
 * into the subquery.  This means we will fail to detect ungrouped columns
 * that appear as outer-level variables within a subquery.	That case seems
 * unreasonably hard to handle here.  Instead, we expect the planner to check
 * for ungrouped columns after it's found all the outer-level references
 * inside the subquery and converted them into a list of parameters for the
 * subquery.
 */
static void
check_ungrouped_columns(Node *node, ParseState *pstate,
						List *groupClauses)
{
	check_ungrouped_columns_context context;

	context.pstate = pstate;
	context.groupClauses = groupClauses;
	check_ungrouped_columns_walker(node, &context);
}

static bool
check_ungrouped_columns_walker(Node *node,
							   check_ungrouped_columns_context *context)
{
	List	   *gl;

	if (node == NULL)
		return false;
	if (IsA(node, Const) ||IsA(node, Param))
		return false;			/* constants are always acceptable */

	/*
	 * If we find an aggregate function, do not recurse into its
	 * arguments.
	 */
	if (IsA(node, Aggref))
		return false;

	/*
	 * Check to see if subexpression as a whole matches any GROUP BY item.
	 * We need to do this at every recursion level so that we recognize
	 * GROUPed-BY expressions before reaching variables within them.
	 */
	foreach(gl, context->groupClauses)
	{
		if (equal(node, lfirst(gl)))
			return false;		/* acceptable, do not descend more */
	}

	/*
	 * If we have an ungrouped Var, we have a failure --- unless it is an
	 * outer-level Var.  In that case it's a constant as far as this query
	 * level is concerned, and we can accept it.  (If it's ungrouped as
	 * far as the upper query is concerned, that's someone else's
	 * problem...)
	 */
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;
		RangeTblEntry *rte;
		char	   *attname;

		if (var->varlevelsup > 0)
			return false;		/* outer-level Var is acceptable */
		/* Found an ungrouped local variable; generate error message */
		Assert(var->varno > 0 &&
			   (int) var->varno <= length(context->pstate->p_rtable));
		rte = rt_fetch(var->varno, context->pstate->p_rtable);
		attname = get_rte_attribute_name(rte, var->varattno);
		elog(ERROR, "Attribute %s.%s must be GROUPed or used in an aggregate function",
			 rte->eref->aliasname, attname);
	}
	/* Otherwise, recurse. */
	return expression_tree_walker(node, check_ungrouped_columns_walker,
								  (void *) context);
}

/*
 * parseCheckAggregates
 *	Check for aggregates where they shouldn't be and improper grouping.
 *
 *	Ideally this should be done earlier, but it's difficult to distinguish
 *	aggregates from plain functions at the grammar level.  So instead we
 *	check here.  This function should be called after the target list and
 *	qualifications are finalized.  BUT: in some cases we want to call this
 *	routine before we've assembled the joinlist and qual into a FromExpr.
 *	So, rather than looking at qry->jointree, look at pstate->p_joinlist
 *	and the explicitly-passed qual.
 */
void
parseCheckAggregates(ParseState *pstate, Query *qry, Node *qual)
{
	List	   *groupClauses = NIL;
	List	   *lst;
	bool		hasJoinRTEs;
	Node	   *clause;

	/* This should only be called if we found aggregates, GROUP, or HAVING */
	Assert(pstate->p_hasAggs || qry->groupClause || qry->havingQual);

	/*
	 * Aggregates must never appear in WHERE or JOIN/ON clauses.
	 *
	 * (Note this check should appear first to deliver an appropriate error
	 * message; otherwise we are likely to complain about some innocent
	 * variable in the target list, which is outright misleading if the
	 * problem is in WHERE.)
	 */
	if (contain_agg_clause(qual))
		elog(ERROR, "Aggregates not allowed in WHERE clause");
	if (contain_agg_clause((Node *) pstate->p_joinlist))
		elog(ERROR, "Aggregates not allowed in JOIN conditions");

	/*
	 * No aggregates allowed in GROUP BY clauses, either.
	 *
	 * While we are at it, build a list of the acceptable GROUP BY
	 * expressions for use by check_ungrouped_columns() (this avoids
	 * repeated scans of the targetlist within the recursive routine...)
	 */
	foreach(lst, qry->groupClause)
	{
		GroupClause *grpcl = lfirst(lst);
		Node	   *expr;

		expr = get_sortgroupclause_expr(grpcl, qry->targetList);
		if (contain_agg_clause(expr))
			elog(ERROR, "Aggregates not allowed in GROUP BY clause");
		groupClauses = lcons(expr, groupClauses);
	}

	/*
	 * If there are join alias vars involved, we have to flatten them
	 * to the underlying vars, so that aliased and unaliased vars will be
	 * correctly taken as equal.  We can skip the expense of doing this
	 * if no rangetable entries are RTE_JOIN kind.
	 */
	hasJoinRTEs = false;
	foreach(lst, pstate->p_rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lst);

		if (rte->rtekind == RTE_JOIN)
		{
			hasJoinRTEs = true;
			break;
		}
	}

	if (hasJoinRTEs)
		groupClauses = (List *) flatten_join_alias_vars((Node *) groupClauses,
														pstate->p_rtable,
														true);

	/*
	 * Check the targetlist and HAVING clause for ungrouped variables.
	 */
	clause = (Node *) qry->targetList;
	if (hasJoinRTEs)
		clause = flatten_join_alias_vars(clause, pstate->p_rtable, true);
	check_ungrouped_columns(clause, pstate, groupClauses);

	clause = (Node *) qry->havingQual;
	if (hasJoinRTEs)
		clause = flatten_join_alias_vars(clause, pstate->p_rtable, true);
	check_ungrouped_columns(clause, pstate, groupClauses);
}
