/*-------------------------------------------------------------------------
 *
 * nbtutils.c
 *	  Utility code for Postgres btree implementation.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/nbtree/nbtutils.c,v 1.65.2.1 2005/11/22 18:23:04 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/nbtree.h"
#include "catalog/catalog.h"
#include "executor/execdebug.h"


/*
 * _bt_mkscankey
 *		Build a scan key that contains comparison data from itup
 *		as well as comparator routines appropriate to the key datatypes.
 *
 *		The result is intended for use with _bt_compare().
 */
ScanKey
_bt_mkscankey(Relation rel, IndexTuple itup)
{
	ScanKey		skey;
	TupleDesc	itupdesc;
	int			natts;
	int			i;

	itupdesc = RelationGetDescr(rel);
	natts = RelationGetNumberOfAttributes(rel);

	skey = (ScanKey) palloc(natts * sizeof(ScanKeyData));

	for (i = 0; i < natts; i++)
	{
		FmgrInfo   *procinfo;
		Datum		arg;
		bool		null;

		/*
		 * We can use the cached (default) support procs since no cross-type
		 * comparison can be needed.
		 */
		procinfo = index_getprocinfo(rel, i + 1, BTORDER_PROC);
		arg = index_getattr(itup, i + 1, itupdesc, &null);
		ScanKeyEntryInitializeWithInfo(&skey[i],
									   null ? SK_ISNULL : 0,
									   (AttrNumber) (i + 1),
									   InvalidStrategy,
									   InvalidOid,
									   procinfo,
									   arg);
	}

	return skey;
}

/*
 * _bt_mkscankey_nodata
 *		Build a scan key that contains comparator routines appropriate to
 *		the key datatypes, but no comparison data.	The comparison data
 *		ultimately used must match the key datatypes.
 *
 *		The result cannot be used with _bt_compare().  Currently this
 *		routine is only called by nbtsort.c and tuplesort.c, which have
 *		their own comparison routines.
 */
ScanKey
_bt_mkscankey_nodata(Relation rel)
{
	ScanKey		skey;
	int			natts;
	int			i;

	natts = RelationGetNumberOfAttributes(rel);

	skey = (ScanKey) palloc(natts * sizeof(ScanKeyData));

	for (i = 0; i < natts; i++)
	{
		FmgrInfo   *procinfo;

		/*
		 * We can use the cached (default) support procs since no cross-type
		 * comparison can be needed.
		 */
		procinfo = index_getprocinfo(rel, i + 1, BTORDER_PROC);
		ScanKeyEntryInitializeWithInfo(&skey[i],
									   SK_ISNULL,
									   (AttrNumber) (i + 1),
									   InvalidStrategy,
									   InvalidOid,
									   procinfo,
									   (Datum) 0);
	}

	return skey;
}

/*
 * free a scan key made by either _bt_mkscankey or _bt_mkscankey_nodata.
 */
void
_bt_freeskey(ScanKey skey)
{
	pfree(skey);
}

/*
 * free a retracement stack made by _bt_search.
 */
void
_bt_freestack(BTStack stack)
{
	BTStack		ostack;

	while (stack != NULL)
	{
		ostack = stack;
		stack = stack->bts_parent;
		pfree(ostack);
	}
}

/*
 * Construct a BTItem from a plain IndexTuple.
 *
 * This is now useless code, since a BTItem *is* an index tuple with
 * no extra stuff.	We hang onto it for the moment to preserve the
 * notational distinction, in case we want to add some extra stuff
 * again someday.
 */
BTItem
_bt_formitem(IndexTuple itup)
{
	int			nbytes_btitem;
	BTItem		btitem;
	Size		tuplen;

	/* make a copy of the index tuple with room for extra stuff */
	tuplen = IndexTupleSize(itup);
	nbytes_btitem = tuplen + (sizeof(BTItemData) - sizeof(IndexTupleData));

	btitem = (BTItem) palloc(nbytes_btitem);
	memcpy((char *) &(btitem->bti_itup), (char *) itup, tuplen);

	return btitem;
}

/*----------
 *	_bt_preprocess_keys() -- Preprocess scan keys
 *
 * The caller-supplied keys (in scan->keyData[]) are copied to
 * so->keyData[] with possible transformation.	scan->numberOfKeys is
 * the number of input keys, so->numberOfKeys gets the number of output
 * keys (possibly less, never greater).
 *
 * The primary purpose of this routine is to discover how many scan keys
 * must be satisfied to continue the scan.	It also attempts to eliminate
 * redundant keys and detect contradictory keys.  At present, redundant and
 * contradictory keys can only be detected for same-data-type comparisons,
 * but that's the usual case so it seems worth doing.
 *
 * The output keys must be sorted by index attribute.  Presently we expect
 * (but verify) that the input keys are already so sorted --- this is done
 * by group_clauses_by_indexkey() in indxpath.c.  Some reordering of the keys
 * within each attribute may be done as a byproduct of the processing here,
 * but no other code depends on that.
 *
 * Aside from preparing so->keyData[], this routine sets
 * so->numberOfRequiredKeys to the number of quals that must be satisfied to
 * continue the scan.  _bt_checkkeys uses this.  For example, if the quals
 * are "x = 1 AND y < 4 AND z < 5", then _bt_checkkeys will reject a tuple
 * (1,2,7), but we must continue the scan in case there are tuples (1,3,z).
 * But once we reach tuples like (1,4,z) we can stop scanning because no
 * later tuples could match.  This is reflected by setting
 * so->numberOfRequiredKeys to 2, the number of leading keys that must be
 * matched to continue the scan.  In general, numberOfRequiredKeys is equal
 * to the number of keys for leading attributes with "=" keys, plus the
 * key(s) for the first non "=" attribute, which can be seen to be correct
 * by considering the above example.  Note in particular that if there are no
 * keys for a given attribute, the keys for subsequent attributes can never
 * be required; for instance "WHERE y = 4" requires a full-index scan.
 *
 * If possible, redundant keys are eliminated: we keep only the tightest
 * >/>= bound and the tightest </<= bound, and if there's an = key then
 * that's the only one returned.  (So, we return either a single = key,
 * or one or two boundary-condition keys for each attr.)  However, we can
 * only detect redundant keys when the right-hand datatypes are all equal
 * to the index datatype, because we do not know suitable operators for
 * comparing right-hand values of two different datatypes.	(In theory
 * we could handle comparison of a RHS of the index datatype with a RHS of
 * another type, but that seems too much pain for too little gain.)  So,
 * keys whose operator has a nondefault subtype (ie, its RHS is not of the
 * index datatype) are ignored here, except for noting whether they impose
 * an "=" condition or not.
 *
 * As a byproduct of this work, we can detect contradictory quals such
 * as "x = 1 AND x > 2".  If we see that, we return so->quals_ok = FALSE,
 * indicating the scan need not be run at all since no tuples can match.
 * Again though, only keys with RHS datatype equal to the index datatype
 * can be checked for contradictions.
 *
 * Furthermore, we detect the case where the index is unique and we have
 * equality quals for all columns.	In this case there can be at most one
 * (visible) matching tuple.  index_getnext uses this to avoid uselessly
 * continuing the scan after finding one match.
 *----------
 */
void
_bt_preprocess_keys(IndexScanDesc scan)
{
	Relation	relation = scan->indexRelation;
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	int			numberOfKeys = scan->numberOfKeys;
	int			new_numberOfKeys;
	int			numberOfEqualCols;
	ScanKey		inkeys;
	ScanKey		outkeys;
	ScanKey		cur;
	ScanKey		xform[BTMaxStrategyNumber];
	bool		hasOtherTypeEqual;
	Datum		test;
	int			i,
				j;
	AttrNumber	attno;

	/* initialize result variables */
	so->qual_ok = true;
	so->numberOfKeys = 0;
	so->numberOfRequiredKeys = 0;
	scan->keys_are_unique = false;

	if (numberOfKeys < 1)
		return;					/* done if qual-less scan */

	inkeys = scan->keyData;
	outkeys = so->keyData;
	cur = &inkeys[0];
	/* we check that input keys are correctly ordered */
	if (cur->sk_attno < 1)
		elog(ERROR, "btree index keys must be ordered by attribute");

	/* We can short-circuit most of the work if there's just one key */
	if (numberOfKeys == 1)
	{
		/*
		 * We don't use indices for 'A is null' and 'A is not null' currently
		 * and 'A < = > <> NULL' will always fail - so qual is not OK if
		 * comparison value is NULL.	  - vadim 03/21/97
		 */
		if (cur->sk_flags & SK_ISNULL)
			so->qual_ok = false;
		else if (relation->rd_index->indisunique &&
				 relation->rd_rel->relnatts == 1)
		{
			/* it's a unique index, do we have an equality qual? */
			if (cur->sk_strategy == BTEqualStrategyNumber)
				scan->keys_are_unique = true;
		}
		memcpy(outkeys, inkeys, sizeof(ScanKeyData));
		so->numberOfKeys = 1;
		if (cur->sk_attno == 1)
			so->numberOfRequiredKeys = 1;
		return;
	}

	/*
	 * Otherwise, do the full set of pushups.
	 */
	new_numberOfKeys = 0;
	numberOfEqualCols = 0;

	/*
	 * Initialize for processing of keys for attr 1.
	 *
	 * xform[i] points to the currently best scan key of strategy type i+1, if
	 * any is found with a default operator subtype; it is NULL if we haven't
	 * yet found such a key for this attr.	Scan keys of nondefault subtypes
	 * are transferred to the output with no processing except for noting if
	 * they are of "=" type.
	 */
	attno = 1;
	memset(xform, 0, sizeof(xform));
	hasOtherTypeEqual = false;

	/*
	 * Loop iterates from 0 to numberOfKeys inclusive; we use the last pass to
	 * handle after-last-key processing.  Actual exit from the loop is at the
	 * "break" statement below.
	 */
	for (i = 0;; cur++, i++)
	{
		if (i < numberOfKeys)
		{
			/* See comments above: any NULL implies cannot match qual */
			if (cur->sk_flags & SK_ISNULL)
			{
				so->qual_ok = false;

				/*
				 * Quit processing so we don't try to invoke comparison
				 * routines on NULLs.
				 */
				return;
			}
		}

		/*
		 * If we are at the end of the keys for a particular attr, finish up
		 * processing and emit the cleaned-up keys.
		 */
		if (i == numberOfKeys || cur->sk_attno != attno)
		{
			int			priorNumberOfEqualCols = numberOfEqualCols;

			/* check input keys are correctly ordered */
			if (i < numberOfKeys && cur->sk_attno < attno)
				elog(ERROR, "btree index keys must be ordered by attribute");

			/*
			 * If = has been specified, no other key will be used. In case of
			 * key > 2 && key == 1 and so on we have to set qual_ok to false
			 * before discarding the other keys.
			 */
			if (xform[BTEqualStrategyNumber - 1])
			{
				ScanKey		eq = xform[BTEqualStrategyNumber - 1];

				for (j = BTMaxStrategyNumber; --j >= 0;)
				{
					ScanKey		chk = xform[j];

					if (!chk || j == (BTEqualStrategyNumber - 1))
						continue;
					test = FunctionCall2(&chk->sk_func,
										 eq->sk_argument,
										 chk->sk_argument);
					if (!DatumGetBool(test))
					{
						so->qual_ok = false;
						break;
					}
				}
				xform[BTLessStrategyNumber - 1] = NULL;
				xform[BTLessEqualStrategyNumber - 1] = NULL;
				xform[BTGreaterEqualStrategyNumber - 1] = NULL;
				xform[BTGreaterStrategyNumber - 1] = NULL;
				/* track number of attrs for which we have "=" keys */
				numberOfEqualCols++;
			}
			else
			{
				/* track number of attrs for which we have "=" keys */
				if (hasOtherTypeEqual)
					numberOfEqualCols++;
			}

			/* keep only one of <, <= */
			if (xform[BTLessStrategyNumber - 1]
				&& xform[BTLessEqualStrategyNumber - 1])
			{
				ScanKey		lt = xform[BTLessStrategyNumber - 1];
				ScanKey		le = xform[BTLessEqualStrategyNumber - 1];

				test = FunctionCall2(&le->sk_func,
									 lt->sk_argument,
									 le->sk_argument);
				if (DatumGetBool(test))
					xform[BTLessEqualStrategyNumber - 1] = NULL;
				else
					xform[BTLessStrategyNumber - 1] = NULL;
			}

			/* keep only one of >, >= */
			if (xform[BTGreaterStrategyNumber - 1]
				&& xform[BTGreaterEqualStrategyNumber - 1])
			{
				ScanKey		gt = xform[BTGreaterStrategyNumber - 1];
				ScanKey		ge = xform[BTGreaterEqualStrategyNumber - 1];

				test = FunctionCall2(&ge->sk_func,
									 gt->sk_argument,
									 ge->sk_argument);
				if (DatumGetBool(test))
					xform[BTGreaterEqualStrategyNumber - 1] = NULL;
				else
					xform[BTGreaterStrategyNumber - 1] = NULL;
			}

			/*
			 * Emit the cleaned-up keys into the outkeys[] array.
			 */
			for (j = BTMaxStrategyNumber; --j >= 0;)
			{
				if (xform[j])
					memcpy(&outkeys[new_numberOfKeys++], xform[j],
						   sizeof(ScanKeyData));
			}

			/*
			 * If all attrs before this one had "=", include these keys into
			 * the required-keys count.
			 */
			if (priorNumberOfEqualCols == attno - 1)
				so->numberOfRequiredKeys = new_numberOfKeys;

			/*
			 * Exit loop here if done.
			 */
			if (i == numberOfKeys)
				break;

			/* Re-initialize for new attno */
			attno = cur->sk_attno;
			memset(xform, 0, sizeof(xform));
			hasOtherTypeEqual = false;
		}

		/* check strategy this key's operator corresponds to */
		j = cur->sk_strategy - 1;

		/* if wrong RHS data type, punt */
		if (cur->sk_subtype != InvalidOid)
		{
			memcpy(&outkeys[new_numberOfKeys++], cur,
				   sizeof(ScanKeyData));
			if (j == (BTEqualStrategyNumber - 1))
				hasOtherTypeEqual = true;
			continue;
		}

		/* have we seen one of these before? */
		if (xform[j])
		{
			/* yup, keep the more restrictive key */
			test = FunctionCall2(&cur->sk_func,
								 cur->sk_argument,
								 xform[j]->sk_argument);
			if (DatumGetBool(test))
				xform[j] = cur;
			else if (j == (BTEqualStrategyNumber - 1))
			{
				/* key == a && key == b, but a != b */
				so->qual_ok = false;
				return;
			}
		}
		else
		{
			/* nope, so remember this scankey */
			xform[j] = cur;
		}
	}

	so->numberOfKeys = new_numberOfKeys;

	/*
	 * If unique index and we have equality keys for all columns, set
	 * keys_are_unique flag for higher levels.
	 */
	if (relation->rd_index->indisunique &&
		relation->rd_rel->relnatts == numberOfEqualCols)
		scan->keys_are_unique = true;
}

/*
 * Test whether an indextuple satisfies all the scankey conditions.
 *
 * If the tuple fails to pass the qual, we also determine whether there's
 * any need to continue the scan beyond this tuple, and set *continuescan
 * accordingly.  See comments for _bt_preprocess_keys(), above, about how
 * this is done.
 */
bool
_bt_checkkeys(IndexScanDesc scan, IndexTuple tuple,
			  ScanDirection dir, bool *continuescan)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	int			keysz = so->numberOfKeys;
	int			ikey;
	TupleDesc	tupdesc;
	ScanKey		key;

	*continuescan = true;

	/* If no keys, always scan the whole index */
	if (keysz == 0)
		return true;

	IncrIndexProcessed();

	tupdesc = RelationGetDescr(scan->indexRelation);

	for (key = so->keyData, ikey = 0; ikey < keysz; key++, ikey++)
	{
		Datum		datum;
		bool		isNull;
		Datum		test;

		datum = index_getattr(tuple,
							  key->sk_attno,
							  tupdesc,
							  &isNull);

		/* btree doesn't support 'A is null' clauses, yet */
		if (key->sk_flags & SK_ISNULL)
		{
			/* we shouldn't get here, really; see _bt_preprocess_keys() */
			*continuescan = false;
			return false;
		}

		if (isNull)
		{
			/*
			 * Since NULLs are sorted after non-NULLs, we know we have reached
			 * the upper limit of the range of values for this index attr.	On
			 * a forward scan, we can stop if this qual is one of the "must
			 * match" subset.  On a backward scan, however, we should keep
			 * going.
			 */
			if (ikey < so->numberOfRequiredKeys &&
				ScanDirectionIsForward(dir))
				*continuescan = false;

			/*
			 * In any case, this indextuple doesn't match the qual.
			 */
			return false;
		}

		test = FunctionCall2(&key->sk_func, datum, key->sk_argument);

		if (!DatumGetBool(test))
		{
			/*
			 * Tuple fails this qual.  If it's a required qual, then we may be
			 * able to conclude no further tuples will pass, either. We have
			 * to look at the scan direction and the qual type.
			 *
			 * Note: the only case in which we would keep going after failing
			 * a required qual is if there are partially-redundant quals that
			 * _bt_preprocess_keys() was unable to eliminate.  For example,
			 * given "x > 4 AND x > 10" where both are cross-type comparisons
			 * and so not removable, we might start the scan at the x = 4
			 * boundary point.	The "x > 10" condition will fail until we pass
			 * x = 10, but we must not stop the scan on its account.
			 *
			 * Note: because we stop the scan as soon as any required equality
			 * qual fails, it is critical that equality quals be used for the
			 * initial positioning in _bt_first() when they are available. See
			 * comments in _bt_first().
			 */
			if (ikey < so->numberOfRequiredKeys)
			{
				switch (key->sk_strategy)
				{
					case BTLessStrategyNumber:
					case BTLessEqualStrategyNumber:
						if (ScanDirectionIsForward(dir))
							*continuescan = false;
						break;
					case BTEqualStrategyNumber:
						*continuescan = false;
						break;
					case BTGreaterEqualStrategyNumber:
					case BTGreaterStrategyNumber:
						if (ScanDirectionIsBackward(dir))
							*continuescan = false;
						break;
					default:
						elog(ERROR, "unrecognized StrategyNumber: %d",
							 key->sk_strategy);
				}
			}

			/*
			 * In any case, this indextuple doesn't match the qual.
			 */
			return false;
		}
	}

	/* If we get here, the tuple passes all quals. */
	return true;
}
