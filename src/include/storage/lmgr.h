/*-------------------------------------------------------------------------
 *
 * lmgr.h
 *	  POSTGRES lock manager definitions.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: lmgr.h,v 1.24 2000/01/26 05:58:33 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef LMGR_H
#define LMGR_H

#include "storage/lock.h"
#include "utils/rel.h"

/* These are the valid values of type LOCKMODE: */

/* NoLock is not a lock mode, but a flag value meaning "don't get a lock" */
#define NoLock					0

#define AccessShareLock			1		/* SELECT */
#define RowShareLock			2		/* SELECT FOR UPDATE */
#define RowExclusiveLock		3		/* INSERT, UPDATE, DELETE */
#define ShareLock				4
#define ShareRowExclusiveLock	5
#define ExclusiveLock			6
#define AccessExclusiveLock		7

extern LOCKMETHOD LockTableId;


extern LOCKMETHOD InitLockTable(void);
extern void RelationInitLockInfo(Relation relation);

extern void LockRelation(Relation relation, LOCKMODE lockmode);
extern void UnlockRelation(Relation relation, LOCKMODE lockmode);

/* this is for indices */
extern void LockPage(Relation relation, BlockNumber blkno, LOCKMODE lockmode);
extern void UnlockPage(Relation relation, BlockNumber blkno, LOCKMODE lockmode);

/* and this is for transactions */
extern void XactLockTableInsert(TransactionId xid);
extern void XactLockTableDelete(TransactionId xid);
extern void XactLockTableWait(TransactionId xid);

/* proc.c */
extern void InitProcGlobal(IPCKey key, int maxBackends);

#endif	 /* LMGR_H */
