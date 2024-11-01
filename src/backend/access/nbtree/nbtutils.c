/*-------------------------------------------------------------------------
 *
 * nbtutils.c
 *	  Utility code for Postgres btree implementation.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
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
#include "access/relscan.h"
#include "commands/progress.h"
#include "lib/qunique.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#define LOOK_AHEAD_REQUIRED_RECHECKS 	3
#define LOOK_AHEAD_DEFAULT_DISTANCE 	5

typedef struct BTSortArrayContext
{
	FmgrInfo   *sortproc;
	Oid			collation;
	bool		reverse;
} BTSortArrayContext;

typedef struct BTScanKeyPreproc
{
	ScanKey		inkey;
	int			inkeyi;
	int			arrayidx;
} BTScanKeyPreproc;

static void _bt_setup_array_cmp(IndexScanDesc scan, ScanKey skey, Oid elemtype,
								FmgrInfo *orderproc, FmgrInfo **sortprocp);
static Datum _bt_find_extreme_element(IndexScanDesc scan, ScanKey skey,
									  Oid elemtype, StrategyNumber strat,
									  Datum *elems, int nelems);
static int	_bt_sort_array_elements(ScanKey skey, FmgrInfo *sortproc,
									bool reverse, Datum *elems, int nelems);
static bool _bt_merge_arrays(IndexScanDesc scan, ScanKey skey,
							 FmgrInfo *sortproc, bool reverse,
							 Oid origelemtype, Oid nextelemtype,
							 Datum *elems_orig, int *nelems_orig,
							 Datum *elems_next, int nelems_next);
static bool _bt_compare_array_scankey_args(IndexScanDesc scan,
										   ScanKey arraysk, ScanKey skey,
										   FmgrInfo *orderproc, BTArrayKeyInfo *array,
										   bool *qual_ok);
static ScanKey _bt_preprocess_array_keys(IndexScanDesc scan, int *new_numberOfKeys);
static void _bt_preprocess_array_keys_final(IndexScanDesc scan, int *keyDataMap);
static int	_bt_compare_array_elements(const void *a, const void *b, void *arg);
static inline int32 _bt_compare_array_skey(FmgrInfo *orderproc,
										   Datum tupdatum, bool tupnull,
										   Datum arrdatum, ScanKey cur);
static int	_bt_binsrch_array_skey(FmgrInfo *orderproc,
								   bool cur_elem_trig, ScanDirection dir,
								   Datum tupdatum, bool tupnull,
								   BTArrayKeyInfo *array, ScanKey cur,
								   int32 *set_elem_result);
static bool _bt_advance_array_keys_increment(IndexScanDesc scan, ScanDirection dir);
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
static bool _bt_compare_scankey_args(IndexScanDesc scan, ScanKey op,
									 ScanKey leftarg, ScanKey rightarg,
									 BTArrayKeyInfo *array, FmgrInfo *orderproc,
									 bool *result);
static bool _bt_fix_scankey_strategy(ScanKey skey, int16 *indoption);
static void _bt_mark_scankey_required(ScanKey skey);
static bool _bt_check_compare(IndexScanDesc scan, ScanDirection dir,
							  IndexTuple tuple, int tupnatts, TupleDesc tupdesc,
							  bool advancenonrequired, bool prechecked, bool firstmatch,
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
 * We set the scan key references from the scan's BTArrayKeyInfo info array to
 * offsets into the temp modified input array returned to caller.  Scans that
 * have array keys should call _bt_preprocess_array_keys_final when standard
 * preprocessing steps are complete.  This will convert the scan key offset
 * references into references to the scan's so->keyData[] output scan keys.
 *
 * Note: the reason we need to return a temp scan key array, rather than just
 * scribbling on scan->keyData, is that callers are permitted to call btrescan
 * without supplying a new set of scankey data.
 */
static ScanKey
_bt_preprocess_array_keys(IndexScanDesc scan, int *new_numberOfKeys)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	Relation	rel = scan->indexRelation;
	int			numberOfKeys = scan->numberOfKeys;
	int16	   *indoption = rel->rd_indoption;
	int			numArrayKeys,
				output_ikey = 0;
	int			origarrayatt = InvalidAttrNumber,
				origarraykey = -1;
	Oid			origelemtype = InvalidOid;
	ScanKey		cur;
	MemoryContext oldContext;
	ScanKey		arrayKeyData;	/* modified copy of scan->keyData */

	Assert(numberOfKeys);

	/* Quick check to see if there are any array keys */
	numArrayKeys = 0;
	for (int i = 0; i < numberOfKeys; i++)
	{
		cur = &scan->keyData[i];
		if (cur->sk_flags & SK_SEARCHARRAY)
		{
			numArrayKeys++;
			Assert(!(cur->sk_flags & (SK_ROW_HEADER | SK_SEARCHNULL | SK_SEARCHNOTNULL)));
			/* If any arrays are null as a whole, we can quit right now. */
			if (cur->sk_flags & SK_ISNULL)
			{
				so->qual_ok = false;
				return NULL;
			}
		}
	}

	/* Quit if nothing to do. */
	if (numArrayKeys == 0)
		return NULL;

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
	arrayKeyData = (ScanKey) palloc(numberOfKeys * sizeof(ScanKeyData));

	/* Allocate space for per-array data in the workspace context */
	so->arrayKeys = (BTArrayKeyInfo *) palloc(numArrayKeys * sizeof(BTArrayKeyInfo));

	/* Allocate space for ORDER procs used to help _bt_checkkeys */
	so->orderProcs = (FmgrInfo *) palloc(numberOfKeys * sizeof(FmgrInfo));

	/* Now process each array key */
	numArrayKeys = 0;
	for (int input_ikey = 0; input_ikey < numberOfKeys; input_ikey++)
	{
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
		int			j;

		/*
		 * Provisionally copy scan key into arrayKeyData[] array we'll return
		 * to _bt_preprocess_keys caller
		 */
		cur = &arrayKeyData[output_ikey];
		*cur = scan->keyData[input_ikey];

		if (!(cur->sk_flags & SK_SEARCHARRAY))
		{
			output_ikey++;		/* keep this non-array scan key */
			continue;
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
		for (j = 0; j < num_elems; j++)
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
				output_ikey++;	/* keep this transformed scan key */
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
				output_ikey++;	/* keep this transformed scan key */
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
							&so->orderProcs[output_ikey], &sortprocp);

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

		/*
		 * And set up the BTArrayKeyInfo data.
		 *
		 * Note: _bt_preprocess_array_keys_final will fix-up each array's
		 * scan_key field later on, after so->keyData[] has been finalized.
		 */
		so->arrayKeys[numArrayKeys].scan_key = output_ikey;
		so->arrayKeys[numArrayKeys].num_elems = num_elems;
		so->arrayKeys[numArrayKeys].elem_values = elem_values;
		numArrayKeys++;
		output_ikey++;			/* keep this scan key/array */
	}

	/* Set final number of equality-type array keys */
	so->numArrayKeys = numArrayKeys;
	/* Set number of scan keys remaining in arrayKeyData[] */
	*new_numberOfKeys = output_ikey;

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

			Assert(array->num_elems > 0);

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
	 * using a spinlock, so defensively limit its size.  In practice this can
	 * only affect parallel scans that use an incomplete opfamily.
	 */
	if (scan->parallel_scan && so->numArrayKeys > INDEX_MAX_KEYS)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg_internal("number of array scan keys left by preprocessing (%d) exceeds the maximum allowed by parallel btree index scans (%d)",
								 so->numArrayKeys, INDEX_MAX_KEYS)));
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
 * return false if the required comparisons cannot not be made (caller must
 * keep both arrays when this happens).
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
 * Compare an array scan key to a scalar scan key, eliminating contradictory
 * array elements such that the scalar scan key becomes redundant.
 *
 * Array elements can be eliminated as contradictory when excluded by some
 * other operator on the same attribute.  For example, with an index scan qual
 * "WHERE a IN (1, 2, 3) AND a < 2", all array elements except the value "1"
 * are eliminated, and the < scan key is eliminated as redundant.  Cases where
 * every array element is eliminated by a redundant scalar scan key have an
 * unsatisfiable qual, which we handle by setting *qual_ok=false for caller.
 *
 * If the opfamily doesn't supply a complete set of cross-type ORDER procs we
 * may not be able to determine which elements are contradictory.  If we have
 * the required ORDER proc then we return true (and validly set *qual_ok),
 * guaranteeing that at least the scalar scan key can be considered redundant.
 * We return false if the comparison could not be made (caller must keep both
 * scan keys when this happens).
 */
static bool
_bt_compare_array_scankey_args(IndexScanDesc scan, ScanKey arraysk, ScanKey skey,
							   FmgrInfo *orderproc, BTArrayKeyInfo *array,
							   bool *qual_ok)
{
	Relation	rel = scan->indexRelation;
	Oid			opcintype = rel->rd_opcintype[arraysk->sk_attno - 1];
	int			cmpresult = 0,
				cmpexact = 0,
				matchelem,
				new_nelems = 0;
	FmgrInfo	crosstypeproc;
	FmgrInfo   *orderprocp = orderproc;

	Assert(arraysk->sk_attno == skey->sk_attno);
	Assert(array->num_elems > 0);
	Assert(!(arraysk->sk_flags & (SK_ISNULL | SK_ROW_HEADER | SK_ROW_MEMBER)));
	Assert((arraysk->sk_flags & SK_SEARCHARRAY) &&
		   arraysk->sk_strategy == BTEqualStrategyNumber);
	Assert(!(skey->sk_flags & (SK_ISNULL | SK_ROW_HEADER | SK_ROW_MEMBER)));
	Assert(!(skey->sk_flags & SK_SEARCHARRAY) ||
		   skey->sk_strategy != BTEqualStrategyNumber);

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
static int
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
 * _bt_start_array_keys() -- Initialize array keys at start of a scan
 *
 * Set up the cur_elem counters and fill in the first sk_argument value for
 * each array scankey.
 */
void
_bt_start_array_keys(IndexScanDesc scan, ScanDirection dir)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	int			i;

	Assert(so->numArrayKeys);
	Assert(so->qual_ok);

	for (i = 0; i < so->numArrayKeys; i++)
	{
		BTArrayKeyInfo *curArrayKey = &so->arrayKeys[i];
		ScanKey		skey = &so->keyData[curArrayKey->scan_key];

		Assert(curArrayKey->num_elems > 0);
		Assert(skey->sk_flags & SK_SEARCHARRAY);

		if (ScanDirectionIsBackward(dir))
			curArrayKey->cur_elem = curArrayKey->num_elems - 1;
		else
			curArrayKey->cur_elem = 0;
		skey->sk_argument = curArrayKey->elem_values[curArrayKey->cur_elem];
	}
	so->scanBehind = so->oppositeDirCheck = false;	/* reset */
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
_bt_advance_array_keys_increment(IndexScanDesc scan, ScanDirection dir)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;

	/*
	 * We must advance the last array key most quickly, since it will
	 * correspond to the lowest-order index column among the available
	 * qualifications
	 */
	for (int i = so->numArrayKeys - 1; i >= 0; i--)
	{
		BTArrayKeyInfo *curArrayKey = &so->arrayKeys[i];
		ScanKey		skey = &so->keyData[curArrayKey->scan_key];
		int			cur_elem = curArrayKey->cur_elem;
		int			num_elems = curArrayKey->num_elems;
		bool		rolled = false;

		if (ScanDirectionIsForward(dir) && ++cur_elem >= num_elems)
		{
			cur_elem = 0;
			rolled = true;
		}
		else if (ScanDirectionIsBackward(dir) && --cur_elem < 0)
		{
			cur_elem = num_elems - 1;
			rolled = true;
		}

		curArrayKey->cur_elem = cur_elem;
		skey->sk_argument = curArrayKey->elem_values[cur_elem];
		if (!rolled)
			return true;

		/* Need to advance next array key, if any */
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
 * _bt_rewind_nonrequired_arrays() -- Rewind non-required arrays
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
 */
static void
_bt_rewind_nonrequired_arrays(IndexScanDesc scan, ScanDirection dir)
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
		Assert(array->scan_key == ikey);

		if ((cur->sk_flags & (SK_BT_REQFWD | SK_BT_REQBKWD)))
			continue;

		if (ScanDirectionIsForward(dir))
			first_elem_dir = 0;
		else
			first_elem_dir = array->num_elems - 1;

		if (array->cur_elem != first_elem_dir)
		{
			array->cur_elem = first_elem_dir;
			cur->sk_argument = array->elem_values[first_elem_dir];
		}
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
 * inability to distinguishing between the < and > cases (it uses equality
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

		result = _bt_compare_array_skey(&so->orderProcs[ikey],
										tupdatum, tupnull,
										cur->sk_argument, cur);

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
 * We also deal with "advancing" non-required arrays here.  Callers whose
 * sktrig scan key is non-required specify sktrig_required=false.  These calls
 * are the only exception to the general rule about always advancing the
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
				has_required_opposite_direction_only = false,
				oppodir_inequality_sktrig = false,
				all_required_satisfied = true,
				all_satisfied = true;

	if (sktrig_required)
	{
		/*
		 * Precondition array state assertion
		 */
		Assert(!_bt_tuple_before_array_skeys(scan, dir, tuple, tupdesc,
											 tupnatts, false, 0, NULL));

		so->scanBehind = so->oppositeDirCheck = false;	/* reset */

		/*
		 * Required scan key wasn't satisfied, so required arrays will have to
		 * advance.  Invalidate page-level state that tracks whether the
		 * scan's required-in-opposite-direction-only keys are known to be
		 * satisfied by page's remaining tuples.
		 */
		pstate->firstmatch = false;

		/* Shouldn't have to invalidate 'prechecked', though */
		Assert(!pstate->prechecked);

		/*
		 * Once we return we'll have a new set of required array keys, so
		 * reset state used by "look ahead" optimization
		 */
		pstate->rechecks = 0;
		pstate->targetdistance = 0;
	}

	Assert(_bt_verify_keys_with_arraykeys(scan));

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
			Assert(sktrig_required);

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

			/*
			 * Set a flag that remembers that this was an inequality required
			 * in the opposite scan direction only, that nevertheless
			 * triggered the call here.
			 *
			 * This only happens when an inequality operator (which must be
			 * strict) encounters a group of NULLs that indicate the end of
			 * non-NULL values for tuples in the current scan direction.
			 */
			if (unlikely(required_opposite_direction_only))
				oppodir_inequality_sktrig = true;

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
			int			final_elem_dir;

			if (ScanDirectionIsBackward(dir) || !array)
				final_elem_dir = 0;
			else
				final_elem_dir = array->num_elems - 1;

			if (array && array->cur_elem != final_elem_dir)
			{
				array->cur_elem = final_elem_dir;
				cur->sk_argument = array->elem_values[final_elem_dir];
			}

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
			int			first_elem_dir;

			if (ScanDirectionIsForward(dir) || !array)
				first_elem_dir = 0;
			else
				first_elem_dir = array->num_elems - 1;

			if (array && array->cur_elem != first_elem_dir)
			{
				array->cur_elem = first_elem_dir;
				cur->sk_argument = array->elem_values[first_elem_dir];
			}

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
			 * Binary search for closest match that's available from the array
			 */
			set_elem = _bt_binsrch_array_skey(&so->orderProcs[ikey],
											  cur_elem_trig, dir,
											  tupdatum, tupnull, array, cur,
											  &result);

			Assert(set_elem >= 0 && set_elem < array->num_elems);
		}
		else
		{
			Assert(sktrig_required && required);

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
		if (required &&
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
			if (required)
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

		/* Advance array keys, even when set_elem isn't an exact match */
		if (array && array->cur_elem != set_elem)
		{
			array->cur_elem = set_elem;
			cur->sk_argument = array->elem_values[set_elem];
		}
	}

	/*
	 * Advance the array keys incrementally whenever "beyond end of array
	 * element" array advancement happens, so that advancement will carry to
	 * higher-order arrays (might exhaust all the scan's arrays instead, which
	 * ends the top-level scan).
	 */
	if (beyond_end_advance && !_bt_advance_array_keys_increment(scan, dir))
		goto end_toplevel_scan;

	Assert(_bt_verify_keys_with_arraykeys(scan));

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
	 */
	if ((sktrig_required && all_required_satisfied) ||
		(!sktrig_required && all_satisfied))
	{
		int			nsktrig = sktrig + 1;
		bool		continuescan;

		Assert(all_required_satisfied);

		/* Recheck _bt_check_compare on behalf of caller */
		if (_bt_check_compare(scan, dir, tuple, tupnatts, tupdesc,
							  false, false, false,
							  &continuescan, &nsktrig) &&
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
	 * by all_required_satisfied. (Actually, oppodir_inequality_sktrig trigger
	 * scan keys are tracked by all_required_satisfied, since it's convenient
	 * for _bt_check_compare to behave as if they are required in the current
	 * scan direction to deal with NULLs.  We'll account for that separately.)
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
	 * being satisfied when we go on to check it against tuples from this
	 * page's right sibling leaf page.  We consider truncated attributes to be
	 * satisfied by required scan keys, which allows the primitive index scan
	 * to continue to the next leaf page.  We must set so->scanBehind to true
	 * to remember that the last page's finaltup had "satisfied" required scan
	 * keys for one or more truncated attribute values (scan keys required in
	 * _either_ scan direction).
	 *
	 * There is a chance that _bt_checkkeys (which checks so->scanBehind) will
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
	 * You can think of this as a speculative bet on what the scan is likely
	 * to find on the next page.  It's not much of a gamble, though, since the
	 * untruncated prefix of attributes must strictly satisfy the new qual
	 * (though it's okay if any non-required scan keys fail to be satisfied).
	 */
	if (so->scanBehind && has_required_opposite_direction_only)
	{
		/*
		 * However, we need to work harder whenever the scan involves a scan
		 * key required in the opposite direction to the scan only, along with
		 * a finaltup with at least one truncated attribute that's associated
		 * with a scan key marked required (required in either direction).
		 *
		 * _bt_check_compare simply won't stop the scan for a scan key that's
		 * marked required in the opposite scan direction only.  That leaves
		 * us without an automatic way of reconsidering any opposite-direction
		 * inequalities if it turns out that starting a new primitive index
		 * scan will allow _bt_first to skip ahead by a great many leaf pages.
		 *
		 * We deal with this by explicitly scheduling a finaltup recheck on
		 * the right sibling page.  _bt_readpage calls _bt_oppodir_checkkeys
		 * for next page's finaltup (and we skip it for this page's finaltup).
		 */
		so->oppositeDirCheck = true;	/* recheck next page's high key */
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
	 * with NULL suffix values.  We need a separate test for this case so that
	 * we don't miss our only opportunity to skip over such a group of pages.
	 * (_bt_first is expected to skip over the group of NULLs by applying a
	 * similar "deduce NOT NULL" rule, where it finishes its insertion scan
	 * key by consing up an explicit SK_SEARCHNOTNULL key.)
	 *
	 * Apply a test against finaltup to detect and recover from the problem:
	 * if even finaltup doesn't satisfy such an inequality, we just skip by
	 * starting a new primitive index scan.  When we skip, we know for sure
	 * that all of the tuples on the current page following caller's tuple are
	 * also before the _bt_first-wise start of tuples for our new qual.  That
	 * at least suggests many more skippable pages beyond the current page.
	 * (when so->oppositeDirCheck was set, this'll happen on the next page.)
	 */
	else if (has_required_opposite_direction_only && pstate->finaltup &&
			 (all_required_satisfied || oppodir_inequality_sktrig) &&
			 unlikely(!_bt_oppodir_checkkeys(scan, dir, pstate->finaltup)))
	{
		/*
		 * Make sure that any non-required arrays are set to the first array
		 * element for the current scan direction
		 */
		_bt_rewind_nonrequired_arrays(scan, dir);
		goto new_prim_scan;
	}

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
		/* Optimization: skip by setting "look ahead" mechanism's offnum */
		Assert(ScanDirectionIsForward(dir));
		pstate->skip = pstate->maxoff + 1;
	}

	/* Caller's tuple doesn't match the new qual */
	return false;

new_prim_scan:

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
	so->needPrimScan = false;	/* ...don't call _bt_first again, though */

	/* Caller's tuple doesn't match any qual */
	return false;
}

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
 * This can be seen to be correct by considering the above example.  Note
 * in particular that if there are no keys for a given attribute, the keys for
 * subsequent attributes can never be required; for instance "WHERE y = 4"
 * requires a full-index scan.
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
 * Row comparison keys are currently also treated without any smarts:
 * we just transfer them into the preprocessed array without any
 * editorialization.  We can treat them the same as an ordinary inequality
 * comparison on the row's first index column, for the purposes of the logic
 * about required keys.
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
		 *
		 * If there are array scan keys in so->keyData[], then the now-current
		 * array elements must already be present in each array's scan key.
		 * Verify that that happened using an assertion.
		 */
		Assert(_bt_verify_keys_with_arraykeys(scan));
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

		/* Also maintain keyDataMap for remapping so->orderProc[] later */
		keyDataMap = MemoryContextAlloc(so->arrayContext,
										numberOfKeys * sizeof(int));
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
			 * redundant.  If we have a case like key = 1 AND key > 2, we can
			 * set qual_ok to false and abandon further processing.
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
						Assert(!array || array->num_elems > 0);
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
				}
				else if (xform[j].inkey->sk_flags & SK_SEARCHARRAY)
				{
					array = &so->arrayKeys[xform[j].arrayidx - 1];
					orderproc = so->orderProcs + xform[j].inkeyi;

					Assert(array->scan_key == xform[j].inkeyi);
					Assert(OidIsValid(orderproc->fn_oid));
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
					Assert(!array || array->num_elems > 0);

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

		if (array->num_elems <= 0)
			return false;

		if (cur->sk_argument != array->elem_values[array->cur_elem])
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

	/*
	 * First, deal with cases where one or both args are NULL.  This should
	 * only happen when the scankeys represent IS NULL/NOT NULL conditions.
	 */
	if ((leftarg->sk_flags | rightarg->sk_flags) & SK_ISNULL)
	{
		bool		leftnull,
					rightnull;

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
 * Test whether an indextuple satisfies all the scankey conditions.
 *
 * Return true if so, false if not.  If the tuple fails to pass the qual,
 * we also determine whether there's any need to continue the scan beyond
 * this tuple, and set pstate.continuescan accordingly.  See comments for
 * _bt_preprocess_keys(), above, about how this is done.
 *
 * Forward scan callers can pass a high key tuple in the hopes of having
 * us set *continuescan to false, and avoiding an unnecessary visit to
 * the page to the right.
 *
 * Advances the scan's array keys when necessary for arrayKeys=true callers.
 * Caller can avoid all array related side-effects when calling just to do a
 * page continuescan precheck -- pass arrayKeys=false for that.  Scans without
 * any arrays keys must always pass arrayKeys=false.
 *
 * Also stops and starts primitive index scans for arrayKeys=true callers.
 * Scans with array keys are required to set up page state that helps us with
 * this.  The page's finaltup tuple (the page high key for a forward scan, or
 * the page's first non-pivot tuple for a backward scan) must be set in
 * pstate.finaltup ahead of the first call here for the page (or possibly the
 * first call after an initial continuescan-setting page precheck call).  Set
 * this to NULL for rightmost page (or the leftmost page for backwards scans).
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
	int			ikey = 0;
	bool		res;

	Assert(BTreeTupleGetNAtts(tuple, scan->indexRelation) == tupnatts);

	res = _bt_check_compare(scan, dir, tuple, tupnatts, tupdesc,
							arrayKeys, pstate->prechecked, pstate->firstmatch,
							&pstate->continuescan, &ikey);

#ifdef USE_ASSERT_CHECKING
	if (!arrayKeys && so->numArrayKeys)
	{
		/*
		 * This is a continuescan precheck call for a scan with array keys.
		 *
		 * Assert that the scan isn't in danger of becoming confused.
		 */
		Assert(!so->scanBehind && !so->oppositeDirCheck);
		Assert(!pstate->prechecked && !pstate->firstmatch);
		Assert(!_bt_tuple_before_array_skeys(scan, dir, tuple, tupdesc,
											 tupnatts, false, 0, NULL));
	}
	if (pstate->prechecked || pstate->firstmatch)
	{
		bool		dcontinuescan;
		int			dikey = 0;

		/*
		 * Call relied on continuescan/firstmatch prechecks -- assert that we
		 * get the same answer without those optimizations
		 */
		Assert(res == _bt_check_compare(scan, dir, tuple, tupnatts, tupdesc,
										false, false, false,
										&dcontinuescan, &dikey));
		Assert(pstate->continuescan == dcontinuescan);
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
	if (_bt_tuple_before_array_skeys(scan, dir, tuple, tupdesc, tupnatts, true,
									 ikey, NULL))
	{
		/*
		 * Tuple is still before the start of matches according to the scan's
		 * required array keys (according to _all_ of its required equality
		 * strategy keys, actually).
		 *
		 * _bt_advance_array_keys occasionally sets so->scanBehind to signal
		 * that the scan's current position/tuples might be significantly
		 * behind (multiple pages behind) its current array keys.  When this
		 * happens, we need to be prepared to recover by starting a new
		 * primitive index scan here, on our own.
		 */
		Assert(!so->scanBehind ||
			   so->keyData[ikey].sk_strategy == BTEqualStrategyNumber);
		if (unlikely(so->scanBehind) && pstate->finaltup &&
			_bt_tuple_before_array_skeys(scan, dir, pstate->finaltup, tupdesc,
										 BTreeTupleGetNAtts(pstate->finaltup,
															scan->indexRelation),
										 false, 0, NULL))
		{
			/* Cut our losses -- start a new primitive index scan now */
			pstate->continuescan = false;
			so->needPrimScan = true;
		}
		else
		{
			/* Override _bt_check_compare, continue primitive scan */
			pstate->continuescan = true;

			/*
			 * We will end up here repeatedly given a group of tuples > the
			 * previous array keys and < the now-current keys (for a backwards
			 * scan it's just the same, though the operators swap positions).
			 *
			 * We must avoid allowing this linear search process to scan very
			 * many tuples from well before the start of tuples matching the
			 * current array keys (or from well before the point where we'll
			 * once again have to advance the scan's array keys).
			 *
			 * We keep the overhead under control by speculatively "looking
			 * ahead" to later still-unscanned items from this same leaf page.
			 * We'll only attempt this once the number of tuples that the
			 * linear search process has examined starts to get out of hand.
			 */
			pstate->rechecks++;
			if (pstate->rechecks >= LOOK_AHEAD_REQUIRED_RECHECKS)
			{
				/* See if we should skip ahead within the current leaf page */
				_bt_checkkeys_look_ahead(scan, pstate, tupnatts, tupdesc);

				/*
				 * Might have set pstate.skip to a later page offset.  When
				 * that happens then _bt_readpage caller will inexpensively
				 * skip ahead to a later tuple from the same page (the one
				 * just after the tuple we successfully "looked ahead" to).
				 */
			}
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
bool
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

	_bt_check_compare(scan, flipped, finaltup, nfinaltupatts, tupdesc,
					  false, false, false, &continuescan, &ikey);

	if (!continuescan && so->keyData[ikey].sk_strategy != BTEqualStrategyNumber)
		return false;

	return true;
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
 * Though we advance non-required array keys on our own, that shouldn't have
 * any lasting consequences for the scan.  By definition, non-required arrays
 * have no fixed relationship with the scan's progress.  (There are delicate
 * considerations for non-required arrays when the arrays need to be advanced
 * following our setting continuescan to false, but that doesn't concern us.)
 *
 * Pass advancenonrequired=false to avoid all array related side effects.
 * This allows _bt_advance_array_keys caller to avoid infinite recursion.
 */
static bool
_bt_check_compare(IndexScanDesc scan, ScanDirection dir,
				  IndexTuple tuple, int tupnatts, TupleDesc tupdesc,
				  bool advancenonrequired, bool prechecked, bool firstmatch,
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
		 * opposite scan direction _only_, or in neither direction
		 */
		if (((key->sk_flags & SK_BT_REQFWD) && ScanDirectionIsForward(dir)) ||
			((key->sk_flags & SK_BT_REQBKWD) && ScanDirectionIsBackward(dir)))
			requiredSameDir = true;
		else if (((key->sk_flags & SK_BT_REQFWD) && ScanDirectionIsBackward(dir)) ||
				 ((key->sk_flags & SK_BT_REQBKWD) && ScanDirectionIsForward(dir)))
			requiredOppositeDirOnly = true;

		/*
		 * If the caller told us the *continuescan flag is known to be true
		 * for the last item on the page, then we know the keys required for
		 * the current direction scan should be matched.  Otherwise, the
		 * *continuescan flag would be set for the current item and
		 * subsequently the last item on the page accordingly.
		 *
		 * If the key is required for the opposite direction scan, we can skip
		 * the check if the caller tells us there was already at least one
		 * matching item on the page. Also, we require the *continuescan flag
		 * to be true for the last item on the page to know there are no
		 * NULLs.
		 *
		 * Both cases above work except for the row keys, where NULLs could be
		 * found in the middle of matching values.
		 */
		if (prechecked &&
			(requiredSameDir || (requiredOppositeDirOnly && firstmatch)) &&
			!(key->sk_flags & SK_ROW_HEADER))
			continue;

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

		/* row-comparison keys need special processing */
		if (key->sk_flags & SK_ROW_HEADER)
		{
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

			/*
			 * In any case, this indextuple doesn't match the qual.
			 */
			return false;
		}

		if (isNull)
		{
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
				if ((key->sk_flags & (SK_BT_REQFWD | SK_BT_REQBKWD)) &&
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
				if ((key->sk_flags & (SK_BT_REQFWD | SK_BT_REQBKWD)) &&
					ScanDirectionIsForward(dir))
					*continuescan = false;
			}

			/*
			 * In any case, this indextuple doesn't match the qual.
			 */
			return false;
		}

		/*
		 * Apply the key-checking function, though only if we must.
		 *
		 * When a key is required in the opposite-of-scan direction _only_,
		 * then it must already be satisfied if firstmatch=true indicates that
		 * an earlier tuple from this same page satisfied it earlier on.
		 */
		if (!(requiredOppositeDirOnly && firstmatch) &&
			!DatumGetBool(FunctionCall2Coll(&key->sk_func, key->sk_collation,
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
			 * Unlike the simple-scankey case, this isn't a disallowed case.
			 * But it can never match.  If all the earlier row comparison
			 * columns are required for the scan direction, we can stop the
			 * scan, because there can't be another tuple that will succeed.
			 */
			if (subkey != (ScanKey) DatumGetPointer(skey->sk_argument))
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
			elog(ERROR, "unrecognized RowCompareType: %d",
				 (int) subkey->sk_strategy);
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
 * any items from the page, and so there is no need to search left from the
 * recorded offset.  (This observation also guarantees that the item is still
 * the right one to delete, which might otherwise be questionable since heap
 * TIDs can get recycled.)	This holds true even if the page has been modified
 * by inserts and page splits, so there is no need to consult the LSN.
 *
 * If the pin was released after reading the page, then we re-read it.  If it
 * has been modified since we read it (as determined by the LSN), we dare not
 * flag any entries because it is possible that the old entry was vacuumed
 * away and the TID was re-used by a completely different heap tuple.
 */
void
_bt_killitems(IndexScanDesc scan)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	Page		page;
	BTPageOpaque opaque;
	OffsetNumber minoff;
	OffsetNumber maxoff;
	int			i;
	int			numKilled = so->numKilled;
	bool		killedsomething = false;
	bool		droppedpin PG_USED_FOR_ASSERTS_ONLY;

	Assert(BTScanPosIsValid(so->currPos));

	/*
	 * Always reset the scan state, so we don't look for same items on other
	 * pages.
	 */
	so->numKilled = 0;

	if (BTScanPosIsPinned(so->currPos))
	{
		/*
		 * We have held the pin on this page since we read the index tuples,
		 * so all we need to do is lock it.  The pin will have prevented
		 * re-use of any TID on the page, so there is no need to check the
		 * LSN.
		 */
		droppedpin = false;
		_bt_lockbuf(scan->indexRelation, so->currPos.buf, BT_READ);

		page = BufferGetPage(so->currPos.buf);
	}
	else
	{
		Buffer		buf;

		droppedpin = true;
		/* Attempt to re-read the buffer, getting pin and lock. */
		buf = _bt_getbuf(scan->indexRelation, so->currPos.currPage, BT_READ);

		page = BufferGetPage(buf);
		if (BufferGetLSNAtomic(buf) == so->currPos.lsn)
			so->currPos.buf = buf;
		else
		{
			/* Modified while not pinned means hinting is not safe. */
			_bt_relbuf(scan->indexRelation, buf);
			return;
		}
	}

	opaque = BTPageGetOpaque(page);
	minoff = P_FIRSTDATAKEY(opaque);
	maxoff = PageGetMaxOffsetNumber(page);

	for (i = 0; i < numKilled; i++)
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
				 * since we first read it (in the !droppedpin case), so it's
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
					Assert(kitem->indexOffset == offnum || !droppedpin);

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

	_bt_unlockbuf(scan->indexRelation, so->currPos.buf);
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
		Form_pg_attribute att;

		datum1 = index_getattr(lastleft, attnum, itupdesc, &isNull1);
		datum2 = index_getattr(firstright, attnum, itupdesc, &isNull2);
		att = TupleDescAttr(itupdesc, attnum - 1);

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
	if (itemsz <= BTMaxItemSize(page))
		return;

	/*
	 * Tuple is probably too large to fit on page, but it's possible that the
	 * index uses version 2 or version 3, or that page is an internal page, in
	 * which case a slightly higher limit applies.
	 */
	if (!needheaptidspace && itemsz <= BTMaxItemSizeNoHeapTid(page))
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
					needheaptidspace ? BTMaxItemSize(page) :
					BTMaxItemSizeNoHeapTid(page),
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
