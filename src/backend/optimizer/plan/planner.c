/*-------------------------------------------------------------------------
 *
 * planner.c
 *	  The query optimizer external interface.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/planner.c,v 1.89 2000/09/12 21:06:54 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "parser/parse_expr.h"
#include "utils/lsyscache.h"


static void preprocess_join_conditions(Query *parse, Node *jtnode);
static List *make_subplanTargetList(Query *parse, List *tlist,
					   AttrNumber **groupColIdx);
static Plan *make_groupplan(List *group_tlist, bool tuplePerGroup,
			   List *groupClause, AttrNumber *grpColIdx,
			   bool is_presorted, Plan *subplan);
static Plan *make_sortplan(List *tlist, Plan *plannode, List *sortcls);

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
	List	   *save_PlannerInitPlan;
	List	   *save_PlannerParamVar;
	int			save_PlannerPlanId;

	/*
	 * The planner can be called recursively (an example is when
	 * eval_const_expressions tries to simplify an SQL function).
	 * So, global state variables must be saved and restored.
	 *
	 * (Perhaps these should be moved into the Query structure instead?)
	 */
	save_PlannerQueryLevel = PlannerQueryLevel;
	save_PlannerInitPlan = PlannerInitPlan;
	save_PlannerParamVar = PlannerParamVar;
	save_PlannerPlanId = PlannerPlanId;

	/* Initialize state for subselects */
	PlannerQueryLevel = 1;
	PlannerInitPlan = NULL;
	PlannerParamVar = NULL;
	PlannerPlanId = 0;

	/* this should go away sometime soon */
	transformKeySetQuery(parse);

	/* primary planning entry point (may recurse for subplans) */
	result_plan = subquery_planner(parse, -1.0 /* default case */ );

	Assert(PlannerQueryLevel == 1);

	/* if top-level query had subqueries, do housekeeping for them */
	if (PlannerPlanId > 0)
	{
		(void) SS_finalize_plan(result_plan);
		result_plan->initPlan = PlannerInitPlan;
	}

	/* executor wants to know total number of Params used overall */
	result_plan->nParamExec = length(PlannerParamVar);

	/* final cleanup of the plan */
	set_plan_references(result_plan);

	/* restore state for outer planner, if any */
	PlannerQueryLevel = save_PlannerQueryLevel;
	PlannerInitPlan = save_PlannerInitPlan;
	PlannerParamVar = save_PlannerParamVar;
	PlannerPlanId = save_PlannerPlanId;

	return result_plan;
}


/*--------------------
 * subquery_planner
 *	  Invokes the planner on a subquery.  We recurse to here for each
 *	  sub-SELECT found in the query tree.
 *
 * parse is the querytree produced by the parser & rewriter.
 * tuple_fraction is the fraction of tuples we expect will be retrieved.
 * tuple_fraction is interpreted as explained for union_planner, below.
 *
 * Basically, this routine does the stuff that should only be done once
 * per Query object.  It then calls union_planner, which may be called
 * recursively on the same Query node in order to handle UNIONs and/or
 * inheritance.  subquery_planner is called recursively from subselect.c
 * to handle sub-Query nodes found within the query's expressions.
 *
 * prepunion.c uses an unholy combination of calling union_planner when
 * recursing on the primary Query node, or subquery_planner when recursing
 * on a UNION'd Query node that hasn't previously been seen by
 * subquery_planner.  That whole chunk of code needs rewritten from scratch.
 *
 * Returns a query plan.
 *--------------------
 */
Plan *
subquery_planner(Query *parse, double tuple_fraction)
{
	/*
	 * A HAVING clause without aggregates is equivalent to a WHERE clause
	 * (except it can only refer to grouped fields).  If there are no aggs
	 * anywhere in the query, then we don't want to create an Agg plan
	 * node, so merge the HAVING condition into WHERE.	(We used to
	 * consider this an error condition, but it seems to be legal SQL.)
	 */
	if (parse->havingQual != NULL && !parse->hasAggs)
	{
		if (parse->qual == NULL)
			parse->qual = parse->havingQual;
		else
			parse->qual = (Node *) make_andclause(lappend(lcons(parse->qual,
																NIL),
													 parse->havingQual));
		parse->havingQual = NULL;
	}

	/*
	 * Simplify constant expressions in targetlist and quals.
	 *
	 * Note that at this point the qual has not yet been converted to
	 * implicit-AND form, so we can apply eval_const_expressions directly.
	 * Also note that we need to do this before SS_process_sublinks,
	 * because that routine inserts bogus "Const" nodes.
	 */
	parse->targetList = (List *)
		eval_const_expressions((Node *) parse->targetList);
	parse->qual = eval_const_expressions(parse->qual);
	parse->havingQual = eval_const_expressions(parse->havingQual);

	/*
	 * Canonicalize the qual, and convert it to implicit-AND format.
	 *
	 * XXX Is there any value in re-applying eval_const_expressions after
	 * canonicalize_qual?
	 */
	parse->qual = (Node *) canonicalize_qual((Expr *) parse->qual, true);

#ifdef OPTIMIZER_DEBUG
	printf("After canonicalize_qual()\n");
	pprint(parse->qual);
#endif

	/*
	 * Ditto for the havingQual
	 */
	parse->havingQual = (Node *) canonicalize_qual((Expr *) parse->havingQual,
												   true);

	/* Expand SubLinks to SubPlans */
	if (parse->hasSubLinks)
	{
		parse->targetList = (List *)
			SS_process_sublinks((Node *) parse->targetList);
		parse->qual = SS_process_sublinks(parse->qual);
		parse->havingQual = SS_process_sublinks(parse->havingQual);

		if (parse->groupClause != NIL)
		{

			/*
			 * Check for ungrouped variables passed to subplans. Note we
			 * do NOT do this for subplans in WHERE; it's legal there
			 * because WHERE is evaluated pre-GROUP.
			 *
			 * An interesting fine point: if we reassigned a HAVING qual into
			 * WHERE above, then we will accept references to ungrouped
			 * vars from subplans in the HAVING qual.  This is not
			 * entirely consistent, but it doesn't seem particularly
			 * harmful...
			 */
			check_subplans_for_ungrouped_vars((Node *) parse->targetList,
											  parse);
			check_subplans_for_ungrouped_vars(parse->havingQual, parse);
		}
	}

	/* Replace uplevel vars with Param nodes */
	if (PlannerQueryLevel > 1)
	{
		parse->targetList = (List *)
			SS_replace_correlation_vars((Node *) parse->targetList);
		parse->qual = SS_replace_correlation_vars(parse->qual);
		parse->havingQual = SS_replace_correlation_vars(parse->havingQual);
	}

	/* Do all the above for each qual condition (ON clause) in the join tree */
	preprocess_join_conditions(parse, (Node *) parse->jointree);

	/* Do the main planning (potentially recursive) */

	return union_planner(parse, tuple_fraction);

	/*
	 * XXX should any more of union_planner's activity be moved here?
	 *
	 * That would take careful study of the interactions with prepunion.c,
	 * but I suspect it would pay off in simplicity and avoidance of
	 * wasted cycles.
	 */
}

/*
 * preprocess_join_conditions
 *		Recursively scan the query's jointree and do subquery_planner's
 *		qual preprocessing work on each ON condition found therein.
 */
static void
preprocess_join_conditions(Query *parse, Node *jtnode)
{
	if (jtnode == NULL)
		return;
	if (IsA(jtnode, List))
	{
		List	   *l;

		foreach(l, (List *) jtnode)
			preprocess_join_conditions(parse, lfirst(l));
	}
	else if (IsA(jtnode, RangeTblRef))
	{
		/* nothing to do here */
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		preprocess_join_conditions(parse, j->larg);
		preprocess_join_conditions(parse, j->rarg);

		/* Simplify constant expressions */
		j->quals = eval_const_expressions(j->quals);

		/* Canonicalize the qual, and convert it to implicit-AND format */
		j->quals = (Node *) canonicalize_qual((Expr *) j->quals, true);

		/* Expand SubLinks to SubPlans */
		if (parse->hasSubLinks)
		{
			j->quals = SS_process_sublinks(j->quals);
			/*
			 * ON conditions, like WHERE clauses, are evaluated pre-GROUP;
			 * so we allow ungrouped vars in them.
			 */
		}

		/* Replace uplevel vars with Param nodes */
		if (PlannerQueryLevel > 1)
			j->quals = SS_replace_correlation_vars(j->quals);
	}
	else
		elog(ERROR, "preprocess_join_conditions: unexpected node type %d",
			 nodeTag(jtnode));
}

/*--------------------
 * union_planner
 *	  Invokes the planner on union-type queries (both regular UNIONs and
 *	  appends produced by inheritance), recursing if necessary to get them
 *	  all, then processes normal plans.
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
Plan *
union_planner(Query *parse,
			  double tuple_fraction)
{
	List	   *tlist = parse->targetList;
	List	   *rangetable = parse->rtable;
	Plan	   *result_plan = (Plan *) NULL;
	AttrNumber *groupColIdx = NULL;
	List	   *current_pathkeys = NIL;
	List	   *group_pathkeys;
	List	   *sort_pathkeys;
	Index		rt_index;
	List	   *inheritors;

	if (parse->unionClause)
	{
		result_plan = plan_union_queries(parse);
		/* XXX do we need to do this? bjm 12/19/97 */
		tlist = preprocess_targetlist(tlist,
									  parse->commandType,
									  parse->resultRelation,
									  parse->rtable);

		/*
		 * We leave current_pathkeys NIL indicating we do not know sort
		 * order.  This is correct for the appended-together subplan
		 * results, even if the subplans themselves produced sorted results.
		 */

		/*
		 * Calculate pathkeys that represent grouping/ordering
		 * requirements
		 */
		group_pathkeys = make_pathkeys_for_sortclauses(parse->groupClause,
													   tlist);
		sort_pathkeys = make_pathkeys_for_sortclauses(parse->sortClause,
													  tlist);
	}
	else if (find_inheritable_rt_entry(rangetable,
									   &rt_index, &inheritors))
	{
		List	   *sub_tlist;

		/*
		 * Generate appropriate target list for subplan; may be different
		 * from tlist if grouping or aggregation is needed.
		 */
		sub_tlist = make_subplanTargetList(parse, tlist, &groupColIdx);

		/*
		 * Recursively plan the subqueries needed for inheritance
		 */
		result_plan = plan_inherit_queries(parse, sub_tlist,
										   rt_index, inheritors);

		/*
		 * Fix up outer target list.  NOTE: unlike the case for
		 * non-inherited query, we pass the unfixed tlist to subplans,
		 * which do their own fixing.  But we still want to fix the outer
		 * target list afterwards. I *think* this is correct --- doing the
		 * fix before recursing is definitely wrong, because
		 * preprocess_targetlist() will do the wrong thing if invoked
		 * twice on the same list. Maybe that is a bug? tgl 6/6/99
		 */
		tlist = preprocess_targetlist(tlist,
									  parse->commandType,
									  parse->resultRelation,
									  parse->rtable);

		if (parse->rowMark != NULL)
			elog(ERROR, "SELECT FOR UPDATE is not supported for inherit queries");

		/*
		 * We leave current_pathkeys NIL indicating we do not know sort
		 * order of the Append-ed results.
		 */

		/*
		 * Calculate pathkeys that represent grouping/ordering
		 * requirements
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
		 * Add row-mark targets for UPDATE (should this be done in
		 * preprocess_targetlist?)
		 */
		if (parse->rowMark != NULL)
		{
			List	   *l;

			foreach(l, parse->rowMark)
			{
				RowMark    *rowmark = (RowMark *) lfirst(l);
				TargetEntry *ctid;
				Resdom	   *resdom;
				Var		   *var;
				char	   *resname;

				if (!(rowmark->info & ROW_MARK_FOR_UPDATE))
					continue;

				resname = (char *) palloc(32);
				sprintf(resname, "ctid%u", rowmark->rti);
				resdom = makeResdom(length(tlist) + 1,
									TIDOID,
									-1,
									resname,
									true);

				var = makeVar(rowmark->rti, -1, TIDOID, -1, 0);

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
		 * the plan can generate, or to stop early due to a LIMIT or other
		 * factors.  If the caller passed a value >= 0, believe that
		 * value, else do our own examination of the query context.
		 */
		if (tuple_fraction < 0.0)
		{
			/* Initial assumption is we need all the tuples */
			tuple_fraction = 0.0;

			/*
			 * Check for a LIMIT clause.
			 */
			if (parse->limitCount != NULL)
			{
				if (IsA(parse->limitCount, Const))
				{
					Const	   *limitc = (Const *) parse->limitCount;
					int			count = (int) (limitc->constvalue);

					/*
					 * The constant can legally be either 0 ("ALL") or a
					 * positive integer.  If it is not ALL, we also need
					 * to consider the OFFSET part of LIMIT.
					 */
					if (count > 0)
					{
						tuple_fraction = (double) count;
						if (parse->limitOffset != NULL)
						{
							if (IsA(parse->limitOffset, Const))
							{
								int			offset;

								limitc = (Const *) parse->limitOffset;
								offset = (int) (limitc->constvalue);
								if (offset > 0)
									tuple_fraction += (double) offset;
							}
							else
							{
								/* It's a PARAM ... punt ... */
								tuple_fraction = 0.10;
							}
						}
					}
				}
				else
				{

					/*
					 * COUNT is a PARAM ... don't know exactly what the
					 * limit will be, but for lack of a better idea assume
					 * 10% of the plan's result is wanted.
					 */
					tuple_fraction = 0.10;
				}
			}

			/*
			 * Check for a retrieve-into-portal, ie DECLARE CURSOR.
			 *
			 * We have no real idea how many tuples the user will ultimately
			 * FETCH from a cursor, but it seems a good bet that he
			 * doesn't want 'em all.  Optimize for 10% retrieval (you
			 * gotta better number?)
			 */
			if (parse->isPortal)
				tuple_fraction = 0.10;
		}

		/*
		 * Adjust tuple_fraction if we see that we are going to apply
		 * grouping/aggregation/etc.  This is not overridable by the
		 * caller, since it reflects plan actions that this routine will
		 * certainly take, not assumptions about context.
		 */
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
			 * of GROUP BY.  (Although we are comparing non-canonicalized
			 * pathkeys here, it should be OK since they will both contain
			 * only single-element sublists at this point.	See
			 * pathkeys.c.)
			 */
			if (parse->groupClause && parse->sortClause &&
				!pathkeys_contained_in(sort_pathkeys, group_pathkeys))
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

		/* Generate the (sub) plan */
		result_plan = query_planner(parse,
									sub_tlist,
									tuple_fraction);

		/*
		 * query_planner returns actual sort order (which is not
		 * necessarily what we requested) in query_pathkeys.
		 */
		current_pathkeys = parse->query_pathkeys;
	}

	/* query_planner returns NULL if it thinks plan is bogus */
	if (!result_plan)
		elog(ERROR, "union_planner: failed to create plan");

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
		 * the same set of vars as the subplan did (but we can exclude any
		 * GROUP BY expressions).  If there are no aggregates then the
		 * Group node had better compute the final tlist.
		 */
		if (parse->hasAggs)
			group_tlist = flatten_tlist(result_plan->targetlist);
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

		result_plan = make_groupplan(group_tlist,
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

	/*
	 * If we were not able to make the plan come out in the right order,
	 * add an explicit sort step.
	 */
	if (parse->sortClause)
	{
		if (!pathkeys_contained_in(sort_pathkeys, current_pathkeys))
			result_plan = make_sortplan(tlist, result_plan,
										parse->sortClause);
	}

	/*
	 * Finally, if there is a DISTINCT clause, add the UNIQUE node.
	 */
	if (parse->distinctClause)
	{
		result_plan = (Plan *) make_unique(tlist, result_plan,
										   parse->distinctClause);
	}

	return result_plan;
}

/*---------------
 * make_subplanTargetList
 *	  Generate appropriate target list when grouping is required.
 *
 * When union_planner inserts Aggregate and/or Group plan nodes above
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
 * use only a+b, but it's not really worth the trouble.)
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
make_groupplan(List *group_tlist,
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
				resdom->reskeyop = get_opcode(grpcl->sortop);
			}
		}

		Assert(keyno > 0);

		subplan = (Plan *) make_sort(sort_tlist, subplan, keyno);
	}

	return (Plan *) make_group(group_tlist, tuplePerGroup, numCols,
							   grpColIdx, subplan);
}

/*
 * make_sortplan
 *	  Add a Sort node to implement an explicit ORDER BY clause.
 */
static Plan *
make_sortplan(List *tlist, Plan *plannode, List *sortcls)
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
			resdom->reskeyop = get_opcode(sortcl->sortop);
		}
	}

	Assert(keyno > 0);

	return (Plan *) make_sort(sort_tlist, plannode, keyno);
}
