/*-------------------------------------------------------------------------
 *
 * pg_collation.c
 *	  routines to support manipulation of the pg_collation relation
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_collation.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/sysattr.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_collation_fn.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/tqual.h"

/*
 * CollationCreate
 *
 * Add a new tuple to pg_collation.
 */
Oid
CollationCreate(const char *collname, Oid collnamespace,
				Oid collowner,
				int32 collencoding,
				const char *collcollate, const char *collctype)
{
	int			i;
	Relation	rel;
	TupleDesc	tupDesc;
	HeapTuple	tup;
	bool		nulls[Natts_pg_collation];
	Datum		values[Natts_pg_collation];
	NameData	name_name, name_collate, name_ctype;
	Oid			oid;
	ObjectAddress myself,
				referenced;

	AssertArg(collname);
	AssertArg(collnamespace);
	AssertArg(collowner);
	AssertArg(collcollate);
	AssertArg(collctype);

	/* make sure there is no existing collation of same name */
	if (SearchSysCacheExists3(COLLNAMEENCNSP,
							  PointerGetDatum(collname),
							  Int32GetDatum(collencoding),
							  ObjectIdGetDatum(collnamespace)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("collation \"%s\" for encoding \"%s\" already exists",
						collname, pg_encoding_to_char(collencoding))));

	/* open pg_collation */
	rel = heap_open(CollationRelationId, RowExclusiveLock);
	tupDesc = rel->rd_att;

	/* initialize nulls and values */
	for (i = 0; i < Natts_pg_collation; i++)
	{
		nulls[i] = false;
		values[i] = (Datum) NULL;
	}

	/* form a tuple */
	namestrcpy(&name_name, collname);
	values[Anum_pg_collation_collname - 1] = NameGetDatum(&name_name);
	values[Anum_pg_collation_collnamespace - 1] = ObjectIdGetDatum(collnamespace);
	values[Anum_pg_collation_collowner - 1] = ObjectIdGetDatum(collowner);
	values[Anum_pg_collation_collencoding - 1] = Int32GetDatum(collencoding);
	namestrcpy(&name_collate, collcollate);
	values[Anum_pg_collation_collcollate - 1] = NameGetDatum(&name_collate);
	namestrcpy(&name_ctype, collctype);
	values[Anum_pg_collation_collctype - 1] = NameGetDatum(&name_ctype);

	tup = heap_form_tuple(tupDesc, values, nulls);

	/* insert a new tuple */
	oid = simple_heap_insert(rel, tup);
	Assert(OidIsValid(oid));

	/* update the index if any */
	CatalogUpdateIndexes(rel, tup);

	myself.classId = CollationRelationId;
	myself.objectId = HeapTupleGetOid(tup);
	myself.objectSubId = 0;

	/* create dependency on namespace */
	referenced.classId = NamespaceRelationId;
	referenced.objectId = collnamespace;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* create dependency on owner */
	recordDependencyOnOwner(CollationRelationId, HeapTupleGetOid(tup),
							collowner);

	/* dependency on extension */
	recordDependencyOnCurrentExtension(&myself);

	/* Post creation hook for new collation */
	InvokeObjectAccessHook(OAT_POST_CREATE,
						   CollationRelationId, HeapTupleGetOid(tup), 0);

	heap_freetuple(tup);
	heap_close(rel, RowExclusiveLock);

	return oid;
}

/*
 * RemoveCollationById
 *
 * Remove a tuple from pg_collation by Oid. This function is solely
 * called inside catalog/dependency.c
 */
void
RemoveCollationById(Oid collationOid)
{
	Relation	rel;
	HeapTuple	tuple;
	HeapScanDesc scan;
	ScanKeyData scanKeyData;

	ScanKeyInit(&scanKeyData,
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(collationOid));

	/* open pg_collation */
	rel = heap_open(CollationRelationId, RowExclusiveLock);

	scan = heap_beginscan(rel, SnapshotNow,
						  1, &scanKeyData);

	/* search for the target tuple */
	if (HeapTupleIsValid(tuple = heap_getnext(scan, ForwardScanDirection)))
		simple_heap_delete(rel, &tuple->t_self);
	else
		elog(ERROR, "could not find tuple for collation %u", collationOid);
	heap_endscan(scan);
	heap_close(rel, RowExclusiveLock);
}
