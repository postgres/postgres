/*-------------------------------------------------------------------------
 *
 * gist_private.h
 *	  private declarations for GiST -- declarations related to the
 *	  internal implementation of GiST, not the public API
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
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
#include "storage/bufmgr.h"
#include "utils/rbtree.h"

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

/*
 * GISTSTATE: information needed for any GiST index operation
 *
 * This struct retains call info for the index's opclass-specific support
 * functions (per index column), plus the index's tuple descriptor.
 */
typedef struct GISTSTATE
{
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

	TupleDesc	tupdesc;
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
	MemoryContext tempCxt;		/* workspace context for calling functions */
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
#define XLOG_GIST_PAGE_DELETE		0x60

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

	/*
	 * follow: 1. gistxlogPage and array of IndexTupleData per page
	 */
} gistxlogPageSplit;

typedef struct gistxlogPage
{
	BlockNumber blkno;
	int			num;			/* number of index tuples following */
} gistxlogPage;

typedef struct gistxlogPageDelete
{
	RelFileNode node;
	BlockNumber blkno;
} gistxlogPageDelete;

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

	/* child's offset */
	OffsetNumber childoffnum;

	/* pointer to parent */
	struct GISTInsertStack *parent;

	/* for gistFindPath */
	struct GISTInsertStack *next;
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

/* gist.c */
extern Datum gistbuild(PG_FUNCTION_ARGS);
extern Datum gistbuildempty(PG_FUNCTION_ARGS);
extern Datum gistinsert(PG_FUNCTION_ARGS);
extern MemoryContext createTempGistContext(void);
extern void initGISTstate(GISTSTATE *giststate, Relation index);
extern void freeGISTstate(GISTSTATE *giststate);

extern SplitedPageLayout *gistSplit(Relation r, Page page, IndexTuple *itup,
		  int len, GISTSTATE *giststate);

extern GISTInsertStack *gistFindPath(Relation r, BlockNumber child);

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
			  Buffer leftchild);

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

extern XLogRecPtr GetXLogRecPtrForTemp(void);

/* gistvacuum.c */
extern Datum gistbulkdelete(PG_FUNCTION_ARGS);
extern Datum gistvacuumcleanup(PG_FUNCTION_ARGS);

/* gistsplit.c */
extern void gistSplitByKey(Relation r, Page page, IndexTuple *itup,
			   int len, GISTSTATE *giststate,
			   GistSplitVector *v,
			   int attno);

#endif   /* GIST_PRIVATE_H */
