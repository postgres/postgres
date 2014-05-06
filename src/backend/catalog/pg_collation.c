/*-------------------------------------------------------------------------
 *
 * pg_collation.c
 *	  routines to support manipulation of the pg_collation relation
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_collation.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_collation_fn.h"
#include "catalog/pg_namespace.h"
#include "mb/pg_wchar.h"
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
	Relation	rel;
	TupleDesc	tupDesc;
	HeapTuple	tup;
	Datum		values[Natts_pg_collation];
	bool		nulls[Natts_pg_collation];
	NameData	name_name,
				name_collate,
				name_ctype;
	Oid			oid;
	ObjectAddress myself,
				referenced;

	AssertArg(collname);
	AssertArg(collnamespace);
	AssertArg(collowner);
	AssertArg(collcollate);
	AssertArg(collctype);

	/*
	 * Make sure there is no existing collation of same name & encoding.
	 *
	 * This would be caught by the unique index anyway; we're just giving a
	 * friendlier error message.  The unique index provides a backstop against
	 * race conditions.
	 */
	if (SearchSysCacheExists3(COLLNAMEENCNSP,
							  PointerGetDatum(collname),
							  Int32GetDatum(collencoding),
							  ObjectIdGetDatum(collnamespace)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("collation \"%s\" for encoding \"%s\" already exists",
						collname, pg_encoding_to_char(collencoding))));

	/*
	 * Also forbid matching an any-encoding entry.  This test of course is not
	 * backed up by the unique index, but it's not a problem since we don't
	 * support adding any-encoding entries after initdb.
	 */
	if (SearchSysCacheExists3(COLLNAMEENCNSP,
							  PointerGetDatum(collname),
							  Int32GetDatum(-1),
							  ObjectIdGetDatum(collnamespace)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("collation \"%s\" already exists",
						collname)));

	/* open pg_collation */
	rel = heap_open(CollationRelationId, RowExclusiveLock);
	tupDesc = RelationGetDescr(rel);

	/* form a tuple */
	memset(nulls, 0, sizeof(nulls));

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

	/* set up dependencies for the new collation */
	myself.classId = CollationRelationId;
	myself.objectId = oid;
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
	recordDependencyOnCurrentExtension(&myself, false);

	/* Post creation hook for new collation */
	InvokeObjectPostCreateHook(CollationRelationId, oid, 0);

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
	ScanKeyData scanKeyData;
	SysScanDesc scandesc;
	HeapTuple	tuple;

	rel = heap_open(CollationRelationId, RowExclusiveLock);

	ScanKeyInit(&scanKeyData,
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(collationOid));

	scandesc = systable_beginscan(rel, CollationOidIndexId, true,
								  NULL, 1, &scanKeyData);

	tuple = systable_getnext(scandesc);

	if (HeapTupleIsValid(tuple))
		simple_heap_delete(rel, &tuple->t_self);
	else
		elog(ERROR, "could not find tuple for collation %u", collationOid);

	systable_endscan(scandesc);

	heap_close(rel, RowExclusiveLock);
}
