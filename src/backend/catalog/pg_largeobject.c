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
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/pg_largeobject.c,v 1.3 2000/10/21 15:55:21 momjian Exp $
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
#include "utils/fmgroids.h"

bytea *_byteain(const char *data, int32 size);

bytea *_byteain(const char *data, int32 size) {
	bytea	*result;

	result = (bytea *)palloc(size + VARHDRSZ);
	result->vl_len = size + VARHDRSZ;
	if (size > 0)
		memcpy(result->vl_dat, data, size);
	
	return result;
}

Oid LargeobjectCreate(Oid loid) {
	Oid		retval;
  	Relation        	pg_largeobject;
	HeapTuple	ntup = (HeapTuple) palloc(sizeof(HeapTupleData));
	Relation		idescs[Num_pg_largeobject_indices];
	Datum		values[Natts_pg_largeobject];
	char		nulls[Natts_pg_largeobject];
	int		i;

	for (i=0; i<Natts_pg_largeobject; i++) {
		nulls[i] = ' ';
		values[i] = (Datum)NULL;
	}

	i = 0;
	values[i++] = ObjectIdGetDatum(loid);
	values[i++] = Int32GetDatum(0);
	values[i++] = (Datum) _byteain(NULL, 0);
	
	pg_largeobject = heap_openr(LargeobjectRelationName, RowExclusiveLock);
	ntup = heap_formtuple(pg_largeobject->rd_att, values, nulls);
	retval = heap_insert(pg_largeobject, ntup);

	if (!IsIgnoringSystemIndexes()) {
		CatalogOpenIndices(Num_pg_largeobject_indices, Name_pg_largeobject_indices, idescs);
		CatalogIndexInsert(idescs, Num_pg_largeobject_indices, pg_largeobject, ntup);
		CatalogCloseIndices(Num_pg_largeobject_indices, idescs);
	}
	
	heap_close(pg_largeobject, RowExclusiveLock);
	heap_freetuple(ntup);
	
	CommandCounterIncrement();

	return retval;
}

void LargeobjectDrop(Oid loid) {
	Relation	pg_largeobject;
	Relation	pg_lo_id;
	ScanKeyData	skey;
	IndexScanDesc	sd = (IndexScanDesc) NULL;
	RetrieveIndexResult	indexRes;
	int	found = 0;

	ScanKeyEntryInitialize(&skey,
					    (bits16) 0x0,
					    (AttrNumber) 1,
					    (RegProcedure) F_OIDEQ,
					    ObjectIdGetDatum(loid));

	pg_largeobject = heap_openr(LargeobjectRelationName, RowShareLock);
	pg_lo_id = index_openr(LargeobjectLOIdIndex);

	sd = index_beginscan(pg_lo_id, false, 1, &skey);

	while((indexRes = index_getnext(sd, ForwardScanDirection))) {
		found++;
		heap_delete(pg_largeobject, &indexRes->heap_iptr, NULL);
		pfree(indexRes);
	}

	index_endscan(sd);

	index_close(pg_lo_id);
	heap_close(pg_largeobject, RowShareLock);
	if (found == 0)
		elog(ERROR, "LargeobjectDrop: large object %d not found", loid);
}

int LargeobjectFind(Oid loid) {
	int	retval = 0;
	Relation	pg_lo_id;
	ScanKeyData	skey;
	IndexScanDesc	sd = (IndexScanDesc) NULL;
	RetrieveIndexResult	indexRes;

	ScanKeyEntryInitialize(&skey,
					    (bits16) 0x0,
					    (AttrNumber) 1,
					    (RegProcedure) F_OIDEQ,
					    ObjectIdGetDatum(loid));

	pg_lo_id = index_openr(LargeobjectLOIdIndex);

	sd = index_beginscan(pg_lo_id, false, 1, &skey);

	if ((indexRes = index_getnext(sd, ForwardScanDirection))) {
		retval = 1;
		pfree(indexRes);
	}

	index_endscan(sd);

	index_close(pg_lo_id);
	return retval;
}

