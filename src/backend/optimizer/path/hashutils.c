/*-------------------------------------------------------------------------
 *
 * hashutils.c
 *	  Utilities for finding applicable merge clauses and pathkeys
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/path/Attic/hashutils.c,v 1.13 1999/02/13 23:16:16 momjian Exp $
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
 * 'inner_relid' is the relid of the inner join relation
 *
 * Returns the new list of hashinfo nodes.
 *
 */
List *
group_clauses_by_hashop(List *restrictinfo_list,
						int inner_relid)
{
	List	   *hashinfo_list = NIL;
	RestrictInfo *restrictinfo = (RestrictInfo *) NULL;
	List	   *i = NIL;
	Oid			hashjoinop = 0;

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
			HashInfo   *xhashinfo = (HashInfo *) NULL;
			Expr	   *clause = restrictinfo->clause;
			Var		   *leftop = get_leftop(clause);
			Var		   *rightop = get_rightop(clause);
			JoinKey    *joinkey = (JoinKey *) NULL;

			xhashinfo = match_hashop_hashinfo(hashjoinop, hashinfo_list);

			if (inner_relid == leftop->varno)
			{
				joinkey = makeNode(JoinKey);
				joinkey->outer = rightop;
				joinkey->inner = leftop;
			}
			else
			{
				joinkey = makeNode(JoinKey);
				joinkey->outer = leftop;
				joinkey->inner = rightop;
			}

			if (xhashinfo == NULL)
			{
				xhashinfo = makeNode(HashInfo);
				xhashinfo->hashop = hashjoinop;

				xhashinfo->jmethod.jmkeys = NIL;
				xhashinfo->jmethod.clauses = NIL;

				/* XXX was push  */
				hashinfo_list = lappend(hashinfo_list, xhashinfo);
				hashinfo_list = nreverse(hashinfo_list);
			}

			xhashinfo->jmethod.clauses = lcons(clause, xhashinfo->jmethod.clauses);

			xhashinfo->jmethod.jmkeys = lcons(joinkey, xhashinfo->jmethod.jmkeys);
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
