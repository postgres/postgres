/*-------------------------------------------------------------------------
 *
 * planmain.c
 *	  Routines to plan a single query
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/planmain.c,v 1.48 1999/12/09 05:58:52 tgl Exp $
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
 *	  tlist is the target list of the query (do NOT use root->targetList!)
 *	  qual is the qualification of the query (likewise!)
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
			  List *tlist,
			  List *qual)
{
	List	   *constant_qual = NIL;
	List	   *var_only_tlist;
	Plan	   *subplan;

	/*
	 * Note: union_planner should already have done constant folding
	 * in both the tlist and qual, so we don't do it again here
	 * (indeed, we may be getting a flattened var-only tlist anyway).
	 *
	 * Is there any value in re-folding the qual after canonicalize_qual?
	 */

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
	{
		tlist = (List *) SS_process_sublinks((Node *) tlist);
		qual = (List *) SS_process_sublinks((Node *) qual);
		if (root->groupClause != NIL)
		{
			/*
			 * Check for ungrouped variables passed to subplans.
			 * Note we do NOT do this for subplans in WHERE; it's legal
			 * there because WHERE is evaluated pre-GROUP.
			 */
			check_subplans_for_ungrouped_vars((Node *) tlist, root, tlist);
		}
	}

	/*
	 * If the query contains no relation references at all, it must be
	 * something like "SELECT 2+2;".  Build a trivial "Result" plan.
	 */
	if (root->rtable == NIL)
	{
		/* If it's not a select, it should have had a target relation... */
		if (root->commandType != CMD_SELECT)
			elog(ERROR, "Empty range table for non-SELECT query");

		root->query_pathkeys = NIL; /* signal unordered result */

		/* Make childless Result node to evaluate given tlist. */
		return (Plan *) make_result(tlist, (Node *) qual, (Plan *) NULL);
	}

	/*
	 * Pull out any non-variable qual clauses so these can be put in a
	 * toplevel "Result" node, where they will gate execution of the whole
	 * plan (the Result will not invoke its descendant plan unless the
	 * quals are true).  Note that any *really* non-variable quals will
	 * have been optimized away by eval_const_expressions().  What we're
	 * mostly interested in here is quals that depend only on outer-level
	 * vars, although if the qual reduces to "WHERE FALSE" this path will
	 * also be taken.
	 */
	qual = pull_constant_clauses(qual, &constant_qual);

	/*
	 * Create a target list that consists solely of (resdom var) target
	 * list entries, i.e., contains no arbitrary expressions.
	 *
	 * All subplan nodes will have "flat" (var-only) tlists.
	 *
	 * This implies that all expression evaluations are done at the root
	 * of the plan tree.  Once upon a time there was code to try to push
	 * expensive function calls down to lower plan nodes, but that's dead
	 * code and has been for a long time...
	 */
	var_only_tlist = flatten_tlist(tlist);

	/*
	 * Choose the best access path and build a plan for it.
	 */
	subplan = subplanner(root, var_only_tlist, qual);

	/*
	 * Build a result node to control the plan if we have constant quals.
	 */
	if (constant_qual)
	{
		/*
		 * The result node will also be responsible for evaluating
		 * the originally requested tlist.
		 */
		subplan = (Plan *) make_result(tlist,
									   (Node *) constant_qual,
									   subplan);
	}
	else
	{
		/*
		 * Replace the toplevel plan node's flattened target list with the
		 * targetlist given by my caller, so that expressions are evaluated.
		 */
		subplan->targetlist = tlist;
	}

	return subplan;

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
	add_missing_rels_to_query(root);

	set_joininfo_mergeable_hashable(root->base_rel_list);

	final_rel = make_one_rel(root, root->base_rel_list);

	if (! final_rel)
	{
		/*
		 * We expect to end up here for a trivial INSERT ... VALUES query
		 * (which will have a target relation, so it gets past query_planner's
		 * check for empty range table; but the target rel is unreferenced
		 * and not marked inJoinSet, so we find there is nothing to join).
		 * 
		 * It's also possible to get here if the query was rewritten by the
		 * rule processor (creating rangetable entries not marked inJoinSet)
		 * but the rules either did nothing or were simplified to nothing
		 * by constant-expression folding.  So, don't complain.
		 */
		root->query_pathkeys = NIL; /* signal unordered result */

		/* Make childless Result node to evaluate given tlist. */
		return (Plan *) make_result(flat_tlist, (Node *) qual, (Plan *) NULL);
	}

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

	/*
	 * Nothing for it but to sort the cheapestpath --- but we let the
	 * caller do that.  union_planner has to be able to add a sort node
	 * anyway, so no need for extra code here.  (Furthermore, the given
	 * pathkeys might involve something we can't compute here, such as
	 * an aggregate function...)
	 */
	root->query_pathkeys = final_rel->cheapestpath->pathkeys;
	return create_plan(final_rel->cheapestpath);
}
