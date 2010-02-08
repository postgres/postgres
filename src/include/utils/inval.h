/*-------------------------------------------------------------------------
 *
 * inval.h
 *	  POSTGRES cache invalidation dispatcher definitions.
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/utils/inval.h,v 1.49 2010/02/08 04:33:55 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef INVAL_H
#define INVAL_H

#include "access/htup.h"
#include "storage/relfilenode.h"
#include "utils/relcache.h"


typedef void (*SyscacheCallbackFunction) (Datum arg, int cacheid, ItemPointer tuplePtr);
typedef void (*RelcacheCallbackFunction) (Datum arg, Oid relid);


extern void AcceptInvalidationMessages(void);

extern void AtStart_Inval(void);

extern void AtSubStart_Inval(void);

extern void AtEOXact_Inval(bool isCommit);

extern void AtEOSubXact_Inval(bool isCommit);

extern void AtPrepare_Inval(void);

extern void PostPrepare_Inval(void);

extern void CommandEndInvalidationMessages(void);

extern void CacheInvalidateHeapTuple(Relation relation, HeapTuple tuple);

extern void CacheInvalidateCatalog(Oid catalogId);

extern void CacheInvalidateRelcache(Relation relation);

extern void CacheInvalidateRelcacheByTuple(HeapTuple classTuple);

extern void CacheInvalidateRelcacheByRelid(Oid relid);

extern void CacheInvalidateSmgr(RelFileNode rnode);

extern void CacheInvalidateRelmap(Oid databaseId);

extern void CacheRegisterSyscacheCallback(int cacheid,
							  SyscacheCallbackFunction func,
							  Datum arg);

extern void CacheRegisterRelcacheCallback(RelcacheCallbackFunction func,
							  Datum arg);

extern void CallSyscacheCallbacks(int cacheid, ItemPointer tuplePtr);

extern void inval_twophase_postcommit(TransactionId xid, uint16 info,
						  void *recdata, uint32 len);

#endif   /* INVAL_H */
