/*-------------------------------------------------------------------------
 *
 * planmain.c
 *	  Routines to plan a single query
 *
 * What's in a name, anyway?  The top-level entry point of the planner/
 * optimizer is over in planner.c, not here as you might think from the
 * file name.  But this is the main code for planning a basic join operation,
 * shorn of features like subselects, inheritance, aggregates, grouping,
 * and so on.  (Those are the things planner.c deals with.)
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/planmain.c,v 1.54 2000/03/24 21:40:43 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/types.h>

#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/tlist.h"


static Plan *subplanner(Query *root, List *flat_tlist, List *qual,
						double tuple_fraction);


/*--------------------
 * query_planner
 *	  Generate a plan for a basic query, which may involve joins but
 *	  not any fancier features.
 *
 * tlist is the target list the query should produce (NOT root->targetList!)
 * qual is the qualification of the query (likewise!)
 * tuple_fraction is the fraction of tuples we expect will be retrieved
 *
 * qual must already have been converted to implicit-AND form.
 *
 * Note: the Query node now also includes a query_pathkeys field, which
 * is both an input and an output of query_planner().  The input value
 * signals query_planner that the indicated sort order is wanted in the
 * final output plan.  The output value is the actual pathkeys of the
 * selected path.  This might not be the same as what the caller requested;
 * the caller must do pathkeys_contained_in() to decide whether an
 * explicit sort is still needed.  (The main reason query_pathkeys is a
 * Query field and not a passed parameter is that the low-level routines
 * in indxpath.c need to see it.)  The pathkeys value passed to query_planner
 * has not yet been "canonicalized", since the necessary info does not get
 * computed until subplanner() scans the qual clauses.  We canonicalize it
 * inside subplanner() as soon as that task is done.  The output value
 * will be in canonical form as well.
 *
 * tuple_fraction is interpreted as follows:
 *    0 (or less): expect all tuples to be retrieved (normal case)
 *	  0 < tuple_fraction < 1: expect the given fraction of tuples available
 *		from the plan to be retrieved
 *	  tuple_fraction >= 1: tuple_fraction is the absolute number of tuples
 *		expected to be retrieved (ie, a LIMIT specification)
 * Note that while this routine and its subroutines treat a negative
 * tuple_fraction the same as 0, union_planner has a different interpretation.
 *
 * Returns a query plan.
 *--------------------
 */
Plan *
query_planner(Query *root,
			  List *tlist,
			  List *qual,
			  double tuple_fraction)
{
	List	   *constant_qual = NIL;
	List	   *var_only_tlist;
	Plan	   *subplan;

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
	subplan = subplanner(root, var_only_tlist, qual, tuple_fraction);

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
}

/*
 * subplanner
 *
 *	 Subplanner creates an entire plan consisting of joins and scans
 *	 for processing a single level of attributes.
 *
 * flat_tlist is the flattened target list
 * qual is the qualification to be satisfied
 * tuple_fraction is the fraction of tuples we expect will be retrieved
 *
 * See query_planner() comments about the interpretation of tuple_fraction.
 *
 * Returns a subplan.
 */
static Plan *
subplanner(Query *root,
		   List *flat_tlist,
		   List *qual,
		   double tuple_fraction)
{
	RelOptInfo *final_rel;
	Path	   *cheapestpath;
	Path	   *presortedpath;

	/*
	 * Initialize the targetlist and qualification, adding entries to
	 * base_rel_list as relation references are found (e.g., in the
	 * qualification, the targetlist, etc.).  Restrict and join clauses
	 * are added to appropriate lists belonging to the mentioned relations,
	 * and we also build lists of equijoined keys for pathkey construction.
	 */
	root->base_rel_list = NIL;
	root->join_rel_list = NIL;
	root->equi_key_list = NIL;

	make_var_only_tlist(root, flat_tlist);
	add_restrict_and_join_to_rels(root, qual);
	add_missing_rels_to_query(root);

	/*
	 * We should now have all the pathkey equivalence sets built,
	 * so it's now possible to convert the requested query_pathkeys
	 * to canonical form.
	 */
	root->query_pathkeys = canonicalize_pathkeys(root, root->query_pathkeys);

	/*
	 * Ready to do the primary planning.
	 */
	final_rel = make_one_rel(root);

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
				set_cheapest(final_rel);
		}
	}
#endif

	/*
	 * Now that we have an estimate of the final rel's size, we can convert
	 * a tuple_fraction specified as an absolute count (ie, a LIMIT option)
	 * into a fraction of the total tuples.
	 */
	if (tuple_fraction >= 1.0)
		tuple_fraction /= final_rel->rows;

	/*
	 * Determine the cheapest path, independently of any ordering
	 * considerations.  We do, however, take into account whether the
	 * whole plan is expected to be evaluated or not.
	 */
	if (tuple_fraction <= 0.0 || tuple_fraction >= 1.0)
		cheapestpath = final_rel->cheapest_total_path;
	else
		cheapestpath =
			get_cheapest_fractional_path_for_pathkeys(final_rel->pathlist,
													  NIL,
													  tuple_fraction);

	Assert(cheapestpath != NULL);

	/*
	 * Select the best path and create a subplan to execute it.
	 *
	 * If no special sort order is wanted, or if the cheapest path is
	 * already appropriately ordered, we use the cheapest path found above.
	 */
	if (root->query_pathkeys == NIL ||
		pathkeys_contained_in(root->query_pathkeys,
							  cheapestpath->pathkeys))
	{
		root->query_pathkeys = cheapestpath->pathkeys;
		return create_plan(root, cheapestpath);
	}

	/*
	 * Otherwise, look to see if we have an already-ordered path that is
	 * cheaper than doing an explicit sort on the cheapest-total-cost path.
	 */
	cheapestpath = final_rel->cheapest_total_path;
	presortedpath =
		get_cheapest_fractional_path_for_pathkeys(final_rel->pathlist,
												  root->query_pathkeys,
												  tuple_fraction);
	if (presortedpath)
	{
		Path		sort_path;	/* dummy for result of cost_sort */

		cost_sort(&sort_path, root->query_pathkeys,
				  final_rel->rows, final_rel->width);
		sort_path.startup_cost += cheapestpath->total_cost;
		sort_path.total_cost += cheapestpath->total_cost;
		if (compare_fractional_path_costs(presortedpath, &sort_path,
										  tuple_fraction) <= 0)
		{
			/* Presorted path is cheaper, use it */
			root->query_pathkeys = presortedpath->pathkeys;
			return create_plan(root, presortedpath);
		}
		/* otherwise, doing it the hard way is still cheaper */
	}

	/*
	 * Nothing for it but to sort the cheapest-total-cost path --- but we let
	 * the caller do that.  union_planner has to be able to add a sort node
	 * anyway, so no need for extra code here.  (Furthermore, the given
	 * pathkeys might involve something we can't compute here, such as an
	 * aggregate function...)
	 */
	root->query_pathkeys = cheapestpath->pathkeys;
	return create_plan(root, cheapestpath);
}
