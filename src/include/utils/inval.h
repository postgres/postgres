/*-------------------------------------------------------------------------
 *
 * inval.h
 *	  POSTGRES cache invalidation dispatcher definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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
											HeapTuple tuple,
											HeapTuple newtuple);

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
