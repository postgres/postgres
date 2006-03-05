/*-------------------------------------------------------------------------
 *
 * inval.h
 *	  POSTGRES cache invalidation dispatcher definitions.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/utils/inval.h,v 1.38 2006/03/05 15:59:07 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef INVAL_H
#define INVAL_H

#include "access/htup.h"


typedef void (*CacheCallbackFunction) (Datum arg, Oid relid);


extern void AcceptInvalidationMessages(void);

extern void AtStart_Inval(void);

extern void AtSubStart_Inval(void);

extern void AtEOXact_Inval(bool isCommit);

extern void AtEOSubXact_Inval(bool isCommit);

extern void AtPrepare_Inval(void);

extern void PostPrepare_Inval(void);

extern void CommandEndInvalidationMessages(void);

extern void CacheInvalidateHeapTuple(Relation relation, HeapTuple tuple);

extern void CacheInvalidateRelcache(Relation relation);

extern void CacheInvalidateRelcacheByTuple(HeapTuple classTuple);

extern void CacheInvalidateRelcacheByRelid(Oid relid);

extern void CacheRegisterSyscacheCallback(int cacheid,
							  CacheCallbackFunction func,
							  Datum arg);

extern void CacheRegisterRelcacheCallback(CacheCallbackFunction func,
							  Datum arg);

extern void inval_twophase_postcommit(TransactionId xid, uint16 info,
						  void *recdata, uint32 len);

#endif   /* INVAL_H */
