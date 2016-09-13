/*-------------------------------------------------------------------------
 *
 * planner.c
 *	  The query optimizer external interface.
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
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
#include <math.h>

#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "catalog/pg_constraint_fn.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "foreign/fdwapi.h"
#include "miscadmin.h"
#include "lib/bipartite_match.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
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
#include "optimizer/var.h"
#include "parser/analyze.h"
#include "parser/parsetree.h"
#include "parser/parse_agg.h"
#include "rewrite/rewriteManip.h"
#include "storage/dsm_impl.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/* GUC parameters */
double		cursor_tuple_fraction = DEFAULT_CURSOR_TUPLE_FRACTION;
int			force_parallel_mode = FORCE_PARALLEL_OFF;

/* Hook for plugins to get control in planner() */
planner_hook_type planner_hook = NULL;

/* Hook for plugins to get control when grouping_planner() plans upper rels */
create_upper_paths_hook_type create_upper_paths_hook = NULL;


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
#define EXPRKIND_TABLESAMPLE	9
#define EXPRKIND_ARBITER_ELEM	10

/* Passthrough data for standard_qp_callback */
typedef struct
{
	List	   *tlist;			/* preprocessed query targetlist */
	List	   *activeWindows;	/* active windows, if any */
	List	   *groupClause;	/* overrides parse->groupClause */
} standard_qp_extra;

/* Local functions */
static Node *preprocess_expression(PlannerInfo *root, Node *expr, int kind);
static void preprocess_qual_conditions(PlannerInfo *root, Node *jtnode);
static void inheritance_planner(PlannerInfo *root);
static void grouping_planner(PlannerInfo *root, bool inheritance_update,
				 double tuple_fraction);
static void preprocess_rowmarks(PlannerInfo *root);
static double preprocess_limit(PlannerInfo *root,
				 double tuple_fraction,
				 int64 *offset_est, int64 *count_est);
static bool limit_needed(Query *parse);
static void remove_useless_groupby_columns(PlannerInfo *root);
static List *preprocess_groupclause(PlannerInfo *root, List *force);
static List *extract_rollup_sets(List *groupingSets);
static List *reorder_grouping_sets(List *groupingSets, List *sortclause);
static void standard_qp_callback(PlannerInfo *root, void *extra);
static double get_number_of_groups(PlannerInfo *root,
					 double path_rows,
					 List *rollup_lists,
					 List *rollup_groupclauses);
static Size estimate_hashagg_tablesize(Path *path,
						   const AggClauseCosts *agg_costs,
						   double dNumGroups);
static RelOptInfo *create_grouping_paths(PlannerInfo *root,
					  RelOptInfo *input_rel,
					  PathTarget *target,
					  const AggClauseCosts *agg_costs,
					  List *rollup_lists,
					  List *rollup_groupclauses);
static RelOptInfo *create_window_paths(PlannerInfo *root,
					RelOptInfo *input_rel,
					PathTarget *input_target,
					PathTarget *output_target,
					List *tlist,
					WindowFuncLists *wflists,
					List *activeWindows);
static void create_one_window_path(PlannerInfo *root,
					   RelOptInfo *window_rel,
					   Path *path,
					   PathTarget *input_target,
					   PathTarget *output_target,
					   List *tlist,
					   WindowFuncLists *wflists,
					   List *activeWindows);
static RelOptInfo *create_distinct_paths(PlannerInfo *root,
					  RelOptInfo *input_rel);
static RelOptInfo *create_ordered_paths(PlannerInfo *root,
					 RelOptInfo *input_rel,
					 PathTarget *target,
					 double limit_tuples);
static PathTarget *make_group_input_target(PlannerInfo *root,
						PathTarget *final_target);
static PathTarget *make_partial_grouping_target(PlannerInfo *root,
							 PathTarget *grouping_target);
static List *postprocess_setop_tlist(List *new_tlist, List *orig_tlist);
static List *select_active_windows(PlannerInfo *root, WindowFuncLists *wflists);
static PathTarget *make_window_input_target(PlannerInfo *root,
						 PathTarget *final_target,
						 List *activeWindows);
static List *make_pathkeys_for_window(PlannerInfo *root, WindowClause *wc,
						 List *tlist);
static PathTarget *make_sort_input_target(PlannerInfo *root,
					   PathTarget *final_target,
					   bool *have_postponed_srfs);


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
	RelOptInfo *final_rel;
	Path	   *best_path;
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
	glob->lastPlanNodeId = 0;
	glob->transientPlan = false;
	glob->dependsOnRole = false;

	/*
	 * Assess whether it's feasible to use parallel mode for this query. We
	 * can't do this in a standalone backend, or if the command will try to
	 * modify any data, or if this is a cursor operation, or if GUCs are set
	 * to values that don't permit parallelism, or if parallel-unsafe
	 * functions are present in the query tree.
	 *
	 * For now, we don't try to use parallel mode if we're running inside a
	 * parallel worker.  We might eventually be able to relax this
	 * restriction, but for now it seems best not to have parallel workers
	 * trying to create their own parallel workers.
	 *
	 * We can't use parallelism in serializable mode because the predicate
	 * locking code is not parallel-aware.  It's not catastrophic if someone
	 * tries to run a parallel plan in serializable mode; it just won't get
	 * any workers and will run serially.  But it seems like a good heuristic
	 * to assume that the same serialization level will be in effect at plan
	 * time and execution time, so don't generate a parallel plan if we're in
	 * serializable mode.
	 */
	if ((cursorOptions & CURSOR_OPT_PARALLEL_OK) != 0 &&
		IsUnderPostmaster &&
		dynamic_shared_memory_type != DSM_IMPL_NONE &&
		parse->commandType == CMD_SELECT &&
		parse->utilityStmt == NULL &&
		!parse->hasModifyingCTE &&
		max_parallel_workers_per_gather > 0 &&
		!IsParallelWorker() &&
		!IsolationIsSerializable())
	{
		/* all the cheap tests pass, so scan the query tree */
		glob->maxParallelHazard = max_parallel_hazard(parse);
		glob->parallelModeOK = (glob->maxParallelHazard != PROPARALLEL_UNSAFE);
	}
	else
	{
		/* skip the query tree scan, just assume it's unsafe */
		glob->maxParallelHazard = PROPARALLEL_UNSAFE;
		glob->parallelModeOK = false;
	}

	/*
	 * glob->parallelModeNeeded should tell us whether it's necessary to
	 * impose the parallel mode restrictions, but we don't actually want to
	 * impose them unless we choose a parallel plan, so it is normally set
	 * only if a parallel plan is chosen (see create_gather_plan).  That way,
	 * people who mislabel their functions but don't use parallelism anyway
	 * aren't harmed.  But when force_parallel_mode is set, we enable the
	 * restrictions whenever possible for testing purposes.
	 */
	glob->parallelModeNeeded = glob->parallelModeOK &&
		(force_parallel_mode != FORCE_PARALLEL_OFF);

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
		 * means the edge cases 0 and 1 have to be treated specially here.  We
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
	root = subquery_planner(glob, parse, NULL,
							false, tuple_fraction);

	/* Select best Path and turn it into a Plan */
	final_rel = fetch_upper_rel(root, UPPERREL_FINAL, NULL);
	best_path = get_cheapest_fractional_path(final_rel, tuple_fraction);

	top_plan = create_plan(root, best_path);

	/*
	 * If creating a plan for a scrollable cursor, make sure it can run
	 * backwards on demand.  Add a Material node at the top at need.
	 */
	if (cursorOptions & CURSOR_OPT_SCROLL)
	{
		if (!ExecSupportsBackwardScan(top_plan))
		{
			Plan	   *sub_plan = top_plan;

			top_plan = materialize_finished_plan(sub_plan);

			/*
			 * XXX horrid kluge: if there are any initPlans attached to the
			 * formerly-top plan node, move them up to the Material node. This
			 * prevents failure in SS_finalize_plan, which see for comments.
			 * We don't bother adjusting the sub_plan's cost estimate for
			 * this.
			 */
			top_plan->initPlan = sub_plan->initPlan;
			sub_plan->initPlan = NIL;
		}
	}

	/*
	 * Optionally add a Gather node for testing purposes, provided this is
	 * actually a safe thing to do.  (Note: we assume adding a Material node
	 * above did not change the parallel safety of the plan, so we can still
	 * rely on best_path->parallel_safe.  However, that flag doesn't account
	 * for initPlans, which render the plan parallel-unsafe.)
	 */
	if (force_parallel_mode != FORCE_PARALLEL_OFF &&
		best_path->parallel_safe &&
		top_plan->initPlan == NIL)
	{
		Gather	   *gather = makeNode(Gather);

		gather->plan.targetlist = top_plan->targetlist;
		gather->plan.qual = NIL;
		gather->plan.lefttree = top_plan;
		gather->plan.righttree = NULL;
		gather->num_workers = 1;
		gather->single_copy = true;
		gather->invisible = (force_parallel_mode == FORCE_PARALLEL_REGRESS);

		/*
		 * Ideally we'd use cost_gather here, but setting up dummy path data
		 * to satisfy it doesn't seem much cleaner than knowing what it does.
		 */
		gather->plan.startup_cost = top_plan->startup_cost +
			parallel_setup_cost;
		gather->plan.total_cost = top_plan->total_cost +
			parallel_setup_cost + parallel_tuple_cost * top_plan->plan_rows;
		gather->plan.plan_rows = top_plan->plan_rows;
		gather->plan.plan_width = top_plan->plan_width;
		gather->plan.parallel_aware = false;

		/* use parallel mode for parallel plans. */
		root->glob->parallelModeNeeded = true;

		top_plan = &gather->plan;
	}

	/*
	 * If any Params were generated, run through the plan tree and compute
	 * each plan node's extParam/allParam sets.  Ideally we'd merge this into
	 * set_plan_references' tree traversal, but for now it has to be separate
	 * because we need to visit subplans before not after main plan.
	 */
	if (glob->nParamExec > 0)
	{
		Assert(list_length(glob->subplans) == list_length(glob->subroots));
		forboth(lp, glob->subplans, lr, glob->subroots)
		{
			Plan	   *subplan = (Plan *) lfirst(lp);
			PlannerInfo *subroot = (PlannerInfo *) lfirst(lr);

			SS_finalize_plan(subroot, subplan);
		}
		SS_finalize_plan(root, top_plan);
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
	result->dependsOnRole = glob->dependsOnRole;
	result->parallelModeNeeded = glob->parallelModeNeeded;
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
 * Basically, this routine does the stuff that should only be done once
 * per Query object.  It then calls grouping_planner.  At one time,
 * grouping_planner could be invoked recursively on the same Query object;
 * that's not currently true, but we keep the separation between the two
 * routines anyway, in case we need it again someday.
 *
 * subquery_planner will be called recursively to handle sub-Query nodes
 * found within the query's expressions and rangetable.
 *
 * Returns the PlannerInfo struct ("root") that contains all data generated
 * while planning the subquery.  In particular, the Path(s) attached to
 * the (UPPERREL_FINAL, NULL) upperrel represent our conclusions about the
 * cheapest way(s) to implement the query.  The top level will select the
 * best Path and pass it through createplan.c to produce a finished Plan.
 *--------------------
 */
PlannerInfo *
subquery_planner(PlannerGlobal *glob, Query *parse,
				 PlannerInfo *parent_root,
				 bool hasRecursion, double tuple_fraction)
{
	PlannerInfo *root;
	List	   *newWithCheckOptions;
	List	   *newHaving;
	bool		hasOuterJoins;
	RelOptInfo *final_rel;
	ListCell   *l;

	/* Create a PlannerInfo data structure for this subquery */
	root = makeNode(PlannerInfo);
	root->parse = parse;
	root->glob = glob;
	root->query_level = parent_root ? parent_root->query_level + 1 : 1;
	root->parent_root = parent_root;
	root->plan_params = NIL;
	root->outer_params = NULL;
	root->planner_cxt = CurrentMemoryContext;
	root->init_plans = NIL;
	root->cte_plan_ids = NIL;
	root->multiexpr_params = NIL;
	root->eq_classes = NIL;
	root->append_rel_list = NIL;
	root->rowMarks = NIL;
	memset(root->upper_rels, 0, sizeof(root->upper_rels));
	memset(root->upper_targets, 0, sizeof(root->upper_targets));
	root->processed_tlist = NIL;
	root->grouping_map = NULL;
	root->minmax_aggs = NIL;
	root->hasInheritedTarget = false;
	root->hasRecursion = hasRecursion;
	if (hasRecursion)
		root->wt_param_id = SS_assign_special_param(root);
	else
		root->wt_param_id = -1;
	root->non_recursive_path = NULL;

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
	pull_up_subqueries(root);

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
	 * Preprocess RowMark information.  We need to do this after subquery
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

	/* Constant-folding might have removed all set-returning functions */
	if (parse->hasTargetSRFs)
		parse->hasTargetSRFs = expression_returns_set((Node *) parse->targetList);

	newWithCheckOptions = NIL;
	foreach(l, parse->withCheckOptions)
	{
		WithCheckOption *wco = (WithCheckOption *) lfirst(l);

		wco->qual = preprocess_expression(root, wco->qual,
										  EXPRKIND_QUAL);
		if (wco->qual != NULL)
			newWithCheckOptions = lappend(newWithCheckOptions, wco);
	}
	parse->withCheckOptions = newWithCheckOptions;

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

	if (parse->onConflict)
	{
		parse->onConflict->arbiterElems = (List *)
			preprocess_expression(root,
								  (Node *) parse->onConflict->arbiterElems,
								  EXPRKIND_ARBITER_ELEM);
		parse->onConflict->arbiterWhere =
			preprocess_expression(root,
								  parse->onConflict->arbiterWhere,
								  EXPRKIND_QUAL);
		parse->onConflict->onConflictSet = (List *)
			preprocess_expression(root,
								  (Node *) parse->onConflict->onConflictSet,
								  EXPRKIND_TARGET);
		parse->onConflict->onConflictWhere =
			preprocess_expression(root,
								  parse->onConflict->onConflictWhere,
								  EXPRKIND_QUAL);
		/* exclRelTlist contains only Vars, so no preprocessing needed */
	}

	root->append_rel_list = (List *)
		preprocess_expression(root, (Node *) root->append_rel_list,
							  EXPRKIND_APPINFO);

	/* Also need to preprocess expressions within RTEs */
	foreach(l, parse->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);
		int			kind;

		if (rte->rtekind == RTE_RELATION)
		{
			if (rte->tablesample)
				rte->tablesample = (TableSampleClause *)
					preprocess_expression(root,
										  (Node *) rte->tablesample,
										  EXPRKIND_TABLESAMPLE);
		}
		else if (rte->rtekind == RTE_SUBQUERY)
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
			/* Preprocess the function expression(s) fully */
			kind = rte->lateral ? EXPRKIND_RTFUNC_LATERAL : EXPRKIND_RTFUNC;
			rte->functions = (List *) preprocess_expression(root, (Node *) rte->functions, kind);
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
	 * only once per group).  We also can't do this if there are any nonempty
	 * grouping sets; moving such a clause into WHERE would potentially change
	 * the results, if any referenced column isn't present in all the grouping
	 * sets.  (If there are only empty grouping sets, then the HAVING clause
	 * must be degenerate as discussed below.)
	 *
	 * Also, it may be that the clause is so expensive to execute that we're
	 * better off doing it only once per group, despite the loss of
	 * selectivity.  This is hard to estimate short of doing the entire
	 * planning process twice, so we use a heuristic: clauses containing
	 * subplans are left in HAVING.  Otherwise, we move or copy the HAVING
	 * clause into WHERE, in hopes of eliminating tuples before aggregation
	 * instead of after.
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

		if ((parse->groupClause && parse->groupingSets) ||
			contain_agg_clause(havingclause) ||
			contain_volatile_functions(havingclause) ||
			contain_subplans(havingclause))
		{
			/* keep it in HAVING */
			newHaving = lappend(newHaving, havingclause);
		}
		else if (parse->groupClause && !parse->groupingSets)
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

	/* Remove any redundant GROUP BY columns */
	remove_useless_groupby_columns(root);

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
		inheritance_planner(root);
	else
		grouping_planner(root, false, tuple_fraction);

	/*
	 * Capture the set of outer-level param IDs we have access to, for use in
	 * extParam/allParam calculations later.
	 */
	SS_identify_outer_params(root);

	/*
	 * If any initPlans were created in this query level, increment the
	 * surviving Paths' costs to account for them.  They won't actually get
	 * attached to the plan tree till create_plan() runs, but we want to be
	 * sure their costs are included now.
	 */
	final_rel = fetch_upper_rel(root, UPPERREL_FINAL, NULL);
	SS_charge_for_initplans(root, final_rel);

	/*
	 * Make sure we've identified the cheapest Path for the final rel.  (By
	 * doing this here not in grouping_planner, we include initPlan costs in
	 * the decision, though it's unlikely that will change anything.)
	 */
	set_cheapest(final_rel);

	return root;
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
	 * We can skip it in non-lateral RTE functions, VALUES lists, and
	 * TABLESAMPLE clauses, however, since they can't contain any Vars of the
	 * current query level.
	 */
	if (root->hasJoinRTEs &&
		!(kind == EXPRKIND_RTFUNC ||
		  kind == EXPRKIND_VALUES ||
		  kind == EXPRKIND_TABLESAMPLE))
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
 *	  Generate Paths in the case where the result relation is an
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
 * Returns nothing; the useful output is in the Paths we attach to
 * the (UPPERREL_FINAL, NULL) upperrel stored in *root.
 *
 * Note that we have not done set_cheapest() on the final rel; it's convenient
 * to leave this to the caller.
 */
static void
inheritance_planner(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	int			parentRTindex = parse->resultRelation;
	Bitmapset  *resultRTindexes;
	Bitmapset  *subqueryRTindexes;
	Bitmapset  *modifiableARIindexes;
	int			nominalRelation = -1;
	List	   *final_rtable = NIL;
	int			save_rel_array_size = 0;
	RelOptInfo **save_rel_array = NULL;
	List	   *subpaths = NIL;
	List	   *subroots = NIL;
	List	   *resultRelations = NIL;
	List	   *withCheckOptionLists = NIL;
	List	   *returningLists = NIL;
	List	   *rowMarks;
	RelOptInfo *final_rel;
	ListCell   *lc;
	Index		rti;

	Assert(parse->commandType != CMD_INSERT);

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
	 *
	 * Note that any RTEs with security barrier quals will be turned into
	 * subqueries during planning, and so we must create copies of them too,
	 * except where they are target relations, which will each only be used in
	 * a single plan.
	 *
	 * To begin with, we'll need a bitmapset of the target relation relids.
	 */
	resultRTindexes = bms_make_singleton(parentRTindex);
	foreach(lc, root->append_rel_list)
	{
		AppendRelInfo *appinfo = (AppendRelInfo *) lfirst(lc);

		if (appinfo->parent_relid == parentRTindex)
			resultRTindexes = bms_add_member(resultRTindexes,
											 appinfo->child_relid);
	}

	/*
	 * Now, generate a bitmapset of the relids of the subquery RTEs, including
	 * security-barrier RTEs that will become subqueries, as just explained.
	 */
	subqueryRTindexes = NULL;
	rti = 1;
	foreach(lc, parse->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		if (rte->rtekind == RTE_SUBQUERY ||
			(rte->securityQuals != NIL &&
			 !bms_is_member(rti, resultRTindexes)))
			subqueryRTindexes = bms_add_member(subqueryRTindexes, rti);
		rti++;
	}

	/*
	 * Next, we want to identify which AppendRelInfo items contain references
	 * to any of the aforesaid subquery RTEs.  These items will need to be
	 * copied and modified to adjust their subquery references; whereas the
	 * other ones need not be touched.  It's worth being tense over this
	 * because we can usually avoid processing most of the AppendRelInfo
	 * items, thereby saving O(N^2) space and time when the target is a large
	 * inheritance tree.  We can identify AppendRelInfo items by their
	 * child_relid, since that should be unique within the list.
	 */
	modifiableARIindexes = NULL;
	if (subqueryRTindexes != NULL)
	{
		foreach(lc, root->append_rel_list)
		{
			AppendRelInfo *appinfo = (AppendRelInfo *) lfirst(lc);

			if (bms_is_member(appinfo->parent_relid, subqueryRTindexes) ||
				bms_is_member(appinfo->child_relid, subqueryRTindexes) ||
				bms_overlap(pull_varnos((Node *) appinfo->translated_vars),
							subqueryRTindexes))
				modifiableARIindexes = bms_add_member(modifiableARIindexes,
													  appinfo->child_relid);
		}
	}

	/*
	 * And now we can get on with generating a plan for each child table.
	 */
	foreach(lc, root->append_rel_list)
	{
		AppendRelInfo *appinfo = (AppendRelInfo *) lfirst(lc);
		PlannerInfo *subroot;
		RelOptInfo *sub_final_rel;
		Path	   *subpath;

		/* append_rel_list contains all append rels; ignore others */
		if (appinfo->parent_relid != parentRTindex)
			continue;

		/*
		 * We need a working copy of the PlannerInfo so that we can control
		 * propagation of information back to the main copy.
		 */
		subroot = makeNode(PlannerInfo);
		memcpy(subroot, root, sizeof(PlannerInfo));

		/*
		 * Generate modified query with this rel as target.  We first apply
		 * adjust_appendrel_attrs, which copies the Query and changes
		 * references to the parent RTE to refer to the current child RTE,
		 * then fool around with subquery RTEs.
		 */
		subroot->parse = (Query *)
			adjust_appendrel_attrs(root,
								   (Node *) parse,
								   appinfo);

		/*
		 * The rowMarks list might contain references to subquery RTEs, so
		 * make a copy that we can apply ChangeVarNodes to.  (Fortunately, the
		 * executor doesn't need to see the modified copies --- we can just
		 * pass it the original rowMarks list.)
		 */
		subroot->rowMarks = (List *) copyObject(root->rowMarks);

		/*
		 * The append_rel_list likewise might contain references to subquery
		 * RTEs (if any subqueries were flattenable UNION ALLs).  So prepare
		 * to apply ChangeVarNodes to that, too.  As explained above, we only
		 * want to copy items that actually contain such references; the rest
		 * can just get linked into the subroot's append_rel_list.
		 *
		 * If we know there are no such references, we can just use the outer
		 * append_rel_list unmodified.
		 */
		if (modifiableARIindexes != NULL)
		{
			ListCell   *lc2;

			subroot->append_rel_list = NIL;
			foreach(lc2, root->append_rel_list)
			{
				AppendRelInfo *appinfo2 = (AppendRelInfo *) lfirst(lc2);

				if (bms_is_member(appinfo2->child_relid, modifiableARIindexes))
					appinfo2 = (AppendRelInfo *) copyObject(appinfo2);

				subroot->append_rel_list = lappend(subroot->append_rel_list,
												   appinfo2);
			}
		}

		/*
		 * Add placeholders to the child Query's rangetable list to fill the
		 * RT indexes already reserved for subqueries in previous children.
		 * These won't be referenced, so there's no need to make them very
		 * valid-looking.
		 */
		while (list_length(subroot->parse->rtable) < list_length(final_rtable))
			subroot->parse->rtable = lappend(subroot->parse->rtable,
											 makeNode(RangeTblEntry));

		/*
		 * If this isn't the first child Query, generate duplicates of all
		 * subquery (or subquery-to-be) RTEs, and adjust Var numbering to
		 * reference the duplicates.  To simplify the loop logic, we scan the
		 * original rtable not the copy just made by adjust_appendrel_attrs;
		 * that should be OK since subquery RTEs couldn't contain any
		 * references to the target rel.
		 */
		if (final_rtable != NIL && subqueryRTindexes != NULL)
		{
			ListCell   *lr;

			rti = 1;
			foreach(lr, parse->rtable)
			{
				RangeTblEntry *rte = (RangeTblEntry *) lfirst(lr);

				if (bms_is_member(rti, subqueryRTindexes))
				{
					Index		newrti;

					/*
					 * The RTE can't contain any references to its own RT
					 * index, except in the security barrier quals, so we can
					 * save a few cycles by applying ChangeVarNodes before we
					 * append the RTE to the rangetable.
					 */
					newrti = list_length(subroot->parse->rtable) + 1;
					ChangeVarNodes((Node *) subroot->parse, rti, newrti, 0);
					ChangeVarNodes((Node *) subroot->rowMarks, rti, newrti, 0);
					/* Skip processing unchanging parts of append_rel_list */
					if (modifiableARIindexes != NULL)
					{
						ListCell   *lc2;

						foreach(lc2, subroot->append_rel_list)
						{
							AppendRelInfo *appinfo2 = (AppendRelInfo *) lfirst(lc2);

							if (bms_is_member(appinfo2->child_relid,
											  modifiableARIindexes))
								ChangeVarNodes((Node *) appinfo2, rti, newrti, 0);
						}
					}
					rte = copyObject(rte);
					ChangeVarNodes((Node *) rte->securityQuals, rti, newrti, 0);
					subroot->parse->rtable = lappend(subroot->parse->rtable,
													 rte);
				}
				rti++;
			}
		}

		/* There shouldn't be any OJ info to translate, as yet */
		Assert(subroot->join_info_list == NIL);
		/* and we haven't created PlaceHolderInfos, either */
		Assert(subroot->placeholder_list == NIL);
		/* hack to mark target relation as an inheritance partition */
		subroot->hasInheritedTarget = true;

		/* Generate Path(s) for accessing this result relation */
		grouping_planner(subroot, true, 0.0 /* retrieve all tuples */ );

		/*
		 * Planning may have modified the query result relation (if there were
		 * security barrier quals on the result RTE).
		 */
		appinfo->child_relid = subroot->parse->resultRelation;

		/*
		 * We'll use the first child relation (even if it's excluded) as the
		 * nominal target relation of the ModifyTable node.  Because of the
		 * way expand_inherited_rtentry works, this should always be the RTE
		 * representing the parent table in its role as a simple member of the
		 * inheritance set.  (It would be logically cleaner to use the
		 * inheritance parent RTE as the nominal target; but since that RTE
		 * will not be otherwise referenced in the plan, doing so would give
		 * rise to confusing use of multiple aliases in EXPLAIN output for
		 * what the user will think is the "same" table.)
		 */
		if (nominalRelation < 0)
			nominalRelation = appinfo->child_relid;

		/*
		 * Select cheapest path in case there's more than one.  We always run
		 * modification queries to conclusion, so we care only for the
		 * cheapest-total path.
		 */
		sub_final_rel = fetch_upper_rel(subroot, UPPERREL_FINAL, NULL);
		set_cheapest(sub_final_rel);
		subpath = sub_final_rel->cheapest_total_path;

		/*
		 * If this child rel was excluded by constraint exclusion, exclude it
		 * from the result plan.
		 */
		if (IS_DUMMY_PATH(subpath))
			continue;

		/*
		 * If this is the first non-excluded child, its post-planning rtable
		 * becomes the initial contents of final_rtable; otherwise, append
		 * just its modified subquery RTEs to final_rtable.
		 */
		if (final_rtable == NIL)
			final_rtable = subroot->parse->rtable;
		else
		{
			List	   *tmp_rtable = NIL;
			ListCell   *cell1,
					   *cell2;

			/*
			 * Check to see if any of the original RTEs were turned into
			 * subqueries during planning.  Currently, this should only ever
			 * happen due to securityQuals being involved which push a
			 * relation down under a subquery, to ensure that the security
			 * barrier quals are evaluated first.
			 *
			 * When this happens, we want to use the new subqueries in the
			 * final rtable.
			 */
			forboth(cell1, final_rtable, cell2, subroot->parse->rtable)
			{
				RangeTblEntry *rte1 = (RangeTblEntry *) lfirst(cell1);
				RangeTblEntry *rte2 = (RangeTblEntry *) lfirst(cell2);

				if (rte1->rtekind == RTE_RELATION &&
					rte2->rtekind == RTE_SUBQUERY)
				{
					/* Should only be when there are securityQuals today */
					Assert(rte1->securityQuals != NIL);
					tmp_rtable = lappend(tmp_rtable, rte2);
				}
				else
					tmp_rtable = lappend(tmp_rtable, rte1);
			}

			final_rtable = list_concat(tmp_rtable,
									   list_copy_tail(subroot->parse->rtable,
												 list_length(final_rtable)));
		}

		/*
		 * We need to collect all the RelOptInfos from all child plans into
		 * the main PlannerInfo, since setrefs.c will need them.  We use the
		 * last child's simple_rel_array (previous ones are too short), so we
		 * have to propagate forward the RelOptInfos that were already built
		 * in previous children.
		 */
		Assert(subroot->simple_rel_array_size >= save_rel_array_size);
		for (rti = 1; rti < save_rel_array_size; rti++)
		{
			RelOptInfo *brel = save_rel_array[rti];

			if (brel)
				subroot->simple_rel_array[rti] = brel;
		}
		save_rel_array_size = subroot->simple_rel_array_size;
		save_rel_array = subroot->simple_rel_array;

		/* Make sure any initplans from this rel get into the outer list */
		root->init_plans = subroot->init_plans;

		/* Build list of sub-paths */
		subpaths = lappend(subpaths, subpath);

		/* Build list of modified subroots, too */
		subroots = lappend(subroots, subroot);

		/* Build list of target-relation RT indexes */
		resultRelations = lappend_int(resultRelations, appinfo->child_relid);

		/* Build lists of per-relation WCO and RETURNING targetlists */
		if (parse->withCheckOptions)
			withCheckOptionLists = lappend(withCheckOptionLists,
										   subroot->parse->withCheckOptions);
		if (parse->returningList)
			returningLists = lappend(returningLists,
									 subroot->parse->returningList);

		Assert(!parse->onConflict);
	}

	/* Result path must go into outer query's FINAL upperrel */
	final_rel = fetch_upper_rel(root, UPPERREL_FINAL, NULL);

	/*
	 * We don't currently worry about setting final_rel's consider_parallel
	 * flag in this case, nor about allowing FDWs or create_upper_paths_hook
	 * to get control here.
	 */

	/*
	 * If we managed to exclude every child rel, return a dummy plan; it
	 * doesn't even need a ModifyTable node.
	 */
	if (subpaths == NIL)
	{
		set_dummy_rel_pathlist(final_rel);
		return;
	}

	/*
	 * Put back the final adjusted rtable into the master copy of the Query.
	 * (We mustn't do this if we found no non-excluded children.)
	 */
	parse->rtable = final_rtable;
	root->simple_rel_array_size = save_rel_array_size;
	root->simple_rel_array = save_rel_array;
	/* Must reconstruct master's simple_rte_array, too */
	root->simple_rte_array = (RangeTblEntry **)
		palloc0((list_length(final_rtable) + 1) * sizeof(RangeTblEntry *));
	rti = 1;
	foreach(lc, final_rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		root->simple_rte_array[rti++] = rte;
	}

	/*
	 * If there was a FOR [KEY] UPDATE/SHARE clause, the LockRows node will
	 * have dealt with fetching non-locked marked rows, else we need to have
	 * ModifyTable do that.
	 */
	if (parse->rowMarks)
		rowMarks = NIL;
	else
		rowMarks = root->rowMarks;

	/* Create Path representing a ModifyTable to do the UPDATE/DELETE work */
	add_path(final_rel, (Path *)
			 create_modifytable_path(root, final_rel,
									 parse->commandType,
									 parse->canSetTag,
									 nominalRelation,
									 resultRelations,
									 subpaths,
									 subroots,
									 withCheckOptionLists,
									 returningLists,
									 rowMarks,
									 NULL,
									 SS_assign_special_param(root)));
}

/*--------------------
 * grouping_planner
 *	  Perform planning steps related to grouping, aggregation, etc.
 *
 * This function adds all required top-level processing to the scan/join
 * Path(s) produced by query_planner.
 *
 * If inheritance_update is true, we're being called from inheritance_planner
 * and should not include a ModifyTable step in the resulting Path(s).
 * (inheritance_planner will create a single ModifyTable node covering all the
 * target tables.)
 *
 * tuple_fraction is the fraction of tuples we expect will be retrieved.
 * tuple_fraction is interpreted as follows:
 *	  0: expect all tuples to be retrieved (normal case)
 *	  0 < tuple_fraction < 1: expect the given fraction of tuples available
 *		from the plan to be retrieved
 *	  tuple_fraction >= 1: tuple_fraction is the absolute number of tuples
 *		expected to be retrieved (ie, a LIMIT specification)
 *
 * Returns nothing; the useful output is in the Paths we attach to the
 * (UPPERREL_FINAL, NULL) upperrel in *root.  In addition,
 * root->processed_tlist contains the final processed targetlist.
 *
 * Note that we have not done set_cheapest() on the final rel; it's convenient
 * to leave this to the caller.
 *--------------------
 */
static void
grouping_planner(PlannerInfo *root, bool inheritance_update,
				 double tuple_fraction)
{
	Query	   *parse = root->parse;
	List	   *tlist = parse->targetList;
	int64		offset_est = 0;
	int64		count_est = 0;
	double		limit_tuples = -1.0;
	bool		have_postponed_srfs = false;
	double		tlist_rows;
	PathTarget *final_target;
	RelOptInfo *current_rel;
	RelOptInfo *final_rel;
	ListCell   *lc;

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

	/* Make tuple_fraction accessible to lower-level routines */
	root->tuple_fraction = tuple_fraction;

	if (parse->setOperations)
	{
		/*
		 * If there's a top-level ORDER BY, assume we have to fetch all the
		 * tuples.  This might be too simplistic given all the hackery below
		 * to possibly avoid the sort; but the odds of accurate estimates here
		 * are pretty low anyway.  XXX try to get rid of this in favor of
		 * letting plan_set_operations generate both fast-start and
		 * cheapest-total paths.
		 */
		if (parse->sortClause)
			root->tuple_fraction = 0.0;

		/*
		 * Construct Paths for set operations.  The results will not need any
		 * work except perhaps a top-level sort and/or LIMIT.  Note that any
		 * special work for recursive unions is the responsibility of
		 * plan_set_operations.
		 */
		current_rel = plan_set_operations(root);

		/*
		 * We should not need to call preprocess_targetlist, since we must be
		 * in a SELECT query node.  Instead, use the targetlist returned by
		 * plan_set_operations (since this tells whether it returned any
		 * resjunk columns!), and transfer any sort key information from the
		 * original tlist.
		 */
		Assert(parse->commandType == CMD_SELECT);

		tlist = root->processed_tlist;	/* from plan_set_operations */

		/* for safety, copy processed_tlist instead of modifying in-place */
		tlist = postprocess_setop_tlist(copyObject(tlist), parse->targetList);

		/* Save aside the final decorated tlist */
		root->processed_tlist = tlist;

		/* Also extract the PathTarget form of the setop result tlist */
		final_target = current_rel->cheapest_total_path->pathtarget;

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
		PathTarget *sort_input_target;
		PathTarget *grouping_target;
		PathTarget *scanjoin_target;
		bool		have_grouping;
		AggClauseCosts agg_costs;
		WindowFuncLists *wflists = NULL;
		List	   *activeWindows = NIL;
		List	   *rollup_lists = NIL;
		List	   *rollup_groupclauses = NIL;
		standard_qp_extra qp_extra;

		/* A recursive query should always have setOperations */
		Assert(!root->hasRecursion);

		/* Preprocess grouping sets and GROUP BY clause, if any */
		if (parse->groupingSets)
		{
			int		   *tleref_to_colnum_map;
			List	   *sets;
			int			maxref;
			ListCell   *lc;
			ListCell   *lc2;
			ListCell   *lc_set;

			parse->groupingSets = expand_grouping_sets(parse->groupingSets, -1);

			/* Identify max SortGroupRef in groupClause, for array sizing */
			maxref = 0;
			foreach(lc, parse->groupClause)
			{
				SortGroupClause *gc = lfirst(lc);

				if (gc->tleSortGroupRef > maxref)
					maxref = gc->tleSortGroupRef;
			}

			/* Allocate workspace array for remapping */
			tleref_to_colnum_map = (int *) palloc((maxref + 1) * sizeof(int));

			/* Examine the rollup sets */
			sets = extract_rollup_sets(parse->groupingSets);

			foreach(lc_set, sets)
			{
				List	   *current_sets = (List *) lfirst(lc_set);
				List	   *groupclause;
				int			ref;

				/*
				 * Reorder the current list of grouping sets into correct
				 * prefix order.  If only one aggregation pass is needed, try
				 * to make the list match the ORDER BY clause; if more than
				 * one pass is needed, we don't bother with that.
				 */
				current_sets = reorder_grouping_sets(current_sets,
													 (list_length(sets) == 1
													  ? parse->sortClause
													  : NIL));

				/*
				 * Order the groupClause appropriately.  If the first grouping
				 * set is empty, this can match regular GROUP BY
				 * preprocessing, otherwise we have to force the groupClause
				 * to match that grouping set's order.
				 */
				groupclause = preprocess_groupclause(root,
													 linitial(current_sets));

				/*
				 * Now that we've pinned down an order for the groupClause for
				 * this list of grouping sets, we need to remap the entries in
				 * the grouping sets from sortgrouprefs to plain indices
				 * (0-based) into the groupClause for this collection of
				 * grouping sets.
				 */
				ref = 0;
				foreach(lc, groupclause)
				{
					SortGroupClause *gc = lfirst(lc);

					tleref_to_colnum_map[gc->tleSortGroupRef] = ref++;
				}

				foreach(lc, current_sets)
				{
					foreach(lc2, (List *) lfirst(lc))
					{
						lfirst_int(lc2) = tleref_to_colnum_map[lfirst_int(lc2)];
					}
				}

				/* Save the reordered sets and corresponding groupclauses */
				rollup_lists = lcons(current_sets, rollup_lists);
				rollup_groupclauses = lcons(groupclause, rollup_groupclauses);
			}
		}
		else
		{
			/* Preprocess regular GROUP BY clause, if any */
			if (parse->groupClause)
				parse->groupClause = preprocess_groupclause(root, NIL);
		}

		/* Preprocess targetlist */
		tlist = preprocess_targetlist(root, tlist);

		if (parse->onConflict)
			parse->onConflict->onConflictSet =
				preprocess_onconflict_targetlist(parse->onConflict->onConflictSet,
												 parse->resultRelation,
												 parse->rtable);

		/*
		 * Expand any rangetable entries that have security barrier quals.
		 * This may add new security barrier subquery RTEs to the rangetable.
		 */
		expand_security_quals(root, tlist);

		/*
		 * We are now done hacking up the query's targetlist.  Most of the
		 * remaining planning work will be done with the PathTarget
		 * representation of tlists, but save aside the full representation so
		 * that we can transfer its decoration (resnames etc) to the topmost
		 * tlist of the finished Plan.
		 */
		root->processed_tlist = tlist;

		/*
		 * Collect statistics about aggregates for estimating costs, and mark
		 * all the aggregates with resolved aggtranstypes.  We must do this
		 * before slicing and dicing the tlist into various pathtargets, else
		 * some copies of the Aggref nodes might escape being marked with the
		 * correct transtypes.
		 *
		 * Note: currently, we do not detect duplicate aggregates here.  This
		 * may result in somewhat-overestimated cost, which is fine for our
		 * purposes since all Paths will get charged the same.  But at some
		 * point we might wish to do that detection in the planner, rather
		 * than during executor startup.
		 */
		MemSet(&agg_costs, 0, sizeof(AggClauseCosts));
		if (parse->hasAggs)
		{
			get_agg_clause_costs(root, (Node *) tlist, AGGSPLIT_SIMPLE,
								 &agg_costs);
			get_agg_clause_costs(root, parse->havingQual, AGGSPLIT_SIMPLE,
								 &agg_costs);
		}

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
		 * Preprocess MIN/MAX aggregates, if any.  Note: be careful about
		 * adding logic between here and the query_planner() call.  Anything
		 * that is needed in MIN/MAX-optimizable cases will have to be
		 * duplicated in planagg.c.
		 */
		if (parse->hasAggs)
			preprocess_minmax_aggregates(root, tlist);

		/*
		 * Figure out whether there's a hard limit on the number of rows that
		 * query_planner's result subplan needs to return.  Even if we know a
		 * hard limit overall, it doesn't apply if the query has any
		 * grouping/aggregation operations, or SRFs in the tlist.
		 */
		if (parse->groupClause ||
			parse->groupingSets ||
			parse->distinctClause ||
			parse->hasAggs ||
			parse->hasWindowFuncs ||
			parse->hasTargetSRFs ||
			root->hasHavingQual)
			root->limit_tuples = -1.0;
		else
			root->limit_tuples = limit_tuples;

		/* Set up data needed by standard_qp_callback */
		qp_extra.tlist = tlist;
		qp_extra.activeWindows = activeWindows;
		qp_extra.groupClause =
			parse->groupingSets ? llast(rollup_groupclauses) : parse->groupClause;

		/*
		 * Generate the best unsorted and presorted paths for the scan/join
		 * portion of this Query, ie the processing represented by the
		 * FROM/WHERE clauses.  (Note there may not be any presorted paths.)
		 * We also generate (in standard_qp_callback) pathkey representations
		 * of the query's sort clause, distinct clause, etc.
		 */
		current_rel = query_planner(root, tlist,
									standard_qp_callback, &qp_extra);

		/*
		 * Convert the query's result tlist into PathTarget format.
		 *
		 * Note: it's desirable to not do this till after query_planner(),
		 * because the target width estimates can use per-Var width numbers
		 * that were obtained within query_planner().
		 */
		final_target = create_pathtarget(root, tlist);

		/*
		 * If ORDER BY was given, consider whether we should use a post-sort
		 * projection, and compute the adjusted target for preceding steps if
		 * so.
		 */
		if (parse->sortClause)
			sort_input_target = make_sort_input_target(root,
													   final_target,
													   &have_postponed_srfs);
		else
			sort_input_target = final_target;

		/*
		 * If we have window functions to deal with, the output from any
		 * grouping step needs to be what the window functions want;
		 * otherwise, it should be sort_input_target.
		 */
		if (activeWindows)
			grouping_target = make_window_input_target(root,
													   final_target,
													   activeWindows);
		else
			grouping_target = sort_input_target;

		/*
		 * If we have grouping or aggregation to do, the topmost scan/join
		 * plan node must emit what the grouping step wants; otherwise, it
		 * should emit grouping_target.
		 */
		have_grouping = (parse->groupClause || parse->groupingSets ||
						 parse->hasAggs || root->hasHavingQual);
		if (have_grouping)
			scanjoin_target = make_group_input_target(root, final_target);
		else
			scanjoin_target = grouping_target;

		/*
		 * Forcibly apply scan/join target to all the Paths for the scan/join
		 * rel.
		 *
		 * In principle we should re-run set_cheapest() here to identify the
		 * cheapest path, but it seems unlikely that adding the same tlist
		 * eval costs to all the paths would change that, so we don't bother.
		 * Instead, just assume that the cheapest-startup and cheapest-total
		 * paths remain so.  (There should be no parameterized paths anymore,
		 * so we needn't worry about updating cheapest_parameterized_paths.)
		 */
		foreach(lc, current_rel->pathlist)
		{
			Path	   *subpath = (Path *) lfirst(lc);
			Path	   *path;

			Assert(subpath->param_info == NULL);
			path = apply_projection_to_path(root, current_rel,
											subpath, scanjoin_target);
			/* If we had to add a Result, path is different from subpath */
			if (path != subpath)
			{
				lfirst(lc) = path;
				if (subpath == current_rel->cheapest_startup_path)
					current_rel->cheapest_startup_path = path;
				if (subpath == current_rel->cheapest_total_path)
					current_rel->cheapest_total_path = path;
			}
		}

		/*
		 * Upper planning steps which make use of the top scan/join rel's
		 * partial pathlist will expect partial paths for that rel to produce
		 * the same output as complete paths ... and we just changed the
		 * output for the complete paths, so we'll need to do the same thing
		 * for partial paths.  But only parallel-safe expressions can be
		 * computed by partial paths.
		 */
		if (current_rel->partial_pathlist &&
			is_parallel_safe(root, (Node *) scanjoin_target->exprs))
		{
			/* Apply the scan/join target to each partial path */
			foreach(lc, current_rel->partial_pathlist)
			{
				Path	   *subpath = (Path *) lfirst(lc);
				Path	   *newpath;

				/* Shouldn't have any parameterized paths anymore */
				Assert(subpath->param_info == NULL);

				/*
				 * Don't use apply_projection_to_path() here, because there
				 * could be other pointers to these paths, and therefore we
				 * mustn't modify them in place.
				 */
				newpath = (Path *) create_projection_path(root,
														  current_rel,
														  subpath,
														  scanjoin_target);
				lfirst(lc) = newpath;
			}
		}
		else
		{
			/*
			 * In the unfortunate event that scanjoin_target is not
			 * parallel-safe, we can't apply it to the partial paths; in that
			 * case, we'll need to forget about the partial paths, which
			 * aren't valid input for upper planning steps.
			 */
			current_rel->partial_pathlist = NIL;
		}

		/*
		 * Save the various upper-rel PathTargets we just computed into
		 * root->upper_targets[].  The core code doesn't use this, but it
		 * provides a convenient place for extensions to get at the info.  For
		 * consistency, we save all the intermediate targets, even though some
		 * of the corresponding upperrels might not be needed for this query.
		 */
		root->upper_targets[UPPERREL_FINAL] = final_target;
		root->upper_targets[UPPERREL_WINDOW] = sort_input_target;
		root->upper_targets[UPPERREL_GROUP_AGG] = grouping_target;

		/*
		 * If we have grouping and/or aggregation, consider ways to implement
		 * that.  We build a new upperrel representing the output of this
		 * phase.
		 */
		if (have_grouping)
		{
			current_rel = create_grouping_paths(root,
												current_rel,
												grouping_target,
												&agg_costs,
												rollup_lists,
												rollup_groupclauses);
		}

		/*
		 * If we have window functions, consider ways to implement those.  We
		 * build a new upperrel representing the output of this phase.
		 */
		if (activeWindows)
		{
			current_rel = create_window_paths(root,
											  current_rel,
											  grouping_target,
											  sort_input_target,
											  tlist,
											  wflists,
											  activeWindows);
		}

		/*
		 * If there is a DISTINCT clause, consider ways to implement that. We
		 * build a new upperrel representing the output of this phase.
		 */
		if (parse->distinctClause)
		{
			current_rel = create_distinct_paths(root,
												current_rel);
		}

	}							/* end of if (setOperations) */

	/*
	 * If ORDER BY was given, consider ways to implement that, and generate a
	 * new upperrel containing only paths that emit the correct ordering and
	 * project the correct final_target.  We can apply the original
	 * limit_tuples limit in sort costing here, but only if there are no
	 * postponed SRFs.
	 */
	if (parse->sortClause)
	{
		current_rel = create_ordered_paths(root,
										   current_rel,
										   final_target,
										   have_postponed_srfs ? -1.0 :
										   limit_tuples);
	}

	/*
	 * If there are set-returning functions in the tlist, scale up the output
	 * rowcounts of all surviving Paths to account for that.  Note that if any
	 * SRFs appear in sorting or grouping columns, we'll have underestimated
	 * the numbers of rows passing through earlier steps; but that's such a
	 * weird usage that it doesn't seem worth greatly complicating matters to
	 * account for it.
	 */
	if (parse->hasTargetSRFs)
		tlist_rows = tlist_returns_set_rows(tlist);
	else
		tlist_rows = 1;

	if (tlist_rows > 1)
	{
		foreach(lc, current_rel->pathlist)
		{
			Path	   *path = (Path *) lfirst(lc);

			/*
			 * We assume that execution costs of the tlist as such were
			 * already accounted for.  However, it still seems appropriate to
			 * charge something more for the executor's general costs of
			 * processing the added tuples.  The cost is probably less than
			 * cpu_tuple_cost, though, so we arbitrarily use half of that.
			 */
			path->total_cost += path->rows * (tlist_rows - 1) *
				cpu_tuple_cost / 2;

			path->rows *= tlist_rows;
		}
		/* No need to run set_cheapest; we're keeping all paths anyway. */
	}

	/*
	 * Now we are prepared to build the final-output upperrel.
	 */
	final_rel = fetch_upper_rel(root, UPPERREL_FINAL, NULL);

	/*
	 * If the input rel is marked consider_parallel and there's nothing that's
	 * not parallel-safe in the LIMIT clause, then the final_rel can be marked
	 * consider_parallel as well.  Note that if the query has rowMarks or is
	 * not a SELECT, consider_parallel will be false for every relation in the
	 * query.
	 */
	if (current_rel->consider_parallel &&
		is_parallel_safe(root, parse->limitOffset) &&
		is_parallel_safe(root, parse->limitCount))
		final_rel->consider_parallel = true;

	/*
	 * If the current_rel belongs to a single FDW, so does the final_rel.
	 */
	final_rel->serverid = current_rel->serverid;
	final_rel->userid = current_rel->userid;
	final_rel->useridiscurrent = current_rel->useridiscurrent;
	final_rel->fdwroutine = current_rel->fdwroutine;

	/*
	 * Generate paths for the final_rel.  Insert all surviving paths, with
	 * LockRows, Limit, and/or ModifyTable steps added if needed.
	 */
	foreach(lc, current_rel->pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);

		/*
		 * If there is a FOR [KEY] UPDATE/SHARE clause, add the LockRows node.
		 * (Note: we intentionally test parse->rowMarks not root->rowMarks
		 * here.  If there are only non-locking rowmarks, they should be
		 * handled by the ModifyTable node instead.  However, root->rowMarks
		 * is what goes into the LockRows node.)
		 */
		if (parse->rowMarks)
		{
			path = (Path *) create_lockrows_path(root, final_rel, path,
												 root->rowMarks,
											  SS_assign_special_param(root));
		}

		/*
		 * If there is a LIMIT/OFFSET clause, add the LIMIT node.
		 */
		if (limit_needed(parse))
		{
			path = (Path *) create_limit_path(root, final_rel, path,
											  parse->limitOffset,
											  parse->limitCount,
											  offset_est, count_est);
		}

		/*
		 * If this is an INSERT/UPDATE/DELETE, and we're not being called from
		 * inheritance_planner, add the ModifyTable node.
		 */
		if (parse->commandType != CMD_SELECT && !inheritance_update)
		{
			List	   *withCheckOptionLists;
			List	   *returningLists;
			List	   *rowMarks;

			/*
			 * Set up the WITH CHECK OPTION and RETURNING lists-of-lists, if
			 * needed.
			 */
			if (parse->withCheckOptions)
				withCheckOptionLists = list_make1(parse->withCheckOptions);
			else
				withCheckOptionLists = NIL;

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

			path = (Path *)
				create_modifytable_path(root, final_rel,
										parse->commandType,
										parse->canSetTag,
										parse->resultRelation,
										list_make1_int(parse->resultRelation),
										list_make1(path),
										list_make1(root),
										withCheckOptionLists,
										returningLists,
										rowMarks,
										parse->onConflict,
										SS_assign_special_param(root));
		}

		/* And shove it into final_rel */
		add_path(final_rel, path);
	}

	/*
	 * If there is an FDW that's responsible for all baserels of the query,
	 * let it consider adding ForeignPaths.
	 */
	if (final_rel->fdwroutine &&
		final_rel->fdwroutine->GetForeignUpperPaths)
		final_rel->fdwroutine->GetForeignUpperPaths(root, UPPERREL_FINAL,
													current_rel, final_rel);

	/* Let extensions possibly add some more paths */
	if (create_upper_paths_hook)
		(*create_upper_paths_hook) (root, UPPERREL_FINAL,
									current_rel, final_rel);

	/* Note: currently, we leave it to callers to do set_cheapest() */
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
		 * applied to an update/delete target rel.  If that ever becomes
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

		rels = bms_del_member(rels, rc->rti);

		newrc = makeNode(PlanRowMark);
		newrc->rti = newrc->prti = rc->rti;
		newrc->rowmarkId = ++(root->glob->lastRowMarkId);
		newrc->markType = select_rowmark_type(rte, rc->strength);
		newrc->allMarkTypes = (1 << newrc->markType);
		newrc->strength = rc->strength;
		newrc->waitPolicy = rc->waitPolicy;
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
		newrc->markType = select_rowmark_type(rte, LCS_NONE);
		newrc->allMarkTypes = (1 << newrc->markType);
		newrc->strength = LCS_NONE;
		newrc->waitPolicy = LockWaitBlock;		/* doesn't matter */
		newrc->isParent = false;

		prowmarks = lappend(prowmarks, newrc);
	}

	root->rowMarks = prowmarks;
}

/*
 * Select RowMarkType to use for a given table
 */
RowMarkType
select_rowmark_type(RangeTblEntry *rte, LockClauseStrength strength)
{
	if (rte->rtekind != RTE_RELATION)
	{
		/* If it's not a table at all, use ROW_MARK_COPY */
		return ROW_MARK_COPY;
	}
	else if (rte->relkind == RELKIND_FOREIGN_TABLE)
	{
		/* Let the FDW select the rowmark type, if it wants to */
		FdwRoutine *fdwroutine = GetFdwRoutineByRelId(rte->relid);

		if (fdwroutine->GetForeignRowMarkType != NULL)
			return fdwroutine->GetForeignRowMarkType(rte, strength);
		/* Otherwise, use ROW_MARK_COPY by default */
		return ROW_MARK_COPY;
	}
	else
	{
		/* Regular table, apply the appropriate lock type */
		switch (strength)
		{
			case LCS_NONE:

				/*
				 * We don't need a tuple lock, only the ability to re-fetch
				 * the row.  Regular tables support ROW_MARK_REFERENCE, but if
				 * this RTE has security barrier quals, it will be turned into
				 * a subquery during planning, so use ROW_MARK_COPY.
				 *
				 * This is only necessary for LCS_NONE, since real tuple locks
				 * on an RTE with security barrier quals are supported by
				 * pushing the lock down into the subquery --- see
				 * expand_security_qual.
				 */
				if (rte->securityQuals != NIL)
					return ROW_MARK_COPY;
				return ROW_MARK_REFERENCE;
				break;
			case LCS_FORKEYSHARE:
				return ROW_MARK_KEYSHARE;
				break;
			case LCS_FORSHARE:
				return ROW_MARK_SHARE;
				break;
			case LCS_FORNOKEYUPDATE:
				return ROW_MARK_NOKEYEXCLUSIVE;
				break;
			case LCS_FORUPDATE:
				return ROW_MARK_EXCLUSIVE;
				break;
		}
		elog(ERROR, "unrecognized LockClauseStrength %d", (int) strength);
		return ROW_MARK_EXCLUSIVE;		/* keep compiler quiet */
	}
}

/*
 * preprocess_limit - do pre-estimation for LIMIT and/or OFFSET clauses
 *
 * We try to estimate the values of the LIMIT/OFFSET clauses, and pass the
 * results back in *count_est and *offset_est.  These variables are set to
 * 0 if the corresponding clause is not present, and -1 if it's present
 * but we couldn't estimate the value for it.  (The "0" convention is OK
 * for OFFSET but a little bit bogus for LIMIT: effectively we estimate
 * LIMIT 0 as though it were LIMIT 1.  But this is in line with the planner's
 * usual practice of never estimating less than one row.)  These values will
 * be passed to create_limit_path, which see if you change this code.
 *
 * The return value is the suitably adjusted tuple_fraction to use for
 * planning the query.  This adjustment is not overridable, since it reflects
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
					*offset_est = 0;	/* treat as not present */
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
		 * We have an OFFSET but no LIMIT.  This acts entirely differently
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
		 * together; likewise if they are both fractional.  If one is
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
 * locution for an optimization fence.  (Because other places in the planner
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

				if (offset != 0)
					return true;	/* OFFSET with a nonzero value */
			}
		}
		else
			return true;		/* non-constant OFFSET */
	}

	return false;				/* don't need a Limit plan node */
}


/*
 * remove_useless_groupby_columns
 *		Remove any columns in the GROUP BY clause that are redundant due to
 *		being functionally dependent on other GROUP BY columns.
 *
 * Since some other DBMSes do not allow references to ungrouped columns, it's
 * not unusual to find all columns listed in GROUP BY even though listing the
 * primary-key columns would be sufficient.  Deleting such excess columns
 * avoids redundant sorting work, so it's worth doing.  When we do this, we
 * must mark the plan as dependent on the pkey constraint (compare the
 * parser's check_ungrouped_columns() and check_functional_grouping()).
 *
 * In principle, we could treat any NOT-NULL columns appearing in a UNIQUE
 * index as the determining columns.  But as with check_functional_grouping(),
 * there's currently no way to represent dependency on a NOT NULL constraint,
 * so we consider only the pkey for now.
 */
static void
remove_useless_groupby_columns(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	Bitmapset **groupbyattnos;
	Bitmapset **surplusvars;
	ListCell   *lc;
	int			relid;

	/* No chance to do anything if there are less than two GROUP BY items */
	if (list_length(parse->groupClause) < 2)
		return;

	/* Don't fiddle with the GROUP BY clause if the query has grouping sets */
	if (parse->groupingSets)
		return;

	/*
	 * Scan the GROUP BY clause to find GROUP BY items that are simple Vars.
	 * Fill groupbyattnos[k] with a bitmapset of the column attnos of RTE k
	 * that are GROUP BY items.
	 */
	groupbyattnos = (Bitmapset **) palloc0(sizeof(Bitmapset *) *
										   (list_length(parse->rtable) + 1));
	foreach(lc, parse->groupClause)
	{
		SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
		TargetEntry *tle = get_sortgroupclause_tle(sgc, parse->targetList);
		Var		   *var = (Var *) tle->expr;

		/*
		 * Ignore non-Vars and Vars from other query levels.
		 *
		 * XXX in principle, stable expressions containing Vars could also be
		 * removed, if all the Vars are functionally dependent on other GROUP
		 * BY items.  But it's not clear that such cases occur often enough to
		 * be worth troubling over.
		 */
		if (!IsA(var, Var) ||
			var->varlevelsup > 0)
			continue;

		/* OK, remember we have this Var */
		relid = var->varno;
		Assert(relid <= list_length(parse->rtable));
		groupbyattnos[relid] = bms_add_member(groupbyattnos[relid],
						 var->varattno - FirstLowInvalidHeapAttributeNumber);
	}

	/*
	 * Consider each relation and see if it is possible to remove some of its
	 * Vars from GROUP BY.  For simplicity and speed, we do the actual removal
	 * in a separate pass.  Here, we just fill surplusvars[k] with a bitmapset
	 * of the column attnos of RTE k that are removable GROUP BY items.
	 */
	surplusvars = NULL;			/* don't allocate array unless required */
	relid = 0;
	foreach(lc, parse->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);
		Bitmapset  *relattnos;
		Bitmapset  *pkattnos;
		Oid			constraintOid;

		relid++;

		/* Only plain relations could have primary-key constraints */
		if (rte->rtekind != RTE_RELATION)
			continue;

		/* Nothing to do unless this rel has multiple Vars in GROUP BY */
		relattnos = groupbyattnos[relid];
		if (bms_membership(relattnos) != BMS_MULTIPLE)
			continue;

		/*
		 * Can't remove any columns for this rel if there is no suitable
		 * (i.e., nondeferrable) primary key constraint.
		 */
		pkattnos = get_primary_key_attnos(rte->relid, false, &constraintOid);
		if (pkattnos == NULL)
			continue;

		/*
		 * If the primary key is a proper subset of relattnos then we have
		 * some items in the GROUP BY that can be removed.
		 */
		if (bms_subset_compare(pkattnos, relattnos) == BMS_SUBSET1)
		{
			/*
			 * To easily remember whether we've found anything to do, we don't
			 * allocate the surplusvars[] array until we find something.
			 */
			if (surplusvars == NULL)
				surplusvars = (Bitmapset **) palloc0(sizeof(Bitmapset *) *
										   (list_length(parse->rtable) + 1));

			/* Remember the attnos of the removable columns */
			surplusvars[relid] = bms_difference(relattnos, pkattnos);

			/* Also, mark the resulting plan as dependent on this constraint */
			parse->constraintDeps = lappend_oid(parse->constraintDeps,
												constraintOid);
		}
	}

	/*
	 * If we found any surplus Vars, build a new GROUP BY clause without them.
	 * (Note: this may leave some TLEs with unreferenced ressortgroupref
	 * markings, but that's harmless.)
	 */
	if (surplusvars != NULL)
	{
		List	   *new_groupby = NIL;

		foreach(lc, parse->groupClause)
		{
			SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
			TargetEntry *tle = get_sortgroupclause_tle(sgc, parse->targetList);
			Var		   *var = (Var *) tle->expr;

			/*
			 * New list must include non-Vars, outer Vars, and anything not
			 * marked as surplus.
			 */
			if (!IsA(var, Var) ||
				var->varlevelsup > 0 ||
			!bms_is_member(var->varattno - FirstLowInvalidHeapAttributeNumber,
						   surplusvars[var->varno]))
				new_groupby = lappend(new_groupby, sgc);
		}

		parse->groupClause = new_groupby;
	}
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
 *
 * For grouping sets, the order of items is instead forced to agree with that
 * of the grouping set (and items not in the grouping set are skipped). The
 * work of sorting the order of grouping set elements to match the ORDER BY if
 * possible is done elsewhere.
 */
static List *
preprocess_groupclause(PlannerInfo *root, List *force)
{
	Query	   *parse = root->parse;
	List	   *new_groupclause = NIL;
	bool		partial_match;
	ListCell   *sl;
	ListCell   *gl;

	/* For grouping sets, we need to force the ordering */
	if (force)
	{
		foreach(sl, force)
		{
			Index		ref = lfirst_int(sl);
			SortGroupClause *cl = get_sortgroupref_clause(ref, parse->groupClause);

			new_groupclause = lappend(new_groupclause, cl);
		}

		return new_groupclause;
	}

	/* If no ORDER BY, nothing useful to do here */
	if (parse->sortClause == NIL)
		return parse->groupClause;

	/*
	 * Scan the ORDER BY clause and construct a list of matching GROUP BY
	 * items, but only as far as we can make a matching prefix.
	 *
	 * This code assumes that the sortClause contains no duplicate items.
	 */
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
		return parse->groupClause;

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
			return parse->groupClause;	/* give up, no common sort possible */
		if (!OidIsValid(gc->sortop))
			return parse->groupClause;	/* give up, GROUP BY can't be sorted */
		new_groupclause = lappend(new_groupclause, gc);
	}

	/* Success --- install the rearranged GROUP BY list */
	Assert(list_length(parse->groupClause) == list_length(new_groupclause));
	return new_groupclause;
}

/*
 * Extract lists of grouping sets that can be implemented using a single
 * rollup-type aggregate pass each. Returns a list of lists of grouping sets.
 *
 * Input must be sorted with smallest sets first. Result has each sublist
 * sorted with smallest sets first.
 *
 * We want to produce the absolute minimum possible number of lists here to
 * avoid excess sorts. Fortunately, there is an algorithm for this; the problem
 * of finding the minimal partition of a partially-ordered set into chains
 * (which is what we need, taking the list of grouping sets as a poset ordered
 * by set inclusion) can be mapped to the problem of finding the maximum
 * cardinality matching on a bipartite graph, which is solvable in polynomial
 * time with a worst case of no worse than O(n^2.5) and usually much
 * better. Since our N is at most 4096, we don't need to consider fallbacks to
 * heuristic or approximate methods.  (Planning time for a 12-d cube is under
 * half a second on my modest system even with optimization off and assertions
 * on.)
 */
static List *
extract_rollup_sets(List *groupingSets)
{
	int			num_sets_raw = list_length(groupingSets);
	int			num_empty = 0;
	int			num_sets = 0;	/* distinct sets */
	int			num_chains = 0;
	List	   *result = NIL;
	List	  **results;
	List	  **orig_sets;
	Bitmapset **set_masks;
	int		   *chains;
	short	  **adjacency;
	short	   *adjacency_buf;
	BipartiteMatchState *state;
	int			i;
	int			j;
	int			j_size;
	ListCell   *lc1 = list_head(groupingSets);
	ListCell   *lc;

	/*
	 * Start by stripping out empty sets.  The algorithm doesn't require this,
	 * but the planner currently needs all empty sets to be returned in the
	 * first list, so we strip them here and add them back after.
	 */
	while (lc1 && lfirst(lc1) == NIL)
	{
		++num_empty;
		lc1 = lnext(lc1);
	}

	/* bail out now if it turns out that all we had were empty sets. */
	if (!lc1)
		return list_make1(groupingSets);

	/*----------
	 * We don't strictly need to remove duplicate sets here, but if we don't,
	 * they tend to become scattered through the result, which is a bit
	 * confusing (and irritating if we ever decide to optimize them out).
	 * So we remove them here and add them back after.
	 *
	 * For each non-duplicate set, we fill in the following:
	 *
	 * orig_sets[i] = list of the original set lists
	 * set_masks[i] = bitmapset for testing inclusion
	 * adjacency[i] = array [n, v1, v2, ... vn] of adjacency indices
	 *
	 * chains[i] will be the result group this set is assigned to.
	 *
	 * We index all of these from 1 rather than 0 because it is convenient
	 * to leave 0 free for the NIL node in the graph algorithm.
	 *----------
	 */
	orig_sets = palloc0((num_sets_raw + 1) * sizeof(List *));
	set_masks = palloc0((num_sets_raw + 1) * sizeof(Bitmapset *));
	adjacency = palloc0((num_sets_raw + 1) * sizeof(short *));
	adjacency_buf = palloc((num_sets_raw + 1) * sizeof(short));

	j_size = 0;
	j = 0;
	i = 1;

	for_each_cell(lc, lc1)
	{
		List	   *candidate = lfirst(lc);
		Bitmapset  *candidate_set = NULL;
		ListCell   *lc2;
		int			dup_of = 0;

		foreach(lc2, candidate)
		{
			candidate_set = bms_add_member(candidate_set, lfirst_int(lc2));
		}

		/* we can only be a dup if we're the same length as a previous set */
		if (j_size == list_length(candidate))
		{
			int			k;

			for (k = j; k < i; ++k)
			{
				if (bms_equal(set_masks[k], candidate_set))
				{
					dup_of = k;
					break;
				}
			}
		}
		else if (j_size < list_length(candidate))
		{
			j_size = list_length(candidate);
			j = i;
		}

		if (dup_of > 0)
		{
			orig_sets[dup_of] = lappend(orig_sets[dup_of], candidate);
			bms_free(candidate_set);
		}
		else
		{
			int			k;
			int			n_adj = 0;

			orig_sets[i] = list_make1(candidate);
			set_masks[i] = candidate_set;

			/* fill in adjacency list; no need to compare equal-size sets */

			for (k = j - 1; k > 0; --k)
			{
				if (bms_is_subset(set_masks[k], candidate_set))
					adjacency_buf[++n_adj] = k;
			}

			if (n_adj > 0)
			{
				adjacency_buf[0] = n_adj;
				adjacency[i] = palloc((n_adj + 1) * sizeof(short));
				memcpy(adjacency[i], adjacency_buf, (n_adj + 1) * sizeof(short));
			}
			else
				adjacency[i] = NULL;

			++i;
		}
	}

	num_sets = i - 1;

	/*
	 * Apply the graph matching algorithm to do the work.
	 */
	state = BipartiteMatch(num_sets, num_sets, adjacency);

	/*
	 * Now, the state->pair* fields have the info we need to assign sets to
	 * chains. Two sets (u,v) belong to the same chain if pair_uv[u] = v or
	 * pair_vu[v] = u (both will be true, but we check both so that we can do
	 * it in one pass)
	 */
	chains = palloc0((num_sets + 1) * sizeof(int));

	for (i = 1; i <= num_sets; ++i)
	{
		int			u = state->pair_vu[i];
		int			v = state->pair_uv[i];

		if (u > 0 && u < i)
			chains[i] = chains[u];
		else if (v > 0 && v < i)
			chains[i] = chains[v];
		else
			chains[i] = ++num_chains;
	}

	/* build result lists. */
	results = palloc0((num_chains + 1) * sizeof(List *));

	for (i = 1; i <= num_sets; ++i)
	{
		int			c = chains[i];

		Assert(c > 0);

		results[c] = list_concat(results[c], orig_sets[i]);
	}

	/* push any empty sets back on the first list. */
	while (num_empty-- > 0)
		results[1] = lcons(NIL, results[1]);

	/* make result list */
	for (i = 1; i <= num_chains; ++i)
		result = lappend(result, results[i]);

	/*
	 * Free all the things.
	 *
	 * (This is over-fussy for small sets but for large sets we could have
	 * tied up a nontrivial amount of memory.)
	 */
	BipartiteMatchFree(state);
	pfree(results);
	pfree(chains);
	for (i = 1; i <= num_sets; ++i)
		if (adjacency[i])
			pfree(adjacency[i]);
	pfree(adjacency);
	pfree(adjacency_buf);
	pfree(orig_sets);
	for (i = 1; i <= num_sets; ++i)
		bms_free(set_masks[i]);
	pfree(set_masks);

	return result;
}

/*
 * Reorder the elements of a list of grouping sets such that they have correct
 * prefix relationships.
 *
 * The input must be ordered with smallest sets first; the result is returned
 * with largest sets first.  Note that the result shares no list substructure
 * with the input, so it's safe for the caller to modify it later.
 *
 * If we're passed in a sortclause, we follow its order of columns to the
 * extent possible, to minimize the chance that we add unnecessary sorts.
 * (We're trying here to ensure that GROUPING SETS ((a,b,c),(c)) ORDER BY c,b,a
 * gets implemented in one pass.)
 */
static List *
reorder_grouping_sets(List *groupingsets, List *sortclause)
{
	ListCell   *lc;
	ListCell   *lc2;
	List	   *previous = NIL;
	List	   *result = NIL;

	foreach(lc, groupingsets)
	{
		List	   *candidate = lfirst(lc);
		List	   *new_elems = list_difference_int(candidate, previous);

		if (list_length(new_elems) > 0)
		{
			while (list_length(sortclause) > list_length(previous))
			{
				SortGroupClause *sc = list_nth(sortclause, list_length(previous));
				int			ref = sc->tleSortGroupRef;

				if (list_member_int(new_elems, ref))
				{
					previous = lappend_int(previous, ref);
					new_elems = list_delete_int(new_elems, ref);
				}
				else
				{
					/* diverged from the sortclause; give up on it */
					sortclause = NIL;
					break;
				}
			}

			foreach(lc2, new_elems)
			{
				previous = lappend_int(previous, lfirst_int(lc2));
			}
		}

		result = lcons(list_copy(previous), result);
		list_free(new_elems);
	}

	list_free(previous);

	return result;
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
	if (qp_extra->groupClause &&
		grouping_is_sortable(qp_extra->groupClause))
		root->group_pathkeys =
			make_pathkeys_for_sortclauses(root,
										  qp_extra->groupClause,
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
 * Estimate number of groups produced by grouping clauses (1 if not grouping)
 *
 * path_rows: number of output rows from scan/join step
 * rollup_lists: list of grouping sets, or NIL if not doing grouping sets
 * rollup_groupclauses: list of grouping clauses for grouping sets,
 *		or NIL if not doing grouping sets
 */
static double
get_number_of_groups(PlannerInfo *root,
					 double path_rows,
					 List *rollup_lists,
					 List *rollup_groupclauses)
{
	Query	   *parse = root->parse;
	double		dNumGroups;

	if (parse->groupClause)
	{
		List	   *groupExprs;

		if (parse->groupingSets)
		{
			/* Add up the estimates for each grouping set */
			ListCell   *lc,
					   *lc2;

			dNumGroups = 0;
			forboth(lc, rollup_groupclauses, lc2, rollup_lists)
			{
				List	   *groupClause = (List *) lfirst(lc);
				List	   *gsets = (List *) lfirst(lc2);
				ListCell   *lc3;

				groupExprs = get_sortgrouplist_exprs(groupClause,
													 parse->targetList);

				foreach(lc3, gsets)
				{
					List	   *gset = (List *) lfirst(lc3);

					dNumGroups += estimate_num_groups(root,
													  groupExprs,
													  path_rows,
													  &gset);
				}
			}
		}
		else
		{
			/* Plain GROUP BY */
			groupExprs = get_sortgrouplist_exprs(parse->groupClause,
												 parse->targetList);

			dNumGroups = estimate_num_groups(root, groupExprs, path_rows,
											 NULL);
		}
	}
	else if (parse->groupingSets)
	{
		/* Empty grouping sets ... one result row for each one */
		dNumGroups = list_length(parse->groupingSets);
	}
	else if (parse->hasAggs || root->hasHavingQual)
	{
		/* Plain aggregation, one result row */
		dNumGroups = 1;
	}
	else
	{
		/* Not grouping */
		dNumGroups = 1;
	}

	return dNumGroups;
}

/*
 * estimate_hashagg_tablesize
 *	  estimate the number of bytes that a hash aggregate hashtable will
 *	  require based on the agg_costs, path width and dNumGroups.
 */
static Size
estimate_hashagg_tablesize(Path *path, const AggClauseCosts *agg_costs,
						   double dNumGroups)
{
	Size		hashentrysize;

	/* Estimate per-hash-entry space at tuple width... */
	hashentrysize = MAXALIGN(path->pathtarget->width) +
		MAXALIGN(SizeofMinimalTupleHeader);

	/* plus space for pass-by-ref transition values... */
	hashentrysize += agg_costs->transitionSpace;
	/* plus the per-hash-entry overhead */
	hashentrysize += hash_agg_entry_size(agg_costs->numAggs);

	return hashentrysize * dNumGroups;
}

/*
 * create_grouping_paths
 *
 * Build a new upperrel containing Paths for grouping and/or aggregation.
 *
 * input_rel: contains the source-data Paths
 * target: the pathtarget for the result Paths to compute
 * agg_costs: cost info about all aggregates in query (in AGGSPLIT_SIMPLE mode)
 * rollup_lists: list of grouping sets, or NIL if not doing grouping sets
 * rollup_groupclauses: list of grouping clauses for grouping sets,
 *		or NIL if not doing grouping sets
 *
 * Note: all Paths in input_rel are expected to return the target computed
 * by make_group_input_target.
 *
 * We need to consider sorted and hashed aggregation in the same function,
 * because otherwise (1) it would be harder to throw an appropriate error
 * message if neither way works, and (2) we should not allow hashtable size
 * considerations to dissuade us from using hashing if sorting is not possible.
 */
static RelOptInfo *
create_grouping_paths(PlannerInfo *root,
					  RelOptInfo *input_rel,
					  PathTarget *target,
					  const AggClauseCosts *agg_costs,
					  List *rollup_lists,
					  List *rollup_groupclauses)
{
	Query	   *parse = root->parse;
	Path	   *cheapest_path = input_rel->cheapest_total_path;
	RelOptInfo *grouped_rel;
	PathTarget *partial_grouping_target = NULL;
	AggClauseCosts agg_partial_costs;	/* parallel only */
	AggClauseCosts agg_final_costs;		/* parallel only */
	Size		hashaggtablesize;
	double		dNumGroups;
	double		dNumPartialGroups = 0;
	bool		can_hash;
	bool		can_sort;
	bool		try_parallel_aggregation;

	ListCell   *lc;

	/* For now, do all work in the (GROUP_AGG, NULL) upperrel */
	grouped_rel = fetch_upper_rel(root, UPPERREL_GROUP_AGG, NULL);

	/*
	 * If the input relation is not parallel-safe, then the grouped relation
	 * can't be parallel-safe, either.  Otherwise, it's parallel-safe if the
	 * target list and HAVING quals are parallel-safe.
	 */
	if (input_rel->consider_parallel &&
		is_parallel_safe(root, (Node *) target->exprs) &&
		is_parallel_safe(root, (Node *) parse->havingQual))
		grouped_rel->consider_parallel = true;

	/*
	 * If the input rel belongs to a single FDW, so does the grouped rel.
	 */
	grouped_rel->serverid = input_rel->serverid;
	grouped_rel->userid = input_rel->userid;
	grouped_rel->useridiscurrent = input_rel->useridiscurrent;
	grouped_rel->fdwroutine = input_rel->fdwroutine;

	/*
	 * Check for degenerate grouping.
	 */
	if ((root->hasHavingQual || parse->groupingSets) &&
		!parse->hasAggs && parse->groupClause == NIL)
	{
		/*
		 * We have a HAVING qual and/or grouping sets, but no aggregates and
		 * no GROUP BY (which implies that the grouping sets are all empty).
		 *
		 * This is a degenerate case in which we are supposed to emit either
		 * zero or one row for each grouping set depending on whether HAVING
		 * succeeds.  Furthermore, there cannot be any variables in either
		 * HAVING or the targetlist, so we actually do not need the FROM table
		 * at all!	We can just throw away the plan-so-far and generate a
		 * Result node.  This is a sufficiently unusual corner case that it's
		 * not worth contorting the structure of this module to avoid having
		 * to generate the earlier paths in the first place.
		 */
		int			nrows = list_length(parse->groupingSets);
		Path	   *path;

		if (nrows > 1)
		{
			/*
			 * Doesn't seem worthwhile writing code to cons up a
			 * generate_series or a values scan to emit multiple rows. Instead
			 * just make N clones and append them.  (With a volatile HAVING
			 * clause, this means you might get between 0 and N output rows.
			 * Offhand I think that's desired.)
			 */
			List	   *paths = NIL;

			while (--nrows >= 0)
			{
				path = (Path *)
					create_result_path(root, grouped_rel,
									   target,
									   (List *) parse->havingQual);
				paths = lappend(paths, path);
			}
			path = (Path *)
				create_append_path(grouped_rel,
								   paths,
								   NULL,
								   0);
			path->pathtarget = target;
		}
		else
		{
			/* No grouping sets, or just one, so one output row */
			path = (Path *)
				create_result_path(root, grouped_rel,
								   target,
								   (List *) parse->havingQual);
		}

		add_path(grouped_rel, path);

		/* No need to consider any other alternatives. */
		set_cheapest(grouped_rel);

		return grouped_rel;
	}

	/*
	 * Estimate number of groups.
	 */
	dNumGroups = get_number_of_groups(root,
									  cheapest_path->rows,
									  rollup_lists,
									  rollup_groupclauses);

	/*
	 * Determine whether it's possible to perform sort-based implementations
	 * of grouping.  (Note that if groupClause is empty,
	 * grouping_is_sortable() is trivially true, and all the
	 * pathkeys_contained_in() tests will succeed too, so that we'll consider
	 * every surviving input path.)
	 */
	can_sort = grouping_is_sortable(parse->groupClause);

	/*
	 * Determine whether we should consider hash-based implementations of
	 * grouping.
	 *
	 * Hashed aggregation only applies if we're grouping.  We currently can't
	 * hash if there are grouping sets, though.
	 *
	 * Executor doesn't support hashed aggregation with DISTINCT or ORDER BY
	 * aggregates.  (Doing so would imply storing *all* the input values in
	 * the hash table, and/or running many sorts in parallel, either of which
	 * seems like a certain loser.)  We similarly don't support ordered-set
	 * aggregates in hashed aggregation, but that case is also included in the
	 * numOrderedAggs count.
	 *
	 * Note: grouping_is_hashable() is much more expensive to check than the
	 * other gating conditions, so we want to do it last.
	 */
	can_hash = (parse->groupClause != NIL &&
				parse->groupingSets == NIL &&
				agg_costs->numOrderedAggs == 0 &&
				grouping_is_hashable(parse->groupClause));

	/*
	 * If grouped_rel->consider_parallel is true, then paths that we generate
	 * for this grouping relation could be run inside of a worker, but that
	 * doesn't mean we can actually use the PartialAggregate/FinalizeAggregate
	 * execution strategy.  Figure that out.
	 */
	if (!grouped_rel->consider_parallel)
	{
		/* Not even parallel-safe. */
		try_parallel_aggregation = false;
	}
	else if (input_rel->partial_pathlist == NIL)
	{
		/* Nothing to use as input for partial aggregate. */
		try_parallel_aggregation = false;
	}
	else if (!parse->hasAggs && parse->groupClause == NIL)
	{
		/*
		 * We don't know how to do parallel aggregation unless we have either
		 * some aggregates or a grouping clause.
		 */
		try_parallel_aggregation = false;
	}
	else if (parse->groupingSets)
	{
		/* We don't know how to do grouping sets in parallel. */
		try_parallel_aggregation = false;
	}
	else if (agg_costs->hasNonPartial || agg_costs->hasNonSerial)
	{
		/* Insufficient support for partial mode. */
		try_parallel_aggregation = false;
	}
	else
	{
		/* Everything looks good. */
		try_parallel_aggregation = true;
	}

	/*
	 * Before generating paths for grouped_rel, we first generate any possible
	 * partial paths; that way, later code can easily consider both parallel
	 * and non-parallel approaches to grouping.  Note that the partial paths
	 * we generate here are also partially aggregated, so simply pushing a
	 * Gather node on top is insufficient to create a final path, as would be
	 * the case for a scan/join rel.
	 */
	if (try_parallel_aggregation)
	{
		Path	   *cheapest_partial_path = linitial(input_rel->partial_pathlist);

		/*
		 * Build target list for partial aggregate paths.  These paths cannot
		 * just emit the same tlist as regular aggregate paths, because (1) we
		 * must include Vars and Aggrefs needed in HAVING, which might not
		 * appear in the result tlist, and (2) the Aggrefs must be set in
		 * partial mode.
		 */
		partial_grouping_target = make_partial_grouping_target(root, target);

		/* Estimate number of partial groups. */
		dNumPartialGroups = get_number_of_groups(root,
												 cheapest_partial_path->rows,
												 NIL,
												 NIL);

		/*
		 * Collect statistics about aggregates for estimating costs of
		 * performing aggregation in parallel.
		 */
		MemSet(&agg_partial_costs, 0, sizeof(AggClauseCosts));
		MemSet(&agg_final_costs, 0, sizeof(AggClauseCosts));
		if (parse->hasAggs)
		{
			/* partial phase */
			get_agg_clause_costs(root, (Node *) partial_grouping_target->exprs,
								 AGGSPLIT_INITIAL_SERIAL,
								 &agg_partial_costs);

			/* final phase */
			get_agg_clause_costs(root, (Node *) target->exprs,
								 AGGSPLIT_FINAL_DESERIAL,
								 &agg_final_costs);
			get_agg_clause_costs(root, parse->havingQual,
								 AGGSPLIT_FINAL_DESERIAL,
								 &agg_final_costs);
		}

		if (can_sort)
		{
			/* This was checked before setting try_parallel_aggregation */
			Assert(parse->hasAggs || parse->groupClause);

			/*
			 * Use any available suitably-sorted path as input, and also
			 * consider sorting the cheapest partial path.
			 */
			foreach(lc, input_rel->partial_pathlist)
			{
				Path	   *path = (Path *) lfirst(lc);
				bool		is_sorted;

				is_sorted = pathkeys_contained_in(root->group_pathkeys,
												  path->pathkeys);
				if (path == cheapest_partial_path || is_sorted)
				{
					/* Sort the cheapest partial path, if it isn't already */
					if (!is_sorted)
						path = (Path *) create_sort_path(root,
														 grouped_rel,
														 path,
														 root->group_pathkeys,
														 -1.0);

					if (parse->hasAggs)
						add_partial_path(grouped_rel, (Path *)
										 create_agg_path(root,
														 grouped_rel,
														 path,
													 partial_grouping_target,
								 parse->groupClause ? AGG_SORTED : AGG_PLAIN,
													 AGGSPLIT_INITIAL_SERIAL,
														 parse->groupClause,
														 NIL,
														 &agg_partial_costs,
														 dNumPartialGroups));
					else
						add_partial_path(grouped_rel, (Path *)
										 create_group_path(root,
														   grouped_rel,
														   path,
													 partial_grouping_target,
														   parse->groupClause,
														   NIL,
														 dNumPartialGroups));
				}
			}
		}

		if (can_hash)
		{
			/* Checked above */
			Assert(parse->hasAggs || parse->groupClause);

			hashaggtablesize =
				estimate_hashagg_tablesize(cheapest_partial_path,
										   &agg_partial_costs,
										   dNumPartialGroups);

			/*
			 * Tentatively produce a partial HashAgg Path, depending on if it
			 * looks as if the hash table will fit in work_mem.
			 */
			if (hashaggtablesize < work_mem * 1024L)
			{
				add_partial_path(grouped_rel, (Path *)
								 create_agg_path(root,
												 grouped_rel,
												 cheapest_partial_path,
												 partial_grouping_target,
												 AGG_HASHED,
												 AGGSPLIT_INITIAL_SERIAL,
												 parse->groupClause,
												 NIL,
												 &agg_partial_costs,
												 dNumPartialGroups));
			}
		}
	}

	/* Build final grouping paths */
	if (can_sort)
	{
		/*
		 * Use any available suitably-sorted path as input, and also consider
		 * sorting the cheapest-total path.
		 */
		foreach(lc, input_rel->pathlist)
		{
			Path	   *path = (Path *) lfirst(lc);
			bool		is_sorted;

			is_sorted = pathkeys_contained_in(root->group_pathkeys,
											  path->pathkeys);
			if (path == cheapest_path || is_sorted)
			{
				/* Sort the cheapest-total path if it isn't already sorted */
				if (!is_sorted)
					path = (Path *) create_sort_path(root,
													 grouped_rel,
													 path,
													 root->group_pathkeys,
													 -1.0);

				/* Now decide what to stick atop it */
				if (parse->groupingSets)
				{
					/*
					 * We have grouping sets, possibly with aggregation.  Make
					 * a GroupingSetsPath.
					 */
					add_path(grouped_rel, (Path *)
							 create_groupingsets_path(root,
													  grouped_rel,
													  path,
													  target,
												  (List *) parse->havingQual,
													  rollup_lists,
													  rollup_groupclauses,
													  agg_costs,
													  dNumGroups));
				}
				else if (parse->hasAggs)
				{
					/*
					 * We have aggregation, possibly with plain GROUP BY. Make
					 * an AggPath.
					 */
					add_path(grouped_rel, (Path *)
							 create_agg_path(root,
											 grouped_rel,
											 path,
											 target,
								 parse->groupClause ? AGG_SORTED : AGG_PLAIN,
											 AGGSPLIT_SIMPLE,
											 parse->groupClause,
											 (List *) parse->havingQual,
											 agg_costs,
											 dNumGroups));
				}
				else if (parse->groupClause)
				{
					/*
					 * We have GROUP BY without aggregation or grouping sets.
					 * Make a GroupPath.
					 */
					add_path(grouped_rel, (Path *)
							 create_group_path(root,
											   grouped_rel,
											   path,
											   target,
											   parse->groupClause,
											   (List *) parse->havingQual,
											   dNumGroups));
				}
				else
				{
					/* Other cases should have been handled above */
					Assert(false);
				}
			}
		}

		/*
		 * Now generate a complete GroupAgg Path atop of the cheapest partial
		 * path. We need only bother with the cheapest path here, as the
		 * output of Gather is never sorted.
		 */
		if (grouped_rel->partial_pathlist)
		{
			Path	   *path = (Path *) linitial(grouped_rel->partial_pathlist);
			double		total_groups = path->rows * path->parallel_workers;

			path = (Path *) create_gather_path(root,
											   grouped_rel,
											   path,
											   partial_grouping_target,
											   NULL,
											   &total_groups);

			/*
			 * Gather is always unsorted, so we'll need to sort, unless
			 * there's no GROUP BY clause, in which case there will only be a
			 * single group.
			 */
			if (parse->groupClause)
				path = (Path *) create_sort_path(root,
												 grouped_rel,
												 path,
												 root->group_pathkeys,
												 -1.0);

			if (parse->hasAggs)
				add_path(grouped_rel, (Path *)
						 create_agg_path(root,
										 grouped_rel,
										 path,
										 target,
								 parse->groupClause ? AGG_SORTED : AGG_PLAIN,
										 AGGSPLIT_FINAL_DESERIAL,
										 parse->groupClause,
										 (List *) parse->havingQual,
										 &agg_final_costs,
										 dNumGroups));
			else
				add_path(grouped_rel, (Path *)
						 create_group_path(root,
										   grouped_rel,
										   path,
										   target,
										   parse->groupClause,
										   (List *) parse->havingQual,
										   dNumGroups));
		}
	}

	if (can_hash)
	{
		hashaggtablesize = estimate_hashagg_tablesize(cheapest_path,
													  agg_costs,
													  dNumGroups);

		/*
		 * Provided that the estimated size of the hashtable does not exceed
		 * work_mem, we'll generate a HashAgg Path, although if we were unable
		 * to sort above, then we'd better generate a Path, so that we at
		 * least have one.
		 */
		if (hashaggtablesize < work_mem * 1024L ||
			grouped_rel->pathlist == NIL)
		{
			/*
			 * We just need an Agg over the cheapest-total input path, since
			 * input order won't matter.
			 */
			add_path(grouped_rel, (Path *)
					 create_agg_path(root, grouped_rel,
									 cheapest_path,
									 target,
									 AGG_HASHED,
									 AGGSPLIT_SIMPLE,
									 parse->groupClause,
									 (List *) parse->havingQual,
									 agg_costs,
									 dNumGroups));
		}

		/*
		 * Generate a HashAgg Path atop of the cheapest partial path. Once
		 * again, we'll only do this if it looks as though the hash table
		 * won't exceed work_mem.
		 */
		if (grouped_rel->partial_pathlist)
		{
			Path	   *path = (Path *) linitial(grouped_rel->partial_pathlist);

			hashaggtablesize = estimate_hashagg_tablesize(path,
														  &agg_final_costs,
														  dNumGroups);

			if (hashaggtablesize < work_mem * 1024L)
			{
				double		total_groups = path->rows * path->parallel_workers;

				path = (Path *) create_gather_path(root,
												   grouped_rel,
												   path,
												   partial_grouping_target,
												   NULL,
												   &total_groups);

				add_path(grouped_rel, (Path *)
						 create_agg_path(root,
										 grouped_rel,
										 path,
										 target,
										 AGG_HASHED,
										 AGGSPLIT_FINAL_DESERIAL,
										 parse->groupClause,
										 (List *) parse->havingQual,
										 &agg_final_costs,
										 dNumGroups));
			}
		}
	}

	/* Give a helpful error if we failed to find any implementation */
	if (grouped_rel->pathlist == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not implement GROUP BY"),
				 errdetail("Some of the datatypes only support hashing, while others only support sorting.")));

	/*
	 * If there is an FDW that's responsible for all baserels of the query,
	 * let it consider adding ForeignPaths.
	 */
	if (grouped_rel->fdwroutine &&
		grouped_rel->fdwroutine->GetForeignUpperPaths)
		grouped_rel->fdwroutine->GetForeignUpperPaths(root, UPPERREL_GROUP_AGG,
													  input_rel, grouped_rel);

	/* Let extensions possibly add some more paths */
	if (create_upper_paths_hook)
		(*create_upper_paths_hook) (root, UPPERREL_GROUP_AGG,
									input_rel, grouped_rel);

	/* Now choose the best path(s) */
	set_cheapest(grouped_rel);

	return grouped_rel;
}

/*
 * create_window_paths
 *
 * Build a new upperrel containing Paths for window-function evaluation.
 *
 * input_rel: contains the source-data Paths
 * input_target: result of make_window_input_target
 * output_target: what the topmost WindowAggPath should return
 * tlist: query's target list (needed to look up pathkeys)
 * wflists: result of find_window_functions
 * activeWindows: result of select_active_windows
 *
 * Note: all Paths in input_rel are expected to return input_target.
 */
static RelOptInfo *
create_window_paths(PlannerInfo *root,
					RelOptInfo *input_rel,
					PathTarget *input_target,
					PathTarget *output_target,
					List *tlist,
					WindowFuncLists *wflists,
					List *activeWindows)
{
	RelOptInfo *window_rel;
	ListCell   *lc;

	/* For now, do all work in the (WINDOW, NULL) upperrel */
	window_rel = fetch_upper_rel(root, UPPERREL_WINDOW, NULL);

	/*
	 * If the input relation is not parallel-safe, then the window relation
	 * can't be parallel-safe, either.  Otherwise, we need to examine the
	 * target list and active windows for non-parallel-safe constructs.
	 */
	if (input_rel->consider_parallel &&
		is_parallel_safe(root, (Node *) output_target->exprs) &&
		is_parallel_safe(root, (Node *) activeWindows))
		window_rel->consider_parallel = true;

	/*
	 * If the input rel belongs to a single FDW, so does the window rel.
	 */
	window_rel->serverid = input_rel->serverid;
	window_rel->userid = input_rel->userid;
	window_rel->useridiscurrent = input_rel->useridiscurrent;
	window_rel->fdwroutine = input_rel->fdwroutine;

	/*
	 * Consider computing window functions starting from the existing
	 * cheapest-total path (which will likely require a sort) as well as any
	 * existing paths that satisfy root->window_pathkeys (which won't).
	 */
	foreach(lc, input_rel->pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);

		if (path == input_rel->cheapest_total_path ||
			pathkeys_contained_in(root->window_pathkeys, path->pathkeys))
			create_one_window_path(root,
								   window_rel,
								   path,
								   input_target,
								   output_target,
								   tlist,
								   wflists,
								   activeWindows);
	}

	/*
	 * If there is an FDW that's responsible for all baserels of the query,
	 * let it consider adding ForeignPaths.
	 */
	if (window_rel->fdwroutine &&
		window_rel->fdwroutine->GetForeignUpperPaths)
		window_rel->fdwroutine->GetForeignUpperPaths(root, UPPERREL_WINDOW,
													 input_rel, window_rel);

	/* Let extensions possibly add some more paths */
	if (create_upper_paths_hook)
		(*create_upper_paths_hook) (root, UPPERREL_WINDOW,
									input_rel, window_rel);

	/* Now choose the best path(s) */
	set_cheapest(window_rel);

	return window_rel;
}

/*
 * Stack window-function implementation steps atop the given Path, and
 * add the result to window_rel.
 *
 * window_rel: upperrel to contain result
 * path: input Path to use (must return input_target)
 * input_target: result of make_window_input_target
 * output_target: what the topmost WindowAggPath should return
 * tlist: query's target list (needed to look up pathkeys)
 * wflists: result of find_window_functions
 * activeWindows: result of select_active_windows
 */
static void
create_one_window_path(PlannerInfo *root,
					   RelOptInfo *window_rel,
					   Path *path,
					   PathTarget *input_target,
					   PathTarget *output_target,
					   List *tlist,
					   WindowFuncLists *wflists,
					   List *activeWindows)
{
	PathTarget *window_target;
	ListCell   *l;

	/*
	 * Since each window clause could require a different sort order, we stack
	 * up a WindowAgg node for each clause, with sort steps between them as
	 * needed.  (We assume that select_active_windows chose a good order for
	 * executing the clauses in.)
	 *
	 * input_target should contain all Vars and Aggs needed for the result.
	 * (In some cases we wouldn't need to propagate all of these all the way
	 * to the top, since they might only be needed as inputs to WindowFuncs.
	 * It's probably not worth trying to optimize that though.)  It must also
	 * contain all window partitioning and sorting expressions, to ensure
	 * they're computed only once at the bottom of the stack (that's critical
	 * for volatile functions).  As we climb up the stack, we'll add outputs
	 * for the WindowFuncs computed at each level.
	 */
	window_target = input_target;

	foreach(l, activeWindows)
	{
		WindowClause *wc = (WindowClause *) lfirst(l);
		List	   *window_pathkeys;

		window_pathkeys = make_pathkeys_for_window(root,
												   wc,
												   tlist);

		/* Sort if necessary */
		if (!pathkeys_contained_in(window_pathkeys, path->pathkeys))
		{
			path = (Path *) create_sort_path(root, window_rel,
											 path,
											 window_pathkeys,
											 -1.0);
		}

		if (lnext(l))
		{
			/*
			 * Add the current WindowFuncs to the output target for this
			 * intermediate WindowAggPath.  We must copy window_target to
			 * avoid changing the previous path's target.
			 *
			 * Note: a WindowFunc adds nothing to the target's eval costs; but
			 * we do need to account for the increase in tlist width.
			 */
			ListCell   *lc2;

			window_target = copy_pathtarget(window_target);
			foreach(lc2, wflists->windowFuncs[wc->winref])
			{
				WindowFunc *wfunc = (WindowFunc *) lfirst(lc2);

				Assert(IsA(wfunc, WindowFunc));
				add_column_to_pathtarget(window_target, (Expr *) wfunc, 0);
				window_target->width += get_typavgwidth(wfunc->wintype, -1);
			}
		}
		else
		{
			/* Install the goal target in the topmost WindowAgg */
			window_target = output_target;
		}

		path = (Path *)
			create_windowagg_path(root, window_rel, path, window_target,
								  wflists->windowFuncs[wc->winref],
								  wc,
								  window_pathkeys);
	}

	add_path(window_rel, path);
}

/*
 * create_distinct_paths
 *
 * Build a new upperrel containing Paths for SELECT DISTINCT evaluation.
 *
 * input_rel: contains the source-data Paths
 *
 * Note: input paths should already compute the desired pathtarget, since
 * Sort/Unique won't project anything.
 */
static RelOptInfo *
create_distinct_paths(PlannerInfo *root,
					  RelOptInfo *input_rel)
{
	Query	   *parse = root->parse;
	Path	   *cheapest_input_path = input_rel->cheapest_total_path;
	RelOptInfo *distinct_rel;
	double		numDistinctRows;
	bool		allow_hash;
	Path	   *path;
	ListCell   *lc;

	/* For now, do all work in the (DISTINCT, NULL) upperrel */
	distinct_rel = fetch_upper_rel(root, UPPERREL_DISTINCT, NULL);

	/*
	 * We don't compute anything at this level, so distinct_rel will be
	 * parallel-safe if the input rel is parallel-safe.  In particular, if
	 * there is a DISTINCT ON (...) clause, any path for the input_rel will
	 * output those expressions, and will not be parallel-safe unless those
	 * expressions are parallel-safe.
	 */
	distinct_rel->consider_parallel = input_rel->consider_parallel;

	/*
	 * If the input rel belongs to a single FDW, so does the distinct_rel.
	 */
	distinct_rel->serverid = input_rel->serverid;
	distinct_rel->userid = input_rel->userid;
	distinct_rel->useridiscurrent = input_rel->useridiscurrent;
	distinct_rel->fdwroutine = input_rel->fdwroutine;

	/* Estimate number of distinct rows there will be */
	if (parse->groupClause || parse->groupingSets || parse->hasAggs ||
		root->hasHavingQual)
	{
		/*
		 * If there was grouping or aggregation, use the number of input rows
		 * as the estimated number of DISTINCT rows (ie, assume the input is
		 * already mostly unique).
		 */
		numDistinctRows = cheapest_input_path->rows;
	}
	else
	{
		/*
		 * Otherwise, the UNIQUE filter has effects comparable to GROUP BY.
		 */
		List	   *distinctExprs;

		distinctExprs = get_sortgrouplist_exprs(parse->distinctClause,
												parse->targetList);
		numDistinctRows = estimate_num_groups(root, distinctExprs,
											  cheapest_input_path->rows,
											  NULL);
	}

	/*
	 * Consider sort-based implementations of DISTINCT, if possible.
	 */
	if (grouping_is_sortable(parse->distinctClause))
	{
		/*
		 * First, if we have any adequately-presorted paths, just stick a
		 * Unique node on those.  Then consider doing an explicit sort of the
		 * cheapest input path and Unique'ing that.
		 *
		 * When we have DISTINCT ON, we must sort by the more rigorous of
		 * DISTINCT and ORDER BY, else it won't have the desired behavior.
		 * Also, if we do have to do an explicit sort, we might as well use
		 * the more rigorous ordering to avoid a second sort later.  (Note
		 * that the parser will have ensured that one clause is a prefix of
		 * the other.)
		 */
		List	   *needed_pathkeys;

		if (parse->hasDistinctOn &&
			list_length(root->distinct_pathkeys) <
			list_length(root->sort_pathkeys))
			needed_pathkeys = root->sort_pathkeys;
		else
			needed_pathkeys = root->distinct_pathkeys;

		foreach(lc, input_rel->pathlist)
		{
			Path	   *path = (Path *) lfirst(lc);

			if (pathkeys_contained_in(needed_pathkeys, path->pathkeys))
			{
				add_path(distinct_rel, (Path *)
						 create_upper_unique_path(root, distinct_rel,
												  path,
										list_length(root->distinct_pathkeys),
												  numDistinctRows));
			}
		}

		/* For explicit-sort case, always use the more rigorous clause */
		if (list_length(root->distinct_pathkeys) <
			list_length(root->sort_pathkeys))
		{
			needed_pathkeys = root->sort_pathkeys;
			/* Assert checks that parser didn't mess up... */
			Assert(pathkeys_contained_in(root->distinct_pathkeys,
										 needed_pathkeys));
		}
		else
			needed_pathkeys = root->distinct_pathkeys;

		path = cheapest_input_path;
		if (!pathkeys_contained_in(needed_pathkeys, path->pathkeys))
			path = (Path *) create_sort_path(root, distinct_rel,
											 path,
											 needed_pathkeys,
											 -1.0);

		add_path(distinct_rel, (Path *)
				 create_upper_unique_path(root, distinct_rel,
										  path,
										list_length(root->distinct_pathkeys),
										  numDistinctRows));
	}

	/*
	 * Consider hash-based implementations of DISTINCT, if possible.
	 *
	 * If we were not able to make any other types of path, we *must* hash or
	 * die trying.  If we do have other choices, there are several things that
	 * should prevent selection of hashing: if the query uses DISTINCT ON
	 * (because it won't really have the expected behavior if we hash), or if
	 * enable_hashagg is off, or if it looks like the hashtable will exceed
	 * work_mem.
	 *
	 * Note: grouping_is_hashable() is much more expensive to check than the
	 * other gating conditions, so we want to do it last.
	 */
	if (distinct_rel->pathlist == NIL)
		allow_hash = true;		/* we have no alternatives */
	else if (parse->hasDistinctOn || !enable_hashagg)
		allow_hash = false;		/* policy-based decision not to hash */
	else
	{
		Size		hashentrysize;

		/* Estimate per-hash-entry space at tuple width... */
		hashentrysize = MAXALIGN(cheapest_input_path->pathtarget->width) +
			MAXALIGN(SizeofMinimalTupleHeader);
		/* plus the per-hash-entry overhead */
		hashentrysize += hash_agg_entry_size(0);

		/* Allow hashing only if hashtable is predicted to fit in work_mem */
		allow_hash = (hashentrysize * numDistinctRows <= work_mem * 1024L);
	}

	if (allow_hash && grouping_is_hashable(parse->distinctClause))
	{
		/* Generate hashed aggregate path --- no sort needed */
		add_path(distinct_rel, (Path *)
				 create_agg_path(root,
								 distinct_rel,
								 cheapest_input_path,
								 cheapest_input_path->pathtarget,
								 AGG_HASHED,
								 AGGSPLIT_SIMPLE,
								 parse->distinctClause,
								 NIL,
								 NULL,
								 numDistinctRows));
	}

	/* Give a helpful error if we failed to find any implementation */
	if (distinct_rel->pathlist == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not implement DISTINCT"),
				 errdetail("Some of the datatypes only support hashing, while others only support sorting.")));

	/*
	 * If there is an FDW that's responsible for all baserels of the query,
	 * let it consider adding ForeignPaths.
	 */
	if (distinct_rel->fdwroutine &&
		distinct_rel->fdwroutine->GetForeignUpperPaths)
		distinct_rel->fdwroutine->GetForeignUpperPaths(root, UPPERREL_DISTINCT,
													input_rel, distinct_rel);

	/* Let extensions possibly add some more paths */
	if (create_upper_paths_hook)
		(*create_upper_paths_hook) (root, UPPERREL_DISTINCT,
									input_rel, distinct_rel);

	/* Now choose the best path(s) */
	set_cheapest(distinct_rel);

	return distinct_rel;
}

/*
 * create_ordered_paths
 *
 * Build a new upperrel containing Paths for ORDER BY evaluation.
 *
 * All paths in the result must satisfy the ORDER BY ordering.
 * The only new path we need consider is an explicit sort on the
 * cheapest-total existing path.
 *
 * input_rel: contains the source-data Paths
 * target: the output tlist the result Paths must emit
 * limit_tuples: estimated bound on the number of output tuples,
 *		or -1 if no LIMIT or couldn't estimate
 */
static RelOptInfo *
create_ordered_paths(PlannerInfo *root,
					 RelOptInfo *input_rel,
					 PathTarget *target,
					 double limit_tuples)
{
	Path	   *cheapest_input_path = input_rel->cheapest_total_path;
	RelOptInfo *ordered_rel;
	ListCell   *lc;

	/* For now, do all work in the (ORDERED, NULL) upperrel */
	ordered_rel = fetch_upper_rel(root, UPPERREL_ORDERED, NULL);

	/*
	 * If the input relation is not parallel-safe, then the ordered relation
	 * can't be parallel-safe, either.  Otherwise, it's parallel-safe if the
	 * target list is parallel-safe.
	 */
	if (input_rel->consider_parallel &&
		is_parallel_safe(root, (Node *) target->exprs))
		ordered_rel->consider_parallel = true;

	/*
	 * If the input rel belongs to a single FDW, so does the ordered_rel.
	 */
	ordered_rel->serverid = input_rel->serverid;
	ordered_rel->userid = input_rel->userid;
	ordered_rel->useridiscurrent = input_rel->useridiscurrent;
	ordered_rel->fdwroutine = input_rel->fdwroutine;

	foreach(lc, input_rel->pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);
		bool		is_sorted;

		is_sorted = pathkeys_contained_in(root->sort_pathkeys,
										  path->pathkeys);
		if (path == cheapest_input_path || is_sorted)
		{
			if (!is_sorted)
			{
				/* An explicit sort here can take advantage of LIMIT */
				path = (Path *) create_sort_path(root,
												 ordered_rel,
												 path,
												 root->sort_pathkeys,
												 limit_tuples);
			}

			/* Add projection step if needed */
			if (path->pathtarget != target)
				path = apply_projection_to_path(root, ordered_rel,
												path, target);

			add_path(ordered_rel, path);
		}
	}

	/*
	 * If there is an FDW that's responsible for all baserels of the query,
	 * let it consider adding ForeignPaths.
	 */
	if (ordered_rel->fdwroutine &&
		ordered_rel->fdwroutine->GetForeignUpperPaths)
		ordered_rel->fdwroutine->GetForeignUpperPaths(root, UPPERREL_ORDERED,
													  input_rel, ordered_rel);

	/* Let extensions possibly add some more paths */
	if (create_upper_paths_hook)
		(*create_upper_paths_hook) (root, UPPERREL_ORDERED,
									input_rel, ordered_rel);

	/*
	 * No need to bother with set_cheapest here; grouping_planner does not
	 * need us to do it.
	 */
	Assert(ordered_rel->pathlist != NIL);

	return ordered_rel;
}


/*
 * make_group_input_target
 *	  Generate appropriate PathTarget for initial input to grouping nodes.
 *
 * If there is grouping or aggregation, the scan/join subplan cannot emit
 * the query's final targetlist; for example, it certainly can't emit any
 * aggregate function calls.  This routine generates the correct target
 * for the scan/join subplan.
 *
 * The query target list passed from the parser already contains entries
 * for all ORDER BY and GROUP BY expressions, but it will not have entries
 * for variables used only in HAVING clauses; so we need to add those
 * variables to the subplan target list.  Also, we flatten all expressions
 * except GROUP BY items into their component variables; other expressions
 * will be computed by the upper plan nodes rather than by the subplan.
 * For example, given a query like
 *		SELECT a+b,SUM(c+d) FROM table GROUP BY a+b;
 * we want to pass this targetlist to the subplan:
 *		a+b,c,d
 * where the a+b target will be used by the Sort/Group steps, and the
 * other targets will be used for computing the final results.
 *
 * 'final_target' is the query's final target list (in PathTarget form)
 *
 * The result is the PathTarget to be computed by the Paths returned from
 * query_planner().
 */
static PathTarget *
make_group_input_target(PlannerInfo *root, PathTarget *final_target)
{
	Query	   *parse = root->parse;
	PathTarget *input_target;
	List	   *non_group_cols;
	List	   *non_group_vars;
	int			i;
	ListCell   *lc;

	/*
	 * We must build a target containing all grouping columns, plus any other
	 * Vars mentioned in the query's targetlist and HAVING qual.
	 */
	input_target = create_empty_pathtarget();
	non_group_cols = NIL;

	i = 0;
	foreach(lc, final_target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		Index		sgref = get_pathtarget_sortgroupref(final_target, i);

		if (sgref && parse->groupClause &&
			get_sortgroupref_clause_noerr(sgref, parse->groupClause) != NULL)
		{
			/*
			 * It's a grouping column, so add it to the input target as-is.
			 */
			add_column_to_pathtarget(input_target, expr, sgref);
		}
		else
		{
			/*
			 * Non-grouping column, so just remember the expression for later
			 * call to pull_var_clause.
			 */
			non_group_cols = lappend(non_group_cols, expr);
		}

		i++;
	}

	/*
	 * If there's a HAVING clause, we'll need the Vars it uses, too.
	 */
	if (parse->havingQual)
		non_group_cols = lappend(non_group_cols, parse->havingQual);

	/*
	 * Pull out all the Vars mentioned in non-group cols (plus HAVING), and
	 * add them to the input target if not already present.  (A Var used
	 * directly as a GROUP BY item will be present already.)  Note this
	 * includes Vars used in resjunk items, so we are covering the needs of
	 * ORDER BY and window specifications.  Vars used within Aggrefs and
	 * WindowFuncs will be pulled out here, too.
	 */
	non_group_vars = pull_var_clause((Node *) non_group_cols,
									 PVC_RECURSE_AGGREGATES |
									 PVC_RECURSE_WINDOWFUNCS |
									 PVC_INCLUDE_PLACEHOLDERS);
	add_new_columns_to_pathtarget(input_target, non_group_vars);

	/* clean up cruft */
	list_free(non_group_vars);
	list_free(non_group_cols);

	/* XXX this causes some redundant cost calculation ... */
	return set_pathtarget_cost_width(root, input_target);
}

/*
 * make_partial_grouping_target
 *	  Generate appropriate PathTarget for output of partial aggregate
 *	  (or partial grouping, if there are no aggregates) nodes.
 *
 * A partial aggregation node needs to emit all the same aggregates that
 * a regular aggregation node would, plus any aggregates used in HAVING;
 * except that the Aggref nodes should be marked as partial aggregates.
 *
 * In addition, we'd better emit any Vars and PlaceholderVars that are
 * used outside of Aggrefs in the aggregation tlist and HAVING.  (Presumably,
 * these would be Vars that are grouped by or used in grouping expressions.)
 *
 * grouping_target is the tlist to be emitted by the topmost aggregation step.
 * We get the HAVING clause out of *root.
 */
static PathTarget *
make_partial_grouping_target(PlannerInfo *root, PathTarget *grouping_target)
{
	Query	   *parse = root->parse;
	PathTarget *partial_target;
	List	   *non_group_cols;
	List	   *non_group_exprs;
	int			i;
	ListCell   *lc;

	partial_target = create_empty_pathtarget();
	non_group_cols = NIL;

	i = 0;
	foreach(lc, grouping_target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		Index		sgref = get_pathtarget_sortgroupref(grouping_target, i);

		if (sgref && parse->groupClause &&
			get_sortgroupref_clause_noerr(sgref, parse->groupClause) != NULL)
		{
			/*
			 * It's a grouping column, so add it to the partial_target as-is.
			 * (This allows the upper agg step to repeat the grouping calcs.)
			 */
			add_column_to_pathtarget(partial_target, expr, sgref);
		}
		else
		{
			/*
			 * Non-grouping column, so just remember the expression for later
			 * call to pull_var_clause.
			 */
			non_group_cols = lappend(non_group_cols, expr);
		}

		i++;
	}

	/*
	 * If there's a HAVING clause, we'll need the Vars/Aggrefs it uses, too.
	 */
	if (parse->havingQual)
		non_group_cols = lappend(non_group_cols, parse->havingQual);

	/*
	 * Pull out all the Vars, PlaceHolderVars, and Aggrefs mentioned in
	 * non-group cols (plus HAVING), and add them to the partial_target if not
	 * already present.  (An expression used directly as a GROUP BY item will
	 * be present already.)  Note this includes Vars used in resjunk items, so
	 * we are covering the needs of ORDER BY and window specifications.
	 */
	non_group_exprs = pull_var_clause((Node *) non_group_cols,
									  PVC_INCLUDE_AGGREGATES |
									  PVC_RECURSE_WINDOWFUNCS |
									  PVC_INCLUDE_PLACEHOLDERS);

	add_new_columns_to_pathtarget(partial_target, non_group_exprs);

	/*
	 * Adjust Aggrefs to put them in partial mode.  At this point all Aggrefs
	 * are at the top level of the target list, so we can just scan the list
	 * rather than recursing through the expression trees.
	 */
	foreach(lc, partial_target->exprs)
	{
		Aggref	   *aggref = (Aggref *) lfirst(lc);

		if (IsA(aggref, Aggref))
		{
			Aggref	   *newaggref;

			/*
			 * We shouldn't need to copy the substructure of the Aggref node,
			 * but flat-copy the node itself to avoid damaging other trees.
			 */
			newaggref = makeNode(Aggref);
			memcpy(newaggref, aggref, sizeof(Aggref));

			/* For now, assume serialization is required */
			mark_partial_aggref(newaggref, AGGSPLIT_INITIAL_SERIAL);

			lfirst(lc) = newaggref;
		}
	}

	/* clean up cruft */
	list_free(non_group_exprs);
	list_free(non_group_cols);

	/* XXX this causes some redundant cost calculation ... */
	return set_pathtarget_cost_width(root, partial_target);
}

/*
 * mark_partial_aggref
 *	  Adjust an Aggref to make it represent a partial-aggregation step.
 *
 * The Aggref node is modified in-place; caller must do any copying required.
 */
void
mark_partial_aggref(Aggref *agg, AggSplit aggsplit)
{
	/* aggtranstype should be computed by this point */
	Assert(OidIsValid(agg->aggtranstype));
	/* ... but aggsplit should still be as the parser left it */
	Assert(agg->aggsplit == AGGSPLIT_SIMPLE);

	/* Mark the Aggref with the intended partial-aggregation mode */
	agg->aggsplit = aggsplit;

	/*
	 * Adjust result type if needed.  Normally, a partial aggregate returns
	 * the aggregate's transition type; but if that's INTERNAL and we're
	 * serializing, it returns BYTEA instead.
	 */
	if (DO_AGGSPLIT_SKIPFINAL(aggsplit))
	{
		if (agg->aggtranstype == INTERNALOID && DO_AGGSPLIT_SERIALIZE(aggsplit))
			agg->aggtype = BYTEAOID;
		else
			agg->aggtype = agg->aggtranstype;
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
 * make_window_input_target
 *	  Generate appropriate PathTarget for initial input to WindowAgg nodes.
 *
 * When the query has window functions, this function computes the desired
 * target to be computed by the node just below the first WindowAgg.
 * This tlist must contain all values needed to evaluate the window functions,
 * compute the final target list, and perform any required final sort step.
 * If multiple WindowAggs are needed, each intermediate one adds its window
 * function results onto this base tlist; only the topmost WindowAgg computes
 * the actual desired target list.
 *
 * This function is much like make_group_input_target, though not quite enough
 * like it to share code.  As in that function, we flatten most expressions
 * into their component variables.  But we do not want to flatten window
 * PARTITION BY/ORDER BY clauses, since that might result in multiple
 * evaluations of them, which would be bad (possibly even resulting in
 * inconsistent answers, if they contain volatile functions).
 * Also, we must not flatten GROUP BY clauses that were left unflattened by
 * make_group_input_target, because we may no longer have access to the
 * individual Vars in them.
 *
 * Another key difference from make_group_input_target is that we don't
 * flatten Aggref expressions, since those are to be computed below the
 * window functions and just referenced like Vars above that.
 *
 * 'final_target' is the query's final target list (in PathTarget form)
 * 'activeWindows' is the list of active windows previously identified by
 *			select_active_windows.
 *
 * The result is the PathTarget to be computed by the plan node immediately
 * below the first WindowAgg node.
 */
static PathTarget *
make_window_input_target(PlannerInfo *root,
						 PathTarget *final_target,
						 List *activeWindows)
{
	Query	   *parse = root->parse;
	PathTarget *input_target;
	Bitmapset  *sgrefs;
	List	   *flattenable_cols;
	List	   *flattenable_vars;
	int			i;
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
	 * Construct a target containing all the non-flattenable targetlist items,
	 * and save aside the others for a moment.
	 */
	input_target = create_empty_pathtarget();
	flattenable_cols = NIL;

	i = 0;
	foreach(lc, final_target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		Index		sgref = get_pathtarget_sortgroupref(final_target, i);

		/*
		 * Don't want to deconstruct window clauses or GROUP BY items.  (Note
		 * that such items can't contain window functions, so it's okay to
		 * compute them below the WindowAgg nodes.)
		 */
		if (sgref != 0 && bms_is_member(sgref, sgrefs))
		{
			/*
			 * Don't want to deconstruct this value, so add it to the input
			 * target as-is.
			 */
			add_column_to_pathtarget(input_target, expr, sgref);
		}
		else
		{
			/*
			 * Column is to be flattened, so just remember the expression for
			 * later call to pull_var_clause.
			 */
			flattenable_cols = lappend(flattenable_cols, expr);
		}

		i++;
	}

	/*
	 * Pull out all the Vars and Aggrefs mentioned in flattenable columns, and
	 * add them to the input target if not already present.  (Some might be
	 * there already because they're used directly as window/group clauses.)
	 *
	 * Note: it's essential to use PVC_INCLUDE_AGGREGATES here, so that any
	 * Aggrefs are placed in the Agg node's tlist and not left to be computed
	 * at higher levels.  On the other hand, we should recurse into
	 * WindowFuncs to make sure their input expressions are available.
	 */
	flattenable_vars = pull_var_clause((Node *) flattenable_cols,
									   PVC_INCLUDE_AGGREGATES |
									   PVC_RECURSE_WINDOWFUNCS |
									   PVC_INCLUDE_PLACEHOLDERS);
	add_new_columns_to_pathtarget(input_target, flattenable_vars);

	/* clean up cruft */
	list_free(flattenable_vars);
	list_free(flattenable_cols);

	/* XXX this causes some redundant cost calculation ... */
	return set_pathtarget_cost_width(root, input_target);
}

/*
 * make_pathkeys_for_window
 *		Create a pathkeys list describing the required input ordering
 *		for the given WindowClause.
 *
 * The required ordering is first the PARTITION keys, then the ORDER keys.
 * In the future we might try to implement windowing using hashing, in which
 * case the ordering could be relaxed, but for now we always sort.
 *
 * Caution: if you change this, see createplan.c's get_column_info_for_window!
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

/*
 * make_sort_input_target
 *	  Generate appropriate PathTarget for initial input to Sort step.
 *
 * If the query has ORDER BY, this function chooses the target to be computed
 * by the node just below the Sort (and DISTINCT, if any, since Unique can't
 * project) steps.  This might or might not be identical to the query's final
 * output target.
 *
 * The main argument for keeping the sort-input tlist the same as the final
 * is that we avoid a separate projection node (which will be needed if
 * they're different, because Sort can't project).  However, there are also
 * advantages to postponing tlist evaluation till after the Sort: it ensures
 * a consistent order of evaluation for any volatile functions in the tlist,
 * and if there's also a LIMIT, we can stop the query without ever computing
 * tlist functions for later rows, which is beneficial for both volatile and
 * expensive functions.
 *
 * Our current policy is to postpone volatile expressions till after the sort
 * unconditionally (assuming that that's possible, ie they are in plain tlist
 * columns and not ORDER BY/GROUP BY/DISTINCT columns).  We also prefer to
 * postpone set-returning expressions, because running them beforehand would
 * bloat the sort dataset, and because it might cause unexpected output order
 * if the sort isn't stable.  However there's a constraint on that: all SRFs
 * in the tlist should be evaluated at the same plan step, so that they can
 * run in sync in ExecTargetList.  So if any SRFs are in sort columns, we
 * mustn't postpone any SRFs.  (Note that in principle that policy should
 * probably get applied to the group/window input targetlists too, but we
 * have not done that historically.)  Lastly, expensive expressions are
 * postponed if there is a LIMIT, or if root->tuple_fraction shows that
 * partial evaluation of the query is possible (if neither is true, we expect
 * to have to evaluate the expressions for every row anyway), or if there are
 * any volatile or set-returning expressions (since once we've put in a
 * projection at all, it won't cost any more to postpone more stuff).
 *
 * Another issue that could potentially be considered here is that
 * evaluating tlist expressions could result in data that's either wider
 * or narrower than the input Vars, thus changing the volume of data that
 * has to go through the Sort.  However, we usually have only a very bad
 * idea of the output width of any expression more complex than a Var,
 * so for now it seems too risky to try to optimize on that basis.
 *
 * Note that if we do produce a modified sort-input target, and then the
 * query ends up not using an explicit Sort, no particular harm is done:
 * we'll initially use the modified target for the preceding path nodes,
 * but then change them to the final target with apply_projection_to_path.
 * Moreover, in such a case the guarantees about evaluation order of
 * volatile functions still hold, since the rows are sorted already.
 *
 * This function has some things in common with make_group_input_target and
 * make_window_input_target, though the detailed rules for what to do are
 * different.  We never flatten/postpone any grouping or ordering columns;
 * those are needed before the sort.  If we do flatten a particular
 * expression, we leave Aggref and WindowFunc nodes alone, since those were
 * computed earlier.
 *
 * 'final_target' is the query's final target list (in PathTarget form)
 * 'have_postponed_srfs' is an output argument, see below
 *
 * The result is the PathTarget to be computed by the plan node immediately
 * below the Sort step (and the Distinct step, if any).  This will be
 * exactly final_target if we decide a projection step wouldn't be helpful.
 *
 * In addition, *have_postponed_srfs is set to TRUE if we choose to postpone
 * any set-returning functions to after the Sort.
 */
static PathTarget *
make_sort_input_target(PlannerInfo *root,
					   PathTarget *final_target,
					   bool *have_postponed_srfs)
{
	Query	   *parse = root->parse;
	PathTarget *input_target;
	int			ncols;
	bool	   *col_is_srf;
	bool	   *postpone_col;
	bool		have_srf;
	bool		have_volatile;
	bool		have_expensive;
	bool		have_srf_sortcols;
	bool		postpone_srfs;
	List	   *postponable_cols;
	List	   *postponable_vars;
	int			i;
	ListCell   *lc;

	/* Shouldn't get here unless query has ORDER BY */
	Assert(parse->sortClause);

	*have_postponed_srfs = false;		/* default result */

	/* Inspect tlist and collect per-column information */
	ncols = list_length(final_target->exprs);
	col_is_srf = (bool *) palloc0(ncols * sizeof(bool));
	postpone_col = (bool *) palloc0(ncols * sizeof(bool));
	have_srf = have_volatile = have_expensive = have_srf_sortcols = false;

	i = 0;
	foreach(lc, final_target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);

		/*
		 * If the column has a sortgroupref, assume it has to be evaluated
		 * before sorting.  Generally such columns would be ORDER BY, GROUP
		 * BY, etc targets.  One exception is columns that were removed from
		 * GROUP BY by remove_useless_groupby_columns() ... but those would
		 * only be Vars anyway.  There don't seem to be any cases where it
		 * would be worth the trouble to double-check.
		 */
		if (get_pathtarget_sortgroupref(final_target, i) == 0)
		{
			/*
			 * Check for SRF or volatile functions.  Check the SRF case first
			 * because we must know whether we have any postponed SRFs.
			 */
			if (parse->hasTargetSRFs &&
				expression_returns_set((Node *) expr))
			{
				/* We'll decide below whether these are postponable */
				col_is_srf[i] = true;
				have_srf = true;
			}
			else if (contain_volatile_functions((Node *) expr))
			{
				/* Unconditionally postpone */
				postpone_col[i] = true;
				have_volatile = true;
			}
			else
			{
				/*
				 * Else check the cost.  XXX it's annoying to have to do this
				 * when set_pathtarget_cost_width() just did it.  Refactor to
				 * allow sharing the work?
				 */
				QualCost	cost;

				cost_qual_eval_node(&cost, (Node *) expr, root);

				/*
				 * We arbitrarily define "expensive" as "more than 10X
				 * cpu_operator_cost".  Note this will take in any PL function
				 * with default cost.
				 */
				if (cost.per_tuple > 10 * cpu_operator_cost)
				{
					postpone_col[i] = true;
					have_expensive = true;
				}
			}
		}
		else
		{
			/* For sortgroupref cols, just check if any contain SRFs */
			if (!have_srf_sortcols &&
				parse->hasTargetSRFs &&
				expression_returns_set((Node *) expr))
				have_srf_sortcols = true;
		}

		i++;
	}

	/*
	 * We can postpone SRFs if we have some but none are in sortgroupref cols.
	 */
	postpone_srfs = (have_srf && !have_srf_sortcols);

	/*
	 * If we don't need a post-sort projection, just return final_target.
	 */
	if (!(postpone_srfs || have_volatile ||
		  (have_expensive &&
		   (parse->limitCount || root->tuple_fraction > 0))))
		return final_target;

	/*
	 * Report whether the post-sort projection will contain set-returning
	 * functions.  This is important because it affects whether the Sort can
	 * rely on the query's LIMIT (if any) to bound the number of rows it needs
	 * to return.
	 */
	*have_postponed_srfs = postpone_srfs;

	/*
	 * Construct the sort-input target, taking all non-postponable columns and
	 * then adding Vars, PlaceHolderVars, Aggrefs, and WindowFuncs found in
	 * the postponable ones.
	 */
	input_target = create_empty_pathtarget();
	postponable_cols = NIL;

	i = 0;
	foreach(lc, final_target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);

		if (postpone_col[i] || (postpone_srfs && col_is_srf[i]))
			postponable_cols = lappend(postponable_cols, expr);
		else
			add_column_to_pathtarget(input_target, expr,
							   get_pathtarget_sortgroupref(final_target, i));

		i++;
	}

	/*
	 * Pull out all the Vars, Aggrefs, and WindowFuncs mentioned in
	 * postponable columns, and add them to the sort-input target if not
	 * already present.  (Some might be there already.)  We mustn't
	 * deconstruct Aggrefs or WindowFuncs here, since the projection node
	 * would be unable to recompute them.
	 */
	postponable_vars = pull_var_clause((Node *) postponable_cols,
									   PVC_INCLUDE_AGGREGATES |
									   PVC_INCLUDE_WINDOWFUNCS |
									   PVC_INCLUDE_PLACEHOLDERS);
	add_new_columns_to_pathtarget(input_target, postponable_vars);

	/* clean up cruft */
	list_free(postponable_vars);
	list_free(postponable_cols);

	/* XXX this represents even more redundant cost calculation ... */
	return set_pathtarget_cost_width(root, input_target);
}

/*
 * get_cheapest_fractional_path
 *	  Find the cheapest path for retrieving a specified fraction of all
 *	  the tuples expected to be returned by the given relation.
 *
 * We interpret tuple_fraction the same way as grouping_planner.
 *
 * We assume set_cheapest() has been run on the given rel.
 */
Path *
get_cheapest_fractional_path(RelOptInfo *rel, double tuple_fraction)
{
	Path	   *best_path = rel->cheapest_total_path;
	ListCell   *l;

	/* If all tuples will be retrieved, just return the cheapest-total path */
	if (tuple_fraction <= 0.0)
		return best_path;

	/* Convert absolute # of tuples to a fraction; no need to clamp to 0..1 */
	if (tuple_fraction >= 1.0 && best_path->rows > 0)
		tuple_fraction /= best_path->rows;

	foreach(l, rel->pathlist)
	{
		Path	   *path = (Path *) lfirst(l);

		if (path == rel->cheapest_total_path ||
		 compare_fractional_path_costs(best_path, path, tuple_fraction) <= 0)
			continue;

		best_path = path;
	}

	return best_path;
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

	/* We can short-circuit the cost comparison if indexscans are disabled */
	if (!enable_indexscan)
		return true;			/* use sort */

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
	rel->reltarget->width = get_relation_data_width(tableOid, NULL);

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
	seqScanPath = create_seqscan_path(root, rel, NULL, 0);
	cost_sort(&seqScanAndSortPath, root, NIL,
			  seqScanPath->total_cost, rel->tuples, rel->reltarget->width,
			  comparisonCost, maintenance_work_mem, -1.0);

	/* Estimate the cost of index scan */
	indexScanPath = create_index_path(root, indexInfo,
									  NIL, NIL, NIL, NIL, NIL,
									  ForwardScanDirection, false,
									  NULL, 1.0);

	return (seqScanAndSortPath.total_cost < indexScanPath->path.total_cost);
}
