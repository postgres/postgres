/*-------------------------------------------------------------------------
 *
 * alter.c
 *	  Drivers for generic alter commands
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/alter.c,v 1.2 2003/07/20 21:56:32 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "commands/alter.h"
#include "commands/conversioncmds.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/proclang.h"
#include "commands/schemacmds.h"
#include "commands/tablecmds.h"
#include "commands/trigger.h"
#include "commands/user.h"
#include "miscadmin.h"
#include "parser/parse_clause.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static void
CheckOwnership(RangeVar *rel, bool noCatalogs)
{
	Oid			relOid;
	HeapTuple	tuple;

	relOid = RangeVarGetRelid(rel, false);
	tuple = SearchSysCache(RELOID,
						   ObjectIdGetDatum(relOid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple)) /* should not happen */
		elog(ERROR, "cache lookup failed for relation %u", relOid);

	if (!pg_class_ownercheck(relOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, rel->relname);

	if (noCatalogs)
	{
		if (!allowSystemTableMods &&
			IsSystemClass((Form_pg_class) GETSTRUCT(tuple)))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("relation \"%s\" is a system catalog",
							rel->relname)));
	}

	ReleaseSysCache(tuple);
}


void
ExecRenameStmt(RenameStmt *stmt)
{
	switch (stmt->renameType)
	{
		case OBJECT_AGGREGATE:
			RenameAggregate(stmt->object, (TypeName *) lfirst(stmt->objarg), stmt->newname);
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

		case OBJECT_GROUP:
			RenameGroup(stmt->subname, stmt->newname);
			break;

		case OBJECT_LANGUAGE:
			RenameLanguage(stmt->subname, stmt->newname);
			break;

		case OBJECT_OPCLASS:
			RenameOpClass(stmt->object, stmt->subname, stmt->newname);
			break;

		case OBJECT_SCHEMA:
			RenameSchema(stmt->subname, stmt->newname);
			break;

		case OBJECT_USER:
			RenameUser(stmt->subname, stmt->newname);
			break;

		case OBJECT_TABLE:
		case OBJECT_COLUMN:
		case OBJECT_TRIGGER:
		{
			Oid			relid;

			CheckOwnership(stmt->relation, true);

			relid = RangeVarGetRelid(stmt->relation, false);

			switch (stmt->renameType)
			{
				case OBJECT_TABLE:
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
						aclcheck_error(aclresult,
									   get_namespace_name(namespaceId));

					renamerel(relid, stmt->newname);
					break;
				}
				case OBJECT_COLUMN:
					renameatt(relid,
							  stmt->subname,		/* old att name */
							  stmt->newname,		/* new att name */
							  interpretInhOption(stmt->relation->inhOpt),		/* recursive? */
							  false);		/* recursing already? */
					break;
				case OBJECT_TRIGGER:
					renametrig(relid,
							   stmt->subname,		/* old att name */
							   stmt->newname);		/* new att name */
					break;
				default:
					/*can't happen*/;
			}
			break;
		}

		default:
			elog(ERROR, "unrecognized rename stmt type: %d",
				 (int) stmt->renameType);
	}
}
