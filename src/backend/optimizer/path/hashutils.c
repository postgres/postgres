/*-------------------------------------------------------------------------
 *
 * hashutils.c
 *	  Utilities for finding applicable merge clauses and pathkeys
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/path/Attic/hashutils.c,v 1.15 1999/04/03 00:18:27 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "nodes/pg_list.h"
#include "nodes/relation.h"

#include "optimizer/internal.h"
#include "optimizer/paths.h"
#include "optimizer/clauses.h"


static HashInfo *match_hashop_hashinfo(Oid hashop, List *hashinfo_list);

/*
 * group_clauses_by_hashop
 *	  If a join clause node in 'restrictinfo_list' is hashjoinable, store
 *	  it within a hashinfo node containing other clause nodes with the same
 *	  hash operator.
 *
 * 'restrictinfo_list' is the list of restrictinfo nodes
 * 'inner_relids' is the list of relids in the inner join relation
 *   (used to determine whether a join var is inner or outer)
 *
 * Returns the new list of hashinfo nodes.
 *
 */
List *
group_clauses_by_hashop(List *restrictinfo_list,
						Relids inner_relids)
{
	List	   *hashinfo_list = NIL;
	RestrictInfo *restrictinfo;
	List	   *i;
	Oid			hashjoinop;

	foreach(i, restrictinfo_list)
	{
		restrictinfo = (RestrictInfo *) lfirst(i);
		hashjoinop = restrictinfo->hashjoinoperator;

		/*
		 * Create a new hashinfo node and add it to 'hashinfo_list' if one
		 * does not yet exist for this hash operator.
		 */
		if (hashjoinop)
		{
			Expr	   *clause = restrictinfo->clause;
			Var		   *leftop = get_leftop(clause);
			Var		   *rightop = get_rightop(clause);
			HashInfo   *xhashinfo;
			JoinKey    *joinkey;

			xhashinfo = match_hashop_hashinfo(hashjoinop, hashinfo_list);
			joinkey = makeNode(JoinKey);
			if (intMember(leftop->varno, inner_relids))
			{
				joinkey->outer = rightop;
				joinkey->inner = leftop;
			}
			else
			{
				joinkey->outer = leftop;
				joinkey->inner = rightop;
			}

			if (xhashinfo == NULL)
			{
				xhashinfo = makeNode(HashInfo);
				xhashinfo->hashop = hashjoinop;
				xhashinfo->jmethod.jmkeys = NIL;
				xhashinfo->jmethod.clauses = NIL;
				hashinfo_list = lcons(xhashinfo, hashinfo_list);
			}

			xhashinfo->jmethod.clauses = lcons(clause,
											   xhashinfo->jmethod.clauses);
			xhashinfo->jmethod.jmkeys = lcons(joinkey,
											  xhashinfo->jmethod.jmkeys);
		}
	}
	return hashinfo_list;
}


/*
 * match_hashop_hashinfo
 *	  Searches the list 'hashinfo_list' for a hashinfo node whose hash op
 *	  field equals 'hashop'.
 *
 * Returns the node if it exists.
 *
 */
static HashInfo *
match_hashop_hashinfo(Oid hashop, List *hashinfo_list)
{
	Oid			key = 0;
	HashInfo	   *xhashinfo = (HashInfo *) NULL;
	List	   *i = NIL;

	foreach(i, hashinfo_list)
	{
		xhashinfo = (HashInfo *) lfirst(i);
		key = xhashinfo->hashop;
		if (hashop == key)
		{						/* found */
			return xhashinfo;	/* should be a hashinfo node ! */
		}
	}
	return (HashInfo *) NIL;
}
