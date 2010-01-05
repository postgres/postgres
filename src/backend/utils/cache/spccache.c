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
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/cache/spccache.c,v 1.1 2010/01/05 21:53:59 rhaas Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "access/reloptions.h"
#include "catalog/pg_tablespace.h"
#include "commands/tablespace.h"
#include "miscadmin.h"
#include "optimizer/cost.h"
#include "utils/catcache.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/spccache.h"
#include "utils/syscache.h"

static HTAB *TableSpaceCacheHash = NULL;

typedef struct {
	Oid			oid;
	TableSpaceOpts *opts;
} TableSpace;

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
InvalidateTableSpaceCacheCallback(Datum arg, int cacheid, ItemPointer tuplePtr)
{
	HASH_SEQ_STATUS status;
	TableSpace *spc;

	hash_seq_init(&status, TableSpaceCacheHash);
	while ((spc = (TableSpace *) hash_seq_search(&status)) != NULL)
	{
		if (hash_search(TableSpaceCacheHash, (void *) &spc->oid, HASH_REMOVE,
						NULL) == NULL)
			elog(ERROR, "hash table corrupted");
		if (spc->opts)
			pfree(spc->opts);
	}
}

/*
 * InitializeTableSpaceCache
 *		Initiate the tablespace cache.
 */
static void
InitializeTableSpaceCache(void)
{
	HASHCTL ctl;

	/* Initialize the hash table. */
	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(TableSpace);
	ctl.hash = tag_hash;
	TableSpaceCacheHash =
		hash_create("TableSpace cache", 16, &ctl,
				    HASH_ELEM | HASH_FUNCTION);

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
 *		Fetch TableSpace structure for a specified table OID.
 *
 * Pointers returned by this function should not be stored, since a cache
 * flush will invalidate them.
 */
static TableSpace *
get_tablespace(Oid spcid)
{
	HeapTuple	tp;
	TableSpace *spc;
	bool		found;

	/*
	 * Since spcid is always from a pg_class tuple, InvalidOid implies the
	 * default.
	 */
	if (spcid == InvalidOid)
		spcid = MyDatabaseTableSpace;

	/* Find existing cache entry, or create a new one. */
	if (!TableSpaceCacheHash)
		InitializeTableSpaceCache();
	spc = (TableSpace *) hash_search(TableSpaceCacheHash, (void *) &spcid,
									 HASH_ENTER, &found);
	if (found)
		return spc;

	/*
	 * Not found in TableSpace cache.  Check catcache.  If we don't find a
	 * valid HeapTuple, it must mean someone has managed to request tablespace
	 * details for a non-existent tablespace.  We'll just treat that case as if
	 * no options were specified.
	 */
	tp = SearchSysCache(TABLESPACEOID, ObjectIdGetDatum(spcid), 0, 0, 0);
	if (!HeapTupleIsValid(tp))
		spc->opts = NULL;
	else
	{
		Datum	datum;
		bool	isNull;
		MemoryContext octx;

		datum = SysCacheGetAttr(TABLESPACEOID,
								tp,
								Anum_pg_tablespace_spcoptions,
								&isNull);
		if (isNull)
			spc->opts = NULL;
		else
		{
			octx = MemoryContextSwitchTo(CacheMemoryContext);
			spc->opts = (TableSpaceOpts *) tablespace_reloptions(datum, false);
			MemoryContextSwitchTo(octx);
		}
		ReleaseSysCache(tp);
	}

	/* Update new TableSpace cache entry with results of option parsing. */
	return spc;
}

/*
 * get_tablespace_page_costs
 *		Return random and sequential page costs for a given tablespace.
 */
void
get_tablespace_page_costs(Oid spcid, double *spc_random_page_cost,
							   double *spc_seq_page_cost)
{
	TableSpace *spc = get_tablespace(spcid);

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
