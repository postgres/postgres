/*-------------------------------------------------------------------------
 *
 * freespace.c
 *	  POSTGRES free space map for quickly finding free space in relations
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/freespace/freespace.c,v 1.15 2003/02/23 06:17:13 tgl Exp $
 *
 *
 * NOTES:
 *
 * The only really interesting aspect of this code is the heuristics for
 * deciding how much information we can afford to keep about each relation,
 * given that we have a limited amount of workspace in shared memory.
 * These currently work as follows:
 *
 * The number of distinct relations tracked is limited by a configuration
 * variable (MaxFSMRelations).	When this would be exceeded, we discard the
 * least frequently used relation.	A previously-unknown relation is always
 * entered into the map with useCount 1 on first reference, even if this
 * causes an existing entry with higher useCount to be discarded.  This may
 * cause a little bit of thrashing among the bottom entries in the list,
 * but if we didn't do it then there'd be no way for a relation not in the
 * map to get in once the map is full.	Note we allow a relation to be in the
 * map even if no pages are currently stored for it: this allows us to track
 * its useCount & threshold, which may eventually go high enough to give it
 * priority for page storage.
 *
 * The total number of pages tracked is also set by a configuration variable
 * (MaxFSMPages).  We allocate these dynamically among the known relations,
 * giving preference to the more-frequently-referenced relations and those
 * with smaller tuples.  This is done by a thresholding mechanism: for each
 * relation we keep track of a current target threshold, and allow only pages
 * with free space >= threshold to be stored in the map.  The threshold starts
 * at BLCKSZ/2 (a somewhat arbitrary choice) for a newly entered relation.
 * On each GetFreeSpace reference, the threshold is moved towards the
 * request size using a slow exponential moving average; this means that
 * for heavily used relations, the threshold will approach the average
 * freespace request size (average tuple size).  Whenever we run out of
 * storage space in the map, we double the threshold of all existing relations
 * (but not to more than BLCKSZ, so as to prevent runaway thresholds).
 * Infrequently used relations will thus tend to have large thresholds.
 *
 * XXX this thresholding mechanism is experimental; need to see how well
 * it works in practice.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "storage/freespace.h"
#include "storage/itemid.h"
#include "storage/lwlock.h"
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
	HTAB	   *relHash;		/* hashtable of FSMRelation entries */
	FSMRelation *relList;		/* FSMRelations in useCount order */
	FSMRelation *relListTail;	/* tail of FSMRelation list */
	int			numRels;		/* number of FSMRelations now in use */
	FSMChunk   *freeChunks;		/* linked list of currently-free chunks */
	int			numFreeChunks;	/* number of free chunks */
};

/*
 * Per-relation struct --- this is an entry in the shared hash table.
 * The hash key is the RelFileNode value (hence, we look at the physical
 * relation ID, not the logical ID, which is appropriate).
 */
struct FSMRelation
{
	RelFileNode key;			/* hash key (must be first) */
	FSMRelation *nextRel;		/* next rel in useCount order */
	FSMRelation *priorRel;		/* prior rel in useCount order */
	int			useCount;		/* use count for prioritizing rels */
	Size		threshold;		/* minimum amount of free space to keep */
	int			nextPage;		/* index (from 0) to start next search at */
	int			numPages;		/* total number of pages we have info
								 * about */
	int			numChunks;		/* number of FSMChunks allocated to rel */
	FSMChunk   *relChunks;		/* linked list of page info chunks */
	FSMChunk   *lastChunk;		/* last chunk in linked list */
};

/*
 * Info about individual pages in a relation is stored in chunks to reduce
 * allocation overhead.  Note that we allow any chunk of a relation's list
 * to be partly full, not only the last chunk; this speeds deletion of
 * individual page entries.  When the total number of unused slots reaches
 * CHUNKPAGES, we compact out the unused slots so that we can return a chunk
 * to the freelist; but there's no point in doing the compaction before that.
 */

#define CHUNKPAGES	32			/* each chunk can store this many pages */

struct FSMChunk
{
	FSMChunk   *next;			/* linked-list link */
	int			numPages;		/* number of pages described here */
	BlockNumber pages[CHUNKPAGES];		/* page numbers within relation */
	ItemLength	bytes[CHUNKPAGES];		/* free space available on each
										 * page */
};


int			MaxFSMRelations;	/* these are set by guc.c */
int			MaxFSMPages;

static FSMHeader *FreeSpaceMap; /* points to FSMHeader in shared memory */


static FSMRelation *lookup_fsm_rel(RelFileNode *rel);
static FSMRelation *create_fsm_rel(RelFileNode *rel);
static void delete_fsm_rel(FSMRelation *fsmrel);
static void link_fsm_rel_after(FSMRelation *fsmrel, FSMRelation *oldrel);
static void unlink_fsm_rel(FSMRelation *fsmrel);
static void free_chunk_chain(FSMChunk *fchunk);
static BlockNumber find_free_space(FSMRelation *fsmrel, Size spaceNeeded);
static void fsm_record_free_space(FSMRelation *fsmrel, BlockNumber page,
					  Size spaceAvail);
static bool lookup_fsm_page_entry(FSMRelation *fsmrel, BlockNumber page,
					  FSMChunk **outChunk, int *outChunkRelIndex);
static bool insert_fsm_page_entry(FSMRelation *fsmrel,
					  BlockNumber page, Size spaceAvail,
					  FSMChunk *chunk, int chunkRelIndex);
static bool append_fsm_chunk(FSMRelation *fsmrel);
static bool push_fsm_page_entry(BlockNumber page, Size spaceAvail,
					FSMChunk *chunk, int chunkRelIndex);
static void delete_fsm_page_entry(FSMRelation *fsmrel, FSMChunk *chunk,
					  int chunkRelIndex);
static void compact_fsm_page_list(FSMRelation *fsmrel);
static void acquire_fsm_free_space(void);


/*
 * Exported routines
 */


/*
 * InitFreeSpaceMap -- Initialize the freespace module.
 *
 * This must be called once during shared memory initialization.
 * It builds the empty free space map table.  FreeSpaceLock must also be
 * initialized at some point, but is not touched here --- we assume there is
 * no need for locking, since only the calling process can be accessing shared
 * memory as yet.
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
	info.keysize = sizeof(RelFileNode);
	info.entrysize = sizeof(FSMRelation);
	info.hash = tag_hash;

	FreeSpaceMap->relHash = ShmemInitHash("Free Space Map Hash",
										  MaxFSMRelations / 10,
										  MaxFSMRelations,
										  &info,
										  (HASH_ELEM | HASH_FUNCTION));

	if (!FreeSpaceMap->relHash)
		elog(FATAL, "Insufficient shared memory for free space map");

	/* Allocate FSMChunks and fill up the free-chunks list */
	nchunks = (MaxFSMPages - 1) / CHUNKPAGES + 1;
	FreeSpaceMap->numFreeChunks = nchunks;

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

/*
 * Estimate amount of shmem space needed for FSM.
 */
int
FreeSpaceShmemSize(void)
{
	int			size;
	int			nchunks;

	/* table header */
	size = MAXALIGN(sizeof(FSMHeader));

	/* hash table, including the FSMRelation objects */
	size += hash_estimate_size(MaxFSMRelations, sizeof(FSMRelation));

	/* FSMChunk objects */
	nchunks = (MaxFSMPages - 1) / CHUNKPAGES + 1;

	size += MAXALIGN(nchunks * sizeof(FSMChunk));

	return size;
}

/*
 * GetPageWithFreeSpace - try to find a page in the given relation with
 *		at least the specified amount of free space.
 *
 * If successful, return the block number; if not, return InvalidBlockNumber.
 *
 * The caller must be prepared for the possibility that the returned page
 * will turn out to have too little space available by the time the caller
 * gets a lock on it.  In that case, the caller should report the actual
 * amount of free space available on that page (via RecordFreeSpace) and
 * then try again.	If InvalidBlockNumber is returned, extend the relation.
 */
BlockNumber
GetPageWithFreeSpace(RelFileNode *rel, Size spaceNeeded)
{
	FSMRelation *fsmrel;
	BlockNumber freepage;

	LWLockAcquire(FreeSpaceLock, LW_EXCLUSIVE);

	/*
	 * We always add a rel to the hashtable when it is inquired about.
	 */
	fsmrel = create_fsm_rel(rel);

	/*
	 * Adjust the threshold towards the space request.	This essentially
	 * implements an exponential moving average with an equivalent period
	 * of about 63 requests.  Ignore silly requests, however, to ensure
	 * that the average stays in bounds.
	 *
	 * In theory, if the threshold increases here we should immediately
	 * delete any pages that fall below the new threshold.	In practice it
	 * seems OK to wait until we have a need to compact space.
	 */
	if (spaceNeeded > 0 && spaceNeeded < BLCKSZ)
	{
		int			cur_avg = (int) fsmrel->threshold;

		cur_avg += ((int) spaceNeeded - cur_avg) / 32;
		fsmrel->threshold = (Size) cur_avg;
	}
	freepage = find_free_space(fsmrel, spaceNeeded);
	LWLockRelease(FreeSpaceLock);
	return freepage;
}

/*
 * RecordFreeSpace - record the amount of free space available on a page.
 *
 * The FSM is at liberty to forget about the page instead, if the amount of
 * free space is too small to be interesting.  The only guarantee is that
 * a subsequent request to get a page with more than spaceAvail space free
 * will not return this page.
 */
void
RecordFreeSpace(RelFileNode *rel, BlockNumber page, Size spaceAvail)
{
	FSMRelation *fsmrel;

	/* Sanity check: ensure spaceAvail will fit into ItemLength */
	AssertArg(spaceAvail < BLCKSZ);

	LWLockAcquire(FreeSpaceLock, LW_EXCLUSIVE);

	/*
	 * We choose not to add rels to the hashtable unless they've been
	 * inquired about with GetPageWithFreeSpace.  Also, a Record operation
	 * does not increase the useCount or change threshold, only Get does.
	 */
	fsmrel = lookup_fsm_rel(rel);
	if (fsmrel)
		fsm_record_free_space(fsmrel, page, spaceAvail);
	LWLockRelease(FreeSpaceLock);
}

/*
 * RecordAndGetPageWithFreeSpace - combo form to save one lock and
 *		hash table lookup cycle.
 */
BlockNumber
RecordAndGetPageWithFreeSpace(RelFileNode *rel,
							  BlockNumber oldPage,
							  Size oldSpaceAvail,
							  Size spaceNeeded)
{
	FSMRelation *fsmrel;
	BlockNumber freepage;

	/* Sanity check: ensure spaceAvail will fit into ItemLength */
	AssertArg(oldSpaceAvail < BLCKSZ);

	LWLockAcquire(FreeSpaceLock, LW_EXCLUSIVE);

	/*
	 * We always add a rel to the hashtable when it is inquired about.
	 */
	fsmrel = create_fsm_rel(rel);

	/*
	 * Adjust the threshold towards the space request, same as in
	 * GetPageWithFreeSpace.
	 *
	 * Note that we do this before storing data for oldPage, which means this
	 * isn't exactly equivalent to Record followed by Get; but it seems
	 * appropriate to adjust the threshold first.
	 */
	if (spaceNeeded > 0 && spaceNeeded < BLCKSZ)
	{
		int			cur_avg = (int) fsmrel->threshold;

		cur_avg += ((int) spaceNeeded - cur_avg) / 32;
		fsmrel->threshold = (Size) cur_avg;
	}
	/* Do the Record */
	fsm_record_free_space(fsmrel, oldPage, oldSpaceAvail);
	/* Do the Get */
	freepage = find_free_space(fsmrel, spaceNeeded);
	LWLockRelease(FreeSpaceLock);
	return freepage;
}

/*
 * MultiRecordFreeSpace - record available-space info about multiple pages
 *		of a relation in one call.
 *
 * First, the FSM must discard any entries it has for pages >= minPage.
 * This allows obsolete info to be discarded (for example, it is used when
 * truncating a relation).  Any entries before minPage should be kept.
 *
 * Second, if nPages > 0, record the page numbers and free space amounts in
 * the given pageSpaces[] array.  As with RecordFreeSpace, the FSM is at
 * liberty to discard some of this information.  The pageSpaces[] array must
 * be sorted in order by blkno, and may not contain entries before minPage.
 */
void
MultiRecordFreeSpace(RelFileNode *rel,
					 BlockNumber minPage,
					 int nPages,
					 PageFreeSpaceInfo *pageSpaces)
{
	FSMRelation *fsmrel;
	int			i;

	LWLockAcquire(FreeSpaceLock, LW_EXCLUSIVE);
	fsmrel = lookup_fsm_rel(rel);
	if (fsmrel)
	{
		/*
		 * Remove entries >= minPage
		 */
		FSMChunk   *chunk;
		int			chunkRelIndex;

		/* Use lookup to locate first entry >= minPage */
		lookup_fsm_page_entry(fsmrel, minPage, &chunk, &chunkRelIndex);
		/* Set free space to 0 for each page >= minPage */
		while (chunk)
		{
			int			numPages = chunk->numPages;

			for (i = chunkRelIndex; i < numPages; i++)
				chunk->bytes[i] = 0;
			chunk = chunk->next;
			chunkRelIndex = 0;
		}
		/* Now compact out the zeroed entries, along with any other junk */
		compact_fsm_page_list(fsmrel);

		/*
		 * Add new entries, if appropriate.
		 *
		 * This can be much cheaper than a full fsm_record_free_space()
		 * call because we know we are appending to the end of the relation.
		 */
		for (i = 0; i < nPages; i++)
		{
			BlockNumber page = pageSpaces[i].blkno;
			Size		avail = pageSpaces[i].avail;
			FSMChunk   *chunk;

			/* Check caller provides sorted data */
			if (i > 0 ? (page <= pageSpaces[i-1].blkno) : (page < minPage))
				elog(ERROR, "MultiRecordFreeSpace: data not in page order");

			/* Ignore pages too small to fit */
			if (avail < fsmrel->threshold)
				continue;

			/* Get another chunk if needed */
			/* We may need to loop if acquire_fsm_free_space() fails */
			while ((chunk = fsmrel->lastChunk) == NULL ||
				   chunk->numPages >= CHUNKPAGES)
			{
				if (!append_fsm_chunk(fsmrel))
					acquire_fsm_free_space();
			}

			/* Recheck in case threshold was raised by acquire */
			if (avail < fsmrel->threshold)
				continue;

			/* Okay to store */
			chunk->pages[chunk->numPages] = page;
			chunk->bytes[chunk->numPages] = (ItemLength) avail;
			chunk->numPages++;
			fsmrel->numPages++;
		}
	}
	LWLockRelease(FreeSpaceLock);
}

/*
 * FreeSpaceMapForgetRel - forget all about a relation.
 *
 * This is called when a relation is deleted.  Although we could just let
 * the rel age out of the map, it's better to reclaim and reuse the space
 * sooner.
 */
void
FreeSpaceMapForgetRel(RelFileNode *rel)
{
	FSMRelation *fsmrel;

	LWLockAcquire(FreeSpaceLock, LW_EXCLUSIVE);
	fsmrel = lookup_fsm_rel(rel);
	if (fsmrel)
		delete_fsm_rel(fsmrel);
	LWLockRelease(FreeSpaceLock);
}

/*
 * FreeSpaceMapForgetDatabase - forget all relations of a database.
 *
 * This is called during DROP DATABASE.  As above, might as well reclaim
 * map space sooner instead of later.
 *
 * XXX when we implement tablespaces, target Oid will need to be tablespace
 * ID not database ID.
 */
void
FreeSpaceMapForgetDatabase(Oid dbid)
{
	FSMRelation *fsmrel,
			   *nextrel;

	LWLockAcquire(FreeSpaceLock, LW_EXCLUSIVE);
	for (fsmrel = FreeSpaceMap->relList; fsmrel; fsmrel = nextrel)
	{
		nextrel = fsmrel->nextRel;		/* in case we delete it */
		if (fsmrel->key.tblNode == dbid)
			delete_fsm_rel(fsmrel);
	}
	LWLockRelease(FreeSpaceLock);
}


/*
 * Internal routines.  These all assume the caller holds the FreeSpaceLock.
 */

/*
 * Lookup a relation in the hash table.  If not present, return NULL.
 * The relation's useCount is not changed.
 */
static FSMRelation *
lookup_fsm_rel(RelFileNode *rel)
{
	FSMRelation *fsmrel;

	fsmrel = (FSMRelation *) hash_search(FreeSpaceMap->relHash,
										 (void *) rel,
										 HASH_FIND,
										 NULL);
	if (!fsmrel)
		return NULL;

	return fsmrel;
}

/*
 * Lookup a relation in the hash table, creating an entry if not present.
 *
 * On successful lookup, the relation's useCount is incremented, possibly
 * causing it to move up in the priority list.
 */
static FSMRelation *
create_fsm_rel(RelFileNode *rel)
{
	FSMRelation *fsmrel,
			   *oldrel;
	bool		found;

	fsmrel = (FSMRelation *) hash_search(FreeSpaceMap->relHash,
										 (void *) rel,
										 HASH_ENTER,
										 &found);
	if (!fsmrel)
		elog(ERROR, "FreeSpaceMap hashtable out of memory");

	if (!found)
	{
		/* New hashtable entry, initialize it (hash_search set the key) */
		fsmrel->useCount = 1;
		fsmrel->threshold = BLCKSZ / 2; /* starting point for new entry */
		fsmrel->nextPage = 0;
		fsmrel->numPages = 0;
		fsmrel->numChunks = 0;
		fsmrel->relChunks = NULL;
		fsmrel->lastChunk = NULL;
		/* Discard lowest-priority existing rel, if we are over limit */
		if (FreeSpaceMap->numRels >= MaxFSMRelations)
			delete_fsm_rel(FreeSpaceMap->relListTail);

		/*
		 * Add new entry in front of any others with useCount 1 (since it
		 * is more recently used than them).
		 */
		oldrel = FreeSpaceMap->relListTail;
		while (oldrel && oldrel->useCount <= 1)
			oldrel = oldrel->priorRel;
		link_fsm_rel_after(fsmrel, oldrel);
		FreeSpaceMap->numRels++;
	}
	else
	{
		int			myCount;

		/* Existing entry, advance its useCount */
		if (++(fsmrel->useCount) >= INT_MAX / 2)
		{
			/* When useCounts threaten to overflow, reduce 'em all 2X */
			for (oldrel = FreeSpaceMap->relList;
				 oldrel != NULL;
				 oldrel = oldrel->nextRel)
				oldrel->useCount >>= 1;
		}
		/* If warranted, move it up the priority list */
		oldrel = fsmrel->priorRel;
		myCount = fsmrel->useCount;
		if (oldrel && oldrel->useCount <= myCount)
		{
			unlink_fsm_rel(fsmrel);
			while (oldrel && oldrel->useCount <= myCount)
				oldrel = oldrel->priorRel;
			link_fsm_rel_after(fsmrel, oldrel);
		}
	}

	return fsmrel;
}

/*
 * Remove an existing FSMRelation entry.
 */
static void
delete_fsm_rel(FSMRelation *fsmrel)
{
	FSMRelation *result;

	free_chunk_chain(fsmrel->relChunks);
	unlink_fsm_rel(fsmrel);
	FreeSpaceMap->numRels--;
	result = (FSMRelation *) hash_search(FreeSpaceMap->relHash,
										 (void *) &(fsmrel->key),
										 HASH_REMOVE,
										 NULL);
	if (!result)
		elog(ERROR, "FreeSpaceMap hashtable corrupted");
}

/*
 * Link a FSMRelation into the priority list just after the given existing
 * entry (or at the head of the list, if oldrel is NULL).
 */
static void
link_fsm_rel_after(FSMRelation *fsmrel, FSMRelation *oldrel)
{
	if (oldrel == NULL)
	{
		/* insert at head */
		fsmrel->priorRel = NULL;
		fsmrel->nextRel = FreeSpaceMap->relList;
		FreeSpaceMap->relList = fsmrel;
		if (fsmrel->nextRel != NULL)
			fsmrel->nextRel->priorRel = fsmrel;
		else
			FreeSpaceMap->relListTail = fsmrel;
	}
	else
	{
		/* insert after oldrel */
		fsmrel->priorRel = oldrel;
		fsmrel->nextRel = oldrel->nextRel;
		oldrel->nextRel = fsmrel;
		if (fsmrel->nextRel != NULL)
			fsmrel->nextRel->priorRel = fsmrel;
		else
			FreeSpaceMap->relListTail = fsmrel;
	}
}

/*
 * Delink a FSMRelation from the priority list.
 */
static void
unlink_fsm_rel(FSMRelation *fsmrel)
{
	if (fsmrel->priorRel != NULL)
		fsmrel->priorRel->nextRel = fsmrel->nextRel;
	else
		FreeSpaceMap->relList = fsmrel->nextRel;
	if (fsmrel->nextRel != NULL)
		fsmrel->nextRel->priorRel = fsmrel->priorRel;
	else
		FreeSpaceMap->relListTail = fsmrel->priorRel;
}

/*
 * Return all the FSMChunks in the chain starting at fchunk to the freelist.
 * (Caller must handle unlinking them from wherever they were.)
 */
static void
free_chunk_chain(FSMChunk *fchunk)
{
	int			nchunks;
	FSMChunk   *lchunk;

	if (fchunk == NULL)
		return;
	nchunks = 1;
	lchunk = fchunk;
	while (lchunk->next != NULL)
	{
		nchunks++;
		lchunk = lchunk->next;
	}
	lchunk->next = FreeSpaceMap->freeChunks;
	FreeSpaceMap->freeChunks = fchunk;
	FreeSpaceMap->numFreeChunks += nchunks;
}

/*
 * Look to see if a page with at least the specified amount of space is
 * available in the given FSMRelation.	If so, return its page number,
 * and advance the nextPage counter so that the next inquiry will return
 * a different page if possible; also update the entry to show that the
 * requested space is not available anymore.  Return InvalidBlockNumber
 * if no success.
 */
static BlockNumber
find_free_space(FSMRelation *fsmrel, Size spaceNeeded)
{
	int			pagesToCheck,	/* outer loop counter */
				pageIndex,		/* current page index relative to relation */
				chunkRelIndex;	/* current page index relative to curChunk */
	FSMChunk   *curChunk;

	pageIndex = fsmrel->nextPage;
	/* Last operation may have left nextPage pointing past end */
	if (pageIndex >= fsmrel->numPages)
		pageIndex = 0;
	curChunk = fsmrel->relChunks;
	chunkRelIndex = pageIndex;

	for (pagesToCheck = fsmrel->numPages; pagesToCheck > 0; pagesToCheck--)
	{
		/*
		 * Make sure we are in the right chunk.  First time through, we
		 * may have to advance through multiple chunks; subsequent loops
		 * should do this at most once.
		 */
		while (chunkRelIndex >= curChunk->numPages)
		{
			chunkRelIndex -= curChunk->numPages;
			curChunk = curChunk->next;
		}
		/* Check the next page */
		if ((Size) curChunk->bytes[chunkRelIndex] >= spaceNeeded)
		{
			/*
			 * Found what we want --- adjust the entry.  In theory we could
			 * delete the entry immediately if it drops below threshold,
			 * but it seems better to wait till we next need space.
			 */
			curChunk->bytes[chunkRelIndex] -= (ItemLength) spaceNeeded;
			fsmrel->nextPage = pageIndex + 1;
			return curChunk->pages[chunkRelIndex];
		}
		/* Advance pageIndex and chunkRelIndex, wrapping around if needed */
		if (++pageIndex >= fsmrel->numPages)
		{
			pageIndex = 0;
			curChunk = fsmrel->relChunks;
			chunkRelIndex = 0;
		}
		else
			chunkRelIndex++;
	}

	return InvalidBlockNumber;	/* nothing found */
}

/*
 * fsm_record_free_space - guts of RecordFreeSpace, which is also used by
 * RecordAndGetPageWithFreeSpace.
 */
static void
fsm_record_free_space(FSMRelation *fsmrel, BlockNumber page, Size spaceAvail)
{
	FSMChunk   *chunk;
	int			chunkRelIndex;

	if (lookup_fsm_page_entry(fsmrel, page, &chunk, &chunkRelIndex))
	{
		/* Found an existing entry for page; update or delete it */
		if (spaceAvail >= fsmrel->threshold)
			chunk->bytes[chunkRelIndex] = (ItemLength) spaceAvail;
		else
			delete_fsm_page_entry(fsmrel, chunk, chunkRelIndex);
	}
	else
	{
		/*
		 * No existing entry; add one if spaceAvail exceeds threshold.
		 *
		 * CORNER CASE: if we have to do acquire_fsm_free_space then our own
		 * threshold will increase, possibly meaning that we shouldn't
		 * store the page after all.  Loop to redo the test if that
		 * happens.  The loop also covers the possibility that
		 * acquire_fsm_free_space must be executed more than once to free
		 * any space (ie, thresholds must be more than doubled).
		 */
		while (spaceAvail >= fsmrel->threshold)
		{
			if (insert_fsm_page_entry(fsmrel, page, spaceAvail,
									  chunk, chunkRelIndex))
				break;
			/* No space, acquire some and recheck threshold */
			acquire_fsm_free_space();
			if (spaceAvail < fsmrel->threshold)
				break;

			/*
			 * Need to redo the lookup since our own page list may well
			 * have lost entries, so position is not correct anymore.
			 */
			if (lookup_fsm_page_entry(fsmrel, page,
									  &chunk, &chunkRelIndex))
				elog(ERROR, "fsm_record_free_space: unexpected match");
		}
	}
}

/*
 * Look for an entry for a specific page (block number) in a FSMRelation.
 * Returns TRUE if a matching entry exists, else FALSE.
 *
 * The output arguments *outChunk, *outChunkRelIndex are set to indicate where
 * the entry exists (if TRUE result) or could be inserted (if FALSE result).
 * *chunk is set to NULL if there is no place to insert, ie, the entry would
 * need to be added to a new chunk.
 */
static bool
lookup_fsm_page_entry(FSMRelation *fsmrel, BlockNumber page,
					  FSMChunk **outChunk, int *outChunkRelIndex)
{
	FSMChunk   *chunk;
	int			chunkRelIndex;

	for (chunk = fsmrel->relChunks; chunk; chunk = chunk->next)
	{
		int			numPages = chunk->numPages;

		/* Can skip the chunk quickly if page must be after last in chunk */
		if (numPages > 0 && page <= chunk->pages[numPages - 1])
		{
			for (chunkRelIndex = 0; chunkRelIndex < numPages; chunkRelIndex++)
			{
				if (page <= chunk->pages[chunkRelIndex])
				{
					*outChunk = chunk;
					*outChunkRelIndex = chunkRelIndex;
					return (page == chunk->pages[chunkRelIndex]);
				}
			}
			/* Should not get here, given above test */
			Assert(false);
		}

		/*
		 * If we are about to fall off the end, and there's space
		 * available in the end chunk, return a pointer to it.
		 */
		if (chunk->next == NULL && numPages < CHUNKPAGES)
		{
			*outChunk = chunk;
			*outChunkRelIndex = numPages;
			return false;
		}
	}

	/*
	 * Adding the page would require a new chunk (or, perhaps, compaction
	 * of available free space --- not my problem here).
	 */
	*outChunk = NULL;
	*outChunkRelIndex = 0;
	return false;
}

/*
 * Insert a new page entry into a FSMRelation's list at given position
 * (chunk == NULL implies at end).
 *
 * If there is no space available to insert the entry, return FALSE.
 */
static bool
insert_fsm_page_entry(FSMRelation *fsmrel, BlockNumber page, Size spaceAvail,
					  FSMChunk *chunk, int chunkRelIndex)
{
	/* Outer loop handles retry after compacting rel's page list */
	for (;;)
	{
		if (fsmrel->numPages >= fsmrel->numChunks * CHUNKPAGES)
		{
			/* No free space within chunk list, so need another chunk */
			if (!append_fsm_chunk(fsmrel))
				return false;	/* can't do it */
			if (chunk == NULL)
			{
				/* Original search found that new page belongs at end */
				chunk = fsmrel->lastChunk;
				chunkRelIndex = 0;
			}
		}

		/*
		 * Try to insert it the easy way, ie, just move down subsequent
		 * data
		 */
		if (chunk &&
			push_fsm_page_entry(page, spaceAvail, chunk, chunkRelIndex))
		{
			fsmrel->numPages++;
			fsmrel->nextPage++; /* don't return same page twice running */
			return true;
		}

		/*
		 * There is space available, but evidently it's before the place
		 * where the page entry needs to go.  Compact the list and try
		 * again. This will require us to redo the search for the
		 * appropriate place. Furthermore, compact_fsm_page_list deletes
		 * empty end chunks, so we may need to repeat the action of
		 * grabbing a new end chunk.
		 */
		compact_fsm_page_list(fsmrel);
		if (lookup_fsm_page_entry(fsmrel, page, &chunk, &chunkRelIndex))
			elog(ERROR, "insert_fsm_page_entry: entry already exists!");
	}
}

/*
 * Add one chunk to a FSMRelation's chunk list, if possible.
 *
 * Returns TRUE if successful, FALSE if no space available.  Note that on
 * success, the new chunk is easily accessible via fsmrel->lastChunk.
 */
static bool
append_fsm_chunk(FSMRelation *fsmrel)
{
	FSMChunk   *newChunk;

	/* Remove a chunk from the freelist */
	if ((newChunk = FreeSpaceMap->freeChunks) == NULL)
		return false;			/* can't do it */
	FreeSpaceMap->freeChunks = newChunk->next;
	FreeSpaceMap->numFreeChunks--;

	/* Initialize chunk to empty */
	newChunk->next = NULL;
	newChunk->numPages = 0;

	/* Link it into FSMRelation */
	if (fsmrel->relChunks == NULL)
		fsmrel->relChunks = newChunk;
	else
		fsmrel->lastChunk->next = newChunk;
	fsmrel->lastChunk = newChunk;
	fsmrel->numChunks++;

	return true;
}

/*
 * Auxiliary routine for insert_fsm_page_entry: try to push entries to the
 * right to insert at chunk/chunkRelIndex.	Return TRUE if successful.
 * Note that the FSMRelation's own fields are not updated.
 */
static bool
push_fsm_page_entry(BlockNumber page, Size spaceAvail,
					FSMChunk *chunk, int chunkRelIndex)
{
	int			i;

	if (chunk->numPages >= CHUNKPAGES)
	{
		if (chunk->next == NULL)
			return false;		/* no space */
		/* try to push chunk's last item to next chunk */
		if (!push_fsm_page_entry(chunk->pages[CHUNKPAGES - 1],
								 chunk->bytes[CHUNKPAGES - 1],
								 chunk->next, 0))
			return false;
		/* successfully pushed it */
		chunk->numPages--;
	}
	for (i = chunk->numPages; i > chunkRelIndex; i--)
	{
		chunk->pages[i] = chunk->pages[i - 1];
		chunk->bytes[i] = chunk->bytes[i - 1];
	}
	chunk->numPages++;
	chunk->pages[chunkRelIndex] = page;
	chunk->bytes[chunkRelIndex] = (ItemLength) spaceAvail;
	return true;
}

/*
 * Delete one page entry from a FSMRelation's list.  Compact free space
 * in the list, but only if a chunk can be returned to the freelist.
 */
static void
delete_fsm_page_entry(FSMRelation *fsmrel, FSMChunk *chunk, int chunkRelIndex)
{
	int			i,
				lim;

	Assert(chunk && chunkRelIndex >= 0 && chunkRelIndex < chunk->numPages);
	/* Compact out space in this chunk */
	lim = --chunk->numPages;
	for (i = chunkRelIndex; i < lim; i++)
	{
		chunk->pages[i] = chunk->pages[i + 1];
		chunk->bytes[i] = chunk->bytes[i + 1];
	}
	/* Compact the whole list if a chunk can be freed */
	fsmrel->numPages--;
	if (fsmrel->numPages <= (fsmrel->numChunks - 1) * CHUNKPAGES)
		compact_fsm_page_list(fsmrel);
}

/*
 * Remove any pages with free space less than the current threshold for the
 * FSMRelation, compact out free slots in non-last chunks, and return any
 * completely freed chunks to the freelist.
 */
static void
compact_fsm_page_list(FSMRelation *fsmrel)
{
	Size		threshold = fsmrel->threshold;
	FSMChunk   *srcChunk,
			   *dstChunk;
	int			srcIndex,
				dstIndex,
				dstPages,
				dstChunkCnt;

	srcChunk = dstChunk = fsmrel->relChunks;
	srcIndex = dstIndex = 0;
	dstPages = 0;				/* total pages kept */
	dstChunkCnt = 1;			/* includes current dstChunk */

	while (srcChunk != NULL)
	{
		int			srcPages = srcChunk->numPages;

		while (srcIndex < srcPages)
		{
			if ((Size) srcChunk->bytes[srcIndex] >= threshold)
			{
				if (dstIndex >= CHUNKPAGES)
				{
					/*
					 * At this point srcChunk must be pointing to a later
					 * chunk, so it's OK to overwrite dstChunk->numPages.
					 */
					dstChunk->numPages = dstIndex;
					dstChunk = dstChunk->next;
					dstChunkCnt++;
					dstIndex = 0;
				}
				dstChunk->pages[dstIndex] = srcChunk->pages[srcIndex];
				dstChunk->bytes[dstIndex] = srcChunk->bytes[srcIndex];
				dstIndex++;
				dstPages++;
			}
			srcIndex++;
		}
		srcChunk = srcChunk->next;
		srcIndex = 0;
	}

	if (dstPages == 0)
	{
		/* No chunks to be kept at all */
		fsmrel->nextPage = 0;
		fsmrel->numPages = 0;
		fsmrel->numChunks = 0;
		fsmrel->relChunks = NULL;
		fsmrel->lastChunk = NULL;
		free_chunk_chain(dstChunk);
	}
	else
	{
		/* we deliberately don't change nextPage here */
		fsmrel->numPages = dstPages;
		fsmrel->numChunks = dstChunkCnt;
		dstChunk->numPages = dstIndex;
		free_chunk_chain(dstChunk->next);
		dstChunk->next = NULL;
		fsmrel->lastChunk = dstChunk;
	}
}

/*
 * Acquire some free space by raising the thresholds of all FSMRelations.
 *
 * Note there is no guarantee as to how much space will be freed by a single
 * invocation; caller may repeat if necessary.
 */
static void
acquire_fsm_free_space(void)
{
	FSMRelation *fsmrel;

	for (fsmrel = FreeSpaceMap->relList; fsmrel; fsmrel = fsmrel->nextRel)
	{
		fsmrel->threshold *= 2;
		/* Limit thresholds to BLCKSZ so they can't get ridiculously large */
		if (fsmrel->threshold > BLCKSZ)
			fsmrel->threshold = BLCKSZ;
		/* Release any pages that don't meet the new threshold */
		compact_fsm_page_list(fsmrel);
	}
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
	FSMRelation *fsmrel;
	FSMRelation *prevrel = NULL;
	int			relNum = 0;
	FSMChunk   *chunk;
	int			chunkRelIndex;
	int			nChunks;
	int			nPages;

	for (fsmrel = FreeSpaceMap->relList; fsmrel; fsmrel = fsmrel->nextRel)
	{
		relNum++;
		fprintf(stderr, "Map %d: rel %u/%u useCount %d threshold %u nextPage %d\nMap= ",
				relNum, fsmrel->key.tblNode, fsmrel->key.relNode,
				fsmrel->useCount, fsmrel->threshold, fsmrel->nextPage);
		nChunks = nPages = 0;
		for (chunk = fsmrel->relChunks; chunk; chunk = chunk->next)
		{
			int			numPages = chunk->numPages;

			nChunks++;
			for (chunkRelIndex = 0; chunkRelIndex < numPages; chunkRelIndex++)
			{
				nPages++;
				fprintf(stderr, " %u:%u",
						chunk->pages[chunkRelIndex],
						chunk->bytes[chunkRelIndex]);
			}
		}
		fprintf(stderr, "\n");
		/* Cross-check local counters and list links */
		if (nPages != fsmrel->numPages)
			fprintf(stderr, "DumpFreeSpace: %d pages in rel, but numPages = %d\n",
					nPages, fsmrel->numPages);
		if (nChunks != fsmrel->numChunks)
			fprintf(stderr, "DumpFreeSpace: %d chunks in rel, but numChunks = %d\n",
					nChunks, fsmrel->numChunks);
		if (prevrel != fsmrel->priorRel)
			fprintf(stderr, "DumpFreeSpace: broken list links\n");
		prevrel = fsmrel;
	}
	if (prevrel != FreeSpaceMap->relListTail)
		fprintf(stderr, "DumpFreeSpace: broken list links\n");
	/* Cross-check global counters */
	if (relNum != FreeSpaceMap->numRels)
		fprintf(stderr, "DumpFreeSpace: %d rels in list, but numRels = %d\n",
				relNum, FreeSpaceMap->numRels);
	nChunks = 0;
	for (chunk = FreeSpaceMap->freeChunks; chunk; chunk = chunk->next)
		nChunks++;
	if (nChunks != FreeSpaceMap->numFreeChunks)
		fprintf(stderr, "DumpFreeSpace: %d chunks in list, but numFreeChunks = %d\n",
				nChunks, FreeSpaceMap->numFreeChunks);
}

#endif   /* FREESPACE_DEBUG */
