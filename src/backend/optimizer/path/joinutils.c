/*-------------------------------------------------------------------------
 *
 * joinutils.c--
 *	  Utilities for matching and building join and path keys
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/path/Attic/joinutils.c,v 1.11 1999/02/08 04:29:12 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/pg_list.h"
#include "nodes/relation.h"
#include "nodes/plannodes.h"

#include "optimizer/internal.h"
#include "optimizer/paths.h"
#include "optimizer/var.h"
#include "optimizer/keys.h"
#include "optimizer/tlist.h"
#include "optimizer/joininfo.h"
#include "optimizer/ordering.h"


static int match_pathkey_joinkeys(List *pathkey, List *joinkeys,
					   int which_subkey);
static bool every_func(List *joinkeys, List *pathkey,
		   int which_subkey);
static List *new_join_pathkey(List *subkeys,
				 List *considered_subkeys, List *join_rel_tlist,
				 List *joinclauses);
static List *new_matching_subkeys(Var *subkey, List *considered_subkeys,
					 List *join_rel_tlist, List *joinclauses);

/****************************************************************************
 *		KEY COMPARISONS
 ****************************************************************************/

/*
 * match-pathkeys-joinkeys--
 *	  Attempts to match the keys of a path against the keys of join clauses.
 *	  This is done by looking for a matching join key in 'joinkeys' for
 *	  every path key in the list 'pathkeys'. If there is a matching join key
 *	  (not necessarily unique) for every path key, then the list of
 *	  corresponding join keys and join clauses are returned in the order in
 *	  which the keys matched the path keys.
 *
 * 'pathkeys' is a list of path keys:
 *		( ( (var) (var) ... ) ( (var) ... ) )
 * 'joinkeys' is a list of join keys:
 *		( (outer inner) (outer inner) ... )
 * 'joinclauses' is a list of clauses corresponding to the join keys in
 *		'joinkeys'
 * 'which-subkey' is a flag that selects the desired subkey of a join key
 *		in 'joinkeys'
 *
 * Returns the join keys and corresponding join clauses in a list if all
 * of the path keys were matched:
 *		(
 *		 ( (outerkey0 innerkey0) ... (outerkeyN innerkeyN) )
 *		 ( clause0 ... clauseN )
 *		)
 * and nil otherwise.
 *
 * Returns a list of matched join keys and a list of matched join clauses
 * in matchedJoinClausesPtr.  - ay 11/94
 */
List *
match_pathkeys_joinkeys(List *pathkeys,
						List *joinkeys,
						List *joinclauses,
						int which_subkey,
						List **matchedJoinClausesPtr)
{
	List	   *matched_joinkeys = NIL;
	List	   *matched_joinclauses = NIL;
	List	   *pathkey = NIL;
	List	   *i = NIL;
	int			matched_joinkey_index = -1;

	foreach(i, pathkeys)
	{
		pathkey = lfirst(i);
		matched_joinkey_index = match_pathkey_joinkeys(pathkey, joinkeys, which_subkey);

		if (matched_joinkey_index != -1)
		{
			List	   *xjoinkey = nth(matched_joinkey_index, joinkeys);
			List	   *joinclause = nth(matched_joinkey_index, joinclauses);

			/* XXX was "push" function */
			matched_joinkeys = lappend(matched_joinkeys, xjoinkey);
			matched_joinkeys = nreverse(matched_joinkeys);

			matched_joinclauses = lappend(matched_joinclauses, joinclause);
			matched_joinclauses = nreverse(matched_joinclauses);
			joinkeys = LispRemove(xjoinkey, joinkeys);
		}
		else
			return NIL;

	}
	if (matched_joinkeys == NULL ||
		length(matched_joinkeys) != length(pathkeys))
		return NIL;

	*matchedJoinClausesPtr = nreverse(matched_joinclauses);
	return nreverse(matched_joinkeys);
}

/*
 * match-pathkey-joinkeys--
 *	  Returns the 0-based index into 'joinkeys' of the first joinkey whose
 *	  outer or inner subkey matches any subkey of 'pathkey'.
 */
static int
match_pathkey_joinkeys(List *pathkey,
					   List *joinkeys,
					   int which_subkey)
{
	Var		   *path_subkey;
	int			pos;
	List	   *i = NIL;
	List	   *x = NIL;
	JoinKey    *jk;

	foreach(i, pathkey)
	{
		path_subkey = (Var *) lfirst(i);
		pos = 0;
		foreach(x, joinkeys)
		{
			jk = (JoinKey *) lfirst(x);
			if (var_equal(path_subkey,
						  extract_subkey(jk, which_subkey)))
				return pos;
			pos++;
		}
	}
	return -1;					/* no index found	*/
}

/*
 * match-paths-joinkeys--
 *	  Attempts to find a path in 'paths' whose keys match a set of join
 *	  keys 'joinkeys'.	To match,
 *	  1. the path node ordering must equal 'ordering'.
 *	  2. each subkey of a given path must match(i.e., be(var_equal) to) the
 *		 appropriate subkey of the corresponding join key in 'joinkeys',
 *		 i.e., the Nth path key must match its subkeys against the subkey of
 *		 the Nth join key in 'joinkeys'.
 *
 * 'joinkeys' is the list of key pairs to which the path keys must be
 *		matched
 * 'ordering' is the ordering of the(outer) path to which 'joinkeys'
 *		must correspond
 * 'paths' is a list of(inner) paths which are to be matched against
 *		each join key in 'joinkeys'
 * 'which-subkey' is a flag that selects the desired subkey of a join key
 *		in 'joinkeys'
 *
 * Returns the matching path node if one exists, nil otherwise.
 */
static bool
every_func(List *joinkeys, List *pathkey, int which_subkey)
{
	JoinKey    *xjoinkey;
	Var		   *temp;
	Var		   *tempkey = NULL;
	bool		found = false;
	List	   *i = NIL;
	List	   *j = NIL;

	foreach(i, joinkeys)
	{
		xjoinkey = (JoinKey *) lfirst(i);
		found = false;
		foreach(j, pathkey)
		{
			temp = (Var *) lfirst((List *) lfirst(j));
			if (temp == NULL)
				continue;
			tempkey = extract_subkey(xjoinkey, which_subkey);
			if (var_equal(tempkey, temp))
			{
				found = true;
				break;
			}
		}
		if (found == false)
			return false;
	}
	return found;
}


/*
 * match_paths_joinkeys -
 *	  find the cheapest path that matches the join keys
 */
Path *
match_paths_joinkeys(List *joinkeys,
					 PathOrder *ordering,
					 List *paths,
					 int which_subkey)
{
	Path	   *matched_path = NULL;
	bool		key_match = false;
	List	   *i = NIL;

	foreach(i, paths)
	{
		Path	   *path = (Path *) lfirst(i);

		key_match = every_func(joinkeys, path->keys, which_subkey);

		if (equal_path_ordering(ordering,
								&path->path_order) &&
			length(joinkeys) == length(path->keys) &&
			key_match)
		{

			if (matched_path)
			{
				if (path->path_cost < matched_path->path_cost)
					matched_path = path;
			}
			else
				matched_path = path;
		}
	}
	return matched_path;
}



/*
 * extract-path-keys--
 *	  Builds a subkey list for a path by pulling one of the subkeys from
 *	  a list of join keys 'joinkeys' and then finding the var node in the
 *	  target list 'tlist' that corresponds to that subkey.
 *
 * 'joinkeys' is a list of join key pairs
 * 'tlist' is a relation target list
 * 'which-subkey' is a flag that selects the desired subkey of a join key
 *		in 'joinkeys'
 *
 * Returns a list of pathkeys: ((tlvar1)(tlvar2)...(tlvarN)).
 * [I've no idea why they have to be list of lists. Should be fixed. -ay 12/94]
 */
List *
extract_path_keys(List *joinkeys,
				  List *tlist,
				  int which_subkey)
{
	List	   *pathkeys = NIL;
	List	   *jk;

	foreach(jk, joinkeys)
	{
		JoinKey    *jkey = (JoinKey *) lfirst(jk);
		Var		   *var,
				   *key;
		List	   *p;

		/*
		 * find the right Var in the target list for this key
		 */
		var = (Var *) extract_subkey(jkey, which_subkey);
		key = (Var *) matching_tlvar(var, tlist);

		/*
		 * include it in the pathkeys list if we haven't already done so
		 */
		foreach(p, pathkeys)
		{
			Var		   *pkey = lfirst((List *) lfirst(p));		/* XXX fix me */

			if (key == pkey)
				break;
		}
		if (p != NIL)
			continue;			/* key already in pathkeys */

		pathkeys = lappend(pathkeys, lcons(key, NIL));
	}
	return pathkeys;
}


/****************************************************************************
 *		NEW PATHKEY FORMATION
 ****************************************************************************/

/*
 * new-join-pathkeys--
 *	  Find the path keys for a join relation by finding all vars in the list
 *	  of join clauses 'joinclauses' such that:
 *		(1) the var corresponding to the outer join relation is a
 *			key on the outer path
 *		(2) the var appears in the target list of the join relation
 *	  In other words, add to each outer path key the inner path keys that
 *	  are required for qualification.
 *
 * 'outer-pathkeys' is the list of the outer path's path keys
 * 'join-rel-tlist' is the target list of the join relation
 * 'joinclauses' is the list of restricting join clauses
 *
 * Returns the list of new path keys.
 *
 */
List *
new_join_pathkeys(List *outer_pathkeys,
				  List *join_rel_tlist,
				  List *joinclauses)
{
	List	   *outer_pathkey = NIL;
	List	   *t_list = NIL;
	List	   *x;
	List	   *i = NIL;

	foreach(i, outer_pathkeys)
	{
		outer_pathkey = lfirst(i);
		x = new_join_pathkey(outer_pathkey, NIL,
							 join_rel_tlist, joinclauses);
		if (x != NIL)
			t_list = lappend(t_list, x);
	}
	return t_list;
}

/*
 * new-join-pathkey--
 *	  Finds new vars that become subkeys due to qualification clauses that
 *	  contain any previously considered subkeys.  These new subkeys plus the
 *	  subkeys from 'subkeys' form a new pathkey for the join relation.
 *
 *	  Note that each returned subkey is the var node found in
 *	  'join-rel-tlist' rather than the joinclause var node.
 *
 * 'subkeys' is a list of subkeys for which matching subkeys are to be
 *		found
 * 'considered-subkeys' is the current list of all subkeys corresponding
 *		to a given pathkey
 *
 * Returns a new pathkey(list of subkeys).
 *
 */
static List *
new_join_pathkey(List *subkeys,
				 List *considered_subkeys,
				 List *join_rel_tlist,
				 List *joinclauses)
{
	List	   *t_list = NIL;
	Var		   *subkey;
	List	   *i = NIL;
	List	   *matched_subkeys = NIL;
	Expr	   *tlist_key = (Expr *) NULL;
	List	   *newly_considered_subkeys = NIL;

	foreach(i, subkeys)
	{
		subkey = (Var *) lfirst(i);
		if (subkey == NULL)
			break;				/* XXX something is wrong */
		matched_subkeys = new_matching_subkeys(subkey, considered_subkeys,
								 join_rel_tlist, joinclauses);
		tlist_key = matching_tlvar(subkey, join_rel_tlist);
		newly_considered_subkeys = NIL;

		if (tlist_key)
		{
			if (!member(tlist_key, matched_subkeys))
				newly_considered_subkeys = lcons(tlist_key,
												 matched_subkeys);
		}
		else
			newly_considered_subkeys = matched_subkeys;

		considered_subkeys = append(considered_subkeys, newly_considered_subkeys);

		t_list = nconc(t_list, newly_considered_subkeys);
	}
	return t_list;
}

/*
 * new-matching-subkeys--
 *	  Returns a list of new subkeys:
 *	  (1) which are not listed in 'considered-subkeys'
 *	  (2) for which the "other" variable in some clause in 'joinclauses' is
 *		  'subkey'
 *	  (3) which are mentioned in 'join-rel-tlist'
 *
 *	  Note that each returned subkey is the var node found in
 *	  'join-rel-tlist' rather than the joinclause var node.
 *
 * 'subkey' is the var node for which we are trying to find matching
 *		clauses
 *
 * Returns a list of new subkeys.
 *
 */
static List *
new_matching_subkeys(Var *subkey,
					 List *considered_subkeys,
					 List *join_rel_tlist,
					 List *joinclauses)
{
	Expr	   *joinclause = NULL;
	List	   *t_list = NIL;
	List	   *temp = NIL;
	List	   *i = NIL;
	Expr	   *tlist_other_var = (Expr *) NULL;

	foreach(i, joinclauses)
	{
		joinclause = lfirst(i);
		tlist_other_var = matching_tlvar(other_join_clause_var(subkey, joinclause),
						   join_rel_tlist);

		if (tlist_other_var &&
			!(member(tlist_other_var, considered_subkeys)))
		{

			/* XXX was "push" function	*/
			considered_subkeys = lappend(considered_subkeys,
										 tlist_other_var);

			/*
			 * considered_subkeys = nreverse(considered_subkeys); XXX -- I
			 * am not sure of this.
			 */

			temp = lcons(tlist_other_var, NIL);
			t_list = nconc(t_list, temp);
		}
	}
	return t_list;
}
