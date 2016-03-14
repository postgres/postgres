/*-------------------------------------------------------------------------
 *
 * extensible.c
 *	  Support for extensible node types
 *
 * Loadable modules can define what are in effect new types of nodes using
 * the routines in this file.  All such nodes are flagged T_ExtensibleNode,
 * with the extnodename field distinguishing the specific type.  Use
 * RegisterExtensibleNodeMethods to register a new type of extensible node,
 * and GetExtensibleNodeMethods to get information about a previously
 * registered type of extensible node.
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/nodes/extensible.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/extensible.h"
#include "utils/hsearch.h"

static HTAB *extensible_node_methods = NULL;

typedef struct
{
	char		extnodename[EXTNODENAME_MAX_LEN];
	const ExtensibleNodeMethods *methods;
} ExtensibleNodeEntry;

/*
 * Register a new type of extensible node.
 */
void
RegisterExtensibleNodeMethods(const ExtensibleNodeMethods *methods)
{
	ExtensibleNodeEntry *entry;
	bool		found;

	if (extensible_node_methods == NULL)
	{
		HASHCTL		ctl;

		memset(&ctl, 0, sizeof(HASHCTL));
		ctl.keysize = EXTNODENAME_MAX_LEN;
		ctl.entrysize = sizeof(ExtensibleNodeEntry);
		extensible_node_methods = hash_create("Extensible Node Methods",
											  100, &ctl, HASH_ELEM);
	}

	if (strlen(methods->extnodename) >= EXTNODENAME_MAX_LEN)
		elog(ERROR, "extensible node name is too long");

	entry = (ExtensibleNodeEntry *) hash_search(extensible_node_methods,
												methods->extnodename,
												HASH_ENTER, &found);
	if (found)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("extensible node type \"%s\" already exists",
						methods->extnodename)));

	entry->methods = methods;
}

/*
 * Get the methods for a given type of extensible node.
 */
const ExtensibleNodeMethods *
GetExtensibleNodeMethods(const char *extnodename, bool missing_ok)
{
	ExtensibleNodeEntry *entry = NULL;

	if (extensible_node_methods != NULL)
		entry = (ExtensibleNodeEntry *) hash_search(extensible_node_methods,
													extnodename,
													HASH_FIND, NULL);

	if (!entry)
	{
		if (missing_ok)
			return NULL;
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("ExtensibleNodeMethods \"%s\" was not registered",
						extnodename)));
	}

	return entry->methods;
}
