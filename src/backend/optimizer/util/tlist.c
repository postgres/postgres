/*-------------------------------------------------------------------------
 *
 * tlist.c
 *	  Target list manipulation routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/tlist.c,v 1.34 1999/07/15 15:19:27 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/relation.h"
#include "nodes/primnodes.h"
#include "nodes/pg_list.h"
#include "nodes/nodeFuncs.h"
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
 * tlistentry_member
 *
 * RETURNS:  the leftmost member of sequence "targetlist" that satisfies
 *			 the predicate "var_equal"
 * MODIFIES: nothing
 * REQUIRES: test = function which can operate on a lispval union
 *			 var = valid var_node
 *			 targetlist = valid sequence
 */
TargetEntry *
tlistentry_member(Var *var, List *targetlist)
{
	if (var)
	{
		List	   *temp;

		foreach(temp, targetlist)
		{
			if (var_equal(var,
						  get_expr(lfirst(temp))))
				return (TargetEntry *) lfirst(temp);
		}
	}
	return NULL;
}

/*
 * matching_tlist_var
 *
 * RETURNS:  var node in a target list which is var_equal to 'var',
 *			 if one exists.
 * REQUIRES: "test" operates on lispval unions,
 *
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
 * add_var_to_tlist
 *	  Creates a targetlist entry corresponding to the supplied var node
 *
 * 'var' and adds the new targetlist entry to the targetlist field of
 * 'rel'
 *
 * RETURNS: nothing
 * MODIFIES: vartype and varid fields of leftmost varnode that matches
 *			 argument "var" (sometimes).
 * CREATES:  new var_node iff no matching var_node exists in targetlist
 */
void
add_var_to_tlist(RelOptInfo *rel, Var *var)
{
	Expr	   *oldvar;

	oldvar = matching_tlist_var(var, rel->targetlist);

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

		rel->targetlist = lappend(tlist,
								  create_tl_element(newvar,
													length(tlist) + 1));

	}
}

/*
 * create_tl_element
 *	  Creates a target list entry node and its associated (resdom var) pair
 *	  with its resdom number equal to 'resdomno' and the joinlist field set
 *	  to 'joinlist'.
 *
 * RETURNS:  newly created tlist_entry
 * CREATES:  new targetlist entry (always).
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
 * tlist_member
 *	  Determines whether a var node is already contained within a
 *	  target list.
 *
 * 'var' is the var node
 * 'tlist' is the target list
 *
 * Returns the resdom entry of the matching var node, or NULL if no match.
 *
 */
Resdom *
tlist_member(Var *var, List *tlist)
{
	if (var)
	{
		List	   *i;

		foreach(i, tlist)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(i);

			if (var_equal(var, get_expr(tle)))
				return tle->resdom;
		}
	}
	return (Resdom *) NULL;
}

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
				return entry;
		}
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
	List	   *src = NIL;
	List	   *dest = NIL;

	for (src = source, dest = target; src != NIL &&
		 dest != NIL; src = lnext(src), dest = lnext(dest))
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
	int			last_resdomno = 1;
	List	   *new_tlist = NIL;
	List	   *tlist_vars = NIL;
	List	   *temp;

	foreach(temp, tlist)
	{
		TargetEntry *temp_entry = (TargetEntry *) lfirst(temp);

		tlist_vars = nconc(tlist_vars,
						 pull_var_clause((Node *) get_expr(temp_entry)));
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
						   false);
			last_resdomno++;
			new_tlist = lappend(new_tlist, makeTargetEntry(r, (Node *) var));
		}
	}

	return new_tlist;
}

/*
 * flatten_tlist_vars
 *	  Redoes the target list of a query with no nested attributes by
 *	  replacing vars within computational expressions with vars from
 *	  the 'flattened' target list of the query.
 *
 * 'full_tlist' is the actual target list
 * 'flat_tlist' is the flattened (var-only) target list
 *
 * Returns the modified actual target list.
 *
 */
List *
flatten_tlist_vars(List *full_tlist, List *flat_tlist)
{
	List	   *result = NIL;
	List	   *x;

	foreach(x, full_tlist)
	{
		TargetEntry *tle = lfirst(x);

		result = lappend(result, makeTargetEntry(tle->resdom,
							   flatten_tlistentry((Node *) get_expr(tle),
												  flat_tlist)));
	}

	return result;
}

/*
 * flatten_tlistentry
 *	  Replaces vars within a target list entry with vars from a flattened
 *	  target list.
 *
 * 'tlistentry' is the target list entry to be modified
 * 'flat_tlist' is the flattened target list
 *
 * Returns the (modified) target_list entry from the target list.
 *
 */
static Node *
flatten_tlistentry(Node *tlistentry, List *flat_tlist)
{
	List	   *temp;

	if (tlistentry == NULL)
		return NULL;
	else if (IsA(tlistentry, Var))
		return (Node *) get_expr(match_varid((Var *) tlistentry,
											 flat_tlist));
	else if (single_node(tlistentry))
		return tlistentry;
	else if (IsA(tlistentry, Iter))
	{
		((Iter *) tlistentry)->iterexpr =
			flatten_tlistentry((Node *) ((Iter *) tlistentry)->iterexpr,
							   flat_tlist);
		return tlistentry;
	}
	else if (is_subplan(tlistentry))
	{
		/* do we need to support this case? */
		elog(ERROR, "flatten_tlistentry: subplan case not implemented");
		return tlistentry;
	}
	else if (IsA(tlistentry, Expr))
	{

		/*
		 * Recursively scan the arguments of an expression. NOTE: this
		 * must come after is_subplan() case since subplan is a kind of
		 * Expr node.
		 */
		foreach(temp, ((Expr *) tlistentry)->args)
			lfirst(temp) = flatten_tlistentry(lfirst(temp), flat_tlist);
		return tlistentry;
	}
	else if (IsA(tlistentry, Aggref))
	{

		/*
		 * XXX shouldn't this be recursing into the agg's target? Seems to
		 * work though, so will leave it alone ... tgl 5/99
		 */
		return tlistentry;
	}
	else if (IsA(tlistentry, ArrayRef))
	{
		ArrayRef   *aref = (ArrayRef *) tlistentry;

		foreach(temp, aref->refupperindexpr)
			lfirst(temp) = flatten_tlistentry(lfirst(temp), flat_tlist);
		foreach(temp, aref->reflowerindexpr)
			lfirst(temp) = flatten_tlistentry(lfirst(temp), flat_tlist);
		aref->refexpr = flatten_tlistentry(aref->refexpr, flat_tlist);
		aref->refassgnexpr = flatten_tlistentry(aref->refassgnexpr, flat_tlist);

		return tlistentry;
	}
	else if (case_clause(tlistentry))
	{
		CaseExpr   *cexpr = (CaseExpr *) tlistentry;

		foreach(temp, cexpr->args)
		{
			CaseWhen   *cwhen = (CaseWhen *) lfirst(temp);

			cwhen->expr = flatten_tlistentry(cwhen->expr, flat_tlist);
			cwhen->result = flatten_tlistentry(cwhen->result, flat_tlist);
		}
		cexpr->defresult = flatten_tlistentry(cexpr->defresult, flat_tlist);

		return tlistentry;
	}
	else
	{
		elog(ERROR, "flatten_tlistentry: Cannot handle node type %d",
			 nodeTag(tlistentry));
		return tlistentry;
	}
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
	TargetEntry *tle;

	foreach(l, targetList)
	{
		tle = (TargetEntry *) lfirst(l);
		if (tle->resdom->resgroupref == groupClause->tleGroupref)
			return get_expr(tle);
	}

	elog(ERROR,
	"get_groupclause_expr: GROUP BY expression not found in targetlist");
	return NULL;
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
/*
 * WARNING!!! If this ever get's used again, the new reference
 * mechanism from group clause to targetlist entry must be implemented
 * here too. Jan
 */
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
						   false);
			last_resdomno++;
			tlist = lappend(tlist, makeTargetEntry(r, (Node *) var));
		}
	}
}

#endif
