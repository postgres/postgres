/*-------------------------------------------------------------------------
 *
 * freespace.c
 *	  POSTGRES free space map for quickly finding free space in relations
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/freespace/freespace.c,v 1.2 2001/06/29 21:08:24 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/freespace.h"
#include "storage/itemid.h"
#include "storage/shmem.h"


/*
 * Shared free-space-map objects
 *
 * Note: we handle pointers to these items as pointers, not as SHMEM_OFFSETs.
 * This assumes that all processes accessing the map will have the shared
 * memory segment mapped at the same place in their address space.
 */
typedef struct FSMHeader FSMHeader;
typedef struct FSMRelation FSMRelation;
typedef struct FSMChunk FSMChunk;

/* Header for whole map */
struct FSMHeader
{
	HTAB	   *relationHash;	/* hashtable of FSMRelation entries */
	FSMRelation *relationList;	/* FSMRelations in order by recency of use */
	int			numRelations;	/* number of FSMRelations now in use */
	FSMChunk   *freeChunks;		/* linked list of currently-free chunks */
};

/*
 * Per-relation struct --- this is an entry in the shared hash table.
 * The hash key is the RelFileNode value (hence, we look at the physical
 * relation ID, not the logical ID, which is appropriate).
 */
struct FSMRelation
{
	RelFileNode	key;			/* hash key (must be first) */
	FSMRelation *nextRel;		/* next rel in order by recency of use */
	FSMRelation *priorRel;		/* prior rel in order by recency of use */
	FSMChunk   *relChunks;		/* linked list of page info chunks */
};

#define SHMEM_FSMHASH_KEYSIZE  sizeof(RelFileNode)
#define SHMEM_FSMHASH_DATASIZE (sizeof(FSMRelation) - SHMEM_FSMHASH_KEYSIZE)

#define CHUNKPAGES  32			/* each chunk can store this many pages */

struct FSMChunk
{
	FSMChunk   *next;			/* linked-list link */
	int			numPages;		/* number of pages described here */
	BlockNumber	pages[CHUNKPAGES]; /* page numbers within relation */
	ItemLength	bytes[CHUNKPAGES]; /* free space available on each page */
};


SPINLOCK	FreeSpaceLock;		/* in Shmem or created in
								 * CreateSpinlocks() */

int		MaxFSMRelations;		/* these are set by guc.c */
int		MaxFSMPages;

static FSMHeader *FreeSpaceMap;	/* points to FSMHeader in shared memory */


/*
 * InitFreeSpaceMap -- Initialize the freespace module.
 *
 * This must be called once during shared memory initialization.
 * It builds the empty free space map table.  FreeSpaceLock must also be
 * initialized at some point, but is not touched here --- we assume there is
 * no need for locking, since only the calling process can be accessing shared
 * memory as yet.  FreeSpaceShmemSize() was called previously while computing
 * the space needed for shared memory.
 */
void
InitFreeSpaceMap(void)
{
	HASHCTL		info;
	FSMChunk   *chunks,
			   *prevchunk;
	int			nchunks;

	/* Create table header */
	FreeSpaceMap = (FSMHeader *) ShmemAlloc(sizeof(FSMHeader));
	if (FreeSpaceMap == NULL)
		elog(FATAL, "Insufficient shared memory for free space map");
	MemSet(FreeSpaceMap, 0, sizeof(FSMHeader));

	/* Create hashtable for FSMRelations */
	info.keysize = SHMEM_FSMHASH_KEYSIZE;
	info.datasize = SHMEM_FSMHASH_DATASIZE;
	info.hash = tag_hash;

	FreeSpaceMap->relationHash = ShmemInitHash("Free Space Map Hash",
											   MaxFSMRelations / 10,
											   MaxFSMRelations,
											   &info,
											   (HASH_ELEM | HASH_FUNCTION));

	if (!FreeSpaceMap->relationHash)
		elog(FATAL, "Insufficient shared memory for free space map");

	/* Allocate FSMChunks and fill up the free-chunks list */
	nchunks = (MaxFSMPages - 1) / CHUNKPAGES + 1;

	chunks = (FSMChunk *) ShmemAlloc(nchunks * sizeof(FSMChunk));
	if (chunks == NULL)
		elog(FATAL, "Insufficient shared memory for free space map");

	prevchunk = NULL;
	while (nchunks-- > 0)
	{
		chunks->next = prevchunk;
		prevchunk = chunks;
		chunks++;
	}
	FreeSpaceMap->freeChunks = prevchunk;
}


int
FreeSpaceShmemSize(void)
{
	int			size;
	int			nchunks;

	/*
	 * There is no point in allowing less than one "chunk" per relation,
	 * so force MaxFSMPages to be at least CHUNKPAGES * MaxFSMRelations.
	 */
	Assert(MaxFSMRelations > 0);
	if (MaxFSMPages < CHUNKPAGES * MaxFSMRelations)
		MaxFSMPages = CHUNKPAGES * MaxFSMRelations;

	/* table header */
	size = MAXALIGN(sizeof(FSMHeader));

	/* hash table, including the FSMRelation objects */
	size += hash_estimate_size(MaxFSMRelations,
							   SHMEM_FSMHASH_KEYSIZE,
							   SHMEM_FSMHASH_DATASIZE);

	/* FSMChunk objects */
	nchunks = (MaxFSMPages - 1) / CHUNKPAGES + 1;

	size += MAXALIGN(nchunks * sizeof(FSMChunk));

	return size;
}

BlockNumber
GetPageWithFreeSpace(RelFileNode *rel, Size spaceNeeded)
{
	return InvalidBlockNumber;	/* stub */
}

void
RecordFreeSpace(RelFileNode *rel, BlockNumber page, Size spaceAvail)
{
	/* stub */
}

BlockNumber
RecordAndGetPageWithFreeSpace(RelFileNode *rel,
							  BlockNumber oldPage,
							  Size oldSpaceAvail,
							  Size spaceNeeded)
{
	return InvalidBlockNumber;	/* stub */
}

void
MultiRecordFreeSpace(RelFileNode *rel,
					 BlockNumber minPage,
					 BlockNumber maxPage,
					 int nPages,
					 BlockNumber *pages,
					 Size *spaceAvail)
{
	/* stub */
}

void
FreeSpaceMapForgetRel(RelFileNode *rel)
{
	/* stub */
}


#ifdef FREESPACE_DEBUG
/*
 * Dump contents of freespace map for debugging.
 *
 * We assume caller holds the FreeSpaceLock, or is otherwise unconcerned
 * about other processes.
 */
void
DumpFreeSpace(void)
{
	/* stub */
}

#endif	 /* FREESPACE_DEBUG */
