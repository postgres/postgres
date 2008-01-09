/*-------------------------------------------------------------------------
 *
 * joinpath.c
 *	  Routines to find all possible paths for processing a set of joins
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/path/joinpath.c,v 1.115 2008/01/09 20:42:27 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"


static void sort_inner_and_outer(PlannerInfo *root, RelOptInfo *joinrel,
					 RelOptInfo *outerrel, RelOptInfo *innerrel,
					 List *restrictlist, List *mergeclause_list,
					 JoinType jointype);
static void match_unsorted_outer(PlannerInfo *root, RelOptInfo *joinrel,
					 RelOptInfo *outerrel, RelOptInfo *innerrel,
					 List *restrictlist, List *mergeclause_list,
					 JoinType jointype);
static void hash_inner_and_outer(PlannerInfo *root, RelOptInfo *joinrel,
					 RelOptInfo *outerrel, RelOptInfo *innerrel,
					 List *restrictlist, JoinType jointype);
static Path *best_appendrel_indexscan(PlannerInfo *root, RelOptInfo *rel,
						 RelOptInfo *outer_rel, JoinType jointype);
static List *select_mergejoin_clauses(PlannerInfo *root,
						 RelOptInfo *joinrel,
						 RelOptInfo *outerrel,
						 RelOptInfo *innerrel,
						 List *restrictlist,
						 JoinType jointype);


/*
 * add_paths_to_joinrel
 *	  Given a join relation and two component rels from which it can be made,
 *	  consider all possible paths that use the two component rels as outer
 *	  and inner rel respectively.  Add these paths to the join rel's pathlist
 *	  if they survive comparison with other paths (and remove any existing
 *	  paths that are dominated by these paths).
 *
 * Modifies the pathlist field of the joinrel node to contain the best
 * paths found so far.
 */
void
add_paths_to_joinrel(PlannerInfo *root,
					 RelOptInfo *joinrel,
					 RelOptInfo *outerrel,
					 RelOptInfo *innerrel,
					 JoinType jointype,
					 List *restrictlist)
{
	List	   *mergeclause_list = NIL;

	/*
	 * Find potential mergejoin clauses.  We can skip this if we are not
	 * interested in doing a mergejoin.  However, mergejoin is currently our
	 * only way of implementing full outer joins, so override mergejoin
	 * disable if it's a full join.
	 */
	if (enable_mergejoin || jointype == JOIN_FULL)
		mergeclause_list = select_mergejoin_clauses(root,
													joinrel,
													outerrel,
													innerrel,
													restrictlist,
													jointype);

	/*
	 * 1. Consider mergejoin paths where both relations must be explicitly
	 * sorted.
	 */
	sort_inner_and_outer(root, joinrel, outerrel, innerrel,
						 restrictlist, mergeclause_list, jointype);

	/*
	 * 2. Consider paths where the outer relation need not be explicitly
	 * sorted. This includes both nestloops and mergejoins where the outer
	 * path is already ordered.
	 */
	match_unsorted_outer(root, joinrel, outerrel, innerrel,
						 restrictlist, mergeclause_list, jointype);

#ifdef NOT_USED

	/*
	 * 3. Consider paths where the inner relation need not be explicitly
	 * sorted.	This includes mergejoins only (nestloops were already built in
	 * match_unsorted_outer).
	 *
	 * Diked out as redundant 2/13/2000 -- tgl.  There isn't any really
	 * significant difference between the inner and outer side of a mergejoin,
	 * so match_unsorted_inner creates no paths that aren't equivalent to
	 * those made by match_unsorted_outer when add_paths_to_joinrel() is
	 * invoked with the two rels given in the other order.
	 */
	match_unsorted_inner(root, joinrel, outerrel, innerrel,
						 restrictlist, mergeclause_list, jointype);
#endif

	/*
	 * 4. Consider paths where both outer and inner relations must be hashed
	 * before being joined.
	 */
	if (enable_hashjoin)
		hash_inner_and_outer(root, joinrel, outerrel, innerrel,
							 restrictlist, jointype);
}

/*
 * sort_inner_and_outer
 *	  Create mergejoin join paths by explicitly sorting both the outer and
 *	  inner join relations on each available merge ordering.
 *
 * 'joinrel' is the join relation
 * 'outerrel' is the outer join relation
 * 'innerrel' is the inner join relation
 * 'restrictlist' contains all of the RestrictInfo nodes for restriction
 *		clauses that apply to this join
 * 'mergeclause_list' is a list of RestrictInfo nodes for available
 *		mergejoin clauses in this join
 * 'jointype' is the type of join to do
 */
static void
sort_inner_and_outer(PlannerInfo *root,
					 RelOptInfo *joinrel,
					 RelOptInfo *outerrel,
					 RelOptInfo *innerrel,
					 List *restrictlist,
					 List *mergeclause_list,
					 JoinType jointype)
{
	bool		useallclauses;
	Path	   *outer_path;
	Path	   *inner_path;
	List	   *all_pathkeys;
	ListCell   *l;

	/*
	 * If we are doing a right or full join, we must use *all* the
	 * mergeclauses as join clauses, else we will not have a valid plan.
	 */
	switch (jointype)
	{
		case JOIN_INNER:
		case JOIN_LEFT:
		case JOIN_IN:
		case JOIN_UNIQUE_OUTER:
		case JOIN_UNIQUE_INNER:
			useallclauses = false;
			break;
		case JOIN_RIGHT:
		case JOIN_FULL:
			useallclauses = true;
			break;
		default:
			elog(ERROR, "unrecognized join type: %d",
				 (int) jointype);
			useallclauses = false;		/* keep compiler quiet */
			break;
	}

	/*
	 * We only consider the cheapest-total-cost input paths, since we are
	 * assuming here that a sort is required.  We will consider
	 * cheapest-startup-cost input paths later, and only if they don't need a
	 * sort.
	 *
	 * If unique-ification is requested, do it and then handle as a plain
	 * inner join.
	 */
	outer_path = outerrel->cheapest_total_path;
	inner_path = innerrel->cheapest_total_path;
	if (jointype == JOIN_UNIQUE_OUTER)
	{
		outer_path = (Path *) create_unique_path(root, outerrel, outer_path);
		jointype = JOIN_INNER;
	}
	else if (jointype == JOIN_UNIQUE_INNER)
	{
		inner_path = (Path *) create_unique_path(root, innerrel, inner_path);
		jointype = JOIN_INNER;
	}

	/*
	 * Each possible ordering of the available mergejoin clauses will generate
	 * a differently-sorted result path at essentially the same cost.  We have
	 * no basis for choosing one over another at this level of joining, but
	 * some sort orders may be more useful than others for higher-level
	 * mergejoins, so it's worth considering multiple orderings.
	 *
	 * Actually, it's not quite true that every mergeclause ordering will
	 * generate a different path order, because some of the clauses may be
	 * partially redundant (refer to the same EquivalenceClasses).	Therefore,
	 * what we do is convert the mergeclause list to a list of canonical
	 * pathkeys, and then consider different orderings of the pathkeys.
	 *
	 * Generating a path for *every* permutation of the pathkeys doesn't seem
	 * like a winning strategy; the cost in planning time is too high. For
	 * now, we generate one path for each pathkey, listing that pathkey first
	 * and the rest in random order.  This should allow at least a one-clause
	 * mergejoin without re-sorting against any other possible mergejoin
	 * partner path.  But if we've not guessed the right ordering of secondary
	 * keys, we may end up evaluating clauses as qpquals when they could have
	 * been done as mergeclauses.  (In practice, it's rare that there's more
	 * than two or three mergeclauses, so expending a huge amount of thought
	 * on that is probably not worth it.)
	 *
	 * The pathkey order returned by select_outer_pathkeys_for_merge() has
	 * some heuristics behind it (see that function), so be sure to try it
	 * exactly as-is as well as making variants.
	 */
	all_pathkeys = select_outer_pathkeys_for_merge(root,
												   mergeclause_list,
												   joinrel);

	foreach(l, all_pathkeys)
	{
		List	   *front_pathkey = (List *) lfirst(l);
		List	   *cur_mergeclauses;
		List	   *outerkeys;
		List	   *innerkeys;
		List	   *merge_pathkeys;

		/* Make a pathkey list with this guy first */
		if (l != list_head(all_pathkeys))
			outerkeys = lcons(front_pathkey,
							  list_delete_ptr(list_copy(all_pathkeys),
											  front_pathkey));
		else
			outerkeys = all_pathkeys;	/* no work at first one... */

		/* Sort the mergeclauses into the corresponding ordering */
		cur_mergeclauses = find_mergeclauses_for_pathkeys(root,
														  outerkeys,
														  true,
														  mergeclause_list);

		/* Should have used them all... */
		Assert(list_length(cur_mergeclauses) == list_length(mergeclause_list));

		/* Build sort pathkeys for the inner side */
		innerkeys = make_inner_pathkeys_for_merge(root,
												  cur_mergeclauses,
												  outerkeys);

		/* Build pathkeys representing output sort order */
		merge_pathkeys = build_join_pathkeys(root, joinrel, jointype,
											 outerkeys);

		/*
		 * And now we can make the path.
		 *
		 * Note: it's possible that the cheapest paths will already be sorted
		 * properly.  create_mergejoin_path will detect that case and suppress
		 * an explicit sort step, so we needn't do so here.
		 */
		add_path(joinrel, (Path *)
				 create_mergejoin_path(root,
									   joinrel,
									   jointype,
									   outer_path,
									   inner_path,
									   restrictlist,
									   merge_pathkeys,
									   cur_mergeclauses,
									   outerkeys,
									   innerkeys));
	}
}

/*
 * match_unsorted_outer
 *	  Creates possible join paths for processing a single join relation
 *	  'joinrel' by employing either iterative substitution or
 *	  mergejoining on each of its possible outer paths (considering
 *	  only outer paths that are already ordered well enough for merging).
 *
 * We always generate a nestloop path for each available outer path.
 * In fact we may generate as many as five: one on the cheapest-total-cost
 * inner path, one on the same with materialization, one on the
 * cheapest-startup-cost inner path (if different), one on the
 * cheapest-total inner-indexscan path (if any), and one on the
 * cheapest-startup inner-indexscan path (if different).
 *
 * We also consider mergejoins if mergejoin clauses are available.	We have
 * two ways to generate the inner path for a mergejoin: sort the cheapest
 * inner path, or use an inner path that is already suitably ordered for the
 * merge.  If we have several mergeclauses, it could be that there is no inner
 * path (or only a very expensive one) for the full list of mergeclauses, but
 * better paths exist if we truncate the mergeclause list (thereby discarding
 * some sort key requirements).  So, we consider truncations of the
 * mergeclause list as well as the full list.  (Ideally we'd consider all
 * subsets of the mergeclause list, but that seems way too expensive.)
 *
 * 'joinrel' is the join relation
 * 'outerrel' is the outer join relation
 * 'innerrel' is the inner join relation
 * 'restrictlist' contains all of the RestrictInfo nodes for restriction
 *		clauses that apply to this join
 * 'mergeclause_list' is a list of RestrictInfo nodes for available
 *		mergejoin clauses in this join
 * 'jointype' is the type of join to do
 */
static void
match_unsorted_outer(PlannerInfo *root,
					 RelOptInfo *joinrel,
					 RelOptInfo *outerrel,
					 RelOptInfo *innerrel,
					 List *restrictlist,
					 List *mergeclause_list,
					 JoinType jointype)
{
	JoinType	save_jointype = jointype;
	bool		nestjoinOK;
	bool		useallclauses;
	Path	   *inner_cheapest_startup = innerrel->cheapest_startup_path;
	Path	   *inner_cheapest_total = innerrel->cheapest_total_path;
	Path	   *matpath = NULL;
	Path	   *index_cheapest_startup = NULL;
	Path	   *index_cheapest_total = NULL;
	ListCell   *l;

	/*
	 * Nestloop only supports inner, left, and IN joins.  Also, if we are
	 * doing a right or full join, we must use *all* the mergeclauses as join
	 * clauses, else we will not have a valid plan.  (Although these two flags
	 * are currently inverses, keep them separate for clarity and possible
	 * future changes.)
	 */
	switch (jointype)
	{
		case JOIN_INNER:
		case JOIN_LEFT:
		case JOIN_IN:
		case JOIN_UNIQUE_OUTER:
		case JOIN_UNIQUE_INNER:
			nestjoinOK = true;
			useallclauses = false;
			break;
		case JOIN_RIGHT:
		case JOIN_FULL:
			nestjoinOK = false;
			useallclauses = true;
			break;
		default:
			elog(ERROR, "unrecognized join type: %d",
				 (int) jointype);
			nestjoinOK = false; /* keep compiler quiet */
			useallclauses = false;
			break;
	}

	/*
	 * If we need to unique-ify the inner path, we will consider only the
	 * cheapest inner.
	 */
	if (jointype == JOIN_UNIQUE_INNER)
	{
		inner_cheapest_total = (Path *)
			create_unique_path(root, innerrel, inner_cheapest_total);
		inner_cheapest_startup = inner_cheapest_total;
		jointype = JOIN_INNER;
	}
	else if (nestjoinOK)
	{
		/*
		 * If the cheapest inner path is a join or seqscan, we should consider
		 * materializing it.  (This is a heuristic: we could consider it
		 * always, but for inner indexscans it's probably a waste of time.)
		 */
		if (!(IsA(inner_cheapest_total, IndexPath) ||
			  IsA(inner_cheapest_total, BitmapHeapPath) ||
			  IsA(inner_cheapest_total, TidPath)))
			matpath = (Path *)
				create_material_path(innerrel, inner_cheapest_total);

		/*
		 * Get the best innerjoin indexpaths (if any) for this outer rel.
		 * They're the same for all outer paths.
		 */
		if (innerrel->reloptkind != RELOPT_JOINREL)
		{
			if (IsA(inner_cheapest_total, AppendPath))
				index_cheapest_total = best_appendrel_indexscan(root,
																innerrel,
																outerrel,
																jointype);
			else if (innerrel->rtekind == RTE_RELATION)
				best_inner_indexscan(root, innerrel, outerrel, jointype,
									 &index_cheapest_startup,
									 &index_cheapest_total);
		}
	}

	foreach(l, outerrel->pathlist)
	{
		Path	   *outerpath = (Path *) lfirst(l);
		List	   *merge_pathkeys;
		List	   *mergeclauses;
		List	   *innersortkeys;
		List	   *trialsortkeys;
		Path	   *cheapest_startup_inner;
		Path	   *cheapest_total_inner;
		int			num_sortkeys;
		int			sortkeycnt;

		/*
		 * If we need to unique-ify the outer path, it's pointless to consider
		 * any but the cheapest outer.
		 */
		if (save_jointype == JOIN_UNIQUE_OUTER)
		{
			if (outerpath != outerrel->cheapest_total_path)
				continue;
			outerpath = (Path *) create_unique_path(root, outerrel, outerpath);
			jointype = JOIN_INNER;
		}

		/*
		 * The result will have this sort order (even if it is implemented as
		 * a nestloop, and even if some of the mergeclauses are implemented by
		 * qpquals rather than as true mergeclauses):
		 */
		merge_pathkeys = build_join_pathkeys(root, joinrel, jointype,
											 outerpath->pathkeys);

		if (nestjoinOK)
		{
			/*
			 * Always consider a nestloop join with this outer and
			 * cheapest-total-cost inner.  When appropriate, also consider
			 * using the materialized form of the cheapest inner, the
			 * cheapest-startup-cost inner path, and the cheapest innerjoin
			 * indexpaths.
			 */
			add_path(joinrel, (Path *)
					 create_nestloop_path(root,
										  joinrel,
										  jointype,
										  outerpath,
										  inner_cheapest_total,
										  restrictlist,
										  merge_pathkeys));
			if (matpath != NULL)
				add_path(joinrel, (Path *)
						 create_nestloop_path(root,
											  joinrel,
											  jointype,
											  outerpath,
											  matpath,
											  restrictlist,
											  merge_pathkeys));
			if (inner_cheapest_startup != inner_cheapest_total)
				add_path(joinrel, (Path *)
						 create_nestloop_path(root,
											  joinrel,
											  jointype,
											  outerpath,
											  inner_cheapest_startup,
											  restrictlist,
											  merge_pathkeys));
			if (index_cheapest_total != NULL)
				add_path(joinrel, (Path *)
						 create_nestloop_path(root,
											  joinrel,
											  jointype,
											  outerpath,
											  index_cheapest_total,
											  restrictlist,
											  merge_pathkeys));
			if (index_cheapest_startup != NULL &&
				index_cheapest_startup != index_cheapest_total)
				add_path(joinrel, (Path *)
						 create_nestloop_path(root,
											  joinrel,
											  jointype,
											  outerpath,
											  index_cheapest_startup,
											  restrictlist,
											  merge_pathkeys));
		}

		/* Can't do anything else if outer path needs to be unique'd */
		if (save_jointype == JOIN_UNIQUE_OUTER)
			continue;

		/* Look for useful mergeclauses (if any) */
		mergeclauses = find_mergeclauses_for_pathkeys(root,
													  outerpath->pathkeys,
													  true,
													  mergeclause_list);

		/*
		 * Done with this outer path if no chance for a mergejoin.
		 *
		 * Special corner case: for "x FULL JOIN y ON true", there will be no
		 * join clauses at all.  Ordinarily we'd generate a clauseless
		 * nestloop path, but since mergejoin is our only join type that
		 * supports FULL JOIN, it's necessary to generate a clauseless
		 * mergejoin path instead.
		 */
		if (mergeclauses == NIL)
		{
			if (jointype == JOIN_FULL)
				 /* okay to try for mergejoin */ ;
			else
				continue;
		}
		if (useallclauses && list_length(mergeclauses) != list_length(mergeclause_list))
			continue;

		/* Compute the required ordering of the inner path */
		innersortkeys = make_inner_pathkeys_for_merge(root,
													  mergeclauses,
													  outerpath->pathkeys);

		/*
		 * Generate a mergejoin on the basis of sorting the cheapest inner.
		 * Since a sort will be needed, only cheapest total cost matters. (But
		 * create_mergejoin_path will do the right thing if
		 * inner_cheapest_total is already correctly sorted.)
		 */
		add_path(joinrel, (Path *)
				 create_mergejoin_path(root,
									   joinrel,
									   jointype,
									   outerpath,
									   inner_cheapest_total,
									   restrictlist,
									   merge_pathkeys,
									   mergeclauses,
									   NIL,
									   innersortkeys));

		/* Can't do anything else if inner path needs to be unique'd */
		if (save_jointype == JOIN_UNIQUE_INNER)
			continue;

		/*
		 * Look for presorted inner paths that satisfy the innersortkey list
		 * --- or any truncation thereof, if we are allowed to build a
		 * mergejoin using a subset of the merge clauses.  Here, we consider
		 * both cheap startup cost and cheap total cost.  We can ignore
		 * inner_cheapest_total on the first iteration, since we already made
		 * a path with it --- but not on later iterations with shorter sort
		 * keys, because then we are considering a different situation, viz
		 * using a simpler mergejoin to avoid a sort of the inner rel.
		 */
		num_sortkeys = list_length(innersortkeys);
		if (num_sortkeys > 1 && !useallclauses)
			trialsortkeys = list_copy(innersortkeys);	/* need modifiable copy */
		else
			trialsortkeys = innersortkeys;		/* won't really truncate */
		cheapest_startup_inner = NULL;
		cheapest_total_inner = NULL;

		for (sortkeycnt = num_sortkeys; sortkeycnt > 0; sortkeycnt--)
		{
			Path	   *innerpath;
			List	   *newclauses = NIL;

			/*
			 * Look for an inner path ordered well enough for the first
			 * 'sortkeycnt' innersortkeys.	NB: trialsortkeys list is modified
			 * destructively, which is why we made a copy...
			 */
			trialsortkeys = list_truncate(trialsortkeys, sortkeycnt);
			innerpath = get_cheapest_path_for_pathkeys(innerrel->pathlist,
													   trialsortkeys,
													   TOTAL_COST);
			if (innerpath != NULL &&
				(innerpath != inner_cheapest_total ||
				 sortkeycnt < num_sortkeys) &&
				(cheapest_total_inner == NULL ||
				 compare_path_costs(innerpath, cheapest_total_inner,
									TOTAL_COST) < 0))
			{
				/* Found a cheap (or even-cheaper) sorted path */
				/* Select the right mergeclauses, if we didn't already */
				if (sortkeycnt < num_sortkeys)
				{
					newclauses =
						find_mergeclauses_for_pathkeys(root,
													   trialsortkeys,
													   false,
													   mergeclauses);
					Assert(newclauses != NIL);
				}
				else
					newclauses = mergeclauses;
				add_path(joinrel, (Path *)
						 create_mergejoin_path(root,
											   joinrel,
											   jointype,
											   outerpath,
											   innerpath,
											   restrictlist,
											   merge_pathkeys,
											   newclauses,
											   NIL,
											   NIL));
				cheapest_total_inner = innerpath;
			}
			/* Same on the basis of cheapest startup cost ... */
			innerpath = get_cheapest_path_for_pathkeys(innerrel->pathlist,
													   trialsortkeys,
													   STARTUP_COST);
			if (innerpath != NULL &&
				(innerpath != inner_cheapest_total ||
				 sortkeycnt < num_sortkeys) &&
				(cheapest_startup_inner == NULL ||
				 compare_path_costs(innerpath, cheapest_startup_inner,
									STARTUP_COST) < 0))
			{
				/* Found a cheap (or even-cheaper) sorted path */
				if (innerpath != cheapest_total_inner)
				{
					/*
					 * Avoid rebuilding clause list if we already made one;
					 * saves memory in big join trees...
					 */
					if (newclauses == NIL)
					{
						if (sortkeycnt < num_sortkeys)
						{
							newclauses =
								find_mergeclauses_for_pathkeys(root,
															   trialsortkeys,
															   false,
															   mergeclauses);
							Assert(newclauses != NIL);
						}
						else
							newclauses = mergeclauses;
					}
					add_path(joinrel, (Path *)
							 create_mergejoin_path(root,
												   joinrel,
												   jointype,
												   outerpath,
												   innerpath,
												   restrictlist,
												   merge_pathkeys,
												   newclauses,
												   NIL,
												   NIL));
				}
				cheapest_startup_inner = innerpath;
			}

			/*
			 * Don't consider truncated sortkeys if we need all clauses.
			 */
			if (useallclauses)
				break;
		}
	}
}

/*
 * hash_inner_and_outer
 *	  Create hashjoin join paths by explicitly hashing both the outer and
 *	  inner keys of each available hash clause.
 *
 * 'joinrel' is the join relation
 * 'outerrel' is the outer join relation
 * 'innerrel' is the inner join relation
 * 'restrictlist' contains all of the RestrictInfo nodes for restriction
 *		clauses that apply to this join
 * 'jointype' is the type of join to do
 */
static void
hash_inner_and_outer(PlannerInfo *root,
					 RelOptInfo *joinrel,
					 RelOptInfo *outerrel,
					 RelOptInfo *innerrel,
					 List *restrictlist,
					 JoinType jointype)
{
	bool		isouterjoin;
	List	   *hashclauses;
	ListCell   *l;

	/*
	 * Hashjoin only supports inner, left, and IN joins.
	 */
	switch (jointype)
	{
		case JOIN_INNER:
		case JOIN_IN:
		case JOIN_UNIQUE_OUTER:
		case JOIN_UNIQUE_INNER:
			isouterjoin = false;
			break;
		case JOIN_LEFT:
			isouterjoin = true;
			break;
		default:
			return;
	}

	/*
	 * We need to build only one hashpath for any given pair of outer and
	 * inner relations; all of the hashable clauses will be used as keys.
	 *
	 * Scan the join's restrictinfo list to find hashjoinable clauses that are
	 * usable with this pair of sub-relations.
	 */
	hashclauses = NIL;
	foreach(l, restrictlist)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(l);

		if (!restrictinfo->can_join ||
			restrictinfo->hashjoinoperator == InvalidOid)
			continue;			/* not hashjoinable */

		/*
		 * If processing an outer join, only use its own join clauses for
		 * hashing.  For inner joins we need not be so picky.
		 */
		if (isouterjoin && restrictinfo->is_pushed_down)
			continue;

		/*
		 * Check if clause is usable with these input rels.
		 */
		if (bms_is_subset(restrictinfo->left_relids, outerrel->relids) &&
			bms_is_subset(restrictinfo->right_relids, innerrel->relids))
		{
			/* righthand side is inner */
		}
		else if (bms_is_subset(restrictinfo->left_relids, innerrel->relids) &&
				 bms_is_subset(restrictinfo->right_relids, outerrel->relids))
		{
			/* lefthand side is inner */
		}
		else
			continue;			/* no good for these input relations */

		hashclauses = lappend(hashclauses, restrictinfo);
	}

	/* If we found any usable hashclauses, make a path */
	if (hashclauses)
	{
		/*
		 * We consider both the cheapest-total-cost and cheapest-startup-cost
		 * outer paths.  There's no need to consider any but the
		 * cheapest-total-cost inner path, however.
		 */
		Path	   *cheapest_startup_outer = outerrel->cheapest_startup_path;
		Path	   *cheapest_total_outer = outerrel->cheapest_total_path;
		Path	   *cheapest_total_inner = innerrel->cheapest_total_path;

		/* Unique-ify if need be */
		if (jointype == JOIN_UNIQUE_OUTER)
		{
			cheapest_total_outer = (Path *)
				create_unique_path(root, outerrel, cheapest_total_outer);
			cheapest_startup_outer = cheapest_total_outer;
			jointype = JOIN_INNER;
		}
		else if (jointype == JOIN_UNIQUE_INNER)
		{
			cheapest_total_inner = (Path *)
				create_unique_path(root, innerrel, cheapest_total_inner);
			jointype = JOIN_INNER;
		}

		add_path(joinrel, (Path *)
				 create_hashjoin_path(root,
									  joinrel,
									  jointype,
									  cheapest_total_outer,
									  cheapest_total_inner,
									  restrictlist,
									  hashclauses));
		if (cheapest_startup_outer != cheapest_total_outer)
			add_path(joinrel, (Path *)
					 create_hashjoin_path(root,
										  joinrel,
										  jointype,
										  cheapest_startup_outer,
										  cheapest_total_inner,
										  restrictlist,
										  hashclauses));
	}
}

/*
 * best_appendrel_indexscan
 *	  Finds the best available set of inner indexscans for a nestloop join
 *	  with the given append relation on the inside and the given outer_rel
 *	  outside.	Returns an AppendPath comprising the best inner scans, or
 *	  NULL if there are no possible inner indexscans.
 *
 * Note that we currently consider only cheapest-total-cost.  It's not
 * very clear what cheapest-startup-cost might mean for an AppendPath.
 */
static Path *
best_appendrel_indexscan(PlannerInfo *root, RelOptInfo *rel,
						 RelOptInfo *outer_rel, JoinType jointype)
{
	int			parentRTindex = rel->relid;
	List	   *append_paths = NIL;
	bool		found_indexscan = false;
	ListCell   *l;

	foreach(l, root->append_rel_list)
	{
		AppendRelInfo *appinfo = (AppendRelInfo *) lfirst(l);
		int			childRTindex;
		RelOptInfo *childrel;
		Path	   *index_cheapest_startup;
		Path	   *index_cheapest_total;

		/* append_rel_list contains all append rels; ignore others */
		if (appinfo->parent_relid != parentRTindex)
			continue;

		childRTindex = appinfo->child_relid;
		childrel = find_base_rel(root, childRTindex);
		Assert(childrel->reloptkind == RELOPT_OTHER_MEMBER_REL);

		/*
		 * Check to see if child was rejected by constraint exclusion. If so,
		 * it will have a cheapest_total_path that's an Append path with no
		 * members (see set_plain_rel_pathlist).
		 */
		if (IsA(childrel->cheapest_total_path, AppendPath) &&
			((AppendPath *) childrel->cheapest_total_path)->subpaths == NIL)
			continue;			/* OK, we can ignore it */

		/*
		 * Get the best innerjoin indexpaths (if any) for this child rel.
		 */
		best_inner_indexscan(root, childrel, outer_rel, jointype,
							 &index_cheapest_startup, &index_cheapest_total);

		/*
		 * If no luck on an indexpath for this rel, we'll still consider an
		 * Append substituting the cheapest-total inner path.  However we must
		 * find at least one indexpath, else there's not going to be any
		 * improvement over the base path for the appendrel.
		 */
		if (index_cheapest_total)
			found_indexscan = true;
		else
			index_cheapest_total = childrel->cheapest_total_path;

		append_paths = lappend(append_paths, index_cheapest_total);
	}

	if (!found_indexscan)
		return NULL;

	/* Form and return the completed Append path. */
	return (Path *) create_append_path(rel, append_paths);
}

/*
 * select_mergejoin_clauses
 *	  Select mergejoin clauses that are usable for a particular join.
 *	  Returns a list of RestrictInfo nodes for those clauses.
 *
 * We also mark each selected RestrictInfo to show which side is currently
 * being considered as outer.  These are transient markings that are only
 * good for the duration of the current add_paths_to_joinrel() call!
 *
 * We examine each restrictinfo clause known for the join to see
 * if it is mergejoinable and involves vars from the two sub-relations
 * currently of interest.
 */
static List *
select_mergejoin_clauses(PlannerInfo *root,
						 RelOptInfo *joinrel,
						 RelOptInfo *outerrel,
						 RelOptInfo *innerrel,
						 List *restrictlist,
						 JoinType jointype)
{
	List	   *result_list = NIL;
	bool		isouterjoin = IS_OUTER_JOIN(jointype);
	bool		have_nonmergeable_joinclause = false;
	ListCell   *l;

	foreach(l, restrictlist)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(l);

		/*
		 * If processing an outer join, only use its own join clauses in the
		 * merge.  For inner joins we can use pushed-down clauses too. (Note:
		 * we don't set have_nonmergeable_joinclause here because pushed-down
		 * clauses will become otherquals not joinquals.)
		 */
		if (isouterjoin && restrictinfo->is_pushed_down)
			continue;

		if (!restrictinfo->can_join ||
			restrictinfo->mergeopfamilies == NIL)
		{
			have_nonmergeable_joinclause = true;
			continue;			/* not mergejoinable */
		}

		/*
		 * Check if clause is usable with these input rels.  All the vars
		 * needed on each side of the clause must be available from one or the
		 * other of the input rels.
		 */
		if (bms_is_subset(restrictinfo->left_relids, outerrel->relids) &&
			bms_is_subset(restrictinfo->right_relids, innerrel->relids))
		{
			/* righthand side is inner */
			restrictinfo->outer_is_left = true;
		}
		else if (bms_is_subset(restrictinfo->left_relids, innerrel->relids) &&
				 bms_is_subset(restrictinfo->right_relids, outerrel->relids))
		{
			/* lefthand side is inner */
			restrictinfo->outer_is_left = false;
		}
		else
		{
			have_nonmergeable_joinclause = true;
			continue;			/* no good for these input relations */
		}

		/*
		 * Insist that each side have a non-redundant eclass.  This
		 * restriction is needed because various bits of the planner expect
		 * that each clause in a merge be associatable with some pathkey in a
		 * canonical pathkey list, but redundant eclasses can't appear in
		 * canonical sort orderings.  (XXX it might be worth relaxing this,
		 * but not enough time to address it for 8.3.)
		 *
		 * Note: it would be bad if this condition failed for an otherwise
		 * mergejoinable FULL JOIN clause, since that would result in
		 * undesirable planner failure.  I believe that is not possible
		 * however; a variable involved in a full join could only appear
		 * in below_outer_join eclasses, which aren't considered redundant.
		 *
		 * This case *can* happen for left/right join clauses: the
		 * outer-side variable could be equated to a constant.  Because we
		 * will propagate that constant across the join clause, the loss of
		 * ability to do a mergejoin is not really all that big a deal, and
		 * so it's not clear that improving this is important.
		 */
		cache_mergeclause_eclasses(root, restrictinfo);

		if (EC_MUST_BE_REDUNDANT(restrictinfo->left_ec) ||
			EC_MUST_BE_REDUNDANT(restrictinfo->right_ec))
		{
			have_nonmergeable_joinclause = true;
			continue;			/* can't handle redundant eclasses */
		}

		result_list = lappend(result_list, restrictinfo);
	}

	/*
	 * If it is a right/full join then *all* the explicit join clauses must be
	 * mergejoinable, else the executor will fail. If we are asked for a right
	 * join then just return NIL to indicate no mergejoin is possible (we can
	 * handle it as a left join instead). If we are asked for a full join then
	 * emit an error, because there is no fallback.
	 */
	if (have_nonmergeable_joinclause)
	{
		switch (jointype)
		{
			case JOIN_RIGHT:
				return NIL;		/* not mergejoinable */
			case JOIN_FULL:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("FULL JOIN is only supported with merge-joinable join conditions")));
				break;
			default:
				/* otherwise, it's OK to have nonmergeable join quals */
				break;
		}
	}

	return result_list;
}
