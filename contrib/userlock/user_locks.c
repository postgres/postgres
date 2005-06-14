/*
 * user_locks.c --
 *
 * This loadable module provides support for user-level long-term
 * cooperative locks.
 *
 * Copyright (C) 1999, Massimo Dal Zotto <dz@cs.unitn.it>
 *
 * This software is distributed under the GNU General Public License
 * either version 2, or (at your option) any later version.
 */
#include "postgres.h"

#include "miscadmin.h"
#include "storage/lmgr.h"
#include "storage/proc.h"

#include "user_locks.h"


#define SET_LOCKTAG_USERLOCK(locktag,id1,id2) \
	((locktag).locktag_field1 = MyDatabaseId, \
	 (locktag).locktag_field2 = (id1), \
	 (locktag).locktag_field3 = (id2), \
	 (locktag).locktag_field4 = 0, \
	 (locktag).locktag_type = LOCKTAG_USERLOCK)


int
user_lock(uint32 id1, uint32 id2, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	SET_LOCKTAG_USERLOCK(tag, id1, id2);

	return (LockAcquire(USER_LOCKMETHOD, &tag, false,
						lockmode, true, true) != LOCKACQUIRE_NOT_AVAIL);
}

int
user_unlock(uint32 id1, uint32 id2, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	SET_LOCKTAG_USERLOCK(tag, id1, id2);

	return LockRelease(USER_LOCKMETHOD, &tag, lockmode, true);
}

int
user_write_lock(uint32 id1, uint32 id2)
{
	return user_lock(id1, id2, ExclusiveLock);
}


int
user_write_unlock(uint32 id1, uint32 id2)
{
	return user_unlock(id1, id2, ExclusiveLock);
}

int
user_write_lock_oid(Oid oid)
{
	return user_lock(0, oid, ExclusiveLock);
}

int
user_write_unlock_oid(Oid oid)
{
	return user_unlock(0, oid, ExclusiveLock);
}

int
user_unlock_all(void)
{
	LockReleaseAll(USER_LOCKMETHOD, true);

	return true;
}
