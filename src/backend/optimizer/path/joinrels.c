/*-------------------------------------------------------------------------
 *
 * joinrels.c--
 *	  Routines to determine which relations should be joined
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/path/joinrels.c,v 1.7 1997/09/08 21:45:00 momjian Exp $
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

static List *find_clause_joins(Query *root, Rel *outer_rel, List *joininfo_list);
static List *find_clauseless_joins(Rel *outer_rel, List *inner_rels);
static Rel *init_join_rel(Rel *outer_rel, Rel *inner_rel, JInfo *joininfo);
static List *
new_join_tlist(List *tlist, List *other_relids,
			   int first_resdomno);
static List *new_joininfo_list(List *joininfo_list, List *join_relids);
static void add_superrels(Rel *rel, Rel *super_rel);
static bool nonoverlap_rels(Rel *rel1, Rel *rel2);
static bool nonoverlap_sets(List *s1, List *s2);
static void
set_joinrel_size(Rel *joinrel, Rel *outer_rel, Rel *inner_rel,
				 JInfo *jinfo);

/*
 * find-join-rels--
 *	  Find all possible joins for each of the outer join relations in
 *	  'outer-rels'.  A rel node is created for each possible join relation,
 *	  and the resulting list of nodes is returned.	If at all possible, only
 *	  those relations for which join clauses exist are considered.	If none
 *	  of these exist for a given relation, all remaining possibilities are
 *	  considered.
 *
 * 'outer-rels' is the list of rel nodes
 *
 * Returns a list of rel nodes corresponding to the new join relations.
 */
List	   *
find_join_rels(Query *root, List *outer_rels)
{
	List	   *joins = NIL;
	List	   *join_list = NIL;
	List	   *r = NIL;

	foreach(r, outer_rels)
	{
		Rel		   *outer_rel = (Rel *) lfirst(r);

		if (!(joins = find_clause_joins(root, outer_rel, outer_rel->joininfo)))
			if (BushyPlanFlag)
				joins = find_clauseless_joins(outer_rel, outer_rels);
			else
				joins = find_clauseless_joins(outer_rel, root->base_relation_list_);

		join_list = nconc(join_list, joins);
	}

	return (join_list);
}

/*
 * find-clause-joins--
 *	  Determines whether joins can be performed between an outer relation
 *	  'outer-rel' and those relations within 'outer-rel's joininfo nodes
 *	  (i.e., relations that participate in join clauses that 'outer-rel'
 *	  participates in).  This is possible if all but one of the relations
 *	  contained within the join clauses of the joininfo node are already
 *	  contained within 'outer-rel'.
 *
 * 'outer-rel' is the relation entry for the outer relation
 * 'joininfo-list' is a list of join clauses which 'outer-rel'
 *		participates in
 *
 * Returns a list of new join relations.
 */
static List *
find_clause_joins(Query *root, Rel *outer_rel, List *joininfo_list)
{
	List	   *join_list = NIL;
	List	   *i = NIL;

	foreach(i, joininfo_list)
	{
		JInfo	   *joininfo = (JInfo *) lfirst(i);
		Rel		   *rel;

		if (!joininfo->inactive)
		{
			List	   *other_rels = joininfo->otherrels;

			if (other_rels != NIL)
			{
				if (length(other_rels) == 1)
				{
					rel = init_join_rel(outer_rel,
								 get_base_rel(root, lfirsti(other_rels)),
										joininfo);
					/* how about right-sided plan ? */
					if (_use_right_sided_plans_ &&
						length(outer_rel->relids) > 1)
					{
						if (rel != NULL)
							join_list = lappend(join_list, rel);
						rel = init_join_rel(get_base_rel(root, lfirsti(other_rels)),
											outer_rel,
											joininfo);
					}
				}
				else if (BushyPlanFlag)
				{
					rel = init_join_rel(outer_rel,
										get_join_rel(root, other_rels),
										joininfo);
				}
				else
				{
					rel = NULL;
				}

				if (rel != NULL)
					join_list = lappend(join_list, rel);
			}
		}
	}

	return (join_list);
}

/*
 * find-clauseless-joins--
 *	  Given an outer relation 'outer-rel' and a list of inner relations
 *	  'inner-rels', create a join relation between 'outer-rel' and each
 *	  member of 'inner-rels' that isn't already included in 'outer-rel'.
 *
 * Returns a list of new join relations.
 */
static List *
find_clauseless_joins(Rel *outer_rel, List *inner_rels)
{
	Rel		   *inner_rel;
	List	   *t_list = NIL;
	List	   *temp_node = NIL;
	List	   *i = NIL;

	foreach(i, inner_rels)
	{
		inner_rel = (Rel *) lfirst(i);
		if (nonoverlap_rels(inner_rel, outer_rel))
		{
			temp_node = lcons(init_join_rel(outer_rel,
											inner_rel,
											(JInfo *) NULL),
							  NIL);
			t_list = nconc(t_list, temp_node);
		}
	}

	return (t_list);
}

/*
 * init-join-rel--
 *	  Creates and initializes a new join relation.
 *
 * 'outer-rel' and 'inner-rel' are relation nodes for the relations to be
 *		joined
 * 'joininfo' is the joininfo node(join clause) containing both
 *		'outer-rel' and 'inner-rel', if any exists
 *
 * Returns the new join relation node.
 */
static Rel *
init_join_rel(Rel *outer_rel, Rel *inner_rel, JInfo *joininfo)
{
	Rel		   *joinrel = makeNode(Rel);
	List	   *joinrel_joininfo_list = NIL;
	List	   *new_outer_tlist;
	List	   *new_inner_tlist;

	/*
	 * Create a new tlist by removing irrelevant elements from both tlists
	 * of the outer and inner join relations and then merging the results
	 * together.
	 */
	new_outer_tlist =
		new_join_tlist(outer_rel->targetlist,	/* XXX 1-based attnos */
					   inner_rel->relids, 1);
	new_inner_tlist =
		new_join_tlist(inner_rel->targetlist,	/* XXX 1-based attnos */
					   outer_rel->relids,
					   length(new_outer_tlist) + 1);

	joinrel->relids = NIL;
	joinrel->indexed = false;
	joinrel->pages = 0;
	joinrel->tuples = 0;
	joinrel->width = 0;
/*	  joinrel->targetlist = NIL;*/
	joinrel->pathlist = NIL;
	joinrel->unorderedpath = (Path *) NULL;
	joinrel->cheapestpath = (Path *) NULL;
	joinrel->pruneable = true;
	joinrel->classlist = NULL;
	joinrel->relam = InvalidOid;
	joinrel->ordering = NULL;
	joinrel->clauseinfo = NIL;
	joinrel->joininfo = NULL;
	joinrel->innerjoin = NIL;
	joinrel->superrels = NIL;

	joinrel->relids = lcons(outer_rel->relids,	/* ??? aren't they lists?
												 * -ay */
							lcons(inner_rel->relids, NIL));

	new_outer_tlist = nconc(new_outer_tlist, new_inner_tlist);
	joinrel->targetlist = new_outer_tlist;

	if (joininfo)
	{
		joinrel->clauseinfo = joininfo->jinfoclauseinfo;
		if (BushyPlanFlag)
			joininfo->inactive = true;
	}

	joinrel_joininfo_list =
		new_joininfo_list(append(outer_rel->joininfo, inner_rel->joininfo),
						intAppend(outer_rel->relids, inner_rel->relids));

	joinrel->joininfo = joinrel_joininfo_list;

	set_joinrel_size(joinrel, outer_rel, inner_rel, joininfo);

	return (joinrel);
}

/*
 * new-join-tlist--
 *	  Builds a join relations's target list by keeping those elements that
 *	  will be in the final target list and any other elements that are still
 *	  needed for future joins.	For a target list entry to still be needed
 *	  for future joins, its 'joinlist' field must not be empty after removal
 *	  of all relids in 'other-relids'.
 *
 * 'tlist' is the target list of one of the join relations
 * 'other-relids' is a list of relids contained within the other
 *				join relation
 * 'first-resdomno' is the resdom number to use for the first created
 *				target list entry
 *
 * Returns the new target list.
 */
static List *
new_join_tlist(List *tlist,
			   List *other_relids,
			   int first_resdomno)
{
	int			resdomno = first_resdomno - 1;
	TargetEntry *xtl = NULL;
	List	   *temp_node = NIL;
	List	   *t_list = NIL;
	List	   *i = NIL;
	List	   *join_list = NIL;
	bool		in_final_tlist = false;


	foreach(i, tlist)
	{
		xtl = lfirst(i);
		in_final_tlist = (join_list == NIL);
		if (in_final_tlist)
		{
			resdomno += 1;
			temp_node =
				lcons(create_tl_element(get_expr(xtl),
										resdomno),
					  NIL);
			t_list = nconc(t_list, temp_node);
		}
	}

	return (t_list);
}

/*
 * new-joininfo-list--
 *	  Builds a join relation's joininfo list by checking for join clauses
 *	  which still need to used in future joins involving this relation.  A
 *	  join clause is still needed if there are still relations in the clause
 *	  not contained in the list of relations comprising this join relation.
 *	  New joininfo nodes are only created and added to
 *	  'current-joininfo-list' if a node for a particular join hasn't already
 *	  been created.
 *
 * 'current-joininfo-list' contains a list of those joininfo nodes that
 *		have already been built
 * 'joininfo-list' is the list of join clauses involving this relation
 * 'join-relids' is a list of relids corresponding to the relations
 *		currently being joined
 *
 * Returns a list of joininfo nodes, new and old.
 */
static List *
new_joininfo_list(List *joininfo_list, List *join_relids)
{
	List	   *current_joininfo_list = NIL;
	List	   *new_otherrels = NIL;
	JInfo	   *other_joininfo = (JInfo *) NULL;
	List	   *xjoininfo = NIL;

	foreach(xjoininfo, joininfo_list)
	{
		List	   *or;
		JInfo	   *joininfo = (JInfo *) lfirst(xjoininfo);

		new_otherrels = joininfo->otherrels;
		foreach(or, new_otherrels)
		{
			if (intMember(lfirsti(or), join_relids))
				new_otherrels = lremove((void *) lfirst(or), new_otherrels);
		}
		joininfo->otherrels = new_otherrels;
		if (new_otherrels != NIL)
		{
			other_joininfo = joininfo_member(new_otherrels,
											 current_joininfo_list);
			if (other_joininfo)
			{
				other_joininfo->jinfoclauseinfo =
					(List *) LispUnion(joininfo->jinfoclauseinfo,
									   other_joininfo->jinfoclauseinfo);
			}
			else
			{
				other_joininfo = makeNode(JInfo);

				other_joininfo->otherrels =
					joininfo->otherrels;
				other_joininfo->jinfoclauseinfo =
					joininfo->jinfoclauseinfo;
				other_joininfo->mergesortable =
					joininfo->mergesortable;
				other_joininfo->hashjoinable =
					joininfo->hashjoinable;
				other_joininfo->inactive = false;

				current_joininfo_list = lcons(other_joininfo,
											  current_joininfo_list);
			}
		}
	}

	return (current_joininfo_list);
}

/*
 * add-new-joininfos--
 *	  For each new join relation, create new joininfos that
 *	  use the join relation as inner relation, and add
 *	  the new joininfos to those rel nodes that still
 *	  have joins with the join relation.
 *
 * 'joinrels' is a list of join relations.
 *
 * Modifies the joininfo field of appropriate rel nodes.
 */
void
add_new_joininfos(Query *root, List *joinrels, List *outerrels)
{
	List	   *xjoinrel = NIL;
	List	   *xrelid = NIL;
	List	   *xrel = NIL;
	List	   *xjoininfo = NIL;

	foreach(xjoinrel, joinrels)
	{
		Rel		   *joinrel = (Rel *) lfirst(xjoinrel);

		foreach(xrelid, joinrel->relids)
		{
			Relid		relid = (Relid) lfirst(xrelid);
			Rel		   *rel = get_join_rel(root, relid);

			add_superrels(rel, joinrel);
		}
	}
	foreach(xjoinrel, joinrels)
	{
		Rel		   *joinrel = (Rel *) lfirst(xjoinrel);

		foreach(xjoininfo, joinrel->joininfo)
		{
			JInfo	   *joininfo = (JInfo *) lfirst(xjoininfo);
			List	   *other_rels = joininfo->otherrels;
			List	   *clause_info = joininfo->jinfoclauseinfo;
			bool		mergesortable = joininfo->mergesortable;
			bool		hashjoinable = joininfo->hashjoinable;

			foreach(xrelid, other_rels)
			{
				Relid		relid = (Relid) lfirst(xrelid);
				Rel		   *rel = get_join_rel(root, relid);
				List	   *super_rels = rel->superrels;
				List	   *xsuper_rel = NIL;
				JInfo	   *new_joininfo = makeNode(JInfo);

				new_joininfo->otherrels = joinrel->relids;
				new_joininfo->jinfoclauseinfo = clause_info;
				new_joininfo->mergesortable = mergesortable;
				new_joininfo->hashjoinable = hashjoinable;
				new_joininfo->inactive = false;
				rel->joininfo =
					lappend(rel->joininfo, new_joininfo);

				foreach(xsuper_rel, super_rels)
				{
					Rel		   *super_rel = (Rel *) lfirst(xsuper_rel);

					if (nonoverlap_rels(super_rel, joinrel))
					{
						List	   *new_relids = super_rel->relids;
						JInfo	   *other_joininfo =
						joininfo_member(new_relids,
										joinrel->joininfo);

						if (other_joininfo)
						{
							other_joininfo->jinfoclauseinfo =
								(List *) LispUnion(clause_info,
										other_joininfo->jinfoclauseinfo);
						}
						else
						{
							JInfo	   *new_joininfo = makeNode(JInfo);

							new_joininfo->otherrels = new_relids;
							new_joininfo->jinfoclauseinfo = clause_info;
							new_joininfo->mergesortable = mergesortable;
							new_joininfo->hashjoinable = hashjoinable;
							new_joininfo->inactive = false;
							joinrel->joininfo =
								lappend(joinrel->joininfo,
										new_joininfo);
						}
					}
				}
			}
		}
	}
	foreach(xrel, outerrels)
	{
		Rel		   *rel = (Rel *) lfirst(xrel);

		rel->superrels = NIL;
	}
}

/*
 * final-join-rels--
 *	   Find the join relation that includes all the original
 *	   relations, i.e. the final join result.
 *
 * 'join-rel-list' is a list of join relations.
 *
 * Returns the list of final join relations.
 */
List	   *
final_join_rels(List *join_rel_list)
{
	List	   *xrel = NIL;
	List	   *temp = NIL;
	List	   *t_list = NIL;

	/*
	 * find the relations that has no further joins, i.e., its joininfos
	 * all have otherrels nil.
	 */
	foreach(xrel, join_rel_list)
	{
		Rel		   *rel = (Rel *) lfirst(xrel);
		List	   *xjoininfo = NIL;
		bool		final = true;

		foreach(xjoininfo, rel->joininfo)
		{
			JInfo	   *joininfo = (JInfo *) lfirst(xjoininfo);

			if (joininfo->otherrels != NIL)
			{
				final = false;
				break;
			}
		}
		if (final)
		{
			temp = lcons(rel, NIL);
			t_list = nconc(t_list, temp);
		}
	}

	return (t_list);
}

/*
 * add_superrels--
 *	  add rel to the temporary property list superrels.
 *
 * 'rel' a rel node
 * 'super-rel' rel node of a join relation that includes rel
 *
 * Modifies the superrels field of rel
 */
static void
add_superrels(Rel *rel, Rel *super_rel)
{
	rel->superrels = lappend(rel->superrels, super_rel);
}

/*
 * nonoverlap-rels--
 *	  test if two join relations overlap, i.e., includes the same
 *	  relation.
 *
 * 'rel1' and 'rel2' are two join relations
 *
 * Returns non-nil if rel1 and rel2 do not overlap.
 */
static bool
nonoverlap_rels(Rel *rel1, Rel *rel2)
{
	return (nonoverlap_sets(rel1->relids, rel2->relids));
}

static bool
nonoverlap_sets(List *s1, List *s2)
{
	List	   *x = NIL;

	foreach(x, s1)
	{
		int			e = lfirsti(x);

		if (intMember(e, s2))
			return (false);
	}
	return (true);
}

static void
set_joinrel_size(Rel *joinrel, Rel *outer_rel, Rel *inner_rel, JInfo *jinfo)
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
		selec = product_selec(jinfo->jinfoclauseinfo);
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
