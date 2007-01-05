/*-------------------------------------------------------------------------
 *
 * joininfo.c
 *	  joininfo list manipulation routines
 *
 * Portions Copyright (c) 1996-2007, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/util/joininfo.c,v 1.46 2007/01/05 22:19:32 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/joininfo.h"
#include "optimizer/pathnode.h"


/*
 * have_relevant_joinclause
 *		Detect whether there is a joinclause that can be used to join
 *		the two given relations.
 */
bool
have_relevant_joinclause(PlannerInfo *root,
						 RelOptInfo *rel1, RelOptInfo *rel2)
{
	bool		result = false;
	Relids		join_relids;
	List	   *joininfo;
	ListCell   *l;

	join_relids = bms_union(rel1->relids, rel2->relids);

	/*
	 * We could scan either relation's joininfo list; may as well use the
	 * shorter one.
	 */
	if (list_length(rel1->joininfo) <= list_length(rel2->joininfo))
		joininfo = rel1->joininfo;
	else
		joininfo = rel2->joininfo;

	foreach(l, joininfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

		if (bms_is_subset(rinfo->required_relids, join_relids))
		{
			result = true;
			break;
		}
	}

	/*
	 * It's possible that the rels correspond to the left and right sides
	 * of a degenerate outer join, that is, one with no joinclause mentioning
	 * the non-nullable side.  The above scan will then have failed to locate
	 * any joinclause indicating we should join, but nonetheless we must
	 * allow the join to occur.
	 *
	 * Note: we need no comparable check for IN-joins because we can handle
	 * sequential buildup of an IN-join to multiple outer-side rels; therefore
	 * the "last ditch" case in make_rels_by_joins() always succeeds.  We
	 * could dispense with this hack if we were willing to try bushy plans
	 * in the "last ditch" case, but that seems too expensive.
	 */
	if (!result)
	{
		foreach(l, root->oj_info_list)
		{
			OuterJoinInfo *ojinfo = (OuterJoinInfo *) lfirst(l);

			/* ignore full joins --- other mechanisms handle them */
			if (ojinfo->is_full_join)
				continue;

			if ((bms_is_subset(ojinfo->min_lefthand, rel1->relids) &&
				 bms_is_subset(ojinfo->min_righthand, rel2->relids)) ||
				(bms_is_subset(ojinfo->min_lefthand, rel2->relids) &&
				 bms_is_subset(ojinfo->min_righthand, rel1->relids)))
			{
				result = true;
				break;
			}
		}
	}

	bms_free(join_relids);

	return result;
}


/*
 * add_join_clause_to_rels
 *	  Add 'restrictinfo' to the joininfo list of each relation it requires.
 *
 * Note that the same copy of the restrictinfo node is linked to by all the
 * lists it is in.	This allows us to exploit caching of information about
 * the restriction clause (but we must be careful that the information does
 * not depend on context).
 *
 * 'restrictinfo' describes the join clause
 * 'join_relids' is the list of relations participating in the join clause
 *				 (there must be more than one)
 */
void
add_join_clause_to_rels(PlannerInfo *root,
						RestrictInfo *restrictinfo,
						Relids join_relids)
{
	Relids		tmprelids;
	int			cur_relid;

	tmprelids = bms_copy(join_relids);
	while ((cur_relid = bms_first_member(tmprelids)) >= 0)
	{
		RelOptInfo *rel = find_base_rel(root, cur_relid);

		rel->joininfo = lappend(rel->joininfo, restrictinfo);
	}
	bms_free(tmprelids);
}

/*
 * remove_join_clause_from_rels
 *	  Delete 'restrictinfo' from all the joininfo lists it is in
 *
 * This reverses the effect of add_join_clause_to_rels.  It's used when we
 * discover that a join clause is redundant.
 *
 * 'restrictinfo' describes the join clause
 * 'join_relids' is the list of relations participating in the join clause
 *				 (there must be more than one)
 */
void
remove_join_clause_from_rels(PlannerInfo *root,
							 RestrictInfo *restrictinfo,
							 Relids join_relids)
{
	Relids		tmprelids;
	int			cur_relid;

	tmprelids = bms_copy(join_relids);
	while ((cur_relid = bms_first_member(tmprelids)) >= 0)
	{
		RelOptInfo *rel = find_base_rel(root, cur_relid);

		/*
		 * Remove the restrictinfo from the list.  Pointer comparison is
		 * sufficient.
		 */
		Assert(list_member_ptr(rel->joininfo, restrictinfo));
		rel->joininfo = list_delete_ptr(rel->joininfo, restrictinfo);
	}
	bms_free(tmprelids);
}
