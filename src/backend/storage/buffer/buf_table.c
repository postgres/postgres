/*-------------------------------------------------------------------------
 *
 * buf_table.c--
 *	  routines for finding buffers in the buffer pool.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/buffer/buf_table.c,v 1.8 1997/09/08 21:46:46 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * OLD COMMENTS
 *
 * Data Structures:
 *
 *		Buffers are identified by their BufferTag (buf.h).	This
 * file contains routines for allocating a shmem hash table to
 * map buffer tags to buffer descriptors.
 *
 * Synchronization:
 *
 *	All routines in this file assume buffer manager spinlock is
 *	held by their caller.
 */

#include "postgres.h"

#include "storage/bufmgr.h"
#include "storage/buf_internals.h"		/* where the declarations go */
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/hsearch.h"

static HTAB *SharedBufHash;

typedef struct lookup
{
	BufferTag	key;
	Buffer		id;
} LookupEnt;

/*
 * Initialize shmem hash table for mapping buffers
 */
void
InitBufTable()
{
	HASHCTL		info;
	int			hash_flags;

	/* assume lock is held */

	/* BufferTag maps to Buffer */
	info.keysize = sizeof(BufferTag);
	info.datasize = sizeof(Buffer);
	info.hash = tag_hash;

	hash_flags = (HASH_ELEM | HASH_FUNCTION);


	SharedBufHash = (HTAB *) ShmemInitHash("Shared Buf Lookup Table",
										   NBuffers, NBuffers,
										   &info, hash_flags);

	if (!SharedBufHash)
	{
		elog(FATAL, "couldn't initialize shared buffer pool Hash Tbl");
		exit(1);
	}

}

BufferDesc *
BufTableLookup(BufferTag *tagPtr)
{
	LookupEnt  *result;
	bool		found;

	if (tagPtr->blockNum == P_NEW)
		return (NULL);

	result = (LookupEnt *)
		hash_search(SharedBufHash, (char *) tagPtr, HASH_FIND, &found);

	if (!result)
	{
		elog(WARN, "BufTableLookup: BufferLookup table corrupted");
		return (NULL);
	}
	if (!found)
	{
		return (NULL);
	}
	return (&(BufferDescriptors[result->id]));
}

/*
 * BufTableDelete
 */
bool
BufTableDelete(BufferDesc *buf)
{
	LookupEnt  *result;
	bool		found;

	/*
	 * buffer not initialized or has been removed from table already.
	 * BM_DELETED keeps us from removing buffer twice.
	 */
	if (buf->flags & BM_DELETED)
	{
		return (TRUE);
	}

	buf->flags |= BM_DELETED;

	result = (LookupEnt *)
		hash_search(SharedBufHash, (char *) &(buf->tag), HASH_REMOVE, &found);

	if (!(result && found))
	{
		elog(WARN, "BufTableDelete: BufferLookup table corrupted");
		return (FALSE);
	}

	return (TRUE);
}

bool
BufTableInsert(BufferDesc *buf)
{
	LookupEnt  *result;
	bool		found;

	/* cannot insert it twice */
	Assert(buf->flags & BM_DELETED);
	buf->flags &= ~(BM_DELETED);

	result = (LookupEnt *)
		hash_search(SharedBufHash, (char *) &(buf->tag), HASH_ENTER, &found);

	if (!result)
	{
		Assert(0);
		elog(WARN, "BufTableInsert: BufferLookup table corrupted");
		return (FALSE);
	}
	/* found something else in the table ! */
	if (found)
	{
		Assert(0);
		elog(WARN, "BufTableInsert: BufferLookup table corrupted");
		return (FALSE);
	}

	result->id = buf->buf_id;
	return (TRUE);
}

/* prints out collision stats for the buf table */
#ifdef NOT_USED
void
DBG_LookupListCheck(int nlookup)
{
	nlookup = 10;

	hash_stats("Shared", SharedBufHash);
}

#endif
