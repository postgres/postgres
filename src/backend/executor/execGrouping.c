/*-------------------------------------------------------------------------
 *
 * execGrouping.c
 *	  executor utility routines for grouping, hashing, and aggregation
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/execGrouping.c,v 1.1 2003/01/10 23:54:24 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "access/heapam.h"
#include "executor/executor.h"
#include "parser/parse_oper.h"
#include "utils/memutils.h"


/*****************************************************************************
 *		Utility routines for grouping tuples together
 *
 * These routines actually implement SQL's notion of "distinct/not distinct".
 * Two tuples match if they are not distinct in all the compared columns,
 * i.e., the column values are either both null, or both non-null and equal.
 *****************************************************************************/

/*
 * execTuplesMatch
 *		Return true if two tuples match in all the indicated fields.
 *		This is used to detect group boundaries in nodeGroup and nodeAgg,
 *		and to decide whether two tuples are distinct or not in nodeUnique.
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
 * execTuplesMatchPrepare
 *		Look up the equality functions needed for execTuplesMatch.
 *		The result is a palloc'd array.
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


/*****************************************************************************
 *		Utility routines for hashing
 *****************************************************************************/

/*
 * ComputeHashFunc
 *
 *		the hash function for hash joins (also used for hash aggregation)
 *
 *		XXX this probably ought to be replaced with datatype-specific
 *		hash functions, such as those already implemented for hash indexes.
 */
uint32
ComputeHashFunc(Datum key, int typLen, bool byVal)
{
	unsigned char *k;

	if (byVal)
	{
		/*
		 * If it's a by-value data type, just hash the whole Datum value.
		 * This assumes that datatypes narrower than Datum are
		 * consistently padded (either zero-extended or sign-extended, but
		 * not random bits) to fill Datum; see the XXXGetDatum macros in
		 * postgres.h. NOTE: it would not work to do hash_any(&key, len)
		 * since this would get the wrong bytes on a big-endian machine.
		 */
		k = (unsigned char *) &key;
		typLen = sizeof(Datum);
	}
	else
	{
		if (typLen > 0)
		{
			/* fixed-width pass-by-reference type */
			k = (unsigned char *) DatumGetPointer(key);
		}
		else if (typLen == -1)
		{
			/*
			 * It's a varlena type, so 'key' points to a "struct varlena".
			 * NOTE: VARSIZE returns the "real" data length plus the
			 * sizeof the "vl_len" attribute of varlena (the length
			 * information). 'key' points to the beginning of the varlena
			 * struct, so we have to use "VARDATA" to find the beginning
			 * of the "real" data.	Also, we have to be careful to detoast
			 * the datum if it's toasted.  (We don't worry about freeing
			 * the detoasted copy; that happens for free when the
			 * per-tuple memory context is reset in ExecHashGetBucket.)
			 */
			struct varlena *vkey = PG_DETOAST_DATUM(key);

			typLen = VARSIZE(vkey) - VARHDRSZ;
			k = (unsigned char *) VARDATA(vkey);
		}
		else if (typLen == -2)
		{
			/* It's a null-terminated C string */
			typLen = strlen(DatumGetCString(key)) + 1;
			k = (unsigned char *) DatumGetPointer(key);
		}
		else
		{
			elog(ERROR, "ComputeHashFunc: Invalid typLen %d", typLen);
			k = NULL;			/* keep compiler quiet */
		}
	}

	return DatumGetUInt32(hash_any(k, typLen));
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
 *	nbuckets: number of buckets to make
 *	entrysize: size of each entry (at least sizeof(TupleHashEntryData))
 *	tablecxt: memory context in which to store table and table entries
 *	tempcxt: short-lived context for evaluation hash and comparison functions
 *
 * The eqfunctions array may be made with execTuplesMatchPrepare().
 *
 * Note that keyColIdx and eqfunctions must be allocated in storage that
 * will live as long as the hashtable does.
 */
TupleHashTable
BuildTupleHashTable(int numCols, AttrNumber *keyColIdx,
					FmgrInfo *eqfunctions,
					int nbuckets, Size entrysize,
					MemoryContext tablecxt, MemoryContext tempcxt)
{
	TupleHashTable	hashtable;
	Size			tabsize;

	Assert(nbuckets > 0);
	Assert(entrysize >= sizeof(TupleHashEntryData));

	tabsize = sizeof(TupleHashTableData) +
		(nbuckets - 1) * sizeof(TupleHashEntry);
	hashtable = (TupleHashTable) MemoryContextAllocZero(tablecxt, tabsize);

	hashtable->numCols = numCols;
	hashtable->keyColIdx = keyColIdx;
	hashtable->eqfunctions = eqfunctions;
	hashtable->tablecxt = tablecxt;
	hashtable->tempcxt = tempcxt;
	hashtable->entrysize = entrysize;
	hashtable->nbuckets = nbuckets;

	return hashtable;
}

/*
 * Find or create a hashtable entry for the tuple group containing the
 * given tuple.
 *
 * On return, *isnew is true if the entry is newly created, false if it
 * existed already.  Any extra space in a new entry has been zeroed.
 */
TupleHashEntry
LookupTupleHashEntry(TupleHashTable hashtable, TupleTableSlot *slot,
					 bool *isnew)
{
	int			numCols = hashtable->numCols;
	AttrNumber *keyColIdx = hashtable->keyColIdx;
	HeapTuple	tuple = slot->val;
	TupleDesc	tupdesc = slot->ttc_tupleDescriptor;
	uint32		hashkey = 0;
	int			i;
	int			bucketno;
	TupleHashEntry entry;
	MemoryContext oldContext;

	/* Need to run the hash function in short-lived context */
	oldContext = MemoryContextSwitchTo(hashtable->tempcxt);

	for (i = 0; i < numCols; i++)
	{
		AttrNumber	att = keyColIdx[i];
		Datum		attr;
		bool		isNull;

		/* rotate hashkey left 1 bit at each step */
		hashkey = (hashkey << 1) | ((hashkey & 0x80000000) ? 1 : 0);

		attr = heap_getattr(tuple, att, tupdesc, &isNull);
		if (isNull)
			continue;			/* treat nulls as having hash key 0 */
		hashkey ^= ComputeHashFunc(attr,
								   (int) tupdesc->attrs[att - 1]->attlen,
								   tupdesc->attrs[att - 1]->attbyval);
	}
	bucketno = hashkey % (uint32) hashtable->nbuckets;

	for (entry = hashtable->buckets[bucketno];
		 entry != NULL;
		 entry = entry->next)
	{
		/* Quick check using hashkey */
		if (entry->hashkey != hashkey)
			continue;
		if (execTuplesMatch(entry->firstTuple,
							tuple,
							tupdesc,
							numCols, keyColIdx,
							hashtable->eqfunctions,
							hashtable->tempcxt))
		{
			MemoryContextSwitchTo(oldContext);
			*isnew = false;
			return entry;
		}
	}

	/* Not there, so build a new one */
	MemoryContextSwitchTo(hashtable->tablecxt);

	entry = (TupleHashEntry) palloc0(hashtable->entrysize);

	entry->hashkey = hashkey;
	entry->firstTuple = heap_copytuple(tuple);

	entry->next = hashtable->buckets[bucketno];
	hashtable->buckets[bucketno] = entry;

	MemoryContextSwitchTo(oldContext);

	*isnew = true;

	return entry;
}

/*
 * Walk through all the entries of a hash table, in no special order.
 * Returns NULL when no more entries remain.
 *
 * Iterator state must be initialized with ResetTupleHashIterator() macro.
 */
TupleHashEntry
ScanTupleHashTable(TupleHashTable hashtable, TupleHashIterator *state)
{
	TupleHashEntry	entry;

	entry = state->next_entry;
	while (entry == NULL)
	{
		if (state->next_bucket >= hashtable->nbuckets)
		{
			/* No more entries in hashtable, so done */
			return NULL;
		}
		entry = hashtable->buckets[state->next_bucket++];
	}
	state->next_entry = entry->next;

	return entry;
}
