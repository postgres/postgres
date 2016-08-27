/*------------------------------------------------------------------------
 *
 * geqo_eval.c
 *	  Routines to evaluate query trees
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/optimizer/geqo/geqo_eval.c
 *
 *-------------------------------------------------------------------------
 */

/* contributed by:
   =*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=
   *  Martin Utesch				 * Institute of Automatic Control	   *
   =							 = University of Mining and Technology =
   *  utesch@aut.tu-freiberg.de  * Freiberg, Germany				   *
   =*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=
 */

#include "postgres.h"

#include <float.h>
#include <limits.h>
#include <math.h>

#include "optimizer/geqo.h"
#include "optimizer/joininfo.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "utils/memutils.h"


/* A "clump" of already-joined relations within gimme_tree */
typedef struct
{
	RelOptInfo *joinrel;		/* joinrel for the set of relations */
	int			size;			/* number of input relations in clump */
} Clump;

static List *merge_clump(PlannerInfo *root, List *clumps, Clump *new_clump,
			bool force);
static bool desirable_join(PlannerInfo *root,
			   RelOptInfo *outer_rel, RelOptInfo *inner_rel);


/*
 * geqo_eval
 *
 * Returns cost of a query tree as an individual of the population.
 *
 * If no legal join order can be extracted from the proposed tour,
 * returns DBL_MAX.
 */
Cost
geqo_eval(PlannerInfo *root, Gene *tour, int num_gene)
{
	MemoryContext mycontext;
	MemoryContext oldcxt;
	RelOptInfo *joinrel;
	Cost		fitness;
	int			savelength;
	struct HTAB *savehash;

	/*
	 * Create a private memory context that will hold all temp storage
	 * allocated inside gimme_tree().
	 *
	 * Since geqo_eval() will be called many times, we can't afford to let all
	 * that memory go unreclaimed until end of statement.  Note we make the
	 * temp context a child of the planner's normal context, so that it will
	 * be freed even if we abort via ereport(ERROR).
	 */
	mycontext = AllocSetContextCreate(CurrentMemoryContext,
									  "GEQO",
									  ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(mycontext);

	/*
	 * gimme_tree will add entries to root->join_rel_list, which may or may
	 * not already contain some entries.  The newly added entries will be
	 * recycled by the MemoryContextDelete below, so we must ensure that the
	 * list is restored to its former state before exiting.  We can do this by
	 * truncating the list to its original length.  NOTE this assumes that any
	 * added entries are appended at the end!
	 *
	 * We also must take care not to mess up the outer join_rel_hash, if there
	 * is one.  We can do this by just temporarily setting the link to NULL.
	 * (If we are dealing with enough join rels, which we very likely are, a
	 * new hash table will get built and used locally.)
	 *
	 * join_rel_level[] shouldn't be in use, so just Assert it isn't.
	 */
	savelength = list_length(root->join_rel_list);
	savehash = root->join_rel_hash;
	Assert(root->join_rel_level == NULL);

	root->join_rel_hash = NULL;

	/* construct the best path for the given combination of relations */
	joinrel = gimme_tree(root, tour, num_gene);

	/*
	 * compute fitness, if we found a valid join
	 *
	 * XXX geqo does not currently support optimization for partial result
	 * retrieval, nor do we take any cognizance of possible use of
	 * parameterized paths --- how to fix?
	 */
	if (joinrel)
	{
		Path	   *best_path = joinrel->cheapest_total_path;

		fitness = best_path->total_cost;
	}
	else
		fitness = DBL_MAX;

	/*
	 * Restore join_rel_list to its former state, and put back original
	 * hashtable if any.
	 */
	root->join_rel_list = list_truncate(root->join_rel_list,
										savelength);
	root->join_rel_hash = savehash;

	/* release all the memory acquired within gimme_tree */
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(mycontext);

	return fitness;
}

/*
 * gimme_tree
 *	  Form planner estimates for a join tree constructed in the specified
 *	  order.
 *
 *	 'tour' is the proposed join order, of length 'num_gene'
 *
 * Returns a new join relation whose cheapest path is the best plan for
 * this join order.  NB: will return NULL if join order is invalid and
 * we can't modify it into a valid order.
 *
 * The original implementation of this routine always joined in the specified
 * order, and so could only build left-sided plans (and right-sided and
 * mixtures, as a byproduct of the fact that make_join_rel() is symmetric).
 * It could never produce a "bushy" plan.  This had a couple of big problems,
 * of which the worst was that there are situations involving join order
 * restrictions where the only valid plans are bushy.
 *
 * The present implementation takes the given tour as a guideline, but
 * postpones joins that are illegal or seem unsuitable according to some
 * heuristic rules.  This allows correct bushy plans to be generated at need,
 * and as a nice side-effect it seems to materially improve the quality of the
 * generated plans.  Note however that since it's just a heuristic, it can
 * still fail in some cases.  (In particular, we might clump together
 * relations that actually mustn't be joined yet due to LATERAL restrictions;
 * since there's no provision for un-clumping, this must lead to failure.)
 */
RelOptInfo *
gimme_tree(PlannerInfo *root, Gene *tour, int num_gene)
{
	GeqoPrivateData *private = (GeqoPrivateData *) root->join_search_private;
	List	   *clumps;
	int			rel_count;

	/*
	 * Sometimes, a relation can't yet be joined to others due to heuristics
	 * or actual semantic restrictions.  We maintain a list of "clumps" of
	 * successfully joined relations, with larger clumps at the front. Each
	 * new relation from the tour is added to the first clump it can be joined
	 * to; if there is none then it becomes a new clump of its own. When we
	 * enlarge an existing clump we check to see if it can now be merged with
	 * any other clumps.  After the tour is all scanned, we forget about the
	 * heuristics and try to forcibly join any remaining clumps.  If we are
	 * unable to merge all the clumps into one, fail.
	 */
	clumps = NIL;

	for (rel_count = 0; rel_count < num_gene; rel_count++)
	{
		int			cur_rel_index;
		RelOptInfo *cur_rel;
		Clump	   *cur_clump;

		/* Get the next input relation */
		cur_rel_index = (int) tour[rel_count];
		cur_rel = (RelOptInfo *) list_nth(private->initial_rels,
										  cur_rel_index - 1);

		/* Make it into a single-rel clump */
		cur_clump = (Clump *) palloc(sizeof(Clump));
		cur_clump->joinrel = cur_rel;
		cur_clump->size = 1;

		/* Merge it into the clumps list, using only desirable joins */
		clumps = merge_clump(root, clumps, cur_clump, false);
	}

	if (list_length(clumps) > 1)
	{
		/* Force-join the remaining clumps in some legal order */
		List	   *fclumps;
		ListCell   *lc;

		fclumps = NIL;
		foreach(lc, clumps)
		{
			Clump	   *clump = (Clump *) lfirst(lc);

			fclumps = merge_clump(root, fclumps, clump, true);
		}
		clumps = fclumps;
	}

	/* Did we succeed in forming a single join relation? */
	if (list_length(clumps) != 1)
		return NULL;

	return ((Clump *) linitial(clumps))->joinrel;
}

/*
 * Merge a "clump" into the list of existing clumps for gimme_tree.
 *
 * We try to merge the clump into some existing clump, and repeat if
 * successful.  When no more merging is possible, insert the clump
 * into the list, preserving the list ordering rule (namely, that
 * clumps of larger size appear earlier).
 *
 * If force is true, merge anywhere a join is legal, even if it causes
 * a cartesian join to be performed.  When force is false, do only
 * "desirable" joins.
 */
static List *
merge_clump(PlannerInfo *root, List *clumps, Clump *new_clump, bool force)
{
	ListCell   *prev;
	ListCell   *lc;

	/* Look for a clump that new_clump can join to */
	prev = NULL;
	foreach(lc, clumps)
	{
		Clump	   *old_clump = (Clump *) lfirst(lc);

		if (force ||
			desirable_join(root, old_clump->joinrel, new_clump->joinrel))
		{
			RelOptInfo *joinrel;

			/*
			 * Construct a RelOptInfo representing the join of these two input
			 * relations.  Note that we expect the joinrel not to exist in
			 * root->join_rel_list yet, and so the paths constructed for it
			 * will only include the ones we want.
			 */
			joinrel = make_join_rel(root,
									old_clump->joinrel,
									new_clump->joinrel);

			/* Keep searching if join order is not valid */
			if (joinrel)
			{
				/* Create GatherPaths for any useful partial paths for rel */
				generate_gather_paths(root, joinrel);

				/* Find and save the cheapest paths for this joinrel */
				set_cheapest(joinrel);

				/* Absorb new clump into old */
				old_clump->joinrel = joinrel;
				old_clump->size += new_clump->size;
				pfree(new_clump);

				/* Remove old_clump from list */
				clumps = list_delete_cell(clumps, lc, prev);

				/*
				 * Recursively try to merge the enlarged old_clump with
				 * others.  When no further merge is possible, we'll reinsert
				 * it into the list.
				 */
				return merge_clump(root, clumps, old_clump, force);
			}
		}
		prev = lc;
	}

	/*
	 * No merging is possible, so add new_clump as an independent clump, in
	 * proper order according to size.  We can be fast for the common case
	 * where it has size 1 --- it should always go at the end.
	 */
	if (clumps == NIL || new_clump->size == 1)
		return lappend(clumps, new_clump);

	/* Check if it belongs at the front */
	lc = list_head(clumps);
	if (new_clump->size > ((Clump *) lfirst(lc))->size)
		return lcons(new_clump, clumps);

	/* Else search for the place to insert it */
	for (;;)
	{
		ListCell   *nxt = lnext(lc);

		if (nxt == NULL || new_clump->size > ((Clump *) lfirst(nxt))->size)
			break;				/* it belongs after 'lc', before 'nxt' */
		lc = nxt;
	}
	lappend_cell(clumps, lc, new_clump);

	return clumps;
}

/*
 * Heuristics for gimme_tree: do we want to join these two relations?
 */
static bool
desirable_join(PlannerInfo *root,
			   RelOptInfo *outer_rel, RelOptInfo *inner_rel)
{
	/*
	 * Join if there is an applicable join clause, or if there is a join order
	 * restriction forcing these rels to be joined.
	 */
	if (have_relevant_joinclause(root, outer_rel, inner_rel) ||
		have_join_order_restriction(root, outer_rel, inner_rel))
		return true;

	/* Otherwise postpone the join till later. */
	return false;
}
