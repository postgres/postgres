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
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: catcache.h,v 1.33 2001/06/18 03:35:07 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CATCACHE_H
#define CATCACHE_H

#include "access/htup.h"
#include "lib/dllist.h"

/*
 *		struct catctup:			individual tuple in the cache.
 *		struct catcache:		information for managing a cache.
 *		struct catcacheheader:	information for managing all the caches.
 */

typedef struct catcache
{
	int			id;				/* cache identifier --- see syscache.h */
	struct catcache *cc_next;	/* link to next catcache */
	char	   *cc_relname;		/* name of relation the tuples come from */
	char	   *cc_indname;		/* name of index matching cache keys */
	int			cc_reloidattr;	/* AttrNumber of relation OID, or 0 */
	TupleDesc	cc_tupdesc;		/* tuple descriptor (copied from reldesc) */
	int			cc_ntup;		/* # of tuples currently in this cache */
	int			cc_size;		/* # of hash buckets in this cache */
	int			cc_nkeys;		/* number of keys (1..4) */
	int			cc_key[4];		/* AttrNumber of each key */
	PGFunction	cc_hashfunc[4]; /* hash function to use for each key */
	ScanKeyData cc_skey[4];		/* precomputed key info for heap scans */
	Dllist		cc_bucket[1];	/* hash buckets --- VARIABLE LENGTH ARRAY */
} CatCache;						/* VARIABLE LENGTH STRUCT */


typedef struct catctup
{
	int			ct_magic;		/* for Assert checks */
#define CT_MAGIC   0x57261502
	CatCache   *my_cache;		/* link to owning catcache */
	/*
	 * Each tuple in a cache is a member of two lists: one lists all the
	 * elements in all the caches in LRU order, and the other lists just
	 * the elements in one hashbucket of one cache, also in LRU order.
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


typedef struct catcacheheader
{
	CatCache   *ch_caches;		/* head of list of CatCache structs */
	int			ch_ntup;		/* # of tuples in all caches */
	int			ch_maxtup;		/* max # of tuples allowed (LRU) */
	Dllist		ch_lrulist;		/* overall LRU list, most recent first */
} CatCacheHeader;


/* this extern duplicates utils/memutils.h... */
extern MemoryContext CacheMemoryContext;

extern void CreateCacheMemoryContext(void);
extern void AtEOXact_CatCache(bool isCommit);

extern CatCache *InitCatCache(int id, char *relname, char *indname,
							  int reloidattr,
							  int nkeys, int *key);

extern HeapTuple SearchCatCache(CatCache *cache,
			   Datum v1, Datum v2,
			   Datum v3, Datum v4);
extern void ReleaseCatCache(HeapTuple tuple);

extern void ResetCatalogCaches(void);
extern void CatalogCacheFlushRelation(Oid relId);
extern void CatalogCacheIdInvalidate(int cacheId, Index hashIndex,
						 ItemPointer pointer);
extern void PrepareToInvalidateCacheTuple(Relation relation,
							  HeapTuple tuple,
							  void (*function) (int, Index, ItemPointer));

#endif	 /* CATCACHE_H */
