/*-------------------------------------------------------------------------
 *
 * buf_table.c
 *	  routines for finding buffers in the buffer pool.
 *
 * NOTE: these days, what this table actually provides is a mapping from
 * BufferTags to CDB indexes, not directly to buffers.	The function names
 * are thus slight misnomers.
 *
 * Note: all routines in this file assume that the BufMgrLock is held
 * by the caller, so no synchronization is needed.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/buffer/buf_table.c,v 1.38 2004/12/31 22:00:49 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/buf_internals.h"
#include "storage/bufmgr.h"


static HTAB *SharedBufHash;


/*
 * Initialize shmem hash table for mapping buffers
 *		size is the desired hash table size (2*NBuffers for ARC algorithm)
 */
void
InitBufTable(int size)
{
	HASHCTL		info;

	/* assume no locking is needed yet */

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
 *		Lookup the given BufferTag; return CDB index, or -1 if not found
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
 * BufTableInsert
 *		Insert a hashtable entry for given tag and CDB index
 */
void
BufTableInsert(BufferTag *tagPtr, int cdb_id)
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

	result->id = cdb_id;
}

/*
 * BufTableDelete
 *		Delete the hashtable entry for given tag
 */
void
BufTableDelete(BufferTag *tagPtr)
{
	BufferLookupEnt *result;

	result = (BufferLookupEnt *)
		hash_search(SharedBufHash, (void *) tagPtr, HASH_REMOVE, NULL);

	if (!result)				/* shouldn't happen */
		elog(ERROR, "shared buffer hash table corrupted");
}
