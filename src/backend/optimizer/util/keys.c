/*-------------------------------------------------------------------------
 *
 * keys.c--
 *	  Key manipulation routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/Attic/keys.c,v 1.7 1998/07/18 04:22:41 momjian Exp $
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
 *		(outer-subkey inner-subkey)
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
 * match-indexkey-operand--
 *	  Returns t iff an index key 'index-key' matches the given clause
 *	  operand.
 *
 */
bool
match_indexkey_operand(int indexkey, Var *operand, RelOptInfo *rel)
{
	if (IsA(operand, Var) &&
		(lfirsti(rel->relids) == operand->varno) &&
		equal_indexkey_var(indexkey, operand))
		return (true);
	else
		return (false);
}

/*
 * equal_indexkey_var--
 *	  Returns t iff an index key 'index-key' matches the corresponding
 *	  fields of var node 'var'.
 *
 */
static bool
equal_indexkey_var(int index_key, Var *var)
{
	if (index_key == var->varattno)
		return (true);
	else
		return (false);
}

/*
 * extract-subkey--
 *	 Returns the subkey in a join key corresponding to the outer or inner
 *	 lelation.
 *
 */
Var *
extract_subkey(JoinKey *jk, int which_subkey)
{
	Var		   *retval;

	switch (which_subkey)
	{
		case OUTER:
			retval = jk->outer;
			break;
		case INNER:
			retval = jk->inner;
			break;
		default:				/* do nothing */
			elog(DEBUG, "extract_subkey with neither INNER or OUTER");
			retval = NULL;
	}
	return (retval);
}

/*
 * samekeys--
 *	  Returns t iff two sets of path keys are equivalent.  They are
 *	  equivalent if the first subkey (var node) within each sublist of
 *	  list 'keys1' is contained within the corresponding sublist of 'keys2'.
 *
 *	  XXX		It isn't necessary to check that each sublist exactly contain
 *				the same elements because if the routine that built these
 *				sublists together is correct, having one element in common
 *				implies having all elements in common.
 *
 */
bool
samekeys(List *keys1, List *keys2)
{
	bool		allmember = true;
	List	   *key1,
			   *key2;

	for (key1 = keys1, key2 = keys2; key1 != NIL && key2 != NIL;
		 key1 = lnext(key1), key2 = lnext(key2))
		if (!member(lfirst(key1), lfirst(key2)))
			allmember = false;

	if ((length(keys2) >= length(keys1)) && allmember)
		return (true);
	else
		return (false);
}

/*
 * collect-index-pathkeys--
 *	  Creates a list of subkeys by retrieving var nodes corresponding to
 *	  each index key in 'index-keys' from the relation's target list
 *	  'tlist'.	If the key is not in the target list, the key is irrelevant
 *	  and is thrown away.  The returned subkey list is of the form:
 *				((var1) (var2) ... (varn))
 *
 * 'index-keys' is a list of index keys
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
		return ((Expr *) get_expr(tlentry));
	else
		return ((Expr *) NULL);
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
			retval = nconc(retval, lcons(lcons(mvar, NIL),
										 NIL));
		index_keys++;
	}
	return (retval);
}
