/*-------------------------------------------------------------------------
 *
 * buf_table.c
 *	  routines for finding buffers in the buffer pool.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/buffer/buf_table.c,v 1.30 2003/11/13 00:40:01 wieck Exp $
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
InitBufTable(int size)
{
	HASHCTL		info;

	/* assume lock is held */

	/* BufferTag maps to Buffer */
	info.keysize = sizeof(BufferTag);
	info.entrysize = sizeof(BufferLookupEnt);
	info.hash = tag_hash;

	SharedBufHash = ShmemInitHash("Shared Buffer Lookup Table",
								  size, size,
								  &info,
								  HASH_ELEM | HASH_FUNCTION);

	if (!SharedBufHash)
		elog(FATAL, "could not initialize shared buffer hash table");
}

/*
 * BufTableLookup
 */
int
BufTableLookup(BufferTag *tagPtr)
{
	BufferLookupEnt *result;

	if (tagPtr->blockNum == P_NEW)
		return -1;

	result = (BufferLookupEnt *)
		hash_search(SharedBufHash, (void *) tagPtr, HASH_FIND, NULL);
	if (!result)
		return -1;

	return result->id;
}

/*
 * BufTableDelete
 */
bool
BufTableInsert(BufferTag *tagPtr, Buffer buf_id)
{
	BufferLookupEnt *result;
	bool		found;

	result = (BufferLookupEnt *)
		hash_search(SharedBufHash, (void *) tagPtr, HASH_ENTER, &found);

	if (!result)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory")));

	if (found)					/* found something else in the table? */
		elog(ERROR, "shared buffer hash table corrupted");

	result->id = buf_id;
	return TRUE;
}

/*
 * BufTableDelete
 */
bool
BufTableDelete(BufferTag *tagPtr)
{
	BufferLookupEnt *result;

	result = (BufferLookupEnt *)
		hash_search(SharedBufHash, (void *) tagPtr, HASH_REMOVE, NULL);

	if (!result)				/* shouldn't happen */
		elog(ERROR, "shared buffer hash table corrupted");

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
