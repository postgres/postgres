/*-------------------------------------------------------------------------
 *
 * alter.c
 *	  Drivers for generic alter commands
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/alter.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_largeobject.h"
#include "catalog/pg_namespace.h"
#include "commands/alter.h"
#include "commands/conversioncmds.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/proclang.h"
#include "commands/schemacmds.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "commands/typecmds.h"
#include "commands/user.h"
#include "miscadmin.h"
#include "parser/parse_clause.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


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

		case OBJECT_CONVERSION:
			RenameConversion(stmt->object, stmt->newname);
			break;

		case OBJECT_DATABASE:
			RenameDatabase(stmt->subname, stmt->newname);
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
		case OBJECT_COLUMN:
		case OBJECT_ATTRIBUTE:
		case OBJECT_TRIGGER:
		case OBJECT_FOREIGN_TABLE:
			{
				Oid			relid;

				CheckRelationOwnership(stmt->relation, true);

				relid = RangeVarGetRelid(stmt->relation, false);

				switch (stmt->renameType)
				{
					case OBJECT_TABLE:
					case OBJECT_SEQUENCE:
					case OBJECT_VIEW:
					case OBJECT_INDEX:
					case OBJECT_FOREIGN_TABLE:
						{
							/*
							 * RENAME TABLE requires that we (still) hold
							 * CREATE rights on the containing namespace, as
							 * well as ownership of the table.
							 */
							Oid			namespaceId = get_rel_namespace(relid);
							AclResult	aclresult;

							aclresult = pg_namespace_aclcheck(namespaceId,
															  GetUserId(),
															  ACL_CREATE);
							if (aclresult != ACLCHECK_OK)
								aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
											get_namespace_name(namespaceId));

							RenameRelation(relid, stmt->newname, stmt->renameType);
							break;
						}
					case OBJECT_COLUMN:
					case OBJECT_ATTRIBUTE:
						renameatt(relid, stmt);
						break;
					case OBJECT_TRIGGER:
						renametrig(relid,
								   stmt->subname,		/* old att name */
								   stmt->newname);		/* new att name */
						break;
					default:
						 /* can't happen */ ;
				}
				break;
			}

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

		case OBJECT_TYPE:
			RenameType(stmt->object, stmt->newname);
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

		case OBJECT_CONVERSION:
			AlterConversionNamespace(stmt->object, stmt->newschema);
			break;

		case OBJECT_FUNCTION:
			AlterFunctionNamespace(stmt->object, stmt->objarg, false,
								   stmt->newschema);
			break;

		case OBJECT_OPERATOR:
			AlterOperatorNamespace(stmt->object, stmt->objarg, stmt->newschema);
			break;

		case OBJECT_OPCLASS:
			AlterOpClassNamespace(stmt->object, stmt->objarg, stmt->newschema);
			break;

		case OBJECT_OPFAMILY:
			AlterOpFamilyNamespace(stmt->object, stmt->objarg, stmt->newschema);
			break;

		case OBJECT_SEQUENCE:
		case OBJECT_TABLE:
		case OBJECT_VIEW:
		case OBJECT_FOREIGN_TABLE:
			CheckRelationOwnership(stmt->relation, true);
			AlterTableNamespace(stmt->relation, stmt->newschema,
								stmt->objectType, AccessExclusiveLock);
			break;

		case OBJECT_TSPARSER:
			AlterTSParserNamespace(stmt->object, stmt->newschema);
			break;

		case OBJECT_TSDICTIONARY:
			AlterTSDictionaryNamespace(stmt->object, stmt->newschema);
			break;

		case OBJECT_TSTEMPLATE:
			AlterTSTemplateNamespace(stmt->object, stmt->newschema);
			break;

		case OBJECT_TSCONFIGURATION:
			AlterTSConfigurationNamespace(stmt->object, stmt->newschema);
			break;

		case OBJECT_TYPE:
		case OBJECT_DOMAIN:
			AlterTypeNamespace(stmt->object, stmt->newschema);
			break;

		default:
			elog(ERROR, "unrecognized AlterObjectSchemaStmt type: %d",
				 (int) stmt->objectType);
	}
}

/*
 * Generic function to change the namespace of a given object, for simple
 * cases (won't work for tables or functions, objects which have more than 2
 * key-attributes to use when searching for their syscache entries --- we
 * don't want nor need to get this generic here).
 *
 * The AlterFooNamespace() calls just above will call a function whose job
 * is to lookup the arguments for the generic function here.
 *
 * Relation must already by open, it's the responsibility of the caller to
 * close it.
 */
void
AlterObjectNamespace(Relation rel, int cacheId,
					 Oid classId, Oid objid, Oid nspOid,
					 int Anum_name, int Anum_namespace, int Anum_owner,
					 AclObjectKind acl_kind,
					 bool superuser_only)
{
	Oid			oldNspOid;
	Datum       name, namespace;
	bool        isnull;
	HeapTuple	tup, newtup = NULL;
	Datum	   *values;
	bool	   *nulls;
	bool	   *replaces;

	tup = SearchSysCacheCopy1(cacheId, ObjectIdGetDatum(objid));
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for object %u: %s",
			 objid, getObjectDescriptionOids(classId, objid));

	name = heap_getattr(tup, Anum_name, rel->rd_att, &isnull);
	namespace = heap_getattr(tup, Anum_namespace, rel->rd_att, &isnull);
	oldNspOid = DatumGetObjectId(namespace);

	/* Check basic namespace related issues */
	CheckSetNamespace(oldNspOid, nspOid, classId, objid);

	/* check for duplicate name (more friendly than unique-index failure) */
	if (SearchSysCacheExists2(cacheId, name, ObjectIdGetDatum(nspOid)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("%s already exists in schema \"%s\"",
						getObjectDescriptionOids(classId, objid),
						get_namespace_name(nspOid))));

	/* Superusers can always do it */
	if (!superuser())
	{
		Datum       owner;
		Oid			ownerId;
		AclResult	aclresult;

		if (superuser_only)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 (errmsg("must be superuser to SET SCHEMA of %s",
							 getObjectDescriptionOids(classId, objid)))));

		/* Otherwise, must be owner of the existing object */
		owner = heap_getattr(tup, Anum_owner, rel->rd_att, &isnull);
		ownerId = DatumGetObjectId(owner);

		if (!has_privs_of_role(GetUserId(), ownerId))
			aclcheck_error(ACLCHECK_NOT_OWNER, acl_kind,
						   NameStr(*(DatumGetName(name))));

		/* owner must have CREATE privilege on namespace */
		aclresult = pg_namespace_aclcheck(oldNspOid, GetUserId(), ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
						   get_namespace_name(oldNspOid));
	}

	/* Prepare to update tuple */
	values = palloc0(rel->rd_att->natts * sizeof(Datum));
	nulls = palloc0(rel->rd_att->natts * sizeof(bool));
	replaces = palloc0(rel->rd_att->natts * sizeof(bool));
	values[Anum_namespace - 1] = nspOid;
	replaces[Anum_namespace - 1] = true;
	newtup = heap_modify_tuple(tup, rel->rd_att, values, nulls, replaces);

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
		case OBJECT_AGGREGATE:
			AlterAggregateOwner(stmt->object, stmt->objarg, newowner);
			break;

		case OBJECT_CONVERSION:
			AlterConversionOwner(stmt->object, newowner);
			break;

		case OBJECT_DATABASE:
			AlterDatabaseOwner(strVal(linitial(stmt->object)), newowner);
			break;

		case OBJECT_FUNCTION:
			AlterFunctionOwner(stmt->object, stmt->objarg, newowner);
			break;

		case OBJECT_LANGUAGE:
			AlterLanguageOwner(strVal(linitial(stmt->object)), newowner);
			break;

		case OBJECT_LARGEOBJECT:
			LargeObjectAlterOwner(oidparse(linitial(stmt->object)), newowner);
			break;

		case OBJECT_OPERATOR:
			Assert(list_length(stmt->objarg) == 2);
			AlterOperatorOwner(stmt->object,
							   (TypeName *) linitial(stmt->objarg),
							   (TypeName *) lsecond(stmt->objarg),
							   newowner);
			break;

		case OBJECT_OPCLASS:
			AlterOpClassOwner(stmt->object, stmt->addname, newowner);
			break;

		case OBJECT_OPFAMILY:
			AlterOpFamilyOwner(stmt->object, stmt->addname, newowner);
			break;

		case OBJECT_SCHEMA:
			AlterSchemaOwner(strVal(linitial(stmt->object)), newowner);
			break;

		case OBJECT_TABLESPACE:
			AlterTableSpaceOwner(strVal(linitial(stmt->object)), newowner);
			break;

		case OBJECT_TYPE:
		case OBJECT_DOMAIN:		/* same as TYPE */
			AlterTypeOwner(stmt->object, newowner);
			break;

		case OBJECT_TSDICTIONARY:
			AlterTSDictionaryOwner(stmt->object, newowner);
			break;

		case OBJECT_TSCONFIGURATION:
			AlterTSConfigurationOwner(stmt->object, newowner);
			break;

		case OBJECT_FDW:
			AlterForeignDataWrapperOwner(strVal(linitial(stmt->object)),
										 newowner);
			break;

		case OBJECT_FOREIGN_SERVER:
			AlterForeignServerOwner(strVal(linitial(stmt->object)), newowner);
			break;

		default:
			elog(ERROR, "unrecognized AlterOwnerStmt type: %d",
				 (int) stmt->objectType);
	}
}
