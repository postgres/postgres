/*-------------------------------------------------------------------------
 *
 * joinrels.c
 *	  Routines to determine which relations should be joined
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/path/joinrels.c,v 1.44 2000/04/12 17:15:20 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/cost.h"
#include "optimizer/joininfo.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/tlist.h"


static RelOptInfo *make_join_rel(Query *root, RelOptInfo *rel1,
			  RelOptInfo *rel2);


/*
 * make_rels_by_joins
 *	  Consider ways to produce join relations containing exactly 'level'
 *	  base relations.  (This is one step of the dynamic-programming method
 *	  embodied in make_one_rel_by_joins.)  Join rel nodes for each feasible
 *	  combination of base rels are created and added to the front of the
 *	  query's join_rel_list.  Implementation paths are created for each
 *	  such joinrel, too.
 *
 * Returns nothing, but adds entries to root->join_rel_list.
 */
void
make_rels_by_joins(Query *root, int level)
{
	List	   *r;

	/*
	 * First, consider left-sided and right-sided plans, in which rels of
	 * exactly level-1 member relations are joined against base relations.
	 * We prefer to join using join clauses, but if we find a rel of
	 * level-1 members that has no join clauses, we will generate
	 * Cartesian-product joins against all base rels not already contained
	 * in it.
	 *
	 * In the first pass (level == 2), we try to join each base rel to each
	 * base rel that appears later in base_rel_list.  (The mirror-image
	 * joins are handled automatically by make_join_rel.)  In later
	 * passes, we try to join rels of size level-1 from join_rel_list to
	 * each base rel in base_rel_list.
	 *
	 * We assume that the rels already present in join_rel_list appear in
	 * decreasing order of level (number of members).  This should be true
	 * since we always add new higher-level rels to the front of the list.
	 */
	if (level == 2)
		r = root->base_rel_list;/* level-1 is base rels */
	else
		r = root->join_rel_list;
	for (; r != NIL; r = lnext(r))
	{
		RelOptInfo *old_rel = (RelOptInfo *) lfirst(r);
		int			old_level = length(old_rel->relids);
		List	   *other_rels;

		if (old_level != level - 1)
			break;

		if (level == 2)
			other_rels = lnext(r);		/* only consider remaining base
										 * rels */
		else
			other_rels = root->base_rel_list;	/* consider all base rels */

		if (old_rel->joininfo != NIL)
		{

			/*
			 * Note that if all available join clauses for this rel
			 * require more than one other rel, we will fail to make any
			 * joins against it here.  That's OK; it'll be considered by
			 * "bushy plan" join code in a higher-level pass.
			 */
			make_rels_by_clause_joins(root,
									  old_rel,
									  other_rels);
		}
		else
		{

			/*
			 * Oops, we have a relation that is not joined to any other
			 * relation.  Cartesian product time.
			 */
			make_rels_by_clauseless_joins(root,
										  old_rel,
										  other_rels);
		}
	}

	/*
	 * Now, consider "bushy plans" in which relations of k base rels are
	 * joined to relations of level-k base rels, for 2 <= k <= level-2.
	 * The previous loop left r pointing to the first rel of level
	 * level-2.
	 *
	 * We only consider bushy-plan joins for pairs of rels where there is a
	 * suitable join clause, in order to avoid unreasonable growth of
	 * planning time.
	 */
	for (; r != NIL; r = lnext(r))
	{
		RelOptInfo *old_rel = (RelOptInfo *) lfirst(r);
		int			old_level = length(old_rel->relids);
		List	   *r2;

		/*
		 * We can quit once past the halfway point (make_join_rel took
		 * care of making the opposite-direction joins)
		 */
		if (old_level * 2 < level)
			break;

		if (old_rel->joininfo == NIL)
			continue;			/* we ignore clauseless joins here */

		foreach(r2, lnext(r))
		{
			RelOptInfo *new_rel = (RelOptInfo *) lfirst(r2);
			int			new_level = length(new_rel->relids);

			if (old_level + new_level > level)
				continue;		/* scan down to new_rels of right size */
			if (old_level + new_level < level)
				break;			/* no more new_rels of right size */
			if (nonoverlap_setsi(old_rel->relids, new_rel->relids))
			{
				List	   *i;

				/*
				 * OK, we can build a rel of the right level from this
				 * pair of rels.  Do so if there is at least one usable
				 * join clause.
				 */
				foreach(i, old_rel->joininfo)
				{
					JoinInfo   *joininfo = (JoinInfo *) lfirst(i);

					if (is_subseti(joininfo->unjoined_relids, new_rel->relids))
					{
						make_join_rel(root, old_rel, new_rel);
						break;
					}
				}
			}
		}
	}
}

/*
 * make_rels_by_clause_joins
 *	  Build joins between the given relation 'old_rel' and other relations
 *	  that are mentioned within old_rel's joininfo nodes (i.e., relations
 *	  that participate in join clauses that 'old_rel' also participates in).
 *	  The join rel nodes are added to root->join_rel_list.
 *
 * 'old_rel' is the relation entry for the relation to be joined
 * 'other_rels': other rels to be considered for joining
 *
 * Currently, this is only used with base rels in other_rels, but it would
 * work for joining to joinrels too, if the caller ensures there is no
 * membership overlap between old_rel and the rels in other_rels.  (We need
 * no extra test for overlap for base rels, since the is_subset test can
 * only succeed when other_rel is not already part of old_rel.)
 *
 * Returns NULL if no suitable joins were found, else the last suitable
 * joinrel processed.  (The only caller who checks the return value is
 * geqo_eval.c, and it sets things up so there can be no more than one
 * "suitable" joinrel; so we don't bother with returning a list.)
 */
RelOptInfo *
make_rels_by_clause_joins(Query *root,
						  RelOptInfo *old_rel,
						  List *other_rels)
{
	RelOptInfo *result = NULL;
	List	   *i,
			   *j;

	foreach(i, old_rel->joininfo)
	{
		JoinInfo   *joininfo = (JoinInfo *) lfirst(i);
		Relids		unjoined_relids = joininfo->unjoined_relids;

		foreach(j, other_rels)
		{
			RelOptInfo *other_rel = (RelOptInfo *) lfirst(j);

			if (is_subseti(unjoined_relids, other_rel->relids))
				result = make_join_rel(root, old_rel, other_rel);
		}
	}

	return result;
}

/*
 * make_rels_by_clauseless_joins
 *	  Given a relation 'old_rel' and a list of other relations
 *	  'other_rels', create a join relation between 'old_rel' and each
 *	  member of 'other_rels' that isn't already included in 'old_rel'.
 *
 * 'old_rel' is the relation entry for the relation to be joined
 * 'other_rels': other rels to be considered for joining
 *
 * Currently, this is only used with base rels in other_rels, but it would
 * work for joining to joinrels too.
 *
 * Returns NULL if no suitable joins were found, else the last suitable
 * joinrel processed.  (The only caller who checks the return value is
 * geqo_eval.c, and it sets things up so there can be no more than one
 * "suitable" joinrel; so we don't bother with returning a list.)
 */
RelOptInfo *
make_rels_by_clauseless_joins(Query *root,
							  RelOptInfo *old_rel,
							  List *other_rels)
{
	RelOptInfo *result = NULL;
	List	   *i;

	foreach(i, other_rels)
	{
		RelOptInfo *other_rel = (RelOptInfo *) lfirst(i);

		if (nonoverlap_setsi(other_rel->relids, old_rel->relids))
			result = make_join_rel(root, old_rel, other_rel);
	}

	return result;
}


/*
 * make_join_rel
 *	   Find or create a join RelOptInfo that represents the join of
 *	   the two given rels, and add to it path information for paths
 *	   created with the two rels as outer and inner rel.
 *	   (The join rel may already contain paths generated from other
 *	   pairs of rels that add up to the same set of base rels.)
 *	   The join rel is stored in the query's join_rel_list.
 */
static RelOptInfo *
make_join_rel(Query *root, RelOptInfo *rel1, RelOptInfo *rel2)
{
	RelOptInfo *joinrel;
	List	   *restrictlist;

	/*
	 * Find or build the join RelOptInfo, and compute the restrictlist
	 * that goes with this particular joining.
	 */
	joinrel = get_join_rel(root, rel1, rel2, &restrictlist);

	/*
	 * We consider paths using each rel as both outer and inner.
	 */
	add_paths_to_joinrel(root, joinrel, rel1, rel2, restrictlist);
	add_paths_to_joinrel(root, joinrel, rel2, rel1, restrictlist);

	return joinrel;
}
