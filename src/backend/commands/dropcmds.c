/*-------------------------------------------------------------------------
 *
 * dropcmds.c
 *	  handle various "DROP" operations
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
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
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_class.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_type.h"
#include "utils/acl.h"

static void does_not_exist_skipping(ObjectType objtype, List *objname);

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

	objects = new_object_addresses();

	foreach(cell1, stmt->objects)
	{
		ObjectAddress	address;
		List	   *objname = lfirst(cell1);
		Relation	relation = NULL;
		Oid			namespaceId;

		/* Get an ObjectAddress for the object. */
		address = get_object_address(stmt->removeType,
									 objname, NIL,
									 &relation,
									 AccessExclusiveLock,
									 stmt->missing_ok);

		/* Issue NOTICE if supplied object was not found. */
		if (!OidIsValid(address.objectId))
		{
			does_not_exist_skipping(stmt->removeType, objname);
			continue;
		}

		/* Check permissions. */
		namespaceId = get_object_namespace(&address);
		if (!OidIsValid(namespaceId) ||
			!pg_namespace_ownercheck(namespaceId, GetUserId()))
			check_object_ownership(GetUserId(), stmt->removeType, address,
								   objname, NIL, relation);

		/* Release any relcache reference count, but keep lock until commit. */
		if (relation)
			heap_close(relation, NoLock);

		add_exact_object_address(&address, objects);
	}

	/* Here we really delete them. */
	performMultipleDeletions(objects, stmt->behavior);

	free_object_addresses(objects);
}

/*
 * Generate a NOTICE stating that the named object was not found, and is
 * being skipped.  This is only relevant when "IF EXISTS" is used; otherwise,
 * get_object_address() will throw an ERROR.
 */
static void
does_not_exist_skipping(ObjectType objtype, List *objname)
{
	const char *msg = NULL;
	char	   *name = NULL;

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
			name =  NameListToString(objname);
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
		default:
			elog(ERROR, "unexpected object type (%d)", (int)objtype);
			break;
	}

	ereport(NOTICE, (errmsg(msg, name)));
}
