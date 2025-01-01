/*-------------------------------------------------------------------------
 *
 * pg_depend.c
 *	  routines to support manipulation of the pg_depend relation
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_depend.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_extension.h"
#include "catalog/partition.h"
#include "commands/extension.h"
#include "miscadmin.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"


static bool isObjectPinned(const ObjectAddress *object);


/*
 * Record a dependency between 2 objects via their respective ObjectAddress.
 * The first argument is the dependent object, the second the one it
 * references.
 *
 * This simply creates an entry in pg_depend, without any other processing.
 */
void
recordDependencyOn(const ObjectAddress *depender,
				   const ObjectAddress *referenced,
				   DependencyType behavior)
{
	recordMultipleDependencies(depender, referenced, 1, behavior);
}

/*
 * Record multiple dependencies (of the same kind) for a single dependent
 * object.  This has a little less overhead than recording each separately.
 */
void
recordMultipleDependencies(const ObjectAddress *depender,
						   const ObjectAddress *referenced,
						   int nreferenced,
						   DependencyType behavior)
{
	Relation	dependDesc;
	CatalogIndexState indstate;
	TupleTableSlot **slot;
	int			i,
				max_slots,
				slot_init_count,
				slot_stored_count;

	if (nreferenced <= 0)
		return;					/* nothing to do */

	/*
	 * During bootstrap, do nothing since pg_depend may not exist yet.
	 *
	 * Objects created during bootstrap are most likely pinned, and the few
	 * that are not do not have dependencies on each other, so that there
	 * would be no need to make a pg_depend entry anyway.
	 */
	if (IsBootstrapProcessingMode())
		return;

	dependDesc = table_open(DependRelationId, RowExclusiveLock);

	/*
	 * Allocate the slots to use, but delay costly initialization until we
	 * know that they will be used.
	 */
	max_slots = Min(nreferenced,
					MAX_CATALOG_MULTI_INSERT_BYTES / sizeof(FormData_pg_depend));
	slot = palloc(sizeof(TupleTableSlot *) * max_slots);

	/* Don't open indexes unless we need to make an update */
	indstate = NULL;

	/* number of slots currently storing tuples */
	slot_stored_count = 0;
	/* number of slots currently initialized */
	slot_init_count = 0;
	for (i = 0; i < nreferenced; i++, referenced++)
	{
		/*
		 * If the referenced object is pinned by the system, there's no real
		 * need to record dependencies on it.  This saves lots of space in
		 * pg_depend, so it's worth the time taken to check.
		 */
		if (isObjectPinned(referenced))
			continue;

		if (slot_init_count < max_slots)
		{
			slot[slot_stored_count] = MakeSingleTupleTableSlot(RelationGetDescr(dependDesc),
															   &TTSOpsHeapTuple);
			slot_init_count++;
		}

		ExecClearTuple(slot[slot_stored_count]);

		/*
		 * Record the dependency.  Note we don't bother to check for duplicate
		 * dependencies; there's no harm in them.
		 */
		slot[slot_stored_count]->tts_values[Anum_pg_depend_refclassid - 1] = ObjectIdGetDatum(referenced->classId);
		slot[slot_stored_count]->tts_values[Anum_pg_depend_refobjid - 1] = ObjectIdGetDatum(referenced->objectId);
		slot[slot_stored_count]->tts_values[Anum_pg_depend_refobjsubid - 1] = Int32GetDatum(referenced->objectSubId);
		slot[slot_stored_count]->tts_values[Anum_pg_depend_deptype - 1] = CharGetDatum((char) behavior);
		slot[slot_stored_count]->tts_values[Anum_pg_depend_classid - 1] = ObjectIdGetDatum(depender->classId);
		slot[slot_stored_count]->tts_values[Anum_pg_depend_objid - 1] = ObjectIdGetDatum(depender->objectId);
		slot[slot_stored_count]->tts_values[Anum_pg_depend_objsubid - 1] = Int32GetDatum(depender->objectSubId);

		memset(slot[slot_stored_count]->tts_isnull, false,
			   slot[slot_stored_count]->tts_tupleDescriptor->natts * sizeof(bool));

		ExecStoreVirtualTuple(slot[slot_stored_count]);
		slot_stored_count++;

		/* If slots are full, insert a batch of tuples */
		if (slot_stored_count == max_slots)
		{
			/* fetch index info only when we know we need it */
			if (indstate == NULL)
				indstate = CatalogOpenIndexes(dependDesc);

			CatalogTuplesMultiInsertWithInfo(dependDesc, slot, slot_stored_count,
											 indstate);
			slot_stored_count = 0;
		}
	}

	/* Insert any tuples left in the buffer */
	if (slot_stored_count > 0)
	{
		/* fetch index info only when we know we need it */
		if (indstate == NULL)
			indstate = CatalogOpenIndexes(dependDesc);

		CatalogTuplesMultiInsertWithInfo(dependDesc, slot, slot_stored_count,
										 indstate);
	}

	if (indstate != NULL)
		CatalogCloseIndexes(indstate);

	table_close(dependDesc, RowExclusiveLock);

	/* Drop only the number of slots used */
	for (i = 0; i < slot_init_count; i++)
		ExecDropSingleTupleTableSlot(slot[i]);
	pfree(slot);
}

/*
 * If we are executing a CREATE EXTENSION operation, mark the given object
 * as being a member of the extension, or check that it already is one.
 * Otherwise, do nothing.
 *
 * This must be called during creation of any user-definable object type
 * that could be a member of an extension.
 *
 * isReplace must be true if the object already existed, and false if it is
 * newly created.  In the former case we insist that it already be a member
 * of the current extension.  In the latter case we can skip checking whether
 * it is already a member of any extension.
 *
 * Note: isReplace = true is typically used when updating an object in
 * CREATE OR REPLACE and similar commands.  We used to allow the target
 * object to not already be an extension member, instead silently absorbing
 * it into the current extension.  However, this was both error-prone
 * (extensions might accidentally overwrite free-standing objects) and
 * a security hazard (since the object would retain its previous ownership).
 */
void
recordDependencyOnCurrentExtension(const ObjectAddress *object,
								   bool isReplace)
{
	/* Only whole objects can be extension members */
	Assert(object->objectSubId == 0);

	if (creating_extension)
	{
		ObjectAddress extension;

		/* Only need to check for existing membership if isReplace */
		if (isReplace)
		{
			Oid			oldext;

			/*
			 * Side note: these catalog lookups are safe only because the
			 * object is a pre-existing one.  In the not-isReplace case, the
			 * caller has most likely not yet done a CommandCounterIncrement
			 * that would make the new object visible.
			 */
			oldext = getExtensionOfObject(object->classId, object->objectId);
			if (OidIsValid(oldext))
			{
				/* If already a member of this extension, nothing to do */
				if (oldext == CurrentExtensionObject)
					return;
				/* Already a member of some other extension, so reject */
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("%s is already a member of extension \"%s\"",
								getObjectDescription(object, false),
								get_extension_name(oldext))));
			}
			/* It's a free-standing object, so reject */
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("%s is not a member of extension \"%s\"",
							getObjectDescription(object, false),
							get_extension_name(CurrentExtensionObject)),
					 errdetail("An extension is not allowed to replace an object that it does not own.")));
		}

		/* OK, record it as a member of CurrentExtensionObject */
		extension.classId = ExtensionRelationId;
		extension.objectId = CurrentExtensionObject;
		extension.objectSubId = 0;

		recordDependencyOn(object, &extension, DEPENDENCY_EXTENSION);
	}
}

/*
 * If we are executing a CREATE EXTENSION operation, check that the given
 * object is a member of the extension, and throw an error if it isn't.
 * Otherwise, do nothing.
 *
 * This must be called whenever a CREATE IF NOT EXISTS operation (for an
 * object type that can be an extension member) has found that an object of
 * the desired name already exists.  It is insecure for an extension to use
 * IF NOT EXISTS except when the conflicting object is already an extension
 * member; otherwise a hostile user could substitute an object with arbitrary
 * properties.
 */
void
checkMembershipInCurrentExtension(const ObjectAddress *object)
{
	/*
	 * This is actually the same condition tested in
	 * recordDependencyOnCurrentExtension; but we want to issue a
	 * differently-worded error, and anyway it would be pretty confusing to
	 * call recordDependencyOnCurrentExtension in these circumstances.
	 */

	/* Only whole objects can be extension members */
	Assert(object->objectSubId == 0);

	if (creating_extension)
	{
		Oid			oldext;

		oldext = getExtensionOfObject(object->classId, object->objectId);
		/* If already a member of this extension, OK */
		if (oldext == CurrentExtensionObject)
			return;
		/* Else complain */
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("%s is not a member of extension \"%s\"",
						getObjectDescription(object, false),
						get_extension_name(CurrentExtensionObject)),
				 errdetail("An extension may only use CREATE ... IF NOT EXISTS to skip object creation if the conflicting object is one that it already owns.")));
	}
}

/*
 * deleteDependencyRecordsFor -- delete all records with given depender
 * classId/objectId.  Returns the number of records deleted.
 *
 * This is used when redefining an existing object.  Links leading to the
 * object do not change, and links leading from it will be recreated
 * (possibly with some differences from before).
 *
 * If skipExtensionDeps is true, we do not delete any dependencies that
 * show that the given object is a member of an extension.  This avoids
 * needing a lot of extra logic to fetch and recreate that dependency.
 */
long
deleteDependencyRecordsFor(Oid classId, Oid objectId,
						   bool skipExtensionDeps)
{
	long		count = 0;
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;

	depRel = table_open(DependRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objectId));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		if (skipExtensionDeps &&
			((Form_pg_depend) GETSTRUCT(tup))->deptype == DEPENDENCY_EXTENSION)
			continue;

		CatalogTupleDelete(depRel, &tup->t_self);
		count++;
	}

	systable_endscan(scan);

	table_close(depRel, RowExclusiveLock);

	return count;
}

/*
 * deleteDependencyRecordsForClass -- delete all records with given depender
 * classId/objectId, dependee classId, and deptype.
 * Returns the number of records deleted.
 *
 * This is a variant of deleteDependencyRecordsFor, useful when revoking
 * an object property that is expressed by a dependency record (such as
 * extension membership).
 */
long
deleteDependencyRecordsForClass(Oid classId, Oid objectId,
								Oid refclassId, char deptype)
{
	long		count = 0;
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;

	depRel = table_open(DependRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objectId));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend depform = (Form_pg_depend) GETSTRUCT(tup);

		if (depform->refclassid == refclassId && depform->deptype == deptype)
		{
			CatalogTupleDelete(depRel, &tup->t_self);
			count++;
		}
	}

	systable_endscan(scan);

	table_close(depRel, RowExclusiveLock);

	return count;
}

/*
 * deleteDependencyRecordsForSpecific -- delete all records with given depender
 * classId/objectId, dependee classId/objectId, of the given deptype.
 * Returns the number of records deleted.
 */
long
deleteDependencyRecordsForSpecific(Oid classId, Oid objectId, char deptype,
								   Oid refclassId, Oid refobjectId)
{
	long		count = 0;
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;

	depRel = table_open(DependRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objectId));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend depform = (Form_pg_depend) GETSTRUCT(tup);

		if (depform->refclassid == refclassId &&
			depform->refobjid == refobjectId &&
			depform->deptype == deptype)
		{
			CatalogTupleDelete(depRel, &tup->t_self);
			count++;
		}
	}

	systable_endscan(scan);

	table_close(depRel, RowExclusiveLock);

	return count;
}

/*
 * Adjust dependency record(s) to point to a different object of the same type
 *
 * classId/objectId specify the referencing object.
 * refClassId/oldRefObjectId specify the old referenced object.
 * newRefObjectId is the new referenced object (must be of class refClassId).
 *
 * Note the lack of objsubid parameters.  If there are subobject references
 * they will all be readjusted.  Also, there is an expectation that we are
 * dealing with NORMAL dependencies: if we have to replace an (implicit)
 * dependency on a pinned object with an explicit dependency on an unpinned
 * one, the new one will be NORMAL.
 *
 * Returns the number of records updated -- zero indicates a problem.
 */
long
changeDependencyFor(Oid classId, Oid objectId,
					Oid refClassId, Oid oldRefObjectId,
					Oid newRefObjectId)
{
	long		count = 0;
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	ObjectAddress objAddr;
	ObjectAddress depAddr;
	bool		oldIsPinned;
	bool		newIsPinned;

	/*
	 * Check to see if either oldRefObjectId or newRefObjectId is pinned.
	 * Pinned objects should not have any dependency entries pointing to them,
	 * so in these cases we should add or remove a pg_depend entry, or do
	 * nothing at all, rather than update an entry as in the normal case.
	 */
	objAddr.classId = refClassId;
	objAddr.objectId = oldRefObjectId;
	objAddr.objectSubId = 0;

	oldIsPinned = isObjectPinned(&objAddr);

	objAddr.objectId = newRefObjectId;

	newIsPinned = isObjectPinned(&objAddr);

	if (oldIsPinned)
	{
		/*
		 * If both are pinned, we need do nothing.  However, return 1 not 0,
		 * else callers will think this is an error case.
		 */
		if (newIsPinned)
			return 1;

		/*
		 * There is no old dependency record, but we should insert a new one.
		 * Assume a normal dependency is wanted.
		 */
		depAddr.classId = classId;
		depAddr.objectId = objectId;
		depAddr.objectSubId = 0;
		recordDependencyOn(&depAddr, &objAddr, DEPENDENCY_NORMAL);

		return 1;
	}

	depRel = table_open(DependRelationId, RowExclusiveLock);

	/* There should be existing dependency record(s), so search. */
	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objectId));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid((tup = systable_getnext(scan))))
	{
		Form_pg_depend depform = (Form_pg_depend) GETSTRUCT(tup);

		if (depform->refclassid == refClassId &&
			depform->refobjid == oldRefObjectId)
		{
			if (newIsPinned)
				CatalogTupleDelete(depRel, &tup->t_self);
			else
			{
				/* make a modifiable copy */
				tup = heap_copytuple(tup);
				depform = (Form_pg_depend) GETSTRUCT(tup);

				depform->refobjid = newRefObjectId;

				CatalogTupleUpdate(depRel, &tup->t_self, tup);

				heap_freetuple(tup);
			}

			count++;
		}
	}

	systable_endscan(scan);

	table_close(depRel, RowExclusiveLock);

	return count;
}

/*
 * Adjust all dependency records to come from a different object of the same type
 *
 * classId/oldObjectId specify the old referencing object.
 * newObjectId is the new referencing object (must be of class classId).
 *
 * Returns the number of records updated.
 */
long
changeDependenciesOf(Oid classId, Oid oldObjectId,
					 Oid newObjectId)
{
	long		count = 0;
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;

	depRel = table_open(DependRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(oldObjectId));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid((tup = systable_getnext(scan))))
	{
		Form_pg_depend depform;

		/* make a modifiable copy */
		tup = heap_copytuple(tup);
		depform = (Form_pg_depend) GETSTRUCT(tup);

		depform->objid = newObjectId;

		CatalogTupleUpdate(depRel, &tup->t_self, tup);

		heap_freetuple(tup);

		count++;
	}

	systable_endscan(scan);

	table_close(depRel, RowExclusiveLock);

	return count;
}

/*
 * Adjust all dependency records to point to a different object of the same type
 *
 * refClassId/oldRefObjectId specify the old referenced object.
 * newRefObjectId is the new referenced object (must be of class refClassId).
 *
 * Returns the number of records updated.
 */
long
changeDependenciesOn(Oid refClassId, Oid oldRefObjectId,
					 Oid newRefObjectId)
{
	long		count = 0;
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	ObjectAddress objAddr;
	bool		newIsPinned;

	depRel = table_open(DependRelationId, RowExclusiveLock);

	/*
	 * If oldRefObjectId is pinned, there won't be any dependency entries on
	 * it --- we can't cope in that case.  (This isn't really worth expending
	 * code to fix, in current usage; it just means you can't rename stuff out
	 * of pg_catalog, which would likely be a bad move anyway.)
	 */
	objAddr.classId = refClassId;
	objAddr.objectId = oldRefObjectId;
	objAddr.objectSubId = 0;

	if (isObjectPinned(&objAddr))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot remove dependency on %s because it is a system object",
						getObjectDescription(&objAddr, false))));

	/*
	 * We can handle adding a dependency on something pinned, though, since
	 * that just means deleting the dependency entry.
	 */
	objAddr.objectId = newRefObjectId;

	newIsPinned = isObjectPinned(&objAddr);

	/* Now search for dependency records */
	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(refClassId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(oldRefObjectId));

	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid((tup = systable_getnext(scan))))
	{
		if (newIsPinned)
			CatalogTupleDelete(depRel, &tup->t_self);
		else
		{
			Form_pg_depend depform;

			/* make a modifiable copy */
			tup = heap_copytuple(tup);
			depform = (Form_pg_depend) GETSTRUCT(tup);

			depform->refobjid = newRefObjectId;

			CatalogTupleUpdate(depRel, &tup->t_self, tup);

			heap_freetuple(tup);
		}

		count++;
	}

	systable_endscan(scan);

	table_close(depRel, RowExclusiveLock);

	return count;
}

/*
 * isObjectPinned()
 *
 * Test if an object is required for basic database functionality.
 *
 * The passed subId, if any, is ignored; we assume that only whole objects
 * are pinned (and that this implies pinning their components).
 */
static bool
isObjectPinned(const ObjectAddress *object)
{
	return IsPinnedObject(object->classId, object->objectId);
}


/*
 * Various special-purpose lookups and manipulations of pg_depend.
 */


/*
 * Find the extension containing the specified object, if any
 *
 * Returns the OID of the extension, or InvalidOid if the object does not
 * belong to any extension.
 *
 * Extension membership is marked by an EXTENSION dependency from the object
 * to the extension.  Note that the result will be indeterminate if pg_depend
 * contains links from this object to more than one extension ... but that
 * should never happen.
 */
Oid
getExtensionOfObject(Oid classId, Oid objectId)
{
	Oid			result = InvalidOid;
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;

	depRel = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objectId));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid((tup = systable_getnext(scan))))
	{
		Form_pg_depend depform = (Form_pg_depend) GETSTRUCT(tup);

		if (depform->refclassid == ExtensionRelationId &&
			depform->deptype == DEPENDENCY_EXTENSION)
		{
			result = depform->refobjid;
			break;				/* no need to keep scanning */
		}
	}

	systable_endscan(scan);

	table_close(depRel, AccessShareLock);

	return result;
}

/*
 * Return (possibly NIL) list of extensions that the given object depends on
 * in DEPENDENCY_AUTO_EXTENSION mode.
 */
List *
getAutoExtensionsOfObject(Oid classId, Oid objectId)
{
	List	   *result = NIL;
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;

	depRel = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objectId));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid((tup = systable_getnext(scan))))
	{
		Form_pg_depend depform = (Form_pg_depend) GETSTRUCT(tup);

		if (depform->refclassid == ExtensionRelationId &&
			depform->deptype == DEPENDENCY_AUTO_EXTENSION)
			result = lappend_oid(result, depform->refobjid);
	}

	systable_endscan(scan);

	table_close(depRel, AccessShareLock);

	return result;
}

/*
 * Detect whether a sequence is marked as "owned" by a column
 *
 * An ownership marker is an AUTO or INTERNAL dependency from the sequence to the
 * column.  If we find one, store the identity of the owning column
 * into *tableId and *colId and return true; else return false.
 *
 * Note: if there's more than one such pg_depend entry then you get
 * a random one of them returned into the out parameters.  This should
 * not happen, though.
 */
bool
sequenceIsOwned(Oid seqId, char deptype, Oid *tableId, int32 *colId)
{
	bool		ret = false;
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;

	depRel = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(seqId));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid((tup = systable_getnext(scan))))
	{
		Form_pg_depend depform = (Form_pg_depend) GETSTRUCT(tup);

		if (depform->refclassid == RelationRelationId &&
			depform->deptype == deptype)
		{
			*tableId = depform->refobjid;
			*colId = depform->refobjsubid;
			ret = true;
			break;				/* no need to keep scanning */
		}
	}

	systable_endscan(scan);

	table_close(depRel, AccessShareLock);

	return ret;
}

/*
 * Collect a list of OIDs of all sequences owned by the specified relation,
 * and column if specified.  If deptype is not zero, then only find sequences
 * with the specified dependency type.
 */
static List *
getOwnedSequences_internal(Oid relid, AttrNumber attnum, char deptype)
{
	List	   *result = NIL;
	Relation	depRel;
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple	tup;

	depRel = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	if (attnum)
		ScanKeyInit(&key[2],
					Anum_pg_depend_refobjsubid,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(attnum));

	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							  NULL, attnum ? 3 : 2, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend deprec = (Form_pg_depend) GETSTRUCT(tup);

		/*
		 * We assume any auto or internal dependency of a sequence on a column
		 * must be what we are looking for.  (We need the relkind test because
		 * indexes can also have auto dependencies on columns.)
		 */
		if (deprec->classid == RelationRelationId &&
			deprec->objsubid == 0 &&
			deprec->refobjsubid != 0 &&
			(deprec->deptype == DEPENDENCY_AUTO || deprec->deptype == DEPENDENCY_INTERNAL) &&
			get_rel_relkind(deprec->objid) == RELKIND_SEQUENCE)
		{
			if (!deptype || deprec->deptype == deptype)
				result = lappend_oid(result, deprec->objid);
		}
	}

	systable_endscan(scan);

	table_close(depRel, AccessShareLock);

	return result;
}

/*
 * Collect a list of OIDs of all sequences owned (identity or serial) by the
 * specified relation.
 */
List *
getOwnedSequences(Oid relid)
{
	return getOwnedSequences_internal(relid, 0, 0);
}

/*
 * Get owned identity sequence, error if not exactly one.
 */
Oid
getIdentitySequence(Relation rel, AttrNumber attnum, bool missing_ok)
{
	Oid			relid = RelationGetRelid(rel);
	List	   *seqlist;

	/*
	 * The identity sequence is associated with the topmost partitioned table,
	 * which might have column order different than the given partition.
	 */
	if (RelationGetForm(rel)->relispartition)
	{
		List	   *ancestors = get_partition_ancestors(relid);
		const char *attname = get_attname(relid, attnum, false);

		relid = llast_oid(ancestors);
		attnum = get_attnum(relid, attname);
		if (attnum == InvalidAttrNumber)
			elog(ERROR, "cache lookup failed for attribute \"%s\" of relation %u",
				 attname, relid);
		list_free(ancestors);
	}

	seqlist = getOwnedSequences_internal(relid, attnum, DEPENDENCY_INTERNAL);
	if (list_length(seqlist) > 1)
		elog(ERROR, "more than one owned sequence found");
	else if (seqlist == NIL)
	{
		if (missing_ok)
			return InvalidOid;
		else
			elog(ERROR, "no owned sequence found");
	}

	return linitial_oid(seqlist);
}

/*
 * get_index_constraint
 *		Given the OID of an index, return the OID of the owning unique,
 *		primary-key, or exclusion constraint, or InvalidOid if there
 *		is no owning constraint.
 */
Oid
get_index_constraint(Oid indexId)
{
	Oid			constraintId = InvalidOid;
	Relation	depRel;
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple	tup;

	/* Search the dependency table for the index */
	depRel = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(indexId));
	ScanKeyInit(&key[2],
				Anum_pg_depend_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(0));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  NULL, 3, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend deprec = (Form_pg_depend) GETSTRUCT(tup);

		/*
		 * We assume any internal dependency on a constraint must be what we
		 * are looking for.
		 */
		if (deprec->refclassid == ConstraintRelationId &&
			deprec->refobjsubid == 0 &&
			deprec->deptype == DEPENDENCY_INTERNAL)
		{
			constraintId = deprec->refobjid;
			break;
		}
	}

	systable_endscan(scan);
	table_close(depRel, AccessShareLock);

	return constraintId;
}

/*
 * get_index_ref_constraints
 *		Given the OID of an index, return the OID of all foreign key
 *		constraints which reference the index.
 */
List *
get_index_ref_constraints(Oid indexId)
{
	List	   *result = NIL;
	Relation	depRel;
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple	tup;

	/* Search the dependency table for the index */
	depRel = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(indexId));
	ScanKeyInit(&key[2],
				Anum_pg_depend_refobjsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(0));

	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							  NULL, 3, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend deprec = (Form_pg_depend) GETSTRUCT(tup);

		/*
		 * We assume any normal dependency from a constraint must be what we
		 * are looking for.
		 */
		if (deprec->classid == ConstraintRelationId &&
			deprec->objsubid == 0 &&
			deprec->deptype == DEPENDENCY_NORMAL)
		{
			result = lappend_oid(result, deprec->objid);
		}
	}

	systable_endscan(scan);
	table_close(depRel, AccessShareLock);

	return result;
}
