/*-------------------------------------------------------------------------
 *
 * pg_shdepend.c
 *	  routines to support manipulation of the pg_shdepend relation
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/catalog/pg_shdepend.c,v 1.15 2006/10/04 00:29:50 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_database.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_shdepend.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_type.h"
#include "commands/conversioncmds.h"
#include "commands/defrem.h"
#include "commands/schemacmds.h"
#include "commands/tablecmds.h"
#include "commands/typecmds.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/syscache.h"


typedef enum
{
	LOCAL_OBJECT,
	SHARED_OBJECT,
	REMOTE_OBJECT
} objectType;

static int getOidListDiff(Oid *list1, int nlist1, Oid *list2, int nlist2,
			   Oid **diff);
static Oid	classIdGetDbId(Oid classId);
static void shdepLockAndCheckObject(Oid classId, Oid objectId);
static void shdepChangeDep(Relation sdepRel, Oid classid, Oid objid,
			   Oid refclassid, Oid refobjid,
			   SharedDependencyType deptype);
static void shdepAddDependency(Relation sdepRel, Oid classId, Oid objectId,
				   Oid refclassId, Oid refobjId,
				   SharedDependencyType deptype);
static void shdepDropDependency(Relation sdepRel, Oid classId, Oid objectId,
					Oid refclassId, Oid refobjId,
					SharedDependencyType deptype);
static void storeObjectDescription(StringInfo descs, objectType type,
					   ObjectAddress *object,
					   SharedDependencyType deptype,
					   int count);
static bool isSharedObjectPinned(Oid classId, Oid objectId, Relation sdepRel);


/*
 * recordSharedDependencyOn
 *
 * Record a dependency between 2 objects via their respective ObjectAddresses.
 * The first argument is the dependent object, the second the one it
 * references (which must be a shared object).
 *
 * This locks the referenced object and makes sure it still exists.
 * Then it creates an entry in pg_shdepend.  The lock is kept until
 * the end of the transaction.
 *
 * Dependencies on pinned objects are not recorded.
 */
void
recordSharedDependencyOn(ObjectAddress *depender,
						 ObjectAddress *referenced,
						 SharedDependencyType deptype)
{
	Relation	sdepRel;

	/*
	 * Objects in pg_shdepend can't have SubIds.
	 */
	Assert(depender->objectSubId == 0);
	Assert(referenced->objectSubId == 0);

	/*
	 * During bootstrap, do nothing since pg_shdepend may not exist yet.
	 * initdb will fill in appropriate pg_shdepend entries after bootstrap.
	 */
	if (IsBootstrapProcessingMode())
		return;

	sdepRel = heap_open(SharedDependRelationId, RowExclusiveLock);

	/* If the referenced object is pinned, do nothing. */
	if (!isSharedObjectPinned(referenced->classId, referenced->objectId,
							  sdepRel))
	{
		shdepAddDependency(sdepRel, depender->classId, depender->objectId,
						   referenced->classId, referenced->objectId,
						   deptype);
	}

	heap_close(sdepRel, RowExclusiveLock);
}

/*
 * recordDependencyOnOwner
 *
 * A convenient wrapper of recordSharedDependencyOn -- register the specified
 * user as owner of the given object.
 *
 * Note: it's the caller's responsibility to ensure that there isn't an owner
 * entry for the object already.
 */
void
recordDependencyOnOwner(Oid classId, Oid objectId, Oid owner)
{
	ObjectAddress myself,
				referenced;

	myself.classId = classId;
	myself.objectId = objectId;
	myself.objectSubId = 0;

	referenced.classId = AuthIdRelationId;
	referenced.objectId = owner;
	referenced.objectSubId = 0;

	recordSharedDependencyOn(&myself, &referenced, SHARED_DEPENDENCY_OWNER);
}

/*
 * shdepChangeDep
 *
 * Update shared dependency records to account for an updated referenced
 * object.	This is an internal workhorse for operations such as changing
 * an object's owner.
 *
 * There must be no more than one existing entry for the given dependent
 * object and dependency type!	So in practice this can only be used for
 * updating SHARED_DEPENDENCY_OWNER entries, which should have that property.
 *
 * If there is no previous entry, we assume it was referencing a PINned
 * object, so we create a new entry.  If the new referenced object is
 * PINned, we don't create an entry (and drop the old one, if any).
 *
 * sdepRel must be the pg_shdepend relation, already opened and suitably
 * locked.
 */
static void
shdepChangeDep(Relation sdepRel, Oid classid, Oid objid,
			   Oid refclassid, Oid refobjid,
			   SharedDependencyType deptype)
{
	Oid			dbid = classIdGetDbId(classid);
	HeapTuple	oldtup = NULL;
	HeapTuple	scantup;
	ScanKeyData key[3];
	SysScanDesc scan;

	/*
	 * Make sure the new referenced object doesn't go away while we record the
	 * dependency.
	 */
	shdepLockAndCheckObject(refclassid, refobjid);

	/*
	 * Look for a previous entry
	 */
	ScanKeyInit(&key[0],
				Anum_pg_shdepend_dbid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(dbid));
	ScanKeyInit(&key[1],
				Anum_pg_shdepend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classid));
	ScanKeyInit(&key[2],
				Anum_pg_shdepend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objid));

	scan = systable_beginscan(sdepRel, SharedDependDependerIndexId, true,
							  SnapshotNow, 3, key);

	while ((scantup = systable_getnext(scan)) != NULL)
	{
		/* Ignore if not of the target dependency type */
		if (((Form_pg_shdepend) GETSTRUCT(scantup))->deptype != deptype)
			continue;
		/* Caller screwed up if multiple matches */
		if (oldtup)
			elog(ERROR,
				 "multiple pg_shdepend entries for object %u/%u deptype %c",
				 classid, objid, deptype);
		oldtup = heap_copytuple(scantup);
	}

	systable_endscan(scan);

	if (isSharedObjectPinned(refclassid, refobjid, sdepRel))
	{
		/* No new entry needed, so just delete existing entry if any */
		if (oldtup)
			simple_heap_delete(sdepRel, &oldtup->t_self);
	}
	else if (oldtup)
	{
		/* Need to update existing entry */
		Form_pg_shdepend shForm = (Form_pg_shdepend) GETSTRUCT(oldtup);

		/* Since oldtup is a copy, we can just modify it in-memory */
		shForm->refclassid = refclassid;
		shForm->refobjid = refobjid;

		simple_heap_update(sdepRel, &oldtup->t_self, oldtup);

		/* keep indexes current */
		CatalogUpdateIndexes(sdepRel, oldtup);
	}
	else
	{
		/* Need to insert new entry */
		Datum		values[Natts_pg_shdepend];
		bool		nulls[Natts_pg_shdepend];

		memset(nulls, 0, sizeof(nulls));

		values[Anum_pg_shdepend_dbid - 1] = ObjectIdGetDatum(dbid);
		values[Anum_pg_shdepend_classid - 1] = ObjectIdGetDatum(classid);
		values[Anum_pg_shdepend_objid - 1] = ObjectIdGetDatum(objid);

		values[Anum_pg_shdepend_refclassid - 1] = ObjectIdGetDatum(refclassid);
		values[Anum_pg_shdepend_refobjid - 1] = ObjectIdGetDatum(refobjid);
		values[Anum_pg_shdepend_deptype - 1] = CharGetDatum(deptype);

		/*
		 * we are reusing oldtup just to avoid declaring a new variable, but
		 * it's certainly a new tuple
		 */
		oldtup = heap_form_tuple(RelationGetDescr(sdepRel), values, nulls);
		simple_heap_insert(sdepRel, oldtup);

		/* keep indexes current */
		CatalogUpdateIndexes(sdepRel, oldtup);
	}

	if (oldtup)
		heap_freetuple(oldtup);
}

/*
 * changeDependencyOnOwner
 *
 * Update the shared dependencies to account for the new owner.
 */
void
changeDependencyOnOwner(Oid classId, Oid objectId, Oid newOwnerId)
{
	Relation	sdepRel;

	sdepRel = heap_open(SharedDependRelationId, RowExclusiveLock);

	/* Adjust the SHARED_DEPENDENCY_OWNER entry */
	shdepChangeDep(sdepRel, classId, objectId,
				   AuthIdRelationId, newOwnerId,
				   SHARED_DEPENDENCY_OWNER);

	/*----------
	 * There should never be a SHARED_DEPENDENCY_ACL entry for the owner,
	 * so get rid of it if there is one.  This can happen if the new owner
	 * was previously granted some rights to the object.
	 *
	 * This step is analogous to aclnewowner's removal of duplicate entries
	 * in the ACL.	We have to do it to handle this scenario:
	 *		A grants some rights on an object to B
	 *		ALTER OWNER changes the object's owner to B
	 *		ALTER OWNER changes the object's owner to C
	 * The third step would remove all mention of B from the object's ACL,
	 * but we'd still have a SHARED_DEPENDENCY_ACL for B if we did not do
	 * things this way.
	 *
	 * The rule against having a SHARED_DEPENDENCY_ACL entry for the owner
	 * allows us to fix things up in just this one place, without having
	 * to make the various ALTER OWNER routines each know about it.
	 *----------
	 */
	shdepDropDependency(sdepRel, classId, objectId,
						AuthIdRelationId, newOwnerId,
						SHARED_DEPENDENCY_ACL);

	heap_close(sdepRel, RowExclusiveLock);
}

/*
 * getOidListDiff
 *		Helper for updateAclDependencies.
 *
 * Takes two Oid arrays and returns elements from the first not found in the
 * second.	We assume both arrays are sorted and de-duped, and that the
 * second array does not contain any values not found in the first.
 *
 * NOTE: Both input arrays are pfreed.
 */
static int
getOidListDiff(Oid *list1, int nlist1, Oid *list2, int nlist2, Oid **diff)
{
	Oid		   *result;
	int			i,
				j,
				k = 0;

	AssertArg(nlist1 >= nlist2 && nlist2 >= 0);

	result = palloc(sizeof(Oid) * (nlist1 - nlist2));
	*diff = result;

	for (i = 0, j = 0; i < nlist1 && j < nlist2;)
	{
		if (list1[i] == list2[j])
		{
			i++;
			j++;
		}
		else if (list1[i] < list2[j])
		{
			result[k++] = list1[i];
			i++;
		}
		else
		{
			/* can't happen */
			elog(WARNING, "invalid element %u in shorter list", list2[j]);
			j++;
		}
	}

	for (; i < nlist1; i++)
		result[k++] = list1[i];

	/* We should have copied the exact number of elements */
	AssertState(k == (nlist1 - nlist2));

	if (list1)
		pfree(list1);
	if (list2)
		pfree(list2);

	return k;
}

/*
 * updateAclDependencies
 *		Update the pg_shdepend info for an object's ACL during GRANT/REVOKE.
 *
 * classId, objectId: identify the object whose ACL this is
 * ownerId: role owning the object
 * isGrant: are we adding or removing ACL entries?
 * noldmembers, oldmembers: array of roleids appearing in old ACL
 * nnewmembers, newmembers: array of roleids appearing in new ACL
 *
 * We calculate the difference between the new and old lists of roles,
 * and then insert (if it's a grant) or delete (if it's a revoke) from
 * pg_shdepend as appropiate.
 *
 * Note that we can't insert blindly at grant, because we would end up with
 * duplicate registered dependencies.  We could check for existence of the
 * tuple before inserting, but that seems to be more expensive than what we are
 * doing now.  On the other hand, we can't just delete the tuples blindly at
 * revoke, because the user may still have other privileges.
 *
 * NOTE: Both input arrays must be sorted and de-duped.  They are pfreed
 * before return.
 */
void
updateAclDependencies(Oid classId, Oid objectId, Oid ownerId, bool isGrant,
					  int noldmembers, Oid *oldmembers,
					  int nnewmembers, Oid *newmembers)
{
	Relation	sdepRel;
	Oid		   *diff;
	int			ndiff,
				i;

	/*
	 * Calculate the differences between the old and new lists.
	 */
	if (isGrant)
		ndiff = getOidListDiff(newmembers, nnewmembers,
							   oldmembers, noldmembers, &diff);
	else
		ndiff = getOidListDiff(oldmembers, noldmembers,
							   newmembers, nnewmembers, &diff);

	if (ndiff > 0)
	{
		sdepRel = heap_open(SharedDependRelationId, RowExclusiveLock);

		/* Add or drop the respective dependency */
		for (i = 0; i < ndiff; i++)
		{
			Oid			roleid = diff[i];

			/*
			 * Skip the owner: he has an OWNER shdep entry instead. (This is
			 * not just a space optimization; it makes ALTER OWNER easier. See
			 * notes in changeDependencyOnOwner.)
			 */
			if (roleid == ownerId)
				continue;

			/* Skip pinned roles */
			if (isSharedObjectPinned(AuthIdRelationId, roleid, sdepRel))
				continue;

			if (isGrant)
				shdepAddDependency(sdepRel, classId, objectId,
								   AuthIdRelationId, roleid,
								   SHARED_DEPENDENCY_ACL);
			else
				shdepDropDependency(sdepRel, classId, objectId,
									AuthIdRelationId, roleid,
									SHARED_DEPENDENCY_ACL);
		}

		heap_close(sdepRel, RowExclusiveLock);
	}

	pfree(diff);
}

/*
 * A struct to keep track of dependencies found in other databases.
 */
typedef struct
{
	Oid			dbOid;
	int			count;
} remoteDep;

/*
 * checkSharedDependencies
 *
 * Check whether there are shared dependency entries for a given shared
 * object.	Returns a string containing a newline-separated list of object
 * descriptions that depend on the shared object, or NULL if none is found.
 *
 * We can find three different kinds of dependencies: dependencies on objects
 * of the current database; dependencies on shared objects; and dependencies
 * on objects local to other databases.  We can (and do) provide descriptions
 * of the two former kinds of objects, but we can't do that for "remote"
 * objects, so we just provide a count of them.
 *
 * If we find a SHARED_DEPENDENCY_PIN entry, we can error out early.
 */
char *
checkSharedDependencies(Oid classId, Oid objectId)
{
	Relation	sdepRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	int			totalDeps = 0;
	int			numLocalDeps = 0;
	int			numSharedDeps = 0;
	List	   *remDeps = NIL;
	ListCell   *cell;
	ObjectAddress object;
	StringInfoData descs;

	/*
	 * We try to limit the number of reported dependencies to something sane,
	 * both for the user's sake and to avoid blowing out memory.
	 */
#define MAX_REPORTED_DEPS 100

	initStringInfo(&descs);

	sdepRel = heap_open(SharedDependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_shdepend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classId));
	ScanKeyInit(&key[1],
				Anum_pg_shdepend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objectId));

	scan = systable_beginscan(sdepRel, SharedDependReferenceIndexId, true,
							  SnapshotNow, 2, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_shdepend sdepForm = (Form_pg_shdepend) GETSTRUCT(tup);

		/* This case can be dispatched quickly */
		if (sdepForm->deptype == SHARED_DEPENDENCY_PIN)
		{
			object.classId = classId;
			object.objectId = objectId;
			object.objectSubId = 0;
			ereport(ERROR,
					(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
					 errmsg("cannot drop %s because it is required by the database system",
							getObjectDescription(&object))));
		}

		object.classId = sdepForm->classid;
		object.objectId = sdepForm->objid;
		object.objectSubId = 0;

		/*
		 * If it's a dependency local to this database or it's a shared
		 * object, describe it.
		 *
		 * If it's a remote dependency, keep track of it so we can report the
		 * number of them later.
		 */
		if (sdepForm->dbid == MyDatabaseId)
		{
			numLocalDeps++;
			if (++totalDeps <= MAX_REPORTED_DEPS)
				storeObjectDescription(&descs, LOCAL_OBJECT, &object,
									   sdepForm->deptype, 0);
		}
		else if (sdepForm->dbid == InvalidOid)
		{
			numSharedDeps++;
			if (++totalDeps <= MAX_REPORTED_DEPS)
				storeObjectDescription(&descs, SHARED_OBJECT, &object,
									   sdepForm->deptype, 0);
		}
		else
		{
			/* It's not local nor shared, so it must be remote. */
			remoteDep  *dep;
			bool		stored = false;

			/*
			 * XXX this info is kept on a simple List.	Maybe it's not good
			 * for performance, but using a hash table seems needlessly
			 * complex.  The expected number of databases is not high anyway,
			 * I suppose.
			 */
			foreach(cell, remDeps)
			{
				dep = lfirst(cell);
				if (dep->dbOid == sdepForm->dbid)
				{
					dep->count++;
					stored = true;
					break;
				}
			}
			if (!stored)
			{
				dep = (remoteDep *) palloc(sizeof(remoteDep));
				dep->dbOid = sdepForm->dbid;
				dep->count = 1;
				remDeps = lappend(remDeps, dep);
				totalDeps++;
			}
		}
	}

	systable_endscan(scan);

	heap_close(sdepRel, AccessShareLock);

	if (totalDeps > MAX_REPORTED_DEPS)
	{
		/*
		 * Report seems unreasonably long, so reduce it to per-database info
		 *
		 * Note: we don't ever suppress per-database totals, which should be
		 * OK as long as there aren't too many databases ...
		 */
		descs.len = 0;			/* reset to empty */
		descs.data[0] = '\0';

		if (numLocalDeps > 0)
		{
			appendStringInfo(&descs, _("%d objects in this database"),
							 numLocalDeps);
			if (numSharedDeps > 0)
				appendStringInfoChar(&descs, '\n');
		}
		if (numSharedDeps > 0)
			appendStringInfo(&descs, _("%d shared objects"),
							 numSharedDeps);
	}

	foreach(cell, remDeps)
	{
		remoteDep  *dep = lfirst(cell);

		object.classId = DatabaseRelationId;
		object.objectId = dep->dbOid;
		object.objectSubId = 0;

		storeObjectDescription(&descs, REMOTE_OBJECT, &object,
							   SHARED_DEPENDENCY_INVALID, dep->count);
	}

	list_free_deep(remDeps);

	if (descs.len == 0)
	{
		pfree(descs.data);
		return NULL;
	}

	return descs.data;
}

/*
 * copyTemplateDependencies
 *
 * Routine to create the initial shared dependencies of a new database.
 * We simply copy the dependencies from the template database.
 */
void
copyTemplateDependencies(Oid templateDbId, Oid newDbId)
{
	Relation	sdepRel;
	TupleDesc	sdepDesc;
	ScanKeyData key[1];
	SysScanDesc scan;
	HeapTuple	tup;
	CatalogIndexState indstate;
	Datum		values[Natts_pg_shdepend];
	bool		nulls[Natts_pg_shdepend];
	bool		replace[Natts_pg_shdepend];

	sdepRel = heap_open(SharedDependRelationId, RowExclusiveLock);
	sdepDesc = RelationGetDescr(sdepRel);

	indstate = CatalogOpenIndexes(sdepRel);

	/* Scan all entries with dbid = templateDbId */
	ScanKeyInit(&key[0],
				Anum_pg_shdepend_dbid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(templateDbId));

	scan = systable_beginscan(sdepRel, SharedDependDependerIndexId, true,
							  SnapshotNow, 1, key);

	/* Set up to copy the tuples except for inserting newDbId */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	memset(replace, false, sizeof(replace));

	replace[Anum_pg_shdepend_dbid - 1] = true;
	values[Anum_pg_shdepend_dbid - 1] = ObjectIdGetDatum(newDbId);

	/*
	 * Copy the entries of the original database, changing the database Id to
	 * that of the new database.  Note that because we are not copying rows
	 * with dbId == 0 (ie, rows describing dependent shared objects) we won't
	 * copy the ownership dependency of the template database itself; this is
	 * what we want.
	 */
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		HeapTuple	newtup;

		newtup = heap_modify_tuple(tup, sdepDesc, values, nulls, replace);
		simple_heap_insert(sdepRel, newtup);

		/* Keep indexes current */
		CatalogIndexInsert(indstate, newtup);

		heap_freetuple(newtup);
	}

	systable_endscan(scan);

	CatalogCloseIndexes(indstate);
	heap_close(sdepRel, RowExclusiveLock);
}

/*
 * dropDatabaseDependencies
 *
 * Delete pg_shdepend entries corresponding to a database that's being
 * dropped.
 */
void
dropDatabaseDependencies(Oid databaseId)
{
	Relation	sdepRel;
	ScanKeyData key[1];
	SysScanDesc scan;
	HeapTuple	tup;

	sdepRel = heap_open(SharedDependRelationId, RowExclusiveLock);

	/*
	 * First, delete all the entries that have the database Oid in the dbid
	 * field.
	 */
	ScanKeyInit(&key[0],
				Anum_pg_shdepend_dbid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(databaseId));
	/* We leave the other index fields unspecified */

	scan = systable_beginscan(sdepRel, SharedDependDependerIndexId, true,
							  SnapshotNow, 1, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		simple_heap_delete(sdepRel, &tup->t_self);
	}

	systable_endscan(scan);

	/* Now delete all entries corresponding to the database itself */
	shdepDropDependency(sdepRel, DatabaseRelationId, databaseId,
						InvalidOid, InvalidOid,
						SHARED_DEPENDENCY_INVALID);

	heap_close(sdepRel, RowExclusiveLock);
}

/*
 * deleteSharedDependencyRecordsFor
 *
 * Delete all pg_shdepend entries corresponding to an object that's being
 * dropped or modified.  The object is assumed to be either a shared object
 * or local to the current database (the classId tells us which).
 */
void
deleteSharedDependencyRecordsFor(Oid classId, Oid objectId)
{
	Relation	sdepRel;

	sdepRel = heap_open(SharedDependRelationId, RowExclusiveLock);

	shdepDropDependency(sdepRel, classId, objectId,
						InvalidOid, InvalidOid,
						SHARED_DEPENDENCY_INVALID);

	heap_close(sdepRel, RowExclusiveLock);
}

/*
 * shdepAddDependency
 *		Internal workhorse for inserting into pg_shdepend
 *
 * sdepRel must be the pg_shdepend relation, already opened and suitably
 * locked.
 */
static void
shdepAddDependency(Relation sdepRel, Oid classId, Oid objectId,
				   Oid refclassId, Oid refobjId,
				   SharedDependencyType deptype)
{
	HeapTuple	tup;
	Datum		values[Natts_pg_shdepend];
	bool		nulls[Natts_pg_shdepend];

	/*
	 * Make sure the object doesn't go away while we record the dependency on
	 * it.	DROP routines should lock the object exclusively before they check
	 * shared dependencies.
	 */
	shdepLockAndCheckObject(refclassId, refobjId);

	memset(nulls, false, sizeof(nulls));

	/*
	 * Form the new tuple and record the dependency.
	 */
	values[Anum_pg_shdepend_dbid - 1] = ObjectIdGetDatum(classIdGetDbId(classId));
	values[Anum_pg_shdepend_classid - 1] = ObjectIdGetDatum(classId);
	values[Anum_pg_shdepend_objid - 1] = ObjectIdGetDatum(objectId);

	values[Anum_pg_shdepend_refclassid - 1] = ObjectIdGetDatum(refclassId);
	values[Anum_pg_shdepend_refobjid - 1] = ObjectIdGetDatum(refobjId);
	values[Anum_pg_shdepend_deptype - 1] = CharGetDatum(deptype);

	tup = heap_form_tuple(sdepRel->rd_att, values, nulls);

	simple_heap_insert(sdepRel, tup);

	/* keep indexes current */
	CatalogUpdateIndexes(sdepRel, tup);

	/* clean up */
	heap_freetuple(tup);
}

/*
 * shdepDropDependency
 *		Internal workhorse for deleting entries from pg_shdepend.
 *
 * We drop entries having the following properties:
 *	dependent object is the one identified by classId/objectId
 *	if refclassId isn't InvalidOid, it must match the entry's refclassid
 *	if refobjId isn't InvalidOid, it must match the entry's refobjid
 *	if deptype isn't SHARED_DEPENDENCY_INVALID, it must match entry's deptype
 *
 * sdepRel must be the pg_shdepend relation, already opened and suitably
 * locked.
 */
static void
shdepDropDependency(Relation sdepRel, Oid classId, Oid objectId,
					Oid refclassId, Oid refobjId,
					SharedDependencyType deptype)
{
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple	tup;

	/* Scan for entries matching the dependent object */
	ScanKeyInit(&key[0],
				Anum_pg_shdepend_dbid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classIdGetDbId(classId)));
	ScanKeyInit(&key[1],
				Anum_pg_shdepend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classId));
	ScanKeyInit(&key[2],
				Anum_pg_shdepend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objectId));

	scan = systable_beginscan(sdepRel, SharedDependDependerIndexId, true,
							  SnapshotNow, 3, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_shdepend shdepForm = (Form_pg_shdepend) GETSTRUCT(tup);

		/* Filter entries according to additional parameters */
		if (OidIsValid(refclassId) && shdepForm->refclassid != refclassId)
			continue;
		if (OidIsValid(refobjId) && shdepForm->refobjid != refobjId)
			continue;
		if (deptype != SHARED_DEPENDENCY_INVALID &&
			shdepForm->deptype != deptype)
			continue;

		/* OK, delete it */
		simple_heap_delete(sdepRel, &tup->t_self);
	}

	systable_endscan(scan);
}

/*
 * classIdGetDbId
 *
 * Get the database Id that should be used in pg_shdepend, given the OID
 * of the catalog containing the object.  For shared objects, it's 0
 * (InvalidOid); for all other objects, it's the current database Id.
 */
static Oid
classIdGetDbId(Oid classId)
{
	Oid			dbId;

	if (IsSharedRelation(classId))
		dbId = InvalidOid;
	else
		dbId = MyDatabaseId;

	return dbId;
}

/*
 * shdepLockAndCheckObject
 *
 * Lock the object that we are about to record a dependency on.
 * After it's locked, verify that it hasn't been dropped while we
 * weren't looking.  If the object has been dropped, this function
 * does not return!
 */
static void
shdepLockAndCheckObject(Oid classId, Oid objectId)
{
	/* AccessShareLock should be OK, since we are not modifying the object */
	LockSharedObject(classId, objectId, 0, AccessShareLock);

	switch (classId)
	{
		case AuthIdRelationId:
			if (!SearchSysCacheExists(AUTHOID,
									  ObjectIdGetDatum(objectId),
									  0, 0, 0))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("role %u was concurrently dropped",
								objectId)));
			break;

			/*
			 * Currently, this routine need not support any other shared
			 * object types besides roles.	If we wanted to record explicit
			 * dependencies on databases or tablespaces, we'd need code along
			 * these lines:
			 */
#ifdef NOT_USED
		case TableSpaceRelationId:
			{
				/* For lack of a syscache on pg_tablespace, do this: */
				char	   *tablespace = get_tablespace_name(objectId);

				if (tablespace == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("tablespace %u was concurrently dropped",
									objectId)));
				pfree(tablespace);
				break;
			}
#endif

		default:
			elog(ERROR, "unrecognized shared classId: %u", classId);
	}
}


/*
 * storeObjectDescription
 *		Append the description of a dependent object to "descs"
 *
 * While searching for dependencies of a shared object, we stash the
 * descriptions of dependent objects we find in a single string, which we
 * later pass to ereport() in the DETAIL field when somebody attempts to
 * drop a referenced shared object.
 *
 * When type is LOCAL_OBJECT or SHARED_OBJECT, we expect object to be the
 * dependent object, deptype is the dependency type, and count is not used.
 * When type is REMOTE_OBJECT, we expect object to be the database object,
 * and count to be nonzero; deptype is not used in this case.
 */
static void
storeObjectDescription(StringInfo descs, objectType type,
					   ObjectAddress *object,
					   SharedDependencyType deptype,
					   int count)
{
	char	   *objdesc = getObjectDescription(object);

	/* separate entries with a newline */
	if (descs->len != 0)
		appendStringInfoChar(descs, '\n');

	switch (type)
	{
		case LOCAL_OBJECT:
		case SHARED_OBJECT:
			if (deptype == SHARED_DEPENDENCY_OWNER)
				appendStringInfo(descs, _("owner of %s"), objdesc);
			else if (deptype == SHARED_DEPENDENCY_ACL)
				appendStringInfo(descs, _("access to %s"), objdesc);
			else
				elog(ERROR, "unrecognized dependency type: %d",
					 (int) deptype);
			break;

		case REMOTE_OBJECT:
			/* translator: %s will always be "database %s" */
			appendStringInfo(descs, _("%d objects in %s"), count, objdesc);
			break;

		default:
			elog(ERROR, "unrecognized object type: %d", type);
	}

	pfree(objdesc);
}


/*
 * isSharedObjectPinned
 *		Return whether a given shared object has a SHARED_DEPENDENCY_PIN entry.
 *
 * sdepRel must be the pg_shdepend relation, already opened and suitably
 * locked.
 */
static bool
isSharedObjectPinned(Oid classId, Oid objectId, Relation sdepRel)
{
	bool		result = false;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;

	ScanKeyInit(&key[0],
				Anum_pg_shdepend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classId));
	ScanKeyInit(&key[1],
				Anum_pg_shdepend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objectId));

	scan = systable_beginscan(sdepRel, SharedDependReferenceIndexId, true,
							  SnapshotNow, 2, key);

	/*
	 * Since we won't generate additional pg_shdepend entries for pinned
	 * objects, there can be at most one entry referencing a pinned object.
	 * Hence, it's sufficient to look at the first returned tuple; we don't
	 * need to loop.
	 */
	tup = systable_getnext(scan);
	if (HeapTupleIsValid(tup))
	{
		Form_pg_shdepend shdepForm = (Form_pg_shdepend) GETSTRUCT(tup);

		if (shdepForm->deptype == SHARED_DEPENDENCY_PIN)
			result = true;
	}

	systable_endscan(scan);

	return result;
}

/*
 * shdepDropOwned
 *
 * Drop the objects owned by any one of the given RoleIds.	If a role has
 * access to an object, the grant will be removed as well (but the object
 * will not, of course.)
 *
 * We can revoke grants immediately while doing the scan, but drops are
 * saved up and done all at once with performMultipleDeletions.  This
 * is necessary so that we don't get failures from trying to delete
 * interdependent objects in the wrong order.
 */
void
shdepDropOwned(List *roleids, DropBehavior behavior)
{
	Relation	sdepRel;
	ListCell   *cell;
	ObjectAddresses *deleteobjs;

	deleteobjs = new_object_addresses();

	sdepRel = heap_open(SharedDependRelationId, AccessExclusiveLock);

	/*
	 * For each role, find the dependent objects and drop them using the
	 * regular (non-shared) dependency management.
	 */
	foreach(cell, roleids)
	{
		Oid			roleid = lfirst_oid(cell);
		ScanKeyData key[2];
		SysScanDesc scan;
		HeapTuple	tuple;

		/* Doesn't work for pinned objects */
		if (isSharedObjectPinned(AuthIdRelationId, roleid, sdepRel))
		{
			ObjectAddress obj;

			obj.classId = AuthIdRelationId;
			obj.objectId = roleid;
			obj.objectSubId = 0;

			ereport(ERROR,
					(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				   errmsg("cannot drop objects owned by %s because they are "
						  "required by the database system",
						  getObjectDescription(&obj))));
		}

		ScanKeyInit(&key[0],
					Anum_pg_shdepend_refclassid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(AuthIdRelationId));
		ScanKeyInit(&key[1],
					Anum_pg_shdepend_refobjid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(roleid));

		scan = systable_beginscan(sdepRel, SharedDependReferenceIndexId, true,
								  SnapshotNow, 2, key);

		while ((tuple = systable_getnext(scan)) != NULL)
		{
			ObjectAddress obj;
			GrantObjectType objtype;
			InternalGrant istmt;
			Form_pg_shdepend sdepForm = (Form_pg_shdepend) GETSTRUCT(tuple);

			/* We only operate on objects in the current database */
			if (sdepForm->dbid != MyDatabaseId)
				continue;

			switch (sdepForm->deptype)
			{
					/* Shouldn't happen */
				case SHARED_DEPENDENCY_PIN:
				case SHARED_DEPENDENCY_INVALID:
					elog(ERROR, "unexpected dependency type");
					break;
				case SHARED_DEPENDENCY_ACL:
					switch (sdepForm->classid)
					{
						case RelationRelationId:
							/* it's OK to use RELATION for a sequence */
							istmt.objtype = ACL_OBJECT_RELATION;
							break;
						case DatabaseRelationId:
							istmt.objtype = ACL_OBJECT_DATABASE;
							break;
						case ProcedureRelationId:
							istmt.objtype = ACL_OBJECT_FUNCTION;
							break;
						case LanguageRelationId:
							istmt.objtype = ACL_OBJECT_LANGUAGE;
							break;
						case NamespaceRelationId:
							istmt.objtype = ACL_OBJECT_NAMESPACE;
							break;
						case TableSpaceRelationId:
							istmt.objtype = ACL_OBJECT_TABLESPACE;
							break;
						default:
							elog(ERROR, "unexpected object type %d",
								 sdepForm->classid);
							/* keep compiler quiet */
							objtype = (GrantObjectType) 0;
							break;
					}
					istmt.is_grant = false;
					istmt.objects = list_make1_oid(sdepForm->objid);
					istmt.all_privs = true;
					istmt.privileges = ACL_NO_RIGHTS;
					istmt.grantees = list_make1_oid(roleid);
					istmt.grant_option = false;
					istmt.behavior = DROP_CASCADE;

					ExecGrantStmt_oids(&istmt);
					break;
				case SHARED_DEPENDENCY_OWNER:
					/* Save it for deletion below */
					obj.classId = sdepForm->classid;
					obj.objectId = sdepForm->objid;
					obj.objectSubId = 0;
					add_exact_object_address(&obj, deleteobjs);
					break;
			}
		}

		systable_endscan(scan);
	}

	/* the dependency mechanism does the actual work */
	performMultipleDeletions(deleteobjs, behavior);

	heap_close(sdepRel, AccessExclusiveLock);

	free_object_addresses(deleteobjs);
}

/*
 * shdepReassignOwned
 *
 * Change the owner of objects owned by any of the roles in roleids to
 * newrole.  Grants are not touched.
 */
void
shdepReassignOwned(List *roleids, Oid newrole)
{
	Relation	sdepRel;
	ListCell   *cell;

	sdepRel = heap_open(SharedDependRelationId, AccessShareLock);

	foreach(cell, roleids)
	{
		SysScanDesc scan;
		ScanKeyData key[2];
		HeapTuple	tuple;
		Oid			roleid = lfirst_oid(cell);

		/* Refuse to work on pinned roles */
		if (isSharedObjectPinned(AuthIdRelationId, roleid, sdepRel))
		{
			ObjectAddress obj;

			obj.classId = AuthIdRelationId;
			obj.objectId = roleid;
			obj.objectSubId = 0;

			ereport(ERROR,
					(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				   errmsg("cannot drop objects owned by %s because they are "
						  "required by the database system",
						  getObjectDescription(&obj))));

			/*
			 * There's no need to tell the whole truth, which is that we
			 * didn't track these dependencies at all ...
			 */
		}

		ScanKeyInit(&key[0],
					Anum_pg_shdepend_refclassid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(AuthIdRelationId));
		ScanKeyInit(&key[1],
					Anum_pg_shdepend_refobjid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(roleid));

		scan = systable_beginscan(sdepRel, SharedDependReferenceIndexId, true,
								  SnapshotNow, 2, key);

		while ((tuple = systable_getnext(scan)) != NULL)
		{
			Form_pg_shdepend sdepForm = (Form_pg_shdepend) GETSTRUCT(tuple);

			/* We only operate on objects in the current database */
			if (sdepForm->dbid != MyDatabaseId)
				continue;

			/* Unexpected because we checked for pins above */
			if (sdepForm->deptype == SHARED_DEPENDENCY_PIN)
				elog(ERROR, "unexpected shared pin");

			/* We leave non-owner dependencies alone */
			if (sdepForm->deptype != SHARED_DEPENDENCY_OWNER)
				continue;

			/* Issue the appropriate ALTER OWNER call */
			switch (sdepForm->classid)
			{
				case ConversionRelationId:
					AlterConversionOwner_oid(sdepForm->objid, newrole);
					break;

				case TypeRelationId:
					AlterTypeOwnerInternal(sdepForm->objid, newrole);
					break;

				case OperatorRelationId:
					AlterOperatorOwner_oid(sdepForm->objid, newrole);
					break;

				case NamespaceRelationId:
					AlterSchemaOwner_oid(sdepForm->objid, newrole);
					break;

				case RelationRelationId:

					/*
					 * Pass recursing = true so that we don't fail on indexes,
					 * owned sequences, etc when we happen to visit them
					 * before their parent table.
					 */
					ATExecChangeOwner(sdepForm->objid, newrole, true);
					break;

				case ProcedureRelationId:
					AlterFunctionOwner_oid(sdepForm->objid, newrole);
					break;

				default:
					elog(ERROR, "unexpected classid %d", sdepForm->classid);
					break;
			}
			/* Make sure the next iteration will see my changes */
			CommandCounterIncrement();
		}

		systable_endscan(scan);
	}

	heap_close(sdepRel, AccessShareLock);
}
