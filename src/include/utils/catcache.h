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
 * $Id: catcache.h,v 1.39 2002/03/03 17:47:56 tgl Exp $
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
	Oid			cc_reloid;		/* OID of relation the tuples come from */
	bool		cc_relisshared; /* is relation shared? */
	TupleDesc	cc_tupdesc;		/* tuple descriptor (copied from reldesc) */
	int			cc_reloidattr;	/* AttrNumber of relation OID attr, or 0 */
	int			cc_ntup;		/* # of tuples currently in this cache */
	int			cc_size;		/* # of hash buckets in this cache */
	int			cc_nkeys;		/* number of keys (1..4) */
	int			cc_key[4];		/* AttrNumber of each key */
	PGFunction	cc_hashfunc[4]; /* hash function to use for each key */
	ScanKeyData cc_skey[4];		/* precomputed key info for heap scans */
	bool		cc_isname[4];	/* flag key columns that are NAMEs */
#ifdef CATCACHE_STATS
	long		cc_searches;	/* total # searches against this cache */
	long		cc_hits;		/* # of matches against existing entry */
	long		cc_neg_hits;	/* # of matches against negative entry */
	long		cc_newloads;	/* # of successful loads of new entry */
	/*
	 * cc_searches - (cc_hits + cc_neg_hits + cc_newloads) is number of
	 * failed searches, each of which will result in loading a negative entry
	 */
	long		cc_invals;		/* # of entries invalidated from cache */
	long		cc_discards;	/* # of entries discarded due to overflow */
#endif
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
	 *
	 * A negative cache entry is an assertion that there is no tuple
	 * matching a particular key.  This is just as useful as a normal entry
	 * so far as avoiding catalog searches is concerned.  Management of
	 * positive and negative entries is identical.
	 */
	Dlelem		lrulist_elem;	/* list member of global LRU list */
	Dlelem		cache_elem;		/* list member of per-bucket list */
	int			refcount;		/* number of active references */
	bool		dead;			/* dead but not yet removed? */
	bool		negative;		/* negative cache entry? */
	uint32		hash_value;		/* hash value for this tuple's keys */
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
extern void InitCatCachePhase2(CatCache *cache);

extern HeapTuple SearchCatCache(CatCache *cache,
			   Datum v1, Datum v2,
			   Datum v3, Datum v4);
extern void ReleaseCatCache(HeapTuple tuple);

extern void ResetCatalogCaches(void);
extern void CatalogCacheFlushRelation(Oid relId);
extern void CatalogCacheIdInvalidate(int cacheId, uint32 hashValue,
						 ItemPointer pointer);
extern void PrepareToInvalidateCacheTuple(Relation relation,
							  HeapTuple tuple,
						void (*function) (int, uint32, ItemPointer, Oid));

#endif   /* CATCACHE_H */
