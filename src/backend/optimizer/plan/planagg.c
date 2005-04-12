/*-------------------------------------------------------------------------
 *
 * planagg.c
 *	  Special planning for aggregate queries.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/plan/planagg.c,v 1.2 2005/04/12 04:26:24 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/skey.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/subselect.h"
#include "parser/parsetree.h"
#include "parser/parse_clause.h"
#include "parser/parse_expr.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


typedef struct
{
	Oid			aggfnoid;		/* pg_proc Oid of the aggregate */
	Oid			aggsortop;		/* Oid of its sort operator */
	Expr	   *target;			/* expression we are aggregating on */
	IndexPath  *path;			/* access path for index scan */
	Cost		pathcost;		/* estimated cost to fetch first row */
	Param	   *param;			/* param for subplan's output */
} MinMaxAggInfo;

static bool find_minmax_aggs_walker(Node *node, List **context);
static bool build_minmax_path(Query *root, RelOptInfo *rel,
							  MinMaxAggInfo *info);
static ScanDirection match_agg_to_index_col(MinMaxAggInfo *info,
											IndexOptInfo *index, int indexcol);
static void make_agg_subplan(Query *root, MinMaxAggInfo *info,
							 List *constant_quals);
static Node *replace_aggs_with_params_mutator(Node *node,  List **context);
static Oid	fetch_agg_sort_op(Oid aggfnoid);


/*
 * optimize_minmax_aggregates - check for optimizing MIN/MAX via indexes
 *
 * This checks to see if we can replace MIN/MAX aggregate functions by
 * subqueries of the form
 *		(SELECT col FROM tab WHERE ... ORDER BY col ASC/DESC LIMIT 1)
 * Given a suitable index on tab.col, this can be much faster than the
 * generic scan-all-the-rows plan.
 *
 * We are passed the Query, the preprocessed tlist, and the best path
 * devised for computing the input of a standard Agg node.  If we are able
 * to optimize all the aggregates, and the result is estimated to be cheaper
 * than the generic aggregate method, then generate and return a Plan that
 * does it that way.  Otherwise, return NULL.
 */
Plan *
optimize_minmax_aggregates(Query *root, List *tlist, Path *best_path)
{
	RangeTblRef *rtr;
	RangeTblEntry *rte;
	RelOptInfo *rel;
	List	   *aggs_list;
	ListCell   *l;
	Cost		total_cost;
	Path		agg_p;
	Plan	   *plan;
	Node	   *hqual;
	QualCost	tlist_cost;
	List	   *constant_quals;

	/* Nothing to do if query has no aggregates */
	if (!root->hasAggs)
		return NULL;

	Assert(!root->setOperations); /* shouldn't get here if a setop */
	Assert(root->rowMarks == NIL); /* nor if FOR UPDATE */

	/*
	 * Reject unoptimizable cases.
	 *
	 * We don't handle GROUP BY, because our current implementations of
	 * grouping require looking at all the rows anyway, and so there's not
	 * much point in optimizing MIN/MAX.
	 */
	if (root->groupClause)
		return NULL;

	/*
	 * We also restrict the query to reference exactly one table, since
	 * join conditions can't be handled reasonably.  (We could perhaps
	 * handle a query containing cartesian-product joins, but it hardly
	 * seems worth the trouble.)
	 */
	Assert(root->jointree != NULL && IsA(root->jointree, FromExpr));
	if (list_length(root->jointree->fromlist) != 1)
		return NULL;
	rtr = (RangeTblRef *) linitial(root->jointree->fromlist);
	if (!IsA(rtr, RangeTblRef))
		return NULL;
	rte = rt_fetch(rtr->rtindex, root->rtable);
	if (rte->rtekind != RTE_RELATION)
		return NULL;
	rel = find_base_rel(root, rtr->rtindex);

	/*
	 * Also reject cases with subplans or volatile functions in WHERE.
	 * This may be overly paranoid, but it's not entirely clear if the
	 * transformation is safe then.
	 */
	if (contain_subplans(root->jointree->quals) ||
		contain_volatile_functions(root->jointree->quals))
		return NULL;

	/*
	 * Since this optimization is not applicable all that often, we want
	 * to fall out before doing very much work if possible.  Therefore
	 * we do the work in several passes.  The first pass scans the tlist
	 * and HAVING qual to find all the aggregates and verify that
	 * each of them is a MIN/MAX aggregate.  If that succeeds, the second
	 * pass looks at each aggregate to see if it is optimizable; if so
	 * we make an IndexPath describing how we would scan it.  (We do not
	 * try to optimize if only some aggs are optimizable, since that means
	 * we'll have to scan all the rows anyway.)  If that succeeds, we have
	 * enough info to compare costs against the generic implementation.
	 * Only if that test passes do we build a Plan.
	 */

	/* Pass 1: find all the aggregates */
	aggs_list = NIL;
	if (find_minmax_aggs_walker((Node *) tlist, &aggs_list))
		return NULL;
	if (find_minmax_aggs_walker(root->havingQual, &aggs_list))
		return NULL;

	/* Pass 2: see if each one is optimizable */
	total_cost = 0;
	foreach(l, aggs_list)
	{
		MinMaxAggInfo *info = (MinMaxAggInfo *) lfirst(l);

		if (!build_minmax_path(root, rel, info))
			return NULL;
		total_cost += info->pathcost;
	}

	/*
	 * Make the cost comparison.
	 *
	 * Note that we don't include evaluation cost of the tlist here;
	 * this is OK since it isn't included in best_path's cost either,
	 * and should be the same in either case.
	 */
	cost_agg(&agg_p, root, AGG_PLAIN, list_length(aggs_list),
			 0, 0,
			 best_path->startup_cost, best_path->total_cost,
			 best_path->parent->rows);

	if (total_cost > agg_p.total_cost)
		return NULL;			/* too expensive */

	/*
	 * OK, we are going to generate an optimized plan.  The first thing we
	 * need to do is look for any non-variable WHERE clauses that query_planner
	 * might have removed from the basic plan.  (Normal WHERE clauses will
	 * be properly incorporated into the sub-plans by create_plan.)  If there
	 * are any, they will be in a gating Result node atop the best_path.
	 * They have to be incorporated into a gating Result in each sub-plan
	 * in order to produce the semantically correct result.
	 */
	if (IsA(best_path, ResultPath))
	{
		Assert(((ResultPath *) best_path)->subpath != NULL);
		constant_quals = ((ResultPath *) best_path)->constantqual;
	}
	else
		constant_quals = NIL;

	/* Pass 3: generate subplans and output Param nodes */
	foreach(l, aggs_list)
	{
		make_agg_subplan(root, (MinMaxAggInfo *) lfirst(l), constant_quals);
	}

	/*
	 * Modify the targetlist and HAVING qual to reference subquery outputs
	 */
	tlist = (List *) replace_aggs_with_params_mutator((Node *) tlist,
													  &aggs_list);
	hqual = replace_aggs_with_params_mutator(root->havingQual,
											 &aggs_list);

	/*
	 * Generate the output plan --- basically just a Result
	 */
	plan = (Plan *) make_result(tlist, hqual, NULL);

	/* Account for evaluation cost of the tlist (make_result did the rest) */
	cost_qual_eval(&tlist_cost, tlist);
	plan->startup_cost += tlist_cost.startup;
	plan->total_cost += tlist_cost.startup + tlist_cost.per_tuple;

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
		MinMaxAggInfo *info;
		ListCell   *l;

		Assert(aggref->agglevelsup == 0);
		if (aggref->aggstar)
			return true;		/* foo(*) is surely not optimizable */
		/* note: we do not care if DISTINCT is mentioned ... */

		aggsortop = fetch_agg_sort_op(aggref->aggfnoid);
		if (!OidIsValid(aggsortop))
			return true;		/* not a MIN/MAX aggregate */

		/*
		 * Check whether it's already in the list, and add it if not.
		 */
		foreach(l, *context)
		{
			info = (MinMaxAggInfo *) lfirst(l);
			if (info->aggfnoid == aggref->aggfnoid &&
				equal(info->target, aggref->target))
				return false;
		}

		info = (MinMaxAggInfo *) palloc0(sizeof(MinMaxAggInfo));
		info->aggfnoid = aggref->aggfnoid;
		info->aggsortop = aggsortop;
		info->target = aggref->target;

		*context = lappend(*context, info);

		/*
		 * We need not recurse into the argument, since it can't contain
		 * any aggregates.
		 */
		return false;
	}
	Assert(!IsA(node, SubLink));
	return expression_tree_walker(node, find_minmax_aggs_walker,
								  (void *) context);
}

/*
 * build_minmax_path
 *		Given a MIN/MAX aggregate, try to find an index it can be optimized
 *		with.  Build a Path describing the best such index path.
 *
 * Returns TRUE if successful, FALSE if not.  In the TRUE case, info->path
 * is filled in.
 *
 * XXX look at sharing more code with indxpath.c.
 *
 * Note: check_partial_indexes() must have been run previously.
 */
static bool
build_minmax_path(Query *root, RelOptInfo *rel, MinMaxAggInfo *info)
{
	IndexPath  *best_path = NULL;
	Cost		best_cost = 0;
	ListCell   *l;

	foreach(l, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(l);
		ScanDirection indexscandir = NoMovementScanDirection;
		int			indexcol;
		int			prevcol;
		List	   *restrictclauses;
		IndexPath  *new_path;
		Cost		new_cost;

		/* Ignore non-btree indexes */
		if (index->relam != BTREE_AM_OID)
			continue;

		/* Ignore partial indexes that do not match the query */
		if (index->indpred != NIL && !index->predOK)
			continue;

		/*
		 * Look for a match to one of the index columns.  (In a stupidly
		 * designed index, there could be multiple matches, but we only
		 * care about the first one.)
		 */
		for (indexcol = 0; indexcol < index->ncolumns; indexcol++)
		{
			indexscandir = match_agg_to_index_col(info, index, indexcol);
			if (!ScanDirectionIsNoMovement(indexscandir))
				break;
		}
		if (ScanDirectionIsNoMovement(indexscandir))
			continue;

		/*
		 * If the match is not at the first index column, we have to verify
		 * that there are "x = something" restrictions on all the earlier
		 * index columns.  Since we'll need the restrictclauses list anyway
		 * to build the path, it's convenient to extract that first and then
		 * look through it for the equality restrictions.
		 */
		restrictclauses = group_clauses_by_indexkey(index);

		if (list_length(restrictclauses) < indexcol)
			continue;			/* definitely haven't got enough */
		for (prevcol = 0; prevcol < indexcol; prevcol++)
		{
			List   *rinfos = (List *) list_nth(restrictclauses, prevcol);
			ListCell *ll;

			foreach(ll, rinfos)
			{
				RestrictInfo *rinfo = (RestrictInfo *) lfirst(ll);
				int			strategy;

				Assert(is_opclause(rinfo->clause));
				strategy =
					get_op_opclass_strategy(((OpExpr *) rinfo->clause)->opno,
											index->classlist[prevcol]);
				if (strategy == BTEqualStrategyNumber)
					break;
			}
			if (ll == NULL)
				break;			/* none are Equal for this index col */
		}
		if (prevcol < indexcol)
			continue;			/* didn't find all Equal clauses */

		/*
		 * Build the access path.  We don't bother marking it with pathkeys.
		 */
		new_path = create_index_path(root, index,
									 restrictclauses,
									 NIL,
									 indexscandir);

		/*
		 * Estimate actual cost of fetching just one row.
		 */
		if (new_path->rows > 1.0)
			new_cost = new_path->path.startup_cost +
				(new_path->path.total_cost - new_path->path.startup_cost)
				* 1.0 / new_path->rows;
		else
			new_cost = new_path->path.total_cost;

		/*
		 * Keep if first or if cheaper than previous best.
		 */
		if (best_path == NULL || new_cost < best_cost)
		{
			best_path = new_path;
			best_cost = new_cost;
		}
	}

	info->path = best_path;
	info->pathcost = best_cost;
	return (best_path != NULL);
}

/*
 * match_agg_to_index_col
 *		Does an aggregate match an index column?
 *
 * It matches if its argument is equal to the index column's data and its
 * sortop is either the LessThan or GreaterThan member of the column's opclass.
 *
 * We return ForwardScanDirection if match the LessThan member,
 * BackwardScanDirection if match the GreaterThan member,
 * and NoMovementScanDirection if there's no match.
 */
static ScanDirection
match_agg_to_index_col(MinMaxAggInfo *info, IndexOptInfo *index, int indexcol)
{
	int			strategy;

	/* Check for data match */
	if (!match_index_to_operand((Node *) info->target, indexcol, index))
		return NoMovementScanDirection;

	/* Look up the operator in the opclass */
	strategy = get_op_opclass_strategy(info->aggsortop,
									   index->classlist[indexcol]);
	if (strategy == BTLessStrategyNumber)
		return ForwardScanDirection;
	if (strategy == BTGreaterStrategyNumber)
		return BackwardScanDirection;
	return NoMovementScanDirection;
}

/*
 * Construct a suitable plan for a converted aggregate query
 */
static void
make_agg_subplan(Query *root, MinMaxAggInfo *info, List *constant_quals)
{
	Query	   *subquery;
	Path	   *path;
	Plan	   *plan;
	TargetEntry *tle;
	SortClause *sortcl;

	/*
	 * Generate a suitably modified Query node.  Much of the work here is
	 * probably unnecessary in the normal case, but we want to make it look
	 * good if someone tries to EXPLAIN the result.
	 */
	subquery = (Query *) copyObject(root);
	subquery->commandType = CMD_SELECT;
	subquery->resultRelation = 0;
	subquery->resultRelations = NIL;
	subquery->into = NULL;
	subquery->hasAggs = false;
	subquery->groupClause = NIL;
	subquery->havingQual = NULL;
	subquery->hasHavingQual = false;
	subquery->distinctClause = NIL;

	/* single tlist entry that is the aggregate target */
	tle = makeTargetEntry(copyObject(info->target),
						  1,
						  pstrdup("agg_target"),
						  false);
	subquery->targetList = list_make1(tle);

	/* set up the appropriate ORDER BY entry */
	sortcl = makeNode(SortClause);
	sortcl->tleSortGroupRef = assignSortGroupRef(tle, subquery->targetList);
	sortcl->sortop = info->aggsortop;
	subquery->sortClause = list_make1(sortcl);

	/* set up LIMIT 1 */
	subquery->limitOffset = NULL;
	subquery->limitCount = (Node *) makeConst(INT4OID, sizeof(int4),
											  Int32GetDatum(1),
											  false, true);

	/*
	 * Generate the plan for the subquery.  We already have a Path for
	 * the basic indexscan, but we have to convert it to a Plan and
	 * attach a LIMIT node above it.  We might need a gating Result, too,
	 * which is most easily added at the Path stage.
	 */
	path = (Path *) info->path;

	if (constant_quals)
		path = (Path *) create_result_path(NULL,
										   path,
										   copyObject(constant_quals));

	plan = create_plan(subquery, path);

	plan->targetlist = copyObject(subquery->targetList);

	plan = (Plan *) make_limit(plan, 
							   subquery->limitOffset,
							   subquery->limitCount);

	/*
	 * Convert the plan into an InitPlan, and make a Param for its result.
	 */
	info->param = SS_make_initplan_from_plan(subquery, plan,
											 exprType((Node *) tle->expr),
											 -1);
}

/*
 * Replace original aggregate calls with subplan output Params
 */
static Node *
replace_aggs_with_params_mutator(Node *node,  List **context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Aggref))
	{
		Aggref	   *aggref = (Aggref *) node;
		ListCell   *l;

		foreach(l, *context)
		{
			MinMaxAggInfo *info = (MinMaxAggInfo *) lfirst(l);

			if (info->aggfnoid == aggref->aggfnoid &&
				equal(info->target, aggref->target))
				return (Node *) info->param;
		}
		elog(ERROR, "failed to re-find aggregate info record");
	}
	Assert(!IsA(node, SubLink));
	return expression_tree_mutator(node, replace_aggs_with_params_mutator,
								   (void *) context);
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
	aggTuple = SearchSysCache(AGGFNOID,
							  ObjectIdGetDatum(aggfnoid),
							  0, 0, 0);
	if (!HeapTupleIsValid(aggTuple))
		return InvalidOid;
	aggform = (Form_pg_aggregate) GETSTRUCT(aggTuple);
	aggsortop = aggform->aggsortop;
	ReleaseSysCache(aggTuple);

	return aggsortop;
}
