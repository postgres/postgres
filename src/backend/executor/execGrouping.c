/*-------------------------------------------------------------------------
 *
 * execGrouping.c
 *	  executor utility routines for grouping, hashing, and aggregation
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/execGrouping.c,v 1.8 2003/08/19 01:13:40 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "access/heapam.h"
#include "executor/executor.h"
#include "parser/parse_oper.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static TupleHashTable CurTupleHashTable = NULL;

static uint32 TupleHashTableHash(const void *key, Size keysize);
static int	TupleHashTableMatch(const void *key1, const void *key2,
								Size keysize);


/*****************************************************************************
 *		Utility routines for grouping tuples together
 *****************************************************************************/

/*
 * execTuplesMatch
 *		Return true if two tuples match in all the indicated fields.
 *
 * This actually implements SQL's notion of "not distinct".  Two nulls
 * match, a null and a not-null don't match.
 *
 * tuple1, tuple2: the tuples to compare
 * tupdesc: tuple descriptor applying to both tuples
 * numCols: the number of attributes to be examined
 * matchColIdx: array of attribute column numbers
 * eqFunctions: array of fmgr lookup info for the equality functions to use
 * evalContext: short-term memory context for executing the functions
 *
 * NB: evalContext is reset each time!
 */
bool
execTuplesMatch(HeapTuple tuple1,
				HeapTuple tuple2,
				TupleDesc tupdesc,
				int numCols,
				AttrNumber *matchColIdx,
				FmgrInfo *eqfunctions,
				MemoryContext evalContext)
{
	MemoryContext oldContext;
	bool		result;
	int			i;

	/* Reset and switch into the temp context. */
	MemoryContextReset(evalContext);
	oldContext = MemoryContextSwitchTo(evalContext);

	/*
	 * We cannot report a match without checking all the fields, but we
	 * can report a non-match as soon as we find unequal fields.  So,
	 * start comparing at the last field (least significant sort key).
	 * That's the most likely to be different if we are dealing with
	 * sorted input.
	 */
	result = true;

	for (i = numCols; --i >= 0;)
	{
		AttrNumber	att = matchColIdx[i];
		Datum		attr1,
					attr2;
		bool		isNull1,
					isNull2;

		attr1 = heap_getattr(tuple1,
							 att,
							 tupdesc,
							 &isNull1);

		attr2 = heap_getattr(tuple2,
							 att,
							 tupdesc,
							 &isNull2);

		if (isNull1 != isNull2)
		{
			result = false;		/* one null and one not; they aren't equal */
			break;
		}

		if (isNull1)
			continue;			/* both are null, treat as equal */

		/* Apply the type-specific equality function */

		if (!DatumGetBool(FunctionCall2(&eqfunctions[i],
										attr1, attr2)))
		{
			result = false;		/* they aren't equal */
			break;
		}
	}

	MemoryContextSwitchTo(oldContext);

	return result;
}

/*
 * execTuplesUnequal
 *		Return true if two tuples are definitely unequal in the indicated
 *		fields.
 *
 * Nulls are neither equal nor unequal to anything else.  A true result
 * is obtained only if there are non-null fields that compare not-equal.
 *
 * Parameters are identical to execTuplesMatch.
 */
bool
execTuplesUnequal(HeapTuple tuple1,
				  HeapTuple tuple2,
				  TupleDesc tupdesc,
				  int numCols,
				  AttrNumber *matchColIdx,
				  FmgrInfo *eqfunctions,
				  MemoryContext evalContext)
{
	MemoryContext oldContext;
	bool		result;
	int			i;

	/* Reset and switch into the temp context. */
	MemoryContextReset(evalContext);
	oldContext = MemoryContextSwitchTo(evalContext);

	/*
	 * We cannot report a match without checking all the fields, but we
	 * can report a non-match as soon as we find unequal fields.  So,
	 * start comparing at the last field (least significant sort key).
	 * That's the most likely to be different if we are dealing with
	 * sorted input.
	 */
	result = false;

	for (i = numCols; --i >= 0;)
	{
		AttrNumber	att = matchColIdx[i];
		Datum		attr1,
					attr2;
		bool		isNull1,
					isNull2;

		attr1 = heap_getattr(tuple1,
							 att,
							 tupdesc,
							 &isNull1);

		if (isNull1)
			continue;			/* can't prove anything here */

		attr2 = heap_getattr(tuple2,
							 att,
							 tupdesc,
							 &isNull2);

		if (isNull2)
			continue;			/* can't prove anything here */

		/* Apply the type-specific equality function */

		if (!DatumGetBool(FunctionCall2(&eqfunctions[i],
										attr1, attr2)))
		{
			result = true;		/* they are unequal */
			break;
		}
	}

	MemoryContextSwitchTo(oldContext);

	return result;
}


/*
 * execTuplesMatchPrepare
 *		Look up the equality functions needed for execTuplesMatch or
 *		execTuplesUnequal.
 *
 * The result is a palloc'd array.
 */
FmgrInfo *
execTuplesMatchPrepare(TupleDesc tupdesc,
					   int numCols,
					   AttrNumber *matchColIdx)
{
	FmgrInfo   *eqfunctions = (FmgrInfo *) palloc(numCols * sizeof(FmgrInfo));
	int			i;

	for (i = 0; i < numCols; i++)
	{
		AttrNumber	att = matchColIdx[i];
		Oid			typid = tupdesc->attrs[att - 1]->atttypid;
		Oid			eq_function;

		eq_function = equality_oper_funcid(typid);
		fmgr_info(eq_function, &eqfunctions[i]);
	}

	return eqfunctions;
}

/*
 * execTuplesHashPrepare
 *		Look up the equality and hashing functions needed for a TupleHashTable.
 *
 * This is similar to execTuplesMatchPrepare, but we also need to find the
 * hash functions associated with the equality operators.  *eqfunctions and
 * *hashfunctions receive the palloc'd result arrays.
 */
void
execTuplesHashPrepare(TupleDesc tupdesc,
					  int numCols,
					  AttrNumber *matchColIdx,
					  FmgrInfo **eqfunctions,
					  FmgrInfo **hashfunctions)
{
	int			i;

	*eqfunctions = (FmgrInfo *) palloc(numCols * sizeof(FmgrInfo));
	*hashfunctions = (FmgrInfo *) palloc(numCols * sizeof(FmgrInfo));

	for (i = 0; i < numCols; i++)
	{
		AttrNumber	att = matchColIdx[i];
		Oid			typid = tupdesc->attrs[att - 1]->atttypid;
		Operator	optup;
		Oid			eq_opr;
		Oid			eq_function;
		Oid			hash_function;

		optup = equality_oper(typid, false);
		eq_opr = oprid(optup);
		eq_function = oprfuncid(optup);
		ReleaseSysCache(optup);
		hash_function = get_op_hash_function(eq_opr);
		if (!OidIsValid(hash_function)) /* should not happen */
			elog(ERROR, "could not find hash function for hash operator %u",
				 eq_opr);
		fmgr_info(eq_function, &(*eqfunctions)[i]);
		fmgr_info(hash_function, &(*hashfunctions)[i]);
	}
}


/*****************************************************************************
 *		Utility routines for all-in-memory hash tables
 *
 * These routines build hash tables for grouping tuples together (eg, for
 * hash aggregation).  There is one entry for each not-distinct set of tuples
 * presented.
 *****************************************************************************/

/*
 * Construct an empty TupleHashTable
 *
 *	numCols, keyColIdx: identify the tuple fields to use as lookup key
 *	eqfunctions: equality comparison functions to use
 *	hashfunctions: datatype-specific hashing functions to use
 *	nbuckets: initial estimate of hashtable size
 *	entrysize: size of each entry (at least sizeof(TupleHashEntryData))
 *	tablecxt: memory context in which to store table and table entries
 *	tempcxt: short-lived context for evaluation hash and comparison functions
 *
 * The function arrays may be made with execTuplesHashPrepare().
 *
 * Note that keyColIdx, eqfunctions, and hashfunctions must be allocated in
 * storage that will live as long as the hashtable does.
 */
TupleHashTable
BuildTupleHashTable(int numCols, AttrNumber *keyColIdx,
					FmgrInfo *eqfunctions,
					FmgrInfo *hashfunctions,
					int nbuckets, Size entrysize,
					MemoryContext tablecxt, MemoryContext tempcxt)
{
	TupleHashTable hashtable;
	HASHCTL		hash_ctl;

	Assert(nbuckets > 0);
	Assert(entrysize >= sizeof(TupleHashEntryData));

	hashtable = (TupleHashTable) MemoryContextAlloc(tablecxt,
												sizeof(TupleHashTableData));

	hashtable->numCols = numCols;
	hashtable->keyColIdx = keyColIdx;
	hashtable->eqfunctions = eqfunctions;
	hashtable->hashfunctions = hashfunctions;
	hashtable->tablecxt = tablecxt;
	hashtable->tempcxt = tempcxt;
	hashtable->entrysize = entrysize;

	MemSet(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(TupleHashEntryData);
	hash_ctl.entrysize = entrysize;
	hash_ctl.hash = TupleHashTableHash;
	hash_ctl.match = TupleHashTableMatch;
	hash_ctl.hcxt = tablecxt;
	hashtable->hashtab = hash_create("TupleHashTable", (long) nbuckets,
									 &hash_ctl,
					HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_CONTEXT);
	if (hashtable->hashtab == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	return hashtable;
}

/*
 * Find or create a hashtable entry for the tuple group containing the
 * given tuple.
 *
 * If isnew is NULL, we do not create new entries; we return NULL if no
 * match is found.
 *
 * If isnew isn't NULL, then a new entry is created if no existing entry
 * matches.  On return, *isnew is true if the entry is newly created,
 * false if it existed already.  Any extra space in a new entry has been
 * zeroed.
 */
TupleHashEntry
LookupTupleHashEntry(TupleHashTable hashtable, TupleTableSlot *slot,
					 bool *isnew)
{
	HeapTuple	tuple = slot->val;
	TupleDesc	tupdesc = slot->ttc_tupleDescriptor;
	TupleHashEntry entry;
	MemoryContext oldContext;
	TupleHashTable saveCurHT;
	bool		found;

	/* Need to run the hash functions in short-lived context */
	oldContext = MemoryContextSwitchTo(hashtable->tempcxt);

	/*
	 * Set up data needed by hash and match functions
	 *
	 * We save and restore CurTupleHashTable just in case someone manages
	 * to invoke this code re-entrantly.
	 */
	hashtable->tupdesc = tupdesc;
	saveCurHT = CurTupleHashTable;
	CurTupleHashTable = hashtable;

	/* Search the hash table */
	entry = (TupleHashEntry) hash_search(hashtable->hashtab,
										 &tuple,
										 isnew ? HASH_ENTER : HASH_FIND,
										 &found);

	if (isnew)
	{
		if (found)
		{
			/* found pre-existing entry */
			*isnew = false;
		}
		else
		{
			/* created new entry ... we hope */
			if (entry == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory")));

			/*
			 * Zero any caller-requested space in the entry.  (This zaps
			 * the "key data" dynahash.c copied into the new entry, but
			 * we don't care since we're about to overwrite it anyway.)
			 */
			MemSet(entry, 0, hashtable->entrysize);

			/* Copy the first tuple into the table context */
			MemoryContextSwitchTo(hashtable->tablecxt);
			entry->firstTuple = heap_copytuple(tuple);

			*isnew = true;
		}
	}

	CurTupleHashTable = saveCurHT;

	MemoryContextSwitchTo(oldContext);

	return entry;
}

/*
 * Compute the hash value for a tuple
 *
 * The passed-in key is a pointer to a HeapTuple pointer -- this is either
 * the firstTuple field of a TupleHashEntry struct, or the key value passed
 * to hash_search.  We ignore the keysize.
 *
 * CurTupleHashTable must be set before calling this, since dynahash.c
 * doesn't provide any API that would let us get at the hashtable otherwise.
 *
 * Also, the caller must select an appropriate memory context for running
 * the hash functions.  (dynahash.c doesn't change CurrentMemoryContext.)
 */
static uint32
TupleHashTableHash(const void *key, Size keysize)
{
	HeapTuple	tuple = *(const HeapTuple *) key;
	TupleHashTable hashtable = CurTupleHashTable;
	int			numCols = hashtable->numCols;
	AttrNumber *keyColIdx = hashtable->keyColIdx;
	TupleDesc	tupdesc = hashtable->tupdesc;
	uint32		hashkey = 0;
	int			i;

	for (i = 0; i < numCols; i++)
	{
		AttrNumber	att = keyColIdx[i];
		Datum		attr;
		bool		isNull;

		/* rotate hashkey left 1 bit at each step */
		hashkey = (hashkey << 1) | ((hashkey & 0x80000000) ? 1 : 0);

		attr = heap_getattr(tuple, att, tupdesc, &isNull);

		if (!isNull)			/* treat nulls as having hash key 0 */
		{
			uint32		hkey;

			hkey = DatumGetUInt32(FunctionCall1(&hashtable->hashfunctions[i],
												attr));
			hashkey ^= hkey;
		}
	}

	return hashkey;
}

/*
 * See whether two tuples (presumably of the same hash value) match
 *
 * As above, the passed pointers are pointers to HeapTuple pointers.
 *
 * CurTupleHashTable must be set before calling this, since dynahash.c
 * doesn't provide any API that would let us get at the hashtable otherwise.
 *
 * Also, the caller must select an appropriate memory context for running
 * the compare functions.  (dynahash.c doesn't change CurrentMemoryContext.)
 */
static int
TupleHashTableMatch(const void *key1, const void *key2, Size keysize)
{
	HeapTuple	tuple1 = *(const HeapTuple *) key1;
	HeapTuple	tuple2 = *(const HeapTuple *) key2;
	TupleHashTable hashtable = CurTupleHashTable;

	if (execTuplesMatch(tuple1,
						tuple2,
						hashtable->tupdesc,
						hashtable->numCols,
						hashtable->keyColIdx,
						hashtable->eqfunctions,
						hashtable->tempcxt))
		return 0;
	else
		return 1;
}
