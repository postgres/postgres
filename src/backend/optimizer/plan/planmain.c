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
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/planmain.c,v 1.64 2001/03/22 03:59:37 momjian Exp $
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
#include "parser/parsetree.h"
#include "utils/memutils.h"


static Plan *subplanner(Query *root, List *flat_tlist,
		   double tuple_fraction);


/*--------------------
 * query_planner
 *	  Generate a plan for a basic query, which may involve joins but
 *	  not any fancier features.
 *
 * tlist is the target list the query should produce (NOT root->targetList!)
 * tuple_fraction is the fraction of tuples we expect will be retrieved
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
 * computed until subplanner() scans the qual clauses.	We canonicalize it
 * inside subplanner() as soon as that task is done.  The output value
 * will be in canonical form as well.
 *
 * tuple_fraction is interpreted as follows:
 *	  0 (or less): expect all tuples to be retrieved (normal case)
 *	  0 < tuple_fraction < 1: expect the given fraction of tuples available
 *		from the plan to be retrieved
 *	  tuple_fraction >= 1: tuple_fraction is the absolute number of tuples
 *		expected to be retrieved (ie, a LIMIT specification)
 * Note that while this routine and its subroutines treat a negative
 * tuple_fraction the same as 0, grouping_planner has a different
 * interpretation.
 *
 * Returns a query plan.
 *--------------------
 */
Plan *
query_planner(Query *root,
			  List *tlist,
			  double tuple_fraction)
{
	List	   *constant_quals;
	List	   *var_only_tlist;
	Plan	   *subplan;

	/*
	 * If the query has an empty join tree, then it's something easy like
	 * "SELECT 2+2;" or "INSERT ... VALUES()".	Fall through quickly.
	 */
	if (root->jointree->fromlist == NIL)
	{
		root->query_pathkeys = NIL;		/* signal unordered result */

		/* Make childless Result node to evaluate given tlist. */
		return (Plan *) make_result(tlist, root->jointree->quals,
									(Plan *) NULL);
	}

	/*
	 * Pull out any non-variable WHERE clauses so these can be put in a
	 * toplevel "Result" node, where they will gate execution of the whole
	 * plan (the Result will not invoke its descendant plan unless the
	 * quals are true).  Note that any *really* non-variable quals will
	 * have been optimized away by eval_const_expressions().  What we're
	 * mostly interested in here is quals that depend only on outer-level
	 * vars, although if the qual reduces to "WHERE FALSE" this path will
	 * also be taken.
	 */
	root->jointree->quals = (Node *)
		pull_constant_clauses((List *) root->jointree->quals,
							  &constant_quals);

	/*
	 * Create a target list that consists solely of (resdom var) target
	 * list entries, i.e., contains no arbitrary expressions.
	 *
	 * All subplan nodes will have "flat" (var-only) tlists.
	 *
	 * This implies that all expression evaluations are done at the root of
	 * the plan tree.  Once upon a time there was code to try to push
	 * expensive function calls down to lower plan nodes, but that's dead
	 * code and has been for a long time...
	 */
	var_only_tlist = flatten_tlist(tlist);

	/*
	 * Choose the best access path and build a plan for it.
	 */
	subplan = subplanner(root, var_only_tlist, tuple_fraction);

	/*
	 * Build a result node to control the plan if we have constant quals,
	 * or if the top-level plan node is one that cannot do expression
	 * evaluation (it won't be able to evaluate the requested tlist).
	 * Currently, the only plan node we might see here that falls into
	 * that category is Append.
	 *
	 * XXX future improvement: if the given tlist is flat anyway, we don't
	 * really need a Result node.
	 */
	if (constant_quals || IsA(subplan, Append))
	{

		/*
		 * The result node will also be responsible for evaluating the
		 * originally requested tlist.
		 */
		subplan = (Plan *) make_result(tlist,
									   (Node *) constant_quals,
									   subplan);
	}
	else
	{

		/*
		 * Replace the toplevel plan node's flattened target list with the
		 * targetlist given by my caller, so that expressions are
		 * evaluated.
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
 * tuple_fraction is the fraction of tuples we expect will be retrieved
 *
 * See query_planner() comments about the interpretation of tuple_fraction.
 *
 * Returns a subplan.
 */
static Plan *
subplanner(Query *root,
		   List *flat_tlist,
		   double tuple_fraction)
{
	List	   *joined_rels;
	List	   *brel;
	RelOptInfo *final_rel;
	Plan	   *resultplan;
	Path	   *cheapestpath;
	Path	   *presortedpath;

	/*
	 * Examine the targetlist and qualifications, adding entries to
	 * base_rel_list as relation references are found (e.g., in the
	 * qualification, the targetlist, etc.).  Restrict and join clauses
	 * are added to appropriate lists belonging to the mentioned
	 * relations.  We also build lists of equijoined keys for pathkey
	 * construction.
	 */
	root->base_rel_list = NIL;
	root->join_rel_list = NIL;
	root->equi_key_list = NIL;

	build_base_rel_tlists(root, flat_tlist);

	(void) distribute_quals_to_rels(root, (Node *) root->jointree);

	/*
	 * Make sure we have RelOptInfo nodes for all relations to be joined.
	 */
	joined_rels = add_missing_rels_to_query(root, (Node *) root->jointree);

	/*
	 * Check that the join tree includes all the base relations used in
	 * the query --- otherwise, the parser or rewriter messed up.
	 */
	foreach(brel, root->base_rel_list)
	{
		RelOptInfo *baserel = (RelOptInfo *) lfirst(brel);
		int			relid = lfirsti(baserel->relids);

		if (!ptrMember(baserel, joined_rels))
			elog(ERROR, "Internal error: no jointree entry for rel %s (%d)",
				 rt_fetch(relid, root->rtable)->eref->relname, relid);
	}

	/*
	 * Use the completed lists of equijoined keys to deduce any implied
	 * but unstated equalities (for example, A=B and B=C imply A=C).
	 */
	generate_implied_equalities(root);

	/*
	 * We should now have all the pathkey equivalence sets built, so it's
	 * now possible to convert the requested query_pathkeys to canonical
	 * form.
	 */
	root->query_pathkeys = canonicalize_pathkeys(root, root->query_pathkeys);

	/*
	 * Ready to do the primary planning.
	 */
	final_rel = make_one_rel(root);

	if (!final_rel)
		elog(ERROR, "subplanner: failed to construct a relation");

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
	 * Now that we have an estimate of the final rel's size, we can
	 * convert a tuple_fraction specified as an absolute count (ie, a
	 * LIMIT option) into a fraction of the total tuples.
	 */
	if (tuple_fraction >= 1.0)
		tuple_fraction /= final_rel->rows;

	/*
	 * Determine the cheapest path, independently of any ordering
	 * considerations.	We do, however, take into account whether the
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
	 * If no special sort order is wanted, or if the cheapest path is already
	 * appropriately ordered, we use the cheapest path found above.
	 */
	if (root->query_pathkeys == NIL ||
		pathkeys_contained_in(root->query_pathkeys,
							  cheapestpath->pathkeys))
	{
		root->query_pathkeys = cheapestpath->pathkeys;
		resultplan = create_plan(root, cheapestpath);
		goto plan_built;
	}

	/*
	 * Otherwise, look to see if we have an already-ordered path that is
	 * cheaper than doing an explicit sort on the cheapest-total-cost
	 * path.
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
			resultplan = create_plan(root, presortedpath);
			goto plan_built;
		}
		/* otherwise, doing it the hard way is still cheaper */
	}

	/*
	 * Nothing for it but to sort the cheapest-total-cost path --- but we
	 * let the caller do that.	grouping_planner has to be able to add a
	 * sort node anyway, so no need for extra code here.  (Furthermore,
	 * the given pathkeys might involve something we can't compute here,
	 * such as an aggregate function...)
	 */
	root->query_pathkeys = cheapestpath->pathkeys;
	resultplan = create_plan(root, cheapestpath);

plan_built:

	return resultplan;
}
