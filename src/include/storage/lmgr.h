/*-------------------------------------------------------------------------
 *
 * lmgr.h
 *	  POSTGRES lock manager definitions.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/storage/lmgr.h,v 1.56.2.1 2008/03/04 19:54:23 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef LMGR_H
#define LMGR_H

#include "storage/lock.h"
#include "utils/rel.h"


extern void RelationInitLockInfo(Relation relation);

/* Lock a relation */
extern void LockRelationOid(Oid relid, LOCKMODE lockmode);
extern bool ConditionalLockRelationOid(Oid relid, LOCKMODE lockmode);
extern void UnlockRelationId(LockRelId *relid, LOCKMODE lockmode);
extern void UnlockRelationOid(Oid relid, LOCKMODE lockmode);

extern void LockRelation(Relation relation, LOCKMODE lockmode);
extern bool ConditionalLockRelation(Relation relation, LOCKMODE lockmode);
extern void UnlockRelation(Relation relation, LOCKMODE lockmode);

extern void LockRelationIdForSession(LockRelId *relid, LOCKMODE lockmode);
extern void UnlockRelationIdForSession(LockRelId *relid, LOCKMODE lockmode);

/* Lock a relation for extension */
extern void LockRelationForExtension(Relation relation, LOCKMODE lockmode);
extern void UnlockRelationForExtension(Relation relation, LOCKMODE lockmode);

/* Lock a page (currently only used within indexes) */
extern void LockPage(Relation relation, BlockNumber blkno, LOCKMODE lockmode);
extern bool ConditionalLockPage(Relation relation, BlockNumber blkno, LOCKMODE lockmode);
extern void UnlockPage(Relation relation, BlockNumber blkno, LOCKMODE lockmode);

/* Lock a tuple (see heap_lock_tuple before assuming you understand this) */
extern void LockTuple(Relation relation, ItemPointer tid, LOCKMODE lockmode);
extern bool ConditionalLockTuple(Relation relation, ItemPointer tid,
					 LOCKMODE lockmode);
extern void UnlockTuple(Relation relation, ItemPointer tid, LOCKMODE lockmode);

/* Lock an XID (used to wait for a transaction to finish) */
extern void XactLockTableInsert(TransactionId xid);
extern void XactLockTableDelete(TransactionId xid);
extern void XactLockTableWait(TransactionId xid);
extern bool ConditionalXactLockTableWait(TransactionId xid);

/* Lock a general object (other than a relation) of the current database */
extern void LockDatabaseObject(Oid classid, Oid objid, uint16 objsubid,
				   LOCKMODE lockmode);
extern void UnlockDatabaseObject(Oid classid, Oid objid, uint16 objsubid,
					 LOCKMODE lockmode);

/* Lock a shared-across-databases object (other than a relation) */
extern void LockSharedObject(Oid classid, Oid objid, uint16 objsubid,
				 LOCKMODE lockmode);
extern void UnlockSharedObject(Oid classid, Oid objid, uint16 objsubid,
				   LOCKMODE lockmode);

#endif   /* LMGR_H */
