/*-------------------------------------------------------------------------
 *
 * attoptcache.c
 *	  Attribute options cache management.
 *
 * Attribute options are cached separately from the fixed-size portion of
 * pg_attribute entries, which are handled by the relcache.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/attoptcache.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/reloptions.h"
#include "utils/attoptcache.h"
#include "utils/catcache.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/syscache.h"
#include "varatt.h"


/* Hash table for information about each attribute's options */
static HTAB *AttoptCacheHash = NULL;

/* attrelid and attnum form the lookup key, and must appear first */
typedef struct
{
	Oid			attrelid;
	int			attnum;
} AttoptCacheKey;

typedef struct
{
	AttoptCacheKey key;			/* lookup key - must be first */
	AttributeOpts *opts;		/* options, or NULL if none */
} AttoptCacheEntry;


/*
 * InvalidateAttoptCacheCallback
 *		Flush cache entry (or entries) when pg_attribute is updated.
 *
 * When pg_attribute is updated, we must flush the cache entry at least
 * for that attribute.
 */
static void
InvalidateAttoptCacheCallback(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS status;
	AttoptCacheEntry *attopt;

	/*
	 * By convention, zero hash value is passed to the callback as a sign that
	 * it's time to invalidate the whole cache. See sinval.c, inval.c and
	 * InvalidateSystemCachesExtended().
	 */
	if (hashvalue == 0)
		hash_seq_init(&status, AttoptCacheHash);
	else
		hash_seq_init_with_hash_value(&status, AttoptCacheHash, hashvalue);

	while ((attopt = (AttoptCacheEntry *) hash_seq_search(&status)) != NULL)
	{
		if (attopt->opts)
			pfree(attopt->opts);
		if (hash_search(AttoptCacheHash,
						&attopt->key,
						HASH_REMOVE,
						NULL) == NULL)
			elog(ERROR, "hash table corrupted");
	}
}

/*
 * Hash function compatible with two-arg system cache hash function.
 */
static uint32
relatt_cache_syshash(const void *key, Size keysize)
{
	const AttoptCacheKey *ckey = key;

	Assert(keysize == sizeof(*ckey));
	return GetSysCacheHashValue2(ATTNUM, ckey->attrelid, ckey->attnum);
}

/*
 * InitializeAttoptCache
 *		Initialize the attribute options cache.
 */
static void
InitializeAttoptCache(void)
{
	HASHCTL		ctl;

	/* Initialize the hash table. */
	ctl.keysize = sizeof(AttoptCacheKey);
	ctl.entrysize = sizeof(AttoptCacheEntry);

	/*
	 * AttoptCacheEntry takes hash value from the system cache. For
	 * AttoptCacheHash we use the same hash in order to speedup search by hash
	 * value. This is used by hash_seq_init_with_hash_value().
	 */
	ctl.hash = relatt_cache_syshash;

	AttoptCacheHash =
		hash_create("Attopt cache", 256, &ctl,
					HASH_ELEM | HASH_FUNCTION);

	/* Make sure we've initialized CacheMemoryContext. */
	if (!CacheMemoryContext)
		CreateCacheMemoryContext();

	/* Watch for invalidation events. */
	CacheRegisterSyscacheCallback(ATTNUM,
								  InvalidateAttoptCacheCallback,
								  (Datum) 0);
}

/*
 * get_attribute_options
 *		Fetch attribute options for a specified table OID.
 */
AttributeOpts *
get_attribute_options(Oid attrelid, int attnum)
{
	AttoptCacheKey key;
	AttoptCacheEntry *attopt;
	AttributeOpts *result;
	HeapTuple	tp;

	/* Find existing cache entry, if any. */
	if (!AttoptCacheHash)
		InitializeAttoptCache();
	memset(&key, 0, sizeof(key));	/* make sure any padding bits are unset */
	key.attrelid = attrelid;
	key.attnum = attnum;
	attopt =
		(AttoptCacheEntry *) hash_search(AttoptCacheHash,
										 &key,
										 HASH_FIND,
										 NULL);

	/* Not found in Attopt cache.  Construct new cache entry. */
	if (!attopt)
	{
		AttributeOpts *opts;

		tp = SearchSysCache2(ATTNUM,
							 ObjectIdGetDatum(attrelid),
							 Int16GetDatum(attnum));

		/*
		 * If we don't find a valid HeapTuple, it must mean someone has
		 * managed to request attribute details for a non-existent attribute.
		 * We treat that case as if no options were specified.
		 */
		if (!HeapTupleIsValid(tp))
			opts = NULL;
		else
		{
			Datum		datum;
			bool		isNull;

			datum = SysCacheGetAttr(ATTNUM,
									tp,
									Anum_pg_attribute_attoptions,
									&isNull);
			if (isNull)
				opts = NULL;
			else
			{
				bytea	   *bytea_opts = attribute_reloptions(datum, false);

				opts = MemoryContextAlloc(CacheMemoryContext,
										  VARSIZE(bytea_opts));
				memcpy(opts, bytea_opts, VARSIZE(bytea_opts));
			}
			ReleaseSysCache(tp);
		}

		/*
		 * It's important to create the actual cache entry only after reading
		 * pg_attribute, since the read could cause a cache flush.
		 */
		attopt = (AttoptCacheEntry *) hash_search(AttoptCacheHash,
												  &key,
												  HASH_ENTER,
												  NULL);
		attopt->opts = opts;
	}

	/* Return results in caller's memory context. */
	if (attopt->opts == NULL)
		return NULL;
	result = palloc(VARSIZE(attopt->opts));
	memcpy(result, attopt->opts, VARSIZE(attopt->opts));
	return result;
}
