/*-------------------------------------------------------------------------
 *
 * joinrels.c
 *	  Routines to determine which relations should be joined
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/path/joinrels.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "optimizer/appendinfo.h"
#include "optimizer/joininfo.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "partitioning/partbounds.h"
#include "utils/memutils.h"


static void make_rels_by_clause_joins(PlannerInfo *root,
									  RelOptInfo *old_rel,
									  List *other_rels,
									  int first_rel_idx);
static void make_rels_by_clauseless_joins(PlannerInfo *root,
										  RelOptInfo *old_rel,
										  List *other_rels);
static bool has_join_restriction(PlannerInfo *root, RelOptInfo *rel);
static bool has_legal_joinclause(PlannerInfo *root, RelOptInfo *rel);
static bool restriction_is_constant_false(List *restrictlist,
										  RelOptInfo *joinrel,
										  bool only_pushed_down);
static void populate_joinrel_with_paths(PlannerInfo *root, RelOptInfo *rel1,
										RelOptInfo *rel2, RelOptInfo *joinrel,
										SpecialJoinInfo *sjinfo, List *restrictlist);
static void try_partitionwise_join(PlannerInfo *root, RelOptInfo *rel1,
								   RelOptInfo *rel2, RelOptInfo *joinrel,
								   SpecialJoinInfo *parent_sjinfo,
								   List *parent_restrictlist);
static SpecialJoinInfo *build_child_join_sjinfo(PlannerInfo *root,
												SpecialJoinInfo *parent_sjinfo,
												Relids left_relids, Relids right_relids);
static void free_child_join_sjinfo(SpecialJoinInfo *child_sjinfo,
								   SpecialJoinInfo *parent_sjinfo);
static void compute_partition_bounds(PlannerInfo *root, RelOptInfo *rel1,
									 RelOptInfo *rel2, RelOptInfo *joinrel,
									 SpecialJoinInfo *parent_sjinfo,
									 List **parts1, List **parts2);
static void get_matching_part_pairs(PlannerInfo *root, RelOptInfo *joinrel,
									RelOptInfo *rel1, RelOptInfo *rel2,
									List **parts1, List **parts2);


/*
 * join_search_one_level
 *	  Consider ways to produce join relations containing exactly 'level'
 *	  jointree items.  (This is one step of the dynamic-programming method
 *	  embodied in standard_join_search.)  Join rel nodes for each feasible
 *	  combination of lower-level rels are created and returned in a list.
 *	  Implementation paths are created for each such joinrel, too.
 *
 * level: level of rels we want to make this time
 * root->join_rel_level[j], 1 <= j < level, is a list of rels containing j items
 *
 * The result is returned in root->join_rel_level[level].
 */
void
join_search_one_level(PlannerInfo *root, int level)
{
	List	  **joinrels = root->join_rel_level;
	ListCell   *r;
	int			k;

	Assert(joinrels[level] == NIL);

	/* Set join_cur_level so that new joinrels are added to proper list */
	root->join_cur_level = level;

	/*
	 * First, consider left-sided and right-sided plans, in which rels of
	 * exactly level-1 member relations are joined against initial relations.
	 * We prefer to join using join clauses, but if we find a rel of level-1
	 * members that has no join clauses, we will generate Cartesian-product
	 * joins against all initial rels not already contained in it.
	 */
	foreach(r, joinrels[level - 1])
	{
		RelOptInfo *old_rel = (RelOptInfo *) lfirst(r);

		if (old_rel->joininfo != NIL || old_rel->has_eclass_joins ||
			has_join_restriction(root, old_rel))
		{
			int			first_rel;

			/*
			 * There are join clauses or join order restrictions relevant to
			 * this rel, so consider joins between this rel and (only) those
			 * initial rels it is linked to by a clause or restriction.
			 *
			 * At level 2 this condition is symmetric, so there is no need to
			 * look at initial rels before this one in the list; we already
			 * considered such joins when we were at the earlier rel.  (The
			 * mirror-image joins are handled automatically by make_join_rel.)
			 * In later passes (level > 2), we join rels of the previous level
			 * to each initial rel they don't already include but have a join
			 * clause or restriction with.
			 */
			if (level == 2)		/* consider remaining initial rels */
				first_rel = foreach_current_index(r) + 1;
			else
				first_rel = 0;

			make_rels_by_clause_joins(root, old_rel, joinrels[1], first_rel);
		}
		else
		{
			/*
			 * Oops, we have a relation that is not joined to any other
			 * relation, either directly or by join-order restrictions.
			 * Cartesian product time.
			 *
			 * We consider a cartesian product with each not-already-included
			 * initial rel, whether it has other join clauses or not.  At
			 * level 2, if there are two or more clauseless initial rels, we
			 * will redundantly consider joining them in both directions; but
			 * such cases aren't common enough to justify adding complexity to
			 * avoid the duplicated effort.
			 */
			make_rels_by_clauseless_joins(root,
										  old_rel,
										  joinrels[1]);
		}
	}

	/*
	 * Now, consider "bushy plans" in which relations of k initial rels are
	 * joined to relations of level-k initial rels, for 2 <= k <= level-2.
	 *
	 * We only consider bushy-plan joins for pairs of rels where there is a
	 * suitable join clause (or join order restriction), in order to avoid
	 * unreasonable growth of planning time.
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
			int			first_rel;
			ListCell   *r2;

			/*
			 * We can ignore relations without join clauses here, unless they
			 * participate in join-order restrictions --- then we might have
			 * to force a bushy join plan.
			 */
			if (old_rel->joininfo == NIL && !old_rel->has_eclass_joins &&
				!has_join_restriction(root, old_rel))
				continue;

			if (k == other_level)	/* only consider remaining rels */
				first_rel = foreach_current_index(r) + 1;
			else
				first_rel = 0;

			for_each_from(r2, joinrels[other_level], first_rel)
			{
				RelOptInfo *new_rel = (RelOptInfo *) lfirst(r2);

				if (!bms_overlap(old_rel->relids, new_rel->relids))
				{
					/*
					 * OK, we can build a rel of the right level from this
					 * pair of rels.  Do so if there is at least one relevant
					 * join clause or join order restriction.
					 */
					if (have_relevant_joinclause(root, old_rel, new_rel) ||
						have_join_order_restriction(root, old_rel, new_rel))
					{
						(void) make_join_rel(root, old_rel, new_rel);
					}
				}
			}
		}
	}

	/*----------
	 * Last-ditch effort: if we failed to find any usable joins so far, force
	 * a set of cartesian-product joins to be generated.  This handles the
	 * special case where all the available rels have join clauses but we
	 * cannot use any of those clauses yet.  This can only happen when we are
	 * considering a join sub-problem (a sub-joinlist) and all the rels in the
	 * sub-problem have only join clauses with rels outside the sub-problem.
	 * An example is
	 *
	 *		SELECT ... FROM a INNER JOIN b ON TRUE, c, d, ...
	 *		WHERE a.w = c.x and b.y = d.z;
	 *
	 * If the "a INNER JOIN b" sub-problem does not get flattened into the
	 * upper level, we must be willing to make a cartesian join of a and b;
	 * but the code above will not have done so, because it thought that both
	 * a and b have joinclauses.  We consider only left-sided and right-sided
	 * cartesian joins in this case (no bushy).
	 *----------
	 */
	if (joinrels[level] == NIL)
	{
		/*
		 * This loop is just like the first one, except we always call
		 * make_rels_by_clauseless_joins().
		 */
		foreach(r, joinrels[level - 1])
		{
			RelOptInfo *old_rel = (RelOptInfo *) lfirst(r);

			make_rels_by_clauseless_joins(root,
										  old_rel,
										  joinrels[1]);
		}

		/*----------
		 * When special joins are involved, there may be no legal way
		 * to make an N-way join for some values of N.  For example consider
		 *
		 * SELECT ... FROM t1 WHERE
		 *	 x IN (SELECT ... FROM t2,t3 WHERE ...) AND
		 *	 y IN (SELECT ... FROM t4,t5 WHERE ...)
		 *
		 * We will flatten this query to a 5-way join problem, but there are
		 * no 4-way joins that join_is_legal() will consider legal.  We have
		 * to accept failure at level 4 and go on to discover a workable
		 * bushy plan at level 5.
		 *
		 * However, if there are no special joins and no lateral references
		 * then join_is_legal() should never fail, and so the following sanity
		 * check is useful.
		 *----------
		 */
		if (joinrels[level] == NIL &&
			root->join_info_list == NIL &&
			!root->hasLateralRTEs)
			elog(ERROR, "failed to build any %d-way joins", level);
	}
}

/*
 * make_rels_by_clause_joins
 *	  Build joins between the given relation 'old_rel' and other relations
 *	  that participate in join clauses that 'old_rel' also participates in
 *	  (or participate in join-order restrictions with it).
 *	  The join rels are returned in root->join_rel_level[join_cur_level].
 *
 * Note: at levels above 2 we will generate the same joined relation in
 * multiple ways --- for example (a join b) join c is the same RelOptInfo as
 * (b join c) join a, though the second case will add a different set of Paths
 * to it.  This is the reason for using the join_rel_level mechanism, which
 * automatically ensures that each new joinrel is only added to the list once.
 *
 * 'old_rel' is the relation entry for the relation to be joined
 * 'other_rels': a list containing the other rels to be considered for joining
 * 'first_rel_idx': the first rel to be considered in 'other_rels'
 *
 * Currently, this is only used with initial rels in other_rels, but it
 * will work for joining to joinrels too.
 */
static void
make_rels_by_clause_joins(PlannerInfo *root,
						  RelOptInfo *old_rel,
						  List *other_rels,
						  int first_rel_idx)
{
	ListCell   *l;

	for_each_from(l, other_rels, first_rel_idx)
	{
		RelOptInfo *other_rel = (RelOptInfo *) lfirst(l);

		if (!bms_overlap(old_rel->relids, other_rel->relids) &&
			(have_relevant_joinclause(root, old_rel, other_rel) ||
			 have_join_order_restriction(root, old_rel, other_rel)))
		{
			(void) make_join_rel(root, old_rel, other_rel);
		}
	}
}

/*
 * make_rels_by_clauseless_joins
 *	  Given a relation 'old_rel' and a list of other relations
 *	  'other_rels', create a join relation between 'old_rel' and each
 *	  member of 'other_rels' that isn't already included in 'old_rel'.
 *	  The join rels are returned in root->join_rel_level[join_cur_level].
 *
 * 'old_rel' is the relation entry for the relation to be joined
 * 'other_rels': a list containing the other rels to be considered for joining
 *
 * Currently, this is only used with initial rels in other_rels, but it would
 * work for joining to joinrels too.
 */
static void
make_rels_by_clauseless_joins(PlannerInfo *root,
							  RelOptInfo *old_rel,
							  List *other_rels)
{
	ListCell   *l;

	foreach(l, other_rels)
	{
		RelOptInfo *other_rel = (RelOptInfo *) lfirst(l);

		if (!bms_overlap(other_rel->relids, old_rel->relids))
		{
			(void) make_join_rel(root, old_rel, other_rel);
		}
	}
}


/*
 * join_is_legal
 *	   Determine whether a proposed join is legal given the query's
 *	   join order constraints; and if it is, determine the join type.
 *
 * Caller must supply not only the two rels, but the union of their relids.
 * (We could simplify the API by computing joinrelids locally, but this
 * would be redundant work in the normal path through make_join_rel.
 * Note that this value does NOT include the RT index of any outer join that
 * might need to be performed here, so it's not the canonical identifier
 * of the join relation.)
 *
 * On success, *sjinfo_p is set to NULL if this is to be a plain inner join,
 * else it's set to point to the associated SpecialJoinInfo node.  Also,
 * *reversed_p is set true if the given relations need to be swapped to
 * match the SpecialJoinInfo node.
 */
static bool
join_is_legal(PlannerInfo *root, RelOptInfo *rel1, RelOptInfo *rel2,
			  Relids joinrelids,
			  SpecialJoinInfo **sjinfo_p, bool *reversed_p)
{
	SpecialJoinInfo *match_sjinfo;
	bool		reversed;
	bool		unique_ified;
	bool		must_be_leftjoin;
	ListCell   *l;

	/*
	 * Ensure output params are set on failure return.  This is just to
	 * suppress uninitialized-variable warnings from overly anal compilers.
	 */
	*sjinfo_p = NULL;
	*reversed_p = false;

	/*
	 * If we have any special joins, the proposed join might be illegal; and
	 * in any case we have to determine its join type.  Scan the join info
	 * list for matches and conflicts.
	 */
	match_sjinfo = NULL;
	reversed = false;
	unique_ified = false;
	must_be_leftjoin = false;

	foreach(l, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(l);

		/*
		 * This special join is not relevant unless its RHS overlaps the
		 * proposed join.  (Check this first as a fast path for dismissing
		 * most irrelevant SJs quickly.)
		 */
		if (!bms_overlap(sjinfo->min_righthand, joinrelids))
			continue;

		/*
		 * Also, not relevant if proposed join is fully contained within RHS
		 * (ie, we're still building up the RHS).
		 */
		if (bms_is_subset(joinrelids, sjinfo->min_righthand))
			continue;

		/*
		 * Also, not relevant if SJ is already done within either input.
		 */
		if (bms_is_subset(sjinfo->min_lefthand, rel1->relids) &&
			bms_is_subset(sjinfo->min_righthand, rel1->relids))
			continue;
		if (bms_is_subset(sjinfo->min_lefthand, rel2->relids) &&
			bms_is_subset(sjinfo->min_righthand, rel2->relids))
			continue;

		/*
		 * If it's a semijoin and we already joined the RHS to any other rels
		 * within either input, then we must have unique-ified the RHS at that
		 * point (see below).  Therefore the semijoin is no longer relevant in
		 * this join path.
		 */
		if (sjinfo->jointype == JOIN_SEMI)
		{
			if (bms_is_subset(sjinfo->syn_righthand, rel1->relids) &&
				!bms_equal(sjinfo->syn_righthand, rel1->relids))
				continue;
			if (bms_is_subset(sjinfo->syn_righthand, rel2->relids) &&
				!bms_equal(sjinfo->syn_righthand, rel2->relids))
				continue;
		}

		/*
		 * If one input contains min_lefthand and the other contains
		 * min_righthand, then we can perform the SJ at this join.
		 *
		 * Reject if we get matches to more than one SJ; that implies we're
		 * considering something that's not really valid.
		 */
		if (bms_is_subset(sjinfo->min_lefthand, rel1->relids) &&
			bms_is_subset(sjinfo->min_righthand, rel2->relids))
		{
			if (match_sjinfo)
				return false;	/* invalid join path */
			match_sjinfo = sjinfo;
			reversed = false;
		}
		else if (bms_is_subset(sjinfo->min_lefthand, rel2->relids) &&
				 bms_is_subset(sjinfo->min_righthand, rel1->relids))
		{
			if (match_sjinfo)
				return false;	/* invalid join path */
			match_sjinfo = sjinfo;
			reversed = true;
		}
		else if (sjinfo->jointype == JOIN_SEMI &&
				 bms_equal(sjinfo->syn_righthand, rel2->relids) &&
				 create_unique_path(root, rel2, rel2->cheapest_total_path,
									sjinfo) != NULL)
		{
			/*----------
			 * For a semijoin, we can join the RHS to anything else by
			 * unique-ifying the RHS (if the RHS can be unique-ified).
			 * We will only get here if we have the full RHS but less
			 * than min_lefthand on the LHS.
			 *
			 * The reason to consider such a join path is exemplified by
			 *	SELECT ... FROM a,b WHERE (a.x,b.y) IN (SELECT c1,c2 FROM c)
			 * If we insist on doing this as a semijoin we will first have
			 * to form the cartesian product of A*B.  But if we unique-ify
			 * C then the semijoin becomes a plain innerjoin and we can join
			 * in any order, eg C to A and then to B.  When C is much smaller
			 * than A and B this can be a huge win.  So we allow C to be
			 * joined to just A or just B here, and then make_join_rel has
			 * to handle the case properly.
			 *
			 * Note that actually we'll allow unique-ified C to be joined to
			 * some other relation D here, too.  That is legal, if usually not
			 * very sane, and this routine is only concerned with legality not
			 * with whether the join is good strategy.
			 *----------
			 */
			if (match_sjinfo)
				return false;	/* invalid join path */
			match_sjinfo = sjinfo;
			reversed = false;
			unique_ified = true;
		}
		else if (sjinfo->jointype == JOIN_SEMI &&
				 bms_equal(sjinfo->syn_righthand, rel1->relids) &&
				 create_unique_path(root, rel1, rel1->cheapest_total_path,
									sjinfo) != NULL)
		{
			/* Reversed semijoin case */
			if (match_sjinfo)
				return false;	/* invalid join path */
			match_sjinfo = sjinfo;
			reversed = true;
			unique_ified = true;
		}
		else
		{
			/*
			 * Otherwise, the proposed join overlaps the RHS but isn't a valid
			 * implementation of this SJ.  But don't panic quite yet: the RHS
			 * violation might have occurred previously, in one or both input
			 * relations, in which case we must have previously decided that
			 * it was OK to commute some other SJ with this one.  If we need
			 * to perform this join to finish building up the RHS, rejecting
			 * it could lead to not finding any plan at all.  (This can occur
			 * because of the heuristics elsewhere in this file that postpone
			 * clauseless joins: we might not consider doing a clauseless join
			 * within the RHS until after we've performed other, validly
			 * commutable SJs with one or both sides of the clauseless join.)
			 * This consideration boils down to the rule that if both inputs
			 * overlap the RHS, we can allow the join --- they are either
			 * fully within the RHS, or represent previously-allowed joins to
			 * rels outside it.
			 */
			if (bms_overlap(rel1->relids, sjinfo->min_righthand) &&
				bms_overlap(rel2->relids, sjinfo->min_righthand))
				continue;		/* assume valid previous violation of RHS */

			/*
			 * The proposed join could still be legal, but only if we're
			 * allowed to associate it into the RHS of this SJ.  That means
			 * this SJ must be a LEFT join (not SEMI or ANTI, and certainly
			 * not FULL) and the proposed join must not overlap the LHS.
			 */
			if (sjinfo->jointype != JOIN_LEFT ||
				bms_overlap(joinrelids, sjinfo->min_lefthand))
				return false;	/* invalid join path */

			/*
			 * To be valid, the proposed join must be a LEFT join; otherwise
			 * it can't associate into this SJ's RHS.  But we may not yet have
			 * found the SpecialJoinInfo matching the proposed join, so we
			 * can't test that yet.  Remember the requirement for later.
			 */
			must_be_leftjoin = true;
		}
	}

	/*
	 * Fail if violated any SJ's RHS and didn't match to a LEFT SJ: the
	 * proposed join can't associate into an SJ's RHS.
	 *
	 * Also, fail if the proposed join's predicate isn't strict; we're
	 * essentially checking to see if we can apply outer-join identity 3, and
	 * that's a requirement.  (This check may be redundant with checks in
	 * make_outerjoininfo, but I'm not quite sure, and it's cheap to test.)
	 */
	if (must_be_leftjoin &&
		(match_sjinfo == NULL ||
		 match_sjinfo->jointype != JOIN_LEFT ||
		 !match_sjinfo->lhs_strict))
		return false;			/* invalid join path */

	/*
	 * We also have to check for constraints imposed by LATERAL references.
	 */
	if (root->hasLateralRTEs)
	{
		bool		lateral_fwd;
		bool		lateral_rev;
		Relids		join_lateral_rels;

		/*
		 * The proposed rels could each contain lateral references to the
		 * other, in which case the join is impossible.  If there are lateral
		 * references in just one direction, then the join has to be done with
		 * a nestloop with the lateral referencer on the inside.  If the join
		 * matches an SJ that cannot be implemented by such a nestloop, the
		 * join is impossible.
		 *
		 * Also, if the lateral reference is only indirect, we should reject
		 * the join; whatever rel(s) the reference chain goes through must be
		 * joined to first.
		 *
		 * Another case that might keep us from building a valid plan is the
		 * implementation restriction described by have_dangerous_phv().
		 */
		lateral_fwd = bms_overlap(rel1->relids, rel2->lateral_relids);
		lateral_rev = bms_overlap(rel2->relids, rel1->lateral_relids);
		if (lateral_fwd && lateral_rev)
			return false;		/* have lateral refs in both directions */
		if (lateral_fwd)
		{
			/* has to be implemented as nestloop with rel1 on left */
			if (match_sjinfo &&
				(reversed ||
				 unique_ified ||
				 match_sjinfo->jointype == JOIN_FULL))
				return false;	/* not implementable as nestloop */
			/* check there is a direct reference from rel2 to rel1 */
			if (!bms_overlap(rel1->relids, rel2->direct_lateral_relids))
				return false;	/* only indirect refs, so reject */
			/* check we won't have a dangerous PHV */
			if (have_dangerous_phv(root, rel1->relids, rel2->lateral_relids))
				return false;	/* might be unable to handle required PHV */
		}
		else if (lateral_rev)
		{
			/* has to be implemented as nestloop with rel2 on left */
			if (match_sjinfo &&
				(!reversed ||
				 unique_ified ||
				 match_sjinfo->jointype == JOIN_FULL))
				return false;	/* not implementable as nestloop */
			/* check there is a direct reference from rel1 to rel2 */
			if (!bms_overlap(rel2->relids, rel1->direct_lateral_relids))
				return false;	/* only indirect refs, so reject */
			/* check we won't have a dangerous PHV */
			if (have_dangerous_phv(root, rel2->relids, rel1->lateral_relids))
				return false;	/* might be unable to handle required PHV */
		}

		/*
		 * LATERAL references could also cause problems later on if we accept
		 * this join: if the join's minimum parameterization includes any rels
		 * that would have to be on the inside of an outer join with this join
		 * rel, then it's never going to be possible to build the complete
		 * query using this join.  We should reject this join not only because
		 * it'll save work, but because if we don't, the clauseless-join
		 * heuristics might think that legality of this join means that some
		 * other join rel need not be formed, and that could lead to failure
		 * to find any plan at all.  We have to consider not only rels that
		 * are directly on the inner side of an OJ with the joinrel, but also
		 * ones that are indirectly so, so search to find all such rels.
		 */
		join_lateral_rels = min_join_parameterization(root, joinrelids,
													  rel1, rel2);
		if (join_lateral_rels)
		{
			Relids		join_plus_rhs = bms_copy(joinrelids);
			bool		more;

			do
			{
				more = false;
				foreach(l, root->join_info_list)
				{
					SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(l);

					/* ignore full joins --- their ordering is predetermined */
					if (sjinfo->jointype == JOIN_FULL)
						continue;

					if (bms_overlap(sjinfo->min_lefthand, join_plus_rhs) &&
						!bms_is_subset(sjinfo->min_righthand, join_plus_rhs))
					{
						join_plus_rhs = bms_add_members(join_plus_rhs,
														sjinfo->min_righthand);
						more = true;
					}
				}
			} while (more);
			if (bms_overlap(join_plus_rhs, join_lateral_rels))
				return false;	/* will not be able to join to some RHS rel */
		}
	}

	/* Otherwise, it's a valid join */
	*sjinfo_p = match_sjinfo;
	*reversed_p = reversed;
	return true;
}

/*
 * init_dummy_sjinfo
 *    Populate the given SpecialJoinInfo for a plain inner join between the
 *    left and right relations specified by left_relids and right_relids
 *    respectively.
 *
 * Normally, an inner join does not have a SpecialJoinInfo node associated with
 * it. But some functions involved in join planning require one containing at
 * least the information of which relations are being joined.  So we initialize
 * that information here.
 */
void
init_dummy_sjinfo(SpecialJoinInfo *sjinfo, Relids left_relids,
				  Relids right_relids)
{
	sjinfo->type = T_SpecialJoinInfo;
	sjinfo->min_lefthand = left_relids;
	sjinfo->min_righthand = right_relids;
	sjinfo->syn_lefthand = left_relids;
	sjinfo->syn_righthand = right_relids;
	sjinfo->jointype = JOIN_INNER;
	sjinfo->ojrelid = 0;
	sjinfo->commute_above_l = NULL;
	sjinfo->commute_above_r = NULL;
	sjinfo->commute_below_l = NULL;
	sjinfo->commute_below_r = NULL;
	/* we don't bother trying to make the remaining fields valid */
	sjinfo->lhs_strict = false;
	sjinfo->semi_can_btree = false;
	sjinfo->semi_can_hash = false;
	sjinfo->semi_operators = NIL;
	sjinfo->semi_rhs_exprs = NIL;
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
 * when working with outer joins, or with IN or EXISTS clauses that have been
 * turned into joins.
 */
RelOptInfo *
make_join_rel(PlannerInfo *root, RelOptInfo *rel1, RelOptInfo *rel2)
{
	Relids		joinrelids;
	SpecialJoinInfo *sjinfo;
	bool		reversed;
	List	   *pushed_down_joins = NIL;
	SpecialJoinInfo sjinfo_data;
	RelOptInfo *joinrel;
	List	   *restrictlist;

	/* We should never try to join two overlapping sets of rels. */
	Assert(!bms_overlap(rel1->relids, rel2->relids));

	/* Construct Relids set that identifies the joinrel (without OJ as yet). */
	joinrelids = bms_union(rel1->relids, rel2->relids);

	/* Check validity and determine join type. */
	if (!join_is_legal(root, rel1, rel2, joinrelids,
					   &sjinfo, &reversed))
	{
		/* invalid join path */
		bms_free(joinrelids);
		return NULL;
	}

	/*
	 * Add outer join relid(s) to form the canonical relids.  Any added outer
	 * joins besides sjinfo itself are appended to pushed_down_joins.
	 */
	joinrelids = add_outer_joins_to_relids(root, joinrelids, sjinfo,
										   &pushed_down_joins);

	/* Swap rels if needed to match the join info. */
	if (reversed)
	{
		RelOptInfo *trel = rel1;

		rel1 = rel2;
		rel2 = trel;
	}

	/*
	 * If it's a plain inner join, then we won't have found anything in
	 * join_info_list.  Make up a SpecialJoinInfo so that selectivity
	 * estimation functions will know what's being joined.
	 */
	if (sjinfo == NULL)
	{
		sjinfo = &sjinfo_data;
		init_dummy_sjinfo(sjinfo, rel1->relids, rel2->relids);
	}

	/*
	 * Find or build the join RelOptInfo, and compute the restrictlist that
	 * goes with this particular joining.
	 */
	joinrel = build_join_rel(root, joinrelids, rel1, rel2,
							 sjinfo, pushed_down_joins,
							 &restrictlist);

	/*
	 * If we've already proven this join is empty, we needn't consider any
	 * more paths for it.
	 */
	if (is_dummy_rel(joinrel))
	{
		bms_free(joinrelids);
		return joinrel;
	}

	/* Add paths to the join relation. */
	populate_joinrel_with_paths(root, rel1, rel2, joinrel, sjinfo,
								restrictlist);

	bms_free(joinrelids);

	return joinrel;
}

/*
 * add_outer_joins_to_relids
 *	  Add relids to input_relids to represent any outer joins that will be
 *	  calculated at this join.
 *
 * input_relids is the union of the relid sets of the two input relations.
 * Note that we modify this in-place and return it; caller must bms_copy()
 * it first, if a separate value is desired.
 *
 * sjinfo represents the join being performed.
 *
 * If the current join completes the calculation of any outer joins that
 * have been pushed down per outer-join identity 3, those relids will be
 * added to the result along with sjinfo's own relid.  If pushed_down_joins
 * is not NULL, then also the SpecialJoinInfos for such added outer joins will
 * be appended to *pushed_down_joins (so caller must initialize it to NIL).
 */
Relids
add_outer_joins_to_relids(PlannerInfo *root, Relids input_relids,
						  SpecialJoinInfo *sjinfo,
						  List **pushed_down_joins)
{
	/* Nothing to do if this isn't an outer join with an assigned relid. */
	if (sjinfo == NULL || sjinfo->ojrelid == 0)
		return input_relids;

	/*
	 * If it's not a left join, we have no rules that would permit executing
	 * it in non-syntactic order, so just form the syntactic relid set.  (This
	 * is just a quick-exit test; we'd come to the same conclusion anyway,
	 * since its commute_below_l and commute_above_l sets must be empty.)
	 */
	if (sjinfo->jointype != JOIN_LEFT)
		return bms_add_member(input_relids, sjinfo->ojrelid);

	/*
	 * We cannot add the OJ relid if this join has been pushed into the RHS of
	 * a syntactically-lower left join per OJ identity 3.  (If it has, then we
	 * cannot claim that its outputs represent the final state of its RHS.)
	 * There will not be any other OJs that can be added either, so we're
	 * done.
	 */
	if (!bms_is_subset(sjinfo->commute_below_l, input_relids))
		return input_relids;

	/* OK to add OJ's own relid */
	input_relids = bms_add_member(input_relids, sjinfo->ojrelid);

	/*
	 * Contrariwise, if we are now forming the final result of such a commuted
	 * pair of OJs, it's time to add the relid(s) of the pushed-down join(s).
	 * We can skip this if this join was never a candidate to be pushed up.
	 */
	if (sjinfo->commute_above_l)
	{
		Relids		commute_above_rels = bms_copy(sjinfo->commute_above_l);
		ListCell   *lc;

		/*
		 * The current join could complete the nulling of more than one
		 * pushed-down join, so we have to examine all the SpecialJoinInfos.
		 * Because join_info_list was built in bottom-up order, it's
		 * sufficient to traverse it once: an ojrelid we add in one loop
		 * iteration would not have affected decisions of earlier iterations.
		 */
		foreach(lc, root->join_info_list)
		{
			SpecialJoinInfo *othersj = (SpecialJoinInfo *) lfirst(lc);

			if (othersj == sjinfo ||
				othersj->ojrelid == 0 || othersj->jointype != JOIN_LEFT)
				continue;		/* definitely not interesting */

			if (!bms_is_member(othersj->ojrelid, commute_above_rels))
				continue;

			/* Add it if not already present but conditions now satisfied */
			if (!bms_is_member(othersj->ojrelid, input_relids) &&
				bms_is_subset(othersj->min_lefthand, input_relids) &&
				bms_is_subset(othersj->min_righthand, input_relids) &&
				bms_is_subset(othersj->commute_below_l, input_relids))
			{
				input_relids = bms_add_member(input_relids, othersj->ojrelid);
				/* report such pushed down outer joins, if asked */
				if (pushed_down_joins != NULL)
					*pushed_down_joins = lappend(*pushed_down_joins, othersj);

				/*
				 * We must also check any joins that othersj potentially
				 * commutes with.  They likewise must appear later in
				 * join_info_list than othersj itself, so we can visit them
				 * later in this loop.
				 */
				commute_above_rels = bms_add_members(commute_above_rels,
													 othersj->commute_above_l);
			}
		}
	}

	return input_relids;
}

/*
 * populate_joinrel_with_paths
 *	  Add paths to the given joinrel for given pair of joining relations. The
 *	  SpecialJoinInfo provides details about the join and the restrictlist
 *	  contains the join clauses and the other clauses applicable for given pair
 *	  of the joining relations.
 */
static void
populate_joinrel_with_paths(PlannerInfo *root, RelOptInfo *rel1,
							RelOptInfo *rel2, RelOptInfo *joinrel,
							SpecialJoinInfo *sjinfo, List *restrictlist)
{
	/*
	 * Consider paths using each rel as both outer and inner.  Depending on
	 * the join type, a provably empty outer or inner rel might mean the join
	 * is provably empty too; in which case throw away any previously computed
	 * paths and mark the join as dummy.  (We do it this way since it's
	 * conceivable that dummy-ness of a multi-element join might only be
	 * noticeable for certain construction paths.)
	 *
	 * Also, a provably constant-false join restriction typically means that
	 * we can skip evaluating one or both sides of the join.  We do this by
	 * marking the appropriate rel as dummy.  For outer joins, a
	 * constant-false restriction that is pushed down still means the whole
	 * join is dummy, while a non-pushed-down one means that no inner rows
	 * will join so we can treat the inner rel as dummy.
	 *
	 * We need only consider the jointypes that appear in join_info_list, plus
	 * JOIN_INNER.
	 */
	switch (sjinfo->jointype)
	{
		case JOIN_INNER:
			if (is_dummy_rel(rel1) || is_dummy_rel(rel2) ||
				restriction_is_constant_false(restrictlist, joinrel, false))
			{
				mark_dummy_rel(joinrel);
				break;
			}
			add_paths_to_joinrel(root, joinrel, rel1, rel2,
								 JOIN_INNER, sjinfo,
								 restrictlist);
			add_paths_to_joinrel(root, joinrel, rel2, rel1,
								 JOIN_INNER, sjinfo,
								 restrictlist);
			break;
		case JOIN_LEFT:
			if (is_dummy_rel(rel1) ||
				restriction_is_constant_false(restrictlist, joinrel, true))
			{
				mark_dummy_rel(joinrel);
				break;
			}
			if (restriction_is_constant_false(restrictlist, joinrel, false) &&
				bms_is_subset(rel2->relids, sjinfo->syn_righthand))
				mark_dummy_rel(rel2);
			add_paths_to_joinrel(root, joinrel, rel1, rel2,
								 JOIN_LEFT, sjinfo,
								 restrictlist);
			add_paths_to_joinrel(root, joinrel, rel2, rel1,
								 JOIN_RIGHT, sjinfo,
								 restrictlist);
			break;
		case JOIN_FULL:
			if ((is_dummy_rel(rel1) && is_dummy_rel(rel2)) ||
				restriction_is_constant_false(restrictlist, joinrel, true))
			{
				mark_dummy_rel(joinrel);
				break;
			}
			add_paths_to_joinrel(root, joinrel, rel1, rel2,
								 JOIN_FULL, sjinfo,
								 restrictlist);
			add_paths_to_joinrel(root, joinrel, rel2, rel1,
								 JOIN_FULL, sjinfo,
								 restrictlist);

			/*
			 * If there are join quals that aren't mergeable or hashable, we
			 * may not be able to build any valid plan.  Complain here so that
			 * we can give a somewhat-useful error message.  (Since we have no
			 * flexibility of planning for a full join, there's no chance of
			 * succeeding later with another pair of input rels.)
			 */
			if (joinrel->pathlist == NIL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("FULL JOIN is only supported with merge-joinable or hash-joinable join conditions")));
			break;
		case JOIN_SEMI:

			/*
			 * We might have a normal semijoin, or a case where we don't have
			 * enough rels to do the semijoin but can unique-ify the RHS and
			 * then do an innerjoin (see comments in join_is_legal).  In the
			 * latter case we can't apply JOIN_SEMI joining.
			 */
			if (bms_is_subset(sjinfo->min_lefthand, rel1->relids) &&
				bms_is_subset(sjinfo->min_righthand, rel2->relids))
			{
				if (is_dummy_rel(rel1) || is_dummy_rel(rel2) ||
					restriction_is_constant_false(restrictlist, joinrel, false))
				{
					mark_dummy_rel(joinrel);
					break;
				}
				add_paths_to_joinrel(root, joinrel, rel1, rel2,
									 JOIN_SEMI, sjinfo,
									 restrictlist);
				add_paths_to_joinrel(root, joinrel, rel2, rel1,
									 JOIN_RIGHT_SEMI, sjinfo,
									 restrictlist);
			}

			/*
			 * If we know how to unique-ify the RHS and one input rel is
			 * exactly the RHS (not a superset) we can consider unique-ifying
			 * it and then doing a regular join.  (The create_unique_path
			 * check here is probably redundant with what join_is_legal did,
			 * but if so the check is cheap because it's cached.  So test
			 * anyway to be sure.)
			 */
			if (bms_equal(sjinfo->syn_righthand, rel2->relids) &&
				create_unique_path(root, rel2, rel2->cheapest_total_path,
								   sjinfo) != NULL)
			{
				if (is_dummy_rel(rel1) || is_dummy_rel(rel2) ||
					restriction_is_constant_false(restrictlist, joinrel, false))
				{
					mark_dummy_rel(joinrel);
					break;
				}
				add_paths_to_joinrel(root, joinrel, rel1, rel2,
									 JOIN_UNIQUE_INNER, sjinfo,
									 restrictlist);
				add_paths_to_joinrel(root, joinrel, rel2, rel1,
									 JOIN_UNIQUE_OUTER, sjinfo,
									 restrictlist);
			}
			break;
		case JOIN_ANTI:
			if (is_dummy_rel(rel1) ||
				restriction_is_constant_false(restrictlist, joinrel, true))
			{
				mark_dummy_rel(joinrel);
				break;
			}
			if (restriction_is_constant_false(restrictlist, joinrel, false) &&
				bms_is_subset(rel2->relids, sjinfo->syn_righthand))
				mark_dummy_rel(rel2);
			add_paths_to_joinrel(root, joinrel, rel1, rel2,
								 JOIN_ANTI, sjinfo,
								 restrictlist);
			add_paths_to_joinrel(root, joinrel, rel2, rel1,
								 JOIN_RIGHT_ANTI, sjinfo,
								 restrictlist);
			break;
		default:
			/* other values not expected here */
			elog(ERROR, "unrecognized join type: %d", (int) sjinfo->jointype);
			break;
	}

	/* Apply partitionwise join technique, if possible. */
	try_partitionwise_join(root, rel1, rel2, joinrel, sjinfo, restrictlist);
}


/*
 * have_join_order_restriction
 *		Detect whether the two relations should be joined to satisfy
 *		a join-order restriction arising from special or lateral joins.
 *
 * In practice this is always used with have_relevant_joinclause(), and so
 * could be merged with that function, but it seems clearer to separate the
 * two concerns.  We need this test because there are degenerate cases where
 * a clauseless join must be performed to satisfy join-order restrictions.
 * Also, if one rel has a lateral reference to the other, or both are needed
 * to compute some PHV, we should consider joining them even if the join would
 * be clauseless.
 *
 * Note: this is only a problem if one side of a degenerate outer join
 * contains multiple rels, or a clauseless join is required within an
 * IN/EXISTS RHS; else we will find a join path via the "last ditch" case in
 * join_search_one_level().  We could dispense with this test if we were
 * willing to try bushy plans in the "last ditch" case, but that seems much
 * less efficient.
 */
bool
have_join_order_restriction(PlannerInfo *root,
							RelOptInfo *rel1, RelOptInfo *rel2)
{
	bool		result = false;
	ListCell   *l;

	/*
	 * If either side has a direct lateral reference to the other, attempt the
	 * join regardless of outer-join considerations.
	 */
	if (bms_overlap(rel1->relids, rel2->direct_lateral_relids) ||
		bms_overlap(rel2->relids, rel1->direct_lateral_relids))
		return true;

	/*
	 * Likewise, if both rels are needed to compute some PlaceHolderVar,
	 * attempt the join regardless of outer-join considerations.  (This is not
	 * very desirable, because a PHV with a large eval_at set will cause a lot
	 * of probably-useless joins to be considered, but failing to do this can
	 * cause us to fail to construct a plan at all.)
	 */
	foreach(l, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(l);

		if (bms_is_subset(rel1->relids, phinfo->ph_eval_at) &&
			bms_is_subset(rel2->relids, phinfo->ph_eval_at))
			return true;
	}

	/*
	 * It's possible that the rels correspond to the left and right sides of a
	 * degenerate outer join, that is, one with no joinclause mentioning the
	 * non-nullable side; in which case we should force the join to occur.
	 *
	 * Also, the two rels could represent a clauseless join that has to be
	 * completed to build up the LHS or RHS of an outer join.
	 */
	foreach(l, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(l);

		/* ignore full joins --- other mechanisms handle them */
		if (sjinfo->jointype == JOIN_FULL)
			continue;

		/* Can we perform the SJ with these rels? */
		if (bms_is_subset(sjinfo->min_lefthand, rel1->relids) &&
			bms_is_subset(sjinfo->min_righthand, rel2->relids))
		{
			result = true;
			break;
		}
		if (bms_is_subset(sjinfo->min_lefthand, rel2->relids) &&
			bms_is_subset(sjinfo->min_righthand, rel1->relids))
		{
			result = true;
			break;
		}

		/*
		 * Might we need to join these rels to complete the RHS?  We have to
		 * use "overlap" tests since either rel might include a lower SJ that
		 * has been proven to commute with this one.
		 */
		if (bms_overlap(sjinfo->min_righthand, rel1->relids) &&
			bms_overlap(sjinfo->min_righthand, rel2->relids))
		{
			result = true;
			break;
		}

		/* Likewise for the LHS. */
		if (bms_overlap(sjinfo->min_lefthand, rel1->relids) &&
			bms_overlap(sjinfo->min_lefthand, rel2->relids))
		{
			result = true;
			break;
		}
	}

	/*
	 * We do not force the join to occur if either input rel can legally be
	 * joined to anything else using joinclauses.  This essentially means that
	 * clauseless bushy joins are put off as long as possible. The reason is
	 * that when there is a join order restriction high up in the join tree
	 * (that is, with many rels inside the LHS or RHS), we would otherwise
	 * expend lots of effort considering very stupid join combinations within
	 * its LHS or RHS.
	 */
	if (result)
	{
		if (has_legal_joinclause(root, rel1) ||
			has_legal_joinclause(root, rel2))
			result = false;
	}

	return result;
}


/*
 * has_join_restriction
 *		Detect whether the specified relation has join-order restrictions,
 *		due to being inside an outer join or an IN (sub-SELECT),
 *		or participating in any LATERAL references or multi-rel PHVs.
 *
 * Essentially, this tests whether have_join_order_restriction() could
 * succeed with this rel and some other one.  It's OK if we sometimes
 * say "true" incorrectly.  (Therefore, we don't bother with the relatively
 * expensive has_legal_joinclause test.)
 */
static bool
has_join_restriction(PlannerInfo *root, RelOptInfo *rel)
{
	ListCell   *l;

	if (rel->lateral_relids != NULL || rel->lateral_referencers != NULL)
		return true;

	foreach(l, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(l);

		if (bms_is_subset(rel->relids, phinfo->ph_eval_at) &&
			!bms_equal(rel->relids, phinfo->ph_eval_at))
			return true;
	}

	foreach(l, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(l);

		/* ignore full joins --- other mechanisms preserve their ordering */
		if (sjinfo->jointype == JOIN_FULL)
			continue;

		/* ignore if SJ is already contained in rel */
		if (bms_is_subset(sjinfo->min_lefthand, rel->relids) &&
			bms_is_subset(sjinfo->min_righthand, rel->relids))
			continue;

		/* restricted if it overlaps LHS or RHS, but doesn't contain SJ */
		if (bms_overlap(sjinfo->min_lefthand, rel->relids) ||
			bms_overlap(sjinfo->min_righthand, rel->relids))
			return true;
	}

	return false;
}


/*
 * has_legal_joinclause
 *		Detect whether the specified relation can legally be joined
 *		to any other rels using join clauses.
 *
 * We consider only joins to single other relations in the current
 * initial_rels list.  This is sufficient to get a "true" result in most real
 * queries, and an occasional erroneous "false" will only cost a bit more
 * planning time.  The reason for this limitation is that considering joins to
 * other joins would require proving that the other join rel can legally be
 * formed, which seems like too much trouble for something that's only a
 * heuristic to save planning time.  (Note: we must look at initial_rels
 * and not all of the query, since when we are planning a sub-joinlist we
 * may be forced to make clauseless joins within initial_rels even though
 * there are join clauses linking to other parts of the query.)
 */
static bool
has_legal_joinclause(PlannerInfo *root, RelOptInfo *rel)
{
	ListCell   *lc;

	foreach(lc, root->initial_rels)
	{
		RelOptInfo *rel2 = (RelOptInfo *) lfirst(lc);

		/* ignore rels that are already in "rel" */
		if (bms_overlap(rel->relids, rel2->relids))
			continue;

		if (have_relevant_joinclause(root, rel, rel2))
		{
			Relids		joinrelids;
			SpecialJoinInfo *sjinfo;
			bool		reversed;

			/* join_is_legal needs relids of the union */
			joinrelids = bms_union(rel->relids, rel2->relids);

			if (join_is_legal(root, rel, rel2, joinrelids,
							  &sjinfo, &reversed))
			{
				/* Yes, this will work */
				bms_free(joinrelids);
				return true;
			}

			bms_free(joinrelids);
		}
	}

	return false;
}


/*
 * There's a pitfall for creating parameterized nestloops: suppose the inner
 * rel (call it A) has a parameter that is a PlaceHolderVar, and that PHV's
 * minimum eval_at set includes the outer rel (B) and some third rel (C).
 * We might think we could create a B/A nestloop join that's parameterized by
 * C.  But we would end up with a plan in which the PHV's expression has to be
 * evaluated as a nestloop parameter at the B/A join; and the executor is only
 * set up to handle simple Vars as NestLoopParams.  Rather than add complexity
 * and overhead to the executor for such corner cases, it seems better to
 * forbid the join.  (Note that we can still make use of A's parameterized
 * path with pre-joined B+C as the outer rel.  have_join_order_restriction()
 * ensures that we will consider making such a join even if there are not
 * other reasons to do so.)
 *
 * So we check whether any PHVs used in the query could pose such a hazard.
 * We don't have any simple way of checking whether a risky PHV would actually
 * be used in the inner plan, and the case is so unusual that it doesn't seem
 * worth working very hard on it.
 *
 * This needs to be checked in two places.  If the inner rel's minimum
 * parameterization would trigger the restriction, then join_is_legal() should
 * reject the join altogether, because there will be no workable paths for it.
 * But joinpath.c has to check again for every proposed nestloop path, because
 * the inner path might have more than the minimum parameterization, causing
 * some PHV to be dangerous for it that otherwise wouldn't be.
 */
bool
have_dangerous_phv(PlannerInfo *root,
				   Relids outer_relids, Relids inner_params)
{
	ListCell   *lc;

	foreach(lc, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(lc);

		if (!bms_is_subset(phinfo->ph_eval_at, inner_params))
			continue;			/* ignore, could not be a nestloop param */
		if (!bms_overlap(phinfo->ph_eval_at, outer_relids))
			continue;			/* ignore, not relevant to this join */
		if (bms_is_subset(phinfo->ph_eval_at, outer_relids))
			continue;			/* safe, it can be eval'd within outerrel */
		/* Otherwise, it's potentially unsafe, so reject the join */
		return true;
	}

	/* OK to perform the join */
	return false;
}


/*
 * is_dummy_rel --- has relation been proven empty?
 */
bool
is_dummy_rel(RelOptInfo *rel)
{
	Path	   *path;

	/*
	 * A rel that is known dummy will have just one path that is a childless
	 * Append.  (Even if somehow it has more paths, a childless Append will
	 * have cost zero and hence should be at the front of the pathlist.)
	 */
	if (rel->pathlist == NIL)
		return false;
	path = (Path *) linitial(rel->pathlist);

	/*
	 * Initially, a dummy path will just be a childless Append.  But in later
	 * planning stages we might stick a ProjectSetPath and/or ProjectionPath
	 * on top, since Append can't project.  Rather than make assumptions about
	 * which combinations can occur, just descend through whatever we find.
	 */
	for (;;)
	{
		if (IsA(path, ProjectionPath))
			path = ((ProjectionPath *) path)->subpath;
		else if (IsA(path, ProjectSetPath))
			path = ((ProjectSetPath *) path)->subpath;
		else
			break;
	}
	if (IS_DUMMY_APPEND(path))
		return true;
	return false;
}

/*
 * Mark a relation as proven empty.
 *
 * During GEQO planning, this can get invoked more than once on the same
 * baserel struct, so it's worth checking to see if the rel is already marked
 * dummy.
 *
 * Also, when called during GEQO join planning, we are in a short-lived
 * memory context.  We must make sure that the dummy path attached to a
 * baserel survives the GEQO cycle, else the baserel is trashed for future
 * GEQO cycles.  On the other hand, when we are marking a joinrel during GEQO,
 * we don't want the dummy path to clutter the main planning context.  Upshot
 * is that the best solution is to explicitly make the dummy path in the same
 * context the given RelOptInfo is in.
 */
void
mark_dummy_rel(RelOptInfo *rel)
{
	MemoryContext oldcontext;

	/* Already marked? */
	if (is_dummy_rel(rel))
		return;

	/* No, so choose correct context to make the dummy path in */
	oldcontext = MemoryContextSwitchTo(GetMemoryChunkContext(rel));

	/* Set dummy size estimate */
	rel->rows = 0;

	/* Evict any previously chosen paths */
	rel->pathlist = NIL;
	rel->partial_pathlist = NIL;

	/* Set up the dummy path */
	add_path(rel, (Path *) create_append_path(NULL, rel, NIL, NIL,
											  NIL, rel->lateral_relids,
											  0, false, -1));

	/* Set or update cheapest_total_path and related fields */
	set_cheapest(rel);

	MemoryContextSwitchTo(oldcontext);
}


/*
 * restriction_is_constant_false --- is a restrictlist just FALSE?
 *
 * In cases where a qual is provably constant FALSE, eval_const_expressions
 * will generally have thrown away anything that's ANDed with it.  In outer
 * join situations this will leave us computing cartesian products only to
 * decide there's no match for an outer row, which is pretty stupid.  So,
 * we need to detect the case.
 *
 * If only_pushed_down is true, then consider only quals that are pushed-down
 * from the point of view of the joinrel.
 */
static bool
restriction_is_constant_false(List *restrictlist,
							  RelOptInfo *joinrel,
							  bool only_pushed_down)
{
	ListCell   *lc;

	/*
	 * Despite the above comment, the restriction list we see here might
	 * possibly have other members besides the FALSE constant, since other
	 * quals could get "pushed down" to the outer join level.  So we check
	 * each member of the list.
	 */
	foreach(lc, restrictlist)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		if (only_pushed_down && !RINFO_IS_PUSHED_DOWN(rinfo, joinrel->relids))
			continue;

		if (rinfo->clause && IsA(rinfo->clause, Const))
		{
			Const	   *con = (Const *) rinfo->clause;

			/* constant NULL is as good as constant FALSE for our purposes */
			if (con->constisnull)
				return true;
			if (!DatumGetBool(con->constvalue))
				return true;
		}
	}
	return false;
}

/*
 * Assess whether join between given two partitioned relations can be broken
 * down into joins between matching partitions; a technique called
 * "partitionwise join"
 *
 * Partitionwise join is possible when a. Joining relations have same
 * partitioning scheme b. There exists an equi-join between the partition keys
 * of the two relations.
 *
 * Partitionwise join is planned as follows (details: optimizer/README.)
 *
 * 1. Create the RelOptInfos for joins between matching partitions i.e
 * child-joins and add paths to them.
 *
 * 2. Construct Append or MergeAppend paths across the set of child joins.
 * This second phase is implemented by generate_partitionwise_join_paths().
 *
 * The RelOptInfo, SpecialJoinInfo and restrictlist for each child join are
 * obtained by translating the respective parent join structures.
 */
static void
try_partitionwise_join(PlannerInfo *root, RelOptInfo *rel1, RelOptInfo *rel2,
					   RelOptInfo *joinrel, SpecialJoinInfo *parent_sjinfo,
					   List *parent_restrictlist)
{
	bool		rel1_is_simple = IS_SIMPLE_REL(rel1);
	bool		rel2_is_simple = IS_SIMPLE_REL(rel2);
	List	   *parts1 = NIL;
	List	   *parts2 = NIL;
	ListCell   *lcr1 = NULL;
	ListCell   *lcr2 = NULL;
	int			cnt_parts;

	/* Guard against stack overflow due to overly deep partition hierarchy. */
	check_stack_depth();

	/* Nothing to do, if the join relation is not partitioned. */
	if (joinrel->part_scheme == NULL || joinrel->nparts == 0)
		return;

	/* The join relation should have consider_partitionwise_join set. */
	Assert(joinrel->consider_partitionwise_join);

	/*
	 * We can not perform partitionwise join if either of the joining
	 * relations is not partitioned.
	 */
	if (!IS_PARTITIONED_REL(rel1) || !IS_PARTITIONED_REL(rel2))
		return;

	Assert(REL_HAS_ALL_PART_PROPS(rel1) && REL_HAS_ALL_PART_PROPS(rel2));

	/* The joining relations should have consider_partitionwise_join set. */
	Assert(rel1->consider_partitionwise_join &&
		   rel2->consider_partitionwise_join);

	/*
	 * The partition scheme of the join relation should match that of the
	 * joining relations.
	 */
	Assert(joinrel->part_scheme == rel1->part_scheme &&
		   joinrel->part_scheme == rel2->part_scheme);

	Assert(!(joinrel->partbounds_merged && (joinrel->nparts <= 0)));

	compute_partition_bounds(root, rel1, rel2, joinrel, parent_sjinfo,
							 &parts1, &parts2);

	if (joinrel->partbounds_merged)
	{
		lcr1 = list_head(parts1);
		lcr2 = list_head(parts2);
	}

	/*
	 * Create child-join relations for this partitioned join, if those don't
	 * exist. Add paths to child-joins for a pair of child relations
	 * corresponding to the given pair of parent relations.
	 */
	for (cnt_parts = 0; cnt_parts < joinrel->nparts; cnt_parts++)
	{
		RelOptInfo *child_rel1;
		RelOptInfo *child_rel2;
		bool		rel1_empty;
		bool		rel2_empty;
		SpecialJoinInfo *child_sjinfo;
		List	   *child_restrictlist;
		RelOptInfo *child_joinrel;
		AppendRelInfo **appinfos;
		int			nappinfos;
		Relids		child_relids;

		if (joinrel->partbounds_merged)
		{
			child_rel1 = lfirst_node(RelOptInfo, lcr1);
			child_rel2 = lfirst_node(RelOptInfo, lcr2);
			lcr1 = lnext(parts1, lcr1);
			lcr2 = lnext(parts2, lcr2);
		}
		else
		{
			child_rel1 = rel1->part_rels[cnt_parts];
			child_rel2 = rel2->part_rels[cnt_parts];
		}

		rel1_empty = (child_rel1 == NULL || IS_DUMMY_REL(child_rel1));
		rel2_empty = (child_rel2 == NULL || IS_DUMMY_REL(child_rel2));

		/*
		 * Check for cases where we can prove that this segment of the join
		 * returns no rows, due to one or both inputs being empty (including
		 * inputs that have been pruned away entirely).  If so just ignore it.
		 * These rules are equivalent to populate_joinrel_with_paths's rules
		 * for dummy input relations.
		 */
		switch (parent_sjinfo->jointype)
		{
			case JOIN_INNER:
			case JOIN_SEMI:
				if (rel1_empty || rel2_empty)
					continue;	/* ignore this join segment */
				break;
			case JOIN_LEFT:
			case JOIN_ANTI:
				if (rel1_empty)
					continue;	/* ignore this join segment */
				break;
			case JOIN_FULL:
				if (rel1_empty && rel2_empty)
					continue;	/* ignore this join segment */
				break;
			default:
				/* other values not expected here */
				elog(ERROR, "unrecognized join type: %d",
					 (int) parent_sjinfo->jointype);
				break;
		}

		/*
		 * If a child has been pruned entirely then we can't generate paths
		 * for it, so we have to reject partitionwise joining unless we were
		 * able to eliminate this partition above.
		 */
		if (child_rel1 == NULL || child_rel2 == NULL)
		{
			/*
			 * Mark the joinrel as unpartitioned so that later functions treat
			 * it correctly.
			 */
			joinrel->nparts = 0;
			return;
		}

		/*
		 * If a leaf relation has consider_partitionwise_join=false, it means
		 * that it's a dummy relation for which we skipped setting up tlist
		 * expressions and adding EC members in set_append_rel_size(), so
		 * again we have to fail here.
		 */
		if (rel1_is_simple && !child_rel1->consider_partitionwise_join)
		{
			Assert(child_rel1->reloptkind == RELOPT_OTHER_MEMBER_REL);
			Assert(IS_DUMMY_REL(child_rel1));
			joinrel->nparts = 0;
			return;
		}
		if (rel2_is_simple && !child_rel2->consider_partitionwise_join)
		{
			Assert(child_rel2->reloptkind == RELOPT_OTHER_MEMBER_REL);
			Assert(IS_DUMMY_REL(child_rel2));
			joinrel->nparts = 0;
			return;
		}

		/* We should never try to join two overlapping sets of rels. */
		Assert(!bms_overlap(child_rel1->relids, child_rel2->relids));

		/*
		 * Construct SpecialJoinInfo from parent join relations's
		 * SpecialJoinInfo.
		 */
		child_sjinfo = build_child_join_sjinfo(root, parent_sjinfo,
											   child_rel1->relids,
											   child_rel2->relids);

		/* Find the AppendRelInfo structures */
		child_relids = bms_union(child_rel1->relids, child_rel2->relids);
		appinfos = find_appinfos_by_relids(root, child_relids,
										   &nappinfos);

		/*
		 * Construct restrictions applicable to the child join from those
		 * applicable to the parent join.
		 */
		child_restrictlist =
			(List *) adjust_appendrel_attrs(root,
											(Node *) parent_restrictlist,
											nappinfos, appinfos);

		/* Find or construct the child join's RelOptInfo */
		child_joinrel = joinrel->part_rels[cnt_parts];
		if (!child_joinrel)
		{
			child_joinrel = build_child_join_rel(root, child_rel1, child_rel2,
												 joinrel, child_restrictlist,
												 child_sjinfo, nappinfos, appinfos);
			joinrel->part_rels[cnt_parts] = child_joinrel;
			joinrel->live_parts = bms_add_member(joinrel->live_parts, cnt_parts);
			joinrel->all_partrels = bms_add_members(joinrel->all_partrels,
													child_joinrel->relids);
		}

		/* Assert we got the right one */
		Assert(bms_equal(child_joinrel->relids,
						 adjust_child_relids(joinrel->relids,
											 nappinfos, appinfos)));

		/* And make paths for the child join */
		populate_joinrel_with_paths(root, child_rel1, child_rel2,
									child_joinrel, child_sjinfo,
									child_restrictlist);

		/*
		 * When there are thousands of partitions involved, this loop will
		 * accumulate a significant amount of memory usage from objects that
		 * are only needed within the loop.  Free these local objects eagerly
		 * at the end of each iteration.
		 */
		pfree(appinfos);
		bms_free(child_relids);
		free_child_join_sjinfo(child_sjinfo, parent_sjinfo);
	}
}

/*
 * Construct the SpecialJoinInfo for a child-join by translating
 * SpecialJoinInfo for the join between parents. left_relids and right_relids
 * are the relids of left and right side of the join respectively.
 *
 * If translations are added to or removed from this function, consider
 * updating free_child_join_sjinfo() accordingly.
 */
static SpecialJoinInfo *
build_child_join_sjinfo(PlannerInfo *root, SpecialJoinInfo *parent_sjinfo,
						Relids left_relids, Relids right_relids)
{
	SpecialJoinInfo *sjinfo = makeNode(SpecialJoinInfo);
	AppendRelInfo **left_appinfos;
	int			left_nappinfos;
	AppendRelInfo **right_appinfos;
	int			right_nappinfos;

	/* Dummy SpecialJoinInfos can be created without any translation. */
	if (parent_sjinfo->jointype == JOIN_INNER)
	{
		Assert(parent_sjinfo->ojrelid == 0);
		init_dummy_sjinfo(sjinfo, left_relids, right_relids);
		return sjinfo;
	}

	memcpy(sjinfo, parent_sjinfo, sizeof(SpecialJoinInfo));
	left_appinfos = find_appinfos_by_relids(root, left_relids,
											&left_nappinfos);
	right_appinfos = find_appinfos_by_relids(root, right_relids,
											 &right_nappinfos);

	sjinfo->min_lefthand = adjust_child_relids(sjinfo->min_lefthand,
											   left_nappinfos, left_appinfos);
	sjinfo->min_righthand = adjust_child_relids(sjinfo->min_righthand,
												right_nappinfos,
												right_appinfos);
	sjinfo->syn_lefthand = adjust_child_relids(sjinfo->syn_lefthand,
											   left_nappinfos, left_appinfos);
	sjinfo->syn_righthand = adjust_child_relids(sjinfo->syn_righthand,
												right_nappinfos,
												right_appinfos);
	/* outer-join relids need no adjustment */
	sjinfo->semi_rhs_exprs = (List *) adjust_appendrel_attrs(root,
															 (Node *) sjinfo->semi_rhs_exprs,
															 right_nappinfos,
															 right_appinfos);

	pfree(left_appinfos);
	pfree(right_appinfos);

	return sjinfo;
}

/*
 * free_child_join_sjinfo
 *		Free memory consumed by a SpecialJoinInfo created by
 *		build_child_join_sjinfo()
 *
 * Only members that are translated copies of their counterpart in the parent
 * SpecialJoinInfo are freed here.
 */
static void
free_child_join_sjinfo(SpecialJoinInfo *child_sjinfo,
					   SpecialJoinInfo *parent_sjinfo)
{
	/*
	 * Dummy SpecialJoinInfos of inner joins do not have any translated fields
	 * and hence no fields that to be freed.
	 */
	if (child_sjinfo->jointype != JOIN_INNER)
	{
		if (child_sjinfo->min_lefthand != parent_sjinfo->min_lefthand)
			bms_free(child_sjinfo->min_lefthand);

		if (child_sjinfo->min_righthand != parent_sjinfo->min_righthand)
			bms_free(child_sjinfo->min_righthand);

		if (child_sjinfo->syn_lefthand != parent_sjinfo->syn_lefthand)
			bms_free(child_sjinfo->syn_lefthand);

		if (child_sjinfo->syn_righthand != parent_sjinfo->syn_righthand)
			bms_free(child_sjinfo->syn_righthand);

		Assert(child_sjinfo->commute_above_l == parent_sjinfo->commute_above_l);
		Assert(child_sjinfo->commute_above_r == parent_sjinfo->commute_above_r);
		Assert(child_sjinfo->commute_below_l == parent_sjinfo->commute_below_l);
		Assert(child_sjinfo->commute_below_r == parent_sjinfo->commute_below_r);

		Assert(child_sjinfo->semi_operators == parent_sjinfo->semi_operators);

		/*
		 * semi_rhs_exprs may in principle be freed, but a simple pfree() does
		 * not suffice, so we leave it alone.
		 */
	}

	pfree(child_sjinfo);
}

/*
 * compute_partition_bounds
 *		Compute the partition bounds for a join rel from those for inputs
 */
static void
compute_partition_bounds(PlannerInfo *root, RelOptInfo *rel1,
						 RelOptInfo *rel2, RelOptInfo *joinrel,
						 SpecialJoinInfo *parent_sjinfo,
						 List **parts1, List **parts2)
{
	/*
	 * If we don't have the partition bounds for the join rel yet, try to
	 * compute those along with pairs of partitions to be joined.
	 */
	if (joinrel->nparts == -1)
	{
		PartitionScheme part_scheme = joinrel->part_scheme;
		PartitionBoundInfo boundinfo = NULL;
		int			nparts = 0;

		Assert(joinrel->boundinfo == NULL);
		Assert(joinrel->part_rels == NULL);

		/*
		 * See if the partition bounds for inputs are exactly the same, in
		 * which case we don't need to work hard: the join rel will have the
		 * same partition bounds as inputs, and the partitions with the same
		 * cardinal positions will form the pairs.
		 *
		 * Note: even in cases where one or both inputs have merged bounds, it
		 * would be possible for both the bounds to be exactly the same, but
		 * it seems unlikely to be worth the cycles to check.
		 */
		if (!rel1->partbounds_merged &&
			!rel2->partbounds_merged &&
			rel1->nparts == rel2->nparts &&
			partition_bounds_equal(part_scheme->partnatts,
								   part_scheme->parttyplen,
								   part_scheme->parttypbyval,
								   rel1->boundinfo, rel2->boundinfo))
		{
			boundinfo = rel1->boundinfo;
			nparts = rel1->nparts;
		}
		else
		{
			/* Try merging the partition bounds for inputs. */
			boundinfo = partition_bounds_merge(part_scheme->partnatts,
											   part_scheme->partsupfunc,
											   part_scheme->partcollation,
											   rel1, rel2,
											   parent_sjinfo->jointype,
											   parts1, parts2);
			if (boundinfo == NULL)
			{
				joinrel->nparts = 0;
				return;
			}
			nparts = list_length(*parts1);
			joinrel->partbounds_merged = true;
		}

		Assert(nparts > 0);
		joinrel->boundinfo = boundinfo;
		joinrel->nparts = nparts;
		joinrel->part_rels =
			(RelOptInfo **) palloc0(sizeof(RelOptInfo *) * nparts);
	}
	else
	{
		Assert(joinrel->nparts > 0);
		Assert(joinrel->boundinfo);
		Assert(joinrel->part_rels);

		/*
		 * If the join rel's partbounds_merged flag is true, it means inputs
		 * are not guaranteed to have the same partition bounds, therefore we
		 * can't assume that the partitions at the same cardinal positions
		 * form the pairs; let get_matching_part_pairs() generate the pairs.
		 * Otherwise, nothing to do since we can assume that.
		 */
		if (joinrel->partbounds_merged)
		{
			get_matching_part_pairs(root, joinrel, rel1, rel2,
									parts1, parts2);
			Assert(list_length(*parts1) == joinrel->nparts);
			Assert(list_length(*parts2) == joinrel->nparts);
		}
	}
}

/*
 * get_matching_part_pairs
 *		Generate pairs of partitions to be joined from inputs
 */
static void
get_matching_part_pairs(PlannerInfo *root, RelOptInfo *joinrel,
						RelOptInfo *rel1, RelOptInfo *rel2,
						List **parts1, List **parts2)
{
	bool		rel1_is_simple = IS_SIMPLE_REL(rel1);
	bool		rel2_is_simple = IS_SIMPLE_REL(rel2);
	int			cnt_parts;

	*parts1 = NIL;
	*parts2 = NIL;

	for (cnt_parts = 0; cnt_parts < joinrel->nparts; cnt_parts++)
	{
		RelOptInfo *child_joinrel = joinrel->part_rels[cnt_parts];
		RelOptInfo *child_rel1;
		RelOptInfo *child_rel2;
		Relids		child_relids1;
		Relids		child_relids2;

		/*
		 * If this segment of the join is empty, it means that this segment
		 * was ignored when previously creating child-join paths for it in
		 * try_partitionwise_join() as it would not contribute to the join
		 * result, due to one or both inputs being empty; add NULL to each of
		 * the given lists so that this segment will be ignored again in that
		 * function.
		 */
		if (!child_joinrel)
		{
			*parts1 = lappend(*parts1, NULL);
			*parts2 = lappend(*parts2, NULL);
			continue;
		}

		/*
		 * Get a relids set of partition(s) involved in this join segment that
		 * are from the rel1 side.
		 */
		child_relids1 = bms_intersect(child_joinrel->relids,
									  rel1->all_partrels);
		Assert(bms_num_members(child_relids1) == bms_num_members(rel1->relids));

		/*
		 * Get a child rel for rel1 with the relids.  Note that we should have
		 * the child rel even if rel1 is a join rel, because in that case the
		 * partitions specified in the relids would have matching/overlapping
		 * boundaries, so the specified partitions should be considered as
		 * ones to be joined when planning partitionwise joins of rel1,
		 * meaning that the child rel would have been built by the time we get
		 * here.
		 */
		if (rel1_is_simple)
		{
			int			varno = bms_singleton_member(child_relids1);

			child_rel1 = find_base_rel(root, varno);
		}
		else
			child_rel1 = find_join_rel(root, child_relids1);
		Assert(child_rel1);

		/*
		 * Get a relids set of partition(s) involved in this join segment that
		 * are from the rel2 side.
		 */
		child_relids2 = bms_intersect(child_joinrel->relids,
									  rel2->all_partrels);
		Assert(bms_num_members(child_relids2) == bms_num_members(rel2->relids));

		/*
		 * Get a child rel for rel2 with the relids.  See above comments.
		 */
		if (rel2_is_simple)
		{
			int			varno = bms_singleton_member(child_relids2);

			child_rel2 = find_base_rel(root, varno);
		}
		else
			child_rel2 = find_join_rel(root, child_relids2);
		Assert(child_rel2);

		/*
		 * The join of rel1 and rel2 is legal, so is the join of the child
		 * rels obtained above; add them to the given lists as a join pair
		 * producing this join segment.
		 */
		*parts1 = lappend(*parts1, child_rel1);
		*parts2 = lappend(*parts2, child_rel2);
	}
}
