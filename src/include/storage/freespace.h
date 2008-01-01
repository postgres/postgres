/*-------------------------------------------------------------------------
 *
 * freespace.h
 *	  POSTGRES free space map for quickly finding free space in relations
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/storage/freespace.h,v 1.27 2008/01/01 19:45:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FREESPACE_H_
#define FREESPACE_H_

#include "storage/relfilenode.h"
#include "storage/itemptr.h"


/*
 * exported types
 */
typedef struct PageFreeSpaceInfo
{
	BlockNumber blkno;			/* which page in relation */
	Size		avail;			/* space available on this page */
} PageFreeSpaceInfo;


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
	BlockNumber interestingPages;		/* # of pages with useful free space */
	int			firstChunk;		/* chunk # of my first chunk in arena */
	int			storedPages;	/* # of pages stored in arena */
	int			nextPage;		/* index (from 0) to start next search at */
};



/* GUC variables */
extern PGDLLIMPORT int MaxFSMRelations;
extern PGDLLIMPORT int MaxFSMPages;


/*
 * function prototypes
 */
extern void InitFreeSpaceMap(void);
extern Size FreeSpaceShmemSize(void);
extern FSMHeader *GetFreeSpaceMap(void);

extern BlockNumber GetPageWithFreeSpace(RelFileNode *rel, Size spaceNeeded);
extern BlockNumber RecordAndGetPageWithFreeSpace(RelFileNode *rel,
							  BlockNumber oldPage,
							  Size oldSpaceAvail,
							  Size spaceNeeded);
extern Size GetAvgFSMRequestSize(RelFileNode *rel);
extern void RecordRelationFreeSpace(RelFileNode *rel,
						BlockNumber interestingPages,
						int nPages,
						PageFreeSpaceInfo *pageSpaces);

extern BlockNumber GetFreeIndexPage(RelFileNode *rel);
extern void RecordIndexFreeSpace(RelFileNode *rel,
					 BlockNumber interestingPages,
					 int nPages,
					 BlockNumber *pages);

extern void FreeSpaceMapTruncateRel(RelFileNode *rel, BlockNumber nblocks);
extern void FreeSpaceMapForgetRel(RelFileNode *rel);
extern void FreeSpaceMapForgetDatabase(Oid dbid);

extern void PrintFreeSpaceMapStatistics(int elevel);

extern void DumpFreeSpaceMap(int code, Datum arg);
extern void LoadFreeSpaceMap(void);

#ifdef FREESPACE_DEBUG
extern void DumpFreeSpace(void);
#endif

#endif   /* FREESPACE_H */
