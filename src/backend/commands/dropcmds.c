/*-------------------------------------------------------------------------
 *
 * dropcmds.c
 *	  handle various "DROP" operations
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/dropcmds.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_class.h"
#include "catalog/pg_proc.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/syscache.h"

static void does_not_exist_skipping(ObjectType objtype,
						List *objname, List *objargs);

/*
 * Drop one or more objects.
 *
 * We don't currently handle all object types here.  Relations, for example,
 * require special handling, because (for example) indexes have additional
 * locking requirements.
 *
 * We look up all the objects first, and then delete them in a single
 * performMultipleDeletions() call.  This avoids unnecessary DROP RESTRICT
 * errors if there are dependencies between them.
 */
void
RemoveObjects(DropStmt *stmt)
{
	ObjectAddresses *objects;
	ListCell   *cell1;
	ListCell   *cell2 = NULL;

	objects = new_object_addresses();

	foreach(cell1, stmt->objects)
	{
		ObjectAddress address;
		List	   *objname = lfirst(cell1);
		List	   *objargs = NIL;
		Relation	relation = NULL;
		Oid			namespaceId;

		if (stmt->arguments)
		{
			cell2 = (!cell2 ? list_head(stmt->arguments) : lnext(cell2));
			objargs = lfirst(cell2);
		}

		/* Get an ObjectAddress for the object. */
		address = get_object_address(stmt->removeType,
									 objname, objargs,
									 &relation,
									 AccessExclusiveLock,
									 stmt->missing_ok);

		/* Issue NOTICE if supplied object was not found. */
		if (!OidIsValid(address.objectId))
		{
			does_not_exist_skipping(stmt->removeType, objname, objargs);
			continue;
		}

		/*
		 * Although COMMENT ON FUNCTION, SECURITY LABEL ON FUNCTION, etc. are
		 * happy to operate on an aggregate as on any other function, we have
		 * historically not allowed this for DROP FUNCTION.
		 */
		if (stmt->removeType == OBJECT_FUNCTION)
		{
			Oid			funcOid = address.objectId;
			HeapTuple	tup;

			tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcOid));
			if (!HeapTupleIsValid(tup)) /* should not happen */
				elog(ERROR, "cache lookup failed for function %u", funcOid);

			if (((Form_pg_proc) GETSTRUCT(tup))->proisagg)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is an aggregate function",
								NameListToString(objname)),
				errhint("Use DROP AGGREGATE to drop aggregate functions.")));

			ReleaseSysCache(tup);
		}

		/* Check permissions. */
		namespaceId = get_object_namespace(&address);
		if (!OidIsValid(namespaceId) ||
			!pg_namespace_ownercheck(namespaceId, GetUserId()))
			check_object_ownership(GetUserId(), stmt->removeType, address,
								   objname, objargs, relation);

		/* Release any relcache reference count, but keep lock until commit. */
		if (relation)
			heap_close(relation, NoLock);

		add_exact_object_address(&address, objects);
	}

	/* Here we really delete them. */
	performMultipleDeletions(objects, stmt->behavior, 0);

	free_object_addresses(objects);
}

/*
 * Generate a NOTICE stating that the named object was not found, and is
 * being skipped.  This is only relevant when "IF EXISTS" is used; otherwise,
 * get_object_address() will throw an ERROR.
 */
static void
does_not_exist_skipping(ObjectType objtype, List *objname, List *objargs)
{
	const char *msg = NULL;
	char	   *name = NULL;
	char	   *args = NULL;

	switch (objtype)
	{
		case OBJECT_TYPE:
		case OBJECT_DOMAIN:
			msg = gettext_noop("type \"%s\" does not exist, skipping");
			name = TypeNameToString(makeTypeNameFromNameList(objname));
			break;
		case OBJECT_COLLATION:
			msg = gettext_noop("collation \"%s\" does not exist, skipping");
			name = NameListToString(objname);
			break;
		case OBJECT_CONVERSION:
			msg = gettext_noop("conversion \"%s\" does not exist, skipping");
			name = NameListToString(objname);
			break;
		case OBJECT_SCHEMA:
			msg = gettext_noop("schema \"%s\" does not exist, skipping");
			name = NameListToString(objname);
			break;
		case OBJECT_TSPARSER:
			msg = gettext_noop("text search parser \"%s\" does not exist, skipping");
			name = NameListToString(objname);
			break;
		case OBJECT_TSDICTIONARY:
			msg = gettext_noop("text search dictionary \"%s\" does not exist, skipping");
			name = NameListToString(objname);
			break;
		case OBJECT_TSTEMPLATE:
			msg = gettext_noop("text search template \"%s\" does not exist, skipping");
			name = NameListToString(objname);
			break;
		case OBJECT_TSCONFIGURATION:
			msg = gettext_noop("text search configuration \"%s\" does not exist, skipping");
			name = NameListToString(objname);
			break;
		case OBJECT_EXTENSION:
			msg = gettext_noop("extension \"%s\" does not exist, skipping");
			name = NameListToString(objname);
			break;
		case OBJECT_FUNCTION:
			msg = gettext_noop("function %s(%s) does not exist, skipping");
			name = NameListToString(objname);
			args = TypeNameListToString(objargs);
			break;
		case OBJECT_AGGREGATE:
			msg = gettext_noop("aggregate %s(%s) does not exist, skipping");
			name = NameListToString(objname);
			args = TypeNameListToString(objargs);
			break;
		case OBJECT_OPERATOR:
			msg = gettext_noop("operator %s does not exist, skipping");
			name = NameListToString(objname);
			break;
		case OBJECT_LANGUAGE:
			msg = gettext_noop("language \"%s\" does not exist, skipping");
			name = NameListToString(objname);
			break;
		case OBJECT_CAST:
			msg = gettext_noop("cast from type %s to type %s does not exist, skipping");
			name = format_type_be(typenameTypeId(NULL,
											(TypeName *) linitial(objname)));
			args = format_type_be(typenameTypeId(NULL,
											(TypeName *) linitial(objargs)));
			break;
		case OBJECT_TRIGGER:
			msg = gettext_noop("trigger \"%s\" for table \"%s\" does not exist, skipping");
			name = strVal(llast(objname));
			args = NameListToString(list_truncate(list_copy(objname),
												  list_length(objname) - 1));
			break;
		case OBJECT_EVENT_TRIGGER:
			msg = gettext_noop("event trigger \"%s\" does not exist, skipping");
			name = NameListToString(objname);
			break;
		case OBJECT_RULE:
			msg = gettext_noop("rule \"%s\" for relation \"%s\" does not exist, skipping");
			name = strVal(llast(objname));
			args = NameListToString(list_truncate(list_copy(objname),
												  list_length(objname) - 1));
			break;
		case OBJECT_FDW:
			msg = gettext_noop("foreign-data wrapper \"%s\" does not exist, skipping");
			name = NameListToString(objname);
			break;
		case OBJECT_FOREIGN_SERVER:
			msg = gettext_noop("server \"%s\" does not exist, skipping");
			name = NameListToString(objname);
			break;
		case OBJECT_OPCLASS:
			msg = gettext_noop("operator class \"%s\" does not exist for access method \"%s\", skipping");
			name = NameListToString(objname);
			args = strVal(linitial(objargs));
			break;
		case OBJECT_OPFAMILY:
			msg = gettext_noop("operator family \"%s\" does not exist for access method \"%s\", skipping");
			name = NameListToString(objname);
			args = strVal(linitial(objargs));
			break;
		default:
			elog(ERROR, "unexpected object type (%d)", (int) objtype);
			break;
	}

	if (!args)
		ereport(NOTICE, (errmsg(msg, name)));
	else
		ereport(NOTICE, (errmsg(msg, name, args)));
}
