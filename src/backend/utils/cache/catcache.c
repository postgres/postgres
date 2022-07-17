/*-------------------------------------------------------------------------
 *
 * catcache.c
 *	  System catalog cache for tuples matching a key.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/catcache.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heaptoast.h"
#include "access/relscan.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/valid.h"
#include "access/xact.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#ifdef CATCACHE_STATS
#include "storage/ipc.h"		/* for on_proc_exit */
#endif
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/resowner_private.h"
#include "utils/syscache.h"


 /* #define CACHEDEBUG */	/* turns DEBUG elogs on */

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
#define CACHE_elog(...)				elog(__VA_ARGS__)
#else
#define CACHE_elog(...)
#endif

/* Cache management header --- pointer is NULL until created */
static CatCacheHeader *CacheHdr = NULL;

static inline HeapTuple SearchCatCacheInternal(CatCache *cache,
											   int nkeys,
											   Datum v1, Datum v2,
											   Datum v3, Datum v4);

static pg_noinline HeapTuple SearchCatCacheMiss(CatCache *cache,
												int nkeys,
												uint32 hashValue,
												Index hashIndex,
												Datum v1, Datum v2,
												Datum v3, Datum v4);

static uint32 CatalogCacheComputeHashValue(CatCache *cache, int nkeys,
										   Datum v1, Datum v2, Datum v3, Datum v4);
static uint32 CatalogCacheComputeTupleHashValue(CatCache *cache, int nkeys,
												HeapTuple tuple);
static inline bool CatalogCacheCompareTuple(const CatCache *cache, int nkeys,
											const Datum *cachekeys,
											const Datum *searchkeys);

#ifdef CATCACHE_STATS
static void CatCachePrintStats(int code, Datum arg);
#endif
static void CatCacheRemoveCTup(CatCache *cache, CatCTup *ct);
static void CatCacheRemoveCList(CatCache *cache, CatCList *cl);
static void CatalogCacheInitializeCache(CatCache *cache);
static CatCTup *CatalogCacheCreateEntry(CatCache *cache, HeapTuple ntp,
										Datum *arguments,
										uint32 hashValue, Index hashIndex,
										bool negative);

static void CatCacheFreeKeys(TupleDesc tupdesc, int nkeys, int *attnos,
							 Datum *keys);
static void CatCacheCopyKeys(TupleDesc tupdesc, int nkeys, int *attnos,
							 Datum *srckeys, Datum *dstkeys);


/*
 *					internal support functions
 */

/*
 * Hash and equality functions for system types that are used as cache key
 * fields.  In some cases, we just call the regular SQL-callable functions for
 * the appropriate data type, but that tends to be a little slow, and the
 * speed of these functions is performance-critical.  Therefore, for data
 * types that frequently occur as catcache keys, we hard-code the logic here.
 * Avoiding the overhead of DirectFunctionCallN(...) is a substantial win, and
 * in certain cases (like int4) we can adopt a faster hash algorithm as well.
 */

static bool
chareqfast(Datum a, Datum b)
{
	return DatumGetChar(a) == DatumGetChar(b);
}

static uint32
charhashfast(Datum datum)
{
	return murmurhash32((int32) DatumGetChar(datum));
}

static bool
nameeqfast(Datum a, Datum b)
{
	char	   *ca = NameStr(*DatumGetName(a));
	char	   *cb = NameStr(*DatumGetName(b));

	return strncmp(ca, cb, NAMEDATALEN) == 0;
}

static uint32
namehashfast(Datum datum)
{
	char	   *key = NameStr(*DatumGetName(datum));

	return hash_any((unsigned char *) key, strlen(key));
}

static bool
int2eqfast(Datum a, Datum b)
{
	return DatumGetInt16(a) == DatumGetInt16(b);
}

static uint32
int2hashfast(Datum datum)
{
	return murmurhash32((int32) DatumGetInt16(datum));
}

static bool
int4eqfast(Datum a, Datum b)
{
	return DatumGetInt32(a) == DatumGetInt32(b);
}

static uint32
int4hashfast(Datum datum)
{
	return murmurhash32((int32) DatumGetInt32(datum));
}

static bool
texteqfast(Datum a, Datum b)
{
	/*
	 * The use of DEFAULT_COLLATION_OID is fairly arbitrary here.  We just
	 * want to take the fast "deterministic" path in texteq().
	 */
	return DatumGetBool(DirectFunctionCall2Coll(texteq, DEFAULT_COLLATION_OID, a, b));
}

static uint32
texthashfast(Datum datum)
{
	/* analogously here as in texteqfast() */
	return DatumGetInt32(DirectFunctionCall1Coll(hashtext, DEFAULT_COLLATION_OID, datum));
}

static bool
oidvectoreqfast(Datum a, Datum b)
{
	return DatumGetBool(DirectFunctionCall2(oidvectoreq, a, b));
}

static uint32
oidvectorhashfast(Datum datum)
{
	return DatumGetInt32(DirectFunctionCall1(hashoidvector, datum));
}

/* Lookup support functions for a type. */
static void
GetCCHashEqFuncs(Oid keytype, CCHashFN *hashfunc, RegProcedure *eqfunc, CCFastEqualFN *fasteqfunc)
{
	switch (keytype)
	{
		case BOOLOID:
			*hashfunc = charhashfast;
			*fasteqfunc = chareqfast;
			*eqfunc = F_BOOLEQ;
			break;
		case CHAROID:
			*hashfunc = charhashfast;
			*fasteqfunc = chareqfast;
			*eqfunc = F_CHAREQ;
			break;
		case NAMEOID:
			*hashfunc = namehashfast;
			*fasteqfunc = nameeqfast;
			*eqfunc = F_NAMEEQ;
			break;
		case INT2OID:
			*hashfunc = int2hashfast;
			*fasteqfunc = int2eqfast;
			*eqfunc = F_INT2EQ;
			break;
		case INT4OID:
			*hashfunc = int4hashfast;
			*fasteqfunc = int4eqfast;
			*eqfunc = F_INT4EQ;
			break;
		case TEXTOID:
			*hashfunc = texthashfast;
			*fasteqfunc = texteqfast;
			*eqfunc = F_TEXTEQ;
			break;
		case OIDOID:
		case REGPROCOID:
		case REGPROCEDUREOID:
		case REGOPEROID:
		case REGOPERATOROID:
		case REGCLASSOID:
		case REGTYPEOID:
		case REGCOLLATIONOID:
		case REGCONFIGOID:
		case REGDICTIONARYOID:
		case REGROLEOID:
		case REGNAMESPACEOID:
			*hashfunc = int4hashfast;
			*fasteqfunc = int4eqfast;
			*eqfunc = F_OIDEQ;
			break;
		case OIDVECTOROID:
			*hashfunc = oidvectorhashfast;
			*fasteqfunc = oidvectoreqfast;
			*eqfunc = F_OIDVECTOREQ;
			break;
		default:
			elog(FATAL, "type %u not supported as catcache key", keytype);
			*hashfunc = NULL;	/* keep compiler quiet */

			*eqfunc = InvalidOid;
			break;
	}
}

/*
 *		CatalogCacheComputeHashValue
 *
 * Compute the hash value associated with a given set of lookup keys
 */
static uint32
CatalogCacheComputeHashValue(CatCache *cache, int nkeys,
							 Datum v1, Datum v2, Datum v3, Datum v4)
{
	uint32		hashValue = 0;
	uint32		oneHash;
	CCHashFN   *cc_hashfunc = cache->cc_hashfunc;

	CACHE_elog(DEBUG2, "CatalogCacheComputeHashValue %s %d %p",
			   cache->cc_relname, nkeys, cache);

	switch (nkeys)
	{
		case 4:
			oneHash = (cc_hashfunc[3]) (v4);
			hashValue ^= pg_rotate_left32(oneHash, 24);
			/* FALLTHROUGH */
		case 3:
			oneHash = (cc_hashfunc[2]) (v3);
			hashValue ^= pg_rotate_left32(oneHash, 16);
			/* FALLTHROUGH */
		case 2:
			oneHash = (cc_hashfunc[1]) (v2);
			hashValue ^= pg_rotate_left32(oneHash, 8);
			/* FALLTHROUGH */
		case 1:
			oneHash = (cc_hashfunc[0]) (v1);
			hashValue ^= oneHash;
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
CatalogCacheComputeTupleHashValue(CatCache *cache, int nkeys, HeapTuple tuple)
{
	Datum		v1 = 0,
				v2 = 0,
				v3 = 0,
				v4 = 0;
	bool		isNull = false;
	int		   *cc_keyno = cache->cc_keyno;
	TupleDesc	cc_tupdesc = cache->cc_tupdesc;

	/* Now extract key fields from tuple, insert into scankey */
	switch (nkeys)
	{
		case 4:
			v4 = fastgetattr(tuple,
							 cc_keyno[3],
							 cc_tupdesc,
							 &isNull);
			Assert(!isNull);
			/* FALLTHROUGH */
		case 3:
			v3 = fastgetattr(tuple,
							 cc_keyno[2],
							 cc_tupdesc,
							 &isNull);
			Assert(!isNull);
			/* FALLTHROUGH */
		case 2:
			v2 = fastgetattr(tuple,
							 cc_keyno[1],
							 cc_tupdesc,
							 &isNull);
			Assert(!isNull);
			/* FALLTHROUGH */
		case 1:
			v1 = fastgetattr(tuple,
							 cc_keyno[0],
							 cc_tupdesc,
							 &isNull);
			Assert(!isNull);
			break;
		default:
			elog(FATAL, "wrong number of hash keys: %d", nkeys);
			break;
	}

	return CatalogCacheComputeHashValue(cache, nkeys, v1, v2, v3, v4);
}

/*
 *		CatalogCacheCompareTuple
 *
 * Compare a tuple to the passed arguments.
 */
static inline bool
CatalogCacheCompareTuple(const CatCache *cache, int nkeys,
						 const Datum *cachekeys,
						 const Datum *searchkeys)
{
	const CCFastEqualFN *cc_fastequal = cache->cc_fastequal;
	int			i;

	for (i = 0; i < nkeys; i++)
	{
		if (!(cc_fastequal[i]) (cachekeys[i], searchkeys[i]))
			return false;
	}
	return true;
}


#ifdef CATCACHE_STATS

static void
CatCachePrintStats(int code, Datum arg)
{
	slist_iter	iter;
	long		cc_searches = 0;
	long		cc_hits = 0;
	long		cc_neg_hits = 0;
	long		cc_newloads = 0;
	long		cc_invals = 0;
	long		cc_lsearches = 0;
	long		cc_lhits = 0;

	slist_foreach(iter, &CacheHdr->ch_caches)
	{
		CatCache   *cache = slist_container(CatCache, cc_next, iter.cur);

		if (cache->cc_ntup == 0 && cache->cc_searches == 0)
			continue;			/* don't print unused caches */
		elog(DEBUG2, "catcache %s/%u: %d tup, %ld srch, %ld+%ld=%ld hits, %ld+%ld=%ld loads, %ld invals, %ld lsrch, %ld lhits",
			 cache->cc_relname,
			 cache->cc_indexoid,
			 cache->cc_ntup,
			 cache->cc_searches,
			 cache->cc_hits,
			 cache->cc_neg_hits,
			 cache->cc_hits + cache->cc_neg_hits,
			 cache->cc_newloads,
			 cache->cc_searches - cache->cc_hits - cache->cc_neg_hits - cache->cc_newloads,
			 cache->cc_searches - cache->cc_hits - cache->cc_neg_hits,
			 cache->cc_invals,
			 cache->cc_lsearches,
			 cache->cc_lhits);
		cc_searches += cache->cc_searches;
		cc_hits += cache->cc_hits;
		cc_neg_hits += cache->cc_neg_hits;
		cc_newloads += cache->cc_newloads;
		cc_invals += cache->cc_invals;
		cc_lsearches += cache->cc_lsearches;
		cc_lhits += cache->cc_lhits;
	}
	elog(DEBUG2, "catcache totals: %d tup, %ld srch, %ld+%ld=%ld hits, %ld+%ld=%ld loads, %ld invals, %ld lsrch, %ld lhits",
		 CacheHdr->ch_ntup,
		 cc_searches,
		 cc_hits,
		 cc_neg_hits,
		 cc_hits + cc_neg_hits,
		 cc_newloads,
		 cc_searches - cc_hits - cc_neg_hits - cc_newloads,
		 cc_searches - cc_hits - cc_neg_hits,
		 cc_invals,
		 cc_lsearches,
		 cc_lhits);
}
#endif							/* CATCACHE_STATS */


/*
 *		CatCacheRemoveCTup
 *
 * Unlink and delete the given cache entry
 *
 * NB: if it is a member of a CatCList, the CatCList is deleted too.
 * Both the cache entry and the list had better have zero refcount.
 */
static void
CatCacheRemoveCTup(CatCache *cache, CatCTup *ct)
{
	Assert(ct->refcount == 0);
	Assert(ct->my_cache == cache);

	if (ct->c_list)
	{
		/*
		 * The cleanest way to handle this is to call CatCacheRemoveCList,
		 * which will recurse back to me, and the recursive call will do the
		 * work.  Set the "dead" flag to make sure it does recurse.
		 */
		ct->dead = true;
		CatCacheRemoveCList(cache, ct->c_list);
		return;					/* nothing left to do */
	}

	/* delink from linked list */
	dlist_delete(&ct->cache_elem);

	/*
	 * Free keys when we're dealing with a negative entry, normal entries just
	 * point into tuple, allocated together with the CatCTup.
	 */
	if (ct->negative)
		CatCacheFreeKeys(cache->cc_tupdesc, cache->cc_nkeys,
						 cache->cc_keyno, ct->keys);

	pfree(ct);

	--cache->cc_ntup;
	--CacheHdr->ch_ntup;
}

/*
 *		CatCacheRemoveCList
 *
 * Unlink and delete the given cache list entry
 *
 * NB: any dead member entries that become unreferenced are deleted too.
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
		/* if the member is dead and now has no references, remove it */
		if (
#ifndef CATCACHE_FORCE_RELEASE
			ct->dead &&
#endif
			ct->refcount == 0)
			CatCacheRemoveCTup(cache, ct);
	}

	/* delink from linked list */
	dlist_delete(&cl->cache_elem);

	/* free associated column data */
	CatCacheFreeKeys(cache->cc_tupdesc, cl->nkeys,
					 cache->cc_keyno, cl->keys);

	pfree(cl);
}


/*
 *	CatCacheInvalidate
 *
 *	Invalidate entries in the specified cache, given a hash value.
 *
 *	We delete cache entries that match the hash value, whether positive
 *	or negative.  We don't care whether the invalidation is the result
 *	of a tuple insertion or a deletion.
 *
 *	We used to try to match positive cache entries by TID, but that is
 *	unsafe after a VACUUM FULL on a system catalog: an inval event could
 *	be queued before VACUUM FULL, and then processed afterwards, when the
 *	target tuple that has to be invalidated has a different TID than it
 *	did when the event was created.  So now we just compare hash values and
 *	accept the small risk of unnecessary invalidations due to false matches.
 *
 *	This routine is only quasi-public: it should only be used by inval.c.
 */
void
CatCacheInvalidate(CatCache *cache, uint32 hashValue)
{
	Index		hashIndex;
	dlist_mutable_iter iter;

	CACHE_elog(DEBUG2, "CatCacheInvalidate: called");

	/*
	 * We don't bother to check whether the cache has finished initialization
	 * yet; if not, there will be no entries in it so no problem.
	 */

	/*
	 * Invalidate *all* CatCLists in this cache; it's too hard to tell which
	 * searches might still be correct, so just zap 'em all.
	 */
	dlist_foreach_modify(iter, &cache->cc_lists)
	{
		CatCList   *cl = dlist_container(CatCList, cache_elem, iter.cur);

		if (cl->refcount > 0)
			cl->dead = true;
		else
			CatCacheRemoveCList(cache, cl);
	}

	/*
	 * inspect the proper hash bucket for tuple matches
	 */
	hashIndex = HASH_INDEX(hashValue, cache->cc_nbuckets);
	dlist_foreach_modify(iter, &cache->cc_bucket[hashIndex])
	{
		CatCTup    *ct = dlist_container(CatCTup, cache_elem, iter.cur);

		if (hashValue == ct->hash_value)
		{
			if (ct->refcount > 0 ||
				(ct->c_list && ct->c_list->refcount > 0))
			{
				ct->dead = true;
				/* list, if any, was marked dead above */
				Assert(ct->c_list == NULL || ct->c_list->dead);
			}
			else
				CatCacheRemoveCTup(cache, ct);
			CACHE_elog(DEBUG2, "CatCacheInvalidate: invalidated");
#ifdef CATCACHE_STATS
			cache->cc_invals++;
#endif
			/* could be multiple matches, so keep looking! */
		}
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
	 * Purely for paranoia, check that context doesn't exist; caller probably
	 * did so already.
	 */
	if (!CacheMemoryContext)
		CacheMemoryContext = AllocSetContextCreate(TopMemoryContext,
												   "CacheMemoryContext",
												   ALLOCSET_DEFAULT_SIZES);
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
	dlist_mutable_iter iter;
	int			i;

	/* Remove each list in this cache, or at least mark it dead */
	dlist_foreach_modify(iter, &cache->cc_lists)
	{
		CatCList   *cl = dlist_container(CatCList, cache_elem, iter.cur);

		if (cl->refcount > 0)
			cl->dead = true;
		else
			CatCacheRemoveCList(cache, cl);
	}

	/* Remove each tuple in this cache, or at least mark it dead */
	for (i = 0; i < cache->cc_nbuckets; i++)
	{
		dlist_head *bucket = &cache->cc_bucket[i];

		dlist_foreach_modify(iter, bucket)
		{
			CatCTup    *ct = dlist_container(CatCTup, cache_elem, iter.cur);

			if (ct->refcount > 0 ||
				(ct->c_list && ct->c_list->refcount > 0))
			{
				ct->dead = true;
				/* list, if any, was marked dead above */
				Assert(ct->c_list == NULL || ct->c_list->dead);
			}
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
	slist_iter	iter;

	CACHE_elog(DEBUG2, "ResetCatalogCaches called");

	slist_foreach(iter, &CacheHdr->ch_caches)
	{
		CatCache   *cache = slist_container(CatCache, cc_next, iter.cur);

		ResetCatalogCache(cache);
	}

	CACHE_elog(DEBUG2, "end of ResetCatalogCaches call");
}

/*
 *		CatalogCacheFlushCatalog
 *
 *	Flush all catcache entries that came from the specified system catalog.
 *	This is needed after VACUUM FULL/CLUSTER on the catalog, since the
 *	tuples very likely now have different TIDs than before.  (At one point
 *	we also tried to force re-execution of CatalogCacheInitializeCache for
 *	the cache(s) on that catalog.  This is a bad idea since it leads to all
 *	kinds of trouble if a cache flush occurs while loading cache entries.
 *	We now avoid the need to do it by copying cc_tupdesc out of the relcache,
 *	rather than relying on the relcache to keep a tupdesc for us.  Of course
 *	this assumes the tupdesc of a cachable system table will not change...)
 */
void
CatalogCacheFlushCatalog(Oid catId)
{
	slist_iter	iter;

	CACHE_elog(DEBUG2, "CatalogCacheFlushCatalog called for %u", catId);

	slist_foreach(iter, &CacheHdr->ch_caches)
	{
		CatCache   *cache = slist_container(CatCache, cc_next, iter.cur);

		/* Does this cache store tuples of the target catalog? */
		if (cache->cc_reloid == catId)
		{
			/* Yes, so flush all its contents */
			ResetCatalogCache(cache);

			/* Tell inval.c to call syscache callbacks for this cache */
			CallSyscacheCallbacks(cache->id, 0);
		}
	}

	CACHE_elog(DEBUG2, "end of CatalogCacheFlushCatalog call");
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
	elog(DEBUG2, "InitCatCache: rel=%u ind=%u id=%d nkeys=%d size=%d", \
		 cp->cc_reloid, cp->cc_indexoid, cp->id, \
		 cp->cc_nkeys, cp->cc_nbuckets); \
} while(0)
#else
#define InitCatCache_DEBUG2
#endif

CatCache *
InitCatCache(int id,
			 Oid reloid,
			 Oid indexoid,
			 int nkeys,
			 const int *key,
			 int nbuckets)
{
	CatCache   *cp;
	MemoryContext oldcxt;
	size_t		sz;
	int			i;

	/*
	 * nbuckets is the initial number of hash buckets to use in this catcache.
	 * It will be enlarged later if it becomes too full.
	 *
	 * nbuckets must be a power of two.  We check this via Assert rather than
	 * a full runtime check because the values will be coming from constant
	 * tables.
	 *
	 * If you're confused by the power-of-two check, see comments in
	 * bitmapset.c for an explanation.
	 */
	Assert(nbuckets > 0 && (nbuckets & -nbuckets) == nbuckets);

	/*
	 * first switch to the cache context so our allocations do not vanish at
	 * the end of a transaction
	 */
	if (!CacheMemoryContext)
		CreateCacheMemoryContext();

	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	/*
	 * if first time through, initialize the cache group header
	 */
	if (CacheHdr == NULL)
	{
		CacheHdr = (CatCacheHeader *) palloc(sizeof(CatCacheHeader));
		slist_init(&CacheHdr->ch_caches);
		CacheHdr->ch_ntup = 0;
#ifdef CATCACHE_STATS
		/* set up to dump stats at backend exit */
		on_proc_exit(CatCachePrintStats, 0);
#endif
	}

	/*
	 * Allocate a new cache structure, aligning to a cacheline boundary
	 *
	 * Note: we rely on zeroing to initialize all the dlist headers correctly
	 */
	sz = sizeof(CatCache) + PG_CACHE_LINE_SIZE;
	cp = (CatCache *) CACHELINEALIGN(palloc0(sz));
	cp->cc_bucket = palloc0(nbuckets * sizeof(dlist_head));

	/*
	 * initialize the cache's relation information for the relation
	 * corresponding to this cache, and initialize some of the new cache's
	 * other internal fields.  But don't open the relation yet.
	 */
	cp->id = id;
	cp->cc_relname = "(not known yet)";
	cp->cc_reloid = reloid;
	cp->cc_indexoid = indexoid;
	cp->cc_relisshared = false; /* temporary */
	cp->cc_tupdesc = (TupleDesc) NULL;
	cp->cc_ntup = 0;
	cp->cc_nbuckets = nbuckets;
	cp->cc_nkeys = nkeys;
	for (i = 0; i < nkeys; ++i)
		cp->cc_keyno[i] = key[i];

	/*
	 * new cache is initialized as far as we can go for now. print some
	 * debugging information, if appropriate.
	 */
	InitCatCache_DEBUG2;

	/*
	 * add completed cache to top of group header's list
	 */
	slist_push_head(&CacheHdr->ch_caches, &cp->cc_next);

	/*
	 * back to the old context before we return...
	 */
	MemoryContextSwitchTo(oldcxt);

	return cp;
}

/*
 * Enlarge a catcache, doubling the number of buckets.
 */
static void
RehashCatCache(CatCache *cp)
{
	dlist_head *newbucket;
	int			newnbuckets;
	int			i;

	elog(DEBUG1, "rehashing catalog cache id %d for %s; %d tups, %d buckets",
		 cp->id, cp->cc_relname, cp->cc_ntup, cp->cc_nbuckets);

	/* Allocate a new, larger, hash table. */
	newnbuckets = cp->cc_nbuckets * 2;
	newbucket = (dlist_head *) MemoryContextAllocZero(CacheMemoryContext, newnbuckets * sizeof(dlist_head));

	/* Move all entries from old hash table to new. */
	for (i = 0; i < cp->cc_nbuckets; i++)
	{
		dlist_mutable_iter iter;

		dlist_foreach_modify(iter, &cp->cc_bucket[i])
		{
			CatCTup    *ct = dlist_container(CatCTup, cache_elem, iter.cur);
			int			hashIndex = HASH_INDEX(ct->hash_value, newnbuckets);

			dlist_delete(iter.cur);
			dlist_push_head(&newbucket[hashIndex], &ct->cache_elem);
		}
	}

	/* Switch to the new array. */
	pfree(cp->cc_bucket);
	cp->cc_nbuckets = newnbuckets;
	cp->cc_bucket = newbucket;
}

/*
 *		CatalogCacheInitializeCache
 *
 * This function does final initialization of a catcache: obtain the tuple
 * descriptor and set up the hash and equality function links.  We assume
 * that the relcache entry can be opened at this point!
 */
#ifdef CACHEDEBUG
#define CatalogCacheInitializeCache_DEBUG1 \
	elog(DEBUG2, "CatalogCacheInitializeCache: cache @%p rel=%u", cache, \
		 cache->cc_reloid)

#define CatalogCacheInitializeCache_DEBUG2 \
do { \
		if (cache->cc_keyno[i] > 0) { \
			elog(DEBUG2, "CatalogCacheInitializeCache: load %d/%d w/%d, %u", \
				i+1, cache->cc_nkeys, cache->cc_keyno[i], \
				 TupleDescAttr(tupdesc, cache->cc_keyno[i] - 1)->atttypid); \
		} else { \
			elog(DEBUG2, "CatalogCacheInitializeCache: load %d/%d w/%d", \
				i+1, cache->cc_nkeys, cache->cc_keyno[i]); \
		} \
} while(0)
#else
#define CatalogCacheInitializeCache_DEBUG1
#define CatalogCacheInitializeCache_DEBUG2
#endif

static void
CatalogCacheInitializeCache(CatCache *cache)
{
	Relation	relation;
	MemoryContext oldcxt;
	TupleDesc	tupdesc;
	int			i;

	CatalogCacheInitializeCache_DEBUG1;

	relation = table_open(cache->cc_reloid, AccessShareLock);

	/*
	 * switch to the cache context so our allocations do not vanish at the end
	 * of a transaction
	 */
	Assert(CacheMemoryContext != NULL);

	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	/*
	 * copy the relcache's tuple descriptor to permanent cache storage
	 */
	tupdesc = CreateTupleDescCopyConstr(RelationGetDescr(relation));

	/*
	 * save the relation's name and relisshared flag, too (cc_relname is used
	 * only for debugging purposes)
	 */
	cache->cc_relname = pstrdup(RelationGetRelationName(relation));
	cache->cc_relisshared = RelationGetForm(relation)->relisshared;

	/*
	 * return to the caller's memory context and close the rel
	 */
	MemoryContextSwitchTo(oldcxt);

	table_close(relation, AccessShareLock);

	CACHE_elog(DEBUG2, "CatalogCacheInitializeCache: %s, %d keys",
			   cache->cc_relname, cache->cc_nkeys);

	/*
	 * initialize cache's key information
	 */
	for (i = 0; i < cache->cc_nkeys; ++i)
	{
		Oid			keytype;
		RegProcedure eqfunc;

		CatalogCacheInitializeCache_DEBUG2;

		if (cache->cc_keyno[i] > 0)
		{
			Form_pg_attribute attr = TupleDescAttr(tupdesc,
												   cache->cc_keyno[i] - 1);

			keytype = attr->atttypid;
			/* cache key columns should always be NOT NULL */
			Assert(attr->attnotnull);
		}
		else
		{
			if (cache->cc_keyno[i] < 0)
				elog(FATAL, "sys attributes are not supported in caches");
			keytype = OIDOID;
		}

		GetCCHashEqFuncs(keytype,
						 &cache->cc_hashfunc[i],
						 &eqfunc,
						 &cache->cc_fastequal[i]);

		/*
		 * Do equality-function lookup (we assume this won't need a catalog
		 * lookup for any supported type)
		 */
		fmgr_info_cxt(eqfunc,
					  &cache->cc_skey[i].sk_func,
					  CacheMemoryContext);

		/* Initialize sk_attno suitably for HeapKeyTest() and heap scans */
		cache->cc_skey[i].sk_attno = cache->cc_keyno[i];

		/* Fill in sk_strategy as well --- always standard equality */
		cache->cc_skey[i].sk_strategy = BTEqualStrategyNumber;
		cache->cc_skey[i].sk_subtype = InvalidOid;
		/* If a catcache key requires a collation, it must be C collation */
		cache->cc_skey[i].sk_collation = C_COLLATION_OID;

		CACHE_elog(DEBUG2, "CatalogCacheInitializeCache %s %d %p",
				   cache->cc_relname, i, cache);
	}

	/*
	 * mark this cache fully initialized
	 */
	cache->cc_tupdesc = tupdesc;
}

/*
 * InitCatCachePhase2 -- external interface for CatalogCacheInitializeCache
 *
 * One reason to call this routine is to ensure that the relcache has
 * created entries for all the catalogs and indexes referenced by catcaches.
 * Therefore, provide an option to open the index as well as fixing the
 * cache itself.  An exception is the indexes on pg_am, which we don't use
 * (cf. IndexScanOK).
 */
void
InitCatCachePhase2(CatCache *cache, bool touch_index)
{
	if (cache->cc_tupdesc == NULL)
		CatalogCacheInitializeCache(cache);

	if (touch_index &&
		cache->id != AMOID &&
		cache->id != AMNAME)
	{
		Relation	idesc;

		/*
		 * We must lock the underlying catalog before opening the index to
		 * avoid deadlock, since index_open could possibly result in reading
		 * this same catalog, and if anyone else is exclusive-locking this
		 * catalog and index they'll be doing it in that order.
		 */
		LockRelationOid(cache->cc_reloid, AccessShareLock);
		idesc = index_open(cache->cc_indexoid, AccessShareLock);

		/*
		 * While we've got the index open, let's check that it's unique (and
		 * not just deferrable-unique, thank you very much).  This is just to
		 * catch thinkos in definitions of new catcaches, so we don't worry
		 * about the pg_am indexes not getting tested.
		 */
		Assert(idesc->rd_index->indisunique &&
			   idesc->rd_index->indimmediate);

		index_close(idesc, AccessShareLock);
		UnlockRelationOid(cache->cc_reloid, AccessShareLock);
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
 *		Once we have completed relcache initialization (signaled by
 *		criticalRelcachesBuilt), we don't have to worry anymore.
 *
 *		Similarly, during backend startup we have to be able to use the
 *		pg_authid, pg_auth_members and pg_database syscaches for
 *		authentication even if we don't yet have relcache entries for those
 *		catalogs' indexes.
 */
static bool
IndexScanOK(CatCache *cache, ScanKey cur_skey)
{
	switch (cache->id)
	{
		case INDEXRELID:

			/*
			 * Rather than tracking exactly which indexes have to be loaded
			 * before we can use indexscans (which changes from time to time),
			 * just force all pg_index searches to be heap scans until we've
			 * built the critical relcaches.
			 */
			if (!criticalRelcachesBuilt)
				return false;
			break;

		case AMOID:
		case AMNAME:

			/*
			 * Always do heap scans in pg_am, because it's so small there's
			 * not much point in an indexscan anyway.  We *must* do this when
			 * initially building critical relcache entries, but we might as
			 * well just always do it.
			 */
			return false;

		case AUTHNAME:
		case AUTHOID:
		case AUTHMEMMEMROLE:
		case DATABASEOID:

			/*
			 * Protect authentication lookups occurring before relcache has
			 * collected entries for shared indexes.
			 */
			if (!criticalSharedRelcachesBuilt)
				return false;
			break;

		default:
			break;
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
 *		the cache.  The caller must not modify the tuple, and must call
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
	return SearchCatCacheInternal(cache, cache->cc_nkeys, v1, v2, v3, v4);
}


/*
 * SearchCatCacheN() are SearchCatCache() versions for a specific number of
 * arguments. The compiler can inline the body and unroll loops, making them a
 * bit faster than SearchCatCache().
 */

HeapTuple
SearchCatCache1(CatCache *cache,
				Datum v1)
{
	return SearchCatCacheInternal(cache, 1, v1, 0, 0, 0);
}


HeapTuple
SearchCatCache2(CatCache *cache,
				Datum v1, Datum v2)
{
	return SearchCatCacheInternal(cache, 2, v1, v2, 0, 0);
}


HeapTuple
SearchCatCache3(CatCache *cache,
				Datum v1, Datum v2, Datum v3)
{
	return SearchCatCacheInternal(cache, 3, v1, v2, v3, 0);
}


HeapTuple
SearchCatCache4(CatCache *cache,
				Datum v1, Datum v2, Datum v3, Datum v4)
{
	return SearchCatCacheInternal(cache, 4, v1, v2, v3, v4);
}

/*
 * Work-horse for SearchCatCache/SearchCatCacheN.
 */
static inline HeapTuple
SearchCatCacheInternal(CatCache *cache,
					   int nkeys,
					   Datum v1,
					   Datum v2,
					   Datum v3,
					   Datum v4)
{
	Datum		arguments[CATCACHE_MAXKEYS];
	uint32		hashValue;
	Index		hashIndex;
	dlist_iter	iter;
	dlist_head *bucket;
	CatCTup    *ct;

	/* Make sure we're in an xact, even if this ends up being a cache hit */
	Assert(IsTransactionState());

	Assert(cache->cc_nkeys == nkeys);

	/*
	 * one-time startup overhead for each cache
	 */
	if (unlikely(cache->cc_tupdesc == NULL))
		CatalogCacheInitializeCache(cache);

#ifdef CATCACHE_STATS
	cache->cc_searches++;
#endif

	/* Initialize local parameter array */
	arguments[0] = v1;
	arguments[1] = v2;
	arguments[2] = v3;
	arguments[3] = v4;

	/*
	 * find the hash bucket in which to look for the tuple
	 */
	hashValue = CatalogCacheComputeHashValue(cache, nkeys, v1, v2, v3, v4);
	hashIndex = HASH_INDEX(hashValue, cache->cc_nbuckets);

	/*
	 * scan the hash bucket until we find a match or exhaust our tuples
	 *
	 * Note: it's okay to use dlist_foreach here, even though we modify the
	 * dlist within the loop, because we don't continue the loop afterwards.
	 */
	bucket = &cache->cc_bucket[hashIndex];
	dlist_foreach(iter, bucket)
	{
		ct = dlist_container(CatCTup, cache_elem, iter.cur);

		if (ct->dead)
			continue;			/* ignore dead entries */

		if (ct->hash_value != hashValue)
			continue;			/* quickly skip entry if wrong hash val */

		if (!CatalogCacheCompareTuple(cache, nkeys, ct->keys, arguments))
			continue;

		/*
		 * We found a match in the cache.  Move it to the front of the list
		 * for its hashbucket, in order to speed subsequent searches.  (The
		 * most frequently accessed elements in any hashbucket will tend to be
		 * near the front of the hashbucket's list.)
		 */
		dlist_move_head(bucket, &ct->cache_elem);

		/*
		 * If it's a positive entry, bump its refcount and return it. If it's
		 * negative, we can report failure to the caller.
		 */
		if (!ct->negative)
		{
			ResourceOwnerEnlargeCatCacheRefs(CurrentResourceOwner);
			ct->refcount++;
			ResourceOwnerRememberCatCacheRef(CurrentResourceOwner, &ct->tuple);

			CACHE_elog(DEBUG2, "SearchCatCache(%s): found in bucket %d",
					   cache->cc_relname, hashIndex);

#ifdef CATCACHE_STATS
			cache->cc_hits++;
#endif

			return &ct->tuple;
		}
		else
		{
			CACHE_elog(DEBUG2, "SearchCatCache(%s): found neg entry in bucket %d",
					   cache->cc_relname, hashIndex);

#ifdef CATCACHE_STATS
			cache->cc_neg_hits++;
#endif

			return NULL;
		}
	}

	return SearchCatCacheMiss(cache, nkeys, hashValue, hashIndex, v1, v2, v3, v4);
}

/*
 * Search the actual catalogs, rather than the cache.
 *
 * This is kept separate from SearchCatCacheInternal() to keep the fast-path
 * as small as possible.  To avoid that effort being undone by a helpful
 * compiler, try to explicitly forbid inlining.
 */
static pg_noinline HeapTuple
SearchCatCacheMiss(CatCache *cache,
				   int nkeys,
				   uint32 hashValue,
				   Index hashIndex,
				   Datum v1,
				   Datum v2,
				   Datum v3,
				   Datum v4)
{
	ScanKeyData cur_skey[CATCACHE_MAXKEYS];
	Relation	relation;
	SysScanDesc scandesc;
	HeapTuple	ntp;
	CatCTup    *ct;
	Datum		arguments[CATCACHE_MAXKEYS];

	/* Initialize local parameter array */
	arguments[0] = v1;
	arguments[1] = v2;
	arguments[2] = v3;
	arguments[3] = v4;

	/*
	 * Ok, need to make a lookup in the relation, copy the scankey and fill
	 * out any per-call fields.
	 */
	memcpy(cur_skey, cache->cc_skey, sizeof(ScanKeyData) * nkeys);
	cur_skey[0].sk_argument = v1;
	cur_skey[1].sk_argument = v2;
	cur_skey[2].sk_argument = v3;
	cur_skey[3].sk_argument = v4;

	/*
	 * Tuple was not found in cache, so we have to try to retrieve it directly
	 * from the relation.  If found, we will add it to the cache; if not
	 * found, we will add a negative cache entry instead.
	 *
	 * NOTE: it is possible for recursive cache lookups to occur while reading
	 * the relation --- for example, due to shared-cache-inval messages being
	 * processed during table_open().  This is OK.  It's even possible for one
	 * of those lookups to find and enter the very same tuple we are trying to
	 * fetch here.  If that happens, we will enter a second copy of the tuple
	 * into the cache.  The first copy will never be referenced again, and
	 * will eventually age out of the cache, so there's no functional problem.
	 * This case is rare enough that it's not worth expending extra cycles to
	 * detect.
	 */
	relation = table_open(cache->cc_reloid, AccessShareLock);

	scandesc = systable_beginscan(relation,
								  cache->cc_indexoid,
								  IndexScanOK(cache, cur_skey),
								  NULL,
								  nkeys,
								  cur_skey);

	ct = NULL;

	while (HeapTupleIsValid(ntp = systable_getnext(scandesc)))
	{
		ct = CatalogCacheCreateEntry(cache, ntp, arguments,
									 hashValue, hashIndex,
									 false);
		/* immediately set the refcount to 1 */
		ResourceOwnerEnlargeCatCacheRefs(CurrentResourceOwner);
		ct->refcount++;
		ResourceOwnerRememberCatCacheRef(CurrentResourceOwner, &ct->tuple);
		break;					/* assume only one match */
	}

	systable_endscan(scandesc);

	table_close(relation, AccessShareLock);

	/*
	 * If tuple was not found, we need to build a negative cache entry
	 * containing a fake tuple.  The fake tuple has the correct key columns,
	 * but nulls everywhere else.
	 *
	 * In bootstrap mode, we don't build negative entries, because the cache
	 * invalidation mechanism isn't alive and can't clear them if the tuple
	 * gets created later.  (Bootstrap doesn't do UPDATEs, so it doesn't need
	 * cache inval for that.)
	 */
	if (ct == NULL)
	{
		if (IsBootstrapProcessingMode())
			return NULL;

		ct = CatalogCacheCreateEntry(cache, NULL, arguments,
									 hashValue, hashIndex,
									 true);

		CACHE_elog(DEBUG2, "SearchCatCache(%s): Contains %d/%d tuples",
				   cache->cc_relname, cache->cc_ntup, CacheHdr->ch_ntup);
		CACHE_elog(DEBUG2, "SearchCatCache(%s): put neg entry in bucket %d",
				   cache->cc_relname, hashIndex);

		/*
		 * We are not returning the negative entry to the caller, so leave its
		 * refcount zero.
		 */

		return NULL;
	}

	CACHE_elog(DEBUG2, "SearchCatCache(%s): Contains %d/%d tuples",
			   cache->cc_relname, cache->cc_ntup, CacheHdr->ch_ntup);
	CACHE_elog(DEBUG2, "SearchCatCache(%s): put in bucket %d",
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
	ResourceOwnerForgetCatCacheRef(CurrentResourceOwner, &ct->tuple);

	if (
#ifndef CATCACHE_FORCE_RELEASE
		ct->dead &&
#endif
		ct->refcount == 0 &&
		(ct->c_list == NULL || ct->c_list->refcount == 0))
		CatCacheRemoveCTup(ct->my_cache, ct);
}


/*
 *	GetCatCacheHashValue
 *
 *		Compute the hash value for a given set of search keys.
 *
 * The reason for exposing this as part of the API is that the hash value is
 * exposed in cache invalidation operations, so there are places outside the
 * catcache code that need to be able to compute the hash values.
 */
uint32
GetCatCacheHashValue(CatCache *cache,
					 Datum v1,
					 Datum v2,
					 Datum v3,
					 Datum v4)
{
	/*
	 * one-time startup overhead for each cache
	 */
	if (cache->cc_tupdesc == NULL)
		CatalogCacheInitializeCache(cache);

	/*
	 * calculate the hash value
	 */
	return CatalogCacheComputeHashValue(cache, cache->cc_nkeys, v1, v2, v3, v4);
}


/*
 *	SearchCatCacheList
 *
 *		Generate a list of all tuples matching a partial key (that is,
 *		a key specifying just the first K of the cache's N key columns).
 *
 *		It doesn't make any sense to specify all of the cache's key columns
 *		here: since the key is unique, there could be at most one match, so
 *		you ought to use SearchCatCache() instead.  Hence this function takes
 *		one fewer Datum argument than SearchCatCache() does.
 *
 *		The caller must not modify the list object or the pointed-to tuples,
 *		and must call ReleaseCatCacheList() when done with the list.
 */
CatCList *
SearchCatCacheList(CatCache *cache,
				   int nkeys,
				   Datum v1,
				   Datum v2,
				   Datum v3)
{
	Datum		v4 = 0;			/* dummy last-column value */
	Datum		arguments[CATCACHE_MAXKEYS];
	uint32		lHashValue;
	dlist_iter	iter;
	CatCList   *cl;
	CatCTup    *ct;
	List	   *volatile ctlist;
	ListCell   *ctlist_item;
	int			nmembers;
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

	/* Initialize local parameter array */
	arguments[0] = v1;
	arguments[1] = v2;
	arguments[2] = v3;
	arguments[3] = v4;

	/*
	 * compute a hash value of the given keys for faster search.  We don't
	 * presently divide the CatCList items into buckets, but this still lets
	 * us skip non-matching items quickly most of the time.
	 */
	lHashValue = CatalogCacheComputeHashValue(cache, nkeys, v1, v2, v3, v4);

	/*
	 * scan the items until we find a match or exhaust our list
	 *
	 * Note: it's okay to use dlist_foreach here, even though we modify the
	 * dlist within the loop, because we don't continue the loop afterwards.
	 */
	dlist_foreach(iter, &cache->cc_lists)
	{
		cl = dlist_container(CatCList, cache_elem, iter.cur);

		if (cl->dead)
			continue;			/* ignore dead entries */

		if (cl->hash_value != lHashValue)
			continue;			/* quickly skip entry if wrong hash val */

		/*
		 * see if the cached list matches our key.
		 */
		if (cl->nkeys != nkeys)
			continue;

		if (!CatalogCacheCompareTuple(cache, nkeys, cl->keys, arguments))
			continue;

		/*
		 * We found a matching list.  Move the list to the front of the
		 * cache's list-of-lists, to speed subsequent searches.  (We do not
		 * move the members to the fronts of their hashbucket lists, however,
		 * since there's no point in that unless they are searched for
		 * individually.)
		 */
		dlist_move_head(&cache->cc_lists, &cl->cache_elem);

		/* Bump the list's refcount and return it */
		ResourceOwnerEnlargeCatCacheListRefs(CurrentResourceOwner);
		cl->refcount++;
		ResourceOwnerRememberCatCacheListRef(CurrentResourceOwner, cl);

		CACHE_elog(DEBUG2, "SearchCatCacheList(%s): found list",
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
	 *
	 * We have to bump the member refcounts temporarily to ensure they won't
	 * get dropped from the cache while loading other members. We use a PG_TRY
	 * block to ensure we can undo those refcounts if we get an error before
	 * we finish constructing the CatCList.
	 */
	ResourceOwnerEnlargeCatCacheListRefs(CurrentResourceOwner);

	ctlist = NIL;

	PG_TRY();
	{
		ScanKeyData cur_skey[CATCACHE_MAXKEYS];
		Relation	relation;
		SysScanDesc scandesc;

		/*
		 * Ok, need to make a lookup in the relation, copy the scankey and
		 * fill out any per-call fields.
		 */
		memcpy(cur_skey, cache->cc_skey, sizeof(ScanKeyData) * cache->cc_nkeys);
		cur_skey[0].sk_argument = v1;
		cur_skey[1].sk_argument = v2;
		cur_skey[2].sk_argument = v3;
		cur_skey[3].sk_argument = v4;

		relation = table_open(cache->cc_reloid, AccessShareLock);

		scandesc = systable_beginscan(relation,
									  cache->cc_indexoid,
									  IndexScanOK(cache, cur_skey),
									  NULL,
									  nkeys,
									  cur_skey);

		/* The list will be ordered iff we are doing an index scan */
		ordered = (scandesc->irel != NULL);

		while (HeapTupleIsValid(ntp = systable_getnext(scandesc)))
		{
			uint32		hashValue;
			Index		hashIndex;
			bool		found = false;
			dlist_head *bucket;

			/*
			 * See if there's an entry for this tuple already.
			 */
			ct = NULL;
			hashValue = CatalogCacheComputeTupleHashValue(cache, cache->cc_nkeys, ntp);
			hashIndex = HASH_INDEX(hashValue, cache->cc_nbuckets);

			bucket = &cache->cc_bucket[hashIndex];
			dlist_foreach(iter, bucket)
			{
				ct = dlist_container(CatCTup, cache_elem, iter.cur);

				if (ct->dead || ct->negative)
					continue;	/* ignore dead and negative entries */

				if (ct->hash_value != hashValue)
					continue;	/* quickly skip entry if wrong hash val */

				if (!ItemPointerEquals(&(ct->tuple.t_self), &(ntp->t_self)))
					continue;	/* not same tuple */

				/*
				 * Found a match, but can't use it if it belongs to another
				 * list already
				 */
				if (ct->c_list)
					continue;

				found = true;
				break;			/* A-OK */
			}

			if (!found)
			{
				/* We didn't find a usable entry, so make a new one */
				ct = CatalogCacheCreateEntry(cache, ntp, arguments,
											 hashValue, hashIndex,
											 false);
			}

			/* Careful here: add entry to ctlist, then bump its refcount */
			/* This way leaves state correct if lappend runs out of memory */
			ctlist = lappend(ctlist, ct);
			ct->refcount++;
		}

		systable_endscan(scandesc);

		table_close(relation, AccessShareLock);

		/* Now we can build the CatCList entry. */
		oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
		nmembers = list_length(ctlist);
		cl = (CatCList *)
			palloc(offsetof(CatCList, members) + nmembers * sizeof(CatCTup *));

		/* Extract key values */
		CatCacheCopyKeys(cache->cc_tupdesc, nkeys, cache->cc_keyno,
						 arguments, cl->keys);
		MemoryContextSwitchTo(oldcxt);

		/*
		 * We are now past the last thing that could trigger an elog before we
		 * have finished building the CatCList and remembering it in the
		 * resource owner.  So it's OK to fall out of the PG_TRY, and indeed
		 * we'd better do so before we start marking the members as belonging
		 * to the list.
		 */
	}
	PG_CATCH();
	{
		foreach(ctlist_item, ctlist)
		{
			ct = (CatCTup *) lfirst(ctlist_item);
			Assert(ct->c_list == NULL);
			Assert(ct->refcount > 0);
			ct->refcount--;
			if (
#ifndef CATCACHE_FORCE_RELEASE
				ct->dead &&
#endif
				ct->refcount == 0 &&
				(ct->c_list == NULL || ct->c_list->refcount == 0))
				CatCacheRemoveCTup(cache, ct);
		}

		PG_RE_THROW();
	}
	PG_END_TRY();

	cl->cl_magic = CL_MAGIC;
	cl->my_cache = cache;
	cl->refcount = 0;			/* for the moment */
	cl->dead = false;
	cl->ordered = ordered;
	cl->nkeys = nkeys;
	cl->hash_value = lHashValue;
	cl->n_members = nmembers;

	i = 0;
	foreach(ctlist_item, ctlist)
	{
		cl->members[i++] = ct = (CatCTup *) lfirst(ctlist_item);
		Assert(ct->c_list == NULL);
		ct->c_list = cl;
		/* release the temporary refcount on the member */
		Assert(ct->refcount > 0);
		ct->refcount--;
		/* mark list dead if any members already dead */
		if (ct->dead)
			cl->dead = true;
	}
	Assert(i == nmembers);

	dlist_push_head(&cache->cc_lists, &cl->cache_elem);

	/* Finally, bump the list's refcount and return it */
	cl->refcount++;
	ResourceOwnerRememberCatCacheListRef(CurrentResourceOwner, cl);

	CACHE_elog(DEBUG2, "SearchCatCacheList(%s): made list of %d members",
			   cache->cc_relname, nmembers);

	return cl;
}

/*
 *	ReleaseCatCacheList
 *
 *	Decrement the reference count of a catcache list.
 */
void
ReleaseCatCacheList(CatCList *list)
{
	/* Safety checks to ensure we were handed a cache entry */
	Assert(list->cl_magic == CL_MAGIC);
	Assert(list->refcount > 0);
	list->refcount--;
	ResourceOwnerForgetCatCacheListRef(CurrentResourceOwner, list);

	if (
#ifndef CATCACHE_FORCE_RELEASE
		list->dead &&
#endif
		list->refcount == 0)
		CatCacheRemoveCList(list->my_cache, list);
}


/*
 * CatalogCacheCreateEntry
 *		Create a new CatCTup entry, copying the given HeapTuple and other
 *		supplied data into it.  The new entry initially has refcount 0.
 */
static CatCTup *
CatalogCacheCreateEntry(CatCache *cache, HeapTuple ntp, Datum *arguments,
						uint32 hashValue, Index hashIndex,
						bool negative)
{
	CatCTup    *ct;
	HeapTuple	dtp;
	MemoryContext oldcxt;

	/* negative entries have no tuple associated */
	if (ntp)
	{
		int			i;

		Assert(!negative);

		/*
		 * If there are any out-of-line toasted fields in the tuple, expand
		 * them in-line.  This saves cycles during later use of the catcache
		 * entry, and also protects us against the possibility of the toast
		 * tuples being freed before we attempt to fetch them, in case of
		 * something using a slightly stale catcache entry.
		 */
		if (HeapTupleHasExternal(ntp))
			dtp = toast_flatten_tuple(ntp, cache->cc_tupdesc);
		else
			dtp = ntp;

		/* Allocate memory for CatCTup and the cached tuple in one go */
		oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

		ct = (CatCTup *) palloc(sizeof(CatCTup) +
								MAXIMUM_ALIGNOF + dtp->t_len);
		ct->tuple.t_len = dtp->t_len;
		ct->tuple.t_self = dtp->t_self;
		ct->tuple.t_tableOid = dtp->t_tableOid;
		ct->tuple.t_data = (HeapTupleHeader)
			MAXALIGN(((char *) ct) + sizeof(CatCTup));
		/* copy tuple contents */
		memcpy((char *) ct->tuple.t_data,
			   (const char *) dtp->t_data,
			   dtp->t_len);
		MemoryContextSwitchTo(oldcxt);

		if (dtp != ntp)
			heap_freetuple(dtp);

		/* extract keys - they'll point into the tuple if not by-value */
		for (i = 0; i < cache->cc_nkeys; i++)
		{
			Datum		atp;
			bool		isnull;

			atp = heap_getattr(&ct->tuple,
							   cache->cc_keyno[i],
							   cache->cc_tupdesc,
							   &isnull);
			Assert(!isnull);
			ct->keys[i] = atp;
		}
	}
	else
	{
		Assert(negative);
		oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
		ct = (CatCTup *) palloc(sizeof(CatCTup));

		/*
		 * Store keys - they'll point into separately allocated memory if not
		 * by-value.
		 */
		CatCacheCopyKeys(cache->cc_tupdesc, cache->cc_nkeys, cache->cc_keyno,
						 arguments, ct->keys);
		MemoryContextSwitchTo(oldcxt);
	}

	/*
	 * Finish initializing the CatCTup header, and add it to the cache's
	 * linked list and counts.
	 */
	ct->ct_magic = CT_MAGIC;
	ct->my_cache = cache;
	ct->c_list = NULL;
	ct->refcount = 0;			/* for the moment */
	ct->dead = false;
	ct->negative = negative;
	ct->hash_value = hashValue;

	dlist_push_head(&cache->cc_bucket[hashIndex], &ct->cache_elem);

	cache->cc_ntup++;
	CacheHdr->ch_ntup++;

	/*
	 * If the hash table has become too full, enlarge the buckets array. Quite
	 * arbitrarily, we enlarge when fill factor > 2.
	 */
	if (cache->cc_ntup > cache->cc_nbuckets * 2)
		RehashCatCache(cache);

	return ct;
}

/*
 * Helper routine that frees keys stored in the keys array.
 */
static void
CatCacheFreeKeys(TupleDesc tupdesc, int nkeys, int *attnos, Datum *keys)
{
	int			i;

	for (i = 0; i < nkeys; i++)
	{
		int			attnum = attnos[i];
		Form_pg_attribute att;

		/* system attribute are not supported in caches */
		Assert(attnum > 0);

		att = TupleDescAttr(tupdesc, attnum - 1);

		if (!att->attbyval)
			pfree(DatumGetPointer(keys[i]));
	}
}

/*
 * Helper routine that copies the keys in the srckeys array into the dstkeys
 * one, guaranteeing that the datums are fully allocated in the current memory
 * context.
 */
static void
CatCacheCopyKeys(TupleDesc tupdesc, int nkeys, int *attnos,
				 Datum *srckeys, Datum *dstkeys)
{
	int			i;

	/*
	 * XXX: memory and lookup performance could possibly be improved by
	 * storing all keys in one allocation.
	 */

	for (i = 0; i < nkeys; i++)
	{
		int			attnum = attnos[i];
		Form_pg_attribute att = TupleDescAttr(tupdesc, attnum - 1);
		Datum		src = srckeys[i];
		NameData	srcname;

		/*
		 * Must be careful in case the caller passed a C string where a NAME
		 * is wanted: convert the given argument to a correctly padded NAME.
		 * Otherwise the memcpy() done by datumCopy() could fall off the end
		 * of memory.
		 */
		if (att->atttypid == NAMEOID)
		{
			namestrcpy(&srcname, DatumGetCString(src));
			src = NameGetDatum(&srcname);
		}

		dstkeys[i] = datumCopy(src,
							   att->attbyval,
							   att->attlen);
	}
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
 *	correct hash value for each such catcache, and call the specified
 *	function to record the cache id and hash value in inval.c's lists.
 *	SysCacheInvalidate will be called later, if appropriate,
 *	using the recorded information.
 *
 *	For an insert or delete, tuple is the target tuple and newtuple is NULL.
 *	For an update, we are called just once, with tuple being the old tuple
 *	version and newtuple the new version.  We should make two list entries
 *	if the tuple's hash value changed, but only one if it didn't.
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
							  HeapTuple newtuple,
							  void (*function) (int, uint32, Oid))
{
	slist_iter	iter;
	Oid			reloid;

	CACHE_elog(DEBUG2, "PrepareToInvalidateCacheTuple: called");

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
	 *		   compute the tuple's hash value(s) in this cache,
	 *		   and call the passed function to register the information.
	 * ----------------
	 */

	slist_foreach(iter, &CacheHdr->ch_caches)
	{
		CatCache   *ccp = slist_container(CatCache, cc_next, iter.cur);
		uint32		hashvalue;
		Oid			dbid;

		if (ccp->cc_reloid != reloid)
			continue;

		/* Just in case cache hasn't finished initialization yet... */
		if (ccp->cc_tupdesc == NULL)
			CatalogCacheInitializeCache(ccp);

		hashvalue = CatalogCacheComputeTupleHashValue(ccp, ccp->cc_nkeys, tuple);
		dbid = ccp->cc_relisshared ? (Oid) 0 : MyDatabaseId;

		(*function) (ccp->id, hashvalue, dbid);

		if (newtuple)
		{
			uint32		newhashvalue;

			newhashvalue = CatalogCacheComputeTupleHashValue(ccp, ccp->cc_nkeys, newtuple);

			if (newhashvalue != hashvalue)
				(*function) (ccp->id, newhashvalue, dbid);
		}
	}
}


/*
 * Subroutines for warning about reference leaks.  These are exported so
 * that resowner.c can call them.
 */
void
PrintCatCacheLeakWarning(HeapTuple tuple)
{
	CatCTup    *ct = (CatCTup *) (((char *) tuple) -
								  offsetof(CatCTup, tuple));

	/* Safety check to ensure we were handed a cache entry */
	Assert(ct->ct_magic == CT_MAGIC);

	elog(WARNING, "cache reference leak: cache %s (%d), tuple %u/%u has count %d",
		 ct->my_cache->cc_relname, ct->my_cache->id,
		 ItemPointerGetBlockNumber(&(tuple->t_self)),
		 ItemPointerGetOffsetNumber(&(tuple->t_self)),
		 ct->refcount);
}

void
PrintCatCacheListLeakWarning(CatCList *list)
{
	elog(WARNING, "cache reference leak: cache %s (%d), list %p has count %d",
		 list->my_cache->cc_relname, list->my_cache->id,
		 list, list->refcount);
}
