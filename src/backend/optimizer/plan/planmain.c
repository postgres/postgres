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
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/planmain.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/placeholder.h"
#include "optimizer/planmain.h"
#include "optimizer/tlist.h"
#include "utils/selfuncs.h"


/*
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
 * root describes the query to plan
 * tlist is the target list the query should produce
 *		(this is NOT necessarily root->parse->targetList!)
 * tuple_fraction is the fraction of tuples we expect will be retrieved
 * limit_tuples is a hard limit on number of tuples to retrieve,
 *		or -1 if no limit
 * qp_callback is a function to compute query_pathkeys once it's safe to do so
 * qp_extra is optional extra data to pass to qp_callback
 *
 * Output parameters:
 * *cheapest_path receives the overall-cheapest path for the query
 * *sorted_path receives the cheapest presorted path for the query,
 *				if any (NULL if there is no useful presorted path)
 * *num_groups receives the estimated number of groups, or 1 if query
 *				does not use grouping
 *
 * Note: the PlannerInfo node also includes a query_pathkeys field, which
 * tells query_planner the sort order that is desired in the final output
 * plan.  This value is *not* available at call time, but is computed by
 * qp_callback once we have completed merging the query's equivalence classes.
 * (We cannot construct canonical pathkeys until that's done.)
 *
 * tuple_fraction is interpreted as follows:
 *	  0: expect all tuples to be retrieved (normal case)
 *	  0 < tuple_fraction < 1: expect the given fraction of tuples available
 *		from the plan to be retrieved
 *	  tuple_fraction >= 1: tuple_fraction is the absolute number of tuples
 *		expected to be retrieved (ie, a LIMIT specification)
 * Note that a nonzero tuple_fraction could come from outer context; it is
 * therefore not redundant with limit_tuples.  We use limit_tuples to determine
 * whether a bounded sort can be used at runtime.
 */
void
query_planner(PlannerInfo *root, List *tlist,
			  double tuple_fraction, double limit_tuples,
			  query_pathkeys_callback qp_callback, void *qp_extra,
			  Path **cheapest_path, Path **sorted_path,
			  double *num_groups)
{
	Query	   *parse = root->parse;
	List	   *joinlist;
	RelOptInfo *final_rel;
	Path	   *cheapestpath;
	Path	   *sortedpath;
	Index		rti;
	double		total_pages;

	/* Make tuple_fraction, limit_tuples accessible to lower-level routines */
	root->tuple_fraction = tuple_fraction;
	root->limit_tuples = limit_tuples;

	*num_groups = 1;			/* default result */

	/*
	 * If the query has an empty join tree, then it's something easy like
	 * "SELECT 2+2;" or "INSERT ... VALUES()".	Fall through quickly.
	 */
	if (parse->jointree->fromlist == NIL)
	{
		/* We need a trivial path result */
		*cheapest_path = (Path *)
			create_result_path((List *) parse->jointree->quals);
		*sorted_path = NULL;

		/*
		 * We still are required to call qp_callback, in case it's something
		 * like "SELECT 2+2 ORDER BY 1".
		 */
		root->canon_pathkeys = NIL;
		(*qp_callback) (root, qp_extra);
		return;
	}

	/*
	 * Init planner lists to empty.
	 *
	 * NOTE: append_rel_list was set up by subquery_planner, so do not touch
	 * here; eq_classes and minmax_aggs may contain data already, too.
	 */
	root->join_rel_list = NIL;
	root->join_rel_hash = NULL;
	root->join_rel_level = NULL;
	root->join_cur_level = 0;
	root->canon_pathkeys = NIL;
	root->left_join_clauses = NIL;
	root->right_join_clauses = NIL;
	root->full_join_clauses = NIL;
	root->join_info_list = NIL;
	root->lateral_info_list = NIL;
	root->placeholder_list = NIL;
	root->initial_rels = NIL;

	/*
	 * Make a flattened version of the rangetable for faster access (this is
	 * OK because the rangetable won't change any more), and set up an empty
	 * array for indexing base relations.
	 */
	setup_simple_rel_arrays(root);

	/*
	 * Construct RelOptInfo nodes for all base relations in query, and
	 * indirectly for all appendrel member relations ("other rels").  This
	 * will give us a RelOptInfo for every "simple" (non-join) rel involved in
	 * the query.
	 *
	 * Note: the reason we find the rels by searching the jointree and
	 * appendrel list, rather than just scanning the rangetable, is that the
	 * rangetable may contain RTEs for rels not actively part of the query,
	 * for example views.  We don't want to make RelOptInfos for them.
	 */
	add_base_rels_to_query(root, (Node *) parse->jointree);

	/*
	 * Examine the targetlist and join tree, adding entries to baserel
	 * targetlists for all referenced Vars, and generating PlaceHolderInfo
	 * entries for all referenced PlaceHolderVars.	Restrict and join clauses
	 * are added to appropriate lists belonging to the mentioned relations. We
	 * also build EquivalenceClasses for provably equivalent expressions. The
	 * SpecialJoinInfo list is also built to hold information about join order
	 * restrictions.  Finally, we form a target joinlist for make_one_rel() to
	 * work from.
	 */
	build_base_rel_tlists(root, tlist);

	find_placeholders_in_jointree(root);

	find_lateral_references(root);

	joinlist = deconstruct_jointree(root);

	/*
	 * Reconsider any postponed outer-join quals now that we have built up
	 * equivalence classes.  (This could result in further additions or
	 * mergings of classes.)
	 */
	reconsider_outer_join_clauses(root);

	/*
	 * If we formed any equivalence classes, generate additional restriction
	 * clauses as appropriate.	(Implied join clauses are formed on-the-fly
	 * later.)
	 */
	generate_base_implied_equalities(root);

	/*
	 * We have completed merging equivalence sets, so it's now possible to
	 * generate pathkeys in canonical form; so compute query_pathkeys and
	 * other pathkeys fields in PlannerInfo.
	 */
	(*qp_callback) (root, qp_extra);

	/*
	 * Examine any "placeholder" expressions generated during subquery pullup.
	 * Make sure that the Vars they need are marked as needed at the relevant
	 * join level.	This must be done before join removal because it might
	 * cause Vars or placeholders to be needed above a join when they weren't
	 * so marked before.
	 */
	fix_placeholder_input_needed_levels(root);

	/*
	 * Remove any useless outer joins.	Ideally this would be done during
	 * jointree preprocessing, but the necessary information isn't available
	 * until we've built baserel data structures and classified qual clauses.
	 */
	joinlist = remove_useless_joins(root, joinlist);

	/*
	 * Now distribute "placeholders" to base rels as needed.  This has to be
	 * done after join removal because removal could change whether a
	 * placeholder is evaluatable at a base rel.
	 */
	add_placeholders_to_base_rels(root);

	/*
	 * Create the LateralJoinInfo list now that we have finalized
	 * PlaceHolderVar eval levels and made any necessary additions to the
	 * lateral_vars lists for lateral references within PlaceHolderVars.
	 */
	create_lateral_join_info(root);

	/*
	 * We should now have size estimates for every actual table involved in
	 * the query, and we also know which if any have been deleted from the
	 * query by join removal; so we can compute total_table_pages.
	 *
	 * Note that appendrels are not double-counted here, even though we don't
	 * bother to distinguish RelOptInfos for appendrel parents, because the
	 * parents will still have size zero.
	 *
	 * XXX if a table is self-joined, we will count it once per appearance,
	 * which perhaps is the wrong thing ... but that's not completely clear,
	 * and detecting self-joins here is difficult, so ignore it for now.
	 */
	total_pages = 0;
	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *brel = root->simple_rel_array[rti];

		if (brel == NULL)
			continue;

		Assert(brel->relid == rti);		/* sanity check on array */

		if (brel->reloptkind == RELOPT_BASEREL ||
			brel->reloptkind == RELOPT_OTHER_MEMBER_REL)
			total_pages += (double) brel->pages;
	}
	root->total_table_pages = total_pages;

	/*
	 * Ready to do the primary planning.
	 */
	final_rel = make_one_rel(root, joinlist);

	if (!final_rel || !final_rel->cheapest_total_path ||
		final_rel->cheapest_total_path->param_info != NULL)
		elog(ERROR, "failed to construct the join relation");

	/*
	 * If there's grouping going on, estimate the number of result groups. We
	 * couldn't do this any earlier because it depends on relation size
	 * estimates that were set up above.
	 *
	 * Then convert tuple_fraction to fractional form if it is absolute, and
	 * adjust it based on the knowledge that grouping_planner will be doing
	 * grouping or aggregation work with our result.
	 *
	 * This introduces some undesirable coupling between this code and
	 * grouping_planner, but the alternatives seem even uglier; we couldn't
	 * pass back completed paths without making these decisions here.
	 */
	if (parse->groupClause)
	{
		List	   *groupExprs;

		groupExprs = get_sortgrouplist_exprs(parse->groupClause,
											 parse->targetList);
		*num_groups = estimate_num_groups(root,
										  groupExprs,
										  final_rel->rows);

		/*
		 * In GROUP BY mode, an absolute LIMIT is relative to the number of
		 * groups not the number of tuples.  If the caller gave us a fraction,
		 * keep it as-is.  (In both cases, we are effectively assuming that
		 * all the groups are about the same size.)
		 */
		if (tuple_fraction >= 1.0)
			tuple_fraction /= *num_groups;

		/*
		 * If both GROUP BY and ORDER BY are specified, we will need two
		 * levels of sort --- and, therefore, certainly need to read all the
		 * tuples --- unless ORDER BY is a subset of GROUP BY.	Likewise if we
		 * have both DISTINCT and GROUP BY, or if we have a window
		 * specification not compatible with the GROUP BY.
		 */
		if (!pathkeys_contained_in(root->sort_pathkeys, root->group_pathkeys) ||
			!pathkeys_contained_in(root->distinct_pathkeys, root->group_pathkeys) ||
		 !pathkeys_contained_in(root->window_pathkeys, root->group_pathkeys))
			tuple_fraction = 0.0;

		/* In any case, limit_tuples shouldn't be specified here */
		Assert(limit_tuples < 0);
	}
	else if (parse->hasAggs || root->hasHavingQual)
	{
		/*
		 * Ungrouped aggregate will certainly want to read all the tuples, and
		 * it will deliver a single result row (so leave *num_groups 1).
		 */
		tuple_fraction = 0.0;

		/* limit_tuples shouldn't be specified here */
		Assert(limit_tuples < 0);
	}
	else if (parse->distinctClause)
	{
		/*
		 * Since there was no grouping or aggregation, it's reasonable to
		 * assume the UNIQUE filter has effects comparable to GROUP BY. Return
		 * the estimated number of output rows for use by caller. (If DISTINCT
		 * is used with grouping, we ignore its effects for rowcount
		 * estimation purposes; this amounts to assuming the grouped rows are
		 * distinct already.)
		 */
		List	   *distinctExprs;

		distinctExprs = get_sortgrouplist_exprs(parse->distinctClause,
												parse->targetList);
		*num_groups = estimate_num_groups(root,
										  distinctExprs,
										  final_rel->rows);

		/*
		 * Adjust tuple_fraction the same way as for GROUP BY, too.
		 */
		if (tuple_fraction >= 1.0)
			tuple_fraction /= *num_groups;

		/* limit_tuples shouldn't be specified here */
		Assert(limit_tuples < 0);
	}
	else
	{
		/*
		 * Plain non-grouped, non-aggregated query: an absolute tuple fraction
		 * can be divided by the number of tuples.
		 */
		if (tuple_fraction >= 1.0)
			tuple_fraction /= final_rel->rows;
	}

	/*
	 * Pick out the cheapest-total path and the cheapest presorted path for
	 * the requested pathkeys (if there is one).  We should take the tuple
	 * fraction into account when selecting the cheapest presorted path, but
	 * not when selecting the cheapest-total path, since if we have to sort
	 * then we'll have to fetch all the tuples.  (But there's a special case:
	 * if query_pathkeys is NIL, meaning order doesn't matter, then the
	 * "cheapest presorted" path will be the cheapest overall for the tuple
	 * fraction.)
	 *
	 * The cheapest-total path is also the one to use if grouping_planner
	 * decides to use hashed aggregation, so we return it separately even if
	 * this routine thinks the presorted path is the winner.
	 */
	cheapestpath = final_rel->cheapest_total_path;

	sortedpath =
		get_cheapest_fractional_path_for_pathkeys(final_rel->pathlist,
												  root->query_pathkeys,
												  NULL,
												  tuple_fraction);

	/* Don't return same path in both guises; just wastes effort */
	if (sortedpath == cheapestpath)
		sortedpath = NULL;

	/*
	 * Forget about the presorted path if it would be cheaper to sort the
	 * cheapest-total path.  Here we need consider only the behavior at the
	 * tuple fraction point.
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
					  final_rel->rows, final_rel->width,
					  0.0, work_mem, limit_tuples);
		}

		if (compare_fractional_path_costs(sortedpath, &sort_path,
										  tuple_fraction) > 0)
		{
			/* Presorted path is a loser */
			sortedpath = NULL;
		}
	}

	*cheapest_path = cheapestpath;
	*sorted_path = sortedpath;
}
