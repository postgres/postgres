/*-------------------------------------------------------------------------
 *
 * joininfo.c
 *	  joininfo list manipulation routines
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/joininfo.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/joininfo.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"


/*
 * have_relevant_joinclause
 *		Detect whether there is a joinclause that involves
 *		the two given relations.
 *
 * Note: the joinclause does not have to be evaluatable with only these two
 * relations.  This is intentional.  For example consider
 *		SELECT * FROM a, b, c WHERE a.x = (b.y + c.z)
 * If a is much larger than the other tables, it may be worthwhile to
 * cross-join b and c and then use an inner indexscan on a.x.  Therefore
 * we should consider this joinclause as reason to join b to c, even though
 * it can't be applied at that join step.
 */
bool
have_relevant_joinclause(PlannerInfo *root,
						 RelOptInfo *rel1, RelOptInfo *rel2)
{
	bool		result = false;
	List	   *joininfo;
	Relids		other_relids;
	ListCell   *l;

	/*
	 * We could scan either relation's joininfo list; may as well use the
	 * shorter one.
	 */
	if (list_length(rel1->joininfo) <= list_length(rel2->joininfo))
	{
		joininfo = rel1->joininfo;
		other_relids = rel2->relids;
	}
	else
	{
		joininfo = rel2->joininfo;
		other_relids = rel1->relids;
	}

	foreach(l, joininfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

		if (bms_overlap(other_relids, rinfo->required_relids))
		{
			result = true;
			break;
		}
	}

	/*
	 * We also need to check the EquivalenceClass data structure, which might
	 * contain relationships not emitted into the joininfo lists.
	 */
	if (!result && rel1->has_eclass_joins && rel2->has_eclass_joins)
		result = have_relevant_eclass_joinclause(root, rel1, rel2);

	return result;
}


/*
 * add_join_clause_to_rels
 *	  Add 'restrictinfo' to the joininfo list of each relation it requires.
 *
 * Note that the same copy of the restrictinfo node is linked to by all the
 * lists it is in.  This allows us to exploit caching of information about
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
 * discover that a relation need not be joined at all.
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
