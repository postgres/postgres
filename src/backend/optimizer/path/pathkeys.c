/*-------------------------------------------------------------------------
 *
 * pathkeys.c
 *	  Utilities for matching and building path keys
 *
 * See src/backend/optimizer/README for a great deal of information about
 * the nature and use of path keys.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/path/pathkeys.c,v 1.53.4.2 2005/01/23 02:23:16 tgl Exp $
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
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"


static PathKeyItem *makePathKeyItem(Node *key, Oid sortop, bool checkType);
static List *make_canonical_pathkey(Query *root, PathKeyItem *item);
static Var *find_indexkey_var(Query *root, RelOptInfo *rel,
				  AttrNumber varattno);


/*
 * makePathKeyItem
 *		create a PathKeyItem node
 */
static PathKeyItem *
makePathKeyItem(Node *key, Oid sortop, bool checkType)
{
	PathKeyItem *item = makeNode(PathKeyItem);

	/*
	 * Some callers pass expressions that are not necessarily of the same
	 * type as the sort operator expects as input (for example when dealing
	 * with an index that uses binary-compatible operators).  We must relabel
	 * these with the correct type so that the key expressions will be seen
	 * as equal() to expressions that have been correctly labeled.
	 */
	if (checkType)
	{
		Oid			lefttype,
					righttype;

		op_input_types(sortop, &lefttype, &righttype);
		if (exprType(key) != lefttype)
			key = (Node *) makeRelabelType((Expr *) key,
										   lefttype, -1,
										   COERCE_DONTCARE);
	}

	item->key = key;
	item->sortop = sortop;
	return item;
}

/*
 * add_equijoined_keys
 *	  The given clause has a mergejoinable operator, so its two sides
 *	  can be considered equal after restriction clause application; in
 *	  particular, any pathkey mentioning one side (with the correct sortop)
 *	  can be expanded to include the other as well.  Record the exprs and
 *	  associated sortops in the query's equi_key_list for future use.
 *
 * The query's equi_key_list field points to a list of sublists of PathKeyItem
 * nodes, where each sublist is a set of two or more exprs+sortops that have
 * been identified as logically equivalent (and, therefore, we may consider
 * any two in a set to be equal).  As described above, we will subsequently
 * use direct pointers to one of these sublists to represent any pathkey
 * that involves an equijoined variable.
 */
void
add_equijoined_keys(Query *root, RestrictInfo *restrictinfo)
{
	Expr	   *clause = restrictinfo->clause;
	PathKeyItem *item1 = makePathKeyItem(get_leftop(clause),
										 restrictinfo->left_sortop,
										 false);
	PathKeyItem *item2 = makePathKeyItem(get_rightop(clause),
										 restrictinfo->right_sortop,
										 false);
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
	 * It may well be that the two items we're given are already known to be
	 * equijoin-equivalent, in which case we don't need to change our data
	 * structure.  If we find both of them in the same equivalence set to
	 * start with, we can quit immediately.
	 *
	 * This is a standard UNION-FIND problem, for which there exist better
	 * data structures than simple lists.  If this code ever proves to be
	 * a bottleneck then it could be sped up --- but for now, simple is
	 * beautiful.
	 */
	newset = NIL;

	/* cannot use foreach here because of possible lremove */
	cursetlink = root->equi_key_list;
	while (cursetlink)
	{
		List	   *curset = lfirst(cursetlink);
		bool		item1here = member(item1, curset);
		bool		item2here = member(item2, curset);

		/* must advance cursetlink before lremove possibly pfree's it */
		cursetlink = lnext(cursetlink);

		if (item1here || item2here)
		{
			/*
			 * If find both in same equivalence set, no need to do any
			 * more
			 */
			if (item1here && item2here)
			{
				/* Better not have seen only one in an earlier set... */
				Assert(newset == NIL);
				return;
			}

			/* Build the new set only when we know we must */
			if (newset == NIL)
				newset = makeList2(item1, item2);

			/* Found a set to merge into our new set */
			newset = set_union(newset, curset);

			/*
			 * Remove old set from equi_key_list.
			 */
			root->equi_key_list = lremove(curset, root->equi_key_list);
			freeList(curset);	/* might as well recycle old cons cells */
		}
	}

	/* Build the new set only when we know we must */
	if (newset == NIL)
		newset = makeList2(item1, item2);

	root->equi_key_list = lcons(newset, root->equi_key_list);
}

/*
 * generate_implied_equalities
 *	  Scan the completed equi_key_list for the query, and generate explicit
 *	  qualifications (WHERE clauses) for all the pairwise equalities not
 *	  already mentioned in the quals; or remove qualifications found to be
 *	  redundant.
 *
 * Adding deduced equalities is useful because the additional clauses help
 * the selectivity-estimation code and may allow better joins to be chosen;
 * and in fact it's *necessary* to ensure that sort keys we think are
 * equivalent really are (see src/backend/optimizer/README for more info).
 *
 * If an equi_key_list set includes any constants then we adopt a different
 * strategy: we record all the "var = const" deductions we can make, and
 * actively remove all the "var = var" clauses that are implied by the set
 * (including the clauses that originally gave rise to the set!).  The reason
 * is that given input like "a = b AND b = 42", once we have deduced "a = 42"
 * there is no longer any need to apply the clause "a = b"; not only is
 * it a waste of time to check it, but we will misestimate selectivity if the
 * clause is left in.  So we must remove it.  For this purpose, any pathkey
 * item that mentions no Vars of the current level can be taken as a constant.
 * (The only case where this would be risky is if the item contains volatile
 * functions; but we will never consider such an expression to be a pathkey
 * at all, because check_mergejoinable() will reject it.)
 *
 * This routine just walks the equi_key_list to find all pairwise equalities.
 * We call process_implied_equality (in plan/initsplan.c) to adjust the
 * restrictinfo datastructures for each pair.
 */
void
generate_implied_equalities(Query *root)
{
	List	   *cursetlink;

	foreach(cursetlink, root->equi_key_list)
	{
		List	   *curset = lfirst(cursetlink);
		int			nitems = length(curset);
		Relids	   *relids;
		bool		have_consts;
		List	   *ptr1;
		int			i1;

		/*
		 * A set containing only two items cannot imply any equalities
		 * beyond the one that created the set, so we can skip it.
		 */
		if (nitems < 3)
			continue;

		/*
		 * Collect info about relids mentioned in each item.  For this
		 * routine we only really care whether there are any at all in
		 * each item, but process_implied_equality() needs the exact sets,
		 * so we may as well pull them here.
		 */
		relids = (Relids *) palloc(nitems * sizeof(Relids));
		have_consts = false;
		i1 = 0;
		foreach(ptr1, curset)
		{
			PathKeyItem *item1 = (PathKeyItem *) lfirst(ptr1);

			relids[i1] = pull_varnos(item1->key);
			if (bms_is_empty(relids[i1]))
				have_consts = true;
			i1++;
		}

		/*
		 * Match each item in the set with all that appear after it (it's
		 * sufficient to generate A=B, need not process B=A too).
		 */
		i1 = 0;
		foreach(ptr1, curset)
		{
			PathKeyItem *item1 = (PathKeyItem *) lfirst(ptr1);
			bool		i1_is_variable = !bms_is_empty(relids[i1]);
			List	   *ptr2;
			int			i2 = i1 + 1;

			foreach(ptr2, lnext(ptr1))
			{
				PathKeyItem *item2 = (PathKeyItem *) lfirst(ptr2);
				bool		i2_is_variable = !bms_is_empty(relids[i2]);

				/*
				 * If it's "const = const" then just ignore it altogether.
				 * There is no place in the restrictinfo structure to
				 * store it.  (If the two consts are in fact unequal, then
				 * propagating the comparison to Vars will cause us to
				 * produce zero rows out, as expected.)
				 */
				if (i1_is_variable || i2_is_variable)
				{
					/*
					 * Tell process_implied_equality to delete the clause,
					 * not add it, if it's "var = var" and we have
					 * constants present in the list.
					 */
					bool		delete_it = (have_consts &&
											 i1_is_variable &&
											 i2_is_variable);

					process_implied_equality(root,
											 item1->key, item2->key,
											 item1->sortop, item2->sortop,
											 relids[i1], relids[i2],
											 delete_it);
				}
				i2++;
			}
			i1++;
		}
	}
}

/*
 * exprs_known_equal
 *	  Detect whether two expressions are known equal due to equijoin clauses.
 *
 * Note: does not bother to check for "equal(item1, item2)"; caller must
 * check that case if it's possible to pass identical items.
 */
bool
exprs_known_equal(Query *root, Node *item1, Node *item2)
{
	List	   *cursetlink;

	foreach(cursetlink, root->equi_key_list)
	{
		List	   *curset = lfirst(cursetlink);
		bool		item1member = false;
		bool		item2member = false;
		List	   *ptr;

		foreach(ptr, curset)
		{
			PathKeyItem *pitem = (PathKeyItem *) lfirst(ptr);

			if (equal(item1, pitem->key))
				item1member = true;
			else if (equal(item2, pitem->key))
				item2member = true;
			/* Exit as soon as equality is proven */
			if (item1member && item2member)
				return true;
		}
	}
	return false;
}


/*
 * make_canonical_pathkey
 *	  Given a PathKeyItem, find the equi_key_list subset it is a member of,
 *	  if any.  If so, return a pointer to that sublist, which is the
 *	  canonical representation (for this query) of that PathKeyItem's
 *	  equivalence set.	If it is not found, add a singleton "equivalence set"
 *	  to the equi_key_list and return that --- see compare_pathkeys.
 *
 * Note that this function must not be used until after we have completed
 * scanning the WHERE clause for equijoin operators.
 */
static List *
make_canonical_pathkey(Query *root, PathKeyItem *item)
{
	List	   *cursetlink;
	List	   *newset;

	foreach(cursetlink, root->equi_key_list)
	{
		List	   *curset = lfirst(cursetlink);

		if (member(item, curset))
			return curset;
	}
	newset = makeList1(item);
	root->equi_key_list = lcons(newset, root->equi_key_list);
	return newset;
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
		List	   *cpathkey;

		/*
		 * It's sufficient to look at the first entry in the sublist; if
		 * there are more entries, they're already part of an equivalence
		 * set by definition.
		 */
		Assert(pathkey != NIL);
		item = (PathKeyItem *) lfirst(pathkey);
		cpathkey = make_canonical_pathkey(root, item);

		/*
		 * Eliminate redundant ordering requests --- ORDER BY A,A is the
		 * same as ORDER BY A.	We want to check this only after we have
		 * canonicalized the keys, so that equivalent-key knowledge is
		 * used when deciding if an item is redundant.
		 */
		if (!ptrMember(cpathkey, new_pathkeys))
			new_pathkeys = lappend(new_pathkeys, cpathkey);
	}
	return new_pathkeys;
}


/*
 * count_canonical_peers
 *	  Given a PathKeyItem, find the equi_key_list subset it is a member of,
 *	  if any.  If so, return the number of other members of the set.
 *	  If not, return 0 (without actually adding it to our equi_key_list).
 *
 * This is a hack to support the rather bogus heuristics in
 * build_subquery_pathkeys.
 */
static int
count_canonical_peers(Query *root, PathKeyItem *item)
{
	List	   *cursetlink;

	foreach(cursetlink, root->equi_key_list)
	{
		List	   *curset = lfirst(cursetlink);

		if (member(item, curset))
			return length(curset) - 1;
	}
	return 0;
}

/****************************************************************************
 *		PATHKEY COMPARISONS
 ****************************************************************************/

/*
 * compare_pathkeys
 *	  Compare two pathkeys to see if they are equivalent, and if not whether
 *	  one is "better" than the other.
 *
 *	  This function may only be applied to canonicalized pathkey lists.
 *	  In the canonical representation, sublists can be checked for equality
 *	  by simple pointer comparison.
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
		 * XXX would like to check that we've been given canonicalized
		 * input, but query root not accessible here...
		 */
#ifdef NOT_USED
		Assert(ptrMember(subkey1, root->equi_key_list));
		Assert(ptrMember(subkey2, root->equi_key_list));
#endif

		/*
		 * We will never have two subkeys where one is a subset of the
		 * other, because of the canonicalization process.	Either they
		 * are equal or they ain't.  Furthermore, we only need pointer
		 * comparison to detect equality.
		 */
		if (subkey1 != subkey2)
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
		return PATHKEYS_BETTER1;	/* key1 is longer */
	return PATHKEYS_BETTER2;	/* key2 is longer */
}

/*
 * compare_noncanonical_pathkeys
 *	  Compare two pathkeys to see if they are equivalent, and if not whether
 *	  one is "better" than the other.  This is used when we must compare
 *	  non-canonicalized pathkeys.
 *
 *	  A pathkey can be considered better than another if it is a superset:
 *	  it contains all the keys of the other plus more.	For example, either
 *	  ((A) (B)) or ((A B)) is better than ((A)).
 *
 *	  Currently, the only user of this routine is grouping_planner(),
 *	  and it will only pass single-element sublists (from
 *	  make_pathkeys_for_sortclauses).  Therefore we don't have to do the
 *	  full two-way-subset-inclusion test on each pair of sublists that is
 *	  implied by the above statement.  Instead we just verify they are
 *	  singleton lists and then do an equal().  This could be improved if
 *	  necessary.
 */
PathKeysComparison
compare_noncanonical_pathkeys(List *keys1, List *keys2)
{
	List	   *key1,
			   *key2;

	for (key1 = keys1, key2 = keys2;
		 key1 != NIL && key2 != NIL;
		 key1 = lnext(key1), key2 = lnext(key2))
	{
		List	   *subkey1 = lfirst(key1);
		List	   *subkey2 = lfirst(key2);

		Assert(length(subkey1) == 1);
		Assert(length(subkey2) == 1);
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
		return PATHKEYS_BETTER1;	/* key1 is longer */
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
 * noncanonical_pathkeys_contained_in
 *	  The same, when we don't have canonical pathkeys.
 */
bool
noncanonical_pathkeys_contained_in(List *keys1, List *keys2)
{
	switch (compare_noncanonical_pathkeys(keys1, keys2))
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
 *
 * We generate the full pathkeys list whether or not all are useful for the
 * current query.  Caller should do truncate_useless_pathkeys().
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
	List	   *indexprs = index->indexprs;

	while (*ordering != InvalidOid)
	{
		PathKeyItem *item;
		Oid			sortop;
		Node	   *indexkey;
		List	   *cpathkey;

		sortop = *ordering;
		if (ScanDirectionIsBackward(scandir))
		{
			sortop = get_commutator(sortop);
			if (sortop == InvalidOid)
				break;			/* oops, no reverse sort operator? */
		}

		if (*indexkeys != 0)
		{
			/* simple index column */
			indexkey = (Node *) find_indexkey_var(root, rel, *indexkeys);
		}
		else
		{
			/* expression --- assume we need not copy it */
			if (indexprs == NIL)
				elog(ERROR, "wrong number of index expressions");
			indexkey = (Node *) lfirst(indexprs);
			indexprs = lnext(indexprs);
		}

		/* OK, make a sublist for this sort key */
		item = makePathKeyItem(indexkey, sortop, true);
		cpathkey = make_canonical_pathkey(root, item);

		/*
		 * Eliminate redundant ordering info; could happen if query is
		 * such that index keys are equijoined...
		 */
		if (!ptrMember(cpathkey, retval))
			retval = lappend(retval, cpathkey);

		indexkeys++;
		ordering++;
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
	Index		relid;
	Oid			reloid,
				vartypeid;
	int32		type_mod;

	foreach(temp, FastListValue(&rel->reltargetlist))
	{
		Var		   *var = (Var *) lfirst(temp);

		if (IsA(var, Var) &&
			var->varattno == varattno)
			return var;
	}

	relid = rel->relid;
	reloid = getrelid(relid, root->rtable);
	get_atttypetypmod(reloid, varattno, &vartypeid, &type_mod);

	return makeVar(relid, varattno, vartypeid, type_mod, 0);
}

/*
 * build_subquery_pathkeys
 *	  Build a pathkeys list that describes the ordering of a subquery's
 *	  result (in the terms of the outer query).  The subquery must already
 *	  have been planned, so that its query_pathkeys field has been set.
 *
 * It is not necessary for caller to do truncate_useless_pathkeys(),
 * because we select keys in a way that takes usefulness of the keys into
 * account.
 */
List *
build_subquery_pathkeys(Query *root, RelOptInfo *rel, Query *subquery)
{
	List	   *retval = NIL;
	int			retvallen = 0;
	int			outer_query_keys = length(root->query_pathkeys);
	List	   *l;

	foreach(l, subquery->query_pathkeys)
	{
		List	   *sub_pathkey = (List *) lfirst(l);
		List	   *j;
		PathKeyItem *best_item = NULL;
		int			best_score = 0;
		List	   *cpathkey;

		/*
		 * The sub_pathkey could contain multiple elements (representing
		 * knowledge that multiple items are effectively equal).  Each
		 * element might match none, one, or more of the output columns
		 * that are visible to the outer query.  This means we may have
		 * multiple possible representations of the sub_pathkey in the
		 * context of the outer query.	Ideally we would generate them all
		 * and put them all into a pathkey list of the outer query,
		 * thereby propagating equality knowledge up to the outer query.
		 * Right now we cannot do so, because the outer query's canonical
		 * pathkey sets are already frozen when this is called.  Instead
		 * we prefer the one that has the highest "score" (number of
		 * canonical pathkey peers, plus one if it matches the outer
		 * query_pathkeys). This is the most likely to be useful in the
		 * outer query.
		 */
		foreach(j, sub_pathkey)
		{
			PathKeyItem *sub_item = (PathKeyItem *) lfirst(j);
			Node	   *sub_key = sub_item->key;
			List	   *k;

			foreach(k, subquery->targetList)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(k);

				if (!tle->resdom->resjunk &&
					equal(tle->expr, sub_key))
				{
					/* Found a representation for this sub_key */
					Var		   *outer_var;
					PathKeyItem *outer_item;
					int			score;

					outer_var = makeVar(rel->relid,
										tle->resdom->resno,
										tle->resdom->restype,
										tle->resdom->restypmod,
										0);
					outer_item = makePathKeyItem((Node *) outer_var,
												 sub_item->sortop,
												 true);
					/* score = # of mergejoin peers */
					score = count_canonical_peers(root, outer_item);
					/* +1 if it matches the proper query_pathkeys item */
					if (retvallen < outer_query_keys &&
						member(outer_item,
							   nth(retvallen, root->query_pathkeys)))
						score++;
					if (score > best_score)
					{
						best_item = outer_item;
						best_score = score;
					}
				}
			}
		}

		/*
		 * If we couldn't find a representation of this sub_pathkey, we're
		 * done (we can't use the ones to its right, either).
		 */
		if (!best_item)
			break;

		/* Canonicalize the chosen item (we did not before) */
		cpathkey = make_canonical_pathkey(root, best_item);

		/*
		 * Eliminate redundant ordering info; could happen if outer query
		 * equijoins subquery keys...
		 */
		if (!ptrMember(cpathkey, retval))
		{
			retval = lappend(retval, cpathkey);
			retvallen++;
		}
	}

	return retval;
}

/*
 * build_join_pathkeys
 *	  Build the path keys for a join relation constructed by mergejoin or
 *	  nestloop join.  These keys should include all the path key vars of the
 *	  outer path (since the join will retain the ordering of the outer path)
 *	  plus any vars of the inner path that are equijoined to the outer vars.
 *
 *	  Per the discussion in backend/optimizer/README, equijoined inner vars
 *	  can be considered path keys of the result, just the same as the outer
 *	  vars they were joined with; furthermore, it doesn't matter what kind
 *	  of join algorithm is actually used.
 *
 *	  EXCEPTION: in a FULL or RIGHT join, we cannot treat the result as
 *	  having the outer path's path keys, because null lefthand rows may be
 *	  inserted at random points.  It must be treated as unsorted.
 *
 * 'joinrel' is the join relation that paths are being formed for
 * 'jointype' is the join type (inner, left, full, etc)
 * 'outer_pathkeys' is the list of the current outer path's path keys
 *
 * Returns the list of new path keys.
 */
List *
build_join_pathkeys(Query *root,
					RelOptInfo *joinrel,
					JoinType jointype,
					List *outer_pathkeys)
{
	if (jointype == JOIN_FULL || jointype == JOIN_RIGHT)
		return NIL;

	/*
	 * This used to be quite a complex bit of code, but now that all
	 * pathkey sublists start out life canonicalized, we don't have to do
	 * a darn thing here!  The inner-rel vars we used to need to add are
	 * *already* part of the outer pathkey!
	 *
	 * We do, however, need to truncate the pathkeys list, since it may
	 * contain pathkeys that were useful for forming this joinrel but are
	 * uninteresting to higher levels.
	 */
	return truncate_useless_pathkeys(root, joinrel, outer_pathkeys);
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
		pathkey = makePathKeyItem(sortkey, sortcl->sortop, true);

		/*
		 * The pathkey becomes a one-element sublist, for now;
		 * canonicalize_pathkeys() might replace it with a longer sublist
		 * later.
		 */
		pathkeys = lappend(pathkeys, makeList1(pathkey));
	}
	return pathkeys;
}

/****************************************************************************
 *		PATHKEYS AND MERGECLAUSES
 ****************************************************************************/

/*
 * cache_mergeclause_pathkeys
 *		Make the cached pathkeys valid in a mergeclause restrictinfo.
 *
 * RestrictInfo contains fields in which we may cache the result
 * of looking up the canonical pathkeys for the left and right sides
 * of the mergeclause.	(Note that in normal cases they will be the
 * same, but not if the mergeclause appears above an OUTER JOIN.)
 * This is a worthwhile savings because these routines will be invoked
 * many times when dealing with a many-relation query.
 *
 * We have to be careful that the cached values are palloc'd in the same
 * context the RestrictInfo node itself is in.	This is not currently a
 * problem for normal planning, but it is an issue for GEQO planning.
 */
void
cache_mergeclause_pathkeys(Query *root, RestrictInfo *restrictinfo)
{
	Node	   *key;
	PathKeyItem *item;
	MemoryContext oldcontext;

	Assert(restrictinfo->mergejoinoperator != InvalidOid);

	if (restrictinfo->left_pathkey == NIL)
	{
		oldcontext = MemoryContextSwitchTo(GetMemoryChunkContext(restrictinfo));
		key = get_leftop(restrictinfo->clause);
		item = makePathKeyItem(key, restrictinfo->left_sortop, false);
		restrictinfo->left_pathkey = make_canonical_pathkey(root, item);
		MemoryContextSwitchTo(oldcontext);
	}
	if (restrictinfo->right_pathkey == NIL)
	{
		oldcontext = MemoryContextSwitchTo(GetMemoryChunkContext(restrictinfo));
		key = get_rightop(restrictinfo->clause);
		item = makePathKeyItem(key, restrictinfo->right_sortop, false);
		restrictinfo->right_pathkey = make_canonical_pathkey(root, item);
		MemoryContextSwitchTo(oldcontext);
	}
}

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
 * an arbitrary choice among the mergeclauses that will work for this side
 * of the join.
 */
List *
find_mergeclauses_for_pathkeys(Query *root,
							   List *pathkeys,
							   List *restrictinfos)
{
	List	   *mergeclauses = NIL;
	List	   *i;

	/* make sure we have pathkeys cached in the clauses */
	foreach(i, restrictinfos)
	{
		RestrictInfo *restrictinfo = lfirst(i);

		cache_mergeclause_pathkeys(root, restrictinfo);
	}

	foreach(i, pathkeys)
	{
		List	   *pathkey = lfirst(i);
		List	   *matched_restrictinfos = NIL;
		List	   *j;

		/*
		 * We can match a pathkey against either left or right side of any
		 * mergejoin clause.  (We examine both sides since we aren't told
		 * if the given pathkeys are for inner or outer input path; no
		 * confusion is possible.)	Furthermore, if there are multiple
		 * matching clauses, take them all.  In plain inner-join scenarios
		 * we expect only one match, because redundant-mergeclause
		 * elimination will have removed any redundant mergeclauses from
		 * the input list. However, in outer-join scenarios there might be
		 * multiple matches. An example is
		 *
		 * select * from a full join b on a.v1 = b.v1 and a.v2 = b.v2 and
		 * a.v1 = b.v2;
		 *
		 * Given the pathkeys ((a.v1), (a.v2)) it is okay to return all three
		 * clauses (in the order a.v1=b.v1, a.v1=b.v2, a.v2=b.v2) and
		 * indeed we *must* do so or we will be unable to form a valid
		 * plan.
		 */
		foreach(j, restrictinfos)
		{
			RestrictInfo *restrictinfo = lfirst(j);

			/*
			 * We can compare canonical pathkey sublists by simple pointer
			 * equality; see compare_pathkeys.
			 */
			if ((pathkey == restrictinfo->left_pathkey ||
				 pathkey == restrictinfo->right_pathkey) &&
				!ptrMember(restrictinfo, mergeclauses))
			{
				matched_restrictinfos = lappend(matched_restrictinfos,
												restrictinfo);
			}
		}

		/*
		 * If we didn't find a mergeclause, we're done --- any additional
		 * sort-key positions in the pathkeys are useless.	(But we can
		 * still mergejoin if we found at least one mergeclause.)
		 */
		if (matched_restrictinfos == NIL)
			break;

		/*
		 * If we did find usable mergeclause(s) for this sort-key
		 * position, add them to result list.
		 */
		mergeclauses = nconc(mergeclauses, matched_restrictinfos);
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
		List	   *pathkey;

		cache_mergeclause_pathkeys(root, restrictinfo);

		if (bms_is_subset(restrictinfo->left_relids, rel->relids))
		{
			/* Rel is left side of mergeclause */
			pathkey = restrictinfo->left_pathkey;
		}
		else if (bms_is_subset(restrictinfo->right_relids, rel->relids))
		{
			/* Rel is right side of mergeclause */
			pathkey = restrictinfo->right_pathkey;
		}
		else
		{
			elog(ERROR, "could not identify which side of mergeclause to use");
			pathkey = NIL;		/* keep compiler quiet */
		}

		/*
		 * When we are given multiple merge clauses, it's possible that
		 * some clauses refer to the same vars as earlier clauses. There's
		 * no reason for us to specify sort keys like (A,B,A) when (A,B)
		 * will do --- and adding redundant sort keys makes add_path think
		 * that this sort order is different from ones that are really the
		 * same, so don't do it.  Since we now have a canonicalized
		 * pathkey, a simple ptrMember test is sufficient to detect
		 * redundant keys.
		 */
		if (!ptrMember(pathkey, pathkeys))
			pathkeys = lappend(pathkeys, pathkey);
	}

	return pathkeys;
}

/****************************************************************************
 *		PATHKEY USEFULNESS CHECKS
 *
 * We only want to remember as many of the pathkeys of a path as have some
 * potential use, either for subsequent mergejoins or for meeting the query's
 * requested output ordering.  This ensures that add_path() won't consider
 * a path to have a usefully different ordering unless it really is useful.
 * These routines check for usefulness of given pathkeys.
 ****************************************************************************/

/*
 * pathkeys_useful_for_merging
 *		Count the number of pathkeys that may be useful for mergejoins
 *		above the given relation (by looking at its joininfo lists).
 *
 * We consider a pathkey potentially useful if it corresponds to the merge
 * ordering of either side of any joinclause for the rel.  This might be
 * overoptimistic, since joinclauses that appear in different join lists
 * might never be usable at the same time, but trying to be exact is likely
 * to be more trouble than it's worth.
 */
int
pathkeys_useful_for_merging(Query *root, RelOptInfo *rel, List *pathkeys)
{
	int			useful = 0;
	List	   *i;

	foreach(i, pathkeys)
	{
		List	   *pathkey = lfirst(i);
		bool		matched = false;
		List	   *j;

		foreach(j, rel->joininfo)
		{
			JoinInfo   *joininfo = (JoinInfo *) lfirst(j);
			List	   *k;

			foreach(k, joininfo->jinfo_restrictinfo)
			{
				RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(k);

				if (restrictinfo->mergejoinoperator == InvalidOid)
					continue;
				cache_mergeclause_pathkeys(root, restrictinfo);

				/*
				 * We can compare canonical pathkey sublists by simple
				 * pointer equality; see compare_pathkeys.
				 */
				if (pathkey == restrictinfo->left_pathkey ||
					pathkey == restrictinfo->right_pathkey)
				{
					matched = true;
					break;
				}
			}

			if (matched)
				break;
		}

		/*
		 * If we didn't find a mergeclause, we're done --- any additional
		 * sort-key positions in the pathkeys are useless.	(But we can
		 * still mergejoin if we found at least one mergeclause.)
		 */
		if (matched)
			useful++;
		else
			break;
	}

	return useful;
}

/*
 * pathkeys_useful_for_ordering
 *		Count the number of pathkeys that are useful for meeting the
 *		query's requested output ordering.
 *
 * Unlike merge pathkeys, this is an all-or-nothing affair: it does us
 * no good to order by just the first key(s) of the requested ordering.
 * So the result is always either 0 or length(root->query_pathkeys).
 */
int
pathkeys_useful_for_ordering(Query *root, List *pathkeys)
{
	if (root->query_pathkeys == NIL)
		return 0;				/* no special ordering requested */

	if (pathkeys == NIL)
		return 0;				/* unordered path */

	if (pathkeys_contained_in(root->query_pathkeys, pathkeys))
	{
		/* It's useful ... or at least the first N keys are */
		return length(root->query_pathkeys);
	}

	return 0;					/* path ordering not useful */
}

/*
 * truncate_useless_pathkeys
 *		Shorten the given pathkey list to just the useful pathkeys.
 */
List *
truncate_useless_pathkeys(Query *root,
						  RelOptInfo *rel,
						  List *pathkeys)
{
	int			nuseful;
	int			nuseful2;

	nuseful = pathkeys_useful_for_merging(root, rel, pathkeys);
	nuseful2 = pathkeys_useful_for_ordering(root, pathkeys);
	if (nuseful2 > nuseful)
		nuseful = nuseful2;

	/*
	 * Note: not safe to modify input list destructively, but we can avoid
	 * copying the list if we're not actually going to change it
	 */
	if (nuseful == length(pathkeys))
		return pathkeys;
	else
		return ltruncate(nuseful, listCopy(pathkeys));
}
