/*-------------------------------------------------------------------------
 *
 * indexing.c
 *	  This file contains routines to support indexes defined on system
 *	  catalogs.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/indexing.c,v 1.104 2003/08/04 02:39:58 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "executor/executor.h"


/*
 * CatalogOpenIndexes - open the indexes on a system catalog.
 *
 * When inserting or updating tuples in a system catalog, call this
 * to prepare to update the indexes for the catalog.
 *
 * In the current implementation, we share code for opening/closing the
 * indexes with execUtils.c.  But we do not use ExecInsertIndexTuples,
 * because we don't want to create an EState.  This implies that we
 * do not support partial or expressional indexes on system catalogs.
 * This could be fixed with localized changes here if we wanted to pay
 * the extra overhead of building an EState.
 */
CatalogIndexState
CatalogOpenIndexes(Relation heapRel)
{
	ResultRelInfo *resultRelInfo;

	resultRelInfo = makeNode(ResultRelInfo);
	resultRelInfo->ri_RangeTableIndex = 1;		/* dummy */
	resultRelInfo->ri_RelationDesc = heapRel;
	resultRelInfo->ri_TrigDesc = NULL;	/* we don't fire triggers */

	ExecOpenIndices(resultRelInfo);

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
void
CatalogIndexInsert(CatalogIndexState indstate, HeapTuple heapTuple)
{
	int			i;
	int			numIndexes;
	RelationPtr relationDescs;
	Relation	heapRelation;
	TupleDesc	heapDescriptor;
	IndexInfo **indexInfoArray;
	Datum		datum[INDEX_MAX_KEYS];
	char		nullv[INDEX_MAX_KEYS];

	/*
	 * Get information from the state structure.
	 */
	numIndexes = indstate->ri_NumIndices;
	relationDescs = indstate->ri_IndexRelationDescs;
	indexInfoArray = indstate->ri_IndexRelationInfo;
	heapRelation = indstate->ri_RelationDesc;
	heapDescriptor = RelationGetDescr(heapRelation);

	/*
	 * for each index, form and insert the index tuple
	 */
	for (i = 0; i < numIndexes; i++)
	{
		IndexInfo  *indexInfo;
		InsertIndexResult result;

		indexInfo = indexInfoArray[i];

		/*
		 * Expressional and partial indexes on system catalogs are not
		 * supported
		 */
		Assert(indexInfo->ii_Expressions == NIL);
		Assert(indexInfo->ii_Predicate == NIL);

		/*
		 * FormIndexDatum fills in its datum and null parameters with
		 * attribute information taken from the given heap tuple.
		 */
		FormIndexDatum(indexInfo,
					   heapTuple,
					   heapDescriptor,
					   NULL,	/* no expression eval to do */
					   datum,
					   nullv);

		/*
		 * The index AM does the rest.
		 */
		result = index_insert(relationDescs[i], /* index relation */
							  datum,	/* array of heaptuple Datums */
							  nullv,	/* info on nulls */
							  &(heapTuple->t_self),		/* tid of heap tuple */
							  heapRelation,
							  relationDescs[i]->rd_index->indisunique);

		if (result)
			pfree(result);
	}
}

/*
 * CatalogUpdateIndexes - do all the indexing work for a new catalog tuple
 *
 * This is a convenience routine for the common case where we only need
 * to insert or update a single tuple in a system catalog.	Avoid using it for
 * multiple tuples, since opening the indexes and building the index info
 * structures is moderately expensive.
 */
void
CatalogUpdateIndexes(Relation heapRel, HeapTuple heapTuple)
{
	CatalogIndexState indstate;

	indstate = CatalogOpenIndexes(heapRel);
	CatalogIndexInsert(indstate, heapTuple);
	CatalogCloseIndexes(indstate);
}
