/*-------------------------------------------------------------------------
 *
 * tlist.c--
 *	  Target list manipulation routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/tlist.c,v 1.12 1998/02/26 04:33:22 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/relation.h"
#include "nodes/primnodes.h"
#include "nodes/pg_list.h"
#include "nodes/nodeFuncs.h"
#include "utils/elog.h"
#include "utils/lsyscache.h"

#include "optimizer/internal.h"
#include "optimizer/var.h"
#include "optimizer/tlist.h"
#include "optimizer/clauses.h"

#include "nodes/makefuncs.h"

static Node *flatten_tlistentry(Node *tlistentry, List *flat_tlist);

/*****************************************************************************
 *	---------- RELATION node target list routines ----------
 *****************************************************************************/

/*
 * tlistentry-member--
 *
 * RETURNS:  the leftmost member of sequence "targetlist" that satisfies
 *			 the predicate "var_equal"
 * MODIFIES: nothing
 * REQUIRES: test = function which can operate on a lispval union
 *			 var = valid var-node
 *			 targetlist = valid sequence
 */
TargetEntry *
tlistentry_member(Var *var, List *targetlist)
{
	if (var)
	{
		List	   *temp = NIL;

		foreach(temp, targetlist)
		{
			if (var_equal(var,
						  get_expr(lfirst(temp))))
				return ((TargetEntry *) lfirst(temp));
		}
	}
	return (NULL);
}

/*
 * matching_tlvar--
 *
 * RETURNS:  var node in a target list which is var_equal to 'var',
 *			 if one exists.
 * REQUIRES: "test" operates on lispval unions,
 *
 */
Expr *
matching_tlvar(Var *var, List *targetlist)
{
	TargetEntry *tlentry;

	tlentry = tlistentry_member(var, targetlist);
	if (tlentry)
		return ((Expr *) get_expr(tlentry));

	return ((Expr *) NULL);
}

/*
 * add_tl_element--
 *	  Creates a targetlist entry corresponding to the supplied var node
 *
 * 'var' and adds the new targetlist entry to the targetlist field of
 * 'rel'
 *
 * RETURNS: nothing
 * MODIFIES: vartype and varid fields of leftmost varnode that matches
 *			 argument "var" (sometimes).
 * CREATES:  new var-node iff no matching var-node exists in targetlist
 */
void
add_tl_element(Rel *rel, Var *var)
{
	Expr	   *oldvar = (Expr *) NULL;

	oldvar = matching_tlvar(var, rel->targetlist);

	/*
	 * If 'var' is not already in 'rel's target list, add a new node.
	 */
	if (oldvar == NULL)
	{
		List	   *tlist = rel->targetlist;
		Var		   *newvar = makeVar(var->varno,
									 var->varattno,
									 var->vartype,
									 var->vartypmod,
									 var->varlevelsup,
									 var->varno,
									 var->varoattno);

		rel->targetlist =
			lappend(tlist,
					create_tl_element(newvar,
									  length(tlist) + 1));

	}
}

/*
 * create_tl_element--
 *	  Creates a target list entry node and its associated (resdom var) pair
 *	  with its resdom number equal to 'resdomno' and the joinlist field set
 *	  to 'joinlist'.
 *
 * RETURNS:  newly created tlist-entry
 * CREATES:  new targetlist entry (always).
 */
TargetEntry *
create_tl_element(Var *var, int resdomno)
{
	TargetEntry *tlelement = makeNode(TargetEntry);

	tlelement->resdom =
		makeResdom(resdomno,
				   var->vartype,
				   var->vartypmod,
				   NULL,
				   (Index) 0,
				   (Oid) 0,
				   0);
	tlelement->expr = (Node *) var;

	return (tlelement);
}

/*
 * get-actual-tlist--
 *	  Returns the targetlist elements from a relation tlist.
 *
 */
List *
get_actual_tlist(List *tlist)
{

	/*
	 * this function is not making sense. - ay 10/94
	 */
#if 0
	List	   *element = NIL;
	List	   *result = NIL;

	if (tlist == NULL)
	{
		elog(DEBUG, "calling get_actual_tlist with empty tlist");
		return (NIL);
	}

	/*
	 * XXX - it is unclear to me what exactly get_entry should be doing,
	 * as it is unclear to me the exact relationship between "TL" "TLE"
	 * and joinlists
	 */

	foreach(element, tlist)
		result = lappend(result, lfirst((List *) lfirst(element)));

	return (result);
#endif
	return tlist;
}

/*****************************************************************************
 *		---------- GENERAL target list routines ----------
 *****************************************************************************/

/*
 * tlist-member--
 *	  Determines whether a var node is already contained within a
 *	  target list.
 *
 * 'var' is the var node
 * 'tlist' is the target list
 * 'dots' is t if we must match dotfields to determine uniqueness
 *
 * Returns the resdom entry of the matching var node.
 *
 */
Resdom *
tlist_member(Var *var, List *tlist)
{
	List	   *i = NIL;
	TargetEntry *temp_tle = (TargetEntry *) NULL;
	TargetEntry *tl_elt = (TargetEntry *) NULL;

	if (var)
	{
		foreach(i, tlist)
		{
			temp_tle = (TargetEntry *) lfirst(i);
			if (var_equal(var, get_expr(temp_tle)))
			{
				tl_elt = temp_tle;
				break;
			}
		}

		if (tl_elt != NULL)
			return (tl_elt->resdom);
		else
			return ((Resdom *) NULL);
	}
	return ((Resdom *) NULL);
}

/*
 *	 Routine to get the resdom out of a targetlist.
 */
Resdom *
tlist_resdom(List *tlist, Resdom *resnode)
{
	Resdom	   *resdom = (Resdom *) NULL;
	List	   *i = NIL;
	TargetEntry *temp_tle = (TargetEntry *) NULL;

	foreach(i, tlist)
	{
		temp_tle = (TargetEntry *) lfirst(i);
		resdom = temp_tle->resdom;
		/* Since resnos are supposed to be unique */
		if (resnode->resno == resdom->resno)
			return (resdom);
	}
	return ((Resdom *) NULL);
}


/*
 * match_varid--
 *	  Searches a target list for an entry with some desired varid.
 *
 * 'varid' is the desired id
 * 'tlist' is the target list that is searched
 *
 * Returns the target list entry (resdom var) of the matching var.
 *
 * Now checks to make sure array references (in addition to range
 * table indices) are identical - retrieve (a.b[1],a.b[2]) should
 * not be turned into retrieve (a.b[1],a.b[1]).
 *
 * [what used to be varid is now broken up into two fields varnoold and
 *	varoattno. Also, nested attnos are long gone. - ay 2/95]
 */
TargetEntry *
match_varid(Var *test_var, List *tlist)
{
	List	   *tl;
	Oid			type_var;

	type_var = (Oid) test_var->vartype;

	Assert(test_var->varlevelsup == 0);
	foreach(tl, tlist)
	{
		TargetEntry *entry;
		Var		   *tlvar;

		entry = lfirst(tl);
		tlvar = get_expr(entry);

		if (!IsA(tlvar, Var))
			continue;

		/*
		 * we test the original varno (instead of varno which might be
		 * changed to INNER/OUTER.
		 */
		Assert(tlvar->varlevelsup == 0);
		if (tlvar->varnoold == test_var->varnoold &&
			tlvar->varoattno == test_var->varoattno)
		{

			if (tlvar->vartype == type_var)
				return (entry);
		}
	}

	return (NULL);
}


/*
 * new-unsorted-tlist--
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
	List	   *x = NIL;

	foreach(x, new_targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(x);

		tle->resdom->reskey = 0;
		tle->resdom->reskeyop = (Oid) 0;
	}
	return (new_targetlist);
}

/*
 * copy-vars--
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
	List	   *src = NIL;
	List	   *dest = NIL;

	for (src = source, dest = target; src != NIL &&
		 dest != NIL; src = lnext(src), dest = lnext(dest))
	{
		TargetEntry *temp = MakeTLE(((TargetEntry *) lfirst(dest))->resdom,
									(Node *) get_expr(lfirst(src)));

		result = lappend(result, temp);
	}
	return (result);
}

/*
 * flatten-tlist--
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
	int			last_resdomno = 1;
	List	   *new_tlist = NIL;
	List	   *tlist_vars = NIL;
	List	   *temp;

	foreach(temp, tlist)
	{
		TargetEntry *temp_entry = NULL;
		List	   *vars;

		temp_entry = lfirst(temp);
		vars = pull_var_clause((Node *) get_expr(temp_entry));
		if (vars != NULL)
		{
			tlist_vars = nconc(tlist_vars, vars);
		}
	}

	foreach(temp, tlist_vars)
	{
		Var		   *var = lfirst(temp);

		if (!(tlist_member(var, new_tlist)))
		{
			Resdom	   *r;

			r = makeResdom(last_resdomno,
						   var->vartype,
						   var->vartypmod,
						   NULL,
						   (Index) 0,
						   (Oid) 0,
						   0);
			last_resdomno++;
			new_tlist = lappend(new_tlist, MakeTLE(r, (Node *) var));
		}
	}

	return new_tlist;
}

/*
 * flatten-tlist-vars--
 *	  Redoes the target list of a query with no nested attributes by
 *	  replacing vars within computational expressions with vars from
 *	  the 'flattened' target list of the query.
 *
 * 'full-tlist' is the actual target list
 * 'flat-tlist' is the flattened (var-only) target list
 *
 * Returns the modified actual target list.
 *
 */
List *
flatten_tlist_vars(List *full_tlist, List *flat_tlist)
{
	List	   *x = NIL;
	List	   *result = NIL;

	foreach(x, full_tlist)
	{
		TargetEntry *tle = lfirst(x);

		result =
			lappend(result,
					MakeTLE(tle->resdom,
							flatten_tlistentry((Node *) get_expr(tle),
											   flat_tlist)));
	}

	return (result);
}

/*
 * flatten-tlistentry--
 *	  Replaces vars within a target list entry with vars from a flattened
 *	  target list.
 *
 * 'tlistentry' is the target list entry to be modified
 * 'flat-tlist' is the flattened target list
 *
 * Returns the (modified) target_list entry from the target list.
 *
 */
static Node *
flatten_tlistentry(Node *tlistentry, List *flat_tlist)
{
	if (tlistentry == NULL)
	{

		return NULL;

	}
	else if (IsA(tlistentry, Var))
	{

		return
			((Node *) get_expr(match_varid((Var *) tlistentry,
										   flat_tlist)));
	}
	else if (IsA(tlistentry, Iter))
	{

		((Iter *) tlistentry)->iterexpr =
			flatten_tlistentry((Node *) ((Iter *) tlistentry)->iterexpr,
							   flat_tlist);
		return tlistentry;

	}
	else if (single_node(tlistentry))
	{

		return tlistentry;

	}
	else if (is_funcclause(tlistentry))
	{
		Expr	   *expr = (Expr *) tlistentry;
		List	   *temp_result = NIL;
		List	   *elt = NIL;

		foreach(elt, expr->args)
			temp_result = lappend(temp_result,
							flatten_tlistentry(lfirst(elt), flat_tlist));

		return
			((Node *) make_funcclause((Func *) expr->oper, temp_result));

	}
	else if (IsA(tlistentry, Aggreg))
	{

		return tlistentry;

	}
	else if (IsA(tlistentry, ArrayRef))
	{
		ArrayRef   *aref = (ArrayRef *) tlistentry;
		List	   *temp = NIL;
		List	   *elt = NIL;

		foreach(elt, aref->refupperindexpr)
			temp = lappend(temp, flatten_tlistentry(lfirst(elt), flat_tlist));
		aref->refupperindexpr = temp;

		temp = NIL;
		foreach(elt, aref->reflowerindexpr)
			temp = lappend(temp, flatten_tlistentry(lfirst(elt), flat_tlist));
		aref->reflowerindexpr = temp;

		aref->refexpr =
			flatten_tlistentry(aref->refexpr, flat_tlist);

		aref->refassgnexpr =
			flatten_tlistentry(aref->refassgnexpr, flat_tlist);

		return tlistentry;
	}
	else
	{
		Expr	   *expr = (Expr *) tlistentry;
		Var		   *left =
		(Var *) flatten_tlistentry((Node *) get_leftop(expr),
								   flat_tlist);
		Var		   *right =
		(Var *) flatten_tlistentry((Node *) get_rightop(expr),
								   flat_tlist);

		return ((Node *)
				make_opclause((Oper *) expr->oper, left, right));
	}
}


TargetEntry *
MakeTLE(Resdom *resdom, Node *expr)
{
	TargetEntry *rt = makeNode(TargetEntry);

	rt->resdom = resdom;
	rt->expr = expr;
	return rt;
}

Var *
get_expr(TargetEntry *tle)
{
	Assert(tle != NULL);
	Assert(tle->expr != NULL);

	return ((Var *) tle->expr);
}


/*****************************************************************************
 *
 *****************************************************************************/

/*
 * AddGroupAttrToTlist -
 *	  append the group attribute to the target list if it's not already
 *	  in there.
 */
#ifdef NOT_USED
void
AddGroupAttrToTlist(List *tlist, List *grpCl)
{
	List	   *gl;
	int			last_resdomno = length(tlist) + 1;

	foreach(gl, grpCl)
	{
		GroupClause *gc = (GroupClause *) lfirst(gl);
		Var		   *var = gc->grpAttr;

		if (!(tlist_member(var, tlist)))
		{
			Resdom	   *r;

			r = makeResdom(last_resdomno,
						   var->vartype,
						   var->vartypmod,
						   NULL,
						   (Index) 0,
						   (Oid) 0,
						   0);
			last_resdomno++;
			tlist = lappend(tlist, MakeTLE(r, (Node *) var));
		}
	}
}

#endif

/* was ExecTargetListLength() in execQual.c,
   moved here to reduce dependencies on the executor module */
int
exec_tlist_length(List *targetlist)
{
	int			len;
	List	   *tl;
	TargetEntry *curTle;

	len = 0;
	foreach(tl, targetlist)
	{
		curTle = lfirst(tl);

		if (curTle->resdom != NULL)
			len++;
	}
	return len;
}
