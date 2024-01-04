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
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
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
static HTAB *custom_scan_methods = NULL;

typedef struct
{
	char		extnodename[EXTNODENAME_MAX_LEN];
	const void *extnodemethods;
} ExtensibleNodeEntry;

/*
 * An internal function to register a new callback structure
 */
static void
RegisterExtensibleNodeEntry(HTAB **p_htable, const char *htable_label,
							const char *extnodename,
							const void *extnodemethods)
{
	ExtensibleNodeEntry *entry;
	bool		found;

	if (*p_htable == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = EXTNODENAME_MAX_LEN;
		ctl.entrysize = sizeof(ExtensibleNodeEntry);

		*p_htable = hash_create(htable_label, 100, &ctl,
								HASH_ELEM | HASH_STRINGS);
	}

	if (strlen(extnodename) >= EXTNODENAME_MAX_LEN)
		elog(ERROR, "extensible node name is too long");

	entry = (ExtensibleNodeEntry *) hash_search(*p_htable,
												extnodename,
												HASH_ENTER, &found);
	if (found)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("extensible node type \"%s\" already exists",
						extnodename)));

	entry->extnodemethods = extnodemethods;
}

/*
 * Register a new type of extensible node.
 */
void
RegisterExtensibleNodeMethods(const ExtensibleNodeMethods *methods)
{
	RegisterExtensibleNodeEntry(&extensible_node_methods,
								"Extensible Node Methods",
								methods->extnodename,
								methods);
}

/*
 * Register a new type of custom scan node
 */
void
RegisterCustomScanMethods(const CustomScanMethods *methods)
{
	RegisterExtensibleNodeEntry(&custom_scan_methods,
								"Custom Scan Methods",
								methods->CustomName,
								methods);
}

/*
 * An internal routine to get an ExtensibleNodeEntry by the given identifier
 */
static const void *
GetExtensibleNodeEntry(HTAB *htable, const char *extnodename, bool missing_ok)
{
	ExtensibleNodeEntry *entry = NULL;

	if (htable != NULL)
		entry = (ExtensibleNodeEntry *) hash_search(htable,
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

	return entry->extnodemethods;
}

/*
 * Get the methods for a given type of extensible node.
 */
const ExtensibleNodeMethods *
GetExtensibleNodeMethods(const char *extnodename, bool missing_ok)
{
	return (const ExtensibleNodeMethods *)
		GetExtensibleNodeEntry(extensible_node_methods,
							   extnodename,
							   missing_ok);
}

/*
 * Get the methods for a given name of CustomScanMethods
 */
const CustomScanMethods *
GetCustomScanMethods(const char *CustomName, bool missing_ok)
{
	return (const CustomScanMethods *)
		GetExtensibleNodeEntry(custom_scan_methods,
							   CustomName,
							   missing_ok);
}
