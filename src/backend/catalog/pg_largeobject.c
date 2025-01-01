/*-------------------------------------------------------------------------
 *
 * pg_largeobject.c
 *	  routines to support manipulation of the pg_largeobject relation
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_largeobject.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/pg_largeobject.h"
#include "catalog/pg_largeobject_metadata.h"
#include "miscadmin.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"


/*
 * Create a large object having the given LO identifier.
 *
 * We create a new large object by inserting an entry into
 * pg_largeobject_metadata without any data pages, so that the object
 * will appear to exist with size 0.
 */
Oid
LargeObjectCreate(Oid loid)
{
	Relation	pg_lo_meta;
	HeapTuple	ntup;
	Oid			loid_new;
	Datum		values[Natts_pg_largeobject_metadata];
	bool		nulls[Natts_pg_largeobject_metadata];

	pg_lo_meta = table_open(LargeObjectMetadataRelationId,
							RowExclusiveLock);

	/*
	 * Insert metadata of the largeobject
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	if (OidIsValid(loid))
		loid_new = loid;
	else
		loid_new = GetNewOidWithIndex(pg_lo_meta,
									  LargeObjectMetadataOidIndexId,
									  Anum_pg_largeobject_metadata_oid);

	values[Anum_pg_largeobject_metadata_oid - 1] = ObjectIdGetDatum(loid_new);
	values[Anum_pg_largeobject_metadata_lomowner - 1]
		= ObjectIdGetDatum(GetUserId());
	nulls[Anum_pg_largeobject_metadata_lomacl - 1] = true;

	ntup = heap_form_tuple(RelationGetDescr(pg_lo_meta),
						   values, nulls);

	CatalogTupleInsert(pg_lo_meta, ntup);

	heap_freetuple(ntup);

	table_close(pg_lo_meta, RowExclusiveLock);

	return loid_new;
}

/*
 * Drop a large object having the given LO identifier.  Both the data pages
 * and metadata must be dropped.
 */
void
LargeObjectDrop(Oid loid)
{
	Relation	pg_lo_meta;
	Relation	pg_largeobject;
	ScanKeyData skey[1];
	SysScanDesc scan;
	HeapTuple	tuple;

	pg_lo_meta = table_open(LargeObjectMetadataRelationId,
							RowExclusiveLock);

	pg_largeobject = table_open(LargeObjectRelationId,
								RowExclusiveLock);

	/*
	 * Delete an entry from pg_largeobject_metadata
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_largeobject_metadata_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(loid));

	scan = systable_beginscan(pg_lo_meta,
							  LargeObjectMetadataOidIndexId, true,
							  NULL, 1, skey);

	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("large object %u does not exist", loid)));

	CatalogTupleDelete(pg_lo_meta, &tuple->t_self);

	systable_endscan(scan);

	/*
	 * Delete all the associated entries from pg_largeobject
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_largeobject_loid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(loid));

	scan = systable_beginscan(pg_largeobject,
							  LargeObjectLOidPNIndexId, true,
							  NULL, 1, skey);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		CatalogTupleDelete(pg_largeobject, &tuple->t_self);
	}

	systable_endscan(scan);

	table_close(pg_largeobject, RowExclusiveLock);

	table_close(pg_lo_meta, RowExclusiveLock);
}

/*
 * LargeObjectExists
 *
 * We don't use the system cache for large object metadata, for fear of
 * using too much local memory.
 *
 * This function always scans the system catalog using an up-to-date snapshot,
 * so it should not be used when a large object is opened in read-only mode
 * (because large objects opened in read only mode are supposed to be viewed
 * relative to the caller's snapshot, whereas in read-write mode they are
 * relative to a current snapshot).
 */
bool
LargeObjectExists(Oid loid)
{
	return LargeObjectExistsWithSnapshot(loid, NULL);
}

/*
 * Same as LargeObjectExists(), except snapshot to read with can be specified.
 */
bool
LargeObjectExistsWithSnapshot(Oid loid, Snapshot snapshot)
{
	Relation	pg_lo_meta;
	ScanKeyData skey[1];
	SysScanDesc sd;
	HeapTuple	tuple;
	bool		retval = false;

	ScanKeyInit(&skey[0],
				Anum_pg_largeobject_metadata_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(loid));

	pg_lo_meta = table_open(LargeObjectMetadataRelationId,
							AccessShareLock);

	sd = systable_beginscan(pg_lo_meta,
							LargeObjectMetadataOidIndexId, true,
							snapshot, 1, skey);

	tuple = systable_getnext(sd);
	if (HeapTupleIsValid(tuple))
		retval = true;

	systable_endscan(sd);

	table_close(pg_lo_meta, AccessShareLock);

	return retval;
}
