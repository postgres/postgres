/*-------------------------------------------------------------------------
 *
 * pathnode.c
 *	  Routines to manipulate pathlists and create path nodes
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/pathnode.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"


typedef enum
{
	COSTS_EQUAL,				/* path costs are fuzzily equal */
	COSTS_BETTER1,				/* first path is cheaper than second */
	COSTS_BETTER2,				/* second path is cheaper than first */
	COSTS_DIFFERENT				/* neither path dominates the other on cost */
} PathCostComparison;

/*
 * STD_FUZZ_FACTOR is the normal fuzz factor for compare_path_costs_fuzzily.
 * XXX is it worth making this user-controllable?  It provides a tradeoff
 * between planner runtime and the accuracy of path cost comparisons.
 */
#define STD_FUZZ_FACTOR 1.01

static List *translate_sub_tlist(List *tlist, int relid);


/*****************************************************************************
 *		MISC. PATH UTILITIES
 *****************************************************************************/

/*
 * compare_path_costs
 *	  Return -1, 0, or +1 according as path1 is cheaper, the same cost,
 *	  or more expensive than path2 for the specified criterion.
 */
int
compare_path_costs(Path *path1, Path *path2, CostSelector criterion)
{
	if (criterion == STARTUP_COST)
	{
		if (path1->startup_cost < path2->startup_cost)
			return -1;
		if (path1->startup_cost > path2->startup_cost)
			return +1;

		/*
		 * If paths have the same startup cost (not at all unlikely), order
		 * them by total cost.
		 */
		if (path1->total_cost < path2->total_cost)
			return -1;
		if (path1->total_cost > path2->total_cost)
			return +1;
	}
	else
	{
		if (path1->total_cost < path2->total_cost)
			return -1;
		if (path1->total_cost > path2->total_cost)
			return +1;

		/*
		 * If paths have the same total cost, order them by startup cost.
		 */
		if (path1->startup_cost < path2->startup_cost)
			return -1;
		if (path1->startup_cost > path2->startup_cost)
			return +1;
	}
	return 0;
}

/*
 * compare_path_fractional_costs
 *	  Return -1, 0, or +1 according as path1 is cheaper, the same cost,
 *	  or more expensive than path2 for fetching the specified fraction
 *	  of the total tuples.
 *
 * If fraction is <= 0 or > 1, we interpret it as 1, ie, we select the
 * path with the cheaper total_cost.
 */
int
compare_fractional_path_costs(Path *path1, Path *path2,
							  double fraction)
{
	Cost		cost1,
				cost2;

	if (fraction <= 0.0 || fraction >= 1.0)
		return compare_path_costs(path1, path2, TOTAL_COST);
	cost1 = path1->startup_cost +
		fraction * (path1->total_cost - path1->startup_cost);
	cost2 = path2->startup_cost +
		fraction * (path2->total_cost - path2->startup_cost);
	if (cost1 < cost2)
		return -1;
	if (cost1 > cost2)
		return +1;
	return 0;
}

/*
 * compare_path_costs_fuzzily
 *	  Compare the costs of two paths to see if either can be said to
 *	  dominate the other.
 *
 * We use fuzzy comparisons so that add_path() can avoid keeping both of
 * a pair of paths that really have insignificantly different cost.
 *
 * The fuzz_factor argument must be 1.0 plus delta, where delta is the
 * fraction of the smaller cost that is considered to be a significant
 * difference.  For example, fuzz_factor = 1.01 makes the fuzziness limit
 * be 1% of the smaller cost.
 *
 * The two paths are said to have "equal" costs if both startup and total
 * costs are fuzzily the same.  Path1 is said to be better than path2 if
 * it has fuzzily better startup cost and fuzzily no worse total cost,
 * or if it has fuzzily better total cost and fuzzily no worse startup cost.
 * Path2 is better than path1 if the reverse holds.  Finally, if one path
 * is fuzzily better than the other on startup cost and fuzzily worse on
 * total cost, we just say that their costs are "different", since neither
 * dominates the other across the whole performance spectrum.
 *
 * This function also enforces a policy rule that paths for which the relevant
 * one of parent->consider_startup and parent->consider_param_startup is false
 * cannot survive comparisons solely on the grounds of good startup cost, so
 * we never return COSTS_DIFFERENT when that is true for the total-cost loser.
 * (But if total costs are fuzzily equal, we compare startup costs anyway,
 * in hopes of eliminating one path or the other.)
 */
static PathCostComparison
compare_path_costs_fuzzily(Path *path1, Path *path2, double fuzz_factor)
{
#define CONSIDER_PATH_STARTUP_COST(p)  \
	((p)->param_info == NULL ? (p)->parent->consider_startup : (p)->parent->consider_param_startup)

	/*
	 * Check total cost first since it's more likely to be different; many
	 * paths have zero startup cost.
	 */
	if (path1->total_cost > path2->total_cost * fuzz_factor)
	{
		/* path1 fuzzily worse on total cost */
		if (CONSIDER_PATH_STARTUP_COST(path1) &&
			path2->startup_cost > path1->startup_cost * fuzz_factor)
		{
			/* ... but path2 fuzzily worse on startup, so DIFFERENT */
			return COSTS_DIFFERENT;
		}
		/* else path2 dominates */
		return COSTS_BETTER2;
	}
	if (path2->total_cost > path1->total_cost * fuzz_factor)
	{
		/* path2 fuzzily worse on total cost */
		if (CONSIDER_PATH_STARTUP_COST(path2) &&
			path1->startup_cost > path2->startup_cost * fuzz_factor)
		{
			/* ... but path1 fuzzily worse on startup, so DIFFERENT */
			return COSTS_DIFFERENT;
		}
		/* else path1 dominates */
		return COSTS_BETTER1;
	}
	/* fuzzily the same on total cost ... */
	if (path1->startup_cost > path2->startup_cost * fuzz_factor)
	{
		/* ... but path1 fuzzily worse on startup, so path2 wins */
		return COSTS_BETTER2;
	}
	if (path2->startup_cost > path1->startup_cost * fuzz_factor)
	{
		/* ... but path2 fuzzily worse on startup, so path1 wins */
		return COSTS_BETTER1;
	}
	/* fuzzily the same on both costs */
	return COSTS_EQUAL;

#undef CONSIDER_PATH_STARTUP_COST
}

/*
 * set_cheapest
 *	  Find the minimum-cost paths from among a relation's paths,
 *	  and save them in the rel's cheapest-path fields.
 *
 * cheapest_total_path is normally the cheapest-total-cost unparameterized
 * path; but if there are no unparameterized paths, we assign it to be the
 * best (cheapest least-parameterized) parameterized path.  However, only
 * unparameterized paths are considered candidates for cheapest_startup_path,
 * so that will be NULL if there are no unparameterized paths.
 *
 * The cheapest_parameterized_paths list collects all parameterized paths
 * that have survived the add_path() tournament for this relation.  (Since
 * add_path ignores pathkeys for a parameterized path, these will be paths
 * that have best cost or best row count for their parameterization.  We
 * may also have both a parallel-safe and a non-parallel-safe path in some
 * cases for the same parameterization in some cases, but this should be
 * relatively rare since, most typically, all paths for the same relation
 * will be parallel-safe or none of them will.)
 *
 * cheapest_parameterized_paths always includes the cheapest-total
 * unparameterized path, too, if there is one; the users of that list find
 * it more convenient if that's included.
 *
 * This is normally called only after we've finished constructing the path
 * list for the rel node.
 */
void
set_cheapest(RelOptInfo *parent_rel)
{
	Path	   *cheapest_startup_path;
	Path	   *cheapest_total_path;
	Path	   *best_param_path;
	List	   *parameterized_paths;
	ListCell   *p;

	Assert(IsA(parent_rel, RelOptInfo));

	if (parent_rel->pathlist == NIL)
		elog(ERROR, "could not devise a query plan for the given query");

	cheapest_startup_path = cheapest_total_path = best_param_path = NULL;
	parameterized_paths = NIL;

	foreach(p, parent_rel->pathlist)
	{
		Path	   *path = (Path *) lfirst(p);
		int			cmp;

		if (path->param_info)
		{
			/* Parameterized path, so add it to parameterized_paths */
			parameterized_paths = lappend(parameterized_paths, path);

			/*
			 * If we have an unparameterized cheapest-total, we no longer care
			 * about finding the best parameterized path, so move on.
			 */
			if (cheapest_total_path)
				continue;

			/*
			 * Otherwise, track the best parameterized path, which is the one
			 * with least total cost among those of the minimum
			 * parameterization.
			 */
			if (best_param_path == NULL)
				best_param_path = path;
			else
			{
				switch (bms_subset_compare(PATH_REQ_OUTER(path),
										   PATH_REQ_OUTER(best_param_path)))
				{
					case BMS_EQUAL:
						/* keep the cheaper one */
						if (compare_path_costs(path, best_param_path,
											   TOTAL_COST) < 0)
							best_param_path = path;
						break;
					case BMS_SUBSET1:
						/* new path is less-parameterized */
						best_param_path = path;
						break;
					case BMS_SUBSET2:
						/* old path is less-parameterized, keep it */
						break;
					case BMS_DIFFERENT:

						/*
						 * This means that neither path has the least possible
						 * parameterization for the rel.  We'll sit on the old
						 * path until something better comes along.
						 */
						break;
				}
			}
		}
		else
		{
			/* Unparameterized path, so consider it for cheapest slots */
			if (cheapest_total_path == NULL)
			{
				cheapest_startup_path = cheapest_total_path = path;
				continue;
			}

			/*
			 * If we find two paths of identical costs, try to keep the
			 * better-sorted one.  The paths might have unrelated sort
			 * orderings, in which case we can only guess which might be
			 * better to keep, but if one is superior then we definitely
			 * should keep that one.
			 */
			cmp = compare_path_costs(cheapest_startup_path, path, STARTUP_COST);
			if (cmp > 0 ||
				(cmp == 0 &&
				 compare_pathkeys(cheapest_startup_path->pathkeys,
								  path->pathkeys) == PATHKEYS_BETTER2))
				cheapest_startup_path = path;

			cmp = compare_path_costs(cheapest_total_path, path, TOTAL_COST);
			if (cmp > 0 ||
				(cmp == 0 &&
				 compare_pathkeys(cheapest_total_path->pathkeys,
								  path->pathkeys) == PATHKEYS_BETTER2))
				cheapest_total_path = path;
		}
	}

	/* Add cheapest unparameterized path, if any, to parameterized_paths */
	if (cheapest_total_path)
		parameterized_paths = lcons(cheapest_total_path, parameterized_paths);

	/*
	 * If there is no unparameterized path, use the best parameterized path as
	 * cheapest_total_path (but not as cheapest_startup_path).
	 */
	if (cheapest_total_path == NULL)
		cheapest_total_path = best_param_path;
	Assert(cheapest_total_path != NULL);

	parent_rel->cheapest_startup_path = cheapest_startup_path;
	parent_rel->cheapest_total_path = cheapest_total_path;
	parent_rel->cheapest_unique_path = NULL;	/* computed only if needed */
	parent_rel->cheapest_parameterized_paths = parameterized_paths;
}

/*
 * add_path
 *	  Consider a potential implementation path for the specified parent rel,
 *	  and add it to the rel's pathlist if it is worthy of consideration.
 *	  A path is worthy if it has a better sort order (better pathkeys) or
 *	  cheaper cost (on either dimension), or generates fewer rows, than any
 *	  existing path that has the same or superset parameterization rels.
 *	  We also consider parallel-safe paths more worthy than others.
 *
 *	  We also remove from the rel's pathlist any old paths that are dominated
 *	  by new_path --- that is, new_path is cheaper, at least as well ordered,
 *	  generates no more rows, requires no outer rels not required by the old
 *	  path, and is no less parallel-safe.
 *
 *	  In most cases, a path with a superset parameterization will generate
 *	  fewer rows (since it has more join clauses to apply), so that those two
 *	  figures of merit move in opposite directions; this means that a path of
 *	  one parameterization can seldom dominate a path of another.  But such
 *	  cases do arise, so we make the full set of checks anyway.
 *
 *	  There are two policy decisions embedded in this function, along with
 *	  its sibling add_path_precheck.  First, we treat all parameterized paths
 *	  as having NIL pathkeys, so that they cannot win comparisons on the
 *	  basis of sort order.  This is to reduce the number of parameterized
 *	  paths that are kept; see discussion in src/backend/optimizer/README.
 *
 *	  Second, we only consider cheap startup cost to be interesting if
 *	  parent_rel->consider_startup is true for an unparameterized path, or
 *	  parent_rel->consider_param_startup is true for a parameterized one.
 *	  Again, this allows discarding useless paths sooner.
 *
 *	  The pathlist is kept sorted by total_cost, with cheaper paths
 *	  at the front.  Within this routine, that's simply a speed hack:
 *	  doing it that way makes it more likely that we will reject an inferior
 *	  path after a few comparisons, rather than many comparisons.
 *	  However, add_path_precheck relies on this ordering to exit early
 *	  when possible.
 *
 *	  NOTE: discarded Path objects are immediately pfree'd to reduce planner
 *	  memory consumption.  We dare not try to free the substructure of a Path,
 *	  since much of it may be shared with other Paths or the query tree itself;
 *	  but just recycling discarded Path nodes is a very useful savings in
 *	  a large join tree.  We can recycle the List nodes of pathlist, too.
 *
 *	  BUT: we do not pfree IndexPath objects, since they may be referenced as
 *	  children of BitmapHeapPaths as well as being paths in their own right.
 *
 * 'parent_rel' is the relation entry to which the path corresponds.
 * 'new_path' is a potential path for parent_rel.
 *
 * Returns nothing, but modifies parent_rel->pathlist.
 */
void
add_path(RelOptInfo *parent_rel, Path *new_path)
{
	bool		accept_new = true;		/* unless we find a superior old path */
	ListCell   *insert_after = NULL;	/* where to insert new item */
	List	   *new_path_pathkeys;
	ListCell   *p1;
	ListCell   *p1_prev;
	ListCell   *p1_next;

	/*
	 * This is a convenient place to check for query cancel --- no part of the
	 * planner goes very long without calling add_path().
	 */
	CHECK_FOR_INTERRUPTS();

	/* Pretend parameterized paths have no pathkeys, per comment above */
	new_path_pathkeys = new_path->param_info ? NIL : new_path->pathkeys;

	/*
	 * Loop to check proposed new path against old paths.  Note it is possible
	 * for more than one old path to be tossed out because new_path dominates
	 * it.
	 *
	 * We can't use foreach here because the loop body may delete the current
	 * list cell.
	 */
	p1_prev = NULL;
	for (p1 = list_head(parent_rel->pathlist); p1 != NULL; p1 = p1_next)
	{
		Path	   *old_path = (Path *) lfirst(p1);
		bool		remove_old = false; /* unless new proves superior */
		PathCostComparison costcmp;
		PathKeysComparison keyscmp;
		BMS_Comparison outercmp;

		p1_next = lnext(p1);

		/*
		 * Do a fuzzy cost comparison with standard fuzziness limit.
		 */
		costcmp = compare_path_costs_fuzzily(new_path, old_path,
											 STD_FUZZ_FACTOR);

		/*
		 * If the two paths compare differently for startup and total cost,
		 * then we want to keep both, and we can skip comparing pathkeys and
		 * required_outer rels.  If they compare the same, proceed with the
		 * other comparisons.  Row count is checked last.  (We make the tests
		 * in this order because the cost comparison is most likely to turn
		 * out "different", and the pathkeys comparison next most likely.  As
		 * explained above, row count very seldom makes a difference, so even
		 * though it's cheap to compare there's not much point in checking it
		 * earlier.)
		 */
		if (costcmp != COSTS_DIFFERENT)
		{
			/* Similarly check to see if either dominates on pathkeys */
			List	   *old_path_pathkeys;

			old_path_pathkeys = old_path->param_info ? NIL : old_path->pathkeys;
			keyscmp = compare_pathkeys(new_path_pathkeys,
									   old_path_pathkeys);
			if (keyscmp != PATHKEYS_DIFFERENT)
			{
				switch (costcmp)
				{
					case COSTS_EQUAL:
						outercmp = bms_subset_compare(PATH_REQ_OUTER(new_path),
												   PATH_REQ_OUTER(old_path));
						if (keyscmp == PATHKEYS_BETTER1)
						{
							if ((outercmp == BMS_EQUAL ||
								 outercmp == BMS_SUBSET1) &&
								new_path->rows <= old_path->rows &&
								new_path->parallel_safe >= old_path->parallel_safe)
								remove_old = true;		/* new dominates old */
						}
						else if (keyscmp == PATHKEYS_BETTER2)
						{
							if ((outercmp == BMS_EQUAL ||
								 outercmp == BMS_SUBSET2) &&
								new_path->rows >= old_path->rows &&
								new_path->parallel_safe <= old_path->parallel_safe)
								accept_new = false;		/* old dominates new */
						}
						else	/* keyscmp == PATHKEYS_EQUAL */
						{
							if (outercmp == BMS_EQUAL)
							{
								/*
								 * Same pathkeys and outer rels, and fuzzily
								 * the same cost, so keep just one; to decide
								 * which, first check parallel-safety, then
								 * rows, then do a fuzzy cost comparison with
								 * very small fuzz limit.  (We used to do an
								 * exact cost comparison, but that results in
								 * annoying platform-specific plan variations
								 * due to roundoff in the cost estimates.)	If
								 * things are still tied, arbitrarily keep
								 * only the old path.  Notice that we will
								 * keep only the old path even if the
								 * less-fuzzy comparison decides the startup
								 * and total costs compare differently.
								 */
								if (new_path->parallel_safe >
									old_path->parallel_safe)
									remove_old = true;	/* new dominates old */
								else if (new_path->parallel_safe <
										 old_path->parallel_safe)
									accept_new = false; /* old dominates new */
								else if (new_path->rows < old_path->rows)
									remove_old = true;	/* new dominates old */
								else if (new_path->rows > old_path->rows)
									accept_new = false; /* old dominates new */
								else if (compare_path_costs_fuzzily(new_path,
																	old_path,
											  1.0000000001) == COSTS_BETTER1)
									remove_old = true;	/* new dominates old */
								else
									accept_new = false; /* old equals or
														 * dominates new */
							}
							else if (outercmp == BMS_SUBSET1 &&
									 new_path->rows <= old_path->rows &&
									 new_path->parallel_safe >= old_path->parallel_safe)
								remove_old = true;		/* new dominates old */
							else if (outercmp == BMS_SUBSET2 &&
									 new_path->rows >= old_path->rows &&
									 new_path->parallel_safe <= old_path->parallel_safe)
								accept_new = false;		/* old dominates new */
							/* else different parameterizations, keep both */
						}
						break;
					case COSTS_BETTER1:
						if (keyscmp != PATHKEYS_BETTER2)
						{
							outercmp = bms_subset_compare(PATH_REQ_OUTER(new_path),
												   PATH_REQ_OUTER(old_path));
							if ((outercmp == BMS_EQUAL ||
								 outercmp == BMS_SUBSET1) &&
								new_path->rows <= old_path->rows &&
								new_path->parallel_safe >= old_path->parallel_safe)
								remove_old = true;		/* new dominates old */
						}
						break;
					case COSTS_BETTER2:
						if (keyscmp != PATHKEYS_BETTER1)
						{
							outercmp = bms_subset_compare(PATH_REQ_OUTER(new_path),
												   PATH_REQ_OUTER(old_path));
							if ((outercmp == BMS_EQUAL ||
								 outercmp == BMS_SUBSET2) &&
								new_path->rows >= old_path->rows &&
								new_path->parallel_safe <= old_path->parallel_safe)
								accept_new = false;		/* old dominates new */
						}
						break;
					case COSTS_DIFFERENT:

						/*
						 * can't get here, but keep this case to keep compiler
						 * quiet
						 */
						break;
				}
			}
		}

		/*
		 * Remove current element from pathlist if dominated by new.
		 */
		if (remove_old)
		{
			parent_rel->pathlist = list_delete_cell(parent_rel->pathlist,
													p1, p1_prev);

			/*
			 * Delete the data pointed-to by the deleted cell, if possible
			 */
			if (!IsA(old_path, IndexPath))
				pfree(old_path);
			/* p1_prev does not advance */
		}
		else
		{
			/* new belongs after this old path if it has cost >= old's */
			if (new_path->total_cost >= old_path->total_cost)
				insert_after = p1;
			/* p1_prev advances */
			p1_prev = p1;
		}

		/*
		 * If we found an old path that dominates new_path, we can quit
		 * scanning the pathlist; we will not add new_path, and we assume
		 * new_path cannot dominate any other elements of the pathlist.
		 */
		if (!accept_new)
			break;
	}

	if (accept_new)
	{
		/* Accept the new path: insert it at proper place in pathlist */
		if (insert_after)
			lappend_cell(parent_rel->pathlist, insert_after, new_path);
		else
			parent_rel->pathlist = lcons(new_path, parent_rel->pathlist);
	}
	else
	{
		/* Reject and recycle the new path */
		if (!IsA(new_path, IndexPath))
			pfree(new_path);
	}
}

/*
 * add_path_precheck
 *	  Check whether a proposed new path could possibly get accepted.
 *	  We assume we know the path's pathkeys and parameterization accurately,
 *	  and have lower bounds for its costs.
 *
 * Note that we do not know the path's rowcount, since getting an estimate for
 * that is too expensive to do before prechecking.  We assume here that paths
 * of a superset parameterization will generate fewer rows; if that holds,
 * then paths with different parameterizations cannot dominate each other
 * and so we can simply ignore existing paths of another parameterization.
 * (In the infrequent cases where that rule of thumb fails, add_path will
 * get rid of the inferior path.)
 *
 * At the time this is called, we haven't actually built a Path structure,
 * so the required information has to be passed piecemeal.
 */
bool
add_path_precheck(RelOptInfo *parent_rel,
				  Cost startup_cost, Cost total_cost,
				  List *pathkeys, Relids required_outer)
{
	List	   *new_path_pathkeys;
	bool		consider_startup;
	ListCell   *p1;

	/* Pretend parameterized paths have no pathkeys, per add_path policy */
	new_path_pathkeys = required_outer ? NIL : pathkeys;

	/* Decide whether new path's startup cost is interesting */
	consider_startup = required_outer ? parent_rel->consider_param_startup : parent_rel->consider_startup;

	foreach(p1, parent_rel->pathlist)
	{
		Path	   *old_path = (Path *) lfirst(p1);
		PathKeysComparison keyscmp;

		/*
		 * We are looking for an old_path with the same parameterization (and
		 * by assumption the same rowcount) that dominates the new path on
		 * pathkeys as well as both cost metrics.  If we find one, we can
		 * reject the new path.
		 *
		 * Cost comparisons here should match compare_path_costs_fuzzily.
		 */
		if (total_cost > old_path->total_cost * STD_FUZZ_FACTOR)
		{
			/* new path can win on startup cost only if consider_startup */
			if (startup_cost > old_path->startup_cost * STD_FUZZ_FACTOR ||
				!consider_startup)
			{
				/* new path loses on cost, so check pathkeys... */
				List	   *old_path_pathkeys;

				old_path_pathkeys = old_path->param_info ? NIL : old_path->pathkeys;
				keyscmp = compare_pathkeys(new_path_pathkeys,
										   old_path_pathkeys);
				if (keyscmp == PATHKEYS_EQUAL ||
					keyscmp == PATHKEYS_BETTER2)
				{
					/* new path does not win on pathkeys... */
					if (bms_equal(required_outer, PATH_REQ_OUTER(old_path)))
					{
						/* Found an old path that dominates the new one */
						return false;
					}
				}
			}
		}
		else
		{
			/*
			 * Since the pathlist is sorted by total_cost, we can stop looking
			 * once we reach a path with a total_cost larger than the new
			 * path's.
			 */
			break;
		}
	}

	return true;
}

/*
 * add_partial_path
 *	  Like add_path, our goal here is to consider whether a path is worthy
 *	  of being kept around, but the considerations here are a bit different.
 *	  A partial path is one which can be executed in any number of workers in
 *	  parallel such that each worker will generate a subset of the path's
 *	  overall result.
 *
 *	  We don't generate parameterized partial paths for several reasons.  Most
 *	  importantly, they're not safe to execute, because there's nothing to
 *	  make sure that a parallel scan within the parameterized portion of the
 *	  plan is running with the same value in every worker at the same time.
 *	  Fortunately, it seems unlikely to be worthwhile anyway, because having
 *	  each worker scan the entire outer relation and a subset of the inner
 *	  relation will generally be a terrible plan.  The inner (parameterized)
 *	  side of the plan will be small anyway.  There could be rare cases where
 *	  this wins big - e.g. if join order constraints put a 1-row relation on
 *	  the outer side of the topmost join with a parameterized plan on the inner
 *	  side - but we'll have to be content not to handle such cases until somebody
 *	  builds an executor infrastructure that can cope with them.
 *
 *	  Because we don't consider parameterized paths here, we also don't
 *	  need to consider the row counts as a measure of quality: every path will
 *	  produce the same number of rows.  Neither do we need to consider startup
 *	  costs: parallelism is only used for plans that will be run to completion.
 *	  Therefore, this routine is much simpler than add_path: it needs to
 *	  consider only pathkeys and total cost.
 */
void
add_partial_path(RelOptInfo *parent_rel, Path *new_path)
{
	bool		accept_new = true;		/* unless we find a superior old path */
	ListCell   *insert_after = NULL;	/* where to insert new item */
	ListCell   *p1;
	ListCell   *p1_prev;
	ListCell   *p1_next;

	/* Check for query cancel. */
	CHECK_FOR_INTERRUPTS();

	/*
	 * As in add_path, throw out any paths which are dominated by the new
	 * path, but throw out the new path if some existing path dominates it.
	 */
	p1_prev = NULL;
	for (p1 = list_head(parent_rel->partial_pathlist); p1 != NULL;
		 p1 = p1_next)
	{
		Path	   *old_path = (Path *) lfirst(p1);
		bool		remove_old = false; /* unless new proves superior */
		PathKeysComparison keyscmp;

		p1_next = lnext(p1);

		/* Compare pathkeys. */
		keyscmp = compare_pathkeys(new_path->pathkeys, old_path->pathkeys);

		/* Unless pathkeys are incompable, keep just one of the two paths. */
		if (keyscmp != PATHKEYS_DIFFERENT)
		{
			if (new_path->total_cost > old_path->total_cost * STD_FUZZ_FACTOR)
			{
				/* New path costs more; keep it only if pathkeys are better. */
				if (keyscmp != PATHKEYS_BETTER1)
					accept_new = false;
			}
			else if (old_path->total_cost > new_path->total_cost
					 * STD_FUZZ_FACTOR)
			{
				/* Old path costs more; keep it only if pathkeys are better. */
				if (keyscmp != PATHKEYS_BETTER2)
					remove_old = true;
			}
			else if (keyscmp == PATHKEYS_BETTER1)
			{
				/* Costs are about the same, new path has better pathkeys. */
				remove_old = true;
			}
			else if (keyscmp == PATHKEYS_BETTER2)
			{
				/* Costs are about the same, old path has better pathkeys. */
				accept_new = false;
			}
			else if (old_path->total_cost > new_path->total_cost * 1.0000000001)
			{
				/* Pathkeys are the same, and the old path costs more. */
				remove_old = true;
			}
			else
			{
				/*
				 * Pathkeys are the same, and new path isn't materially
				 * cheaper.
				 */
				accept_new = false;
			}
		}

		/*
		 * Remove current element from partial_pathlist if dominated by new.
		 */
		if (remove_old)
		{
			parent_rel->partial_pathlist =
				list_delete_cell(parent_rel->partial_pathlist, p1, p1_prev);
			/* add_path has a special case for IndexPath; we don't need it */
			Assert(!IsA(old_path, IndexPath));
			pfree(old_path);
			/* p1_prev does not advance */
		}
		else
		{
			/* new belongs after this old path if it has cost >= old's */
			if (new_path->total_cost >= old_path->total_cost)
				insert_after = p1;
			/* p1_prev advances */
			p1_prev = p1;
		}

		/*
		 * If we found an old path that dominates new_path, we can quit
		 * scanning the partial_pathlist; we will not add new_path, and we
		 * assume new_path cannot dominate any later path.
		 */
		if (!accept_new)
			break;
	}

	if (accept_new)
	{
		/* Accept the new path: insert it at proper place */
		if (insert_after)
			lappend_cell(parent_rel->partial_pathlist, insert_after, new_path);
		else
			parent_rel->partial_pathlist =
				lcons(new_path, parent_rel->partial_pathlist);
	}
	else
	{
		/* add_path has a special case for IndexPath; we don't need it */
		Assert(!IsA(new_path, IndexPath));
		/* Reject and recycle the new path */
		pfree(new_path);
	}
}

/*
 * add_partial_path_precheck
 *	  Check whether a proposed new partial path could possibly get accepted.
 *
 * Unlike add_path_precheck, we can ignore startup cost and parameterization,
 * since they don't matter for partial paths (see add_partial_path).  But
 * we do want to make sure we don't add a partial path if there's already
 * a complete path that dominates it, since in that case the proposed path
 * is surely a loser.
 */
bool
add_partial_path_precheck(RelOptInfo *parent_rel, Cost total_cost,
						  List *pathkeys)
{
	ListCell   *p1;

	/*
	 * Our goal here is twofold.  First, we want to find out whether this path
	 * is clearly inferior to some existing partial path.  If so, we want to
	 * reject it immediately.  Second, we want to find out whether this path
	 * is clearly superior to some existing partial path -- at least, modulo
	 * final cost computations.  If so, we definitely want to consider it.
	 *
	 * Unlike add_path(), we always compare pathkeys here.  This is because we
	 * expect partial_pathlist to be very short, and getting a definitive
	 * answer at this stage avoids the need to call add_path_precheck.
	 */
	foreach(p1, parent_rel->partial_pathlist)
	{
		Path	   *old_path = (Path *) lfirst(p1);
		PathKeysComparison keyscmp;

		keyscmp = compare_pathkeys(pathkeys, old_path->pathkeys);
		if (keyscmp != PATHKEYS_DIFFERENT)
		{
			if (total_cost > old_path->total_cost * STD_FUZZ_FACTOR &&
				keyscmp != PATHKEYS_BETTER1)
				return false;
			if (old_path->total_cost > total_cost * STD_FUZZ_FACTOR &&
				keyscmp != PATHKEYS_BETTER2)
				return true;
		}
	}

	/*
	 * This path is neither clearly inferior to an existing partial path nor
	 * clearly good enough that it might replace one.  Compare it to
	 * non-parallel plans.  If it loses even before accounting for the cost of
	 * the Gather node, we should definitely reject it.
	 *
	 * Note that we pass the total_cost to add_path_precheck twice.  This is
	 * because it's never advantageous to consider the startup cost of a
	 * partial path; the resulting plans, if run in parallel, will be run to
	 * completion.
	 */
	if (!add_path_precheck(parent_rel, total_cost, total_cost, pathkeys,
						   NULL))
		return false;

	return true;
}


/*****************************************************************************
 *		PATH NODE CREATION ROUTINES
 *****************************************************************************/

/*
 * create_seqscan_path
 *	  Creates a path corresponding to a sequential scan, returning the
 *	  pathnode.
 */
Path *
create_seqscan_path(PlannerInfo *root, RelOptInfo *rel,
					Relids required_outer, int parallel_degree)
{
	Path	   *pathnode = makeNode(Path);

	pathnode->pathtype = T_SeqScan;
	pathnode->parent = rel;
	pathnode->pathtarget = &(rel->reltarget);
	pathnode->param_info = get_baserel_parampathinfo(root, rel,
													 required_outer);
	pathnode->parallel_aware = parallel_degree > 0 ? true : false;
	pathnode->parallel_safe = rel->consider_parallel;
	pathnode->parallel_degree = parallel_degree;
	pathnode->pathkeys = NIL;	/* seqscan has unordered result */

	cost_seqscan(pathnode, root, rel, pathnode->param_info);

	return pathnode;
}

/*
 * create_samplescan_path
 *	  Creates a path node for a sampled table scan.
 */
Path *
create_samplescan_path(PlannerInfo *root, RelOptInfo *rel, Relids required_outer)
{
	Path	   *pathnode = makeNode(Path);

	pathnode->pathtype = T_SampleScan;
	pathnode->parent = rel;
	pathnode->pathtarget = &(rel->reltarget);
	pathnode->param_info = get_baserel_parampathinfo(root, rel,
													 required_outer);
	pathnode->parallel_aware = false;
	pathnode->parallel_safe = rel->consider_parallel;
	pathnode->parallel_degree = 0;
	pathnode->pathkeys = NIL;	/* samplescan has unordered result */

	cost_samplescan(pathnode, root, rel, pathnode->param_info);

	return pathnode;
}

/*
 * create_index_path
 *	  Creates a path node for an index scan.
 *
 * 'index' is a usable index.
 * 'indexclauses' is a list of RestrictInfo nodes representing clauses
 *			to be used as index qual conditions in the scan.
 * 'indexclausecols' is an integer list of index column numbers (zero based)
 *			the indexclauses can be used with.
 * 'indexorderbys' is a list of bare expressions (no RestrictInfos)
 *			to be used as index ordering operators in the scan.
 * 'indexorderbycols' is an integer list of index column numbers (zero based)
 *			the ordering operators can be used with.
 * 'pathkeys' describes the ordering of the path.
 * 'indexscandir' is ForwardScanDirection or BackwardScanDirection
 *			for an ordered index, or NoMovementScanDirection for
 *			an unordered index.
 * 'indexonly' is true if an index-only scan is wanted.
 * 'required_outer' is the set of outer relids for a parameterized path.
 * 'loop_count' is the number of repetitions of the indexscan to factor into
 *		estimates of caching behavior.
 *
 * Returns the new path node.
 */
IndexPath *
create_index_path(PlannerInfo *root,
				  IndexOptInfo *index,
				  List *indexclauses,
				  List *indexclausecols,
				  List *indexorderbys,
				  List *indexorderbycols,
				  List *pathkeys,
				  ScanDirection indexscandir,
				  bool indexonly,
				  Relids required_outer,
				  double loop_count)
{
	IndexPath  *pathnode = makeNode(IndexPath);
	RelOptInfo *rel = index->rel;
	List	   *indexquals,
			   *indexqualcols;

	pathnode->path.pathtype = indexonly ? T_IndexOnlyScan : T_IndexScan;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = &(rel->reltarget);
	pathnode->path.param_info = get_baserel_parampathinfo(root, rel,
														  required_outer);
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel;
	pathnode->path.parallel_degree = 0;
	pathnode->path.pathkeys = pathkeys;

	/* Convert clauses to indexquals the executor can handle */
	expand_indexqual_conditions(index, indexclauses, indexclausecols,
								&indexquals, &indexqualcols);

	/* Fill in the pathnode */
	pathnode->indexinfo = index;
	pathnode->indexclauses = indexclauses;
	pathnode->indexquals = indexquals;
	pathnode->indexqualcols = indexqualcols;
	pathnode->indexorderbys = indexorderbys;
	pathnode->indexorderbycols = indexorderbycols;
	pathnode->indexscandir = indexscandir;

	cost_index(pathnode, root, loop_count);

	return pathnode;
}

/*
 * create_bitmap_heap_path
 *	  Creates a path node for a bitmap scan.
 *
 * 'bitmapqual' is a tree of IndexPath, BitmapAndPath, and BitmapOrPath nodes.
 * 'required_outer' is the set of outer relids for a parameterized path.
 * 'loop_count' is the number of repetitions of the indexscan to factor into
 *		estimates of caching behavior.
 *
 * loop_count should match the value used when creating the component
 * IndexPaths.
 */
BitmapHeapPath *
create_bitmap_heap_path(PlannerInfo *root,
						RelOptInfo *rel,
						Path *bitmapqual,
						Relids required_outer,
						double loop_count)
{
	BitmapHeapPath *pathnode = makeNode(BitmapHeapPath);

	pathnode->path.pathtype = T_BitmapHeapScan;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = &(rel->reltarget);
	pathnode->path.param_info = get_baserel_parampathinfo(root, rel,
														  required_outer);
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel;
	pathnode->path.parallel_degree = 0;
	pathnode->path.pathkeys = NIL;		/* always unordered */

	pathnode->bitmapqual = bitmapqual;

	cost_bitmap_heap_scan(&pathnode->path, root, rel,
						  pathnode->path.param_info,
						  bitmapqual, loop_count);

	return pathnode;
}

/*
 * create_bitmap_and_path
 *	  Creates a path node representing a BitmapAnd.
 */
BitmapAndPath *
create_bitmap_and_path(PlannerInfo *root,
					   RelOptInfo *rel,
					   List *bitmapquals)
{
	BitmapAndPath *pathnode = makeNode(BitmapAndPath);

	pathnode->path.pathtype = T_BitmapAnd;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = &(rel->reltarget);
	pathnode->path.param_info = NULL;	/* not used in bitmap trees */

	/*
	 * Currently, a BitmapHeapPath, BitmapAndPath, or BitmapOrPath will be
	 * parallel-safe if and only if rel->consider_parallel is set.  So, we can
	 * set the flag for this path based only on the relation-level flag,
	 * without actually iterating over the list of children.
	 */
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel;
	pathnode->path.parallel_degree = 0;

	pathnode->path.pathkeys = NIL;		/* always unordered */

	pathnode->bitmapquals = bitmapquals;

	/* this sets bitmapselectivity as well as the regular cost fields: */
	cost_bitmap_and_node(pathnode, root);

	return pathnode;
}

/*
 * create_bitmap_or_path
 *	  Creates a path node representing a BitmapOr.
 */
BitmapOrPath *
create_bitmap_or_path(PlannerInfo *root,
					  RelOptInfo *rel,
					  List *bitmapquals)
{
	BitmapOrPath *pathnode = makeNode(BitmapOrPath);

	pathnode->path.pathtype = T_BitmapOr;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = &(rel->reltarget);
	pathnode->path.param_info = NULL;	/* not used in bitmap trees */

	/*
	 * Currently, a BitmapHeapPath, BitmapAndPath, or BitmapOrPath will be
	 * parallel-safe if and only if rel->consider_parallel is set.  So, we can
	 * set the flag for this path based only on the relation-level flag,
	 * without actually iterating over the list of children.
	 */
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel;
	pathnode->path.parallel_degree = 0;

	pathnode->path.pathkeys = NIL;		/* always unordered */

	pathnode->bitmapquals = bitmapquals;

	/* this sets bitmapselectivity as well as the regular cost fields: */
	cost_bitmap_or_node(pathnode, root);

	return pathnode;
}

/*
 * create_tidscan_path
 *	  Creates a path corresponding to a scan by TID, returning the pathnode.
 */
TidPath *
create_tidscan_path(PlannerInfo *root, RelOptInfo *rel, List *tidquals,
					Relids required_outer)
{
	TidPath    *pathnode = makeNode(TidPath);

	pathnode->path.pathtype = T_TidScan;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = &(rel->reltarget);
	pathnode->path.param_info = get_baserel_parampathinfo(root, rel,
														  required_outer);
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel;
	pathnode->path.parallel_degree = 0;
	pathnode->path.pathkeys = NIL;		/* always unordered */

	pathnode->tidquals = tidquals;

	cost_tidscan(&pathnode->path, root, rel, tidquals,
				 pathnode->path.param_info);

	return pathnode;
}

/*
 * create_append_path
 *	  Creates a path corresponding to an Append plan, returning the
 *	  pathnode.
 *
 * Note that we must handle subpaths = NIL, representing a dummy access path.
 */
AppendPath *
create_append_path(RelOptInfo *rel, List *subpaths, Relids required_outer,
				   int parallel_degree)
{
	AppendPath *pathnode = makeNode(AppendPath);
	ListCell   *l;

	pathnode->path.pathtype = T_Append;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = &(rel->reltarget);
	pathnode->path.param_info = get_appendrel_parampathinfo(rel,
															required_outer);
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel;
	pathnode->path.parallel_degree = parallel_degree;
	pathnode->path.pathkeys = NIL;		/* result is always considered
										 * unsorted */
	pathnode->subpaths = subpaths;

	/*
	 * We don't bother with inventing a cost_append(), but just do it here.
	 *
	 * Compute rows and costs as sums of subplan rows and costs.  We charge
	 * nothing extra for the Append itself, which perhaps is too optimistic,
	 * but since it doesn't do any selection or projection, it is a pretty
	 * cheap node.
	 */
	pathnode->path.rows = 0;
	pathnode->path.startup_cost = 0;
	pathnode->path.total_cost = 0;
	foreach(l, subpaths)
	{
		Path	   *subpath = (Path *) lfirst(l);

		pathnode->path.rows += subpath->rows;

		if (l == list_head(subpaths))	/* first node? */
			pathnode->path.startup_cost = subpath->startup_cost;
		pathnode->path.total_cost += subpath->total_cost;
		pathnode->path.parallel_safe = pathnode->path.parallel_safe &&
			subpath->parallel_safe;

		/* All child paths must have same parameterization */
		Assert(bms_equal(PATH_REQ_OUTER(subpath), required_outer));
	}

	return pathnode;
}

/*
 * create_merge_append_path
 *	  Creates a path corresponding to a MergeAppend plan, returning the
 *	  pathnode.
 */
MergeAppendPath *
create_merge_append_path(PlannerInfo *root,
						 RelOptInfo *rel,
						 List *subpaths,
						 List *pathkeys,
						 Relids required_outer)
{
	MergeAppendPath *pathnode = makeNode(MergeAppendPath);
	Cost		input_startup_cost;
	Cost		input_total_cost;
	ListCell   *l;

	pathnode->path.pathtype = T_MergeAppend;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = &(rel->reltarget);
	pathnode->path.param_info = get_appendrel_parampathinfo(rel,
															required_outer);
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel;
	pathnode->path.parallel_degree = 0;
	pathnode->path.pathkeys = pathkeys;
	pathnode->subpaths = subpaths;

	/*
	 * Apply query-wide LIMIT if known and path is for sole base relation.
	 * (Handling this at this low level is a bit klugy.)
	 */
	if (bms_equal(rel->relids, root->all_baserels))
		pathnode->limit_tuples = root->limit_tuples;
	else
		pathnode->limit_tuples = -1.0;

	/*
	 * Add up the sizes and costs of the input paths.
	 */
	pathnode->path.rows = 0;
	input_startup_cost = 0;
	input_total_cost = 0;
	foreach(l, subpaths)
	{
		Path	   *subpath = (Path *) lfirst(l);

		pathnode->path.rows += subpath->rows;
		pathnode->path.parallel_safe = pathnode->path.parallel_safe &&
			subpath->parallel_safe;

		if (pathkeys_contained_in(pathkeys, subpath->pathkeys))
		{
			/* Subpath is adequately ordered, we won't need to sort it */
			input_startup_cost += subpath->startup_cost;
			input_total_cost += subpath->total_cost;
		}
		else
		{
			/* We'll need to insert a Sort node, so include cost for that */
			Path		sort_path;		/* dummy for result of cost_sort */

			cost_sort(&sort_path,
					  root,
					  pathkeys,
					  subpath->total_cost,
					  subpath->parent->tuples,
					  subpath->pathtarget->width,
					  0.0,
					  work_mem,
					  pathnode->limit_tuples);
			input_startup_cost += sort_path.startup_cost;
			input_total_cost += sort_path.total_cost;
		}

		/* All child paths must have same parameterization */
		Assert(bms_equal(PATH_REQ_OUTER(subpath), required_outer));
	}

	/* Now we can compute total costs of the MergeAppend */
	cost_merge_append(&pathnode->path, root,
					  pathkeys, list_length(subpaths),
					  input_startup_cost, input_total_cost,
					  rel->tuples);

	return pathnode;
}

/*
 * create_result_path
 *	  Creates a path representing a Result-and-nothing-else plan.
 *
 * This is only used for degenerate cases, such as a query with an empty
 * jointree.
 */
ResultPath *
create_result_path(PlannerInfo *root, RelOptInfo *rel,
				   PathTarget *target, List *resconstantqual)
{
	ResultPath *pathnode = makeNode(ResultPath);

	pathnode->path.pathtype = T_Result;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = target;
	pathnode->path.param_info = NULL;	/* there are no other rels... */
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel;
	pathnode->path.parallel_degree = 0;
	pathnode->path.pathkeys = NIL;
	pathnode->quals = resconstantqual;

	/* Hardly worth defining a cost_result() function ... just do it */
	pathnode->path.rows = 1;
	pathnode->path.startup_cost = target->cost.startup;
	pathnode->path.total_cost = target->cost.startup +
		cpu_tuple_cost + target->cost.per_tuple;
	if (resconstantqual)
	{
		QualCost	qual_cost;

		cost_qual_eval(&qual_cost, resconstantqual, root);
		/* resconstantqual is evaluated once at startup */
		pathnode->path.startup_cost += qual_cost.startup + qual_cost.per_tuple;
		pathnode->path.total_cost += qual_cost.startup + qual_cost.per_tuple;
	}

	return pathnode;
}

/*
 * create_material_path
 *	  Creates a path corresponding to a Material plan, returning the
 *	  pathnode.
 */
MaterialPath *
create_material_path(RelOptInfo *rel, Path *subpath)
{
	MaterialPath *pathnode = makeNode(MaterialPath);

	Assert(subpath->parent == rel);

	pathnode->path.pathtype = T_Material;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = &(rel->reltarget);
	pathnode->path.param_info = subpath->param_info;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe;
	pathnode->path.parallel_degree = subpath->parallel_degree;
	pathnode->path.pathkeys = subpath->pathkeys;

	pathnode->subpath = subpath;

	cost_material(&pathnode->path,
				  subpath->startup_cost,
				  subpath->total_cost,
				  subpath->rows,
				  subpath->pathtarget->width);

	return pathnode;
}

/*
 * create_unique_path
 *	  Creates a path representing elimination of distinct rows from the
 *	  input data.  Distinct-ness is defined according to the needs of the
 *	  semijoin represented by sjinfo.  If it is not possible to identify
 *	  how to make the data unique, NULL is returned.
 *
 * If used at all, this is likely to be called repeatedly on the same rel;
 * and the input subpath should always be the same (the cheapest_total path
 * for the rel).  So we cache the result.
 */
UniquePath *
create_unique_path(PlannerInfo *root, RelOptInfo *rel, Path *subpath,
				   SpecialJoinInfo *sjinfo)
{
	UniquePath *pathnode;
	Path		sort_path;		/* dummy for result of cost_sort */
	Path		agg_path;		/* dummy for result of cost_agg */
	MemoryContext oldcontext;
	int			numCols;

	/* Caller made a mistake if subpath isn't cheapest_total ... */
	Assert(subpath == rel->cheapest_total_path);
	Assert(subpath->parent == rel);
	/* ... or if SpecialJoinInfo is the wrong one */
	Assert(sjinfo->jointype == JOIN_SEMI);
	Assert(bms_equal(rel->relids, sjinfo->syn_righthand));

	/* If result already cached, return it */
	if (rel->cheapest_unique_path)
		return (UniquePath *) rel->cheapest_unique_path;

	/* If it's not possible to unique-ify, return NULL */
	if (!(sjinfo->semi_can_btree || sjinfo->semi_can_hash))
		return NULL;

	/*
	 * We must ensure path struct and subsidiary data are allocated in main
	 * planning context; otherwise GEQO memory management causes trouble.
	 */
	oldcontext = MemoryContextSwitchTo(root->planner_cxt);

	pathnode = makeNode(UniquePath);

	pathnode->path.pathtype = T_Unique;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = &(rel->reltarget);
	pathnode->path.param_info = subpath->param_info;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe;
	pathnode->path.parallel_degree = subpath->parallel_degree;

	/*
	 * Assume the output is unsorted, since we don't necessarily have pathkeys
	 * to represent it.  (This might get overridden below.)
	 */
	pathnode->path.pathkeys = NIL;

	pathnode->subpath = subpath;
	pathnode->in_operators = sjinfo->semi_operators;
	pathnode->uniq_exprs = sjinfo->semi_rhs_exprs;

	/*
	 * If the input is a relation and it has a unique index that proves the
	 * semi_rhs_exprs are unique, then we don't need to do anything.  Note
	 * that relation_has_unique_index_for automatically considers restriction
	 * clauses for the rel, as well.
	 */
	if (rel->rtekind == RTE_RELATION && sjinfo->semi_can_btree &&
		relation_has_unique_index_for(root, rel, NIL,
									  sjinfo->semi_rhs_exprs,
									  sjinfo->semi_operators))
	{
		pathnode->umethod = UNIQUE_PATH_NOOP;
		pathnode->path.rows = rel->rows;
		pathnode->path.startup_cost = subpath->startup_cost;
		pathnode->path.total_cost = subpath->total_cost;
		pathnode->path.pathkeys = subpath->pathkeys;

		rel->cheapest_unique_path = (Path *) pathnode;

		MemoryContextSwitchTo(oldcontext);

		return pathnode;
	}

	/*
	 * If the input is a subquery whose output must be unique already, then we
	 * don't need to do anything.  The test for uniqueness has to consider
	 * exactly which columns we are extracting; for example "SELECT DISTINCT
	 * x,y" doesn't guarantee that x alone is distinct. So we cannot check for
	 * this optimization unless semi_rhs_exprs consists only of simple Vars
	 * referencing subquery outputs.  (Possibly we could do something with
	 * expressions in the subquery outputs, too, but for now keep it simple.)
	 */
	if (rel->rtekind == RTE_SUBQUERY)
	{
		RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);

		if (query_supports_distinctness(rte->subquery))
		{
			List	   *sub_tlist_colnos;

			sub_tlist_colnos = translate_sub_tlist(sjinfo->semi_rhs_exprs,
												   rel->relid);

			if (sub_tlist_colnos &&
				query_is_distinct_for(rte->subquery,
									  sub_tlist_colnos,
									  sjinfo->semi_operators))
			{
				pathnode->umethod = UNIQUE_PATH_NOOP;
				pathnode->path.rows = rel->rows;
				pathnode->path.startup_cost = subpath->startup_cost;
				pathnode->path.total_cost = subpath->total_cost;
				pathnode->path.pathkeys = subpath->pathkeys;

				rel->cheapest_unique_path = (Path *) pathnode;

				MemoryContextSwitchTo(oldcontext);

				return pathnode;
			}
		}
	}

	/* Estimate number of output rows */
	pathnode->path.rows = estimate_num_groups(root,
											  sjinfo->semi_rhs_exprs,
											  rel->rows,
											  NULL);
	numCols = list_length(sjinfo->semi_rhs_exprs);

	if (sjinfo->semi_can_btree)
	{
		/*
		 * Estimate cost for sort+unique implementation
		 */
		cost_sort(&sort_path, root, NIL,
				  subpath->total_cost,
				  rel->rows,
				  subpath->pathtarget->width,
				  0.0,
				  work_mem,
				  -1.0);

		/*
		 * Charge one cpu_operator_cost per comparison per input tuple. We
		 * assume all columns get compared at most of the tuples. (XXX
		 * probably this is an overestimate.)  This should agree with
		 * create_upper_unique_path.
		 */
		sort_path.total_cost += cpu_operator_cost * rel->rows * numCols;
	}

	if (sjinfo->semi_can_hash)
	{
		/*
		 * Estimate the overhead per hashtable entry at 64 bytes (same as in
		 * planner.c).
		 */
		int			hashentrysize = subpath->pathtarget->width + 64;

		if (hashentrysize * pathnode->path.rows > work_mem * 1024L)
		{
			/*
			 * We should not try to hash.  Hack the SpecialJoinInfo to
			 * remember this, in case we come through here again.
			 */
			sjinfo->semi_can_hash = false;
		}
		else
			cost_agg(&agg_path, root,
					 AGG_HASHED, NULL,
					 numCols, pathnode->path.rows,
					 subpath->startup_cost,
					 subpath->total_cost,
					 rel->rows);
	}

	if (sjinfo->semi_can_btree && sjinfo->semi_can_hash)
	{
		if (agg_path.total_cost < sort_path.total_cost)
			pathnode->umethod = UNIQUE_PATH_HASH;
		else
			pathnode->umethod = UNIQUE_PATH_SORT;
	}
	else if (sjinfo->semi_can_btree)
		pathnode->umethod = UNIQUE_PATH_SORT;
	else if (sjinfo->semi_can_hash)
		pathnode->umethod = UNIQUE_PATH_HASH;
	else
	{
		/* we can get here only if we abandoned hashing above */
		MemoryContextSwitchTo(oldcontext);
		return NULL;
	}

	if (pathnode->umethod == UNIQUE_PATH_HASH)
	{
		pathnode->path.startup_cost = agg_path.startup_cost;
		pathnode->path.total_cost = agg_path.total_cost;
	}
	else
	{
		pathnode->path.startup_cost = sort_path.startup_cost;
		pathnode->path.total_cost = sort_path.total_cost;
	}

	rel->cheapest_unique_path = (Path *) pathnode;

	MemoryContextSwitchTo(oldcontext);

	return pathnode;
}

/*
 * translate_sub_tlist - get subquery column numbers represented by tlist
 *
 * The given targetlist usually contains only Vars referencing the given relid.
 * Extract their varattnos (ie, the column numbers of the subquery) and return
 * as an integer List.
 *
 * If any of the tlist items is not a simple Var, we cannot determine whether
 * the subquery's uniqueness condition (if any) matches ours, so punt and
 * return NIL.
 */
static List *
translate_sub_tlist(List *tlist, int relid)
{
	List	   *result = NIL;
	ListCell   *l;

	foreach(l, tlist)
	{
		Var		   *var = (Var *) lfirst(l);

		if (!var || !IsA(var, Var) ||
			var->varno != relid)
			return NIL;			/* punt */

		result = lappend_int(result, var->varattno);
	}
	return result;
}

/*
 * create_gather_path
 *	  Creates a path corresponding to a gather scan, returning the
 *	  pathnode.
 */
GatherPath *
create_gather_path(PlannerInfo *root, RelOptInfo *rel, Path *subpath,
				   Relids required_outer)
{
	GatherPath *pathnode = makeNode(GatherPath);

	Assert(subpath->parallel_safe);

	pathnode->path.pathtype = T_Gather;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = &(rel->reltarget);
	pathnode->path.param_info = get_baserel_parampathinfo(root, rel,
														  required_outer);
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = false;
	pathnode->path.parallel_degree = subpath->parallel_degree;
	pathnode->path.pathkeys = NIL;		/* Gather has unordered result */

	pathnode->subpath = subpath;
	pathnode->single_copy = false;

	if (pathnode->path.parallel_degree == 0)
	{
		pathnode->path.parallel_degree = 1;
		pathnode->path.pathkeys = subpath->pathkeys;
		pathnode->single_copy = true;
	}

	cost_gather(pathnode, root, rel, pathnode->path.param_info);

	return pathnode;
}

/*
 * create_subqueryscan_path
 *	  Creates a path corresponding to a scan of a subquery,
 *	  returning the pathnode.
 */
SubqueryScanPath *
create_subqueryscan_path(PlannerInfo *root, RelOptInfo *rel, Path *subpath,
						 List *pathkeys, Relids required_outer)
{
	SubqueryScanPath *pathnode = makeNode(SubqueryScanPath);

	pathnode->path.pathtype = T_SubqueryScan;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = &(rel->reltarget);
	pathnode->path.param_info = get_baserel_parampathinfo(root, rel,
														  required_outer);
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe;
	pathnode->path.parallel_degree = subpath->parallel_degree;
	pathnode->path.pathkeys = pathkeys;
	pathnode->subpath = subpath;

	cost_subqueryscan(pathnode, root, rel, pathnode->path.param_info);

	return pathnode;
}

/*
 * create_functionscan_path
 *	  Creates a path corresponding to a sequential scan of a function,
 *	  returning the pathnode.
 */
Path *
create_functionscan_path(PlannerInfo *root, RelOptInfo *rel,
						 List *pathkeys, Relids required_outer)
{
	Path	   *pathnode = makeNode(Path);

	pathnode->pathtype = T_FunctionScan;
	pathnode->parent = rel;
	pathnode->pathtarget = &(rel->reltarget);
	pathnode->param_info = get_baserel_parampathinfo(root, rel,
													 required_outer);
	pathnode->parallel_aware = false;
	pathnode->parallel_safe = rel->consider_parallel;
	pathnode->parallel_degree = 0;
	pathnode->pathkeys = pathkeys;

	cost_functionscan(pathnode, root, rel, pathnode->param_info);

	return pathnode;
}

/*
 * create_valuesscan_path
 *	  Creates a path corresponding to a scan of a VALUES list,
 *	  returning the pathnode.
 */
Path *
create_valuesscan_path(PlannerInfo *root, RelOptInfo *rel,
					   Relids required_outer)
{
	Path	   *pathnode = makeNode(Path);

	pathnode->pathtype = T_ValuesScan;
	pathnode->parent = rel;
	pathnode->pathtarget = &(rel->reltarget);
	pathnode->param_info = get_baserel_parampathinfo(root, rel,
													 required_outer);
	pathnode->parallel_aware = false;
	pathnode->parallel_safe = rel->consider_parallel;
	pathnode->parallel_degree = 0;
	pathnode->pathkeys = NIL;	/* result is always unordered */

	cost_valuesscan(pathnode, root, rel, pathnode->param_info);

	return pathnode;
}

/*
 * create_ctescan_path
 *	  Creates a path corresponding to a scan of a non-self-reference CTE,
 *	  returning the pathnode.
 */
Path *
create_ctescan_path(PlannerInfo *root, RelOptInfo *rel, Relids required_outer)
{
	Path	   *pathnode = makeNode(Path);

	pathnode->pathtype = T_CteScan;
	pathnode->parent = rel;
	pathnode->pathtarget = &(rel->reltarget);
	pathnode->param_info = get_baserel_parampathinfo(root, rel,
													 required_outer);
	pathnode->parallel_aware = false;
	pathnode->parallel_safe = rel->consider_parallel;
	pathnode->parallel_degree = 0;
	pathnode->pathkeys = NIL;	/* XXX for now, result is always unordered */

	cost_ctescan(pathnode, root, rel, pathnode->param_info);

	return pathnode;
}

/*
 * create_worktablescan_path
 *	  Creates a path corresponding to a scan of a self-reference CTE,
 *	  returning the pathnode.
 */
Path *
create_worktablescan_path(PlannerInfo *root, RelOptInfo *rel,
						  Relids required_outer)
{
	Path	   *pathnode = makeNode(Path);

	pathnode->pathtype = T_WorkTableScan;
	pathnode->parent = rel;
	pathnode->pathtarget = &(rel->reltarget);
	pathnode->param_info = get_baserel_parampathinfo(root, rel,
													 required_outer);
	pathnode->parallel_aware = false;
	pathnode->parallel_safe = rel->consider_parallel;
	pathnode->parallel_degree = 0;
	pathnode->pathkeys = NIL;	/* result is always unordered */

	/* Cost is the same as for a regular CTE scan */
	cost_ctescan(pathnode, root, rel, pathnode->param_info);

	return pathnode;
}

/*
 * create_foreignscan_path
 *	  Creates a path corresponding to a scan of a foreign table or
 *	  a foreign join, returning the pathnode.
 *
 * This function is never called from core Postgres; rather, it's expected
 * to be called by the GetForeignPaths or GetForeignJoinPaths function of
 * a foreign data wrapper.  We make the FDW supply all fields of the path,
 * since we do not have any way to calculate them in core.
 */
ForeignPath *
create_foreignscan_path(PlannerInfo *root, RelOptInfo *rel,
						double rows, Cost startup_cost, Cost total_cost,
						List *pathkeys,
						Relids required_outer,
						Path *fdw_outerpath,
						List *fdw_private)
{
	ForeignPath *pathnode = makeNode(ForeignPath);

	pathnode->path.pathtype = T_ForeignScan;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = &(rel->reltarget);
	pathnode->path.param_info = get_baserel_parampathinfo(root, rel,
														  required_outer);
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel;
	pathnode->path.parallel_degree = 0;
	pathnode->path.rows = rows;
	pathnode->path.startup_cost = startup_cost;
	pathnode->path.total_cost = total_cost;
	pathnode->path.pathkeys = pathkeys;

	pathnode->fdw_outerpath = fdw_outerpath;
	pathnode->fdw_private = fdw_private;

	return pathnode;
}

/*
 * calc_nestloop_required_outer
 *	  Compute the required_outer set for a nestloop join path
 *
 * Note: result must not share storage with either input
 */
Relids
calc_nestloop_required_outer(Path *outer_path, Path *inner_path)
{
	Relids		outer_paramrels = PATH_REQ_OUTER(outer_path);
	Relids		inner_paramrels = PATH_REQ_OUTER(inner_path);
	Relids		required_outer;

	/* inner_path can require rels from outer path, but not vice versa */
	Assert(!bms_overlap(outer_paramrels, inner_path->parent->relids));
	/* easy case if inner path is not parameterized */
	if (!inner_paramrels)
		return bms_copy(outer_paramrels);
	/* else, form the union ... */
	required_outer = bms_union(outer_paramrels, inner_paramrels);
	/* ... and remove any mention of now-satisfied outer rels */
	required_outer = bms_del_members(required_outer,
									 outer_path->parent->relids);
	/* maintain invariant that required_outer is exactly NULL if empty */
	if (bms_is_empty(required_outer))
	{
		bms_free(required_outer);
		required_outer = NULL;
	}
	return required_outer;
}

/*
 * calc_non_nestloop_required_outer
 *	  Compute the required_outer set for a merge or hash join path
 *
 * Note: result must not share storage with either input
 */
Relids
calc_non_nestloop_required_outer(Path *outer_path, Path *inner_path)
{
	Relids		outer_paramrels = PATH_REQ_OUTER(outer_path);
	Relids		inner_paramrels = PATH_REQ_OUTER(inner_path);
	Relids		required_outer;

	/* neither path can require rels from the other */
	Assert(!bms_overlap(outer_paramrels, inner_path->parent->relids));
	Assert(!bms_overlap(inner_paramrels, outer_path->parent->relids));
	/* form the union ... */
	required_outer = bms_union(outer_paramrels, inner_paramrels);
	/* we do not need an explicit test for empty; bms_union gets it right */
	return required_outer;
}

/*
 * create_nestloop_path
 *	  Creates a pathnode corresponding to a nestloop join between two
 *	  relations.
 *
 * 'joinrel' is the join relation.
 * 'jointype' is the type of join required
 * 'workspace' is the result from initial_cost_nestloop
 * 'sjinfo' is extra info about the join for selectivity estimation
 * 'semifactors' contains valid data if jointype is SEMI or ANTI
 * 'outer_path' is the outer path
 * 'inner_path' is the inner path
 * 'restrict_clauses' are the RestrictInfo nodes to apply at the join
 * 'pathkeys' are the path keys of the new join path
 * 'required_outer' is the set of required outer rels
 *
 * Returns the resulting path node.
 */
NestPath *
create_nestloop_path(PlannerInfo *root,
					 RelOptInfo *joinrel,
					 JoinType jointype,
					 JoinCostWorkspace *workspace,
					 SpecialJoinInfo *sjinfo,
					 SemiAntiJoinFactors *semifactors,
					 Path *outer_path,
					 Path *inner_path,
					 List *restrict_clauses,
					 List *pathkeys,
					 Relids required_outer)
{
	NestPath   *pathnode = makeNode(NestPath);
	Relids		inner_req_outer = PATH_REQ_OUTER(inner_path);

	/*
	 * If the inner path is parameterized by the outer, we must drop any
	 * restrict_clauses that are due to be moved into the inner path.  We have
	 * to do this now, rather than postpone the work till createplan time,
	 * because the restrict_clauses list can affect the size and cost
	 * estimates for this path.
	 */
	if (bms_overlap(inner_req_outer, outer_path->parent->relids))
	{
		Relids		inner_and_outer = bms_union(inner_path->parent->relids,
												inner_req_outer);
		List	   *jclauses = NIL;
		ListCell   *lc;

		foreach(lc, restrict_clauses)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

			if (!join_clause_is_movable_into(rinfo,
											 inner_path->parent->relids,
											 inner_and_outer))
				jclauses = lappend(jclauses, rinfo);
		}
		restrict_clauses = jclauses;
	}

	pathnode->path.pathtype = T_NestLoop;
	pathnode->path.parent = joinrel;
	pathnode->path.pathtarget = &(joinrel->reltarget);
	pathnode->path.param_info =
		get_joinrel_parampathinfo(root,
								  joinrel,
								  outer_path,
								  inner_path,
								  sjinfo,
								  required_outer,
								  &restrict_clauses);
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = joinrel->consider_parallel &&
		outer_path->parallel_safe && inner_path->parallel_safe;
	/* This is a foolish way to estimate parallel_degree, but for now... */
	pathnode->path.parallel_degree = outer_path->parallel_degree;
	pathnode->path.pathkeys = pathkeys;
	pathnode->jointype = jointype;
	pathnode->outerjoinpath = outer_path;
	pathnode->innerjoinpath = inner_path;
	pathnode->joinrestrictinfo = restrict_clauses;

	final_cost_nestloop(root, pathnode, workspace, sjinfo, semifactors);

	return pathnode;
}

/*
 * create_mergejoin_path
 *	  Creates a pathnode corresponding to a mergejoin join between
 *	  two relations
 *
 * 'joinrel' is the join relation
 * 'jointype' is the type of join required
 * 'workspace' is the result from initial_cost_mergejoin
 * 'sjinfo' is extra info about the join for selectivity estimation
 * 'outer_path' is the outer path
 * 'inner_path' is the inner path
 * 'restrict_clauses' are the RestrictInfo nodes to apply at the join
 * 'pathkeys' are the path keys of the new join path
 * 'required_outer' is the set of required outer rels
 * 'mergeclauses' are the RestrictInfo nodes to use as merge clauses
 *		(this should be a subset of the restrict_clauses list)
 * 'outersortkeys' are the sort varkeys for the outer relation
 * 'innersortkeys' are the sort varkeys for the inner relation
 */
MergePath *
create_mergejoin_path(PlannerInfo *root,
					  RelOptInfo *joinrel,
					  JoinType jointype,
					  JoinCostWorkspace *workspace,
					  SpecialJoinInfo *sjinfo,
					  Path *outer_path,
					  Path *inner_path,
					  List *restrict_clauses,
					  List *pathkeys,
					  Relids required_outer,
					  List *mergeclauses,
					  List *outersortkeys,
					  List *innersortkeys)
{
	MergePath  *pathnode = makeNode(MergePath);

	pathnode->jpath.path.pathtype = T_MergeJoin;
	pathnode->jpath.path.parent = joinrel;
	pathnode->jpath.path.pathtarget = &(joinrel->reltarget);
	pathnode->jpath.path.param_info =
		get_joinrel_parampathinfo(root,
								  joinrel,
								  outer_path,
								  inner_path,
								  sjinfo,
								  required_outer,
								  &restrict_clauses);
	pathnode->jpath.path.parallel_aware = false;
	pathnode->jpath.path.parallel_safe = joinrel->consider_parallel &&
		outer_path->parallel_safe && inner_path->parallel_safe;
	/* This is a foolish way to estimate parallel_degree, but for now... */
	pathnode->jpath.path.parallel_degree = outer_path->parallel_degree;
	pathnode->jpath.path.pathkeys = pathkeys;
	pathnode->jpath.jointype = jointype;
	pathnode->jpath.outerjoinpath = outer_path;
	pathnode->jpath.innerjoinpath = inner_path;
	pathnode->jpath.joinrestrictinfo = restrict_clauses;
	pathnode->path_mergeclauses = mergeclauses;
	pathnode->outersortkeys = outersortkeys;
	pathnode->innersortkeys = innersortkeys;
	/* pathnode->materialize_inner will be set by final_cost_mergejoin */

	final_cost_mergejoin(root, pathnode, workspace, sjinfo);

	return pathnode;
}

/*
 * create_hashjoin_path
 *	  Creates a pathnode corresponding to a hash join between two relations.
 *
 * 'joinrel' is the join relation
 * 'jointype' is the type of join required
 * 'workspace' is the result from initial_cost_hashjoin
 * 'sjinfo' is extra info about the join for selectivity estimation
 * 'semifactors' contains valid data if jointype is SEMI or ANTI
 * 'outer_path' is the cheapest outer path
 * 'inner_path' is the cheapest inner path
 * 'restrict_clauses' are the RestrictInfo nodes to apply at the join
 * 'required_outer' is the set of required outer rels
 * 'hashclauses' are the RestrictInfo nodes to use as hash clauses
 *		(this should be a subset of the restrict_clauses list)
 */
HashPath *
create_hashjoin_path(PlannerInfo *root,
					 RelOptInfo *joinrel,
					 JoinType jointype,
					 JoinCostWorkspace *workspace,
					 SpecialJoinInfo *sjinfo,
					 SemiAntiJoinFactors *semifactors,
					 Path *outer_path,
					 Path *inner_path,
					 List *restrict_clauses,
					 Relids required_outer,
					 List *hashclauses)
{
	HashPath   *pathnode = makeNode(HashPath);

	pathnode->jpath.path.pathtype = T_HashJoin;
	pathnode->jpath.path.parent = joinrel;
	pathnode->jpath.path.pathtarget = &(joinrel->reltarget);
	pathnode->jpath.path.param_info =
		get_joinrel_parampathinfo(root,
								  joinrel,
								  outer_path,
								  inner_path,
								  sjinfo,
								  required_outer,
								  &restrict_clauses);
	pathnode->jpath.path.parallel_aware = false;
	pathnode->jpath.path.parallel_safe = joinrel->consider_parallel &&
		outer_path->parallel_safe && inner_path->parallel_safe;
	/* This is a foolish way to estimate parallel_degree, but for now... */
	pathnode->jpath.path.parallel_degree = outer_path->parallel_degree;

	/*
	 * A hashjoin never has pathkeys, since its output ordering is
	 * unpredictable due to possible batching.  XXX If the inner relation is
	 * small enough, we could instruct the executor that it must not batch,
	 * and then we could assume that the output inherits the outer relation's
	 * ordering, which might save a sort step.  However there is considerable
	 * downside if our estimate of the inner relation size is badly off. For
	 * the moment we don't risk it.  (Note also that if we wanted to take this
	 * seriously, joinpath.c would have to consider many more paths for the
	 * outer rel than it does now.)
	 */
	pathnode->jpath.path.pathkeys = NIL;
	pathnode->jpath.jointype = jointype;
	pathnode->jpath.outerjoinpath = outer_path;
	pathnode->jpath.innerjoinpath = inner_path;
	pathnode->jpath.joinrestrictinfo = restrict_clauses;
	pathnode->path_hashclauses = hashclauses;
	/* final_cost_hashjoin will fill in pathnode->num_batches */

	final_cost_hashjoin(root, pathnode, workspace, sjinfo, semifactors);

	return pathnode;
}

/*
 * create_projection_path
 *	  Creates a pathnode that represents performing a projection.
 *
 * 'rel' is the parent relation associated with the result
 * 'subpath' is the path representing the source of data
 * 'target' is the PathTarget to be computed
 */
ProjectionPath *
create_projection_path(PlannerInfo *root,
					   RelOptInfo *rel,
					   Path *subpath,
					   PathTarget *target)
{
	ProjectionPath *pathnode = makeNode(ProjectionPath);

	pathnode->path.pathtype = T_Result;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = target;
	/* For now, assume we are above any joins, so no parameterization */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe;
	pathnode->path.parallel_degree = subpath->parallel_degree;
	/* Projection does not change the sort order */
	pathnode->path.pathkeys = subpath->pathkeys;

	pathnode->subpath = subpath;

	/*
	 * The Result node's cost is cpu_tuple_cost per row, plus the cost of
	 * evaluating the tlist.  There is no qual to worry about.
	 */
	pathnode->path.rows = subpath->rows;
	pathnode->path.startup_cost = subpath->startup_cost + target->cost.startup;
	pathnode->path.total_cost = subpath->total_cost + target->cost.startup +
		(cpu_tuple_cost + target->cost.per_tuple) * subpath->rows;

	return pathnode;
}

/*
 * apply_projection_to_path
 *	  Add a projection step, or just apply the target directly to given path.
 *
 * Most plan types include ExecProject, so we can implement a new projection
 * without an extra plan node: just replace the given path's pathtarget with
 * the desired one.  If the given path can't project, add a ProjectionPath.
 *
 * We can also short-circuit cases where the targetlist expressions are
 * actually equal; this is not an uncommon case, since it may arise from
 * trying to apply a PathTarget with sortgroupref labeling to a derived
 * path without such labeling.
 *
 * This requires knowing that the source path won't be referenced for other
 * purposes (e.g., other possible paths), since we modify it in-place.  Note
 * also that we mustn't change the source path's parent link; so when it is
 * add_path'd to "rel" things will be a bit inconsistent.  So far that has
 * not caused any trouble.
 *
 * 'rel' is the parent relation associated with the result
 * 'path' is the path representing the source of data
 * 'target' is the PathTarget to be computed
 */
Path *
apply_projection_to_path(PlannerInfo *root,
						 RelOptInfo *rel,
						 Path *path,
						 PathTarget *target)
{
	QualCost	oldcost;

	/* Make a separate ProjectionPath if needed */
	if (!is_projection_capable_path(path) &&
		!equal(path->pathtarget->exprs, target->exprs))
		return (Path *) create_projection_path(root, rel, path, target);

	/*
	 * We can just jam the desired tlist into the existing path, being sure to
	 * update its cost estimates appropriately.
	 */
	oldcost = path->pathtarget->cost;
	path->pathtarget = target;

	path->startup_cost += target->cost.startup - oldcost.startup;
	path->total_cost += target->cost.startup - oldcost.startup +
		(target->cost.per_tuple - oldcost.per_tuple) * path->rows;

	return path;
}

/*
 * create_sort_path
 *	  Creates a pathnode that represents performing an explicit sort.
 *
 * 'rel' is the parent relation associated with the result
 * 'subpath' is the path representing the source of data
 * 'pathkeys' represents the desired sort order
 * 'limit_tuples' is the estimated bound on the number of output tuples,
 *		or -1 if no LIMIT or couldn't estimate
 */
SortPath *
create_sort_path(PlannerInfo *root,
				 RelOptInfo *rel,
				 Path *subpath,
				 List *pathkeys,
				 double limit_tuples)
{
	SortPath   *pathnode = makeNode(SortPath);

	pathnode->path.pathtype = T_Sort;
	pathnode->path.parent = rel;
	/* Sort doesn't project, so use source path's pathtarget */
	pathnode->path.pathtarget = subpath->pathtarget;
	/* For now, assume we are above any joins, so no parameterization */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe;
	pathnode->path.parallel_degree = subpath->parallel_degree;
	pathnode->path.pathkeys = pathkeys;

	pathnode->subpath = subpath;

	cost_sort(&pathnode->path, root, pathkeys,
			  subpath->total_cost,
			  subpath->rows,
			  subpath->pathtarget->width,
			  0.0,				/* XXX comparison_cost shouldn't be 0? */
			  work_mem, limit_tuples);

	return pathnode;
}

/*
 * create_group_path
 *	  Creates a pathnode that represents performing grouping of presorted input
 *
 * 'rel' is the parent relation associated with the result
 * 'subpath' is the path representing the source of data
 * 'target' is the PathTarget to be computed
 * 'groupClause' is a list of SortGroupClause's representing the grouping
 * 'qual' is the HAVING quals if any
 * 'numGroups' is the estimated number of groups
 */
GroupPath *
create_group_path(PlannerInfo *root,
				  RelOptInfo *rel,
				  Path *subpath,
				  PathTarget *target,
				  List *groupClause,
				  List *qual,
				  double numGroups)
{
	GroupPath  *pathnode = makeNode(GroupPath);

	pathnode->path.pathtype = T_Group;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = target;
	/* For now, assume we are above any joins, so no parameterization */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe;
	pathnode->path.parallel_degree = subpath->parallel_degree;
	/* Group doesn't change sort ordering */
	pathnode->path.pathkeys = subpath->pathkeys;

	pathnode->subpath = subpath;

	pathnode->groupClause = groupClause;
	pathnode->qual = qual;

	cost_group(&pathnode->path, root,
			   list_length(groupClause),
			   numGroups,
			   subpath->startup_cost, subpath->total_cost,
			   subpath->rows);

	/* add tlist eval cost for each output row */
	pathnode->path.startup_cost += target->cost.startup;
	pathnode->path.total_cost += target->cost.startup +
		target->cost.per_tuple * pathnode->path.rows;

	return pathnode;
}

/*
 * create_upper_unique_path
 *	  Creates a pathnode that represents performing an explicit Unique step
 *	  on presorted input.
 *
 * This produces a Unique plan node, but the use-case is so different from
 * create_unique_path that it doesn't seem worth trying to merge the two.
 *
 * 'rel' is the parent relation associated with the result
 * 'subpath' is the path representing the source of data
 * 'numCols' is the number of grouping columns
 * 'numGroups' is the estimated number of groups
 *
 * The input path must be sorted on the grouping columns, plus possibly
 * additional columns; so the first numCols pathkeys are the grouping columns
 */
UpperUniquePath *
create_upper_unique_path(PlannerInfo *root,
						 RelOptInfo *rel,
						 Path *subpath,
						 int numCols,
						 double numGroups)
{
	UpperUniquePath *pathnode = makeNode(UpperUniquePath);

	pathnode->path.pathtype = T_Unique;
	pathnode->path.parent = rel;
	/* Unique doesn't project, so use source path's pathtarget */
	pathnode->path.pathtarget = subpath->pathtarget;
	/* For now, assume we are above any joins, so no parameterization */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe;
	pathnode->path.parallel_degree = subpath->parallel_degree;
	/* Unique doesn't change the input ordering */
	pathnode->path.pathkeys = subpath->pathkeys;

	pathnode->subpath = subpath;
	pathnode->numkeys = numCols;

	/*
	 * Charge one cpu_operator_cost per comparison per input tuple. We assume
	 * all columns get compared at most of the tuples.  (XXX probably this is
	 * an overestimate.)
	 */
	pathnode->path.startup_cost = subpath->startup_cost;
	pathnode->path.total_cost = subpath->total_cost +
		cpu_operator_cost * subpath->rows * numCols;
	pathnode->path.rows = numGroups;

	return pathnode;
}

/*
 * create_agg_path
 *	  Creates a pathnode that represents performing aggregation/grouping
 *
 * 'rel' is the parent relation associated with the result
 * 'subpath' is the path representing the source of data
 * 'target' is the PathTarget to be computed
 * 'aggstrategy' is the Agg node's basic implementation strategy
 * 'groupClause' is a list of SortGroupClause's representing the grouping
 * 'qual' is the HAVING quals if any
 * 'aggcosts' contains cost info about the aggregate functions to be computed
 * 'numGroups' is the estimated number of groups (1 if not grouping)
 */
AggPath *
create_agg_path(PlannerInfo *root,
				RelOptInfo *rel,
				Path *subpath,
				PathTarget *target,
				AggStrategy aggstrategy,
				List *groupClause,
				List *qual,
				const AggClauseCosts *aggcosts,
				double numGroups)
{
	AggPath    *pathnode = makeNode(AggPath);

	pathnode->path.pathtype = T_Agg;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = target;
	/* For now, assume we are above any joins, so no parameterization */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe;
	pathnode->path.parallel_degree = subpath->parallel_degree;
	if (aggstrategy == AGG_SORTED)
		pathnode->path.pathkeys = subpath->pathkeys;	/* preserves order */
	else
		pathnode->path.pathkeys = NIL;	/* output is unordered */
	pathnode->subpath = subpath;

	pathnode->aggstrategy = aggstrategy;
	pathnode->numGroups = numGroups;
	pathnode->groupClause = groupClause;
	pathnode->qual = qual;

	cost_agg(&pathnode->path, root,
			 aggstrategy, aggcosts,
			 list_length(groupClause), numGroups,
			 subpath->startup_cost, subpath->total_cost,
			 subpath->rows);

	/* add tlist eval cost for each output row */
	pathnode->path.startup_cost += target->cost.startup;
	pathnode->path.total_cost += target->cost.startup +
		target->cost.per_tuple * pathnode->path.rows;

	return pathnode;
}

/*
 * create_groupingsets_path
 *	  Creates a pathnode that represents performing GROUPING SETS aggregation
 *
 * GroupingSetsPath represents sorted grouping with one or more grouping sets.
 * The input path's result must be sorted to match the last entry in
 * rollup_groupclauses.
 *
 * 'rel' is the parent relation associated with the result
 * 'subpath' is the path representing the source of data
 * 'target' is the PathTarget to be computed
 * 'having_qual' is the HAVING quals if any
 * 'rollup_lists' is a list of grouping sets
 * 'rollup_groupclauses' is a list of grouping clauses for grouping sets
 * 'agg_costs' contains cost info about the aggregate functions to be computed
 * 'numGroups' is the estimated number of groups
 */
GroupingSetsPath *
create_groupingsets_path(PlannerInfo *root,
						 RelOptInfo *rel,
						 Path *subpath,
						 PathTarget *target,
						 List *having_qual,
						 List *rollup_lists,
						 List *rollup_groupclauses,
						 const AggClauseCosts *agg_costs,
						 double numGroups)
{
	GroupingSetsPath *pathnode = makeNode(GroupingSetsPath);
	int			numGroupCols;

	/* The topmost generated Plan node will be an Agg */
	pathnode->path.pathtype = T_Agg;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = target;
	pathnode->path.param_info = subpath->param_info;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe;
	pathnode->path.parallel_degree = subpath->parallel_degree;
	pathnode->subpath = subpath;

	/*
	 * Output will be in sorted order by group_pathkeys if, and only if, there
	 * is a single rollup operation on a non-empty list of grouping
	 * expressions.
	 */
	if (list_length(rollup_groupclauses) == 1 &&
		((List *) linitial(rollup_groupclauses)) != NIL)
		pathnode->path.pathkeys = root->group_pathkeys;
	else
		pathnode->path.pathkeys = NIL;

	pathnode->rollup_groupclauses = rollup_groupclauses;
	pathnode->rollup_lists = rollup_lists;
	pathnode->qual = having_qual;

	Assert(rollup_lists != NIL);
	Assert(list_length(rollup_lists) == list_length(rollup_groupclauses));

	/* Account for cost of the topmost Agg node */
	numGroupCols = list_length((List *) linitial((List *) llast(rollup_lists)));

	cost_agg(&pathnode->path, root,
			 (numGroupCols > 0) ? AGG_SORTED : AGG_PLAIN,
			 agg_costs,
			 numGroupCols,
			 numGroups,
			 subpath->startup_cost,
			 subpath->total_cost,
			 subpath->rows);

	/*
	 * Add in the costs and output rows of the additional sorting/aggregation
	 * steps, if any.  Only total costs count, since the extra sorts aren't
	 * run on startup.
	 */
	if (list_length(rollup_lists) > 1)
	{
		ListCell   *lc;

		foreach(lc, rollup_lists)
		{
			List	   *gsets = (List *) lfirst(lc);
			Path		sort_path;		/* dummy for result of cost_sort */
			Path		agg_path;		/* dummy for result of cost_agg */

			/* We must iterate over all but the last rollup_lists element */
			if (lnext(lc) == NULL)
				break;

			/* Account for cost of sort, but don't charge input cost again */
			cost_sort(&sort_path, root, NIL,
					  0.0,
					  subpath->rows,
					  subpath->pathtarget->width,
					  0.0,
					  work_mem,
					  -1.0);

			/* Account for cost of aggregation */
			numGroupCols = list_length((List *) linitial(gsets));

			cost_agg(&agg_path, root,
					 AGG_SORTED,
					 agg_costs,
					 numGroupCols,
					 numGroups, /* XXX surely not right for all steps? */
					 sort_path.startup_cost,
					 sort_path.total_cost,
					 sort_path.rows);

			pathnode->path.total_cost += agg_path.total_cost;
			pathnode->path.rows += agg_path.rows;
		}
	}

	/* add tlist eval cost for each output row */
	pathnode->path.startup_cost += target->cost.startup;
	pathnode->path.total_cost += target->cost.startup +
		target->cost.per_tuple * pathnode->path.rows;

	return pathnode;
}

/*
 * create_minmaxagg_path
 *	  Creates a pathnode that represents computation of MIN/MAX aggregates
 *
 * 'rel' is the parent relation associated with the result
 * 'target' is the PathTarget to be computed
 * 'mmaggregates' is a list of MinMaxAggInfo structs
 * 'quals' is the HAVING quals if any
 */
MinMaxAggPath *
create_minmaxagg_path(PlannerInfo *root,
					  RelOptInfo *rel,
					  PathTarget *target,
					  List *mmaggregates,
					  List *quals)
{
	MinMaxAggPath *pathnode = makeNode(MinMaxAggPath);
	Cost		initplan_cost;
	ListCell   *lc;

	/* The topmost generated Plan node will be a Result */
	pathnode->path.pathtype = T_Result;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = target;
	/* For now, assume we are above any joins, so no parameterization */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	/* A MinMaxAggPath implies use of subplans, so cannot be parallel-safe */
	pathnode->path.parallel_safe = false;
	pathnode->path.parallel_degree = 0;
	/* Result is one unordered row */
	pathnode->path.rows = 1;
	pathnode->path.pathkeys = NIL;

	pathnode->mmaggregates = mmaggregates;
	pathnode->quals = quals;

	/* Calculate cost of all the initplans ... */
	initplan_cost = 0;
	foreach(lc, mmaggregates)
	{
		MinMaxAggInfo *mminfo = (MinMaxAggInfo *) lfirst(lc);

		initplan_cost += mminfo->pathcost;
	}

	/* add tlist eval cost for each output row, plus cpu_tuple_cost */
	pathnode->path.startup_cost = initplan_cost + target->cost.startup;
	pathnode->path.total_cost = initplan_cost + target->cost.startup +
		target->cost.per_tuple + cpu_tuple_cost;

	return pathnode;
}

/*
 * create_windowagg_path
 *	  Creates a pathnode that represents computation of window functions
 *
 * 'rel' is the parent relation associated with the result
 * 'subpath' is the path representing the source of data
 * 'target' is the PathTarget to be computed
 * 'windowFuncs' is a list of WindowFunc structs
 * 'winclause' is a WindowClause that is common to all the WindowFuncs
 * 'winpathkeys' is the pathkeys for the PARTITION keys + ORDER keys
 *
 * The actual sort order of the input must match winpathkeys, but might
 * have additional keys after those.
 */
WindowAggPath *
create_windowagg_path(PlannerInfo *root,
					  RelOptInfo *rel,
					  Path *subpath,
					  PathTarget *target,
					  List *windowFuncs,
					  WindowClause *winclause,
					  List *winpathkeys)
{
	WindowAggPath *pathnode = makeNode(WindowAggPath);

	pathnode->path.pathtype = T_WindowAgg;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = target;
	/* For now, assume we are above any joins, so no parameterization */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe;
	pathnode->path.parallel_degree = subpath->parallel_degree;
	/* WindowAgg preserves the input sort order */
	pathnode->path.pathkeys = subpath->pathkeys;

	pathnode->subpath = subpath;
	pathnode->winclause = winclause;
	pathnode->winpathkeys = winpathkeys;

	/*
	 * For costing purposes, assume that there are no redundant partitioning
	 * or ordering columns; it's not worth the trouble to deal with that
	 * corner case here.  So we just pass the unmodified list lengths to
	 * cost_windowagg.
	 */
	cost_windowagg(&pathnode->path, root,
				   windowFuncs,
				   list_length(winclause->partitionClause),
				   list_length(winclause->orderClause),
				   subpath->startup_cost,
				   subpath->total_cost,
				   subpath->rows);

	/* add tlist eval cost for each output row */
	pathnode->path.startup_cost += target->cost.startup;
	pathnode->path.total_cost += target->cost.startup +
		target->cost.per_tuple * pathnode->path.rows;

	return pathnode;
}

/*
 * create_setop_path
 *	  Creates a pathnode that represents computation of INTERSECT or EXCEPT
 *
 * 'rel' is the parent relation associated with the result
 * 'subpath' is the path representing the source of data
 * 'cmd' is the specific semantics (INTERSECT or EXCEPT, with/without ALL)
 * 'strategy' is the implementation strategy (sorted or hashed)
 * 'distinctList' is a list of SortGroupClause's representing the grouping
 * 'flagColIdx' is the column number where the flag column will be, if any
 * 'firstFlag' is the flag value for the first input relation when hashing;
 *		or -1 when sorting
 * 'numGroups' is the estimated number of distinct groups
 * 'outputRows' is the estimated number of output rows
 */
SetOpPath *
create_setop_path(PlannerInfo *root,
				  RelOptInfo *rel,
				  Path *subpath,
				  SetOpCmd cmd,
				  SetOpStrategy strategy,
				  List *distinctList,
				  AttrNumber flagColIdx,
				  int firstFlag,
				  double numGroups,
				  double outputRows)
{
	SetOpPath  *pathnode = makeNode(SetOpPath);

	pathnode->path.pathtype = T_SetOp;
	pathnode->path.parent = rel;
	/* SetOp doesn't project, so use source path's pathtarget */
	pathnode->path.pathtarget = subpath->pathtarget;
	/* For now, assume we are above any joins, so no parameterization */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe;
	pathnode->path.parallel_degree = subpath->parallel_degree;
	/* SetOp preserves the input sort order if in sort mode */
	pathnode->path.pathkeys =
		(strategy == SETOP_SORTED) ? subpath->pathkeys : NIL;

	pathnode->subpath = subpath;
	pathnode->cmd = cmd;
	pathnode->strategy = strategy;
	pathnode->distinctList = distinctList;
	pathnode->flagColIdx = flagColIdx;
	pathnode->firstFlag = firstFlag;
	pathnode->numGroups = numGroups;

	/*
	 * Charge one cpu_operator_cost per comparison per input tuple. We assume
	 * all columns get compared at most of the tuples.
	 */
	pathnode->path.startup_cost = subpath->startup_cost;
	pathnode->path.total_cost = subpath->total_cost +
		cpu_operator_cost * subpath->rows * list_length(distinctList);
	pathnode->path.rows = outputRows;

	return pathnode;
}

/*
 * create_recursiveunion_path
 *	  Creates a pathnode that represents a recursive UNION node
 *
 * 'rel' is the parent relation associated with the result
 * 'leftpath' is the source of data for the non-recursive term
 * 'rightpath' is the source of data for the recursive term
 * 'target' is the PathTarget to be computed
 * 'distinctList' is a list of SortGroupClause's representing the grouping
 * 'wtParam' is the ID of Param representing work table
 * 'numGroups' is the estimated number of groups
 *
 * For recursive UNION ALL, distinctList is empty and numGroups is zero
 */
RecursiveUnionPath *
create_recursiveunion_path(PlannerInfo *root,
						   RelOptInfo *rel,
						   Path *leftpath,
						   Path *rightpath,
						   PathTarget *target,
						   List *distinctList,
						   int wtParam,
						   double numGroups)
{
	RecursiveUnionPath *pathnode = makeNode(RecursiveUnionPath);

	pathnode->path.pathtype = T_RecursiveUnion;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = target;
	/* For now, assume we are above any joins, so no parameterization */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		leftpath->parallel_safe && rightpath->parallel_safe;
	/* Foolish, but we'll do it like joins for now: */
	pathnode->path.parallel_degree = leftpath->parallel_degree;
	/* RecursiveUnion result is always unsorted */
	pathnode->path.pathkeys = NIL;

	pathnode->leftpath = leftpath;
	pathnode->rightpath = rightpath;
	pathnode->distinctList = distinctList;
	pathnode->wtParam = wtParam;
	pathnode->numGroups = numGroups;

	cost_recursive_union(&pathnode->path, leftpath, rightpath);

	return pathnode;
}

/*
 * create_lockrows_path
 *	  Creates a pathnode that represents acquiring row locks
 *
 * 'rel' is the parent relation associated with the result
 * 'subpath' is the path representing the source of data
 * 'rowMarks' is a list of PlanRowMark's
 * 'epqParam' is the ID of Param for EvalPlanQual re-eval
 */
LockRowsPath *
create_lockrows_path(PlannerInfo *root, RelOptInfo *rel,
					 Path *subpath, List *rowMarks, int epqParam)
{
	LockRowsPath *pathnode = makeNode(LockRowsPath);

	pathnode->path.pathtype = T_LockRows;
	pathnode->path.parent = rel;
	/* LockRows doesn't project, so use source path's pathtarget */
	pathnode->path.pathtarget = subpath->pathtarget;
	/* For now, assume we are above any joins, so no parameterization */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = false;
	pathnode->path.parallel_degree = 0;
	pathnode->path.rows = subpath->rows;

	/*
	 * The result cannot be assumed sorted, since locking might cause the sort
	 * key columns to be replaced with new values.
	 */
	pathnode->path.pathkeys = NIL;

	pathnode->subpath = subpath;
	pathnode->rowMarks = rowMarks;
	pathnode->epqParam = epqParam;

	/*
	 * We should charge something extra for the costs of row locking and
	 * possible refetches, but it's hard to say how much.  For now, use
	 * cpu_tuple_cost per row.
	 */
	pathnode->path.startup_cost = subpath->startup_cost;
	pathnode->path.total_cost = subpath->total_cost +
		cpu_tuple_cost * subpath->rows;

	return pathnode;
}

/*
 * create_modifytable_path
 *	  Creates a pathnode that represents performing INSERT/UPDATE/DELETE mods
 *
 * 'rel' is the parent relation associated with the result
 * 'operation' is the operation type
 * 'canSetTag' is true if we set the command tag/es_processed
 * 'nominalRelation' is the parent RT index for use of EXPLAIN
 * 'resultRelations' is an integer list of actual RT indexes of target rel(s)
 * 'subpaths' is a list of Path(s) producing source data (one per rel)
 * 'subroots' is a list of PlannerInfo structs (one per rel)
 * 'withCheckOptionLists' is a list of WCO lists (one per rel)
 * 'returningLists' is a list of RETURNING tlists (one per rel)
 * 'rowMarks' is a list of PlanRowMarks (non-locking only)
 * 'onconflict' is the ON CONFLICT clause, or NULL
 * 'epqParam' is the ID of Param for EvalPlanQual re-eval
 */
ModifyTablePath *
create_modifytable_path(PlannerInfo *root, RelOptInfo *rel,
						CmdType operation, bool canSetTag,
						Index nominalRelation,
						List *resultRelations, List *subpaths,
						List *subroots,
						List *withCheckOptionLists, List *returningLists,
						List *rowMarks, OnConflictExpr *onconflict,
						int epqParam)
{
	ModifyTablePath *pathnode = makeNode(ModifyTablePath);
	double		total_size;
	ListCell   *lc;

	Assert(list_length(resultRelations) == list_length(subpaths));
	Assert(list_length(resultRelations) == list_length(subroots));
	Assert(withCheckOptionLists == NIL ||
		   list_length(resultRelations) == list_length(withCheckOptionLists));
	Assert(returningLists == NIL ||
		   list_length(resultRelations) == list_length(returningLists));

	pathnode->path.pathtype = T_ModifyTable;
	pathnode->path.parent = rel;
	/* pathtarget is not interesting, just make it minimally valid */
	pathnode->path.pathtarget = &(rel->reltarget);
	/* For now, assume we are above any joins, so no parameterization */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = false;
	pathnode->path.parallel_degree = 0;
	pathnode->path.pathkeys = NIL;

	/*
	 * Compute cost & rowcount as sum of subpath costs & rowcounts.
	 *
	 * Currently, we don't charge anything extra for the actual table
	 * modification work, nor for the WITH CHECK OPTIONS or RETURNING
	 * expressions if any.  It would only be window dressing, since
	 * ModifyTable is always a top-level node and there is no way for the
	 * costs to change any higher-level planning choices.  But we might want
	 * to make it look better sometime.
	 */
	pathnode->path.startup_cost = 0;
	pathnode->path.total_cost = 0;
	pathnode->path.rows = 0;
	total_size = 0;
	foreach(lc, subpaths)
	{
		Path	   *subpath = (Path *) lfirst(lc);

		if (lc == list_head(subpaths))	/* first node? */
			pathnode->path.startup_cost = subpath->startup_cost;
		pathnode->path.total_cost += subpath->total_cost;
		pathnode->path.rows += subpath->rows;
		total_size += subpath->pathtarget->width * subpath->rows;
	}

	/*
	 * Set width to the average width of the subpath outputs.  XXX this is
	 * totally wrong: we should report zero if no RETURNING, else an average
	 * of the RETURNING tlist widths.  But it's what happened historically,
	 * and improving it is a task for another day.
	 */
	if (pathnode->path.rows > 0)
		total_size /= pathnode->path.rows;
	pathnode->path.pathtarget->width = rint(total_size);

	pathnode->operation = operation;
	pathnode->canSetTag = canSetTag;
	pathnode->nominalRelation = nominalRelation;
	pathnode->resultRelations = resultRelations;
	pathnode->subpaths = subpaths;
	pathnode->subroots = subroots;
	pathnode->withCheckOptionLists = withCheckOptionLists;
	pathnode->returningLists = returningLists;
	pathnode->rowMarks = rowMarks;
	pathnode->onconflict = onconflict;
	pathnode->epqParam = epqParam;

	return pathnode;
}

/*
 * create_limit_path
 *	  Creates a pathnode that represents performing LIMIT/OFFSET
 *
 * In addition to providing the actual OFFSET and LIMIT expressions,
 * the caller must provide estimates of their values for costing purposes.
 * The estimates are as computed by preprocess_limit(), ie, 0 represents
 * the clause not being present, and -1 means it's present but we could
 * not estimate its value.
 *
 * 'rel' is the parent relation associated with the result
 * 'subpath' is the path representing the source of data
 * 'limitOffset' is the actual OFFSET expression, or NULL
 * 'limitCount' is the actual LIMIT expression, or NULL
 * 'offset_est' is the estimated value of the OFFSET expression
 * 'count_est' is the estimated value of the LIMIT expression
 */
LimitPath *
create_limit_path(PlannerInfo *root, RelOptInfo *rel,
				  Path *subpath,
				  Node *limitOffset, Node *limitCount,
				  int64 offset_est, int64 count_est)
{
	LimitPath  *pathnode = makeNode(LimitPath);

	pathnode->path.pathtype = T_Limit;
	pathnode->path.parent = rel;
	/* Limit doesn't project, so use source path's pathtarget */
	pathnode->path.pathtarget = subpath->pathtarget;
	/* For now, assume we are above any joins, so no parameterization */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe;
	pathnode->path.parallel_degree = subpath->parallel_degree;
	pathnode->path.rows = subpath->rows;
	pathnode->path.startup_cost = subpath->startup_cost;
	pathnode->path.total_cost = subpath->total_cost;
	pathnode->path.pathkeys = subpath->pathkeys;
	pathnode->subpath = subpath;
	pathnode->limitOffset = limitOffset;
	pathnode->limitCount = limitCount;

	/*
	 * Adjust the output rows count and costs according to the offset/limit.
	 * This is only a cosmetic issue if we are at top level, but if we are
	 * building a subquery then it's important to report correct info to the
	 * outer planner.
	 *
	 * When the offset or count couldn't be estimated, use 10% of the
	 * estimated number of rows emitted from the subpath.
	 *
	 * XXX we don't bother to add eval costs of the offset/limit expressions
	 * themselves to the path costs.  In theory we should, but in most cases
	 * those expressions are trivial and it's just not worth the trouble.
	 */
	if (offset_est != 0)
	{
		double		offset_rows;

		if (offset_est > 0)
			offset_rows = (double) offset_est;
		else
			offset_rows = clamp_row_est(subpath->rows * 0.10);
		if (offset_rows > pathnode->path.rows)
			offset_rows = pathnode->path.rows;
		if (subpath->rows > 0)
			pathnode->path.startup_cost +=
				(subpath->total_cost - subpath->startup_cost)
				* offset_rows / subpath->rows;
		pathnode->path.rows -= offset_rows;
		if (pathnode->path.rows < 1)
			pathnode->path.rows = 1;
	}

	if (count_est != 0)
	{
		double		count_rows;

		if (count_est > 0)
			count_rows = (double) count_est;
		else
			count_rows = clamp_row_est(subpath->rows * 0.10);
		if (count_rows > pathnode->path.rows)
			count_rows = pathnode->path.rows;
		if (subpath->rows > 0)
			pathnode->path.total_cost = pathnode->path.startup_cost +
				(subpath->total_cost - subpath->startup_cost)
				* count_rows / subpath->rows;
		pathnode->path.rows = count_rows;
		if (pathnode->path.rows < 1)
			pathnode->path.rows = 1;
	}

	return pathnode;
}


/*
 * reparameterize_path
 *		Attempt to modify a Path to have greater parameterization
 *
 * We use this to attempt to bring all child paths of an appendrel to the
 * same parameterization level, ensuring that they all enforce the same set
 * of join quals (and thus that that parameterization can be attributed to
 * an append path built from such paths).  Currently, only a few path types
 * are supported here, though more could be added at need.  We return NULL
 * if we can't reparameterize the given path.
 *
 * Note: we intentionally do not pass created paths to add_path(); it would
 * possibly try to delete them on the grounds of being cost-inferior to the
 * paths they were made from, and we don't want that.  Paths made here are
 * not necessarily of general-purpose usefulness, but they can be useful
 * as members of an append path.
 */
Path *
reparameterize_path(PlannerInfo *root, Path *path,
					Relids required_outer,
					double loop_count)
{
	RelOptInfo *rel = path->parent;

	/* Can only increase, not decrease, path's parameterization */
	if (!bms_is_subset(PATH_REQ_OUTER(path), required_outer))
		return NULL;
	switch (path->pathtype)
	{
		case T_SeqScan:
			return create_seqscan_path(root, rel, required_outer, 0);
		case T_SampleScan:
			return (Path *) create_samplescan_path(root, rel, required_outer);
		case T_IndexScan:
		case T_IndexOnlyScan:
			{
				IndexPath  *ipath = (IndexPath *) path;
				IndexPath  *newpath = makeNode(IndexPath);

				/*
				 * We can't use create_index_path directly, and would not want
				 * to because it would re-compute the indexqual conditions
				 * which is wasted effort.  Instead we hack things a bit:
				 * flat-copy the path node, revise its param_info, and redo
				 * the cost estimate.
				 */
				memcpy(newpath, ipath, sizeof(IndexPath));
				newpath->path.param_info =
					get_baserel_parampathinfo(root, rel, required_outer);
				cost_index(newpath, root, loop_count);
				return (Path *) newpath;
			}
		case T_BitmapHeapScan:
			{
				BitmapHeapPath *bpath = (BitmapHeapPath *) path;

				return (Path *) create_bitmap_heap_path(root,
														rel,
														bpath->bitmapqual,
														required_outer,
														loop_count);
			}
		case T_SubqueryScan:
			{
				SubqueryScanPath *spath = (SubqueryScanPath *) path;

				return (Path *) create_subqueryscan_path(root,
														 rel,
														 spath->subpath,
														 spath->path.pathkeys,
														 required_outer);
			}
		default:
			break;
	}
	return NULL;
}
