/*-------------------------------------------------------------------------
 *
 * catcache.c
 *	  System catalog cache for tuples matching a key.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/cache/catcache.c,v 1.108 2003/08/04 02:40:06 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/hash.h"
#include "access/heapam.h"
#include "access/valid.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "miscadmin.h"
#ifdef CATCACHE_STATS
#include "storage/ipc.h"		/* for on_proc_exit */
#endif
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/catcache.h"
#include "utils/relcache.h"
#include "utils/syscache.h"


 /* #define CACHEDEBUG */	/* turns DEBUG elogs on */

/*
 * Constants related to size of the catcache.
 *
 * NCCBUCKETS must be a power of two and must be less than 64K (because
 * SharedInvalCatcacheMsg crams hash indexes into a uint16 field).	In
 * practice it should be a lot less, anyway, to avoid chewing up too much
 * space on hash bucket headers.
 *
 * MAXCCTUPLES could be as small as a few hundred, if per-backend memory
 * consumption is at a premium.
 */
#define NCCBUCKETS 256			/* Hash buckets per CatCache */
#define MAXCCTUPLES 5000		/* Maximum # of tuples in all caches */

/*
 * Given a hash value and the size of the hash table, find the bucket
 * in which the hash value belongs. Since the hash table must contain
 * a power-of-2 number of elements, this is a simple bitmask.
 */
#define HASH_INDEX(h, sz) ((Index) ((h) & ((sz) - 1)))


/*
 *		variables, macros and other stuff
 */

#ifdef CACHEDEBUG
#define CACHE1_elog(a,b)				elog(a,b)
#define CACHE2_elog(a,b,c)				elog(a,b,c)
#define CACHE3_elog(a,b,c,d)			elog(a,b,c,d)
#define CACHE4_elog(a,b,c,d,e)			elog(a,b,c,d,e)
#define CACHE5_elog(a,b,c,d,e,f)		elog(a,b,c,d,e,f)
#define CACHE6_elog(a,b,c,d,e,f,g)		elog(a,b,c,d,e,f,g)
#else
#define CACHE1_elog(a,b)
#define CACHE2_elog(a,b,c)
#define CACHE3_elog(a,b,c,d)
#define CACHE4_elog(a,b,c,d,e)
#define CACHE5_elog(a,b,c,d,e,f)
#define CACHE6_elog(a,b,c,d,e,f,g)
#endif

/* Cache management header --- pointer is NULL until created */
static CatCacheHeader *CacheHdr = NULL;


static uint32 CatalogCacheComputeHashValue(CatCache *cache, int nkeys,
							 ScanKey cur_skey);
static uint32 CatalogCacheComputeTupleHashValue(CatCache *cache,
								  HeapTuple tuple);

#ifdef CATCACHE_STATS
static void CatCachePrintStats(void);
#endif
static void CatCacheRemoveCTup(CatCache *cache, CatCTup *ct);
static void CatCacheRemoveCList(CatCache *cache, CatCList *cl);
static void CatalogCacheInitializeCache(CatCache *cache);
static CatCTup *CatalogCacheCreateEntry(CatCache *cache, HeapTuple ntp,
						uint32 hashValue, Index hashIndex,
						bool negative);
static HeapTuple build_dummy_tuple(CatCache *cache, int nkeys, ScanKey skeys);


/*
 *					internal support functions
 */

/*
 * Look up the hash and equality functions for system types that are used
 * as cache key fields.
 *
 * XXX this should be replaced by catalog lookups,
 * but that seems to pose considerable risk of circularity...
 */
static void
GetCCHashEqFuncs(Oid keytype, PGFunction *hashfunc, RegProcedure *eqfunc)
{
	switch (keytype)
	{
		case BOOLOID:
			*hashfunc = hashchar;
			*eqfunc = F_BOOLEQ;
			break;
		case CHAROID:
			*hashfunc = hashchar;
			*eqfunc = F_CHAREQ;
			break;
		case NAMEOID:
			*hashfunc = hashname;
			*eqfunc = F_NAMEEQ;
			break;
		case INT2OID:
			*hashfunc = hashint2;
			*eqfunc = F_INT2EQ;
			break;
		case INT2VECTOROID:
			*hashfunc = hashint2vector;
			*eqfunc = F_INT2VECTOREQ;
			break;
		case INT4OID:
			*hashfunc = hashint4;
			*eqfunc = F_INT4EQ;
			break;
		case TEXTOID:
			*hashfunc = hashtext;
			*eqfunc = F_TEXTEQ;
			break;
		case OIDOID:
		case REGPROCOID:
		case REGPROCEDUREOID:
		case REGOPEROID:
		case REGOPERATOROID:
		case REGCLASSOID:
		case REGTYPEOID:
			*hashfunc = hashoid;
			*eqfunc = F_OIDEQ;
			break;
		case OIDVECTOROID:
			*hashfunc = hashoidvector;
			*eqfunc = F_OIDVECTOREQ;
			break;
		default:
			elog(FATAL, "type %u not supported as catcache key", keytype);
			break;
	}
}

/*
 *		CatalogCacheComputeHashValue
 *
 * Compute the hash value associated with a given set of lookup keys
 */
static uint32
CatalogCacheComputeHashValue(CatCache *cache, int nkeys, ScanKey cur_skey)
{
	uint32		hashValue = 0;

	CACHE4_elog(DEBUG2, "CatalogCacheComputeHashValue %s %d %p",
				cache->cc_relname,
				nkeys,
				cache);

	switch (nkeys)
	{
		case 4:
			hashValue ^=
				DatumGetUInt32(DirectFunctionCall1(cache->cc_hashfunc[3],
										  cur_skey[3].sk_argument)) << 9;
			/* FALLTHROUGH */
		case 3:
			hashValue ^=
				DatumGetUInt32(DirectFunctionCall1(cache->cc_hashfunc[2],
										  cur_skey[2].sk_argument)) << 6;
			/* FALLTHROUGH */
		case 2:
			hashValue ^=
				DatumGetUInt32(DirectFunctionCall1(cache->cc_hashfunc[1],
										  cur_skey[1].sk_argument)) << 3;
			/* FALLTHROUGH */
		case 1:
			hashValue ^=
				DatumGetUInt32(DirectFunctionCall1(cache->cc_hashfunc[0],
											   cur_skey[0].sk_argument));
			break;
		default:
			elog(FATAL, "wrong number of hash keys: %d", nkeys);
			break;
	}

	return hashValue;
}

/*
 *		CatalogCacheComputeTupleHashValue
 *
 * Compute the hash value associated with a given tuple to be cached
 */
static uint32
CatalogCacheComputeTupleHashValue(CatCache *cache, HeapTuple tuple)
{
	ScanKeyData cur_skey[4];
	bool		isNull = false;

	/* Copy pre-initialized overhead data for scankey */
	memcpy(cur_skey, cache->cc_skey, sizeof(cur_skey));

	/* Now extract key fields from tuple, insert into scankey */
	switch (cache->cc_nkeys)
	{
		case 4:
			cur_skey[3].sk_argument =
				(cache->cc_key[3] == ObjectIdAttributeNumber)
				? ObjectIdGetDatum(HeapTupleGetOid(tuple))
				: fastgetattr(tuple,
							  cache->cc_key[3],
							  cache->cc_tupdesc,
							  &isNull);
			Assert(!isNull);
			/* FALLTHROUGH */
		case 3:
			cur_skey[2].sk_argument =
				(cache->cc_key[2] == ObjectIdAttributeNumber)
				? ObjectIdGetDatum(HeapTupleGetOid(tuple))
				: fastgetattr(tuple,
							  cache->cc_key[2],
							  cache->cc_tupdesc,
							  &isNull);
			Assert(!isNull);
			/* FALLTHROUGH */
		case 2:
			cur_skey[1].sk_argument =
				(cache->cc_key[1] == ObjectIdAttributeNumber)
				? ObjectIdGetDatum(HeapTupleGetOid(tuple))
				: fastgetattr(tuple,
							  cache->cc_key[1],
							  cache->cc_tupdesc,
							  &isNull);
			Assert(!isNull);
			/* FALLTHROUGH */
		case 1:
			cur_skey[0].sk_argument =
				(cache->cc_key[0] == ObjectIdAttributeNumber)
				? ObjectIdGetDatum(HeapTupleGetOid(tuple))
				: fastgetattr(tuple,
							  cache->cc_key[0],
							  cache->cc_tupdesc,
							  &isNull);
			Assert(!isNull);
			break;
		default:
			elog(FATAL, "wrong number of hash keys: %d", cache->cc_nkeys);
			break;
	}

	return CatalogCacheComputeHashValue(cache, cache->cc_nkeys, cur_skey);
}


#ifdef CATCACHE_STATS

static void
CatCachePrintStats(void)
{
	CatCache   *cache;
	long		cc_searches = 0;
	long		cc_hits = 0;
	long		cc_neg_hits = 0;
	long		cc_newloads = 0;
	long		cc_invals = 0;
	long		cc_discards = 0;
	long		cc_lsearches = 0;
	long		cc_lhits = 0;

	elog(DEBUG2, "catcache stats dump: %d/%d tuples in catcaches",
		 CacheHdr->ch_ntup, CacheHdr->ch_maxtup);

	for (cache = CacheHdr->ch_caches; cache; cache = cache->cc_next)
	{
		if (cache->cc_ntup == 0 && cache->cc_searches == 0)
			continue;			/* don't print unused caches */
		elog(DEBUG2, "catcache %s/%s: %d tup, %ld srch, %ld+%ld=%ld hits, %ld+%ld=%ld loads, %ld invals, %ld discards, %ld lsrch, %ld lhits",
			 cache->cc_relname,
			 cache->cc_indname,
			 cache->cc_ntup,
			 cache->cc_searches,
			 cache->cc_hits,
			 cache->cc_neg_hits,
			 cache->cc_hits + cache->cc_neg_hits,
			 cache->cc_newloads,
			 cache->cc_searches - cache->cc_hits - cache->cc_neg_hits - cache->cc_newloads,
			 cache->cc_searches - cache->cc_hits - cache->cc_neg_hits,
			 cache->cc_invals,
			 cache->cc_discards,
			 cache->cc_lsearches,
			 cache->cc_lhits);
		cc_searches += cache->cc_searches;
		cc_hits += cache->cc_hits;
		cc_neg_hits += cache->cc_neg_hits;
		cc_newloads += cache->cc_newloads;
		cc_invals += cache->cc_invals;
		cc_discards += cache->cc_discards;
		cc_lsearches += cache->cc_lsearches;
		cc_lhits += cache->cc_lhits;
	}
	elog(DEBUG2, "catcache totals: %d tup, %ld srch, %ld+%ld=%ld hits, %ld+%ld=%ld loads, %ld invals, %ld discards, %ld lsrch, %ld lhits",
		 CacheHdr->ch_ntup,
		 cc_searches,
		 cc_hits,
		 cc_neg_hits,
		 cc_hits + cc_neg_hits,
		 cc_newloads,
		 cc_searches - cc_hits - cc_neg_hits - cc_newloads,
		 cc_searches - cc_hits - cc_neg_hits,
		 cc_invals,
		 cc_discards,
		 cc_lsearches,
		 cc_lhits);
}
#endif   /* CATCACHE_STATS */


/*
 *		CatCacheRemoveCTup
 *
 * Unlink and delete the given cache entry
 *
 * NB: if it is a member of a CatCList, the CatCList is deleted too.
 */
static void
CatCacheRemoveCTup(CatCache *cache, CatCTup *ct)
{
	Assert(ct->refcount == 0);
	Assert(ct->my_cache == cache);

	if (ct->c_list)
		CatCacheRemoveCList(cache, ct->c_list);

	/* delink from linked lists */
	DLRemove(&ct->lrulist_elem);
	DLRemove(&ct->cache_elem);

	/* free associated tuple data */
	if (ct->tuple.t_data != NULL)
		pfree(ct->tuple.t_data);
	pfree(ct);

	--cache->cc_ntup;
	--CacheHdr->ch_ntup;
}

/*
 *		CatCacheRemoveCList
 *
 * Unlink and delete the given cache list entry
 */
static void
CatCacheRemoveCList(CatCache *cache, CatCList *cl)
{
	int			i;

	Assert(cl->refcount == 0);
	Assert(cl->my_cache == cache);

	/* delink from member tuples */
	for (i = cl->n_members; --i >= 0;)
	{
		CatCTup    *ct = cl->members[i];

		Assert(ct->c_list == cl);
		ct->c_list = NULL;
	}

	/* delink from linked list */
	DLRemove(&cl->cache_elem);

	/* free associated tuple data */
	if (cl->tuple.t_data != NULL)
		pfree(cl->tuple.t_data);
	pfree(cl);
}


/*
 *	CatalogCacheIdInvalidate
 *
 *	Invalidate entries in the specified cache, given a hash value and
 *	item pointer.  Positive entries are deleted if they match the item
 *	pointer.  Negative entries must be deleted if they match the hash
 *	value (since we do not have the exact key of the tuple that's being
 *	inserted).	But this should only rarely result in loss of a cache
 *	entry that could have been kept.
 *
 *	Note that it's not very relevant whether the tuple identified by
 *	the item pointer is being inserted or deleted.	We don't expect to
 *	find matching positive entries in the one case, and we don't expect
 *	to find matching negative entries in the other; but we will do the
 *	right things in any case.
 *
 *	This routine is only quasi-public: it should only be used by inval.c.
 */
void
CatalogCacheIdInvalidate(int cacheId,
						 uint32 hashValue,
						 ItemPointer pointer)
{
	CatCache   *ccp;

	/*
	 * sanity checks
	 */
	Assert(ItemPointerIsValid(pointer));
	CACHE1_elog(DEBUG2, "CatalogCacheIdInvalidate: called");

	/*
	 * inspect caches to find the proper cache
	 */
	for (ccp = CacheHdr->ch_caches; ccp; ccp = ccp->cc_next)
	{
		Index		hashIndex;
		Dlelem	   *elt,
				   *nextelt;

		if (cacheId != ccp->id)
			continue;

		/*
		 * We don't bother to check whether the cache has finished
		 * initialization yet; if not, there will be no entries in it so
		 * no problem.
		 */

		/*
		 * Invalidate *all* CatCLists in this cache; it's too hard to tell
		 * which searches might still be correct, so just zap 'em all.
		 */
		for (elt = DLGetHead(&ccp->cc_lists); elt; elt = nextelt)
		{
			CatCList   *cl = (CatCList *) DLE_VAL(elt);

			nextelt = DLGetSucc(elt);

			if (cl->refcount > 0)
				cl->dead = true;
			else
				CatCacheRemoveCList(ccp, cl);
		}

		/*
		 * inspect the proper hash bucket for tuple matches
		 */
		hashIndex = HASH_INDEX(hashValue, ccp->cc_nbuckets);

		for (elt = DLGetHead(&ccp->cc_bucket[hashIndex]); elt; elt = nextelt)
		{
			CatCTup    *ct = (CatCTup *) DLE_VAL(elt);

			nextelt = DLGetSucc(elt);

			if (hashValue != ct->hash_value)
				continue;		/* ignore non-matching hash values */

			if (ct->negative ||
				ItemPointerEquals(pointer, &ct->tuple.t_self))
			{
				if (ct->refcount > 0)
					ct->dead = true;
				else
					CatCacheRemoveCTup(ccp, ct);
				CACHE1_elog(DEBUG2, "CatalogCacheIdInvalidate: invalidated");
#ifdef CATCACHE_STATS
				ccp->cc_invals++;
#endif
				/* could be multiple matches, so keep looking! */
			}
		}
		break;					/* need only search this one cache */
	}
}

/* ----------------------------------------------------------------
 *					   public functions
 * ----------------------------------------------------------------
 */


/*
 * Standard routine for creating cache context if it doesn't exist yet
 *
 * There are a lot of places (probably far more than necessary) that check
 * whether CacheMemoryContext exists yet and want to create it if not.
 * We centralize knowledge of exactly how to create it here.
 */
void
CreateCacheMemoryContext(void)
{
	/*
	 * Purely for paranoia, check that context doesn't exist; caller
	 * probably did so already.
	 */
	if (!CacheMemoryContext)
		CacheMemoryContext = AllocSetContextCreate(TopMemoryContext,
												   "CacheMemoryContext",
												ALLOCSET_DEFAULT_MINSIZE,
											   ALLOCSET_DEFAULT_INITSIZE,
											   ALLOCSET_DEFAULT_MAXSIZE);
}


/*
 *		AtEOXact_CatCache
 *
 * Clean up catcaches at end of transaction (either commit or abort)
 *
 * We scan the caches to reset refcounts to zero.  This is of course
 * necessary in the abort case, since elog() may have interrupted routines.
 * In the commit case, any nonzero counts indicate failure to call
 * ReleaseSysCache, so we put out a notice for debugging purposes.
 */
void
AtEOXact_CatCache(bool isCommit)
{
	CatCache   *ccp;
	Dlelem	   *elt,
			   *nextelt;

	/*
	 * First clean up CatCLists
	 */
	for (ccp = CacheHdr->ch_caches; ccp; ccp = ccp->cc_next)
	{
		for (elt = DLGetHead(&ccp->cc_lists); elt; elt = nextelt)
		{
			CatCList   *cl = (CatCList *) DLE_VAL(elt);

			nextelt = DLGetSucc(elt);

			if (cl->refcount != 0)
			{
				if (isCommit)
					elog(WARNING, "cache reference leak: cache %s (%d), list %p has count %d",
						 ccp->cc_relname, ccp->id, cl, cl->refcount);
				cl->refcount = 0;
			}

			/* Clean up any now-deletable dead entries */
			if (cl->dead)
				CatCacheRemoveCList(ccp, cl);
		}
	}

	/*
	 * Now clean up tuples; we can scan them all using the global LRU list
	 */
	for (elt = DLGetHead(&CacheHdr->ch_lrulist); elt; elt = nextelt)
	{
		CatCTup    *ct = (CatCTup *) DLE_VAL(elt);

		nextelt = DLGetSucc(elt);

		if (ct->refcount != 0)
		{
			if (isCommit)
				elog(WARNING, "cache reference leak: cache %s (%d), tuple %u has count %d",
					 ct->my_cache->cc_relname, ct->my_cache->id,
					 HeapTupleGetOid(&ct->tuple),
					 ct->refcount);
			ct->refcount = 0;
		}

		/* Clean up any now-deletable dead entries */
		if (ct->dead)
			CatCacheRemoveCTup(ct->my_cache, ct);
	}
}

/*
 *		ResetCatalogCache
 *
 * Reset one catalog cache to empty.
 *
 * This is not very efficient if the target cache is nearly empty.
 * However, it shouldn't need to be efficient; we don't invoke it often.
 */
static void
ResetCatalogCache(CatCache *cache)
{
	Dlelem	   *elt,
			   *nextelt;
	int			i;

	/* Remove each list in this cache, or at least mark it dead */
	for (elt = DLGetHead(&cache->cc_lists); elt; elt = nextelt)
	{
		CatCList   *cl = (CatCList *) DLE_VAL(elt);

		nextelt = DLGetSucc(elt);

		if (cl->refcount > 0)
			cl->dead = true;
		else
			CatCacheRemoveCList(cache, cl);
	}

	/* Remove each tuple in this cache, or at least mark it dead */
	for (i = 0; i < cache->cc_nbuckets; i++)
	{
		for (elt = DLGetHead(&cache->cc_bucket[i]); elt; elt = nextelt)
		{
			CatCTup    *ct = (CatCTup *) DLE_VAL(elt);

			nextelt = DLGetSucc(elt);

			if (ct->refcount > 0)
				ct->dead = true;
			else
				CatCacheRemoveCTup(cache, ct);
#ifdef CATCACHE_STATS
			cache->cc_invals++;
#endif
		}
	}
}

/*
 *		ResetCatalogCaches
 *
 * Reset all caches when a shared cache inval event forces it
 */
void
ResetCatalogCaches(void)
{
	CatCache   *cache;

	CACHE1_elog(DEBUG2, "ResetCatalogCaches called");

	for (cache = CacheHdr->ch_caches; cache; cache = cache->cc_next)
		ResetCatalogCache(cache);

	CACHE1_elog(DEBUG2, "end of ResetCatalogCaches call");
}

/*
 *		CatalogCacheFlushRelation
 *
 *	This is called by RelationFlushRelation() to clear out cached information
 *	about a relation being dropped.  (This could be a DROP TABLE command,
 *	or a temp table being dropped at end of transaction, or a table created
 *	during the current transaction that is being dropped because of abort.)
 *	Remove all cache entries relevant to the specified relation OID.
 *
 *	A special case occurs when relId is itself one of the cacheable system
 *	tables --- although those'll never be dropped, they can get flushed from
 *	the relcache (VACUUM causes this, for example).  In that case we need
 *	to flush all cache entries that came from that table.  (At one point we
 *	also tried to force re-execution of CatalogCacheInitializeCache for
 *	the cache(s) on that table.  This is a bad idea since it leads to all
 *	kinds of trouble if a cache flush occurs while loading cache entries.
 *	We now avoid the need to do it by copying cc_tupdesc out of the relcache,
 *	rather than relying on the relcache to keep a tupdesc for us.  Of course
 *	this assumes the tupdesc of a cachable system table will not change...)
 */
void
CatalogCacheFlushRelation(Oid relId)
{
	CatCache   *cache;

	CACHE2_elog(DEBUG2, "CatalogCacheFlushRelation called for %u", relId);

	for (cache = CacheHdr->ch_caches; cache; cache = cache->cc_next)
	{
		int			i;

		/* We can ignore uninitialized caches, since they must be empty */
		if (cache->cc_tupdesc == NULL)
			continue;

		/* Does this cache store tuples of the target relation itself? */
		if (cache->cc_tupdesc->attrs[0]->attrelid == relId)
		{
			/* Yes, so flush all its contents */
			ResetCatalogCache(cache);
			continue;
		}

		/* Does this cache store tuples associated with relations at all? */
		if (cache->cc_reloidattr == 0)
			continue;			/* nope, leave it alone */

		/* Yes, scan the tuples and remove those related to relId */
		for (i = 0; i < cache->cc_nbuckets; i++)
		{
			Dlelem	   *elt,
					   *nextelt;

			for (elt = DLGetHead(&cache->cc_bucket[i]); elt; elt = nextelt)
			{
				CatCTup    *ct = (CatCTup *) DLE_VAL(elt);
				Oid			tupRelid;

				nextelt = DLGetSucc(elt);

				/*
				 * Negative entries are never considered related to a rel,
				 * even if the rel is part of their lookup key.
				 */
				if (ct->negative)
					continue;

				if (cache->cc_reloidattr == ObjectIdAttributeNumber)
					tupRelid = HeapTupleGetOid(&ct->tuple);
				else
				{
					bool		isNull;

					tupRelid =
						DatumGetObjectId(fastgetattr(&ct->tuple,
													 cache->cc_reloidattr,
													 cache->cc_tupdesc,
													 &isNull));
					Assert(!isNull);
				}

				if (tupRelid == relId)
				{
					if (ct->refcount > 0)
						ct->dead = true;
					else
						CatCacheRemoveCTup(cache, ct);
#ifdef CATCACHE_STATS
					cache->cc_invals++;
#endif
				}
			}
		}
	}

	CACHE1_elog(DEBUG2, "end of CatalogCacheFlushRelation call");
}

/*
 *		InitCatCache
 *
 *	This allocates and initializes a cache for a system catalog relation.
 *	Actually, the cache is only partially initialized to avoid opening the
 *	relation.  The relation will be opened and the rest of the cache
 *	structure initialized on the first access.
 */
#ifdef CACHEDEBUG
#define InitCatCache_DEBUG2 \
do { \
	elog(DEBUG2, "InitCatCache: rel=%s id=%d nkeys=%d size=%d", \
		cp->cc_relname, cp->id, cp->cc_nkeys, cp->cc_nbuckets); \
} while(0)

#else
#define InitCatCache_DEBUG2
#endif

CatCache *
InitCatCache(int id,
			 const char *relname,
			 const char *indname,
			 int reloidattr,
			 int nkeys,
			 const int *key)
{
	CatCache   *cp;
	MemoryContext oldcxt;
	int			i;

	/*
	 * first switch to the cache context so our allocations do not vanish
	 * at the end of a transaction
	 */
	if (!CacheMemoryContext)
		CreateCacheMemoryContext();

	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	/*
	 * if first time through, initialize the cache group header, including
	 * global LRU list header
	 */
	if (CacheHdr == NULL)
	{
		CacheHdr = (CatCacheHeader *) palloc(sizeof(CatCacheHeader));
		CacheHdr->ch_caches = NULL;
		CacheHdr->ch_ntup = 0;
		CacheHdr->ch_maxtup = MAXCCTUPLES;
		DLInitList(&CacheHdr->ch_lrulist);
#ifdef CATCACHE_STATS
		on_proc_exit(CatCachePrintStats, 0);
#endif
	}

	/*
	 * allocate a new cache structure
	 *
	 * Note: we assume zeroing initializes the Dllist headers correctly
	 */
	cp = (CatCache *) palloc0(sizeof(CatCache) + NCCBUCKETS * sizeof(Dllist));

	/*
	 * initialize the cache's relation information for the relation
	 * corresponding to this cache, and initialize some of the new cache's
	 * other internal fields.  But don't open the relation yet.
	 */
	cp->id = id;
	cp->cc_relname = relname;
	cp->cc_indname = indname;
	cp->cc_reloid = InvalidOid; /* temporary */
	cp->cc_relisshared = false; /* temporary */
	cp->cc_tupdesc = (TupleDesc) NULL;
	cp->cc_reloidattr = reloidattr;
	cp->cc_ntup = 0;
	cp->cc_nbuckets = NCCBUCKETS;
	cp->cc_nkeys = nkeys;
	for (i = 0; i < nkeys; ++i)
		cp->cc_key[i] = key[i];

	/*
	 * new cache is initialized as far as we can go for now. print some
	 * debugging information, if appropriate.
	 */
	InitCatCache_DEBUG2;

	/*
	 * add completed cache to top of group header's list
	 */
	cp->cc_next = CacheHdr->ch_caches;
	CacheHdr->ch_caches = cp;

	/*
	 * back to the old context before we return...
	 */
	MemoryContextSwitchTo(oldcxt);

	return cp;
}

/*
 *		CatalogCacheInitializeCache
 *
 * This function does final initialization of a catcache: obtain the tuple
 * descriptor and set up the hash and equality function links.	We assume
 * that the relcache entry can be opened at this point!
 */
#ifdef CACHEDEBUG
#define CatalogCacheInitializeCache_DEBUG2 \
	elog(DEBUG2, "CatalogCacheInitializeCache: cache @%p %s", cache, \
		 cache->cc_relname)

#define CatalogCacheInitializeCache_DEBUG2 \
do { \
		if (cache->cc_key[i] > 0) { \
			elog(DEBUG2, "CatalogCacheInitializeCache: load %d/%d w/%d, %u", \
				i+1, cache->cc_nkeys, cache->cc_key[i], \
				 tupdesc->attrs[cache->cc_key[i] - 1]->atttypid); \
		} else { \
			elog(DEBUG2, "CatalogCacheInitializeCache: load %d/%d w/%d", \
				i+1, cache->cc_nkeys, cache->cc_key[i]); \
		} \
} while(0)

#else
#define CatalogCacheInitializeCache_DEBUG2
#define CatalogCacheInitializeCache_DEBUG2
#endif

static void
CatalogCacheInitializeCache(CatCache *cache)
{
	Relation	relation;
	MemoryContext oldcxt;
	TupleDesc	tupdesc;
	int			i;

	CatalogCacheInitializeCache_DEBUG2;

	/*
	 * Open the relation without locking --- we only need the tupdesc,
	 * which we assume will never change ...
	 */
	relation = heap_openr(cache->cc_relname, NoLock);
	Assert(RelationIsValid(relation));

	/*
	 * switch to the cache context so our allocations do not vanish at the
	 * end of a transaction
	 */
	Assert(CacheMemoryContext != NULL);

	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	/*
	 * copy the relcache's tuple descriptor to permanent cache storage
	 */
	tupdesc = CreateTupleDescCopyConstr(RelationGetDescr(relation));

	/*
	 * get the relation's OID and relisshared flag, too
	 */
	cache->cc_reloid = RelationGetRelid(relation);
	cache->cc_relisshared = RelationGetForm(relation)->relisshared;

	/*
	 * return to the caller's memory context and close the rel
	 */
	MemoryContextSwitchTo(oldcxt);

	heap_close(relation, NoLock);

	CACHE3_elog(DEBUG2, "CatalogCacheInitializeCache: %s, %d keys",
				cache->cc_relname, cache->cc_nkeys);

	/*
	 * initialize cache's key information
	 */
	for (i = 0; i < cache->cc_nkeys; ++i)
	{
		Oid			keytype;

		CatalogCacheInitializeCache_DEBUG2;

		if (cache->cc_key[i] > 0)
			keytype = tupdesc->attrs[cache->cc_key[i] - 1]->atttypid;
		else
		{
			if (cache->cc_key[i] != ObjectIdAttributeNumber)
				elog(FATAL, "only sys attr supported in caches is OID");
			keytype = OIDOID;
		}

		GetCCHashEqFuncs(keytype,
						 &cache->cc_hashfunc[i],
						 &cache->cc_skey[i].sk_procedure);

		cache->cc_isname[i] = (keytype == NAMEOID);

		/*
		 * Do equality-function lookup (we assume this won't need a
		 * catalog lookup for any supported type)
		 */
		fmgr_info_cxt(cache->cc_skey[i].sk_procedure,
					  &cache->cc_skey[i].sk_func,
					  CacheMemoryContext);

		/* Initialize sk_attno suitably for HeapKeyTest() and heap scans */
		cache->cc_skey[i].sk_attno = cache->cc_key[i];

		CACHE4_elog(DEBUG2, "CatalogCacheInit %s %d %p",
					cache->cc_relname,
					i,
					cache);
	}

	/*
	 * mark this cache fully initialized
	 */
	cache->cc_tupdesc = tupdesc;
}

/*
 * InitCatCachePhase2 -- external interface for CatalogCacheInitializeCache
 *
 * The only reason to call this routine is to ensure that the relcache
 * has created entries for all the catalogs and indexes referenced by
 * catcaches.  Therefore, open the index too.  An exception is the indexes
 * on pg_am, which we don't use (cf. IndexScanOK).
 */
void
InitCatCachePhase2(CatCache *cache)
{
	if (cache->cc_tupdesc == NULL)
		CatalogCacheInitializeCache(cache);

	if (cache->id != AMOID &&
		cache->id != AMNAME)
	{
		Relation	idesc;

		idesc = index_openr(cache->cc_indname);
		index_close(idesc);
	}
}


/*
 *		IndexScanOK
 *
 *		This function checks for tuples that will be fetched by
 *		IndexSupportInitialize() during relcache initialization for
 *		certain system indexes that support critical syscaches.
 *		We can't use an indexscan to fetch these, else we'll get into
 *		infinite recursion.  A plain heap scan will work, however.
 *
 *		Once we have completed relcache initialization (signaled by
 *		criticalRelcachesBuilt), we don't have to worry anymore.
 */
static bool
IndexScanOK(CatCache *cache, ScanKey cur_skey)
{
	if (cache->id == INDEXRELID)
	{
		/*
		 * Since the OIDs of indexes aren't hardwired, it's painful to
		 * figure out which is which.  Just force all pg_index searches to
		 * be heap scans while building the relcaches.
		 */
		if (!criticalRelcachesBuilt)
			return false;
	}
	else if (cache->id == AMOID ||
			 cache->id == AMNAME)
	{
		/*
		 * Always do heap scans in pg_am, because it's so small there's
		 * not much point in an indexscan anyway.  We *must* do this when
		 * initially building critical relcache entries, but we might as
		 * well just always do it.
		 */
		return false;
	}
	else if (cache->id == OPEROID)
	{
		if (!criticalRelcachesBuilt)
		{
			/* Looking for an OID comparison function? */
			Oid			lookup_oid = DatumGetObjectId(cur_skey[0].sk_argument);

			if (lookup_oid >= MIN_OIDCMP && lookup_oid <= MAX_OIDCMP)
				return false;
		}
	}

	/* Normal case, allow index scan */
	return true;
}

/*
 *	SearchCatCache
 *
 *		This call searches a system cache for a tuple, opening the relation
 *		if necessary (on the first access to a particular cache).
 *
 *		The result is NULL if not found, or a pointer to a HeapTuple in
 *		the cache.	The caller must not modify the tuple, and must call
 *		ReleaseCatCache() when done with it.
 *
 * The search key values should be expressed as Datums of the key columns'
 * datatype(s).  (Pass zeroes for any unused parameters.)  As a special
 * exception, the passed-in key for a NAME column can be just a C string;
 * the caller need not go to the trouble of converting it to a fully
 * null-padded NAME.
 */
HeapTuple
SearchCatCache(CatCache *cache,
			   Datum v1,
			   Datum v2,
			   Datum v3,
			   Datum v4)
{
	ScanKeyData cur_skey[4];
	uint32		hashValue;
	Index		hashIndex;
	Dlelem	   *elt;
	CatCTup    *ct;
	Relation	relation;
	SysScanDesc scandesc;
	HeapTuple	ntp;

	/*
	 * one-time startup overhead for each cache
	 */
	if (cache->cc_tupdesc == NULL)
		CatalogCacheInitializeCache(cache);

#ifdef CATCACHE_STATS
	cache->cc_searches++;
#endif

	/*
	 * initialize the search key information
	 */
	memcpy(cur_skey, cache->cc_skey, sizeof(cur_skey));
	cur_skey[0].sk_argument = v1;
	cur_skey[1].sk_argument = v2;
	cur_skey[2].sk_argument = v3;
	cur_skey[3].sk_argument = v4;

	/*
	 * find the hash bucket in which to look for the tuple
	 */
	hashValue = CatalogCacheComputeHashValue(cache, cache->cc_nkeys, cur_skey);
	hashIndex = HASH_INDEX(hashValue, cache->cc_nbuckets);

	/*
	 * scan the hash bucket until we find a match or exhaust our tuples
	 */
	for (elt = DLGetHead(&cache->cc_bucket[hashIndex]);
		 elt;
		 elt = DLGetSucc(elt))
	{
		bool		res;

		ct = (CatCTup *) DLE_VAL(elt);

		if (ct->dead)
			continue;			/* ignore dead entries */

		if (ct->hash_value != hashValue)
			continue;			/* quickly skip entry if wrong hash val */

		/*
		 * see if the cached tuple matches our key.
		 */
		HeapKeyTest(&ct->tuple,
					cache->cc_tupdesc,
					cache->cc_nkeys,
					cur_skey,
					res);
		if (!res)
			continue;

		/*
		 * we found a match in the cache: move it to the front of the
		 * global LRU list.  We also move it to the front of the list for
		 * its hashbucket, in order to speed subsequent searches.  (The
		 * most frequently accessed elements in any hashbucket will tend
		 * to be near the front of the hashbucket's list.)
		 */
		DLMoveToFront(&ct->lrulist_elem);
		DLMoveToFront(&ct->cache_elem);

		/*
		 * If it's a positive entry, bump its refcount and return it. If
		 * it's negative, we can report failure to the caller.
		 */
		if (!ct->negative)
		{
			ct->refcount++;

			CACHE3_elog(DEBUG2, "SearchCatCache(%s): found in bucket %d",
						cache->cc_relname, hashIndex);

#ifdef CATCACHE_STATS
			cache->cc_hits++;
#endif

			return &ct->tuple;
		}
		else
		{
			CACHE3_elog(DEBUG2, "SearchCatCache(%s): found neg entry in bucket %d",
						cache->cc_relname, hashIndex);

#ifdef CATCACHE_STATS
			cache->cc_neg_hits++;
#endif

			return NULL;
		}
	}

	/*
	 * Tuple was not found in cache, so we have to try to retrieve it
	 * directly from the relation.	If found, we will add it to the cache;
	 * if not found, we will add a negative cache entry instead.
	 *
	 * NOTE: it is possible for recursive cache lookups to occur while
	 * reading the relation --- for example, due to shared-cache-inval
	 * messages being processed during heap_open().  This is OK.  It's
	 * even possible for one of those lookups to find and enter the very
	 * same tuple we are trying to fetch here.	If that happens, we will
	 * enter a second copy of the tuple into the cache.  The first copy
	 * will never be referenced again, and will eventually age out of the
	 * cache, so there's no functional problem.  This case is rare enough
	 * that it's not worth expending extra cycles to detect.
	 */
	relation = heap_open(cache->cc_reloid, AccessShareLock);

	scandesc = systable_beginscan(relation,
								  cache->cc_indname,
								  IndexScanOK(cache, cur_skey),
								  SnapshotNow,
								  cache->cc_nkeys,
								  cur_skey);

	ct = NULL;

	while (HeapTupleIsValid(ntp = systable_getnext(scandesc)))
	{
		ct = CatalogCacheCreateEntry(cache, ntp,
									 hashValue, hashIndex,
									 false);
		break;					/* assume only one match */
	}

	systable_endscan(scandesc);

	heap_close(relation, AccessShareLock);

	/*
	 * If tuple was not found, we need to build a negative cache entry
	 * containing a fake tuple.  The fake tuple has the correct key
	 * columns, but nulls everywhere else.
	 */
	if (ct == NULL)
	{
		ntp = build_dummy_tuple(cache, cache->cc_nkeys, cur_skey);
		ct = CatalogCacheCreateEntry(cache, ntp,
									 hashValue, hashIndex,
									 true);
		heap_freetuple(ntp);

		CACHE4_elog(DEBUG2, "SearchCatCache(%s): Contains %d/%d tuples",
					cache->cc_relname, cache->cc_ntup, CacheHdr->ch_ntup);
		CACHE3_elog(DEBUG2, "SearchCatCache(%s): put neg entry in bucket %d",
					cache->cc_relname, hashIndex);

		/*
		 * We are not returning the new entry to the caller, so reset its
		 * refcount.
		 */
		ct->refcount = 0;		/* negative entries never have refs */

		return NULL;
	}

	CACHE4_elog(DEBUG2, "SearchCatCache(%s): Contains %d/%d tuples",
				cache->cc_relname, cache->cc_ntup, CacheHdr->ch_ntup);
	CACHE3_elog(DEBUG2, "SearchCatCache(%s): put in bucket %d",
				cache->cc_relname, hashIndex);

#ifdef CATCACHE_STATS
	cache->cc_newloads++;
#endif

	return &ct->tuple;
}

/*
 *	ReleaseCatCache
 *
 *	Decrement the reference count of a catcache entry (releasing the
 *	hold grabbed by a successful SearchCatCache).
 *
 *	NOTE: if compiled with -DCATCACHE_FORCE_RELEASE then catcache entries
 *	will be freed as soon as their refcount goes to zero.  In combination
 *	with aset.c's CLOBBER_FREED_MEMORY option, this provides a good test
 *	to catch references to already-released catcache entries.
 */
void
ReleaseCatCache(HeapTuple tuple)
{
	CatCTup    *ct = (CatCTup *) (((char *) tuple) -
								  offsetof(CatCTup, tuple));

	/* Safety checks to ensure we were handed a cache entry */
	Assert(ct->ct_magic == CT_MAGIC);
	Assert(ct->refcount > 0);

	ct->refcount--;

	if (ct->refcount == 0
#ifndef CATCACHE_FORCE_RELEASE
		&& ct->dead
#endif
		)
		CatCacheRemoveCTup(ct->my_cache, ct);
}


/*
 *	SearchCatCacheList
 *
 *		Generate a list of all tuples matching a partial key (that is,
 *		a key specifying just the first K of the cache's N key columns).
 *
 *		The caller must not modify the list object or the pointed-to tuples,
 *		and must call ReleaseCatCacheList() when done with the list.
 */
CatCList *
SearchCatCacheList(CatCache *cache,
				   int nkeys,
				   Datum v1,
				   Datum v2,
				   Datum v3,
				   Datum v4)
{
	ScanKeyData cur_skey[4];
	uint32		lHashValue;
	Dlelem	   *elt;
	CatCList   *cl;
	CatCTup    *ct;
	List	   *ctlist;
	int			nmembers;
	Relation	relation;
	SysScanDesc scandesc;
	bool		ordered;
	HeapTuple	ntp;
	MemoryContext oldcxt;
	int			i;

	/*
	 * one-time startup overhead for each cache
	 */
	if (cache->cc_tupdesc == NULL)
		CatalogCacheInitializeCache(cache);

	Assert(nkeys > 0 && nkeys < cache->cc_nkeys);

#ifdef CATCACHE_STATS
	cache->cc_lsearches++;
#endif

	/*
	 * initialize the search key information
	 */
	memcpy(cur_skey, cache->cc_skey, sizeof(cur_skey));
	cur_skey[0].sk_argument = v1;
	cur_skey[1].sk_argument = v2;
	cur_skey[2].sk_argument = v3;
	cur_skey[3].sk_argument = v4;

	/*
	 * compute a hash value of the given keys for faster search.  We don't
	 * presently divide the CatCList items into buckets, but this still
	 * lets us skip non-matching items quickly most of the time.
	 */
	lHashValue = CatalogCacheComputeHashValue(cache, nkeys, cur_skey);

	/*
	 * scan the items until we find a match or exhaust our list
	 */
	for (elt = DLGetHead(&cache->cc_lists);
		 elt;
		 elt = DLGetSucc(elt))
	{
		bool		res;

		cl = (CatCList *) DLE_VAL(elt);

		if (cl->dead)
			continue;			/* ignore dead entries */

		if (cl->hash_value != lHashValue)
			continue;			/* quickly skip entry if wrong hash val */

		/*
		 * see if the cached list matches our key.
		 */
		if (cl->nkeys != nkeys)
			continue;
		HeapKeyTest(&cl->tuple,
					cache->cc_tupdesc,
					nkeys,
					cur_skey,
					res);
		if (!res)
			continue;

		/*
		 * we found a matching list: move each of its members to the front
		 * of the global LRU list.	Also move the list itself to the front
		 * of the cache's list-of-lists, to speed subsequent searches. (We
		 * do not move the members to the fronts of their hashbucket
		 * lists, however, since there's no point in that unless they are
		 * searched for individually.)	Also bump the members' refcounts.
		 */
		for (i = 0; i < cl->n_members; i++)
		{
			cl->members[i]->refcount++;
			DLMoveToFront(&cl->members[i]->lrulist_elem);
		}
		DLMoveToFront(&cl->cache_elem);

		/* Bump the list's refcount and return it */
		cl->refcount++;

		CACHE2_elog(DEBUG2, "SearchCatCacheList(%s): found list",
					cache->cc_relname);

#ifdef CATCACHE_STATS
		cache->cc_lhits++;
#endif

		return cl;
	}

	/*
	 * List was not found in cache, so we have to build it by reading the
	 * relation.  For each matching tuple found in the relation, use an
	 * existing cache entry if possible, else build a new one.
	 */
	relation = heap_open(cache->cc_reloid, AccessShareLock);

	scandesc = systable_beginscan(relation,
								  cache->cc_indname,
								  true,
								  SnapshotNow,
								  nkeys,
								  cur_skey);

	/* The list will be ordered iff we are doing an index scan */
	ordered = (scandesc->irel != NULL);

	ctlist = NIL;
	nmembers = 0;

	while (HeapTupleIsValid(ntp = systable_getnext(scandesc)))
	{
		uint32		hashValue;
		Index		hashIndex;

		/*
		 * See if there's an entry for this tuple already.
		 */
		ct = NULL;
		hashValue = CatalogCacheComputeTupleHashValue(cache, ntp);
		hashIndex = HASH_INDEX(hashValue, cache->cc_nbuckets);

		for (elt = DLGetHead(&cache->cc_bucket[hashIndex]);
			 elt;
			 elt = DLGetSucc(elt))
		{
			ct = (CatCTup *) DLE_VAL(elt);

			if (ct->dead || ct->negative)
				continue;		/* ignore dead and negative entries */

			if (ct->hash_value != hashValue)
				continue;		/* quickly skip entry if wrong hash val */

			if (!ItemPointerEquals(&(ct->tuple.t_self), &(ntp->t_self)))
				continue;		/* not same tuple */

			/*
			 * Found a match, but can't use it if it belongs to another
			 * list already
			 */
			if (ct->c_list)
				continue;

			/* Found a match, so bump its refcount and move to front */
			ct->refcount++;

			DLMoveToFront(&ct->lrulist_elem);

			break;
		}

		if (elt == NULL)
		{
			/* We didn't find a usable entry, so make a new one */
			ct = CatalogCacheCreateEntry(cache, ntp,
										 hashValue, hashIndex,
										 false);
		}

		ctlist = lcons(ct, ctlist);
		nmembers++;
	}

	systable_endscan(scandesc);

	heap_close(relation, AccessShareLock);

	/*
	 * Now we can build the CatCList entry.  First we need a dummy tuple
	 * containing the key values...
	 */
	ntp = build_dummy_tuple(cache, nkeys, cur_skey);
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
	cl = (CatCList *) palloc(sizeof(CatCList) + nmembers * sizeof(CatCTup *));
	heap_copytuple_with_tuple(ntp, &cl->tuple);
	MemoryContextSwitchTo(oldcxt);
	heap_freetuple(ntp);

	cl->cl_magic = CL_MAGIC;
	cl->my_cache = cache;
	DLInitElem(&cl->cache_elem, (void *) cl);
	cl->refcount = 1;			/* count this first reference */
	cl->dead = false;
	cl->ordered = ordered;
	cl->nkeys = nkeys;
	cl->hash_value = lHashValue;
	cl->n_members = nmembers;
	/* The list is backwards because we built it with lcons */
	for (i = nmembers; --i >= 0;)
	{
		cl->members[i] = ct = (CatCTup *) lfirst(ctlist);
		Assert(ct->c_list == NULL);
		ct->c_list = cl;
		/* mark list dead if any members already dead */
		if (ct->dead)
			cl->dead = true;
		ctlist = lnext(ctlist);
	}

	DLAddHead(&cache->cc_lists, &cl->cache_elem);

	CACHE3_elog(DEBUG2, "SearchCatCacheList(%s): made list of %d members",
				cache->cc_relname, nmembers);

	return cl;
}

/*
 *	ReleaseCatCacheList
 *
 *	Decrement the reference counts of a catcache list.
 */
void
ReleaseCatCacheList(CatCList *list)
{
	int			i;

	/* Safety checks to ensure we were handed a cache entry */
	Assert(list->cl_magic == CL_MAGIC);
	Assert(list->refcount > 0);

	for (i = list->n_members; --i >= 0;)
	{
		CatCTup    *ct = list->members[i];

		Assert(ct->refcount > 0);

		ct->refcount--;

		if (ct->dead)
			list->dead = true;
		/* can't remove tuple before list is removed */
	}

	list->refcount--;

	if (list->refcount == 0
#ifndef CATCACHE_FORCE_RELEASE
		&& list->dead
#endif
		)
		CatCacheRemoveCList(list->my_cache, list);
}


/*
 * CatalogCacheCreateEntry
 *		Create a new CatCTup entry, copying the given HeapTuple and other
 *		supplied data into it.	The new entry is given refcount 1.
 */
static CatCTup *
CatalogCacheCreateEntry(CatCache *cache, HeapTuple ntp,
						uint32 hashValue, Index hashIndex, bool negative)
{
	CatCTup    *ct;
	MemoryContext oldcxt;

	/*
	 * Allocate CatCTup header in cache memory, and copy the tuple there
	 * too.
	 */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
	ct = (CatCTup *) palloc(sizeof(CatCTup));
	heap_copytuple_with_tuple(ntp, &ct->tuple);
	MemoryContextSwitchTo(oldcxt);

	/*
	 * Finish initializing the CatCTup header, and add it to the cache's
	 * linked lists and counts.
	 */
	ct->ct_magic = CT_MAGIC;
	ct->my_cache = cache;
	DLInitElem(&ct->lrulist_elem, (void *) ct);
	DLInitElem(&ct->cache_elem, (void *) ct);
	ct->c_list = NULL;
	ct->refcount = 1;			/* count this first reference */
	ct->dead = false;
	ct->negative = negative;
	ct->hash_value = hashValue;

	DLAddHead(&CacheHdr->ch_lrulist, &ct->lrulist_elem);
	DLAddHead(&cache->cc_bucket[hashIndex], &ct->cache_elem);

	cache->cc_ntup++;
	CacheHdr->ch_ntup++;

	/*
	 * If we've exceeded the desired size of the caches, try to throw away
	 * the least recently used entry.  NB: the newly-built entry cannot
	 * get thrown away here, because it has positive refcount.
	 */
	if (CacheHdr->ch_ntup > CacheHdr->ch_maxtup)
	{
		Dlelem	   *elt,
				   *prevelt;

		for (elt = DLGetTail(&CacheHdr->ch_lrulist); elt; elt = prevelt)
		{
			CatCTup    *oldct = (CatCTup *) DLE_VAL(elt);

			prevelt = DLGetPred(elt);

			if (oldct->refcount == 0)
			{
				CACHE2_elog(DEBUG2, "CatCacheCreateEntry(%s): Overflow, LRU removal",
							cache->cc_relname);
#ifdef CATCACHE_STATS
				oldct->my_cache->cc_discards++;
#endif
				CatCacheRemoveCTup(oldct->my_cache, oldct);
				if (CacheHdr->ch_ntup <= CacheHdr->ch_maxtup)
					break;
			}
		}
	}

	return ct;
}

/*
 * build_dummy_tuple
 *		Generate a palloc'd HeapTuple that contains the specified key
 *		columns, and NULLs for other columns.
 *
 * This is used to store the keys for negative cache entries and CatCList
 * entries, which don't have real tuples associated with them.
 */
static HeapTuple
build_dummy_tuple(CatCache *cache, int nkeys, ScanKey skeys)
{
	HeapTuple	ntp;
	TupleDesc	tupDesc = cache->cc_tupdesc;
	Datum	   *values;
	char	   *nulls;
	Oid			tupOid = InvalidOid;
	NameData	tempNames[4];
	int			i;

	values = (Datum *) palloc(tupDesc->natts * sizeof(Datum));
	nulls = (char *) palloc(tupDesc->natts * sizeof(char));

	memset(values, 0, tupDesc->natts * sizeof(Datum));
	memset(nulls, 'n', tupDesc->natts * sizeof(char));

	for (i = 0; i < nkeys; i++)
	{
		int			attindex = cache->cc_key[i];
		Datum		keyval = skeys[i].sk_argument;

		if (attindex > 0)
		{
			/*
			 * Here we must be careful in case the caller passed a C
			 * string where a NAME is wanted: convert the given argument
			 * to a correctly padded NAME.	Otherwise the memcpy() done in
			 * heap_formtuple could fall off the end of memory.
			 */
			if (cache->cc_isname[i])
			{
				Name		newval = &tempNames[i];

				namestrcpy(newval, DatumGetCString(keyval));
				keyval = NameGetDatum(newval);
			}
			values[attindex - 1] = keyval;
			nulls[attindex - 1] = ' ';
		}
		else
		{
			Assert(attindex == ObjectIdAttributeNumber);
			tupOid = DatumGetObjectId(keyval);
		}
	}

	ntp = heap_formtuple(tupDesc, values, nulls);
	if (tupOid != InvalidOid)
		HeapTupleSetOid(ntp, tupOid);

	pfree(values);
	pfree(nulls);

	return ntp;
}


/*
 *	PrepareToInvalidateCacheTuple()
 *
 *	This is part of a rather subtle chain of events, so pay attention:
 *
 *	When a tuple is inserted or deleted, it cannot be flushed from the
 *	catcaches immediately, for reasons explained at the top of cache/inval.c.
 *	Instead we have to add entry(s) for the tuple to a list of pending tuple
 *	invalidations that will be done at the end of the command or transaction.
 *
 *	The lists of tuples that need to be flushed are kept by inval.c.  This
 *	routine is a helper routine for inval.c.  Given a tuple belonging to
 *	the specified relation, find all catcaches it could be in, compute the
 *	correct hash value for each such catcache, and call the specified function
 *	to record the cache id, hash value, and tuple ItemPointer in inval.c's
 *	lists.	CatalogCacheIdInvalidate will be called later, if appropriate,
 *	using the recorded information.
 *
 *	Note that it is irrelevant whether the given tuple is actually loaded
 *	into the catcache at the moment.  Even if it's not there now, it might
 *	be by the end of the command, or there might be a matching negative entry
 *	to flush --- or other backends' caches might have such entries --- so
 *	we have to make list entries to flush it later.
 *
 *	Also note that it's not an error if there are no catcaches for the
 *	specified relation.  inval.c doesn't know exactly which rels have
 *	catcaches --- it will call this routine for any tuple that's in a
 *	system relation.
 */
void
PrepareToInvalidateCacheTuple(Relation relation,
							  HeapTuple tuple,
						void (*function) (int, uint32, ItemPointer, Oid))
{
	CatCache   *ccp;
	Oid			reloid;

	CACHE1_elog(DEBUG2, "PrepareToInvalidateCacheTuple: called");

	/*
	 * sanity checks
	 */
	Assert(RelationIsValid(relation));
	Assert(HeapTupleIsValid(tuple));
	Assert(PointerIsValid(function));
	Assert(CacheHdr != NULL);

	reloid = RelationGetRelid(relation);

	/* ----------------
	 *	for each cache
	 *	   if the cache contains tuples from the specified relation
	 *		   compute the tuple's hash value in this cache,
	 *		   and call the passed function to register the information.
	 * ----------------
	 */

	for (ccp = CacheHdr->ch_caches; ccp; ccp = ccp->cc_next)
	{
		/* Just in case cache hasn't finished initialization yet... */
		if (ccp->cc_tupdesc == NULL)
			CatalogCacheInitializeCache(ccp);

		if (ccp->cc_reloid != reloid)
			continue;

		(*function) (ccp->id,
					 CatalogCacheComputeTupleHashValue(ccp, tuple),
					 &tuple->t_self,
					 ccp->cc_relisshared ? (Oid) 0 : MyDatabaseId);
	}
}
