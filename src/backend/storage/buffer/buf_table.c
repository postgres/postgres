/*-------------------------------------------------------------------------
 *
 * buf_table.c
 *	  routines for mapping BufferTags to buffer indexes.
 *
 * NOTE: this module is called only by freelist.c, and the "buffer IDs"
 * it deals with are whatever freelist.c needs them to be; they may not be
 * directly equivalent to Buffer numbers.
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
 *	  $PostgreSQL: pgsql/src/backend/storage/buffer/buf_table.c,v 1.39 2005/02/03 23:29:11 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/buf_internals.h"
#include "storage/bufmgr.h"


/* entry for buffer lookup hashtable */
typedef struct
{
	BufferTag	key;			/* Tag of a disk page */
	int			id;				/* Associated buffer ID */
} BufferLookupEnt;

static HTAB *SharedBufHash;


/*
 * Estimate space needed for mapping hashtable
 *		size is the desired hash table size (possibly more than NBuffers)
 */
int
BufTableShmemSize(int size)
{
	return hash_estimate_size(size, sizeof(BufferLookupEnt));
}

/*
 * Initialize shmem hash table for mapping buffers
 *		size is the desired hash table size (possibly more than NBuffers)
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
 *		Lookup the given BufferTag; return buffer ID, or -1 if not found
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
 *		Insert a hashtable entry for given tag and buffer ID
 */
void
BufTableInsert(BufferTag *tagPtr, int buf_id)
{
	BufferLookupEnt *result;
	bool		found;

	result = (BufferLookupEnt *)
		hash_search(SharedBufHash, (void *) tagPtr, HASH_ENTER, &found);

	if (!result)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory")));

	if (found)					/* found something already in the table? */
		elog(ERROR, "shared buffer hash table corrupted");

	result->id = buf_id;
}

/*
 * BufTableDelete
 *		Delete the hashtable entry for given tag (which must exist)
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
