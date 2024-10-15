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
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/analyzejoins.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "optimizer/joininfo.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/placeholder.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "utils/lsyscache.h"

/* local functions */
static bool join_is_removable(PlannerInfo *root, SpecialJoinInfo *sjinfo);
static void remove_rel_from_query(PlannerInfo *root, int relid,
								  SpecialJoinInfo *sjinfo);
static void remove_rel_from_restrictinfo(RestrictInfo *rinfo,
										 int relid, int ojrelid);
static void remove_rel_from_eclass(EquivalenceClass *ec,
								   int relid, int ojrelid);
static List *remove_rel_from_joinlist(List *joinlist, int relid, int *nremoved);
static bool rel_supports_distinctness(PlannerInfo *root, RelOptInfo *rel);
static bool rel_is_distinct_for(PlannerInfo *root, RelOptInfo *rel,
								List *clause_list);
static Oid	distinct_col_search(int colno, List *colnos, List *opids);
static bool is_innerrel_unique_for(PlannerInfo *root,
								   Relids joinrelids,
								   Relids outerrelids,
								   RelOptInfo *innerrel,
								   JoinType jointype,
								   List *restrictlist);


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

		remove_rel_from_query(root, innerrelid, sjinfo);

		/* We verify that exactly one reference gets removed from joinlist */
		nremoved = 0;
		joinlist = remove_rel_from_joinlist(joinlist, innerrelid, &nremoved);
		if (nremoved != 1)
			elog(ERROR, "failed to find relation %d in joinlist", innerrelid);

		/*
		 * We can delete this SpecialJoinInfo from the list too, since it's no
		 * longer of interest.  (Since we'll restart the foreach loop
		 * immediately, we don't bother with foreach_delete_current.)
		 */
		root->join_info_list = list_delete_cell(root->join_info_list, lc);

		/*
		 * Restart the scan.  This is necessary to ensure we find all
		 * removable joins independently of ordering of the join_info_list
		 * (note that removal of attr_needed bits may make a join appear
		 * removable that did not before).
		 */
		goto restart;
	}

	return joinlist;
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
	Relids		inputrelids;
	Relids		joinrelids;
	List	   *clause_list = NIL;
	ListCell   *l;
	int			attroff;

	/*
	 * Must be a left join to a single baserel, else we aren't going to be
	 * able to do anything with it.
	 */
	if (sjinfo->jointype != JOIN_LEFT)
		return false;

	if (!bms_get_singleton_member(sjinfo->min_righthand, &innerrelid))
		return false;

	/*
	 * Never try to eliminate a left join to the query result rel.  Although
	 * the case is syntactically impossible in standard SQL, MERGE will build
	 * a join tree that looks exactly like that.
	 */
	if (innerrelid == root->parse->resultRelation)
		return false;

	innerrel = find_base_rel(root, innerrelid);

	/*
	 * Before we go to the effort of checking whether any innerrel variables
	 * are needed above the join, make a quick check to eliminate cases in
	 * which we will surely be unable to prove uniqueness of the innerrel.
	 */
	if (!rel_supports_distinctness(root, innerrel))
		return false;

	/* Compute the relid set for the join we are considering */
	inputrelids = bms_union(sjinfo->min_lefthand, sjinfo->min_righthand);
	Assert(sjinfo->ojrelid != 0);
	joinrelids = bms_copy(inputrelids);
	joinrelids = bms_add_member(joinrelids, sjinfo->ojrelid);

	/*
	 * We can't remove the join if any inner-rel attributes are used above the
	 * join.  Here, "above" the join includes pushed-down conditions, so we
	 * should reject if attr_needed includes the OJ's own relid; therefore,
	 * compare to inputrelids not joinrelids.
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
		if (!bms_is_subset(innerrel->attr_needed[attroff], inputrelids))
			return false;
	}

	/*
	 * Similarly check that the inner rel isn't needed by any PlaceHolderVars
	 * that will be used above the join.  The PHV case is a little bit more
	 * complicated, because PHVs may have been assigned a ph_eval_at location
	 * that includes the innerrel, yet their contained expression might not
	 * actually reference the innerrel (it could be just a constant, for
	 * instance).  If such a PHV is due to be evaluated above the join then it
	 * needn't prevent join removal.
	 */
	foreach(l, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(l);

		if (bms_overlap(phinfo->ph_lateral, innerrel->relids))
			return false;		/* it references innerrel laterally */
		if (!bms_overlap(phinfo->ph_eval_at, innerrel->relids))
			continue;			/* it definitely doesn't reference innerrel */
		if (bms_is_subset(phinfo->ph_needed, inputrelids))
			continue;			/* PHV is not used above the join */
		if (!bms_is_member(sjinfo->ojrelid, phinfo->ph_eval_at))
			return false;		/* it has to be evaluated below the join */

		/*
		 * We need to be sure there will still be a place to evaluate the PHV
		 * if we remove the join, ie that ph_eval_at wouldn't become empty.
		 */
		if (!bms_overlap(sjinfo->min_lefthand, phinfo->ph_eval_at))
			return false;		/* there isn't any other place to eval PHV */
		/* Check contained expression last, since this is a bit expensive */
		if (bms_overlap(pull_varnos(root, (Node *) phinfo->ph_var->phexpr),
						innerrel->relids))
			return false;		/* contained expression references innerrel */
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
		 * If the current join commutes with some other outer join(s) via
		 * outer join identity 3, there will be multiple clones of its join
		 * clauses in the joininfo list.  We want to consider only the
		 * has_clone form of such clauses.  Processing more than one form
		 * would be wasteful, and also some of the others would confuse the
		 * RINFO_IS_PUSHED_DOWN test below.
		 */
		if (restrictinfo->is_clone)
			continue;			/* ignore it */

		/*
		 * If it's not a join clause for this outer join, we can't use it.
		 * Note that if the clause is pushed-down, then it is logically from
		 * above the outer join, even if it references no other rels (it might
		 * be from WHERE, for example).
		 */
		if (RINFO_IS_PUSHED_DOWN(restrictinfo, joinrelids))
			continue;			/* ignore; not useful here */

		/* Ignore if it's not a mergejoinable clause */
		if (!restrictinfo->can_join ||
			restrictinfo->mergeopfamilies == NIL)
			continue;			/* not mergejoinable */

		/*
		 * Check if the clause has the form "outer op inner" or "inner op
		 * outer", and if so mark which side is inner.
		 */
		if (!clause_sides_match_join(restrictinfo, sjinfo->min_lefthand,
									 innerrel->relids))
			continue;			/* no good for these input relations */

		/* OK, add to list */
		clause_list = lappend(clause_list, restrictinfo);
	}

	/*
	 * Now that we have the relevant equality join clauses, try to prove the
	 * innerrel distinct.
	 */
	if (rel_is_distinct_for(root, innerrel, clause_list))
		return true;

	/*
	 * Some day it would be nice to check for other methods of establishing
	 * distinctness.
	 */
	return false;
}


/*
 * Remove the target relid and references to the target join from the
 * planner's data structures, having determined that there is no need
 * to include them in the query.
 *
 * We are not terribly thorough here.  We only bother to update parts of
 * the planner's data structures that will actually be consulted later.
 */
static void
remove_rel_from_query(PlannerInfo *root, int relid, SpecialJoinInfo *sjinfo)
{
	RelOptInfo *rel = find_base_rel(root, relid);
	int			ojrelid = sjinfo->ojrelid;
	Relids		joinrelids;
	Relids		join_plus_commute;
	List	   *joininfos;
	Index		rti;
	ListCell   *l;

	/* Compute the relid set for the join we are considering */
	joinrelids = bms_union(sjinfo->min_lefthand, sjinfo->min_righthand);
	Assert(ojrelid != 0);
	joinrelids = bms_add_member(joinrelids, ojrelid);

	/*
	 * Update all_baserels and related relid sets.
	 */
	root->all_baserels = bms_del_member(root->all_baserels, relid);
	root->outer_join_rels = bms_del_member(root->outer_join_rels, ojrelid);
	root->all_query_rels = bms_del_member(root->all_query_rels, relid);
	root->all_query_rels = bms_del_member(root->all_query_rels, ojrelid);

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
		SpecialJoinInfo *sjinf = (SpecialJoinInfo *) lfirst(l);

		/*
		 * initsplan.c is fairly cavalier about allowing SpecialJoinInfos'
		 * lefthand/righthand relid sets to be shared with other data
		 * structures.  Ensure that we don't modify the original relid sets.
		 * (The commute_xxx sets are always per-SpecialJoinInfo though.)
		 */
		sjinf->min_lefthand = bms_copy(sjinf->min_lefthand);
		sjinf->min_righthand = bms_copy(sjinf->min_righthand);
		sjinf->syn_lefthand = bms_copy(sjinf->syn_lefthand);
		sjinf->syn_righthand = bms_copy(sjinf->syn_righthand);
		/* Now remove relid and ojrelid bits from the sets: */
		sjinf->min_lefthand = bms_del_member(sjinf->min_lefthand, relid);
		sjinf->min_righthand = bms_del_member(sjinf->min_righthand, relid);
		sjinf->syn_lefthand = bms_del_member(sjinf->syn_lefthand, relid);
		sjinf->syn_righthand = bms_del_member(sjinf->syn_righthand, relid);
		sjinf->min_lefthand = bms_del_member(sjinf->min_lefthand, ojrelid);
		sjinf->min_righthand = bms_del_member(sjinf->min_righthand, ojrelid);
		sjinf->syn_lefthand = bms_del_member(sjinf->syn_lefthand, ojrelid);
		sjinf->syn_righthand = bms_del_member(sjinf->syn_righthand, ojrelid);
		/* relid cannot appear in these fields, but ojrelid can: */
		sjinf->commute_above_l = bms_del_member(sjinf->commute_above_l, ojrelid);
		sjinf->commute_above_r = bms_del_member(sjinf->commute_above_r, ojrelid);
		sjinf->commute_below_l = bms_del_member(sjinf->commute_below_l, ojrelid);
		sjinf->commute_below_r = bms_del_member(sjinf->commute_below_r, ojrelid);
	}

	/*
	 * Likewise remove references from PlaceHolderVar data structures,
	 * removing any no-longer-needed placeholders entirely.
	 *
	 * Removal is a bit trickier than it might seem: we can remove PHVs that
	 * are used at the target rel and/or in the join qual, but not those that
	 * are used at join partner rels or above the join.  It's not that easy to
	 * distinguish PHVs used at partner rels from those used in the join qual,
	 * since they will both have ph_needed sets that are subsets of
	 * joinrelids.  However, a PHV used at a partner rel could not have the
	 * target rel in ph_eval_at, so we check that while deciding whether to
	 * remove or just update the PHV.  There is no corresponding test in
	 * join_is_removable because it doesn't need to distinguish those cases.
	 */
	foreach(l, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(l);

		Assert(!bms_is_member(relid, phinfo->ph_lateral));
		if (bms_is_subset(phinfo->ph_needed, joinrelids) &&
			bms_is_member(relid, phinfo->ph_eval_at) &&
			!bms_is_member(ojrelid, phinfo->ph_eval_at))
		{
			root->placeholder_list = foreach_delete_current(root->placeholder_list,
															l);
			root->placeholder_array[phinfo->phid] = NULL;
		}
		else
		{
			PlaceHolderVar *phv = phinfo->ph_var;

			phinfo->ph_eval_at = bms_del_member(phinfo->ph_eval_at, relid);
			phinfo->ph_eval_at = bms_del_member(phinfo->ph_eval_at, ojrelid);
			Assert(!bms_is_empty(phinfo->ph_eval_at));	/* checked previously */
			/* Reduce ph_needed to contain only "relation 0"; see below */
			if (bms_is_member(0, phinfo->ph_needed))
				phinfo->ph_needed = bms_make_singleton(0);
			else
				phinfo->ph_needed = NULL;
			phv->phrels = bms_del_member(phv->phrels, relid);
			phv->phrels = bms_del_member(phv->phrels, ojrelid);
			Assert(!bms_is_empty(phv->phrels));
			Assert(phv->phnullingrels == NULL); /* no need to adjust */
		}
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
	 * We might encounter a qual that is a clone of a deletable qual with some
	 * outer-join relids added (see deconstruct_distribute_oj_quals).  To
	 * ensure we get rid of such clones as well, add the relids of all OJs
	 * commutable with this one to the set we test against for
	 * pushed-down-ness.
	 */
	join_plus_commute = bms_union(joinrelids,
								  sjinfo->commute_above_r);
	join_plus_commute = bms_add_members(join_plus_commute,
										sjinfo->commute_below_l);

	/*
	 * We must make a copy of the rel's old joininfo list before starting the
	 * loop, because otherwise remove_join_clause_from_rels would destroy the
	 * list while we're scanning it.
	 */
	joininfos = list_copy(rel->joininfo);
	foreach(l, joininfos)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

		remove_join_clause_from_rels(root, rinfo, rinfo->required_relids);

		if (RINFO_IS_PUSHED_DOWN(rinfo, join_plus_commute))
		{
			/*
			 * There might be references to relid or ojrelid in the
			 * RestrictInfo's relid sets, as a consequence of PHVs having had
			 * ph_eval_at sets that include those.  We already checked above
			 * that any such PHV is safe (and updated its ph_eval_at), so we
			 * can just drop those references.
			 */
			remove_rel_from_restrictinfo(rinfo, relid, ojrelid);

			/*
			 * Cross-check that the clause itself does not reference the
			 * target rel or join.
			 */
#ifdef USE_ASSERT_CHECKING
			{
				Relids		clause_varnos = pull_varnos(root,
														(Node *) rinfo->clause);

				Assert(!bms_is_member(relid, clause_varnos));
				Assert(!bms_is_member(ojrelid, clause_varnos));
			}
#endif
			/* Now throw it back into the joininfo lists */
			distribute_restrictinfo_to_rels(root, rinfo);
		}
	}

	/*
	 * Likewise remove references from EquivalenceClasses.
	 */
	foreach(l, root->eq_classes)
	{
		EquivalenceClass *ec = (EquivalenceClass *) lfirst(l);

		if (bms_is_member(relid, ec->ec_relids) ||
			bms_is_member(ojrelid, ec->ec_relids))
			remove_rel_from_eclass(ec, relid, ojrelid);
	}

	/*
	 * There may be references to the rel in root->fkey_list, but if so,
	 * match_foreign_keys_to_quals() will get rid of them.
	 */

	/*
	 * Now remove the rel from the baserel array to prevent it from being
	 * referenced again.  (We can't do this earlier because
	 * remove_join_clause_from_rels will touch it.)
	 */
	root->simple_rel_array[relid] = NULL;

	/* And nuke the RelOptInfo, just in case there's another access path */
	pfree(rel);

	/*
	 * Finally, we must recompute per-Var attr_needed and per-PlaceHolderVar
	 * ph_needed relid sets.  These have to be known accurately, else we may
	 * fail to remove other now-removable outer joins.  And our removal of the
	 * join clause(s) for this outer join may mean that Vars that were
	 * formerly needed no longer are.  So we have to do this honestly by
	 * repeating the construction of those relid sets.  We can cheat to one
	 * small extent: we can avoid re-examining the targetlist and HAVING qual
	 * by preserving "relation 0" bits from the existing relid sets.  This is
	 * safe because we'd never remove such references.
	 *
	 * So, start by removing all other bits from attr_needed sets.  (We
	 * already did this above for ph_needed.)
	 */
	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *otherrel = root->simple_rel_array[rti];
		int			attroff;

		/* there may be empty slots corresponding to non-baserel RTEs */
		if (otherrel == NULL)
			continue;

		Assert(otherrel->relid == rti); /* sanity check on array */

		for (attroff = otherrel->max_attr - otherrel->min_attr;
			 attroff >= 0;
			 attroff--)
		{
			if (bms_is_member(0, otherrel->attr_needed[attroff]))
				otherrel->attr_needed[attroff] = bms_make_singleton(0);
			else
				otherrel->attr_needed[attroff] = NULL;
		}
	}

	/*
	 * Now repeat construction of attr_needed bits coming from all other
	 * sources.
	 */
	rebuild_placeholder_attr_needed(root);
	rebuild_joinclause_attr_needed(root);
	rebuild_eclass_attr_needed(root);
	rebuild_lateral_attr_needed(root);
}

/*
 * Remove any references to relid or ojrelid from the RestrictInfo.
 *
 * We only bother to clean out bits in clause_relids and required_relids,
 * not nullingrel bits in contained Vars and PHVs.  (This might have to be
 * improved sometime.)  However, if the RestrictInfo contains an OR clause
 * we have to also clean up the sub-clauses.
 */
static void
remove_rel_from_restrictinfo(RestrictInfo *rinfo, int relid, int ojrelid)
{
	/*
	 * initsplan.c is fairly cavalier about allowing RestrictInfos to share
	 * relid sets with other RestrictInfos, and SpecialJoinInfos too.  Make
	 * sure this RestrictInfo has its own relid sets before we modify them.
	 * (In present usage, clause_relids is probably not shared, but
	 * required_relids could be; let's not assume anything.)
	 */
	rinfo->clause_relids = bms_copy(rinfo->clause_relids);
	rinfo->clause_relids = bms_del_member(rinfo->clause_relids, relid);
	rinfo->clause_relids = bms_del_member(rinfo->clause_relids, ojrelid);
	/* Likewise for required_relids */
	rinfo->required_relids = bms_copy(rinfo->required_relids);
	rinfo->required_relids = bms_del_member(rinfo->required_relids, relid);
	rinfo->required_relids = bms_del_member(rinfo->required_relids, ojrelid);

	/* If it's an OR, recurse to clean up sub-clauses */
	if (restriction_is_or_clause(rinfo))
	{
		ListCell   *lc;

		Assert(is_orclause(rinfo->orclause));
		foreach(lc, ((BoolExpr *) rinfo->orclause)->args)
		{
			Node	   *orarg = (Node *) lfirst(lc);

			/* OR arguments should be ANDs or sub-RestrictInfos */
			if (is_andclause(orarg))
			{
				List	   *andargs = ((BoolExpr *) orarg)->args;
				ListCell   *lc2;

				foreach(lc2, andargs)
				{
					RestrictInfo *rinfo2 = lfirst_node(RestrictInfo, lc2);

					remove_rel_from_restrictinfo(rinfo2, relid, ojrelid);
				}
			}
			else
			{
				RestrictInfo *rinfo2 = castNode(RestrictInfo, orarg);

				remove_rel_from_restrictinfo(rinfo2, relid, ojrelid);
			}
		}
	}
}

/*
 * Remove any references to relid or ojrelid from the EquivalenceClass.
 *
 * Like remove_rel_from_restrictinfo, we don't worry about cleaning out
 * any nullingrel bits in contained Vars and PHVs.  (This might have to be
 * improved sometime.)  We do need to fix the EC and EM relid sets to ensure
 * that implied join equalities will be generated at the appropriate join
 * level(s).
 */
static void
remove_rel_from_eclass(EquivalenceClass *ec, int relid, int ojrelid)
{
	ListCell   *lc;

	/* Fix up the EC's overall relids */
	ec->ec_relids = bms_del_member(ec->ec_relids, relid);
	ec->ec_relids = bms_del_member(ec->ec_relids, ojrelid);

	/*
	 * Fix up the member expressions.  Any non-const member that ends with
	 * empty em_relids must be a Var or PHV of the removed relation.  We don't
	 * need it anymore, so we can drop it.
	 */
	foreach(lc, ec->ec_members)
	{
		EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc);

		if (bms_is_member(relid, cur_em->em_relids) ||
			bms_is_member(ojrelid, cur_em->em_relids))
		{
			Assert(!cur_em->em_is_const);
			cur_em->em_relids = bms_del_member(cur_em->em_relids, relid);
			cur_em->em_relids = bms_del_member(cur_em->em_relids, ojrelid);
			if (bms_is_empty(cur_em->em_relids))
				ec->ec_members = foreach_delete_current(ec->ec_members, lc);
		}
	}

	/* Fix up the source clauses, in case we can re-use them later */
	foreach(lc, ec->ec_sources)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		remove_rel_from_restrictinfo(rinfo, relid, ojrelid);
	}

	/*
	 * Rather than expend code on fixing up any already-derived clauses, just
	 * drop them.  (At this point, any such clauses would be base restriction
	 * clauses, which we'd not need anymore anyway.)
	 */
	ec->ec_derives = NIL;
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


/*
 * reduce_unique_semijoins
 *		Check for semijoins that can be simplified to plain inner joins
 *		because the inner relation is provably unique for the join clauses.
 *
 * Ideally this would happen during reduce_outer_joins, but we don't have
 * enough information at that point.
 *
 * To perform the strength reduction when applicable, we need only delete
 * the semijoin's SpecialJoinInfo from root->join_info_list.  (We don't
 * bother fixing the join type attributed to it in the query jointree,
 * since that won't be consulted again.)
 */
void
reduce_unique_semijoins(PlannerInfo *root)
{
	ListCell   *lc;

	/*
	 * Scan the join_info_list to find semijoins.
	 */
	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);
		int			innerrelid;
		RelOptInfo *innerrel;
		Relids		joinrelids;
		List	   *restrictlist;

		/*
		 * Must be a semijoin to a single baserel, else we aren't going to be
		 * able to do anything with it.
		 */
		if (sjinfo->jointype != JOIN_SEMI)
			continue;

		if (!bms_get_singleton_member(sjinfo->min_righthand, &innerrelid))
			continue;

		innerrel = find_base_rel(root, innerrelid);

		/*
		 * Before we trouble to run generate_join_implied_equalities, make a
		 * quick check to eliminate cases in which we will surely be unable to
		 * prove uniqueness of the innerrel.
		 */
		if (!rel_supports_distinctness(root, innerrel))
			continue;

		/* Compute the relid set for the join we are considering */
		joinrelids = bms_union(sjinfo->min_lefthand, sjinfo->min_righthand);
		Assert(sjinfo->ojrelid == 0);	/* SEMI joins don't have RT indexes */

		/*
		 * Since we're only considering a single-rel RHS, any join clauses it
		 * has must be clauses linking it to the semijoin's min_lefthand.  We
		 * can also consider EC-derived join clauses.
		 */
		restrictlist =
			list_concat(generate_join_implied_equalities(root,
														 joinrelids,
														 sjinfo->min_lefthand,
														 innerrel,
														 NULL),
						innerrel->joininfo);

		/* Test whether the innerrel is unique for those clauses. */
		if (!innerrel_is_unique(root,
								joinrelids, sjinfo->min_lefthand, innerrel,
								JOIN_SEMI, restrictlist, true))
			continue;

		/* OK, remove the SpecialJoinInfo from the list. */
		root->join_info_list = foreach_delete_current(root->join_info_list, lc);
	}
}


/*
 * rel_supports_distinctness
 *		Could the relation possibly be proven distinct on some set of columns?
 *
 * This is effectively a pre-checking function for rel_is_distinct_for().
 * It must return true if rel_is_distinct_for() could possibly return true
 * with this rel, but it should not expend a lot of cycles.  The idea is
 * that callers can avoid doing possibly-expensive processing to compute
 * rel_is_distinct_for()'s argument lists if the call could not possibly
 * succeed.
 */
static bool
rel_supports_distinctness(PlannerInfo *root, RelOptInfo *rel)
{
	/* We only know about baserels ... */
	if (rel->reloptkind != RELOPT_BASEREL)
		return false;
	if (rel->rtekind == RTE_RELATION)
	{
		/*
		 * For a plain relation, we only know how to prove uniqueness by
		 * reference to unique indexes.  Make sure there's at least one
		 * suitable unique index.  It must be immediately enforced, and not a
		 * partial index. (Keep these conditions in sync with
		 * relation_has_unique_index_for!)
		 */
		ListCell   *lc;

		foreach(lc, rel->indexlist)
		{
			IndexOptInfo *ind = (IndexOptInfo *) lfirst(lc);

			if (ind->unique && ind->immediate && ind->indpred == NIL)
				return true;
		}
	}
	else if (rel->rtekind == RTE_SUBQUERY)
	{
		Query	   *subquery = root->simple_rte_array[rel->relid]->subquery;

		/* Check if the subquery has any qualities that support distinctness */
		if (query_supports_distinctness(subquery))
			return true;
	}
	/* We have no proof rules for any other rtekinds. */
	return false;
}

/*
 * rel_is_distinct_for
 *		Does the relation return only distinct rows according to clause_list?
 *
 * clause_list is a list of join restriction clauses involving this rel and
 * some other one.  Return true if no two rows emitted by this rel could
 * possibly join to the same row of the other rel.
 *
 * The caller must have already determined that each condition is a
 * mergejoinable equality with an expression in this relation on one side, and
 * an expression not involving this relation on the other.  The transient
 * outer_is_left flag is used to identify which side references this relation:
 * left side if outer_is_left is false, right side if it is true.
 *
 * Note that the passed-in clause_list may be destructively modified!  This
 * is OK for current uses, because the clause_list is built by the caller for
 * the sole purpose of passing to this function.
 */
static bool
rel_is_distinct_for(PlannerInfo *root, RelOptInfo *rel, List *clause_list)
{
	/*
	 * We could skip a couple of tests here if we assume all callers checked
	 * rel_supports_distinctness first, but it doesn't seem worth taking any
	 * risk for.
	 */
	if (rel->reloptkind != RELOPT_BASEREL)
		return false;
	if (rel->rtekind == RTE_RELATION)
	{
		/*
		 * Examine the indexes to see if we have a matching unique index.
		 * relation_has_unique_index_for automatically adds any usable
		 * restriction clauses for the rel, so we needn't do that here.
		 */
		if (relation_has_unique_index_for(root, rel, clause_list, NIL, NIL))
			return true;
	}
	else if (rel->rtekind == RTE_SUBQUERY)
	{
		Index		relid = rel->relid;
		Query	   *subquery = root->simple_rte_array[relid]->subquery;
		List	   *colnos = NIL;
		List	   *opids = NIL;
		ListCell   *l;

		/*
		 * Build the argument lists for query_is_distinct_for: a list of
		 * output column numbers that the query needs to be distinct over, and
		 * a list of equality operators that the output columns need to be
		 * distinct according to.
		 *
		 * (XXX we are not considering restriction clauses attached to the
		 * subquery; is that worth doing?)
		 */
		foreach(l, clause_list)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);
			Oid			op;
			Var		   *var;

			/*
			 * Get the equality operator we need uniqueness according to.
			 * (This might be a cross-type operator and thus not exactly the
			 * same operator the subquery would consider; that's all right
			 * since query_is_distinct_for can resolve such cases.)  The
			 * caller's mergejoinability test should have selected only
			 * OpExprs.
			 */
			op = castNode(OpExpr, rinfo->clause)->opno;

			/* caller identified the inner side for us */
			if (rinfo->outer_is_left)
				var = (Var *) get_rightop(rinfo->clause);
			else
				var = (Var *) get_leftop(rinfo->clause);

			/*
			 * We may ignore any RelabelType node above the operand.  (There
			 * won't be more than one, since eval_const_expressions() has been
			 * applied already.)
			 */
			if (var && IsA(var, RelabelType))
				var = (Var *) ((RelabelType *) var)->arg;

			/*
			 * If inner side isn't a Var referencing a subquery output column,
			 * this clause doesn't help us.
			 */
			if (!var || !IsA(var, Var) ||
				var->varno != relid || var->varlevelsup != 0)
				continue;

			colnos = lappend_int(colnos, var->varattno);
			opids = lappend_oid(opids, op);
		}

		if (query_is_distinct_for(subquery, colnos, opids))
			return true;
	}
	return false;
}


/*
 * query_supports_distinctness - could the query possibly be proven distinct
 *		on some set of output columns?
 *
 * This is effectively a pre-checking function for query_is_distinct_for().
 * It must return true if query_is_distinct_for() could possibly return true
 * with this query, but it should not expend a lot of cycles.  The idea is
 * that callers can avoid doing possibly-expensive processing to compute
 * query_is_distinct_for()'s argument lists if the call could not possibly
 * succeed.
 */
bool
query_supports_distinctness(Query *query)
{
	/* SRFs break distinctness except with DISTINCT, see below */
	if (query->hasTargetSRFs && query->distinctClause == NIL)
		return false;

	/* check for features we can prove distinctness with */
	if (query->distinctClause != NIL ||
		query->groupClause != NIL ||
		query->groupingSets != NIL ||
		query->hasAggs ||
		query->havingQual ||
		query->setOperations)
		return true;

	return false;
}

/*
 * query_is_distinct_for - does query never return duplicates of the
 *		specified columns?
 *
 * query is a not-yet-planned subquery (in current usage, it's always from
 * a subquery RTE, which the planner avoids scribbling on).
 *
 * colnos is an integer list of output column numbers (resno's).  We are
 * interested in whether rows consisting of just these columns are certain
 * to be distinct.  "Distinctness" is defined according to whether the
 * corresponding upper-level equality operators listed in opids would think
 * the values are distinct.  (Note: the opids entries could be cross-type
 * operators, and thus not exactly the equality operators that the subquery
 * would use itself.  We use equality_ops_are_compatible() to check
 * compatibility.  That looks at btree or hash opfamily membership, and so
 * should give trustworthy answers for all operators that we might need
 * to deal with here.)
 */
bool
query_is_distinct_for(Query *query, List *colnos, List *opids)
{
	ListCell   *l;
	Oid			opid;

	Assert(list_length(colnos) == list_length(opids));

	/*
	 * DISTINCT (including DISTINCT ON) guarantees uniqueness if all the
	 * columns in the DISTINCT clause appear in colnos and operator semantics
	 * match.  This is true even if there are SRFs in the DISTINCT columns or
	 * elsewhere in the tlist.
	 */
	if (query->distinctClause)
	{
		foreach(l, query->distinctClause)
		{
			SortGroupClause *sgc = (SortGroupClause *) lfirst(l);
			TargetEntry *tle = get_sortgroupclause_tle(sgc,
													   query->targetList);

			opid = distinct_col_search(tle->resno, colnos, opids);
			if (!OidIsValid(opid) ||
				!equality_ops_are_compatible(opid, sgc->eqop))
				break;			/* exit early if no match */
		}
		if (l == NULL)			/* had matches for all? */
			return true;
	}

	/*
	 * Otherwise, a set-returning function in the query's targetlist can
	 * result in returning duplicate rows, despite any grouping that might
	 * occur before tlist evaluation.  (If all tlist SRFs are within GROUP BY
	 * columns, it would be safe because they'd be expanded before grouping.
	 * But it doesn't currently seem worth the effort to check for that.)
	 */
	if (query->hasTargetSRFs)
		return false;

	/*
	 * Similarly, GROUP BY without GROUPING SETS guarantees uniqueness if all
	 * the grouped columns appear in colnos and operator semantics match.
	 */
	if (query->groupClause && !query->groupingSets)
	{
		foreach(l, query->groupClause)
		{
			SortGroupClause *sgc = (SortGroupClause *) lfirst(l);
			TargetEntry *tle = get_sortgroupclause_tle(sgc,
													   query->targetList);

			opid = distinct_col_search(tle->resno, colnos, opids);
			if (!OidIsValid(opid) ||
				!equality_ops_are_compatible(opid, sgc->eqop))
				break;			/* exit early if no match */
		}
		if (l == NULL)			/* had matches for all? */
			return true;
	}
	else if (query->groupingSets)
	{
		/*
		 * If we have grouping sets with expressions, we probably don't have
		 * uniqueness and analysis would be hard. Punt.
		 */
		if (query->groupClause)
			return false;

		/*
		 * If we have no groupClause (therefore no grouping expressions), we
		 * might have one or many empty grouping sets. If there's just one,
		 * then we're returning only one row and are certainly unique. But
		 * otherwise, we know we're certainly not unique.
		 */
		if (list_length(query->groupingSets) == 1 &&
			((GroupingSet *) linitial(query->groupingSets))->kind == GROUPING_SET_EMPTY)
			return true;
		else
			return false;
	}
	else
	{
		/*
		 * If we have no GROUP BY, but do have aggregates or HAVING, then the
		 * result is at most one row so it's surely unique, for any operators.
		 */
		if (query->hasAggs || query->havingQual)
			return true;
	}

	/*
	 * UNION, INTERSECT, EXCEPT guarantee uniqueness of the whole output row,
	 * except with ALL.
	 */
	if (query->setOperations)
	{
		SetOperationStmt *topop = castNode(SetOperationStmt, query->setOperations);

		Assert(topop->op != SETOP_NONE);

		if (!topop->all)
		{
			ListCell   *lg;

			/* We're good if all the nonjunk output columns are in colnos */
			lg = list_head(topop->groupClauses);
			foreach(l, query->targetList)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(l);
				SortGroupClause *sgc;

				if (tle->resjunk)
					continue;	/* ignore resjunk columns */

				/* non-resjunk columns should have grouping clauses */
				Assert(lg != NULL);
				sgc = (SortGroupClause *) lfirst(lg);
				lg = lnext(topop->groupClauses, lg);

				opid = distinct_col_search(tle->resno, colnos, opids);
				if (!OidIsValid(opid) ||
					!equality_ops_are_compatible(opid, sgc->eqop))
					break;		/* exit early if no match */
			}
			if (l == NULL)		/* had matches for all? */
				return true;
		}
	}

	/*
	 * XXX Are there any other cases in which we can easily see the result
	 * must be distinct?
	 *
	 * If you do add more smarts to this function, be sure to update
	 * query_supports_distinctness() to match.
	 */

	return false;
}

/*
 * distinct_col_search - subroutine for query_is_distinct_for
 *
 * If colno is in colnos, return the corresponding element of opids,
 * else return InvalidOid.  (Ordinarily colnos would not contain duplicates,
 * but if it does, we arbitrarily select the first match.)
 */
static Oid
distinct_col_search(int colno, List *colnos, List *opids)
{
	ListCell   *lc1,
			   *lc2;

	forboth(lc1, colnos, lc2, opids)
	{
		if (colno == lfirst_int(lc1))
			return lfirst_oid(lc2);
	}
	return InvalidOid;
}


/*
 * innerrel_is_unique
 *	  Check if the innerrel provably contains at most one tuple matching any
 *	  tuple from the outerrel, based on join clauses in the 'restrictlist'.
 *
 * We need an actual RelOptInfo for the innerrel, but it's sufficient to
 * identify the outerrel by its Relids.  This asymmetry supports use of this
 * function before joinrels have been built.  (The caller is expected to
 * also supply the joinrelids, just to save recalculating that.)
 *
 * The proof must be made based only on clauses that will be "joinquals"
 * rather than "otherquals" at execution.  For an inner join there's no
 * difference; but if the join is outer, we must ignore pushed-down quals,
 * as those will become "otherquals".  Note that this means the answer might
 * vary depending on whether IS_OUTER_JOIN(jointype); since we cache the
 * answer without regard to that, callers must take care not to call this
 * with jointypes that would be classified differently by IS_OUTER_JOIN().
 *
 * The actual proof is undertaken by is_innerrel_unique_for(); this function
 * is a frontend that is mainly concerned with caching the answers.
 * In particular, the force_cache argument allows overriding the internal
 * heuristic about whether to cache negative answers; it should be "true"
 * if making an inquiry that is not part of the normal bottom-up join search
 * sequence.
 */
bool
innerrel_is_unique(PlannerInfo *root,
				   Relids joinrelids,
				   Relids outerrelids,
				   RelOptInfo *innerrel,
				   JoinType jointype,
				   List *restrictlist,
				   bool force_cache)
{
	MemoryContext old_context;
	ListCell   *lc;

	/* Certainly can't prove uniqueness when there are no joinclauses */
	if (restrictlist == NIL)
		return false;

	/*
	 * Make a quick check to eliminate cases in which we will surely be unable
	 * to prove uniqueness of the innerrel.
	 */
	if (!rel_supports_distinctness(root, innerrel))
		return false;

	/*
	 * Query the cache to see if we've managed to prove that innerrel is
	 * unique for any subset of this outerrel.  We don't need an exact match,
	 * as extra outerrels can't make the innerrel any less unique (or more
	 * formally, the restrictlist for a join to a superset outerrel must be a
	 * superset of the conditions we successfully used before).
	 */
	foreach(lc, innerrel->unique_for_rels)
	{
		Relids		unique_for_rels = (Relids) lfirst(lc);

		if (bms_is_subset(unique_for_rels, outerrelids))
			return true;		/* Success! */
	}

	/*
	 * Conversely, we may have already determined that this outerrel, or some
	 * superset thereof, cannot prove this innerrel to be unique.
	 */
	foreach(lc, innerrel->non_unique_for_rels)
	{
		Relids		unique_for_rels = (Relids) lfirst(lc);

		if (bms_is_subset(outerrelids, unique_for_rels))
			return false;
	}

	/* No cached information, so try to make the proof. */
	if (is_innerrel_unique_for(root, joinrelids, outerrelids, innerrel,
							   jointype, restrictlist))
	{
		/*
		 * Cache the positive result for future probes, being sure to keep it
		 * in the planner_cxt even if we are working in GEQO.
		 *
		 * Note: one might consider trying to isolate the minimal subset of
		 * the outerrels that proved the innerrel unique.  But it's not worth
		 * the trouble, because the planner builds up joinrels incrementally
		 * and so we'll see the minimally sufficient outerrels before any
		 * supersets of them anyway.
		 */
		old_context = MemoryContextSwitchTo(root->planner_cxt);
		innerrel->unique_for_rels = lappend(innerrel->unique_for_rels,
											bms_copy(outerrelids));
		MemoryContextSwitchTo(old_context);

		return true;			/* Success! */
	}
	else
	{
		/*
		 * None of the join conditions for outerrel proved innerrel unique, so
		 * we can safely reject this outerrel or any subset of it in future
		 * checks.
		 *
		 * However, in normal planning mode, caching this knowledge is totally
		 * pointless; it won't be queried again, because we build up joinrels
		 * from smaller to larger.  It is useful in GEQO mode, where the
		 * knowledge can be carried across successive planning attempts; and
		 * it's likely to be useful when using join-search plugins, too. Hence
		 * cache when join_search_private is non-NULL.  (Yeah, that's a hack,
		 * but it seems reasonable.)
		 *
		 * Also, allow callers to override that heuristic and force caching;
		 * that's useful for reduce_unique_semijoins, which calls here before
		 * the normal join search starts.
		 */
		if (force_cache || root->join_search_private)
		{
			old_context = MemoryContextSwitchTo(root->planner_cxt);
			innerrel->non_unique_for_rels =
				lappend(innerrel->non_unique_for_rels,
						bms_copy(outerrelids));
			MemoryContextSwitchTo(old_context);
		}

		return false;
	}
}

/*
 * is_innerrel_unique_for
 *	  Check if the innerrel provably contains at most one tuple matching any
 *	  tuple from the outerrel, based on join clauses in the 'restrictlist'.
 */
static bool
is_innerrel_unique_for(PlannerInfo *root,
					   Relids joinrelids,
					   Relids outerrelids,
					   RelOptInfo *innerrel,
					   JoinType jointype,
					   List *restrictlist)
{
	List	   *clause_list = NIL;
	ListCell   *lc;

	/*
	 * Search for mergejoinable clauses that constrain the inner rel against
	 * the outer rel.  If an operator is mergejoinable then it behaves like
	 * equality for some btree opclass, so it's what we want.  The
	 * mergejoinability test also eliminates clauses containing volatile
	 * functions, which we couldn't depend on.
	 */
	foreach(lc, restrictlist)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(lc);

		/*
		 * As noted above, if it's a pushed-down clause and we're at an outer
		 * join, we can't use it.
		 */
		if (IS_OUTER_JOIN(jointype) &&
			RINFO_IS_PUSHED_DOWN(restrictinfo, joinrelids))
			continue;

		/* Ignore if it's not a mergejoinable clause */
		if (!restrictinfo->can_join ||
			restrictinfo->mergeopfamilies == NIL)
			continue;			/* not mergejoinable */

		/*
		 * Check if clause has the form "outer op inner" or "inner op outer",
		 * and if so mark which side is inner.
		 */
		if (!clause_sides_match_join(restrictinfo, outerrelids,
									 innerrel->relids))
			continue;			/* no good for these input relations */

		/* OK, add to list */
		clause_list = lappend(clause_list, restrictinfo);
	}

	/* Let rel_is_distinct_for() do the hard work */
	return rel_is_distinct_for(root, innerrel, clause_list);
}
