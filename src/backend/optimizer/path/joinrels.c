/*-------------------------------------------------------------------------
 *
 * joinrels.c
 *	  Routines to determine which relations should be joined
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/path/joinrels.c,v 1.78 2005/12/20 02:30:35 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/joininfo.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"


static List *make_rels_by_clause_joins(PlannerInfo *root,
						  RelOptInfo *old_rel,
						  ListCell *other_rels);
static List *make_rels_by_clauseless_joins(PlannerInfo *root,
							  RelOptInfo *old_rel,
							  ListCell *other_rels);
static bool has_join_restriction(PlannerInfo *root, RelOptInfo *rel);


/*
 * make_rels_by_joins
 *	  Consider ways to produce join relations containing exactly 'level'
 *	  jointree items.  (This is one step of the dynamic-programming method
 *	  embodied in make_one_rel_by_joins.)  Join rel nodes for each feasible
 *	  combination of lower-level rels are created and returned in a list.
 *	  Implementation paths are created for each such joinrel, too.
 *
 * level: level of rels we want to make this time.
 * joinrels[j], 1 <= j < level, is a list of rels containing j items.
 */
List *
make_rels_by_joins(PlannerInfo *root, int level, List **joinrels)
{
	List	   *result_rels = NIL;
	List	   *new_rels;
	ListCell   *r;
	int			k;

	/*
	 * First, consider left-sided and right-sided plans, in which rels of
	 * exactly level-1 member relations are joined against initial relations.
	 * We prefer to join using join clauses, but if we find a rel of level-1
	 * members that has no join clauses, we will generate Cartesian-product
	 * joins against all initial rels not already contained in it.
	 *
	 * In the first pass (level == 2), we try to join each initial rel to each
	 * initial rel that appears later in joinrels[1].  (The mirror-image joins
	 * are handled automatically by make_join_rel.)  In later passes, we try
	 * to join rels of size level-1 from joinrels[level-1] to each initial rel
	 * in joinrels[1].
	 */
	foreach(r, joinrels[level - 1])
	{
		RelOptInfo *old_rel = (RelOptInfo *) lfirst(r);
		ListCell   *other_rels;

		if (level == 2)
			other_rels = lnext(r);		/* only consider remaining initial
										 * rels */
		else
			other_rels = list_head(joinrels[1]);		/* consider all initial
														 * rels */

		if (old_rel->joininfo != NIL)
		{
			/*
			 * Note that if all available join clauses for this rel require
			 * more than one other rel, we will fail to make any joins against
			 * it here.  In most cases that's OK; it'll be considered by
			 * "bushy plan" join code in a higher-level pass where we have
			 * those other rels collected into a join rel.
			 */
			new_rels = make_rels_by_clause_joins(root,
												 old_rel,
												 other_rels);

			/*
			 * An exception occurs when there is a clauseless join inside a
			 * construct that restricts join order, i.e., an outer join RHS
			 * or an IN (sub-SELECT) construct.  Here, the rel may well have
			 * join clauses against stuff outside the OJ RHS or IN sub-SELECT,
			 * but the clauseless join *must* be done before we can make use
			 * of those join clauses.	So do the clauseless join bit.
			 *
			 * See also the last-ditch case below.
			 */
			if (new_rels == NIL && has_join_restriction(root, old_rel))
				new_rels = make_rels_by_clauseless_joins(root,
														 old_rel,
														 other_rels);
		}
		else
		{
			/*
			 * Oops, we have a relation that is not joined to any other
			 * relation.  Cartesian product time.
			 */
			new_rels = make_rels_by_clauseless_joins(root,
													 old_rel,
													 other_rels);
		}

		/*
		 * At levels above 2 we will generate the same joined relation in
		 * multiple ways --- for example (a join b) join c is the same
		 * RelOptInfo as (b join c) join a, though the second case will add a
		 * different set of Paths to it.  To avoid making extra work for
		 * subsequent passes, do not enter the same RelOptInfo into our output
		 * list multiple times.
		 */
		result_rels = list_concat_unique_ptr(result_rels, new_rels);
	}

	/*
	 * Now, consider "bushy plans" in which relations of k initial rels are
	 * joined to relations of level-k initial rels, for 2 <= k <= level-2.
	 *
	 * We only consider bushy-plan joins for pairs of rels where there is a
	 * suitable join clause, in order to avoid unreasonable growth of planning
	 * time.
	 */
	for (k = 2;; k++)
	{
		int			other_level = level - k;

		/*
		 * Since make_join_rel(x, y) handles both x,y and y,x cases, we only
		 * need to go as far as the halfway point.
		 */
		if (k > other_level)
			break;

		foreach(r, joinrels[k])
		{
			RelOptInfo *old_rel = (RelOptInfo *) lfirst(r);
			ListCell   *other_rels;
			ListCell   *r2;

			if (old_rel->joininfo == NIL)
				continue;		/* we ignore clauseless joins here */

			if (k == other_level)
				other_rels = lnext(r);	/* only consider remaining rels */
			else
				other_rels = list_head(joinrels[other_level]);

			for_each_cell(r2, other_rels)
			{
				RelOptInfo *new_rel = (RelOptInfo *) lfirst(r2);

				if (!bms_overlap(old_rel->relids, new_rel->relids))
				{
					/*
					 * OK, we can build a rel of the right level from this
					 * pair of rels.  Do so if there is at least one usable
					 * join clause.
					 */
					if (have_relevant_joinclause(old_rel, new_rel))
					{
						RelOptInfo *jrel;

						jrel = make_join_rel(root, old_rel, new_rel);
						/* Avoid making duplicate entries ... */
						if (jrel)
							result_rels = list_append_unique_ptr(result_rels,
																 jrel);
					}
				}
			}
		}
	}

	/*
	 * Last-ditch effort: if we failed to find any usable joins so far, force
	 * a set of cartesian-product joins to be generated.  This handles the
	 * special case where all the available rels have join clauses but we
	 * cannot use any of the joins yet.  An example is
	 *
	 * SELECT * FROM a,b,c WHERE (a.f1 + b.f2 + c.f3) = 0;
	 *
	 * The join clause will be usable at level 3, but at level 2 we have no
	 * choice but to make cartesian joins.	We consider only left-sided and
	 * right-sided cartesian joins in this case (no bushy).
	 */
	if (result_rels == NIL)
	{
		/*
		 * This loop is just like the first one, except we always call
		 * make_rels_by_clauseless_joins().
		 */
		foreach(r, joinrels[level - 1])
		{
			RelOptInfo *old_rel = (RelOptInfo *) lfirst(r);
			ListCell   *other_rels;

			if (level == 2)
				other_rels = lnext(r);	/* only consider remaining initial
										 * rels */
			else
				other_rels = list_head(joinrels[1]);	/* consider all initial
														 * rels */

			new_rels = make_rels_by_clauseless_joins(root,
													 old_rel,
													 other_rels);

			result_rels = list_concat_unique_ptr(result_rels, new_rels);
		}

		/*----------
		 * When OJs or IN clauses are involved, there may be no legal way
		 * to make an N-way join for some values of N.	For example consider
		 *
		 * SELECT ... FROM t1 WHERE
		 *	 x IN (SELECT ... FROM t2,t3 WHERE ...) AND
		 *	 y IN (SELECT ... FROM t4,t5 WHERE ...)
		 *
		 * We will flatten this query to a 5-way join problem, but there are
		 * no 4-way joins that make_join_rel() will consider legal.  We have
		 * to accept failure at level 4 and go on to discover a workable
		 * bushy plan at level 5.
		 *
		 * However, if there are no such clauses then make_join_rel() should
		 * never fail, and so the following sanity check is useful.
		 *----------
		 */
		if (result_rels == NIL &&
			root->oj_info_list == NIL && root->in_info_list == NIL)
			elog(ERROR, "failed to build any %d-way joins", level);
	}

	return result_rels;
}

/*
 * make_rels_by_clause_joins
 *	  Build joins between the given relation 'old_rel' and other relations
 *	  that are mentioned within old_rel's joininfo list (i.e., relations
 *	  that participate in join clauses that 'old_rel' also participates in).
 *	  The join rel nodes are returned in a list.
 *
 * 'old_rel' is the relation entry for the relation to be joined
 * 'other_rels': the first cell in a linked list containing the other
 * rels to be considered for joining
 *
 * Currently, this is only used with initial rels in other_rels, but it
 * will work for joining to joinrels too.
 */
static List *
make_rels_by_clause_joins(PlannerInfo *root,
						  RelOptInfo *old_rel,
						  ListCell *other_rels)
{
	List	   *result = NIL;
	ListCell   *l;

	for_each_cell(l, other_rels)
	{
		RelOptInfo *other_rel = (RelOptInfo *) lfirst(l);

		if (!bms_overlap(old_rel->relids, other_rel->relids) &&
			have_relevant_joinclause(old_rel, other_rel))
		{
			RelOptInfo *jrel;

			jrel = make_join_rel(root, old_rel, other_rel);
			if (jrel)
				result = lcons(jrel, result);
		}
	}

	return result;
}

/*
 * make_rels_by_clauseless_joins
 *	  Given a relation 'old_rel' and a list of other relations
 *	  'other_rels', create a join relation between 'old_rel' and each
 *	  member of 'other_rels' that isn't already included in 'old_rel'.
 *	  The join rel nodes are returned in a list.
 *
 * 'old_rel' is the relation entry for the relation to be joined
 * 'other_rels': the first cell of a linked list containing the
 * other rels to be considered for joining
 *
 * Currently, this is only used with initial rels in other_rels, but it would
 * work for joining to joinrels too.
 */
static List *
make_rels_by_clauseless_joins(PlannerInfo *root,
							  RelOptInfo *old_rel,
							  ListCell *other_rels)
{
	List	   *result = NIL;
	ListCell   *i;

	for_each_cell(i, other_rels)
	{
		RelOptInfo *other_rel = (RelOptInfo *) lfirst(i);

		if (!bms_overlap(other_rel->relids, old_rel->relids))
		{
			RelOptInfo *jrel;

			jrel = make_join_rel(root, old_rel, other_rel);

			/*
			 * As long as given other_rels are distinct, don't need to test to
			 * see if jrel is already part of output list.
			 */
			if (jrel)
				result = lcons(jrel, result);
		}
	}

	return result;
}


/*
 * has_join_restriction
 *		Detect whether the specified relation has join-order restrictions
 *		due to being inside an OJ RHS or an IN (sub-SELECT).
 */
static bool
has_join_restriction(PlannerInfo *root, RelOptInfo *rel)
{
	ListCell   *l;

	foreach(l, root->oj_info_list)
	{
		OuterJoinInfo *ojinfo = (OuterJoinInfo *) lfirst(l);

		if (bms_is_subset(rel->relids, ojinfo->min_righthand))
			return true;
	}

	foreach(l, root->in_info_list)
	{
		InClauseInfo *ininfo = (InClauseInfo *) lfirst(l);

		if (bms_is_subset(rel->relids, ininfo->righthand))
			return true;
	}
	return false;
}


/*
 * make_join_rel
 *	   Find or create a join RelOptInfo that represents the join of
 *	   the two given rels, and add to it path information for paths
 *	   created with the two rels as outer and inner rel.
 *	   (The join rel may already contain paths generated from other
 *	   pairs of rels that add up to the same set of base rels.)
 *
 * NB: will return NULL if attempted join is not valid.  This can happen
 * when working with outer joins, or with IN clauses that have been turned
 * into joins.
 */
RelOptInfo *
make_join_rel(PlannerInfo *root, RelOptInfo *rel1, RelOptInfo *rel2)
{
	Relids		joinrelids;
	JoinType	jointype;
	bool		is_valid_inner;
	RelOptInfo *joinrel;
	List	   *restrictlist;
	ListCell   *l;

	/* We should never try to join two overlapping sets of rels. */
	Assert(!bms_overlap(rel1->relids, rel2->relids));

	/* Construct Relids set that identifies the joinrel. */
	joinrelids = bms_union(rel1->relids, rel2->relids);

	/*
	 * If we have any outer joins, the proposed join might be illegal; and
	 * in any case we have to determine its join type.  Scan the OJ list
	 * for conflicts.
	 */
	jointype = JOIN_INNER;		/* default if no match to an OJ */
	is_valid_inner = true;

	foreach(l, root->oj_info_list)
	{
		OuterJoinInfo *ojinfo = (OuterJoinInfo *) lfirst(l);

		/*
		 * This OJ is not relevant unless its RHS overlaps the proposed join.
		 * (Check this first as a fast path for dismissing most irrelevant OJs
		 * quickly.)
		 */
		if (!bms_overlap(ojinfo->min_righthand, joinrelids))
			continue;

		/*
		 * Also, not relevant if proposed join is fully contained within RHS
		 * (ie, we're still building up the RHS).
		 */
		if (bms_is_subset(joinrelids, ojinfo->min_righthand))
			continue;

		/*
		 * Also, not relevant if OJ is already done within either input.
		 */
		if (bms_is_subset(ojinfo->min_lefthand, rel1->relids) &&
			bms_is_subset(ojinfo->min_righthand, rel1->relids))
			continue;
		if (bms_is_subset(ojinfo->min_lefthand, rel2->relids) &&
			bms_is_subset(ojinfo->min_righthand, rel2->relids))
			continue;

		/*
		 * If one input contains min_lefthand and the other contains
		 * min_righthand, then we can perform the OJ at this join.
		 *
		 * Barf if we get matches to more than one OJ (is that possible?)
		 */
		if (bms_is_subset(ojinfo->min_lefthand, rel1->relids) &&
			bms_is_subset(ojinfo->min_righthand, rel2->relids))
		{
			if (jointype != JOIN_INNER)
			{
				/* invalid join path */
				bms_free(joinrelids);
				return NULL;
			}
			jointype = ojinfo->is_full_join ? JOIN_FULL : JOIN_LEFT;
		}
		else if (bms_is_subset(ojinfo->min_lefthand, rel2->relids) &&
				 bms_is_subset(ojinfo->min_righthand, rel1->relids))
		{
			if (jointype != JOIN_INNER)
			{
				/* invalid join path */
				bms_free(joinrelids);
				return NULL;
			}
			jointype = ojinfo->is_full_join ? JOIN_FULL : JOIN_RIGHT;
		}
		else
		{
			/*----------
			 * Otherwise, the proposed join overlaps the RHS but isn't
			 * a valid implementation of this OJ.  It might still be
			 * a valid implementation of some other OJ, however.  We have
			 * to allow this to support the associative identity
			 *	(a LJ b on Pab) LJ c ON Pbc = a LJ (b LJ c ON Pbc) on Pab
			 * since joining B directly to C violates the lower OJ's RHS.
			 * We assume that make_outerjoininfo() set things up correctly
			 * so that we'll only match to the upper OJ if the transformation
			 * is valid.  Set flag here to check at bottom of loop.
			 *----------
			 */
			is_valid_inner = false;
		}
	}

	/* Fail if violated some OJ's RHS and didn't match to another OJ */
	if (jointype == JOIN_INNER && !is_valid_inner)
	{
		/* invalid join path */
		bms_free(joinrelids);
		return NULL;
	}

	/*
	 * Similarly, if we are implementing IN clauses as joins, check for
	 * illegal join path and detect whether we need a non-default join type.
	 */
	foreach(l, root->in_info_list)
	{
		InClauseInfo *ininfo = (InClauseInfo *) lfirst(l);

		/*
		 * This IN clause is not relevant unless its RHS overlaps the
		 * proposed join.  (Check this first as a fast path for dismissing
		 * most irrelevant INs quickly.)
		 */
		if (!bms_overlap(ininfo->righthand, joinrelids))
			continue;

		/*
		 * If we are still building the IN clause's RHS, then this IN
		 * clause isn't relevant yet.
		 */
		if (bms_is_subset(joinrelids, ininfo->righthand))
			continue;

		/*
		 * Cannot join if proposed join contains rels not in the RHS *and*
		 * contains only part of the RHS.  We must build the complete RHS
		 * (subselect's join) before it can be joined to rels outside the
		 * subselect.
		 */
		if (!bms_is_subset(ininfo->righthand, joinrelids))
		{
			bms_free(joinrelids);
			return NULL;
		}

		/*
		 * At this point we are considering a join of the IN's RHS to some
		 * other rel(s).
		 *
		 * If we already joined IN's RHS to any other rels in either input
		 * path, then this join is not constrained (the necessary work was
		 * done at the lower level where that join occurred).
		 */
		if (bms_is_subset(ininfo->righthand, rel1->relids) &&
			!bms_equal(ininfo->righthand, rel1->relids))
			continue;
		if (bms_is_subset(ininfo->righthand, rel2->relids) &&
			!bms_equal(ininfo->righthand, rel2->relids))
			continue;

		/*
		 * JOIN_IN technique will work if outerrel includes LHS and innerrel
		 * is exactly RHS; conversely JOIN_REVERSE_IN handles RHS/LHS.
		 *
		 * JOIN_UNIQUE_OUTER will work if outerrel is exactly RHS; conversely
		 * JOIN_UNIQUE_INNER will work if innerrel is exactly RHS.
		 *
		 * But none of these will work if we already found an OJ or another IN
		 * that needs to trigger here.
		 */
		if (jointype != JOIN_INNER)
		{
			bms_free(joinrelids);
			return NULL;
		}
		if (bms_is_subset(ininfo->lefthand, rel1->relids) &&
			bms_equal(ininfo->righthand, rel2->relids))
			jointype = JOIN_IN;
		else if (bms_is_subset(ininfo->lefthand, rel2->relids) &&
				 bms_equal(ininfo->righthand, rel1->relids))
			jointype = JOIN_REVERSE_IN;
		else if (bms_equal(ininfo->righthand, rel1->relids))
			jointype = JOIN_UNIQUE_OUTER;
		else if (bms_equal(ininfo->righthand, rel2->relids))
			jointype = JOIN_UNIQUE_INNER;
		else
		{
			/* invalid join path */
			bms_free(joinrelids);
			return NULL;
		}
	}

	/*
	 * Find or build the join RelOptInfo, and compute the restrictlist that
	 * goes with this particular joining.
	 */
	joinrel = build_join_rel(root, joinrelids, rel1, rel2, jointype,
							 &restrictlist);

	/*
	 * Consider paths using each rel as both outer and inner.
	 */
	switch (jointype)
	{
		case JOIN_INNER:
			add_paths_to_joinrel(root, joinrel, rel1, rel2, JOIN_INNER,
								 restrictlist);
			add_paths_to_joinrel(root, joinrel, rel2, rel1, JOIN_INNER,
								 restrictlist);
			break;
		case JOIN_LEFT:
			add_paths_to_joinrel(root, joinrel, rel1, rel2, JOIN_LEFT,
								 restrictlist);
			add_paths_to_joinrel(root, joinrel, rel2, rel1, JOIN_RIGHT,
								 restrictlist);
			break;
		case JOIN_FULL:
			add_paths_to_joinrel(root, joinrel, rel1, rel2, JOIN_FULL,
								 restrictlist);
			add_paths_to_joinrel(root, joinrel, rel2, rel1, JOIN_FULL,
								 restrictlist);
			break;
		case JOIN_RIGHT:
			add_paths_to_joinrel(root, joinrel, rel1, rel2, JOIN_RIGHT,
								 restrictlist);
			add_paths_to_joinrel(root, joinrel, rel2, rel1, JOIN_LEFT,
								 restrictlist);
			break;
		case JOIN_IN:
			add_paths_to_joinrel(root, joinrel, rel1, rel2, JOIN_IN,
								 restrictlist);
			/* REVERSE_IN isn't supported by joinpath.c */
			add_paths_to_joinrel(root, joinrel, rel1, rel2, JOIN_UNIQUE_INNER,
								 restrictlist);
			add_paths_to_joinrel(root, joinrel, rel2, rel1, JOIN_UNIQUE_OUTER,
								 restrictlist);
			break;
		case JOIN_REVERSE_IN:
			/* REVERSE_IN isn't supported by joinpath.c */
			add_paths_to_joinrel(root, joinrel, rel2, rel1, JOIN_IN,
								 restrictlist);
			add_paths_to_joinrel(root, joinrel, rel1, rel2, JOIN_UNIQUE_OUTER,
								 restrictlist);
			add_paths_to_joinrel(root, joinrel, rel2, rel1, JOIN_UNIQUE_INNER,
								 restrictlist);
			break;
		case JOIN_UNIQUE_OUTER:
			add_paths_to_joinrel(root, joinrel, rel1, rel2, JOIN_UNIQUE_OUTER,
								 restrictlist);
			add_paths_to_joinrel(root, joinrel, rel2, rel1, JOIN_UNIQUE_INNER,
								 restrictlist);
			break;
		case JOIN_UNIQUE_INNER:
			add_paths_to_joinrel(root, joinrel, rel1, rel2, JOIN_UNIQUE_INNER,
								 restrictlist);
			add_paths_to_joinrel(root, joinrel, rel2, rel1, JOIN_UNIQUE_OUTER,
								 restrictlist);
			break;
		default:
			elog(ERROR, "unrecognized join type: %d",
				 (int) jointype);
			break;
	}

	bms_free(joinrelids);

	return joinrel;
}
