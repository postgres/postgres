/*-------------------------------------------------------------------------
 *
 * joininfo.c
 *	  JoinInfo node manipulation routines
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/joininfo.c,v 1.29 2001/03/22 03:59:39 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"


#include "optimizer/joininfo.h"

static JoinInfo *joininfo_member(List *join_relids, List *joininfo_list);

/*
 * joininfo_member
 *	  Determines whether a node has already been created for a join
 *	  between a set of join relations and the relation described by
 *	  'joininfo_list'.
 *
 * 'join_relids' is a list of relids corresponding to the join relation
 * 'joininfo_list' is the list of joininfo nodes against which this is
 *				checked
 *
 * Returns the corresponding node in 'joininfo_list' if such a node
 * exists.
 *
 */
static JoinInfo *
joininfo_member(List *join_relids, List *joininfo_list)
{
	List	   *i;

	foreach(i, joininfo_list)
	{
		JoinInfo   *joininfo = (JoinInfo *) lfirst(i);

		if (sameseti(join_relids, joininfo->unjoined_relids))
			return joininfo;
	}
	return NULL;
}


/*
 * find_joininfo_node
 *	  Find the joininfo node within a relation entry corresponding
 *	  to a join between 'this_rel' and the relations in 'join_relids'.
 *	  A new node is created and added to the relation entry's joininfo
 *	  field if the desired one can't be found.
 *
 * Returns a joininfo node.
 *
 */
JoinInfo   *
find_joininfo_node(RelOptInfo *this_rel, Relids join_relids)
{
	JoinInfo   *joininfo = joininfo_member(join_relids,
										   this_rel->joininfo);

	if (joininfo == NULL)
	{
		joininfo = makeNode(JoinInfo);
		joininfo->unjoined_relids = join_relids;
		joininfo->jinfo_restrictinfo = NIL;
		this_rel->joininfo = lcons(joininfo, this_rel->joininfo);
	}
	return joininfo;
}
