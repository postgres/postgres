/*-------------------------------------------------------------------------
 *
 * pg_largeobject.c
 *	  routines to support manipulation of the pg_largeobject relation
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_largeobject.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/sysattr.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_largeobject.h"
#include "catalog/pg_largeobject_metadata.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/tqual.h"


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

	pg_lo_meta = heap_open(LargeObjectMetadataRelationId,
						   RowExclusiveLock);

	/*
	 * Insert metadata of the largeobject
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	values[Anum_pg_largeobject_metadata_lomowner - 1]
		= ObjectIdGetDatum(GetUserId());
	nulls[Anum_pg_largeobject_metadata_lomacl - 1] = true;

	ntup = heap_form_tuple(RelationGetDescr(pg_lo_meta),
						   values, nulls);
	if (OidIsValid(loid))
		HeapTupleSetOid(ntup, loid);

	loid_new = simple_heap_insert(pg_lo_meta, ntup);
	Assert(!OidIsValid(loid) || loid == loid_new);

	CatalogUpdateIndexes(pg_lo_meta, ntup);

	heap_freetuple(ntup);

	heap_close(pg_lo_meta, RowExclusiveLock);

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

	pg_lo_meta = heap_open(LargeObjectMetadataRelationId,
						   RowExclusiveLock);

	pg_largeobject = heap_open(LargeObjectRelationId,
							   RowExclusiveLock);

	/*
	 * Delete an entry from pg_largeobject_metadata
	 */
	ScanKeyInit(&skey[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(loid));

	scan = systable_beginscan(pg_lo_meta,
							  LargeObjectMetadataOidIndexId, true,
							  SnapshotNow, 1, skey);

	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("large object %u does not exist", loid)));

	simple_heap_delete(pg_lo_meta, &tuple->t_self);

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
							  SnapshotNow, 1, skey);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		simple_heap_delete(pg_largeobject, &tuple->t_self);
	}

	systable_endscan(scan);

	heap_close(pg_largeobject, RowExclusiveLock);

	heap_close(pg_lo_meta, RowExclusiveLock);
}

/*
 * LargeObjectAlterOwner
 *
 * Implementation of ALTER LARGE OBJECT statement
 */
void
LargeObjectAlterOwner(Oid loid, Oid newOwnerId)
{
	Form_pg_largeobject_metadata form_lo_meta;
	Relation	pg_lo_meta;
	ScanKeyData skey[1];
	SysScanDesc scan;
	HeapTuple	oldtup;
	HeapTuple	newtup;

	pg_lo_meta = heap_open(LargeObjectMetadataRelationId,
						   RowExclusiveLock);

	ScanKeyInit(&skey[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(loid));

	scan = systable_beginscan(pg_lo_meta,
							  LargeObjectMetadataOidIndexId, true,
							  SnapshotNow, 1, skey);

	oldtup = systable_getnext(scan);
	if (!HeapTupleIsValid(oldtup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("large object %u does not exist", loid)));

	form_lo_meta = (Form_pg_largeobject_metadata) GETSTRUCT(oldtup);
	if (form_lo_meta->lomowner != newOwnerId)
	{
		Datum		values[Natts_pg_largeobject_metadata];
		bool		nulls[Natts_pg_largeobject_metadata];
		bool		replaces[Natts_pg_largeobject_metadata];
		Acl		   *newAcl;
		Datum		aclDatum;
		bool		isnull;

		/* Superusers can always do it */
		if (!superuser())
		{
			/*
			 * lo_compat_privileges is not checked here, because ALTER LARGE
			 * OBJECT ... OWNER did not exist at all prior to PostgreSQL 9.0.
			 *
			 * We must be the owner of the existing object.
			 */
			if (!pg_largeobject_ownercheck(loid, GetUserId()))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("must be owner of large object %u", loid)));

			/* Must be able to become new owner */
			check_is_member_of_role(GetUserId(), newOwnerId);
		}

		memset(values, 0, sizeof(values));
		memset(nulls, false, sizeof(nulls));
		memset(replaces, false, sizeof(nulls));

		values[Anum_pg_largeobject_metadata_lomowner - 1]
			= ObjectIdGetDatum(newOwnerId);
		replaces[Anum_pg_largeobject_metadata_lomowner - 1] = true;

		/*
		 * Determine the modified ACL for the new owner. This is only
		 * necessary when the ACL is non-null.
		 */
		aclDatum = heap_getattr(oldtup,
								Anum_pg_largeobject_metadata_lomacl,
								RelationGetDescr(pg_lo_meta), &isnull);
		if (!isnull)
		{
			newAcl = aclnewowner(DatumGetAclP(aclDatum),
								 form_lo_meta->lomowner, newOwnerId);
			values[Anum_pg_largeobject_metadata_lomacl - 1]
				= PointerGetDatum(newAcl);
			replaces[Anum_pg_largeobject_metadata_lomacl - 1] = true;
		}

		newtup = heap_modify_tuple(oldtup, RelationGetDescr(pg_lo_meta),
								   values, nulls, replaces);

		simple_heap_update(pg_lo_meta, &newtup->t_self, newtup);
		CatalogUpdateIndexes(pg_lo_meta, newtup);

		heap_freetuple(newtup);

		/* Update owner dependency reference */
		changeDependencyOnOwner(LargeObjectRelationId,
								loid, newOwnerId);
	}
	systable_endscan(scan);

	heap_close(pg_lo_meta, RowExclusiveLock);
}

/*
 * LargeObjectExists
 *
 * We don't use the system cache for large object metadata, for fear of
 * using too much local memory.
 *
 * This function always scans the system catalog using SnapshotNow, so it
 * should not be used when a large object is opened in read-only mode (because
 * large objects opened in read only mode are supposed to be viewed relative
 * to the caller's snapshot, whereas in read-write mode they are relative to
 * SnapshotNow).
 */
bool
LargeObjectExists(Oid loid)
{
	Relation	pg_lo_meta;
	ScanKeyData skey[1];
	SysScanDesc sd;
	HeapTuple	tuple;
	bool		retval = false;

	ScanKeyInit(&skey[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(loid));

	pg_lo_meta = heap_open(LargeObjectMetadataRelationId,
						   AccessShareLock);

	sd = systable_beginscan(pg_lo_meta,
							LargeObjectMetadataOidIndexId, true,
							SnapshotNow, 1, skey);

	tuple = systable_getnext(sd);
	if (HeapTupleIsValid(tuple))
		retval = true;

	systable_endscan(sd);

	heap_close(pg_lo_meta, AccessShareLock);

	return retval;
}
