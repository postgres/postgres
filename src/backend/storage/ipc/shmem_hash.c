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
#include "utils/memutils.h"

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
 * ShmemRequestHash -- Request a shared memory hash table.
 *
 * Similar to ShmemRequestStruct(), but requests a hash table instead of an
 * opaque area.
 */
void
ShmemRequestHashWithOpts(const ShmemHashOpts *options)
{
	ShmemHashOpts *options_copy;

	Assert(options->name != NULL);

	options_copy = MemoryContextAlloc(TopMemoryContext,
									  sizeof(ShmemHashOpts));
	memcpy(options_copy, options, sizeof(ShmemHashOpts));

	/* Set options for the fixed-size area holding the hash table */
	options_copy->base.name = options->name;
	options_copy->base.size = hash_estimate_size(options_copy->nelems,
												 options_copy->hash_info.entrysize);

	ShmemRequestInternal(&options_copy->base, SHMEM_KIND_HASH);
}

void
shmem_hash_init(void *location, ShmemStructOpts *base_options)
{
	ShmemHashOpts *options = (ShmemHashOpts *) base_options;
	int			hash_flags = options->hash_flags;
	HTAB	   *htab;

	options->hash_info.hctl = location;
	htab = shmem_hash_create(location, options->base.size, false,
							 options->name,
							 options->nelems, &options->hash_info, hash_flags);

	if (options->ptr)
		*options->ptr = htab;
}

void
shmem_hash_attach(void *location, ShmemStructOpts *base_options)
{
	ShmemHashOpts *options = (ShmemHashOpts *) base_options;
	int			hash_flags = options->hash_flags;
	HTAB	   *htab;

	/* attach to it rather than allocate and initialize new space */
	hash_flags |= HASH_ATTACH;
	options->hash_info.hctl = location;
	Assert(options->hash_info.hctl != NULL);
	htab = shmem_hash_create(location, options->base.size, true,
							 options->name,
							 options->nelems, &options->hash_info, hash_flags);

	if (options->ptr)
		*options->ptr = htab;
}

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
 * Note: This is a legacy interface, kept for backwards compatibility with
 * extensions.  Use ShmemRequestHash() in new code!
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

	/*
	 * Look it up in the shmem index or allocate.
	 *
	 * NOTE: The area is requested internally as SHMEM_KIND_STRUCT instead of
	 * SHMEM_KIND_HASH.  That's correct because we do the hash table
	 * initialization by calling shmem_hash_create() ourselves.  (We don't
	 * expose the request kind to users; if we did, that would be confusing.)
	 */
	location = ShmemInitStruct(name, size, &found);

	return shmem_hash_create(location, size, found,
							 name, nelems, infoP, hash_flags);
}

/*
 * Initialize or attach to a shared hash table in the given shmem region.
 *
 * This is exposed to allow InitShmemAllocator() to share the logic for
 * bootstrapping the ShmemIndex hash table.
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
