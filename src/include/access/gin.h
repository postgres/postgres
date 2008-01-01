/*--------------------------------------------------------------------------
 * gin.h
 *	  header file for postgres inverted index access method implementation.
 *
 *	Copyright (c) 2006-2008, PostgreSQL Global Development Group
 *
 *	$PostgreSQL: pgsql/src/include/access/gin.h,v 1.16 2008/01/01 19:45:56 momjian Exp $
 *--------------------------------------------------------------------------
 */


#ifndef GIN_H
#define GIN_H

#include "access/relscan.h"
#include "access/skey.h"
#include "access/xlog.h"
#include "access/xlogdefs.h"
#include "storage/bufpage.h"
#include "storage/off.h"
#include "utils/rel.h"
#include "access/itup.h"
#include "fmgr.h"


/*
 * amproc indexes for inverted indexes.
 */
#define GIN_COMPARE_PROC			   1
#define GIN_EXTRACTVALUE_PROC		   2
#define GIN_EXTRACTQUERY_PROC		   3
#define GIN_CONSISTENT_PROC			   4
#define GINNProcs					   4

/*
 * Page opaque data in a inverted index page.
 *
 * Note: GIN does not include a page ID word as do the other index types.
 * This is OK because the opaque data is only 8 bytes and so can be reliably
 * distinguished by size.  Revisit this if the size ever increases.
 */
typedef struct GinPageOpaqueData
{
	BlockNumber rightlink;		/* next page if any */
	OffsetNumber maxoff;		/* number entries on GIN_DATA page: number of
								 * heap ItemPointer on GIN_DATA|GIN_LEAF page
								 * and number of records on GIN_DATA &
								 * ~GIN_LEAF page */
	uint16		flags;			/* see bit definitions below */
} GinPageOpaqueData;

typedef GinPageOpaqueData *GinPageOpaque;

#define GIN_ROOT_BLKNO	(0)

#define GIN_DATA		  (1 << 0)
#define GIN_LEAF		  (1 << 1)
#define GIN_DELETED		  (1 << 2)

/*
 * Works on page
 */
#define GinPageGetOpaque(page) ( (GinPageOpaque) PageGetSpecialPointer(page) )

#define GinPageIsLeaf(page)    ( GinPageGetOpaque(page)->flags & GIN_LEAF )
#define GinPageSetLeaf(page)   ( GinPageGetOpaque(page)->flags |= GIN_LEAF )
#define GinPageSetNonLeaf(page)    ( GinPageGetOpaque(page)->flags &= ~GIN_LEAF )
#define GinPageIsData(page)    ( GinPageGetOpaque(page)->flags & GIN_DATA )
#define GinPageSetData(page)   ( GinPageGetOpaque(page)->flags |= GIN_DATA )

#define GinPageIsDeleted(page) ( GinPageGetOpaque(page)->flags & GIN_DELETED)
#define GinPageSetDeleted(page)    ( GinPageGetOpaque(page)->flags |= GIN_DELETED)
#define GinPageSetNonDeleted(page) ( GinPageGetOpaque(page)->flags &= ~GIN_DELETED)

#define GinPageRightMost(page) ( GinPageGetOpaque(page)->rightlink == InvalidBlockNumber)

/*
 * Define our ItemPointerGet(BlockNumber|GetOffsetNumber)
 * to prevent asserts
 */

#define GinItemPointerGetBlockNumber(pointer) \
	BlockIdGetBlockNumber(&(pointer)->ip_blkid)

#define GinItemPointerGetOffsetNumber(pointer) \
	((pointer)->ip_posid)

typedef struct
{
	BlockIdData child_blkno;	/* use it instead of BlockNumber to save space
								 * on page */
	ItemPointerData key;
} PostingItem;

#define PostingItemGetBlockNumber(pointer) \
	BlockIdGetBlockNumber(&(pointer)->child_blkno)

#define PostingItemSetBlockNumber(pointer, blockNumber) \
	BlockIdSet(&((pointer)->child_blkno), (blockNumber))

/*
 * Support work on IndexTuple on leaf pages
 */
#define GinGetNPosting(itup)	GinItemPointerGetOffsetNumber(&(itup)->t_tid)
#define GinSetNPosting(itup,n)	ItemPointerSetOffsetNumber(&(itup)->t_tid,(n))
#define GIN_TREE_POSTING		((OffsetNumber)0xffff)
#define GinIsPostingTree(itup)	( GinGetNPosting(itup)==GIN_TREE_POSTING )
#define GinSetPostingTree(itup, blkno)	( GinSetNPosting((itup),GIN_TREE_POSTING ), ItemPointerSetBlockNumber(&(itup)->t_tid, blkno) )
#define GinGetPostingTree(itup) GinItemPointerGetBlockNumber(&(itup)->t_tid)

#define GinGetOrigSizePosting(itup) GinItemPointerGetBlockNumber(&(itup)->t_tid)
#define GinSetOrigSizePosting(itup,n)	ItemPointerSetBlockNumber(&(itup)->t_tid,(n))
#define GinGetPosting(itup)			( (ItemPointer)(( ((char*)(itup)) + SHORTALIGN(GinGetOrigSizePosting(itup)) )) )

#define GinMaxItemSize \
	((BLCKSZ - SizeOfPageHeaderData - \
		MAXALIGN(sizeof(GinPageOpaqueData))) / 3 - sizeof(ItemIdData))


/*
 * Data (posting tree) pages
 */
#define GinDataPageGetData(page)	\
	(PageGetContents(page)+MAXALIGN(sizeof(ItemPointerData)))
#define GinDataPageGetRightBound(page)	((ItemPointer)PageGetContents(page))
#define GinSizeOfItem(page) ( (GinPageIsLeaf(page)) ? sizeof(ItemPointerData) : sizeof(PostingItem) )
#define GinDataPageGetItem(page,i) ( GinDataPageGetData(page) + ((i)-1) * GinSizeOfItem(page) )

#define GinDataPageGetFreeSpace(page)	\
	( BLCKSZ - SizeOfPageHeaderData - MAXALIGN(sizeof(GinPageOpaqueData)) - \
		GinPageGetOpaque(page)->maxoff * GinSizeOfItem(page) - \
		MAXALIGN(sizeof(ItemPointerData)))



#define GIN_UNLOCK	BUFFER_LOCK_UNLOCK
#define GIN_SHARE	BUFFER_LOCK_SHARE
#define GIN_EXCLUSIVE  BUFFER_LOCK_EXCLUSIVE

typedef struct GinState
{
	FmgrInfo	compareFn;
	FmgrInfo	extractValueFn;
	FmgrInfo	extractQueryFn;
	FmgrInfo	consistentFn;

	TupleDesc	tupdesc;
} GinState;

/* XLog stuff */

#define XLOG_GIN_CREATE_INDEX  0x00

#define XLOG_GIN_CREATE_PTREE  0x10

typedef struct ginxlogCreatePostingTree
{
	RelFileNode node;
	BlockNumber blkno;
	uint32		nitem;
	/* follows list of heap's ItemPointer */
} ginxlogCreatePostingTree;

#define XLOG_GIN_INSERT  0x20

typedef struct ginxlogInsert
{
	RelFileNode node;
	BlockNumber blkno;
	BlockNumber updateBlkno;
	OffsetNumber offset;
	bool		isDelete;
	bool		isData;
	bool		isLeaf;
	OffsetNumber nitem;

	/*
	 * follows: tuples or ItemPointerData or PostingItem or list of
	 * ItemPointerData
	 */
} ginxlogInsert;

#define XLOG_GIN_SPLIT	0x30

typedef struct ginxlogSplit
{
	RelFileNode node;
	BlockNumber lblkno;
	BlockNumber rootBlkno;
	BlockNumber rblkno;
	BlockNumber rrlink;
	OffsetNumber separator;
	OffsetNumber nitem;

	bool		isData;
	bool		isLeaf;
	bool		isRootSplit;

	BlockNumber leftChildBlkno;
	BlockNumber updateBlkno;

	ItemPointerData rightbound; /* used only in posting tree */
	/* follows: list of tuple or ItemPointerData or PostingItem */
} ginxlogSplit;

#define XLOG_GIN_VACUUM_PAGE	0x40

typedef struct ginxlogVacuumPage
{
	RelFileNode node;
	BlockNumber blkno;
	OffsetNumber nitem;
	/* follows content of page */
} ginxlogVacuumPage;

#define XLOG_GIN_DELETE_PAGE	0x50

typedef struct ginxlogDeletePage
{
	RelFileNode node;
	BlockNumber blkno;
	BlockNumber parentBlkno;
	OffsetNumber parentOffset;
	BlockNumber leftBlkno;
	BlockNumber rightLink;
} ginxlogDeletePage;

/* ginutil.c */
extern Datum ginoptions(PG_FUNCTION_ARGS);
extern void initGinState(GinState *state, Relation index);
extern Buffer GinNewBuffer(Relation index);
extern void GinInitBuffer(Buffer b, uint32 f);
extern void GinInitPage(Page page, uint32 f, Size pageSize);
extern int	compareEntries(GinState *ginstate, Datum a, Datum b);
extern Datum *extractEntriesS(GinState *ginstate, Datum value,
				int32 *nentries, bool *needUnique);
extern Datum *extractEntriesSU(GinState *ginstate, Datum value, int32 *nentries);
extern Page GinPageGetCopyPage(Page page);

/* gininsert.c */
extern Datum ginbuild(PG_FUNCTION_ARGS);
extern Datum gininsert(PG_FUNCTION_ARGS);

/* ginxlog.c */
extern void gin_redo(XLogRecPtr lsn, XLogRecord *record);
extern void gin_desc(StringInfo buf, uint8 xl_info, char *rec);
extern void gin_xlog_startup(void);
extern void gin_xlog_cleanup(void);
extern bool gin_safe_restartpoint(void);

/* ginbtree.c */

typedef struct GinBtreeStack
{
	BlockNumber blkno;
	Buffer		buffer;
	OffsetNumber off;
	/* predictNumber contains prediction number of pages on current level */
	uint32		predictNumber;
	struct GinBtreeStack *parent;
} GinBtreeStack;

typedef struct GinBtreeData *GinBtree;

typedef struct GinBtreeData
{
	/* search methods */
	BlockNumber (*findChildPage) (GinBtree, GinBtreeStack *);
	bool		(*isMoveRight) (GinBtree, Page);
	bool		(*findItem) (GinBtree, GinBtreeStack *);

	/* insert methods */
	OffsetNumber (*findChildPtr) (GinBtree, Page, BlockNumber, OffsetNumber);
	BlockNumber (*getLeftMostPage) (GinBtree, Page);
	bool		(*isEnoughSpace) (GinBtree, Buffer, OffsetNumber);
	void		(*placeToPage) (GinBtree, Buffer, OffsetNumber, XLogRecData **);
	Page		(*splitPage) (GinBtree, Buffer, Buffer, OffsetNumber, XLogRecData **);
	void		(*fillRoot) (GinBtree, Buffer, Buffer, Buffer);

	bool		searchMode;

	Relation	index;
	GinState   *ginstate;
	bool		fullScan;
	bool		isBuild;

	BlockNumber rightblkno;

	/* Entry options */
	Datum		entryValue;
	IndexTuple	entry;
	bool		isDelete;

	/* Data (posting tree) option */
	ItemPointerData *items;
	uint32		nitem;
	uint32		curitem;

	PostingItem pitem;
} GinBtreeData;

extern GinBtreeStack *ginPrepareFindLeafPage(GinBtree btree, BlockNumber blkno);
extern GinBtreeStack *ginFindLeafPage(GinBtree btree, GinBtreeStack *stack);
extern void freeGinBtreeStack(GinBtreeStack *stack);
extern void ginInsertValue(GinBtree btree, GinBtreeStack *stack);
extern void findParents(GinBtree btree, GinBtreeStack *stack, BlockNumber rootBlkno);

/* ginentrypage.c */
extern IndexTuple GinFormTuple(GinState *ginstate, Datum key, ItemPointerData *ipd, uint32 nipd);
extern Datum ginGetHighKey(GinState *ginstate, Page page);
extern void prepareEntryScan(GinBtree btree, Relation index, Datum value, GinState *ginstate);
extern void entryFillRoot(GinBtree btree, Buffer root, Buffer lbuf, Buffer rbuf);
extern IndexTuple ginPageGetLinkItup(Buffer buf);

/* gindatapage.c */
extern int	compareItemPointers(ItemPointer a, ItemPointer b);
extern void
MergeItemPointers(
				  ItemPointerData *dst,
				  ItemPointerData *a, uint32 na,
				  ItemPointerData *b, uint32 nb
);

extern void GinDataPageAddItem(Page page, void *data, OffsetNumber offset);
extern void PageDeletePostingItem(Page page, OffsetNumber offset);

typedef struct
{
	GinBtreeData btree;
	GinBtreeStack *stack;
} GinPostingTreeScan;

extern GinPostingTreeScan *prepareScanPostingTree(Relation index,
					   BlockNumber rootBlkno, bool searchMode);
extern void insertItemPointer(GinPostingTreeScan *gdi,
				  ItemPointerData *items, uint32 nitem);
extern Buffer scanBeginPostingTree(GinPostingTreeScan *gdi);
extern void dataFillRoot(GinBtree btree, Buffer root, Buffer lbuf, Buffer rbuf);
extern void prepareDataScan(GinBtree btree, Relation index);

/* ginscan.c */

typedef struct GinScanEntryData *GinScanEntry;

typedef struct GinScanEntryData
{
	/* link to the equals entry in current scan key */
	GinScanEntry master;

	/*
	 * link to values reported to consistentFn, points to
	 * GinScanKey->entryRes[i]
	 */
	bool	   *pval;

	/* entry, got from extractQueryFn */
	Datum		entry;

	/* current ItemPointer to heap, its offset in buffer and buffer */
	ItemPointerData curItem;
	OffsetNumber offset;
	Buffer		buffer;

	/* in case of Posing list */
	ItemPointerData *list;
	uint32		nlist;

	bool		isFinished;
	bool		reduceResult;
	uint32		predictNumberResult;
} GinScanEntryData;

typedef struct GinScanKeyData
{
	/* Number of entries in query (got by extractQueryFn) */
	uint32		nentries;

	/* array of ItemPointer result, reported to consistentFn */
	bool	   *entryRes;

	/* array of scans per entry */
	GinScanEntry scanEntry;

	/* for calling consistentFn(GinScanKey->entryRes, strategy, query) */
	StrategyNumber strategy;
	Datum		query;

	ItemPointerData curItem;
	bool		firstCall;
	bool		isFinished;
} GinScanKeyData;

typedef GinScanKeyData *GinScanKey;

typedef struct GinScanOpaqueData
{
	MemoryContext tempCtx;
	GinState	ginstate;

	GinScanKey	keys;
	uint32		nkeys;
	bool		isVoidRes;		/* true if ginstate.extractQueryFn guarantees
								 * that nothing will be found */

	GinScanKey	markPos;
} GinScanOpaqueData;

typedef GinScanOpaqueData *GinScanOpaque;

extern Datum ginbeginscan(PG_FUNCTION_ARGS);
extern Datum ginendscan(PG_FUNCTION_ARGS);
extern Datum ginrescan(PG_FUNCTION_ARGS);
extern Datum ginmarkpos(PG_FUNCTION_ARGS);
extern Datum ginrestrpos(PG_FUNCTION_ARGS);
extern void newScanKey(IndexScanDesc scan);

/* ginget.c */
extern PGDLLIMPORT int GinFuzzySearchLimit;

#define ItemPointerSetMax(p)	ItemPointerSet( (p), (BlockNumber)0xffffffff, (OffsetNumber)0xffff )
#define ItemPointerIsMax(p) ( ItemPointerGetBlockNumber(p) == (BlockNumber)0xffffffff && ItemPointerGetOffsetNumber(p) == (OffsetNumber)0xffff )
#define ItemPointerSetMin(p)	ItemPointerSet( (p), (BlockNumber)0, (OffsetNumber)0)
#define ItemPointerIsMin(p) ( ItemPointerGetBlockNumber(p) == (BlockNumber)0 && ItemPointerGetOffsetNumber(p) == (OffsetNumber)0 )

extern Datum gingetmulti(PG_FUNCTION_ARGS);
extern Datum gingettuple(PG_FUNCTION_ARGS);

/* ginvacuum.c */
extern Datum ginbulkdelete(PG_FUNCTION_ARGS);
extern Datum ginvacuumcleanup(PG_FUNCTION_ARGS);

/* ginarrayproc.c */
extern Datum ginarrayextract(PG_FUNCTION_ARGS);
extern Datum ginqueryarrayextract(PG_FUNCTION_ARGS);
extern Datum ginarrayconsistent(PG_FUNCTION_ARGS);

/* ginbulk.c */
typedef struct EntryAccumulator
{
	Datum		value;
	uint32		length;
	uint32		number;
	ItemPointerData *list;
	bool		shouldSort;
	struct EntryAccumulator *left;
	struct EntryAccumulator *right;
} EntryAccumulator;

typedef struct
{
	GinState   *ginstate;
	EntryAccumulator *entries;
	uint32		maxdepth;
	EntryAccumulator **stack;
	uint32		stackpos;
	long		allocatedMemory;

	uint32		length;
	EntryAccumulator *entryallocator;
} BuildAccumulator;

extern void ginInitBA(BuildAccumulator *accum);
extern void ginInsertRecordBA(BuildAccumulator *accum,
				  ItemPointer heapptr, Datum *entries, int32 nentry);
extern ItemPointerData *ginGetEntry(BuildAccumulator *accum, Datum *entry, uint32 *n);

#endif
