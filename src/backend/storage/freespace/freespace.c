/*-------------------------------------------------------------------------
 *
 * freespace.c
 *	  POSTGRES free space map for quickly finding free space in relations
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/freespace/freespace.c,v 1.24.2.1 2003/11/26 20:50:26 tgl Exp $
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
 * least recently used relation.  A doubly-linked list with move-to-front
 * behavior keeps track of which relation is least recently used.
 *
 * For each known relation, we track the average request size given to
 * GetPageWithFreeSpace() as well as the most recent number of pages given
 * to RecordRelationFreeSpace().  The average request size is not directly
 * used in this module, but we expect VACUUM to use it to filter out
 * uninteresting amounts of space before calling RecordRelationFreeSpace().
 * The sum of the RRFS page counts is thus the total number of "interesting"
 * pages that we would like to track; this is called DesiredFSMPages.
 *
 * The number of pages actually tracked is limited by a configuration variable
 * (MaxFSMPages).  When this is less than DesiredFSMPages, each relation
 * gets to keep a fraction MaxFSMPages/DesiredFSMPages of its free pages.
 * We discard pages with less free space to reach this target.
 *
 * Actually, our space allocation is done in "chunks" of CHUNKPAGES pages,
 * with each relation guaranteed at least one chunk.  This reduces thrashing
 * of the storage allocations when there are small changes in the RRFS page
 * counts from one VACUUM to the next.	(XXX it might also be worthwhile to
 * impose some kind of moving-average smoothing on the RRFS page counts?)
 *
 * So the actual arithmetic is: for each relation compute myRequest as the
 * number of chunks needed to hold its RRFS page count (not counting the
 * first, guaranteed chunk); compute sumRequests as the sum of these values
 * over all relations; then for each relation figure its target allocation
 * as
 *			1 + round(spareChunks * myRequest / sumRequests)
 * where spareChunks = totalChunks - numRels is the number of chunks we have
 * a choice what to do with.  We round off these numbers because truncating
 * all of them would waste significant space.  But because of roundoff, it's
 * possible for the last few relations to get less space than they should;
 * the target allocation must be checked against remaining available space.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <unistd.h>

#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/freespace.h"
#include "storage/itemptr.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"


/* Initial value for average-request moving average */
#define INITIAL_AVERAGE ((Size) (BLCKSZ / 32))

/*
 * Number of pages and bytes per allocation chunk.	Indexes can squeeze 50%
 * more pages into the same space because they don't need to remember how much
 * free space on each page.  The nominal number of pages, CHUNKPAGES, is for
 * regular rels, and INDEXCHUNKPAGES is for indexes.  CHUNKPAGES should be
 * even so that no space is wasted in the index case.
 */
#define CHUNKPAGES	16
#define CHUNKBYTES	(CHUNKPAGES * sizeof(FSMPageData))
#define INDEXCHUNKPAGES ((int) (CHUNKBYTES / sizeof(IndexFSMPageData)))


/*
 * Typedefs and macros for items in the page-storage arena.  We use the
 * existing ItemPointer and BlockId data structures, which are designed
 * to pack well (they should be 6 and 4 bytes apiece regardless of machine
 * alignment issues).  Unfortunately we can't use the ItemPointer access
 * macros, because they include Asserts insisting that ip_posid != 0.
 */
typedef ItemPointerData FSMPageData;
typedef BlockIdData IndexFSMPageData;

#define FSMPageGetPageNum(ptr)	\
	BlockIdGetBlockNumber(&(ptr)->ip_blkid)
#define FSMPageGetSpace(ptr)	\
	((Size) (ptr)->ip_posid)
#define FSMPageSetPageNum(ptr, pg)	\
	BlockIdSet(&(ptr)->ip_blkid, pg)
#define FSMPageSetSpace(ptr, sz)	\
	((ptr)->ip_posid = (OffsetNumber) (sz))
#define IndexFSMPageGetPageNum(ptr) \
	BlockIdGetBlockNumber(ptr)
#define IndexFSMPageSetPageNum(ptr, pg) \
	BlockIdSet(ptr, pg)

/*----------
 * During database shutdown, we store the contents of FSM into a disk file,
 * which is re-read during startup.  This way we don't have a startup
 * transient condition where FSM isn't really functioning.
 *
 * The file format is:
 *		label			"FSM\0"
 *		endian			constant 0x01020304 for detecting endianness problems
 *		version#
 *		numRels
 *	-- for each rel, in *reverse* usage order:
 *		relfilenode
 *		isIndex
 *		avgRequest
 *		lastPageCount
 *		storedPages
 *		arena data		array of storedPages FSMPageData or IndexFSMPageData
 *----------
 */

/* Name of FSM cache file (relative to $PGDATA) */
#define FSM_CACHE_FILENAME	"global/pg_fsm.cache"

/* Fixed values in header */
#define FSM_CACHE_LABEL		"FSM"
#define FSM_CACHE_ENDIAN	0x01020304
#define FSM_CACHE_VERSION	20030305

/* File header layout */
typedef struct FsmCacheFileHeader
{
	char		label[4];
	uint32		endian;
	uint32		version;
	int32		numRels;
} FsmCacheFileHeader;

/* Per-relation header */
typedef struct FsmCacheRelHeader
{
	RelFileNode key;			/* hash key (must be first) */
	bool		isIndex;		/* if true, we store only page numbers */
	uint32		avgRequest;		/* moving average of space requests */
	int32		lastPageCount;	/* pages passed to RecordRelationFreeSpace */
	int32		storedPages;	/* # of pages stored in arena */
} FsmCacheRelHeader;


/*
 * Shared free-space-map objects
 *
 * The per-relation objects are indexed by a hash table, and are also members
 * of two linked lists: one ordered by recency of usage (most recent first),
 * and the other ordered by physical location of the associated storage in
 * the page-info arena.
 *
 * Each relation owns one or more chunks of per-page storage in the "arena".
 * The chunks for each relation are always consecutive, so that it can treat
 * its page storage as a simple array.	We further insist that its page data
 * be ordered by block number, so that binary search is possible.
 *
 * Note: we handle pointers to these items as pointers, not as SHMEM_OFFSETs.
 * This assumes that all processes accessing the map will have the shared
 * memory segment mapped at the same place in their address space.
 */
typedef struct FSMHeader FSMHeader;
typedef struct FSMRelation FSMRelation;

/* Header for whole map */
struct FSMHeader
{
	HTAB	   *relHash;		/* hashtable of FSMRelation entries */
	FSMRelation *usageList;		/* FSMRelations in usage-recency order */
	FSMRelation *usageListTail; /* tail of usage-recency list */
	FSMRelation *firstRel;		/* FSMRelations in arena storage order */
	FSMRelation *lastRel;		/* tail of storage-order list */
	int			numRels;		/* number of FSMRelations now in use */
	double		sumRequests;	/* sum of requested chunks over all rels */
	char	   *arena;			/* arena for page-info storage */
	int			totalChunks;	/* total size of arena, in chunks */
	int			usedChunks;		/* # of chunks assigned */
	/* NB: there are totalChunks - usedChunks free chunks at end of arena */
};

/*
 * Per-relation struct --- this is an entry in the shared hash table.
 * The hash key is the RelFileNode value (hence, we look at the physical
 * relation ID, not the logical ID, which is appropriate).
 */
struct FSMRelation
{
	RelFileNode key;			/* hash key (must be first) */
	FSMRelation *nextUsage;		/* next rel in usage-recency order */
	FSMRelation *priorUsage;	/* prior rel in usage-recency order */
	FSMRelation *nextPhysical;	/* next rel in arena-storage order */
	FSMRelation *priorPhysical; /* prior rel in arena-storage order */
	bool		isIndex;		/* if true, we store only page numbers */
	Size		avgRequest;		/* moving average of space requests */
	int			lastPageCount;	/* pages passed to RecordRelationFreeSpace */
	int			firstChunk;		/* chunk # of my first chunk in arena */
	int			storedPages;	/* # of pages stored in arena */
	int			nextPage;		/* index (from 0) to start next search at */
};


int			MaxFSMRelations;	/* these are set by guc.c */
int			MaxFSMPages;

static FSMHeader *FreeSpaceMap; /* points to FSMHeader in shared memory */


static FSMRelation *lookup_fsm_rel(RelFileNode *rel);
static FSMRelation *create_fsm_rel(RelFileNode *rel);
static void delete_fsm_rel(FSMRelation *fsmrel);
static int	realloc_fsm_rel(FSMRelation *fsmrel, int nPages, bool isIndex);
static void link_fsm_rel_usage(FSMRelation *fsmrel);
static void unlink_fsm_rel_usage(FSMRelation *fsmrel);
static void link_fsm_rel_storage(FSMRelation *fsmrel);
static void unlink_fsm_rel_storage(FSMRelation *fsmrel);
static BlockNumber find_free_space(FSMRelation *fsmrel, Size spaceNeeded);
static BlockNumber find_index_free_space(FSMRelation *fsmrel);
static void fsm_record_free_space(FSMRelation *fsmrel, BlockNumber page,
					  Size spaceAvail);
static bool lookup_fsm_page_entry(FSMRelation *fsmrel, BlockNumber page,
					  int *outPageIndex);
static void compact_fsm_storage(void);
static void push_fsm_rels_after(FSMRelation *afterRel);
static void pack_incoming_pages(FSMPageData *newLocation, int newPages,
					PageFreeSpaceInfo *pageSpaces, int nPages);
static void pack_existing_pages(FSMPageData *newLocation, int newPages,
					FSMPageData *oldLocation, int oldPages);
static int	fsm_calc_request(FSMRelation *fsmrel);
static int	fsm_calc_target_allocation(int myRequest);
static int	fsm_current_chunks(FSMRelation *fsmrel);
static int	fsm_current_allocation(FSMRelation *fsmrel);


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
	int			nchunks;

	/* Create table header */
	FreeSpaceMap = (FSMHeader *) ShmemAlloc(sizeof(FSMHeader));
	if (FreeSpaceMap == NULL)
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
			   errmsg("insufficient shared memory for free space map")));
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
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
			   errmsg("insufficient shared memory for free space map")));

	/* Allocate page-storage arena */
	nchunks = (MaxFSMPages - 1) / CHUNKPAGES + 1;
	/* This check ensures spareChunks will be greater than zero */
	if (nchunks <= MaxFSMRelations)
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			   errmsg("max_fsm_pages must exceed max_fsm_relations * %d",
					  CHUNKPAGES)));

	FreeSpaceMap->arena = (char *) ShmemAlloc(nchunks * CHUNKBYTES);
	if (FreeSpaceMap->arena == NULL)
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
			   errmsg("insufficient shared memory for free space map")));

	FreeSpaceMap->totalChunks = nchunks;
	FreeSpaceMap->usedChunks = 0;
	FreeSpaceMap->sumRequests = 0;
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

	/* page-storage arena */
	nchunks = (MaxFSMPages - 1) / CHUNKPAGES + 1;

	if (nchunks >= (INT_MAX / CHUNKBYTES))
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("max_fsm_pages is too large")));

	size += MAXALIGN(nchunks * CHUNKBYTES);

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
 * amount of free space available on that page and then try again (see
 * RecordAndGetPageWithFreeSpace).	If InvalidBlockNumber is returned,
 * extend the relation.
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
	 * Update the moving average of space requests.  This code implements
	 * an exponential moving average with an equivalent period of about 63
	 * requests.  Ignore silly requests, however, to ensure that the
	 * average stays sane.
	 */
	if (spaceNeeded > 0 && spaceNeeded < BLCKSZ)
	{
		int			cur_avg = (int) fsmrel->avgRequest;

		cur_avg += ((int) spaceNeeded - cur_avg) / 32;
		fsmrel->avgRequest = (Size) cur_avg;
	}
	freepage = find_free_space(fsmrel, spaceNeeded);
	LWLockRelease(FreeSpaceLock);
	return freepage;
}

/*
 * RecordAndGetPageWithFreeSpace - update info about a page and try again.
 *
 * We provide this combo form, instead of a separate Record operation,
 * to save one lock and hash table lookup cycle.
 */
BlockNumber
RecordAndGetPageWithFreeSpace(RelFileNode *rel,
							  BlockNumber oldPage,
							  Size oldSpaceAvail,
							  Size spaceNeeded)
{
	FSMRelation *fsmrel;
	BlockNumber freepage;

	/* Sanity check: ensure spaceAvail will fit into OffsetNumber */
	AssertArg(oldSpaceAvail < BLCKSZ);

	LWLockAcquire(FreeSpaceLock, LW_EXCLUSIVE);

	/*
	 * We always add a rel to the hashtable when it is inquired about.
	 */
	fsmrel = create_fsm_rel(rel);

	/* Do the Record */
	fsm_record_free_space(fsmrel, oldPage, oldSpaceAvail);

	/*
	 * Update the moving average of space requests, same as in
	 * GetPageWithFreeSpace.
	 */
	if (spaceNeeded > 0 && spaceNeeded < BLCKSZ)
	{
		int			cur_avg = (int) fsmrel->avgRequest;

		cur_avg += ((int) spaceNeeded - cur_avg) / 32;
		fsmrel->avgRequest = (Size) cur_avg;
	}
	/* Do the Get */
	freepage = find_free_space(fsmrel, spaceNeeded);
	LWLockRelease(FreeSpaceLock);
	return freepage;
}

/*
 * GetAvgFSMRequestSize - get average FSM request size for a relation.
 *
 * If the relation is not known to FSM, return a default value.
 */
Size
GetAvgFSMRequestSize(RelFileNode *rel)
{
	Size		result;
	FSMRelation *fsmrel;

	LWLockAcquire(FreeSpaceLock, LW_EXCLUSIVE);
	fsmrel = lookup_fsm_rel(rel);
	if (fsmrel)
		result = fsmrel->avgRequest;
	else
		result = INITIAL_AVERAGE;
	LWLockRelease(FreeSpaceLock);
	return result;
}

/*
 * RecordRelationFreeSpace - record available-space info about a relation.
 *
 * Any pre-existing info about the relation is assumed obsolete and discarded.
 *
 * The given pageSpaces[] array must be sorted in order by blkno.  Note that
 * the FSM is at liberty to discard some or all of the data.
 */
void
RecordRelationFreeSpace(RelFileNode *rel,
						int nPages,
						PageFreeSpaceInfo *pageSpaces)
{
	FSMRelation *fsmrel;

	/* Limit nPages to something sane */
	if (nPages < 0)
		nPages = 0;
	else if (nPages > MaxFSMPages)
		nPages = MaxFSMPages;

	LWLockAcquire(FreeSpaceLock, LW_EXCLUSIVE);

	/*
	 * Note we don't record info about a relation unless there's already
	 * an FSM entry for it, implying someone has done GetPageWithFreeSpace
	 * for it.	Inactive rels thus will not clutter the map simply by
	 * being vacuumed.
	 */
	fsmrel = lookup_fsm_rel(rel);
	if (fsmrel)
	{
		int			curAlloc;
		int			curAllocPages;
		FSMPageData *newLocation;

		curAlloc = realloc_fsm_rel(fsmrel, nPages, false);
		curAllocPages = curAlloc * CHUNKPAGES;

		/*
		 * If the data fits in our current allocation, just copy it;
		 * otherwise must compress.
		 */
		newLocation = (FSMPageData *)
			(FreeSpaceMap->arena + fsmrel->firstChunk * CHUNKBYTES);
		if (nPages <= curAllocPages)
		{
			int			i;

			for (i = 0; i < nPages; i++)
			{
				BlockNumber page = pageSpaces[i].blkno;
				Size		avail = pageSpaces[i].avail;

				/* Check caller provides sorted data */
				if (i > 0 && page <= pageSpaces[i - 1].blkno)
					elog(ERROR, "free-space data is not in page order");
				FSMPageSetPageNum(newLocation, page);
				FSMPageSetSpace(newLocation, avail);
				newLocation++;
			}
			fsmrel->storedPages = nPages;
		}
		else
		{
			pack_incoming_pages(newLocation, curAllocPages,
								pageSpaces, nPages);
			fsmrel->storedPages = curAllocPages;
		}
	}
	LWLockRelease(FreeSpaceLock);
}

/*
 * GetFreeIndexPage - like GetPageWithFreeSpace, but for indexes
 */
BlockNumber
GetFreeIndexPage(RelFileNode *rel)
{
	FSMRelation *fsmrel;
	BlockNumber freepage;

	LWLockAcquire(FreeSpaceLock, LW_EXCLUSIVE);

	/*
	 * We always add a rel to the hashtable when it is inquired about.
	 */
	fsmrel = create_fsm_rel(rel);

	freepage = find_index_free_space(fsmrel);
	LWLockRelease(FreeSpaceLock);
	return freepage;
}

/*
 * RecordIndexFreeSpace - like RecordRelationFreeSpace, but for indexes
 */
void
RecordIndexFreeSpace(RelFileNode *rel,
					 int nPages,
					 BlockNumber *pages)
{
	FSMRelation *fsmrel;

	/* Limit nPages to something sane */
	if (nPages < 0)
		nPages = 0;
	else if (nPages > MaxFSMPages)
		nPages = MaxFSMPages;

	LWLockAcquire(FreeSpaceLock, LW_EXCLUSIVE);

	/*
	 * Note we don't record info about a relation unless there's already
	 * an FSM entry for it, implying someone has done GetFreeIndexPage for
	 * it.	Inactive rels thus will not clutter the map simply by being
	 * vacuumed.
	 */
	fsmrel = lookup_fsm_rel(rel);
	if (fsmrel)
	{
		int			curAlloc;
		int			curAllocPages;
		int			i;
		IndexFSMPageData *newLocation;

		curAlloc = realloc_fsm_rel(fsmrel, nPages, true);
		curAllocPages = curAlloc * INDEXCHUNKPAGES;

		/*
		 * If the data fits in our current allocation, just copy it;
		 * otherwise must compress.  But compression is easy: we merely
		 * forget extra pages.
		 */
		newLocation = (IndexFSMPageData *)
			(FreeSpaceMap->arena + fsmrel->firstChunk * CHUNKBYTES);
		if (nPages > curAllocPages)
			nPages = curAllocPages;

		for (i = 0; i < nPages; i++)
		{
			BlockNumber page = pages[i];

			/* Check caller provides sorted data */
			if (i > 0 && page <= pages[i - 1])
				elog(ERROR, "free-space data is not in page order");
			IndexFSMPageSetPageNum(newLocation, page);
			newLocation++;
		}
		fsmrel->storedPages = nPages;
	}
	LWLockRelease(FreeSpaceLock);
}

/*
 * FreeSpaceMapTruncateRel - adjust for truncation of a relation.
 *
 * We need to delete any stored data past the new relation length, so that
 * we don't bogusly return removed block numbers.
 */
void
FreeSpaceMapTruncateRel(RelFileNode *rel, BlockNumber nblocks)
{
	FSMRelation *fsmrel;

	LWLockAcquire(FreeSpaceLock, LW_EXCLUSIVE);
	fsmrel = lookup_fsm_rel(rel);
	if (fsmrel)
	{
		int			pageIndex;

		/* Use lookup to locate first entry >= nblocks */
		(void) lookup_fsm_page_entry(fsmrel, nblocks, &pageIndex);
		/* Delete all such entries */
		fsmrel->storedPages = pageIndex;
		/* XXX should we adjust rel's lastPageCount and sumRequests? */
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
	for (fsmrel = FreeSpaceMap->usageList; fsmrel; fsmrel = nextrel)
	{
		nextrel = fsmrel->nextUsage;	/* in case we delete it */
		if (fsmrel->key.tblNode == dbid)
			delete_fsm_rel(fsmrel);
	}
	LWLockRelease(FreeSpaceLock);
}

/*
 * PrintFreeSpaceMapStatistics - print statistics about FSM contents
 *
 * The info is sent to ereport() with the specified message level.	This is
 * intended for use during VACUUM.
 */
void
PrintFreeSpaceMapStatistics(int elevel)
{
	FSMRelation *fsmrel;
	int			storedPages = 0;
	int			numRels;
	double		sumRequests;
	double		needed;

	LWLockAcquire(FreeSpaceLock, LW_EXCLUSIVE);
	/* Count total space used --- tedious, but seems useful */
	for (fsmrel = FreeSpaceMap->firstRel;
		 fsmrel != NULL;
		 fsmrel = fsmrel->nextPhysical)
		storedPages += fsmrel->storedPages;
	/* Copy other stats before dropping lock */
	numRels = FreeSpaceMap->numRels;
	sumRequests = FreeSpaceMap->sumRequests;
	LWLockRelease(FreeSpaceLock);

	/* Convert stats to actual number of page slots needed */
	needed = (sumRequests + numRels) * CHUNKPAGES;

	ereport(elevel,
			(errmsg("free space map: %d relations, %d pages stored; %.0f total pages needed",
					numRels, storedPages, needed),
			 errdetail("Allocated FSM size: %d relations + %d pages = %.0f kB shared memory.",
					   MaxFSMRelations, MaxFSMPages,
					   (double) FreeSpaceShmemSize() / 1024.0)));
}

/*
 * DumpFreeSpaceMap - dump contents of FSM into a disk file for later reload
 *
 * This is expected to be called during database shutdown, after updates to
 * the FSM have stopped.  We lock the FreeSpaceLock but that's purely pro
 * forma --- if anyone else is still accessing FSM, there's a problem.
 */
void
DumpFreeSpaceMap(void)
{
	FILE	   *fp;
	char		cachefilename[MAXPGPATH];
	FsmCacheFileHeader header;
	FSMRelation *fsmrel;

	/* Try to create file */
	snprintf(cachefilename, sizeof(cachefilename), "%s/%s",
			 DataDir, FSM_CACHE_FILENAME);

	unlink(cachefilename);		/* in case it exists w/wrong permissions */

	fp = AllocateFile(cachefilename, PG_BINARY_W);
	if (fp == NULL)
	{
		elog(LOG, "could not write \"%s\": %m", cachefilename);
		return;
	}

	LWLockAcquire(FreeSpaceLock, LW_EXCLUSIVE);

	/* Write file header */
	MemSet(&header, 0, sizeof(header));
	strcpy(header.label, FSM_CACHE_LABEL);
	header.endian = FSM_CACHE_ENDIAN;
	header.version = FSM_CACHE_VERSION;
	header.numRels = FreeSpaceMap->numRels;
	if (fwrite(&header, 1, sizeof(header), fp) != sizeof(header))
		goto write_failed;

	/* For each relation, in order from least to most recently used... */
	for (fsmrel = FreeSpaceMap->usageListTail;
		 fsmrel != NULL;
		 fsmrel = fsmrel->priorUsage)
	{
		FsmCacheRelHeader relheader;
		int			nPages;

		/* Write relation header */
		MemSet(&relheader, 0, sizeof(relheader));
		relheader.key = fsmrel->key;
		relheader.isIndex = fsmrel->isIndex;
		relheader.avgRequest = fsmrel->avgRequest;
		relheader.lastPageCount = fsmrel->lastPageCount;
		relheader.storedPages = fsmrel->storedPages;
		if (fwrite(&relheader, 1, sizeof(relheader), fp) != sizeof(relheader))
			goto write_failed;

		/* Write the per-page data directly from the arena */
		nPages = fsmrel->storedPages;
		if (nPages > 0)
		{
			Size		len;
			char	   *data;

			if (fsmrel->isIndex)
				len = nPages * sizeof(IndexFSMPageData);
			else
				len = nPages * sizeof(FSMPageData);
			data = (char *)
				(FreeSpaceMap->arena + fsmrel->firstChunk * CHUNKBYTES);
			if (fwrite(data, 1, len, fp) != len)
				goto write_failed;
		}
	}

	/* Clean up */
	LWLockRelease(FreeSpaceLock);

	FreeFile(fp);

	return;

write_failed:
	elog(LOG, "could not write \"%s\": %m", cachefilename);

	/* Clean up */
	LWLockRelease(FreeSpaceLock);

	FreeFile(fp);

	/* Remove busted cache file */
	unlink(cachefilename);
}

/*
 * LoadFreeSpaceMap - load contents of FSM from a disk file
 *
 * This is expected to be called during database startup, before any FSM
 * updates begin.  We lock the FreeSpaceLock but that's purely pro
 * forma --- if anyone else is accessing FSM yet, there's a problem.
 *
 * Notes: no complaint is issued if no cache file is found.  If the file is
 * found, it is deleted after reading.	Thus, if we crash without a clean
 * shutdown, the next cycle of life starts with no FSM data.  To do otherwise,
 * we'd need to do significantly more validation in this routine, because of
 * the likelihood that what is in the dump file would be out-of-date, eg
 * there might be entries for deleted or truncated rels.
 */
void
LoadFreeSpaceMap(void)
{
	FILE	   *fp;
	char		cachefilename[MAXPGPATH];
	FsmCacheFileHeader header;
	int			relno;

	/* Try to open file */
	snprintf(cachefilename, sizeof(cachefilename), "%s/%s",
			 DataDir, FSM_CACHE_FILENAME);

	fp = AllocateFile(cachefilename, PG_BINARY_R);
	if (fp == NULL)
	{
		if (errno != ENOENT)
			elog(LOG, "could not read \"%s\": %m", cachefilename);
		return;
	}

	LWLockAcquire(FreeSpaceLock, LW_EXCLUSIVE);

	/* Read and verify file header */
	if (fread(&header, 1, sizeof(header), fp) != sizeof(header) ||
		strcmp(header.label, FSM_CACHE_LABEL) != 0 ||
		header.endian != FSM_CACHE_ENDIAN ||
		header.version != FSM_CACHE_VERSION ||
		header.numRels < 0)
	{
		elog(LOG, "bogus file header in \"%s\"", cachefilename);
		goto read_failed;
	}

	/* For each relation, in order from least to most recently used... */
	for (relno = 0; relno < header.numRels; relno++)
	{
		FsmCacheRelHeader relheader;
		Size		len;
		char	   *data;
		FSMRelation *fsmrel;
		int			nPages;
		int			curAlloc;
		int			curAllocPages;

		/* Read and verify relation header, as best we can */
		if (fread(&relheader, 1, sizeof(relheader), fp) != sizeof(relheader) ||
			(relheader.isIndex != false && relheader.isIndex != true) ||
			relheader.avgRequest >= BLCKSZ ||
			relheader.lastPageCount < 0 ||
			relheader.storedPages < 0)
		{
			elog(LOG, "bogus rel header in \"%s\"", cachefilename);
			goto read_failed;
		}

		/* Make sure lastPageCount doesn't exceed current MaxFSMPages */
		if (relheader.lastPageCount > MaxFSMPages)
			relheader.lastPageCount = MaxFSMPages;

		/* Read the per-page data */
		nPages = relheader.storedPages;
		if (relheader.isIndex)
			len = nPages * sizeof(IndexFSMPageData);
		else
			len = nPages * sizeof(FSMPageData);
		data = (char *) palloc(len + 1);		/* +1 to avoid palloc(0) */
		if (fread(data, 1, len, fp) != len)
		{
			elog(LOG, "premature EOF in \"%s\"", cachefilename);
			pfree(data);
			goto read_failed;
		}

		/*
		 * Okay, create the FSM entry and insert data into it.	Since the
		 * rels were stored in reverse usage order, at the end of the loop
		 * they will be correctly usage-ordered in memory; and if
		 * MaxFSMRelations is less than it used to be, we will correctly
		 * drop the least recently used ones.
		 */
		fsmrel = create_fsm_rel(&relheader.key);
		fsmrel->avgRequest = relheader.avgRequest;

		curAlloc = realloc_fsm_rel(fsmrel, relheader.lastPageCount,
								   relheader.isIndex);
		if (relheader.isIndex)
		{
			IndexFSMPageData *newLocation;

			curAllocPages = curAlloc * INDEXCHUNKPAGES;

			/*
			 * If the data fits in our current allocation, just copy it;
			 * otherwise must compress.  But compression is easy: we
			 * merely forget extra pages.
			 */
			newLocation = (IndexFSMPageData *)
				(FreeSpaceMap->arena + fsmrel->firstChunk * CHUNKBYTES);
			if (nPages > curAllocPages)
				nPages = curAllocPages;
			memcpy(newLocation, data, nPages * sizeof(IndexFSMPageData));
			fsmrel->storedPages = nPages;
		}
		else
		{
			FSMPageData *newLocation;

			curAllocPages = curAlloc * CHUNKPAGES;

			/*
			 * If the data fits in our current allocation, just copy it;
			 * otherwise must compress.
			 */
			newLocation = (FSMPageData *)
				(FreeSpaceMap->arena + fsmrel->firstChunk * CHUNKBYTES);
			if (nPages <= curAllocPages)
			{
				memcpy(newLocation, data, nPages * sizeof(FSMPageData));
				fsmrel->storedPages = nPages;
			}
			else
			{
				pack_existing_pages(newLocation, curAllocPages,
									(FSMPageData *) data, nPages);
				fsmrel->storedPages = curAllocPages;
			}
		}

		pfree(data);
	}

read_failed:

	/* Clean up */
	LWLockRelease(FreeSpaceLock);

	FreeFile(fp);

	/* Remove cache file before it can become stale; see notes above */
	unlink(cachefilename);
}


/*
 * Internal routines.  These all assume the caller holds the FreeSpaceLock.
 */

/*
 * Lookup a relation in the hash table.  If not present, return NULL.
 *
 * The relation's position in the LRU list is not changed.
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
 * On successful lookup, the relation is moved to the front of the LRU list.
 */
static FSMRelation *
create_fsm_rel(RelFileNode *rel)
{
	FSMRelation *fsmrel;
	bool		found;

	fsmrel = (FSMRelation *) hash_search(FreeSpaceMap->relHash,
										 (void *) rel,
										 HASH_ENTER,
										 &found);
	if (!fsmrel)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory")));

	if (!found)
	{
		/* New hashtable entry, initialize it (hash_search set the key) */
		fsmrel->isIndex = false;	/* until we learn different */
		fsmrel->avgRequest = INITIAL_AVERAGE;
		fsmrel->lastPageCount = 0;
		fsmrel->firstChunk = -1;	/* no space allocated */
		fsmrel->storedPages = 0;
		fsmrel->nextPage = 0;

		/* Discard lowest-priority existing rel, if we are over limit */
		if (FreeSpaceMap->numRels >= MaxFSMRelations)
			delete_fsm_rel(FreeSpaceMap->usageListTail);

		/* Add new entry at front of LRU list */
		link_fsm_rel_usage(fsmrel);
		fsmrel->nextPhysical = NULL;	/* not in physical-storage list */
		fsmrel->priorPhysical = NULL;
		FreeSpaceMap->numRels++;
		/* sumRequests is unchanged because request must be zero */
	}
	else
	{
		/* Existing entry, move to front of LRU list */
		if (fsmrel->priorUsage != NULL)
		{
			unlink_fsm_rel_usage(fsmrel);
			link_fsm_rel_usage(fsmrel);
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

	FreeSpaceMap->sumRequests -= fsm_calc_request(fsmrel);
	unlink_fsm_rel_usage(fsmrel);
	unlink_fsm_rel_storage(fsmrel);
	FreeSpaceMap->numRels--;
	result = (FSMRelation *) hash_search(FreeSpaceMap->relHash,
										 (void *) &(fsmrel->key),
										 HASH_REMOVE,
										 NULL);
	if (!result)
		elog(ERROR, "FreeSpaceMap hashtable corrupted");
}

/*
 * Reallocate space for a FSMRelation.
 *
 * This is shared code for RecordRelationFreeSpace and RecordIndexFreeSpace.
 * The return value is the actual new allocation, in chunks.
 */
static int
realloc_fsm_rel(FSMRelation *fsmrel, int nPages, bool isIndex)
{
	int			myRequest;
	int			myAlloc;
	int			curAlloc;

	/*
	 * Delete any existing entries, and update request status.
	 */
	fsmrel->storedPages = 0;
	FreeSpaceMap->sumRequests -= fsm_calc_request(fsmrel);
	fsmrel->lastPageCount = nPages;
	fsmrel->isIndex = isIndex;
	myRequest = fsm_calc_request(fsmrel);
	FreeSpaceMap->sumRequests += myRequest;
	myAlloc = fsm_calc_target_allocation(myRequest);

	/*
	 * Need to reallocate space if (a) my target allocation is more than
	 * my current allocation, AND (b) my actual immediate need
	 * (myRequest+1 chunks) is more than my current allocation. Otherwise
	 * just store the new data in-place.
	 */
	curAlloc = fsm_current_allocation(fsmrel);
	if (myAlloc > curAlloc && (myRequest + 1) > curAlloc && nPages > 0)
	{
		/* Remove entry from storage list, and compact */
		unlink_fsm_rel_storage(fsmrel);
		compact_fsm_storage();
		/* Reattach to end of storage list */
		link_fsm_rel_storage(fsmrel);
		/* And allocate storage */
		fsmrel->firstChunk = FreeSpaceMap->usedChunks;
		FreeSpaceMap->usedChunks += myAlloc;
		curAlloc = myAlloc;
		/* Watch out for roundoff error */
		if (FreeSpaceMap->usedChunks > FreeSpaceMap->totalChunks)
		{
			FreeSpaceMap->usedChunks = FreeSpaceMap->totalChunks;
			curAlloc = FreeSpaceMap->totalChunks - fsmrel->firstChunk;
		}
	}
	return curAlloc;
}

/*
 * Link a FSMRelation into the LRU list (always at the head).
 */
static void
link_fsm_rel_usage(FSMRelation *fsmrel)
{
	fsmrel->priorUsage = NULL;
	fsmrel->nextUsage = FreeSpaceMap->usageList;
	FreeSpaceMap->usageList = fsmrel;
	if (fsmrel->nextUsage != NULL)
		fsmrel->nextUsage->priorUsage = fsmrel;
	else
		FreeSpaceMap->usageListTail = fsmrel;
}

/*
 * Delink a FSMRelation from the LRU list.
 */
static void
unlink_fsm_rel_usage(FSMRelation *fsmrel)
{
	if (fsmrel->priorUsage != NULL)
		fsmrel->priorUsage->nextUsage = fsmrel->nextUsage;
	else
		FreeSpaceMap->usageList = fsmrel->nextUsage;
	if (fsmrel->nextUsage != NULL)
		fsmrel->nextUsage->priorUsage = fsmrel->priorUsage;
	else
		FreeSpaceMap->usageListTail = fsmrel->priorUsage;

	/*
	 * We don't bother resetting fsmrel's links, since it's about to be
	 * deleted or relinked at the head.
	 */
}

/*
 * Link a FSMRelation into the storage-order list (always at the tail).
 */
static void
link_fsm_rel_storage(FSMRelation *fsmrel)
{
	fsmrel->nextPhysical = NULL;
	fsmrel->priorPhysical = FreeSpaceMap->lastRel;
	if (FreeSpaceMap->lastRel != NULL)
		FreeSpaceMap->lastRel->nextPhysical = fsmrel;
	else
		FreeSpaceMap->firstRel = fsmrel;
	FreeSpaceMap->lastRel = fsmrel;
}

/*
 * Delink a FSMRelation from the storage-order list, if it's in it.
 */
static void
unlink_fsm_rel_storage(FSMRelation *fsmrel)
{
	if (fsmrel->priorPhysical != NULL || FreeSpaceMap->firstRel == fsmrel)
	{
		if (fsmrel->priorPhysical != NULL)
			fsmrel->priorPhysical->nextPhysical = fsmrel->nextPhysical;
		else
			FreeSpaceMap->firstRel = fsmrel->nextPhysical;
		if (fsmrel->nextPhysical != NULL)
			fsmrel->nextPhysical->priorPhysical = fsmrel->priorPhysical;
		else
			FreeSpaceMap->lastRel = fsmrel->priorPhysical;
	}
	/* mark as not in list, since we may not put it back immediately */
	fsmrel->nextPhysical = NULL;
	fsmrel->priorPhysical = NULL;
	/* Also mark it as having no storage */
	fsmrel->firstChunk = -1;
	fsmrel->storedPages = 0;
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
	FSMPageData *info;
	int			pagesToCheck,	/* outer loop counter */
				pageIndex;		/* current page index */

	if (fsmrel->isIndex)
		elog(ERROR, "find_free_space called for an index relation");
	info = (FSMPageData *)
		(FreeSpaceMap->arena + fsmrel->firstChunk * CHUNKBYTES);
	pageIndex = fsmrel->nextPage;
	/* Last operation may have left nextPage pointing past end */
	if (pageIndex >= fsmrel->storedPages)
		pageIndex = 0;

	for (pagesToCheck = fsmrel->storedPages; pagesToCheck > 0; pagesToCheck--)
	{
		FSMPageData *page = info + pageIndex;
		Size		spaceAvail = FSMPageGetSpace(page);

		/* Check this page */
		if (spaceAvail >= spaceNeeded)
		{
			/*
			 * Found what we want --- adjust the entry, and update
			 * nextPage.
			 */
			FSMPageSetSpace(page, spaceAvail - spaceNeeded);
			fsmrel->nextPage = pageIndex + 1;
			return FSMPageGetPageNum(page);
		}
		/* Advance pageIndex, wrapping around if needed */
		if (++pageIndex >= fsmrel->storedPages)
			pageIndex = 0;
	}

	return InvalidBlockNumber;	/* nothing found */
}

/*
 * As above, but for index case --- we only deal in whole pages.
 */
static BlockNumber
find_index_free_space(FSMRelation *fsmrel)
{
	IndexFSMPageData *info;
	BlockNumber result;

	/*
	 * If isIndex isn't set, it could be that RecordIndexFreeSpace() has
	 * never yet been called on this relation, and we're still looking at
	 * the default setting from create_fsm_rel().  If so, just act as
	 * though there's no space.
	 */
	if (!fsmrel->isIndex)
	{
		if (fsmrel->storedPages == 0)
			return InvalidBlockNumber;
		elog(ERROR, "find_index_free_space called for a non-index relation");
	}

	/*
	 * For indexes, there's no need for the nextPage state variable; we
	 * just remove and return the first available page.  (We could save
	 * cycles here by returning the last page, but it seems better to
	 * encourage re-use of lower-numbered pages.)
	 */
	if (fsmrel->storedPages <= 0)
		return InvalidBlockNumber;		/* no pages available */
	info = (IndexFSMPageData *)
		(FreeSpaceMap->arena + fsmrel->firstChunk * CHUNKBYTES);
	result = IndexFSMPageGetPageNum(info);
	fsmrel->storedPages--;
	memmove(info, info + 1, fsmrel->storedPages * sizeof(IndexFSMPageData));
	return result;
}

/*
 * fsm_record_free_space - guts of RecordFreeSpace operation (now only
 * provided as part of RecordAndGetPageWithFreeSpace).
 */
static void
fsm_record_free_space(FSMRelation *fsmrel, BlockNumber page, Size spaceAvail)
{
	int			pageIndex;

	if (fsmrel->isIndex)
		elog(ERROR, "fsm_record_free_space called for an index relation");
	if (lookup_fsm_page_entry(fsmrel, page, &pageIndex))
	{
		/* Found an existing entry for page; update it */
		FSMPageData *info;

		info = (FSMPageData *)
			(FreeSpaceMap->arena + fsmrel->firstChunk * CHUNKBYTES);
		info += pageIndex;
		FSMPageSetSpace(info, spaceAvail);
	}
	else
	{
		/*
		 * No existing entry; ignore the call.	We used to add the page to
		 * the FSM --- but in practice, if the page hasn't got enough
		 * space to satisfy the caller who's kicking it back to us, then
		 * it's probably uninteresting to everyone else as well.
		 */
	}
}

/*
 * Look for an entry for a specific page (block number) in a FSMRelation.
 * Returns TRUE if a matching entry exists, else FALSE.
 *
 * The output argument *outPageIndex is set to indicate where the entry exists
 * (if TRUE result) or could be inserted (if FALSE result).
 */
static bool
lookup_fsm_page_entry(FSMRelation *fsmrel, BlockNumber page,
					  int *outPageIndex)
{
	/* Check for empty relation */
	if (fsmrel->storedPages <= 0)
	{
		*outPageIndex = 0;
		return false;
	}

	/* Do binary search */
	if (fsmrel->isIndex)
	{
		IndexFSMPageData *info;
		int			low,
					high;

		info = (IndexFSMPageData *)
			(FreeSpaceMap->arena + fsmrel->firstChunk * CHUNKBYTES);
		low = 0;
		high = fsmrel->storedPages - 1;
		while (low <= high)
		{
			int			middle;
			BlockNumber probe;

			middle = low + (high - low) / 2;
			probe = IndexFSMPageGetPageNum(info + middle);
			if (probe == page)
			{
				*outPageIndex = middle;
				return true;
			}
			else if (probe < page)
				low = middle + 1;
			else
				high = middle - 1;
		}
		*outPageIndex = low;
		return false;
	}
	else
	{
		FSMPageData *info;
		int			low,
					high;

		info = (FSMPageData *)
			(FreeSpaceMap->arena + fsmrel->firstChunk * CHUNKBYTES);
		low = 0;
		high = fsmrel->storedPages - 1;
		while (low <= high)
		{
			int			middle;
			BlockNumber probe;

			middle = low + (high - low) / 2;
			probe = FSMPageGetPageNum(info + middle);
			if (probe == page)
			{
				*outPageIndex = middle;
				return true;
			}
			else if (probe < page)
				low = middle + 1;
			else
				high = middle - 1;
		}
		*outPageIndex = low;
		return false;
	}
}

/*
 * Re-pack the FSM storage arena, dropping data if necessary to meet the
 * current allocation target for each relation.  At conclusion, all available
 * space in the arena will be coalesced at the end.
 */
static void
compact_fsm_storage(void)
{
	int			nextChunkIndex = 0;
	bool		did_push = false;
	FSMRelation *fsmrel;

	for (fsmrel = FreeSpaceMap->firstRel;
		 fsmrel != NULL;
		 fsmrel = fsmrel->nextPhysical)
	{
		int			newAlloc;
		int			newAllocPages;
		int			newChunkIndex;
		int			oldChunkIndex;
		int			curChunks;
		char	   *newLocation;
		char	   *oldLocation;

		/*
		 * Calculate target allocation, make sure we don't overrun due to
		 * roundoff error
		 */
		newAlloc = fsm_calc_target_allocation(fsm_calc_request(fsmrel));
		if (newAlloc > FreeSpaceMap->totalChunks - nextChunkIndex)
			newAlloc = FreeSpaceMap->totalChunks - nextChunkIndex;
		if (fsmrel->isIndex)
			newAllocPages = newAlloc * INDEXCHUNKPAGES;
		else
			newAllocPages = newAlloc * CHUNKPAGES;

		/*
		 * Determine current size, current and new locations
		 */
		curChunks = fsm_current_chunks(fsmrel);
		oldChunkIndex = fsmrel->firstChunk;
		oldLocation = FreeSpaceMap->arena + oldChunkIndex * CHUNKBYTES;
		newChunkIndex = nextChunkIndex;
		newLocation = FreeSpaceMap->arena + newChunkIndex * CHUNKBYTES;

		/*
		 * It's possible that we have to move data down, not up, if the
		 * allocations of previous rels expanded.  This normally means that
		 * our allocation expanded too (or at least got no worse), and
		 * ditto for later rels.  So there should be room to move all our
		 * data down without dropping any --- but we might have to push down
		 * following rels to acquire the room.  We don't want to do the push
		 * more than once, so pack everything against the end of the arena
		 * if so.
		 *
		 * In corner cases where we are on the short end of a roundoff choice
		 * that we were formerly on the long end of, it's possible that we
		 * have to move down and compress our data too.  In fact, even after
		 * pushing down the following rels, there might not be as much space
		 * as we computed for this rel above --- that would imply that some
		 * following rel(s) are also on the losing end of roundoff choices.
		 * We could handle this fairly by doing the per-rel compactions
		 * out-of-order, but that seems like way too much complexity to deal
		 * with a very infrequent corner case.  Instead, we simply drop pages
		 * from the end of the current rel's data until it fits.
		 */
		if (newChunkIndex > oldChunkIndex)
		{
			int			limitChunkIndex;

			if (newAllocPages < fsmrel->storedPages)
			{
				/* move and compress --- just drop excess pages */
				fsmrel->storedPages = newAllocPages;
				curChunks = fsm_current_chunks(fsmrel);
			}
			/* is there enough space? */
			if (fsmrel->nextPhysical != NULL)
				limitChunkIndex = fsmrel->nextPhysical->firstChunk;
			else
				limitChunkIndex = FreeSpaceMap->totalChunks;
			if (newChunkIndex + curChunks > limitChunkIndex)
			{
				/* not enough space, push down following rels */
				if (!did_push)
				{
					push_fsm_rels_after(fsmrel);
					did_push = true;
				}
				/* now is there enough space? */
				if (fsmrel->nextPhysical != NULL)
					limitChunkIndex = fsmrel->nextPhysical->firstChunk;
				else
					limitChunkIndex = FreeSpaceMap->totalChunks;
				if (newChunkIndex + curChunks > limitChunkIndex)
				{
					/* uh-oh, forcibly cut the allocation to fit */
					newAlloc = limitChunkIndex - newChunkIndex;
					/*
					 * If newAlloc < 0 at this point, we are moving the rel's
					 * firstChunk into territory currently assigned to a later
					 * rel.  This is okay so long as we do not copy any data.
					 * The rels will be back in nondecreasing firstChunk order
					 * at completion of the compaction pass.
					 */
					if (newAlloc < 0)
						newAlloc = 0;
					if (fsmrel->isIndex)
						newAllocPages = newAlloc * INDEXCHUNKPAGES;
					else
						newAllocPages = newAlloc * CHUNKPAGES;
					fsmrel->storedPages = newAllocPages;
					curChunks = fsm_current_chunks(fsmrel);
				}
			}
			memmove(newLocation, oldLocation, curChunks * CHUNKBYTES);
		}
		else if (newAllocPages < fsmrel->storedPages)
		{
			/*
			 * Need to compress the page data.	For an index,
			 * "compression" just means dropping excess pages; otherwise
			 * we try to keep the ones with the most space.
			 */
			if (fsmrel->isIndex)
			{
				fsmrel->storedPages = newAllocPages;
				/* may need to move data */
				if (newChunkIndex != oldChunkIndex)
					memmove(newLocation, oldLocation, newAlloc * CHUNKBYTES);
			}
			else
			{
				pack_existing_pages((FSMPageData *) newLocation,
									newAllocPages,
									(FSMPageData *) oldLocation,
									fsmrel->storedPages);
				fsmrel->storedPages = newAllocPages;
			}
		}
		else if (newChunkIndex != oldChunkIndex)
		{
			/*
			 * No compression needed, but must copy the data up
			 */
			memmove(newLocation, oldLocation, curChunks * CHUNKBYTES);
		}
		fsmrel->firstChunk = newChunkIndex;
		nextChunkIndex += newAlloc;
	}
	Assert(nextChunkIndex <= FreeSpaceMap->totalChunks);
	FreeSpaceMap->usedChunks = nextChunkIndex;
}

/*
 * Push all FSMRels physically after afterRel to the end of the storage arena.
 *
 * We sometimes have to do this when deletion or truncation of a relation
 * causes the allocations of remaining rels to expand markedly.  We must
 * temporarily push existing data down to the end so that we can move it
 * back up in an orderly fashion.
 */
static void
push_fsm_rels_after(FSMRelation *afterRel)
{
	int			nextChunkIndex = FreeSpaceMap->totalChunks;
	FSMRelation *fsmrel;

	FreeSpaceMap->usedChunks = FreeSpaceMap->totalChunks;

	for (fsmrel = FreeSpaceMap->lastRel;
		 fsmrel != NULL;
		 fsmrel = fsmrel->priorPhysical)
	{
		int			chunkCount;
		int			newChunkIndex;
		int			oldChunkIndex;
		char	   *newLocation;
		char	   *oldLocation;

		if (fsmrel == afterRel)
			break;

		chunkCount = fsm_current_chunks(fsmrel);
		nextChunkIndex -= chunkCount;
		newChunkIndex = nextChunkIndex;
		oldChunkIndex = fsmrel->firstChunk;
		if (newChunkIndex < oldChunkIndex)
		{
			/* we're pushing down, how can it move up? */
			elog(PANIC, "inconsistent entry sizes in FSM");
		}
		else if (newChunkIndex > oldChunkIndex)
		{
			/* need to move it */
			newLocation = FreeSpaceMap->arena + newChunkIndex * CHUNKBYTES;
			oldLocation = FreeSpaceMap->arena + oldChunkIndex * CHUNKBYTES;
			memmove(newLocation, oldLocation, chunkCount * CHUNKBYTES);
			fsmrel->firstChunk = newChunkIndex;
		}
	}
	Assert(nextChunkIndex >= 0);
}

/*
 * Pack a set of per-page freespace data into a smaller amount of space.
 *
 * The method is to compute a low-resolution histogram of the free space
 * amounts, then determine which histogram bin contains the break point.
 * We then keep all pages above that bin, none below it, and just enough
 * of the pages in that bin to fill the output area exactly.
 */
#define HISTOGRAM_BINS	64

static void
pack_incoming_pages(FSMPageData *newLocation, int newPages,
					PageFreeSpaceInfo *pageSpaces, int nPages)
{
	int			histogram[HISTOGRAM_BINS];
	int			above,
				binct,
				i;
	Size		thresholdL,
				thresholdU;

	Assert(newPages < nPages);	/* else I shouldn't have been called */
	/* Build histogram */
	MemSet(histogram, 0, sizeof(histogram));
	for (i = 0; i < nPages; i++)
	{
		Size		avail = pageSpaces[i].avail;

		if (avail >= BLCKSZ)
			elog(ERROR, "bogus freespace amount");
		avail /= (BLCKSZ / HISTOGRAM_BINS);
		histogram[avail]++;
	}
	/* Find the breakpoint bin */
	above = 0;
	for (i = HISTOGRAM_BINS - 1; i >= 0; i--)
	{
		int			sum = above + histogram[i];

		if (sum > newPages)
			break;
		above = sum;
	}
	Assert(i >= 0);
	thresholdL = i * BLCKSZ / HISTOGRAM_BINS;	/* low bound of bp bin */
	thresholdU = (i + 1) * BLCKSZ / HISTOGRAM_BINS;		/* hi bound */
	binct = newPages - above;	/* number to take from bp bin */
	/* And copy the appropriate data */
	for (i = 0; i < nPages; i++)
	{
		BlockNumber page = pageSpaces[i].blkno;
		Size		avail = pageSpaces[i].avail;

		/* Check caller provides sorted data */
		if (i > 0 && page <= pageSpaces[i - 1].blkno)
			elog(ERROR, "free-space data is not in page order");
		/* Save this page? */
		if (avail >= thresholdU ||
			(avail >= thresholdL && (--binct >= 0)))
		{
			FSMPageSetPageNum(newLocation, page);
			FSMPageSetSpace(newLocation, avail);
			newLocation++;
			newPages--;
		}
	}
	Assert(newPages == 0);
}

/*
 * Pack a set of per-page freespace data into a smaller amount of space.
 *
 * This is algorithmically identical to pack_incoming_pages(), but accepts
 * a different input representation.  Also, we assume the input data has
 * previously been checked for validity (size in bounds, pages in order).
 *
 * Note: it is possible for the source and destination arrays to overlap.
 * The caller is responsible for making sure newLocation is at lower addresses
 * so that we can copy data moving forward in the arrays without problem.
 */
static void
pack_existing_pages(FSMPageData *newLocation, int newPages,
					FSMPageData *oldLocation, int oldPages)
{
	int			histogram[HISTOGRAM_BINS];
	int			above,
				binct,
				i;
	Size		thresholdL,
				thresholdU;

	Assert(newPages < oldPages);	/* else I shouldn't have been called */
	/* Build histogram */
	MemSet(histogram, 0, sizeof(histogram));
	for (i = 0; i < oldPages; i++)
	{
		Size		avail = FSMPageGetSpace(oldLocation + i);

		/* Shouldn't happen, but test to protect against stack clobber */
		if (avail >= BLCKSZ)
			elog(ERROR, "bogus freespace amount");
		avail /= (BLCKSZ / HISTOGRAM_BINS);
		histogram[avail]++;
	}
	/* Find the breakpoint bin */
	above = 0;
	for (i = HISTOGRAM_BINS - 1; i >= 0; i--)
	{
		int			sum = above + histogram[i];

		if (sum > newPages)
			break;
		above = sum;
	}
	Assert(i >= 0);
	thresholdL = i * BLCKSZ / HISTOGRAM_BINS;	/* low bound of bp bin */
	thresholdU = (i + 1) * BLCKSZ / HISTOGRAM_BINS;		/* hi bound */
	binct = newPages - above;	/* number to take from bp bin */
	/* And copy the appropriate data */
	for (i = 0; i < oldPages; i++)
	{
		BlockNumber page = FSMPageGetPageNum(oldLocation + i);
		Size		avail = FSMPageGetSpace(oldLocation + i);

		/* Save this page? */
		if (avail >= thresholdU ||
			(avail >= thresholdL && (--binct >= 0)))
		{
			FSMPageSetPageNum(newLocation, page);
			FSMPageSetSpace(newLocation, avail);
			newLocation++;
			newPages--;
		}
	}
	Assert(newPages == 0);
}

/*
 * Calculate number of chunks "requested" by a rel.
 *
 * Rel's lastPageCount and isIndex settings must be up-to-date when called.
 *
 * See notes at top of file for details.
 */
static int
fsm_calc_request(FSMRelation *fsmrel)
{
	int			chunkCount;

	/* Convert page count to chunk count */
	if (fsmrel->isIndex)
		chunkCount = (fsmrel->lastPageCount - 1) / INDEXCHUNKPAGES + 1;
	else
		chunkCount = (fsmrel->lastPageCount - 1) / CHUNKPAGES + 1;
	/* "Request" is anything beyond our one guaranteed chunk */
	if (chunkCount <= 0)
		return 0;
	else
		return chunkCount - 1;
}

/*
 * Calculate target allocation (number of chunks) for a rel
 *
 * Parameter is the result from fsm_calc_request().  The global sumRequests
 * and numRels totals must be up-to-date already.
 *
 * See notes at top of file for details.
 */
static int
fsm_calc_target_allocation(int myRequest)
{
	double		spareChunks;
	int			extra;

	spareChunks = FreeSpaceMap->totalChunks - FreeSpaceMap->numRels;
	Assert(spareChunks > 0);
	if (spareChunks >= FreeSpaceMap->sumRequests)
	{
		/* We aren't oversubscribed, so allocate exactly the request */
		extra = myRequest;
	}
	else
	{
		extra = (int) rint(spareChunks * myRequest / FreeSpaceMap->sumRequests);
		if (extra < 0)			/* shouldn't happen, but make sure */
			extra = 0;
	}
	return 1 + extra;
}

/*
 * Calculate number of chunks actually used to store current data
 */
static int
fsm_current_chunks(FSMRelation *fsmrel)
{
	int			chunkCount;

	/* Make sure storedPages==0 produces right answer */
	if (fsmrel->storedPages <= 0)
		return 0;
	/* Convert page count to chunk count */
	if (fsmrel->isIndex)
		chunkCount = (fsmrel->storedPages - 1) / INDEXCHUNKPAGES + 1;
	else
		chunkCount = (fsmrel->storedPages - 1) / CHUNKPAGES + 1;
	return chunkCount;
}

/*
 * Calculate current actual allocation (number of chunks) for a rel
 */
static int
fsm_current_allocation(FSMRelation *fsmrel)
{
	if (fsmrel->nextPhysical != NULL)
		return fsmrel->nextPhysical->firstChunk - fsmrel->firstChunk;
	else if (fsmrel == FreeSpaceMap->lastRel)
		return FreeSpaceMap->usedChunks - fsmrel->firstChunk;
	else
	{
		/* it's not in the storage-order list */
		Assert(fsmrel->firstChunk < 0 && fsmrel->storedPages == 0);
		return 0;
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
	int			nPages;

	for (fsmrel = FreeSpaceMap->usageList; fsmrel; fsmrel = fsmrel->nextUsage)
	{
		relNum++;
		fprintf(stderr, "Map %d: rel %u/%u isIndex %d avgRequest %u lastPageCount %d nextPage %d\nMap= ",
				relNum, fsmrel->key.tblNode, fsmrel->key.relNode,
				(int) fsmrel->isIndex, fsmrel->avgRequest,
				fsmrel->lastPageCount, fsmrel->nextPage);
		if (fsmrel->isIndex)
		{
			IndexFSMPageData *page;

			page = (IndexFSMPageData *)
				(FreeSpaceMap->arena + fsmrel->firstChunk * CHUNKBYTES);
			for (nPages = 0; nPages < fsmrel->storedPages; nPages++)
			{
				fprintf(stderr, " %u",
						IndexFSMPageGetPageNum(page));
				page++;
			}
		}
		else
		{
			FSMPageData *page;

			page = (FSMPageData *)
				(FreeSpaceMap->arena + fsmrel->firstChunk * CHUNKBYTES);
			for (nPages = 0; nPages < fsmrel->storedPages; nPages++)
			{
				fprintf(stderr, " %u:%u",
						FSMPageGetPageNum(page),
						FSMPageGetSpace(page));
				page++;
			}
		}
		fprintf(stderr, "\n");
		/* Cross-check list links */
		if (prevrel != fsmrel->priorUsage)
			fprintf(stderr, "DumpFreeSpace: broken list links\n");
		prevrel = fsmrel;
	}
	if (prevrel != FreeSpaceMap->usageListTail)
		fprintf(stderr, "DumpFreeSpace: broken list links\n");
	/* Cross-check global counters */
	if (relNum != FreeSpaceMap->numRels)
		fprintf(stderr, "DumpFreeSpace: %d rels in list, but numRels = %d\n",
				relNum, FreeSpaceMap->numRels);
}

#endif   /* FREESPACE_DEBUG */
