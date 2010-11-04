/*-------------------------------------------------------------------------
 *
 * planagg.c
 *	  Special planning for aggregate queries.
 *
 * This module tries to replace MIN/MAX aggregate functions by subqueries
 * of the form
 *		(SELECT col FROM tab WHERE ... ORDER BY col ASC/DESC LIMIT 1)
 * Given a suitable index on tab.col, this can be much faster than the
 * generic scan-all-the-rows aggregation plan.  We can handle multiple
 * MIN/MAX aggregates by generating multiple subqueries, and their
 * orderings can be different.  However, if the query contains any
 * non-optimizable aggregates, there's no point since we'll have to
 * scan all the rows anyway.
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/planagg.c
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
#include "optimizer/prep.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/subselect.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/* Per-aggregate info during optimize_minmax_aggregates() */
typedef struct
{
	MinMaxAggInfo *mminfo;		/* info gathered by preprocessing */
	Path	   *path;			/* access path for ordered scan */
	Cost		pathcost;		/* estimated cost to fetch first row */
	Param	   *param;			/* param for subplan's output */
} PrivateMMAggInfo;

static bool find_minmax_aggs_walker(Node *node, List **context);
static PrivateMMAggInfo *find_minmax_path(PlannerInfo *root, RelOptInfo *rel,
										  MinMaxAggInfo *mminfo);
static bool path_usable_for_agg(Path *path);
static void make_agg_subplan(PlannerInfo *root, RelOptInfo *rel,
							 PrivateMMAggInfo *info);
static void add_notnull_qual(PlannerInfo *root, RelOptInfo *rel,
							 PrivateMMAggInfo *info, Path *path);
static Node *replace_aggs_with_params_mutator(Node *node, List **context);
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
	 * so there's not much point in optimizing MIN/MAX.
	 */
	if (parse->groupClause || parse->hasWindowFuncs)
		return;

	/*
	 * We also restrict the query to reference exactly one table, since join
	 * conditions can't be handled reasonably.  (We could perhaps handle a
	 * query containing cartesian-product joins, but it hardly seems worth the
	 * trouble.)  However, the single real table could be buried in several
	 * levels of FromExpr due to subqueries.  Note the single table could be
	 * an inheritance parent, too.
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
	if (rte->rtekind != RTE_RELATION)
		return;

	/*
	 * Scan the tlist and HAVING qual to find all the aggregates and verify
	 * all are MIN/MAX aggregates.  Stop as soon as we find one that isn't.
	 */
	aggs_list = NIL;
	if (find_minmax_aggs_walker((Node *) tlist, &aggs_list))
		return;
	if (find_minmax_aggs_walker(parse->havingQual, &aggs_list))
		return;

	/*
	 * OK, there is at least the possibility of performing the optimization.
	 * Build pathkeys (and thereby EquivalenceClasses) for each aggregate.
	 * The existence of the EquivalenceClasses will prompt the path generation
	 * logic to try to build paths matching the desired sort ordering(s).
	 *
	 * Note: the pathkeys are non-canonical at this point.  They'll be fixed
	 * later by canonicalize_all_pathkeys().
	 */
	foreach(lc, aggs_list)
	{
		MinMaxAggInfo *mminfo = (MinMaxAggInfo *) lfirst(lc);

		mminfo->pathkeys = make_pathkeys_for_aggregate(root,
													   mminfo->target,
													   mminfo->aggsortop);
	}

	/*
	 * We're done until path generation is complete.  Save info for later.
	 */
	root->minmax_aggs = aggs_list;
}

/*
 * optimize_minmax_aggregates - check for optimizing MIN/MAX via indexes
 *
 * Check to see whether all the aggregates are in fact optimizable into
 * indexscans. If so, and the result is estimated to be cheaper than the
 * generic aggregate method, then generate and return a Plan that does it
 * that way.  Otherwise, return NULL.
 *
 * We are passed the preprocessed tlist, as well as the best path devised for
 * computing the input of a standard Agg node.
 */
Plan *
optimize_minmax_aggregates(PlannerInfo *root, List *tlist, Path *best_path)
{
	Query	   *parse = root->parse;
	FromExpr   *jtnode;
	RangeTblRef *rtr;
	RelOptInfo *rel;
	List	   *aggs_list;
	ListCell   *lc;
	Cost		total_cost;
	Path		agg_p;
	Plan	   *plan;
	Node	   *hqual;
	QualCost	tlist_cost;

	/* Nothing to do if preprocess_minmax_aggs rejected the query */
	if (root->minmax_aggs == NIL)
		return NULL;

	/* Re-locate the one real table identified by preprocess_minmax_aggs */
	jtnode = parse->jointree;
	while (IsA(jtnode, FromExpr))
	{
		Assert(list_length(jtnode->fromlist) == 1);
		jtnode = linitial(jtnode->fromlist);
	}
	Assert(IsA(jtnode, RangeTblRef));
	rtr = (RangeTblRef *) jtnode;
	rel = find_base_rel(root, rtr->rtindex);

	/*
	 * Examine each agg to see if we can find a suitable ordered path for it.
	 * Give up if any agg isn't indexable.
	 */
	aggs_list = NIL;
	total_cost = 0;
	foreach(lc, root->minmax_aggs)
	{
		MinMaxAggInfo *mminfo = (MinMaxAggInfo *) lfirst(lc);
		PrivateMMAggInfo *info;

		info = find_minmax_path(root, rel, mminfo);
		if (!info)
			return NULL;
		aggs_list = lappend(aggs_list, info);
		total_cost += info->pathcost;
	}

	/*
	 * Now we have enough info to compare costs against the generic aggregate
	 * implementation.
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
	 *
	 * First, generate a subplan and output Param node for each agg.
	 */
	foreach(lc, aggs_list)
	{
		make_agg_subplan(root, rel, (PrivateMMAggInfo *) lfirst(lc));
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
		mminfo->pathkeys = NIL;				/* don't compute pathkeys yet */

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
 * find_minmax_path
 *		Given a MIN/MAX aggregate, try to find an ordered Path it can be
 *		optimized with.
 *
 * If successful, build and return a PrivateMMAggInfo struct.  Otherwise,
 * return NULL.
 */
static PrivateMMAggInfo *
find_minmax_path(PlannerInfo *root, RelOptInfo *rel, MinMaxAggInfo *mminfo)
{
	PrivateMMAggInfo *info;
	Path	   *best_path = NULL;
	Cost		best_cost = 0;
	double		path_fraction;
	PathKey    *mmpathkey;
	ListCell   *lc;

	/*
	 * Punt if the aggregate's pathkey turned out to be redundant, ie its
	 * pathkeys list is now empty.  This would happen with something like
	 * "SELECT max(x) ... WHERE x = constant".  There's no need to try to
	 * optimize such a case, because if there is an index that would help,
	 * it should already have been used with the WHERE clause.
	 */
	if (mminfo->pathkeys == NIL)
		return NULL;

	/*
	 * Search the paths that were generated for the rel to see if there are
	 * any with the desired ordering.  There could be multiple such paths,
	 * in which case take the cheapest (as measured according to how fast it
	 * will be to fetch the first row).
	 *
	 * We can't use pathkeys_contained_in() to check the ordering, because we
	 * would like to match pathkeys regardless of the nulls_first setting.
	 * However, we know that MIN/MAX aggregates will have at most one item in
	 * their pathkeys, so it's not too complicated to match by brute force.
	 *
	 * Note: this test ignores the possible costs associated with skipping
	 * NULL tuples.  We assume that adding the not-null criterion to the
	 * indexqual doesn't really cost anything.
	 */
	if (rel->rows > 1.0)
		path_fraction = 1.0 / rel->rows;
	else
		path_fraction = 1.0;

	Assert(list_length(mminfo->pathkeys) == 1);
	mmpathkey = (PathKey *) linitial(mminfo->pathkeys);

	foreach(lc, rel->pathlist)
	{
		Path   *path = (Path *) lfirst(lc);
		PathKey *pathkey;
		Cost	path_cost;

		if (path->pathkeys == NIL)
			continue;				/* unordered path */
		pathkey = (PathKey *) linitial(path->pathkeys);

		if (mmpathkey->pk_eclass == pathkey->pk_eclass &&
			mmpathkey->pk_opfamily == pathkey->pk_opfamily &&
			mmpathkey->pk_strategy == pathkey->pk_strategy)
		{
			/*
			 * OK, it has the right ordering; is it acceptable otherwise?
			 * (We test in this order because the pathkey check is cheap.)
			 */
			if (path_usable_for_agg(path))
			{
				/*
				 * It'll work; but is it the cheapest?
				 *
				 * Note: cost calculation here should match
				 * compare_fractional_path_costs().
				 */
				path_cost = path->startup_cost +
					path_fraction * (path->total_cost - path->startup_cost);

				if (best_path == NULL || path_cost < best_cost)
				{
					best_path = path;
					best_cost = path_cost;
				}
			}
		}
	}

	/* Fail if no suitable path */
	if (best_path == NULL)
		return NULL;

	/* Construct private state for further processing */
	info = (PrivateMMAggInfo *) palloc(sizeof(PrivateMMAggInfo));
	info->mminfo = mminfo;
	info->path = best_path;
	info->pathcost = best_cost;
	info->param = NULL;			/* will be set later */

	return info;
}

/*
 * To be usable, a Path needs to be an IndexPath on a btree index, or be a
 * MergeAppendPath of such IndexPaths.  This restriction is mainly because
 * we need to be sure the index can handle an added NOT NULL constraint at
 * minimal additional cost.  If you wish to relax it, you'll need to improve
 * add_notnull_qual() too.
 */
static bool
path_usable_for_agg(Path *path)
{
	if (IsA(path, IndexPath))
	{
		IndexPath  *ipath = (IndexPath *) path;

		/* OK if it's a btree index */
		if (ipath->indexinfo->relam == BTREE_AM_OID)
			return true;
	}
	else if (IsA(path, MergeAppendPath))
	{
		MergeAppendPath *mpath = (MergeAppendPath *) path;
		ListCell   *lc;

		foreach(lc, mpath->subpaths)
		{
			if (!path_usable_for_agg((Path *) lfirst(lc)))
				return false;
		}
		return true;
	}
	return false;
}

/*
 * Construct a suitable plan for a converted aggregate query
 */
static void
make_agg_subplan(PlannerInfo *root, RelOptInfo *rel, PrivateMMAggInfo *info)
{
	PlannerInfo subroot;
	Query	   *subparse;
	Plan	   *plan;
	TargetEntry *tle;

	/*
	 * Generate a suitably modified query.	Much of the work here is probably
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
	subparse->sortClause = NIL;
	subroot.hasHavingQual = false;

	/* single tlist entry that is the aggregate target */
	tle = makeTargetEntry(copyObject(info->mminfo->target),
						  1,
						  pstrdup("agg_target"),
						  false);
	subparse->targetList = list_make1(tle);

	/* set up expressions for LIMIT 1 */
	subparse->limitOffset = NULL;
	subparse->limitCount = (Node *) makeConst(INT8OID, -1, sizeof(int64),
											  Int64GetDatum(1), false,
											  FLOAT8PASSBYVAL);

	/*
	 * Modify the ordered Path to add an indexed "target IS NOT NULL"
	 * condition to each scan.  We need this to ensure we don't return a NULL,
	 * which'd be contrary to the standard behavior of MIN/MAX.  We insist on
	 * it being indexed, else the Path might not be as cheap as we thought.
	 */
	add_notnull_qual(root, rel, info, info->path);

	/*
	 * Generate the plan for the subquery. We already have a Path, but we have
	 * to convert it to a Plan and attach a LIMIT node above it.
	 */
	plan = create_plan(&subroot, info->path);

	plan->targetlist = subparse->targetList;

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
 * Attach a suitable NOT NULL qual to the IndexPath, or each of the member
 * IndexPaths.  Note we assume we can modify the paths in-place.
 */
static void
add_notnull_qual(PlannerInfo *root, RelOptInfo *rel, PrivateMMAggInfo *info,
				 Path *path)
{
	if (IsA(path, IndexPath))
	{
		IndexPath  *ipath = (IndexPath *) path;
		Expr	   *target;
		NullTest   *ntest;
		RestrictInfo *rinfo;
		List	   *newquals;
		bool		found_clause;

		/*
		 * If we are looking at a child of the original rel, we have to adjust
		 * the agg target expression to match the child.
		 */
		if (ipath->path.parent != rel)
		{
			AppendRelInfo *appinfo = NULL;
			ListCell *lc;

			/* Search for the appropriate AppendRelInfo */
			foreach(lc, root->append_rel_list)
			{
				appinfo = (AppendRelInfo *) lfirst(lc);
				if (appinfo->parent_relid == rel->relid &&
					appinfo->child_relid == ipath->path.parent->relid)
					break;
				appinfo = NULL;
			}
			if (!appinfo)
				elog(ERROR, "failed to find AppendRelInfo for child rel");
			target = (Expr *)
				adjust_appendrel_attrs((Node *) info->mminfo->target,
									   appinfo);
		}
		else
		{
			/* Otherwise, just make a copy (may not be necessary) */
			target = copyObject(info->mminfo->target);
		}

		/* Build "target IS NOT NULL" expression */
		ntest = makeNode(NullTest);
		ntest->nulltesttype = IS_NOT_NULL;
		ntest->arg = target;
		/* we checked it wasn't a rowtype in find_minmax_aggs_walker */
		ntest->argisrow = false;

		/*
		 * We can skip adding the NOT NULL qual if it duplicates either an
		 * already-given index condition, or a clause of the index predicate.
		 */
		if (list_member(get_actual_clauses(ipath->indexquals), ntest) ||
			list_member(ipath->indexinfo->indpred, ntest))
			return;

		/* Wrap it in a RestrictInfo and prepend to existing indexquals */
		rinfo = make_restrictinfo((Expr *) ntest,
								  true,
								  false,
								  false,
								  NULL,
								  NULL);

		newquals = list_concat(list_make1(rinfo), ipath->indexquals);

		/*
		 * We can't just stick the IS NOT NULL at the front of the list,
		 * though.  It has to go in the right position corresponding to its
		 * index column, which might not be the first one.  Easiest way to fix
		 * this is to run the quals through group_clauses_by_indexkey again.
		 */
		newquals = group_clauses_by_indexkey(ipath->indexinfo,
											 newquals,
											 NIL,
											 NULL,
											 SAOP_FORBID,
											 &found_clause);

		newquals = flatten_clausegroups_list(newquals);

		/* Trouble if we lost any quals */
		if (list_length(newquals) != list_length(ipath->indexquals) + 1)
			elog(ERROR, "add_notnull_qual failed to add NOT NULL qual");

		/*
		 * And update the path's indexquals.  Note we don't bother adding
		 * to indexclauses, which is OK since this is like a generated
		 * index qual.
		 */
		ipath->indexquals = newquals;
	}
	else if (IsA(path, MergeAppendPath))
	{
		MergeAppendPath *mpath = (MergeAppendPath *) path;
		ListCell   *lc;

		foreach(lc, mpath->subpaths)
		{
			add_notnull_qual(root, rel, info, (Path *) lfirst(lc));
		}
	}
	else
	{
		/* shouldn't get here, because of path_usable_for_agg checks */
		elog(ERROR, "add_notnull_qual failed");
	}
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
			PrivateMMAggInfo *info = (PrivateMMAggInfo *) lfirst(l);

			if (info->mminfo->aggfnoid == aggref->aggfnoid &&
				equal(info->mminfo->target, curTarget->expr))
				return (Node *) info->param;
		}
		elog(ERROR, "failed to re-find PrivateMMAggInfo record");
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
