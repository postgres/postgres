/*-------------------------------------------------------------------------
 *
 * planagg.c
 *	  Special planning for aggregate queries.
 *
 * This module tries to replace MIN/MAX aggregate functions by subqueries
 * of the form
 *		(SELECT col FROM tab
 *		 WHERE col IS NOT NULL AND existing-quals
 *		 ORDER BY col ASC/DESC
 *		 LIMIT 1)
 * Given a suitable index on tab.col, this can be much faster than the
 * generic scan-all-the-rows aggregation plan.	We can handle multiple
 * MIN/MAX aggregates by generating multiple subqueries, and their
 * orderings can be different.	However, if the query contains any
 * non-optimizable aggregates, there's no point since we'll have to
 * scan all the rows anyway.
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/planagg.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/subselect.h"
#include "parser/parsetree.h"
#include "parser/parse_clause.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static bool find_minmax_aggs_walker(Node *node, List **context);
static bool build_minmax_path(PlannerInfo *root, MinMaxAggInfo *mminfo,
				  Oid eqop, Oid sortop, bool nulls_first);
static void minmax_qp_callback(PlannerInfo *root, void *extra);
static void make_agg_subplan(PlannerInfo *root, MinMaxAggInfo *mminfo);
static Node *replace_aggs_with_params_mutator(Node *node, PlannerInfo *root);
static Oid	fetch_agg_sort_op(Oid aggfnoid);


/*
 * preprocess_minmax_aggregates - preprocess MIN/MAX aggregates
 *
 * Check to see whether the query contains MIN/MAX aggregate functions that
 * might be optimizable via indexscans.  If it does, and all the aggregates
 * are potentially optimizable, then set up root->minmax_aggs with a list of
 * these aggregates.
 *
 * Note: we are passed the preprocessed targetlist separately, because it's
 * not necessarily equal to root->parse->targetList.
 */
void
preprocess_minmax_aggregates(PlannerInfo *root, List *tlist)
{
	Query	   *parse = root->parse;
	FromExpr   *jtnode;
	RangeTblRef *rtr;
	RangeTblEntry *rte;
	List	   *aggs_list;
	ListCell   *lc;

	/* minmax_aggs list should be empty at this point */
	Assert(root->minmax_aggs == NIL);

	/* Nothing to do if query has no aggregates */
	if (!parse->hasAggs)
		return;

	Assert(!parse->setOperations);		/* shouldn't get here if a setop */
	Assert(parse->rowMarks == NIL);		/* nor if FOR UPDATE */

	/*
	 * Reject unoptimizable cases.
	 *
	 * We don't handle GROUP BY or windowing, because our current
	 * implementations of grouping require looking at all the rows anyway, and
	 * so there's not much point in optimizing MIN/MAX.  (Note: relaxing this
	 * would likely require some restructuring in grouping_planner(), since it
	 * performs assorted processing related to these features between calling
	 * preprocess_minmax_aggregates and optimize_minmax_aggregates.)
	 */
	if (parse->groupClause || parse->hasWindowFuncs)
		return;

	/*
	 * We also restrict the query to reference exactly one table, since join
	 * conditions can't be handled reasonably.  (We could perhaps handle a
	 * query containing cartesian-product joins, but it hardly seems worth the
	 * trouble.)  However, the single table could be buried in several levels
	 * of FromExpr due to subqueries.  Note the "single" table could be an
	 * inheritance parent, too, including the case of a UNION ALL subquery
	 * that's been flattened to an appendrel.
	 */
	jtnode = parse->jointree;
	while (IsA(jtnode, FromExpr))
	{
		if (list_length(jtnode->fromlist) != 1)
			return;
		jtnode = linitial(jtnode->fromlist);
	}
	if (!IsA(jtnode, RangeTblRef))
		return;
	rtr = (RangeTblRef *) jtnode;
	rte = planner_rt_fetch(rtr->rtindex, root);
	if (rte->rtekind == RTE_RELATION)
		 /* ordinary relation, ok */ ;
	else if (rte->rtekind == RTE_SUBQUERY && rte->inh)
		 /* flattened UNION ALL subquery, ok */ ;
	else
		return;

	/*
	 * Scan the tlist and HAVING qual to find all the aggregates and verify
	 * all are MIN/MAX aggregates.	Stop as soon as we find one that isn't.
	 */
	aggs_list = NIL;
	if (find_minmax_aggs_walker((Node *) tlist, &aggs_list))
		return;
	if (find_minmax_aggs_walker(parse->havingQual, &aggs_list))
		return;

	/*
	 * OK, there is at least the possibility of performing the optimization.
	 * Build an access path for each aggregate.  (We must do this now because
	 * we need to call query_planner with a pristine copy of the current query
	 * tree; it'll be too late when optimize_minmax_aggregates gets called.)
	 * If any of the aggregates prove to be non-indexable, give up; there is
	 * no point in optimizing just some of them.
	 */
	foreach(lc, aggs_list)
	{
		MinMaxAggInfo *mminfo = (MinMaxAggInfo *) lfirst(lc);
		Oid			eqop;
		bool		reverse;

		/*
		 * We'll need the equality operator that goes with the aggregate's
		 * ordering operator.
		 */
		eqop = get_equality_op_for_ordering_op(mminfo->aggsortop, &reverse);
		if (!OidIsValid(eqop))	/* shouldn't happen */
			elog(ERROR, "could not find equality operator for ordering operator %u",
				 mminfo->aggsortop);

		/*
		 * We can use either an ordering that gives NULLS FIRST or one that
		 * gives NULLS LAST; furthermore there's unlikely to be much
		 * performance difference between them, so it doesn't seem worth
		 * costing out both ways if we get a hit on the first one.	NULLS
		 * FIRST is more likely to be available if the operator is a
		 * reverse-sort operator, so try that first if reverse.
		 */
		if (build_minmax_path(root, mminfo, eqop, mminfo->aggsortop, reverse))
			continue;
		if (build_minmax_path(root, mminfo, eqop, mminfo->aggsortop, !reverse))
			continue;

		/* No indexable path for this aggregate, so fail */
		return;
	}

	/*
	 * We're done until path generation is complete.  Save info for later.
	 * (Setting root->minmax_aggs non-NIL signals we succeeded in making index
	 * access paths for all the aggregates.)
	 */
	root->minmax_aggs = aggs_list;
}

/*
 * optimize_minmax_aggregates - check for optimizing MIN/MAX via indexes
 *
 * Check to see whether using the aggregate indexscans is cheaper than the
 * generic aggregate method.  If so, generate and return a Plan that does it
 * that way.  Otherwise, return NULL.
 *
 * Note: it seems likely that the generic method will never be cheaper
 * in practice, except maybe for tiny tables where it'd hardly matter.
 * Should we skip even trying to build the standard plan, if
 * preprocess_minmax_aggregates succeeds?
 *
 * We are passed the preprocessed tlist, as well as the estimated costs for
 * doing the aggregates the regular way, and the best path devised for
 * computing the input of a standard Agg node.
 */
Plan *
optimize_minmax_aggregates(PlannerInfo *root, List *tlist,
						   const AggClauseCosts *aggcosts, Path *best_path)
{
	Query	   *parse = root->parse;
	Cost		total_cost;
	Path		agg_p;
	Plan	   *plan;
	Node	   *hqual;
	ListCell   *lc;

	/* Nothing to do if preprocess_minmax_aggs rejected the query */
	if (root->minmax_aggs == NIL)
		return NULL;

	/*
	 * Now we have enough info to compare costs against the generic aggregate
	 * implementation.
	 *
	 * Note that we don't include evaluation cost of the tlist here; this is
	 * OK since it isn't included in best_path's cost either, and should be
	 * the same in either case.
	 */
	total_cost = 0;
	foreach(lc, root->minmax_aggs)
	{
		MinMaxAggInfo *mminfo = (MinMaxAggInfo *) lfirst(lc);

		total_cost += mminfo->pathcost;
	}

	cost_agg(&agg_p, root, AGG_PLAIN, aggcosts,
			 0, 0,
			 best_path->startup_cost, best_path->total_cost,
			 best_path->parent->rows);

	if (total_cost > agg_p.total_cost)
		return NULL;			/* too expensive */

	/*
	 * OK, we are going to generate an optimized plan.
	 *
	 * First, generate a subplan and output Param node for each agg.
	 */
	foreach(lc, root->minmax_aggs)
	{
		MinMaxAggInfo *mminfo = (MinMaxAggInfo *) lfirst(lc);

		make_agg_subplan(root, mminfo);
	}

	/*
	 * Modify the targetlist and HAVING qual to reference subquery outputs
	 */
	tlist = (List *) replace_aggs_with_params_mutator((Node *) tlist, root);
	hqual = replace_aggs_with_params_mutator(parse->havingQual, root);

	/*
	 * We have to replace Aggrefs with Params in equivalence classes too, else
	 * ORDER BY or DISTINCT on an optimized aggregate will fail.  We don't
	 * need to process child eclass members though, since they aren't of
	 * interest anymore --- and replace_aggs_with_params_mutator isn't able to
	 * handle Aggrefs containing translated child Vars, anyway.
	 *
	 * Note: at some point it might become necessary to mutate other data
	 * structures too, such as the query's sortClause or distinctClause. Right
	 * now, those won't be examined after this point.
	 */
	mutate_eclass_expressions(root,
							  replace_aggs_with_params_mutator,
							  (void *) root,
							  false);

	/*
	 * Generate the output plan --- basically just a Result
	 */
	plan = (Plan *) make_result(root, tlist, hqual, NULL);

	/* Account for evaluation cost of the tlist (make_result did the rest) */
	add_tlist_costs_to_plan(root, plan, tlist);

	return plan;
}

/*
 * find_minmax_aggs_walker
 *		Recursively scan the Aggref nodes in an expression tree, and check
 *		that each one is a MIN/MAX aggregate.  If so, build a list of the
 *		distinct aggregate calls in the tree.
 *
 * Returns TRUE if a non-MIN/MAX aggregate is found, FALSE otherwise.
 * (This seemingly-backward definition is used because expression_tree_walker
 * aborts the scan on TRUE return, which is what we want.)
 *
 * Found aggregates are added to the list at *context; it's up to the caller
 * to initialize the list to NIL.
 *
 * This does not descend into subqueries, and so should be used only after
 * reduction of sublinks to subplans.  There mustn't be outer-aggregate
 * references either.
 */
static bool
find_minmax_aggs_walker(Node *node, List **context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
	{
		Aggref	   *aggref = (Aggref *) node;
		Oid			aggsortop;
		TargetEntry *curTarget;
		MinMaxAggInfo *mminfo;
		ListCell   *l;

		Assert(aggref->agglevelsup == 0);
		if (list_length(aggref->args) != 1 || aggref->aggorder != NIL)
			return true;		/* it couldn't be MIN/MAX */
		/* note: we do not care if DISTINCT is mentioned ... */
		curTarget = (TargetEntry *) linitial(aggref->args);

		aggsortop = fetch_agg_sort_op(aggref->aggfnoid);
		if (!OidIsValid(aggsortop))
			return true;		/* not a MIN/MAX aggregate */

		if (contain_mutable_functions((Node *) curTarget->expr))
			return true;		/* not potentially indexable */

		if (type_is_rowtype(exprType((Node *) curTarget->expr)))
			return true;		/* IS NOT NULL would have weird semantics */

		/*
		 * Check whether it's already in the list, and add it if not.
		 */
		foreach(l, *context)
		{
			mminfo = (MinMaxAggInfo *) lfirst(l);
			if (mminfo->aggfnoid == aggref->aggfnoid &&
				equal(mminfo->target, curTarget->expr))
				return false;
		}

		mminfo = makeNode(MinMaxAggInfo);
		mminfo->aggfnoid = aggref->aggfnoid;
		mminfo->aggsortop = aggsortop;
		mminfo->target = curTarget->expr;
		mminfo->subroot = NULL; /* don't compute path yet */
		mminfo->path = NULL;
		mminfo->pathcost = 0;
		mminfo->param = NULL;

		*context = lappend(*context, mminfo);

		/*
		 * We need not recurse into the argument, since it can't contain any
		 * aggregates.
		 */
		return false;
	}
	Assert(!IsA(node, SubLink));
	return expression_tree_walker(node, find_minmax_aggs_walker,
								  (void *) context);
}

/*
 * build_minmax_path
 *		Given a MIN/MAX aggregate, try to build an indexscan Path it can be
 *		optimized with.
 *
 * If successful, stash the best path in *mminfo and return TRUE.
 * Otherwise, return FALSE.
 */
static bool
build_minmax_path(PlannerInfo *root, MinMaxAggInfo *mminfo,
				  Oid eqop, Oid sortop, bool nulls_first)
{
	PlannerInfo *subroot;
	Query	   *parse;
	TargetEntry *tle;
	NullTest   *ntest;
	SortGroupClause *sortcl;
	Path	   *cheapest_path;
	Path	   *sorted_path;
	double		dNumGroups;
	Cost		path_cost;
	double		path_fraction;

	/*----------
	 * Generate modified query of the form
	 *		(SELECT col FROM tab
	 *		 WHERE col IS NOT NULL AND existing-quals
	 *		 ORDER BY col ASC/DESC
	 *		 LIMIT 1)
	 *----------
	 */
	subroot = (PlannerInfo *) palloc(sizeof(PlannerInfo));
	memcpy(subroot, root, sizeof(PlannerInfo));
	subroot->parse = parse = (Query *) copyObject(root->parse);
	/* make sure subroot planning won't change root->init_plans contents */
	subroot->init_plans = list_copy(root->init_plans);
	/* There shouldn't be any OJ or LATERAL info to translate, as yet */
	Assert(subroot->join_info_list == NIL);
	Assert(subroot->lateral_info_list == NIL);
	/* and we haven't created PlaceHolderInfos, either */
	Assert(subroot->placeholder_list == NIL);

	/* single tlist entry that is the aggregate target */
	tle = makeTargetEntry(copyObject(mminfo->target),
						  (AttrNumber) 1,
						  pstrdup("agg_target"),
						  false);
	parse->targetList = list_make1(tle);

	/* No HAVING, no DISTINCT, no aggregates anymore */
	parse->havingQual = NULL;
	subroot->hasHavingQual = false;
	parse->distinctClause = NIL;
	parse->hasDistinctOn = false;
	parse->hasAggs = false;

	/* Build "target IS NOT NULL" expression */
	ntest = makeNode(NullTest);
	ntest->nulltesttype = IS_NOT_NULL;
	ntest->arg = copyObject(mminfo->target);
	/* we checked it wasn't a rowtype in find_minmax_aggs_walker */
	ntest->argisrow = false;

	/* User might have had that in WHERE already */
	if (!list_member((List *) parse->jointree->quals, ntest))
		parse->jointree->quals = (Node *)
			lcons(ntest, (List *) parse->jointree->quals);

	/* Build suitable ORDER BY clause */
	sortcl = makeNode(SortGroupClause);
	sortcl->tleSortGroupRef = assignSortGroupRef(tle, parse->targetList);
	sortcl->eqop = eqop;
	sortcl->sortop = sortop;
	sortcl->nulls_first = nulls_first;
	sortcl->hashable = false;	/* no need to make this accurate */
	parse->sortClause = list_make1(sortcl);

	/* set up expressions for LIMIT 1 */
	parse->limitOffset = NULL;
	parse->limitCount = (Node *) makeConst(INT8OID, -1, InvalidOid,
										   sizeof(int64),
										   Int64GetDatum(1), false,
										   FLOAT8PASSBYVAL);

	/*
	 * Generate the best paths for this query, telling query_planner that we
	 * have LIMIT 1.
	 */
	query_planner(subroot, parse->targetList, 1.0, 1.0,
				  minmax_qp_callback, NULL,
				  &cheapest_path, &sorted_path, &dNumGroups);

	/*
	 * Fail if no presorted path.  However, if query_planner determines that
	 * the presorted path is also the cheapest, it will set sorted_path to
	 * NULL ... don't be fooled.  (This is kind of a pain here, but it
	 * simplifies life for grouping_planner, so leave it be.)
	 */
	if (!sorted_path)
	{
		if (cheapest_path &&
			pathkeys_contained_in(subroot->sort_pathkeys,
								  cheapest_path->pathkeys))
			sorted_path = cheapest_path;
		else
			return false;
	}

	/*
	 * Determine cost to get just the first row of the presorted path.
	 *
	 * Note: cost calculation here should match
	 * compare_fractional_path_costs().
	 */
	if (sorted_path->parent->rows > 1.0)
		path_fraction = 1.0 / sorted_path->parent->rows;
	else
		path_fraction = 1.0;

	path_cost = sorted_path->startup_cost +
		path_fraction * (sorted_path->total_cost - sorted_path->startup_cost);

	/* Save state for further processing */
	mminfo->subroot = subroot;
	mminfo->path = sorted_path;
	mminfo->pathcost = path_cost;

	return true;
}

/*
 * Compute query_pathkeys and other pathkeys during plan generation
 */
static void
minmax_qp_callback(PlannerInfo *root, void *extra)
{
	root->group_pathkeys = NIL;
	root->window_pathkeys = NIL;
	root->distinct_pathkeys = NIL;

	root->sort_pathkeys =
		make_pathkeys_for_sortclauses(root,
									  root->parse->sortClause,
									  root->parse->targetList);

	root->query_pathkeys = root->sort_pathkeys;
}

/*
 * Construct a suitable plan for a converted aggregate query
 */
static void
make_agg_subplan(PlannerInfo *root, MinMaxAggInfo *mminfo)
{
	PlannerInfo *subroot = mminfo->subroot;
	Query	   *subparse = subroot->parse;
	Plan	   *plan;

	/*
	 * Generate the plan for the subquery. We already have a Path, but we have
	 * to convert it to a Plan and attach a LIMIT node above it.
	 */
	plan = create_plan(subroot, mminfo->path);

	plan->targetlist = subparse->targetList;

	plan = (Plan *) make_limit(plan,
							   subparse->limitOffset,
							   subparse->limitCount,
							   0, 1);

	/*
	 * Convert the plan into an InitPlan, and make a Param for its result.
	 */
	mminfo->param =
		SS_make_initplan_from_plan(subroot, plan,
								   exprType((Node *) mminfo->target),
								   -1,
								   exprCollation((Node *) mminfo->target));

	/*
	 * Make sure the initplan gets into the outer PlannerInfo, along with any
	 * other initplans generated by the sub-planning run.  We had to include
	 * the outer PlannerInfo's pre-existing initplans into the inner one's
	 * init_plans list earlier, so make sure we don't put back any duplicate
	 * entries.
	 */
	root->init_plans = list_concat_unique_ptr(root->init_plans,
											  subroot->init_plans);
}

/*
 * Replace original aggregate calls with subplan output Params
 */
static Node *
replace_aggs_with_params_mutator(Node *node, PlannerInfo *root)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Aggref))
	{
		Aggref	   *aggref = (Aggref *) node;
		TargetEntry *curTarget = (TargetEntry *) linitial(aggref->args);
		ListCell   *lc;

		foreach(lc, root->minmax_aggs)
		{
			MinMaxAggInfo *mminfo = (MinMaxAggInfo *) lfirst(lc);

			if (mminfo->aggfnoid == aggref->aggfnoid &&
				equal(mminfo->target, curTarget->expr))
				return (Node *) mminfo->param;
		}
		elog(ERROR, "failed to re-find MinMaxAggInfo record");
	}
	Assert(!IsA(node, SubLink));
	return expression_tree_mutator(node, replace_aggs_with_params_mutator,
								   (void *) root);
}

/*
 * Get the OID of the sort operator, if any, associated with an aggregate.
 * Returns InvalidOid if there is no such operator.
 */
static Oid
fetch_agg_sort_op(Oid aggfnoid)
{
	HeapTuple	aggTuple;
	Form_pg_aggregate aggform;
	Oid			aggsortop;

	/* fetch aggregate entry from pg_aggregate */
	aggTuple = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(aggfnoid));
	if (!HeapTupleIsValid(aggTuple))
		return InvalidOid;
	aggform = (Form_pg_aggregate) GETSTRUCT(aggTuple);
	aggsortop = aggform->aggsortop;
	ReleaseSysCache(aggTuple);

	return aggsortop;
}
