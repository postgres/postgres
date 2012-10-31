/*-------------------------------------------------------------------------
 *
 * alter.c
 *	  Drivers for generic alter commands
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/alter.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_largeobject.h"
#include "catalog/pg_largeobject_metadata.h"
#include "catalog/pg_namespace.h"
#include "commands/alter.h"
#include "commands/collationcmds.h"
#include "commands/conversioncmds.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/event_trigger.h"
#include "commands/extension.h"
#include "commands/proclang.h"
#include "commands/schemacmds.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "commands/typecmds.h"
#include "commands/user.h"
#include "miscadmin.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/tqual.h"


/*
 * Executes an ALTER OBJECT / RENAME TO statement.	Based on the object
 * type, the function appropriate to that type is executed.
 */
void
ExecRenameStmt(RenameStmt *stmt)
{
	switch (stmt->renameType)
	{
		case OBJECT_AGGREGATE:
			RenameAggregate(stmt->object, stmt->objarg, stmt->newname);
			break;

		case OBJECT_COLLATION:
			RenameCollation(stmt->object, stmt->newname);
			break;

		case OBJECT_CONSTRAINT:
			RenameConstraint(stmt);
			break;

		case OBJECT_CONVERSION:
			RenameConversion(stmt->object, stmt->newname);
			break;

		case OBJECT_DATABASE:
			RenameDatabase(stmt->subname, stmt->newname);
			break;

		case OBJECT_FDW:
			RenameForeignDataWrapper(stmt->subname, stmt->newname);
			break;

		case OBJECT_FOREIGN_SERVER:
			RenameForeignServer(stmt->subname, stmt->newname);
			break;

		case OBJECT_EVENT_TRIGGER:
			RenameEventTrigger(stmt->subname, stmt->newname);
			break;

		case OBJECT_FUNCTION:
			RenameFunction(stmt->object, stmt->objarg, stmt->newname);
			break;

		case OBJECT_LANGUAGE:
			RenameLanguage(stmt->subname, stmt->newname);
			break;

		case OBJECT_OPCLASS:
			RenameOpClass(stmt->object, stmt->subname, stmt->newname);
			break;

		case OBJECT_OPFAMILY:
			RenameOpFamily(stmt->object, stmt->subname, stmt->newname);
			break;

		case OBJECT_ROLE:
			RenameRole(stmt->subname, stmt->newname);
			break;

		case OBJECT_SCHEMA:
			RenameSchema(stmt->subname, stmt->newname);
			break;

		case OBJECT_TABLESPACE:
			RenameTableSpace(stmt->subname, stmt->newname);
			break;

		case OBJECT_TABLE:
		case OBJECT_SEQUENCE:
		case OBJECT_VIEW:
		case OBJECT_INDEX:
		case OBJECT_FOREIGN_TABLE:
			RenameRelation(stmt);
			break;

		case OBJECT_COLUMN:
		case OBJECT_ATTRIBUTE:
			renameatt(stmt);
			break;

		case OBJECT_TRIGGER:
			renametrig(stmt);
			break;

		case OBJECT_TSPARSER:
			RenameTSParser(stmt->object, stmt->newname);
			break;

		case OBJECT_TSDICTIONARY:
			RenameTSDictionary(stmt->object, stmt->newname);
			break;

		case OBJECT_TSTEMPLATE:
			RenameTSTemplate(stmt->object, stmt->newname);
			break;

		case OBJECT_TSCONFIGURATION:
			RenameTSConfiguration(stmt->object, stmt->newname);
			break;

		case OBJECT_DOMAIN:
		case OBJECT_TYPE:
			RenameType(stmt);
			break;

		default:
			elog(ERROR, "unrecognized rename stmt type: %d",
				 (int) stmt->renameType);
	}
}

/*
 * Executes an ALTER OBJECT / SET SCHEMA statement.  Based on the object
 * type, the function appropriate to that type is executed.
 */
void
ExecAlterObjectSchemaStmt(AlterObjectSchemaStmt *stmt)
{
	switch (stmt->objectType)
	{
		case OBJECT_AGGREGATE:
			AlterFunctionNamespace(stmt->object, stmt->objarg, true,
								   stmt->newschema);
			break;

		case OBJECT_COLLATION:
			AlterCollationNamespace(stmt->object, stmt->newschema);
			break;

		case OBJECT_EXTENSION:
			AlterExtensionNamespace(stmt->object, stmt->newschema);
			break;

		case OBJECT_FUNCTION:
			AlterFunctionNamespace(stmt->object, stmt->objarg, false,
								   stmt->newschema);
			break;

		case OBJECT_SEQUENCE:
		case OBJECT_TABLE:
		case OBJECT_VIEW:
		case OBJECT_FOREIGN_TABLE:
			AlterTableNamespace(stmt);
			break;

		case OBJECT_TYPE:
		case OBJECT_DOMAIN:
			AlterTypeNamespace(stmt->object, stmt->newschema, stmt->objectType);
			break;

			/* generic code path */
		case OBJECT_CONVERSION:
		case OBJECT_OPERATOR:
		case OBJECT_OPCLASS:
		case OBJECT_OPFAMILY:
		case OBJECT_TSPARSER:
		case OBJECT_TSDICTIONARY:
		case OBJECT_TSTEMPLATE:
		case OBJECT_TSCONFIGURATION:
			{
				Relation	catalog;
				Relation	relation;
				Oid			classId;
				Oid			nspOid;
				ObjectAddress address;

				address = get_object_address(stmt->objectType,
											 stmt->object,
											 stmt->objarg,
											 &relation,
											 AccessExclusiveLock,
											 false);
				Assert(relation == NULL);
				classId = address.classId;
				catalog = heap_open(classId, RowExclusiveLock);
				nspOid = LookupCreationNamespace(stmt->newschema);

				AlterObjectNamespace_internal(catalog, address.objectId,
											  nspOid);
				heap_close(catalog, RowExclusiveLock);
			}
			break;

		default:
			elog(ERROR, "unrecognized AlterObjectSchemaStmt type: %d",
				 (int) stmt->objectType);
	}
}

/*
 * Change an object's namespace given its classOid and object Oid.
 *
 * Objects that don't have a namespace should be ignored.
 *
 * This function is currently used only by ALTER EXTENSION SET SCHEMA,
 * so it only needs to cover object types that can be members of an
 * extension, and it doesn't have to deal with certain special cases
 * such as not wanting to process array types --- those should never
 * be direct members of an extension anyway.
 *
 * Returns the OID of the object's previous namespace, or InvalidOid if
 * object doesn't have a schema.
 */
Oid
AlterObjectNamespace_oid(Oid classId, Oid objid, Oid nspOid,
						 ObjectAddresses *objsMoved)
{
	Oid			oldNspOid = InvalidOid;
	ObjectAddress dep;

	dep.classId = classId;
	dep.objectId = objid;
	dep.objectSubId = 0;

	switch (getObjectClass(&dep))
	{
		case OCLASS_CLASS:
			{
				Relation	rel;

				rel = relation_open(objid, AccessExclusiveLock);
				oldNspOid = RelationGetNamespace(rel);

				AlterTableNamespaceInternal(rel, oldNspOid, nspOid, objsMoved);

				relation_close(rel, NoLock);
				break;
			}

		case OCLASS_PROC:
			oldNspOid = AlterFunctionNamespace_oid(objid, nspOid);
			break;

		case OCLASS_TYPE:
			oldNspOid = AlterTypeNamespace_oid(objid, nspOid, objsMoved);
			break;

		case OCLASS_COLLATION:
			oldNspOid = AlterCollationNamespace_oid(objid, nspOid);
			break;

		case OCLASS_CONVERSION:
		case OCLASS_OPERATOR:
		case OCLASS_OPCLASS:
		case OCLASS_OPFAMILY:
		case OCLASS_TSPARSER:
		case OCLASS_TSDICT:
		case OCLASS_TSTEMPLATE:
		case OCLASS_TSCONFIG:
			{
				Relation	catalog;

				catalog = heap_open(classId, RowExclusiveLock);

				oldNspOid = AlterObjectNamespace_internal(catalog, objid,
														  nspOid);

				heap_close(catalog, RowExclusiveLock);
			}
			break;

		default:
			break;
	}

	return oldNspOid;
}

/*
 * Generic function to change the namespace of a given object, for simple
 * cases (won't work for tables, nor other cases where we need to do more
 * than change the namespace column of a single catalog entry).
 *
 * rel: catalog relation containing object (RowExclusiveLock'd by caller)
 * objid: OID of object to change the namespace of
 * nspOid: OID of new namespace
 *
 * Returns the OID of the object's previous namespace.
 */
Oid
AlterObjectNamespace_internal(Relation rel, Oid objid, Oid nspOid)
{
	Oid			classId = RelationGetRelid(rel);
	int			oidCacheId = get_object_catcache_oid(classId);
	int			nameCacheId = get_object_catcache_name(classId);
	AttrNumber	Anum_name = get_object_attnum_name(classId);
	AttrNumber	Anum_namespace = get_object_attnum_namespace(classId);
	AttrNumber	Anum_owner = get_object_attnum_owner(classId);
	AclObjectKind acl_kind = get_object_aclkind(classId);
	Oid			oldNspOid;
	Datum		name,
				namespace;
	bool		isnull;
	HeapTuple	tup,
				newtup;
	Datum	   *values;
	bool	   *nulls;
	bool	   *replaces;

	tup = SearchSysCacheCopy1(oidCacheId, ObjectIdGetDatum(objid));
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for object %u of catalog \"%s\"",
			 objid, RelationGetRelationName(rel));

	name = heap_getattr(tup, Anum_name, RelationGetDescr(rel), &isnull);
	Assert(!isnull);
	namespace = heap_getattr(tup, Anum_namespace, RelationGetDescr(rel),
							 &isnull);
	Assert(!isnull);
	oldNspOid = DatumGetObjectId(namespace);

	/* Check basic namespace related issues */
	CheckSetNamespace(oldNspOid, nspOid, classId, objid);

	/* Permission checks ... superusers can always do it */
	if (!superuser())
	{
		Datum		owner;
		Oid			ownerId;
		AclResult	aclresult;

		/* Fail if object does not have an explicit owner */
		if (Anum_owner <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 (errmsg("must be superuser to set schema of %s",
							 getObjectDescriptionOids(classId, objid)))));

		/* Otherwise, must be owner of the existing object */
		owner = heap_getattr(tup, Anum_owner, RelationGetDescr(rel), &isnull);
		Assert(!isnull);
		ownerId = DatumGetObjectId(owner);

		if (!has_privs_of_role(GetUserId(), ownerId))
			aclcheck_error(ACLCHECK_NOT_OWNER, acl_kind,
						   NameStr(*(DatumGetName(name))));

		/* User must have CREATE privilege on new namespace */
		aclresult = pg_namespace_aclcheck(nspOid, GetUserId(), ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
						   get_namespace_name(nspOid));
	}

	/*
	 * Check for duplicate name (more friendly than unique-index failure).
	 * Since this is just a friendliness check, we can just skip it in cases
	 * where there isn't a suitable syscache available.
	 */
	if (nameCacheId >= 0 &&
		SearchSysCacheExists2(nameCacheId, name, ObjectIdGetDatum(nspOid)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("%s already exists in schema \"%s\"",
						getObjectDescriptionOids(classId, objid),
						get_namespace_name(nspOid))));

	/* Build modified tuple */
	values = palloc0(RelationGetNumberOfAttributes(rel) * sizeof(Datum));
	nulls = palloc0(RelationGetNumberOfAttributes(rel) * sizeof(bool));
	replaces = palloc0(RelationGetNumberOfAttributes(rel) * sizeof(bool));
	values[Anum_namespace - 1] = ObjectIdGetDatum(nspOid);
	replaces[Anum_namespace - 1] = true;
	newtup = heap_modify_tuple(tup, RelationGetDescr(rel),
							   values, nulls, replaces);

	/* Perform actual update */
	simple_heap_update(rel, &tup->t_self, newtup);
	CatalogUpdateIndexes(rel, newtup);

	/* Release memory */
	pfree(values);
	pfree(nulls);
	pfree(replaces);

	/* update dependencies to point to the new schema */
	changeDependencyFor(classId, objid,
						NamespaceRelationId, oldNspOid, nspOid);

	return oldNspOid;
}


/*
 * Executes an ALTER OBJECT / OWNER TO statement.  Based on the object
 * type, the function appropriate to that type is executed.
 */
void
ExecAlterOwnerStmt(AlterOwnerStmt *stmt)
{
	Oid			newowner = get_role_oid(stmt->newowner, false);

	switch (stmt->objectType)
	{
		case OBJECT_DATABASE:
			AlterDatabaseOwner(strVal(linitial(stmt->object)), newowner);
			break;

		case OBJECT_SCHEMA:
			AlterSchemaOwner(strVal(linitial(stmt->object)), newowner);
			break;

		case OBJECT_TYPE:
		case OBJECT_DOMAIN:		/* same as TYPE */
			AlterTypeOwner(stmt->object, newowner, stmt->objectType);
			break;

		case OBJECT_FDW:
			AlterForeignDataWrapperOwner(strVal(linitial(stmt->object)),
										 newowner);
			break;

		case OBJECT_FOREIGN_SERVER:
			AlterForeignServerOwner(strVal(linitial(stmt->object)), newowner);
			break;

		case OBJECT_EVENT_TRIGGER:
			AlterEventTriggerOwner(strVal(linitial(stmt->object)), newowner);
			break;

		/* Generic cases */
		case OBJECT_AGGREGATE:
		case OBJECT_COLLATION:
		case OBJECT_CONVERSION:
		case OBJECT_FUNCTION:
		case OBJECT_LANGUAGE:
		case OBJECT_LARGEOBJECT:
		case OBJECT_OPERATOR:
		case OBJECT_OPCLASS:
		case OBJECT_OPFAMILY:
		case OBJECT_TABLESPACE:
		case OBJECT_TSDICTIONARY:
		case OBJECT_TSCONFIGURATION:
			{
				Relation	catalog;
				Relation	relation;
				Oid			classId;
				ObjectAddress	address;

				address = get_object_address(stmt->objectType,
											 stmt->object,
											 stmt->objarg,
											 &relation,
											 AccessExclusiveLock,
											 false);
				Assert(relation == NULL);
				classId = address.classId;

				/*
				 * XXX - get_object_address returns Oid of pg_largeobject
				 * catalog for OBJECT_LARGEOBJECT because of historical
				 * reasons.  Fix up it here.
				 */
				if (classId == LargeObjectRelationId)
					classId = LargeObjectMetadataRelationId;

				catalog = heap_open(classId, RowExclusiveLock);

				AlterObjectOwner_internal(catalog, address.objectId, newowner);
				heap_close(catalog, RowExclusiveLock);
			}
			break;

		default:
			elog(ERROR, "unrecognized AlterOwnerStmt type: %d",
				 (int) stmt->objectType);
	}
}

/*
 * Return a copy of the tuple for the object with the given object OID, from
 * the given catalog (which must have been opened by the caller and suitably
 * locked).  NULL is returned if the OID is not found.
 *
 * We try a syscache first, if available.
 *
 * XXX this function seems general in possible usage.  Given sufficient callers
 * elsewhere, we should consider moving it to a more appropriate place.
 */
static HeapTuple
get_catalog_object_by_oid(Relation catalog, Oid objectId)
{
	HeapTuple	tuple;
	Oid			classId = RelationGetRelid(catalog);
	int			oidCacheId = get_object_catcache_oid(classId);

	if (oidCacheId > 0)
	{
		tuple = SearchSysCacheCopy1(oidCacheId, ObjectIdGetDatum(objectId));
		if (!HeapTupleIsValid(tuple))  /* should not happen */
			return NULL;
	}
	else
	{
		Oid			oidIndexId = get_object_oid_index(classId);
		SysScanDesc	scan;
		ScanKeyData	skey;

		Assert(OidIsValid(oidIndexId));

		ScanKeyInit(&skey,
					ObjectIdAttributeNumber,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(objectId));

		scan = systable_beginscan(catalog, oidIndexId, true,
								  SnapshotNow, 1, &skey);
		tuple = systable_getnext(scan);
		if (!HeapTupleIsValid(tuple))
		{
			systable_endscan(scan);
			return NULL;
		}
		tuple = heap_copytuple(tuple);

		systable_endscan(scan);
	}

	return tuple;
}

/*
 * Generic function to change the ownership of a given object, for simple
 * cases (won't work for tables, nor other cases where we need to do more than
 * change the ownership column of a single catalog entry).
 *
 * rel: catalog relation containing object (RowExclusiveLock'd by caller)
 * objectId: OID of object to change the ownership of
 * new_ownerId: OID of new object owner
 */
void
AlterObjectOwner_internal(Relation rel, Oid objectId, Oid new_ownerId)
{
	Oid			classId = RelationGetRelid(rel);
	AttrNumber	Anum_owner = get_object_attnum_owner(classId);
	AttrNumber	Anum_namespace = get_object_attnum_namespace(classId);
	AttrNumber	Anum_acl = get_object_attnum_acl(classId);
	AttrNumber	Anum_name = get_object_attnum_name(classId);
	HeapTuple	oldtup;
	Datum		datum;
	bool		isnull;
	Oid			old_ownerId;
	Oid			namespaceId = InvalidOid;

	oldtup = get_catalog_object_by_oid(rel, objectId);
	if (oldtup == NULL)
		elog(ERROR, "cache lookup failed for object %u of catalog \"%s\"",
			 objectId, RelationGetRelationName(rel));

	datum = heap_getattr(oldtup, Anum_owner,
						 RelationGetDescr(rel), &isnull);
	Assert(!isnull);
	old_ownerId = DatumGetObjectId(datum);

	if (Anum_namespace != InvalidAttrNumber)
	{
		datum = heap_getattr(oldtup, Anum_namespace,
							 RelationGetDescr(rel), &isnull);
		Assert(!isnull);
		namespaceId = DatumGetObjectId(datum);
	}

	if (old_ownerId != new_ownerId)
	{
		AttrNumber	nattrs;
		HeapTuple	newtup;
		Datum	   *values;
		bool	   *nulls;
		bool	   *replaces;

		/* Superusers can bypass permission checks */
		if (!superuser())
		{
			AclObjectKind	aclkind = get_object_aclkind(classId);

			/* must be owner */
			if (!has_privs_of_role(GetUserId(), old_ownerId))
			{
				char   *objname;
				char	namebuf[NAMEDATALEN];

				if (Anum_name != InvalidAttrNumber)
				{
					datum = heap_getattr(oldtup, Anum_name,
										 RelationGetDescr(rel), &isnull);
					Assert(!isnull);
					objname = NameStr(*DatumGetName(datum));
				}
				else
				{
					snprintf(namebuf, sizeof(namebuf), "%u",
							 HeapTupleGetOid(oldtup));
					objname = namebuf;
				}
				aclcheck_error(ACLCHECK_NOT_OWNER, aclkind, objname);
			}
			/* Must be able to become new owner */
			check_is_member_of_role(GetUserId(), new_ownerId);

			/* New owner must have CREATE privilege on namespace */
			if (OidIsValid(namespaceId))
			{
				AclResult   aclresult;

				aclresult = pg_namespace_aclcheck(namespaceId, new_ownerId,
												  ACL_CREATE);
				if (aclresult != ACLCHECK_OK)
					aclcheck_error(aclresult, aclkind,
								   get_namespace_name(namespaceId));
			}
		}

		/* Build a modified tuple */
		nattrs = RelationGetNumberOfAttributes(rel);
		values = palloc0(nattrs * sizeof(Datum));
		nulls = palloc0(nattrs * sizeof(bool));
		replaces = palloc0(nattrs * sizeof(bool));
		values[Anum_owner - 1] = ObjectIdGetDatum(new_ownerId);
		replaces[Anum_owner - 1] = true;

		/*
		 * Determine the modified ACL for the new owner.  This is only
		 * necessary when the ACL is non-null.
		 */
		if (Anum_acl != InvalidAttrNumber)
		{
			datum = heap_getattr(oldtup,
								 Anum_acl, RelationGetDescr(rel), &isnull);
			if (!isnull)
			{
				Acl    *newAcl;

				newAcl = aclnewowner(DatumGetAclP(datum),
									 old_ownerId, new_ownerId);
				values[Anum_acl - 1] = PointerGetDatum(newAcl);
				replaces[Anum_acl - 1] = true;
			}
		}

		newtup = heap_modify_tuple(oldtup, RelationGetDescr(rel),
								   values, nulls, replaces);

		/* Perform actual update */
		simple_heap_update(rel, &newtup->t_self, newtup);
		CatalogUpdateIndexes(rel, newtup);

		/* Update owner dependency reference */
		if (classId == LargeObjectMetadataRelationId)
			classId = LargeObjectRelationId;
		changeDependencyOnOwner(classId, HeapTupleGetOid(newtup), new_ownerId);

		/* Release memory */
		pfree(values);
		pfree(nulls);
		pfree(replaces);
	}
}
