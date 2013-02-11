/*-------------------------------------------------------------------------
 *
 * gist_private.h
 *	  private declarations for GiST -- declarations related to the
 *	  internal implementation of GiST, not the public API
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/gist_private.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef GIST_PRIVATE_H
#define GIST_PRIVATE_H

#include "access/gist.h"
#include "access/itup.h"
#include "fmgr.h"
#include "storage/bufmgr.h"
#include "storage/buffile.h"
#include "utils/rbtree.h"
#include "utils/hsearch.h"

/* Buffer lock modes */
#define GIST_SHARE	BUFFER_LOCK_SHARE
#define GIST_EXCLUSIVE	BUFFER_LOCK_EXCLUSIVE
#define GIST_UNLOCK BUFFER_LOCK_UNLOCK

typedef struct
{
	BlockNumber prev;
	uint32		freespace;
	char		tupledata[1];
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

	TupleDesc	tupdesc;		/* index's tuple descriptor */

	FmgrInfo	consistentFn[INDEX_MAX_KEYS];
	FmgrInfo	unionFn[INDEX_MAX_KEYS];
	FmgrInfo	compressFn[INDEX_MAX_KEYS];
	FmgrInfo	decompressFn[INDEX_MAX_KEYS];
	FmgrInfo	penaltyFn[INDEX_MAX_KEYS];
	FmgrInfo	picksplitFn[INDEX_MAX_KEYS];
	FmgrInfo	equalFn[INDEX_MAX_KEYS];
	FmgrInfo	distanceFn[INDEX_MAX_KEYS];

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
 * To perform an ordered search, we use an RBTree to manage the distance-order
 * queue.  Each GISTSearchTreeItem stores all unvisited items of the same
 * distance; they are GISTSearchItems chained together via their next fields.
 *
 * In a non-ordered search (no order-by operators), the RBTree degenerates
 * to a single item, which we use as a queue of unvisited index pages only.
 * In this case matched heap items from the current index leaf page are
 * remembered in GISTScanOpaqueData.pageData[] and returned directly from
 * there, instead of building a separate GISTSearchItem for each one.
 */

/* Individual heap tuple to be visited */
typedef struct GISTSearchHeapItem
{
	ItemPointerData heapPtr;
	bool		recheck;		/* T if quals must be rechecked */
} GISTSearchHeapItem;

/* Unvisited item, either index page or heap tuple */
typedef struct GISTSearchItem
{
	struct GISTSearchItem *next;	/* list link */
	BlockNumber blkno;			/* index page number, or InvalidBlockNumber */
	union
	{
		GistNSN		parentlsn;	/* parent page's LSN, if index page */
		/* we must store parentlsn to detect whether a split occurred */
		GISTSearchHeapItem heap;	/* heap info, if heap tuple */
	}			data;
} GISTSearchItem;

#define GISTSearchItemIsHeap(item)	((item).blkno == InvalidBlockNumber)

/*
 * Within a GISTSearchTreeItem's chain, heap items always appear before
 * index-page items, since we want to visit heap items first.  lastHeap points
 * to the last heap item in the chain, or is NULL if there are none.
 */
typedef struct GISTSearchTreeItem
{
	RBNode		rbnode;			/* this is an RBTree item */
	GISTSearchItem *head;		/* first chain member */
	GISTSearchItem *lastHeap;	/* last heap-tuple member, if any */
	double		distances[1];	/* array with numberOfOrderBys entries */
} GISTSearchTreeItem;

#define GSTIHDRSZ offsetof(GISTSearchTreeItem, distances)

/*
 * GISTScanOpaqueData: private state for a scan of a GiST index
 */
typedef struct GISTScanOpaqueData
{
	GISTSTATE  *giststate;		/* index information, see above */
	RBTree	   *queue;			/* queue of unvisited items */
	MemoryContext queueCxt;		/* context holding the queue */
	bool		qual_ok;		/* false if qual can never be satisfied */
	bool		firstCall;		/* true until first gistgettuple call */

	GISTSearchTreeItem *curTreeItem;	/* current queue item, if any */

	/* pre-allocated workspace arrays */
	GISTSearchTreeItem *tmpTreeItem;	/* workspace to pass to rb_insert */
	double	   *distances;		/* output area for gistindex_keytest */

	/* In a non-ordered search, returnable heap items are stored here: */
	GISTSearchHeapItem pageData[BLCKSZ / sizeof(IndexTupleData)];
	OffsetNumber nPageData;		/* number of valid items in array */
	OffsetNumber curPageData;	/* next item to return */
} GISTScanOpaqueData;

typedef GISTScanOpaqueData *GISTScanOpaque;


/* XLog stuff */

#define XLOG_GIST_PAGE_UPDATE		0x00
 /* #define XLOG_GIST_NEW_ROOT			 0x20 */	/* not used anymore */
#define XLOG_GIST_PAGE_SPLIT		0x30
 /* #define XLOG_GIST_INSERT_COMPLETE	 0x40 */	/* not used anymore */
#define XLOG_GIST_CREATE_INDEX		0x50
 /* #define XLOG_GIST_PAGE_DELETE		 0x60 */	/* not used anymore */

typedef struct gistxlogPageUpdate
{
	RelFileNode node;
	BlockNumber blkno;

	/*
	 * If this operation completes a page split, by inserting a downlink for
	 * the split page, leftchild points to the left half of the split.
	 */
	BlockNumber leftchild;

	/* number of deleted offsets */
	uint16		ntodelete;

	/*
	 * follow: 1. todelete OffsetNumbers 2. tuples to insert
	 */
} gistxlogPageUpdate;

typedef struct gistxlogPageSplit
{
	RelFileNode node;
	BlockNumber origblkno;		/* splitted page */
	BlockNumber origrlink;		/* rightlink of the page before split */
	GistNSN		orignsn;		/* NSN of the page before split */
	bool		origleaf;		/* was splitted page a leaf page? */

	BlockNumber leftchild;		/* like in gistxlogPageUpdate */
	uint16		npage;			/* # of pages in the split */
	bool		markfollowright;	/* set F_FOLLOW_RIGHT flags */

	/*
	 * follow: 1. gistxlogPage and array of IndexTupleData per page
	 */
} gistxlogPageSplit;

typedef struct gistxlogPage
{
	BlockNumber blkno;
	int			num;			/* number of index tuples following */
} gistxlogPage;

/* SplitedPageLayout - gistSplit function result */
typedef struct SplitedPageLayout
{
	gistxlogPage block;
	IndexTupleData *list;
	int			lenlist;
	IndexTuple	itup;			/* union key for page */
	Page		page;			/* to operate */
	Buffer		buffer;			/* to write after all proceed */

	struct SplitedPageLayout *next;
} SplitedPageLayout;

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

	/* offset of the downlink in the parent page, that points to this page */
	OffsetNumber downlinkoffnum;

	/* pointer to parent */
	struct GISTInsertStack *parent;
} GISTInsertStack;

/* Working state and results for multi-column split logic in gistsplit.c */
typedef struct GistSplitVector
{
	GIST_SPLITVEC splitVector;	/* passed to/from user PickSplit method */

	Datum		spl_lattr[INDEX_MAX_KEYS];		/* Union of subkeys in
												 * splitVector.spl_left */
	bool		spl_lisnull[INDEX_MAX_KEYS];

	Datum		spl_rattr[INDEX_MAX_KEYS];		/* Union of subkeys in
												 * splitVector.spl_right */
	bool		spl_risnull[INDEX_MAX_KEYS];

	bool	   *spl_dontcare;	/* flags tuples which could go to either side
								 * of the split for zero penalty */
} GistSplitVector;

typedef struct
{
	Relation	r;
	Size		freespace;		/* free space to be left */

	GISTInsertStack *stack;
} GISTInsertState;

/* root page of a gist index */
#define GIST_ROOT_BLKNO				0

/*
 * Before PostgreSQL 9.1, we used rely on so-called "invalid tuples" on inner
 * pages to finish crash recovery of incomplete page splits. If a crash
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
	GISTNodeBufferPage *pageBuffer;		/* in-memory buffer page */

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
	int			loadedBuffersCount;		/* # of entries in loadedBuffers */
	int			loadedBuffersLen;		/* allocated size of loadedBuffers */

	/* Level of the current root node (= height of the index tree - 1) */
	int			rootlevel;
} GISTBuildBuffers;

/*
 * Storage type for GiST's reloptions
 */
typedef struct GiSTOptions
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int			fillfactor;		/* page fill factor in percent (0..100) */
	int			bufferingModeOffset;	/* use buffering build? */
} GiSTOptions;

/* gist.c */
extern Datum gistbuildempty(PG_FUNCTION_ARGS);
extern Datum gistinsert(PG_FUNCTION_ARGS);
extern MemoryContext createTempGistContext(void);
extern GISTSTATE *initGISTstate(Relation index);
extern void freeGISTstate(GISTSTATE *giststate);
extern void gistdoinsert(Relation r,
			 IndexTuple itup,
			 Size freespace,
			 GISTSTATE *GISTstate);

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
				bool markleftchild);

extern SplitedPageLayout *gistSplit(Relation r, Page page, IndexTuple *itup,
		  int len, GISTSTATE *giststate);

/* gistxlog.c */
extern void gist_redo(XLogRecPtr lsn, XLogRecord *record);
extern void gist_desc(StringInfo buf, uint8 xl_info, char *rec);
extern void gist_xlog_startup(void);
extern void gist_xlog_cleanup(void);

extern XLogRecPtr gistXLogUpdate(RelFileNode node, Buffer buffer,
			   OffsetNumber *todelete, int ntodelete,
			   IndexTuple *itup, int ntup,
			   Buffer leftchild);

extern XLogRecPtr gistXLogSplit(RelFileNode node,
			  BlockNumber blkno, bool page_is_leaf,
			  SplitedPageLayout *dist,
			  BlockNumber origrlink, GistNSN oldnsn,
			  Buffer leftchild, bool markfollowright);

/* gistget.c */
extern Datum gistgettuple(PG_FUNCTION_ARGS);
extern Datum gistgetbitmap(PG_FUNCTION_ARGS);

/* gistutil.c */

#define GiSTPageSize   \
	( BLCKSZ - SizeOfPageHeaderData - MAXALIGN(sizeof(GISTPageOpaqueData)) )

#define GIST_MIN_FILLFACTOR			10
#define GIST_DEFAULT_FILLFACTOR		90

extern Datum gistoptions(PG_FUNCTION_ARGS);
extern bool gistfitpage(IndexTuple *itvec, int len);
extern bool gistnospace(Page page, IndexTuple *itvec, int len, OffsetNumber todelete, Size freespace);
extern void gistcheckpage(Relation rel, Buffer buf);
extern Buffer gistNewBuffer(Relation r);
extern void gistfillbuffer(Page page, IndexTuple *itup, int len,
			   OffsetNumber off);
extern IndexTuple *gistextractpage(Page page, int *len /* out */ );
extern IndexTuple *gistjoinvector(
			   IndexTuple *itvec, int *len,
			   IndexTuple *additvec, int addlen);
extern IndexTupleData *gistfillitupvec(IndexTuple *vec, int veclen, int *memlen);

extern IndexTuple gistunion(Relation r, IndexTuple *itvec,
		  int len, GISTSTATE *giststate);
extern IndexTuple gistgetadjusted(Relation r,
				IndexTuple oldtup,
				IndexTuple addtup,
				GISTSTATE *giststate);
extern IndexTuple gistFormTuple(GISTSTATE *giststate,
			  Relation r, Datum *attdata, bool *isnull, bool newValues);

extern OffsetNumber gistchoose(Relation r, Page p,
		   IndexTuple it,
		   GISTSTATE *giststate);
extern void gistcentryinit(GISTSTATE *giststate, int nkey,
			   GISTENTRY *e, Datum k,
			   Relation r, Page pg,
			   OffsetNumber o, bool l, bool isNull);

extern void GISTInitBuffer(Buffer b, uint32 f);
extern void gistdentryinit(GISTSTATE *giststate, int nkey, GISTENTRY *e,
			   Datum k, Relation r, Page pg, OffsetNumber o,
			   bool l, bool isNull);

extern float gistpenalty(GISTSTATE *giststate, int attno,
			GISTENTRY *key1, bool isNull1,
			GISTENTRY *key2, bool isNull2);
extern void gistMakeUnionItVec(GISTSTATE *giststate, IndexTuple *itvec, int len,
				   Datum *attr, bool *isnull);
extern bool gistKeyIsEQ(GISTSTATE *giststate, int attno, Datum a, Datum b);
extern void gistDeCompressAtt(GISTSTATE *giststate, Relation r, IndexTuple tuple, Page p,
				  OffsetNumber o, GISTENTRY *attdata, bool *isnull);

extern void gistMakeUnionKey(GISTSTATE *giststate, int attno,
				 GISTENTRY *entry1, bool isnull1,
				 GISTENTRY *entry2, bool isnull2,
				 Datum *dst, bool *dstisnull);

extern XLogRecPtr gistGetFakeLSN(Relation rel);

/* gistvacuum.c */
extern Datum gistbulkdelete(PG_FUNCTION_ARGS);
extern Datum gistvacuumcleanup(PG_FUNCTION_ARGS);

/* gistsplit.c */
extern void gistSplitByKey(Relation r, Page page, IndexTuple *itup,
			   int len, GISTSTATE *giststate,
			   GistSplitVector *v,
			   int attno);

/* gistbuild.c */
extern Datum gistbuild(PG_FUNCTION_ARGS);
extern void gistValidateBufferingOption(char *value);

/* gistbuildbuffers.c */
extern GISTBuildBuffers *gistInitBuildBuffers(int pagesPerBuffer, int levelStep,
					 int maxLevel);
extern GISTNodeBuffer *gistGetNodeBuffer(GISTBuildBuffers *gfbb,
				  GISTSTATE *giststate,
				  BlockNumber blkno, int level);
extern void gistPushItupToNodeBuffer(GISTBuildBuffers *gfbb,
						 GISTNodeBuffer *nodeBuffer, IndexTuple item);
extern bool gistPopItupFromNodeBuffer(GISTBuildBuffers *gfbb,
						  GISTNodeBuffer *nodeBuffer, IndexTuple *item);
extern void gistFreeBuildBuffers(GISTBuildBuffers *gfbb);
extern void gistRelocateBuildBuffersOnSplit(GISTBuildBuffers *gfbb,
								GISTSTATE *giststate, Relation r,
								int level, Buffer buffer,
								List *splitinfo);
extern void gistUnloadNodeBuffers(GISTBuildBuffers *gfbb);

#endif   /* GIST_PRIVATE_H */
