/*-------------------------------------------------------------------------
 *
 * joinrels.c
 *	  Routines to determine which relations should be joined
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/path/joinrels.c,v 1.40 2000/01/09 00:26:33 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#ifdef HAVE_LIMITS_H
#include <limits.h>
#ifndef MAXINT
#define MAXINT		  INT_MAX
#endif
#else
#ifdef HAVE_VALUES_H
#include <values.h>
#endif
#endif

#include "optimizer/cost.h"
#include "optimizer/joininfo.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/tlist.h"

static RelOptInfo *make_join_rel(RelOptInfo *outer_rel, RelOptInfo *inner_rel);
static List *new_join_tlist(List *tlist, int first_resdomno);
static void build_joinrel_restrict_and_join(RelOptInfo *joinrel,
											List *joininfo_list,
											Relids join_relids);

/*
 * make_rels_by_joins
 *	  Find all possible joins for each of the outer join relations in
 *	  'old_rels'.  A rel node is created for each possible join relation,
 *	  and the resulting list of nodes is returned.	If at all possible, only
 *	  those relations for which join clauses exist are considered.	If none
 *	  of these exist for a given relation, all remaining possibilities are
 *	  considered.
 *
 * Returns a list of rel nodes corresponding to the new join relations.
 */
List *
make_rels_by_joins(Query *root, List *old_rels)
{
	List	   *join_list = NIL;
	List	   *r;

	foreach(r, old_rels)
	{
		RelOptInfo *old_rel = (RelOptInfo *) lfirst(r);
		List	   *joined_rels;

		if (!(joined_rels = make_rels_by_clause_joins(root, old_rel,
													  old_rel->joininfo,
													  NIL)))
		{
			/*
			 * Oops, we have a relation that is not joined to any other
			 * relation.  Cartesian product time.
			 */
			joined_rels = make_rels_by_clauseless_joins(old_rel,
													root->base_rel_list);
			joined_rels = nconc(joined_rels,
								make_rels_by_clauseless_joins(old_rel,
															  old_rels));
		}

		join_list = nconc(join_list, joined_rels);
	}

	return join_list;
}

/*
 * make_rels_by_clause_joins
 *	  Build joins between an outer relation 'old_rel' and relations
 *	  within old_rel's joininfo nodes
 *	  (i.e., relations that participate in join clauses that 'old_rel'
 *	  also participates in).
 *
 * 'old_rel' is the relation entry for the outer relation
 * 'joininfo_list' is a list of join clauses which 'old_rel'
 *		participates in
 * 'only_relids': if not NIL, only joins against base rels mentioned in
 *		only_relids are allowable.
 *
 * Returns a list of new join relations.
 */
List *
make_rels_by_clause_joins(Query *root, RelOptInfo *old_rel,
						  List *joininfo_list, Relids only_relids)
{
	List	   *join_list = NIL;
	List	   *i;

	foreach(i, joininfo_list)
	{
		JoinInfo   *joininfo = (JoinInfo *) lfirst(i);
		Relids		unjoined_relids = joininfo->unjoined_relids;
		RelOptInfo *joined_rel;

		if (unjoined_relids == NIL)
			continue;			/* probably can't happen */

		if (length(unjoined_relids) == 1 &&
			(only_relids == NIL ||
			 /* geqo only wants certain relids to be joined to old_rel */
			 intMember(lfirsti(unjoined_relids), only_relids)))
		{
			RelOptInfo   *base_rel = get_base_rel(root,
												  lfirsti(unjoined_relids));

			/* Left-sided join of outer rel against a single base rel */
			joined_rel = make_join_rel(old_rel, base_rel);
			join_list = lappend(join_list, joined_rel);

			/* Consider right-sided plan as well */
			if (length(old_rel->relids) > 1)
			{
				joined_rel = make_join_rel(base_rel, old_rel);
				join_list = lappend(join_list, joined_rel);
			}
		}

		if (only_relids == NIL)	/* no bushy plans for geqo */
		{
			List	   *r;

			/* Build "bushy" plans: join old_rel against all pre-existing
			 * joins of rels it doesn't already contain, if there is a
			 * suitable join clause.
			 */
			foreach(r, root->join_rel_list)
			{
				RelOptInfo *join_rel = lfirst(r);

				Assert(length(join_rel->relids) > 1);
				if (is_subset(unjoined_relids, join_rel->relids) &&
					nonoverlap_sets(old_rel->relids, join_rel->relids))
				{
					joined_rel = make_join_rel(old_rel, join_rel);
					join_list = lappend(join_list, joined_rel);
				}
			}
		}
	}

	return join_list;
}

/*
 * make_rels_by_clauseless_joins
 *	  Given an outer relation 'old_rel' and a list of inner relations
 *	  'inner_rels', create a join relation between 'old_rel' and each
 *	  member of 'inner_rels' that isn't already included in 'old_rel'.
 *
 * Returns a list of new join relations.
 */
List *
make_rels_by_clauseless_joins(RelOptInfo *old_rel, List *inner_rels)
{
	List	   *join_list = NIL;
	List	   *i;

	foreach(i, inner_rels)
	{
		RelOptInfo *inner_rel = (RelOptInfo *) lfirst(i);

		if (nonoverlap_sets(inner_rel->relids, old_rel->relids))
		{
			join_list = lappend(join_list,
								make_join_rel(old_rel, inner_rel));
		}
	}

	return join_list;
}

/*
 * make_join_rel
 *	  Creates and initializes a new join relation.
 *
 * 'outer_rel' and 'inner_rel' are relation nodes for the relations to be
 *		joined
 *
 * Returns the new join relation node.
 */
static RelOptInfo *
make_join_rel(RelOptInfo *outer_rel, RelOptInfo *inner_rel)
{
	RelOptInfo *joinrel = makeNode(RelOptInfo);
	List	   *new_outer_tlist;
	List	   *new_inner_tlist;

	/*
	 * This function uses a trick to pass inner/outer rels as two sublists.
	 * The list will be flattened out in update_rels_pathlist_for_joins().
	 */
	joinrel->relids = lcons(outer_rel->relids, lcons(inner_rel->relids, NIL));
	joinrel->rows = 0;
	joinrel->width = 0;
	joinrel->targetlist = NIL;
	joinrel->pathlist = NIL;
	joinrel->cheapestpath = (Path *) NULL;
	joinrel->pruneable = true;
	joinrel->indexed = false;
	joinrel->pages = 0;
	joinrel->tuples = 0;
	joinrel->restrictinfo = NIL;
	joinrel->joininfo = NIL;
	joinrel->innerjoin = NIL;

	/*
	 * Create a new tlist by removing irrelevant elements from both tlists
	 * of the outer and inner join relations and then merging the results
	 * together.
	 */
	new_outer_tlist = new_join_tlist(outer_rel->targetlist, 1);
	new_inner_tlist = new_join_tlist(inner_rel->targetlist,
									 length(new_outer_tlist) + 1);
	joinrel->targetlist = nconc(new_outer_tlist, new_inner_tlist);

	/*
	 * Construct restrict and join clause lists for the new joinrel.
	 *
	 * nconc(listCopy(x), y) is an idiom for making a new list without
	 * changing either input list.
	 */
	build_joinrel_restrict_and_join(joinrel,
									nconc(listCopy(outer_rel->joininfo),
										  inner_rel->joininfo),
									nconc(listCopy(outer_rel->relids),
										  inner_rel->relids));

	return joinrel;
}

/*
 * new_join_tlist
 *	  Builds a join relation's target list by keeping those elements that
 *	  will be in the final target list and any other elements that are still
 *	  needed for future joins.	For a target list entry to still be needed
 *	  for future joins, its 'joinlist' field must not be empty after removal
 *	  of all relids in 'other_relids'.
 *
 *	  XXX this seems to be a dead test --- we don't keep track of joinlists
 *	  for individual targetlist entries anymore, if we ever did...
 *
 * 'tlist' is the target list of one of the join relations
 * 'other_relids' is a list of relids contained within the other
 *				join relation
 * 'first_resdomno' is the resdom number to use for the first created
 *				target list entry
 *
 * Returns the new target list.
 */
static List *
new_join_tlist(List *tlist,
			   int first_resdomno)
{
	int			resdomno = first_resdomno - 1;
	List	   *t_list = NIL;
	List	   *i;
	List	   *join_list = NIL;

	foreach(i, tlist)
	{
		TargetEntry *xtl = lfirst(i);
		bool		in_final_tlist;

		/*
		 * XXX surely this is wrong?  join_list is never changed?  tgl
		 * 2/99
		 */
		in_final_tlist = (join_list == NIL);
		if (in_final_tlist)
		{
			resdomno += 1;
			t_list = lappend(t_list,
							 create_tl_element(get_expr(xtl), resdomno));
		}
	}

	return t_list;
}

/*
 * build_joinrel_restrict_and_join
 *	  Builds a join relation's restrictinfo and joininfo lists from the
 *	  joininfo lists of the relations it joins.  If a join clause from an
 *	  input relation refers to base rels still not present in the joinrel,
 *	  then it is still a join clause for the joinrel; we put it into an
 *	  appropriate JoinInfo list for the joinrel.  Otherwise, the clause is
 *	  now a restrict clause for the joined relation, and we put it into
 *	  the joinrel's restrictinfo list.  (It will not need to be considered
 *	  further up the join tree.)
 *
 * 'joininfo_list' is a list of joininfo nodes from the relations being joined
 * 'join_relids' is a list of all base relids in the new join relation
 *
 * NB: Formerly, we made deep(!) copies of each input RestrictInfo to pass
 * up to the join relation.  I believe this is no longer necessary, because
 * RestrictInfo nodes are no longer context-dependent.  Instead, just add
 * the original nodes to the lists belonging to the join relation.
 */
static void
build_joinrel_restrict_and_join(RelOptInfo *joinrel,
								List *joininfo_list,
								Relids join_relids)
{
	List	   *xjoininfo;

	foreach(xjoininfo, joininfo_list)
	{
		JoinInfo   *joininfo = (JoinInfo *) lfirst(xjoininfo);
		Relids		new_unjoined_relids;

		new_unjoined_relids = set_differencei(joininfo->unjoined_relids,
											  join_relids);
		if (new_unjoined_relids == NIL)
		{
			/*
			 * Clauses in this JoinInfo list become restriction clauses
			 * for the joinrel, since they refer to no outside rels.
			 *
			 * Be careful to eliminate duplicates, since we will see the
			 * same clauses arriving from both input relations...
			 */
			joinrel->restrictinfo =
				LispUnion(joinrel->restrictinfo,
						  joininfo->jinfo_restrictinfo);
		}
		else
		{
			/*
			 * These clauses are still join clauses at this level,
			 * so find or make the appropriate JoinInfo item for the joinrel,
			 * and add the clauses to it (eliminating duplicates).
			 */
			JoinInfo   *new_joininfo;

			new_joininfo = find_joininfo_node(joinrel, new_unjoined_relids);
			new_joininfo->jinfo_restrictinfo =
				LispUnion(new_joininfo->jinfo_restrictinfo,
						  joininfo->jinfo_restrictinfo);
		}
	}
}

/*
 * get_cheapest_complete_rel
 *	   Find the join relation that includes all the original
 *	   relations, i.e. the final join result.
 *
 * 'join_rel_list' is a list of join relations.
 *
 * Returns the list of final join relations.
 */
RelOptInfo *
get_cheapest_complete_rel(List *join_rel_list)
{
	RelOptInfo *final_rel = NULL;
	List	   *xrel;

	/*
	 * find the relations that have no further joins, i.e., its joininfos
	 * all have unjoined_relids nil.  (Actually, a JoinInfo shouldn't
	 * ever have nil unjoined_relids, so I think this code is overly
	 * complex.  In fact it seems wrong; shouldn't we be looking for
	 * rels with complete relids lists???  Seems like a cartesian-product
	 * case could fail because sub-relations could have nil JoinInfo lists.
	 * Doesn't actually fail but I don't really understand why...)
	 */
	foreach(xrel, join_rel_list)
	{
		RelOptInfo *rel = (RelOptInfo *) lfirst(xrel);
		bool		final = true;
		List	   *xjoininfo;

		foreach(xjoininfo, rel->joininfo)
		{
			JoinInfo   *joininfo = (JoinInfo *) lfirst(xjoininfo);

			if (joininfo->unjoined_relids != NIL)
			{
				final = false;
				break;
			}
		}
		if (final)
			if (final_rel == NULL ||
			 path_is_cheaper(rel->cheapestpath, final_rel->cheapestpath))
				final_rel = rel;
	}

	return final_rel;
}

/*
 * Subset-inclusion tests on integer lists.
 *
 * XXX these probably ought to be in nodes/list.c or some such place.
 */

bool
nonoverlap_sets(List *s1, List *s2)
{
	List	   *x;

	foreach(x, s1)
	{
		int			e = lfirsti(x);

		if (intMember(e, s2))
			return false;
	}
	return true;
}

bool
is_subset(List *s1, List *s2)
{
	List	   *x;

	foreach(x, s1)
	{
		int			e = lfirsti(x);

		if (!intMember(e, s2))
			return false;
	}
	return true;
}
