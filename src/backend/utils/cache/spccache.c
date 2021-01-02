/*-------------------------------------------------------------------------
 *
 * spccache.c
 *	  Tablespace cache management.
 *
 * We cache the parsed version of spcoptions for each tablespace to avoid
 * needing to reparse on every lookup.  Right now, there doesn't appear to
 * be a measurable performance gain from doing this, but that might change
 * in the future as we add more options.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/spccache.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/reloptions.h"
#include "catalog/pg_tablespace.h"
#include "commands/tablespace.h"
#include "miscadmin.h"
#include "optimizer/optimizer.h"
#include "storage/bufmgr.h"
#include "utils/catcache.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/spccache.h"
#include "utils/syscache.h"


/* Hash table for information about each tablespace */
static HTAB *TableSpaceCacheHash = NULL;

typedef struct
{
	Oid			oid;			/* lookup key - must be first */
	TableSpaceOpts *opts;		/* options, or NULL if none */
} TableSpaceCacheEntry;


/*
 * InvalidateTableSpaceCacheCallback
 *		Flush all cache entries when pg_tablespace is updated.
 *
 * When pg_tablespace is updated, we must flush the cache entry at least
 * for that tablespace.  Currently, we just flush them all.  This is quick
 * and easy and doesn't cost much, since there shouldn't be terribly many
 * tablespaces, nor do we expect them to be frequently modified.
 */
static void
InvalidateTableSpaceCacheCallback(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS status;
	TableSpaceCacheEntry *spc;

	hash_seq_init(&status, TableSpaceCacheHash);
	while ((spc = (TableSpaceCacheEntry *) hash_seq_search(&status)) != NULL)
	{
		if (spc->opts)
			pfree(spc->opts);
		if (hash_search(TableSpaceCacheHash,
						(void *) &spc->oid,
						HASH_REMOVE,
						NULL) == NULL)
			elog(ERROR, "hash table corrupted");
	}
}

/*
 * InitializeTableSpaceCache
 *		Initialize the tablespace cache.
 */
static void
InitializeTableSpaceCache(void)
{
	HASHCTL		ctl;

	/* Initialize the hash table. */
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(TableSpaceCacheEntry);
	TableSpaceCacheHash =
		hash_create("TableSpace cache", 16, &ctl,
					HASH_ELEM | HASH_BLOBS);

	/* Make sure we've initialized CacheMemoryContext. */
	if (!CacheMemoryContext)
		CreateCacheMemoryContext();

	/* Watch for invalidation events. */
	CacheRegisterSyscacheCallback(TABLESPACEOID,
								  InvalidateTableSpaceCacheCallback,
								  (Datum) 0);
}

/*
 * get_tablespace
 *		Fetch TableSpaceCacheEntry structure for a specified table OID.
 *
 * Pointers returned by this function should not be stored, since a cache
 * flush will invalidate them.
 */
static TableSpaceCacheEntry *
get_tablespace(Oid spcid)
{
	TableSpaceCacheEntry *spc;
	HeapTuple	tp;
	TableSpaceOpts *opts;

	/*
	 * Since spcid is always from a pg_class tuple, InvalidOid implies the
	 * default.
	 */
	if (spcid == InvalidOid)
		spcid = MyDatabaseTableSpace;

	/* Find existing cache entry, if any. */
	if (!TableSpaceCacheHash)
		InitializeTableSpaceCache();
	spc = (TableSpaceCacheEntry *) hash_search(TableSpaceCacheHash,
											   (void *) &spcid,
											   HASH_FIND,
											   NULL);
	if (spc)
		return spc;

	/*
	 * Not found in TableSpace cache.  Check catcache.  If we don't find a
	 * valid HeapTuple, it must mean someone has managed to request tablespace
	 * details for a non-existent tablespace.  We'll just treat that case as
	 * if no options were specified.
	 */
	tp = SearchSysCache1(TABLESPACEOID, ObjectIdGetDatum(spcid));
	if (!HeapTupleIsValid(tp))
		opts = NULL;
	else
	{
		Datum		datum;
		bool		isNull;

		datum = SysCacheGetAttr(TABLESPACEOID,
								tp,
								Anum_pg_tablespace_spcoptions,
								&isNull);
		if (isNull)
			opts = NULL;
		else
		{
			bytea	   *bytea_opts = tablespace_reloptions(datum, false);

			opts = MemoryContextAlloc(CacheMemoryContext, VARSIZE(bytea_opts));
			memcpy(opts, bytea_opts, VARSIZE(bytea_opts));
		}
		ReleaseSysCache(tp);
	}

	/*
	 * Now create the cache entry.  It's important to do this only after
	 * reading the pg_tablespace entry, since doing so could cause a cache
	 * flush.
	 */
	spc = (TableSpaceCacheEntry *) hash_search(TableSpaceCacheHash,
											   (void *) &spcid,
											   HASH_ENTER,
											   NULL);
	spc->opts = opts;
	return spc;
}

/*
 * get_tablespace_page_costs
 *		Return random and/or sequential page costs for a given tablespace.
 *
 *		This value is not locked by the transaction, so this value may
 *		be changed while a SELECT that has used these values for planning
 *		is still executing.
 */
void
get_tablespace_page_costs(Oid spcid,
						  double *spc_random_page_cost,
						  double *spc_seq_page_cost)
{
	TableSpaceCacheEntry *spc = get_tablespace(spcid);

	Assert(spc != NULL);

	if (spc_random_page_cost)
	{
		if (!spc->opts || spc->opts->random_page_cost < 0)
			*spc_random_page_cost = random_page_cost;
		else
			*spc_random_page_cost = spc->opts->random_page_cost;
	}

	if (spc_seq_page_cost)
	{
		if (!spc->opts || spc->opts->seq_page_cost < 0)
			*spc_seq_page_cost = seq_page_cost;
		else
			*spc_seq_page_cost = spc->opts->seq_page_cost;
	}
}

/*
 * get_tablespace_io_concurrency
 *
 *		This value is not locked by the transaction, so this value may
 *		be changed while a SELECT that has used these values for planning
 *		is still executing.
 */
int
get_tablespace_io_concurrency(Oid spcid)
{
	TableSpaceCacheEntry *spc = get_tablespace(spcid);

	if (!spc->opts || spc->opts->effective_io_concurrency < 0)
		return effective_io_concurrency;
	else
		return spc->opts->effective_io_concurrency;
}

/*
 * get_tablespace_maintenance_io_concurrency
 */
int
get_tablespace_maintenance_io_concurrency(Oid spcid)
{
	TableSpaceCacheEntry *spc = get_tablespace(spcid);

	if (!spc->opts || spc->opts->maintenance_io_concurrency < 0)
		return maintenance_io_concurrency;
	else
		return spc->opts->maintenance_io_concurrency;
}
