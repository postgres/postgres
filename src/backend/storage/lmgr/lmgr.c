/*-------------------------------------------------------------------------
 *
 * lmgr.c--
 *	  POSTGRES lock manager code
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/lmgr/lmgr.c,v 1.21 1998/12/16 11:53:48 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */
/* #define LOCKDEBUGALL 1 */
/* #define LOCKDEBUG	1 */

#ifdef	LOCKDEBUGALL
#define LOCKDEBUG		1
#endif	 /* LOCKDEBUGALL */

#include <string.h>

#include "postgres.h"

#include "access/heapam.h"
#include "access/htup.h"
#include "access/relscan.h"
#include "access/skey.h"
#include "access/xact.h"

#include "storage/block.h"
#include "storage/buf.h"
#include "storage/itemptr.h"
#include "storage/bufpage.h"
#include "storage/multilev.h"
#include "storage/lmgr.h"

#include "utils/palloc.h"
#include "utils/mcxt.h"
#include "utils/rel.h"

#include "catalog/catname.h"
#include "catalog/catalog.h"
#include "catalog/pg_class.h"

#include "nodes/memnodes.h"
#include "storage/bufmgr.h"
#include "access/transam.h"		/* for AmiTransactionId */

extern Oid	MyDatabaseId;

static MASK LockConflicts[] = {
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

/* ExtendLock */
	(1 << ExtendLock)

};

static int	LockPrios[] = {
	(int) NULL,
	1,
	2,
	3,
	4,
	5,
	6,
	7,
	1
};

LOCKMETHOD	LockTableId = (LOCKMETHOD) NULL;
LOCKMETHOD	LongTermTableId = (LOCKMETHOD) NULL;

/*
 * Create the lock table described by LockConflicts and LockPrios.
 */
LOCKMETHOD
InitLockTable()
{
	int			lockmethod;

	lockmethod = LockMethodTableInit("LockTable",
						  LockConflicts, LockPrios, MAX_LOCKMODES - 1);
	LockTableId = lockmethod;
	if (!(LockTableId))
		elog(ERROR, "InitLockTable: couldnt initialize lock table");

#ifdef USER_LOCKS
	/*
	 * Allocate another tableId for long-term locks
	 */
	LongTermTableId = LockMethodTableRename(LockTableId);
	if (!(LongTermTableId))
	{
		elog(ERROR,
			 "InitLockTable: couldn't rename long-term lock table");
	}
#endif

	return LockTableId;
}

/*
 * RelationInitLockInfo --
 *		Initializes the lock information in a relation descriptor.
 */
void
RelationInitLockInfo(Relation relation)
{
	LockInfo	info;
	char	   *relname;
	MemoryContext oldcxt;
	extern Oid	MyDatabaseId;	/* XXX use include */
	extern GlobalMemory CacheCxt;

	Assert(RelationIsValid(relation));
	Assert(OidIsValid(RelationGetRelid(relation)));

	info = (LockInfo) relation->lockInfo;

	if (LockInfoIsValid(info))
		return;

	relname = (char *) RelationGetRelationName(relation);

	oldcxt = MemoryContextSwitchTo((MemoryContext) CacheCxt);
	info = (LockInfo) palloc(sizeof(LockInfoData));
	MemoryContextSwitchTo(oldcxt);

	info->lockRelId.relId = RelationGetRelid(relation);
	if (IsSharedSystemRelationName(relname))
		info->lockRelId.dbId = InvalidOid;
	else
		info->lockRelId.dbId = MyDatabaseId;

	relation->lockInfo = (Pointer) info;
}

/*
 *		LockRelation
 */
void
LockRelation(Relation relation, LOCKMODE lockmode)
{
	LockInfo	lockinfo;
	LOCKTAG		tag;

	if (LockingDisabled())
		return;

	if (!LockInfoIsValid(relation->lockInfo))
		RelationInitLockInfo(relation);

	lockinfo = (LockInfo) relation->lockInfo;

	MemSet(&tag, 0, sizeof(tag));
	tag.relId = lockinfo->lockRelId.relId;
	tag.dbId = lockinfo->lockRelId.dbId;
	tag.objId.blkno = InvalidBlockNumber;

	LockAcquire(LockTableId, &tag, lockmode);
	return;
}

/*
 *		UnlockRelation
 */
void
UnlockRelation(Relation relation, LOCKMODE lockmode)
{
	LockInfo	lockinfo;
	LOCKTAG		tag;

	if (LockingDisabled())
		return;

	lockinfo = (LockInfo) relation->lockInfo;

	if (!LockInfoIsValid(lockinfo))
	{
		elog(ERROR,
			 "Releasing a lock on %s with invalid lock information",
			 RelationGetRelationName(relation));
	}

	MemSet(&tag, 0, sizeof(tag));
	tag.relId = lockinfo->lockRelId.relId;
	tag.dbId = lockinfo->lockRelId.dbId;
	tag.objId.blkno = InvalidBlockNumber;

	LockRelease(LockTableId, &tag, lockmode);
	return;
}

/*
 *		LockPage
 */
void
LockPage(Relation relation, BlockNumber blkno, LOCKMODE lockmode)
{
	LockInfo	lockinfo;
	LOCKTAG		tag;

	if (LockingDisabled())
		return;

	if (!LockInfoIsValid(relation->lockInfo))
		RelationInitLockInfo(relation);

	lockinfo = (LockInfo) relation->lockInfo;

	MemSet(&tag, 0, sizeof(tag));
	tag.relId = lockinfo->lockRelId.relId;
	tag.dbId = lockinfo->lockRelId.dbId;
	tag.objId.blkno = blkno;

	LockAcquire(LockTableId, &tag, lockmode);
	return;
}

/*
 *		UnlockPage
 */
void
UnlockPage(Relation relation, BlockNumber blkno, LOCKMODE lockmode)
{
	LockInfo	lockinfo;
	LOCKTAG		tag;

	if (LockingDisabled())
		return;

	lockinfo = (LockInfo) relation->lockInfo;

	if (!LockInfoIsValid(lockinfo))
	{
		elog(ERROR,
			 "Releasing a lock on %s with invalid lock information",
			 RelationGetRelationName(relation));
	}

	MemSet(&tag, 0, sizeof(tag));
	tag.relId = lockinfo->lockRelId.relId;
	tag.dbId = lockinfo->lockRelId.dbId;
	tag.objId.blkno = blkno;

	LockRelease(LockTableId, &tag, lockmode);
	return;
}

void
XactLockTableInsert(TransactionId xid)
{
	LOCKTAG		tag;

	if (LockingDisabled())
		return;

	MemSet(&tag, 0, sizeof(tag));
	tag.relId = XactLockTableId;
	tag.dbId = InvalidOid;
	tag.objId.xid = xid;

	LockAcquire(LockTableId, &tag, ExclusiveLock);
	return;
}

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

	LockRelease(LockTableId, &tag, ExclusiveLock);
	return;
}

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

	LockAcquire(LockTableId, &tag, ShareLock);

	TransactionIdFlushCache();
	/*
	 * Transaction was committed/aborted/crashed - 
	 * we have to update pg_log if transaction is still
	 * marked as running.
	 */
	if (!TransactionIdDidCommit(xid) && !TransactionIdDidAbort(xid))
		TransactionIdAbort(xid);

	return;
}
