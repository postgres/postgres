/*-------------------------------------------------------------------------
 *
 * joininfo.c
 *	  JoinInfo node manipulation routines
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/joininfo.c,v 1.32 2003/01/20 18:54:56 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/joininfo.h"


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

		if (sameseti(join_relids, joininfo->unjoined_relids))
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
