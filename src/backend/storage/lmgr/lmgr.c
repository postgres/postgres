/*-------------------------------------------------------------------------
 *
 * lmgr.c
 *	  POSTGRES lock manager code
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/lmgr/lmgr.c,v 1.60 2003/09/04 22:06:27 tgl Exp $
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
	0,

	/* AccessShareLock */
	(1 << AccessExclusiveLock),

	/* RowShareLock */
	(1 << ExclusiveLock) | (1 << AccessExclusiveLock),

	/* RowExclusiveLock */
	(1 << ShareLock) | (1 << ShareRowExclusiveLock) |
	(1 << ExclusiveLock) | (1 << AccessExclusiveLock),

	/* ShareUpdateExclusiveLock */
	(1 << ShareUpdateExclusiveLock) |
	(1 << ShareLock) | (1 << ShareRowExclusiveLock) |
	(1 << ExclusiveLock) | (1 << AccessExclusiveLock),

	/* ShareLock */
	(1 << RowExclusiveLock) | (1 << ShareUpdateExclusiveLock) |
	(1 << ShareRowExclusiveLock) |
	(1 << ExclusiveLock) | (1 << AccessExclusiveLock),

	/* ShareRowExclusiveLock */
	(1 << RowExclusiveLock) | (1 << ShareUpdateExclusiveLock) |
	(1 << ShareLock) | (1 << ShareRowExclusiveLock) |
	(1 << ExclusiveLock) | (1 << AccessExclusiveLock),

	/* ExclusiveLock */
	(1 << RowShareLock) |
	(1 << RowExclusiveLock) | (1 << ShareUpdateExclusiveLock) |
	(1 << ShareLock) | (1 << ShareRowExclusiveLock) |
	(1 << ExclusiveLock) | (1 << AccessExclusiveLock),

	/* AccessExclusiveLock */
	(1 << AccessShareLock) | (1 << RowShareLock) |
	(1 << RowExclusiveLock) | (1 << ShareUpdateExclusiveLock) |
	(1 << ShareLock) | (1 << ShareRowExclusiveLock) |
	(1 << ExclusiveLock) | (1 << AccessExclusiveLock)

};

LOCKMETHOD	LockTableId = (LOCKMETHOD) NULL;
LOCKMETHOD	LongTermTableId = (LOCKMETHOD) NULL;

/*
 * Create the lock table described by LockConflicts
 */
LOCKMETHOD
InitLockTable(int maxBackends)
{
	int			lockmethod;

	/* number of lock modes is lengthof()-1 because of dummy zero */
	lockmethod = LockMethodTableInit("LockTable",
									 LockConflicts,
									 lengthof(LockConflicts) - 1,
									 maxBackends);
	LockTableId = lockmethod;

	if (!(LockTableId))
		elog(ERROR, "could not initialize lock table");

#ifdef USER_LOCKS

	/*
	 * Allocate another tableId for long-term locks
	 */
	LongTermTableId = LockMethodTableRename(LockTableId);
	if (!(LongTermTableId))
		elog(ERROR, "could not rename long-term lock table");
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
	Assert(RelationIsValid(relation));
	Assert(OidIsValid(RelationGetRelid(relation)));

	relation->rd_lockInfo.lockRelId.relId = RelationGetRelid(relation);

	if (relation->rd_rel->relisshared)
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

	MemSet(&tag, 0, sizeof(tag));
	tag.relId = relation->rd_lockInfo.lockRelId.relId;
	tag.dbId = relation->rd_lockInfo.lockRelId.dbId;
	tag.objId.blkno = InvalidBlockNumber;

	if (!LockAcquire(LockTableId, &tag, GetCurrentTransactionId(),
					 lockmode, false))
		elog(ERROR, "LockAcquire failed");

	/*
	 * Check to see if the relcache entry has been invalidated while we
	 * were waiting to lock it.  If so, rebuild it, or ereport() trying.
	 * Increment the refcount to ensure that RelationFlushRelation will
	 * rebuild it and not just delete it.
	 */
	RelationIncrementReferenceCount(relation);
	AcceptInvalidationMessages();
	RelationDecrementReferenceCount(relation);
}

/*
 *		ConditionalLockRelation
 *
 * As above, but only lock if we can get the lock without blocking.
 * Returns TRUE iff the lock was acquired.
 *
 * NOTE: we do not currently need conditional versions of all the
 * LockXXX routines in this file, but they could easily be added if needed.
 */
bool
ConditionalLockRelation(Relation relation, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	MemSet(&tag, 0, sizeof(tag));
	tag.relId = relation->rd_lockInfo.lockRelId.relId;
	tag.dbId = relation->rd_lockInfo.lockRelId.dbId;
	tag.objId.blkno = InvalidBlockNumber;

	if (!LockAcquire(LockTableId, &tag, GetCurrentTransactionId(),
					 lockmode, true))
		return false;

	/*
	 * Check to see if the relcache entry has been invalidated while we
	 * were waiting to lock it.  If so, rebuild it, or ereport() trying.
	 * Increment the refcount to ensure that RelationFlushRelation will
	 * rebuild it and not just delete it.
	 */
	RelationIncrementReferenceCount(relation);
	AcceptInvalidationMessages();
	RelationDecrementReferenceCount(relation);

	return true;
}

/*
 *		UnlockRelation
 */
void
UnlockRelation(Relation relation, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	MemSet(&tag, 0, sizeof(tag));
	tag.relId = relation->rd_lockInfo.lockRelId.relId;
	tag.dbId = relation->rd_lockInfo.lockRelId.dbId;
	tag.objId.blkno = InvalidBlockNumber;

	LockRelease(LockTableId, &tag, GetCurrentTransactionId(), lockmode);
}

/*
 *		LockRelationForSession
 *
 * This routine grabs a session-level lock on the target relation.	The
 * session lock persists across transaction boundaries.  It will be removed
 * when UnlockRelationForSession() is called, or if an ereport(ERROR) occurs,
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

	MemSet(&tag, 0, sizeof(tag));
	tag.relId = relid->relId;
	tag.dbId = relid->dbId;
	tag.objId.blkno = InvalidBlockNumber;

	if (!LockAcquire(LockTableId, &tag, InvalidTransactionId,
					 lockmode, false))
		elog(ERROR, "LockAcquire failed");
}

/*
 *		UnlockRelationForSession
 */
void
UnlockRelationForSession(LockRelId *relid, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	MemSet(&tag, 0, sizeof(tag));
	tag.relId = relid->relId;
	tag.dbId = relid->dbId;
	tag.objId.blkno = InvalidBlockNumber;

	LockRelease(LockTableId, &tag, InvalidTransactionId, lockmode);
}

/*
 *		LockPage
 *
 * Obtain a page-level lock.  This is currently used by some index access
 * methods to lock index pages.  For heap relations, it is used only with
 * blkno == 0 to signify locking the relation for extension.
 */
void
LockPage(Relation relation, BlockNumber blkno, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	MemSet(&tag, 0, sizeof(tag));
	tag.relId = relation->rd_lockInfo.lockRelId.relId;
	tag.dbId = relation->rd_lockInfo.lockRelId.dbId;
	tag.objId.blkno = blkno;

	if (!LockAcquire(LockTableId, &tag, GetCurrentTransactionId(),
					 lockmode, false))
		elog(ERROR, "LockAcquire failed");
}

/*
 *		ConditionalLockPage
 *
 * As above, but only lock if we can get the lock without blocking.
 * Returns TRUE iff the lock was acquired.
 */
bool
ConditionalLockPage(Relation relation, BlockNumber blkno, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	MemSet(&tag, 0, sizeof(tag));
	tag.relId = relation->rd_lockInfo.lockRelId.relId;
	tag.dbId = relation->rd_lockInfo.lockRelId.dbId;
	tag.objId.blkno = blkno;

	return LockAcquire(LockTableId, &tag, GetCurrentTransactionId(),
					   lockmode, true);
}

/*
 *		UnlockPage
 */
void
UnlockPage(Relation relation, BlockNumber blkno, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	MemSet(&tag, 0, sizeof(tag));
	tag.relId = relation->rd_lockInfo.lockRelId.relId;
	tag.dbId = relation->rd_lockInfo.lockRelId.dbId;
	tag.objId.blkno = blkno;

	LockRelease(LockTableId, &tag, GetCurrentTransactionId(), lockmode);
}

/*
 *		XactLockTableInsert
 *
 * Insert a lock showing that the given transaction ID is running ---
 * this is done during xact startup.  The lock can then be used to wait
 * for the transaction to finish.
 *
 * We need no corresponding unlock function, since the lock will always
 * be released implicitly at transaction commit/abort, never any other way.
 */
void
XactLockTableInsert(TransactionId xid)
{
	LOCKTAG		tag;

	MemSet(&tag, 0, sizeof(tag));
	tag.relId = XactLockTableId;
	tag.dbId = InvalidOid;		/* xids are globally unique */
	tag.objId.xid = xid;

	if (!LockAcquire(LockTableId, &tag, xid,
					 ExclusiveLock, false))
		elog(ERROR, "LockAcquire failed");
}

/*
 *		XactLockTableWait
 *
 * Wait for the specified transaction to commit or abort.
 */
void
XactLockTableWait(TransactionId xid)
{
	LOCKTAG		tag;
	TransactionId myxid = GetCurrentTransactionId();

	Assert(!TransactionIdEquals(xid, myxid));

	MemSet(&tag, 0, sizeof(tag));
	tag.relId = XactLockTableId;
	tag.dbId = InvalidOid;
	tag.objId.xid = xid;

	if (!LockAcquire(LockTableId, &tag, myxid,
					 ShareLock, false))
		elog(ERROR, "LockAcquire failed");

	LockRelease(LockTableId, &tag, myxid, ShareLock);

	/*
	 * Transaction was committed/aborted/crashed - we have to update
	 * pg_clog if transaction is still marked as running.
	 */
	if (!TransactionIdDidCommit(xid) && !TransactionIdDidAbort(xid))
		TransactionIdAbort(xid);
}
