/*-------------------------------------------------------------------------
 *
 * lmgr.c--
 *	  POSTGRES lock manager code
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/lmgr/lmgr.c,v 1.16 1998/08/01 15:26:24 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */
/* #define LOCKDEBUGALL 1 */
/* #define LOCKDEBUG	1 */

#ifdef	LOCKDEBUGALL
#define LOCKDEBUG		1
#endif							/* LOCKDEBUGALL */

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
#ifdef MULTIBYTE
#include "catalog/pg_class_mb.h"
#else
#include "catalog/pg_class.h"
#endif

#include "nodes/memnodes.h"
#include "storage/bufmgr.h"
#include "access/transam.h"		/* for AmiTransactionId */

extern Oid	MyDatabaseId;

/*
 * RelationInitLockInfo --
 *		Initializes the lock information in a relation descriptor.
 */
void
RelationInitLockInfo(Relation relation)
{
	LockInfo			info;
	char			   *relname;
	MemoryContext		oldcxt;
	extern Oid			MyDatabaseId;	/* XXX use include */
	extern GlobalMemory	CacheCxt;

	Assert(RelationIsValid(relation));
	Assert(OidIsValid(RelationGetRelationId(relation)));

	info = (LockInfo) relation->lockInfo;
	
	if (LockInfoIsValid(info))
		return;
	
	relname = (char *) RelationGetRelationName(relation);

	oldcxt = MemoryContextSwitchTo((MemoryContext) CacheCxt);
	info = (LockInfo) palloc(sizeof(LockInfoData));
	MemoryContextSwitchTo(oldcxt);

	info->lockRelId.relId = RelationGetRelationId(relation);
	if (IsSharedSystemRelationName(relname))
		info->lockRelId.dbId = InvalidOid;
	else
		info->lockRelId.dbId = MyDatabaseId;

#ifdef LowLevelLocking
	memset(info->lockHeld, 0, sizeof(info->lockHeld));
#endif

	relation->lockInfo = (Pointer) info;
}

/*
 * RelationSetLockForDescriptorOpen --
 *		Sets read locks for a relation descriptor.
 */
#ifdef	LOCKDEBUGALL
#define LOCKDEBUGALL_30 \
elog(DEBUG, "RelationSetLockForDescriptorOpen(%s[%d,%d]) called", \
	 RelationGetRelationName(relation), lockRelId.dbId, lockRelId.relId)
#else
#define LOCKDEBUGALL_30
#endif							/* LOCKDEBUGALL */

void
RelationSetLockForDescriptorOpen(Relation relation)
{
	/* ----------------
	 *	sanity checks
	 * ----------------
	 */
	Assert(RelationIsValid(relation));
	if (LockingDisabled())
		return;

	LOCKDEBUGALL_30;

	/* ----------------
	 * read lock catalog tuples which compose the relation descriptor
	 * XXX race condition? XXX For now, do nothing.
	 * ----------------
	 */
}

/* ----------------
 *		RelationSetLockForRead
 * ----------------
 */
#ifdef	LOCKDEBUG
#define LOCKDEBUG_40 \
elog(DEBUG, "RelationSetLockForRead(%s[%d,%d]) called", \
	 RelationGetRelationName(relation), lockRelId.dbId, lockRelId.relId)
#else
#define LOCKDEBUG_40
#endif							/* LOCKDEBUG */

/*
 * RelationSetLockForRead --
 *		Sets relation level read lock.
 */
void
RelationSetLockForRead(Relation relation)
{
	LockInfo	lockinfo;

	/* ----------------
	 *	sanity checks
	 * ----------------
	 */
	Assert(RelationIsValid(relation));
	if (LockingDisabled())
		return;

	LOCKDEBUG_40;

	/* ----------------
	 * If we don't have lock info on the reln just go ahead and
	 * lock it without trying to short circuit the lock manager.
	 * ----------------
	 */
	if (!LockInfoIsValid(relation->lockInfo))
	{
		RelationInitLockInfo(relation);
		lockinfo = (LockInfo) relation->lockInfo;
		MultiLockReln(lockinfo, READ_LOCK);
		return;
	}
	else
		lockinfo = (LockInfo) relation->lockInfo;

	MultiLockReln(lockinfo, READ_LOCK);
}

/* ----------------
 *		RelationUnsetLockForRead
 * ----------------
 */
#ifdef	LOCKDEBUG
#define LOCKDEBUG_50 \
elog(DEBUG, "RelationUnsetLockForRead(%s[%d,%d]) called", \
	 RelationGetRelationName(relation), lockRelId.dbId, lockRelId.relId)
#else
#define LOCKDEBUG_50
#endif							/* LOCKDEBUG */

/*
 * RelationUnsetLockForRead --
 *		Unsets relation level read lock.
 */
void
RelationUnsetLockForRead(Relation relation)
{
	LockInfo	lockinfo;

	/* ----------------
	 *	sanity check
	 * ----------------
	 */
	Assert(RelationIsValid(relation));
	if (LockingDisabled())
		return;

	lockinfo = (LockInfo) relation->lockInfo;

	/* ----------------
	 * If we don't have lock info on the reln just go ahead and
	 * release it.
	 * ----------------
	 */
	if (!LockInfoIsValid(lockinfo))
	{
		elog(ERROR,
			 "Releasing a lock on %s with invalid lock information",
			 RelationGetRelationName(relation));
	}

	MultiReleaseReln(lockinfo, READ_LOCK);
}

/* ----------------
 *		RelationSetLockForWrite(relation)
 * ----------------
 */
#ifdef	LOCKDEBUG
#define LOCKDEBUG_60 \
elog(DEBUG, "RelationSetLockForWrite(%s[%d,%d]) called", \
	 RelationGetRelationName(relation), lockRelId.dbId, lockRelId.relId)
#else
#define LOCKDEBUG_60
#endif							/* LOCKDEBUG */

/*
 * RelationSetLockForWrite --
 *		Sets relation level write lock.
 */
void
RelationSetLockForWrite(Relation relation)
{
	LockInfo	lockinfo;

	/* ----------------
	 *	sanity checks
	 * ----------------
	 */
	Assert(RelationIsValid(relation));
	if (LockingDisabled())
		return;

	LOCKDEBUG_60;

	/* ----------------
	 * If we don't have lock info on the reln just go ahead and
	 * lock it without trying to short circuit the lock manager.
	 * ----------------
	 */
	if (!LockInfoIsValid(relation->lockInfo))
	{
		RelationInitLockInfo(relation);
		lockinfo = (LockInfo) relation->lockInfo;
		MultiLockReln(lockinfo, WRITE_LOCK);
		return;
	}
	else
		lockinfo = (LockInfo) relation->lockInfo;

	MultiLockReln(lockinfo, WRITE_LOCK);
}

/* ----------------
 *		RelationUnsetLockForWrite
 * ----------------
 */
#ifdef	LOCKDEBUG
#define LOCKDEBUG_70 \
elog(DEBUG, "RelationUnsetLockForWrite(%s[%d,%d]) called", \
	 RelationGetRelationName(relation), lockRelId.dbId, lockRelId.relId)
#else
#define LOCKDEBUG_70
#endif							/* LOCKDEBUG */

/*
 * RelationUnsetLockForWrite --
 *		Unsets relation level write lock.
 */
void
RelationUnsetLockForWrite(Relation relation)
{
	LockInfo	lockinfo;

	/* ----------------
	 *	sanity checks
	 * ----------------
	 */
	Assert(RelationIsValid(relation));
	if (LockingDisabled())
		return;

	lockinfo = (LockInfo) relation->lockInfo;

	if (!LockInfoIsValid(lockinfo))
	{
		elog(ERROR,
			 "Releasing a lock on %s with invalid lock information",
			 RelationGetRelationName(relation));
	}

	MultiReleaseReln(lockinfo, WRITE_LOCK);
}

/* ----------------
 *		RelationSetLockForReadPage
 * ----------------
 */
#ifdef	LOCKDEBUG
#define LOCKDEBUG_90 \
elog(DEBUG, "RelationSetLockForReadPage(%s[%d,%d], @%d) called", \
	 RelationGetRelationName(relation), lockRelId.dbId, lockRelId.relId, page)
#else
#define LOCKDEBUG_90
#endif							/* LOCKDEBUG */

/* ----------------
 *		RelationSetLockForWritePage
 * ----------------
 */
#ifdef	LOCKDEBUG
#define LOCKDEBUG_100 \
elog(DEBUG, "RelationSetLockForWritePage(%s[%d,%d], @%d) called", \
	 RelationGetRelationName(relation), lockRelId.dbId, lockRelId.relId, page)
#else
#define LOCKDEBUG_100
#endif							/* LOCKDEBUG */

/*
 * RelationSetLockForWritePage --
 *		Sets write lock on a page.
 */
void
RelationSetLockForWritePage(Relation relation,
							ItemPointer itemPointer)
{
	/* ----------------
	 *	sanity checks
	 * ----------------
	 */
	Assert(RelationIsValid(relation));
	if (LockingDisabled())
		return;

	/* ---------------
	 * Make sure lockinfo is initialized
	 * ---------------
	 */
	if (!LockInfoIsValid(relation->lockInfo))
		RelationInitLockInfo(relation);

	/* ----------------
	 *	attempt to set lock
	 * ----------------
	 */
	MultiLockPage((LockInfo) relation->lockInfo, itemPointer, WRITE_LOCK);
}

/* ----------------
 *		RelationUnsetLockForReadPage
 * ----------------
 */
#ifdef	LOCKDEBUG
#define LOCKDEBUG_110 \
elog(DEBUG, "RelationUnsetLockForReadPage(%s[%d,%d], @%d) called", \
	 RelationGetRelationName(relation), lockRelId.dbId, lockRelId.relId, page)
#else
#define LOCKDEBUG_110
#endif							/* LOCKDEBUG */

/* ----------------
 *		RelationUnsetLockForWritePage
 * ----------------
 */
#ifdef	LOCKDEBUG
#define LOCKDEBUG_120 \
elog(DEBUG, "RelationUnsetLockForWritePage(%s[%d,%d], @%d) called", \
	 RelationGetRelationName(relation), lockRelId.dbId, lockRelId.relId, page)
#else
#define LOCKDEBUG_120
#endif							/* LOCKDEBUG */

/*
 * Set a single level write page lock.	Assumes that you already
 * have a write intent lock on the relation.
 */
void
RelationSetSingleWLockPage(Relation relation,
						   ItemPointer itemPointer)
{

	/* ----------------
	 *	sanity checks
	 * ----------------
	 */
	Assert(RelationIsValid(relation));
	if (LockingDisabled())
		return;

	if (!LockInfoIsValid(relation->lockInfo))
		RelationInitLockInfo(relation);

	SingleLockPage((LockInfo) relation->lockInfo, itemPointer, WRITE_LOCK, !UNLOCK);
}

/*
 * Unset a single level write page lock
 */
void
RelationUnsetSingleWLockPage(Relation relation,
							 ItemPointer itemPointer)
{

	/* ----------------
	 *	sanity checks
	 * ----------------
	 */
	Assert(RelationIsValid(relation));
	if (LockingDisabled())
		return;

	if (!LockInfoIsValid(relation->lockInfo))
		elog(ERROR,
			 "Releasing a lock on %s with invalid lock information",
			 RelationGetRelationName(relation));

	SingleLockPage((LockInfo) relation->lockInfo, itemPointer, WRITE_LOCK, UNLOCK);
}

/*
 * Set a single level read page lock.  Assumes you already have a read
 * intent lock set on the relation.
 */
void
RelationSetSingleRLockPage(Relation relation,
						   ItemPointer itemPointer)
{

	/* ----------------
	 *	sanity checks
	 * ----------------
	 */
	Assert(RelationIsValid(relation));
	if (LockingDisabled())
		return;

	if (!LockInfoIsValid(relation->lockInfo))
		RelationInitLockInfo(relation);

	SingleLockPage((LockInfo) relation->lockInfo, itemPointer, READ_LOCK, !UNLOCK);
}

/*
 * Unset a single level read page lock.
 */
void
RelationUnsetSingleRLockPage(Relation relation,
							 ItemPointer itemPointer)
{

	/* ----------------
	 *	sanity checks
	 * ----------------
	 */
	Assert(RelationIsValid(relation));
	if (LockingDisabled())
		return;

	if (!LockInfoIsValid(relation->lockInfo))
		elog(ERROR,
			 "Releasing a lock on %s with invalid lock information",
			 RelationGetRelationName(relation));

	SingleLockPage((LockInfo) relation->lockInfo, itemPointer, READ_LOCK, UNLOCK);
}

/*
 * Set a read intent lock on a relation.
 *
 * Usually these are set in a multi-level table when you acquiring a
 * page level lock.  i.e. To acquire a lock on a page you first acquire
 * an intent lock on the entire relation.  Acquiring an intent lock along
 * allows one to use the single level locking routines later.  Good for
 * index scans that do a lot of page level locking.
 */
void
RelationSetRIntentLock(Relation relation)
{
	/* -----------------
	 * Sanity check
	 * -----------------
	 */
	Assert(RelationIsValid(relation));
	if (LockingDisabled())
		return;

	if (!LockInfoIsValid(relation->lockInfo))
		RelationInitLockInfo(relation);

	SingleLockReln((LockInfo) relation->lockInfo, READ_LOCK + INTENT, !UNLOCK);
}

/*
 * Unset a read intent lock on a relation
 */
void
RelationUnsetRIntentLock(Relation relation)
{
	/* -----------------
	 * Sanity check
	 * -----------------
	 */
	Assert(RelationIsValid(relation));
	if (LockingDisabled())
		return;

	if (!LockInfoIsValid(relation->lockInfo))
		RelationInitLockInfo(relation);

	SingleLockReln((LockInfo) relation->lockInfo, READ_LOCK + INTENT, UNLOCK);
}

/*
 * Set a write intent lock on a relation. For a more complete explanation
 * see RelationSetRIntentLock()
 */
void
RelationSetWIntentLock(Relation relation)
{
	/* -----------------
	 * Sanity check
	 * -----------------
	 */
	Assert(RelationIsValid(relation));
	if (LockingDisabled())
		return;

	if (!LockInfoIsValid(relation->lockInfo))
		RelationInitLockInfo(relation);

	SingleLockReln((LockInfo) relation->lockInfo, WRITE_LOCK + INTENT, !UNLOCK);
}

/*
 * Unset a write intent lock.
 */
void
RelationUnsetWIntentLock(Relation relation)
{
	/* -----------------
	 * Sanity check
	 * -----------------
	 */
	Assert(RelationIsValid(relation));
	if (LockingDisabled())
		return;

	if (!LockInfoIsValid(relation->lockInfo))
		RelationInitLockInfo(relation);

	SingleLockReln((LockInfo) relation->lockInfo, WRITE_LOCK + INTENT, UNLOCK);
}

/*
 * Extend locks are used primarily in tertiary storage devices such as
 * a WORM disk jukebox.  Sometimes need exclusive access to extend a
 * file by a block.
 */
#ifdef NOT_USED
void
RelationSetLockForExtend(Relation relation)
{
	/* -----------------
	 * Sanity check
	 * -----------------
	 */
	Assert(RelationIsValid(relation));
	if (LockingDisabled())
		return;

	if (!LockInfoIsValid(relation->lockInfo))
		RelationInitLockInfo(relation);

	MultiLockReln((LockInfo) relation->lockInfo, EXTEND_LOCK);
}

#endif

#ifdef NOT_USED
void
RelationUnsetLockForExtend(Relation relation)
{
	/* -----------------
	 * Sanity check
	 * -----------------
	 */
	Assert(RelationIsValid(relation));
	if (LockingDisabled())
		return;

	if (!LockInfoIsValid(relation->lockInfo))
		RelationInitLockInfo(relation);

	MultiReleaseReln((LockInfo) relation->lockInfo, EXTEND_LOCK);
}

#endif

