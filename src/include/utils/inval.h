/*-------------------------------------------------------------------------
 *
 * inval.h
 *	  POSTGRES cache invalidation dispatcher definitions.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/inval.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef INVAL_H
#define INVAL_H

#include "access/htup.h"
#include "storage/relfilelocator.h"
#include "utils/relcache.h"

extern PGDLLIMPORT int debug_discard_caches;

#define MIN_DEBUG_DISCARD_CACHES 0

#ifdef DISCARD_CACHES_ENABLED
 /* Set default based on older compile-time-only cache clobber macros */
#if defined(CLOBBER_CACHE_RECURSIVELY)
#define DEFAULT_DEBUG_DISCARD_CACHES 3
#elif defined(CLOBBER_CACHE_ALWAYS)
#define DEFAULT_DEBUG_DISCARD_CACHES 1
#else
#define DEFAULT_DEBUG_DISCARD_CACHES 0
#endif
#define MAX_DEBUG_DISCARD_CACHES 5
#else							/* not DISCARD_CACHES_ENABLED */
#define DEFAULT_DEBUG_DISCARD_CACHES 0
#define MAX_DEBUG_DISCARD_CACHES 0
#endif							/* not DISCARD_CACHES_ENABLED */


typedef void (*SyscacheCallbackFunction) (Datum arg, int cacheid, uint32 hashvalue);
typedef void (*RelcacheCallbackFunction) (Datum arg, Oid relid);
typedef void (*RelSyncCallbackFunction) (Datum arg, Oid relid);


extern void AcceptInvalidationMessages(void);

extern void AtEOXact_Inval(bool isCommit);

extern void PreInplace_Inval(void);
extern void AtInplace_Inval(void);
extern void ForgetInplace_Inval(void);

extern void AtEOSubXact_Inval(bool isCommit);

extern void PostPrepare_Inval(void);

extern void CommandEndInvalidationMessages(void);

extern void CacheInvalidateHeapTuple(Relation relation,
									 HeapTuple tuple,
									 HeapTuple newtuple);
extern void CacheInvalidateHeapTupleInplace(Relation relation,
											HeapTuple key_equivalent_tuple);

extern void CacheInvalidateCatalog(Oid catalogId);

extern void CacheInvalidateRelcache(Relation relation);

extern void CacheInvalidateRelcacheAll(void);

extern void CacheInvalidateRelcacheByTuple(HeapTuple classTuple);

extern void CacheInvalidateRelcacheByRelid(Oid relid);

extern void CacheInvalidateRelSync(Oid relid);

extern void CacheInvalidateRelSyncAll(void);

extern void CacheInvalidateSmgr(RelFileLocatorBackend rlocator);

extern void CacheInvalidateRelmap(Oid databaseId);

extern void CacheRegisterSyscacheCallback(int cacheid,
										  SyscacheCallbackFunction func,
										  Datum arg);

extern void CacheRegisterRelcacheCallback(RelcacheCallbackFunction func,
										  Datum arg);

extern void CacheRegisterRelSyncCallback(RelSyncCallbackFunction func,
										 Datum arg);

extern void CallSyscacheCallbacks(int cacheid, uint32 hashvalue);

extern void CallRelSyncCallbacks(Oid relid);

extern void InvalidateSystemCaches(void);
extern void InvalidateSystemCachesExtended(bool debug_discard);

extern void LogLogicalInvalidations(void);
#endif							/* INVAL_H */
