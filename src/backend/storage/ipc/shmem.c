/*-------------------------------------------------------------------------
 *
 * shmem.c
 *	  create shared memory and initialize shared memory data structures.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/ipc/shmem.c,v 1.73 2003/10/16 20:59:35 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * POSTGRES processes share one or more regions of shared memory.
 * The shared memory is created by a postmaster and is inherited
 * by each backend via fork().	The routines in this file are used for
 * allocating and binding to shared memory data structures.
 *
 * NOTES:
 *		(a) There are three kinds of shared memory data structures
 *	available to POSTGRES: fixed-size structures, queues and hash
 *	tables.  Fixed-size structures contain things like global variables
 *	for a module and should never be allocated after the process
 *	initialization phase.  Hash tables have a fixed maximum size, but
 *	their actual size can vary dynamically.  When entries are added
 *	to the table, more space is allocated.	Queues link data structures
 *	that have been allocated either as fixed size structures or as hash
 *	buckets.  Each shared data structure has a string name to identify
 *	it (assigned in the module that declares it).
 *
 *		(b) During initialization, each module looks for its
 *	shared data structures in a hash table called the "Shmem Index".
 *	If the data structure is not present, the caller can allocate
 *	a new one and initialize it.  If the data structure is present,
 *	the caller "attaches" to the structure by initializing a pointer
 *	in the local address space.
 *		The shmem index has two purposes: first, it gives us
 *	a simple model of how the world looks when a backend process
 *	initializes.  If something is present in the shmem index,
 *	it is initialized.	If it is not, it is uninitialized.	Second,
 *	the shmem index allows us to allocate shared memory on demand
 *	instead of trying to preallocate structures and hard-wire the
 *	sizes and locations in header files.  If you are using a lot
 *	of shared memory in a lot of different places (and changing
 *	things during development), this is important.
 *
 *		(c) memory allocation model: shared memory can never be
 *	freed, once allocated.	 Each hash table has its own free list,
 *	so hash buckets can be reused when an item is deleted.	However,
 *	if one hash table grows very large and then shrinks, its space
 *	cannot be redistributed to other tables.  We could build a simple
 *	hash bucket garbage collector if need be.  Right now, it seems
 *	unnecessary.
 *
 *		See InitSem() in sem.c for an example of how to use the
 *	shmem index.
 */

#include "postgres.h"

#include "access/transam.h"
#include "storage/pg_shmem.h"
#include "storage/spin.h"
#include "utils/tqual.h"


/* shared memory global variables */

static PGShmemHeader *ShmemSegHdr;		/* shared mem segment header */

SHMEM_OFFSET ShmemBase;			/* start address of shared memory */

static SHMEM_OFFSET ShmemEnd;	/* end+1 address of shared memory */

static slock_t *ShmemLock;		/* spinlock for shared memory allocation */

static HTAB *ShmemIndex = NULL; /* primary index hashtable for shmem */

static bool ShmemBootstrap = false;		/* bootstrapping shmem index? */


/*
 *	InitShmemAllocation() --- set up shared-memory allocation.
 *
 * Note: the argument should be declared "PGShmemHeader *seghdr",
 * but we use void to avoid having to include ipc.h in shmem.h.
 */
void
InitShmemAllocation(void *seghdr)
{
	PGShmemHeader *shmhdr = (PGShmemHeader *) seghdr;

	/* Set up basic pointers to shared memory */
	ShmemSegHdr = shmhdr;
	ShmemBase = (SHMEM_OFFSET) shmhdr;
	ShmemEnd = ShmemBase + shmhdr->totalsize;

	/*
	 * Initialize the spinlock used by ShmemAlloc.	We have to do the
	 * space allocation the hard way, since ShmemAlloc can't be called
	 * yet.
	 */
	ShmemLock = (slock_t *) (((char *) shmhdr) + shmhdr->freeoffset);
	shmhdr->freeoffset += MAXALIGN(sizeof(slock_t));
	Assert(shmhdr->freeoffset <= shmhdr->totalsize);

	SpinLockInit(ShmemLock);

	/* ShmemIndex can't be set up yet (need LWLocks first) */
	ShmemIndex = (HTAB *) NULL;

	/*
	 * Initialize ShmemVariableCache for transaction manager.
	 */
	ShmemVariableCache = (VariableCache)
		ShmemAlloc(sizeof(*ShmemVariableCache));
	memset(ShmemVariableCache, 0, sizeof(*ShmemVariableCache));
}

/*
 * ShmemAlloc -- allocate max-aligned chunk from shared memory
 *
 * Assumes ShmemLock and ShmemSegHdr are initialized.
 *
 * Returns: real pointer to memory or NULL if we are out
 *		of space.  Has to return a real pointer in order
 *		to be compatible with malloc().
 */
void *
ShmemAlloc(Size size)
{
	uint32		newStart;
	uint32		newFree;
	void	   *newSpace;

	/* use volatile pointer to prevent code rearrangement */
	volatile PGShmemHeader *shmemseghdr = ShmemSegHdr;

	/*
	 * ensure all space is adequately aligned.
	 */
	size = MAXALIGN(size);

	Assert(shmemseghdr != NULL);

	SpinLockAcquire(ShmemLock);

	newStart = shmemseghdr->freeoffset;

	/* extra alignment for large requests, since they are probably buffers */
	if (size >= BLCKSZ)
		newStart = BUFFERALIGN(newStart);

	newFree = newStart + size;
	if (newFree <= shmemseghdr->totalsize)
	{
		newSpace = (void *) MAKE_PTR(newStart);
		shmemseghdr->freeoffset = newFree;
	}
	else
		newSpace = NULL;

	SpinLockRelease(ShmemLock);

	if (!newSpace)
		ereport(WARNING,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory")));

	return newSpace;
}

/*
 * ShmemIsValid -- test if an offset refers to valid shared memory
 *
 * Returns TRUE if the pointer is valid.
 */
bool
ShmemIsValid(unsigned long addr)
{
	return (addr < ShmemEnd) && (addr >= ShmemBase);
}

/*
 *	InitShmemIndex() --- set up shmem index table.
 */
void
InitShmemIndex(void)
{
	HASHCTL		info;
	int			hash_flags;
	ShmemIndexEnt *result,
				item;
	bool		found;

	/*
	 * Since ShmemInitHash calls ShmemInitStruct, which expects the
	 * ShmemIndex hashtable to exist already, we have a bit of a
	 * circularity problem in initializing the ShmemIndex itself.  We set
	 * ShmemBootstrap to tell ShmemInitStruct to fake it.
	 */
	ShmemBootstrap = true;

	/* create the shared memory shmem index */
	info.keysize = SHMEM_INDEX_KEYSIZE;
	info.entrysize = sizeof(ShmemIndexEnt);
	hash_flags = HASH_ELEM;

	/* This will acquire the shmem index lock, but not release it. */
	ShmemIndex = ShmemInitHash("ShmemIndex",
							   SHMEM_INDEX_SIZE, SHMEM_INDEX_SIZE,
							   &info, hash_flags);
	if (!ShmemIndex)
		elog(FATAL, "could not initialize Shmem Index");

	/*
	 * Now, create an entry in the hashtable for the index itself.
	 */
	MemSet(item.key, 0, SHMEM_INDEX_KEYSIZE);
	strncpy(item.key, "ShmemIndex", SHMEM_INDEX_KEYSIZE);

	result = (ShmemIndexEnt *)
		hash_search(ShmemIndex, (void *) &item, HASH_ENTER, &found);
	if (!result)
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory")));

	Assert(ShmemBootstrap && !found);

	result->location = MAKE_OFFSET(ShmemIndex->hctl);
	result->size = SHMEM_INDEX_SIZE;

	ShmemBootstrap = false;

	/* now release the lock acquired in ShmemInitStruct */
	LWLockRelease(ShmemIndexLock);
}

/*
 * ShmemInitHash -- Create/Attach to and initialize
 *		shared memory hash table.
 *
 * Notes:
 *
 * assume caller is doing some kind of synchronization
 * so that two people dont try to create/initialize the
 * table at once.
 */
HTAB *
ShmemInitHash(const char *name, /* table string name for shmem index */
			  long init_size,	/* initial table size */
			  long max_size,	/* max size of the table */
			  HASHCTL *infoP,	/* info about key and bucket size */
			  int hash_flags)	/* info about infoP */
{
	bool		found;
	void	   *location;

	/*
	 * Hash tables allocated in shared memory have a fixed directory; it
	 * can't grow or other backends wouldn't be able to find it. So, make
	 * sure we make it big enough to start with.
	 *
	 * The shared memory allocator must be specified too.
	 */
	infoP->dsize = infoP->max_dsize = hash_select_dirsize(max_size);
	infoP->alloc = ShmemAlloc;
	hash_flags |= HASH_SHARED_MEM | HASH_DIRSIZE;

	/* look it up in the shmem index */
	location = ShmemInitStruct(name,
					sizeof(HASHHDR) + infoP->dsize * sizeof(HASHSEGMENT),
							   &found);

	/*
	 * shmem index is corrupted.	Let someone else give the error
	 * message since they have more information
	 */
	if (location == NULL)
		return NULL;

	/*
	 * if it already exists, attach to it rather than allocate and
	 * initialize new space
	 */
	if (found)
		hash_flags |= HASH_ATTACH;

	/* Now provide the header and directory pointers */
	infoP->hctl = (HASHHDR *) location;
	infoP->dir = (HASHSEGMENT *) (((char *) location) + sizeof(HASHHDR));

	return hash_create(name, init_size, infoP, hash_flags);
}

/*
 * ShmemInitStruct -- Create/attach to a structure in shared
 *		memory.
 *
 *	This is called during initialization to find or allocate
 *		a data structure in shared memory.	If no other processes
 *		have created the structure, this routine allocates space
 *		for it.  If it exists already, a pointer to the existing
 *		table is returned.
 *
 *	Returns: real pointer to the object.  FoundPtr is TRUE if
 *		the object is already in the shmem index (hence, already
 *		initialized).
 */
void *
ShmemInitStruct(const char *name, Size size, bool *foundPtr)
{
	ShmemIndexEnt *result,
				item;
	void	   *structPtr;

	strncpy(item.key, name, SHMEM_INDEX_KEYSIZE);
	item.location = BAD_LOCATION;

	LWLockAcquire(ShmemIndexLock, LW_EXCLUSIVE);

	if (!ShmemIndex)
	{
		/*
		 * If the shmem index doesn't exist, we are bootstrapping: we must
		 * be trying to init the shmem index itself.
		 *
		 * Notice that the ShmemIndexLock is held until the shmem index has
		 * been completely initialized.
		 */
		Assert(strcmp(name, "ShmemIndex") == 0);
		Assert(ShmemBootstrap);
		*foundPtr = FALSE;
		return ShmemAlloc(size);
	}

	/* look it up in the shmem index */
	result = (ShmemIndexEnt *)
		hash_search(ShmemIndex, (void *) &item, HASH_ENTER, foundPtr);

	if (!result)
	{
		LWLockRelease(ShmemIndexLock);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory")));
		return NULL;
	}

	if (*foundPtr)
	{
		/*
		 * Structure is in the shmem index so someone else has allocated
		 * it already.	The size better be the same as the size we are
		 * trying to initialize to or there is a name conflict (or worse).
		 */
		if (result->size != size)
		{
			LWLockRelease(ShmemIndexLock);

			elog(WARNING, "ShmemIndex entry size is wrong");
			/* let caller print its message too */
			return NULL;
		}
		structPtr = (void *) MAKE_PTR(result->location);
	}
	else
	{
		/* It isn't in the table yet. allocate and initialize it */
		structPtr = ShmemAlloc(size);
		if (!structPtr)
		{
			/* out of memory */
			Assert(ShmemIndex);
			hash_search(ShmemIndex, (void *) &item, HASH_REMOVE, NULL);
			LWLockRelease(ShmemIndexLock);

			ereport(WARNING,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("could not allocate shared memory segment \"%s\"", name)));
			*foundPtr = FALSE;
			return NULL;
		}
		result->size = size;
		result->location = MAKE_OFFSET(structPtr);
	}
	Assert(ShmemIsValid((unsigned long) structPtr));

	LWLockRelease(ShmemIndexLock);
	return structPtr;
}
