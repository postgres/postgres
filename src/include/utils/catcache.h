/*-------------------------------------------------------------------------
 *
 * catcache.h
 *	  Low-level catalog cache definitions.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: catcache.h,v 1.26 2000/08/06 04:16:40 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CATCACHE_H
#define CATCACHE_H

/* #define CACHEDEBUG */	/* turns DEBUG elogs on */

#include "access/htup.h"
#include "lib/dllist.h"

/*
 * Functions that implement index scans for caches must match this signature
 * (except we allow unused key arguments to be omitted --- is that really
 * portable?)
 */
typedef HeapTuple (*ScanFunc) (Relation heapRelation,
							   Datum key1,
							   Datum key2,
							   Datum key3,
							   Datum key4);

/*
 *		struct catctup:			tuples in the cache.
 *		struct catcache:		information for managing a cache.
 */

typedef struct catctup
{
	HeapTuple	ct_tup;			/* A pointer to a tuple */
	/*
	 * Each tuple in the cache has two catctup items, one in the LRU list
	 * and one in the hashbucket list for its hash value.  ct_node in each
	 * one points to the other one.
	 */
	Dlelem	   *ct_node;		/* the other catctup for this tuple */
} CatCTup;

/* voodoo constants */
#define NCCBUCK 500				/* CatCache buckets */
#define MAXTUP 300				/* Maximum # of tuples stored per cache */


typedef struct catcache
{
	Oid			relationId;
	Oid			indexId;
	char	   *cc_relname;		/* relation name for defered open */
	char	   *cc_indname;		/* index name for defered open */
	ScanFunc	cc_iscanfunc;	/* index scan function */
	TupleDesc	cc_tupdesc;		/* tuple descriptor from reldesc */
	int			id;				/* XXX could be improved -hirohama */
	bool		busy;			/* for detecting recursive lookups */
	short		cc_ntup;		/* # of tuples in this cache	*/
	short		cc_maxtup;		/* max # of tuples allowed (LRU) */
	short		cc_nkeys;
	short		cc_size;
	short		cc_key[4];		/* AttrNumber of each key */
	PGFunction	cc_hashfunc[4]; /* hash function to use for each key */
	ScanKeyData cc_skey[4];
	struct catcache *cc_next;
	Dllist	   *cc_lrulist;		/* LRU list, most recent first */
	Dllist	   *cc_cache[NCCBUCK + 1];	/* hash buckets */
} CatCache;

#define InvalidCatalogCacheId	(-1)

/* this extern duplicates utils/memutils.h... */
extern MemoryContext CacheMemoryContext;

extern void CreateCacheMemoryContext(void);
extern void CatalogCacheIdInvalidate(int cacheId, Index hashIndex,
						 ItemPointer pointer);
extern void ResetSystemCache(void);
extern void SystemCacheRelationFlushed(Oid relId);
extern void SystemCacheAbort(void);
extern CatCache *InitSysCache(char *relname, char *indname, int id,
							  int nkeys, int *key,
							  ScanFunc iScanfuncP);
extern HeapTuple SearchSysCache(CatCache * cache,
								Datum v1, Datum v2,
								Datum v3, Datum v4);
extern void RelationInvalidateCatalogCacheTuple(Relation relation,
								HeapTuple tuple,
								void (*function) (int, Index, ItemPointer));

#endif	 /* CATCACHE_H */
