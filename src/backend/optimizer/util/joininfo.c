/*-------------------------------------------------------------------------
 *
 * joininfo.c
 *	  JoinInfo node manipulation routines
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/joininfo.c,v 1.36 2003/08/04 02:40:01 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/joininfo.h"
#include "optimizer/pathnode.h"


/*
 * find_joininfo_node
 *	  Find the joininfo node within a relation entry corresponding
 *	  to a join between 'this_rel' and the relations in 'join_relids'.
 *	  If there is no such node, return NULL.
 *
 * Returns a joininfo node, or NULL.
 */
JoinInfo *
find_joininfo_node(RelOptInfo *this_rel, Relids join_relids)
{
	List	   *i;

	foreach(i, this_rel->joininfo)
	{
		JoinInfo   *joininfo = (JoinInfo *) lfirst(i);

		if (bms_equal(join_relids, joininfo->unjoined_relids))
			return joininfo;
	}
	return NULL;
}

/*
 * make_joininfo_node
 *	  Find the joininfo node within a relation entry corresponding
 *	  to a join between 'this_rel' and the relations in 'join_relids'.
 *	  A new node is created and added to the relation entry's joininfo
 *	  field if the desired one can't be found.
 *
 * Returns a joininfo node.
 */
JoinInfo *
make_joininfo_node(RelOptInfo *this_rel, Relids join_relids)
{
	JoinInfo   *joininfo = find_joininfo_node(this_rel, join_relids);

	if (joininfo == NULL)
	{
		joininfo = makeNode(JoinInfo);
		joininfo->unjoined_relids = join_relids;
		joininfo->jinfo_restrictinfo = NIL;
		this_rel->joininfo = lcons(joininfo, this_rel->joininfo);
	}
	return joininfo;
}


/*
 * add_join_clause_to_rels
 *	  For every relation participating in a join clause, add 'restrictinfo' to
 *	  the appropriate joininfo list (creating a new list and adding it to the
 *	  appropriate rel node if necessary).
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
add_join_clause_to_rels(Query *root,
						RestrictInfo *restrictinfo,
						Relids join_relids)
{
	Relids		tmprelids;
	int			cur_relid;

	/* For every relid, find the joininfo, and add the proper join entries */
	tmprelids = bms_copy(join_relids);
	while ((cur_relid = bms_first_member(tmprelids)) >= 0)
	{
		Relids		unjoined_relids;
		JoinInfo   *joininfo;

		/* Get the relids not equal to the current relid */
		unjoined_relids = bms_copy(join_relids);
		unjoined_relids = bms_del_member(unjoined_relids, cur_relid);
		Assert(!bms_is_empty(unjoined_relids));

		/*
		 * Find or make the joininfo node for this combination of rels,
		 * and add the restrictinfo node to it.
		 */
		joininfo = make_joininfo_node(find_base_rel(root, cur_relid),
									  unjoined_relids);
		joininfo->jinfo_restrictinfo = lappend(joininfo->jinfo_restrictinfo,
											   restrictinfo);

		/*
		 * Can't bms_free(unjoined_relids) because new joininfo node may
		 * link to it.	We could avoid leaking memory by doing bms_copy()
		 * in make_joininfo_node, but for now speed seems better.
		 */
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
remove_join_clause_from_rels(Query *root,
							 RestrictInfo *restrictinfo,
							 Relids join_relids)
{
	Relids		tmprelids;
	int			cur_relid;

	/* For every relid, find the joininfo */
	tmprelids = bms_copy(join_relids);
	while ((cur_relid = bms_first_member(tmprelids)) >= 0)
	{
		Relids		unjoined_relids;
		JoinInfo   *joininfo;

		/* Get the relids not equal to the current relid */
		unjoined_relids = bms_copy(join_relids);
		unjoined_relids = bms_del_member(unjoined_relids, cur_relid);
		Assert(!bms_is_empty(unjoined_relids));

		/*
		 * Find the joininfo node for this combination of rels; it should
		 * exist already, if add_join_clause_to_rels was called.
		 */
		joininfo = find_joininfo_node(find_base_rel(root, cur_relid),
									  unjoined_relids);
		Assert(joininfo);

		/*
		 * Remove the restrictinfo from the list.  Pointer comparison is
		 * sufficient.
		 */
		Assert(ptrMember(restrictinfo, joininfo->jinfo_restrictinfo));
		joininfo->jinfo_restrictinfo = lremove(restrictinfo,
										   joininfo->jinfo_restrictinfo);
		bms_free(unjoined_relids);
	}
	bms_free(tmprelids);
}
