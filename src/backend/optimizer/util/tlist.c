/*-------------------------------------------------------------------------
 *
 * tlist.c
 *	  Target list manipulation routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/tlist.c,v 1.38 1999/08/10 03:00:15 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"

static Node *flatten_tlist_vars_mutator(Node *node, List *flat_tlist);

/*****************************************************************************
 *	---------- RELATION node target list routines ----------
 *****************************************************************************/

/*
 * tlistentry_member
 *	  Finds the (first) member of the given tlist whose expression is
 *	  var_equal() to the given var.  Result is NULL if no such member.
 */
TargetEntry *
tlistentry_member(Var *var, List *targetlist)
{
	if (var && IsA(var, Var))
	{
		List	   *temp;

		foreach(temp, targetlist)
		{
			if (var_equal(var, get_expr(lfirst(temp))))
				return (TargetEntry *) lfirst(temp);
		}
	}
	return NULL;
}

/*
 * matching_tlist_var
 *	  Same as tlistentry_member(), except returns the tlist expression
 *	  rather than its parent TargetEntry node.
 */
Expr *
matching_tlist_var(Var *var, List *targetlist)
{
	TargetEntry *tlentry;

	tlentry = tlistentry_member(var, targetlist);
	if (tlentry)
		return (Expr *) get_expr(tlentry);

	return (Expr *) NULL;
}

/*
 * tlist_member
 *	  Same as tlistentry_member(), except returns the Resdom node
 *	  rather than its parent TargetEntry node.
 */
Resdom *
tlist_member(Var *var, List *tlist)
{
	TargetEntry *tlentry;

	tlentry = tlistentry_member(var, tlist);
	if (tlentry)
		return tlentry->resdom;

	return (Resdom *) NULL;
}

/*
 * add_var_to_tlist
 *	  Creates a targetlist entry corresponding to the supplied var node
 *	  'var' and adds the new targetlist entry to the targetlist field of
 *	  'rel'.  No entry is created if 'var' is already in the tlist.
 */
void
add_var_to_tlist(RelOptInfo *rel, Var *var)
{
	if (tlistentry_member(var, rel->targetlist) == NULL)
	{
		/* XXX is copyObject necessary here? */
		rel->targetlist = lappend(rel->targetlist,
							create_tl_element((Var *) copyObject(var),
											  length(rel->targetlist) + 1));
	}
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
									  (Index) 0,
									  (Oid) 0,
									  false),
						   (Node *) var);
}

/*
 * get_actual_tlist
 *	  Returns the targetlist elements from a relation tlist.
 *
 */
List *
get_actual_tlist(List *tlist)
{

	/*
	 * this function is not making sense. - ay 10/94
	 */
#ifdef NOT_USED
	List	   *element = NIL;
	List	   *result = NIL;

	if (tlist == NULL)
	{
		elog(DEBUG, "calling get_actual_tlist with empty tlist");
		return NIL;
	}

	/*
	 * XXX - it is unclear to me what exactly get_entry should be doing,
	 * as it is unclear to me the exact relationship between "TL" "TLE"
	 * and joinlists
	 */

	foreach(element, tlist)
		result = lappend(result, lfirst((List *) lfirst(element)));

	return result;
#endif
	return tlist;
}

/*****************************************************************************
 *		---------- GENERAL target list routines ----------
 *****************************************************************************/

/*
 *	 Routine to get the resdom out of a targetlist.
 */
Resdom *
tlist_resdom(List *tlist, Resdom *resnode)
{
	List	   *i;

	foreach(i, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(i);
		Resdom	   *resdom = tle->resdom;

		/* Since resnos are supposed to be unique */
		if (resnode->resno == resdom->resno)
			return resdom;
	}
	return (Resdom *) NULL;
}


/*
 * match_varid
 *	  Searches a target list for an entry matching a given var.
 *
 * Returns the target list entry (resdom var) of the matching var,
 * or NULL if no match.
 */
TargetEntry *
match_varid(Var *test_var, List *tlist)
{
	List	   *tl;

	Assert(test_var->varlevelsup == 0);	/* XXX why? */

	foreach(tl, tlist)
	{
		TargetEntry *entry = lfirst(tl);
		Var		   *tlvar = get_expr(entry);

		if (!IsA(tlvar, Var))
			continue;

		/*
		 * we test the original varno, instead of varno which might be
		 * changed to INNER/OUTER.  XXX is test on vartype necessary?
		 */
		Assert(tlvar->varlevelsup == 0);

		if (tlvar->varnoold == test_var->varnoold &&
			tlvar->varoattno == test_var->varoattno &&
			tlvar->vartype == test_var->vartype)
			return entry;
	}

	return NULL;
}


/*
 * new_unsorted_tlist
 *	  Creates a copy of a target list by creating new resdom nodes
 *	  without sort information.
 *
 * 'targetlist' is the target list to be copied.
 *
 * Returns the resulting target list.
 *
 */
List *
new_unsorted_tlist(List *targetlist)
{
	List	   *new_targetlist = (List *) copyObject((Node *) targetlist);
	List	   *x;

	foreach(x, new_targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(x);

		tle->resdom->reskey = 0;
		tle->resdom->reskeyop = (Oid) 0;
	}
	return new_targetlist;
}

/*
 * copy_vars
 *	  Replaces the var nodes in the first target list with those from
 *	  the second target list.  The two target lists are assumed to be
 *	  identical except their actual resdoms and vars are different.
 *
 * 'target' is the target list to be replaced
 * 'source' is the target list to be copied
 *
 * Returns a new target list.
 *
 */
List *
copy_vars(List *target, List *source)
{
	List	   *result = NIL;
	List	   *src;
	List	   *dest;

	for (src = source, dest = target;
		 src != NIL && dest != NIL;
		 src = lnext(src), dest = lnext(dest))
	{
		TargetEntry *temp = makeTargetEntry(((TargetEntry *) lfirst(dest))->resdom,
										 (Node *) get_expr(lfirst(src)));

		result = lappend(result, temp);
	}
	return result;
}

/*
 * flatten_tlist
 *	  Create a target list that only contains unique variables.
 *
 *
 * 'tlist' is the current target list
 *
 * Returns the "flattened" new target list.
 *
 */
List *
flatten_tlist(List *tlist)
{
	List	   *vlist = pull_var_clause((Node *) tlist);
	int			last_resdomno = 1;
	List	   *new_tlist = NIL;
	List	   *v;

	foreach(v, vlist)
	{
		Var		   *var = lfirst(v);

		if (! tlistentry_member(var, new_tlist))
		{
			Resdom	   *r;

			r = makeResdom(last_resdomno++,
						   var->vartype,
						   var->vartypmod,
						   NULL,
						   (Index) 0,
						   (Oid) 0,
						   false);
			new_tlist = lappend(new_tlist,
								makeTargetEntry(r, (Node *) var));
		}
	}
	freeList(vlist);

	return new_tlist;
}

/*
 * flatten_tlist_vars
 *	  Redoes the target list of a query by replacing vars within
 *	  target expressions with vars from the 'flattened' target list.
 *
 * 'full_tlist' is the original target list
 * 'flat_tlist' is the flattened (var-only) target list
 *
 * Returns the rebuilt target list.  The original is not modified.
 *
 */
List *
flatten_tlist_vars(List *full_tlist, List *flat_tlist)
{
	return (List *) flatten_tlist_vars_mutator((Node *) full_tlist,
											   flat_tlist);
}

static Node *
flatten_tlist_vars_mutator(Node *node, List *flat_tlist)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
		return (Node *) get_expr(match_varid((Var *) node,
											 flat_tlist));
	return expression_tree_mutator(node, flatten_tlist_vars_mutator,
								   (void *) flat_tlist);
}


Var *
get_expr(TargetEntry *tle)
{
	Assert(tle != NULL);
	Assert(tle->expr != NULL);

	return (Var *) tle->expr;
}


Var *
get_groupclause_expr(GroupClause *groupClause, List *targetList)
{
	List	   *l;

	foreach(l, targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);
		if (tle->resdom->resgroupref == groupClause->tleGroupref)
			return get_expr(tle);
	}

	elog(ERROR,
	"get_groupclause_expr: GROUP BY expression not found in targetlist");
	return NULL;
}
