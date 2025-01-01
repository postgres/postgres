/*-------------------------------------------------------------------------
 *
 * indexing.c
 *	  This file contains routines to support indexes defined on system
 *	  catalogs.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/indexing.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "executor/executor.h"
#include "utils/rel.h"


/*
 * CatalogOpenIndexes - open the indexes on a system catalog.
 *
 * When inserting or updating tuples in a system catalog, call this
 * to prepare to update the indexes for the catalog.
 *
 * In the current implementation, we share code for opening/closing the
 * indexes with execUtils.c.  But we do not use ExecInsertIndexTuples,
 * because we don't want to create an EState.  This implies that we
 * do not support partial or expressional indexes on system catalogs,
 * nor can we support generalized exclusion constraints.
 * This could be fixed with localized changes here if we wanted to pay
 * the extra overhead of building an EState.
 */
CatalogIndexState
CatalogOpenIndexes(Relation heapRel)
{
	ResultRelInfo *resultRelInfo;

	resultRelInfo = makeNode(ResultRelInfo);
	resultRelInfo->ri_RangeTableIndex = 0;	/* dummy */
	resultRelInfo->ri_RelationDesc = heapRel;
	resultRelInfo->ri_TrigDesc = NULL;	/* we don't fire triggers */

	ExecOpenIndices(resultRelInfo, false);

	return resultRelInfo;
}

/*
 * CatalogCloseIndexes - clean up resources allocated by CatalogOpenIndexes
 */
void
CatalogCloseIndexes(CatalogIndexState indstate)
{
	ExecCloseIndices(indstate);
	pfree(indstate);
}

/*
 * CatalogIndexInsert - insert index entries for one catalog tuple
 *
 * This should be called for each inserted or updated catalog tuple.
 *
 * This is effectively a cut-down version of ExecInsertIndexTuples.
 */
static void
CatalogIndexInsert(CatalogIndexState indstate, HeapTuple heapTuple,
				   TU_UpdateIndexes updateIndexes)
{
	int			i;
	int			numIndexes;
	RelationPtr relationDescs;
	Relation	heapRelation;
	TupleTableSlot *slot;
	IndexInfo **indexInfoArray;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	bool		onlySummarized = (updateIndexes == TU_Summarizing);

	/*
	 * HOT update does not require index inserts. But with asserts enabled we
	 * want to check that it'd be legal to currently insert into the
	 * table/index.
	 */
#ifndef USE_ASSERT_CHECKING
	if (HeapTupleIsHeapOnly(heapTuple) && !onlySummarized)
		return;
#endif

	/* When only updating summarized indexes, the tuple has to be HOT. */
	Assert((!onlySummarized) || HeapTupleIsHeapOnly(heapTuple));

	/*
	 * Get information from the state structure.  Fall out if nothing to do.
	 */
	numIndexes = indstate->ri_NumIndices;
	if (numIndexes == 0)
		return;
	relationDescs = indstate->ri_IndexRelationDescs;
	indexInfoArray = indstate->ri_IndexRelationInfo;
	heapRelation = indstate->ri_RelationDesc;

	/* Need a slot to hold the tuple being examined */
	slot = MakeSingleTupleTableSlot(RelationGetDescr(heapRelation),
									&TTSOpsHeapTuple);
	ExecStoreHeapTuple(heapTuple, slot, false);

	/*
	 * for each index, form and insert the index tuple
	 */
	for (i = 0; i < numIndexes; i++)
	{
		IndexInfo  *indexInfo;
		Relation	index;

		indexInfo = indexInfoArray[i];
		index = relationDescs[i];

		/* If the index is marked as read-only, ignore it */
		if (!indexInfo->ii_ReadyForInserts)
			continue;

		/*
		 * Expressional and partial indexes on system catalogs are not
		 * supported, nor exclusion constraints, nor deferred uniqueness
		 */
		Assert(indexInfo->ii_Expressions == NIL);
		Assert(indexInfo->ii_Predicate == NIL);
		Assert(indexInfo->ii_ExclusionOps == NULL);
		Assert(index->rd_index->indimmediate);
		Assert(indexInfo->ii_NumIndexKeyAttrs != 0);

		/* see earlier check above */
#ifdef USE_ASSERT_CHECKING
		if (HeapTupleIsHeapOnly(heapTuple) && !onlySummarized)
		{
			Assert(!ReindexIsProcessingIndex(RelationGetRelid(index)));
			continue;
		}
#endif							/* USE_ASSERT_CHECKING */

		/*
		 * Skip insertions into non-summarizing indexes if we only need to
		 * update summarizing indexes.
		 */
		if (onlySummarized && !indexInfo->ii_Summarizing)
			continue;

		/*
		 * FormIndexDatum fills in its values and isnull parameters with the
		 * appropriate values for the column(s) of the index.
		 */
		FormIndexDatum(indexInfo,
					   slot,
					   NULL,	/* no expression eval to do */
					   values,
					   isnull);

		/*
		 * The index AM does the rest.
		 */
		index_insert(index,		/* index relation */
					 values,	/* array of index Datums */
					 isnull,	/* is-null flags */
					 &(heapTuple->t_self),	/* tid of heap tuple */
					 heapRelation,
					 index->rd_index->indisunique ?
					 UNIQUE_CHECK_YES : UNIQUE_CHECK_NO,
					 false,
					 indexInfo);
	}

	ExecDropSingleTupleTableSlot(slot);
}

/*
 * Subroutine to verify that catalog constraints are honored.
 *
 * Tuples inserted via CatalogTupleInsert/CatalogTupleUpdate are generally
 * "hand made", so that it's possible that they fail to satisfy constraints
 * that would be checked if they were being inserted by the executor.  That's
 * a coding error, so we only bother to check for it in assert-enabled builds.
 */
#ifdef USE_ASSERT_CHECKING

static void
CatalogTupleCheckConstraints(Relation heapRel, HeapTuple tup)
{
	/*
	 * Currently, the only constraints implemented for system catalogs are
	 * attnotnull constraints.
	 */
	if (HeapTupleHasNulls(tup))
	{
		TupleDesc	tupdesc = RelationGetDescr(heapRel);
		bits8	   *bp = tup->t_data->t_bits;

		for (int attnum = 0; attnum < tupdesc->natts; attnum++)
		{
			Form_pg_attribute thisatt = TupleDescAttr(tupdesc, attnum);

			Assert(!(thisatt->attnotnull && att_isnull(attnum, bp)));
		}
	}
}

#else							/* !USE_ASSERT_CHECKING */

#define CatalogTupleCheckConstraints(heapRel, tup)  ((void) 0)

#endif							/* USE_ASSERT_CHECKING */

/*
 * CatalogTupleInsert - do heap and indexing work for a new catalog tuple
 *
 * Insert the tuple data in "tup" into the specified catalog relation.
 *
 * This is a convenience routine for the common case of inserting a single
 * tuple in a system catalog; it inserts a new heap tuple, keeping indexes
 * current.  Avoid using it for multiple tuples, since opening the indexes
 * and building the index info structures is moderately expensive.
 * (Use CatalogTupleInsertWithInfo in such cases.)
 */
void
CatalogTupleInsert(Relation heapRel, HeapTuple tup)
{
	CatalogIndexState indstate;

	CatalogTupleCheckConstraints(heapRel, tup);

	indstate = CatalogOpenIndexes(heapRel);

	simple_heap_insert(heapRel, tup);

	CatalogIndexInsert(indstate, tup, TU_All);
	CatalogCloseIndexes(indstate);
}

/*
 * CatalogTupleInsertWithInfo - as above, but with caller-supplied index info
 *
 * This should be used when it's important to amortize CatalogOpenIndexes/
 * CatalogCloseIndexes work across multiple insertions.  At some point we
 * might cache the CatalogIndexState data somewhere (perhaps in the relcache)
 * so that callers needn't trouble over this ... but we don't do so today.
 */
void
CatalogTupleInsertWithInfo(Relation heapRel, HeapTuple tup,
						   CatalogIndexState indstate)
{
	CatalogTupleCheckConstraints(heapRel, tup);

	simple_heap_insert(heapRel, tup);

	CatalogIndexInsert(indstate, tup, TU_All);
}

/*
 * CatalogTuplesMultiInsertWithInfo - as above, but for multiple tuples
 *
 * Insert multiple tuples into the given catalog relation at once, with an
 * amortized cost of CatalogOpenIndexes.
 */
void
CatalogTuplesMultiInsertWithInfo(Relation heapRel, TupleTableSlot **slot,
								 int ntuples, CatalogIndexState indstate)
{
	/* Nothing to do */
	if (ntuples <= 0)
		return;

	heap_multi_insert(heapRel, slot, ntuples,
					  GetCurrentCommandId(true), 0, NULL);

	/*
	 * There is no equivalent to heap_multi_insert for the catalog indexes, so
	 * we must loop over and insert individually.
	 */
	for (int i = 0; i < ntuples; i++)
	{
		bool		should_free;
		HeapTuple	tuple;

		tuple = ExecFetchSlotHeapTuple(slot[i], true, &should_free);
		tuple->t_tableOid = slot[i]->tts_tableOid;
		CatalogIndexInsert(indstate, tuple, TU_All);

		if (should_free)
			heap_freetuple(tuple);
	}
}

/*
 * CatalogTupleUpdate - do heap and indexing work for updating a catalog tuple
 *
 * Update the tuple identified by "otid", replacing it with the data in "tup".
 *
 * This is a convenience routine for the common case of updating a single
 * tuple in a system catalog; it updates one heap tuple, keeping indexes
 * current.  Avoid using it for multiple tuples, since opening the indexes
 * and building the index info structures is moderately expensive.
 * (Use CatalogTupleUpdateWithInfo in such cases.)
 */
void
CatalogTupleUpdate(Relation heapRel, ItemPointer otid, HeapTuple tup)
{
	CatalogIndexState indstate;
	TU_UpdateIndexes updateIndexes = TU_All;

	CatalogTupleCheckConstraints(heapRel, tup);

	indstate = CatalogOpenIndexes(heapRel);

	simple_heap_update(heapRel, otid, tup, &updateIndexes);

	CatalogIndexInsert(indstate, tup, updateIndexes);
	CatalogCloseIndexes(indstate);
}

/*
 * CatalogTupleUpdateWithInfo - as above, but with caller-supplied index info
 *
 * This should be used when it's important to amortize CatalogOpenIndexes/
 * CatalogCloseIndexes work across multiple updates.  At some point we
 * might cache the CatalogIndexState data somewhere (perhaps in the relcache)
 * so that callers needn't trouble over this ... but we don't do so today.
 */
void
CatalogTupleUpdateWithInfo(Relation heapRel, ItemPointer otid, HeapTuple tup,
						   CatalogIndexState indstate)
{
	TU_UpdateIndexes updateIndexes = TU_All;

	CatalogTupleCheckConstraints(heapRel, tup);

	simple_heap_update(heapRel, otid, tup, &updateIndexes);

	CatalogIndexInsert(indstate, tup, updateIndexes);
}

/*
 * CatalogTupleDelete - do heap and indexing work for deleting a catalog tuple
 *
 * Delete the tuple identified by "tid" in the specified catalog.
 *
 * With Postgres heaps, there is no index work to do at deletion time;
 * cleanup will be done later by VACUUM.  However, callers of this function
 * shouldn't have to know that; we'd like a uniform abstraction for all
 * catalog tuple changes.  Hence, provide this currently-trivial wrapper.
 *
 * The abstraction is a bit leaky in that we don't provide an optimized
 * CatalogTupleDeleteWithInfo version, because there is currently nothing to
 * optimize.  If we ever need that, rather than touching a lot of call sites,
 * it might be better to do something about caching CatalogIndexState.
 */
void
CatalogTupleDelete(Relation heapRel, ItemPointer tid)
{
	simple_heap_delete(heapRel, tid);
}
