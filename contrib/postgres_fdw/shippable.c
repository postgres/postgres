/*-------------------------------------------------------------------------
 *
 * shippable.c
 *	  Facility to track database objects shippable to a foreign server.
 *
 * Determine if functions and operators for non-built-in types/functions/ops
 * are shippable to the remote server.
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/postgres_fdw/shippable.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "postgres_fdw.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_depend.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/* Hash table for informations about remote objects we'll call */
static HTAB *ShippableCacheHash = NULL;

/* objid is the lookup key, must appear first */
typedef struct
{
	/* extension the object appears within, or InvalidOid if none */
	Oid	objid;
	Oid	classid;
} ShippableCacheKey;

typedef struct
{
	/* lookup key - must be first */
	ShippableCacheKey key;
	bool shippable;
} ShippableCacheEntry;

/*
 * Flush all cache entries when pg_foreign_server is updated.
 */
static void
InvalidateShippableCacheCallback(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS status;
	ShippableCacheEntry *entry;

	hash_seq_init(&status, ShippableCacheHash);
	while ((entry = (ShippableCacheEntry *) hash_seq_search(&status)) != NULL)
	{
		if (hash_search(ShippableCacheHash,
						(void *) &entry->key,
						HASH_REMOVE,
						NULL) == NULL)
			elog(ERROR, "hash table corrupted");
	}
}

/*
 * Initialize the cache of functions we can ship to remote server.
 */
static void
InitializeShippableCache(void)
{
	HASHCTL ctl;

	/* Initialize the hash table. */
	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(ShippableCacheKey);
	ctl.entrysize = sizeof(ShippableCacheEntry);
	ShippableCacheHash =
		hash_create("Shippable cache", 256, &ctl, HASH_ELEM);

	CacheRegisterSyscacheCallback(FOREIGNSERVEROID,
								  InvalidateShippableCacheCallback,
								  (Datum) 0);
}

/*
 * Returns true if given operator/function is part of an extension listed in
 * the server options.
 */
static bool
lookup_shippable(Oid objnumber, Oid classnumber, List *extension_list)
{
	static int nkeys = 2;
	ScanKeyData key[nkeys];
	HeapTuple tup;
	Relation depRel;
	SysScanDesc scan;
	bool is_shippable = false;

	/* Always return false if the user hasn't set the "extensions" option */
	if (extension_list == NIL)
		return false;

	depRel = heap_open(DependRelationId, RowExclusiveLock);

	/*
	 * Scan the system dependency table for all entries this object
	 * depends on, then iterate through and see if one of them
	 * is an extension declared by the user in the options
	 */
	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classnumber));

	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objnumber));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  GetCatalogSnapshot(depRel->rd_id), nkeys, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(tup);

		if (foundDep->deptype == DEPENDENCY_EXTENSION &&
			list_member_oid(extension_list, foundDep->refobjid))
		{
			is_shippable = true;
			break;
		}
	}

	systable_endscan(scan);
	relation_close(depRel, RowExclusiveLock);

	return is_shippable;
}

/*
 * is_shippable
 *     Is this object (proc/op/type) shippable to foreign server?
 *     Check cache first, then look-up whether (proc/op/type) is
 *     part of a declared extension if it is not cached.
 */
bool
is_shippable(Oid objnumber, Oid classnumber, List *extension_list)
{
	ShippableCacheKey key;
	ShippableCacheEntry *entry;

	/* Always return false if the user hasn't set the "extensions" option */
	if (extension_list == NIL)
		return false;

	/* Find existing cache, if any. */
	if (!ShippableCacheHash)
		InitializeShippableCache();

	/* Zero out the key */
	memset(&key, 0, sizeof(key));

	key.objid = objnumber;
	key.classid = classnumber;

	entry = (ShippableCacheEntry *)
				 hash_search(ShippableCacheHash,
					(void *) &key,
					HASH_FIND,
					NULL);

	/* Not found in ShippableCacheHash cache.  Construct new entry. */
	if (!entry)
	{
		/*
		 * Right now "shippability" is exclusively a function of whether
		 * the obj (proc/op/type) is in an extension declared by the user.
		 * In the future we could additionally have a whitelist of functions
		 * declared one at a time.
		 */
		bool shippable = lookup_shippable(objnumber, classnumber, extension_list);

		/*
		 * Don't create a new hash entry until *after* we have the shippable
		 * result in hand, as the shippable lookup might trigger a cache
		 * invalidation.
		 */
		entry = (ShippableCacheEntry *)
					 hash_search(ShippableCacheHash,
						(void *) &key,
						HASH_ENTER,
						NULL);

		entry->shippable = shippable;
	}

	if (!entry)
		return false;
	else
		return entry->shippable;
}
