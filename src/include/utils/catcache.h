/*-------------------------------------------------------------------------
 *
 * catcache.h--
 *    Low-level catalog cache definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: catcache.h,v 1.3 1996/11/04 07:59:39 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	CATCACHE_H
#define CATCACHE_H

/* #define	CACHEDEBUG 	 turns DEBUG elogs on */

#include <utils/rel.h>
#include <nodes/memnodes.h>
#include <lib/dllist.h>
#include <access/htup.h>

/*
 *	struct catctup:		tuples in the cache.
 *	struct catcache:	information for managing a cache.
 */

typedef struct catctup {
    HeapTuple	ct_tup;		/* A pointer to a tuple		*/
    Dlelem     *ct_node; /* points to LRU list is the CatCTup is in the cache,
			    else, points to the cache if the CatCTup is in 
			    LRU list */
} CatCTup;

/* voodoo constants */
#define	NCCBUCK	500	/* CatCache buckets*/
#define MAXTUP 300	/* Maximum # of tuples cached per cache */

typedef struct catcache {
    Oid		relationId;
    Oid		indexId;
    char	*cc_relname; 	/* relation name for defered open */
    char	*cc_indname; 	/* index name for defered open */
    HeapTuple	(*cc_iscanfunc)(); /* index scanfunction */
    TupleDesc 	cc_tupdesc;	/* tuple descriptor from reldesc */
    int		id;		/* XXX could be improved -hirohama */
    short	cc_ntup; 	/* # of tuples in this cache	*/
    short	cc_maxtup; 	/* max # of tuples allowed (LRU)*/
    short	cc_nkeys;
    short	cc_size;
    short	cc_key[4];
    short	cc_klen[4];
    ScanKeyData	cc_skey[4];
    struct catcache *cc_next;
    Dllist  *cc_lrulist;          /* LRU list, most recent first */
    Dllist  *cc_cache[NCCBUCK+1];
} CatCache;

#define	InvalidCatalogCacheId	(-1)

extern struct catcache	*Caches;
extern GlobalMemory	CacheCxt;

extern void CatalogCacheInitializeCache(struct catcache *cache, 
					Relation relation);
extern void CatalogCacheSetId(CatCache *cacheInOutP, int id);
extern long comphash(long l, char *v);
extern Index CatalogCacheComputeHashIndex(struct catcache *cacheInP);
extern Index CatalogCacheComputeTupleHashIndex(struct catcache *cacheInOutP,
				       Relation relation, HeapTuple tuple);
extern void CatCacheRemoveCTup(CatCache *cache, Dlelem *e);  
extern void CatalogCacheIdInvalidate(int cacheId, Index hashIndex, 
				     ItemPointer pointer);
extern void ResetSystemCache(void);
extern CatCache *InitSysCache(char *relname, char *indname, int id, int nkeys, 
			      int key[], HeapTuple (*iScanfuncP)());
extern HeapTuple SearchSysCache(struct catcache *cache, Datum v1, Datum v2,
				Datum v3, Datum v4);
extern void RelationInvalidateCatalogCacheTuple(Relation relation, 
				HeapTuple tuple, void (*function)());

#endif	/* CATCACHE_H */
