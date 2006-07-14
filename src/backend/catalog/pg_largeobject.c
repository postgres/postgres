/*-------------------------------------------------------------------------
 *
 * pg_largeobject.c
 *	  routines to support manipulation of the pg_largeobject relation
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/catalog/pg_largeobject.c,v 1.26 2006/07/14 14:52:17 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/indexing.h"
#include "catalog/pg_largeobject.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"


/*
 * Create a large object having the given LO identifier.
 *
 * We do this by inserting an empty first page, so that the object will
 * appear to exist with size 0.  Note that the unique index will reject
 * an attempt to create a duplicate page.
 */
void
LargeObjectCreate(Oid loid)
{
	Relation	pg_largeobject;
	HeapTuple	ntup;
	Datum		values[Natts_pg_largeobject];
	char		nulls[Natts_pg_largeobject];
	int			i;

	pg_largeobject = heap_open(LargeObjectRelationId, RowExclusiveLock);

	/*
	 * Form new tuple
	 */
	for (i = 0; i < Natts_pg_largeobject; i++)
	{
		values[i] = (Datum) NULL;
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
	simple_heap_insert(pg_largeobject, ntup);

	/* Update indexes */
	CatalogUpdateIndexes(pg_largeobject, ntup);

	heap_close(pg_largeobject, RowExclusiveLock);

	heap_freetuple(ntup);
}

void
LargeObjectDrop(Oid loid)
{
	bool		found = false;
	Relation	pg_largeobject;
	ScanKeyData skey[1];
	SysScanDesc sd;
	HeapTuple	tuple;

	ScanKeyInit(&skey[0],
				Anum_pg_largeobject_loid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(loid));

	pg_largeobject = heap_open(LargeObjectRelationId, RowExclusiveLock);

	sd = systable_beginscan(pg_largeobject, LargeObjectLOidPNIndexId, true,
							SnapshotNow, 1, skey);

	while ((tuple = systable_getnext(sd)) != NULL)
	{
		simple_heap_delete(pg_largeobject, &tuple->t_self);
		found = true;
	}

	systable_endscan(sd);

	heap_close(pg_largeobject, RowExclusiveLock);

	if (!found)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("large object %u does not exist", loid)));
}

bool
LargeObjectExists(Oid loid)
{
	bool		retval = false;
	Relation	pg_largeobject;
	ScanKeyData skey[1];
	SysScanDesc sd;

	/*
	 * See if we can find any tuples belonging to the specified LO
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_largeobject_loid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(loid));

	pg_largeobject = heap_open(LargeObjectRelationId, AccessShareLock);

	sd = systable_beginscan(pg_largeobject, LargeObjectLOidPNIndexId, true,
							SnapshotNow, 1, skey);

	if (systable_getnext(sd) != NULL)
		retval = true;

	systable_endscan(sd);

	heap_close(pg_largeobject, AccessShareLock);

	return retval;
}
