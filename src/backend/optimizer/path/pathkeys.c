/*-------------------------------------------------------------------------
 *
 * pathkeys.c
 *	  Utilities for matching and building path keys
 *
 * See src/backend/optimizer/README for a great deal of information about
 * the nature and use of path keys.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/path/pathkeys.c,v 1.27 2000/11/12 00:36:58 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "parser/parse_func.h"
#include "utils/lsyscache.h"


static PathKeyItem *makePathKeyItem(Node *key, Oid sortop);
static List *make_canonical_pathkey(Query *root, PathKeyItem *item);
static Var *find_indexkey_var(Query *root, RelOptInfo *rel,
							  AttrNumber varattno);


/*
 * makePathKeyItem
 *		create a PathKeyItem node
 */
static PathKeyItem *
makePathKeyItem(Node *key, Oid sortop)
{
	PathKeyItem *item = makeNode(PathKeyItem);

	item->key = key;
	item->sortop = sortop;
	return item;
}

/*
 * add_equijoined_keys
 *	  The given clause has a mergejoinable operator, so its two sides
 *	  can be considered equal after restriction clause application; in
 *	  particular, any pathkey mentioning one side (with the correct sortop)
 *	  can be expanded to include the other as well.  Record the vars and
 *	  associated sortops in the query's equi_key_list for future use.
 *
 * The query's equi_key_list field points to a list of sublists of PathKeyItem
 * nodes, where each sublist is a set of two or more vars+sortops that have
 * been identified as logically equivalent (and, therefore, we may consider
 * any two in a set to be equal).  As described above, we will subsequently
 * use direct pointers to one of these sublists to represent any pathkey
 * that involves an equijoined variable.
 *
 * This code would actually work fine with expressions more complex than
 * a single Var, but currently it won't see any because check_mergejoinable
 * won't accept such clauses as mergejoinable.
 */
void
add_equijoined_keys(Query *root, RestrictInfo *restrictinfo)
{
	Expr	   *clause = restrictinfo->clause;
	PathKeyItem *item1 = makePathKeyItem((Node *) get_leftop(clause),
										 restrictinfo->left_sortop);
	PathKeyItem *item2 = makePathKeyItem((Node *) get_rightop(clause),
										 restrictinfo->right_sortop);
	List	   *newset,
			   *cursetlink;

	/* We might see a clause X=X; don't make a single-element list from it */
	if (equal(item1, item2))
		return;

	/*
	 * Our plan is to make a two-element set, then sweep through the
	 * existing equijoin sets looking for matches to item1 or item2.  When
	 * we find one, we remove that set from equi_key_list and union it
	 * into our new set. When done, we add the new set to the front of
	 * equi_key_list.
	 *
	 * It may well be that the two items we're given are already known to
	 * be equijoin-equivalent, in which case we don't need to change our
	 * data structure.  If we find both of them in the same equivalence
	 * set to start with, we can quit immediately.
	 *
	 * This is a standard UNION-FIND problem, for which there exist better
	 * data structures than simple lists.  If this code ever proves to be
	 * a bottleneck then it could be sped up --- but for now, simple is
	 * beautiful.
	 */
	newset = NIL;

	foreach(cursetlink, root->equi_key_list)
	{
		List	   *curset = lfirst(cursetlink);
		bool		item1here = member(item1, curset);
		bool		item2here = member(item2, curset);

		if (item1here || item2here)
		{
			/* If find both in same equivalence set, no need to do any more */
			if (item1here && item2here)
			{
				/* Better not have seen only one in an earlier set... */
				Assert(newset == NIL);
				return;
			}

			/* Build the new set only when we know we must */
			if (newset == NIL)
				newset = lcons(item1, lcons(item2, NIL));

			/* Found a set to merge into our new set */
			newset = set_union(newset, curset);

			/*
			 * Remove old set from equi_key_list.  NOTE this does not
			 * change lnext(cursetlink), so the foreach loop doesn't break.
			 */
			root->equi_key_list = lremove(curset, root->equi_key_list);
			freeList(curset);	/* might as well recycle old cons cells */
		}
	}

	/* Build the new set only when we know we must */
	if (newset == NIL)
		newset = lcons(item1, lcons(item2, NIL));

	root->equi_key_list = lcons(newset, root->equi_key_list);
}

/*
 * generate_implied_equalities
 *	  Scan the completed equi_key_list for the query, and generate explicit
 *	  qualifications (WHERE clauses) for all the pairwise equalities not
 *	  already mentioned in the quals.  This is useful because the additional
 *	  clauses help the selectivity-estimation code, and in fact it's
 *	  *necessary* to ensure that sort keys we think are equivalent really
 *	  are (see src/backend/optimizer/README for more info).
 *
 * This routine just walks the equi_key_list to find all pairwise equalities.
 * We call process_implied_equality (in plan/initsplan.c) to determine whether
 * each is already known and add it to the proper restrictinfo list if not.
 */
void
generate_implied_equalities(Query *root)
{
	List	   *cursetlink;

	foreach(cursetlink, root->equi_key_list)
	{
		List	   *curset = lfirst(cursetlink);
		List	   *ptr1;

		/*
		 * A set containing only two items cannot imply any equalities
		 * beyond the one that created the set, so we can skip it.
		 */
		if (length(curset) < 3)
			continue;

		/*
		 * Match each item in the set with all that appear after it
		 * (it's sufficient to generate A=B, need not process B=A too).
		 */
		foreach(ptr1, curset)
		{
			PathKeyItem *item1 = (PathKeyItem *) lfirst(ptr1);
			List	   *ptr2;

			foreach(ptr2, lnext(ptr1))
			{
				PathKeyItem *item2 = (PathKeyItem *) lfirst(ptr2);

				process_implied_equality(root, item1->key, item2->key,
										 item1->sortop, item2->sortop);
			}
		}
	}
}

/*
 * make_canonical_pathkey
 *	  Given a PathKeyItem, find the equi_key_list subset it is a member of,
 *	  if any.  If so, return a pointer to that sublist, which is the
 *	  canonical representation (for this query) of that PathKeyItem's
 *	  equivalence set.	If it is not found, return a single-element list
 *	  containing the PathKeyItem (when the item has no equivalence peers,
 *	  we just allow it to be a standalone list).
 *
 * Note that this function must not be used until after we have completed
 * scanning the WHERE clause for equijoin operators.
 */
static List *
make_canonical_pathkey(Query *root, PathKeyItem *item)
{
	List	   *cursetlink;

	foreach(cursetlink, root->equi_key_list)
	{
		List	   *curset = lfirst(cursetlink);

		if (member(item, curset))
			return curset;
	}
	return lcons(item, NIL);
}

/*
 * canonicalize_pathkeys
 *	   Convert a not-necessarily-canonical pathkeys list to canonical form.
 *
 * Note that this function must not be used until after we have completed
 * scanning the WHERE clause for equijoin operators.
 */
List *
canonicalize_pathkeys(Query *root, List *pathkeys)
{
	List	   *new_pathkeys = NIL;
	List	   *i;

	foreach(i, pathkeys)
	{
		List	   *pathkey = (List *) lfirst(i);
		PathKeyItem *item;

		/*
		 * It's sufficient to look at the first entry in the sublist; if
		 * there are more entries, they're already part of an equivalence
		 * set by definition.
		 */
		Assert(pathkey != NIL);
		item = (PathKeyItem *) lfirst(pathkey);
		new_pathkeys = lappend(new_pathkeys,
							   make_canonical_pathkey(root, item));
	}
	return new_pathkeys;
}

/****************************************************************************
 *		PATHKEY COMPARISONS
 ****************************************************************************/

/*
 * compare_pathkeys
 *	  Compare two pathkeys to see if they are equivalent, and if not whether
 *	  one is "better" than the other.
 *
 *	  A pathkey can be considered better than another if it is a superset:
 *	  it contains all the keys of the other plus more.	For example, either
 *	  ((A) (B)) or ((A B)) is better than ((A)).
 *
 *	  Because we actually only expect to see canonicalized pathkey sublists,
 *	  we don't have to do the full two-way-subset-inclusion test on each
 *	  pair of sublists that is implied by the above statement.	Instead we
 *	  just do an equal().  In the normal case where multi-element sublists
 *	  are pointers into the root's equi_key_list, equal() will be very fast:
 *	  it will recognize pointer equality when the sublists are the same,
 *	  and will fail at the first sublist element when they are not.
 *
 * Yes, this gets called enough to be worth coding it this tensely.
 */
PathKeysComparison
compare_pathkeys(List *keys1, List *keys2)
{
	List	   *key1,
			   *key2;

	for (key1 = keys1, key2 = keys2;
		 key1 != NIL && key2 != NIL;
		 key1 = lnext(key1), key2 = lnext(key2))
	{
		List	   *subkey1 = lfirst(key1);
		List	   *subkey2 = lfirst(key2);

		/*
		 * We will never have two subkeys where one is a subset of the
		 * other, because of the canonicalization explained above.	Either
		 * they are equal or they ain't.
		 */
		if (!equal(subkey1, subkey2))
			return PATHKEYS_DIFFERENT;	/* no need to keep looking */
	}

	/*
	 * If we reached the end of only one list, the other is longer and
	 * therefore not a subset.	(We assume the additional sublist(s) of
	 * the other list are not NIL --- no pathkey list should ever have a
	 * NIL sublist.)
	 */
	if (key1 == NIL && key2 == NIL)
		return PATHKEYS_EQUAL;
	if (key1 != NIL)
		return PATHKEYS_BETTER1;/* key1 is longer */
	return PATHKEYS_BETTER2;	/* key2 is longer */
}

/*
 * pathkeys_contained_in
 *	  Common special case of compare_pathkeys: we just want to know
 *	  if keys2 are at least as well sorted as keys1.
 */
bool
pathkeys_contained_in(List *keys1, List *keys2)
{
	switch (compare_pathkeys(keys1, keys2))
	{
			case PATHKEYS_EQUAL:
			case PATHKEYS_BETTER2:
			return true;
		default:
			break;
	}
	return false;
}

/*
 * get_cheapest_path_for_pathkeys
 *	  Find the cheapest path (according to the specified criterion) that
 *	  satisfies the given pathkeys.  Return NULL if no such path.
 *
 * 'paths' is a list of possible paths that all generate the same relation
 * 'pathkeys' represents a required ordering (already canonicalized!)
 * 'cost_criterion' is STARTUP_COST or TOTAL_COST
 */
Path *
get_cheapest_path_for_pathkeys(List *paths, List *pathkeys,
							   CostSelector cost_criterion)
{
	Path	   *matched_path = NULL;
	List	   *i;

	foreach(i, paths)
	{
		Path	   *path = (Path *) lfirst(i);

		/*
		 * Since cost comparison is a lot cheaper than pathkey comparison,
		 * do that first.  (XXX is that still true?)
		 */
		if (matched_path != NULL &&
			compare_path_costs(matched_path, path, cost_criterion) <= 0)
			continue;

		if (pathkeys_contained_in(pathkeys, path->pathkeys))
			matched_path = path;
	}
	return matched_path;
}

/*
 * get_cheapest_fractional_path_for_pathkeys
 *	  Find the cheapest path (for retrieving a specified fraction of all
 *	  the tuples) that satisfies the given pathkeys.
 *	  Return NULL if no such path.
 *
 * See compare_fractional_path_costs() for the interpretation of the fraction
 * parameter.
 *
 * 'paths' is a list of possible paths that all generate the same relation
 * 'pathkeys' represents a required ordering (already canonicalized!)
 * 'fraction' is the fraction of the total tuples expected to be retrieved
 */
Path *
get_cheapest_fractional_path_for_pathkeys(List *paths,
										  List *pathkeys,
										  double fraction)
{
	Path	   *matched_path = NULL;
	List	   *i;

	foreach(i, paths)
	{
		Path	   *path = (Path *) lfirst(i);

		/*
		 * Since cost comparison is a lot cheaper than pathkey comparison,
		 * do that first.
		 */
		if (matched_path != NULL &&
		compare_fractional_path_costs(matched_path, path, fraction) <= 0)
			continue;

		if (pathkeys_contained_in(pathkeys, path->pathkeys))
			matched_path = path;
	}
	return matched_path;
}

/****************************************************************************
 *		NEW PATHKEY FORMATION
 ****************************************************************************/

/*
 * build_index_pathkeys
 *	  Build a pathkeys list that describes the ordering induced by an index
 *	  scan using the given index.  (Note that an unordered index doesn't
 *	  induce any ordering; such an index will have no sortop OIDS in
 *	  its "ordering" field, and we will return NIL.)
 *
 * If 'scandir' is BackwardScanDirection, attempt to build pathkeys
 * representing a backwards scan of the index.	Return NIL if can't do it.
 */
List *
build_index_pathkeys(Query *root,
					 RelOptInfo *rel,
					 IndexOptInfo *index,
					 ScanDirection scandir)
{
	List	   *retval = NIL;
	int		   *indexkeys = index->indexkeys;
	Oid		   *ordering = index->ordering;
	PathKeyItem *item;
	Oid			sortop;

	if (!indexkeys || indexkeys[0] == 0 ||
		!ordering || ordering[0] == InvalidOid)
		return NIL;				/* unordered index? */

	if (index->indproc)
	{
		/* Functional index: build a representation of the function call */
		Func	   *funcnode = makeNode(Func);
		List	   *funcargs = NIL;

		funcnode->funcid = index->indproc;
		funcnode->functype = get_func_rettype(index->indproc);
		funcnode->func_fcache = NULL;

		while (*indexkeys != 0)
		{
			funcargs = lappend(funcargs,
							   find_indexkey_var(root, rel, *indexkeys));
			indexkeys++;
		}

		sortop = *ordering;
		if (ScanDirectionIsBackward(scandir))
		{
			sortop = get_commutator(sortop);
			if (sortop == InvalidOid)
				return NIL;		/* oops, no reverse sort operator? */
		}

		/* Make a one-sublist pathkeys list for the function expression */
		item = makePathKeyItem((Node *) make_funcclause(funcnode, funcargs),
							   sortop);
		retval = lcons(make_canonical_pathkey(root, item), NIL);
	}
	else
	{
		/* Normal non-functional index */
		while (*indexkeys != 0 && *ordering != InvalidOid)
		{
			Var		   *relvar = find_indexkey_var(root, rel, *indexkeys);

			sortop = *ordering;
			if (ScanDirectionIsBackward(scandir))
			{
				sortop = get_commutator(sortop);
				if (sortop == InvalidOid)
					break;		/* oops, no reverse sort operator? */
			}

			/* OK, make a sublist for this sort key */
			item = makePathKeyItem((Node *) relvar, sortop);
			retval = lappend(retval, make_canonical_pathkey(root, item));

			indexkeys++;
			ordering++;
		}
	}

	return retval;
}

/*
 * Find or make a Var node for the specified attribute of the rel.
 *
 * We first look for the var in the rel's target list, because that's
 * easy and fast.  But the var might not be there (this should normally
 * only happen for vars that are used in WHERE restriction clauses,
 * but not in join clauses or in the SELECT target list).  In that case,
 * gin up a Var node the hard way.
 */
static Var *
find_indexkey_var(Query *root, RelOptInfo *rel, AttrNumber varattno)
{
	List	   *temp;
	int			relid;
	Oid			reloid,
				vartypeid;
	int32		type_mod;

	foreach(temp, rel->targetlist)
	{
		Var		   *tle_var = get_expr(lfirst(temp));

		if (IsA(tle_var, Var) &&tle_var->varattno == varattno)
			return tle_var;
	}

	relid = lfirsti(rel->relids);
	reloid = getrelid(relid, root->rtable);
	vartypeid = get_atttype(reloid, varattno);
	type_mod = get_atttypmod(reloid, varattno);

	return makeVar(relid, varattno, vartypeid, type_mod, 0);
}

/*
 * build_join_pathkeys
 *	  Build the path keys for a join relation constructed by mergejoin or
 *	  nestloop join.  These keys should include all the path key vars of the
 *	  outer path (since the join will retain the ordering of the outer path)
 *	  plus any vars of the inner path that are equijoined to the outer vars.
 *
 *	  Per the discussion at the top of this file, equijoined inner vars
 *	  can be considered path keys of the result, just the same as the outer
 *	  vars they were joined with; furthermore, it doesn't matter what kind
 *	  of join algorithm is actually used.
 *
 * 'outer_pathkeys' is the list of the outer path's path keys
 * 'join_rel_tlist' is the target list of the join relation
 * 'equi_key_list' is the query's list of pathkeyitem equivalence sets
 *
 * Returns the list of new path keys.
 */
List *
build_join_pathkeys(List *outer_pathkeys,
					List *join_rel_tlist,
					List *equi_key_list)
{

	/*
	 * This used to be quite a complex bit of code, but now that all
	 * pathkey sublists start out life canonicalized, we don't have to do
	 * a darn thing here!  The inner-rel vars we used to need to add are
	 * *already* part of the outer pathkey!
	 *
	 * I'd remove the routine entirely, but maybe someday we'll need it...
	 */
	return outer_pathkeys;
}

/****************************************************************************
 *		PATHKEYS AND SORT CLAUSES
 ****************************************************************************/

/*
 * make_pathkeys_for_sortclauses
 *		Generate a pathkeys list that represents the sort order specified
 *		by a list of SortClauses (GroupClauses will work too!)
 *
 * NB: the result is NOT in canonical form, but must be passed through
 * canonicalize_pathkeys() before it can be used for comparisons or
 * labeling relation sort orders.  (We do things this way because
 * grouping_planner needs to be able to construct requested pathkeys
 * before the pathkey equivalence sets have been created for the query.)
 *
 * 'sortclauses' is a list of SortClause or GroupClause nodes
 * 'tlist' is the targetlist to find the referenced tlist entries in
 */
List *
make_pathkeys_for_sortclauses(List *sortclauses,
							  List *tlist)
{
	List	   *pathkeys = NIL;
	List	   *i;

	foreach(i, sortclauses)
	{
		SortClause *sortcl = (SortClause *) lfirst(i);
		Node	   *sortkey;
		PathKeyItem *pathkey;

		sortkey = get_sortgroupclause_expr(sortcl, tlist);
		pathkey = makePathKeyItem(sortkey, sortcl->sortop);

		/*
		 * The pathkey becomes a one-element sublist, for now;
		 * canonicalize_pathkeys() might replace it with a longer sublist
		 * later.
		 */
		pathkeys = lappend(pathkeys, lcons(pathkey, NIL));
	}
	return pathkeys;
}

/****************************************************************************
 *		PATHKEYS AND MERGECLAUSES
 ****************************************************************************/

/*
 * find_mergeclauses_for_pathkeys
 *	  This routine attempts to find a set of mergeclauses that can be
 *	  used with a specified ordering for one of the input relations.
 *	  If successful, it returns a list of mergeclauses.
 *
 * 'pathkeys' is a pathkeys list showing the ordering of an input path.
 *			It doesn't matter whether it is for the inner or outer path.
 * 'restrictinfos' is a list of mergejoinable restriction clauses for the
 *			join relation being formed.
 *
 * The result is NIL if no merge can be done, else a maximal list of
 * usable mergeclauses (represented as a list of their restrictinfo nodes).
 *
 * XXX Ideally we ought to be considering context, ie what path orderings
 * are available on the other side of the join, rather than just making
 * an arbitrary choice among the mergeclause orders that will work for
 * this side of the join.
 */
List *
find_mergeclauses_for_pathkeys(List *pathkeys, List *restrictinfos)
{
	List	   *mergeclauses = NIL;
	List	   *i;

	foreach(i, pathkeys)
	{
		List	   *pathkey = lfirst(i);
		RestrictInfo *matched_restrictinfo = NULL;
		List	   *j;

		/*
		 * We can match any of the keys in this pathkey sublist, since
		 * they're all equivalent.  And we can match against either left
		 * or right side of any mergejoin clause we haven't used yet.  For
		 * the moment we use a dumb "greedy" algorithm with no
		 * backtracking.  Is it worth being any smarter to make a longer
		 * list of usable mergeclauses?  Probably not.
		 */
		foreach(j, pathkey)
		{
			PathKeyItem *keyitem = lfirst(j);
			Node	   *key = keyitem->key;
			Oid			keyop = keyitem->sortop;
			List	   *k;

			foreach(k, restrictinfos)
			{
				RestrictInfo *restrictinfo = lfirst(k);

				Assert(restrictinfo->mergejoinoperator != InvalidOid);

				if (((keyop == restrictinfo->left_sortop &&
					  equal(key, get_leftop(restrictinfo->clause))) ||
					 (keyop == restrictinfo->right_sortop &&
					  equal(key, get_rightop(restrictinfo->clause)))) &&
					!member(restrictinfo, mergeclauses))
				{
					matched_restrictinfo = restrictinfo;
					break;
				}
			}
			if (matched_restrictinfo)
				break;
		}

		/*
		 * If we didn't find a mergeclause, we're done --- any additional
		 * sort-key positions in the pathkeys are useless.	(But we can
		 * still mergejoin if we found at least one mergeclause.)
		 */
		if (!matched_restrictinfo)
			break;

		/*
		 * If we did find a usable mergeclause for this sort-key position,
		 * add it to result list.
		 */
		mergeclauses = lappend(mergeclauses, matched_restrictinfo);
	}

	return mergeclauses;
}

/*
 * make_pathkeys_for_mergeclauses
 *	  Builds a pathkey list representing the explicit sort order that
 *	  must be applied to a path in order to make it usable for the
 *	  given mergeclauses.
 *
 * 'mergeclauses' is a list of RestrictInfos for mergejoin clauses
 *			that will be used in a merge join.
 * 'rel' is the relation the pathkeys will apply to (ie, either the inner
 *			or outer side of the proposed join rel).
 *
 * Returns a pathkeys list that can be applied to the indicated relation.
 *
 * Note that it is not this routine's job to decide whether sorting is
 * actually needed for a particular input path.  Assume a sort is necessary;
 * just make the keys, eh?
 */
List *
make_pathkeys_for_mergeclauses(Query *root,
							   List *mergeclauses,
							   RelOptInfo *rel)
{
	List	   *pathkeys = NIL;
	List	   *i;

	foreach(i, mergeclauses)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(i);
		Node	   *key;
		Oid			sortop;
		PathKeyItem *item;
		List	   *pathkey;

		Assert(restrictinfo->mergejoinoperator != InvalidOid);

		/*
		 * Which key and sortop is needed for this relation?
		 */
		key = (Node *) get_leftop(restrictinfo->clause);
		sortop = restrictinfo->left_sortop;
		if (!IsA(key, Var) ||
			!intMember(((Var *) key)->varno, rel->relids))
		{
			key = (Node *) get_rightop(restrictinfo->clause);
			sortop = restrictinfo->right_sortop;
			if (!IsA(key, Var) ||
				!intMember(((Var *) key)->varno, rel->relids))
				elog(ERROR, "make_pathkeys_for_mergeclauses: can't identify which side of mergeclause to use");
		}

		/*
		 * Find or create canonical pathkey sublist for this sort item.
		 */
		item = makePathKeyItem(key, sortop);
		pathkey = make_canonical_pathkey(root, item);

		/*
		 * Most of the time we will get back a canonical pathkey set
		 * including both the mergeclause's left and right sides (the only
		 * case where we don't is if the mergeclause appeared in an OUTER
		 * JOIN, which causes us not to generate an equijoin set from it).
		 * Therefore, most of the time the item we just made is not part
		 * of the returned structure, and we can free it.  This check
		 * saves a useful amount of storage in a big join tree.
		 */
		if (item != (PathKeyItem *) lfirst(pathkey))
			pfree(item);

		pathkeys = lappend(pathkeys, pathkey);
	}

	return pathkeys;
}
