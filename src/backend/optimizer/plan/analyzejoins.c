/*-------------------------------------------------------------------------
 *
 * analyzejoins.c
 *	  Routines for simplifying joins after initial query analysis
 *
 * While we do a great deal of join simplification in prep/prepjointree.c,
 * certain optimizations cannot be performed at that stage for lack of
 * detailed information about the query.  The routines here are invoked
 * after initsplan.c has done its work, and can do additional join removal
 * and simplification steps based on the information extracted.  The penalty
 * is that we have to work harder to clean up after ourselves when we modify
 * the query, since the derived data structures have to be updated too.
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/analyzejoins.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/joininfo.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/var.h"

/* local functions */
static bool join_is_removable(PlannerInfo *root, SpecialJoinInfo *sjinfo);
static void remove_rel_from_query(PlannerInfo *root, int relid,
					  Relids joinrelids);
static List *remove_rel_from_joinlist(List *joinlist, int relid, int *nremoved);


/*
 * remove_useless_joins
 *		Check for relations that don't actually need to be joined at all,
 *		and remove them from the query.
 *
 * We are passed the current joinlist and return the updated list.  Other
 * data structures that have to be updated are accessible via "root".
 */
List *
remove_useless_joins(PlannerInfo *root, List *joinlist)
{
	ListCell   *lc;

	/*
	 * We are only interested in relations that are left-joined to, so we can
	 * scan the join_info_list to find them easily.
	 */
restart:
	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);
		int			innerrelid;
		int			nremoved;

		/* Skip if not removable */
		if (!join_is_removable(root, sjinfo))
			continue;

		/*
		 * Currently, join_is_removable can only succeed when the sjinfo's
		 * righthand is a single baserel.  Remove that rel from the query and
		 * joinlist.
		 */
		innerrelid = bms_singleton_member(sjinfo->min_righthand);

		remove_rel_from_query(root, innerrelid,
							  bms_union(sjinfo->min_lefthand,
										sjinfo->min_righthand));

		/* We verify that exactly one reference gets removed from joinlist */
		nremoved = 0;
		joinlist = remove_rel_from_joinlist(joinlist, innerrelid, &nremoved);
		if (nremoved != 1)
			elog(ERROR, "failed to find relation %d in joinlist", innerrelid);

		/*
		 * We can delete this SpecialJoinInfo from the list too, since it's no
		 * longer of interest.
		 */
		root->join_info_list = list_delete_ptr(root->join_info_list, sjinfo);

		/*
		 * Restart the scan.  This is necessary to ensure we find all
		 * removable joins independently of ordering of the join_info_list
		 * (note that removal of attr_needed bits may make a join appear
		 * removable that did not before).  Also, since we just deleted the
		 * current list cell, we'd have to have some kluge to continue the
		 * list scan anyway.
		 */
		goto restart;
	}

	return joinlist;
}

/*
 * clause_sides_match_join
 *	  Determine whether a join clause is of the right form to use in this join.
 *
 * We already know that the clause is a binary opclause referencing only the
 * rels in the current join.  The point here is to check whether it has the
 * form "outerrel_expr op innerrel_expr" or "innerrel_expr op outerrel_expr",
 * rather than mixing outer and inner vars on either side.  If it matches,
 * we set the transient flag outer_is_left to identify which side is which.
 */
static inline bool
clause_sides_match_join(RestrictInfo *rinfo, Relids outerrelids,
						Relids innerrelids)
{
	if (bms_is_subset(rinfo->left_relids, outerrelids) &&
		bms_is_subset(rinfo->right_relids, innerrelids))
	{
		/* lefthand side is outer */
		rinfo->outer_is_left = true;
		return true;
	}
	else if (bms_is_subset(rinfo->left_relids, innerrelids) &&
			 bms_is_subset(rinfo->right_relids, outerrelids))
	{
		/* righthand side is outer */
		rinfo->outer_is_left = false;
		return true;
	}
	return false;				/* no good for these input relations */
}

/*
 * join_is_removable
 *	  Check whether we need not perform this special join at all, because
 *	  it will just duplicate its left input.
 *
 * This is true for a left join for which the join condition cannot match
 * more than one inner-side row.  (There are other possibly interesting
 * cases, but we don't have the infrastructure to prove them.)  We also
 * have to check that the inner side doesn't generate any variables needed
 * above the join.
 */
static bool
join_is_removable(PlannerInfo *root, SpecialJoinInfo *sjinfo)
{
	int			innerrelid;
	RelOptInfo *innerrel;
	Relids		joinrelids;
	List	   *clause_list = NIL;
	ListCell   *l;
	int			attroff;

	/*
	 * Currently, we only know how to remove left joins to a baserel with
	 * unique indexes.  We can check most of these criteria pretty trivially
	 * to avoid doing useless extra work.  But checking whether any of the
	 * indexes are unique would require iterating over the indexlist, so for
	 * now we just make sure there are indexes of some sort or other.  If none
	 * of them are unique, join removal will still fail, just slightly later.
	 */
	if (sjinfo->jointype != JOIN_LEFT ||
		sjinfo->delay_upper_joins ||
		bms_membership(sjinfo->min_righthand) != BMS_SINGLETON)
		return false;

	innerrelid = bms_singleton_member(sjinfo->min_righthand);
	innerrel = find_base_rel(root, innerrelid);

	if (innerrel->reloptkind != RELOPT_BASEREL ||
		innerrel->rtekind != RTE_RELATION ||
		innerrel->indexlist == NIL)
		return false;

	/* Compute the relid set for the join we are considering */
	joinrelids = bms_union(sjinfo->min_lefthand, sjinfo->min_righthand);

	/*
	 * We can't remove the join if any inner-rel attributes are used above the
	 * join.
	 *
	 * Note that this test only detects use of inner-rel attributes in higher
	 * join conditions and the target list.  There might be such attributes in
	 * pushed-down conditions at this join, too.  We check that case below.
	 *
	 * As a micro-optimization, it seems better to start with max_attr and
	 * count down rather than starting with min_attr and counting up, on the
	 * theory that the system attributes are somewhat less likely to be wanted
	 * and should be tested last.
	 */
	for (attroff = innerrel->max_attr - innerrel->min_attr;
		 attroff >= 0;
		 attroff--)
	{
		if (!bms_is_subset(innerrel->attr_needed[attroff], joinrelids))
			return false;
	}

	/*
	 * Similarly check that the inner rel isn't needed by any PlaceHolderVars
	 * that will be used above the join.  We only need to fail if such a PHV
	 * actually references some inner-rel attributes; but the correct check
	 * for that is relatively expensive, so we first check against ph_eval_at,
	 * which must mention the inner rel if the PHV uses any inner-rel attrs as
	 * non-lateral references.  Note that if the PHV's syntactic scope is just
	 * the inner rel, we can't drop the rel even if the PHV is variable-free.
	 */
	foreach(l, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(l);

		if (bms_is_subset(phinfo->ph_needed, joinrelids))
			continue;			/* PHV is not used above the join */
		if (bms_overlap(phinfo->ph_lateral, innerrel->relids))
			return false;		/* it references innerrel laterally */
		if (!bms_overlap(phinfo->ph_eval_at, innerrel->relids))
			continue;			/* it definitely doesn't reference innerrel */
		if (bms_is_subset(phinfo->ph_eval_at, innerrel->relids))
			return false;		/* there isn't any other place to eval PHV */
		if (bms_overlap(pull_varnos((Node *) phinfo->ph_var->phexpr),
						innerrel->relids))
			return false;		/* it does reference innerrel */
	}

	/*
	 * Search for mergejoinable clauses that constrain the inner rel against
	 * either the outer rel or a pseudoconstant.  If an operator is
	 * mergejoinable then it behaves like equality for some btree opclass, so
	 * it's what we want.  The mergejoinability test also eliminates clauses
	 * containing volatile functions, which we couldn't depend on.
	 */
	foreach(l, innerrel->joininfo)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(l);

		/*
		 * If it's not a join clause for this outer join, we can't use it.
		 * Note that if the clause is pushed-down, then it is logically from
		 * above the outer join, even if it references no other rels (it might
		 * be from WHERE, for example).
		 */
		if (restrictinfo->is_pushed_down ||
			!bms_equal(restrictinfo->required_relids, joinrelids))
		{
			/*
			 * If such a clause actually references the inner rel then join
			 * removal has to be disallowed.  We have to check this despite
			 * the previous attr_needed checks because of the possibility of
			 * pushed-down clauses referencing the rel.
			 */
			if (bms_is_member(innerrelid, restrictinfo->clause_relids))
				return false;
			continue;			/* else, ignore; not useful here */
		}

		/* Ignore if it's not a mergejoinable clause */
		if (!restrictinfo->can_join ||
			restrictinfo->mergeopfamilies == NIL)
			continue;			/* not mergejoinable */

		/*
		 * Check if clause has the form "outer op inner" or "inner op outer".
		 */
		if (!clause_sides_match_join(restrictinfo, sjinfo->min_lefthand,
									 innerrel->relids))
			continue;			/* no good for these input relations */

		/* OK, add to list */
		clause_list = lappend(clause_list, restrictinfo);
	}

	/*
	 * relation_has_unique_index_for automatically adds any usable restriction
	 * clauses for the innerrel, so we needn't do that here.
	 */

	/* Now examine the indexes to see if we have a matching unique index */
	if (relation_has_unique_index_for(root, innerrel, clause_list, NIL, NIL))
		return true;

	/*
	 * Some day it would be nice to check for other methods of establishing
	 * distinctness.
	 */
	return false;
}


/*
 * Remove the target relid from the planner's data structures, having
 * determined that there is no need to include it in the query.
 *
 * We are not terribly thorough here.  We must make sure that the rel is
 * no longer treated as a baserel, and that attributes of other baserels
 * are no longer marked as being needed at joins involving this rel.
 * Also, join quals involving the rel have to be removed from the joininfo
 * lists, but only if they belong to the outer join identified by joinrelids.
 */
static void
remove_rel_from_query(PlannerInfo *root, int relid, Relids joinrelids)
{
	RelOptInfo *rel = find_base_rel(root, relid);
	List	   *joininfos;
	Index		rti;
	ListCell   *l;
	ListCell   *nextl;

	/*
	 * Mark the rel as "dead" to show it is no longer part of the join tree.
	 * (Removing it from the baserel array altogether seems too risky.)
	 */
	rel->reloptkind = RELOPT_DEADREL;

	/*
	 * Remove references to the rel from other baserels' attr_needed arrays.
	 */
	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *otherrel = root->simple_rel_array[rti];
		int			attroff;

		/* there may be empty slots corresponding to non-baserel RTEs */
		if (otherrel == NULL)
			continue;

		Assert(otherrel->relid == rti); /* sanity check on array */

		/* no point in processing target rel itself */
		if (otherrel == rel)
			continue;

		for (attroff = otherrel->max_attr - otherrel->min_attr;
			 attroff >= 0;
			 attroff--)
		{
			otherrel->attr_needed[attroff] =
				bms_del_member(otherrel->attr_needed[attroff], relid);
		}
	}

	/*
	 * Likewise remove references from SpecialJoinInfo data structures.
	 *
	 * This is relevant in case the outer join we're deleting is nested inside
	 * other outer joins: the upper joins' relid sets have to be adjusted. The
	 * RHS of the target outer join will be made empty here, but that's OK
	 * since caller will delete that SpecialJoinInfo entirely.
	 */
	foreach(l, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(l);

		sjinfo->min_lefthand = bms_del_member(sjinfo->min_lefthand, relid);
		sjinfo->min_righthand = bms_del_member(sjinfo->min_righthand, relid);
		sjinfo->syn_lefthand = bms_del_member(sjinfo->syn_lefthand, relid);
		sjinfo->syn_righthand = bms_del_member(sjinfo->syn_righthand, relid);
	}

	/*
	 * Likewise remove references from LateralJoinInfo data structures.
	 *
	 * If we are deleting a LATERAL subquery, we can forget its
	 * LateralJoinInfos altogether.  Otherwise, make sure the target is not
	 * included in any lateral_lhs set.  (It probably can't be, since that
	 * should have precluded deciding to remove it; but let's cope anyway.)
	 */
	for (l = list_head(root->lateral_info_list); l != NULL; l = nextl)
	{
		LateralJoinInfo *ljinfo = (LateralJoinInfo *) lfirst(l);

		nextl = lnext(l);
		ljinfo->lateral_rhs = bms_del_member(ljinfo->lateral_rhs, relid);
		if (bms_is_empty(ljinfo->lateral_rhs))
			root->lateral_info_list = list_delete_ptr(root->lateral_info_list,
													  ljinfo);
		else
		{
			ljinfo->lateral_lhs = bms_del_member(ljinfo->lateral_lhs, relid);
			Assert(!bms_is_empty(ljinfo->lateral_lhs));
		}
	}

	/*
	 * Likewise remove references from PlaceHolderVar data structures.
	 */
	foreach(l, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(l);

		phinfo->ph_eval_at = bms_del_member(phinfo->ph_eval_at, relid);
		Assert(!bms_is_empty(phinfo->ph_eval_at));
		Assert(!bms_is_member(relid, phinfo->ph_lateral));
		phinfo->ph_needed = bms_del_member(phinfo->ph_needed, relid);
	}

	/*
	 * Remove any joinquals referencing the rel from the joininfo lists.
	 *
	 * In some cases, a joinqual has to be put back after deleting its
	 * reference to the target rel.  This can occur for pseudoconstant and
	 * outerjoin-delayed quals, which can get marked as requiring the rel in
	 * order to force them to be evaluated at or above the join.  We can't
	 * just discard them, though.  Only quals that logically belonged to the
	 * outer join being discarded should be removed from the query.
	 *
	 * We must make a copy of the rel's old joininfo list before starting the
	 * loop, because otherwise remove_join_clause_from_rels would destroy the
	 * list while we're scanning it.
	 */
	joininfos = list_copy(rel->joininfo);
	foreach(l, joininfos)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

		remove_join_clause_from_rels(root, rinfo, rinfo->required_relids);

		if (rinfo->is_pushed_down ||
			!bms_equal(rinfo->required_relids, joinrelids))
		{
			/* Recheck that qual doesn't actually reference the target rel */
			Assert(!bms_is_member(relid, rinfo->clause_relids));

			/*
			 * The required_relids probably aren't shared with anything else,
			 * but let's copy them just to be sure.
			 */
			rinfo->required_relids = bms_copy(rinfo->required_relids);
			rinfo->required_relids = bms_del_member(rinfo->required_relids,
													relid);
			distribute_restrictinfo_to_rels(root, rinfo);
		}
	}
}

/*
 * Remove any occurrences of the target relid from a joinlist structure.
 *
 * It's easiest to build a whole new list structure, so we handle it that
 * way.  Efficiency is not a big deal here.
 *
 * *nremoved is incremented by the number of occurrences removed (there
 * should be exactly one, but the caller checks that).
 */
static List *
remove_rel_from_joinlist(List *joinlist, int relid, int *nremoved)
{
	List	   *result = NIL;
	ListCell   *jl;

	foreach(jl, joinlist)
	{
		Node	   *jlnode = (Node *) lfirst(jl);

		if (IsA(jlnode, RangeTblRef))
		{
			int			varno = ((RangeTblRef *) jlnode)->rtindex;

			if (varno == relid)
				(*nremoved)++;
			else
				result = lappend(result, jlnode);
		}
		else if (IsA(jlnode, List))
		{
			/* Recurse to handle subproblem */
			List	   *sublist;

			sublist = remove_rel_from_joinlist((List *) jlnode,
											   relid, nremoved);
			/* Avoid including empty sub-lists in the result */
			if (sublist)
				result = lappend(result, sublist);
		}
		else
		{
			elog(ERROR, "unrecognized joinlist node type: %d",
				 (int) nodeTag(jlnode));
		}
	}

	return result;
}
