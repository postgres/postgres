/*-------------------------------------------------------------------------
 *
 * mergeutils.c
 *	  Utilities for finding applicable merge clauses and pathkeys
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/path/Attic/mergeutils.c,v 1.21 1999/04/03 00:18:28 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/pg_list.h"
#include "nodes/relation.h"

#include "optimizer/internal.h"
#include "optimizer/paths.h"
#include "optimizer/clauses.h"
#include "optimizer/ordering.h"

/*
 * group_clauses_by_order
 *	  If a join clause node in 'restrictinfo_list' is mergejoinable, store
 *	  it within a mergeinfo node containing other clause nodes with the same
 *	  mergejoin ordering.
 *
 * XXX This is completely braindead: there is no reason anymore to segregate
 * mergejoin clauses by join operator, since the executor can handle mergejoin
 * clause sets with different operators in them.  Instead, we ought to be
 * building a MergeInfo for each potentially useful ordering of the input
 * relations.  But right now the optimizer's internal data structures do not
 * support that (MergeInfo can only store one MergeOrder for a set of clauses).
 * Something to fix next time...
 *
 * 'restrictinfo_list' is the list of restrictinfo nodes
 * 'inner_relids' is the list of relids in the inner join relation
 *   (used to determine whether a join var is inner or outer)
 *
 * Returns the new list of mergeinfo nodes.
 *
 */
List *
group_clauses_by_order(List *restrictinfo_list,
					   Relids inner_relids)
{
	List	   *mergeinfo_list = NIL;
	List	   *xrestrictinfo;

	foreach(xrestrictinfo, restrictinfo_list)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(xrestrictinfo);
		MergeOrder *merge_ordering = restrictinfo->mergejoinorder;

		if (merge_ordering)
		{
			/*
			 * Create a new mergeinfo node and add it to 'mergeinfo_list'
			 * if one does not yet exist for this merge ordering.
			 */
			Expr	   *clause = restrictinfo->clause;
			Var		   *leftop = get_leftop(clause);
			Var		   *rightop = get_rightop(clause);
			PathOrder	*pathorder;
			MergeInfo	*xmergeinfo;
			JoinKey    *jmkeys;

			pathorder = makeNode(PathOrder);
			pathorder->ordtype = MERGE_ORDER;
			pathorder->ord.merge = merge_ordering;
			xmergeinfo = match_order_mergeinfo(pathorder, mergeinfo_list);
			jmkeys = makeNode(JoinKey);
			if (intMember(leftop->varno, inner_relids))
			{
				jmkeys->outer = rightop;
				jmkeys->inner = leftop;
			}
			else
			{
				jmkeys->outer = leftop;
				jmkeys->inner = rightop;
			}

			if (xmergeinfo == NULL)
			{
				xmergeinfo = makeNode(MergeInfo);
				xmergeinfo->m_ordering = merge_ordering;
				mergeinfo_list = lcons(xmergeinfo, mergeinfo_list);
			}

			xmergeinfo->jmethod.clauses = lcons(clause,
												xmergeinfo->jmethod.clauses);
			xmergeinfo->jmethod.jmkeys = lcons(jmkeys,
											   xmergeinfo->jmethod.jmkeys);
		}
	}
	return mergeinfo_list;
}


/*
 * match_order_mergeinfo
 *	  Searches the list 'mergeinfo_list' for a mergeinfo node whose order
 *	  field equals 'ordering'.
 *
 * Returns the node if it exists.
 *
 */
MergeInfo *
match_order_mergeinfo(PathOrder *ordering, List *mergeinfo_list)
{
	MergeOrder *xmergeorder;
	List	   *xmergeinfo = NIL;

	foreach(xmergeinfo, mergeinfo_list)
	{
		MergeInfo	   *mergeinfo = (MergeInfo *) lfirst(xmergeinfo);

		xmergeorder = mergeinfo->m_ordering;

		if ((ordering->ordtype == MERGE_ORDER &&
		 equal_merge_ordering(ordering->ord.merge, xmergeorder)) ||
			(ordering->ordtype == SORTOP_ORDER &&
		   equal_path_merge_ordering(ordering->ord.sortop, xmergeorder)))
		{

			return mergeinfo;
		}
	}
	return (MergeInfo *) NIL;
}
