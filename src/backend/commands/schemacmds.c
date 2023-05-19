/*-------------------------------------------------------------------------
 *
 * schemacmds.c
 *	  schema creation/manipulation commands
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/schemacmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
#include "commands/dbcommands.h"
#include "commands/event_trigger.h"
#include "commands/schemacmds.h"
#include "miscadmin.h"
#include "parser/parse_utilcmd.h"
#include "parser/scansup.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/syscache.h"

static void AlterSchemaOwner_internal(HeapTuple tup, Relation rel, Oid newOwnerId);

/*
 * CREATE SCHEMA
 *
 * Note: caller should pass in location information for the whole
 * CREATE SCHEMA statement, which in turn we pass down as the location
 * of the component commands.  This comports with our general plan of
 * reporting location/len for the whole command even when executing
 * a subquery.
 */
Oid
CreateSchemaCommand(CreateSchemaStmt *stmt, const char *queryString,
					int stmt_location, int stmt_len)
{
	const char *schemaName = stmt->schemaname;
	Oid			namespaceId;
	List	   *parsetree_list;
	ListCell   *parsetree_item;
	Oid			owner_uid;
	Oid			saved_uid;
	int			save_sec_context;
	int			save_nestlevel;
	char	   *nsp = namespace_search_path;
	AclResult	aclresult;
	ObjectAddress address;
	StringInfoData pathbuf;

	GetUserIdAndSecContext(&saved_uid, &save_sec_context);

	/*
	 * Who is supposed to own the new schema?
	 */
	if (stmt->authrole)
		owner_uid = get_rolespec_oid(stmt->authrole, false);
	else
		owner_uid = saved_uid;

	/* fill schema name with the user name if not specified */
	if (!schemaName)
	{
		HeapTuple	tuple;

		tuple = SearchSysCache1(AUTHOID, ObjectIdGetDatum(owner_uid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for role %u", owner_uid);
		schemaName =
			pstrdup(NameStr(((Form_pg_authid) GETSTRUCT(tuple))->rolname));
		ReleaseSysCache(tuple);
	}

	/*
	 * To create a schema, must have schema-create privilege on the current
	 * database and must be able to become the target role (this does not
	 * imply that the target role itself must have create-schema privilege).
	 * The latter provision guards against "giveaway" attacks.  Note that a
	 * superuser will always have both of these privileges a fortiori.
	 */
	aclresult = object_aclcheck(DatabaseRelationId, MyDatabaseId, saved_uid, ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_DATABASE,
					   get_database_name(MyDatabaseId));

	check_can_set_role(saved_uid, owner_uid);

	/* Additional check to protect reserved schema names */
	if (!allowSystemTableMods && IsReservedName(schemaName))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("unacceptable schema name \"%s\"", schemaName),
				 errdetail("The prefix \"pg_\" is reserved for system schemas.")));

	/*
	 * If if_not_exists was given and the schema already exists, bail out.
	 * (Note: we needn't check this when not if_not_exists, because
	 * NamespaceCreate will complain anyway.)  We could do this before making
	 * the permissions checks, but since CREATE TABLE IF NOT EXISTS makes its
	 * creation-permission check first, we do likewise.
	 */
	if (stmt->if_not_exists)
	{
		namespaceId = get_namespace_oid(schemaName, true);
		if (OidIsValid(namespaceId))
		{
			/*
			 * If we are in an extension script, insist that the pre-existing
			 * object be a member of the extension, to avoid security risks.
			 */
			ObjectAddressSet(address, NamespaceRelationId, namespaceId);
			checkMembershipInCurrentExtension(&address);

			/* OK to skip */
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_SCHEMA),
					 errmsg("schema \"%s\" already exists, skipping",
							schemaName)));
			return InvalidOid;
		}
	}

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
	namespaceId = NamespaceCreate(schemaName, owner_uid, false);

	/* Advance cmd counter to make the namespace visible */
	CommandCounterIncrement();

	/*
	 * Prepend the new schema to the current search path.
	 *
	 * We use the equivalent of a function SET option to allow the setting to
	 * persist for exactly the duration of the schema creation.  guc.c also
	 * takes care of undoing the setting on error.
	 */
	save_nestlevel = NewGUCNestLevel();

	initStringInfo(&pathbuf);
	appendStringInfoString(&pathbuf, quote_identifier(schemaName));

	while (scanner_isspace(*nsp))
		nsp++;

	if (*nsp != '\0')
		appendStringInfo(&pathbuf, ", %s", nsp);

	(void) set_config_option("search_path", pathbuf.data,
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);

	/*
	 * Report the new schema to possibly interested event triggers.  Note we
	 * must do this here and not in ProcessUtilitySlow because otherwise the
	 * objects created below are reported before the schema, which would be
	 * wrong.
	 */
	ObjectAddressSet(address, NamespaceRelationId, namespaceId);
	EventTriggerCollectSimpleCommand(address, InvalidObjectAddress,
									 (Node *) stmt);

	/*
	 * Examine the list of commands embedded in the CREATE SCHEMA command, and
	 * reorganize them into a sequentially executable order with no forward
	 * references.  Note that the result is still a list of raw parsetrees ---
	 * we cannot, in general, run parse analysis on one statement until we
	 * have actually executed the prior ones.
	 */
	parsetree_list = transformCreateSchemaStmtElements(stmt->schemaElts,
													   schemaName);

	/*
	 * Execute each command contained in the CREATE SCHEMA.  Since the grammar
	 * allows only utility commands in CREATE SCHEMA, there is no need to pass
	 * them through parse_analyze_*() or the rewriter; we can just hand them
	 * straight to ProcessUtility.
	 */
	foreach(parsetree_item, parsetree_list)
	{
		Node	   *stmt = (Node *) lfirst(parsetree_item);
		PlannedStmt *wrapper;

		/* need to make a wrapper PlannedStmt */
		wrapper = makeNode(PlannedStmt);
		wrapper->commandType = CMD_UTILITY;
		wrapper->canSetTag = false;
		wrapper->utilityStmt = stmt;
		wrapper->stmt_location = stmt_location;
		wrapper->stmt_len = stmt_len;

		/* do this step */
		ProcessUtility(wrapper,
					   queryString,
					   false,
					   PROCESS_UTILITY_SUBCOMMAND,
					   NULL,
					   NULL,
					   None_Receiver,
					   NULL);

		/* make sure later steps can see the object created here */
		CommandCounterIncrement();
	}

	/*
	 * Restore the GUC variable search_path we set above.
	 */
	AtEOXact_GUC(true, save_nestlevel);

	/* Reset current user and security context */
	SetUserIdAndSecContext(saved_uid, save_sec_context);

	return namespaceId;
}


/*
 * Rename schema
 */
ObjectAddress
RenameSchema(const char *oldname, const char *newname)
{
	Oid			nspOid;
	HeapTuple	tup;
	Relation	rel;
	AclResult	aclresult;
	ObjectAddress address;
	Form_pg_namespace nspform;

	rel = table_open(NamespaceRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(NAMESPACENAME, CStringGetDatum(oldname));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_SCHEMA),
				 errmsg("schema \"%s\" does not exist", oldname)));

	nspform = (Form_pg_namespace) GETSTRUCT(tup);
	nspOid = nspform->oid;

	/* make sure the new name doesn't exist */
	if (OidIsValid(get_namespace_oid(newname, true)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_SCHEMA),
				 errmsg("schema \"%s\" already exists", newname)));

	/* must be owner */
	if (!object_ownercheck(NamespaceRelationId, nspOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_SCHEMA,
					   oldname);

	/* must have CREATE privilege on database */
	aclresult = object_aclcheck(DatabaseRelationId, MyDatabaseId, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_DATABASE,
					   get_database_name(MyDatabaseId));

	if (!allowSystemTableMods && IsReservedName(newname))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("unacceptable schema name \"%s\"", newname),
				 errdetail("The prefix \"pg_\" is reserved for system schemas.")));

	/* rename */
	namestrcpy(&nspform->nspname, newname);
	CatalogTupleUpdate(rel, &tup->t_self, tup);

	InvokeObjectPostAlterHook(NamespaceRelationId, nspOid, 0);

	ObjectAddressSet(address, NamespaceRelationId, nspOid);

	table_close(rel, NoLock);
	heap_freetuple(tup);

	return address;
}

void
AlterSchemaOwner_oid(Oid schemaoid, Oid newOwnerId)
{
	HeapTuple	tup;
	Relation	rel;

	rel = table_open(NamespaceRelationId, RowExclusiveLock);

	tup = SearchSysCache1(NAMESPACEOID, ObjectIdGetDatum(schemaoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for schema %u", schemaoid);

	AlterSchemaOwner_internal(tup, rel, newOwnerId);

	ReleaseSysCache(tup);

	table_close(rel, RowExclusiveLock);
}


/*
 * Change schema owner
 */
ObjectAddress
AlterSchemaOwner(const char *name, Oid newOwnerId)
{
	Oid			nspOid;
	HeapTuple	tup;
	Relation	rel;
	ObjectAddress address;
	Form_pg_namespace nspform;

	rel = table_open(NamespaceRelationId, RowExclusiveLock);

	tup = SearchSysCache1(NAMESPACENAME, CStringGetDatum(name));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_SCHEMA),
				 errmsg("schema \"%s\" does not exist", name)));

	nspform = (Form_pg_namespace) GETSTRUCT(tup);
	nspOid = nspform->oid;

	AlterSchemaOwner_internal(tup, rel, newOwnerId);

	ObjectAddressSet(address, NamespaceRelationId, nspOid);

	ReleaseSysCache(tup);

	table_close(rel, RowExclusiveLock);

	return address;
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
		if (!object_ownercheck(NamespaceRelationId, nspForm->oid, GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_SCHEMA,
						   NameStr(nspForm->nspname));

		/* Must be able to become new owner */
		check_can_set_role(GetUserId(), newOwnerId);

		/*
		 * must have create-schema rights
		 *
		 * NOTE: This is different from other alter-owner checks in that the
		 * current user is checked for create privileges instead of the
		 * destination owner.  This is consistent with the CREATE case for
		 * schemas.  Because superusers will always have this right, we need
		 * no special case for them.
		 */
		aclresult = object_aclcheck(DatabaseRelationId, MyDatabaseId, GetUserId(),
									ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_DATABASE,
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

		CatalogTupleUpdate(rel, &newtuple->t_self, newtuple);

		heap_freetuple(newtuple);

		/* Update owner dependency reference */
		changeDependencyOnOwner(NamespaceRelationId, nspForm->oid,
								newOwnerId);
	}

	InvokeObjectPostAlterHook(NamespaceRelationId,
							  nspForm->oid, 0);
}
