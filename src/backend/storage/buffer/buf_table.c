/*-------------------------------------------------------------------------
 *
 * buf_table.c
 *	  routines for finding buffers in the buffer pool.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/buffer/buf_table.c,v 1.23 2001/10/01 05:36:13 tgl Exp $
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
 *	All routines in this file assume BufMgrLock is held by their caller.
 */

#include "postgres.h"

#include "storage/buf_internals.h"
#include "storage/bufmgr.h"

static HTAB *SharedBufHash;


/*
 * Initialize shmem hash table for mapping buffers
 */
void
InitBufTable(void)
{
	HASHCTL		info;

	/* assume lock is held */

	/* BufferTag maps to Buffer */
	info.keysize = sizeof(BufferTag);
	info.entrysize = sizeof(BufferLookupEnt);
	info.hash = tag_hash;

	SharedBufHash = ShmemInitHash("Shared Buffer Lookup Table",
								  NBuffers, NBuffers,
								  &info,
								  HASH_ELEM | HASH_FUNCTION);

	if (!SharedBufHash)
		elog(FATAL, "couldn't initialize shared buffer pool Hash Tbl");
}

BufferDesc *
BufTableLookup(BufferTag *tagPtr)
{
	BufferLookupEnt  *result;
	bool		found;

	if (tagPtr->blockNum == P_NEW)
		return NULL;

	result = (BufferLookupEnt *)
		hash_search(SharedBufHash, (void *) tagPtr, HASH_FIND, &found);

	if (!result)
	{
		elog(ERROR, "BufTableLookup: BufferLookup table corrupted");
		return NULL;
	}
	if (!found)
		return NULL;
	return &(BufferDescriptors[result->id]);
}

/*
 * BufTableDelete
 */
bool
BufTableDelete(BufferDesc *buf)
{
	BufferLookupEnt  *result;
	bool		found;

	/*
	 * buffer not initialized or has been removed from table already.
	 * BM_DELETED keeps us from removing buffer twice.
	 */
	if (buf->flags & BM_DELETED)
		return TRUE;

	buf->flags |= BM_DELETED;

	result = (BufferLookupEnt *)
		hash_search(SharedBufHash, (void *) &(buf->tag), HASH_REMOVE, &found);

	if (!(result && found))
	{
		elog(ERROR, "BufTableDelete: BufferLookup table corrupted");
		return FALSE;
	}

	/*
	 * Clear the buffer's tag.  This doesn't matter for the hash table,
	 * since the buffer is already removed from it, but it ensures that
	 * sequential searches through the buffer table won't think the buffer
	 * is still valid for its old page.
	 */
	buf->tag.rnode.relNode = InvalidOid;
	buf->tag.rnode.tblNode = InvalidOid;

	return TRUE;
}

bool
BufTableInsert(BufferDesc *buf)
{
	BufferLookupEnt  *result;
	bool		found;

	/* cannot insert it twice */
	Assert(buf->flags & BM_DELETED);
	buf->flags &= ~(BM_DELETED);

	result = (BufferLookupEnt *)
		hash_search(SharedBufHash, (void *) &(buf->tag), HASH_ENTER, &found);

	if (!result)
	{
		Assert(0);
		elog(ERROR, "BufTableInsert: BufferLookup table corrupted");
		return FALSE;
	}
	/* found something else in the table ! */
	if (found)
	{
		Assert(0);
		elog(ERROR, "BufTableInsert: BufferLookup table corrupted");
		return FALSE;
	}

	result->id = buf->buf_id;
	return TRUE;
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
