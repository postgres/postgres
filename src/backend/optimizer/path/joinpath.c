/*-------------------------------------------------------------------------
 *
 * joinpath.c
 *	  Routines to find all possible paths for processing a set of joins
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/path/joinpath.c,v 1.48 2000/01/22 23:50:15 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>
#include <math.h>

#include "postgres.h"

#include "access/htup.h"
#include "catalog/pg_attribute.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"

static Path *best_innerjoin(List *join_paths, List *outer_relid);
static List *sort_inner_and_outer(RelOptInfo *joinrel,
								  RelOptInfo *outerrel,
								  RelOptInfo *innerrel,
								  List *mergeclause_list);
static List *match_unsorted_outer(RelOptInfo *joinrel, RelOptInfo *outerrel,
								  RelOptInfo *innerrel, List *outerpath_list,
								  Path *cheapest_inner, Path *best_innerjoin,
								  List *mergeclause_list);
static List *match_unsorted_inner(RelOptInfo *joinrel, RelOptInfo *outerrel,
								  RelOptInfo *innerrel, List *innerpath_list,
								  List *mergeclause_list);
static List *hash_inner_and_outer(Query *root, RelOptInfo *joinrel,
								  RelOptInfo *outerrel, RelOptInfo *innerrel);
static Selectivity estimate_disbursion(Query *root, Var *var);
static List *select_mergejoin_clauses(List *restrictinfo_list);

/*
 * update_rels_pathlist_for_joins
 *	  Creates all possible ways to process joins for each of the join
 *	  relations in the list 'joinrels.'  Each unique path will be included
 *	  in the join relation's 'pathlist' field.
 *
 * 'joinrels' is the list of relation entries to be joined
 *
 * Modifies the pathlist field of each joinrel node to contain
 * the unique join paths.
 */
void
update_rels_pathlist_for_joins(Query *root, List *joinrels)
{
	List	   *j;

	foreach(j, joinrels)
	{
		RelOptInfo *joinrel = (RelOptInfo *) lfirst(j);
		Relids		innerrelids;
		Relids		outerrelids;
		RelOptInfo *innerrel;
		RelOptInfo *outerrel;
		Path	   *bestinnerjoin;
		List	   *pathlist;
		List	   *mergeclause_list = NIL;

		/*
		 * On entry, joinrel->relids is a list of two sublists of relids,
		 * namely the outer and inner member relids.  Extract these sublists
		 * and change joinrel->relids to a flattened single list.
		 * (Use listCopy so as not to damage the member lists...)
		 */
		outerrelids = lfirst(joinrel->relids);
		innerrelids = lsecond(joinrel->relids);

		joinrel->relids = nconc(listCopy(outerrelids),
								listCopy(innerrelids));

		/*
		 * Get the corresponding RelOptInfos for the outer and inner sides.
		 * Base relation id is an integer and join relation relid is a
		 * list of integers.
		 */
		innerrel = (length(innerrelids) == 1) ?
			get_base_rel(root, lfirsti(innerrelids)) :
			get_join_rel(root, innerrelids);
		outerrel = (length(outerrelids) == 1) ?
			get_base_rel(root, lfirsti(outerrelids)) :
			get_join_rel(root, outerrelids);

		/*
		 * Get the best inner join for match_unsorted_outer().
		 */
		bestinnerjoin = best_innerjoin(innerrel->innerjoin, outerrel->relids);

		/*
		 * Find potential mergejoin clauses.
		 */
		if (enable_mergejoin)
			mergeclause_list = select_mergejoin_clauses(joinrel->restrictinfo);

		/*
		 * 1. Consider mergejoin paths where both relations must be
		 * explicitly sorted.
		 */
		pathlist = sort_inner_and_outer(joinrel, outerrel,
										innerrel, mergeclause_list);

		/*
		 * 2. Consider paths where the outer relation need not be
		 * explicitly sorted. This includes both nestloops and
		 * mergejoins where the outer path is already ordered.
		 */
		pathlist = add_pathlist(joinrel, pathlist,
								match_unsorted_outer(joinrel,
													 outerrel,
													 innerrel,
													 outerrel->pathlist,
													 innerrel->cheapestpath,
													 bestinnerjoin,
													 mergeclause_list));

		/*
		 * 3. Consider paths where the inner relation need not be
		 * explicitly sorted.  This includes mergejoins only
		 * (nestloops were already built in match_unsorted_outer).
		 */
		pathlist = add_pathlist(joinrel, pathlist,
								match_unsorted_inner(joinrel, outerrel,
													 innerrel,
													 innerrel->pathlist,
													 mergeclause_list));

		/*
		 * 4. Consider paths where both outer and inner relations must be
		 * hashed before being joined.
		 */
		if (enable_hashjoin)
			pathlist = add_pathlist(joinrel, pathlist,
									hash_inner_and_outer(root, joinrel,
														 outerrel,
														 innerrel));

		/* Save the completed pathlist in the join rel */
		joinrel->pathlist = pathlist;
	}
}

/*
 * best_innerjoin
 *	  Find the cheapest index path that has already been identified by
 *	  indexable_joinclauses() as being a possible inner path for the given
 *	  outer relation(s) in a nestloop join.
 *
 * 'join_paths' is a list of potential inner indexscan join paths
 * 'outer_relids' is the relid list of the outer join relation
 *
 * Returns the pathnode of the best path, or NULL if there's no
 * usable path.
 */
static Path *
best_innerjoin(List *join_paths, Relids outer_relids)
{
	Path	   *cheapest = (Path *) NULL;
	List	   *join_path;

	foreach(join_path, join_paths)
	{
		Path	   *path = (Path *) lfirst(join_path);

		Assert(IsA(path, IndexPath));

		/* path->joinrelids is the set of base rels that must be part of
		 * outer_relids in order to use this inner path, because those
		 * rels are used in the index join quals of this inner path.
		 */
		if (is_subset(((IndexPath *) path)->joinrelids, outer_relids) &&
			(cheapest == NULL ||
			 path_is_cheaper(path, cheapest)))
			cheapest = path;
	}
	return cheapest;
}

/*
 * sort_inner_and_outer
 *	  Create mergejoin join paths by explicitly sorting both the outer and
 *	  inner join relations on each available merge ordering.
 *
 * 'joinrel' is the join relation
 * 'outerrel' is the outer join relation
 * 'innerrel' is the inner join relation
 * 'mergeclause_list' is a list of RestrictInfo nodes for available
 *				mergejoin clauses between these two relations
 *
 * Returns a list of mergejoin paths.
 */
static List *
sort_inner_and_outer(RelOptInfo *joinrel,
					 RelOptInfo *outerrel,
					 RelOptInfo *innerrel,
					 List *mergeclause_list)
{
	List	   *path_list = NIL;
	List	   *i;

	/*
	 * Each possible ordering of the available mergejoin clauses will
	 * generate a differently-sorted result path at essentially the
	 * same cost.  We have no basis for choosing one over another at
	 * this level of joining, but some sort orders may be more useful
	 * than others for higher-level mergejoins.  Generating a path here
	 * for *every* permutation of mergejoin clauses doesn't seem like
	 * a winning strategy, however; the cost in planning time is too high.
	 *
	 * For now, we generate one path for each mergejoin clause, listing that
	 * clause first and the rest in random order.  This should allow at least
	 * a one-clause mergejoin without re-sorting against any other possible
	 * mergejoin partner path.  But if we've not guessed the right ordering
	 * of secondary clauses, we may end up evaluating clauses as qpquals when
	 * they could have been done as mergeclauses.  We need to figure out a
	 * better way.  (Two possible approaches: look at all the relevant index
	 * relations to suggest plausible sort orders, or make just one output
	 * path and somehow mark it as having a sort-order that can be rearranged
	 * freely.)
	 */
	foreach(i, mergeclause_list)
	{
		RestrictInfo   *restrictinfo = lfirst(i);
		List		   *curclause_list;
		List		   *outerkeys;
		List		   *innerkeys;
		List		   *merge_pathkeys;
		MergePath	   *path_node;

		/* Make a mergeclause list with this guy first. */
		curclause_list = lcons(restrictinfo,
							   lremove(restrictinfo,
									   listCopy(mergeclause_list)));
		/* Build sort pathkeys for both sides.
		 *
		 * Note: it's possible that the cheapest path will already be
		 * sorted properly --- create_mergejoin_path will detect that case
		 * and suppress an explicit sort step.
		 */
		outerkeys = make_pathkeys_for_mergeclauses(curclause_list,
												   outerrel->targetlist);
		innerkeys = make_pathkeys_for_mergeclauses(curclause_list,
												   innerrel->targetlist);
		/* Build pathkeys representing output sort order. */
		merge_pathkeys = build_join_pathkeys(outerkeys, joinrel->targetlist,
											 curclause_list);
		/* And now we can make the path. */
		path_node = create_mergejoin_path(joinrel,
										  outerrel->cheapestpath,
										  innerrel->cheapestpath,
										  merge_pathkeys,
										  get_actual_clauses(curclause_list),
										  outerkeys,
										  innerkeys);

		path_list = lappend(path_list, path_node);
	}
	return path_list;
}

/*
 * match_unsorted_outer
 *	  Creates possible join paths for processing a single join relation
 *	  'joinrel' by employing either iterative substitution or
 *	  mergejoining on each of its possible outer paths (considering
 *	  only outer paths that are already ordered well enough for merging).
 *
 * We always generate a nestloop path for each available outer path.
 * If an indexscan inner path exists that is compatible with this outer rel
 * and cheaper than the cheapest general-purpose inner path, then we use
 * the indexscan inner path; else we use the cheapest general-purpose inner.
 *
 * We also consider mergejoins if mergejoin clauses are available.  We have
 * two ways to generate the inner path for a mergejoin: use the cheapest
 * inner path (sorting it if it's not suitably ordered already), or using an
 * inner path that is already suitably ordered for the merge.  If the
 * cheapest inner path is suitably ordered, then by definition it's the one
 * to use.  Otherwise, we look for ordered paths that are cheaper than the
 * cheapest inner + sort costs.  If we have several mergeclauses, it could be
 * that there is no inner path (or only a very expensive one) for the full
 * list of mergeclauses, but better paths exist if we truncate the
 * mergeclause list (thereby discarding some sort key requirements).  So, we
 * consider truncations of the mergeclause list as well as the full list.
 * In any case, we find the cheapest suitable path and generate a single
 * output mergejoin path.  (Since all the possible mergejoins will have
 * identical output pathkeys, there is no need to keep any but the cheapest.)
 *
 * 'joinrel' is the join relation
 * 'outerrel' is the outer join relation
 * 'innerrel' is the inner join relation
 * 'outerpath_list' is the list of possible outer paths
 * 'cheapest_inner' is the cheapest inner path
 * 'best_innerjoin' is the best inner index path (if any)
 * 'mergeclause_list' is a list of RestrictInfo nodes for available
 *				mergejoin clauses between these two relations
 *
 * Returns a list of possible join path nodes.
 */
static List *
match_unsorted_outer(RelOptInfo *joinrel,
					 RelOptInfo *outerrel,
					 RelOptInfo *innerrel,
					 List *outerpath_list,
					 Path *cheapest_inner,
					 Path *best_innerjoin,
					 List *mergeclause_list)
{
	List	   *path_list = NIL;
	Path	   *nestinnerpath;
	List	   *i;

	/*
	 * We only use the best innerjoin indexpath if it is cheaper
	 * than the cheapest general-purpose inner path.
	 */
	if (best_innerjoin &&
		path_is_cheaper(best_innerjoin, cheapest_inner))
		nestinnerpath = best_innerjoin;
	else
		nestinnerpath = cheapest_inner;

	foreach(i, outerpath_list)
	{
		Path	   *outerpath = (Path *) lfirst(i);
		List	   *mergeclauses;
		List	   *merge_pathkeys;
		List	   *innersortkeys;
		Path	   *mergeinnerpath;
		int			mergeclausecount;

		/* Look for useful mergeclauses (if any) */
		mergeclauses = find_mergeclauses_for_pathkeys(outerpath->pathkeys,
													  mergeclause_list);
		/*
		 * The result will have this sort order (even if it is implemented
		 * as a nestloop, and even if some of the mergeclauses are implemented
		 * by qpquals rather than as true mergeclauses):
		 */
		merge_pathkeys = build_join_pathkeys(outerpath->pathkeys,
											 joinrel->targetlist,
											 mergeclauses);

		/* Always consider a nestloop join with this outer and best inner. */
		path_list = lappend(path_list,
							create_nestloop_path(joinrel,
												 outerpath,
												 nestinnerpath,
												 merge_pathkeys));

		/* Done with this outer path if no chance for a mergejoin */
		if (mergeclauses == NIL)
			continue;

		/* Compute the required ordering of the inner path */
		innersortkeys = make_pathkeys_for_mergeclauses(mergeclauses,
													   innerrel->targetlist);

		/* Set up on the assumption that we will use the cheapest_inner */
		mergeinnerpath = cheapest_inner;
		mergeclausecount = length(mergeclauses);

		/* If the cheapest_inner doesn't need to be sorted, it is the winner
		 * by definition.
		 */
		if (pathkeys_contained_in(innersortkeys,
								  cheapest_inner->pathkeys))
		{
			/* cheapest_inner is the winner */
			innersortkeys = NIL; /* we do not need to sort it... */
		}
		else
		{
			/* look for a presorted path that's cheaper */
			List	   *trialsortkeys = listCopy(innersortkeys);
			Cost		cheapest_cost;
			int			clausecount;

			cheapest_cost = cheapest_inner->path_cost +
				cost_sort(innersortkeys, innerrel->rows, innerrel->width);

			for (clausecount = mergeclausecount;
				 clausecount > 0;
				 clausecount--)
			{
				Path	   *trialinnerpath;

				/* Look for an inner path ordered well enough to merge with
				 * the first 'clausecount' mergeclauses.  NB: trialsortkeys
				 * is modified destructively, which is why we made a copy...
				 */
				trialinnerpath =
					get_cheapest_path_for_pathkeys(innerrel->pathlist,
												   ltruncate(clausecount,
															 trialsortkeys),
												   false);
				if (trialinnerpath != NULL &&
					trialinnerpath->path_cost < cheapest_cost)
				{
					/* Found a cheaper (or even-cheaper) sorted path */
					cheapest_cost = trialinnerpath->path_cost;
					mergeinnerpath = trialinnerpath;
					mergeclausecount = clausecount;
					innersortkeys = NIL; /* we will not need to sort it... */
				}
			}
		}

		/* Finally, we can build the mergejoin path */
		mergeclauses = ltruncate(mergeclausecount,
								 get_actual_clauses(mergeclauses));
		path_list = lappend(path_list,
							create_mergejoin_path(joinrel,
												  outerpath,
												  mergeinnerpath,
												  merge_pathkeys,
												  mergeclauses,
												  NIL,
												  innersortkeys));
	}

	return path_list;
}

/*
 * match_unsorted_inner
 *	  Generate mergejoin paths that use an explicit sort of the outer path
 *	  with an already-ordered inner path.
 *
 * 'joinrel' is the join result relation
 * 'outerrel' is the outer join relation
 * 'innerrel' is the inner join relation
 * 'innerpath_list' is the list of possible inner join paths
 * 'mergeclause_list' is a list of RestrictInfo nodes for available
 *				mergejoin clauses between these two relations
 *
 * Returns a list of possible merge paths.
 */
static List *
match_unsorted_inner(RelOptInfo *joinrel,
					 RelOptInfo *outerrel,
					 RelOptInfo *innerrel,
					 List *innerpath_list,
					 List *mergeclause_list)
{
	List	   *path_list = NIL;
	List	   *i;

	foreach(i, innerpath_list)
	{
		Path	   *innerpath = (Path *) lfirst(i);
		List	   *mergeclauses;

		/* Look for useful mergeclauses (if any) */
		mergeclauses = find_mergeclauses_for_pathkeys(innerpath->pathkeys,
													  mergeclause_list);

		if (mergeclauses)
		{
			List	   *outersortkeys;
			Path	   *mergeouterpath;
			List	   *merge_pathkeys;

			/* Compute the required ordering of the outer path */
			outersortkeys =
				make_pathkeys_for_mergeclauses(mergeclauses,
											   outerrel->targetlist);

			/* Look for an outer path already ordered well enough to merge */
			mergeouterpath =
				get_cheapest_path_for_pathkeys(outerrel->pathlist,
											   outersortkeys,
											   false);

			/* Should we use the mergeouter, or sort the cheapest outer? */
			if (mergeouterpath != NULL &&
				mergeouterpath->path_cost <=
				(outerrel->cheapestpath->path_cost +
				 cost_sort(outersortkeys, outerrel->rows, outerrel->width)))
			{
				/* Use mergeouterpath */
				outersortkeys = NIL;	/* no explicit sort step */
			}
			else
			{
				/* Use outerrel->cheapestpath, with the outersortkeys */
				mergeouterpath = outerrel->cheapestpath;
			}

			/* Compute pathkeys the result will have */
			merge_pathkeys = build_join_pathkeys(
				outersortkeys ? outersortkeys : mergeouterpath->pathkeys,
				joinrel->targetlist,
				mergeclauses);

			mergeclauses = get_actual_clauses(mergeclauses);
			path_list = lappend(path_list,
								create_mergejoin_path(joinrel,
													  mergeouterpath,
													  innerpath,
													  merge_pathkeys,
													  mergeclauses,
													  outersortkeys,
													  NIL));
		}
	}

	return path_list;
}

/*
 * hash_inner_and_outer
 *	  Create hashjoin join paths by explicitly hashing both the outer and
 *	  inner join relations of each available hash clause.
 *
 * 'joinrel' is the join relation
 * 'outerrel' is the outer join relation
 * 'innerrel' is the inner join relation
 *
 * Returns a list of hashjoin paths.
 */
static List *
hash_inner_and_outer(Query *root,
					 RelOptInfo *joinrel,
					 RelOptInfo *outerrel,
					 RelOptInfo *innerrel)
{
	List	   *hpath_list = NIL;
	List	   *i;

	foreach(i, joinrel->restrictinfo)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(i);

		/* we consider only clauses previously marked hashjoinable */
		if (restrictinfo->hashjoinoperator)
		{
			Expr	   *clause = restrictinfo->clause;
			Var		   *leftop = get_leftop(clause);
			Var		   *rightop = get_rightop(clause);
			Var		   *innerop;
			Selectivity	innerdisbursion;
			HashPath   *hash_path;

			/* find the inner var and estimate its disbursion */
			if (intMember(leftop->varno, innerrel->relids))
				innerop = leftop;
			else
				innerop = rightop;
			innerdisbursion = estimate_disbursion(root, innerop);

			hash_path = create_hashjoin_path(joinrel,
											 outerrel->cheapestpath,
											 innerrel->cheapestpath,
											 lcons(clause, NIL),
											 innerdisbursion);
			hpath_list = lappend(hpath_list, hash_path);
		}
	}

	return hpath_list;
}

/*
 * Estimate disbursion of the specified Var
 *
 * We use a default of 0.1 if we can't figure out anything better.
 * This will typically discourage use of a hash rather strongly,
 * if the inner relation is large.  We do not want to hash unless
 * we know that the inner rel is well-dispersed (or the alternatives
 * seem much worse).
 */
static Selectivity
estimate_disbursion(Query *root, Var *var)
{
	Oid			relid;

	if (! IsA(var, Var))
		return 0.1;

	relid = getrelid(var->varno, root->rtable);

	return (Selectivity) get_attdisbursion(relid, var->varattno, 0.1);
}

/*
 * select_mergejoin_clauses
 *	  Select mergejoin clauses that are usable for a particular join.
 *	  Returns a list of RestrictInfo nodes for those clauses.
 *
 * Currently, all we need is the restrictinfo list of the joinrel.
 * By definition, any mergejoinable clause in that list will work ---
 * it must involve only vars in the join, or it wouldn't have been
 * in the restrict list, and it must involve vars on both sides of
 * the join, or it wouldn't have made it up to this level of join.
 * Since we currently allow only simple Vars as the left and right
 * sides of mergejoin clauses, that means the mergejoin clauses must
 * be usable for this join.  If we ever allow more complex expressions
 * containing multiple Vars, we would need to check that each side
 * of a potential joinclause uses only vars from one side of the join.
 */
static List *
select_mergejoin_clauses(List *restrictinfo_list)
{
	List	   *result_list = NIL;
	List	   *i;

	foreach(i, restrictinfo_list)
	{
		RestrictInfo   *restrictinfo = lfirst(i);

		if (restrictinfo->mergejoinoperator != InvalidOid)
			result_list = lcons(restrictinfo, result_list);
	}

	return result_list;
}
