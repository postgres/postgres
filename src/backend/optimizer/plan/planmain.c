/*-------------------------------------------------------------------------
 *
 * planmain.c
 *	  Routines to plan a single query
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/planmain.c,v 1.45 1999/09/26 02:28:27 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>

#include "postgres.h"


#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/prep.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "utils/lsyscache.h"


static Plan *subplanner(Query *root, List *flat_tlist, List *qual);

/*
 * query_planner
 *	  Routine to create a query plan.  It does so by first creating a
 *	  subplan for the topmost level of attributes in the query.  Then,
 *	  it modifies all target list and qualifications to consider the next
 *	  level of nesting and creates a plan for this modified query by
 *	  recursively calling itself.  The two pieces are then merged together
 *	  by creating a result node that indicates which attributes should
 *	  be placed where and any relation level qualifications to be
 *	  satisfied.
 *
 *	  command-type is the query command, e.g., select, delete, etc.
 *	  tlist is the target list of the query
 *	  qual is the qualification of the query
 *
 *	  Note: the Query node now also includes a query_pathkeys field, which
 *	  is both an input and an output of query_planner().  The input value
 *	  signals query_planner that the indicated sort order is wanted in the
 *	  final output plan.  The output value is the actual pathkeys of the
 *	  selected path.  This might not be the same as what the caller requested;
 *	  the caller must do pathkeys_contained_in() to decide whether an
 *	  explicit sort is still needed.  (The main reason query_pathkeys is a
 *	  Query field and not a passed parameter is that the low-level routines
 *	  in indxpath.c need to see it.)
 *
 *	  Returns a query plan.
 */
Plan *
query_planner(Query *root,
			  int command_type,
			  List *tlist,
			  List *qual)
{
	List	   *constant_qual = NIL;
	List	   *var_only_tlist;
	List	   *level_tlist;
	Plan	   *subplan;

	/*
	 * Simplify constant expressions in both targetlist and qual.
	 *
	 * Note that at this point the qual has not yet been converted to
	 * implicit-AND form, so we can apply eval_const_expressions directly.
	 * Also note that we need to do this before SS_process_sublinks,
	 * because that routine inserts bogus "Const" nodes.
	 */
	tlist = (List *) eval_const_expressions((Node *) tlist);
	qual = (List *) eval_const_expressions((Node *) qual);

	/*
	 * Canonicalize the qual, and convert it to implicit-AND format.
	 */
	qual = canonicalize_qual((Expr *) qual, true);
#ifdef OPTIMIZER_DEBUG
	printf("After canonicalize_qual()\n");
	pprint(qual);
#endif

	/* Replace uplevel vars with Param nodes */
	if (PlannerQueryLevel > 1)
	{
		tlist = (List *) SS_replace_correlation_vars((Node *) tlist);
		qual = (List *) SS_replace_correlation_vars((Node *) qual);
	}
	/* Expand SubLinks to SubPlans */
	if (root->hasSubLinks)
		qual = (List *) SS_process_sublinks((Node *) qual);

	/*
	 * Pull out any non-variable qualifications so these can be put in the
	 * topmost result node.  (Any *really* non-variable quals will probably
	 * have been optimized away by eval_const_expressions().  What we're
	 * looking for here is quals that depend only on outer-level vars...)
	 */
	qual = pull_constant_clauses(qual, &constant_qual);

	/*
	 * Create a target list that consists solely of (resdom var) target
	 * list entries, i.e., contains no arbitrary expressions.
	 */
	var_only_tlist = flatten_tlist(tlist);
	if (var_only_tlist)
		level_tlist = var_only_tlist;
	else
		/* from old code. the logic is beyond me. - ay 2/95 */
		level_tlist = tlist;

	/*
	 * A query may have a non-variable target list and a non-variable
	 * qualification only under certain conditions: - the query creates
	 * all-new tuples, or - the query is a replace (a scan must still be
	 * done in this case).
	 */
	if (var_only_tlist == NULL && qual == NULL)
	{
		root->query_pathkeys = NIL; /* these plans make unordered results */

		switch (command_type)
		{
			case CMD_SELECT:
			case CMD_INSERT:
				return ((Plan *) make_result(tlist,
											 (Node *) constant_qual,
											 (Plan *) NULL));
				break;
			case CMD_DELETE:
			case CMD_UPDATE:
				{
					SeqScan    *scan = make_seqscan(tlist,
													NIL,
													root->resultRelation);

					if (constant_qual != NULL)
						return ((Plan *) make_result(tlist,
													 (Node *) constant_qual,
													 (Plan *) scan));
					else
						return (Plan *) scan;
				}
				break;
			default:
				return (Plan *) NULL;
		}
	}

	/*
	 * Choose the best access path and build a plan for it.
	 */
	subplan = subplanner(root, level_tlist, qual);

	/*
	 * Build a result node linking the plan if we have constant quals
	 */
	if (constant_qual)
	{
		subplan = (Plan *) make_result(tlist,
									   (Node *) constant_qual,
									   subplan);

		root->query_pathkeys = NIL; /* result is unordered, no? */

		return subplan;
	}

	/*
	 * Replace the toplevel plan node's flattened target list with the
	 * targetlist given by my caller, so that expressions are evaluated.
	 *
	 * This implies that all expression evaluations are done at the root
	 * of the plan tree.  Once upon a time there was code to try to push
	 * expensive function calls down to lower plan nodes, but that's dead
	 * code and has been for a long time...
	 */
	else
	{
		subplan->targetlist = tlist;

		return subplan;
	}

#ifdef NOT_USED

	/*
	 * Destructively modify the query plan's targetlist to add fjoin lists
	 * to flatten functions that return sets of base types
	 */
	subplan->targetlist = generate_fjoin(subplan->targetlist);
#endif

}

/*
 * subplanner
 *
 *	 Subplanner creates an entire plan consisting of joins and scans
 *	 for processing a single level of attributes.
 *
 *	 flat_tlist is the flattened target list
 *	 qual is the qualification to be satisfied
 *
 *	 Returns a subplan.
 *
 */
static Plan *
subplanner(Query *root,
		   List *flat_tlist,
		   List *qual)
{
	RelOptInfo *final_rel;
	Cost		cheapest_cost;
	Path	   *sortedpath;

	/*
	 * Initialize the targetlist and qualification, adding entries to
	 * base_rel_list as relation references are found (e.g., in the
	 * qualification, the targetlist, etc.)
	 */
	root->base_rel_list = NIL;
	root->join_rel_list = NIL;

	make_var_only_tlist(root, flat_tlist);
	add_restrict_and_join_to_rels(root, qual);
	add_missing_vars_to_tlist(root, flat_tlist);

	set_joininfo_mergeable_hashable(root->base_rel_list);

	final_rel = make_one_rel(root, root->base_rel_list);

#ifdef NOT_USED					/* fix xfunc */

	/*
	 * Perform Predicate Migration on each path, to optimize and correctly
	 * assess the cost of each before choosing the cheapest one. -- JMH,
	 * 11/16/92
	 *
	 * Needn't do so if the top rel is pruneable: that means there's no
	 * expensive functions left to pull up.  -- JMH, 11/22/92
	 */
	if (XfuncMode != XFUNC_OFF && XfuncMode != XFUNC_NOPM &&
		XfuncMode != XFUNC_NOPULL && !final_rel->pruneable)
	{
		List	   *pathnode;

		foreach(pathnode, final_rel->pathlist)
		{
			if (xfunc_do_predmig((Path *) lfirst(pathnode)))
				set_cheapest(final_rel, final_rel->pathlist);
		}
	}
#endif

	if (! final_rel)
	{
		elog(NOTICE, "final relation is null");
		root->query_pathkeys = NIL; /* result is unordered, no? */
		return create_plan((Path *) NULL);
	}

	/*
	 * Determine the cheapest path and create a subplan to execute it.
	 *
	 * If no special sort order is wanted, or if the cheapest path is
	 * already appropriately ordered, just use the cheapest path.
	 */
	if (root->query_pathkeys == NIL ||
		pathkeys_contained_in(root->query_pathkeys,
							  final_rel->cheapestpath->pathkeys))
	{
		root->query_pathkeys = final_rel->cheapestpath->pathkeys;
		return create_plan(final_rel->cheapestpath);
	}

	/*
	 * Otherwise, look to see if we have an already-ordered path that is
	 * cheaper than doing an explicit sort on cheapestpath.
	 */
	cheapest_cost = final_rel->cheapestpath->path_cost +
		cost_sort(root->query_pathkeys, final_rel->size, final_rel->width);

	sortedpath = get_cheapest_path_for_pathkeys(final_rel->pathlist,
												root->query_pathkeys,
												false);
	if (sortedpath)
	{
		if (sortedpath->path_cost <= cheapest_cost)
		{
			/* Found a better presorted path, use it */
			root->query_pathkeys = sortedpath->pathkeys;
			return create_plan(sortedpath);
		}
		/* otherwise, doing it the hard way is still cheaper */
	}
	else
	{
		/*
		 * If we found no usable presorted path at all, it is possible
		 * that the user asked for descending sort order.  Check to see
		 * if we can satisfy the pathkeys by using a backwards indexscan.
		 * To do this, we commute all the operators in the pathkeys and
		 * then look for a matching path that is an IndexPath.
		 */
		List	   *commuted_pathkeys = copyObject(root->query_pathkeys);

		if (commute_pathkeys(commuted_pathkeys))
		{
			/* pass 'true' to force only IndexPaths to be considered */
			sortedpath = get_cheapest_path_for_pathkeys(final_rel->pathlist,
														commuted_pathkeys,
														true);
			if (sortedpath && sortedpath->path_cost <= cheapest_cost)
			{
				/*
				 * Kluge here: since IndexPath has no representation for
				 * backwards scan, we have to convert to Plan format and
				 * then poke the result.
				 */
				Plan	   *sortedplan = create_plan(sortedpath);
				List	   *sortedpathkeys;

				Assert(IsA(sortedplan, IndexScan));
				((IndexScan *) sortedplan)->indxorderdir = BackwardScanDirection;
				/*
				 * Need to generate commuted keys representing the actual
				 * sort order.  This should succeed, probably, but just in
				 * case it does not, use the original root->query_pathkeys
				 * as a conservative approximation.
				 */
				sortedpathkeys = copyObject(sortedpath->pathkeys);
				if (commute_pathkeys(sortedpathkeys))
					root->query_pathkeys = sortedpathkeys;

				return sortedplan;
			}
		}
	}

	/* Nothing for it but to sort the cheapestpath --- but we let the
	 * caller do that.  union_planner has to be able to add a sort node
	 * anyway, so no need for extra code here.  (Furthermore, the given
	 * pathkeys might involve something we can't compute yet, such as
	 * an aggregate function...)
	 */
	root->query_pathkeys = final_rel->cheapestpath->pathkeys;
	return create_plan(final_rel->cheapestpath);
}
