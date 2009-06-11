/*-------------------------------------------------------------------------
 *
 * planner.c
 *	  The query optimizer external interface.
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/plan/planner.c,v 1.256 2009/06/11 14:48:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "catalog/pg_operator.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#ifdef OPTIMIZER_DEBUG
#include "nodes/print.h"
#endif
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/* GUC parameter */
double		cursor_tuple_fraction = DEFAULT_CURSOR_TUPLE_FRACTION;

/* Hook for plugins to get control in planner() */
planner_hook_type planner_hook = NULL;


/* Expression kind codes for preprocess_expression */
#define EXPRKIND_QUAL		0
#define EXPRKIND_TARGET		1
#define EXPRKIND_RTFUNC		2
#define EXPRKIND_VALUES		3
#define EXPRKIND_LIMIT		4
#define EXPRKIND_APPINFO	5


static Node *preprocess_expression(PlannerInfo *root, Node *expr, int kind);
static void preprocess_qual_conditions(PlannerInfo *root, Node *jtnode);
static Plan *inheritance_planner(PlannerInfo *root);
static Plan *grouping_planner(PlannerInfo *root, double tuple_fraction);
static bool is_dummy_plan(Plan *plan);
static double preprocess_limit(PlannerInfo *root,
				 double tuple_fraction,
				 int64 *offset_est, int64 *count_est);
static void preprocess_groupclause(PlannerInfo *root);
static bool choose_hashed_grouping(PlannerInfo *root,
					   double tuple_fraction, double limit_tuples,
					   Path *cheapest_path, Path *sorted_path,
					   double dNumGroups, AggClauseCounts *agg_counts);
static bool choose_hashed_distinct(PlannerInfo *root,
					   Plan *input_plan, List *input_pathkeys,
					   double tuple_fraction, double limit_tuples,
					   double dNumDistinctRows);
static List *make_subplanTargetList(PlannerInfo *root, List *tlist,
					   AttrNumber **groupColIdx, bool *need_tlist_eval);
static void locate_grouping_columns(PlannerInfo *root,
						List *tlist,
						List *sub_tlist,
						AttrNumber *groupColIdx);
static List *postprocess_setop_tlist(List *new_tlist, List *orig_tlist);
static List *select_active_windows(PlannerInfo *root, WindowFuncLists *wflists);
static List *add_volatile_sort_exprs(List *window_tlist, List *tlist,
						List *activeWindows);
static List *make_pathkeys_for_window(PlannerInfo *root, WindowClause *wc,
						 List *tlist, bool canonicalize);
static void get_column_info_for_window(PlannerInfo *root, WindowClause *wc,
						   List *tlist,
						   int numSortCols, AttrNumber *sortColIdx,
						   int *partNumCols,
						   AttrNumber **partColIdx,
						   Oid **partOperators,
						   int *ordNumCols,
						   AttrNumber **ordColIdx,
						   Oid **ordOperators);


/*****************************************************************************
 *
 *	   Query optimizer entry point
 *
 * To support loadable plugins that monitor or modify planner behavior,
 * we provide a hook variable that lets a plugin get control before and
 * after the standard planning process.  The plugin would normally call
 * standard_planner().
 *
 * Note to plugin authors: standard_planner() scribbles on its Query input,
 * so you'd better copy that data structure if you want to plan more than once.
 *
 *****************************************************************************/
PlannedStmt *
planner(Query *parse, int cursorOptions, ParamListInfo boundParams)
{
	PlannedStmt *result;

	if (planner_hook)
		result = (*planner_hook) (parse, cursorOptions, boundParams);
	else
		result = standard_planner(parse, cursorOptions, boundParams);
	return result;
}

PlannedStmt *
standard_planner(Query *parse, int cursorOptions, ParamListInfo boundParams)
{
	PlannedStmt *result;
	PlannerGlobal *glob;
	double		tuple_fraction;
	PlannerInfo *root;
	Plan	   *top_plan;
	ListCell   *lp,
			   *lr;

	/* Cursor options may come from caller or from DECLARE CURSOR stmt */
	if (parse->utilityStmt &&
		IsA(parse->utilityStmt, DeclareCursorStmt))
		cursorOptions |= ((DeclareCursorStmt *) parse->utilityStmt)->options;

	/*
	 * Set up global state for this planner invocation.  This data is needed
	 * across all levels of sub-Query that might exist in the given command,
	 * so we keep it in a separate struct that's linked to by each per-Query
	 * PlannerInfo.
	 */
	glob = makeNode(PlannerGlobal);

	glob->boundParams = boundParams;
	glob->paramlist = NIL;
	glob->subplans = NIL;
	glob->subrtables = NIL;
	glob->rewindPlanIDs = NULL;
	glob->finalrtable = NIL;
	glob->relationOids = NIL;
	glob->invalItems = NIL;
	glob->lastPHId = 0;
	glob->transientPlan = false;

	/* Determine what fraction of the plan is likely to be scanned */
	if (cursorOptions & CURSOR_OPT_FAST_PLAN)
	{
		/*
		 * We have no real idea how many tuples the user will ultimately FETCH
		 * from a cursor, but it is often the case that he doesn't want 'em
		 * all, or would prefer a fast-start plan anyway so that he can
		 * process some of the tuples sooner.  Use a GUC parameter to decide
		 * what fraction to optimize for.
		 */
		tuple_fraction = cursor_tuple_fraction;

		/*
		 * We document cursor_tuple_fraction as simply being a fraction, which
		 * means the edge cases 0 and 1 have to be treated specially here.	We
		 * convert 1 to 0 ("all the tuples") and 0 to a very small fraction.
		 */
		if (tuple_fraction >= 1.0)
			tuple_fraction = 0.0;
		else if (tuple_fraction <= 0.0)
			tuple_fraction = 1e-10;
	}
	else
	{
		/* Default assumption is we need all the tuples */
		tuple_fraction = 0.0;
	}

	/* primary planning entry point (may recurse for subqueries) */
	top_plan = subquery_planner(glob, parse, NULL,
								false, tuple_fraction, &root);

	/*
	 * If creating a plan for a scrollable cursor, make sure it can run
	 * backwards on demand.  Add a Material node at the top at need.
	 */
	if (cursorOptions & CURSOR_OPT_SCROLL)
	{
		if (!ExecSupportsBackwardScan(top_plan))
			top_plan = materialize_finished_plan(top_plan);
	}

	/* final cleanup of the plan */
	Assert(glob->finalrtable == NIL);
	top_plan = set_plan_references(glob, top_plan, root->parse->rtable);
	/* ... and the subplans (both regular subplans and initplans) */
	Assert(list_length(glob->subplans) == list_length(glob->subrtables));
	forboth(lp, glob->subplans, lr, glob->subrtables)
	{
		Plan	   *subplan = (Plan *) lfirst(lp);
		List	   *subrtable = (List *) lfirst(lr);

		lfirst(lp) = set_plan_references(glob, subplan, subrtable);
	}

	/* build the PlannedStmt result */
	result = makeNode(PlannedStmt);

	result->commandType = parse->commandType;
	result->canSetTag = parse->canSetTag;
	result->transientPlan = glob->transientPlan;
	result->planTree = top_plan;
	result->rtable = glob->finalrtable;
	result->resultRelations = root->resultRelations;
	result->utilityStmt = parse->utilityStmt;
	result->intoClause = parse->intoClause;
	result->subplans = glob->subplans;
	result->rewindPlanIDs = glob->rewindPlanIDs;
	result->returningLists = root->returningLists;
	result->rowMarks = parse->rowMarks;
	result->relationOids = glob->relationOids;
	result->invalItems = glob->invalItems;
	result->nParamExec = list_length(glob->paramlist);

	return result;
}


/*--------------------
 * subquery_planner
 *	  Invokes the planner on a subquery.  We recurse to here for each
 *	  sub-SELECT found in the query tree.
 *
 * glob is the global state for the current planner run.
 * parse is the querytree produced by the parser & rewriter.
 * parent_root is the immediate parent Query's info (NULL at the top level).
 * hasRecursion is true if this is a recursive WITH query.
 * tuple_fraction is the fraction of tuples we expect will be retrieved.
 * tuple_fraction is interpreted as explained for grouping_planner, below.
 *
 * If subroot isn't NULL, we pass back the query's final PlannerInfo struct;
 * among other things this tells the output sort ordering of the plan.
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
subquery_planner(PlannerGlobal *glob, Query *parse,
				 PlannerInfo *parent_root,
				 bool hasRecursion, double tuple_fraction,
				 PlannerInfo **subroot)
{
	int			num_old_subplans = list_length(glob->subplans);
	PlannerInfo *root;
	Plan	   *plan;
	List	   *newHaving;
	bool		hasOuterJoins;
	ListCell   *l;

	/* Create a PlannerInfo data structure for this subquery */
	root = makeNode(PlannerInfo);
	root->parse = parse;
	root->glob = glob;
	root->query_level = parent_root ? parent_root->query_level + 1 : 1;
	root->parent_root = parent_root;
	root->planner_cxt = CurrentMemoryContext;
	root->init_plans = NIL;
	root->cte_plan_ids = NIL;
	root->eq_classes = NIL;
	root->append_rel_list = NIL;

	root->hasRecursion = hasRecursion;
	if (hasRecursion)
		root->wt_param_id = SS_assign_worktable_param(root);
	else
		root->wt_param_id = -1;
	root->non_recursive_plan = NULL;

	/*
	 * If there is a WITH list, process each WITH query and build an initplan
	 * SubPlan structure for it.
	 */
	if (parse->cteList)
		SS_process_ctes(root);

	/*
	 * Look for ANY and EXISTS SubLinks in WHERE and JOIN/ON clauses, and try
	 * to transform them into joins.  Note that this step does not descend
	 * into subqueries; if we pull up any subqueries below, their SubLinks are
	 * processed just before pulling them up.
	 */
	if (parse->hasSubLinks)
		pull_up_sublinks(root);

	/*
	 * Scan the rangetable for set-returning functions, and inline them if
	 * possible (producing subqueries that might get pulled up next).
	 * Recursion issues here are handled in the same way as for SubLinks.
	 */
	inline_set_returning_functions(root);

	/*
	 * Check to see if any subqueries in the rangetable can be merged into
	 * this query.
	 */
	parse->jointree = (FromExpr *)
		pull_up_subqueries(root, (Node *) parse->jointree, NULL, NULL);

	/*
	 * Detect whether any rangetable entries are RTE_JOIN kind; if not, we can
	 * avoid the expense of doing flatten_join_alias_vars().  Also check for
	 * outer joins --- if none, we can skip reduce_outer_joins(). This must be
	 * done after we have done pull_up_subqueries, of course.
	 */
	root->hasJoinRTEs = false;
	hasOuterJoins = false;
	foreach(l, parse->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);

		if (rte->rtekind == RTE_JOIN)
		{
			root->hasJoinRTEs = true;
			if (IS_OUTER_JOIN(rte->jointype))
			{
				hasOuterJoins = true;
				/* Can quit scanning once we find an outer join */
				break;
			}
		}
	}

	/*
	 * Expand any rangetable entries that are inheritance sets into "append
	 * relations".  This can add entries to the rangetable, but they must be
	 * plain base relations not joins, so it's OK (and marginally more
	 * efficient) to do it after checking for join RTEs.  We must do it after
	 * pulling up subqueries, else we'd fail to handle inherited tables in
	 * subqueries.
	 */
	expand_inherited_tables(root);

	/*
	 * Set hasHavingQual to remember if HAVING clause is present.  Needed
	 * because preprocess_expression will reduce a constant-true condition to
	 * an empty qual list ... but "HAVING TRUE" is not a semantic no-op.
	 */
	root->hasHavingQual = (parse->havingQual != NULL);

	/* Clear this flag; might get set in distribute_qual_to_rels */
	root->hasPseudoConstantQuals = false;

	/*
	 * Do expression preprocessing on targetlist and quals.
	 */
	parse->targetList = (List *)
		preprocess_expression(root, (Node *) parse->targetList,
							  EXPRKIND_TARGET);

	parse->returningList = (List *)
		preprocess_expression(root, (Node *) parse->returningList,
							  EXPRKIND_TARGET);

	preprocess_qual_conditions(root, (Node *) parse->jointree);

	parse->havingQual = preprocess_expression(root, parse->havingQual,
											  EXPRKIND_QUAL);

	parse->limitOffset = preprocess_expression(root, parse->limitOffset,
											   EXPRKIND_LIMIT);
	parse->limitCount = preprocess_expression(root, parse->limitCount,
											  EXPRKIND_LIMIT);

	root->append_rel_list = (List *)
		preprocess_expression(root, (Node *) root->append_rel_list,
							  EXPRKIND_APPINFO);

	/* Also need to preprocess expressions for function and values RTEs */
	foreach(l, parse->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);

		if (rte->rtekind == RTE_FUNCTION)
			rte->funcexpr = preprocess_expression(root, rte->funcexpr,
												  EXPRKIND_RTFUNC);
		else if (rte->rtekind == RTE_VALUES)
			rte->values_lists = (List *)
				preprocess_expression(root, (Node *) rte->values_lists,
									  EXPRKIND_VALUES);
	}

	/*
	 * In some cases we may want to transfer a HAVING clause into WHERE. We
	 * cannot do so if the HAVING clause contains aggregates (obviously) or
	 * volatile functions (since a HAVING clause is supposed to be executed
	 * only once per group).  Also, it may be that the clause is so expensive
	 * to execute that we're better off doing it only once per group, despite
	 * the loss of selectivity.  This is hard to estimate short of doing the
	 * entire planning process twice, so we use a heuristic: clauses
	 * containing subplans are left in HAVING.	Otherwise, we move or copy the
	 * HAVING clause into WHERE, in hopes of eliminating tuples before
	 * aggregation instead of after.
	 *
	 * If the query has explicit grouping then we can simply move such a
	 * clause into WHERE; any group that fails the clause will not be in the
	 * output because none of its tuples will reach the grouping or
	 * aggregation stage.  Otherwise we must have a degenerate (variable-free)
	 * HAVING clause, which we put in WHERE so that query_planner() can use it
	 * in a gating Result node, but also keep in HAVING to ensure that we
	 * don't emit a bogus aggregated row. (This could be done better, but it
	 * seems not worth optimizing.)
	 *
	 * Note that both havingQual and parse->jointree->quals are in
	 * implicitly-ANDed-list form at this point, even though they are declared
	 * as Node *.
	 */
	newHaving = NIL;
	foreach(l, (List *) parse->havingQual)
	{
		Node	   *havingclause = (Node *) lfirst(l);

		if (contain_agg_clause(havingclause) ||
			contain_volatile_functions(havingclause) ||
			contain_subplans(havingclause))
		{
			/* keep it in HAVING */
			newHaving = lappend(newHaving, havingclause);
		}
		else if (parse->groupClause)
		{
			/* move it to WHERE */
			parse->jointree->quals = (Node *)
				lappend((List *) parse->jointree->quals, havingclause);
		}
		else
		{
			/* put a copy in WHERE, keep it in HAVING */
			parse->jointree->quals = (Node *)
				lappend((List *) parse->jointree->quals,
						copyObject(havingclause));
			newHaving = lappend(newHaving, havingclause);
		}
	}
	parse->havingQual = (Node *) newHaving;

	/*
	 * If we have any outer joins, try to reduce them to plain inner joins.
	 * This step is most easily done after we've done expression
	 * preprocessing.
	 */
	if (hasOuterJoins)
		reduce_outer_joins(root);

	/*
	 * Do the main planning.  If we have an inherited target relation, that
	 * needs special processing, else go straight to grouping_planner.
	 */
	if (parse->resultRelation &&
		rt_fetch(parse->resultRelation, parse->rtable)->inh)
		plan = inheritance_planner(root);
	else
		plan = grouping_planner(root, tuple_fraction);

	/*
	 * If any subplans were generated, or if we're inside a subplan, build
	 * initPlan list and extParam/allParam sets for plan nodes, and attach the
	 * initPlans to the top plan node.
	 */
	if (list_length(glob->subplans) != num_old_subplans ||
		root->query_level > 1)
		SS_finalize_plan(root, plan, true);

	/* Return internal info if caller wants it */
	if (subroot)
		*subroot = root;

	return plan;
}

/*
 * preprocess_expression
 *		Do subquery_planner's preprocessing work for an expression,
 *		which can be a targetlist, a WHERE clause (including JOIN/ON
 *		conditions), or a HAVING clause.
 */
static Node *
preprocess_expression(PlannerInfo *root, Node *expr, int kind)
{
	/*
	 * Fall out quickly if expression is empty.  This occurs often enough to
	 * be worth checking.  Note that null->null is the correct conversion for
	 * implicit-AND result format, too.
	 */
	if (expr == NULL)
		return NULL;

	/*
	 * If the query has any join RTEs, replace join alias variables with
	 * base-relation variables. We must do this before sublink processing,
	 * else sublinks expanded out from join aliases wouldn't get processed. We
	 * can skip it in VALUES lists, however, since they can't contain any Vars
	 * at all.
	 */
	if (root->hasJoinRTEs && kind != EXPRKIND_VALUES)
		expr = flatten_join_alias_vars(root, expr);

	/*
	 * Simplify constant expressions.
	 *
	 * Note: one essential effect here is to insert the current actual values
	 * of any default arguments for functions.	To ensure that happens, we
	 * *must* process all expressions here.  Previous PG versions sometimes
	 * skipped const-simplification if it didn't seem worth the trouble, but
	 * we can't do that anymore.
	 *
	 * Note: this also flattens nested AND and OR expressions into N-argument
	 * form.  All processing of a qual expression after this point must be
	 * careful to maintain AND/OR flatness --- that is, do not generate a tree
	 * with AND directly under AND, nor OR directly under OR.
	 */
	expr = eval_const_expressions(root, expr);

	/*
	 * If it's a qual or havingQual, canonicalize it.
	 */
	if (kind == EXPRKIND_QUAL)
	{
		expr = (Node *) canonicalize_qual((Expr *) expr);

#ifdef OPTIMIZER_DEBUG
		printf("After canonicalize_qual()\n");
		pprint(expr);
#endif
	}

	/* Expand SubLinks to SubPlans */
	if (root->parse->hasSubLinks)
		expr = SS_process_sublinks(root, expr, (kind == EXPRKIND_QUAL));

	/*
	 * XXX do not insert anything here unless you have grokked the comments in
	 * SS_replace_correlation_vars ...
	 */

	/* Replace uplevel vars with Param nodes (this IS possible in VALUES) */
	if (root->query_level > 1)
		expr = SS_replace_correlation_vars(root, expr);

	/*
	 * If it's a qual or havingQual, convert it to implicit-AND format. (We
	 * don't want to do this before eval_const_expressions, since the latter
	 * would be unable to simplify a top-level AND correctly. Also,
	 * SS_process_sublinks expects explicit-AND format.)
	 */
	if (kind == EXPRKIND_QUAL)
		expr = (Node *) make_ands_implicit((Expr *) expr);

	return expr;
}

/*
 * preprocess_qual_conditions
 *		Recursively scan the query's jointree and do subquery_planner's
 *		preprocessing work on each qual condition found therein.
 */
static void
preprocess_qual_conditions(PlannerInfo *root, Node *jtnode)
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
		ListCell   *l;

		foreach(l, f->fromlist)
			preprocess_qual_conditions(root, lfirst(l));

		f->quals = preprocess_expression(root, f->quals, EXPRKIND_QUAL);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		preprocess_qual_conditions(root, j->larg);
		preprocess_qual_conditions(root, j->rarg);

		j->quals = preprocess_expression(root, j->quals, EXPRKIND_QUAL);
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
}

/*
 * inheritance_planner
 *	  Generate a plan in the case where the result relation is an
 *	  inheritance set.
 *
 * We have to handle this case differently from cases where a source relation
 * is an inheritance set. Source inheritance is expanded at the bottom of the
 * plan tree (see allpaths.c), but target inheritance has to be expanded at
 * the top.  The reason is that for UPDATE, each target relation needs a
 * different targetlist matching its own column set.  Also, for both UPDATE
 * and DELETE, the executor needs the Append plan node at the top, else it
 * can't keep track of which table is the current target table.  Fortunately,
 * the UPDATE/DELETE target can never be the nullable side of an outer join,
 * so it's OK to generate the plan this way.
 *
 * Returns a query plan.
 */
static Plan *
inheritance_planner(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	int			parentRTindex = parse->resultRelation;
	List	   *subplans = NIL;
	List	   *resultRelations = NIL;
	List	   *returningLists = NIL;
	List	   *rtable = NIL;
	List	   *tlist = NIL;
	PlannerInfo subroot;
	ListCell   *l;

	foreach(l, root->append_rel_list)
	{
		AppendRelInfo *appinfo = (AppendRelInfo *) lfirst(l);
		Plan	   *subplan;

		/* append_rel_list contains all append rels; ignore others */
		if (appinfo->parent_relid != parentRTindex)
			continue;

		/*
		 * Generate modified query with this rel as target.
		 */
		memcpy(&subroot, root, sizeof(PlannerInfo));
		subroot.parse = (Query *)
			adjust_appendrel_attrs((Node *) parse,
								   appinfo);
		subroot.returningLists = NIL;
		subroot.init_plans = NIL;
		/* We needn't modify the child's append_rel_list */
		/* There shouldn't be any OJ info to translate, as yet */
		Assert(subroot.join_info_list == NIL);
		/* and we haven't created PlaceHolderInfos, either */
		Assert(subroot.placeholder_list == NIL);

		/* Generate plan */
		subplan = grouping_planner(&subroot, 0.0 /* retrieve all tuples */ );

		/*
		 * If this child rel was excluded by constraint exclusion, exclude it
		 * from the plan.
		 */
		if (is_dummy_plan(subplan))
			continue;

		/* Save rtable and tlist from first rel for use below */
		if (subplans == NIL)
		{
			rtable = subroot.parse->rtable;
			tlist = subplan->targetlist;
		}

		subplans = lappend(subplans, subplan);

		/* Make sure any initplans from this rel get into the outer list */
		root->init_plans = list_concat(root->init_plans, subroot.init_plans);

		/* Build target-relations list for the executor */
		resultRelations = lappend_int(resultRelations, appinfo->child_relid);

		/* Build list of per-relation RETURNING targetlists */
		if (parse->returningList)
		{
			Assert(list_length(subroot.returningLists) == 1);
			returningLists = list_concat(returningLists,
										 subroot.returningLists);
		}
	}

	root->resultRelations = resultRelations;
	root->returningLists = returningLists;

	/* Mark result as unordered (probably unnecessary) */
	root->query_pathkeys = NIL;

	/*
	 * If we managed to exclude every child rel, return a dummy plan
	 */
	if (subplans == NIL)
	{
		root->resultRelations = list_make1_int(parentRTindex);
		/* although dummy, it must have a valid tlist for executor */
		tlist = preprocess_targetlist(root, parse->targetList);
		return (Plan *) make_result(root,
									tlist,
									(Node *) list_make1(makeBoolConst(false,
																	  false)),
									NULL);
	}

	/*
	 * Planning might have modified the rangetable, due to changes of the
	 * Query structures inside subquery RTEs.  We have to ensure that this
	 * gets propagated back to the master copy.  But can't do this until we
	 * are done planning, because all the calls to grouping_planner need
	 * virgin sub-Queries to work from.  (We are effectively assuming that
	 * sub-Queries will get planned identically each time, or at least that
	 * the impacts on their rangetables will be the same each time.)
	 *
	 * XXX should clean this up someday
	 */
	parse->rtable = rtable;

	/* Suppress Append if there's only one surviving child rel */
	if (list_length(subplans) == 1)
		return (Plan *) linitial(subplans);

	return (Plan *) make_append(subplans, true, tlist);
}

/*--------------------
 * grouping_planner
 *	  Perform planning steps related to grouping, aggregation, etc.
 *	  This primarily means adding top-level processing to the basic
 *	  query plan produced by query_planner.
 *
 * tuple_fraction is the fraction of tuples we expect will be retrieved
 *
 * tuple_fraction is interpreted as follows:
 *	  0: expect all tuples to be retrieved (normal case)
 *	  0 < tuple_fraction < 1: expect the given fraction of tuples available
 *		from the plan to be retrieved
 *	  tuple_fraction >= 1: tuple_fraction is the absolute number of tuples
 *		expected to be retrieved (ie, a LIMIT specification)
 *
 * Returns a query plan.  Also, root->query_pathkeys is returned as the
 * actual output ordering of the plan (in pathkey format).
 *--------------------
 */
static Plan *
grouping_planner(PlannerInfo *root, double tuple_fraction)
{
	Query	   *parse = root->parse;
	List	   *tlist = parse->targetList;
	int64		offset_est = 0;
	int64		count_est = 0;
	double		limit_tuples = -1.0;
	Plan	   *result_plan;
	List	   *current_pathkeys;
	double		dNumGroups = 0;

	/* Tweak caller-supplied tuple_fraction if have LIMIT/OFFSET */
	if (parse->limitCount || parse->limitOffset)
	{
		tuple_fraction = preprocess_limit(root, tuple_fraction,
										  &offset_est, &count_est);

		/*
		 * If we have a known LIMIT, and don't have an unknown OFFSET, we can
		 * estimate the effects of using a bounded sort.
		 */
		if (count_est > 0 && offset_est >= 0)
			limit_tuples = (double) count_est + (double) offset_est;
	}

	if (parse->setOperations)
	{
		List	   *set_sortclauses;

		/*
		 * If there's a top-level ORDER BY, assume we have to fetch all the
		 * tuples.	This might be too simplistic given all the hackery below
		 * to possibly avoid the sort; but the odds of accurate estimates here
		 * are pretty low anyway.
		 */
		if (parse->sortClause)
			tuple_fraction = 0.0;

		/*
		 * Construct the plan for set operations.  The result will not need
		 * any work except perhaps a top-level sort and/or LIMIT.  Note that
		 * any special work for recursive unions is the responsibility of
		 * plan_set_operations.
		 */
		result_plan = plan_set_operations(root, tuple_fraction,
										  &set_sortclauses);

		/*
		 * Calculate pathkeys representing the sort order (if any) of the set
		 * operation's result.  We have to do this before overwriting the sort
		 * key information...
		 */
		current_pathkeys = make_pathkeys_for_sortclauses(root,
														 set_sortclauses,
													 result_plan->targetlist,
														 true);

		/*
		 * We should not need to call preprocess_targetlist, since we must be
		 * in a SELECT query node.	Instead, use the targetlist returned by
		 * plan_set_operations (since this tells whether it returned any
		 * resjunk columns!), and transfer any sort key information from the
		 * original tlist.
		 */
		Assert(parse->commandType == CMD_SELECT);

		tlist = postprocess_setop_tlist(copyObject(result_plan->targetlist),
										tlist);

		/*
		 * Can't handle FOR UPDATE/SHARE here (parser should have checked
		 * already, but let's make sure).
		 */
		if (parse->rowMarks)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("SELECT FOR UPDATE/SHARE is not allowed with UNION/INTERSECT/EXCEPT")));

		/*
		 * Calculate pathkeys that represent result ordering requirements
		 */
		Assert(parse->distinctClause == NIL);
		root->sort_pathkeys = make_pathkeys_for_sortclauses(root,
															parse->sortClause,
															tlist,
															true);
	}
	else
	{
		/* No set operations, do regular planning */
		List	   *sub_tlist;
		AttrNumber *groupColIdx = NULL;
		bool		need_tlist_eval = true;
		QualCost	tlist_cost;
		Path	   *cheapest_path;
		Path	   *sorted_path;
		Path	   *best_path;
		long		numGroups = 0;
		AggClauseCounts agg_counts;
		int			numGroupCols;
		bool		use_hashed_grouping = false;
		WindowFuncLists *wflists = NULL;
		List	   *activeWindows = NIL;

		MemSet(&agg_counts, 0, sizeof(AggClauseCounts));

		/* A recursive query should always have setOperations */
		Assert(!root->hasRecursion);

		/* Preprocess GROUP BY clause, if any */
		if (parse->groupClause)
			preprocess_groupclause(root);
		numGroupCols = list_length(parse->groupClause);

		/* Preprocess targetlist */
		tlist = preprocess_targetlist(root, tlist);

		/*
		 * Locate any window functions in the tlist.  (We don't need to look
		 * anywhere else, since expressions used in ORDER BY will be in there
		 * too.)  Note that they could all have been eliminated by constant
		 * folding, in which case we don't need to do any more work.
		 */
		if (parse->hasWindowFuncs)
		{
			wflists = find_window_functions((Node *) tlist,
											list_length(parse->windowClause));
			if (wflists->numWindowFuncs > 0)
				activeWindows = select_active_windows(root, wflists);
			else
				parse->hasWindowFuncs = false;
		}

		/*
		 * Generate appropriate target list for subplan; may be different from
		 * tlist if grouping or aggregation is needed.
		 */
		sub_tlist = make_subplanTargetList(root, tlist,
										   &groupColIdx, &need_tlist_eval);

		/*
		 * Calculate pathkeys that represent grouping/ordering requirements.
		 * Stash them in PlannerInfo so that query_planner can canonicalize
		 * them after EquivalenceClasses have been formed.	The sortClause is
		 * certainly sort-able, but GROUP BY and DISTINCT might not be, in
		 * which case we just leave their pathkeys empty.
		 */
		if (parse->groupClause &&
			grouping_is_sortable(parse->groupClause))
			root->group_pathkeys =
				make_pathkeys_for_sortclauses(root,
											  parse->groupClause,
											  tlist,
											  false);
		else
			root->group_pathkeys = NIL;

		/* We consider only the first (bottom) window in pathkeys logic */
		if (activeWindows != NIL)
		{
			WindowClause *wc = (WindowClause *) linitial(activeWindows);

			root->window_pathkeys = make_pathkeys_for_window(root,
															 wc,
															 tlist,
															 false);
		}
		else
			root->window_pathkeys = NIL;

		if (parse->distinctClause &&
			grouping_is_sortable(parse->distinctClause))
			root->distinct_pathkeys =
				make_pathkeys_for_sortclauses(root,
											  parse->distinctClause,
											  tlist,
											  false);
		else
			root->distinct_pathkeys = NIL;

		root->sort_pathkeys =
			make_pathkeys_for_sortclauses(root,
										  parse->sortClause,
										  tlist,
										  false);

		/*
		 * Will need actual number of aggregates for estimating costs.
		 *
		 * Note: we do not attempt to detect duplicate aggregates here; a
		 * somewhat-overestimated count is okay for our present purposes.
		 *
		 * Note: think not that we can turn off hasAggs if we find no aggs. It
		 * is possible for constant-expression simplification to remove all
		 * explicit references to aggs, but we still have to follow the
		 * aggregate semantics (eg, producing only one output row).
		 */
		if (parse->hasAggs)
		{
			count_agg_clauses((Node *) tlist, &agg_counts);
			count_agg_clauses(parse->havingQual, &agg_counts);
		}

		/*
		 * Figure out whether we want a sorted result from query_planner.
		 *
		 * If we have a sortable GROUP BY clause, then we want a result sorted
		 * properly for grouping.  Otherwise, if we have window functions to
		 * evaluate, we try to sort for the first window.  Otherwise, if
		 * there's a sortable DISTINCT clause that's more rigorous than the
		 * ORDER BY clause, we try to produce output that's sufficiently well
		 * sorted for the DISTINCT.  Otherwise, if there is an ORDER BY
		 * clause, we want to sort by the ORDER BY clause.
		 *
		 * Note: if we have both ORDER BY and GROUP BY, and ORDER BY is a
		 * superset of GROUP BY, it would be tempting to request sort by ORDER
		 * BY --- but that might just leave us failing to exploit an available
		 * sort order at all.  Needs more thought.	The choice for DISTINCT
		 * versus ORDER BY is much easier, since we know that the parser
		 * ensured that one is a superset of the other.
		 */
		if (root->group_pathkeys)
			root->query_pathkeys = root->group_pathkeys;
		else if (root->window_pathkeys)
			root->query_pathkeys = root->window_pathkeys;
		else if (list_length(root->distinct_pathkeys) >
				 list_length(root->sort_pathkeys))
			root->query_pathkeys = root->distinct_pathkeys;
		else if (root->sort_pathkeys)
			root->query_pathkeys = root->sort_pathkeys;
		else
			root->query_pathkeys = NIL;

		/*
		 * Generate the best unsorted and presorted paths for this Query (but
		 * note there may not be any presorted path).  query_planner will also
		 * estimate the number of groups in the query, and canonicalize all
		 * the pathkeys.
		 */
		query_planner(root, sub_tlist, tuple_fraction, limit_tuples,
					  &cheapest_path, &sorted_path, &dNumGroups);

		/*
		 * If grouping, decide whether to use sorted or hashed grouping.
		 */
		if (parse->groupClause)
		{
			bool		can_hash;
			bool		can_sort;

			/*
			 * Executor doesn't support hashed aggregation with DISTINCT
			 * aggregates.	(Doing so would imply storing *all* the input
			 * values in the hash table, which seems like a certain loser.)
			 */
			can_hash = (agg_counts.numDistinctAggs == 0 &&
						grouping_is_hashable(parse->groupClause));
			can_sort = grouping_is_sortable(parse->groupClause);
			if (can_hash && can_sort)
			{
				/* we have a meaningful choice to make ... */
				use_hashed_grouping =
					choose_hashed_grouping(root,
										   tuple_fraction, limit_tuples,
										   cheapest_path, sorted_path,
										   dNumGroups, &agg_counts);
			}
			else if (can_hash)
				use_hashed_grouping = true;
			else if (can_sort)
				use_hashed_grouping = false;
			else
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("could not implement GROUP BY"),
						 errdetail("Some of the datatypes only support hashing, while others only support sorting.")));

			/* Also convert # groups to long int --- but 'ware overflow! */
			numGroups = (long) Min(dNumGroups, (double) LONG_MAX);
		}

		/*
		 * Select the best path.  If we are doing hashed grouping, we will
		 * always read all the input tuples, so use the cheapest-total path.
		 * Otherwise, trust query_planner's decision about which to use.
		 */
		if (use_hashed_grouping || !sorted_path)
			best_path = cheapest_path;
		else
			best_path = sorted_path;

		/*
		 * Check to see if it's possible to optimize MIN/MAX aggregates. If
		 * so, we will forget all the work we did so far to choose a "regular"
		 * path ... but we had to do it anyway to be able to tell which way is
		 * cheaper.
		 */
		result_plan = optimize_minmax_aggregates(root,
												 tlist,
												 best_path);
		if (result_plan != NULL)
		{
			/*
			 * optimize_minmax_aggregates generated the full plan, with the
			 * right tlist, and it has no sort order.
			 */
			current_pathkeys = NIL;
		}
		else
		{
			/*
			 * Normal case --- create a plan according to query_planner's
			 * results.
			 */
			bool		need_sort_for_grouping = false;

			result_plan = create_plan(root, best_path);
			current_pathkeys = best_path->pathkeys;

			/* Detect if we'll need an explicit sort for grouping */
			if (parse->groupClause && !use_hashed_grouping &&
			  !pathkeys_contained_in(root->group_pathkeys, current_pathkeys))
			{
				need_sort_for_grouping = true;

				/*
				 * Always override query_planner's tlist, so that we don't
				 * sort useless data from a "physical" tlist.
				 */
				need_tlist_eval = true;
			}

			/*
			 * create_plan() returns a plan with just a "flat" tlist of
			 * required Vars.  Usually we need to insert the sub_tlist as the
			 * tlist of the top plan node.	However, we can skip that if we
			 * determined that whatever query_planner chose to return will be
			 * good enough.
			 */
			if (need_tlist_eval)
			{
				/*
				 * If the top-level plan node is one that cannot do expression
				 * evaluation, we must insert a Result node to project the
				 * desired tlist.
				 */
				if (!is_projection_capable_plan(result_plan))
				{
					result_plan = (Plan *) make_result(root,
													   sub_tlist,
													   NULL,
													   result_plan);
				}
				else
				{
					/*
					 * Otherwise, just replace the subplan's flat tlist with
					 * the desired tlist.
					 */
					result_plan->targetlist = sub_tlist;
				}

				/*
				 * Also, account for the cost of evaluation of the sub_tlist.
				 *
				 * Up to now, we have only been dealing with "flat" tlists,
				 * containing just Vars.  So their evaluation cost is zero
				 * according to the model used by cost_qual_eval() (or if you
				 * prefer, the cost is factored into cpu_tuple_cost).  Thus we
				 * can avoid accounting for tlist cost throughout
				 * query_planner() and subroutines.  But now we've inserted a
				 * tlist that might contain actual operators, sub-selects, etc
				 * --- so we'd better account for its cost.
				 *
				 * Below this point, any tlist eval cost for added-on nodes
				 * should be accounted for as we create those nodes.
				 * Presently, of the node types we can add on, only Agg,
				 * WindowAgg, and Group project new tlists (the rest just copy
				 * their input tuples) --- so make_agg(), make_windowagg() and
				 * make_group() are responsible for computing the added cost.
				 */
				cost_qual_eval(&tlist_cost, sub_tlist, root);
				result_plan->startup_cost += tlist_cost.startup;
				result_plan->total_cost += tlist_cost.startup +
					tlist_cost.per_tuple * result_plan->plan_rows;
			}
			else
			{
				/*
				 * Since we're using query_planner's tlist and not the one
				 * make_subplanTargetList calculated, we have to refigure any
				 * grouping-column indexes make_subplanTargetList computed.
				 */
				locate_grouping_columns(root, tlist, result_plan->targetlist,
										groupColIdx);
			}

			/*
			 * Insert AGG or GROUP node if needed, plus an explicit sort step
			 * if necessary.
			 *
			 * HAVING clause, if any, becomes qual of the Agg or Group node.
			 */
			if (use_hashed_grouping)
			{
				/* Hashed aggregate plan --- no sort needed */
				result_plan = (Plan *) make_agg(root,
												tlist,
												(List *) parse->havingQual,
												AGG_HASHED,
												numGroupCols,
												groupColIdx,
									extract_grouping_ops(parse->groupClause),
												numGroups,
												agg_counts.numAggs,
												result_plan);
				/* Hashed aggregation produces randomly-ordered results */
				current_pathkeys = NIL;
			}
			else if (parse->hasAggs)
			{
				/* Plain aggregate plan --- sort if needed */
				AggStrategy aggstrategy;

				if (parse->groupClause)
				{
					if (need_sort_for_grouping)
					{
						result_plan = (Plan *)
							make_sort_from_groupcols(root,
													 parse->groupClause,
													 groupColIdx,
													 result_plan);
						current_pathkeys = root->group_pathkeys;
					}
					aggstrategy = AGG_SORTED;

					/*
					 * The AGG node will not change the sort ordering of its
					 * groups, so current_pathkeys describes the result too.
					 */
				}
				else
				{
					aggstrategy = AGG_PLAIN;
					/* Result will be only one row anyway; no sort order */
					current_pathkeys = NIL;
				}

				result_plan = (Plan *) make_agg(root,
												tlist,
												(List *) parse->havingQual,
												aggstrategy,
												numGroupCols,
												groupColIdx,
									extract_grouping_ops(parse->groupClause),
												numGroups,
												agg_counts.numAggs,
												result_plan);
			}
			else if (parse->groupClause)
			{
				/*
				 * GROUP BY without aggregation, so insert a group node (plus
				 * the appropriate sort node, if necessary).
				 *
				 * Add an explicit sort if we couldn't make the path come out
				 * the way the GROUP node needs it.
				 */
				if (need_sort_for_grouping)
				{
					result_plan = (Plan *)
						make_sort_from_groupcols(root,
												 parse->groupClause,
												 groupColIdx,
												 result_plan);
					current_pathkeys = root->group_pathkeys;
				}

				result_plan = (Plan *) make_group(root,
												  tlist,
												  (List *) parse->havingQual,
												  numGroupCols,
												  groupColIdx,
									extract_grouping_ops(parse->groupClause),
												  dNumGroups,
												  result_plan);
				/* The Group node won't change sort ordering */
			}
			else if (root->hasHavingQual)
			{
				/*
				 * No aggregates, and no GROUP BY, but we have a HAVING qual.
				 * This is a degenerate case in which we are supposed to emit
				 * either 0 or 1 row depending on whether HAVING succeeds.
				 * Furthermore, there cannot be any variables in either HAVING
				 * or the targetlist, so we actually do not need the FROM
				 * table at all!  We can just throw away the plan-so-far and
				 * generate a Result node.	This is a sufficiently unusual
				 * corner case that it's not worth contorting the structure of
				 * this routine to avoid having to generate the plan in the
				 * first place.
				 */
				result_plan = (Plan *) make_result(root,
												   tlist,
												   parse->havingQual,
												   NULL);
			}
		}						/* end of non-minmax-aggregate case */

		/*
		 * Since each window function could require a different sort order, we
		 * stack up a WindowAgg node for each window, with sort steps between
		 * them as needed.
		 */
		if (activeWindows)
		{
			List	   *window_tlist;
			ListCell   *l;

			/*
			 * If the top-level plan node is one that cannot do expression
			 * evaluation, we must insert a Result node to project the desired
			 * tlist.  (In some cases this might not really be required, but
			 * it's not worth trying to avoid it.)  Note that on second and
			 * subsequent passes through the following loop, the top-level
			 * node will be a WindowAgg which we know can project; so we only
			 * need to check once.
			 */
			if (!is_projection_capable_plan(result_plan))
			{
				result_plan = (Plan *) make_result(root,
												   NIL,
												   NULL,
												   result_plan);
			}

			/*
			 * The "base" targetlist for all steps of the windowing process is
			 * a flat tlist of all Vars and Aggs needed in the result. (In
			 * some cases we wouldn't need to propagate all of these all the
			 * way to the top, since they might only be needed as inputs to
			 * WindowFuncs.  It's probably not worth trying to optimize that
			 * though.)  We also need any volatile sort expressions, because
			 * make_sort_from_pathkeys won't add those on its own, and anyway
			 * we want them evaluated only once at the bottom of the stack.
			 * As we climb up the stack, we add outputs for the WindowFuncs
			 * computed at each level.	Also, each input tlist has to present
			 * all the columns needed to sort the data for the next WindowAgg
			 * step.  That's handled internally by make_sort_from_pathkeys,
			 * but we need the copyObject steps here to ensure that each plan
			 * node has a separately modifiable tlist.
			 */
			window_tlist = flatten_tlist(tlist);
			if (parse->hasAggs)
				window_tlist = add_to_flat_tlist(window_tlist,
											pull_agg_clause((Node *) tlist));
			window_tlist = add_volatile_sort_exprs(window_tlist, tlist,
												   activeWindows);
			result_plan->targetlist = (List *) copyObject(window_tlist);

			foreach(l, activeWindows)
			{
				WindowClause *wc = (WindowClause *) lfirst(l);
				List	   *window_pathkeys;
				int			partNumCols;
				AttrNumber *partColIdx;
				Oid		   *partOperators;
				int			ordNumCols;
				AttrNumber *ordColIdx;
				Oid		   *ordOperators;

				window_pathkeys = make_pathkeys_for_window(root,
														   wc,
														   tlist,
														   true);

				/*
				 * This is a bit tricky: we build a sort node even if we don't
				 * really have to sort.  Even when no explicit sort is needed,
				 * we need to have suitable resjunk items added to the input
				 * plan's tlist for any partitioning or ordering columns that
				 * aren't plain Vars.  Furthermore, this way we can use
				 * existing infrastructure to identify which input columns are
				 * the interesting ones.
				 */
				if (window_pathkeys)
				{
					Sort	   *sort_plan;

					sort_plan = make_sort_from_pathkeys(root,
														result_plan,
														window_pathkeys,
														-1.0);
					if (!pathkeys_contained_in(window_pathkeys,
											   current_pathkeys))
					{
						/* we do indeed need to sort */
						result_plan = (Plan *) sort_plan;
						current_pathkeys = window_pathkeys;
					}
					/* In either case, extract the per-column information */
					get_column_info_for_window(root, wc, tlist,
											   sort_plan->numCols,
											   sort_plan->sortColIdx,
											   &partNumCols,
											   &partColIdx,
											   &partOperators,
											   &ordNumCols,
											   &ordColIdx,
											   &ordOperators);
				}
				else
				{
					/* empty window specification, nothing to sort */
					partNumCols = 0;
					partColIdx = NULL;
					partOperators = NULL;
					ordNumCols = 0;
					ordColIdx = NULL;
					ordOperators = NULL;
				}

				if (lnext(l))
				{
					/* Add the current WindowFuncs to the running tlist */
					window_tlist = add_to_flat_tlist(window_tlist,
										   wflists->windowFuncs[wc->winref]);
				}
				else
				{
					/* Install the original tlist in the topmost WindowAgg */
					window_tlist = tlist;
				}

				/* ... and make the WindowAgg plan node */
				result_plan = (Plan *)
					make_windowagg(root,
								   (List *) copyObject(window_tlist),
							   list_length(wflists->windowFuncs[wc->winref]),
								   wc->winref,
								   partNumCols,
								   partColIdx,
								   partOperators,
								   ordNumCols,
								   ordColIdx,
								   ordOperators,
								   wc->frameOptions,
								   result_plan);
			}
		}
	}							/* end of if (setOperations) */

	/*
	 * If there is a DISTINCT clause, add the necessary node(s).
	 */
	if (parse->distinctClause)
	{
		double		dNumDistinctRows;
		long		numDistinctRows;
		bool		use_hashed_distinct;
		bool		can_sort;
		bool		can_hash;

		/*
		 * If there was grouping or aggregation, use the current number of
		 * rows as the estimated number of DISTINCT rows (ie, assume the
		 * result was already mostly unique).  If not, use the number of
		 * distinct-groups calculated by query_planner.
		 */
		if (parse->groupClause || root->hasHavingQual || parse->hasAggs)
			dNumDistinctRows = result_plan->plan_rows;
		else
			dNumDistinctRows = dNumGroups;

		/* Also convert to long int --- but 'ware overflow! */
		numDistinctRows = (long) Min(dNumDistinctRows, (double) LONG_MAX);

		/*
		 * If we have a sortable DISTINCT ON clause, we always use sorting.
		 * This enforces the expected behavior of DISTINCT ON.
		 */
		can_sort = grouping_is_sortable(parse->distinctClause);
		if (can_sort && parse->hasDistinctOn)
			use_hashed_distinct = false;
		else
		{
			can_hash = grouping_is_hashable(parse->distinctClause);
			if (can_hash && can_sort)
			{
				/* we have a meaningful choice to make ... */
				use_hashed_distinct =
					choose_hashed_distinct(root,
										   result_plan, current_pathkeys,
										   tuple_fraction, limit_tuples,
										   dNumDistinctRows);
			}
			else if (can_hash)
				use_hashed_distinct = true;
			else if (can_sort)
				use_hashed_distinct = false;
			else
			{
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("could not implement DISTINCT"),
						 errdetail("Some of the datatypes only support hashing, while others only support sorting.")));
				use_hashed_distinct = false;	/* keep compiler quiet */
			}
		}

		if (use_hashed_distinct)
		{
			/* Hashed aggregate plan --- no sort needed */
			result_plan = (Plan *) make_agg(root,
											result_plan->targetlist,
											NIL,
											AGG_HASHED,
										  list_length(parse->distinctClause),
								 extract_grouping_cols(parse->distinctClause,
													result_plan->targetlist),
								 extract_grouping_ops(parse->distinctClause),
											numDistinctRows,
											0,
											result_plan);
			/* Hashed aggregation produces randomly-ordered results */
			current_pathkeys = NIL;
		}
		else
		{
			/*
			 * Use a Unique node to implement DISTINCT.  Add an explicit sort
			 * if we couldn't make the path come out the way the Unique node
			 * needs it.  If we do have to sort, always sort by the more
			 * rigorous of DISTINCT and ORDER BY, to avoid a second sort
			 * below.  However, for regular DISTINCT, don't sort now if we
			 * don't have to --- sorting afterwards will likely be cheaper,
			 * and also has the possibility of optimizing via LIMIT.  But for
			 * DISTINCT ON, we *must* force the final sort now, else it won't
			 * have the desired behavior.
			 */
			List	   *needed_pathkeys;

			if (parse->hasDistinctOn &&
				list_length(root->distinct_pathkeys) <
				list_length(root->sort_pathkeys))
				needed_pathkeys = root->sort_pathkeys;
			else
				needed_pathkeys = root->distinct_pathkeys;

			if (!pathkeys_contained_in(needed_pathkeys, current_pathkeys))
			{
				if (list_length(root->distinct_pathkeys) >=
					list_length(root->sort_pathkeys))
					current_pathkeys = root->distinct_pathkeys;
				else
				{
					current_pathkeys = root->sort_pathkeys;
					/* Assert checks that parser didn't mess up... */
					Assert(pathkeys_contained_in(root->distinct_pathkeys,
												 current_pathkeys));
				}

				result_plan = (Plan *) make_sort_from_pathkeys(root,
															   result_plan,
															current_pathkeys,
															   -1.0);
			}

			result_plan = (Plan *) make_unique(result_plan,
											   parse->distinctClause);
			result_plan->plan_rows = dNumDistinctRows;
			/* The Unique node won't change sort ordering */
		}
	}

	/*
	 * If ORDER BY was given and we were not able to make the plan come out in
	 * the right order, add an explicit sort step.
	 */
	if (parse->sortClause)
	{
		if (!pathkeys_contained_in(root->sort_pathkeys, current_pathkeys))
		{
			result_plan = (Plan *) make_sort_from_pathkeys(root,
														   result_plan,
														 root->sort_pathkeys,
														   limit_tuples);
			current_pathkeys = root->sort_pathkeys;
		}
	}

	/*
	 * Finally, if there is a LIMIT/OFFSET clause, add the LIMIT node.
	 */
	if (parse->limitCount || parse->limitOffset)
	{
		result_plan = (Plan *) make_limit(result_plan,
										  parse->limitOffset,
										  parse->limitCount,
										  offset_est,
										  count_est);
	}

	/*
	 * Deal with the RETURNING clause if any.  It's convenient to pass the
	 * returningList through setrefs.c now rather than at top level (if we
	 * waited, handling inherited UPDATE/DELETE would be much harder).
	 */
	if (parse->returningList)
	{
		List	   *rlist;

		Assert(parse->resultRelation);
		rlist = set_returning_clause_references(root->glob,
												parse->returningList,
												result_plan,
												parse->resultRelation);
		root->returningLists = list_make1(rlist);
	}
	else
		root->returningLists = NIL;

	/* Compute result-relations list if needed */
	if (parse->resultRelation)
		root->resultRelations = list_make1_int(parse->resultRelation);
	else
		root->resultRelations = NIL;

	/*
	 * Return the actual output ordering in query_pathkeys for possible use by
	 * an outer query level.
	 */
	root->query_pathkeys = current_pathkeys;

	return result_plan;
}

/*
 * Detect whether a plan node is a "dummy" plan created when a relation
 * is deemed not to need scanning due to constraint exclusion.
 *
 * Currently, such dummy plans are Result nodes with constant FALSE
 * filter quals.
 */
static bool
is_dummy_plan(Plan *plan)
{
	if (IsA(plan, Result))
	{
		List	   *rcqual = (List *) ((Result *) plan)->resconstantqual;

		if (list_length(rcqual) == 1)
		{
			Const	   *constqual = (Const *) linitial(rcqual);

			if (constqual && IsA(constqual, Const))
			{
				if (!constqual->constisnull &&
					!DatumGetBool(constqual->constvalue))
					return true;
			}
		}
	}
	return false;
}

/*
 * preprocess_limit - do pre-estimation for LIMIT and/or OFFSET clauses
 *
 * We try to estimate the values of the LIMIT/OFFSET clauses, and pass the
 * results back in *count_est and *offset_est.	These variables are set to
 * 0 if the corresponding clause is not present, and -1 if it's present
 * but we couldn't estimate the value for it.  (The "0" convention is OK
 * for OFFSET but a little bit bogus for LIMIT: effectively we estimate
 * LIMIT 0 as though it were LIMIT 1.  But this is in line with the planner's
 * usual practice of never estimating less than one row.)  These values will
 * be passed to make_limit, which see if you change this code.
 *
 * The return value is the suitably adjusted tuple_fraction to use for
 * planning the query.	This adjustment is not overridable, since it reflects
 * plan actions that grouping_planner() will certainly take, not assumptions
 * about context.
 */
static double
preprocess_limit(PlannerInfo *root, double tuple_fraction,
				 int64 *offset_est, int64 *count_est)
{
	Query	   *parse = root->parse;
	Node	   *est;
	double		limit_fraction;

	/* Should not be called unless LIMIT or OFFSET */
	Assert(parse->limitCount || parse->limitOffset);

	/*
	 * Try to obtain the clause values.  We use estimate_expression_value
	 * primarily because it can sometimes do something useful with Params.
	 */
	if (parse->limitCount)
	{
		est = estimate_expression_value(root, parse->limitCount);
		if (est && IsA(est, Const))
		{
			if (((Const *) est)->constisnull)
			{
				/* NULL indicates LIMIT ALL, ie, no limit */
				*count_est = 0; /* treat as not present */
			}
			else
			{
				*count_est = DatumGetInt64(((Const *) est)->constvalue);
				if (*count_est <= 0)
					*count_est = 1;		/* force to at least 1 */
			}
		}
		else
			*count_est = -1;	/* can't estimate */
	}
	else
		*count_est = 0;			/* not present */

	if (parse->limitOffset)
	{
		est = estimate_expression_value(root, parse->limitOffset);
		if (est && IsA(est, Const))
		{
			if (((Const *) est)->constisnull)
			{
				/* Treat NULL as no offset; the executor will too */
				*offset_est = 0;	/* treat as not present */
			}
			else
			{
				*offset_est = DatumGetInt64(((Const *) est)->constvalue);
				if (*offset_est < 0)
					*offset_est = 0;	/* less than 0 is same as 0 */
			}
		}
		else
			*offset_est = -1;	/* can't estimate */
	}
	else
		*offset_est = 0;		/* not present */

	if (*count_est != 0)
	{
		/*
		 * A LIMIT clause limits the absolute number of tuples returned.
		 * However, if it's not a constant LIMIT then we have to guess; for
		 * lack of a better idea, assume 10% of the plan's result is wanted.
		 */
		if (*count_est < 0 || *offset_est < 0)
		{
			/* LIMIT or OFFSET is an expression ... punt ... */
			limit_fraction = 0.10;
		}
		else
		{
			/* LIMIT (plus OFFSET, if any) is max number of tuples needed */
			limit_fraction = (double) *count_est + (double) *offset_est;
		}

		/*
		 * If we have absolute limits from both caller and LIMIT, use the
		 * smaller value; likewise if they are both fractional.  If one is
		 * fractional and the other absolute, we can't easily determine which
		 * is smaller, but we use the heuristic that the absolute will usually
		 * be smaller.
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
				/* caller absolute, limit fractional; use caller's value */
			}
		}
		else if (tuple_fraction > 0.0)
		{
			if (limit_fraction >= 1.0)
			{
				/* caller fractional, limit absolute; use limit */
				tuple_fraction = limit_fraction;
			}
			else
			{
				/* both fractional */
				tuple_fraction = Min(tuple_fraction, limit_fraction);
			}
		}
		else
		{
			/* no info from caller, just use limit */
			tuple_fraction = limit_fraction;
		}
	}
	else if (*offset_est != 0 && tuple_fraction > 0.0)
	{
		/*
		 * We have an OFFSET but no LIMIT.	This acts entirely differently
		 * from the LIMIT case: here, we need to increase rather than decrease
		 * the caller's tuple_fraction, because the OFFSET acts to cause more
		 * tuples to be fetched instead of fewer.  This only matters if we got
		 * a tuple_fraction > 0, however.
		 *
		 * As above, use 10% if OFFSET is present but unestimatable.
		 */
		if (*offset_est < 0)
			limit_fraction = 0.10;
		else
			limit_fraction = (double) *offset_est;

		/*
		 * If we have absolute counts from both caller and OFFSET, add them
		 * together; likewise if they are both fractional.	If one is
		 * fractional and the other absolute, we want to take the larger, and
		 * we heuristically assume that's the fractional one.
		 */
		if (tuple_fraction >= 1.0)
		{
			if (limit_fraction >= 1.0)
			{
				/* both absolute, so add them together */
				tuple_fraction += limit_fraction;
			}
			else
			{
				/* caller absolute, limit fractional; use limit */
				tuple_fraction = limit_fraction;
			}
		}
		else
		{
			if (limit_fraction >= 1.0)
			{
				/* caller fractional, limit absolute; use caller's value */
			}
			else
			{
				/* both fractional, so add them together */
				tuple_fraction += limit_fraction;
				if (tuple_fraction >= 1.0)
					tuple_fraction = 0.0;		/* assume fetch all */
			}
		}
	}

	return tuple_fraction;
}


/*
 * preprocess_groupclause - do preparatory work on GROUP BY clause
 *
 * The idea here is to adjust the ordering of the GROUP BY elements
 * (which in itself is semantically insignificant) to match ORDER BY,
 * thereby allowing a single sort operation to both implement the ORDER BY
 * requirement and set up for a Unique step that implements GROUP BY.
 *
 * In principle it might be interesting to consider other orderings of the
 * GROUP BY elements, which could match the sort ordering of other
 * possible plans (eg an indexscan) and thereby reduce cost.  We don't
 * bother with that, though.  Hashed grouping will frequently win anyway.
 *
 * Note: we need no comparable processing of the distinctClause because
 * the parser already enforced that that matches ORDER BY.
 */
static void
preprocess_groupclause(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	List	   *new_groupclause;
	bool		partial_match;
	ListCell   *sl;
	ListCell   *gl;

	/* If no ORDER BY, nothing useful to do here */
	if (parse->sortClause == NIL)
		return;

	/*
	 * Scan the ORDER BY clause and construct a list of matching GROUP BY
	 * items, but only as far as we can make a matching prefix.
	 *
	 * This code assumes that the sortClause contains no duplicate items.
	 */
	new_groupclause = NIL;
	foreach(sl, parse->sortClause)
	{
		SortGroupClause *sc = (SortGroupClause *) lfirst(sl);

		foreach(gl, parse->groupClause)
		{
			SortGroupClause *gc = (SortGroupClause *) lfirst(gl);

			if (equal(gc, sc))
			{
				new_groupclause = lappend(new_groupclause, gc);
				break;
			}
		}
		if (gl == NULL)
			break;				/* no match, so stop scanning */
	}

	/* Did we match all of the ORDER BY list, or just some of it? */
	partial_match = (sl != NULL);

	/* If no match at all, no point in reordering GROUP BY */
	if (new_groupclause == NIL)
		return;

	/*
	 * Add any remaining GROUP BY items to the new list, but only if we were
	 * able to make a complete match.  In other words, we only rearrange the
	 * GROUP BY list if the result is that one list is a prefix of the other
	 * --- otherwise there's no possibility of a common sort.  Also, give up
	 * if there are any non-sortable GROUP BY items, since then there's no
	 * hope anyway.
	 */
	foreach(gl, parse->groupClause)
	{
		SortGroupClause *gc = (SortGroupClause *) lfirst(gl);

		if (list_member_ptr(new_groupclause, gc))
			continue;			/* it matched an ORDER BY item */
		if (partial_match)
			return;				/* give up, no common sort possible */
		if (!OidIsValid(gc->sortop))
			return;				/* give up, GROUP BY can't be sorted */
		new_groupclause = lappend(new_groupclause, gc);
	}

	/* Success --- install the rearranged GROUP BY list */
	Assert(list_length(parse->groupClause) == list_length(new_groupclause));
	parse->groupClause = new_groupclause;
}

/*
 * choose_hashed_grouping - should we use hashed grouping?
 *
 * Note: this is only applied when both alternatives are actually feasible.
 */
static bool
choose_hashed_grouping(PlannerInfo *root,
					   double tuple_fraction, double limit_tuples,
					   Path *cheapest_path, Path *sorted_path,
					   double dNumGroups, AggClauseCounts *agg_counts)
{
	int			numGroupCols = list_length(root->parse->groupClause);
	double		cheapest_path_rows;
	int			cheapest_path_width;
	Size		hashentrysize;
	List	   *target_pathkeys;
	List	   *current_pathkeys;
	Path		hashed_p;
	Path		sorted_p;

	/* Prefer sorting when enable_hashagg is off */
	if (!enable_hashagg)
		return false;

	/*
	 * Don't do it if it doesn't look like the hashtable will fit into
	 * work_mem.
	 *
	 * Beware here of the possibility that cheapest_path->parent is NULL. This
	 * could happen if user does something silly like SELECT 'foo' GROUP BY 1;
	 */
	if (cheapest_path->parent)
	{
		cheapest_path_rows = cheapest_path->parent->rows;
		cheapest_path_width = cheapest_path->parent->width;
	}
	else
	{
		cheapest_path_rows = 1; /* assume non-set result */
		cheapest_path_width = 100;		/* arbitrary */
	}

	/* Estimate per-hash-entry space at tuple width... */
	hashentrysize = MAXALIGN(cheapest_path_width) + MAXALIGN(sizeof(MinimalTupleData));
	/* plus space for pass-by-ref transition values... */
	hashentrysize += agg_counts->transitionSpace;
	/* plus the per-hash-entry overhead */
	hashentrysize += hash_agg_entry_size(agg_counts->numAggs);

	if (hashentrysize * dNumGroups > work_mem * 1024L)
		return false;

	/*
	 * When we have both GROUP BY and DISTINCT, use the more-rigorous of
	 * DISTINCT and ORDER BY as the assumed required output sort order. This
	 * is an oversimplification because the DISTINCT might get implemented via
	 * hashing, but it's not clear that the case is common enough (or that our
	 * estimates are good enough) to justify trying to solve it exactly.
	 */
	if (list_length(root->distinct_pathkeys) >
		list_length(root->sort_pathkeys))
		target_pathkeys = root->distinct_pathkeys;
	else
		target_pathkeys = root->sort_pathkeys;

	/*
	 * See if the estimated cost is no more than doing it the other way. While
	 * avoiding the need for sorted input is usually a win, the fact that the
	 * output won't be sorted may be a loss; so we need to do an actual cost
	 * comparison.
	 *
	 * We need to consider cheapest_path + hashagg [+ final sort] versus
	 * either cheapest_path [+ sort] + group or agg [+ final sort] or
	 * presorted_path + group or agg [+ final sort] where brackets indicate a
	 * step that may not be needed. We assume query_planner() will have
	 * returned a presorted path only if it's a winner compared to
	 * cheapest_path for this purpose.
	 *
	 * These path variables are dummies that just hold cost fields; we don't
	 * make actual Paths for these steps.
	 */
	cost_agg(&hashed_p, root, AGG_HASHED, agg_counts->numAggs,
			 numGroupCols, dNumGroups,
			 cheapest_path->startup_cost, cheapest_path->total_cost,
			 cheapest_path_rows);
	/* Result of hashed agg is always unsorted */
	if (target_pathkeys)
		cost_sort(&hashed_p, root, target_pathkeys, hashed_p.total_cost,
				  dNumGroups, cheapest_path_width, limit_tuples);

	if (sorted_path)
	{
		sorted_p.startup_cost = sorted_path->startup_cost;
		sorted_p.total_cost = sorted_path->total_cost;
		current_pathkeys = sorted_path->pathkeys;
	}
	else
	{
		sorted_p.startup_cost = cheapest_path->startup_cost;
		sorted_p.total_cost = cheapest_path->total_cost;
		current_pathkeys = cheapest_path->pathkeys;
	}
	if (!pathkeys_contained_in(root->group_pathkeys, current_pathkeys))
	{
		cost_sort(&sorted_p, root, root->group_pathkeys, sorted_p.total_cost,
				  cheapest_path_rows, cheapest_path_width, -1.0);
		current_pathkeys = root->group_pathkeys;
	}

	if (root->parse->hasAggs)
		cost_agg(&sorted_p, root, AGG_SORTED, agg_counts->numAggs,
				 numGroupCols, dNumGroups,
				 sorted_p.startup_cost, sorted_p.total_cost,
				 cheapest_path_rows);
	else
		cost_group(&sorted_p, root, numGroupCols, dNumGroups,
				   sorted_p.startup_cost, sorted_p.total_cost,
				   cheapest_path_rows);
	/* The Agg or Group node will preserve ordering */
	if (target_pathkeys &&
		!pathkeys_contained_in(target_pathkeys, current_pathkeys))
		cost_sort(&sorted_p, root, target_pathkeys, sorted_p.total_cost,
				  dNumGroups, cheapest_path_width, limit_tuples);

	/*
	 * Now make the decision using the top-level tuple fraction.  First we
	 * have to convert an absolute count (LIMIT) into fractional form.
	 */
	if (tuple_fraction >= 1.0)
		tuple_fraction /= dNumGroups;

	if (compare_fractional_path_costs(&hashed_p, &sorted_p,
									  tuple_fraction) < 0)
	{
		/* Hashed is cheaper, so use it */
		return true;
	}
	return false;
}

/*
 * choose_hashed_distinct - should we use hashing for DISTINCT?
 *
 * This is fairly similar to choose_hashed_grouping, but there are enough
 * differences that it doesn't seem worth trying to unify the two functions.
 *
 * But note that making the two choices independently is a bit bogus in
 * itself.	If the two could be combined into a single choice operation
 * it'd probably be better, but that seems far too unwieldy to be practical,
 * especially considering that the combination of GROUP BY and DISTINCT
 * isn't very common in real queries.  By separating them, we are giving
 * extra preference to using a sorting implementation when a common sort key
 * is available ... and that's not necessarily wrong anyway.
 *
 * Note: this is only applied when both alternatives are actually feasible.
 */
static bool
choose_hashed_distinct(PlannerInfo *root,
					   Plan *input_plan, List *input_pathkeys,
					   double tuple_fraction, double limit_tuples,
					   double dNumDistinctRows)
{
	int			numDistinctCols = list_length(root->parse->distinctClause);
	Size		hashentrysize;
	List	   *current_pathkeys;
	List	   *needed_pathkeys;
	Path		hashed_p;
	Path		sorted_p;

	/* Prefer sorting when enable_hashagg is off */
	if (!enable_hashagg)
		return false;

	/*
	 * Don't do it if it doesn't look like the hashtable will fit into
	 * work_mem.
	 */
	hashentrysize = MAXALIGN(input_plan->plan_width) + MAXALIGN(sizeof(MinimalTupleData));

	if (hashentrysize * dNumDistinctRows > work_mem * 1024L)
		return false;

	/*
	 * See if the estimated cost is no more than doing it the other way. While
	 * avoiding the need for sorted input is usually a win, the fact that the
	 * output won't be sorted may be a loss; so we need to do an actual cost
	 * comparison.
	 *
	 * We need to consider input_plan + hashagg [+ final sort] versus
	 * input_plan [+ sort] + group [+ final sort] where brackets indicate a
	 * step that may not be needed.
	 *
	 * These path variables are dummies that just hold cost fields; we don't
	 * make actual Paths for these steps.
	 */
	cost_agg(&hashed_p, root, AGG_HASHED, 0,
			 numDistinctCols, dNumDistinctRows,
			 input_plan->startup_cost, input_plan->total_cost,
			 input_plan->plan_rows);

	/*
	 * Result of hashed agg is always unsorted, so if ORDER BY is present we
	 * need to charge for the final sort.
	 */
	if (root->parse->sortClause)
		cost_sort(&hashed_p, root, root->sort_pathkeys, hashed_p.total_cost,
				  dNumDistinctRows, input_plan->plan_width, limit_tuples);

	/*
	 * Now for the GROUP case.	See comments in grouping_planner about the
	 * sorting choices here --- this code should match that code.
	 */
	sorted_p.startup_cost = input_plan->startup_cost;
	sorted_p.total_cost = input_plan->total_cost;
	current_pathkeys = input_pathkeys;
	if (root->parse->hasDistinctOn &&
		list_length(root->distinct_pathkeys) <
		list_length(root->sort_pathkeys))
		needed_pathkeys = root->sort_pathkeys;
	else
		needed_pathkeys = root->distinct_pathkeys;
	if (!pathkeys_contained_in(needed_pathkeys, current_pathkeys))
	{
		if (list_length(root->distinct_pathkeys) >=
			list_length(root->sort_pathkeys))
			current_pathkeys = root->distinct_pathkeys;
		else
			current_pathkeys = root->sort_pathkeys;
		cost_sort(&sorted_p, root, current_pathkeys, sorted_p.total_cost,
				  input_plan->plan_rows, input_plan->plan_width, -1.0);
	}
	cost_group(&sorted_p, root, numDistinctCols, dNumDistinctRows,
			   sorted_p.startup_cost, sorted_p.total_cost,
			   input_plan->plan_rows);
	if (root->parse->sortClause &&
		!pathkeys_contained_in(root->sort_pathkeys, current_pathkeys))
		cost_sort(&sorted_p, root, root->sort_pathkeys, sorted_p.total_cost,
				  dNumDistinctRows, input_plan->plan_width, limit_tuples);

	/*
	 * Now make the decision using the top-level tuple fraction.  First we
	 * have to convert an absolute count (LIMIT) into fractional form.
	 */
	if (tuple_fraction >= 1.0)
		tuple_fraction /= dNumDistinctRows;

	if (compare_fractional_path_costs(&hashed_p, &sorted_p,
									  tuple_fraction) < 0)
	{
		/* Hashed is cheaper, so use it */
		return true;
	}
	return false;
}

/*---------------
 * make_subplanTargetList
 *	  Generate appropriate target list when grouping is required.
 *
 * When grouping_planner inserts Aggregate, Group, or Result plan nodes
 * above the result of query_planner, we typically want to pass a different
 * target list to query_planner than the outer plan nodes should have.
 * This routine generates the correct target list for the subplan.
 *
 * The initial target list passed from the parser already contains entries
 * for all ORDER BY and GROUP BY expressions, but it will not have entries
 * for variables used only in HAVING clauses; so we need to add those
 * variables to the subplan target list.  Also, we flatten all expressions
 * except GROUP BY items into their component variables; the other expressions
 * will be computed by the inserted nodes rather than by the subplan.
 * For example, given a query like
 *		SELECT a+b,SUM(c+d) FROM table GROUP BY a+b;
 * we want to pass this targetlist to the subplan:
 *		a,b,c,d,a+b
 * where the a+b target will be used by the Sort/Group steps, and the
 * other targets will be used for computing the final results.	(In the
 * above example we could theoretically suppress the a and b targets and
 * pass down only c,d,a+b, but it's not really worth the trouble to
 * eliminate simple var references from the subplan.  We will avoid doing
 * the extra computation to recompute a+b at the outer level; see
 * fix_upper_expr() in setrefs.c.)
 *
 * If we are grouping or aggregating, *and* there are no non-Var grouping
 * expressions, then the returned tlist is effectively dummy; we do not
 * need to force it to be evaluated, because all the Vars it contains
 * should be present in the output of query_planner anyway.
 *
 * 'tlist' is the query's target list.
 * 'groupColIdx' receives an array of column numbers for the GROUP BY
 *			expressions (if there are any) in the subplan's target list.
 * 'need_tlist_eval' is set true if we really need to evaluate the
 *			result tlist.
 *
 * The result is the targetlist to be passed to the subplan.
 *---------------
 */
static List *
make_subplanTargetList(PlannerInfo *root,
					   List *tlist,
					   AttrNumber **groupColIdx,
					   bool *need_tlist_eval)
{
	Query	   *parse = root->parse;
	List	   *sub_tlist;
	List	   *extravars;
	int			numCols;

	*groupColIdx = NULL;

	/*
	 * If we're not grouping or aggregating, there's nothing to do here;
	 * query_planner should receive the unmodified target list.
	 */
	if (!parse->hasAggs && !parse->groupClause && !root->hasHavingQual &&
		!parse->hasWindowFuncs)
	{
		*need_tlist_eval = true;
		return tlist;
	}

	/*
	 * Otherwise, start with a "flattened" tlist (having just the vars
	 * mentioned in the targetlist and HAVING qual --- but not upper-level
	 * Vars; they will be replaced by Params later on).  Note this includes
	 * vars used in resjunk items, so we are covering the needs of ORDER BY
	 * and window specifications.
	 */
	sub_tlist = flatten_tlist(tlist);
	extravars = pull_var_clause(parse->havingQual, PVC_INCLUDE_PLACEHOLDERS);
	sub_tlist = add_to_flat_tlist(sub_tlist, extravars);
	list_free(extravars);
	*need_tlist_eval = false;	/* only eval if not flat tlist */

	/*
	 * If grouping, create sub_tlist entries for all GROUP BY expressions
	 * (GROUP BY items that are simple Vars should be in the list already),
	 * and make an array showing where the group columns are in the sub_tlist.
	 */
	numCols = list_length(parse->groupClause);
	if (numCols > 0)
	{
		int			keyno = 0;
		AttrNumber *grpColIdx;
		ListCell   *gl;

		grpColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);
		*groupColIdx = grpColIdx;

		foreach(gl, parse->groupClause)
		{
			SortGroupClause *grpcl = (SortGroupClause *) lfirst(gl);
			Node	   *groupexpr = get_sortgroupclause_expr(grpcl, tlist);
			TargetEntry *te;

			/*
			 * Find or make a matching sub_tlist entry.  If the groupexpr
			 * isn't a Var, no point in searching.  (Note that the parser
			 * won't make multiple groupClause entries for the same TLE.)
			 */
			if (groupexpr && IsA(groupexpr, Var))
				te = tlist_member(groupexpr, sub_tlist);
			else
				te = NULL;

			if (!te)
			{
				te = makeTargetEntry((Expr *) groupexpr,
									 list_length(sub_tlist) + 1,
									 NULL,
									 false);
				sub_tlist = lappend(sub_tlist, te);
				*need_tlist_eval = true;		/* it's not flat anymore */
			}

			/* and save its resno */
			grpColIdx[keyno++] = te->resno;
		}
	}

	return sub_tlist;
}

/*
 * locate_grouping_columns
 *		Locate grouping columns in the tlist chosen by query_planner.
 *
 * This is only needed if we don't use the sub_tlist chosen by
 * make_subplanTargetList.	We have to forget the column indexes found
 * by that routine and re-locate the grouping exprs in the real sub_tlist.
 */
static void
locate_grouping_columns(PlannerInfo *root,
						List *tlist,
						List *sub_tlist,
						AttrNumber *groupColIdx)
{
	int			keyno = 0;
	ListCell   *gl;

	/*
	 * No work unless grouping.
	 */
	if (!root->parse->groupClause)
	{
		Assert(groupColIdx == NULL);
		return;
	}
	Assert(groupColIdx != NULL);

	foreach(gl, root->parse->groupClause)
	{
		SortGroupClause *grpcl = (SortGroupClause *) lfirst(gl);
		Node	   *groupexpr = get_sortgroupclause_expr(grpcl, tlist);
		TargetEntry *te = tlist_member(groupexpr, sub_tlist);

		if (!te)
			elog(ERROR, "failed to locate grouping columns");
		groupColIdx[keyno++] = te->resno;
	}
}

/*
 * postprocess_setop_tlist
 *	  Fix up targetlist returned by plan_set_operations().
 *
 * We need to transpose sort key info from the orig_tlist into new_tlist.
 * NOTE: this would not be good enough if we supported resjunk sort keys
 * for results of set operations --- then, we'd need to project a whole
 * new tlist to evaluate the resjunk columns.  For now, just ereport if we
 * find any resjunk columns in orig_tlist.
 */
static List *
postprocess_setop_tlist(List *new_tlist, List *orig_tlist)
{
	ListCell   *l;
	ListCell   *orig_tlist_item = list_head(orig_tlist);

	foreach(l, new_tlist)
	{
		TargetEntry *new_tle = (TargetEntry *) lfirst(l);
		TargetEntry *orig_tle;

		/* ignore resjunk columns in setop result */
		if (new_tle->resjunk)
			continue;

		Assert(orig_tlist_item != NULL);
		orig_tle = (TargetEntry *) lfirst(orig_tlist_item);
		orig_tlist_item = lnext(orig_tlist_item);
		if (orig_tle->resjunk)	/* should not happen */
			elog(ERROR, "resjunk output columns are not implemented");
		Assert(new_tle->resno == orig_tle->resno);
		new_tle->ressortgroupref = orig_tle->ressortgroupref;
	}
	if (orig_tlist_item != NULL)
		elog(ERROR, "resjunk output columns are not implemented");
	return new_tlist;
}

/*
 * select_active_windows
 *		Create a list of the "active" window clauses (ie, those referenced
 *		by non-deleted WindowFuncs) in the order they are to be executed.
 */
static List *
select_active_windows(PlannerInfo *root, WindowFuncLists *wflists)
{
	List	   *result;
	List	   *actives;
	ListCell   *lc;

	/* First, make a list of the active windows */
	actives = NIL;
	foreach(lc, root->parse->windowClause)
	{
		WindowClause *wc = (WindowClause *) lfirst(lc);

		/* It's only active if wflists shows some related WindowFuncs */
		Assert(wc->winref <= wflists->maxWinRef);
		if (wflists->windowFuncs[wc->winref] != NIL)
			actives = lappend(actives, wc);
	}

	/*
	 * Now, ensure that windows with identical partitioning/ordering clauses
	 * are adjacent in the list.  This is required by the SQL standard, which
	 * says that only one sort is to be used for such windows, even if they
	 * are otherwise distinct (eg, different names or framing clauses).
	 *
	 * There is room to be much smarter here, for example detecting whether
	 * one window's sort keys are a prefix of another's (so that sorting for
	 * the latter would do for the former), or putting windows first that
	 * match a sort order available for the underlying query.  For the moment
	 * we are content with meeting the spec.
	 */
	result = NIL;
	while (actives != NIL)
	{
		WindowClause *wc = (WindowClause *) linitial(actives);
		ListCell   *prev;
		ListCell   *next;

		/* Move wc from actives to result */
		actives = list_delete_first(actives);
		result = lappend(result, wc);

		/* Now move any matching windows from actives to result */
		prev = NULL;
		for (lc = list_head(actives); lc; lc = next)
		{
			WindowClause *wc2 = (WindowClause *) lfirst(lc);

			next = lnext(lc);
			/* framing options are NOT to be compared here! */
			if (equal(wc->partitionClause, wc2->partitionClause) &&
				equal(wc->orderClause, wc2->orderClause))
			{
				actives = list_delete_cell(actives, lc, prev);
				result = lappend(result, wc2);
			}
			else
				prev = lc;
		}
	}

	return result;
}

/*
 * add_volatile_sort_exprs
 *		Identify any volatile sort/group expressions used by the active
 *		windows, and add them to window_tlist if not already present.
 *		Return the modified window_tlist.
 */
static List *
add_volatile_sort_exprs(List *window_tlist, List *tlist, List *activeWindows)
{
	Bitmapset  *sgrefs = NULL;
	ListCell   *lc;

	/* First, collect the sortgrouprefs of the windows into a bitmapset */
	foreach(lc, activeWindows)
	{
		WindowClause *wc = (WindowClause *) lfirst(lc);
		ListCell   *lc2;

		foreach(lc2, wc->partitionClause)
		{
			SortGroupClause *sortcl = (SortGroupClause *) lfirst(lc2);

			sgrefs = bms_add_member(sgrefs, sortcl->tleSortGroupRef);
		}
		foreach(lc2, wc->orderClause)
		{
			SortGroupClause *sortcl = (SortGroupClause *) lfirst(lc2);

			sgrefs = bms_add_member(sgrefs, sortcl->tleSortGroupRef);
		}
	}

	/*
	 * Now scan the original tlist to find the referenced expressions. Any
	 * that are volatile must be added to window_tlist.
	 *
	 * Note: we know that the input window_tlist contains no items marked with
	 * ressortgrouprefs, so we don't have to worry about collisions of the
	 * reference numbers.
	 */
	foreach(lc, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		if (tle->ressortgroupref != 0 &&
			bms_is_member(tle->ressortgroupref, sgrefs) &&
			contain_volatile_functions((Node *) tle->expr))
		{
			TargetEntry *newtle;

			newtle = makeTargetEntry(tle->expr,
									 list_length(window_tlist) + 1,
									 NULL,
									 false);
			newtle->ressortgroupref = tle->ressortgroupref;
			window_tlist = lappend(window_tlist, newtle);
		}
	}

	return window_tlist;
}

/*
 * make_pathkeys_for_window
 *		Create a pathkeys list describing the required input ordering
 *		for the given WindowClause.
 *
 * The required ordering is first the PARTITION keys, then the ORDER keys.
 * In the future we might try to implement windowing using hashing, in which
 * case the ordering could be relaxed, but for now we always sort.
 */
static List *
make_pathkeys_for_window(PlannerInfo *root, WindowClause *wc,
						 List *tlist, bool canonicalize)
{
	List	   *window_pathkeys;
	List	   *window_sortclauses;

	/* Throw error if can't sort */
	if (!grouping_is_sortable(wc->partitionClause))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not implement window PARTITION BY"),
				 errdetail("Window partitioning columns must be of sortable datatypes.")));
	if (!grouping_is_sortable(wc->orderClause))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not implement window ORDER BY"),
		errdetail("Window ordering columns must be of sortable datatypes.")));

	/* Okay, make the combined pathkeys */
	window_sortclauses = list_concat(list_copy(wc->partitionClause),
									 list_copy(wc->orderClause));
	window_pathkeys = make_pathkeys_for_sortclauses(root,
													window_sortclauses,
													tlist,
													canonicalize);
	list_free(window_sortclauses);
	return window_pathkeys;
}

/*----------
 * get_column_info_for_window
 *		Get the partitioning/ordering column numbers and equality operators
 *		for a WindowAgg node.
 *
 * This depends on the behavior of make_pathkeys_for_window()!
 *
 * We are given the target WindowClause and an array of the input column
 * numbers associated with the resulting pathkeys.	In the easy case, there
 * are the same number of pathkey columns as partitioning + ordering columns
 * and we just have to copy some data around.  However, it's possible that
 * some of the original partitioning + ordering columns were eliminated as
 * redundant during the transformation to pathkeys.  (This can happen even
 * though the parser gets rid of obvious duplicates.  A typical scenario is a
 * window specification "PARTITION BY x ORDER BY y" coupled with a clause
 * "WHERE x = y" that causes the two sort columns to be recognized as
 * redundant.)	In that unusual case, we have to work a lot harder to
 * determine which keys are significant.
 *
 * The method used here is a bit brute-force: add the sort columns to a list
 * one at a time and note when the resulting pathkey list gets longer.	But
 * it's a sufficiently uncommon case that a faster way doesn't seem worth
 * the amount of code refactoring that'd be needed.
 *----------
 */
static void
get_column_info_for_window(PlannerInfo *root, WindowClause *wc, List *tlist,
						   int numSortCols, AttrNumber *sortColIdx,
						   int *partNumCols,
						   AttrNumber **partColIdx,
						   Oid **partOperators,
						   int *ordNumCols,
						   AttrNumber **ordColIdx,
						   Oid **ordOperators)
{
	int			numPart = list_length(wc->partitionClause);
	int			numOrder = list_length(wc->orderClause);

	if (numSortCols == numPart + numOrder)
	{
		/* easy case */
		*partNumCols = numPart;
		*partColIdx = sortColIdx;
		*partOperators = extract_grouping_ops(wc->partitionClause);
		*ordNumCols = numOrder;
		*ordColIdx = sortColIdx + numPart;
		*ordOperators = extract_grouping_ops(wc->orderClause);
	}
	else
	{
		List	   *sortclauses;
		List	   *pathkeys;
		int			scidx;
		ListCell   *lc;

		/* first, allocate what's certainly enough space for the arrays */
		*partNumCols = 0;
		*partColIdx = (AttrNumber *) palloc(numPart * sizeof(AttrNumber));
		*partOperators = (Oid *) palloc(numPart * sizeof(Oid));
		*ordNumCols = 0;
		*ordColIdx = (AttrNumber *) palloc(numOrder * sizeof(AttrNumber));
		*ordOperators = (Oid *) palloc(numOrder * sizeof(Oid));
		sortclauses = NIL;
		pathkeys = NIL;
		scidx = 0;
		foreach(lc, wc->partitionClause)
		{
			SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
			List	   *new_pathkeys;

			sortclauses = lappend(sortclauses, sgc);
			new_pathkeys = make_pathkeys_for_sortclauses(root,
														 sortclauses,
														 tlist,
														 true);
			if (list_length(new_pathkeys) > list_length(pathkeys))
			{
				/* this sort clause is actually significant */
				(*partColIdx)[*partNumCols] = sortColIdx[scidx++];
				(*partOperators)[*partNumCols] = sgc->eqop;
				(*partNumCols)++;
				pathkeys = new_pathkeys;
			}
		}
		foreach(lc, wc->orderClause)
		{
			SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
			List	   *new_pathkeys;

			sortclauses = lappend(sortclauses, sgc);
			new_pathkeys = make_pathkeys_for_sortclauses(root,
														 sortclauses,
														 tlist,
														 true);
			if (list_length(new_pathkeys) > list_length(pathkeys))
			{
				/* this sort clause is actually significant */
				(*ordColIdx)[*ordNumCols] = sortColIdx[scidx++];
				(*ordOperators)[*ordNumCols] = sgc->eqop;
				(*ordNumCols)++;
				pathkeys = new_pathkeys;
			}
		}
		/* complain if we didn't eat exactly the right number of sort cols */
		if (scidx != numSortCols)
			elog(ERROR, "failed to deconstruct sort operators into partitioning/ordering operators");
	}
}


/*
 * expression_planner
 *		Perform planner's transformations on a standalone expression.
 *
 * Various utility commands need to evaluate expressions that are not part
 * of a plannable query.  They can do so using the executor's regular
 * expression-execution machinery, but first the expression has to be fed
 * through here to transform it from parser output to something executable.
 *
 * Currently, we disallow sublinks in standalone expressions, so there's no
 * real "planning" involved here.  (That might not always be true though.)
 * What we must do is run eval_const_expressions to ensure that any function
 * default arguments get inserted.	The fact that constant subexpressions
 * get simplified is a side-effect that is useful when the expression will
 * get evaluated more than once.  Also, we must fix operator function IDs.
 *
 * Note: this must not make any damaging changes to the passed-in expression
 * tree.  (It would actually be okay to apply fix_opfuncids to it, but since
 * we first do an expression_tree_mutator-based walk, what is returned will
 * be a new node tree.)
 */
Expr *
expression_planner(Expr *expr)
{
	Node	   *result;

	/* Insert default arguments and simplify constant subexprs */
	result = eval_const_expressions(NULL, (Node *) expr);

	/* Fill in opfuncid values if missing */
	fix_opfuncids(result);

	return (Expr *) result;
}
