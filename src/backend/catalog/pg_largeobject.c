/*-------------------------------------------------------------------------
 *
 * pg_largeobject.c
 *	  routines to support manipulation of the pg_largeobject relation
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/pg_largeobject.c,v 1.6 2001/01/23 04:32:21 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/pg_largeobject.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"


/*
 * Create a large object having the given LO identifier.
 *
 * We do this by inserting an empty first page, so that the object will
 * appear to exist with size 0.  Note that the unique index will reject
 * an attempt to create a duplicate page.
 *
 * Return value is OID assigned to the page tuple (any use in it?)
 */
Oid
LargeObjectCreate(Oid loid)
{
	Oid			retval;
	Relation	pg_largeobject;
	HeapTuple	ntup;
	Relation	idescs[Num_pg_largeobject_indices];
	Datum		values[Natts_pg_largeobject];
	char		nulls[Natts_pg_largeobject];
	int			i;

	pg_largeobject = heap_openr(LargeObjectRelationName, RowExclusiveLock);

	/*
	 * Form new tuple
	 */
	for (i = 0; i < Natts_pg_largeobject; i++)
	{
		values[i] = (Datum)NULL;
		nulls[i] = ' ';
	}

	i = 0;
	values[i++] = ObjectIdGetDatum(loid);
	values[i++] = Int32GetDatum(0);
	values[i++] = DirectFunctionCall1(byteain,
									  CStringGetDatum(""));
	
	ntup = heap_formtuple(pg_largeobject->rd_att, values, nulls);

	/*
	 * Insert it
	 */
	retval = heap_insert(pg_largeobject, ntup);

	/*
	 * Update indices
	 */
	if (!IsIgnoringSystemIndexes())
	{
		CatalogOpenIndices(Num_pg_largeobject_indices, Name_pg_largeobject_indices, idescs);
		CatalogIndexInsert(idescs, Num_pg_largeobject_indices, pg_largeobject, ntup);
		CatalogCloseIndices(Num_pg_largeobject_indices, idescs);
	}
	
	heap_close(pg_largeobject, RowExclusiveLock);

	heap_freetuple(ntup);

	return retval;
}

void
LargeObjectDrop(Oid loid)
{
	bool		found = false;
	Relation	pg_largeobject;
	Relation	pg_lo_idx;
	ScanKeyData	skey[1];
	IndexScanDesc sd;
	RetrieveIndexResult	indexRes;
	HeapTupleData tuple;
	Buffer		buffer;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_OIDEQ,
						   ObjectIdGetDatum(loid));

	pg_largeobject = heap_openr(LargeObjectRelationName, RowShareLock);
	pg_lo_idx = index_openr(LargeObjectLOidPNIndex);

	sd = index_beginscan(pg_lo_idx, false, 1, skey);

	tuple.t_datamcxt = CurrentMemoryContext;
	tuple.t_data = NULL;

	while ((indexRes = index_getnext(sd, ForwardScanDirection)))
	{
		tuple.t_self = indexRes->heap_iptr;
		heap_fetch(pg_largeobject, SnapshotNow, &tuple, &buffer);
		pfree(indexRes);
		if (tuple.t_data != NULL)
		{
			simple_heap_delete(pg_largeobject, &tuple.t_self);
			ReleaseBuffer(buffer);
			found = true;
		}
	}

	index_endscan(sd);

	index_close(pg_lo_idx);
	heap_close(pg_largeobject, RowShareLock);

	if (!found)
		elog(ERROR, "LargeObjectDrop: large object %u not found", loid);
}

bool
LargeObjectExists(Oid loid)
{
	bool		retval = false;
	Relation	pg_largeobject;
	Relation	pg_lo_idx;
	ScanKeyData	skey[1];
	IndexScanDesc sd;
	RetrieveIndexResult	indexRes;
	HeapTupleData tuple;
	Buffer		buffer;

	/*
	 * See if we can find any tuples belonging to the specified LO
	 */
	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_OIDEQ,
						   ObjectIdGetDatum(loid));

	pg_largeobject = heap_openr(LargeObjectRelationName, RowShareLock);
	pg_lo_idx = index_openr(LargeObjectLOidPNIndex);

	sd = index_beginscan(pg_lo_idx, false, 1, skey);

	tuple.t_datamcxt = CurrentMemoryContext;
	tuple.t_data = NULL;

	while ((indexRes = index_getnext(sd, ForwardScanDirection)))
	{
		tuple.t_self = indexRes->heap_iptr;
		heap_fetch(pg_largeobject, SnapshotNow, &tuple, &buffer);
		pfree(indexRes);
		if (tuple.t_data != NULL)
		{
			retval = true;
			ReleaseBuffer(buffer);
			break;
		}
	}

	index_endscan(sd);

	index_close(pg_lo_idx);
	heap_close(pg_largeobject, RowShareLock);

	return retval;
}
