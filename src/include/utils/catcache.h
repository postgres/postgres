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
 * $Id: catcache.h,v 1.29 2000/11/24 04:16:11 tgl Exp $
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
	int			ct_magic;		/* for Assert checks */
#define CT_MAGIC   0x57261502

	/*
	 * Each tuple in a cache is a member of two lists: one lists all the
	 * elements in that cache in LRU order, and the other lists just the
	 * elements in one hashbucket, also in LRU order.
	 *
	 * A tuple marked "dead" must not be returned by subsequent searches.
	 * However, it won't be physically deleted from the cache until its
	 * refcount goes to zero.
	 */
	Dlelem		lrulist_elem;	/* list member of global LRU list */
	Dlelem		cache_elem;		/* list member of per-bucket list */
	int			refcount;		/* number of active references */
	bool		dead;			/* dead but not yet removed? */
	HeapTupleData tuple;		/* tuple management header */
} CatCTup;


/* voodoo constants */
#define NCCBUCK 500				/* CatCache buckets */
#define MAXTUP 500				/* Maximum # of tuples stored per cache */


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
	ScanKeyData cc_skey[4];		/* precomputed key info for heap scans */
	Dllist		cc_lrulist;		/* overall LRU list, most recent first */
	Dllist		cc_cache[NCCBUCK]; /* hash buckets */
} CatCache;

#define InvalidCatalogCacheId	(-1)

/* this extern duplicates utils/memutils.h... */
extern MemoryContext CacheMemoryContext;

extern void CreateCacheMemoryContext(void);
extern void AtEOXact_CatCache(bool isCommit);

extern CatCache *InitCatCache(int id, char *relname, char *indname,
							  int nkeys, int *key);

extern HeapTuple SearchCatCache(CatCache *cache,
								Datum v1, Datum v2,
								Datum v3, Datum v4);
extern void ReleaseCatCache(HeapTuple tuple);

extern void ResetSystemCache(void);
extern void SystemCacheRelationFlushed(Oid relId);
extern void CatalogCacheIdInvalidate(int cacheId, Index hashIndex,
									 ItemPointer pointer);
extern void RelationInvalidateCatalogCacheTuple(Relation relation,
								HeapTuple tuple,
								void (*function) (int, Index, ItemPointer));

#endif	 /* CATCACHE_H */
