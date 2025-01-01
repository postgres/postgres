/*-------------------------------------------------------------------------
 *
 * pg_attrdef.c
 *	  routines to support manipulation of the pg_attrdef relation
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_attrdef.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_attrdef.h"
#include "executor/executor.h"
#include "optimizer/optimizer.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/syscache.h"


/*
 * Store a default expression for column attnum of relation rel.
 *
 * Returns the OID of the new pg_attrdef tuple.
 *
 * add_column_mode must be true if we are storing the default for a new
 * attribute, and false if it's for an already existing attribute. The reason
 * for this is that the missing value must never be updated after it is set,
 * which can only be when a column is added to the table. Otherwise we would
 * in effect be changing existing tuples.
 */
Oid
StoreAttrDefault(Relation rel, AttrNumber attnum,
				 Node *expr, bool is_internal, bool add_column_mode)
{
	char	   *adbin;
	Relation	adrel;
	HeapTuple	tuple;
	Datum		values[4];
	static bool nulls[4] = {false, false, false, false};
	Relation	attrrel;
	HeapTuple	atttup;
	Form_pg_attribute attStruct;
	char		attgenerated;
	Oid			attrdefOid;
	ObjectAddress colobject,
				defobject;

	adrel = table_open(AttrDefaultRelationId, RowExclusiveLock);

	/*
	 * Flatten expression to string form for storage.
	 */
	adbin = nodeToString(expr);

	/*
	 * Make the pg_attrdef entry.
	 */
	attrdefOid = GetNewOidWithIndex(adrel, AttrDefaultOidIndexId,
									Anum_pg_attrdef_oid);
	values[Anum_pg_attrdef_oid - 1] = ObjectIdGetDatum(attrdefOid);
	values[Anum_pg_attrdef_adrelid - 1] = RelationGetRelid(rel);
	values[Anum_pg_attrdef_adnum - 1] = attnum;
	values[Anum_pg_attrdef_adbin - 1] = CStringGetTextDatum(adbin);

	tuple = heap_form_tuple(adrel->rd_att, values, nulls);
	CatalogTupleInsert(adrel, tuple);

	defobject.classId = AttrDefaultRelationId;
	defobject.objectId = attrdefOid;
	defobject.objectSubId = 0;

	table_close(adrel, RowExclusiveLock);

	/* now can free some of the stuff allocated above */
	pfree(DatumGetPointer(values[Anum_pg_attrdef_adbin - 1]));
	heap_freetuple(tuple);
	pfree(adbin);

	/*
	 * Update the pg_attribute entry for the column to show that a default
	 * exists.
	 */
	attrrel = table_open(AttributeRelationId, RowExclusiveLock);
	atttup = SearchSysCacheCopy2(ATTNUM,
								 ObjectIdGetDatum(RelationGetRelid(rel)),
								 Int16GetDatum(attnum));
	if (!HeapTupleIsValid(atttup))
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 attnum, RelationGetRelid(rel));
	attStruct = (Form_pg_attribute) GETSTRUCT(atttup);
	attgenerated = attStruct->attgenerated;
	if (!attStruct->atthasdef)
	{
		Form_pg_attribute defAttStruct;

		ExprState  *exprState;
		Expr	   *expr2 = (Expr *) expr;
		EState	   *estate = NULL;
		ExprContext *econtext;
		Datum		valuesAtt[Natts_pg_attribute] = {0};
		bool		nullsAtt[Natts_pg_attribute] = {0};
		bool		replacesAtt[Natts_pg_attribute] = {0};
		Datum		missingval = (Datum) 0;
		bool		missingIsNull = true;

		valuesAtt[Anum_pg_attribute_atthasdef - 1] = true;
		replacesAtt[Anum_pg_attribute_atthasdef - 1] = true;

		if (rel->rd_rel->relkind == RELKIND_RELATION && add_column_mode &&
			!attgenerated)
		{
			expr2 = expression_planner(expr2);
			estate = CreateExecutorState();
			exprState = ExecPrepareExpr(expr2, estate);
			econtext = GetPerTupleExprContext(estate);

			missingval = ExecEvalExpr(exprState, econtext,
									  &missingIsNull);

			FreeExecutorState(estate);

			defAttStruct = TupleDescAttr(rel->rd_att, attnum - 1);

			if (missingIsNull)
			{
				/* if the default evaluates to NULL, just store a NULL array */
				missingval = (Datum) 0;
			}
			else
			{
				/* otherwise make a one-element array of the value */
				missingval = PointerGetDatum(construct_array(&missingval,
															 1,
															 defAttStruct->atttypid,
															 defAttStruct->attlen,
															 defAttStruct->attbyval,
															 defAttStruct->attalign));
			}

			valuesAtt[Anum_pg_attribute_atthasmissing - 1] = !missingIsNull;
			replacesAtt[Anum_pg_attribute_atthasmissing - 1] = true;
			valuesAtt[Anum_pg_attribute_attmissingval - 1] = missingval;
			replacesAtt[Anum_pg_attribute_attmissingval - 1] = true;
			nullsAtt[Anum_pg_attribute_attmissingval - 1] = missingIsNull;
		}
		atttup = heap_modify_tuple(atttup, RelationGetDescr(attrrel),
								   valuesAtt, nullsAtt, replacesAtt);

		CatalogTupleUpdate(attrrel, &atttup->t_self, atttup);

		if (!missingIsNull)
			pfree(DatumGetPointer(missingval));
	}
	table_close(attrrel, RowExclusiveLock);
	heap_freetuple(atttup);

	/*
	 * Make a dependency so that the pg_attrdef entry goes away if the column
	 * (or whole table) is deleted.  In the case of a generated column, make
	 * it an internal dependency to prevent the default expression from being
	 * deleted separately.
	 */
	colobject.classId = RelationRelationId;
	colobject.objectId = RelationGetRelid(rel);
	colobject.objectSubId = attnum;

	recordDependencyOn(&defobject, &colobject,
					   attgenerated ? DEPENDENCY_INTERNAL : DEPENDENCY_AUTO);

	/*
	 * Record dependencies on objects used in the expression, too.
	 */
	recordDependencyOnSingleRelExpr(&defobject, expr, RelationGetRelid(rel),
									DEPENDENCY_NORMAL,
									DEPENDENCY_NORMAL, false);

	/*
	 * Post creation hook for attribute defaults.
	 *
	 * XXX. ALTER TABLE ALTER COLUMN SET/DROP DEFAULT is implemented with a
	 * couple of deletion/creation of the attribute's default entry, so the
	 * callee should check existence of an older version of this entry if it
	 * needs to distinguish.
	 */
	InvokeObjectPostCreateHookArg(AttrDefaultRelationId,
								  RelationGetRelid(rel), attnum, is_internal);

	return attrdefOid;
}


/*
 *		RemoveAttrDefault
 *
 * If the specified relation/attribute has a default, remove it.
 * (If no default, raise error if complain is true, else return quietly.)
 */
void
RemoveAttrDefault(Oid relid, AttrNumber attnum,
				  DropBehavior behavior, bool complain, bool internal)
{
	Relation	attrdef_rel;
	ScanKeyData scankeys[2];
	SysScanDesc scan;
	HeapTuple	tuple;
	bool		found = false;

	attrdef_rel = table_open(AttrDefaultRelationId, RowExclusiveLock);

	ScanKeyInit(&scankeys[0],
				Anum_pg_attrdef_adrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&scankeys[1],
				Anum_pg_attrdef_adnum,
				BTEqualStrategyNumber, F_INT2EQ,
				Int16GetDatum(attnum));

	scan = systable_beginscan(attrdef_rel, AttrDefaultIndexId, true,
							  NULL, 2, scankeys);

	/* There should be at most one matching tuple, but we loop anyway */
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		ObjectAddress object;
		Form_pg_attrdef attrtuple = (Form_pg_attrdef) GETSTRUCT(tuple);

		object.classId = AttrDefaultRelationId;
		object.objectId = attrtuple->oid;
		object.objectSubId = 0;

		performDeletion(&object, behavior,
						internal ? PERFORM_DELETION_INTERNAL : 0);

		found = true;
	}

	systable_endscan(scan);
	table_close(attrdef_rel, RowExclusiveLock);

	if (complain && !found)
		elog(ERROR, "could not find attrdef tuple for relation %u attnum %d",
			 relid, attnum);
}

/*
 *		RemoveAttrDefaultById
 *
 * Remove a pg_attrdef entry specified by OID.  This is the guts of
 * attribute-default removal.  Note it should be called via performDeletion,
 * not directly.
 */
void
RemoveAttrDefaultById(Oid attrdefId)
{
	Relation	attrdef_rel;
	Relation	attr_rel;
	Relation	myrel;
	ScanKeyData scankeys[1];
	SysScanDesc scan;
	HeapTuple	tuple;
	Oid			myrelid;
	AttrNumber	myattnum;

	/* Grab an appropriate lock on the pg_attrdef relation */
	attrdef_rel = table_open(AttrDefaultRelationId, RowExclusiveLock);

	/* Find the pg_attrdef tuple */
	ScanKeyInit(&scankeys[0],
				Anum_pg_attrdef_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(attrdefId));

	scan = systable_beginscan(attrdef_rel, AttrDefaultOidIndexId, true,
							  NULL, 1, scankeys);

	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for attrdef %u", attrdefId);

	myrelid = ((Form_pg_attrdef) GETSTRUCT(tuple))->adrelid;
	myattnum = ((Form_pg_attrdef) GETSTRUCT(tuple))->adnum;

	/* Get an exclusive lock on the relation owning the attribute */
	myrel = relation_open(myrelid, AccessExclusiveLock);

	/* Now we can delete the pg_attrdef row */
	CatalogTupleDelete(attrdef_rel, &tuple->t_self);

	systable_endscan(scan);
	table_close(attrdef_rel, RowExclusiveLock);

	/* Fix the pg_attribute row */
	attr_rel = table_open(AttributeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy2(ATTNUM,
								ObjectIdGetDatum(myrelid),
								Int16GetDatum(myattnum));
	if (!HeapTupleIsValid(tuple))	/* shouldn't happen */
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 myattnum, myrelid);

	((Form_pg_attribute) GETSTRUCT(tuple))->atthasdef = false;

	CatalogTupleUpdate(attr_rel, &tuple->t_self, tuple);

	/*
	 * Our update of the pg_attribute row will force a relcache rebuild, so
	 * there's nothing else to do here.
	 */
	table_close(attr_rel, RowExclusiveLock);

	/* Keep lock on attribute's rel until end of xact */
	relation_close(myrel, NoLock);
}


/*
 * Get the pg_attrdef OID of the default expression for a column
 * identified by relation OID and column number.
 *
 * Returns InvalidOid if there is no such pg_attrdef entry.
 */
Oid
GetAttrDefaultOid(Oid relid, AttrNumber attnum)
{
	Oid			result = InvalidOid;
	Relation	attrdef;
	ScanKeyData keys[2];
	SysScanDesc scan;
	HeapTuple	tup;

	attrdef = table_open(AttrDefaultRelationId, AccessShareLock);
	ScanKeyInit(&keys[0],
				Anum_pg_attrdef_adrelid,
				BTEqualStrategyNumber,
				F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&keys[1],
				Anum_pg_attrdef_adnum,
				BTEqualStrategyNumber,
				F_INT2EQ,
				Int16GetDatum(attnum));
	scan = systable_beginscan(attrdef, AttrDefaultIndexId, true,
							  NULL, 2, keys);

	if (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_attrdef atdform = (Form_pg_attrdef) GETSTRUCT(tup);

		result = atdform->oid;
	}

	systable_endscan(scan);
	table_close(attrdef, AccessShareLock);

	return result;
}

/*
 * Given a pg_attrdef OID, return the relation OID and column number of
 * the owning column (represented as an ObjectAddress for convenience).
 *
 * Returns InvalidObjectAddress if there is no such pg_attrdef entry.
 */
ObjectAddress
GetAttrDefaultColumnAddress(Oid attrdefoid)
{
	ObjectAddress result = InvalidObjectAddress;
	Relation	attrdef;
	ScanKeyData skey[1];
	SysScanDesc scan;
	HeapTuple	tup;

	attrdef = table_open(AttrDefaultRelationId, AccessShareLock);
	ScanKeyInit(&skey[0],
				Anum_pg_attrdef_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(attrdefoid));
	scan = systable_beginscan(attrdef, AttrDefaultOidIndexId, true,
							  NULL, 1, skey);

	if (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_attrdef atdform = (Form_pg_attrdef) GETSTRUCT(tup);

		result.classId = RelationRelationId;
		result.objectId = atdform->adrelid;
		result.objectSubId = atdform->adnum;
	}

	systable_endscan(scan);
	table_close(attrdef, AccessShareLock);

	return result;
}
