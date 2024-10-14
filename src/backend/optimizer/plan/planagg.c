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
 * generic scan-all-the-rows aggregation plan.  We can handle multiple
 * MIN/MAX aggregates by generating multiple subqueries, and their
 * orderings can be different.  However, if the query contains any
 * non-optimizable aggregates, there's no point since we'll have to
 * scan all the rows anyway.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
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
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "parser/parse_clause.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

static bool can_minmax_aggs(PlannerInfo *root, List **context);
static bool build_minmax_path(PlannerInfo *root, MinMaxAggInfo *mminfo,
							  Oid eqop, Oid sortop, bool reverse_sort,
							  bool nulls_first);
static void minmax_qp_callback(PlannerInfo *root, void *extra);
static Oid	fetch_agg_sort_op(Oid aggfnoid);


/*
 * preprocess_minmax_aggregates - preprocess MIN/MAX aggregates
 *
 * Check to see whether the query contains MIN/MAX aggregate functions that
 * might be optimizable via indexscans.  If it does, and all the aggregates
 * are potentially optimizable, then create a MinMaxAggPath and add it to
 * the (UPPERREL_GROUP_AGG, NULL) upperrel.
 *
 * This should be called by grouping_planner() just before it's ready to call
 * query_planner(), because we generate indexscan paths by cloning the
 * planner's state and invoking query_planner() on a modified version of
 * the query parsetree.  Thus, all preprocessing needed before query_planner()
 * must already be done.  This relies on the list of aggregates in
 * root->agginfos, so preprocess_aggrefs() must have been called already, too.
 */
void
preprocess_minmax_aggregates(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	FromExpr   *jtnode;
	RangeTblRef *rtr;
	RangeTblEntry *rte;
	List	   *aggs_list;
	RelOptInfo *grouped_rel;
	ListCell   *lc;

	/* minmax_aggs list should be empty at this point */
	Assert(root->minmax_aggs == NIL);

	/* Nothing to do if query has no aggregates */
	if (!parse->hasAggs)
		return;

	Assert(!parse->setOperations);	/* shouldn't get here if a setop */
	Assert(parse->rowMarks == NIL); /* nor if FOR UPDATE */

	/*
	 * Reject unoptimizable cases.
	 *
	 * We don't handle GROUP BY or windowing, because our current
	 * implementations of grouping require looking at all the rows anyway, and
	 * so there's not much point in optimizing MIN/MAX.
	 */
	if (parse->groupClause || list_length(parse->groupingSets) > 1 ||
		parse->hasWindowFuncs)
		return;

	/*
	 * Reject if query contains any CTEs; there's no way to build an indexscan
	 * on one so we couldn't succeed here.  (If the CTEs are unreferenced,
	 * that's not true, but it doesn't seem worth expending cycles to check.)
	 */
	if (parse->cteList)
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
	 * Examine all the aggregates and verify all are MIN/MAX aggregates.  Stop
	 * as soon as we find one that isn't.
	 */
	aggs_list = NIL;
	if (!can_minmax_aggs(root, &aggs_list))
		return;

	/*
	 * OK, there is at least the possibility of performing the optimization.
	 * Build an access path for each aggregate.  If any of the aggregates
	 * prove to be non-indexable, give up; there is no point in optimizing
	 * just some of them.
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
		 * costing out both ways if we get a hit on the first one.  NULLS
		 * FIRST is more likely to be available if the operator is a
		 * reverse-sort operator, so try that first if reverse.
		 */
		if (build_minmax_path(root, mminfo, eqop, mminfo->aggsortop, reverse, reverse))
			continue;
		if (build_minmax_path(root, mminfo, eqop, mminfo->aggsortop, reverse, !reverse))
			continue;

		/* No indexable path for this aggregate, so fail */
		return;
	}

	/*
	 * OK, we can do the query this way.  Prepare to create a MinMaxAggPath
	 * node.
	 *
	 * First, create an output Param node for each agg.  (If we end up not
	 * using the MinMaxAggPath, we'll waste a PARAM_EXEC slot for each agg,
	 * which is not worth worrying about.  We can't wait till create_plan time
	 * to decide whether to make the Param, unfortunately.)
	 */
	foreach(lc, aggs_list)
	{
		MinMaxAggInfo *mminfo = (MinMaxAggInfo *) lfirst(lc);

		mminfo->param =
			SS_make_initplan_output_param(root,
										  exprType((Node *) mminfo->target),
										  -1,
										  exprCollation((Node *) mminfo->target));
	}

	/*
	 * Create a MinMaxAggPath node with the appropriate estimated costs and
	 * other needed data, and add it to the UPPERREL_GROUP_AGG upperrel, where
	 * it will compete against the standard aggregate implementation.  (It
	 * will likely always win, but we need not assume that here.)
	 *
	 * Note: grouping_planner won't have created this upperrel yet, but it's
	 * fine for us to create it first.  We will not have inserted the correct
	 * consider_parallel value in it, but MinMaxAggPath paths are currently
	 * never parallel-safe anyway, so that doesn't matter.  Likewise, it
	 * doesn't matter that we haven't filled FDW-related fields in the rel.
	 * Also, because there are no rowmarks, we know that the processed_tlist
	 * doesn't need to change anymore, so making the pathtarget now is safe.
	 */
	grouped_rel = fetch_upper_rel(root, UPPERREL_GROUP_AGG, NULL);
	add_path(grouped_rel, (Path *)
			 create_minmaxagg_path(root, grouped_rel,
								   create_pathtarget(root,
													 root->processed_tlist),
								   aggs_list,
								   (List *) parse->havingQual));
}

/*
 * can_minmax_aggs
 *		Examine all the aggregates in the query, and check if they are
 *		all MIN/MAX aggregates.  If so, build a list of MinMaxAggInfo
 *		nodes for them.
 *
 * Returns false if a non-MIN/MAX aggregate is found, true otherwise.
 */
static bool
can_minmax_aggs(PlannerInfo *root, List **context)
{
	ListCell   *lc;

	/*
	 * This function used to have to scan the query for itself, but now we can
	 * just thumb through the AggInfo list made by preprocess_aggrefs.
	 */
	foreach(lc, root->agginfos)
	{
		AggInfo    *agginfo = lfirst_node(AggInfo, lc);
		Aggref	   *aggref = linitial_node(Aggref, agginfo->aggrefs);
		Oid			aggsortop;
		TargetEntry *curTarget;
		MinMaxAggInfo *mminfo;

		Assert(aggref->agglevelsup == 0);
		if (list_length(aggref->args) != 1)
			return false;		/* it couldn't be MIN/MAX */

		/*
		 * ORDER BY is usually irrelevant for MIN/MAX, but it can change the
		 * outcome if the aggsortop's operator class recognizes non-identical
		 * values as equal.  For example, 4.0 and 4.00 are equal according to
		 * numeric_ops, yet distinguishable.  If MIN() receives more than one
		 * value equal to 4.0 and no value less than 4.0, it is unspecified
		 * which of those equal values MIN() returns.  An ORDER BY expression
		 * that differs for each of those equal values of the argument
		 * expression makes the result predictable once again.  This is a
		 * niche requirement, and we do not implement it with subquery paths.
		 * In any case, this test lets us reject ordered-set aggregates
		 * quickly.
		 */
		if (aggref->aggorder != NIL)
			return false;
		/* note: we do not care if DISTINCT is mentioned ... */

		/*
		 * We might implement the optimization when a FILTER clause is present
		 * by adding the filter to the quals of the generated subquery.  For
		 * now, just punt.
		 */
		if (aggref->aggfilter != NULL)
			return false;

		aggsortop = fetch_agg_sort_op(aggref->aggfnoid);
		if (!OidIsValid(aggsortop))
			return false;		/* not a MIN/MAX aggregate */

		curTarget = (TargetEntry *) linitial(aggref->args);

		if (contain_mutable_functions((Node *) curTarget->expr))
			return false;		/* not potentially indexable */

		if (type_is_rowtype(exprType((Node *) curTarget->expr)))
			return false;		/* IS NOT NULL would have weird semantics */

		mminfo = makeNode(MinMaxAggInfo);
		mminfo->aggfnoid = aggref->aggfnoid;
		mminfo->aggsortop = aggsortop;
		mminfo->target = curTarget->expr;
		mminfo->subroot = NULL; /* don't compute path yet */
		mminfo->path = NULL;
		mminfo->pathcost = 0;
		mminfo->param = NULL;

		*context = lappend(*context, mminfo);
	}
	return true;
}

/*
 * build_minmax_path
 *		Given a MIN/MAX aggregate, try to build an indexscan Path it can be
 *		optimized with.
 *
 * If successful, stash the best path in *mminfo and return true.
 * Otherwise, return false.
 */
static bool
build_minmax_path(PlannerInfo *root, MinMaxAggInfo *mminfo,
				  Oid eqop, Oid sortop, bool reverse_sort, bool nulls_first)
{
	PlannerInfo *subroot;
	Query	   *parse;
	TargetEntry *tle;
	List	   *tlist;
	NullTest   *ntest;
	SortGroupClause *sortcl;
	RelOptInfo *final_rel;
	Path	   *sorted_path;
	Cost		path_cost;
	double		path_fraction;

	/*
	 * We are going to construct what is effectively a sub-SELECT query, so
	 * clone the current query level's state and adjust it to make it look
	 * like a subquery.  Any outer references will now be one level higher
	 * than before.  (This means that when we are done, there will be no Vars
	 * of level 1, which is why the subquery can become an initplan.)
	 */
	subroot = (PlannerInfo *) palloc(sizeof(PlannerInfo));
	memcpy(subroot, root, sizeof(PlannerInfo));
	subroot->query_level++;
	subroot->parent_root = root;
	/* reset subplan-related stuff */
	subroot->plan_params = NIL;
	subroot->outer_params = NULL;
	subroot->init_plans = NIL;
	subroot->agginfos = NIL;
	subroot->aggtransinfos = NIL;

	subroot->parse = parse = copyObject(root->parse);
	IncrementVarSublevelsUp((Node *) parse, 1, 1);

	/* append_rel_list might contain outer Vars? */
	subroot->append_rel_list = copyObject(root->append_rel_list);
	IncrementVarSublevelsUp((Node *) subroot->append_rel_list, 1, 1);
	/* There shouldn't be any OJ info to translate, as yet */
	Assert(subroot->join_info_list == NIL);
	/* and we haven't made equivalence classes, either */
	Assert(subroot->eq_classes == NIL);
	/* and we haven't created PlaceHolderInfos, either */
	Assert(subroot->placeholder_list == NIL);

	/*----------
	 * Generate modified query of the form
	 *		(SELECT col FROM tab
	 *		 WHERE col IS NOT NULL AND existing-quals
	 *		 ORDER BY col ASC/DESC
	 *		 LIMIT 1)
	 *----------
	 */
	/* single tlist entry that is the aggregate target */
	tle = makeTargetEntry(copyObject(mminfo->target),
						  (AttrNumber) 1,
						  pstrdup("agg_target"),
						  false);
	tlist = list_make1(tle);
	subroot->processed_tlist = parse->targetList = tlist;

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
	/* we checked it wasn't a rowtype in can_minmax_aggs */
	ntest->argisrow = false;
	ntest->location = -1;

	/* User might have had that in WHERE already */
	if (!list_member((List *) parse->jointree->quals, ntest))
		parse->jointree->quals = (Node *)
			lcons(ntest, (List *) parse->jointree->quals);

	/* Build suitable ORDER BY clause */
	sortcl = makeNode(SortGroupClause);
	sortcl->tleSortGroupRef = assignSortGroupRef(tle, subroot->processed_tlist);
	sortcl->eqop = eqop;
	sortcl->sortop = sortop;
	sortcl->reverse_sort = reverse_sort;
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
	subroot->tuple_fraction = 1.0;
	subroot->limit_tuples = 1.0;

	final_rel = query_planner(subroot, minmax_qp_callback, NULL);

	/*
	 * Since we didn't go through subquery_planner() to handle the subquery,
	 * we have to do some of the same cleanup it would do, in particular cope
	 * with params and initplans used within this subquery.  (This won't
	 * matter if we end up not using the subplan.)
	 */
	SS_identify_outer_params(subroot);
	SS_charge_for_initplans(subroot, final_rel);

	/*
	 * Get the best presorted path, that being the one that's cheapest for
	 * fetching just one row.  If there's no such path, fail.
	 */
	if (final_rel->rows > 1.0)
		path_fraction = 1.0 / final_rel->rows;
	else
		path_fraction = 1.0;

	sorted_path =
		get_cheapest_fractional_path_for_pathkeys(final_rel->pathlist,
												  subroot->query_pathkeys,
												  NULL,
												  path_fraction);
	if (!sorted_path)
		return false;

	/*
	 * The path might not return exactly what we want, so fix that.  (We
	 * assume that this won't change any conclusions about which was the
	 * cheapest path.)
	 */
	sorted_path = apply_projection_to_path(subroot, final_rel, sorted_path,
										   create_pathtarget(subroot,
															 subroot->processed_tlist));

	/*
	 * Determine cost to get just the first row of the presorted path.
	 *
	 * Note: cost calculation here should match
	 * compare_fractional_path_costs().
	 */
	path_cost = sorted_path->startup_cost +
		path_fraction * (sorted_path->total_cost - sorted_path->startup_cost);

	/* Save state for further processing */
	mminfo->subroot = subroot;
	mminfo->path = sorted_path;
	mminfo->pathcost = path_cost;

	return true;
}

/*
 * Compute query_pathkeys and other pathkeys during query_planner()
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
