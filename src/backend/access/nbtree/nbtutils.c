/*-------------------------------------------------------------------------
 *
 * btutils.c
 *	  Utility code for Postgres btree implementation.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/nbtree/nbtutils.c,v 1.54 2003/08/04 02:39:57 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/istrat.h"
#include "access/nbtree.h"
#include "catalog/catalog.h"
#include "executor/execdebug.h"


static int	_bt_getstrategynumber(RegProcedure sk_procedure, StrategyMap map);


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
	FmgrInfo   *procinfo;
	Datum		arg;
	bool		null;
	bits16		flag;

	itupdesc = RelationGetDescr(rel);
	natts = RelationGetNumberOfAttributes(rel);

	skey = (ScanKey) palloc(natts * sizeof(ScanKeyData));

	for (i = 0; i < natts; i++)
	{
		procinfo = index_getprocinfo(rel, i + 1, BTORDER_PROC);
		arg = index_getattr(itup, i + 1, itupdesc, &null);
		flag = null ? SK_ISNULL : 0x0;
		ScanKeyEntryInitializeWithInfo(&skey[i],
									   flag,
									   (AttrNumber) (i + 1),
									   procinfo,
									   CurrentMemoryContext,
									   arg);
	}

	return skey;
}

/*
 * _bt_mkscankey_nodata
 *		Build a scan key that contains comparator routines appropriate to
 *		the key datatypes, but no comparison data.
 *
 *		The result cannot be used with _bt_compare().  Currently this
 *		routine is only called by utils/sort/tuplesort.c, which has its
 *		own comparison routine.
 */
ScanKey
_bt_mkscankey_nodata(Relation rel)
{
	ScanKey		skey;
	int			natts;
	int			i;
	FmgrInfo   *procinfo;

	natts = RelationGetNumberOfAttributes(rel);

	skey = (ScanKey) palloc(natts * sizeof(ScanKeyData));

	for (i = 0; i < natts; i++)
	{
		procinfo = index_getprocinfo(rel, i + 1, BTORDER_PROC);
		ScanKeyEntryInitializeWithInfo(&skey[i],
									   SK_ISNULL,
									   (AttrNumber) (i + 1),
									   procinfo,
									   CurrentMemoryContext,
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

	while (stack != (BTStack) NULL)
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
 *	_bt_orderkeys() -- Put keys in a sensible order for conjunctive quals.
 *
 * After this routine runs, the scan keys are ordered by index attribute
 * (all quals for attr 1, then all for attr 2, etc) and within each attr
 * the keys are ordered by constraint type: ">", ">=", "=", "<=", "<".
 * Furthermore, redundant keys are eliminated: we keep only the tightest
 * >/>= bound and the tightest </<= bound, and if there's an = key then
 * that's the only one returned.  (So, we return either a single = key,
 * or one or two boundary-condition keys for each attr.)
 *
 * As a byproduct of this work, we can detect contradictory quals such
 * as "x = 1 AND x > 2".  If we see that, we return so->quals_ok = FALSE,
 * indicating the scan need not be run at all since no tuples can match.
 *
 * Another byproduct is to determine how many quals must be satisfied to
 * continue the scan.  _bt_checkkeys uses this.  For example, if the quals
 * are "x = 1 AND y < 4 AND z < 5", then _bt_checkkeys will reject a tuple
 * (1,2,7), but we must continue the scan in case there are tuples (1,3,z).
 * But once we reach tuples like (1,4,z) we can stop scanning because no
 * later tuples could match.  This is reflected by setting
 * so->numberOfRequiredKeys to the number of leading keys that must be
 * matched to continue the scan.  numberOfRequiredKeys is equal to the
 * number of leading "=" keys plus the key(s) for the first non "="
 * attribute, which can be seen to be correct by considering the above
 * example.
 *
 * Furthermore, we detect the case where the index is unique and we have
 * equality quals for all columns.	In this case there can be at most one
 * (visible) matching tuple.  index_getnext uses this to avoid uselessly
 * continuing the scan after finding one match.
 *
 * The initial ordering of the keys is expected to be by attribute already
 * (see group_clauses_by_indexkey() in indxpath.c).  The task here is to
 * standardize the appearance of multiple keys for the same attribute.
 *
 * XXX this routine is one of many places that fail to handle SK_COMMUTE
 * scankeys properly.  Currently, the planner is careful never to generate
 * any indexquals that would require SK_COMMUTE to be set.	Someday we ought
 * to try to fix this, though it's not real critical as long as indexable
 * operators all have commutators...
 *
 * Note: this routine invokes comparison operators via OidFunctionCallN,
 * ie, without caching function lookups.  No point in trying to be smarter,
 * since these comparisons are executed only when the user expresses a
 * hokey qualification, and happen only once per scan anyway.
 *----------
 */
void
_bt_orderkeys(IndexScanDesc scan)
{
	Relation	relation = scan->indexRelation;
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	ScanKeyData xform[BTMaxStrategyNumber];
	bool		init[BTMaxStrategyNumber];
	int			numberOfKeys = so->numberOfKeys;
	ScanKey		key;
	ScanKey		cur;
	StrategyMap map;
	Datum		test;
	int			i,
				j;
	AttrNumber	attno;
	int			new_numberOfKeys;
	bool		allEqualSoFar;

	so->qual_ok = true;
	so->numberOfRequiredKeys = 0;
	scan->keys_are_unique = false;

	if (numberOfKeys < 1)
		return;					/* done if qual-less scan */

	key = so->keyData;
	cur = &key[0];
	/* check input keys are correctly ordered */
	if (cur->sk_attno != 1)
		elog(ERROR, "key(s) for attribute 1 missed");

	/* We can short-circuit most of the work if there's just one key */
	if (numberOfKeys == 1)
	{
		/*
		 * We don't use indices for 'A is null' and 'A is not null'
		 * currently and 'A < = > <> NULL' will always fail - so qual is
		 * not Ok if comparison value is NULL.		- vadim 03/21/97
		 */
		if (cur->sk_flags & SK_ISNULL)
			so->qual_ok = false;
		else if (relation->rd_index->indisunique &&
				 relation->rd_rel->relnatts == 1)
		{
			/* it's a unique index, do we have an equality qual? */
			map = IndexStrategyGetStrategyMap(RelationGetIndexStrategy(relation),
											  BTMaxStrategyNumber,
											  1);
			j = _bt_getstrategynumber(cur->sk_procedure, map);
			if (j == (BTEqualStrategyNumber - 1))
				scan->keys_are_unique = true;
		}
		so->numberOfRequiredKeys = 1;
		return;
	}

	/*
	 * Otherwise, do the full set of pushups.
	 */
	new_numberOfKeys = 0;
	allEqualSoFar = true;

	/*
	 * Initialize for processing of keys for attr 1.
	 *
	 * xform[i] holds a copy of the current scan key of strategy type i+1, if
	 * any; init[i] is TRUE if we have found such a key for this attr.
	 */
	attno = 1;
	map = IndexStrategyGetStrategyMap(RelationGetIndexStrategy(relation),
									  BTMaxStrategyNumber,
									  attno);
	MemSet(xform, 0, sizeof(xform));	/* not really necessary */
	MemSet(init, 0, sizeof(init));

	/*
	 * Loop iterates from 0 to numberOfKeys inclusive; we use the last
	 * pass to handle after-last-key processing.  Actual exit from the
	 * loop is at the "break" statement below.
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
		 * If we are at the end of the keys for a particular attr, finish
		 * up processing and emit the cleaned-up keys.
		 */
		if (i == numberOfKeys || cur->sk_attno != attno)
		{
			bool		priorAllEqualSoFar = allEqualSoFar;

			/* check input keys are correctly ordered */
			if (i < numberOfKeys && cur->sk_attno != attno + 1)
				elog(ERROR, "key(s) for attribute %d missed", attno + 1);

			/*
			 * If = has been specified, no other key will be used. In case
			 * of key > 2 && key == 1 and so on we have to set qual_ok to
			 * false before discarding the other keys.
			 */
			if (init[BTEqualStrategyNumber - 1])
			{
				ScanKeyData *eq,
						   *chk;

				eq = &xform[BTEqualStrategyNumber - 1];
				for (j = BTMaxStrategyNumber; --j >= 0;)
				{
					if (!init[j] ||
						j == (BTEqualStrategyNumber - 1))
						continue;
					chk = &xform[j];
					test = OidFunctionCall2(chk->sk_procedure,
											eq->sk_argument,
											chk->sk_argument);
					if (!DatumGetBool(test))
						so->qual_ok = false;
				}
				init[BTLessStrategyNumber - 1] = false;
				init[BTLessEqualStrategyNumber - 1] = false;
				init[BTGreaterEqualStrategyNumber - 1] = false;
				init[BTGreaterStrategyNumber - 1] = false;
			}
			else
			{
				/*
				 * No "=" for this key, so we're done with required keys
				 */
				allEqualSoFar = false;
			}

			/* keep only one of <, <= */
			if (init[BTLessStrategyNumber - 1]
				&& init[BTLessEqualStrategyNumber - 1])
			{
				ScanKeyData *lt = &xform[BTLessStrategyNumber - 1];
				ScanKeyData *le = &xform[BTLessEqualStrategyNumber - 1];

				test = OidFunctionCall2(le->sk_procedure,
										lt->sk_argument,
										le->sk_argument);
				if (DatumGetBool(test))
					init[BTLessEqualStrategyNumber - 1] = false;
				else
					init[BTLessStrategyNumber - 1] = false;
			}

			/* keep only one of >, >= */
			if (init[BTGreaterStrategyNumber - 1]
				&& init[BTGreaterEqualStrategyNumber - 1])
			{
				ScanKeyData *gt = &xform[BTGreaterStrategyNumber - 1];
				ScanKeyData *ge = &xform[BTGreaterEqualStrategyNumber - 1];

				test = OidFunctionCall2(ge->sk_procedure,
										gt->sk_argument,
										ge->sk_argument);
				if (DatumGetBool(test))
					init[BTGreaterEqualStrategyNumber - 1] = false;
				else
					init[BTGreaterStrategyNumber - 1] = false;
			}

			/*
			 * Emit the cleaned-up keys back into the key[] array in the
			 * correct order.  Note we are overwriting our input here!
			 * It's OK because (a) xform[] is a physical copy of the keys
			 * we want, (b) we cannot emit more keys than we input, so we
			 * won't overwrite as-yet-unprocessed keys.
			 */
			for (j = BTMaxStrategyNumber; --j >= 0;)
			{
				if (init[j])
					memcpy(&key[new_numberOfKeys++], &xform[j],
						   sizeof(ScanKeyData));
			}

			/*
			 * If all attrs before this one had "=", include these keys
			 * into the required-keys count.
			 */
			if (priorAllEqualSoFar)
				so->numberOfRequiredKeys = new_numberOfKeys;

			/*
			 * Exit loop here if done.
			 */
			if (i == numberOfKeys)
				break;

			/* Re-initialize for new attno */
			attno = cur->sk_attno;
			map = IndexStrategyGetStrategyMap(RelationGetIndexStrategy(relation),
											  BTMaxStrategyNumber,
											  attno);
			MemSet(xform, 0, sizeof(xform));	/* not really necessary */
			MemSet(init, 0, sizeof(init));
		}

		/* figure out which strategy this key's operator corresponds to */
		j = _bt_getstrategynumber(cur->sk_procedure, map);

		/* have we seen one of these before? */
		if (init[j])
		{
			/* yup, keep the more restrictive value */
			test = FunctionCall2(&cur->sk_func,
								 cur->sk_argument,
								 xform[j].sk_argument);
			if (DatumGetBool(test))
				xform[j].sk_argument = cur->sk_argument;
			else if (j == (BTEqualStrategyNumber - 1))
				so->qual_ok = false;
			/* key == a && key == b, but a != b */
		}
		else
		{
			/* nope, so remember this scankey */
			memcpy(&xform[j], cur, sizeof(ScanKeyData));
			init[j] = true;
		}
	}

	so->numberOfKeys = new_numberOfKeys;

	/*
	 * If unique index and we have equality keys for all columns, set
	 * keys_are_unique flag for higher levels.
	 */
	if (allEqualSoFar && relation->rd_index->indisunique &&
		relation->rd_rel->relnatts == new_numberOfKeys)
		scan->keys_are_unique = true;
}

/*
 * Determine which btree strategy an operator procedure matches.
 *
 * Result is strategy number minus 1.
 */
static int
_bt_getstrategynumber(RegProcedure sk_procedure, StrategyMap map)
{
	int			j;

	for (j = BTMaxStrategyNumber; --j >= 0;)
	{
		if (sk_procedure == map->entry[j].sk_procedure)
			return j;
	}
	elog(ERROR, "could not identify operator %u", sk_procedure);
	return -1;					/* keep compiler quiet */
}

/*
 * Test whether an indextuple satisfies all the scankey conditions.
 *
 * If the tuple fails to pass the qual, we also determine whether there's
 * any need to continue the scan beyond this tuple, and set *continuescan
 * accordingly.  See comments for _bt_orderkeys(), above, about how this is
 * done.
 */
bool
_bt_checkkeys(IndexScanDesc scan, IndexTuple tuple,
			  ScanDirection dir, bool *continuescan)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	int			keysz = so->numberOfKeys;
	int			keysok;
	TupleDesc	tupdesc;
	ScanKey		key;

	*continuescan = true;

	/* If no keys, always scan the whole index */
	if (keysz == 0)
		return true;

	tupdesc = RelationGetDescr(scan->indexRelation);
	key = so->keyData;
	keysok = 0;

	IncrIndexProcessed();

	while (keysz > 0)
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
			/* we shouldn't get here, really; see _bt_orderkeys() */
			*continuescan = false;
			return false;
		}

		if (isNull)
		{
			/*
			 * Since NULLs are sorted after non-NULLs, we know we have
			 * reached the upper limit of the range of values for this
			 * index attr.	On a forward scan, we can stop if this qual is
			 * one of the "must match" subset.	On a backward scan,
			 * however, we should keep going.
			 */
			if (keysok < so->numberOfRequiredKeys &&
				ScanDirectionIsForward(dir))
				*continuescan = false;

			/*
			 * In any case, this indextuple doesn't match the qual.
			 */
			return false;
		}

		if (key->sk_flags & SK_COMMUTE)
			test = FunctionCall2(&key->sk_func,
								 key->sk_argument, datum);
		else
			test = FunctionCall2(&key->sk_func,
								 datum, key->sk_argument);

		if (DatumGetBool(test) == !!(key->sk_flags & SK_NEGATE))
		{
			/*
			 * Tuple fails this qual.  If it's a required qual, then we
			 * can conclude no further tuples will pass, either.
			 */
			if (keysok < so->numberOfRequiredKeys)
				*continuescan = false;
			return false;
		}

		keysok++;
		key++;
		keysz--;
	}

	/* If we get here, the tuple passes all quals. */
	return true;
}
