/*-------------------------------------------------------------------------
 *
 * single.c--
 *	  set single locks in the multi-level lock hierarchy
 *
 *	  Sometimes we don't want to set all levels of the multi-level
 *		lock hierarchy at once.  This allows us to set and release
 *		one level at a time.  It's useful in index scans when
 *		you can set an intent lock at the beginning and thereafter
 *		only set page locks.  Tends to speed things up.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/lmgr/Attic/single.c,v 1.8 1998/07/13 16:34:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>

#include "postgres.h"

#include "storage/lmgr.h"		/* where the declarations go */
#include "storage/lock.h"
#include "storage/multilev.h"
#include "utils/rel.h"

/*
 * SingleLockReln -- lock a relation
 *
 * Returns: TRUE if the lock can be set, FALSE otherwise.
 */
bool
SingleLockReln(LockInfo lockinfo, LOCKMODE lockmode, int action)
{
	LOCKTAG		tag;

	/*
	 * LOCKTAG has two bytes of padding, unfortunately.  The hash function
	 * will return miss if the padding bytes aren't zero'd.
	 */
	MemSet(&tag, 0, sizeof(tag));
	tag.relId = lockinfo->lockRelId.relId;
	tag.dbId = lockinfo->lockRelId.dbId;
	BlockIdSet(&(tag.tupleId.ip_blkid), InvalidBlockNumber);
	tag.tupleId.ip_posid = InvalidOffsetNumber;

	if (action == UNLOCK)
		return (LockRelease(MultiTableId, &tag, lockmode));
	else
		return (LockAcquire(MultiTableId, &tag, lockmode));
}

/*
 * SingleLockPage -- use multi-level lock table, but lock
 *		only at the page level.
 *
 * Assumes that an INTENT lock has already been set in the
 * multi-level lock table.
 *
 */
bool
SingleLockPage(LockInfo lockinfo,
			   ItemPointer tidPtr,
			   LOCKMODE lockmode,
			   int action)
{
	LOCKTAG		tag;

	/*
	 * LOCKTAG has two bytes of padding, unfortunately.  The hash function
	 * will return miss if the padding bytes aren't zero'd.
	 */
	MemSet(&tag, 0, sizeof(tag));
	tag.relId = lockinfo->lockRelId.relId;
	tag.dbId = lockinfo->lockRelId.dbId;
	BlockIdCopy(&(tag.tupleId.ip_blkid), &(tidPtr->ip_blkid));
	tag.tupleId.ip_posid = InvalidOffsetNumber;


	if (action == UNLOCK)
		return (LockRelease(MultiTableId, &tag, lockmode));
	else
		return (LockAcquire(MultiTableId, &tag, lockmode));
}
