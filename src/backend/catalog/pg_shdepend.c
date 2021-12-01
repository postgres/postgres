/*-------------------------------------------------------------------------
 *
 * pg_shdepend.c
 *	  routines to support manipulation of the pg_shdepend relation
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_shdepend.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_database.h"
#include "catalog/pg_default_acl.h"
#include "catalog/pg_event_trigger.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_language.h"
#include "catalog/pg_largeobject.h"
#include "catalog/pg_largeobject_metadata.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_shdepend.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_type.h"
#include "catalog/pg_user_mapping.h"
#include "commands/alter.h"
#include "commands/dbcommands.h"
#include "commands/collationcmds.h"
#include "commands/conversioncmds.h"
#include "commands/defrem.h"
#include "commands/event_trigger.h"
#include "commands/extension.h"
#include "commands/policy.h"
#include "commands/proclang.h"
#include "commands/publicationcmds.h"
#include "commands/schemacmds.h"
#include "commands/subscriptioncmds.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "commands/typecmds.h"
#include "storage/lmgr.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/syscache.h"


typedef enum
{
	LOCAL_OBJECT,
	SHARED_OBJECT,
	REMOTE_OBJECT
} SharedDependencyObjectType;

typedef struct
{
	ObjectAddress object;
	char		deptype;
	SharedDependencyObjectType objtype;
} ShDependObjectInfo;

static void getOidListDiff(Oid *list1, int *nlist1, Oid *list2, int *nlist2);
static Oid	classIdGetDbId(Oid classId);
static void shdepChangeDep(Relation sdepRel,
						   Oid classid, Oid objid, int32 objsubid,
						   Oid refclassid, Oid refobjid,
						   SharedDependencyType deptype);
static void shdepAddDependency(Relation sdepRel,
							   Oid classId, Oid objectId, int32 objsubId,
							   Oid refclassId, Oid refobjId,
							   SharedDependencyType deptype);
static void shdepDropDependency(Relation sdepRel,
								Oid classId, Oid objectId, int32 objsubId,
								bool drop_subobjects,
								Oid refclassId, Oid refobjId,
								SharedDependencyType deptype);
static void storeObjectDescription(StringInfo descs,
								   SharedDependencyObjectType type,
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

	sdepRel = table_open(SharedDependRelationId, RowExclusiveLock);

	/* If the referenced object is pinned, do nothing. */
	if (!isSharedObjectPinned(referenced->classId, referenced->objectId,
							  sdepRel))
	{
		shdepAddDependency(sdepRel, depender->classId, depender->objectId,
						   depender->objectSubId,
						   referenced->classId, referenced->objectId,
						   deptype);
	}

	table_close(sdepRel, RowExclusiveLock);
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
 * object.  This is an internal workhorse for operations such as changing
 * an object's owner.
 *
 * There must be no more than one existing entry for the given dependent
 * object and dependency type!	So in practice this can only be used for
 * updating SHARED_DEPENDENCY_OWNER and SHARED_DEPENDENCY_TABLESPACE
 * entries, which should have that property.
 *
 * If there is no previous entry, we assume it was referencing a PINned
 * object, so we create a new entry.  If the new referenced object is
 * PINned, we don't create an entry (and drop the old one, if any).
 * (For tablespaces, we don't record dependencies in certain cases, so
 * there are other possible reasons for entries to be missing.)
 *
 * sdepRel must be the pg_shdepend relation, already opened and suitably
 * locked.
 */
static void
shdepChangeDep(Relation sdepRel,
			   Oid classid, Oid objid, int32 objsubid,
			   Oid refclassid, Oid refobjid,
			   SharedDependencyType deptype)
{
	Oid			dbid = classIdGetDbId(classid);
	HeapTuple	oldtup = NULL;
	HeapTuple	scantup;
	ScanKeyData key[4];
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
	ScanKeyInit(&key[3],
				Anum_pg_shdepend_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(objsubid));

	scan = systable_beginscan(sdepRel, SharedDependDependerIndexId, true,
							  NULL, 4, key);

	while ((scantup = systable_getnext(scan)) != NULL)
	{
		/* Ignore if not of the target dependency type */
		if (((Form_pg_shdepend) GETSTRUCT(scantup))->deptype != deptype)
			continue;
		/* Caller screwed up if multiple matches */
		if (oldtup)
			elog(ERROR,
				 "multiple pg_shdepend entries for object %u/%u/%d deptype %c",
				 classid, objid, objsubid, deptype);
		oldtup = heap_copytuple(scantup);
	}

	systable_endscan(scan);

	if (isSharedObjectPinned(refclassid, refobjid, sdepRel))
	{
		/* No new entry needed, so just delete existing entry if any */
		if (oldtup)
			CatalogTupleDelete(sdepRel, &oldtup->t_self);
	}
	else if (oldtup)
	{
		/* Need to update existing entry */
		Form_pg_shdepend shForm = (Form_pg_shdepend) GETSTRUCT(oldtup);

		/* Since oldtup is a copy, we can just modify it in-memory */
		shForm->refclassid = refclassid;
		shForm->refobjid = refobjid;

		CatalogTupleUpdate(sdepRel, &oldtup->t_self, oldtup);
	}
	else
	{
		/* Need to insert new entry */
		Datum		values[Natts_pg_shdepend];
		bool		nulls[Natts_pg_shdepend];

		memset(nulls, false, sizeof(nulls));

		values[Anum_pg_shdepend_dbid - 1] = ObjectIdGetDatum(dbid);
		values[Anum_pg_shdepend_classid - 1] = ObjectIdGetDatum(classid);
		values[Anum_pg_shdepend_objid - 1] = ObjectIdGetDatum(objid);
		values[Anum_pg_shdepend_objsubid - 1] = Int32GetDatum(objsubid);

		values[Anum_pg_shdepend_refclassid - 1] = ObjectIdGetDatum(refclassid);
		values[Anum_pg_shdepend_refobjid - 1] = ObjectIdGetDatum(refobjid);
		values[Anum_pg_shdepend_deptype - 1] = CharGetDatum(deptype);

		/*
		 * we are reusing oldtup just to avoid declaring a new variable, but
		 * it's certainly a new tuple
		 */
		oldtup = heap_form_tuple(RelationGetDescr(sdepRel), values, nulls);
		CatalogTupleInsert(sdepRel, oldtup);
	}

	if (oldtup)
		heap_freetuple(oldtup);
}

/*
 * changeDependencyOnOwner
 *
 * Update the shared dependencies to account for the new owner.
 *
 * Note: we don't need an objsubid argument because only whole objects
 * have owners.
 */
void
changeDependencyOnOwner(Oid classId, Oid objectId, Oid newOwnerId)
{
	Relation	sdepRel;

	sdepRel = table_open(SharedDependRelationId, RowExclusiveLock);

	/* Adjust the SHARED_DEPENDENCY_OWNER entry */
	shdepChangeDep(sdepRel,
				   classId, objectId, 0,
				   AuthIdRelationId, newOwnerId,
				   SHARED_DEPENDENCY_OWNER);

	/*----------
	 * There should never be a SHARED_DEPENDENCY_ACL entry for the owner,
	 * so get rid of it if there is one.  This can happen if the new owner
	 * was previously granted some rights to the object.
	 *
	 * This step is analogous to aclnewowner's removal of duplicate entries
	 * in the ACL.  We have to do it to handle this scenario:
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
	shdepDropDependency(sdepRel, classId, objectId, 0, true,
						AuthIdRelationId, newOwnerId,
						SHARED_DEPENDENCY_ACL);

	table_close(sdepRel, RowExclusiveLock);
}

/*
 * recordDependencyOnTablespace
 *
 * A convenient wrapper of recordSharedDependencyOn -- register the specified
 * tablespace as default for the given object.
 *
 * Note: it's the caller's responsibility to ensure that there isn't a
 * tablespace entry for the object already.
 */
void
recordDependencyOnTablespace(Oid classId, Oid objectId, Oid tablespace)
{
	ObjectAddress myself,
				  referenced;

	ObjectAddressSet(myself, classId, objectId);
	ObjectAddressSet(referenced, TableSpaceRelationId, tablespace);

	recordSharedDependencyOn(&myself, &referenced,
							 SHARED_DEPENDENCY_TABLESPACE);
}

/*
 * changeDependencyOnTablespace
 *
 * Update the shared dependencies to account for the new tablespace.
 *
 * Note: we don't need an objsubid argument because only whole objects
 * have tablespaces.
 */
void
changeDependencyOnTablespace(Oid classId, Oid objectId, Oid newTablespaceId)
{
	Relation	sdepRel;

	sdepRel = table_open(SharedDependRelationId, RowExclusiveLock);

	if (newTablespaceId != DEFAULTTABLESPACE_OID &&
		newTablespaceId != InvalidOid)
		shdepChangeDep(sdepRel,
					   classId, objectId, 0,
					   TableSpaceRelationId, newTablespaceId,
					   SHARED_DEPENDENCY_TABLESPACE);
	else
		shdepDropDependency(sdepRel,
							classId, objectId, 0, true,
							InvalidOid, InvalidOid,
							SHARED_DEPENDENCY_INVALID);

	table_close(sdepRel, RowExclusiveLock);
}

/*
 * getOidListDiff
 *		Helper for updateAclDependencies.
 *
 * Takes two Oid arrays and removes elements that are common to both arrays,
 * leaving just those that are in one input but not the other.
 * We assume both arrays have been sorted and de-duped.
 */
static void
getOidListDiff(Oid *list1, int *nlist1, Oid *list2, int *nlist2)
{
	int			in1,
				in2,
				out1,
				out2;

	in1 = in2 = out1 = out2 = 0;
	while (in1 < *nlist1 && in2 < *nlist2)
	{
		if (list1[in1] == list2[in2])
		{
			/* skip over duplicates */
			in1++;
			in2++;
		}
		else if (list1[in1] < list2[in2])
		{
			/* list1[in1] is not in list2 */
			list1[out1++] = list1[in1++];
		}
		else
		{
			/* list2[in2] is not in list1 */
			list2[out2++] = list2[in2++];
		}
	}

	/* any remaining list1 entries are not in list2 */
	while (in1 < *nlist1)
	{
		list1[out1++] = list1[in1++];
	}

	/* any remaining list2 entries are not in list1 */
	while (in2 < *nlist2)
	{
		list2[out2++] = list2[in2++];
	}

	*nlist1 = out1;
	*nlist2 = out2;
}

/*
 * updateAclDependencies
 *		Update the pg_shdepend info for an object's ACL during GRANT/REVOKE.
 *
 * classId, objectId, objsubId: identify the object whose ACL this is
 * ownerId: role owning the object
 * noldmembers, oldmembers: array of roleids appearing in old ACL
 * nnewmembers, newmembers: array of roleids appearing in new ACL
 *
 * We calculate the differences between the new and old lists of roles,
 * and then insert or delete from pg_shdepend as appropriate.
 *
 * Note that we can't just insert all referenced roles blindly during GRANT,
 * because we would end up with duplicate registered dependencies.  We could
 * check for existence of the tuples before inserting, but that seems to be
 * more expensive than what we are doing here.  Likewise we can't just delete
 * blindly during REVOKE, because the user may still have other privileges.
 * It is also possible that REVOKE actually adds dependencies, due to
 * instantiation of a formerly implicit default ACL (although at present,
 * all such dependencies should be for the owning role, which we ignore here).
 *
 * NOTE: Both input arrays must be sorted and de-duped.  (Typically they
 * are extracted from an ACL array by aclmembers(), which takes care of
 * both requirements.)	The arrays are pfreed before return.
 */
void
updateAclDependencies(Oid classId, Oid objectId, int32 objsubId,
					  Oid ownerId,
					  int noldmembers, Oid *oldmembers,
					  int nnewmembers, Oid *newmembers)
{
	Relation	sdepRel;
	int			i;

	/*
	 * Remove entries that are common to both lists; those represent existing
	 * dependencies we don't need to change.
	 *
	 * OK to overwrite the inputs since we'll pfree them anyway.
	 */
	getOidListDiff(oldmembers, &noldmembers, newmembers, &nnewmembers);

	if (noldmembers > 0 || nnewmembers > 0)
	{
		sdepRel = table_open(SharedDependRelationId, RowExclusiveLock);

		/* Add new dependencies that weren't already present */
		for (i = 0; i < nnewmembers; i++)
		{
			Oid			roleid = newmembers[i];

			/*
			 * Skip the owner: he has an OWNER shdep entry instead. (This is
			 * not just a space optimization; it makes ALTER OWNER easier. See
			 * notes in changeDependencyOnOwner.)
			 */
			if (roleid == ownerId)
				continue;

			/* Skip pinned roles; they don't need dependency entries */
			if (isSharedObjectPinned(AuthIdRelationId, roleid, sdepRel))
				continue;

			shdepAddDependency(sdepRel, classId, objectId, objsubId,
							   AuthIdRelationId, roleid,
							   SHARED_DEPENDENCY_ACL);
		}

		/* Drop no-longer-used old dependencies */
		for (i = 0; i < noldmembers; i++)
		{
			Oid			roleid = oldmembers[i];

			/* Skip the owner, same as above */
			if (roleid == ownerId)
				continue;

			/* Skip pinned roles */
			if (isSharedObjectPinned(AuthIdRelationId, roleid, sdepRel))
				continue;

			shdepDropDependency(sdepRel, classId, objectId, objsubId,
								false,	/* exact match on objsubId */
								AuthIdRelationId, roleid,
								SHARED_DEPENDENCY_ACL);
		}

		table_close(sdepRel, RowExclusiveLock);
	}

	if (oldmembers)
		pfree(oldmembers);
	if (newmembers)
		pfree(newmembers);
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
 * qsort comparator for ShDependObjectInfo items
 */
static int
shared_dependency_comparator(const void *a, const void *b)
{
	const ShDependObjectInfo *obja = (const ShDependObjectInfo *) a;
	const ShDependObjectInfo *objb = (const ShDependObjectInfo *) b;

	/*
	 * Primary sort key is OID ascending.
	 */
	if (obja->object.objectId < objb->object.objectId)
		return -1;
	if (obja->object.objectId > objb->object.objectId)
		return 1;

	/*
	 * Next sort on catalog ID, in case identical OIDs appear in different
	 * catalogs.  Sort direction is pretty arbitrary here.
	 */
	if (obja->object.classId < objb->object.classId)
		return -1;
	if (obja->object.classId > objb->object.classId)
		return 1;

	/*
	 * Sort on object subId.
	 *
	 * We sort the subId as an unsigned int so that 0 (the whole object) will
	 * come first.
	 */
	if ((unsigned int) obja->object.objectSubId < (unsigned int) objb->object.objectSubId)
		return -1;
	if ((unsigned int) obja->object.objectSubId > (unsigned int) objb->object.objectSubId)
		return 1;

	/*
	 * Last, sort on deptype, in case the same object has multiple dependency
	 * types.  (Note that there's no need to consider objtype, as that's
	 * determined by the catalog OID.)
	 */
	if (obja->deptype < objb->deptype)
		return -1;
	if (obja->deptype > objb->deptype)
		return 1;

	return 0;
}

/*
 * checkSharedDependencies
 *
 * Check whether there are shared dependency entries for a given shared
 * object; return true if so.
 *
 * In addition, return a string containing a newline-separated list of object
 * descriptions that depend on the shared object, or NULL if none is found.
 * We actually return two such strings; the "detail" result is suitable for
 * returning to the client as an errdetail() string, and is limited in size.
 * The "detail_log" string is potentially much longer, and should be emitted
 * to the server log only.
 *
 * We can find three different kinds of dependencies: dependencies on objects
 * of the current database; dependencies on shared objects; and dependencies
 * on objects local to other databases.  We can (and do) provide descriptions
 * of the two former kinds of objects, but we can't do that for "remote"
 * objects, so we just provide a count of them.
 *
 * If we find a SHARED_DEPENDENCY_PIN entry, we can error out early.
 */
bool
checkSharedDependencies(Oid classId, Oid objectId,
						char **detail_msg, char **detail_log_msg)
{
	Relation	sdepRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	int			numReportedDeps = 0;
	int			numNotReportedDeps = 0;
	int			numNotReportedDbs = 0;
	List	   *remDeps = NIL;
	ListCell   *cell;
	ObjectAddress object;
	ShDependObjectInfo *objects;
	int			numobjects;
	int			allocedobjects;
	StringInfoData descs;
	StringInfoData alldescs;

	/*
	 * We limit the number of dependencies reported to the client to
	 * MAX_REPORTED_DEPS, since client software may not deal well with
	 * enormous error strings.  The server log always gets a full report.
	 *
	 * For stability of regression test results, we sort local and shared
	 * objects by OID before reporting them.  We don't worry about the order
	 * in which other databases are reported, though.
	 */
#define MAX_REPORTED_DEPS 100

	allocedobjects = 128;		/* arbitrary initial array size */
	objects = (ShDependObjectInfo *)
		palloc(allocedobjects * sizeof(ShDependObjectInfo));
	numobjects = 0;
	initStringInfo(&descs);
	initStringInfo(&alldescs);

	sdepRel = table_open(SharedDependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_shdepend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classId));
	ScanKeyInit(&key[1],
				Anum_pg_shdepend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objectId));

	scan = systable_beginscan(sdepRel, SharedDependReferenceIndexId, true,
							  NULL, 2, key);

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
		object.objectSubId = sdepForm->objsubid;

		/*
		 * If it's a dependency local to this database or it's a shared
		 * object, add it to the objects array.
		 *
		 * If it's a remote dependency, keep track of it so we can report the
		 * number of them later.
		 */
		if (sdepForm->dbid == MyDatabaseId ||
			sdepForm->dbid == InvalidOid)
		{
			if (numobjects >= allocedobjects)
			{
				allocedobjects *= 2;
				objects = (ShDependObjectInfo *)
					repalloc(objects,
							 allocedobjects * sizeof(ShDependObjectInfo));
			}
			objects[numobjects].object = object;
			objects[numobjects].deptype = sdepForm->deptype;
			objects[numobjects].objtype = (sdepForm->dbid == MyDatabaseId) ?
				LOCAL_OBJECT : SHARED_OBJECT;
			numobjects++;
		}
		else
		{
			/* It's not local nor shared, so it must be remote. */
			remoteDep  *dep;
			bool		stored = false;

			/*
			 * XXX this info is kept on a simple List.  Maybe it's not good
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
			}
		}
	}

	systable_endscan(scan);

	table_close(sdepRel, AccessShareLock);

	/*
	 * Sort and report local and shared objects.
	 */
	if (numobjects > 1)
		qsort((void *) objects, numobjects,
			  sizeof(ShDependObjectInfo), shared_dependency_comparator);

	for (int i = 0; i < numobjects; i++)
	{
		if (numReportedDeps < MAX_REPORTED_DEPS)
		{
			numReportedDeps++;
			storeObjectDescription(&descs,
								   objects[i].objtype,
								   &objects[i].object,
								   objects[i].deptype,
								   0);
		}
		else
			numNotReportedDeps++;
		storeObjectDescription(&alldescs,
							   objects[i].objtype,
							   &objects[i].object,
							   objects[i].deptype,
							   0);
	}

	/*
	 * Summarize dependencies in remote databases.
	 */
	foreach(cell, remDeps)
	{
		remoteDep  *dep = lfirst(cell);

		object.classId = DatabaseRelationId;
		object.objectId = dep->dbOid;
		object.objectSubId = 0;

		if (numReportedDeps < MAX_REPORTED_DEPS)
		{
			numReportedDeps++;
			storeObjectDescription(&descs, REMOTE_OBJECT, &object,
								   SHARED_DEPENDENCY_INVALID, dep->count);
		}
		else
			numNotReportedDbs++;
		storeObjectDescription(&alldescs, REMOTE_OBJECT, &object,
							   SHARED_DEPENDENCY_INVALID, dep->count);
	}

	pfree(objects);
	list_free_deep(remDeps);

	if (descs.len == 0)
	{
		pfree(descs.data);
		pfree(alldescs.data);
		*detail_msg = *detail_log_msg = NULL;
		return false;
	}

	if (numNotReportedDeps > 0)
		appendStringInfo(&descs, ngettext("\nand %d other object "
										  "(see server log for list)",
										  "\nand %d other objects "
										  "(see server log for list)",
										  numNotReportedDeps),
						 numNotReportedDeps);
	if (numNotReportedDbs > 0)
		appendStringInfo(&descs, ngettext("\nand objects in %d other database "
										  "(see server log for list)",
										  "\nand objects in %d other databases "
										  "(see server log for list)",
										  numNotReportedDbs),
						 numNotReportedDbs);

	*detail_msg = descs.data;
	*detail_log_msg = alldescs.data;
	return true;
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

	sdepRel = table_open(SharedDependRelationId, RowExclusiveLock);
	sdepDesc = RelationGetDescr(sdepRel);

	indstate = CatalogOpenIndexes(sdepRel);

	/* Scan all entries with dbid = templateDbId */
	ScanKeyInit(&key[0],
				Anum_pg_shdepend_dbid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(templateDbId));

	scan = systable_beginscan(sdepRel, SharedDependDependerIndexId, true,
							  NULL, 1, key);

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
		CatalogTupleInsertWithInfo(sdepRel, newtup, indstate);

		heap_freetuple(newtup);
	}

	systable_endscan(scan);

	CatalogCloseIndexes(indstate);
	table_close(sdepRel, RowExclusiveLock);
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

	sdepRel = table_open(SharedDependRelationId, RowExclusiveLock);

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
							  NULL, 1, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		CatalogTupleDelete(sdepRel, &tup->t_self);
	}

	systable_endscan(scan);

	/* Now delete all entries corresponding to the database itself */
	shdepDropDependency(sdepRel, DatabaseRelationId, databaseId, 0, true,
						InvalidOid, InvalidOid,
						SHARED_DEPENDENCY_INVALID);

	table_close(sdepRel, RowExclusiveLock);
}

/*
 * deleteSharedDependencyRecordsFor
 *
 * Delete all pg_shdepend entries corresponding to an object that's being
 * dropped or modified.  The object is assumed to be either a shared object
 * or local to the current database (the classId tells us which).
 *
 * If objectSubId is zero, we are deleting a whole object, so get rid of
 * pg_shdepend entries for subobjects as well.
 */
void
deleteSharedDependencyRecordsFor(Oid classId, Oid objectId, int32 objectSubId)
{
	Relation	sdepRel;

	sdepRel = table_open(SharedDependRelationId, RowExclusiveLock);

	shdepDropDependency(sdepRel, classId, objectId, objectSubId,
						(objectSubId == 0),
						InvalidOid, InvalidOid,
						SHARED_DEPENDENCY_INVALID);

	table_close(sdepRel, RowExclusiveLock);
}

/*
 * shdepAddDependency
 *		Internal workhorse for inserting into pg_shdepend
 *
 * sdepRel must be the pg_shdepend relation, already opened and suitably
 * locked.
 */
static void
shdepAddDependency(Relation sdepRel,
				   Oid classId, Oid objectId, int32 objsubId,
				   Oid refclassId, Oid refobjId,
				   SharedDependencyType deptype)
{
	HeapTuple	tup;
	Datum		values[Natts_pg_shdepend];
	bool		nulls[Natts_pg_shdepend];

	/*
	 * Make sure the object doesn't go away while we record the dependency on
	 * it.  DROP routines should lock the object exclusively before they check
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
	values[Anum_pg_shdepend_objsubid - 1] = Int32GetDatum(objsubId);

	values[Anum_pg_shdepend_refclassid - 1] = ObjectIdGetDatum(refclassId);
	values[Anum_pg_shdepend_refobjid - 1] = ObjectIdGetDatum(refobjId);
	values[Anum_pg_shdepend_deptype - 1] = CharGetDatum(deptype);

	tup = heap_form_tuple(sdepRel->rd_att, values, nulls);

	CatalogTupleInsert(sdepRel, tup);

	/* clean up */
	heap_freetuple(tup);
}

/*
 * shdepDropDependency
 *		Internal workhorse for deleting entries from pg_shdepend.
 *
 * We drop entries having the following properties:
 *	dependent object is the one identified by classId/objectId/objsubId
 *	if refclassId isn't InvalidOid, it must match the entry's refclassid
 *	if refobjId isn't InvalidOid, it must match the entry's refobjid
 *	if deptype isn't SHARED_DEPENDENCY_INVALID, it must match entry's deptype
 *
 * If drop_subobjects is true, we ignore objsubId and consider all entries
 * matching classId/objectId.
 *
 * sdepRel must be the pg_shdepend relation, already opened and suitably
 * locked.
 */
static void
shdepDropDependency(Relation sdepRel,
					Oid classId, Oid objectId, int32 objsubId,
					bool drop_subobjects,
					Oid refclassId, Oid refobjId,
					SharedDependencyType deptype)
{
	ScanKeyData key[4];
	int			nkeys;
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
	if (drop_subobjects)
		nkeys = 3;
	else
	{
		ScanKeyInit(&key[3],
					Anum_pg_shdepend_objsubid,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(objsubId));
		nkeys = 4;
	}

	scan = systable_beginscan(sdepRel, SharedDependDependerIndexId, true,
							  NULL, nkeys, key);

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
		CatalogTupleDelete(sdepRel, &tup->t_self);
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
void
shdepLockAndCheckObject(Oid classId, Oid objectId)
{
	/* AccessShareLock should be OK, since we are not modifying the object */
	LockSharedObject(classId, objectId, 0, AccessShareLock);

	switch (classId)
	{
		case AuthIdRelationId:
			if (!SearchSysCacheExists1(AUTHOID, ObjectIdGetDatum(objectId)))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("role %u was concurrently dropped",
								objectId)));
			break;

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

		case DatabaseRelationId:
			{
				/* For lack of a syscache on pg_database, do this: */
				char	   *database = get_database_name(objectId);

				if (database == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("database %u was concurrently dropped",
									objectId)));
				pfree(database);
				break;
			}


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
storeObjectDescription(StringInfo descs,
					   SharedDependencyObjectType type,
					   ObjectAddress *object,
					   SharedDependencyType deptype,
					   int count)
{
	char	   *objdesc = getObjectDescription(object);

	/*
	 * An object being dropped concurrently doesn't need to be reported.
	 */
	if (objdesc == NULL)
		return;

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
				appendStringInfo(descs, _("privileges for %s"), objdesc);
			else if (deptype == SHARED_DEPENDENCY_POLICY)
				appendStringInfo(descs, _("target of %s"), objdesc);
			else if (deptype == SHARED_DEPENDENCY_TABLESPACE)
				appendStringInfo(descs, _("tablespace for %s"), objdesc);
			else
				elog(ERROR, "unrecognized dependency type: %d",
					 (int) deptype);
			break;

		case REMOTE_OBJECT:
			/* translator: %s will always be "database %s" */
			appendStringInfo(descs, ngettext("%d object in %s",
											 "%d objects in %s",
											 count),
							 count, objdesc);
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
							  NULL, 2, key);

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
 * Drop the objects owned by any one of the given RoleIds.  If a role has
 * access to an object, the grant will be removed as well (but the object
 * will not, of course).
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

	/*
	 * We don't need this strong a lock here, but we'll call routines that
	 * acquire RowExclusiveLock.  Better get that right now to avoid potential
	 * deadlock failures.
	 */
	sdepRel = table_open(SharedDependRelationId, RowExclusiveLock);

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
								  NULL, 2, key);

		while ((tuple = systable_getnext(scan)) != NULL)
		{
			Form_pg_shdepend sdepForm = (Form_pg_shdepend) GETSTRUCT(tuple);
			ObjectAddress obj;

			/*
			 * We only operate on shared objects and objects in the current
			 * database
			 */
			if (sdepForm->dbid != MyDatabaseId &&
				sdepForm->dbid != InvalidOid)
				continue;

			switch (sdepForm->deptype)
			{
					/* Shouldn't happen */
				case SHARED_DEPENDENCY_PIN:
				case SHARED_DEPENDENCY_INVALID:
					elog(ERROR, "unexpected dependency type");
					break;
				case SHARED_DEPENDENCY_ACL:
					RemoveRoleFromObjectACL(roleid,
											sdepForm->classid,
											sdepForm->objid);
					break;
				case SHARED_DEPENDENCY_POLICY:
					/*
					 * Try to remove role from policy; if unable to, remove
					 * policy.
					 */
					if (!RemoveRoleFromObjectPolicy(roleid,
													sdepForm->classid,
													sdepForm->objid))
					{
						obj.classId = sdepForm->classid;
						obj.objectId = sdepForm->objid;
						obj.objectSubId = sdepForm->objsubid;
						/*
						 * Acquire lock on object, then verify this dependency
						 * is still relevant.  If not, the object might have
						 * been dropped or the policy modified.  Ignore the
						 * object in that case.
						 */
						AcquireDeletionLock(&obj, 0);
						if (!systable_recheck_tuple(scan, tuple))
						{
							ReleaseDeletionLock(&obj);
							break;
						}
						add_exact_object_address(&obj, deleteobjs);
					}
					break;
				case SHARED_DEPENDENCY_OWNER:
					/* If a local object, save it for deletion below */
					if (sdepForm->dbid == MyDatabaseId)
					{
						obj.classId = sdepForm->classid;
						obj.objectId = sdepForm->objid;
						obj.objectSubId = sdepForm->objsubid;
						/* as above */
						AcquireDeletionLock(&obj, 0);
						if (!systable_recheck_tuple(scan, tuple))
						{
							ReleaseDeletionLock(&obj);
							break;
						}
						add_exact_object_address(&obj, deleteobjs);
					}
					break;
			}
		}

		systable_endscan(scan);
	}

	/*
	 * For stability of deletion-report ordering, sort the objects into
	 * approximate reverse creation order before deletion.  (This might also
	 * make the deletion go a bit faster, since there's less chance of having
	 * to rearrange the objects due to dependencies.)
	 */
	sort_object_addresses(deleteobjs);

	/* the dependency mechanism does the actual work */
	performMultipleDeletions(deleteobjs, behavior, 0);

	table_close(sdepRel, RowExclusiveLock);

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

	/*
	 * We don't need this strong a lock here, but we'll call routines that
	 * acquire RowExclusiveLock.  Better get that right now to avoid potential
	 * deadlock problems.
	 */
	sdepRel = table_open(SharedDependRelationId, RowExclusiveLock);

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
					 errmsg("cannot reassign ownership of objects owned by %s because they are required by the database system",
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
								  NULL, 2, key);

		while ((tuple = systable_getnext(scan)) != NULL)
		{
			Form_pg_shdepend sdepForm = (Form_pg_shdepend) GETSTRUCT(tuple);
			MemoryContext cxt,
						oldcxt;

			/*
			 * We only operate on shared objects and objects in the current
			 * database
			 */
			if (sdepForm->dbid != MyDatabaseId &&
				sdepForm->dbid != InvalidOid)
				continue;

			/* Unexpected because we checked for pins above */
			if (sdepForm->deptype == SHARED_DEPENDENCY_PIN)
				elog(ERROR, "unexpected shared pin");

			/* We leave non-owner dependencies alone */
			if (sdepForm->deptype != SHARED_DEPENDENCY_OWNER)
				continue;

			/*
			 * The various ALTER OWNER routines tend to leak memory in
			 * CurrentMemoryContext.  That's not a problem when they're only
			 * called once per command; but in this usage where we might be
			 * touching many objects, it can amount to a serious memory leak.
			 * Fix that by running each call in a short-lived context.
			 */
			cxt = AllocSetContextCreate(CurrentMemoryContext,
										"shdepReassignOwned",
										ALLOCSET_DEFAULT_SIZES);
			oldcxt = MemoryContextSwitchTo(cxt);

			/* Issue the appropriate ALTER OWNER call */
			switch (sdepForm->classid)
			{
				case TypeRelationId:
					AlterTypeOwner_oid(sdepForm->objid, newrole, true);
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
					ATExecChangeOwner(sdepForm->objid, newrole, true, AccessExclusiveLock);
					break;

				case DefaultAclRelationId:

					/*
					 * Ignore default ACLs; they should be handled by DROP
					 * OWNED, not REASSIGN OWNED.
					 */
					break;

				case UserMappingRelationId:
					/* ditto */
					break;

				case ForeignServerRelationId:
					AlterForeignServerOwner_oid(sdepForm->objid, newrole);
					break;

				case ForeignDataWrapperRelationId:
					AlterForeignDataWrapperOwner_oid(sdepForm->objid, newrole);
					break;

				case EventTriggerRelationId:
					AlterEventTriggerOwner_oid(sdepForm->objid, newrole);
					break;

				case PublicationRelationId:
					AlterPublicationOwner_oid(sdepForm->objid, newrole);
					break;

				case SubscriptionRelationId:
					AlterSubscriptionOwner_oid(sdepForm->objid, newrole);
					break;

					/* Generic alter owner cases */
				case CollationRelationId:
				case ConversionRelationId:
				case OperatorRelationId:
				case ProcedureRelationId:
				case LanguageRelationId:
				case LargeObjectRelationId:
				case OperatorFamilyRelationId:
				case OperatorClassRelationId:
				case ExtensionRelationId:
				case StatisticExtRelationId:
				case TableSpaceRelationId:
				case DatabaseRelationId:
				case TSConfigRelationId:
				case TSDictionaryRelationId:
					{
						Oid			classId = sdepForm->classid;
						Relation	catalog;

						if (classId == LargeObjectRelationId)
							classId = LargeObjectMetadataRelationId;

						catalog = table_open(classId, RowExclusiveLock);

						AlterObjectOwner_internal(catalog, sdepForm->objid,
												  newrole);

						table_close(catalog, NoLock);
					}
					break;

				default:
					elog(ERROR, "unexpected classid %u", sdepForm->classid);
					break;
			}

			/* Clean up */
			MemoryContextSwitchTo(oldcxt);
			MemoryContextDelete(cxt);

			/* Make sure the next iteration will see my changes */
			CommandCounterIncrement();
		}

		systable_endscan(scan);
	}

	table_close(sdepRel, RowExclusiveLock);
}
