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
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/planmain.c,v 1.78.4.1 2005/09/28 21:17:49 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"


/*--------------------
 * query_planner
 *	  Generate a path (that is, a simplified plan) for a basic query,
 *	  which may involve joins but not any fancier features.
 *
 * Since query_planner does not handle the toplevel processing (grouping,
 * sorting, etc) it cannot select the best path by itself.	It selects
 * two paths: the cheapest path that produces all the required tuples,
 * independent of any ordering considerations, and the cheapest path that
 * produces the expected fraction of the required tuples in the required
 * ordering, if there is a path that is cheaper for this than just sorting
 * the output of the cheapest overall path.  The caller (grouping_planner)
 * will make the final decision about which to use.
 *
 * Input parameters:
 * root is the query to plan
 * tlist is the target list the query should produce (NOT root->targetList!)
 * tuple_fraction is the fraction of tuples we expect will be retrieved
 *
 * Output parameters:
 * *cheapest_path receives the overall-cheapest path for the query
 * *sorted_path receives the cheapest presorted path for the query,
 *				if any (NULL if there is no useful presorted path)
 *
 * Note: the Query node also includes a query_pathkeys field, which is both
 * an input and an output of query_planner().  The input value signals
 * query_planner that the indicated sort order is wanted in the final output
 * plan.  But this value has not yet been "canonicalized", since the needed
 * info does not get computed until we scan the qual clauses.  We canonicalize
 * it as soon as that task is done.  (The main reason query_pathkeys is a
 * Query field and not a passed parameter is that the low-level routines in
 * indxpath.c need to see it.)
 *
 * tuple_fraction is interpreted as follows:
 *	  0: expect all tuples to be retrieved (normal case)
 *	  0 < tuple_fraction < 1: expect the given fraction of tuples available
 *		from the plan to be retrieved
 *	  tuple_fraction >= 1: tuple_fraction is the absolute number of tuples
 *		expected to be retrieved (ie, a LIMIT specification)
 *--------------------
 */
void
query_planner(Query *root, List *tlist, double tuple_fraction,
			  Path **cheapest_path, Path **sorted_path)
{
	List	   *constant_quals;
	RelOptInfo *final_rel;
	Path	   *cheapestpath;
	Path	   *sortedpath;

	/*
	 * If the query has an empty join tree, then it's something easy like
	 * "SELECT 2+2;" or "INSERT ... VALUES()".	Fall through quickly.
	 */
	if (root->jointree->fromlist == NIL)
	{
		*cheapest_path = (Path *) create_result_path(NULL, NULL,
										 (List *) root->jointree->quals);
		*sorted_path = NULL;
		return;
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
	 * init planner lists to empty
	 *
	 * NOTE: in_info_list was set up by subquery_planner, do not touch here
	 */
	root->base_rel_list = NIL;
	root->other_rel_list = NIL;
	root->join_rel_list = NIL;
	root->equi_key_list = NIL;

	/*
	 * Construct RelOptInfo nodes for all base relations in query.
	 */
	add_base_rels_to_query(root, (Node *) root->jointree);

	/*
	 * Examine the targetlist and qualifications, adding entries to
	 * baserel targetlists for all referenced Vars.  Restrict and join
	 * clauses are added to appropriate lists belonging to the mentioned
	 * relations.  We also build lists of equijoined keys for pathkey
	 * construction.
	 *
	 * Note: all subplan nodes will have "flat" (var-only) tlists. This
	 * implies that all expression evaluations are done at the root of the
	 * plan tree.  Once upon a time there was code to try to push
	 * expensive function calls down to lower plan nodes, but that's dead
	 * code and has been for a long time...
	 */
	build_base_rel_tlists(root, tlist);

	(void) distribute_quals_to_rels(root, (Node *) root->jointree, false);

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

	if (!final_rel || !final_rel->cheapest_total_path)
		elog(ERROR, "failed to construct the join relation");

	/*
	 * Now that we have an estimate of the final rel's size, we can
	 * convert a tuple_fraction specified as an absolute count (ie, a
	 * LIMIT option) into a fraction of the total tuples.
	 */
	if (tuple_fraction >= 1.0)
		tuple_fraction /= final_rel->rows;

	/*
	 * Pick out the cheapest-total path and the cheapest presorted path
	 * for the requested pathkeys (if there is one).  We should take the
	 * tuple fraction into account when selecting the cheapest presorted
	 * path, but not when selecting the cheapest-total path, since if we
	 * have to sort then we'll have to fetch all the tuples.  (But there's
	 * a special case: if query_pathkeys is NIL, meaning order doesn't
	 * matter, then the "cheapest presorted" path will be the cheapest
	 * overall for the tuple fraction.)
	 *
	 * The cheapest-total path is also the one to use if grouping_planner
	 * decides to use hashed aggregation, so we return it separately even
	 * if this routine thinks the presorted path is the winner.
	 */
	cheapestpath = final_rel->cheapest_total_path;

	sortedpath =
		get_cheapest_fractional_path_for_pathkeys(final_rel->pathlist,
												  root->query_pathkeys,
												  tuple_fraction);

	/* Don't return same path in both guises; just wastes effort */
	if (sortedpath == cheapestpath)
		sortedpath = NULL;

	/*
	 * Forget about the presorted path if it would be cheaper to sort the
	 * cheapest-total path.  Here we need consider only the behavior at
	 * the tuple fraction point.
	 */
	if (sortedpath)
	{
		Path		sort_path;	/* dummy for result of cost_sort */

		if (root->query_pathkeys == NIL ||
			pathkeys_contained_in(root->query_pathkeys,
								  cheapestpath->pathkeys))
		{
			/* No sort needed for cheapest path */
			sort_path.startup_cost = cheapestpath->startup_cost;
			sort_path.total_cost = cheapestpath->total_cost;
		}
		else
		{
			/* Figure cost for sorting */
			cost_sort(&sort_path, root, root->query_pathkeys,
					  cheapestpath->total_cost,
					  final_rel->rows, final_rel->width);
		}

		if (compare_fractional_path_costs(sortedpath, &sort_path,
										  tuple_fraction) > 0)
		{
			/* Presorted path is a loser */
			sortedpath = NULL;
		}
	}

	/*
	 * If we have constant quals, add a toplevel Result step to process
	 * them.
	 */
	if (constant_quals)
	{
		cheapestpath = (Path *) create_result_path(final_rel,
												   cheapestpath,
												   constant_quals);
		if (sortedpath)
			sortedpath = (Path *) create_result_path(final_rel,
													 sortedpath,
													 constant_quals);
	}

	*cheapest_path = cheapestpath;
	*sorted_path = sortedpath;
}
