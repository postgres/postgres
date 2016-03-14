/*-------------------------------------------------------------------------
 *
 * tlist.c
 *	  Target list manipulation routines
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/tlist.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/tlist.h"


/*****************************************************************************
 *		Target list creation and searching utilities
 *****************************************************************************/

/*
 * tlist_member
 *	  Finds the (first) member of the given tlist whose expression is
 *	  equal() to the given expression.  Result is NULL if no such member.
 */
TargetEntry *
tlist_member(Node *node, List *targetlist)
{
	ListCell   *temp;

	foreach(temp, targetlist)
	{
		TargetEntry *tlentry = (TargetEntry *) lfirst(temp);

		if (equal(node, tlentry->expr))
			return tlentry;
	}
	return NULL;
}

/*
 * tlist_member_ignore_relabel
 *	  Same as above, except that we ignore top-level RelabelType nodes
 *	  while checking for a match.  This is needed for some scenarios
 *	  involving binary-compatible sort operations.
 */
TargetEntry *
tlist_member_ignore_relabel(Node *node, List *targetlist)
{
	ListCell   *temp;

	while (node && IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;

	foreach(temp, targetlist)
	{
		TargetEntry *tlentry = (TargetEntry *) lfirst(temp);
		Expr	   *tlexpr = tlentry->expr;

		while (tlexpr && IsA(tlexpr, RelabelType))
			tlexpr = ((RelabelType *) tlexpr)->arg;

		if (equal(node, tlexpr))
			return tlentry;
	}
	return NULL;
}

/*
 * tlist_member_match_var
 *	  Same as above, except that we match the provided Var on the basis
 *	  of varno/varattno/varlevelsup/vartype only, rather than full equal().
 *
 * This is needed in some cases where we can't be sure of an exact typmod
 * match.  For safety, though, we insist on vartype match.
 */
static TargetEntry *
tlist_member_match_var(Var *var, List *targetlist)
{
	ListCell   *temp;

	foreach(temp, targetlist)
	{
		TargetEntry *tlentry = (TargetEntry *) lfirst(temp);
		Var		   *tlvar = (Var *) tlentry->expr;

		if (!tlvar || !IsA(tlvar, Var))
			continue;
		if (var->varno == tlvar->varno &&
			var->varattno == tlvar->varattno &&
			var->varlevelsup == tlvar->varlevelsup &&
			var->vartype == tlvar->vartype)
			return tlentry;
	}
	return NULL;
}

/*
 * add_to_flat_tlist
 *		Add more items to a flattened tlist (if they're not already in it)
 *
 * 'tlist' is the flattened tlist
 * 'exprs' is a list of expressions (usually, but not necessarily, Vars)
 *
 * Returns the extended tlist.
 */
List *
add_to_flat_tlist(List *tlist, List *exprs)
{
	int			next_resno = list_length(tlist) + 1;
	ListCell   *lc;

	foreach(lc, exprs)
	{
		Node	   *expr = (Node *) lfirst(lc);

		if (!tlist_member(expr, tlist))
		{
			TargetEntry *tle;

			tle = makeTargetEntry(copyObject(expr),		/* copy needed?? */
								  next_resno++,
								  NULL,
								  false);
			tlist = lappend(tlist, tle);
		}
	}
	return tlist;
}


/*
 * get_tlist_exprs
 *		Get just the expression subtrees of a tlist
 *
 * Resjunk columns are ignored unless includeJunk is true
 */
List *
get_tlist_exprs(List *tlist, bool includeJunk)
{
	List	   *result = NIL;
	ListCell   *l;

	foreach(l, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->resjunk && !includeJunk)
			continue;

		result = lappend(result, tle->expr);
	}
	return result;
}


/*
 * count_nonjunk_tlist_entries
 *		What it says ...
 */
int
count_nonjunk_tlist_entries(List *tlist)
{
	int			len = 0;
	ListCell   *l;

	foreach(l, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (!tle->resjunk)
			len++;
	}
	return len;
}


/*
 * tlist_same_exprs
 *		Check whether two target lists contain the same expressions
 *
 * Note: this function is used to decide whether it's safe to jam a new tlist
 * into a non-projection-capable plan node.  Obviously we can't do that unless
 * the node's tlist shows it already returns the column values we want.
 * However, we can ignore the TargetEntry attributes resname, ressortgroupref,
 * resorigtbl, resorigcol, and resjunk, because those are only labelings that
 * don't affect the row values computed by the node.  (Moreover, if we didn't
 * ignore them, we'd frequently fail to make the desired optimization, since
 * the planner tends to not bother to make resname etc. valid in intermediate
 * plan nodes.)  Note that on success, the caller must still jam the desired
 * tlist into the plan node, else it won't have the desired labeling fields.
 */
bool
tlist_same_exprs(List *tlist1, List *tlist2)
{
	ListCell   *lc1,
			   *lc2;

	if (list_length(tlist1) != list_length(tlist2))
		return false;			/* not same length, so can't match */

	forboth(lc1, tlist1, lc2, tlist2)
	{
		TargetEntry *tle1 = (TargetEntry *) lfirst(lc1);
		TargetEntry *tle2 = (TargetEntry *) lfirst(lc2);

		if (!equal(tle1->expr, tle2->expr))
			return false;
	}

	return true;
}


/*
 * Does tlist have same output datatypes as listed in colTypes?
 *
 * Resjunk columns are ignored if junkOK is true; otherwise presence of
 * a resjunk column will always cause a 'false' result.
 *
 * Note: currently no callers care about comparing typmods.
 */
bool
tlist_same_datatypes(List *tlist, List *colTypes, bool junkOK)
{
	ListCell   *l;
	ListCell   *curColType = list_head(colTypes);

	foreach(l, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->resjunk)
		{
			if (!junkOK)
				return false;
		}
		else
		{
			if (curColType == NULL)
				return false;	/* tlist longer than colTypes */
			if (exprType((Node *) tle->expr) != lfirst_oid(curColType))
				return false;
			curColType = lnext(curColType);
		}
	}
	if (curColType != NULL)
		return false;			/* tlist shorter than colTypes */
	return true;
}

/*
 * Does tlist have same exposed collations as listed in colCollations?
 *
 * Identical logic to the above, but for collations.
 */
bool
tlist_same_collations(List *tlist, List *colCollations, bool junkOK)
{
	ListCell   *l;
	ListCell   *curColColl = list_head(colCollations);

	foreach(l, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->resjunk)
		{
			if (!junkOK)
				return false;
		}
		else
		{
			if (curColColl == NULL)
				return false;	/* tlist longer than colCollations */
			if (exprCollation((Node *) tle->expr) != lfirst_oid(curColColl))
				return false;
			curColColl = lnext(curColColl);
		}
	}
	if (curColColl != NULL)
		return false;			/* tlist shorter than colCollations */
	return true;
}

/*
 * apply_tlist_labeling
 *		Apply the TargetEntry labeling attributes of src_tlist to dest_tlist
 *
 * This is useful for reattaching column names etc to a plan's final output
 * targetlist.
 */
void
apply_tlist_labeling(List *dest_tlist, List *src_tlist)
{
	ListCell   *ld,
			   *ls;

	Assert(list_length(dest_tlist) == list_length(src_tlist));
	forboth(ld, dest_tlist, ls, src_tlist)
	{
		TargetEntry *dest_tle = (TargetEntry *) lfirst(ld);
		TargetEntry *src_tle = (TargetEntry *) lfirst(ls);

		Assert(dest_tle->resno == src_tle->resno);
		dest_tle->resname = src_tle->resname;
		dest_tle->ressortgroupref = src_tle->ressortgroupref;
		dest_tle->resorigtbl = src_tle->resorigtbl;
		dest_tle->resorigcol = src_tle->resorigcol;
		dest_tle->resjunk = src_tle->resjunk;
	}
}


/*
 * get_sortgroupref_tle
 *		Find the targetlist entry matching the given SortGroupRef index,
 *		and return it.
 */
TargetEntry *
get_sortgroupref_tle(Index sortref, List *targetList)
{
	ListCell   *l;

	foreach(l, targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->ressortgroupref == sortref)
			return tle;
	}

	elog(ERROR, "ORDER/GROUP BY expression not found in targetlist");
	return NULL;				/* keep compiler quiet */
}

/*
 * get_sortgroupclause_tle
 *		Find the targetlist entry matching the given SortGroupClause
 *		by ressortgroupref, and return it.
 */
TargetEntry *
get_sortgroupclause_tle(SortGroupClause *sgClause,
						List *targetList)
{
	return get_sortgroupref_tle(sgClause->tleSortGroupRef, targetList);
}

/*
 * get_sortgroupclause_expr
 *		Find the targetlist entry matching the given SortGroupClause
 *		by ressortgroupref, and return its expression.
 */
Node *
get_sortgroupclause_expr(SortGroupClause *sgClause, List *targetList)
{
	TargetEntry *tle = get_sortgroupclause_tle(sgClause, targetList);

	return (Node *) tle->expr;
}

/*
 * get_sortgrouplist_exprs
 *		Given a list of SortGroupClauses, build a list
 *		of the referenced targetlist expressions.
 */
List *
get_sortgrouplist_exprs(List *sgClauses, List *targetList)
{
	List	   *result = NIL;
	ListCell   *l;

	foreach(l, sgClauses)
	{
		SortGroupClause *sortcl = (SortGroupClause *) lfirst(l);
		Node	   *sortexpr;

		sortexpr = get_sortgroupclause_expr(sortcl, targetList);
		result = lappend(result, sortexpr);
	}
	return result;
}


/*****************************************************************************
 *		Functions to extract data from a list of SortGroupClauses
 *
 * These don't really belong in tlist.c, but they are sort of related to the
 * functions just above, and they don't seem to deserve their own file.
 *****************************************************************************/

/*
 * get_sortgroupref_clause
 *		Find the SortGroupClause matching the given SortGroupRef index,
 *		and return it.
 */
SortGroupClause *
get_sortgroupref_clause(Index sortref, List *clauses)
{
	ListCell   *l;

	foreach(l, clauses)
	{
		SortGroupClause *cl = (SortGroupClause *) lfirst(l);

		if (cl->tleSortGroupRef == sortref)
			return cl;
	}

	elog(ERROR, "ORDER/GROUP BY expression not found in list");
	return NULL;				/* keep compiler quiet */
}

/*
 * get_sortgroupref_clause_noerr
 *		As above, but return NULL rather than throwing an error if not found.
 */
SortGroupClause *
get_sortgroupref_clause_noerr(Index sortref, List *clauses)
{
	ListCell   *l;

	foreach(l, clauses)
	{
		SortGroupClause *cl = (SortGroupClause *) lfirst(l);

		if (cl->tleSortGroupRef == sortref)
			return cl;
	}

	return NULL;
}

/*
 * extract_grouping_ops - make an array of the equality operator OIDs
 *		for a SortGroupClause list
 */
Oid *
extract_grouping_ops(List *groupClause)
{
	int			numCols = list_length(groupClause);
	int			colno = 0;
	Oid		   *groupOperators;
	ListCell   *glitem;

	groupOperators = (Oid *) palloc(sizeof(Oid) * numCols);

	foreach(glitem, groupClause)
	{
		SortGroupClause *groupcl = (SortGroupClause *) lfirst(glitem);

		groupOperators[colno] = groupcl->eqop;
		Assert(OidIsValid(groupOperators[colno]));
		colno++;
	}

	return groupOperators;
}

/*
 * extract_grouping_cols - make an array of the grouping column resnos
 *		for a SortGroupClause list
 */
AttrNumber *
extract_grouping_cols(List *groupClause, List *tlist)
{
	AttrNumber *grpColIdx;
	int			numCols = list_length(groupClause);
	int			colno = 0;
	ListCell   *glitem;

	grpColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);

	foreach(glitem, groupClause)
	{
		SortGroupClause *groupcl = (SortGroupClause *) lfirst(glitem);
		TargetEntry *tle = get_sortgroupclause_tle(groupcl, tlist);

		grpColIdx[colno++] = tle->resno;
	}

	return grpColIdx;
}

/*
 * grouping_is_sortable - is it possible to implement grouping list by sorting?
 *
 * This is easy since the parser will have included a sortop if one exists.
 */
bool
grouping_is_sortable(List *groupClause)
{
	ListCell   *glitem;

	foreach(glitem, groupClause)
	{
		SortGroupClause *groupcl = (SortGroupClause *) lfirst(glitem);

		if (!OidIsValid(groupcl->sortop))
			return false;
	}
	return true;
}

/*
 * grouping_is_hashable - is it possible to implement grouping list by hashing?
 *
 * We rely on the parser to have set the hashable flag correctly.
 */
bool
grouping_is_hashable(List *groupClause)
{
	ListCell   *glitem;

	foreach(glitem, groupClause)
	{
		SortGroupClause *groupcl = (SortGroupClause *) lfirst(glitem);

		if (!groupcl->hashable)
			return false;
	}
	return true;
}


/*****************************************************************************
 *		PathTarget manipulation functions
 *
 * PathTarget is a somewhat stripped-down version of a full targetlist; it
 * omits all the TargetEntry decoration except (optionally) sortgroupref data,
 * and it adds evaluation cost and output data width info.
 *****************************************************************************/

/*
 * make_pathtarget_from_tlist
 *	  Construct a PathTarget equivalent to the given targetlist.
 *
 * This leaves the cost and width fields as zeroes.  Most callers will want
 * to use create_pathtarget(), so as to get those set.
 */
PathTarget *
make_pathtarget_from_tlist(List *tlist)
{
	PathTarget *target = makeNode(PathTarget);
	int			i;
	ListCell   *lc;

	target->sortgrouprefs = (Index *) palloc(list_length(tlist) * sizeof(Index));

	i = 0;
	foreach(lc, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		target->exprs = lappend(target->exprs, tle->expr);
		target->sortgrouprefs[i] = tle->ressortgroupref;
		i++;
	}

	return target;
}

/*
 * make_tlist_from_pathtarget
 *	  Construct a targetlist from a PathTarget.
 */
List *
make_tlist_from_pathtarget(PathTarget *target)
{
	List	   *tlist = NIL;
	int			i;
	ListCell   *lc;

	i = 0;
	foreach(lc, target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		TargetEntry *tle;

		tle = makeTargetEntry(expr,
							  i + 1,
							  NULL,
							  false);
		if (target->sortgrouprefs)
			tle->ressortgroupref = target->sortgrouprefs[i];
		tlist = lappend(tlist, tle);
		i++;
	}

	return tlist;
}

/*
 * copy_pathtarget
 *	  Copy a PathTarget.
 *
 * The new PathTarget has its own List cells, but shares the underlying
 * target expression trees with the old one.  We duplicate the List cells
 * so that items can be added to one target without damaging the other.
 */
PathTarget *
copy_pathtarget(PathTarget *src)
{
	PathTarget *dst = makeNode(PathTarget);

	/* Copy scalar fields */
	memcpy(dst, src, sizeof(PathTarget));
	/* Shallow-copy the expression list */
	dst->exprs = list_copy(src->exprs);
	/* Duplicate sortgrouprefs if any (if not, the memcpy handled this) */
	if (src->sortgrouprefs)
	{
		Size		nbytes = list_length(src->exprs) * sizeof(Index);

		dst->sortgrouprefs = (Index *) palloc(nbytes);
		memcpy(dst->sortgrouprefs, src->sortgrouprefs, nbytes);
	}
	return dst;
}

/*
 * create_empty_pathtarget
 *	  Create an empty (zero columns, zero cost) PathTarget.
 */
PathTarget *
create_empty_pathtarget(void)
{
	/* This is easy, but we don't want callers to hard-wire this ... */
	return makeNode(PathTarget);
}

/*
 * add_column_to_pathtarget
 *		Append a target column to the PathTarget.
 *
 * As with make_pathtarget_from_tlist, we leave it to the caller to update
 * the cost and width fields.
 */
void
add_column_to_pathtarget(PathTarget *target, Expr *expr, Index sortgroupref)
{
	/* Updating the exprs list is easy ... */
	target->exprs = lappend(target->exprs, expr);
	/* ... the sortgroupref data, a bit less so */
	if (target->sortgrouprefs)
	{
		int			nexprs = list_length(target->exprs);

		/* This might look inefficient, but actually it's usually cheap */
		target->sortgrouprefs = (Index *)
			repalloc(target->sortgrouprefs, nexprs * sizeof(Index));
		target->sortgrouprefs[nexprs - 1] = sortgroupref;
	}
	else if (sortgroupref)
	{
		/* Adding sortgroupref labeling to a previously unlabeled target */
		int			nexprs = list_length(target->exprs);

		target->sortgrouprefs = (Index *) palloc0(nexprs * sizeof(Index));
		target->sortgrouprefs[nexprs - 1] = sortgroupref;
	}
}

/*
 * add_new_column_to_pathtarget
 *		Append a target column to the PathTarget, but only if it's not
 *		equal() to any pre-existing target expression.
 *
 * The caller cannot specify a sortgroupref, since it would be unclear how
 * to merge that with a pre-existing column.
 *
 * As with make_pathtarget_from_tlist, we leave it to the caller to update
 * the cost and width fields.
 */
void
add_new_column_to_pathtarget(PathTarget *target, Expr *expr)
{
	if (!list_member(target->exprs, expr))
		add_column_to_pathtarget(target, expr, 0);
}

/*
 * add_new_columns_to_pathtarget
 *		Apply add_new_column_to_pathtarget() for each element of the list.
 */
void
add_new_columns_to_pathtarget(PathTarget *target, List *exprs)
{
	ListCell   *lc;

	foreach(lc, exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);

		add_new_column_to_pathtarget(target, expr);
	}
}

/*
 * apply_pathtarget_labeling_to_tlist
 *		Apply any sortgrouprefs in the PathTarget to matching tlist entries
 *
 * Here, we do not assume that the tlist entries are one-for-one with the
 * PathTarget.  The intended use of this function is to deal with cases
 * where createplan.c has decided to use some other tlist and we have
 * to identify what matches exist.
 */
void
apply_pathtarget_labeling_to_tlist(List *tlist, PathTarget *target)
{
	int			i;
	ListCell   *lc;

	/* Nothing to do if PathTarget has no sortgrouprefs data */
	if (target->sortgrouprefs == NULL)
		return;

	i = 0;
	foreach(lc, target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		TargetEntry *tle;

		if (target->sortgrouprefs[i])
		{
			/*
			 * For Vars, use tlist_member_match_var's weakened matching rule;
			 * this allows us to deal with some cases where a set-returning
			 * function has been inlined, so that we now have more knowledge
			 * about what it returns than we did when the original Var was
			 * created.  Otherwise, use regular equal() to see if there's a
			 * matching TLE.  (In current usage, only the Var case is actually
			 * needed; but it seems best to have sane behavior here for
			 * non-Vars too.)
			 */
			if (expr && IsA(expr, Var))
				tle = tlist_member_match_var((Var *) expr, tlist);
			else
				tle = tlist_member((Node *) expr, tlist);
			if (tle)
				tle->ressortgroupref = target->sortgrouprefs[i];
		}
		i++;
	}
}
