/*-------------------------------------------------------------------------
 *
 * catcache.c--
 *    System catalog cache for tuples matching a key.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/utils/cache/catcache.c,v 1.2 1996/10/13 18:38:51 momjian Exp $
 *
 * Notes:
 *	XXX This needs to use exception.h to handle recovery when
 *		an abort occurs during DisableCache.
 *	
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include "postgres.h"
#include "access/heapam.h"
#include "access/genam.h"
#include "utils/builtins.h"
#include "utils/tqual.h"
#include "storage/bufpage.h"
#include "access/valid.h"
#include "miscadmin.h"
#include "utils/portal.h"
#include "utils/catcache.h"
#include "fmgr.h"		/* for F_BOOLEQ, etc.  DANGER */
#include "utils/elog.h"
#include "utils/palloc.h"
#include "utils/mcxt.h"
#include "utils/rel.h"
#include "catalog/pg_type.h"	/* for OID of int28 type */
#include "lib/dllist.h"

/* ----------------
 *	variables, macros and other stuff
 *
 *  note CCSIZE allocates 51 buckets .. one was already allocated in
 *  the catcache structure.
 * ----------------
 */

#ifdef CACHEDEBUG
#define CACHE1_elog(a,b)		elog(a,b)
#define CACHE2_elog(a,b,c)		elog(a,b,c)
#define CACHE3_elog(a,b,c,d)		elog(a,b,c,d)
#define CACHE4_elog(a,b,c,d,e)		elog(a,b,c,d,e)
#define CACHE5_elog(a,b,c,d,e,f)	elog(a,b,c,d,e,f)
#define CACHE6_elog(a,b,c,d,e,f,g)	elog(a,b,c,d,e,f,g)
#else
#define CACHE1_elog(a,b)
#define CACHE2_elog(a,b,c)
#define CACHE3_elog(a,b,c,d)
#define CACHE4_elog(a,b,c,d,e)
#define CACHE5_elog(a,b,c,d,e,f)
#define CACHE6_elog(a,b,c,d,e,f,g)
#endif

CatCache	*Caches = NULL;
GlobalMemory	CacheCxt;

static int	DisableCache;

/* ----------------
 *	EQPROC is used in CatalogCacheInitializeCache
 * 	XXX this should be replaced by catalog lookups soon
 * ----------------
 */
static long   eqproc[] = {
    F_BOOLEQ, 0l, F_CHAREQ, F_CHAR16EQ, 0l,
    F_INT2EQ, F_KEYFIRSTEQ, F_INT4EQ, 0l, F_TEXTEQ,
    F_OIDEQ, 0l, 0l, 0l, F_OID8EQ
};

#define	EQPROC(SYSTEMTYPEOID)	eqproc[(SYSTEMTYPEOID)-16]

/* ----------------------------------------------------------------
 *		    internal support functions
 * ----------------------------------------------------------------
 */
/* --------------------------------
 *	CatalogCacheInitializeCache
 * --------------------------------
 */
#ifdef CACHEDEBUG
#define CatalogCacheInitializeCache_DEBUG1 \
    elog(DEBUG, "CatalogCacheInitializeCache: cache @%08lx", cache); \
    if (relation) \
        elog(DEBUG, "CatalogCacheInitializeCache: called w/relation(inval)"); \
    else \
        elog(DEBUG, "CatalogCacheInitializeCache: called w/relname %s", \
	    cache->cc_relname)
#define CatalogCacheInitializeCache_DEBUG2 \
        if (cache->cc_key[i] > 0) { \
            elog(DEBUG, "CatalogCacheInitializeCache: load %d/%d w/%d, %d", \
	        i+1, cache->cc_nkeys, cache->cc_key[i], \
	        relation->rd_att->attrs[cache->cc_key[i] - 1]->attlen); \
	} else { \
            elog(DEBUG, "CatalogCacheInitializeCache: load %d/%d w/%d", \
	        i+1, cache->cc_nkeys, cache->cc_key[i]); \
	}
#else
#define CatalogCacheInitializeCache_DEBUG1
#define CatalogCacheInitializeCache_DEBUG2
#endif

void
CatalogCacheInitializeCache(struct catcache *cache,
			    Relation relation)
{
    MemoryContext	oldcxt;
    short 		didopen = 0;
    short 		i;
    TupleDesc		tupdesc;
    
    CatalogCacheInitializeCache_DEBUG1;
    
    /* ----------------
     *	first switch to the cache context so our allocations
     *  do not vanish at the end of a transaction
     * ----------------
     */
    if (!CacheCxt)
	CacheCxt = CreateGlobalMemory("Cache");
    oldcxt = MemoryContextSwitchTo((MemoryContext)CacheCxt);
    
    /* ----------------
     *  If no relation was passed we must open it to get access to 
     *  its fields.  If one of the other caches has already opened
     *  it we use heap_open() instead of heap_openr()
     * ----------------
     */
    if (! RelationIsValid(relation)) {
	struct catcache *cp;
	/* ----------------
	 *  scan the caches to see if any other cache has opened the relation
	 * ----------------
	 */
        for (cp = Caches; cp; cp = cp->cc_next) {
	    if (strncmp(cp->cc_relname, cache->cc_relname, NAMEDATALEN) == 0) {
		if (cp->relationId != InvalidOid)
		    break;
	    }
	}
	
	/* ----------------
	 *  open the relation by name or by id
	 * ----------------
	 */
	if (cp)
    	    relation = heap_open(cp->relationId);
	else
	  {
    	    relation = heap_openr(cache->cc_relname);
          }	

	didopen = 1;
    }
    
    /* ----------------
     *	initialize the cache's relation id
     * ----------------
     */
    Assert(RelationIsValid(relation));
    cache->relationId = RelationGetRelationId(relation);
    tupdesc = cache->cc_tupdesc = RelationGetTupleDescriptor(relation);
    
    CACHE3_elog(DEBUG, "CatalogCacheInitializeCache: relid %d, %d keys", 
		cache->relationId, cache->cc_nkeys);
    
    /* ----------------
     *	initialize cache's key information
     * ----------------
     */
    for (i = 0; i < cache->cc_nkeys; ++i) {
	CatalogCacheInitializeCache_DEBUG2;
	
	if (cache->cc_key[i] > 0) {
	    
	    /*
	     *  Yoiks.  The implementation of the hashing code and the
	     *  implementation of int28's are at loggerheads.  The right
	     *  thing to do is to throw out the implementation of int28's
	     *  altogether; until that happens, we do the right thing here
	     *  to guarantee that the hash key generator doesn't try to
	     *  dereference an int2 by mistake.
	     */
	    
	    if (tupdesc->attrs[cache->cc_key[i]-1]->atttypid == INT28OID)
		cache->cc_klen[i] = sizeof (short);
	    else
		cache->cc_klen[i] = tupdesc->attrs[cache->cc_key[i]-1]->attlen;
	    
	    cache->cc_skey[i].sk_procedure =
	        EQPROC(tupdesc->attrs[cache->cc_key[i]-1]->atttypid);
	    
	    fmgr_info(cache->cc_skey[i].sk_procedure, 
		      (func_ptr *) &cache->cc_skey[i].sk_func,
		      (int *) &cache->cc_skey[i].sk_nargs);
	    
	    CACHE5_elog(DEBUG, "CatalogCacheInit %16s %d %d %x",
			&relation->rd_rel->relname,
			i,
			tupdesc->attrs[ cache->cc_key[i]-1 ]->attlen,
			cache);
	}
    }
    
    /* ----------------
     *	close the relation if we opened it
     * ----------------
     */
    if (didopen)
        heap_close(relation);
    
    /* ----------------
     *	initialize index information for the cache.  this
     *  should only be done once per cache.
     * ----------------
     */
    if (cache->cc_indname != NULL && cache->indexId == InvalidOid)
	{
	    if (RelationGetRelationTupleForm(relation)->relhasindex)
		{
		    /*
		     * If the index doesn't exist we are in trouble.
		     */
		    relation = index_openr( cache->cc_indname);
		    Assert(relation);
		    cache->indexId = RelationGetRelationId(relation);
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
 *	CatalogCacheSetId
 *
 * 	XXX temporary function
 * --------------------------------
 */
void
CatalogCacheSetId(CatCache *cacheInOutP, int id)
{
    Assert(id == InvalidCatalogCacheId || id >= 0);
    cacheInOutP->id = id;
}

/* ----------------
 * comphash --
 *	Compute a hash value, somehow.
 *
 * XXX explain algorithm here.
 *
 * l is length of the attribute value, v
 * v is the attribute value ("Datum")
 * ----------------
 */
long
comphash(long l, register char *v)
{
    long  i;
    NameData n;

    CACHE3_elog(DEBUG, "comphash (%d,%x)", l, v);
    
    switch (l) {
    case 1:
    case 2:
    case 4:
	return((long) v);
    }
    
    if (l == NAMEDATALEN)  {
	/* if it's a name, make sure that the values
	   are null-padded.

	   Note that this other fixed-length types can also have 
	   the same typelen so this may break them     - XXX
	   */
	namestrcpy(&n,v);
	v = n.data;
    } else
	if (l < 0)
	    l = VARSIZE(v);
    
    i = 0;
    while (l--) {
	i += *v++;
    }
    return(i);
}

/* --------------------------------
 *	CatalogCacheComputeHashIndex
 * --------------------------------
 */
Index
CatalogCacheComputeHashIndex(struct catcache *cacheInP)
{
    Index	hashIndex;
    hashIndex = 0x0;
    CACHE6_elog(DEBUG,"CatalogCacheComputeHashIndex %s %d %d %d %x",
		cacheInP->cc_relname,
		cacheInP->cc_nkeys,
		cacheInP->cc_klen[0],
		cacheInP->cc_klen[1],
		cacheInP);
    
    switch (cacheInP->cc_nkeys) {
    case 4:
	hashIndex ^= comphash(cacheInP->cc_klen[3],
			      (char*)cacheInP->cc_skey[3].sk_argument) << 9;
	/* FALLTHROUGH */
    case 3:
	hashIndex ^= comphash(cacheInP->cc_klen[2],
			      (char*)cacheInP->cc_skey[2].sk_argument) << 6;
	/* FALLTHROUGH */
    case 2:
	hashIndex ^= comphash(cacheInP->cc_klen[1],
			      (char*)cacheInP->cc_skey[1].sk_argument) << 3;
	/* FALLTHROUGH */
    case 1:
	hashIndex ^= comphash(cacheInP->cc_klen[0],
			      (char*)cacheInP->cc_skey[0].sk_argument);
	break;
    default:
	elog(FATAL, "CCComputeHashIndex: %d cc_nkeys", cacheInP->cc_nkeys);
	break;
    }
    hashIndex %= cacheInP->cc_size;
    return (hashIndex);
}

/* --------------------------------
 *	CatalogCacheComputeTupleHashIndex
 * --------------------------------
 */
Index
CatalogCacheComputeTupleHashIndex(struct catcache	*cacheInOutP,
				  Relation relation,
				  HeapTuple tuple)
{
    bool		isNull = '\0';
    if (cacheInOutP->relationId == InvalidOid)
	CatalogCacheInitializeCache(cacheInOutP, relation);
    switch (cacheInOutP->cc_nkeys) {
    case 4:
	cacheInOutP->cc_skey[3].sk_argument =
	    (cacheInOutP->cc_key[3] == ObjectIdAttributeNumber)
		? (Datum)tuple->t_oid
		    : (Datum)fastgetattr(tuple,
					 cacheInOutP->cc_key[3],
				 RelationGetTupleDescriptor(relation),
					 &isNull);
	Assert(!isNull);
	/* FALLTHROUGH */
    case 3:
	cacheInOutP->cc_skey[2].sk_argument =
	    (cacheInOutP->cc_key[2] == ObjectIdAttributeNumber)
		? (Datum)tuple->t_oid
		    : (Datum)fastgetattr(tuple,
					 cacheInOutP->cc_key[2],
				 RelationGetTupleDescriptor(relation),
					 &isNull);
	Assert(!isNull);
	/* FALLTHROUGH */
    case 2:
	cacheInOutP->cc_skey[1].sk_argument =
	    (cacheInOutP->cc_key[1] == ObjectIdAttributeNumber)
		? (Datum)tuple->t_oid
		    : (Datum)fastgetattr(tuple,
					 cacheInOutP->cc_key[1],
				RelationGetTupleDescriptor(relation),
					 &isNull);
	Assert(!isNull);
	/* FALLTHROUGH */
    case 1:
	cacheInOutP->cc_skey[0].sk_argument =
	    (cacheInOutP->cc_key[0] == ObjectIdAttributeNumber)
		? (Datum)tuple->t_oid
		    : (Datum)fastgetattr(tuple,
					 cacheInOutP->cc_key[0],
					 RelationGetTupleDescriptor(relation),
					 &isNull);
	Assert(!isNull);
	break;
    default:
	elog(FATAL, "CCComputeTupleHashIndex: %d cc_nkeys",
	     cacheInOutP->cc_nkeys
	     );
	break;
    }
    
    return
	CatalogCacheComputeHashIndex(cacheInOutP);
}

/* --------------------------------
 *	CatCacheRemoveCTup
 * --------------------------------
 */
void
CatCacheRemoveCTup(CatCache *cache, Dlelem *elt)
{
    CatCTup *ct;
    CatCTup *other_ct; 
    Dlelem *other_elt;

    if (elt)
      ct = (CatCTup*) DLE_VAL(elt);
    else
      return;

    other_elt = ct->ct_node;
    other_ct = (CatCTup*)DLE_VAL(other_elt);
    DLRemove(other_elt);
    DLFreeElem(other_elt);
    free(other_ct);
    DLRemove(elt);
    DLFreeElem(elt);
    free(ct);
    --cache->cc_ntup;
}

/* --------------------------------
 *  CatalogCacheIdInvalidate()
 *
 *  Invalidate a tuple given a cache id.  In this case the id should always
 *  be found (whether the cache has opened its relation or not).  Of course,
 *  if the cache has yet to open its relation, there will be no tuples so
 *  no problem.
 * --------------------------------
 */
void
CatalogCacheIdInvalidate(int cacheId, /* XXX */
			 Index hashIndex,
			 ItemPointer pointer)
{
    CatCache 	*ccp;
    CatCTup 	*ct;
    Dlelem      *elt;
    MemoryContext	oldcxt;
    
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
    oldcxt = MemoryContextSwitchTo((MemoryContext)CacheCxt);
    
    /* ----------------
     *	inspect every cache that could contain the tuple
     * ----------------
     */
    for (ccp = Caches; ccp; ccp = ccp->cc_next) {
	if (cacheId != ccp->id) 
    	    continue;
	/* ----------------
	 *  inspect the hash bucket until we find a match or exhaust
	 * ----------------
	 */
	for (elt = DLGetHead(ccp->cc_cache[hashIndex]);
	     elt;
	     elt = DLGetSucc(elt))
	    {
	      ct = (CatCTup*) DLE_VAL(elt);
	      if (ItemPointerEquals(pointer, &ct->ct_tup->t_ctid)) 
		break;
	    }
	
	/* ----------------
	 *  if we found a matching tuple, invalidate it.
	 * ----------------
	 */

	if (elt) {
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
    /*	sendpm('I', "Invalidated tuple"); */
}

/* ----------------------------------------------------------------
 *		       public functions
 *
 *	ResetSystemCache
 *	InitIndexedSysCache
 *	InitSysCache
 *  	SearchSysCache
 *  	RelationInvalidateCatalogCacheTuple
 * ----------------------------------------------------------------
 */
/* --------------------------------
 *	ResetSystemCache
 * --------------------------------
 */
void
ResetSystemCache()
{
    MemoryContext	oldcxt;
    struct catcache	*cache;
    
    /* ----------------
     *	sanity checks
     * ----------------
     */
    CACHE1_elog(DEBUG, "ResetSystemCache called");
    if (DisableCache) {
	elog(WARN, "ResetSystemCache: Called while cache disabled");
	return;
    }
    
    /* ----------------
     *	first switch to the cache context so our allocations
     *  do not vanish at the end of a transaction
     * ----------------
     */
    if (!CacheCxt)
	CacheCxt = CreateGlobalMemory("Cache");
    
    oldcxt = MemoryContextSwitchTo((MemoryContext)CacheCxt);
    
    /* ----------------
     *  here we purge the contents of all the caches
     *
     *	for each system cache
     *     for each hash bucket
     *         for each tuple in hash bucket
     *		   remove the tuple
     * ----------------
     */
    for (cache = Caches; PointerIsValid(cache); cache = cache->cc_next) {
        int hash;
	for (hash = 0; hash < NCCBUCK; hash += 1) {
	    Dlelem *elt, *nextelt;
	    for (elt = DLGetHead(cache->cc_cache[hash]); elt; elt = nextelt) {
		nextelt = DLGetSucc(elt);
		CatCacheRemoveCTup(cache, elt);
	        if (cache->cc_ntup == -1) 
		    elog(WARN, "ResetSystemCache: cc_ntup<0 (software error)");
	    }
	}
	cache->cc_ntup = 0; /* in case of WARN error above */
    }
    
    CACHE1_elog(DEBUG, "end of ResetSystemCache call");
    
    /* ----------------
     *	back to the old context before we return...
     * ----------------
     */
    MemoryContextSwitchTo(oldcxt);
}

/* --------------------------------
 *	InitIndexedSysCache
 *
 *  This allocates and initializes a cache for a system catalog relation.
 *  Actually, the cache is only partially initialized to avoid opening the
 *  relation.  The relation will be opened and the rest of the cache
 *  structure initialized on the first access.
 * --------------------------------
 */
#ifdef CACHEDEBUG
#define InitSysCache_DEBUG1 \
elog(DEBUG, "InitSysCache: rid=%d id=%d nkeys=%d size=%d\n", \
     cp->relationId, cp->id, cp->cc_nkeys, cp->cc_size); \
    for (i = 0; i < nkeys; i += 1) { \
					 elog(DEBUG, "InitSysCache: key=%d len=%d skey=[%d %d %d %d]\n", \
					      cp->cc_key[i], cp->cc_klen[i], \
					      cp->cc_skey[i].sk_flags, \
					      cp->cc_skey[i].sk_attno, \
					      cp->cc_skey[i].sk_procedure, \
					      cp->cc_skey[i].sk_argument); \
					      }
#else
#define InitSysCache_DEBUG1
#endif

CatCache*
InitSysCache(char *relname,
	     char *iname,
	     int id,
	     int nkeys,
	     int key[], 
	     HeapTuple (*iScanfuncP)())
{
    CatCache     *cp;
    register int        i;
    MemoryContext	oldcxt;
    
    char *indname;

    indname = (iname) ? iname : NULL;

    /* ----------------
     *	first switch to the cache context so our allocations
     *  do not vanish at the end of a transaction
     * ----------------
     */
    if (!CacheCxt)
	CacheCxt = CreateGlobalMemory("Cache");
    
    oldcxt = MemoryContextSwitchTo((MemoryContext)CacheCxt);
    
    /* ----------------
     *  allocate a new cache structure
     * ----------------
     */
    cp = (CatCache *)palloc(sizeof(CatCache));
    memset((char*)cp, 0, sizeof(CatCache));

    /* ----------------
     *	initialize the cache buckets (each bucket is a list header)
     *  and the LRU tuple list
     * ----------------
     */
    for (i = 0; i <= NCCBUCK; ++i) {
      cp->cc_cache[i] = DLNewList();
    }
    
    cp->cc_lrulist = DLNewList();
    
    /* ----------------
     *  Caches is the pointer to the head of the list of all the
     *  system caches.  here we add the new cache to the top of the list.
     * ----------------
     */
    cp->cc_next = Caches;  /* list of caches (single link) */
    Caches = cp;
    
    /* ----------------
     *	initialize the cache's relation information for the relation
     *  corresponding to this cache and initialize some of the the new
     *  cache's other internal fields.
     * ----------------
     */
    cp->relationId = 	InvalidOid;
    cp->indexId = 	InvalidOid;
    cp->cc_relname = 	relname;
    cp->cc_indname = 	indname;
    cp->cc_tupdesc =	(TupleDesc) NULL;
    cp->id  = id;
    cp->cc_maxtup = 	MAXTUP;
    cp->cc_size = 	NCCBUCK;
    cp->cc_nkeys = 	nkeys;
    cp->cc_iscanfunc = 	iScanfuncP;
    
    /* ----------------
     *	initialize the cache's key information
     * ----------------
     */
    for (i = 0; i < nkeys; ++i) {
	cp->cc_key[i] = key[i];
	if (!key[i]) {
	    elog(FATAL, "InitSysCache: called with 0 key[%d]", i);
	}
	if (key[i] < 0) {
	    if (key[i] != ObjectIdAttributeNumber) {
		elog(FATAL, "InitSysCache: called with %d key[%d]", key[i], i);
	    } else {
		cp->cc_klen[i] = sizeof(Oid);
		/*
		 * ScanKeyEntryData and struct skey are equivalent. It looks
		 * like a move was made to obsolete struct skey, but it
		 * didn't reach this file.  Someday we should clean up this
		 * code and consolidate to ScanKeyEntry - mer 10 Nov 1991
		 */
		ScanKeyEntryInitialize(&cp->cc_skey[i], 
				       (bits16)0,
				       (AttrNumber)key[i],
				       (RegProcedure)F_OIDEQ,
				       (Datum)0);
		continue;
	    }
	}
	
	cp->cc_skey[i].sk_attno = key[i];
    }
    
    /* ----------------
     *	all done.  new cache is initialized.  print some debugging
     *  information, if appropriate.
     * ----------------
     */
    InitSysCache_DEBUG1;
    
    /* ----------------
     *	back to the old context before we return...
     * ----------------
     */
    MemoryContextSwitchTo(oldcxt);
    return(cp);
}


/* --------------------------------
 *  	SearchSysCache
 *
 *  	This call searches a system cache for a tuple, opening the relation
 *  	if necessary (the first access to a particular cache).
 * --------------------------------
 */
HeapTuple
SearchSysCache(struct catcache *cache,
	       Datum v1,
	       Datum v2,
	       Datum v3,
	       Datum v4)
{
    unsigned	hash;
    CatCTup	*ct; 
    CatCTup	*nct; 
    CatCTup	*nct2; 
    Dlelem      *elt;
    HeapTuple		ntp;
    Buffer		buffer;
    
    Relation		relation;
    MemoryContext	oldcxt;
    
    /* ----------------
     *	sanity checks
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
	  ct = (CatCTup*)DLE_VAL(elt);
	    /* ----------------
	     *  see if the cached tuple matches our key.
	     *  (should we be worried about time ranges? -cim 10/2/90)
	     * ----------------
	     */
	    if (heap_keytest(ct->ct_tup,
			     cache->cc_tupdesc,
			     cache->cc_nkeys,
			     cache->cc_skey))
		break;
	}
    
    /* ----------------
     *	if we found a tuple in the cache, move it to the top of the
     *  lru list, and return it.
     * ----------------
     */
    if (elt) {
        Dlelem* old_lru_elt;
	old_lru_elt = ((CatCTup*)DLE_VAL(elt))->ct_node;
	DLRemove(old_lru_elt);
	DLAddHead(cache->cc_lrulist, old_lru_elt);

#ifdef CACHEDEBUG
	relation = heap_open(cache->relationId);
	CACHE3_elog(DEBUG, "SearchSysCache(%s): found in bucket %d",
		    RelationGetRelationName(relation), hash);
	heap_close(relation);
#endif /* CACHEDEBUG */
	
	return (ct->ct_tup); 
    }
    
    /* ----------------
     *	Tuple was not found in cache, so we have to try and
     *  retrieve it directly from the relation.  If it's found,
     *  we add it to the cache.  We must avoid recursion here,
     *  so we disable cache operations.  If operations are
     *  currently disabled and we couldn't find the requested item
     *  in the cache, then this may be a recursive request, and we
     *  abort with an error.
     * ----------------
     */
    
    if (DisableCache) {
	elog(WARN, "SearchSysCache: Called while cache disabled");
	return((HeapTuple) NULL);
    }
    
    /* ----------------
     *	open the relation associated with the cache
     * ----------------
     */
    relation = heap_open(cache->relationId);
    CACHE2_elog(DEBUG, "SearchSysCache(%s)",
		RelationGetRelationName(relation));
    
    /* ----------------
     *  DisableCache and then switch to the cache memory context.
     * ----------------
     */
    DisableCache = 1;
    
    if (!CacheCxt)
	CacheCxt = CreateGlobalMemory("Cache");
    
    oldcxt = MemoryContextSwitchTo((MemoryContext)CacheCxt);
    
    /* ----------------
     *  Scan the relation to find the tuple.  If there's an index, and
     *  if this isn't bootstrap (initdb) time, use the index.
     * ----------------
     */
    CACHE2_elog(DEBUG, "SearchSysCache: performing scan (override==%d)",
		heapisoverride());
    
    if ((RelationGetRelationTupleForm(relation))->relhasindex
	&& !IsBootstrapProcessingMode())
	{
	    Assert(cache->cc_iscanfunc);
	    switch(cache->cc_nkeys)
		{
		case 4: ntp = cache->cc_iscanfunc(relation,v1,v2,v3,v4); break;
		case 3: ntp = cache->cc_iscanfunc(relation,v1,v2,v3); break;
		case 2: ntp = cache->cc_iscanfunc(relation,v1,v2); break;
		case 1: ntp = cache->cc_iscanfunc(relation,v1); break;
		}
	}
    else
	{
	    HeapScanDesc	sd;
	    
	    sd =  heap_beginscan(relation, 0, NowTimeQual,
				 cache->cc_nkeys, cache->cc_skey);
	    
	    ntp = heap_getnext(sd, 0, &buffer);
	    
	    if (HeapTupleIsValid(ntp)) {
		CACHE1_elog(DEBUG, "SearchSysCache: found tuple");
		ntp = heap_copytuple(ntp);
	    }
	    
	    heap_endscan(sd);
	}
    
    DisableCache = 0;
    
    /* ----------------
     *	scan is complete.  if tup is valid, we copy it and add the copy to
     *  the cache.
     * ----------------
     */
    if (HeapTupleIsValid(ntp)) {
	/* ----------------
	 *  allocate a new cache tuple holder, store the pointer
	 *  to the heap tuple there and initialize the list pointers.
	 * ----------------
	 */
        Dlelem *lru_elt;

	/* this is a little cumbersome here because we want the Dlelem's
	   in both doubly linked lists to point to one another.
	   That makes it easier to remove something from both the cache bucket
	   and the lru list at the same time */
	nct = (CatCTup*) malloc(sizeof(CatCTup));
	nct->ct_tup = ntp;
	elt = DLNewElem(nct);
	nct2 = (CatCTup*) malloc(sizeof(CatCTup));
	nct2->ct_tup = ntp;
	lru_elt = DLNewElem(nct2);
	nct2->ct_node = elt;
	nct->ct_node = lru_elt;
	
	DLAddHead(cache->cc_lrulist, lru_elt);
	DLAddHead(cache->cc_cache[hash], elt);

	/* ----------------
	 *  deal with hash bucket overflow
	 * ----------------
	 */
	if (++cache->cc_ntup > cache->cc_maxtup) {
	    CatCTup *ct;
	    elt = DLGetTail(cache->cc_lrulist);
	    ct = (CatCTup *) DLE_VAL(elt);
	    
	    if (ct != nct) {
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
     *  and return the tuple we found (or NULL)
     * ----------------
     */
    heap_close(relation);
    
    MemoryContextSwitchTo(oldcxt);
    return ntp;
}

/* --------------------------------
 *  RelationInvalidateCatalogCacheTuple()
 *
 *  Invalidate a tuple from a specific relation.  This call determines the
 *  cache in question and calls CatalogCacheIdInvalidate().  It is -ok-
 *  if the relation cannot be found, it simply means this backend has yet
 *  to open it.
 * --------------------------------
 */
void
RelationInvalidateCatalogCacheTuple(Relation relation,
				    HeapTuple tuple,
 				    void (*function)(int, Index, ItemPointer))
{
    struct catcache 	*ccp;
    MemoryContext	oldcxt;
    Oid		relationId;
    
    /* ----------------
     *	sanity checks
     * ----------------
     */
    Assert(RelationIsValid(relation));
    Assert(HeapTupleIsValid(tuple));
    CACHE1_elog(DEBUG, "RelationInvalidateCatalogCacheTuple: called");
    
    /* ----------------
     *	switch to the cache memory context
     * ----------------
     */
    if (!CacheCxt)
	CacheCxt = CreateGlobalMemory("Cache");
    oldcxt = MemoryContextSwitchTo((MemoryContext)CacheCxt);
    
    /* ----------------
     *	for each cache
     *     if the cache contains tuples from the specified relation
     *	       call the invalidation function on the tuples
     *	       in the proper hash bucket
     * ----------------
     */
    relationId = RelationGetRelationId(relation);
    
    for (ccp = Caches; ccp; ccp = ccp->cc_next) {
        if (relationId != ccp->relationId) 
	    continue;
	
	/* OPT inline simplification of CatalogCacheIdInvalidate */
	if (!PointerIsValid(function)) {
	    function = CatalogCacheIdInvalidate;
	}
	
	(*function)(ccp->id,
		    CatalogCacheComputeTupleHashIndex(ccp, relation, tuple),
		    &tuple->t_ctid);
	
	heap_close(relation);
    }
    
    /* ----------------
     *	return to the proper memory context
     * ----------------
     */
    MemoryContextSwitchTo(oldcxt);
    
    /*	sendpm('I', "Invalidated tuple"); */
}

