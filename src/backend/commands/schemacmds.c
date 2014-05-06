/*-------------------------------------------------------------------------
 *
 * schemacmds.c
 *	  schema creation/manipulation commands
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/commands/schemacmds.c,v 1.57 2010/02/26 02:00:39 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_namespace.h"
#include "commands/dbcommands.h"
#include "commands/schemacmds.h"
#include "miscadmin.h"
#include "parser/parse_utilcmd.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static void AlterSchemaOwner_internal(HeapTuple tup, Relation rel, Oid newOwnerId);

/*
 * CREATE SCHEMA
 */
void
CreateSchemaCommand(CreateSchemaStmt *stmt, const char *queryString)
{
	const char *schemaName = stmt->schemaname;
	const char *authId = stmt->authid;
	Oid			namespaceId;
	OverrideSearchPath *overridePath;
	List	   *parsetree_list;
	ListCell   *parsetree_item;
	Oid			owner_uid;
	Oid			saved_uid;
	int			save_sec_context;
	AclResult	aclresult;

	GetUserIdAndSecContext(&saved_uid, &save_sec_context);

	/*
	 * Who is supposed to own the new schema?
	 */
	if (authId)
		owner_uid = get_roleid_checked(authId);
	else
		owner_uid = saved_uid;

	/*
	 * To create a schema, must have schema-create privilege on the current
	 * database and must be able to become the target role (this does not
	 * imply that the target role itself must have create-schema privilege).
	 * The latter provision guards against "giveaway" attacks.  Note that a
	 * superuser will always have both of these privileges a fortiori.
	 */
	aclresult = pg_database_aclcheck(MyDatabaseId, saved_uid, ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_DATABASE,
					   get_database_name(MyDatabaseId));

	check_is_member_of_role(saved_uid, owner_uid);

	/* Additional check to protect reserved schema names */
	if (!allowSystemTableMods && IsReservedName(schemaName))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("unacceptable schema name \"%s\"", schemaName),
		   errdetail("The prefix \"pg_\" is reserved for system schemas.")));

	/*
	 * If the requested authorization is different from the current user,
	 * temporarily set the current user so that the object(s) will be created
	 * with the correct ownership.
	 *
	 * (The setting will be restored at the end of this routine, or in case of
	 * error, transaction abort will clean things up.)
	 */
	if (saved_uid != owner_uid)
		SetUserIdAndSecContext(owner_uid,
							save_sec_context | SECURITY_LOCAL_USERID_CHANGE);

	/* Create the schema's namespace */
	namespaceId = NamespaceCreate(schemaName, owner_uid);

	/* Advance cmd counter to make the namespace visible */
	CommandCounterIncrement();

	/*
	 * Temporarily make the new namespace be the front of the search path, as
	 * well as the default creation target namespace.  This will be undone at
	 * the end of this routine, or upon error.
	 */
	overridePath = GetOverrideSearchPath(CurrentMemoryContext);
	overridePath->schemas = lcons_oid(namespaceId, overridePath->schemas);
	/* XXX should we clear overridePath->useTemp? */
	PushOverrideSearchPath(overridePath);

	/*
	 * Examine the list of commands embedded in the CREATE SCHEMA command, and
	 * reorganize them into a sequentially executable order with no forward
	 * references.  Note that the result is still a list of raw parsetrees ---
	 * we cannot, in general, run parse analysis on one statement until we
	 * have actually executed the prior ones.
	 */
	parsetree_list = transformCreateSchemaStmt(stmt);

	/*
	 * Execute each command contained in the CREATE SCHEMA.  Since the grammar
	 * allows only utility commands in CREATE SCHEMA, there is no need to pass
	 * them through parse_analyze() or the rewriter; we can just hand them
	 * straight to ProcessUtility.
	 */
	foreach(parsetree_item, parsetree_list)
	{
		Node	   *stmt = (Node *) lfirst(parsetree_item);

		/* do this step */
		ProcessUtility(stmt,
					   queryString,
					   NULL,
					   false,	/* not top level */
					   None_Receiver,
					   NULL);
		/* make sure later steps can see the object created here */
		CommandCounterIncrement();
	}

	/* Reset search path to normal state */
	PopOverrideSearchPath();

	/* Reset current user and security context */
	SetUserIdAndSecContext(saved_uid, save_sec_context);
}


/*
 *	RemoveSchemas
 *		Implements DROP SCHEMA.
 */
void
RemoveSchemas(DropStmt *drop)
{
	ObjectAddresses *objects;
	ListCell   *cell;

	/*
	 * First we identify all the schemas, then we delete them in a single
	 * performMultipleDeletions() call.  This is to avoid unwanted DROP
	 * RESTRICT errors if one of the schemas depends on another.
	 */
	objects = new_object_addresses();

	foreach(cell, drop->objects)
	{
		List	   *names = (List *) lfirst(cell);
		char	   *namespaceName;
		Oid			namespaceId;
		ObjectAddress object;

		if (list_length(names) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("schema name cannot be qualified")));
		namespaceName = strVal(linitial(names));

		namespaceId = GetSysCacheOid1(NAMESPACENAME,
									  CStringGetDatum(namespaceName));

		if (!OidIsValid(namespaceId))
		{
			if (!drop->missing_ok)
			{
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_SCHEMA),
						 errmsg("schema \"%s\" does not exist",
								namespaceName)));
			}
			else
			{
				ereport(NOTICE,
						(errmsg("schema \"%s\" does not exist, skipping",
								namespaceName)));
			}
			continue;
		}

		/* Permission check */
		if (!pg_namespace_ownercheck(namespaceId, GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_NAMESPACE,
						   namespaceName);

		object.classId = NamespaceRelationId;
		object.objectId = namespaceId;
		object.objectSubId = 0;

		add_exact_object_address(&object, objects);
	}

	/*
	 * Do the deletions.  Objects contained in the schema(s) are removed by
	 * means of their dependency links to the schema.
	 */
	performMultipleDeletions(objects, drop->behavior);

	free_object_addresses(objects);
}


/*
 * Guts of schema deletion.
 */
void
RemoveSchemaById(Oid schemaOid)
{
	Relation	relation;
	HeapTuple	tup;

	relation = heap_open(NamespaceRelationId, RowExclusiveLock);

	tup = SearchSysCache1(NAMESPACEOID,
						  ObjectIdGetDatum(schemaOid));
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for namespace %u", schemaOid);

	simple_heap_delete(relation, &tup->t_self);

	ReleaseSysCache(tup);

	heap_close(relation, RowExclusiveLock);
}


/*
 * Rename schema
 */
void
RenameSchema(const char *oldname, const char *newname)
{
	HeapTuple	tup;
	Relation	rel;
	AclResult	aclresult;

	rel = heap_open(NamespaceRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(NAMESPACENAME, CStringGetDatum(oldname));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_SCHEMA),
				 errmsg("schema \"%s\" does not exist", oldname)));

	/* make sure the new name doesn't exist */
	if (HeapTupleIsValid(
						 SearchSysCache1(NAMESPACENAME,
										 CStringGetDatum(newname))))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_SCHEMA),
				 errmsg("schema \"%s\" already exists", newname)));

	/* must be owner */
	if (!pg_namespace_ownercheck(HeapTupleGetOid(tup), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_NAMESPACE,
					   oldname);

	/* must have CREATE privilege on database */
	aclresult = pg_database_aclcheck(MyDatabaseId, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_DATABASE,
					   get_database_name(MyDatabaseId));

	if (!allowSystemTableMods && IsReservedName(newname))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("unacceptable schema name \"%s\"", newname),
		   errdetail("The prefix \"pg_\" is reserved for system schemas.")));

	/* rename */
	namestrcpy(&(((Form_pg_namespace) GETSTRUCT(tup))->nspname), newname);
	simple_heap_update(rel, &tup->t_self, tup);
	CatalogUpdateIndexes(rel, tup);

	heap_close(rel, NoLock);
	heap_freetuple(tup);
}

void
AlterSchemaOwner_oid(Oid oid, Oid newOwnerId)
{
	HeapTuple	tup;
	Relation	rel;

	rel = heap_open(NamespaceRelationId, RowExclusiveLock);

	tup = SearchSysCache1(NAMESPACEOID, ObjectIdGetDatum(oid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for schema %u", oid);

	AlterSchemaOwner_internal(tup, rel, newOwnerId);

	ReleaseSysCache(tup);

	heap_close(rel, RowExclusiveLock);
}


/*
 * Change schema owner
 */
void
AlterSchemaOwner(const char *name, Oid newOwnerId)
{
	HeapTuple	tup;
	Relation	rel;

	rel = heap_open(NamespaceRelationId, RowExclusiveLock);

	tup = SearchSysCache1(NAMESPACENAME, CStringGetDatum(name));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_SCHEMA),
				 errmsg("schema \"%s\" does not exist", name)));

	AlterSchemaOwner_internal(tup, rel, newOwnerId);

	ReleaseSysCache(tup);

	heap_close(rel, RowExclusiveLock);
}

static void
AlterSchemaOwner_internal(HeapTuple tup, Relation rel, Oid newOwnerId)
{
	Form_pg_namespace nspForm;

	Assert(tup->t_tableOid == NamespaceRelationId);
	Assert(RelationGetRelid(rel) == NamespaceRelationId);

	nspForm = (Form_pg_namespace) GETSTRUCT(tup);

	/*
	 * If the new owner is the same as the existing owner, consider the
	 * command to have succeeded.  This is for dump restoration purposes.
	 */
	if (nspForm->nspowner != newOwnerId)
	{
		Datum		repl_val[Natts_pg_namespace];
		bool		repl_null[Natts_pg_namespace];
		bool		repl_repl[Natts_pg_namespace];
		Acl		   *newAcl;
		Datum		aclDatum;
		bool		isNull;
		HeapTuple	newtuple;
		AclResult	aclresult;

		/* Otherwise, must be owner of the existing object */
		if (!pg_namespace_ownercheck(HeapTupleGetOid(tup), GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_NAMESPACE,
						   NameStr(nspForm->nspname));

		/* Must be able to become new owner */
		check_is_member_of_role(GetUserId(), newOwnerId);

		/*
		 * must have create-schema rights
		 *
		 * NOTE: This is different from other alter-owner checks in that the
		 * current user is checked for create privileges instead of the
		 * destination owner.  This is consistent with the CREATE case for
		 * schemas.  Because superusers will always have this right, we need
		 * no special case for them.
		 */
		aclresult = pg_database_aclcheck(MyDatabaseId, GetUserId(),
										 ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_DATABASE,
						   get_database_name(MyDatabaseId));

		memset(repl_null, false, sizeof(repl_null));
		memset(repl_repl, false, sizeof(repl_repl));

		repl_repl[Anum_pg_namespace_nspowner - 1] = true;
		repl_val[Anum_pg_namespace_nspowner - 1] = ObjectIdGetDatum(newOwnerId);

		/*
		 * Determine the modified ACL for the new owner.  This is only
		 * necessary when the ACL is non-null.
		 */
		aclDatum = SysCacheGetAttr(NAMESPACENAME, tup,
								   Anum_pg_namespace_nspacl,
								   &isNull);
		if (!isNull)
		{
			newAcl = aclnewowner(DatumGetAclP(aclDatum),
								 nspForm->nspowner, newOwnerId);
			repl_repl[Anum_pg_namespace_nspacl - 1] = true;
			repl_val[Anum_pg_namespace_nspacl - 1] = PointerGetDatum(newAcl);
		}

		newtuple = heap_modify_tuple(tup, RelationGetDescr(rel), repl_val, repl_null, repl_repl);

		simple_heap_update(rel, &newtuple->t_self, newtuple);
		CatalogUpdateIndexes(rel, newtuple);

		heap_freetuple(newtuple);

		/* Update owner dependency reference */
		changeDependencyOnOwner(NamespaceRelationId, HeapTupleGetOid(tup),
								newOwnerId);
	}

}
