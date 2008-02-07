/*-------------------------------------------------------------------------
 *
 * alter.c
 *	  Drivers for generic alter commands
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/commands/alter.c,v 1.20.2.1 2008/02/07 21:08:04 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/namespace.h"
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
#include "utils/lsyscache.h"


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
		case OBJECT_INDEX:
		case OBJECT_COLUMN:
		case OBJECT_TRIGGER:
			{
				Oid			relid;

				CheckRelationOwnership(stmt->relation, true);

				relid = RangeVarGetRelid(stmt->relation, false);

				switch (stmt->renameType)
				{
					case OBJECT_TABLE:
					case OBJECT_INDEX:
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

							renamerel(relid, stmt->newname);
							break;
						}
					case OBJECT_COLUMN:
						renameatt(relid,
								  stmt->subname,		/* old att name */
								  stmt->newname,		/* new att name */
								  interpretInhOption(stmt->relation->inhOpt),	/* recursive? */
								  false);		/* recursing already? */
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

		case OBJECT_FUNCTION:
			AlterFunctionNamespace(stmt->object, stmt->objarg, false,
								   stmt->newschema);
			break;

		case OBJECT_SEQUENCE:
		case OBJECT_TABLE:
			CheckRelationOwnership(stmt->relation, true);
			AlterTableNamespace(stmt->relation, stmt->newschema);
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
 * Executes an ALTER OBJECT / OWNER TO statement.  Based on the object
 * type, the function appropriate to that type is executed.
 */
void
ExecAlterOwnerStmt(AlterOwnerStmt *stmt)
{
	Oid			newowner = get_roleid_checked(stmt->newowner);

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

		default:
			elog(ERROR, "unrecognized AlterOwnerStmt type: %d",
				 (int) stmt->objectType);
	}
}
