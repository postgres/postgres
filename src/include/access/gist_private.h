/*-------------------------------------------------------------------------
 *
 * gist_private.h
 *	  private declarations for GiST -- declarations related to the
 *	  internal implementation of GiST, not the public API
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/gist_private.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef GIST_PRIVATE_H
#define GIST_PRIVATE_H

#include "access/amapi.h"
#include "access/gist.h"
#include "access/itup.h"
#include "lib/pairingheap.h"
#include "storage/bufmgr.h"
#include "storage/buffile.h"
#include "utils/hsearch.h"
#include "access/genam.h"

/*
 * Maximum number of "halves" a page can be split into in one operation.
 * Typically a split produces 2 halves, but can be more if keys have very
 * different lengths, or when inserting multiple keys in one operation (as
 * when inserting downlinks to an internal node).  There is no theoretical
 * limit on this, but in practice if you get more than a handful page halves
 * in one split, there's something wrong with the opclass implementation.
 * GIST_MAX_SPLIT_PAGES is an arbitrary limit on that, used to size some
 * local arrays used during split.  Note that there is also a limit on the
 * number of buffers that can be held locked at a time, MAX_SIMUL_LWLOCKS,
 * so if you raise this higher than that limit, you'll just get a different
 * error.
 */
#define GIST_MAX_SPLIT_PAGES		75

/* Buffer lock modes */
#define GIST_SHARE	BUFFER_LOCK_SHARE
#define GIST_EXCLUSIVE	BUFFER_LOCK_EXCLUSIVE
#define GIST_UNLOCK BUFFER_LOCK_UNLOCK

typedef struct
{
	BlockNumber prev;
	uint32		freespace;
	char		tupledata[FLEXIBLE_ARRAY_MEMBER];
} GISTNodeBufferPage;

#define BUFFER_PAGE_DATA_OFFSET MAXALIGN(offsetof(GISTNodeBufferPage, tupledata))
/* Returns free space in node buffer page */
#define PAGE_FREE_SPACE(nbp) (nbp->freespace)
/* Checks if node buffer page is empty */
#define PAGE_IS_EMPTY(nbp) (nbp->freespace == BLCKSZ - BUFFER_PAGE_DATA_OFFSET)
/* Checks if node buffers page don't contain sufficient space for index tuple */
#define PAGE_NO_SPACE(nbp, itup) (PAGE_FREE_SPACE(nbp) < \
										MAXALIGN(IndexTupleSize(itup)))

/*
 * GISTSTATE: information needed for any GiST index operation
 *
 * This struct retains call info for the index's opclass-specific support
 * functions (per index column), plus the index's tuple descriptor.
 *
 * scanCxt holds the GISTSTATE itself as well as any data that lives for the
 * lifetime of the index operation.  We pass this to the support functions
 * via fn_mcxt, so that they can store scan-lifespan data in it.  The
 * functions are invoked in tempCxt, which is typically short-lifespan
 * (that is, it's reset after each tuple).  However, tempCxt can be the same
 * as scanCxt if we're not bothering with per-tuple context resets.
 */
typedef struct GISTSTATE
{
	MemoryContext scanCxt;		/* context for scan-lifespan data */
	MemoryContext tempCxt;		/* short-term context for calling functions */

	TupleDesc	leafTupdesc;	/* index's tuple descriptor */
	TupleDesc	nonLeafTupdesc; /* truncated tuple descriptor for non-leaf
								 * pages */
	TupleDesc	fetchTupdesc;	/* tuple descriptor for tuples returned in an
								 * index-only scan */

	FmgrInfo	consistentFn[INDEX_MAX_KEYS];
	FmgrInfo	unionFn[INDEX_MAX_KEYS];
	FmgrInfo	compressFn[INDEX_MAX_KEYS];
	FmgrInfo	decompressFn[INDEX_MAX_KEYS];
	FmgrInfo	penaltyFn[INDEX_MAX_KEYS];
	FmgrInfo	picksplitFn[INDEX_MAX_KEYS];
	FmgrInfo	equalFn[INDEX_MAX_KEYS];
	FmgrInfo	distanceFn[INDEX_MAX_KEYS];
	FmgrInfo	fetchFn[INDEX_MAX_KEYS];

	/* Collations to pass to the support functions */
	Oid			supportCollation[INDEX_MAX_KEYS];
} GISTSTATE;


/*
 * During a GiST index search, we must maintain a queue of unvisited items,
 * which can be either individual heap tuples or whole index pages.  If it
 * is an ordered search, the unvisited items should be visited in distance
 * order.  Unvisited items at the same distance should be visited in
 * depth-first order, that is heap items first, then lower index pages, then
 * upper index pages; this rule avoids doing extra work during a search that
 * ends early due to LIMIT.
 *
 * To perform an ordered search, we use a pairing heap to manage the
 * distance-order queue.  In a non-ordered search (no order-by operators),
 * we use it to return heap tuples before unvisited index pages, to
 * ensure depth-first order, but all entries are otherwise considered
 * equal.
 */

/* Individual heap tuple to be visited */
typedef struct GISTSearchHeapItem
{
	ItemPointerData heapPtr;
	bool		recheck;		/* T if quals must be rechecked */
	bool		recheckDistances;	/* T if distances must be rechecked */
	HeapTuple	recontup;		/* data reconstructed from the index, used in
								 * index-only scans */
	OffsetNumber offnum;		/* track offset in page to mark tuple as
								 * LP_DEAD */
} GISTSearchHeapItem;

/* Unvisited item, either index page or heap tuple */
typedef struct GISTSearchItem
{
	pairingheap_node phNode;
	BlockNumber blkno;			/* index page number, or InvalidBlockNumber */
	union
	{
		GistNSN		parentlsn;	/* parent page's LSN, if index page */
		/* we must store parentlsn to detect whether a split occurred */
		GISTSearchHeapItem heap;	/* heap info, if heap tuple */
	}			data;

	/* numberOfOrderBys entries */
	IndexOrderByDistance distances[FLEXIBLE_ARRAY_MEMBER];
} GISTSearchItem;

#define GISTSearchItemIsHeap(item)	((item).blkno == InvalidBlockNumber)

#define SizeOfGISTSearchItem(n_distances) \
	(offsetof(GISTSearchItem, distances) + \
	 sizeof(IndexOrderByDistance) * (n_distances))

/*
 * GISTScanOpaqueData: private state for a scan of a GiST index
 */
typedef struct GISTScanOpaqueData
{
	GISTSTATE  *giststate;		/* index information, see above */
	Oid		   *orderByTypes;	/* datatypes of ORDER BY expressions */

	pairingheap *queue;			/* queue of unvisited items */
	MemoryContext queueCxt;		/* context holding the queue */
	bool		qual_ok;		/* false if qual can never be satisfied */
	bool		firstCall;		/* true until first gistgettuple call */

	/* pre-allocated workspace arrays */
	IndexOrderByDistance *distances;	/* output area for gistindex_keytest */

	/* info about killed items if any (killedItems is NULL if never used) */
	OffsetNumber *killedItems;	/* offset numbers of killed items */
	int			numKilled;		/* number of currently stored items */
	BlockNumber curBlkno;		/* current number of block */
	GistNSN		curPageLSN;		/* pos in the WAL stream when page was read */

	/* In a non-ordered search, returnable heap items are stored here: */
	GISTSearchHeapItem pageData[BLCKSZ / sizeof(IndexTupleData)];
	OffsetNumber nPageData;		/* number of valid items in array */
	OffsetNumber curPageData;	/* next item to return */
	MemoryContext pageDataCxt;	/* context holding the fetched tuples, for
								 * index-only scans */
} GISTScanOpaqueData;

typedef GISTScanOpaqueData *GISTScanOpaque;

/* despite the name, gistxlogPage is not part of any xlog record */
typedef struct gistxlogPage
{
	BlockNumber blkno;
	int			num;			/* number of index tuples following */
} gistxlogPage;

/* SplitPageLayout - gistSplit function result */
typedef struct SplitPageLayout
{
	gistxlogPage block;
	IndexTupleData *list;
	int			lenlist;
	IndexTuple	itup;			/* union key for page */
	Page		page;			/* to operate */
	Buffer		buffer;			/* to write after all proceed */

	struct SplitPageLayout *next;
} SplitPageLayout;

/*
 * GISTInsertStack used for locking buffers and transfer arguments during
 * insertion
 */
typedef struct GISTInsertStack
{
	/* current page */
	BlockNumber blkno;
	Buffer		buffer;
	Page		page;

	/*
	 * log sequence number from page->lsn to recognize page update and compare
	 * it with page's nsn to recognize page split
	 */
	GistNSN		lsn;

	/*
	 * If set, we split the page while descending the tree to find an
	 * insertion target. It means that we need to retry from the parent,
	 * because the downlink of this page might no longer cover the new key.
	 */
	bool		retry_from_parent;

	/* offset of the downlink in the parent page, that points to this page */
	OffsetNumber downlinkoffnum;

	/* pointer to parent */
	struct GISTInsertStack *parent;
} GISTInsertStack;

/* Working state and results for multi-column split logic in gistsplit.c */
typedef struct GistSplitVector
{
	GIST_SPLITVEC splitVector;	/* passed to/from user PickSplit method */

	Datum		spl_lattr[INDEX_MAX_KEYS];	/* Union of subkeys in
											 * splitVector.spl_left */
	bool		spl_lisnull[INDEX_MAX_KEYS];

	Datum		spl_rattr[INDEX_MAX_KEYS];	/* Union of subkeys in
											 * splitVector.spl_right */
	bool		spl_risnull[INDEX_MAX_KEYS];

	bool	   *spl_dontcare;	/* flags tuples which could go to either side
								 * of the split for zero penalty */
} GistSplitVector;

typedef struct
{
	Relation	r;
	Relation	heapRel;
	Size		freespace;		/* free space to be left */
	bool		is_build;

	GISTInsertStack *stack;
} GISTInsertState;

/* root page of a gist index */
#define GIST_ROOT_BLKNO				0

/*
 * Before PostgreSQL 9.1, we used to rely on so-called "invalid tuples" on
 * inner pages to finish crash recovery of incomplete page splits. If a crash
 * happened in the middle of a page split, so that the downlink pointers were
 * not yet inserted, crash recovery inserted a special downlink pointer. The
 * semantics of an invalid tuple was that it if you encounter one in a scan,
 * it must always be followed, because we don't know if the tuples on the
 * child page match or not.
 *
 * We no longer create such invalid tuples, we now mark the left-half of such
 * an incomplete split with the F_FOLLOW_RIGHT flag instead, and finish the
 * split properly the next time we need to insert on that page. To retain
 * on-disk compatibility for the sake of pg_upgrade, we still store 0xffff as
 * the offset number of all inner tuples. If we encounter any invalid tuples
 * with 0xfffe during insertion, we throw an error, though scans still handle
 * them. You should only encounter invalid tuples if you pg_upgrade a pre-9.1
 * gist index which already has invalid tuples in it because of a crash. That
 * should be rare, and you are recommended to REINDEX anyway if you have any
 * invalid tuples in an index, so throwing an error is as far as we go with
 * supporting that.
 */
#define TUPLE_IS_VALID		0xffff
#define TUPLE_IS_INVALID	0xfffe

#define  GistTupleIsInvalid(itup)	( ItemPointerGetOffsetNumber( &((itup)->t_tid) ) == TUPLE_IS_INVALID )
#define  GistTupleSetValid(itup)	ItemPointerSetOffsetNumber( &((itup)->t_tid), TUPLE_IS_VALID )




/*
 * A buffer attached to an internal node, used when building an index in
 * buffering mode.
 */
typedef struct
{
	BlockNumber nodeBlocknum;	/* index block # this buffer is for */
	int32		blocksCount;	/* current # of blocks occupied by buffer */

	BlockNumber pageBlocknum;	/* temporary file block # */
	GISTNodeBufferPage *pageBuffer; /* in-memory buffer page */

	/* is this buffer queued for emptying? */
	bool		queuedForEmptying;

	/* is this a temporary copy, not in the hash table? */
	bool		isTemp;

	int			level;			/* 0 == leaf */
} GISTNodeBuffer;

/*
 * Does specified level have buffers? (Beware of multiple evaluation of
 * arguments.)
 */
#define LEVEL_HAS_BUFFERS(nlevel, gfbb) \
	((nlevel) != 0 && (nlevel) % (gfbb)->levelStep == 0 && \
	 (nlevel) != (gfbb)->rootlevel)

/* Is specified buffer at least half-filled (should be queued for emptying)? */
#define BUFFER_HALF_FILLED(nodeBuffer, gfbb) \
	((nodeBuffer)->blocksCount > (gfbb)->pagesPerBuffer / 2)

/*
 * Is specified buffer full? Our buffers can actually grow indefinitely,
 * beyond the "maximum" size, so this just means whether the buffer has grown
 * beyond the nominal maximum size.
 */
#define BUFFER_OVERFLOWED(nodeBuffer, gfbb) \
	((nodeBuffer)->blocksCount > (gfbb)->pagesPerBuffer)

/*
 * Data structure with general information about build buffers.
 */
typedef struct GISTBuildBuffers
{
	/* Persistent memory context for the buffers and metadata. */
	MemoryContext context;

	BufFile    *pfile;			/* Temporary file to store buffers in */
	long		nFileBlocks;	/* Current size of the temporary file */

	/*
	 * resizable array of free blocks.
	 */
	long	   *freeBlocks;
	int			nFreeBlocks;	/* # of currently free blocks in the array */
	int			freeBlocksLen;	/* current allocated length of the array */

	/* Hash for buffers by block number */
	HTAB	   *nodeBuffersTab;

	/* List of buffers scheduled for emptying */
	List	   *bufferEmptyingQueue;

	/*
	 * Parameters to the buffering build algorithm. levelStep determines which
	 * levels in the tree have buffers, and pagesPerBuffer determines how
	 * large each buffer is.
	 */
	int			levelStep;
	int			pagesPerBuffer;

	/* Array of lists of buffers on each level, for final emptying */
	List	  **buffersOnLevels;
	int			buffersOnLevelsLen;

	/*
	 * Dynamically-sized array of buffers that currently have their last page
	 * loaded in main memory.
	 */
	GISTNodeBuffer **loadedBuffers;
	int			loadedBuffersCount; /* # of entries in loadedBuffers */
	int			loadedBuffersLen;	/* allocated size of loadedBuffers */

	/* Level of the current root node (= height of the index tree - 1) */
	int			rootlevel;
} GISTBuildBuffers;

/* GiSTOptions->buffering_mode values */
typedef enum GistOptBufferingMode
{
	GIST_OPTION_BUFFERING_AUTO,
	GIST_OPTION_BUFFERING_ON,
	GIST_OPTION_BUFFERING_OFF,
} GistOptBufferingMode;

/*
 * Storage type for GiST's reloptions
 */
typedef struct GiSTOptions
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int			fillfactor;		/* page fill factor in percent (0..100) */
	GistOptBufferingMode buffering_mode;	/* buffering build mode */
} GiSTOptions;

/* gist.c */
extern void gistbuildempty(Relation index);
extern bool gistinsert(Relation r, Datum *values, bool *isnull,
					   ItemPointer ht_ctid, Relation heapRel,
					   IndexUniqueCheck checkUnique,
					   bool indexUnchanged,
					   struct IndexInfo *indexInfo);
extern MemoryContext createTempGistContext(void);
extern GISTSTATE *initGISTstate(Relation index);
extern void freeGISTstate(GISTSTATE *giststate);
extern void gistdoinsert(Relation r,
						 IndexTuple itup,
						 Size freespace,
						 GISTSTATE *giststate,
						 Relation heapRel,
						 bool is_build);

/* A List of these is returned from gistplacetopage() in *splitinfo */
typedef struct
{
	Buffer		buf;			/* the split page "half" */
	IndexTuple	downlink;		/* downlink for this half. */
} GISTPageSplitInfo;

extern bool gistplacetopage(Relation rel, Size freespace, GISTSTATE *giststate,
							Buffer buffer,
							IndexTuple *itup, int ntup,
							OffsetNumber oldoffnum, BlockNumber *newblkno,
							Buffer leftchildbuf,
							List **splitinfo,
							bool markfollowright,
							Relation heapRel,
							bool is_build);

extern SplitPageLayout *gistSplit(Relation r, Page page, IndexTuple *itup,
								  int len, GISTSTATE *giststate);

/* gistxlog.c */
extern XLogRecPtr gistXLogPageDelete(Buffer buffer,
									 FullTransactionId xid, Buffer parentBuffer,
									 OffsetNumber downlinkOffset);

extern void gistXLogPageReuse(Relation rel, Relation heaprel, BlockNumber blkno,
							  FullTransactionId deleteXid);

extern XLogRecPtr gistXLogUpdate(Buffer buffer,
								 OffsetNumber *todelete, int ntodelete,
								 IndexTuple *itup, int ituplen,
								 Buffer leftchildbuf);

extern XLogRecPtr gistXLogDelete(Buffer buffer, OffsetNumber *todelete,
								 int ntodelete, TransactionId snapshotConflictHorizon,
								 Relation heaprel);

extern XLogRecPtr gistXLogSplit(bool page_is_leaf,
								SplitPageLayout *dist,
								BlockNumber origrlink, GistNSN orignsn,
								Buffer leftchildbuf, bool markfollowright);

extern XLogRecPtr gistXLogAssignLSN(void);

/* gistget.c */
extern bool gistgettuple(IndexScanDesc scan, ScanDirection dir);
extern int64 gistgetbitmap(IndexScanDesc scan, TIDBitmap *tbm);
extern bool gistcanreturn(Relation index, int attno);

/* gistvalidate.c */
extern bool gistvalidate(Oid opclassoid);
extern void gistadjustmembers(Oid opfamilyoid,
							  Oid opclassoid,
							  List *operators,
							  List *functions);

/* gistutil.c */

#define GiSTPageSize   \
	( BLCKSZ - SizeOfPageHeaderData - MAXALIGN(sizeof(GISTPageOpaqueData)) )

#define GIST_MIN_FILLFACTOR			10
#define GIST_DEFAULT_FILLFACTOR		90

extern bytea *gistoptions(Datum reloptions, bool validate);
extern bool gistproperty(Oid index_oid, int attno,
						 IndexAMProperty prop, const char *propname,
						 bool *res, bool *isnull);
extern bool gistfitpage(IndexTuple *itvec, int len);
extern bool gistnospace(Page page, IndexTuple *itvec, int len, OffsetNumber todelete, Size freespace);
extern void gistcheckpage(Relation rel, Buffer buf);
extern Buffer gistNewBuffer(Relation r, Relation heaprel);
extern bool gistPageRecyclable(Page page);
extern void gistfillbuffer(Page page, IndexTuple *itup, int len,
						   OffsetNumber off);
extern IndexTuple *gistextractpage(Page page, int *len /* out */ );
extern IndexTuple *gistjoinvector(IndexTuple *itvec, int *len,
								  IndexTuple *additvec, int addlen);
extern IndexTupleData *gistfillitupvec(IndexTuple *vec, int veclen, int *memlen);

extern IndexTuple gistunion(Relation r, IndexTuple *itvec,
							int len, GISTSTATE *giststate);
extern IndexTuple gistgetadjusted(Relation r,
								  IndexTuple oldtup,
								  IndexTuple addtup,
								  GISTSTATE *giststate);
extern IndexTuple gistFormTuple(GISTSTATE *giststate,
								Relation r, const Datum *attdata, const bool *isnull, bool isleaf);
extern void gistCompressValues(GISTSTATE *giststate, Relation r,
							   const Datum *attdata, const bool *isnull, bool isleaf, Datum *compatt);

extern OffsetNumber gistchoose(Relation r, Page p,
							   IndexTuple it,
							   GISTSTATE *giststate);

extern void GISTInitBuffer(Buffer b, uint32 f);
extern void gistinitpage(Page page, uint32 f);
extern void gistdentryinit(GISTSTATE *giststate, int nkey, GISTENTRY *e,
						   Datum k, Relation r, Page pg, OffsetNumber o,
						   bool l, bool isNull);

extern float gistpenalty(GISTSTATE *giststate, int attno,
						 GISTENTRY *orig, bool isNullOrig,
						 GISTENTRY *add, bool isNullAdd);
extern void gistMakeUnionItVec(GISTSTATE *giststate, IndexTuple *itvec, int len,
							   Datum *attr, bool *isnull);
extern bool gistKeyIsEQ(GISTSTATE *giststate, int attno, Datum a, Datum b);
extern void gistDeCompressAtt(GISTSTATE *giststate, Relation r, IndexTuple tuple, Page p,
							  OffsetNumber o, GISTENTRY *attdata, bool *isnull);
extern HeapTuple gistFetchTuple(GISTSTATE *giststate, Relation r,
								IndexTuple tuple);
extern void gistMakeUnionKey(GISTSTATE *giststate, int attno,
							 GISTENTRY *entry1, bool isnull1,
							 GISTENTRY *entry2, bool isnull2,
							 Datum *dst, bool *dstisnull);

extern XLogRecPtr gistGetFakeLSN(Relation rel);

/* gistvacuum.c */
extern IndexBulkDeleteResult *gistbulkdelete(IndexVacuumInfo *info,
											 IndexBulkDeleteResult *stats,
											 IndexBulkDeleteCallback callback,
											 void *callback_state);
extern IndexBulkDeleteResult *gistvacuumcleanup(IndexVacuumInfo *info,
												IndexBulkDeleteResult *stats);

/* gistsplit.c */
extern void gistSplitByKey(Relation r, Page page, IndexTuple *itup,
						   int len, GISTSTATE *giststate,
						   GistSplitVector *v,
						   int attno);

/* gistbuild.c */
extern IndexBuildResult *gistbuild(Relation heap, Relation index,
								   struct IndexInfo *indexInfo);

/* gistbuildbuffers.c */
extern GISTBuildBuffers *gistInitBuildBuffers(int pagesPerBuffer, int levelStep,
											  int maxLevel);
extern GISTNodeBuffer *gistGetNodeBuffer(GISTBuildBuffers *gfbb,
										 GISTSTATE *giststate,
										 BlockNumber nodeBlocknum, int level);
extern void gistPushItupToNodeBuffer(GISTBuildBuffers *gfbb,
									 GISTNodeBuffer *nodeBuffer, IndexTuple itup);
extern bool gistPopItupFromNodeBuffer(GISTBuildBuffers *gfbb,
									  GISTNodeBuffer *nodeBuffer, IndexTuple *itup);
extern void gistFreeBuildBuffers(GISTBuildBuffers *gfbb);
extern void gistRelocateBuildBuffersOnSplit(GISTBuildBuffers *gfbb,
											GISTSTATE *giststate, Relation r,
											int level, Buffer buffer,
											List *splitinfo);
extern void gistUnloadNodeBuffers(GISTBuildBuffers *gfbb);

#endif							/* GIST_PRIVATE_H */
