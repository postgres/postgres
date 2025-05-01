/*-------------------------------------------------------------------------
 *
 * nbtpreprocesskeys.c
 *	  Preprocessing for Postgres btree scan keys.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/nbtree/nbtpreprocesskeys.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/nbtree.h"
#include "lib/qunique.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

typedef struct BTScanKeyPreproc
{
	ScanKey		inkey;
	int			inkeyi;
	int			arrayidx;
} BTScanKeyPreproc;

typedef struct BTSortArrayContext
{
	FmgrInfo   *sortproc;
	Oid			collation;
	bool		reverse;
} BTSortArrayContext;

static bool _bt_fix_scankey_strategy(ScanKey skey, int16 *indoption);
static void _bt_mark_scankey_required(ScanKey skey);
static bool _bt_compare_scankey_args(IndexScanDesc scan, ScanKey op,
									 ScanKey leftarg, ScanKey rightarg,
									 BTArrayKeyInfo *array, FmgrInfo *orderproc,
									 bool *result);
static bool _bt_compare_array_scankey_args(IndexScanDesc scan,
										   ScanKey arraysk, ScanKey skey,
										   FmgrInfo *orderproc, BTArrayKeyInfo *array,
										   bool *qual_ok);
static bool _bt_saoparray_shrink(IndexScanDesc scan, ScanKey arraysk,
								 ScanKey skey, FmgrInfo *orderproc,
								 BTArrayKeyInfo *array, bool *qual_ok);
static bool _bt_skiparray_shrink(IndexScanDesc scan, ScanKey skey,
								 BTArrayKeyInfo *array, bool *qual_ok);
static void _bt_skiparray_strat_adjust(IndexScanDesc scan, ScanKey arraysk,
									   BTArrayKeyInfo *array);
static void _bt_skiparray_strat_decrement(IndexScanDesc scan, ScanKey arraysk,
										  BTArrayKeyInfo *array);
static void _bt_skiparray_strat_increment(IndexScanDesc scan, ScanKey arraysk,
										  BTArrayKeyInfo *array);
static ScanKey _bt_preprocess_array_keys(IndexScanDesc scan, int *new_numberOfKeys);
static void _bt_preprocess_array_keys_final(IndexScanDesc scan, int *keyDataMap);
static int	_bt_num_array_keys(IndexScanDesc scan, Oid *skip_eq_ops_out,
							   int *numSkipArrayKeys_out);
static Datum _bt_find_extreme_element(IndexScanDesc scan, ScanKey skey,
									  Oid elemtype, StrategyNumber strat,
									  Datum *elems, int nelems);
static void _bt_setup_array_cmp(IndexScanDesc scan, ScanKey skey, Oid elemtype,
								FmgrInfo *orderproc, FmgrInfo **sortprocp);
static int	_bt_sort_array_elements(ScanKey skey, FmgrInfo *sortproc,
									bool reverse, Datum *elems, int nelems);
static bool _bt_merge_arrays(IndexScanDesc scan, ScanKey skey,
							 FmgrInfo *sortproc, bool reverse,
							 Oid origelemtype, Oid nextelemtype,
							 Datum *elems_orig, int *nelems_orig,
							 Datum *elems_next, int nelems_next);
static int	_bt_compare_array_elements(const void *a, const void *b, void *arg);


/*
 *	_bt_preprocess_keys() -- Preprocess scan keys
 *
 * The given search-type keys (taken from scan->keyData[])
 * are copied to so->keyData[] with possible transformation.
 * scan->numberOfKeys is the number of input keys, so->numberOfKeys gets
 * the number of output keys.  Calling here a second or subsequent time
 * (during the same btrescan) is a no-op.
 *
 * The output keys are marked with additional sk_flags bits beyond the
 * system-standard bits supplied by the caller.  The DESC and NULLS_FIRST
 * indoption bits for the relevant index attribute are copied into the flags.
 * Also, for a DESC column, we commute (flip) all the sk_strategy numbers
 * so that the index sorts in the desired direction.
 *
 * One key purpose of this routine is to discover which scan keys must be
 * satisfied to continue the scan.  It also attempts to eliminate redundant
 * keys and detect contradictory keys.  (If the index opfamily provides
 * incomplete sets of cross-type operators, we may fail to detect redundant
 * or contradictory keys, but we can survive that.)
 *
 * The output keys must be sorted by index attribute.  Presently we expect
 * (but verify) that the input keys are already so sorted --- this is done
 * by match_clauses_to_index() in indxpath.c.  Some reordering of the keys
 * within each attribute may be done as a byproduct of the processing here.
 * That process must leave array scan keys (within an attribute) in the same
 * order as corresponding entries from the scan's BTArrayKeyInfo array info.
 * We might also construct skip array scan keys that weren't present in the
 * original input keys; these are also output in standard attribute order.
 *
 * The output keys are marked with flags SK_BT_REQFWD and/or SK_BT_REQBKWD
 * if they must be satisfied in order to continue the scan forward or backward
 * respectively.  _bt_checkkeys uses these flags.  For example, if the quals
 * are "x = 1 AND y < 4 AND z < 5", then _bt_checkkeys will reject a tuple
 * (1,2,7), but we must continue the scan in case there are tuples (1,3,z).
 * But once we reach tuples like (1,4,z) we can stop scanning because no
 * later tuples could match.  This is reflected by marking the x and y keys,
 * but not the z key, with SK_BT_REQFWD.  In general, the keys for leading
 * attributes with "=" keys are marked both SK_BT_REQFWD and SK_BT_REQBKWD.
 * For the first attribute without an "=" key, any "<" and "<=" keys are
 * marked SK_BT_REQFWD while any ">" and ">=" keys are marked SK_BT_REQBKWD.
 * This can be seen to be correct by considering the above example.
 *
 * If we never generated skip array scan keys, it would be possible for "gaps"
 * to appear that make it unsafe to mark any subsequent input scan keys
 * (copied from scan->keyData[]) as required to continue the scan.  Prior to
 * Postgres 18, a qual like "WHERE y = 4" always resulted in a full scan.
 * This qual now becomes "WHERE x = ANY('{every possible x value}') and y = 4"
 * on output.  In other words, preprocessing now adds a skip array on "x".
 * This has the potential to be much more efficient than a full index scan
 * (though it behaves like a full scan when there's many distinct "x" values).
 *
 * If possible, redundant keys are eliminated: we keep only the tightest
 * >/>= bound and the tightest </<= bound, and if there's an = key then
 * that's the only one returned.  (So, we return either a single = key,
 * or one or two boundary-condition keys for each attr.)  However, if we
 * cannot compare two keys for lack of a suitable cross-type operator,
 * we cannot eliminate either.  If there are two such keys of the same
 * operator strategy, the second one is just pushed into the output array
 * without further processing here.  We may also emit both >/>= or both
 * </<= keys if we can't compare them.  The logic about required keys still
 * works if we don't eliminate redundant keys.
 *
 * Note that one reason we need direction-sensitive required-key flags is
 * precisely that we may not be able to eliminate redundant keys.  Suppose
 * we have "x > 4::int AND x > 10::bigint", and we are unable to determine
 * which key is more restrictive for lack of a suitable cross-type operator.
 * _bt_first will arbitrarily pick one of the keys to do the initial
 * positioning with.  If it picks x > 4, then the x > 10 condition will fail
 * until we reach index entries > 10; but we can't stop the scan just because
 * x > 10 is failing.  On the other hand, if we are scanning backwards, then
 * failure of either key is indeed enough to stop the scan.  (In general, when
 * inequality keys are present, the initial-positioning code only promises to
 * position before the first possible match, not exactly at the first match,
 * for a forward scan; or after the last match for a backward scan.)
 *
 * As a byproduct of this work, we can detect contradictory quals such
 * as "x = 1 AND x > 2".  If we see that, we return so->qual_ok = false,
 * indicating the scan need not be run at all since no tuples can match.
 * (In this case we do not bother completing the output key array!)
 * Again, missing cross-type operators might cause us to fail to prove the
 * quals contradictory when they really are, but the scan will work correctly.
 *
 * Skip array = keys will even be generated in the presence of "contradictory"
 * inequality quals when it'll enable marking later input quals as required.
 * We'll merge any such inequalities into the generated skip array by setting
 * its array.low_compare or array.high_compare key field.  The resulting skip
 * array will generate its array elements from a range that's constrained by
 * any merged input inequalities (which won't get output in so->keyData[]).
 *
 * Row comparison keys currently have a couple of notable limitations.
 * Right now we just transfer them into the preprocessed array without any
 * editorialization.  We can treat them the same as an ordinary inequality
 * comparison on the row's first index column, for the purposes of the logic
 * about required keys.  Also, we are unable to merge a row comparison key
 * into a skip array (only ordinary inequalities are merged).  A key that
 * comes after a Row comparison key is therefore never marked as required.
 *
 * Note: the reason we have to copy the preprocessed scan keys into private
 * storage is that we are modifying the array based on comparisons of the
 * key argument values, which could change on a rescan.  Therefore we can't
 * overwrite the source data.
 */
void
_bt_preprocess_keys(IndexScanDesc scan)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	int			numberOfKeys = scan->numberOfKeys;
	int16	   *indoption = scan->indexRelation->rd_indoption;
	int			new_numberOfKeys;
	int			numberOfEqualCols;
	ScanKey		inkeys;
	BTScanKeyPreproc xform[BTMaxStrategyNumber];
	bool		test_result;
	AttrNumber	attno;
	ScanKey		arrayKeyData;
	int		   *keyDataMap = NULL;
	int			arrayidx = 0;

	if (so->numberOfKeys > 0)
	{
		/*
		 * Only need to do preprocessing once per btrescan, at most.  All
		 * calls after the first are handled as no-ops.
		 */
		return;
	}

	/* initialize result variables */
	so->qual_ok = true;
	so->numberOfKeys = 0;

	if (numberOfKeys < 1)
		return;					/* done if qual-less scan */

	/* If any keys are SK_SEARCHARRAY type, set up array-key info */
	arrayKeyData = _bt_preprocess_array_keys(scan, &numberOfKeys);
	if (!so->qual_ok)
	{
		/* unmatchable array, so give up */
		return;
	}

	/*
	 * Treat arrayKeyData[] (a partially preprocessed copy of scan->keyData[])
	 * as our input if _bt_preprocess_array_keys just allocated it, else just
	 * use scan->keyData[]
	 */
	if (arrayKeyData)
	{
		inkeys = arrayKeyData;

		/* Also maintain keyDataMap for remapping so->orderProcs[] later */
		keyDataMap = MemoryContextAlloc(so->arrayContext,
										numberOfKeys * sizeof(int));

		/*
		 * Also enlarge output array when it might otherwise not have room for
		 * a skip array's scan key
		 */
		if (numberOfKeys > scan->numberOfKeys)
			so->keyData = repalloc(so->keyData,
								   numberOfKeys * sizeof(ScanKeyData));
	}
	else
		inkeys = scan->keyData;

	/* we check that input keys are correctly ordered */
	if (inkeys[0].sk_attno < 1)
		elog(ERROR, "btree index keys must be ordered by attribute");

	/* We can short-circuit most of the work if there's just one key */
	if (numberOfKeys == 1)
	{
		/* Apply indoption to scankey (might change sk_strategy!) */
		if (!_bt_fix_scankey_strategy(&inkeys[0], indoption))
			so->qual_ok = false;
		memcpy(&so->keyData[0], &inkeys[0], sizeof(ScanKeyData));
		so->numberOfKeys = 1;
		/* We can mark the qual as required if it's for first index col */
		if (inkeys[0].sk_attno == 1)
			_bt_mark_scankey_required(&so->keyData[0]);
		if (arrayKeyData)
		{
			/*
			 * Don't call _bt_preprocess_array_keys_final in this fast path
			 * (we'll miss out on the single value array transformation, but
			 * that's not nearly as important when there's only one scan key)
			 */
			Assert(so->keyData[0].sk_flags & SK_SEARCHARRAY);
			Assert(so->keyData[0].sk_strategy != BTEqualStrategyNumber ||
				   (so->arrayKeys[0].scan_key == 0 &&
					!(so->keyData[0].sk_flags & SK_BT_SKIP) &&
					OidIsValid(so->orderProcs[0].fn_oid)));
		}

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
	 * xform[i] points to the currently best scan key of strategy type i+1; it
	 * is NULL if we haven't yet found such a key for this attr.
	 */
	attno = 1;
	memset(xform, 0, sizeof(xform));

	/*
	 * Loop iterates from 0 to numberOfKeys inclusive; we use the last pass to
	 * handle after-last-key processing.  Actual exit from the loop is at the
	 * "break" statement below.
	 */
	for (int i = 0;; i++)
	{
		ScanKey		inkey = inkeys + i;
		int			j;

		if (i < numberOfKeys)
		{
			/* Apply indoption to scankey (might change sk_strategy!) */
			if (!_bt_fix_scankey_strategy(inkey, indoption))
			{
				/* NULL can't be matched, so give up */
				so->qual_ok = false;
				return;
			}
		}

		/*
		 * If we are at the end of the keys for a particular attr, finish up
		 * processing and emit the cleaned-up keys.
		 */
		if (i == numberOfKeys || inkey->sk_attno != attno)
		{
			int			priorNumberOfEqualCols = numberOfEqualCols;

			/* check input keys are correctly ordered */
			if (i < numberOfKeys && inkey->sk_attno < attno)
				elog(ERROR, "btree index keys must be ordered by attribute");

			/*
			 * If = has been specified, all other keys can be eliminated as
			 * redundant.  Note that this is no less true if the = key is
			 * SEARCHARRAY; the only real difference is that the inequality
			 * key _becomes_ redundant by making _bt_compare_scankey_args
			 * eliminate the subset of elements that won't need to be matched
			 * (with SAOP arrays and skip arrays alike).
			 *
			 * If we have a case like "key = 1 AND key > 2", we set qual_ok to
			 * false and abandon further processing.  We'll do the same thing
			 * given a case like "key IN (0, 1) AND key > 2".
			 *
			 * We also have to deal with the case of "key IS NULL", which is
			 * unsatisfiable in combination with any other index condition. By
			 * the time we get here, that's been classified as an equality
			 * check, and we've rejected any combination of it with a regular
			 * equality condition; but not with other types of conditions.
			 */
			if (xform[BTEqualStrategyNumber - 1].inkey)
			{
				ScanKey		eq = xform[BTEqualStrategyNumber - 1].inkey;
				BTArrayKeyInfo *array = NULL;
				FmgrInfo   *orderproc = NULL;

				if (arrayKeyData && (eq->sk_flags & SK_SEARCHARRAY))
				{
					int			eq_in_ikey,
								eq_arrayidx;

					eq_in_ikey = xform[BTEqualStrategyNumber - 1].inkeyi;
					eq_arrayidx = xform[BTEqualStrategyNumber - 1].arrayidx;
					array = &so->arrayKeys[eq_arrayidx - 1];
					orderproc = so->orderProcs + eq_in_ikey;

					Assert(array->scan_key == eq_in_ikey);
					Assert(OidIsValid(orderproc->fn_oid));
				}

				for (j = BTMaxStrategyNumber; --j >= 0;)
				{
					ScanKey		chk = xform[j].inkey;

					if (!chk || j == (BTEqualStrategyNumber - 1))
						continue;

					if (eq->sk_flags & SK_SEARCHNULL)
					{
						/* IS NULL is contradictory to anything else */
						so->qual_ok = false;
						return;
					}

					if (_bt_compare_scankey_args(scan, chk, eq, chk,
												 array, orderproc,
												 &test_result))
					{
						if (!test_result)
						{
							/* keys proven mutually contradictory */
							so->qual_ok = false;
							return;
						}
						/* else discard the redundant non-equality key */
						xform[j].inkey = NULL;
						xform[j].inkeyi = -1;
					}
					/* else, cannot determine redundancy, keep both keys */
				}
				/* track number of attrs for which we have "=" keys */
				numberOfEqualCols++;
			}

			/* try to keep only one of <, <= */
			if (xform[BTLessStrategyNumber - 1].inkey &&
				xform[BTLessEqualStrategyNumber - 1].inkey)
			{
				ScanKey		lt = xform[BTLessStrategyNumber - 1].inkey;
				ScanKey		le = xform[BTLessEqualStrategyNumber - 1].inkey;

				if (_bt_compare_scankey_args(scan, le, lt, le, NULL, NULL,
											 &test_result))
				{
					if (test_result)
						xform[BTLessEqualStrategyNumber - 1].inkey = NULL;
					else
						xform[BTLessStrategyNumber - 1].inkey = NULL;
				}
			}

			/* try to keep only one of >, >= */
			if (xform[BTGreaterStrategyNumber - 1].inkey &&
				xform[BTGreaterEqualStrategyNumber - 1].inkey)
			{
				ScanKey		gt = xform[BTGreaterStrategyNumber - 1].inkey;
				ScanKey		ge = xform[BTGreaterEqualStrategyNumber - 1].inkey;

				if (_bt_compare_scankey_args(scan, ge, gt, ge, NULL, NULL,
											 &test_result))
				{
					if (test_result)
						xform[BTGreaterEqualStrategyNumber - 1].inkey = NULL;
					else
						xform[BTGreaterStrategyNumber - 1].inkey = NULL;
				}
			}

			/*
			 * Emit the cleaned-up keys into the so->keyData[] array, and then
			 * mark them if they are required.  They are required (possibly
			 * only in one direction) if all attrs before this one had "=".
			 *
			 * In practice we'll rarely output non-required scan keys here;
			 * typically, _bt_preprocess_array_keys has already added "=" keys
			 * sufficient to form an unbroken series of "=" constraints on all
			 * attrs prior to the attr from the final scan->keyData[] key.
			 */
			for (j = BTMaxStrategyNumber; --j >= 0;)
			{
				if (xform[j].inkey)
				{
					ScanKey		outkey = &so->keyData[new_numberOfKeys++];

					memcpy(outkey, xform[j].inkey, sizeof(ScanKeyData));
					if (arrayKeyData)
						keyDataMap[new_numberOfKeys - 1] = xform[j].inkeyi;
					if (priorNumberOfEqualCols == attno - 1)
						_bt_mark_scankey_required(outkey);
				}
			}

			/*
			 * Exit loop here if done.
			 */
			if (i == numberOfKeys)
				break;

			/* Re-initialize for new attno */
			attno = inkey->sk_attno;
			memset(xform, 0, sizeof(xform));
		}

		/* check strategy this key's operator corresponds to */
		j = inkey->sk_strategy - 1;

		/* if row comparison, push it directly to the output array */
		if (inkey->sk_flags & SK_ROW_HEADER)
		{
			ScanKey		outkey = &so->keyData[new_numberOfKeys++];

			memcpy(outkey, inkey, sizeof(ScanKeyData));
			if (arrayKeyData)
				keyDataMap[new_numberOfKeys - 1] = i;
			if (numberOfEqualCols == attno - 1)
				_bt_mark_scankey_required(outkey);

			/*
			 * We don't support RowCompare using equality; such a qual would
			 * mess up the numberOfEqualCols tracking.
			 */
			Assert(j != (BTEqualStrategyNumber - 1));
			continue;
		}

		if (inkey->sk_strategy == BTEqualStrategyNumber &&
			(inkey->sk_flags & SK_SEARCHARRAY))
		{
			/* must track how input scan keys map to arrays */
			Assert(arrayKeyData);
			arrayidx++;
		}

		/*
		 * have we seen a scan key for this same attribute and using this same
		 * operator strategy before now?
		 */
		if (xform[j].inkey == NULL)
		{
			/* nope, so this scan key wins by default (at least for now) */
			xform[j].inkey = inkey;
			xform[j].inkeyi = i;
			xform[j].arrayidx = arrayidx;
		}
		else
		{
			FmgrInfo   *orderproc = NULL;
			BTArrayKeyInfo *array = NULL;

			/*
			 * Seen one of these before, so keep only the more restrictive key
			 * if possible
			 */
			if (j == (BTEqualStrategyNumber - 1) && arrayKeyData)
			{
				/*
				 * Have to set up array keys
				 */
				if (inkey->sk_flags & SK_SEARCHARRAY)
				{
					array = &so->arrayKeys[arrayidx - 1];
					orderproc = so->orderProcs + i;

					Assert(array->scan_key == i);
					Assert(OidIsValid(orderproc->fn_oid));
					Assert(!(inkey->sk_flags & SK_BT_SKIP));
				}
				else if (xform[j].inkey->sk_flags & SK_SEARCHARRAY)
				{
					array = &so->arrayKeys[xform[j].arrayidx - 1];
					orderproc = so->orderProcs + xform[j].inkeyi;

					Assert(array->scan_key == xform[j].inkeyi);
					Assert(OidIsValid(orderproc->fn_oid));
					Assert(!(xform[j].inkey->sk_flags & SK_BT_SKIP));
				}

				/*
				 * Both scan keys might have arrays, in which case we'll
				 * arbitrarily pass only one of the arrays.  That won't
				 * matter, since _bt_compare_scankey_args is aware that two
				 * SEARCHARRAY scan keys mean that _bt_preprocess_array_keys
				 * failed to eliminate redundant arrays through array merging.
				 * _bt_compare_scankey_args just returns false when it sees
				 * this; it won't even try to examine either array.
				 */
			}

			if (_bt_compare_scankey_args(scan, inkey, inkey, xform[j].inkey,
										 array, orderproc, &test_result))
			{
				/* Have all we need to determine redundancy */
				if (test_result)
				{
					/*
					 * New key is more restrictive, and so replaces old key...
					 */
					if (j != (BTEqualStrategyNumber - 1) ||
						!(xform[j].inkey->sk_flags & SK_SEARCHARRAY))
					{
						xform[j].inkey = inkey;
						xform[j].inkeyi = i;
						xform[j].arrayidx = arrayidx;
					}
					else
					{
						/*
						 * ...unless we have to keep the old key because it's
						 * an array that rendered the new key redundant.  We
						 * need to make sure that we don't throw away an array
						 * scan key.  _bt_preprocess_array_keys_final expects
						 * us to keep all of the arrays that weren't already
						 * eliminated by _bt_preprocess_array_keys earlier on.
						 */
						Assert(!(inkey->sk_flags & SK_SEARCHARRAY));
					}
				}
				else if (j == (BTEqualStrategyNumber - 1))
				{
					/* key == a && key == b, but a != b */
					so->qual_ok = false;
					return;
				}
				/* else old key is more restrictive, keep it */
			}
			else
			{
				/*
				 * We can't determine which key is more restrictive.  Push
				 * xform[j] directly to the output array, then set xform[j] to
				 * the new scan key.
				 *
				 * Note: We do things this way around so that our arrays are
				 * always in the same order as their corresponding scan keys,
				 * even with incomplete opfamilies.  _bt_advance_array_keys
				 * depends on this.
				 */
				ScanKey		outkey = &so->keyData[new_numberOfKeys++];

				memcpy(outkey, xform[j].inkey, sizeof(ScanKeyData));
				if (arrayKeyData)
					keyDataMap[new_numberOfKeys - 1] = xform[j].inkeyi;
				if (numberOfEqualCols == attno - 1)
					_bt_mark_scankey_required(outkey);
				xform[j].inkey = inkey;
				xform[j].inkeyi = i;
				xform[j].arrayidx = arrayidx;
			}
		}
	}

	so->numberOfKeys = new_numberOfKeys;

	/*
	 * Now that we've built a temporary mapping from so->keyData[] (output
	 * scan keys) to arrayKeyData[] (our input scan keys), fix array->scan_key
	 * references.  Also consolidate the so->orderProcs[] array such that it
	 * can be subscripted using so->keyData[]-wise offsets.
	 */
	if (arrayKeyData)
		_bt_preprocess_array_keys_final(scan, keyDataMap);

	/* Could pfree arrayKeyData/keyDataMap now, but not worth the cycles */
}

/*
 * Adjust a scankey's strategy and flags setting as needed for indoptions.
 *
 * We copy the appropriate indoption value into the scankey sk_flags
 * (shifting to avoid clobbering system-defined flag bits).  Also, if
 * the DESC option is set, commute (flip) the operator strategy number.
 *
 * A secondary purpose is to check for IS NULL/NOT NULL scankeys and set up
 * the strategy field correctly for them.
 *
 * Lastly, for ordinary scankeys (not IS NULL/NOT NULL), we check for a
 * NULL comparison value.  Since all btree operators are assumed strict,
 * a NULL means that the qual cannot be satisfied.  We return true if the
 * comparison value isn't NULL, or false if the scan should be abandoned.
 *
 * This function is applied to the *input* scankey structure; therefore
 * on a rescan we will be looking at already-processed scankeys.  Hence
 * we have to be careful not to re-commute the strategy if we already did it.
 * It's a bit ugly to modify the caller's copy of the scankey but in practice
 * there shouldn't be any problem, since the index's indoptions are certainly
 * not going to change while the scankey survives.
 */
static bool
_bt_fix_scankey_strategy(ScanKey skey, int16 *indoption)
{
	int			addflags;

	addflags = indoption[skey->sk_attno - 1] << SK_BT_INDOPTION_SHIFT;

	/*
	 * We treat all btree operators as strict (even if they're not so marked
	 * in pg_proc). This means that it is impossible for an operator condition
	 * with a NULL comparison constant to succeed, and we can reject it right
	 * away.
	 *
	 * However, we now also support "x IS NULL" clauses as search conditions,
	 * so in that case keep going. The planner has not filled in any
	 * particular strategy in this case, so set it to BTEqualStrategyNumber
	 * --- we can treat IS NULL as an equality operator for purposes of search
	 * strategy.
	 *
	 * Likewise, "x IS NOT NULL" is supported.  We treat that as either "less
	 * than NULL" in a NULLS LAST index, or "greater than NULL" in a NULLS
	 * FIRST index.
	 *
	 * Note: someday we might have to fill in sk_collation from the index
	 * column's collation.  At the moment this is a non-issue because we'll
	 * never actually call the comparison operator on a NULL.
	 */
	if (skey->sk_flags & SK_ISNULL)
	{
		/* SK_ISNULL shouldn't be set in a row header scankey */
		Assert(!(skey->sk_flags & SK_ROW_HEADER));

		/* Set indoption flags in scankey (might be done already) */
		skey->sk_flags |= addflags;

		/* Set correct strategy for IS NULL or NOT NULL search */
		if (skey->sk_flags & SK_SEARCHNULL)
		{
			skey->sk_strategy = BTEqualStrategyNumber;
			skey->sk_subtype = InvalidOid;
			skey->sk_collation = InvalidOid;
		}
		else if (skey->sk_flags & SK_SEARCHNOTNULL)
		{
			if (skey->sk_flags & SK_BT_NULLS_FIRST)
				skey->sk_strategy = BTGreaterStrategyNumber;
			else
				skey->sk_strategy = BTLessStrategyNumber;
			skey->sk_subtype = InvalidOid;
			skey->sk_collation = InvalidOid;
		}
		else
		{
			/* regular qual, so it cannot be satisfied */
			return false;
		}

		/* Needn't do the rest */
		return true;
	}

	/* Adjust strategy for DESC, if we didn't already */
	if ((addflags & SK_BT_DESC) && !(skey->sk_flags & SK_BT_DESC))
		skey->sk_strategy = BTCommuteStrategyNumber(skey->sk_strategy);
	skey->sk_flags |= addflags;

	/* If it's a row header, fix row member flags and strategies similarly */
	if (skey->sk_flags & SK_ROW_HEADER)
	{
		ScanKey		subkey = (ScanKey) DatumGetPointer(skey->sk_argument);

		if (subkey->sk_flags & SK_ISNULL)
		{
			/* First row member is NULL, so RowCompare is unsatisfiable */
			Assert(subkey->sk_flags & SK_ROW_MEMBER);
			return false;
		}

		for (;;)
		{
			Assert(subkey->sk_flags & SK_ROW_MEMBER);
			addflags = indoption[subkey->sk_attno - 1] << SK_BT_INDOPTION_SHIFT;
			if ((addflags & SK_BT_DESC) && !(subkey->sk_flags & SK_BT_DESC))
				subkey->sk_strategy = BTCommuteStrategyNumber(subkey->sk_strategy);
			subkey->sk_flags |= addflags;
			if (subkey->sk_flags & SK_ROW_END)
				break;
			subkey++;
		}
	}

	return true;
}

/*
 * Mark a scankey as "required to continue the scan".
 *
 * Depending on the operator type, the key may be required for both scan
 * directions or just one.  Also, if the key is a row comparison header,
 * we have to mark its first subsidiary ScanKey as required.  (Subsequent
 * subsidiary ScanKeys are normally for lower-order columns, and thus
 * cannot be required, since they're after the first non-equality scankey.)
 *
 * Note: when we set required-key flag bits in a subsidiary scankey, we are
 * scribbling on a data structure belonging to the index AM's caller, not on
 * our private copy.  This should be OK because the marking will not change
 * from scan to scan within a query, and so we'd just re-mark the same way
 * anyway on a rescan.  Something to keep an eye on though.
 */
static void
_bt_mark_scankey_required(ScanKey skey)
{
	int			addflags;

	switch (skey->sk_strategy)
	{
		case BTLessStrategyNumber:
		case BTLessEqualStrategyNumber:
			addflags = SK_BT_REQFWD;
			break;
		case BTEqualStrategyNumber:
			addflags = SK_BT_REQFWD | SK_BT_REQBKWD;
			break;
		case BTGreaterEqualStrategyNumber:
		case BTGreaterStrategyNumber:
			addflags = SK_BT_REQBKWD;
			break;
		default:
			elog(ERROR, "unrecognized StrategyNumber: %d",
				 (int) skey->sk_strategy);
			addflags = 0;		/* keep compiler quiet */
			break;
	}

	skey->sk_flags |= addflags;

	if (skey->sk_flags & SK_ROW_HEADER)
	{
		ScanKey		subkey = (ScanKey) DatumGetPointer(skey->sk_argument);

		/* First subkey should be same column/operator as the header */
		Assert(subkey->sk_flags & SK_ROW_MEMBER);
		Assert(subkey->sk_attno == skey->sk_attno);
		Assert(subkey->sk_strategy == skey->sk_strategy);
		subkey->sk_flags |= addflags;
	}
}

/*
 * Compare two scankey values using a specified operator.
 *
 * The test we want to perform is logically "leftarg op rightarg", where
 * leftarg and rightarg are the sk_argument values in those ScanKeys, and
 * the comparison operator is the one in the op ScanKey.  However, in
 * cross-data-type situations we may need to look up the correct operator in
 * the index's opfamily: it is the one having amopstrategy = op->sk_strategy
 * and amoplefttype/amoprighttype equal to the two argument datatypes.
 *
 * If the opfamily doesn't supply a complete set of cross-type operators we
 * may not be able to make the comparison.  If we can make the comparison
 * we store the operator result in *result and return true.  We return false
 * if the comparison could not be made.
 *
 * If either leftarg or rightarg are an array, we'll apply array-specific
 * rules to determine which array elements are redundant on behalf of caller.
 * It is up to our caller to save whichever of the two scan keys is the array,
 * and discard the non-array scan key (the non-array scan key is guaranteed to
 * be redundant with any complete opfamily).  Caller isn't expected to call
 * here with a pair of array scan keys provided we're dealing with a complete
 * opfamily (_bt_preprocess_array_keys will merge array keys together to make
 * sure of that).
 *
 * Note: we'll also shrink caller's array as needed to eliminate redundant
 * array elements.  One reason why caller should prefer to discard non-array
 * scan keys is so that we'll have the opportunity to shrink the array
 * multiple times, in multiple calls (for each of several other scan keys on
 * the same index attribute).
 *
 * Note: op always points at the same ScanKey as either leftarg or rightarg.
 * Since we don't scribble on the scankeys themselves, this aliasing should
 * cause no trouble.
 *
 * Note: this routine needs to be insensitive to any DESC option applied
 * to the index column.  For example, "x < 4" is a tighter constraint than
 * "x < 5" regardless of which way the index is sorted.
 */
static bool
_bt_compare_scankey_args(IndexScanDesc scan, ScanKey op,
						 ScanKey leftarg, ScanKey rightarg,
						 BTArrayKeyInfo *array, FmgrInfo *orderproc,
						 bool *result)
{
	Relation	rel = scan->indexRelation;
	Oid			lefttype,
				righttype,
				optype,
				opcintype,
				cmp_op;
	StrategyNumber strat;

	Assert(!((leftarg->sk_flags | rightarg->sk_flags) &
			 (SK_ROW_HEADER | SK_ROW_MEMBER)));

	/*
	 * First, deal with cases where one or both args are NULL.  This should
	 * only happen when the scankeys represent IS NULL/NOT NULL conditions.
	 */
	if ((leftarg->sk_flags | rightarg->sk_flags) & SK_ISNULL)
	{
		bool		leftnull,
					rightnull;

		/* Handle skip array comparison with IS NOT NULL scan key */
		if ((leftarg->sk_flags | rightarg->sk_flags) & SK_BT_SKIP)
		{
			/* Shouldn't generate skip array in presence of IS NULL key */
			Assert(!((leftarg->sk_flags | rightarg->sk_flags) & SK_SEARCHNULL));
			Assert((leftarg->sk_flags | rightarg->sk_flags) & SK_SEARCHNOTNULL);

			/* Skip array will have no NULL element/IS NULL scan key */
			Assert(array->num_elems == -1);
			array->null_elem = false;

			/* IS NOT NULL key (could be leftarg or rightarg) now redundant */
			*result = true;
			return true;
		}

		if (leftarg->sk_flags & SK_ISNULL)
		{
			Assert(leftarg->sk_flags & (SK_SEARCHNULL | SK_SEARCHNOTNULL));
			leftnull = true;
		}
		else
			leftnull = false;
		if (rightarg->sk_flags & SK_ISNULL)
		{
			Assert(rightarg->sk_flags & (SK_SEARCHNULL | SK_SEARCHNOTNULL));
			rightnull = true;
		}
		else
			rightnull = false;

		/*
		 * We treat NULL as either greater than or less than all other values.
		 * Since true > false, the tests below work correctly for NULLS LAST
		 * logic.  If the index is NULLS FIRST, we need to flip the strategy.
		 */
		strat = op->sk_strategy;
		if (op->sk_flags & SK_BT_NULLS_FIRST)
			strat = BTCommuteStrategyNumber(strat);

		switch (strat)
		{
			case BTLessStrategyNumber:
				*result = (leftnull < rightnull);
				break;
			case BTLessEqualStrategyNumber:
				*result = (leftnull <= rightnull);
				break;
			case BTEqualStrategyNumber:
				*result = (leftnull == rightnull);
				break;
			case BTGreaterEqualStrategyNumber:
				*result = (leftnull >= rightnull);
				break;
			case BTGreaterStrategyNumber:
				*result = (leftnull > rightnull);
				break;
			default:
				elog(ERROR, "unrecognized StrategyNumber: %d", (int) strat);
				*result = false;	/* keep compiler quiet */
				break;
		}
		return true;
	}

	/*
	 * If either leftarg or rightarg are equality-type array scankeys, we need
	 * specialized handling (since by now we know that IS NULL wasn't used)
	 */
	if (array)
	{
		bool		leftarray,
					rightarray;

		leftarray = ((leftarg->sk_flags & SK_SEARCHARRAY) &&
					 leftarg->sk_strategy == BTEqualStrategyNumber);
		rightarray = ((rightarg->sk_flags & SK_SEARCHARRAY) &&
					  rightarg->sk_strategy == BTEqualStrategyNumber);

		/*
		 * _bt_preprocess_array_keys is responsible for merging together array
		 * scan keys, and will do so whenever the opfamily has the required
		 * cross-type support.  If it failed to do that, we handle it just
		 * like the case where we can't make the comparison ourselves.
		 */
		if (leftarray && rightarray)
		{
			/* Can't make the comparison */
			*result = false;	/* suppress compiler warnings */
			Assert(!((leftarg->sk_flags | rightarg->sk_flags) & SK_BT_SKIP));
			return false;
		}

		/*
		 * Otherwise we need to determine if either one of leftarg or rightarg
		 * uses an array, then pass this through to a dedicated helper
		 * function.
		 */
		if (leftarray)
			return _bt_compare_array_scankey_args(scan, leftarg, rightarg,
												  orderproc, array, result);
		else if (rightarray)
			return _bt_compare_array_scankey_args(scan, rightarg, leftarg,
												  orderproc, array, result);

		/* FALL THRU */
	}

	/*
	 * The opfamily we need to worry about is identified by the index column.
	 */
	Assert(leftarg->sk_attno == rightarg->sk_attno);

	opcintype = rel->rd_opcintype[leftarg->sk_attno - 1];

	/*
	 * Determine the actual datatypes of the ScanKey arguments.  We have to
	 * support the convention that sk_subtype == InvalidOid means the opclass
	 * input type; this is a hack to simplify life for ScanKeyInit().
	 */
	lefttype = leftarg->sk_subtype;
	if (lefttype == InvalidOid)
		lefttype = opcintype;
	righttype = rightarg->sk_subtype;
	if (righttype == InvalidOid)
		righttype = opcintype;
	optype = op->sk_subtype;
	if (optype == InvalidOid)
		optype = opcintype;

	/*
	 * If leftarg and rightarg match the types expected for the "op" scankey,
	 * we can use its already-looked-up comparison function.
	 */
	if (lefttype == opcintype && righttype == optype)
	{
		*result = DatumGetBool(FunctionCall2Coll(&op->sk_func,
												 op->sk_collation,
												 leftarg->sk_argument,
												 rightarg->sk_argument));
		return true;
	}

	/*
	 * Otherwise, we need to go to the syscache to find the appropriate
	 * operator.  (This cannot result in infinite recursion, since no
	 * indexscan initiated by syscache lookup will use cross-data-type
	 * operators.)
	 *
	 * If the sk_strategy was flipped by _bt_fix_scankey_strategy, we have to
	 * un-flip it to get the correct opfamily member.
	 */
	strat = op->sk_strategy;
	if (op->sk_flags & SK_BT_DESC)
		strat = BTCommuteStrategyNumber(strat);

	cmp_op = get_opfamily_member(rel->rd_opfamily[leftarg->sk_attno - 1],
								 lefttype,
								 righttype,
								 strat);
	if (OidIsValid(cmp_op))
	{
		RegProcedure cmp_proc = get_opcode(cmp_op);

		if (RegProcedureIsValid(cmp_proc))
		{
			*result = DatumGetBool(OidFunctionCall2Coll(cmp_proc,
														op->sk_collation,
														leftarg->sk_argument,
														rightarg->sk_argument));
			return true;
		}
	}

	/* Can't make the comparison */
	*result = false;			/* suppress compiler warnings */
	return false;
}

/*
 * Compare an array scan key to a scalar scan key, eliminating contradictory
 * array elements such that the scalar scan key becomes redundant.
 *
 * If the opfamily is incomplete we may not be able to determine which
 * elements are contradictory.  When we return true we'll have validly set
 * *qual_ok, guaranteeing that at least the scalar scan key can be considered
 * redundant.  We return false if the comparison could not be made (caller
 * must keep both scan keys when this happens).
 *
 * Note: it's up to caller to deal with IS [NOT] NULL scan keys, as well as
 * row comparison scan keys.  We only deal with scalar scan keys.
 */
static bool
_bt_compare_array_scankey_args(IndexScanDesc scan, ScanKey arraysk, ScanKey skey,
							   FmgrInfo *orderproc, BTArrayKeyInfo *array,
							   bool *qual_ok)
{
	Assert(arraysk->sk_attno == skey->sk_attno);
	Assert(!(arraysk->sk_flags & (SK_ISNULL | SK_ROW_HEADER | SK_ROW_MEMBER)));
	Assert((arraysk->sk_flags & SK_SEARCHARRAY) &&
		   arraysk->sk_strategy == BTEqualStrategyNumber);
	/* don't expect to have to deal with NULLs/row comparison scan keys */
	Assert(!(skey->sk_flags & (SK_ISNULL | SK_ROW_HEADER | SK_ROW_MEMBER)));
	Assert(!(skey->sk_flags & SK_SEARCHARRAY) ||
		   skey->sk_strategy != BTEqualStrategyNumber);

	/*
	 * Just call the appropriate helper function based on whether it's a SAOP
	 * array or a skip array.  Both helpers will set *qual_ok in passing.
	 */
	if (array->num_elems != -1)
		return _bt_saoparray_shrink(scan, arraysk, skey, orderproc, array,
									qual_ok);
	else
		return _bt_skiparray_shrink(scan, skey, array, qual_ok);
}

/*
 * Preprocessing of SAOP array scan key, used to determine which array
 * elements are eliminated as contradictory by a non-array scalar key.
 *
 * _bt_compare_array_scankey_args helper function.
 *
 * Array elements can be eliminated as contradictory when excluded by some
 * other operator on the same attribute.  For example, with an index scan qual
 * "WHERE a IN (1, 2, 3) AND a < 2", all array elements except the value "1"
 * are eliminated, and the < scan key is eliminated as redundant.  Cases where
 * every array element is eliminated by a redundant scalar scan key have an
 * unsatisfiable qual, which we handle by setting *qual_ok=false for caller.
 */
static bool
_bt_saoparray_shrink(IndexScanDesc scan, ScanKey arraysk, ScanKey skey,
					 FmgrInfo *orderproc, BTArrayKeyInfo *array, bool *qual_ok)
{
	Relation	rel = scan->indexRelation;
	Oid			opcintype = rel->rd_opcintype[arraysk->sk_attno - 1];
	int			cmpresult = 0,
				cmpexact = 0,
				matchelem,
				new_nelems = 0;
	FmgrInfo	crosstypeproc;
	FmgrInfo   *orderprocp = orderproc;

	Assert(array->num_elems > 0);
	Assert(!(arraysk->sk_flags & SK_BT_SKIP));

	/*
	 * _bt_binsrch_array_skey searches an array for the entry best matching a
	 * datum of opclass input type for the index's attribute (on-disk type).
	 * We can reuse the array's ORDER proc whenever the non-array scan key's
	 * type is a match for the corresponding attribute's input opclass type.
	 * Otherwise, we have to do another ORDER proc lookup so that our call to
	 * _bt_binsrch_array_skey applies the correct comparator.
	 *
	 * Note: we have to support the convention that sk_subtype == InvalidOid
	 * means the opclass input type; this is a hack to simplify life for
	 * ScanKeyInit().
	 */
	if (skey->sk_subtype != opcintype && skey->sk_subtype != InvalidOid)
	{
		RegProcedure cmp_proc;
		Oid			arraysk_elemtype;

		/*
		 * Need an ORDER proc lookup to detect redundancy/contradictoriness
		 * with this pair of scankeys.
		 *
		 * Scalar scan key's argument will be passed to _bt_compare_array_skey
		 * as its tupdatum/lefthand argument (rhs arg is for array elements).
		 */
		arraysk_elemtype = arraysk->sk_subtype;
		if (arraysk_elemtype == InvalidOid)
			arraysk_elemtype = rel->rd_opcintype[arraysk->sk_attno - 1];
		cmp_proc = get_opfamily_proc(rel->rd_opfamily[arraysk->sk_attno - 1],
									 skey->sk_subtype, arraysk_elemtype,
									 BTORDER_PROC);
		if (!RegProcedureIsValid(cmp_proc))
		{
			/* Can't make the comparison */
			*qual_ok = false;	/* suppress compiler warnings */
			return false;
		}

		/* We have all we need to determine redundancy/contradictoriness */
		orderprocp = &crosstypeproc;
		fmgr_info(cmp_proc, orderprocp);
	}

	matchelem = _bt_binsrch_array_skey(orderprocp, false,
									   NoMovementScanDirection,
									   skey->sk_argument, false, array,
									   arraysk, &cmpresult);

	switch (skey->sk_strategy)
	{
		case BTLessStrategyNumber:
			cmpexact = 1;		/* exclude exact match, if any */
			/* FALL THRU */
		case BTLessEqualStrategyNumber:
			if (cmpresult >= cmpexact)
				matchelem++;
			/* Resize, keeping elements from the start of the array */
			new_nelems = matchelem;
			break;
		case BTEqualStrategyNumber:
			if (cmpresult != 0)
			{
				/* qual is unsatisfiable */
				new_nelems = 0;
			}
			else
			{
				/* Shift matching element to the start of the array, resize */
				array->elem_values[0] = array->elem_values[matchelem];
				new_nelems = 1;
			}
			break;
		case BTGreaterEqualStrategyNumber:
			cmpexact = 1;		/* include exact match, if any */
			/* FALL THRU */
		case BTGreaterStrategyNumber:
			if (cmpresult >= cmpexact)
				matchelem++;
			/* Shift matching elements to the start of the array, resize */
			new_nelems = array->num_elems - matchelem;
			memmove(array->elem_values, array->elem_values + matchelem,
					sizeof(Datum) * new_nelems);
			break;
		default:
			elog(ERROR, "unrecognized StrategyNumber: %d",
				 (int) skey->sk_strategy);
			break;
	}

	Assert(new_nelems >= 0);
	Assert(new_nelems <= array->num_elems);

	array->num_elems = new_nelems;
	*qual_ok = new_nelems > 0;

	return true;
}

/*
 * Preprocessing of skip array scan key, used to determine redundancy against
 * a non-array scalar scan key (must be an inequality).
 *
 * _bt_compare_array_scankey_args helper function.
 *
 * Skip arrays work by procedurally generating their elements as needed, so we
 * just store the inequality as the skip array's low_compare or high_compare
 * (except when there's already a more restrictive low_compare/high_compare).
 * The array's final elements are the range of values that still satisfy the
 * array's final low_compare and high_compare.
 */
static bool
_bt_skiparray_shrink(IndexScanDesc scan, ScanKey skey, BTArrayKeyInfo *array,
					 bool *qual_ok)
{
	bool		test_result;

	Assert(array->num_elems == -1);

	/*
	 * Array's index attribute will be constrained by a strict operator/key.
	 * Array must not "contain a NULL element" (i.e. the scan must not apply
	 * "IS NULL" qual when it reaches the end of the index that stores NULLs).
	 */
	array->null_elem = false;
	*qual_ok = true;

	/*
	 * Consider if we should treat caller's scalar scan key as the skip
	 * array's high_compare or low_compare.
	 *
	 * In general the current array element must either be a copy of a value
	 * taken from an index tuple, or a derivative value generated by opclass's
	 * skip support function.  That way the scan can always safely assume that
	 * it's okay to use the only-input-opclass-type proc from so->orderProcs[]
	 * (they can be cross-type with SAOP arrays, but never with skip arrays).
	 *
	 * This approach is enabled by MINVAL/MAXVAL sentinel key markings, which
	 * can be thought of as representing either the lowest or highest matching
	 * array element (excluding the NULL element, where applicable, though as
	 * just discussed it isn't applicable to this range skip array anyway).
	 * Array keys marked MINVAL/MAXVAL never have a valid datum in their
	 * sk_argument field.  The scan directly applies the array's low_compare
	 * key when it encounters MINVAL in the array key proper (just as it
	 * applies high_compare when it sees MAXVAL set in the array key proper).
	 * The scan must never use the array's so->orderProcs[] proc against
	 * low_compare's/high_compare's sk_argument, either (so->orderProcs[] is
	 * only intended to be used with rhs datums from the array proper/index).
	 */
	switch (skey->sk_strategy)
	{
		case BTLessStrategyNumber:
		case BTLessEqualStrategyNumber:
			if (array->high_compare)
			{
				/* replace existing high_compare with caller's key? */
				if (!_bt_compare_scankey_args(scan, array->high_compare, skey,
											  array->high_compare, NULL, NULL,
											  &test_result))
					return false;	/* can't determine more restrictive key */

				if (!test_result)
					return true;	/* no, just discard caller's key */

				/* yes, replace existing high_compare with caller's key */
			}

			/* caller's key becomes skip array's high_compare */
			array->high_compare = skey;
			break;
		case BTGreaterEqualStrategyNumber:
		case BTGreaterStrategyNumber:
			if (array->low_compare)
			{
				/* replace existing low_compare with caller's key? */
				if (!_bt_compare_scankey_args(scan, array->low_compare, skey,
											  array->low_compare, NULL, NULL,
											  &test_result))
					return false;	/* can't determine more restrictive key */

				if (!test_result)
					return true;	/* no, just discard caller's key */

				/* yes, replace existing low_compare with caller's key */
			}

			/* caller's key becomes skip array's low_compare */
			array->low_compare = skey;
			break;
		case BTEqualStrategyNumber:
		default:
			elog(ERROR, "unrecognized StrategyNumber: %d",
				 (int) skey->sk_strategy);
			break;
	}

	return true;
}

/*
 * Applies the opfamily's skip support routine to convert the skip array's >
 * low_compare key (if any) into a >= key, and to convert its < high_compare
 * key (if any) into a <= key.  Decrements the high_compare key's sk_argument,
 * and/or increments the low_compare key's sk_argument (also adjusts their
 * operator strategies, while changing the operator as appropriate).
 *
 * This optional optimization reduces the number of descents required within
 * _bt_first.  Whenever _bt_first is called with a skip array whose current
 * array element is the sentinel value MINVAL, using a transformed >= key
 * instead of using the original > key makes it safe to include lower-order
 * scan keys in the insertion scan key (there must be lower-order scan keys
 * after the skip array).  We will avoid an extra _bt_first to find the first
 * value in the index > sk_argument -- at least when the first real matching
 * value in the index happens to be an exact match for the sk_argument value
 * that we produced here by incrementing the original input key's sk_argument.
 * (Backwards scans derive the same benefit when they encounter the sentinel
 * value MAXVAL, by converting the high_compare key from < to <=.)
 *
 * Note: The transformation is only correct when it cannot allow the scan to
 * overlook matching tuples, but we don't have enough semantic information to
 * safely make sure that can't happen during scans with cross-type operators.
 * That's why we'll never apply the transformation in cross-type scenarios.
 * For example, if we attempted to convert "sales_ts > '2024-01-01'::date"
 * into "sales_ts >= '2024-01-02'::date" given a "sales_ts" attribute whose
 * input opclass is timestamp_ops, the scan would overlook almost all (or all)
 * tuples for sales that fell on '2024-01-01'.
 *
 * Note: We can safely modify array->low_compare/array->high_compare in place
 * because they just point to copies of our scan->keyData[] input scan keys
 * (namely the copies returned by _bt_preprocess_array_keys to be used as
 * input into the standard preprocessing steps in _bt_preprocess_keys).
 * Everything will be reset if there's a rescan.
 */
static void
_bt_skiparray_strat_adjust(IndexScanDesc scan, ScanKey arraysk,
						   BTArrayKeyInfo *array)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	MemoryContext oldContext;

	/*
	 * Called last among all preprocessing steps, when the skip array's final
	 * low_compare and high_compare have both been chosen
	 */
	Assert(arraysk->sk_flags & SK_BT_SKIP);
	Assert(array->num_elems == -1 && !array->null_elem && array->sksup);

	oldContext = MemoryContextSwitchTo(so->arrayContext);

	if (array->high_compare &&
		array->high_compare->sk_strategy == BTLessStrategyNumber)
		_bt_skiparray_strat_decrement(scan, arraysk, array);

	if (array->low_compare &&
		array->low_compare->sk_strategy == BTGreaterStrategyNumber)
		_bt_skiparray_strat_increment(scan, arraysk, array);

	MemoryContextSwitchTo(oldContext);
}

/*
 * Convert skip array's > low_compare key into a >= key
 */
static void
_bt_skiparray_strat_decrement(IndexScanDesc scan, ScanKey arraysk,
							  BTArrayKeyInfo *array)
{
	Relation	rel = scan->indexRelation;
	Oid			opfamily = rel->rd_opfamily[arraysk->sk_attno - 1],
				opcintype = rel->rd_opcintype[arraysk->sk_attno - 1],
				leop;
	RegProcedure cmp_proc;
	ScanKey		high_compare = array->high_compare;
	Datum		orig_sk_argument = high_compare->sk_argument,
				new_sk_argument;
	bool		uflow;

	Assert(high_compare->sk_strategy == BTLessStrategyNumber);

	/*
	 * Only perform the transformation when the operator type matches the
	 * index attribute's input opclass type
	 */
	if (high_compare->sk_subtype != opcintype &&
		high_compare->sk_subtype != InvalidOid)
		return;

	/* Decrement, handling underflow by marking the qual unsatisfiable */
	new_sk_argument = array->sksup->decrement(rel, orig_sk_argument, &uflow);
	if (uflow)
	{
		BTScanOpaque so = (BTScanOpaque) scan->opaque;

		so->qual_ok = false;
		return;
	}

	/* Look up <= operator (might fail) */
	leop = get_opfamily_member(opfamily, opcintype, opcintype,
							   BTLessEqualStrategyNumber);
	if (!OidIsValid(leop))
		return;
	cmp_proc = get_opcode(leop);
	if (RegProcedureIsValid(cmp_proc))
	{
		/* Transform < high_compare key into <= key */
		fmgr_info(cmp_proc, &high_compare->sk_func);
		high_compare->sk_argument = new_sk_argument;
		high_compare->sk_strategy = BTLessEqualStrategyNumber;
	}
}

/*
 * Convert skip array's < low_compare key into a <= key
 */
static void
_bt_skiparray_strat_increment(IndexScanDesc scan, ScanKey arraysk,
							  BTArrayKeyInfo *array)
{
	Relation	rel = scan->indexRelation;
	Oid			opfamily = rel->rd_opfamily[arraysk->sk_attno - 1],
				opcintype = rel->rd_opcintype[arraysk->sk_attno - 1],
				geop;
	RegProcedure cmp_proc;
	ScanKey		low_compare = array->low_compare;
	Datum		orig_sk_argument = low_compare->sk_argument,
				new_sk_argument;
	bool		oflow;

	Assert(low_compare->sk_strategy == BTGreaterStrategyNumber);

	/*
	 * Only perform the transformation when the operator type matches the
	 * index attribute's input opclass type
	 */
	if (low_compare->sk_subtype != opcintype &&
		low_compare->sk_subtype != InvalidOid)
		return;

	/* Increment, handling overflow by marking the qual unsatisfiable */
	new_sk_argument = array->sksup->increment(rel, orig_sk_argument, &oflow);
	if (oflow)
	{
		BTScanOpaque so = (BTScanOpaque) scan->opaque;

		so->qual_ok = false;
		return;
	}

	/* Look up >= operator (might fail) */
	geop = get_opfamily_member(opfamily, opcintype, opcintype,
							   BTGreaterEqualStrategyNumber);
	if (!OidIsValid(geop))
		return;
	cmp_proc = get_opcode(geop);
	if (RegProcedureIsValid(cmp_proc))
	{
		/* Transform > low_compare key into >= key */
		fmgr_info(cmp_proc, &low_compare->sk_func);
		low_compare->sk_argument = new_sk_argument;
		low_compare->sk_strategy = BTGreaterEqualStrategyNumber;
	}
}

/*
 *	_bt_preprocess_array_keys() -- Preprocess SK_SEARCHARRAY scan keys
 *
 * If there are any SK_SEARCHARRAY scan keys, deconstruct the array(s) and
 * set up BTArrayKeyInfo info for each one that is an equality-type key.
 * Returns modified scan keys as input for further, standard preprocessing.
 *
 * Currently we perform two kinds of preprocessing to deal with redundancies.
 * For inequality array keys, it's sufficient to find the extreme element
 * value and replace the whole array with that scalar value.  This eliminates
 * all but one array element as redundant.  Similarly, we are capable of
 * "merging together" multiple equality array keys (from two or more input
 * scan keys) into a single output scan key containing only the intersecting
 * array elements.  This can eliminate many redundant array elements, as well
 * as eliminating whole array scan keys as redundant.  It can also allow us to
 * detect contradictory quals.
 *
 * Caller must pass *new_numberOfKeys to give us a way to change the number of
 * scan keys that caller treats as input to standard preprocessing steps.  The
 * returned array is smaller than scan->keyData[] when we could eliminate a
 * redundant array scan key (redundant with another array scan key).  It is
 * convenient for _bt_preprocess_keys caller to have to deal with no more than
 * one equality strategy array scan key per index attribute.  We'll always be
 * able to set things up that way when complete opfamilies are used.
 *
 * We're also responsible for generating skip arrays (and their associated
 * scan keys) here.  This enables skip scan.  We do this for index attributes
 * that initially lacked an equality condition within scan->keyData[], iff
 * doing so allows a later scan key (that was passed to us in scan->keyData[])
 * to be marked required by our _bt_preprocess_keys caller.
 *
 * We set the scan key references from the scan's BTArrayKeyInfo info array to
 * offsets into the temp modified input array returned to caller.  Scans that
 * have array keys should call _bt_preprocess_array_keys_final when standard
 * preprocessing steps are complete.  This will convert the scan key offset
 * references into references to the scan's so->keyData[] output scan keys.
 *
 * Note: the reason we need to return a temp scan key array, rather than just
 * modifying scan->keyData[], is that callers are permitted to call btrescan
 * without supplying a new set of scankey data.  Certain other preprocessing
 * routines (e.g., _bt_fix_scankey_strategy) _can_ modify scan->keyData[], but
 * we can't make that work here because our modifications are non-idempotent.
 */
static ScanKey
_bt_preprocess_array_keys(IndexScanDesc scan, int *new_numberOfKeys)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	Relation	rel = scan->indexRelation;
	int16	   *indoption = rel->rd_indoption;
	Oid			skip_eq_ops[INDEX_MAX_KEYS];
	int			numArrayKeys,
				numSkipArrayKeys,
				numArrayKeyData;
	AttrNumber	attno_skip = 1;
	int			origarrayatt = InvalidAttrNumber,
				origarraykey = -1;
	Oid			origelemtype = InvalidOid;
	MemoryContext oldContext;
	ScanKey		arrayKeyData;	/* modified copy of scan->keyData */

	/*
	 * Check the number of input array keys within scan->keyData[] input keys
	 * (also checks if we should add extra skip arrays based on input keys)
	 */
	numArrayKeys = _bt_num_array_keys(scan, skip_eq_ops, &numSkipArrayKeys);

	/* Quit if nothing to do. */
	if (numArrayKeys == 0)
		return NULL;

	/*
	 * Estimated final size of arrayKeyData[] array we'll return to our caller
	 * is the size of the original scan->keyData[] input array, plus space for
	 * any additional skip array scan keys we'll need to generate below
	 */
	numArrayKeyData = scan->numberOfKeys + numSkipArrayKeys;

	/*
	 * Make a scan-lifespan context to hold array-associated data, or reset it
	 * if we already have one from a previous rescan cycle.
	 */
	if (so->arrayContext == NULL)
		so->arrayContext = AllocSetContextCreate(CurrentMemoryContext,
												 "BTree array context",
												 ALLOCSET_SMALL_SIZES);
	else
		MemoryContextReset(so->arrayContext);

	oldContext = MemoryContextSwitchTo(so->arrayContext);

	/* Create output scan keys in the workspace context */
	arrayKeyData = (ScanKey) palloc(numArrayKeyData * sizeof(ScanKeyData));

	/* Allocate space for per-array data in the workspace context */
	so->skipScan = (numSkipArrayKeys > 0);
	so->arrayKeys = (BTArrayKeyInfo *) palloc(numArrayKeys * sizeof(BTArrayKeyInfo));

	/* Allocate space for ORDER procs used to help _bt_checkkeys */
	so->orderProcs = (FmgrInfo *) palloc(numArrayKeyData * sizeof(FmgrInfo));

	numArrayKeys = 0;
	numArrayKeyData = 0;
	for (int input_ikey = 0; input_ikey < scan->numberOfKeys; input_ikey++)
	{
		ScanKey		inkey = scan->keyData + input_ikey,
					cur;
		FmgrInfo	sortproc;
		FmgrInfo   *sortprocp = &sortproc;
		Oid			elemtype;
		bool		reverse;
		ArrayType  *arrayval;
		int16		elmlen;
		bool		elmbyval;
		char		elmalign;
		int			num_elems;
		Datum	   *elem_values;
		bool	   *elem_nulls;
		int			num_nonnulls;

		/* set up next output scan key */
		cur = &arrayKeyData[numArrayKeyData];

		/* Backfill skip arrays for attrs < or <= input key's attr? */
		while (numSkipArrayKeys && attno_skip <= inkey->sk_attno)
		{
			Oid			opfamily = rel->rd_opfamily[attno_skip - 1];
			Oid			opcintype = rel->rd_opcintype[attno_skip - 1];
			Oid			collation = rel->rd_indcollation[attno_skip - 1];
			Oid			eq_op = skip_eq_ops[attno_skip - 1];
			CompactAttribute *attr;
			RegProcedure cmp_proc;

			if (!OidIsValid(eq_op))
			{
				/*
				 * Attribute already has an = input key, so don't output a
				 * skip array for attno_skip.  Just copy attribute's = input
				 * key into arrayKeyData[] once outside this inner loop.
				 *
				 * Note: When we get here there must be a later attribute that
				 * lacks an equality input key, and still needs a skip array
				 * (if there wasn't then numSkipArrayKeys would be 0 by now).
				 */
				Assert(attno_skip == inkey->sk_attno);
				/* inkey can't be last input key to be marked required: */
				Assert(input_ikey < scan->numberOfKeys - 1);
#if 0
				/* Could be a redundant input scan key, so can't do this: */
				Assert(inkey->sk_strategy == BTEqualStrategyNumber ||
					   (inkey->sk_flags & SK_SEARCHNULL));
#endif

				attno_skip++;
				break;
			}

			cmp_proc = get_opcode(eq_op);
			if (!RegProcedureIsValid(cmp_proc))
				elog(ERROR, "missing oprcode for skipping equals operator %u", eq_op);

			ScanKeyEntryInitialize(cur,
								   SK_SEARCHARRAY | SK_BT_SKIP, /* flags */
								   attno_skip,	/* skipped att number */
								   BTEqualStrategyNumber,	/* equality strategy */
								   InvalidOid,	/* opclass input subtype */
								   collation,	/* index column's collation */
								   cmp_proc,	/* equality operator's proc */
								   (Datum) 0);	/* constant */

			/* Initialize generic BTArrayKeyInfo fields */
			so->arrayKeys[numArrayKeys].scan_key = numArrayKeyData;
			so->arrayKeys[numArrayKeys].num_elems = -1;

			/* Initialize skip array specific BTArrayKeyInfo fields */
			attr = TupleDescCompactAttr(RelationGetDescr(rel), attno_skip - 1);
			reverse = (indoption[attno_skip - 1] & INDOPTION_DESC) != 0;
			so->arrayKeys[numArrayKeys].attlen = attr->attlen;
			so->arrayKeys[numArrayKeys].attbyval = attr->attbyval;
			so->arrayKeys[numArrayKeys].null_elem = true;	/* for now */
			so->arrayKeys[numArrayKeys].sksup =
				PrepareSkipSupportFromOpclass(opfamily, opcintype, reverse);
			so->arrayKeys[numArrayKeys].low_compare = NULL; /* for now */
			so->arrayKeys[numArrayKeys].high_compare = NULL;	/* for now */

			/*
			 * We'll need a 3-way ORDER proc.  Set that up now.
			 */
			_bt_setup_array_cmp(scan, cur, opcintype,
								&so->orderProcs[numArrayKeyData], NULL);

			numArrayKeys++;
			numArrayKeyData++;	/* keep this scan key/array */

			/* set up next output scan key */
			cur = &arrayKeyData[numArrayKeyData];

			/* remember having output this skip array and scan key */
			numSkipArrayKeys--;
			attno_skip++;
		}

		/*
		 * Provisionally copy scan key into arrayKeyData[] array we'll return
		 * to _bt_preprocess_keys caller
		 */
		*cur = *inkey;

		if (!(cur->sk_flags & SK_SEARCHARRAY))
		{
			numArrayKeyData++;	/* keep this non-array scan key */
			continue;
		}

		/*
		 * Process SAOP array scan key
		 */
		Assert(!(cur->sk_flags & (SK_ROW_HEADER | SK_SEARCHNULL | SK_SEARCHNOTNULL)));

		/* If array is null as a whole, the scan qual is unsatisfiable */
		if (cur->sk_flags & SK_ISNULL)
		{
			so->qual_ok = false;
			break;
		}

		/*
		 * Deconstruct the array into elements
		 */
		arrayval = DatumGetArrayTypeP(cur->sk_argument);
		/* We could cache this data, but not clear it's worth it */
		get_typlenbyvalalign(ARR_ELEMTYPE(arrayval),
							 &elmlen, &elmbyval, &elmalign);
		deconstruct_array(arrayval,
						  ARR_ELEMTYPE(arrayval),
						  elmlen, elmbyval, elmalign,
						  &elem_values, &elem_nulls, &num_elems);

		/*
		 * Compress out any null elements.  We can ignore them since we assume
		 * all btree operators are strict.
		 */
		num_nonnulls = 0;
		for (int j = 0; j < num_elems; j++)
		{
			if (!elem_nulls[j])
				elem_values[num_nonnulls++] = elem_values[j];
		}

		/* We could pfree(elem_nulls) now, but not worth the cycles */

		/* If there's no non-nulls, the scan qual is unsatisfiable */
		if (num_nonnulls == 0)
		{
			so->qual_ok = false;
			break;
		}

		/*
		 * Determine the nominal datatype of the array elements.  We have to
		 * support the convention that sk_subtype == InvalidOid means the
		 * opclass input type; this is a hack to simplify life for
		 * ScanKeyInit().
		 */
		elemtype = cur->sk_subtype;
		if (elemtype == InvalidOid)
			elemtype = rel->rd_opcintype[cur->sk_attno - 1];

		/*
		 * If the comparison operator is not equality, then the array qual
		 * degenerates to a simple comparison against the smallest or largest
		 * non-null array element, as appropriate.
		 */
		switch (cur->sk_strategy)
		{
			case BTLessStrategyNumber:
			case BTLessEqualStrategyNumber:
				cur->sk_argument =
					_bt_find_extreme_element(scan, cur, elemtype,
											 BTGreaterStrategyNumber,
											 elem_values, num_nonnulls);
				numArrayKeyData++;	/* keep this transformed scan key */
				continue;
			case BTEqualStrategyNumber:
				/* proceed with rest of loop */
				break;
			case BTGreaterEqualStrategyNumber:
			case BTGreaterStrategyNumber:
				cur->sk_argument =
					_bt_find_extreme_element(scan, cur, elemtype,
											 BTLessStrategyNumber,
											 elem_values, num_nonnulls);
				numArrayKeyData++;	/* keep this transformed scan key */
				continue;
			default:
				elog(ERROR, "unrecognized StrategyNumber: %d",
					 (int) cur->sk_strategy);
				break;
		}

		/*
		 * We'll need a 3-way ORDER proc to perform binary searches for the
		 * next matching array element.  Set that up now.
		 *
		 * Array scan keys with cross-type equality operators will require a
		 * separate same-type ORDER proc for sorting their array.  Otherwise,
		 * sortproc just points to the same proc used during binary searches.
		 */
		_bt_setup_array_cmp(scan, cur, elemtype,
							&so->orderProcs[numArrayKeyData], &sortprocp);

		/*
		 * Sort the non-null elements and eliminate any duplicates.  We must
		 * sort in the same ordering used by the index column, so that the
		 * arrays can be advanced in lockstep with the scan's progress through
		 * the index's key space.
		 */
		reverse = (indoption[cur->sk_attno - 1] & INDOPTION_DESC) != 0;
		num_elems = _bt_sort_array_elements(cur, sortprocp, reverse,
											elem_values, num_nonnulls);

		if (origarrayatt == cur->sk_attno)
		{
			BTArrayKeyInfo *orig = &so->arrayKeys[origarraykey];

			/*
			 * This array scan key is redundant with a previous equality
			 * operator array scan key.  Merge the two arrays together to
			 * eliminate contradictory non-intersecting elements (or try to).
			 *
			 * We merge this next array back into attribute's original array.
			 */
			Assert(arrayKeyData[orig->scan_key].sk_attno == cur->sk_attno);
			Assert(arrayKeyData[orig->scan_key].sk_collation ==
				   cur->sk_collation);
			if (_bt_merge_arrays(scan, cur, sortprocp, reverse,
								 origelemtype, elemtype,
								 orig->elem_values, &orig->num_elems,
								 elem_values, num_elems))
			{
				/* Successfully eliminated this array */
				pfree(elem_values);

				/*
				 * If no intersecting elements remain in the original array,
				 * the scan qual is unsatisfiable
				 */
				if (orig->num_elems == 0)
				{
					so->qual_ok = false;
					break;
				}

				/* Throw away this scan key/array */
				continue;
			}

			/*
			 * Unable to merge this array with previous array due to a lack of
			 * suitable cross-type opfamily support.  Will need to keep both
			 * scan keys/arrays.
			 */
		}
		else
		{
			/*
			 * This array is the first for current index attribute.
			 *
			 * If it turns out to not be the last array (that is, if the next
			 * array is redundantly applied to this same index attribute),
			 * we'll then treat this array as the attribute's "original" array
			 * when merging.
			 */
			origarrayatt = cur->sk_attno;
			origarraykey = numArrayKeys;
			origelemtype = elemtype;
		}

		/* Initialize generic BTArrayKeyInfo fields */
		so->arrayKeys[numArrayKeys].scan_key = numArrayKeyData;
		so->arrayKeys[numArrayKeys].num_elems = num_elems;

		/* Initialize SAOP array specific BTArrayKeyInfo fields */
		so->arrayKeys[numArrayKeys].elem_values = elem_values;
		so->arrayKeys[numArrayKeys].cur_elem = -1;	/* i.e. invalid */

		numArrayKeys++;
		numArrayKeyData++;		/* keep this scan key/array */
	}

	Assert(numSkipArrayKeys == 0 || !so->qual_ok);

	/* Set final number of equality-type array keys */
	so->numArrayKeys = numArrayKeys;
	/* Set number of scan keys in arrayKeyData[] */
	*new_numberOfKeys = numArrayKeyData;

	MemoryContextSwitchTo(oldContext);

	return arrayKeyData;
}

/*
 *	_bt_preprocess_array_keys_final() -- fix up array scan key references
 *
 * When _bt_preprocess_array_keys performed initial array preprocessing, it
 * set each array's array->scan_key to its scankey's arrayKeyData[] offset.
 * This function handles translation of the scan key references from the
 * BTArrayKeyInfo info array, from input scan key references (to the keys in
 * arrayKeyData[]), into output references (to the keys in so->keyData[]).
 * Caller's keyDataMap[] array tells us how to perform this remapping.
 *
 * Also finalizes so->orderProcs[] for the scan.  Arrays already have an ORDER
 * proc, which might need to be repositioned to its so->keyData[]-wise offset
 * (very much like the remapping that we apply to array->scan_key references).
 * Non-array equality strategy scan keys (that survived preprocessing) don't
 * yet have an so->orderProcs[] entry, so we set one for them here.
 *
 * Also converts single-element array scan keys into equivalent non-array
 * equality scan keys, which decrements so->numArrayKeys.  It's possible that
 * this will leave this new btrescan without any arrays at all.  This isn't
 * necessary for correctness; it's just an optimization.  Non-array equality
 * scan keys are slightly faster than equivalent array scan keys at runtime.
 */
static void
_bt_preprocess_array_keys_final(IndexScanDesc scan, int *keyDataMap)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	Relation	rel = scan->indexRelation;
	int			arrayidx = 0;
	int			last_equal_output_ikey PG_USED_FOR_ASSERTS_ONLY = -1;

	Assert(so->qual_ok);

	/*
	 * Nothing for us to do when _bt_preprocess_array_keys only had to deal
	 * with array inequalities
	 */
	if (so->numArrayKeys == 0)
		return;

	for (int output_ikey = 0; output_ikey < so->numberOfKeys; output_ikey++)
	{
		ScanKey		outkey = so->keyData + output_ikey;
		int			input_ikey;
		bool		found PG_USED_FOR_ASSERTS_ONLY = false;

		Assert(outkey->sk_strategy != InvalidStrategy);

		if (outkey->sk_strategy != BTEqualStrategyNumber)
			continue;

		input_ikey = keyDataMap[output_ikey];

		Assert(last_equal_output_ikey < output_ikey);
		Assert(last_equal_output_ikey < input_ikey);
		last_equal_output_ikey = output_ikey;

		/*
		 * We're lazy about looking up ORDER procs for non-array keys, since
		 * not all input keys become output keys.  Take care of it now.
		 */
		if (!(outkey->sk_flags & SK_SEARCHARRAY))
		{
			Oid			elemtype;

			/* No need for an ORDER proc given an IS NULL scan key */
			if (outkey->sk_flags & SK_SEARCHNULL)
				continue;

			/*
			 * A non-required scan key doesn't need an ORDER proc, either
			 * (unless it's associated with an array, which this one isn't)
			 */
			if (!(outkey->sk_flags & SK_BT_REQFWD))
				continue;

			elemtype = outkey->sk_subtype;
			if (elemtype == InvalidOid)
				elemtype = rel->rd_opcintype[outkey->sk_attno - 1];

			_bt_setup_array_cmp(scan, outkey, elemtype,
								&so->orderProcs[output_ikey], NULL);
			continue;
		}

		/*
		 * Reorder existing array scan key so->orderProcs[] entries.
		 *
		 * Doing this in-place is safe because preprocessing is required to
		 * output all equality strategy scan keys in original input order
		 * (among each group of entries against the same index attribute).
		 * This is also the order that the arrays themselves appear in.
		 */
		so->orderProcs[output_ikey] = so->orderProcs[input_ikey];

		/* Fix-up array->scan_key references for arrays */
		for (; arrayidx < so->numArrayKeys; arrayidx++)
		{
			BTArrayKeyInfo *array = &so->arrayKeys[arrayidx];

			/*
			 * All skip arrays must be marked required, and final column can
			 * never have a skip array
			 */
			Assert(array->num_elems > 0 || array->num_elems == -1);
			Assert(array->num_elems != -1 || outkey->sk_flags & SK_BT_REQFWD);
			Assert(array->num_elems != -1 ||
				   outkey->sk_attno < IndexRelationGetNumberOfKeyAttributes(rel));

			if (array->scan_key == input_ikey)
			{
				/* found it */
				array->scan_key = output_ikey;
				found = true;

				/*
				 * Transform array scan keys that have exactly 1 element
				 * remaining (following all prior preprocessing) into
				 * equivalent non-array scan keys.
				 */
				if (array->num_elems == 1)
				{
					outkey->sk_flags &= ~SK_SEARCHARRAY;
					outkey->sk_argument = array->elem_values[0];
					so->numArrayKeys--;

					/* If we're out of array keys, we can quit right away */
					if (so->numArrayKeys == 0)
						return;

					/* Shift other arrays forward */
					memmove(array, array + 1,
							sizeof(BTArrayKeyInfo) *
							(so->numArrayKeys - arrayidx));

					/*
					 * Don't increment arrayidx (there was an entry that was
					 * just shifted forward to the offset at arrayidx, which
					 * will still need to be matched)
					 */
				}
				else
				{
					/*
					 * Any skip array low_compare and high_compare scan keys
					 * are now final.  Transform the array's > low_compare key
					 * into a >= key (and < high_compare keys into a <= key).
					 */
					if (array->num_elems == -1 && array->sksup &&
						!array->null_elem)
						_bt_skiparray_strat_adjust(scan, outkey, array);

					/* Match found, so done with this array */
					arrayidx++;
				}

				break;
			}
		}

		Assert(found);
	}

	/*
	 * Parallel index scans require space in shared memory to store the
	 * current array elements (for arrays kept by preprocessing) to schedule
	 * the next primitive index scan.  The underlying structure is protected
	 * using an LWLock, so defensively limit its size.  In practice this can
	 * only affect parallel scans that use an incomplete opfamily.
	 */
	if (scan->parallel_scan && so->numArrayKeys > INDEX_MAX_KEYS)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg_internal("number of array scan keys left by preprocessing (%d) exceeds the maximum allowed by parallel btree index scans (%d)",
								 so->numArrayKeys, INDEX_MAX_KEYS)));
}

/*
 *	_bt_num_array_keys() -- determine # of BTArrayKeyInfo entries
 *
 * _bt_preprocess_array_keys helper function.  Returns the estimated size of
 * the scan's BTArrayKeyInfo array, which is guaranteed to be large enough to
 * fit every so->arrayKeys[] entry.
 *
 * Also sets *numSkipArrayKeys_out to the number of skip arrays caller must
 * add to the scan keys it'll output.  Caller must add this many skip arrays:
 * one array for each of the most significant attributes that lack a = input
 * key (IS NULL keys count as = input keys here).  The specific attributes
 * that need skip arrays are indicated by initializing skip_eq_ops_out[] arg
 * 0-based attribute offset to a valid = op strategy Oid.  We'll only ever set
 * skip_eq_ops_out[] entries to InvalidOid for attributes that already have an
 * equality key in scan->keyData[] input keys -- and only when there's some
 * later "attribute gap" for us to "fill-in" with a skip array.
 *
 * We're optimistic about skipping working out: we always add exactly the skip
 * arrays needed to maximize the number of input scan keys that can ultimately
 * be marked as required to continue the scan (but no more).  Given a
 * multi-column index on (a, b, c, d), we add skip arrays as follows:
 *
 * Input keys						Output keys (after all preprocessing)
 * ----------						-------------------------------------
 * a = 1							a = 1 (no skip arrays)
 * b = 42							skip a AND b = 42
 * a = 1 AND b = 42					a = 1 AND b = 42 (no skip arrays)
 * a >= 1 AND b = 42				range skip a AND b = 42
 * a = 1 AND b > 42					a = 1 AND b > 42 (no skip arrays)
 * a >= 1 AND a <= 3 AND b = 42		range skip a AND b = 42
 * a = 1 AND c <= 27				a = 1 AND skip b AND c <= 27
 * a = 1 AND d >= 1					a = 1 AND skip b AND skip c AND d >= 1
 * a = 1 AND b >= 42 AND d > 1		a = 1 AND range skip b AND skip c AND d > 1
 */
static int
_bt_num_array_keys(IndexScanDesc scan, Oid *skip_eq_ops_out,
				   int *numSkipArrayKeys_out)
{
	Relation	rel = scan->indexRelation;
	AttrNumber	attno_skip = 1,
				attno_inkey = 1;
	bool		attno_has_equal = false,
				attno_has_rowcompare = false;
	int			numSAOPArrayKeys,
				numSkipArrayKeys,
				prev_numSkipArrayKeys;

	Assert(scan->numberOfKeys);

	/* Initial pass over input scan keys counts the number of SAOP arrays */
	numSAOPArrayKeys = 0;
	*numSkipArrayKeys_out = prev_numSkipArrayKeys = numSkipArrayKeys = 0;
	for (int i = 0; i < scan->numberOfKeys; i++)
	{
		ScanKey		inkey = scan->keyData + i;

		if (inkey->sk_flags & SK_SEARCHARRAY)
			numSAOPArrayKeys++;
	}

#ifdef DEBUG_DISABLE_SKIP_SCAN
	/* don't attempt to add skip arrays */
	return numSAOPArrayKeys;
#endif

	for (int i = 0;; i++)
	{
		ScanKey		inkey = scan->keyData + i;

		/*
		 * Backfill skip arrays for any wholly omitted attributes prior to
		 * attno_inkey
		 */
		while (attno_skip < attno_inkey)
		{
			Oid			opfamily = rel->rd_opfamily[attno_skip - 1];
			Oid			opcintype = rel->rd_opcintype[attno_skip - 1];

			/* Look up input opclass's equality operator (might fail) */
			skip_eq_ops_out[attno_skip - 1] =
				get_opfamily_member(opfamily, opcintype, opcintype,
									BTEqualStrategyNumber);
			if (!OidIsValid(skip_eq_ops_out[attno_skip - 1]))
			{
				/*
				 * Cannot generate a skip array for this or later attributes
				 * (input opclass lacks an equality strategy operator)
				 */
				*numSkipArrayKeys_out = prev_numSkipArrayKeys;
				return numSAOPArrayKeys + prev_numSkipArrayKeys;
			}

			/* plan on adding a backfill skip array for this attribute */
			numSkipArrayKeys++;
			attno_skip++;
		}

		prev_numSkipArrayKeys = numSkipArrayKeys;

		/*
		 * Stop once past the final input scan key.  We deliberately never add
		 * a skip array for the last input scan key's attribute -- even when
		 * there are only inequality keys on that attribute.
		 */
		if (i == scan->numberOfKeys)
			break;

		/*
		 * Later preprocessing steps cannot merge a RowCompare into a skip
		 * array, so stop adding skip arrays once we see one.  (Note that we
		 * can backfill skip arrays before a RowCompare, which will allow keys
		 * up to and including the RowCompare to be marked required.)
		 *
		 * Skip arrays work by maintaining a current array element value,
		 * which anchors lower-order keys via an implied equality constraint.
		 * This is incompatible with the current nbtree row comparison design,
		 * which compares all columns together, as an indivisible group.
		 * Alternative designs that can be used alongside skip arrays are
		 * possible, but it's not clear that they're really worth pursuing.
		 *
		 * A RowCompare qual "(a, b, c) > (10, 'foo', 42)" is equivalent to
		 * "(a=10 AND b='foo' AND c>42) OR (a=10 AND b>'foo') OR (a>10)".
		 * Decomposing this RowCompare into these 3 disjuncts allows each
		 * disjunct to be executed as a separate "single value" index scan.
		 * That'll give all 3 scans the ability to add skip arrays in the
		 * usual way (when there are any scalar keys after the RowCompare).
		 * Under this scheme, a qual "(a, b, c) > (10, 'foo', 42) AND d = 99"
		 * performs 3 separate scans, each of which can mark keys up to and
		 * including its "d = 99" key as required to continue the scan.
		 */
		if (attno_has_rowcompare)
			break;

		/*
		 * Now consider next attno_inkey (or keep going if this is an
		 * additional scan key against the same attribute)
		 */
		if (attno_inkey < inkey->sk_attno)
		{
			/*
			 * Now add skip array for previous scan key's attribute, though
			 * only if the attribute has no equality strategy scan keys
			 */
			if (attno_has_equal)
			{
				/* Attributes with an = key must have InvalidOid eq_op set */
				skip_eq_ops_out[attno_skip - 1] = InvalidOid;
			}
			else
			{
				Oid			opfamily = rel->rd_opfamily[attno_skip - 1];
				Oid			opcintype = rel->rd_opcintype[attno_skip - 1];

				/* Look up input opclass's equality operator (might fail) */
				skip_eq_ops_out[attno_skip - 1] =
					get_opfamily_member(opfamily, opcintype, opcintype,
										BTEqualStrategyNumber);

				if (!OidIsValid(skip_eq_ops_out[attno_skip - 1]))
				{
					/*
					 * Input opclass lacks an equality strategy operator, so
					 * don't generate a skip array that definitely won't work
					 */
					break;
				}

				/* plan on adding a backfill skip array for this attribute */
				numSkipArrayKeys++;
			}

			/* Set things up for this new attribute */
			attno_skip++;
			attno_inkey = inkey->sk_attno;
			attno_has_equal = false;
		}

		/*
		 * Track if this attribute's scan keys include any equality strategy
		 * scan keys (IS NULL keys count as equality keys here).  Also track
		 * if it has any RowCompare keys.
		 */
		if (inkey->sk_strategy == BTEqualStrategyNumber ||
			(inkey->sk_flags & SK_SEARCHNULL))
			attno_has_equal = true;
		if (inkey->sk_flags & SK_ROW_HEADER)
			attno_has_rowcompare = true;
	}

	*numSkipArrayKeys_out = numSkipArrayKeys;
	return numSAOPArrayKeys + numSkipArrayKeys;
}

/*
 * _bt_find_extreme_element() -- get least or greatest array element
 *
 * scan and skey identify the index column, whose opfamily determines the
 * comparison semantics.  strat should be BTLessStrategyNumber to get the
 * least element, or BTGreaterStrategyNumber to get the greatest.
 */
static Datum
_bt_find_extreme_element(IndexScanDesc scan, ScanKey skey, Oid elemtype,
						 StrategyNumber strat,
						 Datum *elems, int nelems)
{
	Relation	rel = scan->indexRelation;
	Oid			cmp_op;
	RegProcedure cmp_proc;
	FmgrInfo	flinfo;
	Datum		result;
	int			i;

	/*
	 * Look up the appropriate comparison operator in the opfamily.
	 *
	 * Note: it's possible that this would fail, if the opfamily is
	 * incomplete, but it seems quite unlikely that an opfamily would omit
	 * non-cross-type comparison operators for any datatype that it supports
	 * at all.
	 */
	Assert(skey->sk_strategy != BTEqualStrategyNumber);
	Assert(OidIsValid(elemtype));
	cmp_op = get_opfamily_member(rel->rd_opfamily[skey->sk_attno - 1],
								 elemtype,
								 elemtype,
								 strat);
	if (!OidIsValid(cmp_op))
		elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
			 strat, elemtype, elemtype,
			 rel->rd_opfamily[skey->sk_attno - 1]);
	cmp_proc = get_opcode(cmp_op);
	if (!RegProcedureIsValid(cmp_proc))
		elog(ERROR, "missing oprcode for operator %u", cmp_op);

	fmgr_info(cmp_proc, &flinfo);

	Assert(nelems > 0);
	result = elems[0];
	for (i = 1; i < nelems; i++)
	{
		if (DatumGetBool(FunctionCall2Coll(&flinfo,
										   skey->sk_collation,
										   elems[i],
										   result)))
			result = elems[i];
	}

	return result;
}

/*
 * _bt_setup_array_cmp() -- Set up array comparison functions
 *
 * Sets ORDER proc in caller's orderproc argument, which is used during binary
 * searches of arrays during the index scan.  Also sets a same-type ORDER proc
 * in caller's *sortprocp argument, which is used when sorting the array.
 *
 * Preprocessing calls here with all equality strategy scan keys (when scan
 * uses equality array keys), including those not associated with any array.
 * See _bt_advance_array_keys for an explanation of why it'll need to treat
 * simple scalar equality scan keys as degenerate single element arrays.
 *
 * Caller should pass an orderproc pointing to space that'll store the ORDER
 * proc for the scan, and a *sortprocp pointing to its own separate space.
 * When calling here for a non-array scan key, sortprocp arg should be NULL.
 *
 * In the common case where we don't need to deal with cross-type operators,
 * only one ORDER proc is actually required by caller.  We'll set *sortprocp
 * to point to the same memory that caller's orderproc continues to point to.
 * Otherwise, *sortprocp will continue to point to caller's own space.  Either
 * way, *sortprocp will point to a same-type ORDER proc (since that's the only
 * safe way to sort/deduplicate the array associated with caller's scan key).
 */
static void
_bt_setup_array_cmp(IndexScanDesc scan, ScanKey skey, Oid elemtype,
					FmgrInfo *orderproc, FmgrInfo **sortprocp)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	Relation	rel = scan->indexRelation;
	RegProcedure cmp_proc;
	Oid			opcintype = rel->rd_opcintype[skey->sk_attno - 1];

	Assert(skey->sk_strategy == BTEqualStrategyNumber);
	Assert(OidIsValid(elemtype));

	/*
	 * If scankey operator is not a cross-type comparison, we can use the
	 * cached comparison function; otherwise gotta look it up in the catalogs
	 */
	if (elemtype == opcintype)
	{
		/* Set same-type ORDER procs for caller */
		*orderproc = *index_getprocinfo(rel, skey->sk_attno, BTORDER_PROC);
		if (sortprocp)
			*sortprocp = orderproc;

		return;
	}

	/*
	 * Look up the appropriate cross-type comparison function in the opfamily.
	 *
	 * Use the opclass input type as the left hand arg type, and the array
	 * element type as the right hand arg type (since binary searches use an
	 * index tuple's attribute value to search for a matching array element).
	 *
	 * Note: it's possible that this would fail, if the opfamily is
	 * incomplete, but only in cases where it's quite likely that _bt_first
	 * would fail in just the same way (had we not failed before it could).
	 */
	cmp_proc = get_opfamily_proc(rel->rd_opfamily[skey->sk_attno - 1],
								 opcintype, elemtype, BTORDER_PROC);
	if (!RegProcedureIsValid(cmp_proc))
		elog(ERROR, "missing support function %d(%u,%u) for attribute %d of index \"%s\"",
			 BTORDER_PROC, opcintype, elemtype, skey->sk_attno,
			 RelationGetRelationName(rel));

	/* Set cross-type ORDER proc for caller */
	fmgr_info_cxt(cmp_proc, orderproc, so->arrayContext);

	/* Done if caller doesn't actually have an array they'll need to sort */
	if (!sortprocp)
		return;

	/*
	 * Look up the appropriate same-type comparison function in the opfamily.
	 *
	 * Note: it's possible that this would fail, if the opfamily is
	 * incomplete, but it seems quite unlikely that an opfamily would omit
	 * non-cross-type comparison procs for any datatype that it supports at
	 * all.
	 */
	cmp_proc = get_opfamily_proc(rel->rd_opfamily[skey->sk_attno - 1],
								 elemtype, elemtype, BTORDER_PROC);
	if (!RegProcedureIsValid(cmp_proc))
		elog(ERROR, "missing support function %d(%u,%u) for attribute %d of index \"%s\"",
			 BTORDER_PROC, elemtype, elemtype,
			 skey->sk_attno, RelationGetRelationName(rel));

	/* Set same-type ORDER proc for caller */
	fmgr_info_cxt(cmp_proc, *sortprocp, so->arrayContext);
}

/*
 * _bt_sort_array_elements() -- sort and de-dup array elements
 *
 * The array elements are sorted in-place, and the new number of elements
 * after duplicate removal is returned.
 *
 * skey identifies the index column whose opfamily determines the comparison
 * semantics, and sortproc is a corresponding ORDER proc.  If reverse is true,
 * we sort in descending order.
 */
static int
_bt_sort_array_elements(ScanKey skey, FmgrInfo *sortproc, bool reverse,
						Datum *elems, int nelems)
{
	BTSortArrayContext cxt;

	if (nelems <= 1)
		return nelems;			/* no work to do */

	/* Sort the array elements */
	cxt.sortproc = sortproc;
	cxt.collation = skey->sk_collation;
	cxt.reverse = reverse;
	qsort_arg(elems, nelems, sizeof(Datum),
			  _bt_compare_array_elements, &cxt);

	/* Now scan the sorted elements and remove duplicates */
	return qunique_arg(elems, nelems, sizeof(Datum),
					   _bt_compare_array_elements, &cxt);
}

/*
 * _bt_merge_arrays() -- merge next array's elements into an original array
 *
 * Called when preprocessing encounters a pair of array equality scan keys,
 * both against the same index attribute (during initial array preprocessing).
 * Merging reorganizes caller's original array (the left hand arg) in-place,
 * without ever copying elements from one array into the other. (Mixing the
 * elements together like this would be wrong, since they don't necessarily
 * use the same underlying element type, despite all the other similarities.)
 *
 * Both arrays must have already been sorted and deduplicated by calling
 * _bt_sort_array_elements.  sortproc is the same-type ORDER proc that was
 * just used to sort and deduplicate caller's "next" array.  We'll usually be
 * able to reuse that order PROC to merge the arrays together now.  If not,
 * then we'll perform a separate ORDER proc lookup.
 *
 * If the opfamily doesn't supply a complete set of cross-type ORDER procs we
 * may not be able to determine which elements are contradictory.  If we have
 * the required ORDER proc then we return true (and validly set *nelems_orig),
 * guaranteeing that at least the next array can be considered redundant.  We
 * return false if the required comparisons cannot be made (caller must keep
 * both arrays when this happens).
 */
static bool
_bt_merge_arrays(IndexScanDesc scan, ScanKey skey, FmgrInfo *sortproc,
				 bool reverse, Oid origelemtype, Oid nextelemtype,
				 Datum *elems_orig, int *nelems_orig,
				 Datum *elems_next, int nelems_next)
{
	Relation	rel = scan->indexRelation;
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	BTSortArrayContext cxt;
	int			nelems_orig_start = *nelems_orig,
				nelems_orig_merged = 0;
	FmgrInfo   *mergeproc = sortproc;
	FmgrInfo	crosstypeproc;

	Assert(skey->sk_strategy == BTEqualStrategyNumber);
	Assert(OidIsValid(origelemtype) && OidIsValid(nextelemtype));

	if (origelemtype != nextelemtype)
	{
		RegProcedure cmp_proc;

		/*
		 * Cross-array-element-type merging is required, so can't just reuse
		 * sortproc when merging
		 */
		cmp_proc = get_opfamily_proc(rel->rd_opfamily[skey->sk_attno - 1],
									 origelemtype, nextelemtype, BTORDER_PROC);
		if (!RegProcedureIsValid(cmp_proc))
		{
			/* Can't make the required comparisons */
			return false;
		}

		/* We have all we need to determine redundancy/contradictoriness */
		mergeproc = &crosstypeproc;
		fmgr_info_cxt(cmp_proc, mergeproc, so->arrayContext);
	}

	cxt.sortproc = mergeproc;
	cxt.collation = skey->sk_collation;
	cxt.reverse = reverse;

	for (int i = 0, j = 0; i < nelems_orig_start && j < nelems_next;)
	{
		Datum	   *oelem = elems_orig + i,
				   *nelem = elems_next + j;
		int			res = _bt_compare_array_elements(oelem, nelem, &cxt);

		if (res == 0)
		{
			elems_orig[nelems_orig_merged++] = *oelem;
			i++;
			j++;
		}
		else if (res < 0)
			i++;
		else					/* res > 0 */
			j++;
	}

	*nelems_orig = nelems_orig_merged;

	return true;
}

/*
 * qsort_arg comparator for sorting array elements
 */
static int
_bt_compare_array_elements(const void *a, const void *b, void *arg)
{
	Datum		da = *((const Datum *) a);
	Datum		db = *((const Datum *) b);
	BTSortArrayContext *cxt = (BTSortArrayContext *) arg;
	int32		compare;

	compare = DatumGetInt32(FunctionCall2Coll(cxt->sortproc,
											  cxt->collation,
											  da, db));
	if (cxt->reverse)
		INVERT_COMPARE_RESULT(compare);
	return compare;
}
