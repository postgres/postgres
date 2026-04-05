/*-------------------------------------------------------------------------
 *
 * shmem_hash.c
 *	  hash table implementation in shared memory
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * A shared memory hash table implementation on top of the named, fixed-size
 * shared memory areas managed by shmem.c.  Each hash table has its own free
 * list, so hash buckets can be reused when an item is deleted.
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/shmem_hash.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "storage/shmem.h"
#include "storage/shmem_internal.h"

/*
 * A very simple allocator used to carve out different parts of a hash table
 * from a previously allocated contiguous shared memory area.
 */
typedef struct shmem_hash_allocator
{
	char	   *next;			/* start of free space in the area */
	char	   *end;			/* end of the shmem area */
} shmem_hash_allocator;

static void *ShmemHashAlloc(Size size, void *alloc_arg);

/*
 * ShmemInitHash -- Create and initialize, or attach to, a
 *		shared memory hash table.
 *
 * We assume caller is doing some kind of synchronization
 * so that two processes don't try to create/initialize the same
 * table at once.  (In practice, all creations are done in the postmaster
 * process; child processes should always be attaching to existing tables.)
 *
 * nelems is the maximum number of hashtable entries.
 *
 * *infoP and hash_flags must specify at least the entry sizes and key
 * comparison semantics (see hash_create()).  Flag bits and values specific
 * to shared-memory hash tables are added here, except that callers may
 * choose to specify HASH_PARTITION.
 *
 * Note: before Postgres 9.0, this function returned NULL for some failure
 * cases.  Now, it always throws error instead, so callers need not check
 * for NULL.
 */
HTAB *
ShmemInitHash(const char *name,		/* table string name for shmem index */
			  int64 nelems,		/* size of the table */
			  HASHCTL *infoP,	/* info about key and bucket size */
			  int hash_flags)	/* info about infoP */
{
	bool		found;
	size_t		size;
	void	   *location;

	size = hash_estimate_size(nelems, infoP->entrysize);

	/* look it up in the shmem index or allocate */
	location = ShmemInitStruct(name, size, &found);

	return shmem_hash_create(location, size, found,
							 name, nelems, infoP, hash_flags);
}

/*
 * Initialize or attach to a shared hash table in the given shmem region.
 *
 * This is extracted from ShmemInitHash() to allow InitShmemAllocator() to
 * share the logic for bootstrapping the ShmemIndex hash table.
 */
HTAB *
shmem_hash_create(void *location, size_t size, bool found,
				  const char *name, int64 nelems, HASHCTL *infoP, int hash_flags)
{
	shmem_hash_allocator allocator;

	/*
	 * Hash tables allocated in shared memory have a fixed directory and have
	 * all elements allocated upfront.  We don't support growing because we'd
	 * need to grow the underlying shmem region with it.
	 *
	 * The shared memory allocator must be specified too.
	 */
	infoP->alloc = ShmemHashAlloc;
	infoP->alloc_arg = NULL;
	hash_flags |= HASH_SHARED_MEM | HASH_ALLOC | HASH_FIXED_SIZE;

	/*
	 * if it already exists, attach to it rather than allocate and initialize
	 * new space
	 */
	if (!found)
	{
		allocator.next = (char *) location;
		allocator.end = (char *) location + size;
		infoP->alloc_arg = &allocator;
	}
	else
	{
		/* Pass location of hashtable header to hash_create */
		infoP->hctl = (HASHHDR *) location;
		hash_flags |= HASH_ATTACH;
	}

	return hash_create(name, nelems, infoP, hash_flags);
}

/*
 * ShmemHashAlloc -- alloc callback for shared memory hash tables
 *
 * Carve out the allocation from a pre-allocated region.  All shared memory
 * hash tables are initialized with HASH_FIXED_SIZE, so all the allocations
 * happen upfront during initialization and no locking is required.
 */
static void *
ShmemHashAlloc(Size size, void *alloc_arg)
{
	shmem_hash_allocator *allocator = (shmem_hash_allocator *) alloc_arg;
	void	   *result;

	size = MAXALIGN(size);

	if (allocator->end - allocator->next < size)
		return NULL;
	result = allocator->next;
	allocator->next += size;

	return result;
}
