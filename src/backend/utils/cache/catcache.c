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
 *	  $Header: /cvsroot/pgsql/src/backend/utils/cache/catcache.c,v 1.65 2000/06/05 07:28:53 tgl Exp $
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
static Index CatalogCacheComputeHashIndex(struct catcache * cacheInP);
static Index CatalogCacheComputeTupleHashIndex(struct catcache * cacheInOutP,
								  Relation relation,
								  HeapTuple tuple);
static void CatalogCacheInitializeCache(struct catcache * cache,
							Relation relation);
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

GlobalMemory CacheCxt;			/* context in which caches are allocated */

/* CacheCxt is global because relcache uses it too. */


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
			return hashtext;
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


/* --------------------------------
 *		CatalogCacheInitializeCache
 * --------------------------------
 */
#ifdef CACHEDEBUG
#define CatalogCacheInitializeCache_DEBUG1 \
do { \
	elog(DEBUG, "CatalogCacheInitializeCache: cache @%08lx", cache); \
	if (relation) \
		elog(DEBUG, "CatalogCacheInitializeCache: called w/relation(inval)"); \
	else \
		elog(DEBUG, "CatalogCacheInitializeCache: called w/relname %s", \
			cache->cc_relname) \
} while(0)

#define CatalogCacheInitializeCache_DEBUG2 \
do { \
		if (cache->cc_key[i] > 0) { \
			elog(DEBUG, "CatalogCacheInitializeCache: load %d/%d w/%d, %d", \
				i+1, cache->cc_nkeys, cache->cc_key[i], \
				relation->rd_att->attrs[cache->cc_key[i] - 1]->attlen); \
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
CatalogCacheInitializeCache(struct catcache * cache,
							Relation relation)
{
	MemoryContext oldcxt;
	short		didopen = 0;
	short		i;
	TupleDesc	tupdesc;

	CatalogCacheInitializeCache_DEBUG1;

	/* ----------------
	 *	first switch to the cache context so our allocations
	 *	do not vanish at the end of a transaction
	 * ----------------
	 */
	if (!CacheCxt)
		CacheCxt = CreateGlobalMemory("Cache");
	oldcxt = MemoryContextSwitchTo((MemoryContext) CacheCxt);

	/* ----------------
	 *	If no relation was passed we must open it to get access to
	 *	its fields.  If one of the other caches has already opened
	 *	it we use heap_open() instead of heap_openr().
	 *	XXX is that really worth the trouble of checking?
	 * ----------------
	 */
	if (!RelationIsValid(relation))
	{
		struct catcache *cp;

		/* ----------------
		 *	scan the caches to see if any other cache has opened the relation
		 * ----------------
		 */
		for (cp = Caches; cp; cp = cp->cc_next)
		{
			if (strncmp(cp->cc_relname, cache->cc_relname, NAMEDATALEN) == 0)
			{
				if (cp->relationId != InvalidOid)
					break;
			}
		}

		/* ----------------
		 *	open the relation by name or by id
		 * ----------------
		 */
		if (cp)
			relation = heap_open(cp->relationId, NoLock);
		else
			relation = heap_openr(cache->cc_relname, NoLock);

		didopen = 1;
	}

	/* ----------------
	 *	initialize the cache's relation id and tuple descriptor
	 * ----------------
	 */
	Assert(RelationIsValid(relation));
	cache->relationId = RelationGetRelid(relation);
	tupdesc = CreateTupleDescCopyConstr(RelationGetDescr(relation));
	cache->cc_tupdesc = tupdesc;

	CACHE3_elog(DEBUG, "CatalogCacheInitializeCache: relid %u, %d keys",
				cache->relationId, cache->cc_nkeys);

	/* ----------------
	 *	initialize cache's key information
	 * ----------------
	 */
	for (i = 0; i < cache->cc_nkeys; ++i)
	{
		CatalogCacheInitializeCache_DEBUG2;

		if (cache->cc_key[i] > 0)
		{
			Oid			keytype = tupdesc->attrs[cache->cc_key[i] - 1]->atttypid;

			cache->cc_hashfunc[i] = GetCCHashFunc(keytype);

			/*
			 * If GetCCHashFunc liked the type, safe to index into
			 * eqproc[]
			 */
			cache->cc_skey[i].sk_procedure = EQPROC(keytype);

			fmgr_info(cache->cc_skey[i].sk_procedure,
					  &cache->cc_skey[i].sk_func);
			cache->cc_skey[i].sk_nargs = cache->cc_skey[i].sk_func.fn_nargs;

			CACHE4_elog(DEBUG, "CatalogCacheInit %s %d %x",
						RelationGetRelationName(relation),
						i,
						cache);
		}
	}

	/* ----------------
	 *	close the relation if we opened it
	 * ----------------
	 */
	if (didopen)
		heap_close(relation, NoLock);

	/* ----------------
	 *	initialize index information for the cache.  this
	 *	should only be done once per cache.
	 * ----------------
	 */
	if (cache->cc_indname != NULL && cache->indexId == InvalidOid)
	{
		if (!IsIgnoringSystemIndexes() && RelationGetForm(relation)->relhasindex)
		{

			/*
			 * If the index doesn't exist we are in trouble.
			 */
			relation = index_openr(cache->cc_indname);
			Assert(relation);
			cache->indexId = RelationGetRelid(relation);
			index_close(relation);
		}
		else
			cache->cc_indname = NULL;
	}

	/* ----------------
	 *	return to the proper memory context
	 * ----------------
	 */
	MemoryContextSwitchTo(oldcxt);
}

/* --------------------------------
 *		CatalogCacheComputeHashIndex
 * --------------------------------
 */
static Index
CatalogCacheComputeHashIndex(struct catcache * cacheInP)
{
	uint32		hashIndex = 0;

	CACHE4_elog(DEBUG, "CatalogCacheComputeHashIndex %s %d %x",
				cacheInP->cc_relname,
				cacheInP->cc_nkeys,
				cacheInP);

	switch (cacheInP->cc_nkeys)
	{
		case 4:
			hashIndex ^=
				DatumGetUInt32(DirectFunctionCall1(cacheInP->cc_hashfunc[3],
							   cacheInP->cc_skey[3].sk_argument)) << 9;
			/* FALLTHROUGH */
		case 3:
			hashIndex ^=
				DatumGetUInt32(DirectFunctionCall1(cacheInP->cc_hashfunc[2],
							   cacheInP->cc_skey[2].sk_argument)) << 6;
			/* FALLTHROUGH */
		case 2:
			hashIndex ^=
				DatumGetUInt32(DirectFunctionCall1(cacheInP->cc_hashfunc[1],
							   cacheInP->cc_skey[1].sk_argument)) << 3;
			/* FALLTHROUGH */
		case 1:
			hashIndex ^=
				DatumGetUInt32(DirectFunctionCall1(cacheInP->cc_hashfunc[0],
							   cacheInP->cc_skey[0].sk_argument));
			break;
		default:
			elog(FATAL, "CCComputeHashIndex: %d cc_nkeys", cacheInP->cc_nkeys);
			break;
	}
	hashIndex %= (uint32) cacheInP->cc_size;
	return (Index) hashIndex;
}

/* --------------------------------
 *		CatalogCacheComputeTupleHashIndex
 * --------------------------------
 */
static Index
CatalogCacheComputeTupleHashIndex(struct catcache * cacheInOutP,
								  Relation relation,
								  HeapTuple tuple)
{
	bool		isNull = false;

	/* XXX is this really needed? */
	if (cacheInOutP->relationId == InvalidOid)
		CatalogCacheInitializeCache(cacheInOutP, relation);

	switch (cacheInOutP->cc_nkeys)
	{
		case 4:
			cacheInOutP->cc_skey[3].sk_argument =
				(cacheInOutP->cc_key[3] == ObjectIdAttributeNumber)
				? (Datum) tuple->t_data->t_oid
				: fastgetattr(tuple,
							  cacheInOutP->cc_key[3],
							  RelationGetDescr(relation),
							  &isNull);
			Assert(!isNull);
			/* FALLTHROUGH */
		case 3:
			cacheInOutP->cc_skey[2].sk_argument =
				(cacheInOutP->cc_key[2] == ObjectIdAttributeNumber)
				? (Datum) tuple->t_data->t_oid
				: fastgetattr(tuple,
							  cacheInOutP->cc_key[2],
							  RelationGetDescr(relation),
							  &isNull);
			Assert(!isNull);
			/* FALLTHROUGH */
		case 2:
			cacheInOutP->cc_skey[1].sk_argument =
				(cacheInOutP->cc_key[1] == ObjectIdAttributeNumber)
				? (Datum) tuple->t_data->t_oid
				: fastgetattr(tuple,
							  cacheInOutP->cc_key[1],
							  RelationGetDescr(relation),
							  &isNull);
			Assert(!isNull);
			/* FALLTHROUGH */
		case 1:
			cacheInOutP->cc_skey[0].sk_argument =
				(cacheInOutP->cc_key[0] == ObjectIdAttributeNumber)
				? (Datum) tuple->t_data->t_oid
				: fastgetattr(tuple,
							  cacheInOutP->cc_key[0],
							  RelationGetDescr(relation),
							  &isNull);
			Assert(!isNull);
			break;
		default:
			elog(FATAL, "CCComputeTupleHashIndex: %d cc_nkeys",
				 cacheInOutP->cc_nkeys);
			break;
	}

	return CatalogCacheComputeHashIndex(cacheInOutP);
}

/* --------------------------------
 *		CatCacheRemoveCTup
 *
 *		NB: assumes caller has switched to CacheCxt
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
	MemoryContext oldcxt;

	/* ----------------
	 *	sanity checks
	 * ----------------
	 */
	Assert(hashIndex < NCCBUCK);
	Assert(ItemPointerIsValid(pointer));
	CACHE1_elog(DEBUG, "CatalogCacheIdInvalidate: called");

	/* ----------------
	 *	switch to the cache context for our memory allocations
	 * ----------------
	 */
	if (!CacheCxt)
		CacheCxt = CreateGlobalMemory("Cache");
	oldcxt = MemoryContextSwitchTo((MemoryContext) CacheCxt);

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

	/* ----------------
	 *	return to the proper memory context
	 * ----------------
	 */
	MemoryContextSwitchTo(oldcxt);
}

/* ----------------------------------------------------------------
 *					   public functions
 *
 *		ResetSystemCache
 *		InitIndexedSysCache
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
ResetSystemCache()
{
	MemoryContext oldcxt;
	struct catcache *cache;

	CACHE1_elog(DEBUG, "ResetSystemCache called");

	/* ----------------
	 *	first switch to the cache context so our allocations
	 *	do not vanish at the end of a transaction
	 * ----------------
	 */
	if (!CacheCxt)
		CacheCxt = CreateGlobalMemory("Cache");

	oldcxt = MemoryContextSwitchTo((MemoryContext) CacheCxt);

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
				if (cache->cc_ntup < 0)
					elog(NOTICE,
						 "ResetSystemCache: cc_ntup<0 (software error)");
			}
		}
		cache->cc_ntup = 0;		/* in case of WARN error above */
		cache->busy = false;	/* to recover from recursive-use error */
	}

	CACHE1_elog(DEBUG, "end of ResetSystemCache call");

	/* ----------------
	 *	back to the old context before we return...
	 * ----------------
	 */
	MemoryContextSwitchTo(oldcxt);
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
 *		InitIndexedSysCache
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
	elog(DEBUG, "InitSysCache: rid=%u id=%d nkeys=%d size=%d\n", \
		cp->relationId, cp->id, cp->cc_nkeys, cp->cc_size); \
	for (i = 0; i < nkeys; i += 1) \
	{ \
		elog(DEBUG, "InitSysCache: key=%d skey=[%d %d %d %d]\n", \
			 cp->cc_key[i], \
			 cp->cc_skey[i].sk_flags, \
			 cp->cc_skey[i].sk_attno, \
			 cp->cc_skey[i].sk_procedure, \
			 cp->cc_skey[i].sk_argument); \
	} \
} while(0)

#else
#define InitSysCache_DEBUG1
#endif

CatCache   *
InitSysCache(char *relname,
			 char *iname,
			 int id,
			 int nkeys,
			 int *key,
			 HeapTuple (*iScanfuncP) ())
{
	CatCache   *cp;
	int			i;
	MemoryContext oldcxt;

	char	   *indname;

	indname = (iname) ? iname : NULL;

	/* ----------------
	 *	first switch to the cache context so our allocations
	 *	do not vanish at the end of a transaction
	 * ----------------
	 */
	if (!CacheCxt)
		CacheCxt = CreateGlobalMemory("Cache");

	oldcxt = MemoryContextSwitchTo((MemoryContext) CacheCxt);

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
	cp->relationId = InvalidOid;
	cp->indexId = InvalidOid;
	cp->cc_relname = relname;
	cp->cc_indname = indname;
	cp->cc_tupdesc = (TupleDesc) NULL;
	cp->id = id;
	cp->busy = false;
	cp->cc_maxtup = MAXTUP;
	cp->cc_size = NCCBUCK;
	cp->cc_nkeys = nkeys;
	cp->cc_iscanfunc = iScanfuncP;

	/* ----------------
	 *	partially initialize the cache's key information
	 *	CatalogCacheInitializeCache() will do the rest
	 * ----------------
	 */
	for (i = 0; i < nkeys; ++i)
	{
		cp->cc_key[i] = key[i];
		if (!key[i])
			elog(FATAL, "InitSysCache: called with 0 key[%d]", i);
		if (key[i] < 0)
		{
			if (key[i] != ObjectIdAttributeNumber)
				elog(FATAL, "InitSysCache: called with %d key[%d]", key[i], i);
			else
			{
				cp->cc_hashfunc[i] = GetCCHashFunc(OIDOID);
				ScanKeyEntryInitialize(&cp->cc_skey[i],
									   (bits16) 0,
									   (AttrNumber) key[i],
									   (RegProcedure) F_OIDEQ,
									   (Datum) 0);
				continue;
			}
		}

		cp->cc_skey[i].sk_attno = key[i];
	}

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
 *		SearchSelfReferences
 *
 *		This call searches for self-referencing information,
 *		which causes infinite recursion in the system catalog cache.
 *		This code short-circuits the normal index lookup for cache loads
 *		in those cases and replaces it with a heap scan.
 *
 *		cache should already be initailized
 * --------------------------------
 */
static HeapTuple
SearchSelfReferences(struct catcache * cache)
{
	HeapTuple	ntp;
	Relation	rel;

	if (cache->id == INDEXRELID)
	{
		static Oid	indexSelfOid = InvalidOid;
		static HeapTuple indexSelfTuple = NULL;

		if (!OidIsValid(indexSelfOid))
		{
			ScanKeyData key;
			HeapScanDesc sd;

			/* Find oid of pg_index_indexrelid_index */
			rel = heap_openr(RelationRelationName, AccessShareLock);
			ScanKeyEntryInitialize(&key, 0, Anum_pg_class_relname,
							 F_NAMEEQ, PointerGetDatum(IndexRelidIndex));
			sd = heap_beginscan(rel, false, SnapshotNow, 1, &key);
			ntp = heap_getnext(sd, 0);
			if (!HeapTupleIsValid(ntp))
				elog(ERROR, "SearchSelfReferences: %s not found in %s",
					 IndexRelidIndex, RelationRelationName);
			indexSelfOid = ntp->t_data->t_oid;
			heap_endscan(sd);
			heap_close(rel, AccessShareLock);
		}
		/* Looking for something other than pg_index_indexrelid_index? */
		if ((Oid) cache->cc_skey[0].sk_argument != indexSelfOid)
			return (HeapTuple) 0;

		/* Do we need to load our private copy of the tuple? */
		if (!HeapTupleIsValid(indexSelfTuple))
		{
			HeapScanDesc sd;
			MemoryContext oldcxt;

			if (!CacheCxt)
				CacheCxt = CreateGlobalMemory("Cache");
			rel = heap_open(cache->relationId, AccessShareLock);
			sd = heap_beginscan(rel, false, SnapshotNow, 1, cache->cc_skey);
			ntp = heap_getnext(sd, 0);
			if (!HeapTupleIsValid(ntp))
				elog(ERROR, "SearchSelfReferences: tuple not found");
			oldcxt = MemoryContextSwitchTo((MemoryContext) CacheCxt);
			indexSelfTuple = heap_copytuple(ntp);
			MemoryContextSwitchTo(oldcxt);
			heap_endscan(sd);
			heap_close(rel, AccessShareLock);
		}
		return indexSelfTuple;
	}
	else if (cache->id == OPEROID)
	{
		/* bootstrapping this requires preloading a range of rows. bjm */
		static HeapTuple operatorSelfTuple[MAX_OIDCMP - MIN_OIDCMP + 1];
		Oid			lookup_oid = (Oid) cache->cc_skey[0].sk_argument;

		if (lookup_oid < MIN_OIDCMP || lookup_oid > MAX_OIDCMP)
			return (HeapTuple) 0;

		if (!HeapTupleIsValid(operatorSelfTuple[lookup_oid - MIN_OIDCMP]))
		{
			HeapScanDesc sd;
			MemoryContext oldcxt;

			if (!CacheCxt)
				CacheCxt = CreateGlobalMemory("Cache");
			rel = heap_open(cache->relationId, AccessShareLock);
			sd = heap_beginscan(rel, false, SnapshotNow, 1, cache->cc_skey);
			ntp = heap_getnext(sd, 0);
			if (!HeapTupleIsValid(ntp))
				elog(ERROR, "SearchSelfReferences: tuple not found");
			oldcxt = MemoryContextSwitchTo((MemoryContext) CacheCxt);
			operatorSelfTuple[lookup_oid - MIN_OIDCMP] = heap_copytuple(ntp);
			MemoryContextSwitchTo(oldcxt);
			heap_endscan(sd);
			heap_close(rel, AccessShareLock);
		}
		return operatorSelfTuple[lookup_oid - MIN_OIDCMP];
	}
	else
		return (HeapTuple) 0;

}

/* --------------------------------
 *		SearchSysCache
 *
 *		This call searches a system cache for a tuple, opening the relation
 *		if necessary (the first access to a particular cache).
 * --------------------------------
 */
HeapTuple
SearchSysCache(struct catcache * cache,
			   Datum v1,
			   Datum v2,
			   Datum v3,
			   Datum v4)
{
	unsigned	hash;
	CatCTup    *ct = NULL;
	CatCTup    *nct;
	CatCTup    *nct2;
	Dlelem	   *elt;
	HeapTuple	ntp = NULL;

	Relation	relation;
	MemoryContext oldcxt;

	/* ----------------
	 *	one-time startup overhead
	 * ----------------
	 */
	if (cache->relationId == InvalidOid)
		CatalogCacheInitializeCache(cache, NULL);

	/* ----------------
	 *	initialize the search key information
	 * ----------------
	 */
	cache->cc_skey[0].sk_argument = v1;
	cache->cc_skey[1].sk_argument = v2;
	cache->cc_skey[2].sk_argument = v3;
	cache->cc_skey[3].sk_argument = v4;

	/*
	 * resolve self referencing informtion
	 */
	if ((ntp = SearchSelfReferences(cache)))
		return ntp;

	/* ----------------
	 *	find the hash bucket in which to look for the tuple
	 * ----------------
	 */
	hash = CatalogCacheComputeHashIndex(cache);

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
					cache->cc_skey,
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
	 *	To guard against possible infinite recursion, we mark this cache
	 *	"busy" while trying to load a new entry for it.  It is OK to
	 *	recursively invoke SearchSysCache for a different cache, but
	 *	a recursive call for the same cache will error out.  (We could
	 *	store the specific key(s) being looked for, and consider only
	 *	a recursive request for the same key to be an error, but this
	 *	simple scheme is sufficient for now.)
	 * ----------------
	 */

	if (cache->busy)
		elog(ERROR, "SearchSysCache: recursive use of cache %d", cache->id);
	cache->busy = true;

	/* ----------------
	 *	open the relation associated with the cache
	 * ----------------
	 */
	relation = heap_open(cache->relationId, AccessShareLock);
	CACHE2_elog(DEBUG, "SearchSysCache(%s)",
				RelationGetRelationName(relation));

	/* ----------------
	 *	Switch to the cache memory context.
	 * ----------------
	 */

	if (!CacheCxt)
		CacheCxt = CreateGlobalMemory("Cache");

	oldcxt = MemoryContextSwitchTo((MemoryContext) CacheCxt);

	/* ----------------
	 *	Scan the relation to find the tuple.  If there's an index, and
	 *	if this isn't bootstrap (initdb) time, use the index.
	 * ----------------
	 */
	CACHE1_elog(DEBUG, "SearchSysCache: performing scan");

	if ((RelationGetForm(relation))->relhasindex
		&& !IsIgnoringSystemIndexes())
	{
		/* ----------
		 *	Switch back to old memory context so memory not freed
		 *	in the scan function will go away at transaction end.
		 *	wieck - 10/18/1996
		 * ----------
		 */
		HeapTuple	indextp;

		MemoryContextSwitchTo(oldcxt);
		Assert(cache->cc_iscanfunc);
		switch (cache->cc_nkeys)
		{
			case 4:
				indextp = cache->cc_iscanfunc(relation, v1, v2, v3, v4);
				break;
			case 3:
				indextp = cache->cc_iscanfunc(relation, v1, v2, v3);
				break;
			case 2:
				indextp = cache->cc_iscanfunc(relation, v1, v2);
				break;
			case 1:
				indextp = cache->cc_iscanfunc(relation, v1);
				break;
			default:
				indextp = NULL;
				break;
		}
		/* ----------
		 *	Back to Cache context. If we got a tuple copy it
		 *	into our context.	wieck - 10/18/1996
		 *	And free the tuple that was allocated in the
		 *	transaction's context.   tgl - 02/03/2000
		 * ----------
		 */
		if (HeapTupleIsValid(indextp))
		{
			MemoryContextSwitchTo((MemoryContext) CacheCxt);
			ntp = heap_copytuple(indextp);
			MemoryContextSwitchTo(oldcxt);
			heap_freetuple(indextp);
		}
		MemoryContextSwitchTo((MemoryContext) CacheCxt);
	}
	else
	{
		HeapScanDesc sd;

		/* ----------
		 *	As above do the lookup in the callers memory
		 *	context.
		 *	wieck - 10/18/1996
		 * ----------
		 */
		MemoryContextSwitchTo(oldcxt);

		sd = heap_beginscan(relation, 0, SnapshotNow,
							cache->cc_nkeys, cache->cc_skey);

		ntp = heap_getnext(sd, 0);

		MemoryContextSwitchTo((MemoryContext) CacheCxt);

		if (HeapTupleIsValid(ntp))
		{
			CACHE1_elog(DEBUG, "SearchSysCache: found tuple");
			ntp = heap_copytuple(ntp);
			/* We should not free the result of heap_getnext... */
		}

		MemoryContextSwitchTo(oldcxt);

		heap_endscan(sd);

		MemoryContextSwitchTo((MemoryContext) CacheCxt);
	}

	cache->busy = false;

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
							RelationGetRelationName(relation));

				CatCacheRemoveCTup(cache, elt);
			}
		}

		CACHE4_elog(DEBUG, "SearchSysCache(%s): Contains %d/%d tuples",
					RelationGetRelationName(relation),
					cache->cc_ntup, cache->cc_maxtup);
		CACHE3_elog(DEBUG, "SearchSysCache(%s): put in bucket %d",
					RelationGetRelationName(relation), hash);
	}

	/* ----------------
	 *	close the relation, switch back to the original memory context
	 *	and return the tuple we found (or NULL)
	 * ----------------
	 */
	heap_close(relation, AccessShareLock);

	MemoryContextSwitchTo(oldcxt);

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
	struct catcache *ccp;
	MemoryContext oldcxt;
	Oid			relationId;

	/* ----------------
	 *	sanity checks
	 * ----------------
	 */
	Assert(RelationIsValid(relation));
	Assert(HeapTupleIsValid(tuple));
	Assert(PointerIsValid(function));
	CACHE1_elog(DEBUG, "RelationInvalidateCatalogCacheTuple: called");

	/* ----------------
	 *	switch to the cache memory context
	 * ----------------
	 */
	if (!CacheCxt)
		CacheCxt = CreateGlobalMemory("Cache");
	oldcxt = MemoryContextSwitchTo((MemoryContext) CacheCxt);

	/* ----------------
	 *	for each cache
	 *	   if the cache contains tuples from the specified relation
	 *		   call the invalidation function on the tuples
	 *		   in the proper hash bucket
	 * ----------------
	 */
	relationId = RelationGetRelid(relation);

	for (ccp = Caches; ccp; ccp = ccp->cc_next)
	{
		if (relationId != ccp->relationId)
			continue;

#ifdef NOT_USED
		/* OPT inline simplification of CatalogCacheIdInvalidate */
		if (!PointerIsValid(function))
			function = CatalogCacheIdInvalidate;
#endif

		(*function) (ccp->id,
				 CatalogCacheComputeTupleHashIndex(ccp, relation, tuple),
					 &tuple->t_self);
	}

	/* ----------------
	 *	return to the proper memory context
	 * ----------------
	 */
	MemoryContextSwitchTo(oldcxt);

	/* sendpm('I', "Invalidated tuple"); */
}
