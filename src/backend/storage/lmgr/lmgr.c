/*-------------------------------------------------------------------------
 *
 * lmgr.c
 *	  POSTGRES lock manager code
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/lmgr/lmgr.c,v 1.44 2001/01/24 19:43:07 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/inval.h"


static LOCKMASK LockConflicts[] = {
	(int) NULL,

/* AccessShareLock */
	(1 << AccessExclusiveLock),

/* RowShareLock */
	(1 << ExclusiveLock) | (1 << AccessExclusiveLock),

/* RowExclusiveLock */
	(1 << ExclusiveLock) | (1 << ShareRowExclusiveLock) | (1 << ShareLock) |
	(1 << AccessExclusiveLock),

/* ShareLock */
	(1 << ExclusiveLock) | (1 << ShareRowExclusiveLock) |
	(1 << RowExclusiveLock) | (1 << AccessExclusiveLock),

/* ShareRowExclusiveLock */
	(1 << ExclusiveLock) | (1 << ShareRowExclusiveLock) |
	(1 << ShareLock) | (1 << RowExclusiveLock) | (1 << AccessExclusiveLock),

/* ExclusiveLock */
	(1 << ExclusiveLock) | (1 << ShareRowExclusiveLock) | (1 << ShareLock) |
	(1 << RowExclusiveLock) | (1 << RowShareLock) | (1 << AccessExclusiveLock),

/* AccessExclusiveLock */
	(1 << ExclusiveLock) | (1 << ShareRowExclusiveLock) | (1 << ShareLock) |
	(1 << RowExclusiveLock) | (1 << RowShareLock) | (1 << AccessExclusiveLock) |
	(1 << AccessShareLock),

};

static int	LockPrios[] = {
	(int) NULL,
	1,
	2,
	3,
	4,
	5,
	6,
	7
};

LOCKMETHOD	LockTableId = (LOCKMETHOD) NULL;
LOCKMETHOD	LongTermTableId = (LOCKMETHOD) NULL;

/*
 * Create the lock table described by LockConflicts and LockPrios.
 */
LOCKMETHOD
InitLockTable(int maxBackends)
{
	int			lockmethod;

	lockmethod = LockMethodTableInit("LockTable",
									 LockConflicts, LockPrios,
									 MAX_LOCKMODES - 1, maxBackends);
	LockTableId = lockmethod;

	if (!(LockTableId))
		elog(ERROR, "InitLockTable: couldn't initialize lock table");

#ifdef USER_LOCKS

	/*
	 * Allocate another tableId for long-term locks
	 */
	LongTermTableId = LockMethodTableRename(LockTableId);
	if (!(LongTermTableId))
		elog(ERROR, "InitLockTable: couldn't rename long-term lock table");
#endif

	return LockTableId;
}

/*
 * RelationInitLockInfo
 *		Initializes the lock information in a relation descriptor.
 *
 *		relcache.c must call this during creation of any reldesc.
 */
void
RelationInitLockInfo(Relation relation)
{
	char	   *relname;

	Assert(RelationIsValid(relation));
	Assert(OidIsValid(RelationGetRelid(relation)));

	relname = (char *) RelationGetPhysicalRelationName(relation);

	relation->rd_lockInfo.lockRelId.relId = RelationGetRelid(relation);

	if (IsSharedSystemRelationName(relname))
		relation->rd_lockInfo.lockRelId.dbId = InvalidOid;
	else
		relation->rd_lockInfo.lockRelId.dbId = MyDatabaseId;
}

/*
 *		LockRelation
 */
void
LockRelation(Relation relation, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	if (LockingDisabled())
		return;

	MemSet(&tag, 0, sizeof(tag));
	tag.relId = relation->rd_lockInfo.lockRelId.relId;
	tag.dbId = relation->rd_lockInfo.lockRelId.dbId;
	tag.objId.blkno = InvalidBlockNumber;

	if (!LockAcquire(LockTableId, &tag, GetCurrentTransactionId(), lockmode))
		elog(ERROR, "LockRelation: LockAcquire failed");

	/*
	 * Check to see if the relcache entry has been invalidated while we
	 * were waiting to lock it.  If so, rebuild it, or elog() trying.
	 * Increment the refcount to ensure that RelationFlushRelation will
	 * rebuild it and not just delete it.
	 */
	RelationIncrementReferenceCount(relation);
	DiscardInvalid();
	RelationDecrementReferenceCount(relation);
}

/*
 *		UnlockRelation
 */
void
UnlockRelation(Relation relation, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	if (LockingDisabled())
		return;

	MemSet(&tag, 0, sizeof(tag));
	tag.relId = relation->rd_lockInfo.lockRelId.relId;
	tag.dbId = relation->rd_lockInfo.lockRelId.dbId;
	tag.objId.blkno = InvalidBlockNumber;

	LockRelease(LockTableId, &tag, GetCurrentTransactionId(), lockmode);
}

/*
 *		LockRelationForSession
 *
 * This routine grabs a session-level lock on the target relation.  The
 * session lock persists across transaction boundaries.  It will be removed
 * when UnlockRelationForSession() is called, or if an elog(ERROR) occurs,
 * or if the backend exits.
 *
 * Note that one should also grab a transaction-level lock on the rel
 * in any transaction that actually uses the rel, to ensure that the
 * relcache entry is up to date.
 */
void
LockRelationForSession(LockRelId *relid, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	if (LockingDisabled())
		return;

	MemSet(&tag, 0, sizeof(tag));
	tag.relId = relid->relId;
	tag.dbId = relid->dbId;
	tag.objId.blkno = InvalidBlockNumber;

	if (!LockAcquire(LockTableId, &tag, InvalidTransactionId, lockmode))
		elog(ERROR, "LockRelationForSession: LockAcquire failed");
}

/*
 *		UnlockRelationForSession
 */
void
UnlockRelationForSession(LockRelId *relid, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	if (LockingDisabled())
		return;

	MemSet(&tag, 0, sizeof(tag));
	tag.relId = relid->relId;
	tag.dbId = relid->dbId;
	tag.objId.blkno = InvalidBlockNumber;

	LockRelease(LockTableId, &tag, InvalidTransactionId, lockmode);
}

/*
 *		LockPage
 */
void
LockPage(Relation relation, BlockNumber blkno, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	if (LockingDisabled())
		return;

	MemSet(&tag, 0, sizeof(tag));
	tag.relId = relation->rd_lockInfo.lockRelId.relId;
	tag.dbId = relation->rd_lockInfo.lockRelId.dbId;
	tag.objId.blkno = blkno;

	if (!LockAcquire(LockTableId, &tag, GetCurrentTransactionId(), lockmode))
		elog(ERROR, "LockPage: LockAcquire failed");
}

/*
 *		UnlockPage
 */
void
UnlockPage(Relation relation, BlockNumber blkno, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	if (LockingDisabled())
		return;

	MemSet(&tag, 0, sizeof(tag));
	tag.relId = relation->rd_lockInfo.lockRelId.relId;
	tag.dbId = relation->rd_lockInfo.lockRelId.dbId;
	tag.objId.blkno = blkno;

	LockRelease(LockTableId, &tag, GetCurrentTransactionId(), lockmode);
}

void
XactLockTableInsert(TransactionId xid)
{
	LOCKTAG		tag;

	if (LockingDisabled())
		return;

	MemSet(&tag, 0, sizeof(tag));
	tag.relId = XactLockTableId;
	tag.dbId = InvalidOid;		/* xids are globally unique */
	tag.objId.xid = xid;

	if (!LockAcquire(LockTableId, &tag, xid, ExclusiveLock))
		elog(ERROR, "XactLockTableInsert: LockAcquire failed");
}

#ifdef NOT_USED
void
XactLockTableDelete(TransactionId xid)
{
	LOCKTAG		tag;

	if (LockingDisabled())
		return;

	MemSet(&tag, 0, sizeof(tag));
	tag.relId = XactLockTableId;
	tag.dbId = InvalidOid;
	tag.objId.xid = xid;

	LockRelease(LockTableId, &tag, xid, ExclusiveLock);
}
#endif

void
XactLockTableWait(TransactionId xid)
{
	LOCKTAG		tag;

	if (LockingDisabled())
		return;

	MemSet(&tag, 0, sizeof(tag));
	tag.relId = XactLockTableId;
	tag.dbId = InvalidOid;
	tag.objId.xid = xid;

	if (!LockAcquire(LockTableId, &tag, GetCurrentTransactionId(), ShareLock))
		elog(ERROR, "XactLockTableWait: LockAcquire failed");

	LockRelease(LockTableId, &tag, GetCurrentTransactionId(), ShareLock);

	/*
	 * Transaction was committed/aborted/crashed - we have to update
	 * pg_log if transaction is still marked as running.
	 */
	if (!TransactionIdDidCommit(xid) && !TransactionIdDidAbort(xid))
		TransactionIdAbort(xid);
}
