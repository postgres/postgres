/*-------------------------------------------------------------------------
 *
 * hashutils.c--
 *	  Utilities for finding applicable merge clauses and pathkeys
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/path/Attic/hashutils.c,v 1.4 1997/09/08 21:44:51 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "nodes/pg_list.h"
#include "nodes/relation.h"

#include "optimizer/internal.h"
#include "optimizer/paths.h"
#include "optimizer/clauses.h"


static HInfo *match_hashop_hashinfo(Oid hashop, List *hashinfo_list);

/*
 * group-clauses-by-hashop--
 *	  If a join clause node in 'clauseinfo-list' is hashjoinable, store
 *	  it within a hashinfo node containing other clause nodes with the same
 *	  hash operator.
 *
 * 'clauseinfo-list' is the list of clauseinfo nodes
 * 'inner-relid' is the relid of the inner join relation
 *
 * Returns the new list of hashinfo nodes.
 *
 */
List	   *
group_clauses_by_hashop(List *clauseinfo_list,
						int inner_relid)
{
	List	   *hashinfo_list = NIL;
	CInfo	   *clauseinfo = (CInfo *) NULL;
	List	   *i = NIL;
	Oid			hashjoinop = 0;

	foreach(i, clauseinfo_list)
	{
		clauseinfo = (CInfo *) lfirst(i);
		hashjoinop = clauseinfo->hashjoinoperator;

		/*
		 * Create a new hashinfo node and add it to 'hashinfo-list' if one
		 * does not yet exist for this hash operator.
		 */
		if (hashjoinop)
		{
			HInfo	   *xhashinfo = (HInfo *) NULL;
			Expr	   *clause = clauseinfo->clause;
			Var		   *leftop = get_leftop(clause);
			Var		   *rightop = get_rightop(clause);
			JoinKey    *keys = (JoinKey *) NULL;

			xhashinfo =
				match_hashop_hashinfo(hashjoinop, hashinfo_list);

			if (inner_relid == leftop->varno)
			{
				keys = makeNode(JoinKey);
				keys->outer = rightop;
				keys->inner = leftop;
			}
			else
			{
				keys = makeNode(JoinKey);
				keys->outer = leftop;
				keys->inner = rightop;
			}

			if (xhashinfo == NULL)
			{
				xhashinfo = makeNode(HInfo);
				xhashinfo->hashop = hashjoinop;

				xhashinfo->jmethod.jmkeys = NIL;
				xhashinfo->jmethod.clauses = NIL;

				/* XXX was push  */
				hashinfo_list = lappend(hashinfo_list, xhashinfo);
				hashinfo_list = nreverse(hashinfo_list);
			}

			xhashinfo->jmethod.clauses =
				lcons(clause, xhashinfo->jmethod.clauses);

			xhashinfo->jmethod.jmkeys =
				lcons(keys, xhashinfo->jmethod.jmkeys);
		}
	}
	return (hashinfo_list);
}


/*
 * match-hashop-hashinfo--
 *	  Searches the list 'hashinfo-list' for a hashinfo node whose hash op
 *	  field equals 'hashop'.
 *
 * Returns the node if it exists.
 *
 */
static HInfo *
match_hashop_hashinfo(Oid hashop, List *hashinfo_list)
{
	Oid			key = 0;
	HInfo	   *xhashinfo = (HInfo *) NULL;
	List	   *i = NIL;

	foreach(i, hashinfo_list)
	{
		xhashinfo = (HInfo *) lfirst(i);
		key = xhashinfo->hashop;
		if (hashop == key)
		{						/* found */
			return (xhashinfo); /* should be a hashinfo node ! */
		}
	}
	return ((HInfo *) NIL);
}
