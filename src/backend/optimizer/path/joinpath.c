/*-------------------------------------------------------------------------
 *
 * joinpath.c
 *	  Routines to find all possible paths for processing a set of joins
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/path/joinpath.c,v 1.57 2000/09/29 18:21:32 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>
#include <math.h>

#include "postgres.h"

#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"

static void sort_inner_and_outer(Query *root, RelOptInfo *joinrel,
								 RelOptInfo *outerrel, RelOptInfo *innerrel,
								 List *restrictlist, List *mergeclause_list,
								 JoinType jointype);
static void match_unsorted_outer(Query *root, RelOptInfo *joinrel,
								 RelOptInfo *outerrel, RelOptInfo *innerrel,
								 List *restrictlist, List *mergeclause_list,
								 JoinType jointype);

#ifdef NOT_USED
static void match_unsorted_inner(Query *root, RelOptInfo *joinrel,
								 RelOptInfo *outerrel, RelOptInfo *innerrel,
								 List *restrictlist, List *mergeclause_list,
								 JoinType jointype);

#endif
static void hash_inner_and_outer(Query *root, RelOptInfo *joinrel,
								 RelOptInfo *outerrel, RelOptInfo *innerrel,
								 List *restrictlist, JoinType jointype);
static Path *best_innerjoin(List *join_paths, List *outer_relid,
							JoinType jointype);
static Selectivity estimate_disbursion(Query *root, Var *var);
static List *select_mergejoin_clauses(RelOptInfo *joinrel,
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
add_paths_to_joinrel(Query *root,
					 RelOptInfo *joinrel,
					 RelOptInfo *outerrel,
					 RelOptInfo *innerrel,
					 JoinType jointype,
					 List *restrictlist)
{
	List	   *mergeclause_list = NIL;

	/*
	 * Find potential mergejoin clauses.
	 */
	if (enable_mergejoin)
		mergeclause_list = select_mergejoin_clauses(joinrel,
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
	 * sorted.	This includes mergejoins only (nestloops were already
	 * built in match_unsorted_outer).
	 *
	 * Diked out as redundant 2/13/2000 -- tgl.  There isn't any really
	 * significant difference between the inner and outer side of a
	 * mergejoin, so match_unsorted_inner creates no paths that aren't
	 * equivalent to those made by match_unsorted_outer when
	 * add_paths_to_joinrel() is invoked with the two rels given in the
	 * other order.
	 */
	match_unsorted_inner(root, joinrel, outerrel, innerrel,
						 restrictlist, mergeclause_list, jointype);
#endif

	/*
	 * 4. Consider paths where both outer and inner relations must be
	 * hashed before being joined.
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
sort_inner_and_outer(Query *root,
					 RelOptInfo *joinrel,
					 RelOptInfo *outerrel,
					 RelOptInfo *innerrel,
					 List *restrictlist,
					 List *mergeclause_list,
					 JoinType jointype)
{
	List	   *i;

	/*
	 * Each possible ordering of the available mergejoin clauses will
	 * generate a differently-sorted result path at essentially the same
	 * cost.  We have no basis for choosing one over another at this level
	 * of joining, but some sort orders may be more useful than others for
	 * higher-level mergejoins.  Generating a path here for *every*
	 * permutation of mergejoin clauses doesn't seem like a winning
	 * strategy, however; the cost in planning time is too high.
	 *
	 * For now, we generate one path for each mergejoin clause, listing that
	 * clause first and the rest in random order.  This should allow at
	 * least a one-clause mergejoin without re-sorting against any other
	 * possible mergejoin partner path.  But if we've not guessed the
	 * right ordering of secondary clauses, we may end up evaluating
	 * clauses as qpquals when they could have been done as mergeclauses.
	 * We need to figure out a better way.	(Two possible approaches: look
	 * at all the relevant index relations to suggest plausible sort
	 * orders, or make just one output path and somehow mark it as having
	 * a sort-order that can be rearranged freely.)
	 */
	foreach(i, mergeclause_list)
	{
		RestrictInfo *restrictinfo = lfirst(i);
		List	   *curclause_list;
		List	   *outerkeys;
		List	   *innerkeys;
		List	   *merge_pathkeys;

		/* Make a mergeclause list with this guy first. */
		if (i != mergeclause_list)
			curclause_list = lcons(restrictinfo,
								   lremove(restrictinfo,
										   listCopy(mergeclause_list)));
		else
			curclause_list = mergeclause_list;	/* no work at first one... */

		/*
		 * Build sort pathkeys for both sides.
		 *
		 * Note: it's possible that the cheapest paths will already be sorted
		 * properly.  create_mergejoin_path will detect that case and
		 * suppress an explicit sort step, so we needn't do so here.
		 */
		outerkeys = make_pathkeys_for_mergeclauses(root,
												   curclause_list,
												   outerrel);
		innerkeys = make_pathkeys_for_mergeclauses(root,
												   curclause_list,
												   innerrel);
		/* Build pathkeys representing output sort order. */
		merge_pathkeys = build_join_pathkeys(outerkeys,
											 joinrel->targetlist,
											 root->equi_key_list);

		/*
		 * And now we can make the path.  We only consider the cheapest-
		 * total-cost input paths, since we are assuming here that a sort
		 * is required.  We will consider cheapest-startup-cost input
		 * paths later, and only if they don't need a sort.
		 */
		add_path(joinrel, (Path *)
				 create_mergejoin_path(joinrel,
									   jointype,
									   outerrel->cheapest_total_path,
									   innerrel->cheapest_total_path,
									   restrictlist,
									   merge_pathkeys,
									   curclause_list,
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
 * In fact we may generate as many as three: one on the cheapest-total-cost
 * inner path, one on the cheapest-startup-cost inner path (if different),
 * and one on the best inner-indexscan path (if any).
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
match_unsorted_outer(Query *root,
					 RelOptInfo *joinrel,
					 RelOptInfo *outerrel,
					 RelOptInfo *innerrel,
					 List *restrictlist,
					 List *mergeclause_list,
					 JoinType jointype)
{
	bool		nestjoinOK;
	Path	   *bestinnerjoin;
	List	   *i;

	/*
	 * Nestloop only supports inner and left joins.
	 */
	switch (jointype)
	{
		case JOIN_INNER:
		case JOIN_LEFT:
			nestjoinOK = true;
			break;
		default:
			nestjoinOK = false;
			break;
	}

	/*
	 * Get the best innerjoin indexpath (if any) for this outer rel. It's
	 * the same for all outer paths.
	 */
	bestinnerjoin = best_innerjoin(innerrel->innerjoin, outerrel->relids,
								   jointype);

	foreach(i, outerrel->pathlist)
	{
		Path	   *outerpath = (Path *) lfirst(i);
		List	   *merge_pathkeys;
		List	   *mergeclauses;
		List	   *innersortkeys;
		List	   *trialsortkeys;
		Path	   *cheapest_startup_inner;
		Path	   *cheapest_total_inner;
		int			num_mergeclauses;
		int			clausecnt;

		/*
		 * The result will have this sort order (even if it is implemented
		 * as a nestloop, and even if some of the mergeclauses are
		 * implemented by qpquals rather than as true mergeclauses):
		 */
		merge_pathkeys = build_join_pathkeys(outerpath->pathkeys,
											 joinrel->targetlist,
											 root->equi_key_list);

		if (nestjoinOK)
		{
			/*
			 * Always consider a nestloop join with this outer and cheapest-
			 * total-cost inner.  Consider nestloops using the cheapest-
			 * startup-cost inner as well, and the best innerjoin indexpath.
			 */
			add_path(joinrel, (Path *)
					 create_nestloop_path(joinrel,
										  jointype,
										  outerpath,
										  innerrel->cheapest_total_path,
										  restrictlist,
										  merge_pathkeys));
			if (innerrel->cheapest_startup_path !=
				innerrel->cheapest_total_path)
				add_path(joinrel, (Path *)
						 create_nestloop_path(joinrel,
											  jointype,
											  outerpath,
											  innerrel->cheapest_startup_path,
											  restrictlist,
											  merge_pathkeys));
			if (bestinnerjoin != NULL)
				add_path(joinrel, (Path *)
						 create_nestloop_path(joinrel,
											  jointype,
											  outerpath,
											  bestinnerjoin,
											  restrictlist,
											  merge_pathkeys));
		}

		/* Look for useful mergeclauses (if any) */
		mergeclauses = find_mergeclauses_for_pathkeys(outerpath->pathkeys,
													  mergeclause_list);

		/* Done with this outer path if no chance for a mergejoin */
		if (mergeclauses == NIL)
			continue;

		/* Compute the required ordering of the inner path */
		innersortkeys = make_pathkeys_for_mergeclauses(root,
													   mergeclauses,
													   innerrel);

		/*
		 * Generate a mergejoin on the basis of sorting the cheapest
		 * inner. Since a sort will be needed, only cheapest total cost
		 * matters.
		 */
		add_path(joinrel, (Path *)
				 create_mergejoin_path(joinrel,
									   jointype,
									   outerpath,
									   innerrel->cheapest_total_path,
									   restrictlist,
									   merge_pathkeys,
									   mergeclauses,
									   NIL,
									   innersortkeys));

		/*
		 * Look for presorted inner paths that satisfy the mergeclause
		 * list or any truncation thereof.	Here, we consider both cheap
		 * startup cost and cheap total cost.
		 */
		trialsortkeys = listCopy(innersortkeys);		/* modifiable copy */
		cheapest_startup_inner = NULL;
		cheapest_total_inner = NULL;
		num_mergeclauses = length(mergeclauses);

		for (clausecnt = num_mergeclauses; clausecnt > 0; clausecnt--)
		{
			Path	   *innerpath;
			List	   *newclauses = NIL;

			/*
			 * Look for an inner path ordered well enough to merge with
			 * the first 'clausecnt' mergeclauses.	NB: trialsortkeys list
			 * is modified destructively, which is why we made a copy...
			 */
			trialsortkeys = ltruncate(clausecnt, trialsortkeys);
			innerpath = get_cheapest_path_for_pathkeys(innerrel->pathlist,
													   trialsortkeys,
													   TOTAL_COST);
			if (innerpath != NULL &&
				(cheapest_total_inner == NULL ||
				 compare_path_costs(innerpath, cheapest_total_inner,
									TOTAL_COST) < 0))
			{
				/* Found a cheap (or even-cheaper) sorted path */
				if (clausecnt < num_mergeclauses)
					newclauses = ltruncate(clausecnt,
										   listCopy(mergeclauses));
				else
					newclauses = mergeclauses;
				add_path(joinrel, (Path *)
						 create_mergejoin_path(joinrel,
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
				(cheapest_startup_inner == NULL ||
				 compare_path_costs(innerpath, cheapest_startup_inner,
									STARTUP_COST) < 0))
			{
				/* Found a cheap (or even-cheaper) sorted path */
				if (innerpath != cheapest_total_inner)
				{

					/*
					 * Avoid rebuilding clause list if we already made
					 * one; saves memory in big join trees...
					 */
					if (newclauses == NIL)
					{
						if (clausecnt < num_mergeclauses)
							newclauses = ltruncate(clausecnt,
												 listCopy(mergeclauses));
						else
							newclauses = mergeclauses;
					}
					add_path(joinrel, (Path *)
							 create_mergejoin_path(joinrel,
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
		}
	}
}

#ifdef NOT_USED

/*
 * match_unsorted_inner
 *	  Generate mergejoin paths that use an explicit sort of the outer path
 *	  with an already-ordered inner path.
 *
 * 'joinrel' is the join result relation
 * 'outerrel' is the outer join relation
 * 'innerrel' is the inner join relation
 * 'restrictlist' contains all of the RestrictInfo nodes for restriction
 *		clauses that apply to this join
 * 'mergeclause_list' is a list of RestrictInfo nodes for available
 *		mergejoin clauses in this join
 * 'jointype' is the type of join to do
 */
static void
match_unsorted_inner(Query *root,
					 RelOptInfo *joinrel,
					 RelOptInfo *outerrel,
					 RelOptInfo *innerrel,
					 List *restrictlist,
					 List *mergeclause_list,
					 JoinType jointype)
{
	List	   *i;

	foreach(i, innerrel->pathlist)
	{
		Path	   *innerpath = (Path *) lfirst(i);
		List	   *mergeclauses;
		List	   *outersortkeys;
		List	   *merge_pathkeys;
		Path	   *totalouterpath;
		Path	   *startupouterpath;

		/* Look for useful mergeclauses (if any) */
		mergeclauses = find_mergeclauses_for_pathkeys(innerpath->pathkeys,
													  mergeclause_list);
		if (mergeclauses == NIL)
			continue;

		/* Compute the required ordering of the outer path */
		outersortkeys = make_pathkeys_for_mergeclauses(root,
													   mergeclauses,
													   outerrel);

		/*
		 * Generate a mergejoin on the basis of sorting the cheapest
		 * outer. Since a sort will be needed, only cheapest total cost
		 * matters.
		 */
		merge_pathkeys = build_join_pathkeys(outersortkeys,
											 joinrel->targetlist,
											 root->equi_key_list);
		add_path(joinrel, (Path *)
				 create_mergejoin_path(joinrel,
									   jointype,
									   outerrel->cheapest_total_path,
									   innerpath,
									   restrictlist,
									   merge_pathkeys,
									   mergeclauses,
									   outersortkeys,
									   NIL));

		/*
		 * Now generate mergejoins based on already-sufficiently-ordered
		 * outer paths.  There's likely to be some redundancy here with
		 * paths already generated by merge_unsorted_outer ... but since
		 * merge_unsorted_outer doesn't consider all permutations of the
		 * mergeclause list, it may fail to notice that this particular
		 * innerpath could have been used with this outerpath.
		 */
		totalouterpath = get_cheapest_path_for_pathkeys(outerrel->pathlist,
														outersortkeys,
														TOTAL_COST);
		if (totalouterpath == NULL)
			continue;			/* there won't be a startup-cost path
								 * either */

		merge_pathkeys = build_join_pathkeys(totalouterpath->pathkeys,
											 joinrel->targetlist,
											 root->equi_key_list);
		add_path(joinrel, (Path *)
				 create_mergejoin_path(joinrel,
									   jointype,
									   totalouterpath,
									   innerpath,
									   restrictlist,
									   merge_pathkeys,
									   mergeclauses,
									   NIL,
									   NIL));

		startupouterpath = get_cheapest_path_for_pathkeys(outerrel->pathlist,
														  outersortkeys,
														  STARTUP_COST);
		if (startupouterpath != NULL && startupouterpath != totalouterpath)
		{
			merge_pathkeys = build_join_pathkeys(startupouterpath->pathkeys,
												 joinrel->targetlist,
												 root->equi_key_list);
			add_path(joinrel, (Path *)
					 create_mergejoin_path(joinrel,
										   jointype,
										   startupouterpath,
										   innerpath,
										   restrictlist,
										   merge_pathkeys,
										   mergeclauses,
										   NIL,
										   NIL));
		}
	}
}

#endif

/*
 * hash_inner_and_outer
 *	  Create hashjoin join paths by explicitly hashing both the outer and
 *	  inner join relations of each available hash clause.
 *
 * 'joinrel' is the join relation
 * 'outerrel' is the outer join relation
 * 'innerrel' is the inner join relation
 * 'restrictlist' contains all of the RestrictInfo nodes for restriction
 *		clauses that apply to this join
 * 'jointype' is the type of join to do
 */
static void
hash_inner_and_outer(Query *root,
					 RelOptInfo *joinrel,
					 RelOptInfo *outerrel,
					 RelOptInfo *innerrel,
					 List *restrictlist,
					 JoinType jointype)
{
	Relids		outerrelids = outerrel->relids;
	Relids		innerrelids = innerrel->relids;
	bool		isouterjoin;
	List	   *i;

	/*
	 * Hashjoin only supports inner and left joins.
	 */
	switch (jointype)
	{
		case JOIN_INNER:
			isouterjoin = false;
			break;
		case JOIN_LEFT:
			isouterjoin = true;
			break;
		default:
			return;
	}

	/*
	 * Scan the join's restrictinfo list to find hashjoinable clauses that
	 * are usable with this pair of sub-relations.	Since we currently
	 * accept only var-op-var clauses as hashjoinable, we need only check
	 * the membership of the vars to determine whether a particular clause
	 * can be used with this pair of sub-relations.  This code would need
	 * to be upgraded if we wanted to allow more-complex expressions in
	 * hash joins.
	 */
	foreach(i, restrictlist)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(i);
		Expr	   *clause;
		Var		   *left,
				   *right,
				   *inner;
		List	   *hashclauses;
		Selectivity innerdisbursion;

		if (restrictinfo->hashjoinoperator == InvalidOid)
			continue;			/* not hashjoinable */

		/*
		 * If processing an outer join, only use its own join clauses for
		 * hashing.  For inner joins we need not be so picky.
		 */
		if (isouterjoin && restrictinfo->ispusheddown)
			continue;

		clause = restrictinfo->clause;
		/* these must be OK, since check_hashjoinable accepted the clause */
		left = get_leftop(clause);
		right = get_rightop(clause);

		/* check if clause is usable with these sub-rels, find inner var */
		if (intMember(left->varno, outerrelids) &&
			intMember(right->varno, innerrelids))
			inner = right;
		else if (intMember(left->varno, innerrelids) &&
				 intMember(right->varno, outerrelids))
			inner = left;
		else
			continue;			/* no good for these input relations */

		/* always a one-element list of hash clauses */
		hashclauses = makeList1(restrictinfo);

		/* estimate disbursion of inner var for costing purposes */
		innerdisbursion = estimate_disbursion(root, inner);

		/*
		 * We consider both the cheapest-total-cost and
		 * cheapest-startup-cost outer paths.  There's no need to consider
		 * any but the cheapest- total-cost inner path, however.
		 */
		add_path(joinrel, (Path *)
				 create_hashjoin_path(joinrel,
									  jointype,
									  outerrel->cheapest_total_path,
									  innerrel->cheapest_total_path,
									  restrictlist,
									  hashclauses,
									  innerdisbursion));
		if (outerrel->cheapest_startup_path != outerrel->cheapest_total_path)
			add_path(joinrel, (Path *)
					 create_hashjoin_path(joinrel,
										  jointype,
										  outerrel->cheapest_startup_path,
										  innerrel->cheapest_total_path,
										  restrictlist,
										  hashclauses,
										  innerdisbursion));
	}
}

/*
 * best_innerjoin
 *	  Find the cheapest index path that has already been identified by
 *	  indexable_joinclauses() as being a possible inner path for the given
 *	  outer relation(s) in a nestloop join.
 *
 * We compare indexpaths on total_cost only, assuming that they will all have
 * zero or negligible startup_cost.  We might have to think harder someday...
 *
 * 'join_paths' is a list of potential inner indexscan join paths
 * 'outer_relids' is the relid list of the outer join relation
 *
 * Returns the pathnode of the best path, or NULL if there's no
 * usable path.
 */
static Path *
best_innerjoin(List *join_paths, Relids outer_relids, JoinType jointype)
{
	Path	   *cheapest = (Path *) NULL;
	bool		isouterjoin;
	List	   *join_path;

	/*
	 * Nestloop only supports inner and left joins.
	 */
	switch (jointype)
	{
		case JOIN_INNER:
			isouterjoin = false;
			break;
		case JOIN_LEFT:
			isouterjoin = true;
			break;
		default:
			return NULL;
	}

	foreach(join_path, join_paths)
	{
		IndexPath   *path = (IndexPath *) lfirst(join_path);

		Assert(IsA(path, IndexPath));

		/*
		 * If processing an outer join, only use explicit join clauses in the
		 * inner indexscan.  For inner joins we need not be so picky.
		 */
		if (isouterjoin && !path->alljoinquals)
			continue;

		/*
		 * path->joinrelids is the set of base rels that must be part of
		 * outer_relids in order to use this inner path, because those
		 * rels are used in the index join quals of this inner path.
		 */
		if (is_subseti(path->joinrelids, outer_relids) &&
			(cheapest == NULL ||
			 compare_path_costs((Path *) path, cheapest, TOTAL_COST) < 0))
			cheapest = (Path *) path;
	}
	return cheapest;
}

/*
 * Estimate disbursion of the specified Var
 *
 * We use a default of 0.1 if we can't figure out anything better.
 * This will typically discourage use of a hash rather strongly,
 * if the inner relation is large.	We do not want to hash unless
 * we know that the inner rel is well-dispersed (or the alternatives
 * seem much worse).
 */
static Selectivity
estimate_disbursion(Query *root, Var *var)
{
	Oid			relid;

	if (!IsA(var, Var))
		return 0.1;

	relid = getrelid(var->varno, root->rtable);

	if (relid == InvalidOid)
		return 0.1;

	return (Selectivity) get_attdisbursion(relid, var->varattno, 0.1);
}

/*
 * select_mergejoin_clauses
 *	  Select mergejoin clauses that are usable for a particular join.
 *	  Returns a list of RestrictInfo nodes for those clauses.
 *
 * We examine each restrictinfo clause known for the join to see
 * if it is mergejoinable and involves vars from the two sub-relations
 * currently of interest.
 *
 * Since we currently allow only plain Vars as the left and right sides
 * of mergejoin clauses, this test is relatively simple.  This routine
 * would need to be upgraded to support more-complex expressions
 * as sides of mergejoins.	In theory, we could allow arbitrarily complex
 * expressions in mergejoins, so long as one side uses only vars from one
 * sub-relation and the other side uses only vars from the other.
 */
static List *
select_mergejoin_clauses(RelOptInfo *joinrel,
						 RelOptInfo *outerrel,
						 RelOptInfo *innerrel,
						 List *restrictlist,
						 JoinType jointype)
{
	List	   *result_list = NIL;
	Relids		outerrelids = outerrel->relids;
	Relids		innerrelids = innerrel->relids;
	bool		isouterjoin = IS_OUTER_JOIN(jointype);
	List	   *i;

	foreach(i, restrictlist)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(i);
		Expr	   *clause;
		Var		   *left,
				   *right;

		/*
		 * If processing an outer join, only use its own join clauses in the
		 * merge.  For inner joins we need not be so picky.
		 *
		 * Furthermore, if it is a right/full join then *all* the explicit
		 * join clauses must be mergejoinable, else the executor will fail.
		 * If we are asked for a right join then just return NIL to indicate
		 * no mergejoin is possible (we can handle it as a left join instead).
		 * If we are asked for a full join then emit an error, because there
		 * is no fallback.
		 */
		if (isouterjoin)
		{
			if (restrictinfo->ispusheddown)
				continue;
			switch (jointype)
			{
				case JOIN_RIGHT:
					if (restrictinfo->mergejoinoperator == InvalidOid)
						return NIL;	/* not mergejoinable */
					break;
				case JOIN_FULL:
					if (restrictinfo->mergejoinoperator == InvalidOid)
						elog(ERROR, "FULL JOIN is only supported with mergejoinable join conditions");
					break;
				default:
					/* otherwise, it's OK to have nonmergeable join quals */
					break;
			}
		}

		if (restrictinfo->mergejoinoperator == InvalidOid)
			continue;			/* not mergejoinable */

		clause = restrictinfo->clause;
		/* these must be OK, since check_mergejoinable accepted the clause */
		left = get_leftop(clause);
		right = get_rightop(clause);

		if ((intMember(left->varno, outerrelids) &&
			 intMember(right->varno, innerrelids)) ||
			(intMember(left->varno, innerrelids) &&
			 intMember(right->varno, outerrelids)))
			result_list = lcons(restrictinfo, result_list);
	}

	return result_list;
}
