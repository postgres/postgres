/*-------------------------------------------------------------------------
 *
 * catcache.c
 *	  System catalog cache for tuples matching a key.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/cache/catcache.c,v 1.71 2000/11/10 00:33:10 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/hash.h"
#include "access/heapam.h"
#include "access/valid.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/catcache.h"
#include "utils/syscache.h"

static void CatCacheRemoveCTup(CatCache *cache, Dlelem *e);
static Index CatalogCacheComputeHashIndex(CatCache *cache,
										  ScanKey cur_skey);
static Index CatalogCacheComputeTupleHashIndex(CatCache *cache,
											   HeapTuple tuple);
static void CatalogCacheInitializeCache(CatCache *cache);
static Datum cc_hashname(PG_FUNCTION_ARGS);

/* ----------------
 *		variables, macros and other stuff
 * ----------------
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

static CatCache *Caches = NULL; /* head of list of caches */


/* ----------------
 *		EQPROC is used in CatalogCacheInitializeCache to find the equality
 *		functions for system types that are used as cache key fields.
 *		See also GetCCHashFunc, which should support the same set of types.
 *
 *		XXX this should be replaced by catalog lookups,
 *		but that seems to pose considerable risk of circularity...
 * ----------------
 */
static const Oid eqproc[] = {
	F_BOOLEQ, InvalidOid, F_CHAREQ, F_NAMEEQ, InvalidOid,
	F_INT2EQ, F_INT2VECTOREQ, F_INT4EQ, F_OIDEQ, F_TEXTEQ,
	F_OIDEQ, InvalidOid, InvalidOid, InvalidOid, F_OIDVECTOREQ
};

#define EQPROC(SYSTEMTYPEOID)	eqproc[(SYSTEMTYPEOID)-BOOLOID]

/* ----------------------------------------------------------------
 *					internal support functions
 * ----------------------------------------------------------------
 */

static PGFunction
GetCCHashFunc(Oid keytype)
{
	switch (keytype)
	{
		case BOOLOID:
		case CHAROID:
			return hashchar;
		case NAMEOID:
			return cc_hashname;
		case INT2OID:
			return hashint2;
		case INT2VECTOROID:
			return hashint2vector;
		case INT4OID:
			return hashint4;
		case TEXTOID:
			return hashvarlena;
		case REGPROCOID:
		case OIDOID:
			return hashoid;
		case OIDVECTOROID:
			return hashoidvector;
		default:
			elog(FATAL, "GetCCHashFunc: type %u unsupported as catcache key",
				 keytype);
			return (PGFunction) NULL;
	}
}

static Datum
cc_hashname(PG_FUNCTION_ARGS)
{

	/*
	 * We need our own variant of hashname because we want to accept
	 * null-terminated C strings as search values for name fields. So, we
	 * have to make sure the data is correctly padded before we compute
	 * the hash value.
	 */
	NameData	my_n;

	namestrcpy(&my_n, NameStr(* PG_GETARG_NAME(0)));

	return DirectFunctionCall1(hashname, NameGetDatum(&my_n));
}


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
	/* Purely for paranoia, check that context doesn't exist;
	 * caller probably did so already.
	 */
	if (!CacheMemoryContext)
		CacheMemoryContext = AllocSetContextCreate(TopMemoryContext,
												   "CacheMemoryContext",
												   ALLOCSET_DEFAULT_MINSIZE,
												   ALLOCSET_DEFAULT_INITSIZE,
												   ALLOCSET_DEFAULT_MAXSIZE);
}


/* --------------------------------
 *		CatalogCacheInitializeCache
 *
 * This function does final initialization of a catcache: obtain the tuple
 * descriptor and set up the hash and equality function links.  We assume
 * that the relcache entry can be opened at this point!
 * --------------------------------
 */
#ifdef CACHEDEBUG
#define CatalogCacheInitializeCache_DEBUG1 \
	elog(DEBUG, "CatalogCacheInitializeCache: cache @%p %s", cache, \
		 cache->cc_relname)

#define CatalogCacheInitializeCache_DEBUG2 \
do { \
		if (cache->cc_key[i] > 0) { \
			elog(DEBUG, "CatalogCacheInitializeCache: load %d/%d w/%d, %u", \
				i+1, cache->cc_nkeys, cache->cc_key[i], \
				 tupdesc->attrs[cache->cc_key[i] - 1]->atttypid); \
		} else { \
			elog(DEBUG, "CatalogCacheInitializeCache: load %d/%d w/%d", \
				i+1, cache->cc_nkeys, cache->cc_key[i]); \
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
	short		i;

	CatalogCacheInitializeCache_DEBUG1;

	/*
	 * Open the relation without locking --- we only need the tupdesc,
	 * which we assume will never change ...
	 */
	relation = heap_openr(cache->cc_relname, NoLock);
	Assert(RelationIsValid(relation));

	/* ----------------
	 *	switch to the cache context so our allocations
	 *	do not vanish at the end of a transaction
	 * ----------------
	 */
	if (!CacheMemoryContext)
		CreateCacheMemoryContext();

	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	/* ----------------
	 *	copy the relcache's tuple descriptor to permanent cache storage
	 * ----------------
	 */
	tupdesc = CreateTupleDescCopyConstr(RelationGetDescr(relation));

	/* ----------------
	 *	return to the caller's memory context and close the rel
	 * ----------------
	 */
	MemoryContextSwitchTo(oldcxt);

	heap_close(relation, NoLock);

	CACHE3_elog(DEBUG, "CatalogCacheInitializeCache: %s, %d keys",
				cache->cc_relname, cache->cc_nkeys);

	/* ----------------
	 *	initialize cache's key information
	 * ----------------
	 */
	for (i = 0; i < cache->cc_nkeys; ++i)
	{
		Oid			keytype;

		CatalogCacheInitializeCache_DEBUG2;

		if (cache->cc_key[i] > 0)
		{
			keytype = tupdesc->attrs[cache->cc_key[i] - 1]->atttypid;
		}
		else
		{
			if (cache->cc_key[i] != ObjectIdAttributeNumber)
				elog(FATAL, "CatalogCacheInit: only sys attr supported is OID");
			keytype = OIDOID;
		}

		cache->cc_hashfunc[i] = GetCCHashFunc(keytype);
		/*
		 * If GetCCHashFunc liked the type, safe to index into eqproc[]
		 */
		cache->cc_skey[i].sk_procedure = EQPROC(keytype);

		fmgr_info(cache->cc_skey[i].sk_procedure,
				  &cache->cc_skey[i].sk_func);
		cache->cc_skey[i].sk_nargs = cache->cc_skey[i].sk_func.fn_nargs;

		/* Initialize sk_attno suitably for index scans */
		cache->cc_skey[i].sk_attno = i+1;

		CACHE4_elog(DEBUG, "CatalogCacheInit %s %d %p",
					cache->cc_relname,
					i,
					cache);
	}

	/* ----------------
	 *	mark this cache fully initialized
	 * ----------------
	 */
	cache->cc_tupdesc = tupdesc;
}

/* --------------------------------
 *		CatalogCacheComputeHashIndex
 * --------------------------------
 */
static Index
CatalogCacheComputeHashIndex(CatCache *cache, ScanKey cur_skey)
{
	uint32		hashIndex = 0;

	CACHE4_elog(DEBUG, "CatalogCacheComputeHashIndex %s %d %p",
				cache->cc_relname,
				cache->cc_nkeys,
				cache);

	switch (cache->cc_nkeys)
	{
		case 4:
			hashIndex ^=
				DatumGetUInt32(DirectFunctionCall1(cache->cc_hashfunc[3],
							   cur_skey[3].sk_argument)) << 9;
			/* FALLTHROUGH */
		case 3:
			hashIndex ^=
				DatumGetUInt32(DirectFunctionCall1(cache->cc_hashfunc[2],
							   cur_skey[2].sk_argument)) << 6;
			/* FALLTHROUGH */
		case 2:
			hashIndex ^=
				DatumGetUInt32(DirectFunctionCall1(cache->cc_hashfunc[1],
							   cur_skey[1].sk_argument)) << 3;
			/* FALLTHROUGH */
		case 1:
			hashIndex ^=
				DatumGetUInt32(DirectFunctionCall1(cache->cc_hashfunc[0],
							   cur_skey[0].sk_argument));
			break;
		default:
			elog(FATAL, "CCComputeHashIndex: %d cc_nkeys", cache->cc_nkeys);
			break;
	}
	hashIndex %= (uint32) cache->cc_size;
	return (Index) hashIndex;
}

/* --------------------------------
 *		CatalogCacheComputeTupleHashIndex
 * --------------------------------
 */
static Index
CatalogCacheComputeTupleHashIndex(CatCache *cache,
								  HeapTuple tuple)
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
				? ObjectIdGetDatum(tuple->t_data->t_oid)
				: fastgetattr(tuple,
							  cache->cc_key[3],
							  cache->cc_tupdesc,
							  &isNull);
			Assert(!isNull);
			/* FALLTHROUGH */
		case 3:
			cur_skey[2].sk_argument =
				(cache->cc_key[2] == ObjectIdAttributeNumber)
				? ObjectIdGetDatum(tuple->t_data->t_oid)
				: fastgetattr(tuple,
							  cache->cc_key[2],
							  cache->cc_tupdesc,
							  &isNull);
			Assert(!isNull);
			/* FALLTHROUGH */
		case 2:
			cur_skey[1].sk_argument =
				(cache->cc_key[1] == ObjectIdAttributeNumber)
				? ObjectIdGetDatum(tuple->t_data->t_oid)
				: fastgetattr(tuple,
							  cache->cc_key[1],
							  cache->cc_tupdesc,
							  &isNull);
			Assert(!isNull);
			/* FALLTHROUGH */
		case 1:
			cur_skey[0].sk_argument =
				(cache->cc_key[0] == ObjectIdAttributeNumber)
				? ObjectIdGetDatum(tuple->t_data->t_oid)
				: fastgetattr(tuple,
							  cache->cc_key[0],
							  cache->cc_tupdesc,
							  &isNull);
			Assert(!isNull);
			break;
		default:
			elog(FATAL, "CCComputeTupleHashIndex: %d cc_nkeys",
				 cache->cc_nkeys);
			break;
	}

	return CatalogCacheComputeHashIndex(cache, cur_skey);
}

/* --------------------------------
 *		CatCacheRemoveCTup
 * --------------------------------
 */
static void
CatCacheRemoveCTup(CatCache *cache, Dlelem *elt)
{
	CatCTup    *ct;
	CatCTup    *other_ct;
	Dlelem	   *other_elt;

	if (!elt)					/* probably-useless safety check */
		return;

	/* We need to zap both linked-list elements as well as the tuple */

	ct = (CatCTup *) DLE_VAL(elt);
	other_elt = ct->ct_node;
	other_ct = (CatCTup *) DLE_VAL(other_elt);

	heap_freetuple(ct->ct_tup);

	DLRemove(other_elt);
	DLFreeElem(other_elt);
	pfree(other_ct);
	DLRemove(elt);
	DLFreeElem(elt);
	pfree(ct);

	--cache->cc_ntup;
}

/* --------------------------------
 *	CatalogCacheIdInvalidate()
 *
 *	Invalidate a tuple given a cache id.  In this case the id should always
 *	be found (whether the cache has opened its relation or not).  Of course,
 *	if the cache has yet to open its relation, there will be no tuples so
 *	no problem.
 * --------------------------------
 */
void
CatalogCacheIdInvalidate(int cacheId,	/* XXX */
						 Index hashIndex,
						 ItemPointer pointer)
{
	CatCache   *ccp;
	CatCTup    *ct;
	Dlelem	   *elt;

	/* ----------------
	 *	sanity checks
	 * ----------------
	 */
	Assert(hashIndex < NCCBUCK);
	Assert(ItemPointerIsValid(pointer));
	CACHE1_elog(DEBUG, "CatalogCacheIdInvalidate: called");

	/* ----------------
	 *	inspect every cache that could contain the tuple
	 * ----------------
	 */
	for (ccp = Caches; ccp; ccp = ccp->cc_next)
	{
		if (cacheId != ccp->id)
			continue;
		/* ----------------
		 *	inspect the hash bucket until we find a match or exhaust
		 * ----------------
		 */
		for (elt = DLGetHead(ccp->cc_cache[hashIndex]);
			 elt;
			 elt = DLGetSucc(elt))
		{
			ct = (CatCTup *) DLE_VAL(elt);
			if (ItemPointerEquals(pointer, &ct->ct_tup->t_self))
				break;
		}

		/* ----------------
		 *	if we found a matching tuple, invalidate it.
		 * ----------------
		 */

		if (elt)
		{
			CatCacheRemoveCTup(ccp, elt);

			CACHE1_elog(DEBUG, "CatalogCacheIdInvalidate: invalidated");
		}

		if (cacheId != InvalidCatalogCacheId)
			break;
	}
}

/* ----------------------------------------------------------------
 *					   public functions
 *
 *		ResetSystemCache
 *		InitSysCache
 *		SearchSysCache
 *		RelationInvalidateCatalogCacheTuple
 * ----------------------------------------------------------------
 */
/* --------------------------------
 *		ResetSystemCache
 * --------------------------------
 */
void
ResetSystemCache(void)
{
	CatCache *cache;

	CACHE1_elog(DEBUG, "ResetSystemCache called");

	/* ----------------
	 *	here we purge the contents of all the caches
	 *
	 *	for each system cache
	 *	   for each hash bucket
	 *		   for each tuple in hash bucket
	 *			   remove the tuple
	 * ----------------
	 */
	for (cache = Caches; PointerIsValid(cache); cache = cache->cc_next)
	{
		int			hash;

		for (hash = 0; hash < NCCBUCK; hash += 1)
		{
			Dlelem	   *elt,
					   *nextelt;

			for (elt = DLGetHead(cache->cc_cache[hash]); elt; elt = nextelt)
			{
				nextelt = DLGetSucc(elt);
				CatCacheRemoveCTup(cache, elt);
			}
		}

		/* double-check that ntup is now zero */
		if (cache->cc_ntup != 0)
		{
			elog(NOTICE,
				 "ResetSystemCache: cache %d has cc_ntup = %d, should be 0",
				 cache->id, cache->cc_ntup);
			cache->cc_ntup = 0;
		}
	}

	CACHE1_elog(DEBUG, "end of ResetSystemCache call");
}

/* --------------------------------
 *		SystemCacheRelationFlushed
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
 *	to flush all cache entries from that table.  The brute-force method
 *	currently used takes care of that quite handily.  (At one point we
 *	also tried to force re-execution of CatalogCacheInitializeCache for
 *	the cache(s) on that table.  This is a bad idea since it leads to all
 *	kinds of trouble if a cache flush occurs while loading cache entries.
 *	We now avoid the need to do it by copying cc_tupdesc out of the relcache,
 *	rather than relying on the relcache to keep a tupdesc for us.  Of course
 *	this assumes the tupdesc of a cachable system table will not change...)
 * --------------------------------
 */
void
SystemCacheRelationFlushed(Oid relId)
{

	/*
	 * XXX Ideally we'd search the caches and just zap entries that
	 * actually refer to or come from the indicated relation.  For now, we
	 * take the brute-force approach: just flush the caches entirely.
	 */
	ResetSystemCache();
}

/* --------------------------------
 *		InitSysCache
 *
 *	This allocates and initializes a cache for a system catalog relation.
 *	Actually, the cache is only partially initialized to avoid opening the
 *	relation.  The relation will be opened and the rest of the cache
 *	structure initialized on the first access.
 * --------------------------------
 */
#ifdef CACHEDEBUG
#define InitSysCache_DEBUG1 \
do { \
	elog(DEBUG, "InitSysCache: rel=%s id=%d nkeys=%d size=%d\n", \
		cp->cc_relname, cp->id, cp->cc_nkeys, cp->cc_size); \
} while(0)

#else
#define InitSysCache_DEBUG1
#endif

CatCache *
InitSysCache(int id,
			 char *relname,
			 char *indname,
			 int nkeys,
			 int *key)
{
	CatCache   *cp;
	MemoryContext oldcxt;
	int			i;

	/* ----------------
	 *	first switch to the cache context so our allocations
	 *	do not vanish at the end of a transaction
	 * ----------------
	 */
	if (!CacheMemoryContext)
		CreateCacheMemoryContext();

	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	/* ----------------
	 *	allocate a new cache structure
	 * ----------------
	 */
	cp = (CatCache *) palloc(sizeof(CatCache));
	MemSet((char *) cp, 0, sizeof(CatCache));

	/* ----------------
	 *	initialize the cache buckets (each bucket is a list header)
	 *	and the LRU tuple list
	 * ----------------
	 */
	{

		/*
		 * We can only do this optimization because the number of hash
		 * buckets never changes.  Without it, we call palloc() too much.
		 * We could move this to dllist.c, but the way we do this is not
		 * dynamic/portable, so why allow other routines to use it.
		 */
		Dllist	   *cache_begin = palloc((NCCBUCK + 1) * sizeof(Dllist));

		for (i = 0; i <= NCCBUCK; ++i)
		{
			cp->cc_cache[i] = &cache_begin[i];
			cp->cc_cache[i]->dll_head = 0;
			cp->cc_cache[i]->dll_tail = 0;
		}
	}

	cp->cc_lrulist = DLNewList();

	/* ----------------
	 *	Caches is the pointer to the head of the list of all the
	 *	system caches.	here we add the new cache to the top of the list.
	 * ----------------
	 */
	cp->cc_next = Caches;		/* list of caches (single link) */
	Caches = cp;

	/* ----------------
	 *	initialize the cache's relation information for the relation
	 *	corresponding to this cache and initialize some of the the new
	 *	cache's other internal fields.
	 * ----------------
	 */
	cp->cc_relname = relname;
	cp->cc_indname = indname;
	cp->cc_tupdesc = (TupleDesc) NULL;
	cp->id = id;
	cp->cc_maxtup = MAXTUP;
	cp->cc_size = NCCBUCK;
	cp->cc_nkeys = nkeys;
	for (i = 0; i < nkeys; ++i)
		cp->cc_key[i] = key[i];

	/* ----------------
	 *	all done.  new cache is initialized.  print some debugging
	 *	information, if appropriate.
	 * ----------------
	 */
	InitSysCache_DEBUG1;

	/* ----------------
	 *	back to the old context before we return...
	 * ----------------
	 */
	MemoryContextSwitchTo(oldcxt);

	return cp;
}


/* --------------------------------
 *		IndexScanOK
 *
 *		This function checks for tuples that will be fetched by
 *		IndexSupportInitialize() during relcache initialization for
 *		certain system indexes that support critical syscaches.
 *		We can't use an indexscan to fetch these, else we'll get into
 *		infinite recursion.  A plain heap scan will work, however.
 * --------------------------------
 */
static bool
IndexScanOK(CatCache *cache, ScanKey cur_skey)
{
	if (cache->id == INDEXRELID)
	{
		static Oid	indexSelfOid = InvalidOid;

		/* One-time lookup of the OID of pg_index_indexrelid_index */
		if (!OidIsValid(indexSelfOid))
		{
			Relation	rel;
			ScanKeyData key;
			HeapScanDesc sd;
			HeapTuple	ntp;

			rel = heap_openr(RelationRelationName, AccessShareLock);
			ScanKeyEntryInitialize(&key, 0, Anum_pg_class_relname,
								   F_NAMEEQ,
								   PointerGetDatum(IndexRelidIndex));
			sd = heap_beginscan(rel, false, SnapshotNow, 1, &key);
			ntp = heap_getnext(sd, 0);
			if (!HeapTupleIsValid(ntp))
				elog(ERROR, "SearchSelfReferences: %s not found in %s",
					 IndexRelidIndex, RelationRelationName);
			indexSelfOid = ntp->t_data->t_oid;
			heap_endscan(sd);
			heap_close(rel, AccessShareLock);
		}

		/* Looking for pg_index_indexrelid_index? */
		if (DatumGetObjectId(cur_skey[0].sk_argument) == indexSelfOid)
			return false;
	}
	else if (cache->id == OPEROID)
	{
		/* Looking for an OID comparison function? */
		Oid			lookup_oid = DatumGetObjectId(cur_skey[0].sk_argument);

		if (lookup_oid >= MIN_OIDCMP && lookup_oid <= MAX_OIDCMP)
			return false;
	}

	/* Normal case, allow index scan */
	return true;
}

/* --------------------------------
 *		SearchSysCache
 *
 *		This call searches a system cache for a tuple, opening the relation
 *		if necessary (the first access to a particular cache).
 * --------------------------------
 */
HeapTuple
SearchSysCache(CatCache *cache,
			   Datum v1,
			   Datum v2,
			   Datum v3,
			   Datum v4)
{
	ScanKeyData cur_skey[4];
	Index		hash;
	CatCTup    *ct = NULL;
	CatCTup    *nct;
	CatCTup    *nct2;
	Dlelem	   *elt;
	HeapTuple	ntp;
	Relation	relation;
	MemoryContext oldcxt;

	/* ----------------
	 *	one-time startup overhead
	 * ----------------
	 */
	if (cache->cc_tupdesc == NULL)
		CatalogCacheInitializeCache(cache);

	/* ----------------
	 *	initialize the search key information
	 * ----------------
	 */
	memcpy(cur_skey, cache->cc_skey, sizeof(cur_skey));
	cur_skey[0].sk_argument = v1;
	cur_skey[1].sk_argument = v2;
	cur_skey[2].sk_argument = v3;
	cur_skey[3].sk_argument = v4;

	/* ----------------
	 *	find the hash bucket in which to look for the tuple
	 * ----------------
	 */
	hash = CatalogCacheComputeHashIndex(cache, cur_skey);

	/* ----------------
	 *	scan the hash bucket until we find a match or exhaust our tuples
	 * ----------------
	 */
	for (elt = DLGetHead(cache->cc_cache[hash]);
		 elt;
		 elt = DLGetSucc(elt))
	{
		bool		res;

		ct = (CatCTup *) DLE_VAL(elt);
		/* ----------------
		 *	see if the cached tuple matches our key.
		 *	(should we be worried about time ranges? -cim 10/2/90)
		 * ----------------
		 */
		HeapKeyTest(ct->ct_tup,
					cache->cc_tupdesc,
					cache->cc_nkeys,
					cur_skey,
					res);
		if (res)
			break;
	}

	/* ----------------
	 *	if we found a tuple in the cache, move it to the top of the
	 *	lru list, and return it.  We also move it to the front of the
	 *	list for its hashbucket, in order to speed subsequent searches.
	 *	(The most frequently accessed elements in any hashbucket will
	 *	tend to be near the front of the hashbucket's list.)
	 * ----------------
	 */
	if (elt)
	{
		Dlelem	   *old_lru_elt = ((CatCTup *) DLE_VAL(elt))->ct_node;

		DLMoveToFront(old_lru_elt);
		DLMoveToFront(elt);

#ifdef CACHEDEBUG
		CACHE3_elog(DEBUG, "SearchSysCache(%s): found in bucket %d",
					cache->cc_relname, hash);
#endif	 /* CACHEDEBUG */

		return ct->ct_tup;
	}

	/* ----------------
	 *	Tuple was not found in cache, so we have to try and
	 *	retrieve it directly from the relation.  If it's found,
	 *	we add it to the cache.
	 *
	 *	NOTE: it is possible for recursive cache lookups to occur while
	 *	reading the relation --- for example, due to shared-cache-inval
	 *	messages being processed during heap_open().  This is OK.  It's
	 *	even possible for one of those lookups to find and enter the
	 *	very same tuple we are trying to fetch here.  If that happens,
	 *	we will enter a second copy of the tuple into the cache.  The
	 *	first copy will never be referenced again, and will eventually
	 *	age out of the cache, so there's no functional problem.  This case
	 *	is rare enough that it's not worth expending extra cycles to detect.
	 * ----------------
	 */

	/* ----------------
	 *	open the relation associated with the cache
	 * ----------------
	 */
	relation = heap_openr(cache->cc_relname, AccessShareLock);

	/* ----------------
	 *	Scan the relation to find the tuple.  If there's an index, and
	 *	if it's safe to do so, use the index.  Else do a heap scan.
	 * ----------------
	 */
	ntp = NULL;

	if ((RelationGetForm(relation))->relhasindex &&
		!IsIgnoringSystemIndexes() &&
		IndexScanOK(cache, cur_skey))
	{
		Relation	idesc;
		IndexScanDesc isd;
		RetrieveIndexResult indexRes;
		HeapTupleData tuple;
		Buffer		buffer;

		CACHE2_elog(DEBUG, "SearchSysCache(%s): performing index scan",
					cache->cc_relname);

		idesc = index_openr(cache->cc_indname);
		isd = index_beginscan(idesc, false, cache->cc_nkeys, cur_skey);
		tuple.t_datamcxt = CurrentMemoryContext;
		tuple.t_data = NULL;
		while ((indexRes = index_getnext(isd, ForwardScanDirection)))
		{
			tuple.t_self = indexRes->heap_iptr;
			heap_fetch(relation, SnapshotNow, &tuple, &buffer);
			pfree(indexRes);
			if (tuple.t_data != NULL)
			{
				/* Copy tuple into our context */
				oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
				ntp = heap_copytuple(&tuple);
				MemoryContextSwitchTo(oldcxt);
				ReleaseBuffer(buffer);
				break;
			}
		}
		index_endscan(isd);
		index_close(idesc);
	}
	else
	{
		HeapScanDesc sd;
		int			i;

		CACHE2_elog(DEBUG, "SearchSysCache(%s): performing heap scan",
					cache->cc_relname);

		/*
		 * For a heap scan, sk_attno has to be set to the heap attribute
		 * number(s), not the index attribute numbers.
		 */
		for (i = 0; i < cache->cc_nkeys; ++i)
			cur_skey[i].sk_attno = cache->cc_key[i];

		sd = heap_beginscan(relation, 0, SnapshotNow,
							cache->cc_nkeys, cur_skey);

		ntp = heap_getnext(sd, 0);

		if (HeapTupleIsValid(ntp))
		{
			/* Copy tuple into our context */
			oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
			ntp = heap_copytuple(ntp);
			MemoryContextSwitchTo(oldcxt);
			/* We should not free the result of heap_getnext... */
		}

		heap_endscan(sd);
	}

	/* ----------------
	 *	scan is complete.  if tup is valid, we can add it to the cache.
	 *	note we have already copied it into the cache memory context.
	 * ----------------
	 */
	if (HeapTupleIsValid(ntp))
	{
		/* ----------------
		 *	allocate a new cache tuple holder, store the pointer
		 *	to the heap tuple there and initialize the list pointers.
		 * ----------------
		 */
		Dlelem	   *lru_elt;

		CACHE1_elog(DEBUG, "SearchSysCache: found tuple");

		oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

		/*
		 * this is a little cumbersome here because we want the Dlelem's
		 * in both doubly linked lists to point to one another. That makes
		 * it easier to remove something from both the cache bucket and
		 * the lru list at the same time
		 */
		nct = (CatCTup *) palloc(sizeof(CatCTup));
		nct->ct_tup = ntp;
		elt = DLNewElem(nct);
		nct2 = (CatCTup *) palloc(sizeof(CatCTup));
		nct2->ct_tup = ntp;
		lru_elt = DLNewElem(nct2);
		nct2->ct_node = elt;
		nct->ct_node = lru_elt;

		DLAddHead(cache->cc_lrulist, lru_elt);
		DLAddHead(cache->cc_cache[hash], elt);

		MemoryContextSwitchTo(oldcxt);

		/* ----------------
		 *	If we've exceeded the desired size of this cache,
		 *	throw away the least recently used entry.
		 * ----------------
		 */
		if (++cache->cc_ntup > cache->cc_maxtup)
		{
			CatCTup    *ct;

			elt = DLGetTail(cache->cc_lrulist);
			ct = (CatCTup *) DLE_VAL(elt);

			if (ct != nct)		/* shouldn't be possible, but be safe... */
			{
				CACHE2_elog(DEBUG, "SearchSysCache(%s): Overflow, LRU removal",
							cache->cc_relname);

				CatCacheRemoveCTup(cache, elt);
			}
		}

		CACHE4_elog(DEBUG, "SearchSysCache(%s): Contains %d/%d tuples",
					cache->cc_relname, cache->cc_ntup, cache->cc_maxtup);
		CACHE3_elog(DEBUG, "SearchSysCache(%s): put in bucket %d",
					cache->cc_relname, hash);
	}

	/* ----------------
	 *	close the relation and return the tuple we found (or NULL)
	 * ----------------
	 */
	heap_close(relation, AccessShareLock);

	return ntp;
}

/* --------------------------------
 *	RelationInvalidateCatalogCacheTuple()
 *
 *	Invalidate a tuple from a specific relation.  This call determines the
 *	cache in question and calls CatalogCacheIdInvalidate().  It is -ok-
 *	if the relation cannot be found, it simply means this backend has yet
 *	to open it.
 * --------------------------------
 */
void
RelationInvalidateCatalogCacheTuple(Relation relation,
									HeapTuple tuple,
							  void (*function) (int, Index, ItemPointer))
{
	CatCache   *ccp;

	/* ----------------
	 *	sanity checks
	 * ----------------
	 */
	Assert(RelationIsValid(relation));
	Assert(HeapTupleIsValid(tuple));
	Assert(PointerIsValid(function));
	CACHE1_elog(DEBUG, "RelationInvalidateCatalogCacheTuple: called");

	/* ----------------
	 *	for each cache
	 *	   if the cache contains tuples from the specified relation
	 *		   call the invalidation function on the tuples
	 *		   in the proper hash bucket
	 * ----------------
	 */

	for (ccp = Caches; ccp; ccp = ccp->cc_next)
	{
		if (strcmp(ccp->cc_relname, RelationGetRelationName(relation)) != 0)
			continue;

		/* Just in case cache hasn't finished initialization yet... */
		if (ccp->cc_tupdesc == NULL)
			CatalogCacheInitializeCache(ccp);

		(*function) (ccp->id,
					 CatalogCacheComputeTupleHashIndex(ccp, tuple),
					 &tuple->t_self);
	}
}
