/*-------------------------------------------------------------------------
 *
 * planagg.c
 *	  Special planning for aggregate queries.
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/plan/planagg.c,v 1.53 2010/07/06 19:18:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/predtest.h"
#include "optimizer/subselect.h"
#include "parser/parse_clause.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


typedef struct
{
	Oid			aggfnoid;		/* pg_proc Oid of the aggregate */
	Oid			aggsortop;		/* Oid of its sort operator */
	Expr	   *target;			/* expression we are aggregating on */
	NullTest   *notnulltest;	/* expression for "target IS NOT NULL" */
	IndexPath  *path;			/* access path for index scan */
	Cost		pathcost;		/* estimated cost to fetch first row */
	bool		nulls_first;	/* null ordering direction matching index */
	Param	   *param;			/* param for subplan's output */
} MinMaxAggInfo;

static bool find_minmax_aggs_walker(Node *node, List **context);
static bool build_minmax_path(PlannerInfo *root, RelOptInfo *rel,
				  MinMaxAggInfo *info);
static ScanDirection match_agg_to_index_col(MinMaxAggInfo *info,
					   IndexOptInfo *index, int indexcol);
static void make_agg_subplan(PlannerInfo *root, MinMaxAggInfo *info);
static void attach_notnull_index_qual(MinMaxAggInfo *info, IndexScan *iplan);
static Node *replace_aggs_with_params_mutator(Node *node, List **context);
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
 * We are passed the preprocessed tlist, and the best path
 * devised for computing the input of a standard Agg node.  If we are able
 * to optimize all the aggregates, and the result is estimated to be cheaper
 * than the generic aggregate method, then generate and return a Plan that
 * does it that way.  Otherwise, return NULL.
 */
Plan *
optimize_minmax_aggregates(PlannerInfo *root, List *tlist, Path *best_path)
{
	Query	   *parse = root->parse;
	FromExpr   *jtnode;
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

	/* Nothing to do if query has no aggregates */
	if (!parse->hasAggs)
		return NULL;

	Assert(!parse->setOperations);		/* shouldn't get here if a setop */
	Assert(parse->rowMarks == NIL);		/* nor if FOR UPDATE */

	/*
	 * Reject unoptimizable cases.
	 *
	 * We don't handle GROUP BY or windowing, because our current
	 * implementations of grouping require looking at all the rows anyway, and
	 * so there's not much point in optimizing MIN/MAX.
	 */
	if (parse->groupClause || parse->hasWindowFuncs)
		return NULL;

	/*
	 * We also restrict the query to reference exactly one table, since join
	 * conditions can't be handled reasonably.  (We could perhaps handle a
	 * query containing cartesian-product joins, but it hardly seems worth the
	 * trouble.)  However, the single real table could be buried in several
	 * levels of FromExpr.
	 */
	jtnode = parse->jointree;
	while (IsA(jtnode, FromExpr))
	{
		if (list_length(jtnode->fromlist) != 1)
			return NULL;
		jtnode = linitial(jtnode->fromlist);
	}
	if (!IsA(jtnode, RangeTblRef))
		return NULL;
	rtr = (RangeTblRef *) jtnode;
	rte = planner_rt_fetch(rtr->rtindex, root);
	if (rte->rtekind != RTE_RELATION || rte->inh)
		return NULL;
	rel = find_base_rel(root, rtr->rtindex);

	/*
	 * Since this optimization is not applicable all that often, we want to
	 * fall out before doing very much work if possible.  Therefore we do the
	 * work in several passes.  The first pass scans the tlist and HAVING qual
	 * to find all the aggregates and verify that each of them is a MIN/MAX
	 * aggregate.  If that succeeds, the second pass looks at each aggregate
	 * to see if it is optimizable; if so we make an IndexPath describing how
	 * we would scan it.  (We do not try to optimize if only some aggs are
	 * optimizable, since that means we'll have to scan all the rows anyway.)
	 * If that succeeds, we have enough info to compare costs against the
	 * generic implementation. Only if that test passes do we build a Plan.
	 */

	/* Pass 1: find all the aggregates */
	aggs_list = NIL;
	if (find_minmax_aggs_walker((Node *) tlist, &aggs_list))
		return NULL;
	if (find_minmax_aggs_walker(parse->havingQual, &aggs_list))
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
	 * Note that we don't include evaluation cost of the tlist here; this is
	 * OK since it isn't included in best_path's cost either, and should be
	 * the same in either case.
	 */
	cost_agg(&agg_p, root, AGG_PLAIN, list_length(aggs_list),
			 0, 0,
			 best_path->startup_cost, best_path->total_cost,
			 best_path->parent->rows);

	if (total_cost > agg_p.total_cost)
		return NULL;			/* too expensive */

	/*
	 * OK, we are going to generate an optimized plan.
	 */

	/* Pass 3: generate subplans and output Param nodes */
	foreach(l, aggs_list)
	{
		make_agg_subplan(root, (MinMaxAggInfo *) lfirst(l));
	}

	/*
	 * Modify the targetlist and HAVING qual to reference subquery outputs
	 */
	tlist = (List *) replace_aggs_with_params_mutator((Node *) tlist,
													  &aggs_list);
	hqual = replace_aggs_with_params_mutator(parse->havingQual,
											 &aggs_list);

	/*
	 * We have to replace Aggrefs with Params in equivalence classes too, else
	 * ORDER BY or DISTINCT on an optimized aggregate will fail.
	 *
	 * Note: at some point it might become necessary to mutate other data
	 * structures too, such as the query's sortClause or distinctClause. Right
	 * now, those won't be examined after this point.
	 */
	mutate_eclass_expressions(root,
							  replace_aggs_with_params_mutator,
							  &aggs_list);

	/*
	 * Generate the output plan --- basically just a Result
	 */
	plan = (Plan *) make_result(root, tlist, hqual, NULL);

	/* Account for evaluation cost of the tlist (make_result did the rest) */
	cost_qual_eval(&tlist_cost, tlist, root);
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
		TargetEntry *curTarget;
		MinMaxAggInfo *info;
		ListCell   *l;

		Assert(aggref->agglevelsup == 0);
		if (list_length(aggref->args) != 1 || aggref->aggorder != NIL)
			return true;		/* it couldn't be MIN/MAX */
		/* note: we do not care if DISTINCT is mentioned ... */

		aggsortop = fetch_agg_sort_op(aggref->aggfnoid);
		if (!OidIsValid(aggsortop))
			return true;		/* not a MIN/MAX aggregate */

		/*
		 * Check whether it's already in the list, and add it if not.
		 */
		curTarget = (TargetEntry *) linitial(aggref->args);
		foreach(l, *context)
		{
			info = (MinMaxAggInfo *) lfirst(l);
			if (info->aggfnoid == aggref->aggfnoid &&
				equal(info->target, curTarget->expr))
				return false;
		}

		info = (MinMaxAggInfo *) palloc0(sizeof(MinMaxAggInfo));
		info->aggfnoid = aggref->aggfnoid;
		info->aggsortop = aggsortop;
		info->target = curTarget->expr;

		*context = lappend(*context, info);

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
build_minmax_path(PlannerInfo *root, RelOptInfo *rel, MinMaxAggInfo *info)
{
	IndexPath  *best_path = NULL;
	Cost		best_cost = 0;
	bool		best_nulls_first = false;
	NullTest   *ntest;
	List	   *allquals;
	ListCell   *l;

	/* Build "target IS NOT NULL" expression for use below */
	ntest = makeNode(NullTest);
	ntest->nulltesttype = IS_NOT_NULL;
	ntest->arg = copyObject(info->target);
	ntest->argisrow = type_is_rowtype(exprType((Node *) ntest->arg));
	if (ntest->argisrow)
		return false;			/* punt on composites */
	info->notnulltest = ntest;

	/*
	 * Build list of existing restriction clauses plus the notnull test. We
	 * cheat a bit by not bothering with a RestrictInfo node for the notnull
	 * test --- predicate_implied_by() won't care.
	 */
	allquals = list_concat(list_make1(ntest), rel->baserestrictinfo);

	foreach(l, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(l);
		ScanDirection indexscandir = NoMovementScanDirection;
		int			indexcol;
		int			prevcol;
		List	   *restrictclauses;
		IndexPath  *new_path;
		Cost		new_cost;
		bool		found_clause;

		/* Ignore non-btree indexes */
		if (index->relam != BTREE_AM_OID)
			continue;

		/*
		 * Ignore partial indexes that do not match the query --- unless their
		 * predicates can be proven from the baserestrict list plus the IS NOT
		 * NULL test.  In that case we can use them.
		 */
		if (index->indpred != NIL && !index->predOK &&
			!predicate_implied_by(index->indpred, allquals))
			continue;

		/*
		 * Look for a match to one of the index columns.  (In a stupidly
		 * designed index, there could be multiple matches, but we only care
		 * about the first one.)
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
		 * index columns.  Since we'll need the restrictclauses list anyway to
		 * build the path, it's convenient to extract that first and then look
		 * through it for the equality restrictions.
		 */
		restrictclauses = group_clauses_by_indexkey(index,
												index->rel->baserestrictinfo,
													NIL,
													NULL,
													SAOP_FORBID,
													&found_clause);

		if (list_length(restrictclauses) < indexcol)
			continue;			/* definitely haven't got enough */
		for (prevcol = 0; prevcol < indexcol; prevcol++)
		{
			List	   *rinfos = (List *) list_nth(restrictclauses, prevcol);
			ListCell   *ll;

			foreach(ll, rinfos)
			{
				RestrictInfo *rinfo = (RestrictInfo *) lfirst(ll);
				int			strategy;

				/* Could be an IS_NULL test, if so ignore */
				if (!is_opclause(rinfo->clause))
					continue;
				strategy =
					get_op_opfamily_strategy(((OpExpr *) rinfo->clause)->opno,
											 index->opfamily[prevcol]);
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
									 indexscandir,
									 NULL);

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
			if (ScanDirectionIsForward(indexscandir))
				best_nulls_first = index->nulls_first[indexcol];
			else
				best_nulls_first = !index->nulls_first[indexcol];
		}
	}

	info->path = best_path;
	info->pathcost = best_cost;
	info->nulls_first = best_nulls_first;
	return (best_path != NULL);
}

/*
 * match_agg_to_index_col
 *		Does an aggregate match an index column?
 *
 * It matches if its argument is equal to the index column's data and its
 * sortop is either the forward or reverse sort operator for the column.
 *
 * We return ForwardScanDirection if match the forward sort operator,
 * BackwardScanDirection if match the reverse sort operator,
 * and NoMovementScanDirection if there's no match.
 */
static ScanDirection
match_agg_to_index_col(MinMaxAggInfo *info, IndexOptInfo *index, int indexcol)
{
	ScanDirection result;

	/* Check for operator match first (cheaper) */
	if (info->aggsortop == index->fwdsortop[indexcol])
		result = ForwardScanDirection;
	else if (info->aggsortop == index->revsortop[indexcol])
		result = BackwardScanDirection;
	else
		return NoMovementScanDirection;

	/* Check for data match */
	if (!match_index_to_operand((Node *) info->target, indexcol, index))
		return NoMovementScanDirection;

	return result;
}

/*
 * Construct a suitable plan for a converted aggregate query
 */
static void
make_agg_subplan(PlannerInfo *root, MinMaxAggInfo *info)
{
	PlannerInfo subroot;
	Query	   *subparse;
	Plan	   *plan;
	IndexScan  *iplan;
	TargetEntry *tle;
	SortGroupClause *sortcl;

	/*
	 * Generate a suitably modified query.  Much of the work here is probably
	 * unnecessary in the normal case, but we want to make it look good if
	 * someone tries to EXPLAIN the result.
	 */
	memcpy(&subroot, root, sizeof(PlannerInfo));
	subroot.parse = subparse = (Query *) copyObject(root->parse);
	subparse->commandType = CMD_SELECT;
	subparse->resultRelation = 0;
	subparse->returningList = NIL;
	subparse->utilityStmt = NULL;
	subparse->intoClause = NULL;
	subparse->hasAggs = false;
	subparse->hasDistinctOn = false;
	subparse->groupClause = NIL;
	subparse->havingQual = NULL;
	subparse->distinctClause = NIL;
	subroot.hasHavingQual = false;

	/* single tlist entry that is the aggregate target */
	tle = makeTargetEntry(copyObject(info->target),
						  1,
						  pstrdup("agg_target"),
						  false);
	subparse->targetList = list_make1(tle);

	/* set up the appropriate ORDER BY entry */
	sortcl = makeNode(SortGroupClause);
	sortcl->tleSortGroupRef = assignSortGroupRef(tle, subparse->targetList);
	sortcl->eqop = get_equality_op_for_ordering_op(info->aggsortop, NULL);
	if (!OidIsValid(sortcl->eqop))		/* shouldn't happen */
		elog(ERROR, "could not find equality operator for ordering operator %u",
			 info->aggsortop);
	sortcl->sortop = info->aggsortop;
	sortcl->nulls_first = info->nulls_first;
	subparse->sortClause = list_make1(sortcl);

	/* set up LIMIT 1 */
	subparse->limitOffset = NULL;
	subparse->limitCount = (Node *) makeConst(INT8OID, -1, sizeof(int64),
											  Int64GetDatum(1), false,
											  FLOAT8PASSBYVAL);

	/*
	 * Generate the plan for the subquery.  We already have a Path for the
	 * basic indexscan, but we have to convert it to a Plan and attach a LIMIT
	 * node above it.
	 *
	 * Also we must add a "WHERE target IS NOT NULL" restriction to the
	 * indexscan, to be sure we don't return a NULL, which'd be contrary to
	 * the standard behavior of MIN/MAX.
	 *
	 * The NOT NULL qual has to go on the actual indexscan; create_plan might
	 * have stuck a gating Result atop that, if there were any pseudoconstant
	 * quals.
	 */
	plan = create_plan(&subroot, (Path *) info->path);

	plan->targetlist = copyObject(subparse->targetList);

	if (IsA(plan, Result))
		iplan = (IndexScan *) plan->lefttree;
	else
		iplan = (IndexScan *) plan;
	if (!IsA(iplan, IndexScan))
		elog(ERROR, "result of create_plan(IndexPath) isn't an IndexScan");

	attach_notnull_index_qual(info, iplan);

	plan = (Plan *) make_limit(plan,
							   subparse->limitOffset,
							   subparse->limitCount,
							   0, 1);

	/*
	 * Convert the plan into an InitPlan, and make a Param for its result.
	 */
	info->param = SS_make_initplan_from_plan(&subroot, plan,
											 exprType((Node *) tle->expr),
											 -1);

	/*
	 * Put the updated list of InitPlans back into the outer PlannerInfo.
	 */
	root->init_plans = subroot.init_plans;
}

/*
 * Add "target IS NOT NULL" to the quals of the given indexscan.
 *
 * This is trickier than it sounds because the new qual has to be added at an
 * appropriate place in the qual list, to preserve the list's ordering by
 * index column position.
 */
static void
attach_notnull_index_qual(MinMaxAggInfo *info, IndexScan *iplan)
{
	NullTest   *ntest;
	List	   *newindexqual;
	List	   *newindexqualorig;
	bool		done;
	ListCell   *lc1;
	ListCell   *lc2;
	Expr	   *leftop;
	AttrNumber	targetattno;

	/*
	 * We can skip adding the NOT NULL qual if it duplicates either an
	 * already-given WHERE condition, or a clause of the index predicate.
	 */
	if (list_member(iplan->indexqualorig, info->notnulltest) ||
		list_member(info->path->indexinfo->indpred, info->notnulltest))
		return;

	/* Need a "fixed" copy as well as the original */
	ntest = copyObject(info->notnulltest);
	ntest->arg = (Expr *) fix_indexqual_operand((Node *) ntest->arg,
												info->path->indexinfo);

	/* Identify the target index column from the "fixed" copy */
	leftop = ntest->arg;

	if (leftop && IsA(leftop, RelabelType))
		leftop = ((RelabelType *) leftop)->arg;

	Assert(leftop != NULL);

	if (!IsA(leftop, Var))
		elog(ERROR, "NullTest indexqual has wrong key");

	targetattno = ((Var *) leftop)->varattno;

	/*
	 * list.c doesn't expose a primitive to insert a list cell at an arbitrary
	 * position, so our strategy is to copy the lists and insert the null test
	 * when we reach an appropriate spot.
	 */
	newindexqual = newindexqualorig = NIL;
	done = false;

	forboth(lc1, iplan->indexqual, lc2, iplan->indexqualorig)
	{
		Expr	   *qual = (Expr *) lfirst(lc1);
		Expr	   *qualorig = (Expr *) lfirst(lc2);
		AttrNumber	varattno;

		/*
		 * Identify which index column this qual is for.  This code should
		 * match the qual disassembly code in ExecIndexBuildScanKeys.
		 */
		if (IsA(qual, OpExpr))
		{
			/* indexkey op expression */
			leftop = (Expr *) get_leftop(qual);

			if (leftop && IsA(leftop, RelabelType))
				leftop = ((RelabelType *) leftop)->arg;

			Assert(leftop != NULL);

			if (!IsA(leftop, Var))
				elog(ERROR, "indexqual doesn't have key on left side");

			varattno = ((Var *) leftop)->varattno;
		}
		else if (IsA(qual, RowCompareExpr))
		{
			/* (indexkey, indexkey, ...) op (expression, expression, ...) */
			RowCompareExpr *rc = (RowCompareExpr *) qual;

			/*
			 * Examine just the first column of the rowcompare, which is what
			 * determines its placement in the overall qual list.
			 */
			leftop = (Expr *) linitial(rc->largs);

			if (leftop && IsA(leftop, RelabelType))
				leftop = ((RelabelType *) leftop)->arg;

			Assert(leftop != NULL);

			if (!IsA(leftop, Var))
				elog(ERROR, "indexqual doesn't have key on left side");

			varattno = ((Var *) leftop)->varattno;
		}
		else if (IsA(qual, ScalarArrayOpExpr))
		{
			/* indexkey op ANY (array-expression) */
			ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) qual;

			leftop = (Expr *) linitial(saop->args);

			if (leftop && IsA(leftop, RelabelType))
				leftop = ((RelabelType *) leftop)->arg;

			Assert(leftop != NULL);

			if (!IsA(leftop, Var))
				elog(ERROR, "indexqual doesn't have key on left side");

			varattno = ((Var *) leftop)->varattno;
		}
		else if (IsA(qual, NullTest))
		{
			/* indexkey IS NULL or indexkey IS NOT NULL */
			NullTest   *ntest = (NullTest *) qual;

			leftop = ntest->arg;

			if (leftop && IsA(leftop, RelabelType))
				leftop = ((RelabelType *) leftop)->arg;

			Assert(leftop != NULL);

			if (!IsA(leftop, Var))
				elog(ERROR, "NullTest indexqual has wrong key");

			varattno = ((Var *) leftop)->varattno;
		}
		else
		{
			elog(ERROR, "unsupported indexqual type: %d",
				 (int) nodeTag(qual));
			varattno = 0;		/* keep compiler quiet */
		}

		/* Insert the null test at the first place it can legally go */
		if (!done && targetattno <= varattno)
		{
			newindexqual = lappend(newindexqual, ntest);
			newindexqualorig = lappend(newindexqualorig, info->notnulltest);
			done = true;
		}

		newindexqual = lappend(newindexqual, qual);
		newindexqualorig = lappend(newindexqualorig, qualorig);
	}

	/* Add the null test at the end if it must follow all existing quals */
	if (!done)
	{
		newindexqual = lappend(newindexqual, ntest);
		newindexqualorig = lappend(newindexqualorig, info->notnulltest);
	}

	iplan->indexqual = newindexqual;
	iplan->indexqualorig = newindexqualorig;
}

/*
 * Replace original aggregate calls with subplan output Params
 */
static Node *
replace_aggs_with_params_mutator(Node *node, List **context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Aggref))
	{
		Aggref	   *aggref = (Aggref *) node;
		TargetEntry *curTarget = (TargetEntry *) linitial(aggref->args);
		ListCell   *l;

		foreach(l, *context)
		{
			MinMaxAggInfo *info = (MinMaxAggInfo *) lfirst(l);

			if (info->aggfnoid == aggref->aggfnoid &&
				equal(info->target, curTarget->expr))
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
	aggTuple = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(aggfnoid));
	if (!HeapTupleIsValid(aggTuple))
		return InvalidOid;
	aggform = (Form_pg_aggregate) GETSTRUCT(aggTuple);
	aggsortop = aggform->aggsortop;
	ReleaseSysCache(aggTuple);

	return aggsortop;
}
