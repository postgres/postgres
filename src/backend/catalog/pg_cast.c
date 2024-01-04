/*-------------------------------------------------------------------------
 *
 * pg_cast.c
 *	  routines to support manipulation of the pg_cast relation
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_cast.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/*
 * ----------------------------------------------------------------
 *		CastCreate
 *
 * Forms and inserts catalog tuples for a new cast being created.
 * Caller must have already checked privileges, and done consistency
 * checks on the given datatypes and cast function (if applicable).
 *
 * Since we allow binary coercibility of the datatypes to the cast
 * function's input and result, there could be one or two WITHOUT FUNCTION
 * casts that this one depends on.  We don't record that explicitly
 * in pg_cast, but we still need to make dependencies on those casts.
 *
 * 'behavior' indicates the types of the dependencies that the new
 * cast will have on its input and output types, the cast function,
 * and the other casts if any.
 * ----------------------------------------------------------------
 */
ObjectAddress
CastCreate(Oid sourcetypeid, Oid targettypeid,
		   Oid funcid, Oid incastid, Oid outcastid,
		   char castcontext, char castmethod, DependencyType behavior)
{
	Relation	relation;
	HeapTuple	tuple;
	Oid			castid;
	Datum		values[Natts_pg_cast];
	bool		nulls[Natts_pg_cast] = {0};
	ObjectAddress myself,
				referenced;
	ObjectAddresses *addrs;

	relation = table_open(CastRelationId, RowExclusiveLock);

	/*
	 * Check for duplicate.  This is just to give a friendly error message,
	 * the unique index would catch it anyway (so no need to sweat about race
	 * conditions).
	 */
	tuple = SearchSysCache2(CASTSOURCETARGET,
							ObjectIdGetDatum(sourcetypeid),
							ObjectIdGetDatum(targettypeid));
	if (HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("cast from type %s to type %s already exists",
						format_type_be(sourcetypeid),
						format_type_be(targettypeid))));

	/* ready to go */
	castid = GetNewOidWithIndex(relation, CastOidIndexId, Anum_pg_cast_oid);
	values[Anum_pg_cast_oid - 1] = ObjectIdGetDatum(castid);
	values[Anum_pg_cast_castsource - 1] = ObjectIdGetDatum(sourcetypeid);
	values[Anum_pg_cast_casttarget - 1] = ObjectIdGetDatum(targettypeid);
	values[Anum_pg_cast_castfunc - 1] = ObjectIdGetDatum(funcid);
	values[Anum_pg_cast_castcontext - 1] = CharGetDatum(castcontext);
	values[Anum_pg_cast_castmethod - 1] = CharGetDatum(castmethod);

	tuple = heap_form_tuple(RelationGetDescr(relation), values, nulls);

	CatalogTupleInsert(relation, tuple);

	addrs = new_object_addresses();

	/* make dependency entries */
	ObjectAddressSet(myself, CastRelationId, castid);

	/* dependency on source type */
	ObjectAddressSet(referenced, TypeRelationId, sourcetypeid);
	add_exact_object_address(&referenced, addrs);

	/* dependency on target type */
	ObjectAddressSet(referenced, TypeRelationId, targettypeid);
	add_exact_object_address(&referenced, addrs);

	/* dependency on function */
	if (OidIsValid(funcid))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, funcid);
		add_exact_object_address(&referenced, addrs);
	}

	/* dependencies on casts required for function */
	if (OidIsValid(incastid))
	{
		ObjectAddressSet(referenced, CastRelationId, incastid);
		add_exact_object_address(&referenced, addrs);
	}
	if (OidIsValid(outcastid))
	{
		ObjectAddressSet(referenced, CastRelationId, outcastid);
		add_exact_object_address(&referenced, addrs);
	}

	record_object_address_dependencies(&myself, addrs, behavior);
	free_object_addresses(addrs);

	/* dependency on extension */
	recordDependencyOnCurrentExtension(&myself, false);

	/* Post creation hook for new cast */
	InvokeObjectPostCreateHook(CastRelationId, castid, 0);

	heap_freetuple(tuple);

	table_close(relation, RowExclusiveLock);

	return myself;
}
