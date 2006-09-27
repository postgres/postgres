/*-------------------------------------------------------------------------
 *
 * shmem.c
 *	  create shared memory and initialize shared memory data structures.
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/ipc/shmem.c,v 1.96 2006/09/27 18:40:09 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * POSTGRES processes share one or more regions of shared memory.
 * The shared memory is created by a postmaster and is inherited
 * by each backend via fork() (or, in some ports, via other OS-specific
 * methods).  The routines in this file are used for allocating and
 * binding to shared memory data structures.
 *
 * NOTES:
 *		(a) There are three kinds of shared memory data structures
 *	available to POSTGRES: fixed-size structures, queues and hash
 *	tables.  Fixed-size structures contain things like global variables
 *	for a module and should never be allocated after the shared memory
 *	initialization phase.  Hash tables have a fixed maximum size, but
 *	their actual size can vary dynamically.  When entries are added
 *	to the table, more space is allocated.	Queues link data structures
 *	that have been allocated either within fixed-size structures or as hash
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
 *		(c) In standard Unix-ish environments, individual backends do not
 *	need to re-establish their local pointers into shared memory, because
 *	they inherit correct values of those variables via fork() from the
 *	postmaster.  However, this does not work in the EXEC_BACKEND case.
 *	In ports using EXEC_BACKEND, new backends have to set up their local
 *	pointers using the method described in (b) above.

 *		(d) memory allocation model: shared memory can never be
 *	freed, once allocated.	 Each hash table has its own free list,
 *	so hash buckets can be reused when an item is deleted.	However,
 *	if one hash table grows very large and then shrinks, its space
 *	cannot be redistributed to other tables.  We could build a simple
 *	hash bucket garbage collector if need be.  Right now, it seems
 *	unnecessary.
 *
 *      (e) Add-ins can request their own logical shared memory segments
 *  by calling RegisterAddinContext() from the preload-libraries hook.
 *  Each call establishes a uniquely named add-in shared memopry
 *  context which will be set up as part of postgres intialisation.
 *  Memory can be allocated from these contexts using
 *  ShmemAllocFromContext(), and can be reset to its initial condition
 *  using ShmemResetContext().  Also, RegisterAddinLWLock(LWLockid *lock_ptr)
 *  can be used to request that a LWLock be allocated, placed into *lock_ptr.
 */

#include "postgres.h"

#include "access/transam.h"
#include "miscadmin.h"
#include "storage/lwlock.h"
#include "storage/pg_shmem.h"
#include "storage/shmem.h"
#include "storage/spin.h"


/* shared memory global variables */

static PGShmemHeader *ShmemSegHdr;		/* shared mem segment header */

SHMEM_OFFSET ShmemBase;			/* start address of shared memory */

static SHMEM_OFFSET ShmemEnd;	/* end+1 address of shared memory */

slock_t    *ShmemLock;			/* spinlock for shared memory and LWLock
								 * allocation */

static HTAB *ShmemIndex = NULL; /* primary index hashtable for shmem */

/* Structures and globals for managing add-in shared memory contexts */
typedef struct context
{
	char           *name;
	Size            size;
	PGShmemHeader  *seg_hdr;
	struct context *next;
} ContextNode;

static ContextNode *addin_contexts = NULL;
static Size addin_contexts_size = 0;



/*
 *	InitShmemAccess() --- set up basic pointers to shared memory.
 *
 * Note: the argument should be declared "PGShmemHeader *seghdr",
 * but we use void to avoid having to include ipc.h in shmem.h.
 */
void
InitShmemAccess(void *seghdr)
{
	PGShmemHeader *shmhdr = (PGShmemHeader *) seghdr;

	ShmemSegHdr = shmhdr;
	ShmemBase = (SHMEM_OFFSET) shmhdr;
	ShmemEnd = ShmemBase + shmhdr->totalsize;
}

/*
 *	InitShmemAllocation() --- set up shared-memory space allocation.
 *
 * This should be called only in the postmaster or a standalone backend.
 */
void
InitShmemAllocation(void)
{
	PGShmemHeader *shmhdr = ShmemSegHdr;

	Assert(shmhdr != NULL);

	/*
	 * Initialize the spinlock used by ShmemAlloc.  We have to do the
	 * space allocation the hard way, since obviously ShmemAlloc can't
	 * be called yet.
	 */
	ShmemLock = (slock_t *) (((char *) shmhdr) + shmhdr->freeoffset);
	shmhdr->freeoffset += MAXALIGN(sizeof(slock_t));
	Assert(shmhdr->freeoffset <= shmhdr->totalsize);

	SpinLockInit(ShmemLock);

	/* ShmemIndex can't be set up yet (need LWLocks first) */
	shmhdr->indexoffset = 0;
	ShmemIndex = (HTAB *) NULL;

	/*
	 * Initialize ShmemVariableCache for transaction manager.
	 * (This doesn't really belong here, but not worth moving.)
	 */
	ShmemVariableCache = (VariableCache)
	ShmemAlloc(sizeof(*ShmemVariableCache));
	memset(ShmemVariableCache, 0, sizeof(*ShmemVariableCache));
}

/*
 * RegisterAddinContext -- Register the requirement for a named shared
 *                         memory context.
 */
void
RegisterAddinContext(const char *name, Size size)
{
	char *newstr = malloc(strlen(name) + 1);
	ContextNode *node = malloc(sizeof(ContextNode));

	strcpy(newstr, name);
	node->name = newstr;

	/* Round up to typical page size */
	node->size = add_size(size, 8192 - (size % 8192));
	node->next = addin_contexts;

	addin_contexts = node;
	addin_contexts_size = add_size(addin_contexts_size, node->size);
}


/*
 * ContextFromName -- Return the ContextNode for the given named
 *                    context, or NULL if not found.
 */
static ContextNode *
ContextFromName(const char *name)
{
	ContextNode *context = addin_contexts;

	while (context)
	{
		if (strcmp(name, context->name) == 0)
			return context;
		context = context->next;
	}
	return NULL;
}

/*
 * InitAddinContexts -- Initialise the registered addin shared memory
 *                      contexts. 
 */
void
InitAddinContexts(void *start)
{
	PGShmemHeader *next_segment = (PGShmemHeader *) start;
	ContextNode *context = addin_contexts;

	while (context)
	{
		context->seg_hdr = next_segment;

		next_segment->totalsize = context->size;
		next_segment->freeoffset = MAXALIGN(sizeof(PGShmemHeader));

		next_segment = (PGShmemHeader *) 
			((char *) next_segment + context->size);
		context = context->next;
	}
}

/*
 * ShmemResetContext -- Re-initialise the named addin shared memory context.
 */
void
ShmemResetContext(const char *name)
{
	PGShmemHeader *segment;
	ContextNode *context = ContextFromName(name);

	if (!context)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("cannot reset unknown shared memory context %s",
						name)));

	segment = context->seg_hdr;
	segment->freeoffset = MAXALIGN(sizeof(PGShmemHeader));
}

/*
 * AddinShmemSize -- Report how much shared memory has been registered
 *  for add-ins.
 */
Size
AddinShmemSize(void)
{
	return addin_contexts_size;
}

/*
 * ShmemAllocFromContext -- allocate max-aligned chunk from shared memory
 *
 * Assumes ShmemLock and ShmemSegHdr are initialized.
 *
 * Returns: real pointer to memory or NULL if we are out
 *		of space.  Has to return a real pointer in order
 *		to be compatible with malloc().
 */
void *
ShmemAllocFromContext(Size size, const char *context_name)
{
	Size		  newStart;
	Size	 	  newFree;
	void	     *newSpace;
	ContextNode  *context;

	/* use volatile pointer to prevent code rearrangement */
	volatile PGShmemHeader *shmemseghdr = ShmemSegHdr;

	/* 
	 * if context_name is provided, allocate from the named context
	 */
	if (context_name)
	{
		context = ContextFromName(context_name);
		if (!context)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("cannot reset unknown shared memory context %s",
							context_name)));
		shmemseghdr = context->seg_hdr;
	}

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
		newSpace = (void *) MAKE_PTRFROM((SHMEM_OFFSET) shmemseghdr, newStart);
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
	return ShmemAllocFromContext(size, NULL);
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
 *	InitShmemIndex() --- set up or attach to shmem index table.
 */
void
InitShmemIndex(void)
{
	HASHCTL		info;
	int			hash_flags;

	/*
	 * Since ShmemInitHash calls ShmemInitStruct, which expects the ShmemIndex
	 * hashtable to exist already, we have a bit of a circularity problem in
	 * initializing the ShmemIndex itself.	The special "ShmemIndex" hash
	 * table name will tell ShmemInitStruct to fake it.
	 */

	/* create the shared memory shmem index */
	info.keysize = SHMEM_INDEX_KEYSIZE;
	info.entrysize = sizeof(ShmemIndexEnt);
	hash_flags = HASH_ELEM;

	ShmemIndex = ShmemInitHash("ShmemIndex",
							   SHMEM_INDEX_SIZE, SHMEM_INDEX_SIZE,
							   &info, hash_flags);
	if (!ShmemIndex)
		elog(FATAL, "could not initialize Shmem Index");
}

/*
 * ShmemInitHash -- Create and initialize, or attach to, a
 *		shared memory hash table.
 *
 * We assume caller is doing some kind of synchronization
 * so that two people don't try to create/initialize the
 * table at once.
 *
 * max_size is the estimated maximum number of hashtable entries.  This is
 * not a hard limit, but the access efficiency will degrade if it is
 * exceeded substantially (since it's used to compute directory size and
 * the hash table buckets will get overfull).
 *
 * init_size is the number of hashtable entries to preallocate.  For a table
 * whose maximum size is certain, this should be equal to max_size; that
 * ensures that no run-time out-of-shared-memory failures can occur.
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
	 * Hash tables allocated in shared memory have a fixed directory; it can't
	 * grow or other backends wouldn't be able to find it. So, make sure we
	 * make it big enough to start with.
	 *
	 * The shared memory allocator must be specified too.
	 */
	infoP->dsize = infoP->max_dsize = hash_select_dirsize(max_size);
	infoP->alloc = ShmemAlloc;
	hash_flags |= HASH_SHARED_MEM | HASH_ALLOC | HASH_DIRSIZE;

	/* look it up in the shmem index */
	location = ShmemInitStruct(name,
							   hash_get_shared_size(infoP, hash_flags),
							   &found);

	/*
	 * If fail, shmem index is corrupted.  Let caller give the error message
	 * since it has more information
	 */
	if (location == NULL)
		return NULL;

	/*
	 * if it already exists, attach to it rather than allocate and initialize
	 * new space
	 */
	if (found)
		hash_flags |= HASH_ATTACH;

	/* Pass location of hashtable header to hash_create */
	infoP->hctl = (HASHHDR *) location;

	return hash_create(name, init_size, infoP, hash_flags);
}

/*
 * ShmemInitStruct -- Create/attach to a structure in shared
 *		memory.
 *
 *	This is called during initialization to find or allocate
 *		a data structure in shared memory.	If no other process
 *		has created the structure, this routine allocates space
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
	ShmemIndexEnt *result;
	void	   *structPtr;

	LWLockAcquire(ShmemIndexLock, LW_EXCLUSIVE);

	if (!ShmemIndex)
	{
		PGShmemHeader *shmemseghdr = ShmemSegHdr;

		Assert(strcmp(name, "ShmemIndex") == 0);
		if (IsUnderPostmaster)
		{
			/* Must be initializing a (non-standalone) backend */
			Assert(shmemseghdr->indexoffset != 0);
			structPtr = (void *) MAKE_PTR(shmemseghdr->indexoffset);
			*foundPtr = TRUE;
		}
		else
		{
			/*
			 * If the shmem index doesn't exist, we are bootstrapping: we must
			 * be trying to init the shmem index itself.
			 *
			 * Notice that the ShmemIndexLock is released before the shmem
			 * index has been initialized.  This should be OK because no
			 * other process can be accessing shared memory yet.
			 */
			Assert(shmemseghdr->indexoffset == 0);
			structPtr = ShmemAlloc(size);
			shmemseghdr->indexoffset = MAKE_OFFSET(structPtr);
			*foundPtr = FALSE;
		}
		LWLockRelease(ShmemIndexLock);
		return structPtr;
	}

	/* look it up in the shmem index */
	result = (ShmemIndexEnt *)
		hash_search(ShmemIndex, name, HASH_ENTER_NULL, foundPtr);

	if (!result)
	{
		LWLockRelease(ShmemIndexLock);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory")));
	}

	if (*foundPtr)
	{
		/*
		 * Structure is in the shmem index so someone else has allocated it
		 * already.  The size better be the same as the size we are trying to
		 * initialize to or there is a name conflict (or worse).
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
			hash_search(ShmemIndex, name, HASH_REMOVE, NULL);
			LWLockRelease(ShmemIndexLock);

			ereport(WARNING,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("could not allocate shared memory segment \"%s\"",
							name)));
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


/*
 * Add two Size values, checking for overflow
 */
Size
add_size(Size s1, Size s2)
{
	Size		result;

	result = s1 + s2;
	/* We are assuming Size is an unsigned type here... */
	if (result < s1 || result < s2)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("requested shared memory size overflows size_t")));
	return result;
}

/*
 * Multiply two Size values, checking for overflow
 */
Size
mul_size(Size s1, Size s2)
{
	Size		result;

	if (s1 == 0 || s2 == 0)
		return 0;
	result = s1 * s2;
	/* We are assuming Size is an unsigned type here... */
	if (result / s2 != s1)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("requested shared memory size overflows size_t")));
	return result;
}
