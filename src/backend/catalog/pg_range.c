/*-------------------------------------------------------------------------
 *
 * pg_range.c
 *	  routines to support manipulation of the pg_range relation
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_range.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_range.h"
#include "catalog/pg_type.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"


/*
 * RangeCreate
 *		Create an entry in pg_range.
 */
void
RangeCreate(Oid rangeTypeOid, Oid rangeSubType, Oid rangeCollation,
			Oid rangeSubOpclass, RegProcedure rangeCanonical,
			RegProcedure rangeSubDiff, Oid multirangeTypeOid)
{
	Relation	pg_range;
	Datum		values[Natts_pg_range];
	bool		nulls[Natts_pg_range];
	HeapTuple	tup;
	ObjectAddress myself;
	ObjectAddress referenced;
	ObjectAddress referencing;
	ObjectAddresses *addrs;

	pg_range = table_open(RangeRelationId, RowExclusiveLock);

	memset(nulls, 0, sizeof(nulls));

	values[Anum_pg_range_rngtypid - 1] = ObjectIdGetDatum(rangeTypeOid);
	values[Anum_pg_range_rngsubtype - 1] = ObjectIdGetDatum(rangeSubType);
	values[Anum_pg_range_rngcollation - 1] = ObjectIdGetDatum(rangeCollation);
	values[Anum_pg_range_rngsubopc - 1] = ObjectIdGetDatum(rangeSubOpclass);
	values[Anum_pg_range_rngcanonical - 1] = ObjectIdGetDatum(rangeCanonical);
	values[Anum_pg_range_rngsubdiff - 1] = ObjectIdGetDatum(rangeSubDiff);
	values[Anum_pg_range_rngmultitypid - 1] = ObjectIdGetDatum(multirangeTypeOid);

	tup = heap_form_tuple(RelationGetDescr(pg_range), values, nulls);

	CatalogTupleInsert(pg_range, tup);
	heap_freetuple(tup);

	/* record type's dependencies on range-related items */
	addrs = new_object_addresses();

	ObjectAddressSet(myself, TypeRelationId, rangeTypeOid);

	ObjectAddressSet(referenced, TypeRelationId, rangeSubType);
	add_exact_object_address(&referenced, addrs);

	ObjectAddressSet(referenced, OperatorClassRelationId, rangeSubOpclass);
	add_exact_object_address(&referenced, addrs);

	if (OidIsValid(rangeCollation))
	{
		ObjectAddressSet(referenced, CollationRelationId, rangeCollation);
		add_exact_object_address(&referenced, addrs);
	}

	if (OidIsValid(rangeCanonical))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, rangeCanonical);
		add_exact_object_address(&referenced, addrs);
	}

	if (OidIsValid(rangeSubDiff))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, rangeSubDiff);
		add_exact_object_address(&referenced, addrs);
	}

	record_object_address_dependencies(&myself, addrs, DEPENDENCY_NORMAL);
	free_object_addresses(addrs);

	/* record multirange type's dependency on the range type */
	referencing.classId = TypeRelationId;
	referencing.objectId = multirangeTypeOid;
	referencing.objectSubId = 0;
	recordDependencyOn(&referencing, &myself, DEPENDENCY_INTERNAL);

	table_close(pg_range, RowExclusiveLock);
}


/*
 * RangeDelete
 *		Remove the pg_range entry for the specified type.
 */
void
RangeDelete(Oid rangeTypeOid)
{
	Relation	pg_range;
	ScanKeyData key[1];
	SysScanDesc scan;
	HeapTuple	tup;

	pg_range = table_open(RangeRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_range_rngtypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(rangeTypeOid));

	scan = systable_beginscan(pg_range, RangeTypidIndexId, true,
							  NULL, 1, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		CatalogTupleDelete(pg_range, &tup->t_self);
	}

	systable_endscan(scan);

	table_close(pg_range, RowExclusiveLock);
}
