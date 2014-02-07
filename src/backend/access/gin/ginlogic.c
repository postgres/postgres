/*-------------------------------------------------------------------------
 *
 * ginlogic.c
 *	  routines for performing binary- and ternary-logic consistent checks.
 *
 * A GIN operator class provides a consistent function which checks if a
 * tuple matches a qual, when the given set of keys are present in the tuple.
 * The consistent function is passed a TRUE/FALSE argument for every key,
 * indicating if that key is present, and it returns TRUE or FALSE. However,
 * a GIN scan can apply various optimizations, if it can determine that an
 * item matches or doesn't match, even if it doesn't know if some of the keys
 * are present or not. Hence, it's useful to have a ternary-logic consistent
 * function, where where each key can be TRUE (present), FALSE (not present),
 * or MAYBE (don't know if present). This file provides such a ternary-logic
 * consistent function,  implemented by calling the regular boolean consistent
 * function many times, with all the MAYBE arguments set to all combinations
 * of TRUE and FALSE.
 *
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gin/ginlogic.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin_private.h"
#include "access/reloptions.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"


/*
 * Maximum number of MAYBE inputs that shimTriConsistentFn will try to
 * resolve by calling all combinations.
 */
#define	MAX_MAYBE_ENTRIES	4

/*
 * A dummy consistent function for an EVERYTHING key. Just claim it matches.
 */
static bool
trueConsistentFn(GinScanKey key)
{
	key->recheckCurItem = false;
	return true;
}
static GinLogicValue
trueTriConsistentFn(GinScanKey key)
{
	return GIN_MAYBE;
}

/*
 * A helper function for calling a regular, binary logic, consistent function.
 */
static bool
normalBoolConsistentFn(GinScanKey key)
{
	/*
	 * Initialize recheckCurItem in case the consistentFn doesn't know it
	 * should set it.  The safe assumption in that case is to force recheck.
	 */
	key->recheckCurItem = true;

	return DatumGetBool(FunctionCall8Coll(key->consistentFmgrInfo,
										  key->collation,
										  PointerGetDatum(key->entryRes),
										  UInt16GetDatum(key->strategy),
										  key->query,
										  UInt32GetDatum(key->nuserentries),
										  PointerGetDatum(key->extra_data),
									   PointerGetDatum(&key->recheckCurItem),
										  PointerGetDatum(key->queryValues),
									 PointerGetDatum(key->queryCategories)));
}

/*
 * This function implements a tri-state consistency check, using a boolean
 * consistent function provided by the opclass.
 *
 * Our strategy is to call consistentFn with MAYBE inputs replaced with every
 * combination of TRUE/FALSE. If consistentFn returns the same value for every
 * combination, that's the overall result. Otherwise, return MAYBE. Testing
 * every combination is O(n^2), so this is only feasible for a small number of
 * MAYBE inputs.
 *
 * NB: This function modifies the key->entryRes array!
 */
static GinLogicValue
shimTriConsistentFn(GinScanKey key)
{
	int			nmaybe;
	int			maybeEntries[MAX_MAYBE_ENTRIES];
	int			i;
	bool		boolResult;
	bool		recheck = false;
	GinLogicValue curResult;

	/*
	 * Count how many MAYBE inputs there are, and store their indexes in
	 * maybeEntries. If there are too many MAYBE inputs, it's not feasible to
	 * test all combinations, so give up and return MAYBE.
	 */
	nmaybe = 0;
	for (i = 0; i < key->nentries; i++)
	{
		if (key->entryRes[i] == GIN_MAYBE)
		{
			if (nmaybe >= MAX_MAYBE_ENTRIES)
				return GIN_MAYBE;
			maybeEntries[nmaybe++] = i;
		}
	}

	/*
	 * If none of the inputs were MAYBE, so we can just call consistent
	 * function as is.
	 */
	if (nmaybe == 0)
		return normalBoolConsistentFn(key);

	/* First call consistent function with all the maybe-inputs set FALSE */
	for (i = 0; i < nmaybe; i++)
		key->entryRes[maybeEntries[i]] = GIN_FALSE;
	curResult = normalBoolConsistentFn(key);

	for (;;)
	{
		/* Twiddle the entries for next combination. */
		for (i = 0; i < nmaybe; i++)
		{
			if (key->entryRes[maybeEntries[i]] == GIN_FALSE)
			{
				key->entryRes[maybeEntries[i]] = GIN_TRUE;
				break;
			}
			else
				key->entryRes[maybeEntries[i]] = GIN_FALSE;
		}
		if (i == nmaybe)
			break;

		boolResult = normalBoolConsistentFn(key);
		recheck |= key->recheckCurItem;

		if (curResult != boolResult)
			return GIN_MAYBE;
	}

	/* TRUE with recheck is taken to mean MAYBE */
	if (curResult == GIN_TRUE && recheck)
		curResult = GIN_MAYBE;

	return curResult;
}

/*
 * Set up the implementation of the consistent functions for a scan key.
 */
void
ginInitConsistentFunction(GinState *ginstate, GinScanKey key)
{
	if (key->searchMode == GIN_SEARCH_MODE_EVERYTHING)
	{
		key->boolConsistentFn = trueConsistentFn;
		key->triConsistentFn = trueTriConsistentFn;
	}
	else
	{
		key->consistentFmgrInfo = &ginstate->consistentFn[key->attnum - 1];
		key->collation = ginstate->supportCollation[key->attnum - 1];
		key->boolConsistentFn = normalBoolConsistentFn;
		key->triConsistentFn = shimTriConsistentFn;
	}
}
