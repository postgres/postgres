/*-------------------------------------------------------------------------
 *
 * modulecmds.c
 *	  module creation/manipulation commands
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/modulecmds.c
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
#include "catalog/pg_namespace.h"
#include "commands/dbcommands.h"
#include "commands/event_trigger.h"
#include "commands/modulecmds.h"
#include "miscadmin.h"
#include "parser/parse_utilcmd.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

/*
 * CREATE MODULE
 *
 * Note: caller should pass in location information for the whole
 * CREATE MODULE statement, which in turn we pass down as the location
 * of the component commands.  This comports with our general plan of
 * reporting location/len for the whole command even when executing
 * a subquery.
 */
ObjectAddress
CreateModuleCommand(ParseState *pstate, CreateModuleStmt *stmt, const char *queryString,
					int stmt_location, int stmt_len)
{
	char	   *modulename;
	Oid			namespaceId;
	Oid			moduleId;
	OverrideSearchPath *overridePath;
	List	   *parsetree_list;
	ListCell   *parsetree_item;
	Oid			owner_uid;
	Oid			saved_uid;
	int			save_sec_context;
	AclResult	aclresult;
	ObjectAddress myself,
				referenced;
	ObjectAddresses *addrs;

	GetUserIdAndSecContext(&saved_uid, &save_sec_context);

	/*
	 * Who is supposed to own the new module?
	 */
	if (stmt->authrole)
		owner_uid = get_rolespec_oid(stmt->authrole, false);
	else
		owner_uid = saved_uid;

	/* Convert list of names to a name and namespace */
	namespaceId = QualifiedNameGetCreationNamespace(stmt->modulename,
													&modulename, false);

	/* Check we have creation rights in target namespace */
	aclresult = pg_namespace_aclcheck(namespaceId, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(namespaceId));

	/*
	 * If if_not_exists was given and the module already exists, bail out.
	 * (Note: we needn't check this when not if_not_exists, because
	 * NamespaceCreate will complain anyway.)  We could do this before making
	 * the permissions checks, but since CREATE TABLE IF NOT EXISTS makes its
	 * creation-permission check first, we do likewise.
	 */

	if (stmt->if_not_exists &&
		SearchSysCacheExists2(NAMESPACENAME, PointerGetDatum(modulename),
							  ObjectIdGetDatum(namespaceId)))
	{
		ereport(NOTICE,
				(errcode(ERRCODE_DUPLICATE_SCHEMA),
				 errmsg("module \"%s\" already exists, skipping",
						modulename)));
		return InvalidObjectAddress;
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

	/* Create the module's namespace */
	moduleId = NamespaceCreate(modulename, namespaceId, NSPKIND_MODULE,
							   owner_uid, false);

	/* Advance cmd counter to make the namespace visible */
	CommandCounterIncrement();

	/*
	 * Temporarily make the new namespace be the front of the search path, as
	 * well as the default creation target namespace.  This will be undone at
	 * the end of this routine, or upon error.
	 */
	overridePath = GetOverrideSearchPath(CurrentMemoryContext);
	overridePath->schemas = lcons_oid(moduleId, overridePath->schemas);
	/* XXX should we clear overridePath->useTemp? */
	PushOverrideSearchPath(overridePath);

	/*
	 * Report the new module to possibly interested event triggers.  Note we
	 * must do this here and not in ProcessUtilitySlow because otherwise the
	 * objects created below are reported before the module, which would be
	 * wrong.
	 */
	ObjectAddressSet(myself, NamespaceRelationId, moduleId);
	EventTriggerCollectSimpleCommand(myself, InvalidObjectAddress,
									 (Node *) stmt);

	/*
	 * Examine the list of commands embedded in the CREATE MODULE command, and
	 * reorganize them into a sequentially executable order with no forward
	 * references.  Note that the result is still a list of raw parsetrees ---
	 * we cannot, in general, run parse analysis on one statement until we
	 * have actually executed the prior ones.
	 */
	parsetree_list = transformCreateModuleStmt(stmt);

	/*
	 * Execute each command contained in the CREATE MODULE.  Since the grammar
	 * allows only utility commands in CREATE MODULE, there is no need to pass
	 * them through parse_analyze() or the rewriter; we can just hand them
	 * straight to ProcessUtility.
	 */
	foreach(parsetree_item, parsetree_list)
	{
		Node	   *stmt = (Node *) lfirst(parsetree_item);
		PlannedStmt *wrapper;

		wrapper = makeNode(PlannedStmt);
		wrapper->commandType = CMD_UTILITY;
		wrapper->canSetTag = false;
		wrapper->utilityStmt = stmt;
		wrapper->stmt_location = stmt_location;
		wrapper->stmt_len = stmt_len;

		ProcessUtility(wrapper,
					   queryString,
					   false,
					   PROCESS_UTILITY_SUBCOMMAND,
					   NULL,
					   NULL,
					   None_Receiver,
					   NULL);

		CommandCounterIncrement();
	}

	/* Reset search path to normal state */
	PopOverrideSearchPath();

	/* Reset current user and security context */
	SetUserIdAndSecContext(saved_uid, save_sec_context);

	addrs = new_object_addresses();


	/* dependency on namespace */
	ObjectAddressSet(referenced, NamespaceRelationId, namespaceId);
	add_exact_object_address(&referenced, addrs);

	record_object_address_dependencies(&myself, addrs, DEPENDENCY_NORMAL);
	free_object_addresses(addrs);

	recordDependencyOnOwner(NamespaceRelationId, moduleId, owner_uid);

	return myself;
}
