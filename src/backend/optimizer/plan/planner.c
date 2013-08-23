/*-------------------------------------------------------------------------
 *
 * planner.c
 *	  The query optimizer external interface.
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/planner.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "access/htup_details.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#ifdef OPTIMIZER_DEBUG
#include "nodes/print.h"
#endif
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "parser/analyze.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/rel.h"


/* GUC parameter */
double		cursor_tuple_fraction = DEFAULT_CURSOR_TUPLE_FRACTION;

/* Hook for plugins to get control in planner() */
planner_hook_type planner_hook = NULL;


/* Expression kind codes for preprocess_expression */
#define EXPRKIND_QUAL			0
#define EXPRKIND_TARGET			1
#define EXPRKIND_RTFUNC			2
#define EXPRKIND_RTFUNC_LATERAL 3
#define EXPRKIND_VALUES			4
#define EXPRKIND_VALUES_LATERAL 5
#define EXPRKIND_LIMIT			6
#define EXPRKIND_APPINFO		7
#define EXPRKIND_PHV			8

/* Passthrough data for standard_qp_callback */
typedef struct
{
	List	   *tlist;			/* preprocessed query targetlist */
	List	   *activeWindows;	/* active windows, if any */
} standard_qp_extra;

/* Local functions */
static Node *preprocess_expression(PlannerInfo *root, Node *expr, int kind);
static void preprocess_qual_conditions(PlannerInfo *root, Node *jtnode);
static Plan *inheritance_planner(PlannerInfo *root);
static Plan *grouping_planner(PlannerInfo *root, double tuple_fraction);
static void preprocess_rowmarks(PlannerInfo *root);
static double preprocess_limit(PlannerInfo *root,
				 double tuple_fraction,
				 int64 *offset_est, int64 *count_est);
static bool limit_needed(Query *parse);
static void preprocess_groupclause(PlannerInfo *root);
static void standard_qp_callback(PlannerInfo *root, void *extra);
static bool choose_hashed_grouping(PlannerInfo *root,
					   double tuple_fraction, double limit_tuples,
					   double path_rows, int path_width,
					   Path *cheapest_path, Path *sorted_path,
					   double dNumGroups, AggClauseCosts *agg_costs);
static bool choose_hashed_distinct(PlannerInfo *root,
					   double tuple_fraction, double limit_tuples,
					   double path_rows, int path_width,
					   Cost cheapest_startup_cost, Cost cheapest_total_cost,
					   Cost sorted_startup_cost, Cost sorted_total_cost,
					   List *sorted_pathkeys,
					   double dNumDistinctRows);
static List *make_subplanTargetList(PlannerInfo *root, List *tlist,
					   AttrNumber **groupColIdx, bool *need_tlist_eval);
static int	get_grouping_column_index(Query *parse, TargetEntry *tle);
static void locate_grouping_columns(PlannerInfo *root,
						List *tlist,
						List *sub_tlist,
						AttrNumber *groupColIdx);
static List *postprocess_setop_tlist(List *new_tlist, List *orig_tlist);
static List *select_active_windows(PlannerInfo *root, WindowFuncLists *wflists);
static List *make_windowInputTargetList(PlannerInfo *root,
						   List *tlist, List *activeWindows);
static List *make_pathkeys_for_window(PlannerInfo *root, WindowClause *wc,
						 List *tlist);
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
	glob->subplans = NIL;
	glob->subroots = NIL;
	glob->rewindPlanIDs = NULL;
	glob->finalrtable = NIL;
	glob->finalrowmarks = NIL;
	glob->resultRelations = NIL;
	glob->relationOids = NIL;
	glob->invalItems = NIL;
	glob->nParamExec = 0;
	glob->lastPHId = 0;
	glob->lastRowMarkId = 0;
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
	Assert(glob->finalrowmarks == NIL);
	Assert(glob->resultRelations == NIL);
	top_plan = set_plan_references(root, top_plan);
	/* ... and the subplans (both regular subplans and initplans) */
	Assert(list_length(glob->subplans) == list_length(glob->subroots));
	forboth(lp, glob->subplans, lr, glob->subroots)
	{
		Plan	   *subplan = (Plan *) lfirst(lp);
		PlannerInfo *subroot = (PlannerInfo *) lfirst(lr);

		lfirst(lp) = set_plan_references(subroot, subplan);
	}

	/* build the PlannedStmt result */
	result = makeNode(PlannedStmt);

	result->commandType = parse->commandType;
	result->queryId = parse->queryId;
	result->hasReturning = (parse->returningList != NIL);
	result->hasModifyingCTE = parse->hasModifyingCTE;
	result->canSetTag = parse->canSetTag;
	result->transientPlan = glob->transientPlan;
	result->planTree = top_plan;
	result->rtable = glob->finalrtable;
	result->resultRelations = glob->resultRelations;
	result->utilityStmt = parse->utilityStmt;
	result->subplans = glob->subplans;
	result->rewindPlanIDs = glob->rewindPlanIDs;
	result->rowMarks = glob->finalrowmarks;
	result->relationOids = glob->relationOids;
	result->invalItems = glob->invalItems;
	result->nParamExec = glob->nParamExec;

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
	root->plan_params = NIL;
	root->planner_cxt = CurrentMemoryContext;
	root->init_plans = NIL;
	root->cte_plan_ids = NIL;
	root->eq_classes = NIL;
	root->append_rel_list = NIL;
	root->rowMarks = NIL;
	root->hasInheritedTarget = false;

	root->hasRecursion = hasRecursion;
	if (hasRecursion)
		root->wt_param_id = SS_assign_special_param(root);
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
	 * Check to see if any subqueries in the jointree can be merged into this
	 * query.
	 */
	parse->jointree = (FromExpr *)
		pull_up_subqueries(root, (Node *) parse->jointree);

	/*
	 * If this is a simple UNION ALL query, flatten it into an appendrel. We
	 * do this now because it requires applying pull_up_subqueries to the leaf
	 * queries of the UNION ALL, which weren't touched above because they
	 * weren't referenced by the jointree (they will be after we do this).
	 */
	if (parse->setOperations)
		flatten_simple_union_all(root);

	/*
	 * Detect whether any rangetable entries are RTE_JOIN kind; if not, we can
	 * avoid the expense of doing flatten_join_alias_vars().  Also check for
	 * outer joins --- if none, we can skip reduce_outer_joins().  And check
	 * for LATERAL RTEs, too.  This must be done after we have done
	 * pull_up_subqueries(), of course.
	 */
	root->hasJoinRTEs = false;
	root->hasLateralRTEs = false;
	hasOuterJoins = false;
	foreach(l, parse->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);

		if (rte->rtekind == RTE_JOIN)
		{
			root->hasJoinRTEs = true;
			if (IS_OUTER_JOIN(rte->jointype))
				hasOuterJoins = true;
		}
		if (rte->lateral)
			root->hasLateralRTEs = true;
	}

	/*
	 * Preprocess RowMark information.	We need to do this after subquery
	 * pullup (so that all non-inherited RTEs are present) and before
	 * inheritance expansion (so that the info is available for
	 * expand_inherited_tables to examine and modify).
	 */
	preprocess_rowmarks(root);

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
	 * Do expression preprocessing on targetlist and quals, as well as other
	 * random expressions in the querytree.  Note that we do not need to
	 * handle sort/group expressions explicitly, because they are actually
	 * part of the targetlist.
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

	foreach(l, parse->windowClause)
	{
		WindowClause *wc = (WindowClause *) lfirst(l);

		/* partitionClause/orderClause are sort/group expressions */
		wc->startOffset = preprocess_expression(root, wc->startOffset,
												EXPRKIND_LIMIT);
		wc->endOffset = preprocess_expression(root, wc->endOffset,
											  EXPRKIND_LIMIT);
	}

	parse->limitOffset = preprocess_expression(root, parse->limitOffset,
											   EXPRKIND_LIMIT);
	parse->limitCount = preprocess_expression(root, parse->limitCount,
											  EXPRKIND_LIMIT);

	root->append_rel_list = (List *)
		preprocess_expression(root, (Node *) root->append_rel_list,
							  EXPRKIND_APPINFO);

	/* Also need to preprocess expressions within RTEs */
	foreach(l, parse->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);
		int			kind;

		if (rte->rtekind == RTE_SUBQUERY)
		{
			/*
			 * We don't want to do all preprocessing yet on the subquery's
			 * expressions, since that will happen when we plan it.  But if it
			 * contains any join aliases of our level, those have to get
			 * expanded now, because planning of the subquery won't do it.
			 * That's only possible if the subquery is LATERAL.
			 */
			if (rte->lateral && root->hasJoinRTEs)
				rte->subquery = (Query *)
					flatten_join_alias_vars(root, (Node *) rte->subquery);
		}
		else if (rte->rtekind == RTE_FUNCTION)
		{
			/* Preprocess the function expression fully */
			kind = rte->lateral ? EXPRKIND_RTFUNC_LATERAL : EXPRKIND_RTFUNC;
			rte->funcexpr = preprocess_expression(root, rte->funcexpr, kind);
		}
		else if (rte->rtekind == RTE_VALUES)
		{
			/* Preprocess the values lists fully */
			kind = rte->lateral ? EXPRKIND_VALUES_LATERAL : EXPRKIND_VALUES;
			rte->values_lists = (List *)
				preprocess_expression(root, (Node *) rte->values_lists, kind);
		}
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
	{
		plan = grouping_planner(root, tuple_fraction);
		/* If it's not SELECT, we need a ModifyTable node */
		if (parse->commandType != CMD_SELECT)
		{
			List	   *returningLists;
			List	   *rowMarks;

			/*
			 * Set up the RETURNING list-of-lists, if needed.
			 */
			if (parse->returningList)
				returningLists = list_make1(parse->returningList);
			else
				returningLists = NIL;

			/*
			 * If there was a FOR [KEY] UPDATE/SHARE clause, the LockRows node
			 * will have dealt with fetching non-locked marked rows, else we
			 * need to have ModifyTable do that.
			 */
			if (parse->rowMarks)
				rowMarks = NIL;
			else
				rowMarks = root->rowMarks;

			plan = (Plan *) make_modifytable(root,
											 parse->commandType,
											 parse->canSetTag,
									   list_make1_int(parse->resultRelation),
											 list_make1(plan),
											 returningLists,
											 rowMarks,
											 SS_assign_special_param(root));
		}
	}

	/*
	 * If any subplans were generated, or if there are any parameters to worry
	 * about, build initPlan list and extParam/allParam sets for plan nodes,
	 * and attach the initPlans to the top plan node.
	 */
	if (list_length(glob->subplans) != num_old_subplans ||
		root->glob->nParamExec > 0)
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
 *		conditions), a HAVING clause, or a few other things.
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
	 * base-relation variables.  We must do this before sublink processing,
	 * else sublinks expanded out from join aliases would not get processed.
	 * We can skip it in non-lateral RTE functions and VALUES lists, however,
	 * since they can't contain any Vars of the current query level.
	 */
	if (root->hasJoinRTEs &&
		!(kind == EXPRKIND_RTFUNC || kind == EXPRKIND_VALUES))
		expr = flatten_join_alias_vars(root, expr);

	/*
	 * Simplify constant expressions.
	 *
	 * Note: an essential effect of this is to convert named-argument function
	 * calls to positional notation and insert the current actual values of
	 * any default arguments for functions.  To ensure that happens, we *must*
	 * process all expressions here.  Previous PG versions sometimes skipped
	 * const-simplification if it didn't seem worth the trouble, but we can't
	 * do that anymore.
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
 * preprocess_phv_expression
 *	  Do preprocessing on a PlaceHolderVar expression that's been pulled up.
 *
 * If a LATERAL subquery references an output of another subquery, and that
 * output must be wrapped in a PlaceHolderVar because of an intermediate outer
 * join, then we'll push the PlaceHolderVar expression down into the subquery
 * and later pull it back up during find_lateral_references, which runs after
 * subquery_planner has preprocessed all the expressions that were in the
 * current query level to start with.  So we need to preprocess it then.
 */
Expr *
preprocess_phv_expression(PlannerInfo *root, Expr *expr)
{
	return (Expr *) preprocess_expression(root, (Node *) expr, EXPRKIND_PHV);
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
 * different targetlist matching its own column set.  Fortunately,
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
	List	   *final_rtable = NIL;
	int			save_rel_array_size = 0;
	RelOptInfo **save_rel_array = NULL;
	List	   *subplans = NIL;
	List	   *resultRelations = NIL;
	List	   *returningLists = NIL;
	List	   *rowMarks;
	ListCell   *lc;

	/*
	 * We generate a modified instance of the original Query for each target
	 * relation, plan that, and put all the plans into a list that will be
	 * controlled by a single ModifyTable node.  All the instances share the
	 * same rangetable, but each instance must have its own set of subquery
	 * RTEs within the finished rangetable because (1) they are likely to get
	 * scribbled on during planning, and (2) it's not inconceivable that
	 * subqueries could get planned differently in different cases.  We need
	 * not create duplicate copies of other RTE kinds, in particular not the
	 * target relations, because they don't have either of those issues.  Not
	 * having to duplicate the target relations is important because doing so
	 * (1) would result in a rangetable of length O(N^2) for N targets, with
	 * at least O(N^3) work expended here; and (2) would greatly complicate
	 * management of the rowMarks list.
	 */
	foreach(lc, root->append_rel_list)
	{
		AppendRelInfo *appinfo = (AppendRelInfo *) lfirst(lc);
		PlannerInfo subroot;
		Plan	   *subplan;
		Index		rti;

		/* append_rel_list contains all append rels; ignore others */
		if (appinfo->parent_relid != parentRTindex)
			continue;

		/*
		 * We need a working copy of the PlannerInfo so that we can control
		 * propagation of information back to the main copy.
		 */
		memcpy(&subroot, root, sizeof(PlannerInfo));

		/*
		 * Generate modified query with this rel as target.  We first apply
		 * adjust_appendrel_attrs, which copies the Query and changes
		 * references to the parent RTE to refer to the current child RTE,
		 * then fool around with subquery RTEs.
		 */
		subroot.parse = (Query *)
			adjust_appendrel_attrs(root,
								   (Node *) parse,
								   appinfo);

		/*
		 * The rowMarks list might contain references to subquery RTEs, so
		 * make a copy that we can apply ChangeVarNodes to.  (Fortunately, the
		 * executor doesn't need to see the modified copies --- we can just
		 * pass it the original rowMarks list.)
		 */
		subroot.rowMarks = (List *) copyObject(root->rowMarks);

		/*
		 * Add placeholders to the child Query's rangetable list to fill the
		 * RT indexes already reserved for subqueries in previous children.
		 * These won't be referenced, so there's no need to make them very
		 * valid-looking.
		 */
		while (list_length(subroot.parse->rtable) < list_length(final_rtable))
			subroot.parse->rtable = lappend(subroot.parse->rtable,
											makeNode(RangeTblEntry));

		/*
		 * If this isn't the first child Query, generate duplicates of all
		 * subquery RTEs, and adjust Var numbering to reference the
		 * duplicates. To simplify the loop logic, we scan the original rtable
		 * not the copy just made by adjust_appendrel_attrs; that should be OK
		 * since subquery RTEs couldn't contain any references to the target
		 * rel.
		 */
		if (final_rtable != NIL)
		{
			ListCell   *lr;

			rti = 1;
			foreach(lr, parse->rtable)
			{
				RangeTblEntry *rte = (RangeTblEntry *) lfirst(lr);

				if (rte->rtekind == RTE_SUBQUERY)
				{
					Index		newrti;

					/*
					 * The RTE can't contain any references to its own RT
					 * index, so we can save a few cycles by applying
					 * ChangeVarNodes before we append the RTE to the
					 * rangetable.
					 */
					newrti = list_length(subroot.parse->rtable) + 1;
					ChangeVarNodes((Node *) subroot.parse, rti, newrti, 0);
					ChangeVarNodes((Node *) subroot.rowMarks, rti, newrti, 0);
					rte = copyObject(rte);
					subroot.parse->rtable = lappend(subroot.parse->rtable,
													rte);
				}
				rti++;
			}
		}

		/* We needn't modify the child's append_rel_list */
		/* There shouldn't be any OJ or LATERAL info to translate, as yet */
		Assert(subroot.join_info_list == NIL);
		Assert(subroot.lateral_info_list == NIL);
		/* and we haven't created PlaceHolderInfos, either */
		Assert(subroot.placeholder_list == NIL);
		/* hack to mark target relation as an inheritance partition */
		subroot.hasInheritedTarget = true;

		/* Generate plan */
		subplan = grouping_planner(&subroot, 0.0 /* retrieve all tuples */ );

		/*
		 * If this child rel was excluded by constraint exclusion, exclude it
		 * from the result plan.
		 */
		if (is_dummy_plan(subplan))
			continue;

		subplans = lappend(subplans, subplan);

		/*
		 * If this is the first non-excluded child, its post-planning rtable
		 * becomes the initial contents of final_rtable; otherwise, append
		 * just its modified subquery RTEs to final_rtable.
		 */
		if (final_rtable == NIL)
			final_rtable = subroot.parse->rtable;
		else
			final_rtable = list_concat(final_rtable,
									   list_copy_tail(subroot.parse->rtable,
												 list_length(final_rtable)));

		/*
		 * We need to collect all the RelOptInfos from all child plans into
		 * the main PlannerInfo, since setrefs.c will need them.  We use the
		 * last child's simple_rel_array (previous ones are too short), so we
		 * have to propagate forward the RelOptInfos that were already built
		 * in previous children.
		 */
		Assert(subroot.simple_rel_array_size >= save_rel_array_size);
		for (rti = 1; rti < save_rel_array_size; rti++)
		{
			RelOptInfo *brel = save_rel_array[rti];

			if (brel)
				subroot.simple_rel_array[rti] = brel;
		}
		save_rel_array_size = subroot.simple_rel_array_size;
		save_rel_array = subroot.simple_rel_array;

		/* Make sure any initplans from this rel get into the outer list */
		root->init_plans = subroot.init_plans;

		/* Build list of target-relation RT indexes */
		resultRelations = lappend_int(resultRelations, appinfo->child_relid);

		/* Build list of per-relation RETURNING targetlists */
		if (parse->returningList)
			returningLists = lappend(returningLists,
									 subroot.parse->returningList);
	}

	/* Mark result as unordered (probably unnecessary) */
	root->query_pathkeys = NIL;

	/*
	 * If we managed to exclude every child rel, return a dummy plan; it
	 * doesn't even need a ModifyTable node.
	 */
	if (subplans == NIL)
	{
		/* although dummy, it must have a valid tlist for executor */
		List	   *tlist;

		tlist = preprocess_targetlist(root, parse->targetList);
		return (Plan *) make_result(root,
									tlist,
									(Node *) list_make1(makeBoolConst(false,
																	  false)),
									NULL);
	}

	/*
	 * Put back the final adjusted rtable into the master copy of the Query.
	 */
	parse->rtable = final_rtable;
	root->simple_rel_array_size = save_rel_array_size;
	root->simple_rel_array = save_rel_array;

	/*
	 * If there was a FOR [KEY] UPDATE/SHARE clause, the LockRows node will
	 * have dealt with fetching non-locked marked rows, else we need to have
	 * ModifyTable do that.
	 */
	if (parse->rowMarks)
		rowMarks = NIL;
	else
		rowMarks = root->rowMarks;

	/* And last, tack on a ModifyTable node to do the UPDATE/DELETE work */
	return (Plan *) make_modifytable(root,
									 parse->commandType,
									 parse->canSetTag,
									 resultRelations,
									 subplans,
									 returningLists,
									 rowMarks,
									 SS_assign_special_param(root));
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
	bool		use_hashed_distinct = false;
	bool		tested_hashed_distinct = false;

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
													result_plan->targetlist);

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
		 * Can't handle FOR [KEY] UPDATE/SHARE here (parser should have
		 * checked already, but let's make sure).
		 */
		if (parse->rowMarks)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 /*------
					   translator: %s is a SQL row locking clause such as FOR UPDATE */
					 errmsg("%s is not allowed with UNION/INTERSECT/EXCEPT",
							LCS_asString(((RowMarkClause *)
										  linitial(parse->rowMarks))->strength))));

		/*
		 * Calculate pathkeys that represent result ordering requirements
		 */
		Assert(parse->distinctClause == NIL);
		root->sort_pathkeys = make_pathkeys_for_sortclauses(root,
															parse->sortClause,
															tlist);
	}
	else
	{
		/* No set operations, do regular planning */
		List	   *sub_tlist;
		double		sub_limit_tuples;
		AttrNumber *groupColIdx = NULL;
		bool		need_tlist_eval = true;
		standard_qp_extra qp_extra;
		Path	   *cheapest_path;
		Path	   *sorted_path;
		Path	   *best_path;
		long		numGroups = 0;
		AggClauseCosts agg_costs;
		int			numGroupCols;
		double		path_rows;
		int			path_width;
		bool		use_hashed_grouping = false;
		WindowFuncLists *wflists = NULL;
		List	   *activeWindows = NIL;

		MemSet(&agg_costs, 0, sizeof(AggClauseCosts));

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
		 * Do aggregate preprocessing, if the query has any aggs.
		 *
		 * Note: think not that we can turn off hasAggs if we find no aggs. It
		 * is possible for constant-expression simplification to remove all
		 * explicit references to aggs, but we still have to follow the
		 * aggregate semantics (eg, producing only one output row).
		 */
		if (parse->hasAggs)
		{
			/*
			 * Collect statistics about aggregates for estimating costs. Note:
			 * we do not attempt to detect duplicate aggregates here; a
			 * somewhat-overestimated cost is okay for our present purposes.
			 */
			count_agg_clauses(root, (Node *) tlist, &agg_costs);
			count_agg_clauses(root, parse->havingQual, &agg_costs);

			/*
			 * Preprocess MIN/MAX aggregates, if any.  Note: be careful about
			 * adding logic between here and the optimize_minmax_aggregates
			 * call.  Anything that is needed in MIN/MAX-optimizable cases
			 * will have to be duplicated in planagg.c.
			 */
			preprocess_minmax_aggregates(root, tlist);
		}

		/*
		 * Figure out whether there's a hard limit on the number of rows that
		 * query_planner's result subplan needs to return.  Even if we know a
		 * hard limit overall, it doesn't apply if the query has any
		 * grouping/aggregation operations.
		 */
		if (parse->groupClause ||
			parse->distinctClause ||
			parse->hasAggs ||
			parse->hasWindowFuncs ||
			root->hasHavingQual)
			sub_limit_tuples = -1.0;
		else
			sub_limit_tuples = limit_tuples;

		/* Set up data needed by standard_qp_callback */
		qp_extra.tlist = tlist;
		qp_extra.activeWindows = activeWindows;

		/*
		 * Generate the best unsorted and presorted paths for this Query (but
		 * note there may not be any presorted path).  We also generate (in
		 * standard_qp_callback) pathkey representations of the query's sort
		 * clause, distinct clause, etc.  query_planner will also estimate the
		 * number of groups in the query.
		 */
		query_planner(root, sub_tlist, tuple_fraction, sub_limit_tuples,
					  standard_qp_callback, &qp_extra,
					  &cheapest_path, &sorted_path, &dNumGroups);

		/*
		 * Extract rowcount and width estimates for possible use in grouping
		 * decisions.  Beware here of the possibility that
		 * cheapest_path->parent is NULL (ie, there is no FROM clause).
		 */
		if (cheapest_path->parent)
		{
			path_rows = cheapest_path->parent->rows;
			path_width = cheapest_path->parent->width;
		}
		else
		{
			path_rows = 1;		/* assume non-set result */
			path_width = 100;	/* arbitrary */
		}

		if (parse->groupClause)
		{
			/*
			 * If grouping, decide whether to use sorted or hashed grouping.
			 */
			use_hashed_grouping =
				choose_hashed_grouping(root,
									   tuple_fraction, limit_tuples,
									   path_rows, path_width,
									   cheapest_path, sorted_path,
									   dNumGroups, &agg_costs);
			/* Also convert # groups to long int --- but 'ware overflow! */
			numGroups = (long) Min(dNumGroups, (double) LONG_MAX);
		}
		else if (parse->distinctClause && sorted_path &&
				 !root->hasHavingQual && !parse->hasAggs && !activeWindows)
		{
			/*
			 * We'll reach the DISTINCT stage without any intermediate
			 * processing, so figure out whether we will want to hash or not
			 * so we can choose whether to use cheapest or sorted path.
			 */
			use_hashed_distinct =
				choose_hashed_distinct(root,
									   tuple_fraction, limit_tuples,
									   path_rows, path_width,
									   cheapest_path->startup_cost,
									   cheapest_path->total_cost,
									   sorted_path->startup_cost,
									   sorted_path->total_cost,
									   sorted_path->pathkeys,
									   dNumGroups);
			tested_hashed_distinct = true;
		}

		/*
		 * Select the best path.  If we are doing hashed grouping, we will
		 * always read all the input tuples, so use the cheapest-total path.
		 * Otherwise, trust query_planner's decision about which to use.
		 */
		if (use_hashed_grouping || use_hashed_distinct || !sorted_path)
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
												 &agg_costs,
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
				 * Always override create_plan's tlist, so that we don't sort
				 * useless data from a "physical" tlist.
				 */
				need_tlist_eval = true;
			}

			/*
			 * create_plan returns a plan with just a "flat" tlist of required
			 * Vars.  Usually we need to insert the sub_tlist as the tlist of
			 * the top plan node.  However, we can skip that if we determined
			 * that whatever create_plan chose to return will be good enough.
			 */
			if (need_tlist_eval)
			{
				/*
				 * If the top-level plan node is one that cannot do expression
				 * evaluation and its existing target list isn't already what
				 * we need, we must insert a Result node to project the
				 * desired tlist.
				 */
				if (!is_projection_capable_plan(result_plan) &&
					!tlist_same_exprs(sub_tlist, result_plan->targetlist))
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
				 * See comments for add_tlist_costs_to_plan() for more info.
				 */
				add_tlist_costs_to_plan(root, result_plan, sub_tlist);
			}
			else
			{
				/*
				 * Since we're using create_plan's tlist and not the one
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
												&agg_costs,
												numGroupCols,
												groupColIdx,
									extract_grouping_ops(parse->groupClause),
												numGroups,
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
												&agg_costs,
												numGroupCols,
												groupColIdx,
									extract_grouping_ops(parse->groupClause),
												numGroups,
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
			 * it's not worth trying to avoid it.  In particular, think not to
			 * skip adding the Result if the initial window_tlist matches the
			 * top-level plan node's output, because we might change the tlist
			 * inside the following loop.)	Note that on second and subsequent
			 * passes through the following loop, the top-level node will be a
			 * WindowAgg which we know can project; so we only need to check
			 * once.
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
			 * a flat tlist of all Vars and Aggs needed in the result.	(In
			 * some cases we wouldn't need to propagate all of these all the
			 * way to the top, since they might only be needed as inputs to
			 * WindowFuncs.  It's probably not worth trying to optimize that
			 * though.)  We also add window partitioning and sorting
			 * expressions to the base tlist, to ensure they're computed only
			 * once at the bottom of the stack (that's critical for volatile
			 * functions).	As we climb up the stack, we'll add outputs for
			 * the WindowFuncs computed at each level.
			 */
			window_tlist = make_windowInputTargetList(root,
													  tlist,
													  activeWindows);

			/*
			 * The copyObject steps here are needed to ensure that each plan
			 * node has a separately modifiable tlist.	(XXX wouldn't a
			 * shallow list copy do for that?)
			 */
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
														   tlist);

				/*
				 * This is a bit tricky: we build a sort node even if we don't
				 * really have to sort.  Even when no explicit sort is needed,
				 * we need to have suitable resjunk items added to the input
				 * plan's tlist for any partitioning or ordering columns that
				 * aren't plain Vars.  (In theory, make_windowInputTargetList
				 * should have provided all such columns, but let's not assume
				 * that here.)	Furthermore, this way we can use existing
				 * infrastructure to identify which input columns are the
				 * interesting ones.
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
								   wflists->windowFuncs[wc->winref],
								   wc->winref,
								   partNumCols,
								   partColIdx,
								   partOperators,
								   ordNumCols,
								   ordColIdx,
								   ordOperators,
								   wc->frameOptions,
								   wc->startOffset,
								   wc->endOffset,
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

		/* Choose implementation method if we didn't already */
		if (!tested_hashed_distinct)
		{
			/*
			 * At this point, either hashed or sorted grouping will have to
			 * work from result_plan, so we pass that as both "cheapest" and
			 * "sorted".
			 */
			use_hashed_distinct =
				choose_hashed_distinct(root,
									   tuple_fraction, limit_tuples,
									   result_plan->plan_rows,
									   result_plan->plan_width,
									   result_plan->startup_cost,
									   result_plan->total_cost,
									   result_plan->startup_cost,
									   result_plan->total_cost,
									   current_pathkeys,
									   dNumDistinctRows);
		}

		if (use_hashed_distinct)
		{
			/* Hashed aggregate plan --- no sort needed */
			result_plan = (Plan *) make_agg(root,
											result_plan->targetlist,
											NIL,
											AGG_HASHED,
											NULL,
										  list_length(parse->distinctClause),
								 extract_grouping_cols(parse->distinctClause,
													result_plan->targetlist),
								 extract_grouping_ops(parse->distinctClause),
											numDistinctRows,
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
	 * If there is a FOR [KEY] UPDATE/SHARE clause, add the LockRows node.
	 * (Note: we intentionally test parse->rowMarks not root->rowMarks here.
	 * If there are only non-locking rowmarks, they should be handled by the
	 * ModifyTable node instead.)
	 */
	if (parse->rowMarks)
	{
		result_plan = (Plan *) make_lockrows(result_plan,
											 root->rowMarks,
											 SS_assign_special_param(root));

		/*
		 * The result can no longer be assumed sorted, since locking might
		 * cause the sort key columns to be replaced with new values.
		 */
		current_pathkeys = NIL;
	}

	/*
	 * Finally, if there is a LIMIT/OFFSET clause, add the LIMIT node.
	 */
	if (limit_needed(parse))
	{
		result_plan = (Plan *) make_limit(result_plan,
										  parse->limitOffset,
										  parse->limitCount,
										  offset_est,
										  count_est);
	}

	/*
	 * Return the actual output ordering in query_pathkeys for possible use by
	 * an outer query level.
	 */
	root->query_pathkeys = current_pathkeys;

	return result_plan;
}

/*
 * add_tlist_costs_to_plan
 *
 * Estimate the execution costs associated with evaluating the targetlist
 * expressions, and add them to the cost estimates for the Plan node.
 *
 * If the tlist contains set-returning functions, also inflate the Plan's cost
 * and plan_rows estimates accordingly.  (Hence, this must be called *after*
 * any logic that uses plan_rows to, eg, estimate qual evaluation costs.)
 *
 * Note: during initial stages of planning, we mostly consider plan nodes with
 * "flat" tlists, containing just Vars.  So their evaluation cost is zero
 * according to the model used by cost_qual_eval() (or if you prefer, the cost
 * is factored into cpu_tuple_cost).  Thus we can avoid accounting for tlist
 * cost throughout query_planner() and subroutines.  But once we apply a
 * tlist that might contain actual operators, sub-selects, etc, we'd better
 * account for its cost.  Any set-returning functions in the tlist must also
 * affect the estimated rowcount.
 *
 * Once grouping_planner() has applied a general tlist to the topmost
 * scan/join plan node, any tlist eval cost for added-on nodes should be
 * accounted for as we create those nodes.	Presently, of the node types we
 * can add on later, only Agg, WindowAgg, and Group project new tlists (the
 * rest just copy their input tuples) --- so make_agg(), make_windowagg() and
 * make_group() are responsible for calling this function to account for their
 * tlist costs.
 */
void
add_tlist_costs_to_plan(PlannerInfo *root, Plan *plan, List *tlist)
{
	QualCost	tlist_cost;
	double		tlist_rows;

	cost_qual_eval(&tlist_cost, tlist, root);
	plan->startup_cost += tlist_cost.startup;
	plan->total_cost += tlist_cost.startup +
		tlist_cost.per_tuple * plan->plan_rows;

	tlist_rows = tlist_returns_set_rows(tlist);
	if (tlist_rows > 1)
	{
		/*
		 * We assume that execution costs of the tlist proper were all
		 * accounted for by cost_qual_eval.  However, it still seems
		 * appropriate to charge something more for the executor's general
		 * costs of processing the added tuples.  The cost is probably less
		 * than cpu_tuple_cost, though, so we arbitrarily use half of that.
		 */
		plan->total_cost += plan->plan_rows * (tlist_rows - 1) *
			cpu_tuple_cost / 2;

		plan->plan_rows *= tlist_rows;
	}
}

/*
 * Detect whether a plan node is a "dummy" plan created when a relation
 * is deemed not to need scanning due to constraint exclusion.
 *
 * Currently, such dummy plans are Result nodes with constant FALSE
 * filter quals (see set_dummy_rel_pathlist and create_append_plan).
 *
 * XXX this probably ought to be somewhere else, but not clear where.
 */
bool
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
 * Create a bitmapset of the RT indexes of live base relations
 *
 * Helper for preprocess_rowmarks ... at this point in the proceedings,
 * the only good way to distinguish baserels from appendrel children
 * is to see what is in the join tree.
 */
static Bitmapset *
get_base_rel_indexes(Node *jtnode)
{
	Bitmapset  *result;

	if (jtnode == NULL)
		return NULL;
	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;

		result = bms_make_singleton(varno);
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		result = NULL;
		foreach(l, f->fromlist)
			result = bms_join(result,
							  get_base_rel_indexes(lfirst(l)));
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		result = bms_join(get_base_rel_indexes(j->larg),
						  get_base_rel_indexes(j->rarg));
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
		result = NULL;			/* keep compiler quiet */
	}
	return result;
}

/*
 * preprocess_rowmarks - set up PlanRowMarks if needed
 */
static void
preprocess_rowmarks(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	Bitmapset  *rels;
	List	   *prowmarks;
	ListCell   *l;
	int			i;

	if (parse->rowMarks)
	{
		/*
		 * We've got trouble if FOR [KEY] UPDATE/SHARE appears inside
		 * grouping, since grouping renders a reference to individual tuple
		 * CTIDs invalid.  This is also checked at parse time, but that's
		 * insufficient because of rule substitution, query pullup, etc.
		 */
		CheckSelectLocking(parse, ((RowMarkClause *)
							linitial(parse->rowMarks))->strength);
	}
	else
	{
		/*
		 * We only need rowmarks for UPDATE, DELETE, or FOR [KEY]
		 * UPDATE/SHARE.
		 */
		if (parse->commandType != CMD_UPDATE &&
			parse->commandType != CMD_DELETE)
			return;
	}

	/*
	 * We need to have rowmarks for all base relations except the target. We
	 * make a bitmapset of all base rels and then remove the items we don't
	 * need or have FOR [KEY] UPDATE/SHARE marks for.
	 */
	rels = get_base_rel_indexes((Node *) parse->jointree);
	if (parse->resultRelation)
		rels = bms_del_member(rels, parse->resultRelation);

	/*
	 * Convert RowMarkClauses to PlanRowMark representation.
	 */
	prowmarks = NIL;
	foreach(l, parse->rowMarks)
	{
		RowMarkClause *rc = (RowMarkClause *) lfirst(l);
		RangeTblEntry *rte = rt_fetch(rc->rti, parse->rtable);
		PlanRowMark *newrc;

		/*
		 * Currently, it is syntactically impossible to have FOR UPDATE et al
		 * applied to an update/delete target rel.	If that ever becomes
		 * possible, we should drop the target from the PlanRowMark list.
		 */
		Assert(rc->rti != parse->resultRelation);

		/*
		 * Ignore RowMarkClauses for subqueries; they aren't real tables and
		 * can't support true locking.  Subqueries that got flattened into the
		 * main query should be ignored completely.  Any that didn't will get
		 * ROW_MARK_COPY items in the next loop.
		 */
		if (rte->rtekind != RTE_RELATION)
			continue;

		/*
		 * Similarly, ignore RowMarkClauses for foreign tables; foreign tables
		 * will instead get ROW_MARK_COPY items in the next loop.  (FDWs might
		 * choose to do something special while fetching their rows, but that
		 * is of no concern here.)
		 */
		if (rte->relkind == RELKIND_FOREIGN_TABLE)
			continue;

		rels = bms_del_member(rels, rc->rti);

		newrc = makeNode(PlanRowMark);
		newrc->rti = newrc->prti = rc->rti;
		newrc->rowmarkId = ++(root->glob->lastRowMarkId);
		switch (rc->strength)
		{
			case LCS_FORUPDATE:
				newrc->markType = ROW_MARK_EXCLUSIVE;
				break;
			case LCS_FORNOKEYUPDATE:
				newrc->markType = ROW_MARK_NOKEYEXCLUSIVE;
				break;
			case LCS_FORSHARE:
				newrc->markType = ROW_MARK_SHARE;
				break;
			case LCS_FORKEYSHARE:
				newrc->markType = ROW_MARK_KEYSHARE;
				break;
		}
		newrc->noWait = rc->noWait;
		newrc->isParent = false;

		prowmarks = lappend(prowmarks, newrc);
	}

	/*
	 * Now, add rowmarks for any non-target, non-locked base relations.
	 */
	i = 0;
	foreach(l, parse->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);
		PlanRowMark *newrc;

		i++;
		if (!bms_is_member(i, rels))
			continue;

		newrc = makeNode(PlanRowMark);
		newrc->rti = newrc->prti = i;
		newrc->rowmarkId = ++(root->glob->lastRowMarkId);
		/* real tables support REFERENCE, anything else needs COPY */
		if (rte->rtekind == RTE_RELATION &&
			rte->relkind != RELKIND_FOREIGN_TABLE)
			newrc->markType = ROW_MARK_REFERENCE;
		else
			newrc->markType = ROW_MARK_COPY;
		newrc->noWait = false;	/* doesn't matter */
		newrc->isParent = false;

		prowmarks = lappend(prowmarks, newrc);
	}

	root->rowMarks = prowmarks;
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
 * limit_needed - do we actually need a Limit plan node?
 *
 * If we have constant-zero OFFSET and constant-null LIMIT, we can skip adding
 * a Limit node.  This is worth checking for because "OFFSET 0" is a common
 * locution for an optimization fence.	(Because other places in the planner
 * merely check whether parse->limitOffset isn't NULL, it will still work as
 * an optimization fence --- we're just suppressing unnecessary run-time
 * overhead.)
 *
 * This might look like it could be merged into preprocess_limit, but there's
 * a key distinction: here we need hard constants in OFFSET/LIMIT, whereas
 * in preprocess_limit it's good enough to consider estimated values.
 */
static bool
limit_needed(Query *parse)
{
	Node	   *node;

	node = parse->limitCount;
	if (node)
	{
		if (IsA(node, Const))
		{
			/* NULL indicates LIMIT ALL, ie, no limit */
			if (!((Const *) node)->constisnull)
				return true;	/* LIMIT with a constant value */
		}
		else
			return true;		/* non-constant LIMIT */
	}

	node = parse->limitOffset;
	if (node)
	{
		if (IsA(node, Const))
		{
			/* Treat NULL as no offset; the executor would too */
			if (!((Const *) node)->constisnull)
			{
				int64		offset = DatumGetInt64(((Const *) node)->constvalue);

				/* Executor would treat less-than-zero same as zero */
				if (offset > 0)
					return true;	/* OFFSET with a positive value */
			}
		}
		else
			return true;		/* non-constant OFFSET */
	}

	return false;				/* don't need a Limit plan node */
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
 * Compute query_pathkeys and other pathkeys during plan generation
 */
static void
standard_qp_callback(PlannerInfo *root, void *extra)
{
	Query	   *parse = root->parse;
	standard_qp_extra *qp_extra = (standard_qp_extra *) extra;
	List	   *tlist = qp_extra->tlist;
	List	   *activeWindows = qp_extra->activeWindows;

	/*
	 * Calculate pathkeys that represent grouping/ordering requirements.  The
	 * sortClause is certainly sort-able, but GROUP BY and DISTINCT might not
	 * be, in which case we just leave their pathkeys empty.
	 */
	if (parse->groupClause &&
		grouping_is_sortable(parse->groupClause))
		root->group_pathkeys =
			make_pathkeys_for_sortclauses(root,
										  parse->groupClause,
										  tlist);
	else
		root->group_pathkeys = NIL;

	/* We consider only the first (bottom) window in pathkeys logic */
	if (activeWindows != NIL)
	{
		WindowClause *wc = (WindowClause *) linitial(activeWindows);

		root->window_pathkeys = make_pathkeys_for_window(root,
														 wc,
														 tlist);
	}
	else
		root->window_pathkeys = NIL;

	if (parse->distinctClause &&
		grouping_is_sortable(parse->distinctClause))
		root->distinct_pathkeys =
			make_pathkeys_for_sortclauses(root,
										  parse->distinctClause,
										  tlist);
	else
		root->distinct_pathkeys = NIL;

	root->sort_pathkeys =
		make_pathkeys_for_sortclauses(root,
									  parse->sortClause,
									  tlist);

	/*
	 * Figure out whether we want a sorted result from query_planner.
	 *
	 * If we have a sortable GROUP BY clause, then we want a result sorted
	 * properly for grouping.  Otherwise, if we have window functions to
	 * evaluate, we try to sort for the first window.  Otherwise, if there's a
	 * sortable DISTINCT clause that's more rigorous than the ORDER BY clause,
	 * we try to produce output that's sufficiently well sorted for the
	 * DISTINCT.  Otherwise, if there is an ORDER BY clause, we want to sort
	 * by the ORDER BY clause.
	 *
	 * Note: if we have both ORDER BY and GROUP BY, and ORDER BY is a superset
	 * of GROUP BY, it would be tempting to request sort by ORDER BY --- but
	 * that might just leave us failing to exploit an available sort order at
	 * all.  Needs more thought.  The choice for DISTINCT versus ORDER BY is
	 * much easier, since we know that the parser ensured that one is a
	 * superset of the other.
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
}

/*
 * choose_hashed_grouping - should we use hashed grouping?
 *
 * Returns TRUE to select hashing, FALSE to select sorting.
 */
static bool
choose_hashed_grouping(PlannerInfo *root,
					   double tuple_fraction, double limit_tuples,
					   double path_rows, int path_width,
					   Path *cheapest_path, Path *sorted_path,
					   double dNumGroups, AggClauseCosts *agg_costs)
{
	Query	   *parse = root->parse;
	int			numGroupCols = list_length(parse->groupClause);
	bool		can_hash;
	bool		can_sort;
	Size		hashentrysize;
	List	   *target_pathkeys;
	List	   *current_pathkeys;
	Path		hashed_p;
	Path		sorted_p;

	/*
	 * Executor doesn't support hashed aggregation with DISTINCT or ORDER BY
	 * aggregates.	(Doing so would imply storing *all* the input values in
	 * the hash table, and/or running many sorts in parallel, either of which
	 * seems like a certain loser.)
	 */
	can_hash = (agg_costs->numOrderedAggs == 0 &&
				grouping_is_hashable(parse->groupClause));
	can_sort = grouping_is_sortable(parse->groupClause);

	/* Quick out if only one choice is workable */
	if (!(can_hash && can_sort))
	{
		if (can_hash)
			return true;
		else if (can_sort)
			return false;
		else
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("could not implement GROUP BY"),
					 errdetail("Some of the datatypes only support hashing, while others only support sorting.")));
	}

	/* Prefer sorting when enable_hashagg is off */
	if (!enable_hashagg)
		return false;

	/*
	 * Don't do it if it doesn't look like the hashtable will fit into
	 * work_mem.
	 */

	/* Estimate per-hash-entry space at tuple width... */
	hashentrysize = MAXALIGN(path_width) + MAXALIGN(sizeof(MinimalTupleData));
	/* plus space for pass-by-ref transition values... */
	hashentrysize += agg_costs->transitionSpace;
	/* plus the per-hash-entry overhead */
	hashentrysize += hash_agg_entry_size(agg_costs->numAggs);

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
	cost_agg(&hashed_p, root, AGG_HASHED, agg_costs,
			 numGroupCols, dNumGroups,
			 cheapest_path->startup_cost, cheapest_path->total_cost,
			 path_rows);
	/* Result of hashed agg is always unsorted */
	if (target_pathkeys)
		cost_sort(&hashed_p, root, target_pathkeys, hashed_p.total_cost,
				  dNumGroups, path_width,
				  0.0, work_mem, limit_tuples);

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
				  path_rows, path_width,
				  0.0, work_mem, -1.0);
		current_pathkeys = root->group_pathkeys;
	}

	if (parse->hasAggs)
		cost_agg(&sorted_p, root, AGG_SORTED, agg_costs,
				 numGroupCols, dNumGroups,
				 sorted_p.startup_cost, sorted_p.total_cost,
				 path_rows);
	else
		cost_group(&sorted_p, root, numGroupCols, dNumGroups,
				   sorted_p.startup_cost, sorted_p.total_cost,
				   path_rows);
	/* The Agg or Group node will preserve ordering */
	if (target_pathkeys &&
		!pathkeys_contained_in(target_pathkeys, current_pathkeys))
		cost_sort(&sorted_p, root, target_pathkeys, sorted_p.total_cost,
				  dNumGroups, path_width,
				  0.0, work_mem, limit_tuples);

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
 * (One difference is that we sometimes apply this after forming a Plan,
 * so the input alternatives can't be represented as Paths --- instead we
 * pass in the costs as individual variables.)
 *
 * But note that making the two choices independently is a bit bogus in
 * itself.	If the two could be combined into a single choice operation
 * it'd probably be better, but that seems far too unwieldy to be practical,
 * especially considering that the combination of GROUP BY and DISTINCT
 * isn't very common in real queries.  By separating them, we are giving
 * extra preference to using a sorting implementation when a common sort key
 * is available ... and that's not necessarily wrong anyway.
 *
 * Returns TRUE to select hashing, FALSE to select sorting.
 */
static bool
choose_hashed_distinct(PlannerInfo *root,
					   double tuple_fraction, double limit_tuples,
					   double path_rows, int path_width,
					   Cost cheapest_startup_cost, Cost cheapest_total_cost,
					   Cost sorted_startup_cost, Cost sorted_total_cost,
					   List *sorted_pathkeys,
					   double dNumDistinctRows)
{
	Query	   *parse = root->parse;
	int			numDistinctCols = list_length(parse->distinctClause);
	bool		can_sort;
	bool		can_hash;
	Size		hashentrysize;
	List	   *current_pathkeys;
	List	   *needed_pathkeys;
	Path		hashed_p;
	Path		sorted_p;

	/*
	 * If we have a sortable DISTINCT ON clause, we always use sorting. This
	 * enforces the expected behavior of DISTINCT ON.
	 */
	can_sort = grouping_is_sortable(parse->distinctClause);
	if (can_sort && parse->hasDistinctOn)
		return false;

	can_hash = grouping_is_hashable(parse->distinctClause);

	/* Quick out if only one choice is workable */
	if (!(can_hash && can_sort))
	{
		if (can_hash)
			return true;
		else if (can_sort)
			return false;
		else
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("could not implement DISTINCT"),
					 errdetail("Some of the datatypes only support hashing, while others only support sorting.")));
	}

	/* Prefer sorting when enable_hashagg is off */
	if (!enable_hashagg)
		return false;

	/*
	 * Don't do it if it doesn't look like the hashtable will fit into
	 * work_mem.
	 */

	/* Estimate per-hash-entry space at tuple width... */
	hashentrysize = MAXALIGN(path_width) + MAXALIGN(sizeof(MinimalTupleData));
	/* plus the per-hash-entry overhead */
	hashentrysize += hash_agg_entry_size(0);

	if (hashentrysize * dNumDistinctRows > work_mem * 1024L)
		return false;

	/*
	 * See if the estimated cost is no more than doing it the other way. While
	 * avoiding the need for sorted input is usually a win, the fact that the
	 * output won't be sorted may be a loss; so we need to do an actual cost
	 * comparison.
	 *
	 * We need to consider cheapest_path + hashagg [+ final sort] versus
	 * sorted_path [+ sort] + group [+ final sort] where brackets indicate a
	 * step that may not be needed.
	 *
	 * These path variables are dummies that just hold cost fields; we don't
	 * make actual Paths for these steps.
	 */
	cost_agg(&hashed_p, root, AGG_HASHED, NULL,
			 numDistinctCols, dNumDistinctRows,
			 cheapest_startup_cost, cheapest_total_cost,
			 path_rows);

	/*
	 * Result of hashed agg is always unsorted, so if ORDER BY is present we
	 * need to charge for the final sort.
	 */
	if (parse->sortClause)
		cost_sort(&hashed_p, root, root->sort_pathkeys, hashed_p.total_cost,
				  dNumDistinctRows, path_width,
				  0.0, work_mem, limit_tuples);

	/*
	 * Now for the GROUP case.	See comments in grouping_planner about the
	 * sorting choices here --- this code should match that code.
	 */
	sorted_p.startup_cost = sorted_startup_cost;
	sorted_p.total_cost = sorted_total_cost;
	current_pathkeys = sorted_pathkeys;
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
			current_pathkeys = root->sort_pathkeys;
		cost_sort(&sorted_p, root, current_pathkeys, sorted_p.total_cost,
				  path_rows, path_width,
				  0.0, work_mem, -1.0);
	}
	cost_group(&sorted_p, root, numDistinctCols, dNumDistinctRows,
			   sorted_p.startup_cost, sorted_p.total_cost,
			   path_rows);
	if (parse->sortClause &&
		!pathkeys_contained_in(root->sort_pathkeys, current_pathkeys))
		cost_sort(&sorted_p, root, root->sort_pathkeys, sorted_p.total_cost,
				  dNumDistinctRows, path_width,
				  0.0, work_mem, limit_tuples);

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

/*
 * make_subplanTargetList
 *	  Generate appropriate target list when grouping is required.
 *
 * When grouping_planner inserts grouping or aggregation plan nodes
 * above the scan/join plan constructed by query_planner+create_plan,
 * we typically want the scan/join plan to emit a different target list
 * than the outer plan nodes should have.  This routine generates the
 * correct target list for the scan/join subplan.
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
 *		a+b,c,d
 * where the a+b target will be used by the Sort/Group steps, and the
 * other targets will be used for computing the final results.
 *
 * If we are grouping or aggregating, *and* there are no non-Var grouping
 * expressions, then the returned tlist is effectively dummy; we do not
 * need to force it to be evaluated, because all the Vars it contains
 * should be present in the "flat" tlist generated by create_plan, though
 * possibly in a different order.  In that case we'll use create_plan's tlist,
 * and the tlist made here is only needed as input to query_planner to tell
 * it which Vars are needed in the output of the scan/join plan.
 *
 * 'tlist' is the query's target list.
 * 'groupColIdx' receives an array of column numbers for the GROUP BY
 *			expressions (if there are any) in the returned target list.
 * 'need_tlist_eval' is set true if we really need to evaluate the
 *			returned tlist as-is.  (Note: locate_grouping_columns assumes
 *			that if this is FALSE, all grouping columns are simple Vars.)
 *
 * The result is the targetlist to be passed to query_planner.
 */
static List *
make_subplanTargetList(PlannerInfo *root,
					   List *tlist,
					   AttrNumber **groupColIdx,
					   bool *need_tlist_eval)
{
	Query	   *parse = root->parse;
	List	   *sub_tlist;
	List	   *non_group_cols;
	List	   *non_group_vars;
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
	 * Otherwise, we must build a tlist containing all grouping columns, plus
	 * any other Vars mentioned in the targetlist and HAVING qual.
	 */
	sub_tlist = NIL;
	non_group_cols = NIL;
	*need_tlist_eval = false;	/* only eval if not flat tlist */

	numCols = list_length(parse->groupClause);
	if (numCols > 0)
	{
		/*
		 * If grouping, create sub_tlist entries for all GROUP BY columns, and
		 * make an array showing where the group columns are in the sub_tlist.
		 *
		 * Note: with this implementation, the array entries will always be
		 * 1..N, but we don't want callers to assume that.
		 */
		AttrNumber *grpColIdx;
		ListCell   *tl;

		grpColIdx = (AttrNumber *) palloc0(sizeof(AttrNumber) * numCols);
		*groupColIdx = grpColIdx;

		foreach(tl, tlist)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(tl);
			int			colno;

			colno = get_grouping_column_index(parse, tle);
			if (colno >= 0)
			{
				/*
				 * It's a grouping column, so add it to the result tlist and
				 * remember its resno in grpColIdx[].
				 */
				TargetEntry *newtle;

				newtle = makeTargetEntry(tle->expr,
										 list_length(sub_tlist) + 1,
										 NULL,
										 false);
				sub_tlist = lappend(sub_tlist, newtle);

				Assert(grpColIdx[colno] == 0);	/* no dups expected */
				grpColIdx[colno] = newtle->resno;

				if (!(newtle->expr && IsA(newtle->expr, Var)))
					*need_tlist_eval = true;	/* tlist contains non Vars */
			}
			else
			{
				/*
				 * Non-grouping column, so just remember the expression for
				 * later call to pull_var_clause.  There's no need for
				 * pull_var_clause to examine the TargetEntry node itself.
				 */
				non_group_cols = lappend(non_group_cols, tle->expr);
			}
		}
	}
	else
	{
		/*
		 * With no grouping columns, just pass whole tlist to pull_var_clause.
		 * Need (shallow) copy to avoid damaging input tlist below.
		 */
		non_group_cols = list_copy(tlist);
	}

	/*
	 * If there's a HAVING clause, we'll need the Vars it uses, too.
	 */
	if (parse->havingQual)
		non_group_cols = lappend(non_group_cols, parse->havingQual);

	/*
	 * Pull out all the Vars mentioned in non-group cols (plus HAVING), and
	 * add them to the result tlist if not already present.  (A Var used
	 * directly as a GROUP BY item will be present already.)  Note this
	 * includes Vars used in resjunk items, so we are covering the needs of
	 * ORDER BY and window specifications.	Vars used within Aggrefs will be
	 * pulled out here, too.
	 */
	non_group_vars = pull_var_clause((Node *) non_group_cols,
									 PVC_RECURSE_AGGREGATES,
									 PVC_INCLUDE_PLACEHOLDERS);
	sub_tlist = add_to_flat_tlist(sub_tlist, non_group_vars);

	/* clean up cruft */
	list_free(non_group_vars);
	list_free(non_group_cols);

	return sub_tlist;
}

/*
 * get_grouping_column_index
 *		Get the GROUP BY column position, if any, of a targetlist entry.
 *
 * Returns the index (counting from 0) of the TLE in the GROUP BY list, or -1
 * if it's not a grouping column.  Note: the result is unique because the
 * parser won't make multiple groupClause entries for the same TLE.
 */
static int
get_grouping_column_index(Query *parse, TargetEntry *tle)
{
	int			colno = 0;
	Index		ressortgroupref = tle->ressortgroupref;
	ListCell   *gl;

	/* No need to search groupClause if TLE hasn't got a sortgroupref */
	if (ressortgroupref == 0)
		return -1;

	foreach(gl, parse->groupClause)
	{
		SortGroupClause *grpcl = (SortGroupClause *) lfirst(gl);

		if (grpcl->tleSortGroupRef == ressortgroupref)
			return colno;
		colno++;
	}

	return -1;
}

/*
 * locate_grouping_columns
 *		Locate grouping columns in the tlist chosen by create_plan.
 *
 * This is only needed if we don't use the sub_tlist chosen by
 * make_subplanTargetList.	We have to forget the column indexes found
 * by that routine and re-locate the grouping exprs in the real sub_tlist.
 * We assume the grouping exprs are just Vars (see make_subplanTargetList).
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
		Var		   *groupexpr = (Var *) get_sortgroupclause_expr(grpcl, tlist);
		TargetEntry *te;

		/*
		 * The grouping column returned by create_plan might not have the same
		 * typmod as the original Var.	(This can happen in cases where a
		 * set-returning function has been inlined, so that we now have more
		 * knowledge about what it returns than we did when the original Var
		 * was created.)  So we can't use tlist_member() to search the tlist;
		 * instead use tlist_member_match_var.	For safety, still check that
		 * the vartype matches.
		 */
		if (!(groupexpr && IsA(groupexpr, Var)))
			elog(ERROR, "grouping column is not a Var as expected");
		te = tlist_member_match_var(groupexpr, sub_tlist);
		if (!te)
			elog(ERROR, "failed to locate grouping columns");
		Assert(((Var *) te->expr)->vartype == groupexpr->vartype);
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
 * make_windowInputTargetList
 *	  Generate appropriate target list for initial input to WindowAgg nodes.
 *
 * When grouping_planner inserts one or more WindowAgg nodes into the plan,
 * this function computes the initial target list to be computed by the node
 * just below the first WindowAgg.	This list must contain all values needed
 * to evaluate the window functions, compute the final target list, and
 * perform any required final sort step.  If multiple WindowAggs are needed,
 * each intermediate one adds its window function results onto this tlist;
 * only the topmost WindowAgg computes the actual desired target list.
 *
 * This function is much like make_subplanTargetList, though not quite enough
 * like it to share code.  As in that function, we flatten most expressions
 * into their component variables.	But we do not want to flatten window
 * PARTITION BY/ORDER BY clauses, since that might result in multiple
 * evaluations of them, which would be bad (possibly even resulting in
 * inconsistent answers, if they contain volatile functions).  Also, we must
 * not flatten GROUP BY clauses that were left unflattened by
 * make_subplanTargetList, because we may no longer have access to the
 * individual Vars in them.
 *
 * Another key difference from make_subplanTargetList is that we don't flatten
 * Aggref expressions, since those are to be computed below the window
 * functions and just referenced like Vars above that.
 *
 * 'tlist' is the query's final target list.
 * 'activeWindows' is the list of active windows previously identified by
 *			select_active_windows.
 *
 * The result is the targetlist to be computed by the plan node immediately
 * below the first WindowAgg node.
 */
static List *
make_windowInputTargetList(PlannerInfo *root,
						   List *tlist,
						   List *activeWindows)
{
	Query	   *parse = root->parse;
	Bitmapset  *sgrefs;
	List	   *new_tlist;
	List	   *flattenable_cols;
	List	   *flattenable_vars;
	ListCell   *lc;

	Assert(parse->hasWindowFuncs);

	/*
	 * Collect the sortgroupref numbers of window PARTITION/ORDER BY clauses
	 * into a bitmapset for convenient reference below.
	 */
	sgrefs = NULL;
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

	/* Add in sortgroupref numbers of GROUP BY clauses, too */
	foreach(lc, parse->groupClause)
	{
		SortGroupClause *grpcl = (SortGroupClause *) lfirst(lc);

		sgrefs = bms_add_member(sgrefs, grpcl->tleSortGroupRef);
	}

	/*
	 * Construct a tlist containing all the non-flattenable tlist items, and
	 * save aside the others for a moment.
	 */
	new_tlist = NIL;
	flattenable_cols = NIL;

	foreach(lc, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		/*
		 * Don't want to deconstruct window clauses or GROUP BY items.  (Note
		 * that such items can't contain window functions, so it's okay to
		 * compute them below the WindowAgg nodes.)
		 */
		if (tle->ressortgroupref != 0 &&
			bms_is_member(tle->ressortgroupref, sgrefs))
		{
			/* Don't want to deconstruct this value, so add to new_tlist */
			TargetEntry *newtle;

			newtle = makeTargetEntry(tle->expr,
									 list_length(new_tlist) + 1,
									 NULL,
									 false);
			/* Preserve its sortgroupref marking, in case it's volatile */
			newtle->ressortgroupref = tle->ressortgroupref;
			new_tlist = lappend(new_tlist, newtle);
		}
		else
		{
			/*
			 * Column is to be flattened, so just remember the expression for
			 * later call to pull_var_clause.  There's no need for
			 * pull_var_clause to examine the TargetEntry node itself.
			 */
			flattenable_cols = lappend(flattenable_cols, tle->expr);
		}
	}

	/*
	 * Pull out all the Vars and Aggrefs mentioned in flattenable columns, and
	 * add them to the result tlist if not already present.  (Some might be
	 * there already because they're used directly as window/group clauses.)
	 *
	 * Note: it's essential to use PVC_INCLUDE_AGGREGATES here, so that the
	 * Aggrefs are placed in the Agg node's tlist and not left to be computed
	 * at higher levels.
	 */
	flattenable_vars = pull_var_clause((Node *) flattenable_cols,
									   PVC_INCLUDE_AGGREGATES,
									   PVC_INCLUDE_PLACEHOLDERS);
	new_tlist = add_to_flat_tlist(new_tlist, flattenable_vars);

	/* clean up cruft */
	list_free(flattenable_vars);
	list_free(flattenable_cols);

	return new_tlist;
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
						 List *tlist)
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
													tlist);
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
														 tlist);
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
														 tlist);
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
 * calls are converted to positional notation and function default arguments
 * get inserted.  The fact that constant subexpressions get simplified is a
 * side-effect that is useful when the expression will get evaluated more than
 * once.  Also, we must fix operator function IDs.
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

	/*
	 * Convert named-argument function calls, insert default arguments and
	 * simplify constant subexprs
	 */
	result = eval_const_expressions(NULL, (Node *) expr);

	/* Fill in opfuncid values if missing */
	fix_opfuncids(result);

	return (Expr *) result;
}


/*
 * plan_cluster_use_sort
 *		Use the planner to decide how CLUSTER should implement sorting
 *
 * tableOid is the OID of a table to be clustered on its index indexOid
 * (which is already known to be a btree index).  Decide whether it's
 * cheaper to do an indexscan or a seqscan-plus-sort to execute the CLUSTER.
 * Return TRUE to use sorting, FALSE to use an indexscan.
 *
 * Note: caller had better already hold some type of lock on the table.
 */
bool
plan_cluster_use_sort(Oid tableOid, Oid indexOid)
{
	PlannerInfo *root;
	Query	   *query;
	PlannerGlobal *glob;
	RangeTblEntry *rte;
	RelOptInfo *rel;
	IndexOptInfo *indexInfo;
	QualCost	indexExprCost;
	Cost		comparisonCost;
	Path	   *seqScanPath;
	Path		seqScanAndSortPath;
	IndexPath  *indexScanPath;
	ListCell   *lc;

	/* Set up mostly-dummy planner state */
	query = makeNode(Query);
	query->commandType = CMD_SELECT;

	glob = makeNode(PlannerGlobal);

	root = makeNode(PlannerInfo);
	root->parse = query;
	root->glob = glob;
	root->query_level = 1;
	root->planner_cxt = CurrentMemoryContext;
	root->wt_param_id = -1;

	/* Build a minimal RTE for the rel */
	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RELATION;
	rte->relid = tableOid;
	rte->relkind = RELKIND_RELATION;	/* Don't be too picky. */
	rte->lateral = false;
	rte->inh = false;
	rte->inFromCl = true;
	query->rtable = list_make1(rte);

	/* Set up RTE/RelOptInfo arrays */
	setup_simple_rel_arrays(root);

	/* Build RelOptInfo */
	rel = build_simple_rel(root, 1, RELOPT_BASEREL);

	/* Locate IndexOptInfo for the target index */
	indexInfo = NULL;
	foreach(lc, rel->indexlist)
	{
		indexInfo = (IndexOptInfo *) lfirst(lc);
		if (indexInfo->indexoid == indexOid)
			break;
	}

	/*
	 * It's possible that get_relation_info did not generate an IndexOptInfo
	 * for the desired index; this could happen if it's not yet reached its
	 * indcheckxmin usability horizon, or if it's a system index and we're
	 * ignoring system indexes.  In such cases we should tell CLUSTER to not
	 * trust the index contents but use seqscan-and-sort.
	 */
	if (lc == NULL)				/* not in the list? */
		return true;			/* use sort */

	/*
	 * Rather than doing all the pushups that would be needed to use
	 * set_baserel_size_estimates, just do a quick hack for rows and width.
	 */
	rel->rows = rel->tuples;
	rel->width = get_relation_data_width(tableOid, NULL);

	root->total_table_pages = rel->pages;

	/*
	 * Determine eval cost of the index expressions, if any.  We need to
	 * charge twice that amount for each tuple comparison that happens during
	 * the sort, since tuplesort.c will have to re-evaluate the index
	 * expressions each time.  (XXX that's pretty inefficient...)
	 */
	cost_qual_eval(&indexExprCost, indexInfo->indexprs, root);
	comparisonCost = 2.0 * (indexExprCost.startup + indexExprCost.per_tuple);

	/* Estimate the cost of seq scan + sort */
	seqScanPath = create_seqscan_path(root, rel, NULL);
	cost_sort(&seqScanAndSortPath, root, NIL,
			  seqScanPath->total_cost, rel->tuples, rel->width,
			  comparisonCost, maintenance_work_mem, -1.0);

	/* Estimate the cost of index scan */
	indexScanPath = create_index_path(root, indexInfo,
									  NIL, NIL, NIL, NIL, NIL,
									  ForwardScanDirection, false,
									  NULL, 1.0);

	return (seqScanAndSortPath.total_cost < indexScanPath->path.total_cost);
}
