/*
 * user_locks.c --
 *
 * This loadable module, together with my user-lock.patch applied to the
 * backend, provides support for user-level long-term cooperative locks.
 *
 * Copyright (c) 1996, Massimo Dal Zotto <dz@cs.unitn.it>
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "postgres.h"
#include "miscadmin.h"
#include "storage/lock.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/block.h"
#include "storage/multilev.h"
#include "utils/elog.h"

#include "user_locks.h"

#define USER_LOCKS_TABLE_ID 0

extern Oid	MyDatabaseId;

int
user_lock(unsigned int id1, unsigned int id2, LOCKT lockt)
{
	LOCKTAG		tag;

	memset(&tag, 0, sizeof(LOCKTAG));
	tag.relId = 0;
	tag.dbId = MyDatabaseId;
	tag.tupleId.ip_blkid.bi_hi = id2 >> 16;
	tag.tupleId.ip_blkid.bi_lo = id2 & 0xffff;
	tag.tupleId.ip_posid = (unsigned short) (id1 & 0xffff);

	return LockAcquire(USER_LOCKS_TABLE_ID, &tag, lockt);
}

int
user_unlock(unsigned int id1, unsigned int id2, LOCKT lockt)
{
	LOCKTAG		tag;

	memset(&tag, 0, sizeof(LOCKTAG));
	tag.relId = 0;
	tag.dbId = MyDatabaseId;
	tag.tupleId.ip_blkid.bi_hi = id2 >> 16;
	tag.tupleId.ip_blkid.bi_lo = id2 & 0xffff;
	tag.tupleId.ip_posid = (unsigned short) (id1 & 0xffff);

	return LockRelease(USER_LOCKS_TABLE_ID, &tag, lockt);
}

int
user_write_lock(unsigned int id1, unsigned int id2)
{
	return user_lock(id1, id2, WRITE_LOCK);
}


int
user_write_unlock(unsigned int id1, unsigned int id2)
{
	return user_unlock(id1, id2, WRITE_LOCK);
}

int
user_write_lock_oid(Oid oid)
{
	return user_lock(0, oid, WRITE_LOCK);
}

int
user_write_unlock_oid(Oid oid)
{
	return user_unlock(0, oid, WRITE_LOCK);
}

int
user_unlock_all()
{
	PROC	   *proc;
	SHMEM_OFFSET location;

	ShmemPIDLookup(getpid(), &location);
	if (location == INVALID_OFFSET)
	{
		elog(NOTICE, "UserUnlockAll: unable to get proc ptr");
		return -1;
	}

	proc = (PROC *) MAKE_PTR(location);
	return LockReleaseAll(USER_LOCKS_TABLE_ID, &proc->lockQueue);
}

/* end of file */
