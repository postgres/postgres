/*-------------------------------------------------------------------------
 *
 * keys.c
 *	  Key manipulation routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/Attic/keys.c,v 1.19 1999/02/20 18:01:02 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "nodes/pg_list.h"
#include "nodes/nodes.h"
#include "nodes/relation.h"
#include "utils/elog.h"

#include "optimizer/internal.h"
#include "optimizer/keys.h"
#include "optimizer/tlist.h"


static Expr *matching2_tlvar(int var, List *tlist, bool (*test) ());
static bool equal_indexkey_var(int index_key, Var *var);

/*
 * 1. index key
 *		one of:
 *				attnum
 *				(attnum arrayindex)
 * 2. path key
 *		(subkey1 ... subkeyN)
 *		where subkeyI is a var node
 *		note that the 'Keys field is a list of these
 * 3. join key
 *		(outer_subkey inner_subkey)
 *				where each subkey is a var node
 * 4. sort key
 *		one of:
 *				SortKey node
 *				number
 *				nil
 *		(may also refer to the 'SortKey field of a SortKey node,
 *		 which looks exactly like an index key)
 *
 */

/*
 * match_indexkey_operand
 *	  Returns t iff an index key 'index_key' matches the given clause
 *	  operand.
 *
 */
bool
match_indexkey_operand(int indexkey, Var *operand, RelOptInfo *rel)
{
	if (IsA(operand, Var) &&
		(lfirsti(rel->relids) == operand->varno) &&
		equal_indexkey_var(indexkey, operand))
		return true;
	else
		return false;
}

/*
 * equal_indexkey_var
 *	  Returns t iff an index key 'index_key' matches the corresponding
 *	  fields of var node 'var'.
 *
 */
static bool
equal_indexkey_var(int index_key, Var *var)
{
	if (index_key == var->varattno)
		return true;
	else
		return false;
}

/*
 * extract_join_key
 *	 Returns the subkey in a join key corresponding to the outer or inner
 *	 relation.
 *
 */
Var *
extract_join_key(JoinKey *jk, int outer_or_inner)
{
	Var		   *retval;

	switch (outer_or_inner)
	{
		case OUTER:
			retval = jk->outer;
			break;
		case INNER:
			retval = jk->inner;
			break;
		default:				/* do nothing */
			elog(DEBUG, "extract_join_key with neither INNER or OUTER");
			retval = NULL;
	}
	return retval;
}

/*
 * pathkeys_match
 *	  Returns t iff two sets of path keys are equivalent.  They are
 *	  equivalent if the first Var nodes match the second Var nodes.
 *
 *	See the top of optimizer/path/pathkeys.c for a description of pathkeys.
 *	Each pathkey is ordered by its join order, so they not pre-ordered to
 *	match.  We must search them ourselves.
 *
 *	This gets called a lot, so it is optimized.
 */
bool
pathkeys_match(List *keys1, List *keys2, int *better_key)
{
	List	   *key1,
			   *key2;
	bool		key1_subsetof_key2 = true,
				key2_subsetof_key1 = true;

	for (key1 = keys1, key2 = keys2;
		 key1 != NIL && key2 != NIL;
		 key1 = lnext(key1), key2 = lnext(key2))
	{
		List *i;

		if (key1_subsetof_key2)
			foreach(i, lfirst(key1))
			{
				Var	*subkey = lfirst(i);
				if (!member(subkey, lfirst(key2)))
				{
					key1_subsetof_key2 = false;
					break;
				}
			}

		if (key2_subsetof_key1)
			foreach(i, lfirst(key2))
			{
				Var	*subkey = lfirst(i);
				if (!member(subkey, lfirst(key1)))
				{
					key2_subsetof_key1 = false;
					break;
				}
			}
		if (!key1_subsetof_key2 && !key2_subsetof_key1)
			break;	/* no need to continue comparisons. */
	}

	if (!key1_subsetof_key2 && !key2_subsetof_key1)
	{
		*better_key = 0;
		return false;
	}
	if (key1_subsetof_key2 && !key2_subsetof_key1)
	{
		*better_key = 2;
		return true;
	}
	if (!key1_subsetof_key2 && key2_subsetof_key1)
	{
		*better_key = 1;
		return true;
	}

	*better_key = 0;
	return true;

}

/*
 * collect_index_pathkeys
 *	  Creates a list of subkeys by retrieving var nodes corresponding to
 *	  each index key in 'index_keys' from the relation's target list
 *	  'tlist'.	If the key is not in the target list, the key is irrelevant
 *	  and is thrown away.  The returned subkey list is of the form:
 *				((var1) (var2) ... (varn))
 *
 * 'index_keys' is a list of index keys
 * 'tlist' is a relation target list
 *
 * Returns the list of cons'd subkeys.
 *
 */
/* This function is identical to matching_tlvar and tlistentry_member.
 * They should be merged.
 */
static Expr *
matching2_tlvar(int var, List *tlist, bool (*test) ())
{
	TargetEntry *tlentry = NULL;

	if (var)
	{
		List	   *temp;

		foreach(temp, tlist)
		{
			if ((*test) (var, get_expr(lfirst(temp))))
			{
				tlentry = lfirst(temp);
				break;
			}
		}
	}

	if (tlentry)
		return (Expr *) get_expr(tlentry);
	else
		return (Expr *) NULL;
}


List *
collect_index_pathkeys(int *index_keys, List *tlist)
{
	List	   *retval = NIL;

	Assert(index_keys != NULL);

	while (index_keys[0] != 0)
	{
		Expr	   *mvar;

		mvar = matching2_tlvar(index_keys[0],
							   tlist,
							   equal_indexkey_var);
		if (mvar)
			retval = lappend(retval, lcons(mvar, NIL));
		index_keys++;
	}
	return retval;
}
