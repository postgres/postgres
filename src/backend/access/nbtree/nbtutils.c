/*-------------------------------------------------------------------------
 *
 * nbtutils.c
 *	  Utility code for Postgres btree implementation.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/nbtree/nbtutils.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <time.h>

#include "access/nbtree.h"
#include "access/reloptions.h"
#include "commands/progress.h"
#include "miscadmin.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"

#define LOOK_AHEAD_REQUIRED_RECHECKS 	3
#define LOOK_AHEAD_DEFAULT_DISTANCE 	5
#define NSKIPADVANCES_THRESHOLD			3

static inline int32 _bt_compare_array_skey(FmgrInfo *orderproc,
										   Datum tupdatum, bool tupnull,
										   Datum arrdatum, ScanKey cur);
static void _bt_binsrch_skiparray_skey(bool cur_elem_trig, ScanDirection dir,
									   Datum tupdatum, bool tupnull,
									   BTArrayKeyInfo *array, ScanKey cur,
									   int32 *set_elem_result);
static void _bt_skiparray_set_element(Relation rel, ScanKey skey, BTArrayKeyInfo *array,
									  int32 set_elem_result, Datum tupdatum, bool tupnull);
static void _bt_skiparray_set_isnull(Relation rel, ScanKey skey, BTArrayKeyInfo *array);
static void _bt_array_set_low_or_high(Relation rel, ScanKey skey,
									  BTArrayKeyInfo *array, bool low_not_high);
static bool _bt_array_decrement(Relation rel, ScanKey skey, BTArrayKeyInfo *array);
static bool _bt_array_increment(Relation rel, ScanKey skey, BTArrayKeyInfo *array);
static bool _bt_advance_array_keys_increment(IndexScanDesc scan, ScanDirection dir,
											 bool *skip_array_set);
static void _bt_rewind_nonrequired_arrays(IndexScanDesc scan, ScanDirection dir);
static bool _bt_tuple_before_array_skeys(IndexScanDesc scan, ScanDirection dir,
										 IndexTuple tuple, TupleDesc tupdesc, int tupnatts,
										 bool readpagetup, int sktrig, bool *scanBehind);
static bool _bt_advance_array_keys(IndexScanDesc scan, BTReadPageState *pstate,
								   IndexTuple tuple, int tupnatts, TupleDesc tupdesc,
								   int sktrig, bool sktrig_required);
#ifdef USE_ASSERT_CHECKING
static bool _bt_verify_arrays_bt_first(IndexScanDesc scan, ScanDirection dir);
static bool _bt_verify_keys_with_arraykeys(IndexScanDesc scan);
#endif
static bool _bt_oppodir_checkkeys(IndexScanDesc scan, ScanDirection dir,
								  IndexTuple finaltup);
static bool _bt_check_compare(IndexScanDesc scan, ScanDirection dir,
							  IndexTuple tuple, int tupnatts, TupleDesc tupdesc,
							  bool advancenonrequired, bool forcenonrequired,
							  bool *continuescan, int *ikey);
static bool _bt_check_rowcompare(ScanKey skey,
								 IndexTuple tuple, int tupnatts, TupleDesc tupdesc,
								 ScanDirection dir, bool *continuescan);
static void _bt_checkkeys_look_ahead(IndexScanDesc scan, BTReadPageState *pstate,
									 int tupnatts, TupleDesc tupdesc);
static int	_bt_keep_natts(Relation rel, IndexTuple lastleft,
						   IndexTuple firstright, BTScanInsert itup_key);


/*
 * _bt_mkscankey
 *		Build an insertion scan key that contains comparison data from itup
 *		as well as comparator routines appropriate to the key datatypes.
 *
 *		The result is intended for use with _bt_compare() and _bt_truncate().
 *		Callers that don't need to fill out the insertion scankey arguments
 *		(e.g. they use an ad-hoc comparison routine, or only need a scankey
 *		for _bt_truncate()) can pass a NULL index tuple.  The scankey will
 *		be initialized as if an "all truncated" pivot tuple was passed
 *		instead.
 *
 *		Note that we may occasionally have to share lock the metapage to
 *		determine whether or not the keys in the index are expected to be
 *		unique (i.e. if this is a "heapkeyspace" index).  We assume a
 *		heapkeyspace index when caller passes a NULL tuple, allowing index
 *		build callers to avoid accessing the non-existent metapage.  We
 *		also assume that the index is _not_ allequalimage when a NULL tuple
 *		is passed; CREATE INDEX callers call _bt_allequalimage() to set the
 *		field themselves.
 */
BTScanInsert
_bt_mkscankey(Relation rel, IndexTuple itup)
{
	BTScanInsert key;
	ScanKey		skey;
	TupleDesc	itupdesc;
	int			indnkeyatts;
	int16	   *indoption;
	int			tupnatts;
	int			i;

	itupdesc = RelationGetDescr(rel);
	indnkeyatts = IndexRelationGetNumberOfKeyAttributes(rel);
	indoption = rel->rd_indoption;
	tupnatts = itup ? BTreeTupleGetNAtts(itup, rel) : 0;

	Assert(tupnatts <= IndexRelationGetNumberOfAttributes(rel));

	/*
	 * We'll execute search using scan key constructed on key columns.
	 * Truncated attributes and non-key attributes are omitted from the final
	 * scan key.
	 */
	key = palloc(offsetof(BTScanInsertData, scankeys) +
				 sizeof(ScanKeyData) * indnkeyatts);
	if (itup)
		_bt_metaversion(rel, &key->heapkeyspace, &key->allequalimage);
	else
	{
		/* Utility statement callers can set these fields themselves */
		key->heapkeyspace = true;
		key->allequalimage = false;
	}
	key->anynullkeys = false;	/* initial assumption */
	key->nextkey = false;		/* usual case, required by btinsert */
	key->backward = false;		/* usual case, required by btinsert */
	key->keysz = Min(indnkeyatts, tupnatts);
	key->scantid = key->heapkeyspace && itup ?
		BTreeTupleGetHeapTID(itup) : NULL;
	skey = key->scankeys;
	for (i = 0; i < indnkeyatts; i++)
	{
		FmgrInfo   *procinfo;
		Datum		arg;
		bool		null;
		int			flags;

		/*
		 * We can use the cached (default) support procs since no cross-type
		 * comparison can be needed.
		 */
		procinfo = index_getprocinfo(rel, i + 1, BTORDER_PROC);

		/*
		 * Key arguments built from truncated attributes (or when caller
		 * provides no tuple) are defensively represented as NULL values. They
		 * should never be used.
		 */
		if (i < tupnatts)
			arg = index_getattr(itup, i + 1, itupdesc, &null);
		else
		{
			arg = (Datum) 0;
			null = true;
		}
		flags = (null ? SK_ISNULL : 0) | (indoption[i] << SK_BT_INDOPTION_SHIFT);
		ScanKeyEntryInitializeWithInfo(&skey[i],
									   flags,
									   (AttrNumber) (i + 1),
									   InvalidStrategy,
									   InvalidOid,
									   rel->rd_indcollation[i],
									   procinfo,
									   arg);
		/* Record if any key attribute is NULL (or truncated) */
		if (null)
			key->anynullkeys = true;
	}

	/*
	 * In NULLS NOT DISTINCT mode, we pretend that there are no null keys, so
	 * that full uniqueness check is done.
	 */
	if (rel->rd_index->indnullsnotdistinct)
		key->anynullkeys = false;

	return key;
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
 * _bt_compare_array_skey() -- apply array comparison function
 *
 * Compares caller's tuple attribute value to a scan key/array element.
 * Helper function used during binary searches of SK_SEARCHARRAY arrays.
 *
 *		This routine returns:
 *			<0 if tupdatum < arrdatum;
 *			 0 if tupdatum == arrdatum;
 *			>0 if tupdatum > arrdatum.
 *
 * This is essentially the same interface as _bt_compare: both functions
 * compare the value that they're searching for to a binary search pivot.
 * However, unlike _bt_compare, this function's "tuple argument" comes first,
 * while its "array/scankey argument" comes second.
*/
static inline int32
_bt_compare_array_skey(FmgrInfo *orderproc,
					   Datum tupdatum, bool tupnull,
					   Datum arrdatum, ScanKey cur)
{
	int32		result = 0;

	Assert(cur->sk_strategy == BTEqualStrategyNumber);
	Assert(!(cur->sk_flags & (SK_BT_MINVAL | SK_BT_MAXVAL)));

	if (tupnull)				/* NULL tupdatum */
	{
		if (cur->sk_flags & SK_ISNULL)
			result = 0;			/* NULL "=" NULL */
		else if (cur->sk_flags & SK_BT_NULLS_FIRST)
			result = -1;		/* NULL "<" NOT_NULL */
		else
			result = 1;			/* NULL ">" NOT_NULL */
	}
	else if (cur->sk_flags & SK_ISNULL) /* NOT_NULL tupdatum, NULL arrdatum */
	{
		if (cur->sk_flags & SK_BT_NULLS_FIRST)
			result = 1;			/* NOT_NULL ">" NULL */
		else
			result = -1;		/* NOT_NULL "<" NULL */
	}
	else
	{
		/*
		 * Like _bt_compare, we need to be careful of cross-type comparisons,
		 * so the left value has to be the value that came from an index tuple
		 */
		result = DatumGetInt32(FunctionCall2Coll(orderproc, cur->sk_collation,
												 tupdatum, arrdatum));

		/*
		 * We flip the sign by following the obvious rule: flip whenever the
		 * column is a DESC column.
		 *
		 * _bt_compare does it the wrong way around (flip when *ASC*) in order
		 * to compensate for passing its orderproc arguments backwards.  We
		 * don't need to play these games because we find it natural to pass
		 * tupdatum as the left value (and arrdatum as the right value).
		 */
		if (cur->sk_flags & SK_BT_DESC)
			INVERT_COMPARE_RESULT(result);
	}

	return result;
}

/*
 * _bt_binsrch_array_skey() -- Binary search for next matching array key
 *
 * Returns an index to the first array element >= caller's tupdatum argument.
 * This convention is more natural for forwards scan callers, but that can't
 * really matter to backwards scan callers.  Both callers require handling for
 * the case where the match we return is < tupdatum, and symmetric handling
 * for the case where our best match is > tupdatum.
 *
 * Also sets *set_elem_result to the result _bt_compare_array_skey returned
 * when we used it to compare the matching array element to tupdatum/tupnull.
 *
 * cur_elem_trig indicates if array advancement was triggered by this array's
 * scan key, and that the array is for a required scan key.  We can apply this
 * information to find the next matching array element in the current scan
 * direction using far fewer comparisons (fewer on average, compared to naive
 * binary search).  This scheme takes advantage of an important property of
 * required arrays: required arrays always advance in lockstep with the index
 * scan's progress through the index's key space.
 */
int
_bt_binsrch_array_skey(FmgrInfo *orderproc,
					   bool cur_elem_trig, ScanDirection dir,
					   Datum tupdatum, bool tupnull,
					   BTArrayKeyInfo *array, ScanKey cur,
					   int32 *set_elem_result)
{
	int			low_elem = 0,
				mid_elem = -1,
				high_elem = array->num_elems - 1,
				result = 0;
	Datum		arrdatum;

	Assert(cur->sk_flags & SK_SEARCHARRAY);
	Assert(!(cur->sk_flags & SK_BT_SKIP));
	Assert(!(cur->sk_flags & SK_ISNULL));	/* SAOP arrays never have NULLs */
	Assert(cur->sk_strategy == BTEqualStrategyNumber);

	if (cur_elem_trig)
	{
		Assert(!ScanDirectionIsNoMovement(dir));
		Assert(cur->sk_flags & SK_BT_REQFWD);

		/*
		 * When the scan key that triggered array advancement is a required
		 * array scan key, it is now certain that the current array element
		 * (plus all prior elements relative to the current scan direction)
		 * cannot possibly be at or ahead of the corresponding tuple value.
		 * (_bt_checkkeys must have called _bt_tuple_before_array_skeys, which
		 * makes sure this is true as a condition of advancing the arrays.)
		 *
		 * This makes it safe to exclude array elements up to and including
		 * the former-current array element from our search.
		 *
		 * Separately, when array advancement was triggered by a required scan
		 * key, the array element immediately after the former-current element
		 * is often either an exact tupdatum match, or a "close by" near-match
		 * (a near-match tupdatum is one whose key space falls _between_ the
		 * former-current and new-current array elements).  We'll detect both
		 * cases via an optimistic comparison of the new search lower bound
		 * (or new search upper bound in the case of backwards scans).
		 */
		if (ScanDirectionIsForward(dir))
		{
			low_elem = array->cur_elem + 1; /* old cur_elem exhausted */

			/* Compare prospective new cur_elem (also the new lower bound) */
			if (high_elem >= low_elem)
			{
				arrdatum = array->elem_values[low_elem];
				result = _bt_compare_array_skey(orderproc, tupdatum, tupnull,
												arrdatum, cur);

				if (result <= 0)
				{
					/* Optimistic comparison optimization worked out */
					*set_elem_result = result;
					return low_elem;
				}
				mid_elem = low_elem;
				low_elem++;		/* this cur_elem exhausted, too */
			}

			if (high_elem < low_elem)
			{
				/* Caller needs to perform "beyond end" array advancement */
				*set_elem_result = 1;
				return high_elem;
			}
		}
		else
		{
			high_elem = array->cur_elem - 1;	/* old cur_elem exhausted */

			/* Compare prospective new cur_elem (also the new upper bound) */
			if (high_elem >= low_elem)
			{
				arrdatum = array->elem_values[high_elem];
				result = _bt_compare_array_skey(orderproc, tupdatum, tupnull,
												arrdatum, cur);

				if (result >= 0)
				{
					/* Optimistic comparison optimization worked out */
					*set_elem_result = result;
					return high_elem;
				}
				mid_elem = high_elem;
				high_elem--;	/* this cur_elem exhausted, too */
			}

			if (high_elem < low_elem)
			{
				/* Caller needs to perform "beyond end" array advancement */
				*set_elem_result = -1;
				return low_elem;
			}
		}
	}

	while (high_elem > low_elem)
	{
		mid_elem = low_elem + ((high_elem - low_elem) / 2);
		arrdatum = array->elem_values[mid_elem];

		result = _bt_compare_array_skey(orderproc, tupdatum, tupnull,
										arrdatum, cur);

		if (result == 0)
		{
			/*
			 * It's safe to quit as soon as we see an equal array element.
			 * This often saves an extra comparison or two...
			 */
			low_elem = mid_elem;
			break;
		}

		if (result > 0)
			low_elem = mid_elem + 1;
		else
			high_elem = mid_elem;
	}

	/*
	 * ...but our caller also cares about how its searched-for tuple datum
	 * compares to the low_elem datum.  Must always set *set_elem_result with
	 * the result of that comparison specifically.
	 */
	if (low_elem != mid_elem)
		result = _bt_compare_array_skey(orderproc, tupdatum, tupnull,
										array->elem_values[low_elem], cur);

	*set_elem_result = result;

	return low_elem;
}

/*
 * _bt_binsrch_skiparray_skey() -- "Binary search" within a skip array
 *
 * Does not return an index into the array, since skip arrays don't really
 * contain elements (they generate their array elements procedurally instead).
 * Our interface matches that of _bt_binsrch_array_skey in every other way.
 *
 * Sets *set_elem_result just like _bt_binsrch_array_skey would with a true
 * array.  The value 0 indicates that tupdatum/tupnull is within the range of
 * the skip array.  We return -1 when tupdatum/tupnull is lower that any value
 * within the range of the array, and 1 when it is higher than every value.
 * Caller should pass *set_elem_result to _bt_skiparray_set_element to advance
 * the array.
 *
 * cur_elem_trig indicates if array advancement was triggered by this array's
 * scan key.  We use this to optimize-away comparisons that are known by our
 * caller to be unnecessary from context, just like _bt_binsrch_array_skey.
 */
static void
_bt_binsrch_skiparray_skey(bool cur_elem_trig, ScanDirection dir,
						   Datum tupdatum, bool tupnull,
						   BTArrayKeyInfo *array, ScanKey cur,
						   int32 *set_elem_result)
{
	Assert(cur->sk_flags & SK_BT_SKIP);
	Assert(cur->sk_flags & SK_SEARCHARRAY);
	Assert(cur->sk_flags & SK_BT_REQFWD);
	Assert(array->num_elems == -1);
	Assert(!ScanDirectionIsNoMovement(dir));

	if (array->null_elem)
	{
		Assert(!array->low_compare && !array->high_compare);

		*set_elem_result = 0;
		return;
	}

	if (tupnull)				/* NULL tupdatum */
	{
		if (cur->sk_flags & SK_BT_NULLS_FIRST)
			*set_elem_result = -1;	/* NULL "<" NOT_NULL */
		else
			*set_elem_result = 1;	/* NULL ">" NOT_NULL */
		return;
	}

	/*
	 * Array inequalities determine whether tupdatum is within the range of
	 * caller's skip array
	 */
	*set_elem_result = 0;
	if (ScanDirectionIsForward(dir))
	{
		/*
		 * Evaluate low_compare first (unless cur_elem_trig tells us that it
		 * cannot possibly fail to be satisfied), then evaluate high_compare
		 */
		if (!cur_elem_trig && array->low_compare &&
			!DatumGetBool(FunctionCall2Coll(&array->low_compare->sk_func,
											array->low_compare->sk_collation,
											tupdatum,
											array->low_compare->sk_argument)))
			*set_elem_result = -1;
		else if (array->high_compare &&
				 !DatumGetBool(FunctionCall2Coll(&array->high_compare->sk_func,
												 array->high_compare->sk_collation,
												 tupdatum,
												 array->high_compare->sk_argument)))
			*set_elem_result = 1;
	}
	else
	{
		/*
		 * Evaluate high_compare first (unless cur_elem_trig tells us that it
		 * cannot possibly fail to be satisfied), then evaluate low_compare
		 */
		if (!cur_elem_trig && array->high_compare &&
			!DatumGetBool(FunctionCall2Coll(&array->high_compare->sk_func,
											array->high_compare->sk_collation,
											tupdatum,
											array->high_compare->sk_argument)))
			*set_elem_result = 1;
		else if (array->low_compare &&
				 !DatumGetBool(FunctionCall2Coll(&array->low_compare->sk_func,
												 array->low_compare->sk_collation,
												 tupdatum,
												 array->low_compare->sk_argument)))
			*set_elem_result = -1;
	}

	/*
	 * Assert that any keys that were assumed to be satisfied already (due to
	 * caller passing cur_elem_trig=true) really are satisfied as expected
	 */
#ifdef USE_ASSERT_CHECKING
	if (cur_elem_trig)
	{
		if (ScanDirectionIsForward(dir) && array->low_compare)
			Assert(DatumGetBool(FunctionCall2Coll(&array->low_compare->sk_func,
												  array->low_compare->sk_collation,
												  tupdatum,
												  array->low_compare->sk_argument)));

		if (ScanDirectionIsBackward(dir) && array->high_compare)
			Assert(DatumGetBool(FunctionCall2Coll(&array->high_compare->sk_func,
												  array->high_compare->sk_collation,
												  tupdatum,
												  array->high_compare->sk_argument)));
	}
#endif
}

/*
 * _bt_skiparray_set_element() -- Set skip array scan key's sk_argument
 *
 * Caller passes set_elem_result returned by _bt_binsrch_skiparray_skey for
 * caller's tupdatum/tupnull.
 *
 * We copy tupdatum/tupnull into skey's sk_argument iff set_elem_result == 0.
 * Otherwise, we set skey to either the lowest or highest value that's within
 * the range of caller's skip array (whichever is the best available match to
 * tupdatum/tupnull that is still within the range of the skip array according
 * to _bt_binsrch_skiparray_skey/set_elem_result).
 */
static void
_bt_skiparray_set_element(Relation rel, ScanKey skey, BTArrayKeyInfo *array,
						  int32 set_elem_result, Datum tupdatum, bool tupnull)
{
	Assert(skey->sk_flags & SK_BT_SKIP);
	Assert(skey->sk_flags & SK_SEARCHARRAY);

	if (set_elem_result)
	{
		/* tupdatum/tupnull is out of the range of the skip array */
		Assert(!array->null_elem);

		_bt_array_set_low_or_high(rel, skey, array, set_elem_result < 0);
		return;
	}

	/* Advance skip array to tupdatum (or tupnull) value */
	if (unlikely(tupnull))
	{
		_bt_skiparray_set_isnull(rel, skey, array);
		return;
	}

	/* Free memory previously allocated for sk_argument if needed */
	if (!array->attbyval && skey->sk_argument)
		pfree(DatumGetPointer(skey->sk_argument));

	/* tupdatum becomes new sk_argument/new current element */
	skey->sk_flags &= ~(SK_SEARCHNULL | SK_ISNULL |
						SK_BT_MINVAL | SK_BT_MAXVAL |
						SK_BT_NEXT | SK_BT_PRIOR);
	skey->sk_argument = datumCopy(tupdatum, array->attbyval, array->attlen);
}

/*
 * _bt_skiparray_set_isnull() -- set skip array scan key to NULL
 */
static void
_bt_skiparray_set_isnull(Relation rel, ScanKey skey, BTArrayKeyInfo *array)
{
	Assert(skey->sk_flags & SK_BT_SKIP);
	Assert(skey->sk_flags & SK_SEARCHARRAY);
	Assert(array->null_elem && !array->low_compare && !array->high_compare);

	/* Free memory previously allocated for sk_argument if needed */
	if (!array->attbyval && skey->sk_argument)
		pfree(DatumGetPointer(skey->sk_argument));

	/* NULL becomes new sk_argument/new current element */
	skey->sk_argument = (Datum) 0;
	skey->sk_flags &= ~(SK_BT_MINVAL | SK_BT_MAXVAL |
						SK_BT_NEXT | SK_BT_PRIOR);
	skey->sk_flags |= (SK_SEARCHNULL | SK_ISNULL);
}

/*
 * _bt_start_array_keys() -- Initialize array keys at start of a scan
 *
 * Set up the cur_elem counters and fill in the first sk_argument value for
 * each array scankey.
 */
void
_bt_start_array_keys(IndexScanDesc scan, ScanDirection dir)
{
	Relation	rel = scan->indexRelation;
	BTScanOpaque so = (BTScanOpaque) scan->opaque;

	Assert(so->numArrayKeys);
	Assert(so->qual_ok);

	for (int i = 0; i < so->numArrayKeys; i++)
	{
		BTArrayKeyInfo *array = &so->arrayKeys[i];
		ScanKey		skey = &so->keyData[array->scan_key];

		Assert(skey->sk_flags & SK_SEARCHARRAY);

		_bt_array_set_low_or_high(rel, skey, array,
								  ScanDirectionIsForward(dir));
	}
	so->scanBehind = so->oppositeDirCheck = false;	/* reset */
}

/*
 * _bt_array_set_low_or_high() -- Set array scan key to lowest/highest element
 *
 * Caller also passes associated scan key, which will have its argument set to
 * the lowest/highest array value in passing.
 */
static void
_bt_array_set_low_or_high(Relation rel, ScanKey skey, BTArrayKeyInfo *array,
						  bool low_not_high)
{
	Assert(skey->sk_flags & SK_SEARCHARRAY);

	if (array->num_elems != -1)
	{
		/* set low or high element for SAOP array */
		int			set_elem = 0;

		Assert(!(skey->sk_flags & SK_BT_SKIP));

		if (!low_not_high)
			set_elem = array->num_elems - 1;

		/*
		 * Just copy over array datum (only skip arrays require freeing and
		 * allocating memory for sk_argument)
		 */
		array->cur_elem = set_elem;
		skey->sk_argument = array->elem_values[set_elem];

		return;
	}

	/* set low or high element for skip array */
	Assert(skey->sk_flags & SK_BT_SKIP);
	Assert(array->num_elems == -1);

	/* Free memory previously allocated for sk_argument if needed */
	if (!array->attbyval && skey->sk_argument)
		pfree(DatumGetPointer(skey->sk_argument));

	/* Reset flags */
	skey->sk_argument = (Datum) 0;
	skey->sk_flags &= ~(SK_SEARCHNULL | SK_ISNULL |
						SK_BT_MINVAL | SK_BT_MAXVAL |
						SK_BT_NEXT | SK_BT_PRIOR);

	if (array->null_elem &&
		(low_not_high == ((skey->sk_flags & SK_BT_NULLS_FIRST) != 0)))
	{
		/* Requested element (either lowest or highest) has the value NULL */
		skey->sk_flags |= (SK_SEARCHNULL | SK_ISNULL);
	}
	else if (low_not_high)
	{
		/* Setting array to lowest element (according to low_compare) */
		skey->sk_flags |= SK_BT_MINVAL;
	}
	else
	{
		/* Setting array to highest element (according to high_compare) */
		skey->sk_flags |= SK_BT_MAXVAL;
	}
}

/*
 * _bt_array_decrement() -- decrement array scan key's sk_argument
 *
 * Return value indicates whether caller's array was successfully decremented.
 * Cannot decrement an array whose current element is already the first one.
 */
static bool
_bt_array_decrement(Relation rel, ScanKey skey, BTArrayKeyInfo *array)
{
	bool		uflow = false;
	Datum		dec_sk_argument;

	Assert(skey->sk_flags & SK_SEARCHARRAY);
	Assert(!(skey->sk_flags & (SK_BT_MAXVAL | SK_BT_NEXT | SK_BT_PRIOR)));

	/* SAOP array? */
	if (array->num_elems != -1)
	{
		Assert(!(skey->sk_flags & (SK_BT_SKIP | SK_BT_MINVAL | SK_BT_MAXVAL)));
		if (array->cur_elem > 0)
		{
			/*
			 * Just decrement current element, and assign its datum to skey
			 * (only skip arrays need us to free existing sk_argument memory)
			 */
			array->cur_elem--;
			skey->sk_argument = array->elem_values[array->cur_elem];

			/* Successfully decremented array */
			return true;
		}

		/* Cannot decrement to before first array element */
		return false;
	}

	/* Nope, this is a skip array */
	Assert(skey->sk_flags & SK_BT_SKIP);

	/*
	 * The sentinel value that represents the minimum value within the range
	 * of a skip array (often just -inf) is never decrementable
	 */
	if (skey->sk_flags & SK_BT_MINVAL)
		return false;

	/*
	 * When the current array element is NULL, and the lowest sorting value in
	 * the index is also NULL, we cannot decrement before first array element
	 */
	if ((skey->sk_flags & SK_ISNULL) && (skey->sk_flags & SK_BT_NULLS_FIRST))
		return false;

	/*
	 * Opclasses without skip support "decrement" the scan key's current
	 * element by setting the PRIOR flag.  The true prior value is determined
	 * by repositioning to the last index tuple < existing sk_argument/current
	 * array element.  Note that this works in the usual way when the scan key
	 * is already marked ISNULL (i.e. when the current element is NULL).
	 */
	if (!array->sksup)
	{
		/* Successfully "decremented" array */
		skey->sk_flags |= SK_BT_PRIOR;
		return true;
	}

	/*
	 * Opclasses with skip support directly decrement sk_argument
	 */
	if (skey->sk_flags & SK_ISNULL)
	{
		Assert(!(skey->sk_flags & SK_BT_NULLS_FIRST));

		/*
		 * Existing sk_argument/array element is NULL (for an IS NULL qual).
		 *
		 * "Decrement" from NULL to the high_elem value provided by opclass
		 * skip support routine.
		 */
		skey->sk_flags &= ~(SK_SEARCHNULL | SK_ISNULL);
		skey->sk_argument = datumCopy(array->sksup->high_elem,
									  array->attbyval, array->attlen);
		return true;
	}

	/*
	 * Ask opclass support routine to provide decremented copy of existing
	 * non-NULL sk_argument
	 */
	dec_sk_argument = array->sksup->decrement(rel, skey->sk_argument, &uflow);
	if (unlikely(uflow))
	{
		/* dec_sk_argument has undefined value (so no pfree) */
		if (array->null_elem && (skey->sk_flags & SK_BT_NULLS_FIRST))
		{
			_bt_skiparray_set_isnull(rel, skey, array);

			/* Successfully "decremented" array to NULL */
			return true;
		}

		/* Cannot decrement to before first array element */
		return false;
	}

	/*
	 * Successfully decremented sk_argument to a non-NULL value.  Make sure
	 * that the decremented value is still within the range of the array.
	 */
	if (array->low_compare &&
		!DatumGetBool(FunctionCall2Coll(&array->low_compare->sk_func,
										array->low_compare->sk_collation,
										dec_sk_argument,
										array->low_compare->sk_argument)))
	{
		/* Keep existing sk_argument after all */
		if (!array->attbyval)
			pfree(DatumGetPointer(dec_sk_argument));

		/* Cannot decrement to before first array element */
		return false;
	}

	/* Accept value returned by opclass decrement callback */
	if (!array->attbyval && skey->sk_argument)
		pfree(DatumGetPointer(skey->sk_argument));
	skey->sk_argument = dec_sk_argument;

	/* Successfully decremented array */
	return true;
}

/*
 * _bt_array_increment() -- increment array scan key's sk_argument
 *
 * Return value indicates whether caller's array was successfully incremented.
 * Cannot increment an array whose current element is already the final one.
 */
static bool
_bt_array_increment(Relation rel, ScanKey skey, BTArrayKeyInfo *array)
{
	bool		oflow = false;
	Datum		inc_sk_argument;

	Assert(skey->sk_flags & SK_SEARCHARRAY);
	Assert(!(skey->sk_flags & (SK_BT_MINVAL | SK_BT_NEXT | SK_BT_PRIOR)));

	/* SAOP array? */
	if (array->num_elems != -1)
	{
		Assert(!(skey->sk_flags & (SK_BT_SKIP | SK_BT_MINVAL | SK_BT_MAXVAL)));
		if (array->cur_elem < array->num_elems - 1)
		{
			/*
			 * Just increment current element, and assign its datum to skey
			 * (only skip arrays need us to free existing sk_argument memory)
			 */
			array->cur_elem++;
			skey->sk_argument = array->elem_values[array->cur_elem];

			/* Successfully incremented array */
			return true;
		}

		/* Cannot increment past final array element */
		return false;
	}

	/* Nope, this is a skip array */
	Assert(skey->sk_flags & SK_BT_SKIP);

	/*
	 * The sentinel value that represents the maximum value within the range
	 * of a skip array (often just +inf) is never incrementable
	 */
	if (skey->sk_flags & SK_BT_MAXVAL)
		return false;

	/*
	 * When the current array element is NULL, and the highest sorting value
	 * in the index is also NULL, we cannot increment past the final element
	 */
	if ((skey->sk_flags & SK_ISNULL) && !(skey->sk_flags & SK_BT_NULLS_FIRST))
		return false;

	/*
	 * Opclasses without skip support "increment" the scan key's current
	 * element by setting the NEXT flag.  The true next value is determined by
	 * repositioning to the first index tuple > existing sk_argument/current
	 * array element.  Note that this works in the usual way when the scan key
	 * is already marked ISNULL (i.e. when the current element is NULL).
	 */
	if (!array->sksup)
	{
		/* Successfully "incremented" array */
		skey->sk_flags |= SK_BT_NEXT;
		return true;
	}

	/*
	 * Opclasses with skip support directly increment sk_argument
	 */
	if (skey->sk_flags & SK_ISNULL)
	{
		Assert(skey->sk_flags & SK_BT_NULLS_FIRST);

		/*
		 * Existing sk_argument/array element is NULL (for an IS NULL qual).
		 *
		 * "Increment" from NULL to the low_elem value provided by opclass
		 * skip support routine.
		 */
		skey->sk_flags &= ~(SK_SEARCHNULL | SK_ISNULL);
		skey->sk_argument = datumCopy(array->sksup->low_elem,
									  array->attbyval, array->attlen);
		return true;
	}

	/*
	 * Ask opclass support routine to provide incremented copy of existing
	 * non-NULL sk_argument
	 */
	inc_sk_argument = array->sksup->increment(rel, skey->sk_argument, &oflow);
	if (unlikely(oflow))
	{
		/* inc_sk_argument has undefined value (so no pfree) */
		if (array->null_elem && !(skey->sk_flags & SK_BT_NULLS_FIRST))
		{
			_bt_skiparray_set_isnull(rel, skey, array);

			/* Successfully "incremented" array to NULL */
			return true;
		}

		/* Cannot increment past final array element */
		return false;
	}

	/*
	 * Successfully incremented sk_argument to a non-NULL value.  Make sure
	 * that the incremented value is still within the range of the array.
	 */
	if (array->high_compare &&
		!DatumGetBool(FunctionCall2Coll(&array->high_compare->sk_func,
										array->high_compare->sk_collation,
										inc_sk_argument,
										array->high_compare->sk_argument)))
	{
		/* Keep existing sk_argument after all */
		if (!array->attbyval)
			pfree(DatumGetPointer(inc_sk_argument));

		/* Cannot increment past final array element */
		return false;
	}

	/* Accept value returned by opclass increment callback */
	if (!array->attbyval && skey->sk_argument)
		pfree(DatumGetPointer(skey->sk_argument));
	skey->sk_argument = inc_sk_argument;

	/* Successfully incremented array */
	return true;
}

/*
 * _bt_advance_array_keys_increment() -- Advance to next set of array elements
 *
 * Advances the array keys by a single increment in the current scan
 * direction.  When there are multiple array keys this can roll over from the
 * lowest order array to higher order arrays.
 *
 * Returns true if there is another set of values to consider, false if not.
 * On true result, the scankeys are initialized with the next set of values.
 * On false result, the scankeys stay the same, and the array keys are not
 * advanced (every array remains at its final element for scan direction).
 */
static bool
_bt_advance_array_keys_increment(IndexScanDesc scan, ScanDirection dir,
								 bool *skip_array_set)
{
	Relation	rel = scan->indexRelation;
	BTScanOpaque so = (BTScanOpaque) scan->opaque;

	/*
	 * We must advance the last array key most quickly, since it will
	 * correspond to the lowest-order index column among the available
	 * qualifications
	 */
	for (int i = so->numArrayKeys - 1; i >= 0; i--)
	{
		BTArrayKeyInfo *array = &so->arrayKeys[i];
		ScanKey		skey = &so->keyData[array->scan_key];

		if (array->num_elems == -1)
			*skip_array_set = true;

		if (ScanDirectionIsForward(dir))
		{
			if (_bt_array_increment(rel, skey, array))
				return true;
		}
		else
		{
			if (_bt_array_decrement(rel, skey, array))
				return true;
		}

		/*
		 * Couldn't increment (or decrement) array.  Handle array roll over.
		 *
		 * Start over at the array's lowest sorting value (or its highest
		 * value, for backward scans)...
		 */
		_bt_array_set_low_or_high(rel, skey, array,
								  ScanDirectionIsForward(dir));

		/* ...then increment (or decrement) next most significant array */
	}

	/*
	 * The array keys are now exhausted.
	 *
	 * Restore the array keys to the state they were in immediately before we
	 * were called.  This ensures that the arrays only ever ratchet in the
	 * current scan direction.
	 *
	 * Without this, scans could overlook matching tuples when the scan
	 * direction gets reversed just before btgettuple runs out of items to
	 * return, but just after _bt_readpage prepares all the items from the
	 * scan's final page in so->currPos.  When we're on the final page it is
	 * typical for so->currPos to get invalidated once btgettuple finally
	 * returns false, which'll effectively invalidate the scan's array keys.
	 * That hasn't happened yet, though -- and in general it may never happen.
	 */
	_bt_start_array_keys(scan, -dir);

	return false;
}

/*
 * _bt_rewind_nonrequired_arrays() -- Rewind SAOP arrays not marked required
 *
 * Called when _bt_advance_array_keys decides to start a new primitive index
 * scan on the basis of the current scan position being before the position
 * that _bt_first is capable of repositioning the scan to by applying an
 * inequality operator required in the opposite-to-scan direction only.
 *
 * Although equality strategy scan keys (for both arrays and non-arrays alike)
 * are either marked required in both directions or in neither direction,
 * there is a sense in which non-required arrays behave like required arrays.
 * With a qual such as "WHERE a IN (100, 200) AND b >= 3 AND c IN (5, 6, 7)",
 * the scan key on "c" is non-required, but nevertheless enables positioning
 * the scan at the first tuple >= "(100, 3, 5)" on the leaf level during the
 * first descent of the tree by _bt_first.  Later on, there could also be a
 * second descent, that places the scan right before tuples >= "(200, 3, 5)".
 * _bt_first must never be allowed to build an insertion scan key whose "c"
 * entry is set to a value other than 5, the "c" array's first element/value.
 * (Actually, it's the first in the current scan direction.  This example uses
 * a forward scan.)
 *
 * Calling here resets the array scan key elements for the scan's non-required
 * arrays.  This is strictly necessary for correctness in a subset of cases
 * involving "required in opposite direction"-triggered primitive index scans.
 * Not all callers are at risk of _bt_first using a non-required array like
 * this, but advancement always resets the arrays when another primitive scan
 * is scheduled, just to keep things simple.  Array advancement even makes
 * sure to reset non-required arrays during scans that have no inequalities.
 * (Advancement still won't call here when there are no inequalities, though
 * that's just because it's all handled indirectly instead.)
 *
 * Note: _bt_verify_arrays_bt_first is called by an assertion to enforce that
 * everybody got this right.
 *
 * Note: In practice almost all SAOP arrays are marked required during
 * preprocessing (if necessary by generating skip arrays).  It is hardly ever
 * truly necessary to call here, but consistently doing so is simpler.
 */
static void
_bt_rewind_nonrequired_arrays(IndexScanDesc scan, ScanDirection dir)
{
	Relation	rel = scan->indexRelation;
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	int			arrayidx = 0;

	for (int ikey = 0; ikey < so->numberOfKeys; ikey++)
	{
		ScanKey		cur = so->keyData + ikey;
		BTArrayKeyInfo *array = NULL;

		if (!(cur->sk_flags & SK_SEARCHARRAY) ||
			cur->sk_strategy != BTEqualStrategyNumber)
			continue;

		array = &so->arrayKeys[arrayidx++];
		Assert(array->scan_key == ikey);

		if ((cur->sk_flags & (SK_BT_REQFWD | SK_BT_REQBKWD)))
			continue;

		Assert(array->num_elems != -1); /* No non-required skip arrays */

		_bt_array_set_low_or_high(rel, cur, array,
								  ScanDirectionIsForward(dir));
	}
}

/*
 * _bt_tuple_before_array_skeys() -- too early to advance required arrays?
 *
 * We always compare the tuple using the current array keys (which we assume
 * are already set in so->keyData[]).  readpagetup indicates if tuple is the
 * scan's current _bt_readpage-wise tuple.
 *
 * readpagetup callers must only call here when _bt_check_compare already set
 * continuescan=false.  We help these callers deal with _bt_check_compare's
 * inability to distinguish between the < and > cases (it uses equality
 * operator scan keys, whereas we use 3-way ORDER procs).  These callers pass
 * a _bt_check_compare-set sktrig value that indicates which scan key
 * triggered the call (!readpagetup callers just pass us sktrig=0 instead).
 * This information allows us to avoid wastefully checking earlier scan keys
 * that were already deemed to have been satisfied inside _bt_check_compare.
 *
 * Returns false when caller's tuple is >= the current required equality scan
 * keys (or <=, in the case of backwards scans).  This happens to readpagetup
 * callers when the scan has reached the point of needing its array keys
 * advanced; caller will need to advance required and non-required arrays at
 * scan key offsets >= sktrig, plus scan keys < sktrig iff sktrig rolls over.
 * (When we return false to readpagetup callers, tuple can only be == current
 * required equality scan keys when caller's sktrig indicates that the arrays
 * need to be advanced due to an unsatisfied required inequality key trigger.)
 *
 * Returns true when caller passes a tuple that is < the current set of
 * equality keys for the most significant non-equal required scan key/column
 * (or > the keys, during backwards scans).  This happens to readpagetup
 * callers when tuple is still before the start of matches for the scan's
 * required equality strategy scan keys.  (sktrig can't have indicated that an
 * inequality strategy scan key wasn't satisfied in _bt_check_compare when we
 * return true.  In fact, we automatically return false when passed such an
 * inequality sktrig by readpagetup callers -- _bt_check_compare's initial
 * continuescan=false doesn't really need to be confirmed here by us.)
 *
 * !readpagetup callers optionally pass us *scanBehind, which tracks whether
 * any missing truncated attributes might have affected array advancement
 * (compared to what would happen if it was shown the first non-pivot tuple on
 * the page to the right of caller's finaltup/high key tuple instead).  It's
 * only possible that we'll set *scanBehind to true when caller passes us a
 * pivot tuple (with truncated -inf attributes) that we return false for.
 */
static bool
_bt_tuple_before_array_skeys(IndexScanDesc scan, ScanDirection dir,
							 IndexTuple tuple, TupleDesc tupdesc, int tupnatts,
							 bool readpagetup, int sktrig, bool *scanBehind)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;

	Assert(so->numArrayKeys);
	Assert(so->numberOfKeys);
	Assert(sktrig == 0 || readpagetup);
	Assert(!readpagetup || scanBehind == NULL);

	if (scanBehind)
		*scanBehind = false;

	for (int ikey = sktrig; ikey < so->numberOfKeys; ikey++)
	{
		ScanKey		cur = so->keyData + ikey;
		Datum		tupdatum;
		bool		tupnull;
		int32		result;

		/* readpagetup calls require one ORDER proc comparison (at most) */
		Assert(!readpagetup || ikey == sktrig);

		/*
		 * Once we reach a non-required scan key, we're completely done.
		 *
		 * Note: we deliberately don't consider the scan direction here.
		 * _bt_advance_array_keys caller requires that we track *scanBehind
		 * without concern for scan direction.
		 */
		if ((cur->sk_flags & (SK_BT_REQFWD | SK_BT_REQBKWD)) == 0)
		{
			Assert(!readpagetup);
			Assert(ikey > sktrig || ikey == 0);
			return false;
		}

		if (cur->sk_attno > tupnatts)
		{
			Assert(!readpagetup);

			/*
			 * When we reach a high key's truncated attribute, assume that the
			 * tuple attribute's value is >= the scan's equality constraint
			 * scan keys (but set *scanBehind to let interested callers know
			 * that a truncated attribute might have affected our answer).
			 */
			if (scanBehind)
				*scanBehind = true;

			return false;
		}

		/*
		 * Deal with inequality strategy scan keys that _bt_check_compare set
		 * continuescan=false for
		 */
		if (cur->sk_strategy != BTEqualStrategyNumber)
		{
			/*
			 * When _bt_check_compare indicated that a required inequality
			 * scan key wasn't satisfied, there's no need to verify anything;
			 * caller always calls _bt_advance_array_keys with this sktrig.
			 */
			if (readpagetup)
				return false;

			/*
			 * Otherwise we can't give up, since we must check all required
			 * scan keys (required in either direction) in order to correctly
			 * track *scanBehind for caller
			 */
			continue;
		}

		tupdatum = index_getattr(tuple, cur->sk_attno, tupdesc, &tupnull);

		if (likely(!(cur->sk_flags & (SK_BT_MINVAL | SK_BT_MAXVAL))))
		{
			/* Scankey has a valid/comparable sk_argument value */
			result = _bt_compare_array_skey(&so->orderProcs[ikey],
											tupdatum, tupnull,
											cur->sk_argument, cur);

			if (result == 0)
			{
				/*
				 * Interpret result in a way that takes NEXT/PRIOR into
				 * account
				 */
				if (cur->sk_flags & SK_BT_NEXT)
					result = -1;
				else if (cur->sk_flags & SK_BT_PRIOR)
					result = 1;

				Assert(result == 0 || (cur->sk_flags & SK_BT_SKIP));
			}
		}
		else
		{
			BTArrayKeyInfo *array = NULL;

			/*
			 * Current array element/array = scan key value is a sentinel
			 * value that represents the lowest (or highest) possible value
			 * that's still within the range of the array.
			 *
			 * Like _bt_first, we only see MINVAL keys during forwards scans
			 * (and similarly only see MAXVAL keys during backwards scans).
			 * Even if the scan's direction changes, we'll stop at some higher
			 * order key before we can ever reach any MAXVAL (or MINVAL) keys.
			 * (However, unlike _bt_first we _can_ get to keys marked either
			 * NEXT or PRIOR, regardless of the scan's current direction.)
			 */
			Assert(ScanDirectionIsForward(dir) ?
				   !(cur->sk_flags & SK_BT_MAXVAL) :
				   !(cur->sk_flags & SK_BT_MINVAL));

			/*
			 * There are no valid sk_argument values in MINVAL/MAXVAL keys.
			 * Check if tupdatum is within the range of skip array instead.
			 */
			for (int arrayidx = 0; arrayidx < so->numArrayKeys; arrayidx++)
			{
				array = &so->arrayKeys[arrayidx];
				if (array->scan_key == ikey)
					break;
			}

			_bt_binsrch_skiparray_skey(false, dir, tupdatum, tupnull,
									   array, cur, &result);

			if (result == 0)
			{
				/*
				 * tupdatum satisfies both low_compare and high_compare, so
				 * it's time to advance the array keys.
				 *
				 * Note: It's possible that the skip array will "advance" from
				 * its MINVAL (or MAXVAL) representation to an alternative,
				 * logically equivalent representation of the same value: a
				 * representation where the = key gets a valid datum in its
				 * sk_argument.  This is only possible when low_compare uses
				 * the >= strategy (or high_compare uses the <= strategy).
				 */
				return false;
			}
		}

		/*
		 * Does this comparison indicate that caller must _not_ advance the
		 * scan's arrays just yet?
		 */
		if ((ScanDirectionIsForward(dir) && result < 0) ||
			(ScanDirectionIsBackward(dir) && result > 0))
			return true;

		/*
		 * Does this comparison indicate that caller should now advance the
		 * scan's arrays?  (Must be if we get here during a readpagetup call.)
		 */
		if (readpagetup || result != 0)
		{
			Assert(result != 0);
			return false;
		}

		/*
		 * Inconclusive -- need to check later scan keys, too.
		 *
		 * This must be a finaltup precheck, or a call made from an assertion.
		 */
		Assert(result == 0);
	}

	Assert(!readpagetup);

	return false;
}

/*
 * _bt_start_prim_scan() -- start scheduled primitive index scan?
 *
 * Returns true if _bt_checkkeys scheduled another primitive index scan, just
 * as the last one ended.  Otherwise returns false, indicating that the array
 * keys are now fully exhausted.
 *
 * Only call here during scans with one or more equality type array scan keys,
 * after _bt_first or _bt_next return false.
 */
bool
_bt_start_prim_scan(IndexScanDesc scan, ScanDirection dir)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;

	Assert(so->numArrayKeys);

	so->scanBehind = so->oppositeDirCheck = false;	/* reset */

	/*
	 * Array keys are advanced within _bt_checkkeys when the scan reaches the
	 * leaf level (more precisely, they're advanced when the scan reaches the
	 * end of each distinct set of array elements).  This process avoids
	 * repeat access to leaf pages (across multiple primitive index scans) by
	 * advancing the scan's array keys when it allows the primitive index scan
	 * to find nearby matching tuples (or when it eliminates ranges of array
	 * key space that can't possibly be satisfied by any index tuple).
	 *
	 * _bt_checkkeys sets a simple flag variable to schedule another primitive
	 * index scan.  The flag tells us what to do.
	 *
	 * We cannot rely on _bt_first always reaching _bt_checkkeys.  There are
	 * various cases where that won't happen.  For example, if the index is
	 * completely empty, then _bt_first won't call _bt_readpage/_bt_checkkeys.
	 * We also don't expect a call to _bt_checkkeys during searches for a
	 * non-existent value that happens to be lower/higher than any existing
	 * value in the index.
	 *
	 * We don't require special handling for these cases -- we don't need to
	 * be explicitly instructed to _not_ perform another primitive index scan.
	 * It's up to code under the control of _bt_first to always set the flag
	 * when another primitive index scan will be required.
	 *
	 * This works correctly, even with the tricky cases listed above, which
	 * all involve access to leaf pages "near the boundaries of the key space"
	 * (whether it's from a leftmost/rightmost page, or an imaginary empty
	 * leaf root page).  If _bt_checkkeys cannot be reached by a primitive
	 * index scan for one set of array keys, then it also won't be reached for
	 * any later set ("later" in terms of the direction that we scan the index
	 * and advance the arrays).  The array keys won't have advanced in these
	 * cases, but that's the correct behavior (even _bt_advance_array_keys
	 * won't always advance the arrays at the point they become "exhausted").
	 */
	if (so->needPrimScan)
	{
		Assert(_bt_verify_arrays_bt_first(scan, dir));

		/*
		 * Flag was set -- must call _bt_first again, which will reset the
		 * scan's needPrimScan flag
		 */
		return true;
	}

	/* The top-level index scan ran out of tuples in this scan direction */
	if (scan->parallel_scan != NULL)
		_bt_parallel_done(scan);

	return false;
}

/*
 * _bt_advance_array_keys() -- Advance array elements using a tuple
 *
 * The scan always gets a new qual as a consequence of calling here (except
 * when we determine that the top-level scan has run out of matching tuples).
 * All later _bt_check_compare calls also use the same new qual that was first
 * used here (at least until the next call here advances the keys once again).
 * It's convenient to structure _bt_check_compare rechecks of caller's tuple
 * (using the new qual) as one the steps of advancing the scan's array keys,
 * so this function works as a wrapper around _bt_check_compare.
 *
 * Like _bt_check_compare, we'll set pstate.continuescan on behalf of the
 * caller, and return a boolean indicating if caller's tuple satisfies the
 * scan's new qual.  But unlike _bt_check_compare, we set so->needPrimScan
 * when we set continuescan=false, indicating if a new primitive index scan
 * has been scheduled (otherwise, the top-level scan has run out of tuples in
 * the current scan direction).
 *
 * Caller must use _bt_tuple_before_array_skeys to determine if the current
 * place in the scan is >= the current array keys _before_ calling here.
 * We're responsible for ensuring that caller's tuple is <= the newly advanced
 * required array keys once we return.  We try to find an exact match, but
 * failing that we'll advance the array keys to whatever set of array elements
 * comes next in the key space for the current scan direction.  Required array
 * keys "ratchet forwards" (or backwards).  They can only advance as the scan
 * itself advances through the index/key space.
 *
 * (The rules are the same for backwards scans, except that the operators are
 * flipped: just replace the precondition's >= operator with a <=, and the
 * postcondition's <= operator with a >=.  In other words, just swap the
 * precondition with the postcondition.)
 *
 * We also deal with "advancing" non-required arrays here (or arrays that are
 * treated as non-required for the duration of a _bt_readpage call).  Callers
 * whose sktrig scan key is non-required specify sktrig_required=false.  These
 * calls are the only exception to the general rule about always advancing the
 * required array keys (the scan may not even have a required array).  These
 * callers should just pass a NULL pstate (since there is never any question
 * of stopping the scan).  No call to _bt_tuple_before_array_skeys is required
 * ahead of these calls (it's already clear that any required scan keys must
 * be satisfied by caller's tuple).
 *
 * Note that we deal with non-array required equality strategy scan keys as
 * degenerate single element arrays here.  Obviously, they can never really
 * advance in the way that real arrays can, but they must still affect how we
 * advance real array scan keys (exactly like true array equality scan keys).
 * We have to keep around a 3-way ORDER proc for these (using the "=" operator
 * won't do), since in general whether the tuple is < or > _any_ unsatisfied
 * required equality key influences how the scan's real arrays must advance.
 *
 * Note also that we may sometimes need to advance the array keys when the
 * existing required array keys (and other required equality keys) are already
 * an exact match for every corresponding value from caller's tuple.  We must
 * do this for inequalities that _bt_check_compare set continuescan=false for.
 * They'll advance the array keys here, just like any other scan key that
 * _bt_check_compare stops on.  (This can even happen _after_ we advance the
 * array keys, in which case we'll advance the array keys a second time.  That
 * way _bt_checkkeys caller always has its required arrays advance to the
 * maximum possible extent that its tuple will allow.)
 */
static bool
_bt_advance_array_keys(IndexScanDesc scan, BTReadPageState *pstate,
					   IndexTuple tuple, int tupnatts, TupleDesc tupdesc,
					   int sktrig, bool sktrig_required)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	Relation	rel = scan->indexRelation;
	ScanDirection dir = so->currPos.dir;
	int			arrayidx = 0;
	bool		beyond_end_advance = false,
				skip_array_advanced = false,
				has_required_opposite_direction_only = false,
				all_required_satisfied = true,
				all_satisfied = true;

	Assert(!so->needPrimScan && !so->scanBehind && !so->oppositeDirCheck);
	Assert(_bt_verify_keys_with_arraykeys(scan));

	if (sktrig_required)
	{
		/*
		 * Precondition array state assertion
		 */
		Assert(!_bt_tuple_before_array_skeys(scan, dir, tuple, tupdesc,
											 tupnatts, false, 0, NULL));

		/*
		 * Once we return we'll have a new set of required array keys, so
		 * reset state used by "look ahead" optimization
		 */
		pstate->rechecks = 0;
		pstate->targetdistance = 0;
	}
	else if (sktrig < so->numberOfKeys - 1 &&
			 !(so->keyData[so->numberOfKeys - 1].sk_flags & SK_SEARCHARRAY))
	{
		int			least_sign_ikey = so->numberOfKeys - 1;
		bool		continuescan;

		/*
		 * Optimization: perform a precheck of the least significant key
		 * during !sktrig_required calls when it isn't already our sktrig
		 * (provided the precheck key is not itself an array).
		 *
		 * When the precheck works out we'll avoid an expensive binary search
		 * of sktrig's array (plus any other arrays before least_sign_ikey).
		 */
		Assert(so->keyData[sktrig].sk_flags & SK_SEARCHARRAY);
		if (!_bt_check_compare(scan, dir, tuple, tupnatts, tupdesc, false,
							   false, &continuescan,
							   &least_sign_ikey))
			return false;
	}

	for (int ikey = 0; ikey < so->numberOfKeys; ikey++)
	{
		ScanKey		cur = so->keyData + ikey;
		BTArrayKeyInfo *array = NULL;
		Datum		tupdatum;
		bool		required = false,
					required_opposite_direction_only = false,
					tupnull;
		int32		result;
		int			set_elem = 0;

		if (cur->sk_strategy == BTEqualStrategyNumber)
		{
			/* Manage array state */
			if (cur->sk_flags & SK_SEARCHARRAY)
			{
				array = &so->arrayKeys[arrayidx++];
				Assert(array->scan_key == ikey);
			}
		}
		else
		{
			/*
			 * Are any inequalities required in the opposite direction only
			 * present here?
			 */
			if (((ScanDirectionIsForward(dir) &&
				  (cur->sk_flags & (SK_BT_REQBKWD))) ||
				 (ScanDirectionIsBackward(dir) &&
				  (cur->sk_flags & (SK_BT_REQFWD)))))
				has_required_opposite_direction_only =
					required_opposite_direction_only = true;
		}

		/* Optimization: skip over known-satisfied scan keys */
		if (ikey < sktrig)
			continue;

		if (cur->sk_flags & (SK_BT_REQFWD | SK_BT_REQBKWD))
		{
			required = true;

			if (cur->sk_attno > tupnatts)
			{
				/* Set this just like _bt_tuple_before_array_skeys */
				Assert(sktrig < ikey);
				so->scanBehind = true;
			}
		}

		/*
		 * Handle a required non-array scan key that the initial call to
		 * _bt_check_compare indicated triggered array advancement, if any.
		 *
		 * The non-array scan key's strategy will be <, <=, or = during a
		 * forwards scan (or any one of =, >=, or > during a backwards scan).
		 * It follows that the corresponding tuple attribute's value must now
		 * be either > or >= the scan key value (for backwards scans it must
		 * be either < or <= that value).
		 *
		 * If this is a required equality strategy scan key, this is just an
		 * optimization; _bt_tuple_before_array_skeys already confirmed that
		 * this scan key places us ahead of caller's tuple.  There's no need
		 * to repeat that work now.  (The same underlying principle also gets
		 * applied by the cur_elem_trig optimization used to speed up searches
		 * for the next array element.)
		 *
		 * If this is a required inequality strategy scan key, we _must_ rely
		 * on _bt_check_compare like this; we aren't capable of directly
		 * evaluating required inequality strategy scan keys here, on our own.
		 */
		if (ikey == sktrig && !array)
		{
			Assert(sktrig_required && required && all_required_satisfied);

			/* Use "beyond end" advancement.  See below for an explanation. */
			beyond_end_advance = true;
			all_satisfied = all_required_satisfied = false;

			continue;
		}

		/*
		 * Nothing more for us to do with an inequality strategy scan key that
		 * wasn't the one that _bt_check_compare stopped on, though.
		 *
		 * Note: if our later call to _bt_check_compare (to recheck caller's
		 * tuple) sets continuescan=false due to finding this same inequality
		 * unsatisfied (possible when it's required in the scan direction),
		 * we'll deal with it via a recursive "second pass" call.
		 */
		else if (cur->sk_strategy != BTEqualStrategyNumber)
			continue;

		/*
		 * Nothing for us to do with an equality strategy scan key that isn't
		 * marked required, either -- unless it's a non-required array
		 */
		else if (!required && !array)
			continue;

		/*
		 * Here we perform steps for all array scan keys after a required
		 * array scan key whose binary search triggered "beyond end of array
		 * element" array advancement due to encountering a tuple attribute
		 * value > the closest matching array key (or < for backwards scans).
		 */
		if (beyond_end_advance)
		{
			if (array)
				_bt_array_set_low_or_high(rel, cur, array,
										  ScanDirectionIsBackward(dir));

			continue;
		}

		/*
		 * Here we perform steps for all array scan keys after a required
		 * array scan key whose tuple attribute was < the closest matching
		 * array key when we dealt with it (or > for backwards scans).
		 *
		 * This earlier required array key already puts us ahead of caller's
		 * tuple in the key space (for the current scan direction).  We must
		 * make sure that subsequent lower-order array keys do not put us too
		 * far ahead (ahead of tuples that have yet to be seen by our caller).
		 * For example, when a tuple "(a, b) = (42, 5)" advances the array
		 * keys on "a" from 40 to 45, we must also set "b" to whatever the
		 * first array element for "b" is.  It would be wrong to allow "b" to
		 * be set based on the tuple value.
		 *
		 * Perform the same steps with truncated high key attributes.  You can
		 * think of this as a "binary search" for the element closest to the
		 * value -inf.  Again, the arrays must never get ahead of the scan.
		 */
		if (!all_required_satisfied || cur->sk_attno > tupnatts)
		{
			if (array)
				_bt_array_set_low_or_high(rel, cur, array,
										  ScanDirectionIsForward(dir));

			continue;
		}

		/*
		 * Search in scankey's array for the corresponding tuple attribute
		 * value from caller's tuple
		 */
		tupdatum = index_getattr(tuple, cur->sk_attno, tupdesc, &tupnull);

		if (array)
		{
			bool		cur_elem_trig = (sktrig_required && ikey == sktrig);

			/*
			 * "Binary search" by checking if tupdatum/tupnull are within the
			 * range of the skip array
			 */
			if (array->num_elems == -1)
				_bt_binsrch_skiparray_skey(cur_elem_trig, dir,
										   tupdatum, tupnull, array, cur,
										   &result);

			/*
			 * Binary search for the closest match from the SAOP array
			 */
			else
				set_elem = _bt_binsrch_array_skey(&so->orderProcs[ikey],
												  cur_elem_trig, dir,
												  tupdatum, tupnull, array, cur,
												  &result);
		}
		else
		{
			Assert(required);

			/*
			 * This is a required non-array equality strategy scan key, which
			 * we'll treat as a degenerate single element array.
			 *
			 * This scan key's imaginary "array" can't really advance, but it
			 * can still roll over like any other array.  (Actually, this is
			 * no different to real single value arrays, which never advance
			 * without rolling over -- they can never truly advance, either.)
			 */
			result = _bt_compare_array_skey(&so->orderProcs[ikey],
											tupdatum, tupnull,
											cur->sk_argument, cur);
		}

		/*
		 * Consider "beyond end of array element" array advancement.
		 *
		 * When the tuple attribute value is > the closest matching array key
		 * (or < in the backwards scan case), we need to ratchet this array
		 * forward (backward) by one increment, so that caller's tuple ends up
		 * being < final array value instead (or > final array value instead).
		 * This process has to work for all of the arrays, not just this one:
		 * it must "carry" to higher-order arrays when the set_elem that we
		 * just found happens to be the final one for the scan's direction.
		 * Incrementing (decrementing) set_elem itself isn't good enough.
		 *
		 * Our approach is to provisionally use set_elem as if it was an exact
		 * match now, then set each later/less significant array to whatever
		 * its final element is.  Once outside the loop we'll then "increment
		 * this array's set_elem" by calling _bt_advance_array_keys_increment.
		 * That way the process rolls over to higher order arrays as needed.
		 *
		 * Under this scheme any required arrays only ever ratchet forwards
		 * (or backwards), and always do so to the maximum possible extent
		 * that we can know will be safe without seeing the scan's next tuple.
		 * We don't need any special handling for required scan keys that lack
		 * a real array to advance, nor for redundant scan keys that couldn't
		 * be eliminated by _bt_preprocess_keys.  It won't matter if some of
		 * our "true" array scan keys (or even all of them) are non-required.
		 */
		if (sktrig_required && required &&
			((ScanDirectionIsForward(dir) && result > 0) ||
			 (ScanDirectionIsBackward(dir) && result < 0)))
			beyond_end_advance = true;

		Assert(all_required_satisfied && all_satisfied);
		if (result != 0)
		{
			/*
			 * Track whether caller's tuple satisfies our new post-advancement
			 * qual, for required scan keys, as well as for the entire set of
			 * interesting scan keys (all required scan keys plus non-required
			 * array scan keys are considered interesting.)
			 */
			all_satisfied = false;
			if (sktrig_required && required)
				all_required_satisfied = false;
			else
			{
				/*
				 * There's no need to advance the arrays using the best
				 * available match for a non-required array.  Give up now.
				 * (Though note that sktrig_required calls still have to do
				 * all the usual post-advancement steps, including the recheck
				 * call to _bt_check_compare.)
				 */
				break;
			}
		}

		/* Advance array keys, even when we don't have an exact match */
		if (array)
		{
			if (array->num_elems == -1)
			{
				/* Skip array's new element is tupdatum (or MINVAL/MAXVAL) */
				_bt_skiparray_set_element(rel, cur, array, result,
										  tupdatum, tupnull);
				skip_array_advanced = true;
			}
			else if (array->cur_elem != set_elem)
			{
				/* SAOP array's new element is set_elem datum */
				array->cur_elem = set_elem;
				cur->sk_argument = array->elem_values[set_elem];
			}
		}
	}

	/*
	 * Advance the array keys incrementally whenever "beyond end of array
	 * element" array advancement happens, so that advancement will carry to
	 * higher-order arrays (might exhaust all the scan's arrays instead, which
	 * ends the top-level scan).
	 */
	if (beyond_end_advance &&
		!_bt_advance_array_keys_increment(scan, dir, &skip_array_advanced))
		goto end_toplevel_scan;

	Assert(_bt_verify_keys_with_arraykeys(scan));

	/*
	 * Maintain a page-level count of the number of times the scan's array
	 * keys advanced in a way that affected at least one skip array
	 */
	if (sktrig_required && skip_array_advanced)
		pstate->nskipadvances++;

	/*
	 * Does tuple now satisfy our new qual?  Recheck with _bt_check_compare.
	 *
	 * Calls triggered by an unsatisfied required scan key, whose tuple now
	 * satisfies all required scan keys, but not all nonrequired array keys,
	 * will still require a recheck call to _bt_check_compare.  They'll still
	 * need its "second pass" handling of required inequality scan keys.
	 * (Might have missed a still-unsatisfied required inequality scan key
	 * that caller didn't detect as the sktrig scan key during its initial
	 * _bt_check_compare call that used the old/original qual.)
	 *
	 * Calls triggered by an unsatisfied nonrequired array scan key never need
	 * "second pass" handling of required inequalities (nor any other handling
	 * of any required scan key).  All that matters is whether caller's tuple
	 * satisfies the new qual, so it's safe to just skip the _bt_check_compare
	 * recheck when we've already determined that it can only return 'false'.
	 *
	 * Note: In practice most scan keys are marked required by preprocessing,
	 * if necessary by generating a preceding skip array.  We nevertheless
	 * often handle array keys marked required as if they were nonrequired.
	 * This behavior is requested by our _bt_check_compare caller, though only
	 * when it is passed "forcenonrequired=true" by _bt_checkkeys.
	 */
	if ((sktrig_required && all_required_satisfied) ||
		(!sktrig_required && all_satisfied))
	{
		int			nsktrig = sktrig + 1;
		bool		continuescan;

		Assert(all_required_satisfied);

		/* Recheck _bt_check_compare on behalf of caller */
		if (_bt_check_compare(scan, dir, tuple, tupnatts, tupdesc, false,
							  !sktrig_required, &continuescan,
							  &nsktrig) &&
			!so->scanBehind)
		{
			/* This tuple satisfies the new qual */
			Assert(all_satisfied && continuescan);

			if (pstate)
				pstate->continuescan = true;

			return true;
		}

		/*
		 * Consider "second pass" handling of required inequalities.
		 *
		 * It's possible that our _bt_check_compare call indicated that the
		 * scan should end due to some unsatisfied inequality that wasn't
		 * initially recognized as such by us.  Handle this by calling
		 * ourselves recursively, this time indicating that the trigger is the
		 * inequality that we missed first time around (and using a set of
		 * required array/equality keys that are now exact matches for tuple).
		 *
		 * We make a strong, general guarantee that every _bt_checkkeys call
		 * here will advance the array keys to the maximum possible extent
		 * that we can know to be safe based on caller's tuple alone.  If we
		 * didn't perform this step, then that guarantee wouldn't quite hold.
		 */
		if (unlikely(!continuescan))
		{
			bool		satisfied PG_USED_FOR_ASSERTS_ONLY;

			Assert(sktrig_required);
			Assert(so->keyData[nsktrig].sk_strategy != BTEqualStrategyNumber);

			/*
			 * The tuple must use "beyond end" advancement during the
			 * recursive call, so we cannot possibly end up back here when
			 * recursing.  We'll consume a small, fixed amount of stack space.
			 */
			Assert(!beyond_end_advance);

			/* Advance the array keys a second time using same tuple */
			satisfied = _bt_advance_array_keys(scan, pstate, tuple, tupnatts,
											   tupdesc, nsktrig, true);

			/* This tuple doesn't satisfy the inequality */
			Assert(!satisfied);
			return false;
		}

		/*
		 * Some non-required scan key (from new qual) still not satisfied.
		 *
		 * All scan keys required in the current scan direction must still be
		 * satisfied, though, so we can trust all_required_satisfied below.
		 */
	}

	/*
	 * When we were called just to deal with "advancing" non-required arrays,
	 * this is as far as we can go (cannot stop the scan for these callers)
	 */
	if (!sktrig_required)
	{
		/* Caller's tuple doesn't match any qual */
		return false;
	}

	/*
	 * Postcondition array state assertion (for still-unsatisfied tuples).
	 *
	 * By here we have established that the scan's required arrays (scan must
	 * have at least one required array) advanced, without becoming exhausted.
	 *
	 * Caller's tuple is now < the newly advanced array keys (or > when this
	 * is a backwards scan), except in the case where we only got this far due
	 * to an unsatisfied non-required scan key.  Verify that with an assert.
	 *
	 * Note: we don't just quit at this point when all required scan keys were
	 * found to be satisfied because we need to consider edge-cases involving
	 * scan keys required in the opposite direction only; those aren't tracked
	 * by all_required_satisfied.
	 */
	Assert(_bt_tuple_before_array_skeys(scan, dir, tuple, tupdesc, tupnatts,
										false, 0, NULL) ==
		   !all_required_satisfied);

	/*
	 * We generally permit primitive index scans to continue onto the next
	 * sibling page when the page's finaltup satisfies all required scan keys
	 * at the point where we're between pages.
	 *
	 * If caller's tuple is also the page's finaltup, and we see that required
	 * scan keys still aren't satisfied, start a new primitive index scan.
	 */
	if (!all_required_satisfied && pstate->finaltup == tuple)
		goto new_prim_scan;

	/*
	 * Proactively check finaltup (don't wait until finaltup is reached by the
	 * scan) when it might well turn out to not be satisfied later on.
	 *
	 * Note: if so->scanBehind hasn't already been set for finaltup by us,
	 * it'll be set during this call to _bt_tuple_before_array_skeys.  Either
	 * way, it'll be set correctly (for the whole page) after this point.
	 */
	if (!all_required_satisfied && pstate->finaltup &&
		_bt_tuple_before_array_skeys(scan, dir, pstate->finaltup, tupdesc,
									 BTreeTupleGetNAtts(pstate->finaltup, rel),
									 false, 0, &so->scanBehind))
		goto new_prim_scan;

	/*
	 * When we encounter a truncated finaltup high key attribute, we're
	 * optimistic about the chances of its corresponding required scan key
	 * being satisfied when we go on to recheck it against tuples from this
	 * page's right sibling leaf page.  We consider truncated attributes to be
	 * satisfied by required scan keys, which allows the primitive index scan
	 * to continue to the next leaf page.  We must set so->scanBehind to true
	 * to remember that the last page's finaltup had "satisfied" required scan
	 * keys for one or more truncated attribute values (scan keys required in
	 * _either_ scan direction).
	 *
	 * There is a chance that _bt_readpage (which checks so->scanBehind) will
	 * find that even the sibling leaf page's finaltup is < the new array
	 * keys.  When that happens, our optimistic policy will have incurred a
	 * single extra leaf page access that could have been avoided.
	 *
	 * A pessimistic policy would give backward scans a gratuitous advantage
	 * over forward scans.  We'd punish forward scans for applying more
	 * accurate information from the high key, rather than just using the
	 * final non-pivot tuple as finaltup, in the style of backward scans.
	 * Being pessimistic would also give some scans with non-required arrays a
	 * perverse advantage over similar scans that use required arrays instead.
	 *
	 * This is similar to our scan-level heuristics, below.  They also set
	 * scanBehind to speculatively continue the primscan onto the next page.
	 */
	if (so->scanBehind)
	{
		/* Truncated high key -- _bt_scanbehind_checkkeys recheck scheduled */
	}

	/*
	 * Handle inequalities marked required in the opposite scan direction.
	 * They can also signal that we should start a new primitive index scan.
	 *
	 * It's possible that the scan is now positioned where "matching" tuples
	 * begin, and that caller's tuple satisfies all scan keys required in the
	 * current scan direction.  But if caller's tuple still doesn't satisfy
	 * other scan keys that are required in the opposite scan direction only
	 * (e.g., a required >= strategy scan key when scan direction is forward),
	 * it's still possible that there are many leaf pages before the page that
	 * _bt_first could skip straight to.  Groveling through all those pages
	 * will always give correct answers, but it can be very inefficient.  We
	 * must avoid needlessly scanning extra pages.
	 *
	 * Separately, it's possible that _bt_check_compare set continuescan=false
	 * for a scan key that's required in the opposite direction only.  This is
	 * a special case, that happens only when _bt_check_compare sees that the
	 * inequality encountered a NULL value.  This signals the end of non-NULL
	 * values in the current scan direction, which is reason enough to end the
	 * (primitive) scan.  If this happens at the start of a large group of
	 * NULL values, then we shouldn't expect to be called again until after
	 * the scan has already read indefinitely-many leaf pages full of tuples
	 * with NULL suffix values.  (_bt_first is expected to skip over the group
	 * of NULLs by applying a similar "deduce NOT NULL" rule of its own, which
	 * involves consing up an explicit SK_SEARCHNOTNULL key.)
	 *
	 * Apply a test against finaltup to detect and recover from the problem:
	 * if even finaltup doesn't satisfy such an inequality, we just skip by
	 * starting a new primitive index scan.  When we skip, we know for sure
	 * that all of the tuples on the current page following caller's tuple are
	 * also before the _bt_first-wise start of tuples for our new qual.  That
	 * at least suggests many more skippable pages beyond the current page.
	 * (when so->scanBehind and so->oppositeDirCheck are set, this'll happen
	 * when we test the next page's finaltup/high key instead.)
	 */
	else if (has_required_opposite_direction_only && pstate->finaltup &&
			 unlikely(!_bt_oppodir_checkkeys(scan, dir, pstate->finaltup)))
	{
		/*
		 * Make sure that any SAOP arrays that were not marked required by
		 * preprocessing are reset to their first element for this direction
		 */
		_bt_rewind_nonrequired_arrays(scan, dir);
		goto new_prim_scan;
	}

continue_scan:

	/*
	 * Stick with the ongoing primitive index scan for now.
	 *
	 * It's possible that later tuples will also turn out to have values that
	 * are still < the now-current array keys (or > the current array keys).
	 * Our caller will handle this by performing what amounts to a linear
	 * search of the page, implemented by calling _bt_check_compare and then
	 * _bt_tuple_before_array_skeys for each tuple.
	 *
	 * This approach has various advantages over a binary search of the page.
	 * Repeated binary searches of the page (one binary search for every array
	 * advancement) won't outperform a continuous linear search.  While there
	 * are workloads that a naive linear search won't handle well, our caller
	 * has a "look ahead" fallback mechanism to deal with that problem.
	 */
	pstate->continuescan = true;	/* Override _bt_check_compare */
	so->needPrimScan = false;	/* _bt_readpage has more tuples to check */

	if (so->scanBehind)
	{
		/*
		 * Remember if recheck needs to call _bt_oppodir_checkkeys for next
		 * page's finaltup (see above comments about "Handle inequalities
		 * marked required in the opposite scan direction" for why).
		 */
		so->oppositeDirCheck = has_required_opposite_direction_only;

		_bt_rewind_nonrequired_arrays(scan, dir);

		/*
		 * skip by setting "look ahead" mechanism's offnum for forwards scans
		 * (backwards scans check scanBehind flag directly instead)
		 */
		if (ScanDirectionIsForward(dir))
			pstate->skip = pstate->maxoff + 1;
	}

	/* Caller's tuple doesn't match the new qual */
	return false;

new_prim_scan:

	Assert(pstate->finaltup);	/* not on rightmost/leftmost page */

	/*
	 * Looks like another primitive index scan is required.  But consider
	 * continuing the current primscan based on scan-level heuristics.
	 *
	 * Continue the ongoing primitive scan (and schedule a recheck for when
	 * the scan arrives on the next sibling leaf page) when it has already
	 * read at least one leaf page before the one we're reading now.  This
	 * makes primscan scheduling more efficient when scanning subsets of an
	 * index with many distinct attribute values matching many array elements.
	 * It encourages fewer, larger primitive scans where that makes sense.
	 * This will in turn encourage _bt_readpage to apply the pstate.startikey
	 * optimization more often.
	 *
	 * Also continue the ongoing primitive index scan when it is still on the
	 * first page if there have been more than NSKIPADVANCES_THRESHOLD calls
	 * here that each advanced at least one of the scan's skip arrays
	 * (deliberately ignore advancements that only affected SAOP arrays here).
	 * A page that cycles through this many skip array elements is quite
	 * likely to neighbor similar pages, that we'll also need to read.
	 *
	 * Note: These heuristics aren't as aggressive as you might think.  We're
	 * conservative about allowing a primitive scan to step from the first
	 * leaf page it reads to the page's sibling page (we only allow it on
	 * first pages whose finaltup strongly suggests that it'll work out, as
	 * well as first pages that have a large number of skip array advances).
	 * Clearing this first page finaltup hurdle is a strong signal in itself.
	 *
	 * Note: The NSKIPADVANCES_THRESHOLD heuristic exists only to avoid
	 * pathological cases.  Specifically, cases where a skip scan should just
	 * behave like a traditional full index scan, but ends up "skipping" again
	 * and again, descending to the prior leaf page's direct sibling leaf page
	 * each time.  This misbehavior would otherwise be possible during scans
	 * that never quite manage to "clear the first page finaltup hurdle".
	 */
	if (!pstate->firstpage || pstate->nskipadvances > NSKIPADVANCES_THRESHOLD)
	{
		/* Schedule a recheck once on the next (or previous) page */
		so->scanBehind = true;

		/* Continue the current primitive scan after all */
		goto continue_scan;
	}

	/*
	 * End this primitive index scan, but schedule another.
	 *
	 * Note: We make a soft assumption that the current scan direction will
	 * also be used within _bt_next, when it is asked to step off this page.
	 * It is up to _bt_next to cancel this scheduled primitive index scan
	 * whenever it steps to a page in the direction opposite currPos.dir.
	 */
	pstate->continuescan = false;	/* Tell _bt_readpage we're done... */
	so->needPrimScan = true;	/* ...but call _bt_first again */

	if (scan->parallel_scan)
		_bt_parallel_primscan_schedule(scan, so->currPos.currPage);

	/* Caller's tuple doesn't match the new qual */
	return false;

end_toplevel_scan:

	/*
	 * End the current primitive index scan, but don't schedule another.
	 *
	 * This ends the entire top-level scan in the current scan direction.
	 *
	 * Note: The scan's arrays (including any non-required arrays) are now in
	 * their final positions for the current scan direction.  If the scan
	 * direction happens to change, then the arrays will already be in their
	 * first positions for what will then be the current scan direction.
	 */
	pstate->continuescan = false;	/* Tell _bt_readpage we're done... */
	so->needPrimScan = false;	/* ...and don't call _bt_first again */

	/* Caller's tuple doesn't match any qual */
	return false;
}

#ifdef USE_ASSERT_CHECKING
/*
 * Verify that the scan's qual state matches what we expect at the point that
 * _bt_start_prim_scan is about to start a just-scheduled new primitive scan.
 *
 * We enforce a rule against non-required array scan keys: they must start out
 * with whatever element is the first for the scan's current scan direction.
 * See _bt_rewind_nonrequired_arrays comments for an explanation.
 */
static bool
_bt_verify_arrays_bt_first(IndexScanDesc scan, ScanDirection dir)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	int			arrayidx = 0;

	for (int ikey = 0; ikey < so->numberOfKeys; ikey++)
	{
		ScanKey		cur = so->keyData + ikey;
		BTArrayKeyInfo *array = NULL;
		int			first_elem_dir;

		if (!(cur->sk_flags & SK_SEARCHARRAY) ||
			cur->sk_strategy != BTEqualStrategyNumber)
			continue;

		array = &so->arrayKeys[arrayidx++];

		if (((cur->sk_flags & SK_BT_REQFWD) && ScanDirectionIsForward(dir)) ||
			((cur->sk_flags & SK_BT_REQBKWD) && ScanDirectionIsBackward(dir)))
			continue;

		if (ScanDirectionIsForward(dir))
			first_elem_dir = 0;
		else
			first_elem_dir = array->num_elems - 1;

		if (array->cur_elem != first_elem_dir)
			return false;
	}

	return _bt_verify_keys_with_arraykeys(scan);
}

/*
 * Verify that the scan's "so->keyData[]" scan keys are in agreement with
 * its array key state
 */
static bool
_bt_verify_keys_with_arraykeys(IndexScanDesc scan)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	int			last_sk_attno = InvalidAttrNumber,
				arrayidx = 0;

	if (!so->qual_ok)
		return false;

	for (int ikey = 0; ikey < so->numberOfKeys; ikey++)
	{
		ScanKey		cur = so->keyData + ikey;
		BTArrayKeyInfo *array;

		if (cur->sk_strategy != BTEqualStrategyNumber ||
			!(cur->sk_flags & SK_SEARCHARRAY))
			continue;

		array = &so->arrayKeys[arrayidx++];
		if (array->scan_key != ikey)
			return false;

		if (array->num_elems == 0 || array->num_elems < -1)
			return false;

		if (array->num_elems != -1 &&
			cur->sk_argument != array->elem_values[array->cur_elem])
			return false;
		if (last_sk_attno > cur->sk_attno)
			return false;
		last_sk_attno = cur->sk_attno;
	}

	if (arrayidx != so->numArrayKeys)
		return false;

	return true;
}
#endif

/*
 * Test whether an indextuple satisfies all the scankey conditions.
 *
 * Return true if so, false if not.  If the tuple fails to pass the qual,
 * we also determine whether there's any need to continue the scan beyond
 * this tuple, and set pstate.continuescan accordingly.  See comments for
 * _bt_preprocess_keys() about how this is done.
 *
 * Forward scan callers can pass a high key tuple in the hopes of having
 * us set *continuescan to false, and avoiding an unnecessary visit to
 * the page to the right.
 *
 * Advances the scan's array keys when necessary for arrayKeys=true callers.
 * Scans without any array keys must always pass arrayKeys=false.
 *
 * Also stops and starts primitive index scans for arrayKeys=true callers.
 * Scans with array keys are required to set up page state that helps us with
 * this.  The page's finaltup tuple (the page high key for a forward scan, or
 * the page's first non-pivot tuple for a backward scan) must be set in
 * pstate.finaltup ahead of the first call here for the page.  Set this to
 * NULL for rightmost page (or the leftmost page for backwards scans).
 *
 * scan: index scan descriptor (containing a search-type scankey)
 * pstate: page level input and output parameters
 * arrayKeys: should we advance the scan's array keys if necessary?
 * tuple: index tuple to test
 * tupnatts: number of attributes in tupnatts (high key may be truncated)
 */
bool
_bt_checkkeys(IndexScanDesc scan, BTReadPageState *pstate, bool arrayKeys,
			  IndexTuple tuple, int tupnatts)
{
	TupleDesc	tupdesc = RelationGetDescr(scan->indexRelation);
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	ScanDirection dir = so->currPos.dir;
	int			ikey = pstate->startikey;
	bool		res;

	Assert(BTreeTupleGetNAtts(tuple, scan->indexRelation) == tupnatts);
	Assert(!so->needPrimScan && !so->scanBehind && !so->oppositeDirCheck);
	Assert(arrayKeys || so->numArrayKeys == 0);

	res = _bt_check_compare(scan, dir, tuple, tupnatts, tupdesc, arrayKeys,
							pstate->forcenonrequired, &pstate->continuescan,
							&ikey);

	/*
	 * If _bt_check_compare relied on the pstate.startikey optimization, call
	 * again (in assert-enabled builds) to verify it didn't affect our answer.
	 *
	 * Note: we can't do this when !pstate.forcenonrequired, since any arrays
	 * before pstate.startikey won't have advanced on this page at all.
	 */
	Assert(!pstate->forcenonrequired || arrayKeys);
#ifdef USE_ASSERT_CHECKING
	if (pstate->startikey > 0 && !pstate->forcenonrequired)
	{
		bool		dres,
					dcontinuescan;
		int			dikey = 0;

		/* Pass arrayKeys=false to avoid array side-effects */
		dres = _bt_check_compare(scan, dir, tuple, tupnatts, tupdesc, false,
								 pstate->forcenonrequired, &dcontinuescan,
								 &dikey);
		Assert(res == dres);
		Assert(pstate->continuescan == dcontinuescan);

		/*
		 * Should also get the same ikey result.  We need a slightly weaker
		 * assertion during arrayKeys calls, since they might be using an
		 * array that couldn't be marked required during preprocessing.
		 */
		Assert(arrayKeys || ikey == dikey);
		Assert(ikey <= dikey);
	}
#endif

	/*
	 * Only one _bt_check_compare call is required in the common case where
	 * there are no equality strategy array scan keys.  Otherwise we can only
	 * accept _bt_check_compare's answer unreservedly when it didn't set
	 * pstate.continuescan=false.
	 */
	if (!arrayKeys || pstate->continuescan)
		return res;

	/*
	 * _bt_check_compare call set continuescan=false in the presence of
	 * equality type array keys.  This could mean that the tuple is just past
	 * the end of matches for the current array keys.
	 *
	 * It's also possible that the scan is still _before_ the _start_ of
	 * tuples matching the current set of array keys.  Check for that first.
	 */
	Assert(!pstate->forcenonrequired);
	if (_bt_tuple_before_array_skeys(scan, dir, tuple, tupdesc, tupnatts, true,
									 ikey, NULL))
	{
		/* Override _bt_check_compare, continue primitive scan */
		pstate->continuescan = true;

		/*
		 * We will end up here repeatedly given a group of tuples > the
		 * previous array keys and < the now-current keys (for a backwards
		 * scan it's just the same, though the operators swap positions).
		 *
		 * We must avoid allowing this linear search process to scan very many
		 * tuples from well before the start of tuples matching the current
		 * array keys (or from well before the point where we'll once again
		 * have to advance the scan's array keys).
		 *
		 * We keep the overhead under control by speculatively "looking ahead"
		 * to later still-unscanned items from this same leaf page.  We'll
		 * only attempt this once the number of tuples that the linear search
		 * process has examined starts to get out of hand.
		 */
		pstate->rechecks++;
		if (pstate->rechecks >= LOOK_AHEAD_REQUIRED_RECHECKS)
		{
			/* See if we should skip ahead within the current leaf page */
			_bt_checkkeys_look_ahead(scan, pstate, tupnatts, tupdesc);

			/*
			 * Might have set pstate.skip to a later page offset.  When that
			 * happens then _bt_readpage caller will inexpensively skip ahead
			 * to a later tuple from the same page (the one just after the
			 * tuple we successfully "looked ahead" to).
			 */
		}

		/* This indextuple doesn't match the current qual, in any case */
		return false;
	}

	/*
	 * Caller's tuple is >= the current set of array keys and other equality
	 * constraint scan keys (or <= if this is a backwards scan).  It's now
	 * clear that we _must_ advance any required array keys in lockstep with
	 * the scan.
	 */
	return _bt_advance_array_keys(scan, pstate, tuple, tupnatts, tupdesc,
								  ikey, true);
}

/*
 * Test whether caller's finaltup tuple is still before the start of matches
 * for the current array keys.
 *
 * Called at the start of reading a page during a scan with array keys, though
 * only when the so->scanBehind flag was set on the scan's prior page.
 *
 * Returns false if the tuple is still before the start of matches.  When that
 * happens, caller should cut its losses and start a new primitive index scan.
 * Otherwise returns true.
 */
bool
_bt_scanbehind_checkkeys(IndexScanDesc scan, ScanDirection dir,
						 IndexTuple finaltup)
{
	Relation	rel = scan->indexRelation;
	TupleDesc	tupdesc = RelationGetDescr(rel);
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	int			nfinaltupatts = BTreeTupleGetNAtts(finaltup, rel);
	bool		scanBehind;

	Assert(so->numArrayKeys);

	if (_bt_tuple_before_array_skeys(scan, dir, finaltup, tupdesc,
									 nfinaltupatts, false, 0, &scanBehind))
		return false;

	/*
	 * If scanBehind was set, all of the untruncated attribute values from
	 * finaltup that correspond to an array match the array's current element,
	 * but there are other keys associated with truncated suffix attributes.
	 * Array advancement must have incremented the scan's arrays on the
	 * previous page, resulting in a set of array keys that happen to be an
	 * exact match for the current page high key's untruncated prefix values.
	 *
	 * This page definitely doesn't contain tuples that the scan will need to
	 * return.  The next page may or may not contain relevant tuples.  Handle
	 * this by cutting our losses and starting a new primscan.
	 */
	if (scanBehind)
		return false;

	if (!so->oppositeDirCheck)
		return true;

	return _bt_oppodir_checkkeys(scan, dir, finaltup);
}

/*
 * Test whether an indextuple fails to satisfy an inequality required in the
 * opposite direction only.
 *
 * Caller's finaltup tuple is the page high key (for forwards scans), or the
 * first non-pivot tuple (for backwards scans).  Called during scans with
 * required array keys and required opposite-direction inequalities.
 *
 * Returns false if an inequality scan key required in the opposite direction
 * only isn't satisfied (and any earlier required scan keys are satisfied).
 * Otherwise returns true.
 *
 * An unsatisfied inequality required in the opposite direction only might
 * well enable skipping over many leaf pages, provided another _bt_first call
 * takes place.  This type of unsatisfied inequality won't usually cause
 * _bt_checkkeys to stop the scan to consider array advancement/starting a new
 * primitive index scan.
 */
static bool
_bt_oppodir_checkkeys(IndexScanDesc scan, ScanDirection dir,
					  IndexTuple finaltup)
{
	Relation	rel = scan->indexRelation;
	TupleDesc	tupdesc = RelationGetDescr(rel);
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	int			nfinaltupatts = BTreeTupleGetNAtts(finaltup, rel);
	bool		continuescan;
	ScanDirection flipped = -dir;
	int			ikey = 0;

	Assert(so->numArrayKeys);

	_bt_check_compare(scan, flipped, finaltup, nfinaltupatts, tupdesc, false,
					  false, &continuescan,
					  &ikey);

	if (!continuescan && so->keyData[ikey].sk_strategy != BTEqualStrategyNumber)
		return false;

	return true;
}

/*
 * Determines an offset to the first scan key (an so->keyData[]-wise offset)
 * that is _not_ guaranteed to be satisfied by every tuple from pstate.page,
 * which is set in pstate.startikey for _bt_checkkeys calls for the page.
 * This allows caller to save cycles on comparisons of a prefix of keys while
 * reading pstate.page.
 *
 * Also determines if later calls to _bt_checkkeys (for pstate.page) should be
 * forced to treat all required scan keys >= pstate.startikey as nonrequired
 * (that is, if they're to be treated as if any SK_BT_REQFWD/SK_BT_REQBKWD
 * markings that were set by preprocessing were not set at all, for the
 * duration of _bt_checkkeys calls prior to the call for pstate.finaltup).
 * This is indicated to caller by setting pstate.forcenonrequired.
 *
 * Call here at the start of reading a leaf page beyond the first one for the
 * primitive index scan.  We consider all non-pivot tuples, so it doesn't make
 * sense to call here when only a subset of those tuples can ever be read.
 * This is also a good idea on performance grounds; not calling here when on
 * the first page (first for the current primitive scan) avoids wasting cycles
 * during selective point queries.  They typically don't stand to gain as much
 * when we can set pstate.startikey, and are likely to notice the overhead of
 * calling here.  (Also, allowing pstate.forcenonrequired to be set on a
 * primscan's first page would mislead _bt_advance_array_keys, which expects
 * pstate.nskipadvances to be representative of every first page's key space.)
 *
 * Caller must call _bt_start_array_keys and reset startikey/forcenonrequired
 * ahead of the finaltup _bt_checkkeys call when we set forcenonrequired=true.
 * This will give _bt_checkkeys the opportunity to call _bt_advance_array_keys
 * with sktrig_required=true, restoring the invariant that the scan's required
 * arrays always track the scan's progress through the index's key space.
 * Caller won't need to do this on the rightmost/leftmost page in the index
 * (where pstate.finaltup isn't ever set), since forcenonrequired will never
 * be set here in the first place.
 */
void
_bt_set_startikey(IndexScanDesc scan, BTReadPageState *pstate)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	Relation	rel = scan->indexRelation;
	TupleDesc	tupdesc = RelationGetDescr(rel);
	ItemId		iid;
	IndexTuple	firsttup,
				lasttup;
	int			startikey = 0,
				arrayidx = 0,
				firstchangingattnum;
	bool		start_past_saop_eq = false;

	Assert(!so->scanBehind);
	Assert(pstate->minoff < pstate->maxoff);
	Assert(!pstate->firstpage);
	Assert(pstate->startikey == 0);
	Assert(!so->numArrayKeys || pstate->finaltup ||
		   P_RIGHTMOST(BTPageGetOpaque(pstate->page)) ||
		   P_LEFTMOST(BTPageGetOpaque(pstate->page)));

	if (so->numberOfKeys == 0)
		return;

	/* minoff is an offset to the lowest non-pivot tuple on the page */
	iid = PageGetItemId(pstate->page, pstate->minoff);
	firsttup = (IndexTuple) PageGetItem(pstate->page, iid);

	/* maxoff is an offset to the highest non-pivot tuple on the page */
	iid = PageGetItemId(pstate->page, pstate->maxoff);
	lasttup = (IndexTuple) PageGetItem(pstate->page, iid);

	/* Determine the first attribute whose values change on caller's page */
	firstchangingattnum = _bt_keep_natts_fast(rel, firsttup, lasttup);

	for (; startikey < so->numberOfKeys; startikey++)
	{
		ScanKey		key = so->keyData + startikey;
		BTArrayKeyInfo *array;
		Datum		firstdatum,
					lastdatum;
		bool		firstnull,
					lastnull;
		int32		result;

		/*
		 * Determine if it's safe to set pstate.startikey to an offset to a
		 * key that comes after this key, by examining this key
		 */
		if (!(key->sk_flags & (SK_BT_REQFWD | SK_BT_REQBKWD)))
		{
			/* Scan key isn't marked required (corner case) */
			Assert(!(key->sk_flags & SK_ROW_HEADER));
			break;				/* unsafe */
		}
		if (key->sk_flags & SK_ROW_HEADER)
		{
			/*
			 * RowCompare inequality.
			 *
			 * Only the first subkey from a RowCompare can ever be marked
			 * required (that happens when the row header is marked required).
			 * There is no simple, general way for us to transitively deduce
			 * whether or not every tuple on the page satisfies a RowCompare
			 * key based only on firsttup and lasttup -- so we just give up.
			 */
			if (!start_past_saop_eq && !so->skipScan)
				break;			/* unsafe to go further */

			/*
			 * We have to be even more careful with RowCompares that come
			 * after an array: we assume it's unsafe to even bypass the array.
			 * Calling _bt_start_array_keys to recover the scan's arrays
			 * following use of forcenonrequired mode isn't compatible with
			 * _bt_check_rowcompare's continuescan=false behavior with NULL
			 * row compare members.  _bt_advance_array_keys must not make a
			 * decision on the basis of a key not being satisfied in the
			 * opposite-to-scan direction until the scan reaches a leaf page
			 * where the same key begins to be satisfied in scan direction.
			 * The _bt_first !used_all_subkeys behavior makes this limitation
			 * hard to work around some other way.
			 */
			return;				/* completely unsafe to set pstate.startikey */
		}
		if (key->sk_strategy != BTEqualStrategyNumber)
		{
			/*
			 * Scalar inequality key.
			 *
			 * It's definitely safe for _bt_checkkeys to avoid assessing this
			 * inequality when the page's first and last non-pivot tuples both
			 * satisfy the inequality (since the same must also be true of all
			 * the tuples in between these two).
			 *
			 * Unlike the "=" case, it doesn't matter if this attribute has
			 * more than one distinct value (though it _is_ necessary for any
			 * and all _prior_ attributes to contain no more than one distinct
			 * value amongst all of the tuples from pstate.page).
			 */
			if (key->sk_attno > firstchangingattnum)	/* >, not >= */
				break;			/* unsafe, preceding attr has multiple
								 * distinct values */

			firstdatum = index_getattr(firsttup, key->sk_attno, tupdesc, &firstnull);
			lastdatum = index_getattr(lasttup, key->sk_attno, tupdesc, &lastnull);

			if (key->sk_flags & SK_ISNULL)
			{
				/* IS NOT NULL key */
				Assert(key->sk_flags & SK_SEARCHNOTNULL);

				if (firstnull || lastnull)
					break;		/* unsafe */

				/* Safe, IS NOT NULL key satisfied by every tuple */
				continue;
			}

			/* Test firsttup */
			if (firstnull ||
				!DatumGetBool(FunctionCall2Coll(&key->sk_func,
												key->sk_collation, firstdatum,
												key->sk_argument)))
				break;			/* unsafe */

			/* Test lasttup */
			if (lastnull ||
				!DatumGetBool(FunctionCall2Coll(&key->sk_func,
												key->sk_collation, lastdatum,
												key->sk_argument)))
				break;			/* unsafe */

			/* Safe, scalar inequality satisfied by every tuple */
			continue;
		}

		/* Some = key (could be a scalar = key, could be an array = key) */
		Assert(key->sk_strategy == BTEqualStrategyNumber);

		if (!(key->sk_flags & SK_SEARCHARRAY))
		{
			/*
			 * Scalar = key (possibly an IS NULL key).
			 *
			 * It is unsafe to set pstate.startikey to an ikey beyond this
			 * key, unless the = key is satisfied by every possible tuple on
			 * the page (possible only when attribute has just one distinct
			 * value among all tuples on the page).
			 */
			if (key->sk_attno >= firstchangingattnum)
				break;			/* unsafe, multiple distinct attr values */

			firstdatum = index_getattr(firsttup, key->sk_attno, tupdesc,
									   &firstnull);
			if (key->sk_flags & SK_ISNULL)
			{
				/* IS NULL key */
				Assert(key->sk_flags & SK_SEARCHNULL);

				if (!firstnull)
					break;		/* unsafe */

				/* Safe, IS NULL key satisfied by every tuple */
				continue;
			}
			if (firstnull ||
				!DatumGetBool(FunctionCall2Coll(&key->sk_func,
												key->sk_collation, firstdatum,
												key->sk_argument)))
				break;			/* unsafe */

			/* Safe, scalar = key satisfied by every tuple */
			continue;
		}

		/* = array key (could be a SAOP array, could be a skip array) */
		array = &so->arrayKeys[arrayidx++];
		Assert(array->scan_key == startikey);
		if (array->num_elems != -1)
		{
			/*
			 * SAOP array = key.
			 *
			 * Handle this like we handle scalar = keys (though binary search
			 * for a matching element, to avoid relying on key's sk_argument).
			 */
			if (key->sk_attno >= firstchangingattnum)
				break;			/* unsafe, multiple distinct attr values */

			firstdatum = index_getattr(firsttup, key->sk_attno, tupdesc,
									   &firstnull);
			_bt_binsrch_array_skey(&so->orderProcs[startikey],
								   false, NoMovementScanDirection,
								   firstdatum, firstnull, array, key,
								   &result);
			if (result != 0)
				break;			/* unsafe */

			/* Safe, SAOP = key satisfied by every tuple */
			start_past_saop_eq = true;
			continue;
		}

		/*
		 * Skip array = key
		 */
		Assert(key->sk_flags & SK_BT_SKIP);
		if (array->null_elem)
		{
			/*
			 * Non-range skip array = key.
			 *
			 * Safe, non-range skip array "satisfied" by every tuple on page
			 * (safe even when "key->sk_attno > firstchangingattnum").
			 */
			continue;
		}

		/*
		 * Range skip array = key.
		 *
		 * Handle this like we handle scalar inequality keys (but avoid using
		 * key's sk_argument directly, as in the SAOP array case).
		 */
		if (key->sk_attno > firstchangingattnum)	/* >, not >= */
			break;				/* unsafe, preceding attr has multiple
								 * distinct values */

		firstdatum = index_getattr(firsttup, key->sk_attno, tupdesc, &firstnull);
		lastdatum = index_getattr(lasttup, key->sk_attno, tupdesc, &lastnull);

		/* Test firsttup */
		_bt_binsrch_skiparray_skey(false, ForwardScanDirection,
								   firstdatum, firstnull, array, key,
								   &result);
		if (result != 0)
			break;				/* unsafe */

		/* Test lasttup */
		_bt_binsrch_skiparray_skey(false, ForwardScanDirection,
								   lastdatum, lastnull, array, key,
								   &result);
		if (result != 0)
			break;				/* unsafe */

		/* Safe, range skip array satisfied by every tuple on page */
	}

	/*
	 * Use of forcenonrequired is typically undesirable, since it'll force
	 * _bt_readpage caller to read every tuple on the page -- even though, in
	 * general, it might well be possible to end the scan on an earlier tuple.
	 * However, caller must use forcenonrequired when start_past_saop_eq=true,
	 * since the usual required array behavior might fail to roll over to the
	 * SAOP array.
	 *
	 * We always prefer forcenonrequired=true during scans with skip arrays
	 * (except on the first page of each primitive index scan), though -- even
	 * when "startikey == 0".  That way, _bt_advance_array_keys's low-order
	 * key precheck optimization can always be used (unless on the first page
	 * of the scan).  It seems slightly preferable to check more tuples when
	 * that allows us to do significantly less skip array maintenance.
	 */
	pstate->forcenonrequired = (start_past_saop_eq || so->skipScan);
	pstate->startikey = startikey;

	/*
	 * _bt_readpage caller is required to call _bt_checkkeys against page's
	 * finaltup with forcenonrequired=false whenever we initially set
	 * forcenonrequired=true.  That way the scan's arrays will reliably track
	 * its progress through the index's key space.
	 *
	 * We don't expect this when _bt_readpage caller has no finaltup due to
	 * its page being the rightmost (or the leftmost, during backwards scans).
	 * When we see that _bt_readpage has no finaltup, back out of everything.
	 */
	Assert(!pstate->forcenonrequired || so->numArrayKeys);
	if (pstate->forcenonrequired && !pstate->finaltup)
	{
		pstate->forcenonrequired = false;
		pstate->startikey = 0;
	}
}

/*
 * Test whether an indextuple satisfies current scan condition.
 *
 * Return true if so, false if not.  If not, also sets *continuescan to false
 * when it's also not possible for any later tuples to pass the current qual
 * (with the scan's current set of array keys, in the current scan direction),
 * in addition to setting *ikey to the so->keyData[] subscript/offset for the
 * unsatisfied scan key (needed when caller must consider advancing the scan's
 * array keys).
 *
 * This is a subroutine for _bt_checkkeys.  We provisionally assume that
 * reaching the end of the current set of required keys (in particular the
 * current required array keys) ends the ongoing (primitive) index scan.
 * Callers without array keys should just end the scan right away when they
 * find that continuescan has been set to false here by us.  Things are more
 * complicated for callers with array keys.
 *
 * Callers with array keys must first consider advancing the arrays when
 * continuescan has been set to false here by us.  They must then consider if
 * it really does make sense to end the current (primitive) index scan, in
 * light of everything that is known at that point.  (In general when we set
 * continuescan=false for these callers it must be treated as provisional.)
 *
 * We deal with advancing unsatisfied non-required arrays directly, though.
 * This is safe, since by definition non-required keys can't end the scan.
 * This is just how we determine if non-required arrays are just unsatisfied
 * by the current array key, or if they're truly unsatisfied (that is, if
 * they're unsatisfied by every possible array key).
 *
 * Pass advancenonrequired=false to avoid all array related side effects.
 * This allows _bt_advance_array_keys caller to avoid infinite recursion.
 *
 * Pass forcenonrequired=true to instruct us to treat all keys as nonrequired.
 * This is used to make it safe to temporarily stop properly maintaining the
 * scan's required arrays.  _bt_checkkeys caller (_bt_readpage, actually)
 * determines a prefix of keys that must satisfy every possible corresponding
 * index attribute value from its page, which is passed to us via *ikey arg
 * (this is the first key that might be unsatisfied by tuples on the page).
 * Obviously, we won't maintain any array keys from before *ikey, so it's
 * quite possible for such arrays to "fall behind" the index's keyspace.
 * Caller will need to "catch up" by passing forcenonrequired=true (alongside
 * an *ikey=0) once the page's finaltup is reached.
 *
 * Note: it's safe to pass an *ikey > 0 with forcenonrequired=false, but only
 * when caller determines that it won't affect array maintenance.
 */
static bool
_bt_check_compare(IndexScanDesc scan, ScanDirection dir,
				  IndexTuple tuple, int tupnatts, TupleDesc tupdesc,
				  bool advancenonrequired, bool forcenonrequired,
				  bool *continuescan, int *ikey)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;

	*continuescan = true;		/* default assumption */

	for (; *ikey < so->numberOfKeys; (*ikey)++)
	{
		ScanKey		key = so->keyData + *ikey;
		Datum		datum;
		bool		isNull;
		bool		requiredSameDir = false,
					requiredOppositeDirOnly = false;

		/*
		 * Check if the key is required in the current scan direction, in the
		 * opposite scan direction _only_, or in neither direction (except
		 * when we're forced to treat all scan keys as nonrequired)
		 */
		if (forcenonrequired)
		{
			/* treating scan's keys as non-required */
		}
		else if (((key->sk_flags & SK_BT_REQFWD) && ScanDirectionIsForward(dir)) ||
				 ((key->sk_flags & SK_BT_REQBKWD) && ScanDirectionIsBackward(dir)))
			requiredSameDir = true;
		else if (((key->sk_flags & SK_BT_REQFWD) && ScanDirectionIsBackward(dir)) ||
				 ((key->sk_flags & SK_BT_REQBKWD) && ScanDirectionIsForward(dir)))
			requiredOppositeDirOnly = true;

		if (key->sk_attno > tupnatts)
		{
			/*
			 * This attribute is truncated (must be high key).  The value for
			 * this attribute in the first non-pivot tuple on the page to the
			 * right could be any possible value.  Assume that truncated
			 * attribute passes the qual.
			 */
			Assert(BTreeTupleIsPivot(tuple));
			continue;
		}

		/*
		 * A skip array scan key uses one of several sentinel values.  We just
		 * fall back on _bt_tuple_before_array_skeys when we see such a value.
		 */
		if (key->sk_flags & (SK_BT_MINVAL | SK_BT_MAXVAL |
							 SK_BT_NEXT | SK_BT_PRIOR))
		{
			Assert(key->sk_flags & SK_SEARCHARRAY);
			Assert(key->sk_flags & SK_BT_SKIP);
			Assert(requiredSameDir || forcenonrequired);

			/*
			 * Cannot fall back on _bt_tuple_before_array_skeys when we're
			 * treating the scan's keys as nonrequired, though.  Just handle
			 * this like any other non-required equality-type array key.
			 */
			if (forcenonrequired)
				return _bt_advance_array_keys(scan, NULL, tuple, tupnatts,
											  tupdesc, *ikey, false);

			*continuescan = false;
			return false;
		}

		/* row-comparison keys need special processing */
		if (key->sk_flags & SK_ROW_HEADER)
		{
			Assert(!forcenonrequired);	/* forbidden by _bt_set_startikey */

			if (_bt_check_rowcompare(key, tuple, tupnatts, tupdesc, dir,
									 continuescan))
				continue;
			return false;
		}

		datum = index_getattr(tuple,
							  key->sk_attno,
							  tupdesc,
							  &isNull);

		if (key->sk_flags & SK_ISNULL)
		{
			/* Handle IS NULL/NOT NULL tests */
			if (key->sk_flags & SK_SEARCHNULL)
			{
				if (isNull)
					continue;	/* tuple satisfies this qual */
			}
			else
			{
				Assert(key->sk_flags & SK_SEARCHNOTNULL);
				Assert(!(key->sk_flags & SK_BT_SKIP));
				if (!isNull)
					continue;	/* tuple satisfies this qual */
			}

			/*
			 * Tuple fails this qual.  If it's a required qual for the current
			 * scan direction, then we can conclude no further tuples will
			 * pass, either.
			 */
			if (requiredSameDir)
				*continuescan = false;
			else if (unlikely(key->sk_flags & SK_BT_SKIP))
			{
				/*
				 * If we're treating scan keys as nonrequired, and encounter a
				 * skip array scan key whose current element is NULL, then it
				 * must be a non-range skip array.  It must be satisfied, so
				 * there's no need to call _bt_advance_array_keys to check.
				 */
				Assert(forcenonrequired && *ikey > 0);
				continue;
			}

			/*
			 * This indextuple doesn't match the qual.
			 */
			return false;
		}

		if (isNull)
		{
			/*
			 * Scalar scan key isn't satisfied by NULL tuple value.
			 *
			 * If we're treating scan keys as nonrequired, and key is for a
			 * skip array, then we must attempt to advance the array to NULL
			 * (if we're successful then the tuple might match the qual).
			 */
			if (unlikely(forcenonrequired && key->sk_flags & SK_BT_SKIP))
				return _bt_advance_array_keys(scan, NULL, tuple, tupnatts,
											  tupdesc, *ikey, false);

			if (key->sk_flags & SK_BT_NULLS_FIRST)
			{
				/*
				 * Since NULLs are sorted before non-NULLs, we know we have
				 * reached the lower limit of the range of values for this
				 * index attr.  On a backward scan, we can stop if this qual
				 * is one of the "must match" subset.  We can stop regardless
				 * of whether the qual is > or <, so long as it's required,
				 * because it's not possible for any future tuples to pass. On
				 * a forward scan, however, we must keep going, because we may
				 * have initially positioned to the start of the index.
				 * (_bt_advance_array_keys also relies on this behavior during
				 * forward scans.)
				 */
				if ((requiredSameDir || requiredOppositeDirOnly) &&
					ScanDirectionIsBackward(dir))
					*continuescan = false;
			}
			else
			{
				/*
				 * Since NULLs are sorted after non-NULLs, we know we have
				 * reached the upper limit of the range of values for this
				 * index attr.  On a forward scan, we can stop if this qual is
				 * one of the "must match" subset.  We can stop regardless of
				 * whether the qual is > or <, so long as it's required,
				 * because it's not possible for any future tuples to pass. On
				 * a backward scan, however, we must keep going, because we
				 * may have initially positioned to the end of the index.
				 * (_bt_advance_array_keys also relies on this behavior during
				 * backward scans.)
				 */
				if ((requiredSameDir || requiredOppositeDirOnly) &&
					ScanDirectionIsForward(dir))
					*continuescan = false;
			}

			/*
			 * This indextuple doesn't match the qual.
			 */
			return false;
		}

		if (!DatumGetBool(FunctionCall2Coll(&key->sk_func, key->sk_collation,
											datum, key->sk_argument)))
		{
			/*
			 * Tuple fails this qual.  If it's a required qual for the current
			 * scan direction, then we can conclude no further tuples will
			 * pass, either.
			 *
			 * Note: because we stop the scan as soon as any required equality
			 * qual fails, it is critical that equality quals be used for the
			 * initial positioning in _bt_first() when they are available. See
			 * comments in _bt_first().
			 */
			if (requiredSameDir)
				*continuescan = false;

			/*
			 * If this is a non-required equality-type array key, the tuple
			 * needs to be checked against every possible array key.  Handle
			 * this by "advancing" the scan key's array to a matching value
			 * (if we're successful then the tuple might match the qual).
			 */
			else if (advancenonrequired &&
					 key->sk_strategy == BTEqualStrategyNumber &&
					 (key->sk_flags & SK_SEARCHARRAY))
				return _bt_advance_array_keys(scan, NULL, tuple, tupnatts,
											  tupdesc, *ikey, false);

			/*
			 * This indextuple doesn't match the qual.
			 */
			return false;
		}
	}

	/* If we get here, the tuple passes all index quals. */
	return true;
}

/*
 * Test whether an indextuple satisfies a row-comparison scan condition.
 *
 * Return true if so, false if not.  If not, also clear *continuescan if
 * it's not possible for any future tuples in the current scan direction
 * to pass the qual.
 *
 * This is a subroutine for _bt_checkkeys/_bt_check_compare.
 */
static bool
_bt_check_rowcompare(ScanKey skey, IndexTuple tuple, int tupnatts,
					 TupleDesc tupdesc, ScanDirection dir, bool *continuescan)
{
	ScanKey		subkey = (ScanKey) DatumGetPointer(skey->sk_argument);
	int32		cmpresult = 0;
	bool		result;

	/* First subkey should be same as the header says */
	Assert(subkey->sk_attno == skey->sk_attno);

	/* Loop over columns of the row condition */
	for (;;)
	{
		Datum		datum;
		bool		isNull;

		Assert(subkey->sk_flags & SK_ROW_MEMBER);

		if (subkey->sk_attno > tupnatts)
		{
			/*
			 * This attribute is truncated (must be high key).  The value for
			 * this attribute in the first non-pivot tuple on the page to the
			 * right could be any possible value.  Assume that truncated
			 * attribute passes the qual.
			 */
			Assert(BTreeTupleIsPivot(tuple));
			cmpresult = 0;
			if (subkey->sk_flags & SK_ROW_END)
				break;
			subkey++;
			continue;
		}

		datum = index_getattr(tuple,
							  subkey->sk_attno,
							  tupdesc,
							  &isNull);

		if (isNull)
		{
			if (subkey->sk_flags & SK_BT_NULLS_FIRST)
			{
				/*
				 * Since NULLs are sorted before non-NULLs, we know we have
				 * reached the lower limit of the range of values for this
				 * index attr.  On a backward scan, we can stop if this qual
				 * is one of the "must match" subset.  We can stop regardless
				 * of whether the qual is > or <, so long as it's required,
				 * because it's not possible for any future tuples to pass. On
				 * a forward scan, however, we must keep going, because we may
				 * have initially positioned to the start of the index.
				 * (_bt_advance_array_keys also relies on this behavior during
				 * forward scans.)
				 */
				if ((subkey->sk_flags & (SK_BT_REQFWD | SK_BT_REQBKWD)) &&
					ScanDirectionIsBackward(dir))
					*continuescan = false;
			}
			else
			{
				/*
				 * Since NULLs are sorted after non-NULLs, we know we have
				 * reached the upper limit of the range of values for this
				 * index attr.  On a forward scan, we can stop if this qual is
				 * one of the "must match" subset.  We can stop regardless of
				 * whether the qual is > or <, so long as it's required,
				 * because it's not possible for any future tuples to pass. On
				 * a backward scan, however, we must keep going, because we
				 * may have initially positioned to the end of the index.
				 * (_bt_advance_array_keys also relies on this behavior during
				 * backward scans.)
				 */
				if ((subkey->sk_flags & (SK_BT_REQFWD | SK_BT_REQBKWD)) &&
					ScanDirectionIsForward(dir))
					*continuescan = false;
			}

			/*
			 * In any case, this indextuple doesn't match the qual.
			 */
			return false;
		}

		if (subkey->sk_flags & SK_ISNULL)
		{
			/*
			 * Unlike the simple-scankey case, this isn't a disallowed case
			 * (except when it's the first row element that has the NULL arg).
			 * But it can never match.  If all the earlier row comparison
			 * columns are required for the scan direction, we can stop the
			 * scan, because there can't be another tuple that will succeed.
			 */
			Assert(subkey != (ScanKey) DatumGetPointer(skey->sk_argument));
			subkey--;
			if ((subkey->sk_flags & SK_BT_REQFWD) &&
				ScanDirectionIsForward(dir))
				*continuescan = false;
			else if ((subkey->sk_flags & SK_BT_REQBKWD) &&
					 ScanDirectionIsBackward(dir))
				*continuescan = false;
			return false;
		}

		/* Perform the test --- three-way comparison not bool operator */
		cmpresult = DatumGetInt32(FunctionCall2Coll(&subkey->sk_func,
													subkey->sk_collation,
													datum,
													subkey->sk_argument));

		if (subkey->sk_flags & SK_BT_DESC)
			INVERT_COMPARE_RESULT(cmpresult);

		/* Done comparing if unequal, else advance to next column */
		if (cmpresult != 0)
			break;

		if (subkey->sk_flags & SK_ROW_END)
			break;
		subkey++;
	}

	/*
	 * At this point cmpresult indicates the overall result of the row
	 * comparison, and subkey points to the deciding column (or the last
	 * column if the result is "=").
	 */
	switch (subkey->sk_strategy)
	{
			/* EQ and NE cases aren't allowed here */
		case BTLessStrategyNumber:
			result = (cmpresult < 0);
			break;
		case BTLessEqualStrategyNumber:
			result = (cmpresult <= 0);
			break;
		case BTGreaterEqualStrategyNumber:
			result = (cmpresult >= 0);
			break;
		case BTGreaterStrategyNumber:
			result = (cmpresult > 0);
			break;
		default:
			elog(ERROR, "unexpected strategy number %d", subkey->sk_strategy);
			result = 0;			/* keep compiler quiet */
			break;
	}

	if (!result)
	{
		/*
		 * Tuple fails this qual.  If it's a required qual for the current
		 * scan direction, then we can conclude no further tuples will pass,
		 * either.  Note we have to look at the deciding column, not
		 * necessarily the first or last column of the row condition.
		 */
		if ((subkey->sk_flags & SK_BT_REQFWD) &&
			ScanDirectionIsForward(dir))
			*continuescan = false;
		else if ((subkey->sk_flags & SK_BT_REQBKWD) &&
				 ScanDirectionIsBackward(dir))
			*continuescan = false;
	}

	return result;
}

/*
 * Determine if a scan with array keys should skip over uninteresting tuples.
 *
 * This is a subroutine for _bt_checkkeys.  Called when _bt_readpage's linear
 * search process (started after it finishes reading an initial group of
 * matching tuples, used to locate the start of the next group of tuples
 * matching the next set of required array keys) has already scanned an
 * excessive number of tuples whose key space is "between arrays".
 *
 * When we perform look ahead successfully, we'll sets pstate.skip, which
 * instructs _bt_readpage to skip ahead to that tuple next (could be past the
 * end of the scan's leaf page).  Pages where the optimization is effective
 * will generally still need to skip several times.  Each call here performs
 * only a single "look ahead" comparison of a later tuple, whose distance from
 * the current tuple's offset number is determined by applying heuristics.
 */
static void
_bt_checkkeys_look_ahead(IndexScanDesc scan, BTReadPageState *pstate,
						 int tupnatts, TupleDesc tupdesc)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	ScanDirection dir = so->currPos.dir;
	OffsetNumber aheadoffnum;
	IndexTuple	ahead;

	Assert(!pstate->forcenonrequired);

	/* Avoid looking ahead when comparing the page high key */
	if (pstate->offnum < pstate->minoff)
		return;

	/*
	 * Don't look ahead when there aren't enough tuples remaining on the page
	 * (in the current scan direction) for it to be worth our while
	 */
	if (ScanDirectionIsForward(dir) &&
		pstate->offnum >= pstate->maxoff - LOOK_AHEAD_DEFAULT_DISTANCE)
		return;
	else if (ScanDirectionIsBackward(dir) &&
			 pstate->offnum <= pstate->minoff + LOOK_AHEAD_DEFAULT_DISTANCE)
		return;

	/*
	 * The look ahead distance starts small, and ramps up as each call here
	 * allows _bt_readpage to skip over more tuples
	 */
	if (!pstate->targetdistance)
		pstate->targetdistance = LOOK_AHEAD_DEFAULT_DISTANCE;
	else if (pstate->targetdistance < MaxIndexTuplesPerPage / 2)
		pstate->targetdistance *= 2;

	/* Don't read past the end (or before the start) of the page, though */
	if (ScanDirectionIsForward(dir))
		aheadoffnum = Min((int) pstate->maxoff,
						  (int) pstate->offnum + pstate->targetdistance);
	else
		aheadoffnum = Max((int) pstate->minoff,
						  (int) pstate->offnum - pstate->targetdistance);

	ahead = (IndexTuple) PageGetItem(pstate->page,
									 PageGetItemId(pstate->page, aheadoffnum));
	if (_bt_tuple_before_array_skeys(scan, dir, ahead, tupdesc, tupnatts,
									 false, 0, NULL))
	{
		/*
		 * Success -- instruct _bt_readpage to skip ahead to very next tuple
		 * after the one we determined was still before the current array keys
		 */
		if (ScanDirectionIsForward(dir))
			pstate->skip = aheadoffnum + 1;
		else
			pstate->skip = aheadoffnum - 1;
	}
	else
	{
		/*
		 * Failure -- "ahead" tuple is too far ahead (we were too aggressive).
		 *
		 * Reset the number of rechecks, and aggressively reduce the target
		 * distance (we're much more aggressive here than we were when the
		 * distance was initially ramped up).
		 */
		pstate->rechecks = 0;
		pstate->targetdistance = Max(pstate->targetdistance / 8, 1);
	}
}

/*
 * _bt_killitems - set LP_DEAD state for items an indexscan caller has
 * told us were killed
 *
 * scan->opaque, referenced locally through so, contains information about the
 * current page and killed tuples thereon (generally, this should only be
 * called if so->numKilled > 0).
 *
 * The caller does not have a lock on the page and may or may not have the
 * page pinned in a buffer.  Note that read-lock is sufficient for setting
 * LP_DEAD status (which is only a hint).
 *
 * We match items by heap TID before assuming they are the right ones to
 * delete.  We cope with cases where items have moved right due to insertions.
 * If an item has moved off the current page due to a split, we'll fail to
 * find it and do nothing (this is not an error case --- we assume the item
 * will eventually get marked in a future indexscan).
 *
 * Note that if we hold a pin on the target page continuously from initially
 * reading the items until applying this function, VACUUM cannot have deleted
 * any items on the page, so the page's TIDs can't have been recycled by now.
 * There's no risk that we'll confuse a new index tuple that happens to use a
 * recycled TID with a now-removed tuple with the same TID (that used to be on
 * this same page).  We can't rely on that during scans that drop pins eagerly
 * (so->dropPin scans), though, so we must condition setting LP_DEAD bits on
 * the page LSN having not changed since back when _bt_readpage saw the page.
 */
void
_bt_killitems(IndexScanDesc scan)
{
	Relation	rel = scan->indexRelation;
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	Page		page;
	BTPageOpaque opaque;
	OffsetNumber minoff;
	OffsetNumber maxoff;
	int			numKilled = so->numKilled;
	bool		killedsomething = false;

	Assert(numKilled > 0);
	Assert(BTScanPosIsValid(so->currPos));
	Assert(scan->heapRelation != NULL); /* can't be a bitmap index scan */

	/* Always invalidate so->killedItems[] before leaving so->currPos */
	so->numKilled = 0;

	if (!so->dropPin)
	{
		/*
		 * We have held the pin on this page since we read the index tuples,
		 * so all we need to do is lock it.  The pin will have prevented
		 * concurrent VACUUMs from recycling any of the TIDs on the page.
		 */
		Assert(BTScanPosIsPinned(so->currPos));
		_bt_lockbuf(rel, so->currPos.buf, BT_READ);
	}
	else
	{
		Buffer		buf;
		XLogRecPtr	latestlsn;

		Assert(!BTScanPosIsPinned(so->currPos));
		Assert(RelationNeedsWAL(rel));
		buf = _bt_getbuf(rel, so->currPos.currPage, BT_READ);

		latestlsn = BufferGetLSNAtomic(buf);
		Assert(!XLogRecPtrIsInvalid(so->currPos.lsn));
		Assert(so->currPos.lsn <= latestlsn);
		if (so->currPos.lsn != latestlsn)
		{
			/* Modified, give up on hinting */
			_bt_relbuf(rel, buf);
			return;
		}

		/* Unmodified, hinting is safe */
		so->currPos.buf = buf;
	}

	page = BufferGetPage(so->currPos.buf);
	opaque = BTPageGetOpaque(page);
	minoff = P_FIRSTDATAKEY(opaque);
	maxoff = PageGetMaxOffsetNumber(page);

	for (int i = 0; i < numKilled; i++)
	{
		int			itemIndex = so->killedItems[i];
		BTScanPosItem *kitem = &so->currPos.items[itemIndex];
		OffsetNumber offnum = kitem->indexOffset;

		Assert(itemIndex >= so->currPos.firstItem &&
			   itemIndex <= so->currPos.lastItem);
		if (offnum < minoff)
			continue;			/* pure paranoia */
		while (offnum <= maxoff)
		{
			ItemId		iid = PageGetItemId(page, offnum);
			IndexTuple	ituple = (IndexTuple) PageGetItem(page, iid);
			bool		killtuple = false;

			if (BTreeTupleIsPosting(ituple))
			{
				int			pi = i + 1;
				int			nposting = BTreeTupleGetNPosting(ituple);
				int			j;

				/*
				 * We rely on the convention that heap TIDs in the scanpos
				 * items array are stored in ascending heap TID order for a
				 * group of TIDs that originally came from a posting list
				 * tuple.  This convention even applies during backwards
				 * scans, where returning the TIDs in descending order might
				 * seem more natural.  This is about effectiveness, not
				 * correctness.
				 *
				 * Note that the page may have been modified in almost any way
				 * since we first read it (in the !so->dropPin case), so it's
				 * possible that this posting list tuple wasn't a posting list
				 * tuple when we first encountered its heap TIDs.
				 */
				for (j = 0; j < nposting; j++)
				{
					ItemPointer item = BTreeTupleGetPostingN(ituple, j);

					if (!ItemPointerEquals(item, &kitem->heapTid))
						break;	/* out of posting list loop */

					/*
					 * kitem must have matching offnum when heap TIDs match,
					 * though only in the common case where the page can't
					 * have been concurrently modified
					 */
					Assert(kitem->indexOffset == offnum || !so->dropPin);

					/*
					 * Read-ahead to later kitems here.
					 *
					 * We rely on the assumption that not advancing kitem here
					 * will prevent us from considering the posting list tuple
					 * fully dead by not matching its next heap TID in next
					 * loop iteration.
					 *
					 * If, on the other hand, this is the final heap TID in
					 * the posting list tuple, then tuple gets killed
					 * regardless (i.e. we handle the case where the last
					 * kitem is also the last heap TID in the last index tuple
					 * correctly -- posting tuple still gets killed).
					 */
					if (pi < numKilled)
						kitem = &so->currPos.items[so->killedItems[pi++]];
				}

				/*
				 * Don't bother advancing the outermost loop's int iterator to
				 * avoid processing killed items that relate to the same
				 * offnum/posting list tuple.  This micro-optimization hardly
				 * seems worth it.  (Further iterations of the outermost loop
				 * will fail to match on this same posting list's first heap
				 * TID instead, so we'll advance to the next offnum/index
				 * tuple pretty quickly.)
				 */
				if (j == nposting)
					killtuple = true;
			}
			else if (ItemPointerEquals(&ituple->t_tid, &kitem->heapTid))
				killtuple = true;

			/*
			 * Mark index item as dead, if it isn't already.  Since this
			 * happens while holding a buffer lock possibly in shared mode,
			 * it's possible that multiple processes attempt to do this
			 * simultaneously, leading to multiple full-page images being sent
			 * to WAL (if wal_log_hints or data checksums are enabled), which
			 * is undesirable.
			 */
			if (killtuple && !ItemIdIsDead(iid))
			{
				/* found the item/all posting list items */
				ItemIdMarkDead(iid);
				killedsomething = true;
				break;			/* out of inner search loop */
			}
			offnum = OffsetNumberNext(offnum);
		}
	}

	/*
	 * Since this can be redone later if needed, mark as dirty hint.
	 *
	 * Whenever we mark anything LP_DEAD, we also set the page's
	 * BTP_HAS_GARBAGE flag, which is likewise just a hint.  (Note that we
	 * only rely on the page-level flag in !heapkeyspace indexes.)
	 */
	if (killedsomething)
	{
		opaque->btpo_flags |= BTP_HAS_GARBAGE;
		MarkBufferDirtyHint(so->currPos.buf, true);
	}

	_bt_unlockbuf(rel, so->currPos.buf);
}


/*
 * The following routines manage a shared-memory area in which we track
 * assignment of "vacuum cycle IDs" to currently-active btree vacuuming
 * operations.  There is a single counter which increments each time we
 * start a vacuum to assign it a cycle ID.  Since multiple vacuums could
 * be active concurrently, we have to track the cycle ID for each active
 * vacuum; this requires at most MaxBackends entries (usually far fewer).
 * We assume at most one vacuum can be active for a given index.
 *
 * Access to the shared memory area is controlled by BtreeVacuumLock.
 * In principle we could use a separate lmgr locktag for each index,
 * but a single LWLock is much cheaper, and given the short time that
 * the lock is ever held, the concurrency hit should be minimal.
 */

typedef struct BTOneVacInfo
{
	LockRelId	relid;			/* global identifier of an index */
	BTCycleId	cycleid;		/* cycle ID for its active VACUUM */
} BTOneVacInfo;

typedef struct BTVacInfo
{
	BTCycleId	cycle_ctr;		/* cycle ID most recently assigned */
	int			num_vacuums;	/* number of currently active VACUUMs */
	int			max_vacuums;	/* allocated length of vacuums[] array */
	BTOneVacInfo vacuums[FLEXIBLE_ARRAY_MEMBER];
} BTVacInfo;

static BTVacInfo *btvacinfo;


/*
 * _bt_vacuum_cycleid --- get the active vacuum cycle ID for an index,
 *		or zero if there is no active VACUUM
 *
 * Note: for correct interlocking, the caller must already hold pin and
 * exclusive lock on each buffer it will store the cycle ID into.  This
 * ensures that even if a VACUUM starts immediately afterwards, it cannot
 * process those pages until the page split is complete.
 */
BTCycleId
_bt_vacuum_cycleid(Relation rel)
{
	BTCycleId	result = 0;
	int			i;

	/* Share lock is enough since this is a read-only operation */
	LWLockAcquire(BtreeVacuumLock, LW_SHARED);

	for (i = 0; i < btvacinfo->num_vacuums; i++)
	{
		BTOneVacInfo *vac = &btvacinfo->vacuums[i];

		if (vac->relid.relId == rel->rd_lockInfo.lockRelId.relId &&
			vac->relid.dbId == rel->rd_lockInfo.lockRelId.dbId)
		{
			result = vac->cycleid;
			break;
		}
	}

	LWLockRelease(BtreeVacuumLock);
	return result;
}

/*
 * _bt_start_vacuum --- assign a cycle ID to a just-starting VACUUM operation
 *
 * Note: the caller must guarantee that it will eventually call
 * _bt_end_vacuum, else we'll permanently leak an array slot.  To ensure
 * that this happens even in elog(FATAL) scenarios, the appropriate coding
 * is not just a PG_TRY, but
 *		PG_ENSURE_ERROR_CLEANUP(_bt_end_vacuum_callback, PointerGetDatum(rel))
 */
BTCycleId
_bt_start_vacuum(Relation rel)
{
	BTCycleId	result;
	int			i;
	BTOneVacInfo *vac;

	LWLockAcquire(BtreeVacuumLock, LW_EXCLUSIVE);

	/*
	 * Assign the next cycle ID, being careful to avoid zero as well as the
	 * reserved high values.
	 */
	result = ++(btvacinfo->cycle_ctr);
	if (result == 0 || result > MAX_BT_CYCLE_ID)
		result = btvacinfo->cycle_ctr = 1;

	/* Let's just make sure there's no entry already for this index */
	for (i = 0; i < btvacinfo->num_vacuums; i++)
	{
		vac = &btvacinfo->vacuums[i];
		if (vac->relid.relId == rel->rd_lockInfo.lockRelId.relId &&
			vac->relid.dbId == rel->rd_lockInfo.lockRelId.dbId)
		{
			/*
			 * Unlike most places in the backend, we have to explicitly
			 * release our LWLock before throwing an error.  This is because
			 * we expect _bt_end_vacuum() to be called before transaction
			 * abort cleanup can run to release LWLocks.
			 */
			LWLockRelease(BtreeVacuumLock);
			elog(ERROR, "multiple active vacuums for index \"%s\"",
				 RelationGetRelationName(rel));
		}
	}

	/* OK, add an entry */
	if (btvacinfo->num_vacuums >= btvacinfo->max_vacuums)
	{
		LWLockRelease(BtreeVacuumLock);
		elog(ERROR, "out of btvacinfo slots");
	}
	vac = &btvacinfo->vacuums[btvacinfo->num_vacuums];
	vac->relid = rel->rd_lockInfo.lockRelId;
	vac->cycleid = result;
	btvacinfo->num_vacuums++;

	LWLockRelease(BtreeVacuumLock);
	return result;
}

/*
 * _bt_end_vacuum --- mark a btree VACUUM operation as done
 *
 * Note: this is deliberately coded not to complain if no entry is found;
 * this allows the caller to put PG_TRY around the start_vacuum operation.
 */
void
_bt_end_vacuum(Relation rel)
{
	int			i;

	LWLockAcquire(BtreeVacuumLock, LW_EXCLUSIVE);

	/* Find the array entry */
	for (i = 0; i < btvacinfo->num_vacuums; i++)
	{
		BTOneVacInfo *vac = &btvacinfo->vacuums[i];

		if (vac->relid.relId == rel->rd_lockInfo.lockRelId.relId &&
			vac->relid.dbId == rel->rd_lockInfo.lockRelId.dbId)
		{
			/* Remove it by shifting down the last entry */
			*vac = btvacinfo->vacuums[btvacinfo->num_vacuums - 1];
			btvacinfo->num_vacuums--;
			break;
		}
	}

	LWLockRelease(BtreeVacuumLock);
}

/*
 * _bt_end_vacuum wrapped as an on_shmem_exit callback function
 */
void
_bt_end_vacuum_callback(int code, Datum arg)
{
	_bt_end_vacuum((Relation) DatumGetPointer(arg));
}

/*
 * BTreeShmemSize --- report amount of shared memory space needed
 */
Size
BTreeShmemSize(void)
{
	Size		size;

	size = offsetof(BTVacInfo, vacuums);
	size = add_size(size, mul_size(MaxBackends, sizeof(BTOneVacInfo)));
	return size;
}

/*
 * BTreeShmemInit --- initialize this module's shared memory
 */
void
BTreeShmemInit(void)
{
	bool		found;

	btvacinfo = (BTVacInfo *) ShmemInitStruct("BTree Vacuum State",
											  BTreeShmemSize(),
											  &found);

	if (!IsUnderPostmaster)
	{
		/* Initialize shared memory area */
		Assert(!found);

		/*
		 * It doesn't really matter what the cycle counter starts at, but
		 * having it always start the same doesn't seem good.  Seed with
		 * low-order bits of time() instead.
		 */
		btvacinfo->cycle_ctr = (BTCycleId) time(NULL);

		btvacinfo->num_vacuums = 0;
		btvacinfo->max_vacuums = MaxBackends;
	}
	else
		Assert(found);
}

bytea *
btoptions(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"fillfactor", RELOPT_TYPE_INT, offsetof(BTOptions, fillfactor)},
		{"vacuum_cleanup_index_scale_factor", RELOPT_TYPE_REAL,
		offsetof(BTOptions, vacuum_cleanup_index_scale_factor)},
		{"deduplicate_items", RELOPT_TYPE_BOOL,
		offsetof(BTOptions, deduplicate_items)}
	};

	return (bytea *) build_reloptions(reloptions, validate,
									  RELOPT_KIND_BTREE,
									  sizeof(BTOptions),
									  tab, lengthof(tab));
}

/*
 *	btproperty() -- Check boolean properties of indexes.
 *
 * This is optional, but handling AMPROP_RETURNABLE here saves opening the rel
 * to call btcanreturn.
 */
bool
btproperty(Oid index_oid, int attno,
		   IndexAMProperty prop, const char *propname,
		   bool *res, bool *isnull)
{
	switch (prop)
	{
		case AMPROP_RETURNABLE:
			/* answer only for columns, not AM or whole index */
			if (attno == 0)
				return false;
			/* otherwise, btree can always return data */
			*res = true;
			return true;

		default:
			return false;		/* punt to generic code */
	}
}

/*
 *	btbuildphasename() -- Return name of index build phase.
 */
char *
btbuildphasename(int64 phasenum)
{
	switch (phasenum)
	{
		case PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE:
			return "initializing";
		case PROGRESS_BTREE_PHASE_INDEXBUILD_TABLESCAN:
			return "scanning table";
		case PROGRESS_BTREE_PHASE_PERFORMSORT_1:
			return "sorting live tuples";
		case PROGRESS_BTREE_PHASE_PERFORMSORT_2:
			return "sorting dead tuples";
		case PROGRESS_BTREE_PHASE_LEAF_LOAD:
			return "loading tuples in tree";
		default:
			return NULL;
	}
}

/*
 *	_bt_truncate() -- create tuple without unneeded suffix attributes.
 *
 * Returns truncated pivot index tuple allocated in caller's memory context,
 * with key attributes copied from caller's firstright argument.  If rel is
 * an INCLUDE index, non-key attributes will definitely be truncated away,
 * since they're not part of the key space.  More aggressive suffix
 * truncation can take place when it's clear that the returned tuple does not
 * need one or more suffix key attributes.  We only need to keep firstright
 * attributes up to and including the first non-lastleft-equal attribute.
 * Caller's insertion scankey is used to compare the tuples; the scankey's
 * argument values are not considered here.
 *
 * Note that returned tuple's t_tid offset will hold the number of attributes
 * present, so the original item pointer offset is not represented.  Caller
 * should only change truncated tuple's downlink.  Note also that truncated
 * key attributes are treated as containing "minus infinity" values by
 * _bt_compare().
 *
 * In the worst case (when a heap TID must be appended to distinguish lastleft
 * from firstright), the size of the returned tuple is the size of firstright
 * plus the size of an additional MAXALIGN()'d item pointer.  This guarantee
 * is important, since callers need to stay under the 1/3 of a page
 * restriction on tuple size.  If this routine is ever taught to truncate
 * within an attribute/datum, it will need to avoid returning an enlarged
 * tuple to caller when truncation + TOAST compression ends up enlarging the
 * final datum.
 */
IndexTuple
_bt_truncate(Relation rel, IndexTuple lastleft, IndexTuple firstright,
			 BTScanInsert itup_key)
{
	TupleDesc	itupdesc = RelationGetDescr(rel);
	int16		nkeyatts = IndexRelationGetNumberOfKeyAttributes(rel);
	int			keepnatts;
	IndexTuple	pivot;
	IndexTuple	tidpivot;
	ItemPointer pivotheaptid;
	Size		newsize;

	/*
	 * We should only ever truncate non-pivot tuples from leaf pages.  It's
	 * never okay to truncate when splitting an internal page.
	 */
	Assert(!BTreeTupleIsPivot(lastleft) && !BTreeTupleIsPivot(firstright));

	/* Determine how many attributes must be kept in truncated tuple */
	keepnatts = _bt_keep_natts(rel, lastleft, firstright, itup_key);

#ifdef DEBUG_NO_TRUNCATE
	/* Force truncation to be ineffective for testing purposes */
	keepnatts = nkeyatts + 1;
#endif

	pivot = index_truncate_tuple(itupdesc, firstright,
								 Min(keepnatts, nkeyatts));

	if (BTreeTupleIsPosting(pivot))
	{
		/*
		 * index_truncate_tuple() just returns a straight copy of firstright
		 * when it has no attributes to truncate.  When that happens, we may
		 * need to truncate away a posting list here instead.
		 */
		Assert(keepnatts == nkeyatts || keepnatts == nkeyatts + 1);
		Assert(IndexRelationGetNumberOfAttributes(rel) == nkeyatts);
		pivot->t_info &= ~INDEX_SIZE_MASK;
		pivot->t_info |= MAXALIGN(BTreeTupleGetPostingOffset(firstright));
	}

	/*
	 * If there is a distinguishing key attribute within pivot tuple, we're
	 * done
	 */
	if (keepnatts <= nkeyatts)
	{
		BTreeTupleSetNAtts(pivot, keepnatts, false);
		return pivot;
	}

	/*
	 * We have to store a heap TID in the new pivot tuple, since no non-TID
	 * key attribute value in firstright distinguishes the right side of the
	 * split from the left side.  nbtree conceptualizes this case as an
	 * inability to truncate away any key attributes, since heap TID is
	 * treated as just another key attribute (despite lacking a pg_attribute
	 * entry).
	 *
	 * Use enlarged space that holds a copy of pivot.  We need the extra space
	 * to store a heap TID at the end (using the special pivot tuple
	 * representation).  Note that the original pivot already has firstright's
	 * possible posting list/non-key attribute values removed at this point.
	 */
	newsize = MAXALIGN(IndexTupleSize(pivot)) + MAXALIGN(sizeof(ItemPointerData));
	tidpivot = palloc0(newsize);
	memcpy(tidpivot, pivot, MAXALIGN(IndexTupleSize(pivot)));
	/* Cannot leak memory here */
	pfree(pivot);

	/*
	 * Store all of firstright's key attribute values plus a tiebreaker heap
	 * TID value in enlarged pivot tuple
	 */
	tidpivot->t_info &= ~INDEX_SIZE_MASK;
	tidpivot->t_info |= newsize;
	BTreeTupleSetNAtts(tidpivot, nkeyatts, true);
	pivotheaptid = BTreeTupleGetHeapTID(tidpivot);

	/*
	 * Lehman & Yao use lastleft as the leaf high key in all cases, but don't
	 * consider suffix truncation.  It seems like a good idea to follow that
	 * example in cases where no truncation takes place -- use lastleft's heap
	 * TID.  (This is also the closest value to negative infinity that's
	 * legally usable.)
	 */
	ItemPointerCopy(BTreeTupleGetMaxHeapTID(lastleft), pivotheaptid);

	/*
	 * We're done.  Assert() that heap TID invariants hold before returning.
	 *
	 * Lehman and Yao require that the downlink to the right page, which is to
	 * be inserted into the parent page in the second phase of a page split be
	 * a strict lower bound on items on the right page, and a non-strict upper
	 * bound for items on the left page.  Assert that heap TIDs follow these
	 * invariants, since a heap TID value is apparently needed as a
	 * tiebreaker.
	 */
#ifndef DEBUG_NO_TRUNCATE
	Assert(ItemPointerCompare(BTreeTupleGetMaxHeapTID(lastleft),
							  BTreeTupleGetHeapTID(firstright)) < 0);
	Assert(ItemPointerCompare(pivotheaptid,
							  BTreeTupleGetHeapTID(lastleft)) >= 0);
	Assert(ItemPointerCompare(pivotheaptid,
							  BTreeTupleGetHeapTID(firstright)) < 0);
#else

	/*
	 * Those invariants aren't guaranteed to hold for lastleft + firstright
	 * heap TID attribute values when they're considered here only because
	 * DEBUG_NO_TRUNCATE is defined (a heap TID is probably not actually
	 * needed as a tiebreaker).  DEBUG_NO_TRUNCATE must therefore use a heap
	 * TID value that always works as a strict lower bound for items to the
	 * right.  In particular, it must avoid using firstright's leading key
	 * attribute values along with lastleft's heap TID value when lastleft's
	 * TID happens to be greater than firstright's TID.
	 */
	ItemPointerCopy(BTreeTupleGetHeapTID(firstright), pivotheaptid);

	/*
	 * Pivot heap TID should never be fully equal to firstright.  Note that
	 * the pivot heap TID will still end up equal to lastleft's heap TID when
	 * that's the only usable value.
	 */
	ItemPointerSetOffsetNumber(pivotheaptid,
							   OffsetNumberPrev(ItemPointerGetOffsetNumber(pivotheaptid)));
	Assert(ItemPointerCompare(pivotheaptid,
							  BTreeTupleGetHeapTID(firstright)) < 0);
#endif

	return tidpivot;
}

/*
 * _bt_keep_natts - how many key attributes to keep when truncating.
 *
 * Caller provides two tuples that enclose a split point.  Caller's insertion
 * scankey is used to compare the tuples; the scankey's argument values are
 * not considered here.
 *
 * This can return a number of attributes that is one greater than the
 * number of key attributes for the index relation.  This indicates that the
 * caller must use a heap TID as a unique-ifier in new pivot tuple.
 */
static int
_bt_keep_natts(Relation rel, IndexTuple lastleft, IndexTuple firstright,
			   BTScanInsert itup_key)
{
	int			nkeyatts = IndexRelationGetNumberOfKeyAttributes(rel);
	TupleDesc	itupdesc = RelationGetDescr(rel);
	int			keepnatts;
	ScanKey		scankey;

	/*
	 * _bt_compare() treats truncated key attributes as having the value minus
	 * infinity, which would break searches within !heapkeyspace indexes.  We
	 * must still truncate away non-key attribute values, though.
	 */
	if (!itup_key->heapkeyspace)
		return nkeyatts;

	scankey = itup_key->scankeys;
	keepnatts = 1;
	for (int attnum = 1; attnum <= nkeyatts; attnum++, scankey++)
	{
		Datum		datum1,
					datum2;
		bool		isNull1,
					isNull2;

		datum1 = index_getattr(lastleft, attnum, itupdesc, &isNull1);
		datum2 = index_getattr(firstright, attnum, itupdesc, &isNull2);

		if (isNull1 != isNull2)
			break;

		if (!isNull1 &&
			DatumGetInt32(FunctionCall2Coll(&scankey->sk_func,
											scankey->sk_collation,
											datum1,
											datum2)) != 0)
			break;

		keepnatts++;
	}

	/*
	 * Assert that _bt_keep_natts_fast() agrees with us in passing.  This is
	 * expected in an allequalimage index.
	 */
	Assert(!itup_key->allequalimage ||
		   keepnatts == _bt_keep_natts_fast(rel, lastleft, firstright));

	return keepnatts;
}

/*
 * _bt_keep_natts_fast - fast bitwise variant of _bt_keep_natts.
 *
 * This is exported so that a candidate split point can have its effect on
 * suffix truncation inexpensively evaluated ahead of time when finding a
 * split location.  A naive bitwise approach to datum comparisons is used to
 * save cycles.
 *
 * The approach taken here usually provides the same answer as _bt_keep_natts
 * will (for the same pair of tuples from a heapkeyspace index), since the
 * majority of btree opclasses can never indicate that two datums are equal
 * unless they're bitwise equal after detoasting.  When an index only has
 * "equal image" columns, routine is guaranteed to give the same result as
 * _bt_keep_natts would.
 *
 * Callers can rely on the fact that attributes considered equal here are
 * definitely also equal according to _bt_keep_natts, even when the index uses
 * an opclass or collation that is not "allequalimage"/deduplication-safe.
 * This weaker guarantee is good enough for nbtsplitloc.c caller, since false
 * negatives generally only have the effect of making leaf page splits use a
 * more balanced split point.
 */
int
_bt_keep_natts_fast(Relation rel, IndexTuple lastleft, IndexTuple firstright)
{
	TupleDesc	itupdesc = RelationGetDescr(rel);
	int			keysz = IndexRelationGetNumberOfKeyAttributes(rel);
	int			keepnatts;

	keepnatts = 1;
	for (int attnum = 1; attnum <= keysz; attnum++)
	{
		Datum		datum1,
					datum2;
		bool		isNull1,
					isNull2;
		CompactAttribute *att;

		datum1 = index_getattr(lastleft, attnum, itupdesc, &isNull1);
		datum2 = index_getattr(firstright, attnum, itupdesc, &isNull2);
		att = TupleDescCompactAttr(itupdesc, attnum - 1);

		if (isNull1 != isNull2)
			break;

		if (!isNull1 &&
			!datum_image_eq(datum1, datum2, att->attbyval, att->attlen))
			break;

		keepnatts++;
	}

	return keepnatts;
}

/*
 *  _bt_check_natts() -- Verify tuple has expected number of attributes.
 *
 * Returns value indicating if the expected number of attributes were found
 * for a particular offset on page.  This can be used as a general purpose
 * sanity check.
 *
 * Testing a tuple directly with BTreeTupleGetNAtts() should generally be
 * preferred to calling here.  That's usually more convenient, and is always
 * more explicit.  Call here instead when offnum's tuple may be a negative
 * infinity tuple that uses the pre-v11 on-disk representation, or when a low
 * context check is appropriate.  This routine is as strict as possible about
 * what is expected on each version of btree.
 */
bool
_bt_check_natts(Relation rel, bool heapkeyspace, Page page, OffsetNumber offnum)
{
	int16		natts = IndexRelationGetNumberOfAttributes(rel);
	int16		nkeyatts = IndexRelationGetNumberOfKeyAttributes(rel);
	BTPageOpaque opaque = BTPageGetOpaque(page);
	IndexTuple	itup;
	int			tupnatts;

	/*
	 * We cannot reliably test a deleted or half-dead page, since they have
	 * dummy high keys
	 */
	if (P_IGNORE(opaque))
		return true;

	Assert(offnum >= FirstOffsetNumber &&
		   offnum <= PageGetMaxOffsetNumber(page));

	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offnum));
	tupnatts = BTreeTupleGetNAtts(itup, rel);

	/* !heapkeyspace indexes do not support deduplication */
	if (!heapkeyspace && BTreeTupleIsPosting(itup))
		return false;

	/* Posting list tuples should never have "pivot heap TID" bit set */
	if (BTreeTupleIsPosting(itup) &&
		(ItemPointerGetOffsetNumberNoCheck(&itup->t_tid) &
		 BT_PIVOT_HEAP_TID_ATTR) != 0)
		return false;

	/* INCLUDE indexes do not support deduplication */
	if (natts != nkeyatts && BTreeTupleIsPosting(itup))
		return false;

	if (P_ISLEAF(opaque))
	{
		if (offnum >= P_FIRSTDATAKEY(opaque))
		{
			/*
			 * Non-pivot tuple should never be explicitly marked as a pivot
			 * tuple
			 */
			if (BTreeTupleIsPivot(itup))
				return false;

			/*
			 * Leaf tuples that are not the page high key (non-pivot tuples)
			 * should never be truncated.  (Note that tupnatts must have been
			 * inferred, even with a posting list tuple, because only pivot
			 * tuples store tupnatts directly.)
			 */
			return tupnatts == natts;
		}
		else
		{
			/*
			 * Rightmost page doesn't contain a page high key, so tuple was
			 * checked above as ordinary leaf tuple
			 */
			Assert(!P_RIGHTMOST(opaque));

			/*
			 * !heapkeyspace high key tuple contains only key attributes. Note
			 * that tupnatts will only have been explicitly represented in
			 * !heapkeyspace indexes that happen to have non-key attributes.
			 */
			if (!heapkeyspace)
				return tupnatts == nkeyatts;

			/* Use generic heapkeyspace pivot tuple handling */
		}
	}
	else						/* !P_ISLEAF(opaque) */
	{
		if (offnum == P_FIRSTDATAKEY(opaque))
		{
			/*
			 * The first tuple on any internal page (possibly the first after
			 * its high key) is its negative infinity tuple.  Negative
			 * infinity tuples are always truncated to zero attributes.  They
			 * are a particular kind of pivot tuple.
			 */
			if (heapkeyspace)
				return tupnatts == 0;

			/*
			 * The number of attributes won't be explicitly represented if the
			 * negative infinity tuple was generated during a page split that
			 * occurred with a version of Postgres before v11.  There must be
			 * a problem when there is an explicit representation that is
			 * non-zero, or when there is no explicit representation and the
			 * tuple is evidently not a pre-pg_upgrade tuple.
			 *
			 * Prior to v11, downlinks always had P_HIKEY as their offset.
			 * Accept that as an alternative indication of a valid
			 * !heapkeyspace negative infinity tuple.
			 */
			return tupnatts == 0 ||
				ItemPointerGetOffsetNumber(&(itup->t_tid)) == P_HIKEY;
		}
		else
		{
			/*
			 * !heapkeyspace downlink tuple with separator key contains only
			 * key attributes.  Note that tupnatts will only have been
			 * explicitly represented in !heapkeyspace indexes that happen to
			 * have non-key attributes.
			 */
			if (!heapkeyspace)
				return tupnatts == nkeyatts;

			/* Use generic heapkeyspace pivot tuple handling */
		}
	}

	/* Handle heapkeyspace pivot tuples (excluding minus infinity items) */
	Assert(heapkeyspace);

	/*
	 * Explicit representation of the number of attributes is mandatory with
	 * heapkeyspace index pivot tuples, regardless of whether or not there are
	 * non-key attributes.
	 */
	if (!BTreeTupleIsPivot(itup))
		return false;

	/* Pivot tuple should not use posting list representation (redundant) */
	if (BTreeTupleIsPosting(itup))
		return false;

	/*
	 * Heap TID is a tiebreaker key attribute, so it cannot be untruncated
	 * when any other key attribute is truncated
	 */
	if (BTreeTupleGetHeapTID(itup) != NULL && tupnatts != nkeyatts)
		return false;

	/*
	 * Pivot tuple must have at least one untruncated key attribute (minus
	 * infinity pivot tuples are the only exception).  Pivot tuples can never
	 * represent that there is a value present for a key attribute that
	 * exceeds pg_index.indnkeyatts for the index.
	 */
	return tupnatts > 0 && tupnatts <= nkeyatts;
}

/*
 *
 *  _bt_check_third_page() -- check whether tuple fits on a btree page at all.
 *
 * We actually need to be able to fit three items on every page, so restrict
 * any one item to 1/3 the per-page available space.  Note that itemsz should
 * not include the ItemId overhead.
 *
 * It might be useful to apply TOAST methods rather than throw an error here.
 * Using out of line storage would break assumptions made by suffix truncation
 * and by contrib/amcheck, though.
 */
void
_bt_check_third_page(Relation rel, Relation heap, bool needheaptidspace,
					 Page page, IndexTuple newtup)
{
	Size		itemsz;
	BTPageOpaque opaque;

	itemsz = MAXALIGN(IndexTupleSize(newtup));

	/* Double check item size against limit */
	if (itemsz <= BTMaxItemSize)
		return;

	/*
	 * Tuple is probably too large to fit on page, but it's possible that the
	 * index uses version 2 or version 3, or that page is an internal page, in
	 * which case a slightly higher limit applies.
	 */
	if (!needheaptidspace && itemsz <= BTMaxItemSizeNoHeapTid)
		return;

	/*
	 * Internal page insertions cannot fail here, because that would mean that
	 * an earlier leaf level insertion that should have failed didn't
	 */
	opaque = BTPageGetOpaque(page);
	if (!P_ISLEAF(opaque))
		elog(ERROR, "cannot insert oversized tuple of size %zu on internal page of index \"%s\"",
			 itemsz, RelationGetRelationName(rel));

	ereport(ERROR,
			(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
			 errmsg("index row size %zu exceeds btree version %u maximum %zu for index \"%s\"",
					itemsz,
					needheaptidspace ? BTREE_VERSION : BTREE_NOVAC_VERSION,
					needheaptidspace ? BTMaxItemSize : BTMaxItemSizeNoHeapTid,
					RelationGetRelationName(rel)),
			 errdetail("Index row references tuple (%u,%u) in relation \"%s\".",
					   ItemPointerGetBlockNumber(BTreeTupleGetHeapTID(newtup)),
					   ItemPointerGetOffsetNumber(BTreeTupleGetHeapTID(newtup)),
					   RelationGetRelationName(heap)),
			 errhint("Values larger than 1/3 of a buffer page cannot be indexed.\n"
					 "Consider a function index of an MD5 hash of the value, "
					 "or use full text indexing."),
			 errtableconstraint(heap, RelationGetRelationName(rel))));
}

/*
 * Are all attributes in rel "equality is image equality" attributes?
 *
 * We use each attribute's BTEQUALIMAGE_PROC opclass procedure.  If any
 * opclass either lacks a BTEQUALIMAGE_PROC procedure or returns false, we
 * return false; otherwise we return true.
 *
 * Returned boolean value is stored in index metapage during index builds.
 * Deduplication can only be used when we return true.
 */
bool
_bt_allequalimage(Relation rel, bool debugmessage)
{
	bool		allequalimage = true;

	/* INCLUDE indexes can never support deduplication */
	if (IndexRelationGetNumberOfAttributes(rel) !=
		IndexRelationGetNumberOfKeyAttributes(rel))
		return false;

	for (int i = 0; i < IndexRelationGetNumberOfKeyAttributes(rel); i++)
	{
		Oid			opfamily = rel->rd_opfamily[i];
		Oid			opcintype = rel->rd_opcintype[i];
		Oid			collation = rel->rd_indcollation[i];
		Oid			equalimageproc;

		equalimageproc = get_opfamily_proc(opfamily, opcintype, opcintype,
										   BTEQUALIMAGE_PROC);

		/*
		 * If there is no BTEQUALIMAGE_PROC then deduplication is assumed to
		 * be unsafe.  Otherwise, actually call proc and see what it says.
		 */
		if (!OidIsValid(equalimageproc) ||
			!DatumGetBool(OidFunctionCall1Coll(equalimageproc, collation,
											   ObjectIdGetDatum(opcintype))))
		{
			allequalimage = false;
			break;
		}
	}

	if (debugmessage)
	{
		if (allequalimage)
			elog(DEBUG1, "index \"%s\" can safely use deduplication",
				 RelationGetRelationName(rel));
		else
			elog(DEBUG1, "index \"%s\" cannot use deduplication",
				 RelationGetRelationName(rel));
	}

	return allequalimage;
}
