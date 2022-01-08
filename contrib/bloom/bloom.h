/*-------------------------------------------------------------------------
 *
 * bloom.h
 *	  Header for bloom index.
 *
 * Copyright (c) 2016-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/bloom/bloom.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _BLOOM_H_
#define _BLOOM_H_

#include "access/amapi.h"
#include "access/generic_xlog.h"
#include "access/itup.h"
#include "access/xlog.h"
#include "fmgr.h"
#include "nodes/pathnodes.h"

/* Support procedures numbers */
#define BLOOM_HASH_PROC			1
#define BLOOM_OPTIONS_PROC		2
#define BLOOM_NPROC				2

/* Scan strategies */
#define BLOOM_EQUAL_STRATEGY	1
#define BLOOM_NSTRATEGIES		1

/* Opaque for bloom pages */
typedef struct BloomPageOpaqueData
{
	OffsetNumber maxoff;		/* number of index tuples on page */
	uint16		flags;			/* see bit definitions below */
	uint16		unused;			/* placeholder to force maxaligning of size of
								 * BloomPageOpaqueData and to place
								 * bloom_page_id exactly at the end of page */
	uint16		bloom_page_id;	/* for identification of BLOOM indexes */
} BloomPageOpaqueData;

typedef BloomPageOpaqueData *BloomPageOpaque;

/* Bloom page flags */
#define BLOOM_META		(1<<0)
#define BLOOM_DELETED	(2<<0)

/*
 * The page ID is for the convenience of pg_filedump and similar utilities,
 * which otherwise would have a hard time telling pages of different index
 * types apart.  It should be the last 2 bytes on the page.  This is more or
 * less "free" due to alignment considerations.
 *
 * See comments above GinPageOpaqueData.
 */
#define BLOOM_PAGE_ID		0xFF83

/* Macros for accessing bloom page structures */
#define BloomPageGetOpaque(page) ((BloomPageOpaque) PageGetSpecialPointer(page))
#define BloomPageGetMaxOffset(page) (BloomPageGetOpaque(page)->maxoff)
#define BloomPageIsMeta(page) \
	((BloomPageGetOpaque(page)->flags & BLOOM_META) != 0)
#define BloomPageIsDeleted(page) \
	((BloomPageGetOpaque(page)->flags & BLOOM_DELETED) != 0)
#define BloomPageSetDeleted(page) \
	(BloomPageGetOpaque(page)->flags |= BLOOM_DELETED)
#define BloomPageSetNonDeleted(page) \
	(BloomPageGetOpaque(page)->flags &= ~BLOOM_DELETED)
#define BloomPageGetData(page)		((BloomTuple *)PageGetContents(page))
#define BloomPageGetTuple(state, page, offset) \
	((BloomTuple *)(PageGetContents(page) \
		+ (state)->sizeOfBloomTuple * ((offset) - 1)))
#define BloomPageGetNextTuple(state, tuple) \
	((BloomTuple *)((Pointer)(tuple) + (state)->sizeOfBloomTuple))

/* Preserved page numbers */
#define BLOOM_METAPAGE_BLKNO	(0)
#define BLOOM_HEAD_BLKNO		(1) /* first data page */

/*
 * We store Bloom signatures as arrays of uint16 words.
 */
typedef uint16 BloomSignatureWord;

#define SIGNWORDBITS ((int) (BITS_PER_BYTE * sizeof(BloomSignatureWord)))

/*
 * Default and maximum Bloom signature length in bits.
 */
#define DEFAULT_BLOOM_LENGTH	(5 * SIGNWORDBITS)
#define MAX_BLOOM_LENGTH		(256 * SIGNWORDBITS)

/*
 * Default and maximum signature bits generated per index key.
 */
#define DEFAULT_BLOOM_BITS		2
#define MAX_BLOOM_BITS			(MAX_BLOOM_LENGTH - 1)

/* Bloom index options */
typedef struct BloomOptions
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int			bloomLength;	/* length of signature in words (not bits!) */
	int			bitSize[INDEX_MAX_KEYS];	/* # of bits generated for each
											 * index key */
} BloomOptions;

/*
 * FreeBlockNumberArray - array of block numbers sized so that metadata fill
 * all space in metapage.
 */
typedef BlockNumber FreeBlockNumberArray[
										 MAXALIGN_DOWN(
													   BLCKSZ - SizeOfPageHeaderData - MAXALIGN(sizeof(BloomPageOpaqueData))
													   - MAXALIGN(sizeof(uint16) * 2 + sizeof(uint32) + sizeof(BloomOptions))
													   ) / sizeof(BlockNumber)
];

/* Metadata of bloom index */
typedef struct BloomMetaPageData
{
	uint32		magickNumber;
	uint16		nStart;
	uint16		nEnd;
	BloomOptions opts;
	FreeBlockNumberArray notFullPage;
} BloomMetaPageData;

/* Magic number to distinguish bloom pages among anothers */
#define BLOOM_MAGICK_NUMBER (0xDBAC0DED)

/* Number of blocks numbers fit in BloomMetaPageData */
#define BloomMetaBlockN		(sizeof(FreeBlockNumberArray) / sizeof(BlockNumber))

#define BloomPageGetMeta(page)	((BloomMetaPageData *) PageGetContents(page))

typedef struct BloomState
{
	FmgrInfo	hashFn[INDEX_MAX_KEYS];
	Oid			collations[INDEX_MAX_KEYS];
	BloomOptions opts;			/* copy of options on index's metapage */
	int32		nColumns;

	/*
	 * sizeOfBloomTuple is index-specific, and it depends on reloptions, so
	 * precompute it
	 */
	Size		sizeOfBloomTuple;
} BloomState;

#define BloomPageGetFreeSpace(state, page) \
	(BLCKSZ - MAXALIGN(SizeOfPageHeaderData) \
		- BloomPageGetMaxOffset(page) * (state)->sizeOfBloomTuple \
		- MAXALIGN(sizeof(BloomPageOpaqueData)))

/*
 * Tuples are very different from all other relations
 */
typedef struct BloomTuple
{
	ItemPointerData heapPtr;
	BloomSignatureWord sign[FLEXIBLE_ARRAY_MEMBER];
} BloomTuple;

#define BLOOMTUPLEHDRSZ offsetof(BloomTuple, sign)

/* Opaque data structure for bloom index scan */
typedef struct BloomScanOpaqueData
{
	BloomSignatureWord *sign;	/* Scan signature */
	BloomState	state;
} BloomScanOpaqueData;

typedef BloomScanOpaqueData *BloomScanOpaque;

/* blutils.c */
extern void _PG_init(void);
extern void initBloomState(BloomState *state, Relation index);
extern void BloomFillMetapage(Relation index, Page metaPage);
extern void BloomInitMetapage(Relation index);
extern void BloomInitPage(Page page, uint16 flags);
extern Buffer BloomNewBuffer(Relation index);
extern void signValue(BloomState *state, BloomSignatureWord *sign, Datum value, int attno);
extern BloomTuple *BloomFormTuple(BloomState *state, ItemPointer iptr, Datum *values, bool *isnull);
extern bool BloomPageAddItem(BloomState *state, Page page, BloomTuple *tuple);

/* blvalidate.c */
extern bool blvalidate(Oid opclassoid);

/* index access method interface functions */
extern bool blinsert(Relation index, Datum *values, bool *isnull,
					 ItemPointer ht_ctid, Relation heapRel,
					 IndexUniqueCheck checkUnique,
					 bool indexUnchanged,
					 struct IndexInfo *indexInfo);
extern IndexScanDesc blbeginscan(Relation r, int nkeys, int norderbys);
extern int64 blgetbitmap(IndexScanDesc scan, TIDBitmap *tbm);
extern void blrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
					 ScanKey orderbys, int norderbys);
extern void blendscan(IndexScanDesc scan);
extern IndexBuildResult *blbuild(Relation heap, Relation index,
								 struct IndexInfo *indexInfo);
extern void blbuildempty(Relation index);
extern IndexBulkDeleteResult *blbulkdelete(IndexVacuumInfo *info,
										   IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback,
										   void *callback_state);
extern IndexBulkDeleteResult *blvacuumcleanup(IndexVacuumInfo *info,
											  IndexBulkDeleteResult *stats);
extern bytea *bloptions(Datum reloptions, bool validate);
extern void blcostestimate(PlannerInfo *root, IndexPath *path,
						   double loop_count, Cost *indexStartupCost,
						   Cost *indexTotalCost, Selectivity *indexSelectivity,
						   double *indexCorrelation, double *indexPages);

#endif
