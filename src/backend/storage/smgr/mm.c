/*-------------------------------------------------------------------------
 *
 * mm.c
 *	  main memory storage manager
 *
 *	  This code manages relations that reside in (presumably stable)
 *	  main memory.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/smgr/Attic/mm.c,v 1.34 2003/08/04 02:40:04 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "storage/smgr.h"
#include "miscadmin.h"


#ifdef STABLE_MEMORY_STORAGE

/*
 *	MMCacheTag -- Unique triplet for blocks stored by the main memory
 *				  storage manager.
 */

typedef struct MMCacheTag
{
	Oid			mmct_dbid;
	Oid			mmct_relid;
	BlockNumber mmct_blkno;
}	MMCacheTag;

/*
 *	Shared-memory hash table for main memory relations contains
 *	entries of this form.
 */

typedef struct MMHashEntry
{
	MMCacheTag	mmhe_tag;
	int			mmhe_bufno;
}	MMHashEntry;

/*
 * MMRelTag -- Unique identifier for each relation that is stored in the
 *					main-memory storage manager.
 */

typedef struct MMRelTag
{
	Oid			mmrt_dbid;
	Oid			mmrt_relid;
}	MMRelTag;

/*
 *	Shared-memory hash table for # blocks in main memory relations contains
 *	entries of this form.
 */

typedef struct MMRelHashEntry
{
	MMRelTag	mmrhe_tag;
	int			mmrhe_nblocks;
}	MMRelHashEntry;

#define MMNBUFFERS		10
#define MMNRELATIONS	2

static int *MMCurTop;
static int *MMCurRelno;
static MMCacheTag *MMBlockTags;
static char *MMBlockCache;
static HTAB *MMCacheHT;
static HTAB *MMRelCacheHT;

int
mminit(void)
{
	char	   *mmcacheblk;
	int			mmsize = 0;
	bool		found;
	HASHCTL		info;

	LWLockAcquire(MMCacheLock, LW_EXCLUSIVE);

	mmsize += MAXALIGN(BLCKSZ * MMNBUFFERS);
	mmsize += MAXALIGN(sizeof(*MMCurTop));
	mmsize += MAXALIGN(sizeof(*MMCurRelno));
	mmsize += MAXALIGN((MMNBUFFERS * sizeof(MMCacheTag)));
	mmcacheblk = (char *) ShmemInitStruct("Main memory smgr", mmsize, &found);

	if (mmcacheblk == (char *) NULL)
	{
		LWLockRelease(MMCacheLock);
		return SM_FAIL;
	}

	info.keysize = sizeof(MMCacheTag);
	info.entrysize = sizeof(MMHashEntry);
	info.hash = tag_hash;

	MMCacheHT = ShmemInitHash("Main memory store HT",
							  MMNBUFFERS, MMNBUFFERS,
							  &info, (HASH_ELEM | HASH_FUNCTION));

	if (MMCacheHT == (HTAB *) NULL)
	{
		LWLockRelease(MMCacheLock);
		return SM_FAIL;
	}

	info.keysize = sizeof(MMRelTag);
	info.entrysize = sizeof(MMRelHashEntry);
	info.hash = tag_hash;

	MMRelCacheHT = ShmemInitHash("Main memory rel HT",
								 MMNRELATIONS, MMNRELATIONS,
								 &info, (HASH_ELEM | HASH_FUNCTION));

	if (MMRelCacheHT == (HTAB *) NULL)
	{
		LWLockRelease(MMCacheLock);
		return SM_FAIL;
	}

	if (IsUnderPostmaster)		/* was IsPostmaster bjm */
	{
		MemSet(mmcacheblk, 0, mmsize);
		LWLockRelease(MMCacheLock);
		return SM_SUCCESS;
	}

	LWLockRelease(MMCacheLock);

	MMCurTop = (int *) mmcacheblk;
	mmcacheblk += sizeof(int);
	MMCurRelno = (int *) mmcacheblk;
	mmcacheblk += sizeof(int);
	MMBlockTags = (MMCacheTag *) mmcacheblk;
	mmcacheblk += (MMNBUFFERS * sizeof(MMCacheTag));
	MMBlockCache = mmcacheblk;

	return SM_SUCCESS;
}

int
mmshutdown(void)
{
	return SM_SUCCESS;
}

int
mmcreate(Relation reln)
{
	MMRelHashEntry *entry;
	bool		found;
	MMRelTag	tag;

	LWLockAcquire(MMCacheLock, LW_EXCLUSIVE);

	if (*MMCurRelno == MMNRELATIONS)
	{
		LWLockRelease(MMCacheLock);
		return SM_FAIL;
	}

	(*MMCurRelno)++;

	tag.mmrt_relid = RelationGetRelid(reln);
	if (reln->rd_rel->relisshared)
		tag.mmrt_dbid = (Oid) 0;
	else
		tag.mmrt_dbid = MyDatabaseId;

	entry = (MMRelHashEntry *) hash_search(MMRelCacheHT,
										   (void *) &tag,
										   HASH_ENTER, &found);

	if (entry == (MMRelHashEntry *) NULL)
	{
		LWLockRelease(MMCacheLock);
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	}

	if (found)
	{
		/* already exists */
		LWLockRelease(MMCacheLock);
		return SM_FAIL;
	}

	entry->mmrhe_nblocks = 0;

	LWLockRelease(MMCacheLock);

	return SM_SUCCESS;
}

/*
 *	mmunlink() -- Unlink a relation.
 *
 * XXX currently broken: needs to accept RelFileNode, not Relation
 */
int
mmunlink(RelFileNode rnode)
{
	int			i;
	MMHashEntry *entry;
	MMRelHashEntry *rentry;
	MMRelTag	rtag;

	LWLockAcquire(MMCacheLock, LW_EXCLUSIVE);

	for (i = 0; i < MMNBUFFERS; i++)
	{
		if (MMBlockTags[i].mmct_dbid == rnode.tblNode
			&& MMBlockTags[i].mmct_relid == rnode.relNode)
		{
			entry = (MMHashEntry *) hash_search(MMCacheHT,
												(void *) &MMBlockTags[i],
												HASH_REMOVE, NULL);
			if (entry == (MMHashEntry *) NULL)
			{
				LWLockRelease(MMCacheLock);
				elog(FATAL, "cache hash table corrupted");
			}
			MMBlockTags[i].mmct_dbid = (Oid) 0;
			MMBlockTags[i].mmct_relid = (Oid) 0;
			MMBlockTags[i].mmct_blkno = (BlockNumber) 0;
		}
	}
	rtag.mmrt_dbid = rnode.tblNode;
	rtag.mmrt_relid = rnode.relNode;

	rentry = (MMRelHashEntry *) hash_search(MMRelCacheHT,
											(void *) &rtag,
											HASH_REMOVE, NULL);

	if (rentry == (MMRelHashEntry *) NULL)
	{
		LWLockRelease(MMCacheLock);
		elog(FATAL, "rel cache hash table corrupted");
	}

	(*MMCurRelno)--;

	LWLockRelease(MMCacheLock);
	return 1;
}

/*
 *	mmextend() -- Add a block to the specified relation.
 *
 *		This routine returns SM_FAIL or SM_SUCCESS, with errno set as
 *		appropriate.
 */
int
mmextend(Relation reln, BlockNumber blocknum, char *buffer)
{
	MMRelHashEntry *rentry;
	MMHashEntry *entry;
	int			i;
	Oid			reldbid;
	int			offset;
	bool		found;
	MMRelTag	rtag;
	MMCacheTag	tag;

	if (reln->rd_rel->relisshared)
		reldbid = (Oid) 0;
	else
		reldbid = MyDatabaseId;

	tag.mmct_dbid = rtag.mmrt_dbid = reldbid;
	tag.mmct_relid = rtag.mmrt_relid = RelationGetRelid(reln);

	LWLockAcquire(MMCacheLock, LW_EXCLUSIVE);

	if (*MMCurTop == MMNBUFFERS)
	{
		for (i = 0; i < MMNBUFFERS; i++)
		{
			if (MMBlockTags[i].mmct_dbid == 0 &&
				MMBlockTags[i].mmct_relid == 0)
				break;
		}
		if (i == MMNBUFFERS)
		{
			LWLockRelease(MMCacheLock);
			return SM_FAIL;
		}
	}
	else
	{
		i = *MMCurTop;
		(*MMCurTop)++;
	}

	rentry = (MMRelHashEntry *) hash_search(MMRelCacheHT,
											(void *) &rtag,
											HASH_FIND, NULL);
	if (rentry == (MMRelHashEntry *) NULL)
	{
		LWLockRelease(MMCacheLock);
		elog(FATAL, "rel cache hash table corrupted");
	}

	tag.mmct_blkno = rentry->mmrhe_nblocks;

	entry = (MMHashEntry *) hash_search(MMCacheHT,
										(void *) &tag,
										HASH_ENTER, &found);
	if (entry == (MMHashEntry *) NULL || found)
	{
		LWLockRelease(MMCacheLock);
		elog(FATAL, "cache hash table corrupted");
	}

	entry->mmhe_bufno = i;
	MMBlockTags[i].mmct_dbid = reldbid;
	MMBlockTags[i].mmct_relid = RelationGetRelid(reln);
	MMBlockTags[i].mmct_blkno = rentry->mmrhe_nblocks;

	/* page numbers are zero-based, so we increment this at the end */
	(rentry->mmrhe_nblocks)++;

	/* write the extended page */
	offset = (i * BLCKSZ);
	memmove(&(MMBlockCache[offset]), buffer, BLCKSZ);

	LWLockRelease(MMCacheLock);

	return SM_SUCCESS;
}

/*
 *	mmopen() -- Open the specified relation.
 */
int
mmopen(Relation reln)
{
	/* automatically successful */
	return 0;
}

/*
 *	mmclose() -- Close the specified relation.
 *
 *		Returns SM_SUCCESS or SM_FAIL with errno set as appropriate.
 */
int
mmclose(Relation reln)
{
	/* automatically successful */
	return SM_SUCCESS;
}

/*
 *	mmread() -- Read the specified block from a relation.
 *
 *		Returns SM_SUCCESS or SM_FAIL.
 */
int
mmread(Relation reln, BlockNumber blocknum, char *buffer)
{
	MMHashEntry *entry;
	int			offset;
	MMCacheTag	tag;

	if (reln->rd_rel->relisshared)
		tag.mmct_dbid = (Oid) 0;
	else
		tag.mmct_dbid = MyDatabaseId;

	tag.mmct_relid = RelationGetRelid(reln);
	tag.mmct_blkno = blocknum;

	LWLockAcquire(MMCacheLock, LW_EXCLUSIVE);
	entry = (MMHashEntry *) hash_search(MMCacheHT,
										(void *) &tag,
										HASH_FIND, NULL);

	if (entry == (MMHashEntry *) NULL)
	{
		/* reading nonexistent pages is defined to fill them with zeroes */
		LWLockRelease(MMCacheLock);
		MemSet(buffer, 0, BLCKSZ);
		return SM_SUCCESS;
	}

	offset = (entry->mmhe_bufno * BLCKSZ);
	memmove(buffer, &MMBlockCache[offset], BLCKSZ);

	LWLockRelease(MMCacheLock);

	return SM_SUCCESS;
}

/*
 *	mmwrite() -- Write the supplied block at the appropriate location.
 *
 *		Returns SM_SUCCESS or SM_FAIL.
 */
int
mmwrite(Relation reln, BlockNumber blocknum, char *buffer)
{
	MMHashEntry *entry;
	int			offset;
	MMCacheTag	tag;

	if (reln->rd_rel->relisshared)
		tag.mmct_dbid = (Oid) 0;
	else
		tag.mmct_dbid = MyDatabaseId;

	tag.mmct_relid = RelationGetRelid(reln);
	tag.mmct_blkno = blocknum;

	LWLockAcquire(MMCacheLock, LW_EXCLUSIVE);
	entry = (MMHashEntry *) hash_search(MMCacheHT,
										(void *) &tag,
										HASH_FIND, NULL);

	if (entry == (MMHashEntry *) NULL)
	{
		LWLockRelease(MMCacheLock);
		elog(FATAL, "cache hash table missing requested page");
	}

	offset = (entry->mmhe_bufno * BLCKSZ);
	memmove(&MMBlockCache[offset], buffer, BLCKSZ);

	LWLockRelease(MMCacheLock);

	return SM_SUCCESS;
}

/*
 *	mmblindwrt() -- Write a block to stable storage blind.
 *
 *		We have to be able to do this using only the rnode of the relation
 *		in which the block belongs.  Otherwise this is much like mmwrite().
 */
int
mmblindwrt(RelFileNode rnode,
		   BlockNumber blkno,
		   char *buffer)
{
	return SM_FAIL;
}

/*
 *	mmnblocks() -- Get the number of blocks stored in a relation.
 *
 *		Returns # of blocks or InvalidBlockNumber on error.
 */
BlockNumber
mmnblocks(Relation reln)
{
	MMRelTag	rtag;
	MMRelHashEntry *rentry;
	BlockNumber nblocks;

	if (reln->rd_rel->relisshared)
		rtag.mmrt_dbid = (Oid) 0;
	else
		rtag.mmrt_dbid = MyDatabaseId;

	rtag.mmrt_relid = RelationGetRelid(reln);

	LWLockAcquire(MMCacheLock, LW_EXCLUSIVE);

	rentry = (MMRelHashEntry *) hash_search(MMRelCacheHT,
											(void *) &rtag,
											HASH_FIND, NULL);

	if (rentry)
		nblocks = rentry->mmrhe_nblocks;
	else
		nblocks = InvalidBlockNumber;

	LWLockRelease(MMCacheLock);

	return nblocks;
}

/*
 *	mmcommit() -- Commit a transaction.
 *
 *		Returns SM_SUCCESS or SM_FAIL with errno set as appropriate.
 */
int
mmcommit(void)
{
	return SM_SUCCESS;
}

/*
 *	mmabort() -- Abort a transaction.
 */

int
mmabort(void)
{
	return SM_SUCCESS;
}

/*
 *	MMShmemSize() -- Declare amount of shared memory we require.
 *
 *		The shared memory initialization code creates a block of shared
 *		memory exactly big enough to hold all the structures it needs to.
 *		This routine declares how much space the main memory storage
 *		manager will use.
 */
int
MMShmemSize(void)
{
	int			size = 0;

	/*
	 * first compute space occupied by the (dbid,relid,blkno) hash table
	 */
	size += hash_estimate_size(MMNBUFFERS, sizeof(MMHashEntry));

	/*
	 * now do the same for the rel hash table
	 */
	size += hash_estimate_size(MMNRELATIONS, sizeof(MMRelHashEntry));

	/*
	 * finally, add in the memory block we use directly
	 */

	size += MAXALIGN(BLCKSZ * MMNBUFFERS);
	size += MAXALIGN(sizeof(*MMCurTop));
	size += MAXALIGN(sizeof(*MMCurRelno));
	size += MAXALIGN(MMNBUFFERS * sizeof(MMCacheTag));

	return size;
}

#endif   /* STABLE_MEMORY_STORAGE */
