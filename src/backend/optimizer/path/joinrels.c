/*-------------------------------------------------------------------------
 *
 * joinrels.c
 *	  Routines to determine which relations should be joined
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/path/joinrels.c,v 1.28 1999/02/18 04:45:36 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/pg_list.h"
#include "nodes/relation.h"

#include "optimizer/internal.h"
#include "optimizer/cost.h"
#include "optimizer/paths.h"
#include "optimizer/tlist.h"
#include "optimizer/joininfo.h"
#include "optimizer/pathnode.h"

#ifdef USE_RIGHT_SIDED_PLANS
bool		_use_right_sided_plans_ = true;

#else
bool		_use_right_sided_plans_ = false;

#endif

static List *new_joininfo_list(List *joininfo_list, Relids join_relids);
static bool nonoverlap_sets(List *s1, List *s2);
static bool is_subset(List *s1, List *s2);
static void set_joinrel_size(RelOptInfo *joinrel, RelOptInfo *outer_rel,
				RelOptInfo *inner_rel, JoinInfo *jinfo);

/*
 * make_rels_by_joins
 *	  Find all possible joins for each of the outer join relations in
 *	  'outer_rels'.  A rel node is created for each possible join relation,
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
	List	   *joined_rels = NIL;
	List	   *join_list = NIL;
	List	   *r = NIL;

	foreach(r, old_rels)
	{
		RelOptInfo *old_rel = (RelOptInfo *) lfirst(r);

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
			if (BushyPlanFlag)
				joined_rels = make_rels_by_clauseless_joins(old_rel,
															old_rels);
		}

		join_list = nconc(join_list, joined_rels);
	}

	return join_list;
}

/*
 * make_rels_by_clause_joins
 *	  Determines whether joins can be performed between an outer relation
 *	  'outer_rel' and those relations within 'outer_rel's joininfo nodes
 *	  (i.e., relations that participate in join clauses that 'outer_rel'
 *	  participates in).  This is possible if all but one of the relations
 *	  contained within the join clauses of the joininfo node are already
 *	  contained within 'outer_rel'.
 *
 * 'outer_rel' is the relation entry for the outer relation
 * 'joininfo_list' is a list of join clauses which 'outer_rel'
 *		participates in
 *
 * Returns a list of new join relations.
 */
List *
make_rels_by_clause_joins(Query *root, RelOptInfo *old_rel,
				 				List *joininfo_list, Relids only_relids)
{
	List	   *join_list = NIL;
	List	   *i = NIL;

	foreach(i, joininfo_list)
 	{
		JoinInfo   *joininfo = (JoinInfo *) lfirst(i);
		RelOptInfo *joined_rel;
		Relids		unjoined_relids = joininfo->unjoined_relids;

		if (unjoined_relids != NIL)
		{
			if (length(unjoined_relids) == 1 &&
				(only_relids == NIL ||
				/* geqo only wants certain relids to make new rels */
				 intMember(lfirsti(unjoined_relids), only_relids)))
			{
				joined_rel = make_join_rel(old_rel,
									get_base_rel(root,
												 lfirsti(unjoined_relids)),
									joininfo);
				join_list = lappend(join_list, joined_rel);

				/* Right-sided plan */
				if (_use_right_sided_plans_ &&
					length(old_rel->relids) > 1)
				{
					joined_rel = make_join_rel(
								get_base_rel(root, lfirsti(unjoined_relids)),
											old_rel,
											joininfo);
					join_list = lappend(join_list, joined_rel);
				}
			}

			if (BushyPlanFlag && only_relids == NIL) /* no bushy from geqo */
			{
				List *r;

				foreach(r, root->join_rel_list)
				{
					RelOptInfo *join_rel = lfirst(r);

					Assert(length(join_rel->relids) > 1);
					if (is_subset(unjoined_relids, join_rel->relids) &&
						nonoverlap_sets(old_rel->relids, join_rel->relids))
					{
						joined_rel = make_join_rel(old_rel,
													join_rel,
													joininfo);
						join_list = lappend(join_list, joined_rel);
					}
				}
			}
		}
	}

	return join_list;
}

/*
 * make_rels_by_clauseless_joins
 *	  Given an outer relation 'outer_rel' and a list of inner relations
 *	  'inner_rels', create a join relation between 'outer_rel' and each
 *	  member of 'inner_rels' that isn't already included in 'outer_rel'.
 *
 * Returns a list of new join relations.
 */
List *
make_rels_by_clauseless_joins(RelOptInfo *old_rel, List *inner_rels)
{
	RelOptInfo *inner_rel;
	List	   *t_list = NIL;
	List	   *i = NIL;

	foreach(i, inner_rels)
	{
		inner_rel = (RelOptInfo *) lfirst(i);
		if (nonoverlap_sets(inner_rel->relids, old_rel->relids))
		{
			t_list = lappend(t_list,
							 make_join_rel(old_rel,
										   inner_rel,
										   (JoinInfo *) NULL));
		}
	}

	return t_list;
}

/*
 * make_join_rel
 *	  Creates and initializes a new join relation.
 *
 * 'outer_rel' and 'inner_rel' are relation nodes for the relations to be
 *		joined
 * 'joininfo' is the joininfo node(join clause) containing both
 *		'outer_rel' and 'inner_rel', if any exists
 *
 * Returns the new join relation node.
 */
RelOptInfo *
make_join_rel(RelOptInfo *outer_rel, RelOptInfo *inner_rel, JoinInfo *joininfo)
{
	RelOptInfo *joinrel = makeNode(RelOptInfo);
	List	   *joinrel_joininfo_list = NIL;
	List	   *new_outer_tlist;
	List	   *new_inner_tlist;

	/*
	 * Create a new tlist by removing irrelevant elements from both tlists
	 * of the outer and inner join relations and then merging the results
	 * together.
	 */
	new_outer_tlist = new_join_tlist(outer_rel->targetlist,	1);
	new_inner_tlist = new_join_tlist(inner_rel->targetlist,
									 length(new_outer_tlist) + 1);

	joinrel->relids = NIL;
	joinrel->indexed = false;
	joinrel->pages = 0;
	joinrel->tuples = 0;
	joinrel->width = 0;
/*	joinrel->targetlist = NIL;*/
	joinrel->pathlist = NIL;
	joinrel->cheapestpath = (Path *) NULL;
	joinrel->pruneable = true;
	joinrel->classlist = NULL;
	joinrel->relam = InvalidOid;
	joinrel->ordering = NULL;
	joinrel->restrictinfo = NIL;
	joinrel->joininfo = NULL;
	joinrel->innerjoin = NIL;

	/*
	 * This function uses a trick to pass inner/outer rels as
	 * different lists, and then flattens it out later.  bjm
	 */
	joinrel->relids = lcons(outer_rel->relids, lcons(inner_rel->relids, NIL));

	new_outer_tlist = nconc(new_outer_tlist, new_inner_tlist);
	joinrel->targetlist = new_outer_tlist;

	if (joininfo)
		joinrel->restrictinfo = joininfo->jinfo_restrictinfo;

	joinrel_joininfo_list = new_joininfo_list(append(outer_rel->joininfo,
													 inner_rel->joininfo),
						intAppend(outer_rel->relids, inner_rel->relids));

	joinrel->joininfo = joinrel_joininfo_list;

	set_joinrel_size(joinrel, outer_rel, inner_rel, joininfo);

	return joinrel;
}

/*
 * new_join_tlist
 *	  Builds a join relations's target list by keeping those elements that
 *	  will be in the final target list and any other elements that are still
 *	  needed for future joins.	For a target list entry to still be needed
 *	  for future joins, its 'joinlist' field must not be empty after removal
 *	  of all relids in 'other_relids'.
 *
 * 'tlist' is the target list of one of the join relations
 * 'other_relids' is a list of relids contained within the other
 *				join relation
 * 'first_resdomno' is the resdom number to use for the first created
 *				target list entry
 *
 * Returns the new target list.
 */
List *
new_join_tlist(List *tlist,
			   int first_resdomno)
{
	int			resdomno = first_resdomno - 1;
	TargetEntry *xtl = NULL;
	List	   *t_list = NIL;
	List	   *i = NIL;
	List	   *join_list = NIL;
	bool		in_final_tlist = false;

	foreach(i, tlist)
	{
		xtl = lfirst(i);
		/* XXX surely this is wrong?  join_list is never changed?  tgl 2/99 */
		in_final_tlist = (join_list == NIL);
		if (in_final_tlist)
		{
			resdomno += 1;
			t_list = lappend(t_list,create_tl_element(get_expr(xtl), resdomno));
		}
	}

	return t_list;
}

/*
 * new_joininfo_list
 *	  Builds a join relation's joininfo list by checking for join clauses
 *	  which still need to used in future joins involving this relation.  A
 *	  join clause is still needed if there are still relations in the clause
 *	  not contained in the list of relations comprising this join relation.
 *	  New joininfo nodes are only created and added to
 *	  'current_joininfo_list' if a node for a particular join hasn't already
 *	  been created.
 *
 * 'current_joininfo_list' contains a list of those joininfo nodes that
 *		have already been built
 * 'joininfo_list' is the list of join clauses involving this relation
 * 'join_relids' is a list of relids corresponding to the relations
 *		currently being joined
 *
 * Returns a list of joininfo nodes, new and old.
 */
static List *
new_joininfo_list(List *joininfo_list, Relids join_relids)
{
	List	   *current_joininfo_list = NIL;
	Relids		new_unjoined_relids = NIL;
	JoinInfo   *other_joininfo = (JoinInfo *) NULL;
	List	   *xjoininfo = NIL;

	foreach(xjoininfo, joininfo_list)
	{
		List	   *or;
		JoinInfo   *joininfo = (JoinInfo *) lfirst(xjoininfo);

		new_unjoined_relids = joininfo->unjoined_relids;
		foreach(or, new_unjoined_relids)
		{
			if (intMember(lfirsti(or), join_relids))
				new_unjoined_relids = lremove((void *) lfirst(or), new_unjoined_relids);
		}
		joininfo->unjoined_relids = new_unjoined_relids;
		if (new_unjoined_relids != NIL)
		{
			other_joininfo = joininfo_member(new_unjoined_relids,
											 current_joininfo_list);
			if (other_joininfo)
			{
				other_joininfo->jinfo_restrictinfo = (List *)
									LispUnion(joininfo->jinfo_restrictinfo,
									other_joininfo->jinfo_restrictinfo);
			}
			else
			{
				other_joininfo = makeNode(JoinInfo);

				other_joininfo->unjoined_relids = joininfo->unjoined_relids;
				other_joininfo->jinfo_restrictinfo = joininfo->jinfo_restrictinfo;
				other_joininfo->mergejoinable = joininfo->mergejoinable;
				other_joininfo->hashjoinable = joininfo->hashjoinable;

				current_joininfo_list = lcons(other_joininfo,
											  current_joininfo_list);
			}
		}
	}

	return current_joininfo_list;
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
	List	   *xrel = NIL;
	RelOptInfo *final_rel = NULL;

	/*
	 * find the relations that have no further joins, i.e., its joininfos
	 * all have unjoined_relids nil.
	 */
	foreach(xrel, join_rel_list)
	{
		RelOptInfo *rel = (RelOptInfo *) lfirst(xrel);
		List	   *xjoininfo = NIL;
		bool		final = true;

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

static bool
nonoverlap_sets(List *s1, List *s2)
{
	List	   *x = NIL;

	foreach(x, s1)
	{
		int			e = lfirsti(x);

		if (intMember(e, s2))
			return false;
	}
	return true;
}

static bool
is_subset(List *s1, List *s2)
{
	List	   *x = NIL;

	foreach(x, s1)
	{
		int			e = lfirsti(x);

		if (!intMember(e, s2))
			return false;
	}
	return true;
}

static void
set_joinrel_size(RelOptInfo *joinrel, RelOptInfo *outer_rel, RelOptInfo *inner_rel, JoinInfo *jinfo)
{
	int			ntuples;
	float		selec;

	/*
	 * voodoo magic. but better than a size of 0. I have no idea why we
	 * didn't set the size before. -ay 2/95
	 */
	if (jinfo == NULL)
	{
		/* worst case: the cartesian product */
		ntuples = outer_rel->tuples * inner_rel->tuples;
	}
	else
	{
		selec = product_selec(jinfo->jinfo_restrictinfo);
/*		ntuples = Min(outer_rel->tuples,inner_rel->tuples) * selec; */
		ntuples = outer_rel->tuples * inner_rel->tuples * selec;
	}

	/*
	 * I bet sizes less than 1 will screw up optimization so make the best
	 * case 1 instead of 0	- jolly
	 */
	if (ntuples < 1)
		ntuples = 1;

	joinrel->tuples = ntuples;
}
