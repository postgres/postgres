/*--------------------------------------------------------------------------
 * gin_private.h
 *	  header file for postgres inverted index access method implementation.
 *
 *	Copyright (c) 2006-2025, PostgreSQL Global Development Group
 *
 *	src/include/access/gin_private.h
 *--------------------------------------------------------------------------
 */
#ifndef GIN_PRIVATE_H
#define GIN_PRIVATE_H

#include "access/amapi.h"
#include "access/gin.h"
#include "access/ginblock.h"
#include "access/itup.h"
#include "common/int.h"
#include "catalog/pg_am_d.h"
#include "fmgr.h"
#include "lib/rbtree.h"
#include "storage/bufmgr.h"

/*
 * Storage type for GIN's reloptions
 */
typedef struct GinOptions
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	bool		useFastUpdate;	/* use fast updates? */
	int			pendingListCleanupSize; /* maximum size of pending list */
} GinOptions;

#define GIN_DEFAULT_USE_FASTUPDATE	true
#define GinGetUseFastUpdate(relation) \
	(AssertMacro(relation->rd_rel->relkind == RELKIND_INDEX && \
				 relation->rd_rel->relam == GIN_AM_OID), \
	 (relation)->rd_options ? \
	 ((GinOptions *) (relation)->rd_options)->useFastUpdate : GIN_DEFAULT_USE_FASTUPDATE)
#define GinGetPendingListCleanupSize(relation) \
	(AssertMacro(relation->rd_rel->relkind == RELKIND_INDEX && \
				 relation->rd_rel->relam == GIN_AM_OID), \
	 (relation)->rd_options && \
	 ((GinOptions *) (relation)->rd_options)->pendingListCleanupSize != -1 ? \
	 ((GinOptions *) (relation)->rd_options)->pendingListCleanupSize : \
	 gin_pending_list_limit)


/* Macros for buffer lock/unlock operations */
#define GIN_UNLOCK	BUFFER_LOCK_UNLOCK
#define GIN_SHARE	BUFFER_LOCK_SHARE
#define GIN_EXCLUSIVE  BUFFER_LOCK_EXCLUSIVE


/*
 * GinState: working data structure describing the index being worked on
 */
typedef struct GinState
{
	Relation	index;
	bool		oneCol;			/* true if single-column index */

	/*
	 * origTupdesc is the nominal tuple descriptor of the index, ie, the i'th
	 * attribute shows the key type (not the input data type!) of the i'th
	 * index column.  In a single-column index this describes the actual leaf
	 * index tuples.  In a multi-column index, the actual leaf tuples contain
	 * a smallint column number followed by a key datum of the appropriate
	 * type for that column.  We set up tupdesc[i] to describe the actual
	 * rowtype of the index tuples for the i'th column, ie, (int2, keytype).
	 * Note that in any case, leaf tuples contain more data than is known to
	 * the TupleDesc; see access/gin/README for details.
	 */
	TupleDesc	origTupdesc;
	TupleDesc	tupdesc[INDEX_MAX_KEYS];

	/*
	 * Per-index-column opclass support functions
	 */
	FmgrInfo	compareFn[INDEX_MAX_KEYS];
	FmgrInfo	extractValueFn[INDEX_MAX_KEYS];
	FmgrInfo	extractQueryFn[INDEX_MAX_KEYS];
	FmgrInfo	consistentFn[INDEX_MAX_KEYS];
	FmgrInfo	triConsistentFn[INDEX_MAX_KEYS];
	FmgrInfo	comparePartialFn[INDEX_MAX_KEYS];	/* optional method */
	/* canPartialMatch[i] is true if comparePartialFn[i] is valid */
	bool		canPartialMatch[INDEX_MAX_KEYS];
	/* Collations to pass to the support functions */
	Oid			supportCollation[INDEX_MAX_KEYS];
} GinState;


/* ginutil.c */
extern bytea *ginoptions(Datum reloptions, bool validate);
extern void initGinState(GinState *state, Relation index);
extern Buffer GinNewBuffer(Relation index);
extern void GinInitBuffer(Buffer b, uint32 f);
extern void GinInitPage(Page page, uint32 f, Size pageSize);
extern void GinInitMetabuffer(Buffer b);
extern int	ginCompareEntries(GinState *ginstate, OffsetNumber attnum,
							  Datum a, GinNullCategory categorya,
							  Datum b, GinNullCategory categoryb);
extern int	ginCompareAttEntries(GinState *ginstate,
								 OffsetNumber attnuma, Datum a, GinNullCategory categorya,
								 OffsetNumber attnumb, Datum b, GinNullCategory categoryb);
extern Datum *ginExtractEntries(GinState *ginstate, OffsetNumber attnum,
								Datum value, bool isNull,
								int32 *nentries, GinNullCategory **categories);

extern OffsetNumber gintuple_get_attrnum(GinState *ginstate, IndexTuple tuple);
extern Datum gintuple_get_key(GinState *ginstate, IndexTuple tuple,
							  GinNullCategory *category);

/* gininsert.c */
extern IndexBuildResult *ginbuild(Relation heap, Relation index,
								  struct IndexInfo *indexInfo);
extern void ginbuildempty(Relation index);
extern bool gininsert(Relation index, Datum *values, bool *isnull,
					  ItemPointer ht_ctid, Relation heapRel,
					  IndexUniqueCheck checkUnique,
					  bool indexUnchanged,
					  struct IndexInfo *indexInfo);
extern void ginEntryInsert(GinState *ginstate,
						   OffsetNumber attnum, Datum key, GinNullCategory category,
						   ItemPointerData *items, uint32 nitem,
						   GinStatsData *buildStats);

/* ginbtree.c */

typedef struct GinBtreeStack
{
	BlockNumber blkno;
	Buffer		buffer;
	OffsetNumber off;
	ItemPointerData iptr;
	/* predictNumber contains predicted number of pages on current level */
	uint32		predictNumber;
	struct GinBtreeStack *parent;
} GinBtreeStack;

typedef struct GinBtreeData *GinBtree;

/* Return codes for GinBtreeData.beginPlaceToPage method */
typedef enum
{
	GPTP_NO_WORK,
	GPTP_INSERT,
	GPTP_SPLIT,
} GinPlaceToPageRC;

typedef struct GinBtreeData
{
	/* search methods */
	BlockNumber (*findChildPage) (GinBtree, GinBtreeStack *);
	BlockNumber (*getLeftMostChild) (GinBtree, Page);
	bool		(*isMoveRight) (GinBtree, Page);
	bool		(*findItem) (GinBtree, GinBtreeStack *);

	/* insert methods */
	OffsetNumber (*findChildPtr) (GinBtree, Page, BlockNumber, OffsetNumber);
	GinPlaceToPageRC (*beginPlaceToPage) (GinBtree, Buffer, GinBtreeStack *, void *, BlockNumber, void **, Page *, Page *);
	void		(*execPlaceToPage) (GinBtree, Buffer, GinBtreeStack *, void *, BlockNumber, void *);
	void	   *(*prepareDownlink) (GinBtree, Buffer);
	void		(*fillRoot) (GinBtree, Page, BlockNumber, Page, BlockNumber, Page);

	bool		isData;

	Relation	index;
	BlockNumber rootBlkno;
	GinState   *ginstate;		/* not valid in a data scan */
	bool		fullScan;
	bool		isBuild;

	/* Search key for Entry tree */
	OffsetNumber entryAttnum;
	Datum		entryKey;
	GinNullCategory entryCategory;

	/* Search key for data tree (posting tree) */
	ItemPointerData itemptr;
} GinBtreeData;

/* This represents a tuple to be inserted to entry tree. */
typedef struct
{
	IndexTuple	entry;			/* tuple to insert */
	bool		isDelete;		/* delete old tuple at same offset? */
} GinBtreeEntryInsertData;

/*
 * This represents an itempointer, or many itempointers, to be inserted to
 * a data (posting tree) leaf page
 */
typedef struct
{
	ItemPointerData *items;
	uint32		nitem;
	uint32		curitem;
} GinBtreeDataLeafInsertData;

/*
 * For internal data (posting tree) pages, the insertion payload is a
 * PostingItem
 */

extern GinBtreeStack *ginFindLeafPage(GinBtree btree, bool searchMode,
									  bool rootConflictCheck);
extern Buffer ginStepRight(Buffer buffer, Relation index, int lockmode);
extern void freeGinBtreeStack(GinBtreeStack *stack);
extern void ginInsertValue(GinBtree btree, GinBtreeStack *stack,
						   void *insertdata, GinStatsData *buildStats);

/* ginentrypage.c */
extern IndexTuple GinFormTuple(GinState *ginstate,
							   OffsetNumber attnum, Datum key, GinNullCategory category,
							   Pointer data, Size dataSize, int nipd, bool errorTooBig);
extern void ginPrepareEntryScan(GinBtree btree, OffsetNumber attnum,
								Datum key, GinNullCategory category,
								GinState *ginstate);
extern void ginEntryFillRoot(GinBtree btree, Page root, BlockNumber lblkno, Page lpage, BlockNumber rblkno, Page rpage);
extern ItemPointer ginReadTuple(GinState *ginstate, OffsetNumber attnum,
								IndexTuple itup, int *nitems);

/* gindatapage.c */
extern ItemPointer GinDataLeafPageGetItems(Page page, int *nitems, ItemPointerData advancePast);
extern int	GinDataLeafPageGetItemsToTbm(Page page, TIDBitmap *tbm);
extern BlockNumber createPostingTree(Relation index,
									 ItemPointerData *items, uint32 nitems,
									 GinStatsData *buildStats, Buffer entrybuffer);
extern void GinDataPageAddPostingItem(Page page, PostingItem *data, OffsetNumber offset);
extern void GinPageDeletePostingItem(Page page, OffsetNumber offset);
extern void ginInsertItemPointers(Relation index, BlockNumber rootBlkno,
								  ItemPointerData *items, uint32 nitem,
								  GinStatsData *buildStats);
extern GinBtreeStack *ginScanBeginPostingTree(GinBtree btree, Relation index, BlockNumber rootBlkno);
extern void ginDataFillRoot(GinBtree btree, Page root, BlockNumber lblkno, Page lpage, BlockNumber rblkno, Page rpage);

/*
 * This is declared in ginvacuum.c, but is passed between ginVacuumItemPointers
 * and ginVacuumPostingTreeLeaf and as an opaque struct, so we need a forward
 * declaration for it.
 */
typedef struct GinVacuumState GinVacuumState;

extern void ginVacuumPostingTreeLeaf(Relation indexrel, Buffer buffer,
									 GinVacuumState *gvs);

/* ginscan.c */

/*
 * GinScanKeyData describes a single GIN index qualifier expression.
 *
 * From each qual expression, we extract one or more specific index search
 * conditions, which are represented by GinScanEntryData.  It's quite
 * possible for identical search conditions to be requested by more than
 * one qual expression, in which case we merge such conditions to have just
 * one unique GinScanEntry --- this is particularly important for efficiency
 * when dealing with full-index-scan entries.  So there can be multiple
 * GinScanKeyData.scanEntry pointers to the same GinScanEntryData.
 *
 * In each GinScanKeyData, nentries is the true number of entries, while
 * nuserentries is the number that extractQueryFn returned (which is what
 * we report to consistentFn).  The "user" entries must come first.
 */
typedef struct GinScanKeyData *GinScanKey;

typedef struct GinScanEntryData *GinScanEntry;

typedef struct GinScanKeyData
{
	/* Real number of entries in scanEntry[] (always > 0) */
	uint32		nentries;
	/* Number of entries that extractQueryFn and consistentFn know about */
	uint32		nuserentries;

	/* array of GinScanEntry pointers, one per extracted search condition */
	GinScanEntry *scanEntry;

	/*
	 * At least one of the entries in requiredEntries must be present for a
	 * tuple to match the overall qual.
	 *
	 * additionalEntries contains entries that are needed by the consistent
	 * function to decide if an item matches, but are not sufficient to
	 * satisfy the qual without entries from requiredEntries.
	 */
	GinScanEntry *requiredEntries;
	int			nrequired;
	GinScanEntry *additionalEntries;
	int			nadditional;

	/* array of check flags, reported to consistentFn */
	GinTernaryValue *entryRes;
	bool		(*boolConsistentFn) (GinScanKey key);
	GinTernaryValue (*triConsistentFn) (GinScanKey key);
	FmgrInfo   *consistentFmgrInfo;
	FmgrInfo   *triConsistentFmgrInfo;
	Oid			collation;

	/* other data needed for calling consistentFn */
	Datum		query;
	/* NB: these three arrays have only nuserentries elements! */
	Datum	   *queryValues;
	GinNullCategory *queryCategories;
	Pointer    *extra_data;
	StrategyNumber strategy;
	int32		searchMode;
	OffsetNumber attnum;

	/*
	 * An excludeOnly scan key is not able to enumerate all matching tuples.
	 * That is, to be semantically correct on its own, it would need to have a
	 * GIN_CAT_EMPTY_QUERY scanEntry, but it doesn't.  Such a key can still be
	 * used to filter tuples returned by other scan keys, so we will get the
	 * right answers as long as there's at least one non-excludeOnly scan key
	 * for each index attribute considered by the search.  For efficiency
	 * reasons we don't want to have unnecessary GIN_CAT_EMPTY_QUERY entries,
	 * so we will convert an excludeOnly scan key to non-excludeOnly (by
	 * adding a GIN_CAT_EMPTY_QUERY scanEntry) only if there are no other
	 * non-excludeOnly scan keys.
	 */
	bool		excludeOnly;

	/*
	 * Match status data.  curItem is the TID most recently tested (could be a
	 * lossy-page pointer).  curItemMatches is true if it passes the
	 * consistentFn test; if so, recheckCurItem is the recheck flag.
	 * isFinished means that all the input entry streams are finished, so this
	 * key cannot succeed for any later TIDs.
	 */
	ItemPointerData curItem;
	bool		curItemMatches;
	bool		recheckCurItem;
	bool		isFinished;
}			GinScanKeyData;

typedef struct GinScanEntryData
{
	/* query key and other information from extractQueryFn */
	Datum		queryKey;
	GinNullCategory queryCategory;
	bool		isPartialMatch;
	Pointer		extra_data;
	StrategyNumber strategy;
	int32		searchMode;
	OffsetNumber attnum;

	/* Current page in posting tree */
	Buffer		buffer;

	/* current ItemPointer to heap */
	ItemPointerData curItem;

	/* for a partial-match or full-scan query, we accumulate all TIDs here */
	TIDBitmap  *matchBitmap;
	TBMPrivateIterator *matchIterator;
	TBMIterateResult *matchResult;

	/* used for Posting list and one page in Posting tree */
	ItemPointerData *list;
	int			nlist;
	OffsetNumber offset;

	bool		isFinished;
	bool		reduceResult;
	uint32		predictNumberResult;
	GinBtreeData btree;
}			GinScanEntryData;

typedef struct GinScanOpaqueData
{
	MemoryContext tempCtx;
	GinState	ginstate;

	GinScanKey	keys;			/* one per scan qualifier expr */
	uint32		nkeys;

	GinScanEntry *entries;		/* one per index search condition */
	uint32		totalentries;
	uint32		allocentries;	/* allocated length of entries[] */

	MemoryContext keyCtx;		/* used to hold key and entry data */

	bool		isVoidRes;		/* true if query is unsatisfiable */
} GinScanOpaqueData;

typedef GinScanOpaqueData *GinScanOpaque;

extern IndexScanDesc ginbeginscan(Relation rel, int nkeys, int norderbys);
extern void ginendscan(IndexScanDesc scan);
extern void ginrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
					  ScanKey orderbys, int norderbys);
extern void ginNewScanKey(IndexScanDesc scan);
extern void ginFreeScanKeys(GinScanOpaque so);

/* ginget.c */
extern int64 gingetbitmap(IndexScanDesc scan, TIDBitmap *tbm);

/* ginlogic.c */
extern void ginInitConsistentFunction(GinState *ginstate, GinScanKey key);

/* ginvacuum.c */
extern IndexBulkDeleteResult *ginbulkdelete(IndexVacuumInfo *info,
											IndexBulkDeleteResult *stats,
											IndexBulkDeleteCallback callback,
											void *callback_state);
extern IndexBulkDeleteResult *ginvacuumcleanup(IndexVacuumInfo *info,
											   IndexBulkDeleteResult *stats);
extern ItemPointer ginVacuumItemPointers(GinVacuumState *gvs,
										 ItemPointerData *items, int nitem, int *nremaining);

/* ginvalidate.c */
extern bool ginvalidate(Oid opclassoid);
extern void ginadjustmembers(Oid opfamilyoid,
							 Oid opclassoid,
							 List *operators,
							 List *functions);

/* ginbulk.c */
typedef struct GinEntryAccumulator
{
	RBTNode		rbtnode;
	Datum		key;
	GinNullCategory category;
	OffsetNumber attnum;
	bool		shouldSort;
	ItemPointerData *list;
	uint32		maxcount;		/* allocated size of list[] */
	uint32		count;			/* current number of list[] entries */
} GinEntryAccumulator;

typedef struct
{
	GinState   *ginstate;
	Size		allocatedMemory;
	GinEntryAccumulator *entryallocator;
	uint32		eas_used;
	RBTree	   *tree;
	RBTreeIterator tree_walk;
} BuildAccumulator;

extern void ginInitBA(BuildAccumulator *accum);
extern void ginInsertBAEntries(BuildAccumulator *accum,
							   ItemPointer heapptr, OffsetNumber attnum,
							   Datum *entries, GinNullCategory *categories,
							   int32 nentries);
extern void ginBeginBAScan(BuildAccumulator *accum);
extern ItemPointerData *ginGetBAEntry(BuildAccumulator *accum,
									  OffsetNumber *attnum, Datum *key, GinNullCategory *category,
									  uint32 *n);

/* ginfast.c */

typedef struct GinTupleCollector
{
	IndexTuple *tuples;
	uint32		ntuples;
	uint32		lentuples;
	uint32		sumsize;
} GinTupleCollector;

extern void ginHeapTupleFastInsert(GinState *ginstate,
								   GinTupleCollector *collector);
extern void ginHeapTupleFastCollect(GinState *ginstate,
									GinTupleCollector *collector,
									OffsetNumber attnum, Datum value, bool isNull,
									ItemPointer ht_ctid);
extern void ginInsertCleanup(GinState *ginstate, bool full_clean,
							 bool fill_fsm, bool forceCleanup, IndexBulkDeleteResult *stats);

/* ginpostinglist.c */

extern GinPostingList *ginCompressPostingList(const ItemPointer ipd, int nipd,
											  int maxsize, int *nwritten);
extern int	ginPostingListDecodeAllSegmentsToTbm(GinPostingList *ptr, int len, TIDBitmap *tbm);

extern ItemPointer ginPostingListDecodeAllSegments(GinPostingList *segment, int len,
												   int *ndecoded_out);
extern ItemPointer ginPostingListDecode(GinPostingList *plist, int *ndecoded_out);
extern ItemPointer ginMergeItemPointers(ItemPointerData *a, uint32 na,
										ItemPointerData *b, uint32 nb,
										int *nmerged);

/*
 * Merging the results of several gin scans compares item pointers a lot,
 * so we want this to be inlined.
 */
static inline int
ginCompareItemPointers(ItemPointer a, ItemPointer b)
{
	uint64		ia = (uint64) GinItemPointerGetBlockNumber(a) << 32 | GinItemPointerGetOffsetNumber(a);
	uint64		ib = (uint64) GinItemPointerGetBlockNumber(b) << 32 | GinItemPointerGetOffsetNumber(b);

	return pg_cmp_u64(ia, ib);
}

extern int	ginTraverseLock(Buffer buffer, bool searchMode);

#endif							/* GIN_PRIVATE_H */
