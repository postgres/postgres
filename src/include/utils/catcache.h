/*-------------------------------------------------------------------------
 *
 * catcache.h
 *	  Low-level catalog cache definitions.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: catcache.h,v 1.21 2000/04/12 17:16:54 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CATCACHE_H
#define CATCACHE_H

/* #define		CACHEDEBUG		 turns DEBUG elogs on */

#include "access/htup.h"
#include "lib/dllist.h"

/*
 *		struct catctup:			tuples in the cache.
 *		struct catcache:		information for managing a cache.
 */

typedef struct catctup
{
	HeapTuple	ct_tup;			/* A pointer to a tuple			*/

	/*
	 * Each tuple in the cache has two catctup items, one in the LRU list
	 * and one in the hashbucket list for its hash value.  ct_node in each
	 * one points to the other one.
	 */
	Dlelem	   *ct_node;		/* the other catctup for this tuple */
} CatCTup;

/* voodoo constants */
#define NCCBUCK 500				/* CatCache buckets */
#define MAXTUP 300				/* Maximum # of tuples cached per cache */

typedef uint32 (*CCHashFunc) (Datum);

typedef struct catcache
{
	Oid			relationId;
	Oid			indexId;
	char	   *cc_relname;		/* relation name for defered open */
	char	   *cc_indname;		/* index name for defered open */
	HeapTuple	(*cc_iscanfunc) ();		/* index scanfunction */
	TupleDesc	cc_tupdesc;		/* tuple descriptor from reldesc */
	int			id;				/* XXX could be improved -hirohama */
	bool		busy;			/* for detecting recursive lookups */
	short		cc_ntup;		/* # of tuples in this cache	*/
	short		cc_maxtup;		/* max # of tuples allowed (LRU) */
	short		cc_nkeys;
	short		cc_size;
	short		cc_key[4];		/* AttrNumber of each key */
	CCHashFunc	cc_hashfunc[4]; /* hash function to use for each key */
	ScanKeyData cc_skey[4];
	struct catcache *cc_next;
	Dllist	   *cc_lrulist;		/* LRU list, most recent first */
	Dllist	   *cc_cache[NCCBUCK + 1];	/* hash buckets */
} CatCache;

#define InvalidCatalogCacheId	(-1)

extern GlobalMemory CacheCxt;

extern void CatalogCacheIdInvalidate(int cacheId, Index hashIndex,
						 ItemPointer pointer);
extern void ResetSystemCache(void);
extern void SystemCacheRelationFlushed(Oid relId);
extern CatCache *InitSysCache(char *relname, char *indname, int id, int nkeys,
			 int *key, HeapTuple (*iScanfuncP) ());
extern HeapTuple SearchSysCache(struct catcache * cache, Datum v1, Datum v2,
			   Datum v3, Datum v4);
extern void RelationInvalidateCatalogCacheTuple(Relation relation,
									HeapTuple tuple, void (*function) ());

#endif	 /* CATCACHE_H */
