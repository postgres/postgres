/*-------------------------------------------------------------------------
 *
 * tlist.c
 *	  Target list manipulation routines
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/tlist.c,v 1.60 2003/08/04 02:40:01 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"


/*****************************************************************************
 *		Target list creation and searching utilities
 *****************************************************************************/

/*
 * tlistentry_member
 *	  Finds the (first) member of the given tlist whose expression is
 *	  equal() to the given expression.	Result is NULL if no such member.
 */
TargetEntry *
tlistentry_member(Node *node, List *targetlist)
{
	List	   *temp;

	foreach(temp, targetlist)
	{
		TargetEntry *tlentry = (TargetEntry *) lfirst(temp);

		if (equal(node, tlentry->expr))
			return tlentry;
	}
	return NULL;
}

#ifdef NOT_USED
/*
 * matching_tlist_expr
 *	  Same as tlistentry_member(), except returns the tlist expression
 *	  rather than its parent TargetEntry node.
 */
Node *
matching_tlist_expr(Node *node, List *targetlist)
{
	TargetEntry *tlentry;

	tlentry = tlistentry_member(node, targetlist);
	if (tlentry)
		return tlentry->expr;

	return (Node *) NULL;
}
#endif

/*
 * tlist_member
 *	  Same as tlistentry_member(), except returns the Resdom node
 *	  rather than its parent TargetEntry node.
 */
Resdom *
tlist_member(Node *node, List *targetlist)
{
	TargetEntry *tlentry;

	tlentry = tlistentry_member(node, targetlist);
	if (tlentry)
		return tlentry->resdom;

	return (Resdom *) NULL;
}

/*
 * create_tl_element
 *	  Creates a target list entry node and its associated (resdom var) pair
 *	  with its resdom number equal to 'resdomno'.
 */
TargetEntry *
create_tl_element(Var *var, int resdomno)
{
	return makeTargetEntry(makeResdom(resdomno,
									  var->vartype,
									  var->vartypmod,
									  NULL,
									  false),
						   (Expr *) var);
}

/*****************************************************************************
 *		---------- GENERAL target list routines ----------
 *****************************************************************************/

/*
 * flatten_tlist
 *	  Create a target list that only contains unique variables.
 *
 * Note that Vars with varlevelsup > 0 are not included in the output
 * tlist.  We expect that those will eventually be replaced with Params,
 * but that probably has not happened at the time this routine is called.
 *
 * 'tlist' is the current target list
 *
 * Returns the "flattened" new target list.
 *
 * The result is entirely new structure sharing no nodes with the original.
 * Copying the Var nodes is probably overkill, but be safe for now.
 */
List *
flatten_tlist(List *tlist)
{
	List	   *vlist = pull_var_clause((Node *) tlist, false);
	List	   *new_tlist;

	new_tlist = add_to_flat_tlist(NIL, vlist);
	freeList(vlist);
	return new_tlist;
}

/*
 * add_to_flat_tlist
 *		Add more vars to a flattened tlist (if they're not already in it)
 *
 * 'tlist' is the flattened tlist
 * 'vars' is a list of var nodes
 *
 * Returns the extended tlist.
 */
List *
add_to_flat_tlist(List *tlist, List *vars)
{
	int			next_resdomno = length(tlist) + 1;
	List	   *v;

	foreach(v, vars)
	{
		Var		   *var = lfirst(v);

		if (!tlistentry_member((Node *) var, tlist))
		{
			Resdom	   *r;

			r = makeResdom(next_resdomno++,
						   var->vartype,
						   var->vartypmod,
						   NULL,
						   false);
			tlist = lappend(tlist,
							makeTargetEntry(r, copyObject(var)));
		}
	}
	return tlist;
}

/*
 * get_sortgroupclause_tle
 *		Find the targetlist entry matching the given SortClause
 *		(or GroupClause) by ressortgroupref, and return it.
 *
 * Because GroupClause is typedef'd as SortClause, either kind of
 * node can be passed without casting.
 */
TargetEntry *
get_sortgroupclause_tle(SortClause *sortClause,
						List *targetList)
{
	Index		refnumber = sortClause->tleSortGroupRef;
	List	   *l;

	foreach(l, targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->resdom->ressortgroupref == refnumber)
			return tle;
	}

	elog(ERROR, "ORDER/GROUP BY expression not found in targetlist");
	return NULL;				/* keep compiler quiet */
}

/*
 * get_sortgroupclause_expr
 *		Find the targetlist entry matching the given SortClause
 *		(or GroupClause) by ressortgroupref, and return its expression.
 *
 * Because GroupClause is typedef'd as SortClause, either kind of
 * node can be passed without casting.
 */
Node *
get_sortgroupclause_expr(SortClause *sortClause, List *targetList)
{
	TargetEntry *tle = get_sortgroupclause_tle(sortClause, targetList);

	return (Node *) tle->expr;
}

/*
 * get_sortgrouplist_exprs
 *		Given a list of SortClauses (or GroupClauses), build a list
 *		of the referenced targetlist expressions.
 */
List *
get_sortgrouplist_exprs(List *sortClauses, List *targetList)
{
	List	   *result = NIL;
	List	   *l;

	foreach(l, sortClauses)
	{
		SortClause *sortcl = (SortClause *) lfirst(l);
		Node	   *sortexpr;

		sortexpr = get_sortgroupclause_expr(sortcl, targetList);
		result = lappend(result, sortexpr);
	}
	return result;
}
