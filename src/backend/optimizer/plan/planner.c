/*-------------------------------------------------------------------------
 *
 * planner.c
 *	  The query optimizer external interface.
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/planner.c,v 1.125.2.2 2003/03/05 18:38:26 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#ifdef OPTIMIZER_DEBUG
#include "nodes/print.h"
#endif
#include "optimizer/clauses.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "parser/analyze.h"
#include "parser/parsetree.h"
#include "parser/parse_expr.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"


/* Expression kind codes for preprocess_expression */
#define EXPRKIND_TARGET 0
#define EXPRKIND_WHERE	1
#define EXPRKIND_HAVING 2


static Node *pull_up_subqueries(Query *parse, Node *jtnode,
				   bool below_outer_join);
static bool is_simple_subquery(Query *subquery);
static bool has_nullable_targetlist(Query *subquery);
static void resolvenew_in_jointree(Node *jtnode, int varno, List *subtlist);
static Node *preprocess_jointree(Query *parse, Node *jtnode);
static Node *preprocess_expression(Query *parse, Node *expr, int kind);
static void preprocess_qual_conditions(Query *parse, Node *jtnode);
static Plan *inheritance_planner(Query *parse, List *inheritlist);
static Plan *grouping_planner(Query *parse, double tuple_fraction);
static List *make_subplanTargetList(Query *parse, List *tlist,
					   AttrNumber **groupColIdx);
static Plan *make_groupplan(Query *parse,
			   List *group_tlist, bool tuplePerGroup,
			   List *groupClause, AttrNumber *grpColIdx,
			   bool is_presorted, Plan *subplan);
static List *postprocess_setop_tlist(List *new_tlist, List *orig_tlist);


/*****************************************************************************
 *
 *	   Query optimizer entry point
 *
 *****************************************************************************/
Plan *
planner(Query *parse)
{
	Plan	   *result_plan;
	Index		save_PlannerQueryLevel;
	List	   *save_PlannerParamVar;

	/*
	 * The planner can be called recursively (an example is when
	 * eval_const_expressions tries to pre-evaluate an SQL function). So,
	 * these global state variables must be saved and restored.
	 *
	 * These vars cannot be moved into the Query structure since their whole
	 * purpose is communication across multiple sub-Queries.
	 *
	 * Note we do NOT save and restore PlannerPlanId: it exists to assign
	 * unique IDs to SubPlan nodes, and we want those IDs to be unique for
	 * the life of a backend.  Also, PlannerInitPlan is saved/restored in
	 * subquery_planner, not here.
	 */
	save_PlannerQueryLevel = PlannerQueryLevel;
	save_PlannerParamVar = PlannerParamVar;

	/* Initialize state for handling outer-level references and params */
	PlannerQueryLevel = 0;		/* will be 1 in top-level subquery_planner */
	PlannerParamVar = NIL;

	/* primary planning entry point (may recurse for subqueries) */
	result_plan = subquery_planner(parse, -1.0 /* default case */ );

	Assert(PlannerQueryLevel == 0);

	/* executor wants to know total number of Params used overall */
	result_plan->nParamExec = length(PlannerParamVar);

	/* final cleanup of the plan */
	set_plan_references(result_plan, parse->rtable);

	/* restore state for outer planner, if any */
	PlannerQueryLevel = save_PlannerQueryLevel;
	PlannerParamVar = save_PlannerParamVar;

	return result_plan;
}


/*--------------------
 * subquery_planner
 *	  Invokes the planner on a subquery.  We recurse to here for each
 *	  sub-SELECT found in the query tree.
 *
 * parse is the querytree produced by the parser & rewriter.
 * tuple_fraction is the fraction of tuples we expect will be retrieved.
 * tuple_fraction is interpreted as explained for grouping_planner, below.
 *
 * Basically, this routine does the stuff that should only be done once
 * per Query object.  It then calls grouping_planner.  At one time,
 * grouping_planner could be invoked recursively on the same Query object;
 * that's not currently true, but we keep the separation between the two
 * routines anyway, in case we need it again someday.
 *
 * subquery_planner will be called recursively to handle sub-Query nodes
 * found within the query's expressions and rangetable.
 *
 * Returns a query plan.
 *--------------------
 */
Plan *
subquery_planner(Query *parse, double tuple_fraction)
{
	List	   *saved_initplan = PlannerInitPlan;
	int			saved_planid = PlannerPlanId;
	Plan	   *plan;
	List	   *newHaving;
	List	   *lst;

	/* Set up for a new level of subquery */
	PlannerQueryLevel++;
	PlannerInitPlan = NIL;

	/*
	 * Check to see if any subqueries in the rangetable can be merged into
	 * this query.
	 */
	parse->jointree = (FromExpr *)
		pull_up_subqueries(parse, (Node *) parse->jointree, false);

	/*
	 * If so, we may have created opportunities to simplify the jointree.
	 */
	parse->jointree = (FromExpr *)
		preprocess_jointree(parse, (Node *) parse->jointree);

	/*
	 * Do expression preprocessing on targetlist and quals.
	 */
	parse->targetList = (List *)
		preprocess_expression(parse, (Node *) parse->targetList,
							  EXPRKIND_TARGET);

	preprocess_qual_conditions(parse, (Node *) parse->jointree);

	parse->havingQual = preprocess_expression(parse, parse->havingQual,
											  EXPRKIND_HAVING);

	/* Also need to preprocess expressions for function RTEs */
	foreach(lst, parse->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lst);

		if (rte->rtekind == RTE_FUNCTION)
			rte->funcexpr = preprocess_expression(parse, rte->funcexpr,
												  EXPRKIND_TARGET);
		/* These are not targetlist items, but close enough... */
	}

	/*
	 * Check for ungrouped variables passed to subplans in targetlist and
	 * HAVING clause (but not in WHERE or JOIN/ON clauses, since those are
	 * evaluated before grouping).	We can't do this any earlier because
	 * we must use the preprocessed targetlist for comparisons of grouped
	 * expressions.
	 */
	if (parse->hasSubLinks &&
		(parse->groupClause != NIL || parse->hasAggs))
		check_subplans_for_ungrouped_vars(parse);

	/*
	 * A HAVING clause without aggregates is equivalent to a WHERE clause
	 * (except it can only refer to grouped fields).  Transfer any
	 * agg-free clauses of the HAVING qual into WHERE.	This may seem like
	 * wasting cycles to cater to stupidly-written queries, but there are
	 * other reasons for doing it.	Firstly, if the query contains no aggs
	 * at all, then we aren't going to generate an Agg plan node, and so
	 * there'll be no place to execute HAVING conditions; without this
	 * transfer, we'd lose the HAVING condition entirely, which is wrong.
	 * Secondly, when we push down a qual condition into a sub-query, it's
	 * easiest to push the qual into HAVING always, in case it contains
	 * aggs, and then let this code sort it out.
	 *
	 * Note that both havingQual and parse->jointree->quals are in
	 * implicitly-ANDed-list form at this point, even though they are
	 * declared as Node *.	Also note that contain_agg_clause does not
	 * recurse into sub-selects, which is exactly what we need here.
	 */
	newHaving = NIL;
	foreach(lst, (List *) parse->havingQual)
	{
		Node	   *havingclause = (Node *) lfirst(lst);

		if (contain_agg_clause(havingclause))
			newHaving = lappend(newHaving, havingclause);
		else
			parse->jointree->quals = (Node *)
				lappend((List *) parse->jointree->quals, havingclause);
	}
	parse->havingQual = (Node *) newHaving;

	/*
	 * Do the main planning.  If we have an inherited target relation,
	 * that needs special processing, else go straight to
	 * grouping_planner.
	 */
	if (parse->resultRelation &&
	 (lst = expand_inherted_rtentry(parse, parse->resultRelation, false))
		!= NIL)
		plan = inheritance_planner(parse, lst);
	else
		plan = grouping_planner(parse, tuple_fraction);

	/*
	 * If any subplans were generated, or if we're inside a subplan, build
	 * subPlan, extParam and locParam lists for plan nodes.
	 */
	if (PlannerPlanId != saved_planid || PlannerQueryLevel > 1)
	{
		(void) SS_finalize_plan(plan, parse->rtable);

		/*
		 * At the moment, SS_finalize_plan doesn't handle initPlans and so
		 * we assign them to the topmost plan node.
		 */
		plan->initPlan = PlannerInitPlan;
		/* Must add the initPlans' extParams to the topmost node's, too */
		foreach(lst, plan->initPlan)
		{
			SubPlan    *subplan = (SubPlan *) lfirst(lst);

			plan->extParam = set_unioni(plan->extParam,
										subplan->plan->extParam);
		}
	}

	/* Return to outer subquery context */
	PlannerQueryLevel--;
	PlannerInitPlan = saved_initplan;
	/* we do NOT restore PlannerPlanId; that's not an oversight! */

	return plan;
}

/*
 * pull_up_subqueries
 *		Look for subqueries in the rangetable that can be pulled up into
 *		the parent query.  If the subquery has no special features like
 *		grouping/aggregation then we can merge it into the parent's jointree.
 *
 * below_outer_join is true if this jointree node is within the nullable
 * side of an outer join.  This restricts what we can do.
 *
 * A tricky aspect of this code is that if we pull up a subquery we have
 * to replace Vars that reference the subquery's outputs throughout the
 * parent query, including quals attached to jointree nodes above the one
 * we are currently processing!  We handle this by being careful not to
 * change the jointree structure while recursing: no nodes other than
 * subquery RangeTblRef entries will be replaced.  Also, we can't turn
 * ResolveNew loose on the whole jointree, because it'll return a mutated
 * copy of the tree; we have to invoke it just on the quals, instead.
 */
static Node *
pull_up_subqueries(Query *parse, Node *jtnode, bool below_outer_join)
{
	if (jtnode == NULL)
		return NULL;
	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;
		RangeTblEntry *rte = rt_fetch(varno, parse->rtable);
		Query	   *subquery = rte->subquery;

		/*
		 * Is this a subquery RTE, and if so, is the subquery simple
		 * enough to pull up?  (If not, do nothing at this node.)
		 *
		 * If we are inside an outer join, only pull up subqueries whose
		 * targetlists are nullable --- otherwise substituting their tlist
		 * entries for upper Var references would do the wrong thing (the
		 * results wouldn't become NULL when they're supposed to). XXX
		 * This could be improved by generating pseudo-variables for such
		 * expressions; we'd have to figure out how to get the pseudo-
		 * variables evaluated at the right place in the modified plan
		 * tree. Fix it someday.
		 *
		 * Note: even if the subquery itself is simple enough, we can't pull
		 * it up if there is a reference to its whole tuple result.
		 * Perhaps a pseudo-variable is the answer here too.
		 */
		if (rte->rtekind == RTE_SUBQUERY && is_simple_subquery(subquery) &&
			(!below_outer_join || has_nullable_targetlist(subquery)) &&
			!contain_whole_tuple_var((Node *) parse, varno, 0))
		{
			int			rtoffset;
			List	   *subtlist;
			List	   *rt;

			/*
			 * First, recursively pull up the subquery's subqueries, so
			 * that this routine's processing is complete for its jointree
			 * and rangetable.	NB: if the same subquery is referenced
			 * from multiple jointree items (which can't happen normally,
			 * but might after rule rewriting), then we will invoke this
			 * processing multiple times on that subquery.	OK because
			 * nothing will happen after the first time.  We do have to be
			 * careful to copy everything we pull up, however, or risk
			 * having chunks of structure multiply linked.
			 *
			 * Note: 'false' is correct here even if we are within an outer
			 * join in the upper query; the lower query starts with a clean
			 * slate for outer-join semantics.
			 */
			subquery->jointree = (FromExpr *)
				pull_up_subqueries(subquery, (Node *) subquery->jointree,
								   false);

			/*
			 * Now make a modifiable copy of the subquery that we can run
			 * OffsetVarNodes and IncrementVarSublevelsUp on.
			 */
			subquery = copyObject(subquery);

			/*
			 * Adjust level-0 varnos in subquery so that we can append its
			 * rangetable to upper query's.
			 */
			rtoffset = length(parse->rtable);
			OffsetVarNodes((Node *) subquery, rtoffset, 0);

			/*
			 * Upper-level vars in subquery are now one level closer to their
			 * parent than before.
			 */
			IncrementVarSublevelsUp((Node *) subquery, -1, 1);

			/*
			 * Replace all of the top query's references to the subquery's
			 * outputs with copies of the adjusted subtlist items, being
			 * careful not to replace any of the jointree structure.
			 * (This'd be a lot cleaner if we could use
			 * query_tree_mutator.)
			 */
			subtlist = subquery->targetList;
			parse->targetList = (List *)
				ResolveNew((Node *) parse->targetList,
						   varno, 0, subtlist, CMD_SELECT, 0);
			resolvenew_in_jointree((Node *) parse->jointree, varno, subtlist);
			Assert(parse->setOperations == NULL);
			parse->havingQual =
				ResolveNew(parse->havingQual,
						   varno, 0, subtlist, CMD_SELECT, 0);

			foreach(rt, parse->rtable)
			{
				RangeTblEntry *rte = (RangeTblEntry *) lfirst(rt);

				if (rte->rtekind == RTE_JOIN)
					rte->joinaliasvars = (List *)
						ResolveNew((Node *) rte->joinaliasvars,
								   varno, 0, subtlist, CMD_SELECT, 0);
			}

			/*
			 * Now append the adjusted rtable entries to upper query. (We
			 * hold off until after fixing the upper rtable entries; no
			 * point in running that code on the subquery ones too.)
			 */
			parse->rtable = nconc(parse->rtable, subquery->rtable);

			/*
			 * Pull up any FOR UPDATE markers, too.  (OffsetVarNodes
			 * already adjusted the marker values, so just nconc the
			 * list.)
			 */
			parse->rowMarks = nconc(parse->rowMarks, subquery->rowMarks);

			/*
			 * Miscellaneous housekeeping.
			 */
			parse->hasSubLinks |= subquery->hasSubLinks;
			/* subquery won't be pulled up if it hasAggs, so no work there */

			/*
			 * Return the adjusted subquery jointree to replace the
			 * RangeTblRef entry in my jointree.
			 */
			return (Node *) subquery->jointree;
		}
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		List	   *l;

		foreach(l, f->fromlist)
			lfirst(l) = pull_up_subqueries(parse, lfirst(l),
										   below_outer_join);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		/* Recurse, being careful to tell myself when inside outer join */
		switch (j->jointype)
		{
			case JOIN_INNER:
				j->larg = pull_up_subqueries(parse, j->larg,
											 below_outer_join);
				j->rarg = pull_up_subqueries(parse, j->rarg,
											 below_outer_join);
				break;
			case JOIN_LEFT:
				j->larg = pull_up_subqueries(parse, j->larg,
											 below_outer_join);
				j->rarg = pull_up_subqueries(parse, j->rarg,
											 true);
				break;
			case JOIN_FULL:
				j->larg = pull_up_subqueries(parse, j->larg,
											 true);
				j->rarg = pull_up_subqueries(parse, j->rarg,
											 true);
				break;
			case JOIN_RIGHT:
				j->larg = pull_up_subqueries(parse, j->larg,
											 true);
				j->rarg = pull_up_subqueries(parse, j->rarg,
											 below_outer_join);
				break;
			case JOIN_UNION:

				/*
				 * This is where we fail if upper levels of planner
				 * haven't rewritten UNION JOIN as an Append ...
				 */
				elog(ERROR, "UNION JOIN is not implemented yet");
				break;
			default:
				elog(ERROR, "pull_up_subqueries: unexpected join type %d",
					 j->jointype);
				break;
		}
	}
	else
		elog(ERROR, "pull_up_subqueries: unexpected node type %d",
			 nodeTag(jtnode));
	return jtnode;
}

/*
 * is_simple_subquery
 *	  Check a subquery in the range table to see if it's simple enough
 *	  to pull up into the parent query.
 */
static bool
is_simple_subquery(Query *subquery)
{
	/*
	 * Let's just make sure it's a valid subselect ...
	 */
	if (!IsA(subquery, Query) ||
		subquery->commandType != CMD_SELECT ||
		subquery->resultRelation != 0 ||
		subquery->into != NULL ||
		subquery->isPortal)
		elog(ERROR, "is_simple_subquery: subquery is bogus");

	/*
	 * Can't currently pull up a query with setops. Maybe after querytree
	 * redesign...
	 */
	if (subquery->setOperations)
		return false;

	/*
	 * Can't pull up a subquery involving grouping, aggregation, sorting,
	 * or limiting.
	 */
	if (subquery->hasAggs ||
		subquery->groupClause ||
		subquery->havingQual ||
		subquery->sortClause ||
		subquery->distinctClause ||
		subquery->limitOffset ||
		subquery->limitCount)
		return false;

	/*
	 * Don't pull up a subquery that has any set-returning functions in
	 * its targetlist.	Otherwise we might well wind up inserting
	 * set-returning functions into places where they mustn't go, such as
	 * quals of higher queries.
	 */
	if (expression_returns_set((Node *) subquery->targetList))
		return false;

	/*
	 * Don't pull up a subquery that has any sublinks in its targetlist,
	 * either.  As of PG 7.3 this creates problems because the pulled-up
	 * expressions may go into join alias lists, and the sublinks would
	 * not get fixed because we do flatten_join_alias_vars() too late.
	 * Eventually we should do a complete flatten_join_alias_vars as the
	 * first step of preprocess_expression, and then we could probably
	 * support this.  (BUT: it might be a bad idea anyway, due to possibly
	 * causing multiple evaluations of an expensive sublink.)
	 */
	if (subquery->hasSubLinks &&
		contain_subplans((Node *) subquery->targetList))
		return false;

	/*
	 * Hack: don't try to pull up a subquery with an empty jointree.
	 * query_planner() will correctly generate a Result plan for a
	 * jointree that's totally empty, but I don't think the right things
	 * happen if an empty FromExpr appears lower down in a jointree. Not
	 * worth working hard on this, just to collapse SubqueryScan/Result
	 * into Result...
	 */
	if (subquery->jointree->fromlist == NIL)
		return false;

	return true;
}

/*
 * has_nullable_targetlist
 *	  Check a subquery in the range table to see if all the non-junk
 *	  targetlist items are simple variables (and, hence, will correctly
 *	  go to NULL when examined above the point of an outer join).
 *
 * A possible future extension is to accept strict functions of simple
 * variables, eg, "x + 1".
 */
static bool
has_nullable_targetlist(Query *subquery)
{
	List	   *l;

	foreach(l, subquery->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		/* ignore resjunk columns */
		if (tle->resdom->resjunk)
			continue;

		/* Okay if tlist item is a simple Var */
		if (tle->expr && IsA(tle->expr, Var))
			continue;

		return false;
	}
	return true;
}

/*
 * Helper routine for pull_up_subqueries: do ResolveNew on every expression
 * in the jointree, without changing the jointree structure itself.  Ugly,
 * but there's no other way...
 */
static void
resolvenew_in_jointree(Node *jtnode, int varno, List *subtlist)
{
	if (jtnode == NULL)
		return;
	if (IsA(jtnode, RangeTblRef))
	{
		/* nothing to do here */
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		List	   *l;

		foreach(l, f->fromlist)
			resolvenew_in_jointree(lfirst(l), varno, subtlist);
		f->quals = ResolveNew(f->quals,
							  varno, 0, subtlist, CMD_SELECT, 0);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		resolvenew_in_jointree(j->larg, varno, subtlist);
		resolvenew_in_jointree(j->rarg, varno, subtlist);
		j->quals = ResolveNew(j->quals,
							  varno, 0, subtlist, CMD_SELECT, 0);

		/*
		 * We don't bother to update the colvars list, since it won't be
		 * used again ...
		 */
	}
	else
		elog(ERROR, "resolvenew_in_jointree: unexpected node type %d",
			 nodeTag(jtnode));
}

/*
 * preprocess_jointree
 *		Attempt to simplify a query's jointree.
 *
 * If we succeed in pulling up a subquery then we might form a jointree
 * in which a FromExpr is a direct child of another FromExpr.  In that
 * case we can consider collapsing the two FromExprs into one.	This is
 * an optional conversion, since the planner will work correctly either
 * way.  But we may find a better plan (at the cost of more planning time)
 * if we merge the two nodes.
 *
 * NOTE: don't try to do this in the same jointree scan that does subquery
 * pullup!	Since we're changing the jointree structure here, that wouldn't
 * work reliably --- see comments for pull_up_subqueries().
 */
static Node *
preprocess_jointree(Query *parse, Node *jtnode)
{
	if (jtnode == NULL)
		return NULL;
	if (IsA(jtnode, RangeTblRef))
	{
		/* nothing to do here... */
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		List	   *newlist = NIL;
		List	   *l;

		foreach(l, f->fromlist)
		{
			Node	   *child = (Node *) lfirst(l);

			/* Recursively simplify the child... */
			child = preprocess_jointree(parse, child);
			/* Now, is it a FromExpr? */
			if (child && IsA(child, FromExpr))
			{
				/*
				 * Yes, so do we want to merge it into parent?	Always do
				 * so if child has just one element (since that doesn't
				 * make the parent's list any longer).  Otherwise we have
				 * to be careful about the increase in planning time
				 * caused by combining the two join search spaces into
				 * one.  Our heuristic is to merge if the merge will
				 * produce a join list no longer than GEQO_RELS/2.
				 * (Perhaps need an additional user parameter?)
				 */
				FromExpr   *subf = (FromExpr *) child;
				int			childlen = length(subf->fromlist);
				int			myothers = length(newlist) + length(lnext(l));

				if (childlen <= 1 || (childlen + myothers) <= geqo_rels / 2)
				{
					newlist = nconc(newlist, subf->fromlist);
					f->quals = make_and_qual(subf->quals, f->quals);
				}
				else
					newlist = lappend(newlist, child);
			}
			else
				newlist = lappend(newlist, child);
		}
		f->fromlist = newlist;
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		/* Can't usefully change the JoinExpr, but recurse on children */
		j->larg = preprocess_jointree(parse, j->larg);
		j->rarg = preprocess_jointree(parse, j->rarg);
	}
	else
		elog(ERROR, "preprocess_jointree: unexpected node type %d",
			 nodeTag(jtnode));
	return jtnode;
}

/*
 * preprocess_expression
 *		Do subquery_planner's preprocessing work for an expression,
 *		which can be a targetlist, a WHERE clause (including JOIN/ON
 *		conditions), or a HAVING clause.
 */
static Node *
preprocess_expression(Query *parse, Node *expr, int kind)
{
	bool		has_join_rtes;
	List	   *rt;

	/*
	 * Simplify constant expressions.
	 *
	 * Note that at this point quals have not yet been converted to
	 * implicit-AND form, so we can apply eval_const_expressions directly.
	 * Also note that we need to do this before SS_process_sublinks,
	 * because that routine inserts bogus "Const" nodes.
	 */
	expr = eval_const_expressions(expr);

	/*
	 * If it's a qual or havingQual, canonicalize it, and convert it to
	 * implicit-AND format.
	 *
	 * XXX Is there any value in re-applying eval_const_expressions after
	 * canonicalize_qual?
	 */
	if (kind != EXPRKIND_TARGET)
	{
		expr = (Node *) canonicalize_qual((Expr *) expr, true);

#ifdef OPTIMIZER_DEBUG
		printf("After canonicalize_qual()\n");
		pprint(expr);
#endif
	}

	/* Expand SubLinks to SubPlans */
	if (parse->hasSubLinks)
		expr = SS_process_sublinks(expr);

	/* Replace uplevel vars with Param nodes */
	if (PlannerQueryLevel > 1)
		expr = SS_replace_correlation_vars(expr);

	/*
	 * If the query has any join RTEs, try to replace join alias variables
	 * with base-relation variables, to allow quals to be pushed down. We
	 * must do this after sublink processing, since it does not recurse
	 * into sublinks.
	 *
	 * The flattening pass is expensive enough that it seems worthwhile to
	 * scan the rangetable to see if we can avoid it.
	 */
	has_join_rtes = false;
	foreach(rt, parse->rtable)
	{
		RangeTblEntry *rte = lfirst(rt);

		if (rte->rtekind == RTE_JOIN)
		{
			has_join_rtes = true;
			break;
		}
	}
	if (has_join_rtes)
		expr = flatten_join_alias_vars(expr, parse->rtable, false);

	return expr;
}

/*
 * preprocess_qual_conditions
 *		Recursively scan the query's jointree and do subquery_planner's
 *		preprocessing work on each qual condition found therein.
 */
static void
preprocess_qual_conditions(Query *parse, Node *jtnode)
{
	if (jtnode == NULL)
		return;
	if (IsA(jtnode, RangeTblRef))
	{
		/* nothing to do here */
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		List	   *l;

		foreach(l, f->fromlist)
			preprocess_qual_conditions(parse, lfirst(l));

		f->quals = preprocess_expression(parse, f->quals, EXPRKIND_WHERE);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		preprocess_qual_conditions(parse, j->larg);
		preprocess_qual_conditions(parse, j->rarg);

		j->quals = preprocess_expression(parse, j->quals, EXPRKIND_WHERE);
	}
	else
		elog(ERROR, "preprocess_qual_conditions: unexpected node type %d",
			 nodeTag(jtnode));
}

/*--------------------
 * inheritance_planner
 *	  Generate a plan in the case where the result relation is an
 *	  inheritance set.
 *
 * We have to handle this case differently from cases where a source
 * relation is an inheritance set.	Source inheritance is expanded at
 * the bottom of the plan tree (see allpaths.c), but target inheritance
 * has to be expanded at the top.  The reason is that for UPDATE, each
 * target relation needs a different targetlist matching its own column
 * set.  (This is not so critical for DELETE, but for simplicity we treat
 * inherited DELETE the same way.)	Fortunately, the UPDATE/DELETE target
 * can never be the nullable side of an outer join, so it's OK to generate
 * the plan this way.
 *
 * parse is the querytree produced by the parser & rewriter.
 * inheritlist is an integer list of RT indexes for the result relation set.
 *
 * Returns a query plan.
 *--------------------
 */
static Plan *
inheritance_planner(Query *parse, List *inheritlist)
{
	int			parentRTindex = parse->resultRelation;
	Oid			parentOID = getrelid(parentRTindex, parse->rtable);
	int			mainrtlength = length(parse->rtable);
	List	   *subplans = NIL;
	List	   *tlist = NIL;
	List	   *l;

	foreach(l, inheritlist)
	{
		int			childRTindex = lfirsti(l);
		Oid			childOID = getrelid(childRTindex, parse->rtable);
		int			subrtlength;
		Query	   *subquery;
		Plan	   *subplan;

		/* Generate modified query with this rel as target */
		subquery = (Query *) adjust_inherited_attrs((Node *) parse,
												parentRTindex, parentOID,
												 childRTindex, childOID);
		/* Generate plan */
		subplan = grouping_planner(subquery, 0.0 /* retrieve all tuples */ );
		subplans = lappend(subplans, subplan);
		/*
		 * It's possible that additional RTEs got added to the rangetable
		 * due to expansion of inherited source tables (see allpaths.c).
		 * If so, we must copy 'em back to the main parse tree's rtable.
		 *
		 * XXX my goodness this is ugly.  Really need to think about ways
		 * to rein in planner's habit of scribbling on its input.
		 */
		subrtlength = length(subquery->rtable);
		if (subrtlength > mainrtlength)
		{
			List   *subrt = subquery->rtable;

			while (mainrtlength-- > 0) /* wish we had nthcdr() */
				subrt = lnext(subrt);
			parse->rtable = nconc(parse->rtable, subrt);
			mainrtlength = subrtlength;
		}
		/* Save preprocessed tlist from first rel for use in Append */
		if (tlist == NIL)
			tlist = subplan->targetlist;
	}

	/* Save the target-relations list for the executor, too */
	parse->resultRelations = inheritlist;

	return (Plan *) make_append(subplans, true, tlist);
}

/*--------------------
 * grouping_planner
 *	  Perform planning steps related to grouping, aggregation, etc.
 *	  This primarily means adding top-level processing to the basic
 *	  query plan produced by query_planner.
 *
 * parse is the querytree produced by the parser & rewriter.
 * tuple_fraction is the fraction of tuples we expect will be retrieved
 *
 * tuple_fraction is interpreted as follows:
 *	  < 0: determine fraction by inspection of query (normal case)
 *	  0: expect all tuples to be retrieved
 *	  0 < tuple_fraction < 1: expect the given fraction of tuples available
 *		from the plan to be retrieved
 *	  tuple_fraction >= 1: tuple_fraction is the absolute number of tuples
 *		expected to be retrieved (ie, a LIMIT specification)
 * The normal case is to pass -1, but some callers pass values >= 0 to
 * override this routine's determination of the appropriate fraction.
 *
 * Returns a query plan.
 *--------------------
 */
static Plan *
grouping_planner(Query *parse, double tuple_fraction)
{
	List	   *tlist = parse->targetList;
	Plan	   *result_plan;
	List	   *current_pathkeys;
	List	   *group_pathkeys;
	List	   *sort_pathkeys;
	AttrNumber *groupColIdx = NULL;

	if (parse->setOperations)
	{
		/*
		 * Construct the plan for set operations.  The result will not
		 * need any work except perhaps a top-level sort and/or LIMIT.
		 */
		result_plan = plan_set_operations(parse);

		/*
		 * We should not need to call preprocess_targetlist, since we must
		 * be in a SELECT query node.  Instead, use the targetlist
		 * returned by plan_set_operations (since this tells whether it
		 * returned any resjunk columns!), and transfer any sort key
		 * information from the original tlist.
		 */
		Assert(parse->commandType == CMD_SELECT);

		tlist = postprocess_setop_tlist(result_plan->targetlist, tlist);

		/*
		 * Can't handle FOR UPDATE here (parser should have checked
		 * already, but let's make sure).
		 */
		if (parse->rowMarks)
			elog(ERROR, "SELECT FOR UPDATE is not allowed with UNION/INTERSECT/EXCEPT");

		/*
		 * We set current_pathkeys NIL indicating we do not know sort
		 * order.  This is correct when the top set operation is UNION
		 * ALL, since the appended-together results are unsorted even if
		 * the subplans were sorted.  For other set operations we could be
		 * smarter --- room for future improvement!
		 */
		current_pathkeys = NIL;

		/*
		 * Calculate pathkeys that represent grouping/ordering
		 * requirements (grouping should always be null, but...)
		 */
		group_pathkeys = make_pathkeys_for_sortclauses(parse->groupClause,
													   tlist);
		sort_pathkeys = make_pathkeys_for_sortclauses(parse->sortClause,
													  tlist);
	}
	else
	{
		List	   *sub_tlist;

		/* Preprocess targetlist in case we are inside an INSERT/UPDATE. */
		tlist = preprocess_targetlist(tlist,
									  parse->commandType,
									  parse->resultRelation,
									  parse->rtable);

		/*
		 * Add TID targets for rels selected FOR UPDATE (should this be
		 * done in preprocess_targetlist?).  The executor uses the TID to
		 * know which rows to lock, much as for UPDATE or DELETE.
		 */
		if (parse->rowMarks)
		{
			List	   *l;

			/*
			 * We've got trouble if the FOR UPDATE appears inside
			 * grouping, since grouping renders a reference to individual
			 * tuple CTIDs invalid.  This is also checked at parse time,
			 * but that's insufficient because of rule substitution, query
			 * pullup, etc.
			 */
			CheckSelectForUpdate(parse);

			/*
			 * Currently the executor only supports FOR UPDATE at top
			 * level
			 */
			if (PlannerQueryLevel > 1)
				elog(ERROR, "SELECT FOR UPDATE is not allowed in subselects");

			foreach(l, parse->rowMarks)
			{
				Index		rti = lfirsti(l);
				char	   *resname;
				Resdom	   *resdom;
				Var		   *var;
				TargetEntry *ctid;

				resname = (char *) palloc(32);
				snprintf(resname, 32, "ctid%u", rti);
				resdom = makeResdom(length(tlist) + 1,
									TIDOID,
									-1,
									resname,
									true);

				var = makeVar(rti,
							  SelfItemPointerAttributeNumber,
							  TIDOID,
							  -1,
							  0);

				ctid = makeTargetEntry(resdom, (Node *) var);
				tlist = lappend(tlist, ctid);
			}
		}

		/*
		 * Generate appropriate target list for subplan; may be different
		 * from tlist if grouping or aggregation is needed.
		 */
		sub_tlist = make_subplanTargetList(parse, tlist, &groupColIdx);

		/*
		 * Calculate pathkeys that represent grouping/ordering
		 * requirements
		 */
		group_pathkeys = make_pathkeys_for_sortclauses(parse->groupClause,
													   tlist);
		sort_pathkeys = make_pathkeys_for_sortclauses(parse->sortClause,
													  tlist);

		/*
		 * Figure out whether we need a sorted result from query_planner.
		 *
		 * If we have a GROUP BY clause, then we want a result sorted
		 * properly for grouping.  Otherwise, if there is an ORDER BY
		 * clause, we want to sort by the ORDER BY clause.	(Note: if we
		 * have both, and ORDER BY is a superset of GROUP BY, it would be
		 * tempting to request sort by ORDER BY --- but that might just
		 * leave us failing to exploit an available sort order at all.
		 * Needs more thought...)
		 */
		if (parse->groupClause)
			parse->query_pathkeys = group_pathkeys;
		else if (parse->sortClause)
			parse->query_pathkeys = sort_pathkeys;
		else
			parse->query_pathkeys = NIL;

		/*
		 * Figure out whether we expect to retrieve all the tuples that
		 * the plan can generate, or to stop early due to outside factors
		 * such as a cursor.  If the caller passed a value >= 0, believe
		 * that value, else do our own examination of the query context.
		 */
		if (tuple_fraction < 0.0)
		{
			/* Initial assumption is we need all the tuples */
			tuple_fraction = 0.0;

			/*
			 * Check for retrieve-into-portal, ie DECLARE CURSOR.
			 *
			 * We have no real idea how many tuples the user will ultimately
			 * FETCH from a cursor, but it seems a good bet that he
			 * doesn't want 'em all.  Optimize for 10% retrieval (you
			 * gotta better number?  Should this be a SETtable parameter?)
			 */
			if (parse->isPortal)
				tuple_fraction = 0.10;
		}

		/*
		 * Adjust tuple_fraction if we see that we are going to apply
		 * limiting/grouping/aggregation/etc.  This is not overridable by
		 * the caller, since it reflects plan actions that this routine
		 * will certainly take, not assumptions about context.
		 */
		if (parse->limitCount != NULL)
		{
			/*
			 * A LIMIT clause limits the absolute number of tuples
			 * returned. However, if it's not a constant LIMIT then we
			 * have to punt; for lack of a better idea, assume 10% of the
			 * plan's result is wanted.
			 */
			double		limit_fraction = 0.0;

			if (IsA(parse->limitCount, Const))
			{
				Const	   *limitc = (Const *) parse->limitCount;
				int32		count = DatumGetInt32(limitc->constvalue);

				/*
				 * A NULL-constant LIMIT represents "LIMIT ALL", which we
				 * treat the same as no limit (ie, expect to retrieve all
				 * the tuples).
				 */
				if (!limitc->constisnull && count > 0)
				{
					limit_fraction = (double) count;
					/* We must also consider the OFFSET, if present */
					if (parse->limitOffset != NULL)
					{
						if (IsA(parse->limitOffset, Const))
						{
							int32		offset;

							limitc = (Const *) parse->limitOffset;
							offset = DatumGetInt32(limitc->constvalue);
							if (!limitc->constisnull && offset > 0)
								limit_fraction += (double) offset;
						}
						else
						{
							/* OFFSET is an expression ... punt ... */
							limit_fraction = 0.10;
						}
					}
				}
			}
			else
			{
				/* LIMIT is an expression ... punt ... */
				limit_fraction = 0.10;
			}

			if (limit_fraction > 0.0)
			{
				/*
				 * If we have absolute limits from both caller and LIMIT,
				 * use the smaller value; if one is fractional and the
				 * other absolute, treat the fraction as a fraction of the
				 * absolute value; else we can multiply the two fractions
				 * together.
				 */
				if (tuple_fraction >= 1.0)
				{
					if (limit_fraction >= 1.0)
					{
						/* both absolute */
						tuple_fraction = Min(tuple_fraction, limit_fraction);
					}
					else
					{
						/* caller absolute, limit fractional */
						tuple_fraction *= limit_fraction;
						if (tuple_fraction < 1.0)
							tuple_fraction = 1.0;
					}
				}
				else if (tuple_fraction > 0.0)
				{
					if (limit_fraction >= 1.0)
					{
						/* caller fractional, limit absolute */
						tuple_fraction *= limit_fraction;
						if (tuple_fraction < 1.0)
							tuple_fraction = 1.0;
					}
					else
					{
						/* both fractional */
						tuple_fraction *= limit_fraction;
					}
				}
				else
				{
					/* no info from caller, just use limit */
					tuple_fraction = limit_fraction;
				}
			}
		}

		if (parse->groupClause)
		{
			/*
			 * In GROUP BY mode, we have the little problem that we don't
			 * really know how many input tuples will be needed to make a
			 * group, so we can't translate an output LIMIT count into an
			 * input count.  For lack of a better idea, assume 25% of the
			 * input data will be processed if there is any output limit.
			 * However, if the caller gave us a fraction rather than an
			 * absolute count, we can keep using that fraction (which
			 * amounts to assuming that all the groups are about the same
			 * size).
			 */
			if (tuple_fraction >= 1.0)
				tuple_fraction = 0.25;

			/*
			 * If both GROUP BY and ORDER BY are specified, we will need
			 * two levels of sort --- and, therefore, certainly need to
			 * read all the input tuples --- unless ORDER BY is a subset
			 * of GROUP BY.  (We have not yet canonicalized the pathkeys,
			 * so must use the slower noncanonical comparison method.)
			 */
			if (parse->groupClause && parse->sortClause &&
				!noncanonical_pathkeys_contained_in(sort_pathkeys,
													group_pathkeys))
				tuple_fraction = 0.0;
		}
		else if (parse->hasAggs)
		{
			/*
			 * Ungrouped aggregate will certainly want all the input
			 * tuples.
			 */
			tuple_fraction = 0.0;
		}
		else if (parse->distinctClause)
		{
			/*
			 * SELECT DISTINCT, like GROUP, will absorb an unpredictable
			 * number of input tuples per output tuple.  Handle the same
			 * way.
			 */
			if (tuple_fraction >= 1.0)
				tuple_fraction = 0.25;
		}

		/* Generate the basic plan for this Query */
		result_plan = query_planner(parse,
									sub_tlist,
									tuple_fraction);

		/*
		 * query_planner returns actual sort order (which is not
		 * necessarily what we requested) in query_pathkeys.
		 */
		current_pathkeys = parse->query_pathkeys;
	}

	/*
	 * We couldn't canonicalize group_pathkeys and sort_pathkeys before
	 * running query_planner(), so do it now.
	 */
	group_pathkeys = canonicalize_pathkeys(parse, group_pathkeys);
	sort_pathkeys = canonicalize_pathkeys(parse, sort_pathkeys);

	/*
	 * If we have a GROUP BY clause, insert a group node (plus the
	 * appropriate sort node, if necessary).
	 */
	if (parse->groupClause)
	{
		bool		tuplePerGroup;
		List	   *group_tlist;
		bool		is_sorted;

		/*
		 * Decide whether how many tuples per group the Group node needs
		 * to return. (Needs only one tuple per group if no aggregate is
		 * present. Otherwise, need every tuple from the group to do the
		 * aggregation.)  Note tuplePerGroup is named backwards :-(
		 */
		tuplePerGroup = parse->hasAggs;

		/*
		 * If there are aggregates then the Group node should just return
		 * the same set of vars as the subplan did.  If there are no aggs
		 * then the Group node had better compute the final tlist.
		 */
		if (parse->hasAggs)
			group_tlist = new_unsorted_tlist(result_plan->targetlist);
		else
			group_tlist = tlist;

		/*
		 * Figure out whether the path result is already ordered the way
		 * we need it --- if so, no need for an explicit sort step.
		 */
		if (pathkeys_contained_in(group_pathkeys, current_pathkeys))
		{
			is_sorted = true;	/* no sort needed now */
			/* current_pathkeys remains unchanged */
		}
		else
		{
			/*
			 * We will need to do an explicit sort by the GROUP BY clause.
			 * make_groupplan will do the work, but set current_pathkeys
			 * to indicate the resulting order.
			 */
			is_sorted = false;
			current_pathkeys = group_pathkeys;
		}

		result_plan = make_groupplan(parse,
									 group_tlist,
									 tuplePerGroup,
									 parse->groupClause,
									 groupColIdx,
									 is_sorted,
									 result_plan);
	}

	/*
	 * If aggregate is present, insert the Agg node
	 *
	 * HAVING clause, if any, becomes qual of the Agg node
	 */
	if (parse->hasAggs)
	{
		result_plan = (Plan *) make_agg(tlist,
										(List *) parse->havingQual,
										result_plan);
		/* Note: Agg does not affect any existing sort order of the tuples */
	}
	else
	{
		/* If there are no Aggs, we shouldn't have any HAVING qual anymore */
		Assert(parse->havingQual == NULL);
	}

	/*
	 * If we were not able to make the plan come out in the right order,
	 * add an explicit sort step.
	 */
	if (parse->sortClause)
	{
		if (!pathkeys_contained_in(sort_pathkeys, current_pathkeys))
			result_plan = make_sortplan(parse, tlist, result_plan,
										parse->sortClause);
	}

	/*
	 * If there is a DISTINCT clause, add the UNIQUE node.
	 */
	if (parse->distinctClause)
	{
		result_plan = (Plan *) make_unique(tlist, result_plan,
										   parse->distinctClause);
	}

	/*
	 * Finally, if there is a LIMIT/OFFSET clause, add the LIMIT node.
	 */
	if (parse->limitOffset || parse->limitCount)
	{
		result_plan = (Plan *) make_limit(tlist, result_plan,
										  parse->limitOffset,
										  parse->limitCount);
	}

	return result_plan;
}

/*---------------
 * make_subplanTargetList
 *	  Generate appropriate target list when grouping is required.
 *
 * When grouping_planner inserts Aggregate and/or Group plan nodes above
 * the result of query_planner, we typically want to pass a different
 * target list to query_planner than the outer plan nodes should have.
 * This routine generates the correct target list for the subplan.
 *
 * The initial target list passed from the parser already contains entries
 * for all ORDER BY and GROUP BY expressions, but it will not have entries
 * for variables used only in HAVING clauses; so we need to add those
 * variables to the subplan target list.  Also, if we are doing either
 * grouping or aggregation, we flatten all expressions except GROUP BY items
 * into their component variables; the other expressions will be computed by
 * the inserted nodes rather than by the subplan.  For example,
 * given a query like
 *		SELECT a+b,SUM(c+d) FROM table GROUP BY a+b;
 * we want to pass this targetlist to the subplan:
 *		a,b,c,d,a+b
 * where the a+b target will be used by the Sort/Group steps, and the
 * other targets will be used for computing the final results.	(In the
 * above example we could theoretically suppress the a and b targets and
 * pass down only c,d,a+b, but it's not really worth the trouble to
 * eliminate simple var references from the subplan.  We will avoid doing
 * the extra computation to recompute a+b at the outer level; see
 * replace_vars_with_subplan_refs() in setrefs.c.)
 *
 * 'parse' is the query being processed.
 * 'tlist' is the query's target list.
 * 'groupColIdx' receives an array of column numbers for the GROUP BY
 * expressions (if there are any) in the subplan's target list.
 *
 * The result is the targetlist to be passed to the subplan.
 *---------------
 */
static List *
make_subplanTargetList(Query *parse,
					   List *tlist,
					   AttrNumber **groupColIdx)
{
	List	   *sub_tlist;
	List	   *extravars;
	int			numCols;

	*groupColIdx = NULL;

	/*
	 * If we're not grouping or aggregating, nothing to do here;
	 * query_planner should receive the unmodified target list.
	 */
	if (!parse->hasAggs && !parse->groupClause && !parse->havingQual)
		return tlist;

	/*
	 * Otherwise, start with a "flattened" tlist (having just the vars
	 * mentioned in the targetlist and HAVING qual --- but not upper-
	 * level Vars; they will be replaced by Params later on).
	 */
	sub_tlist = flatten_tlist(tlist);
	extravars = pull_var_clause(parse->havingQual, false);
	sub_tlist = add_to_flat_tlist(sub_tlist, extravars);
	freeList(extravars);

	/*
	 * If grouping, create sub_tlist entries for all GROUP BY expressions
	 * (GROUP BY items that are simple Vars should be in the list
	 * already), and make an array showing where the group columns are in
	 * the sub_tlist.
	 */
	numCols = length(parse->groupClause);
	if (numCols > 0)
	{
		int			keyno = 0;
		AttrNumber *grpColIdx;
		List	   *gl;

		grpColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);
		*groupColIdx = grpColIdx;

		foreach(gl, parse->groupClause)
		{
			GroupClause *grpcl = (GroupClause *) lfirst(gl);
			Node	   *groupexpr = get_sortgroupclause_expr(grpcl, tlist);
			TargetEntry *te = NULL;
			List	   *sl;

			/* Find or make a matching sub_tlist entry */
			foreach(sl, sub_tlist)
			{
				te = (TargetEntry *) lfirst(sl);
				if (equal(groupexpr, te->expr))
					break;
			}
			if (!sl)
			{
				te = makeTargetEntry(makeResdom(length(sub_tlist) + 1,
												exprType(groupexpr),
												exprTypmod(groupexpr),
												NULL,
												false),
									 groupexpr);
				sub_tlist = lappend(sub_tlist, te);
			}

			/* and save its resno */
			grpColIdx[keyno++] = te->resdom->resno;
		}
	}

	return sub_tlist;
}

/*
 * make_groupplan
 *		Add a Group node for GROUP BY processing.
 *		If we couldn't make the subplan produce presorted output for grouping,
 *		first add an explicit Sort node.
 */
static Plan *
make_groupplan(Query *parse,
			   List *group_tlist,
			   bool tuplePerGroup,
			   List *groupClause,
			   AttrNumber *grpColIdx,
			   bool is_presorted,
			   Plan *subplan)
{
	int			numCols = length(groupClause);

	if (!is_presorted)
	{
		/*
		 * The Sort node always just takes a copy of the subplan's tlist
		 * plus ordering information.  (This might seem inefficient if the
		 * subplan contains complex GROUP BY expressions, but in fact Sort
		 * does not evaluate its targetlist --- it only outputs the same
		 * tuples in a new order.  So the expressions we might be copying
		 * are just dummies with no extra execution cost.)
		 */
		List	   *sort_tlist = new_unsorted_tlist(subplan->targetlist);
		int			keyno = 0;
		List	   *gl;

		foreach(gl, groupClause)
		{
			GroupClause *grpcl = (GroupClause *) lfirst(gl);
			TargetEntry *te = nth(grpColIdx[keyno] - 1, sort_tlist);
			Resdom	   *resdom = te->resdom;

			/*
			 * Check for the possibility of duplicate group-by clauses ---
			 * the parser should have removed 'em, but the Sort executor
			 * will get terribly confused if any get through!
			 */
			if (resdom->reskey == 0)
			{
				/* OK, insert the ordering info needed by the executor. */
				resdom->reskey = ++keyno;
				resdom->reskeyop = grpcl->sortop;
			}
		}

		Assert(keyno > 0);

		subplan = (Plan *) make_sort(parse, sort_tlist, subplan, keyno);
	}

	return (Plan *) make_group(group_tlist, tuplePerGroup, numCols,
							   grpColIdx, subplan);
}

/*
 * make_sortplan
 *	  Add a Sort node to implement an explicit ORDER BY clause.
 */
Plan *
make_sortplan(Query *parse, List *tlist, Plan *plannode, List *sortcls)
{
	List	   *sort_tlist;
	List	   *i;
	int			keyno = 0;

	/*
	 * First make a copy of the tlist so that we don't corrupt the
	 * original.
	 */
	sort_tlist = new_unsorted_tlist(tlist);

	foreach(i, sortcls)
	{
		SortClause *sortcl = (SortClause *) lfirst(i);
		TargetEntry *tle = get_sortgroupclause_tle(sortcl, sort_tlist);
		Resdom	   *resdom = tle->resdom;

		/*
		 * Check for the possibility of duplicate order-by clauses --- the
		 * parser should have removed 'em, but the executor will get
		 * terribly confused if any get through!
		 */
		if (resdom->reskey == 0)
		{
			/* OK, insert the ordering info needed by the executor. */
			resdom->reskey = ++keyno;
			resdom->reskeyop = sortcl->sortop;
		}
	}

	Assert(keyno > 0);

	return (Plan *) make_sort(parse, sort_tlist, plannode, keyno);
}

/*
 * postprocess_setop_tlist
 *	  Fix up targetlist returned by plan_set_operations().
 *
 * We need to transpose sort key info from the orig_tlist into new_tlist.
 * NOTE: this would not be good enough if we supported resjunk sort keys
 * for results of set operations --- then, we'd need to project a whole
 * new tlist to evaluate the resjunk columns.  For now, just elog if we
 * find any resjunk columns in orig_tlist.
 */
static List *
postprocess_setop_tlist(List *new_tlist, List *orig_tlist)
{
	List	   *l;

	foreach(l, new_tlist)
	{
		TargetEntry *new_tle = (TargetEntry *) lfirst(l);
		TargetEntry *orig_tle;

		/* ignore resjunk columns in setop result */
		if (new_tle->resdom->resjunk)
			continue;

		Assert(orig_tlist != NIL);
		orig_tle = (TargetEntry *) lfirst(orig_tlist);
		orig_tlist = lnext(orig_tlist);
		if (orig_tle->resdom->resjunk)
			elog(ERROR, "postprocess_setop_tlist: resjunk output columns not implemented");
		Assert(new_tle->resdom->resno == orig_tle->resdom->resno);
		Assert(new_tle->resdom->restype == orig_tle->resdom->restype);
		new_tle->resdom->ressortgroupref = orig_tle->resdom->ressortgroupref;
	}
	if (orig_tlist != NIL)
		elog(ERROR, "postprocess_setop_tlist: resjunk output columns not implemented");
	return new_tlist;
}
