/*-------------------------------------------------------------------------
 *
 * catcache.h
 *	  Low-level catalog cache definitions.
 *
 * NOTE: every catalog cache must have a corresponding unique index on
 * the system table that it caches --- ie, the index must match the keys
 * used to do lookups in this cache.  All cache fetches are done with
 * indexscans (under normal conditions).  The index should be unique to
 * guarantee that there can only be one matching row for a key combination.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: catcache.h,v 1.27 2000/11/10 00:33:12 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CATCACHE_H
#define CATCACHE_H

/* #define CACHEDEBUG */	/* turns DEBUG elogs on */

#include "access/htup.h"
#include "lib/dllist.h"

/*
 *		struct catctup:			individual tuple in the cache.
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
	int			id;				/* cache identifier --- see syscache.h */
	struct catcache *cc_next;	/* link to next catcache */
	char	   *cc_relname;		/* name of relation the tuples come from */
	char	   *cc_indname;		/* name of index matching cache keys */
	TupleDesc	cc_tupdesc;		/* tuple descriptor (copied from reldesc) */
	short		cc_ntup;		/* # of tuples in this cache */
	short		cc_maxtup;		/* max # of tuples allowed (LRU) */
	short		cc_size;		/* # of hash buckets in this cache */
	short		cc_nkeys;		/* number of keys (1..4) */
	short		cc_key[4];		/* AttrNumber of each key */
	PGFunction	cc_hashfunc[4]; /* hash function to use for each key */
	ScanKeyData cc_skey[4];		/* precomputed key info for indexscans */
	Dllist	   *cc_lrulist;		/* LRU list, most recent first */
	Dllist	   *cc_cache[NCCBUCK + 1];	/* hash buckets */
} CatCache;

#define InvalidCatalogCacheId	(-1)

/* this extern duplicates utils/memutils.h... */
extern MemoryContext CacheMemoryContext;

extern void CreateCacheMemoryContext(void);

extern CatCache *InitSysCache(int id, char *relname, char *indname,
							  int nkeys, int *key);
extern HeapTuple SearchSysCache(CatCache *cache,
								Datum v1, Datum v2,
								Datum v3, Datum v4);

extern void ResetSystemCache(void);
extern void SystemCacheRelationFlushed(Oid relId);
extern void CatalogCacheIdInvalidate(int cacheId, Index hashIndex,
									 ItemPointer pointer);
extern void RelationInvalidateCatalogCacheTuple(Relation relation,
								HeapTuple tuple,
								void (*function) (int, Index, ItemPointer));

#endif	 /* CATCACHE_H */
