/*-------------------------------------------------------------------------
 *
 * multi.c--
 *	  multi level lock table manager
 *
 *	  Standard multi-level lock manager as per the Gray paper
 *	  (at least, that is what it is supposed to be).  We implement
 *	  three levels -- RELN, PAGE, TUPLE.  Tuple is actually TID
 *	  a physical record pointer.  It isn't an object id.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/lmgr/Attic/multi.c,v 1.11 1998/01/23 19:53:40 scrappy Exp $
 *
 * NOTES:
 *	 (1) The lock.c module assumes that the caller here is doing
 *		 two phase locking.
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <string.h>
#include "postgres.h"
#include "storage/lmgr.h"
#include "storage/multilev.h"

#include "utils/rel.h"
#include "miscadmin.h"			/* MyDatabaseId */

static bool
MultiAcquire(LockTableId tableId, LOCKTAG *tag, LOCKT lockt,
			 PG_LOCK_LEVEL level);
static bool
MultiRelease(LockTableId tableId, LOCKTAG *tag, LOCKT lockt,
			 PG_LOCK_LEVEL level);

/*
 * INTENT indicates to higher level that a lower level lock has been
 * set.  For example, a write lock on a tuple conflicts with a write
 * lock on a relation.	This conflict is detected as a WRITE_INTENT/
 * WRITE conflict between the tuple's intent lock and the relation's
 * write lock.
 */
static int	MultiConflicts[] = {
	(int) NULL,
	/* All reads and writes at any level conflict with a write lock */
	(1 << WRITE_LOCK) | (1 << WRITE_INTENT) | (1 << READ_LOCK) | (1 << READ_INTENT),
	/* read locks conflict with write locks at curr and lower levels */
	(1 << WRITE_LOCK) | (1 << WRITE_INTENT),
	/* write intent locks */
	(1 << READ_LOCK) | (1 << WRITE_LOCK),
	/* read intent locks */
	(1 << WRITE_LOCK),

	/*
	 * extend locks for archive storage manager conflict only w/extend
	 * locks
	 */
	(1 << EXTEND_LOCK)
};

/*
 * write locks have higher priority than read locks and extend locks.  May
 * want to treat INTENT locks differently.
 */
static int	MultiPrios[] = {
	(int) NULL,
	2,
	1,
	2,
	1,
	1
};

/*
 * Lock table identifier for this lock table.  The multi-level
 * lock table is ONE lock table, not three.
 */
LockTableId MultiTableId = (LockTableId) NULL;
LockTableId ShortTermTableId = (LockTableId) NULL;

/*
 * Create the lock table described by MultiConflicts and Multiprio.
 */
LockTableId
InitMultiLevelLockm()
{
	int			tableId;

	/* -----------------------
	 * If we're already initialized just return the table id.
	 * -----------------------
	 */
	if (MultiTableId)
		return MultiTableId;

	tableId = LockTabInit("LockTable", MultiConflicts, MultiPrios, 5);
	MultiTableId = tableId;
	if (!(MultiTableId))
	{
		elog(ERROR, "InitMultiLockm: couldnt initialize lock table");
	}
	/* -----------------------
	 * No short term lock table for now.  -Jeff 15 July 1991
	 *
	 * ShortTermTableId = LockTabRename(tableId);
	 * if (! (ShortTermTableId)) {
	 *	 elog(ERROR,"InitMultiLockm: couldnt rename lock table");
	 * }
	 * -----------------------
	 */
	return MultiTableId;
}

/*
 * MultiLockReln -- lock a relation
 *
 * Returns: TRUE if the lock can be set, FALSE otherwise.
 */
bool
MultiLockReln(LockInfo linfo, LOCKT lockt)
{
	LOCKTAG		tag;

	/*
	 * LOCKTAG has two bytes of padding, unfortunately.  The hash function
	 * will return miss if the padding bytes aren't zero'd.
	 */
	MemSet(&tag, 0, sizeof(tag));
	tag.relId = linfo->lRelId.relId;
	tag.dbId = linfo->lRelId.dbId;
	return (MultiAcquire(MultiTableId, &tag, lockt, RELN_LEVEL));
}

/*
 * MultiLockTuple -- Lock the TID associated with a tuple
 *
 * Returns: TRUE if lock is set, FALSE otherwise.
 *
 * Side Effects: causes intention level locks to be set
 *		at the page and relation level.
 */
bool
MultiLockTuple(LockInfo linfo, ItemPointer tidPtr, LOCKT lockt)
{
	LOCKTAG		tag;

	/*
	 * LOCKTAG has two bytes of padding, unfortunately.  The hash function
	 * will return miss if the padding bytes aren't zero'd.
	 */
	MemSet(&tag, 0, sizeof(tag));

	tag.relId = linfo->lRelId.relId;
	tag.dbId = linfo->lRelId.dbId;

	/* not locking any valid Tuple, just the page */
	tag.tupleId = *tidPtr;
	return (MultiAcquire(MultiTableId, &tag, lockt, TUPLE_LEVEL));
}

/*
 * same as above at page level
 */
bool
MultiLockPage(LockInfo linfo, ItemPointer tidPtr, LOCKT lockt)
{
	LOCKTAG		tag;

	/*
	 * LOCKTAG has two bytes of padding, unfortunately.  The hash function
	 * will return miss if the padding bytes aren't zero'd.
	 */
	MemSet(&tag, 0, sizeof(tag));


	/* ----------------------------
	 * Now we want to set the page offset to be invalid
	 * and lock the block.	There is some confusion here as to what
	 * a page is.  In Postgres a page is an 8k block, however this
	 * block may be partitioned into many subpages which are sometimes
	 * also called pages.  The term is overloaded, so don't be fooled
	 * when we say lock the page we mean the 8k block. -Jeff 16 July 1991
	 * ----------------------------
	 */
	tag.relId = linfo->lRelId.relId;
	tag.dbId = linfo->lRelId.dbId;
	BlockIdCopy(&(tag.tupleId.ip_blkid), &(tidPtr->ip_blkid));
	return (MultiAcquire(MultiTableId, &tag, lockt, PAGE_LEVEL));
}

/*
 * MultiAcquire -- acquire multi level lock at requested level
 *
 * Returns: TRUE if lock is set, FALSE if not
 * Side Effects:
 */
static bool
MultiAcquire(LockTableId tableId,
			 LOCKTAG *tag,
			 LOCKT lockt,
			 PG_LOCK_LEVEL level)
{
	LOCKT		locks[N_LEVELS];
	int			i,
				status;
	LOCKTAG		xxTag,
			   *tmpTag = &xxTag;
	int			retStatus = TRUE;

	/*
	 * Three levels implemented.  If we set a low level (e.g. Tuple) lock,
	 * we must set INTENT locks on the higher levels.  The intent lock
	 * detects conflicts between the low level lock and an existing high
	 * level lock.	For example, setting a write lock on a tuple in a
	 * relation is disallowed if there is an existing read lock on the
	 * entire relation.  The write lock would set a WRITE + INTENT lock on
	 * the relation and that lock would conflict with the read.
	 */
	switch (level)
	{
		case RELN_LEVEL:
			locks[0] = lockt;
			locks[1] = NO_LOCK;
			locks[2] = NO_LOCK;
			break;
		case PAGE_LEVEL:
			locks[0] = lockt + INTENT;
			locks[1] = lockt;
			locks[2] = NO_LOCK;
			break;
		case TUPLE_LEVEL:
			locks[0] = lockt + INTENT;
			locks[1] = lockt + INTENT;
			locks[2] = lockt;
			break;
		default:
			elog(ERROR, "MultiAcquire: bad lock level");
			return (FALSE);
	}

	/*
	 * construct a new tag as we go. Always loop through all levels, but
	 * if we arent' seting a low level lock, locks[i] is set to NO_LOCK
	 * for the lower levels.  Always start from the highest level and go
	 * to the lowest level.
	 */
	MemSet(tmpTag, 0, sizeof(*tmpTag));
	tmpTag->relId = tag->relId;
	tmpTag->dbId = tag->dbId;

	for (i = 0; i < N_LEVELS; i++)
	{
		if (locks[i] != NO_LOCK)
		{
			switch (i)
			{
				case RELN_LEVEL:
					/* -------------
					 * Set the block # and offset to invalid
					 * -------------
					 */
					BlockIdSet(&(tmpTag->tupleId.ip_blkid), InvalidBlockNumber);
					tmpTag->tupleId.ip_posid = InvalidOffsetNumber;
					break;
				case PAGE_LEVEL:
					/* -------------
					 * Copy the block #, set the offset to invalid
					 * -------------
					 */
					BlockIdCopy(&(tmpTag->tupleId.ip_blkid),
								&(tag->tupleId.ip_blkid));
					tmpTag->tupleId.ip_posid = InvalidOffsetNumber;
					break;
				case TUPLE_LEVEL:
					/* --------------
					 * Copy the entire tuple id.
					 * --------------
					 */
					ItemPointerCopy(&tmpTag->tupleId, &tag->tupleId);
					break;
			}

			status = LockAcquire(tableId, tmpTag, locks[i]);
			if (!status)
			{

				/*
				 * failed for some reason. Before returning we have to
				 * release all of the locks we just acquired.
				 * MultiRelease(xx,xx,xx, i) means release starting from
				 * the last level lock we successfully acquired
				 */
				retStatus = FALSE;
				MultiRelease(tableId, tag, lockt, i);
				/* now leave the loop.	Don't try for any more locks */
				break;
			}
		}
	}
	return (retStatus);
}

/* ------------------
 * Release a page in the multi-level lock table
 * ------------------
 */
#ifdef NOT_USED
bool
MultiReleasePage(LockInfo linfo, ItemPointer tidPtr, LOCKT lockt)
{
	LOCKTAG		tag;

	/* ------------------
	 * LOCKTAG has two bytes of padding, unfortunately.  The
	 * hash function will return miss if the padding bytes aren't
	 * zero'd.
	 * ------------------
	 */
	MemSet(&tag, 0, sizeof(LOCKTAG));

	tag.relId = linfo->lRelId.relId;
	tag.dbId = linfo->lRelId.dbId;
	BlockIdCopy(&(tag.tupleId.ip_blkid), &(tidPtr->ip_blkid));

	return (MultiRelease(MultiTableId, &tag, lockt, PAGE_LEVEL));
}

#endif

/* ------------------
 * Release a relation in the multi-level lock table
 * ------------------
 */
bool
MultiReleaseReln(LockInfo linfo, LOCKT lockt)
{
	LOCKTAG		tag;

	/* ------------------
	 * LOCKTAG has two bytes of padding, unfortunately.  The
	 * hash function will return miss if the padding bytes aren't
	 * zero'd.
	 * ------------------
	 */
	MemSet(&tag, 0, sizeof(LOCKTAG));
	tag.relId = linfo->lRelId.relId;
	tag.dbId = linfo->lRelId.dbId;

	return (MultiRelease(MultiTableId, &tag, lockt, RELN_LEVEL));
}

/*
 * MultiRelease -- release a multi-level lock
 *
 * Returns: TRUE if successful, FALSE otherwise.
 */
static bool
MultiRelease(LockTableId tableId,
			 LOCKTAG *tag,
			 LOCKT lockt,
			 PG_LOCK_LEVEL level)
{
	LOCKT		locks[N_LEVELS];
	int			i,
				status;
	LOCKTAG		xxTag,
			   *tmpTag = &xxTag;

	/*
	 * same level scheme as MultiAcquire().
	 */
	switch (level)
	{
		case RELN_LEVEL:
			locks[0] = lockt;
			locks[1] = NO_LOCK;
			locks[2] = NO_LOCK;
			break;
		case PAGE_LEVEL:
			locks[0] = lockt + INTENT;
			locks[1] = lockt;
			locks[2] = NO_LOCK;
			break;
		case TUPLE_LEVEL:
			locks[0] = lockt + INTENT;
			locks[1] = lockt + INTENT;
			locks[2] = lockt;
			break;
		default:
			elog(ERROR, "MultiRelease: bad lockt");
	}

	/*
	 * again, construct the tag on the fly.  This time, however, we
	 * release the locks in the REVERSE order -- from lowest level to
	 * highest level.
	 *
	 * Must zero out the tag to set padding byes to zero and ensure hashing
	 * consistency.
	 */
	MemSet(tmpTag, 0, sizeof(*tmpTag));
	tmpTag->relId = tag->relId;
	tmpTag->dbId = tag->dbId;

	for (i = (N_LEVELS - 1); i >= 0; i--)
	{
		if (locks[i] != NO_LOCK)
		{
			switch (i)
			{
				case RELN_LEVEL:
					/* -------------
					 * Set the block # and offset to invalid
					 * -------------
					 */
					BlockIdSet(&(tmpTag->tupleId.ip_blkid), InvalidBlockNumber);
					tmpTag->tupleId.ip_posid = InvalidOffsetNumber;
					break;
				case PAGE_LEVEL:
					/* -------------
					 * Copy the block #, set the offset to invalid
					 * -------------
					 */
					BlockIdCopy(&(tmpTag->tupleId.ip_blkid),
								&(tag->tupleId.ip_blkid));
					tmpTag->tupleId.ip_posid = InvalidOffsetNumber;
					break;
				case TUPLE_LEVEL:
					ItemPointerCopy(&tmpTag->tupleId, &tag->tupleId);
					break;
			}
			status = LockRelease(tableId, tmpTag, locks[i]);
			if (!status)
			{
				elog(ERROR, "MultiRelease: couldn't release after error");
			}
		}
	}
	/* shouldn't reach here */
	return false;
}
