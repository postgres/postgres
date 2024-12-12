/*-------------------------------------------------------------------------
 *
 * planner.c
 *	  The query optimizer external interface.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
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

#include "access/genam.h"
#include "access/parallel.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "foreign/fdwapi.h"
#include "jit/jit.h"
#include "lib/bipartite_match.h"
#include "lib/knapsack.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#ifdef OPTIMIZER_DEBUG
#include "nodes/print.h"
#endif
#include "nodes/supportnodes.h"
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/paramassign.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "parser/analyze.h"
#include "parser/parse_agg.h"
#include "parser/parse_clause.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "partitioning/partdesc.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"

/* GUC parameters */
double		cursor_tuple_fraction = DEFAULT_CURSOR_TUPLE_FRACTION;
int			debug_parallel_query = DEBUG_PARALLEL_OFF;
bool		parallel_leader_participation = true;
bool		enable_distinct_reordering = true;

/* Hook for plugins to get control in planner() */
planner_hook_type planner_hook = NULL;

/* Hook for plugins to get control when grouping_planner() plans upper rels */
create_upper_paths_hook_type create_upper_paths_hook = NULL;


/* Expression kind codes for preprocess_expression */
#define EXPRKIND_QUAL				0
#define EXPRKIND_TARGET				1
#define EXPRKIND_RTFUNC				2
#define EXPRKIND_RTFUNC_LATERAL		3
#define EXPRKIND_VALUES				4
#define EXPRKIND_VALUES_LATERAL		5
#define EXPRKIND_LIMIT				6
#define EXPRKIND_APPINFO			7
#define EXPRKIND_PHV				8
#define EXPRKIND_TABLESAMPLE		9
#define EXPRKIND_ARBITER_ELEM		10
#define EXPRKIND_TABLEFUNC			11
#define EXPRKIND_TABLEFUNC_LATERAL	12
#define EXPRKIND_GROUPEXPR			13

/*
 * Data specific to grouping sets
 */
typedef struct
{
	List	   *rollups;
	List	   *hash_sets_idx;
	double		dNumHashGroups;
	bool		any_hashable;
	Bitmapset  *unsortable_refs;
	Bitmapset  *unhashable_refs;
	List	   *unsortable_sets;
	int		   *tleref_to_colnum_map;
} grouping_sets_data;

/*
 * Temporary structure for use during WindowClause reordering in order to be
 * able to sort WindowClauses on partitioning/ordering prefix.
 */
typedef struct
{
	WindowClause *wc;
	List	   *uniqueOrder;	/* A List of unique ordering/partitioning
								 * clauses per Window */
} WindowClauseSortData;

/* Passthrough data for standard_qp_callback */
typedef struct
{
	List	   *activeWindows;	/* active windows, if any */
	grouping_sets_data *gset_data;	/* grouping sets data, if any */
	SetOperationStmt *setop;	/* parent set operation or NULL if not a
								 * subquery belonging to a set operation */
} standard_qp_extra;

/* Local functions */
static Node *preprocess_expression(PlannerInfo *root, Node *expr, int kind);
static void preprocess_qual_conditions(PlannerInfo *root, Node *jtnode);
static void grouping_planner(PlannerInfo *root, double tuple_fraction,
							 SetOperationStmt *setops);
static grouping_sets_data *preprocess_grouping_sets(PlannerInfo *root);
static List *remap_to_groupclause_idx(List *groupClause, List *gsets,
									  int *tleref_to_colnum_map);
static void preprocess_rowmarks(PlannerInfo *root);
static double preprocess_limit(PlannerInfo *root,
							   double tuple_fraction,
							   int64 *offset_est, int64 *count_est);
static List *preprocess_groupclause(PlannerInfo *root, List *force);
static List *extract_rollup_sets(List *groupingSets);
static List *reorder_grouping_sets(List *groupingSets, List *sortclause);
static void standard_qp_callback(PlannerInfo *root, void *extra);
static double get_number_of_groups(PlannerInfo *root,
								   double path_rows,
								   grouping_sets_data *gd,
								   List *target_list);
static RelOptInfo *create_grouping_paths(PlannerInfo *root,
										 RelOptInfo *input_rel,
										 PathTarget *target,
										 bool target_parallel_safe,
										 grouping_sets_data *gd);
static bool is_degenerate_grouping(PlannerInfo *root);
static void create_degenerate_grouping_paths(PlannerInfo *root,
											 RelOptInfo *input_rel,
											 RelOptInfo *grouped_rel);
static RelOptInfo *make_grouping_rel(PlannerInfo *root, RelOptInfo *input_rel,
									 PathTarget *target, bool target_parallel_safe,
									 Node *havingQual);
static void create_ordinary_grouping_paths(PlannerInfo *root,
										   RelOptInfo *input_rel,
										   RelOptInfo *grouped_rel,
										   const AggClauseCosts *agg_costs,
										   grouping_sets_data *gd,
										   GroupPathExtraData *extra,
										   RelOptInfo **partially_grouped_rel_p);
static void consider_groupingsets_paths(PlannerInfo *root,
										RelOptInfo *grouped_rel,
										Path *path,
										bool is_sorted,
										bool can_hash,
										grouping_sets_data *gd,
										const AggClauseCosts *agg_costs,
										double dNumGroups);
static RelOptInfo *create_window_paths(PlannerInfo *root,
									   RelOptInfo *input_rel,
									   PathTarget *input_target,
									   PathTarget *output_target,
									   bool output_target_parallel_safe,
									   WindowFuncLists *wflists,
									   List *activeWindows);
static void create_one_window_path(PlannerInfo *root,
								   RelOptInfo *window_rel,
								   Path *path,
								   PathTarget *input_target,
								   PathTarget *output_target,
								   WindowFuncLists *wflists,
								   List *activeWindows);
static RelOptInfo *create_distinct_paths(PlannerInfo *root,
										 RelOptInfo *input_rel,
										 PathTarget *target);
static void create_partial_distinct_paths(PlannerInfo *root,
										  RelOptInfo *input_rel,
										  RelOptInfo *final_distinct_rel,
										  PathTarget *target);
static RelOptInfo *create_final_distinct_paths(PlannerInfo *root,
											   RelOptInfo *input_rel,
											   RelOptInfo *distinct_rel);
static List *get_useful_pathkeys_for_distinct(PlannerInfo *root,
											  List *needed_pathkeys,
											  List *path_pathkeys);
static RelOptInfo *create_ordered_paths(PlannerInfo *root,
										RelOptInfo *input_rel,
										PathTarget *target,
										bool target_parallel_safe,
										double limit_tuples);
static PathTarget *make_group_input_target(PlannerInfo *root,
										   PathTarget *final_target);
static PathTarget *make_partial_grouping_target(PlannerInfo *root,
												PathTarget *grouping_target,
												Node *havingQual);
static List *postprocess_setop_tlist(List *new_tlist, List *orig_tlist);
static void optimize_window_clauses(PlannerInfo *root,
									WindowFuncLists *wflists);
static List *select_active_windows(PlannerInfo *root, WindowFuncLists *wflists);
static PathTarget *make_window_input_target(PlannerInfo *root,
											PathTarget *final_target,
											List *activeWindows);
static List *make_pathkeys_for_window(PlannerInfo *root, WindowClause *wc,
									  List *tlist);
static PathTarget *make_sort_input_target(PlannerInfo *root,
										  PathTarget *final_target,
										  bool *have_postponed_srfs);
static void adjust_paths_for_srfs(PlannerInfo *root, RelOptInfo *rel,
								  List *targets, List *targets_contain_srfs);
static void add_paths_to_grouping_rel(PlannerInfo *root, RelOptInfo *input_rel,
									  RelOptInfo *grouped_rel,
									  RelOptInfo *partially_grouped_rel,
									  const AggClauseCosts *agg_costs,
									  grouping_sets_data *gd,
									  double dNumGroups,
									  GroupPathExtraData *extra);
static RelOptInfo *create_partial_grouping_paths(PlannerInfo *root,
												 RelOptInfo *grouped_rel,
												 RelOptInfo *input_rel,
												 grouping_sets_data *gd,
												 GroupPathExtraData *extra,
												 bool force_rel_creation);
static Path *make_ordered_path(PlannerInfo *root,
							   RelOptInfo *rel,
							   Path *path,
							   Path *cheapest_path,
							   List *pathkeys,
							   double limit_tuples);
static void gather_grouping_paths(PlannerInfo *root, RelOptInfo *rel);
static bool can_partial_agg(PlannerInfo *root);
static void apply_scanjoin_target_to_paths(PlannerInfo *root,
										   RelOptInfo *rel,
										   List *scanjoin_targets,
										   List *scanjoin_targets_contain_srfs,
										   bool scanjoin_target_parallel_safe,
										   bool tlist_same_exprs);
static void create_partitionwise_grouping_paths(PlannerInfo *root,
												RelOptInfo *input_rel,
												RelOptInfo *grouped_rel,
												RelOptInfo *partially_grouped_rel,
												const AggClauseCosts *agg_costs,
												grouping_sets_data *gd,
												PartitionwiseAggregateType patype,
												GroupPathExtraData *extra);
static bool group_by_has_partkey(RelOptInfo *input_rel,
								 List *targetList,
								 List *groupClause);
static int	common_prefix_cmp(const void *a, const void *b);
static List *generate_setop_child_grouplist(SetOperationStmt *op,
											List *targetlist);


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
planner(Query *parse, const char *query_string, int cursorOptions,
		ParamListInfo boundParams)
{
	PlannedStmt *result;

	if (planner_hook)
		result = (*planner_hook) (parse, query_string, cursorOptions, boundParams);
	else
		result = standard_planner(parse, query_string, cursorOptions, boundParams);
	return result;
}

PlannedStmt *
standard_planner(Query *parse, const char *query_string, int cursorOptions,
				 ParamListInfo boundParams)
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

	/*
	 * Set up global state for this planner invocation.  This data is needed
	 * across all levels of sub-Query that might exist in the given command,
	 * so we keep it in a separate struct that's linked to by each per-Query
	 * PlannerInfo.
	 */
	glob = makeNode(PlannerGlobal);

	glob->boundParams = boundParams;
	glob->subplans = NIL;
	glob->subpaths = NIL;
	glob->subroots = NIL;
	glob->rewindPlanIDs = NULL;
	glob->finalrtable = NIL;
	glob->finalrteperminfos = NIL;
	glob->finalrowmarks = NIL;
	glob->resultRelations = NIL;
	glob->appendRelations = NIL;
	glob->relationOids = NIL;
	glob->invalItems = NIL;
	glob->paramExecTypes = NIL;
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
	 * (Note that we do allow CREATE TABLE AS, SELECT INTO, and CREATE
	 * MATERIALIZED VIEW to use parallel plans, but this is safe only because
	 * the command is writing into a completely new table which workers won't
	 * be able to see.  If the workers could see the table, the fact that
	 * group locking would cause them to ignore the leader's heavyweight GIN
	 * page locks would make this unsafe.  We'll have to fix that somehow if
	 * we want to allow parallel inserts in general; updates and deletes have
	 * additional problems especially around combo CIDs.)
	 *
	 * For now, we don't try to use parallel mode if we're running inside a
	 * parallel worker.  We might eventually be able to relax this
	 * restriction, but for now it seems best not to have parallel workers
	 * trying to create their own parallel workers.
	 */
	if ((cursorOptions & CURSOR_OPT_PARALLEL_OK) != 0 &&
		IsUnderPostmaster &&
		parse->commandType == CMD_SELECT &&
		!parse->hasModifyingCTE &&
		max_parallel_workers_per_gather > 0 &&
		!IsParallelWorker())
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
	 * glob->parallelModeNeeded is normally set to false here and changed to
	 * true during plan creation if a Gather or Gather Merge plan is actually
	 * created (cf. create_gather_plan, create_gather_merge_plan).
	 *
	 * However, if debug_parallel_query = on or debug_parallel_query =
	 * regress, then we impose parallel mode whenever it's safe to do so, even
	 * if the final plan doesn't use parallelism.  It's not safe to do so if
	 * the query contains anything parallel-unsafe; parallelModeOK will be
	 * false in that case.  Note that parallelModeOK can't change after this
	 * point. Otherwise, everything in the query is either parallel-safe or
	 * parallel-restricted, and in either case it should be OK to impose
	 * parallel-mode restrictions.  If that ends up breaking something, then
	 * either some function the user included in the query is incorrectly
	 * labeled as parallel-safe or parallel-restricted when in reality it's
	 * parallel-unsafe, or else the query planner itself has a bug.
	 */
	glob->parallelModeNeeded = glob->parallelModeOK &&
		(debug_parallel_query != DEBUG_PARALLEL_OFF);

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
	root = subquery_planner(glob, parse, NULL, false, tuple_fraction, NULL);

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
			top_plan = materialize_finished_plan(top_plan);
	}

	/*
	 * Optionally add a Gather node for testing purposes, provided this is
	 * actually a safe thing to do.
	 *
	 * We can add Gather even when top_plan has parallel-safe initPlans, but
	 * then we have to move the initPlans to the Gather node because of
	 * SS_finalize_plan's limitations.  That would cause cosmetic breakage of
	 * regression tests when debug_parallel_query = regress, because initPlans
	 * that would normally appear on the top_plan move to the Gather, causing
	 * them to disappear from EXPLAIN output.  That doesn't seem worth kluging
	 * EXPLAIN to hide, so skip it when debug_parallel_query = regress.
	 */
	if (debug_parallel_query != DEBUG_PARALLEL_OFF &&
		top_plan->parallel_safe &&
		(top_plan->initPlan == NIL ||
		 debug_parallel_query != DEBUG_PARALLEL_REGRESS))
	{
		Gather	   *gather = makeNode(Gather);
		Cost		initplan_cost;
		bool		unsafe_initplans;

		gather->plan.targetlist = top_plan->targetlist;
		gather->plan.qual = NIL;
		gather->plan.lefttree = top_plan;
		gather->plan.righttree = NULL;
		gather->num_workers = 1;
		gather->single_copy = true;
		gather->invisible = (debug_parallel_query == DEBUG_PARALLEL_REGRESS);

		/* Transfer any initPlans to the new top node */
		gather->plan.initPlan = top_plan->initPlan;
		top_plan->initPlan = NIL;

		/*
		 * Since this Gather has no parallel-aware descendants to signal to,
		 * we don't need a rescan Param.
		 */
		gather->rescan_param = -1;

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
		gather->plan.parallel_safe = false;

		/*
		 * Delete the initplans' cost from top_plan.  We needn't add it to the
		 * Gather node, since the above coding already included it there.
		 */
		SS_compute_initplan_cost(gather->plan.initPlan,
								 &initplan_cost, &unsafe_initplans);
		top_plan->startup_cost -= initplan_cost;
		top_plan->total_cost -= initplan_cost;

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
	if (glob->paramExecTypes != NIL)
	{
		Assert(list_length(glob->subplans) == list_length(glob->subroots));
		forboth(lp, glob->subplans, lr, glob->subroots)
		{
			Plan	   *subplan = (Plan *) lfirst(lp);
			PlannerInfo *subroot = lfirst_node(PlannerInfo, lr);

			SS_finalize_plan(subroot, subplan);
		}
		SS_finalize_plan(root, top_plan);
	}

	/* final cleanup of the plan */
	Assert(glob->finalrtable == NIL);
	Assert(glob->finalrteperminfos == NIL);
	Assert(glob->finalrowmarks == NIL);
	Assert(glob->resultRelations == NIL);
	Assert(glob->appendRelations == NIL);
	top_plan = set_plan_references(root, top_plan);
	/* ... and the subplans (both regular subplans and initplans) */
	Assert(list_length(glob->subplans) == list_length(glob->subroots));
	forboth(lp, glob->subplans, lr, glob->subroots)
	{
		Plan	   *subplan = (Plan *) lfirst(lp);
		PlannerInfo *subroot = lfirst_node(PlannerInfo, lr);

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
	result->permInfos = glob->finalrteperminfos;
	result->resultRelations = glob->resultRelations;
	result->appendRelations = glob->appendRelations;
	result->subplans = glob->subplans;
	result->rewindPlanIDs = glob->rewindPlanIDs;
	result->rowMarks = glob->finalrowmarks;
	result->relationOids = glob->relationOids;
	result->invalItems = glob->invalItems;
	result->paramExecTypes = glob->paramExecTypes;
	/* utilityStmt should be null, but we might as well copy it */
	result->utilityStmt = parse->utilityStmt;
	result->stmt_location = parse->stmt_location;
	result->stmt_len = parse->stmt_len;

	result->jitFlags = PGJIT_NONE;
	if (jit_enabled && jit_above_cost >= 0 &&
		top_plan->total_cost > jit_above_cost)
	{
		result->jitFlags |= PGJIT_PERFORM;

		/*
		 * Decide how much effort should be put into generating better code.
		 */
		if (jit_optimize_above_cost >= 0 &&
			top_plan->total_cost > jit_optimize_above_cost)
			result->jitFlags |= PGJIT_OPT3;
		if (jit_inline_above_cost >= 0 &&
			top_plan->total_cost > jit_inline_above_cost)
			result->jitFlags |= PGJIT_INLINE;

		/*
		 * Decide which operations should be JITed.
		 */
		if (jit_expressions)
			result->jitFlags |= PGJIT_EXPR;
		if (jit_tuple_deforming)
			result->jitFlags |= PGJIT_DEFORM;
	}

	if (glob->partition_directory != NULL)
		DestroyPartitionDirectory(glob->partition_directory);

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
 * setops is used for set operation subqueries to provide the subquery with
 * the context in which it's being used so that Paths correctly sorted for the
 * set operation can be generated.  NULL when not planning a set operation
 * child.
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
subquery_planner(PlannerGlobal *glob, Query *parse, PlannerInfo *parent_root,
				 bool hasRecursion, double tuple_fraction,
				 SetOperationStmt *setops)
{
	PlannerInfo *root;
	List	   *newWithCheckOptions;
	List	   *newHaving;
	bool		hasOuterJoins;
	bool		hasResultRTEs;
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
	root->join_domains = NIL;
	root->eq_classes = NIL;
	root->ec_merging_done = false;
	root->last_rinfo_serial = 0;
	root->all_result_relids =
		parse->resultRelation ? bms_make_singleton(parse->resultRelation) : NULL;
	root->leaf_result_relids = NULL;	/* we'll find out leaf-ness later */
	root->append_rel_list = NIL;
	root->row_identity_vars = NIL;
	root->rowMarks = NIL;
	memset(root->upper_rels, 0, sizeof(root->upper_rels));
	memset(root->upper_targets, 0, sizeof(root->upper_targets));
	root->processed_groupClause = NIL;
	root->processed_distinctClause = NIL;
	root->processed_tlist = NIL;
	root->update_colnos = NIL;
	root->grouping_map = NULL;
	root->minmax_aggs = NIL;
	root->qual_security_level = 0;
	root->hasPseudoConstantQuals = false;
	root->hasAlternativeSubPlans = false;
	root->placeholdersFrozen = false;
	root->hasRecursion = hasRecursion;
	if (hasRecursion)
		root->wt_param_id = assign_special_exec_param(root);
	else
		root->wt_param_id = -1;
	root->non_recursive_path = NULL;
	root->partColsUpdated = false;

	/*
	 * Create the top-level join domain.  This won't have valid contents until
	 * deconstruct_jointree fills it in, but the node needs to exist before
	 * that so we can build EquivalenceClasses referencing it.
	 */
	root->join_domains = list_make1(makeNode(JoinDomain));

	/*
	 * If there is a WITH list, process each WITH query and either convert it
	 * to RTE_SUBQUERY RTE(s) or build an initplan SubPlan structure for it.
	 */
	if (parse->cteList)
		SS_process_ctes(root);

	/*
	 * If it's a MERGE command, transform the joinlist as appropriate.
	 */
	transform_MERGE_to_join(parse);

	/*
	 * If the FROM clause is empty, replace it with a dummy RTE_RESULT RTE, so
	 * that we don't need so many special cases to deal with that situation.
	 */
	replace_empty_jointree(parse);

	/*
	 * Look for ANY and EXISTS SubLinks in WHERE and JOIN/ON clauses, and try
	 * to transform them into joins.  Note that this step does not descend
	 * into subqueries; if we pull up any subqueries below, their SubLinks are
	 * processed just before pulling them up.
	 */
	if (parse->hasSubLinks)
		pull_up_sublinks(root);

	/*
	 * Scan the rangetable for function RTEs, do const-simplification on them,
	 * and then inline them if possible (producing subqueries that might get
	 * pulled up next).  Recursion issues here are handled in the same way as
	 * for SubLinks.
	 */
	preprocess_function_rtes(root);

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
	 * Survey the rangetable to see what kinds of entries are present.  We can
	 * skip some later processing if relevant SQL features are not used; for
	 * example if there are no JOIN RTEs we can avoid the expense of doing
	 * flatten_join_alias_vars().  This must be done after we have finished
	 * adding rangetable entries, of course.  (Note: actually, processing of
	 * inherited or partitioned rels can cause RTEs for their child tables to
	 * get added later; but those must all be RTE_RELATION entries, so they
	 * don't invalidate the conclusions drawn here.)
	 */
	root->hasJoinRTEs = false;
	root->hasLateralRTEs = false;
	root->group_rtindex = 0;
	hasOuterJoins = false;
	hasResultRTEs = false;
	foreach(l, parse->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, l);

		switch (rte->rtekind)
		{
			case RTE_RELATION:
				if (rte->inh)
				{
					/*
					 * Check to see if the relation actually has any children;
					 * if not, clear the inh flag so we can treat it as a
					 * plain base relation.
					 *
					 * Note: this could give a false-positive result, if the
					 * rel once had children but no longer does.  We used to
					 * be able to clear rte->inh later on when we discovered
					 * that, but no more; we have to handle such cases as
					 * full-fledged inheritance.
					 */
					rte->inh = has_subclass(rte->relid);
				}
				break;
			case RTE_JOIN:
				root->hasJoinRTEs = true;
				if (IS_OUTER_JOIN(rte->jointype))
					hasOuterJoins = true;
				break;
			case RTE_RESULT:
				hasResultRTEs = true;
				break;
			case RTE_GROUP:
				Assert(parse->hasGroupRTE);
				root->group_rtindex = list_cell_number(parse->rtable, l) + 1;
				break;
			default:
				/* No work here for other RTE types */
				break;
		}

		if (rte->lateral)
			root->hasLateralRTEs = true;

		/*
		 * We can also determine the maximum security level required for any
		 * securityQuals now.  Addition of inheritance-child RTEs won't affect
		 * this, because child tables don't have their own securityQuals; see
		 * expand_single_inheritance_child().
		 */
		if (rte->securityQuals)
			root->qual_security_level = Max(root->qual_security_level,
											list_length(rte->securityQuals));
	}

	/*
	 * If we have now verified that the query target relation is
	 * non-inheriting, mark it as a leaf target.
	 */
	if (parse->resultRelation)
	{
		RangeTblEntry *rte = rt_fetch(parse->resultRelation, parse->rtable);

		if (!rte->inh)
			root->leaf_result_relids =
				bms_make_singleton(parse->resultRelation);
	}

	/*
	 * Preprocess RowMark information.  We need to do this after subquery
	 * pullup, so that all base relations are present.
	 */
	preprocess_rowmarks(root);

	/*
	 * Set hasHavingQual to remember if HAVING clause is present.  Needed
	 * because preprocess_expression will reduce a constant-true condition to
	 * an empty qual list ... but "HAVING TRUE" is not a semantic no-op.
	 */
	root->hasHavingQual = (parse->havingQual != NULL);

	/*
	 * Do expression preprocessing on targetlist and quals, as well as other
	 * random expressions in the querytree.  Note that we do not need to
	 * handle sort/group expressions explicitly, because they are actually
	 * part of the targetlist.
	 */
	parse->targetList = (List *)
		preprocess_expression(root, (Node *) parse->targetList,
							  EXPRKIND_TARGET);

	newWithCheckOptions = NIL;
	foreach(l, parse->withCheckOptions)
	{
		WithCheckOption *wco = lfirst_node(WithCheckOption, l);

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
		WindowClause *wc = lfirst_node(WindowClause, l);

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

	foreach(l, parse->mergeActionList)
	{
		MergeAction *action = (MergeAction *) lfirst(l);

		action->targetList = (List *)
			preprocess_expression(root,
								  (Node *) action->targetList,
								  EXPRKIND_TARGET);
		action->qual =
			preprocess_expression(root,
								  (Node *) action->qual,
								  EXPRKIND_QUAL);
	}

	parse->mergeJoinCondition =
		preprocess_expression(root, parse->mergeJoinCondition, EXPRKIND_QUAL);

	root->append_rel_list = (List *)
		preprocess_expression(root, (Node *) root->append_rel_list,
							  EXPRKIND_APPINFO);

	/* Also need to preprocess expressions within RTEs */
	foreach(l, parse->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, l);
		int			kind;
		ListCell   *lcsq;

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
					flatten_join_alias_vars(root, root->parse,
											(Node *) rte->subquery);
		}
		else if (rte->rtekind == RTE_FUNCTION)
		{
			/* Preprocess the function expression(s) fully */
			kind = rte->lateral ? EXPRKIND_RTFUNC_LATERAL : EXPRKIND_RTFUNC;
			rte->functions = (List *)
				preprocess_expression(root, (Node *) rte->functions, kind);
		}
		else if (rte->rtekind == RTE_TABLEFUNC)
		{
			/* Preprocess the function expression(s) fully */
			kind = rte->lateral ? EXPRKIND_TABLEFUNC_LATERAL : EXPRKIND_TABLEFUNC;
			rte->tablefunc = (TableFunc *)
				preprocess_expression(root, (Node *) rte->tablefunc, kind);
		}
		else if (rte->rtekind == RTE_VALUES)
		{
			/* Preprocess the values lists fully */
			kind = rte->lateral ? EXPRKIND_VALUES_LATERAL : EXPRKIND_VALUES;
			rte->values_lists = (List *)
				preprocess_expression(root, (Node *) rte->values_lists, kind);
		}
		else if (rte->rtekind == RTE_GROUP)
		{
			/* Preprocess the groupexprs list fully */
			rte->groupexprs = (List *)
				preprocess_expression(root, (Node *) rte->groupexprs,
									  EXPRKIND_GROUPEXPR);
		}

		/*
		 * Process each element of the securityQuals list as if it were a
		 * separate qual expression (as indeed it is).  We need to do it this
		 * way to get proper canonicalization of AND/OR structure.  Note that
		 * this converts each element into an implicit-AND sublist.
		 */
		foreach(lcsq, rte->securityQuals)
		{
			lfirst(lcsq) = preprocess_expression(root,
												 (Node *) lfirst(lcsq),
												 EXPRKIND_QUAL);
		}
	}

	/*
	 * Now that we are done preprocessing expressions, and in particular done
	 * flattening join alias variables, get rid of the joinaliasvars lists.
	 * They no longer match what expressions in the rest of the tree look
	 * like, because we have not preprocessed expressions in those lists (and
	 * do not want to; for example, expanding a SubLink there would result in
	 * a useless unreferenced subplan).  Leaving them in place simply creates
	 * a hazard for later scans of the tree.  We could try to prevent that by
	 * using QTW_IGNORE_JOINALIASES in every tree scan done after this point,
	 * but that doesn't sound very reliable.
	 */
	if (root->hasJoinRTEs)
	{
		foreach(l, parse->rtable)
		{
			RangeTblEntry *rte = lfirst_node(RangeTblEntry, l);

			rte->joinaliasvars = NIL;
		}
	}

	/*
	 * Replace any Vars in the subquery's targetlist and havingQual that
	 * reference GROUP outputs with the underlying grouping expressions.
	 *
	 * Note that we need to perform this replacement after we've preprocessed
	 * the grouping expressions.  This is to ensure that there is only one
	 * instance of SubPlan for each SubLink contained within the grouping
	 * expressions.
	 */
	if (parse->hasGroupRTE)
	{
		parse->targetList = (List *)
			flatten_group_exprs(root, root->parse, (Node *) parse->targetList);
		parse->havingQual =
			flatten_group_exprs(root, root->parse, parse->havingQual);
	}

	/* Constant-folding might have removed all set-returning functions */
	if (parse->hasTargetSRFs)
		parse->hasTargetSRFs = expression_returns_set((Node *) parse->targetList);

	/*
	 * In some cases we may want to transfer a HAVING clause into WHERE. We
	 * cannot do so if the HAVING clause contains aggregates (obviously) or
	 * volatile functions (since a HAVING clause is supposed to be executed
	 * only once per group).  We also can't do this if there are any nonempty
	 * grouping sets and the clause references any columns that are nullable
	 * by the grouping sets; moving such a clause into WHERE would potentially
	 * change the results.  (If there are only empty grouping sets, then the
	 * HAVING clause must be degenerate as discussed below.)
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
	 * Note that a HAVING clause may contain expressions that are not fully
	 * preprocessed.  This can happen if these expressions are part of
	 * grouping items.  In such cases, they are replaced with GROUP Vars in
	 * the parser and then replaced back after we've done with expression
	 * preprocessing on havingQual.  This is not an issue if the clause
	 * remains in HAVING, because these expressions will be matched to lower
	 * target items in setrefs.c.  However, if the clause is moved or copied
	 * into WHERE, we need to ensure that these expressions are fully
	 * preprocessed.
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
			contain_subplans(havingclause) ||
			(parse->groupClause && parse->groupingSets &&
			 bms_is_member(root->group_rtindex, pull_varnos(root, havingclause))))
		{
			/* keep it in HAVING */
			newHaving = lappend(newHaving, havingclause);
		}
		else if (parse->groupClause)
		{
			Node	   *whereclause;

			/* Preprocess the HAVING clause fully */
			whereclause = preprocess_expression(root, havingclause,
												EXPRKIND_QUAL);
			/* ... and move it to WHERE */
			parse->jointree->quals = (Node *)
				list_concat((List *) parse->jointree->quals,
							(List *) whereclause);
		}
		else
		{
			Node	   *whereclause;

			/* Preprocess the HAVING clause fully */
			whereclause = preprocess_expression(root, copyObject(havingclause),
												EXPRKIND_QUAL);
			/* ... and put a copy in WHERE */
			parse->jointree->quals = (Node *)
				list_concat((List *) parse->jointree->quals,
							(List *) whereclause);
			/* ... and also keep it in HAVING */
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
	 * If we have any RTE_RESULT relations, see if they can be deleted from
	 * the jointree.  We also rely on this processing to flatten single-child
	 * FromExprs underneath outer joins.  This step is most effectively done
	 * after we've done expression preprocessing and outer join reduction.
	 */
	if (hasResultRTEs || hasOuterJoins)
		remove_useless_result_rtes(root);

	/*
	 * Do the main planning.
	 */
	grouping_planner(root, tuple_fraction, setops);

	/*
	 * Capture the set of outer-level param IDs we have access to, for use in
	 * extParam/allParam calculations later.
	 */
	SS_identify_outer_params(root);

	/*
	 * If any initPlans were created in this query level, adjust the surviving
	 * Paths' costs and parallel-safety flags to account for them.  The
	 * initPlans won't actually get attached to the plan tree till
	 * create_plan() runs, but we must include their effects now.
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
	 * base-relation variables.  We must do this first, since any expressions
	 * we may extract from the joinaliasvars lists have not been preprocessed.
	 * For example, if we did this after sublink processing, sublinks expanded
	 * out from join aliases would not get processed.  But we can skip this in
	 * non-lateral RTE functions, VALUES lists, and TABLESAMPLE clauses, since
	 * they can't contain any Vars of the current query level.
	 */
	if (root->hasJoinRTEs &&
		!(kind == EXPRKIND_RTFUNC ||
		  kind == EXPRKIND_VALUES ||
		  kind == EXPRKIND_TABLESAMPLE ||
		  kind == EXPRKIND_TABLEFUNC))
		expr = flatten_join_alias_vars(root, root->parse, expr);

	/*
	 * Simplify constant expressions.  For function RTEs, this was already
	 * done by preprocess_function_rtes.  (But note we must do it again for
	 * EXPRKIND_RTFUNC_LATERAL, because those might by now contain
	 * un-simplified subexpressions inserted by flattening of subqueries or
	 * join alias variables.)
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
	if (kind != EXPRKIND_RTFUNC)
		expr = eval_const_expressions(root, expr);

	/*
	 * If it's a qual or havingQual, canonicalize it.
	 */
	if (kind == EXPRKIND_QUAL)
	{
		expr = (Node *) canonicalize_qual((Expr *) expr, false);

#ifdef OPTIMIZER_DEBUG
		printf("After canonicalize_qual()\n");
		pprint(expr);
#endif
	}

	/*
	 * Check for ANY ScalarArrayOpExpr with Const arrays and set the
	 * hashfuncid of any that might execute more quickly by using hash lookups
	 * instead of a linear search.
	 */
	if (kind == EXPRKIND_QUAL || kind == EXPRKIND_TARGET)
	{
		convert_saop_to_hashed_saop(expr);
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

/*--------------------
 * grouping_planner
 *	  Perform planning steps related to grouping, aggregation, etc.
 *
 * This function adds all required top-level processing to the scan/join
 * Path(s) produced by query_planner.
 *
 * tuple_fraction is the fraction of tuples we expect will be retrieved.
 * tuple_fraction is interpreted as follows:
 *	  0: expect all tuples to be retrieved (normal case)
 *	  0 < tuple_fraction < 1: expect the given fraction of tuples available
 *		from the plan to be retrieved
 *	  tuple_fraction >= 1: tuple_fraction is the absolute number of tuples
 *		expected to be retrieved (ie, a LIMIT specification).
 * setops is used for set operation subqueries to provide the subquery with
 * the context in which it's being used so that Paths correctly sorted for the
 * set operation can be generated.  NULL when not planning a set operation
 * child.
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
grouping_planner(PlannerInfo *root, double tuple_fraction,
				 SetOperationStmt *setops)
{
	Query	   *parse = root->parse;
	int64		offset_est = 0;
	int64		count_est = 0;
	double		limit_tuples = -1.0;
	bool		have_postponed_srfs = false;
	PathTarget *final_target;
	List	   *final_targets;
	List	   *final_targets_contain_srfs;
	bool		final_target_parallel_safe;
	RelOptInfo *current_rel;
	RelOptInfo *final_rel;
	FinalPathExtraData extra;
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
		 * Construct Paths for set operations.  The results will not need any
		 * work except perhaps a top-level sort and/or LIMIT.  Note that any
		 * special work for recursive unions is the responsibility of
		 * plan_set_operations.
		 */
		current_rel = plan_set_operations(root);

		/*
		 * We should not need to call preprocess_targetlist, since we must be
		 * in a SELECT query node.  Instead, use the processed_tlist returned
		 * by plan_set_operations (since this tells whether it returned any
		 * resjunk columns!), and transfer any sort key information from the
		 * original tlist.
		 */
		Assert(parse->commandType == CMD_SELECT);

		/* for safety, copy processed_tlist instead of modifying in-place */
		root->processed_tlist =
			postprocess_setop_tlist(copyObject(root->processed_tlist),
									parse->targetList);

		/* Also extract the PathTarget form of the setop result tlist */
		final_target = current_rel->cheapest_total_path->pathtarget;

		/* And check whether it's parallel safe */
		final_target_parallel_safe =
			is_parallel_safe(root, (Node *) final_target->exprs);

		/* The setop result tlist couldn't contain any SRFs */
		Assert(!parse->hasTargetSRFs);
		final_targets = final_targets_contain_srfs = NIL;

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
							LCS_asString(linitial_node(RowMarkClause,
													   parse->rowMarks)->strength))));

		/*
		 * Calculate pathkeys that represent result ordering requirements
		 */
		Assert(parse->distinctClause == NIL);
		root->sort_pathkeys = make_pathkeys_for_sortclauses(root,
															parse->sortClause,
															root->processed_tlist);
	}
	else
	{
		/* No set operations, do regular planning */
		PathTarget *sort_input_target;
		List	   *sort_input_targets;
		List	   *sort_input_targets_contain_srfs;
		bool		sort_input_target_parallel_safe;
		PathTarget *grouping_target;
		List	   *grouping_targets;
		List	   *grouping_targets_contain_srfs;
		bool		grouping_target_parallel_safe;
		PathTarget *scanjoin_target;
		List	   *scanjoin_targets;
		List	   *scanjoin_targets_contain_srfs;
		bool		scanjoin_target_parallel_safe;
		bool		scanjoin_target_same_exprs;
		bool		have_grouping;
		WindowFuncLists *wflists = NULL;
		List	   *activeWindows = NIL;
		grouping_sets_data *gset_data = NULL;
		standard_qp_extra qp_extra;

		/* A recursive query should always have setOperations */
		Assert(!root->hasRecursion);

		/* Preprocess grouping sets and GROUP BY clause, if any */
		if (parse->groupingSets)
		{
			gset_data = preprocess_grouping_sets(root);
		}
		else if (parse->groupClause)
		{
			/* Preprocess regular GROUP BY clause, if any */
			root->processed_groupClause = preprocess_groupclause(root, NIL);
		}

		/*
		 * Preprocess targetlist.  Note that much of the remaining planning
		 * work will be done with the PathTarget representation of tlists, but
		 * we must also maintain the full representation of the final tlist so
		 * that we can transfer its decoration (resnames etc) to the topmost
		 * tlist of the finished Plan.  This is kept in processed_tlist.
		 */
		preprocess_targetlist(root);

		/*
		 * Mark all the aggregates with resolved aggtranstypes, and detect
		 * aggregates that are duplicates or can share transition state.  We
		 * must do this before slicing and dicing the tlist into various
		 * pathtargets, else some copies of the Aggref nodes might escape
		 * being marked.
		 */
		if (parse->hasAggs)
		{
			preprocess_aggrefs(root, (Node *) root->processed_tlist);
			preprocess_aggrefs(root, (Node *) parse->havingQual);
		}

		/*
		 * Locate any window functions in the tlist.  (We don't need to look
		 * anywhere else, since expressions used in ORDER BY will be in there
		 * too.)  Note that they could all have been eliminated by constant
		 * folding, in which case we don't need to do any more work.
		 */
		if (parse->hasWindowFuncs)
		{
			wflists = find_window_functions((Node *) root->processed_tlist,
											list_length(parse->windowClause));
			if (wflists->numWindowFuncs > 0)
			{
				/*
				 * See if any modifications can be made to each WindowClause
				 * to allow the executor to execute the WindowFuncs more
				 * quickly.
				 */
				optimize_window_clauses(root, wflists);

				activeWindows = select_active_windows(root, wflists);
			}
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
			preprocess_minmax_aggregates(root);

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
		qp_extra.activeWindows = activeWindows;
		qp_extra.gset_data = gset_data;

		/*
		 * If we're a subquery for a set operation, store the SetOperationStmt
		 * in qp_extra.
		 */
		qp_extra.setop = setops;

		/*
		 * Generate the best unsorted and presorted paths for the scan/join
		 * portion of this Query, ie the processing represented by the
		 * FROM/WHERE clauses.  (Note there may not be any presorted paths.)
		 * We also generate (in standard_qp_callback) pathkey representations
		 * of the query's sort clause, distinct clause, etc.
		 */
		current_rel = query_planner(root, standard_qp_callback, &qp_extra);

		/*
		 * Convert the query's result tlist into PathTarget format.
		 *
		 * Note: this cannot be done before query_planner() has performed
		 * appendrel expansion, because that might add resjunk entries to
		 * root->processed_tlist.  Waiting till afterwards is also helpful
		 * because the target width estimates can use per-Var width numbers
		 * that were obtained within query_planner().
		 */
		final_target = create_pathtarget(root, root->processed_tlist);
		final_target_parallel_safe =
			is_parallel_safe(root, (Node *) final_target->exprs);

		/*
		 * If ORDER BY was given, consider whether we should use a post-sort
		 * projection, and compute the adjusted target for preceding steps if
		 * so.
		 */
		if (parse->sortClause)
		{
			sort_input_target = make_sort_input_target(root,
													   final_target,
													   &have_postponed_srfs);
			sort_input_target_parallel_safe =
				is_parallel_safe(root, (Node *) sort_input_target->exprs);
		}
		else
		{
			sort_input_target = final_target;
			sort_input_target_parallel_safe = final_target_parallel_safe;
		}

		/*
		 * If we have window functions to deal with, the output from any
		 * grouping step needs to be what the window functions want;
		 * otherwise, it should be sort_input_target.
		 */
		if (activeWindows)
		{
			grouping_target = make_window_input_target(root,
													   final_target,
													   activeWindows);
			grouping_target_parallel_safe =
				is_parallel_safe(root, (Node *) grouping_target->exprs);
		}
		else
		{
			grouping_target = sort_input_target;
			grouping_target_parallel_safe = sort_input_target_parallel_safe;
		}

		/*
		 * If we have grouping or aggregation to do, the topmost scan/join
		 * plan node must emit what the grouping step wants; otherwise, it
		 * should emit grouping_target.
		 */
		have_grouping = (parse->groupClause || parse->groupingSets ||
						 parse->hasAggs || root->hasHavingQual);
		if (have_grouping)
		{
			scanjoin_target = make_group_input_target(root, final_target);
			scanjoin_target_parallel_safe =
				is_parallel_safe(root, (Node *) scanjoin_target->exprs);
		}
		else
		{
			scanjoin_target = grouping_target;
			scanjoin_target_parallel_safe = grouping_target_parallel_safe;
		}

		/*
		 * If there are any SRFs in the targetlist, we must separate each of
		 * these PathTargets into SRF-computing and SRF-free targets.  Replace
		 * each of the named targets with a SRF-free version, and remember the
		 * list of additional projection steps we need to add afterwards.
		 */
		if (parse->hasTargetSRFs)
		{
			/* final_target doesn't recompute any SRFs in sort_input_target */
			split_pathtarget_at_srfs(root, final_target, sort_input_target,
									 &final_targets,
									 &final_targets_contain_srfs);
			final_target = linitial_node(PathTarget, final_targets);
			Assert(!linitial_int(final_targets_contain_srfs));
			/* likewise for sort_input_target vs. grouping_target */
			split_pathtarget_at_srfs(root, sort_input_target, grouping_target,
									 &sort_input_targets,
									 &sort_input_targets_contain_srfs);
			sort_input_target = linitial_node(PathTarget, sort_input_targets);
			Assert(!linitial_int(sort_input_targets_contain_srfs));
			/* likewise for grouping_target vs. scanjoin_target */
			split_pathtarget_at_srfs(root, grouping_target, scanjoin_target,
									 &grouping_targets,
									 &grouping_targets_contain_srfs);
			grouping_target = linitial_node(PathTarget, grouping_targets);
			Assert(!linitial_int(grouping_targets_contain_srfs));
			/* scanjoin_target will not have any SRFs precomputed for it */
			split_pathtarget_at_srfs(root, scanjoin_target, NULL,
									 &scanjoin_targets,
									 &scanjoin_targets_contain_srfs);
			scanjoin_target = linitial_node(PathTarget, scanjoin_targets);
			Assert(!linitial_int(scanjoin_targets_contain_srfs));
		}
		else
		{
			/* initialize lists; for most of these, dummy values are OK */
			final_targets = final_targets_contain_srfs = NIL;
			sort_input_targets = sort_input_targets_contain_srfs = NIL;
			grouping_targets = grouping_targets_contain_srfs = NIL;
			scanjoin_targets = list_make1(scanjoin_target);
			scanjoin_targets_contain_srfs = NIL;
		}

		/* Apply scan/join target. */
		scanjoin_target_same_exprs = list_length(scanjoin_targets) == 1
			&& equal(scanjoin_target->exprs, current_rel->reltarget->exprs);
		apply_scanjoin_target_to_paths(root, current_rel, scanjoin_targets,
									   scanjoin_targets_contain_srfs,
									   scanjoin_target_parallel_safe,
									   scanjoin_target_same_exprs);

		/*
		 * Save the various upper-rel PathTargets we just computed into
		 * root->upper_targets[].  The core code doesn't use this, but it
		 * provides a convenient place for extensions to get at the info.  For
		 * consistency, we save all the intermediate targets, even though some
		 * of the corresponding upperrels might not be needed for this query.
		 */
		root->upper_targets[UPPERREL_FINAL] = final_target;
		root->upper_targets[UPPERREL_ORDERED] = final_target;
		root->upper_targets[UPPERREL_DISTINCT] = sort_input_target;
		root->upper_targets[UPPERREL_PARTIAL_DISTINCT] = sort_input_target;
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
												grouping_target_parallel_safe,
												gset_data);
			/* Fix things up if grouping_target contains SRFs */
			if (parse->hasTargetSRFs)
				adjust_paths_for_srfs(root, current_rel,
									  grouping_targets,
									  grouping_targets_contain_srfs);
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
											  sort_input_target_parallel_safe,
											  wflists,
											  activeWindows);
			/* Fix things up if sort_input_target contains SRFs */
			if (parse->hasTargetSRFs)
				adjust_paths_for_srfs(root, current_rel,
									  sort_input_targets,
									  sort_input_targets_contain_srfs);
		}

		/*
		 * If there is a DISTINCT clause, consider ways to implement that. We
		 * build a new upperrel representing the output of this phase.
		 */
		if (parse->distinctClause)
		{
			current_rel = create_distinct_paths(root,
												current_rel,
												sort_input_target);
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
										   final_target_parallel_safe,
										   have_postponed_srfs ? -1.0 :
										   limit_tuples);
		/* Fix things up if final_target contains SRFs */
		if (parse->hasTargetSRFs)
			adjust_paths_for_srfs(root, current_rel,
								  final_targets,
								  final_targets_contain_srfs);
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
												 assign_special_exec_param(root));
		}

		/*
		 * If there is a LIMIT/OFFSET clause, add the LIMIT node.
		 */
		if (limit_needed(parse))
		{
			path = (Path *) create_limit_path(root, final_rel, path,
											  parse->limitOffset,
											  parse->limitCount,
											  parse->limitOption,
											  offset_est, count_est);
		}

		/*
		 * If this is an INSERT/UPDATE/DELETE/MERGE, add the ModifyTable node.
		 */
		if (parse->commandType != CMD_SELECT)
		{
			Index		rootRelation;
			List	   *resultRelations = NIL;
			List	   *updateColnosLists = NIL;
			List	   *withCheckOptionLists = NIL;
			List	   *returningLists = NIL;
			List	   *mergeActionLists = NIL;
			List	   *mergeJoinConditions = NIL;
			List	   *rowMarks;

			if (bms_membership(root->all_result_relids) == BMS_MULTIPLE)
			{
				/* Inherited UPDATE/DELETE/MERGE */
				RelOptInfo *top_result_rel = find_base_rel(root,
														   parse->resultRelation);
				int			resultRelation = -1;

				/* Pass the root result rel forward to the executor. */
				rootRelation = parse->resultRelation;

				/* Add only leaf children to ModifyTable. */
				while ((resultRelation = bms_next_member(root->leaf_result_relids,
														 resultRelation)) >= 0)
				{
					RelOptInfo *this_result_rel = find_base_rel(root,
																resultRelation);

					/*
					 * Also exclude any leaf rels that have turned dummy since
					 * being added to the list, for example, by being excluded
					 * by constraint exclusion.
					 */
					if (IS_DUMMY_REL(this_result_rel))
						continue;

					/* Build per-target-rel lists needed by ModifyTable */
					resultRelations = lappend_int(resultRelations,
												  resultRelation);
					if (parse->commandType == CMD_UPDATE)
					{
						List	   *update_colnos = root->update_colnos;

						if (this_result_rel != top_result_rel)
							update_colnos =
								adjust_inherited_attnums_multilevel(root,
																	update_colnos,
																	this_result_rel->relid,
																	top_result_rel->relid);
						updateColnosLists = lappend(updateColnosLists,
													update_colnos);
					}
					if (parse->withCheckOptions)
					{
						List	   *withCheckOptions = parse->withCheckOptions;

						if (this_result_rel != top_result_rel)
							withCheckOptions = (List *)
								adjust_appendrel_attrs_multilevel(root,
																  (Node *) withCheckOptions,
																  this_result_rel,
																  top_result_rel);
						withCheckOptionLists = lappend(withCheckOptionLists,
													   withCheckOptions);
					}
					if (parse->returningList)
					{
						List	   *returningList = parse->returningList;

						if (this_result_rel != top_result_rel)
							returningList = (List *)
								adjust_appendrel_attrs_multilevel(root,
																  (Node *) returningList,
																  this_result_rel,
																  top_result_rel);
						returningLists = lappend(returningLists,
												 returningList);
					}
					if (parse->mergeActionList)
					{
						ListCell   *l;
						List	   *mergeActionList = NIL;

						/*
						 * Copy MergeActions and translate stuff that
						 * references attribute numbers.
						 */
						foreach(l, parse->mergeActionList)
						{
							MergeAction *action = lfirst(l),
									   *leaf_action = copyObject(action);

							leaf_action->qual =
								adjust_appendrel_attrs_multilevel(root,
																  (Node *) action->qual,
																  this_result_rel,
																  top_result_rel);
							leaf_action->targetList = (List *)
								adjust_appendrel_attrs_multilevel(root,
																  (Node *) action->targetList,
																  this_result_rel,
																  top_result_rel);
							if (leaf_action->commandType == CMD_UPDATE)
								leaf_action->updateColnos =
									adjust_inherited_attnums_multilevel(root,
																		action->updateColnos,
																		this_result_rel->relid,
																		top_result_rel->relid);
							mergeActionList = lappend(mergeActionList,
													  leaf_action);
						}

						mergeActionLists = lappend(mergeActionLists,
												   mergeActionList);
					}
					if (parse->commandType == CMD_MERGE)
					{
						Node	   *mergeJoinCondition = parse->mergeJoinCondition;

						if (this_result_rel != top_result_rel)
							mergeJoinCondition =
								adjust_appendrel_attrs_multilevel(root,
																  mergeJoinCondition,
																  this_result_rel,
																  top_result_rel);
						mergeJoinConditions = lappend(mergeJoinConditions,
													  mergeJoinCondition);
					}
				}

				if (resultRelations == NIL)
				{
					/*
					 * We managed to exclude every child rel, so generate a
					 * dummy one-relation plan using info for the top target
					 * rel (even though that may not be a leaf target).
					 * Although it's clear that no data will be updated or
					 * deleted, we still need to have a ModifyTable node so
					 * that any statement triggers will be executed.  (This
					 * could be cleaner if we fixed nodeModifyTable.c to allow
					 * zero target relations, but that probably wouldn't be a
					 * net win.)
					 */
					resultRelations = list_make1_int(parse->resultRelation);
					if (parse->commandType == CMD_UPDATE)
						updateColnosLists = list_make1(root->update_colnos);
					if (parse->withCheckOptions)
						withCheckOptionLists = list_make1(parse->withCheckOptions);
					if (parse->returningList)
						returningLists = list_make1(parse->returningList);
					if (parse->mergeActionList)
						mergeActionLists = list_make1(parse->mergeActionList);
					if (parse->commandType == CMD_MERGE)
						mergeJoinConditions = list_make1(parse->mergeJoinCondition);
				}
			}
			else
			{
				/* Single-relation INSERT/UPDATE/DELETE/MERGE. */
				rootRelation = 0;	/* there's no separate root rel */
				resultRelations = list_make1_int(parse->resultRelation);
				if (parse->commandType == CMD_UPDATE)
					updateColnosLists = list_make1(root->update_colnos);
				if (parse->withCheckOptions)
					withCheckOptionLists = list_make1(parse->withCheckOptions);
				if (parse->returningList)
					returningLists = list_make1(parse->returningList);
				if (parse->mergeActionList)
					mergeActionLists = list_make1(parse->mergeActionList);
				if (parse->commandType == CMD_MERGE)
					mergeJoinConditions = list_make1(parse->mergeJoinCondition);
			}

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
										path,
										parse->commandType,
										parse->canSetTag,
										parse->resultRelation,
										rootRelation,
										root->partColsUpdated,
										resultRelations,
										updateColnosLists,
										withCheckOptionLists,
										returningLists,
										rowMarks,
										parse->onConflict,
										mergeActionLists,
										mergeJoinConditions,
										assign_special_exec_param(root));
		}

		/* And shove it into final_rel */
		add_path(final_rel, path);
	}

	/*
	 * Generate partial paths for final_rel, too, if outer query levels might
	 * be able to make use of them.
	 */
	if (final_rel->consider_parallel && root->query_level > 1 &&
		!limit_needed(parse))
	{
		Assert(!parse->rowMarks && parse->commandType == CMD_SELECT);
		foreach(lc, current_rel->partial_pathlist)
		{
			Path	   *partial_path = (Path *) lfirst(lc);

			add_partial_path(final_rel, partial_path);
		}
	}

	extra.limit_needed = limit_needed(parse);
	extra.limit_tuples = limit_tuples;
	extra.count_est = count_est;
	extra.offset_est = offset_est;

	/*
	 * If there is an FDW that's responsible for all baserels of the query,
	 * let it consider adding ForeignPaths.
	 */
	if (final_rel->fdwroutine &&
		final_rel->fdwroutine->GetForeignUpperPaths)
		final_rel->fdwroutine->GetForeignUpperPaths(root, UPPERREL_FINAL,
													current_rel, final_rel,
													&extra);

	/* Let extensions possibly add some more paths */
	if (create_upper_paths_hook)
		(*create_upper_paths_hook) (root, UPPERREL_FINAL,
									current_rel, final_rel, &extra);

	/* Note: currently, we leave it to callers to do set_cheapest() */
}

/*
 * Do preprocessing for groupingSets clause and related data.  This handles the
 * preliminary steps of expanding the grouping sets, organizing them into lists
 * of rollups, and preparing annotations which will later be filled in with
 * size estimates.
 */
static grouping_sets_data *
preprocess_grouping_sets(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	List	   *sets;
	int			maxref = 0;
	ListCell   *lc_set;
	grouping_sets_data *gd = palloc0(sizeof(grouping_sets_data));

	parse->groupingSets = expand_grouping_sets(parse->groupingSets, parse->groupDistinct, -1);

	gd->any_hashable = false;
	gd->unhashable_refs = NULL;
	gd->unsortable_refs = NULL;
	gd->unsortable_sets = NIL;

	/*
	 * We don't currently make any attempt to optimize the groupClause when
	 * there are grouping sets, so just duplicate it in processed_groupClause.
	 */
	root->processed_groupClause = parse->groupClause;

	if (parse->groupClause)
	{
		ListCell   *lc;

		foreach(lc, parse->groupClause)
		{
			SortGroupClause *gc = lfirst_node(SortGroupClause, lc);
			Index		ref = gc->tleSortGroupRef;

			if (ref > maxref)
				maxref = ref;

			if (!gc->hashable)
				gd->unhashable_refs = bms_add_member(gd->unhashable_refs, ref);

			if (!OidIsValid(gc->sortop))
				gd->unsortable_refs = bms_add_member(gd->unsortable_refs, ref);
		}
	}

	/* Allocate workspace array for remapping */
	gd->tleref_to_colnum_map = (int *) palloc((maxref + 1) * sizeof(int));

	/*
	 * If we have any unsortable sets, we must extract them before trying to
	 * prepare rollups. Unsortable sets don't go through
	 * reorder_grouping_sets, so we must apply the GroupingSetData annotation
	 * here.
	 */
	if (!bms_is_empty(gd->unsortable_refs))
	{
		List	   *sortable_sets = NIL;
		ListCell   *lc;

		foreach(lc, parse->groupingSets)
		{
			List	   *gset = (List *) lfirst(lc);

			if (bms_overlap_list(gd->unsortable_refs, gset))
			{
				GroupingSetData *gs = makeNode(GroupingSetData);

				gs->set = gset;
				gd->unsortable_sets = lappend(gd->unsortable_sets, gs);

				/*
				 * We must enforce here that an unsortable set is hashable;
				 * later code assumes this.  Parse analysis only checks that
				 * every individual column is either hashable or sortable.
				 *
				 * Note that passing this test doesn't guarantee we can
				 * generate a plan; there might be other showstoppers.
				 */
				if (bms_overlap_list(gd->unhashable_refs, gset))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("could not implement GROUP BY"),
							 errdetail("Some of the datatypes only support hashing, while others only support sorting.")));
			}
			else
				sortable_sets = lappend(sortable_sets, gset);
		}

		if (sortable_sets)
			sets = extract_rollup_sets(sortable_sets);
		else
			sets = NIL;
	}
	else
		sets = extract_rollup_sets(parse->groupingSets);

	foreach(lc_set, sets)
	{
		List	   *current_sets = (List *) lfirst(lc_set);
		RollupData *rollup = makeNode(RollupData);
		GroupingSetData *gs;

		/*
		 * Reorder the current list of grouping sets into correct prefix
		 * order.  If only one aggregation pass is needed, try to make the
		 * list match the ORDER BY clause; if more than one pass is needed, we
		 * don't bother with that.
		 *
		 * Note that this reorders the sets from smallest-member-first to
		 * largest-member-first, and applies the GroupingSetData annotations,
		 * though the data will be filled in later.
		 */
		current_sets = reorder_grouping_sets(current_sets,
											 (list_length(sets) == 1
											  ? parse->sortClause
											  : NIL));

		/*
		 * Get the initial (and therefore largest) grouping set.
		 */
		gs = linitial_node(GroupingSetData, current_sets);

		/*
		 * Order the groupClause appropriately.  If the first grouping set is
		 * empty, then the groupClause must also be empty; otherwise we have
		 * to force the groupClause to match that grouping set's order.
		 *
		 * (The first grouping set can be empty even though parse->groupClause
		 * is not empty only if all non-empty grouping sets are unsortable.
		 * The groupClauses for hashed grouping sets are built later on.)
		 */
		if (gs->set)
			rollup->groupClause = preprocess_groupclause(root, gs->set);
		else
			rollup->groupClause = NIL;

		/*
		 * Is it hashable? We pretend empty sets are hashable even though we
		 * actually force them not to be hashed later. But don't bother if
		 * there's nothing but empty sets (since in that case we can't hash
		 * anything).
		 */
		if (gs->set &&
			!bms_overlap_list(gd->unhashable_refs, gs->set))
		{
			rollup->hashable = true;
			gd->any_hashable = true;
		}

		/*
		 * Now that we've pinned down an order for the groupClause for this
		 * list of grouping sets, we need to remap the entries in the grouping
		 * sets from sortgrouprefs to plain indices (0-based) into the
		 * groupClause for this collection of grouping sets. We keep the
		 * original form for later use, though.
		 */
		rollup->gsets = remap_to_groupclause_idx(rollup->groupClause,
												 current_sets,
												 gd->tleref_to_colnum_map);
		rollup->gsets_data = current_sets;

		gd->rollups = lappend(gd->rollups, rollup);
	}

	if (gd->unsortable_sets)
	{
		/*
		 * We have not yet pinned down a groupclause for this, but we will
		 * need index-based lists for estimation purposes. Construct
		 * hash_sets_idx based on the entire original groupclause for now.
		 */
		gd->hash_sets_idx = remap_to_groupclause_idx(parse->groupClause,
													 gd->unsortable_sets,
													 gd->tleref_to_colnum_map);
		gd->any_hashable = true;
	}

	return gd;
}

/*
 * Given a groupclause and a list of GroupingSetData, return equivalent sets
 * (without annotation) mapped to indexes into the given groupclause.
 */
static List *
remap_to_groupclause_idx(List *groupClause,
						 List *gsets,
						 int *tleref_to_colnum_map)
{
	int			ref = 0;
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, groupClause)
	{
		SortGroupClause *gc = lfirst_node(SortGroupClause, lc);

		tleref_to_colnum_map[gc->tleSortGroupRef] = ref++;
	}

	foreach(lc, gsets)
	{
		List	   *set = NIL;
		ListCell   *lc2;
		GroupingSetData *gs = lfirst_node(GroupingSetData, lc);

		foreach(lc2, gs->set)
		{
			set = lappend_int(set, tleref_to_colnum_map[lfirst_int(lc2)]);
		}

		result = lappend(result, set);
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
		CheckSelectLocking(parse, linitial_node(RowMarkClause,
												parse->rowMarks)->strength);
	}
	else
	{
		/*
		 * We only need rowmarks for UPDATE, DELETE, MERGE, or FOR [KEY]
		 * UPDATE/SHARE.
		 */
		if (parse->commandType != CMD_UPDATE &&
			parse->commandType != CMD_DELETE &&
			parse->commandType != CMD_MERGE)
			return;
	}

	/*
	 * We need to have rowmarks for all base relations except the target. We
	 * make a bitmapset of all base rels and then remove the items we don't
	 * need or have FOR [KEY] UPDATE/SHARE marks for.
	 */
	rels = get_relids_in_jointree((Node *) parse->jointree, false, false);
	if (parse->resultRelation)
		rels = bms_del_member(rels, parse->resultRelation);

	/*
	 * Convert RowMarkClauses to PlanRowMark representation.
	 */
	prowmarks = NIL;
	foreach(l, parse->rowMarks)
	{
		RowMarkClause *rc = lfirst_node(RowMarkClause, l);
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
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, l);
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
		newrc->waitPolicy = LockWaitBlock;	/* doesn't matter */
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
				 * the row.
				 */
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
		return ROW_MARK_EXCLUSIVE;	/* keep compiler quiet */
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
					*count_est = 1; /* force to at least 1 */
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
					tuple_fraction = 0.0;	/* assume fetch all */
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
bool
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
 * preprocess_groupclause - do preparatory work on GROUP BY clause
 *
 * The idea here is to adjust the ordering of the GROUP BY elements
 * (which in itself is semantically insignificant) to match ORDER BY,
 * thereby allowing a single sort operation to both implement the ORDER BY
 * requirement and set up for a Unique step that implements GROUP BY.
 * We also consider partial match between GROUP BY and ORDER BY elements,
 * which could allow to implement ORDER BY using the incremental sort.
 *
 * We also consider other orderings of the GROUP BY elements, which could
 * match the sort ordering of other possible plans (eg an indexscan) and
 * thereby reduce cost.  This is implemented during the generation of grouping
 * paths.  See get_useful_group_keys_orderings() for details.
 *
 * Note: we need no comparable processing of the distinctClause because
 * the parser already enforced that that matches ORDER BY.
 *
 * Note: we return a fresh List, but its elements are the same
 * SortGroupClauses appearing in parse->groupClause.  This is important
 * because later processing may modify the processed_groupClause list.
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
		return list_copy(parse->groupClause);

	/*
	 * Scan the ORDER BY clause and construct a list of matching GROUP BY
	 * items, but only as far as we can make a matching prefix.
	 *
	 * This code assumes that the sortClause contains no duplicate items.
	 */
	foreach(sl, parse->sortClause)
	{
		SortGroupClause *sc = lfirst_node(SortGroupClause, sl);

		foreach(gl, parse->groupClause)
		{
			SortGroupClause *gc = lfirst_node(SortGroupClause, gl);

			if (equal(gc, sc))
			{
				new_groupclause = lappend(new_groupclause, gc);
				break;
			}
		}
		if (gl == NULL)
			break;				/* no match, so stop scanning */
	}


	/* If no match at all, no point in reordering GROUP BY */
	if (new_groupclause == NIL)
		return list_copy(parse->groupClause);

	/*
	 * Add any remaining GROUP BY items to the new list.  We don't require a
	 * complete match, because even partial match allows ORDER BY to be
	 * implemented using incremental sort.  Also, give up if there are any
	 * non-sortable GROUP BY items, since then there's no hope anyway.
	 */
	foreach(gl, parse->groupClause)
	{
		SortGroupClause *gc = lfirst_node(SortGroupClause, gl);

		if (list_member_ptr(new_groupclause, gc))
			continue;			/* it matched an ORDER BY item */
		if (!OidIsValid(gc->sortop))	/* give up, GROUP BY can't be sorted */
			return list_copy(parse->groupClause);
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
		lc1 = lnext(groupingSets, lc1);
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

	for_each_cell(lc, groupingSets, lc1)
	{
		List	   *candidate = (List *) lfirst(lc);
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
 * prefix relationships. Also inserts the GroupingSetData annotations.
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
reorder_grouping_sets(List *groupingSets, List *sortclause)
{
	ListCell   *lc;
	List	   *previous = NIL;
	List	   *result = NIL;

	foreach(lc, groupingSets)
	{
		List	   *candidate = (List *) lfirst(lc);
		List	   *new_elems = list_difference_int(candidate, previous);
		GroupingSetData *gs = makeNode(GroupingSetData);

		while (list_length(sortclause) > list_length(previous) &&
			   new_elems != NIL)
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

		previous = list_concat(previous, new_elems);

		gs->set = list_copy(previous);
		result = lcons(gs, result);
	}

	list_free(previous);

	return result;
}

/*
 * has_volatile_pathkey
 *		Returns true if any PathKey in 'keys' has an EquivalenceClass
 *		containing a volatile function.  Otherwise returns false.
 */
static bool
has_volatile_pathkey(List *keys)
{
	ListCell   *lc;

	foreach(lc, keys)
	{
		PathKey    *pathkey = lfirst_node(PathKey, lc);

		if (pathkey->pk_eclass->ec_has_volatile)
			return true;
	}

	return false;
}

/*
 * adjust_group_pathkeys_for_groupagg
 *		Add pathkeys to root->group_pathkeys to reflect the best set of
 *		pre-ordered input for ordered aggregates.
 *
 * We define "best" as the pathkeys that suit the largest number of
 * aggregate functions.  We find these by looking at the first ORDER BY /
 * DISTINCT aggregate and take the pathkeys for that before searching for
 * other aggregates that require the same or a more strict variation of the
 * same pathkeys.  We then repeat that process for any remaining aggregates
 * with different pathkeys and if we find another set of pathkeys that suits a
 * larger number of aggregates then we select those pathkeys instead.
 *
 * When the best pathkeys are found we also mark each Aggref that can use
 * those pathkeys as aggpresorted = true.
 *
 * Note: When an aggregate function's ORDER BY / DISTINCT clause contains any
 * volatile functions, we never make use of these pathkeys.  We want to ensure
 * that sorts using volatile functions are done independently in each Aggref
 * rather than once at the query level.  If we were to allow this then Aggrefs
 * with compatible sort orders would all transition their rows in the same
 * order if those pathkeys were deemed to be the best pathkeys to sort on.
 * Whereas, if some other set of Aggref's pathkeys happened to be deemed
 * better pathkeys to sort on, then the volatile function Aggrefs would be
 * left to perform their sorts individually.  To avoid this inconsistent
 * behavior which could make Aggref results depend on what other Aggrefs the
 * query contains, we always force Aggrefs with volatile functions to perform
 * their own sorts.
 */
static void
adjust_group_pathkeys_for_groupagg(PlannerInfo *root)
{
	List	   *grouppathkeys = root->group_pathkeys;
	List	   *bestpathkeys;
	Bitmapset  *bestaggs;
	Bitmapset  *unprocessed_aggs;
	ListCell   *lc;
	int			i;

	/* Shouldn't be here if there are grouping sets */
	Assert(root->parse->groupingSets == NIL);
	/* Shouldn't be here unless there are some ordered aggregates */
	Assert(root->numOrderedAggs > 0);

	/* Do nothing if disabled */
	if (!enable_presorted_aggregate)
		return;

	/*
	 * Make a first pass over all AggInfos to collect a Bitmapset containing
	 * the indexes of all AggInfos to be processed below.
	 */
	unprocessed_aggs = NULL;
	foreach(lc, root->agginfos)
	{
		AggInfo    *agginfo = lfirst_node(AggInfo, lc);
		Aggref	   *aggref = linitial_node(Aggref, agginfo->aggrefs);

		if (AGGKIND_IS_ORDERED_SET(aggref->aggkind))
			continue;

		/* only add aggregates with a DISTINCT or ORDER BY */
		if (aggref->aggdistinct != NIL || aggref->aggorder != NIL)
			unprocessed_aggs = bms_add_member(unprocessed_aggs,
											  foreach_current_index(lc));
	}

	/*
	 * Now process all the unprocessed_aggs to find the best set of pathkeys
	 * for the given set of aggregates.
	 *
	 * On the first outer loop here 'bestaggs' will be empty.   We'll populate
	 * this during the first loop using the pathkeys for the very first
	 * AggInfo then taking any stronger pathkeys from any other AggInfos with
	 * a more strict set of compatible pathkeys.  Once the outer loop is
	 * complete, we mark off all the aggregates with compatible pathkeys then
	 * remove those from the unprocessed_aggs and repeat the process to try to
	 * find another set of pathkeys that are suitable for a larger number of
	 * aggregates.  The outer loop will stop when there are not enough
	 * unprocessed aggregates for it to be possible to find a set of pathkeys
	 * to suit a larger number of aggregates.
	 */
	bestpathkeys = NIL;
	bestaggs = NULL;
	while (bms_num_members(unprocessed_aggs) > bms_num_members(bestaggs))
	{
		Bitmapset  *aggindexes = NULL;
		List	   *currpathkeys = NIL;

		i = -1;
		while ((i = bms_next_member(unprocessed_aggs, i)) >= 0)
		{
			AggInfo    *agginfo = list_nth_node(AggInfo, root->agginfos, i);
			Aggref	   *aggref = linitial_node(Aggref, agginfo->aggrefs);
			List	   *sortlist;
			List	   *pathkeys;

			if (aggref->aggdistinct != NIL)
				sortlist = aggref->aggdistinct;
			else
				sortlist = aggref->aggorder;

			pathkeys = make_pathkeys_for_sortclauses(root, sortlist,
													 aggref->args);

			/*
			 * Ignore Aggrefs which have volatile functions in their ORDER BY
			 * or DISTINCT clause.
			 */
			if (has_volatile_pathkey(pathkeys))
			{
				unprocessed_aggs = bms_del_member(unprocessed_aggs, i);
				continue;
			}

			/*
			 * When not set yet, take the pathkeys from the first unprocessed
			 * aggregate.
			 */
			if (currpathkeys == NIL)
			{
				currpathkeys = pathkeys;

				/* include the GROUP BY pathkeys, if they exist */
				if (grouppathkeys != NIL)
					currpathkeys = append_pathkeys(list_copy(grouppathkeys),
												   currpathkeys);

				/* record that we found pathkeys for this aggregate */
				aggindexes = bms_add_member(aggindexes, i);
			}
			else
			{
				/* now look for a stronger set of matching pathkeys */

				/* include the GROUP BY pathkeys, if they exist */
				if (grouppathkeys != NIL)
					pathkeys = append_pathkeys(list_copy(grouppathkeys),
											   pathkeys);

				/* are 'pathkeys' compatible or better than 'currpathkeys'? */
				switch (compare_pathkeys(currpathkeys, pathkeys))
				{
					case PATHKEYS_BETTER2:
						/* 'pathkeys' are stronger, use these ones instead */
						currpathkeys = pathkeys;
						/* FALLTHROUGH */

					case PATHKEYS_BETTER1:
						/* 'pathkeys' are less strict */
						/* FALLTHROUGH */

					case PATHKEYS_EQUAL:
						/* mark this aggregate as covered by 'currpathkeys' */
						aggindexes = bms_add_member(aggindexes, i);
						break;

					case PATHKEYS_DIFFERENT:
						break;
				}
			}
		}

		/* remove the aggregates that we've just processed */
		unprocessed_aggs = bms_del_members(unprocessed_aggs, aggindexes);

		/*
		 * If this pass included more aggregates than the previous best then
		 * use these ones as the best set.
		 */
		if (bms_num_members(aggindexes) > bms_num_members(bestaggs))
		{
			bestaggs = aggindexes;
			bestpathkeys = currpathkeys;
		}
	}

	/*
	 * If we found any ordered aggregates, update root->group_pathkeys to add
	 * the best set of aggregate pathkeys.  Note that bestpathkeys includes
	 * the original GROUP BY pathkeys already.
	 */
	if (bestpathkeys != NIL)
		root->group_pathkeys = bestpathkeys;

	/*
	 * Now that we've found the best set of aggregates we can set the
	 * presorted flag to indicate to the executor that it needn't bother
	 * performing a sort for these Aggrefs.  We're able to do this now as
	 * there's no chance of a Hash Aggregate plan as create_grouping_paths
	 * will not mark the GROUP BY as GROUPING_CAN_USE_HASH due to the presence
	 * of ordered aggregates.
	 */
	i = -1;
	while ((i = bms_next_member(bestaggs, i)) >= 0)
	{
		AggInfo    *agginfo = list_nth_node(AggInfo, root->agginfos, i);

		foreach(lc, agginfo->aggrefs)
		{
			Aggref	   *aggref = lfirst_node(Aggref, lc);

			aggref->aggpresorted = true;
		}
	}
}

/*
 * Compute query_pathkeys and other pathkeys during plan generation
 */
static void
standard_qp_callback(PlannerInfo *root, void *extra)
{
	Query	   *parse = root->parse;
	standard_qp_extra *qp_extra = (standard_qp_extra *) extra;
	List	   *tlist = root->processed_tlist;
	List	   *activeWindows = qp_extra->activeWindows;

	/*
	 * Calculate pathkeys that represent grouping/ordering and/or ordered
	 * aggregate requirements.
	 */
	if (qp_extra->gset_data)
	{
		/*
		 * With grouping sets, just use the first RollupData's groupClause. We
		 * don't make any effort to optimize grouping clauses when there are
		 * grouping sets, nor can we combine aggregate ordering keys with
		 * grouping.
		 */
		List	   *rollups = qp_extra->gset_data->rollups;
		List	   *groupClause = (rollups ? linitial_node(RollupData, rollups)->groupClause : NIL);

		if (grouping_is_sortable(groupClause))
		{
			bool		sortable;

			/*
			 * The groupClause is logically below the grouping step.  So if
			 * there is an RTE entry for the grouping step, we need to remove
			 * its RT index from the sort expressions before we make PathKeys
			 * for them.
			 */
			root->group_pathkeys =
				make_pathkeys_for_sortclauses_extended(root,
													   &groupClause,
													   tlist,
													   false,
													   parse->hasGroupRTE,
													   &sortable,
													   false);
			Assert(sortable);
			root->num_groupby_pathkeys = list_length(root->group_pathkeys);
		}
		else
		{
			root->group_pathkeys = NIL;
			root->num_groupby_pathkeys = 0;
		}
	}
	else if (parse->groupClause || root->numOrderedAggs > 0)
	{
		/*
		 * With a plain GROUP BY list, we can remove any grouping items that
		 * are proven redundant by EquivalenceClass processing.  For example,
		 * we can remove y given "WHERE x = y GROUP BY x, y".  These aren't
		 * especially common cases, but they're nearly free to detect.  Note
		 * that we remove redundant items from processed_groupClause but not
		 * the original parse->groupClause.
		 */
		bool		sortable;

		/*
		 * Convert group clauses into pathkeys.  Set the ec_sortref field of
		 * EquivalenceClass'es if it's not set yet.
		 */
		root->group_pathkeys =
			make_pathkeys_for_sortclauses_extended(root,
												   &root->processed_groupClause,
												   tlist,
												   true,
												   false,
												   &sortable,
												   true);
		if (!sortable)
		{
			/* Can't sort; no point in considering aggregate ordering either */
			root->group_pathkeys = NIL;
			root->num_groupby_pathkeys = 0;
		}
		else
		{
			root->num_groupby_pathkeys = list_length(root->group_pathkeys);
			/* If we have ordered aggs, consider adding onto group_pathkeys */
			if (root->numOrderedAggs > 0)
				adjust_group_pathkeys_for_groupagg(root);
		}
	}
	else
	{
		root->group_pathkeys = NIL;
		root->num_groupby_pathkeys = 0;
	}

	/* We consider only the first (bottom) window in pathkeys logic */
	if (activeWindows != NIL)
	{
		WindowClause *wc = linitial_node(WindowClause, activeWindows);

		root->window_pathkeys = make_pathkeys_for_window(root,
														 wc,
														 tlist);
	}
	else
		root->window_pathkeys = NIL;

	/*
	 * As with GROUP BY, we can discard any DISTINCT items that are proven
	 * redundant by EquivalenceClass processing.  The non-redundant list is
	 * kept in root->processed_distinctClause, leaving the original
	 * parse->distinctClause alone.
	 */
	if (parse->distinctClause)
	{
		bool		sortable;

		/* Make a copy since pathkey processing can modify the list */
		root->processed_distinctClause = list_copy(parse->distinctClause);
		root->distinct_pathkeys =
			make_pathkeys_for_sortclauses_extended(root,
												   &root->processed_distinctClause,
												   tlist,
												   true,
												   false,
												   &sortable,
												   false);
		if (!sortable)
			root->distinct_pathkeys = NIL;
	}
	else
		root->distinct_pathkeys = NIL;

	root->sort_pathkeys =
		make_pathkeys_for_sortclauses(root,
									  parse->sortClause,
									  tlist);

	/* setting setop_pathkeys might be useful to the union planner */
	if (qp_extra->setop != NULL &&
		set_operation_ordered_results_useful(qp_extra->setop))
	{
		List	   *groupClauses;
		bool		sortable;

		groupClauses = generate_setop_child_grouplist(qp_extra->setop, tlist);

		root->setop_pathkeys =
			make_pathkeys_for_sortclauses_extended(root,
												   &groupClauses,
												   tlist,
												   false,
												   false,
												   &sortable,
												   false);
		if (!sortable)
			root->setop_pathkeys = NIL;
	}
	else
		root->setop_pathkeys = NIL;

	/*
	 * Figure out whether we want a sorted result from query_planner.
	 *
	 * If we have a sortable GROUP BY clause, then we want a result sorted
	 * properly for grouping.  Otherwise, if we have window functions to
	 * evaluate, we try to sort for the first window.  Otherwise, if there's a
	 * sortable DISTINCT clause that's more rigorous than the ORDER BY clause,
	 * we try to produce output that's sufficiently well sorted for the
	 * DISTINCT.  Otherwise, if there is an ORDER BY clause, we want to sort
	 * by the ORDER BY clause.  Otherwise, if we're a subquery being planned
	 * for a set operation which can benefit from presorted results and have a
	 * sortable targetlist, we want to sort by the target list.
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
	else if (root->setop_pathkeys != NIL)
		root->query_pathkeys = root->setop_pathkeys;
	else
		root->query_pathkeys = NIL;
}

/*
 * Estimate number of groups produced by grouping clauses (1 if not grouping)
 *
 * path_rows: number of output rows from scan/join step
 * gd: grouping sets data including list of grouping sets and their clauses
 * target_list: target list containing group clause references
 *
 * If doing grouping sets, we also annotate the gsets data with the estimates
 * for each set and each individual rollup list, with a view to later
 * determining whether some combination of them could be hashed instead.
 */
static double
get_number_of_groups(PlannerInfo *root,
					 double path_rows,
					 grouping_sets_data *gd,
					 List *target_list)
{
	Query	   *parse = root->parse;
	double		dNumGroups;

	if (parse->groupClause)
	{
		List	   *groupExprs;

		if (parse->groupingSets)
		{
			/* Add up the estimates for each grouping set */
			ListCell   *lc;

			Assert(gd);			/* keep Coverity happy */

			dNumGroups = 0;

			foreach(lc, gd->rollups)
			{
				RollupData *rollup = lfirst_node(RollupData, lc);
				ListCell   *lc2;
				ListCell   *lc3;

				groupExprs = get_sortgrouplist_exprs(rollup->groupClause,
													 target_list);

				rollup->numGroups = 0.0;

				forboth(lc2, rollup->gsets, lc3, rollup->gsets_data)
				{
					List	   *gset = (List *) lfirst(lc2);
					GroupingSetData *gs = lfirst_node(GroupingSetData, lc3);
					double		numGroups = estimate_num_groups(root,
																groupExprs,
																path_rows,
																&gset,
																NULL);

					gs->numGroups = numGroups;
					rollup->numGroups += numGroups;
				}

				dNumGroups += rollup->numGroups;
			}

			if (gd->hash_sets_idx)
			{
				ListCell   *lc2;

				gd->dNumHashGroups = 0;

				groupExprs = get_sortgrouplist_exprs(parse->groupClause,
													 target_list);

				forboth(lc, gd->hash_sets_idx, lc2, gd->unsortable_sets)
				{
					List	   *gset = (List *) lfirst(lc);
					GroupingSetData *gs = lfirst_node(GroupingSetData, lc2);
					double		numGroups = estimate_num_groups(root,
																groupExprs,
																path_rows,
																&gset,
																NULL);

					gs->numGroups = numGroups;
					gd->dNumHashGroups += numGroups;
				}

				dNumGroups += gd->dNumHashGroups;
			}
		}
		else
		{
			/* Plain GROUP BY -- estimate based on optimized groupClause */
			groupExprs = get_sortgrouplist_exprs(root->processed_groupClause,
												 target_list);

			dNumGroups = estimate_num_groups(root, groupExprs, path_rows,
											 NULL, NULL);
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
 * create_grouping_paths
 *
 * Build a new upperrel containing Paths for grouping and/or aggregation.
 * Along the way, we also build an upperrel for Paths which are partially
 * grouped and/or aggregated.  A partially grouped and/or aggregated path
 * needs a FinalizeAggregate node to complete the aggregation.  Currently,
 * the only partially grouped paths we build are also partial paths; that
 * is, they need a Gather and then a FinalizeAggregate.
 *
 * input_rel: contains the source-data Paths
 * target: the pathtarget for the result Paths to compute
 * gd: grouping sets data including list of grouping sets and their clauses
 *
 * Note: all Paths in input_rel are expected to return the target computed
 * by make_group_input_target.
 */
static RelOptInfo *
create_grouping_paths(PlannerInfo *root,
					  RelOptInfo *input_rel,
					  PathTarget *target,
					  bool target_parallel_safe,
					  grouping_sets_data *gd)
{
	Query	   *parse = root->parse;
	RelOptInfo *grouped_rel;
	RelOptInfo *partially_grouped_rel;
	AggClauseCosts agg_costs;

	MemSet(&agg_costs, 0, sizeof(AggClauseCosts));
	get_agg_clause_costs(root, AGGSPLIT_SIMPLE, &agg_costs);

	/*
	 * Create grouping relation to hold fully aggregated grouping and/or
	 * aggregation paths.
	 */
	grouped_rel = make_grouping_rel(root, input_rel, target,
									target_parallel_safe, parse->havingQual);

	/*
	 * Create either paths for a degenerate grouping or paths for ordinary
	 * grouping, as appropriate.
	 */
	if (is_degenerate_grouping(root))
		create_degenerate_grouping_paths(root, input_rel, grouped_rel);
	else
	{
		int			flags = 0;
		GroupPathExtraData extra;

		/*
		 * Determine whether it's possible to perform sort-based
		 * implementations of grouping.  (Note that if processed_groupClause
		 * is empty, grouping_is_sortable() is trivially true, and all the
		 * pathkeys_contained_in() tests will succeed too, so that we'll
		 * consider every surviving input path.)
		 *
		 * If we have grouping sets, we might be able to sort some but not all
		 * of them; in this case, we need can_sort to be true as long as we
		 * must consider any sorted-input plan.
		 */
		if ((gd && gd->rollups != NIL)
			|| grouping_is_sortable(root->processed_groupClause))
			flags |= GROUPING_CAN_USE_SORT;

		/*
		 * Determine whether we should consider hash-based implementations of
		 * grouping.
		 *
		 * Hashed aggregation only applies if we're grouping. If we have
		 * grouping sets, some groups might be hashable but others not; in
		 * this case we set can_hash true as long as there is nothing globally
		 * preventing us from hashing (and we should therefore consider plans
		 * with hashes).
		 *
		 * Executor doesn't support hashed aggregation with DISTINCT or ORDER
		 * BY aggregates.  (Doing so would imply storing *all* the input
		 * values in the hash table, and/or running many sorts in parallel,
		 * either of which seems like a certain loser.)  We similarly don't
		 * support ordered-set aggregates in hashed aggregation, but that case
		 * is also included in the numOrderedAggs count.
		 *
		 * Note: grouping_is_hashable() is much more expensive to check than
		 * the other gating conditions, so we want to do it last.
		 */
		if ((parse->groupClause != NIL &&
			 root->numOrderedAggs == 0 &&
			 (gd ? gd->any_hashable : grouping_is_hashable(root->processed_groupClause))))
			flags |= GROUPING_CAN_USE_HASH;

		/*
		 * Determine whether partial aggregation is possible.
		 */
		if (can_partial_agg(root))
			flags |= GROUPING_CAN_PARTIAL_AGG;

		extra.flags = flags;
		extra.target_parallel_safe = target_parallel_safe;
		extra.havingQual = parse->havingQual;
		extra.targetList = parse->targetList;
		extra.partial_costs_set = false;

		/*
		 * Determine whether partitionwise aggregation is in theory possible.
		 * It can be disabled by the user, and for now, we don't try to
		 * support grouping sets.  create_ordinary_grouping_paths() will check
		 * additional conditions, such as whether input_rel is partitioned.
		 */
		if (enable_partitionwise_aggregate && !parse->groupingSets)
			extra.patype = PARTITIONWISE_AGGREGATE_FULL;
		else
			extra.patype = PARTITIONWISE_AGGREGATE_NONE;

		create_ordinary_grouping_paths(root, input_rel, grouped_rel,
									   &agg_costs, gd, &extra,
									   &partially_grouped_rel);
	}

	set_cheapest(grouped_rel);
	return grouped_rel;
}

/*
 * make_grouping_rel
 *
 * Create a new grouping rel and set basic properties.
 *
 * input_rel represents the underlying scan/join relation.
 * target is the output expected from the grouping relation.
 */
static RelOptInfo *
make_grouping_rel(PlannerInfo *root, RelOptInfo *input_rel,
				  PathTarget *target, bool target_parallel_safe,
				  Node *havingQual)
{
	RelOptInfo *grouped_rel;

	if (IS_OTHER_REL(input_rel))
	{
		grouped_rel = fetch_upper_rel(root, UPPERREL_GROUP_AGG,
									  input_rel->relids);
		grouped_rel->reloptkind = RELOPT_OTHER_UPPER_REL;
	}
	else
	{
		/*
		 * By tradition, the relids set for the main grouping relation is
		 * NULL.  (This could be changed, but might require adjustments
		 * elsewhere.)
		 */
		grouped_rel = fetch_upper_rel(root, UPPERREL_GROUP_AGG, NULL);
	}

	/* Set target. */
	grouped_rel->reltarget = target;

	/*
	 * If the input relation is not parallel-safe, then the grouped relation
	 * can't be parallel-safe, either.  Otherwise, it's parallel-safe if the
	 * target list and HAVING quals are parallel-safe.
	 */
	if (input_rel->consider_parallel && target_parallel_safe &&
		is_parallel_safe(root, (Node *) havingQual))
		grouped_rel->consider_parallel = true;

	/*
	 * If the input rel belongs to a single FDW, so does the grouped rel.
	 */
	grouped_rel->serverid = input_rel->serverid;
	grouped_rel->userid = input_rel->userid;
	grouped_rel->useridiscurrent = input_rel->useridiscurrent;
	grouped_rel->fdwroutine = input_rel->fdwroutine;

	return grouped_rel;
}

/*
 * is_degenerate_grouping
 *
 * A degenerate grouping is one in which the query has a HAVING qual and/or
 * grouping sets, but no aggregates and no GROUP BY (which implies that the
 * grouping sets are all empty).
 */
static bool
is_degenerate_grouping(PlannerInfo *root)
{
	Query	   *parse = root->parse;

	return (root->hasHavingQual || parse->groupingSets) &&
		!parse->hasAggs && parse->groupClause == NIL;
}

/*
 * create_degenerate_grouping_paths
 *
 * When the grouping is degenerate (see is_degenerate_grouping), we are
 * supposed to emit either zero or one row for each grouping set depending on
 * whether HAVING succeeds.  Furthermore, there cannot be any variables in
 * either HAVING or the targetlist, so we actually do not need the FROM table
 * at all! We can just throw away the plan-so-far and generate a Result node.
 * This is a sufficiently unusual corner case that it's not worth contorting
 * the structure of this module to avoid having to generate the earlier paths
 * in the first place.
 */
static void
create_degenerate_grouping_paths(PlannerInfo *root, RelOptInfo *input_rel,
								 RelOptInfo *grouped_rel)
{
	Query	   *parse = root->parse;
	int			nrows;
	Path	   *path;

	nrows = list_length(parse->groupingSets);
	if (nrows > 1)
	{
		/*
		 * Doesn't seem worthwhile writing code to cons up a generate_series
		 * or a values scan to emit multiple rows. Instead just make N clones
		 * and append them.  (With a volatile HAVING clause, this means you
		 * might get between 0 and N output rows. Offhand I think that's
		 * desired.)
		 */
		List	   *paths = NIL;

		while (--nrows >= 0)
		{
			path = (Path *)
				create_group_result_path(root, grouped_rel,
										 grouped_rel->reltarget,
										 (List *) parse->havingQual);
			paths = lappend(paths, path);
		}
		path = (Path *)
			create_append_path(root,
							   grouped_rel,
							   paths,
							   NIL,
							   NIL,
							   NULL,
							   0,
							   false,
							   -1);
	}
	else
	{
		/* No grouping sets, or just one, so one output row */
		path = (Path *)
			create_group_result_path(root, grouped_rel,
									 grouped_rel->reltarget,
									 (List *) parse->havingQual);
	}

	add_path(grouped_rel, path);
}

/*
 * create_ordinary_grouping_paths
 *
 * Create grouping paths for the ordinary (that is, non-degenerate) case.
 *
 * We need to consider sorted and hashed aggregation in the same function,
 * because otherwise (1) it would be harder to throw an appropriate error
 * message if neither way works, and (2) we should not allow hashtable size
 * considerations to dissuade us from using hashing if sorting is not possible.
 *
 * *partially_grouped_rel_p will be set to the partially grouped rel which this
 * function creates, or to NULL if it doesn't create one.
 */
static void
create_ordinary_grouping_paths(PlannerInfo *root, RelOptInfo *input_rel,
							   RelOptInfo *grouped_rel,
							   const AggClauseCosts *agg_costs,
							   grouping_sets_data *gd,
							   GroupPathExtraData *extra,
							   RelOptInfo **partially_grouped_rel_p)
{
	Path	   *cheapest_path = input_rel->cheapest_total_path;
	RelOptInfo *partially_grouped_rel = NULL;
	double		dNumGroups;
	PartitionwiseAggregateType patype = PARTITIONWISE_AGGREGATE_NONE;

	/*
	 * If this is the topmost grouping relation or if the parent relation is
	 * doing some form of partitionwise aggregation, then we may be able to do
	 * it at this level also.  However, if the input relation is not
	 * partitioned, partitionwise aggregate is impossible.
	 */
	if (extra->patype != PARTITIONWISE_AGGREGATE_NONE &&
		IS_PARTITIONED_REL(input_rel))
	{
		/*
		 * If this is the topmost relation or if the parent relation is doing
		 * full partitionwise aggregation, then we can do full partitionwise
		 * aggregation provided that the GROUP BY clause contains all of the
		 * partitioning columns at this level and the collation used by GROUP
		 * BY matches the partitioning collation.  Otherwise, we can do at
		 * most partial partitionwise aggregation.  But if partial aggregation
		 * is not supported in general then we can't use it for partitionwise
		 * aggregation either.
		 *
		 * Check parse->groupClause not processed_groupClause, because it's
		 * okay if some of the partitioning columns were proved redundant.
		 */
		if (extra->patype == PARTITIONWISE_AGGREGATE_FULL &&
			group_by_has_partkey(input_rel, extra->targetList,
								 root->parse->groupClause))
			patype = PARTITIONWISE_AGGREGATE_FULL;
		else if ((extra->flags & GROUPING_CAN_PARTIAL_AGG) != 0)
			patype = PARTITIONWISE_AGGREGATE_PARTIAL;
		else
			patype = PARTITIONWISE_AGGREGATE_NONE;
	}

	/*
	 * Before generating paths for grouped_rel, we first generate any possible
	 * partially grouped paths; that way, later code can easily consider both
	 * parallel and non-parallel approaches to grouping.
	 */
	if ((extra->flags & GROUPING_CAN_PARTIAL_AGG) != 0)
	{
		bool		force_rel_creation;

		/*
		 * If we're doing partitionwise aggregation at this level, force
		 * creation of a partially_grouped_rel so we can add partitionwise
		 * paths to it.
		 */
		force_rel_creation = (patype == PARTITIONWISE_AGGREGATE_PARTIAL);

		partially_grouped_rel =
			create_partial_grouping_paths(root,
										  grouped_rel,
										  input_rel,
										  gd,
										  extra,
										  force_rel_creation);
	}

	/* Set out parameter. */
	*partially_grouped_rel_p = partially_grouped_rel;

	/* Apply partitionwise aggregation technique, if possible. */
	if (patype != PARTITIONWISE_AGGREGATE_NONE)
		create_partitionwise_grouping_paths(root, input_rel, grouped_rel,
											partially_grouped_rel, agg_costs,
											gd, patype, extra);

	/* If we are doing partial aggregation only, return. */
	if (extra->patype == PARTITIONWISE_AGGREGATE_PARTIAL)
	{
		Assert(partially_grouped_rel);

		if (partially_grouped_rel->pathlist)
			set_cheapest(partially_grouped_rel);

		return;
	}

	/* Gather any partially grouped partial paths. */
	if (partially_grouped_rel && partially_grouped_rel->partial_pathlist)
	{
		gather_grouping_paths(root, partially_grouped_rel);
		set_cheapest(partially_grouped_rel);
	}

	/*
	 * Estimate number of groups.
	 */
	dNumGroups = get_number_of_groups(root,
									  cheapest_path->rows,
									  gd,
									  extra->targetList);

	/* Build final grouping paths */
	add_paths_to_grouping_rel(root, input_rel, grouped_rel,
							  partially_grouped_rel, agg_costs, gd,
							  dNumGroups, extra);

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
													  input_rel, grouped_rel,
													  extra);

	/* Let extensions possibly add some more paths */
	if (create_upper_paths_hook)
		(*create_upper_paths_hook) (root, UPPERREL_GROUP_AGG,
									input_rel, grouped_rel,
									extra);
}

/*
 * For a given input path, consider the possible ways of doing grouping sets on
 * it, by combinations of hashing and sorting.  This can be called multiple
 * times, so it's important that it not scribble on input.  No result is
 * returned, but any generated paths are added to grouped_rel.
 */
static void
consider_groupingsets_paths(PlannerInfo *root,
							RelOptInfo *grouped_rel,
							Path *path,
							bool is_sorted,
							bool can_hash,
							grouping_sets_data *gd,
							const AggClauseCosts *agg_costs,
							double dNumGroups)
{
	Query	   *parse = root->parse;
	Size		hash_mem_limit = get_hash_memory_limit();

	/*
	 * If we're not being offered sorted input, then only consider plans that
	 * can be done entirely by hashing.
	 *
	 * We can hash everything if it looks like it'll fit in hash_mem. But if
	 * the input is actually sorted despite not being advertised as such, we
	 * prefer to make use of that in order to use less memory.
	 *
	 * If none of the grouping sets are sortable, then ignore the hash_mem
	 * limit and generate a path anyway, since otherwise we'll just fail.
	 */
	if (!is_sorted)
	{
		List	   *new_rollups = NIL;
		RollupData *unhashed_rollup = NULL;
		List	   *sets_data;
		List	   *empty_sets_data = NIL;
		List	   *empty_sets = NIL;
		ListCell   *lc;
		ListCell   *l_start = list_head(gd->rollups);
		AggStrategy strat = AGG_HASHED;
		double		hashsize;
		double		exclude_groups = 0.0;

		Assert(can_hash);

		/*
		 * If the input is coincidentally sorted usefully (which can happen
		 * even if is_sorted is false, since that only means that our caller
		 * has set up the sorting for us), then save some hashtable space by
		 * making use of that. But we need to watch out for degenerate cases:
		 *
		 * 1) If there are any empty grouping sets, then group_pathkeys might
		 * be NIL if all non-empty grouping sets are unsortable. In this case,
		 * there will be a rollup containing only empty groups, and the
		 * pathkeys_contained_in test is vacuously true; this is ok.
		 *
		 * XXX: the above relies on the fact that group_pathkeys is generated
		 * from the first rollup. If we add the ability to consider multiple
		 * sort orders for grouping input, this assumption might fail.
		 *
		 * 2) If there are no empty sets and only unsortable sets, then the
		 * rollups list will be empty (and thus l_start == NULL), and
		 * group_pathkeys will be NIL; we must ensure that the vacuously-true
		 * pathkeys_contained_in test doesn't cause us to crash.
		 */
		if (l_start != NULL &&
			pathkeys_contained_in(root->group_pathkeys, path->pathkeys))
		{
			unhashed_rollup = lfirst_node(RollupData, l_start);
			exclude_groups = unhashed_rollup->numGroups;
			l_start = lnext(gd->rollups, l_start);
		}

		hashsize = estimate_hashagg_tablesize(root,
											  path,
											  agg_costs,
											  dNumGroups - exclude_groups);

		/*
		 * gd->rollups is empty if we have only unsortable columns to work
		 * with.  Override hash_mem in that case; otherwise, we'll rely on the
		 * sorted-input case to generate usable mixed paths.
		 */
		if (hashsize > hash_mem_limit && gd->rollups)
			return;				/* nope, won't fit */

		/*
		 * We need to burst the existing rollups list into individual grouping
		 * sets and recompute a groupClause for each set.
		 */
		sets_data = list_copy(gd->unsortable_sets);

		for_each_cell(lc, gd->rollups, l_start)
		{
			RollupData *rollup = lfirst_node(RollupData, lc);

			/*
			 * If we find an unhashable rollup that's not been skipped by the
			 * "actually sorted" check above, we can't cope; we'd need sorted
			 * input (with a different sort order) but we can't get that here.
			 * So bail out; we'll get a valid path from the is_sorted case
			 * instead.
			 *
			 * The mere presence of empty grouping sets doesn't make a rollup
			 * unhashable (see preprocess_grouping_sets), we handle those
			 * specially below.
			 */
			if (!rollup->hashable)
				return;

			sets_data = list_concat(sets_data, rollup->gsets_data);
		}
		foreach(lc, sets_data)
		{
			GroupingSetData *gs = lfirst_node(GroupingSetData, lc);
			List	   *gset = gs->set;
			RollupData *rollup;

			if (gset == NIL)
			{
				/* Empty grouping sets can't be hashed. */
				empty_sets_data = lappend(empty_sets_data, gs);
				empty_sets = lappend(empty_sets, NIL);
			}
			else
			{
				rollup = makeNode(RollupData);

				rollup->groupClause = preprocess_groupclause(root, gset);
				rollup->gsets_data = list_make1(gs);
				rollup->gsets = remap_to_groupclause_idx(rollup->groupClause,
														 rollup->gsets_data,
														 gd->tleref_to_colnum_map);
				rollup->numGroups = gs->numGroups;
				rollup->hashable = true;
				rollup->is_hashed = true;
				new_rollups = lappend(new_rollups, rollup);
			}
		}

		/*
		 * If we didn't find anything nonempty to hash, then bail.  We'll
		 * generate a path from the is_sorted case.
		 */
		if (new_rollups == NIL)
			return;

		/*
		 * If there were empty grouping sets they should have been in the
		 * first rollup.
		 */
		Assert(!unhashed_rollup || !empty_sets);

		if (unhashed_rollup)
		{
			new_rollups = lappend(new_rollups, unhashed_rollup);
			strat = AGG_MIXED;
		}
		else if (empty_sets)
		{
			RollupData *rollup = makeNode(RollupData);

			rollup->groupClause = NIL;
			rollup->gsets_data = empty_sets_data;
			rollup->gsets = empty_sets;
			rollup->numGroups = list_length(empty_sets);
			rollup->hashable = false;
			rollup->is_hashed = false;
			new_rollups = lappend(new_rollups, rollup);
			strat = AGG_MIXED;
		}

		add_path(grouped_rel, (Path *)
				 create_groupingsets_path(root,
										  grouped_rel,
										  path,
										  (List *) parse->havingQual,
										  strat,
										  new_rollups,
										  agg_costs));
		return;
	}

	/*
	 * If we have sorted input but nothing we can do with it, bail.
	 */
	if (gd->rollups == NIL)
		return;

	/*
	 * Given sorted input, we try and make two paths: one sorted and one mixed
	 * sort/hash. (We need to try both because hashagg might be disabled, or
	 * some columns might not be sortable.)
	 *
	 * can_hash is passed in as false if some obstacle elsewhere (such as
	 * ordered aggs) means that we shouldn't consider hashing at all.
	 */
	if (can_hash && gd->any_hashable)
	{
		List	   *rollups = NIL;
		List	   *hash_sets = list_copy(gd->unsortable_sets);
		double		availspace = hash_mem_limit;
		ListCell   *lc;

		/*
		 * Account first for space needed for groups we can't sort at all.
		 */
		availspace -= estimate_hashagg_tablesize(root,
												 path,
												 agg_costs,
												 gd->dNumHashGroups);

		if (availspace > 0 && list_length(gd->rollups) > 1)
		{
			double		scale;
			int			num_rollups = list_length(gd->rollups);
			int			k_capacity;
			int		   *k_weights = palloc(num_rollups * sizeof(int));
			Bitmapset  *hash_items = NULL;
			int			i;

			/*
			 * We treat this as a knapsack problem: the knapsack capacity
			 * represents hash_mem, the item weights are the estimated memory
			 * usage of the hashtables needed to implement a single rollup,
			 * and we really ought to use the cost saving as the item value;
			 * however, currently the costs assigned to sort nodes don't
			 * reflect the comparison costs well, and so we treat all items as
			 * of equal value (each rollup we hash instead saves us one sort).
			 *
			 * To use the discrete knapsack, we need to scale the values to a
			 * reasonably small bounded range.  We choose to allow a 5% error
			 * margin; we have no more than 4096 rollups in the worst possible
			 * case, which with a 5% error margin will require a bit over 42MB
			 * of workspace. (Anyone wanting to plan queries that complex had
			 * better have the memory for it.  In more reasonable cases, with
			 * no more than a couple of dozen rollups, the memory usage will
			 * be negligible.)
			 *
			 * k_capacity is naturally bounded, but we clamp the values for
			 * scale and weight (below) to avoid overflows or underflows (or
			 * uselessly trying to use a scale factor less than 1 byte).
			 */
			scale = Max(availspace / (20.0 * num_rollups), 1.0);
			k_capacity = (int) floor(availspace / scale);

			/*
			 * We leave the first rollup out of consideration since it's the
			 * one that matches the input sort order.  We assign indexes "i"
			 * to only those entries considered for hashing; the second loop,
			 * below, must use the same condition.
			 */
			i = 0;
			for_each_from(lc, gd->rollups, 1)
			{
				RollupData *rollup = lfirst_node(RollupData, lc);

				if (rollup->hashable)
				{
					double		sz = estimate_hashagg_tablesize(root,
																path,
																agg_costs,
																rollup->numGroups);

					/*
					 * If sz is enormous, but hash_mem (and hence scale) is
					 * small, avoid integer overflow here.
					 */
					k_weights[i] = (int) Min(floor(sz / scale),
											 k_capacity + 1.0);
					++i;
				}
			}

			/*
			 * Apply knapsack algorithm; compute the set of items which
			 * maximizes the value stored (in this case the number of sorts
			 * saved) while keeping the total size (approximately) within
			 * capacity.
			 */
			if (i > 0)
				hash_items = DiscreteKnapsack(k_capacity, i, k_weights, NULL);

			if (!bms_is_empty(hash_items))
			{
				rollups = list_make1(linitial(gd->rollups));

				i = 0;
				for_each_from(lc, gd->rollups, 1)
				{
					RollupData *rollup = lfirst_node(RollupData, lc);

					if (rollup->hashable)
					{
						if (bms_is_member(i, hash_items))
							hash_sets = list_concat(hash_sets,
													rollup->gsets_data);
						else
							rollups = lappend(rollups, rollup);
						++i;
					}
					else
						rollups = lappend(rollups, rollup);
				}
			}
		}

		if (!rollups && hash_sets)
			rollups = list_copy(gd->rollups);

		foreach(lc, hash_sets)
		{
			GroupingSetData *gs = lfirst_node(GroupingSetData, lc);
			RollupData *rollup = makeNode(RollupData);

			Assert(gs->set != NIL);

			rollup->groupClause = preprocess_groupclause(root, gs->set);
			rollup->gsets_data = list_make1(gs);
			rollup->gsets = remap_to_groupclause_idx(rollup->groupClause,
													 rollup->gsets_data,
													 gd->tleref_to_colnum_map);
			rollup->numGroups = gs->numGroups;
			rollup->hashable = true;
			rollup->is_hashed = true;
			rollups = lcons(rollup, rollups);
		}

		if (rollups)
		{
			add_path(grouped_rel, (Path *)
					 create_groupingsets_path(root,
											  grouped_rel,
											  path,
											  (List *) parse->havingQual,
											  AGG_MIXED,
											  rollups,
											  agg_costs));
		}
	}

	/*
	 * Now try the simple sorted case.
	 */
	if (!gd->unsortable_sets)
		add_path(grouped_rel, (Path *)
				 create_groupingsets_path(root,
										  grouped_rel,
										  path,
										  (List *) parse->havingQual,
										  AGG_SORTED,
										  gd->rollups,
										  agg_costs));
}

/*
 * create_window_paths
 *
 * Build a new upperrel containing Paths for window-function evaluation.
 *
 * input_rel: contains the source-data Paths
 * input_target: result of make_window_input_target
 * output_target: what the topmost WindowAggPath should return
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
					bool output_target_parallel_safe,
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
	if (input_rel->consider_parallel && output_target_parallel_safe &&
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
	 * existing paths that satisfy or partially satisfy root->window_pathkeys.
	 */
	foreach(lc, input_rel->pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);
		int			presorted_keys;

		if (path == input_rel->cheapest_total_path ||
			pathkeys_count_contained_in(root->window_pathkeys, path->pathkeys,
										&presorted_keys) ||
			presorted_keys > 0)
			create_one_window_path(root,
								   window_rel,
								   path,
								   input_target,
								   output_target,
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
													 input_rel, window_rel,
													 NULL);

	/* Let extensions possibly add some more paths */
	if (create_upper_paths_hook)
		(*create_upper_paths_hook) (root, UPPERREL_WINDOW,
									input_rel, window_rel, NULL);

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
 * wflists: result of find_window_functions
 * activeWindows: result of select_active_windows
 */
static void
create_one_window_path(PlannerInfo *root,
					   RelOptInfo *window_rel,
					   Path *path,
					   PathTarget *input_target,
					   PathTarget *output_target,
					   WindowFuncLists *wflists,
					   List *activeWindows)
{
	PathTarget *window_target;
	ListCell   *l;
	List	   *topqual = NIL;

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
		WindowClause *wc = lfirst_node(WindowClause, l);
		List	   *window_pathkeys;
		List	   *runcondition = NIL;
		int			presorted_keys;
		bool		is_sorted;
		bool		topwindow;
		ListCell   *lc2;

		window_pathkeys = make_pathkeys_for_window(root,
												   wc,
												   root->processed_tlist);

		is_sorted = pathkeys_count_contained_in(window_pathkeys,
												path->pathkeys,
												&presorted_keys);

		/* Sort if necessary */
		if (!is_sorted)
		{
			/*
			 * No presorted keys or incremental sort disabled, just perform a
			 * complete sort.
			 */
			if (presorted_keys == 0 || !enable_incremental_sort)
				path = (Path *) create_sort_path(root, window_rel,
												 path,
												 window_pathkeys,
												 -1.0);
			else
			{
				/*
				 * Since we have presorted keys and incremental sort is
				 * enabled, just use incremental sort.
				 */
				path = (Path *) create_incremental_sort_path(root,
															 window_rel,
															 path,
															 window_pathkeys,
															 presorted_keys,
															 -1.0);
			}
		}

		if (lnext(activeWindows, l))
		{
			/*
			 * Add the current WindowFuncs to the output target for this
			 * intermediate WindowAggPath.  We must copy window_target to
			 * avoid changing the previous path's target.
			 *
			 * Note: a WindowFunc adds nothing to the target's eval costs; but
			 * we do need to account for the increase in tlist width.
			 */
			int64		tuple_width = window_target->width;

			window_target = copy_pathtarget(window_target);
			foreach(lc2, wflists->windowFuncs[wc->winref])
			{
				WindowFunc *wfunc = lfirst_node(WindowFunc, lc2);

				add_column_to_pathtarget(window_target, (Expr *) wfunc, 0);
				tuple_width += get_typavgwidth(wfunc->wintype, -1);
			}
			window_target->width = clamp_width_est(tuple_width);
		}
		else
		{
			/* Install the goal target in the topmost WindowAgg */
			window_target = output_target;
		}

		/* mark the final item in the list as the top-level window */
		topwindow = foreach_current_index(l) == list_length(activeWindows) - 1;

		/*
		 * Collect the WindowFuncRunConditions from each WindowFunc and
		 * convert them into OpExprs
		 */
		foreach(lc2, wflists->windowFuncs[wc->winref])
		{
			ListCell   *lc3;
			WindowFunc *wfunc = lfirst_node(WindowFunc, lc2);

			foreach(lc3, wfunc->runCondition)
			{
				WindowFuncRunCondition *wfuncrc =
					lfirst_node(WindowFuncRunCondition, lc3);
				Expr	   *opexpr;
				Expr	   *leftop;
				Expr	   *rightop;

				if (wfuncrc->wfunc_left)
				{
					leftop = (Expr *) copyObject(wfunc);
					rightop = copyObject(wfuncrc->arg);
				}
				else
				{
					leftop = copyObject(wfuncrc->arg);
					rightop = (Expr *) copyObject(wfunc);
				}

				opexpr = make_opclause(wfuncrc->opno,
									   BOOLOID,
									   false,
									   leftop,
									   rightop,
									   InvalidOid,
									   wfuncrc->inputcollid);

				runcondition = lappend(runcondition, opexpr);

				if (!topwindow)
					topqual = lappend(topqual, opexpr);
			}
		}

		path = (Path *)
			create_windowagg_path(root, window_rel, path, window_target,
								  wflists->windowFuncs[wc->winref],
								  runcondition, wc,
								  topwindow ? topqual : NIL, topwindow);
	}

	add_path(window_rel, path);
}

/*
 * create_distinct_paths
 *
 * Build a new upperrel containing Paths for SELECT DISTINCT evaluation.
 *
 * input_rel: contains the source-data Paths
 * target: the pathtarget for the result Paths to compute
 *
 * Note: input paths should already compute the desired pathtarget, since
 * Sort/Unique won't project anything.
 */
static RelOptInfo *
create_distinct_paths(PlannerInfo *root, RelOptInfo *input_rel,
					  PathTarget *target)
{
	RelOptInfo *distinct_rel;

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

	/* build distinct paths based on input_rel's pathlist */
	create_final_distinct_paths(root, input_rel, distinct_rel);

	/* now build distinct paths based on input_rel's partial_pathlist */
	create_partial_distinct_paths(root, input_rel, distinct_rel, target);

	/* Give a helpful error if we failed to create any paths */
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
		distinct_rel->fdwroutine->GetForeignUpperPaths(root,
													   UPPERREL_DISTINCT,
													   input_rel,
													   distinct_rel,
													   NULL);

	/* Let extensions possibly add some more paths */
	if (create_upper_paths_hook)
		(*create_upper_paths_hook) (root, UPPERREL_DISTINCT, input_rel,
									distinct_rel, NULL);

	/* Now choose the best path(s) */
	set_cheapest(distinct_rel);

	return distinct_rel;
}

/*
 * create_partial_distinct_paths
 *
 * Process 'input_rel' partial paths and add unique/aggregate paths to the
 * UPPERREL_PARTIAL_DISTINCT rel.  For paths created, add Gather/GatherMerge
 * paths on top and add a final unique/aggregate path to remove any duplicate
 * produced from combining rows from parallel workers.
 */
static void
create_partial_distinct_paths(PlannerInfo *root, RelOptInfo *input_rel,
							  RelOptInfo *final_distinct_rel,
							  PathTarget *target)
{
	RelOptInfo *partial_distinct_rel;
	Query	   *parse;
	List	   *distinctExprs;
	double		numDistinctRows;
	Path	   *cheapest_partial_path;
	ListCell   *lc;

	/* nothing to do when there are no partial paths in the input rel */
	if (!input_rel->consider_parallel || input_rel->partial_pathlist == NIL)
		return;

	parse = root->parse;

	/* can't do parallel DISTINCT ON */
	if (parse->hasDistinctOn)
		return;

	partial_distinct_rel = fetch_upper_rel(root, UPPERREL_PARTIAL_DISTINCT,
										   NULL);
	partial_distinct_rel->reltarget = target;
	partial_distinct_rel->consider_parallel = input_rel->consider_parallel;

	/*
	 * If input_rel belongs to a single FDW, so does the partial_distinct_rel.
	 */
	partial_distinct_rel->serverid = input_rel->serverid;
	partial_distinct_rel->userid = input_rel->userid;
	partial_distinct_rel->useridiscurrent = input_rel->useridiscurrent;
	partial_distinct_rel->fdwroutine = input_rel->fdwroutine;

	cheapest_partial_path = linitial(input_rel->partial_pathlist);

	distinctExprs = get_sortgrouplist_exprs(root->processed_distinctClause,
											parse->targetList);

	/* estimate how many distinct rows we'll get from each worker */
	numDistinctRows = estimate_num_groups(root, distinctExprs,
										  cheapest_partial_path->rows,
										  NULL, NULL);

	/*
	 * Try sorting the cheapest path and incrementally sorting any paths with
	 * presorted keys and put a unique paths atop of those.  We'll also
	 * attempt to reorder the required pathkeys to match the input path's
	 * pathkeys as much as possible, in hopes of avoiding a possible need to
	 * re-sort.
	 */
	if (grouping_is_sortable(root->processed_distinctClause))
	{
		foreach(lc, input_rel->partial_pathlist)
		{
			Path	   *input_path = (Path *) lfirst(lc);
			Path	   *sorted_path;
			List	   *useful_pathkeys_list = NIL;

			useful_pathkeys_list =
				get_useful_pathkeys_for_distinct(root,
												 root->distinct_pathkeys,
												 input_path->pathkeys);
			Assert(list_length(useful_pathkeys_list) > 0);

			foreach_node(List, useful_pathkeys, useful_pathkeys_list)
			{
				sorted_path = make_ordered_path(root,
												partial_distinct_rel,
												input_path,
												cheapest_partial_path,
												useful_pathkeys,
												-1.0);

				if (sorted_path == NULL)
					continue;

				/*
				 * An empty distinct_pathkeys means all tuples have the same
				 * value for the DISTINCT clause.  See
				 * create_final_distinct_paths()
				 */
				if (root->distinct_pathkeys == NIL)
				{
					Node	   *limitCount;

					limitCount = (Node *) makeConst(INT8OID, -1, InvalidOid,
													sizeof(int64),
													Int64GetDatum(1), false,
													FLOAT8PASSBYVAL);

					/*
					 * Apply a LimitPath onto the partial path to restrict the
					 * tuples from each worker to 1.
					 * create_final_distinct_paths will need to apply an
					 * additional LimitPath to restrict this to a single row
					 * after the Gather node.  If the query already has a
					 * LIMIT clause, then we could end up with three Limit
					 * nodes in the final plan.  Consolidating the top two of
					 * these could be done, but does not seem worth troubling
					 * over.
					 */
					add_partial_path(partial_distinct_rel, (Path *)
									 create_limit_path(root, partial_distinct_rel,
													   sorted_path,
													   NULL,
													   limitCount,
													   LIMIT_OPTION_COUNT,
													   0, 1));
				}
				else
				{
					add_partial_path(partial_distinct_rel, (Path *)
									 create_upper_unique_path(root, partial_distinct_rel,
															  sorted_path,
															  list_length(root->distinct_pathkeys),
															  numDistinctRows));
				}
			}
		}
	}

	/*
	 * Now try hash aggregate paths, if enabled and hashing is possible. Since
	 * we're not on the hook to ensure we do our best to create at least one
	 * path here, we treat enable_hashagg as a hard off-switch rather than the
	 * slightly softer variant in create_final_distinct_paths.
	 */
	if (enable_hashagg && grouping_is_hashable(root->processed_distinctClause))
	{
		add_partial_path(partial_distinct_rel, (Path *)
						 create_agg_path(root,
										 partial_distinct_rel,
										 cheapest_partial_path,
										 cheapest_partial_path->pathtarget,
										 AGG_HASHED,
										 AGGSPLIT_SIMPLE,
										 root->processed_distinctClause,
										 NIL,
										 NULL,
										 numDistinctRows));
	}

	/*
	 * If there is an FDW that's responsible for all baserels of the query,
	 * let it consider adding ForeignPaths.
	 */
	if (partial_distinct_rel->fdwroutine &&
		partial_distinct_rel->fdwroutine->GetForeignUpperPaths)
		partial_distinct_rel->fdwroutine->GetForeignUpperPaths(root,
															   UPPERREL_PARTIAL_DISTINCT,
															   input_rel,
															   partial_distinct_rel,
															   NULL);

	/* Let extensions possibly add some more partial paths */
	if (create_upper_paths_hook)
		(*create_upper_paths_hook) (root, UPPERREL_PARTIAL_DISTINCT,
									input_rel, partial_distinct_rel, NULL);

	if (partial_distinct_rel->partial_pathlist != NIL)
	{
		generate_useful_gather_paths(root, partial_distinct_rel, true);
		set_cheapest(partial_distinct_rel);

		/*
		 * Finally, create paths to distinctify the final result.  This step
		 * is needed to remove any duplicates due to combining rows from
		 * parallel workers.
		 */
		create_final_distinct_paths(root, partial_distinct_rel,
									final_distinct_rel);
	}
}

/*
 * create_final_distinct_paths
 *		Create distinct paths in 'distinct_rel' based on 'input_rel' pathlist
 *
 * input_rel: contains the source-data paths
 * distinct_rel: destination relation for storing created paths
 */
static RelOptInfo *
create_final_distinct_paths(PlannerInfo *root, RelOptInfo *input_rel,
							RelOptInfo *distinct_rel)
{
	Query	   *parse = root->parse;
	Path	   *cheapest_input_path = input_rel->cheapest_total_path;
	double		numDistinctRows;
	bool		allow_hash;

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

		distinctExprs = get_sortgrouplist_exprs(root->processed_distinctClause,
												parse->targetList);
		numDistinctRows = estimate_num_groups(root, distinctExprs,
											  cheapest_input_path->rows,
											  NULL, NULL);
	}

	/*
	 * Consider sort-based implementations of DISTINCT, if possible.
	 */
	if (grouping_is_sortable(root->processed_distinctClause))
	{
		/*
		 * Firstly, if we have any adequately-presorted paths, just stick a
		 * Unique node on those.  We also, consider doing an explicit sort of
		 * the cheapest input path and Unique'ing that.  If any paths have
		 * presorted keys then we'll create an incremental sort atop of those
		 * before adding a unique node on the top.  We'll also attempt to
		 * reorder the required pathkeys to match the input path's pathkeys as
		 * much as possible, in hopes of avoiding a possible need to re-sort.
		 *
		 * When we have DISTINCT ON, we must sort by the more rigorous of
		 * DISTINCT and ORDER BY, else it won't have the desired behavior.
		 * Also, if we do have to do an explicit sort, we might as well use
		 * the more rigorous ordering to avoid a second sort later.  (Note
		 * that the parser will have ensured that one clause is a prefix of
		 * the other.)
		 */
		List	   *needed_pathkeys;
		ListCell   *lc;
		double		limittuples = root->distinct_pathkeys == NIL ? 1.0 : -1.0;

		if (parse->hasDistinctOn &&
			list_length(root->distinct_pathkeys) <
			list_length(root->sort_pathkeys))
			needed_pathkeys = root->sort_pathkeys;
		else
			needed_pathkeys = root->distinct_pathkeys;

		foreach(lc, input_rel->pathlist)
		{
			Path	   *input_path = (Path *) lfirst(lc);
			Path	   *sorted_path;
			List	   *useful_pathkeys_list = NIL;

			useful_pathkeys_list =
				get_useful_pathkeys_for_distinct(root,
												 needed_pathkeys,
												 input_path->pathkeys);
			Assert(list_length(useful_pathkeys_list) > 0);

			foreach_node(List, useful_pathkeys, useful_pathkeys_list)
			{
				sorted_path = make_ordered_path(root,
												distinct_rel,
												input_path,
												cheapest_input_path,
												useful_pathkeys,
												limittuples);

				if (sorted_path == NULL)
					continue;

				/*
				 * distinct_pathkeys may have become empty if all of the
				 * pathkeys were determined to be redundant.  If all of the
				 * pathkeys are redundant then each DISTINCT target must only
				 * allow a single value, therefore all resulting tuples must
				 * be identical (or at least indistinguishable by an equality
				 * check).  We can uniquify these tuples simply by just taking
				 * the first tuple.  All we do here is add a path to do "LIMIT
				 * 1" atop of 'sorted_path'.  When doing a DISTINCT ON we may
				 * still have a non-NIL sort_pathkeys list, so we must still
				 * only do this with paths which are correctly sorted by
				 * sort_pathkeys.
				 */
				if (root->distinct_pathkeys == NIL)
				{
					Node	   *limitCount;

					limitCount = (Node *) makeConst(INT8OID, -1, InvalidOid,
													sizeof(int64),
													Int64GetDatum(1), false,
													FLOAT8PASSBYVAL);

					/*
					 * If the query already has a LIMIT clause, then we could
					 * end up with a duplicate LimitPath in the final plan.
					 * That does not seem worth troubling over too much.
					 */
					add_path(distinct_rel, (Path *)
							 create_limit_path(root, distinct_rel, sorted_path,
											   NULL, limitCount,
											   LIMIT_OPTION_COUNT, 0, 1));
				}
				else
				{
					add_path(distinct_rel, (Path *)
							 create_upper_unique_path(root, distinct_rel,
													  sorted_path,
													  list_length(root->distinct_pathkeys),
													  numDistinctRows));
				}
			}
		}
	}

	/*
	 * Consider hash-based implementations of DISTINCT, if possible.
	 *
	 * If we were not able to make any other types of path, we *must* hash or
	 * die trying.  If we do have other choices, there are two things that
	 * should prevent selection of hashing: if the query uses DISTINCT ON
	 * (because it won't really have the expected behavior if we hash), or if
	 * enable_hashagg is off.
	 *
	 * Note: grouping_is_hashable() is much more expensive to check than the
	 * other gating conditions, so we want to do it last.
	 */
	if (distinct_rel->pathlist == NIL)
		allow_hash = true;		/* we have no alternatives */
	else if (parse->hasDistinctOn || !enable_hashagg)
		allow_hash = false;		/* policy-based decision not to hash */
	else
		allow_hash = true;		/* default */

	if (allow_hash && grouping_is_hashable(root->processed_distinctClause))
	{
		/* Generate hashed aggregate path --- no sort needed */
		add_path(distinct_rel, (Path *)
				 create_agg_path(root,
								 distinct_rel,
								 cheapest_input_path,
								 cheapest_input_path->pathtarget,
								 AGG_HASHED,
								 AGGSPLIT_SIMPLE,
								 root->processed_distinctClause,
								 NIL,
								 NULL,
								 numDistinctRows));
	}

	return distinct_rel;
}

/*
 * get_useful_pathkeys_for_distinct
 * 	  Get useful orderings of pathkeys for distinctClause by reordering
 * 	  'needed_pathkeys' to match the given 'path_pathkeys' as much as possible.
 *
 * This returns a list of pathkeys that can be useful for DISTINCT or DISTINCT
 * ON clause.  For convenience, it always includes the given 'needed_pathkeys'.
 */
static List *
get_useful_pathkeys_for_distinct(PlannerInfo *root, List *needed_pathkeys,
								 List *path_pathkeys)
{
	List	   *useful_pathkeys_list = NIL;
	List	   *useful_pathkeys = NIL;

	/* always include the given 'needed_pathkeys' */
	useful_pathkeys_list = lappend(useful_pathkeys_list,
								   needed_pathkeys);

	if (!enable_distinct_reordering)
		return useful_pathkeys_list;

	/*
	 * Scan the given 'path_pathkeys' and construct a list of PathKey nodes
	 * that match 'needed_pathkeys', but only up to the longest matching
	 * prefix.
	 *
	 * When we have DISTINCT ON, we must ensure that the resulting pathkey
	 * list matches initial distinctClause pathkeys; otherwise, it won't have
	 * the desired behavior.
	 */
	foreach_node(PathKey, pathkey, path_pathkeys)
	{
		/*
		 * The PathKey nodes are canonical, so they can be checked for
		 * equality by simple pointer comparison.
		 */
		if (!list_member_ptr(needed_pathkeys, pathkey))
			break;
		if (root->parse->hasDistinctOn &&
			!list_member_ptr(root->distinct_pathkeys, pathkey))
			break;

		useful_pathkeys = lappend(useful_pathkeys, pathkey);
	}

	/* If no match at all, no point in reordering needed_pathkeys */
	if (useful_pathkeys == NIL)
		return useful_pathkeys_list;

	/*
	 * If not full match, the resulting pathkey list is not useful without
	 * incremental sort.
	 */
	if (list_length(useful_pathkeys) < list_length(needed_pathkeys) &&
		!enable_incremental_sort)
		return useful_pathkeys_list;

	/* Append the remaining PathKey nodes in needed_pathkeys */
	useful_pathkeys = list_concat_unique_ptr(useful_pathkeys,
											 needed_pathkeys);

	/*
	 * If the resulting pathkey list is the same as the 'needed_pathkeys',
	 * just drop it.
	 */
	if (compare_pathkeys(needed_pathkeys,
						 useful_pathkeys) == PATHKEYS_EQUAL)
		return useful_pathkeys_list;

	useful_pathkeys_list = lappend(useful_pathkeys_list,
								   useful_pathkeys);

	return useful_pathkeys_list;
}

/*
 * create_ordered_paths
 *
 * Build a new upperrel containing Paths for ORDER BY evaluation.
 *
 * All paths in the result must satisfy the ORDER BY ordering.
 * The only new paths we need consider are an explicit full sort
 * and incremental sort on the cheapest-total existing path.
 *
 * input_rel: contains the source-data Paths
 * target: the output tlist the result Paths must emit
 * limit_tuples: estimated bound on the number of output tuples,
 *		or -1 if no LIMIT or couldn't estimate
 *
 * XXX This only looks at sort_pathkeys. I wonder if it needs to look at the
 * other pathkeys (grouping, ...) like generate_useful_gather_paths.
 */
static RelOptInfo *
create_ordered_paths(PlannerInfo *root,
					 RelOptInfo *input_rel,
					 PathTarget *target,
					 bool target_parallel_safe,
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
	if (input_rel->consider_parallel && target_parallel_safe)
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
		Path	   *input_path = (Path *) lfirst(lc);
		Path	   *sorted_path;
		bool		is_sorted;
		int			presorted_keys;

		is_sorted = pathkeys_count_contained_in(root->sort_pathkeys,
												input_path->pathkeys, &presorted_keys);

		if (is_sorted)
			sorted_path = input_path;
		else
		{
			/*
			 * Try at least sorting the cheapest path and also try
			 * incrementally sorting any path which is partially sorted
			 * already (no need to deal with paths which have presorted keys
			 * when incremental sort is disabled unless it's the cheapest
			 * input path).
			 */
			if (input_path != cheapest_input_path &&
				(presorted_keys == 0 || !enable_incremental_sort))
				continue;

			/*
			 * We've no need to consider both a sort and incremental sort.
			 * We'll just do a sort if there are no presorted keys and an
			 * incremental sort when there are presorted keys.
			 */
			if (presorted_keys == 0 || !enable_incremental_sort)
				sorted_path = (Path *) create_sort_path(root,
														ordered_rel,
														input_path,
														root->sort_pathkeys,
														limit_tuples);
			else
				sorted_path = (Path *) create_incremental_sort_path(root,
																	ordered_rel,
																	input_path,
																	root->sort_pathkeys,
																	presorted_keys,
																	limit_tuples);
		}

		/*
		 * If the pathtarget of the result path has different expressions from
		 * the target to be applied, a projection step is needed.
		 */
		if (!equal(sorted_path->pathtarget->exprs, target->exprs))
			sorted_path = apply_projection_to_path(root, ordered_rel,
												   sorted_path, target);

		add_path(ordered_rel, sorted_path);
	}

	/*
	 * generate_gather_paths() will have already generated a simple Gather
	 * path for the best parallel path, if any, and the loop above will have
	 * considered sorting it.  Similarly, generate_gather_paths() will also
	 * have generated order-preserving Gather Merge plans which can be used
	 * without sorting if they happen to match the sort_pathkeys, and the loop
	 * above will have handled those as well.  However, there's one more
	 * possibility: it may make sense to sort the cheapest partial path or
	 * incrementally sort any partial path that is partially sorted according
	 * to the required output order and then use Gather Merge.
	 */
	if (ordered_rel->consider_parallel && root->sort_pathkeys != NIL &&
		input_rel->partial_pathlist != NIL)
	{
		Path	   *cheapest_partial_path;

		cheapest_partial_path = linitial(input_rel->partial_pathlist);

		foreach(lc, input_rel->partial_pathlist)
		{
			Path	   *input_path = (Path *) lfirst(lc);
			Path	   *sorted_path;
			bool		is_sorted;
			int			presorted_keys;
			double		total_groups;

			is_sorted = pathkeys_count_contained_in(root->sort_pathkeys,
													input_path->pathkeys,
													&presorted_keys);

			if (is_sorted)
				continue;

			/*
			 * Try at least sorting the cheapest path and also try
			 * incrementally sorting any path which is partially sorted
			 * already (no need to deal with paths which have presorted keys
			 * when incremental sort is disabled unless it's the cheapest
			 * partial path).
			 */
			if (input_path != cheapest_partial_path &&
				(presorted_keys == 0 || !enable_incremental_sort))
				continue;

			/*
			 * We've no need to consider both a sort and incremental sort.
			 * We'll just do a sort if there are no presorted keys and an
			 * incremental sort when there are presorted keys.
			 */
			if (presorted_keys == 0 || !enable_incremental_sort)
				sorted_path = (Path *) create_sort_path(root,
														ordered_rel,
														input_path,
														root->sort_pathkeys,
														limit_tuples);
			else
				sorted_path = (Path *) create_incremental_sort_path(root,
																	ordered_rel,
																	input_path,
																	root->sort_pathkeys,
																	presorted_keys,
																	limit_tuples);
			total_groups = compute_gather_rows(sorted_path);
			sorted_path = (Path *)
				create_gather_merge_path(root, ordered_rel,
										 sorted_path,
										 sorted_path->pathtarget,
										 root->sort_pathkeys, NULL,
										 &total_groups);

			/*
			 * If the pathtarget of the result path has different expressions
			 * from the target to be applied, a projection step is needed.
			 */
			if (!equal(sorted_path->pathtarget->exprs, target->exprs))
				sorted_path = apply_projection_to_path(root, ordered_rel,
													   sorted_path, target);

			add_path(ordered_rel, sorted_path);
		}
	}

	/*
	 * If there is an FDW that's responsible for all baserels of the query,
	 * let it consider adding ForeignPaths.
	 */
	if (ordered_rel->fdwroutine &&
		ordered_rel->fdwroutine->GetForeignUpperPaths)
		ordered_rel->fdwroutine->GetForeignUpperPaths(root, UPPERREL_ORDERED,
													  input_rel, ordered_rel,
													  NULL);

	/* Let extensions possibly add some more paths */
	if (create_upper_paths_hook)
		(*create_upper_paths_hook) (root, UPPERREL_ORDERED,
									input_rel, ordered_rel, NULL);

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

		if (sgref && root->processed_groupClause &&
			get_sortgroupref_clause_noerr(sgref,
										  root->processed_groupClause) != NULL)
		{
			/*
			 * It's a grouping column, so add it to the input target as-is.
			 *
			 * Note that the target is logically below the grouping step.  So
			 * with grouping sets we need to remove the RT index of the
			 * grouping step if there is any from the target expression.
			 */
			if (parse->hasGroupRTE && parse->groupingSets != NIL)
			{
				Assert(root->group_rtindex > 0);
				expr = (Expr *)
					remove_nulling_relids((Node *) expr,
										  bms_make_singleton(root->group_rtindex),
										  NULL);
			}
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
	 *
	 * Note that the target is logically below the grouping step.  So with
	 * grouping sets we need to remove the RT index of the grouping step if
	 * there is any from the non-group Vars.
	 */
	non_group_vars = pull_var_clause((Node *) non_group_cols,
									 PVC_RECURSE_AGGREGATES |
									 PVC_RECURSE_WINDOWFUNCS |
									 PVC_INCLUDE_PLACEHOLDERS);
	if (parse->hasGroupRTE && parse->groupingSets != NIL)
	{
		Assert(root->group_rtindex > 0);
		non_group_vars = (List *)
			remove_nulling_relids((Node *) non_group_vars,
								  bms_make_singleton(root->group_rtindex),
								  NULL);
	}
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
 * In addition, we'd better emit any Vars and PlaceHolderVars that are
 * used outside of Aggrefs in the aggregation tlist and HAVING.  (Presumably,
 * these would be Vars that are grouped by or used in grouping expressions.)
 *
 * grouping_target is the tlist to be emitted by the topmost aggregation step.
 * havingQual represents the HAVING clause.
 */
static PathTarget *
make_partial_grouping_target(PlannerInfo *root,
							 PathTarget *grouping_target,
							 Node *havingQual)
{
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

		if (sgref && root->processed_groupClause &&
			get_sortgroupref_clause_noerr(sgref,
										  root->processed_groupClause) != NULL)
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
	if (havingQual)
		non_group_cols = lappend(non_group_cols, havingQual);

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
		TargetEntry *new_tle = lfirst_node(TargetEntry, l);
		TargetEntry *orig_tle;

		/* ignore resjunk columns in setop result */
		if (new_tle->resjunk)
			continue;

		Assert(orig_tlist_item != NULL);
		orig_tle = lfirst_node(TargetEntry, orig_tlist_item);
		orig_tlist_item = lnext(orig_tlist, orig_tlist_item);
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
 * optimize_window_clauses
 *		Call each WindowFunc's prosupport function to see if we're able to
 *		make any adjustments to any of the WindowClause's so that the executor
 *		can execute the window functions in a more optimal way.
 *
 * Currently we only allow adjustments to the WindowClause's frameOptions.  We
 * may allow more things to be done here in the future.
 */
static void
optimize_window_clauses(PlannerInfo *root, WindowFuncLists *wflists)
{
	List	   *windowClause = root->parse->windowClause;
	ListCell   *lc;

	foreach(lc, windowClause)
	{
		WindowClause *wc = lfirst_node(WindowClause, lc);
		ListCell   *lc2;
		int			optimizedFrameOptions = 0;

		Assert(wc->winref <= wflists->maxWinRef);

		/* skip any WindowClauses that have no WindowFuncs */
		if (wflists->windowFuncs[wc->winref] == NIL)
			continue;

		foreach(lc2, wflists->windowFuncs[wc->winref])
		{
			SupportRequestOptimizeWindowClause req;
			SupportRequestOptimizeWindowClause *res;
			WindowFunc *wfunc = lfirst_node(WindowFunc, lc2);
			Oid			prosupport;

			prosupport = get_func_support(wfunc->winfnoid);

			/* Check if there's a support function for 'wfunc' */
			if (!OidIsValid(prosupport))
				break;			/* can't optimize this WindowClause */

			req.type = T_SupportRequestOptimizeWindowClause;
			req.window_clause = wc;
			req.window_func = wfunc;
			req.frameOptions = wc->frameOptions;

			/* call the support function */
			res = (SupportRequestOptimizeWindowClause *)
				DatumGetPointer(OidFunctionCall1(prosupport,
												 PointerGetDatum(&req)));

			/*
			 * Skip to next WindowClause if the support function does not
			 * support this request type.
			 */
			if (res == NULL)
				break;

			/*
			 * Save these frameOptions for the first WindowFunc for this
			 * WindowClause.
			 */
			if (foreach_current_index(lc2) == 0)
				optimizedFrameOptions = res->frameOptions;

			/*
			 * On subsequent WindowFuncs, if the frameOptions are not the same
			 * then we're unable to optimize the frameOptions for this
			 * WindowClause.
			 */
			else if (optimizedFrameOptions != res->frameOptions)
				break;			/* skip to the next WindowClause, if any */
		}

		/* adjust the frameOptions if all WindowFunc's agree that it's ok */
		if (lc2 == NULL && wc->frameOptions != optimizedFrameOptions)
		{
			ListCell   *lc3;

			/* apply the new frame options */
			wc->frameOptions = optimizedFrameOptions;

			/*
			 * We now check to see if changing the frameOptions has caused
			 * this WindowClause to be a duplicate of some other WindowClause.
			 * This can only happen if we have multiple WindowClauses, so
			 * don't bother if there's only 1.
			 */
			if (list_length(windowClause) == 1)
				continue;

			/*
			 * Do the duplicate check and reuse the existing WindowClause if
			 * we find a duplicate.
			 */
			foreach(lc3, windowClause)
			{
				WindowClause *existing_wc = lfirst_node(WindowClause, lc3);

				/* skip over the WindowClause we're currently editing */
				if (existing_wc == wc)
					continue;

				/*
				 * Perform the same duplicate check that is done in
				 * transformWindowFuncCall.
				 */
				if (equal(wc->partitionClause, existing_wc->partitionClause) &&
					equal(wc->orderClause, existing_wc->orderClause) &&
					wc->frameOptions == existing_wc->frameOptions &&
					equal(wc->startOffset, existing_wc->startOffset) &&
					equal(wc->endOffset, existing_wc->endOffset))
				{
					ListCell   *lc4;

					/*
					 * Now move each WindowFunc in 'wc' into 'existing_wc'.
					 * This required adjusting each WindowFunc's winref and
					 * moving the WindowFuncs in 'wc' to the list of
					 * WindowFuncs in 'existing_wc'.
					 */
					foreach(lc4, wflists->windowFuncs[wc->winref])
					{
						WindowFunc *wfunc = lfirst_node(WindowFunc, lc4);

						wfunc->winref = existing_wc->winref;
					}

					/* move list items */
					wflists->windowFuncs[existing_wc->winref] = list_concat(wflists->windowFuncs[existing_wc->winref],
																			wflists->windowFuncs[wc->winref]);
					wflists->windowFuncs[wc->winref] = NIL;

					/*
					 * transformWindowFuncCall() should have made sure there
					 * are no other duplicates, so we needn't bother looking
					 * any further.
					 */
					break;
				}
			}
		}
	}
}

/*
 * select_active_windows
 *		Create a list of the "active" window clauses (ie, those referenced
 *		by non-deleted WindowFuncs) in the order they are to be executed.
 */
static List *
select_active_windows(PlannerInfo *root, WindowFuncLists *wflists)
{
	List	   *windowClause = root->parse->windowClause;
	List	   *result = NIL;
	ListCell   *lc;
	int			nActive = 0;
	WindowClauseSortData *actives = palloc(sizeof(WindowClauseSortData)
										   * list_length(windowClause));

	/* First, construct an array of the active windows */
	foreach(lc, windowClause)
	{
		WindowClause *wc = lfirst_node(WindowClause, lc);

		/* It's only active if wflists shows some related WindowFuncs */
		Assert(wc->winref <= wflists->maxWinRef);
		if (wflists->windowFuncs[wc->winref] == NIL)
			continue;

		actives[nActive].wc = wc;	/* original clause */

		/*
		 * For sorting, we want the list of partition keys followed by the
		 * list of sort keys. But pathkeys construction will remove duplicates
		 * between the two, so we can as well (even though we can't detect all
		 * of the duplicates, since some may come from ECs - that might mean
		 * we miss optimization chances here). We must, however, ensure that
		 * the order of entries is preserved with respect to the ones we do
		 * keep.
		 *
		 * partitionClause and orderClause had their own duplicates removed in
		 * parse analysis, so we're only concerned here with removing
		 * orderClause entries that also appear in partitionClause.
		 */
		actives[nActive].uniqueOrder =
			list_concat_unique(list_copy(wc->partitionClause),
							   wc->orderClause);
		nActive++;
	}

	/*
	 * Sort active windows by their partitioning/ordering clauses, ignoring
	 * any framing clauses, so that the windows that need the same sorting are
	 * adjacent in the list. When we come to generate paths, this will avoid
	 * inserting additional Sort nodes.
	 *
	 * This is how we implement a specific requirement from the SQL standard,
	 * which says that when two or more windows are order-equivalent (i.e.
	 * have matching partition and order clauses, even if their names or
	 * framing clauses differ), then all peer rows must be presented in the
	 * same order in all of them. If we allowed multiple sort nodes for such
	 * cases, we'd risk having the peer rows end up in different orders in
	 * equivalent windows due to sort instability. (See General Rule 4 of
	 * <window clause> in SQL2008 - SQL2016.)
	 *
	 * Additionally, if the entire list of clauses of one window is a prefix
	 * of another, put first the window with stronger sorting requirements.
	 * This way we will first sort for stronger window, and won't have to sort
	 * again for the weaker one.
	 */
	qsort(actives, nActive, sizeof(WindowClauseSortData), common_prefix_cmp);

	/* build ordered list of the original WindowClause nodes */
	for (int i = 0; i < nActive; i++)
		result = lappend(result, actives[i].wc);

	pfree(actives);

	return result;
}

/*
 * common_prefix_cmp
 *	  QSort comparison function for WindowClauseSortData
 *
 * Sort the windows by the required sorting clauses. First, compare the sort
 * clauses themselves. Second, if one window's clauses are a prefix of another
 * one's clauses, put the window with more sort clauses first.
 *
 * We purposefully sort by the highest tleSortGroupRef first.  Since
 * tleSortGroupRefs are assigned for the query's DISTINCT and ORDER BY first
 * and because here we sort the lowest tleSortGroupRefs last, if a
 * WindowClause is sharing a tleSortGroupRef with the query's DISTINCT or
 * ORDER BY clause, this makes it more likely that the final WindowAgg will
 * provide presorted input for the query's DISTINCT or ORDER BY clause, thus
 * reducing the total number of sorts required for the query.
 */
static int
common_prefix_cmp(const void *a, const void *b)
{
	const WindowClauseSortData *wcsa = a;
	const WindowClauseSortData *wcsb = b;
	ListCell   *item_a;
	ListCell   *item_b;

	forboth(item_a, wcsa->uniqueOrder, item_b, wcsb->uniqueOrder)
	{
		SortGroupClause *sca = lfirst_node(SortGroupClause, item_a);
		SortGroupClause *scb = lfirst_node(SortGroupClause, item_b);

		if (sca->tleSortGroupRef > scb->tleSortGroupRef)
			return -1;
		else if (sca->tleSortGroupRef < scb->tleSortGroupRef)
			return 1;
		else if (sca->sortop > scb->sortop)
			return -1;
		else if (sca->sortop < scb->sortop)
			return 1;
		else if (sca->nulls_first && !scb->nulls_first)
			return -1;
		else if (!sca->nulls_first && scb->nulls_first)
			return 1;
		/* no need to compare eqop, since it is fully determined by sortop */
	}

	if (list_length(wcsa->uniqueOrder) > list_length(wcsb->uniqueOrder))
		return -1;
	else if (list_length(wcsa->uniqueOrder) < list_length(wcsb->uniqueOrder))
		return 1;

	return 0;
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
	PathTarget *input_target;
	Bitmapset  *sgrefs;
	List	   *flattenable_cols;
	List	   *flattenable_vars;
	int			i;
	ListCell   *lc;

	Assert(root->parse->hasWindowFuncs);

	/*
	 * Collect the sortgroupref numbers of window PARTITION/ORDER BY clauses
	 * into a bitmapset for convenient reference below.
	 */
	sgrefs = NULL;
	foreach(lc, activeWindows)
	{
		WindowClause *wc = lfirst_node(WindowClause, lc);
		ListCell   *lc2;

		foreach(lc2, wc->partitionClause)
		{
			SortGroupClause *sortcl = lfirst_node(SortGroupClause, lc2);

			sgrefs = bms_add_member(sgrefs, sortcl->tleSortGroupRef);
		}
		foreach(lc2, wc->orderClause)
		{
			SortGroupClause *sortcl = lfirst_node(SortGroupClause, lc2);

			sgrefs = bms_add_member(sgrefs, sortcl->tleSortGroupRef);
		}
	}

	/* Add in sortgroupref numbers of GROUP BY clauses, too */
	foreach(lc, root->processed_groupClause)
	{
		SortGroupClause *grpcl = lfirst_node(SortGroupClause, lc);

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
 * Modifies wc's partitionClause to remove any clauses which are deemed
 * redundant by the pathkey logic.
 *
 * The required ordering is first the PARTITION keys, then the ORDER keys.
 * In the future we might try to implement windowing using hashing, in which
 * case the ordering could be relaxed, but for now we always sort.
 */
static List *
make_pathkeys_for_window(PlannerInfo *root, WindowClause *wc,
						 List *tlist)
{
	List	   *window_pathkeys = NIL;

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

	/*
	 * First fetch the pathkeys for the PARTITION BY clause.  We can safely
	 * remove any clauses from the wc->partitionClause for redundant pathkeys.
	 */
	if (wc->partitionClause != NIL)
	{
		bool		sortable;

		window_pathkeys = make_pathkeys_for_sortclauses_extended(root,
																 &wc->partitionClause,
																 tlist,
																 true,
																 false,
																 &sortable,
																 false);

		Assert(sortable);
	}

	/*
	 * In principle, we could also consider removing redundant ORDER BY items
	 * too as doing so does not alter the result of peer row checks done by
	 * the executor.  However, we must *not* remove the ordering column for
	 * RANGE OFFSET cases, as the executor needs that for in_range tests even
	 * if it's known to be equal to some partitioning column.
	 */
	if (wc->orderClause != NIL)
	{
		List	   *orderby_pathkeys;

		orderby_pathkeys = make_pathkeys_for_sortclauses(root,
														 wc->orderClause,
														 tlist);

		/* Okay, make the combined pathkeys */
		if (window_pathkeys != NIL)
			window_pathkeys = append_pathkeys(window_pathkeys, orderby_pathkeys);
		else
			window_pathkeys = orderby_pathkeys;
	}

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
 * run in sync in nodeProjectSet.  So if any SRFs are in sort columns, we
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
 * In addition, *have_postponed_srfs is set to true if we choose to postpone
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

	*have_postponed_srfs = false;	/* default result */

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
 * adjust_paths_for_srfs
 *		Fix up the Paths of the given upperrel to handle tSRFs properly.
 *
 * The executor can only handle set-returning functions that appear at the
 * top level of the targetlist of a ProjectSet plan node.  If we have any SRFs
 * that are not at top level, we need to split up the evaluation into multiple
 * plan levels in which each level satisfies this constraint.  This function
 * modifies each Path of an upperrel that (might) compute any SRFs in its
 * output tlist to insert appropriate projection steps.
 *
 * The given targets and targets_contain_srfs lists are from
 * split_pathtarget_at_srfs().  We assume the existing Paths emit the first
 * target in targets.
 */
static void
adjust_paths_for_srfs(PlannerInfo *root, RelOptInfo *rel,
					  List *targets, List *targets_contain_srfs)
{
	ListCell   *lc;

	Assert(list_length(targets) == list_length(targets_contain_srfs));
	Assert(!linitial_int(targets_contain_srfs));

	/* If no SRFs appear at this plan level, nothing to do */
	if (list_length(targets) == 1)
		return;

	/*
	 * Stack SRF-evaluation nodes atop each path for the rel.
	 *
	 * In principle we should re-run set_cheapest() here to identify the
	 * cheapest path, but it seems unlikely that adding the same tlist eval
	 * costs to all the paths would change that, so we don't bother. Instead,
	 * just assume that the cheapest-startup and cheapest-total paths remain
	 * so.  (There should be no parameterized paths anymore, so we needn't
	 * worry about updating cheapest_parameterized_paths.)
	 */
	foreach(lc, rel->pathlist)
	{
		Path	   *subpath = (Path *) lfirst(lc);
		Path	   *newpath = subpath;
		ListCell   *lc1,
				   *lc2;

		Assert(subpath->param_info == NULL);
		forboth(lc1, targets, lc2, targets_contain_srfs)
		{
			PathTarget *thistarget = lfirst_node(PathTarget, lc1);
			bool		contains_srfs = (bool) lfirst_int(lc2);

			/* If this level doesn't contain SRFs, do regular projection */
			if (contains_srfs)
				newpath = (Path *) create_set_projection_path(root,
															  rel,
															  newpath,
															  thistarget);
			else
				newpath = (Path *) apply_projection_to_path(root,
															rel,
															newpath,
															thistarget);
		}
		lfirst(lc) = newpath;
		if (subpath == rel->cheapest_startup_path)
			rel->cheapest_startup_path = newpath;
		if (subpath == rel->cheapest_total_path)
			rel->cheapest_total_path = newpath;
	}

	/* Likewise for partial paths, if any */
	foreach(lc, rel->partial_pathlist)
	{
		Path	   *subpath = (Path *) lfirst(lc);
		Path	   *newpath = subpath;
		ListCell   *lc1,
				   *lc2;

		Assert(subpath->param_info == NULL);
		forboth(lc1, targets, lc2, targets_contain_srfs)
		{
			PathTarget *thistarget = lfirst_node(PathTarget, lc1);
			bool		contains_srfs = (bool) lfirst_int(lc2);

			/* If this level doesn't contain SRFs, do regular projection */
			if (contains_srfs)
				newpath = (Path *) create_set_projection_path(root,
															  rel,
															  newpath,
															  thistarget);
			else
			{
				/* avoid apply_projection_to_path, in case of multiple refs */
				newpath = (Path *) create_projection_path(root,
														  rel,
														  newpath,
														  thistarget);
			}
		}
		lfirst(lc) = newpath;
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
 * This does not return any information about dependencies of the expression.
 * Hence callers should use the results only for the duration of the current
 * query.  Callers that would like to cache the results for longer should use
 * expression_planner_with_deps, probably via the plancache.
 *
 * Note: this must not make any damaging changes to the passed-in expression
 * tree.  (It would actually be okay to apply fix_opfuncids to it, but since
 * we first do an expression_tree_mutator-based walk, what is returned will
 * be a new node tree.)  The result is constructed in the current memory
 * context; beware that this can leak a lot of additional stuff there, too.
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
 * expression_planner_with_deps
 *		Perform planner's transformations on a standalone expression,
 *		returning expression dependency information along with the result.
 *
 * This is identical to expression_planner() except that it also returns
 * information about possible dependencies of the expression, ie identities of
 * objects whose definitions affect the result.  As in a PlannedStmt, these
 * are expressed as a list of relation Oids and a list of PlanInvalItems.
 */
Expr *
expression_planner_with_deps(Expr *expr,
							 List **relationOids,
							 List **invalItems)
{
	Node	   *result;
	PlannerGlobal glob;
	PlannerInfo root;

	/* Make up dummy planner state so we can use setrefs machinery */
	MemSet(&glob, 0, sizeof(glob));
	glob.type = T_PlannerGlobal;
	glob.relationOids = NIL;
	glob.invalItems = NIL;

	MemSet(&root, 0, sizeof(root));
	root.type = T_PlannerInfo;
	root.glob = &glob;

	/*
	 * Convert named-argument function calls, insert default arguments and
	 * simplify constant subexprs.  Collect identities of inlined functions
	 * and elided domains, too.
	 */
	result = eval_const_expressions(&root, (Node *) expr);

	/* Fill in opfuncid values if missing */
	fix_opfuncids(result);

	/*
	 * Now walk the finished expression to find anything else we ought to
	 * record as an expression dependency.
	 */
	(void) extract_query_dependencies_walker(result, &root);

	*relationOids = glob.relationOids;
	*invalItems = glob.invalItems;

	return (Expr *) result;
}


/*
 * plan_cluster_use_sort
 *		Use the planner to decide how CLUSTER should implement sorting
 *
 * tableOid is the OID of a table to be clustered on its index indexOid
 * (which is already known to be a btree index).  Decide whether it's
 * cheaper to do an indexscan or a seqscan-plus-sort to execute the CLUSTER.
 * Return true to use sorting, false to use an indexscan.
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
	root->join_domains = list_make1(makeNode(JoinDomain));

	/* Build a minimal RTE for the rel */
	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RELATION;
	rte->relid = tableOid;
	rte->relkind = RELKIND_RELATION;	/* Don't be too picky. */
	rte->rellockmode = AccessShareLock;
	rte->lateral = false;
	rte->inh = false;
	rte->inFromCl = true;
	query->rtable = list_make1(rte);
	addRTEPermissionInfo(&query->rteperminfos, rte);

	/* Set up RTE/RelOptInfo arrays */
	setup_simple_rel_arrays(root);

	/* Build RelOptInfo */
	rel = build_simple_rel(root, 1, NULL);

	/* Locate IndexOptInfo for the target index */
	indexInfo = NULL;
	foreach(lc, rel->indexlist)
	{
		indexInfo = lfirst_node(IndexOptInfo, lc);
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
			  seqScanPath->disabled_nodes,
			  seqScanPath->total_cost, rel->tuples, rel->reltarget->width,
			  comparisonCost, maintenance_work_mem, -1.0);

	/* Estimate the cost of index scan */
	indexScanPath = create_index_path(root, indexInfo,
									  NIL, NIL, NIL, NIL,
									  ForwardScanDirection, false,
									  NULL, 1.0, false);

	return (seqScanAndSortPath.total_cost < indexScanPath->path.total_cost);
}

/*
 * plan_create_index_workers
 *		Use the planner to decide how many parallel worker processes
 *		CREATE INDEX should request for use
 *
 * tableOid is the table on which the index is to be built.  indexOid is the
 * OID of an index to be created or reindexed (which must be a btree index).
 *
 * Return value is the number of parallel worker processes to request.  It
 * may be unsafe to proceed if this is 0.  Note that this does not include the
 * leader participating as a worker (value is always a number of parallel
 * worker processes).
 *
 * Note: caller had better already hold some type of lock on the table and
 * index.
 */
int
plan_create_index_workers(Oid tableOid, Oid indexOid)
{
	PlannerInfo *root;
	Query	   *query;
	PlannerGlobal *glob;
	RangeTblEntry *rte;
	Relation	heap;
	Relation	index;
	RelOptInfo *rel;
	int			parallel_workers;
	BlockNumber heap_blocks;
	double		reltuples;
	double		allvisfrac;

	/*
	 * We don't allow performing parallel operation in standalone backend or
	 * when parallelism is disabled.
	 */
	if (!IsUnderPostmaster || max_parallel_maintenance_workers == 0)
		return 0;

	/* Set up largely-dummy planner state */
	query = makeNode(Query);
	query->commandType = CMD_SELECT;

	glob = makeNode(PlannerGlobal);

	root = makeNode(PlannerInfo);
	root->parse = query;
	root->glob = glob;
	root->query_level = 1;
	root->planner_cxt = CurrentMemoryContext;
	root->wt_param_id = -1;
	root->join_domains = list_make1(makeNode(JoinDomain));

	/*
	 * Build a minimal RTE.
	 *
	 * Mark the RTE with inh = true.  This is a kludge to prevent
	 * get_relation_info() from fetching index info, which is necessary
	 * because it does not expect that any IndexOptInfo is currently
	 * undergoing REINDEX.
	 */
	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RELATION;
	rte->relid = tableOid;
	rte->relkind = RELKIND_RELATION;	/* Don't be too picky. */
	rte->rellockmode = AccessShareLock;
	rte->lateral = false;
	rte->inh = true;
	rte->inFromCl = true;
	query->rtable = list_make1(rte);
	addRTEPermissionInfo(&query->rteperminfos, rte);

	/* Set up RTE/RelOptInfo arrays */
	setup_simple_rel_arrays(root);

	/* Build RelOptInfo */
	rel = build_simple_rel(root, 1, NULL);

	/* Rels are assumed already locked by the caller */
	heap = table_open(tableOid, NoLock);
	index = index_open(indexOid, NoLock);

	/*
	 * Determine if it's safe to proceed.
	 *
	 * Currently, parallel workers can't access the leader's temporary tables.
	 * Furthermore, any index predicate or index expressions must be parallel
	 * safe.
	 */
	if (heap->rd_rel->relpersistence == RELPERSISTENCE_TEMP ||
		!is_parallel_safe(root, (Node *) RelationGetIndexExpressions(index)) ||
		!is_parallel_safe(root, (Node *) RelationGetIndexPredicate(index)))
	{
		parallel_workers = 0;
		goto done;
	}

	/*
	 * If parallel_workers storage parameter is set for the table, accept that
	 * as the number of parallel worker processes to launch (though still cap
	 * at max_parallel_maintenance_workers).  Note that we deliberately do not
	 * consider any other factor when parallel_workers is set. (e.g., memory
	 * use by workers.)
	 */
	if (rel->rel_parallel_workers != -1)
	{
		parallel_workers = Min(rel->rel_parallel_workers,
							   max_parallel_maintenance_workers);
		goto done;
	}

	/*
	 * Estimate heap relation size ourselves, since rel->pages cannot be
	 * trusted (heap RTE was marked as inheritance parent)
	 */
	estimate_rel_size(heap, NULL, &heap_blocks, &reltuples, &allvisfrac);

	/*
	 * Determine number of workers to scan the heap relation using generic
	 * model
	 */
	parallel_workers = compute_parallel_worker(rel, heap_blocks, -1,
											   max_parallel_maintenance_workers);

	/*
	 * Cap workers based on available maintenance_work_mem as needed.
	 *
	 * Note that each tuplesort participant receives an even share of the
	 * total maintenance_work_mem budget.  Aim to leave participants
	 * (including the leader as a participant) with no less than 32MB of
	 * memory.  This leaves cases where maintenance_work_mem is set to 64MB
	 * immediately past the threshold of being capable of launching a single
	 * parallel worker to sort.
	 */
	while (parallel_workers > 0 &&
		   maintenance_work_mem / (parallel_workers + 1) < 32768L)
		parallel_workers--;

done:
	index_close(index, NoLock);
	table_close(heap, NoLock);

	return parallel_workers;
}

/*
 * add_paths_to_grouping_rel
 *
 * Add non-partial paths to grouping relation.
 */
static void
add_paths_to_grouping_rel(PlannerInfo *root, RelOptInfo *input_rel,
						  RelOptInfo *grouped_rel,
						  RelOptInfo *partially_grouped_rel,
						  const AggClauseCosts *agg_costs,
						  grouping_sets_data *gd, double dNumGroups,
						  GroupPathExtraData *extra)
{
	Query	   *parse = root->parse;
	Path	   *cheapest_path = input_rel->cheapest_total_path;
	ListCell   *lc;
	bool		can_hash = (extra->flags & GROUPING_CAN_USE_HASH) != 0;
	bool		can_sort = (extra->flags & GROUPING_CAN_USE_SORT) != 0;
	List	   *havingQual = (List *) extra->havingQual;
	AggClauseCosts *agg_final_costs = &extra->agg_final_costs;

	if (can_sort)
	{
		/*
		 * Use any available suitably-sorted path as input, and also consider
		 * sorting the cheapest-total path and incremental sort on any paths
		 * with presorted keys.
		 */
		foreach(lc, input_rel->pathlist)
		{
			ListCell   *lc2;
			Path	   *path = (Path *) lfirst(lc);
			Path	   *path_save = path;
			List	   *pathkey_orderings = NIL;

			/* generate alternative group orderings that might be useful */
			pathkey_orderings = get_useful_group_keys_orderings(root, path);

			Assert(list_length(pathkey_orderings) > 0);

			foreach(lc2, pathkey_orderings)
			{
				GroupByOrdering *info = (GroupByOrdering *) lfirst(lc2);

				/* restore the path (we replace it in the loop) */
				path = path_save;

				path = make_ordered_path(root,
										 grouped_rel,
										 path,
										 cheapest_path,
										 info->pathkeys,
										 -1.0);
				if (path == NULL)
					continue;

				/* Now decide what to stick atop it */
				if (parse->groupingSets)
				{
					consider_groupingsets_paths(root, grouped_rel,
												path, true, can_hash,
												gd, agg_costs, dNumGroups);
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
											 grouped_rel->reltarget,
											 parse->groupClause ? AGG_SORTED : AGG_PLAIN,
											 AGGSPLIT_SIMPLE,
											 info->clauses,
											 havingQual,
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
											   info->clauses,
											   havingQual,
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
		 * Instead of operating directly on the input relation, we can
		 * consider finalizing a partially aggregated path.
		 */
		if (partially_grouped_rel != NULL)
		{
			foreach(lc, partially_grouped_rel->pathlist)
			{
				ListCell   *lc2;
				Path	   *path = (Path *) lfirst(lc);
				Path	   *path_save = path;
				List	   *pathkey_orderings = NIL;

				/* generate alternative group orderings that might be useful */
				pathkey_orderings = get_useful_group_keys_orderings(root, path);

				Assert(list_length(pathkey_orderings) > 0);

				/* process all potentially interesting grouping reorderings */
				foreach(lc2, pathkey_orderings)
				{
					GroupByOrdering *info = (GroupByOrdering *) lfirst(lc2);

					/* restore the path (we replace it in the loop) */
					path = path_save;

					path = make_ordered_path(root,
											 grouped_rel,
											 path,
											 partially_grouped_rel->cheapest_total_path,
											 info->pathkeys,
											 -1.0);

					if (path == NULL)
						continue;

					if (parse->hasAggs)
						add_path(grouped_rel, (Path *)
								 create_agg_path(root,
												 grouped_rel,
												 path,
												 grouped_rel->reltarget,
												 parse->groupClause ? AGG_SORTED : AGG_PLAIN,
												 AGGSPLIT_FINAL_DESERIAL,
												 info->clauses,
												 havingQual,
												 agg_final_costs,
												 dNumGroups));
					else
						add_path(grouped_rel, (Path *)
								 create_group_path(root,
												   grouped_rel,
												   path,
												   info->clauses,
												   havingQual,
												   dNumGroups));

				}
			}
		}
	}

	if (can_hash)
	{
		if (parse->groupingSets)
		{
			/*
			 * Try for a hash-only groupingsets path over unsorted input.
			 */
			consider_groupingsets_paths(root, grouped_rel,
										cheapest_path, false, true,
										gd, agg_costs, dNumGroups);
		}
		else
		{
			/*
			 * Generate a HashAgg Path.  We just need an Agg over the
			 * cheapest-total input path, since input order won't matter.
			 */
			add_path(grouped_rel, (Path *)
					 create_agg_path(root, grouped_rel,
									 cheapest_path,
									 grouped_rel->reltarget,
									 AGG_HASHED,
									 AGGSPLIT_SIMPLE,
									 root->processed_groupClause,
									 havingQual,
									 agg_costs,
									 dNumGroups));
		}

		/*
		 * Generate a Finalize HashAgg Path atop of the cheapest partially
		 * grouped path, assuming there is one
		 */
		if (partially_grouped_rel && partially_grouped_rel->pathlist)
		{
			Path	   *path = partially_grouped_rel->cheapest_total_path;

			add_path(grouped_rel, (Path *)
					 create_agg_path(root,
									 grouped_rel,
									 path,
									 grouped_rel->reltarget,
									 AGG_HASHED,
									 AGGSPLIT_FINAL_DESERIAL,
									 root->processed_groupClause,
									 havingQual,
									 agg_final_costs,
									 dNumGroups));
		}
	}

	/*
	 * When partitionwise aggregate is used, we might have fully aggregated
	 * paths in the partial pathlist, because add_paths_to_append_rel() will
	 * consider a path for grouped_rel consisting of a Parallel Append of
	 * non-partial paths from each child.
	 */
	if (grouped_rel->partial_pathlist != NIL)
		gather_grouping_paths(root, grouped_rel);
}

/*
 * create_partial_grouping_paths
 *
 * Create a new upper relation representing the result of partial aggregation
 * and populate it with appropriate paths.  Note that we don't finalize the
 * lists of paths here, so the caller can add additional partial or non-partial
 * paths and must afterward call gather_grouping_paths and set_cheapest on
 * the returned upper relation.
 *
 * All paths for this new upper relation -- both partial and non-partial --
 * have been partially aggregated but require a subsequent FinalizeAggregate
 * step.
 *
 * NB: This function is allowed to return NULL if it determines that there is
 * no real need to create a new RelOptInfo.
 */
static RelOptInfo *
create_partial_grouping_paths(PlannerInfo *root,
							  RelOptInfo *grouped_rel,
							  RelOptInfo *input_rel,
							  grouping_sets_data *gd,
							  GroupPathExtraData *extra,
							  bool force_rel_creation)
{
	Query	   *parse = root->parse;
	RelOptInfo *partially_grouped_rel;
	AggClauseCosts *agg_partial_costs = &extra->agg_partial_costs;
	AggClauseCosts *agg_final_costs = &extra->agg_final_costs;
	Path	   *cheapest_partial_path = NULL;
	Path	   *cheapest_total_path = NULL;
	double		dNumPartialGroups = 0;
	double		dNumPartialPartialGroups = 0;
	ListCell   *lc;
	bool		can_hash = (extra->flags & GROUPING_CAN_USE_HASH) != 0;
	bool		can_sort = (extra->flags & GROUPING_CAN_USE_SORT) != 0;

	/*
	 * Consider whether we should generate partially aggregated non-partial
	 * paths.  We can only do this if we have a non-partial path, and only if
	 * the parent of the input rel is performing partial partitionwise
	 * aggregation.  (Note that extra->patype is the type of partitionwise
	 * aggregation being used at the parent level, not this level.)
	 */
	if (input_rel->pathlist != NIL &&
		extra->patype == PARTITIONWISE_AGGREGATE_PARTIAL)
		cheapest_total_path = input_rel->cheapest_total_path;

	/*
	 * If parallelism is possible for grouped_rel, then we should consider
	 * generating partially-grouped partial paths.  However, if the input rel
	 * has no partial paths, then we can't.
	 */
	if (grouped_rel->consider_parallel && input_rel->partial_pathlist != NIL)
		cheapest_partial_path = linitial(input_rel->partial_pathlist);

	/*
	 * If we can't partially aggregate partial paths, and we can't partially
	 * aggregate non-partial paths, then don't bother creating the new
	 * RelOptInfo at all, unless the caller specified force_rel_creation.
	 */
	if (cheapest_total_path == NULL &&
		cheapest_partial_path == NULL &&
		!force_rel_creation)
		return NULL;

	/*
	 * Build a new upper relation to represent the result of partially
	 * aggregating the rows from the input relation.
	 */
	partially_grouped_rel = fetch_upper_rel(root,
											UPPERREL_PARTIAL_GROUP_AGG,
											grouped_rel->relids);
	partially_grouped_rel->consider_parallel =
		grouped_rel->consider_parallel;
	partially_grouped_rel->reloptkind = grouped_rel->reloptkind;
	partially_grouped_rel->serverid = grouped_rel->serverid;
	partially_grouped_rel->userid = grouped_rel->userid;
	partially_grouped_rel->useridiscurrent = grouped_rel->useridiscurrent;
	partially_grouped_rel->fdwroutine = grouped_rel->fdwroutine;

	/*
	 * Build target list for partial aggregate paths.  These paths cannot just
	 * emit the same tlist as regular aggregate paths, because (1) we must
	 * include Vars and Aggrefs needed in HAVING, which might not appear in
	 * the result tlist, and (2) the Aggrefs must be set in partial mode.
	 */
	partially_grouped_rel->reltarget =
		make_partial_grouping_target(root, grouped_rel->reltarget,
									 extra->havingQual);

	if (!extra->partial_costs_set)
	{
		/*
		 * Collect statistics about aggregates for estimating costs of
		 * performing aggregation in parallel.
		 */
		MemSet(agg_partial_costs, 0, sizeof(AggClauseCosts));
		MemSet(agg_final_costs, 0, sizeof(AggClauseCosts));
		if (parse->hasAggs)
		{
			/* partial phase */
			get_agg_clause_costs(root, AGGSPLIT_INITIAL_SERIAL,
								 agg_partial_costs);

			/* final phase */
			get_agg_clause_costs(root, AGGSPLIT_FINAL_DESERIAL,
								 agg_final_costs);
		}

		extra->partial_costs_set = true;
	}

	/* Estimate number of partial groups. */
	if (cheapest_total_path != NULL)
		dNumPartialGroups =
			get_number_of_groups(root,
								 cheapest_total_path->rows,
								 gd,
								 extra->targetList);
	if (cheapest_partial_path != NULL)
		dNumPartialPartialGroups =
			get_number_of_groups(root,
								 cheapest_partial_path->rows,
								 gd,
								 extra->targetList);

	if (can_sort && cheapest_total_path != NULL)
	{
		/* This should have been checked previously */
		Assert(parse->hasAggs || parse->groupClause);

		/*
		 * Use any available suitably-sorted path as input, and also consider
		 * sorting the cheapest partial path.
		 */
		foreach(lc, input_rel->pathlist)
		{
			ListCell   *lc2;
			Path	   *path = (Path *) lfirst(lc);
			Path	   *path_save = path;
			List	   *pathkey_orderings = NIL;

			/* generate alternative group orderings that might be useful */
			pathkey_orderings = get_useful_group_keys_orderings(root, path);

			Assert(list_length(pathkey_orderings) > 0);

			/* process all potentially interesting grouping reorderings */
			foreach(lc2, pathkey_orderings)
			{
				GroupByOrdering *info = (GroupByOrdering *) lfirst(lc2);

				/* restore the path (we replace it in the loop) */
				path = path_save;

				path = make_ordered_path(root,
										 partially_grouped_rel,
										 path,
										 cheapest_total_path,
										 info->pathkeys,
										 -1.0);

				if (path == NULL)
					continue;

				if (parse->hasAggs)
					add_path(partially_grouped_rel, (Path *)
							 create_agg_path(root,
											 partially_grouped_rel,
											 path,
											 partially_grouped_rel->reltarget,
											 parse->groupClause ? AGG_SORTED : AGG_PLAIN,
											 AGGSPLIT_INITIAL_SERIAL,
											 info->clauses,
											 NIL,
											 agg_partial_costs,
											 dNumPartialGroups));
				else
					add_path(partially_grouped_rel, (Path *)
							 create_group_path(root,
											   partially_grouped_rel,
											   path,
											   info->clauses,
											   NIL,
											   dNumPartialGroups));
			}
		}
	}

	if (can_sort && cheapest_partial_path != NULL)
	{
		/* Similar to above logic, but for partial paths. */
		foreach(lc, input_rel->partial_pathlist)
		{
			ListCell   *lc2;
			Path	   *path = (Path *) lfirst(lc);
			Path	   *path_save = path;
			List	   *pathkey_orderings = NIL;

			/* generate alternative group orderings that might be useful */
			pathkey_orderings = get_useful_group_keys_orderings(root, path);

			Assert(list_length(pathkey_orderings) > 0);

			/* process all potentially interesting grouping reorderings */
			foreach(lc2, pathkey_orderings)
			{
				GroupByOrdering *info = (GroupByOrdering *) lfirst(lc2);


				/* restore the path (we replace it in the loop) */
				path = path_save;

				path = make_ordered_path(root,
										 partially_grouped_rel,
										 path,
										 cheapest_partial_path,
										 info->pathkeys,
										 -1.0);

				if (path == NULL)
					continue;

				if (parse->hasAggs)
					add_partial_path(partially_grouped_rel, (Path *)
									 create_agg_path(root,
													 partially_grouped_rel,
													 path,
													 partially_grouped_rel->reltarget,
													 parse->groupClause ? AGG_SORTED : AGG_PLAIN,
													 AGGSPLIT_INITIAL_SERIAL,
													 info->clauses,
													 NIL,
													 agg_partial_costs,
													 dNumPartialPartialGroups));
				else
					add_partial_path(partially_grouped_rel, (Path *)
									 create_group_path(root,
													   partially_grouped_rel,
													   path,
													   info->clauses,
													   NIL,
													   dNumPartialPartialGroups));
			}
		}
	}

	/*
	 * Add a partially-grouped HashAgg Path where possible
	 */
	if (can_hash && cheapest_total_path != NULL)
	{
		/* Checked above */
		Assert(parse->hasAggs || parse->groupClause);

		add_path(partially_grouped_rel, (Path *)
				 create_agg_path(root,
								 partially_grouped_rel,
								 cheapest_total_path,
								 partially_grouped_rel->reltarget,
								 AGG_HASHED,
								 AGGSPLIT_INITIAL_SERIAL,
								 root->processed_groupClause,
								 NIL,
								 agg_partial_costs,
								 dNumPartialGroups));
	}

	/*
	 * Now add a partially-grouped HashAgg partial Path where possible
	 */
	if (can_hash && cheapest_partial_path != NULL)
	{
		add_partial_path(partially_grouped_rel, (Path *)
						 create_agg_path(root,
										 partially_grouped_rel,
										 cheapest_partial_path,
										 partially_grouped_rel->reltarget,
										 AGG_HASHED,
										 AGGSPLIT_INITIAL_SERIAL,
										 root->processed_groupClause,
										 NIL,
										 agg_partial_costs,
										 dNumPartialPartialGroups));
	}

	/*
	 * If there is an FDW that's responsible for all baserels of the query,
	 * let it consider adding partially grouped ForeignPaths.
	 */
	if (partially_grouped_rel->fdwroutine &&
		partially_grouped_rel->fdwroutine->GetForeignUpperPaths)
	{
		FdwRoutine *fdwroutine = partially_grouped_rel->fdwroutine;

		fdwroutine->GetForeignUpperPaths(root,
										 UPPERREL_PARTIAL_GROUP_AGG,
										 input_rel, partially_grouped_rel,
										 extra);
	}

	return partially_grouped_rel;
}

/*
 * make_ordered_path
 *		Return a path ordered by 'pathkeys' based on the given 'path'.  May
 *		return NULL if it doesn't make sense to generate an ordered path in
 *		this case.
 */
static Path *
make_ordered_path(PlannerInfo *root, RelOptInfo *rel, Path *path,
				  Path *cheapest_path, List *pathkeys, double limit_tuples)
{
	bool		is_sorted;
	int			presorted_keys;

	is_sorted = pathkeys_count_contained_in(pathkeys,
											path->pathkeys,
											&presorted_keys);

	if (!is_sorted)
	{
		/*
		 * Try at least sorting the cheapest path and also try incrementally
		 * sorting any path which is partially sorted already (no need to deal
		 * with paths which have presorted keys when incremental sort is
		 * disabled unless it's the cheapest input path).
		 */
		if (path != cheapest_path &&
			(presorted_keys == 0 || !enable_incremental_sort))
			return NULL;

		/*
		 * We've no need to consider both a sort and incremental sort. We'll
		 * just do a sort if there are no presorted keys and an incremental
		 * sort when there are presorted keys.
		 */
		if (presorted_keys == 0 || !enable_incremental_sort)
			path = (Path *) create_sort_path(root,
											 rel,
											 path,
											 pathkeys,
											 limit_tuples);
		else
			path = (Path *) create_incremental_sort_path(root,
														 rel,
														 path,
														 pathkeys,
														 presorted_keys,
														 limit_tuples);
	}

	return path;
}

/*
 * Generate Gather and Gather Merge paths for a grouping relation or partial
 * grouping relation.
 *
 * generate_useful_gather_paths does most of the work, but we also consider a
 * special case: we could try sorting the data by the group_pathkeys and then
 * applying Gather Merge.
 *
 * NB: This function shouldn't be used for anything other than a grouped or
 * partially grouped relation not only because of the fact that it explicitly
 * references group_pathkeys but we pass "true" as the third argument to
 * generate_useful_gather_paths().
 */
static void
gather_grouping_paths(PlannerInfo *root, RelOptInfo *rel)
{
	ListCell   *lc;
	Path	   *cheapest_partial_path;
	List	   *groupby_pathkeys;

	/*
	 * This occurs after any partial aggregation has taken place, so trim off
	 * any pathkeys added for ORDER BY / DISTINCT aggregates.
	 */
	if (list_length(root->group_pathkeys) > root->num_groupby_pathkeys)
		groupby_pathkeys = list_copy_head(root->group_pathkeys,
										  root->num_groupby_pathkeys);
	else
		groupby_pathkeys = root->group_pathkeys;

	/* Try Gather for unordered paths and Gather Merge for ordered ones. */
	generate_useful_gather_paths(root, rel, true);

	cheapest_partial_path = linitial(rel->partial_pathlist);

	/* XXX Shouldn't this also consider the group-key-reordering? */
	foreach(lc, rel->partial_pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);
		bool		is_sorted;
		int			presorted_keys;
		double		total_groups;

		is_sorted = pathkeys_count_contained_in(groupby_pathkeys,
												path->pathkeys,
												&presorted_keys);

		if (is_sorted)
			continue;

		/*
		 * Try at least sorting the cheapest path and also try incrementally
		 * sorting any path which is partially sorted already (no need to deal
		 * with paths which have presorted keys when incremental sort is
		 * disabled unless it's the cheapest input path).
		 */
		if (path != cheapest_partial_path &&
			(presorted_keys == 0 || !enable_incremental_sort))
			continue;

		/*
		 * We've no need to consider both a sort and incremental sort. We'll
		 * just do a sort if there are no presorted keys and an incremental
		 * sort when there are presorted keys.
		 */
		if (presorted_keys == 0 || !enable_incremental_sort)
			path = (Path *) create_sort_path(root, rel, path,
											 groupby_pathkeys,
											 -1.0);
		else
			path = (Path *) create_incremental_sort_path(root,
														 rel,
														 path,
														 groupby_pathkeys,
														 presorted_keys,
														 -1.0);
		total_groups = compute_gather_rows(path);
		path = (Path *)
			create_gather_merge_path(root,
									 rel,
									 path,
									 rel->reltarget,
									 groupby_pathkeys,
									 NULL,
									 &total_groups);

		add_path(rel, path);
	}
}

/*
 * can_partial_agg
 *
 * Determines whether or not partial grouping and/or aggregation is possible.
 * Returns true when possible, false otherwise.
 */
static bool
can_partial_agg(PlannerInfo *root)
{
	Query	   *parse = root->parse;

	if (!parse->hasAggs && parse->groupClause == NIL)
	{
		/*
		 * We don't know how to do parallel aggregation unless we have either
		 * some aggregates or a grouping clause.
		 */
		return false;
	}
	else if (parse->groupingSets)
	{
		/* We don't know how to do grouping sets in parallel. */
		return false;
	}
	else if (root->hasNonPartialAggs || root->hasNonSerialAggs)
	{
		/* Insufficient support for partial mode. */
		return false;
	}

	/* Everything looks good. */
	return true;
}

/*
 * apply_scanjoin_target_to_paths
 *
 * Adjust the final scan/join relation, and recursively all of its children,
 * to generate the final scan/join target.  It would be more correct to model
 * this as a separate planning step with a new RelOptInfo at the toplevel and
 * for each child relation, but doing it this way is noticeably cheaper.
 * Maybe that problem can be solved at some point, but for now we do this.
 *
 * If tlist_same_exprs is true, then the scan/join target to be applied has
 * the same expressions as the existing reltarget, so we need only insert the
 * appropriate sortgroupref information.  By avoiding the creation of
 * projection paths we save effort both immediately and at plan creation time.
 */
static void
apply_scanjoin_target_to_paths(PlannerInfo *root,
							   RelOptInfo *rel,
							   List *scanjoin_targets,
							   List *scanjoin_targets_contain_srfs,
							   bool scanjoin_target_parallel_safe,
							   bool tlist_same_exprs)
{
	bool		rel_is_partitioned = IS_PARTITIONED_REL(rel);
	PathTarget *scanjoin_target;
	ListCell   *lc;

	/* This recurses, so be paranoid. */
	check_stack_depth();

	/*
	 * If the rel is partitioned, we want to drop its existing paths and
	 * generate new ones.  This function would still be correct if we kept the
	 * existing paths: we'd modify them to generate the correct target above
	 * the partitioning Append, and then they'd compete on cost with paths
	 * generating the target below the Append.  However, in our current cost
	 * model the latter way is always the same or cheaper cost, so modifying
	 * the existing paths would just be useless work.  Moreover, when the cost
	 * is the same, varying roundoff errors might sometimes allow an existing
	 * path to be picked, resulting in undesirable cross-platform plan
	 * variations.  So we drop old paths and thereby force the work to be done
	 * below the Append, except in the case of a non-parallel-safe target.
	 *
	 * Some care is needed, because we have to allow
	 * generate_useful_gather_paths to see the old partial paths in the next
	 * stanza.  Hence, zap the main pathlist here, then allow
	 * generate_useful_gather_paths to add path(s) to the main list, and
	 * finally zap the partial pathlist.
	 */
	if (rel_is_partitioned)
		rel->pathlist = NIL;

	/*
	 * If the scan/join target is not parallel-safe, partial paths cannot
	 * generate it.
	 */
	if (!scanjoin_target_parallel_safe)
	{
		/*
		 * Since we can't generate the final scan/join target in parallel
		 * workers, this is our last opportunity to use any partial paths that
		 * exist; so build Gather path(s) that use them and emit whatever the
		 * current reltarget is.  We don't do this in the case where the
		 * target is parallel-safe, since we will be able to generate superior
		 * paths by doing it after the final scan/join target has been
		 * applied.
		 */
		generate_useful_gather_paths(root, rel, false);

		/* Can't use parallel query above this level. */
		rel->partial_pathlist = NIL;
		rel->consider_parallel = false;
	}

	/* Finish dropping old paths for a partitioned rel, per comment above */
	if (rel_is_partitioned)
		rel->partial_pathlist = NIL;

	/* Extract SRF-free scan/join target. */
	scanjoin_target = linitial_node(PathTarget, scanjoin_targets);

	/*
	 * Apply the SRF-free scan/join target to each existing path.
	 *
	 * If the tlist exprs are the same, we can just inject the sortgroupref
	 * information into the existing pathtargets.  Otherwise, replace each
	 * path with a projection path that generates the SRF-free scan/join
	 * target.  This can't change the ordering of paths within rel->pathlist,
	 * so we just modify the list in place.
	 */
	foreach(lc, rel->pathlist)
	{
		Path	   *subpath = (Path *) lfirst(lc);

		/* Shouldn't have any parameterized paths anymore */
		Assert(subpath->param_info == NULL);

		if (tlist_same_exprs)
			subpath->pathtarget->sortgrouprefs =
				scanjoin_target->sortgrouprefs;
		else
		{
			Path	   *newpath;

			newpath = (Path *) create_projection_path(root, rel, subpath,
													  scanjoin_target);
			lfirst(lc) = newpath;
		}
	}

	/* Likewise adjust the targets for any partial paths. */
	foreach(lc, rel->partial_pathlist)
	{
		Path	   *subpath = (Path *) lfirst(lc);

		/* Shouldn't have any parameterized paths anymore */
		Assert(subpath->param_info == NULL);

		if (tlist_same_exprs)
			subpath->pathtarget->sortgrouprefs =
				scanjoin_target->sortgrouprefs;
		else
		{
			Path	   *newpath;

			newpath = (Path *) create_projection_path(root, rel, subpath,
													  scanjoin_target);
			lfirst(lc) = newpath;
		}
	}

	/*
	 * Now, if final scan/join target contains SRFs, insert ProjectSetPath(s)
	 * atop each existing path.  (Note that this function doesn't look at the
	 * cheapest-path fields, which is a good thing because they're bogus right
	 * now.)
	 */
	if (root->parse->hasTargetSRFs)
		adjust_paths_for_srfs(root, rel,
							  scanjoin_targets,
							  scanjoin_targets_contain_srfs);

	/*
	 * Update the rel's target to be the final (with SRFs) scan/join target.
	 * This now matches the actual output of all the paths, and we might get
	 * confused in createplan.c if they don't agree.  We must do this now so
	 * that any append paths made in the next part will use the correct
	 * pathtarget (cf. create_append_path).
	 *
	 * Note that this is also necessary if GetForeignUpperPaths() gets called
	 * on the final scan/join relation or on any of its children, since the
	 * FDW might look at the rel's target to create ForeignPaths.
	 */
	rel->reltarget = llast_node(PathTarget, scanjoin_targets);

	/*
	 * If the relation is partitioned, recursively apply the scan/join target
	 * to all partitions, and generate brand-new Append paths in which the
	 * scan/join target is computed below the Append rather than above it.
	 * Since Append is not projection-capable, that might save a separate
	 * Result node, and it also is important for partitionwise aggregate.
	 */
	if (rel_is_partitioned)
	{
		List	   *live_children = NIL;
		int			i;

		/* Adjust each partition. */
		i = -1;
		while ((i = bms_next_member(rel->live_parts, i)) >= 0)
		{
			RelOptInfo *child_rel = rel->part_rels[i];
			AppendRelInfo **appinfos;
			int			nappinfos;
			List	   *child_scanjoin_targets = NIL;

			Assert(child_rel != NULL);

			/* Dummy children can be ignored. */
			if (IS_DUMMY_REL(child_rel))
				continue;

			/* Translate scan/join targets for this child. */
			appinfos = find_appinfos_by_relids(root, child_rel->relids,
											   &nappinfos);
			foreach(lc, scanjoin_targets)
			{
				PathTarget *target = lfirst_node(PathTarget, lc);

				target = copy_pathtarget(target);
				target->exprs = (List *)
					adjust_appendrel_attrs(root,
										   (Node *) target->exprs,
										   nappinfos, appinfos);
				child_scanjoin_targets = lappend(child_scanjoin_targets,
												 target);
			}
			pfree(appinfos);

			/* Recursion does the real work. */
			apply_scanjoin_target_to_paths(root, child_rel,
										   child_scanjoin_targets,
										   scanjoin_targets_contain_srfs,
										   scanjoin_target_parallel_safe,
										   tlist_same_exprs);

			/* Save non-dummy children for Append paths. */
			if (!IS_DUMMY_REL(child_rel))
				live_children = lappend(live_children, child_rel);
		}

		/* Build new paths for this relation by appending child paths. */
		add_paths_to_append_rel(root, rel, live_children);
	}

	/*
	 * Consider generating Gather or Gather Merge paths.  We must only do this
	 * if the relation is parallel safe, and we don't do it for child rels to
	 * avoid creating multiple Gather nodes within the same plan. We must do
	 * this after all paths have been generated and before set_cheapest, since
	 * one of the generated paths may turn out to be the cheapest one.
	 */
	if (rel->consider_parallel && !IS_OTHER_REL(rel))
		generate_useful_gather_paths(root, rel, false);

	/*
	 * Reassess which paths are the cheapest, now that we've potentially added
	 * new Gather (or Gather Merge) and/or Append (or MergeAppend) paths to
	 * this relation.
	 */
	set_cheapest(rel);
}

/*
 * create_partitionwise_grouping_paths
 *
 * If the partition keys of input relation are part of the GROUP BY clause, all
 * the rows belonging to a given group come from a single partition.  This
 * allows aggregation/grouping over a partitioned relation to be broken down
 * into aggregation/grouping on each partition.  This should be no worse, and
 * often better, than the normal approach.
 *
 * However, if the GROUP BY clause does not contain all the partition keys,
 * rows from a given group may be spread across multiple partitions. In that
 * case, we perform partial aggregation for each group, append the results,
 * and then finalize aggregation.  This is less certain to win than the
 * previous case.  It may win if the PartialAggregate stage greatly reduces
 * the number of groups, because fewer rows will pass through the Append node.
 * It may lose if we have lots of small groups.
 */
static void
create_partitionwise_grouping_paths(PlannerInfo *root,
									RelOptInfo *input_rel,
									RelOptInfo *grouped_rel,
									RelOptInfo *partially_grouped_rel,
									const AggClauseCosts *agg_costs,
									grouping_sets_data *gd,
									PartitionwiseAggregateType patype,
									GroupPathExtraData *extra)
{
	List	   *grouped_live_children = NIL;
	List	   *partially_grouped_live_children = NIL;
	PathTarget *target = grouped_rel->reltarget;
	bool		partial_grouping_valid = true;
	int			i;

	Assert(patype != PARTITIONWISE_AGGREGATE_NONE);
	Assert(patype != PARTITIONWISE_AGGREGATE_PARTIAL ||
		   partially_grouped_rel != NULL);

	/* Add paths for partitionwise aggregation/grouping. */
	i = -1;
	while ((i = bms_next_member(input_rel->live_parts, i)) >= 0)
	{
		RelOptInfo *child_input_rel = input_rel->part_rels[i];
		PathTarget *child_target;
		AppendRelInfo **appinfos;
		int			nappinfos;
		GroupPathExtraData child_extra;
		RelOptInfo *child_grouped_rel;
		RelOptInfo *child_partially_grouped_rel;

		Assert(child_input_rel != NULL);

		/* Dummy children can be ignored. */
		if (IS_DUMMY_REL(child_input_rel))
			continue;

		child_target = copy_pathtarget(target);

		/*
		 * Copy the given "extra" structure as is and then override the
		 * members specific to this child.
		 */
		memcpy(&child_extra, extra, sizeof(child_extra));

		appinfos = find_appinfos_by_relids(root, child_input_rel->relids,
										   &nappinfos);

		child_target->exprs = (List *)
			adjust_appendrel_attrs(root,
								   (Node *) target->exprs,
								   nappinfos, appinfos);

		/* Translate havingQual and targetList. */
		child_extra.havingQual = (Node *)
			adjust_appendrel_attrs(root,
								   extra->havingQual,
								   nappinfos, appinfos);
		child_extra.targetList = (List *)
			adjust_appendrel_attrs(root,
								   (Node *) extra->targetList,
								   nappinfos, appinfos);

		/*
		 * extra->patype was the value computed for our parent rel; patype is
		 * the value for this relation.  For the child, our value is its
		 * parent rel's value.
		 */
		child_extra.patype = patype;

		/*
		 * Create grouping relation to hold fully aggregated grouping and/or
		 * aggregation paths for the child.
		 */
		child_grouped_rel = make_grouping_rel(root, child_input_rel,
											  child_target,
											  extra->target_parallel_safe,
											  child_extra.havingQual);

		/* Create grouping paths for this child relation. */
		create_ordinary_grouping_paths(root, child_input_rel,
									   child_grouped_rel,
									   agg_costs, gd, &child_extra,
									   &child_partially_grouped_rel);

		if (child_partially_grouped_rel)
		{
			partially_grouped_live_children =
				lappend(partially_grouped_live_children,
						child_partially_grouped_rel);
		}
		else
			partial_grouping_valid = false;

		if (patype == PARTITIONWISE_AGGREGATE_FULL)
		{
			set_cheapest(child_grouped_rel);
			grouped_live_children = lappend(grouped_live_children,
											child_grouped_rel);
		}

		pfree(appinfos);
	}

	/*
	 * Try to create append paths for partially grouped children. For full
	 * partitionwise aggregation, we might have paths in the partial_pathlist
	 * if parallel aggregation is possible.  For partial partitionwise
	 * aggregation, we may have paths in both pathlist and partial_pathlist.
	 *
	 * NB: We must have a partially grouped path for every child in order to
	 * generate a partially grouped path for this relation.
	 */
	if (partially_grouped_rel && partial_grouping_valid)
	{
		Assert(partially_grouped_live_children != NIL);

		add_paths_to_append_rel(root, partially_grouped_rel,
								partially_grouped_live_children);

		/*
		 * We need call set_cheapest, since the finalization step will use the
		 * cheapest path from the rel.
		 */
		if (partially_grouped_rel->pathlist)
			set_cheapest(partially_grouped_rel);
	}

	/* If possible, create append paths for fully grouped children. */
	if (patype == PARTITIONWISE_AGGREGATE_FULL)
	{
		Assert(grouped_live_children != NIL);

		add_paths_to_append_rel(root, grouped_rel, grouped_live_children);
	}
}

/*
 * group_by_has_partkey
 *
 * Returns true if all the partition keys of the given relation are part of
 * the GROUP BY clauses, including having matching collation, false otherwise.
 */
static bool
group_by_has_partkey(RelOptInfo *input_rel,
					 List *targetList,
					 List *groupClause)
{
	List	   *groupexprs = get_sortgrouplist_exprs(groupClause, targetList);
	int			cnt = 0;
	int			partnatts;

	/* Input relation should be partitioned. */
	Assert(input_rel->part_scheme);

	/* Rule out early, if there are no partition keys present. */
	if (!input_rel->partexprs)
		return false;

	partnatts = input_rel->part_scheme->partnatts;

	for (cnt = 0; cnt < partnatts; cnt++)
	{
		List	   *partexprs = input_rel->partexprs[cnt];
		ListCell   *lc;
		bool		found = false;

		foreach(lc, partexprs)
		{
			ListCell   *lg;
			Expr	   *partexpr = lfirst(lc);
			Oid			partcoll = input_rel->part_scheme->partcollation[cnt];

			foreach(lg, groupexprs)
			{
				Expr	   *groupexpr = lfirst(lg);
				Oid			groupcoll = exprCollation((Node *) groupexpr);

				/*
				 * Note: we can assume there is at most one RelabelType node;
				 * eval_const_expressions() will have simplified if more than
				 * one.
				 */
				if (IsA(groupexpr, RelabelType))
					groupexpr = ((RelabelType *) groupexpr)->arg;

				if (equal(groupexpr, partexpr))
				{
					/*
					 * Reject a match if the grouping collation does not match
					 * the partitioning collation.
					 */
					if (OidIsValid(partcoll) && OidIsValid(groupcoll) &&
						partcoll != groupcoll)
						return false;

					found = true;
					break;
				}
			}

			if (found)
				break;
		}

		/*
		 * If none of the partition key expressions match with any of the
		 * GROUP BY expression, return false.
		 */
		if (!found)
			return false;
	}

	return true;
}

/*
 * generate_setop_child_grouplist
 *		Build a SortGroupClause list defining the sort/grouping properties
 *		of the child of a set operation.
 *
 * This is similar to generate_setop_grouplist() but differs as the setop
 * child query's targetlist entries may already have a tleSortGroupRef
 * assigned for other purposes, such as GROUP BYs.  Here we keep the
 * SortGroupClause list in the same order as 'op' groupClauses and just adjust
 * the tleSortGroupRef to reference the TargetEntry's 'ressortgroupref'.
 */
static List *
generate_setop_child_grouplist(SetOperationStmt *op, List *targetlist)
{
	List	   *grouplist = copyObject(op->groupClauses);
	ListCell   *lg;
	ListCell   *lt;

	lg = list_head(grouplist);
	foreach(lt, targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lt);
		SortGroupClause *sgc;

		/* resjunk columns could have sortgrouprefs.  Leave these alone */
		if (tle->resjunk)
			continue;

		/* we expect every non-resjunk target to have a SortGroupClause */
		Assert(lg != NULL);
		sgc = (SortGroupClause *) lfirst(lg);
		lg = lnext(grouplist, lg);

		/* assign a tleSortGroupRef, or reuse the existing one */
		sgc->tleSortGroupRef = assignSortGroupRef(tle, targetlist);
	}
	Assert(lg == NULL);
	return grouplist;
}
