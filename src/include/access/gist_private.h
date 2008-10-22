/*-------------------------------------------------------------------------
 *
 * gist_private.h
 *	  private declarations for GiST -- declarations related to the
 *	  internal implementation of GiST, not the public API
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/gist_private.h,v 1.24.2.2 2008/10/22 12:55:59 teodor Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef GIST_PRIVATE_H
#define GIST_PRIVATE_H

#include "access/gist.h"
#include "access/itup.h"
#include "access/xlog.h"
#include "access/xlogdefs.h"
#include "fmgr.h"

#define GIST_UNLOCK BUFFER_LOCK_UNLOCK
#define GIST_SHARE	BUFFER_LOCK_SHARE
#define GIST_EXCLUSIVE	BUFFER_LOCK_EXCLUSIVE


/*
 * XXX old comment!!!
 * When we descend a tree, we keep a stack of parent pointers. This
 * allows us to follow a chain of internal node points until we reach
 * a leaf node, and then back up the stack to re-examine the internal
 * nodes.
 *
 * 'parent' is the previous stack entry -- i.e. the node we arrived
 * from. 'block' is the node's block number. 'offset' is the offset in
 * the node's page that we stopped at (i.e. we followed the child
 * pointer located at the specified offset).
 */
typedef struct GISTSearchStack
{
	struct GISTSearchStack *next;
	BlockNumber block;
	/* to identify page changed */
	GistNSN		lsn;
	/* to recognize split occured */
	GistNSN		parentlsn;
} GISTSearchStack;

typedef struct GISTSTATE
{
	FmgrInfo	consistentFn[INDEX_MAX_KEYS];
	FmgrInfo	unionFn[INDEX_MAX_KEYS];
	FmgrInfo	compressFn[INDEX_MAX_KEYS];
	FmgrInfo	decompressFn[INDEX_MAX_KEYS];
	FmgrInfo	penaltyFn[INDEX_MAX_KEYS];
	FmgrInfo	picksplitFn[INDEX_MAX_KEYS];
	FmgrInfo	equalFn[INDEX_MAX_KEYS];

	TupleDesc	tupdesc;
} GISTSTATE;

typedef struct MatchedItemPtr 
{
	ItemPointerData		heapPtr;
	OffsetNumber		pageOffset; /* offset in index page */
} MatchedItemPtr;

/*
 *	When we're doing a scan, we need to keep track of the parent stack
 *	for the marked and current items.
 */
typedef struct GISTScanOpaqueData
{
	GISTSearchStack *stack;
	GISTSearchStack *markstk;
	uint16		flags;
	GISTSTATE  *giststate;
	MemoryContext tempCxt;
	Buffer		curbuf;
	Buffer		markbuf;

	MatchedItemPtr 	pageData[BLCKSZ/sizeof(IndexTupleData)];
	OffsetNumber    nPageData;
	OffsetNumber    curPageData;
	MatchedItemPtr 	markPageData[BLCKSZ/sizeof(IndexTupleData)];
	OffsetNumber    markNPageData;
	OffsetNumber    markCurPageData;
} GISTScanOpaqueData;

typedef GISTScanOpaqueData *GISTScanOpaque;

/* XLog stuff */
extern const XLogRecPtr XLogRecPtrForTemp;

#define XLOG_GIST_PAGE_UPDATE		0x00
#define XLOG_GIST_NEW_ROOT			0x20
#define XLOG_GIST_PAGE_SPLIT		0x30
#define XLOG_GIST_INSERT_COMPLETE	0x40
#define XLOG_GIST_CREATE_INDEX		0x50
#define XLOG_GIST_PAGE_DELETE		0x60

typedef struct gistxlogPageUpdate
{
	RelFileNode node;
	BlockNumber blkno;

	/*
	 * It used to identify completeness of insert. Sets to leaf itup
	 */
	ItemPointerData key;

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
	bool		origleaf;		/* was splitted page a leaf page? */
	uint16		npage;

	/* see comments on gistxlogPageUpdate */
	ItemPointerData key;

	/*
	 * follow: 1. gistxlogPage and array of IndexTupleData per page
	 */
} gistxlogPageSplit;

typedef struct gistxlogPage
{
	BlockNumber blkno;
	int			num;			/* number of index tuples following */
} gistxlogPage;

typedef struct gistxlogInsertComplete
{
	RelFileNode node;
	/* follows ItemPointerData key to clean */
} gistxlogInsertComplete;

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
	 * log sequence number from page->lsn to recognize page update	and
	 * compare it with page's nsn to recognize page split
	 */
	GistNSN		lsn;

	/* child's offset */
	OffsetNumber childoffnum;

	/* pointer to parent and child */
	struct GISTInsertStack *parent;
	struct GISTInsertStack *child;

	/* for gistFindPath */
	struct GISTInsertStack *next;
} GISTInsertStack;

typedef struct GistSplitVector
{
	GIST_SPLITVEC splitVector;	/* to/from PickSplit method */

	Datum		spl_lattr[INDEX_MAX_KEYS];		/* Union of subkeys in
												 * spl_left */
	bool		spl_lisnull[INDEX_MAX_KEYS];
	bool		spl_leftvalid;

	Datum		spl_rattr[INDEX_MAX_KEYS];		/* Union of subkeys in
												 * spl_right */
	bool		spl_risnull[INDEX_MAX_KEYS];
	bool		spl_rightvalid;

	bool	   *spl_equiv;		/* equivalent tuples which can be freely
								 * distributed between left and right pages */
} GistSplitVector;

#define XLogRecPtrIsInvalid( r )	( (r).xlogid == 0 && (r).xrecoff == 0 )

typedef struct
{
	Relation	r;
	IndexTuple *itup;			/* in/out, points to compressed entry */
	int			ituplen;		/* length of itup */
	Size		freespace;		/* free space to be left */
	GISTInsertStack *stack;
	bool		needInsertComplete;

	/* pointer to heap tuple */
	ItemPointerData key;
} GISTInsertState;

/*
 * When we're doing a scan and updating a tree at the same time, the
 * updates may affect the scan.  We use the flags entry of the scan's
 * opaque space to record our actual position in response to updates
 * that we can't handle simply by adjusting pointers.
 */
#define GS_CURBEFORE	((uint16) (1 << 0))
#define GS_MRKBEFORE	((uint16) (1 << 1))

/* root page of a gist index */
#define GIST_ROOT_BLKNO				0

/*
 * mark tuples on inner pages during recovery
 */
#define TUPLE_IS_VALID		0xffff
#define TUPLE_IS_INVALID	0xfffe

#define  GistTupleIsInvalid(itup)	( ItemPointerGetOffsetNumber( &((itup)->t_tid) ) == TUPLE_IS_INVALID )
#define  GistTupleSetValid(itup)	ItemPointerSetOffsetNumber( &((itup)->t_tid), TUPLE_IS_VALID )
#define  GistTupleSetInvalid(itup)	ItemPointerSetOffsetNumber( &((itup)->t_tid), TUPLE_IS_INVALID )

/* gist.c */
extern Datum gistbuild(PG_FUNCTION_ARGS);
extern Datum gistinsert(PG_FUNCTION_ARGS);
extern MemoryContext createTempGistContext(void);
extern void initGISTstate(GISTSTATE *giststate, Relation index);
extern void freeGISTstate(GISTSTATE *giststate);
extern void gistmakedeal(GISTInsertState *state, GISTSTATE *giststate);
extern void gistnewroot(Relation r, Buffer buffer, IndexTuple *itup, int len, ItemPointer key);

extern SplitedPageLayout *gistSplit(Relation r, Page page, IndexTuple *itup,
		  int len, GISTSTATE *giststate);

extern GISTInsertStack *gistFindPath(Relation r, BlockNumber child);

/* gistxlog.c */
extern void gist_redo(XLogRecPtr lsn, XLogRecord *record);
extern void gist_desc(StringInfo buf, uint8 xl_info, char *rec);
extern void gist_xlog_startup(void);
extern void gist_xlog_cleanup(void);
extern bool gist_safe_restartpoint(void);
extern IndexTuple gist_form_invalid_tuple(BlockNumber blkno);

extern XLogRecData *formUpdateRdata(RelFileNode node, Buffer buffer,
				OffsetNumber *todelete, int ntodelete,
				IndexTuple *itup, int ituplen, ItemPointer key);

extern XLogRecData *formSplitRdata(RelFileNode node,
			   BlockNumber blkno, bool page_is_leaf,
			   ItemPointer key, SplitedPageLayout *dist);

extern XLogRecPtr gistxlogInsertCompletion(RelFileNode node, ItemPointerData *keys, int len);

/* gistget.c */
extern Datum gistgettuple(PG_FUNCTION_ARGS);
extern Datum gistgetmulti(PG_FUNCTION_ARGS);

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
extern OffsetNumber gistfillbuffer(Relation r, Page page, IndexTuple *itup,
			   int len, OffsetNumber off);
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
extern bool gistMakeUnionItVec(GISTSTATE *giststate, IndexTuple *itvec, int len, int startkey,
				   Datum *attr, bool *isnull);
extern bool gistKeyIsEQ(GISTSTATE *giststate, int attno, Datum a, Datum b);
extern void gistDeCompressAtt(GISTSTATE *giststate, Relation r, IndexTuple tuple, Page p,
				  OffsetNumber o, GISTENTRY *attdata, bool *isnull);

extern void gistMakeUnionKey(GISTSTATE *giststate, int attno,
				 GISTENTRY *entry1, bool isnull1,
				 GISTENTRY *entry2, bool isnull2,
				 Datum *dst, bool *dstisnull);

/* gistvacuum.c */
extern Datum gistbulkdelete(PG_FUNCTION_ARGS);
extern Datum gistvacuumcleanup(PG_FUNCTION_ARGS);

/* gistsplit.c */
extern void gistSplitByKey(Relation r, Page page, IndexTuple *itup,
			   int len, GISTSTATE *giststate,
			   GistSplitVector *v, GistEntryVector *entryvec,
			   int attno);

#endif   /* GIST_PRIVATE_H */
